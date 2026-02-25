# Critical Fix Implementation Plan

**Three fixes for the plan rollback / resolver / loop detection cascade.**

Produced: Feb 25, 2026
Author: Architect Agent

---

## Summary

Three interdependent fixes that address a systemic failure cascade where:
1. Failed plans leave zombie nodes that poison subsequent retries
2. The function resolver matches irrelevant classes when the Blueprint's own components have the right function
3. The loop detector kills runs when errors come from zombie nodes, not from the AI's current plan

## Implementation Order and Dependencies

```
T1 (Rollback on failure)      -- no dependencies, start first
T2 (Replace mode pre-cleanup) -- depends on T1 (uses same rollback tracking)
T3 (Component search order)   -- independent, can run in parallel with T1/T2
T4 (BroadSearch scoring)      -- depends on T3 (uses same BP context plumbing)
T5 (Stale error tagging)      -- depends on T1 (rollback changes context)
T6 (Stale-aware loop detect)  -- depends on T5 (uses stale tags)
```

Recommended parallel lanes:
- **Lane A**: T1 -> T2 -> T5 -> T6
- **Lane B**: T3 -> T4

---

## Fix 1: Plan Rollback on Failure

### T1 -- Rollback created nodes when plan execution produces compile failure

**Files to read first:**
- `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h` (FOlivePlanExecutionContext struct)
- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` (Execute method, lines 87-225; PhaseCreateNodes lines 231-432; CleanupCreatedNodes lambda lines 253-275)
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (v2.0 executor lambda, lines 6635-6840)
- `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp` (StageReport compile error handling, lines 578-612)

**Context:**

Currently, `CleanupCreatedNodes` only runs inside `PhaseCreateNodes` when Phase 1 itself fails (a node couldn't be created). If Phase 1 succeeds but later phases fail (bad wiring, compile errors), all created nodes are committed permanently. The write pipeline's `Transaction->Cancel()` only fires when the executor returns `!bSuccess` -- but for partial success (bPartial), the executor returns `Success()`, so the transaction commits.

The key insight: compile errors are detected in StageReport (Stage 6), AFTER the transaction has already committed in Stage 4. By that point, rollback via transaction cancel is impossible. We need the executor itself to detect failure-worthy conditions and roll back nodes before returning.

However, the executor does NOT know about compile results -- it returns before compilation happens. The compile check is in `StageVerify` (Stage 5) and `StageReport` (Stage 6). So we have two options:
- (A) Move compile into the executor lambda (bad -- violates separation of concerns)
- (B) Make the executor return a structure that the tool handler inspects AFTER pipeline execution, and if compile failed, issue a separate cleanup pass

Neither is clean. The real solution is simpler:

**The key realization: when the executor returns `FOliveWriteResult::Success()` for partial success, the pipeline commits the transaction. Then Stage 5 compiles. If compile fails, Stage 6 sets `bSuccess = false` -- but the transaction is already committed. The nodes survive.**

**The fix**: When the `apply_plan_json` handler receives a final result with `bSuccess == false` AND `compile_result` errors, AND the executor created nodes, perform a post-pipeline cleanup that removes the nodes this plan created. This cleanup runs OUTSIDE the pipeline transaction (which already committed) as a separate edit.

But wait -- there's an even simpler approach that the fix plan hints at. The existing `CleanupCreatedNodes` lambda in Phase 1 already knows how to remove nodes. We can hoist that pattern to the `Execute()` method level and run it when the overall plan has wiring failures (bPartial) that are likely to cause compile errors.

After careful analysis, the cleanest approach is:

**Approach: Post-pipeline cleanup in the tool handler.**

After `ExecuteWithOptionalConfirmation` returns, the handler inspects the result. If the result has compile errors (`compile_result.success == false`), the handler uses the `step_to_node_map` from the result data to remove all non-reused nodes from the graph. This is a separate `FScopedTransaction` that shows up as "Olive AI: Rollback failed plan" in undo history.

**What to implement:**

1. **Add `ReusedStepIds` to `FOlivePlanExecutionContext`** so the handler can distinguish created vs. reused nodes.

2. **Surface `ReusedStepIds` through the result chain.** Add a `TSet<FString> ReusedStepIds` field to `FOliveIRBlueprintPlanResult`. Populate it in `AssembleResult`. Serialize it in the handler's ResultData JSON as `"reused_step_ids": [...]`.

3. **Add rollback logic in the tool handler.** After the pipeline returns, if compile failed, iterate `step_to_node_map`, skip entries in `reused_step_ids`, and remove the rest via `Graph->RemoveNode()`.

4. **For reused event nodes**: Break connections that were made during this plan's execution. Before plan execution, snapshot the event node's existing connections. After failed compile, restore the snapshot by breaking any NEW connections and leaving original ones.

**Files to modify:**

#### `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`

Add to `FOlivePlanExecutionContext`:
```cpp
/** Step IDs that reuse pre-existing event nodes (do NOT remove on rollback) */
TSet<FString> ReusedStepIds;
```

Add to `FOlivePlanExecutionContext`:
```cpp
/** Snapshot of connections on reused event nodes BEFORE plan execution.
 *  Key = StepId, Value = set of "PinName->LinkedNodeGuid:PinName" for each pre-existing connection.
 *  Used to restore event nodes to their pre-plan state on rollback. */
TMap<FString, TSet<FString>> ReusedEventPrePlanConnections;
```

#### `Source/OliveAIRuntime/Public/IR/BlueprintPlanIR.h`

Add to `FOliveIRBlueprintPlanResult`:
```cpp
/** Step IDs that reused pre-existing event nodes (not created by this plan) */
TSet<FString> ReusedStepIds;
```

#### `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

In `PhaseCreateNodes`, where `ReusedStepIds.Add(StepId)` already exists (line 347):
- Also add: `Context.ReusedStepIds.Add(StepId);`
- Immediately after reuse detection, snapshot the event node's connections:
```cpp
// Snapshot connections for rollback
TSet<FString> ConnectionSnapshot;
for (UEdGraphPin* Pin : ExistingNode->Pins)
{
    if (Pin)
    {
        for (UEdGraphPin* Linked : Pin->LinkedTo)
        {
            if (Linked && Linked->GetOwningNode())
            {
                ConnectionSnapshot.Add(FString::Printf(TEXT("%s->%s:%s"),
                    *Pin->GetName(),
                    *Linked->GetOwningNode()->NodeGuid.ToString(),
                    *Linked->GetName()));
            }
        }
    }
}
Context.ReusedEventPrePlanConnections.Add(StepId, MoveTemp(ConnectionSnapshot));
```

