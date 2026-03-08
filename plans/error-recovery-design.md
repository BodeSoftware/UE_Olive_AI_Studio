# Error Recovery Design: Class-Scoped Suggestions + Progressive Self-Correction + Component API Map

## Problem Statement

When the Builder calls `apply_plan_json` with a `call` op like `SetSpeed` targeting `ProjectileMovementComponent`, `FindFunctionEx` fails and the current error path produces fuzzy suggestions from ALL classes in `AllSearchedClasses` (e.g., "Did you mean: SetSphereRadius (SphereComponent)?"). The Builder wastes 3-4 turns guessing before self-correcting or giving up.

Three root causes:

1. **Wrong-class fuzzy matches**: The fuzzy suggestion pool in `FindFunction()` iterates ALL classes searched (parent hierarchy, SCS components, library classes) and scores by string similarity alone. A function on `SphereComponent` can outscore the correct property on `ProjectileMovementComponent`.

2. **Vague self-correction guidance**: `FUNCTION_NOT_FOUND` handler in `OliveSelfCorrectionPolicy.cpp` is one line: "Use blueprint.search_nodes to find the correct function name." This does not tell the Builder to look at the class-scoped alternatives already in the error, nor does it escalate on repeated failures.

3. **No upfront API surface**: The Builder starts with no knowledge of what functions/properties exist on the components in its Build Plan. It relies on the AI's general UE knowledge, which is often wrong for specific component APIs (e.g., knowing `MaxSpeed` exists but calling it `SetSpeed`).

## Three Fixes

---

### Fix 1: Class-Scoped Function and Property Suggestions in FUNCTION_NOT_FOUND

**Goal**: When `FindFunctionEx` fails and a target class is known, replace the cross-class fuzzy match with class-scoped function AND property suggestions. Detect property-name matches and tell the Builder to use `set_var`/`get_var`.

#### Where to Change

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**Location**: `ResolveCallOp()` lines 1707-1757 (the "Function NOT found" error block)

**File**: `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`
**Location**: New static helper function. NOT a member of `FOliveNodeFactory` (avoid coupling). Place as a free function in an anonymous namespace in `OliveBlueprintPlanResolver.cpp`.

#### Design

Add a new helper function in the anonymous namespace of `OliveBlueprintPlanResolver.cpp`:

```cpp
namespace
{
    /**
     * Collect class-scoped function and property suggestions for a FUNCTION_NOT_FOUND error.
     * Scans the given class (and its parent hierarchy up to UObject) for:
     * - BlueprintCallable/BlueprintPure functions similar to SearchName
     * - BlueprintVisible/BlueprintReadOnly properties similar to SearchName
     *
     * Returns a formatted suggestion string with function matches and property
     * matches (the latter flagged as "use set_var/get_var, not call").
     *
     * @param TargetClass       The UClass to search (e.g., UProjectileMovementComponent)
     * @param SearchName        The function name the AI tried (e.g., "SetSpeed")
     * @param MaxFunctions      Maximum function suggestions to return (default 5)
     * @param MaxProperties     Maximum property suggestions to return (default 3)
     * @return Formatted suggestion string, empty if TargetClass is nullptr
     */
    FString BuildClassScopedSuggestions(
        UClass* TargetClass,
        const FString& SearchName,
        int32 MaxFunctions = 5,
        int32 MaxProperties = 3);
}
```

**Algorithm**:

1. Resolve `TargetClass` from `Step.TargetClass` (using the same `FindClass()` path already used in `ResolveCallOp`). If the step has no `target_class`, try SCS component resolution: iterate `BP->SimpleConstructionScript->GetAllNodes()`, find any component class where the function name is a plausible match (substring of a property name or function name). Use the first match. If no class can be resolved, fall back to the existing catalog fuzzy match (current behavior unchanged).

2. **Function scan**: `TFieldIterator<UFunction>(TargetClass, EFieldIteratorFlags::IncludeSuper)`. Filter to `FUNC_BlueprintCallable | FUNC_BlueprintPure`. Skip functions whose names start with `DEPRECATED_`, `Internal_`, `PostEditChange`, `Serialize`, `BeginDestroy`, `EndPlay` (implementation detail functions). Score using the same 3-criteria scoring already in `FindFunction` (substring containment, common prefix, camel-case word overlap). Collect top `MaxFunctions`.

