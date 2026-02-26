# Blueprint Pipeline Reliability -- Implementation Plan

This document provides exact, line-level instructions for implementing the three phases
described in `plans/blueprint-pipeline-reliability-handoff.md`. Every line number and
code reference has been verified against the actual codebase as of 2026-02-25.

**IMPORTANT**: The handoff plan's line numbers (e.g., "line ~73044") are completely stale
and do not correspond to any actual file. This document replaces all of those with
verified locations.

---

## Phase 1: Safety Net (Granular Fallback + Retry Policy)

### 1A: Modify Retry Policy -- Remove Global Budget Cap

**File**: `Source/OliveAIEditor/Public/Brain/OliveRetryPolicy.h`
**Lines**: 10-23

The `FOliveRetryPolicy` struct currently has:
```cpp
int32 MaxCorrectionCyclesPerWorker = 5;  // line 16
```

**Changes**:

1. Raise `MaxCorrectionCyclesPerWorker` default from `5` to `20`. Do NOT remove the field
   entirely because `IsBudgetExhausted()` (line 61) references it, and
   `OliveConversationManager.cpp` line 134 sets it from settings. Raising it to 20 makes
   it a backstop that never fires before per-error limits.

2. Add a feature flag after `RetryDelaySeconds` (after line 22):
```cpp
/** When true, plan-loop detection triggers granular fallback instead of StopWorker */
bool bAllowGranularFallback = true;
```

**What NOT to change**: `MaxRetriesPerError = 3` (line 13). This is the primary limiter
and works correctly. Do not touch `FOliveLoopDetector` class (lines 29-116).

**ConversationManager update**: In
`Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` line 134, the
constructor wires `RetryPolicy.MaxCorrectionCyclesPerWorker` from settings. This still
works -- the settings value now has a higher floor. No change needed here unless the user
has explicitly set a low value in settings. The default change is sufficient.

---

### 1B: Add Fallback State Tracking to Self-Correction Policy

**File**: `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h`
**Lines**: 159-162

Add new private members after `PreviousPlanHashes` (line 160):

```cpp
/** Map of plan content hash -> submission count. Reset per turn. */
TMap<FString, int32> PreviousPlanHashes;

/** Whether we have switched to granular fallback mode for this turn */
bool bIsInGranularFallback = false;

/** The last plan failure error text (so granular mode knows what to avoid) */
FString LastPlanFailureReason;
```

Add new private method declarations after `BuildPlanHash` (before line 159):

```cpp
/**
 * Build message for plan failures where rollback happened but retries remain.
 * Tells AI to resubmit corrected plan, NOT to reference deleted node IDs.
 */
FString BuildRollbackAwareMessage(
    const FString& ToolName,
    const FString& Errors,
    int32 AttemptNum,
    int32 MaxAttempts,
    int32 RolledBackNodeCount) const;

/**
 * Build message for forced switch from plan mode to granular step-by-step mode.
 * This is mandatory -- the AI MUST use granular tools after this message.
 */
FString BuildGranularFallbackMessage(
    const FString& ToolName,
    const FString& Errors,
    const FString& AssetPath,
    int32 RolledBackNodeCount) const;
```

**File**: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`
**Function**: `Reset()` at line 194

Currently:
```cpp
void FOliveSelfCorrectionPolicy::Reset()
{
    PreviousPlanHashes.Empty();
}
```

Change to:
```cpp
void FOliveSelfCorrectionPolicy::Reset()
{
    PreviousPlanHashes.Empty();
    bIsInGranularFallback = false;
    LastPlanFailureReason.Empty();
}
```

**ConversationManager reset**: In
`Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` line 252,
`SelfCorrectionPolicy.Reset()` is already called per user message. Since we added
the fields to `Reset()`, no additional change is needed in ConversationManager.

---

### 1C: Modify Evaluate() -- Rollback-Aware + Fallback Switch

**File**: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`
**Function**: `Evaluate()` starting at line 15

This is the core change, concentrated in the compile failure branch (lines 72-123).

**Step 1 -- Extract rollback count from result JSON**

The `HasCompileFailure()` method (line 199) already parses a `DataObj` from the nested
`"data"` JSON object (lines 213-225). We need to extract `rolled_back_nodes` from that
same data. However, `DataObj` is local to `HasCompileFailure()` and not returned.

Option A (recommended): Add an out-parameter `int32& OutRolledBackNodeCount` to
`HasCompileFailure()`. This is cleaner than re-parsing JSON.

Update the signature in the header (line 103):
```cpp
bool HasCompileFailure(
    const FString& ResultJson,
    FString& OutErrors,
    FString& OutAssetPath,
    bool& OutHasStaleErrors,
    int32& OutRolledBackNodeCount) const;
```

In the implementation (line 199), initialize `OutRolledBackNodeCount = 0` at the top,
then after `DataObj` is populated (around line 225), extract:
```cpp
if (DataObj.IsValid())
{
    double RolledBackDouble = 0;
    if (DataObj->TryGetNumberField(TEXT("rolled_back_nodes"), RolledBackDouble))
    {
        OutRolledBackNodeCount = static_cast<int32>(RolledBackDouble);
    }
}
```

Update the call site in `Evaluate()` (line 75):
```cpp
int32 RolledBackNodeCount = 0;
if (HasCompileFailure(ResultJson, CompileErrors, AssetPath, bHasStaleErrors, RolledBackNodeCount))
```

**Step 2 -- Modify the loop detection branch for compile failures**

