# Bugfix Plan: Cascade Failure & max_tokens

## Issue 1: Cascading Self-Correction Failure (4 tasks)

### Problem Summary
When a batch of tool calls arrives (e.g., 17 tool calls from Claude CLI), the self-correction policy uses a **global** `CurrentAttemptCount` shared across all tools. Two different error types (`VALIDATION_INVALID_VALUE` on `blueprint.create` + `ASSET_NOT_FOUND` on `add_component`) collide in the same counter, triggering premature loop detection. Additionally, once `bStopAfterToolResults` is set, remaining tool calls in the batch still execute.

---

### Task 1: Add `GetAttemptCount()` to `FOliveLoopDetector`

**File:** `Source/OliveAIEditor/Public/Brain/OliveRetryPolicy.h`

Add a public method to `FOliveLoopDetector` that returns the per-signature attempt count:

```cpp
// Add after GetTotalAttempts() (line 55), before IsBudgetExhausted:

/** Get the attempt count for a specific error signature */
int32 GetAttemptCount(const FString& ErrorSignature) const;
```

**File:** `Source/OliveAIEditor/Private/Brain/OliveRetryPolicy.cpp`

Add implementation:

```cpp
int32 FOliveLoopDetector::GetAttemptCount(const FString& ErrorSignature) const
{
    const TArray<FString>* Fixes = AttemptHistory.Find(ErrorSignature);
    return Fixes ? Fixes->Num() : 0;
}
```

---

### Task 2: Use Per-Signature Counting in `FOliveSelfCorrectionPolicy`

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

Replace the global `CurrentAttemptCount++` with per-signature counts from the LoopDetector.

**In the compile failure branch (lines 25-27):**
```cpp
// BEFORE:
CurrentAttemptCount++;
Decision.AttemptNumber = CurrentAttemptCount;
Decision.MaxAttempts = Policy.MaxRetriesPerError;

// AFTER:
const FString Signature = FOliveLoopDetector::BuildCompileErrorSignature(AssetPath, CompileErrors);
LoopDetector.RecordAttempt(Signature, FString::Printf(TEXT("compile_retry_%d"), LoopDetector.GetAttemptCount(Signature) + 1));

const int32 SignatureAttempts = LoopDetector.GetAttemptCount(Signature);
Decision.AttemptNumber = SignatureAttempts;
Decision.MaxAttempts = Policy.MaxRetriesPerError;
```
Note: Move the `Signature` computation and `RecordAttempt` call BEFORE the decision fields, and remove the duplicate `Signature`/`RecordAttempt` that were on lines 30-31.

**In the tool failure branch (lines 54-56):**
Same pattern — replace `CurrentAttemptCount++` with per-signature count from LoopDetector. Move `Signature` computation and `RecordAttempt` before the decision fields, remove duplicates from lines 59-60.

**File:** `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h`

Remove `CurrentAttemptCount` member (line 108). Remove it from `Reset()` too (line 86 of .cpp). The LoopDetector now owns all counting.

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

Update `Reset()` to be empty (or remove it — but keeping it empty is safer for future use). Remove `CurrentAttemptCount = 0;`.

---

### Task 3: Skip Remaining Tool Calls After Stop

**File:** `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`

In `ProcessPendingToolCalls()` (line 696-699), add an early-out check inside the tool execution loop:

```cpp
// BEFORE (line 696-699):
for (const FOliveStreamChunk& ToolCall : CallsToProcess)
{
    ExecuteToolCall(ToolCall);
}

// AFTER:
for (const FOliveStreamChunk& ToolCall : CallsToProcess)
{
    // If a previous tool in this batch triggered stop, skip remaining tools
    // and generate a skip result so the LLM knows they weren't executed
    if (bStopAfterToolResults)
    {
        FOliveChatMessage SkipMessage;
        SkipMessage.Role = EOliveChatRole::Tool;
        SkipMessage.ToolCallId = ToolCall.ToolCallId;
        SkipMessage.ToolName = ToolCall.ToolName;
        SkipMessage.Content = TEXT("{\"success\":false,\"error\":{\"code\":\"SKIPPED\",\"message\":\"Skipped: a previous tool call in this batch triggered a stop.\"}}");
        SkipMessage.Timestamp = FDateTime::UtcNow();
        PendingToolResults.Add(SkipMessage);

        PendingToolExecutions--;
        continue;
    }

    ExecuteToolCall(ToolCall);
}

// After the loop, check if all were skipped and we need to finalize
if (PendingToolExecutions <= 0)
{
    ContinueAfterToolResults();
}
```

