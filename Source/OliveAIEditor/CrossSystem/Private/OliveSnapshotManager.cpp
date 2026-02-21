// Copyright Bode Software. All Rights Reserved.

#include "OliveSnapshotManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY(LogOliveSnapshot);

namespace OliveSnapshotInternal
{
	static FString SanitizePathSegment(const FString& In)
	{
		FString Out = In;
		for (int32 i = 0; i < Out.Len(); ++i)
		{
			const TCHAR Ch = Out[i];
			if (!(FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-')))
			{
				Out[i] = TEXT('_');
			}
		}
		return Out;
	}

	static bool CopyFileEnsureDir(const FString& Src, const FString& Dst)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Dst), true);
		return IFileManager::Get().Copy(*Dst, *Src) == COPY_OK;
	}
}

// ============================================================
// FOliveSnapshotInfo
// ============================================================

TSharedPtr<FJsonObject> FOliveSnapshotInfo::ToJson() const
{
	TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
	JsonObject->SetStringField(TEXT("id"), SnapshotId);
	JsonObject->SetStringField(TEXT("name"), Name);
	JsonObject->SetStringField(TEXT("description"), Description);
	JsonObject->SetStringField(TEXT("created_at"), CreatedAt.ToIso8601());

	TArray<TSharedPtr<FJsonValue>> AssetArray;
	for (const FString& Path : AssetPaths)
	{
		AssetArray.Add(MakeShared<FJsonValueString>(Path));
	}
	JsonObject->SetArrayField(TEXT("assets"), AssetArray);

	return JsonObject;
}

FOliveSnapshotInfo FOliveSnapshotInfo::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveSnapshotInfo Info;

	if (!JsonObject.IsValid())
	{
		return Info;
	}

	Info.SnapshotId = JsonObject->GetStringField(TEXT("id"));
	Info.Name = JsonObject->GetStringField(TEXT("name"));
	Info.Description = JsonObject->GetStringField(TEXT("description"));

	FString CreatedAtStr = JsonObject->GetStringField(TEXT("created_at"));
	FDateTime::ParseIso8601(*CreatedAtStr, Info.CreatedAt);

	const TArray<TSharedPtr<FJsonValue>>* AssetArray;
	if (JsonObject->TryGetArrayField(TEXT("assets"), AssetArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *AssetArray)
		{
			FString Path;
			if (Value->TryGetString(Path))
			{
				Info.AssetPaths.Add(Path);
			}
		}
	}

	return Info;
}

// ============================================================
// FOliveSnapshotDiffEntry
// ============================================================

TSharedPtr<FJsonObject> FOliveSnapshotDiffEntry::ToJson() const
{
	TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();
	JsonObject->SetStringField(TEXT("asset_path"), AssetPath);
	JsonObject->SetStringField(TEXT("change_type"), ChangeType);

	if (SnapshotState.IsValid())
	{
		JsonObject->SetObjectField(TEXT("snapshot_state"), SnapshotState);
	}

	if (CurrentState.IsValid())
	{
		JsonObject->SetObjectField(TEXT("current_state"), CurrentState);
	}

	TArray<TSharedPtr<FJsonValue>> FieldsArray;
	for (const FString& Field : ChangedFields)
	{
		FieldsArray.Add(MakeShared<FJsonValueString>(Field));
	}
	JsonObject->SetArrayField(TEXT("changed_fields"), FieldsArray);

	return JsonObject;
}

// ============================================================
// FOliveSnapshotManager - Singleton
// ============================================================

FOliveSnapshotManager& FOliveSnapshotManager::Get()
{
	static FOliveSnapshotManager Instance;
	return Instance;
}

FOliveSnapshotManager::FOliveSnapshotManager()
{
}

// ============================================================
// Path Helpers
// ============================================================

FString FOliveSnapshotManager::GetSnapshotDir() const
{
	return FPaths::ProjectSavedDir() / TEXT("OliveAI") / TEXT("Snapshots");
}

FString FOliveSnapshotManager::GetSnapshotFilePath(const FString& SnapshotId) const
{
	return GetSnapshotDir() / (SnapshotId + TEXT(".json"));
}

FString FOliveSnapshotManager::GetSnapshotIndexPath() const
{
	return GetSnapshotDir() / TEXT("index.json");
}

FString FOliveSnapshotManager::GetSnapshotAssetBackupDir(const FString& SnapshotId, const FString& AssetPath) const
{
	const FString Key = OliveSnapshotInternal::SanitizePathSegment(AssetPath);
	return GetSnapshotDir() / SnapshotId / TEXT("packages") / Key;
}

