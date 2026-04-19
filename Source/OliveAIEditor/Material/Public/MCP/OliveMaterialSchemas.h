// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace OliveMaterialSchemas
{
    TSharedPtr<FJsonObject> List();
    TSharedPtr<FJsonObject> Read();
    TSharedPtr<FJsonObject> ApplyToComponent();
    TSharedPtr<FJsonObject> SetParameterColor();
    TSharedPtr<FJsonObject> CreateInstance();
}
