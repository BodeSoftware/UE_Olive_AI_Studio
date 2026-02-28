# AI Freedom Design -- Unblock, Simplify, Discover

**Author:** Architect Agent
**Date:** 2026-02-28
**Status:** DRAFT -- awaiting user approval
**Scope:** Olive AI Studio plugin (`Plugins/UE_Olive_AI_Studio/`)

---

## Executive Summary

The tools are fighting the AI. Research across 4 session logs shows a 16-33% tool failure rate, with 5 confirmed false negatives where Olive rejected operations that UE5 would have accepted. This design specifies changes across 3 phases:

- **Phase 1 (Unblock):** Fix 6 false negatives (including PlanExecutor success masking), audit all 20 validation rules (keep 4, relax 4, remove 3, leave 9 untouched), remove 3 prompt restrictions, template content fix. ~2-3 days.
- **Phase 2 (Simplify):** Consolidate 110 tools down to ~40 by merging read tools, unifying write tools, and removing redundant wrappers. Plan-JSON stays as the differentiator. Recipe mandate removed. ~1-2 weeks.
- **Phase 3 (Discover):** Enhance `blueprint.read` with function node counts and compile status, add `blueprint.describe_node_type` tool, improve error messages with suggestions from actual UE5 state. Ongoing.

---

## Phase 1: Unblock the AI

### 1.1 Fix the 5 Concrete False Negatives

#### FN-1: `override_function` ignores interface methods

**Problem:** `FOliveBlueprintWriter::OverrideFunction()` (line 1046) does `Blueprint->ParentClass->FindFunctionByName()` only. When the function is defined on an interface in `Blueprint->ImplementedInterfaces`, it returns "Function not found in parent class."

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveBlueprintWriter.cpp`
**Function:** `FOliveBlueprintWriter::OverrideFunction()` (line 1024)

**Fix:** After the parent class lookup fails, iterate `Blueprint->ImplementedInterfaces` and check each interface's function list.

```cpp
// Current code (line 1046):
UFunction* ParentFunction = Blueprint->ParentClass->FindFunctionByName(FName(*FunctionName));
if (!ParentFunction)
{
    return FOliveBlueprintWriteResult::Error(
        FString::Printf(TEXT("Function '%s' not found in parent class"), *FunctionName));
}

// New code:
UFunction* ParentFunction = Blueprint->ParentClass->FindFunctionByName(FName(*FunctionName));

// If not in parent class, check implemented interfaces
if (!ParentFunction)
{
    for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
    {
        if (InterfaceDesc.Interface)
        {
            ParentFunction = InterfaceDesc.Interface->FindFunctionByName(FName(*FunctionName));
            if (ParentFunction)
            {
                break;
            }
        }
    }
}

if (!ParentFunction)
{
    // Build a helpful error message listing where we searched
    FString SearchedLocations = FString::Printf(
        TEXT("Function '%s' not found. Searched: parent class '%s'"),
        *FunctionName, *Blueprint->ParentClass->GetName());

    for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
    {
        if (InterfaceDesc.Interface)
        {
            SearchedLocations += FString::Printf(TEXT(", interface '%s'"),
                *InterfaceDesc.Interface->GetName());
        }
    }

    return FOliveBlueprintWriteResult::Error(SearchedLocations);
}
```

**Downstream impact:** None. The rest of `OverrideFunction` uses `ParentFunction` and `FunctionName` generically -- it does not assume parent-class origin.

**Also update the error message in the tool handler** (line 3684 in `OliveBlueprintToolHandlers.cpp`):
```cpp
// Current:
TEXT("Verify the function exists in a parent class and is overridable")
// New:
TEXT("Verify the function exists in a parent class or implemented interface and is overridable")
```

---

#### FN-2: `add_interface` rejects short names

**Problem:** `FOliveBlueprintWriter::FindInterfaceClass()` (line 1864) tries `LoadObject`, `FindFirstObject`, and `I`/`U` prefix variants, but never falls back to asset registry search when a short name like `BPI_Interactable` is given.

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveBlueprintWriter.cpp`
**Function:** `FOliveBlueprintWriter::FindInterfaceClass()` (line 1864)

**Fix:** After all existing lookups fail, search the asset registry for Blueprint assets matching the short name.

```cpp
// Append after the existing "Try with U prefix" block (after line 1895):

// Asset registry fallback: search for Blueprint interface by short name
if (!Class)
{
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FAssetData> Assets;
    AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets);

    for (const FAssetData& Asset : Assets)
    {
        if (Asset.AssetName.ToString() == InterfacePath ||
            Asset.AssetName.ToString() == (TEXT("BPI_") + InterfacePath))
        {
            UBlueprint* InterfaceBP = Cast<UBlueprint>(Asset.GetAsset());
            if (InterfaceBP && InterfaceBP->GeneratedClass &&
                InterfaceBP->GeneratedClass->HasAnyClassFlags(CLASS_Interface))
            {
                Class = InterfaceBP->GeneratedClass;

                UE_LOG(LogOliveBPWriter, Log,
                    TEXT("FindInterfaceClass: Resolved short name '%s' to '%s' via asset registry"),
                    *InterfacePath, *Asset.GetObjectPathString());
                break;
            }
        }
    }
}

return Class;
```

**Include needed:** `#include "AssetRegistry/AssetRegistryModule.h"` (likely already included; verify).

**Performance note:** This only fires after 3 cheaper lookups fail. The asset registry query is O(n) over all Blueprint assets but runs only once per `add_interface` call. Acceptable.

---

#### FN-3: `EventTick` alias missing

