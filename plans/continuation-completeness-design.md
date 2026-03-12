# Continuation Completeness System Design

**Date:** 2026-03-12
**Author:** Architect Agent
**Status:** Design Complete -- Ready for Implementation

## Problem Statement

In multi-asset autonomous runs, the agent creates all assets (Blueprints, variables, components, function signatures) breadth-first, then stalls for 120+ seconds trying to plan interconnected logic for multiple function graphs simultaneously. The idle timeout kills the process before any `apply_plan_json` calls are submitted. Result: every function graph is an empty shell (just the entry point), the system is completely non-functional.

## Root Cause Analysis

Two factors combine to create this failure:
1. **Flat idle timeout** -- The 120s stdout idle timeout (`CLI_IDLE_TIMEOUT_SECONDS`) fires indiscriminately. When the agent is genuinely thinking (stdout flowing as the LLM generates text), the *tool idle timeout* also uses the same 120s constant (`CLI_IDLE_TIMEOUT_SECONDS` on line 1109) rather than reading `AutonomousIdleToolSeconds` from settings (which defaults to 240s). Both timeouts are too short for complex multi-function planning.
2. **Generic continuation prompt** -- When auto-continue fires, `BuildContinuationPrompt` injects the original task + tool call log + generic "break into smaller steps" message. It does NOT identify which specific functions are empty or provide ordering guidance. The agent often repeats the same breadth-first pattern.

## Changes

---

### Change 1: Progressive Idle Timeout (Nudge at 120s, Kill at 300s)

**Complexity:** COMPLEX (coder)

#### Problem

The current code uses a flat 120s constant for both the stdout idle timeout (line 1083) and the tool-call idle timeout (line 1109). The settings field `AutonomousIdleToolSeconds` (defaulting to 240) exists but is never read. When the agent is actively generating text (stdout flowing) but hasn't called a tool, the tool idle timeout kills it at 120s.

#### Architectural Constraint: Stdin Is Closed

A critical discovery from reading the code: `LaunchCLIProcess` closes the stdin pipe at line 1035 after delivering the initial message. There is no way to inject text into stdin mid-process. The member `StdinWritePipe` is never assigned by `LaunchCLIProcess` (it uses local variables). Therefore, **the "nudge" cannot be a stdin injection**.

#### Design: Two-Tier Kill, Not Stdin Nudge

Since we cannot write to stdin, the "nudge" becomes an early kill-and-relaunch with an enriched continuation prompt. The 300s hard kill is the safety net.

**New constants:**

```cpp
namespace
{
    /** Tier 1: Kill after this many seconds of no tool call and relaunch with
     *  a targeted continuation prompt. The agent gets to keep planning context
     *  from the new prompt (which includes empty-graph analysis). */
    constexpr double CLI_TOOL_IDLE_NUDGE_SECONDS = 120.0;

    /** Tier 2: Hard kill. Safety net for genuinely hung processes.
     *  If the re-launched process also goes idle, this fires. */
    constexpr double CLI_TOOL_IDLE_KILL_SECONDS = 300.0;

    /** Stdout idle timeout. Process produces no stdout at all (not even thinking text).
     *  This catches completely frozen processes (segfault, deadlock). */
    constexpr double CLI_STDOUT_IDLE_SECONDS = 300.0;
}
```

**Timeout logic changes in `LaunchCLIProcess` read loop:**

The tool-call idle timeout (lines 1101-1123) changes from a single threshold to a two-tier system:

```
// Pseudocode for the new logic:
double TimeSinceLastTool = Now - LastToolCallTimestamp;

if (TimeSinceLastTool > CLI_TOOL_IDLE_KILL_SECONDS)
{
    // Tier 2: hard kill -- genuinely hung
    bLastRunTimedOut = true;
    terminate;
}
else if (TimeSinceLastTool > CLI_TOOL_IDLE_NUDGE_SECONDS && !bNudgeKillIssued)
{
    // Tier 1: nudge kill -- agent is thinking too long
    // Mark as timeout so HandleResponseCompleteAutonomous triggers auto-continue
    bNudgeKillIssued = true;
    bLastRunTimedOut = true;
    terminate;
}
```

New local bool `bNudgeKillIssued` (default false) in the background lambda prevents the nudge from firing twice in the same process lifetime. Since the process is terminated, this effectively means: first timeout at 120s = nudge-kill, second process (relaunched by auto-continue) gets the full 300s.

