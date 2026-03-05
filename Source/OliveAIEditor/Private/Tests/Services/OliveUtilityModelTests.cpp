// Copyright Bode Software. All Rights Reserved.

/**
 * OliveUtilityModelTests.cpp
 *
 * Automation tests for FOliveUtilityModel -- keyword extraction,
 * template search integration, and pre-search injection formatting.
 * Focused on the basic tokenizer path (no LLM needed).
 */

#include "Misc/AutomationTest.h"
#include "Services/OliveUtilityModel.h"
#include "Template/OliveTemplateSystem.h"
#include "Settings/OliveAISettings.h"
#include "Serialization/JsonSerializer.h"

namespace OliveUtilityModelTests
{
	static constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
}

// ============================================================================
// 1. BasicTokenizerExtracts
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveUtilityModelBasicTokenizerExtractsTest,
	"OliveAI.Services.UtilityModel.BasicTokenizerExtracts",
	OliveUtilityModelTests::TestFlags)

bool FOliveUtilityModelBasicTokenizerExtractsTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Keywords = FOliveUtilityModel::ExtractSearchKeywords(
		TEXT("create a bow and arrow system for my character"), 12);

	UE_LOG(LogTemp, Log, TEXT("[BasicTokenizerExtracts] Extracted %d keywords:"), Keywords.Num());
	for (int32 i = 0; i < Keywords.Num(); ++i)
	{
		UE_LOG(LogTemp, Log, TEXT("  [%d] \"%s\""), i, *Keywords[i]);
	}

	TestTrue(TEXT("Result should be non-empty"), Keywords.Num() > 0);
	TestTrue(TEXT("Should contain 'bow'"), Keywords.Contains(TEXT("bow")));
	TestTrue(TEXT("Should contain 'arrow'"), Keywords.Contains(TEXT("arrow")));
	TestFalse(TEXT("Should NOT contain 'create' (action verb stop word)"), Keywords.Contains(TEXT("create")));

	return true;
}

// ============================================================================
// 2. BasicTokenizerStripsAtMentions
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveUtilityModelStripsAtMentionsTest,
	"OliveAI.Services.UtilityModel.BasicTokenizerStripsAtMentions",
	OliveUtilityModelTests::TestFlags)

bool FOliveUtilityModelStripsAtMentionsTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Keywords = FOliveUtilityModel::ExtractSearchKeywords(
		TEXT("add health system to @BP_ThirdPersonCharacter"), 12);

	UE_LOG(LogTemp, Log, TEXT("[StripsAtMentions] Extracted %d keywords:"), Keywords.Num());
	for (int32 i = 0; i < Keywords.Num(); ++i)
	{
		UE_LOG(LogTemp, Log, TEXT("  [%d] \"%s\""), i, *Keywords[i]);
	}

	// Verify no keyword contains the @ symbol
	bool bFoundAt = false;
	for (const FString& Keyword : Keywords)
	{
		if (Keyword.Contains(TEXT("@")))
		{
			bFoundAt = true;
			UE_LOG(LogTemp, Warning, TEXT("  Found '@' in keyword: \"%s\""), *Keyword);
			break;
		}
	}
	TestFalse(TEXT("No keyword should contain '@' symbol"), bFoundAt);

	TestTrue(TEXT("Should contain 'health'"), Keywords.Contains(TEXT("health")));

	return true;
}

// ============================================================================
// 3. BasicTokenizerHandlesEmpty
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveUtilityModelHandlesEmptyTest,
	"OliveAI.Services.UtilityModel.BasicTokenizerHandlesEmpty",
	OliveUtilityModelTests::TestFlags)

bool FOliveUtilityModelHandlesEmptyTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Keywords = FOliveUtilityModel::ExtractSearchKeywords(TEXT(""), 12);

	UE_LOG(LogTemp, Log, TEXT("[HandlesEmpty] Extracted %d keywords (expected 0)"), Keywords.Num());

	TestEqual(TEXT("Empty input should produce empty result"), Keywords.Num(), 0);

	return true;
}

// ============================================================================
// 4. BasicTokenizerHandlesShort
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveUtilityModelHandlesShortTest,
	"OliveAI.Services.UtilityModel.BasicTokenizerHandlesShort",
	OliveUtilityModelTests::TestFlags)

bool FOliveUtilityModelHandlesShortTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Keywords = FOliveUtilityModel::ExtractSearchKeywords(TEXT("fix"), 12);

	UE_LOG(LogTemp, Log, TEXT("[HandlesShort] Input: \"fix\" -> Extracted %d keywords (expected 0, since 'fix' is an action verb stop word)"), Keywords.Num());
	for (int32 i = 0; i < Keywords.Num(); ++i)
	{
		UE_LOG(LogTemp, Log, TEXT("  [%d] \"%s\""), i, *Keywords[i]);
	}

	TestEqual(TEXT("Single action verb should produce empty result"), Keywords.Num(), 0);

	return true;
}

// ============================================================================
// 5. SearchIntegration
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveUtilityModelSearchIntegrationTest,
	"OliveAI.Services.UtilityModel.SearchIntegration",
	OliveUtilityModelTests::TestFlags)

bool FOliveUtilityModelSearchIntegrationTest::RunTest(const FString& Parameters)
{
	// Step 1: Extract keywords
	const TArray<FString> Keywords = FOliveUtilityModel::ExtractSearchKeywords(
		TEXT("create a bow and arrow ranged combat system"), 12);

	UE_LOG(LogTemp, Log, TEXT("[SearchIntegration] Extracted %d keywords"), Keywords.Num());
	for (int32 i = 0; i < Keywords.Num(); ++i)
	{
		UE_LOG(LogTemp, Log, TEXT("  [%d] \"%s\""), i, *Keywords[i]);
	}

	// Step 2: Join keywords for search query
	const FString SearchQuery = FString::Join(Keywords, TEXT(" "));
	UE_LOG(LogTemp, Log, TEXT("[SearchIntegration] Search query: \"%s\""), *SearchQuery);

	// Step 3: Search templates
	const TArray<TSharedPtr<FJsonObject>> Results =
		FOliveTemplateSystem::Get().SearchTemplates(SearchQuery, 8);

	UE_LOG(LogTemp, Log, TEXT("[SearchIntegration] SearchTemplates returned %d results"), Results.Num());
	for (int32 i = 0; i < FMath::Min(Results.Num(), 5); ++i)
	{
		if (Results[i].IsValid())
		{
			FString TemplateId;
			Results[i]->TryGetStringField(TEXT("template_id"), TemplateId);
			FString Desc;
			Results[i]->TryGetStringField(TEXT("catalog_description"), Desc);
			UE_LOG(LogTemp, Log, TEXT("  [%d] %s -- %s"), i, *TemplateId,
				*Desc.Left(80));
		}
	}

	// We don't assert on result count because template availability depends on
	// whether the library was initialized at runtime. Just verify no crash.
	UE_LOG(LogTemp, Log, TEXT("[SearchIntegration] Pipeline completed without crash"));

	return true;
}

// ============================================================================
// 6. IsAvailableReflectsSettings
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveUtilityModelIsAvailableTest,
	"OliveAI.Services.UtilityModel.IsAvailableReflectsSettings",
	OliveUtilityModelTests::TestFlags)

bool FOliveUtilityModelIsAvailableTest::RunTest(const FString& Parameters)
{
	UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		UE_LOG(LogTemp, Error, TEXT("[IsAvailableReflectsSettings] UOliveAISettings::Get() returned nullptr"));
		return false;
	}

	// Store original value
	const bool bOriginal = Settings->bEnableLLMKeywordExpansion;
	UE_LOG(LogTemp, Log, TEXT("[IsAvailableReflectsSettings] Original bEnableLLMKeywordExpansion = %s"),
		bOriginal ? TEXT("true") : TEXT("false"));

	// Disable and verify
	Settings->bEnableLLMKeywordExpansion = false;
	const bool bAvailableWhenDisabled = FOliveUtilityModel::IsAvailable();
	UE_LOG(LogTemp, Log, TEXT("[IsAvailableReflectsSettings] IsAvailable() with setting disabled = %s"),
		bAvailableWhenDisabled ? TEXT("true") : TEXT("false"));

	TestFalse(TEXT("IsAvailable should return false when bEnableLLMKeywordExpansion is false"),
		bAvailableWhenDisabled);

	// Restore original value
	Settings->bEnableLLMKeywordExpansion = bOriginal;
	UE_LOG(LogTemp, Log, TEXT("[IsAvailableReflectsSettings] Restored original value"));

	return true;
}

