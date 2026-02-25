// Copyright Bode Software. All Rights Reserved.

#include "OliveFunctionResolver.h"
#include "OliveClassResolver.h"

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
#include "Components/ActorComponent.h"

// SCS includes (for component class search)
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

// Iterator for broad search
#include "UObject/UObjectIterator.h"

// Catalog for fuzzy search
#include "OliveNodeCatalog.h"

DEFINE_LOG_CATEGORY(LogOliveFunctionResolver);

// ============================================================================
// Resolve (main entry point)
// ============================================================================

FOliveFunctionMatch FOliveFunctionResolver::Resolve(
    const FString& FunctionName,
    const FString& TargetClass,
    UBlueprint* Blueprint)
{
    // Guard: empty function name
    if (FunctionName.IsEmpty())
    {
        return FOliveFunctionMatch();
    }

    // Build the class search order
    TArray<UClass*> SearchOrder = GetSearchOrder(TargetClass, Blueprint);

    // STRATEGY 1: Exact match on each class in search order
    for (UClass* Class : SearchOrder)
    {
        UFunction* Found = TryExactMatch(FunctionName, Class);
        if (Found)
        {
            FOliveFunctionMatch Match;
            Match.Function = Found;
            Match.OwningClass = Class;
            // Distinguish component class matches from regular exact matches
            if (Class->IsChildOf(UActorComponent::StaticClass()))
            {
                Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::ComponentClassSearch;
            }
            else
            {
                Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::ExactName;
            }
            Match.Confidence = 100;
            Match.DisplayName = Found->GetDisplayNameText().ToString();
            return Match;
        }
    }

    // STRATEGY 2: K2_ prefix manipulation on each class in search order
    for (UClass* Class : SearchOrder)
    {
        FString ResolvedName;
        UFunction* Found = TryK2PrefixMatch(FunctionName, Class, ResolvedName);
        if (Found)
        {
            UE_LOG(LogOliveFunctionResolver, Log,
                TEXT("K2 prefix resolved '%s' -> '%s' on %s"),
                *FunctionName, *ResolvedName, *Class->GetName());

            FOliveFunctionMatch Match;
            Match.Function = Found;
            Match.OwningClass = Class;
            // Distinguish component class matches from regular K2 prefix matches
            if (Class->IsChildOf(UActorComponent::StaticClass()))
            {
                Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::ComponentClassSearch;
                Match.Confidence = 95;
            }
            else
            {
                Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::K2Prefix;
                Match.Confidence = 95;
            }
            Match.DisplayName = Found->GetDisplayNameText().ToString();
            return Match;
        }
    }

    // STRATEGY 3: Alias map lookup
    {
        FString AliasResolved;
        UFunction* Found = TryAliasMatch(FunctionName, AliasResolved);
        if (Found)
        {
            UE_LOG(LogOliveFunctionResolver, Log,
                TEXT("Alias resolved '%s' -> '%s'"), *FunctionName, *AliasResolved);

            FOliveFunctionMatch Match;
            Match.Function = Found;
            Match.OwningClass = Found->GetOwnerClass();
            Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::Alias;
            Match.Confidence = 90;
            Match.DisplayName = Found->GetDisplayNameText().ToString();
            return Match;
        }
    }

    // STRATEGY 4: Node catalog match
    {
        FOliveFunctionMatch CatalogMatch = TryCatalogMatch(FunctionName);
        if (CatalogMatch.IsValid())
        {
            return CatalogMatch;
        }
    }

    // STRATEGY 5: Broad search across all function libraries and gameplay classes
    // Uses relevance-aware scoring: function libraries get 70, components on this
    // Blueprint get 90, unrelated gameplay classes get 40. Only accepts matches
    // at confidence >= 60 to prevent irrelevant class matching (e.g., SetSpeed
    // matching WindDirectionalSourceComponent on a bullet Blueprint).
    {
        TArray<FOliveFunctionMatch> BroadResults = BroadSearch(FunctionName, 5, Blueprint);
        if (BroadResults.Num() > 0)
        {
            const FOliveFunctionMatch& Best = BroadResults[0];

            // Minimum acceptance threshold: reject low-confidence broad matches
            // so they fall through to the failure path with suggestions instead
            static constexpr int32 BroadSearchAcceptanceThreshold = 60;
            if (Best.Confidence >= BroadSearchAcceptanceThreshold)
            {
                return Best;
            }

            // Below threshold: log rejection and fall through to failure path.
            // GetCandidates will still surface the low-confidence match as a suggestion.
            UE_LOG(LogOliveFunctionResolver, Warning,
                TEXT("BroadSearch found '%s' on %s but confidence %d < threshold %d. Rejecting."),
                *FunctionName,
                Best.OwningClass ? *Best.OwningClass->GetName() : TEXT("?"),
                Best.Confidence,
                BroadSearchAcceptanceThreshold);
        }
    }

    // FAILURE: No match found
    UE_LOG(LogOliveFunctionResolver, Warning,
        TEXT("Function '%s' could not be resolved by any strategy"), *FunctionName);
    return FOliveFunctionMatch();
}

