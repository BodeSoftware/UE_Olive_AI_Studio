# Resolver Removal + Template Check Rule

Two changes: (1) remove `FOliveFunctionResolver` from the plan pipeline and consolidate function lookup into `FOliveNodeFactory::FindFunction`, (2) add a "check templates before modifying" rule to AGENTS.md and the autonomous sandbox CLAUDE.md.

---

## Change 1: Remove FOliveFunctionResolver from the Plan Pipeline

### Problem Summary

`FOliveFunctionResolver::Resolve()` sits between the AI's plan and the node factory. When it cannot find a function, it "accepts as-is" and reports the plan as resolved successfully. The node factory then fails at execution time after partial nodes have been created, triggering expensive rollbacks. In the pickup task, this caused 7 plan failures and 3 batch rollbacks. The resolver provides no value the factory cannot provide -- except K2 prefix handling and the alias map, which are trivially movable.

### Key Insight: ResolvedOwningClass, bIsPure, and bIsLatent

The resolver currently populates three fields on `FOliveResolvedStep` that are consumed downstream:

1. **`ResolvedOwningClass`** -- Used by `FOlivePlanValidator::CheckComponentFunctionTargets()` to detect component-only functions (lines 141-205 of OlivePlanValidator.cpp), and by `FOlivePlanExecutor::PhaseAutoWireComponents()` (Phase 1.5, lines 765-879 of OlivePlanExecutor.cpp) to auto-wire component targets.

2. **`bIsPure`** -- Used by `CollapseExecThroughPureSteps()` to remove pure nodes from exec chains. For call ops, currently set from `Match.Function->HasAnyFunctionFlags(FUNC_BlueprintPure)`. For non-call ops, set statically by the resolver.

3. **`bIsLatent`** -- Used by `FOlivePlanValidator` to reject latent functions in function graphs. Set from `Match.Function->HasMetaData("Latent")`.

After removing the resolver, **ResolveCallOp must still populate these three fields.** The simplest path: use `FOliveNodeFactory::FindFunction()` directly in ResolveCallOp (it already searches the same class lists), then introspect the returned `UFunction*` for flags.

### Files Affected

| File | Action |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | **Major edit**: rewrite `ResolveCallOp()` |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | **Edit**: add K2 prefix fallback + alias lookup to `FindFunction()` |
| `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h` | **Edit**: add `static const TMap<FString, FString>& GetAliasMap()` as public method |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp` | **Keep but do not modify**. Not deleted yet -- kept as dead code for one release cycle. |
| `Source/OliveAIEditor/Blueprint/Public/Plan/OliveFunctionResolver.h` | **Keep but do not modify**. Same reason. |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | **Edit**: remove `#include "Plan/OliveFunctionResolver.h"` (line 38) |

### Detailed Changes

#### T1: Move alias map and K2 prefix fallback into `FOliveNodeFactory::FindFunction()`

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`

**Current `FindFunction()` behavior (lines 1455-1521):**
1. Search specified ClassName
2. Search Blueprint->GeneratedClass
3. Search 11 common library classes
4. If all fail, return nullptr

**New `FindFunction()` behavior:**
1. **Alias lookup first** -- before any class search, check the alias map. If the function name maps to an alias, replace `FunctionName` with the alias value for all subsequent searches. This ensures `GetLocation` -> `K2_GetActorLocation` works immediately.
2. Search specified ClassName (exact, then K2 prefix)
3. Search Blueprint->GeneratedClass (exact, then K2 prefix)
4. **Search Blueprint parent class hierarchy** (exact, then K2 prefix) -- the resolver had this via `GetSearchOrder()`, but the factory does not. Add it.
5. **Search Blueprint SCS component classes** (exact, then K2 prefix) -- same reason.
6. Search 11 common library classes (exact, then K2 prefix)
7. If all fail, return nullptr

**K2 prefix logic to add** (inline in FindFunction, not a separate method):
```
// After each FindFunctionByName(FName(*FunctionName)) fails on a given class:
// Try K2_ prefix if not already present
if (!FunctionName.StartsWith(TEXT("K2_")))
{
    Func = Class->FindFunctionByName(FName(*(TEXT("K2_") + FunctionName)));
}
// Try removing K2_ prefix if present
else
{
    Func = Class->FindFunctionByName(FName(*FunctionName.Mid(3)));
}
```

Apply this to every class in the search order, not just as a separate pass. This means each class gets tried with exact name first, then K2 variant, before moving to the next class.

**New includes needed in OliveNodeFactory.cpp:**
- `#include "Engine/SimpleConstructionScript.h"` (already present at line 36)
- `#include "Engine/SCS_Node.h"` (already present at line 37)
- `#include "Components/ActorComponent.h"` -- for `UActorComponent::StaticClass()` in parent class walk (check if already included transitively via Actor.h; if not, add explicitly)
- `#include "GameFramework/CharacterMovementComponent.h"` -- NOT needed, the resolver had it but it was only used in SCS iteration which iterates dynamically

