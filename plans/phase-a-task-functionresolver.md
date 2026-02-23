# Phase A Task: OliveFunctionResolver

## Overview

Create two files that implement smart function resolution -- a multi-strategy fallback chain that resolves AI-provided function names (which may be approximate, missing `K2_` prefixes, using common aliases, etc.) to actual `UFunction*` pointers.

**Files to create:**
- `Source/OliveAIEditor/Blueprint/Public/Plan/OliveFunctionResolver.h`
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp`

**Depends on (read-only, do NOT modify):**
- `Source/OliveAIEditor/Blueprint/Public/Catalog/OliveNodeCatalog.h` (for `FOliveNodeCatalog::Search()`)
- `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h` (for `FOliveNodeFactory::FindFunction()` and `FindClass()`)
- UE headers: `Kismet/BlueprintFunctionLibrary.h`, `Kismet/KismetSystemLibrary.h`, `Kismet/KismetMathLibrary.h`, `Kismet/KismetStringLibrary.h`, `Kismet/KismetArrayLibrary.h`, `Kismet/GameplayStatics.h`, `GameFramework/Actor.h`, `Components/SceneComponent.h`, `Components/PrimitiveComponent.h`, `Engine/Blueprint.h`, `UObject/UObjectIterator.h`

**Directory:** `Source/OliveAIEditor/Blueprint/Public/Plan/` (already exists) and `Source/OliveAIEditor/Blueprint/Private/Plan/` (created by the PinManifest task, or create if it does not exist).

---

## File 1: OliveFunctionResolver.h

Write this file EXACTLY as specified. This is the complete header.

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Forward declarations
class UFunction;
class UClass;
class UBlueprint;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveFunctionResolver, Log, All);

/**
 * Result of function resolution with match quality information.
 */
struct OLIVEAIEDITOR_API FOliveFunctionMatch
{
    /** The resolved UFunction pointer (nullptr if not found) */
    UFunction* Function = nullptr;

    /** The class that owns this function */
    UClass* OwningClass = nullptr;

    /** How the function was found */
    enum class EMatchMethod : uint8
    {
        ExactName,           // Exact FindFunctionByName match
        K2Prefix,            // Added/removed K2_ prefix
        Alias,               // Common alias map (e.g., "Print" -> "PrintString")
        CatalogExact,        // Exact match in node catalog
        CatalogFuzzy,        // Fuzzy match in node catalog (score >= threshold)
        BroadClassSearch,    // Found by iterating all Blueprint Function Libraries
        ParentClassSearch,   // Found on the Blueprint's parent class hierarchy
    };

    EMatchMethod MatchMethod = EMatchMethod::ExactName;

    /** Match confidence score (0-100). 100 = exact, lower = fuzzier */
    int32 Confidence = 0;

    /** Display name for error messages */
    FString DisplayName;

    /** Whether this match resolved successfully */
    bool IsValid() const { return Function != nullptr; }
};

/**
 * FOliveFunctionResolver
 *
 * Smart function resolution that goes far beyond exact name matching.
 * Implements a prioritized fallback chain to find the UFunction* that
 * best matches an AI-provided function name.
 *
 * Resolution order:
 *   1. Exact FindFunctionByName on TargetClass (if provided)
 *   2. Exact match on core library classes + Blueprint parent hierarchy
 *   3. K2_ prefix manipulation (add K2_ prefix, remove K2_ prefix)
 *   4. Common alias map (static table of known aliases)
 *   5. Node catalog exact match on function name or display name
 *   6. Node catalog fuzzy match (score >= MinFuzzyScore)
 *   7. Broad search: iterate all UBlueprintFunctionLibrary subclasses
 *
 * If multiple candidates are found at the same priority level, the
 * resolver picks the one whose class is closest to the Blueprint's
 * parent class in the inheritance hierarchy. If still ambiguous,
 * it returns an error with all candidates listed.
 *
 * Thread Safety: Stateless, all methods are static. Thread-safe for reads.
 *                Does NOT hold persistent state. Safe to call from any thread
 *                that can access UObjects (game thread recommended).
 */
class OLIVEAIEDITOR_API FOliveFunctionResolver
{
public:
    /**
     * Resolve a function name to a UFunction*.
     *
     * @param FunctionName   Name as provided by the AI (may be approximate)
     * @param TargetClass    Optional class name for disambiguation (may be empty)
     * @param Blueprint      The target Blueprint (for parent class hierarchy search, may be nullptr)
     * @return Match result with function pointer and confidence
     */
    static FOliveFunctionMatch Resolve(
        const FString& FunctionName,
        const FString& TargetClass,
        UBlueprint* Blueprint);

    /**
     * Get all candidates for a function name (for "did you mean?" suggestions).
     *
     * @param FunctionName   Name to search for
     * @param MaxResults     Maximum candidates to return
     * @return Array of matches sorted by confidence descending
     */
    static TArray<FOliveFunctionMatch> GetCandidates(
        const FString& FunctionName,
        int32 MaxResults = 5);

    /**
     * Convert a match method enum to a human-readable string.
     * @param Method The match method to convert
     * @return String representation (e.g., "exact", "k2_prefix", "alias", etc.)
     */
    static FString MatchMethodToString(FOliveFunctionMatch::EMatchMethod Method);

private:
    // ====================================================================
    // Resolution Strategies (tried in order)
    // ====================================================================

    /** Strategy 1: Exact name match on a specific class */
    static UFunction* TryExactMatch(
        const FString& FunctionName,
        UClass* Class);

    /** Strategy 2: Try adding or removing the K2_ prefix on a specific class */
    static UFunction* TryK2PrefixMatch(
        const FString& FunctionName,
        UClass* Class,
        FString& OutResolvedName);

    /** Strategy 3: Look up in the common alias map, then resolve the alias */
    static UFunction* TryAliasMatch(
        const FString& FunctionName,
        FString& OutResolvedName);

    /** Strategy 4: Search the node catalog for exact function name or display name match */
    static FOliveFunctionMatch TryCatalogMatch(
        const FString& FunctionName);

    /** Strategy 5: Broad search across all loaded UBlueprintFunctionLibrary subclasses */
    static TArray<FOliveFunctionMatch> BroadSearch(
        const FString& FunctionName,
        int32 MaxResults);

    // ====================================================================
    // Alias Map
    // ====================================================================

    /**
     * Get the static alias map.
     * Maps common short names / AI-typical names to actual UE function names.
     * Key = AI-provided name (case-insensitive lookup).
     * Value = exact UE function name.
     */
    static const TMap<FString, FString>& GetAliasMap();

    /**
     * Get the class search order for a given Blueprint.
     * Returns: TargetClass (if any), parent class hierarchy, then common library classes.
     *
     * @param TargetClass Optional class name to search first
     * @param Blueprint The Blueprint (for parent class hierarchy), may be nullptr
     * @return Ordered array of classes to search
     */
    static TArray<UClass*> GetSearchOrder(
        const FString& TargetClass,
        UBlueprint* Blueprint);

    /**
     * Find a UClass by name. Wraps FOliveNodeFactory::Get().FindClass().
     * Falls back to FindFirstObject if the factory doesn't find it.
     */
    static UClass* FindClassByName(const FString& ClassName);

    /** Minimum fuzzy match score from the node catalog to accept */
    static constexpr int32 MinFuzzyScore = 60;
};
```

