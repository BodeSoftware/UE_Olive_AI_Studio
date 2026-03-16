# Non-Interactive Pins Fix Design

## Problem

Blueprint nodes created by the Olive AI plugin have exec pins and data pins that cannot be dragged or connected by the user in the Blueprint editor. The nodes render correctly, wiring by tools works, but manual interaction is broken.

## Root Cause (Confirmed from UE5.5 Engine Source)

**The type-specific node creators in `OliveNodeFactory.cpp` skip three critical finalization steps that the engine always performs.**

### What the Engine Does (Every Node Creation Path)

Every engine node creation path (`EdGraphSchema_K2.cpp:6118-6124`, `BlueprintNodeSpawner.cpp:348-353`, `EdGraphSchema_K2_Actions.cpp:264-270`) follows this sequence:

```cpp
NewNode->SetFlags(RF_Transactional);     // (1) Enable undo/redo snapshots
Graph->AddNode(NewNode, false, false);
NewNode->CreateNewGuid();                // (2) Unique GUID for diffing/widget lookup
NewNode->PostPlacedNewNode();            // (3) One-time init hook for subclasses
NewNode->AllocateDefaultPins();
```

### What Our Code Does (Type-Specific Creators)

`CreateCallFunctionNode`, `CreateVariableGetNode`, `CreateVariableSetNode`, `CreateEventNode`, `CreateCustomEventNode`, `CreateBranchNode`, `CreateSequenceNode`, `CreateCastNode`, `CreateSpawnActorNode`, `CreateMakeStructNode`, `CreateBreakStructNode`, `CreateMacroInstanceNode`, `CreateCallDelegateNode`, `CreateAddDelegateNode`, `CreateComponentBoundEventNode`, and ~15 other type-specific creators all follow this pattern:

```cpp
UK2Node_Foo* Node = NewObject<UK2Node_Foo>(Graph);
Node->ConfigureSpecifics(...);
Node->AllocateDefaultPins();
Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
return Node;
```

Missing: `SetFlags(RF_Transactional)`, `CreateNewGuid()`, `PostPlacedNewNode()`.

Only `CreateNodeByClass()` (the universal fallback, line 1893) does all three plus `ReconstructNode()` and `NotifyGraphChanged()`. This creator works because it was specifically patched (it has comments about "defense-in-depth" and "Slate pin widget cache").

### Why This Causes Broken Pins

**The primary cause is missing `RF_Transactional`.**

Without `RF_Transactional`, the Unreal transaction system (`FScopedTransaction`) does not properly snapshot the node's object state before modifications. When the user performs undo (Ctrl+Z) or the write pipeline's `FScopedTransaction` rolls back on failure:

1. The transaction system cannot restore the node to its pre-modification state
2. Pin objects (`UEdGraphPin`) that were modified, wired, or replaced during `ReconstructNode` (which happens during compile) are left in an inconsistent state
3. The Slate `SGraphPin` widgets hold raw pointers to pin objects. After a compile cycle calls `ReconstructNode` on every node (Stage IX of `FBlueprintCompilationManager`), old pin objects are trashed and new ones created. Without `RF_Transactional`, the transaction system cannot properly track these pin replacements.
4. The `SGraphPanel` defers widget rebuild via `RegisterActiveTimer(0.f, ...)`. If a widget rebuild races with pin replacement from `ReconstructNode`, the widget can end up pointing at a trashed pin object. The `bGraphDataInvalid` flag is only set by `SGraphNode::InvalidateGraphData()`, which is only called by `SGraphPanel::RemoveNode()` -- NOT by `ReconstructNode`.

**The secondary cause is missing `CreateNewGuid()`.**

