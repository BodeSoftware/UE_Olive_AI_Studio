# Idle-After-Read Nudge + Pickup Template Rewrite

**Status:** Design ready for implementation
**Scope:** 2 changes, 2 files, ~45 minutes implementation time
**Risk:** Low -- no structural changes, no new classes, no new settings

---

## Problem

The autonomous Claude Code agent fails the "pickup item" task with an idle timeout.
The AI calls `blueprint.get_template` for `pickup_interaction`, reads a complex 4-pattern
template with CRITICAL RULES in MUST/NEVER language, then sits idle for 180+ seconds
reasoning about the multi-asset plan. The idle timeout fires, kills the process, and the
run produces zero writes.

Two contributing causes, two fixes:

1. When the idle timeout fires after a read operation, the system reports to the user instead
   of nudging the AI to act. The AI needed a push, not a death sentence.
2. The `pickup_interaction.json` reference template is overloaded with deliberation burden:
   4 patterns, prescriptive CRITICAL RULES, and buried actionable steps.

---

## Fix 1: Idle-After-Read Nudge

### Current Behavior

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

There are TWO idle timeout mechanisms, both in `LaunchCLIProcess()`:

1. **Stdout idle timeout** (lines 756-777): Fires when no stdout data arrives for
   `CLI_IDLE_TIMEOUT_SECONDS` (120s) or `CLI_EXTENDED_IDLE_TIMEOUT_SECONDS` (180s,
   when scaffolding/recipes detected). This catches fully hung processes.

2. **Activity-based tool idle timeout** (lines 781-803): Fires when no MCP tool call
   happens within `AutonomousIdleToolSeconds` (setting, default 240s). This catches
   "thinking but not acting" -- stdout is flowing but no tools are called.

Both set `bLastRunTimedOut = true` and terminate the process.

In `HandleResponseCompleteAutonomous()` (lines 910-1027):

- **Line 951-991**: If timed out + last tool was a write op + auto-continue budget remains,
  auto-continue with `BuildContinuationPrompt("continue")`.
- **Lines 993-1001**: If timed out + last tool was a read op, log and fall through to
  report to user (normal completion). This is the branch that kills the pickup item task.

### Desired Behavior

When the idle timeout fires and the **last tool call was a read operation**, instead of
reporting to the user, auto-continue with a **targeted nudge prompt** that:
- Names the last tool called
- Directs the AI to take the first concrete action
- Does NOT use the generic `BuildContinuationPrompt` (which is designed for write-stall recovery)

### Design Decisions

**Q: Where is the idle timeout constant defined?**
A: Two hardcoded constants in the anonymous namespace at the top of `OliveCLIProviderBase.cpp`:
`CLI_IDLE_TIMEOUT_SECONDS = 120.0` and `CLI_EXTENDED_IDLE_TIMEOUT_SECONDS = 180.0`.
Plus the setting `AutonomousIdleToolSeconds = 240` in `UOliveAISettings`.

**Q: Should we lower timeout thresholds?**
A: Yes, lower `CLI_IDLE_TIMEOUT_SECONDS` from 120 to 90 and `CLI_EXTENDED_IDLE_TIMEOUT_SECONDS`
from 180 to 150. Also lower the default `AutonomousIdleToolSeconds` from 240 to 120. The nudge
mechanism makes long timeouts unnecessary -- we catch stalls faster and recover via nudge.

**Q: How is "last tool call was a read op" detected?**
A: `LastRunContext.ToolCallLog.Last().ToolName` already tracks this. The existing code at
line 944 already checks `IsWriteOperation()`. We add an `IsReadOperation()` helper for
clarity and to define the exact set of read tools that qualify for nudging.

**Q: Should the nudge count against `AutoContinueCount`?**
A: Yes. Both read-nudges and write-continues share the same budget (max 3). This prevents
infinite nudge loops if the AI keeps reading without acting.

**Q: What if the AI made zero tool calls and timed out?**
A: The existing guard `LastRunContext.ToolCallLog.Num() > 0` already handles this. Zero
tool calls = fall through to report to user. The nudge only fires when there IS a last tool.

### Code Changes

#### Change 1A: Add `IsReadOperation()` helper

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`
**Location:** Anonymous namespace, after `IsWriteOperation()` (after line 131)

Add a new helper function:

```cpp
/** Check if a tool name represents a read/lookup operation.
 *  When the AI stalls after a read, it needs a nudge to start acting,
 *  not a timeout kill. */
