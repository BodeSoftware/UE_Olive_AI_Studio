// Copyright Bode Software. All Rights Reserved.

/**
 * OliveCLIToolCallParserTests.cpp
 *
 * Unit tests for FOliveCLIToolCallParser and FOliveCLIToolSchemaSerializer.
 * Validates parsing of <tool_call> XML blocks from CLI responses and
 * serialization of tool definitions into compact text for system prompts.
 */

#include "Misc/AutomationTest.h"

#include "Providers/OliveCLIToolCallParser.h"
#include "Providers/OliveCLIToolSchemaSerializer.h"
#include "MCP/OliveToolRegistry.h"

// ============================================================================
// Test Helpers
// ============================================================================

namespace OliveCLIToolTests
{
	static constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/**
	 * Build a minimal FOliveToolDefinition for serializer tests.
	 *
	 * @param Name           Tool name (e.g., "test.create")
	 * @param Desc           Tool description
	 * @param Category       Tool category for grouping
	 * @param Params         Map of param name -> param type (e.g., "path" -> "string")
	 * @param RequiredParams Names of parameters that are required
	 * @return A fully constructed FOliveToolDefinition with InputSchema populated
	 */
	static FOliveToolDefinition MakeTestTool(
		const FString& Name,
		const FString& Desc,
		const FString& Category,
		const TMap<FString, FString>& Params,
		const TArray<FString>& RequiredParams)
	{
		FOliveToolDefinition Def;
		Def.Name = Name;
		Def.Description = Desc;
		Def.Category = Category;

		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		for (const auto& P : Params)
		{
			TSharedPtr<FJsonObject> PropDef = MakeShared<FJsonObject>();
			PropDef->SetStringField(TEXT("type"), P.Value);
			Props->SetObjectField(P.Key, PropDef);
		}
		Schema->SetObjectField(TEXT("properties"), Props);

		TArray<TSharedPtr<FJsonValue>> Req;
		for (const FString& R : RequiredParams)
		{
			Req.Add(MakeShared<FJsonValueString>(R));
		}
		Schema->SetArrayField(TEXT("required"), Req);

		Def.InputSchema = Schema;
		return Def;
	}
}

// ============================================================================
// Parser Tests
// ============================================================================

// ----------------------------------------------------------------------------
// Test 1: Single tool call surrounded by prose text
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCLIParserSingleToolCallTest,
	"OliveAI.CLIParser.SingleToolCall",
	OliveCLIToolTests::TestFlags)

bool FOliveCLIParserSingleToolCallTest::RunTest(const FString& Parameters)
{
	const FString Input =
		TEXT("Here is my analysis of the project.\n")
		TEXT("<tool_call id=\"tc_1\">{\"name\":\"blueprint.create\",\"arguments\":{\"path\":\"/Game/Blueprints/BP_Test\"}}</tool_call>\n")
		TEXT("I have created the blueprint for you.");

	TArray<FOliveStreamChunk> ToolCalls;
	FString CleanText;

	const bool bResult = FOliveCLIToolCallParser::Parse(Input, ToolCalls, CleanText);

	TestTrue(TEXT("Parse should return true for valid tool call"), bResult);
	TestEqual(TEXT("Should extract exactly one tool call"), ToolCalls.Num(), 1);

	if (ToolCalls.Num() == 1)
	{
		const FOliveStreamChunk& Call = ToolCalls[0];
		TestTrue(TEXT("Chunk should be flagged as tool call"), Call.bIsToolCall);
		TestEqual(TEXT("Tool name should be 'blueprint.create'"), Call.ToolName, TEXT("blueprint.create"));
		TestEqual(TEXT("Tool call ID should be 'tc_1'"), Call.ToolCallId, TEXT("tc_1"));
		TestTrue(TEXT("Tool arguments should be valid"), Call.ToolArguments.IsValid());

		if (Call.ToolArguments.IsValid())
		{
			FString PathValue;
			TestTrue(TEXT("Arguments should have 'path' field"), Call.ToolArguments->TryGetStringField(TEXT("path"), PathValue));
			TestEqual(TEXT("Path argument should match"), PathValue, TEXT("/Game/Blueprints/BP_Test"));
		}
	}

	// Verify clean text has the prose but not the tool_call block
	TestTrue(TEXT("Clean text should contain leading prose"), CleanText.Contains(TEXT("Here is my analysis")));
	TestTrue(TEXT("Clean text should contain trailing prose"), CleanText.Contains(TEXT("I have created the blueprint")));
	TestFalse(TEXT("Clean text should NOT contain <tool_call> tag"), CleanText.Contains(TEXT("<tool_call")));
	TestFalse(TEXT("Clean text should NOT contain </tool_call> tag"), CleanText.Contains(TEXT("</tool_call>")));

	return true;
}

