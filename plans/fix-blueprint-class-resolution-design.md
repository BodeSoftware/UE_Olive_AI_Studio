# Fix Blueprint Class Resolution Failures

**Date:** 2026-02-24
**Status:** Design
**Scope:** 3 issues across 4 files, all backward-compatible fixes

---

## Problem Summary

Three distinct failures observed when the AI creates or references Blueprint-derived classes:

1. **Critical**: `FOliveNodeFactory::FindClass()` and `FOliveBlueprintWriter::FindParentClass()` cannot resolve short Blueprint asset names (e.g., `"BP_Bullet"` without a path). The AI commonly provides these short names. Both methods only attempt Blueprint/asset-registry lookup when the string contains `/`.

2. **Prompt/Guardrail**: The `PLAN_INVALID_REF_FORMAT` error message in `OliveIRSchema.cpp` says "malformed @ref" but does not explain *why* it failed or guide the AI toward the correct usage (referencing a step_id, not a component name).

3. **Prompt**: The `BP_CONNECT_PINS_FAILED` self-correction hint in `OliveSelfCorrectionPolicy.cpp` does not aggressively direct the AI to call `blueprint.read` first to discover actual pin names. It lists multiple possible causes but the AI frequently retries with hallucinated pin names.

---

## Design

### Task 1: Shared Blueprint Class Resolver (`FOliveClassResolver`)

**What:** Extract a new static utility class `FOliveClassResolver` that provides a single `ResolveClass()` method. Both `FOliveNodeFactory::FindClass()` and `FOliveBlueprintWriter::FindParentClass()` will delegate to it.

**Why a new class instead of putting it on one of the existing ones:**
- `FOliveNodeFactory::FindClass()` is a private method on a singleton -- cannot be called from `FOliveBlueprintWriter` without making it public and creating a dependency from the writer to the factory.
- `FOliveBlueprintWriter::FindParentClass()` has similar access issues going the other direction.
- `FOliveFunctionResolver::FindClassByName()` already exists as a third copy of nearly the same logic but is also a private static method.
- A shared utility keeps all three callers clean. It lives alongside the existing Blueprint utilities.

**Where it lives:**

```
Source/OliveAIEditor/Blueprint/Public/OliveClassResolver.h   (header)
Source/OliveAIEditor/Blueprint/Private/OliveClassResolver.cpp (impl)
```

This follows the existing pattern where Blueprint-scoped utilities live directly under `Blueprint/Public/` (e.g., `OliveBlueprintTypes.h`, `OliveBlueprintNavigator.h`).

#### Interface

