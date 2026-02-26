# Autonomous Efficiency Round 4 -- Design Plan

**Author:** Architect Agent
**Date:** 2026-02-26
**Status:** Design Complete -- Ready for Implementation

---

## Executive Summary

Five fixes targeting autonomous mode performance. Testing showed autonomous mode is FASTER without templates (4:53) than with them (10:11) because the AI micro-edits template-created logic instead of trusting it. The AI also stalls on multi-asset tasks (120s idle timeout kills an incomplete run). These fixes eliminate the two biggest time sinks (template overhead, stalling) and reduce inference noise.

**Quality Constraint:** Every fix is quality-positive or quality-neutral. None sacrifice output quality for speed.

---

## Fix 1: Slim Templates + Write-Your-Own-Logic

### Problem

Template-created plans produce 62-second thinking gaps and 27 tool calls of micro-editing. The AI is demonstrably good at writing plan_json itself -- the no-template test produced working blueprints with only 1 failure and completed 2x faster.

### Root Cause

Templates embed full plan_json with 10-20 steps per function. The AI receives `function_details` showing these plans were executed, then reads the functions to understand them, then edits individual nodes. This is slower than the AI writing its own 6-10 step plan from scratch.

### Solution

Strip `plan` keys from ALL factory templates. Templates create STRUCTURE only: components, variables, empty function stubs (with correct signatures), event dispatchers. The AI writes its own plan_json for each function. Recipes (`olive.get_recipe`) provide tested wiring patterns as reference.

### Quality Impact

**Quality-positive.** The AI writes simpler plans it understands, leading to fewer retry loops. Recipes still provide correct patterns. The no-template run proved the AI produces working code without embedded plans.

### Task 1A: Slim gun.json

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Content\Templates\factory\gun.json`

Remove the `"plan"` key from all three functions: Fire, ResetCanFire, Reload. Keep everything else (function name, description, inputs, outputs).

**Before (Fire function, lines 171-301):**
```json
{
    "name": "Fire",
    "description": "Attempt to fire. Decrements ammo if available, fires OnFired. When ammo reaches zero, fires OnAmmoEmpty.",
    "inputs": [],
    "outputs": [
        { "name": "bSuccess", "type": "Boolean" }
    ],
    "plan": {
        "schema_version": "2.0",
        "steps": [
            ... 15 steps ...
        ]
    }
}
```

**After:**
```json
{
    "name": "Fire",
    "description": "Attempt to fire. Decrements ammo if available, fires OnFired. When ammo reaches zero, fires OnAmmoEmpty.",
    "inputs": [],
    "outputs": [
        { "name": "bSuccess", "type": "Boolean" }
    ]
}
```

Apply the same removal to `ResetCanFire` (lines 308-320) and `Reload` (lines 329-364). Remove the entire `"plan": { ... }` key-value pair from each.

### Task 1B: Slim stat_component.json

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Content\Templates\factory\stat_component.json`

Remove the `"plan"` key from `ApplyStatChange` (lines 107-203). Keep function name, description, inputs, outputs.

**After:**
```json
{
    "name": "Apply${stat_name}Change",
    "description": "Change stat by delta (negative = drain, positive = restore). Clamps to [0, max]. Fires change and depletion events.",
    "inputs": [
        { "name": "Delta", "type": "Float" }
    ],
    "outputs": [
        { "name": "NewValue", "type": "Float" }
    ]
}
```

### Task 1C: Verify projectile.json

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Content\Templates\factory\projectile.json`

This template has NO `plan` keys -- it only creates structure (components + variables). No changes needed. Verify and move on.

### Task 1D: Update AGENTS.md

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\AGENTS.md`

Replace the current template guidance (around line 15-16) with:

**Current (lines 15-17):**
```
1. Check templates first: `blueprint.list_templates` -- if one fits, use `blueprint.create_from_template`
2. After creation, check `function_details` in the result to see which functions have graph logic. Only `blueprint.read` if you need to modify a function the template already wired.
3. Otherwise: `blueprint.create` -> `blueprint.add_component` / `blueprint.add_variable` -> `blueprint.apply_plan_json`
```

**Replace with:**
```
1. Check templates first: `blueprint.list_templates` -- if one fits, use `blueprint.create_from_template`
2. Templates create STRUCTURE only (components, variables, empty function stubs, event dispatchers). After template creation, write plan_json for EACH function listed in the result. Call `olive.get_recipe` first for each function's wiring pattern.
3. Do NOT call `blueprint.read` or `blueprint.read_function` after template creation -- the functions are empty stubs waiting for your logic.
4. Otherwise: `blueprint.create` -> `blueprint.add_component` / `blueprint.add_variable` -> `blueprint.apply_plan_json`
```

Also update the recipe routing guidance (line 24):

**Current:**
```
**Before your first plan_json for each Blueprint:** Call `olive.get_recipe` with the pattern you need (e.g., "fire weapon", "spawn projectile", "health component"). Recipes contain tested wiring patterns that prevent common errors.
```

**Replace with:**
```
**Before your first plan_json for each function:** Call `olive.get_recipe` with the pattern you need (e.g., "fire weapon", "spawn projectile", "health component"). Recipes contain tested wiring patterns. This applies AFTER template creation too -- templates create empty function stubs, you write the logic.
```