In `AssembleResult`:
- Copy `Context.ReusedStepIds` to `Result.ReusedStepIds`.

#### `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

In the v2.0 executor lambda, also capture and serialize `ReusedStepIds` into ResultData:
```cpp
if (PlanResult.ReusedStepIds.Num() > 0)
{
    TArray<TSharedPtr<FJsonValue>> ReusedArr;
    for (const FString& Id : PlanResult.ReusedStepIds)
    {
        ReusedArr.Add(MakeShared<FJsonValueString>(Id));
    }
    ResultData->SetArrayField(TEXT("reused_step_ids"), ReusedArr);
}
```

**After the pipeline call** (after `ExecuteWithOptionalConfirmation` returns), add a new section:

```cpp
// ------------------------------------------------------------------
// 12. Post-pipeline rollback on compile failure
// ------------------------------------------------------------------
// If the pipeline reports compile errors, the nodes created by this plan
// are now zombie nodes. Remove them so the AI can retry cleanly.
if (PipelineResult.IsValid() && PipelineResult.Data.IsValid())
{
    bool bCompileSuccess = true;
    const TSharedPtr<FJsonObject>* CompileResultObj = nullptr;
    if (PipelineResult.Data->TryGetObjectField(TEXT("compile_result"), CompileResultObj) && CompileResultObj)
    {
        (*CompileResultObj)->TryGetBoolField(TEXT("success"), bCompileSuccess);
    }

    if (!bCompileSuccess)
    {
        RollbackPlanNodes(Blueprint, TargetGraph, PipelineResult.Data);
    }
}
```

Add a new static helper function in the anonymous namespace of OliveBlueprintToolHandlers.cpp:

```cpp
/**
 * Remove nodes created by a failed plan, restoring the graph to pre-plan state.
 * Reused event nodes are NOT removed, but connections added during this plan are broken.
 */
static void RollbackPlanNodes(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& ResultData)
{
    if (!Blueprint || !Graph || !ResultData.IsValid())
    {
        return;
    }

    // Extract step_to_node_map and reused_step_ids from result data
    const TSharedPtr<FJsonObject>* MapObj = nullptr;
    if (!ResultData->TryGetObjectField(TEXT("step_to_node_map"), MapObj) || !MapObj)
    {
        return;
    }

    TSet<FString> ReusedStepIds;
    const TArray<TSharedPtr<FJsonValue>>* ReusedArr = nullptr;
    if (ResultData->TryGetArrayField(TEXT("reused_step_ids"), ReusedArr) && ReusedArr)
    {
        for (const auto& Val : *ReusedArr)
        {
            ReusedStepIds.Add(Val->AsString());
        }
    }

    FScopedTransaction Transaction(
        NSLOCTEXT("OliveBPTools", "RollbackPlan", "Olive AI: Rollback failed plan"));
    Blueprint->Modify();

    FOliveGraphWriter& Writer = FOliveGraphWriter::Get();
    int32 RemovedCount = 0;

    for (const auto& Pair : (*MapObj)->Values)
    {
        const FString& StepId = Pair.Key;
        const FString NodeId = Pair.Value->AsString();

        if (ReusedStepIds.Contains(StepId))
        {
            // Reused event node: break connections added by this plan
            // (In practice, the event node just has its exec output disconnected.
            // Since we removed the target nodes, those connections are already broken.)
            continue;
        }

        // Find and remove the node
        UEdGraphNode* Node = Writer.GetCachedNode(
            Blueprint->GetPathName(), NodeId);
        if (Node && Graph->Nodes.Contains(Node))
        {
            Node->BreakAllNodeLinks();
            Graph->RemoveNode(Node);
            Writer.RemoveFromCache(Blueprint->GetPathName(), NodeId);
            RemovedCount++;
        }
    }

    if (RemovedCount > 0)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        UE_LOG(LogOliveBPTools, Log,
            TEXT("Rolled back %d nodes from failed plan on '%s'"),
            RemovedCount, *Blueprint->GetName());
    }
}
```

**IMPORTANT**: `Writer.RemoveFromCache()` is a private method. The coder should instead use `Writer.ClearNodeCache(AssetPath)` after the loop, or make `RemoveFromCache` accessible. Checking the header, `RemoveFromCache` IS private. Use `ClearNodeCache` after all removals instead.

Actually, re-reading the header more carefully, `FOliveGraphWriter::RemoveFromCache` IS private (line 321). The coder has two options:
1. Call `Writer.ClearNodeCache(AssetPath)` after all removals (clears entire cache for this BP -- slightly aggressive but safe since the node IDs are invalidated anyway)
2. Just skip cache cleanup -- the removed nodes will become stale TWeakObjectPtr entries that auto-clean next access

Option 1 is cleaner. Use `Writer.ClearNodeCache(Blueprint->GetPathName())` after the removal loop.

**Edge case: The node might have been created by Phase 1.5 auto-wire (UK2Node_VariableGet injected for component targets).** These getter nodes are NOT in `StepToNodeMap`. They need to be tracked separately. However, when we call `Node->BreakAllNodeLinks()` on the call function node, the getter's output connection breaks. The getter becomes an orphan. Since we then compile (the rollback restores to pre-plan state and a clean compile will happen on next plan), the orphan getter is harmless.

Actually wait -- we should NOT recompile after rollback. The purpose of rollback is to get back to the pre-plan state. The AI will submit a new plan. The new plan's pipeline will compile.

**Also**: The Phase 1.5 getter nodes (UK2Node_VariableGet) are added to the graph but NOT tracked in `StepToNodeMap`. They are effectively leaked on rollback. To fix this properly:

Add to `FOlivePlanExecutionContext`:
```cpp
/** Nodes created by Phase 1.5 auto-wiring (getter nodes for component targets).
 *  Tracked separately from StepToNodePtr because they don't correspond to plan steps. */
TArray<UEdGraphNode*> AutoWiredGetterNodes;
```

In Phase 1.5, after `Context.Graph->AddNode(GetNode, ...)`, add:
```cpp
Context.AutoWiredGetterNodes.Add(GetNode);
```

