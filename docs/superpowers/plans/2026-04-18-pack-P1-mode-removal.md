# Pack P1 — Mode Removal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove Ask/Plan/Code chat modes. Plugin operates in a single always-on mode equivalent to today's Code. Eliminate mode gating from the write pipeline, mode UI badge, slash commands, settings, and related tests.

**Architecture:** Delete `EOliveChatModeConfig` and `EOliveChatMode` enums. Drop Stage 2 (Mode Gate) from `FOliveWritePipeline` — pipeline goes from 6 stages to 5. Remove `ChatMode` field from `FOliveWriteRequest`. Strip mode-conditional branches from tool handlers. Remove mode badge widget from `SOliveAIChatPanel`. Remove `/ask`, `/plan` slash commands; `/code` becomes silent no-op alias for one release.

**Tech Stack:** UE 5.5 C++, Slate, UE Automation tests.

**Related spec:** `docs/superpowers/specs/2026-04-18-plugin-makeover-design.md` §5.

---

## File Structure

**Delete:**
- `Source/OliveAIEditor/Private/Tests/Brain/OliveChatModeTests.cpp`

**Modify (remove mode references):**
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` — remove `EOliveChatModeConfig` enum (lines 30-36) and `DefaultChatMode` property (lines 246-250)
- `Source/OliveAIEditor/Private/Settings/OliveAISettings.cpp` — remove any DefaultChatMode initialization
- `Source/OliveAIEditor/Public/Brain/OliveBrainState.h` — remove `EOliveChatMode` enum
- `Source/OliveAIEditor/Public/Brain/OliveToolExecutionContext.h` — remove ChatMode field
- `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h` — remove `ChatMode` from `FOliveWriteRequest`, remove `EOliveWriteStage::ModeGate`, remove `StageModeGate()` signature
- `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp` — delete Stage 2 block (lines 155-168), delete `StageModeGate()` impl (lines 258-296)
- `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h` — remove mode persistence
- `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` — remove mode logic
- `Source/OliveAIEditor/Public/UI/SOliveAIChatPanel.h` — remove mode badge widget decl
- `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp` — remove mode badge widget impl, `/ask` and `/plan` handlers
- `Source/OliveAIEditor/Public/MCP/OliveMCPServer.h` — remove mode propagation to `FOliveToolExecutionContext`
- `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp` — remove mode propagation
- `Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h` — remove any mode fields
- `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp` — remove any mode checks
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` — remove mode-conditional branches
- `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h` / `.cpp` — remove mode-specific prompt sections (will be fully rewritten in P2; P1 only needs to compile)
- `Source/OliveAIEditor/Private/Tests/Conversation/OliveConversationManagerTests.cpp` — remove mode-dependent test cases

**All occurrences of:** `ASK_MODE`, `PLAN_MODE`, `EOliveChatMode`, `EOliveChatModeConfig`, `DefaultChatMode`, `ChatMode` (in pipeline/request/handler context), `ModeGate`, `StageModeGate`, `bRequiresModeCheck`.

Use `Grep` with these terms to find every occurrence before starting. The set of 19 files identified by the grep is the full impact surface.

---

## Tasks

### Task 1: Inventory mode touchpoints (no code yet)

**Files:** none changed.

- [ ] **Step 1: Run grep and write inventory**

Run each grep in order:

```bash
# From plugin root
```

Use the `Grep` tool:
- pattern `EOliveChatMode|EOliveChatModeConfig`
- pattern `ASK_MODE|PLAN_MODE`
- pattern `StageModeGate|ModeGate`
- pattern `ChatMode` (excluding false positives — only where it refers to Ask/Plan/Code, not "chat" messages)
- pattern `/ask |/plan |SlashCommand.*[Aa]sk|SlashCommand.*[Pp]lan`

Write the full list of `file:line` hits to `docs/superpowers/plans/p1-mode-inventory.md`. This is your working checklist; you will verify zero hits at the end.

- [ ] **Step 2: Commit inventory**

```bash
git add docs/superpowers/plans/p1-mode-inventory.md
git commit -m "P1: inventory mode touchpoints"
```

---

### Task 2: Delete `FOliveWritePipeline` Stage 2 (Mode Gate)

**Files:**
- Modify: `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h`
- Modify: `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`

- [ ] **Step 1: Write a failing compile-level check first**

Open `OliveWritePipeline.h`. Confirm `EOliveWriteStage` enum contains `ModeGate`. Confirm `FOliveWriteRequest` contains `ChatMode`. Confirm `StageModeGate()` is declared. These are your "asserts" — they must disappear.