Current code at lines 107-114:
```cpp
// Check for loops
if (LoopDetector.IsLooping(Signature, Policy) || LoopDetector.IsOscillating() || LoopDetector.IsBudgetExhausted(Policy))
{
    Decision.Action = EOliveCorrectionAction::StopWorker;
    Decision.LoopReport = LoopDetector.BuildLoopReport();
    UE_LOG(LogOliveAI, Warning, TEXT("SelfCorrection: Loop detected for compile failure on '%s'. Stopping."), *AssetPath);
    return Decision;
}
```

Replace with:
```cpp
// Check for loops
if (LoopDetector.IsLooping(Signature, Policy) || LoopDetector.IsOscillating())
{
    if (!bIsInGranularFallback && Policy.bAllowGranularFallback)
    {
        // Switch to granular fallback instead of dying
        bIsInGranularFallback = true;
        LastPlanFailureReason = CompileErrors;

        // Reset the loop detector so granular attempts get a fresh error budget
        LoopDetector.Reset();

        Decision.Action = EOliveCorrectionAction::FeedBackErrors;
        Decision.EnrichedMessage = BuildGranularFallbackMessage(
            ToolName, CompileErrors, AssetPath, RolledBackNodeCount);

        UE_LOG(LogOliveAI, Warning,
            TEXT("SelfCorrection: Plan loop detected for '%s'. Switching to GRANULAR FALLBACK mode."),
            *AssetPath);
        return Decision;
    }

    // Already in granular fallback, or fallback disabled -- truly stop
    Decision.Action = EOliveCorrectionAction::StopWorker;
    Decision.LoopReport = LoopDetector.BuildLoopReport();
    UE_LOG(LogOliveAI, Warning,
        TEXT("SelfCorrection: Loop detected for compile failure on '%s' (granular=%s). Stopping."),
        *AssetPath, bIsInGranularFallback ? TEXT("true") : TEXT("false"));
    return Decision;
}
// Retain IsBudgetExhausted as hard backstop (now at 20 cycles)
if (LoopDetector.IsBudgetExhausted(Policy))
{
    Decision.Action = EOliveCorrectionAction::StopWorker;
    Decision.LoopReport = LoopDetector.BuildLoopReport();
    UE_LOG(LogOliveAI, Warning,
        TEXT("SelfCorrection: Global budget exhausted on '%s'. Stopping."), *AssetPath);
    return Decision;
}
```

**Step 3 -- Add rollback-aware messaging for non-looping compile failures**

Current code at line 118:
```cpp
Decision.EnrichedMessage = BuildCompileErrorMessage(ToolName, CompileErrors, SignatureAttempts, Policy.MaxRetriesPerError);
```

Replace with:
```cpp
if (RolledBackNodeCount > 0)
{
    Decision.EnrichedMessage = BuildRollbackAwareMessage(
        ToolName, CompileErrors, SignatureAttempts, Policy.MaxRetriesPerError, RolledBackNodeCount);
}
else
{
    Decision.EnrichedMessage = BuildCompileErrorMessage(
        ToolName, CompileErrors, SignatureAttempts, Policy.MaxRetriesPerError);
}
```

---

### 1D: New Message Builder Functions

**File**: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

Add after the existing `BuildToolErrorMessage()` function (which ends at line 699).

**BuildRollbackAwareMessage** (for plan retries 1-3 after rollback):

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
             "%d nodes were ROLLED BACK -- the graph is restored to its pre-plan state.\n"
             "Errors:\n%s\n"
             "REQUIRED ACTION: Fix the plan and resubmit with apply_plan_json.\n"
             "Do NOT use connect_pins or reference any node IDs from the failed plan -- those nodes no longer exist.\n"
             "Common fixes:\n"
             "- Latent calls (Delay, AI MoveTo, etc.) CANNOT be in function graphs -- use a Custom Event in EventGraph instead\n"
             "- Function parameters are NOT class variables -- if in a function graph, parameters are pins on the entry node\n"
             "- Missing variables -- ensure add_variable succeeded before referencing in a plan"),
        AttemptNum, MaxAttempts, *ToolName, RolledBackNodeCount, *Errors);
}
```

**BuildGranularFallbackMessage** (for forced switch after 3 plan failures):

```cpp
FString FOliveSelfCorrectionPolicy::BuildGranularFallbackMessage(
    const FString& ToolName,
    const FString& Errors,
    const FString& AssetPath,
    int32 RolledBackNodeCount) const
{
    return FString::Printf(
        TEXT("[PLAN APPROACH FAILED -- SWITCHING TO STEP-BY-STEP MODE]\n"
             "The plan_json approach failed 3 times with the same error. %d nodes were rolled back.\n"
             "Last error:\n%s\n\n"
             "You MUST now switch to granular node-by-node building:\n"
             "1. Call blueprint.read or blueprint.read_function on '%s' to see current graph state\n"
             "2. Use blueprint.add_node to create nodes ONE AT A TIME -- each call returns the node_id and pin manifest\n"
             "3. Use blueprint.connect_pins to wire them using the node_ids from step 2\n"
             "4. Use blueprint.set_pin_default for any default values\n"
             "5. Call blueprint.compile to verify\n\n"
             "This is slower but gives you accurate state after each operation.\n"
             "Do NOT use apply_plan_json again for this graph -- it has failed repeatedly."),
        RolledBackNodeCount, *Errors, *AssetPath);
}
```

---

### 1E: Template Compile Error Propagation

**File**: `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp`
**Lines**: 1292-1349 (compile + result assembly section)

Currently the template always returns `FOliveToolResult::Success(ResultData)` at line 1348,
even when compile fails. The compile errors are only visible buried inside the `warnings`
array (lines 1296-1300).

**Changes**:

1. After the compile section (line 1293-1300), add a compile_result JSON object to
   ResultData so `HasCompileFailure()` in self-correction can detect it:

Insert after line 1300 (`}` closing the compile error loop), before line 1302 (`// 13. Build result`):
```cpp
// Add compile_result structure for self-correction detection
{
    TSharedPtr<FJsonObject> CompileResultJson = MakeShared<FJsonObject>();
    CompileResultJson->SetBoolField(TEXT("success"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        TArray<TSharedPtr<FJsonValue>> ErrorJsonArray;
        for (const FOliveIRCompileError& Err : CompileResult.Errors)
        {
            ErrorJsonArray.Add(MakeShared<FJsonValueString>(Err.Message));
        }
        CompileResultJson->SetArrayField(TEXT("errors"), ErrorJsonArray);
    }
    // Store temporarily; will be added to ResultData below
    // (variable scope: CompileResultJson persists until result assembly)
}
```