Surface this through the result chain by including auto-wired getter node IDs in a new ResultData field `"auto_wired_getter_node_ids"`. Or simpler: just include them in the cleanup.

**Simpler approach for Phase 1.5 nodes**: In the `Execute()` method, track ALL nodes in the graph before execution starts, and on rollback, remove any node that was NOT in the pre-execution set. This is the most robust approach -- no tracking gaps possible.

**Final approach for T1:**

In `FOlivePlanExecutor::Execute()`, before Phase 1:
```cpp
// Snapshot graph node count for potential rollback
TSet<UEdGraphNode*> PrePlanNodes;
for (UEdGraphNode* Node : Graph->Nodes)
{
    PrePlanNodes.Add(Node);
}
```

Then surface this through Context or directly in the result so the handler can use it. But this requires plumbing through the lambda capture, which is complex.

**Simplest correct approach**: Track created-node UEdGraphNode* pointers in the tool handler lambda, not in the executor. After `PlanExecutor.Execute()` returns, compare Graph->Nodes against the pre-execution snapshot. Remove any new nodes.

This is what I recommend. The tool handler lambda already has `ExecutionGraph` and `BP`. Add a pre-execution snapshot:

```cpp
// Snapshot for rollback
TSet<FGuid> PrePlanNodeGuids;
for (UEdGraphNode* Node : ExecutionGraph->Nodes)
{
    if (Node) PrePlanNodeGuids.Add(Node->NodeGuid);
}
```

Then after execution, store `PrePlanNodeGuids` in the ResultData (as serialized JSON) so the post-pipeline rollback function can use it.

Wait, this is getting complicated because the lambda returns an `FOliveWriteResult`, and we need to pass the snapshot through the pipeline to the post-pipeline handler. The cleanest way:

**Revised final approach:**

Do the rollback INSIDE the executor lambda, before returning. The lambda already checks `PlanResult.bPartial` and `PlanResult.bSuccess`. Currently, partial success returns `FOliveWriteResult::Success()`. Instead:

1. When partial success: do NOT immediately return. Instead, trigger a compile check right there in the lambda.
2. If compile shows errors, roll back the nodes and return `ExecutionError`.
3. If compile succeeds despite partial wiring, return `Success` as today.

But this means the lambda is doing its own compile, which conflicts with the pipeline's Stage 5 compile. We'd need `Request.bAutoCompile = false` when doing the compile in the lambda, or we'd double-compile.

**Actually, the simplest approach that works with the existing architecture:**

The tool handler wraps the pipeline call. The pipeline returns an `FOliveToolResult`. The handler examines it. If compile failed, the handler issues a separate cleanup transaction.

The pipeline result (`FOliveToolResult`) contains `Data` with `step_to_node_map` and `compile_result`. The handler already has `Blueprint` and `TargetGraph` in scope. The handler does NOT need to be inside the lambda -- it happens after the pipeline returns.

Let me re-read how the handler calls the pipeline and what it returns.

From the code at line 6840, the lambda ends. Then somewhere after that is the `ExecuteWithOptionalConfirmation` call and the return. Let me check.

```cpp
// Line 6648: Executor.BindLambda([...] -> FOliveWriteResult { ... });
// After the lambda binding, the handler calls the pipeline
```

The handler function returns `FOliveToolResult`, not `FOliveWriteResult`. The pipeline internally converts. Let me check the return path.

The handler uses `ExecuteWithOptionalConfirmation` which returns `FOliveToolResult`. So:

```cpp
FOliveToolResult PipelineResult = ExecuteWithOptionalConfirmation(Request, Executor);
// Check PipelineResult for compile failure, then rollback
return PipelineResult;  // or modified version
```

This is the right place. The handler has `Blueprint`, `TargetGraph` (from line 6498), and the result. It can do the cleanup.

**But there's a problem**: `TargetGraph` might be null if `bGraphMissing` was true and the graph was created inside the lambda. In that case, the graph was created inside the transaction. If the executor returned error, the transaction was cancelled and the graph doesn't exist. If the executor returned success (partial), the graph exists and was committed.

For the common case (EventGraph), `TargetGraph` is always non-null. For function graphs that were created inside the lambda, we need to re-find the graph. This is easily done with `FindGraphByName(Blueprint, GraphTarget)` (which is already used at line 6498).

**Final concrete implementation for T1:**

In `HandleBlueprintApplyPlanJson`, after `ExecuteWithOptionalConfirmation` returns:

```cpp
// ------------------------------------------------------------------
// 12. Post-pipeline rollback on compile failure
// ------------------------------------------------------------------
if (!PipelineResult.bSuccess && PipelineResult.Data.IsValid())
{
    // Check if this was a compile failure (not a pre-execution failure)
    const TSharedPtr<FJsonObject>* CompileObj = nullptr;
    bool bCompileFailed = false;
    if (PipelineResult.Data->TryGetObjectField(TEXT("compile_result"), CompileObj) && CompileObj)
    {
        bool bCompileSuccess = true;
        (*CompileObj)->TryGetBoolField(TEXT("success"), bCompileSuccess);
        bCompileFailed = !bCompileSuccess;
    }

    const TSharedPtr<FJsonObject>* StepMapObj = nullptr;
    const bool bHasStepMap = PipelineResult.Data->TryGetObjectField(TEXT("step_to_node_map"), StepMapObj) && StepMapObj;

    if (bCompileFailed && bHasStepMap)
    {
        // Re-find graph (may have been created inside the pipeline transaction)
        UEdGraph* RollbackGraph = FindGraphByName(Blueprint, GraphTarget);
        if (RollbackGraph)
        {
            // Build reused set
            TSet<FString> ReusedIds;
            const TArray<TSharedPtr<FJsonValue>>* ReusedArr = nullptr;
            if (PipelineResult.Data->TryGetArrayField(TEXT("reused_step_ids"), ReusedArr) && ReusedArr)
            {
                for (const auto& V : *ReusedArr) ReusedIds.Add(V->AsString());
            }

            // Rollback
            int32 RemovedCount = RollbackPlanNodes(Blueprint, RollbackGraph, *StepMapObj, ReusedIds, AssetPath);
            if (RemovedCount > 0)
            {
                // Add rollback info to the result
                PipelineResult.Data->SetNumberField(TEXT("rolled_back_nodes"), RemovedCount);
                PipelineResult.Data->SetStringField(TEXT("rollback_message"),
                    FString::Printf(TEXT("Rolled back %d nodes from failed plan. "
                        "The graph has been restored to its pre-plan state. "
                        "Fix the plan and resubmit."), RemovedCount));
            }
        }
    }
}

return PipelineResult;
```