```cpp
// OliveClassResolver.h
#pragma once

#include "CoreMinimal.h"

class UClass;
class UBlueprint;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveClassResolver, Log, All);

/**
 * Result of class resolution with provenance information.
 */
struct OLIVEAIEDITOR_API FOliveClassResolveResult
{
    /** The resolved UClass (nullptr if not found) */
    UClass* Class = nullptr;

    /** How the class was found */
    enum class EResolveMethod : uint8
    {
        DirectLookup,        // FindFirstObject<UClass> by exact name
        PrefixMatch,         // Added A/U prefix
        NativeClassPath,     // /Script/Engine.AClassName etc.
        BlueprintAssetPath,  // Loaded UBlueprint from full path
        BlueprintShortName,  // Asset registry search by short Blueprint name
        Cached,              // Found in LRU cache
    };

    EResolveMethod Method = EResolveMethod::DirectLookup;

    /** Whether resolution succeeded */
    bool IsValid() const { return Class != nullptr; }
};

/**
 * FOliveClassResolver
 *
 * Shared utility for resolving class names to UClass pointers.
 * Handles native C++ classes (AActor, UObject, etc.), Blueprint-generated
 * classes via full asset path, AND Blueprint classes via short name
 * (e.g., "BP_Bullet") using asset registry search.
 *
 * Resolution chain (stops at first success):
 *   1. LRU cache lookup (if enabled)
 *   2. FindFirstObject<UClass>(Name, NativeFirst)
 *   3. FindFirstObject with A/U prefixes
 *   4. FindFirstObject with _C suffix (Blueprint generated class names)
 *   5. Fully-qualified native class paths (/Script/Engine.AName, etc.)
 *   6. Blueprint path (if contains '/') -- LoadObject<UBlueprint>
 *   7. Asset registry search for short Blueprint names (no '/')
 *
 * Thread Safety: Game thread only (uses LoadObject, asset registry).
 *                Cache is NOT thread-safe -- callers are already on game thread.
 *
 * Callers:
 *   - FOliveNodeFactory::FindClass()
 *   - FOliveBlueprintWriter::FindParentClass()
 *   - FOliveFunctionResolver::FindClassByName()
 */
class OLIVEAIEDITOR_API FOliveClassResolver
{
public:
    /**
     * Resolve a class name or path to a UClass*.
     *
     * @param ClassNameOrPath  Short name ("Actor", "BP_Bullet") or full path
     *                         ("/Game/Blueprints/BP_Bullet")
     * @return Resolution result with UClass* and method
     */
    static FOliveClassResolveResult Resolve(const FString& ClassNameOrPath);

    /**
     * Clear the resolution cache.
     * Call when assets are added/removed/renamed.
     */
    static void ClearCache();

    /** Maximum cache size (LRU eviction beyond this) */
    static constexpr int32 MaxCacheSize = 256;

private:
    // ----- Resolution strategies (tried in order) -----

    /** Strategy 1-2: Direct lookup and A/U prefix variants */
    static UClass* TryDirectLookup(const FString& Name);

    /** Strategy 3: Try _C suffix for Blueprint generated class names */
    static UClass* TrySuffixLookup(const FString& Name);

    /** Strategy 4: Fully-qualified native class paths */
    static UClass* TryNativeClassPaths(const FString& Name);

    /** Strategy 5: Load as Blueprint asset path (contains '/') */
    static UClass* TryBlueprintPath(const FString& Path);

    /** Strategy 6: Asset registry search for short Blueprint names */
    static UClass* TryAssetRegistrySearch(const FString& ShortName);

    // ----- Cache -----

    /** LRU cache: ClassName -> UClass* (weak references via TWeakObjectPtr) */
    struct FCacheEntry
    {
        TWeakObjectPtr<UClass> ClassPtr;
        int32 AccessOrder = 0;   // lower = older
    };

    static TMap<FString, FCacheEntry>& GetCache();
    static int32& GetAccessCounter();

    /** Look up in cache, return nullptr if miss or stale */
    static UClass* CacheLookup(const FString& Key);

    /** Insert into cache with LRU eviction */
    static void CacheInsert(const FString& Key, UClass* Value);
};
```

#### Resolution Strategy Detail

The resolution chain is ordered from cheapest to most expensive:

| Step | Strategy | Cost | Handles |
|------|----------|------|---------|
| 0 | Cache lookup | O(1) | Repeated lookups for same class |
| 1 | `FindFirstObject<UClass>(Name, NativeFirst)` | Very cheap | `AActor`, `UObject`, `ACharacter` |
| 2 | Prefix variants: `A`+Name, `U`+Name | Very cheap | `Actor` -> `AActor`, `Object` -> `UObject` |
| 3 | `_C` suffix: Name+`_C` | Very cheap | `BP_Bullet_C` (generated class name) |
| 4 | Native paths: `/Script/Engine.AName`, `/Script/CoreUObject.UName` | Medium (StaticLoadClass) | Full-path native names |
| 5 | Blueprint path (if contains `/`) | Medium (LoadObject) | `/Game/Blueprints/BP_Bullet` |
| 6 | Asset registry short name search | Most expensive | `BP_Bullet` (no path) |

