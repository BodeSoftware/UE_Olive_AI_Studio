# Research: Non-Interactive Pins on Programmatically Created Blueprint Nodes (UE 5.5)

## Question
Why do pins on nodes created via `NewObject + AllocateDefaultPins + Graph->AddNode` appear
visually but cannot be dragged to form connections until the editor is restarted?

## Findings

### 1. What `PostPlacedNewNode` Does and Who Calls It

`UEdGraphNode::PostPlacedNewNode()` is declared at
`Engine/Source/Runtime/Engine/Classes/EdGraph/EdGraphNode.h:809` as a virtual no-op.

**Subclasses that override it with meaningful logic (in BlueprintGraph):**

| Node class | What PostPlacedNewNode does |
|---|---|
| `UK2Node_CallFunction` | `FunctionReference.RefreshGivenNewSelfScope()` — refreshes the function scope after the node is placed in a live Blueprint context. Also sets `ENodeEnabledState::DevelopmentOnly` for dev-only functions. Source: `K2Node_CallFunction.cpp:2531` |
| `UK2Node_FunctionResult` | `SyncWithEntryNode()` + `SyncWithPrimaryResultNode()` — syncs the result node's pins with the function's entry signature and any pre-existing result nodes. **Without this, the return node has wrong/missing output pins.** Source: `K2Node_FunctionResult.cpp:257` |
| `UK2Node_BreakStruct` | Sets `bMadeAfterOverridePinRemoval = true` — a compat flag. Non-critical for pin dragging. Source: `K2Node_BreakStruct.cpp:381` |
| `UK2Node_AssignDelegate` | Spawns companion event node. Source: `K2Node_AssignDelegate.cpp:88` |
| `UK2Node_Composite` | Opens collapsed graph. Source: `K2Node_Composite.cpp:308` |
| `UK2Node_SpawnActorFromClass` | Refreshes class pin. |
| `UK2Node_ConstructObjectFromClass` | Refreshes class pin. |

**Where Epic calls it in normal node placement:**

Path 1 — `FEdGraphSchemaAction_K2NewNode::CreateNode()` in `EdGraphSchema_K2_Actions.cpp:246–326`:
```
ParentGraph->Modify()
ParentGraph->AddNode(ResultNode, true, bSelectNewNode)   // line 267
ResultNode->CreateNewGuid()                               // line 269
ResultNode->PostPlacedNewNode()                           // line 270  ← CALLED
ResultNode->AllocateDefaultPins()                         // line 271
AutowireNewNode(FromPin)
MarkBlueprintAsModified / MarkBlueprintAsStructurallyModified
```

Path 2 — `UBlueprintNodeSpawner::SpawnNode()` in `BlueprintNodeSpawner.cpp:330–354`:
```
NewObject<UEdGraphNode>(ParentGraph, InNodeClass)
CreateNewGuid()
PostSpawnDelegate (sets properties, e.g. FunctionReference)
AllocateDefaultPins()                                     // line 345
PostPlacedNewNode()                                       // line 346  ← CALLED
ParentGraph->Modify()
ParentGraph->AddNode(bFromUI=true, bSelectNewNode=false)
```

Path 3 — `FGraphNodeCreator<T>::Finalize()` in `EdGraph.h:297–307`:
```
Node->CreateNewGuid()
Node->PostPlacedNewNode()    ← CALLED
if (Node->Pins.Num() == 0) Node->AllocateDefaultPins()
```

**Our code does NOT call `PostPlacedNewNode()`.** This is a divergence from all three Epic paths.

For most node types (`UK2Node_Event`, `UK2Node_CustomEvent`, `UK2Node_IfThenElse`, etc.) that
do not override `PostPlacedNewNode`, this is harmless — the base no-op fires. For
`UK2Node_CallFunction` and `UK2Node_FunctionResult` the missing call has specific consequences.

### 2. The Actual Pin-Drag Mechanism and What Breaks It

Pin dragging in `SGraphPin::OnMouseButtonDown` (`SGraphPin.cpp:406–542`) requires three
conditions all true simultaneously:

```cpp
if (IsEditingEnabled())                          // line 408
if (!GraphPinObj->bNotConnectable && bDragAndDropEnabled)   // line 520
    if (TSharedPtr<SGraphPanel> OwnerGraphPanel = OwnerNodePinned->GetOwnerPanel())  // line 528
        return FReply::Handled().BeginDragDrop(SpawnPinDragEvent(...))
```

- `IsEditingEnabled()` → `OwnerNodePtr.Pin()->IsNodeEditable() && IsEditable.Get()`
  (`SGraphPin.cpp:1442–1448`)
- `IsNodeEditable()` → `OwnerGraphPanelPtr.IsValid() ? OwnerGraphPanelPtr.Pin()->IsGraphEditable() : true`
  (`SGraphNode.cpp:218–222`)
- `bDragAndDropEnabled` defaults to `true` (`SGraphPin.cpp:115`)

**The critical path is `OwnerNodePtr` and `OwnerGraphPanelPtr`.**

`SGraphPin::OwnerNodePtr` is set only in `SGraphPin::SetOwner()` (`SGraphPin.cpp:935–938`), which
is called by `SGraphNode::AddPin()` (`SGraphNode.cpp:1269`), which is called by
`SGraphNode::CreatePinWidgets()` (`SGraphNode.cpp:1239`).

`SGraphNode::OwnerGraphPanelPtr` is set only in `SGraphNode::SetOwner()` (`SGraphNode.cpp:541–570`),
which is called by `SGraphPanel::AddGraphNode()` (`SGraphPanel.cpp:1661–1673`).

**Pin widgets that exist without a valid `OwnerGraphPanelPtr` on their owning `SGraphNode` will
fail the `GetOwnerPanel()` check silently — `FReply::Unhandled()` is returned but the pin
appears visually normal.** This is the exact symptom described: pins visible but not draggable.

### 3. `SGraphPanel::OnGraphChanged` — The Deferred Widget Creation Path

`SGraphPanel` listens to `UEdGraph::OnGraphChanged` delegate (registered at construction,
`SGraphPanel.cpp:129–130`). Its handler `OnGraphChanged` (`SGraphPanel.cpp:2414`) has two paths:

**Path A: `GRAPHACTION_Default` (our `Graph->NotifyGraphChanged()` with no args)**
`EdGraph.cpp:281–285` shows the no-arg version sends `GRAPHACTION_Default`.
`SGraphPanel::OnGraphChanged:2416–2434`: When `bShouldPurge || Action == GRAPHACTION_Default`,
calls `PurgeVisualRepresentation()` (clears ALL node widgets, sets `bVisualUpdatePending=true`),
then registers a Slate timer at 0.f delay to call `Update()`.

`Update()` (`SGraphPanel.cpp:2233`) scans `GraphObj->Nodes` and calls `AddNode(Node, CheckUserAddedNodesList)`
for every node that doesn't have a widget yet. This correctly creates all widgets.

**Path B: `GRAPHACTION_AddNode` (from `Graph->AddNode()`)**
`SGraphPanel::OnGraphChanged:2505–2545`: For K2 graphs, `ShouldAlwaysPurgeOnModification()`
returns `false` (`EdGraphSchema_K2.h:573`), so the incremental path runs. A Slate timer
at 0.f delay calls `SGraphPanel::AddNode(Node, bForceUserAdded ? WasUserAdded : NotUserAdded)`.
`bForceUserAdded` = `EditAction.bUserInvoked`, which is `false` in our case (we pass `bFromUI=false`).

**Critical race condition:** If `GRAPHACTION_Default` fires AND sets `bVisualUpdatePending=true`
before the `GRAPHACTION_AddNode` timer executes, the `AddNode` timer body (`SGraphPanel.cpp:2519`)
sees `bVisualUpdatePending=true` and skips creating the widget:
```cpp
if (Parent->bVisualUpdatePending) {
    if (bForceUserAdded) { Parent->UserAddedNodes.Add(Node); }
    // else: do nothing — node widget won't be created here
}
```
The node widget creation is then fully deferred to the `Update()` call.

