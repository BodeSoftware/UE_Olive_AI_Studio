# Phase 3: Error Recovery, Loop Prevention & Context Injection -- Design Document

**Author:** Architect Agent
**Date:** 2026-02-25
**Source:** `plans/olive_master_implementation_plan_v2.md`, Changes 3.1-3.4
**Effort estimate:** ~6 hours
**Files touched:** 5

---

## Table of Contents

1. [Overview](#overview)
2. [Change 3.1: Plan Content Deduplication](#change-31)
3. [Change 3.2: Progressive Error Disclosure](#change-32)
4. [Change 3.3: Update Stale Guidance Strings](#change-33)
5. [Change 3.4: Blueprint Context Injection](#change-34)
6. [Implementation Order](#implementation-order)
7. [Task List](#task-list)

---

<a id="overview"></a>
## 1. Overview

Phase 3 addresses three classes of problem:

1. **Identical plan retry loops** -- the AI resubmits the same failing plan verbatim, wasting retry budget without progress.
2. **Information starvation on retries** -- on first failure, the AI gets maximum error detail even when a terse nudge would suffice; on third failure, it gets the same terse nudge when it needs maximum detail.
3. **Hallucinated context** -- the AI guesses component/variable names because it has no visibility into the Blueprint's current structure.

The changes are independent of each other (no data flow between them), so they can be implemented and tested in any order. However, I recommend the order below for incremental compilation safety.

### Dependencies on Other Phases

- **None from Phase 1 or Phase 2.** Phase 3 operates on SelfCorrectionPolicy, PromptAssembler, and OperationHistory -- all independent of the plan executor and resolver changes.
- Phase 3 changes ARE compatible with Phase 1/2 changes (no conflicts in any file).

---

<a id="change-31"></a>
## 2. Change 3.1: Plan Content Deduplication

### Problem

When `apply_plan_json` or `preview_plan_json` fails, the SelfCorrectionPolicy feeds back error guidance. The AI sometimes resubmits the exact same plan, hoping for a different result. The existing `FOliveLoopDetector` catches repeated *error signatures* (tool+error_code+asset), but two submissions of the same plan with different error messages get different signatures. We need content-based dedup that hashes the actual plan JSON.

### Design Decision: Where to Hash

The master plan suggests accessing the plan JSON from `FOliveOperationHistoryStore` via a `FindLatest()` method. However, `FOliveSelfCorrectionPolicy` currently has no reference to the HistoryStore, and adding a cross-cutting dependency from a Brain-layer class to the ConversationManager's private HistoryStore is architecturally unclean.

**Better approach:** Pass the tool call arguments (which contain the plan JSON) into `Evaluate()` as an additional parameter. The `HandleToolResult()` call site in `OliveConversationManager.cpp` already has access to `ActiveToolCallArgs[ToolCallId]` right before it calls `Evaluate()`. This avoids any new singleton access or cross-module coupling.

### Files Modified

| File | What Changes |
|------|-------------|
| `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h` | Add `PreviousPlanHashes` map, `BuildPlanHash()` helper, new `Evaluate()` parameter |
| `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | Add dedup check at top of `Evaluate()`, implement `BuildPlanHash()`, reset in `Reset()` |
| `Source/OliveAIEditor/Public/Brain/OliveRetryPolicy.h` | Make `HashString()` public (move from `private:` to `public:`) |
| `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` | Pass tool call args to `Evaluate()` |

### Interface Additions

**OliveSelfCorrectionPolicy.h** -- add to class declaration:

```cpp
public:
    // Existing Evaluate signature changes -- add ToolCallArgs parameter:
    FOliveCorrectionDecision Evaluate(
        const FString& ToolName,
        const FString& ResultJson,
        FOliveLoopDetector& LoopDetector,
        const FOliveRetryPolicy& Policy,
        const FString& AssetContext = TEXT(""),
        const TSharedPtr<FJsonObject>& ToolCallArgs = nullptr  // NEW
    );

private:
    /**
     * Build a stable hash of the plan content from tool call arguments.
     * Returns empty string if ToolCallArgs is null or doesn't contain a plan object.
     * Hash covers: tool name + asset_path + graph_name + condensed plan JSON.
     */
    FString BuildPlanHash(const FString& ToolName, const TSharedPtr<FJsonObject>& ToolCallArgs) const;

    /**
     * Map of plan content hash -> submission count.
     * Tracks how many times each unique plan has been submitted.
     * Reset per user message turn (via Reset()).
     */
    TMap<FString, int32> PreviousPlanHashes;
```

**OliveRetryPolicy.h** -- move `HashString` from `private:` to `public:` on `FOliveLoopDetector`:

```cpp
public:
    /** Simple CRC32-based string hash. Used by multiple subsystems. */
    static FString HashString(const FString& Input);
```

### Data Flow

```
HandleToolResult()
  |
  +-- Reads ActiveToolCallArgs[ToolCallId] (TSharedPtr<FJsonObject>)
  |
  +-- Passes it as 6th arg to SelfCorrectionPolicy.Evaluate()
  |
  v
Evaluate()
  |
  +-- If ToolName is "apply_plan_json" or "preview_plan_json":
  |     +-- BuildPlanHash(ToolName, ToolCallArgs)
  |     +-- If hash found in PreviousPlanHashes:
  |     |     +-- Increment count
  |     |     +-- If count < 3: FeedBackErrors with IDENTICAL PLAN message
  |     |     +-- If count >= 3: StopWorker
  |     |     +-- Return early (skip normal tool failure check)
  |     +-- Else: Add hash to map with count=1
  |
  +-- Continue with normal HasCompileFailure / HasToolFailure checks
```

### BuildPlanHash Implementation Detail

```cpp
FString FOliveSelfCorrectionPolicy::BuildPlanHash(
    const FString& ToolName,
    const TSharedPtr<FJsonObject>& ToolCallArgs) const
{
    if (!ToolCallArgs.IsValid())
    {
        return FString();
    }

    // Extract identifying fields
    FString AssetPath;
    ToolCallArgs->TryGetStringField(TEXT("asset_path"), AssetPath);

    FString GraphName;
    ToolCallArgs->TryGetStringField(TEXT("graph_name"), GraphName);

    // Extract and serialize the plan object
    const TSharedPtr<FJsonObject>* PlanObj = nullptr;
    FString PlanString;
    if (ToolCallArgs->TryGetObjectField(TEXT("plan"), PlanObj) && PlanObj && (*PlanObj).IsValid())
    {
        auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PlanString);
        FJsonSerializer::Serialize((*PlanObj).ToSharedRef(), Writer);
        Writer->Close();
    }

    if (PlanString.IsEmpty())
    {
        return FString();
    }

    // Composite key: tool + asset + graph + plan content
    const FString Composite = FString::Printf(TEXT("%s|%s|%s|%s"),
        *ToolName, *AssetPath, *GraphName, *PlanString);

    return FOliveLoopDetector::HashString(Composite);
}
```

### Dedup Check in Evaluate (insert at top, before compile failure check)

```cpp
// Plan deduplication: detect when AI submits identical plans
if ((ToolName == TEXT("blueprint.apply_plan_json") || ToolName == TEXT("blueprint.preview_plan_json"))
    && ToolCallArgs.IsValid())
{
    const FString PlanHash = BuildPlanHash(ToolName, ToolCallArgs);

    if (!PlanHash.IsEmpty())
    {
        int32& SubmitCount = PreviousPlanHashes.FindOrAdd(PlanHash, 0);
        SubmitCount++;

        if (SubmitCount > 1)
        {
            // Extract error info from this attempt's result for context
            FString ErrorCode, ErrorMessage;
            HasToolFailure(ResultJson, ErrorCode, ErrorMessage);

            Decision.Action = EOliveCorrectionAction::FeedBackErrors;
            Decision.EnrichedMessage = FString::Printf(
                TEXT("[IDENTICAL PLAN - Seen %d time(s)] Your plan is identical to a "
                     "previous submission that failed. Previous error: %s %s\n\n"
                     "You MUST change the failing step's approach. Specifically:\n"
                     "- If a function wasn't found, use blueprint.search_nodes first\n"
                     "- If pin connection failed, use @step.auto instead of exact names\n"
                     "- If component Target was missing, add a get_var step and wire it\n"
                     "- Consider using olive.get_recipe for the correct pattern\n"
                     "Do NOT resubmit the same plan."),
                SubmitCount, *ErrorCode, *ErrorMessage);

            UE_LOG(LogOliveAI, Warning,
                TEXT("SelfCorrection: Identical plan hash=%s, submission #%d"),
                *PlanHash, SubmitCount);

            // Escalate to stop after 3 identical submissions
            if (SubmitCount >= 3)
            {
                Decision.Action = EOliveCorrectionAction::StopWorker;
                Decision.LoopReport = FString::Printf(
                    TEXT("Stopped: identical plan submitted %d times without changes."),
                    SubmitCount);
            }

            return Decision;
        }
    }
}
```

### Reset

In `FOliveSelfCorrectionPolicy::Reset()`:
```cpp
void FOliveSelfCorrectionPolicy::Reset()
{
    PreviousPlanHashes.Empty();
}
```

### Call Site Change in OliveConversationManager.cpp

In `HandleToolResult()`, around line 1119, change:

```cpp
// BEFORE:
FOliveCorrectionDecision Decision = SelfCorrectionPolicy.Evaluate(
    ToolName, ResultContent, LoopDetector, RetryPolicy, AssetContext);

// AFTER:
// Look up tool call arguments for plan dedup
TSharedPtr<FJsonObject> ToolCallArgsForEval;
if (const TSharedPtr<FJsonObject>* FoundArgs = ActiveToolCallArgs.Find(ToolCallId))
{
    ToolCallArgsForEval = *FoundArgs;
}

FOliveCorrectionDecision Decision = SelfCorrectionPolicy.Evaluate(
    ToolName, ResultContent, LoopDetector, RetryPolicy, AssetContext, ToolCallArgsForEval);
```

### Edge Cases

1. **Plan with no `plan` object field** -- `BuildPlanHash` returns empty, dedup silently skipped. No impact on non-plan tools.
2. **Same plan, different asset_path** -- Different hash (asset_path is part of composite). Correct behavior: it IS a different operation.
3. **Same plan, different graph_name** -- Different hash. Correct: targeting different function graphs.
4. **Plan succeeds on first attempt** -- `HasToolFailure` returns false, the dedup check runs but the plan hash is stored. If the AI re-submits the same successful plan, the dedup fires. This is actually desirable -- submitting the same plan twice is wasteful even if it succeeds.
5. **Preview then apply with same plan** -- Different tool names = different hash. No false positive.
6. **CRC32 collision** -- Extremely unlikely for similar plan JSON strings. Acceptable risk since the consequence is a false positive (one extra "identical plan" message), not data loss.

### Verification

- Submit the same `apply_plan_json` plan twice. Second submission should get `[IDENTICAL PLAN - Seen 2 time(s)]` message.
- Submit the same plan 3 times. Third should trigger `StopWorker`.
- Submit a different plan (change one step). No dedup triggered.
- Submit same plan to different asset_path. No dedup triggered.
- Call a non-plan tool. No dedup triggered (silent pass-through).
- Start a new user message turn. `SelfCorrectionPolicy.Reset()` clears hashes.

---

<a id="change-32"></a>
## 3. Change 3.2: Progressive Error Disclosure

### Problem

Currently `BuildToolErrorMessage` always includes the full error message and guidance regardless of attempt number. On the first attempt, this overwhelms context. On the third attempt, it provides no escalation signal.

### Design

Restructure `BuildToolErrorMessage` to progressively disclose information:

- **Attempt 1:** Error code + targeted guidance. Omit raw error message (guidance is usually sufficient).
- **Attempt 2:** Error code + targeted guidance + full raw error message.
- **Attempt 3+:** Error code + targeted guidance + full raw error message + "try fundamentally different approach" escalation.

### Files Modified

| File | What Changes |
|------|-------------|
| `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | Restructure `BuildToolErrorMessage()` return value |

### Current Code (line 252-397)

The current `BuildToolErrorMessage` constructs:
```
[TOOL FAILED - Attempt X/Y] Tool 'Z' failed with error CODE: MESSAGE
FIX GUIDANCE: ...
```

The error code, full message, and guidance are all shown on every attempt.

### New Code

Replace the final return statement at line 396 (`return Header + TEXT("\nFIX GUIDANCE: ") + Guidance;`) with progressive disclosure:

```cpp
    // Progressive error disclosure based on attempt number
    FString Result;

    if (AttemptNum == 1)
    {
        // First attempt: header (error code only) + guidance.
        // Omit raw error message to avoid overwhelming context.
        FString ShortHeader = FString::Printf(
            TEXT("[TOOL FAILED - Attempt %d/%d] Tool '%s' error: %s"),
            AttemptNum, MaxAttempts, *ToolName, *ErrorCode);
        Result = ShortHeader + TEXT("\nFIX GUIDANCE: ") + Guidance;
    }
    else if (AttemptNum == 2)
    {
        // Second attempt: full header (with error message) + guidance.
        Result = Header + TEXT("\nFIX GUIDANCE: ") + Guidance;
    }
    else
    {
        // Third+ attempt: full header + guidance + escalation directive.
        Result = Header + TEXT("\nFIX GUIDANCE: ") + Guidance;
        Result += FString::Printf(
            TEXT("\n\n[ESCALATION - Attempt %d/%d] Previous approaches have not worked. "
                 "You MUST try a fundamentally different strategy:\n"
                 "- Use olive.get_recipe to find the correct pattern for this task\n"
                 "- Use @step.auto for ALL data wires instead of explicit pin names\n"
                 "- Simplify the plan by breaking it into smaller operations\n"
                 "- Read the Blueprint state with blueprint.read before retrying"),
            AttemptNum, MaxAttempts);
    }

    return Result;
```

### Important: Header Variable Keeps Full Message

The existing `Header` variable (line 259-261) already includes the full error message:
```cpp
FString Header = FString::Printf(
    TEXT("[TOOL FAILED - Attempt %d/%d] Tool '%s' failed with error %s: %s"),
    AttemptNum, MaxAttempts, *ToolName, *ErrorCode, *ErrorMessage);
```

For attempt 1, we construct a shorter header that omits `ErrorMessage`. For attempts 2+, we use the full `Header` as-is.

### Edge Cases

1. **ErrorMessage is empty** -- Short header and full header look identical. No harm.
2. **MaxAttempts is 1** -- Only attempt 1 path is hit. Guidance-only, which is appropriate for a single-shot.
3. **AttemptNum exceeds MaxAttempts** -- Falls into `else` branch. Correct: escalation is appropriate.

### Verification

- Trigger a tool failure 3 times with the same error code.
- Attempt 1 log: `[TOOL FAILED - Attempt 1/3] Tool 'X' error: CODE` (no raw message after the code).
- Attempt 2 log: `[TOOL FAILED - Attempt 2/3] Tool 'X' failed with error CODE: full message here`.
- Attempt 3 log: Same as 2 + `[ESCALATION - Attempt 3/3]` block.

---

<a id="change-33"></a>
## 4. Change 3.3: Update Stale Guidance Strings

### Problem

Two guidance strings reference outdated patterns that conflict with Phase 1 fixes:

1. `PLAN_RESOLVE_FAILED` mentions "get_var for a component" as a common mistake -- but after Phase 1.1, get_var for components IS correct. The actual mistake is `set_var` for a component.
2. `PLAN_VALIDATION_FAILED` references `GetComponentByClass` as the fix for `COMPONENT_FUNCTION_ON_ACTOR`, but after Phase 2.1, the executor auto-wires single-match components.

### Files Modified

| File | What Changes |
|------|-------------|
| `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | Two string replacements in `BuildToolErrorMessage()` |

### Change 3.3a: PLAN_RESOLVE_FAILED

**Location:** Line 333-339, the `PLAN_RESOLVE_FAILED` case.

**Current:**
```cpp
else if (ErrorCode == TEXT("PLAN_RESOLVE_FAILED") || ErrorCode == TEXT("PLAN_LOWER_FAILED") || ErrorCode == TEXT("PLAN_EXECUTION_FAILED"))
{
    Guidance = TEXT("The plan failed during resolution or execution. Check the error details for which step failed. "
        "If a step used the wrong pattern (e.g., get_var for a component, or an invented function name), "
        "call olive.get_recipe with a query describing what you need (e.g., 'component reference' or 'spawn actor') "
        "to get the correct pattern. Fix the failing step and resubmit the corrected plan.");
}
```

**New:**
```cpp
else if (ErrorCode == TEXT("PLAN_RESOLVE_FAILED") || ErrorCode == TEXT("PLAN_LOWER_FAILED") || ErrorCode == TEXT("PLAN_EXECUTION_FAILED"))
{
    Guidance = TEXT("The plan failed during resolution or execution. Check the error details for which step failed. "
        "Common mistakes: set_var on a component (use get_var to read, then call setter), "
        "invented function names (search with blueprint.search_nodes first), "
        "wrong pin names (use @step.auto instead of guessing). "
        "Call olive.get_recipe with a query describing what you need (e.g., 'component reference' or 'spawn actor') "
        "to get the correct pattern. Fix the failing step and resubmit the corrected plan.");
}
```

### Change 3.3b: PLAN_VALIDATION_FAILED

**Location:** Line 384-389, the `PLAN_VALIDATION_FAILED` case.

**Current:**
```cpp
else if (ErrorCode == TEXT("PLAN_VALIDATION_FAILED"))
{
    Guidance = TEXT("The plan has structural issues detected before execution. Read the error details carefully. "
        "COMPONENT_FUNCTION_ON_ACTOR: add a GetComponentByClass step and wire its output to Target. "
        "EXEC_WIRING_CONFLICT: remove exec_after and restructure using exec_outputs on the branch node. "
        "Fix the plan and resubmit.");
}
```

**New:**
```cpp
else if (ErrorCode == TEXT("PLAN_VALIDATION_FAILED"))
{
    Guidance = TEXT("The plan has structural issues detected before execution. Read the error details carefully. "
        "COMPONENT_FUNCTION_ON_ACTOR: if only one matching component exists, "
        "the executor will auto-wire it (no action needed). If multiple components "
        "match, add a get_var step for the specific component and wire its output to Target. "
        "EXEC_WIRING_CONFLICT: remove exec_after and restructure using exec_outputs on the branch node. "
        "Fix the plan and resubmit.");
}
```

### Edge Cases

None -- these are pure string replacements with no behavioral change beyond the guidance text.

### Verification

- Trigger a `PLAN_RESOLVE_FAILED` error. Guidance should mention "set_var on a component" not "get_var for a component".
- Trigger a `PLAN_VALIDATION_FAILED` with `COMPONENT_FUNCTION_ON_ACTOR`. Guidance should mention auto-wiring for single components.

---

<a id="change-34"></a>
## 5. Change 3.4: Blueprint Context Injection

### Problem

The AI frequently halluccinates component names, variable names, and parent classes because it has no visibility into the Blueprint's current structure when assembling the plan. It must guess or use a separate `blueprint.read` call first, wasting an iteration.

### Design Decision: Where to Inject

There are two prompt paths:

1. **API providers** -- `FOlivePromptAssembler::AssembleSystemPromptInternal()` assembles the system prompt. It already calls `GetActiveContext()` for assets in `ActiveContextPaths`, but `GetActiveContext()` only shows asset name, class, parent class, and interfaces -- NOT components or variables.

2. **CLI providers** -- `FOlivePromptAssembler::BuildSharedSystemPreamble()` provides cross-cutting context. It does NOT include active asset context (that's in `AssembleSystemPromptInternal` only).

The cleanest injection point is **inside `GetActiveContext()`** in `OlivePromptAssembler.cpp`. When iterating over context assets, if an asset is a Blueprint, load it and append component/variable info. This catches BOTH prompt paths because `GetActiveContext` is called from `AssembleSystemPromptInternal`.

For the CLI path, `BuildSharedSystemPreamble` does NOT call `GetActiveContext`. So CLI providers do not currently get blueprint context. This is acceptable because CLI providers (Claude Code) use `blueprint.read` as their first tool call anyway, and adding the full context to the CLI stdin prompt would increase cost. If this becomes a problem, a future change can add an optional asset path parameter to `BuildSharedSystemPreamble`.

### Files Modified

| File | What Changes |
|------|-------------|
| `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h` | Add `BuildBlueprintContextBlock()` method declaration |
| `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp` | Implement `BuildBlueprintContextBlock()`, call it from `GetActiveContext()` |

### Interface Additions

**OlivePromptAssembler.h** -- add to public section:

```cpp
    /**
     * Build a compact context block for a Blueprint asset showing its
     * components and variables. Used by GetActiveContext() to enrich
     * Blueprint context in the system prompt.
     *
     * @param AssetPath Path to the Blueprint asset
     * @return Context block string, or empty if asset is not a Blueprint or not found
     */
    FString BuildBlueprintContextBlock(const FString& AssetPath) const;
```

### Implementation

**OlivePromptAssembler.cpp** -- new method:

```cpp
FString FOlivePromptAssembler::BuildBlueprintContextBlock(const FString& AssetPath) const
{
    // Load the Blueprint asset. Use FindObject first (may already be loaded),
    // fall back to LoadObject. Suppress loading in PIE or during save.
    UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *AssetPath);
    if (!Blueprint)
    {
        // Try loading with the full object path
        FString FullPath = AssetPath;
        if (!FullPath.EndsWith(TEXT(".") + FPaths::GetBaseFilename(AssetPath)))
        {
            FullPath = AssetPath + TEXT(".") + FPaths::GetBaseFilename(AssetPath);
        }
        Blueprint = LoadObject<UBlueprint>(nullptr, *FullPath);
    }

    if (!Blueprint)
    {
        return FString();
    }

    FString Block;

    // Components
    if (Blueprint->SimpleConstructionScript)
    {
        TArray<USCS_Node*> AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
        if (AllNodes.Num() > 0)
        {
            Block += TEXT("  Components:\n");
            for (USCS_Node* SCSNode : AllNodes)
            {
                if (!SCSNode) continue;
                FString ClassName = SCSNode->ComponentClass
                    ? SCSNode->ComponentClass->GetName() : TEXT("Unknown");
                Block += FString::Printf(TEXT("    - %s (%s)\n"),
                    *SCSNode->GetVariableName().ToString(), *ClassName);
            }
        }
    }

    // Variables
    if (Blueprint->NewVariables.Num() > 0)
    {
        Block += TEXT("  Variables:\n");
        for (const FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            FString TypeStr = Var.VarType.PinCategory.ToString();
            // Add subcategory for object/struct types
            if (!Var.VarType.PinSubCategoryObject.IsNull())
            {
                if (UObject* SubCatObj = Var.VarType.PinSubCategoryObject.Get())
                {
                    TypeStr = SubCatObj->GetName();
                }
            }
            Block += FString::Printf(TEXT("    - %s (%s)\n"),
                *Var.VarName.ToString(), *TypeStr);
        }
    }

    return Block;
}
```

**Injection into GetActiveContext()** -- in `OlivePromptAssembler.cpp`, line ~204 (inside the `AssetInfo.IsSet()` block, after the Interfaces section):

After the current code:
```cpp
            // Add interfaces
            if (AssetInfo->Interfaces.Num() > 0)
            {
                AssetLine += TEXT("  Interfaces: ");
                AssetLine += FString::Join(AssetInfo->Interfaces, TEXT(", "));
                AssetLine += TEXT("\n");
            }
```

Add:
```cpp
            // Add Blueprint component/variable context for Blueprints
            if (AssetInfo->bIsBlueprint)
            {
                FString BPContext = BuildBlueprintContextBlock(Path);
                if (!BPContext.IsEmpty())
                {
                    AssetLine += BPContext;
                }
            }
```

### Token Budget Impact

A typical Blueprint with 3 components and 3 variables adds ~100-150 characters (~25-38 tokens). The existing token budget check in `GetActiveContext()` already guards against overflow -- if the enriched `AssetLine` exceeds the remaining budget, the asset is skipped with a truncation note. No additional budget management needed.

### Required Includes

In `OlivePromptAssembler.cpp`, add these includes:
```cpp
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
```

### Data Flow

```
ConversationManager::BuildSystemMessage()
  |
  +-- FOlivePromptAssembler::AssembleSystemPrompt()
       |
       +-- AssembleSystemPromptInternal()
            |
            +-- GetActiveContext(ContextAssetPaths, RemainingTokens)
                 |
                 +-- For each asset in ActiveContextPaths:
                      +-- FOliveProjectIndex::GetAssetByPath()
                      +-- If bIsBlueprint: BuildBlueprintContextBlock(Path)
                      |    +-- FindObject / LoadObject to get UBlueprint*
                      |    +-- Iterate SCS nodes -> component names + classes
                      |    +-- Iterate NewVariables -> variable names + types
                      +-- Token budget check -> include or truncate
```

### Edge Cases

1. **Blueprint not loaded** -- `LoadObject` loads it on demand. This is acceptable in the editor context (we are assembling a prompt, not in a hot path). If the Blueprint fails to load (corrupt, missing), `BuildBlueprintContextBlock` returns empty and the asset context falls back to the basic name/class/parent info.

2. **Blueprint with no SCS** -- `SimpleConstructionScript` is null for some Blueprint types (e.g., function libraries, interfaces). The method returns empty. The basic asset line still shows.

3. **Component Blueprint** -- Components themselves can be Blueprints. Their SCS would be for sub-components. The method works correctly regardless.

4. **Very large Blueprint (50+ components, 50+ variables)** -- The token budget check in `GetActiveContext` prevents this from overwhelming the context. If the enriched line exceeds the budget, it gets truncated.

5. **PIE active** -- Loading assets during PIE is safe for read-only operations (we never modify the Blueprint here). No guard needed.

6. **Asset path format** -- The path in `ActiveContextPaths` may be a package path (`/Game/Blueprints/BP_Gun`) or an object path (`/Game/Blueprints/BP_Gun.BP_Gun`). The method tries both formats via `FindObject` then `LoadObject` with suffix.

### Verification

- Set `ActiveContextPaths` to include a Blueprint with components and variables.
- Call `AssembleSystemPrompt()` and inspect the result.
- The `## Active Context Assets` section should show component and variable lists under the Blueprint entry.
- Verify token count stays within budget by testing with a Blueprint that has 20+ components.

---

<a id="implementation-order"></a>
## 6. Implementation Order

```
Task 1: Change 3.3 (stale guidance strings)     -- 15 min, 0 dependencies
Task 2: Change 3.2 (progressive error disclosure) -- 30 min, 0 dependencies
Task 3: Change 3.1 (plan content dedup)           -- 2 hours, depends on HashString being public
Task 4: Change 3.4 (blueprint context injection)   -- 2 hours, 0 dependencies
```

Rationale:
- 3.3 is a trivial string replacement with zero risk. Do it first for an easy compile check.
- 3.2 is a refactor of one function's return logic. Quick and self-contained.
- 3.1 requires interface changes to `Evaluate()` (new parameter) and `HashString` visibility change, plus changes in two files. More complex, do it third.
- 3.4 is in a completely separate file (`OlivePromptAssembler`) and requires new UE includes. Save for last to keep the SelfCorrectionPolicy changes isolated.

---

<a id="task-list"></a>
## 7. Task List

### Task 1: Update Stale Guidance Strings (Change 3.3)

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

1. **3.3a:** Find the `PLAN_RESOLVE_FAILED` case (line 333-339). Replace `"get_var for a component"` with `"set_var on a component (use get_var to read, then call setter)"`. Also replace `"or an invented function name"` with `"invented function names (search with blueprint.search_nodes first), wrong pin names (use @step.auto instead of guessing)"`. Full replacement text is in [Change 3.3a section](#change-33) above.

2. **3.3b:** Find the `PLAN_VALIDATION_FAILED` case (line 384-389). Replace the `COMPONENT_FUNCTION_ON_ACTOR` guidance from `"add a GetComponentByClass step and wire its output to Target"` to the auto-wiring-aware text in [Change 3.3b section](#change-33) above.

**Expected after task:** Compiles cleanly. No behavioral change except guidance text.

---

### Task 2: Progressive Error Disclosure (Change 3.2)

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

1. Find the `return` statement at the end of `BuildToolErrorMessage()` (line 396):
   ```cpp
   return Header + TEXT("\nFIX GUIDANCE: ") + Guidance;
   ```

2. Replace it with the progressive disclosure logic from [Change 3.2 section](#change-32). The key difference: for `AttemptNum == 1`, construct a shorter header that omits `ErrorMessage`.

3. The existing `Header` variable (line 259) stays unchanged. We use it for attempts 2+.

**Expected after task:** Compiles cleanly. First failure shows code-only header; second adds full message; third adds escalation.

---

### Task 3: Plan Content Deduplication (Change 3.1)

This task spans 4 files. Follow this sub-order:

**Step 3a: Make HashString public**

**File:** `Source/OliveAIEditor/Public/Brain/OliveRetryPolicy.h`

1. Move the `HashString` declaration from `private:` to `public:` on `FOliveLoopDetector`. It is currently at line 115 (under `private:`). Move it to after line 93 (after `BuildLoopReport()`), inside the `public:` section.

2. Add a doc comment:
   ```cpp
   /** Simple CRC32-based string hash. Returns 8-char hex string. */
   static FString HashString(const FString& Input);
   ```

**Expected:** Compiles. No behavioral change.

**Step 3b: Add members and method to SelfCorrectionPolicy**

**File:** `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h`

1. Add `#include "Dom/JsonObject.h"` at the top (after existing includes) if not already present. Check: the file currently does NOT include it. Add it.

2. Change the `Evaluate()` signature to add the optional `ToolCallArgs` parameter:
   ```cpp
   FOliveCorrectionDecision Evaluate(
       const FString& ToolName,
       const FString& ResultJson,
       FOliveLoopDetector& LoopDetector,
       const FOliveRetryPolicy& Policy,
       const FString& AssetContext = TEXT(""),
       const TSharedPtr<FJsonObject>& ToolCallArgs = nullptr
   );
   ```

3. Add to the `private:` section (after `BuildToolErrorMessage`):
   ```cpp
   /**
    * Build a stable hash of plan content from tool call arguments.
    * Returns empty string if args don't contain a plan object.
    */
   FString BuildPlanHash(const FString& ToolName, const TSharedPtr<FJsonObject>& ToolCallArgs) const;

   /** Map of plan content hash -> submission count. Reset per turn. */
   TMap<FString, int32> PreviousPlanHashes;
   ```

**Expected:** Compiles (cpp file not yet updated, but no references to new members fail).

**Step 3c: Implement dedup logic in cpp**

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

1. Add include at top:
   ```cpp
   #include "Serialization/JsonWriter.h"
   ```
   Note: `Serialization/JsonSerializer.h` is already included. Check if `JsonWriter.h` is needed for `TJsonWriterFactory`. Actually, `JsonSerializer.h` typically includes what's needed. Verify by building -- if `TCondensedJsonPrintPolicy` is not found, add `#include "Policies/CondensedJsonPrintPolicy.h"`.

2. Update the `Evaluate()` signature to match the header (add `const TSharedPtr<FJsonObject>& ToolCallArgs` parameter).

3. Insert the dedup check block at the TOP of `Evaluate()`, after the `FOliveCorrectionDecision Decision;` declaration (line 20) and BEFORE the `HasCompileFailure` check (line 23). Full code is in [Change 3.1 section](#change-31).

4. Update `Reset()` to clear the hash map:
   ```cpp
   void FOliveSelfCorrectionPolicy::Reset()
   {
       PreviousPlanHashes.Empty();
   }
   ```

5. Add `BuildPlanHash()` implementation at the end of the file. Full code is in [Change 3.1 section](#change-31).

**Expected:** Compiles. Plan dedup is active.

**Step 3d: Pass ToolCallArgs through from ConversationManager**

**File:** `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`

1. Find line ~1119 where `SelfCorrectionPolicy.Evaluate()` is called.

2. Just BEFORE the call, add:
   ```cpp
   // Look up tool call arguments for plan dedup
   TSharedPtr<FJsonObject> ToolCallArgsForEval;
   if (const TSharedPtr<FJsonObject>* FoundArgs = ActiveToolCallArgs.Find(ToolCallId))
   {
       ToolCallArgsForEval = *FoundArgs;
   }
   ```

3. Change the `Evaluate()` call to pass the new argument:
   ```cpp
   FOliveCorrectionDecision Decision = SelfCorrectionPolicy.Evaluate(
       ToolName, ResultContent, LoopDetector, RetryPolicy, AssetContext, ToolCallArgsForEval);
   ```

**Expected:** Compiles. Full dedup pipeline is wired.

---

### Task 4: Blueprint Context Injection (Change 3.4)

**Step 4a: Add method declaration**

**File:** `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h`

1. Add to the public section (after `GetKnowledgePackById`):
   ```cpp
   /**
    * Build a compact context block for a Blueprint asset showing its
    * components and variables. Returns empty string if asset is not
    * a Blueprint or cannot be loaded.
    *
    * @param AssetPath Package path to the Blueprint asset
    * @return Indented context block, or empty string
    */
   FString BuildBlueprintContextBlock(const FString& AssetPath) const;
   ```

**Expected:** Compiles (method not yet implemented, but no calls yet either).

**Step 4b: Implement BuildBlueprintContextBlock**

**File:** `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp`

1. Add includes at the top (after existing includes):
   ```cpp
   #include "Engine/Blueprint.h"
   #include "Engine/SimpleConstructionScript.h"
   #include "Engine/SCS_Node.h"
   ```

2. Add the implementation before the `// Private Methods` section (or at the end of the Components section, after `GetKnowledgePackById`). Full implementation is in [Change 3.4 section](#change-34).

**Expected:** Compiles. Method exists but is not called yet.

**Step 4c: Call from GetActiveContext**

**File:** `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp`

1. Find `GetActiveContext()` method (line 181).

2. Inside the `if (AssetInfo.IsSet())` block, after the Interfaces section (after line ~216: `AssetLine += TEXT("\n");` closing the interfaces block), add:

   ```cpp
            // Add Blueprint component/variable context
            if (AssetInfo->bIsBlueprint)
            {
                FString BPContext = BuildBlueprintContextBlock(Path);
                if (!BPContext.IsEmpty())
                {
                    AssetLine += BPContext;
                }
            }
   ```

   IMPORTANT: Insert this AFTER the `Interfaces` block but BEFORE the closing of the `if (AssetInfo.IsSet())` block. The token budget check on line 222 (`if (CurrentTokens + LineTokens > MaxTokens)`) will naturally handle the larger `AssetLine`.

**Expected:** Compiles. Blueprint assets in `ActiveContextPaths` now show component/variable lists in the system prompt.

---

## Summary of All File Changes

| File | Changes | Task |
|------|---------|------|
| `Source/OliveAIEditor/Public/Brain/OliveRetryPolicy.h` | Move `HashString` to public | 3a |
| `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h` | Add include, new `Evaluate` param, `BuildPlanHash`, `PreviousPlanHashes` | 3b |
| `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | Dedup in `Evaluate`, `BuildPlanHash` impl, `Reset` update, progressive disclosure, guidance strings | 1, 2, 3c |
| `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` | Pass `ToolCallArgs` to `Evaluate` | 3d |
| `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h` | Add `BuildBlueprintContextBlock` declaration | 4a |
| `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp` | Add includes, implement `BuildBlueprintContextBlock`, call from `GetActiveContext` | 4b, 4c |

**No new files created.** All changes are modifications to existing files.

**No IR changes.** No changes to `OliveAIRuntime` module.

**No tool registration changes.** No schema changes.

---

## Verification Checklist

| Test | Expected | Task |
|------|----------|------|
| Compile after Task 1 | Clean | 1 |
| Compile after Task 2 | Clean | 2 |
| Compile after Task 3 | Clean | 3 |
| Compile after Task 4 | Clean | 4 |
| Submit identical plan twice | IDENTICAL PLAN message on 2nd | 3 |
| Submit identical plan 3 times | StopWorker on 3rd | 3 |
| Different plan, same error | Normal retry (no dedup) | 3 |
| Same plan, different asset_path | Normal retry (no dedup) | 3 |
| Non-plan tool call | No dedup (silent pass) | 3 |
| New user message turn | Hash map cleared via Reset() | 3 |
| 1st tool failure | Code-only header, guidance | 2 |
| 2nd tool failure | Full header + guidance | 2 |
| 3rd tool failure | Full header + guidance + escalation | 2 |
| PLAN_RESOLVE_FAILED guidance | Mentions "set_var on a component" | 1 |
| PLAN_VALIDATION_FAILED guidance | Mentions auto-wiring for single components | 1 |
| Blueprint in ActiveContextPaths | Components + variables in system prompt | 4 |
| Non-Blueprint in ActiveContextPaths | Basic info only (no BP context) | 4 |
| Blueprint with no components/variables | No extra context appended | 4 |
| Large Blueprint (20+ components) | Token budget truncates gracefully | 4 |
