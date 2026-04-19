// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

class AActor;

struct FOliveActorSummary
{
    FString Name;
    FString ClassPath;
    FVector Location = FVector::ZeroVector;
    FRotator Rotation = FRotator::ZeroRotator;
    FVector Scale = FVector::OneVector;
    FString FolderPath;
};

class OLIVEAIEDITOR_API FOliveLevelReader
{
public:
    static FOliveLevelReader& Get();

    /** Returns all actors in the current editor level. */
    TArray<FOliveActorSummary> ListActors() const;

    /** Case-insensitive substring match on actor name; optional class filter (exact path). */
    TArray<FOliveActorSummary> FindActors(const FString& NamePattern, const FString& ClassPath) const;

    /** Returns material paths for all static/skeletal mesh components of the named actor. */
    TArray<FString> GetActorMaterials(const FString& ActorName) const;

    /** Helper: resolve an actor by name, nullptr if not found. */
    AActor* FindActorByName(const FString& ActorName) const;

private:
    FOliveLevelReader() = default;
    FOliveLevelReader(const FOliveLevelReader&) = delete;
};
