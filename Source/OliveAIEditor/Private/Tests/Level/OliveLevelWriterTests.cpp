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
    if (R.Messages.Num() > 0)
    {
        TestEqual(TEXT("Error code"), R.Messages[0].Code, FString(TEXT("CLASS_NOT_FOUND")));
    }
    return true;
}
