# Pack P3 — Brain Layer Simplification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `FOliveSelfCorrectionPolicy` with retry-once-on-transient-error. Slim `FOliveRunManager` to a linear loop. Delete `FOlivePromptDistiller`. Keep `FOliveBrainLayer` state machine and `FOliveLoopDetector`.

**Architecture:** The current Brain layer carries ~400 lines of progressive-disclosure error handling, 3-tier error classification, plan deduplication, and multi-step orchestration. With smaller prompts (P2) and a better-calibrated tool surface (P5), the LLM can self-correct on its own from 3-part errors. Replace `FOliveSelfCorrectionPolicy::Evaluate()` with a single `ShouldRetry(Result, AttemptCount)` returning `true` once per transient error. `FOliveRunManager` becomes a linear loop: send message → receive response → execute tool calls → send tool results → repeat until the model stops or the loop detector fires. Delete the distiller and its call site.

**Tech Stack:** UE 5.5 C++, UE Automation tests.

**Related spec:** `docs/superpowers/specs/2026-04-18-plugin-makeover-design.md` §4.4.

**Note on dependencies:** P3 is independent of P1/P2/P4/P5/P6/P7 at code level. It can start in parallel with any of them, but touches shared Brain headers — merge after P1 if both change `OliveBrainState.h`.

---

## File Structure

**Replace (full rewrite, same file):**
- `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h`
- `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

**Slim (targeted edits, same file):**
- `Source/OliveAIEditor/Public/Chat/OliveRunManager.h`
- `Source/OliveAIEditor/Private/Chat/OliveRunManager.cpp`

**Simplify:**
- `Source/OliveAIEditor/Public/Brain/OliveOperationHistory.h` / `.cpp` — strip anything only used by self-correction's progressive disclosure or plan-hash dedup.

**Delete:**
- `Source/OliveAIEditor/Public/Brain/OlivePromptDistiller.h`
- `Source/OliveAIEditor/Private/Brain/OlivePromptDistiller.cpp`
- Any test files dedicated to the distiller or to progressive-disclosure behavior.

**Keep unchanged:**
- `Source/OliveAIEditor/Public/Brain/OliveBrainLayer.h` / `.cpp` — state machine (Idle/Active/Cancelling/Idle) stays.
- `Source/OliveAIEditor/Public/Brain/OliveRetryPolicy.h` — `FOliveLoopDetector` stays.

**Settings:**
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` — remove `MaxCorrectionCyclesPerRun` (line 304) and `PromptDistillationRawResults` (line 300). Keep `MaxProviderRetries`, `MaxRetryAfterWaitSeconds`.

---

## Tasks

### Task 1: Write retry-once policy tests (failing)

**Files:**
- Create: `Source/OliveAIEditor/Private/Tests/Brain/OliveSelfCorrectionPolicyTests.cpp`