**Step 6 detail (asset registry search):**

```cpp
static UClass* TryAssetRegistrySearch(const FString& ShortName)
{
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

    // Strategy 6a: Exact package name search (fast)
    // Try common content paths with exact name
    TArray<FString> CommonPrefixes = {
        FString::Printf(TEXT("/Game/Blueprints/%s"), *ShortName),
        FString::Printf(TEXT("/Game/%s"), *ShortName),
        FString::Printf(TEXT("/Game/Characters/%s"), *ShortName),
        FString::Printf(TEXT("/Game/Weapons/%s"), *ShortName),
        FString::Printf(TEXT("/Game/Items/%s"), *ShortName),
    };

    for (const FString& Path : CommonPrefixes)
    {
        FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
        if (AssetData.IsValid())
        {
            UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
            if (BP && BP->GeneratedClass)
            {
                return BP->GeneratedClass;
            }
        }
    }

    // Strategy 6b: Full registry search filtered by UBlueprint class
    FARFilter Filter;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;
    Filter.bRecursivePaths = true;

    TArray<FAssetData> Results;
    Registry.GetAssets(Filter, Results);

    // Filter by exact asset name match (case-insensitive)
    for (const FAssetData& Data : Results)
    {
        if (Data.AssetName.ToString().Equals(ShortName, ESearchCase::IgnoreCase))
        {
            UBlueprint* BP = Cast<UBlueprint>(Data.GetAsset());
            if (BP && BP->GeneratedClass)
            {
                return BP->GeneratedClass;
            }
        }
    }

    return nullptr;
}
```

**Caching strategy:**
- LRU cache with `MaxCacheSize = 256` entries.
- Uses `TWeakObjectPtr<UClass>` so GC'd classes are detected as stale.
- Cache key is the original input string (case-preserved).
- Cache is cleared on `ClearCache()` -- callers should hook this to asset rename/delete delegates if desired. For now, manual clear is sufficient since class resolution is infrequent relative to cache lifetime.
- Cache is NOT thread-safe -- all callers are already on the game thread.

#### Caller Rewiring

**FOliveNodeFactory::FindClass() (OliveNodeFactory.cpp:758):**

Replace the entire method body with:
```cpp
UClass* FOliveNodeFactory::FindClass(const FString& ClassName)
{
    FOliveClassResolveResult Result = FOliveClassResolver::Resolve(ClassName);
    if (Result.IsValid())
    {
        UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindClass('%s'): resolved via %s -> %s"),
            *ClassName, /* method string */, *Result.Class->GetName());
        return Result.Class;
    }

    UE_LOG(LogOliveNodeFactory, Warning, TEXT("FindClass('%s'): FAILED - all resolution strategies exhausted"), *ClassName);
    return nullptr;
}
```

**FOliveBlueprintWriter::FindParentClass() (OliveBlueprintWriter.cpp:1785):**

Replace the entire method body with:
```cpp
UClass* FOliveBlueprintWriter::FindParentClass(const FString& ClassName)
{
    const FString Normalized = ClassName.TrimStartAndEnd();
    if (Normalized.IsEmpty())
    {
        return nullptr;
    }

    FOliveClassResolveResult Result = FOliveClassResolver::Resolve(Normalized);
    if (Result.IsValid())
    {
        return Result.Class;
    }

    return nullptr;
}
```

**FOliveFunctionResolver::FindClassByName() (OliveFunctionResolver.cpp:575):**

Replace the entire method body with:
```cpp
UClass* FOliveFunctionResolver::FindClassByName(const FString& ClassName)
{
    if (ClassName.IsEmpty())
    {
        return nullptr;
    }

    FOliveClassResolveResult Result = FOliveClassResolver::Resolve(ClassName);
    return Result.Class;  // nullptr if not found
}
```

#### Error Message Improvement

