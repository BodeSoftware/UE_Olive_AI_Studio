// Copyright Bode Software. All Rights Reserved.

#include "Reader/OliveLevelReader.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/UnrealEditorSubsystem.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveLevelReader, Log, All);

FOliveLevelReader& FOliveLevelReader::Get()
{
    static FOliveLevelReader Instance;
    return Instance;
}

static UWorld* GetEditorWorld()
{
    UUnrealEditorSubsystem* EdSub = GEditor ? GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>() : nullptr;
    return EdSub ? EdSub->GetEditorWorld() : nullptr;
}

static FOliveActorSummary SummaryOf(AActor* Actor)
{
    FOliveActorSummary S;
    S.Name = Actor->GetActorNameOrLabel();
    S.ClassPath = Actor->GetClass()->GetPathName();
    S.Location = Actor->GetActorLocation();
    S.Rotation = Actor->GetActorRotation();
    S.Scale = Actor->GetActorScale3D();
    S.FolderPath = Actor->GetFolderPath().ToString();
    return S;
}

TArray<FOliveActorSummary> FOliveLevelReader::ListActors() const
{
    TArray<FOliveActorSummary> Out;
    UWorld* World = GetEditorWorld();
    if (!World) return Out;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        Out.Add(SummaryOf(*It));
    }
    return Out;
}

TArray<FOliveActorSummary> FOliveLevelReader::FindActors(const FString& NamePattern, const FString& ClassPath) const
{
    TArray<FOliveActorSummary> Out;
    UWorld* World = GetEditorWorld();
    if (!World) return Out;

    UClass* ClassFilter = nullptr;
    if (!ClassPath.IsEmpty())
    {
        ClassFilter = FindObject<UClass>(nullptr, *ClassPath);
    }

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (ClassFilter && !A->IsA(ClassFilter)) continue;
        if (!NamePattern.IsEmpty() && !A->GetActorNameOrLabel().Contains(NamePattern, ESearchCase::IgnoreCase)) continue;
        Out.Add(SummaryOf(A));
    }
    return Out;
}

AActor* FOliveLevelReader::FindActorByName(const FString& ActorName) const
{
    UWorld* World = GetEditorWorld();
    if (!World) return nullptr;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetActorNameOrLabel().Equals(ActorName, ESearchCase::IgnoreCase))
        {
            return *It;
        }
    }
    return nullptr;
}

TArray<FString> FOliveLevelReader::GetActorMaterials(const FString& ActorName) const
{
    TArray<FString> Out;
    AActor* Actor = FindActorByName(ActorName);
    if (!Actor) return Out;

    TArray<UMeshComponent*> MeshComps;
    Actor->GetComponents<UMeshComponent>(MeshComps);
    for (UMeshComponent* Comp : MeshComps)
    {
        const int32 Slots = Comp->GetNumMaterials();
        for (int32 i = 0; i < Slots; ++i)
        {
            UMaterialInterface* M = Comp->GetMaterial(i);
            Out.Add(M ? M->GetPathName() : TEXT("(none)"));
        }
    }
    return Out;
}