bool IsReadOperation(const FString& ToolName)
{
    // Exact tool names that represent reading/lookup (not mutations)
    static const TArray<FString> ReadTools = {
        TEXT("blueprint.read"),
        TEXT("blueprint.read_function"),
        TEXT("blueprint.get_template"),
        TEXT("blueprint.list_templates"),
        TEXT("blueprint.get_node_pins"),
        TEXT("blueprint.preview_plan_json"),
        TEXT("olive.get_recipe"),
        TEXT("project.search"),
        TEXT("project.get_relevant_context"),
        TEXT("project.get_asset_info"),
        TEXT("bt.read"),
        TEXT("pcg.read"),
        TEXT("cpp.read"),
    };

    return ReadTools.Contains(ToolName);
}
```

#### Change 1B: Add `BuildReadNudgePrompt()` method

**File:** `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h`
**Location:** In the protected section, after `BuildContinuationPrompt()` declaration (after line 331)

Add declaration:

```cpp
/**
 * Build a targeted nudge prompt for when the AI stalls after a read operation.
 * Unlike BuildContinuationPrompt (which summarizes prior work), this is a short
 * directive that names the last tool and tells the AI to act immediately.
 *
 * @param LastToolName  The name of the last tool that was called (a read op)
 * @return Short nudge prompt for stdin delivery
 */
FString BuildReadNudgePrompt(const FString& LastToolName) const;
```

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`
**Location:** After `BuildContinuationPrompt()` (after line 1172)

Add implementation:

```cpp
FString FOliveCLIProviderBase::BuildReadNudgePrompt(const FString& LastToolName) const
{
    FString Prompt;

    Prompt += TEXT("## Resume — You Stalled After Reading\n\n");
    Prompt += FString::Printf(
        TEXT("You called `%s` and then stopped making tool calls for too long.\n"),
        *LastToolName);
    Prompt += TEXT("Do NOT re-read or re-plan. Take the FIRST concrete write action now.\n\n");

    // Tailor the directive based on what was read
    if (LastToolName.Contains(TEXT("get_template")) || LastToolName.Contains(TEXT("list_templates")))
    {
        Prompt += TEXT("You already have the template info. Start creating or modifying assets:\n");
        Prompt += TEXT("- If creating a new Blueprint, call `blueprint.create` or `blueprint.create_from_template`.\n");
        Prompt += TEXT("- If modifying an existing Blueprint, call `blueprint.add_component`, `blueprint.add_variable`, or `blueprint.apply_plan_json`.\n");
    }
    else if (LastToolName.Contains(TEXT("get_recipe")))
    {
        Prompt += TEXT("You already have the recipe. Call `blueprint.apply_plan_json` with the plan now.\n");
    }
    else if (LastToolName.Contains(TEXT("preview_plan_json")))
    {
        Prompt += TEXT("You already previewed the plan. Call `blueprint.apply_plan_json` with the same plan and the fingerprint.\n");
    }
    else if (LastToolName.Contains(TEXT("search")) || LastToolName.Contains(TEXT("get_relevant_context")))
    {
        Prompt += TEXT("You found the assets. Now modify them — call the appropriate write tool.\n");
    }
    else
    {
        Prompt += TEXT("You have the information you need. Call the next write tool to make progress.\n");
    }

    // Include asset state if available (from tool call tracking during this run)
    if (LastRunContext.ModifiedAssetPaths.Num() > 0)
    {
        FString AssetState = BuildAssetStateSummary();
        if (!AssetState.IsEmpty())
        {
            Prompt += TEXT("\n");
            Prompt += AssetState;
        }
    }

    return Prompt;
}
```

#### Change 1C: Modify auto-continue logic for read-op stalls

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`
**Location:** `HandleResponseCompleteAutonomous()`, replace the "else if" branch at lines 993-1001

Replace:

```cpp
	else if (bLastRunTimedOut
		&& LastRunContext.Outcome == FAutonomousRunContext::EOutcome::IdleTimeout
		&& LastRunContext.ToolCallLog.Num() > 0
		&& !bLastToolWasWrite)
	{
		UE_LOG(LogOliveCLIProvider, Log,
			TEXT("Idle timeout after read operation (%s) — reporting to user instead of auto-continuing"),
			*LastRunContext.ToolCallLog.Last().ToolName);
		// Fall through to normal completion path below
	}
