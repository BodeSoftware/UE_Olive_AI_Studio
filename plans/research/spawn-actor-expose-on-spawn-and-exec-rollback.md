# Research: SpawnActor ExposeOnSpawn Pin Timing & Exec→Exec TypesIncompatible After Rollback

## Question

1. How does `UK2Node_SpawnActorFromClass` know which pins to show, and how can we force it to pick up newly-added `ExposeOnSpawn` variables after construction?
2. How does "Exec→Exec: TypesIncompatible" arise on BeginPlay's exec output after a failed/rolled-back plan? What is the actual failure path?

---

## Findings: Problem 1 — ExposeOnSpawn Pin Timing

### How pins are created

`UK2Node_SpawnActorFromClass` delegates spawn-variable pin creation to its grandparent class, `UK2Node_ConstructObjectFromClass::CreatePinsForClass()`.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_ConstructObjectFromClass.cpp`, line 120.

The core loop:

```cpp
for (TFieldIterator<FProperty> PropertyIt(InClass, EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
{
    FProperty* Property = *PropertyIt;
    const bool bIsExposedToSpawn = UEdGraphSchema_K2::IsPropertyExposedOnSpawn(Property);
    const bool bIsSettableExternally = !Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance);

    if( bIsExposedToSpawn &&
        !Property->HasAnyPropertyFlags(CPF_Parm) &&
        bIsSettableExternally &&
        Property->HasAllPropertyFlags(CPF_BlueprintVisible) &&
        !bIsDelegate &&
        (nullptr == FindPin(Property->GetFName())) &&
        FBlueprintEditorUtils::PropertyStillExists(Property))
    {
        // Creates the pin
    }
}
```

`IsPropertyExposedOnSpawn()` checks **both** `CPF_ExposeOnSpawn` **and** the `ExposeOnSpawn` meta key. It calls `FBlueprintEditorUtils::GetMostUpToDateProperty(Property)` first to chase renames.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp`, line 1306.

```cpp
bool UEdGraphSchema_K2::IsPropertyExposedOnSpawn(const FProperty* Property)
{
    Property = FBlueprintEditorUtils::GetMostUpToDateProperty(Property);
    if (Property)
    {
        const bool bMeta = Property->HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn);
        const bool bFlag = Property->HasAllPropertyFlags(CPF_ExposeOnSpawn);
        // ...
    }
}
```

**The check happens at the time `CreatePinsForClass()` is called.** It is not dynamic/lazy. If the variable does not yet have the flag when `AllocateDefaultPins()` runs, no pin is created.

### When CreatePinsForClass is called

`CreatePinsForClass` is called from three places in `UK2Node_ConstructObjectFromClass`:

1. `PostPlacedNewNode()` — when the node is dropped in the editor by the user (line 212).
2. `ReallocatePinsDuringReconstruction()` — inside `ReconstructNode()` (line 190).
3. `OnClassPinChanged()` — when the "Class" pin's value or connection changes (line 276).

`AllocateDefaultPins()` (the base constructor path used by `NewObject<>`) does **not** call `CreatePinsForClass`. The spawn-var pins are only created in `PostPlacedNewNode` or `ReconstructNode` paths.

**Consequence for the plan executor:** When Olive creates a SpawnActor node via `NewObject<UK2Node_SpawnActorFromClass>` + `AllocateDefaultPins()`, no `CreatePinsForClass` is called during construction. The node only gets the fixed pins (exec, class, transform, collision, owner, result). If a subsequent step in the same plan sets `ExposeOnSpawn=true` on a variable, those variable pins will never appear on the already-allocated node.

### ReconstructNode behavior

`UK2Node::ReconstructNode()` (line 704 of `K2Node.cpp`) does the following:

1. Calls `Modify()` — writes current pin state to undo buffer.
2. Moves current `Pins` to `OldPins`.
3. Calls `ReallocatePinsDuringReconstruction(OldPins)` — which for this node calls `AllocateDefaultPins()` then `CreatePinsForClass(UseSpawnClass)`.
4. Calls `RewireOldPinsToNewPins(OldPins, Pins, ...)` — attempts to preserve existing connections.
5. Calls `PostReconstructNode()`.

So calling `SpawnActorNode->ReconstructNode()` **after** the variable is marked as ExposeOnSpawn **will** create the new pin. The new pin will be added with no connections (existing exec/transform connections are re-wired from OldPins). This is safe to call.

**However,** `ReconstructNode` calls `Modify()` internally, so it will consume a transaction entry. It must be called inside the same `FScopedTransaction` that created the SpawnActor node — or a new scoped transaction. It must also be called on the game thread.

### Safe approach: call ReconstructNode after variable configuration

The correct sequence for the plan executor:

1. Execute `set_var` step (or equivalent) that adds `ExposeOnSpawn=true` to the variable — this modifies the Blueprint's `SkeletonGeneratedClass` / `NewVariables`.
2. Call `SpawnActorNode->ReconstructNode()`.
3. Continue wiring the new pin.

This is the same path the Blueprint editor takes when a user manually ticks "Expose on Spawn" — it calls `FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified()` → which triggers a reconstruction of all nodes that might have spawned the class.

**Alternative: reorder steps at plan resolution time.** During `FOliveBlueprintPlanResolver::Resolve()`, detect when a `spawn_actor` step is followed by a step that adds a variable with `ExposeOnSpawn=true`. Insert the variable-creation step before the spawn node's allocation. Since `CreatePinsForClass` is called from `PostPlacedNewNode`/`ReconstructNode` and not `AllocateDefaultPins`, the variable needs to exist on `SkeletonGeneratedClass` **before** `ReconstructNode` is called (not necessarily before `NewObject` + `AllocateDefaultPins`).

The ordering fix is simpler and more reliable:
- Variable creation → `ReconstructNode` on spawn node → wire the ExposeOnSpawn pins.

### Note on GetMostUpToDateProperty

`IsPropertyExposedOnSpawn` calls `FBlueprintEditorUtils::GetMostUpToDateProperty(Property)`. This means it uses the live property from the most recently compiled class, not a stale pointer. If the skeleton class has the new variable but the generated class hasn't been recompiled, the property will still be found as long as the property pointer from `SkeletonGeneratedClass` is used.

---

## Findings: Problem 2 — Exec→Exec TypesIncompatible After Rollback

### What "TypesIncompatible" means in our code

The error string "TypesIncompatible" comes from `FOliveWiringDiagnostic::Reason = EOliveWiringFailureReason::TypesIncompatible` set in `OlivePinConnector.cpp`, line 795.

In `BuildWiringDiagnostic`, this branch is the **fallback else** that fires when no other specific classification matched. The message text at line 797 adds "Exec pins carry execution flow, not data..." when **one** of the two pins is exec. But the scenario being reported is **both** pins being exec, and the connection fails.

**Two separate failure causes produce this code:**

1. `CategorizePinsByDirection` returns false (both pins output, or both input) → `CanCreateConnection` returns `CONNECT_RESPONSE_DISALLOW, "Directions are not compatible"` — but Olive's BuildWiringDiagnostic catches direction mismatch separately via `SourcePin->Direction == TargetPin->Direction`. So this case emits `EOliveWiringFailureReason::DirectionMismatch`, not `TypesIncompatible`.

2. **`bOrphanedPin == true`** on one of the pins → `CanCreateConnection` at line 2174 returns `CONNECT_RESPONSE_DISALLOW, "Cannot make new connections to orphaned pin"`. Olive's `BuildWiringDiagnostic` does NOT check `bOrphanedPin`, so it falls through to the generic else block and emits `TypesIncompatible`. **This is the most likely cause.**

### How a pin becomes orphaned after rollback

`bOrphanedPin` is set to `true` in `UK2Node::RewireOldPinsToNewPins()` at line 1440 of `K2Node.cpp`:

```cpp
OldPin->bOrphanedPin = true;
OldPin->bNotConnectable = true;
OrphanedOldPins.Add(OldPin);
```

This happens during `ReconstructNode()` when an old pin cannot be matched to a new pin on the reconstructed node. The orphaned pin is kept alive (added to the new `Pins` array) but marked non-connectable.

`bOrphanedPin` is **serialized to the undo buffer** — it appears explicitly in `UEdGraphPin::ExportTextItem()` at line 1097:

```cpp
ValueStr += PinHelpers::bOrphanedPinName + TEXT("=");
bool LocalOrphanedPin = bOrphanedPin;
BoolPropCDO->ExportTextItem_Direct(ValueStr, &LocalOrphanedPin, nullptr, nullptr, PortFlags, nullptr);
```

The serialization comment notes that `bWasTrashed` and `bSavePinIfOrphaned` are transient (not serialized), but `bOrphanedPin` is NOT transient — it is persisted.

**Therefore: if a transaction modifies a node such that pins become orphaned, and that transaction is rolled back via `FScopedTransaction`, the undo system restores pin state from the saved buffer — which includes `bOrphanedPin=false` for the pre-modification state.** This should be safe.

**But there is a specific failure scenario: the undo buffer for pin state is on the OWNING NODE.** `UEdGraphPin::Modify()` calls `LocalOwningNode->Modify()` — it snapshots the *node*, not the pin directly. If the failed plan created and then immediately destroyed nodes, the BeginPlay node itself may not have had `Modify()` called on it before the plan mutated its `LinkedTo` array.

`MakeLinkTo` at line 532 of `EdGraphPin.cpp`:
```cpp
LinkedTo.Add(ToPin);
ToPin->LinkedTo.Add(this);
```

There is no automatic `Modify()` call inside `MakeLinkTo`. The caller is responsible for calling `Modify()` on the owning nodes. The K2 schema's `TryCreateConnection` calls `FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint)` but does NOT call `Modify()` on the individual nodes.

**If BeginPlay's exec output was wired during a failed plan, and `BeginPlay_Node->Modify()` was never called before the link was made, the undo system will not fully restore the pin's `LinkedTo` array.** After rollback, BeginPlay's pin will still show the stale wire from the failed plan, even though the target node was removed. This leaves `LinkedTo[0]` pointing at a destroyed pin object (or null after GC), which makes the orphan cleanup in `ReconstructNode` mark the pin as broken — but without the usual `bOrphanedPin` flag since it was never reconstructed.

### The actual connection-time check that fails

Looking at `CanCreateConnection` again:

```cpp
if (PinA->bOrphanedPin || PinB->bOrphanedPin)
{
    return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("Cannot make new connections to orphaned pin"));
}
```

And:

```cpp
if (!CategorizePinsByDirection(PinA, PinB, /*out*/ InputPin, /*out*/ OutputPin))
{
    return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("Directions are not compatible"));
}
```

For "Exec→Exec TypesIncompatible" to fire (rather than "Directions are not compatible"):
- The two pins MUST have opposite directions (one input, one output).
- The type check MUST fail via `ArePinsCompatible` returning false.

For two `PC_Exec` pins with opposite directions, `ArePinsCompatible` → `ArePinTypesCompatible` is called. For two exec pins with the same `PinCategory == PC_Exec`, the code path at line 4465:

```cpp
else if (Output.PinCategory == Input.PinCategory)
{
    if ((Output.PinSubCategory == Input.PinSubCategory)
        && (Output.PinSubCategoryObject == Input.PinSubCategoryObject)
        && (Output.PinSubCategoryMemberReference == Input.PinSubCategoryMemberReference))
    {
        ...
        return true;
    }
    // Reals check, etc.
}
```

For exec pins, `PinSubCategory` is empty and `PinSubCategoryObject` is null — so they always match. **Two properly-typed exec pins with opposite directions will ALWAYS return `ArePinsCompatible = true` and `CanCreateConnection` will succeed.**

**This means: if we get "Exec→Exec: TypesIncompatible", one of the following must be true:**

1. The pin's `bOrphanedPin` flag is set (gives "Cannot make new connections to orphaned pin" from `CanCreateConnection`, falls through to our TypesIncompatible catch-all since we don't classify that response code).
2. One pin has been corrupted to have a different `PinCategory` than `PC_Exec` (very unlikely post-undo).
3. `CategorizePinsByDirection` failed — both pins have the same direction. This would give "Directions are not compatible" from `CanCreateConnection`, but OlivePinConnector classifies that as `DirectionMismatch`, not `TypesIncompatible`. **So the "TypesIncompatible" label means this path was not hit.**

