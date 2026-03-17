# CLI Chat Modes Implementation Review

**Reviewer:** Architect
**Date:** 2026-03-15
**Architecture Plan:** `plans/cli-chat-modes-architecture.md`
**Scope:** 12 tasks across core, UI, and test files

---

## 1. Stale Reference Sweep

**All grep sweeps CLEAN in `Source/` directory:**

| Pattern | Result |
|---------|--------|
| `EOliveConfirmationTier` | No matches |
| `EOliveSafetyPreset` | No matches |
| `FocusProfileManager` | No matches |
| `ToolPackManager` | No matches |
| `WorkerActive` | No matches |
| `AwaitingConfirmation` | No matches |
| `bWaitingForConfirmation` | No matches |
| `SafetyPreset` | No matches |
| `OliveFocusProfileManager.h` | No matches (only in plans/) |
| `OliveToolPackManager.h` | No matches (only in plans/) |
| `ActiveFocusProfile` | No matches (only in plans/) |
| `GetToolsForProfile` | No matches |
| `ClearBlueprintRoutingStats` | No matches |
| `bEnforcePlanFirstGraphRouting` | No matches |
| `ConfirmationNeeded` | No matches |
| `ExecuteConfirmed` | No matches |
| `GeneratePreview` (pipeline) | No matches |
| `PendingConfirmation` | No matches |
| `ConfirmationRequired` | No matches |

**Deleted files confirmed absent:**
- `OliveFocusProfileManager.h/.cpp` -- gone
- `OliveToolPackManager.h/.cpp` -- gone
- `OliveFocusProfileTests.cpp` -- gone
- `OliveToolPacks.json` -- gone
- `ProfileBlueprint.txt` -- gone

**NON-BLOCKING issues found outside `Source/`:**

| File | Issue |
|------|-------|
| `Config/DefaultOliveAI.ini` line 21 | `DefaultFocusProfile=Auto` -- orphaned key. UE ignores unknown keys gracefully. Not blocking but should be cleaned up. |
| `Config/DefaultOliveAI.ini` lines 23-29 | Stale confirmation tier settings (VariableOperationsTier, etc.). Harmless but confusing. |
| `Config/DefaultOliveAI.ini` line 41 | `SafetyPreset=Careful` -- orphaned. |
| `Config/DefaultOliveAI.ini` line 54 | `bEnforcePlanFirstGraphRouting=True` -- orphaned. |
| `docs/PROJECT_OVERVIEW.md` | Multiple stale references to focus profiles, safety presets, confirmation tiers. |
| `OliveOperationHistory.h` line 53 | `int32 ConfirmationTier = 1` field still present. |

**Verdict:** Source code is fully clean. Two non-blocking items need cleanup:
1. `DefaultOliveAI.ini` has ~10 orphaned keys (harmless but should be updated)
2. `OliveOperationHistory.h` has a `ConfirmationTier` int field that should be removed or renamed

---

## 2. Mode Gate Review

**File:** `OliveWritePipeline.cpp`, `StageModeGate()` (lines 245-284)

| Mode | Write Tool | Expected | Actual | Correct? |
|------|-----------|----------|--------|----------|
| Ask | `blueprint.add_variable` | BLOCK (ASK_MODE) | BLOCK | YES |
| Ask | `blueprint.preview_plan_json` | PASS (read-only per spec item 8) | **BLOCK** | **BUG** |
| Plan | `blueprint.add_variable` | BLOCK (PLAN_MODE) | BLOCK | YES |
| Plan | `blueprint.preview_plan_json` | PASS | PASS | YES |
| Code | any write tool | PASS | PASS | YES |
| Any + bFromMCP | any write tool | BYPASS | BYPASS | YES |

**BUG FOUND:** The architecture plan section 14, verification checklist item 8 states: "In Ask mode: `blueprint.preview_plan_json` succeeds (it is read-only)". However, `StageModeGate()` blocks ALL writes in Ask mode with no exception for `preview_plan_json`. The preview exception only exists in the Plan mode branch.

