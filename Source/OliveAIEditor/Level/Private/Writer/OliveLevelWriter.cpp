// Copyright Bode Software. All Rights Reserved.

#include "Writer/OliveLevelWriter.h"
#include "Reader/OliveLevelReader.h"
#include "Editor.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/UnrealEditorSubsystem.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInterface.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "OliveLevelWriter"

FOliveLevelWriter& FOliveLevelWriter::Get()
{
    static FOliveLevelWriter Instance;
    return Instance;
}

static UWorld* EditorWorld()
{
    UUnrealEditorSubsystem* EdSub = GEditor ? GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>() : nullptr;
    return EdSub ? EdSub->GetEditorWorld() : nullptr;
}

static UClass* ResolveActorClass(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    if (UClass* C = FindObject<UClass>(nullptr, *Path)) return C;
    return LoadObject<UClass>(nullptr, *Path);
}

FOliveToolResult FOliveLevelWriter::SpawnActor(const FOliveSpawnActorArgs& Args)
{
    UWorld* World = EditorWorld();
    if (!World)
        return FOliveToolResult::Error(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available."), TEXT("Open a level first."));

    UClass* ActorClass = ResolveActorClass(Args.ClassPath);
    if (!ActorClass)
        return FOliveToolResult::Error(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Actor class '%s' not found."), *Args.ClassPath),
            TEXT("Provide a full class path like /Script/Engine.StaticMeshActor."));

    FScopedTransaction Tx(LOCTEXT("OliveSpawnActor", "Olive: Spawn Actor"));

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    FTransform Xform(Args.Rotation, Args.Location, Args.Scale);
    AActor* A = World->SpawnActor(ActorClass, &Xform, Params);
    if (!A)
        return FOliveToolResult::Error(TEXT("SPAWN_FAILED"), TEXT("SpawnActor returned nullptr."), TEXT("Check the editor log."));

    if (!Args.Label.IsEmpty())
    {
        A->SetActorLabel(Args.Label);
    }

    if (!Args.MeshPath.IsEmpty())
    {
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Args.MeshPath);
        UStaticMeshComponent* SMC = A->FindComponentByClass<UStaticMeshComponent>();
        if (Mesh && SMC)
        {
            SMC->Modify();
            SMC->SetStaticMesh(Mesh);
        }
    }

    if (!Args.MaterialPath.IsEmpty())
    {
        UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *Args.MaterialPath);
        UMeshComponent* MC = A->FindComponentByClass<UMeshComponent>();
        if (Mat && MC)
        {
            MC->Modify();
            MC->SetMaterial(0, Mat);
        }
    }

    if (UPrimitiveComponent* Prim = A->FindComponentByClass<UPrimitiveComponent>())
    {
        Prim->Modify();
        Prim->SetSimulatePhysics(Args.bSimulatePhysics);
        Prim->SetEnableGravity(Args.bEnableGravity);
        if (Args.bSimulatePhysics)
        {
            Prim->SetMassOverrideInKg(NAME_None, Args.Mass, true);
        }
    }

    FOliveToolResult R = FOliveToolResult::Success();
    R.Data = MakeShared<FJsonObject>();
    R.Data->SetStringField(TEXT("actor_name"), A->GetActorNameOrLabel());
    R.Data->SetStringField(TEXT("class_path"), ActorClass->GetPathName());
    return R;
}

FOliveToolResult FOliveLevelWriter::DeleteActor(const FString& ActorName)
{
    AActor* A = FOliveLevelReader::Get().FindActorByName(ActorName);
    if (!A)
        return FOliveToolResult::Error(TEXT("ACTOR_NOT_FOUND"),
            FString::Printf(TEXT("Actor '%s' not found."), *ActorName),
            TEXT("Call level.list_actors or level.find_actors first."));

    FScopedTransaction Tx(LOCTEXT("OliveDeleteActor", "Olive: Delete Actor"));
    A->Modify();
    A->Destroy();
    return FOliveToolResult::Success();
}