**Therefore: The most likely root cause is `bOrphanedPin == true` on BeginPlay's exec output pin.** `CanCreateConnection` returns `CONNECT_RESPONSE_DISALLOW` with the orphan message. Olive's `BuildWiringDiagnostic` does not have a case for `bOrphanedPin`, sees both categories are `PC_Exec` (not data categories), falls through to the generic else, and emits `TypesIncompatible` with the exec-flow message.

### Why bOrphanedPin survives rollback

From the serialization analysis: `bOrphanedPin` is serialized. It should be restored by undo. **However:** if the BeginPlay event node itself was never `Modify()`-called before the first failed plan, the undo buffer has no snapshot of it. After rollback of the failed plan's nodes, GC may trash the dangling pin pointer in BeginPlay's `LinkedTo`. On the *next* `ReconstructNode` call (e.g., triggered by compile or structural modification), the orphan cleanup fires and sets `bOrphanedPin = true` on BeginPlay's exec out pin.

There is a second path: `SetSavePinIfOrphaned()` exists, and exec pins that match `SaveAllButExec` mode are explicitly excluded from being saved as orphans. But that gate is in the "orphan save" path, not the "orphan detection" path. The `bOrphanedPin = true` assignment still runs regardless.

### Role of PostReconstructNode and PinConnectionListChanged

