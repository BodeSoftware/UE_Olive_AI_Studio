// Copyright Bode Software. All Rights Reserved.

/**
 * OliveClassResolver.cpp
 *
 * Implementation of the shared Blueprint/native class resolution utility.
 * Provides a 6-step resolution chain (plus cache) ordered from cheapest
 * to most expensive. Used by OliveNodeFactory, OliveBlueprintWriter,
 * and OliveFunctionResolver as a single source of truth for class lookups.
 */

#include "OliveClassResolver.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogOliveClassResolver);

// ============================================================================
// Resolve (main entry point)
// ============================================================================

FOliveClassResolveResult FOliveClassResolver::Resolve(const FString& ClassNameOrPath)
{
	FOliveClassResolveResult Result;

	// Step 0: Early-out for empty/whitespace input
	const FString Trimmed = ClassNameOrPath.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("Resolve('%s'): empty/whitespace input, returning invalid"), *ClassNameOrPath);
		return Result;
	}

	// Step 1: LRU cache lookup
	UClass* Cached = CacheLookup(Trimmed);
	if (Cached)
	{
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("Resolve('%s'): cache HIT -> %s"), *Trimmed, *Cached->GetName());
		Result.Class = Cached;
		Result.Method = FOliveClassResolveResult::EResolveMethod::Cached;
		return Result;
	}

	// Step 2: Direct lookup via FindFirstObject<UClass>
	UClass* Found = TryDirectLookup(Trimmed);
	if (Found)
	{
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("Resolve('%s'): direct lookup -> %s"), *Trimmed, *Found->GetName());
		Result.Class = Found;
		Result.Method = FOliveClassResolveResult::EResolveMethod::DirectLookup;
		CacheInsert(Trimmed, Found);
		return Result;
	}

	// Step 3: A/U prefix variants
	Found = TryPrefixLookup(Trimmed);
	if (Found)
	{
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("Resolve('%s'): prefix match -> %s"), *Trimmed, *Found->GetName());
		Result.Class = Found;
		Result.Method = FOliveClassResolveResult::EResolveMethod::PrefixMatch;
		CacheInsert(Trimmed, Found);
		return Result;
	}

	// Step 4: _C suffix for Blueprint generated class names
	Found = TrySuffixLookup(Trimmed);
	if (Found)
	{
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("Resolve('%s'): _C suffix match -> %s"), *Trimmed, *Found->GetName());
		Result.Class = Found;
		Result.Method = FOliveClassResolveResult::EResolveMethod::SuffixMatch;
		CacheInsert(Trimmed, Found);
		return Result;
	}

	// Step 5: Fully-qualified native class paths
	Found = TryNativeClassPaths(Trimmed);
	if (Found)
	{
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("Resolve('%s'): native class path -> %s"), *Trimmed, *Found->GetName());
		Result.Class = Found;
		Result.Method = FOliveClassResolveResult::EResolveMethod::NativeClassPath;
		CacheInsert(Trimmed, Found);
		return Result;
	}

	// Step 6: Blueprint asset path (if input contains '/')
	if (Trimmed.Contains(TEXT("/")))
	{
		Found = TryBlueprintPath(Trimmed);
		if (Found)
		{
			UE_LOG(LogOliveClassResolver, Verbose, TEXT("Resolve('%s'): Blueprint path -> %s"), *Trimmed, *Found->GetName());
			Result.Class = Found;
			Result.Method = FOliveClassResolveResult::EResolveMethod::BlueprintAssetPath;
			CacheInsert(Trimmed, Found);
			return Result;
		}
	}

	// Step 7: Asset registry search for short Blueprint names (no '/')
	if (!Trimmed.Contains(TEXT("/")))
	{
		Found = TryAssetRegistrySearch(Trimmed);
		if (Found)
		{
			UE_LOG(LogOliveClassResolver, Verbose, TEXT("Resolve('%s'): asset registry search -> %s"), *Trimmed, *Found->GetName());
			Result.Class = Found;
			Result.Method = FOliveClassResolveResult::EResolveMethod::BlueprintShortName;
			CacheInsert(Trimmed, Found);
			return Result;
		}
	}

	// All strategies exhausted
	UE_LOG(LogOliveClassResolver, Warning,
		TEXT("Resolve('%s'): FAILED - all resolution strategies exhausted "
			"(direct, prefix [A/U], suffix [_C], native paths, Blueprint path=%s, asset registry=%s)"),
		*Trimmed,
		Trimmed.Contains(TEXT("/")) ? TEXT("tried") : TEXT("skipped (no '/')"),
		Trimmed.Contains(TEXT("/")) ? TEXT("skipped (has '/')") : TEXT("tried"));

	return Result;
}

