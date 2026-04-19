// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "MCP/OliveToolRegistry.h"

/**
 * P5 Blueprint family consolidation -- alias round-trip coverage.
 *
 * Each test calls the legacy tool name with minimum params and asserts that:
 *   (a) the result is NOT TOOL_NOT_FOUND (proves the alias resolved and routed
 *       to a real consolidated handler), AND
 *   (b) the request was recognised far enough to reach the consolidated
 *       dispatcher (per-handler validation errors like ASSET_NOT_FOUND are OK).
 *
 * We intentionally target a Blueprint that does not exist so the tests do not
 * mutate the project content. We assert on the error code, not on the exact
 * error string, so minor wording changes do not break coverage.
 */
namespace OliveBlueprintAliasTests
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

	static TSharedPtr<FJsonObject> MakeBPParams(const FString& Path)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("path"), Path);
		return P;
	}
}

// ---------------------------------------------------------------------------
// blueprint.remove_node -> blueprint.delete(entity='node')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBPAliasRemoveNodeTest,
	"OliveAI.MCP.Alias.Blueprint.RemoveNode",
	OliveBlueprintAliasTests::TestFlags)

bool FOliveBPAliasRemoveNodeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBlueprintAliasTests::MakeBPParams(TEXT("/Game/Tests/BP_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("node_id"), TEXT("fake_node_0"));
	P->SetStringField(TEXT("graph"), TEXT("EventGraph"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.remove_node"), P);

	TestTrue(TEXT("Alias blueprint.remove_node must resolve (no TOOL_NOT_FOUND)"),
		OliveBlueprintAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Blueprint does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// blueprint.remove_component -> blueprint.delete(entity='component')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBPAliasRemoveComponentTest,
	"OliveAI.MCP.Alias.Blueprint.RemoveComponent",
	OliveBlueprintAliasTests::TestFlags)

bool FOliveBPAliasRemoveComponentTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBlueprintAliasTests::MakeBPParams(TEXT("/Game/Tests/BP_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("name"), TEXT("FakeComp"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.remove_component"), P);

	TestTrue(TEXT("Alias blueprint.remove_component must resolve (no TOOL_NOT_FOUND)"),
		OliveBlueprintAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Blueprint does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// blueprint.modify_component -> blueprint.modify(entity='component', action='set_properties')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBPAliasModifyComponentTest,
	"OliveAI.MCP.Alias.Blueprint.ModifyComponent",
	OliveBlueprintAliasTests::TestFlags)

bool FOliveBPAliasModifyComponentTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBlueprintAliasTests::MakeBPParams(TEXT("/Game/Tests/BP_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("name"), TEXT("FakeComp"));
	P->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.modify_component"), P);

	TestTrue(TEXT("Alias blueprint.modify_component must resolve (no TOOL_NOT_FOUND)"),
		OliveBlueprintAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Blueprint does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// blueprint.set_pin_default -> blueprint.modify(entity='pin_default')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBPAliasSetPinDefaultTest,
	"OliveAI.MCP.Alias.Blueprint.SetPinDefault",
	OliveBlueprintAliasTests::TestFlags)

bool FOliveBPAliasSetPinDefaultTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBlueprintAliasTests::MakeBPParams(TEXT("/Game/Tests/BP_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("graph"), TEXT("EventGraph"));
	P->SetStringField(TEXT("pin"), TEXT("fake_node.Value"));
	P->SetStringField(TEXT("value"), TEXT("42"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.set_pin_default"), P);

	TestTrue(TEXT("Alias blueprint.set_pin_default must resolve (no TOOL_NOT_FOUND)"),
		OliveBlueprintAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Blueprint does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// blueprint.add_node -> blueprint.add(entity='node')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBPAliasAddNodeTest,
	"OliveAI.MCP.Alias.Blueprint.AddNode",
	OliveBlueprintAliasTests::TestFlags)

bool FOliveBPAliasAddNodeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBlueprintAliasTests::MakeBPParams(TEXT("/Game/Tests/BP_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("graph"), TEXT("EventGraph"));
	P->SetStringField(TEXT("type"), TEXT("Branch"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.add_node"), P);

	TestTrue(TEXT("Alias blueprint.add_node must resolve (no TOOL_NOT_FOUND)"),
		OliveBlueprintAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Blueprint does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// blueprint.add_custom_event -> blueprint.add(entity='custom_event')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBPAliasAddCustomEventTest,
	"OliveAI.MCP.Alias.Blueprint.AddCustomEvent",
	OliveBlueprintAliasTests::TestFlags)

bool FOliveBPAliasAddCustomEventTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBlueprintAliasTests::MakeBPParams(TEXT("/Game/Tests/BP_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("name"), TEXT("MyCustomEvent"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.add_custom_event"), P);

	TestTrue(TEXT("Alias blueprint.add_custom_event must resolve (no TOOL_NOT_FOUND)"),
		OliveBlueprintAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Blueprint does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// blueprint.get_node_pins -> blueprint.read(section='pins')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBPAliasGetNodePinsTest,
	"OliveAI.MCP.Alias.Blueprint.GetNodePins",
	OliveBlueprintAliasTests::TestFlags)

bool FOliveBPAliasGetNodePinsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBlueprintAliasTests::MakeBPParams(TEXT("/Game/Tests/BP_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("graph"), TEXT("EventGraph"));
	P->SetStringField(TEXT("node_id"), TEXT("fake_node_0"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.get_node_pins"), P);

	TestTrue(TEXT("Alias blueprint.get_node_pins must resolve (no TOOL_NOT_FOUND)"),
		OliveBlueprintAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Blueprint does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// blueprint.verify_completion -> blueprint.compile(verify=true)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBPAliasVerifyCompletionTest,
	"OliveAI.MCP.Alias.Blueprint.VerifyCompletion",
	OliveBlueprintAliasTests::TestFlags)

bool FOliveBPAliasVerifyCompletionTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	// verify_completion historically used asset_path; confirm alias normalizes.
	P->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/BP_DoesNotExist_P5Alias"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("blueprint.verify_completion"), P);

	TestTrue(TEXT("Alias blueprint.verify_completion must resolve (no TOOL_NOT_FOUND)"),
		OliveBlueprintAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Blueprint does not exist"), R.bSuccess);
	return true;
}

// ===========================================================================
// P5 BT + Blackboard family consolidation -- alias round-trip coverage.
//
// Same shape as the Blueprint tests above: legacy tool names must resolve (no
// TOOL_NOT_FOUND) and the per-handler validation can return whatever error
// code it likes.
// ===========================================================================

namespace OliveBTAliasTests
{
	using OliveBlueprintAliasTests::TestFlags;
	using OliveBlueprintAliasTests::AliasResolved;

	static TSharedPtr<FJsonObject> MakeBTParams(const FString& Path)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("path"), Path);
		return P;
	}
}

// ---------------------------------------------------------------------------
// behaviortree.add_composite -> behaviortree.add(node_type='composite')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBTAliasAddCompositeTest,
	"OliveAI.MCP.Alias.BT.AddComposite",
	OliveBTAliasTests::TestFlags)

bool FOliveBTAliasAddCompositeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBTAliasTests::MakeBTParams(TEXT("/Game/Tests/BT_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("parent_node_id"), TEXT("root"));
	P->SetStringField(TEXT("composite_type"), TEXT("Selector"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("behaviortree.add_composite"), P);

	TestTrue(TEXT("Alias behaviortree.add_composite must resolve (no TOOL_NOT_FOUND)"),
		OliveBTAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Behavior Tree does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// behaviortree.modify_node -> behaviortree.modify(entity='node')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBTAliasModifyNodeTest,
	"OliveAI.MCP.Alias.BT.ModifyNode",
	OliveBTAliasTests::TestFlags)

bool FOliveBTAliasModifyNodeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBTAliasTests::MakeBTParams(TEXT("/Game/Tests/BT_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("node_id"), TEXT("fake_node_0"));
	P->SetStringField(TEXT("property"), TEXT("FlowAbortMode"));
	P->SetStringField(TEXT("value"), TEXT("Both"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("behaviortree.modify_node"), P);

	TestTrue(TEXT("Alias behaviortree.modify_node must resolve (no TOOL_NOT_FOUND)"),
		OliveBTAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Behavior Tree does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// behaviortree.set_blackboard -> behaviortree.modify(entity='blackboard_ref')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBTAliasSetBlackboardTest,
	"OliveAI.MCP.Alias.BT.SetBlackboard",
	OliveBTAliasTests::TestFlags)

bool FOliveBTAliasSetBlackboardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBTAliasTests::MakeBTParams(TEXT("/Game/Tests/BT_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("blackboard"), TEXT("/Game/Tests/BB_DoesNotExist_P5Alias"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("behaviortree.set_blackboard"), P);

	TestTrue(TEXT("Alias behaviortree.set_blackboard must resolve (no TOOL_NOT_FOUND)"),
		OliveBTAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Behavior Tree does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// behaviortree.remove_node -> behaviortree.remove (pass-through)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBTAliasRemoveNodeTest,
	"OliveAI.MCP.Alias.BT.RemoveNode",
	OliveBTAliasTests::TestFlags)

bool FOliveBTAliasRemoveNodeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBTAliasTests::MakeBTParams(TEXT("/Game/Tests/BT_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("node_id"), TEXT("fake_node_0"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("behaviortree.remove_node"), P);

	TestTrue(TEXT("Alias behaviortree.remove_node must resolve (no TOOL_NOT_FOUND)"),
		OliveBTAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Behavior Tree does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// blackboard.add_key -> blackboard.modify(action='add_key')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBBAliasAddKeyTest,
	"OliveAI.MCP.Alias.Blackboard.AddKey",
	OliveBTAliasTests::TestFlags)

bool FOliveBBAliasAddKeyTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBTAliasTests::MakeBTParams(TEXT("/Game/Tests/BB_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("name"), TEXT("FakeKey"));
	P->SetStringField(TEXT("key_type"), TEXT("bool"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("blackboard.add_key"), P);

	TestTrue(TEXT("Alias blackboard.add_key must resolve (no TOOL_NOT_FOUND)"),
		OliveBTAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Blackboard does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// blackboard.set_parent -> blackboard.modify(action='set_parent')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBBAliasSetParentTest,
	"OliveAI.MCP.Alias.Blackboard.SetParent",
	OliveBTAliasTests::TestFlags)

bool FOliveBBAliasSetParentTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = OliveBTAliasTests::MakeBTParams(TEXT("/Game/Tests/BB_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("parent_path"), TEXT("/Game/Tests/BB_Parent_DoesNotExist"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("blackboard.set_parent"), P);

	TestTrue(TEXT("Alias blackboard.set_parent must resolve (no TOOL_NOT_FOUND)"),
		OliveBTAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Blackboard does not exist"), R.bSuccess);
	return true;
}