Without `CreateNewGuid()`, all nodes have a zero `FGuid`. The `SGraphPanel::NodeGuidMap` (line 1669) maps GUID to `SGraphNode` widget. With all GUIDs zero, only the last-added node's widget is in the map. While `NodeGuidMap` is not used for pin interaction directly, it is used by:
- `GetNodeWidgetFromGuid()` for graph diffing
- The undo system when restoring graph state (serialization includes `NodeGuid`)
- Blueprint compiler's node matching during `ReconstructNode` pin remapping

When multiple nodes share the zero GUID and the compiler reconstructs nodes, the pin-remapping logic in `ReconstructNode` (K2Node.cpp line 746-752: `ReallocatePinsDuringReconstruction` + `RewireOldPinsToNewPins`) may fail to correctly match old pins to new pins, leaving orphaned pin references.

### Why Previous Fix Attempts May Have Failed

1. **Adding `ReconstructNode()` + `NotifyGraphChanged()` only to `CreateNodeByClass()`**: This fixed the universal fallback path but did not fix the type-specific creators, which handle the majority of plan_json node creation.

2. **Adding `NotifyGraphChanged()` after operations**: `NotifyGraphChanged` with `GRAPHACTION_Default` does purge and rebuild widgets. But if the node's GUID is zero and `RF_Transactional` is missing, the rebuilt widgets still reference pin objects that may become stale after the next compile cycle.

3. **Calling `RefreshBlueprintEditorState()` (our custom helper)**: This calls `Graph->NotifyGraphChanged()` + `Blueprint->BroadcastChanged()`. `BroadcastChanged` triggers `FBlueprintEditor::OnBlueprintChanged` which calls `RefreshEditors(UnknownReason)` which calls `DocumentManager->RefreshAllTabs()` which calls `GraphEditor->NotifyGraphChanged()` which fires `GRAPHACTION_Default` which does purge + rebuild. This IS the correct refresh path. But without `RF_Transactional` on nodes, the transaction/undo system corrupts pin state, and the rebuilt widgets pick up corrupted pins.

## Eliminated Hypotheses

Based on engine source reading, these are NOT the cause:

- **`bFromUI=false`**: `UEdGraph::AddNode` uses `bFromUI` only to set `bUserInvoked` on the `FEdGraphEditAction` struct. It does NOT control `PostPlacedNewNode()` calls, widget creation, or pin connectivity. The only effect is cosmetic: `bUserInvoked=true` causes `SGraphPanel` to play a spawn animation and request rename-on-spawn.

- **`bNotConnectable` pin flag**: Default-initialized to `false` in `UEdGraphPin` constructor. `AllocateDefaultPins()` creates pins with `bNotConnectable=false`. No code path in our plugin sets it to `true`.

- **`bOrphanedPin` flag**: Only set by `ReconstructNode` during pin remapping and by serialization. Would prevent connection but not drag interaction. Also default `false`.

- **Missing `PostPlacedNewNode()`**: Virtual no-op on `UEdGraphNode` base class. Only relevant for specific subclasses like `UK2Node_AssignDelegate` (spawns an attached custom event). Most K2Node subclasses do not override it. Not the primary cause, but should be called for correctness.

- **`AllocateDefaultPins()` order**: Our code calls it BEFORE `AddNode()`. The engine calls it AFTER. But `SGraphPanel::OnGraphChanged` defers widget creation via `RegisterActiveTimer(0.f, ...)` specifically to handle the engine's ordering. Either order works because the widget is created on the next frame tick regardless.

## Fix

### Location: `FOliveNodeFactory::CreateNode()` in `OliveNodeFactory.cpp`

The fix is a 4-line post-creation block in the central `CreateNode()` method, after the type-specific creator returns and before position is set. This is a single call site that all creators pass through.

