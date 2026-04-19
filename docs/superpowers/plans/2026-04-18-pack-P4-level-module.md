# Pack P4 — `level.*` Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new `Level/` submodule with 8 MCP tools for actor/level operations. All writes go through the existing `FOliveWritePipeline` with transactions + snapshots — this is the key differentiator from the reference plugin.

**Architecture:** Mirror the existing `Blueprint/`, `BehaviorTree/`, `PCG/`, `Niagara/`, `Cpp/` submodule pattern. `FOliveLevelReader` (singleton) for queries, `FOliveLevelWriter` (singleton) for mutations, `FOliveLevelToolHandlers` for tool registration, `OliveLevelSchemas` for JSON schemas. Writes use `UEditorActorSubsystem` for spawn/destroy and `FScopedTransaction` via the pipeline.

**Tech Stack:** UE 5.5 C++, `UEditorActorSubsystem`, `UUnrealEditorSubsystem`, `FScopedTransaction`, existing `FOliveWritePipeline`.

**Related spec:** `docs/superpowers/specs/2026-04-18-plugin-makeover-design.md` §3.10, §6.

**Independence:** Fully isolated — zero dependencies on P1/P2/P3/P5/P6/P7. Can run in parallel with all others.

---

## Tools to implement

| Tool | Type | Purpose |
|---|---|---|
| `level.list_actors` | read | Enumerate actors in current level |
| `level.find_actors` | read | Filter by name pattern or class |
| `level.spawn_actor` | write | Spawn through pipeline (snapshot + transaction) |
| `level.delete_actor` | write | Destroy through pipeline |
| `level.set_transform` | write | Location / rotation / scale |
| `level.set_physics` | write | Per-component `bSimulatePhysics`, `bEnableGravity`, Mass |
| `level.apply_material` | write | Apply material to actor's mesh component |
| `level.get_actor_materials` | read | Query actor's material slots |

---

## File Structure

**Create:**

```
Source/OliveAIEditor/Level/
├── Public/
│   ├── Reader/OliveLevelReader.h
│   ├── Writer/OliveLevelWriter.h
│   └── MCP/
│       ├── OliveLevelToolHandlers.h
│       └── OliveLevelSchemas.h
└── Private/
    ├── Reader/OliveLevelReader.cpp
    ├── Writer/OliveLevelWriter.cpp
    └── MCP/
        ├── OliveLevelToolHandlers.cpp
        └── OliveLevelSchemas.cpp
```

**Modify:**

- `Source/OliveAIEditor/OliveAIEditor.Build.cs` — add Level to include path list and module deps (`UnrealEd`, `EditorSubsystem`).
- `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp` — register `FOliveLevelToolHandlers` in `OnPostEngineInit()` after step 10.

**Create tests:**

```
Source/OliveAIEditor/Private/Tests/Level/
├── OliveLevelReaderTests.cpp
├── OliveLevelWriterTests.cpp
└── OliveLevelToolsTests.cpp
```

---

## Tasks

### Task 1: Scaffold directory structure and Build.cs

**Files:**
- Create: all directories under `Source/OliveAIEditor/Level/`.
- Modify: `Source/OliveAIEditor/OliveAIEditor.Build.cs`.

- [ ] **Step 1: Create directories**

```bash
mkdir -p "Source/OliveAIEditor/Level/Public/Reader" \
         "Source/OliveAIEditor/Level/Public/Writer" \
         "Source/OliveAIEditor/Level/Public/MCP" \
         "Source/OliveAIEditor/Level/Private/Reader" \
         "Source/OliveAIEditor/Level/Private/Writer" \
         "Source/OliveAIEditor/Level/Private/MCP" \
         "Source/OliveAIEditor/Private/Tests/Level"
```

- [ ] **Step 2: Update Build.cs**

Open `Source/OliveAIEditor/OliveAIEditor.Build.cs`. Find the recursive include paths list (for `Blueprint`, `BehaviorTree`, etc.). Add `Level` entries.

Example pattern (match existing style):

```csharp
PublicIncludePaths.AddRange(new string[] {
    // ...existing entries...
    Path.Combine(ModuleDirectory, "Level/Public"),
    Path.Combine(ModuleDirectory, "Level/Public/Reader"),
    Path.Combine(ModuleDirectory, "Level/Public/Writer"),
    Path.Combine(ModuleDirectory, "Level/Public/MCP"),
});

PrivateIncludePaths.AddRange(new string[] {
    // ...existing entries...
    Path.Combine(ModuleDirectory, "Level/Private"),
    Path.Combine(ModuleDirectory, "Level/Private/Reader"),
    Path.Combine(ModuleDirectory, "Level/Private/Writer"),
    Path.Combine(ModuleDirectory, "Level/Private/MCP"),
});
```