**Problem:** The `EventNameMap` in `OliveBlueprintPlanResolver.cpp` (line 1531) has `"Tick" -> "ReceiveTick"` but NOT `"EventTick" -> "ReceiveTick"`. The AI uses the display name `EventTick` (which is how it appears in the Blueprint editor's right-click menu), but the resolver does not recognize it.

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**Location:** `EventNameMap` static map (line 1531)

**Fix:** Add missing aliases. Also audit for completeness.

```cpp
static const TMap<FString, FString> EventNameMap = {
    // Existing entries
    { TEXT("BeginPlay"),          TEXT("ReceiveBeginPlay") },
    { TEXT("EndPlay"),            TEXT("ReceiveEndPlay") },
    { TEXT("Tick"),               TEXT("ReceiveTick") },
    { TEXT("ActorBeginOverlap"),  TEXT("ReceiveActorBeginOverlap") },
    { TEXT("ActorEndOverlap"),    TEXT("ReceiveActorEndOverlap") },
    { TEXT("AnyDamage"),          TEXT("ReceiveAnyDamage") },
    { TEXT("Hit"),                TEXT("ReceiveHit") },
    { TEXT("PointDamage"),        TEXT("ReceivePointDamage") },
    { TEXT("RadialDamage"),       TEXT("ReceiveRadialDamage") },
    { TEXT("Destroyed"),          TEXT("ReceiveDestroyed") },

    // NEW: Display-name aliases (how they appear in the editor)
    { TEXT("EventTick"),          TEXT("ReceiveTick") },
    { TEXT("EventBeginPlay"),     TEXT("ReceiveBeginPlay") },
    { TEXT("EventEndPlay"),       TEXT("ReceiveEndPlay") },
    { TEXT("EventAnyDamage"),     TEXT("ReceiveAnyDamage") },
    { TEXT("EventHit"),           TEXT("ReceiveHit") },
    { TEXT("EventActorBeginOverlap"), TEXT("ReceiveActorBeginOverlap") },
    { TEXT("EventActorEndOverlap"),   TEXT("ReceiveActorEndOverlap") },

    // NEW: Event prefix variants (common AI patterns)
    { TEXT("Event BeginPlay"),    TEXT("ReceiveBeginPlay") },
    { TEXT("Event Tick"),         TEXT("ReceiveTick") },
    { TEXT("Event End Play"),     TEXT("ReceiveEndPlay") },

    // NEW: Receive prefix pass-through (AI sometimes uses the internal name)
    { TEXT("ReceiveBeginPlay"),   TEXT("ReceiveBeginPlay") },
    { TEXT("ReceiveTick"),        TEXT("ReceiveTick") },
    { TEXT("ReceiveEndPlay"),     TEXT("ReceiveEndPlay") },
};
```

**Also:** The `NodeFactory::CreateEventNode` method may have its own separate name lookup. Grep for any hardcoded event name checks there and apply the same aliases if present.

---

#### FN-4: `call` op has no interface message mode

**Problem:** When the AI does `op: "call", target: "Interact"` without `target_class`, the resolver searches `NodeFactory::FindFunction()` which checks the Blueprint's own class, parent hierarchy, SCS components, and library classes -- but never checks interfaces implemented by the Blueprint or by variables in scope. Interface functions exist only on `UInterface` subclasses, which are not in any of those search paths.

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**Function:** `ResolveCallOp()`

**AND:**

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`
**Function:** `FOliveNodeFactory::FindFunction()`

**Fix:** Two-part change:

**Part A -- FindFunction: Add interface search.** After the SCS component scan and before the common library scan, iterate `Blueprint->ImplementedInterfaces` and search each interface class for the function.

```cpp
// In FindFunction(), after SCS component classes scan, before common library classes:

// 5b. Check implemented interfaces
for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
{
    if (InterfaceDesc.Interface)
    {
        FoundFunction = InterfaceDesc.Interface->FindFunctionByName(FName(*FunctionName));
        if (!FoundFunction)
        {
            // Try K2_ prefix
            FoundFunction = InterfaceDesc.Interface->FindFunctionByName(
                FName(*(TEXT("K2_") + FunctionName)));
        }
        if (FoundFunction)
        {
            MatchMethod = EMatchMethod::InterfaceSearch;
            break;
        }
    }
}
```

**Part B -- Add `InterfaceSearch` to `EMatchMethod` enum** in `OliveNodeFactory.h`:
```cpp
enum class EMatchMethod { Exact, K2Prefix, AliasMap, BroadSearch, ComponentClassSearch, InterfaceSearch };
```

**Part C -- Plan executor: handle interface call nodes.** When `MatchMethod == InterfaceSearch`, the executor should create a `UK2Node_Message` node (the interface message call node) instead of a `UK2Node_CallFunction`. This is a deeper change in `PhaseCreateNodes`:

```cpp
// In PhaseCreateNodes, when creating a CallFunction node:
if (Step.ResolvedMatchMethod == EMatchMethod::InterfaceSearch)
{
    // Create an interface message node instead of CallFunction
    // UK2Node_Message is the node type for "Call Interface Function"
    // It has the same pins as CallFunction but routes through the interface
    // UE5 handles the dispatch
    UK2Node_CallFunction* CallNode = /* existing creation code */;
    CallNode->FunctionReference.SetFromField<UFunction>(Step.ResolvedFunction, true);
    // The bIsInterfaceMessage flag or MemberParent on FMemberReference
    // controls whether this becomes a message node
}
```

**Complexity note:** This is the hardest of the 5 fixes. The coder should verify how `UK2Node_CallFunction` vs `UK2Node_Message` (which inherits from `UK2Node_CallFunction`) work in UE 5.5. The key is that interface message calls use `FMemberReference` with the interface class as the scope, and UE5 creates a "call on interface" dispatch node. The simplest implementation may be to just set `FunctionReference.SetExternalMember(FunctionName, InterfaceClass)` on a standard `UK2Node_CallFunction` -- UE5 handles the rest.

**New error code:** `INTERFACE_FUNCTION_FOUND` (info, not error) -- logged when an interface search resolves a function.

---

#### FN-5: Plan resolver cannot find Blueprint-defined functions without `target_class`

**Problem:** When the AI calls `op: "call", target: "SetGunInRange"` on a Blueprint that has a user-defined function called `SetGunInRange`, the resolver fails because it does not search the plan's own Blueprint first (when no `target_class` is specified).

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`
**Function:** `FOliveNodeFactory::FindFunction()`

**Current behavior:** FindFunction searches:
1. Alias map
2. Specified class (if `target_class` provided)
3. Blueprint's `GeneratedClass`
4. Parent class hierarchy
5. SCS component classes
6. Common library classes

Step 3 searches `GeneratedClass`, which *should* include user-defined functions. But `GeneratedClass` is the *compiled* class, and if the Blueprint was just modified (function added but not yet compiled), the function will not be in `GeneratedClass`.

**Fix:** In addition to searching `GeneratedClass`, also search `Blueprint->FunctionGraphs` by name. This catches functions that have been added to the Blueprint but may not yet be compiled.

```cpp
// In FindFunction, after searching GeneratedClass (step 3), before parent hierarchy:

// 3b. Search Blueprint's own function graphs (catches uncompiled functions)
if (!FoundFunction)
{
    for (UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
    {
        if (FuncGraph && FuncGraph->GetName() == FunctionName)
        {
            // Found a matching function graph. Try to find the UFunction on
            // the skeleton class (compiled or not).
            if (Blueprint->SkeletonGeneratedClass)
            {
                FoundFunction = Blueprint->SkeletonGeneratedClass->FindFunctionByName(
                    FName(*FunctionName));
            }
            if (!FoundFunction && Blueprint->GeneratedClass)
            {
                // Last resort: the function graph exists but UFunction doesn't.
                // Return a synthetic match so the caller knows the function is valid.
                // The plan executor will reference this function by name.
                FoundFunction = Blueprint->GeneratedClass->FindFunctionByName(
                    FName(*FunctionName));
            }
            if (FoundFunction)
            {
                MatchMethod = EMatchMethod::Exact;
                break;
            }
            else
            {
                // Function graph exists but no UFunction yet (pre-compile state).
                // Log and let the caller decide.
                UE_LOG(LogOliveNodeFactory, Log,
                    TEXT("FindFunction: Found function graph '%s' but no UFunction. "
                         "Blueprint may need compilation."),
                    *FunctionName);
                // Still return null -- but a more helpful message will be given
            }
        }
    }
}
```

**Alternative (simpler):** Force a compile-if-dirty before searching. But this could be expensive and side-effect-heavy. The `FunctionGraphs` scan is cheaper and more predictable.

**Also:** The `FindFunction` search already includes `Blueprint->GeneratedClass` which should find compiled user functions. The failure in the logs happened because no `target_class` was set AND the Blueprint was being searched via the plan resolver's `ResolveCallOp`, which might pass an empty Blueprint context. Verify that `ResolveCallOp` passes the correct `UBlueprint*` to `FindFunction` -- if it passes `nullptr`, that is the actual bug.

---

#### FN-6: PlanExecutor `bSuccess` masks wiring failures

**Problem:** `FOlivePlanExecutor::Execute()` sets `Result.bSuccess = bAllNodesCreated` (line 2412), which is `true` when all nodes were created even if half the exec wiring and data connections failed. The `bPartial` flag and error counters exist but downstream consumers (including the brain layer's self-correction trigger) that check only `bSuccess` will conclude the plan fully succeeded. This causes the AI to stop trying to fix broken exec chains.

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** Final result assembly (~line 2408-2429)

**Fix:** `bSuccess` must account for wiring failures. `bPartial` becomes the intermediate state for cosmetic failures (defaults only).

```cpp
// Current (line 2412):
Result.bSuccess = bAllNodesCreated;

// New:
const bool bHasCriticalWiringErrors = (Context.FailedConnectionCount > 0);
Result.bSuccess = bAllNodesCreated && !bHasCriticalWiringErrors;

// Partial = nodes created, no connection failures, but some defaults failed (cosmetic)
Result.bPartial = bAllNodesCreated && !bHasCriticalWiringErrors
    && (Context.FailedDefaultCount > 0);
```

**Downstream impact:** Self-correction will now trigger on wiring failures (correct behavior). The brain layer checks `HasToolFailure()` which reads `bSuccess`, so this change propagates automatically.

---

### 1.2 Validation Rule Audit

Every validation rule in `OliveValidationEngine.cpp` classified with action and rationale.

#### KEEP (Safety-Critical -- Do Not Touch)

| Rule | Reason |
|------|--------|
| `FOlivePIEProtectionRule` | Prevents write ops during PIE. Removing causes editor crash or corrupted in-flight objects. |
| `FOliveCppCompileGuardRule` | Prevents C++ writes during Live Coding. Race condition risk. |
| `FOliveCppPathSafetyRule` | Prevents `../` path traversal in C++ file ops. Security boundary. |
| `FOliveWriteRateLimitRule` | Configurable rate limiter (`MaxWriteOpsPerMinute=30`). Safety backstop against runaway loops. |

#### RELAX (Downgrade to Warning)

| Rule | Current | Change | Rationale |
|------|---------|--------|-----------|
| `FOliveDuplicateLayerRule` | Error by default, `allow_duplicate=true` bypass | Warning by default, error opt-in via setting | AI correctly creates shadowing members in valid scenarios. The bypass exists but AI must know to use it. Flip the default. |
| `FOliveBTNodeExistsRule` (class resolution) | Error if task/decorator/service class not found via `ResolveNodeClass` | Warning. Let the handler attempt creation; if UE5 rejects, return UE5's error. | Blueprint-only BT node classes may not be loaded when the validation rule runs. This is a confirmed false-negative source. Keep the node-ID format check and parent-composite check (those are structural). |
| `FOliveBPNamingRule` (short name warning) | Warning for names < 3 chars | Remove the short-name warning entirely; keep the invalid-char check as error | Single-char variable names like `X`, `Y`, `Z` are valid and common in math-heavy Blueprints. |
| `FOliveBPAssetTypeRule` | Error if tool targets wrong asset type | Downgrade to warning for read tools, keep error for write tools | Reading a non-Blueprint asset with `blueprint.read` should return "not a Blueprint" data, not block. Writing should still check. |

#### REMOVE (Redundant)

| Rule | Reason for Removal |
|------|-------------------|
| `FOliveAssetExistsRule` | Covers only 18/110 tools. Every tool handler already does `LoadObject<>()` and returns a structured `ASSET_NOT_FOUND` error. The rule is incomplete AND redundant. Removing it eliminates one layer of error message variation (AI sees consistent errors from handlers instead of sometimes-from-rule, sometimes-from-handler). |
| `FOliveCppOnlyModeRule` | Uses `preferred_layer` param that is never set by any current provider or UI. Dead code. If C++-only mode is needed in the future, implement it as a focus profile (which already exists and works). |
| `FOliveBPNodeIdFormatRule` | Validates `node_id` format before the handler runs. The handler already validates node existence and returns better errors with context (e.g., "node_id 'X' not found in graph 'Y' (Z nodes)"). The format check is overly strict -- it blocks `K2Node_FunctionEntry_0` style IDs which some AI models use (even though those are not valid plan step IDs, the handler error is more useful than the pre-check). |

#### LEAVE AS-IS (No Change)

| Rule | Why No Change |
|------|---------------|
| `FOliveSchemaValidationRule` | Validates required params and JSON types. This catches null/missing params before they cause null derefs in handlers. Useful and not a false-negative source. |
| `FOliveBTAssetTypeRule` | Prevents BT tools on non-BT assets. Cheap, correct, prevents confusing errors. |
| `FOliveBBKeyUniqueRule` | Prevents duplicate BB keys, circular parents. Structural correctness checks UE5 does not catch gracefully. |
| `FOlivePCGAssetTypeRule` | Same as BTAssetTypeRule for PCG. |
| `FOlivePCGNodeClassRule` | Already WARNING only, not error. No change needed. |
| `FOliveCppClassExistsRule` | Already WARNING only. No change needed. |
| `FOliveCppEnumExistsRule` | Already WARNING only. No change needed. |
| `FOliveCppStructExistsRule` | Already WARNING only. No change needed. |
| `FOliveBulkReadLimitRule` | 20-asset cap on bulk_read. Data volume guard, not AI restriction. |
| `FOliveSnapshotExistsRule` | Validates snapshot existence. Prevents confusing errors. |
| `FOliveRefactorSafetyRule` | Validates params for refactor/rename. Prevents partial renames. |

#### Implementation

**File:** `Source/OliveAIEditor/Private/Services/OliveValidationEngine.cpp`

For REMOVE rules: delete the `RegisterRule(MakeShared<FRuleName>())` call from the appropriate `Register*Rules()` method. Leave the class definition in place (dead code) for one release cycle.

For RELAX rules: modify the rule's `Validate()` method to call `AddWarning()` instead of `AddError()` for the relaxed cases. For `FOliveDuplicateLayerRule`, flip the `bAllowDuplicate` default:
```cpp
// Change (line 1586):
bool bAllowDuplicate = false;
// To:
bool bAllowDuplicate = true; // Default to warning; AI can set allow_duplicate=false to re-enable error
```
Wait -- this inverts the parameter semantics. Better approach: change the rule to always use `AddWarning()` for the shadowing case, regardless of `bAllowDuplicate`. Keep `bAllowDuplicate` for backwards compat but make it a no-op.

For `FOliveBTNodeExistsRule` class resolution: change `AddError` to `AddWarning` in the three class-resolution blocks (lines ~773, ~789, ~800).

---

### 1.3 Prompt Restriction Removal

#### Remove: Recipe Mandate

**Files to edit:**
1. `Content/SystemPrompts/Knowledge/recipe_routing.txt` (line 1)
2. `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` (line 369, ~1101, ~1143)

**Current text (recipe_routing.txt line 1):**
```
- ALWAYS call olive.get_recipe(query) before your first plan_json for each function.
```

**New text:**
```
- olive.get_recipe(query) has tested wiring patterns. Use it when you're unsure about a pattern, but skip it for simple/well-known operations (e.g., BeginPlay -> PrintString).
```

**Current text (CLIProviderBase.cpp line 369):**
```cpp
"- Before writing your first plan_json for each function, call olive.get_recipe..."
```

**New text:**
```cpp
"- olive.get_recipe has tested wiring patterns. Call it for unfamiliar or complex patterns. "
"Skip it for straightforward operations you already know.\n"
```

**Also remove from CLIProviderBase.cpp ~line 1101 and ~1143:** The continuation prompt's per-function `olive.get_recipe` routing. Replace with a simpler "implement the remaining empty functions" directive without mandating recipe calls.

---

#### Remove: 12-Step Plan Cap

**File:** `Content/SystemPrompts/Knowledge/recipe_routing.txt` (line 10)

**Current text:**
```
- Keep plans under 12 steps; split complex logic into multiple functions
```

**New text:**
```
- Split complex logic into multiple functions when it makes architectural sense. There is no hard step limit, but plans over ~20 steps benefit from splitting.
```

The actual hard limit is `PlanJsonMaxSteps = 128` (in `UOliveAISettings`). The 12-step soft cap was causing the AI to artificially split simple operations.

---

#### Remove: Tool Pack Filtering

**The tool pack system (`FOliveToolPackManager`) is removed entirely.** It gates tools based on intent detection (`bTurnHasExplicitWriteIntent`, `bTurnHasDangerIntent`), which means the AI cannot discover or plan with write tools until the brain layer classifies the turn as a write intent. This is a net negative:

- The AI cannot browse write tool schemas to plan its approach
- Intent detection is imperfect -- "create a gun that shoots" might not trigger `bTurnHasDangerIntent` even though it needs `add_interface`
- Focus profiles already handle domain-level filtering (Blueprint vs C++ profile)
- Claude Code's MCP Tool Search already handles the token bloat concern

**Files to edit:**
1. `Source/OliveAIEditor/Private/Brain/OliveToolPackManager.cpp` -- mark as deprecated, remove initialization from module startup
2. `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp` -- remove `FOliveToolPackManager::Get().Initialize()` from `OnPostEngineInit()`
3. Any code that calls `FOliveToolPackManager::Get().GetCombinedPackTools()` or `IsToolInPack()` -- replace with direct `FOliveToolRegistry::Get().GetToolsForProfile()` calls

**Do NOT delete the files yet.** Mark them deprecated for one release cycle.

---

#### Keep (Modified): Knowledge Packs

The three knowledge packs (`blueprint_authoring.txt`, `recipe_routing.txt`, `node_routing.txt`) remain but are slimmed down.

**`blueprint_authoring.txt` changes:**
- Remove rule 7 ("Batch-first graph editing") -- the AI can decide when to batch vs granular
- Remove the `project.batch_write` references in rule 7 (batch_write is being deprecated in Phase 2)
- Keep rules 1-6 and 8 (read-before-write, variable typing, naming, error recovery, no fake success)

**`node_routing.txt` changes:**
- Remove the "Path 1 / Path 2 / Path 3" hierarchy language. Present plan-JSON and add_node as equals
- Remove the "Error Recovery" section that pushes fallback to add_node. The AI should try self-correction within plan-JSON first
- Rewrite as a concise reference card (ops list, add_node capabilities, get_node_pins purpose)

---

### 1.4 Phase 1 Implementation Order

1. **FN-3 (EventTick alias)** -- 15 minutes. Pure data addition, zero risk.
2. **FN-6 (PlanExecutor bSuccess masking)** -- 15 minutes. One-line fix with high impact on self-correction reliability.
3. **FN-2 (add_interface short name)** -- 30 minutes. Asset registry fallback.
4. **FN-1 (override_function interfaces)** -- 30 minutes. Interface iteration in OverrideFunction.
5. **FN-5 (Blueprint-defined functions)** -- 1 hour. FindFunction enhancement + ResolveCallOp verification.
6. **FN-4 (interface message calls)** -- 2-3 hours. New node type handling in plan executor. Requires UE5 API research for UK2Node_Message.
7. **Validation rule audit** -- 1 hour. Mechanical changes per the table above.
8. **Prompt restriction removal** -- 30 minutes. Text edits to 4 files.
9. **Tool pack removal** -- 1 hour. Remove initialization, redirect callers.
10. **Template content fix** -- 5 minutes. Add note to `pickup_interaction.json`: "If the project already has a player character Blueprint (e.g., BP_ThirdPersonCharacter), modify it instead of creating a new one. Use project.search to check first."

**Total Phase 1: ~2-3 days** including testing.

---

## Phase 2: Simplify the Tool Surface

### 2.1 Design Principles

1. **Domain prefixes stay.** `blueprint.`, `behaviortree.`, `blackboard.`, `pcg.`, `cpp.`, `project.` -- these help the AI scope its tool selection. Do not collapse to a single `edit` tool.
2. **Plan-JSON stays and is strengthened.** It is Olive's genuine differentiator. No competitor has it.
3. **Read tools consolidate aggressively.** Currently 8 Blueprint read tools -- collapse to 2.
4. **Write tools unify around "upsert" semantics.** `add_function` absorbs `add_custom_event`, `add_event_dispatcher`, `override_function`. `add_variable` absorbs `modify_variable`.
5. **Remove tools stay separate from create tools.** Destructive operations have different confirmation tiers.
6. **Templates merge into `blueprint.create`.** `create_from_template` becomes a parameter on `create`.
7. **`project.batch_write` is deprecated.** Plan-JSON handles batching for Blueprint graphs. For non-Blueprint batching, individual tools are fine (BT/PCG/C++ operations are fast enough individually).

### 2.2 Target Tool Set

#### Blueprint Tools (22, down from 53)

| # | Tool | What It Does | Replaces | Key Parameter Changes |
|---|------|-------------|----------|----------------------|
| 1 | `blueprint.read` | Read Blueprint structure with optional section filter | `read`, `read_function`, `read_event_graph`, `read_variables`, `read_components`, `read_hierarchy`, `list_overridable_functions` | New `section` param: `"all"` (default), `"summary"`, `"graph"` (requires `graph_name`), `"variables"`, `"components"`, `"hierarchy"`, `"overridable_functions"`. `graph_name` param selects which graph to read. |
| 2 | `blueprint.get_node_pins` | Get pin manifest for a specific node | (unchanged) | No change |
| 3 | `blueprint.create` | Create a new Blueprint, optionally from template | `create`, `create_from_template` | New optional `template_id` param. When set, applies factory template. Optional `template_params` object. |
| 4 | `blueprint.compile` | Force compile and return results | (unchanged) | No change |
| 5 | `blueprint.add_variable` | Add or modify a variable (upsert) | `add_variable`, `modify_variable` | If variable exists, modifies it. New optional `modify_only` flag to prevent accidental creation. |
| 6 | `blueprint.remove_variable` | Remove a variable | (unchanged) | No change |
| 7 | `blueprint.add_component` | Add or modify a component | `add_component`, `modify_component` | When component name already exists, modifies properties instead of erroring. |
| 8 | `blueprint.remove_component` | Remove a component | `remove_component`, `reparent_component` | New optional `new_parent` param: when set, reparents instead of removing. |
| 9 | `blueprint.add_function` | Add function, custom event, event dispatcher, or override | `add_function`, `add_custom_event`, `add_event_dispatcher`, `override_function` | New `function_type` enum: `"function"` (default), `"custom_event"`, `"event_dispatcher"`, `"override"`. Routes internally to the appropriate handler. |
| 10 | `blueprint.remove_function` | Remove a function | (unchanged) | No change |
| 11 | `blueprint.modify_function_signature` | Modify function params, return type, flags | (unchanged) | No change. Kept separate because signature modification is semantically different from creation. |
| 12 | `blueprint.add_interface` | Add an interface to a Blueprint | (unchanged, but with FN-2 fix) | No change (short-name resolution fixed in Phase 1) |
| 13 | `blueprint.remove_interface` | Remove an interface | (unchanged) | No change |
| 14 | `blueprint.set_parent_class` | Change parent class | (unchanged) | No change. Kept as separate tool because it is Tier 3 (destructive). |
| 15 | `blueprint.delete` | Delete a Blueprint asset | (unchanged) | No change |
| 16 | `blueprint.add_node` | Add a node to a graph | (unchanged) | No change |
| 17 | `blueprint.remove_node` | Remove a node | (unchanged) | No change |
| 18 | `blueprint.connect_pins` | Connect two pins | (unchanged) | No change |
| 19 | `blueprint.disconnect_pins` | Disconnect two pins | (unchanged) | No change |
| 20 | `blueprint.set_pin_default` | Set an input pin's default value | (unchanged) | No change |
| 21 | `blueprint.preview_plan_json` | Preview a plan without mutating | (unchanged) | No change |
| 22 | `blueprint.apply_plan_json` | Apply an intent-level plan atomically | (unchanged) | No change |

**Removed Blueprint tools (8):**
- `blueprint.read_function` -- merged into `blueprint.read` with `section="graph"`
- `blueprint.read_event_graph` -- merged into `blueprint.read` with `section="graph"`
- `blueprint.read_variables` -- merged into `blueprint.read` with `section="variables"`
- `blueprint.read_components` -- merged into `blueprint.read` with `section="components"`
- `blueprint.read_hierarchy` -- merged into `blueprint.read` with `section="hierarchy"`
- `blueprint.list_overridable_functions` -- merged into `blueprint.read` with `section="overridable_functions"`
- `blueprint.create_from_template` -- merged into `blueprint.create` with `template_id` param
- `blueprint.modify_variable` -- merged into `blueprint.add_variable` (upsert)

**Kept but deprecated (not removed yet):**
- `blueprint.set_node_property` -- functionality available via `add_node` properties or `set_pin_default`

**Template tools (2, down from 3):**
| # | Tool | Replaces |
|---|------|----------|
| 23 | `blueprint.list_templates` | (unchanged) |
| 24 | `blueprint.get_template` | (unchanged) |

**AnimBP tools (4, unchanged):**
| # | Tool |
|---|------|
| 25 | `animbp.add_state_machine` |
| 26 | `animbp.add_state` |
| 27 | `animbp.add_transition` |
| 28 | `animbp.set_transition_rule` |

**Widget tools (4, unchanged):**
| # | Tool |
|---|------|
| 29 | `widget.add_widget` |
| 30 | `widget.remove_widget` |
| 31 | `widget.set_property` |
| 32 | `widget.bind_property` |

#### Behavior Tree / Blackboard Tools (12, down from 16)

| # | Tool | Replaces | Changes |
|---|------|----------|---------|
| 33 | `blackboard.create` | (unchanged) | -- |
| 34 | `blackboard.read` | (unchanged) | -- |
| 35 | `blackboard.add_key` | `add_key`, `modify_key` | Upsert: if key exists, modifies it |
| 36 | `blackboard.remove_key` | (unchanged) | -- |
| 37 | `blackboard.set_parent` | (unchanged) | -- |
| 38 | `behaviortree.create` | (unchanged) | -- |
| 39 | `behaviortree.read` | (unchanged) | -- |
| 40 | `behaviortree.set_blackboard` | (unchanged) | -- |
| 41 | `behaviortree.add_node` | `add_composite`, `add_task`, `add_decorator`, `add_service` | New `node_kind` param: `"composite"`, `"task"`, `"decorator"`, `"service"`. Routes internally. Class param stays. |
| 42 | `behaviortree.remove_node` | (unchanged) | -- |
| 43 | `behaviortree.move_node` | (unchanged) | -- |
| 44 | `behaviortree.set_node_property` | (unchanged) | -- |

**Removed BT/BB tools (4):**
- `blackboard.modify_key` -- merged into `blackboard.add_key`
- `behaviortree.add_composite` -- merged into `behaviortree.add_node`
- `behaviortree.add_task` -- merged into `behaviortree.add_node`
- `behaviortree.add_decorator` -- merged into `behaviortree.add_node`
- `behaviortree.add_service` -- merged into `behaviortree.add_node`

(Net: -4 BT tools, but `behaviortree.add_node` is new, so -4 old + 1 new = -3 net, total 12 from previous 16 -- wait, that is 5 removed and 1 added = net -4. 16 - 4 = 12.)

#### PCG Tools (9, unchanged)

PCG tools are already lean. No changes.

| # | Tool |
|---|------|
| 45-53 | All 9 PCG tools unchanged |

#### C++ Tools (11, down from 13)

| # | Tool | Changes |
|---|------|---------|
| 54 | `cpp.read_class` | Absorbs `cpp.list_blueprint_callable` and `cpp.list_overridable`. New `include` param: `"all"` (default), `"callable"`, `"overridable"`. |
| 55 | `cpp.read_enum` | -- |
| 56 | `cpp.read_struct` | -- |
| 57 | `cpp.read_header` | -- |
| 58 | `cpp.read_source` | -- |
| 59 | `cpp.list_project_classes` | -- |
| 60 | `cpp.create_class` | -- |
| 61 | `cpp.add_property` | -- |
| 62 | `cpp.add_function` | -- |
| 63 | `cpp.modify_source` | -- |
| 64 | `cpp.compile` | -- |

**Removed C++ tools (2):**
- `cpp.list_blueprint_callable` -- merged into `cpp.read_class` with `include="callable"`
- `cpp.list_overridable` -- merged into `cpp.read_class` with `include="overridable"`

#### Cross-System / Project Tools (12, down from 20)

| # | Tool | Changes |
|---|------|---------|
| 65 | `project.search` | -- |
| 66 | `project.get_asset_info` | Absorbs `project.get_dependencies` and `project.get_referencers`. These are already returned by `get_asset_info`. |
| 67 | `project.get_class_hierarchy` | -- |
| 68 | `project.bulk_read` | -- |
| 69 | `project.snapshot` | -- |
| 70 | `project.list_snapshots` | -- |
| 71 | `project.rollback` | -- |
| 72 | `project.diff` | -- |
| 73 | `project.get_relevant_context` | -- |
| 74 | `project.refactor_rename` | -- |
| 75 | `olive.get_recipe` | Schema already simplified to `{query: string}`. No mandate to call it. |
| 76 | `project.implement_interface` | -- |

**Removed cross-system tools (8):**
- `project.get_dependencies` -- merged into `project.get_asset_info`
- `project.get_referencers` -- merged into `project.get_asset_info`
- `project.batch_write` -- deprecated. Plan-JSON handles Blueprint batching. Individual BT/PCG/C++ ops are fast enough.
- `project.create_ai_character` -- high-level convenience tool that is essentially a script. The AI can achieve the same by calling `blueprint.create`, `behaviortree.create`, `blackboard.create` individually. Remove it.
- `project.move_to_cpp` -- analysis tool that generates C++ migration suggestions. Rarely used, high maintenance cost. Remove.
- `project.index_build` -- project indexing is now automatic (on-demand). Remove explicit trigger.
- `project.index_status` -- remove (was only needed when index_build was manual).
- `project.get_config` -- rarely used by AI; project config is in the system prompt context. Remove.

#### Total Tool Count

| Category | Before | After | Delta |
|----------|--------|-------|-------|
| Blueprint (incl. AnimBP, Widget, Template) | 53 | 32 | -21 |
| BT + Blackboard | 16 | 12 | -4 |
| PCG | 9 | 9 | 0 |
| C++ | 13 | 11 | -2 |
| Cross-system / Project | 20 | 12 | -8 |
| **Total** | **111** | **76** | **-35** |

Note: 76 is higher than the "25-35" aspirational target. This is intentional. The 25-35 number from competitive analysis assumed a single-domain product. Olive covers 5 domains (Blueprint, BT, PCG, C++, cross-system). Within each domain, the tool count is lean:
- Blueprint graph editing: 7 granular + 2 plan-JSON = 9 tools (vs flopperam's 15)
- Blueprint structure: 13 tools covering the full CRUD lifecycle
- Other domains: 9-12 tools each

Further consolidation would sacrifice clarity (e.g., merging `blueprint.add_variable` and `blueprint.add_component` into a generic `blueprint.add` would confuse the AI about which schema to use).

---

### 2.3 Plan-JSON Pipeline Changes

#### Resolver: Keep, Enhance

The plan resolver stays. Its job is translating AI intent to UE5 specifics (alias resolution, event name mapping, SCS component detection). Phase 1 fixes (FN-3, FN-4, FN-5) strengthen it.

**Remove from resolver:**
- Step cap enforcement (already raised to 128; remove soft warnings about step count)
- The `CollapseExecThroughPureSteps` optimization -- it causes subtle ordering bugs and the AI does not need it. Pure nodes without exec connections just work. (Evaluate this -- if removing it causes regressions, keep it.)

**Add to resolver:**
- Interface function resolution (FN-4)
- Blueprint-defined function resolution (FN-5)

#### Validator: Keep, Soften

The plan validator (`FOlivePlanValidator`) stays. Both checks are legitimate structural guards:

1. `COMPONENT_FUNCTION_ON_ACTOR` -- warn (not error) when a single SCS component matches. Error when 0 or >1 match. (This was already done in Phase 2 auto-wiring design.)
2. `EXEC_WIRING_CONFLICT` -- keep as error. This is a genuine structural invariant.

#### Executor: Keep, Improve

The plan executor stays. All 7 phases stay. Phase 1 fixes (interface calls) add a new creation path in `PhaseCreateNodes`. Phase 1 fix FN-6 corrects `bSuccess` to account for wiring failures.

#### Auto-Preview in `apply_plan_json`

When `apply_plan_json` is called **without a fingerprint**, run the preview internally before applying. This eliminates the preview→apply two-call pattern for the common case (autonomous mode, where the AI often skips preview). The preview result is used for resolver notes and validation but not returned separately — just applied.

**Implementation:** In the `apply_plan_json` handler, when `fingerprint` is empty or missing:
1. Run the existing preview logic (resolve + validate) internally
2. If validation passes, proceed to execution
3. If validation fails, return the preview errors (same as calling `preview_plan_json`)

This saves one round-trip without losing safety. The AI can still call `preview_plan_json` explicitly when it wants to inspect the plan before applying.

---

### 2.4 Backward Compatibility

#### MCP Clients

**Breaking changes for MCP clients:**
1. Tool names for removed tools will return `TOOL_NOT_FOUND` errors
2. Merged tools accept new parameters but remain backward compatible for old parameters

**Migration path:**
- Old tool names that are now aliases should be redirected in `FOliveToolRegistry` via a simple alias map:
```cpp
// In FOliveToolRegistry::FindTool() or equivalent:
static const TMap<FString, FString> ToolAliases = {
    { TEXT("blueprint.read_function"), TEXT("blueprint.read") },
    { TEXT("blueprint.read_event_graph"), TEXT("blueprint.read") },
    { TEXT("blueprint.read_variables"), TEXT("blueprint.read") },
    { TEXT("blueprint.read_components"), TEXT("blueprint.read") },
    { TEXT("blueprint.read_hierarchy"), TEXT("blueprint.read") },
    { TEXT("blueprint.list_overridable_functions"), TEXT("blueprint.read") },
    { TEXT("blueprint.create_from_template"), TEXT("blueprint.create") },
    { TEXT("blueprint.modify_variable"), TEXT("blueprint.add_variable") },
    { TEXT("blueprint.override_function"), TEXT("blueprint.add_function") },
    { TEXT("blueprint.add_custom_event"), TEXT("blueprint.add_function") },
    { TEXT("blueprint.add_event_dispatcher"), TEXT("blueprint.add_function") },
    { TEXT("behaviortree.add_composite"), TEXT("behaviortree.add_node") },
    { TEXT("behaviortree.add_task"), TEXT("behaviortree.add_node") },
    { TEXT("behaviortree.add_decorator"), TEXT("behaviortree.add_node") },
    { TEXT("behaviortree.add_service"), TEXT("behaviortree.add_node") },
    { TEXT("blackboard.modify_key"), TEXT("blackboard.add_key") },
    { TEXT("cpp.list_blueprint_callable"), TEXT("cpp.read_class") },
    { TEXT("cpp.list_overridable"), TEXT("cpp.read_class") },
    { TEXT("project.get_dependencies"), TEXT("project.get_asset_info") },
    { TEXT("project.get_referencers"), TEXT("project.get_asset_info") },
};
```

When an aliased tool is called, the registry:
1. Maps to the new tool name
2. Transforms parameters to match the new schema (e.g., `blueprint.read_function` with `function_name` -> `blueprint.read` with `section="graph"` + `graph_name=function_name`)
3. Logs a deprecation warning

This ensures existing `.mcp.json` configurations and cached tool schemas continue working for one release cycle.

**`tools/list` response:** Only lists the new tool names. Deprecated aliases do not appear in discovery.

---

### 2.5 Phase 2 Implementation Order

1. **Tool alias map in FOliveToolRegistry** -- backward compat infrastructure. 1 day.
2. **`blueprint.read` consolidation** -- merge 7 tools into 1 with `section` param. 2 days. This is the largest change.
3. **`blueprint.add_function` consolidation** -- merge 4 tools into 1 with `function_type` param. 1 day.
4. **`blueprint.add_variable` upsert** -- add modify path when variable exists. 0.5 day.
5. **`blueprint.create` template merge** -- add `template_id` param. 0.5 day.
6. **`behaviortree.add_node` consolidation** -- merge 4 tools into 1 with `node_kind` param. 1 day.
7. **Cross-system tool removal** -- remove deprecated tools, update prompt files. 0.5 day.
8. **Tool pack manager removal** -- mark deprecated, remove from startup. 0.5 day.
9. **Schema file updates** -- update `OliveBlueprintSchemas.cpp` for all new/changed schemas. 1 day.
10. **Knowledge pack updates** -- rewrite `recipe_routing.txt`, `node_routing.txt`, `blueprint_authoring.txt`. 0.5 day.

**Total Phase 2: ~8-9 days** (~2 calendar weeks with testing).

---

## Phase 3: Discovery Improvements

### 3.1 Enhanced `blueprint.read` Output

#### Function Node Counts

**Problem:** `blueprint.read` lists function names but not how many nodes each function graph contains. The AI cannot distinguish between an empty stub (0-1 nodes) and a fully-implemented function (20+ nodes) without calling `read` again with `section="graph"`.

**File:** `Source/OliveAIEditor/Blueprint/Private/Reader/OliveBlueprintReader.cpp`
**Function:** `ReadGraphNames()` (line 957)

**Fix:** Change `ReadGraphNames` to also count nodes per graph:

```cpp
void FOliveBlueprintReader::ReadGraphNames(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const
{
    if (!Blueprint) return;

    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph)
        {
            FOliveIRGraphSummary Summary;
            Summary.Name = Graph->GetName();
            Summary.NodeCount = Graph->Nodes.Num();
            OutIR.EventGraphSummaries.Add(MoveTemp(Summary));
            OutIR.EventGraphNames.Add(Graph->GetName()); // backward compat
        }
    }

    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph)
        {
            FOliveIRGraphSummary Summary;
            Summary.Name = Graph->GetName();
            Summary.NodeCount = Graph->Nodes.Num();
            OutIR.FunctionSummaries.Add(MoveTemp(Summary));
            OutIR.FunctionNames.Add(Graph->GetName()); // backward compat
        }
    }

    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        if (Graph)
        {
            OutIR.MacroNames.Add(Graph->GetName());
        }
    }
}
```

**New IR struct** (in `CommonIR.h`):
```cpp
USTRUCT()
struct FOliveIRGraphSummary
{
    GENERATED_BODY()

    UPROPERTY()
    FString Name;

    UPROPERTY()
    int32 NodeCount = 0;
};
```

**New fields on `FOliveIRBlueprint`** (in `BlueprintIR.h`):
```cpp
UPROPERTY()
TArray<FOliveIRGraphSummary> EventGraphSummaries;

UPROPERTY()
TArray<FOliveIRGraphSummary> FunctionSummaries;
```

The existing `EventGraphNames` and `FunctionNames` arrays are kept for backward compat. New code should use the summaries.

**JSON output change (additive):**
```json
{
  "functions": [
    { "name": "SetGunInRange", "node_count": 0 },
    { "name": "Fire", "node_count": 12 },
    { "name": "Reload", "node_count": 7 }
  ]
}
```

This lets the AI see at a glance which functions are empty stubs vs implemented.

---

#### Compile Status in Read Output

**Problem:** The AI cannot learn about compile errors without calling `blueprint.compile`. If the last compile had errors, that information should be visible in `blueprint.read`.

**File:** `Source/OliveAIEditor/Blueprint/Private/Reader/OliveBlueprintReader.cpp`
**Function:** `ReadCompilationStatus()` (already exists)

**Check:** Verify that `ReadCompilationStatus` returns the last compile result including errors. If it only returns a status enum (e.g., `UpToDate`, `Dirty`, `Error`), enhance it to include the error messages from `Blueprint->CompilerResultsLog` or equivalent.

If `Blueprint->Status == BS_Error`, read the compile log entries and include them:

```cpp
void FOliveBlueprintReader::ReadCompilationStatus(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const
{
    // ... existing status reading ...

    // NEW: Include compile error messages when status is Error
    if (Blueprint->Status == BS_Error)
    {
        for (const TSharedRef<FTokenizedMessage>& Message : Blueprint->CompilerResultsLog)
        {
            if (Message->GetSeverity() == EMessageSeverity::Error)
            {
                OutIR.CompileErrors.Add(Message->ToText().ToString());
            }
        }
    }
}
```

**New field on `FOliveIRBlueprint`:**
```cpp
UPROPERTY()
TArray<FString> CompileErrors;
```

---

### 3.2 New Tool: `blueprint.describe_node_type`

**Purpose:** Given a node type string (e.g., `"K2Node_ComponentBoundEvent"`, `"CallFunction"`, `"Branch"`), return a description of what the node does, what properties it requires, and what pins it will have.

This is a **read-only discovery tool** that helps the AI plan before creating nodes.

**Tool name:** `blueprint.describe_node_type`
**Category:** `blueprint`, `read`

**Schema:**
```json
{
  "type": { "type": "string", "description": "Node type to describe (curated name or UK2Node class name)" }
}
```

**Response:**
```json
{
  "type": "K2Node_ComponentBoundEvent",
  "display_name": "Component Bound Event",
  "description": "An event bound to a specific component's delegate (e.g., OnComponentHit, OnComponentBeginOverlap)",
  "required_properties": {
    "DelegatePropertyName": "The delegate to bind to (e.g., 'OnComponentHit')",
    "ComponentPropertyName": "The component variable name (e.g., 'CollisionComp')"
  },
  "typical_pins": {
    "output_exec": "then",
    "output_data": ["HitComponent", "OtherActor", "OtherComp", "NormalImpulse", "Hit"]
  },
  "notes": "Pins depend on the delegate signature. Use get_node_pins after creation to see actual pins."
}
```

**Implementation:** Uses `FOliveNodeCatalog` for curated types and `FindK2NodeClass` for universal types. For universal types, instantiate a temporary node, call `AllocateDefaultPins`, read the pin manifest, then discard. This is safe because the node is never added to a graph.

**File:** New handler in `OliveBlueprintToolHandlers.cpp`, new schema in `OliveBlueprintSchemas.cpp`.

---

### 3.3 Error Message Improvements

Every error message that currently says "not found" should include what was searched and suggest alternatives.

#### Pattern: "Searched X, Y, Z. Did you mean?"

**Current:**
```
Function 'Interact' not found in parent class
```

**New:**
```
Function 'Interact' not found. Searched: parent class 'Actor', interfaces: 'BPI_Interactable'.
Suggestion: Use blueprint.override_function with function_name='Interact' (found in interface 'BPI_Interactable').
```

**Current:**
```
Interface 'BPI_Interactable' not found
```

**New:**
```
Interface 'BPI_Interactable' not found by short name.
Suggestion: Use the full path '/Game/Interfaces/BPI_Interactable', or search with project.search(query='BPI_Interactable').
```

**Current:**
```
FindFunction('GetMesh', class=''): FAILED
```

**New:**
```
Function 'GetMesh' not found. Searched: Blueprint class, parent hierarchy (Actor), SCS components,
11 library classes. Suggestion: Specify target_class='Character' (GetMesh is defined on ACharacter).
```

#### Implementation Approach

Each error site needs a `BuildSearchedLocationsString()` helper that collects where the search looked. This is most impactful in:

1. `FOliveNodeFactory::FindFunction()` -- return a structured result with search history
2. `FOliveBlueprintWriter::OverrideFunction()` -- include interface list in error
3. `FOliveBlueprintWriter::FindInterfaceClass()` -- suggest asset registry search
4. `OliveBlueprintPlanResolver::ResolveCallOp()` -- include resolver notes in error

**New struct for FindFunction:**
```cpp
struct FOliveFunctionSearchResult
{
    UFunction* Function = nullptr;
    EMatchMethod MatchMethod = EMatchMethod::Exact;
    FString MatchedClassName;
    TArray<FString> SearchedLocations; // For error messages

    bool IsValid() const { return Function != nullptr; }
};
```

Replace the current `UFunction*` return with this struct. Callers that only need the function pointer use `Result.Function`.

---

### 3.4 Phase 3 Implementation Order

1. **Function node counts in ReadGraphNames** -- 1 hour. IR struct addition + reader change.
2. **Compile status enhancement** -- 1 hour. Read compile errors into IR.
3. **`blueprint.describe_node_type` tool** -- 3-4 hours. New handler, schema, catalog integration.
4. **Error message improvements** -- 2-3 hours per error site. Can be done incrementally.

**Total Phase 3: Ongoing.** Items 1-3 are discrete tasks (~1 day). Item 4 is a continuous improvement effort.

---

## Risk Assessment

### Phase 1 Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| FN-4 (interface message) creates wrong node type | Medium | Medium | The coder must verify `UK2Node_Message` behavior in UE 5.5. If `UK2Node_CallFunction` with interface scope works, use that instead. |
| Removing `FOliveAssetExistsRule` causes different error messages | Low | Low | Handler errors are already structured (`ASSET_NOT_FOUND` code). AI self-correction works on error codes, not message text. |
| Relaxing `FOliveDuplicateLayerRule` causes accidental shadowing | Low | Low | The AI already handles shadowing correctly in most cases. The warning still appears in tool output. |
| Removing tool pack filtering exposes all tools to context | Medium | Low | Claude Code MCP Tool Search handles 95% token reduction. For non-CC clients, focus profiles still filter. |

### Phase 2 Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Tool consolidation breaks existing MCP client scripts | High | Medium | Alias map provides backward compat for one release cycle. Aliases logged as deprecation warnings. |
| `blueprint.read` with `section` param becomes too complex | Medium | Medium | Each section handler is a clean function call. The router is a switch statement. |
| Removing `project.batch_write` breaks workflows that depend on atomic multi-tool execution | Low | Medium | Plan-JSON is atomic. For non-Blueprint, individual operations with snapshot/rollback provide atomicity. |
| `behaviortree.add_node` unified tool confuses the AI about which params are needed | Medium | Medium | Use `required_if` in schema validation: `task_class` required when `node_kind="task"`, etc. |

### Phase 3 Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `describe_node_type` temporary node instantiation causes side effects | Low | High | Use `NewObject<UK2Node>(GetTransientPackage())` -- transient package ensures no persistence. Verify no global registration. |
| Compile errors in `blueprint.read` output cause token bloat | Low | Low | Cap at 10 errors in the IR output. |

### Rollback Strategy

Every phase can be rolled back independently:
- **Phase 1:** Each FN fix is a single function change. Revert the commit.
- **Phase 2:** The alias map provides full backward compat. To roll back, re-register the old tools alongside the new ones (both active simultaneously) and remove the alias map.
- **Phase 3:** All changes are additive (new fields, new tool). Rollback = delete the new code.

Snapshot/rollback (`project.snapshot` / `project.rollback`) continues to work throughout all phases for asset-level rollback.

---

## File Change Summary

### Phase 1 Files

| File | Change Type | Description |
|------|------------|-------------|
| `Blueprint/Private/Writer/OliveBlueprintWriter.cpp` | MODIFY | FN-1 (interface search in OverrideFunction), FN-2 (asset registry fallback in FindInterfaceClass) |
| `Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | MODIFY | FN-3 (EventNameMap additions) |
| `Blueprint/Private/Writer/OliveNodeFactory.cpp` | MODIFY | FN-4 (interface search in FindFunction), FN-5 (FunctionGraphs scan in FindFunction) |
| `Blueprint/Public/Writer/OliveNodeFactory.h` | MODIFY | FN-4 (add InterfaceSearch to EMatchMethod enum) |
| `Blueprint/Private/Plan/OlivePlanExecutor.cpp` | MODIFY | FN-4 (interface message node creation in PhaseCreateNodes), FN-6 (bSuccess wiring fix) |
| `Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | MODIFY | Error message improvements for override_function |
| `Content/Templates/reference/pickup_interaction.json` | MODIFY | Add note about using existing player character BP |
| `Private/Services/OliveValidationEngine.cpp` | MODIFY | Remove 3 rules, relax 4 rules |
| `Content/SystemPrompts/Knowledge/recipe_routing.txt` | MODIFY | Remove recipe mandate, remove 12-step cap |
| `Content/SystemPrompts/Knowledge/blueprint_authoring.txt` | MODIFY | Remove batch-first rule |
| `Content/SystemPrompts/Knowledge/node_routing.txt` | MODIFY | Remove path hierarchy language |
| `Private/Providers/OliveCLIProviderBase.cpp` | MODIFY | Remove recipe mandate from BuildSystemPrompt and BuildContinuationPrompt |
| `Private/OliveAIEditorModule.cpp` | MODIFY | Remove ToolPackManager initialization |
| `Private/Brain/OliveToolPackManager.cpp` | MODIFY | Mark deprecated |

### Phase 2 Files

| File | Change Type | Description |
|------|------------|-------------|
| `Public/MCP/OliveToolRegistry.h` | MODIFY | Add tool alias map |
| `Private/MCP/OliveToolRegistry.cpp` | MODIFY | Implement alias resolution in FindTool/DispatchTool |
| `Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | MODIFY | Consolidate read handlers (section router), consolidate add_function handler |
| `Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` | MODIFY | Update schemas for consolidated tools |
| `BehaviorTree/Private/MCP/OliveBTToolHandlers.cpp` | MODIFY | Consolidate add_* handlers into add_node |
| `CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp` | MODIFY | Remove deprecated tools |
| `Private/MCP/OliveToolRegistry.cpp` | MODIFY | Remove deprecated project tool registrations |

### Phase 3 Files

| File | Change Type | Description |
|------|------------|-------------|
| `OliveAIRuntime/Public/IR/CommonIR.h` | MODIFY | Add FOliveIRGraphSummary struct |
| `OliveAIRuntime/Public/IR/BlueprintIR.h` | MODIFY | Add graph summaries and compile errors to FOliveIRBlueprint |
| `Blueprint/Private/Reader/OliveBlueprintReader.cpp` | MODIFY | ReadGraphNames enhancement, ReadCompilationStatus enhancement |
| `Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | MODIFY | New describe_node_type handler |
| `Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` | MODIFY | New describe_node_type schema |
| `Blueprint/Private/Writer/OliveNodeFactory.cpp` | MODIFY | Return FOliveFunctionSearchResult instead of UFunction* |
| `Blueprint/Public/Writer/OliveNodeFactory.h` | MODIFY | FOliveFunctionSearchResult struct definition |

---

## Summary for the Coder

**Phase 1 is the priority.** The 5 false negatives are causing real failures in production use. Start with FN-3 (trivial alias addition), then FN-2 and FN-1 (asset registry and interface lookups). FN-5 may already work if `ResolveCallOp` correctly passes the Blueprint to `FindFunction` -- verify first. FN-4 is the most complex and can be tackled last.

The validation rule changes are mechanical -- change `AddError` to `AddWarning` or remove `RegisterRule` calls per the audit table. The prompt changes are text edits.

**Phase 2 is the largest effort** but the alias map infrastructure makes it safe. Build the alias map first, then consolidate one tool category at a time. The `blueprint.read` consolidation is the biggest single task (7 handlers -> 1 router).

**Phase 3 is incremental.** The node count enhancement and compile status are quick wins. `describe_node_type` is a nice-to-have. Error message improvements are ongoing.
