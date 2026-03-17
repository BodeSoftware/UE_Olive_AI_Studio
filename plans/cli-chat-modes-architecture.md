# Architecture Plan: CLI-Style Chat Modes (Code / Plan / Ask)

**Author:** Architect
**Date:** 2026-03-15
**Design Spec:** `plans/cli-chat-modes-design.md`
**Status:** READY FOR REVIEW

---

## 1. New Types

### 1.1 EOliveChatMode (new enum)

**File:** `Source/OliveAIEditor/Public/Brain/OliveBrainState.h`

Place immediately after the existing `EOliveRunOutcome` enum. This file already houses all brain/run enums and their `LexToString` overloads.

```cpp
/**
 * Chat interaction mode -- controls tool access and AI behavior.
 * Modeled after Claude Code CLI's /code, /plan, /ask commands.
 */
enum class EOliveChatMode : uint8
{
    Code,   // Full autonomous execution -- all tools, no confirmation except destructive ops
    Plan,   // Read + plan -- write tools return PLAN_MODE error, preview allowed
    Ask     // Read-only -- write tools return ASK_MODE error
};

inline const TCHAR* LexToString(EOliveChatMode Mode)
{
    switch (Mode)
    {
    case EOliveChatMode::Code: return TEXT("Code");
    case EOliveChatMode::Plan: return TEXT("Plan");
    case EOliveChatMode::Ask:  return TEXT("Ask");
    default: return TEXT("Unknown");
    }
}
```

We also need a UENUM mirror in settings for Config serialization. Add to `OliveAISettings.h` near the top where the other UENUMs live:

```cpp
UENUM(BlueprintType)
enum class EOliveChatModeConfig : uint8
{
    Code UMETA(DisplayName = "Code (Autonomous)"),
    Plan UMETA(DisplayName = "Plan (Review First)"),
    Ask  UMETA(DisplayName = "Ask (Read-Only)")
};
```

Conversion helpers (free functions in `OliveBrainState.h`):

```cpp
inline EOliveChatMode ChatModeFromConfig(EOliveChatModeConfig C)
{
    switch (C)
    {
    case EOliveChatModeConfig::Plan: return EOliveChatMode::Plan;
    case EOliveChatModeConfig::Ask:  return EOliveChatMode::Ask;
    default: return EOliveChatMode::Code;
    }
}
```

### 1.2 Simplified Brain States

**File:** `Source/OliveAIEditor/Public/Brain/OliveBrainState.h`

Replace the 7-state `EOliveBrainState` with 3 states:

```cpp
enum class EOliveBrainState : uint8
{
    Idle,           // No active operation -- last run outcome available via GetLastOutcome()
    Active,         // AI is working (streaming, tools, compiling, self-correcting)
    Cancelling      // User hit Stop -- draining in-flight ops
};
```

Remove `Planning`, `WorkerActive`, `AwaitingConfirmation`, `Completed`, `Error`. Update `LexToString` accordingly.

**Key change:** `BeginRun()` transitions `Idle -> Active`. `CompleteRun()` transitions `Active -> Idle` (stores outcome but does not have a terminal state). `RequestCancel()` transitions `Active -> Cancelling`. Cancelling drains and transitions `Cancelling -> Idle`.

---

## 2. File-by-File Change List

### 2.1 Files to MODIFY