Also update template-specific rules (line 84):

**Current:**
```
- **Always call `olive.get_recipe` before your first `apply_plan_json`** for each Blueprint. Recipes contain tested wiring patterns. Skip only if you already used `create_from_template` which embeds plans.
```

**Replace with:**
```
- **Always call `olive.get_recipe` before your first `apply_plan_json`** for each function. Recipes contain tested wiring patterns. This includes functions created by templates (they are empty stubs).
```

And line 86:
**Current:**
```
- Component overlap/hit events: use `op:"event"` with `target:"OnComponentBeginOverlap"` (auto-detects component). Add `"component_name":"MyComp"` in `properties` if ambiguous.
```
**Keep as-is.** This is correct and useful.

### Task 1E: Update recipe_routing.txt

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Content\SystemPrompts\Knowledge\recipe_routing.txt`

**Current line 1:**
```
## Routing
- ALWAYS call olive.get_recipe(query) before your first plan_json for each Blueprint. Contains tested wiring patterns. Skip only after create_from_template.
```

**Replace line 1:**
```
## Routing
- ALWAYS call olive.get_recipe(query) before your first plan_json for each function. Contains tested wiring patterns. This includes functions created by templates -- templates create empty stubs, you write the logic.
```

### Task 1F: Update cli_blueprint.txt

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Content\SystemPrompts\Knowledge\cli_blueprint.txt`

**Current template section (lines 67-71):**
```
## Templates
Before creating a Blueprint from scratch, check if an available template matches.
- blueprint.create_from_template(template_id, preset, asset_path) creates a complete Blueprint in one call.
- Templates include components, variables, event dispatchers, functions, and event graphs.
- Use a preset name if one matches (e.g., "Bullet", "Rocket" for projectile template).
- If no template matches, proceed with the normal CREATE workflow above.
```

**Replace with:**
```
## Templates
Before creating a Blueprint from scratch, check if an available template matches.
- blueprint.create_from_template(template_id, preset, asset_path) creates the Blueprint STRUCTURE in one call.
- Templates create: components, variables, event dispatchers, and empty function stubs (with correct signatures).
- After template creation, write plan_json for EACH function listed in the result. Call olive.get_recipe first for each.
- Do NOT call blueprint.read or read_function after template creation -- the functions are empty stubs.
- Use a preset name if one matches (e.g., "Bullet", "Rocket" for projectile template).
- Call list_templates ONCE per task. Call get_template only for templates you plan to use.
- If no template matches, proceed with the normal CREATE workflow above.
```

### Task 1G: Update SetupAutonomousSandbox CLAUDE.md

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Private\Providers\OliveCLIProviderBase.cpp`

In `SetupAutonomousSandbox()`, update the CLAUDE.md content at line 269:

**Current (line 269):**
```cpp
ClaudeMd += TEXT("- Before writing your first plan_json for each Blueprint, call olive.get_recipe to look up the correct wiring pattern. Skip only if create_from_template already provided the logic.\n");
```

**Replace with:**
```cpp
ClaudeMd += TEXT("- Before writing your first plan_json for each function, call olive.get_recipe to look up the correct wiring pattern. This includes functions created by templates -- templates create empty stubs, you write the logic.\n");
```

**Current (line 271):**
```cpp
ClaudeMd += TEXT("- After `create_from_template`, check the `function_details` in the result. Only call `blueprint.read` if you need to modify an existing function that the template already wired (to see its current pin layout). Do NOT read functions just to see what the template created -- the result already tells you.\n");
```

**Replace with:**
```cpp
ClaudeMd += TEXT("- After `create_from_template`, check the result for the list of created functions. Write plan_json for EACH function -- they are empty stubs. Do NOT call blueprint.read or read_function after template creation.\n");
```

---

## Fix 2: Auto-Continue on Stall

### Problem

The AI completes one function or asset, then goes silent for 120 seconds until the idle timeout kills it. Multi-asset tasks are left incomplete. This happened in EVERY multi-asset test.

### Root Cause

After completing one logical unit of work, the AI enters a long thinking phase where no stdout is produced and no tool calls are made. Both timeout mechanisms fire, killing an otherwise productive run.

### Solution

Reduce idle timeout to 50 seconds and automatically relaunch with continuation context. The `FAutonomousRunContext` infrastructure from Round 3 Fix 2 already tracks everything needed.

### Quality Impact

**Quality-positive.** Worst case: stalls 3 times and stops (same result as today, minus 210s of dead wait time). Best case: completes the full task across 2-3 shorter runs.

### Task 2A: Reduce Idle Timeout

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Private\Providers\OliveCLIProviderBase.cpp`

Change the anonymous namespace constant at line 31:

**Current:**
```cpp
constexpr double CLI_IDLE_TIMEOUT_SECONDS = 120.0;
```

**Replace with:**
```cpp
constexpr double CLI_IDLE_TIMEOUT_SECONDS = 50.0;
```

### Task 2B: Add Auto-Continue Counter to Header

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Public\Providers\OliveCLIProviderBase.h`

Add two new members to the `protected:` state section, after `bLastRunTimedOut` (line 433):

```cpp
/** Number of automatic continuations since last user-initiated message.
 *  Reset to 0 on each non-continuation SendMessageAutonomous call. */