Actually, a simpler approach: add the compile_result to ResultData in the result
assembly section. After line 1310 (`ResultData->SetBoolField(TEXT("compiled"), ...)`),
add:
```cpp
// compile_result structure for self-correction compatibility
{
    TSharedPtr<FJsonObject> CompileResultJson = MakeShared<FJsonObject>();
    CompileResultJson->SetBoolField(TEXT("success"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        TArray<TSharedPtr<FJsonValue>> ErrorJsonArray;
        for (const FOliveIRCompileError& Err : CompileResult.Errors)
        {
            ErrorJsonArray.Add(MakeShared<FJsonValueString>(Err.Message));
        }
        CompileResultJson->SetArrayField(TEXT("errors"), ErrorJsonArray);
    }
    ResultData->SetObjectField(TEXT("compile_result"), CompileResultJson);
}
```

2. Change the success message. Currently line 1348 returns:
```cpp
return FOliveToolResult::Success(ResultData);
```

Change to conditional messaging:
```cpp
if (!CompileResult.bSuccess)
{
    FString Msg = FString::Printf(
        TEXT("Template '%s' applied but Blueprint FAILED TO COMPILE. %d compile error(s). "
             "Review errors and fix before proceeding."),
        *TemplateId, CompileResult.Errors.Num());
    return FOliveToolResult::Success(ResultData, Msg);
}
return FOliveToolResult::Success(ResultData);
```

**Note**: Check whether `FOliveToolResult::Success()` accepts an optional message parameter.
If not, set the message on the result manually:
```cpp
FOliveToolResult Result = FOliveToolResult::Success(ResultData);
if (!CompileResult.bSuccess)
{
    Result.Message = FString::Printf(
        TEXT("Template '%s' applied but Blueprint FAILED TO COMPILE. %d compile error(s). "
             "Review errors and fix before proceeding."),
        *TemplateId, CompileResult.Errors.Num());
}
return Result;
```

Verify the `FOliveToolResult` struct to confirm the `Message` field name.

---

### 1F: Handle Tool Failures After Rollback

**File**: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`
**Lines**: 126-187 (tool failure branch)

The tool failure path also needs rollback detection. Extract `rolled_back_nodes` from the
result JSON. Since `HasToolFailure()` already parses the JSON (line 397), add similar
extraction.

Add a helper at the top of the tool failure branch (after line 127):
```cpp
// Extract rollback info from result (same pattern as compile failure path)
int32 ToolRolledBackNodeCount = 0;
{
    TSharedPtr<FJsonObject> JsonObj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
    if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
    {
        const TSharedPtr<FJsonObject>* DataObj = nullptr;
        if (JsonObj->TryGetObjectField(TEXT("data"), DataObj) && DataObj && (*DataObj).IsValid())
        {
            double RBDouble = 0;
            if ((*DataObj)->TryGetNumberField(TEXT("rolled_back_nodes"), RBDouble))
            {
                ToolRolledBackNodeCount = static_cast<int32>(RBDouble);
            }
        }
    }
}
```

Then in the tool failure loop detection (line 157):
```cpp
if (LoopDetector.IsLooping(Signature, Policy) || LoopDetector.IsOscillating() || LoopDetector.IsBudgetExhausted(Policy))
```

Apply the same granular fallback pattern as the compile failure branch: check
`bIsInGranularFallback`, switch if not already in it, stop if already in it.

And for the standard retry path (line 182), if `ToolRolledBackNodeCount > 0`,
append a note to the enriched message:
```cpp
if (ToolRolledBackNodeCount > 0)
{
    Decision.EnrichedMessage += FString::Printf(
        TEXT("\nNOTE: %d nodes were ROLLED BACK. Do NOT reference node IDs from the failed operation."),
        ToolRolledBackNodeCount);
}
```

---

### Phase 1 Build Verification

After completing all Phase 1 tasks, run incremental build:
```
"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex
```

Expected: Clean compile. The only header changes are additive (new fields, new method
declarations). No signature changes to public APIs except the optional new parameter on
`HasCompileFailure` (which is private).

---

## Phase 2: Graph Context Threading

### 2A: Add Graph Context Struct

**File**: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`

Add the struct BEFORE `FOliveResolverNote` (before line 19). This is a new struct at file
scope, not inside any class.

