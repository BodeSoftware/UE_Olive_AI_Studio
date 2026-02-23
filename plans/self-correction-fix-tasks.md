# Self-Correction Fix Plan -- Architectural Review and Coder Tasks

**Author:** Architect Agent
**Date:** 2026-02-23
**Status:** Ready for Implementation
**Scope:** 7 fixes across 6 files, organized into 3 phases

---

## Architectural Review

### Overall Assessment

The plan correctly identifies a three-layer failure cascade: (1) the CLI process crashes from command-line length overflow, (2) the crash is misclassified as terminal (preventing retry), and (3) even when correction feedback reaches the AI, nothing forces the AI to act on it. The fixes are well-sequenced and address all three layers. I have reviewed every source file involved and confirm the plan is architecturally sound, with several corrections and clarifications noted below.

### Fix-by-Fix Review

#### Fix 1 -- Stdin Pipe for CLI Prompt Delivery (APPROVED WITH NOTES)

**Assessment: Correct and necessary.**

The current code at line 315 of `OliveClaudeCodeProvider.cpp` passes the full prompt via `-p "%s"` in the command-line arguments. On Windows, `CreateProcessW` has a hard limit of 32,767 characters for the command line. The enriched error messages from the self-correction policy (each tool result gets a multi-line correction directive appended) compound rapidly across iterations.

**Critical API details the coder MUST know:**

1. **`FPlatformProcess::CreatePipe(ReadPipe, WritePipe, bWritePipeLocal)`** -- For the stdin pipe, call with `bWritePipeLocal = true`. This makes the **write** end non-inheritable (parent keeps it), while the **read** end is inheritable (child gets it). Default (`false`) makes the read end non-inheritable, which is correct for stdout (parent reads) but wrong for stdin (parent writes).

2. **`FPlatformProcess::CreateProc` parameter semantics** -- The 10th parameter is `PipeReadChild`, documented as "Optional HANDLE to pipe for redirecting stdin". This receives the **read** end of the stdin pipe (the end the child reads from). The current code passes `nullptr` here (line 375).

3. **`FPlatformProcess::WritePipe(void*, const FString&)` already appends `\n`** -- The Windows implementation at line 1843 of `WindowsPlatformProcess.cpp` converts to UTF-8 and appends a newline. The coder's `SendToProcess()` method (line 764) also appends `\n`. If the coder reuses `SendToProcess` for the one-shot write, this results in a double newline. For the one-shot stdin write, use `WritePipe` directly with the raw bytes overload, or use the FString overload but do NOT add an extra `\n`.

4. **Pipe close order matters** -- After writing the prompt to stdin, the write end MUST be closed. This signals EOF to the child process. If the write end is not closed, the child will block waiting for more input indefinitely.

5. **Class member cleanup** -- The existing `StdinWritePipe` member (line 134 in the header) and the existing `KillClaudeProcess()` cleanup (lines 744-748) already handle stdin pipe cleanup. The one-shot write approach means `StdinWritePipe` should be set to `nullptr` after closing, or better yet, use local variables for the stdin pipe in the one-shot flow and only use the member for the ongoing process model.

**Risk: Medium.** Pipe management across threads is fiddly. The write happens on the background thread (inside the `AsyncTask` lambda), which is fine since it is before the read loop starts.

#### Fix 2 -- Process Crash Error Classification (APPROVED)

**Assessment: Simple and correct.**

The `ClassifyError` method in `OliveProviderRetryManager.cpp` defaults to `Terminal` at line 323 when no heuristic matches. The error message format from `HandleResponseComplete` (line 481) is `"Claude process exited with code %d"`. The process-crash pattern should match this format as Transient.

**One concern:** Negative exit codes on Windows typically indicate unhandled exceptions (e.g., `STATUS_STACK_BUFFER_OVERRUN = 0xC0000409` which as signed int32 is `-1073740791`). Not all process crashes are transient -- some indicate genuine bugs. However, since Fix 1 addresses the root cause (stdin pipe eliminates the crash), Fix 2 is a safety net that ensures any remaining transient process failures get retried. This is the right call.

**Addition recommended:** Also match "Claude process" as a substring rather than an exact format, in case the error message wording changes. A pattern like `"process exited with code"` is more resilient.

#### Fix 3 -- System Prompt Rules (APPROVED)

**Assessment: Low risk, purely additive.**

The system prompt in `BuildSystemPrompt()` (lines 537-602) already contains extensive guidance. Adding three explicit rules about workflow ordering, step ordering, and target asset is a minor text addition. The rules should be added to the existing `## Rules` section (around line 582) rather than creating a new section.