3. **Property scan**: `TFieldIterator<FProperty>(TargetClass, EFieldIteratorFlags::IncludeSuper)`. Filter to properties with `CPF_BlueprintVisible` or `CPF_BlueprintReadOnly`. Skip properties starting with `bNet`, `Rep_`, `DEPRECATED_`. Score by same similarity algorithm. Collect top `MaxProperties`.

4. **Property-function cross-match**: For each top property, check if the AI's `SearchName` contains the property name or vice versa (case-insensitive). If so, flag it prominently: `"MaxSpeed is a float property on ProjectileMovementComponent -- use set_var to write it, get_var to read it."` This catches the `SetSpeed` -> `MaxSpeed` pattern.

5. **Format**: Build a compact suggestion string:
   ```
   Available on ProjectileMovementComponent:
   Functions: StopMovementImmediately(), SetVelocityInLocalSpace(NewVelocity), MoveUpdatedComponent(Delta, NewRotation, bSweep), ...
   Properties: MaxSpeed (float), InitialSpeed (float), ProjectileGravityScale (float), Velocity (FVector), ...
   Likely fix: 'MaxSpeed' is a property (float), not a function. Use op:set_var target:MaxSpeed instead of op:call target:SetSpeed.
   ```

6. **Integration in ResolveCallOp**: Replace the existing catalog fuzzy match block (lines 1710-1719) with a conditional:

```
// Pseudocode for the replacement logic:
if (TargetClass can be resolved from Step.TargetClass or SCS scan)
{
    Suggestion = BuildClassScopedSuggestions(TargetClass, Step.Target);
    // Also populate Error.Alternatives with function names only (for dedup detection)
}
else
{
    // No class context -- fall back to existing catalog fuzzy match
    // (current code at lines 1710-1719, unchanged)
}
```

The `ErrorMessage` construction at line 1722 stays the same (already includes `SearchResult.BuildSearchedLocationsString()`). The `Suggestion` string (line 1727-1746) is replaced with the output of `BuildClassScopedSuggestions()`, followed by the existing target_class/omit-target_class advice.

#### What Does NOT Change

- `FindFunction()` and `FindFunctionEx()` in `OliveNodeFactory.cpp` are untouched. The fuzzy suggestions there remain for the UE_LOG output and for `LastFuzzySuggestions` (used by `add_node` codepath). The change is only in how `ResolveCallOp` constructs the error's `Suggestion` field.
- The `FOliveIRBlueprintPlanError` struct is unchanged.
- The `Error.Alternatives` array still gets populated (with class-scoped function names instead of catalog matches).

#### Edge Cases

- **No target_class and no SCS components**: Falls back to existing catalog fuzzy match. No regression.
- **TargetClass resolves but has 0 BlueprintCallable functions**: Return empty function list, still show properties. If both are empty, fall back to catalog fuzzy match.
- **The AI provides target_class but it's a typo** (e.g., `ProjectileMovement` instead of `ProjectileMovementComponent`): `FindClass()` already handles this with prefix stripping in the resolver. If it fails, we fall back to catalog fuzzy match.
- **Function exists but not as BlueprintCallable**: The scan skips it, so the AI won't see internal-only functions. This is correct.

---

### Fix 2: Upgrade Self-Correction Policy for FUNCTION_NOT_FOUND

**Goal**: Replace the single-line guidance with progressive, attempt-aware recovery instructions that reference the class-scoped suggestions from Fix 1.

#### Where to Change

