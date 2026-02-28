// Copyright Bode Software. All Rights Reserved.
//
// DEPRECATED: Tool pack filtering removed in AI Freedom update. This class is
// retained for one release cycle. All tool visibility is now handled by Focus
// Profiles (FOliveFocusProfileManager) and the tool registry directly.
// Do NOT add new callers. Remove this file after the deprecation period.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

/**
 * Tool pack identifiers
 * DEPRECATED: See file-level comment.
 */
enum class EOliveToolPack : uint8
{
	ReadPack,           // Safe reads, context gathering
	WritePackBasic,     // Tier 1 simple writes
	WritePackGraph,     // Tier 2 graph editing
	DangerPack          // Tier 3 destructive/refactor operations
};

/**
 * Tool Pack Manager
 *
 * DEPRECATED: Tool pack filtering removed in AI Freedom update. This class is
 * retained for one release cycle. All tool visibility is now handled by Focus
 * Profiles (FOliveFocusProfileManager) and the tool registry directly.
 *
 * Previously managed config-driven tool subsets that reduced per-call schema
 * token cost. Tool packs layered on top of Focus Profiles: profiles were the
 * upper bound (permission boundary), packs were the per-call subset.
 */
class OLIVEAIEDITOR_API FOliveToolPackManager
{
public:
	/** Get singleton instance */
	static FOliveToolPackManager& Get();

	/** Initialize by loading pack definitions from config */
	void Initialize();

	/**
	 * Get tools for a single pack, filtered by the active focus profile.
	 * @param Pack Which pack to retrieve
	 * @param FocusProfileName Active focus profile (upper bound filter)
	 * @return Tool definitions in this pack that are allowed by the profile
	 */
	TArray<FOliveToolDefinition> GetPackTools(
		EOliveToolPack Pack,
		const FString& FocusProfileName
	) const;

	/**
	 * Get tools for multiple packs combined, filtered by profile.
	 * Results are deduplicated.
	 * @param Packs Array of packs to combine
	 * @param FocusProfileName Active focus profile
	 * @return Union of tools from all packs, filtered by profile
	 */
	TArray<FOliveToolDefinition> GetCombinedPackTools(
		const TArray<EOliveToolPack>& Packs,
		const FString& FocusProfileName
	) const;

	/**
	 * Get tool names in a pack (before profile filtering).
	 */
	TArray<FString> GetPackToolNames(EOliveToolPack Pack) const;

	/**
	 * Check if a tool is in a specific pack.
	 */
	bool IsToolInPack(const FString& ToolName, EOliveToolPack Pack) const;

	/** Get the number of tools in a pack (before profile filtering) */
	int32 GetPackSize(EOliveToolPack Pack) const;

	/** Check if packs were loaded successfully */
	bool IsInitialized() const { return bInitialized; }

private:
	FOliveToolPackManager() = default;

	/** Load pack definitions from JSON config file */
	bool LoadPacksFromConfig();

	/** Register default fallback packs (if config file missing) */
	void RegisterDefaultPacks();

	/** Convert pack enum to config key string */
	static FString PackToConfigKey(EOliveToolPack Pack);

	/** Pack definitions: pack enum -> set of tool names */
	TMap<EOliveToolPack, TSet<FString>> PackDefinitions;

	/** Whether initialization completed */
	bool bInitialized = false;
};
