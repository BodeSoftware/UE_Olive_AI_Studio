// Copyright Bode Software. All Rights Reserved.

#include "OliveMaterialReader.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture.h"

FOliveMaterialReader& FOliveMaterialReader::Get()
{
    static FOliveMaterialReader I;
    return I;
}

TArray<FOliveMaterialSummary> FOliveMaterialReader::ListMaterials(const FString& SearchPath, bool bIncludeEngine) const
{
    TArray<FOliveMaterialSummary> Out;
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    TArray<FAssetData> Assets;
    FARFilter Filter;
    Filter.bRecursivePaths = true;
    if (!SearchPath.IsEmpty())
    {
        Filter.PackagePaths.Add(*SearchPath);
    }
    else
    {
        Filter.PackagePaths.Add(TEXT("/Game"));
        if (bIncludeEngine) Filter.PackagePaths.Add(TEXT("/Engine"));
    }
    Filter.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;

    AR.GetAssets(Filter, Assets);

    for (const FAssetData& A : Assets)
    {
        FOliveMaterialSummary S;
        S.Path = A.GetObjectPathString();
        S.Name = A.AssetName.ToString();
        S.bIsInstance = A.AssetClassPath == UMaterialInstanceConstant::StaticClass()->GetClassPathName();
        if (S.bIsInstance)
        {
            if (UMaterialInstance* MI = Cast<UMaterialInstance>(A.GetAsset()))
            {
                S.ParentPath = MI->Parent ? MI->Parent->GetPathName() : FString();
            }
        }
        Out.Add(S);
    }
    return Out;
}

FOliveMaterialDetails FOliveMaterialReader::ReadMaterial(const FString& MaterialPath) const
{
    FOliveMaterialDetails D;
    UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!M) return D;
    D.Path = M->GetPathName();

    if (UMaterialInstance* MI = Cast<UMaterialInstance>(M))
    {
        D.bIsInstance = true;
        D.ParentPath = MI->Parent ? MI->Parent->GetPathName() : FString();

        for (const FVectorParameterValue& V : MI->VectorParameterValues)
        {
            D.VectorParameters.Add(V.ParameterInfo.Name.ToString(), V.ParameterValue);
        }
        for (const FScalarParameterValue& S : MI->ScalarParameterValues)
        {
            D.ScalarParameters.Add(S.ParameterInfo.Name.ToString(), S.ParameterValue);
        }
        for (const FTextureParameterValue& T : MI->TextureParameterValues)
        {
            D.TextureParameters.Add(T.ParameterInfo.Name.ToString(),
                T.ParameterValue ? T.ParameterValue->GetPathName() : FString());
        }
    }

    return D;
}