bool FOliveSnapshotManager::GetAssetPackageFiles(const FString& AssetPath, TArray<FString>& OutFiles) const
{
	OutFiles.Reset();

	FString PackageName = AssetPath;
	if (PackageName.Contains(TEXT(".")))
	{
		PackageName = FPackageName::ObjectPathToPackageName(PackageName);
	}

	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		return false;
	}

	const FString UAssetPath = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!FPaths::FileExists(UAssetPath))
	{
		return false;
	}

	OutFiles.Add(UAssetPath);

	const FString UExpPath = FPaths::ChangeExtension(UAssetPath, TEXT("uexp"));
	if (FPaths::FileExists(UExpPath))
	{
		OutFiles.Add(UExpPath);
	}

	const FString UBulkPath = FPaths::ChangeExtension(UAssetPath, TEXT("ubulk"));
	if (FPaths::FileExists(UBulkPath))
	{
		OutFiles.Add(UBulkPath);
	}

	return OutFiles.Num() > 0;
}

// ============================================================
// Index Persistence
// ============================================================

void FOliveSnapshotManager::SaveSnapshotIndex()
{
	const FString Dir = GetSnapshotDir();
	IFileManager::Get().MakeDirectory(*Dir, true);

	TArray<TSharedPtr<FJsonValue>> IndexArray;
	for (const FOliveSnapshotInfo& Info : SnapshotIndex)
	{
		IndexArray.Add(MakeShared<FJsonValueObject>(Info.ToJson()));
	}

	TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetArrayField(TEXT("snapshots"), IndexArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);

	const FString IndexPath = GetSnapshotIndexPath();
	if (!FFileHelper::SaveStringToFile(OutputString, *IndexPath))
	{
		UE_LOG(LogOliveSnapshot, Error, TEXT("Failed to save snapshot index to %s"), *IndexPath);
	}
}

void FOliveSnapshotManager::LoadSnapshotIndex()
{
	FScopeLock Lock(&SnapshotLock);

	if (bIndexLoaded)
	{
		return;
	}

	const FString IndexPath = GetSnapshotIndexPath();
	FString JsonString;

	if (!FFileHelper::LoadFileToString(JsonString, *IndexPath))
	{
		// No index file yet - that's fine, start with empty index
		bIndexLoaded = true;
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogOliveSnapshot, Warning, TEXT("Failed to parse snapshot index from %s"), *IndexPath);
		bIndexLoaded = true;
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* SnapshotsArray;
	if (RootObject->TryGetArrayField(TEXT("snapshots"), SnapshotsArray))
	{
		for (const TSharedPtr<FJsonValue>& Value : *SnapshotsArray)
		{
			const TSharedPtr<FJsonObject>* InfoObject;
			if (Value->TryGetObject(InfoObject))
			{
				SnapshotIndex.Add(FOliveSnapshotInfo::FromJson(*InfoObject));
			}
		}
	}

	bIndexLoaded = true;
	UE_LOG(LogOliveSnapshot, Log, TEXT("Loaded snapshot index with %d entries"), SnapshotIndex.Num());
}

// ============================================================
// Asset IR Reading
// ============================================================

FString FOliveSnapshotManager::GetReadToolForAsset(const FString& AssetPath) const
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	if (!AssetData.IsValid())
	{
		return FString();
	}

	const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();

	if (ClassName == TEXT("Blueprint") ||
		ClassName == TEXT("AnimBlueprint") ||
		ClassName == TEXT("WidgetBlueprint"))
	{
		return TEXT("blueprint.read");
	}
	else if (ClassName == TEXT("BehaviorTree"))
	{
		return TEXT("behaviortree.read");
	}
	else if (ClassName == TEXT("BlackboardData"))
	{
		return TEXT("blackboard.read");
	}
	else if (ClassName == TEXT("PCGGraph") || ClassName == TEXT("PCGGraphInstance"))
	{
		return TEXT("pcg.read");
	}

	return FString();
}