// ============================================================================
// GetSearchOrder
// ============================================================================

TArray<UClass*> FOliveFunctionResolver::GetSearchOrder(
    const FString& TargetClass,
    UBlueprint* Blueprint)
{
    TArray<UClass*> Result;
    TSet<UClass*> Added;

    // 1. Target class (if specified)
    if (!TargetClass.IsEmpty())
    {
        UClass* TC = FindClassByName(TargetClass);
        if (TC && !Added.Contains(TC))
        {
            Result.Add(TC);
            Added.Add(TC);
        }
    }

    // 2. Blueprint parent class hierarchy
    if (Blueprint && Blueprint->ParentClass)
    {
        UClass* Current = Blueprint->ParentClass;
        while (Current)
        {
            if (!Added.Contains(Current))
            {
                Result.Add(Current);
                Added.Add(Current);
            }
            Current = Current->GetSuperClass();
        }
    }

    // 2.5: Component classes on this Blueprint's SCS
    // This ensures functions like SetSpeed are found on the Blueprint's own
    // ProjectileMovementComponent before falling through to BroadSearch
    // where they might match an irrelevant class.
    if (Blueprint && Blueprint->SimpleConstructionScript)
    {
        TArray<USCS_Node*> AllSCSNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
        for (USCS_Node* SCSNode : AllSCSNodes)
        {
            if (SCSNode && SCSNode->ComponentClass && !Added.Contains(SCSNode->ComponentClass))
            {
                Result.Add(SCSNode->ComponentClass);
                Added.Add(SCSNode->ComponentClass);
            }
        }
    }

    // 3. Common library classes (in priority order)
    UClass* CommonClasses[] = {
        UKismetSystemLibrary::StaticClass(),
        UKismetMathLibrary::StaticClass(),
        UKismetStringLibrary::StaticClass(),
        UKismetArrayLibrary::StaticClass(),
        UGameplayStatics::StaticClass(),
        UObject::StaticClass(),
        AActor::StaticClass(),
        USceneComponent::StaticClass(),
        UPrimitiveComponent::StaticClass(),
        APawn::StaticClass(),
        ACharacter::StaticClass(),
    };

    for (UClass* Class : CommonClasses)
    {
        if (!Added.Contains(Class))
        {
            Result.Add(Class);
            Added.Add(Class);
        }
    }

    return Result;
}

// ============================================================================
// TryExactMatch
// ============================================================================

UFunction* FOliveFunctionResolver::TryExactMatch(const FString& FunctionName, UClass* Class)
{
    if (!Class)
    {
        return nullptr;
    }
    return Class->FindFunctionByName(FName(*FunctionName));
}

// ============================================================================
// TryK2PrefixMatch
// ============================================================================

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

// ============================================================================
// TryAliasMatch
// ============================================================================

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

// ============================================================================
// TryCatalogMatch
// ============================================================================

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

// ============================================================================
// BroadSearch
// ============================================================================