This ensures that when `bStopAfterToolResults` is set by `HandleToolResult()` on tool N, tools N+1 through the end are skipped with a clear "SKIPPED" result that the LLM receives.

---

### Task 4: Guard Against Double `CompleteRun`

**File:** `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`

In `HandleToolResult()` at line 808-816, guard the `CompleteRun` call:

```cpp
// BEFORE:
case EOliveCorrectionAction::StopWorker:
    ResultContent = ResultContent + TEXT("\n\n") + Decision.LoopReport;
    UE_LOG(LogOliveAI, Warning, TEXT("Self-correction loop detected for tool '%s'. Stopping."), *ToolName);
    if (Brain.IsValid())
    {
        Brain->CompleteRun(EOliveRunOutcome::Failed);
    }
    bStopAfterToolResults = true;
    break;

// AFTER:
case EOliveCorrectionAction::StopWorker:
    ResultContent = ResultContent + TEXT("\n\n") + Decision.LoopReport;
    UE_LOG(LogOliveAI, Warning, TEXT("Self-correction loop detected for tool '%s'. Stopping."), *ToolName);
    bStopAfterToolResults = true;
    // Don't call CompleteRun here — ContinueAfterToolResults() handles it once after all results are collected
    break;
```

And in `ContinueAfterToolResults()` (line 976-984), the existing code already handles the cleanup:
```cpp
if (bStopAfterToolResults)
{
    bStopAfterToolResults = false;
    bIsProcessing = false;
    if (Brain.IsValid() && Brain->GetState() != EOliveBrainState::Idle)
    {
        Brain->CompleteRun(EOliveRunOutcome::Failed);  // Single call here
        Brain->ResetToIdle();
    }
    ...
}
```

This is correct — just remove the `CompleteRun` from `HandleToolResult` so it only fires once.

---

## Issue 2: OpenAI `max_tokens` Not Supported on Reasoning Models (3 tasks)

### Problem Summary
OpenAI's reasoning models (o1, o3, o4-mini) and gpt-4.1 series reject `max_tokens` and require `max_completion_tokens`. They also reject `temperature`.

---

### Task 5: Fix `FOliveOpenAIProvider` — Add `IsReasoningModel()` + conditional params

**File:** `Source/OliveAIEditor/Public/Providers/OliveOpenAIProvider.h`

Add helper method declaration in the private section (after `BuildRequestBody`, around line 59):

```cpp
/** Check if a model requires max_completion_tokens instead of max_tokens */
static bool IsReasoningModel(const FString& ModelId);
```

**File:** `Source/OliveAIEditor/Private/Providers/OliveOpenAIProvider.cpp`

Add the helper implementation (before `BuildRequestBody` or after it):

```cpp
bool FOliveOpenAIProvider::IsReasoningModel(const FString& ModelId)
{
    // OpenAI reasoning models: o1, o1-mini, o1-pro, o3, o3-mini, o4-mini
    // gpt-4.1 series also requires max_completion_tokens
    return ModelId.StartsWith(TEXT("o1"))
        || ModelId.StartsWith(TEXT("o3"))
        || ModelId.StartsWith(TEXT("o4"))
        || ModelId.Contains(TEXT("gpt-4.1"));
}
```

Update `BuildRequestBody()` (lines 216-218):

```cpp
// BEFORE:
Body->SetBoolField(TEXT("stream"), true);
Body->SetNumberField(TEXT("temperature"), EffectiveTemperature);
Body->SetNumberField(TEXT("max_tokens"), EffectiveMaxTokens);

// AFTER:
Body->SetBoolField(TEXT("stream"), true);

if (IsReasoningModel(Config.ModelId))
{
    Body->SetNumberField(TEXT("max_completion_tokens"), EffectiveMaxTokens);
    // Reasoning models reject temperature and top_p
}
else
{
    Body->SetNumberField(TEXT("temperature"), EffectiveTemperature);
    Body->SetNumberField(TEXT("max_tokens"), EffectiveMaxTokens);
}
```

Update `GetAvailableModels()` (lines 31-40):

```cpp
// BEFORE:
return {
    TEXT("gpt-4o"),
    TEXT("gpt-4o-mini"),
    TEXT("gpt-4-turbo"),
    TEXT("o1"),
    TEXT("o1-mini")
};

// AFTER:
return {
    TEXT("gpt-4o"),
    TEXT("gpt-4o-mini"),
    TEXT("gpt-4.1"),
    TEXT("gpt-4.1-mini"),
    TEXT("gpt-4.1-nano"),
    TEXT("o3"),
    TEXT("o3-mini"),
    TEXT("o4-mini"),
    TEXT("o1"),
    TEXT("o1-mini")
};
```

---

