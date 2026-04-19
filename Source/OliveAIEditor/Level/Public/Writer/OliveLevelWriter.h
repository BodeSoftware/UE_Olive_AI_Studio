// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

class AActor;

struct FOliveSpawnActorArgs
{
    FString ClassPath;
    FString MeshPath;
    FString MaterialPath;
    FString Label;
    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    FVector Scale = FVector::OneVector;
    bool bSimulatePhysics = false;
    bool bEnableGravity = true;
    float Mass = 100.0f;
};

class OLIVEAIEDITOR_API FOliveLevelWriter
{
public:
    static FOliveLevelWriter& Get();

    FOliveToolResult SpawnActor(const FOliveSpawnActorArgs& Args);
    FOliveToolResult DeleteActor(const FString& ActorName);
    FOliveToolResult SetTransform(const FString& ActorName, const FVector& Location, const FRotator& Rotation, const FVector& Scale);
    FOliveToolResult SetPhysics(const FString& ActorName, const FString& ComponentName, bool bSimulatePhysics, bool bEnableGravity, float Mass);
    FOliveToolResult ApplyMaterial(const FString& ActorName, const FString& ComponentName, int32 Slot, const FString& MaterialPath);

private:
    FOliveLevelWriter() = default;
};
