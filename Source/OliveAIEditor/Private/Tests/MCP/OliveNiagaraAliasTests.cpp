// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

#include "MCP/OliveToolRegistry.h"

/**
 * P5 Niagara family consolidation -- alias round-trip coverage.
 *
 * Each test calls the legacy tool name with minimum params and asserts that:
 *   (a) the result is NOT TOOL_NOT_FOUND (proves the alias resolved and routed
 *       to a real consolidated handler), AND
 *   (b) the request was recognised far enough to reach the consolidated
 *       dispatcher (per-handler validation errors like NIAGARA_SYSTEM_NOT_FOUND
 *       are OK).
 *
 * We intentionally target a Niagara system that does not exist so the tests do
 * not mutate the project content. We assert on the error code, not on the exact
 * error string, so minor wording changes do not break coverage.
 *
 * Niagara tool registration is guarded by FOliveNiagaraAvailability. On systems
 * without the Niagara plugin enabled, aliases will still resolve but the
 * underlying tools may be absent. In that case the aliased tool lookup yields
 * TOOL_NOT_FOUND and the test will be skipped via early return after logging.
 */
namespace OliveNiagaraAliasTests
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

	/** True if the Niagara tool family appears registered (real canonical survives in registry). */
	static bool IsNiagaraRegistered()
	{
		return FOliveToolRegistry::Get().HasTool(TEXT("niagara.read"));
	}

	static TSharedPtr<FJsonObject> MakeNiagaraParams(const FString& Path)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("path"), Path);
		return P;
	}
}

// ---------------------------------------------------------------------------
// niagara.add_emitter -> niagara.add(kind='emitter')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveNiagaraAliasAddEmitterTest,
	"OliveAI.MCP.Alias.Niagara.AddEmitter",
	OliveNiagaraAliasTests::TestFlags)

bool FOliveNiagaraAliasAddEmitterTest::RunTest(const FString& Parameters)
{
	if (!OliveNiagaraAliasTests::IsNiagaraRegistered())
	{
		AddInfo(TEXT("niagara.read not registered (Niagara plugin unavailable); skipping alias test."));
		return true;
	}

	TSharedPtr<FJsonObject> P = OliveNiagaraAliasTests::MakeNiagaraParams(
		TEXT("/Game/Tests/Niagara_DoesNotExist_P5Alias"));
	// No source_emitter / name — alias default add-empty path is fine.

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("niagara.add_emitter"), P);

	TestTrue(TEXT("Alias niagara.add_emitter must resolve (no TOOL_NOT_FOUND)"),
		OliveNiagaraAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Niagara system does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// niagara.add_module -> niagara.add(kind='module')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveNiagaraAliasAddModuleTest,
	"OliveAI.MCP.Alias.Niagara.AddModule",
	OliveNiagaraAliasTests::TestFlags)

bool FOliveNiagaraAliasAddModuleTest::RunTest(const FString& Parameters)
{
	if (!OliveNiagaraAliasTests::IsNiagaraRegistered())
	{
		AddInfo(TEXT("niagara.read not registered (Niagara plugin unavailable); skipping alias test."));
		return true;
	}

	TSharedPtr<FJsonObject> P = OliveNiagaraAliasTests::MakeNiagaraParams(
		TEXT("/Game/Tests/Niagara_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("emitter_id"), TEXT("emitter_0"));
	P->SetStringField(TEXT("stage"), TEXT("ParticleUpdate"));
	P->SetStringField(TEXT("module"), TEXT("Spawn Rate"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("niagara.add_module"), P);

	TestTrue(TEXT("Alias niagara.add_module must resolve (no TOOL_NOT_FOUND)"),
		OliveNiagaraAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Niagara system does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// niagara.set_parameter -> niagara.modify(entity='parameter')
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveNiagaraAliasSetParameterTest,
	"OliveAI.MCP.Alias.Niagara.SetParameter",
	OliveNiagaraAliasTests::TestFlags)

bool FOliveNiagaraAliasSetParameterTest::RunTest(const FString& Parameters)
{
	if (!OliveNiagaraAliasTests::IsNiagaraRegistered())
	{
		AddInfo(TEXT("niagara.read not registered (Niagara plugin unavailable); skipping alias test."));
		return true;
	}

	TSharedPtr<FJsonObject> P = OliveNiagaraAliasTests::MakeNiagaraParams(
		TEXT("/Game/Tests/Niagara_DoesNotExist_P5Alias"));
	P->SetStringField(TEXT("module_id"), TEXT("emitter_0.ParticleUpdate.module_2"));
	P->SetStringField(TEXT("parameter"), TEXT("SpawnRate"));
	P->SetStringField(TEXT("value"), TEXT("42.0"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("niagara.set_parameter"), P);

	TestTrue(TEXT("Alias niagara.set_parameter must resolve (no TOOL_NOT_FOUND)"),
		OliveNiagaraAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Niagara system does not exist"), R.bSuccess);
	return true;
}

// ---------------------------------------------------------------------------
// niagara.read_system -> niagara.read (pass-through)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveNiagaraAliasReadSystemTest,
	"OliveAI.MCP.Alias.Niagara.ReadSystem",
	OliveNiagaraAliasTests::TestFlags)

bool FOliveNiagaraAliasReadSystemTest::RunTest(const FString& Parameters)
{
	if (!OliveNiagaraAliasTests::IsNiagaraRegistered())
	{
		AddInfo(TEXT("niagara.read not registered (Niagara plugin unavailable); skipping alias test."));
		return true;
	}

	TSharedPtr<FJsonObject> P = OliveNiagaraAliasTests::MakeNiagaraParams(
		TEXT("/Game/Tests/Niagara_DoesNotExist_P5Alias"));

	FOliveToolResult R = FOliveToolRegistry::Get().ExecuteTool(TEXT("niagara.read_system"), P);

	TestTrue(TEXT("Alias niagara.read_system must resolve (no TOOL_NOT_FOUND)"),
		OliveNiagaraAliasTests::AliasResolved(R));
	TestFalse(TEXT("Should fail because target Niagara system does not exist"), R.bSuccess);
	return true;
}
