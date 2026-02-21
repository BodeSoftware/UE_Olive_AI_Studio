// Copyright Bode Software. All Rights Reserved.

#include "OliveMultiAssetOperations.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Dom/JsonValue.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOliveMultiAsset);

// ============================================================================
// Singleton
// ============================================================================

FOliveMultiAssetOperations& FOliveMultiAssetOperations::Get()
{
	static FOliveMultiAssetOperations Instance;
	return Instance;
}

// ============================================================================
// BulkRead
// ============================================================================

FOliveToolResult FOliveMultiAssetOperations::BulkRead(const TArray<FString>& Paths, const FString& ReadMode)
{
	if (Paths.Num() == 0)
	{
		return FOliveToolResult::Error(TEXT("EMPTY_PATHS"), TEXT("No asset paths provided"), TEXT("Provide at least one asset path"));
	}
	if (Paths.Num() > 20)
	{
		return FOliveToolResult::Error(TEXT("TOO_MANY_ASSETS"),
			FString::Printf(TEXT("Cannot read more than 20 assets at once (got %d)"), Paths.Num()),
			TEXT("Split into multiple bulk_read calls"));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AssetsArray;
	int32 SuccessCount = 0;
	int32 FailCount = 0;

	for (const FString& AssetPath : Paths)
	{
		FString ReadTool = GetReadToolForAsset(AssetPath);

		TSharedPtr<FJsonObject> AssetResult = MakeShared<FJsonObject>();
		AssetResult->SetStringField(TEXT("path"), AssetPath);

		if (ReadTool.IsEmpty())
		{
			AssetResult->SetBoolField(TEXT("success"), false);
			AssetResult->SetStringField(TEXT("error"), TEXT("Unknown asset type or asset not found"));
			FailCount++;
			UE_LOG(LogOliveMultiAsset, Warning, TEXT("BulkRead: Could not determine read tool for '%s'"), *AssetPath);
		}
		else
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("path"), AssetPath);
			if (ReadTool == TEXT("blueprint.read"))
			{
				Params->SetStringField(TEXT("mode"), ReadMode);
			}

			FOliveToolResult ToolResult = FOliveToolRegistry::Get().ExecuteTool(ReadTool, Params);
			AssetResult->SetBoolField(TEXT("success"), ToolResult.bSuccess);
			AssetResult->SetStringField(TEXT("tool"), ReadTool);
			if (ToolResult.bSuccess && ToolResult.Data.IsValid())
			{
				AssetResult->SetObjectField(TEXT("data"), ToolResult.Data);
				SuccessCount++;
			}
			else
			{
				FString ErrorMsg = ToolResult.Messages.Num() > 0 ? ToolResult.Messages[0].Message : TEXT("Read failed");
				AssetResult->SetStringField(TEXT("error"), ErrorMsg);
				FailCount++;
				UE_LOG(LogOliveMultiAsset, Warning, TEXT("BulkRead: Failed to read '%s' via %s: %s"), *AssetPath, *ReadTool, *ErrorMsg);
			}
		}

		AssetsArray.Add(MakeShared<FJsonValueObject>(AssetResult));
	}

	ResultData->SetArrayField(TEXT("assets"), AssetsArray);
	ResultData->SetNumberField(TEXT("total"), Paths.Num());
	ResultData->SetNumberField(TEXT("success_count"), SuccessCount);
	ResultData->SetNumberField(TEXT("fail_count"), FailCount);

	if (FailCount == Paths.Num())
	{
		UE_LOG(LogOliveMultiAsset, Error, TEXT("BulkRead: All %d asset reads failed"), Paths.Num());
		return FOliveToolResult::Error(TEXT("ALL_FAILED"), TEXT("All asset reads failed"));
	}

	UE_LOG(LogOliveMultiAsset, Log, TEXT("BulkRead: %d/%d assets read successfully"), SuccessCount, Paths.Num());
	return FOliveToolResult::Success(ResultData);
}