**This is the correct behavior and not a bug by itself.** The node widgets do get created via
`Update()`. The problem lies elsewhere.

### 4. The `ReconstructNode` + `NotifyGraphChanged` Ordering Issue

Our `CreateNodeByClass` sequence (`OliveNodeFactory.cpp:2192–2209`):
```
NewNode->CreateNewGuid()          // line 2193
NewNode->AllocateDefaultPins()    // line 2196
Graph->AddNode(false, false)      // line 2199 → registers GRAPHACTION_AddNode Slate timer
NewNode->ReconstructNode()        // line 2204 → calls Graph->NotifyNodeChanged(this)
Graph->NotifyGraphChanged()       // line 2209 → GRAPHACTION_Default → PurgeVisualRepresentation
```

`UK2Node::ReconstructNode()` (`K2Node.cpp:704`) ends by calling `GetGraph()->NotifyNodeChanged(this)`
at line 793. `NotifyNodeChanged` sends `GRAPHACTION_EditNode` (`EdGraph.cpp:287–293`).

The `GRAPHACTION_EditNode` handler in `SGraphPanel::OnGraphChanged:2587–2591` calls
`RefreshNode(Node)` immediately (not deferred). `RefreshNode` (`SGraphPanel.cpp:2099–2106`) calls
`GetNodeWidgetFromGuid(Node.NodeGuid)` which looks up `NodeGuidMap`. **If the `GRAPHACTION_AddNode`
Slate timer hasn't fired yet, `NodeGuidMap` does not contain this node's GUID** (it's only inserted
in `SGraphPanel::AddGraphNode` at line 1669), so `GetNodeWidgetFromGuid` returns null and
`UpdateGraphNode` is never called.

Then our explicit `Graph->NotifyGraphChanged()` (no-arg) at line 2209 fires `GRAPHACTION_Default`
which purges everything and schedules `Update()`. `Update()` finally calls `AddNode()` for all
nodes including the new one, which calls `SGraphPanel::AddNode()` → `FNodeFactory::CreateNodeWidget()`
→ `SGraphNode::UpdateGraphNode()` → `SGraphNode::CreatePinWidgets()`. At this point pins get
`OwnerNodePtr` set.

**So the widget creation chain works, but there are scenarios where it does not:**

### 5. The Real Root Cause: `bVisualUpdatePending` Gate + `BroadcastChanged` Double-Purge

When our code calls `Graph->NotifyGraphChanged()` followed by `Blueprint->BroadcastChanged()`:

1. `Graph->NotifyGraphChanged()` → `GRAPHACTION_Default` → `PurgeVisualRepresentation()` sets
   `bVisualUpdatePending=true`, registers Update timer.
2. `Blueprint->BroadcastChanged()` → `FBlueprintEditor::OnBlueprintChanged()` (`BlueprintEditor.cpp:4062`)
   → `RefreshEditors()` (`BlueprintEditor.cpp:989`) → `DocumentManager->RefreshAllTabs()` →
   `FGraphEditorSummoner::OnTabRefreshed()` (`BlueprintEditorTabFactories.cpp:67–71`) →
   `SGraphEditor::NotifyGraphChanged()` (`SGraphEditorImpl.cpp:179–182`) →
   `UEdGraph::NotifyGraphChanged()` (no-arg) → SECOND `PurgeVisualRepresentation()` + second
   Update timer.

This double-purge is harmless in isolation (second purge clears already-cleared widgets). BUT:

The `MarkBlueprintAsModified` call at `OliveGraphWriter.cpp:211` also triggers `OnBlueprintChanged`
in the Blueprint editor (via `PostEditChange` chain), which calls `RefreshEditors()` again.
This can fire before or after the Slate timers, depending on call-stack context.

**The most important scenario where pins end up non-interactive:**