```cpp
// Forward declaration for graph context
class UEdGraph;

/**
 * Captured context about the target graph for plan resolution and validation.
 * Built once before plan execution, consumed by resolver, validator, and executor.
 */
struct OLIVEAIEDITOR_API FOliveGraphContext
{
    /** Name of the graph (e.g., "EventGraph", "Fire", "ApplyHitDamage") */
    FString GraphName;

    /** True if this is a function graph (not EventGraph, not macro) */
    bool bIsFunctionGraph = false;

    /** True if this is a macro graph */
    bool bIsMacroGraph = false;

    /** Function input parameter names -- only populated for function graphs */
    TArray<FString> InputParamNames;

    /** Function output/return parameter names -- only populated for function graphs */
    TArray<FString> OutputParamNames;

    /** The actual UEdGraph pointer (for direct inspection if needed) */
    UEdGraph* Graph = nullptr;

    /** Build context from a Blueprint and graph name */
    static FOliveGraphContext BuildFromBlueprint(UBlueprint* Blueprint, const FString& GraphName);
};
```

**Implementation** -- add to
`Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`.

Add includes at the top (after existing includes, around line 20):
```cpp
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
```

Note: `K2Node_FunctionEntry.h` is NOT currently included in this file (it IS included in
OlivePlanExecutor.cpp). `K2Node_FunctionResult.h` is not included anywhere in the plan
pipeline yet.

Add the implementation before the existing `Resolve()` method (before line 102):

```cpp
FOliveGraphContext FOliveGraphContext::BuildFromBlueprint(UBlueprint* Blueprint, const FString& GraphName)
{
    FOliveGraphContext Ctx;
    Ctx.GraphName = GraphName;

    if (!Blueprint)
    {
        return Ctx;
    }

    // Search UbergraphPages (EventGraph and other ubergraphs)
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph && Graph->GetFName() == FName(*GraphName))
        {
            Ctx.Graph = Graph;
            return Ctx; // EventGraph/ubergraph -- not a function graph
        }
    }

    // Search FunctionGraphs
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == FName(*GraphName))
        {
            Ctx.Graph = Graph;
            Ctx.bIsFunctionGraph = true;

            // Scan for FunctionEntry to get input param names
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
                {
                    for (const auto& Pin : Entry->UserDefinedPins)
                    {
                        if (Pin.IsValid())
                        {
                            Ctx.InputParamNames.Add(Pin->PinName.ToString());
                        }
                    }
                }
                else if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
                {
                    for (const auto& Pin : Result->UserDefinedPins)
                    {
                        if (Pin.IsValid())
                        {
                            Ctx.OutputParamNames.Add(Pin->PinName.ToString());
                        }
                    }
                }
            }

            UE_LOG(LogOlivePlanResolver, Log,
                TEXT("GraphContext: '%s' is function graph (%d inputs, %d outputs)"),
                *GraphName, Ctx.InputParamNames.Num(), Ctx.OutputParamNames.Num());
            return Ctx;
        }
    }

    // Search MacroGraphs
    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        if (Graph && Graph->GetFName() == FName(*GraphName))
        {
            Ctx.Graph = Graph;
            Ctx.bIsMacroGraph = true;
            return Ctx;
        }
    }

    // Graph not found -- could be a new graph about to be created
    UE_LOG(LogOlivePlanResolver, Log,
        TEXT("GraphContext: Graph '%s' not found in Blueprint '%s' -- using default context"),
        *GraphName, *Blueprint->GetName());
    return Ctx;
}
```

---

### 2B: Add bIsLatent to Resolved Steps

**File**: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`
**Struct**: `FOliveResolvedStep` (line 50)

Add after `bIsPure` (line 69):
```cpp
/**
 * Whether this step uses a latent function (e.g., Delay, AI MoveTo).
 * Latent functions cannot be used in function graphs.
 * Set by the resolver from UFunction::HasMetaData("Latent") or op type.
 */
bool bIsLatent = false;
```

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

In `ResolveCallOp()`, after `bIsPure` is set (line 487):
```cpp
Out.bIsPure = Match.Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
Out.bIsLatent = Match.Function->HasMetaData(TEXT("Latent"));
```

In `ResolveStep()`, after the Delay op branch (line 356-358):
```cpp
else if (Op == OlivePlanOps::Delay)
{
    bResult = ResolveSimpleOp(Step, OliveNodeTypes::Delay, OutResolved);
    if (bResult)
    {
        OutResolved.bIsLatent = true;
    }
}
```

This changes the existing one-liner at line 358 to a multi-line block. The `bIsLatent`
flag on Delay must be set explicitly because `ResolveSimpleOp` does not check UFunction
metadata.

---

### 2C: Thread GraphContext Through Resolver

**File**: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`

Update `Resolve()` signature (line 130-132):
```cpp
static FOlivePlanResolveResult Resolve(
    const FOliveIRBlueprintPlan& Plan,
    UBlueprint* Blueprint,
    const FOliveGraphContext& GraphContext = FOliveGraphContext());
```

Update private method signatures. `ResolveStep()` (line 214-220):
```cpp
static bool ResolveStep(
    const FOliveIRBlueprintPlanStep& Step,
    UBlueprint* Blueprint,
    int32 StepIndex,
    FOliveResolvedStep& OutResolved,
    TArray<FOliveIRBlueprintPlanError>& OutErrors,
    TArray<FString>& OutWarnings,
    const FOliveGraphContext& GraphContext);
```

`ResolveGetVarOp()` (line 236-242):
```cpp
static bool ResolveGetVarOp(
    const FOliveIRBlueprintPlanStep& Step,
    UBlueprint* BP,
    int32 Idx,
    FOliveResolvedStep& Out,
    TArray<FOliveIRBlueprintPlanError>& Errors,
    TArray<FString>& Warnings,
    const FOliveGraphContext& GraphContext);
```

