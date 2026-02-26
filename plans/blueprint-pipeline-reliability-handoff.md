# Blueprint Pipeline Reliability Upgrade — Implementation Handoff

## Background & Problem Statement

The Blueprint plan execution pipeline has systemic failures that cause cascading death spirals. When a plan fails and gets rolled back, self-correction tells the AI to fix nodes that no longer exist, burning all retry budget on impossible actions. Additionally, the pipeline lacks graph context — it doesn't know whether it's operating in a function graph or EventGraph, leading to preventable errors like latent calls in functions and broken function parameter references.

This upgrade addresses these issues through three phases:

1. **Phase 1: Safety Net** — Granular fallback after plan failures + retry policy changes
2. **Phase 2: Graph Context** — Thread graph type and function signature through the pipeline
3. **Phase 3: Lookup Tables** — Node factory library expansion + Float→Double aliases

The key architectural principle: **Phase 1 prevents death spirals from ANY error (including future unknowns). Phase 2 prevents known error categories from ever reaching the retry system. Phase 3 fills data gaps that cause node creation failures.**

---

## Phase 1: Safety Net (Granular Fallback + Retry Policy)

### Goal

When a plan fails 3 times with the same error, instead of dying, FORCE the AI into step-by-step node-by-node mode using existing granular tools (blueprint.add_node, blueprint.connect_pins, etc.). This is not optional — after 3 plan failures the AI must use granular tools. Remove the global `MaxCorrectionCyclesPerWorker` hard cap. The granular fallback gets a fresh `MaxRetriesPerError = 3` budget per unique error via loop detector reset.

The flow:
- **Attempts 1-3 (plan mode):** Each rollback tells AI "resubmit corrected plan, do NOT use connect_pins on deleted nodes." No mention of granular mode yet. Uses `MaxRetriesPerError = 3`.
- **After 3 same-error failures (forced switch):** Loop detector resets. Message says "plan approach failed, you MUST switch to step-by-step." The AI is told NOT to use apply_plan_json again.
- **Granular mode (fresh budget):** AI reads graph state, builds node-by-node. Gets a fresh `MaxRetriesPerError = 3` per unique error. If same error hits 3 times again → now truly dead.

### Why This Is Priority 1

This is the single highest-impact change. It prevents death spirals from ANY cause — not just the errors we've identified, but any future error we haven't seen yet. The granular tools already exist and work. We just need to route the AI to them when plans fail.

### Existing Granular Tools (already implemented, no changes needed)

These tools all exist and are registered. The AI already knows how to use them from its system prompt. No changes to these tools are required:

- `blueprint.read` / `blueprint.read_function` / `blueprint.read_event_graph` — see current graph state
- `blueprint.add_node` — create one node, returns node_id + full pin manifest
- `blueprint.connect_pins` — wire two pins
- `blueprint.disconnect_pins` — break a connection
- `blueprint.set_pin_default` — set default value on a pin
- `blueprint.set_node_property` — modify node properties
- `blueprint.remove_node` — delete a node
- `blueprint.compile` — compile the Blueprint
- `project.batch_write` — atomic multi-op with template references

### 1A: Modify Retry Policy — Remove Global Budget Cap

**File**: `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h`  
**Struct**: `FOliveRetryPolicy`

Current:
```cpp
struct OLIVEAIEDITOR_API FOliveRetryPolicy
{
    int32 MaxRetriesPerError = 3;
    int32 MaxCorrectionCyclesPerWorker = 5;
    int32 MaxWorkerFailures = 2;
    float RetryDelaySeconds = 0.0f;
};
```

Changes:
- Remove `MaxCorrectionCyclesPerWorker` field entirely, OR set it to a very high number (e.g., 20) so it never fires as the practical limiter
- Add a new field: `bool bAllowGranularFallback = true;` — feature flag to enable/disable

**File**: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`  
**Function**: `FOliveLoopDetector::IsBudgetExhausted` (line ~73044)

Current:
```cpp
bool FOliveLoopDetector::IsBudgetExhausted(const FOliveRetryPolicy& Policy) const
{
    return TotalAttempts >= Policy.MaxCorrectionCyclesPerWorker;
}
```

Change: Either remove this check entirely from the kill switch condition at line ~73269, or set it to the new high value. The per-error limit (MaxRetriesPerError = 3) already prevents infinite loops for any single error.

### 1B: Add Fallback State Tracking to Self-Correction Policy

**File**: `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h`

Add state tracking to the policy class (or to `FOliveLoopDetector`):

```cpp
// Track whether we've switched to granular fallback mode
bool bIsInGranularFallback = false;