**Rate limit interaction:** Rate-limited tool calls still fire the `OnToolCalled` delegate (the MCP server dispatches the handler, which returns a RATE_LIMITED error, but the delegate fires before the handler returns). So `LastToolCallTimestamp` is updated on rate-limited calls. This is correct behavior -- the agent IS actively working.

**Stdout idle timeout change:** Raise from 120s to 300s. A process that produces no stdout at all for 300s is genuinely hung. The old 120s was too aggressive for autonomous runs where the LLM generates long internal thinking blocks.

**Settings integration:** Read `AutonomousIdleToolSeconds` from `UOliveAISettings` for the nudge threshold (so users can tune it). Keep the kill threshold at `max(NudgeThreshold * 2.5, 300)` or a separate setting.

#### Insertion Points

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

1. **Lines 36-37** -- Replace `CLI_IDLE_TIMEOUT_SECONDS` constant with the three new constants
2. **Lines 1048-1049** -- After reading `MaxRuntimeSeconds` from settings, also read the nudge timeout:
   ```cpp
   const double NudgeSeconds = RuntimeSettings
       ? FMath::Max(static_cast<double>(RuntimeSettings->AutonomousIdleToolSeconds), 60.0)
       : CLI_TOOL_IDLE_NUDGE_SECONDS;
   ```
3. **Line 1040** -- Add `bool bNudgeKillIssued = false;` alongside `LastOutputTime`
4. **Lines 1082-1098** -- Stdout idle: change `EffectiveIdleTimeout` to `CLI_STDOUT_IDLE_SECONDS`
5. **Lines 1101-1123** -- Tool idle: replace single-threshold with two-tier logic

**File:** `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h`

No changes needed -- all new state is local to `LaunchCLIProcess` background lambda.

#### Edge Cases

- **First run vs auto-continue:** The nudge-kill triggers auto-continue at `AutoContinueCount < MaxAutoContinues`. Since `MaxAutoContinues = 2`, the sequence is: Run 1 (120s nudge-kill) -> Run 2 (300s hard kill) -> Report to user. This is 420s total, which is generous enough for complex tasks.
- **Process that produces stdout but no tools for 300s:** Hard-killed. This catches infinite "thinking" loops.
- **Process with no stdout at all:** Caught by the new 300s stdout idle timeout. Covers segfaults and deadlocks.

---

### Change 2: Post-Run Empty-Graph Scan + Directive Continuation Prompt

**Complexity:** COMPLEX (coder)

#### Problem

When auto-continue fires after a timeout, `BuildContinuationPrompt` generates a generic message. It knows which assets were modified (from `LastRunContext.ModifiedAssetPaths`) and shows their current state via `BuildAssetStateSummary`, but:
1. It doesn't specifically call out which function graphs are empty
2. It doesn't provide function signatures (inputs/outputs) for empty functions
3. It doesn't order functions by dependency complexity
4. It doesn't give a concrete "write THIS function FIRST" directive

The existing `BuildAssetStateSummary` (line 1579) already loads Blueprints and iterates function graphs with node counts. It even marks empty functions with "EMPTY, needs plan_json" (line 1669). But it doesn't extract input/output parameter signatures or available variables for the continuation prompt.

#### Design: Extend BuildAssetStateSummary + Restructure BuildContinuationPrompt

**Step A: New helper -- `ScanEmptyFunctionGraphs()`**

A new free function (or static method on the provider base) that scans Blueprints for empty function graphs and returns structured data:

```cpp
/** Info about a function graph that has no logic (just entry point). */
struct FEmptyFunctionInfo
{
    FString AssetPath;
    FString FunctionName;
    int32 NodeCount;          // Typically 1-2 (entry + maybe result)
    TArray<FString> Inputs;   // "ParamName:TypeName" pairs
    TArray<FString> Outputs;  // "ParamName:TypeName" pairs
    bool bHasCrossAssetDeps;  // References types from other modified assets
};

/**
 * Scan modified assets for empty function graphs.
 * MUST be called on the game thread (loads UObject packages).
 *
 * @param AssetPaths  Asset paths to scan
 * @return Array of empty function info, sorted: pure-logic functions first,
 *         cross-asset-dependent functions last
 */
TArray<FEmptyFunctionInfo> ScanEmptyFunctionGraphs(const TArray<FString>& AssetPaths);
```