| # | File | Changes |
|---|------|---------|
| 1 | `Source/OliveAIEditor/Public/Brain/OliveBrainState.h` | Replace 7-state enum with 3-state, add `EOliveChatMode`, add `LexToString` overloads, add config conversion helper |
| 2 | `Source/OliveAIEditor/Public/Brain/OliveBrainLayer.h` | Update `IsActive()`, update `BeginRun()`/`CompleteRun()` for 3-state model. Remove `AwaitingConfirmation` references. |
| 3 | `Source/OliveAIEditor/Private/Brain/OliveBrainLayer.cpp` | Rewrite `IsValidTransition()` for 3 states. `CompleteRun()` always transitions to Idle. `BeginRun()` transitions to Active. Remove `WorkerActive` -> `AwaitingConfirmation` path. `SetWorkerPhase()` checks `Active` not `WorkerActive`. |
| 4 | `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` | Remove `EOliveConfirmationTier`, `EOliveSafetyPreset`, all 7 tier UPROPERTYs, `SafetyPreset`, `GetEffectiveTier()`, `SetSafetyPreset()`, `OnSafetyPresetChanged`, `DefaultFocusProfile`, `CustomFocusProfilesJson`, `CustomFocusProfilesSchemaVersion`, `bEnforcePlanFirstGraphRouting`, `PlanFirstGraphRoutingThreshold`. Add `EOliveChatModeConfig` UENUM and `DefaultChatMode` UPROPERTY. |
| 5 | `Source/OliveAIEditor/Private/Settings/OliveAISettings.cpp` | Remove `GetEffectiveTier()`, `SetSafetyPreset()`, `OnSafetyPresetChanged` static. Add `PostInitProperties()` migration from old `SafetyPreset` to `DefaultChatMode`. |
| 6 | `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h` | Remove `EOliveConfirmationRequirement` enum. Remove `ConfirmationRequired`, `PlanDescription`, `PreviewData` from `FOliveWriteResult`. Remove `ConfirmationNeeded()` factory. Remove `ExecuteConfirmed()`, `GeneratePreview()`. Remove `GetOperationTier()`, `TierToRequirement()`, `GeneratePlanDescription()`, `BuildPreviewPayload()`, `BuildImpactAnalysis()`, `BuildStructuredChanges()`, `PendingConfirmations`, `ConfirmationLock`, `GenerateConfirmationToken()`. Add `SetCurrentChatMode()` / `GetCurrentChatMode()`. Keep `StageConfirm()` but rename to `StageModeGate()` and change return semantics. |
| 7 | `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp` | Rewrite `StageConfirm()` -> `StageModeGate()`. Remove all tier routing logic, preview generation, confirmation token logic. New implementation: Ask mode blocks all writes, Plan mode blocks writes except `blueprint.preview_plan_json`, Code mode checks two hardcoded destructive ops (delete, destructive reparent). Remove `ExecuteConfirmed()`, `GeneratePreview()`, `GetOperationTier()`, `TierToRequirement()`, `GeneratePlanDescription()`, `BuildPreviewPayload()`, `BuildImpactAnalysis()`, `BuildStructuredChanges()`, `GenerateConfirmationToken()`. |
| 8 | `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h` | Remove `SetFocusProfile()`, `GetFocusProfile()`, `SetDeferredFocusProfile()`, `GetDeferredFocusProfile()`, `ConfirmPendingOperation()`, `DenyPendingOperation()`, `IsWaitingForConfirmation()`, `ActivateNextPendingConfirmation()`, `OnConfirmationRequired` delegate, `OnDeferredProfileApplied` delegate. Remove all confirmation state fields. Remove `bTurnHasDangerIntent`. Add `SetChatMode()`, `GetChatMode()`, `FPreviousModeState`, `EOliveChatMode ActiveChatMode`, `FPreviousModeState SavedModeState`. Remove `ActiveFocusProfile`, `DeferredFocusProfile`. Add `DeferredChatMode` (TOptional). Change `GetAvailableTools()` to mode-based filtering. |
| 9 | `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` | Remove all `FocusProfile` references, confirmation flow code, `SetFocusProfile()`, `ConfirmPendingOperation()`, `DenyPendingOperation()`, `ActivateNextPendingConfirmation()`. Implement `SetChatMode()` with deferred switching during processing. Implement mode-based `GetAvailableTools()`. Update `HandleComplete()`/`HandleError()` to restore saved mode state and apply deferred mode. Pass mode to `FOliveWritePipeline::Get().SetCurrentChatMode()` at the start of `SendToProvider()` and `SendUserMessageAutonomous()`. Remove `#include "Profiles/OliveFocusProfileManager.h"`. |
| 10 | `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h` | Change `AssembleSystemPrompt()` signature from `FocusProfileName` to `EOliveChatMode`. Same for `AssembleSystemPromptWithBase()`. Remove `GetProfilePromptAddition()`, `GetAllowedWorkerDomains()`. Remove `ProfileCapabilityPackIds` map. Remove `ProfilePrompts` map. Change `GetCapabilityKnowledge()` to take no profile argument (always returns all packs). Change `BuildSharedSystemPreamble()` to take `EOliveChatMode` instead of profile name. |
| 11 | `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp` | Remove profile-to-pack mapping. `GetCapabilityKnowledge()` always returns all packs (blueprint_authoring, recipe_routing, node_routing, blueprint_design_patterns, events_vs_functions). Remove `GetProfilePromptAddition()`. Add `GetModeSuffix(EOliveChatMode)` that returns the mode-specific suffix text. `AssembleSystemPromptInternal()` appends mode suffix at the end instead of profile prompt addition. Remove `ProfilePrompts` loading. Remove `ProfileCapabilityPackIds` initialization. |
| 12 | `Source/OliveAIEditor/Public/UI/SOliveAIChatPanel.h` | Remove `BuildFocusDropdown()`, `BuildSafetyPresetToggle()`, `BuildFocusProfileMenuContent()`, `OnFocusProfileSelected()`, `OnSafetyPresetChanged()`, `GetSafetyPresetColor()`, `HandleConfirmationRequired()`, `HandleDeferredProfileApplied()`. Remove `FocusProfiles`, `CurrentFocusProfile`, `SafetyPresetOptions`, `CurrentSafetyPreset`, `FocusDropdown`, `DeferredProfileWarning`. Add `BuildModeBadge()`, `OnModeBadgeClicked()`, `CycleMode()`, `HandleModeChanged(EOliveChatMode)`. Add `CurrentChatMode` field. |
| 13 | `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp` | Remove focus dropdown construction, safety preset toggle, confirmation required handler. Add mode badge widget (compact button left of input field). Add slash command parsing in `OnMessageSubmitted()`: intercept `/code`, `/plan`, `/ask`, `/mode`. Add `Ctrl+Shift+M` keyboard shortcut handling. Add system message insertion on mode change. Wire mode changes to ConversationManager. Update `Construct()` to subscribe to mode change events instead of confirmation/profile events. |
| 14 | `Source/OliveAIEditor/Public/UI/SOliveAIInputField.h` | Add `OnSlashCommand` delegate for pre-send interception of `/` commands. |
| 15 | `Source/OliveAIEditor/Private/UI/SOliveAIInputField.cpp` | In `SubmitMessage()`, check if text starts with `/`. If so, fire `OnSlashCommand` instead of `OnMessageSubmit`. If the slash command is unrecognized, let it through as a normal message. |
| 16 | `Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h` | Remove `GetToolsForProfile()`. Add `GetToolsForMode(EOliveChatMode)`. Keep `GetToolsListMCP()` but change `ProfileFilter` param to mode-based (or remove the parameter since MCP always returns all tools). |
| 17 | `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp` | Replace `GetToolsForProfile()` body with `GetToolsForMode()`. For `Code`/`Plan`: return all tools. For `Ask`: return only tools tagged `"read"` or `"discovery"` or `"search"` or `"info"` (i.e., exclude `"write"` and `"danger"` tags). Remove `bEnforcePlanFirstGraphRouting` routing stats in `ExecuteTool()`. Remove `ClearBlueprintRoutingStats()`. |
| 18 | `Source/OliveAIEditor/Public/MCP/OliveMCPServer.h` | No changes needed. MCP path continues to set `bFromMCP = true` which bypasses Stage 2 entirely. External agents are always Code mode (per design spec). |
| 19 | `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp` | Remove `FOliveFocusProfileManager::Get().Initialize()` call. Remove `FOliveToolPackManager::Get().Initialize()` comment. Remove `#include "Profiles/OliveFocusProfileManager.h"` and the ToolPackManager comment. |
| 20 | `Source/OliveAIEditor/Public/OliveAIEditorModule.h` | Remove mention of `FOliveFocusProfileManager::Get()` from the doc comment. |
| 21 | `Source/OliveAIEditor/Public/Brain/OliveOperationHistory.h` | Check for `AwaitingConfirmation` references and remove if present. |
| 22 | `Source/OliveAIEditor/Private/Tests/Conversation/OliveConversationManagerTests.cpp` | Update tests: remove confirmation flow tests, add mode-switching tests. |
| 23 | `Source/OliveAIEditor/Private/Tests/Brain/OliveBrainLayerTests.cpp` | Update state transition tests for 3-state model. |
| 24 | `Source/OliveAIEditor/Public/Chat/OliveRunManager.h` | If it references `AwaitingConfirmation` or focus profiles, update accordingly. |
| 25 | `Source/OliveAIEditor/Private/Brain/OliveToolPackManager.cpp` | References `GetToolsForProfile()` -- will fail to compile after removal. File is being deleted so this is moot. |
| 26 | `Source/OliveAIEditor/Blueprint/Private/Catalog/OliveNodeCatalog.cpp` | Grep shows `FocusProfileManager` reference. Check and remove if it's just an include. |
| 27 | `.claude/agents/*.md` (AGENTS.md / cli_blueprint.txt etc.) | Update mode references in autonomous sandbox prompts. Remove "search templates first" directives. Add mode-aware context. |

### 2.2 Files to DELETE

| # | File | Reason |
|---|------|--------|
| 1 | `Source/OliveAIEditor/Public/Profiles/OliveFocusProfileManager.h` | Entire focus profile system removed |
| 2 | `Source/OliveAIEditor/Private/Profiles/OliveFocusProfileManager.cpp` | Implementation removed |
| 3 | `Source/OliveAIEditor/Public/Brain/OliveToolPackManager.h` | Already deprecated, now deleted |
| 4 | `Source/OliveAIEditor/Private/Brain/OliveToolPackManager.cpp` | Implementation removed |
| 5 | `Source/OliveAIEditor/Private/Tests/FocusProfiles/OliveFocusProfileTests.cpp` | Tests for removed system |
| 6 | `Config/OliveToolPacks.json` | Config for removed ToolPackManager |
| 7 | `Content/SystemPrompts/ProfileBlueprint.txt` | Profile-specific prompt (replaced by mode suffix) |
| 8 | `Content/SystemPrompts/ProfileAIBehavior.txt.bak` | Stale backup |
| 9 | `Content/SystemPrompts/ProfileCppBlueprint.txt.bak` | Stale backup |
| 10 | `Content/SystemPrompts/ProfileLevelPCG.txt.bak` | Stale backup |