// ============================================================================
// GetReadToolForAsset
// ============================================================================

FString FOliveMultiAssetOperations::GetReadToolForAsset(const FString& AssetPath) const
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Try to get asset data with the path as-is
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	if (!AssetData.IsValid())
	{
		// Try with object path format (append .AssetName if not present)
		FString BaseFilename = FPaths::GetBaseFilename(AssetPath);
		FString ObjectPath = AssetPath + TEXT(".") + BaseFilename;
		AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	}

	if (!AssetData.IsValid())
	{
		return FString();
	}

	FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();

	if (ClassName == TEXT("Blueprint") || ClassName == TEXT("WidgetBlueprint") || ClassName == TEXT("AnimBlueprint"))
	{
		return TEXT("blueprint.read");
	}
	if (ClassName == TEXT("BehaviorTree"))
	{
		return TEXT("behaviortree.read");
	}
	if (ClassName == TEXT("BlackboardData"))
	{
		return TEXT("blackboard.read");
	}
	if (ClassName.Contains(TEXT("PCG")))
	{
		return TEXT("pcg.read");
	}

	return FString();
}

// ============================================================================
// ImplementInterface
// ============================================================================

FOliveToolResult FOliveMultiAssetOperations::ImplementInterface(const TArray<FString>& Paths, const FString& InterfaceName)
{
	if (Paths.Num() == 0)
	{
		return FOliveToolResult::Error(TEXT("EMPTY_PATHS"), TEXT("No Blueprint paths provided"), TEXT("Provide at least one Blueprint path"));
	}
	if (InterfaceName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("EMPTY_INTERFACE"), TEXT("Interface name cannot be empty"), TEXT("Provide a valid interface name"));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	TArray<FString> SuccessfulPaths;
	int32 SuccessCount = 0;
	bool bOperationFailed = false;
	FString FailureReason;

	UE_LOG(LogOliveMultiAsset, Log, TEXT("ImplementInterface: Adding '%s' to %d Blueprints"), *InterfaceName, Paths.Num());

	for (const FString& Path : Paths)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), Path);
		Params->SetStringField(TEXT("interface"), InterfaceName);

		FOliveToolResult ToolResult = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.add_interface"), Params);

		TSharedPtr<FJsonObject> EntryResult = MakeShared<FJsonObject>();
		EntryResult->SetStringField(TEXT("path"), Path);
		EntryResult->SetBoolField(TEXT("success"), ToolResult.bSuccess);
		if (!ToolResult.bSuccess && ToolResult.Messages.Num() > 0)
		{
			EntryResult->SetStringField(TEXT("error"), ToolResult.Messages[0].Message);
			UE_LOG(LogOliveMultiAsset, Warning, TEXT("ImplementInterface: Failed for '%s': %s"), *Path, *ToolResult.Messages[0].Message);
			bOperationFailed = true;
			FailureReason = ToolResult.Messages[0].Message;
		}
		else if (!ToolResult.bSuccess)
		{
			bOperationFailed = true;
			FailureReason = TEXT("blueprint.add_interface failed");
		}
		else if (ToolResult.bSuccess)
		{
			SuccessCount++;
			SuccessfulPaths.Add(Path);
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(EntryResult));

		if (bOperationFailed)
		{
			break;
		}
	}

	bool bRolledBack = false;
	TArray<TSharedPtr<FJsonValue>> CompensationArray;
	if (bOperationFailed)
	{
		bRolledBack = true;
		for (int32 i = SuccessfulPaths.Num() - 1; i >= 0; --i)
		{
			const FString& Path = SuccessfulPaths[i];
			TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
			RemoveParams->SetStringField(TEXT("path"), Path);
			RemoveParams->SetStringField(TEXT("interface"), InterfaceName);
			const FOliveToolResult RemoveResult = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.remove_interface"), RemoveParams);

			TSharedPtr<FJsonObject> Compensation = MakeShared<FJsonObject>();
			Compensation->SetStringField(TEXT("path"), Path);
			Compensation->SetBoolField(TEXT("success"), RemoveResult.bSuccess);
			Compensation->SetStringField(TEXT("action"), TEXT("blueprint.remove_interface"));
			if (!RemoveResult.bSuccess && RemoveResult.Messages.Num() > 0)
			{
				Compensation->SetStringField(TEXT("error"), RemoveResult.Messages[0].Message);
			}
			CompensationArray.Add(MakeShared<FJsonValueObject>(Compensation));
		}
	}

	ResultData->SetArrayField(TEXT("results"), ResultsArray);
	ResultData->SetNumberField(TEXT("success_count"), SuccessCount);
	ResultData->SetNumberField(TEXT("total"), Paths.Num());
	ResultData->SetStringField(TEXT("interface"), InterfaceName);
	ResultData->SetBoolField(TEXT("rolled_back"), bRolledBack);
	ResultData->SetArrayField(TEXT("compensation"), CompensationArray);

	if (bOperationFailed)
	{
		ResultData->SetStringField(TEXT("status"), TEXT("failed"));
		ResultData->SetStringField(TEXT("failure_reason"), FailureReason);
		FOliveToolResult FailureResult;
		FailureResult.bSuccess = false;
		FailureResult.Data = ResultData;
		FOliveIRMessage Message;
		Message.Severity = EOliveIRSeverity::Error;
		Message.Code = TEXT("ATOMIC_OPERATION_FAILED");
		Message.Message = TEXT("project.implement_interface failed and rollback was attempted.");
		Message.Suggestion = TEXT("Review compensation results and rerun with smaller batch if needed.");
		FailureResult.Messages.Add(Message);
		return FailureResult;
	}
	ResultData->SetStringField(TEXT("status"), TEXT("success"));

	UE_LOG(LogOliveMultiAsset, Log, TEXT("ImplementInterface: %d/%d Blueprints updated successfully"), SuccessCount, Paths.Num());
	return FOliveToolResult::Success(ResultData);
}

