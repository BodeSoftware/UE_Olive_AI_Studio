# Phase 1 Dispatcher Pin Pre-Creation

## Problem Statement

When a plan contains `bind_dispatcher` + `custom_event` (the handler), the custom_event node is created in Phase 1 with only its default pins (exec, then, OutputDelegate). The dispatcher's signature pins (e.g., `~InventoryText`, `~Quantity`) only appear AFTER the OutputDelegate pin is connected to the bind_dispatcher's Delegate pin, which triggers `UK2Node_CustomEvent::ReconstructNode()` -> `SetDelegateSignature()`.

The auto-wire that makes this connection runs at the END of Phase 4 (line 3160 of OlivePlanExecutor.cpp), but Phase 4's data wiring loop (line 2879) runs FIRST. When the plan references `@handler.~InventoryText`, `WireDataConnection` fails because that pin does not exist yet on the custom_event node.

**Result**: DATA_PIN_NOT_FOUND errors cause rollback, the agent retries without dispatchers, and falls back to direct calls.

## Root Cause

Phase ordering:
1. Phase 1: CreateNodes -- custom_event created with `AllocateDefaultPins()` (no signature pins)
2. Phase 4 data wiring loop (line 2879-2953): tries to wire `@handler.~InventoryText` -- **FAILS**, pin does not exist
3. Phase 4 delegate auto-wire (line 3160-3262): connects OutputDelegate -> Delegate pin, triggering `ReconstructNode` which adds signature pins -- **TOO LATE**

## Technical Approach

### Core Insight

`UK2Node_CustomEvent::SetDelegateSignature(const UFunction*)` (UE source, K2Node_CustomEvent.cpp:458) populates `UserDefinedPins` from the delegate's signature function. Followed by `ReconstructNode()`, this materializes the output pins on the custom_event node. We already have the `FMulticastDelegateProperty` lookup code in `CreateBindDelegateNode` (OliveNodeFactory.cpp:1284-1313) and in the resolver (OliveBlueprintPlanResolver.cpp:3125-3146).

### The Fix

After Phase 1 creates all nodes (line 426 of OlivePlanExecutor.cpp, after `PhaseCreateNodes` returns), add a **Phase 1.25** pass that:

1. Iterates `bind_dispatcher` steps in the plan
2. For each, finds the matching `custom_event` step using the same two-pass name-matching logic already in the Phase 4 auto-wire post-pass (line 3167-3235)
3. Looks up the `FMulticastDelegateProperty` on the Blueprint (same scan as `CreateBindDelegateNode`)
4. Calls `SetDelegateSignature(DelegateProp->SignatureFunction)` on the `UK2Node_CustomEvent`
5. Calls `ReconstructNode()` to materialize the pins
6. **Rebuilds the pin manifest** for the custom_event step so Phase 4 can find the new pins

Step 6 is critical: the manifest was built in Phase 1 right after node creation, before the signature pins existed. After `ReconstructNode()`, the manifest must be rebuilt to include the new output pins.

### Why Phase 1.25 (not inside Phase 1)

The fix needs BOTH the custom_event node AND the bind_dispatcher step to exist in the context. Since Phase 1 creates nodes sequentially, the custom_event may be created before the bind_dispatcher (or vice versa). A post-Phase-1 pass guarantees both nodes exist.

### Why not move the auto-wire earlier in Phase 4

Moving the delegate auto-wire before the data wiring loop would fix the connection, but `TryCreateConnection` triggers `ReconstructNode()` which invalidates pin pointers. The manifests built in Phase 1 would become stale (pins destroyed and recreated). A dedicated Phase 1.25 that also rebuilds manifests is cleaner and more explicit.

## Detailed Code Changes

### File: OlivePlanExecutor.h

**Location**: After `PhaseAutoWireComponentTargets` declaration (line 298), before `PhaseWireExec` (line 301).

Add new private method declaration:

```cpp
/**
 * Phase 1.25: Pre-create dispatcher signature pins on custom_event nodes.
 *
 * When a plan contains bind_dispatcher + custom_event pairs, the custom_event
 * node needs the dispatcher's parameter pins BEFORE Phase 4 data wiring.
 * Normally these pins only appear after the delegate auto-wire triggers
 * ReconstructNode. This phase pre-creates them using SetDelegateSignature.
 *
 * For each bind_dispatcher step:
 * 1. Find matching custom_event (name-match priority, then first-unwired fallback)
 * 2. Look up FMulticastDelegateProperty on Blueprint
 * 3. Call SetDelegateSignature + ReconstructNode on custom_event node
 * 4. Rebuild pin manifest for the custom_event step
 *
 * CONTINUE-ON-FAILURE. Failure here means Phase 4 data wiring will fail
 * for dispatcher parameter pins, but exec wiring and other data wiring proceed.
 */
void PhasePreCreateDispatcherPins(
    const FOliveIRBlueprintPlan& Plan,
    FOlivePlanExecutionContext& Context);
```

### File: OlivePlanExecutor.cpp

#### Change 1: Call site in Execute() (Senior)

**Location**: Between Phase 1.5 (line 522, `PhaseAutoWireComponentTargets`) and the batch scope for Phases 3-5.5 (line 543).

Insert after Phase 1.5 logging (after line 534):

```cpp
// Phase 1.25: Pre-create dispatcher signature pins on custom_event nodes.
// Must run after Phase 1 (both custom_event and bind_dispatcher nodes exist)
// and before Phase 4 (data wiring needs the signature pins).
UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 1.25: Pre-Create Dispatcher Pins"));
PhasePreCreateDispatcherPins(Plan, Context);
```

Note: Phase 1.25 logically belongs before Phase 1.5 (component auto-wiring), but it can safely run after since component targets and dispatcher pins are orthogonal. Placing it after 1.5 avoids renumbering the existing 1.5 logs.

#### Change 2: PhasePreCreateDispatcherPins implementation (Senior)

**New function body.** Approximate location: after `PhaseAutoWireComponentTargets` definition, before `PhaseWireExec`.

The implementation mirrors the existing Phase 4 auto-wire post-pass (lines 3160-3262) for the matching logic, but instead of wiring pins, it calls `SetDelegateSignature` + `ReconstructNode` + rebuilds the manifest.

**Pseudocode with line-number references to existing patterns**:

```
for each bind_dispatcher step in Plan.Steps:
    // Get dispatcher name (same as line 3192)
    DispatcherName = BindStep.Target

    // --- Look up FMulticastDelegateProperty ---
    // Reuse the same lookup pattern as CreateBindDelegateNode (OliveNodeFactory.cpp:1284-1313)
    // Check cross_blueprint flag from resolved properties first, then self-Blueprint
    DelegateProp = nullptr

    // Check if bind_dispatcher step was resolved with cross_blueprint flag
    // (resolver stores this in Properties: "cross_blueprint" + "target_class")
    // For cross-blueprint, search target class; for self, search Blueprint skeleton/generated

    Search SkeletonGeneratedClass, then GeneratedClass for FMulticastDelegateProperty
    matching DispatcherName

    if !DelegateProp || !DelegateProp->SignatureFunction:
        log warning, continue (Phase 4 auto-wire will also fail, but that's expected)

    // --- Find matching custom_event ---
    // Same two-pass logic as lines 3198-3235
    BestEventNode = nullptr
    BestStepId = ""

    for each custom_event step in Plan.Steps:
        EventNode = Context.GetNodePtr(CandStep.StepId)
        if !EventNode: continue

        CustomEventNode = Cast<UK2Node_CustomEvent>(EventNode)
        if !CustomEventNode: continue

        // Name-match priority (same as line 3218-3226)
        if CandStep.Target contains DispatcherName (case-insensitive):
            BestEventNode = CustomEventNode
            BestStepId = CandStep.StepId
            break

        // Fallback to first candidate
        if !BestEventNode:
            BestEventNode = CustomEventNode
            BestStepId = CandStep.StepId

    if !BestEventNode:
        log warning, continue

    // --- Pre-create signature pins ---
    CustomEventNode->SetDelegateSignature(DelegateProp->SignatureFunction)
    CustomEventNode->ReconstructNode()

    // --- Rebuild pin manifest ---
    // The manifest was built in Phase 1 with only default pins.
    // After ReconstructNode, the node has new output pins matching the delegate signature.
    NodeId = Context.GetNodeId(BestStepId)
    NodeType from StepManifests[BestStepId] (preserve original)

    NewManifest = FOlivePinManifest::Build(BestEventNode, BestStepId, NodeId, NodeType)
    Context.StepManifests[BestStepId] = NewManifest  // Replace old manifest

    log success with pin count
```