**Note:** These rules only affect the Claude Code CLI provider. The system prompt for API-based providers is assembled in `FOlivePromptAssembler`. If these rules are universal, they should eventually be added to the prompt template in `Content/SystemPrompts/`, but that is a follow-up, not part of this fix.

#### Fix 4 -- Aggressive Context Distillation (APPROVED WITH DESIGN NOTES)

**Assessment: Correct approach, needs precise specification.**

The current distiller (lines 12-75 of `OlivePromptDistiller.cpp`) only summarizes tool results: it keeps the last N verbatim and summarizes older ones. It does NOT enforce a total character budget. As the agentic loop iterates, the total context grows unboundedly.

**Design decisions the coder needs:**

1. **Budget scope** -- The `MaxTotalResultChars` budget should apply to tool result messages only, not to user/assistant messages. Summarizing user messages would lose task context; summarizing assistant messages would lose reasoning chains.

2. **Progressive summarization** -- When total tool result chars exceed budget, the distiller should progressively summarize from oldest to newest until under budget. Already-summarized messages should not be re-summarized (they are already one-liners).

3. **Budget value** -- 80K chars is approximately 20K tokens. This is reasonable given that Claude's context window is 200K tokens and we need room for system prompt + user messages + assistant messages + tool schemas. However, this should be configurable via `FOliveDistillationConfig` rather than hardcoded.

4. **Integration with existing Distill()** -- The budget enforcement should happen AFTER the existing age-based summarization pass, as a second pass. This is cleaner than trying to combine both heuristics.

#### Fix 5 -- Error-Specific Self-Correction Hints (APPROVED)

**Assessment: High value, low risk.**

The current `BuildToolErrorMessage` (lines 184-196 of `OliveSelfCorrectionPolicy.cpp`) returns generic guidance. Error-code-specific hints will significantly improve correction success rates.

**Error codes to handle** (verified against our codebase):
- `VALIDATION_MISSING_PARAM` -- Tell the AI which param is missing
- `ASSET_NOT_FOUND` -- Tell the AI to use `project.search_assets` to find correct path
- `BP_ADD_NODE_FAILED` -- Tell the AI to use `blueprint.search_nodes` to find correct node type
- `NODE_TYPE_UNKNOWN` -- Include fuzzy match suggestions if present in the error message
- `DUPLICATE_NATIVE_EVENT` -- Tell the AI the event already exists, use `blueprint.read` to see it
- `FUNCTION_NOT_FOUND` -- Tell the AI to use `blueprint.search_nodes`
- `DATA_PIN_NOT_FOUND` / `EXEC_PIN_NOT_FOUND` -- Tell the AI to use `blueprint.read` to see actual pin names
- `BP_REMOVE_NODE_FAILED` -- Tell the AI to check if the node exists first
- `USER_DENIED` -- This is intentional; no retry needed

**Fallback:** Unknown error codes should still get the existing generic message.

#### Fix 6 -- Correction Directive Injection (APPROVED WITH ARCHITECTURE CONCERN)

**Assessment: Correct in principle, needs careful placement.**

The plan calls for tracking failed tool results and injecting an aggregated correction directive. This is the right approach. The question is WHERE in the flow this happens.

**Recommended placement:** In `ContinueAfterToolResults()` (line 1081 of `OliveConversationManager.cpp`), just before the `SendToProvider()` call on line 1126. At this point, all tool results have been added to `MessageHistory`, and we are about to re-enter the agentic loop. The correction directive should be injected as a `System` role message at the end of the history, containing:
- A count of failed tools in this batch
- The specific tool names and error codes
- An explicit instruction: "You MUST retry the failed operations before proceeding"

**Architecture concern:** Adding a `System` message mid-conversation is unusual. Some providers may not handle interleaved system messages well. The safer approach is to inject it as a `User` role message (a synthetic user message that the actual user did not type). This is semantically less clean but more reliable across providers. The coder should use a `User` role message with a clear prefix like `[SYSTEM: ...]` to distinguish it from actual user input.

**Alternative approach (recommended):** Rather than a separate message, append the correction directive to the LAST tool result message's content. This keeps the message history cleaner and avoids the interleaved-system-message problem. The self-correction policy already appends enriched messages to tool results (line 919). The aggregated directive would be an additional summary appended to the last tool result.

**Decision:** Use the "append to last tool result" approach. It is simpler, more reliable across providers, and does not require new message types. The aggregation logic goes in `ContinueAfterToolResults()`.