Implementation:
1. For each asset path, load `UBlueprint*`
2. Iterate `Blueprint->FunctionGraphs`
3. If `Graph->Nodes.Num() <= 2`, it's empty (just FunctionEntry + possibly FunctionResult)
4. Find the function signature via `Blueprint->FunctionGraphs` entries -- find the `UK2Node_FunctionEntry`/`UK2Node_FunctionResult` nodes to get pin names and types
5. OR use `FindFField<UFunction>(Blueprint->SkeletonGeneratedClass, *FuncName)` to get the `UFunction*` and iterate its parameters
6. Check `bHasCrossAssetDeps`: scan parameter types for references to other assets in `AssetPaths` (e.g., if a function takes `BP_Weapon*` and BP_Weapon is in `AssetPaths`, it has cross-asset deps)
7. Sort: non-cross-asset-dep functions first, cross-asset-dep functions last

**Step B: Restructure `BuildContinuationPrompt()`**

Replace the generic "### Your Task Now" section (lines 1544-1557) with a directive section that uses `ScanEmptyFunctionGraphs()`:

```
### Remaining Work: Empty Function Graphs

EMPTY (no logic — write these with apply_plan_json):
1. BP_InventoryComponent::AddItem (ItemName:String, ItemIcon:Texture2D*, ItemQuantity:Integer)
   Available vars: ItemNames(StringArray), ItemIcons(ObjectArray), ItemQuantities(IntArray)
2. BP_InventoryComponent::RemoveItem (ItemName:String)
   Available vars: same as above
3. WBP_InventoryGrid::RefreshInventory (no inputs)
   Cross-asset: needs WBP_InventorySlot (CreateWidget)

### Your Task Now
Write the logic for BP_InventoryComponent::AddItem FIRST using apply_plan_json.
Then write RemoveItem. Then RefreshInventory.
Do NOT re-read these assets — their current state is shown above.
Compile after each function.
```

**Batching rules:**
- Include full context (parameters + available variables) for the first 3 functions
- Include only names for functions 4+
- Cap total "Remaining Work" section at 4000 chars
- If there are 0 empty functions, skip the section entirely (task may be complete or only needs compilation)

**Available variables:** For each Blueprint with empty functions, list variables that could be relevant to the function's logic. Collect from `Blueprint->NewVariables` -- show name and type category (array, object, etc.). Cap at 10 variables per Blueprint.

**Dependency ordering heuristic:**
1. Functions with only primitive/struct inputs (no cross-asset references) -> first
2. Functions that reference types from the same Blueprint -> second
3. Functions that reference types from other modified assets -> last
4. Within each tier, order by parameter count (simpler first)

#### Insertion Points

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

1. **After line ~1577** (after `BuildContinuationPrompt`) -- Add the `ScanEmptyFunctionGraphs()` function and the `FEmptyFunctionInfo` struct. These can go in the anonymous namespace since they're only used by this file.
2. **Lines 1536-1541** -- In `BuildContinuationPrompt()`, after the existing `BuildAssetStateSummary()` call: add a new `ScanEmptyFunctionGraphs()` call and format the directive section.
3. **Lines 1544-1557** -- Replace the generic "### Your Task Now" section with the new directive that names the first function to implement.

**File:** `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h`

No public API changes. `FEmptyFunctionInfo` and `ScanEmptyFunctionGraphs` are implementation details in the anonymous namespace (or private static).

#### Data Flow

```
HandleResponseCompleteAutonomous (timeout detected)
    -> AutoContinueCount++
    -> BuildContinuationPrompt(nudge_message)
        -> BuildAssetStateSummary() [existing -- loads BPs, shows node counts]
        -> ScanEmptyFunctionGraphs(LastRunContext.ModifiedAssetPaths) [NEW]
            -> For each BP: load, iterate FunctionGraphs, check node count
            -> Extract signatures from UK2Node_FunctionEntry pins or UFunction*
            -> Sort by dependency complexity
        -> Format "### Remaining Work" section with first 3 functions fully detailed
        -> Format "### Your Task Now" with concrete first-function directive
    -> SendMessageAutonomous(enriched_prompt)
```

#### Edge Cases

