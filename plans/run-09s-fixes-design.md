# Run 09s Fixes Design

Two fixes: a Phase 0 validator check and a pre-execution cleanup pass.

---

## Fix 1: EXEC_SOURCE_IS_RETURN (Phase 0 Check 5)

**Problem:** Plans wire `exec_after` or `exec_outputs` FROM a `return`/`set_var`-on-output step (NodeType = `FunctionOutput`). FunctionResult nodes have an exec input but no exec output -- they are terminal. This causes silent wiring failures.

**Implementation:**

### OlivePlanValidator.h

Add after `CheckVariableExists` declaration:

```cpp
/**
 * Check 5: Return node as exec source guard.
 * Rejects exec_after or exec_outputs values that reference a step
 * whose resolved NodeType is FunctionOutput. FunctionResult nodes
 * are terminal (exec input only, no exec output).
 */
static void CheckExecSourceIsReturn(
    const FOlivePlanValidationContext& Context,
    FOlivePlanValidationResult& Result);
```

### OlivePlanValidator.cpp

Add call in `Validate()` after `CheckVariableExists(Context, Result);`:
```cpp
CheckExecSourceIsReturn(Context, Result);
```

Logic (~30 lines):
1. Build `TSet<FString> ReturnStepIds` -- iterate `ResolvedSteps`, collect StepIds where `NodeType == OliveNodeTypes::FunctionOutput`.
2. If set is empty, return early.
3. For each plan step, check:
   - `ExecAfter`: if it names a step in `ReturnStepIds`, emit error.
   - `ExecOutputs`: each value that names a step in `ReturnStepIds`, emit error.
4. Error format:
   - Code: `EXEC_SOURCE_IS_RETURN`
   - StepId: the step that has the bad `exec_after`/`exec_outputs`
   - Message: `"Step '{StepId}' chains exec from '{ReturnStepId}' which resolves to FunctionResult (terminal node, no exec output)."`
   - Suggestion: `"FunctionResult is a terminal node. Chain execution from the step BEFORE the return, or remove the exec reference to '{ReturnStepId}'."`

**Priority:** P0 (Junior). ~30 lines of new code, pattern matches existing Check 2.

---

## Fix 2: Stale Event Chain Cleanup Before Plan Retry

**Problem:** Successful plan_json calls leave nodes on the graph. When the agent retries the same event logic (e.g., after a correction on a different plan), duplicate node chains accumulate. The plan executor reuses the event node but creates new downstream nodes alongside the old ones.

**Solution:** Before Phase 1, scan the incoming plan for event/custom_event steps. For each, find the existing event node and trace its exec chain. If the chain is isolated (no connections to nodes outside the chain), delete all non-event nodes in it. The event node itself is preserved for reuse.

### OlivePlanExecutor.h

Add private method:

```cpp
/**
 * Pre-Phase 1: Remove orphaned exec chains from events the current plan targets.
 * For each event/custom_event step in the plan, finds the existing event node
 * (if any), traces its forward exec chain, and deletes chain nodes that form
 * an isolated subgraph. The event node itself is preserved for Phase 1 reuse.
 *
 * @param Plan          The plan about to execute (scanned for event steps)
 * @param Blueprint     Target Blueprint (for FindExistingEventNode)
 * @param Graph         Target graph (Modify'd by caller)
 * @return Number of nodes removed
 */
int32 CleanupStaleEventChains(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    UBlueprint* Blueprint,
    UEdGraph* Graph);
```

### OlivePlanExecutor.cpp -- `Execute()`

Insert before `PhaseCreateNodes` call (after context initialization, ~line 378):

```cpp
// Pre-Phase 1: Clean up stale event chains that the current plan will recreate
const int32 CleanedCount = CleanupStaleEventChains(Plan, ResolvedSteps, Blueprint, Graph);
if (CleanedCount > 0)
{
    UE_LOG(LogOlivePlanExecutor, Log,
        TEXT("Pre-cleanup: Removed %d stale nodes from event chains targeted by this plan"), CleanedCount);
}
```

### OlivePlanExecutor.cpp -- `CleanupStaleEventChains()` implementation

Algorithm (~80 lines):

1. **Identify target events.** Iterate plan steps + resolved steps. For `event`/`custom_event` ops, extract event name. Also handle `component_bound_event` via resolved NodeType. Collect into `TArray<TPair<FString, bool>> TargetEvents` (name, bIsCustom). For component bound events, use `FindExistingComponentBoundEventNode` instead.

2. **For each target event, find existing node.** Call `FindExistingEventNode(Graph, Blueprint, Name, bIsCustom)`. If nullptr, skip (no cleanup needed -- first time this event appears).

3. **Trace forward exec chain.** BFS/DFS from the event node's exec output pins:
   - Follow `LinkedTo` on each exec output pin
   - Add each reached node to `TSet<UEdGraphNode*> ChainNodes`
   - Do NOT add the event node itself to the removal set
   - Cap traversal at 200 nodes (safety)

4. **Check isolation.** For each node in `ChainNodes`, check ALL pins (exec and data, input and output). If any `LinkedTo` pin connects to a node NOT in `ChainNodes` AND not the event node itself, mark the chain as non-isolated. Skip deletion for non-isolated chains.

5. **Delete isolated chain.** Call `Graph->Modify()`. For each node in `ChainNodes`: `Graph->RemoveNode(Node)`. Increment counter.

**Edge cases:**
- Event node has no exec output connections: `ChainNodes` is empty, nothing to delete. Correct.
- Chain connects to nodes from a DIFFERENT event: isolation check catches it, chain is preserved. Correct.
- Multiple events in the plan target chains that share nodes: isolation check catches shared nodes, both chains preserved. Correct (conservative).
- Component bound events: use `FindExistingComponentBoundEventNode` with delegate name + component name from resolved step properties.

**Priority:** P1 (Senior). ~80 lines, requires careful exec chain traversal and isolation check.

---

## Implementation Order

1. **Fix 1** (Junior, ~30 lines) -- straightforward validator check, pattern-matches existing checks
2. **Fix 2** (Senior, ~80 lines) -- graph traversal logic, needs careful isolation check

## Files Modified

| File | Fix |
|------|-----|
| `Blueprint/Public/Plan/OlivePlanValidator.h` | Fix 1: declaration |
| `Blueprint/Private/Plan/OlivePlanValidator.cpp` | Fix 1: implementation + call site |
| `Blueprint/Public/Plan/OlivePlanExecutor.h` | Fix 2: declaration |
| `Blueprint/Private/Plan/OlivePlanExecutor.cpp` | Fix 2: implementation + call site |