TSharedPtr<FJsonObject> FOliveSnapshotManager::ReadAssetIR(const FString& AssetPath)
{
	const FString ToolName = GetReadToolForAsset(AssetPath);
	if (ToolName.IsEmpty())
	{
		UE_LOG(LogOliveSnapshot, Warning, TEXT("No read tool available for asset: %s"), *AssetPath);
		return nullptr;
	}

	if (!FOliveToolRegistry::Get().HasTool(ToolName))
	{
		UE_LOG(LogOliveSnapshot, Warning, TEXT("Read tool '%s' is not registered for asset: %s"), *ToolName, *AssetPath);
		return nullptr;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path"), AssetPath);

	FOliveToolResult Result = FOliveToolRegistry::Get().ExecuteTool(ToolName, Params);
	if (!Result.bSuccess)
	{
		UE_LOG(LogOliveSnapshot, Warning, TEXT("Failed to read IR for asset '%s' via tool '%s'"), *AssetPath, *ToolName);
		return nullptr;
	}

	return Result.Data;
}

// ============================================================
// JSON Comparison
// ============================================================

TArray<FString> FOliveSnapshotManager::CompareJsonObjects(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B) const
{
	TArray<FString> DifferingFields;

	if (!A.IsValid() && !B.IsValid())
	{
		return DifferingFields;
	}

	if (!A.IsValid() || !B.IsValid())
	{
		// One is null - all fields differ
		const TSharedPtr<FJsonObject>& ValidObj = A.IsValid() ? A : B;
		TArray<FString> Keys;
		ValidObj->Values.GetKeys(Keys);
		return Keys;
	}

	// Collect all keys from both objects
	TSet<FString> AllKeys;
	{
		TArray<FString> KeysA;
		A->Values.GetKeys(KeysA);
		for (const FString& Key : KeysA)
		{
			AllKeys.Add(Key);
		}
	}
	{
		TArray<FString> KeysB;
		B->Values.GetKeys(KeysB);
		for (const FString& Key : KeysB)
		{
			AllKeys.Add(Key);
		}
	}

	for (const FString& Key : AllKeys)
	{
		const TSharedPtr<FJsonValue>* ValueA = A->Values.Find(Key);
		const TSharedPtr<FJsonValue>* ValueB = B->Values.Find(Key);

		if (!ValueA || !ValueB)
		{
			// Key exists in one but not the other
			DifferingFields.Add(Key);
			continue;
		}

		// Serialize both values to strings and compare
		FString StringA;
		FString StringB;

		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> WriterA = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&StringA);
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> WriterB = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&StringB);

		FJsonSerializer::Serialize(*ValueA, Key, WriterA);
		FJsonSerializer::Serialize(*ValueB, Key, WriterB);

		WriterA->Close();
		WriterB->Close();

		if (StringA != StringB)
		{
			DifferingFields.Add(Key);
		}
	}

	return DifferingFields;
}

// ============================================================
// Create Snapshot
// ============================================================