Confirm `UnrealEd` and `EditorSubsystem` are in `PublicDependencyModuleNames` or `PrivateDependencyModuleNames`. If not, add to `PrivateDependencyModuleNames`.

- [ ] **Step 3: Build (expect no errors — nothing references new paths yet)**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Level/ Source/OliveAIEditor/Private/Tests/Level/ Source/OliveAIEditor/OliveAIEditor.Build.cs
git commit -m "P4: scaffold Level/ submodule directories and Build.cs"
```

---

### Task 2: Implement `FOliveLevelReader` — list_actors + find_actors + get_actor_materials

**Files:**
- Create: `Source/OliveAIEditor/Level/Public/Reader/OliveLevelReader.h`
- Create: `Source/OliveAIEditor/Level/Private/Reader/OliveLevelReader.cpp`

- [ ] **Step 1: Write the header**

```cpp
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
```

- [ ] **Step 2: Write the cpp**

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "Reader/OliveLevelReader.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/UnrealEditorSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
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
```

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Level/Public/Reader/OliveLevelReader.h Source/OliveAIEditor/Level/Private/Reader/OliveLevelReader.cpp
git commit -m "P4: implement FOliveLevelReader (list/find/materials)"
```

---

### Task 3: Reader unit tests

**Files:**
- Create: `Source/OliveAIEditor/Private/Tests/Level/OliveLevelReaderTests.cpp`

- [ ] **Step 1: Write tests**

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Reader/OliveLevelReader.h"
#include "Editor.h"
#include "Subsystems/UnrealEditorSubsystem.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelReaderListActorsTest,
    "OliveAI.Level.Reader.ListActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelReaderListActorsTest::RunTest(const FString& Parameters)
{
    UUnrealEditorSubsystem* EdSub = GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>();
    UWorld* World = EdSub ? EdSub->GetEditorWorld() : nullptr;
    if (!TestNotNull(TEXT("Editor world should exist"), World)) return false;

    AStaticMeshActor* Placed = World->SpawnActor<AStaticMeshActor>();
    Placed->SetActorLabel(TEXT("OliveTest_ListActor"));

    TArray<FOliveActorSummary> All = FOliveLevelReader::Get().ListActors();
    const bool bFound = All.ContainsByPredicate([](const FOliveActorSummary& S) {
        return S.Name == TEXT("OliveTest_ListActor");
    });
    TestTrue(TEXT("Spawned actor appears in ListActors"), bFound);

    Placed->Destroy();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelReaderFindActorsTest,
    "OliveAI.Level.Reader.FindActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelReaderFindActorsTest::RunTest(const FString& Parameters)
{
    UUnrealEditorSubsystem* EdSub = GEditor->GetEditorSubsystem<UUnrealEditorSubsystem>();
    UWorld* World = EdSub->GetEditorWorld();
    AStaticMeshActor* A = World->SpawnActor<AStaticMeshActor>();
    A->SetActorLabel(TEXT("OliveTest_FindMe_SMA"));

    TArray<FOliveActorSummary> Hits = FOliveLevelReader::Get().FindActors(TEXT("FindMe"), TEXT(""));
    TestEqual(TEXT("FindActors matches one"), Hits.Num(), 1);
    TestEqual(TEXT("Name"), Hits[0].Name, FString(TEXT("OliveTest_FindMe_SMA")));

    A->Destroy();
    return true;
}
```

- [ ] **Step 2: Build and run tests**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Session Frontend > Automation > `OliveAI.Level.Reader.*`. Both tests pass.

- [ ] **Step 3: Commit**

```bash
git add Source/OliveAIEditor/Private/Tests/Level/OliveLevelReaderTests.cpp
git commit -m "P4: LevelReader tests"
```

---

### Task 4: Implement `FOliveLevelWriter` — spawn / delete / set_transform / set_physics / apply_material

**Files:**
- Create: `Source/OliveAIEditor/Level/Public/Writer/OliveLevelWriter.h`
- Create: `Source/OliveAIEditor/Level/Private/Writer/OliveLevelWriter.cpp`