- [ ] **Step 2: Remove from header**

Edit `OliveWritePipeline.h`:
- Remove `ModeGate` from `EOliveWriteStage` enum.
- Remove `ChatMode` field from `FOliveWriteRequest` struct (leave every other field alone).
- Remove `bFromMCP` field ONLY if it is exclusively used for mode gating — check call sites first. If used for anything else (logging, bypass), keep it.
- Remove `StageModeGate()` method declaration.
- Remove `#include` for chat-mode headers if any.

- [ ] **Step 3: Remove from cpp**

Edit `OliveWritePipeline.cpp`:
- Delete the Stage 2 block (around lines 155-168): the comment `// Stage 2: Mode Gate` and the scoped call to `StageModeGate(Request)`.
- Delete the full `FOliveWritePipeline::StageModeGate(...)` implementation (around lines 258-296).
- Update the Stage numbering in any comments: Stage 3 becomes Stage 2, etc.
- Remove any `#include "Brain/OliveBrainState.h"` if it was only used for the mode enum.

- [ ] **Step 4: Build**

Run via the UBT bridge (user must have started `S:\docker-os\start-ubt-bridge-host.bat`):

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: compile errors in callers that reference `Request.ChatMode` or `StageModeGate`. Record the error list — you will fix these in Task 3.

