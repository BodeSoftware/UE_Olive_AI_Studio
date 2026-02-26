# Autonomous Efficiency Round 3 -- Design Plan

**Author:** Architect Agent
**Date:** 2026-02-26
**Priority:** High
**Estimated effort:** ~5 hours across 3 fixes

---

## Problem Summary

A real autonomous test run revealed three distinct failure modes:

1. **Stall on multi-asset tasks** (Run 2 failure): The AI scaffolded all structures across 3 Blueprints (add_component, add_variable, add_function), then went completely silent for 120 seconds until the idle timeout killed the process. Root cause: the sandbox CLAUDE.md line "Create ALL asset structures first, then wire graph logic for each" tells the AI to defer ALL graph wiring until after ALL scaffolding across ALL assets. For complex tasks this creates an impossible planning cliff -- the AI accumulates huge context and freezes.

2. **"Continue" is stateless** (Run 3 failure): Each autonomous launch creates a fresh Claude Code process. When the user says "continue", the new process knows nothing about the previous run. It discovers 104 tools and immediately exits without doing anything.

3. **Post-template read flood** (Run 1 inefficiency): After `create_from_template`, the AI made 7 read calls to inspect what was just created. The template result already lists components/variables/functions but does not describe the graph logic within functions, so the AI reads each function to understand what the template built.

---

## Fix 1: Anti-Stall Interleave Guidance

### Root Cause

Three prompt sources contain the problematic "scaffold everything first" directive:

| Location | Current text |
|----------|-------------|
| `OliveCLIProviderBase.cpp` line ~265 | (not present -- but AGENTS.md is appended which contains it) |
| `AGENTS.md` line 26 | `**Multi-asset (2+ Blueprints):** Create ALL asset structures first, then wire graph logic for each.` |
| `Content/SystemPrompts/Knowledge/recipe_routing.txt` line 6 | `MULTI-ASSET (2+ blueprints): create ALL structures first -> wire each graph -> budget iterations per asset` |
| `Content/SystemPrompts/Knowledge/cli_blueprint.txt` lines 16-20 | Multi-asset workflow section |

The sandbox `CLAUDE.md` (written by `SetupAutonomousSandbox`) includes AGENTS.md content verbatim, and also has its own Critical Rules section. Both must change.

### Design

Replace the "scaffold everything then wire" approach with an **interleaved** approach: complete one asset at a time. Within each asset, scaffold all structures (components, variables, functions), then wire all functions, then compile once.

**Key rule:** "After adding a function to a Blueprint, write plan_json for that function before creating the next function. Compile once per asset after all its functions are wired, not after each one."

### Changes

#### File 1: `AGENTS.md` (line 26)

Replace:
```
**Multi-asset (2+ Blueprints):** Create ALL asset structures first, then wire graph logic for each.
```

With:
```
**Multi-asset (2+ Blueprints):** Complete ONE asset at a time. For each asset: create structure (components, variables, functions), wire all plan_json for its functions, then compile. Move to the next asset only after the current one compiles clean.
```

#### File 2: `Content/SystemPrompts/Knowledge/recipe_routing.txt` (line 6)

Replace:
```
- MULTI-ASSET (2+ blueprints): create ALL structures first -> wire each graph -> budget iterations per asset
```

With:
```
- MULTI-ASSET (2+ blueprints): complete ONE asset at a time. Structure -> plan_json for all functions -> compile. Then next asset. Never scaffold everything across all assets before wiring any logic.
```

#### File 3: `Content/SystemPrompts/Knowledge/cli_blueprint.txt` (lines 16-20)

Replace:
```
MULTI-ASSET (2+ Blueprints needed, e.g. "gun and bullet"):
1. Create ALL asset structures first (blueprint.create + add_component + add_variable for EVERY asset)
2. Wire graph logic for each asset AFTER all structures exist
3. Budget: with 10 iterations and 2 assets, spend max 5 per asset
4. NEVER spend all iterations on one asset when multiple are needed
```