**Note:** The resolver also had `UObject::StaticClass()` in its common classes list. The factory already has this in its library classes array (line 1496 from the read output? No -- checking lines 1490-1502: KismetSystemLibrary, KismetMathLibrary, KismetStringLibrary, KismetArrayLibrary, GameplayStatics, UObject, Actor, SceneComponent, PrimitiveComponent, Pawn, Character). Good, `UObject::StaticClass()` is already there.

#### T2: Add alias map to `FOliveNodeFactory`

**File:** `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h`

Add a new public static method:
```cpp
/**
 * Get the function name alias map.
 * Maps common AI-provided names to actual UE function names.
 * Used as the first step in FindFunction() resolution.
 * @return Static alias map (case-insensitive lookup recommended)
 */
static const TMap<FString, FString>& GetAliasMap();
```

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`

Copy the entire `GetAliasMap()` implementation verbatim from `OliveFunctionResolver.cpp` (lines 734-989). This is a ~250-line static function that returns a `static const TMap<FString, FString>`. No modification needed to the map contents.

Place it after `FindFunction()` and before `FindStruct()`.

#### T3: Rewrite `ResolveCallOp()` in `OliveBlueprintPlanResolver.cpp`

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

**Current behavior (lines 1083-1226):**
1. Call `FOliveFunctionResolver::Resolve()` to get `FOliveFunctionMatch`
2. If match found: set `function_name`, `target_class`, `ResolvedOwningClass`, `bIsPure`, `bIsLatent` from the match
3. If match not found but no TargetClass: accept as-is (the "lying" path)
4. If match not found with TargetClass: error with suggestions from `GetCandidates()`

**New behavior:**
1. Validate `Step.Target` is non-empty (keep existing check)
2. Call `FOliveNodeFactory::Get().FindFunction(Step.Target, Step.TargetClass, BP)` directly
3. If `UFunction*` found:
   - Set `function_name` = `Function->GetName()` (canonical name, which includes K2_ prefix if that is the real name)
   - Set `target_class` = `Function->GetOwnerClass()->GetName()`
   - Set `ResolvedOwningClass` = `Function->GetOwnerClass()`
   - Set `bIsPure` = `Function->HasAnyFunctionFlags(FUNC_BlueprintPure)`
   - Set `bIsLatent` = `Function->HasMetaData(TEXT("Latent"))`
   - Log a resolver note if the canonical name differs from `Step.Target` (so the AI learns the correct name)
   - Return true
4. If `UFunction*` NOT found:
   - **Error unconditionally** -- no more "accepted as-is" path
   - Error code: `FUNCTION_NOT_FOUND`
   - Suggestion: use node catalog fuzzy match to suggest alternatives (keep the existing `FOliveNodeCatalog::Get().FuzzyMatch()` call)
   - Return false

**What this eliminates:**
- The entire "accepted as-is" path (lines 1185-1226)
- All references to `FOliveFunctionResolver::Resolve()`, `GetCandidates()`, `MatchMethodToString()`
- The `FOliveFunctionMatch` struct usage in this file

**What this preserves:**
- `ResolvedOwningClass` still populated (from `Function->GetOwnerClass()`)
- `bIsPure` still populated (from `FUNC_BlueprintPure` flag)
- `bIsLatent` still populated (from `Latent` metadata)
- Error reporting with alternatives (using catalog fuzzy match instead of resolver candidates)

#### T4: Remove dead includes

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
- Remove `#include "Plan/OliveFunctionResolver.h"` (line 15)
- Already includes `Writer/OliveNodeFactory.h` (line 13) -- no new include needed

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
- Remove `#include "Plan/OliveFunctionResolver.h"` (line 38)
- No other code in this file references `FOliveFunctionResolver` or `FOliveFunctionMatch`