// ============================================================================
// ClearCache
// ============================================================================

void FOliveClassResolver::ClearCache()
{
	GetCache().Empty();
	GetAccessCounter() = 0;
	UE_LOG(LogOliveClassResolver, Verbose, TEXT("Cache cleared"));
}

// ============================================================================
// Strategy 2: Direct Lookup
// ============================================================================

UClass* FOliveClassResolver::TryDirectLookup(const FString& Name)
{
	UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryDirectLookup('%s')"), *Name);
	UClass* Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
	if (Found)
	{
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryDirectLookup('%s'): found -> %s"), *Name, *Found->GetPathName());
	}
	return Found;
}

// ============================================================================
// Strategy 3: Prefix Lookup (A/U)
// ============================================================================

UClass* FOliveClassResolver::TryPrefixLookup(const FString& Name)
{
	// Skip if the name already starts with A or U (would duplicate direct lookup)
	static const TCHAR* Prefixes[] = { TEXT("A"), TEXT("U") };

	for (const TCHAR* Prefix : Prefixes)
	{
		// If the name already starts with this prefix, skip to avoid "AActor" -> "AActor" duplicate
		if (Name.Len() > 1 && Name[0] == Prefix[0] && FChar::IsUpper(Name[1]))
		{
			continue;
		}

		FString PrefixedName = FString(Prefix) + Name;
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryPrefixLookup: trying '%s'"), *PrefixedName);

		UClass* Found = FindFirstObject<UClass>(*PrefixedName, EFindFirstObjectOptions::NativeFirst);
		if (Found)
		{
			UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryPrefixLookup: found '%s' -> %s"), *PrefixedName, *Found->GetPathName());
			return Found;
		}
	}

	return nullptr;
}

// ============================================================================
// Strategy 4: Suffix Lookup (_C)
// ============================================================================

UClass* FOliveClassResolver::TrySuffixLookup(const FString& Name)
{
	// Only try _C suffix if the name doesn't already end with _C
	if (Name.EndsWith(TEXT("_C")))
	{
		return nullptr;
	}

	FString SuffixedName = Name + TEXT("_C");
	UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TrySuffixLookup: trying '%s'"), *SuffixedName);

	UClass* Found = FindFirstObject<UClass>(*SuffixedName, EFindFirstObjectOptions::NativeFirst);
	if (Found)
	{
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TrySuffixLookup: found '%s' -> %s"), *SuffixedName, *Found->GetPathName());
	}
	return Found;
}

// ============================================================================
// Strategy 5: Native Class Paths
// ============================================================================

UClass* FOliveClassResolver::TryNativeClassPaths(const FString& Name)
{
	// Build a set of fully-qualified paths to try.
	// Common modules: Engine, CoreUObject, GameplayAbilities, AIModule, NavigationSystem, etc.
	TArray<FString> ClassPaths;

	// Try with the name as-is (it may already have the prefix)
	ClassPaths.Add(FString::Printf(TEXT("/Script/Engine.%s"), *Name));
	ClassPaths.Add(FString::Printf(TEXT("/Script/CoreUObject.%s"), *Name));

	// Also try with A and U prefixes if not already there
	if (!(Name.Len() > 1 && Name[0] == TEXT('A') && FChar::IsUpper(Name[1])))
	{
		ClassPaths.Add(FString::Printf(TEXT("/Script/Engine.A%s"), *Name));
	}
	if (!(Name.Len() > 1 && Name[0] == TEXT('U') && FChar::IsUpper(Name[1])))
	{
		ClassPaths.Add(FString::Printf(TEXT("/Script/Engine.U%s"), *Name));
		ClassPaths.Add(FString::Printf(TEXT("/Script/CoreUObject.U%s"), *Name));
	}

	// Additional common modules
	ClassPaths.Add(FString::Printf(TEXT("/Script/GameplayAbilities.%s"), *Name));
	ClassPaths.Add(FString::Printf(TEXT("/Script/AIModule.%s"), *Name));
	ClassPaths.Add(FString::Printf(TEXT("/Script/NavigationSystem.%s"), *Name));
	ClassPaths.Add(FString::Printf(TEXT("/Script/UMG.%s"), *Name));
	ClassPaths.Add(FString::Printf(TEXT("/Script/EnhancedInput.%s"), *Name));

	for (const FString& ClassPath : ClassPaths)
	{
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryNativeClassPaths: trying '%s'"), *ClassPath);

		UClass* Found = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
		if (Found)
		{
			UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryNativeClassPaths: found '%s' -> %s"), *ClassPath, *Found->GetPathName());
			return Found;
		}
	}

	return nullptr;
}

// ============================================================================
// Strategy 6: Blueprint Asset Path
// ============================================================================