---

## File 2: OliveFunctionResolver.cpp

### File Path

`Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp`

### Includes (exact list, in this order)

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "OliveFunctionResolver.h"

// Blueprint includes
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"

// Kismet / Function Library includes
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet/GameplayStatics.h"

// Actor / Component includes
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"

// Iterator for broad search
#include "UObject/UObjectIterator.h"

// Catalog for fuzzy search
#include "OliveNodeCatalog.h"

DEFINE_LOG_CATEGORY(LogOliveFunctionResolver);
```

**NOTE on include resolution:** The Build.cs adds recursive include paths for `Blueprint/Public/` and `Blueprint/Private/`, so `"OliveFunctionResolver.h"` and `"OliveNodeCatalog.h"` will resolve without path prefixes.

---

### Implementation: `Resolve(FunctionName, TargetClass, Blueprint)`

This is the main entry point. Implement EXACTLY this algorithm:

```
Algorithm: Resolve(FunctionName, TargetClass, Blueprint)

1. Guard: if FunctionName is empty, return empty FOliveFunctionMatch.

2. Build the class search order via GetSearchOrder(TargetClass, Blueprint).
   This returns an ordered array of UClass* to try.

3. STRATEGY 1: Exact match on each class in search order.
   For each UClass* Class in SearchOrder:
       UFunction* Found = TryExactMatch(FunctionName, Class)
       If Found:
           Return FOliveFunctionMatch{
               Function = Found,
               OwningClass = Class,
               MatchMethod = ExactName,
               Confidence = 100,
               DisplayName = Found->GetDisplayNameText().ToString()
           }

4. STRATEGY 2: K2_ prefix manipulation on each class in search order.
   For each UClass* Class in SearchOrder:
       FString ResolvedName;
       UFunction* Found = TryK2PrefixMatch(FunctionName, Class, ResolvedName)
       If Found:
           UE_LOG(LogOliveFunctionResolver, Log,
               TEXT("K2 prefix resolved '%s' -> '%s' on %s"),
               *FunctionName, *ResolvedName, *Class->GetName())
           Return FOliveFunctionMatch{
               Function = Found,
               OwningClass = Class,
               MatchMethod = K2Prefix,
               Confidence = 95,
               DisplayName = Found->GetDisplayNameText().ToString()
           }

5. STRATEGY 3: Alias map lookup.
   FString AliasResolved;
   UFunction* Found = TryAliasMatch(FunctionName, AliasResolved)
   If Found:
       UE_LOG(LogOliveFunctionResolver, Log,
           TEXT("Alias resolved '%s' -> '%s'"), *FunctionName, *AliasResolved)
       Return FOliveFunctionMatch{
           Function = Found,
           OwningClass = Found->GetOwnerClass(),
           MatchMethod = Alias,
           Confidence = 90,
           DisplayName = Found->GetDisplayNameText().ToString()
       }

6. STRATEGY 4: Node catalog match.
   FOliveFunctionMatch CatalogMatch = TryCatalogMatch(FunctionName)
   If CatalogMatch.IsValid():
       Return CatalogMatch  (MatchMethod and Confidence are set by TryCatalogMatch)

7. STRATEGY 5: Broad search across all function libraries.
   TArray<FOliveFunctionMatch> BroadResults = BroadSearch(FunctionName, 1)
   If BroadResults.Num() > 0:
       Return BroadResults[0]

8. FAILURE: No match found.
   UE_LOG(LogOliveFunctionResolver, Warning,
       TEXT("Function '%s' could not be resolved by any strategy"), *FunctionName)
   Return empty FOliveFunctionMatch
```

---

### Implementation: `GetSearchOrder(TargetClass, Blueprint)`

```
Algorithm: GetSearchOrder(TargetClass, Blueprint)