```cpp
UEdGraphNode* FOliveNodeFactory::CreateNode(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& NodeType,
    const TMap<FString, FString>& Properties,
    int32 PosX,
    int32 PosY)
{
    // ... validation, creator dispatch ...

    UEdGraphNode* NewNode = nullptr;
    if (const FNodeCreator* Creator = NodeCreators.Find(NodeType))
    {
        NewNode = (*Creator)(Blueprint, Graph, Properties);
    }
    else
    {
        NewNode = CreateNodeByClass(Blueprint, Graph, NodeType, Properties);
    }

    if (!NewNode)
    {
        // ... error handling ...
        return nullptr;
    }

    // --- BEGIN FIX: Finalize node to match engine expectations ---
    // The engine's node creation paths (BlueprintNodeSpawner, EdGraphSchema_K2)
    // always call these three steps. Without them:
    // - RF_Transactional: undo/redo corrupts pin state after ReconstructNode
    // - CreateNewGuid: compiler's pin remapping during ReconstructNode may
    //   fail to match old pins to new pins, leaving orphaned references
    // - PostPlacedNewNode: subclass-specific one-time init (e.g., delegate
    //   nodes that spawn paired event nodes)
    //
    // CreateNodeByClass already does these, so guard against double-call.
    if (!NewNode->HasAnyFlags(RF_Transactional))
    {
        NewNode->SetFlags(RF_Transactional);
    }
    if (!NewNode->NodeGuid.IsValid())
    {
        NewNode->CreateNewGuid();
    }
    // PostPlacedNewNode is idempotent for all K2Node subclasses in UE 5.5.
    // It is a no-op on base UEdGraphNode.
    // NOTE: Do NOT call it if CreateNodeByClass was used (it already ran
    // ReconstructNode which subsumes PostPlacedNewNode's purpose).
    // --- END FIX ---

    SetNodePosition(NewNode, PosX, PosY);
    // ...
}
```

### Why This Works

1. **Single call site**: All 30+ type-specific creators return through `CreateNode()`. No need to patch each one.

2. **Safe for `CreateNodeByClass`**: The guard `!HasAnyFlags(RF_Transactional)` and `!NodeGuid.IsValid()` prevent double-application since `CreateNodeByClass` already does these steps.

3. **Minimal risk**: `SetFlags(RF_Transactional)` is purely additive -- it enables undo tracking that was previously missing. `CreateNewGuid()` assigns a unique ID that was previously zero. Neither operation can break existing functionality.

4. **No compile-time cost**: These are O(1) operations (flag set, GUID generation).

### What NOT To Do

- Do NOT add `ReconstructNode()` to `CreateNode()`. For type-specific creators, `AllocateDefaultPins()` already produces correct pins. `ReconstructNode()` destroys and recreates pins, which would invalidate any wiring done between node creation and the subsequent exec/data wiring phases (Phase 2-4 in the plan executor). `CreateNodeByClass` needs `ReconstructNode` because it handles arbitrary K2Node classes where `AllocateDefaultPins` alone may not produce correct pins.

- Do NOT add `PostPlacedNewNode()` to the generic path without auditing all K2Node subclasses used by type-specific creators. For example, `UK2Node_CallDelegate`'s `PostPlacedNewNode` might create companion nodes that conflict with our plan executor's explicit node creation.

## Files Modified

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | Add 4-line post-creation block in `CreateNode()` after line ~148 |

## Verification

After applying the fix:
1. Create a Blueprint via the AI (plan_json or add_node)
2. Open it in the Blueprint editor
3. Attempt to drag an exec pin from one node -- it should start a wire drag
4. Attempt to connect two data pins by dragging -- it should show the connection preview and allow the connection
5. Undo (Ctrl+Z) the AI's changes, then Redo (Ctrl+Y) -- nodes and connections should be restored correctly
6. If the Blueprint was already open during AI modification, verify pins are interactive without needing to close and reopen

## Implementation Order

1. Add the `RF_Transactional` + `CreateNewGuid()` block to `CreateNode()` (the fix itself)
2. Build and verify compilation
3. Manual test: have the AI create a simple Blueprint (event + function call + variable set), then interact with pins in the editor
4. Regression test: run an existing plan_json test to verify no node creation regressions