- **No empty functions:** The scan returns empty array. The continuation prompt skips the "Remaining Work" section and says "All functions have graph logic. Compile each Blueprint and verify 0 errors."
- **All functions are cross-asset-dependent:** Put the one with the fewest dependencies first. In the extreme case (circular dependencies), just pick the one with fewest parameters.
- **EventGraph needs logic too:** Check `UbergraphPages` for node count. If the EventGraph has only a few nodes (e.g., just BeginPlay with no connections), mention it: "EventGraph has 2 nodes -- may need BeginPlay logic wired."
- **Widget Blueprint / AnimBP:** The same scan works -- `FunctionGraphs` exists on all `UBlueprint` subclasses. Widget event graphs are in `UbergraphPages`.
- **Prompt size bloat:** Hard cap at 4000 chars for the "Remaining Work" section. If exceeded, truncate to function names only (no signatures) for functions beyond the first 3.

---

### Change 3: Depth-First Warning in Tool Results

**Complexity:** SIMPLE (coder_junior)

#### Problem

When the agent calls `blueprint.add_function` or `blueprint.create`, it doesn't see any reminder about existing empty function graphs on the current run's assets. The existing message in `add_function` (line 4075-4079) says "Do NOT add another function until this one has graph logic" but doesn't name specific empty functions on OTHER assets.

#### Design: Lightweight Empty-Graph Check in Tool Result

After a successful `blueprint.add_function` call, scan the CURRENT Blueprint (the one the function was just added to) for other empty function graphs. If any exist, append a one-line note to the result message.

This is scoped to the current Blueprint only (not all run assets) to keep it cheap and relevant. The agent is already working on this Blueprint -- reminding it about empty functions on a different Blueprint would be confusing.

**Implementation:**

In `HandleAddFunctionType_Function`, after the pipeline execution succeeds (around line 4082), before returning:

```cpp
// After: return FOliveWriteResult::Success(ResultData);
// Scan the same Blueprint for other empty function graphs
int32 EmptyCount = 0;
FString FirstEmpty;
for (const UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
{
    if (!FuncGraph) continue;
    // Skip the function we just added
    if (FuncGraph->GetName() == Signature.Name) continue;
    if (FuncGraph->Nodes.Num() <= 2)
    {
        EmptyCount++;
        if (FirstEmpty.IsEmpty()) FirstEmpty = FuncGraph->GetName();
    }
}
if (EmptyCount > 0)
{
    FString Note = FString::Printf(
        TEXT(" Note: %s::%s has no graph logic yet (%d empty function(s) on this Blueprint)."),
        *FPaths::GetBaseFilename(AssetPath), *FirstEmpty, EmptyCount);
    // Append to the message field in ResultData
    FString Msg;
    ResultData->TryGetStringField(TEXT("message"), Msg);
    ResultData->SetStringField(TEXT("message"), Msg + Note);
}
```

**For `blueprint.create`:** The create handler produces a new Blueprint that typically has no functions yet (unless using a template). Adding a warning here would always say "0 empty functions" which is useless. Skip `blueprint.create` -- the warning only matters when functions already exist.

**For `blueprint.add_function` with `function_type=custom_event`:** Custom events go into the EventGraph, not separate function graphs. The scan should still check `FunctionGraphs` for empty function stubs, but the custom event itself won't appear there. The warning is still relevant.

#### Insertion Points

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

1. **Lines 4071-4083** -- Inside the executor lambda's success path in `HandleAddFunctionType_Function`. The executor lambda already has access to `AssetPath` and `Signature`. It does NOT have direct access to `Blueprint*` (the lambda captures by value and the Blueprint was loaded before pipeline execution). The executor lambda receives `UObject* Target` which IS the Blueprint.

Revised insertion: Inside the executor lambda, after building `ResultData`, cast `Target` to `UBlueprint*`:

```cpp
UBlueprint* BP = Cast<UBlueprint>(Target);
if (BP)
{
    int32 EmptyCount = 0;
    FString FirstEmpty;
    for (const UEdGraph* FuncGraph : BP->FunctionGraphs)
    {
        if (!FuncGraph) continue;
        if (FuncGraph->GetName() == Signature.Name) continue;
        if (FuncGraph->Nodes.Num() <= 2)
        {
            EmptyCount++;
            if (FirstEmpty.IsEmpty()) FirstEmpty = FuncGraph->GetName();
        }
    }
    if (EmptyCount > 0)
    {
        FString CurrentMsg;
        ResultData->TryGetStringField(TEXT("message"), CurrentMsg);
        ResultData->SetStringField(TEXT("message"),
            CurrentMsg + FString::Printf(
                TEXT(" Note: %s has no graph logic yet (%d empty function(s) total)."),
                *FirstEmpty, EmptyCount));
    }
}
```