### 2.3 Files to CREATE

None. All changes go into existing files or replace deleted files' functionality inline.

---

## 3. Removal Manifest

### 3.1 Enums to REMOVE

| Enum | File | Replacement |
|------|------|-------------|
| `EOliveConfirmationTier` | `OliveAISettings.h` | Mode gate in pipeline |
| `EOliveSafetyPreset` | `OliveAISettings.h` | `DefaultChatMode` setting |
| `EOliveConfirmationRequirement` | `OliveWritePipeline.h` | Mode gate returns `FOliveWriteResult` directly |
| `EOliveToolPack` | `OliveToolPackManager.h` | Nothing (file deleted) |

### 3.2 Structs to REMOVE

| Struct | File |
|--------|------|
| `FOliveFocusProfile` | `OliveFocusProfileManager.h` |
| `FPendingConfirmationRequest` | `OliveConversationManager.h` |

### 3.3 Classes to REMOVE (entire singleton lifecycle)

| Class | Files |
|-------|-------|
| `FOliveFocusProfileManager` | `.h` + `.cpp` in Profiles/ |
| `FOliveToolPackManager` | `.h` + `.cpp` in Brain/ |

### 3.4 Methods to REMOVE

**From `UOliveAISettings`:**
- `GetEffectiveTier()`
- `SetSafetyPreset()`
- Static delegate `OnSafetyPresetChanged`

**From `FOliveWritePipeline`:**
- `ExecuteConfirmed()`
- `GeneratePreview()`
- `GetOperationTier()`
- `TierToRequirement()`
- `GeneratePlanDescription()`
- `BuildPreviewPayload()`
- `BuildImpactAnalysis()`
- `BuildStructuredChanges()`
- `GenerateConfirmationToken()`

**From `FOliveConversationManager`:**
- `SetFocusProfile()`
- `GetFocusProfile()`
- `SetDeferredFocusProfile()`
- `GetDeferredFocusProfile()`
- `ConfirmPendingOperation()`
- `DenyPendingOperation()`
- `IsWaitingForConfirmation()`
- `ActivateNextPendingConfirmation()`

**From `FOlivePromptAssembler`:**
- `GetProfilePromptAddition()`
- `GetAllowedWorkerDomains()` (if it exists; check at implementation time)

**From `FOliveToolRegistry`:**
- `GetToolsForProfile()`
- `ClearBlueprintRoutingStats()`

**From `SOliveAIChatPanel`:**
- `BuildFocusDropdown()`
- `BuildSafetyPresetToggle()`
- `BuildFocusProfileMenuContent()`
- `OnFocusProfileSelected()`
- `OnSafetyPresetChanged()`
- `GetSafetyPresetColor()`
- `HandleConfirmationRequired()`
- `HandleDeferredProfileApplied()`

### 3.5 UPROPERTYs to REMOVE from `UOliveAISettings`

- `VariableOperationsTier`
- `ComponentOperationsTier`
- `CreateOperationsTier`
- `FunctionCreationTier`
- `GraphEditingTier`
- `RefactoringTier`
- `DeleteOperationsTier`
- `SafetyPreset`
- `DefaultFocusProfile`
- `CustomFocusProfilesJson`
- `CustomFocusProfilesSchemaVersion`
- `bEnforcePlanFirstGraphRouting`
- `PlanFirstGraphRoutingThreshold`

### 3.6 Delegates to REMOVE

- `FOnOliveChatConfirmationRequired` (OliveConversationManager.h)
- `FOnOliveChatDeferredProfileApplied` (OliveConversationManager.h)
- `FOnSafetyPresetChanged` (OliveAISettings.h)

### 3.7 Fields to REMOVE from `FOliveConversationManager`

- `ActiveFocusProfile`
- `DeferredFocusProfile`
- `bWaitingForConfirmation`
- `PendingConfirmationToolCallId`
- `PendingConfirmationToolName`
- `PendingConfirmationArguments`
- `PendingConfirmationToken`
- `PendingConfirmationQueue`
- `bTurnHasDangerIntent`

### 3.8 Fields to REMOVE from `FOliveWritePipeline`

- `PendingConfirmations`
- `ConfirmationLock`

### 3.9 Fields to REMOVE from `FOliveWriteResult`

- `ConfirmationRequired`
- `PlanDescription`
- `PreviewData`

### 3.10 Fields to REMOVE from `SOliveAIChatPanel`

- `FocusProfiles`
- `CurrentFocusProfile`
- `SafetyPresetOptions`
- `CurrentSafetyPreset`
- `FocusDropdown`
- `DeferredProfileWarning`

---

## 4. New Stage 2 (Mode Gate) Implementation

### 4.1 Overview

`StageConfirm()` is renamed to `StageModeGate()`. It receives the write request and the current chat mode. Logic:

```
StageModeGate(Request, Mode):
    // MCP requests always bypass (unchanged behavior)
    if Request.bFromMCP:
        return empty (pass-through)

    // Ask mode: block everything
    if Mode == Ask:
        return BlockResult("ASK_MODE", "Read-only mode. Switch to Code or Plan to make changes.")

    // Plan mode: block writes except preview
    if Mode == Plan:
        if Request.ToolName == "blueprint.preview_plan_json":
            return empty (pass-through)
        return BlockResult("PLAN_MODE", "Write blocked in Plan mode. Present your plan, then say 'go' to execute.", "Describe what you would do using read tools, then the user will approve.")

    // Code mode: two hardcoded destructive prompts
    if Request.ToolName == "blueprint.delete":
        // TODO (future): for now, auto-execute like all other ops.
        // The design spec calls for a one-time prompt, but implementing the
        // prompt UX (Allow/Deny buttons mid-stream) is a Phase 2 UI task.
        // Snapshots are the safety net.
        return empty (pass-through)

    if Request.ToolName == "blueprint.set_parent_class":
        // Check IsDestructiveReparent -- if parent changes to incompatible type
        // Same as above: auto-execute for Phase 1, prompt in Phase 2
        return empty (pass-through)

    return empty (pass-through)
```

### 4.2 Mode Storage on Pipeline

The pipeline is a singleton, so the mode must be set per-request-scope. Two options:

**Option A (chosen):** Add `EOliveChatMode CurrentChatMode` field to `FOliveWriteRequest`. This is the cleanest -- the mode travels with the request and there is no global mutable state issue.

Add to `FOliveWriteRequest`:
```cpp
/** Current chat mode -- controls Stage 2 behavior. Set by ConversationManager before pipeline entry. */
EOliveChatMode ChatMode = EOliveChatMode::Code;
```