When `FindClass()` or `FindParentClass()` fails, the callers should provide a better error message that includes the asset registry search note. The callers already generate their own error strings, so no change is needed in the resolver itself -- the callers just get better resolution coverage.

However, we should add a self-correction hint for a new error code. See Task 4 below.

---

### Task 2: Improve `PLAN_INVALID_REF_FORMAT` Error Message

**File:** `Source/OliveAIRuntime/Private/IR/OliveIRSchema.cpp`
**Location:** Lines 953-959 (inside the `@ref` validation block)

**Current message:**
```
Step X ('Y'): input 'Z' has malformed @ref '@MuzzlePoint'
@ref format must be "@stepId.pinName" (e.g., "@s1.ReturnValue")
```

**New message:**
```
Step X ('Y'): input 'Z' has malformed @ref '@MuzzlePoint' -- @ref must reference a step_id from this plan, not a component or actor name.
@ref format is "@step_id.pin_name" (e.g. "@s1.ReturnValue").
To access a component, add a CallFunction step for GetComponentByClass or GetDefaultSubobjectByName and reference THAT step's output.
To access a variable, add a get_var step and reference its output.
```

**Exact change (OliveIRSchema.cpp, lines 954-958):**

Replace:
```cpp
AddError(
    TEXT("PLAN_INVALID_REF_FORMAT"),
    StepLocation + TEXT("/inputs/") + PinName,
    FString::Printf(TEXT("Step %d ('%s'): input '%s' has malformed @ref '%s'"), i, *StepId, *PinName, *Value),
    TEXT("@ref format must be \"@stepId.pinName\" (e.g., \"@s1.ReturnValue\")"));
```

With:
```cpp
AddError(
    TEXT("PLAN_INVALID_REF_FORMAT"),
    StepLocation + TEXT("/inputs/") + PinName,
    FString::Printf(TEXT("Step %d ('%s'): input '%s' has malformed @ref '%s' — @ref must reference a step_id from this plan, not a component, variable, or actor name"),
        i, *StepId, *PinName, *Value),
    TEXT("@ref format is \"@step_id.pin_name\" (e.g., \"@s1.ReturnValue\"). "
         "To access a component, add a 'call' step targeting GetComponentByClass and reference that step's output. "
         "To access a variable, add a 'get_var' step and reference its output."));
```

**No header changes needed.** This is purely a string constant change.

---