UClass* FOliveClassResolver::TryBlueprintPath(const FString& Path)
{
	UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryBlueprintPath: trying '%s'"), *Path);

	// Try loading as a Blueprint asset
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Path);
	if (BP)
	{
		if (BP->GeneratedClass)
		{
			UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryBlueprintPath: found Blueprint '%s' -> GeneratedClass=%s"),
				*Path, *BP->GeneratedClass->GetName());
			return BP->GeneratedClass;
		}
		else
		{
			UE_LOG(LogOliveClassResolver, Warning,
				TEXT("  TryBlueprintPath: found Blueprint '%s' but GeneratedClass is null (broken Blueprint?)"), *Path);
		}
	}

	// If path ends with _C, strip it and retry as Blueprint asset path
	// e.g. "/Game/Blueprints/BPI_Interactable_C" -> "/Game/Blueprints/BPI_Interactable"
	if (Path.EndsWith(TEXT("_C")))
	{
		FString StrippedPath = Path.LeftChop(2);
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryBlueprintPath: stripping _C suffix, trying '%s'"), *StrippedPath);

		UBlueprint* StrippedBP = LoadObject<UBlueprint>(nullptr, *StrippedPath);
		if (StrippedBP && StrippedBP->GeneratedClass)
		{
			UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryBlueprintPath: found Blueprint '%s' -> GeneratedClass=%s"),
				*StrippedPath, *StrippedBP->GeneratedClass->GetName());
			return StrippedBP->GeneratedClass;
		}
	}

	// Also try as a class object reference (path ending with _C)
	if (!Path.EndsWith(TEXT("_C")))
	{
		FString ClassPath = Path + TEXT("_C");
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryBlueprintPath: trying class ref '%s'"), *ClassPath);

		UClass* Found = LoadObject<UClass>(nullptr, *ClassPath);
		if (Found)
		{
			UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryBlueprintPath: found class ref '%s' -> %s"),
				*ClassPath, *Found->GetPathName());
			return Found;
		}
	}

	return nullptr;
}

// ============================================================================
// Strategy 7: Asset Registry Search
// ============================================================================

UClass* FOliveClassResolver::TryAssetRegistrySearch(const FString& ShortName)
{
	UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryAssetRegistrySearch('%s'): starting"), *ShortName);

	FAssetRegistryModule* AssetRegistryModule = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
	if (!AssetRegistryModule)
	{
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryAssetRegistrySearch: AssetRegistry module not available"));
		return nullptr;
	}

	IAssetRegistry& Registry = AssetRegistryModule->Get();

	// ----------------------------------------------------------------
	// Strategy 7a: Try common content paths first (fast, O(1) per path)
	// Each path is tried both as a package path and as an object path.
	// ----------------------------------------------------------------
	const TArray<FString> CommonPrefixes = {
		FString::Printf(TEXT("/Game/Blueprints/%s"), *ShortName),
		FString::Printf(TEXT("/Game/%s"), *ShortName),
		FString::Printf(TEXT("/Game/Characters/%s"), *ShortName),
		FString::Printf(TEXT("/Game/Weapons/%s"), *ShortName),
		FString::Printf(TEXT("/Game/Items/%s"), *ShortName),
	};

	for (const FString& BasePath : CommonPrefixes)
	{
		// GetAssetByObjectPath needs the full object path format: /Game/Path/Name.Name
		FString ObjectPath = BasePath + TEXT(".") + ShortName;
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("  TryAssetRegistrySearch(7a): trying '%s'"), *ObjectPath);

		FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		if (AssetData.IsValid())
		{
			UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
			if (BP)
			{
				if (BP->GeneratedClass)
				{
					UE_LOG(LogOliveClassResolver, Verbose,
						TEXT("  TryAssetRegistrySearch(7a): found at '%s' -> %s"),
						*ObjectPath, *BP->GeneratedClass->GetName());
					return BP->GeneratedClass;
				}
				else
				{
					UE_LOG(LogOliveClassResolver, Warning,
						TEXT("  TryAssetRegistrySearch(7a): found Blueprint at '%s' but GeneratedClass is null, skipping"),
						*ObjectPath);
				}
			}
		}
	}

	// ----------------------------------------------------------------
	// Strategy 7b: Full registry scan filtered by UBlueprint class
	// Only queries metadata first, only loads assets on name match.
	// ----------------------------------------------------------------
	UE_LOG(LogOliveClassResolver, Verbose,
		TEXT("  TryAssetRegistrySearch(7b): common paths exhausted, scanning full registry for '%s'"), *ShortName);

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Results;
	Registry.GetAssets(Filter, Results);

	UE_LOG(LogOliveClassResolver, Verbose,
		TEXT("  TryAssetRegistrySearch(7b): scanning %d Blueprint assets for name '%s'"),
		Results.Num(), *ShortName);

	bool bFoundMultiple = false;
	UClass* FirstMatch = nullptr;
	FString FirstMatchPath;

	for (const FAssetData& Data : Results)
	{
		if (Data.AssetName.ToString().Equals(ShortName, ESearchCase::IgnoreCase))
		{
			UBlueprint* BP = Cast<UBlueprint>(Data.GetAsset());
			if (BP)
			{
				if (BP->GeneratedClass)
				{
					if (FirstMatch)
					{
						// Multiple matches found -- log warning, use first
						if (!bFoundMultiple)
						{
							UE_LOG(LogOliveClassResolver, Warning,
								TEXT("  TryAssetRegistrySearch(7b): multiple Blueprints named '%s' found: "
									"'%s' (using), '%s' (skipping). Returning first match."),
								*ShortName, *FirstMatchPath, *Data.GetObjectPathString());
							bFoundMultiple = true;
						}
					}
					else
					{
						FirstMatch = BP->GeneratedClass;
						FirstMatchPath = Data.GetObjectPathString();
						UE_LOG(LogOliveClassResolver, Verbose,
							TEXT("  TryAssetRegistrySearch(7b): found '%s' at '%s'"),
							*ShortName, *FirstMatchPath);
						// Don't return yet -- scan for duplicates to warn about ambiguity
					}
				}
				else
				{
					UE_LOG(LogOliveClassResolver, Warning,
						TEXT("  TryAssetRegistrySearch(7b): found Blueprint '%s' at '%s' but GeneratedClass is null, skipping"),
						*ShortName, *Data.GetObjectPathString());
				}
			}
		}
	}

	return FirstMatch;
}

