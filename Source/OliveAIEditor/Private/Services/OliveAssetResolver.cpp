// Copyright Bode Software. All Rights Reserved.

#include "Services/OliveAssetResolver.h"
#include "OliveAIEditorModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "UObject/ObjectRedirector.h"

FOliveAssetResolver& FOliveAssetResolver::Get()
{
	static FOliveAssetResolver Instance;
	return Instance;
}

FOliveAssetResolveInfo FOliveAssetResolver::ResolveByPath(const FString& AssetPath, UClass* ExpectedClass)
{
	FOliveAssetResolveInfo Info;
	Info.OriginalPath = AssetPath;

	// Validate path format
	if (AssetPath.IsEmpty())
	{
		Info.Result = EOliveAssetResolveResult::PathInvalid;
		Info.ErrorMessage = TEXT("Asset path is empty");
		return Info;
	}

	// Normalize the path
	FString NormalizedPath = NormalizePath(AssetPath);

	// Check if asset exists
	if (!DoesAssetExist(NormalizedPath))
	{
		Info.Result = EOliveAssetResolveResult::NotFound;
		Info.ErrorMessage = FString::Printf(TEXT("Asset not found: %s"), *NormalizedPath);
		return Info;
	}

	// Follow redirectors
	FString FinalPath = FollowRedirectors(NormalizedPath);
	if (FinalPath != NormalizedPath)
	{
		Info.RedirectedFrom = NormalizedPath;
		Info.Result = EOliveAssetResolveResult::Redirected;
	}

	Info.ResolvedPath = FinalPath;

	// Try to load the asset
	UObject* LoadedAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *FinalPath);
	if (!LoadedAsset)
	{
		Info.Result = EOliveAssetResolveResult::LoadFailed;
		Info.ErrorMessage = FString::Printf(TEXT("Failed to load asset: %s"), *FinalPath);
		return Info;
	}

	// Check type if expected class specified
	if (ExpectedClass && !LoadedAsset->IsA(ExpectedClass))
	{
		Info.Result = EOliveAssetResolveResult::WrongType;
		Info.ErrorMessage = FString::Printf(TEXT("Asset is %s, expected %s"),
			*LoadedAsset->GetClass()->GetName(), *ExpectedClass->GetName());
		return Info;
	}

	// Success - fill in remaining info
	if (Info.Result != EOliveAssetResolveResult::Redirected)
	{
		Info.Result = EOliveAssetResolveResult::Success;
	}
	Info.Asset = LoadedAsset;
	Info.bIsBeingEdited = IsAssetBeingEdited(FinalPath);

	// Check if dirty
	if (UPackage* Package = LoadedAsset->GetOutermost())
	{
		Info.bIsDirty = Package->IsDirty();
	}

	return Info;
}

TArray<FOliveAssetResolveInfo> FOliveAssetResolver::ResolveByName(const FString& AssetName, UClass* ClassFilter)
{
	TArray<FOliveAssetResolveInfo> Results;

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Search for assets with this name
	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAssetsByPackageName(*AssetName, AssetDataList);

	// If no exact match, try a broader search
	if (AssetDataList.Num() == 0)
	{
		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.bRecursiveClasses = true;

		if (ClassFilter)
		{
			Filter.ClassPaths.Add(ClassFilter->GetClassPathName());
		}

		AssetRegistry.GetAssets(Filter, AssetDataList);

		// Filter by name
		AssetDataList.RemoveAll([&AssetName](const FAssetData& Data)
		{
			return !Data.AssetName.ToString().Contains(AssetName);
		});
	}

	// Convert to resolve info
	for (const FAssetData& Data : AssetDataList)
	{
		FOliveAssetResolveInfo Info = ResolveByPath(Data.GetObjectPathString(), ClassFilter);
		if (Info.IsSuccess())
		{
			Results.Add(MoveTemp(Info));
		}
	}

	return Results;
}

bool FOliveAssetResolver::IsAssetBeingEdited(const FString& AssetPath) const
{
	UAssetEditorSubsystem* AssetEditorSubsystem = GetAssetEditorSubsystem();
	if (!AssetEditorSubsystem)
	{
		return false;
	}

	UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Asset)
	{
		return false;
	}

	return AssetEditorSubsystem->FindEditorForAsset(Asset, false) != nullptr;
}

IAssetEditorInstance* FOliveAssetResolver::GetAssetEditor(const FString& AssetPath) const
{
	UAssetEditorSubsystem* AssetEditorSubsystem = GetAssetEditorSubsystem();
	if (!AssetEditorSubsystem)
	{
		return nullptr;
	}

	UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Asset)
	{
		return nullptr;
	}

	return AssetEditorSubsystem->FindEditorForAsset(Asset, false);
}

FString FOliveAssetResolver::FollowRedirectors(const FString& AssetPath) const
{
	FString CurrentPath = AssetPath;

	for (int32 i = 0; i < MaxRedirectorDepth; ++i)
	{
		// Try to load as redirector
		UObjectRedirector* Redirector = LoadObject<UObjectRedirector>(nullptr, *CurrentPath);
		if (!Redirector)
		{
			break;
		}

		// Get destination
		UObject* Destination = Redirector->DestinationObject;
		if (!Destination)
		{
			break;
		}

		CurrentPath = Destination->GetPathName();
	}

	return CurrentPath;
}

bool FOliveAssetResolver::DoesAssetExist(const FString& AssetPath) const
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Convert to package path
	FString PackagePath = AssetPath;
	if (PackagePath.Contains(TEXT(".")))
	{
		PackagePath = FPackageName::ObjectPathToPackageName(AssetPath);
	}

	// Use GetAssetByObjectPath for the full path
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	return AssetData.IsValid();
}

UClass* FOliveAssetResolver::GetAssetClass(const FString& AssetPath) const
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	if (!AssetData.IsValid())
	{
		return nullptr;
	}

	return AssetData.GetClass();
}

FString FOliveAssetResolver::NormalizePath(const FString& AssetPath) const
{
	FString Result = AssetPath;

	// Remove leading/trailing whitespace
	Result.TrimStartAndEndInline();

	// Replace backslashes with forward slashes
	Result.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Remove double slashes
	while (Result.Contains(TEXT("//")))
	{
		Result.ReplaceInline(TEXT("//"), TEXT("/"));
	}

	// Ensure starts with /
	if (!Result.StartsWith(TEXT("/")))
	{
		Result = TEXT("/") + Result;
	}

	return Result;
}

UAssetEditorSubsystem* FOliveAssetResolver::GetAssetEditorSubsystem() const
{
	if (GEditor)
	{
		return GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	}
	return nullptr;
}
