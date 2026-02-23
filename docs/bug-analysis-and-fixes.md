# Bug Analysis & Fixes — Feb 23 2026

## Two Issues Found

---

## Issue 1: Claude CLI Testing — Cascading Self-Correction Failure

### What Happened

The log shows Claude Code sent **17 tool calls in a single batch** for "create a gun blueprint that shoots bullets". The very first two (`blueprint.create` tc_1 and tc_2) failed with `VALIDATION_INVALID_VALUE` — meaning Claude sent an invalid `type` parameter (not one of: `Normal`, `Interface`, `FunctionLibrary`, `MacroLibrary`, `AnimationBlueprint`, `WidgetBlueprint`).

Because `blueprint.create` failed, the blueprints `/Game/Blueprints/BP_Gun` and `/Game/Blueprints/BP_Bullet` were never created. **Every subsequent tool call** (tc_3 through tc_17) then failed with `ASSET_NOT_FOUND` because they all depend on those blueprints existing.

### The Real Bug: Shared `CurrentAttemptCount` Across All Tool Calls

The `FOliveSelfCorrectionPolicy` has a **single** `CurrentAttemptCount` that increments on *every* failed tool call, regardless of which tool failed or why. With `MaxRetriesPerError = 3`:

```
tc_1  blueprint.create   VALIDATION_INVALID_VALUE  → attempt 1/3 (feed back)
tc_2  blueprint.create   VALIDATION_INVALID_VALUE  → attempt 2/3 (feed back)
tc_3  add_component      ASSET_NOT_FOUND           → attempt 3/3 (feed back)
tc_4  add_component      ASSET_NOT_FOUND           → attempt 4/3 → LOOP DETECTED → StopWorker
tc_5+ ALL remaining                                → loop detected, stop, stop, stop...
```

The counter hit 3 by tc_3 and triggered loop detection by tc_4. But the ASSET_NOT_FOUND errors on tc_3/tc_4 are a **different root cause** than the VALIDATION_INVALID_VALUE on tc_1/tc_2. The system treats unrelated errors as retries of the same problem.

Additionally, once `Brain->CompleteRun(Failed)` fires (at tc_4), the run is already in Error state, but the remaining tc_5–tc_17 **continue executing anyway** — each one re-triggering `CompleteRun` and logging redundant "Brain: Run completed... outcome=2" messages.

### Fix: Per-Signature Counting (Not Global)