With:
```
MULTI-ASSET (2+ Blueprints needed, e.g. "gun and bullet"):
1. Complete ONE asset fully before starting the next:
   a. Create structure: blueprint.create (or create_from_template) + add_component + add_variable + add_function
   b. Wire ALL functions: get_recipe + preview + apply plan_json for each function
   c. Compile once after all functions are wired
   d. Fix any compile errors before moving on
2. Then create and wire the next asset the same way.
3. Why: deferring all wiring until after all scaffolding creates an impossible planning cliff and causes stalls.
```

#### File 4: `OliveCLIProviderBase.cpp` -- `SetupAutonomousSandbox()` (Critical Rules section, around line 267)

The sandbox CLAUDE.md already contains a Critical Rules section (lines 260-272 in the method). We need to add the interleave rule here explicitly, because the model sees this section first and it takes priority.

After the existing line:
```cpp
ClaudeMd += TEXT("- After `create_from_template`, always `blueprint.read` the result before modifying it.\n");
```

Add a new interleave rule:
```cpp
ClaudeMd += TEXT("- Multi-asset tasks: complete ONE Blueprint fully (structure + plan_json + compile) before starting the next. Do NOT scaffold all assets first then wire later -- this causes stalls.\n");
```

Also, after the existing line:
```cpp
ClaudeMd += TEXT("- If `apply_plan_json` fails, re-read the graph, fix the plan, and retry once. Fall back to add_node/connect_pins only after a second failure.\n\n");
```

Add function-level interleave guidance:
```cpp
ClaudeMd += TEXT("- After adding a function to a Blueprint, write plan_json for that function before creating the next function or asset. Compile once per asset after all its functions are wired.\n");
```

### Risks

- The interleaved approach may cause issues when Blueprint A's plan references Blueprint B (e.g., `spawn_actor` with B's class). Mitigation: the resolver's `ExpandPlanInputs` already handles cross-asset references by class name, and the AI typically creates the spawned class first. The new guidance says "complete ONE asset fully" which naturally leads to creating the dependency first.
- If the user's task involves tightly coupled assets (A calls a function on B that doesn't exist yet), the AI may need to deviate. This is acceptable -- the guidance says "complete ONE asset at a time" as a default, not an absolute rule.

---

## Fix 2: "Continue" Context Injection

### Root Cause

`SendMessageAutonomous()` launches a fresh CLI process each time. Each process reads the sandbox CLAUDE.md (static, written fresh each launch) and discovers tools via MCP. It has zero knowledge of previous runs.

When the user says "continue", the process sees a one-word message with no context about what was previously done. It discovers 104 tools and exits because it has nothing concrete to act on.

### Design

Track what happened during each autonomous run and inject that context into the prompt when the user sends a continuation message.

#### Data Structure

New struct on `FOliveCLIProviderBase`:

```cpp
/** Context from the most recent autonomous run, used to enrich "continue" messages. */
struct FAutonomousRunContext
{
    /** The original user message that started the run */
    FString OriginalMessage;

    /** Asset paths that were modified during the run (extracted from MCP tool calls) */
    TArray<FString> ModifiedAssetPaths;

    /** Tool call log: tool name + asset path (if any) + success/fail. Capped at 50 entries. */
    struct FToolCallEntry
    {
        FString ToolName;
        FString AssetPath;
        bool bSuccess = true;
    };
    TArray<FToolCallEntry> ToolCallLog;

    /** Run outcome */
    enum class EOutcome : uint8
    {
        Completed,   // Process exited normally
        IdleTimeout, // Killed by idle timeout
        RuntimeLimit // Killed by runtime limit
    };
    EOutcome Outcome = EOutcome::Completed;

    /** Whether this context is valid (a run has completed) */
    bool bValid = false;

    /** Reset to empty state */
    void Reset()
    {
        OriginalMessage.Empty();
        ModifiedAssetPaths.Empty();
        ToolCallLog.Empty();
        Outcome = EOutcome::Completed;
        bValid = false;
    }
};
```

Add as a member on `FOliveCLIProviderBase`:
```cpp
FAutonomousRunContext LastRunContext;
```

#### Tracking Tool Calls (Where Data Comes From)