The `RollbackPlanNodes` helper:

```cpp
/**
 * Remove nodes created by a failed plan execution.
 * @return Number of nodes removed
 */
static int32 RollbackPlanNodes(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FJsonObject& StepToNodeMap,
    const TSet<FString>& ReusedStepIds,
    const FString& AssetPath)
{
    FScopedTransaction Transaction(
        NSLOCTEXT("OliveBPTools", "RollbackPlan", "Olive AI: Rollback failed plan"));
    Blueprint->Modify();

    FOliveGraphWriter& Writer = FOliveGraphWriter::Get();
    int32 RemovedCount = 0;

    for (const auto& Pair : StepToNodeMap.Values)
    {
        const FString& StepId = Pair.Key;
        if (ReusedStepIds.Contains(StepId))
        {
            continue; // Do not remove pre-existing event nodes
        }

        const FString NodeId = Pair.Value->AsString();
        UEdGraphNode* Node = Writer.GetCachedNode(AssetPath, NodeId);
        if (Node && Graph->Nodes.Contains(Node))
        {
            Node->BreakAllNodeLinks();
            Graph->RemoveNode(Node);
            RemovedCount++;
        }
    }

    // Clear GraphWriter cache for this Blueprint (node IDs are now invalid)
    if (RemovedCount > 0)
    {
        Writer.ClearNodeCache(AssetPath);
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }

    return RemovedCount;
}
```

**CRITICAL**: `FOliveToolResult` does not have a `Data` field directly -- it wraps the JSON result. The coder needs to check how `ExecuteWithOptionalConfirmation` returns data. Looking at the pattern, `ExecuteWithOptionalConfirmation` returns `FOliveToolResult`. The `FOliveToolResult` has a `Data` field (TSharedPtr<FJsonObject>). Check `OliveToolRegistry.h` for the struct definition.

Let me verify.

Looking at my agent memory: "FOliveToolResult vs FOliveWriteResult: Tool handlers return FOliveToolResult; pipeline returns FOliveWriteResult which converts via .ToToolResult()."

So the pipeline returns FOliveWriteResult, which is converted to FOliveToolResult by the infrastructure. The handler needs to inspect the FOliveToolResult's Data field. The Data field should contain the nested `step_to_node_map` etc. inside `data` (since ToJson nests ResultData under `data`).

Wait -- the handler gets the raw `FOliveToolResult` back from `ExecuteWithOptionalConfirmation`. The `Data` field is the raw TSharedPtr<FJsonObject>, which IS the pipeline's ResultData. So `PipelineResult.Data->TryGetObjectField("compile_result", ...)` should work directly (the pipeline's StageReport puts compile_result directly into ResultData at line 582).

Good. The coder should verify this by checking what `ExecuteWithOptionalConfirmation` actually returns and how `Data` is populated.

**What to test:**
1. Create a plan that will fail to compile (e.g., call a component function without Target wired). Verify nodes are rolled back after compile failure.
2. Create a plan that succeeds. Verify no rollback happens.
3. Create a plan with a reused event node that fails to compile. Verify the event node is NOT removed but new nodes are.

---

### T2 -- Replace mode pre-cleanup

**Files to read first:**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (v2.0 executor lambda, lines 6635-6840)

**Depends on:** T1 (uses same rollback infrastructure)

**What to implement:**

When `Mode == "replace"`, before executing the plan, clear all non-event, non-function-entry nodes from the target graph. This gives the AI a clean slate.

In the v2.0 executor lambda, after finding/creating the graph but BEFORE `PlanExecutor.Execute()`:

```cpp
// Replace mode: clear graph of non-entry nodes
if (CapturedMode == TEXT("replace") && ExecutionGraph)
{
    TArray<UEdGraphNode*> NodesToRemove;
    for (UEdGraphNode* Node : ExecutionGraph->Nodes)
    {
        if (!Node) continue;
        // Keep event nodes and function entry nodes
        if (Node->IsA<UK2Node_Event>()) continue;
        if (Node->IsA<UK2Node_CustomEvent>()) continue;
        if (Node->IsA<UK2Node_FunctionEntry>()) continue;
        // Keep function result nodes too
        if (Node->IsA<UK2Node_FunctionResult>()) continue;
        NodesToRemove.Add(Node);
    }

    for (UEdGraphNode* Node : NodesToRemove)
    {
        Node->BreakAllNodeLinks();
        ExecutionGraph->RemoveNode(Node);
    }

    // Clear cache since we removed nodes
    FOliveGraphWriter::Get().ClearNodeCache(AssetPath);

    UE_LOG(LogOliveBPTools, Log,
        TEXT("Replace mode: cleared %d non-entry nodes from graph '%s'"),
        NodesToRemove.Num(), *GraphTarget);
}
```

**Also needed**: Capture `Mode` into the lambda. Currently it's parsed at line 6431 but not captured. Add `CapturedMode = Mode` to the lambda capture list.

The lambda capture at line 6648-6652 needs to add `Mode`:
```cpp
Executor.BindLambda(
    [CapturedResolvedSteps = MoveTemp(CapturedResolvedSteps),
     CapturedPlan = MoveTemp(CapturedPlan),
     CapturedResolverNotes = MoveTemp(CapturedResolverNotes),
     CapturedMode = Mode,          // <-- ADD THIS
     AssetPath, GraphTarget]
```

**Headers needed in OliveBlueprintToolHandlers.cpp** (probably already included):
- `K2Node_FunctionResult.h` -- verify this is included. If not, add it.

**What to test:**
1. Submit a plan with `mode: "replace"` on EventGraph that has existing nodes. Verify old nodes are removed before new ones are created.
2. Verify event nodes (BeginPlay, Tick) survive the replace clear.
3. Submit a plan with `mode: "merge"` (default). Verify no pre-cleanup happens.

---

## Fix 2: Blueprint-Aware Function Resolver

### T3 -- Add component classes to the search order