- [ ] **Step 1: Write the header**

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

class AActor;

struct FOliveSpawnActorArgs
{
    FString ClassPath;            // required, e.g., "/Script/Engine.StaticMeshActor"
    FString MeshPath;             // optional, assigned to the first mesh component
    FString MaterialPath;         // optional
    FString Label;                // optional; auto-generated if empty
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
```

- [ ] **Step 2: Write the cpp**

Full implementation. Each method wraps work in `FScopedTransaction` and calls `Modify()` on affected objects so Ctrl+Z works.

```cpp
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
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInterface.h"
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
        return FOliveToolResult::ExecutionError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available."), TEXT("Open a level first."));

    UClass* ActorClass = ResolveActorClass(Args.ClassPath);
    if (!ActorClass)
        return FOliveToolResult::ValidationError(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Actor class '%s' not found."), *Args.ClassPath),
            TEXT("Provide a full class path like /Script/Engine.StaticMeshActor."));

    FScopedTransaction Tx(LOCTEXT("OliveSpawnActor", "Olive: Spawn Actor"));

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    FTransform Xform(Args.Rotation, Args.Location, Args.Scale);
    AActor* A = World->SpawnActor(ActorClass, &Xform, Params);
    if (!A)
        return FOliveToolResult::ExecutionError(TEXT("SPAWN_FAILED"), TEXT("SpawnActor returned nullptr."), TEXT("Check the editor log."));

    if (!Args.Label.IsEmpty())
    {
        A->SetActorLabel(Args.Label);
    }

