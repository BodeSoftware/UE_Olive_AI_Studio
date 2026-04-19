// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "MCP/OliveToolRegistry.h"

/**
 * P5 PCG family consolidation -- alias round-trip coverage.
 *
 * Each test calls the legacy tool name with minimum params and asserts that:
 *   (a) the result is NOT TOOL_NOT_FOUND (proves the alias resolved and routed
 *       to a real consolidated handler), AND
 *   (b) the request was recognised far enough to reach the consolidated
 *       dispatcher (per-handler validation errors like PCG_GRAPH_NOT_FOUND are OK).
 *
 * We intentionally target a PCG graph that does not exist so the tests do not
 * mutate the project content. We assert on the error code, not on the exact
 * error string, so minor wording changes do not break coverage.
 */
namespace OlivePCGAliasTests
{
	static constexpr EAutomationTestFlags TestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** True if the alias resolved to a registered tool. The error code must not be TOOL_NOT_FOUND. */
	static bool AliasResolved(const FOliveToolResult& Result)
	{
		if (Result.Messages.Num() == 0)
		{
			// Success counts as resolved, too.
			return true;
		}
		return Result.Messages[0].Code != TEXT("TOOL_NOT_FOUND");
	}

	static TSharedPtr<FJsonObject> MakePCGParams(const FString& Path)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("path"), Path);
		return P;
	}
}

// ---------------------------------------------------------------------------
// pcg.add_node -> pcg.add(node_kind='node')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOlivePCGAliasAddNodeTest,
	"OliveAI.MCP.Alias.PCG.AddNode",
	OlivePCGAliasTests::TestFlags)

bool FOlivePCGAliasAddNodeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OlivePCGAliasTests::MakePCGParams(TEXT("/Game/Tests/PCG_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("settings_class"), TEXT("SurfaceSampler"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("pcg.add_node"), P);

	TestTrue(TEXT("Alias pcg.add_node must resolve (no TOOL_NOT_FOUND)"),
		OlivePCGAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target PCG graph does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// pcg.add_subgraph -> pcg.add(node_kind='subgraph')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOlivePCGAliasAddSubgraphTest,
	"OliveAI.MCP.Alias.PCG.AddSubgraph",
	OlivePCGAliasTests::TestFlags)

bool FOlivePCGAliasAddSubgraphTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OlivePCGAliasTests::MakePCGParams(TEXT("/Game/Tests/PCG_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("subgraph_path"), TEXT("/Game/Tests/PCG_Sub_DoesNotExist"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("pcg.add_subgraph"), P);

	TestTrue(TEXT("Alias pcg.add_subgraph must resolve (no TOOL_NOT_FOUND)"),
		OlivePCGAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target PCG graph does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// pcg.modify_node -> pcg.modify(entity='node')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOlivePCGAliasModifyNodeTest,
	"OliveAI.MCP.Alias.PCG.ModifyNode",
	OlivePCGAliasTests::TestFlags)

bool FOlivePCGAliasModifyNodeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OlivePCGAliasTests::MakePCGParams(TEXT("/Game/Tests/PCG_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("node_id"), TEXT("node_0"));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("PointsPerSquaredMeter"), TEXT("1.5"));
	P->SetObjectField(TEXT("properties"), Properties);

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("pcg.modify_node"), P);

	TestTrue(TEXT("Alias pcg.modify_node must resolve (no TOOL_NOT_FOUND)"),
		OlivePCGAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target PCG graph does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// pcg.disconnect -> pcg.connect(break=true)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOlivePCGAliasDisconnectTest,
	"OliveAI.MCP.Alias.PCG.Disconnect",
	OlivePCGAliasTests::TestFlags)

bool FOlivePCGAliasDisconnectTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OlivePCGAliasTests::MakePCGParams(TEXT("/Game/Tests/PCG_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("source_node_id"), TEXT("node_0"));
	P->SetStringField(TEXT("source_pin"), TEXT("Out"));
	P->SetStringField(TEXT("target_node_id"), TEXT("node_1"));
	P->SetStringField(TEXT("target_pin"), TEXT("In"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("pcg.disconnect"), P);

	TestTrue(TEXT("Alias pcg.disconnect must resolve (no TOOL_NOT_FOUND)"),
		OlivePCGAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target PCG graph does not exist"), R.bSuccess);
	return true;
}