**Files to read first:**
- `Source/OliveAIEditor/Blueprint/Public/Plan/OliveFunctionResolver.h` (GetSearchOrder signature, lines 156-161)
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp` (GetSearchOrder implementation, lines 138-196; Resolve main entry, lines 40-132)
- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` (Phase 1.5 SCS traversal pattern, lines 485-624 -- reuse same SCS scan logic)

**What to implement:**

#### Part A: Modify `GetSearchOrder` in `OliveFunctionResolver.cpp`

Between the "Blueprint parent class hierarchy" section (step 2) and the "Common library classes" section (step 3), insert a new step that scans the Blueprint's SCS for component classes:

```cpp
// 2.5: Component classes on this Blueprint's SCS
if (Blueprint && Blueprint->SimpleConstructionScript)
{
    TArray<USCS_Node*> AllSCSNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
    for (USCS_Node* SCSNode : AllSCSNodes)
    {
        if (SCSNode && SCSNode->ComponentClass && !Added.Contains(SCSNode->ComponentClass))
        {
            Result.Add(SCSNode->ComponentClass);
            Added.Add(SCSNode->ComponentClass);
        }
    }
}
```

**Headers needed in OliveFunctionResolver.cpp:**
- `Engine/SimpleConstructionScript.h` -- ADD if not present
- `Engine/SCS_Node.h` -- ADD if not present

Check current includes (lines 1-33). `SimpleConstructionScript.h` and `SCS_Node.h` are NOT currently included. Add them.

#### Part B: Add `ComponentClassSearch` match method

In `OliveFunctionResolver.h`, add to the `EMatchMethod` enum (after `ParentClassSearch`):
```cpp
ComponentClassSearch,   // Found on a component class on this Blueprint's SCS
```

In `OliveFunctionResolver.cpp`, in the `MatchMethodToString` function, add a case for `ComponentClassSearch`.

Now, to distinguish between parent class matches and component class matches in the `Resolve` function's Strategy 1 loop, we need to know whether the class came from the SCS or from the parent hierarchy. The simplest way: track which classes are component classes.

Modify `GetSearchOrder` to also output a set of component class indices (or use a separate return). But that changes the signature. Simpler: after Strategy 1 finds a match, check if the matching class is a component class (via `IsChildOf(UActorComponent::StaticClass())`):

In `Resolve`, after Strategy 1 finds a match:
```cpp
if (Found)
{
    FOliveFunctionMatch Match;
    Match.Function = Found;
    Match.OwningClass = Class;
    // Determine match method based on class type
    if (Class->IsChildOf(UActorComponent::StaticClass()))
    {
        Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::ComponentClassSearch;
    }
    else
    {
        Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::ExactName;
    }
    Match.Confidence = 100;
    Match.DisplayName = Found->GetDisplayNameText().ToString();
    return Match;
}
```

Similarly for Strategy 2 (K2 prefix):
```cpp
if (Found)
{
    FOliveFunctionMatch Match;
    Match.Function = Found;
    Match.OwningClass = Class;
    if (Class->IsChildOf(UActorComponent::StaticClass()))
    {
        Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::ComponentClassSearch;
        Match.Confidence = 95;
    }
    else
    {
        Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::K2Prefix;
        Match.Confidence = 95;
    }
    ...
}
```

**What to test:**
1. Resolve `SetSpeed` on a Blueprint with `ProjectileMovementComponent`. Should find `UProjectileMovementComponent::SetSpeed` with `ComponentClassSearch` method, confidence 100.
2. Resolve `Delay` on the same Blueprint. Should NOT match on any component. Should find it on `KismetSystemLibrary` as before.
3. Resolve `SetRelativeLocation` on a Blueprint with `StaticMeshComponent`. Should find it via inheritance (`USceneComponent::K2_SetRelativeLocation`) through the component search, with `ComponentClassSearch` method.
4. Blueprint with no SCS (data-only BP). Should behave exactly as before.

---

### T4 -- Relevance-aware BroadSearch scoring with minimum threshold

**Files to read first:**
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp` (BroadSearch, lines 413-479; Resolve main entry Strategy 5, lines 119-127)
- `Source/OliveAIEditor/Blueprint/Public/Plan/OliveFunctionResolver.h` (BroadSearch signature, line 134)

**Depends on:** T3 (same conceptual understanding of component classes; also needs Blueprint* in BroadSearch)

**What to implement:**

#### Part A: Pass Blueprint context to BroadSearch

Change `BroadSearch` signature to accept a `UBlueprint*`:

In `OliveFunctionResolver.h`:
```cpp
static TArray<FOliveFunctionMatch> BroadSearch(
    const FString& FunctionName,
    int32 MaxResults,
    UBlueprint* Blueprint = nullptr);
```

In `OliveFunctionResolver.cpp`, update the implementation signature. At the call site in `Resolve` (line 121), pass Blueprint:
```cpp
TArray<FOliveFunctionMatch> BroadResults = BroadSearch(FunctionName, 1, Blueprint);
```

Also update the call in `GetCandidates` (line 534):
```cpp
TArray<FOliveFunctionMatch> BroadResults = BroadSearch(
    FunctionName, MaxResults - Candidates.Num(), nullptr);