// Store the last plan failure reason so granular mode knows what to avoid
FString LastPlanFailureReason;
```

Add a reset for these in the `Reset()` method (called per user message at line ~74456).

### 1C: Modify Self-Correction Evaluate() — Rollback-Aware + Fallback Switch

**File**: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`  
**Function**: `FOliveSelfCorrectionPolicy::Evaluate` (line ~73176)

This is the core change. The compile failure branch starts at line ~73236:

```cpp
if (HasCompileFailure(ResultJson, CompileErrors, AssetPath, bHasStaleErrors))
```

After the stale error check (line ~73258) and before the existing retry logic (line ~73260), add rollback detection:

**Step 1**: Detect whether nodes were rolled back by parsing `rolled_back_nodes` from the result JSON. The field is already set by `apply_plan_json` at line ~31117. The `HasCompileFailure` method already parses the `data` object (line ~73388) — extract `rolled_back_nodes` from the same `DataObj`:

```cpp
// Check if nodes were rolled back
int32 RolledBackNodeCount = 0;
if (DataObj.IsValid())
{
    double RolledBackDouble = 0;
    if (DataObj->TryGetNumberField(TEXT("rolled_back_nodes"), RolledBackDouble))
    {
        RolledBackNodeCount = static_cast<int32>(RolledBackDouble);
    }
}
```

**Step 2**: In the existing loop detection check (line ~73269), when `IsLooping` returns true (same error 3 times), instead of immediately setting `StopWorker`, check if we should switch to granular fallback:

Current behavior at line ~73269:
```cpp
if (LoopDetector.IsLooping(Signature, Policy) || LoopDetector.IsOscillating() || LoopDetector.IsBudgetExhausted(Policy))
{
    Decision.Action = EOliveCorrectionAction::StopWorker;
    ...
}
```

New behavior:
```cpp
if (LoopDetector.IsLooping(Signature, Policy) || LoopDetector.IsOscillating())
{
    if (!bIsInGranularFallback && Policy.bAllowGranularFallback)
    {
        // Switch to granular fallback instead of dying
        bIsInGranularFallback = true;
        LastPlanFailureReason = CompileErrors;

        // Reset the loop detector so granular attempts get fresh MaxRetriesPerError
        LoopDetector.Reset();

        Decision.Action = EOliveCorrectionAction::FeedBackErrors;
        Decision.EnrichedMessage = BuildGranularFallbackMessage(
            ToolName, CompileErrors, AssetPath, RolledBackNodeCount);
        return Decision;
    }
    else if (bIsInGranularFallback)
    {
        // Granular fallback also hit MaxRetriesPerError on same error — now truly stop
        Decision.Action = EOliveCorrectionAction::StopWorker;
        Decision.LoopReport = LoopDetector.BuildLoopReport();
        return Decision;
    }
}
```

**Step 3**: The loop detector was reset when entering granular mode, so `MaxRetriesPerError = 3` applies fresh. The AI gets 3 retries per unique error in granular mode — same rule as plan mode, just a clean slate. If the same error hits 3 times in granular mode, `IsLooping` returns true again and this time `bIsInGranularFallback` is already true, so we hit `StopWorker`.

**Step 4 (optional but recommended)**: When `bIsInGranularFallback` is true, if the AI calls `apply_plan_json` anyway, reject it immediately with a message reminding it to use granular tools. This prevents the AI from ignoring the forced switch. Check for this in the plan dedup / identical plan detection block (line ~73200 area) or in the tool handler itself.

**Step 4**: Even when NOT looping yet, if rollback happened, change the existing FeedBackErrors message. Currently at line ~73279:

```cpp
Decision.EnrichedMessage = BuildCompileErrorMessage(ToolName, CompileErrors, SignatureAttempts, Policy.MaxRetriesPerError);
```

Change to:
```cpp
if (RolledBackNodeCount > 0)
{
    Decision.EnrichedMessage = BuildRollbackAwareMessage(
        ToolName, CompileErrors, SignatureAttempts, Policy.MaxRetriesPerError, RolledBackNodeCount);
}
else
{
    Decision.EnrichedMessage = BuildCompileErrorMessage(ToolName, CompileErrors, SignatureAttempts, Policy.MaxRetriesPerError);
}
```