`PostReconstructNode()` (declared at K2Node.h line 318) is a virtual that lets node subclasses do post-reconstruction work. For `UK2Node_Event` (which BeginPlay is), the base implementation is used. It does not clear `bOrphanedPin`.

`PinConnectionListChanged()` is called by `MakeLinkTo/BreakLinkTo` on the owning node when a pin's link list changes. For `UK2Node_ConstructObjectFromClass`, this is overridden to detect class pin changes. For `UK2Node_Event`, the base `UK2Node` version is used — it does not reset `bOrphanedPin`.

**There is no automatic reset of `bOrphanedPin` short of a full `ReconstructNode()` pass.**

---

## Additional Confirmed Findings (2026-03-06)

### MakeLinkTo does NOT call Modify() — confirmed

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Private/EdGraph/EdGraphPin.cpp` line 532.

`MakeLinkTo()` directly appends to `LinkedTo` with no `Modify()` call. `UEdGraphPin::Modify()` itself just
delegates to `LocalOwningNode->Modify()`. The K2 schema's `TryCreateConnection` calls
`MarkBlueprintAsModified(Blueprint)` but NOT `Node->Modify()`.

`BreakLinkTo()` DOES call `Modify()` on both nodes before removing from `LinkedTo` — so breaks
are properly tracked. But wires (makes) are only tracked if the caller explicitly called
`Node->Modify()` beforehand.

### Olive's PhaseCreateNodes reuse gap — confirmed

Source: `OlivePlanExecutor.cpp` line ~535 — when `FindExistingEventNode()` returns a non-null result:

```cpp
if (ExistingNode)
{
    // Reuse existing event node.
    const FString ReuseNodeId = ExistingNode->NodeGuid.ToString();
    // ...registration...
    // MISSING: ExistingNode->Modify()
}
```

When BeginPlay is reused, its node is never `Modify()`'d at plan-start. PhaseWireExec later calls
`TryCreateConnection` (via `OlivePinConnector::Connect`) which calls `Blueprint->Modify()` but not
`BeginPlayNode->Modify()`. The wire is made without a proper undo snapshot on the BeginPlay node.

After rollback, BeginPlay's `LinkedTo` is in an indeterminate state. On next compile or structural
modification, a `ReconstructNode` pass runs, detects a stale link pointing at a destroyed node, and
creates an orphan pin with `bOrphanedPin = true`.

### bOrphanedPin serialization — confirmed not transient

Source: `EdGraphPin.cpp` line 1097. `bOrphanedPin` is explicitly serialized in `ExportTextItem()`.
It is NOT in the transient list (which only includes `bIsDiffing`, `bDisplayAsMutableRef`,
`bSavePinIfOrphaned`, `bWasTrashed`). So `bOrphanedPin = true` persists to disk and survives undo
boundaries if the pin's owner was dirtied after the flag was set.

### CanCreateConnection exec-pin check order — confirmed

Source: `EdGraphSchema_K2.cpp` line 2174 — `bOrphanedPin` check fires BEFORE direction check and
BEFORE type compatibility check. Two `PC_Exec` pins with valid opposite directions will always pass
`ArePinsCompatible()`. So "TypesIncompatible" for exec→exec is definitively a `bOrphanedPin` case
(or very unlikely pin corruption), not a true type mismatch.

---

## Recommendations

### Problem 1 (ExposeOnSpawn timing)

- **Recommended fix:** In `FOlivePlanExecutor`, after executing any step that modifies a variable's `ExposeOnSpawn` flag (e.g., via `set_var` property modification or `blueprint.add_variable` with `expose_on_spawn=true`), scan `Context.Graph->Nodes` for any `UK2Node_SpawnActorFromClass` or `UK2Node_SpawnActor` nodes that reference the modified Blueprint class, and call `Node->ReconstructNode()` on each.

- **Alternative (more surgical):** In `FOliveBlueprintPlanResolver::Resolve()`, detect the `spawn_actor → set_variable_expose_on_spawn` step ordering problem and reorder: move variable creation steps before the spawn node is used. This avoids the need to reconstruct at all, since the variable will exist on the class when `CreatePinsForClass` is called.

- **Do not** call `CreatePinsForClass` directly on an already-constructed node — it does not remove stale pins, does not handle the output type, and does not rewire. Always use `ReconstructNode()`.

- `ReconstructNode()` must be called on the game thread inside a `FScopedTransaction`. The existing transaction from the plan executor is sufficient — `ReconstructNode()` calls `Modify()` internally.

- **Key gotcha:** `CreatePinsForClass` is called from `PostPlacedNewNode()` in the normal editor flow, but Olive's `NewObject` + `AllocateDefaultPins()` path skips `PostPlacedNewNode`. To trigger the equivalent, Olive should call `Node->GetGraph()->NotifyGraphChanged()` after `ReconstructNode` to refresh the editor UI.

### Problem 2 (Exec→Exec TypesIncompatible after rollback)

- **The "TypesIncompatible" label is misleading.** The actual failure is one of:
  1. `bOrphanedPin == true` on the exec pin, OR
  2. A direction mismatch (both output or both input, but classified differently).

- **Immediate fix for diagnostic accuracy:** In `BuildWiringDiagnostic()`, add an explicit check for `bOrphanedPin` before the generic else block:
  ```cpp
  else if (SourcePin->bOrphanedPin || TargetPin->bOrphanedPin)
  {
      Diag.Reason = EOliveWiringFailureReason::OrphanedPin; // add new enum value
      Diag.WhyAutoFixFailed = TEXT("One of the pins is orphaned (left over from a previous failed graph edit). "
          "Call Node->ReconstructNode() to clean up orphaned pins before wiring.");
  }
  ```

- **Root cause fix:** Before wiring exec connections in `PhaseWireExec`, check `Pin->bOrphanedPin` on the source and target pins. If either is orphaned, call `OwningNode->ReconstructNode()` to rebuild the node's pin set. This clears the orphaned state.

- **Rollback + Modify() gap:** The deeper cause is that BeginPlay's node is not `Modify()`-called before a failed plan mutates its `LinkedTo`. In `OlivePlanExecutor::PhaseWireExec`, before calling `MakeLinkTo` (via `TryCreateConnection`), ensure `BeginPlay_Node->Modify()` has been called so the undo buffer has a clean snapshot. The K2 schema's `TryCreateConnection` calls `MarkBlueprintAsModified` but NOT `Node->Modify()`. The `Blueprint->Modify()` call in `OlivePinConnector::Connect` at line 100 is insufficient — it snapshots the Blueprint asset, not the individual node. Individual nodes need their own `Modify()` call for pin state to undo correctly.

- **Verification:** The `bOrphanedPin` hypothesis can be confirmed by logging `Pin->bOrphanedPin` in `WireExecConnection` before calling `Connector.Connect`. If true, the hypothesis is confirmed and `Node->ReconstructNode()` is the fix.

- The `FScopedTransaction` mechanism itself is not broken — it correctly restores what was `Modify()`-called. The bug is missing `Modify()` calls on nodes before wiring.