This goes between lines 4080 and 4082 (before `return FOliveWriteResult::Success(ResultData);`).

#### Edge Cases

- **Function just added IS the only one:** EmptyCount = 0 (we skip the just-added function). No warning.
- **Template-created functions already have logic:** NodeCount > 2, so they're not flagged.
- **Very many empty functions:** The warning says the count but only names the first one. This is intentional -- one specific name is more actionable than a list.

---

### Change 4: Target Class Inference from @ref Component Inputs

**Complexity:** SIMPLE (coder_junior -- contained, ~30 lines)

#### Problem

When the AI writes `{"op":"call", "target":"SetStaticMesh", "inputs":{"Target":"@PickupMesh"}}`, the resolver calls `FindFunctionEx("SetStaticMesh", "", BP)`. FindFunction searches the Actor class hierarchy and common libraries but not `UStaticMeshComponent` specifically. It fails with FUNCTION_NOT_FOUND.

Meanwhile, `ExpandComponentRefs` already identified `PickupMesh` as an SCS component and synthesized a `_synth_getcomp_pickupmesh` get_var step. The component's class (`UStaticMeshComponent`) is known. But `ResolveCallOp` doesn't look at the Target input's component class.

The `CastTargetMap` already handles cast steps and `get_var` steps for object-typed variables. The fix extends this same pattern to include SCS component references.

#### Design: Extend CastTargetMap Pre-Scan to Include Component Classes

In the pre-scan loop that builds `CastTargetMap` (lines 446-499), add a third scan for synthetic component get_var steps:

```cpp
// 3. Scan for synthetic component get_var steps created by ExpandComponentRefs.
// These steps have IDs like "_synth_getcomp_pickupmesh" and their Target is
// the component variable name. Look up the SCS node to get the component class.
if (Blueprint && Blueprint->SimpleConstructionScript)
{
    for (const FOliveIRBlueprintPlanStep& PreScanStep : MutablePlan.Steps)
    {
        if (PreScanStep.Op == OlivePlanOps::GetVar
            && PreScanStep.StepId.StartsWith(TEXT("_synth_getcomp_")))
        {
            if (CastTargetMap.Contains(PreScanStep.StepId))
            {
                continue;
            }

            // PreScanStep.Target is the component variable name
            TArray<USCS_Node*> AllSCSNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
            for (USCS_Node* SCSNode : AllSCSNodes)
            {
                if (SCSNode && SCSNode->ComponentClass
                    && SCSNode->GetVariableName().ToString().Equals(
                        PreScanStep.Target, ESearchCase::IgnoreCase))
                {
                    CastTargetMap.Add(PreScanStep.StepId,
                        SCSNode->ComponentClass->GetName());
                    break;
                }
            }
        }
    }
}
```

This is sufficient because when `ResolveCallOp` fails to find a function, it already scans all step inputs for `@ref` entries and looks them up in `CastTargetMap` (lines 1829-1931). If the AI wrote `"Target": "@PickupMesh"` and ExpandComponentRefs turned it into `"Target": "@_synth_getcomp_pickupmesh"`, the existing fallback will:
1. Parse `@_synth_getcomp_pickupmesh` -> step ID `_synth_getcomp_pickupmesh`
2. Find `StaticMeshComponent` in `CastTargetMap`
3. Call `FindFunctionEx("SetStaticMesh", "StaticMeshComponent", nullptr)`
4. Succeed, because `SetStaticMesh` is a member of `UStaticMeshComponent`

**But wait -- does ExpandComponentRefs rewrite `@PickupMesh` into `@_synth_getcomp_pickupmesh.auto` in the step's Inputs?**

Yes. Looking at `ExpandComponentRefs` (around line 1040), when it finds `@ComponentName` as a bare (dotless) ref, it synthesizes a `_synth_getcomp_` step and rewrites the input to `@_synth_getcomp_xxx.auto`. So by the time `ResolveCallOp` runs, the input already references the synth step.