### 1D: New Message Builder Functions

**File**: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

Add two new message builder functions:

**BuildRollbackAwareMessage** — for plan failures where rollback happened but we still have plan retries left. This message ONLY offers plan resubmission, NOT granular tools. The granular switch is forced separately after 3 failures:

```cpp
FString FOliveSelfCorrectionPolicy::BuildRollbackAwareMessage(
    const FString& ToolName,
    const FString& Errors,
    int32 AttemptNum,
    int32 MaxAttempts,
    int32 RolledBackNodeCount) const
{
    return FString::Printf(
        TEXT("[COMPILE FAILED + ROLLBACK - Attempt %d/%d] The Blueprint failed to compile after executing '%s'. "
             "%d nodes were ROLLED BACK — the graph is restored to its pre-plan state.\n"
             "Errors:\n%s\n"
             "REQUIRED ACTION: Fix the plan and resubmit with apply_plan_json.\n"
             "Do NOT use connect_pins or reference any node IDs from the failed plan — those nodes no longer exist.\n"
             "Common fixes:\n"
             "- Latent calls (Delay, AI MoveTo, etc.) CANNOT be in function graphs — use a Custom Event in EventGraph instead\n"
             "- Function parameters are NOT class variables — if in a function graph, parameters are pins on the entry node\n"
             "- Missing variables — ensure add_variable succeeded before referencing in a plan"),
        AttemptNum, MaxAttempts, *ToolName, RolledBackNodeCount, *Errors);
}
```

**BuildGranularFallbackMessage** — for when plan retries are exhausted and we're FORCING the switch to step-by-step. This is not optional — the AI MUST use granular tools:

```cpp
FString FOliveSelfCorrectionPolicy::BuildGranularFallbackMessage(
    const FString& ToolName,
    const FString& Errors,
    const FString& AssetPath,
    int32 RolledBackNodeCount) const
{
    return FString::Printf(
        TEXT("[PLAN APPROACH FAILED — SWITCHING TO STEP-BY-STEP MODE]\n"
             "The plan_json approach failed 3 times with the same error. %d nodes were rolled back.\n"
             "Last error:\n%s\n\n"
             "You MUST now switch to granular node-by-node building:\n"
             "1. Call blueprint.read or blueprint.read_function on '%s' to see current graph state\n"
             "2. Use blueprint.add_node to create nodes ONE AT A TIME — each call returns the node_id and pin manifest\n"
             "3. Use blueprint.connect_pins to wire them using the node_ids from step 2\n"
             "4. Use blueprint.set_pin_default for any default values\n"
             "5. Call blueprint.compile to verify\n\n"
             "This is slower but gives you accurate state after each operation.\n"
             "Do NOT use apply_plan_json again for this graph — it has failed repeatedly."),
        RolledBackNodeCount, *Errors, *AssetPath);
}
```

Declare both in the header file alongside the existing `BuildCompileErrorMessage` and `BuildToolErrorMessage` declarations (line ~96811).

### 1E: Template Compile Error Propagation

**File**: `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp`

Currently `create_from_template` returns success even when sub-plans have compile errors. The AI never gets a signal that BP_Bullet's ApplyHitDamage function failed.

Find the template result assembly (around line ~1292 based on earlier analysis — search for `create_from_template` result building). After the compile step:

1. If compile fails, change the success message to prominently flag it:
   - Instead of: `"Template 'projectile' applied successfully"`
   - Use: `"Template 'projectile' applied but Blueprint FAILED TO COMPILE. N compile error(s). Review errors and fix before proceeding."`

2. Add `compile_errors` as a top-level array in the result data (not buried in nested objects) so self-correction can detect and act on them.

3. Add a `compile_result` object with the same shape as plan apply (`{success: bool, errors: [...]}`) so the existing `HasCompileFailure` detection in self-correction works for template results too.

The tool result should still be `FOliveToolResult::Success()` (the template structure was created), but the message and data make the compile failure impossible to miss.

### 1F: Also Handle Tool Failures After Rollback