The pipeline reads `Request.ChatMode` in `StageModeGate()`. Tool handlers already build `FOliveWriteRequest` and pass it to the pipeline, so the ConversationManager sets the mode on the request before dispatching.

**Where the mode gets set:**
- In `ExecuteToolCall()` on `OliveConversationManager.cpp`, the tool handler builds `FOliveWriteRequest`. We need the mode to be propagated. The simplest approach: the `FOliveToolExecutionContext` (already threaded through tool calls) carries the mode, and tool handlers copy it to `FOliveWriteRequest.ChatMode`.
- Alternatively, since `FOliveWriteRequest.bFromMCP` is already set by tool handlers, we add `ChatMode` alongside it. Tool handlers in the orchestrated path set `ChatMode = ConversationManager.GetChatMode()`. MCP tool handlers set `bFromMCP = true` which makes `ChatMode` irrelevant.

**Practical injection point:** The `FOliveToolExecutionContext` struct (in `Brain/OliveToolExecutionContext.h`) already carries `ActiveFocusProfile`. Replace that with `ChatMode`:

```cpp
// Replace:  FName ActiveFocusProfile;
// With:
EOliveChatMode ChatMode = EOliveChatMode::Code;
```

Then in `OliveConversationManager.cpp` where `ToolContext.ActiveFocusProfile` is set (~line 1193, 1492), set `ToolContext.ChatMode` instead. Tool handlers that build `FOliveWriteRequest` copy `Context.ChatMode` to `Request.ChatMode`.

### 4.3 Mode Gate Return for Plan/Ask

The pipeline's `StageModeGate()` returns a `TOptional<FOliveWriteResult>`. When the mode blocks, it returns a result with `bSuccess = false` and a structured error. The existing pattern in `StageConfirm()` already returns this way.

The error code and message are important -- the AI sees them and adjusts behavior:

- **ASK_MODE:** `FOliveWriteResult::ExecutionError("ASK_MODE", "Write tools are not available in Ask mode. Switch to Code or Plan mode to make changes.")`
- **PLAN_MODE:** `FOliveWriteResult::ExecutionError("PLAN_MODE", "Write tools are blocked in Plan mode. Present your plan and the user will approve execution.", "Describe what you would do, then wait for the user to switch to Code mode or say 'go'.")`

These flow through `.ToToolResult()` back to the AI as structured tool results.

### 4.4 bFromMCP Interaction

The existing `if (!Request.bFromMCP)` guard at line 189 of OliveWritePipeline.cpp already skips Stage 2 for MCP requests. This stays unchanged. `StageModeGate()` is only called for non-MCP requests, maintaining backward compatibility with external agents.

---

## 5. Brain Layer Simplification

### 5.1 State Machine

```
Idle --> Active --> Idle
  |                  ^
  +--> Cancelling ---+
```

**Transition table:**

| From | To | Condition |
|------|----|-----------|
| Idle | Active | `BeginRun()` called |
| Active | Idle | `CompleteRun()` called (any outcome) |
| Active | Cancelling | `RequestCancel()` called |
| Cancelling | Idle | Cancel complete |

**Removed transitions:**
- `Idle -> Planning` (merged into Active)
- `WorkerActive -> AwaitingConfirmation` (confirmation removed)
- `AwaitingConfirmation -> WorkerActive` (confirmation removed)
- `WorkerActive -> Completed` (Completed merged into Idle)
- `WorkerActive -> Error` (Error merged into Idle)
- `Completed -> Idle` (no terminal states)
- `Error -> Idle` (no terminal states)

### 5.2 IsValidTransition() Rewrite

```cpp
bool FOliveBrainLayer::IsValidTransition(EOliveBrainState From, EOliveBrainState To) const
{
    switch (From)
    {
    case EOliveBrainState::Idle:
        return To == EOliveBrainState::Active;

    case EOliveBrainState::Active:
        return To == EOliveBrainState::Idle
            || To == EOliveBrainState::Cancelling;

    case EOliveBrainState::Cancelling:
        return To == EOliveBrainState::Idle;

    default:
        return false;
    }
}
```

### 5.3 BeginRun() / CompleteRun()

`BeginRun()`: transitions `Idle -> Active`.
`CompleteRun()`: stores outcome, transitions `Active -> Idle`. Both success and failure go to Idle.

`SetWorkerPhase()` checks for `Active` (was `WorkerActive`).

### 5.4 IsActive()

```cpp
bool FOliveBrainLayer::IsActive() const
{
    return CurrentState != EOliveBrainState::Idle;
}
```

### 5.5 Worker Phases Stay

`EOliveWorkerPhase` is unchanged (Streaming, ExecutingTools, Compiling, SelfCorrecting, Complete). These are metadata on the Active state, used for UI status display.

---

## 6. Settings Migration

### 6.1 Removed UPROPERTYs

When the 13 UPROPERTYs listed in section 3.5 are removed from the UCLASS, their values become orphaned in `DefaultEditor.ini`. UE handles this gracefully -- unknown keys are preserved but ignored on load. No manual migration of the .ini file is needed.

### 6.2 DefaultChatMode Mapping

Add to `UOliveAISettings::PostInitProperties()` (or `PostEditChangeProperty` if needed):

```cpp
void UOliveAISettings::PostInitProperties()
{
    Super::PostInitProperties();

    // One-time migration: map old SafetyPreset to DefaultChatMode
    // SafetyPreset was stored as an int in config. We detect it by checking
    // if the config section has the old key but our new key is still at default.
    //
    // Note: After the migration, SafetyPreset is no longer a UPROPERTY so UE
    // won't load it. We do a raw config read to detect it.
    if (!bChatModeMigrated)
    {
        FString OldPresetStr;
        if (GConfig && GConfig->GetString(TEXT("/Script/OliveAIEditor.OliveAISettings"),
            TEXT("SafetyPreset"), OldPresetStr, GEditorPerProjectIni))
        {
            if (OldPresetStr == TEXT("YOLO") || OldPresetStr == TEXT("2"))
            {
                DefaultChatMode = EOliveChatModeConfig::Code;
            }
            else if (OldPresetStr == TEXT("Careful") || OldPresetStr == TEXT("0"))
            {
                DefaultChatMode = EOliveChatModeConfig::Plan;
            }
            else // Fast or unknown
            {
                DefaultChatMode = EOliveChatModeConfig::Code;
            }
            UE_LOG(LogOliveAI, Log, TEXT("Migrated SafetyPreset '%s' -> DefaultChatMode '%s'"),
                *OldPresetStr, LexToString(ChatModeFromConfig(DefaultChatMode)));
            bChatModeMigrated = true;
            SaveConfig();
        }
    }
}
```

Add a transient `bChatModeMigrated` bool (not UPROPERTY, or a hidden UPROPERTY) to gate migration to run-once. Alternatively, since `SafetyPreset` is no longer a UPROPERTY and won't be loaded, we can just check if `DefaultChatMode` has ever been explicitly saved by looking for its key in config. If missing, do the migration.

**Simpler approach (recommended):** Skip the migration. The default is `Code` which matches the autonomous-first philosophy. Users who had `Careful` can switch to Plan mode manually. The migration code adds complexity for a one-time transition. Log a message at startup if old keys are detected pointing users to the new setting.