FOliveToolResult FOliveSnapshotManager::CreateSnapshot(const FString& InName, const TArray<FString>& AssetPaths, const FString& Description)
{
	if (AssetPaths.Num() == 0)
	{
		return FOliveToolResult::Error(
			TEXT("INVALID_PARAMS"),
			TEXT("At least one asset path is required to create a snapshot"),
			TEXT("Provide asset paths like [\"/Game/BP_Player\", \"/Game/BT_Enemy\"]")
		);
	}

	LoadSnapshotIndex();

	const FString SnapshotId = FGuid::NewGuid().ToString();
	const FDateTime Now = FDateTime::UtcNow();

	// Read IR for each asset
	TSharedPtr<FJsonObject> AssetsObject = MakeShared<FJsonObject>();
	TArray<FString> SuccessfulPaths;
	TArray<FString> FailedPaths;
	TArray<FString> BackupWarningPaths;

	for (const FString& AssetPath : AssetPaths)
	{
		TSharedPtr<FJsonObject> IR = ReadAssetIR(AssetPath);
		if (IR.IsValid())
		{
			AssetsObject->SetObjectField(AssetPath, IR);
			SuccessfulPaths.Add(AssetPath);

			TArray<FString> PackageFiles;
			if (GetAssetPackageFiles(AssetPath, PackageFiles))
			{
				const FString BackupDir = GetSnapshotAssetBackupDir(SnapshotId, AssetPath);
				for (const FString& FilePath : PackageFiles)
				{
					const FString Dst = BackupDir / FPaths::GetCleanFilename(FilePath);
					if (!OliveSnapshotInternal::CopyFileEnsureDir(FilePath, Dst))
					{
						BackupWarningPaths.Add(AssetPath);
						UE_LOG(LogOliveSnapshot, Warning, TEXT("Failed package backup copy for '%s' (%s)"), *AssetPath, *FilePath);
						break;
					}
				}
			}
			else
			{
				BackupWarningPaths.Add(AssetPath);
				UE_LOG(LogOliveSnapshot, Warning, TEXT("No package files found for snapshot backup: %s"), *AssetPath);
			}
		}
		else
		{
			FailedPaths.Add(AssetPath);
			UE_LOG(LogOliveSnapshot, Warning, TEXT("Skipping asset '%s' - could not read IR"), *AssetPath);
		}
	}

	if (SuccessfulPaths.Num() == 0)
	{
		return FOliveToolResult::Error(
			TEXT("SNAPSHOT_FAILED"),
			TEXT("Could not read IR for any of the specified assets"),
			TEXT("Ensure assets exist and have registered read tools (blueprint.read, behaviortree.read, etc.)")
		);
	}

	// Build snapshot info
	FOliveSnapshotInfo Info;
	Info.SnapshotId = SnapshotId;
	Info.Name = InName;
	Info.Description = Description;
	Info.CreatedAt = Now;
	Info.AssetPaths = SuccessfulPaths;

	// Build snapshot file JSON
	TSharedPtr<FJsonObject> SnapshotFile = MakeShared<FJsonObject>();
	SnapshotFile->SetObjectField(TEXT("info"), Info.ToJson());
	SnapshotFile->SetObjectField(TEXT("assets"), AssetsObject);

	// Write snapshot file to disk
	const FString Dir = GetSnapshotDir();
	IFileManager::Get().MakeDirectory(*Dir, true);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(SnapshotFile.ToSharedRef(), Writer);

	const FString FilePath = GetSnapshotFilePath(SnapshotId);
	if (!FFileHelper::SaveStringToFile(OutputString, *FilePath))
	{
		return FOliveToolResult::Error(
			TEXT("WRITE_FAILED"),
			FString::Printf(TEXT("Failed to write snapshot file to %s"), *FilePath),
			TEXT("Check disk permissions and available space")
		);
	}

	// Update index
	{
		FScopeLock Lock(&SnapshotLock);
		SnapshotIndex.Add(Info);
	}
	SaveSnapshotIndex();

	UE_LOG(LogOliveSnapshot, Log, TEXT("Created snapshot '%s' (ID: %s) with %d assets"), *InName, *SnapshotId, SuccessfulPaths.Num());

	// Build result
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetObjectField(TEXT("snapshot"), Info.ToJson());
	ResultData->SetNumberField(TEXT("asset_count"), SuccessfulPaths.Num());

	if (FailedPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FailedArray;
		for (const FString& Path : FailedPaths)
		{
			FailedArray.Add(MakeShared<FJsonValueString>(Path));
		}
		ResultData->SetArrayField(TEXT("failed_assets"), FailedArray);
	}
	if (BackupWarningPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> BackupWarnArray;
		for (const FString& Path : BackupWarningPaths)
		{
			BackupWarnArray.Add(MakeShared<FJsonValueString>(Path));
		}
		ResultData->SetArrayField(TEXT("backup_warnings"), BackupWarnArray);
	}

	return FOliveToolResult::Success(ResultData);
}

// ============================================================
// List Snapshots
// ============================================================

