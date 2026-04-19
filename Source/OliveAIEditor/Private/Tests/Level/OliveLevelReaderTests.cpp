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