// ============================================================================
// Cache: GetCache / GetAccessCounter
// ============================================================================

TMap<FString, FOliveClassResolver::FCacheEntry>& FOliveClassResolver::GetCache()
{
	static TMap<FString, FCacheEntry> Cache;
	return Cache;
}

int32& FOliveClassResolver::GetAccessCounter()
{
	static int32 Counter = 0;
	return Counter;
}

// ============================================================================
// Cache: Lookup
// ============================================================================

UClass* FOliveClassResolver::CacheLookup(const FString& Key)
{
	TMap<FString, FCacheEntry>& Cache = GetCache();
	FCacheEntry* Entry = Cache.Find(Key);
	if (!Entry)
	{
		return nullptr;
	}

	// Check if the weak pointer is still valid (class may have been GC'd)
	UClass* Class = Entry->ClassPtr.Get();
	if (!Class)
	{
		// Stale entry -- remove it
		UE_LOG(LogOliveClassResolver, Verbose, TEXT("  CacheLookup('%s'): stale entry (GC'd), removing"), *Key);
		Cache.Remove(Key);
		return nullptr;
	}

	// Update access order (LRU touch)
	Entry->AccessOrder = ++GetAccessCounter();
	return Class;
}

// ============================================================================
// Cache: Insert with LRU Eviction
// ============================================================================

void FOliveClassResolver::CacheInsert(const FString& Key, UClass* Value)
{
	if (!Value)
	{
		return;
	}

	TMap<FString, FCacheEntry>& Cache = GetCache();

	// If already cached, just update
	FCacheEntry* Existing = Cache.Find(Key);
	if (Existing)
	{
		Existing->ClassPtr = Value;
		Existing->AccessOrder = ++GetAccessCounter();
		return;
	}

	// Evict oldest entry if cache is full
	if (Cache.Num() >= MaxCacheSize)
	{
		FString OldestKey;
		int32 OldestOrder = INT32_MAX;

		for (const auto& Pair : Cache)
		{
			if (Pair.Value.AccessOrder < OldestOrder)
			{
				OldestOrder = Pair.Value.AccessOrder;
				OldestKey = Pair.Key;
			}
		}

		if (!OldestKey.IsEmpty())
		{
			UE_LOG(LogOliveClassResolver, Verbose,
				TEXT("  CacheInsert: evicting LRU entry '%s' (AccessOrder=%d) to make room for '%s'"),
				*OldestKey, OldestOrder, *Key);
			Cache.Remove(OldestKey);
		}
	}

	// Insert new entry
	FCacheEntry NewEntry;
	NewEntry.ClassPtr = Value;
	NewEntry.AccessOrder = ++GetAccessCounter();
	Cache.Add(Key, NewEntry);

	UE_LOG(LogOliveClassResolver, Verbose,
		TEXT("  CacheInsert('%s'): cached (size=%d/%d)"), *Key, Cache.Num(), MaxCacheSize);
}