**File**: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`
**Location**: `BuildToolErrorMessage()`, the `FUNCTION_NOT_FOUND` branch (line 717-720)

#### Design

Replace:
```cpp
else if (ErrorCode == TEXT("FUNCTION_NOT_FOUND"))
{
    Guidance = TEXT("The function was not found. Use blueprint.search_nodes to find the correct function name. Check for K2_ prefixes and class membership.");
}
```

With attempt-progressive guidance:

```cpp
else if (ErrorCode == TEXT("FUNCTION_NOT_FOUND"))
{
    if (AttemptNum <= 1)
    {
        // Attempt 1: Direct the Builder to the class-scoped suggestions
        // already present in the error message (from Fix 1)
        Guidance = TEXT(
            "The function does not exist on the target class. "
            "Read the 'Available on [Class]' list in the error above carefully.\n"
            "Common mistake: the AI name may refer to a PROPERTY, not a function. "
            "Properties must use op:set_var (to write) or op:get_var (to read), "
            "not op:call.\n"
            "If the error lists matching properties, switch to set_var/get_var.");
    }
    else if (AttemptNum == 2)
    {
        // Attempt 2: Escalate to explicit read + search
        Guidance = TEXT(
            "Second attempt failed. The function truly does not exist.\n"
            "1. If the error showed matching properties, use set_var/get_var instead.\n"
            "2. Call blueprint.read(path, section=\"components\") to see all component "
            "types on the Blueprint.\n"
            "3. Use blueprint.search_nodes with the component class name to find "
            "available functions.\n"
            "Do NOT guess another function name.");
    }
    else
    {
        // Attempt 3+: Escalate to granular tools or Python
        Guidance = TEXT(
            "Multiple attempts have failed to find this function. "
            "The function likely does not exist in the form you expect.\n"
            "Options:\n"
            "1. Use set_var/get_var if the target is a property\n"
            "2. Use add_node with a specific UK2Node class name\n"
            "3. Use editor.run_python to accomplish this via Python scripting\n"
            "4. Skip this step and move on to the next part of the plan");
    }
}
```

#### What Does NOT Change

- The `BuildToolErrorMessage()` signature stays the same. `AttemptNum` is already passed in.
- The error category classification in `ClassifyErrorCode()` stays as-is (`FUNCTION_NOT_FOUND` is Category A: FixableMistake).
- No changes to `Evaluate()` or `FOliveCorrectionDecision`.

#### Edge Cases

- **First attempt with no class context** (no target_class, Fix 1 fell back to catalog match): Attempt 1 guidance still says "read the Available list" -- this is slightly misleading since the list will be catalog fuzzy matches. Acceptable because the guidance still tells the Builder about set_var/get_var as alternatives, which is the most common fix regardless.
- **The Builder hits FUNCTION_NOT_FOUND across multiple different functions**: Each gets its own error signature via `BuildToolErrorSignature()`, so attempt counts are per-signature. Guidance is correct per function.

---

### Fix 3: Component API Map Injection in Pipeline

**Goal**: Before the Builder starts, inject a compact listing of functions and properties for each SCS component that the Build Plan mentions. Like Aider's repo map -- the Builder knows what's callable before it tries.

#### Where to Change

**File**: `Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp`
**Location**: `FormatForPromptInjection()` -- add a new section between Section 3 (Build Plan) and Section 4 (Existing Asset Context).

**File**: `Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h`
**Location**: `FOliveArchitectResult` struct -- add a new field for the component API map.

#### Data Flow

```
ParseBuildPlan() -> OutResult.Components["BP_Projectile"] = [("ProjectileMovement", "ProjectileMovementComponent"), ...]
                                        |
                                        v
                     BuildComponentAPIMap(OutResult.Components)  [NEW]
                                        |
                                        v
                     OutResult.ComponentAPIMap = "### ProjectileMovementComponent\n..."
                                        |
                                        v
                     FormatForPromptInjection() -> Section 3.5: ## Component API Reference
```

#### Design

**New static method** on `FOliveAgentPipeline`:

```cpp
/**
 * Build a compact API reference for component classes mentioned in the Build Plan.
 * Enumerates BlueprintCallable functions (name + param names only) and
 * BlueprintVisible properties (name + type only) for each unique component class.
 *
 * Capped at ~3000 chars total. If budget exceeded, truncates per-class entries
 * with "... and N more" suffix.
 *
 * @param Components  Per-asset component map from ParseBuildPlan
 * @return Formatted markdown block, or empty string if no components resolved
 */
