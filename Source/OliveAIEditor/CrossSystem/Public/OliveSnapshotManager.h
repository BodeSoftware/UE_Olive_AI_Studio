// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveSnapshot, Log, All);

/**
 * Snapshot metadata stored alongside IR data
 */
struct OLIVEAIEDITOR_API FOliveSnapshotInfo
{
	FString SnapshotId;         // GUID-based unique ID
	FString Name;               // User-given name
	FString Description;        // Optional description
	FDateTime CreatedAt;        // When snapshot was taken
	TArray<FString> AssetPaths; // Assets included in this snapshot

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveSnapshotInfo FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Diff entry showing what changed between snapshot and current state
 */
struct OLIVEAIEDITOR_API FOliveSnapshotDiffEntry
{
	FString AssetPath;
	FString ChangeType;                    // "modified", "deleted", "added" (added = in current but not snapshot)
	TSharedPtr<FJsonObject> SnapshotState; // IR at snapshot time (null if added)
	TSharedPtr<FJsonObject> CurrentState;  // Current IR (null if deleted from project)
	TArray<FString> ChangedFields;         // Top-level fields that differ

	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * FOliveSnapshotManager
 *
 * Manages IR snapshots of project assets for comparison and rollback.
 * Snapshots are stored as JSON files in {Project}/Saved/OliveAI/Snapshots/.
 */
class OLIVEAIEDITOR_API FOliveSnapshotManager
{
public:
	static FOliveSnapshotManager& Get();

	// Create a snapshot of the given assets
	// Reads current IR by dispatching to existing tool handlers (blueprint.read, behaviortree.read, etc.)
	FOliveToolResult CreateSnapshot(const FString& Name, const TArray<FString>& AssetPaths, const FString& Description = TEXT(""));

	// List all available snapshots, optionally filtered by asset path
	FOliveToolResult ListSnapshots(const FString& AssetFilter = TEXT(""));

	// Compare snapshot state against current state
	FOliveToolResult DiffSnapshot(const FString& SnapshotId, const TArray<FString>& PathFilter = TArray<FString>());

	// Rollback assets to snapshot state with preview/confirm workflow
	FOliveToolResult RollbackSnapshot(
		const FString& SnapshotId,
		const TArray<FString>& PathFilter = TArray<FString>(),
		bool bPreviewOnly = true,
		const FString& ConfirmationToken = TEXT(""));

	// Delete a snapshot
	bool DeleteSnapshot(const FString& SnapshotId);

	// Snapshot C++ source files (from backup paths) into a snapshot directory
	bool SnapshotCppFiles(const FString& SnapshotId, const TArray<FString>& SourceFilePaths);

	// Get snapshot info by ID
	TOptional<FOliveSnapshotInfo> GetSnapshotInfo(const FString& SnapshotId) const;

private:
	FOliveSnapshotManager();

	// Get snapshot directory path
	FString GetSnapshotDir() const;

	// Get path for a specific snapshot's data file
	FString GetSnapshotFilePath(const FString& SnapshotId) const;

	// Get path for snapshot index file
	FString GetSnapshotIndexPath() const;

	// Read current IR for an asset by dispatching to the appropriate read tool
	TSharedPtr<FJsonObject> ReadAssetIR(const FString& AssetPath);

	// Determine what type of asset this is and return the appropriate read tool name
	FString GetReadToolForAsset(const FString& AssetPath) const;

	// Save/load snapshot index
	void SaveSnapshotIndex();
	void LoadSnapshotIndex();

	// Compare two JSON objects and return list of differing top-level fields
	TArray<FString> CompareJsonObjects(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B) const;

	// Resolve object path to package files on disk (.uasset/.uexp/.ubulk)
	bool GetAssetPackageFiles(const FString& AssetPath, TArray<FString>& OutFiles) const;

	// Create rollback preview payload and confirmation token
	FOliveToolResult CreateRollbackPreview(const FString& SnapshotId, const TArray<FString>& PathFilter);

	// Execute rollback after confirmation token validation
	FOliveToolResult ExecuteRollback(const FString& SnapshotId, const TArray<FString>& PathFilter, const FString& ConfirmationToken);

	// Build file backup paths for a snapshot and asset path
	FString GetSnapshotAssetBackupDir(const FString& SnapshotId, const FString& AssetPath) const;

	struct FRollbackConfirmation
	{
		FString SnapshotId;
		TArray<FString> PathFilter;
		FDateTime ExpiresAtUtc;
	};

	// Cached snapshot index
	TArray<FOliveSnapshotInfo> SnapshotIndex;
	bool bIndexLoaded = false;
	TMap<FString, FRollbackConfirmation> PendingRollbackConfirmations;

	mutable FCriticalSection SnapshotLock;
};