**Key details for the delegate property lookup**:

For the self-Blueprint path (the common case), the delegate property lookup is:
```cpp
FMulticastDelegateProperty* DelegateProp = nullptr;
const FName DelegateFName(*DispatcherName);

UClass* SearchClass = Context.Blueprint->SkeletonGeneratedClass
    ? Context.Blueprint->SkeletonGeneratedClass
    : Context.Blueprint->GeneratedClass;

if (SearchClass)
{
    for (TFieldIterator<FMulticastDelegateProperty> It(SearchClass); It; ++It)
    {
        if (It->GetFName() == DelegateFName)
        {
            DelegateProp = *It;
            break;
        }
    }
}
```

For cross-blueprint (bind_dispatcher resolved with `cross_blueprint` property), the resolver stores `target_class` in the resolved step's Properties. Look up via `FOliveClassResolver::Resolve()` then iterate that class instead. This is the same pattern as `CreateBindDelegateNode` lines 1248-1280.

However, at Phase 1.25 time we only have the plan steps, not the resolved steps (those are consumed by Phase 1). We need the resolver properties to know if it's cross-blueprint. **Solution**: Check the plan step's `Inputs` for a `Target` key starting with `@` -- this is the same heuristic the resolver uses (line 3066-3067). If present, it's cross-blueprint. But we don't need to handle cross-blueprint here because:

1. Cross-blueprint dispatchers are rare
2. The custom_event for a cross-blueprint bind would still need the same signature pins
3. The dispatcher property can still be found on the target class

**Simpler approach**: Always search the Blueprint first (covers 95%+ of cases). If the dispatcher is not found on self, check if the bind_dispatcher resolved step stored a `target_class` in its Properties. We DO have access to the resolved steps -- they're passed to `Execute()` and we can thread them through, or we can look at `Context.StepToNodePtr` and cast the bind_dispatcher node to `UK2Node_AddDelegate` to get the delegate property directly from the node.

**Best approach**: Cast the bind_dispatcher node to `UK2Node_AddDelegate` and call `GetDelegateSignature()` to get the `UFunction*` directly. This works for both self and cross-blueprint because the node was already created with the correct delegate property in Phase 1. This completely avoids duplicating the property lookup.

```cpp
UK2Node_AddDelegate* AddDelegateNode = Cast<UK2Node_AddDelegate>(Context.GetNodePtr(BindStep.StepId));
if (!AddDelegateNode) continue;

UFunction* DelegateSig = AddDelegateNode->GetDelegateSignature();
if (!DelegateSig) { log warning; continue; }
```

This is much simpler and more robust. `GetDelegateSignature()` is a public method on `UK2Node_BaseMCDelegate` (parent of `UK2Node_AddDelegate`).

### File: OliveNodeFactory.cpp

**No changes needed.** The custom event creation (`CreateCustomEventNode`, line 705-724) stays as-is. The signature pins are added after Phase 1 by the executor.

### File: OliveBlueprintPlanResolver.cpp

**No changes needed.** The resolver already correctly resolves `bind_dispatcher` and `custom_event` steps.

### File: OlivePinManifest.h / OlivePinManifest.cpp

**No changes needed.** `FOlivePinManifest::Build()` already works on any `UEdGraphNode*` and reads current pins. It will correctly pick up the new signature pins after `ReconstructNode()`.

## Task Breakdown

### Task 1 (Junior): Add method declaration to OlivePlanExecutor.h

**File**: `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`

**What**: Add the `PhasePreCreateDispatcherPins` private method declaration as specified above, between `PhaseAutoWireComponentTargets` and `PhaseWireExec`.

**Expected**: Compiles with empty implementation.

### Task 2 (Senior): Implement PhasePreCreateDispatcherPins

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

**What**: Implement the full `PhasePreCreateDispatcherPins` function. Key steps:
1. Iterate `bind_dispatcher` plan steps
2. Cast `Context.GetNodePtr(BindStep.StepId)` to `UK2Node_AddDelegate*`
3. Call `GetDelegateSignature()` to get `UFunction*`
4. Find matching custom_event using two-pass name logic (reuse pattern from lines 3198-3235)
5. Cast to `UK2Node_CustomEvent*`, call `SetDelegateSignature(DelegateSig)` then `ReconstructNode()`
6. Rebuild manifest: `FOlivePinManifest::Build(CustomEventNode, BestStepId, NodeId, NodeType)` and replace in `Context.StepManifests`
7. Log the new pin count