#### Fix 7 -- Block Premature Completion on Unresolved Failures (APPROVED WITH LIMITS)

**Assessment: Important but needs a hard cap.**

The plan calls for intercepting text-only completions when there are unresolved failures, and re-prompting up to 2 times. This is the right approach.

**Concerns:**

1. **Re-prompt cap is essential.** The plan says 2 times, which is good. Without a cap, this becomes an infinite loop. The cap MUST be enforced.

2. **What counts as "unresolved"?** A failure is unresolved if the tool returned `success: false` AND the correction policy said `FeedBackErrors` (not `StopWorker`). If the loop detector already said `StopWorker`, the failure is "resolved" (we gave up on it).

3. **PartialSuccess outcome** -- The plan mentions using a `PartialSuccess` outcome. I need to check if `EOliveRunOutcome` has this variant. If not, adding it is a small change.

4. **The re-prompt injection** -- When the AI responds text-only despite unresolved failures, the system should inject a message like: `"[SYSTEM: You have X unresolved tool failures. You MUST call tools to fix them before responding with text. Remaining re-prompts: Y]"`. This should be injected as the User role message (same reasoning as Fix 6).

**Risk:** Medium. This introduces a new sub-loop inside `HandleComplete`. The interaction between this sub-loop and the existing agentic loop, message queue, and brain layer state needs careful handling.

---

## Cross-Cutting Concerns

### 1. Thread Safety
Fix 1 introduces pipe operations on the background thread. The existing code already does all pipe operations on the background thread (inside the `AsyncTask` lambda in `SendMessage`), so this is consistent. No new thread-safety concerns.

### 2. Memory / Resource Leaks
Fix 1 introduces a second pipe pair. The existing `KillClaudeProcess()` already cleans up `StdinWritePipe`. The one-shot approach means the pipe is closed immediately after writing, so the only leak risk is in error paths. The coder must ensure all error paths close both pipe pairs.

### 3. Provider Compatibility
Fixes 6 and 7 inject synthetic messages. These must work with all 8 providers. Using `User` role messages (not `System`) ensures compatibility since all providers handle user messages.

### 4. Token Budget Impact
Fix 4 introduces a hard character budget. Fix 6 adds a correction directive message. These work in the same direction (controlling context size) but must be coordinated. The correction directive should NOT be subject to distillation since it is always recent.

---

## Implementation Tasks

### Phase 1: Foundation (Fixes 1, 2, 3)

These fixes address the root cause (CLI crash) and its immediate consequence (terminal classification). They must be implemented together.

---

#### Task P1-A: Stdin Pipe for CLI Prompt Delivery

**File:** `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp`

**Step 1: Remove `-p` from CLI args**

At line 314-318, the `ClaudeArgs` format string currently is:
```cpp
FString ClaudeArgs = FString::Printf(
    TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --max-turns 1 %s-p \"%s\""),
    *SystemPromptArg,
    *EscapedPrompt
);
```

Change to:
```cpp
FString ClaudeArgs = FString::Printf(
    TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --max-turns 1 %s"),
    *SystemPromptArg
);
```

Note: The `EscapedPrompt` and its escaping logic (lines 292-294) are no longer needed for command-line purposes. However, keep the prompt variable itself since we need it for stdin. The escaping (`ReplaceInline` for backslashes and quotes) is also no longer needed since stdin is raw data, not a shell argument. Remove lines 291-294 (the `EscapedPrompt` variable and its escaping).

**Step 2: Create stdin pipe**

After the stdout pipe creation block (line 352), add a second pipe pair for stdin. CRITICAL: use `bWritePipeLocal = true` because the parent writes and the child reads.

```cpp
void* StdinRead = nullptr;
void* StdinWrite = nullptr;

if (!FPlatformProcess::CreatePipe(StdinRead, StdinWrite, /*bWritePipeLocal=*/true))
{
    FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
    AsyncTask(ENamedThreads::GameThread, [this]()
    {
        FScopeLock Lock(&CallbackLock);
        bIsBusy = false;
        CurrentOnError.ExecuteIfBound(TEXT("Failed to create stdin pipe for Claude process"));
    });
    return;
}
```

**Step 3: Pass stdin read end to CreateProc**

At line 365, change the last parameter from `nullptr` to `StdinRead`:

```cpp
ProcessHandle = FPlatformProcess::CreateProc(
    *Executable,
    *Args,
    false,  // bLaunchDetached
    true,   // bLaunchHidden
    true,   // bLaunchReallyHidden
    &ProcessId,
    0,      // PriorityModifier
    *WorkingDirectory,
    StdoutWrite,  // stdout pipe (child writes)
    StdinRead     // stdin pipe (child reads)
);
```

**Step 4: Handle error path for both pipe pairs**

At line 378 (the `!ProcessHandle.IsValid()` error path), close BOTH pipe pairs:

```cpp
if (!ProcessHandle.IsValid())
{
    FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
    FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
    // ... existing error handling ...
}
```

**Step 5: Close pipe ends and write prompt**

After the process spawn succeeds and the stdout write end is closed (line 391), close the stdin read end (we only write) and write the prompt:

```cpp
// Close write end of stdout pipe (we only read)
FPlatformProcess::ClosePipe(nullptr, StdoutWrite);

// Close read end of stdin pipe (we only write), then write prompt and close
FPlatformProcess::ClosePipe(StdinRead, nullptr);

// Write the prompt to stdin. Use the raw bytes overload to avoid
// the automatic newline appended by the FString overload.
FTCHARToUTF8 Utf8Prompt(*Prompt);
int32 BytesWritten = 0;
FPlatformProcess::WritePipe(
    StdinWrite,
    reinterpret_cast<const uint8*>(Utf8Prompt.Get()),
    Utf8Prompt.Length(),
    &BytesWritten);

// Close write end to signal EOF -- child will read all input and proceed
FPlatformProcess::ClosePipe(nullptr, StdinWrite);
StdinWrite = nullptr;
```

IMPORTANT: Use the `uint8*` overload of `WritePipe`, not the `FString` overload, because the FString overload appends `\n` automatically. The prompt data should be written as-is.

Alternatively, if you prefer using `FPlatformProcess::WritePipe(StdinWrite, Prompt)` (the FString overload), that is acceptable but be aware it adds a trailing newline. Claude Code CLI reads stdin to EOF, so a trailing newline is harmless.

**Step 6: Remove system prompt escaping**

The `EscapedSystemPrompt` variable (lines 297-299) is still needed because it goes into the `--append-system-prompt` argument. However, the `EscapedPrompt` variable (lines 292-294) should be removed since the prompt no longer goes on the command line.

Also update the log message at line 345 to not log the args (which no longer contain the prompt), or add a separate log for stdin prompt size:

```cpp
UE_LOG(LogOliveClaudeCode, Log, TEXT("Prompt delivered via stdin: %d chars"), Prompt.Len());
```

---

#### Task P1-B: Process Crash Error Classification

**File:** `Source/OliveAIEditor/Private/Providers/OliveProviderRetryManager.cpp`

**Location:** In `ClassifyError()`, in the Tier 2 heuristic section, BEFORE the terminal auth/client errors block (line 309). The process crash pattern must be checked before the terminal patterns because "exited with code" could potentially match other patterns.

**Add this block** after the "5xx server errors" block (after line 292) and before the "Use explicit HttpStatus hint" block (line 294):

```cpp
// Process crash / abnormal exit (e.g. command-line overflow, segfault)
if (LowerMessage.Contains(TEXT("process exited with code")) ||
    LowerMessage.Contains(TEXT("process crashed")) ||
    LowerMessage.Contains(TEXT("process terminated")))
{
    return FOliveProviderErrorInfo::Transient(ErrorMessage, 0);
}
```

This must go BEFORE the terminal auth checks because we do not want `"process exited with code 400"` (hypothetical) to match the `Contains("400")` terminal pattern.

---

#### Task P1-C: System Prompt Rules

**File:** `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp`

**Location:** In `BuildSystemPrompt()`, add to the existing `## Rules` section. After line 589 (the component classes line) and before the tool definitions block (line 592).

**Add these three rules:**

```cpp
SystemPrompt += TEXT("- WORKFLOW: Always create the Blueprint first, then add variables/components, then add graph logic\n");
SystemPrompt += TEXT("- STEP ORDER: In plan JSON, define events before the nodes that connect to them via exec_after\n");
SystemPrompt += TEXT("- TARGET ASSET: Always specify the full asset path (e.g. /Game/Blueprints/BP_MyActor) in every tool call, never assume\n");
```

---

### Phase 2: Self-Correction Quality (Fixes 5, 6, 7)

These fixes improve the quality of correction feedback and ensure the AI acts on it.

---

#### Task P2-A: Error-Specific Self-Correction Hints

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

**Location:** Replace the body of `BuildToolErrorMessage()` (lines 184-196).

