// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OliveFocusProfileManager.generated.h"

/**
 * Focus Profile Definition
 *
 * Defines a tool filtering profile that controls which tools
 * are available for a particular workflow.
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveFocusProfile
{
	GENERATED_BODY()

	/** Internal profile name */
	UPROPERTY()
	FString Name;

	/** Localized display name */
	UPROPERTY()
	FText DisplayName;

	/** Description tooltip */
	UPROPERTY()
	FText Description;

	/** Tool categories to include (empty = all) */
	UPROPERTY()
	TArray<FString> ToolCategories;

	/** Specific tools to exclude */
	UPROPERTY()
	TArray<FString> ExcludedTools;

	/** Additional text to append to system prompt */
	UPROPERTY()
	FString SystemPromptAddition;

	/** Icon name for UI */
	UPROPERTY()
	FString IconName;

	/** Sort order in dropdown */
	UPROPERTY()
	int32 SortOrder = 0;

	/** Is this a built-in profile */
	bool bIsBuiltIn = true;
};

/**
 * Focus Profile Manager
 *
 * Manages focus profiles that filter available tools based on
 * the current workflow. Supports built-in and custom profiles.
 */
class OLIVEAIEDITOR_API FOliveFocusProfileManager
{
public:
	/** Get singleton instance */
	static FOliveFocusProfileManager& Get();

	/** Initialize with default profiles */
	void Initialize();

	// ==========================================
	// Profile Access
	// ==========================================

	/** Get all available profiles */
	TArray<FOliveFocusProfile> GetAllProfiles() const;

	/** Get profile names for UI */
	TArray<FString> GetProfileNames() const;

	/** Get a specific profile by name */
	TOptional<FOliveFocusProfile> GetProfile(const FString& Name) const;

	/** Check if a profile exists */
	bool HasProfile(const FString& Name) const;

	/** Get default profile */
	const FOliveFocusProfile& GetDefaultProfile() const;

	// ==========================================
	// Tool Filtering
	// ==========================================

	/**
	 * Get tool categories for a profile
	 * @param ProfileName Profile to query
	 * @return Tool categories (empty = all categories)
	 */
	TArray<FString> GetToolCategoriesForProfile(const FString& ProfileName) const;

	/**
	 * Get excluded tools for a profile
	 */
	TArray<FString> GetExcludedToolsForProfile(const FString& ProfileName) const;

	/**
	 * Check if a tool is allowed for a profile
	 * @param ProfileName Profile to check
	 * @param ToolName Tool name
	 * @param ToolCategory Tool category
	 * @return true if allowed
	 */
	bool IsToolAllowedForProfile(
		const FString& ProfileName,
		const FString& ToolName,
		const FString& ToolCategory
	) const;

	/**
	 * Get system prompt addition for profile
	 */
	FString GetSystemPromptAddition(const FString& ProfileName) const;

	// ==========================================
	// Custom Profiles
	// ==========================================

	/** Add a custom profile */
	void AddCustomProfile(const FOliveFocusProfile& Profile);

	/** Remove a custom profile (cannot remove built-in) */
	bool RemoveCustomProfile(const FString& Name);

	/** Save custom profiles to config */
	void SaveCustomProfiles();

	/** Load custom profiles from config */
	void LoadCustomProfiles();

private:
	FOliveFocusProfileManager() = default;

	/** Register default built-in profiles */
	void RegisterDefaultProfiles();

	/** Profiles storage */
	TMap<FString, FOliveFocusProfile> Profiles;

	/** Names of custom (non-built-in) profiles */
	TArray<FString> CustomProfileNames;
};