When `Update()` runs and calls `AddNode(Node, CheckUserAddedNodesList)`, it calls
`SGraphPanel::AddNode()` which calls `FNodeFactory::CreateNodeWidget(Node)`. The widget factory
creates an `SGraphNode` subclass and calls `Construct()` → `UpdateGraphNode()` →
`CreatePinWidgets()`. At the point `CreatePinWidgets()` runs, it reads `GraphNode->Pins`. If
`ReconstructNode()` has run and the new `UEdGraphPin*` objects are valid, pins get created with
`SGraphPin::SetOwner(SharedThis(this))` — setting `OwnerNodePtr`.

Then `SGraphPanel::AddGraphNode()` calls `SGraphNode::SetOwner(SharedThis(this))` — setting
`OwnerGraphPanelPtr` on the SGraphNode.

**Pin drag requires both `OwnerNodePtr` (set during `CreatePinWidgets`) and `OwnerGraphPanelPtr`
(set during `AddGraphNode/SetOwner`). The code sets them in the right order (pins first, then
panel), so this should work.**

### 6. The True Root Cause: `UEdGraphPin` Outer Mismatch

`SGraphNode::CreatePinWidgets()` (`SGraphNode.cpp:1239–1260`) has this guard:

```cpp
if (!ensureMsgf(CurPin->GetOuter() == GraphNode, TEXT("...")))
{
    continue;  // SKIPS pin widget creation silently
}
```

`UEdGraphPin` objects are created as subojects of their owning node in `AllocateDefaultPins`.
**If `ReconstructNode()` is called after `AllocateDefaultPins`, it calls `ReallocatePinsDuringReconstruction`
which calls `Pins.Reset()` then `AllocateDefaultPins()` again** — creating new `UEdGraphPin*`
objects owned by the same node. These should pass the `GetOuter() == GraphNode` check.

However, `FGraphNodeCreator::Finalize()` (`EdGraph.h:303–306`) only calls `AllocateDefaultPins`
if `Node->Pins.Num() == 0`. Our code calls `AllocateDefaultPins()` explicitly then later
`ReconstructNode()` which calls it again. The resulting pins have `GraphNode` as their outer, so
the outer check passes.

### 7. The Actual Explanation for "Restart Fixes It"

"Restart fixes it" is the definitive clue. It means: on the next editor session, when the
Blueprint is loaded, `Update()` is called fresh and creates widgets for all nodes including
the newly created ones. All pins get `OwnerNodePtr` and `OwnerGraphPanelPtr` set correctly.

This pattern of "works after restart but not immediately" points to a **Slate widget lifecycle
state machine issue**, specifically one of:

**Hypothesis A: Stale widget from pre-`ReconstructNode` pin set**
Sequence of events if the `GRAPHACTION_AddNode` timer fires BEFORE `ReconstructNode()` runs
(which cannot happen in our current code since we call `ReconstructNode()` synchronously
before returning from `CreateNodeByClass`). This is ruled out.

**Hypothesis B: `bVisualUpdatePending` + timer ordering**
If `bVisualUpdatePending=true` is already set from a PREVIOUS operation's `NotifyGraphChanged`
when our new `GRAPHACTION_AddNode` fires, the add timer body skips creating the widget (sees
`bVisualUpdatePending=true`, `bForceUserAdded=false`, does nothing). The only path that creates
the widget is `Update()`. If `Update()` already ran (clearing `bVisualUpdatePending`) before the
`GRAPHACTION_AddNode` timer fires, the node has no widget at all — it's invisible, not just
non-interactive. But this would cause nodes to be invisible, not just non-interactive.

**Hypothesis C: Pin widget `OwnerNodePtr` set but `OwnerGraphPanelPtr` not yet set**
`SGraphNode::SetOwner()` (`SGraphNode.cpp:541–570`) calls `CreatePinWidgets()` AGAIN if
`GetPinVisibility() != Pin_Show` and the pin boxes are already populated. But normally
`CreatePinWidgets()` has already been called during `UpdateGraphNode()` inside `Construct()`.
The second call via `SetOwner()` only applies in pin-hiding mode — when the pin visibility
mode is not `Pin_Show`.