`ResolveSetVarOp()` (line 244-251):
```cpp
static bool ResolveSetVarOp(
    const FOliveIRBlueprintPlanStep& Step,
    UBlueprint* BP,
    int32 Idx,
    FOliveResolvedStep& Out,
    TArray<FOliveIRBlueprintPlanError>& Errors,
    TArray<FString>& Warnings,
    const FOliveGraphContext& GraphContext);
```

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

Update `Resolve()` (line 102) to accept and pass the context:
```cpp
FOlivePlanResolveResult FOliveBlueprintPlanResolver::Resolve(
    const FOliveIRBlueprintPlan& Plan,
    UBlueprint* Blueprint,
    const FOliveGraphContext& GraphContext)
```

In the `Resolve()` body, pass `GraphContext` to `ResolveStep()` calls (around line 122 area --
search for all calls to `ResolveStep`).

Update `ResolveStep()` implementation (line 282) to accept and forward the context.
Forward it to `ResolveGetVarOp()` and `ResolveSetVarOp()` calls (lines 306, 310).

**ResolveGetVarOp modification** (line 596):

Add at the TOP of the function body, before any existing variable lookup:
```cpp
// If we're in a function graph, check if target matches a function input parameter
if (GraphContext.bIsFunctionGraph)
{
    for (const FString& ParamName : GraphContext.InputParamNames)
    {
        if (ParamName.Equals(Step.Target, ESearchCase::IgnoreCase))
        {
            // This is a function input parameter, not a class variable
            Out.StepId = Step.StepId;
            Out.NodeType = OliveNodeTypes::FunctionInput;
            Out.Properties.Add(TEXT("param_name"), ParamName);
            Out.bIsPure = true;

            Out.ResolverNotes.Add(FOliveResolverNote{
                TEXT("target"),
                Step.Target,
                FString::Printf(TEXT("FunctionInput(%s)"), *ParamName),
                TEXT("Matched function input parameter -- will map to FunctionEntry output pin")
            });

            UE_LOG(LogOlivePlanResolver, Log,
                TEXT("    ResolveGetVarOp: '%s' matched function input param '%s'"),
                *Step.Target, *ParamName);
            return true;
        }
    }
}
```

**ResolveSetVarOp modification** (line 688):

Add at the TOP of the function body, before existing variable lookup:
```cpp
// If we're in a function graph, check if target matches a function output parameter
if (GraphContext.bIsFunctionGraph)
{
    for (const FString& ParamName : GraphContext.OutputParamNames)
    {
        if (ParamName.Equals(Step.Target, ESearchCase::IgnoreCase))
        {
            Out.StepId = Step.StepId;
            Out.NodeType = OliveNodeTypes::FunctionOutput;
            Out.Properties.Add(TEXT("param_name"), ParamName);
            Out.bIsPure = false; // FunctionResult has exec input

            Out.ResolverNotes.Add(FOliveResolverNote{
                TEXT("target"),
                Step.Target,
                FString::Printf(TEXT("FunctionOutput(%s)"), *ParamName),
                TEXT("Matched function output parameter -- will map to FunctionResult input pin")
            });

            UE_LOG(LogOlivePlanResolver, Log,
                TEXT("    ResolveSetVarOp: '%s' matched function output param '%s'"),
                *Step.Target, *ParamName);
            return true;
        }
    }
}
```

---

### 2D: Add Latent-in-Function Validation

**File**: `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanValidator.h`

Update `Validate()` signature (line 72-75):
```cpp
static FOlivePlanValidationResult Validate(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    UBlueprint* Blueprint,
    const FOliveGraphContext& GraphContext = FOliveGraphContext());
```

Add forward declaration at the top (after line 9):
```cpp
struct FOliveGraphContext;
```

Add include in `OlivePlanValidator.h` (or rely on the forward declaration since we only
use it as a const ref parameter -- forward declaration is sufficient for the header):
```cpp
// Forward declaration is enough; FOliveGraphContext is defined in OliveBlueprintPlanResolver.h
```

Add private method declaration (after line 94):
```cpp
/**
 * Check 3: Latent-in-function-graph guard.
 * Rejects latent actions (Delay, AI MoveTo, etc.) in function graphs.
 */
static void CheckLatentInFunctionGraph(
    const FOlivePlanValidationContext& Context,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    const FOliveGraphContext& GraphContext,
    FOlivePlanValidationResult& Result);
```

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp`

Add include at the top (after line 8):
```cpp
#include "Plan/OliveBlueprintPlanResolver.h"  // For FOliveGraphContext
```

Note: This include already exists at line 4. So no new include needed for the header,
but we need `FOliveGraphContext` which is defined in OliveBlueprintPlanResolver.h, which
IS already included.

Update `Validate()` signature (line 16-19):
```cpp
FOlivePlanValidationResult FOlivePlanValidator::Validate(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    UBlueprint* Blueprint,
    const FOliveGraphContext& GraphContext)
```

Add the new check call (after line 41, alongside existing checks):
```cpp
CheckComponentFunctionTargets(Context, Result);
CheckExecWiringConflicts(Context, Result);
CheckLatentInFunctionGraph(Context, ResolvedSteps, GraphContext, Result);
```

Add implementation at the end of the file:
```cpp
// ============================================================================
// Check 3: Latent-in-Function-Graph Guard
// ============================================================================