    // Optional: mesh + material on the first mesh component
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
        return FOliveToolResult::ValidationError(TEXT("ACTOR_NOT_FOUND"),
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
        return FOliveToolResult::ValidationError(TEXT("ACTOR_NOT_FOUND"),
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
        return FOliveToolResult::ValidationError(TEXT("COMPONENT_NOT_FOUND"),
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
        return FOliveToolResult::ValidationError(TEXT("ACTOR_NOT_FOUND"),
            FString::Printf(TEXT("Actor '%s' not found."), *ActorName), TEXT(""));

    UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!Mat)
        return FOliveToolResult::ValidationError(TEXT("MATERIAL_NOT_FOUND"),
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
        return FOliveToolResult::ValidationError(TEXT("COMPONENT_NOT_FOUND"),
            TEXT("No MeshComponent matched."), TEXT("Call level.get_actor_materials to list components."));

    FScopedTransaction Tx(LOCTEXT("OliveApplyMaterial", "Olive: Apply Material"));
    Target->Modify();
    Target->SetMaterial(Slot, Mat);
    return FOliveToolResult::Success();
}

#undef LOCTEXT_NAMESPACE
```

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Level/Public/Writer/OliveLevelWriter.h Source/OliveAIEditor/Level/Private/Writer/OliveLevelWriter.cpp
git commit -m "P4: implement FOliveLevelWriter (spawn/delete/transform/physics/material)"
```

---

### Task 5: Writer unit tests

**Files:**
- Create: `Source/OliveAIEditor/Private/Tests/Level/OliveLevelWriterTests.cpp`

- [ ] **Step 1: Write tests**

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Writer/OliveLevelWriter.h"
#include "Reader/OliveLevelReader.h"
#include "Editor.h"
#include "Subsystems/UnrealEditorSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelWriterSpawnTest,
    "OliveAI.Level.Writer.SpawnActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelWriterSpawnTest::RunTest(const FString& Parameters)
{
    FOliveSpawnActorArgs Args;
    Args.ClassPath = TEXT("/Script/Engine.StaticMeshActor");
    Args.Label = TEXT("OliveTest_SpawnWriter_SMA");
    Args.Location = FVector(100, 200, 300);

    FOliveToolResult R = FOliveLevelWriter::Get().SpawnActor(Args);
    TestTrue(TEXT("SpawnActor should succeed"), R.bSuccess);

    AActor* A = FOliveLevelReader::Get().FindActorByName(TEXT("OliveTest_SpawnWriter_SMA"));
    TestNotNull(TEXT("Spawned actor should be findable"), A);
    if (A) TestEqual(TEXT("Location"), A->GetActorLocation(), FVector(100, 200, 300));

    if (A) A->Destroy();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelWriterDeleteTest,
    "OliveAI.Level.Writer.DeleteActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelWriterDeleteTest::RunTest(const FString& Parameters)
{
    FOliveSpawnActorArgs Args;
    Args.ClassPath = TEXT("/Script/Engine.StaticMeshActor");
    Args.Label = TEXT("OliveTest_Delete_SMA");
    FOliveLevelWriter::Get().SpawnActor(Args);

    FOliveToolResult R = FOliveLevelWriter::Get().DeleteActor(TEXT("OliveTest_Delete_SMA"));
    TestTrue(TEXT("DeleteActor should succeed"), R.bSuccess);

    AActor* A = FOliveLevelReader::Get().FindActorByName(TEXT("OliveTest_Delete_SMA"));
    TestNull(TEXT("Actor should be gone"), A);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelWriterBadClassTest,
    "OliveAI.Level.Writer.BadClassValidationError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelWriterBadClassTest::RunTest(const FString& Parameters)
{
    FOliveSpawnActorArgs Args;
    Args.ClassPath = TEXT("/Script/Engine.DoesNotExist_XYZ");
    FOliveToolResult R = FOliveLevelWriter::Get().SpawnActor(Args);
    TestFalse(TEXT("SpawnActor with bad class fails"), R.bSuccess);
    TestEqual(TEXT("Error code"), R.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
    return true;
}
```

- [ ] **Step 2: Run tests**

Session Frontend > Automation > `OliveAI.Level.Writer.*`. All pass.

- [ ] **Step 3: Commit**

```bash
git add Source/OliveAIEditor/Private/Tests/Level/OliveLevelWriterTests.cpp
git commit -m "P4: LevelWriter tests"
```

---

### Task 6: Schemas for 8 tools

**Files:**
- Create: `Source/OliveAIEditor/Level/Public/MCP/OliveLevelSchemas.h`
- Create: `Source/OliveAIEditor/Level/Private/MCP/OliveLevelSchemas.cpp`

- [ ] **Step 1: Write the header**

```cpp
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
```

- [ ] **Step 2: Write the cpp**

Match the JSON-schema style used by `OliveBlueprintSchemas.cpp` — open that file for the exact macro/helper pattern. Each schema is `{"type": "object", "properties": {...}, "required": [...]}`.

Example for `spawn_actor`:

```cpp
TSharedPtr<FJsonObject> OliveLevelSchemas::SpawnActor()
{
    TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

    auto Str = [](const FString& Desc){
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), TEXT("string"));
        O->SetStringField(TEXT("description"), Desc);
        return O;
    };
    auto Num = [](const FString& Desc){
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), TEXT("number"));
        O->SetStringField(TEXT("description"), Desc);
        return O;
    };
    auto Bool = [](const FString& Desc){
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), TEXT("boolean"));
        O->SetStringField(TEXT("description"), Desc);
        return O;
    };
    auto Vec3 = [&](const FString& Desc){
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), TEXT("array"));
        O->SetStringField(TEXT("description"), Desc);
        TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
        Items->SetStringField(TEXT("type"), TEXT("number"));
        O->SetObjectField(TEXT("items"), Items);
        O->SetNumberField(TEXT("minItems"), 3);
        O->SetNumberField(TEXT("maxItems"), 3);
        return O;
    };

    Props->SetObjectField(TEXT("class_path"), Str(TEXT("Full class path, e.g. /Script/Engine.StaticMeshActor")));
    Props->SetObjectField(TEXT("mesh_path"), Str(TEXT("Optional static mesh asset path")));
    Props->SetObjectField(TEXT("material_path"), Str(TEXT("Optional material asset path")));
    Props->SetObjectField(TEXT("label"), Str(TEXT("Optional actor label; auto-generated if empty")));
    Props->SetObjectField(TEXT("location"), Vec3(TEXT("[x, y, z]")));
    Props->SetObjectField(TEXT("rotation"), Vec3(TEXT("[pitch, yaw, roll]")));
    Props->SetObjectField(TEXT("scale"), Vec3(TEXT("[sx, sy, sz]")));
    Props->SetObjectField(TEXT("simulate_physics"), Bool(TEXT("Simulate physics on the primitive component")));
    Props->SetObjectField(TEXT("enable_gravity"), Bool(TEXT("Enable gravity")));
    Props->SetObjectField(TEXT("mass"), Num(TEXT("Mass in kg (only applied if simulate_physics=true)")));

    Schema->SetObjectField(TEXT("properties"), Props);

    TArray<TSharedPtr<FJsonValue>> Required;
    Required.Add(MakeShared<FJsonValueString>(TEXT("class_path")));
    Schema->SetArrayField(TEXT("required"), Required);

    return Schema;
}
```

Implement similar schemas for the remaining 7 tools. Each describes its parameters with `type`, `description`, and `required` arrays. Use identical helper lambdas.

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Clean build.

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Level/Public/MCP/OliveLevelSchemas.h Source/OliveAIEditor/Level/Private/MCP/OliveLevelSchemas.cpp
git commit -m "P4: level.* tool schemas"
```