// ----------------------------------------------------------------------------
// Test 2: Multiple tool calls with interstitial text
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCLIParserMultipleToolCallsTest,
	"OliveAI.CLIParser.MultipleToolCalls",
	OliveCLIToolTests::TestFlags)

bool FOliveCLIParserMultipleToolCallsTest::RunTest(const FString& Parameters)
{
	const FString Input =
		TEXT("First, let me read the blueprint.\n")
		TEXT("<tool_call id=\"tc_1\">{\"name\":\"blueprint.read\",\"arguments\":{\"path\":\"/Game/BP_A\"}}</tool_call>\n")
		TEXT("Now I will add a variable.\n")
		TEXT("<tool_call id=\"tc_2\">{\"name\":\"blueprint.add_variable\",\"arguments\":{\"path\":\"/Game/BP_A\",\"name\":\"Health\"}}</tool_call>\n")
		TEXT("Finally, let me compile.\n")
		TEXT("<tool_call id=\"tc_3\">{\"name\":\"blueprint.compile\",\"arguments\":{\"path\":\"/Game/BP_A\"}}</tool_call>\n")
		TEXT("All done.");

	TArray<FOliveStreamChunk> ToolCalls;
	FString CleanText;

	const bool bResult = FOliveCLIToolCallParser::Parse(Input, ToolCalls, CleanText);

	TestTrue(TEXT("Parse should return true"), bResult);
	TestEqual(TEXT("Should extract exactly 3 tool calls"), ToolCalls.Num(), 3);

	if (ToolCalls.Num() == 3)
	{
		// Verify order and names
		TestEqual(TEXT("First tool call name"), ToolCalls[0].ToolName, TEXT("blueprint.read"));
		TestEqual(TEXT("First tool call id"), ToolCalls[0].ToolCallId, TEXT("tc_1"));

		TestEqual(TEXT("Second tool call name"), ToolCalls[1].ToolName, TEXT("blueprint.add_variable"));
		TestEqual(TEXT("Second tool call id"), ToolCalls[1].ToolCallId, TEXT("tc_2"));

		TestEqual(TEXT("Third tool call name"), ToolCalls[2].ToolName, TEXT("blueprint.compile"));
		TestEqual(TEXT("Third tool call id"), ToolCalls[2].ToolCallId, TEXT("tc_3"));
	}

	// Verify interstitial text is preserved
	TestTrue(TEXT("Clean text should contain first sentence"), CleanText.Contains(TEXT("First, let me read")));
	TestTrue(TEXT("Clean text should contain second sentence"), CleanText.Contains(TEXT("Now I will add")));
	TestTrue(TEXT("Clean text should contain third sentence"), CleanText.Contains(TEXT("Finally, let me compile")));
	TestTrue(TEXT("Clean text should contain final sentence"), CleanText.Contains(TEXT("All done.")));
	TestFalse(TEXT("Clean text should NOT contain any tool_call tags"), CleanText.Contains(TEXT("<tool_call")));

	return true;
}

// ----------------------------------------------------------------------------
// Test 3: No tool calls in the response
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCLIParserNoToolCallsTest,
	"OliveAI.CLIParser.NoToolCalls",
	OliveCLIToolTests::TestFlags)