The same rollback + bad messaging problem can happen via the tool failure path (line ~73286), not just the compile failure path. When `HasToolFailure` detects an error and the result JSON contains `rolled_back_nodes > 0`, apply the same rollback-aware messaging pattern. Check the tool failure branch at line ~73286 and add the same `RolledBackNodeCount` detection and message adjustment.

### Phase 1 Verification

1. **Build** — incremental compile must succeed
2. **Test plan rollback recovery** — Submit a plan that will compile-fail (e.g., Delay in function graph). After rollback, self-correction should say "resubmit corrected plan, do NOT use connect_pins"
3. **Test fallback switch** — Force the same plan failure 3 times. On the 3rd failure, self-correction should switch to granular mode message instead of dying
4. **Test granular fresh budget** — In granular mode, verify the loop detector was reset and the AI gets a fresh MaxRetriesPerError = 3 per unique error
5. **Test granular eventual stop** — If the same error hits 3 times in granular mode, the system should now truly stop
6. **Test template compile errors** — `create_from_template` with a broken function plan should surface compile errors prominently in the result
7. **Test no regression** — Normal successful plans should work exactly as before with no behavior change

---

## Phase 2: Graph Context Threading

### Goal

Pass graph type (EventGraph vs function vs macro) and function signature (parameters, return values) through the entire plan pipeline — resolver, validator, executor. This enables:

- Resolver auto-detects function parameters when `get_var` is used inside a function graph
- Validator rejects latent calls in function graphs before any nodes are created
- AI gets clear error messages with correct fix guidance (e.g., "use a Custom Event instead")

### 2A: Add Graph Context Struct

**File**: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`

Create a context struct that will flow through the pipeline:

```cpp
/**
 * Captured context about the target graph for plan resolution and validation.
 * Built once before plan execution, consumed by resolver, validator, and executor.
 */
struct FOliveGraphContext
{
    /** Name of the graph (e.g., "EventGraph", "Fire", "ApplyHitDamage") */
    FString GraphName;

    /** True if this is a function graph (not EventGraph, not macro) */
    bool bIsFunctionGraph = false;

    /** True if this is a macro graph */
    bool bIsMacroGraph = false;

    /** Function input parameters — only populated for function graphs */
    TArray<FString> InputParamNames;

    /** Function output/return parameters — only populated for function graphs */
    TArray<FString> OutputParamNames;

    /** The actual UEdGraph pointer (for direct pin inspection if needed) */
    UEdGraph* Graph = nullptr;
};
```

Add a static builder:

```cpp
static FOliveGraphContext BuildFromBlueprint(UBlueprint* Blueprint, const FString& GraphName);
```

Implement in the .cpp file. The builder should:
1. Search `Blueprint->UbergraphPages` for EventGraph matches
2. Search `Blueprint->FunctionGraphs` for function graph matches
3. Search `Blueprint->MacroGraphs` for macro matches
4. If function graph found, scan for `UK2Node_FunctionEntry` and read its `UserDefinedPins` for input param names
5. Scan for `UK2Node_FunctionResult` and read its `UserDefinedPins` for output param names

Include `K2Node_FunctionEntry.h` and `K2Node_FunctionResult.h`.

### 2B: Add bIsLatent to Resolved Steps

**File**: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`  
**Struct**: `FOliveResolvedStep`

Add after the existing `bIsPure` field:

```cpp
bool bIsLatent = false;
```

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

Set `bIsLatent` in two places:

1. In `ResolveCallOp()` — after setting `bIsPure`, add:
```cpp
Out.bIsLatent = Match.Function->HasMetaData(TEXT("Latent"));
```

2. In `ResolveStep()` for non-call ops, specifically the Delay op:
```cpp
OutResolved.bIsLatent = (Op == OlivePlanOps::Delay);
```

### 2C: Thread GraphContext Through Resolver

**File**: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`

Update `Resolve()` signature to accept the context:

```cpp
static FOlivePlanResolveResult Resolve(
    FOliveIRBlueprintPlan& Plan,
    UBlueprint* Blueprint,
    const FOliveGraphContext& GraphContext = FOliveGraphContext());
