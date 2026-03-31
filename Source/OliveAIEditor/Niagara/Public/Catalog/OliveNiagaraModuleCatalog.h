// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/NiagaraIR.h"

class FJsonObject;
class UNiagaraScript;

/**
 * Information about a single Niagara module script available in the module library.
 * Built from asset registry data during catalog initialization.
 */
struct OLIVEAIEDITOR_API FOliveNiagaraModuleInfo
{
	/** Full asset path (e.g., "/Niagara/Modules/UpdateAge.UpdateAge") */
	FString ScriptAssetPath;

	/** Friendly display name (e.g., "Update Age", "Add Velocity") */
	FString DisplayName;

	/** Category derived from asset path (e.g., "Forces", "Location", "Velocity") */
	FString Category;

	/** Description from asset metadata or tooltip */
	FString Description;

	/** Keywords for search matching */
	TArray<FString> Keywords;

	/** Which stages accept this module (e.g., ParticleUpdate, EmitterSpawn) */
	TArray<EOliveIRNiagaraStage> ValidStages;

	/** Serialize to JSON for tool results */
	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * FOliveNiagaraModuleCatalog
 *
 * Discovers and indexes all available Niagara module scripts via the asset registry.
 * Provides fuzzy search with optional stage filtering for AI tool use.
 *
 * Unlike the PCG catalog which uses GetDerivedClasses(), Niagara modules are
 * UNiagaraScript assets discovered at runtime. Only scripts with
 * ENiagaraScriptUsage::Module and library-level visibility are indexed.
 *
 * Usage:
 *   FOliveNiagaraModuleCatalog::Get().Initialize();
 *   auto Results = FOliveNiagaraModuleCatalog::Get().Search("velocity", EOliveIRNiagaraStage::ParticleUpdate);
 *   const auto* Module = FOliveNiagaraModuleCatalog::Get().GetByPath("/Niagara/Modules/AddVelocity.AddVelocity");
 */
class OLIVEAIEDITOR_API FOliveNiagaraModuleCatalog
{
public:
	/** Get the singleton instance */
	static FOliveNiagaraModuleCatalog& Get();

	/**
	 * Scan asset registry and cache all Niagara module scripts.
	 * Safe to call multiple times; subsequent calls are no-ops.
	 */
	void Initialize();

	/** Clear all cached data and mark as uninitialized */
	void Shutdown();

	/**
	 * Fuzzy search for modules matching a query, optionally filtered by stage.
	 * @param Query Search string to match against name, category, keywords, description
	 * @param FilterStage If not Unknown, only return modules valid for this stage
	 * @return Matching modules sorted by relevance (highest score first)
	 */
	TArray<FOliveNiagaraModuleInfo> Search(const FString& Query, EOliveIRNiagaraStage FilterStage = EOliveIRNiagaraStage::Unknown) const;

	/**
	 * Look up a module by its full asset path.
	 * @param ScriptPath Full object path (e.g., "/Niagara/Modules/UpdateAge.UpdateAge")
	 * @return Pointer to the cached info, or nullptr if not found
	 */
	const FOliveNiagaraModuleInfo* GetByPath(const FString& ScriptPath) const;

	/**
	 * Find and load a UNiagaraScript for a module, supporting both full paths
	 * and display name lookups.
	 * @param NameOrPath Full asset path (if contains "/") or display name
	 * @return Loaded UNiagaraScript*, or nullptr if not found
	 */
	UNiagaraScript* FindModuleScript(const FString& NameOrPath) const;

	/** Serialize the entire catalog to JSON with module list and count */
	TSharedPtr<FJsonObject> ToJson() const;

	/** @return Number of indexed modules */
	int32 GetModuleCount() const;

private:
	FOliveNiagaraModuleCatalog() = default;
	~FOliveNiagaraModuleCatalog() = default;
	FOliveNiagaraModuleCatalog(const FOliveNiagaraModuleCatalog&) = delete;
	FOliveNiagaraModuleCatalog& operator=(const FOliveNiagaraModuleCatalog&) = delete;

	/**
	 * Compute a relevance score for a module against a search query.
	 * @param Info Module to score
	 * @param Query Lowercased search query
	 * @return Score >= 0; higher is more relevant. 0 means no match.
	 */
	int32 ComputeSearchScore(const FOliveNiagaraModuleInfo& Info, const FString& Query) const;

	/** All discovered module infos */
	TArray<FOliveNiagaraModuleInfo> ModuleInfos;

	/** ScriptAssetPath -> index in ModuleInfos for O(1) lookup */
	TMap<FString, int32> PathIndex;

	/** DisplayName (lowercased) -> index in ModuleInfos for name-based lookup */
	TMap<FString, int32> DisplayNameIndex;

	bool bInitialized = false;
};