void FOlivePlanValidator::CheckLatentInFunctionGraph(
    const FOlivePlanValidationContext& Context,
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
        if (!ResolvedSteps[i].bIsLatent)
        {
            continue;
        }

        const FString& StepId = Context.Plan.Steps.IsValidIndex(i)
            ? Context.Plan.Steps[i].StepId
            : ResolvedSteps[i].StepId;

        // Determine the latent function name for the error message
        FString LatentName = TEXT("latent action");
        if (Context.Plan.Steps.IsValidIndex(i)
            && Context.Plan.Steps[i].Op == OlivePlanOps::Delay)
        {
            LatentName = TEXT("Delay");
        }
        else if (const FString* FN = ResolvedSteps[i].Properties.Find(TEXT("function_name")))
        {
            LatentName = *FN;
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

        UE_LOG(LogOlivePlanValidator, Warning,
            TEXT("Phase 0: LATENT_IN_FUNCTION -- step '%s' uses '%s' in function graph '%s'"),
            *StepId, *LatentName, *GraphContext.GraphName);
    }
}
```

**New error code**: `LATENT_IN_FUNCTION`

---

### 2E: New NodeType Constants for Function Params

**File**: `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h`
**Namespace**: `OliveNodeTypes` (line 21)

Add after the existing `Comment` and `Reroute` constants (after line 59):
```cpp
// Function Parameter (virtual -- maps to existing FunctionEntry/FunctionResult nodes)
const FString FunctionInput = TEXT("FunctionInput");
const FString FunctionOutput = TEXT("FunctionOutput");
```

---

### 2F: Executor Handles FunctionInput / FunctionOutput

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

Add include (if not already present):
```cpp
#include "K2Node_FunctionResult.h"
```

Check: `K2Node_FunctionEntry.h` is already included at line 31.
`K2Node_FunctionResult.h` is NOT currently included. Add it after line 31.

In `PhaseCreateNodes()`, add virtual step handling BEFORE the normal node creation block.
The event reuse check ends at line 365 (`}`). The normal node creation starts at line 367
(`// Normal node creation via GraphWriter.AddNode()`).

Insert between lines 365 and 367:

```cpp
        // ----------------------------------------------------------------
        // Virtual step: FunctionInput -- maps to existing FunctionEntry node
        // ----------------------------------------------------------------
        if (NodeType == OliveNodeTypes::FunctionInput)
        {
            UK2Node_FunctionEntry* EntryNode = nullptr;
            for (UEdGraphNode* Node : Context.Graph->Nodes)
            {
                EntryNode = Cast<UK2Node_FunctionEntry>(Node);
                if (EntryNode) break;
            }

            if (!EntryNode)
            {
                UE_LOG(LogOlivePlanExecutor, Error,
                    TEXT("Phase 1 FAIL: Step '%s' (FunctionInput) -- no FunctionEntry node in graph '%s'"),
                    *StepId, *Context.GraphName);

                Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("NODE_CREATION_FAILED"),
                    StepId,
                    FString::Printf(TEXT("/steps/%d"), i),
                    FString::Printf(TEXT("No FunctionEntry node found in graph '%s' for FunctionInput step '%s'"),
                        *Context.GraphName, *StepId),
                    TEXT("Ensure this is a function graph (not EventGraph)")));

                CleanupCreatedNodes();
                return false;
            }

            const FString ReuseNodeId = EntryNode->NodeGuid.ToString();
            FOlivePinManifest Manifest = FOlivePinManifest::Build(
                EntryNode, StepId, ReuseNodeId, NodeType);

            Context.StepManifests.Add(StepId, MoveTemp(Manifest));
            Context.StepToNodeMap.Add(StepId, ReuseNodeId);
            Context.StepToNodePtr.Add(StepId, EntryNode);
            Context.CreatedNodeCount++;
            ReusedStepIds.Add(StepId);
            Context.ReusedStepIds.Add(StepId);

            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("  -> Virtual FunctionInput step '%s' -> FunctionEntry node '%s'"),
                *StepId, *ReuseNodeId);
            continue;
        }

        // ----------------------------------------------------------------
        // Virtual step: FunctionOutput -- maps to existing FunctionResult node
        // ----------------------------------------------------------------
        if (NodeType == OliveNodeTypes::FunctionOutput)
        {
            UK2Node_FunctionResult* ResultNode = nullptr;
            for (UEdGraphNode* Node : Context.Graph->Nodes)
            {
                ResultNode = Cast<UK2Node_FunctionResult>(Node);
                if (ResultNode) break;
            }

            if (!ResultNode)
            {
                UE_LOG(LogOlivePlanExecutor, Error,
                    TEXT("Phase 1 FAIL: Step '%s' (FunctionOutput) -- no FunctionResult node in graph '%s'"),
                    *StepId, *Context.GraphName);

                Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("NODE_CREATION_FAILED"),
                    StepId,
                    FString::Printf(TEXT("/steps/%d"), i),
                    FString::Printf(TEXT("No FunctionResult node found in graph '%s' for FunctionOutput step '%s'"),
                        *Context.GraphName, *StepId),
                    TEXT("Ensure the function has return values defined")));

                CleanupCreatedNodes();
                return false;
            }

            const FString ReuseNodeId = ResultNode->NodeGuid.ToString();
            FOlivePinManifest Manifest = FOlivePinManifest::Build(
                ResultNode, StepId, ReuseNodeId, NodeType);

            Context.StepManifests.Add(StepId, MoveTemp(Manifest));
            Context.StepToNodeMap.Add(StepId, ReuseNodeId);
            Context.StepToNodePtr.Add(StepId, ResultNode);
            Context.CreatedNodeCount++;
            ReusedStepIds.Add(StepId);
            Context.ReusedStepIds.Add(StepId);

            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("  -> Virtual FunctionOutput step '%s' -> FunctionResult node '%s'"),
                *StepId, *ReuseNodeId);
            continue;
        }
```

**Critical**: Adding to `ReusedStepIds` ensures rollback does NOT delete
FunctionEntry/FunctionResult nodes. They are structural, not plan-created.