### 6.3 New Setting

```cpp
UPROPERTY(Config, EditAnywhere, Category="Chat",
    meta=(DisplayName="Default Chat Mode",
          ToolTip="Default mode when opening the chat. Code: full autonomy. Plan: review before executing. Ask: read-only."))
EOliveChatModeConfig DefaultChatMode = EOliveChatModeConfig::Code;
```

---

## 7. ConversationManager Changes

### 7.1 Mode Storage

Replace `ActiveFocusProfile` (FString) with `ActiveChatMode` (EOliveChatMode). Initialize from settings on `StartNewSession()`.

```cpp
EOliveChatMode ActiveChatMode = EOliveChatMode::Code;
TOptional<EOliveChatMode> DeferredChatMode;  // Applied when processing completes
```

### 7.2 SetChatMode()

```cpp
void FOliveConversationManager::SetChatMode(EOliveChatMode NewMode)
{
    if (bIsProcessing)
    {
        DeferredChatMode = NewMode;
        UE_LOG(LogOliveAI, Log, TEXT("Mode switch to %s deferred until processing completes"),
            LexToString(NewMode));
        // Fire delegate so UI can show "Mode will switch after current operation"
        OnModeSwitchDeferred.Broadcast(NewMode);
        return;
    }

    const EOliveChatMode OldMode = ActiveChatMode;
    ActiveChatMode = NewMode;
    DeferredChatMode.Reset();
    UE_LOG(LogOliveAI, Log, TEXT("Chat mode: %s -> %s"), LexToString(OldMode), LexToString(NewMode));
    OnModeChanged.Broadcast(NewMode);
}
```

### 7.3 Plan Mode Approval -> Temporary Code

When the user sends "go"/"do it"/"execute"/"approved"/"build it" in Plan mode, the ConversationManager detects the approval phrase and temporarily flips to Code mode:

```cpp
// In SendUserMessage(), before dispatching:
if (ActiveChatMode == EOliveChatMode::Plan && IsApprovalPhrase(Message))
{
    SavedModeState.Set(EOliveChatMode::Plan);
    ActiveChatMode = EOliveChatMode::Code;
    UE_LOG(LogOliveAI, Log, TEXT("Plan approved -- temporary Code mode for execution"));
    OnModeChanged.Broadcast(EOliveChatMode::Code);
    // The mode reverts in HandleComplete/HandleError (see below)
}
```

### 7.3 Deferred Mode Application on Completion

Plan mode stays in Plan mode. There is no approval-phrase detection and no `SavedModeState` / `FPreviousModeState`. The user switches to Code mode explicitly via `/code` when ready to execute.

The only mode transition that happens automatically is applying a `DeferredChatMode` on completion — this handles the case where the user typed `/plan` while a Code run was in progress. In `HandleComplete()`, `HandleError()`, and `CancelCurrentRequest()`:

```cpp
// Apply deferred mode switch if pending
if (DeferredChatMode.IsSet())
{
    ActiveChatMode = DeferredChatMode.GetValue();
    DeferredChatMode.Reset();
    OnModeChanged.Broadcast(ActiveChatMode);
    UE_LOG(LogOliveAI, Log, TEXT("Applied deferred mode switch to %s"), LexToString(ActiveChatMode));
}
```

### 7.5 GetAvailableTools() Mode-Based Filtering

```cpp
TArray<FOliveToolDefinition> FOliveConversationManager::GetAvailableTools()
{
    switch (ActiveChatMode)
    {
    case EOliveChatMode::Ask:
        return FOliveToolRegistry::Get().GetToolsForMode(EOliveChatMode::Ask);

    case EOliveChatMode::Plan:
    case EOliveChatMode::Code:
    default:
        // Plan and Code both return all tools.
        // Plan mode blocks writes at the pipeline level (Stage 2), not at the tool list level.
        // This lets the AI see write tool schemas for planning.
        return FOliveToolRegistry::Get().GetAllTools();
    }
}
```

### 7.6 New Delegates

```cpp
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveChatModeChanged, EOliveChatMode /* NewMode */);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveChatModeSwitchDeferred, EOliveChatMode /* PendingMode */);
```

These replace `OnConfirmationRequired` and `OnDeferredProfileApplied`.

### 7.7 Intent Flags

`bTurnHasExplicitWriteIntent` stays -- it is used for the zero-tool re-prompt guard and multi-asset iteration budget, not for tool pack gating.

`bTurnHasDangerIntent` is removed -- it was only used for tool pack gating (already deprecated).

---

## 8. PromptAssembler Changes

### 8.1 Signature Changes

```cpp
// Old:
FString AssembleSystemPrompt(const FString& FocusProfileName, const TArray<FString>& ContextAssetPaths, int32 MaxTokens = 4000);

// New:
FString AssembleSystemPrompt(EOliveChatMode Mode, const TArray<FString>& ContextAssetPaths, int32 MaxTokens = 4000);
```

Same for `AssembleSystemPromptWithBase()` and `BuildSharedSystemPreamble()`.

### 8.2 Mode Suffix

New private method:

```cpp
FString FOlivePromptAssembler::GetModeSuffix(EOliveChatMode Mode) const
{
    switch (Mode)
    {
    case EOliveChatMode::Code:
        return TEXT("You are in Code mode. Execute the user's request fully -- research, plan, build, "
            "compile, and verify. Use whatever tools and approach you judge best. Take a snapshot "
            "before destructive changes. Do not ask for permission on standard operations.");

    case EOliveChatMode::Plan:
        return TEXT("You are in Plan mode. Research the codebase and present a structured plan. "
            "Use read tools freely to understand the current state. Do not execute write operations. "
            "Present your plan as: 1) what assets to create/modify, 2) what each asset needs "
            "(components, variables, functions), 3) how assets communicate. "
            "The user will approve before you build.");

    case EOliveChatMode::Ask:
        return TEXT("You are in Ask mode. Answer questions about the project using read tools. "
            "Explain what you find clearly. Do not make any changes to assets.");

    default:
        return FString();
    }
}
```

### 8.3 Knowledge Packs (All Modes)

Remove the `ProfileCapabilityPackIds` map. `GetCapabilityKnowledge()` always returns all packs:

```cpp
FString FOlivePromptAssembler::GetCapabilityKnowledge() const
{
    FString Result;
    for (const auto& Pair : CapabilityKnowledgePacks)
    {
        Result += Pair.Value;
        Result += TEXT("\n\n");
    }
    return Result;
}
```

### 8.4 Template Prompt Change

In the base prompt or knowledge packs, find and replace any "search templates first" directive with:

> "Templates are available for common patterns -- use `list_templates` to search if you want reference material."

This is a neutral, informational mention. The AI uses templates when it judges they fit.

---

## 9. UI Changes

### 9.1 Mode Badge Widget

A compact button placed to the left of the input text field in `BuildInputArea()`. Replaces the focus dropdown and safety preset toggle.

**Visual spec:**
```
[CODE] | Type your message...          [Send]
```