FOliveToolResult FOliveSnapshotManager::ListSnapshots(const FString& AssetFilter)
{
	LoadSnapshotIndex();

	FScopeLock Lock(&SnapshotLock);

	TArray<TSharedPtr<FJsonValue>> SnapshotsArray;

	for (const FOliveSnapshotInfo& Info : SnapshotIndex)
	{
		if (!AssetFilter.IsEmpty())
		{
			// Filter: only include snapshots that contain the specified asset
			bool bContainsAsset = false;
			for (const FString& Path : Info.AssetPaths)
			{
				if (Path.Contains(AssetFilter))
				{
					bContainsAsset = true;
					break;
				}
			}
			if (!bContainsAsset)
			{
				continue;
			}
		}

		SnapshotsArray.Add(MakeShared<FJsonValueObject>(Info.ToJson()));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetArrayField(TEXT("snapshots"), SnapshotsArray);
	ResultData->SetNumberField(TEXT("count"), SnapshotsArray.Num());

	return FOliveToolResult::Success(ResultData);
}

// ============================================================
// Diff Snapshot
// ============================================================

FOliveToolResult FOliveSnapshotManager::DiffSnapshot(const FString& SnapshotId, const TArray<FString>& PathFilter)
{
	LoadSnapshotIndex();

	// Load snapshot file
	const FString FilePath = GetSnapshotFilePath(SnapshotId);
	FString JsonString;

	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		return FOliveToolResult::Error(
			TEXT("SNAPSHOT_NOT_FOUND"),
			FString::Printf(TEXT("Snapshot file not found: %s"), *SnapshotId),
			TEXT("Use snapshot.list to see available snapshots")
		);
	}

	TSharedPtr<FJsonObject> SnapshotFile;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, SnapshotFile) || !SnapshotFile.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("PARSE_ERROR"),
			FString::Printf(TEXT("Failed to parse snapshot file: %s"), *SnapshotId),
			TEXT("The snapshot file may be corrupted. Try deleting it and creating a new snapshot.")
		);
	}

	const TSharedPtr<FJsonObject>* AssetsObject;
	if (!SnapshotFile->TryGetObjectField(TEXT("assets"), AssetsObject))
	{
		return FOliveToolResult::Error(
			TEXT("INVALID_SNAPSHOT"),
			TEXT("Snapshot file is missing 'assets' field"),
			TEXT("The snapshot file may be corrupted.")
		);
	}

	// Determine which asset paths to diff
	TArray<FString> AssetKeys;
	(*AssetsObject)->Values.GetKeys(AssetKeys);

	if (PathFilter.Num() > 0)
	{
		// Filter to only requested paths
		AssetKeys = AssetKeys.FilterByPredicate([&PathFilter](const FString& Key)
		{
			for (const FString& Filter : PathFilter)
			{
				if (Key.Contains(Filter))
				{
					return true;
				}
			}
			return false;
		});
	}

	// Build diff entries
	TArray<TSharedPtr<FJsonValue>> DiffArray;

	for (const FString& AssetPath : AssetKeys)
	{
		FOliveSnapshotDiffEntry Entry;
		Entry.AssetPath = AssetPath;

		const TSharedPtr<FJsonObject>* SnapshotIR;
		(*AssetsObject)->TryGetObjectField(AssetPath, SnapshotIR);
		Entry.SnapshotState = SnapshotIR ? *SnapshotIR : nullptr;

		// Read current IR
		TSharedPtr<FJsonObject> CurrentIR = ReadAssetIR(AssetPath);
		Entry.CurrentState = CurrentIR;

		if (!Entry.SnapshotState.IsValid() && CurrentIR.IsValid())
		{
			Entry.ChangeType = TEXT("added");
		}
		else if (Entry.SnapshotState.IsValid() && !CurrentIR.IsValid())
		{
			Entry.ChangeType = TEXT("deleted");
		}
		else if (Entry.SnapshotState.IsValid() && CurrentIR.IsValid())
		{
			Entry.ChangedFields = CompareJsonObjects(Entry.SnapshotState, CurrentIR);
			Entry.ChangeType = Entry.ChangedFields.Num() > 0 ? TEXT("modified") : TEXT("unchanged");
		}
		else
		{
			// Both null - skip
			continue;
		}

		DiffArray.Add(MakeShared<FJsonValueObject>(Entry.ToJson()));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("snapshot_id"), SnapshotId);
	ResultData->SetArrayField(TEXT("diffs"), DiffArray);
	ResultData->SetNumberField(TEXT("total_assets"), DiffArray.Num());

	// Count changes by type
	int32 ModifiedCount = 0;
	int32 DeletedCount = 0;
	int32 AddedCount = 0;
	int32 UnchangedCount = 0;

	for (const TSharedPtr<FJsonValue>& Val : DiffArray)
	{
		const TSharedPtr<FJsonObject>* Obj;
		if (Val->TryGetObject(Obj))
		{
			FString ChangeType = (*Obj)->GetStringField(TEXT("change_type"));
			if (ChangeType == TEXT("modified")) ModifiedCount++;
			else if (ChangeType == TEXT("deleted")) DeletedCount++;
			else if (ChangeType == TEXT("added")) AddedCount++;
			else if (ChangeType == TEXT("unchanged")) UnchangedCount++;
		}
	}

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("modified"), ModifiedCount);
	Summary->SetNumberField(TEXT("deleted"), DeletedCount);
	Summary->SetNumberField(TEXT("added"), AddedCount);
	Summary->SetNumberField(TEXT("unchanged"), UnchangedCount);
	ResultData->SetObjectField(TEXT("summary"), Summary);

	return FOliveToolResult::Success(ResultData);
}

// ============================================================
// Rollback Snapshot
// ============================================================

FOliveToolResult FOliveSnapshotManager::RollbackSnapshot(
	const FString& SnapshotId,
	const TArray<FString>& PathFilter,
	bool bPreviewOnly,
	const FString& ConfirmationToken)
{
	if (bPreviewOnly)
	{
		return CreateRollbackPreview(SnapshotId, PathFilter);
	}

	return ExecuteRollback(SnapshotId, PathFilter, ConfirmationToken);
}

