// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "OliveMaterialReader.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialListEngineTest,
    "OliveAI.Material.Reader.ListEngineIncludesBasic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialListEngineTest::RunTest(const FString& Parameters)
{
    TArray<FOliveMaterialSummary> Mats = FOliveMaterialReader::Get().ListMaterials(TEXT("/Engine"), true);
    TestTrue(TEXT("Engine material enumeration returns >0"), Mats.Num() > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialReadBasicTest,
    "OliveAI.Material.Reader.ReadEngineBasicMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialReadBasicTest::RunTest(const FString& Parameters)
{
    FOliveMaterialDetails D = FOliveMaterialReader::Get().ReadMaterial(
        TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
    TestEqual(TEXT("Path matches"),
        D.Path, FString(TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    TestFalse(TEXT("DefaultMaterial is not an instance"), D.bIsInstance);
    return true;
}