static FString BuildComponentAPIMap(
    const TMap<FString, TArray<TPair<FString, FString>>>& Components);
```

**Algorithm**:

1. Collect unique class names across all assets in `Components`. Deduplicate (e.g., if two assets both have `StaticMeshComponent`, emit it once).

2. For each unique class name:
   a. Resolve via `FindFirstObjectSafe<UClass>()` with `U` prefix (e.g., `"ProjectileMovementComponent"` -> `"UProjectileMovementComponent"`). Also try `FindClass()` pattern from the resolver.
   b. If resolution fails, skip with a log warning.
   c. Enumerate `BlueprintCallable` functions (same filter as Fix 1: skip `DEPRECATED_`, `Internal_`, etc.). For each function, format as `FuncName(Param1, Param2)` -- parameter names only, no types (keeps it compact).
   d. Enumerate `BlueprintVisible` properties. Format as `PropName (TypeName)`.
   e. Cap functions at 15 per class, properties at 10 per class.

3. Format per class:
   ```
   ### ProjectileMovementComponent
   Functions: StopMovementImmediately(), SetVelocityInLocalSpace(NewVelocity), MoveUpdatedComponent(Delta, NewRotation, bSweep), ...
   Properties: MaxSpeed (float), InitialSpeed (float), ProjectileGravityScale (float), Velocity (FVector), ...
   ```

4. Track total character count. When exceeding 3000 chars (configurable `static constexpr int32 API_MAP_BUDGET = 3000`), stop adding classes. Append `"... and N more component types omitted."` The 3000-char cap ensures the API map adds ~750-800 tokens at most to the Builder prompt.

5. If the total output is non-empty, wrap it:
   ```
   ## Component API Reference

   Use set_var/get_var for properties, call for functions.

   [per-class blocks]
   ```

**New field on `FOliveArchitectResult`**:

```cpp
/** Compact API map of component functions/properties for Builder context. */
FString ComponentAPIMap;
```

This field is populated right after `ParseBuildPlan()` returns, in the same call site (either `RunArchitect()` or `RunPlanner()`).

**Integration in `FormatForPromptInjection()`**:

After Section 3 (Build Plan + Validator Warnings) and before Section 4 (Existing Asset Context), add:

```cpp
// Section 3.5: Component API Reference (Aider-style repo map)
if (!Architect.ComponentAPIMap.IsEmpty())
{
    Output += Architect.ComponentAPIMap;
    Output += TEXT("\n");
}
```

This goes AFTER the Build Plan so the Builder sees the plan first (what to build), then the API reference (what functions/properties are available on the components it needs to use).

**Where `ComponentAPIMap` gets populated**:

In `RunArchitect()` (API path) and `RunPlanner()` (CLI path), both already call `ParseBuildPlan()`. After `ParseBuildPlan()` returns, add:

```cpp
Result.ComponentAPIMap = BuildComponentAPIMap(Result.Components);
```

#### Filtering Rules

Functions to SKIP (name prefix filter):
- `DEPRECATED_`
- `Internal_`
- `PostEditChange` (any variant)
- `Serialize`
- `BeginDestroy`
- `FinishDestroy`
- `PostInitProperties`
- `PostLoad`
- `AddReferencedObjects`
- `GetLifetimeReplicatedProps`
- `OnRep_` (replication callbacks -- noise for the Builder)

Functions to SKIP (flag filter):
- NOT `FUNC_BlueprintCallable` AND NOT `FUNC_BlueprintPure` -- skip
- `FUNC_EditorOnly` with `FUNC_Exec` but without `FUNC_BlueprintCallable` -- skip (these are console commands)

Properties to SKIP:
- NOT `CPF_BlueprintVisible` AND NOT `CPF_BlueprintReadOnly` -- skip
- `CPF_Deprecated` -- skip
- Name starts with `bRep_`, `Rep_`, or `DEPRECATED_` -- skip

The shared filter logic between Fix 1 and Fix 3 should be extracted into a common static helper. Place it as a free function in a new namespace or as a private static method visible to both call sites. Since Fix 1 lives in `OliveBlueprintPlanResolver.cpp` and Fix 3 lives in `OliveAgentPipeline.cpp`, the shared helper should go into a lightweight header.

#### Shared Helper Location

**New file**: `Source/OliveAIEditor/Blueprint/Public/Writer/OliveClassAPIHelper.h`
**New file**: `Source/OliveAIEditor/Blueprint/Private/Writer/OliveClassAPIHelper.cpp`

```cpp
// OliveClassAPIHelper.h
#pragma once