int32 AutoContinueCount = 0;

/** Maximum automatic continuations before giving up and reporting to user */
static constexpr int32 MaxAutoContinues = 3;
```

### Task 2C: Implement Auto-Continue Logic

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Private\Providers\OliveCLIProviderBase.cpp`

**Step 1: Reset counter on fresh user messages.** In `SendMessageAutonomous()`, after the `bIsBusy` check (line 304) but before storing callbacks, add:

```cpp
// Reset auto-continue counter on fresh user messages (not continuations)
if (!LastRunContext.bValid || !IsContinuationMessage(UserMessage))
{
    AutoContinueCount = 0;
}
```

**Step 2: Add auto-continue in `HandleResponseCompleteAutonomous()`.** After the existing run context capture (after line 766: `// else stays Completed`), but BEFORE the log and error/completion callbacks, add auto-continue logic:

```cpp
// Auto-continue: if the run timed out (stalled) and made real progress,
// automatically relaunch with continuation context.
if (bLastRunTimedOut && LastRunContext.ToolCallLog.Num() > 0 && AutoContinueCount < MaxAutoContinues)
{
    AutoContinueCount++;

    UE_LOG(LogOliveCLIProvider, Log,
        TEXT("Auto-continuing after stall (attempt %d/%d): %d modified assets from previous run"),
        AutoContinueCount, MaxAutoContinues, LastRunContext.ModifiedAssetPaths.Num());

    // Build continuation prompt from the run context we just captured
    FString ContinuationPrompt = BuildContinuationPrompt(TEXT("continue"));

    // Release busy state so SendMessageAutonomous can acquire it
    bIsBusy = false;

    // Re-use the existing callbacks (they are still valid in CallbackLock scope).
    // Copy them out before the recursive call clears them.
    FOnOliveStreamChunk SavedOnChunk = CurrentOnChunk;
    FOnOliveComplete SavedOnComplete = CurrentOnComplete;
    FOnOliveError SavedOnError = CurrentOnError;

    // Dispatch the continuation on the next game thread tick to avoid
    // re-entering SendMessageAutonomous from within HandleResponseComplete.
    AsyncTask(ENamedThreads::GameThread, [this, ContinuationPrompt,
        SavedOnChunk, SavedOnComplete, SavedOnError]()
    {
        // Re-check AliveGuard before accessing members
        if (!(*AliveGuard))
        {
            return;
        }

        SendMessageAutonomous(ContinuationPrompt, SavedOnChunk, SavedOnComplete, SavedOnError);
    });

    return; // Skip normal completion callback
}
```

**CRITICAL: This `return` must happen BEFORE the existing completion/error callback dispatch (lines 777-790).** The auto-continue silently relaunches without notifying the user of the intermediate timeout.

**Step 3: Ensure the auto-continue prompt is treated as a continuation.** `IsContinuationMessage("continue")` already returns `true`, so `BuildContinuationPrompt` will be called again inside `SendMessageAutonomous`, enriching the prompt with the accumulated context.

### Edge Cases

1. **Provider destroyed during async dispatch:** The `AliveGuard` check in the lambda prevents use-after-free.
2. **User cancels during auto-continue:** `CancelRequest()` sets `bIsBusy = false` and kills the process. The next `SendMessageAutonomous` will see `bIsBusy = false` and proceed, but the auto-continue lambda will find `bIsBusy = true` from the cancel and bail. Actually: the cancel kills the process, which triggers completion, which may trigger another auto-continue. To prevent this, add a guard in the auto-continue check: `&& !bStopReading` (already set by CancelRequest).
3. **Three stalls in a row:** After `MaxAutoContinues`, flow falls through to the normal completion callback, which reports the accumulated text to the user.
4. **Run timeout vs stall timeout:** `bLastRunTimedOut` is set for both idle timeout and runtime limit. Auto-continue should fire for idle timeouts but NOT runtime limits (the run has been going long enough). Refine the check: distinguish `FAutonomousRunContext::EOutcome::IdleTimeout` vs `RuntimeLimit`.

**Refined condition (replaces the one above):**
```cpp
if (bLastRunTimedOut
    && LastRunContext.Outcome == FAutonomousRunContext::EOutcome::IdleTimeout
    && LastRunContext.ToolCallLog.Num() > 0
    && AutoContinueCount < MaxAutoContinues)
```

This ensures runtime-limit kills go straight to the user (the run was long enough, auto-continuing would exceed time budgets).

---

## Fix 3: Tool Count Reduction (Autonomous Mode Only)

### Problem

`tools/list` returns 104 tools. Every inference turn processes all 104 schemas. For "create a gun blueprint," PCG/C++/BT tools are pure noise.

### Root Cause

The MCP server returns all registered tools regardless of context. Focus profiles exist but are not applied in MCP mode.

### Solution

Add a tool filter on `FOliveMCPServer` that restricts `HandleToolsList` but NOT `HandleToolsCall`. The CLI provider sets the filter before launching the autonomous process and clears it on completion.

### Quality Impact

**Quality-neutral to quality-positive.** Hidden tools still execute if called (defense-in-depth). Ambiguous messages get all tools. Less noise means faster inference and fewer confused tool choices.