**If `SGraphPanel::IsGraphEditable()` returns false at the time `SetOwner()` is called**, then
`IsNodeEditable()` returns false, and `IsEditingEnabled()` on the pin returns false, blocking drag.
But this would affect ALL nodes, not just programmatically created ones.

**Hypothesis D (MOST LIKELY): Widget created before node is in `NodeGuidMap`**
`SGraphPanel::AddNode()` creates the widget and calls `AddGraphNode()` which inserts into
`NodeGuidMap` keyed by `Node->NodeGuid`. The `NodeGuid` is set by `CreateNewGuid()`. If
`CreateNewGuid()` is called BEFORE `AddNode()`, the GUID is valid. In our code: line 2193 sets
GUID, line 2199 calls `Graph->AddNode()`. So the GUID exists when the timer eventually runs.

**Hypothesis E (CONFIRMED LIKELY ROOT CAUSE): The `bNotConnectable` flag on orphaned pins**
`UK2Node::ReconstructNode()` (`K2Node.cpp:716–742`) iterates pins and checks for orphaned links.
`RewireOldPinsToNewPins()` (called at line 752) can mark pins as orphaned (`bOrphanedPin=true`).
Orphaned pins have `bNotConnectable=true` set in some codepaths. If any of the reconstructed
pins get their `bOrphanedPin` set, `GetIsConnectable()` returns `false` (SGraphPin.cpp:1500–1504:
`return GraphPin ? !GraphPin->bNotConnectable : false`), blocking drag.

To verify: check if the non-draggable pins have `bNotConnectable=true` or `bOrphanedPin=true`
after node creation.

### 8. Summary of Call Chain Differences (Ours vs Epic's)

| Step | Epic's `FEdGraphSchemaAction_K2NewNode` | Our `CreateNodeByClass` |
|---|---|---|
| 1 | `ParentGraph->Modify()` | `Blueprint->Modify()` (outer scope) |
| 2 | `ConstructionFn(ParentGraph)` (NewObject) | `NewObject<UEdGraphNode>(Graph, NodeClass)` |
| 3 | Set properties | Set properties via reflection |
| 4 | `Graph->AddNode(true, bSelectNewNode)` | `AllocateDefaultPins()` |
| 5 | `CreateNewGuid()` | `CreateNewGuid()` |
| 6 | **`PostPlacedNewNode()`** ← MISSING | `Graph->AddNode(false, false)` |
| 7 | `AllocateDefaultPins()` | `ReconstructNode()` |
| 8 | `AutowireNewNode()` | `Graph->NotifyGraphChanged()` (no-arg) |
| 9 | `MarkBlueprintAsModified` | `MarkBlueprintAsModified()` |
| 10 | — | `BroadcastChanged()` |

Key differences:
- **We call `AllocateDefaultPins()` before `AddNode()`; Epic calls it after.**
- **We call `ReconstructNode()` after `AddNode()`; Epic does NOT call `ReconstructNode()`.**
- **We never call `PostPlacedNewNode()`.**
- **We call the no-arg `NotifyGraphChanged()` explicitly; Epic does not.**

### 9. The `NodeToWidgetLookup` vs `NodeGuidMap` Split

`SNodePanel` uses `NodeToWidgetLookup` (keyed by `UObject*` pointer from `GetObjectBeingDisplayed()`).
`SGraphPanel` uses `NodeGuidMap` (keyed by `FGuid`).

Both are populated in `SNodePanel::AddGraphNode()` (`SNodePanel.cpp:1251–1254`) and
`SGraphPanel::AddGraphNode()` (`SGraphPanel.cpp:1661–1673`) respectively.

`RefreshNode()` uses `NodeGuidMap`: `GetNodeWidgetFromGuid(Node.NodeGuid)`.
Pin connection drawing uses `NodeToWidgetLookup`.

