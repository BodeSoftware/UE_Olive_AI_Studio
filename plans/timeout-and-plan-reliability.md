# Timeout and Plan Reliability Fixes

**Priority order**: C > B > A > D (per log analysis)
**Root cause**: Plan JSON wiring failures cascade into manual fallback, which is 10-100x slower, which hits the 300s hard timeout while still making progress.

---

## Issue C: Partial Success Should Not Trigger Rollback (HIGHEST PRIORITY)

### Root Cause (Verified)

The cascade works as follows:

1. `FOlivePlanExecutor::Execute()` creates all 13 nodes, 12/14 wires succeed, 2 fail.
2. `AssembleResult()` sets `bSuccess=true` (all nodes created), `bPartial=true` (some wiring failed).
3. The executor lambda in the tool handler (line 6983) returns `FOliveWriteResult::Success(ResultData)` with `status:"partial_success"`.
4. The write pipeline commits the transaction in Stage 4 (line 193). Nodes persist.
5. Stage 5 (Verify) compiles the Blueprint. The 2 broken wires cause compile errors.
6. **Stage 6 (Report) at line 596-598**: `if (CR.HasErrors()) { FinalResult.bSuccess = false; }` -- this flips the pipeline result to failure.
7. Back in the tool handler, `PipelineResult.bSuccess` is now FALSE.
8. **Post-pipeline rollback at line 7291**: `if (bIsV2Plan && !PipelineResult.bSuccess && PipelineResult.ResultData.IsValid())` -- triggers rollback.
9. `RollbackPlanNodes()` removes ALL 13 nodes (including the 12 with correct wiring).
10. The AI sees "rolled back" and resorts to individual `add_node`/`connect_pins` calls (10-100x slower).

**The rollback was designed for total Phase 1 failures** (node creation aborted). It should NEVER trigger on partial success where most wiring succeeded.

### Task C1: Guard Post-Pipeline Rollback Against Partial Success

**File**: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

**Exact location**: Line 7291, the `if` condition for the post-pipeline rollback block.

**Current code** (line 7291):
```cpp
if (bIsV2Plan && !PipelineResult.bSuccess && PipelineResult.ResultData.IsValid())
```

**Change to**:
```cpp
// Check if this was a partial success (all nodes created, some wiring failed).
// Partial success should NOT be rolled back -- the nodes and successful wires
// persist, and the AI can fix the remaining failures with connect_pins.
// Rollback is only for TOTAL failure (Phase 1 node creation aborted).
bool bIsPartialSuccess = false;
if (PipelineResult.ResultData.IsValid())
{
    FString Status;
    if (PipelineResult.ResultData->TryGetStringField(TEXT("status"), Status)
        && Status == TEXT("partial_success"))
    {
        bIsPartialSuccess = true;
    }
}

if (bIsV2Plan && !PipelineResult.bSuccess && !bIsPartialSuccess && PipelineResult.ResultData.IsValid())
```

**Why `status` field is reliable**: The executor lambda sets `ResultData->SetStringField(TEXT("status"), TEXT("partial_success"))` at line 6995. This field survives through the pipeline because `StageVerify` copies `ExecuteResult` to `VerifyResult` at line 481 (`FOliveWriteResult VerifyResult = ExecuteResult;`), and `StageReport` copies `VerifyResult` to `FinalResult` at line 568. The `ResultData` pointer is shared, so the `status` field persists.