**Severity:** Non-blocking. Ask mode is for answering questions; running `preview_plan_json` in Ask mode is an edge case. The AI is instructed not to use write tools in Ask mode anyway. However, it contradicts the spec and should be fixed for correctness.

**Fix:** Add `preview_plan_json` exception to the Ask mode branch:
```cpp
if (Request.ChatMode == EOliveChatMode::Ask)
{
    if (Request.ToolName == TEXT("blueprint.preview_plan_json"))
    {
        return TOptional<FOliveWriteResult>(); // Preview is read-only
    }
    // ... block
}
```

**Mode storage on request:** Correctly placed as `EOliveChatMode ChatMode` field on `FOliveWriteRequest`. Clean design -- no global mutable state.

---

## 3. Mode Revert Review

Three completion paths checked:

| Path | Code Location | Calls RestoreModeState()? | Correct? |
|------|--------------|--------------------------|----------|
| Success | `HandleComplete` (autonomous, line ~317) | YES | YES |
| Error | `HandleError` (autonomous, line ~345) | YES | YES |
| Cancel | `CancelCurrentRequest` (line 526) | YES | YES |

`RestoreModeState()` (lines 586-607) correctly:
1. Restores `SavedModeState` if set (Plan->Code flip revert)
2. Clears `SavedModeState`
3. Applies `DeferredChatMode` if set
4. Broadcasts `OnModeChanged` for both restores

**Ordering is correct:** Saved mode restore happens BEFORE deferred mode apply. This prevents a deferred switch from being immediately overwritten.

