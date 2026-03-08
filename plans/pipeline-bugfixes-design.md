# Pipeline Bugfixes Design

Five targeted fixes in existing code. No new files, no new modules.

---

## Bug 1: `@entry.~Param` creates broken get_var nodes

### Root Cause

The `~` prefix is an executor-level convention for sub-pin suffix hints (e.g., `@step.~X` means "connect to the X sub-pin after splitting"). When the AI writes `@entry.~Instigator`, the resolver's `ExpandComponentRefs()` extracts `PinHint = "~Instigator"` at line 828. It tries to match `~Instigator` against `GraphContext.InputParamNames`, which contains `Instigator` (no tilde). No match, so `ParamTarget` stays `~Instigator`. The synthesized get_var step gets `Target = "~Instigator"`, which doesn't exist. The executor creates a GetVariable node for a nonexistent variable, producing 0 output pins and cascading wiring failures.

### File

`Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` -- `ExpandComponentRefs()`, lines 828-880

### Change

After extracting `PinHint` at line 828, strip a leading `~` before the parameter name match loop. Use the clean name for `ParamTarget`, `SynthKey`, and `SynthStepId`. Keep the rewritten reference as `@synthId.auto` (unchanged from current behavior -- the `.auto` pin resolution on the FunctionInput node will find the correct output pin).

```cpp
FString PinHint = RefBody.Mid(DotIndex + 1);

// NEW: Strip tilde prefix for parameter name resolution.
// The tilde is an executor-level sub-pin hint, not part of the param name.
FString ParamLookup = PinHint;
if (ParamLookup.StartsWith(TEXT("~")))
{
    ParamLookup = ParamLookup.Mid(1);
}

// Resolve the parameter name to canonical casing from GraphContext
FString ParamTarget = ParamLookup;  // was: PinHint
for (const FString& ParamName : GraphContext.InputParamNames)
{
    if (ParamName.Equals(ParamLookup, ESearchCase::IgnoreCase))  // was: PinHint
    {
        ParamTarget = ParamName;
        break;
    }
}
```

All downstream uses of `ParamTarget` (SynthKey at line 843, SynthStepId at line 847, SynthStep.Target at line 855) automatically get the clean name. Line 879 (`@synthId.auto`) stays unchanged.

Result: `@entry.~Instigator` produces synth step `_synth_param_instigator` with `Target="Instigator"`, which correctly matches in `ResolveGetVarOp()` and resolves to `FunctionInput`.

---

## Bug 2: `set_var` for nonexistent variables should be a hard error

### Root Cause

In `ResolveSetVarOp()` (lines 2034-2149), when `BlueprintHasVariable()` returns false for the current BP, all parents, and the native GeneratedClass, the code emits a **warning** (line 2140) and returns `true`, allowing execution to proceed. The executor creates a SetVariable node with `SetSelfMember` for a nonexistent variable, which compiles to an error or produces broken wiring. Wastes a Builder turn.

### File

`Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` -- `ResolveSetVarOp()`, lines 2134-2149

### Change

Replace the warning-and-continue `else` block (lines 2134-2149) with a hard error that lists available variables:

```cpp
else
{
    // Variable not found on this Blueprint, parents, or generated class.
    TArray<FString> AvailableVars;
    for (const FBPVariableDescription& Var : BP->NewVariables)
    {
        AvailableVars.Add(Var.VarName.ToString());
    }

    FString VarListStr = AvailableVars.Num() > 0
        ? FString::Join(AvailableVars, TEXT(", "))
        : TEXT("(none)");

    Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
        TEXT("VARIABLE_NOT_FOUND"),
        Step.StepId,
        FString::Printf(TEXT("/steps/%d/target"), Idx),
        FString::Printf(
            TEXT("Variable '%s' does not exist on Blueprint '%s'. "
                 "Available variables: %s"),
            *Step.Target, *BP->GetName(), *VarListStr),
        TEXT("Check the variable name or add the variable first "
             "with blueprint.add_variable")));

    return false;
}
```

Keep the existing paths above it unchanged:
- Component match (lines 2050-2117) -- already returns false with COMPONENT_NOT_VARIABLE
- Native C++ property on GeneratedClass (lines 2122-2133) -- proceeds normally

### Safety

Variables added via `blueprint.add_variable` are written to `NewVariables` directly, so they will be found even before compilation. Inherited variables are checked in the parent walk above this code.

---

## Bug 3: `connect_pins` blank error codes -- safety net in ToToolResult

### Root Cause

The connect_pins pipeline has correct error code propagation in the executor lambda (lines 4798-4838 in OliveBlueprintToolHandlers.cpp set `BP_CONNECT_PINS_INCOMPATIBLE` or `BP_CONNECT_PINS_FAILED`). However, `FOliveWriteResult::ToToolResult()` (OliveWritePipeline.cpp line 32-41) blindly copies `ValidationMessages` to `Messages`. If a pipeline stage produces a failure with `ResultData` containing error info but empty `ValidationMessages`, the `FOliveToolResult::ToJson()` method (OliveToolRegistry.cpp lines 619-630) finds no `Messages[0].Code` and serializes an empty error block.

This can happen when:
- `FOliveWriteResult::ValidationError()` is called with a `FOliveValidationResult` whose messages lack codes
- A pipeline stage manually constructs an error result without populating both `ResultData` and `ValidationMessages`

### File

`Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp` -- `ToToolResult()`, lines 32-41

### Change

Add a safety net after the existing code: if the result is a failure with no Messages but has ResultData with error info, synthesize a message:

```cpp
FOliveToolResult FOliveWriteResult::ToToolResult() const
{
    FOliveToolResult ToolResult;
    ToolResult.bSuccess = bSuccess;
    ToolResult.Data = ResultData;
    ToolResult.Messages = ValidationMessages;
    ToolResult.ExecutionTimeMs = ExecutionTimeMs;

    // Safety net: if failure has no messages but has ResultData with error info,
    // synthesize a message so ToJson() produces a structured error block.
    if (!bSuccess && ToolResult.Messages.Num() == 0 && ResultData.IsValid())
    {
        FString Code, Message, Suggestion;
        ResultData->TryGetStringField(TEXT("error_code"), Code);
        ResultData->TryGetStringField(TEXT("error_message"), Message);
        ResultData->TryGetStringField(TEXT("suggestion"), Suggestion);

        if (!Message.IsEmpty())
        {
            FOliveIRMessage SynthMsg;
            SynthMsg.Severity = EOliveIRSeverity::Error;
            SynthMsg.Code = Code.IsEmpty() ? TEXT("PIPELINE_ERROR") : Code;
            SynthMsg.Message = Message;
            SynthMsg.Suggestion = Suggestion;
            ToolResult.Messages.Add(MoveTemp(SynthMsg));
        }
    }

    return ToolResult;
}
```

This is defensive -- it won't trigger in the normal connect_pins path (which correctly populates ValidationMessages via `ExecutionError()`), but catches any pipeline path that sets ResultData without also setting ValidationMessages.

---

## Bug 4: `add_node type="CallFunction"` can't find BP-local functions

### Root Cause

`FindFunction()` Step 2b (lines 2106-2142 of OliveNodeFactory.cpp) searches `Blueprint->FunctionGraphs` for a matching graph name, then looks up the UFunction on `SkeletonGeneratedClass` and `GeneratedClass`. When the function was recently created (e.g., via `blueprint.add_function` earlier in the same session) but the Blueprint hasn't been compiled, neither class has the UFunction yet. The code logs "Blueprint may need compilation" and falls through to search parent/SCS/interfaces/libraries -- all fail. `FindFunction` returns null.

### File

`Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` -- `FindFunction()`, Step 2b (lines 2133-2139)

### Change

Replace the "log and continue" `else` block (lines 2133-2139) with a node refresh + retry:

```cpp
else
{
    // Function graph exists but no UFunction on skeleton.
    // Blueprint likely hasn't been compiled since this function was created.
    // Refresh nodes to regenerate the skeleton class with function stubs.
    UE_LOG(LogOliveNodeFactory, Log,
        TEXT("FindFunction: Found function graph '%s' but no UFunction "
             "-- refreshing nodes to update skeleton"),
        *ResolvedName);

    FBlueprintEditorUtils::RefreshAllNodes(Blueprint);

    // Retry lookup after refresh
    if (Blueprint->SkeletonGeneratedClass)
    {
        FoundFunction = Blueprint->SkeletonGeneratedClass->FindFunctionByName(
            FName(*ResolvedName));
    }
    if (!FoundFunction && Blueprint->GeneratedClass)
    {
        FoundFunction = Blueprint->GeneratedClass->FindFunctionByName(
            FName(*ResolvedName));
    }
    if (FoundFunction)
    {
        UE_LOG(LogOliveNodeFactory, Log,
            TEXT("FindFunction('%s'): found after node refresh"),
            *ResolvedName);
        ReportMatch(EOliveFunctionMatchMethod::FunctionGraph);
        return FoundFunction;
    }
    else
    {
        UE_LOG(LogOliveNodeFactory, Warning,
            TEXT("FindFunction: Node refresh did not produce UFunction "
                 "for '%s'. Continuing search."),
            *ResolvedName);
    }
}
```

`FBlueprintEditorUtils::RefreshAllNodes()` is already used in this codebase (`OliveBlueprintWriter.cpp:390`). It regenerates the skeleton class without a full compile. Only triggers when a function graph exists but UFunction doesn't -- not on every FindFunction call.

### Edge Cases

- **Recursive calls**: Both function stubs should appear after one RefreshAllNodes pass.
- **Performance**: RefreshAllNodes is lightweight (~10-50ms). Only triggers on the specific "graph found, UFunction missing" path.
- **Thread safety**: Already on game thread via MCP dispatch.

---

## Bug 5: `remove_node` response should hint about stale IDs

### Root Cause

The reader (`OliveGraphReader::BuildNodeIdMap()`) generates sequential IDs from scratch on every read by iterating `Graph->Nodes`. After removing a node, remaining nodes shift indices. Example: remove `node_1` from `[node_0, node_1, node_2]` -- next read produces `[node_0, node_1]` where `node_1` is the former `node_2`. Writer-cached IDs (from `add_node`) are stable, but reader-generated IDs shift.

### File

`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` -- `HandleBlueprintRemoveNode()`, success result block (after line 4624)

### Change

Add one line after the existing `message` field:

```cpp
ResultData->SetStringField(TEXT("note"),
    TEXT("Node indices from blueprint.read will have shifted. "
         "Re-read the graph before referencing other nodes by index."));
```

---

## Implementation Order

1. **Bug 5** (remove_node note) -- 1 line, zero risk
2. **Bug 1** (@entry.~Param tilde strip) -- ~5 lines in ExpandComponentRefs
3. **Bug 2** (set_var hard error) -- ~15 lines replacing warning block in ResolveSetVarOp
4. **Bug 3** (ToToolResult safety net) -- ~15 lines defensive addition in ToToolResult
5. **Bug 4** (FindFunction skeleton refresh) -- ~20 lines replacing log-and-continue in FindFunction

All five bugs are in different methods/files and can be implemented independently. Bugs 1 and 2 are both in `OliveBlueprintPlanResolver.cpp` but in separate methods. No IR schema, tool schema, or public interface changes.
