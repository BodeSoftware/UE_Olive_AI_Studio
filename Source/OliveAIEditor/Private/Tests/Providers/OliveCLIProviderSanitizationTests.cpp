// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Providers/OliveClaudeCodeProvider.h"
#include "Providers/OliveCodexProvider.h"

namespace OliveCLIProviderSanitizationTests
{
	static constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	class FClaudeHarness : public FOliveClaudeCodeProvider
	{
	public:
		using FOliveClaudeCodeProvider::ParseOutputLine;

		FString GetAccumulatedResponse() const
		{
			return AccumulatedResponse;
		}
	};

	class FCodexHarness : public FOliveCodexProvider
	{
	public:
		using FOliveCodexProvider::ParseOutputLine;

		FString GetAccumulatedResponse() const
		{
			return AccumulatedResponse;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveClaudeProviderFiltersWorkerQuitDiagnosticTest,
	"OliveAI.CLIProvider.ClaudeFiltersWorkerQuitDiagnostic",
	OliveCLIProviderSanitizationTests::TestFlags)

bool FOliveClaudeProviderFiltersWorkerQuitDiagnosticTest::RunTest(const FString& Parameters)
{
	using namespace OliveCLIProviderSanitizationTests;

	FClaudeHarness Provider;
	Provider.ParseOutputLine(TEXT("worker quit with fatal"));
	Provider.ParseOutputLine(TEXT("{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}}"));

	TestEqual(TEXT("Claude parser should ignore worker quit diagnostic"), Provider.GetAccumulatedResponse(), TEXT("hello"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveCodexProviderFiltersWorkerQuitDiagnosticTest,
	"OliveAI.CLIProvider.CodexFiltersWorkerQuitDiagnostic",
	OliveCLIProviderSanitizationTests::TestFlags)

bool FOliveCodexProviderFiltersWorkerQuitDiagnosticTest::RunTest(const FString& Parameters)
{
	using namespace OliveCLIProviderSanitizationTests;

	FCodexHarness Provider;
	Provider.ParseOutputLine(TEXT("worker quit with fatal"));
	Provider.ParseOutputLine(TEXT("{\"type\":\"item.completed\",\"item\":{\"id\":\"item_3\",\"type\":\"agent_message\",\"text\":\"hello\"}}"));

	TestEqual(TEXT("Codex parser should ignore worker quit diagnostic"), Provider.GetAccumulatedResponse(), TEXT("hello"));
	return true;
}