### Task 3: Improve `BP_CONNECT_PINS_FAILED` Self-Correction Hint

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`
**Location:** Lines 271-277 (the `BP_CONNECT_PINS_FAILED` branch in `BuildToolErrorMessage`)

**Current guidance:**
```
"Pin connection failed. Common causes: "
"1) Node not found -- node_ids are scoped per graph. "
"2) Pin format -- use 'node_id.pin_name' (dot, NOT colon). "
"3) Pin name mismatch -- use pin_manifests from apply_plan_json result. "
"4) Type mismatch -- ensure compatible pin types."
```

**New guidance (more directive, forces `blueprint.read` before retry):**
```
"Pin connection failed. BEFORE RETRYING: call blueprint.read with include_pins:true on the target graph "
"to get the actual pin names. Do NOT guess or hallucinate pin names. "
"Common causes: "
"1) Pin name mismatch -- pins often have internal names different from display names "
"   (e.g., 'SpawnTransform' not 'Location', 'ReturnValue' not 'Result'). "
"2) Node not found -- node_ids are scoped per graph. Use blueprint.read to verify. "
"3) Pin format -- use 'node_id.pin_name' (dot separator, NOT colon). "
"4) Type mismatch -- ensure compatible pin types. "
"MANDATORY: your next tool call must be blueprint.read (or blueprint.read_function/blueprint.read_event_graph) "
"with include_pins:true to discover actual pin names. Never retry with the same pin names that just failed."
```

**Exact change (OliveSelfCorrectionPolicy.cpp, lines 271-277):**

Replace:
```cpp
else if (ErrorCode == TEXT("BP_CONNECT_PINS_FAILED"))
{
    Guidance = TEXT("Pin connection failed. Common causes: "
        "1) Node not found — node_ids are scoped per graph. "
        "2) Pin format — use 'node_id.pin_name' (dot, NOT colon). "
        "3) Pin name mismatch — use pin_manifests from apply_plan_json result. "
        "4) Type mismatch — ensure compatible pin types.");
}
```

With:
```cpp
else if (ErrorCode == TEXT("BP_CONNECT_PINS_FAILED"))
{
    Guidance = TEXT("Pin connection failed. BEFORE RETRYING: call blueprint.read "
        "with include_pins:true on the target graph to get the ACTUAL pin names. "
        "Do NOT guess or hallucinate pin names. "
        "Common causes: "
        "1) Pin name mismatch — pins often have internal names different from display names "
        "(e.g., 'SpawnTransform' not 'Location', 'ReturnValue' not 'Result'). "
        "2) Node not found — node_ids are scoped per graph. Use blueprint.read to verify. "
        "3) Pin format — use 'node_id.pin_name' (dot separator, NOT colon). "
        "4) Type mismatch — ensure compatible pin types. "
        "MANDATORY: your next tool call MUST be blueprint.read (or blueprint.read_function / "
        "blueprint.read_event_graph) with include_pins:true. Never retry connect_pins "
        "with the same pin names that just failed.");
}
```

**No header changes needed.**

---

### Task 4: Add `BP_CLASS_NOT_FOUND` Self-Correction Hint

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

Currently there is no specific guidance for when `FindClass` fails in the context of `blueprint.add_node` (SpawnActor / Cast). The error propagates as `BP_ADD_NODE_FAILED` with a message like "Actor class 'BP_Bullet' not found". This is already partially covered by the `NODE_TYPE_UNKNOWN || BP_ADD_NODE_FAILED` branch, but the guidance is about node types, not class resolution.

**Add a new branch** specifically for class-related failures. Insert BEFORE the existing `NODE_TYPE_UNKNOWN` branch:

```cpp
else if (ErrorCode == TEXT("BP_ADD_NODE_FAILED") && ErrorMessage.Contains(TEXT("not found")))
{
    // Class resolution failure from SpawnActor/Cast/etc.
    // After Task 1, FindClass covers short BP names, but the AI may still provide
    // non-existent class names. Guide it to search.
    if (ErrorMessage.Contains(TEXT("class")) || ErrorMessage.Contains(TEXT("Class")))
    {
        Guidance = TEXT("The specified class was not found. "
            "If this is a Blueprint class, provide the full asset path (e.g., '/Game/Blueprints/BP_Bullet') "
            "or use project.search_assets to find the correct path. "
            "If this is a native C++ class, try the full name with prefix (e.g., 'ACharacter', 'APawn'). "
            "Common mistake: using a display name instead of the actual class name.");
    }
    else
    {
        Guidance = TEXT("The node type was not found. Use blueprint.search_nodes to find the correct node type identifier, then retry.");
    }
}
```

Wait -- this approach is fragile because it relies on substring matching in error messages. Better approach: **change the error code** in `OliveNodeFactory.cpp` for class-not-found specifically.

**Revised approach:**

In `OliveNodeFactory.cpp`, when `FindClass()` returns nullptr in `CreateSpawnActorNode` and `CreateCastNode`, set a distinct error code pattern in `LastError` that the tool handler can distinguish. But `LastError` is just a string, not structured. The tool handler in `OliveBlueprintToolHandlers.cpp` wraps this as `BP_ADD_NODE_FAILED`.

Given the current architecture where `LastError` is an unstructured string, the simplest fix is to add the class name to the self-correction hint for `BP_ADD_NODE_FAILED` by making that branch check for the word "class" in the error message. This is pragmatic, not perfect.

**Alternatively**, we keep the existing `BP_ADD_NODE_FAILED` / `NODE_TYPE_UNKNOWN` branch and just expand its guidance to also cover class resolution:

Replace lines 209-212:
```cpp
else if (ErrorCode == TEXT("NODE_TYPE_UNKNOWN") || ErrorCode == TEXT("BP_ADD_NODE_FAILED"))
{
    Guidance = TEXT("The node type was not found. Use blueprint.search_nodes to find the correct node type identifier, then retry.");
}
```

With:
```cpp
else if (ErrorCode == TEXT("NODE_TYPE_UNKNOWN") || ErrorCode == TEXT("BP_ADD_NODE_FAILED"))
{
    if (ErrorMessage.Contains(TEXT("class")) && ErrorMessage.Contains(TEXT("not found")))
    {
        Guidance = TEXT("The specified class was not found. "
            "If this is a Blueprint class, provide the full asset path (e.g., '/Game/Blueprints/BP_Bullet'). "
            "Use project.search_assets to find the correct path. "
            "If this is a native C++ class, use the full prefixed name (e.g., 'ACharacter', 'APawn'). "
            "Do NOT guess class names — search first.");
    }
    else
    {
        Guidance = TEXT("The node type was not found. Use blueprint.search_nodes to find the correct node type identifier, then retry.");
    }
}
```

**No header changes needed.**

---

## File Summary

### New Files
| File | Purpose |
|------|---------|
| `Source/OliveAIEditor/Blueprint/Public/OliveClassResolver.h` | Shared class resolver interface |
| `Source/OliveAIEditor/Blueprint/Private/OliveClassResolver.cpp` | Implementation with 6-step resolution chain + LRU cache |

### Modified Files
| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | `FindClass()` delegates to `FOliveClassResolver::Resolve()` |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveBlueprintWriter.cpp` | `FindParentClass()` delegates to `FOliveClassResolver::Resolve()` |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp` | `FindClassByName()` delegates to `FOliveClassResolver::Resolve()` |
| `Source/OliveAIRuntime/Private/IR/OliveIRSchema.cpp` | Improved `PLAN_INVALID_REF_FORMAT` error message |
| `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | Improved `BP_CONNECT_PINS_FAILED` and `BP_ADD_NODE_FAILED` guidance |