**New implementation:**

```cpp
FString FOliveSelfCorrectionPolicy::BuildToolErrorMessage(
    const FString& ToolName,
    const FString& ErrorCode,
    const FString& ErrorMessage,
    int32 AttemptNum,
    int32 MaxAttempts) const
{
    FString Header = FString::Printf(
        TEXT("[TOOL FAILED - Attempt %d/%d] Tool '%s' failed with error %s: %s"),
        AttemptNum, MaxAttempts, *ToolName, *ErrorCode, *ErrorMessage);

    FString Guidance;

    if (ErrorCode == TEXT("VALIDATION_MISSING_PARAM"))
    {
        Guidance = TEXT("Check the tool schema for required parameters. Re-call the tool with all required parameters filled in.");
    }
    else if (ErrorCode == TEXT("ASSET_NOT_FOUND"))
    {
        Guidance = TEXT("The asset path is wrong. Use project.search_assets to find the correct path, then retry with the corrected path.");
    }
    else if (ErrorCode == TEXT("NODE_TYPE_UNKNOWN") || ErrorCode == TEXT("BP_ADD_NODE_FAILED"))
    {
        Guidance = TEXT("The node type was not found. Use blueprint.search_nodes to find the correct node type identifier, then retry.");
    }
    else if (ErrorCode == TEXT("FUNCTION_NOT_FOUND"))
    {
        Guidance = TEXT("The function was not found. Use blueprint.search_nodes to find the correct function name. Check for K2_ prefixes and class membership.");
    }
    else if (ErrorCode == TEXT("DUPLICATE_NATIVE_EVENT"))
    {
        Guidance = TEXT("This event already exists in the graph. Use blueprint.read to see existing nodes. Reuse the existing event node instead of creating a new one.");
    }
    else if (ErrorCode == TEXT("DATA_PIN_NOT_FOUND") || ErrorCode == TEXT("EXEC_PIN_NOT_FOUND") || ErrorCode == TEXT("DATA_PIN_AMBIGUOUS"))
    {
        Guidance = TEXT("Pin name mismatch. Use blueprint.read with include_pins:true to see the actual pin names on the target node, then retry with the correct pin name.");
    }
    else if (ErrorCode == TEXT("BP_REMOVE_NODE_FAILED"))
    {
        Guidance = TEXT("The node could not be removed. Use blueprint.read to check if the node exists and get its correct node_id.");
    }
    else if (ErrorCode == TEXT("USER_DENIED"))
    {
        Guidance = TEXT("The user denied this operation. Ask the user how they would like to proceed instead of retrying.");
    }
    else
    {
        // Fallback: generic guidance
        Guidance = TEXT("Analyze the error and try a different approach. If the parameters were wrong, read the asset first to verify its current state.");
    }

    return Header + TEXT("\nFIX GUIDANCE: ") + Guidance;
}
```

---

#### Task P2-B: Correction Directive Injection

**Files:**
- `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h`
- `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`

**Header changes:**

Add a new private member to track failed tool results in the current batch. In the "Streaming State" section of the header (after line 345, near `PendingToolExecutions`):

```cpp
/** Count of failed tool results in the current batch (for correction directive) */
int32 CurrentBatchFailureCount = 0;

/** Accumulated correction summary for the current batch */
FString CurrentBatchCorrectionSummary;
```

Add reset of these in `ClearHistory()`, `CancelCurrentRequest()`, and at the top of `ProcessPendingToolCalls()`.

**Implementation changes in OliveConversationManager.cpp:**

**(B1) Track failures in HandleToolResult:**

In `HandleToolResult()`, after the self-correction policy evaluation (after line 937), add failure tracking:

```cpp
// Track failures for batch-level correction directive
if (!Result.bSuccess)
{
    CurrentBatchFailureCount++;
    FString ErrorCode = TEXT("UNKNOWN");
    FString ErrorMsg = TEXT("Unknown error");
    if (Result.Data.IsValid())
    {
        const TSharedPtr<FJsonObject>* ErrorObj;
        if (Result.Data->TryGetObjectField(TEXT("error"), ErrorObj))
        {
            (*ErrorObj)->TryGetStringField(TEXT("code"), ErrorCode);
            (*ErrorObj)->TryGetStringField(TEXT("message"), ErrorMsg);
        }
    }
    CurrentBatchCorrectionSummary += FString::Printf(
        TEXT("- %s (id: %s): %s - %s\n"),
        *ToolName, *ToolCallId, *ErrorCode, *ErrorMsg);
}
```