**File: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`**

The `CurrentAttemptCount` should NOT be a single global counter. The `LoopDetector` already tracks per-signature attempt counts — the policy should use those instead of its own counter.

```cpp
// BEFORE (buggy):
FOliveCorrectionDecision FOliveSelfCorrectionPolicy::Evaluate(
    const FString& ToolName,
    const FString& ResultJson,
    FOliveLoopDetector& LoopDetector,
    const FOliveRetryPolicy& Policy)
{
    // ...tool failure branch...
    CurrentAttemptCount++;                    // ← GLOBAL counter, shared across all tools
    Decision.AttemptNumber = CurrentAttemptCount;
    Decision.MaxAttempts = Policy.MaxRetriesPerError;

    const FString Signature = FOliveLoopDetector::BuildToolErrorSignature(ToolName, ErrorCode, TEXT(""));
    LoopDetector.RecordAttempt(Signature, ...);

    if (LoopDetector.IsLooping(Signature, Policy) || ...)
    {
        Decision.Action = EOliveCorrectionAction::StopWorker;
        // ...
    }
```

```cpp
// AFTER (fixed):
FOliveCorrectionDecision FOliveSelfCorrectionPolicy::Evaluate(
    const FString& ToolName,
    const FString& ResultJson,
    FOliveLoopDetector& LoopDetector,
    const FOliveRetryPolicy& Policy)
{
    // ...tool failure branch...
    const FString Signature = FOliveLoopDetector::BuildToolErrorSignature(ToolName, ErrorCode, TEXT(""));
    LoopDetector.RecordAttempt(Signature, ...);

    // Use PER-SIGNATURE count from LoopDetector, not the global counter
    int32 SignatureAttempts = LoopDetector.GetAttemptCount(Signature);
    Decision.AttemptNumber = SignatureAttempts;
    Decision.MaxAttempts = Policy.MaxRetriesPerError;

    if (LoopDetector.IsLooping(Signature, Policy) || ...)
    {
        Decision.Action = EOliveCorrectionAction::StopWorker;
        // ...
    }
```

**Also in `RunManager`:** Once `bStopAfterToolResults = true` or `Brain->CompleteRun(Failed)` is called, skip executing remaining tool calls in the batch:

**File: `Source/OliveAIEditor/Private/Chat/OliveRunManager.cpp`**

In the tool execution loop, add an early-out check:

```cpp
// In the tool result handler, before executing next tool:
if (bStopAfterToolResults)
{
    // Don't execute remaining tools — the run is already failed
    // Send accumulated results and stop
    break;
}
```

### Secondary Fix: Dependent Tool Call Ordering

Claude CLI sent all 17 tool calls in one shot with `--max-turns 1`. The `blueprint.add_component` calls *depend* on `blueprint.create` succeeding first. Consider either:

1. **Sequential dependency detection**: If a tool call references a path that another tool call in the same batch is creating, execute the create first.
2. **Or**: When a `blueprint.create` fails, immediately skip all subsequent tool calls that reference the same asset path, returning a clear error like `"Skipped: prerequisite blueprint.create for '/Game/Blueprints/BP_Bullet' failed"`.

---

## Issue 2: OpenAI API — `max_tokens` Not Supported on Newer Models

### What Happened

OpenAI's `o1`, `o3`, `o4-mini`, and `gpt-4.1` series models reject the `max_tokens` parameter and require `max_completion_tokens` instead. The error:

```
[HTTP:400] OpenAI API error: Unsupported parameter: 'max_tokens' is not supported
with this model. Use 'max_completion_tokens' instead.
```

### Root Cause

All three OpenAI-family providers unconditionally send `max_tokens`:

- `FOliveOpenAIProvider::BuildRequestBody` (line ~76197)
- `FOliveOpenAICompatibleProvider::BuildRequestBody` (line ~75534)  
- `FOliveOpenRouterProvider::BuildRequestBody` (line ~76865)

### Fix

**File: `Source/OliveAIEditor/Private/Providers/OliveOpenAIProvider.cpp`**

In `BuildRequestBody`, detect reasoning models and use the correct parameter:

```cpp
// BEFORE:
Body->SetNumberField(TEXT("max_tokens"), EffectiveMaxTokens);

// AFTER:
if (IsReasoningModel(Config.ModelId))
{
    Body->SetNumberField(TEXT("max_completion_tokens"), EffectiveMaxTokens);
    // Reasoning models also don't support temperature
    // (don't set temperature field)
}
else
{
    Body->SetNumberField(TEXT("max_tokens"), EffectiveMaxTokens);
    Body->SetNumberField(TEXT("temperature"), EffectiveTemperature);
}
```

Add a helper method to the header and cpp:

```cpp
// In OliveOpenAIProvider.h — add to private section:
static bool IsReasoningModel(const FString& ModelId);

// In OliveOpenAIProvider.cpp:
bool FOliveOpenAIProvider::IsReasoningModel(const FString& ModelId)
{
    // OpenAI reasoning models: o1, o1-mini, o1-pro, o3, o3-mini, o4-mini
    // Also: gpt-4.1 series uses max_completion_tokens
    return ModelId.StartsWith(TEXT("o1"))
        || ModelId.StartsWith(TEXT("o3"))
        || ModelId.StartsWith(TEXT("o4"))
        || ModelId.Contains(TEXT("gpt-4.1"));
}
```

Also move the `temperature` line to be conditional (reasoning models reject it too):

```cpp
// Full corrected BuildRequestBody:
Body->SetStringField(TEXT("model"), Config.ModelId);
Body->SetArrayField(TEXT("messages"), ConvertMessagesToJson(Messages));

if (Tools.Num() > 0)
{
    Body->SetArrayField(TEXT("tools"), ConvertToolsToJson(Tools));
    Body->SetStringField(TEXT("tool_choice"), TEXT("auto"));
}

Body->SetBoolField(TEXT("stream"), true);

if (IsReasoningModel(Config.ModelId))
{
    Body->SetNumberField(TEXT("max_completion_tokens"), EffectiveMaxTokens);
    // reasoning models: no temperature, no top_p
}
else
{
    Body->SetNumberField(TEXT("temperature"), EffectiveTemperature);
    Body->SetNumberField(TEXT("max_tokens"), EffectiveMaxTokens);
}
```

**Apply the same fix to:**
- `FOliveOpenAICompatibleProvider::BuildRequestBody` — same pattern, but since this is a generic compatible provider, it should also check the model name. Alternatively, add a config flag `bUseMaxCompletionTokens`.
- `FOliveOpenRouterProvider::BuildRequestBody` — OpenRouter proxies to various models, so the same detection applies.

### Also Update Available Models List

The `GetAvailableModels()` list is outdated:

```cpp
// BEFORE:
return { "gpt-4o", "gpt-4o-mini", "gpt-4-turbo", "o1", "o1-mini" };

// AFTER:
return { "gpt-4o", "gpt-4o-mini", "gpt-4.1", "gpt-4.1-mini", "gpt-4.1-nano",
         "o3", "o3-mini", "o4-mini", "o1", "o1-mini" };
```

---

## Summary of Files to Change

| File | Change |
|------|--------|
| `OliveSelfCorrectionPolicy.cpp` | Use per-signature attempt count from LoopDetector instead of global `CurrentAttemptCount` |
| `OliveRunManager.cpp` | Skip remaining tool calls once `bStopAfterToolResults` is set |
| `OliveOpenAIProvider.cpp` | Add `IsReasoningModel()`, use `max_completion_tokens` for reasoning models, skip temperature |
| `OliveOpenAIProvider.h` | Declare `IsReasoningModel()` |
| `OliveOpenAICompatibleProvider.cpp` | Same `max_completion_tokens` fix |
| `OliveOpenRouterProvider.cpp` | Same `max_completion_tokens` fix |