```
(GetCandidates doesn't have Blueprint context, so pass nullptr.)

#### Part B: Relevance-aware scoring

In `BroadSearch`, after finding a match (line 450), replace the confidence assignment:

```cpp
if (Found && Found->HasAnyFunctionFlags(FUNC_BlueprintCallable))
{
    // Determine relevance-aware confidence
    int32 Confidence = 0;

    if (bIsFunctionLibrary)
    {
        Confidence = 70; // Libraries are designed for general use
    }
    else if (bIsGameplayClass && Blueprint && Blueprint->SimpleConstructionScript)
    {
        // Check if this class matches a component on the Blueprint
        bool bIsOnBlueprint = false;
        for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (SCSNode && SCSNode->ComponentClass &&
                Class->IsChildOf(SCSNode->ComponentClass))
            {
                bIsOnBlueprint = true;
                break;
            }
        }
        Confidence = bIsOnBlueprint ? 90 : 40;
    }
    else if (bIsGameplayClass)
    {
        Confidence = 40; // Gameplay class with no Blueprint context
    }
    else
    {
        Confidence = 40; // Fallback
    }

    FOliveFunctionMatch Match;
    Match.Function = Found;
    Match.OwningClass = Class;
    Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::BroadClassSearch;
    Match.Confidence = Confidence;
    Match.DisplayName = Found->GetDisplayNameText().ToString();
    Results.Add(Match);
    ...
}
```

#### Part C: Minimum acceptance threshold

In `Resolve`, Strategy 5 (around line 119-127), add a threshold check:

```cpp
// STRATEGY 5: Broad search
{
    TArray<FOliveFunctionMatch> BroadResults = BroadSearch(FunctionName, 5, Blueprint);
    if (BroadResults.Num() > 0)
    {
        const FOliveFunctionMatch& Best = BroadResults[0];
        if (Best.Confidence >= 60)
        {
            return Best;
        }

        // Below threshold: log rejection and fall through to failure
        UE_LOG(LogOliveFunctionResolver, Warning,
            TEXT("BroadSearch found '%s' on %s but confidence %d < threshold 60. Rejecting."),
            *FunctionName,
            Best.OwningClass ? *Best.OwningClass->GetName() : TEXT("?"),
            Best.Confidence);

        // Fall through to failure path -- candidates will be used for suggestions
    }
}
```

Also update `GetCandidates` to NOT apply the threshold (it's for suggestions, not acceptance).

**What to test:**
1. `SetSpeed` on BP with no ProjectileMovementComponent, no explicit target_class. BroadSearch finds `WindDirectionalSourceComponent::SetSpeed` at confidence 40. Threshold rejects it. Resolver returns failure with suggestion "Did you mean WindDirectionalSourceComponent::SetSpeed (40%)?".
2. `Delay` (library function). BroadSearch finds `KismetSystemLibrary::Delay` at confidence 70. Passes threshold.
3. `SetSpeed` on BP WITH ProjectileMovementComponent. Should never reach BroadSearch -- T3's component search catches it at confidence 100.

---

## Fix 3: Smarter Loop Detection

### T5 -- Tag warnings with step ownership (stale vs current plan)

**Files to read first:**
- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` (PhasePreCompileValidation, lines 1852-2036; Check 2 "step: unknown" tagging, line 2022)
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (v2.0 result serialization, lines 6686-6773)

**Depends on:** T1 (with rollback, stale errors become much rarer, but we still need detection for edge cases)

**What to implement:**

Phase 5.5's Check 2 already emits `(step: unknown)` for nodes not in the current plan vs `(step: set_speed)` for nodes that ARE in the plan. This tagging is embedded in the `PreCompileIssues` strings and forwarded to `Warnings`.

The issue is that the compile errors from Stage 5 (StageVerify) are DIFFERENT strings from Phase 5.5's warnings. The compile errors come from `FKismetCompilerContext` and say things like:

> "This blueprint (self) is not a WindDirectionalSourceComponent, therefore 'Target' must have a connection."

These don't have step ownership tags. They're raw compiler output.

**Approach:** Instead of trying to parse compiler error strings (brittle), use a different signal. After T1's rollback is implemented, failed plans don't leave zombie nodes. So stale errors would only come from:
1. Nodes created by granular tools (add_node, connect_pins) before the plan
2. Pre-existing errors in the Blueprint

For case 1: The compile errors will mention node types/functions that are NOT in the current plan's step list. We can cross-reference.

For case 2: Baseline the Blueprint's compile state before the plan.

**Concrete implementation:**

Add a new field to the ResultData JSON that the self-correction policy can inspect:

In the v2.0 executor lambda, BEFORE executing the plan, compile and record any pre-existing errors:

```cpp
// Baseline: record pre-existing compile errors
TSet<FString> PreExistingCompileErrors;
{
    FOliveIRCompileResult BaselineCompile = FOliveCompileManager::CompileAndGatherErrors(BP);
    for (const FString& Err : BaselineCompile.Errors)
    {
        PreExistingCompileErrors.Add(Err);
    }
}
```

Wait -- this compiles the Blueprint before the plan runs, which is expensive and changes the Blueprint state. Bad idea in the executor lambda.

**Better approach:** Don't compile beforehand. Instead, after the plan runs and compile fails, cross-reference the compile errors against the plan's resolved steps.

In the tool handler, after the pipeline returns with compile errors, add metadata:

```cpp
// Annotate compile errors with ownership info
if (bCompileFailed && StepMapObj)
{
    // Build a set of function/class names from the plan's steps
    TSet<FString> PlanFunctionNames;
    TSet<FString> PlanClassNames;
    for (const FOliveResolvedStep& Step : CapturedResolvedSteps)
    {
        if (const FString* FN = Step.Properties.Find(TEXT("function_name")))
            PlanFunctionNames.Add(*FN);
        if (const FString* TC = Step.Properties.Find(TEXT("target_class")))
            PlanClassNames.Add(*TC);
    }
    ResultData->SetBoolField(TEXT("has_stale_errors"), ...);
}
```

But the problem is the handler doesn't have `CapturedResolvedSteps` -- those are captured inside the lambda. We need a different approach.

**Simplest approach that works:** In the executor lambda, embed step ownership info directly in the plan result warnings. Phase 5.5 already does this. The issue is the compile errors from Stage 5 don't have it.

**Key insight from the fix plan:** The Phase 5.5 `UNWIRED_TARGET` warning already says `(step: unknown)` vs `(step: set_speed)`. The compile error message says "WindDirectionalSourceComponent" and "Target". The self-correction policy needs to correlate these.

**Revised approach for T5:**

In the executor's `AssembleResult`, add a new field to `FOliveIRBlueprintPlanResult`:

```cpp
/** Set of class names mentioned in this plan's resolved steps.
 *  Used by self-correction policy to distinguish stale compile errors
 *  from errors caused by this plan. */
TSet<FString> PlanClassNames;

/** Set of function names mentioned in this plan's resolved steps. */
TSet<FString> PlanFunctionNames;
```

Populate in `AssembleResult`:
```cpp
// Collect plan's class/function names for stale error detection
for (const auto& Step : Plan.Steps)
{
    // These are from the RESOLVED steps in context, not raw plan steps
}
```

Wait, `AssembleResult` takes `Plan` (raw) and `Context` but not `ResolvedSteps`. The resolved steps are in the caller's scope. We need to either pass them or extract from context.

**Simplest: Add resolved step info to context.**

In `FOlivePlanExecutionContext`, add:
```cpp
/** Function names resolved in this plan (for stale error detection) */
TSet<FString> ResolvedFunctionNames;

/** Class names resolved in this plan (for stale error detection) */
TSet<FString> ResolvedClassNames;
```