---

## Implementation Order

The coder should implement these tasks in this exact order:

### Task 1: `FOliveClassResolver` (new files + rewiring)
1. Create `OliveClassResolver.h` with the interface shown above
2. Create `OliveClassResolver.cpp` implementing the 6-step resolution chain
3. Add `#include "OliveClassResolver.h"` to `OliveNodeFactory.cpp`, `OliveBlueprintWriter.cpp`, and `OliveFunctionResolver.cpp`
4. Replace `FOliveNodeFactory::FindClass()` body to delegate to `FOliveClassResolver::Resolve()`
5. Replace `FOliveBlueprintWriter::FindParentClass()` body to delegate (keep the `TrimStartAndEnd()` normalization)
6. Replace `FOliveFunctionResolver::FindClassByName()` body to delegate
7. Build and verify no regressions

### Task 2: `PLAN_INVALID_REF_FORMAT` message improvement
1. Edit `OliveIRSchema.cpp` lines 954-958 with the expanded message
2. Build and verify

### Task 3: `BP_CONNECT_PINS_FAILED` hint improvement
1. Edit `OliveSelfCorrectionPolicy.cpp` lines 271-277 with the expanded guidance
2. Build and verify

### Task 4: `BP_ADD_NODE_FAILED` hint improvement
1. Edit `OliveSelfCorrectionPolicy.cpp` lines 209-212 to add class-specific guidance branch
2. Build and verify

---

## Edge Cases & Error Handling

### Task 1 Edge Cases