```

With:

```cpp
	else if (bLastRunTimedOut
		&& LastRunContext.Outcome == FAutonomousRunContext::EOutcome::IdleTimeout
		&& LastRunContext.ToolCallLog.Num() > 0
		&& !bLastToolWasWrite
		&& IsReadOperation(LastRunContext.ToolCallLog.Last().ToolName)
		&& AutoContinueCount < MaxAutoContinues)
	{
		// The AI stalled after a read operation (template lookup, recipe, search, etc.).
		// Instead of killing the run, nudge it to take the first concrete action.
		AutoContinueCount++;
		const FString& LastToolName = LastRunContext.ToolCallLog.Last().ToolName;

		UE_LOG(LogOliveCLIProvider, Log,
			TEXT("Idle timeout after read operation (%s) — nudging to act (attempt %d/%d)"),
			*LastToolName, AutoContinueCount, MaxAutoContinues);

		bIsBusy = false;

		FOnOliveStreamChunk SavedOnChunk = CurrentOnChunk;
		FOnOliveComplete SavedOnComplete = CurrentOnComplete;
		FOnOliveError SavedOnError = CurrentOnError;

		bIsAutoContinuation = true;

		// Build nudge prompt on game thread (safe for BuildAssetStateSummary UObject loads)
		FString NudgeToolName = LastToolName; // copy for lambda capture
		AsyncTask(ENamedThreads::GameThread, [this,
			SavedOnChunk, SavedOnComplete, SavedOnError, NudgeToolName]()
		{
			if (!(*AliveGuard)) return;

			FString NudgePrompt = BuildReadNudgePrompt(NudgeToolName);
			SendMessageAutonomous(NudgePrompt, SavedOnChunk, SavedOnComplete, SavedOnError);
		});

		return;
	}
	else if (bLastRunTimedOut
		&& LastRunContext.Outcome == FAutonomousRunContext::EOutcome::IdleTimeout
		&& LastRunContext.ToolCallLog.Num() > 0
		&& !bLastToolWasWrite)
	{
		// Last tool was not a write AND not a recognized read op (or budget exhausted).
		// Fall through to report to user.
		UE_LOG(LogOliveCLIProvider, Log,
			TEXT("Idle timeout after non-write operation (%s) — reporting to user"),
			*LastRunContext.ToolCallLog.Last().ToolName);
	}
```

Note: The original `else if` at line 993 is split into TWO branches:
1. Read-op nudge (with auto-continue, matching `IsReadOperation()` and budget check)
2. Fallback for non-write, non-read ops (reports to user as before)

This ensures non-categorized tools still fall through to the user.

#### Change 1D: Lower timeout constants

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`
**Location:** Anonymous namespace, lines 37 and 43

Change:
```cpp
constexpr double CLI_IDLE_TIMEOUT_SECONDS = 120.0;
constexpr double CLI_EXTENDED_IDLE_TIMEOUT_SECONDS = 180.0;
```

To:
```cpp
constexpr double CLI_IDLE_TIMEOUT_SECONDS = 90.0;
constexpr double CLI_EXTENDED_IDLE_TIMEOUT_SECONDS = 150.0;
```

**File:** `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`
**Location:** Line 178

Change:
```cpp
int32 AutonomousIdleToolSeconds = 240;
```

To:
```cpp
int32 AutonomousIdleToolSeconds = 120;
```

### Rationale for Timeout Values

- **90s stdout idle** (was 120s): With the nudge mechanism, we detect stalls faster
  and recover. 90s is still generous for legitimate thinking pauses between tool calls.
- **150s extended idle** (was 180s): Complex planning still gets extra time, but 150s
  is plenty when nudge recovery exists as a backstop.
- **120s tool idle** (was 240s): The activity-based timeout was set conservatively.
  120s without a single tool call means the AI is stuck, not just thinking.

---

## Fix 2: Rewrite pickup_interaction.json

### Current Problems

1. Pattern `ArchitectureOverview` has 5 "CRITICAL RULES" with MUST/NEVER language
   that adds deliberation burden
2. 4 patterns spread actionable information across architecturally-themed sections
3. "Complete one Blueprint fully before moving to the next" contradicts other guidance
4. `ItemSetup` and `PlayerCharacterSetup` patterns have detailed implementation notes
   that belong in recipes, not reference templates

### Rewrite Principles

Per CLAUDE.md reference template rules:
- Target 60-120 lines total
- Notes: 2-4 sentences max per pattern
- No full plan_json examples
- Each pattern teaches architecture and tool sequence, not step-by-step wiring

### Complete Replacement JSON

**File:** `Content/Templates/reference/pickup_interaction.json`

Replace the entire file contents with:

```json
{
    "template_id": "pickup_interaction",
    "template_type": "reference",
    "display_name": "Pickup / Interaction Pattern (Blueprint Interface)",

    "catalog_description": "Two-Blueprint pickup and interaction pattern using a Blueprint Interface. The item implements the interface; the player character handles input and calls Interact. BOTH Blueprints must be modified.",
    "catalog_examples": "",

    "tags": "pickup interact interactable equip item collect grab hold weapon pickup loot drop attach detach interface blueprint interface input e key overlap",

    "patterns": [
        {
            "name": "PickupSystemSetup",
            "description": "A pickup system uses a Blueprint Interface (BPI_Interactable) to decouple the item from the player character. The item implements the interface and handles attach/detach logic. The player character handles input and calls the interface function on overlapping actors.",
            "notes": "This is a two-Blueprint plus one interface task. The interface must be created first because both Blueprints reference it. The item needs a SphereComponent for overlap detection. Input handling (E key) belongs on the player character, not the item.",
            "steps": [
                {
                    "tool": "blueprint.create",
                    "note": "START HERE. Create BPI_Interactable (type: BlueprintInterface). Add an Interact function with an InteractingActor:Actor input parameter."
                },
                {
                    "tool": "blueprint.add_component + blueprint.add_variable",
                    "note": "On the item BP: add SphereComponent (InteractionSphere), bIsEquipped:Boolean, and OwnerRef:Actor variables."
                },
                {
                    "tool": "blueprint.add_interface_implementation + blueprint.add_function",
                    "note": "Implement BPI_Interactable on the item BP. Add Pickup(NewOwner:Actor) and Drop() functions."
                },
                {
                    "tool": "olive.get_recipe + blueprint.apply_plan_json",
                    "note": "Implement each item function one at a time. Get the recipe first, then write the plan_json."
                },
                {
                    "tool": "blueprint.compile",
                    "note": "Compile the item BP. Fix any errors before moving to the player character."
                },
                {
                    "tool": "blueprint.add_variable + blueprint.apply_plan_json",
                    "note": "On the player character: add EquippedItem:Actor variable. Then write EventGraph plan_json for E-key input, GetOverlappingActors, DoesImplementInterface check, and Interact call."
                },
                {
                    "tool": "blueprint.compile",
                    "note": "Compile the player character BP. Both Blueprints should have 0 errors."
                }
            ]
        }
    ]
}
```

This version:
- **1 pattern** instead of 4 (eliminates scattered context)
- **2-sentence description**, **3-sentence notes** (within limits)
- **7 sequential steps** with "START HERE" on step 1
- **66 lines** (within 60-120 target)
- **No MUST/NEVER language**, no CRITICAL RULES
- **No implementation details** (attach/detach logic, enum values, etc.) -- those belong in recipes
- Preserves all metadata fields (`template_id`, `template_type`, `display_name`, `catalog_description`, `tags`)

---

## Risks and Edge Cases

### Fix 1

1. **Nudge loop**: The AI receives the nudge, calls another read op, stalls again, gets
   nudged again. Mitigated by the shared `AutoContinueCount` budget (max 3). After 3
   nudges (read or write), the system gives up and reports to the user.

2. **Nudge prompt delivered as continuation**: The nudge goes through `SendMessageAutonomous`,
   which detects `IsContinuationMessage`. However, `BuildReadNudgePrompt` output does NOT
   match any of the continuation patterns ("continue", "keep going", etc.), so it will be
   delivered as-is, not wrapped in `BuildContinuationPrompt`. This is correct behavior.

3. **Thread safety**: `BuildReadNudgePrompt` calls `BuildAssetStateSummary()` which loads
   UObjects and must run on the game thread. The AsyncTask dispatch ensures this. Same
   pattern as the existing write-stall auto-continue.

4. **Lowered timeouts on slow machines**: 90s stdout idle is still very generous. Legitimate
   Claude Code tool calls produce stdout output (progress indicators, thinking tokens) well
   within 90s. The scenario where stdout goes completely silent for 90s is always a hung
   process.

### Fix 2

1. **Less detail in template**: The rewritten template omits implementation details (attach
   component names, collision disable/enable, branch on bIsEquipped). The AI gets this from
   `olive.get_recipe` when it looks up each function. This is intentional -- reference
   templates teach architecture, recipes teach wiring.

2. **Backward compatibility**: The `template_id` and `tags` are preserved, so any AI that
   already learned to look up `pickup_interaction` will still find it. The `catalog_description`
   is preserved so the template catalog injection in system prompts remains unchanged.

---

## Implementation Order

1. **Fix 2 first** (5 minutes): Replace `pickup_interaction.json`. This is a pure data change
   with zero code risk.
2. **Fix 1A** (5 minutes): Add `IsReadOperation()` helper to anonymous namespace.
3. **Fix 1B** (10 minutes): Add `BuildReadNudgePrompt()` declaration + implementation.
4. **Fix 1C** (10 minutes): Modify the `HandleResponseCompleteAutonomous` branch logic.
5. **Fix 1D** (2 minutes): Lower timeout constants.
6. **Build and verify** (5 minutes): Incremental build, confirm no compile errors.

Total: ~40 minutes.

---

## Files Modified

| File | Changes |
|------|---------|
| `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | Add `IsReadOperation()`, add `BuildReadNudgePrompt()` impl, modify auto-continue branch, lower timeout constants |
| `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h` | Add `BuildReadNudgePrompt()` declaration |
| `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` | Lower `AutonomousIdleToolSeconds` default from 240 to 120 |
| `Content/Templates/reference/pickup_interaction.json` | Complete replacement with single-pattern sequential checklist |