Populate in `Execute()` before Phase 1, from `ResolvedSteps`:
```cpp
for (const FOliveResolvedStep& Step : ResolvedSteps)
{
    if (const FString* FN = Step.Properties.Find(TEXT("function_name")))
        Context.ResolvedFunctionNames.Add(*FN);
    if (const FString* TC = Step.Properties.Find(TEXT("target_class")))
        Context.ResolvedClassNames.Add(*TC);
}
```

Then in `AssembleResult`, copy to result:
```cpp
Result.PlanClassNames = Context.ResolvedClassNames;
Result.PlanFunctionNames = Context.ResolvedFunctionNames;
```

And in `FOliveIRBlueprintPlanResult`, add:
```cpp
/** Class names from this plan's resolved steps */
TSet<FString> PlanClassNames;

/** Function names from this plan's resolved steps */
TSet<FString> PlanFunctionNames;
```

In the tool handler, serialize these into ResultData:
```cpp
if (PlanResult.PlanClassNames.Num() > 0)
{
    TArray<TSharedPtr<FJsonValue>> ClassArr;
    for (const FString& CN : PlanResult.PlanClassNames)
        ClassArr.Add(MakeShared<FJsonValueString>(CN));
    ResultData->SetArrayField(TEXT("plan_class_names"), ClassArr);
}
if (PlanResult.PlanFunctionNames.Num() > 0)
{
    TArray<TSharedPtr<FJsonValue>> FuncArr;
    for (const FString& FN : PlanResult.PlanFunctionNames)
        FuncArr.Add(MakeShared<FJsonValueString>(FN));
    ResultData->SetArrayField(TEXT("plan_function_names"), FuncArr);
}
```

**Files to modify:**

1. `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h` -- Add `ResolvedFunctionNames`, `ResolvedClassNames` to context
2. `Source/OliveAIRuntime/Public/IR/BlueprintPlanIR.h` -- Add `PlanClassNames`, `PlanFunctionNames` to result
3. `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` -- Populate in Execute() and AssembleResult()
4. `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` -- Serialize into ResultData

---

### T6 -- Stale-aware loop detection in self-correction policy

**Files to read first:**
- `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h` (Evaluate signature, HasCompileFailure)
- `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` (compile failure handling, lines 72-98; HasCompileFailure, lines 175-253)
- `Source/OliveAIEditor/Public/Brain/OliveRetryPolicy.h` (FOliveLoopDetector)

**Depends on:** T5 (uses plan_class_names, plan_function_names from result data)

**What to implement:**

In `Evaluate`, when a compile failure is detected, check whether the compile error mentions class names or function names that are NOT in the current plan. If so, classify the error as "stale" and handle it differently.

#### Modify `HasCompileFailure` to also extract plan ownership data

Change signature (or add a new overload):
```cpp
bool HasCompileFailure(
    const FString& ResultJson,
    FString& OutErrors,
    FString& OutAssetPath,
    bool& OutHasStaleErrors) const;
```

In the implementation, after extracting compile errors, also check for `plan_class_names` and `plan_function_names`:

```cpp
// Check if compile errors are stale (caused by nodes NOT in this plan)
OutHasStaleErrors = false;
TSet<FString> PlanClasses;
TSet<FString> PlanFunctions;

// Extract from data (which is nested inside the result JSON)
const TSharedPtr<FJsonObject>* DataObjPtr = nullptr;
TSharedPtr<FJsonObject> EffectiveDataObj = DataObj.IsValid() ? DataObj : JsonObj;
if (EffectiveDataObj.IsValid())
{
    const TArray<TSharedPtr<FJsonValue>>* ClassArr = nullptr;
    if (EffectiveDataObj->TryGetArrayField(TEXT("plan_class_names"), ClassArr) && ClassArr)
    {
        for (const auto& V : *ClassArr) PlanClasses.Add(V->AsString());
    }
    const TArray<TSharedPtr<FJsonValue>>* FuncArr = nullptr;
    if (EffectiveDataObj->TryGetArrayField(TEXT("plan_function_names"), FuncArr) && FuncArr)
    {
        for (const auto& V : *FuncArr) PlanFunctions.Add(V->AsString());
    }
}

if (PlanClasses.Num() > 0 || PlanFunctions.Num() > 0)
{
    // Check each compile error line against plan classes/functions
    bool bAnyErrorMatchesPlan = false;
    TArray<FString> ErrorLines;
    OutErrors.ParseIntoArrayLines(ErrorLines);

    for (const FString& Line : ErrorLines)
    {
        bool bLineMatchesPlan = false;
        for (const FString& ClassName : PlanClasses)
        {
            if (Line.Contains(ClassName))
            {
                bLineMatchesPlan = true;
                break;
            }
        }
        if (!bLineMatchesPlan)
        {
            for (const FString& FuncName : PlanFunctions)
            {
                if (Line.Contains(FuncName))
                {
                    bLineMatchesPlan = true;
                    break;
                }
            }
        }
        if (bLineMatchesPlan)
        {
            bAnyErrorMatchesPlan = true;
        }
    }

    // If NO compile error mentions any class/function from this plan,
    // all errors are stale (from previous plans or pre-existing issues)
    OutHasStaleErrors = !bAnyErrorMatchesPlan;
}
```

#### Modify `Evaluate` to handle stale errors differently

In the compile failure handling section (lines 72-98):

```cpp
FString CompileErrors, AssetPath;
bool bHasStaleErrors = false;
if (HasCompileFailure(ResultJson, CompileErrors, AssetPath, bHasStaleErrors))
{
    if (bHasStaleErrors)
    {
        // Stale error: compile error is NOT caused by this plan's steps.
        // Do NOT count toward loop detector. Inject different guidance.
        Decision.Action = EOliveCorrectionAction::FeedBackErrors;
        Decision.EnrichedMessage = FString::Printf(
            TEXT("[STALE COMPILE ERROR] The compile error is NOT caused by your current plan. "
                 "It comes from leftover nodes in the graph from a previous operation. "
                 "Errors:\n%s\n"
                 "RECOMMENDED: Resubmit your plan with mode: \"replace\" to clear the graph "
                 "before creating new nodes. Alternatively, use blueprint.read to find the "
                 "offending nodes and remove them with blueprint.remove_node."),
            *CompileErrors);
        Decision.AttemptNumber = 1; // Don't count this toward attempts
        Decision.MaxAttempts = Policy.MaxRetriesPerError;

        UE_LOG(LogOliveAI, Log,
            TEXT("SelfCorrection: Stale compile error on '%s' — not counting toward loop detector"),
            *AssetPath);
        return Decision;
    }

    // Normal compile error handling (existing code)
    const FString Signature = FOliveLoopDetector::BuildCompileErrorSignature(AssetPath, CompileErrors);
    LoopDetector.RecordAttempt(Signature, ...);
    ...
}
```

