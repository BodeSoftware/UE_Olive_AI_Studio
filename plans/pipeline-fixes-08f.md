# Pipeline Fixes -- Session 08f Issues

**Date:** 2026-03-08
**Status:** Ready for implementation
**Files modified:** 4 existing files, 0 new files

---

## Implementation Order

1. **Issue 2** -- Compile tool bug (highest impact: causes 6-retry cascade)
2. **Issue 1** -- Planner knowledge injection
3. **Issue 5** -- Stale node IDs after rollback
4. **Issue 4** -- `remove_function` force parameter
5. **Issue 3** -- Pipeline status messages

---

## Issue 2: Compile Tool Returns SUCCESS on Failure

### Root Cause

`HandleBlueprintCompile` at line 2303 of `OliveBlueprintToolHandlers.cpp`:
```cpp
// Return success even if compilation had errors (the errors are in the result)
return FOliveToolResult::Success(ResultData);
```

The comment is intentional but wrong. When the compile tool returns SUCCESS, the Builder sees "compilation succeeded" and proceeds, never reading the error details buried in `ResultData`. This triggers the 6-retry cascade observed in session 08f.

### File

`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

### Change

Replace lines 2296-2305 (the compile result section) with logic that checks `CompileResult.bSuccess`:

```cpp
// Compile directly using FOliveCompileManager (no write pipeline needed)
FOliveCompileManager& CompileManager = FOliveCompileManager::Get();
FOliveIRCompileResult CompileResult = CompileManager.Compile(Blueprint);

// Build result data
TSharedPtr<FJsonObject> ResultData = CompileResult.ToJson();

// Return failure if compilation had errors, so the Builder knows to fix them
if (!CompileResult.bSuccess)
{
    // Build a human-readable error summary for the Builder
    FString ErrorSummary = FString::Printf(
        TEXT("Compilation FAILED with %d error(s)."),
        CompileResult.Errors.Num());

    // Include first 5 error messages for immediate visibility
    const int32 MaxErrors = FMath::Min(CompileResult.Errors.Num(), 5);
    for (int32 i = 0; i < MaxErrors; ++i)
    {
        ErrorSummary += TEXT("\n  - ") + CompileResult.Errors[i].Message;
    }
    if (CompileResult.Errors.Num() > MaxErrors)
    {
        ErrorSummary += FString::Printf(
            TEXT("\n  ... and %d more error(s). See 'errors' array in result data."),
            CompileResult.Errors.Num() - MaxErrors);
    }

    FOliveToolResult FailResult = FOliveToolResult::Error(
        TEXT("COMPILE_FAILED"),
        ErrorSummary,
        TEXT("Fix the reported errors and compile again"));
    FailResult.ResultData = ResultData;  // Full error details still available
    return FailResult;
}

return FOliveToolResult::Success(ResultData);
```

### Key Behaviors
- `CompileResult.bSuccess == false` --> `FOliveToolResult::Error()` with `COMPILE_FAILED` code
- Error summary includes up to 5 error messages inline so the Builder sees them without parsing JSON
- Full `ResultData` (with all errors, warnings, timing) still attached for structured access
- `CompileResult.bSuccess == true` --> unchanged `FOliveToolResult::Success()`

### Verification
- Compile a BP with known errors (e.g., unwired exec pin) and confirm the tool returns FAILED
- Compile a clean BP and confirm it still returns SUCCESS

---

## Issue 1: Planner Knowledge Deficit

### Root Cause

The Planner gets a 15-line hardcoded "Blueprint Pattern Knowledge" block (~700 chars) with bullet points. The Builder gets ~29.5KB of knowledge packs (`events_vs_functions.txt`, `blueprint_design_patterns.txt`, etc.) via AGENTS.md. The Planner makes architectural decisions (function vs event, separate actor vs component, interface vs cast) without the knowledge to decide correctly.

### File

`Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp`

### Change A: Add helper function to load knowledge pack files

Add a new helper function in the anonymous namespace (around line 40, after existing helpers):

```cpp
/**
 * Load a knowledge pack file from the plugin's Content/SystemPrompts/Knowledge/ directory.
 * Returns empty string on failure (caller logs the warning).
 */
FString LoadKnowledgePack(const FString& Filename)
{
    // Resolve plugin content directory
    const FString PluginContentDir = FPaths::Combine(
        IPluginManager::Get().FindPlugin(TEXT("UE_Olive_AI_Studio"))->GetBaseDir(),
        TEXT("Content/SystemPrompts/Knowledge"));

    const FString FilePath = FPaths::Combine(PluginContentDir, Filename);

    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *FilePath))
    {
        UE_LOG(LogOliveAgentPipeline, Warning,
            TEXT("Failed to load knowledge pack: %s"), *FilePath);
        return FString();
    }
    return Content;
}