```

Update private methods `ResolveStep()`, `ResolveGetVarOp()`, `ResolveSetVarOp()` to also accept `const FOliveGraphContext&`.

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

**Modify ResolveGetVarOp()**: At the TOP, before the existing variable lookup logic, add function parameter detection:

```cpp
// If we're in a function graph, check if the target matches a function input parameter
if (GraphContext.bIsFunctionGraph)
{
    for (const FString& ParamName : GraphContext.InputParamNames)
    {
        if (ParamName.Equals(Step.Target, ESearchCase::IgnoreCase))
        {
            // This is a function input parameter, not a class variable
            Out.NodeType = OliveNodeTypes::FunctionInput; // NEW constant, see 2E
            Out.Properties.Add(TEXT("param_name"), ParamName);
            Out.bIsPure = true;
            return true;
        }
    }
}
```

**Modify ResolveSetVarOp()**: Same pattern for function output parameters:

```cpp
if (GraphContext.bIsFunctionGraph)
{
    for (const FString& ParamName : GraphContext.OutputParamNames)
    {
        if (ParamName.Equals(Step.Target, ESearchCase::IgnoreCase))
        {
            Out.NodeType = OliveNodeTypes::FunctionOutput; // NEW constant, see 2E
            Out.Properties.Add(TEXT("param_name"), ParamName);
            Out.bIsPure = false; // FunctionResult has exec input
            return true;
        }
    }
}
```

Pass `GraphContext` through `Resolve()` → `ResolveStep()` → `ResolveGetVarOp()` / `ResolveSetVarOp()`.

### 2D: Add Latent-in-Function Validation

**File**: `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanValidator.h`

Update `Validate()` to accept graph context:

```cpp
static FOlivePlanValidationResult Validate(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    UBlueprint* Blueprint,
    const FOliveGraphContext& GraphContext = FOliveGraphContext());
```

Add private method declaration:

```cpp
static void CheckLatentInFunctionGraph(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    const FOliveGraphContext& GraphContext,
    FOlivePlanValidationResult& Result);
```

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp`

Add the new check to the end of `Validate()`, alongside existing checks (`CheckComponentFunctionTargets`, `CheckExecWiringConflicts`):

```cpp
CheckLatentInFunctionGraph(Plan, ResolvedSteps, GraphContext, Result);
```

Implement:

```cpp
void FOlivePlanValidator::CheckLatentInFunctionGraph(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    const FOliveGraphContext& GraphContext,
    FOlivePlanValidationResult& Result)
{
    if (!GraphContext.bIsFunctionGraph)
    {
        return; // Only relevant for function graphs
    }

    for (int32 i = 0; i < ResolvedSteps.Num(); ++i)
    {
        if (ResolvedSteps[i].bIsLatent)
        {
            const FString& StepId = Plan.Steps[i].StepId;

            // Determine the latent function name for the error message
            FString LatentName = TEXT("latent action");
            if (Plan.Steps[i].Op == EOliveIRPlanOp::Delay)
            {
                LatentName = TEXT("Delay");
            }
            else if (ResolvedSteps[i].Properties.Contains(TEXT("function_name")))
            {
                LatentName = ResolvedSteps[i].Properties[TEXT("function_name")];
            }

            Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                TEXT("LATENT_IN_FUNCTION"),
                StepId,
                FString::Printf(TEXT("/steps/%d"), i),
                FString::Printf(
                    TEXT("Step '%s' uses latent function '%s' which cannot be used in function graph '%s'. "
                         "Latent calls (Delay, AI MoveTo, etc.) are only allowed in EventGraph. "
                         "If you need latent behavior, use a Custom Event in EventGraph instead of a function."),
                    *StepId, *LatentName, *GraphContext.GraphName),
                TEXT("Move this logic to a custom_event in EventGraph, or use SetTimerByFunction as a non-latent alternative.")));
        }
    }
}
```

### 2E: New NodeType Constants for Function Params

**File**: `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h`

Add to `OliveNodeTypes` namespace:

```cpp
const FString FunctionInput = TEXT("FunctionInput");
const FString FunctionOutput = TEXT("FunctionOutput");
```

### 2F: Executor Handles FunctionInput / FunctionOutput

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

In `PhaseCreateNodes()`, the executor creates nodes for each resolved step. For `FunctionInput` and `FunctionOutput`, we don't create new nodes — we register the existing `FunctionEntry` and `FunctionResult` nodes as virtual steps.

Find the node creation block in PhaseCreateNodes (around line ~350, after the existing event reuse path). Add before the normal node creation block:

```cpp
// Virtual step: FunctionInput — maps to an existing FunctionEntry node's output pin
if (ResolvedStep.NodeType == OliveNodeTypes::FunctionInput)
{
    // Find the FunctionEntry node in this graph
    UK2Node_FunctionEntry* EntryNode = nullptr;
    for (UEdGraphNode* Node : Context.Graph->Nodes)
    {
        EntryNode = Cast<UK2Node_FunctionEntry>(Node);
        if (EntryNode) break;
    }

    if (!EntryNode)
    {
        // Error: no function entry node found
        Context.Errors.Add(...);
        return false;
    }

    // Register this step as pointing to the FunctionEntry node
    const FString NodeId = FOliveGraphWriter::Get().GetOrAssignNodeId(Context.AssetPath, EntryNode);
    Context.StepToNodePtr.Add(StepId, EntryNode);
    Context.StepToNodeMap.Add(StepId, NodeId);

    // Build pin manifest so @step_id.auto can find the parameter output pin
    Context.StepManifests.Add(StepId, FOlivePinManifest::Build(EntryNode));

    // Mark as reused so rollback doesn't delete it
    Context.ReusedStepIds.Add(StepId);

    Context.CreatedNodeCount++; // Count as "created" for reporting
    continue; // Skip normal node creation
}

// Virtual step: FunctionOutput — maps to an existing FunctionResult node's input pin
if (ResolvedStep.NodeType == OliveNodeTypes::FunctionOutput)
{
    UK2Node_FunctionResult* ResultNode = nullptr;
    for (UEdGraphNode* Node : Context.Graph->Nodes)
    {
        ResultNode = Cast<UK2Node_FunctionResult>(Node);
        if (ResultNode) break;
    }

    if (!ResultNode)
    {
        Context.Errors.Add(...);
        return false;
    }

    const FString NodeId = FOliveGraphWriter::Get().GetOrAssignNodeId(Context.AssetPath, ResultNode);
    Context.StepToNodePtr.Add(StepId, ResultNode);
    Context.StepToNodeMap.Add(StepId, NodeId);
    Context.StepManifests.Add(StepId, FOlivePinManifest::Build(ResultNode));
    Context.ReusedStepIds.Add(StepId);
    Context.CreatedNodeCount++;
    continue;
}
```

**Critical**: Adding to `ReusedStepIds` ensures rollback won't delete FunctionEntry/FunctionResult nodes — they're structural, not plan-created.

Include `K2Node_FunctionEntry.h` and `K2Node_FunctionResult.h`.

### 2G: Update All Callers — Pass GraphContext

**File**: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

Both the preview (line ~6280) and apply (line ~6613) paths call `Resolve()` and `Validate()`. Update both:

```cpp
// Build graph context
FOliveGraphContext GraphContext = FOliveGraphContext::BuildFromBlueprint(Blueprint, GraphName);

// Pass to resolver
FOlivePlanResolveResult ResolveResult = FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint, GraphContext);

// Pass to validator
FOlivePlanValidationResult ValidationResult = FOlivePlanValidator::Validate(
    Plan, ResolveResult.ResolvedSteps, Blueprint, GraphContext);
```

**File**: `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp`

Templates also call `Resolve()` and `Validate()` for each sub-plan. Find the function graph plan execution (around line ~1141) and event graph plan execution (around line ~1227). Pass appropriate graph context:

- For function body plans: `FOliveGraphContext::BuildFromBlueprint(Blueprint, FunctionName)`
- For event graph plans: `FOliveGraphContext::BuildFromBlueprint(Blueprint, TEXT("EventGraph"))`

### Phase 2 Verification

1. **Build** — incremental compile must succeed
2. **Test function param detection** — Plan with `get_var("HitActor")` in a function graph where HitActor is a function input parameter should resolve as `FunctionInput` and wire to FunctionEntry output pin
3. **Test function return detection** — Plan with `set_var("DamageApplied")` where DamageApplied is a function output should resolve as `FunctionOutput` and wire to FunctionResult input pin
4. **Test latent-in-function rejection** — Plan with `Delay` in a function graph should be rejected at validation (Phase 0) with message mentioning Custom Events, before any nodes are created
5. **Test EventGraph still works** — Plans targeting EventGraph should work exactly as before, including Delay
6. **Test template function plans** — `create_from_template` for projectile template's ApplyHitDamage function should now resolve HitActor/HitResult correctly as function params
7. **Test rollback doesn't delete structural nodes** — If a function plan fails and rolls back, FunctionEntry and FunctionResult nodes must survive