**What to test:**
1. Plan fails with compile error mentioning `WindDirectionalSourceComponent`. Plan's `plan_class_names` does NOT include `WindDirectionalSourceComponent`. Error classified as stale. Loop detector NOT incremented.
2. Plan fails with compile error mentioning `ProjectileMovementComponent`. Plan's `plan_class_names` INCLUDES `ProjectileMovementComponent`. Error classified as current-plan. Loop detector incremented normally.
3. Blueprint has pre-existing compile errors. Plan succeeds for its own steps, but those errors persist. Classified as stale.

---

## Complete File Modification List

| File | Tasks | Changes |
|------|-------|---------|
| `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h` | T1, T5 | Add `ReusedStepIds`, `ReusedEventPrePlanConnections`, `AutoWiredGetterNodes`, `ResolvedFunctionNames`, `ResolvedClassNames` to context |
| `Source/OliveAIRuntime/Public/IR/BlueprintPlanIR.h` | T1, T5 | Add `ReusedStepIds`, `PlanClassNames`, `PlanFunctionNames` to result |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` | T1, T5 | Populate new context fields in PhaseCreateNodes and Execute; copy to result in AssembleResult |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | T1, T2, T5 | Add `RollbackPlanNodes` helper; add post-pipeline rollback section; capture Mode into lambda; add replace mode pre-cleanup; serialize reused_step_ids, plan_class_names, plan_function_names |
| `Source/OliveAIEditor/Blueprint/Public/Plan/OliveFunctionResolver.h` | T3, T4 | Add `ComponentClassSearch` enum value; change BroadSearch signature |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp` | T3, T4 | Add SCS scan to GetSearchOrder; add component-aware match method detection in Resolve; change BroadSearch scoring; add threshold check |
| `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h` | T6 | Change HasCompileFailure signature (add bHasStaleErrors out param) |
| `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | T6 | Implement stale error detection; add stale-error branch in Evaluate |

---

## New Error Codes

None. Existing codes are reused:
- `COMPILE_FAILED` (unchanged, but now the rollback message accompanies it)
- `PLAN_EXECUTION_FAILED` (unchanged)

## New Result Data Fields

| Field | Type | Where | Purpose |
|-------|------|-------|---------|
| `reused_step_ids` | string[] | ResultData | Which steps reused existing event nodes |
| `rolled_back_nodes` | int | ResultData | How many nodes were rolled back after compile failure |
| `rollback_message` | string | ResultData | Human-readable rollback explanation |
| `plan_class_names` | string[] | ResultData | Class names from this plan's resolved steps |
| `plan_function_names` | string[] | ResultData | Function names from this plan's resolved steps |

---

## Task Summary for Coder

### T1: Rollback created nodes on compile failure
- **Effort**: ~2 hours
- **Files**: OlivePlanExecutor.h, BlueprintPlanIR.h, OlivePlanExecutor.cpp, OliveBlueprintToolHandlers.cpp
- **Key**: Add `RollbackPlanNodes` helper in tool handlers. Populate `ReusedStepIds` in context+result. Add post-pipeline rollback check after `ExecuteWithOptionalConfirmation`.
- **Critical detail**: Use `Writer.ClearNodeCache(AssetPath)` after removals, NOT `Writer.RemoveFromCache()` (which is private).
- **Critical detail**: Check that `FOliveToolResult.Data` contains `compile_result` and `step_to_node_map` at the expected nesting level. The pipeline's `StageReport` puts `compile_result` directly into `ResultData`, which becomes `FOliveToolResult.Data`. So access is `PipelineResult.Data->TryGetObjectField("compile_result", ...)` -- no extra `"data"` nesting at this level.

### T2: Replace mode pre-cleanup
- **Effort**: ~1 hour
- **Files**: OliveBlueprintToolHandlers.cpp
- **Key**: Capture `Mode` into v2.0 lambda. Before `PlanExecutor.Execute()`, if mode=="replace", iterate graph nodes and remove non-entry nodes. Include `K2Node_FunctionResult.h` if not already included.
- **Depends on**: T1 (same area of code)

### T3: Add component classes to search order
- **Effort**: ~1.5 hours
- **Files**: OliveFunctionResolver.h, OliveFunctionResolver.cpp
- **Key**: Insert SCS scan between parent hierarchy and common libraries in `GetSearchOrder`. Add `ComponentClassSearch` enum value. Add SCS includes.
- **Independent**: Can run in parallel with T1/T2.

### T4: Relevance-aware BroadSearch scoring
- **Effort**: ~1.5 hours
- **Files**: OliveFunctionResolver.h, OliveFunctionResolver.cpp
- **Key**: Pass `Blueprint*` to BroadSearch. Score based on class relevance. Add threshold of 60 in Resolve's Strategy 5. Log rejected low-confidence matches.
- **Depends on**: T3 (must be done first for includes and understanding)

### T5: Tag plan results with ownership metadata
- **Effort**: ~1 hour
- **Files**: OlivePlanExecutor.h, BlueprintPlanIR.h, OlivePlanExecutor.cpp, OliveBlueprintToolHandlers.cpp
- **Key**: Add `ResolvedFunctionNames`, `ResolvedClassNames` to context. Populate from ResolvedSteps in Execute(). Copy to result. Serialize in handler.
- **Depends on**: T1 (same files)

### T6: Stale-aware loop detection
- **Effort**: ~1.5 hours
- **Files**: OliveSelfCorrectionPolicy.h, OliveSelfCorrectionPolicy.cpp
- **Key**: Extend `HasCompileFailure` with stale detection. Cross-reference compile error text against plan_class_names/plan_function_names. Skip loop detector for stale errors.
- **Depends on**: T5 (uses the metadata fields)

**Total estimated effort: ~8.5 hours**
