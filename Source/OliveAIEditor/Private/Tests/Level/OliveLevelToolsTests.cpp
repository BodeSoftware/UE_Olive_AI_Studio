// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "MCP/OliveToolRegistry.h"
#include "Reader/OliveLevelReader.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"

// --- Helpers ---
namespace
{
    TSharedPtr<FJsonObject> MakeParams()
    {
        return MakeShared<FJsonObject>();
    }

    TSharedPtr<FJsonValue> Num(double D) { return MakeShared<FJsonValueNumber>(D); }
    TArray<TSharedPtr<FJsonValue>> Vec3(double X, double Y, double Z)
    {
        return { Num(X), Num(Y), Num(Z) };
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelListActorsToolTest,
    "OliveAI.Level.Tool.ListActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelListActorsToolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeParams();
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("level.list_actors"), P);
    TestTrue(TEXT("level.list_actors should succeed"), R.bSuccess);
    TestTrue(TEXT("Result has 'actors'"),
        R.Data.IsValid() && R.Data->HasField(TEXT("actors")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelFindActorsToolTest,
    "OliveAI.Level.Tool.FindActors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelFindActorsToolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeParams();
    P->SetStringField(TEXT("pattern"), TEXT(""));
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("level.find_actors"), P);
    TestTrue(TEXT("level.find_actors should succeed"), R.bSuccess);
    TestTrue(TEXT("Result has 'count'"),
        R.Data.IsValid() && R.Data->HasField(TEXT("count")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelSpawnAndDeleteToolTest,
    "OliveAI.Level.Tool.SpawnAndDelete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelSpawnAndDeleteToolTest::RunTest(const FString& Parameters)
{
    // Spawn
    {
        TSharedPtr<FJsonObject> P = MakeParams();
        P->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.StaticMeshActor"));
        P->SetStringField(TEXT("label"), TEXT("OliveTest_ToolSpawn_SMA"));
        FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("level.spawn_actor"), P);
        TestTrue(TEXT("spawn_actor should succeed"), R.bSuccess);
    }
    // Delete
    {
        TSharedPtr<FJsonObject> P = MakeParams();
        P->SetStringField(TEXT("name"), TEXT("OliveTest_ToolSpawn_SMA"));
        FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("level.delete_actor"), P);
        TestTrue(TEXT("delete_actor should succeed"), R.bSuccess);
    }
    // Confirm gone
    AActor* A = FOliveLevelReader::Get().FindActorByName(TEXT("OliveTest_ToolSpawn_SMA"));
    TestNull(TEXT("Actor should be deleted"), A);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelSetTransformToolTest,
    "OliveAI.Level.Tool.SetTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelSetTransformToolTest::RunTest(const FString& Parameters)
{
    // Spawn
    {
        TSharedPtr<FJsonObject> P = MakeParams();
        P->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.StaticMeshActor"));
        P->SetStringField(TEXT("label"), TEXT("OliveTest_SetXform_SMA"));
        FOliveToolRegistry::Get().ExecuteTool(TEXT("level.spawn_actor"), P);
    }
    // Move
    TSharedPtr<FJsonObject> P = MakeParams();
    P->SetStringField(TEXT("name"), TEXT("OliveTest_SetXform_SMA"));
    P->SetArrayField(TEXT("location"), Vec3(10, 20, 30));
    P->SetArrayField(TEXT("rotation"), Vec3(0, 0, 0));
    P->SetArrayField(TEXT("scale"), Vec3(2, 2, 2));
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("level.set_transform"), P);
    TestTrue(TEXT("set_transform should succeed"), R.bSuccess);

    AActor* A = FOliveLevelReader::Get().FindActorByName(TEXT("OliveTest_SetXform_SMA"));
    TestNotNull(TEXT("Actor exists"), A);
    if (A)
    {
        TestEqual(TEXT("Location"), A->GetActorLocation(), FVector(10, 20, 30));
        TestEqual(TEXT("Scale"), A->GetActorScale3D(), FVector(2, 2, 2));
        A->Destroy();
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelSetPhysicsToolTest,
    "OliveAI.Level.Tool.SetPhysics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelSetPhysicsToolTest::RunTest(const FString& Parameters)
{
    // Spawn
    {
        TSharedPtr<FJsonObject> P = MakeParams();
        P->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.StaticMeshActor"));
        P->SetStringField(TEXT("label"), TEXT("OliveTest_Phys_SMA"));
        FOliveToolRegistry::Get().ExecuteTool(TEXT("level.spawn_actor"), P);
    }
    // Set physics — keep simulate_physics=false; StaticMeshActor with no mesh assigned cannot simulate
    TSharedPtr<FJsonObject> P = MakeParams();
    P->SetStringField(TEXT("name"), TEXT("OliveTest_Phys_SMA"));
    P->SetBoolField(TEXT("simulate_physics"), false);
    P->SetBoolField(TEXT("enable_gravity"), false);
    P->SetNumberField(TEXT("mass"), 50.0);
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("level.set_physics"), P);
    TestTrue(TEXT("set_physics should succeed"), R.bSuccess);

    AActor* A = FOliveLevelReader::Get().FindActorByName(TEXT("OliveTest_Phys_SMA"));
    if (A) A->Destroy();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelApplyMaterialToolTest,
    "OliveAI.Level.Tool.ApplyMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelApplyMaterialToolTest::RunTest(const FString& Parameters)
{
    // Spawn a StaticMeshActor with a mesh so its mesh component has a material slot
    {
        TSharedPtr<FJsonObject> P = MakeParams();
        P->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.StaticMeshActor"));
        P->SetStringField(TEXT("label"), TEXT("OliveTest_Mat_SMA"));
        P->SetStringField(TEXT("mesh_path"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        FOliveToolRegistry::Get().ExecuteTool(TEXT("level.spawn_actor"), P);
    }
    TSharedPtr<FJsonObject> P = MakeParams();
    P->SetStringField(TEXT("name"), TEXT("OliveTest_Mat_SMA"));
    P->SetNumberField(TEXT("slot"), 0);
    P->SetStringField(TEXT("material_path"), TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("level.apply_material"), P);
    TestTrue(TEXT("apply_material should succeed"), R.bSuccess);

    AActor* A = FOliveLevelReader::Get().FindActorByName(TEXT("OliveTest_Mat_SMA"));
    if (A) A->Destroy();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelGetActorMaterialsToolTest,
    "OliveAI.Level.Tool.GetActorMaterials",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelGetActorMaterialsToolTest::RunTest(const FString& Parameters)
{
    {
        TSharedPtr<FJsonObject> P = MakeParams();
        P->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.StaticMeshActor"));
        P->SetStringField(TEXT("label"), TEXT("OliveTest_GetMat_SMA"));
        P->SetStringField(TEXT("mesh_path"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        FOliveToolRegistry::Get().ExecuteTool(TEXT("level.spawn_actor"), P);
    }
    TSharedPtr<FJsonObject> P = MakeParams();
    P->SetStringField(TEXT("name"), TEXT("OliveTest_GetMat_SMA"));
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("level.get_actor_materials"), P);
    TestTrue(TEXT("get_actor_materials should succeed"), R.bSuccess);
    TestTrue(TEXT("Has 'materials' array"),
        R.Data.IsValid() && R.Data->HasField(TEXT("materials")));

    AActor* A = FOliveLevelReader::Get().FindActorByName(TEXT("OliveTest_GetMat_SMA"));
    if (A) A->Destroy();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveLevelSpawnBadClassToolTest,
    "OliveAI.Level.Tool.SpawnBadClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveLevelSpawnBadClassToolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeParams();
    P->SetStringField(TEXT("class_path"), TEXT("/Script/Engine.DoesNotExist_ABC"));
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("level.spawn_actor"), P);
    TestFalse(TEXT("Bad class should fail"), R.bSuccess);
    if (R.Messages.Num() > 0)
    {
        TestEqual(TEXT("Error code"), R.Messages[0].Code, FString(TEXT("CLASS_NOT_FOUND")));
    }
    return true;
}