**Edge cases**:
- Partial success + compile errors: nodes persist, AI gets `wiring_errors` + `compile_result` + `self_correction_hint` and can fix with granular `connect_pins`/`set_pin_default`
- Total failure (Phase 1 aborted): `status` is NOT `"partial_success"` (executor returns `ExecutionError`), rollback still triggers correctly
- Full success + compile errors (rare -- e.g., pre-existing errors): `status` is not set to `"partial_success"`, rollback triggers -- this is correct behavior (the plan's nodes caused compile errors)

### Task C2: Verify ResultData Survives Pipeline Stages

**File**: `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`

**Verification task** (read-only, no code change expected):
1. Confirm `StageVerify()` at line 481 copies `ExecuteResult` (including `ResultData`) into `VerifyResult`
2. Confirm `StageReport()` at line 568 copies `VerifyResult` into `FinalResult`
3. Confirm compile result is ADDED to `ResultData` at line 582 (`SetObjectField("compile_result", ...)`)
4. Confirm this means the final `PipelineResult.ResultData` contains BOTH the executor's fields (step_to_node_map, wiring_errors, status, etc.) AND the compile result

If any step replaces `ResultData` instead of extending it, that is a bug that needs fixing. From the code I read, it does copy-and-extend, which is correct.

---

## Issue B: Fix Two Plan JSON Bugs (HIGH PRIORITY)

### Bug B1: Function Graph Entry Detection

#### Problem

When the AI sends `op: "event"` with `target: "Fire"` and the target graph IS a function graph named "Fire", the resolver routes through `ResolveEventOp` which tries to map it to a Blueprint event name (line 1316). Since "Fire" is not in the `EventNameMap`, it passes through as-is with `event_name: "Fire"`. The executor then enters the event reuse check at line 331, calls `FindExistingEventNode("Fire", bIsCustomEvent=false)`, which looks for an event node -- but there is no event node, there is a FunctionEntry node. Node creation fails.

#### Fix Design

**Location**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`, in `ResolveStep()` (line 643)

**Strategy**: When `op == "event"` and the graph context says `bIsFunctionGraph == true`, remap the op to `FunctionInput` instead of `Event`. The AI is saying "I want the entry point of this graph" -- in a function graph, that is the FunctionEntry node.

**Exact change in `ResolveStep()` at the `Event` dispatch (line 674)**:

```cpp
else if (Op == OlivePlanOps::Event)
{
    // In function graphs, "event" targeting the graph name (or generic names like "entry")
    // maps to the FunctionEntry node, not a Blueprint event node.
    if (GraphContext.bIsFunctionGraph)
    {
        // Check if the target matches the function name or is a generic entry alias
        const bool bTargetsFunction = Step.Target.Equals(GraphContext.GraphName, ESearchCase::IgnoreCase)
            || Step.Target.Equals(TEXT("entry"), ESearchCase::IgnoreCase)
            || Step.Target.Equals(TEXT("Entry"), ESearchCase::IgnoreCase)
            || Step.Target.IsEmpty();

        if (bTargetsFunction)
        {
            bResult = ResolveSimpleOp(Step, OliveNodeTypes::FunctionInput, OutResolved);
            if (bResult)
            {
                OutResolved.Properties.Add(TEXT("function_name"), GraphContext.GraphName);

                FOliveResolverNote Note;
                Note.Field = TEXT("op");
                Note.OriginalValue = FString::Printf(TEXT("event:%s"), *Step.Target);
                Note.ResolvedValue = TEXT("FunctionInput");
                Note.Reason = FString::Printf(
                    TEXT("Graph '%s' is a function graph. Mapped event op to FunctionEntry node."),
                    *GraphContext.GraphName);
                OutResolved.ResolverNotes.Add(MoveTemp(Note));
            }
        }
        else
        {
            // Target doesn't match the function name -- fall through to normal event resolution
            // (could be a component delegate event in a function graph, though unusual)
            bResult = ResolveEventOp(Step, Blueprint, StepIndex, OutResolved, OutErrors);
        }
    }
    else
    {
        bResult = ResolveEventOp(Step, Blueprint, StepIndex, OutResolved, OutErrors);
    }
}
```

**Also handle `op: "entry"` aliases**: The AI might use `"entry"` as an op. Currently the plan vocabulary does not include "entry" as a valid op (OlivePlanOps only has `Event`). Two options:

1. (Preferred) Handle it in `ResolveStep` before the op dispatch -- if `Op == "entry"`, silently remap to `"event"`:
   ```cpp
   FString EffectiveOp = Op;
   if (EffectiveOp == TEXT("entry"))
   {
       EffectiveOp = OlivePlanOps::Event;
       // Log resolver note for transparency
   }
   ```

2. (Alternative) Add `"entry"` to `OlivePlanOps` vocabulary. This is a bigger change that touches the IR module.

Option 1 is preferred because it is a one-line remap in the resolver with no schema changes.

**Edge cases**:
- Function graph with no FunctionEntry node: This is handled by existing code in PhaseCreateNodes (line 391-406) which returns a clean error
- Function graph where the AI targets a different event (e.g., a component event): Falls through to normal ResolveEventOp
- Empty target on event op in function graph: Mapped to FunctionInput (reasonable default)

### Bug B2: ExpandComponentRefs Doesn't Handle Blueprint Variable Refs

#### Problem

The executor logs `Data wire FAILED: Invalid @ref format: '@MuzzlePoint'. Expected '@stepId.pinHint'`. This means a bare `@MuzzlePoint` reference survived through the resolver without being rewritten to `@_synth_getvar_xxx.auto`.

`ExpandComponentRefs` currently only checks `SCSComponentNames` (from `SimpleConstructionScript` nodes). But in a gun Blueprint, `MuzzlePoint` is typically a Blueprint variable (added via `blueprint.add_variable` with type `USceneComponent`), NOT an SCS component (added via `blueprint.add_component`). SCS only contains components in the component tree -- NOT arbitrary variables of component type.

Since `SCSComponentNames` does not contain `MuzzlePoint`, the bare `@MuzzlePoint` passes through unmodified and hits `ParseDataRef` in the executor, which requires a dot in the reference.

#### Actual Fix for B2

The fix is to make ExpandComponentRefs ALSO check Blueprint variables (not just SCS components) for bare @refs. If `@MuzzlePoint` matches a Blueprint variable name, synthesize a get_var step.

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`, `ExpandComponentRefs()` method

**Change**: After building `SCSComponentNames`, also build a `BlueprintVariableNames` set from `Blueprint->NewVariables`. Then, in the bare-ref (no dot) branch, check BOTH sets.

```cpp
// Build set of Blueprint variable names (NewVariables, not SCS)
TSet<FString> BlueprintVariableNames;
for (const FBPVariableDescription& Var : Blueprint->NewVariables)
{
    BlueprintVariableNames.Add(Var.VarName.ToString());
}
```

Then at line 542 (bare ref, no dot), after checking `SCSComponentNames`:

```cpp
// Check Blueprint variables (for bare @VarName refs like @MuzzlePoint)
if (BlueprintVariableNames.Contains(RefBody))
{
    FString* ExistingSynthId = SynthesizedComponentSteps.Find(RefBody);
    if (!ExistingSynthId)
    {
        FString SynthStepId = FString::Printf(TEXT("_synth_getvar_%s"), *RefBody.ToLower());

        FOliveIRBlueprintPlanStep SynthStep;
        SynthStep.StepId = SynthStepId;
        SynthStep.Op = OlivePlanOps::GetVar;
        SynthStep.Target = RefBody;

        Inserts.Add({ MoveTemp(SynthStep), i });
        SynthesizedComponentSteps.Add(RefBody, SynthStepId);
        ExistingStepIds.Add(SynthStepId);

        FOliveResolverNote Note;
        Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
        Note.OriginalValue = Value;
        Note.ResolvedValue = FString::Printf(TEXT("Synthesized get_var step '%s' for variable '%s'"), *SynthStepId, *RefBody);
        Note.Reason = TEXT("Bare @ref with no dot referenced a Blueprint variable name. Synthesized a get_var step and used .auto pin matching.");
        OutNotes.Add(MoveTemp(Note));
    }

    FString SynthId = SynthesizedComponentSteps[RefBody];
    RewrittenInputs.Add(PinName, FString::Printf(TEXT("@%s.auto"), *SynthId));
    bExpanded = true;
    continue;
}
```

**Also apply same fix for dotted refs** (e.g., `@MuzzlePoint.WorldLocation`): At the dotted branch (line 451-536), after checking `FunctionInputParams` and `SCSComponentNames`, add a `BlueprintVariableNames` check.

**Edge cases**:
- Variable name collides with step ID: Checked first at line 458 (`ExistingStepIds.Contains(RefStepId)`), step ID takes priority
- Variable name collides with function param: Function param is checked first, takes priority
- Variable name collides with SCS component: SCS component is checked first, takes priority (they should be identical anyway since SCS components are also variables)

### Bug B3: No Integer-to-Boolean Auto-Conversion for Branch Conditions

#### Problem

The AI wired `@get_ammo.auto` (Integer output) to `check_ammo.Condition` (Boolean input). The `@step.auto` matching looks for type-compatible outputs. `FindTypeCompatibleOutput` checks `IRTypeCategory` match. Integer is `EOliveIRTypeCategory::Integer`, Boolean is `EOliveIRTypeCategory::Boolean` -- they don't match. No auto-conversion node is inserted because the PIN connection attempt never happens (the source pin is not found by type match, so the wire fails before reaching `FOlivePinConnector::Connect`).

#### Fix Design

**Strategy**: Handle this at the resolver level, not the executor. When the resolver sees a `branch` step with an input that references a non-boolean source, synthesize a comparison step. This is analogous to how `ExpandPlanInputs` synthesizes `MakeTransform` for `SpawnActor`.

**New pre-process pass**: `ExpandBranchConditions` in the resolver.

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**Header**: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`

**Logic**:
1. Iterate all steps. For `branch` steps, check the `Condition` input.
2. If the `Condition` input is an `@ref`, look up the source step to determine if it resolves to a non-boolean output.
3. If the source step is `get_var` and the variable is Integer/Float, synthesize a `> 0` comparison step.
4. This is complex to do at resolve time because we don't have pin types yet (those come after node creation).

**Simpler alternative**: Handle it in the executor's PhaseWireData, at the type-mismatch point. When `FindTypeCompatibleOutput` fails because the source has Integer and target wants Boolean, synthesize a comparison node inline.

**Even simpler alternative (RECOMMENDED)**: In `FindTypeCompatibleOutput`, when looking for a Boolean match and finding only Integer/Float/Double, RELAX the match. Integers ARE implicitly boolean in UE4 Blueprints -- any `> 0` interpretation is the AI's intent. BUT: UE Blueprints do NOT auto-convert Integer to Boolean. The Branch node's Condition pin requires a Boolean. So we need an explicit conversion.

**RECOMMENDED APPROACH**: Add an Integer-to-Boolean special case in the resolver's `ExpandPlanInputs` or a new `ExpandBranchConditions` pass. When a `branch` step's `Condition` input is `@ref` pointing to a step whose op is `get_var` (and not a boolean variable), synthesize a `call` step for `Greater > 0` comparison.

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

**New static method**:
```cpp
static bool ExpandBranchConditions(
    FOliveIRBlueprintPlan& Plan,
    UBlueprint* Blueprint,
    TArray<FOliveResolverNote>& OutNotes);
```

**Call site**: In `Resolve()` at line 226, after `ExpandPlanInputs`:
```cpp
// Pass 3: Expand branch conditions with non-boolean @refs to > 0 comparisons
ExpandBranchConditions(MutablePlan, Blueprint, ExpansionNotes);
```

**Implementation**:
```cpp
bool FOliveBlueprintPlanResolver::ExpandBranchConditions(
    FOliveIRBlueprintPlan& Plan,
    UBlueprint* Blueprint,
    TArray<FOliveResolverNote>& OutNotes)
{
    // Build step lookup for source step analysis
    TMap<FString, const FOliveIRBlueprintPlanStep*> StepLookup;
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        StepLookup.Add(Step.StepId, &Step);
    }

    bool bExpanded = false;

    for (int32 i = 0; i < Plan.Steps.Num(); ++i)
    {
        FOliveIRBlueprintPlanStep& Step = Plan.Steps[i];

        if (Step.Op != OlivePlanOps::Branch)
        {
            continue;
        }

        // Check the Condition input
        FString* ConditionValue = Step.Inputs.Find(TEXT("Condition"));
        if (!ConditionValue || !ConditionValue->StartsWith(TEXT("@")))
        {
            continue;
        }

        // Parse the @ref
        FString RefBody = ConditionValue->Mid(1);
        int32 DotIdx;
        if (!RefBody.FindChar(TEXT('.'), DotIdx))
        {
            continue; // Bare ref, can't analyze source
        }

        FString SourceStepId = RefBody.Left(DotIdx);
        const FOliveIRBlueprintPlanStep** SourceStepPtr = StepLookup.Find(SourceStepId);
        if (!SourceStepPtr)
        {
            continue; // Source step not found, will fail at wiring time
        }

        const FOliveIRBlueprintPlanStep& SourceStep = **SourceStepPtr;

        // Check if the source step produces a non-boolean output
        // For get_var: check the variable type on the Blueprint
        bool bNeedsComparison = false;

        if (SourceStep.Op == OlivePlanOps::GetVar)
        {
            // Look up variable type
            for (const FBPVariableDescription& Var : Blueprint->NewVariables)
            {
                if (Var.VarName.ToString() == SourceStep.Target)
                {
                    // Check if the variable is NOT boolean
                    const FString Category = Var.VarType.PinCategory.ToString();
                    if (Category != TEXT("bool"))
                    {
                        bNeedsComparison = true;
                    }
                    break;
                }
            }
        }

        if (!bNeedsComparison)
        {
            continue;
        }

        // Synthesize a > 0 comparison step
        FString SynthStepId = FString::Printf(TEXT("_synth_cmp_%s"), *Step.StepId);

        FOliveIRBlueprintPlanStep CompareStep;
        CompareStep.StepId = SynthStepId;
        CompareStep.Op = OlivePlanOps::Call;
        CompareStep.Target = TEXT("Greater_IntInt"); // UKismetMathLibrary::Greater_IntInt
        CompareStep.Inputs.Add(TEXT("A"), *ConditionValue); // Forward the original @ref
        CompareStep.Inputs.Add(TEXT("B"), TEXT("0"));

        // Rewrite the branch's Condition to point at the comparison result
        *ConditionValue = FString::Printf(TEXT("@%s.auto"), *SynthStepId);

        // Insert before the branch step
        Plan.Steps.Insert(CompareStep, i);
        StepLookup.Add(SynthStepId, &Plan.Steps[i]); // Update lookup
        ++i; // Skip the inserted step

        bExpanded = true;

        FOliveResolverNote Note;
        Note.Field = FString::Printf(TEXT("step '%s' inputs.Condition"), *Step.StepId);
        Note.OriginalValue = FString::Printf(TEXT("@%s (non-boolean)"), *SourceStepId);
        Note.ResolvedValue = FString::Printf(TEXT("Synthesized > 0 comparison step '%s'"), *SynthStepId);
        Note.Reason = TEXT("Branch Condition requires Boolean. Source provides Integer. Synthesized a > 0 comparison.");
        OutNotes.Add(MoveTemp(Note));
    }

    return bExpanded;
}
```

**Edge cases**:
- Float variable: `Greater_FloatFloat` would be needed. Check pin category and dispatch to the right comparison function. For simplicity, use `Greater_IntInt` for int and `Greater_FloatFloat` for float/double.
- Already a boolean variable: `bNeedsComparison` stays false, no synthesis
- Condition is a literal (not @ref): Skipped by the `StartsWith("@")` check
- Source step is a `call` (not `get_var`): Harder to determine output type pre-execution. Skip for now -- this only handles the common `get_var -> branch` pattern. The AI can always use an explicit comparison.
- `Greater_IntInt` might not resolve: Use the alias map (FunctionResolver has `Greater` -> `Greater_IntInt`). Safer to use the UE internal name directly.

**Declaration to add in header** (`OliveBlueprintPlanResolver.h`):
```cpp
static bool ExpandBranchConditions(
    FOliveIRBlueprintPlan& Plan,
    UBlueprint* Blueprint,
    TArray<FOliveResolverNote>& OutNotes);
