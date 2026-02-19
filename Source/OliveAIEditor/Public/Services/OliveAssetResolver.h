// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OliveAssetResolver.generated.h"

class IAssetEditorInstance;

/**
 * Result of resolving an asset
 */
UENUM()
enum class EOliveAssetResolveResult : uint8
{
	Success,
	NotFound,
	WrongType,
	Redirected,
	LoadFailed,
	CurrentlyEdited,
	PathInvalid
};

/**
 * Information about a resolved asset
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveAssetResolveInfo
{
	GENERATED_BODY()

	/** Result of the resolution attempt */
	UPROPERTY()
	EOliveAssetResolveResult Result = EOliveAssetResolveResult::NotFound;

	/** The resolved asset path */
	UPROPERTY()
	FString ResolvedPath;

	/** The original path that was requested */
	UPROPERTY()
	FString OriginalPath;

	/** If a redirector was followed, the original redirector path */
	UPROPERTY()
	FString RedirectedFrom;

	/** Whether the asset is currently being edited in the editor */
	UPROPERTY()
	bool bIsBeingEdited = false;

	/** Whether the asset has unsaved changes */
	UPROPERTY()
	bool bIsDirty = false;

	/** Error message if resolution failed */
	UPROPERTY()
	FString ErrorMessage;

	/** The resolved asset (may be null) */
	UPROPERTY()
	TObjectPtr<UObject> Asset = nullptr;

	/** Check if resolution was successful */
	bool IsSuccess() const { return Result == EOliveAssetResolveResult::Success || Result == EOliveAssetResolveResult::Redirected; }
};

/**
 * Asset Resolver
 *
 * Resolves assets by path/name/class with comprehensive error handling.
 * Handles redirectors, missing assets, and editor state checking.
 */
class OLIVEAIEDITOR_API FOliveAssetResolver
{
public:
	/** Get singleton instance */
	static FOliveAssetResolver& Get();

	/**
	 * Resolve an asset by exact path
	 * @param AssetPath Full asset path (e.g., /Game/Blueprints/BP_Player)
	 * @param ExpectedClass Optional class filter
	 * @return Resolution information
	 */
	FOliveAssetResolveInfo ResolveByPath(const FString& AssetPath, UClass* ExpectedClass = nullptr);

	/**
	 * Resolve assets by name (may return multiple matches)
	 * @param AssetName Asset name without path
	 * @param ClassFilter Optional class filter
	 * @return Array of matching assets
	 */
	TArray<FOliveAssetResolveInfo> ResolveByName(const FString& AssetName, UClass* ClassFilter = nullptr);

	/**
	 * Check if an asset is currently being edited
	 * @param AssetPath Asset to check
	 * @return True if asset is open in an editor
	 */
	bool IsAssetBeingEdited(const FString& AssetPath) const;

	/**
	 * Get the editor instance for an asset if it's open
	 * @param AssetPath Asset to check
	 * @return Editor instance or nullptr
	 */
	IAssetEditorInstance* GetAssetEditor(const FString& AssetPath) const;

	/**
	 * Follow redirectors to the final asset
	 * @param AssetPath Starting path
	 * @return Final resolved path
	 */
	FString FollowRedirectors(const FString& AssetPath) const;

	/**
	 * Check if an asset exists (without loading it)
	 * @param AssetPath Asset to check
	 * @return True if asset exists
	 */
	bool DoesAssetExist(const FString& AssetPath) const;

	/**
	 * Get the class of an asset (without fully loading it)
	 * @param AssetPath Asset to check
	 * @return Asset class or nullptr
	 */
	UClass* GetAssetClass(const FString& AssetPath) const;

	/**
	 * Normalize an asset path (resolve relative paths, remove double slashes, etc.)
	 * @param AssetPath Path to normalize
	 * @return Normalized path
	 */
	FString NormalizePath(const FString& AssetPath) const;

private:
	FOliveAssetResolver() = default;

	/** Get the asset editor subsystem */
	class UAssetEditorSubsystem* GetAssetEditorSubsystem() const;

	/** Maximum redirector chain length */
	static constexpr int32 MaxRedirectorDepth = 10;
};