FOliveToolResult FOliveSnapshotManager::CreateRollbackPreview(const FString& SnapshotId, const TArray<FString>& PathFilter)
{
	LoadSnapshotIndex();

	// Load snapshot file
	const FString FilePath = GetSnapshotFilePath(SnapshotId);
	FString JsonString;

	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		return FOliveToolResult::Error(
			TEXT("SNAPSHOT_NOT_FOUND"),
			FString::Printf(TEXT("Snapshot file not found: %s"), *SnapshotId),
			TEXT("Use snapshot.list to see available snapshots")
		);
	}

	TSharedPtr<FJsonObject> SnapshotFile;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, SnapshotFile) || !SnapshotFile.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("PARSE_ERROR"),
			FString::Printf(TEXT("Failed to parse snapshot file: %s"), *SnapshotId),
			TEXT("The snapshot file may be corrupted.")
		);
	}

	const TSharedPtr<FJsonObject>* AssetsObject;
	if (!SnapshotFile->TryGetObjectField(TEXT("assets"), AssetsObject))
	{
		return FOliveToolResult::Error(
			TEXT("INVALID_SNAPSHOT"),
			TEXT("Snapshot file is missing 'assets' field"),
			TEXT("The snapshot file may be corrupted.")
		);
	}

	// Determine which assets are affected
	TArray<FString> AssetKeys;
	(*AssetsObject)->Values.GetKeys(AssetKeys);

	if (PathFilter.Num() > 0)
	{
		AssetKeys = AssetKeys.FilterByPredicate([&PathFilter](const FString& Key)
		{
			for (const FString& Filter : PathFilter)
			{
				if (Key.Contains(Filter))
				{
					return true;
				}
			}
			return false;
		});
	}

	const FString Token = FGuid::NewGuid().ToString();
	{
		FScopeLock Lock(&SnapshotLock);
		FRollbackConfirmation Entry;
		Entry.SnapshotId = SnapshotId;
		Entry.PathFilter = PathFilter;
		Entry.ExpiresAtUtc = FDateTime::UtcNow() + FTimespan::FromMinutes(10);
		PendingRollbackConfirmations.Add(Token, Entry);
	}

	FOliveToolResult DiffResult = DiffSnapshot(SnapshotId, PathFilter);

	TArray<TSharedPtr<FJsonValue>> AffectedArray;
	for (const FString& AssetPath : AssetKeys)
	{
		AffectedArray.Add(MakeShared<FJsonValueString>(AssetPath));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("snapshot_id"), SnapshotId);
	ResultData->SetStringField(TEXT("mode"), TEXT("preview"));
	ResultData->SetStringField(TEXT("confirmation_token"), Token);
	ResultData->SetArrayField(TEXT("affected_assets"), AffectedArray);
	ResultData->SetNumberField(TEXT("asset_count"), AssetKeys.Num());
	ResultData->SetStringField(TEXT("note"), TEXT("Call project.rollback with preview_only=false and this confirmation_token to execute."));
	if (DiffResult.bSuccess && DiffResult.Data.IsValid())
	{
		ResultData->SetObjectField(TEXT("diff"), DiffResult.Data);
	}

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveSnapshotManager::ExecuteRollback(
	const FString& SnapshotId,
	const TArray<FString>& PathFilter,
	const FString& ConfirmationToken)
{
	if (ConfirmationToken.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("MISSING_CONFIRMATION_TOKEN"),
			TEXT("confirmation_token is required when preview_only is false"),
			TEXT("Run preview call first to get a token"));
	}

	FRollbackConfirmation Confirmation;
	{
		FScopeLock Lock(&SnapshotLock);
		FRollbackConfirmation* Entry = PendingRollbackConfirmations.Find(ConfirmationToken);
		if (!Entry)
		{
			return FOliveToolResult::Error(
				TEXT("INVALID_CONFIRMATION_TOKEN"),
				TEXT("Invalid or expired confirmation token"),
				TEXT("Run project.rollback preview again"));
		}
		if (Entry->ExpiresAtUtc < FDateTime::UtcNow())
		{
			PendingRollbackConfirmations.Remove(ConfirmationToken);
			return FOliveToolResult::Error(
				TEXT("EXPIRED_CONFIRMATION_TOKEN"),
				TEXT("Confirmation token has expired"),
				TEXT("Run project.rollback preview again"));
		}
		if (Entry->SnapshotId != SnapshotId)
		{
			return FOliveToolResult::Error(
				TEXT("TOKEN_SNAPSHOT_MISMATCH"),
				TEXT("confirmation_token does not match snapshot_id"),
				TEXT("Use the token returned for this snapshot preview"));
		}

		Confirmation = *Entry;
		PendingRollbackConfirmations.Remove(ConfirmationToken);
	}

	TArray<FString> EffectiveFilter = PathFilter.Num() > 0 ? PathFilter : Confirmation.PathFilter;

	// Load snapshot file and gather target asset paths
	const FString FilePath = GetSnapshotFilePath(SnapshotId);
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		return FOliveToolResult::Error(
			TEXT("SNAPSHOT_NOT_FOUND"),
			FString::Printf(TEXT("Snapshot file not found: %s"), *SnapshotId));
	}

	TSharedPtr<FJsonObject> SnapshotFile;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, SnapshotFile) || !SnapshotFile.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("PARSE_ERROR"),
			FString::Printf(TEXT("Failed to parse snapshot file: %s"), *SnapshotId));
	}

	const TSharedPtr<FJsonObject>* AssetsObject;
	if (!SnapshotFile->TryGetObjectField(TEXT("assets"), AssetsObject))
	{
		return FOliveToolResult::Error(
			TEXT("INVALID_SNAPSHOT"),
			TEXT("Snapshot file is missing 'assets' field"));
	}

	TArray<FString> AssetKeys;
	(*AssetsObject)->Values.GetKeys(AssetKeys);
	if (EffectiveFilter.Num() > 0)
	{
		AssetKeys = AssetKeys.FilterByPredicate([&EffectiveFilter](const FString& Key)
		{
			for (const FString& Filter : EffectiveFilter)
			{
				if (Key.Contains(Filter))
				{
					return true;
				}
			}
			return false;
		});
	}

	const FString TempRoot = GetSnapshotDir() / TEXT("_rollback_temp") / ConfirmationToken;
	IFileManager::Get().MakeDirectory(*TempRoot, true);

	TArray<FString> RestoredAssets;
	TArray<FString> FailedAssets;
	TArray<FString> CompensationSources;

	bool bRollbackFailed = false;

	for (const FString& AssetPath : AssetKeys)
	{
		TArray<FString> CurrentFiles;
		GetAssetPackageFiles(AssetPath, CurrentFiles);

		const FString SnapshotBackupDir = GetSnapshotAssetBackupDir(SnapshotId, AssetPath);
		TArray<FString> SnapshotBackupFiles;
		IFileManager::Get().FindFiles(SnapshotBackupFiles, *(SnapshotBackupDir / TEXT("*")), true, false);
		if (SnapshotBackupFiles.Num() == 0)
		{
			FailedAssets.Add(AssetPath);
			bRollbackFailed = true;
			continue;
		}

		// Backup current files for compensation
		for (const FString& CurrentFile : CurrentFiles)
		{
			const FString CompDst = TempRoot / OliveSnapshotInternal::SanitizePathSegment(AssetPath) / FPaths::GetCleanFilename(CurrentFile);
			if (OliveSnapshotInternal::CopyFileEnsureDir(CurrentFile, CompDst))
			{
				CompensationSources.Add(CompDst + TEXT("|") + CurrentFile);
			}
		}

		// Restore snapshot files to package location
		bool bAssetRestoreOk = true;
		FString PackageName = AssetPath;
		if (PackageName.Contains(TEXT(".")))
		{
			PackageName = FPackageName::ObjectPathToPackageName(PackageName);
		}
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			bAssetRestoreOk = false;
		}
		const FString BasePackage = bAssetRestoreOk
			? FPaths::ChangeExtension(FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), TEXT(""))
			: FString();

		if (!bAssetRestoreOk)
		{
			FailedAssets.Add(AssetPath);
			bRollbackFailed = true;
			continue;
		}

		for (const FString& BackupFilename : SnapshotBackupFiles)
		{
			const FString Src = SnapshotBackupDir / BackupFilename;
			const FString Ext = FPaths::GetExtension(BackupFilename, true);
			const FString Dst = BasePackage + Ext;

			if (!OliveSnapshotInternal::CopyFileEnsureDir(Src, Dst))
			{
				bAssetRestoreOk = false;
				break;
			}
		}

		if (!bAssetRestoreOk)
		{
			FailedAssets.Add(AssetPath);
			bRollbackFailed = true;
		}
		else
		{
			RestoredAssets.Add(AssetPath);
		}
	}

	bool bCompensationApplied = false;
	if (bRollbackFailed)
	{
		bCompensationApplied = true;
		for (const FString& Mapping : CompensationSources)
		{
			FString Src;
			FString Dst;
			if (Mapping.Split(TEXT("|"), &Src, &Dst))
			{
				OliveSnapshotInternal::CopyFileEnsureDir(Src, Dst);
			}
		}
	}

	// Cleanup temp compensation directory
	IFileManager::Get().DeleteDirectory(*TempRoot, false, true);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("snapshot_id"), SnapshotId);
	ResultData->SetStringField(TEXT("mode"), TEXT("executed"));
	ResultData->SetBoolField(TEXT("compensation_applied"), bCompensationApplied);

	TArray<TSharedPtr<FJsonValue>> AffectedArray;
	for (const FString& AssetPath : AssetKeys)
	{
		AffectedArray.Add(MakeShared<FJsonValueString>(AssetPath));
	}
	ResultData->SetArrayField(TEXT("affected_assets"), AffectedArray);

	TArray<TSharedPtr<FJsonValue>> RestoredArray;
	for (const FString& AssetPath : RestoredAssets)
	{
		RestoredArray.Add(MakeShared<FJsonValueString>(AssetPath));
	}
	ResultData->SetArrayField(TEXT("restored_assets"), RestoredArray);

	TArray<TSharedPtr<FJsonValue>> FailedArray;
	for (const FString& AssetPath : FailedAssets)
	{
		FailedArray.Add(MakeShared<FJsonValueString>(AssetPath));
	}
	ResultData->SetArrayField(TEXT("failed_assets"), FailedArray);

	if (bRollbackFailed)
	{
		ResultData->SetStringField(TEXT("status"), TEXT("failed"));
		FOliveToolResult Result;
		Result.bSuccess = false;
		Result.Data = ResultData;
		FOliveIRMessage Message;
		Message.Severity = EOliveIRSeverity::Error;
		Message.Code = TEXT("ROLLBACK_FAILED");
		Message.Message = TEXT("Rollback failed for one or more assets. Compensation attempted.");
		Message.Suggestion = TEXT("Review failed_assets and retry with a fresh snapshot");
		Result.Messages.Add(Message);
		return Result;
	}

	ResultData->SetStringField(TEXT("status"), TEXT("success"));
	return FOliveToolResult::Success(ResultData);
}