### Task 3A: Add Tool Filter API to MCP Server

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Public\MCP\OliveMCPServer.h`

Add to the public section (after `SendNotification`, before `private:`):

```cpp
// ==========================================
// Tool Filtering (Autonomous Mode)
// ==========================================

/**
 * Set a tool name prefix filter for tools/list responses.
 * When set, HandleToolsList returns only tools whose names start with
 * one of the allowed prefixes. HandleToolsCall is NOT affected (any
 * registered tool can still be called regardless of filter).
 *
 * @param AllowedPrefixes Set of tool name prefixes (e.g., {"blueprint.", "project.", "olive."})
 */
void SetToolFilter(const TSet<FString>& AllowedPrefixes);

/**
 * Clear the tool filter, returning to full tool list.
 */
void ClearToolFilter();
```

Add to the private state section (after `EventRetentionSeconds`):

```cpp
/** Active tool filter prefixes. Empty = no filter (return all tools). */
TSet<FString> ToolFilterPrefixes;

/** Lock for tool filter access (filter set on game thread, read on HTTP thread) */
mutable FCriticalSection ToolFilterLock;
```

### Task 3B: Implement Filter in MCP Server

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Private\MCP\OliveMCPServer.cpp`

**SetToolFilter/ClearToolFilter implementations** (add after `HandleEventsPoll`):

```cpp
void FOliveMCPServer::SetToolFilter(const TSet<FString>& AllowedPrefixes)
{
    FScopeLock Lock(&ToolFilterLock);
    ToolFilterPrefixes = AllowedPrefixes;
    UE_LOG(LogOliveAI, Log, TEXT("MCP tool filter set: %d prefixes"), AllowedPrefixes.Num());
}

void FOliveMCPServer::ClearToolFilter()
{
    FScopeLock Lock(&ToolFilterLock);
    ToolFilterPrefixes.Empty();
    UE_LOG(LogOliveAI, Log, TEXT("MCP tool filter cleared"));
}
```

**Modify `HandleToolsList`** (line 526). Replace:
```cpp
TSharedPtr<FJsonObject> FOliveMCPServer::HandleToolsList(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Result = FOliveToolRegistry::Get().GetToolsListMCP();

    // Log tool count for diagnostics
    int32 ToolCount = 0;
    const TArray<TSharedPtr<FJsonValue>>* ToolsArray;
    if (Result.IsValid() && Result->TryGetArrayField(TEXT("tools"), ToolsArray))
    {
        ToolCount = ToolsArray->Num();
    }
    UE_LOG(LogOliveAI, Log, TEXT("MCP tools/list: returning %d tools"), ToolCount);

    return Result;
}
```

**With:**
```cpp
TSharedPtr<FJsonObject> FOliveMCPServer::HandleToolsList(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> FullResult = FOliveToolRegistry::Get().GetToolsListMCP();

    // Apply tool filter if active
    TSet<FString> ActiveFilter;
    {
        FScopeLock Lock(&ToolFilterLock);
        ActiveFilter = ToolFilterPrefixes;
    }

    if (ActiveFilter.Num() > 0)
    {
        // Filter tools by prefix
        TArray<TSharedPtr<FJsonValue>> FilteredTools;
        const TArray<TSharedPtr<FJsonValue>>* AllTools;
        if (FullResult.IsValid() && FullResult->TryGetArrayField(TEXT("tools"), AllTools))
        {
            for (const TSharedPtr<FJsonValue>& ToolVal : *AllTools)
            {
                const TSharedPtr<FJsonObject>* ToolObj;
                if (ToolVal->TryGetObject(ToolObj) && ToolObj)
                {
                    FString ToolName;
                    (*ToolObj)->TryGetStringField(TEXT("name"), ToolName);

                    bool bAllowed = false;
                    for (const FString& Prefix : ActiveFilter)
                    {
                        if (ToolName.StartsWith(Prefix))
                        {
                            bAllowed = true;
                            break;
                        }
                    }

                    if (bAllowed)
                    {
                        FilteredTools.Add(ToolVal);
                    }
                }
            }
        }

        TSharedPtr<FJsonObject> FilteredResult = MakeShared<FJsonObject>();
        FilteredResult->SetArrayField(TEXT("tools"), FilteredTools);

        UE_LOG(LogOliveAI, Log, TEXT("MCP tools/list: returning %d/%d tools (filtered by %d prefixes)"),
            FilteredTools.Num(),
            AllTools ? AllTools->Num() : 0,
            ActiveFilter.Num());

        return FilteredResult;
    }

    // No filter -- return all tools
    int32 ToolCount = 0;
    const TArray<TSharedPtr<FJsonValue>>* ToolsArray;
    if (FullResult.IsValid() && FullResult->TryGetArrayField(TEXT("tools"), ToolsArray))
    {
        ToolCount = ToolsArray->Num();
    }
    UE_LOG(LogOliveAI, Log, TEXT("MCP tools/list: returning %d tools"), ToolCount);

    return FullResult;
}
```

**CRITICAL: `HandleToolsCall` is NOT modified.** Any registered tool can still be called regardless of filter.