// ============================================================================
// RefactorRename
// ============================================================================

FOliveToolResult FOliveMultiAssetOperations::RefactorRename(const FString& AssetPath, const FString& NewName, bool bUpdateReferences)
{
	if (AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("EMPTY_PATH"), TEXT("Asset path cannot be empty"), TEXT("Provide a valid asset path"));
	}
	if (NewName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("EMPTY_NAME"), TEXT("New name cannot be empty"), TEXT("Provide a valid new name"));
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	if (!AssetData.IsValid())
	{
		// Try with object path format
		FString BaseFilename = FPaths::GetBaseFilename(AssetPath);
		FString ObjectPath = AssetPath + TEXT(".") + BaseFilename;
		AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	}

	if (!AssetData.IsValid())
	{
		return FOliveToolResult::Error(TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath),
			TEXT("Check the asset path and ensure it exists in the project"));
	}

	// Build new path
	FString PackagePath = FPaths::GetPath(AssetPath);
	FString NewPath = PackagePath / NewName;

	UE_LOG(LogOliveMultiAsset, Log, TEXT("RefactorRename: Renaming '%s' to '%s' (update refs: %s)"),
		*AssetPath, *NewPath, bUpdateReferences ? TEXT("true") : TEXT("false"));

	// Use AssetTools for rename (handles references)
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

	TArray<FAssetRenameData> RenameData;
	FAssetRenameData RenameEntry;
	RenameEntry.Asset = AssetData.GetAsset();
	RenameEntry.NewPackagePath = PackagePath;
	RenameEntry.NewName = NewName;
	RenameData.Add(RenameEntry);

	bool bSuccess = AssetToolsModule.Get().RenameAssets(RenameData);

	if (bSuccess)
	{
		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		ResultData->SetStringField(TEXT("old_path"), AssetPath);
		ResultData->SetStringField(TEXT("new_path"), NewPath);
		ResultData->SetStringField(TEXT("new_name"), NewName);
		ResultData->SetBoolField(TEXT("references_updated"), bUpdateReferences);

		UE_LOG(LogOliveMultiAsset, Log, TEXT("RefactorRename: Successfully renamed '%s' to '%s'"), *AssetPath, *NewPath);
		return FOliveToolResult::Success(ResultData);
	}

	UE_LOG(LogOliveMultiAsset, Error, TEXT("RefactorRename: Failed to rename '%s' to '%s'"), *AssetPath, *NewName);
	return FOliveToolResult::Error(TEXT("RENAME_FAILED"),
		FString::Printf(TEXT("Failed to rename '%s' to '%s'"), *AssetPath, *NewName),
		TEXT("Check that the asset is not open in an editor or locked by another process"));
}

