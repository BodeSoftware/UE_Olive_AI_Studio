// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Providers/OliveCodexProvider.h"

namespace OliveCodexProviderTests
{
	static constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCodexProviderParseMcpToolCallTest,
	"OliveAI.CodexProvider.ParseMcpToolCall",
	OliveCodexProviderTests::TestFlags)

bool FOliveCodexProviderParseMcpToolCallTest::RunTest(const FString& Parameters)
{
	const FString Line = TEXT("{\"type\":\"item.completed\",\"item\":{\"id\":\"item_1\",\"type\":\"mcp_tool_call\",\"tool\":\"blueprint.read\"}}");

	FString ToolName;
	const bool bParsed = FOliveCodexProvider::ExtractMcpToolNameFromJsonLine(Line, ToolName);

	TestTrue(TEXT("mcp_tool_call should parse as MCP tool event"), bParsed);
	TestEqual(TEXT("Tool name should be extracted from 'tool' field"), ToolName, TEXT("blueprint.read"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCodexProviderParseMcpCallTest,
	"OliveAI.CodexProvider.ParseMcpCall",
	OliveCodexProviderTests::TestFlags)

bool FOliveCodexProviderParseMcpCallTest::RunTest(const FString& Parameters)
{
	const FString Line = TEXT("{\"type\":\"item.completed\",\"item\":{\"id\":\"item_2\",\"type\":\"mcp_call\",\"name\":\"blueprint.add_variable\"}}");

	FString ToolName;
	const bool bParsed = FOliveCodexProvider::ExtractMcpToolNameFromJsonLine(Line, ToolName);

	TestTrue(TEXT("mcp_call should parse as MCP tool event"), bParsed);
	TestEqual(TEXT("Tool name should be extracted from 'name' field"), ToolName, TEXT("blueprint.add_variable"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCodexProviderParseNonToolItemTest,
	"OliveAI.CodexProvider.ParseNonToolItem",
	OliveCodexProviderTests::TestFlags)

bool FOliveCodexProviderParseNonToolItemTest::RunTest(const FString& Parameters)
{
	const FString Line = TEXT("{\"type\":\"item.completed\",\"item\":{\"id\":\"item_3\",\"type\":\"agent_message\",\"text\":\"hello\"}}");

	FString ToolName;
	const bool bParsed = FOliveCodexProvider::ExtractMcpToolNameFromJsonLine(Line, ToolName);

	TestFalse(TEXT("Non-tool items should not parse as MCP tool events"), bParsed);
	TestTrue(TEXT("Tool name should be empty when parse fails"), ToolName.IsEmpty());
	return true;
}