---

## Phase 3: Lookup Table Fixes

### Goal

Fill data gaps in the function resolver alias map and node factory library search that cause preventable node creation failures.

### 3A: Float→Double Aliases

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp`  
**Function**: `GetAliasMap()` (line ~36661)

Add a new section after the Math Operations section (after line ~36777, the `Power` entry):

```cpp
// ================================================================
// UE 5.5+ Float→Double Renames
// ================================================================
// In UE 5.5, many float math functions were renamed to use Double.
// These aliases ensure plans written with old names still resolve.
Map.Add(TEXT("Add_FloatFloat"), TEXT("Add_DoubleDouble"));
Map.Add(TEXT("Subtract_FloatFloat"), TEXT("Subtract_DoubleDouble"));
Map.Add(TEXT("Multiply_FloatFloat"), TEXT("Multiply_DoubleDouble"));
Map.Add(TEXT("Divide_FloatFloat"), TEXT("Divide_DoubleDouble"));
Map.Add(TEXT("Less_FloatFloat"), TEXT("Less_DoubleDouble"));
Map.Add(TEXT("Greater_FloatFloat"), TEXT("Greater_DoubleDouble"));
Map.Add(TEXT("LessEqual_FloatFloat"), TEXT("LessEqual_DoubleDouble"));
Map.Add(TEXT("GreaterEqual_FloatFloat"), TEXT("GreaterEqual_DoubleDouble"));
Map.Add(TEXT("EqualEqual_FloatFloat"), TEXT("EqualEqual_DoubleDouble"));
Map.Add(TEXT("NotEqual_FloatFloat"), TEXT("NotEqual_DoubleDouble"));
```

Note: `MultiplyMultiply_FloatFloat` (Power), `Percent_FloatFloat`, `NearlyEqual_FloatFloat`, `InRange_FloatFloat` kept their names in UE 5.5 — no aliases needed.

### 3B: Node Factory Library Expansion

**File**: `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`  
**Function**: `FindFunction()` (line ~52518)

Current (only 3 classes):
```cpp
TArray<UClass*> LibraryClasses = {
    UKismetSystemLibrary::StaticClass(),
    UObject::StaticClass(),
    AActor::StaticClass(),
};
```

Expand to match what the resolver already searches (line ~36210):

```cpp
TArray<UClass*> LibraryClasses = {
    UKismetSystemLibrary::StaticClass(),
    UKismetMathLibrary::StaticClass(),       // NEW - math ops
    UKismetStringLibrary::StaticClass(),     // NEW - string ops
    UKismetArrayLibrary::StaticClass(),      // NEW - array ops
    UGameplayStatics::StaticClass(),         // NEW - gameplay utilities
    UObject::StaticClass(),
    AActor::StaticClass(),
    USceneComponent::StaticClass(),          // NEW - component functions
    UPrimitiveComponent::StaticClass(),      // NEW - physics functions
    APawn::StaticClass(),                    // NEW - pawn functions
    ACharacter::StaticClass(),               // NEW - character functions
};
```

Add missing `#include` directives at the top of the file:
```cpp
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
```

Update the warning log at line ~52536 to list all classes so debugging is accurate:
```cpp
UE_LOG(LogOliveNodeFactory, Warning,
    TEXT("FindFunction('%s', class='%s'): FAILED — searched specified class + Blueprint GeneratedClass + library classes "
         "[KismetSystemLibrary, KismetMathLibrary, KismetStringLibrary, KismetArrayLibrary, GameplayStatics, "
         "Object, Actor, SceneComponent, PrimitiveComponent, Pawn, Character]"),
    *FunctionName, *ClassName);
```

### Phase 3 Verification

1. **Build** — incremental compile must succeed
2. **Test Float→Double** — Plan using `Add_FloatFloat` should alias-resolve to `Add_DoubleDouble` and be found by node factory
3. **Test math functions** — Plan calling `Add_DoubleDouble` directly should be found in `UKismetMathLibrary`
4. **Test gameplay statics** — Plan calling `ApplyDamage` should be found in `UGameplayStatics` by node factory (currently found by resolver but fails at factory)
5. **Test string library** — Functions like `Concat_StrStr` should resolve and create nodes successfully

---