**(B2) Inject directive in ContinueAfterToolResults:**

In `ContinueAfterToolResults()`, after adding tool results to history (after line 1087) and before the `bStopAfterToolResults` check (line 1099), inject the correction directive if there were failures:

```cpp
// Inject correction directive if there were failures in this batch
if (CurrentBatchFailureCount > 0 && !bStopAfterToolResults)
{
    FString Directive = FString::Printf(
        TEXT("[CORRECTION REQUIRED: %d tool(s) failed in this batch. Failed operations:\n%s"
             "You MUST retry the failed operations before proceeding to new work. "
             "Read the asset state first if you are unsure of current values.]"),
        CurrentBatchFailureCount, *CurrentBatchCorrectionSummary);

    FOliveChatMessage DirectiveMessage;
    DirectiveMessage.Role = EOliveChatRole::User;
    DirectiveMessage.Content = Directive;
    DirectiveMessage.Timestamp = FDateTime::UtcNow();
    AddMessage(DirectiveMessage);

    UE_LOG(LogOliveAI, Log, TEXT("Injected correction directive for %d failed tools"), CurrentBatchFailureCount);
}

// Reset batch failure tracking
CurrentBatchFailureCount = 0;
CurrentBatchCorrectionSummary.Empty();
```

**(B3) Reset in ProcessPendingToolCalls:**

At the top of `ProcessPendingToolCalls()` (after line 681 where `PendingToolResults.Empty()` is called), add:

```cpp
CurrentBatchFailureCount = 0;
CurrentBatchCorrectionSummary.Empty();
```

**(B4) Reset in ClearHistory and CancelCurrentRequest:**

Add to both methods:
```cpp
CurrentBatchFailureCount = 0;
CurrentBatchCorrectionSummary.Empty();
```

---

#### Task P2-C: Block Premature Completion on Unresolved Failures

**Files:**
- `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h`
- `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`

**Header changes:**

Add new private members in the "Streaming State" section:

```cpp
/** Whether there are unresolved failures that require the AI to retry */
bool bHasPendingCorrections = false;

/** Number of times we have re-prompted the AI to address corrections in the current turn */
int32 CorrectionRepromptCount = 0;

/** Maximum re-prompts for unresolved corrections per turn */
static constexpr int32 MaxCorrectionReprompts = 2;
```

Reset these in `ClearHistory()`, `CancelCurrentRequest()`, and `SendUserMessage()` (when `CurrentToolIteration` is reset to 0).

**Implementation changes in OliveConversationManager.cpp:**

**(C1) Set bHasPendingCorrections in ContinueAfterToolResults:**

In the correction directive injection block from Task P2-B, also set the flag:

```cpp
if (CurrentBatchFailureCount > 0 && !bStopAfterToolResults)
{
    bHasPendingCorrections = true;
    // ... rest of directive injection from P2-B ...
}
```

**(C2) Clear bHasPendingCorrections when tools are called successfully:**

In `HandleToolResult()`, if the correction policy returns `Continue` (success), check if ALL pending corrections might now be resolved. The simplest approach: clear `bHasPendingCorrections` whenever we are about to `SendToProvider()` again (in `ContinueAfterToolResults`) and the CURRENT batch had at least one tool call. Since the AI called tools, it is attempting correction. Set:

```cpp
// In ContinueAfterToolResults, just before SendToProvider():
if (CurrentBatchFailureCount == 0)
{
    // All tools in this batch succeeded -- corrections are resolved
    bHasPendingCorrections = false;
    CorrectionRepromptCount = 0;
}
```

**(C3) Intercept text-only completion in HandleComplete:**

In `HandleComplete()`, in the "no tool calls" branch (line 581-611), add a check BEFORE the completion logic:

```cpp
if (PendingToolCalls.Num() > 0)
{
    ProcessPendingToolCalls();
}
else
{
    // Check if AI responded text-only despite unresolved failures
    if (bHasPendingCorrections && CorrectionRepromptCount < MaxCorrectionReprompts)
    {
        CorrectionRepromptCount++;

        FString RepromptText = FString::Printf(
            TEXT("[SYSTEM: You responded with text but there are still unresolved tool failures "
                 "from a previous batch. You MUST call the appropriate tools to fix these errors "
                 "before completing. Re-prompt %d/%d.]"),
            CorrectionRepromptCount, MaxCorrectionReprompts);

        FOliveChatMessage RepromptMessage;
        RepromptMessage.Role = EOliveChatRole::User;
        RepromptMessage.Content = RepromptText;
        RepromptMessage.Timestamp = FDateTime::UtcNow();
        AddMessage(RepromptMessage);

        UE_LOG(LogOliveAI, Warning,
            TEXT("AI responded text-only with unresolved corrections. Re-prompting (%d/%d)"),
            CorrectionRepromptCount, MaxCorrectionReprompts);

        // Re-enter the agentic loop
        SendToProvider();
        return;
    }

    // If corrections exhausted or no pending corrections, complete normally
    // but use PartialSuccess if there were unresolved failures
    EOliveRunOutcome FinalOutcome = bHasPendingCorrections
        ? EOliveRunOutcome::PartialSuccess
        : EOliveRunOutcome::Completed;

    // Complete run if active
    if (bRunModeActive && FOliveRunManager::Get().HasActiveRun())
    {
        FOliveRunManager::Get().CompleteRun();
        bRunModeActive = false;
    }
    // Brain: transition to Completed/PartialSuccess -> Idle
    if (Brain.IsValid() && Brain->GetState() != EOliveBrainState::Idle)
    {
        Brain->CompleteRun(FinalOutcome);
        Brain->ResetToIdle();
    }

    // Reset correction state
    bHasPendingCorrections = false;
    CorrectionRepromptCount = 0;

    // No tool calls, we're done
    bIsProcessing = false;
    OnProcessingComplete.Broadcast();

    // ... rest of existing completion logic (deferred profile, drain queue) ...
}
```

**IMPORTANT:** Check if `EOliveRunOutcome::PartialSuccess` exists. If it does not exist in the enum, add it.

**(C4) Check EOliveRunOutcome enum:**

The coder must verify the `EOliveRunOutcome` enum definition. If `PartialSuccess` does not exist, add it between `Completed` and `Failed`. If adding it is too invasive (other code checks for specific values), use `Completed` and log a warning about unresolved failures instead.

**(C5) Reset in SendUserMessage:**

In `SendUserMessage()`, where `CurrentToolIteration = 0` is set (line 222), also reset:

```cpp
bHasPendingCorrections = false;
CorrectionRepromptCount = 0;
```

---

### Phase 3: Context Budget (Fix 4)

This fix is independent and can be implemented after Phases 1 and 2 are verified working.

---

#### Task P3-A: Context Budget for Prompt Distiller

**Files:**
- `Source/OliveAIEditor/Public/Brain/OlivePromptDistiller.h`
- `Source/OliveAIEditor/Private/Brain/OlivePromptDistiller.cpp`

**Header changes:**

Add a new field to `FOliveDistillationConfig`:

```cpp
/** Maximum total characters for all tool result messages combined.
  * When exceeded, older tool results are progressively summarized.
  * 0 = no limit. Default 80000 (~20K tokens). */
int32 MaxTotalResultChars = 80000;
```

Add `AssistantDistillThreshold` for the second concern (long assistant messages in later iterations):

```cpp
/** Max chars for a single assistant message before truncation.
  * Only applied to non-recent assistant messages. 0 = no limit. */
int32 MaxAssistantChars = 4000;
```

**Implementation changes in OlivePromptDistiller.cpp:**

The `Distill()` method needs a second pass after the existing age-based summarization. After line 74 (the existing return), restructure as follows:

**(A1) After the existing age-based pass, add budget enforcement:**

```cpp
// Pass 2: Enforce total character budget for tool results
if (Config.MaxTotalResultChars > 0)
{
    // Calculate current total of tool result chars
    int32 TotalResultChars = 0;
    for (int32 i = 0; i < Messages.Num(); i++)
    {
        if (Messages[i].Role == EOliveChatRole::Tool)
        {
            TotalResultChars += Messages[i].Content.Len();
        }
    }

    // If over budget, progressively summarize from oldest to newest
    // Skip messages that are already one-line summaries (start with '[')
    if (TotalResultChars > Config.MaxTotalResultChars)
    {
        for (int32 i = 0; i < Messages.Num() && TotalResultChars > Config.MaxTotalResultChars; i++)
        {
            if (Messages[i].Role != EOliveChatRole::Tool)
            {
                continue;
            }

            // Skip already-summarized messages (they start with '[toolname]')
            if (Messages[i].Content.StartsWith(TEXT("[")))
            {
                continue;
            }

            const int32 OriginalLen = Messages[i].Content.Len();
            Messages[i].Content = SummarizeToolResult(Messages[i].ToolName, Messages[i].Content);
            const int32 NewLen = Messages[i].Content.Len();
            TotalResultChars -= (OriginalLen - NewLen);

            Result.MessagesSummarized++;
            const int32 OriginalTokens = FMath::CeilToInt(OriginalLen / CharsPerToken);
            const int32 NewTokens = EstimateTokens(Messages[i].Content);
            Result.TokensSaved += FMath::Max(0, OriginalTokens - NewTokens);
        }
    }
}
```

