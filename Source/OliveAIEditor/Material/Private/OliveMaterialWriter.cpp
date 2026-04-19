// Copyright Bode Software. All Rights Reserved.

#include "OliveMaterialWriter.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Components/MeshComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "ScopedTransaction.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "OliveMaterialWriter"

FOliveMaterialWriter& FOliveMaterialWriter::Get()
{
    static FOliveMaterialWriter I;
    return I;
}

static UBlueprint* LoadBP(const FString& Path)
{
    return LoadObject<UBlueprint>(nullptr, *Path);
}

FOliveToolResult FOliveMaterialWriter::ApplyToComponent(const FString& BlueprintPath, const FString& ComponentName, int32 Slot, const FString& MaterialPath)
{
    UBlueprint* BP = LoadBP(BlueprintPath);
    if (!BP)
        return FOliveToolResult::Error(TEXT("BP_NOT_FOUND"),
            FString::Printf(TEXT("Blueprint '%s' not found."), *BlueprintPath),
            TEXT("Use a full asset path like /Game/Path/BP_Name.BP_Name."));

    UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!Mat)
        return FOliveToolResult::Error(TEXT("MATERIAL_NOT_FOUND"),
            FString::Printf(TEXT("Material '%s' not found."), *MaterialPath), TEXT(""));

    if (!BP->SimpleConstructionScript)
        return FOliveToolResult::Error(TEXT("NO_SCS"),
            TEXT("Blueprint has no SimpleConstructionScript."), TEXT(""));

    USCS_Node* Target = nullptr;
    for (USCS_Node* N : BP->SimpleConstructionScript->GetAllNodes())
    {
        if (N && N->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
        {
            Target = N;
            break;
        }
    }
    if (!Target)
        return FOliveToolResult::Error(TEXT("COMPONENT_NOT_FOUND"),
            FString::Printf(TEXT("Component '%s' not found in Blueprint."), *ComponentName), TEXT(""));

    UMeshComponent* Template = Cast<UMeshComponent>(Target->ComponentTemplate);
    if (!Template)
        return FOliveToolResult::Error(TEXT("COMPONENT_NOT_MESH"),
            TEXT("Component is not a MeshComponent."), TEXT(""));

    FScopedTransaction Tx(LOCTEXT("OliveApplyMaterialBP", "Olive: Apply Material to BP Component"));
    Template->Modify();
    Template->SetMaterial(Slot, Mat);
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    return FOliveToolResult::Success();
}

FOliveToolResult FOliveMaterialWriter::SetParameterColor(const FString& MIPath, const FString& ParameterName, const FLinearColor& Color)
{
    UMaterialInstanceConstant* MI = LoadObject<UMaterialInstanceConstant>(nullptr, *MIPath);
    if (!MI)
        return FOliveToolResult::Error(TEXT("MI_NOT_FOUND"),
            FString::Printf(TEXT("Material instance '%s' not found."), *MIPath),
            TEXT("The target must be a UMaterialInstanceConstant, not a UMaterial."));

    FScopedTransaction Tx(LOCTEXT("OliveSetParamColor", "Olive: Set Material Instance Vector Parameter"));
    MI->Modify();
    MI->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(*ParameterName), Color);
    MI->PostEditChange();

    return FOliveToolResult::Success();
}

FOliveToolResult FOliveMaterialWriter::CreateInstance(const FString& NewInstancePath, const FString& ParentMaterialPath)
{
    UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentMaterialPath);
    if (!Parent)
        return FOliveToolResult::Error(TEXT("PARENT_NOT_FOUND"),
            FString::Printf(TEXT("Parent material '%s' not found."), *ParentMaterialPath), TEXT(""));

    // Derive package name and asset name.
    FString PackageName, AssetName;
    FString Input = NewInstancePath;
    // Strip any ".AssetName" suffix to get the clean package name
    int32 DotIdx = INDEX_NONE;
    if (Input.FindLastChar('.', DotIdx))
    {
        PackageName = Input.Left(DotIdx);
        AssetName = Input.Mid(DotIdx + 1);
    }
    else
    {
        PackageName = Input;
        AssetName = FPaths::GetBaseFilename(Input);
    }

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    IAssetTools& AssetTools = AssetToolsModule.Get();

    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = Parent;

    UObject* Created = AssetTools.CreateAsset(
        AssetName,
        FPackageName::GetLongPackagePath(PackageName),
        UMaterialInstanceConstant::StaticClass(),
        Factory);
    if (!Created)
        return FOliveToolResult::Error(TEXT("CREATE_FAILED"),
            TEXT("CreateAsset returned nullptr."),
            TEXT("Check that the target package path does not already contain the asset."));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("path"), Created->GetPathName());
    return FOliveToolResult::Success(Data);
}

#undef LOCTEXT_NAMESPACE