### Task 3C: Set Filter in CLI Provider

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Private\Providers\OliveCLIProviderBase.cpp`

Add a helper function (in the anonymous namespace or as a private method):

```cpp
namespace
{
    /** Determine tool prefixes needed for a given user message */
    TSet<FString> DetermineToolPrefixes(const FString& Message)
    {
        FString Lower = Message.ToLower();

        // Always-included core tools
        TSet<FString> Prefixes = {
            TEXT("project."),
            TEXT("olive."),
            TEXT("cross_system."),
        };

        bool bHasBlueprint = Lower.Contains(TEXT("blueprint")) || Lower.Contains(TEXT("actor"))
            || Lower.Contains(TEXT("component")) || Lower.Contains(TEXT("variable"))
            || Lower.Contains(TEXT("function")) || Lower.Contains(TEXT("event graph"));
        bool bHasBT = Lower.Contains(TEXT("behavior tree")) || Lower.Contains(TEXT("behaviour tree"))
            || Lower.Contains(TEXT("blackboard")) || Lower.Contains(TEXT(" bt "))
            || Lower.Contains(TEXT(" ai "));
        bool bHasPCG = Lower.Contains(TEXT("pcg")) || Lower.Contains(TEXT("procedural"));
        bool bHasCpp = Lower.Contains(TEXT("c++")) || Lower.Contains(TEXT("cpp"))
            || Lower.Contains(TEXT("header")) || Lower.Contains(TEXT("source file"));

        // Count how many domains are explicitly mentioned
        int32 DomainCount = (bHasBlueprint ? 1 : 0) + (bHasBT ? 1 : 0)
            + (bHasPCG ? 1 : 0) + (bHasCpp ? 1 : 0);

        // If multiple domains or none (ambiguous), return empty to show all tools
        if (DomainCount > 1)
        {
            return TSet<FString>(); // Empty = no filter
        }

        if (DomainCount == 0)
        {
            // Default: assume Blueprint (most common use case)
            // Include animation and widget tools since they are Blueprint subtypes
            Prefixes.Add(TEXT("blueprint."));
            Prefixes.Add(TEXT("animbp."));
            Prefixes.Add(TEXT("widget."));
            return Prefixes;
        }

        if (bHasBlueprint)
        {
            Prefixes.Add(TEXT("blueprint."));
            Prefixes.Add(TEXT("animbp."));
            Prefixes.Add(TEXT("widget."));
        }
        if (bHasBT)
        {
            Prefixes.Add(TEXT("bt."));
            Prefixes.Add(TEXT("blackboard."));
            // Also include Blueprint tools (BT tasks often reference BPs)
            Prefixes.Add(TEXT("blueprint."));
        }
        if (bHasPCG)
        {
            Prefixes.Add(TEXT("pcg."));
        }
        if (bHasCpp)
        {
            Prefixes.Add(TEXT("cpp."));
        }

        return Prefixes;
    }
}
```

**In `SendMessageAutonomous()`**, after `SetupAutonomousSandbox()` (line 333) and before `LaunchCLIProcess` (line 405), add:

```cpp
// Set tool filter based on message content (autonomous mode only)
TSet<FString> ToolPrefixes = DetermineToolPrefixes(LastRunContext.OriginalMessage);
if (ToolPrefixes.Num() > 0)
{
    FOliveMCPServer::Get().SetToolFilter(ToolPrefixes);
}
// else: empty set = no filter, show all tools
```

**In `HandleResponseCompleteAutonomous()`**, after the MCP delegate cleanup (line 756) and BEFORE auto-continue logic, add:

```cpp
// Clear tool filter (must happen before potential auto-continue)
FOliveMCPServer::Get().ClearToolFilter();
```

**IMPORTANT:** The auto-continue logic (Fix 2) will call `SendMessageAutonomous()` again, which will re-set the filter based on `LastRunContext.OriginalMessage`. Since we store the original user message (not the continuation prompt) in `OriginalMessage`, the same filter is applied consistently across continuations.

### Task 3D: Clear Filter on Cancel

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Private\Providers\OliveCLIProviderBase.cpp`

In `CancelRequest()`, add `FOliveMCPServer::Get().ClearToolFilter();` alongside the existing cleanup.

### Edge Cases

1. **AI needs a hidden tool:** `HandleToolsCall` accepts any registered tool. The AI would need to know the tool name from context (e.g., a recipe mentions a PCG tool). This is extremely unlikely but handled gracefully.
2. **Continuation runs:** Auto-continue re-sets the filter based on the original message. Same domain, same filter.
3. **Non-autonomous mode:** `SendMessage()` (orchestrated path) never sets a filter. The filter is never set for API providers.
4. **Thread safety:** `ToolFilterLock` protects concurrent access. Set on game thread, read on HTTP thread.

---

## Fix 4: Component Event + Resolver Bugs

### Problem A: Component Bound Events in Resolver

The plan resolver's `ResolveEventOp` only handles native events (BeginPlay, Tick, etc.) via the `EventNameMap`. Component delegate events like `OnComponentBeginOverlap` are NOT in the `EventNameMap` -- they pass through as-is to `NodeFactory::CreateEventNode`, which handles them correctly (lines 357-434). So the resolver does not block them, but it also does not validate them.