If a node widget exists in `NodeGuidMap` but not in `NodeToWidgetLookup` (or vice versa), some
operations silently fail. This can happen if `AddGraphNode` is called in only one panel class
but the subclass override is missing — but this is not the case for Blueprint graphs.

### 10. `NotifyGraphChanged(GRAPHACTION_Default)` is Destructive for In-Progress Batches

Our `OlivePlanExecutor` creates multiple nodes in sequence across 7 phases. During this batch,
each call to `CreateNodeByClass` fires `Graph->NotifyGraphChanged()` (via `ReconstructNode` →
`NotifyNodeChanged` AND our explicit call). Each `GRAPHACTION_Default` triggers a new
`PurgeVisualRepresentation()` + `Update()` cycle.

The `GRAPHACTION_EditNode` from `ReconstructNode` calls `RefreshNode()` → `GetNodeWidgetFromGuid()`
but the node may not be in `NodeGuidMap` yet (the `AddNode` timer hasn't fired). So `RefreshNode`
does nothing.

The purge+update cycle from `NotifyGraphChanged()` should eventually create all widgets correctly.
But with batch operations, there are many pending Slate timers. If any purge fires AFTER some
`AddNode` timers have already fired (adding widgets), those widgets get destroyed by the purge,
then recreated by `Update()`. During the window between destruction and recreation, any
in-progress drag operations would fail.

## Recommendations

1. **Call `PostPlacedNewNode()` in `CreateNodeByClass`** immediately after `AllocateDefaultPins()`,
   before `Graph->AddNode()`. This matches the `BlueprintNodeSpawner` path. For `UK2Node_FunctionResult`
   this is especially critical — without it, the result node's pins don't sync with the function
   signature. For `UK2Node_CallFunction` this refreshes the function scope.

   Correct order for `CreateNodeByClass`:
   ```
   NewObject
   SetProperties (reflection)
   SetFromFunction (for CallFunction)
   CreateNewGuid
   AllocateDefaultPins
   PostPlacedNewNode      ← add this
   Graph->AddNode(false, false)
   ReconstructNode        ← keep for defense-in-depth, but consider removing
   ```

2. **Remove the explicit `Graph->NotifyGraphChanged()` call from `CreateNodeByClass`**.
   `ReconstructNode()` already calls `GetGraph()->NotifyNodeChanged(this)` at its end
   (`K2Node.cpp:793`), which sends `GRAPHACTION_EditNode`. That's the right granular notification.
   The no-arg `NotifyGraphChanged()` triggers a full purge which is expensive and causes
   the re-creation race described above.

3. **`Graph->NotifyGraphChanged()` in `OliveGraphWriter::AddNode` (`RefreshBlueprintEditorState`)
   is already triggering the full purge+rebuild.** This single call at the end of the write
   operation is sufficient. The additional call inside `CreateNodeByClass` is redundant and
   harmful for batch operations.

4. **Investigate `bOrphanedPin` / `bNotConnectable` on created pins.** After calling
   `ReconstructNode()`, any pins that existed before reconstruction but no longer match the new
   pin layout get `bOrphanedPin=true`. If `AllocateDefaultPins()` creates pins, then
   `ReconstructNode()` → `ReallocatePinsDuringReconstruction()` re-creates them, old pin objects
   may be orphaned. The new pins should be clean, but verify that the `SGraphPin` widgets being
   used by Slate point to the NEW pin objects (not the old orphaned ones).
   This is the most likely direct cause of the non-interactive symptom: Slate is holding
   `SGraphPin` widgets that point to old `UEdGraphPin*` objects with `bOrphanedPin=true` or
   `bNotConnectable=true`.

5. **Consider removing `ReconstructNode()` from `CreateNodeByClass` entirely** for the base case.
   Epic's standard path (`FEdGraphSchemaAction_K2NewNode`) does NOT call `ReconstructNode()`.
   The `ReconstructNode` call was added for defense-in-depth, but it creates the orphaned-pin
   scenario by destroying the pins that `AllocateDefaultPins()` just created. Keep it only for
   `UK2Node_SpawnActorFromClass` (which needs it for `ExposeOnSpawn` vars — there it is already
   called separately and explicitly).

6. **The "restart fixes it" symptom** is fully explained by the Slate widget lifecycle: on
   restart, a fresh `Update()` call creates clean widgets with the correct pin objects. No stale
   `SGraphPin → UEdGraphPin` pointer exists. The fix is to ensure that after any in-session node
   creation, the `SGraphPanel::Update()` path creates fresh widgets from the current (post-
   `ReconstructNode`) pin state, not from a stale intermediate state.

7. **The `bFromUI=false` flag in `Graph->AddNode(false, false)` is correct** — it prevents the
   rename-on-spawn and play-spawn-effect animation. It does NOT affect pin connectability.
   The `bSelectNewNode=false` flag is also correct.

8. **`MarkBlueprintAsStructurallyModified` vs `MarkBlueprintAsModified`**: For new nodes,
   `MarkBlueprintAsStructurallyModified` is more correct (it rebuilds the skeleton and refreshes
   the Blueprint editor graph tabs). Epic uses the structural version when
   `ResultNode->NodeCausesStructuralBlueprintChange()` returns true. For `UK2Node_CallFunction`
   this is false; for event/variable nodes it may be true. Using the wrong one can leave the
   editor in a stale state. This is handled correctly in `OliveGraphWriter` already.

## Key Files

- `Engine/Source/Runtime/Engine/Classes/EdGraph/EdGraph.h:274–326` — `FGraphNodeCreator<T>` and `Finalize()`
- `Engine/Source/Runtime/Engine/Private/EdGraph/EdGraph.cpp:229–244` — `UEdGraph::AddNode` implementation
- `Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2_Actions.cpp:240–326` — K2 node creation (canonical path)
- `Engine/Source/Editor/BlueprintGraph/Private/BlueprintNodeSpawner.cpp:330–354` — Spawner path
- `Engine/Source/Editor/GraphEditor/Private/SGraphPanel.cpp:2113–2171` — `SGraphPanel::AddNode` (widget creation)
- `Engine/Source/Editor/GraphEditor/Private/SGraphPanel.cpp:2414–2591` — `SGraphPanel::OnGraphChanged`
- `Engine/Source/Editor/GraphEditor/Private/SGraphNode.cpp:541–571` — `SGraphNode::SetOwner` (sets `OwnerGraphPanelPtr`)
- `Engine/Source/Editor/GraphEditor/Private/SGraphNode.cpp:1239–1260` — `SGraphNode::CreatePinWidgets` (outer check)
- `Engine/Source/Editor/GraphEditor/Private/SGraphPin.cpp:406–542` — `SGraphPin::OnMouseButtonDown` (drag gating)
- `Engine/Source/Editor/GraphEditor/Private/SGraphPin.cpp:1442–1503` — `IsEditingEnabled` and `GetIsConnectable`
- `Engine/Source/Editor/BlueprintGraph/Private/K2Node.cpp:704–793` — `UK2Node::ReconstructNode`
- `Engine/Source/Editor/BlueprintGraph/Private/K2Node_FunctionResult.cpp:257–265` — `PostPlacedNewNode` syncing pins
- `Engine/Source/Editor/BlueprintGraph/Private/K2Node_CallFunction.cpp:2531–2547` — `PostPlacedNewNode` scope refresh
- `Engine/Source/Editor/Kismet/Private/BlueprintEditorTabFactories.cpp:67–71` — `OnTabRefreshed` → `NotifyGraphChanged`
- `Engine/Source/Editor/Kismet/Private/BlueprintEditor.cpp:4062–4081` — `OnBlueprintChangedImpl` → `RefreshEditors`
- `Plugins/.../Blueprint/Private/Writer/OliveNodeFactory.cpp:1938–2248` — our `CreateNodeByClass`
- `Plugins/.../Blueprint/Private/Writer/OliveGraphWriter.cpp:156–218` — our `AddNode` with notification calls
