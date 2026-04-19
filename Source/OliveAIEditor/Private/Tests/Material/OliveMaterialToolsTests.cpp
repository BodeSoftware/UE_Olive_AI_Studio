// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "MCP/OliveToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialListToolTest,
    "OliveAI.Material.Tool.List",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialListToolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("search_path"), TEXT("/Engine"));
    P->SetBoolField(TEXT("include_engine_materials"), true);
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("material.list"), P);
    TestTrue(TEXT("material.list success"), R.bSuccess);
    TestTrue(TEXT("Has materials array"),
        R.Data.IsValid() && R.Data->HasField(TEXT("materials")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialReadToolTest,
    "OliveAI.Material.Tool.Read",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialReadToolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("path"), TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("material.read"), P);
    TestTrue(TEXT("material.read success"), R.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialReadMissingPathTest,
    "OliveAI.Material.Tool.ReadMissingPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialReadMissingPathTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("material.read"), P);
    TestFalse(TEXT("Missing path should fail"), R.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialApplyMissingArgsTest,
    "OliveAI.Material.Tool.ApplyMissingArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialApplyMissingArgsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("material.apply_to_component"), P);
    TestFalse(TEXT("Missing required params should fail"), R.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialSetColorMissingArgsTest,
    "OliveAI.Material.Tool.SetColorMissingArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialSetColorMissingArgsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("material.set_parameter_color"), P);
    TestFalse(TEXT("Missing color args should fail"), R.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialCreateInstanceBadParentTest,
    "OliveAI.Material.Tool.CreateInstanceBadParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialCreateInstanceBadParentTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("new_instance_path"), TEXT("/Game/Tests/MI_Olive_ShouldNotCreate.MI_Olive_ShouldNotCreate"));
    P->SetStringField(TEXT("parent_material_path"), TEXT("/Engine/Does_Not_Exist.Does_Not_Exist"));
    FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("material.create_instance"), P);
    TestFalse(TEXT("Bad parent should fail"), R.bSuccess);
    if (R.Messages.Num() > 0)
    {
        TestEqual(TEXT("Error code"), R.Messages[0].Code, FString(TEXT("PARENT_NOT_FOUND")));
    }
    return true;
}