### Implementation Order

1. **T2 first** -- Add `GetAliasMap()` to NodeFactory .h and .cpp (pure addition, no behavior change)
2. **T1 second** -- Enhance `FindFunction()` with alias lookup, K2 prefix fallback, parent class hierarchy, SCS component classes. This is the most complex change but is still additive (existing callers get better resolution for free).
3. **T3 third** -- Rewrite `ResolveCallOp()` to use `FindFunction()` directly instead of `FOliveFunctionResolver::Resolve()`. This is the breaking change where the "accepted as-is" path disappears.
4. **T4 last** -- Remove dead includes. Trivial cleanup.

### Verification

After all changes, build and verify:

1. **Compile succeeds** -- the removed include must not cause unresolved symbols
2. **Gun template task** -- `K2_SetTimerByFunctionName`, `SetActorLocation`, `DestroyActor` should all resolve via the enhanced `FindFunction()`
3. **Blueprint-defined functions** -- custom functions created via `add_function` must still be found (this was always handled by the GeneratedClass search in FindFunction, which is preserved)
4. **Library functions** -- `PrintString`, `MakeVector`, `Lerp` etc. must still be found (library class search preserved)
5. **K2 prefix functions** -- `GetRootComponent` must find `K2_GetRootComponent` (new K2 prefix fallback in FindFunction)
6. **Alias functions** -- `GetLocation` must find `K2_GetActorLocation` (new alias lookup in FindFunction)
7. **Component functions** -- `SetSpeed` on a Blueprint with ProjectileMovementComponent must find `UProjectileMovementComponent::SetSpeed` (new SCS component class search in FindFunction)
8. **Unknown functions fail immediately** -- a made-up function name like `DoTheThing` must produce `FUNCTION_NOT_FOUND` error at resolution time, NOT succeed and fail at execution time

### What NOT to Do

- **Do NOT delete OliveFunctionResolver.h/.cpp yet.** Keep them as dead code for one release cycle. They compile, they just are not called. This preserves the ability to revert if the change causes unexpected regressions.
- **Do NOT change `FOliveResolvedStep` struct.** The `ResolvedOwningClass`, `bIsPure`, and `bIsLatent` fields remain and are still populated by the rewritten `ResolveCallOp()`.
- **Do NOT change `FOlivePlanValidator` or `FOlivePlanExecutor`.** They consume `FOliveResolvedStep` unchanged.
- **Do NOT remove the `CATALOG_SEARCH_LIMIT` constant** in the anonymous namespace of OliveBlueprintPlanResolver.cpp -- it is still used for fuzzy match suggestions in the error path.

---

## Change 2: Template Check Rule for Modify Tasks

### Problem

The AI sometimes implements behavior from scratch on existing Blueprints without checking whether a reference template already documents the correct pattern. This wastes tokens and produces worse results than following a known-good architectural pattern.

### What to Add

A single rule, added in two places:

> **Before implementing behavior on any existing Blueprint, call `blueprint.list_templates` to check for matching reference templates. If one matches the task domain (e.g., projectile, pickup, component patterns), call `blueprint.get_template` to read the architecture guidance before writing any plan_json.**

### Files Affected