---

### 2G: Update All Callers -- Pass GraphContext

**File**: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

There are 4 call sites to update (2 for Resolve, 2 for Validate):

**Preview handler** (around line 6280):

Before the existing `Resolve()` call, add:
```cpp
FOliveGraphContext GraphContext = FOliveGraphContext::BuildFromBlueprint(Blueprint, GraphName);
```

Update line 6280:
```cpp
FOlivePlanResolveResult ResolveResult = FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint, GraphContext);
```

Update line 6316-6317:
```cpp
FOlivePlanValidationResult Phase0Result = FOlivePlanValidator::Validate(
    Plan, ResolveResult.ResolvedSteps, Blueprint, GraphContext);
```

**Apply handler** (around line 6613):

Before the existing `Resolve()` call, add:
```cpp
FOliveGraphContext GraphContext = FOliveGraphContext::BuildFromBlueprint(Blueprint, GraphName);
```

Update line 6613:
```cpp
FOlivePlanResolveResult ResolveResult = FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint, GraphContext);
```

Update line 6649-6650:
```cpp
FOlivePlanValidationResult Phase0Result = FOlivePlanValidator::Validate(
    Plan, ResolveResult.ResolvedSteps, Blueprint, GraphContext);
```

**Include**: Add to the tool handlers file:
```cpp
#include "Plan/OliveBlueprintPlanResolver.h"  // Already included (for FOliveBlueprintPlanResolver)
```

The include for `OliveBlueprintPlanResolver.h` should already exist since the file calls
`Resolve()` and `CollapseExecThroughPureSteps()`. Verify, but no new include is likely
needed since `FOliveGraphContext` is defined in the same header.

**File**: `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp`

**Function sub-plan** (around line 1140-1141):

Before the Resolve call, add:
```cpp
FOliveGraphContext FuncContext = FOliveGraphContext::BuildFromBlueprint(Blueprint, FuncName);
```

Update line 1140-1141:
```cpp
FOlivePlanResolveResult ResolveResult =
    FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint, FuncContext);
```

**Event graph sub-plan** (around line 1226-1227):

Before the Resolve call, add:
```cpp
FOliveGraphContext EGContext = FOliveGraphContext::BuildFromBlueprint(Blueprint, TEXT("EventGraph"));
```

Update line 1226-1227:
```cpp
FOlivePlanResolveResult ResolveResult =
    FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint, EGContext);
```

**Note**: The template system does NOT call `FOlivePlanValidator::Validate()` for sub-plans.
This is intentional -- template plans are authored by developers, not the AI. The latent
validation is primarily useful for AI-generated plans. However, if we want defense-in-depth,
add validation calls after Resolve in both template paths. This is OPTIONAL for Phase 2.

**Include**: Add at the top of OliveTemplateSystem.cpp if not already present:
```cpp
#include "Plan/OliveBlueprintPlanResolver.h"
```

Verify: the template system already calls `FOliveBlueprintPlanResolver::Resolve()`, so
the include should already exist.

---

### Phase 2 Build Verification

Run incremental build. Expected changes:
- New struct `FOliveGraphContext` in header (additive)
- New field `bIsLatent` on `FOliveResolvedStep` (additive)
- Changed signatures on `Resolve()`, `Validate()`, private resolver methods (all with defaults)
- New node type constants (additive)
- New code in `PhaseCreateNodes()` (additive)
- New validation check (additive)

Signature changes use default parameters (`= FOliveGraphContext()`) so existing callers
that do NOT pass GraphContext still compile. This makes the change backward-compatible.

---

## Phase 3: Lookup Table Fixes

### 3A: Float-to-Double Aliases

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp`
**Function**: `GetAliasMap()` at line 697

The alias map is a static `TMap<FString, FString>` built by a lambda (lines 699-933).
The last entry is at line 930 (`Map.Add(TEXT("GetVelocity"), TEXT("GetVelocity"));`).

Add a new section BEFORE the `return Map;` statement (before line 932):

```cpp
        // ================================================================
        // UE 5.5+ Float->Double Renames
        // ================================================================
        // In UE 5.5, many float math functions were renamed to use Double.
        // These aliases ensure plans written with old Float names still resolve.
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

**What NOT to change**: Existing aliases. Do not rename any existing entries.

---

### 3B: Node Factory Library Expansion

**File**: `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`
**Function**: `FindFunction()` at line 1063

**Add includes** at the top (after line 38, the existing utility includes):
```cpp
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
```

Note: `Kismet/KismetArrayLibrary.h` is already included at line 37. `GameFramework/Actor.h`
is already included at line 38.

**Update library classes** at lines 1098-1102:

Replace:
```cpp
TArray<UClass*> LibraryClasses = {
    UKismetSystemLibrary::StaticClass(),
    UObject::StaticClass(),
    AActor::StaticClass(),
};
```

With:
```cpp
TArray<UClass*> LibraryClasses = {
    UKismetSystemLibrary::StaticClass(),
    UKismetMathLibrary::StaticClass(),
    UKismetStringLibrary::StaticClass(),
    UKismetArrayLibrary::StaticClass(),
    UGameplayStatics::StaticClass(),
    UObject::StaticClass(),
    AActor::StaticClass(),
    USceneComponent::StaticClass(),
    UPrimitiveComponent::StaticClass(),
    APawn::StaticClass(),
    ACharacter::StaticClass(),
};
```