// ============================================================
// Delete Snapshot
// ============================================================

bool FOliveSnapshotManager::DeleteSnapshot(const FString& SnapshotId)
{
	LoadSnapshotIndex();

	// Remove from index
	{
		FScopeLock Lock(&SnapshotLock);

		int32 IndexToRemove = INDEX_NONE;
		for (int32 i = 0; i < SnapshotIndex.Num(); ++i)
		{
			if (SnapshotIndex[i].SnapshotId == SnapshotId)
			{
				IndexToRemove = i;
				break;
			}
		}

		if (IndexToRemove == INDEX_NONE)
		{
			UE_LOG(LogOliveSnapshot, Warning, TEXT("Snapshot '%s' not found in index"), *SnapshotId);
			return false;
		}

		SnapshotIndex.RemoveAt(IndexToRemove);
	}

	SaveSnapshotIndex();

	// Delete snapshot file
	const FString FilePath = GetSnapshotFilePath(SnapshotId);
	if (IFileManager::Get().FileExists(*FilePath))
	{
		if (!IFileManager::Get().Delete(*FilePath))
		{
			UE_LOG(LogOliveSnapshot, Error, TEXT("Failed to delete snapshot file: %s"), *FilePath);
			return false;
		}
	}
	const FString SnapshotPackageDir = GetSnapshotDir() / SnapshotId;
	if (IFileManager::Get().DirectoryExists(*SnapshotPackageDir))
	{
		IFileManager::Get().DeleteDirectory(*SnapshotPackageDir, false, true);
	}

	UE_LOG(LogOliveSnapshot, Log, TEXT("Deleted snapshot: %s"), *SnapshotId);
	return true;
}

// ============================================================
// Get Snapshot Info
// ============================================================

TOptional<FOliveSnapshotInfo> FOliveSnapshotManager::GetSnapshotInfo(const FString& SnapshotId) const
{
	// We need to load index if not already loaded - use const_cast since LoadSnapshotIndex
	// is logically const (lazy init of cache)
	const_cast<FOliveSnapshotManager*>(this)->LoadSnapshotIndex();

	FScopeLock Lock(&SnapshotLock);

	for (const FOliveSnapshotInfo& Info : SnapshotIndex)
	{
		if (Info.SnapshotId == SnapshotId)
		{
			return Info;
		}
	}

	return TOptional<FOliveSnapshotInfo>();
}