**Alternative for `@ComponentName.auto` (dotted ref):** If the AI writes `"Target": "@PickupMesh.auto"`, `ExpandComponentRefs` does NOT synthesize a step (the `@ref.pin` pattern is handled differently). In this case, the ref goes straight to the executor, and the CastTargetMap approach doesn't help. However, this is acceptable -- the AI should use `@PickupMesh` (dotless) for components, per the cli_blueprint.txt rules.

#### Insertion Points

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

1. **After line ~492** (after the get_var object-typed variable scan, before the CastTargetMap log): Insert the SCS component scan block (~20 lines).

#### Edge Cases

- **Component doesn't exist on SCS:** The scan simply skips it -- no entry in CastTargetMap. The existing FUNCTION_NOT_FOUND error fires as before.
- **Native C++ components (e.g., Mesh on ACharacter):** These are `FObjectProperty` entries, not SCS nodes. The scan won't find them. However, the existing FindFunction search already covers parent class hierarchy, so `SetStaticMesh` on `Mesh` (which is a `USkeletalMeshComponent`) would be found through the SCS component class search step in FindFunction itself.
- **Multiple components of same class:** Only the referenced component's class matters -- all instances of `UStaticMeshComponent` share the same function set.
- **Synth step from parent BP's component:** `ExpandComponentRefs` walks parent SCS too, so the synth step exists. The CastTargetMap scan should also walk parent SCS. Use `Blueprint->SimpleConstructionScript->GetAllNodes()` which includes inherited nodes. For native components, check `GeneratedClass` hierarchy's `FObjectProperty` entries.

**Enhanced version (walk native components too):**

```cpp
// Also check native C++ components (e.g., CapsuleComponent on ACharacter)
if (Blueprint->GeneratedClass)
{
    for (TFieldIterator<FObjectProperty> It(Blueprint->GeneratedClass,
        EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        FObjectProperty* ObjProp = *It;
        if (ObjProp && ObjProp->PropertyClass
            && ObjProp->PropertyClass->IsChildOf(UActorComponent::StaticClass())
            && ObjProp->GetName().Equals(PreScanStep.Target, ESearchCase::IgnoreCase))
        {
            CastTargetMap.Add(PreScanStep.StepId, ObjProp->PropertyClass->GetName());
            break;
        }
    }
}
```

Actually, this native component check should be inside the existing per-step loop. Let me restructure -- the full scan should happen for ALL `_synth_getcomp_` steps, checking SCS first, then native properties.

---

### Change 5: Depth-First Guidance in Knowledge Pack

**Complexity:** SIMPLE (coder_junior -- text edit only)

#### Problem

The cli_blueprint.txt knowledge pack has a "MULTI-ASSET" line (line 25) that says "build each one fully (structure + logic + compile) before starting the next." But this is easy to miss among all the other rules. The agent needs a more prominent reminder tied to the specific failure mode (empty function shells).

#### Design: Add "Build Order for Multi-Asset Tasks" Section

Insert a new section after the "Compile-Per-Function" section (after line 143):

```
## Multi-Asset Build Order

For tasks that create multiple Blueprints (e.g., inventory system, weapon system):
1. Create ALL assets first (blueprint.create for each).
2. Add variables + components to ALL assets.
3. Then go DEPTH-FIRST: pick ONE Blueprint, write ALL its function graph logic (apply_plan_json), compile to 0 errors, then move to the next.

WRONG: Create BP_Inventory -> add functions -> Create WBP_Grid -> add functions -> (timeout with no graph logic)
RIGHT: Create BP_Inventory -> add functions -> write AddItem logic -> write RemoveItem logic -> compile -> Create WBP_Grid -> ...

Empty function shells are worthless. A Blueprint with 5 function signatures but 0 graph logic does nothing.
If you've created function signatures, write their logic IMMEDIATELY before creating more assets.
```

#### Insertion Point

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

After line 143 (after the "## Compile-Per-Function" section and before "## Common Mistakes").

---

## Module Boundary Specification

All changes are contained within existing files:

| Change | Files Modified | New Files | Dependencies Added |
|--------|---------------|-----------|-------------------|
| 1 | OliveCLIProviderBase.cpp | None | None |
| 2 | OliveCLIProviderBase.cpp | None | EdGraph/EdGraphNode.h (for UK2Node_FunctionEntry pin access) |
| 3 | OliveBlueprintToolHandlers.cpp | None | None |
| 4 | OliveBlueprintPlanResolver.cpp | None | None |
| 5 | cli_blueprint.txt | None | None |