bool FOliveCLIParserNoToolCallsTest::RunTest(const FString& Parameters)
{
	const FString Input = TEXT("This is just plain text with no tool calls at all. Nothing to parse here.");

	TArray<FOliveStreamChunk> ToolCalls;
	FString CleanText;

	const bool bResult = FOliveCLIToolCallParser::Parse(Input, ToolCalls, CleanText);

	TestFalse(TEXT("Parse should return false when no tool calls found"), bResult);
	TestEqual(TEXT("OutToolCalls should be empty"), ToolCalls.Num(), 0);
	TestEqual(TEXT("Clean text should equal the original input"), CleanText, Input);

	return true;
}

// ----------------------------------------------------------------------------
// Test 4: Malformed JSON inside a tool_call block
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCLIParserMalformedJSONTest,
	"OliveAI.CLIParser.MalformedJSON",
	OliveCLIToolTests::TestFlags)

bool FOliveCLIParserMalformedJSONTest::RunTest(const FString& Parameters)
{
	const FString Input =
		TEXT("Attempting a call.\n")
		TEXT("<tool_call id=\"tc_1\">NOT VALID JSON</tool_call>\n")
		TEXT("Done.");

	TArray<FOliveStreamChunk> ToolCalls;
	FString CleanText;

	const bool bResult = FOliveCLIToolCallParser::Parse(Input, ToolCalls, CleanText);

	TestFalse(TEXT("Parse should return false when JSON is malformed"), bResult);
	TestEqual(TEXT("OutToolCalls should be empty (malformed call skipped)"), ToolCalls.Num(), 0);

	// The parser includes raw malformed blocks in clean text so they are not silently lost
	TestTrue(TEXT("Clean text should contain the surrounding prose"), CleanText.Contains(TEXT("Attempting a call")));
	TestTrue(TEXT("Clean text should contain trailing text"), CleanText.Contains(TEXT("Done.")));

	return true;
}

// ----------------------------------------------------------------------------
// Test 5: Deeply nested JSON arguments
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCLIParserNestedJSONTest,
	"OliveAI.CLIParser.NestedJSON",
	OliveCLIToolTests::TestFlags)

bool FOliveCLIParserNestedJSONTest::RunTest(const FString& Parameters)
{
	const FString Input =
		TEXT("<tool_call id=\"tc_1\">")
		TEXT("{\"name\":\"blueprint.apply_plan_json\",\"arguments\":{\"path\":\"/Game/BP\",\"plan\":{\"schema_version\":\"2.0\",\"steps\":[{\"step_id\":\"s1\",\"op\":\"event\",\"target\":\"BeginPlay\"}]}}}")
		TEXT("</tool_call>");

	TArray<FOliveStreamChunk> ToolCalls;
	FString CleanText;

	const bool bResult = FOliveCLIToolCallParser::Parse(Input, ToolCalls, CleanText);

	TestTrue(TEXT("Parse should return true for nested JSON"), bResult);
	TestEqual(TEXT("Should extract exactly one tool call"), ToolCalls.Num(), 1);

	if (ToolCalls.Num() == 1)
	{
		const FOliveStreamChunk& Call = ToolCalls[0];
		TestEqual(TEXT("Tool name should be 'blueprint.apply_plan_json'"), Call.ToolName, TEXT("blueprint.apply_plan_json"));
		TestTrue(TEXT("Tool arguments should be valid"), Call.ToolArguments.IsValid());

		if (Call.ToolArguments.IsValid())
		{
			// Verify the nested "plan" object exists
			const TSharedPtr<FJsonObject>* PlanPtr = nullptr;
			TestTrue(TEXT("Arguments should have 'plan' object"), Call.ToolArguments->TryGetObjectField(TEXT("plan"), PlanPtr));

			if (PlanPtr && PlanPtr->IsValid())
			{
				const TSharedPtr<FJsonObject>& Plan = *PlanPtr;

				FString SchemaVersion;
				TestTrue(TEXT("Plan should have 'schema_version'"), Plan->TryGetStringField(TEXT("schema_version"), SchemaVersion));
				TestEqual(TEXT("Schema version should be '2.0'"), SchemaVersion, TEXT("2.0"));

				// Verify nested "steps" array
				const TArray<TSharedPtr<FJsonValue>>* StepsArray = nullptr;
				TestTrue(TEXT("Plan should have 'steps' array"), Plan->TryGetArrayField(TEXT("steps"), StepsArray));

				if (StepsArray)
				{
					TestEqual(TEXT("Steps array should have 1 element"), StepsArray->Num(), 1);

					if (StepsArray->Num() == 1)
					{
						const TSharedPtr<FJsonObject>& Step = (*StepsArray)[0]->AsObject();
						TestTrue(TEXT("Step should be a valid object"), Step.IsValid());

						if (Step.IsValid())
						{
							FString StepId;
							Step->TryGetStringField(TEXT("step_id"), StepId);
							TestEqual(TEXT("Step ID should be 's1'"), StepId, TEXT("s1"));

							FString Op;
							Step->TryGetStringField(TEXT("op"), Op);
							TestEqual(TEXT("Step op should be 'event'"), Op, TEXT("event"));
						}
					}
				}
			}
		}
	}

	return true;
}