```

---

## Issue A: Activity-Based Timeout (MEDIUM PRIORITY)

### Problem

The current timeout system in `LaunchCLIProcess()` has two mechanisms:
1. **Idle timeout** (line 524): 120 seconds with no stdout output -- kills the process
2. **Total runtime limit** (line 540): `AutonomousMaxRuntimeSeconds` (default 300s) -- hard kill regardless of progress

In the observed scenario, Claude Code was actively making tool calls every 15-30 seconds but hit the 300s hard limit while still making progress. The idle timeout never triggered because stdout was continuously producing output.

### Fix Design

**Strategy**: Replace the hard runtime limit with an activity-based timeout. "Activity" means a successful MCP tool call was received. The hard limit stays as a safety net but is raised to a higher value.

**New setting**: `AutonomousIdleToolSeconds` (default 120) -- kill if no successful MCP tool call in this many seconds. This is distinct from the stdout idle timeout (which catches hung processes that produce no output at all).

**Mechanism**: The MCP server already broadcasts `OnToolCalled` (line 693 of OliveMCPServer.cpp). The CLI provider subscribes to this delegate and updates a `LastToolCallTime` timestamp. The read loop checks this timestamp instead of (or in addition to) the hard runtime limit.

### Task A1: Add Activity Tracking to CLI Provider

**File**: `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h`

**Add member**:
```cpp
/** Timestamp of last successful MCP tool call during autonomous mode.
 *  Updated by MCP server OnToolCalled delegate. Used for activity-based timeout. */