// ============================================================================
// CreateAICharacter
// ============================================================================

FOliveToolResult FOliveMultiAssetOperations::CreateAICharacter(
	const FString& Name,
	const FString& Path,
	const FString& ParentClass,
	const TArray<TPair<FString, FString>>& BlackboardKeys,
	const FString& BehaviorTreeRoot)
{
	if (Name.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("EMPTY_NAME"), TEXT("Character name cannot be empty"), TEXT("Provide a character name"));
	}
	if (Path.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("EMPTY_PATH"), TEXT("Path cannot be empty"), TEXT("Provide a content directory path"));
	}

	FString BBPath = Path / (TEXT("BB_") + Name);
	FString BTPath = Path / (TEXT("BT_") + Name);
	FString BPPath = Path / (TEXT("BP_") + Name);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> CreatedAssets;
	TArray<FString> CreatedPaths;
	TArray<TSharedPtr<FJsonValue>> CompensationArray;
	auto RollbackCreatedAssets = [this, &CreatedPaths, &CompensationArray]()
	{
		for (int32 Idx = CreatedPaths.Num() - 1; Idx >= 0; --Idx)
		{
			const FString& CreatedPath = CreatedPaths[Idx];
			const bool bDeleted = DeleteAssetByPath(CreatedPath);
			TSharedPtr<FJsonObject> Compensation = MakeShared<FJsonObject>();
			Compensation->SetStringField(TEXT("path"), CreatedPath);
			Compensation->SetStringField(TEXT("action"), TEXT("delete_asset"));
			Compensation->SetBoolField(TEXT("success"), bDeleted);
			CompensationArray.Add(MakeShared<FJsonValueObject>(Compensation));
		}
	};

	UE_LOG(LogOliveMultiAsset, Log, TEXT("CreateAICharacter: Creating AI character '%s' at '%s'"), *Name, *Path);

	// 1. Create Blackboard
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), BBPath);

		FOliveToolResult Result = FOliveToolRegistry::Get().ExecuteTool(TEXT("blackboard.create"), Params);

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("type"), TEXT("Blackboard"));
		Entry->SetStringField(TEXT("path"), BBPath);
		Entry->SetBoolField(TEXT("success"), Result.bSuccess);
		CreatedAssets.Add(MakeShared<FJsonValueObject>(Entry));

		if (!Result.bSuccess)
		{
			ResultData->SetArrayField(TEXT("created_assets"), CreatedAssets);
			FString ErrorMsg = Result.Messages.Num() > 0 ? Result.Messages[0].Message : TEXT("Unknown error");
			UE_LOG(LogOliveMultiAsset, Error, TEXT("CreateAICharacter: Failed to create Blackboard at '%s': %s"), *BBPath, *ErrorMsg);
			return FOliveToolResult::Error(TEXT("BB_CREATE_FAILED"),
				FString::Printf(TEXT("Failed to create Blackboard at %s"), *BBPath),
				TEXT("Check that the path is valid and writable"));
		}
		CreatedPaths.Add(BBPath);

		UE_LOG(LogOliveMultiAsset, Log, TEXT("CreateAICharacter: Created Blackboard at '%s'"), *BBPath);
	}

	// 2. Add Blackboard keys
	for (const auto& Key : BlackboardKeys)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), BBPath);
		Params->SetStringField(TEXT("name"), Key.Key);
		Params->SetStringField(TEXT("key_type"), Key.Value);

		FOliveToolResult Result = FOliveToolRegistry::Get().ExecuteTool(TEXT("blackboard.add_key"), Params);
		if (Result.bSuccess)
		{
			UE_LOG(LogOliveMultiAsset, Log, TEXT("CreateAICharacter: Added Blackboard key '%s' (%s)"), *Key.Key, *Key.Value);
		}
		else
		{
			UE_LOG(LogOliveMultiAsset, Error, TEXT("CreateAICharacter: Failed to add Blackboard key '%s' (%s)"), *Key.Key, *Key.Value);
			RollbackCreatedAssets();
			FOliveToolResult Failure = FOliveToolResult::Error(
				TEXT("ATOMIC_OPERATION_FAILED"),
				FString::Printf(TEXT("Failed to add Blackboard key '%s'. Rolled back created assets."), *Key.Key),
				TEXT("Verify Blackboard key types and retry."));
			if (Failure.Data.IsValid())
			{
				Failure.Data->SetArrayField(TEXT("compensation"), CompensationArray);
			}
			return Failure;
		}
	}

	// 3. Create Behavior Tree with Blackboard
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), BTPath);
		Params->SetStringField(TEXT("blackboard"), BBPath);

		FOliveToolResult Result = FOliveToolRegistry::Get().ExecuteTool(TEXT("behaviortree.create"), Params);

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("type"), TEXT("BehaviorTree"));
		Entry->SetStringField(TEXT("path"), BTPath);
		Entry->SetBoolField(TEXT("success"), Result.bSuccess);
		CreatedAssets.Add(MakeShared<FJsonValueObject>(Entry));

		if (Result.bSuccess)
		{
			UE_LOG(LogOliveMultiAsset, Log, TEXT("CreateAICharacter: Created BehaviorTree at '%s'"), *BTPath);
			CreatedPaths.Add(BTPath);
		}
		else
		{
			UE_LOG(LogOliveMultiAsset, Error, TEXT("CreateAICharacter: Failed to create BehaviorTree at '%s'"), *BTPath);
			RollbackCreatedAssets();
			FOliveToolResult Failure = FOliveToolResult::Error(
				TEXT("ATOMIC_OPERATION_FAILED"),
				TEXT("Failed to create BehaviorTree. Rolled back created assets."),
				TEXT("Ensure BehaviorTree tools are available and path is writable."));
			if (Failure.Data.IsValid())
			{
				Failure.Data->SetArrayField(TEXT("compensation"), CompensationArray);
			}
			return Failure;
		}
	}

	// 4. Create Blueprint
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), BPPath);
		Params->SetStringField(TEXT("parent_class"), ParentClass);

		FOliveToolResult Result = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.create"), Params);

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("type"), TEXT("Blueprint"));
		Entry->SetStringField(TEXT("path"), BPPath);
		Entry->SetBoolField(TEXT("success"), Result.bSuccess);
		CreatedAssets.Add(MakeShared<FJsonValueObject>(Entry));

		if (Result.bSuccess)
		{
			UE_LOG(LogOliveMultiAsset, Log, TEXT("CreateAICharacter: Created Blueprint at '%s'"), *BPPath);
			CreatedPaths.Add(BPPath);
		}
		else
		{
			UE_LOG(LogOliveMultiAsset, Error, TEXT("CreateAICharacter: Failed to create Blueprint at '%s'"), *BPPath);
			RollbackCreatedAssets();
			FOliveToolResult Failure = FOliveToolResult::Error(
				TEXT("ATOMIC_OPERATION_FAILED"),
				TEXT("Failed to create Blueprint. Rolled back created assets."),
				TEXT("Ensure Blueprint creation parameters are valid and retry."));
			if (Failure.Data.IsValid())
			{
				Failure.Data->SetArrayField(TEXT("compensation"), CompensationArray);
			}
			return Failure;
		}
	}

	ResultData->SetStringField(TEXT("name"), Name);
	ResultData->SetArrayField(TEXT("created_assets"), CreatedAssets);
	ResultData->SetStringField(TEXT("blueprint"), BPPath);
	ResultData->SetStringField(TEXT("behavior_tree"), BTPath);
	ResultData->SetStringField(TEXT("blackboard"), BBPath);
	ResultData->SetBoolField(TEXT("rolled_back"), false);
	ResultData->SetArrayField(TEXT("compensation"), CompensationArray);

	UE_LOG(LogOliveMultiAsset, Log, TEXT("CreateAICharacter: AI character '%s' creation complete"), *Name);
	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveMultiAssetOperations::MoveToCpp(
	const FString& AssetPath,
	const FString& ModuleName,
	const FString& TargetClassName,
	const FString& ParentClass,
	bool bCreateWrapperBlueprint,
	bool bCompileAfter)
{
	TSharedPtr<FJsonObject> ReadParams = MakeShared<FJsonObject>();
	ReadParams->SetStringField(TEXT("path"), AssetPath);
	ReadParams->SetStringField(TEXT("mode"), TEXT("full"));
	const FOliveToolResult ReadResult = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.read"), ReadParams);
	if (!ReadResult.bSuccess || !ReadResult.Data.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("BLUEPRINT_READ_FAILED"),
			FString::Printf(TEXT("Failed to read Blueprint for migration: %s"), *AssetPath),
			TEXT("Verify asset path and that blueprint.read is available."));
	}

	FString EffectiveParentClass = ParentClass;
	if (EffectiveParentClass.IsEmpty())
	{
		ReadResult.Data->TryGetStringField(TEXT("parent_class"), EffectiveParentClass);
	}
	if (EffectiveParentClass.IsEmpty())
	{
		EffectiveParentClass = TEXT("AActor");
	}

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("class_name"), TargetClassName);
	CreateParams->SetStringField(TEXT("parent_class"), EffectiveParentClass);
	CreateParams->SetStringField(TEXT("module_name"), ModuleName);
	const FOliveToolResult CreateResult = FOliveToolRegistry::Get().ExecuteTool(TEXT("cpp.create_class"), CreateParams);
	if (!CreateResult.bSuccess || !CreateResult.Data.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("CPP_CLASS_CREATE_FAILED"),
			FString::Printf(TEXT("Failed to create C++ class scaffold: %s"), *TargetClassName),
			TEXT("Verify module name and class naming conventions."));
	}

	FString HeaderPath;
	CreateResult.Data->TryGetStringField(TEXT("header_path"), HeaderPath);

	TArray<TSharedPtr<FJsonValue>> SuggestedProperties;
	TArray<TSharedPtr<FJsonValue>> SuggestedFunctions;
	TArray<TSharedPtr<FJsonValue>> WarningArray;
	TArray<TSharedPtr<FJsonValue>> ManualSteps;

	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	if (ReadResult.Data->TryGetArrayField(TEXT("variables"), Variables) && !HeaderPath.IsEmpty())
	{
		int32 AddedProperties = 0;
		for (const TSharedPtr<FJsonValue>& Value : *Variables)
		{
			const TSharedPtr<FJsonObject> VarObj = Value->AsObject();
			if (!VarObj.IsValid())
			{
				continue;
			}

			FString VarName;
			VarObj->TryGetStringField(TEXT("name"), VarName);
			if (VarName.IsEmpty())
			{
				continue;
			}

			FString CppType = TEXT("float");
			FString TypeName;
			if (VarObj->TryGetStringField(TEXT("type"), TypeName))
			{
				const FString Lower = TypeName.ToLower();
				if (Lower.Contains(TEXT("bool"))) CppType = TEXT("bool");
				else if (Lower.Contains(TEXT("int"))) CppType = TEXT("int32");
				else if (Lower.Contains(TEXT("float"))) CppType = TEXT("float");
				else if (Lower.Contains(TEXT("name"))) CppType = TEXT("FName");
				else if (Lower.Contains(TEXT("string"))) CppType = TEXT("FString");
				else if (Lower.Contains(TEXT("vector"))) CppType = TEXT("FVector");
				else if (Lower.Contains(TEXT("rotator"))) CppType = TEXT("FRotator");
				else CppType = TEXT("float");
			}

			TSharedPtr<FJsonObject> Suggested = MakeShared<FJsonObject>();
			Suggested->SetStringField(TEXT("name"), VarName);
			Suggested->SetStringField(TEXT("type"), CppType);
			SuggestedProperties.Add(MakeShared<FJsonValueObject>(Suggested));

			if (AddedProperties < 20)
			{
				TSharedPtr<FJsonObject> AddPropParams = MakeShared<FJsonObject>();
				AddPropParams->SetStringField(TEXT("file_path"), HeaderPath);
				AddPropParams->SetStringField(TEXT("property_name"), VarName);
				AddPropParams->SetStringField(TEXT("property_type"), CppType);
				TArray<TSharedPtr<FJsonValue>> Specifiers;
				Specifiers.Add(MakeShared<FJsonValueString>(TEXT("EditAnywhere")));
				Specifiers.Add(MakeShared<FJsonValueString>(TEXT("BlueprintReadWrite")));
				AddPropParams->SetArrayField(TEXT("specifiers"), Specifiers);
				AddPropParams->SetStringField(TEXT("category"), TEXT("Migrated"));
				const FOliveToolResult AddPropResult = FOliveToolRegistry::Get().ExecuteTool(TEXT("cpp.add_property"), AddPropParams);
				if (!AddPropResult.bSuccess)
				{
					TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
					WarnObj->SetStringField(TEXT("code"), TEXT("PROPERTY_SCAFFOLD_FAILED"));
					WarnObj->SetStringField(TEXT("message"), FString::Printf(TEXT("Could not scaffold property '%s'"), *VarName));
					WarningArray.Add(MakeShared<FJsonValueObject>(WarnObj));
				}
			}

			AddedProperties++;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	if (ReadResult.Data->TryGetArrayField(TEXT("functions"), Functions) && !HeaderPath.IsEmpty())
	{
		int32 AddedFunctions = 0;
		for (const TSharedPtr<FJsonValue>& Value : *Functions)
		{
			const TSharedPtr<FJsonObject> FuncObj = Value->AsObject();
			if (!FuncObj.IsValid())
			{
				continue;
			}

			FString FunctionName;
			FuncObj->TryGetStringField(TEXT("name"), FunctionName);
			if (FunctionName.IsEmpty())
			{
				continue;
			}

			TSharedPtr<FJsonObject> Suggested = MakeShared<FJsonObject>();
			Suggested->SetStringField(TEXT("name"), FunctionName);
			Suggested->SetStringField(TEXT("return_type"), TEXT("void"));
			SuggestedFunctions.Add(MakeShared<FJsonValueObject>(Suggested));

			if (AddedFunctions < 12)
			{
				TSharedPtr<FJsonObject> AddFuncParams = MakeShared<FJsonObject>();
				AddFuncParams->SetStringField(TEXT("file_path"), HeaderPath);
				AddFuncParams->SetStringField(TEXT("function_name"), FunctionName);
				AddFuncParams->SetStringField(TEXT("return_type"), TEXT("void"));
				TArray<TSharedPtr<FJsonValue>> Specifiers;
				Specifiers.Add(MakeShared<FJsonValueString>(TEXT("BlueprintCallable")));
				Specifiers.Add(MakeShared<FJsonValueString>(TEXT("Category=Migrated")));
				AddFuncParams->SetArrayField(TEXT("specifiers"), Specifiers);
				const FOliveToolResult AddFuncResult = FOliveToolRegistry::Get().ExecuteTool(TEXT("cpp.add_function"), AddFuncParams);
				if (!AddFuncResult.bSuccess)
				{
					TSharedPtr<FJsonObject> WarnObj = MakeShared<FJsonObject>();
					WarnObj->SetStringField(TEXT("code"), TEXT("FUNCTION_SCAFFOLD_FAILED"));
					WarnObj->SetStringField(TEXT("message"), FString::Printf(TEXT("Could not scaffold function '%s'"), *FunctionName));
					WarningArray.Add(MakeShared<FJsonValueObject>(WarnObj));
				}
			}
			AddedFunctions++;
		}
	}

	if (bCompileAfter)
	{
		FOliveToolRegistry::Get().ExecuteTool(TEXT("cpp.compile"), MakeShared<FJsonObject>());
	}

	ManualSteps.Add(MakeShared<FJsonValueString>(TEXT("Reparent the Blueprint to the new C++ class when compile succeeds.")));
	ManualSteps.Add(MakeShared<FJsonValueString>(TEXT("Move complex Blueprint graph logic into native C++ methods incrementally.")));
	ManualSteps.Add(MakeShared<FJsonValueString>(TEXT("Preserve designer-tuned visual scripting logic in wrapper Blueprint where needed.")));

	TArray<TSharedPtr<FJsonValue>> MigrationPlan;
	MigrationPlan.Add(MakeShared<FJsonValueString>(TEXT("Read Blueprint and gather variables/functions/components.")));
	MigrationPlan.Add(MakeShared<FJsonValueString>(TEXT("Create C++ class scaffold.")));
	MigrationPlan.Add(MakeShared<FJsonValueString>(TEXT("Scaffold straightforward UPROPERTY/UFUNCTION declarations.")));
	MigrationPlan.Add(MakeShared<FJsonValueString>(TEXT("Complete manual migration of complex graph/event logic.")));
	MigrationPlan.Add(MakeShared<FJsonValueString>(TEXT("Compile and reparent Blueprint wrapper to new class.")));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("asset_path"), AssetPath);
	ResultData->SetStringField(TEXT("module_name"), ModuleName);
	ResultData->SetStringField(TEXT("target_class_name"), TargetClassName);
	ResultData->SetStringField(TEXT("parent_class"), EffectiveParentClass);
	ResultData->SetBoolField(TEXT("create_wrapper_blueprint"), bCreateWrapperBlueprint);
	ResultData->SetObjectField(TEXT("created_cpp_files"), CreateResult.Data);
	ResultData->SetArrayField(TEXT("migration_plan"), MigrationPlan);
	ResultData->SetArrayField(TEXT("suggested_uproperties"), SuggestedProperties);
	ResultData->SetArrayField(TEXT("suggested_ufunctions"), SuggestedFunctions);
	ResultData->SetArrayField(TEXT("manual_steps"), ManualSteps);
	ResultData->SetArrayField(TEXT("warnings"), WarningArray);
	ResultData->SetStringField(TEXT("mode"), TEXT("plan_scaffold"));

	return FOliveToolResult::Success(ResultData);
}

bool FOliveMultiAssetOperations::DeleteAssetByPath(const FString& AssetPath)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	if (!AssetData.IsValid())
	{
		const FString BaseFilename = FPaths::GetBaseFilename(AssetPath);
		const FString ObjectPath = AssetPath + TEXT(".") + BaseFilename;
		AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	}

	if (!AssetData.IsValid())
	{
		return true;
	}

	UObject* AssetObj = AssetData.GetAsset();
	if (!AssetObj)
	{
		return false;
	}

	TArray<UObject*> ToDelete;
	ToDelete.Add(AssetObj);
	return ObjectTools::DeleteObjectsUnchecked(ToDelete) > 0;
}