// ----------------------------------------------------------------------------
// Test 6: Missing id attribute (should auto-generate)
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCLIParserMissingIdTest,
	"OliveAI.CLIParser.MissingId",
	OliveCLIToolTests::TestFlags)

bool FOliveCLIParserMissingIdTest::RunTest(const FString& Parameters)
{
	const FString Input =
		TEXT("<tool_call>{\"name\":\"test.tool\",\"arguments\":{}}</tool_call>");

	TArray<FOliveStreamChunk> ToolCalls;
	FString CleanText;

	const bool bResult = FOliveCLIToolCallParser::Parse(Input, ToolCalls, CleanText);

	TestTrue(TEXT("Parse should return true even without id attribute"), bResult);
	TestEqual(TEXT("Should extract exactly one tool call"), ToolCalls.Num(), 1);

	if (ToolCalls.Num() == 1)
	{
		const FOliveStreamChunk& Call = ToolCalls[0];
		TestEqual(TEXT("Tool name should be 'test.tool'"), Call.ToolName, TEXT("test.tool"));
		TestFalse(TEXT("Auto-generated ToolCallId should not be empty"), Call.ToolCallId.IsEmpty());
		TestTrue(TEXT("Auto-generated ToolCallId should start with 'tc_'"), Call.ToolCallId.StartsWith(TEXT("tc_")));
	}

	return true;
}

// ============================================================================
// Serializer Tests
// ============================================================================

// ----------------------------------------------------------------------------
// Test 7: Basic serialization with required and optional params
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCLISerializerBasicTest,
	"OliveAI.CLISerializer.BasicSerialization",
	OliveCLIToolTests::TestFlags)

bool FOliveCLISerializerBasicTest::RunTest(const FString& Parameters)
{
	using namespace OliveCLIToolTests;

	TMap<FString, FString> Params;
	Params.Add(TEXT("path"), TEXT("string"));
	Params.Add(TEXT("type"), TEXT("string"));

	TArray<FString> Required;
	Required.Add(TEXT("path"));

	FOliveToolDefinition Tool = MakeTestTool(
		TEXT("test.create"),
		TEXT("Test tool for creating things"),
		TEXT("test"),
		Params,
		Required);

	TArray<FOliveToolDefinition> Tools;
	Tools.Add(Tool);

	const FString Output = FOliveCLIToolSchemaSerializer::Serialize(Tools);

	TestTrue(TEXT("Output should contain tool name"), Output.Contains(TEXT("test.create")));
	TestTrue(TEXT("Output should contain [required] annotation"), Output.Contains(TEXT("[required]")));
	TestTrue(TEXT("Output should contain tool description"), Output.Contains(TEXT("Test tool for creating things")));
	TestTrue(TEXT("Output should contain category header"), Output.Contains(TEXT("### test")));
	TestTrue(TEXT("Output should contain 'path' parameter"), Output.Contains(TEXT("path")));
	TestTrue(TEXT("Output should contain 'type' parameter"), Output.Contains(TEXT("type")));
	TestFalse(TEXT("Output should not be empty"), Output.IsEmpty());

	return true;
}