TArray<FOliveFunctionMatch> FOliveFunctionResolver::BroadSearch(
    const FString& FunctionName,
    int32 MaxResults,
    UBlueprint* Blueprint)
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
            // Relevance-aware confidence scoring:
            //   Function library (designed for general use):    70
            //   Gameplay class present on this Blueprint's SCS: 90
            //   Gameplay class NOT on this Blueprint (or no BP): 40
            //   Fallback:                                        40
            int32 Confidence = 40;

            if (bIsFunctionLibrary)
            {
                Confidence = 70;
            }
            else if (bIsGameplayClass)
            {
                // Check if this class matches a component on the Blueprint's SCS
                bool bIsOnBlueprint = false;
                if (Blueprint && Blueprint->SimpleConstructionScript)
                {
                    for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
                    {
                        if (SCSNode && SCSNode->ComponentClass &&
                            Class->IsChildOf(SCSNode->ComponentClass))
                        {
                            bIsOnBlueprint = true;
                            break;
                        }
                    }
                }
                Confidence = bIsOnBlueprint ? 90 : 40;
            }

            FOliveFunctionMatch Match;
            Match.Function = Found;
            Match.OwningClass = Class;
            Match.MatchMethod = FOliveFunctionMatch::EMatchMethod::BroadClassSearch;
            Match.Confidence = Confidence;
            Match.DisplayName = Found->GetDisplayNameText().ToString();

            Results.Add(Match);

            UE_LOG(LogOliveFunctionResolver, Log,
                TEXT("Broad search found '%s' on %s (confidence=%d, library=%d, onBP=%d)"),
                *FunctionName, *Class->GetName(), Match.Confidence,
                bIsFunctionLibrary ? 1 : 0,
                (bIsGameplayClass && Confidence == 90) ? 1 : 0);

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

// ============================================================================
// GetCandidates
// ============================================================================

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
            FunctionName, MaxResults - Candidates.Num(), nullptr);
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

// ============================================================================
// MatchMethodToString
// ============================================================================

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
        case FOliveFunctionMatch::EMatchMethod::ComponentClassSearch: return TEXT("component_class_search");
        default:                                                     return TEXT("unknown");
    }
}

// ============================================================================
// FindClassByName
// ============================================================================

UClass* FOliveFunctionResolver::FindClassByName(const FString& ClassName)
{
    if (ClassName.IsEmpty())
    {
        return nullptr;
    }

    FOliveClassResolveResult Result = FOliveClassResolver::Resolve(ClassName);
    return Result.Class;  // nullptr if not found
}

// ============================================================================
// GetAliasMap
// ============================================================================

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
        Map.Add(TEXT("Format"), TEXT("Format"));
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
        Map.Add(TEXT("GetClass"), TEXT("GetClass"));
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

        // ================================================================
        // Transform Construction (UKismetMathLibrary)
        // ================================================================
        Map.Add(TEXT("MakeTransform"), TEXT("MakeTransform"));
        Map.Add(TEXT("BreakTransform"), TEXT("BreakTransform"));
        Map.Add(TEXT("MakeVector"), TEXT("MakeVector"));
        Map.Add(TEXT("BreakVector"), TEXT("BreakVector"));
        Map.Add(TEXT("MakeRotator"), TEXT("MakeRotator"));
        Map.Add(TEXT("BreakRotator"), TEXT("BreakRotator"));
        Map.Add(TEXT("MakeVector2D"), TEXT("MakeVector2D"));
        Map.Add(TEXT("BreakVector2D"), TEXT("BreakVector2D"));

        // ================================================================
        // Commonly Attempted Names
        // ================================================================
        Map.Add(TEXT("GetActorTransform"), TEXT("GetActorTransform"));
        Map.Add(TEXT("SetMaterial"), TEXT("SetMaterial"));
        Map.Add(TEXT("SetCollisionProfileName"), TEXT("SetCollisionProfileName"));
        Map.Add(TEXT("SetVisibility"), TEXT("SetVisibility"));
        Map.Add(TEXT("SetHiddenInGame"), TEXT("SetHiddenInGame"));

        // ================================================================
        // Movement
        // ================================================================
        Map.Add(TEXT("LaunchCharacter"), TEXT("LaunchCharacter"));
        Map.Add(TEXT("AddMovementInput"), TEXT("AddMovementInput"));
        Map.Add(TEXT("GetVelocity"), TEXT("GetVelocity"));

        return Map;
    }();

    return Aliases;
}
