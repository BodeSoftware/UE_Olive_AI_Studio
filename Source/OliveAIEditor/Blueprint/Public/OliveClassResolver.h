// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UClass;
class UBlueprint;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveClassResolver, Log, All);

/**
 * Result of class resolution with provenance information.
 * Returned by FOliveClassResolver::Resolve() to tell callers both
 * the resolved UClass* and how it was found.
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
		SuffixMatch,         // Added _C suffix (Blueprint generated class name)
		NativeClassPath,     // /Script/Engine.AClassName etc.
		BlueprintAssetPath,  // Loaded UBlueprint from full asset path
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
 *   0. Early-out for empty/whitespace input
 *   1. LRU cache lookup (TWeakObjectPtr, auto-invalidates GC'd classes)
 *   2. FindFirstObject<UClass>(Name, NativeFirst)
 *   3. FindFirstObject with A/U prefixes
 *   4. FindFirstObject with _C suffix (Blueprint generated class names)
 *   5. Fully-qualified native class paths (/Script/Engine.AName, etc.)
 *   6. Blueprint path (if contains '/') -- LoadObject<UBlueprint>
 *   7. Asset registry search for short Blueprint names (no '/')
 *      a. Try common content paths first (fast)
 *      b. Fall back to full registry scan filtered by UBlueprint class
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
	 * @param ClassNameOrPath  Short name ("Actor", "BP_Bullet"), prefixed name ("AActor"),
	 *                         generated class name ("BP_Bullet_C"), or full asset path
	 *                         ("/Game/Blueprints/BP_Bullet")
	 * @return Resolution result with UClass* and EResolveMethod. Check IsValid() for success.
	 */
	static FOliveClassResolveResult Resolve(const FString& ClassNameOrPath);

	/**
	 * Clear the resolution cache.
	 * Call when assets are added/removed/renamed.
	 */
	static void ClearCache();

	/** Maximum number of entries in the LRU cache before eviction */
	static constexpr int32 MaxCacheSize = 256;

private:
	// ====================================================================
	// Resolution Strategies (tried in order, cheapest to most expensive)
	// ====================================================================

	/**
	 * Strategy 2: Direct lookup via FindFirstObject<UClass>(Name, NativeFirst).
	 * @param Name The exact class name to search for
	 * @return UClass* if found, nullptr otherwise
	 */
	static UClass* TryDirectLookup(const FString& Name);

	/**
	 * Strategy 3: Try adding A/U prefixes to the name.
	 * Handles cases where AI provides "Actor" instead of "AActor".
	 * @param Name The unprefixed class name
	 * @return UClass* if found with a prefix, nullptr otherwise
	 */
	static UClass* TryPrefixLookup(const FString& Name);

	/**
	 * Strategy 4: Try appending _C suffix for Blueprint generated class names.
	 * Handles cases where AI provides "BP_Bullet" and the generated class is "BP_Bullet_C".
	 * @param Name The class name without _C suffix
	 * @return UClass* if found with _C suffix, nullptr otherwise
	 */
	static UClass* TrySuffixLookup(const FString& Name);

	/**
	 * Strategy 5: Try fully-qualified native class paths via StaticLoadClass.
	 * Tries /Script/Engine.AName, /Script/Engine.UName, /Script/CoreUObject.UName, etc.
	 * @param Name The class name (with or without prefix)
	 * @return UClass* if found via native path, nullptr otherwise
	 */
	static UClass* TryNativeClassPaths(const FString& Name);

	/**
	 * Strategy 6: Load a Blueprint from a full asset path (path contains '/').
	 * Uses LoadObject<UBlueprint> and returns its GeneratedClass.
	 * Also tries appending _C for class object references.
	 * @param Path The full asset path (e.g., "/Game/Blueprints/BP_Bullet")
	 * @return UClass* (the GeneratedClass) if found, nullptr otherwise
	 */
	static UClass* TryBlueprintPath(const FString& Path);

	/**
	 * Strategy 7: Search the asset registry for a Blueprint with the given short name.
	 * Two sub-strategies:
	 *   a. Try common content paths first (fast, O(1) lookups per path)
	 *   b. Fall back to full registry scan filtered by UBlueprint class
	 * @param ShortName The Blueprint asset name without any path (e.g., "BP_Bullet")
	 * @return UClass* (the GeneratedClass) if found, nullptr otherwise
	 */
	static UClass* TryAssetRegistrySearch(const FString& ShortName);

	// ====================================================================
	// LRU Cache
	// ====================================================================

	/** Single cache entry holding a weak class pointer and LRU access order */
	struct FCacheEntry
	{
		TWeakObjectPtr<UClass> ClassPtr;
		int32 AccessOrder = 0;   // Higher = more recently used
	};

	/** Get the static cache map (lazy-initialized) */
	static TMap<FString, FCacheEntry>& GetCache();

	/** Get the static access counter (monotonically increasing) */
	static int32& GetAccessCounter();

	/**
	 * Look up a class in the cache.
	 * @param Key The original input string (cache key)
	 * @return UClass* if found and still valid, nullptr on miss or stale entry
	 */
	static UClass* CacheLookup(const FString& Key);

	/**
	 * Insert a resolved class into the cache with LRU eviction.
	 * If the cache is full (MaxCacheSize), the least recently used entry is evicted.
	 * @param Key   The original input string (cache key, case-preserved)
	 * @param Value The resolved UClass* to cache
	 */
	static void CacheInsert(const FString& Key, UClass* Value);
};