std::atomic<double> LastToolCallTimestamp{0.0};

/** Delegate handle for MCP tool call subscription (cleaned up on process exit) */
FDelegateHandle ToolCallDelegateHandle;
```

### Task A2: Subscribe to MCP Tool Calls in Autonomous Launch

**File**: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**In `SendMessageAutonomous()`, before `LaunchCLIProcess()`**:
```cpp
// Subscribe to MCP tool call events for activity tracking
LastToolCallTimestamp.store(FPlatformTime::Seconds());
ToolCallDelegateHandle = FOliveMCPServer::Get().OnToolCalled.AddLambda(
    [this, Guard](const FString& ToolName, const FString& ClientId)
    {
        if (*Guard)
        {
            LastToolCallTimestamp.store(FPlatformTime::Seconds());
        }
    });
```

**In `HandleResponseCompleteAutonomous()`, clean up**:
```cpp
FOliveMCPServer::Get().OnToolCalled.Remove(ToolCallDelegateHandle);
ToolCallDelegateHandle.Reset();
```

**Also clean up in `KillProcess()` and destructor** to prevent dangling delegates.

### Task A3: Implement Activity-Based Timeout in Read Loop

**File**: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**In the read loop** (around line 540), replace the hard runtime check with:

```cpp
// Activity-based timeout: kill if no MCP tool call in AutonomousIdleToolSeconds.
// This catches "thinking but not acting" scenarios where stdout is flowing
// (so idle timeout doesn't trigger) but no tool calls are being made.
const double LastToolCall = LastToolCallTimestamp.load();
if (LastToolCall > 0.0)
{
    const double IdleToolTimeout = RuntimeSettings ? static_cast<double>(RuntimeSettings->AutonomousIdleToolSeconds) : 120.0;
    if (IdleToolTimeout > 0.0 && (FPlatformTime::Seconds() - LastToolCall) > IdleToolTimeout)
    {
        UE_LOG(LogOliveCLIProvider, Warning,
            TEXT("%s process: no MCP tool call in %.0f seconds - terminating"),
            *CLIName, IdleToolTimeout);
        bStopReading = true;
        if (*Guard)
        {
            FPlatformProcess::TerminateProc(ProcessHandle, true);
        }
        break;
    }
}

