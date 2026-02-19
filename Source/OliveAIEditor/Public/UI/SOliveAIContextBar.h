// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SWrapBox;

/**
 * Context Asset Removed Delegate
 */
DECLARE_DELEGATE_OneParam(FOnOliveContextAssetRemoved, const FString& /* AssetPath */);

/**
 * Context Bar Widget
 *
 * Displays assets currently in context for the conversation.
 * Shows auto-detected context (currently open asset) and
 * manually added @mentioned assets.
 */
class OLIVEAIEDITOR_API SOliveAIContextBar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOliveAIContextBar) {}
		SLATE_EVENT(FOnOliveContextAssetRemoved, OnAssetRemoved)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// ==========================================
	// Context Management
	// ==========================================

	/**
	 * Add an asset to context
	 * @param AssetPath Full asset path
	 * @param DisplayName Friendly display name
	 * @param bIsAutoContext Whether this was auto-detected
	 */
	void AddContextAsset(const FString& AssetPath, const FString& DisplayName, bool bIsAutoContext = false);

	/**
	 * Remove an asset from context
	 * @param AssetPath Asset path to remove
	 */
	void RemoveContextAsset(const FString& AssetPath);

	/**
	 * Set the auto-detected context asset
	 * @param AssetPath Asset path
	 * @param DisplayName Display name
	 */
	void SetAutoContext(const FString& AssetPath, const FString& DisplayName);

	/**
	 * Clear the auto-detected context
	 */
	void ClearAutoContext();

	/**
	 * Clear all manually added context
	 */
	void ClearManualContext();

	/**
	 * Clear all context
	 */
	void ClearAllContext();

	/**
	 * Get all context asset paths
	 */
	TArray<FString> GetContextAssetPaths() const;

	/**
	 * Check if an asset is in context
	 */
	bool HasContextAsset(const FString& AssetPath) const;

private:
	/** Build a tag widget for an asset */
	TSharedRef<SWidget> BuildAssetTag(const FString& AssetPath, const FString& DisplayName, bool bIsAutoContext);

	/** Handle remove button clicked */
	FReply OnRemoveClicked(FString AssetPath);

	/** Handle asset tag clicked */
	FReply OnAssetTagClicked(FString AssetPath);

	/** Refresh the display */
	void RefreshDisplay();

	// ==========================================
	// State
	// ==========================================

	/** Container for tags */
	TSharedPtr<SWrapBox> TagContainer;

	/** Manual context assets (path -> display name) */
	TMap<FString, FString> ManualContextAssets;

	/** Auto context asset path */
	FString AutoContextPath;

	/** Auto context display name */
	FString AutoContextDisplayName;

	/** Map of path to widget for quick lookup */
	TMap<FString, TSharedPtr<SWidget>> AssetTagWidgets;

	/** Delegate for when an asset is removed */
	FOnOliveContextAssetRemoved OnAssetRemoved;
};