**Update warning log** at line 1115:
```cpp
UE_LOG(LogOliveNodeFactory, Warning,
    TEXT("FindFunction('%s', class='%s'): FAILED -- searched specified class + Blueprint GeneratedClass + "
         "library classes [KismetSystemLibrary, KismetMathLibrary, KismetStringLibrary, KismetArrayLibrary, "
         "GameplayStatics, Object, Actor, SceneComponent, PrimitiveComponent, Pawn, Character]"),
    *FunctionName, *ClassName);
```

---

### Phase 3 Build Verification

Run incremental build. Expected: clean compile. These are purely additive changes (new
aliases, new array entries, new includes). No existing code is modified.

---

## Risk Assessment

### High Risk
- **Phase 1C**: The Evaluate() modification is complex. The compile failure branch
  (lines 72-123) and tool failure branch (lines 126-187) both need the same pattern.
  Ensure the `bIsInGranularFallback` flag is checked consistently in both branches.
- **Phase 2C**: Changing `Resolve()` signature affects all callers. The default parameter
  (`= FOliveGraphContext()`) mitigates this, but verify that the template system and tool
  handlers both pass it correctly.

### Medium Risk
- **Phase 2F**: The virtual step insertion in PhaseCreateNodes must come BEFORE the normal
  creation block but AFTER the event reuse check. Incorrect placement would skip event
  reuse or bypass virtual step handling.
- **Phase 1E**: The template result modification must not break the existing result
  structure. The `compiled` bool field is already there; we're adding `compile_result`
  alongside it, not replacing it.

### Low Risk
- **Phase 3**: Pure data additions. Cannot break anything.
- **Phase 2E**: Adding constants to a namespace. Purely additive.

---

## Implementation Order

1. **Phase 1A** -- Retry policy (trivial, 5 min)
2. **Phase 1B** -- State tracking fields (trivial, 10 min)
3. **Phase 1D** -- Message builders (standalone, no dependencies, 15 min)
4. **Phase 1C** -- Core Evaluate() modification (depends on 1A, 1B, 1D -- 30 min)
5. **Phase 1E** -- Template compile error propagation (standalone, 15 min)
6. **Phase 1F** -- Tool failure rollback handling (depends on 1C pattern, 15 min)
7. **BUILD + VERIFY Phase 1**
8. **Phase 2A** -- Graph context struct (standalone, 20 min)
9. **Phase 2B** -- bIsLatent field (standalone, 10 min)
10. **Phase 2E** -- NodeType constants (standalone, 5 min)
11. **Phase 2C** -- Thread context through resolver (depends on 2A, 2B -- 30 min)
12. **Phase 2D** -- Latent-in-function validation (depends on 2A, 2B -- 20 min)
13. **Phase 2F** -- Executor virtual steps (depends on 2E -- 20 min)
14. **Phase 2G** -- Update callers (depends on 2A, 2C, 2D -- 15 min)
15. **BUILD + VERIFY Phase 2**
16. **Phase 3A** -- Float-to-Double aliases (standalone, 10 min)
17. **Phase 3B** -- Library expansion (standalone, 10 min)
18. **BUILD + VERIFY Phase 3**

Total estimated time: ~4 hours

---

## Error Code Summary

| Code | Phase | Meaning |
|------|-------|---------|
| `LATENT_IN_FUNCTION` | 2D | Latent call (Delay, AI MoveTo) in function graph |
| `NODE_CREATION_FAILED` | 2F | FunctionEntry/FunctionResult not found (reused existing code) |

No new error codes needed for Phase 1 (uses existing FeedBackErrors/StopWorker actions)
or Phase 3 (data additions only).

---

## Files Modified Summary

### Phase 1 (4 files)
| File | Changes |
|------|---------|
| `Public/Brain/OliveRetryPolicy.h` | Raise MaxCorrectionCyclesPerWorker to 20, add bAllowGranularFallback |
| `Public/Brain/OliveSelfCorrectionPolicy.h` | Add bIsInGranularFallback, LastPlanFailureReason, 2 new method declarations; add param to HasCompileFailure |
| `Private/Brain/OliveSelfCorrectionPolicy.cpp` | Rollback extraction, fallback switch in compile+tool failure branches, Reset() update, 2 new message builders |
| `Blueprint/Private/Template/OliveTemplateSystem.cpp` | Add compile_result to result JSON, conditional success message |

### Phase 2 (7 files)
| File | Changes |
|------|---------|
| `Blueprint/Public/Plan/OliveBlueprintPlanResolver.h` | FOliveGraphContext struct + builder, bIsLatent on FOliveResolvedStep, updated signatures |
| `Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | BuildFromBlueprint(), function param detection in GetVar/SetVar, latent flag, context threading |
| `Blueprint/Public/Plan/OlivePlanValidator.h` | Updated Validate() signature, forward decl, CheckLatentInFunctionGraph decl |
| `Blueprint/Private/Plan/OlivePlanValidator.cpp` | CheckLatentInFunctionGraph implementation |
| `Blueprint/Public/Writer/OliveNodeFactory.h` | FunctionInput/FunctionOutput constants |
| `Blueprint/Private/Plan/OlivePlanExecutor.cpp` | K2Node_FunctionResult include, virtual step blocks in PhaseCreateNodes |
| `Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Build + pass GraphContext at 4 call sites |
| `Blueprint/Private/Template/OliveTemplateSystem.cpp` | Build + pass GraphContext at 2 Resolve() call sites |

### Phase 3 (2 files)
| File | Changes |
|------|---------|
| `Blueprint/Private/Plan/OliveFunctionResolver.cpp` | 10 Float-to-Double alias additions |
| `Blueprint/Private/Writer/OliveNodeFactory.cpp` | 7 new includes, 8 new library classes, updated warning log |
