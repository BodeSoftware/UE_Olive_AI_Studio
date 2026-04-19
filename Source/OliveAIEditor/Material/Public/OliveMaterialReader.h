// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FOliveMaterialSummary
{
    FString Path;
    FString Name;
    FString ParentPath;
    bool bIsInstance = false;
};

struct FOliveMaterialDetails
{
    FString Path;
    FString ParentPath;
    bool bIsInstance = false;
    TMap<FString, FLinearColor> VectorParameters;
    TMap<FString, float> ScalarParameters;
    TMap<FString, FString> TextureParameters;
};

class OLIVEAIEDITOR_API FOliveMaterialReader
{
public:
    static FOliveMaterialReader& Get();

    TArray<FOliveMaterialSummary> ListMaterials(const FString& SearchPath, bool bIncludeEngine) const;
    FOliveMaterialDetails ReadMaterial(const FString& MaterialPath) const;

private:
    FOliveMaterialReader() = default;
};