/**
 * Load blueprint_design_patterns.txt but only sections 0-3.
 * Truncates at the "## 4." header (section 4+ is execution-level detail, not planning).
 */
FString LoadDesignPatternsSections0To3()
{
    FString Full = LoadKnowledgePack(TEXT("blueprint_design_patterns.txt"));
    if (Full.IsEmpty()) return Full;

    // Find the "## 4." section header and truncate
    int32 Section4Pos = Full.Find(TEXT("\n## 4."), ESearchCase::CaseSensitive);
    if (Section4Pos != INDEX_NONE)
    {
        Full.LeftInline(Section4Pos);
    }
    return Full;
}
```

Note: Add `#include "Interfaces/IPluginManager.h"` to the includes if not already present.

### Change B: Replace hardcoded block in `RunPlannerWithTools()`

In `RunPlannerWithTools()`, **replace lines 2148-2167** (the hardcoded "Blueprint Pattern Knowledge" block) with:

```cpp
// Inject comprehensive knowledge packs from disk (same knowledge the Builder gets)
{
    FString EventsVsFunctions = LoadKnowledgePack(TEXT("events_vs_functions.txt"));
    FString DesignPatterns = LoadDesignPatternsSections0To3();

    if (!EventsVsFunctions.IsEmpty() || !DesignPatterns.IsEmpty())
    {
        PlannerUserPrompt += TEXT("\n## Blueprint Architecture Knowledge\n\n");

        if (!EventsVsFunctions.IsEmpty())
        {
            // Strip the TAGS: header line if present (not useful for the Planner)
            int32 TagsEnd = EventsVsFunctions.Find(TEXT("\n---\n"));
            if (TagsEnd != INDEX_NONE)
            {
                EventsVsFunctions.RightChopInline(TagsEnd + 5);
            }
            PlannerUserPrompt += EventsVsFunctions;
            PlannerUserPrompt += TEXT("\n");
        }

        if (!DesignPatterns.IsEmpty())
        {
            PlannerUserPrompt += DesignPatterns;
            PlannerUserPrompt += TEXT("\n");
        }
    }
    else
    {
        // Fallback: if files couldn't be loaded, keep minimal inline knowledge
        PlannerUserPrompt += TEXT("\n## Blueprint Architecture Knowledge\n\n");
        PlannerUserPrompt += TEXT("- USE A FUNCTION when: returns a value AND logic is synchronous\n");
        PlannerUserPrompt += TEXT("- USE AN EVENT when: logic spans multiple frames OR no return value needed\n");
        PlannerUserPrompt += TEXT("- Functions CANNOT contain Timeline, Delay, or latent actions\n");
        PlannerUserPrompt += TEXT("- Interface: no outputs = implementable event, has outputs = synchronous function\n");
    }

    // Keep compact ops reference inline (not in the knowledge pack files)
    PlannerUserPrompt += TEXT("\n### Plan JSON Ops Reference\n");
    PlannerUserPrompt += TEXT("- Dispatchers: use `call_delegate` to fire, `bind_dispatcher` to bind, `call_dispatcher` to call\n");
    PlannerUserPrompt += TEXT("- Overlap events: `{\"op\":\"event\",\"target\":\"OnComponentBeginOverlap\",\"properties\":{\"component_name\":\"CompName\"}}`\n");
    PlannerUserPrompt += TEXT("- Interface calls: use `target_class` with the interface name\n");
    PlannerUserPrompt += TEXT("- Enhanced Input: `{\"op\":\"event\",\"target\":\"IA_ActionName\"}`\n");
    PlannerUserPrompt += TEXT("- Timeline nodes require EventGraph -- use Custom Events, not functions\n");
    PlannerUserPrompt += TEXT("- plan_json creates NEW nodes only -- cannot reference existing nodes by ID\n");
    PlannerUserPrompt += TEXT("- For binding to dispatchers, use granular add_node (K2Node_AssignDelegate), not plan_json\n\n");
}
```

### Change C: Add the same injection to `RunPlanner()`

In `RunPlanner()`, after the template overviews block (after line 1982), insert the same knowledge injection. Use the same pattern as Change B. Insert before the `SendAgentCompletion()` call at line 1985.

### Token budget note

`events_vs_functions.txt` is 42 lines / ~2.5KB. Sections 0-3 of `blueprint_design_patterns.txt` are ~150 lines / ~6KB. Total injection is ~8.5KB -- roughly 2K tokens. This replaces ~700 chars of inline knowledge. The Planner's context typically has 4-8K of task + assets + templates, so this brings it to ~12-16K total. Well within the 30s / 2048-token response budget for the Architect role.

---

## Issue 5: Stale Node IDs After Rollback

### Root Cause

When `plan_json` fails (e.g., data wire failure triggers `bHasDataWireFailure = true`), the executor rolls back via `FScopedTransaction` and the `PLAN_EXECUTION_FAILED` error message says "Plan execution failed: N of M nodes created" but does NOT warn that the node IDs from `step_to_node_map` are now invalid. The Builder still references these IDs in subsequent `get_node_pins`, `connect_pins`, and `remove_node` calls.

### File

`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