---

### Task 7: Tool handlers + registration

**Files:**
- Create: `Source/OliveAIEditor/Level/Public/MCP/OliveLevelToolHandlers.h`
- Create: `Source/OliveAIEditor/Level/Private/MCP/OliveLevelToolHandlers.cpp`
- Modify: `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`

- [ ] **Step 1: Write the header**

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

class FOliveLevelToolHandlers
{
public:
    static FOliveLevelToolHandlers& Get();
    void RegisterAllTools();

private:
    FOliveLevelToolHandlers() = default;
    TArray<FString> RegisteredToolNames;

    // Handlers
    FOliveToolResult HandleListActors(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleFindActors(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleSpawnActor(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleDeleteActor(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleSetTransform(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleSetPhysics(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleApplyMaterial(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleGetActorMaterials(const TSharedPtr<FJsonObject>& Params);
};
```

- [ ] **Step 2: Write the cpp**

Follow the pattern in `OlivePythonToolHandlers.cpp`. For each tool: extract params from the JSON object, validate required fields, call the Reader/Writer, build `FOliveToolResult` with `Data` populated for reads.

Registration function:

```cpp
void FOliveLevelToolHandlers::RegisterAllTools()
{
    FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

    Registry.RegisterTool(
        TEXT("level.list_actors"),
        TEXT("List all actors in the current editor level."),
        OliveLevelSchemas::ListActors(),
        FOliveToolHandler::CreateRaw(this, &FOliveLevelToolHandlers::HandleListActors),
        {TEXT("level"), TEXT("read")},
        TEXT("level")
    );
    RegisteredToolNames.Add(TEXT("level.list_actors"));

    // ... repeat for the other 7 tools
}
```

Handler example (pattern for reads):

```cpp
FOliveToolResult FOliveLevelToolHandlers::HandleListActors(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FOliveActorSummary> Actors = FOliveLevelReader::Get().ListActors();

    TArray<TSharedPtr<FJsonValue>> JsonActors;
    for (const FOliveActorSummary& S : Actors)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"), S.Name);
        O->SetStringField(TEXT("class_path"), S.ClassPath);
        O->SetStringField(TEXT("folder"), S.FolderPath);
        TArray<TSharedPtr<FJsonValue>> Loc;
        Loc.Add(MakeShared<FJsonValueNumber>(S.Location.X));
        Loc.Add(MakeShared<FJsonValueNumber>(S.Location.Y));
        Loc.Add(MakeShared<FJsonValueNumber>(S.Location.Z));
        O->SetArrayField(TEXT("location"), Loc);
        JsonActors.Add(MakeShared<FJsonValueObject>(O));
    }

    FOliveToolResult R = FOliveToolResult::Success();
    R.Data = MakeShared<FJsonObject>();
    R.Data->SetArrayField(TEXT("actors"), JsonActors);
    R.Data->SetNumberField(TEXT("count"), Actors.Num());
    return R;
}
```

Write handlers for all 8 tools. Required-field validation returns 3-part errors:

```cpp
if (!Params.IsValid() || !Params->HasField(TEXT("class_path")))
{
    return FOliveToolResult::ValidationError(
        TEXT("MISSING_PARAM"),
        TEXT("Parameter 'class_path' is required."),
        TEXT("Example: {\"class_path\": \"/Script/Engine.StaticMeshActor\"}"));
}
```

- [ ] **Step 3: Register in `OliveAIEditorModule.cpp`**

Open `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`. Find `OnPostEngineInit()`. Find the `FOliveCrossSystemToolHandlers::Get().RegisterAllTools()` call (step 10). Immediately after it, add:

```cpp
#include "MCP/OliveLevelToolHandlers.h"
// ...
FOliveLevelToolHandlers::Get().RegisterAllTools();
UE_LOG(LogOliveAI, Log, TEXT("Registered level.* MCP tools"));
```

Include goes at the top of the file with the other MCP-handler includes.

- [ ] **Step 4: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Clean build.

- [ ] **Step 5: Commit**

```bash
git add Source/OliveAIEditor/Level/Public/MCP/OliveLevelToolHandlers.h Source/OliveAIEditor/Level/Private/MCP/OliveLevelToolHandlers.cpp Source/OliveAIEditor/Private/OliveAIEditorModule.cpp
git commit -m "P4: level.* tool handlers + module registration"
```

---

### Task 8: End-to-end tool tests

**Files:**
- Create: `Source/OliveAIEditor/Private/Tests/Level/OliveLevelToolsTests.cpp`

- [ ] **Step 1: Write end-to-end tests**

One test per tool: build a params JSON object, call `FOliveToolRegistry::Get().InvokeTool(name, params)`, assert success and data shape.

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "MCP/OliveToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelListActorsToolTest,
    "OliveAI.Level.Tool.ListActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelListActorsToolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    FOliveToolResult R = FOliveToolRegistry::Get().InvokeTool(TEXT("level.list_actors"), Params);
    TestTrue(TEXT("level.list_actors should succeed"), R.bSuccess);
    TestTrue(TEXT("Result has 'actors' array"), R.Data.IsValid() && R.Data->HasField(TEXT("actors")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelSpawnAndDeleteToolTest,
    "OliveAI.Level.Tool.SpawnAndDelete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelSpawnAndDeleteToolTest::RunTest(const FString& Parameters)
{
    {
        TSharedPtr<FJsonObject> SpawnParams = MakeShared<FJsonObject>();
        SpawnParams->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.StaticMeshActor"));
        SpawnParams->SetStringField(TEXT("label"), TEXT("OliveTest_ToolSpawn_SMA"));
        FOliveToolResult R = FOliveToolRegistry::Get().InvokeTool(TEXT("level.spawn_actor"), SpawnParams);
        TestTrue(TEXT("spawn_actor should succeed"), R.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> DelParams = MakeShared<FJsonObject>();
        DelParams->SetStringField(TEXT("name"), TEXT("OliveTest_ToolSpawn_SMA"));
        FOliveToolResult R = FOliveToolRegistry::Get().InvokeTool(TEXT("level.delete_actor"), DelParams);
        TestTrue(TEXT("delete_actor should succeed"), R.bSuccess);
    }
    return true;
}
```

Add analogous tests for `level.find_actors`, `level.set_transform`, `level.set_physics`, `level.apply_material`, `level.get_actor_materials`, `level.spawn_actor` with invalid class_path (validation-error path).

- [ ] **Step 2: Run tests**

Session Frontend > Automation > `OliveAI.Level.Tool.*`. All pass.

- [ ] **Step 3: Commit**

```bash
git add Source/OliveAIEditor/Private/Tests/Level/OliveLevelToolsTests.cpp
git commit -m "P4: end-to-end level.* tool tests"
```

---

### Task 9: CLAUDE.md update

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Add the new module**

In the "Module Layout" section, add `Level/` to the tree. In "Startup Order" `OnPostEngineInit()`, add the new step after step 10. In "Key File Locations" table, add:

```markdown
| Level reader | `Source/OliveAIEditor/Level/Public/Reader/OliveLevelReader.h` |
| Level writer | `Source/OliveAIEditor/Level/Public/Writer/OliveLevelWriter.h` |
| Level tool handlers | `Source/OliveAIEditor/Level/Private/MCP/OliveLevelToolHandlers.cpp` |
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "P4: document level.* module in CLAUDE.md"
```

---

## Acceptance criteria

1. `ubt-build-5.5` green.
2. `OliveAI.Level.*` automation suite green (all 8 tools covered).
3. `FOliveToolRegistry::GetToolNames()` contains all 8 `level.*` tool names.
4. Spawning an actor, editing, undoing (Ctrl+Z), redoing all work in-editor — transaction integration verified manually.
5. `CLAUDE.md` updated.

## Out of scope

- `material.*` module (that's P6).
- World builders (that's P7).
- Integration with any snapshot/rollback beyond what the pipeline already provides.
- Prompt updates mentioning level tools (covered by P2's worker-prompt pass, if at all — level tools' schemas are enough for the LLM to use them).