#include "CoreMinimal.h"

class UClass;

/**
 * Lightweight helper for enumerating the Blueprint-visible API surface of a UClass.
 * Used by:
 * - OliveBlueprintPlanResolver (class-scoped FUNCTION_NOT_FOUND suggestions)
 * - OliveAgentPipeline (component API map injection)
 *
 * All methods are static. No state.
 */
class OLIVEAIEDITOR_API FOliveClassAPIHelper
{
public:
    struct FFunctionEntry
    {
        FString Name;
        TArray<FString> ParamNames;  // Input parameter names only
        bool bIsPure = false;
    };

    struct FPropertyEntry
    {
        FString Name;
        FString TypeName;   // Human-readable (e.g., "float", "FVector", "bool")
    };

    /**
     * Enumerate Blueprint-callable functions on a class.
     * Filters out internal/deprecated functions. Includes inherited functions.
     *
     * @param Class     The UClass to scan
     * @param MaxCount  Maximum entries to return (0 = unlimited)
     * @return Sorted by name, filtered to Blueprint-accessible functions
     */
    static TArray<FFunctionEntry> GetCallableFunctions(UClass* Class, int32 MaxCount = 0);

    /**
     * Enumerate Blueprint-visible properties on a class.
     * Filters out internal/deprecated/replication properties. Includes inherited.
     *
     * @param Class     The UClass to scan
     * @param MaxCount  Maximum entries to return (0 = unlimited)
     * @return Sorted by name, filtered to Blueprint-visible properties
     */
    static TArray<FPropertyEntry> GetVisibleProperties(UClass* Class, int32 MaxCount = 0);

    /**
     * Score a function or property name against a search query.
     * Uses the same 3-criteria scoring as FindFunction's fuzzy match:
     * substring containment, common prefix, camel-case word overlap.
     *
     * @param CandidateName   The function/property name to score
     * @param SearchName      The AI's attempted name
     * @return Score (higher is better, 0 means no match)
     */
    static int32 ScoreSimilarity(const FString& CandidateName, const FString& SearchName);

    /**
     * Format a compact API summary for a class.
     * Used by BuildComponentAPIMap in OliveAgentPipeline.
     *
     * @param Class          The UClass to summarize
     * @param MaxFunctions   Max function entries
     * @param MaxProperties  Max property entries
     * @return Formatted "Functions: ...\nProperties: ..." block
     */
    static FString FormatCompactAPISummary(UClass* Class, int32 MaxFunctions = 15, int32 MaxProperties = 10);