// ----------------------------------------------------------------------------
// Test 8: Category grouping with multiple tools
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCLISerializerCategoryGroupingTest,
	"OliveAI.CLISerializer.CategoryGrouping",
	OliveCLIToolTests::TestFlags)

bool FOliveCLISerializerCategoryGroupingTest::RunTest(const FString& Parameters)
{
	using namespace OliveCLIToolTests;

	TMap<FString, FString> EmptyParams;
	TArray<FString> NoRequired;

	TArray<FOliveToolDefinition> Tools;
	Tools.Add(MakeTestTool(TEXT("alpha.read"), TEXT("Read alpha"), TEXT("alpha"), EmptyParams, NoRequired));
	Tools.Add(MakeTestTool(TEXT("alpha.write"), TEXT("Write alpha"), TEXT("alpha"), EmptyParams, NoRequired));
	Tools.Add(MakeTestTool(TEXT("beta.scan"), TEXT("Scan beta"), TEXT("beta"), EmptyParams, NoRequired));

	const FString Output = FOliveCLIToolSchemaSerializer::Serialize(Tools);

	TestTrue(TEXT("Output should contain alpha category header"), Output.Contains(TEXT("### alpha")));
	TestTrue(TEXT("Output should contain beta category header"), Output.Contains(TEXT("### beta")));
	TestTrue(TEXT("Output should contain alpha.read tool"), Output.Contains(TEXT("alpha.read")));
	TestTrue(TEXT("Output should contain alpha.write tool"), Output.Contains(TEXT("alpha.write")));
	TestTrue(TEXT("Output should contain beta.scan tool"), Output.Contains(TEXT("beta.scan")));

	// Verify categories appear in sorted order (alpha before beta)
	const int32 AlphaPos = Output.Find(TEXT("### alpha"));
	const int32 BetaPos = Output.Find(TEXT("### beta"));
	TestTrue(TEXT("Alpha category should appear before beta (sorted)"), AlphaPos < BetaPos);

	return true;
}

// ----------------------------------------------------------------------------
// Test 9: Compact mode omits descriptions
// ----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCLISerializerCompactModeTest,
	"OliveAI.CLISerializer.CompactMode",
	OliveCLIToolTests::TestFlags)

bool FOliveCLISerializerCompactModeTest::RunTest(const FString& Parameters)
{
	using namespace OliveCLIToolTests;

	TMap<FString, FString> Params;
	Params.Add(TEXT("path"), TEXT("string"));

	TArray<FString> Required;
	Required.Add(TEXT("path"));

	FOliveToolDefinition Tool = MakeTestTool(
		TEXT("test.create"),
		TEXT("This description should be hidden in compact mode"),
		TEXT("test"),
		Params,
		Required);

	TArray<FOliveToolDefinition> Tools;
	Tools.Add(Tool);

	// Serialize in compact mode
	const FString CompactOutput = FOliveCLIToolSchemaSerializer::Serialize(Tools, true);

	TestTrue(TEXT("Compact output should contain tool name"), CompactOutput.Contains(TEXT("test.create")));
	TestFalse(TEXT("Compact output should NOT contain description"), CompactOutput.Contains(TEXT("This description should be hidden")));

	// Verify non-compact mode does include description (sanity check)
	const FString FullOutput = FOliveCLIToolSchemaSerializer::Serialize(Tools, false);
	TestTrue(TEXT("Full output should contain description"), FullOutput.Contains(TEXT("This description should be hidden")));

	return true;
}