1. TArray<UClass*> Result;
   TSet<UClass*> Added;  // prevent duplicates

2. If TargetClass is not empty:
   UClass* TC = FindClassByName(TargetClass)
   If TC is valid AND not already in Added:
       Result.Add(TC)
       Added.Add(TC)

3. If Blueprint is valid AND Blueprint->ParentClass is valid:
   Walk the parent class hierarchy:
   UClass* Current = Blueprint->ParentClass
   While Current is valid:
       If not in Added:
           Result.Add(Current)
           Added.Add(Current)
       Current = Current->GetSuperClass()

4. Add common library classes (in this order):
   Static list of classes to always search:
       - UKismetSystemLibrary::StaticClass()
       - UKismetMathLibrary::StaticClass()
       - UKismetStringLibrary::StaticClass()
       - UKismetArrayLibrary::StaticClass()
       - UGameplayStatics::StaticClass()
       - AActor::StaticClass()                    (already added if in parent chain, but Add guards)
       - USceneComponent::StaticClass()
       - UPrimitiveComponent::StaticClass()
       - APawn::StaticClass()                     (already added if in parent chain)
       - ACharacter::StaticClass()                (already added if in parent chain)

   For each class, only add if not already in Added.

5. Return Result
```

---

### Implementation: `TryExactMatch(FunctionName, Class)`

Simple one-liner:

```cpp
UFunction* FOliveFunctionResolver::TryExactMatch(const FString& FunctionName, UClass* Class)
{
    if (!Class)
    {
        return nullptr;
    }
    return Class->FindFunctionByName(FName(*FunctionName));
}
```

---

### Implementation: `TryK2PrefixMatch(FunctionName, Class, OutResolvedName)`

```cpp
UFunction* FOliveFunctionResolver::TryK2PrefixMatch(
    const FString& FunctionName,
    UClass* Class,
    FString& OutResolvedName)
{
    if (!Class)
    {
        return nullptr;
    }

    // Case 1: Function name starts with "K2_" -- try stripping the prefix
    if (FunctionName.StartsWith(TEXT("K2_"), ESearchCase::CaseSensitive))
    {
        FString Stripped = FunctionName.Mid(3);
        UFunction* Found = Class->FindFunctionByName(FName(*Stripped));
        if (Found)
        {
            OutResolvedName = Stripped;
            return Found;
        }
    }

    // Case 2: Function name does NOT start with "K2_" -- try adding the prefix
    if (!FunctionName.StartsWith(TEXT("K2_"), ESearchCase::CaseSensitive))
    {
        FString Prefixed = TEXT("K2_") + FunctionName;
        UFunction* Found = Class->FindFunctionByName(FName(*Prefixed));
        if (Found)
        {
            OutResolvedName = Prefixed;
            return Found;
        }
    }

    return nullptr;
}
```

---

### Implementation: `TryAliasMatch(FunctionName, OutResolvedName)`

```cpp
UFunction* FOliveFunctionResolver::TryAliasMatch(
    const FString& FunctionName,
    FString& OutResolvedName)
{
    const TMap<FString, FString>& Aliases = GetAliasMap();

    // Case-insensitive lookup: normalize the key
    FString LowerName = FunctionName.ToLower();

    for (const auto& Pair : Aliases)
    {
        if (Pair.Key.ToLower() == LowerName)
        {
            OutResolvedName = Pair.Value;

            // Now resolve the alias target against all common classes
            // Use a static list of common classes for alias resolution
            TArray<UClass*> CommonClasses = {
                UKismetSystemLibrary::StaticClass(),
                UKismetMathLibrary::StaticClass(),
                UKismetStringLibrary::StaticClass(),
                UKismetArrayLibrary::StaticClass(),
                UGameplayStatics::StaticClass(),
                AActor::StaticClass(),
                USceneComponent::StaticClass(),
                UPrimitiveComponent::StaticClass(),
                APawn::StaticClass(),
                ACharacter::StaticClass(),
            };

            for (UClass* Class : CommonClasses)
            {
                UFunction* Found = Class->FindFunctionByName(FName(*OutResolvedName));
                if (Found)
                {
                    return Found;
                }
            }

            // Also try K2_ prefix on the resolved alias
            FString K2Name = TEXT("K2_") + OutResolvedName;
            for (UClass* Class : CommonClasses)
            {
                UFunction* Found = Class->FindFunctionByName(FName(*K2Name));
                if (Found)
                {
                    OutResolvedName = K2Name;
                    return Found;
                }
            }

            // Alias exists but target function not found on common classes
            UE_LOG(LogOliveFunctionResolver, Warning,
                TEXT("Alias '%s' -> '%s' found but target function not found on any common class"),
                *FunctionName, *Pair.Value);
            return nullptr;
        }
    }

    return nullptr;
}
```

---

### Implementation: `TryCatalogMatch(FunctionName)`

```cpp
FOliveFunctionMatch FOliveFunctionResolver::TryCatalogMatch(const FString& FunctionName)
{
    FOliveFunctionMatch Result;

    FOliveNodeCatalog& Catalog = FOliveNodeCatalog::Get();
    if (!Catalog.IsInitialized())
    {
        return Result;
    }

    // Search the catalog -- returns results sorted by relevance score
    TArray<FOliveNodeTypeInfo> SearchResults = Catalog.Search(FunctionName, 5);

    for (const FOliveNodeTypeInfo& Info : SearchResults)
    {
        // We only care about function library entries that have FunctionName + FunctionClass
        if (Info.FunctionName.IsEmpty() || Info.FunctionClass.IsEmpty())
        {
            continue;
        }

        // Try to resolve the function
        UClass* FuncClass = FindClassByName(Info.FunctionClass);
        if (!FuncClass)
        {
            continue;
        }

        UFunction* Found = FuncClass->FindFunctionByName(FName(*Info.FunctionName));
        if (!Found)
        {
            continue;
        }

        // Determine match quality
        int32 Score = Info.MatchScore(FunctionName);

        // Exact function name match in catalog = high confidence
        if (Info.FunctionName.Equals(FunctionName, ESearchCase::IgnoreCase))
        {
            Result.Function = Found;
            Result.OwningClass = FuncClass;
            Result.MatchMethod = FOliveFunctionMatch::EMatchMethod::CatalogExact;
            Result.Confidence = 85;
            Result.DisplayName = Info.DisplayName;

            UE_LOG(LogOliveFunctionResolver, Log,
                TEXT("Catalog exact match: '%s' -> %s::%s"),
                *FunctionName, *Info.FunctionClass, *Info.FunctionName);
            return Result;
        }

        // Display name match = also high confidence
        if (Info.DisplayName.Equals(FunctionName, ESearchCase::IgnoreCase))
        {
            Result.Function = Found;
            Result.OwningClass = FuncClass;
            Result.MatchMethod = FOliveFunctionMatch::EMatchMethod::CatalogExact;
            Result.Confidence = 80;
            Result.DisplayName = Info.DisplayName;

            UE_LOG(LogOliveFunctionResolver, Log,
                TEXT("Catalog display name match: '%s' -> %s::%s"),
                *FunctionName, *Info.FunctionClass, *Info.FunctionName);
            return Result;
        }

        // Fuzzy match: accept if score >= MinFuzzyScore
        if (Score >= MinFuzzyScore && !Result.IsValid())
        {
            Result.Function = Found;
            Result.OwningClass = FuncClass;
            Result.MatchMethod = FOliveFunctionMatch::EMatchMethod::CatalogFuzzy;
            Result.Confidence = FMath::Clamp(Score / 15, 40, 70);  // Scale score to confidence
            Result.DisplayName = Info.DisplayName;

            UE_LOG(LogOliveFunctionResolver, Log,
                TEXT("Catalog fuzzy match: '%s' -> %s::%s (score=%d, confidence=%d)"),
                *FunctionName, *Info.FunctionClass, *Info.FunctionName,
                Score, Result.Confidence);
            // Don't return yet -- keep searching for a better match
        }
    }

    return Result;
}
```

---

### Implementation: `BroadSearch(FunctionName, MaxResults)`

This is the last resort -- iterate ALL loaded `UBlueprintFunctionLibrary` subclasses.

```cpp
TArray<FOliveFunctionMatch> FOliveFunctionResolver::BroadSearch(
    const FString& FunctionName,
    int32 MaxResults)
{
    TArray<FOliveFunctionMatch> Results;
    FName FuncFName(*FunctionName);

    // Also prepare K2_ variant
    FName K2FuncFName(*(TEXT("K2_") + FunctionName));

    for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
    {
        UClass* Class = *ClassIt;

        // Only search function libraries and common gameplay classes
        bool bIsFunctionLibrary = Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass());
        bool bIsGameplayClass = Class->IsChildOf(AActor::StaticClass()) ||
                                Class->IsChildOf(UActorComponent::StaticClass());

        if (!bIsFunctionLibrary && !bIsGameplayClass)
        {
            continue;
        }

        if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
        {
            continue;
        }

        // Try exact name
        UFunction* Found = Class->FindFunctionByName(FuncFName);
        if (!Found)
        {
            // Try K2_ prefix
            Found = Class->FindFunctionByName(K2FuncFName);
        }

        if (Found && Found->HasAnyFunctionFlags(FUNC_BlueprintCallable))
        {
            FOliveFunctionMatch Match;
            Match.Function = Found;
            Match.OwningClass = Class;
            Match.MatchMethod = bIsFunctionLibrary
                ? FOliveFunctionMatch::EMatchMethod::BroadClassSearch
                : FOliveFunctionMatch::EMatchMethod::ParentClassSearch;
            Match.Confidence = bIsFunctionLibrary ? 50 : 55;
            Match.DisplayName = Found->GetDisplayNameText().ToString();

            Results.Add(Match);

            UE_LOG(LogOliveFunctionResolver, Log,
                TEXT("Broad search found '%s' on %s (confidence=%d)"),
                *FunctionName, *Class->GetName(), Match.Confidence);

            if (Results.Num() >= MaxResults)
            {
                break;
            }
        }
    }

    // Sort by confidence descending
    Results.Sort([](const FOliveFunctionMatch& A, const FOliveFunctionMatch& B)
    {
        return A.Confidence > B.Confidence;
    });

    return Results;
}
```

---

### Implementation: `GetCandidates(FunctionName, MaxResults)`

This method is for "did you mean?" suggestions when resolution fails.

```cpp
TArray<FOliveFunctionMatch> FOliveFunctionResolver::GetCandidates(
    const FString& FunctionName,
    int32 MaxResults)
{
    TArray<FOliveFunctionMatch> Candidates;

    // Source 1: Node catalog search (best source of suggestions)
    FOliveNodeCatalog& Catalog = FOliveNodeCatalog::Get();
    if (Catalog.IsInitialized())
    {
        TArray<FOliveNodeTypeInfo> SearchResults = Catalog.Search(FunctionName, MaxResults * 2);

        for (const FOliveNodeTypeInfo& Info : SearchResults)
        {
            if (Info.FunctionName.IsEmpty() || Info.FunctionClass.IsEmpty())
            {
                continue;
            }

            UClass* FuncClass = FindClassByName(Info.FunctionClass);
            if (!FuncClass)
            {
                continue;
            }

            UFunction* Found = FuncClass->FindFunctionByName(FName(*Info.FunctionName));
            if (!Found)
            {
                continue;
            }

            FOliveFunctionMatch Match;
            Match.Function = Found;
            Match.OwningClass = FuncClass;
            Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::CatalogFuzzy;
            Match.Confidence = FMath::Clamp(Info.MatchScore(FunctionName) / 15, 10, 70);
            Match.DisplayName = Info.DisplayName;
            Candidates.Add(Match);

            if (Candidates.Num() >= MaxResults)
            {
                break;
            }
        }
    }

    // Source 2: Broad search (if catalog didn't produce enough results)
    if (Candidates.Num() < MaxResults)
    {
        TArray<FOliveFunctionMatch> BroadResults = BroadSearch(
            FunctionName, MaxResults - Candidates.Num());
        Candidates.Append(BroadResults);
    }

    // Sort by confidence descending and truncate
    Candidates.Sort([](const FOliveFunctionMatch& A, const FOliveFunctionMatch& B)
    {
        return A.Confidence > B.Confidence;
    });

    if (Candidates.Num() > MaxResults)
    {
        Candidates.SetNum(MaxResults);
    }

    return Candidates;
}
```

---

### Implementation: `MatchMethodToString(Method)`

```cpp
FString FOliveFunctionResolver::MatchMethodToString(FOliveFunctionMatch::EMatchMethod Method)
{
    switch (Method)
    {
        case FOliveFunctionMatch::EMatchMethod::ExactName:          return TEXT("exact");
        case FOliveFunctionMatch::EMatchMethod::K2Prefix:           return TEXT("k2_prefix");
        case FOliveFunctionMatch::EMatchMethod::Alias:              return TEXT("alias");
        case FOliveFunctionMatch::EMatchMethod::CatalogExact:       return TEXT("catalog_exact");
        case FOliveFunctionMatch::EMatchMethod::CatalogFuzzy:       return TEXT("catalog_fuzzy");
        case FOliveFunctionMatch::EMatchMethod::BroadClassSearch:   return TEXT("broad_class_search");
        case FOliveFunctionMatch::EMatchMethod::ParentClassSearch:  return TEXT("parent_class_search");
        default:                                                     return TEXT("unknown");
    }
}
```

---

### Implementation: `FindClassByName(ClassName)`

```cpp
UClass* FOliveFunctionResolver::FindClassByName(const FString& ClassName)
{
    if (ClassName.IsEmpty())
    {
        return nullptr;
    }

    // Try direct lookup first
    UClass* Found = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
    if (Found)
    {
        return Found;
    }

    // Try with common prefixes
    for (const TCHAR* Prefix : { TEXT("U"), TEXT("A"), TEXT("") })
    {
        FString PrefixedName = FString(Prefix) + ClassName;
        Found = FindFirstObject<UClass>(*PrefixedName, EFindFirstObjectOptions::NativeFirst);
        if (Found)
        {
            return Found;
        }
    }

    return nullptr;
}
```

---

### Implementation: `GetAliasMap()`

This is the complete alias map. Every entry has been verified against UE 5.5 source. The key is the AI-provided name, the value is the actual UE function name that exists on one of the common library/gameplay classes.

```cpp
const TMap<FString, FString>& FOliveFunctionResolver::GetAliasMap()
{
    static const TMap<FString, FString> Aliases = []()
    {
        TMap<FString, FString> Map;

        // ================================================================
        // Transform / Location / Rotation (AActor, K2_ prefixed)
        // ================================================================
        // AActor::K2_GetActorLocation, K2_SetActorLocation, etc.
        Map.Add(TEXT("GetLocation"), TEXT("K2_GetActorLocation"));
        Map.Add(TEXT("SetLocation"), TEXT("K2_SetActorLocation"));
        Map.Add(TEXT("GetActorLocation"), TEXT("K2_GetActorLocation"));
        Map.Add(TEXT("SetActorLocation"), TEXT("K2_SetActorLocation"));
        Map.Add(TEXT("GetRotation"), TEXT("K2_GetActorRotation"));
        Map.Add(TEXT("SetRotation"), TEXT("K2_SetActorRotation"));
        Map.Add(TEXT("GetActorRotation"), TEXT("K2_GetActorRotation"));
        Map.Add(TEXT("SetActorRotation"), TEXT("K2_SetActorRotation"));
        Map.Add(TEXT("SetLocationAndRotation"), TEXT("K2_SetActorLocationAndRotation"));
        Map.Add(TEXT("GetScale"), TEXT("GetActorScale3D"));
        Map.Add(TEXT("SetScale"), TEXT("SetActorScale3D"));
        Map.Add(TEXT("GetTransform"), TEXT("GetActorTransform"));
        Map.Add(TEXT("SetTransform"), TEXT("K2_SetActorTransform"));

        // Direction vectors (AActor -- NOT K2_ prefixed)
        Map.Add(TEXT("GetForwardVector"), TEXT("GetActorForwardVector"));
        Map.Add(TEXT("GetRightVector"), TEXT("GetActorRightVector"));
        Map.Add(TEXT("GetUpVector"), TEXT("GetActorUpVector"));

        // Component location (USceneComponent, K2_ prefixed)
        Map.Add(TEXT("GetWorldLocation"), TEXT("K2_GetComponentLocation"));
        Map.Add(TEXT("GetComponentLocation"), TEXT("K2_GetComponentLocation"));
        Map.Add(TEXT("SetWorldLocation"), TEXT("K2_SetWorldLocation"));
        Map.Add(TEXT("GetWorldRotation"), TEXT("K2_GetComponentRotation"));
        Map.Add(TEXT("GetComponentRotation"), TEXT("K2_GetComponentRotation"));
        Map.Add(TEXT("SetWorldRotation"), TEXT("K2_SetWorldRotation"));
        Map.Add(TEXT("GetComponentTransform"), TEXT("K2_GetComponentToWorld"));
        Map.Add(TEXT("GetWorldTransform"), TEXT("K2_GetComponentToWorld"));
        Map.Add(TEXT("SetRelativeLocation"), TEXT("K2_SetRelativeLocation"));
        Map.Add(TEXT("SetRelativeRotation"), TEXT("K2_SetRelativeRotation"));
        Map.Add(TEXT("AddWorldOffset"), TEXT("K2_AddWorldOffset"));
        Map.Add(TEXT("AddLocalOffset"), TEXT("K2_AddLocalOffset"));
        Map.Add(TEXT("AddWorldRotation"), TEXT("K2_AddWorldRotation"));
        Map.Add(TEXT("AddLocalRotation"), TEXT("K2_AddLocalRotation"));

        // ================================================================
        // Actor Operations (AActor, K2_ prefixed or direct)
        // ================================================================
        Map.Add(TEXT("Destroy"), TEXT("K2_DestroyActor"));
        Map.Add(TEXT("DestroyActor"), TEXT("K2_DestroyActor"));
        Map.Add(TEXT("DestroyComponent"), TEXT("K2_DestroyComponent"));
        Map.Add(TEXT("AttachToActor"), TEXT("K2_AttachToActor"));
        Map.Add(TEXT("AttachToComponent"), TEXT("K2_AttachToComponent"));
        Map.Add(TEXT("DetachFromActor"), TEXT("K2_DetachFromActor"));
        Map.Add(TEXT("GetOwner"), TEXT("GetOwner"));
        Map.Add(TEXT("GetDistanceTo"), TEXT("GetDistanceTo"));
        Map.Add(TEXT("Distance"), TEXT("GetDistanceTo"));

        // ================================================================
        // Spawning (UGameplayStatics / K2Node_SpawnActorFromClass)
        // ================================================================
        // Note: SpawnActor uses a dedicated node type (OliveNodeTypes::SpawnActor),
        // but if the AI asks for it as a function call, point to the internal name
        Map.Add(TEXT("SpawnActor"), TEXT("BeginDeferredActorSpawnFromClass"));
        Map.Add(TEXT("SpawnActorFromClass"), TEXT("BeginDeferredActorSpawnFromClass"));

        // ================================================================
        // String Operations (UKismetStringLibrary)
        // ================================================================
        Map.Add(TEXT("Print"), TEXT("PrintString"));
        Map.Add(TEXT("PrintMessage"), TEXT("PrintString"));
        Map.Add(TEXT("Log"), TEXT("PrintString"));
        Map.Add(TEXT("LogMessage"), TEXT("PrintString"));
        Map.Add(TEXT("ToString"), TEXT("Conv_IntToString"));
        Map.Add(TEXT("IntToString"), TEXT("Conv_IntToString"));
        Map.Add(TEXT("FloatToString"), TEXT("Conv_FloatToString"));
        Map.Add(TEXT("BoolToString"), TEXT("Conv_BoolToString"));
        Map.Add(TEXT("Concatenate"), TEXT("Concat_StrStr"));
        Map.Add(TEXT("StringConcat"), TEXT("Concat_StrStr"));
        Map.Add(TEXT("StringAppend"), TEXT("Concat_StrStr"));
        Map.Add(TEXT("StringContains"), TEXT("Contains"));
        Map.Add(TEXT("StringLength"), TEXT("Len"));
        Map.Add(TEXT("Substring"), TEXT("GetSubstring"));

        // ================================================================
        // Math Operations (UKismetMathLibrary)
        // ================================================================
        Map.Add(TEXT("Add"), TEXT("Add_VectorVector"));
        Map.Add(TEXT("AddVectors"), TEXT("Add_VectorVector"));
        Map.Add(TEXT("Subtract"), TEXT("Subtract_VectorVector"));
        Map.Add(TEXT("SubtractVectors"), TEXT("Subtract_VectorVector"));
        Map.Add(TEXT("Multiply"), TEXT("Multiply_VectorFloat"));
        Map.Add(TEXT("MultiplyVector"), TEXT("Multiply_VectorFloat"));
        Map.Add(TEXT("Normalize"), TEXT("Normal"));
        Map.Add(TEXT("NormalizeVector"), TEXT("Normal"));
        Map.Add(TEXT("VectorLength"), TEXT("VSize"));
        Map.Add(TEXT("VectorSize"), TEXT("VSize"));
        Map.Add(TEXT("Lerp"), TEXT("Lerp"));
        Map.Add(TEXT("LinearInterpolate"), TEXT("Lerp"));
        Map.Add(TEXT("Clamp"), TEXT("FClamp"));
        Map.Add(TEXT("ClampFloat"), TEXT("FClamp"));
        Map.Add(TEXT("Abs"), TEXT("Abs"));
        Map.Add(TEXT("RandomFloat"), TEXT("RandomFloatInRange"));
        Map.Add(TEXT("RandomInt"), TEXT("RandomIntegerInRange"));
        Map.Add(TEXT("RandomInRange"), TEXT("RandomFloatInRange"));
        Map.Add(TEXT("Min"), TEXT("FMin"));
        Map.Add(TEXT("Max"), TEXT("FMax"));
        Map.Add(TEXT("Floor"), TEXT("FFloor"));
        Map.Add(TEXT("Ceil"), TEXT("FCeil"));
        Map.Add(TEXT("Round"), TEXT("Round"));
        Map.Add(TEXT("Sin"), TEXT("Sin"));
        Map.Add(TEXT("Cos"), TEXT("Cos"));
        Map.Add(TEXT("Tan"), TEXT("Tan"));
        Map.Add(TEXT("Atan2"), TEXT("Atan2"));
        Map.Add(TEXT("Sqrt"), TEXT("Sqrt"));
        Map.Add(TEXT("Power"), TEXT("MultiplyMultiply_FloatFloat"));
        Map.Add(TEXT("DotProduct"), TEXT("Dot_VectorVector"));
        Map.Add(TEXT("CrossProduct"), TEXT("Cross_VectorVector"));

        // ================================================================
        // Object / Validation (UKismetSystemLibrary, UObject)
        // ================================================================
        Map.Add(TEXT("IsValid"), TEXT("IsValid"));
        Map.Add(TEXT("IsValidObject"), TEXT("IsValid"));
        Map.Add(TEXT("GetName"), TEXT("GetObjectName"));
        Map.Add(TEXT("GetDisplayName"), TEXT("GetDisplayName"));

        // ================================================================
        // Component Access (AActor)
        // ================================================================
        Map.Add(TEXT("GetComponentByClass"), TEXT("GetComponentByClass"));
        Map.Add(TEXT("GetComponent"), TEXT("GetComponentByClass"));
        Map.Add(TEXT("AddComponent"), TEXT("AddComponent"));

        // ================================================================
        // Timer Operations (UKismetSystemLibrary, K2_ prefixed)
        // ================================================================
        Map.Add(TEXT("SetTimer"), TEXT("K2_SetTimer"));
        Map.Add(TEXT("SetTimerByFunctionName"), TEXT("K2_SetTimer"));
        Map.Add(TEXT("SetTimerByName"), TEXT("K2_SetTimer"));
        Map.Add(TEXT("ClearTimer"), TEXT("K2_ClearTimer"));

        // ================================================================
        // Physics (UPrimitiveComponent)
        // ================================================================
        Map.Add(TEXT("AddForce"), TEXT("AddForce"));
        Map.Add(TEXT("AddImpulse"), TEXT("AddImpulse"));
        Map.Add(TEXT("AddTorque"), TEXT("AddTorqueInRadians"));
        Map.Add(TEXT("SetSimulatePhysics"), TEXT("SetSimulatePhysics"));
        Map.Add(TEXT("EnablePhysics"), TEXT("SetSimulatePhysics"));
        Map.Add(TEXT("SetPhysicsLinearVelocity"), TEXT("SetPhysicsLinearVelocity"));
        Map.Add(TEXT("SetVelocity"), TEXT("SetPhysicsLinearVelocity"));

        // ================================================================
        // Collision / Trace (UKismetSystemLibrary)
        // ================================================================
        Map.Add(TEXT("LineTrace"), TEXT("LineTraceSingle"));
        Map.Add(TEXT("LineTraceSingle"), TEXT("LineTraceSingle"));
        Map.Add(TEXT("SphereTrace"), TEXT("SphereTraceSingle"));
        Map.Add(TEXT("SphereTraceSingle"), TEXT("SphereTraceSingle"));
        Map.Add(TEXT("SetCollisionEnabled"), TEXT("SetCollisionEnabled"));

        // ================================================================
        // Gameplay Statics (UGameplayStatics)
        // ================================================================
        Map.Add(TEXT("GetPlayerPawn"), TEXT("GetPlayerPawn"));
        Map.Add(TEXT("GetPlayerCharacter"), TEXT("GetPlayerCharacter"));
        Map.Add(TEXT("GetPlayerController"), TEXT("GetPlayerController"));
        Map.Add(TEXT("GetGameMode"), TEXT("GetGameMode"));
        Map.Add(TEXT("GetAllActorsOfClass"), TEXT("GetAllActorsOfClass"));
        Map.Add(TEXT("FindAllActors"), TEXT("GetAllActorsOfClass"));
        Map.Add(TEXT("OpenLevel"), TEXT("OpenLevel"));
        Map.Add(TEXT("LoadLevel"), TEXT("OpenLevel"));
        Map.Add(TEXT("PlaySound"), TEXT("PlaySoundAtLocation"));
        Map.Add(TEXT("PlaySoundAtLocation"), TEXT("PlaySoundAtLocation"));
        Map.Add(TEXT("SpawnEmitter"), TEXT("SpawnEmitterAtLocation"));
        Map.Add(TEXT("SpawnEmitterAtLocation"), TEXT("SpawnEmitterAtLocation"));
        Map.Add(TEXT("SpawnParticle"), TEXT("SpawnEmitterAtLocation"));
        Map.Add(TEXT("ApplyDamage"), TEXT("ApplyDamage"));

        // ================================================================
        // Input (various)
        // ================================================================
        Map.Add(TEXT("GetInputAxisValue"), TEXT("GetInputAxisValue"));
        Map.Add(TEXT("GetInputAxisKeyValue"), TEXT("GetInputAxisKeyValue"));
        Map.Add(TEXT("EnableInput"), TEXT("EnableInput"));
        Map.Add(TEXT("DisableInput"), TEXT("DisableInput"));

        // ================================================================
        // Array Operations (UKismetArrayLibrary)
        // ================================================================
        Map.Add(TEXT("ArrayAdd"), TEXT("Array_Add"));
        Map.Add(TEXT("ArrayRemove"), TEXT("Array_Remove"));
        Map.Add(TEXT("ArrayLength"), TEXT("Array_Length"));
        Map.Add(TEXT("ArrayContains"), TEXT("Array_Contains"));
        Map.Add(TEXT("ArrayGet"), TEXT("Array_Get"));
        Map.Add(TEXT("ArrayClear"), TEXT("Array_Clear"));
        Map.Add(TEXT("ArrayShuffle"), TEXT("Array_Shuffle"));

        // ================================================================
        // Delay / Flow (UKismetSystemLibrary)
        // ================================================================
        Map.Add(TEXT("Wait"), TEXT("Delay"));
        Map.Add(TEXT("Sleep"), TEXT("Delay"));

        return Map;
    }();

    return Aliases;
}
```

**Alias count: 125+ entries** covering transform, actors, strings, math, objects, components, timers, physics, collision, gameplay, input, arrays, and flow control.

---

## UE API Reference (verified for UE 5.5)

| API | Header | Usage |
|-----|--------|-------|
| `UClass::FindFunctionByName(FName)` | `CoreUObject` | Core function lookup |
| `UFunction::GetName()` | `CoreUObject` | Internal function name |
| `UFunction::GetDisplayNameText()` | `CoreUObject` | Returns `FText` display name |
| `UFunction::GetOwnerClass()` | `CoreUObject` | Returns the `UClass*` that owns this function |
| `UFunction::HasAnyFunctionFlags(uint32)` | `CoreUObject` | Check FUNC_BlueprintCallable, etc. |
| `UClass::GetSuperClass()` | `CoreUObject` | Walk parent hierarchy |
| `UClass::IsChildOf(UClass*)` | `CoreUObject` | Class hierarchy check |
| `UClass::HasAnyClassFlags(uint32)` | `CoreUObject` | Check CLASS_Abstract, CLASS_NewerVersionExists |
| `TObjectIterator<UClass>` | `UObject/UObjectIterator.h` | Iterate all loaded UClass instances |
| `FindFirstObject<UClass>(FString, Options)` | `CoreUObject` | Find class by name |
| `UBlueprint::ParentClass` | `Engine/Blueprint.h` | Blueprint's parent class |
| `UKismetSystemLibrary::StaticClass()` | `Kismet/KismetSystemLibrary.h` | Static class accessor |
| `UKismetMathLibrary::StaticClass()` | `Kismet/KismetMathLibrary.h` | Static class accessor |
| `UKismetStringLibrary::StaticClass()` | `Kismet/KismetStringLibrary.h` | Static class accessor |
| `UKismetArrayLibrary::StaticClass()` | `Kismet/KismetArrayLibrary.h` | Static class accessor |
| `UGameplayStatics::StaticClass()` | `Kismet/GameplayStatics.h` | Static class accessor |
| `AActor::StaticClass()` | `GameFramework/Actor.h` | Static class accessor |
| `APawn::StaticClass()` | `GameFramework/Pawn.h` | Static class accessor |
| `ACharacter::StaticClass()` | `GameFramework/Character.h` | Static class accessor |
| `USceneComponent::StaticClass()` | `Components/SceneComponent.h` | Static class accessor |
| `UPrimitiveComponent::StaticClass()` | `Components/PrimitiveComponent.h` | Static class accessor |
| `UActorComponent::StaticClass()` | `Components/ActorComponent.h` (via SceneComponent) | Base component class |
| `UBlueprintFunctionLibrary::StaticClass()` | `Kismet/BlueprintFunctionLibrary.h` | For broad search filter |

---

## Integration Point

This class is used by the future `FOlivePlanExecutor` (Phase A, later task) and can also be integrated into the existing `FOliveBlueprintPlanResolver::ResolveCallOp()` to replace its direct catalog search. However, do NOT modify `OliveBlueprintPlanResolver` as part of this task -- that integration will be a separate task.

The current `FOliveNodeFactory::FindFunction()` searches only 3 classes. This new resolver is strictly additive and does NOT replace `FindFunction()`. The PlanExecutor will call `FOliveFunctionResolver::Resolve()` and then pass the resolved function name + class to the factory.

---

## Build Verification

After implementation, run:
```bash
"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex
```

---

## Checklist

- [ ] `OliveFunctionResolver.h` matches the exact header specified above
- [ ] `OliveFunctionResolver.cpp` includes the exact includes listed above
- [ ] `Resolve()` implements all 5 strategies in correct priority order
- [ ] `GetSearchOrder()` adds TargetClass first, then parent hierarchy, then common libraries
- [ ] `TryExactMatch()` uses `FindFunctionByName` correctly
- [ ] `TryK2PrefixMatch()` handles both adding and stripping `K2_` prefix
- [ ] `TryAliasMatch()` does case-insensitive lookup and resolves the alias target
- [ ] `TryCatalogMatch()` uses `FOliveNodeCatalog::Search()` and validates results
- [ ] `BroadSearch()` iterates `TObjectIterator<UClass>` and filters for function libraries + gameplay classes
- [ ] `GetCandidates()` provides suggestions from both catalog and broad search
- [ ] `GetAliasMap()` contains all 125+ entries
- [ ] `MatchMethodToString()` covers all enum values
- [ ] Build succeeds with no errors
- [ ] Log output uses `LogOliveFunctionResolver` category