The `OnToolCalled` delegate currently provides only `(ToolName, ClientId)`. This is insufficient -- we need the asset path from the tool call arguments.

**Option A (preferred): Extend the delegate.** Change `FOnMCPToolCalled` to pass a third parameter: the arguments JSON object.

```cpp
// OliveMCPServer.h
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnMCPToolCalled, const FString& /* ToolName */, const FString& /* ClientId */, const TSharedPtr<FJsonObject>& /* Arguments */);
```

Update both broadcast sites in `OliveMCPServer.cpp` (HandleToolsCall at line 587 and HandleToolsCallAsync at line 693):
```cpp
OnToolCalled.Broadcast(ToolName, ClientId, Arguments);
```

Update the subscriber in `OliveCLIProviderBase.cpp` `SendMessageAutonomous()` (around line 349):
```cpp
ToolCallDelegateHandle = FOliveMCPServer::Get().OnToolCalled.AddLambda(
    [this, Guard](const FString& ToolName, const FString& ClientId, const TSharedPtr<FJsonObject>& Arguments)
    {
        if (*Guard)
        {
            LastToolCallTimestamp.store(FPlatformTime::Seconds());

            // Track tool call for continuation context
            FAutonomousRunContext::FToolCallEntry Entry;
            Entry.ToolName = ToolName;
            // Extract asset_path from arguments (most tools have this)
            if (Arguments.IsValid())
            {
                FString AssetPath;
                if (Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
                    Arguments->TryGetStringField(TEXT("path"), AssetPath))
                {
                    Entry.AssetPath = AssetPath;
                    if (!LastRunContext.ModifiedAssetPaths.Contains(AssetPath))
                    {
                        LastRunContext.ModifiedAssetPaths.Add(AssetPath);
                    }
                }
            }
            // bSuccess is set to true; we don't know the result at broadcast time
            // (broadcast fires before execution). This is fine -- we only need the
            // asset list and tool call sequence, not per-call success.
            if (LastRunContext.ToolCallLog.Num() < 50)
            {
                LastRunContext.ToolCallLog.Add(MoveTemp(Entry));
            }
        }
    });
```