- [ ] **Step 1: Author the test file**

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Brain/OliveSelfCorrectionPolicy.h"
#include "MCP/OliveToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveSelfCorrectionRetryOnceTest,
    "OliveAI.Brain.SelfCorrection.RetryOnceOnTransient",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveSelfCorrectionRetryOnceTest::RunTest(const FString& Parameters)
{
    FOliveToolResult TransientTimeout;
    TransientTimeout.bSuccess = false;
    TransientTimeout.ErrorCode = TEXT("TIMEOUT");

    FOliveSelfCorrectionPolicy Policy;

    TestTrue(TEXT("First transient error should trigger retry"),
        Policy.ShouldRetry(TransientTimeout, /*Attempt*/ 1));
    TestFalse(TEXT("Second transient error should NOT retry"),
        Policy.ShouldRetry(TransientTimeout, /*Attempt*/ 2));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveSelfCorrectionNoRetryOnUserErrorTest,
    "OliveAI.Brain.SelfCorrection.NoRetryOnUserError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveSelfCorrectionNoRetryOnUserErrorTest::RunTest(const FString& Parameters)
{
    FOliveToolResult ValidationError;
    ValidationError.bSuccess = false;
    ValidationError.ErrorCode = TEXT("VALIDATION_FAILED");

    FOliveSelfCorrectionPolicy Policy;

    TestFalse(TEXT("Validation errors should NOT trigger retry"),
        Policy.ShouldRetry(ValidationError, /*Attempt*/ 1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveSelfCorrectionRetryOnRateLimitTest,
    "OliveAI.Brain.SelfCorrection.RetryOnRateLimit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveSelfCorrectionRetryOnRateLimitTest::RunTest(const FString& Parameters)
{
    FOliveToolResult RateLimit;
    RateLimit.bSuccess = false;
    RateLimit.ErrorCode = TEXT("RATE_LIMIT");

    FOliveSelfCorrectionPolicy Policy;

    TestTrue(TEXT("Rate limits should trigger one retry"),
        Policy.ShouldRetry(RateLimit, /*Attempt*/ 1));
    return true;
}
```

- [ ] **Step 2: Run — expect fail (API doesn't match yet)**

Build. Tests should fail to compile because `FOliveSelfCorrectionPolicy::ShouldRetry` doesn't exist yet.

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: FAIL — compile error on `ShouldRetry`.

- [ ] **Step 3: Commit failing test**

```bash
git add Source/OliveAIEditor/Private/Tests/Brain/OliveSelfCorrectionPolicyTests.cpp
git commit -m "P3: failing tests for retry-once self-correction policy"
```

---

### Task 2: Replace `FOliveSelfCorrectionPolicy`

**Files:**
- Rewrite: `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h`
- Rewrite: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

- [ ] **Step 1: New header (whole file)**

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

/**
 * Minimal self-correction policy: retry exactly once on transient errors.
 *
 * Transient errors: TIMEOUT, RATE_LIMIT, HTTP_5xx.
 * Non-transient (validation, not found, execution errors, compile errors):
 * pass through. The LLM decides what to do next based on the 3-part error
 * (code + message + suggestion).
 */
class OLIVEAIEDITOR_API FOliveSelfCorrectionPolicy
{
public:
    FOliveSelfCorrectionPolicy() = default;

    /** Returns true iff this tool result should be retried. Attempt is 1-based. */
    bool ShouldRetry(const FOliveToolResult& Result, int32 Attempt) const;

private:
    static bool IsTransient(const FString& ErrorCode);
};
```

- [ ] **Step 2: New cpp (whole file)**

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "Brain/OliveSelfCorrectionPolicy.h"

bool FOliveSelfCorrectionPolicy::ShouldRetry(const FOliveToolResult& Result, int32 Attempt) const
{
    if (Result.bSuccess) return false;
    if (Attempt != 1) return false;
    return IsTransient(Result.ErrorCode);
}

bool FOliveSelfCorrectionPolicy::IsTransient(const FString& ErrorCode)
{
    if (ErrorCode.Equals(TEXT("TIMEOUT"), ESearchCase::IgnoreCase)) return true;
    if (ErrorCode.Equals(TEXT("RATE_LIMIT"), ESearchCase::IgnoreCase)) return true;
    if (ErrorCode.StartsWith(TEXT("HTTP_5"), ESearchCase::IgnoreCase)) return true;
    if (ErrorCode.Contains(TEXT("TRANSIENT"), ESearchCase::IgnoreCase)) return true;
    return false;
}
```

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: build fails where `FOliveSelfCorrectionPolicy::Evaluate()` or `PreviousPlanHashes` is referenced by call sites. Those are `FOliveRunManager`, `FOliveConversationManager`, and possibly `FOliveOperationHistory`. Fix them in Task 3.

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp
git commit -m "P3: replace self-correction policy with retry-once-on-transient"
```

---

### Task 3: Rewire call sites to use new ShouldRetry API

**Files:**
- Modify: `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` (call site of old `Policy.Evaluate(...)`)
- Modify: `Source/OliveAIEditor/Private/Chat/OliveRunManager.cpp` (if it calls the policy)
- Modify: any other file from build errors.

- [ ] **Step 1: Find old call sites**

Use Grep for: `SelfCorrectionPolicy`, `Policy.Evaluate`, `PreviousPlanHashes`, `BuildPlanHash`, `BuildToolErrorMessage`.

- [ ] **Step 2: Replace each Evaluate() call**

Each call of `Policy.Evaluate(Result, AttemptCount, ToolArgs)` becomes:

```cpp
if (Policy.ShouldRetry(Result, AttemptCount))
{
    // re-run the same tool call with same args
    // (the LLM is not re-prompted; we just retry the transport)
}
else
{
    // deliver the 3-part error to the LLM verbatim and let it decide
}
```

The old code that built progressive error messages (terse → full → escalate) is deleted. The raw `Result.ErrorCode + ": " + Result.ErrorMessage + "\n" + Result.Suggestion` is what the LLM sees.

- [ ] **Step 3: Remove PreviousPlanHashes and plan-dedup code**

Delete any struct member, function, or callsite referencing `PreviousPlanHashes`, `BuildPlanHash`, plan-content deduplication, and `FOliveLoopDetector::HashString` if the public-hash access was added specifically for this feature (check `OliveRetryPolicy.h` — HashString stays public if you use it elsewhere, otherwise return to private).

- [ ] **Step 4: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 5: Run the Task 1 tests**

Session Frontend > Automation > `OliveAI.Brain.SelfCorrection.*`. All three tests pass.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "P3: rewire call sites to new ShouldRetry API; remove plan-hash dedup"
```

---

### Task 4: Slim `FOliveRunManager` to a linear loop

**Files:**
- Modify: `Source/OliveAIEditor/Public/Chat/OliveRunManager.h`
- Modify: `Source/OliveAIEditor/Private/Chat/OliveRunManager.cpp`

- [ ] **Step 1: Read the current implementation**

Open both files. Identify: run-outcome state machine (`Completed`/`PartialSuccess`/`Failed`/`Cancelled`), 5-step checkpoint logic, multi-step orchestration (anything that tracks "step N of M"), context-window distillation calls.

- [ ] **Step 2: Rewrite the main loop**

The public entry point (likely `StartRun(FString Message, TFunction<void(EOliveRunOutcome)> OnComplete)`) must now:

```cpp
// Pseudocode shape. Preserve existing async/streaming primitives.
void FOliveRunManager::StartRun(const FString& UserMessage, FOnRunComplete OnComplete)
{
    ConversationManager.AppendUserMessage(UserMessage);
    TickLoop(OnComplete);
}

void FOliveRunManager::TickLoop(FOnRunComplete OnComplete)
{
    if (LoopDetector.IsStuck())
    {
        OnComplete.ExecuteIfBound(EOliveRunOutcome::Failed);
        return;
    }

    Provider.SendMessage(ConversationManager.GetHistory(), [this, OnComplete](FOliveProviderResponse Response)
    {
        if (Response.bStopped)
        {
            OnComplete.ExecuteIfBound(EOliveRunOutcome::Completed);
            return;
        }

        // Execute all tool calls in the response, sequentially on the game thread.
        for (const FOliveToolCall& Call : Response.ToolCalls)
        {
            ExecuteToolAndAppendResult(Call);
        }

        // Loop.
        TickLoop(OnComplete);
    });
}
```

Preserve existing threading model and cancellation guards (`AliveGuard` pattern from the CLI provider).

- [ ] **Step 3: Remove checkpointing**

Delete the code that calls `FOliveSnapshotManager::Snapshot()` every N steps inside the run. Manual `project.snapshot` still works; auto-checkpointing via RunManager is removed.

- [ ] **Step 4: Collapse run outcomes**

Replace `EOliveRunOutcome { Completed, PartialSuccess, Failed, Cancelled }` with:

```cpp
enum class EOliveRunOutcome : uint8
{
    Completed,
    Cancelled,
    Failed,      // fatal: loop detector tripped, or provider error after retry
};
```

Update all call sites.

- [ ] **Step 5: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add Source/OliveAIEditor/Public/Chat/OliveRunManager.h Source/OliveAIEditor/Private/Chat/OliveRunManager.cpp
git commit -m "P3: slim RunManager to a linear loop; collapse outcomes to 3"
```

---

### Task 5: Delete `FOlivePromptDistiller`

**Files:**
- Delete: `Source/OliveAIEditor/Public/Brain/OlivePromptDistiller.h`
- Delete: `Source/OliveAIEditor/Private/Brain/OlivePromptDistiller.cpp`
- Modify: every call site.

- [ ] **Step 1: Find all references**

Grep for `PromptDistiller`, `Distill`.

- [ ] **Step 2: Remove call sites**

Each call (typically in `FOliveConversationManager` or `FOliveRunManager`) that looked like `DistilledHistory = Distiller.Distill(FullHistory)` becomes simply: pass the full history through.

- [ ] **Step 3: Delete the files**

```bash
git rm Source/OliveAIEditor/Public/Brain/OlivePromptDistiller.h
git rm Source/OliveAIEditor/Private/Brain/OlivePromptDistiller.cpp
```

- [ ] **Step 4: Remove setting**

Edit `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`: delete the `PromptDistillationRawResults` UPROPERTY (around line 300).

Also delete `MaxCorrectionCyclesPerRun` (around line 304) — no longer consumed.

- [ ] **Step 5: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "P3: delete PromptDistiller and related settings"
```

---

### Task 6: Simplify `FOliveOperationHistory`

**Files:**
- Modify: `Source/OliveAIEditor/Public/Brain/OliveOperationHistory.h`
- Modify: `Source/OliveAIEditor/Private/Brain/OliveOperationHistory.cpp`

- [ ] **Step 1: Identify what self-correction consumed**

Look for methods like `GetFailuresForCurrentStep`, `ClassifyErrors`, or fields tracking per-error history used only by the old progressive disclosure.

- [ ] **Step 2: Delete unused history tracking**

Remove any field, method, or data structure that only served the old `Evaluate()` logic. Keep:
- Append-new-operation
- Read-recent-operations-for-UI-feed
- Clear-on-new-run

Anything else is cut.

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 4: Run all brain tests**

Session Frontend > Automation > `OliveAI.Brain.*`. All pass.

- [ ] **Step 5: Commit**

```bash
git add Source/OliveAIEditor/Public/Brain/OliveOperationHistory.h Source/OliveAIEditor/Private/Brain/OliveOperationHistory.cpp
git commit -m "P3: simplify OperationHistory, remove self-correction-only fields"
```

---

### Task 7: Smoke test and CLAUDE.md update

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: End-to-end smoke test**

Open UE Editor. Open Olive AI chat panel. Send two prompts:

1. "Create BP_SmokeTest that prints 'ok' on BeginPlay." — should succeed.
2. "Call a tool that doesn't exist." — LLM should receive a `TOOL_NOT_FOUND` error (or equivalent) and decide what to do on its own, without progressive-disclosure escalation.

- [ ] **Step 2: Update CLAUDE.md**

In the "Brain Layer" section, rewrite to describe the new shape:
- `FOliveSelfCorrectionPolicy` → retry-once-on-transient-error (one method `ShouldRetry`).
- `FOliveRunManager` → linear loop.
- `FOlivePromptDistiller` → removed.
- `EOliveRunOutcome` → 3 values (`Completed`, `Cancelled`, `Failed`).

In "Key Singletons" table: remove the distiller row. Update the self-correction row.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "P3: update CLAUDE.md for simplified brain layer"
```

---

## Acceptance criteria

1. `ubt-build-5.5` green.
2. `OliveAI.*` automation suite green, including three new `OliveAI.Brain.SelfCorrection.*` tests.
3. Grep for `Evaluate\s*\(` in `OliveSelfCorrectionPolicy` returns zero hits (old API gone).
4. `OlivePromptDistiller.h`/`.cpp` deleted.
5. `PromptDistillationRawResults` and `MaxCorrectionCyclesPerRun` settings removed.
6. `FOliveSelfCorrectionPolicy` LOC ≤ 40 lines (vs ~400 before).
7. Smoke test: end-to-end Blueprint creation via chat panel works.
8. `CLAUDE.md` updated.

## Out of scope

- Provider simplification (legacy Claude Code settings remain).
- Tool consolidation (P5).
- Mode removal (P1).