**After investigation: Component bound events already work end-to-end.** The resolver passes the event name through to NodeFactory, which does the SCS scan and creates `UK2Node_ComponentBoundEvent`. The `cli_blueprint.txt` already documents this (line 48-49). The `AGENTS.md` already documents this (line 86).

**What IS broken:** The resolver does NOT pass through the `component_name` property from the plan step to the NodeFactory properties. If the plan step has `"properties": {"component_name": "CollisionSphere"}`, the resolver's `MergeStepProperties` at line 1062 would pick it up -- but only if `ResolveEventOp` returns the right properties.

Let me verify this more carefully.

### Task 4A: Verify Component Event Property Propagation

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Blueprint\Private\Plan\OliveBlueprintPlanResolver.cpp`

Check `ResolveEventOp` (line 1541). It sets `Out.Properties.Add(TEXT("event_name"), ResolvedEventName)` at line 1594. Then back in `ResolveStep` at line 1062, `MergeStepProperties(Step, OutResolved.Properties)` merges any additional `properties` from the plan step.

**The flow is:**
1. `ResolveEventOp` sets `event_name` in `Out.Properties`
2. `MergeStepProperties` adds plan step's `properties` (including `component_name` if present)
3. `NodeFactory::CreateEventNode` checks `Properties.Find(TEXT("component_name"))` at line 368

**This should work.** But verify that `MergeStepProperties` does not overwrite `event_name` if the step also has an `event_name` in properties. Read the MergeStepProperties implementation.

**Action:** Coder should verify `MergeStepProperties` preserves the resolver-set `event_name` and adds `component_name` from step properties. If there is a conflict (step properties has `event_name` that differs from the resolved one), the resolver's value should win.

### Problem B: SetCollisionEnabled Class Resolution

The resolver found `SetCollisionEnabled` on `StaticMeshComponent` instead of the parent `UPrimitiveComponent`. This is actually correct behavior -- `SetCollisionEnabled` IS defined on `UPrimitiveComponent`, and `StaticMeshComponent` inherits from `UPrimitiveComponent`.

**After investigation:** The `GetSearchOrder` (line 181) already includes SCS component classes (step 4, line 230-241). If the BP has a `StaticMeshComponent` in SCS, it adds `UStaticMeshComponent` to the search order. `TryExactMatch` at line 274 calls `Class->FindFunctionByName()`, which searches the class AND its parent hierarchy. So `SetCollisionEnabled` on a `UStaticMeshComponent` search WILL find the function on `UPrimitiveComponent`.

**This is not actually a bug.** The function is found correctly; it just reports as `ComponentClassSearch` match method instead of `ParentClassSearch`. The match is valid. The AI may have been confused by the match method string in the result, not by an actual failure.

**However**, there is a potential issue: if the target_class is explicitly set to `StaticMeshComponent` in the plan step and the function is NOT found directly on that class, the resolver may fail before reaching the parent hierarchy scan. Let me check.

Actually, `FindFunctionByName` searches the entire class hierarchy by default in UE. So `UStaticMeshComponent::FindFunctionByName("SetCollisionEnabled")` will find it on `UPrimitiveComponent`. This is correct.

### Task 4B: No Code Changes Needed for Resolver

The resolver and node factory already handle component bound events correctly. The property propagation path (ResolveEventOp -> MergeStepProperties -> CreateEventNode) already works.

**However**, we should add component delegate event names to the resolver documentation/suggestions so the AI gets better error messages if something goes wrong. Add these names to the `ResolveEventOp` suggestions:

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Blueprint\Private\Plan\OliveBlueprintPlanResolver.cpp`

At line 1559, the error suggestion says:
```
"Set 'target' to the event name (e.g., \"BeginPlay\", \"Tick\", \"ActorBeginOverlap\")"
```

**Improve to:**
```
"Set 'target' to the event name (e.g., \"BeginPlay\", \"Tick\", \"ActorBeginOverlap\", \"OnComponentBeginOverlap\", \"OnComponentHit\")"
```

This is a minor improvement to error guidance.

### Task 4C: Add Resolver Note for Component Events

When a component delegate event passes through `ResolveEventOp` without being in the `EventNameMap`, it would be helpful to add a resolver note indicating it will be treated as a component delegate event.

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Blueprint\Private\Plan\OliveBlueprintPlanResolver.cpp`

After line 1594 (`Out.Properties.Add(TEXT("event_name"), ResolvedEventName)`), add:

```cpp
// If the event name was NOT in the EventNameMap and starts with "On" or "OnComponent",
// it is likely a component delegate event. Add a resolver note for transparency.
if (!EventNameMap.Contains(Step.Target) && !Step.Target.StartsWith(TEXT("Receive")))
{
    FOliveResolverNote Note;
    Note.Field = TEXT("event_name");
    Note.OriginalValue = Step.Target;
    Note.ResolvedValue = ResolvedEventName;
    Note.Reason = TEXT("Not a native event override. Will be resolved as a component delegate event by NodeFactory.");
    Out.ResolverNotes.Add(MoveTemp(Note));
}
```

### Quality Impact

**Quality-positive.** Better error messages and transparency notes help the AI self-correct faster. No functional changes to the working path.

---

## Fix 5: Auto-Read Mode for Small Blueprints

### Problem

For small blueprints (under ~50 nodes), `blueprint.read` returns a summary requiring follow-up `read_function`/`read_event_graph` calls. This wastes 3-5 round-trips.

### Solution

When mode is "summary" (the default) and the Blueprint has fewer than a configurable threshold of total nodes, automatically return full graph data inline.

### Quality Impact

**Quality-positive.** Same data, fewer calls. The AI gets full context immediately, leading to better first-attempt plans.

### Task 5A: Add Auto-Full-Read Logic

**File:** `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Blueprint\Private\MCP\OliveBlueprintToolHandlers.cpp`

Modify `HandleBlueprintRead()` (line 990). After the mode validation (line 1028) and asset resolution (line 1041), replace the read logic at lines 1043-1054:

**Current:**
```cpp
// Read Blueprint
FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
TOptional<FOliveIRBlueprint> Result;