**IMPORTANT**: The `OnToolCalled.Broadcast` fires BEFORE tool execution (it's at the top of HandleToolsCallAsync/HandleToolsCall, before the game-thread dispatch). So `bSuccess` is always true here. This is acceptable -- we care about which assets were touched and what tools were called, not individual success/failure. The overall run outcome (timeout/completion) captures whether the run succeeded.

#### Storing Context on Run Completion

In `SendMessageAutonomous()`, before launching:
```cpp
// Initialize run context tracking
LastRunContext.Reset();
LastRunContext.OriginalMessage = UserMessage;
LastRunContext.bValid = false; // Set true on completion
```

In `HandleResponseCompleteAutonomous()`, before the completion callback:
```cpp
// Capture run context for potential continuation
LastRunContext.bValid = true;
if (bStopReading)
{
    // Process was terminated (timeout)
    const double LastTool = LastToolCallTimestamp.load();
    const double IdleToolTimeout = RuntimeSettings ?
        static_cast<double>(RuntimeSettings->AutonomousIdleToolSeconds) : 120.0;
    if (LastTool > 0.0 && (FPlatformTime::Seconds() - LastTool) > IdleToolTimeout)
    {
        LastRunContext.Outcome = FAutonomousRunContext::EOutcome::IdleTimeout;
    }
    else
    {
        LastRunContext.Outcome = FAutonomousRunContext::EOutcome::RuntimeLimit;
    }
}
else
{
    LastRunContext.Outcome = FAutonomousRunContext::EOutcome::Completed;
}
```

Wait -- `HandleResponseCompleteAutonomous` runs under `CallbackLock` on the game thread, but the `bStopReading` flag was set on the background thread. We need a different approach.

Better: Pass the timeout reason through. Add a member:
```cpp
/** Whether the last process termination was due to timeout (vs normal exit) */
bool bLastRunTimedOut = false;
```

Set `bLastRunTimedOut = true` in the idle-timeout and runtime-limit blocks of `LaunchCLIProcess`, and `bLastRunTimedOut = false` at the start of `SendMessageAutonomous`.

Then in `HandleResponseCompleteAutonomous`:
```cpp
LastRunContext.bValid = true;
if (bLastRunTimedOut)
{
    LastRunContext.Outcome = FAutonomousRunContext::EOutcome::IdleTimeout;
}
// else stays Completed (the default)
```

#### Detecting Continuation Messages

Add a helper method on `FOliveCLIProviderBase`:

```cpp
/** Check if a user message looks like a continuation request */
bool IsContinuationMessage(const FString& Message) const
{
    FString Lower = Message.ToLower().TrimStartAndEnd();
    // Common continuation phrases
    return Lower == TEXT("continue") ||
           Lower == TEXT("keep going") ||
           Lower == TEXT("finish") ||
           Lower == TEXT("finish the task") ||
           Lower == TEXT("keep working") ||
           Lower == TEXT("resume") ||
           Lower.StartsWith(TEXT("continue ")) ||
           Lower.StartsWith(TEXT("keep going")) ||
           Lower.StartsWith(TEXT("finish "));
}
```

#### Building Enriched Prompt

Add a helper method on `FOliveCLIProviderBase`:

```cpp
/**
 * Build an enriched prompt for a continuation message by injecting context
 * from the previous autonomous run.
 *
 * @param UserMessage The user's continuation message (e.g., "continue")
 * @return Enriched prompt with previous run context
 */
FString BuildContinuationPrompt(const FString& UserMessage) const
{
    FString Prompt;

    // Header
    Prompt += TEXT("## Continuation of Previous Task\n\n");

    // Original task
    Prompt += TEXT("### Original Task\n");
    Prompt += LastRunContext.OriginalMessage;
    Prompt += TEXT("\n\n");

    // What was done
    Prompt += TEXT("### What Was Already Done\n");
    if (LastRunContext.ToolCallLog.Num() > 0)
    {
        // Group by asset for readability
        TMap<FString, TArray<FString>> ByAsset;
        for (const auto& Entry : LastRunContext.ToolCallLog)
        {
            FString Key = Entry.AssetPath.IsEmpty() ? TEXT("(general)") : Entry.AssetPath;
            ByAsset.FindOrAdd(Key).Add(Entry.ToolName);
        }

        for (const auto& Pair : ByAsset)
        {
            Prompt += FString::Printf(TEXT("- %s: "), *Pair.Key);
            // Deduplicate consecutive identical tool names for brevity
            TArray<FString> Condensed;
            FString LastTool;
            int32 Count = 0;
            for (const FString& Tool : Pair.Value)
            {
                if (Tool == LastTool)
                {
                    Count++;
                }
                else
                {
                    if (!LastTool.IsEmpty())
                    {
                        Condensed.Add(Count > 1 ?
                            FString::Printf(TEXT("%s x%d"), *LastTool, Count) : LastTool);
                    }
                    LastTool = Tool;
                    Count = 1;
                }
            }
            if (!LastTool.IsEmpty())
            {
                Condensed.Add(Count > 1 ?
                    FString::Printf(TEXT("%s x%d"), *LastTool, Count) : LastTool);
            }
            Prompt += FString::Join(Condensed, TEXT(", "));
            Prompt += TEXT("\n");
        }
    }
    else
    {
        Prompt += TEXT("No tool calls were recorded from the previous run.\n");
    }

    // Run outcome
    Prompt += TEXT("\n### Previous Run Outcome\n");
    switch (LastRunContext.Outcome)
    {
    case FAutonomousRunContext::EOutcome::IdleTimeout:
        Prompt += TEXT("The previous run STALLED (idle timeout -- no tool calls for 120 seconds). ");
        Prompt += TEXT("The task is incomplete. You need to continue from where it stopped.\n");
        break;
    case FAutonomousRunContext::EOutcome::RuntimeLimit:
        Prompt += TEXT("The previous run hit the runtime limit. The task is incomplete.\n");
        break;
    case FAutonomousRunContext::EOutcome::Completed:
        Prompt += TEXT("The previous run completed normally. ");
        Prompt += TEXT("The user wants you to continue or finish remaining work.\n");
        break;
    }

    // Action directive
    Prompt += TEXT("\n### Your Task Now\n");
    Prompt += TEXT("1. Read each modified asset listed above with `blueprint.read` to understand current state.\n");
    Prompt += TEXT("2. Determine what still needs to be done to complete the original task.\n");
    Prompt += TEXT("3. Complete the remaining work. Focus on functions that are missing graph logic (plan_json) and fixing any compile errors.\n");

    // Include the user's continuation message if it has additional context
    FString Trimmed = UserMessage.TrimStartAndEnd();
    if (!Trimmed.Equals(TEXT("continue"), ESearchCase::IgnoreCase) &&
        !Trimmed.Equals(TEXT("keep going"), ESearchCase::IgnoreCase) &&
        !Trimmed.Equals(TEXT("finish"), ESearchCase::IgnoreCase) &&
        !Trimmed.Equals(TEXT("resume"), ESearchCase::IgnoreCase))
    {
        Prompt += TEXT("\n### Additional Instructions\n");
        Prompt += Trimmed;
        Prompt += TEXT("\n");
    }

    return Prompt;
}
```

#### Integration Point

In `SendMessageAutonomous()`, after `SetupAutonomousSandbox()` and before `LaunchCLIProcess`, check for continuation:

```cpp
// Enrich continuation messages with context from the previous run
FString EffectiveMessage = UserMessage;
if (LastRunContext.bValid && IsContinuationMessage(UserMessage))
{
    EffectiveMessage = BuildContinuationPrompt(UserMessage);
    UE_LOG(LogOliveCLIProvider, Log,
        TEXT("Continuation detected: enriched prompt with %d modified assets from previous run"),
        LastRunContext.ModifiedAssetPaths.Num());
}

// Initialize run context tracking for this new run
LastRunContext.Reset();
LastRunContext.OriginalMessage = UserMessage;
```

Then pass `EffectiveMessage` (not `UserMessage`) to `LaunchCLIProcess`.

### File Changes Summary

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h` | Add `FAutonomousRunContext` struct, `LastRunContext` member, `bLastRunTimedOut` member, `IsContinuationMessage()`, `BuildContinuationPrompt()` |
| `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | Track tool calls in OnToolCalled lambda, store context in HandleResponseCompleteAutonomous, build enriched prompt in SendMessageAutonomous |
| `Source/OliveAIEditor/Public/MCP/OliveMCPServer.h` | Change `FOnMCPToolCalled` delegate to 3 params (add `TSharedPtr<FJsonObject>` Arguments) |
| `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp` | Update both `OnToolCalled.Broadcast` calls to pass Arguments |

### Risks

- **Delegate signature change is breaking for all subscribers.** Currently there are exactly two subscribers: (1) the `CLIProviderBase` lambda in `SendMessageAutonomous`, (2) nothing else in the codebase (verified via grep for `OnToolCalled`). So the blast radius is contained.
- **The `OnToolCalled` broadcast fires before execution**, so we cannot capture success/failure of individual tool calls. This is acceptable -- we need the asset list and tool sequence, not per-call outcomes.
- **`BuildContinuationPrompt` accesses `LastRunContext` which is a member on the provider.** The provider instance persists across runs (it is owned by `ConversationManager`). If the user switches providers between runs, `LastRunContext` is lost because a new provider instance is created. This is acceptable -- switching providers between "continue" messages is an unlikely edge case.
- **Tool call log capped at 50 entries.** A typical gun+bullet run uses ~35 tool calls. 50 is generous without being unbounded.

### Edge Cases

1. **User sends "continue" but no previous run exists**: `LastRunContext.bValid == false`, skip enrichment, pass the raw message. The AI gets a bare "continue" which is unhelpful but not harmful.
2. **User sends "continue building the gun and also add a sword"**: The continuation detector matches "continue " prefix, so the enriched prompt is built. The user's full message is included under "Additional Instructions".
3. **Previous run completed successfully**: The enriched prompt says "completed normally" and the AI can read assets and verify everything is done, or add the user's new request on top.

---

## Fix 3: Template Read Reduction

### Root Cause

The `create_from_template` result (from `FOliveTemplateSystem::ApplyTemplate`) returns:
```json
{
  "asset_path": "/Game/BP_Gun",
  "template_id": "gun",
  "compiled": true,
  "components": ["GunMesh", "MuzzlePoint"],
  "variables": ["Ammo", "MaxAmmo", "FireRate", "Damage", "MuzzleVelocity", "bCanFire"],
  "functions": ["Fire", "Reload", "StartFiring", "StopFiring"],
  "event_dispatchers": ["OnFired", "OnReloaded", "OnAmmoChanged"],
  "applied_params": { ... }
}
```

This tells the AI WHAT was created but not WHAT LOGIC is inside each function. The AI has no way to know that `Fire` already contains 8 nodes including `SpawnActor` and `SetTimerByFunctionName` without reading the function graph. So it calls `blueprint.read_function` 4 times + `blueprint.read_event_graph` + `blueprint.read`.

### Design

Enrich the `ApplyTemplate` result with a per-function graph summary that describes the plan steps that were executed. This way the AI knows which functions already have logic and what that logic does, eliminating the need for post-template reads.

#### Changes to `OliveTemplateSystem.cpp`

After each successful plan execution (both for functions at line ~1262 and event graphs at line ~1366), capture a summary of what the plan built.

Add a new tracking struct (local to the function, not a class member):

```cpp
struct FTemplateFunctionSummary
{
    FString Name;
    int32 NodeCount = 0;
    TArray<FString> StepSummaries; // e.g., "evt: event BeginPlay", "spawn: spawn_actor Actor"
    bool bPlanExecuted = false;
    bool bPlanSucceeded = false;
    TArray<FString> PlanErrors;
};
```

Collect summaries during plan execution:

```cpp
// After successful PlanExecutor.Execute():
FTemplateFunctionSummary FuncSummary;
FuncSummary.Name = FuncName;
FuncSummary.bPlanExecuted = true;
FuncSummary.bPlanSucceeded = PlanResult.bSuccess || PlanResult.bPartial;
FuncSummary.NodeCount = PlanResult.CreatedNodeCount; // Already tracked in PlanResult

// Build step summaries from the plan
for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
{
    FString Summary = FString::Printf(TEXT("%s: %s"), *Step.StepId, *Step.Op);
    if (!Step.Target.IsEmpty())
    {
        Summary += TEXT(" ") + Step.Target;
    }
    FuncSummary.StepSummaries.Add(Summary);
}

// Capture errors
if (!PlanResult.bSuccess)
{
    for (const auto& Err : PlanResult.Errors)
    {
        FuncSummary.PlanErrors.Add(Err.Message);
    }
}

FunctionSummaries.Add(MoveTemp(FuncSummary));
```

Then in the result building section (around line 1447), add function detail:

```cpp
// Function details with graph logic summary
TArray<TSharedPtr<FJsonValue>> FuncDetailsArray;
for (const FTemplateFunctionSummary& FuncSummary : FunctionSummaries)
{
    TSharedPtr<FJsonObject> FuncDetail = MakeShared<FJsonObject>();
    FuncDetail->SetStringField(TEXT("name"), FuncSummary.Name);
    FuncDetail->SetBoolField(TEXT("has_graph_logic"), FuncSummary.bPlanExecuted);

    if (FuncSummary.bPlanExecuted)
    {
        FuncDetail->SetNumberField(TEXT("node_count"), FuncSummary.NodeCount);
        FuncDetail->SetBoolField(TEXT("plan_succeeded"), FuncSummary.bPlanSucceeded);

        // Step summaries (compact, one line per step)
        TArray<TSharedPtr<FJsonValue>> StepJsonArray;
        for (const FString& S : FuncSummary.StepSummaries)
        {
            StepJsonArray.Add(MakeShared<FJsonValueString>(S));
        }
        FuncDetail->SetArrayField(TEXT("plan_steps"), StepJsonArray);

        if (FuncSummary.PlanErrors.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> ErrArray;
            for (const FString& E : FuncSummary.PlanErrors)
            {
                ErrArray.Add(MakeShared<FJsonValueString>(E));
            }
            FuncDetail->SetArrayField(TEXT("plan_errors"), ErrArray);
        }
    }
    else
    {
        // Function was created but has no plan (empty function body -- just entry node)
        FuncDetail->SetStringField(TEXT("note"), TEXT("Empty function body - needs plan_json"));
    }

    FuncDetailsArray.Add(MakeShared<FJsonValueObject>(FuncDetail));
}
ResultData->SetArrayField(TEXT("function_details"), FuncDetailsArray);
```

Do the same for event graph plans, using a separate `EventGraphSummaries` array.

#### What About `PlanResult.CreatedNodeCount`?

Check if `FOliveIRBlueprintPlanResult` already has a node count field.

Looking at the struct: it has `CreatedNodes` (TMap of step_id -> node_id), `Errors`, `Warnings`, `bSuccess`, `bPartial`. The node count can be derived from `CreatedNodes.Num()`.

So use:
```cpp
FuncSummary.NodeCount = PlanResult.CreatedNodes.Num();
```

#### Resulting JSON Shape

After this change, the `create_from_template` result looks like:

```json
{
  "asset_path": "/Game/Blueprints/BP_Gun",
  "template_id": "gun",
  "compiled": true,
  "components": ["GunMesh", "MuzzlePoint"],
  "variables": ["Ammo", "MaxAmmo", "FireRate", ...],
  "functions": ["Fire", "Reload", "StartFiring", "StopFiring"],
  "event_dispatchers": ["OnFired", "OnReloaded", "OnAmmoChanged"],
  "function_details": [
    {
      "name": "Fire",
      "has_graph_logic": true,
      "node_count": 8,
      "plan_succeeded": true,
      "plan_steps": [
        "check_ammo: get_var Ammo",
        "branch: branch",
        "dec: call Decrement_IntInt",
        "set_ammo: set_var Ammo",
        "fire_delegate: call_delegate OnFired",
        "spawn: spawn_actor",
        "timer: call SetTimerByFunctionName"
      ]
    },
    {
      "name": "Reload",
      "has_graph_logic": true,
      "node_count": 4,
      "plan_succeeded": true,
      "plan_steps": ["set_max: get_var MaxAmmo", "set: set_var Ammo", "notify: call_delegate OnReloaded"]
    },
    {
      "name": "StartFiring",
      "has_graph_logic": false,
      "note": "Empty function body - needs plan_json"
    }
  ],
  "event_graph_details": [
    {
      "name": "EventGraph",
      "has_graph_logic": true,
      "node_count": 3,
      "plan_succeeded": true,
      "plan_steps": ["evt: event BeginPlay", "init: call StartFiring"]
    }
  ]
}
```

With this, the AI can see that `Fire` has 8 nodes including spawn_actor and call_delegate, and `StartFiring` is an empty stub that needs logic. No reads necessary.

#### Prompt Reinforcement

Update the sandbox CLAUDE.md rule about reading after template creation.

Current rule (line 271):
```cpp
ClaudeMd += TEXT("- After `create_from_template`, always `blueprint.read` the result before modifying it.\n");
```

Replace with:
```cpp
ClaudeMd += TEXT("- After `create_from_template`, check the `function_details` in the result. Only call `blueprint.read` if you need to modify an existing function that the template already wired (to see its current pin layout). Do NOT read functions just to see what the template created -- the result already tells you.\n");
```

Also update `AGENTS.md` line 17:
```
2. After creation, `blueprint.read` the result to see what the template already built before adding more logic.
```
Replace with:
```
2. After creation, check `function_details` in the result to see which functions have graph logic. Only `blueprint.read` if you need to modify a function the template already wired.
```

### File Changes Summary

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp` | Add `FTemplateFunctionSummary` struct, collect summaries during plan execution, add `function_details` and `event_graph_details` to ResultData JSON |
| `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | Update template read rule in CLAUDE.md |
| `AGENTS.md` | Update post-template instruction |

### Risks

- **Increased response size**: Each function detail adds ~100-200 chars. For a template with 4 functions and 1 event graph, that is ~600-1000 extra chars. Trivial compared to the 7 tool calls it eliminates.
- **`FOliveIRBlueprintPlanResult.CreatedNodes`**: Need to verify this map is populated. If the PlanExecutor does not populate it for template-path execution (vs tool-handler path), the node count would be 0. Verify by reading `OlivePlanExecutor.cpp` -- it populates `CreatedNodes` in `PhaseCreateNodes` unconditionally.

---

## Implementation Order

The three fixes are independent and can be implemented in parallel. However, if done sequentially:

### Phase 1: Fix 1 (Anti-Stall Interleave) -- ~30 minutes
1. Edit `AGENTS.md` (1 line change)
2. Edit `Content/SystemPrompts/Knowledge/recipe_routing.txt` (1 line change)
3. Edit `Content/SystemPrompts/Knowledge/cli_blueprint.txt` (replace 4-line block)
4. Edit `OliveCLIProviderBase.cpp` `SetupAutonomousSandbox()` (add 2 rules in CLAUDE.md generation)

### Phase 2: Fix 3 (Template Read Reduction) -- ~1.5 hours
1. Edit `OliveTemplateSystem.cpp`:
   a. Add `FTemplateFunctionSummary` local struct in `ApplyTemplate`
   b. Collect summaries after function plan execution (~line 1262)
   c. Collect summaries after event graph plan execution (~line 1366)
   d. Also track functions with no plan (created but no `plan` key in JSON)
   e. Add `function_details` and `event_graph_details` arrays to ResultData (~line 1447)
2. Edit `OliveCLIProviderBase.cpp` -- update CLAUDE.md template read rule
3. Edit `AGENTS.md` -- update post-template instruction

### Phase 3: Fix 2 (Continue Context Injection) -- ~3 hours
1. Edit `OliveMCPServer.h` -- change `FOnMCPToolCalled` delegate to 3 params
2. Edit `OliveMCPServer.cpp` -- update both `OnToolCalled.Broadcast` calls to pass `Arguments`
3. Edit `OliveCLIProviderBase.h`:
   a. Add `FAutonomousRunContext` struct
   b. Add `LastRunContext` member
   c. Add `bLastRunTimedOut` member
   d. Add `IsContinuationMessage()` declaration
   e. Add `BuildContinuationPrompt()` declaration
4. Edit `OliveCLIProviderBase.cpp`:
   a. Update `SendMessageAutonomous()` -- initialize LastRunContext, check for continuation, build enriched prompt
   b. Update OnToolCalled lambda -- capture tool name + asset path + log entry
   c. Update idle-timeout / runtime-limit blocks in `LaunchCLIProcess` -- set `bLastRunTimedOut = true`
   d. Reset `bLastRunTimedOut = false` at start of `SendMessageAutonomous`
   e. Update `HandleResponseCompleteAutonomous()` -- finalize LastRunContext with outcome
   f. Add `IsContinuationMessage()` implementation
   g. Add `BuildContinuationPrompt()` implementation

### Verification

After implementation, test with the original scenario:
1. "Create a gun blueprint that shoots bullets" -- should complete without stalling
2. If it stalls, "continue" should provide rich context and allow the AI to finish
3. After `create_from_template`, the AI should NOT make redundant read calls

---

## Files Modified (Complete List)

| File | Fix(es) | Nature |
|------|---------|--------|
| `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h` | 2 | Add struct + members + methods |
| `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | 1, 2, 3 | Prompt changes + tracking + continuation logic |
| `Source/OliveAIEditor/Public/MCP/OliveMCPServer.h` | 2 | Delegate signature change |
| `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp` | 2 | Pass Arguments to broadcast |
| `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp` | 3 | Add function_details to result |
| `AGENTS.md` | 1, 3 | Update multi-asset and post-template text |
| `Content/SystemPrompts/Knowledge/recipe_routing.txt` | 1 | Update multi-asset text |
| `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | 1 | Update multi-asset workflow |