### Change

At the `PLAN_EXECUTION_FAILED` error path (lines 8276-8286), append the stale-ID warning to the error message:

```cpp
if (!PlanResult.bSuccess)
{
    // Node creation failed entirely -- transaction rolled back, all node IDs are invalid
    FString ErrorMsg = FString::Printf(
        TEXT("Plan execution failed: %d of %d nodes created. "
             "IMPORTANT: All nodes from this plan_json call have been ROLLED BACK and removed from the graph. "
             "Node IDs from step_to_node_map are NO LONGER VALID. "
             "Call blueprint.read on the target function/graph to get current node IDs before "
             "referencing any nodes."),
        static_cast<int32>(PlanResult.StepToNodeMap.Num()),
        CapturedPlan.Steps.Num());

    FOliveWriteResult ErrorResult = FOliveWriteResult::ExecutionError(
        TEXT("PLAN_EXECUTION_FAILED"),
        ErrorMsg,
        PlanResult.Errors.Num() > 0 ? PlanResult.Errors[0].Suggestion : TEXT(""));
    ErrorResult.ResultData = ResultData;
    return ErrorResult;
}
```

Also, when `bHasDataWireFailure` is true, the `PlanResult.bSuccess` is false but `StepToNodeMap` is still populated with the now-invalid IDs. Remove or mark these in the `ResultData` to prevent confusion:

In the `ResultData` assembly section (before the `!PlanResult.bSuccess` check), add a field when the plan failed:

```cpp
if (!PlanResult.bSuccess)
{
    // Clear step_to_node_map from ResultData -- these IDs are invalid after rollback
    ResultData->SetStringField(TEXT("rollback_warning"),
        TEXT("All nodes were rolled back. Node IDs in step_to_node_map are INVALID."));
}
```

This goes just before the existing `if (!PlanResult.bSuccess)` block at line 8276.

### Additional hardening

Check if `ResultData` includes `step_to_node_map` in the failure path. If so, either remove it or rename it to `rolled_back_node_ids` to make the invalidity obvious. The coder should verify by reading the ResultData assembly code above line 8276.

---

## Issue 4: `remove_function` Safety Guard

### Root Cause

The guard at lines 4076-4108 blocks removal of functions with >2 nodes. The `force` parameter is already parsed at line 4078-4079 (`Params->TryGetBoolField(TEXT("force"), bForce)`), and the guard already checks `!bForce`. The error message at line 4101 already mentions `force: true`.

**Wait -- re-reading the code, the `force` parameter is ALREADY implemented.** Lines 4077-4079 parse it, line 4081 checks `!bForce`. The error message at line 4102 already says "pass 'force': true to override this safety check".

### Actual issue

The schema at `OliveBlueprintSchemas::BlueprintRemoveFunction()` (line 774-790) does NOT include the `force` parameter. So while the handler code accepts `force`, the schema sent to the AI does not advertise it. The AI never knows it can pass `force: true`.

### File

`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`

### Change

In `BlueprintRemoveFunction()` (around line 782), add the `force` property to the schema:

```cpp
Properties->SetObjectField(TEXT("path"),
    StringProp(TEXT("Blueprint asset path")));

Properties->SetObjectField(TEXT("name"),
    StringProp(TEXT("Function name to remove")));

Properties->SetObjectField(TEXT("force"),
    BoolProp(TEXT("Force removal even if the function has graph logic. "
        "Default false. When false, removal is blocked if the function "
        "has more than entry+return nodes. Use true when you intend "
        "to recreate the function with different logic.")));
```

Check that `BoolProp()` exists as a helper in the schemas file (similar to `StringProp()`). If not, use:
```cpp
// Manual bool property
TSharedPtr<FJsonObject> ForceProp = MakeShareable(new FJsonObject());
ForceProp->SetStringField(TEXT("type"), TEXT("boolean"));
ForceProp->SetStringField(TEXT("description"), TEXT("Force removal even if ..."));
Properties->SetObjectField(TEXT("force"), ForceProp);
```

### Verification
- Call `tools/list` and confirm `blueprint.remove_function` schema includes `force` parameter
- Call `blueprint.remove_function` with `force: false` on a function with logic -- should be blocked
- Call with `force: true` -- should succeed

---

## Issue 3: Unresponsive Pipeline Startup (~2 min silence)

### Root Cause

The agent pipeline runs synchronously inside `SendMessageAutonomous()` at line 590 of `OliveCLIProviderBase.cpp`. The entire `Pipeline.Execute()` call (Scout 15s + Planner 102s = 117s) blocks before `LaunchCLIProcess()`. During this time, `OnChunk` is never called, so the chat UI shows nothing.