- Background color: Code=green (#4CAF50 at 30% opacity), Plan=amber (#FF9800 at 30% opacity), Ask=blue (#2196F3 at 30% opacity)
- Text: uppercase mode name, bold, 10pt
- Click: cycles Code -> Plan -> Ask -> Code
- Tooltip: "Click to cycle mode, or type /code /plan /ask"

**Construction:**

```cpp
TSharedRef<SWidget> SOliveAIChatPanel::BuildModeBadge()
{
    return SNew(SButton)
        .ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("FlatButton"))
        .OnClicked(this, &SOliveAIChatPanel::OnModeBadgeClicked)
        .ToolTipText(LOCTEXT("ModeBadgeTooltip", "Click to cycle mode (Code/Plan/Ask), or type /code /plan /ask"))
        .ContentPadding(FMargin(8, 2))
        [
            SNew(STextBlock)
            .Text(this, &SOliveAIChatPanel::GetModeBadgeText)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
            .ColorAndOpacity(this, &SOliveAIChatPanel::GetModeBadgeColor)
        ];
}
```

### 9.2 Slash Command Parsing

In `SOliveAIInputField::SubmitMessage()`, before firing `OnMessageSubmit`:

```cpp
FString Text = GetText().TrimStartAndEnd();
if (Text.StartsWith(TEXT("/")))
{
    FString Command = Text.ToLower();
    if (Command == TEXT("/code") || Command == TEXT("/plan") || Command == TEXT("/ask") || Command == TEXT("/mode") || Command == TEXT("/status"))
    {
        OnSlashCommand.ExecuteIfBound(Text);
        Clear();
        return;
    }
    // Unrecognized slash commands pass through as normal messages
}
OnMessageSubmit.ExecuteIfBound(Text);
Clear();
```

In `SOliveAIChatPanel`, handle the slash command delegate:

```cpp
void SOliveAIChatPanel::HandleSlashCommand(const FString& Command)
{
    FString Cmd = Command.ToLower().TrimStartAndEnd();

    if (Cmd == TEXT("/code"))
    {
        ConversationManager->SetChatMode(EOliveChatMode::Code);
    }
    else if (Cmd == TEXT("/plan"))
    {
        ConversationManager->SetChatMode(EOliveChatMode::Plan);
    }
    else if (Cmd == TEXT("/ask"))
    {
        ConversationManager->SetChatMode(EOliveChatMode::Ask);
    }
    else if (Cmd == TEXT("/mode"))
    {
        // Insert system message showing current mode
        AddSystemMessage(FString::Printf(TEXT("Current mode: %s"), LexToString(ConversationManager->GetChatMode())));
    }
    else if (Cmd == TEXT("/status"))
    {
        // Show mode + brain state + last run outcome + active tool count
        FString Status = FString::Printf(TEXT("Mode: %s | Brain: %s | Last run: %s | Tools: %d"),
            LexToString(ConversationManager->GetChatMode()),
            LexToString(BrainLayer->GetState()),
            LexToString(BrainLayer->GetLastOutcome()),
            ConversationManager->GetAvailableToolCount());
        AddSystemMessage(Status);
    }
}
```

### 9.3 Keyboard Shortcut

`Ctrl+Shift+M` cycles modes. Handle in `SOliveAIChatPanel` via `FUICommandInfo` registration or inline key event handling.

### 9.4 System Messages on Mode Change

When the mode changes, insert a system message:

```
Switched to Plan mode. Read tools active. Write tools blocked until you approve.
Switched to Code mode. All tools active. Executing autonomously.
Switched to Ask mode. Read-only. No changes will be made.
```

When a mode switch is deferred:

```
Mode will switch to Plan after the current operation finishes.
```

### 9.5 Removed UI Elements

- Focus profile dropdown and its menu content builder
- Safety preset toggle (3-state Careful/Fast/YOLO button)
- Confirmation dialog (the `HandleConfirmationRequired` handler that shows Allow/Deny)
- The "Deferred profile warning" display

---

## 10. Integration with Autonomous/MCP Path

### 10.1 MCP Server (No Changes Needed)

External MCP agents (Claude Code CLI, Cursor, etc.) always operate in Code mode. The MCP tool dispatch path sets `bFromMCP = true` on every request, which causes `StageModeGate()` to be skipped entirely (the `if (!Request.bFromMCP)` guard at line 189 of OliveWritePipeline.cpp). This is the existing behavior and remains unchanged.

The design spec explicitly states: "External MCP agents are always in Code mode. They manage their own permission model." Therefore, no mode field is needed on the MCP server.

### 10.2 Autonomous CLI Path

`SendUserMessageAutonomous()` launches a long-lived CLI process that uses MCP for tool calls. Since MCP calls set `bFromMCP = true`, they bypass Stage 2 regardless of mode. The autonomous path is always effectively Code mode.

The mode suffix IS injected into the autonomous prompt via `BuildSharedSystemPreamble()` which now takes `EOliveChatMode` instead of a profile name. In the autonomous path, this is always called with `EOliveChatMode::Code`.

Ask mode blocks autonomous execution (returns an error — no mutations allowed). Plan and Code mode both allow it. There is no automatic mode flip — if the user is in Plan mode and triggers an autonomous run, the run executes in Code mode (MCP tools bypass the mode gate), and the mode stays Plan from the ConversationManager's perspective. The mode suffix injected into the prompt is `Code` regardless.

### 10.3 Tool Context Propagation

The `FOliveToolExecutionContext` struct (threaded through tool handlers) gets `ChatMode` instead of `ActiveFocusProfile`. In the orchestrated path, this is set from `ConversationManager.GetChatMode()`. In the MCP path, the context already has `bFromMCP = true` which bypasses the mode gate, so the `ChatMode` field on the context is irrelevant but defaults to `Code`.

---

## 11. Ordered Task List for Coder

### Phase 1: Core Infrastructure (2 tasks, foundational)

**Task 1: Brain State Enum and Mode Enum [MUST BE FIRST]**
- File: `Source/OliveAIEditor/Public/Brain/OliveBrainState.h`
- Replace `EOliveBrainState` 7-state enum with 3-state (Idle, Active, Cancelling)
- Add `EOliveChatMode` enum (Code, Plan, Ask)
- Add `LexToString()` for both new enums
- Add `ChatModeFromConfig()` conversion helper
- Update `LexToString(EOliveWorkerPhase)` -- no changes needed, just verify
- Estimated: ~40 lines changed
- Dependencies: None. Every subsequent task depends on this.

**Task 2: Brain Layer 3-State Rewrite**
- Files: `Source/OliveAIEditor/Public/Brain/OliveBrainLayer.h`, `Private/Brain/OliveBrainLayer.cpp`
- Rewrite `IsValidTransition()` for 3 states
- `BeginRun()` transitions `Idle -> Active` (was `Idle -> WorkerActive`)
- `CompleteRun()` always transitions to `Idle` regardless of outcome (was `Active -> Completed` or `Active -> Error`)
- `SetWorkerPhase()` checks `Active` (was `WorkerActive`)
- `IsActive()` returns `CurrentState != Idle`
- `RequestCancel()` transitions `Active -> Cancelling`
- `ResetToIdle()` transitions any state -> Idle
- Estimated: ~50 lines changed
- Dependencies: Task 1

### Phase 2: Settings and Pipeline (3 tasks)

**Task 3: Settings Cleanup [Do before ConversationManager]**
- Files: `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`, `Private/Settings/OliveAISettings.cpp`
- Add `EOliveChatModeConfig` UENUM (UCLASS-compatible mirror of `EOliveChatMode`)
- Add `DefaultChatMode` UPROPERTY
- Remove `EOliveConfirmationTier` enum
- Remove `EOliveSafetyPreset` enum
- Remove all 7 tier UPROPERTYs
- Remove `SafetyPreset` UPROPERTY
- Remove `GetEffectiveTier()`, `SetSafetyPreset()`, `OnSafetyPresetChanged` static delegate
- Remove `DefaultFocusProfile`, `CustomFocusProfilesJson`, `CustomFocusProfilesSchemaVersion`
- Remove `bEnforcePlanFirstGraphRouting`, `PlanFirstGraphRoutingThreshold`
- Add startup log message if old SafetyPreset config key detected (informational, no auto-migration)
- Estimated: ~130 lines removed, ~30 lines added
- Dependencies: Task 1

**Task 4: Write Pipeline Mode Gate**
- Files: `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h`, `Private/Pipeline/OliveWritePipeline.cpp`
- Add `EOliveChatMode ChatMode` to `FOliveWriteRequest`
- Rename `StageConfirm()` to `StageModeGate()`
- Implement mode gate logic (section 4 above)
- Remove `EOliveConfirmationRequirement` enum
- Remove from `FOliveWriteResult`: `ConfirmationRequired`, `PlanDescription`, `PreviewData`, `ConfirmationNeeded()` factory
- Remove `ExecuteConfirmed()`, `GeneratePreview()`
- Remove `GetOperationTier()`, `TierToRequirement()`, `GeneratePlanDescription()`
- Remove `BuildPreviewPayload()`, `BuildImpactAnalysis()`, `BuildStructuredChanges()`
- Remove `PendingConfirmations`, `ConfirmationLock`, `GenerateConfirmationToken()`
- Estimated: ~600 lines removed, ~40 lines added
- Dependencies: Task 1, Task 3 (settings enums removed)

**Task 5: Tool Registry Mode Filtering**
- Files: `Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h`, `Private/MCP/OliveToolRegistry.cpp`
- Replace `GetToolsForProfile()` with `GetToolsForMode(EOliveChatMode)`
- For Code/Plan: return all tools
- For Ask: return tools that do NOT have the `"write"` or `"danger"` or `"refactor"` or `"create"` tag. Keep tools tagged `"read"`, `"discovery"`, `"search"`, `"info"`, `"project"`. Default: if a tool has no write-family tags, include it.
- Remove plan-first routing stats code (`bEnforcePlanFirstGraphRouting` check in `ExecuteTool()`, `ClearBlueprintRoutingStats()`)
- Update `GetToolsListMCP()` to remove the `ProfileFilter` parameter (MCP always returns all tools)
- Estimated: ~80 lines removed, ~30 lines added
- Dependencies: Task 1

### Phase 3: ConversationManager (1 task, most complex)

**Task 6: ConversationManager Mode Integration [Most Complex]**
- Files: `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h`, `Private/Chat/OliveConversationManager.cpp`
- Add `ActiveChatMode`, `DeferredChatMode` fields (no `SavedModeState` — approval flip removed)
- Add `SetChatMode()`, `GetChatMode()` methods
- Add `OnModeChanged`, `OnModeSwitchDeferred` delegates
- Remove all focus profile fields and methods (section 3.7)
- Remove all confirmation fields and methods (section 3.4)
- Remove `OnConfirmationRequired`, `OnDeferredProfileApplied` delegates
- Remove `bTurnHasDangerIntent`
- Remove `#include "Profiles/OliveFocusProfileManager.h"`
- Plan mode stays in Plan mode — no `IsApprovalPhrase()`, no `SavedModeState`, no temporary flip
- Implement deferred mode switch application in `HandleComplete()`, `HandleError()`, and `CancelCurrentRequest()`
- Add `Provider->ResetSession()` call in `StartNewSession()`
- Update `GetAvailableTools()` to use mode-based filtering
- Update `BuildSystemMessage()` to pass mode to PromptAssembler instead of profile
- Set `ChatMode` on `FOliveToolExecutionContext` everywhere `ActiveFocusProfile` was set
- Initialize `ActiveChatMode` from settings in `StartNewSession()`
- Estimated: ~300 lines removed, ~130 lines added
- Dependencies: Tasks 1-5 (settings, pipeline, tool registry)

### Phase 4: PromptAssembler (1 task)

**Task 7: PromptAssembler Mode-Based Assembly**
- Files: `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h`, `Private/Chat/OlivePromptAssembler.cpp`
- Change `AssembleSystemPrompt()` to take `EOliveChatMode` instead of `FocusProfileName`
- Change `AssembleSystemPromptWithBase()` similarly
- Change `BuildSharedSystemPreamble()` similarly
- Remove `GetProfilePromptAddition()`, `ProfilePrompts` map
- Remove `ProfileCapabilityPackIds` map
- `GetCapabilityKnowledge()` takes no argument, returns all packs
- Add `GetModeSuffix(EOliveChatMode)` private method
- In `AssembleSystemPromptInternal()`, replace profile prompt addition with mode suffix
- In `LoadPromptTemplates()`, remove profile prompt file loading
- Estimated: ~100 lines removed, ~50 lines added
- Dependencies: Task 1 (for `EOliveChatMode`), Task 6 (callers use new signature)

### Phase 5: UI Changes (2 tasks)

**Task 8: Input Field Slash Commands**
- Files: `Source/OliveAIEditor/Public/UI/SOliveAIInputField.h`, `Private/UI/SOliveAIInputField.cpp` (or wherever the .cpp lives)
- Add `FOnOliveSlashCommand` delegate
- Add `SLATE_EVENT(FOnOliveSlashCommand, OnSlashCommand)` to SLATE_BEGIN_ARGS
- In `SubmitMessage()`, intercept `/code`, `/plan`, `/ask`, `/mode`, `/status` and fire the delegate instead of `OnMessageSubmit`
- Estimated: ~25 lines added
- Dependencies: None (can be done in parallel with Phase 2-4)

**Task 9: Chat Panel Mode UI**
- Files: `Source/OliveAIEditor/Public/UI/SOliveAIChatPanel.h`, `Private/UI/SOliveAIChatPanel.cpp`
- Remove `BuildFocusDropdown()`, `BuildSafetyPresetToggle()`, `BuildFocusProfileMenuContent()`, and all related handlers
- Remove confirmation handler (`HandleConfirmationRequired()`) and deferred profile handler
- Remove member variables: `FocusProfiles`, `CurrentFocusProfile`, `SafetyPresetOptions`, `CurrentSafetyPreset`, `FocusDropdown`, `DeferredProfileWarning`
- Add `BuildModeBadge()` method
- Add `OnModeBadgeClicked()` -> cycles mode
- Add `GetModeBadgeText()` -> returns "CODE"/"PLAN"/"ASK"
- Add `GetModeBadgeColor()` -> green/amber/blue
- In `BuildInputArea()`, replace focus dropdown + safety preset with mode badge
- Wire `OnSlashCommand` from InputField to `HandleSlashCommand()`
- Wire `ConversationManager->OnModeChanged` to update badge
- Add `Ctrl+Shift+M` shortcut (via `FUICommandList` or inline key handler)
- Insert system messages on mode change
- Estimated: ~250 lines removed, ~150 lines added
- Dependencies: Tasks 6, 8

### Phase 6: File Deletion and Cleanup (2 tasks)

**Task 10: Delete Removed Files**
- Delete files listed in section 2.2
- Remove `#include` references to deleted files from all surviving files
- Remove `FOliveFocusProfileManager::Get().Initialize()` from `OliveAIEditorModule.cpp`
- Remove `FOliveToolPackManager` comment block from `OliveAIEditorModule.cpp`
- Update `OliveAIEditorModule.h` doc comment to remove `FOliveFocusProfileManager::Get()`
- Check `OliveNodeCatalog.cpp` for `FocusProfileManager` include and remove
- Estimated: 10 files deleted, ~20 include lines removed
- Dependencies: Tasks 3-9 (all code references removed first)

**Task 11: Test Updates**
- Delete `Source/OliveAIEditor/Private/Tests/FocusProfiles/OliveFocusProfileTests.cpp`
- Update `Source/OliveAIEditor/Private/Tests/Brain/OliveBrainLayerTests.cpp` for 3-state model
- Update `Source/OliveAIEditor/Private/Tests/Conversation/OliveConversationManagerTests.cpp`: remove confirmation tests, add mode switching tests
- New tests:
  - Mode gate blocks write in Ask mode
  - Mode gate blocks write in Plan mode
  - Mode gate allows `preview_plan_json` in Plan mode
  - Mode gate passes in Code mode
  - Deferred mode switch applies after processing
  - Session reset clears mode state
  - Slash command parsing (/code, /plan, /ask, /mode)
  - Brain state transitions (Idle->Active->Idle, Active->Cancelling->Idle)
- Estimated: ~200 lines removed, ~300 lines added
- Dependencies: All previous tasks

### Phase 7: Documentation and Prompts (1 task)

**Task 12: Prompt and Agent Doc Updates**
- Update `Content/SystemPrompts/Base.txt` or `BaseSystemPrompt.txt` to remove any "search templates first" language
- Replace with neutral template mention: "Templates are available for common patterns -- use `list_templates` to search if you want reference material."
- Update any agent prompts in `.claude/agents/` that reference focus profiles, safety presets, or confirmation tiers
- Update `CLAUDE.md`:
  - Settings section: replace confirmation tiers with DefaultChatMode
  - Focus Profiles section: replace with Chat Modes (Code/Plan/Ask)
  - Brain Layer section: update state machine description
  - Write Pipeline section: update Stage 2 description
- Estimated: ~100 lines changed across multiple files
- Dependencies: All code tasks done

---

## 12. Dependency Graph

```
Task 1 (Brain enums) ──────────────────────────────────────────────┐
    │                                                               │
    ├── Task 2 (Brain layer rewrite)                                │
    │                                                               │
    ├── Task 3 (Settings cleanup) ──────────────┐                  │
    │                                            │                  │
    ├── Task 4 (Pipeline mode gate) ────────────┤                  │
    │                                            │                  │
    ├── Task 5 (Tool registry) ────────────────┤                  │
    │                                            │                  │
    │                                            ├── Task 6 (ConvMgr) ──┐
    │                                            │                       │
    │                                  Task 7 (PromptAssembler) ────────┤
    │                                                                    │
    Task 8 (Input field slash cmds) ────────────────── Task 9 (Chat panel UI)
                                                                    │
                                                       Task 10 (File deletion)
                                                                    │
                                                       Task 11 (Tests)
                                                                    │
                                                       Task 12 (Docs/Prompts)
```

Tasks 1 is the root. Tasks 2, 3, 4, 5, 8 can be done in parallel after Task 1. Task 6 depends on 3, 4, 5. Task 7 depends on 6. Task 9 depends on 6, 8. Tasks 10-12 are cleanup after all code.

---

## 13. Risk Assessment

### Low Risk
- Brain state simplification -- the 3-state model is strictly simpler
- Settings removal -- orphaned config keys are harmless
- Focus profile deletion -- already minimally used (Auto profile was effectively pass-through)
- Tool pack deletion -- already deprecated and commented out

### Medium Risk
- Mode gate in pipeline -- must not accidentally block MCP requests. Mitigation: `bFromMCP` guard is already on line 189 and stays unchanged.
- ConversationManager rework -- the confirmation flow is deeply interleaved with the agentic loop. Careful surgical removal needed. Mitigation: search for all `bWaitingForConfirmation` references and trace each code path.

### Things Explicitly NOT Changed
- `FOliveRunManager` -- run tracking, checkpoints, rollback all stay
- `FOliveSnapshotManager` -- auto-snapshot before runs stays
- `FOliveSelfCorrectionPolicy` -- error classification and retry stays
- `FOliveLoopDetector` -- infinite loop detection stays
- `bAutoCompileAfterWrite` -- compile-after-write stays
- `bPlanJsonRequirePreviewForApply` -- preview-before-apply stays
- All write pipeline stages except Stage 2
- All tool handlers (blueprint, BT, PCG, C++, cross-system, python)
- MCP server and bridge
- Template system

---

## 14. Verification Checklist

After implementation, verify:

1. In Code mode: `blueprint.create` executes without pause
2. In Code mode: `blueprint.apply_plan_json` executes without pause
3. In Plan mode: `blueprint.create` returns `PLAN_MODE` error
4. In Plan mode: `blueprint.preview_plan_json` succeeds
5. In Plan mode: User says "go" -> message sent in Plan mode -> AI can plan but not execute writes
6. In Ask mode: `blueprint.read` succeeds
7. In Ask mode: `blueprint.create` returns `ASK_MODE` error
8. In Ask mode: `blueprint.preview_plan_json` succeeds (it is read-only)
9. Mode badge shows correct color and text
10. `/code`, `/plan`, `/ask`, `/status` slash commands work
11. `Ctrl+Shift+M` cycles modes
12. Mode switch during processing is deferred
13. Deferred mode switch (set during processing) applies after completion or cancel
14. MCP tool calls are unaffected by mode (bFromMCP bypass)
15. Autonomous CLI runs always execute as Code mode
16. No focus profile dropdown in UI
17. No safety preset toggle in UI
18. No confirmation dialog appears
19. Brain state transitions: Idle->Active->Idle works
20. Brain state transitions: Active->Cancelling->Idle works
21. Settings page shows DefaultChatMode instead of old settings
22. Clean compile with zero warnings