### Task 6: Fix `FOliveOpenAICompatibleProvider` — Same conditional params

**File:** `Source/OliveAIEditor/Public/Providers/OliveOpenAICompatibleProvider.h`

Add helper in private section (after `BuildRequestBody`, around line 65):

```cpp
/** Check if a model requires max_completion_tokens instead of max_tokens */
static bool IsReasoningModel(const FString& ModelId);
```

**File:** `Source/OliveAIEditor/Private/Providers/OliveOpenAICompatibleProvider.cpp`

Add same `IsReasoningModel()` implementation (duplicated since these are independent classes — no shared base).

Update `BuildRequestBody()` (lines 223-225) with same conditional pattern.

---

### Task 7: Fix `FOliveOpenRouterProvider` — Same conditional params + update model list

**File:** `Source/OliveAIEditor/Public/Providers/OliveOpenRouterProvider.h`

Add helper in private section (after `BuildRequestBody`, around line 58):

```cpp
/** Check if a model requires max_completion_tokens instead of max_tokens */
static bool IsReasoningModel(const FString& ModelId);
```

**File:** `Source/OliveAIEditor/Private/Providers/OliveOpenRouterProvider.cpp`

Add `IsReasoningModel()` — but for OpenRouter, model IDs have a prefix like `openai/o3`:

```cpp
bool FOliveOpenRouterProvider::IsReasoningModel(const FString& ModelId)
{
    // OpenRouter model IDs have provider prefix (e.g., "openai/o3")
    // Extract the model part after the last slash
    FString ModelPart = ModelId;
    int32 SlashIdx;
    if (ModelId.FindLastChar(TEXT('/'), SlashIdx))
    {
        ModelPart = ModelId.Mid(SlashIdx + 1);
    }

    return ModelPart.StartsWith(TEXT("o1"))
        || ModelPart.StartsWith(TEXT("o3"))
        || ModelPart.StartsWith(TEXT("o4"))
        || ModelPart.Contains(TEXT("gpt-4.1"));
}
```

Update `BuildRequestBody()` (lines 217-219) with same conditional pattern.

Update `GetAvailableModels()` to include new OpenAI models:

```cpp
return {
    TEXT("anthropic/claude-sonnet-4"),
    TEXT("anthropic/claude-opus-4"),
    TEXT("anthropic/claude-3.5-sonnet"),
    TEXT("anthropic/claude-3-opus"),
    TEXT("openai/gpt-4o"),
    TEXT("openai/gpt-4o-mini"),
    TEXT("openai/gpt-4.1"),
    TEXT("openai/gpt-4.1-mini"),
    TEXT("openai/o3"),
    TEXT("openai/o3-mini"),
    TEXT("openai/o4-mini"),
    TEXT("google/gemini-pro-1.5"),
    TEXT("meta-llama/llama-3.1-70b-instruct"),
    TEXT("meta-llama/llama-3.1-405b-instruct")
};
```

---

## Files Modified Summary

| # | File | Change |
|---|------|--------|
| 1 | `OliveRetryPolicy.h` | Add `GetAttemptCount()` to `FOliveLoopDetector` |
| 2 | `OliveRetryPolicy.cpp` | Implement `GetAttemptCount()` |
| 3 | `OliveSelfCorrectionPolicy.h` | Remove `CurrentAttemptCount` member |
| 4 | `OliveSelfCorrectionPolicy.cpp` | Use per-signature counts, simplify `Reset()` |
| 5 | `OliveConversationManager.cpp` | Skip remaining tools after stop; remove duplicate `CompleteRun` |
| 6 | `OliveOpenAIProvider.h` | Add `IsReasoningModel()` declaration |
| 7 | `OliveOpenAIProvider.cpp` | Add `IsReasoningModel()`, conditional params, update models |
| 8 | `OliveOpenAICompatibleProvider.h` | Add `IsReasoningModel()` declaration |
| 9 | `OliveOpenAICompatibleProvider.cpp` | Add `IsReasoningModel()`, conditional params |
| 10 | `OliveOpenRouterProvider.h` | Add `IsReasoningModel()` declaration |
| 11 | `OliveOpenRouterProvider.cpp` | Add `IsReasoningModel()` (with prefix handling), conditional params, update models |

## Execution Order

Tasks 1-4 (Issue 1) and Tasks 5-7 (Issue 2) are independent and can be parallelized:
- **Tasks 1 → 2** (sequential: header first, then cpp uses it)
- **Tasks 3 → 4** (sequential: Task 3 changes a file Task 4 also changes)
- **Tasks 5, 6, 7** (independent: each provider is a separate file pair)

After all edits: **build and verify compilation**.