No new public APIs. No new singleton classes. No new header files. Changes 2 and 4 add private implementation functions in anonymous namespaces.

## Implementation Order

```
Phase 1 (parallel -- two junior tasks):
  [T1] Change 5: cli_blueprint.txt text edit (5 minutes)
  [T2] Change 3: add_function empty-graph warning (30 minutes)

Phase 2 (parallel -- one junior, one senior):
  [T3] Change 4: CastTargetMap component class extension (30 minutes, junior)
  [T4] Change 1: Progressive idle timeout (1 hour, senior)

Phase 3 (depends on Change 1):
  [T5] Change 2: Empty-graph scan + directive continuation (2 hours, senior)
```

Rationale for ordering:
- Changes 5 and 3 are independent text/code edits with zero coupling. Do them first.
- Change 4 is independent of the timeout system. Can be done in parallel.
- Change 1 must be done before Change 2 because the enriched continuation prompt (Change 2) is only useful if the nudge-kill (Change 1) is working.
- Change 2 is the most complex and highest-priority change. It depends on Change 1's nudge-kill being wired up so the auto-continue path fires and uses the new prompt.

## Testing Plan

### Change 1: Timeout Verification
- Start an autonomous run with a complex multi-asset task
- Monitor logs for `"no MCP tool call in 120 seconds"` at the nudge tier
- Verify auto-continue fires (log: `"Run timed out (attempt 1/2) -- relaunching"`)
- Verify second run gets 300s timeout
- Test rate-limit interaction: send rapid write calls to trigger RATE_LIMITED, verify timer resets

### Change 2: Continuation Prompt Quality
- After a timeout + auto-continue, capture the enriched prompt (log it)
- Verify it lists specific empty function names with signatures
- Verify non-cross-asset functions are listed before cross-asset ones
- Verify the "Your Task Now" directive names a specific first function
- Test edge case: all functions have logic (should say "compile and verify")
- Test edge case: single Blueprint with 10+ empty functions (should cap at 3 detailed + rest names-only)

### Change 3: Tool Result Warning
- Call `blueprint.add_function` on a Blueprint that already has 2 empty function graphs
- Verify the tool result contains `"Note: FuncName has no graph logic yet (2 empty function(s) total)"`
- Call `blueprint.add_function` on a Blueprint with no other functions -- verify no warning

### Change 4: Component Target Inference
- Create a plan with `{"op":"call","target":"SetStaticMesh","inputs":{"Target":"@MeshComp"}}` where MeshComp is a UStaticMeshComponent on the Blueprint's SCS
- Verify the resolver finds SetStaticMesh via CastTargetMap (log: `"via cast target"`)
- Test with a native component (e.g., `@CapsuleComponent` on a Character BP)

### Change 5: Knowledge Pack
- Verify cli_blueprint.txt loads correctly (no parse errors)
- Run an autonomous multi-asset task and verify the agent follows depth-first ordering

## Summary for the Coder

**Start with Changes 5 and 3** (simple, can be done in parallel by a junior coder):
- Change 5: Add a "Multi-Asset Build Order" section to `cli_blueprint.txt` after line 143
- Change 3: In `HandleAddFunctionType_Function`'s executor lambda success path (line ~4080), cast `Target` to `UBlueprint*`, count empty function graphs, append a note to the result message

**Then Change 4** (simple, junior):
- In `OliveBlueprintPlanResolver.cpp` around line 492, add a third pre-scan loop for `_synth_getcomp_` steps that looks up SCS component classes and adds them to `CastTargetMap`

**Then Change 1** (complex, senior):
- Replace the flat 120s timeout constants with a two-tier system (120s nudge-kill / 300s hard kill)
- The nudge-kill terminates the process (not stdin injection -- stdin is already closed)
- Read `AutonomousIdleToolSeconds` from settings for the nudge threshold
- Raise stdout idle timeout from 120s to 300s

**Finally Change 2** (complex, senior -- depends on Change 1):
- Add `ScanEmptyFunctionGraphs()` helper in the anonymous namespace of OliveCLIProviderBase.cpp
- Call it from `BuildContinuationPrompt()` after `BuildAssetStateSummary()`
- Generate a structured "Remaining Work" section listing empty functions with signatures
- Replace the generic "Your Task Now" directive with a concrete first-function instruction
- Sort by dependency complexity (pure-logic first, cross-asset last)
- Cap at 4000 chars for the section