FOliveToolResult FOliveLevelWriter::SetTransform(const FString& ActorName, const FVector& Location, const FRotator& Rotation, const FVector& Scale)
{
    AActor* A = FOliveLevelReader::Get().FindActorByName(ActorName);
    if (!A)
        return FOliveToolResult::Error(TEXT("ACTOR_NOT_FOUND"),
            FString::Printf(TEXT("Actor '%s' not found."), *ActorName), TEXT(""));

    FScopedTransaction Tx(LOCTEXT("OliveSetTransform", "Olive: Set Transform"));
    A->Modify();
    A->SetActorLocation(Location);
    A->SetActorRotation(Rotation);
    A->SetActorScale3D(Scale);
    return FOliveToolResult::Success();
}

static UPrimitiveComponent* FindPrim(AActor* A, const FString& ComponentName)
{
    if (!A) return nullptr;
    if (ComponentName.IsEmpty()) return A->FindComponentByClass<UPrimitiveComponent>();
    TArray<UPrimitiveComponent*> All;
    A->GetComponents<UPrimitiveComponent>(All);
    for (UPrimitiveComponent* C : All)
    {
        if (C->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)) return C;
    }
    return nullptr;
}

FOliveToolResult FOliveLevelWriter::SetPhysics(const FString& ActorName, const FString& ComponentName, bool bSimulatePhysics, bool bEnableGravity, float Mass)
{
    AActor* A = FOliveLevelReader::Get().FindActorByName(ActorName);
    UPrimitiveComponent* Prim = FindPrim(A, ComponentName);
    if (!Prim)
        return FOliveToolResult::Error(TEXT("COMPONENT_NOT_FOUND"),
            FString::Printf(TEXT("PrimitiveComponent '%s' not found on actor '%s'."), *ComponentName, *ActorName), TEXT(""));

    FScopedTransaction Tx(LOCTEXT("OliveSetPhysics", "Olive: Set Physics"));
    Prim->Modify();
    Prim->SetSimulatePhysics(bSimulatePhysics);
    Prim->SetEnableGravity(bEnableGravity);
    if (bSimulatePhysics && Mass > 0.0f)
    {
        Prim->SetMassOverrideInKg(NAME_None, Mass, true);
    }
    return FOliveToolResult::Success();
}

FOliveToolResult FOliveLevelWriter::ApplyMaterial(const FString& ActorName, const FString& ComponentName, int32 Slot, const FString& MaterialPath)
{
    AActor* A = FOliveLevelReader::Get().FindActorByName(ActorName);
    if (!A)
        return FOliveToolResult::Error(TEXT("ACTOR_NOT_FOUND"),
            FString::Printf(TEXT("Actor '%s' not found."), *ActorName), TEXT(""));

    UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!Mat)
        return FOliveToolResult::Error(TEXT("MATERIAL_NOT_FOUND"),
            FString::Printf(TEXT("Material '%s' not found."), *MaterialPath),
            TEXT("Provide a full package path like /Game/.../M_Name.M_Name."));

    TArray<UMeshComponent*> Meshes;
    A->GetComponents<UMeshComponent>(Meshes);
    UMeshComponent* Target = nullptr;
    if (ComponentName.IsEmpty() && Meshes.Num() > 0)
    {
        Target = Meshes[0];
    }
    else
    {
        for (UMeshComponent* C : Meshes)
        {
            if (C->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)) { Target = C; break; }
        }
    }

    if (!Target)
        return FOliveToolResult::Error(TEXT("COMPONENT_NOT_FOUND"),
            TEXT("No MeshComponent matched."), TEXT("Call level.get_actor_materials to list components."));

    FScopedTransaction Tx(LOCTEXT("OliveApplyMaterial", "Olive: Apply Material"));
    Target->Modify();
    Target->SetMaterial(Slot, Mat);
    return FOliveToolResult::Success();
}

#undef LOCTEXT_NAMESPACE
