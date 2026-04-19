// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "MCP/OliveToolRegistry.h"

/**
 * P5 C++ family consolidation -- alias round-trip coverage.
 *
 * Each test calls the legacy tool name with minimum params and asserts that:
 *   (a) the result is NOT TOOL_NOT_FOUND (proves the alias resolved and routed
 *       to a real consolidated handler), AND
 *   (b) the request was recognised far enough to reach the consolidated
 *       dispatcher (per-handler validation errors such as CLASS_NOT_FOUND are
 *       acceptable signals that dispatch worked).
 *
 * Tests intentionally target class/file names that do not exist so the tests
 * do not mutate the project. We assert on the error code being NOT
 * TOOL_NOT_FOUND rather than on success or exact wording, so minor handler
 * changes do not break coverage.
 */
namespace OliveCppAliasTests
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

	/** True if the C++ tool family appears registered (real canonical survives in registry). */
	static bool IsCppRegistered()
	{
		return FOliveToolRegistry::Get().HasTool(TEXT("cpp.read"));
	}
}

// ---------------------------------------------------------------------------
// cpp.read_class -> cpp.read(entity='class')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCppAliasReadClassTest,
	"OliveAI.MCP.Alias.Cpp.ReadClass",
	OliveCppAliasTests::TestFlags)

bool FOliveCppAliasReadClassTest::RunTest(const FString& Parameters)
{
	if (!OliveCppAliasTests::IsCppRegistered())
	{
		AddInfo(TEXT("cpp.read not registered; skipping alias test."));
		return true;
	}

	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("class_name"), TEXT("AClass_DoesNotExist_P5Alias"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("cpp.read_class"), P);

	TestTrue(TEXT("Alias cpp.read_class must resolve (no TOOL_NOT_FOUND)"),
		OliveCppAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target class does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// cpp.list_project_classes -> cpp.list(kind='project')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCppAliasListProjectClassesTest,
	"OliveAI.MCP.Alias.Cpp.ListProjectClasses",
	OliveCppAliasTests::TestFlags)

bool FOliveCppAliasListProjectClassesTest::RunTest(const FString& Parameters)
{
	if (!OliveCppAliasTests::IsCppRegistered())
	{
		AddInfo(TEXT("cpp.read not registered; skipping alias test."));
		return true;
	}

	// cpp.list_project_classes has no required fields; give a bogus module filter so
	// the underlying handler returns an empty list (still a resolved success).
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("module_filter"), TEXT("ModuleDoesNotExist_P5Alias"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("cpp.list_project_classes"), P);

	TestTrue(TEXT("Alias cpp.list_project_classes must resolve (no TOOL_NOT_FOUND)"),
		OliveCppAliasTests::AliasResolved(R));
	// The underlying handler returns success with an empty list when no classes match.
	// We accept either outcome as long as the alias itself routed.
	return true;
}

// ---------------------------------------------------------------------------
// cpp.add_function -> cpp.add(entity='function')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCppAliasAddFunctionTest,
	"OliveAI.MCP.Alias.Cpp.AddFunction",
	OliveCppAliasTests::TestFlags)

bool FOliveCppAliasAddFunctionTest::RunTest(const FString& Parameters)
{
	if (!OliveCppAliasTests::IsCppRegistered())
	{
		AddInfo(TEXT("cpp.read not registered; skipping alias test."));
		return true;
	}

	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("file_path"), TEXT("NoSuchModule/Public/DoesNotExist_P5Alias.h"));
	P->SetStringField(TEXT("function_name"), TEXT("P5AliasProbe"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("cpp.add_function"), P);

	TestTrue(TEXT("Alias cpp.add_function must resolve (no TOOL_NOT_FOUND)"),
		OliveCppAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target header does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// cpp.add_property -> cpp.add(entity='property')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCppAliasAddPropertyTest,
	"OliveAI.MCP.Alias.Cpp.AddProperty",
	OliveCppAliasTests::TestFlags)

bool FOliveCppAliasAddPropertyTest::RunTest(const FString& Parameters)
{
	if (!OliveCppAliasTests::IsCppRegistered())
	{
		AddInfo(TEXT("cpp.read not registered; skipping alias test."));
		return true;
	}

	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("file_path"), TEXT("NoSuchModule/Public/DoesNotExist_P5Alias.h"));
	P->SetStringField(TEXT("property_name"), TEXT("P5AliasProbe"));
	P->SetStringField(TEXT("property_type"), TEXT("int32"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("cpp.add_property"), P);

	TestTrue(TEXT("Alias cpp.add_property must resolve (no TOOL_NOT_FOUND)"),
		OliveCppAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target header does not exist"), R.bSuccess);
	return true;
}