## Implementation Order

**Phase 1 first.** This is the safety net that prevents death spirals immediately. It touches only the self-correction policy and template result formatting — low risk, high impact.

**Phase 2 second.** This threads graph context through the pipeline. It touches resolver, validator, executor, tool handlers, and template system — more files but well-defined changes. Depends on Phase 1 being stable so that any issues during development are caught gracefully instead of death spiraling.

**Phase 3 third.** These are isolated lookup table additions. Zero risk, can be done any time. But they're most useful after Phase 2 since the resolver and factory need to agree on what functions exist.

---

## Files Modified Summary

### Phase 1 (6 files)
| File | Change |
|------|--------|
| `OliveSelfCorrectionPolicy.h` | Add `bIsInGranularFallback` state field, new message builder declarations, `bAllowGranularFallback` on retry policy |
| `OliveSelfCorrectionPolicy.cpp` | Rollback detection, fallback switch logic, loop detector reset on switch, new message builders |
| `OliveRetryPolicy` (in .h) | Remove/raise MaxCorrectionCyclesPerWorker |
| `OliveLoopDetector` (in .cpp) | Update IsBudgetExhausted or remove from kill switch |
| `OliveTemplateSystem.cpp` | Surface compile errors prominently in template results |
| `ConversationManager` (wherever LoopDetector.Reset lives) | Reset `bIsInGranularFallback` per user message |

### Phase 2 (8 files)
| File | Change |
|------|--------|
| `OliveBlueprintPlanResolver.h` | FOliveGraphContext struct, bIsLatent field, updated signatures |
| `OliveBlueprintPlanResolver.cpp` | BuildFromBlueprint, function param detection, latent flag setting |
| `OlivePlanValidator.h` | Updated Validate() signature, CheckLatentInFunctionGraph declaration |
| `OlivePlanValidator.cpp` | CheckLatentInFunctionGraph implementation |
| `OliveNodeFactory.h` | FunctionInput/FunctionOutput NodeType constants |
| `OlivePlanExecutor.cpp` | Virtual step registration for FunctionInput/FunctionOutput |
| `OliveBlueprintToolHandlers.cpp` | Build and pass GraphContext to Resolve() and Validate() (4 call sites) |
| `OliveTemplateSystem.cpp` | Pass GraphContext to Resolve() for function body plans |

### Phase 3 (2 files)
| File | Change |
|------|--------|
| `OliveFunctionResolver.cpp` | 10 Float→Double alias additions |
| `OliveNodeFactory.cpp` | 8 library class additions + includes |

---

## Key Design Decisions

1. **Why not remove rollback entirely?** Rollback is correct behavior — leaving broken nodes in the graph creates worse problems. The issue is only the *messaging* after rollback, not rollback itself.

2. **Why reset error signatures when switching to granular?** Because plan errors ("compile failed, nodes rolled back") and granular errors ("pin not found", "node type unknown") are fundamentally different. Old signatures would pollute the loop detector and cause premature stops.

3. **Why keep get_var/set_var instead of adding new plan ops?** The AI doesn't need to know the difference between a function parameter and a class variable. That's an implementation detail. Making the resolver smart enough to figure it out based on context keeps the plan vocabulary simple and reduces the chance of AI mistakes.

4. **Why FunctionInput/FunctionOutput as virtual steps instead of new nodes?** Function parameters already exist as pins on FunctionEntry/FunctionResult. Creating new getter nodes would be redundant and could confuse the graph. Virtual steps point to existing structural nodes, which is both simpler and correct.

5. **Why reset the loop detector when switching to granular?** Two reasons. First, plan errors ("compile failed, nodes rolled back") and granular errors ("pin not found", "node type unknown") are fundamentally different — old signatures would pollute the new strategy. Second, the AI needs a fair shot at the new approach with a fresh `MaxRetriesPerError = 3` budget per unique error, same rule applied consistently in both modes.

6. **Why is the granular switch forced, not optional?** If we offer "resubmit plan OR switch to granular," the AI might keep choosing to resubmit plans and never actually switch. After 3 failures with the same error, the plan approach is demonstrably not working. The switch must be mandatory — the message explicitly says "Do NOT use apply_plan_json again" and "You MUST switch to step-by-step." During attempts 1-3, the AI only sees "resubmit corrected plan" — granular mode isn't mentioned until the forced switch.