**Include needed**: `K2Node_AddDelegate.h` (already included at line 39), `K2Node_CustomEvent.h` (already included at line 30).

**No new includes required.**

### Task 3 (Junior): Add call site in Execute()

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

**What**: Add the Phase 1.25 call between Phase 1.5 and the batch scope. Insert after line 534 (the "no auto-wiring needed" log):

```cpp
// Phase 1.25: Pre-create dispatcher signature pins on custom_event nodes
UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 1.25: Pre-Create Dispatcher Pins"));
PhasePreCreateDispatcherPins(Plan, Context);
```

**Expected**: Phase 1.25 runs between Phase 1.5 and Phase 3 in every plan execution. When no bind_dispatcher steps exist, it's a no-op (iterates plan steps once, finds nothing).

## Risk Analysis

### Low Risk

1. **Phase ordering is safe**: Phase 1.25 only reads nodes created in Phase 1 and modifies custom_event nodes. No other phase has touched these nodes yet. Phase 1.5 (component target auto-wire) only operates on `CallFunction` nodes, never custom_events.

2. **`SetDelegateSignature` is idempotent on `UserDefinedPins`**: If called multiple times, it clears and repopulates. No accumulation.

3. **`ReconstructNode()` is safe post-Phase-1**: The node was just created, has no wired connections yet (wiring is Phase 3+), so reconstruction won't break anything.

4. **Manifest rebuild is safe**: `FOlivePinManifest::Build` creates a fresh manifest from current pins. Replacing the old one is a simple map assignment.

5. **No-op when no dispatchers**: The outer loop over `bind_dispatcher` steps exits immediately. Zero cost for plans without dispatchers.

### Medium Risk

1. **Multiple bind_dispatchers sharing one custom_event**: If two dispatchers both match the same custom_event (rare but possible if the plan is malformed), the second `SetDelegateSignature` call would overwrite the first's pins. Mitigation: Track which custom_events have already been signature-set and skip them. Same pattern as the Phase 4 auto-wire which skips already-wired OutputDelegate pins (line 3213-3215).

2. **Cross-blueprint dispatcher lookup**: `GetDelegateSignature()` on the `UK2Node_AddDelegate` node should work for both self and cross-blueprint since `SetFromProperty` was already called in Phase 1. If `GetDelegateSignature()` returns nullptr for some edge case, the fallback is graceful (log warning, skip -- Phase 4 auto-wire will also fail with the same issue).

### Mitigations

- Add a `TSet<FString> SignatureSetStepIds` local to track which custom_event steps already received a signature. Skip if already processed.
- Null-check `GetDelegateSignature()` return value before calling `SetDelegateSignature`.
- Log clearly so failures are diagnosable.

## Verification Steps

1. **Unit test scenario**: Plan with `custom_event` (handler) + `bind_dispatcher` + data wiring from handler's signature pins. Verify Phase 4 data wiring succeeds (no DATA_PIN_NOT_FOUND errors).

2. **Manual test**: Create a Blueprint with an event dispatcher that has parameters (e.g., `OnInventoryChanged(FText InventoryText, int32 Quantity)`). Submit a plan_json that:
   - Creates a `custom_event` named `HandleInventoryChanged`
   - Creates a `bind_dispatcher` targeting `OnInventoryChanged`
   - Wires `@handler.~InventoryText` to a `PrintString` node's `InString` input

   Verify: no rollback, pins exist, wiring succeeds, compiles clean.

3. **Regression test**: Run existing plan_json tests. Phase 1.25 should be a no-op for plans without dispatchers.

4. **Log inspection**: Check `LogOlivePlanExecutor` for "Phase 1.25" entries confirming:
   - Dispatcher found via `GetDelegateSignature()`
   - Custom_event matched
   - Signature pins created (log new pin count)
   - Manifest rebuilt

## Implementation Order

1. Task 1 (Junior) -- header declaration
2. Task 2 (Senior) -- implementation
3. Task 3 (Junior) -- call site

Tasks 1 and 3 can be done by the same person in sequence. Task 2 is the core work.
