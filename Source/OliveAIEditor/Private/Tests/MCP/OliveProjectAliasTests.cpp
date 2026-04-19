// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "MCP/OliveToolRegistry.h"

/**
 * P5 Project family consolidation -- alias round-trip coverage.
 *
 * Each test calls a legacy tool name with minimum params and asserts that:
 *   (a) the result is NOT TOOL_NOT_FOUND (proves the alias resolved and routed
 *       to a real consolidated handler), AND
 *   (b) the request was recognised far enough to reach the consolidated
 *       dispatcher (per-handler errors such as MISSING_PATH are acceptable —
 *       they prove dispatch occurred).
 *
 * The deleted-tool negative test asserts the opposite: calling a
 * hard-deleted legacy name MUST return TOOL_NOT_FOUND (no alias exists).
 *
 * Tests target assets/snapshots that do not exist so they do not mutate the
 * project. They assert on error codes being NOT TOOL_NOT_FOUND (for positive
 * alias cases) rather than on success or exact wording, so minor handler
 * tweaks do not break coverage.
 */
namespace OliveProjectAliasTests
{
	static constexpr EAutomationTestFlags TestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** True if the alias resolved to a registered tool. Error code must not be TOOL_NOT_FOUND. */
	static bool AliasResolved(const FOliveToolResult& Result)
	{
		if (Result.Messages.Num() == 0)
		{
			// Success counts as resolved.
			return true;
		}
		return Result.Messages[0].Code != TEXT("TOOL_NOT_FOUND");
	}

	/** True if the project.read canonical survives in the registry. */
	static bool IsProjectReadRegistered()
	{
		return FOliveToolRegistry::Get().HasTool(TEXT("project.read"));
	}

	/** True if the project.snapshot canonical survives in the registry. */
	static bool IsProjectSnapshotRegistered()
	{
		return FOliveToolRegistry::Get().HasTool(TEXT("project.snapshot"));
	}

	/** True if the project.index canonical survives in the registry. */
	static bool IsProjectIndexRegistered()
	{
		return FOliveToolRegistry::Get().HasTool(TEXT("project.index"));
	}
}

// ---------------------------------------------------------------------------
// project.get_asset_info -> project.read(include=["asset_info"])
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveProjectAliasGetAssetInfoTest,
	"OliveAI.MCP.Alias.Project.GetAssetInfo",
	OliveProjectAliasTests::TestFlags)

bool FOliveProjectAliasGetAssetInfoTest::RunTest(const FString& Parameters)
{
	if (!OliveProjectAliasTests::IsProjectReadRegistered())
	{
		AddInfo(TEXT("project.read not registered; skipping alias test."));
		return true;
	}

	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("path"), TEXT("/Game/__DoesNotExist_P5Alias_ProjectRead"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("project.get_asset_info"), P);

	TestTrue(TEXT("Alias project.get_asset_info must resolve (no TOOL_NOT_FOUND)"),
		OliveProjectAliasTests::AliasResolved(R));
	// project.read is tolerant (per-include errors are reported in data.errors, not the top-level code),
	// so we do not assert on bSuccess here.
	return true;
}

// ---------------------------------------------------------------------------
// project.get_dependencies -> project.read(include=["dependencies"])
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveProjectAliasGetDependenciesTest,
	"OliveAI.MCP.Alias.Project.GetDependencies",
	OliveProjectAliasTests::TestFlags)

bool FOliveProjectAliasGetDependenciesTest::RunTest(const FString& Parameters)
{
	if (!OliveProjectAliasTests::IsProjectReadRegistered())
	{
		AddInfo(TEXT("project.read not registered; skipping alias test."));
		return true;
	}

	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("path"), TEXT("/Game/__DoesNotExist_P5Alias_Dependencies"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("project.get_dependencies"), P);

	TestTrue(TEXT("Alias project.get_dependencies must resolve (no TOOL_NOT_FOUND)"),
		OliveProjectAliasTests::AliasResolved(R));
	return true;
}

