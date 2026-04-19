// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

class UBlueprint;

class OLIVEAIEDITOR_API FOliveMaterialWriter
{
public:
    static FOliveMaterialWriter& Get();

    FOliveToolResult ApplyToComponent(const FString& BlueprintPath, const FString& ComponentName, int32 Slot, const FString& MaterialPath);
    FOliveToolResult SetParameterColor(const FString& MaterialInstancePath, const FString& ParameterName, const FLinearColor& Color);
    FOliveToolResult CreateInstance(const FString& NewInstancePath, const FString& ParentMaterialPath);

private:
    FOliveMaterialWriter() = default;
};