// Hard runtime limit stays as absolute safety net (raised default to 900s)
if (MaxRuntimeSeconds > 0.0 && (FPlatformTime::Seconds() - ProcessStartTime) > MaxRuntimeSeconds)
{
    // ... existing code ...
}
```

**Note on thread safety**: `LastToolCallTimestamp` is `std::atomic<double>` and the read loop runs on a background thread. The MCP tool call delegate fires on the game thread. `std::atomic<double>` with default memory ordering is sufficient for this use case (we only need to see the latest timestamp, not strict ordering).

**Note on capturing**: The `LastToolCallTimestamp` is a member, not a local. The background lambda in `LaunchCLIProcess` accesses it via `this`, guarded by `AliveGuard`. The `OnToolCalled` lambda also accesses via `this` with the same guard. Both are safe because: (1) AliveGuard is set to false in the destructor BEFORE member destruction, (2) the atomic is destroyed with the object, and (3) both lambdas check guard before access.

### Task A4: Add Setting for Activity Timeout

**File**: `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`

**Add after `AutonomousMaxRuntimeSeconds`**:
```cpp
/** Maximum seconds with no MCP tool call before killing an autonomous CLI process.
 *  This catches "thinking but not acting" -- the AI produces stdout but makes no progress.
 *  Set to 0 to disable. The idle stdout timeout (120s) still catches fully hung processes. */