    /**
     * Build class-scoped suggestions for a FUNCTION_NOT_FOUND error.
     * Returns function matches + property matches with set_var/get_var guidance.
     * Used by ResolveCallOp in OliveBlueprintPlanResolver.
     *
     * @param TargetClass    The UClass where the function was expected
     * @param SearchName     The function name the AI tried
     * @param OutAlternatives  Populated with function names for Error.Alternatives
     * @param MaxFunctions   Maximum function suggestions
     * @param MaxProperties  Maximum property suggestions
     * @return Formatted suggestion string
     */
    static FString BuildScopedSuggestions(
        UClass* TargetClass,
        const FString& SearchName,
        TArray<FString>& OutAlternatives,
        int32 MaxFunctions = 5,
        int32 MaxProperties = 3);
};
```

This header is included by both `OliveBlueprintPlanResolver.cpp` and `OliveAgentPipeline.cpp`. The implementation in `OliveClassAPIHelper.cpp` contains the filtering logic, scoring algorithm, and formatting.

#### Edge Cases

- **Build Plan mentions a class that doesn't exist** (e.g., typo "MyCustomComp"): `FindFirstObjectSafe` returns null. Skip that class silently. Log a verbose warning.
- **Component class has 100+ functions** (e.g., `USkeletalMeshComponent`): The per-class cap of 15 functions / 10 properties handles this. The functions listed are alphabetically sorted, not similarity-ranked (ranking is only for Fix 1's error suggestions).
- **Budget exceeded with just 1 component class**: Unlikely but possible for `AActor` (though `AActor` would not appear as a component). If it happens, the 15-function cap already limits it to ~400 chars per class.
- **Duplicate class names across assets**: Deduplication handles this -- each class appears once in the API map.
- **No components in the Build Plan**: `BuildComponentAPIMap` returns empty string. No section emitted.
- **`ParseBuildPlan` fails to extract components**: The `Components` map is empty. Same result -- no section.

---

## File Changes Summary

| File | Change | Fix |
|------|--------|-----|
| `Source/OliveAIEditor/Blueprint/Public/Writer/OliveClassAPIHelper.h` | **NEW** -- static helper for class API enumeration | 1, 3 |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveClassAPIHelper.cpp` | **NEW** -- implementation of filtering, scoring, formatting | 1, 3 |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | **MODIFY** -- `ResolveCallOp()` lines 1707-1757: replace catalog fuzzy match with class-scoped suggestions via `FOliveClassAPIHelper::BuildScopedSuggestions()` | 1 |
| `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | **MODIFY** -- `BuildToolErrorMessage()` line 717-720: replace single-line FUNCTION_NOT_FOUND handler with attempt-progressive guidance | 2 |
| `Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h` | **MODIFY** -- add `FString ComponentAPIMap` field to `FOliveArchitectResult` | 3 |
| `Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp` | **MODIFY** -- add `BuildComponentAPIMap()` static method; call it after `ParseBuildPlan()` in both `RunArchitect()` and `RunPlanner()`; add Section 3.5 in `FormatForPromptInjection()` | 3 |
| `Source/OliveAIEditor/Public/Brain/OliveAgentPipeline.h` | **MODIFY** -- add `BuildComponentAPIMap()` declaration in private static helpers section | 3 |

---

## Implementation Order

### Phase 1: Shared Helper (prerequisite for Fix 1 and Fix 3)

1. Create `OliveClassAPIHelper.h` with the struct definitions and method declarations.
2. Implement `OliveClassAPIHelper.cpp`:
   - `GetCallableFunctions()` -- TFieldIterator + filter + sort
   - `GetVisibleProperties()` -- TFieldIterator + filter + sort
   - `ScoreSimilarity()` -- extract the existing scoring algorithm from `OliveNodeFactory.cpp` lines 2452-2543 into this reusable form
   - `FormatCompactAPISummary()` -- format for pipeline injection
   - `BuildScopedSuggestions()` -- format for error messages, with property cross-match detection
3. Build and verify no compile errors.

### Phase 2: Fix 1 (class-scoped error suggestions)

4. In `OliveBlueprintPlanResolver.cpp`, add `#include "Writer/OliveClassAPIHelper.h"`.
5. In `ResolveCallOp()`, replace lines 1707-1757 with the conditional logic described above.
6. Resolve the target class: if `Step.TargetClass` is non-empty, use `FindFirstObjectSafe<UClass>()` with the `U` prefix. If empty, check if `FindFunctionEx` recorded SCS component classes in its search (the `SearchResult.SearchedLocations` trail mentions component classes). As a simpler alternative, iterate `BP->SimpleConstructionScript->GetAllNodes()` and check each component class for name similarity to `Step.Target` using `ScoreSimilarity()`. Pick the best-scoring class.
7. Call `FOliveClassAPIHelper::BuildScopedSuggestions()` with the resolved class.
8. Populate `Error.Suggestion` and `Error.Alternatives` from the helper's output.
9. Build. Test with a plan_json that calls `SetSpeed` on a BP with `ProjectileMovementComponent`. Verify the error message now shows `MaxSpeed` as a property suggestion.

### Phase 3: Fix 2 (progressive self-correction)