| File | Action |
|------|--------|
| `AGENTS.md` | Add rule to the Blueprint Templates section (after line 271) |
| `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | Add rule to the Critical Rules section in `SetupAutonomousSandbox()` |

### Detailed Changes

#### T5: Add rule to AGENTS.md

**File:** `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/AGENTS.md`

Insert after the "Reference templates that violate these rules must be rewritten before merging." line (line 271), before the blank line and `### Safety Presets`:

```markdown

**Modify-task template check (MUST follow):**
- Before implementing any behavior on an existing Blueprint, call `blueprint.list_templates` to check for matching reference templates. If one matches the task domain (e.g., projectile, pickup, component patterns, UE events), call `blueprint.get_template` to read the architecture guidance before writing any plan_json.
```

#### T6: Add rule to sandbox CLAUDE.md

**File:** `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

In the `SetupAutonomousSandbox()` function, add a new line after line 371 (the `schema_version "2.0"` rule) and before line 372 (the template creation rule about checking function stubs):

```cpp
ClaudeMd += TEXT("- Before implementing behavior on an existing Blueprint, call `blueprint.list_templates` to check for matching reference templates. If one matches, call `blueprint.get_template` to read the pattern before writing plan_json.\n");
```

This places it in the Critical Rules section, between the schema_version rule and the post-template rule, which is the natural position for a "check templates first" instruction.

### Implementation Order

5. **T5** -- Edit AGENTS.md (no compilation needed)
6. **T6** -- Edit OliveCLIProviderBase.cpp (requires compilation)

Both T5 and T6 are independent of T1-T4 and can be done in parallel.

### Verification

1. **AGENTS.md** -- Read the file and confirm the rule appears in the Blueprint Templates subsection
2. **Sandbox CLAUDE.md** -- Launch the plugin, trigger an autonomous run, and read `Saved/OliveAI/AgentSandbox/CLAUDE.md` to confirm the rule appears in the Critical Rules section
3. **Build succeeds** -- the new string literal in SetupAutonomousSandbox compiles cleanly

---

## Full Implementation Order

| Order | Task | File | Depends On |
|-------|------|------|------------|
| 1 | T2: Add GetAliasMap to NodeFactory | OliveNodeFactory.h + .cpp | None |
| 2 | T1: Enhance FindFunction | OliveNodeFactory.cpp | T2 |
| 3 | T3: Rewrite ResolveCallOp | OliveBlueprintPlanResolver.cpp | T1 |
| 4 | T4: Remove dead includes | OliveBlueprintPlanResolver.cpp, OliveBlueprintToolHandlers.cpp | T3 |
| 5 | T5: AGENTS.md template rule | AGENTS.md | None |
| 6 | T6: Sandbox CLAUDE.md template rule | OliveCLIProviderBase.cpp | None |

T5 and T6 can run in parallel with T1-T4. Total estimated time: ~3 hours for an experienced coder.

---

## Risk Assessment

**Low risk:**
- T2 (additive -- new static method, no behavior change)
- T4 (dead include removal)
- T5, T6 (documentation/prompt changes)

**Medium risk:**
- T1 (enhancing FindFunction) -- adding parent class hierarchy and SCS component search could theoretically find functions the old code did not. However, these are the SAME classes the resolver was already searching, so results should be identical or better.

**High risk:**
- T3 (removing the "accepted as-is" fallback) -- this is the intentional breaking change. Functions that previously passed resolution and failed at execution will now fail at resolution. The AI gets a clearer error earlier, but plans that previously "sometimes worked" (because the factory happened to find the function even though the resolver did not) now consistently fail at resolution. **This is the desired outcome.** The factory's FindFunction (now enhanced with T1) should find everything the resolver found, plus more.

### Rollback Plan

If T3 causes regressions, the coder can revert just the ResolveCallOp changes and re-add the `#include "Plan/OliveFunctionResolver.h"` line. The resolver files are intentionally kept as dead code for this purpose. T1 and T2 (FindFunction enhancements) are purely beneficial and should be kept regardless.