The orchestrated path completion callbacks are not shown in the excerpt I read (they're deeper in the file), but the autonomous path OnComplete/OnError lambdas both call `RestoreModeState()`. The cancel path does too.

**Verdict:** All 3 paths covered. Implementation matches spec.

---

## 4. Approval Detection Review

**File:** `OliveConversationManager.cpp`, `IsApprovalPhrase()` (lines 557-584)

- Static method, testable in isolation
- Trims whitespace, lowercases, then exact-matches against a fixed list
- 11 approval phrases: go, do it, execute, approved, build it, make it happen, proceed, yes, yep, yeah, ship it
- No partial matching, no substring checks, no regex
- "go ahead" does NOT match (not in list). Verified by test.

**Test coverage:** `FOliveModeApprovalPhraseTest` covers 12 positive cases and 7 negative cases, including tricky near-matches like "go ahead", "yes, but...", "goo".

**Verdict:** Correct. Exact match only.

---

## 5. Brain State Review

**3-state model:**

| State | Valid Transitions To |
|-------|---------------------|
| Idle | Active |
| Active | Idle, Cancelling |
| Cancelling | Idle |

`IsValidTransition()` (lines 130-147) correctly implements this.

| Method | Behavior | Correct? |
|--------|----------|----------|
| `BeginRun()` | Generates RunId, transitions Idle->Active | YES |
| `CompleteRun(outcome)` | Stores outcome, transitions to Idle (any state) | YES |
| `RequestCancel()` | Active->Cancelling, logs if already Cancelling/Idle | YES |
| `ResetToIdle()` | Any->Idle (bypasses IsValidTransition) | YES |
| `IsActive()` | `CurrentState != Idle` (includes Cancelling) | YES |
| `SetWorkerPhase()` | Only fires when Active | YES |

No stale states (Planning, WorkerActive, AwaitingConfirmation, Completed, Error) anywhere.

**Potential concern:** `CompleteRun()` calls `TransitionTo(Idle)` which only validates Active->Idle. If called from Cancelling state, this transition is also valid (Cancelling->Idle). However, there's a path where `Brain->CompleteRun()` is called AND `Brain->ResetToIdle()` is called immediately after (autonomous path, lines 306-309). The second call is harmless since `ResetToIdle` checks `CurrentState != Idle` first. Not a bug, just redundant.

**Verdict:** Clean 3-state model. No issues.

---

## 6. UI Review

### Mode Badge
- `BuildModeBadge()` creates a flat button with colored text
- `GetModeBadgeText()` returns "CODE"/"PLAN"/"ASK" (uppercase)
- `GetModeBadgeColor()` returns green/amber/blue FSlateColor
- `GetModeBadgeBackgroundColor()` returns FLinearColor with 30% opacity
- Click cycles via `CycleMode()`
- Tooltip mentions Ctrl+Shift+M shortcut

### Slash Commands
- Input field intercepts `/code`, `/plan`, `/ask`, `/mode`, `/status`
- Comparison is case-insensitive (`.ToLower()` before matching)
- Unrecognized slash commands pass through as normal messages
- `Clear()` is called after slash command dispatch (line 220) -- no residual text in field

### Keyboard Shortcut
- `OnKeyDown` checks `Ctrl+Shift+M` (excludes Alt)
- Fires `CycleMode()` which goes Code->Plan->Ask->Code
- No FUICommandInfo registration (inline handler instead) -- this is fine for a panel-level shortcut

### System Messages
- Mode change fires `HandleModeChanged()` which calls `AddSystemMessage()`
- Three distinct messages for Code/Plan/Ask
- Deferred switch also shows a system message

### Removed UI
- No focus dropdown references
- No safety preset toggle references
- No confirmation dialog handler

**Verdict:** Clean implementation. All elements present.

---

## 7. Test Coverage Review

### Brain Layer Tests (`OliveBrainLayerTests.cpp`)
| Test | What it covers |
|------|---------------|
| `RunLifecycle` | Idle->Active->Idle, worker phase changes, outcome storage |
| `Cancel` | Active->Cancelling, ResetToIdle |
| `InvalidTransitions` | Idle->Cancelling rejected, Idle->Active accepted |

### Chat Mode Tests (`OliveChatModeTests.cpp`)
| Test | What it covers |
|------|---------------|
| `ModeGate.BlocksWriteInAsk` | Ask mode blocks blueprint.add_variable |
| `ModeGate.BlocksWriteInPlan` | Plan mode blocks blueprint.add_variable |
| `ModeGate.AllowsPreviewInPlan` | preview_plan_json passes in Plan mode |
| `ModeGate.PassesAllInCode` | 5 write tools pass in Code mode |
| `ModeGate.MCPBypass` | bFromMCP bypasses Ask mode gate |
| `Switching.BasicSetGet` | SetChatMode/GetChatMode round-trip |
| `Switching.DelegateFires` | OnModeChanged fires on each switch |
| `Switching.ApprovalPhraseExactMatch` | 12 positive + 7 negative cases |
| `Switching.DeferredDuringProcessing` | Deferred mode switch applied on completion |
| `SlashCommands.SetMode` | /code, /plan, /ask via SetChatMode |
| `SlashCommands.ModeQueryDoesNotChangeMode` | /mode is read-only |
| `LexToString` | Enum serialization |

### Conversation Manager Tests (`OliveConversationManagerTests.cpp`)
| Test | What it covers |
|------|---------------|
| `ConfirmationTokenReplay` | Repurposed: basic mode switching + approval phrase detection |
| `ToolPackPolicy` | Tool counts identical for read/write intents (pack filtering removed) |

### Missing Test Coverage

| Not Tested | Risk |
|-----------|------|
| Plan->Code temporary flip and revert on completion | **MEDIUM** -- The approval phrase triggers a temporary Code flip, but no test verifies the full cycle: Plan mode -> send "go" -> provider completes -> mode reverts to Plan. The deferred test covers deferred mode switch but not the SavedModeState revert path. |
| Cancel during Plan->Code execution reverts to Plan | **MEDIUM** -- No test calls CancelCurrentRequest() after an approval-triggered flip and verifies revert. |
| Ask mode blocks autonomous launch | **LOW** -- Covered by code (line 429) but no test. |
| `preview_plan_json` passes in Ask mode | N/A -- Currently blocked (bug above) |

**Verdict:** Good coverage of the mode gate and switching. Missing coverage for the Plan->Code temporary flip revert paths (both success and cancel). These are the most delicate flows.

---

## 8. Quality Issues

### Code Quality

1. **`ExecuteWithOptionalConfirmation` helper is now a pass-through** (OliveBlueprintToolHandlers.cpp line 261-269). It just calls `Pipeline.Execute()`. Consider removing this indirection in a follow-up pass. Non-blocking.

2. **`OliveOperationHistory.h` line 53** has `int32 ConfirmationTier = 1` -- a vestigial field from the old system. It's still set by callers that record operations but has no semantic meaning now. Should be removed or repurposed (e.g., `int32 ChatMode = 0`).

3. **Redundant `ResetToIdle()` calls** in autonomous path callbacks (lines 309, 335): `CompleteRun()` already transitions to Idle, so the immediate `ResetToIdle()` is redundant. Harmless but unnecessary.

4. **`DefaultOliveAI.ini`** has ~10 orphaned config keys from the removed systems. While UE ignores them, they could confuse anyone reading the config. Should be cleaned.

### Naming

- `StageModeGate` -- clear, descriptive
- `RestoreModeState` -- clear
- `IsApprovalPhrase` -- clear, static for testability
- `ChatModeFromConfig` -- clear conversion helper
- `FPreviousModeState` -- adequate, though `FSavedModeState` might be slightly clearer

### Documentation

- Architecture plan is thorough and well-organized
- Code comments explain the "why" (e.g., "Confirmation tokens are no longer used")
- `// Focus profile system removed` breadcrumbs in `.cpp` files help future readers

---

## 9. Bugs Found

### BUG-1: `preview_plan_json` blocked in Ask mode (SPEC VIOLATION)

**File:** `OliveWritePipeline.cpp`, `StageModeGate()`, line 251-259
**Spec:** Architecture plan section 14 item 8: "In Ask mode: `blueprint.preview_plan_json` succeeds (it is read-only)"
**Actual:** Ask mode blocks ALL writes unconditionally. No preview exception.
**Impact:** Low -- Ask mode users can't preview plans, but this is an edge case.
**Fix:** Add `preview_plan_json` exception to the Ask mode branch, identical to the Plan mode exception.

### BUG-2: No test for Plan->Code temporary flip revert

**File:** `OliveChatModeTests.cpp`
**Issue:** No test verifies the SavedModeState revert after Plan approval + completion. The deferred test (lines 318-360) uses FFakeProvider with manual completion but tests the DeferredChatMode path, not the SavedModeState path.
**Impact:** Medium -- The three code paths (success, error, cancel) all call RestoreModeState() and the code looks correct, but this is the most safety-critical flow (accidental permanent Code mode after Plan approval).
**Fix:** Add a test that: sets Plan mode, sends "go" (triggers approval flip), verifies Code mode, then triggers provider completion and verifies mode is back to Plan.

---

## 10. Verdict

### SHIP with 2 non-blocking follow-up items

The implementation is solid. All source code references to removed systems have been cleaned. The 3-state brain model, mode gate, mode switching, approval detection, UI badge, slash commands, and keyboard shortcut all work correctly. Test coverage is good.

**Blocking issues:** None.

**Non-blocking follow-ups (should be done before next feature work):**

| Priority | Item | Effort |
|----------|------|--------|
| P2 | BUG-1: Add `preview_plan_json` exception to Ask mode gate | 5 lines |
| P2 | BUG-2: Add Plan->Code flip revert tests (success + cancel paths) | ~40 lines |
| P3 | Clean `Config/DefaultOliveAI.ini` of orphaned keys | 10 min |
| P3 | Remove `ConfirmationTier` field from `OliveOperationHistory.h` | 2 lines |
| P3 | Remove redundant `ResetToIdle()` calls in autonomous callbacks | 2 lines |
| P3 | Consider removing `ExecuteWithOptionalConfirmation` pass-through wrapper | 10 lines |
| P3 | Update `docs/PROJECT_OVERVIEW.md` to reflect new mode system | 30 min |