| Scenario | Handling |
|----------|----------|
| Empty/whitespace-only input | Return invalid result immediately (step 0) |
| Name matches multiple Blueprints (e.g., `BP_Enemy` in `/Game/` and `/Game/Archive/`) | Take the first match from common paths, then first from registry. Log a warning. |
| Blueprint asset exists but `GeneratedClass` is null (broken BP) | Skip, continue to next strategy. Log warning. |
| Asset registry not ready at startup | `TryAssetRegistrySearch` returns nullptr. Not a crash -- caller falls through gracefully. |
| Cache entry points to GC'd class | `TWeakObjectPtr` is stale, treated as cache miss. Entry is evicted. |
| Very long input (someone passes a huge string) | `FindFirstObject` handles this gracefully. Asset registry search is name-filtered. No issue. |
| Input is a full path like `/Game/Blueprints/BP_Bullet` | Hits step 5 (contains `/`) before reaching step 6. Works correctly. |
| Input is a generated class name like `BP_Bullet_C` | Hits step 1 or 3 (direct lookup or _C suffix). Works correctly. |
| Input is `/Script/Engine.AActor` (fully qualified native) | Hits step 1 (FindFirstObject handles these). Falls through to step 4 if not. |

### Task 2/3/4 Edge Cases
These are string constant changes with no runtime edge cases.

---

## Module Boundary

### Dependencies Added
- `OliveClassResolver.h` depends on: `CoreMinimal.h`, `AssetRegistry` module (for `IAssetRegistry`, `FAssetData`, `FARFilter`)
- No new module dependencies for the editor module -- `AssetRegistry` is already a dependency

### What Depends on It
- `OliveNodeFactory.cpp` (includes header, calls `Resolve`)
- `OliveBlueprintWriter.cpp` (includes header, calls `Resolve`)
- `OliveFunctionResolver.cpp` (includes header, calls `Resolve`)
- Future callers that need class resolution (BT node catalog, PCG, etc.)

### Public API
- `FOliveClassResolver::Resolve()` -- the main entry point
- `FOliveClassResolver::ClearCache()` -- for cache invalidation
- `FOliveClassResolveResult` struct -- result type

---

## Coder Notes

1. **Do not add the new .h/.cpp to a Build.cs** -- UBT auto-discovers files in the module source tree. Just create the files in the right directories.

2. **The `AssetRegistry` module include**: Add `#include "AssetRegistry/AssetRegistryModule.h"` and `#include "AssetRegistry/IAssetRegistry.h"` in the .cpp file, NOT the header. The header only needs `CoreMinimal.h` and forward declarations.

3. **Cache implementation**: Use a static `TMap` with a static access counter. On every lookup, increment the counter and update the entry's `AccessOrder`. On eviction, find the entry with the lowest `AccessOrder`. This is O(N) eviction but N=256 max so it is negligible.

4. **Log verbosely**: Each resolution step should log at `Verbose` level what it tried and whether it succeeded. The final failure should log at `Warning` level.

5. **The common-paths optimization in Step 6a** (`/Game/Blueprints/`, `/Game/`, etc.) is a heuristic. If the project uses non-standard content organization, it gracefully falls through to the full registry search in Step 6b.

6. **For Step 6b (full registry search)**: Use `GetAssets()` with a `UBlueprint` class filter, then filter by exact name match. This avoids loading every Blueprint in the project -- `GetAssets()` only queries metadata.  However, calling `Data.GetAsset()` does load the matching Blueprint. Only load after finding a name match.

7. **The `_C` suffix step (Step 3)**: Blueprint generated classes are named `AssetName_C`. If the AI passes `BP_Bullet_C`, `FindFirstObject<UClass>` should find it directly in Step 1. Step 3 handles the case where the AI passes `BP_Bullet` and we try `BP_Bullet_C` as a generated class name.

8. **Keep the existing log category names**: `LogOliveNodeFactory` stays in `FindClass()`, `LogOliveBPWriter` stays in `FindParentClass()`. The resolver has its own `LogOliveClassResolver`.