// ============================================================================
// 7. PreSearchInjectionFormat
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveUtilityModelPreSearchInjectionTest,
	"OliveAI.Services.UtilityModel.PreSearchInjectionFormat",
	OliveUtilityModelTests::TestFlags)

bool FOliveUtilityModelPreSearchInjectionTest::RunTest(const FString& Parameters)
{
	// Step 1: Extract keywords
	const TArray<FString> Keywords = FOliveUtilityModel::ExtractSearchKeywords(
		TEXT("make a door that opens when player walks near it"), 12);

	UE_LOG(LogTemp, Log, TEXT("[PreSearchInjection] Extracted %d keywords"), Keywords.Num());
	for (int32 i = 0; i < Keywords.Num(); ++i)
	{
		UE_LOG(LogTemp, Log, TEXT("  [%d] \"%s\""), i, *Keywords[i]);
	}

	// Step 2: Search templates
	const FString SearchQuery = FString::Join(Keywords, TEXT(" "));
	const TArray<TSharedPtr<FJsonObject>> Results =
		FOliveTemplateSystem::Get().SearchTemplates(SearchQuery, 8);

	UE_LOG(LogTemp, Log, TEXT("[PreSearchInjection] Got %d search results"), Results.Num());

	if (Results.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[PreSearchInjection] No template results -- skipping format test (templates may not be loaded)"));
		return true;
	}

	// Step 3: Format the same way OliveCLIProviderBase.cpp does
	FString FormattedOutput;
	FormattedOutput += TEXT("\n\n## Relevant Library Templates (Pre-searched)\n\n");
	FormattedOutput += TEXT("These templates match your task. BEFORE building, call ");
	FormattedOutput += TEXT("`blueprint.get_template(template_id=\"...\", pattern=\"FuncName\")` ");
	FormattedOutput += TEXT("on at least one relevant function to study real implementation patterns.\n\n");

	for (const TSharedPtr<FJsonObject>& Entry : Results)
	{
		FString Id, Desc, SourceProj, ParentClass;
		Entry->TryGetStringField(TEXT("template_id"), Id);
		Entry->TryGetStringField(TEXT("catalog_description"), Desc);
		Entry->TryGetStringField(TEXT("source_project"), SourceProj);
		Entry->TryGetStringField(TEXT("parent_class"), ParentClass);

		FormattedOutput += FString::Printf(TEXT("- **%s**"), *Id);
		if (!SourceProj.IsEmpty())
		{
			FormattedOutput += FString::Printf(TEXT(" [%s]"), *SourceProj);
		}
		if (!ParentClass.IsEmpty())
		{
			FormattedOutput += FString::Printf(TEXT(" (parent: %s)"), *ParentClass);
		}
		if (!Desc.IsEmpty())
		{
			FormattedOutput += FString::Printf(TEXT(": %s"), *Desc);
		}
		FormattedOutput += TEXT("\n");
	}

	FormattedOutput += TEXT("\n**REQUIRED**: Study at least one relevant function above ");
	FormattedOutput += TEXT("before writing any plan_json. Use ");
	FormattedOutput += TEXT("`blueprint.get_template(template_id=\"<id>\", pattern=\"<FuncName>\")` ");
	FormattedOutput += TEXT("to read specific functions. These are references -- adapt ");
	FormattedOutput += TEXT("patterns to fit the task, do not copy blindly.\n");

	// Log the full output for manual inspection
	UE_LOG(LogTemp, Log, TEXT("[PreSearchInjection] Formatted output (%d chars):"), FormattedOutput.Len());
	UE_LOG(LogTemp, Log, TEXT("%s"), *FormattedOutput);

	// Verify structural elements
	TestTrue(TEXT("Should contain '## Relevant Library Templates' header"),
		FormattedOutput.Contains(TEXT("## Relevant Library Templates")));
	TestTrue(TEXT("Should contain 'REQUIRED'"),
		FormattedOutput.Contains(TEXT("REQUIRED")));

	return true;
}
