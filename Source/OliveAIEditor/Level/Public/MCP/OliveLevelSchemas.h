// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace OliveLevelSchemas
{
    TSharedPtr<FJsonObject> ListActors();
    TSharedPtr<FJsonObject> FindActors();
    TSharedPtr<FJsonObject> SpawnActor();
    TSharedPtr<FJsonObject> DeleteActor();
    TSharedPtr<FJsonObject> SetTransform();
    TSharedPtr<FJsonObject> SetPhysics();
    TSharedPtr<FJsonObject> ApplyMaterial();
    TSharedPtr<FJsonObject> GetActorMaterials();
}