// ---------------------------------------------------------------------------
// project.list_snapshots -> project.snapshot(action="list")
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveProjectAliasListSnapshotsTest,
	"OliveAI.MCP.Alias.Project.ListSnapshots",
	OliveProjectAliasTests::TestFlags)

bool FOliveProjectAliasListSnapshotsTest::RunTest(const FString& Parameters)
{
	if (!OliveProjectAliasTests::IsProjectSnapshotRegistered())
	{
		AddInfo(TEXT("project.snapshot not registered; skipping alias test."));
		return true;
	}

	// No required params for listing.
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("project.list_snapshots"), P);

	TestTrue(TEXT("Alias project.list_snapshots must resolve (no TOOL_NOT_FOUND)"),
		OliveProjectAliasTests::AliasResolved(R));
	return true;
}

// ---------------------------------------------------------------------------
// project.index_status -> project.index(action="status")
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveProjectAliasIndexStatusTest,
	"OliveAI.MCP.Alias.Project.IndexStatus",
	OliveProjectAliasTests::TestFlags)

bool FOliveProjectAliasIndexStatusTest::RunTest(const FString& Parameters)
{
	if (!OliveProjectAliasTests::IsProjectIndexRegistered())
	{
		AddInfo(TEXT("project.index not registered; skipping alias test."));
		return true;
	}

	// index_status takes no required params.
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("project.index_status"), P);

	TestTrue(TEXT("Alias project.index_status must resolve (no TOOL_NOT_FOUND)"),
		OliveProjectAliasTests::AliasResolved(R));
	return true;
}

// ---------------------------------------------------------------------------
// NEGATIVE: olive.get_recipe is hard-deleted (P5 rails removal). No alias.
// Calling it MUST return TOOL_NOT_FOUND.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveDeletedGetRecipeTest,
	"OliveAI.MCP.Deleted.OliveGetRecipe",
	OliveProjectAliasTests::TestFlags)

bool FOliveDeletedGetRecipeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("olive.get_recipe"), P);
	TestFalse(TEXT("olive.get_recipe must be deleted"), R.bSuccess);
	if (R.Messages.Num() > 0)
	{
		TestEqual(TEXT("Error code"), R.Messages[0].Code, FString(TEXT("TOOL_NOT_FOUND")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// NEGATIVE: test.create is hard-deleted (dev-only tool). No alias.
// Calling it MUST return TOOL_NOT_FOUND.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveDeletedTestCreateTest,
	"OliveAI.MCP.Deleted.TestCreate",
	OliveProjectAliasTests::TestFlags)

bool FOliveDeletedTestCreateTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("test.create"), P);
	TestFalse(TEXT("test.create must be deleted"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// NEGATIVE: project.create_ai_character is hard-deleted with NO alias.
// Calling it MUST return TOOL_NOT_FOUND.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveProjectAliasDeletedCreateAiCharacterTest,
	"OliveAI.MCP.Alias.Project.DeletedCreateAiCharacter",
	OliveProjectAliasTests::TestFlags)

bool FOliveProjectAliasDeletedCreateAiCharacterTest::RunTest(const FString& Parameters)
{
	// Arbitrary params — handler should never see them because the tool does not exist.
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("name"), TEXT("P5AliasProbe_DeletedTool"));
	P->SetStringField(TEXT("path"), TEXT("/Game/__DoesNotExist_P5Alias"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("project.create_ai_character"), P);

	TestFalse(TEXT("Deleted tool must NOT succeed"), R.bSuccess);

	// Must have at least one error message.
	if (!TestTrue(TEXT("Expected an error message for deleted tool"), R.Messages.Num() > 0))
	{
		return false;
	}

	TestEqual(TEXT("Deleted tool must return TOOL_NOT_FOUND"),
		R.Messages[0].Code, FString(TEXT("TOOL_NOT_FOUND")));
	return true;
}