if (Mode == TEXT("full"))
{
    Result = Reader.ReadBlueprintFull(ResolvedPath);
}
else
{
    Result = Reader.ReadBlueprintSummary(ResolvedPath);
}
```

**Replace with:**
```cpp
// Read Blueprint
FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
TOptional<FOliveIRBlueprint> Result;

if (Mode == TEXT("full"))
{
    Result = Reader.ReadBlueprintFull(ResolvedPath);
}
else
{
    // Auto-full-read: if the Blueprint is small, return full data automatically
    // to save the AI from needing follow-up read_function/read_event_graph calls.
    // First do a summary read to check size.
    Result = Reader.ReadBlueprintSummary(ResolvedPath);

    if (Result.IsSet())
    {
        // Count total nodes across all graphs to estimate Blueprint complexity.
        // Summary includes graph names with node counts. Check if small enough
        // for auto-upgrade to full read.
        int32 TotalNodeCount = 0;
        for (const FOliveIRGraph& Graph : Result->EventGraphs)
        {
            TotalNodeCount += Graph.Nodes.Num();
        }
        for (const FOliveIRGraph& Graph : Result->FunctionGraphs)
        {
            TotalNodeCount += Graph.Nodes.Num();
        }
        for (const FOliveIRGraph& Graph : Result->MacroGraphs)
        {
            TotalNodeCount += Graph.Nodes.Num();
        }

        // Summary mode returns graph metadata (names, node counts) but NOT
        // full node data (Nodes array is empty in summary). Check the graph
        // count and function count to estimate complexity.
        const int32 TotalGraphCount = Result->EventGraphs.Num()
            + Result->FunctionGraphs.Num()
            + Result->MacroGraphs.Num();

        // Heuristic: small Blueprint = few graphs and few variables.
        // The threshold is conservative: under 5 graphs means at most ~50 nodes
        // in a typical gameplay Blueprint.
        static constexpr int32 AUTO_FULL_READ_MAX_GRAPHS = 5;

        if (TotalGraphCount <= AUTO_FULL_READ_MAX_GRAPHS)
        {
            // Re-read as full to include all graph data
            TOptional<FOliveIRBlueprint> FullResult = Reader.ReadBlueprintFull(ResolvedPath);
            if (FullResult.IsSet())
            {
                Result = MoveTemp(FullResult);

                UE_LOG(LogOliveBPTools, Log,
                    TEXT("HandleBlueprintRead: auto-upgraded to full read (%d graphs)"),
                    TotalGraphCount);
            }
        }
    }
}
```

**IMPORTANT NOTE:** The summary read returns graph metadata (names, node counts in the graph summary) but NOT full node data (the `Nodes` array is empty in summary mode). We use graph count as a proxy because it is available in summary mode. An alternative is to count nodes directly from the UBlueprint's graphs, but that requires loading the Blueprint separately from the reader. Using graph count is simpler and avoids double-loading.

**Better approach: Count nodes directly.** Since we have the resolved path, load the Blueprint and count nodes directly:

```cpp
else
{
    // Auto-full-read for small Blueprints: count total nodes first.
    // If small enough, return full data to save follow-up read calls.
    static constexpr int32 AUTO_FULL_READ_NODE_THRESHOLD = 50;

    UBlueprint* Blueprint = Cast<UBlueprint>(
        StaticLoadObject(UBlueprint::StaticClass(), nullptr, *ResolvedPath));

    bool bAutoFull = false;
    if (Blueprint)
    {
        int32 TotalNodeCount = 0;
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (Graph) TotalNodeCount += Graph->Nodes.Num();
        }
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph) TotalNodeCount += Graph->Nodes.Num();
        }
        for (UEdGraph* Graph : Blueprint->MacroGraphs)
        {
            if (Graph) TotalNodeCount += Graph->Nodes.Num();
        }

        if (TotalNodeCount <= AUTO_FULL_READ_NODE_THRESHOLD)
        {
            bAutoFull = true;
            UE_LOG(LogOliveBPTools, Log,
                TEXT("HandleBlueprintRead: auto-upgrade to full read (%d nodes <= %d threshold)"),
                TotalNodeCount, AUTO_FULL_READ_NODE_THRESHOLD);
        }
    }

    if (bAutoFull)
    {
        Result = Reader.ReadBlueprintFull(ResolvedPath);
    }
    else
    {
        Result = Reader.ReadBlueprintSummary(ResolvedPath);
    }
}
```

This avoids double-reading. The `StaticLoadObject` is the same call the Reader uses internally (`LoadBlueprint`), so the asset is already in memory and the cost is negligible.

### Task 5B: Add Auto-Read Indicator to Result

When auto-full-read is triggered, inject a hint into the result JSON so the AI knows it has full data:

After line 1066 (`TSharedPtr<FJsonObject> ResultData = Result->ToJson();`), add:

```cpp
// If we auto-upgraded to full read, mark it in the result
if (Mode == TEXT("summary") && bAutoFull && ResultData.IsValid())
{
    ResultData->SetStringField(TEXT("read_mode"), TEXT("auto_full"));
    ResultData->SetStringField(TEXT("read_mode_note"),
        TEXT("Blueprint is small enough that full graph data was included automatically. "
             "No need to call read_function or read_event_graph."));
}
```

**Note:** The `bAutoFull` variable needs to be visible at this scope. Move it outside the if/else block or restructure slightly.

### Edge Cases

1. **Large Blueprints:** Threshold of 50 nodes prevents auto-full-read on complex BPs. The summary + follow-up pattern is still used.
2. **Empty Blueprints (just created):** 0 nodes < 50, so auto-full-read triggers. This returns full data for newly created BPs, which is helpful (the AI sees it is empty and can proceed).
3. **Blueprint with many graphs but few nodes each:** If 10 graphs x 3 nodes = 30 total, auto-full-read triggers. This is correct -- 30 nodes is still small.

---

## Implementation Order

### Parallelization

Fixes 1, 3, 4, and 5 are completely independent and can be implemented in parallel by separate coders.

Fix 2 depends on Fix 1 only in that both modify `OliveCLIProviderBase.cpp` -- but they touch different sections (Fix 1 modifies `SetupAutonomousSandbox` around line 269, Fix 2 modifies `HandleResponseCompleteAutonomous` around line 762 and `SendMessageAutonomous` around line 304). They can be implemented in parallel if both coders are aware of the merge points.

### Recommended Order (Single Coder)

1. **Fix 1 (Slim Templates)** -- 30 minutes. JSON edits + prompt text updates. Immediate impact, zero code risk.
2. **Fix 2 (Auto-Continue)** -- 1.5 hours. Most impactful for multi-asset tasks. Medium complexity.
3. **Fix 3 (Tool Filter)** -- 1.5 hours. MCP server + CLI provider changes. Medium complexity.
4. **Fix 5 (Auto-Read)** -- 45 minutes. Single handler modification. Low complexity.
5. **Fix 4 (Resolver Notes)** -- 30 minutes. Minor improvement. Low complexity.

**Total estimated time:** ~4.5 hours

### Build Verification Points

1. After Fix 1: Build + verify templates load (check `LogOliveTemplates` for parse errors)
2. After Fix 2: Build + test idle timeout fires at 50s + auto-continue launches
3. After Fix 3: Build + test `tools/list` returns filtered count + `tools/call` still works for hidden tools
4. After Fix 5: Build + test `blueprint.read` on a small BP returns full data
5. After Fix 4: Build only (no behavioral change, just better error messages)

---

## Files Modified Summary

| Fix | File | Change Type |
|-----|------|-------------|
| 1 | `Content/Templates/factory/gun.json` | Remove `plan` keys from 3 functions |
| 1 | `Content/Templates/factory/stat_component.json` | Remove `plan` key from 1 function |
| 1 | `Content/Templates/factory/projectile.json` | Verify no plans (no change) |
| 1 | `AGENTS.md` | Update template guidance text |
| 1 | `Content/SystemPrompts/Knowledge/recipe_routing.txt` | Update routing rule |
| 1 | `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | Update template section |
| 1 | `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | Update sandbox CLAUDE.md text |
| 2 | `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h` | Add `AutoContinueCount`, `MaxAutoContinues` |
| 2 | `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | Reduce timeout, add auto-continue logic |
| 3 | `Source/OliveAIEditor/Public/MCP/OliveMCPServer.h` | Add `SetToolFilter`/`ClearToolFilter` API |
| 3 | `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp` | Implement filter in `HandleToolsList` |
| 3 | `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | Set/clear filter around autonomous launch |
| 4 | `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | Better error suggestion + resolver note |
| 5 | `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Auto-full-read for small BPs |

---

## New Error Codes

None. All changes use existing error codes or are non-error changes.

## New Settings

None. Thresholds are compile-time constants:
- `CLI_IDLE_TIMEOUT_SECONDS = 50.0` (was 120.0)
- `MaxAutoContinues = 3`
- `AUTO_FULL_READ_NODE_THRESHOLD = 50`

---

## Risk Assessment

| Fix | Risk | Mitigation |
|-----|------|-----------|
| 1 | AI writes worse plans than templates had | Recipes provide same patterns. AI proved capable in no-template test. |
| 2 | Auto-continue loops indefinitely | Hard cap at 3. Runtime limit timeout skipped. Cancel respected. |
| 3 | AI needs a filtered-out tool | `HandleToolsCall` accepts any tool. Filtering is discovrability only. |
| 4 | None | Pure improvement to error messages |
| 5 | Double Blueprint load on summary read | `StaticLoadObject` caches. Reader uses same call internally. Negligible cost. |