- [ ] **Step 5: Commit (build is expected to fail here — that's the TDD red)**

```bash
git add Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp
git commit -m "P1: remove Stage 2 (Mode Gate) from write pipeline"
```

---

### Task 3: Remove `ChatMode` from all call sites and tool handlers

**Files:**
- Modify: every file reported by build errors in Task 2 Step 4.
- Modify: `Source/OliveAIEditor/Public/Brain/OliveToolExecutionContext.h` (drop `ChatMode` field).
- Modify: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (remove any mode-conditional branches feeding `ChatMode` into the request).

- [ ] **Step 1: Fix every compile error from Task 2**

For each build error:
- If it's `Request.ChatMode = ...`: delete that line.
- If it's `if (ChatMode == EOliveChatMode::...)`: delete the conditional entirely. The code inside the condition must remain (it is the always-allowed path).
- If it's a constructor arg: remove the arg and the corresponding parameter in the call.

- [ ] **Step 2: Remove ChatMode from FOliveToolExecutionContext**

Edit `OliveToolExecutionContext.h`: remove `ChatMode` member. Remove its initialization in any constructor. Update `.cpp` if one exists.

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "P1: remove ChatMode from tool execution context and call sites"
```

---

### Task 4: Delete `EOliveChatMode` and `EOliveChatModeConfig` enums

**Files:**
- Modify: `Source/OliveAIEditor/Public/Brain/OliveBrainState.h` — delete `EOliveChatMode` enum.
- Modify: `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` — delete `EOliveChatModeConfig` enum (lines 30-36) and `DefaultChatMode` property (lines 246-250).
- Modify: `Source/OliveAIEditor/Private/Settings/OliveAISettings.cpp` — remove any `DefaultChatMode` initialization.

- [ ] **Step 1: Delete enums**

Remove both enum blocks entirely. Leave surrounding code intact.

- [ ] **Step 2: Delete settings property**

Remove the `DefaultChatMode` UPROPERTY block (lines 246-250 in settings header). If `DefaultOliveAI.ini` has a `DefaultChatMode=` entry, leave it — ini entries for missing properties are ignored by UE.

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build. Any remaining reference to the deleted enums will break the build — fix them.

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Public/Brain/OliveBrainState.h Source/OliveAIEditor/Public/Settings/OliveAISettings.h Source/OliveAIEditor/Private/Settings/OliveAISettings.cpp
git commit -m "P1: delete EOliveChatMode and EOliveChatModeConfig enums"
```

---

### Task 5: Remove slash commands and mode badge from chat panel

**Files:**
- Modify: `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp` — remove mode badge Slate widget and `/ask`, `/plan` slash command handlers. `/code` becomes silent no-op (prints nothing, swallows input) for one release.
- Modify: `Source/OliveAIEditor/Public/UI/SOliveAIChatPanel.h` — remove mode badge widget declaration.
- Modify: `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h` / `.cpp` — remove mode persistence (ConversationManager state).
- Modify: `Source/OliveAIEditor/Public/Chat/OliveEditorChatSession.h` / `.cpp` — remove mode-change broadcast.

- [ ] **Step 1: Remove the mode badge widget**

In `SOliveAIChatPanel.cpp`, locate the Slate declaration that builds the mode indicator (look for "Mode" `STextBlock` and the border around it). Delete the widget and its child bindings. Remove corresponding `SLATE_ARGUMENT` / member variables in the header.

- [ ] **Step 2: Update slash command handler**

In the slash-command dispatch (search for `StartsWith(TEXT("/")`):
- Remove branches for `/ask` and `/plan`.
- For `/code`: replace body with a one-line comment: `// /code is a silent no-op after mode removal (back-compat)`.
- Remove the slash-command discovery string list if it exposes these to users.

- [ ] **Step 3: Remove mode persistence**

In `OliveConversationManager.h/.cpp`: remove member `CurrentMode`, remove getter/setter, remove any serialization.

In `OliveEditorChatSession.h/.cpp`: remove `OnModeChanged` delegate and its broadcasts.

- [ ] **Step 4: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 5: Manual smoke test**

Open editor. Open Olive AI chat panel. Confirm:
- No mode badge visible.
- Typing `/ask hi` or `/plan hi` produces no mode-switch toast (they either type through or are ignored).
- Typing `/code` is silently consumed.

- [ ] **Step 6: Commit**

```bash
git add Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp Source/OliveAIEditor/Public/UI/SOliveAIChatPanel.h Source/OliveAIEditor/Public/Chat/OliveConversationManager.h Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp Source/OliveAIEditor/Public/Chat/OliveEditorChatSession.h Source/OliveAIEditor/Private/Chat/OliveEditorChatSession.cpp
git commit -m "P1: remove mode badge, slash commands, and mode persistence from chat UI"
```

---

### Task 6: Delete mode-related tests

**Files:**
- Delete: `Source/OliveAIEditor/Private/Tests/Brain/OliveChatModeTests.cpp`
- Modify: `Source/OliveAIEditor/Private/Tests/Conversation/OliveConversationManagerTests.cpp` — remove mode-dependent test cases.

- [ ] **Step 1: Delete the dedicated mode test file**

```bash
git rm Source/OliveAIEditor/Private/Tests/Brain/OliveChatModeTests.cpp
```

- [ ] **Step 2: Strip mode cases from conversation manager tests**

Open `OliveConversationManagerTests.cpp`. Any test that sets or asserts on `CurrentMode`, `DefaultChatMode`, or mode-change behavior: delete the whole `IMPLEMENT_SIMPLE_AUTOMATION_TEST` block. Other tests stay.

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 4: Run all OliveAI automation tests**

In UE editor: `Tools > Test Automation > Session Frontend`. Filter `OliveAI.*`. Run all. Expected: all remaining tests pass.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "P1: delete mode-related tests"
```

---

### Task 7: Final verification

**Files:** none changed.

- [ ] **Step 1: Grep for any lingering references**

Run each grep — all must return zero hits:

- `EOliveChatMode` (both variants)
- `ASK_MODE`
- `PLAN_MODE`
- `StageModeGate`
- `DefaultChatMode`

- [ ] **Step 2: Check that remaining `ChatMode` hits are unrelated**

Run `Grep` for `ChatMode`. Every remaining hit must be in a comment, docs, or a string literal unrelated to mode gating. If any compile-reaching code still references a `ChatMode` symbol, return to Task 3.

- [ ] **Step 3: Run full test suite**

Session Frontend > Automation > filter `OliveAI.*` > Run All. All green.

- [ ] **Step 4: Update CLAUDE.md**

Open plugin `CLAUDE.md`. In the "Write Pipeline (6 Stages)" section, change to "Write Pipeline (5 Stages)" and remove Stage 2 (Mode Gate) entry. Renumber subsequent stages. In the "Chat Modes" section, delete the entire subsection.

- [ ] **Step 5: Final commit**

```bash
git add CLAUDE.md
git commit -m "P1: update CLAUDE.md for 5-stage write pipeline and no modes"
```

---

## Acceptance criteria

1. `ubt-build-5.5` green.
2. `OliveAI.*` automation suite green.
3. Zero grep hits for `EOliveChatMode`, `EOliveChatModeConfig`, `ASK_MODE`, `PLAN_MODE`, `StageModeGate`, `DefaultChatMode`.
4. Chat panel opens; no mode badge; `/ask`, `/plan`, `/code` all behave as described.
5. `CLAUDE.md` updated.

## Out of scope

- Prompt rewriting (that's P2).
- Brain layer refactor (that's P3).
- Tool consolidation (that's P5).
- Removing `bEnableLegacyClaudeCodeProvider` or any other non-mode settings.