**(A2) Optional: Truncate long assistant messages from older iterations:**

```cpp
// Pass 3: Truncate old assistant messages that are excessively long
if (Config.MaxAssistantChars > 0)
{
    // Find the last assistant message index to skip it
    int32 LastAssistantIdx = -1;
    for (int32 i = Messages.Num() - 1; i >= 0; --i)
    {
        if (Messages[i].Role == EOliveChatRole::Assistant)
        {
            LastAssistantIdx = i;
            break;
        }
    }

    for (int32 i = 0; i < Messages.Num(); i++)
    {
        if (Messages[i].Role == EOliveChatRole::Assistant
            && i != LastAssistantIdx
            && Messages[i].Content.Len() > Config.MaxAssistantChars)
        {
            const int32 OriginalLen = Messages[i].Content.Len();
            Messages[i].Content = Messages[i].Content.Left(Config.MaxAssistantChars)
                + TEXT("\n[... truncated for context budget ...]");
            Result.MessagesSummarized++;
            const int32 SavedChars = OriginalLen - Messages[i].Content.Len();
            Result.TokensSaved += FMath::Max(0, FMath::CeilToInt(SavedChars / CharsPerToken));
        }
    }
}
```

**(A3) Set the budget in ConversationManager::SendToProvider():**

In `OliveConversationManager.cpp`, in `SendToProvider()`, after the `DistillConfig` is created (line 413), set the budget:

```cpp
FOliveDistillationConfig DistillConfig;
if (const UOliveAISettings* Settings = UOliveAISettings::Get())
{
    DistillConfig.RecentPairsToKeep = Settings->PromptDistillationRawResults;
}
DistillConfig.MaxTotalResultChars = 80000;  // ~20K tokens
DistillConfig.MaxAssistantChars = 4000;
```

Ideally, `MaxTotalResultChars` would come from `UOliveAISettings`, but adding a new setting is a follow-up. Hardcode 80000 for now with a `// TODO: make configurable via UOliveAISettings` comment.

---

## Implementation Order Summary

| Task | Phase | File(s) | Depends On | Effort |
|------|-------|---------|------------|--------|
| P1-A | 1 | OliveClaudeCodeProvider.cpp | None | Medium |
| P1-B | 1 | OliveProviderRetryManager.cpp | None | Small |
| P1-C | 1 | OliveClaudeCodeProvider.cpp | None | Small |
| P2-A | 2 | OliveSelfCorrectionPolicy.cpp | None | Small |
| P2-B | 2 | OliveConversationManager.cpp/.h | None | Medium |
| P2-C | 2 | OliveConversationManager.cpp/.h | P2-B | Medium |
| P3-A | 3 | OlivePromptDistiller.cpp/.h, OliveConversationManager.cpp | None | Medium |

**Phase 1 tasks (P1-A, P1-B, P1-C) can all be done in parallel.** They touch different areas of different files.

**Phase 2 tasks must be sequential:** P2-A is independent, but P2-B must come before P2-C because P2-C depends on the `CurrentBatchFailureCount` tracking introduced in P2-B.

**Phase 3 (P3-A) is independent** and can be done at any time.

---

## Verification Checklist

After implementation, verify:

1. **P1-A:** Build and run with a prompt that exceeds 32KB. Verify the Claude process receives the full prompt via stdin and responds correctly.
2. **P1-B:** Simulate a process crash error message. Verify it is classified as Transient (not Terminal) and triggers retry.
3. **P1-C:** Verify the system prompt includes the three new rules.
4. **P2-A:** Trigger a `NODE_TYPE_UNKNOWN` error. Verify the enriched message includes specific guidance about `blueprint.search_nodes`.
5. **P2-B:** Execute a batch with 2 tool calls where 1 fails. Verify a correction directive is injected as a User message before `SendToProvider()`.
6. **P2-C:** Make the AI respond text-only after a tool failure. Verify it gets re-prompted up to 2 times. Verify the 3rd text-only response completes with PartialSuccess.
7. **P3-A:** Run a 10+ iteration agentic loop. Verify total tool result content stays under 80K chars. Verify older results are progressively summarized.