10. In `OliveSelfCorrectionPolicy.cpp`, replace the `FUNCTION_NOT_FOUND` block at line 717-720 with the attempt-progressive guidance.
11. Build. No new test needed -- this is a string-only change gated by `AttemptNum`.

### Phase 4: Fix 3 (component API map injection)

12. In `OliveAgentConfig.h`, add `FString ComponentAPIMap;` to `FOliveArchitectResult`.
13. In `OliveAgentPipeline.h`, add `static FString BuildComponentAPIMap(const TMap<FString, TArray<TPair<FString, FString>>>& Components);` in the private static helpers section.
14. In `OliveAgentPipeline.cpp`:
    - Implement `BuildComponentAPIMap()` using `FOliveClassAPIHelper::FormatCompactAPISummary()`.
    - After `ParseBuildPlan()` calls in `RunArchitect()` and `RunPlanner()`, add: `Result.ComponentAPIMap = BuildComponentAPIMap(Result.Components);`
    - In `FormatForPromptInjection()`, after the Validator Warnings block (line 393), add Section 3.5.
15. Build. Test with a Build Plan that mentions components. Verify the API map appears in the formatted prompt.

---

## Example Output (Fix 1)

Before (current behavior):
```
FUNCTION_NOT_FOUND: Function 'SetSpeed' not found. Searched: alias map (180 entries), specified class 'ProjectileMovementComponent', Blueprint class 'BP_Projectile_C', ...
Suggestion: Did you mean: SetSphereRadius (SphereComponent), SetSimulatePhysics (PrimitiveComponent), SetHiddenInGame (SceneComponent), SetIsReplicated (ActorComponent), SetActive (ActorComponent). Check if 'ProjectileMovementComponent' is the correct target_class, or omit it to search all scopes.
```

After (with Fix 1):
```
FUNCTION_NOT_FOUND: Function 'SetSpeed' not found. Searched: alias map (180 entries), specified class 'ProjectileMovementComponent', Blueprint class 'BP_Projectile_C', ...
Suggestion: Available on ProjectileMovementComponent:
Functions: StopMovementImmediately(), SetVelocityInLocalSpace(NewVelocity), MoveUpdatedComponent(Delta, NewRotation, bSweep), SetUpdatedComponent(NewComponent), StopSimulating(HitResult)
Properties: MaxSpeed (float), InitialSpeed (float), ProjectileGravityScale (float), Velocity (FVector), HomingAccelerationMagnitude (float)
Likely fix: 'MaxSpeed' is a property (float), not a function. Use op:set_var target:MaxSpeed instead of op:call target:SetSpeed.
Check if 'ProjectileMovementComponent' is the correct target_class, or omit it to search all scopes.
```

## Example Output (Fix 3)

Injected into Builder prompt after the Build Plan:
```
## Component API Reference

Use set_var/get_var for properties, call for functions.

### ProjectileMovementComponent
Functions: MoveUpdatedComponent(Delta, NewRotation, bSweep), SetUpdatedComponent(NewComponent), SetVelocityInLocalSpace(NewVelocity), StopMovementImmediately(), StopSimulating(HitResult)
Properties: InitialSpeed (float), MaxSpeed (float), ProjectileGravityScale (float), Velocity (FVector), bRotationFollowsVelocity (bool), bShouldBounce (bool), Bounciness (float), HomingTargetComponent (USceneComponent*), HomingAccelerationMagnitude (float)

### SphereComponent
Functions: SetSphereRadius(InSphereRadius, bUpdateOverlaps)
Properties: SphereRadius (float)
```

---

## What This Does NOT Change

- `FindFunction()` / `FindFunctionEx()` in `OliveNodeFactory.cpp` -- untouched. Internal fuzzy match stays for logging.
- `FOliveNodeCatalog` -- not modified. Its `FuzzyMatch()` is still used for `add_node` and `search_nodes` tools.
- `BlueprintPlanIR.h` -- `FOliveIRBlueprintPlanError` struct unchanged.
- No new settings. No new tools. No new IR types.
- The existing fallback to catalog fuzzy match is preserved when no target class can be resolved.