UPROPERTY(Config, EditAnywhere, Category="AI Provider",
    meta=(DisplayName="Autonomous Tool Idle Timeout (seconds)", ClampMin=0, ClampMax=600))
int32 AutonomousIdleToolSeconds = 120;
```

**Raise the default hard limit**:
```cpp
// Change default from 300 to 900
int32 AutonomousMaxRuntimeSeconds = 900;
```

### Task A5: Log Activity Stats on Completion

**File**: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**In `HandleResponseCompleteAutonomous()`**, log:
```cpp
const double TotalRuntime = FPlatformTime::Seconds() - /* capture start time */;
const double LastTool = LastToolCallTimestamp.load();
UE_LOG(LogOliveCLIProvider, Log,
    TEXT("Autonomous run complete: %.1fs total runtime, last tool call %.1fs ago"),
    TotalRuntime, LastTool > 0.0 ? FPlatformTime::Seconds() - LastTool : -1.0);
```

---

## Issue D: Reduce Redundant Previews (LOW PRIORITY)

### Problem

Claude previewed 3-4 times before attempting its first apply. Each preview is a full tool call round-trip (~5 seconds), wasting 15-20 seconds per plan.

### Task D1: Add Prompt Guidance to Limit Previews

**File**: `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**Change**: Add a rule in the "## Rules" section:

After the existing rule "- Use apply_plan_json for 3+ nodes. For 1-2 nodes, add_node + connect_pins is fine." add:

```
- ONE PREVIEW per apply attempt. If preview succeeds, apply immediately in the next turn. Do NOT preview the same plan multiple times.
- If apply_plan_json returns wiring errors (partial success), fix with granular connect_pins/set_pin_default -- do NOT re-preview the whole plan.
```

### Task D2: Add Same Guidance to AGENTS.md

**File**: `AGENTS.md`

**Change**: In the "## Important Rules" section, after the preview rule:

```
- **One preview per apply**: Preview once, then apply. Do not re-preview the same plan. If apply fails partially, fix with `connect_pins`/`set_pin_default`, not another full plan.
```

### Task D3: Add Same Guidance to Sandbox CLAUDE.md

**File**: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**In `SetupAutonomousSandbox()`**, in the "## Critical Rules" block, add:
```
- ONE preview before each apply. If preview succeeds, apply in the next turn. Do NOT re-preview the same plan.\n
```

---

## Implementation Order

1. **C1** (guard rollback) -- highest impact, smallest change, ~15 min
2. **B1** (function graph entry detection) -- enables correct function graph plans, ~30 min
3. **B2** (bare variable refs) -- enables @VariableName syntax for non-SCS variables, ~30 min
4. **B3** (branch condition synthesis) -- new resolver pass, ~45 min
5. **A1-A5** (activity timeout) -- requires touching provider, MCP server, settings, ~60 min
6. **D1-D3** (prompt guidance) -- text-only changes, ~10 min

Total estimated time: ~3 hours

## Dependencies

- C1 has no dependencies
- B1, B2, B3 are independent of each other
- A1-A5 are sequential (A1 -> A2 -> A3 -> A4 -> A5)
- D1-D3 are independent of everything else

Two coders could work in parallel: Coder A on C1 + B1 + B2 + B3, Coder B on A1-A5 + D1-D3.

## New Error Codes

None needed. All fixes either prevent errors or improve existing error paths.

## New Settings

- `AutonomousIdleToolSeconds` (int32, default 120, 0-600) -- activity-based timeout
- `AutonomousMaxRuntimeSeconds` default raised from 300 to 900

## Files Modified Summary

| File | Issues |
|------|--------|
| `Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | C1, C2 |
| `Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | B1, B2, B3 |
| `Blueprint/Public/Plan/OliveBlueprintPlanResolver.h` | B3 (declaration) |
| `Private/Providers/OliveCLIProviderBase.cpp` | A2, A3, A5, D3 |
| `Public/Providers/OliveCLIProviderBase.h` | A1 |
| `Public/Settings/OliveAISettings.h` | A4 |
| `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | D1 |
| `AGENTS.md` | D2 |