### Approach

Post status messages through the existing `OnChunk` callback. The `FOnOliveStreamChunk` callback is already wired up and broadcasting to `OnStreamChunk` which the chat panel listens to. We can emit synthetic stream chunks with status text during pipeline phases.

The key insight: `SendMessageAutonomous()` stores `OnChunk` in `CurrentOnChunk` at line 484. After that, we can call it directly. However, the pipeline runs BEFORE `LaunchCLIProcess()`, so we need to emit status chunks during the pipeline itself.

### File

`Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

### Change A: Add a status emission helper

Add a private helper method (or local lambda) that emits a status chunk:

```cpp
// In the anonymous namespace or as a local lambda inside SendMessageAutonomous:
auto EmitStatus = [&](const FString& StatusText)
{
    FOliveStreamChunk StatusChunk;
    StatusChunk.Text = StatusText + TEXT("\n");
    StatusChunk.bIsFirst = false;
    if (CurrentOnChunk.IsBound())
    {
        CurrentOnChunk.Execute(StatusChunk);
    }
};
```

### Change B: Emit status messages around pipeline phases

In `SendMessageAutonomous()`, around lines 585-622 (the pipeline execution block), wrap with status emissions:

```cpp
if (!bIsContinuation && MessageImpliesMutation(EffectiveMessage))
{
    // Emit pipeline start status
    EmitStatus(TEXT("*Analyzing task and searching project for relevant assets...*"));

    FOliveAgentPipeline Pipeline;
    CachedPipelineResult = Pipeline.Execute(EffectiveMessage, PipelineContextAssets);

    if (CachedPipelineResult.bValid)
    {
        EmitStatus(TEXT("*Build plan ready. Launching builder...*"));
        // ... existing injection code ...
    }
    else
    {
        EmitStatus(TEXT("*Planning complete. Launching builder...*"));
        // ... existing fallback code ...
    }
}
```

### Change C (optional, higher effort): Phase-level status from inside the pipeline

For finer-grained feedback (showing "Planning architecture..." separately from "Searching..."), we could add an `FOnOliveStreamChunk` callback parameter to `FOliveAgentPipeline::Execute()` and `ExecuteCLIPath()`. This is more intrusive.

**Recommendation:** Start with Change B only (two status messages: start and end). This eliminates the 117s of total silence with minimal code change. If users want per-phase updates, we can add Change C as a follow-up.

### Alternative approach considered: `OnMessageAdded` delegate

We could post `FOliveChatMessage` with `EOliveChatRole::System` via `ConversationManager->AddMessage()`. However, `SendMessageAutonomous` runs on `OliveCLIProviderBase` which does not have a reference to the ConversationManager. The `OnChunk` approach is simpler because the callback is already available.

### UI rendering note

The status text uses markdown italics (`*...*`) so the chat panel renders it as a distinct status message rather than regular assistant text. The coder should verify that the chat panel's markdown renderer handles this correctly. If not, plain text is fine too -- the important thing is that SOMETHING appears.

### Verification
- Send a mutation message and confirm status text appears in the chat panel within 1-2 seconds
- Confirm the status text does not corrupt the actual response (it's prepended to the streaming content)

---

## File Change Summary

| File | Issue(s) | Changes |
|------|----------|---------|
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | 2, 5 | Compile tool error propagation; rollback warning in apply_plan_json |
| `Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp` | 1 | Knowledge pack loading helpers; injection in RunPlanner() and RunPlannerWithTools() |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` | 4 | Add `force` param to remove_function schema |
| `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | 3 | Status message emission during pipeline |

---

## Coder Instructions

1. **Start with Issue 2** (compile tool). It is a 10-line change in one location. Build and test with a BP that has a known error.

2. **Issue 1** (knowledge injection). Add the helper functions in the anonymous namespace. Replace the hardcoded block in `RunPlannerWithTools()`. Add the same injection to `RunPlanner()`. Verify `IPluginManager` include is available (it should be, but check). Build.

3. **Issue 5** (stale IDs). Modify the `PLAN_EXECUTION_FAILED` error message. Check the ResultData assembly code above line 8276 to see if `step_to_node_map` is populated in the failure path -- if so, add the `rollback_warning` field. Build.

4. **Issue 4** (remove_function schema). Check if `BoolProp()` helper exists in OliveBlueprintSchemas.cpp. Add the `force` property. Build and verify via `tools/list`.

5. **Issue 3** (status messages). Add the `EmitStatus` lambda inside `SendMessageAutonomous()`, after `CurrentOnChunk` is set (line 484). Add the two status emissions around `Pipeline.Execute()`. Build and test end-to-end.

6. **Final build** and spot-check all 5 fixes.
