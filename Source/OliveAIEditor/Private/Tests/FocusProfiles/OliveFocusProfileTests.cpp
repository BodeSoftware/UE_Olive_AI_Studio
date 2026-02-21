// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Profiles/OliveFocusProfileManager.h"
#include "Settings/OliveAISettings.h"
#include "MCP/OliveToolRegistry.h"
#include "Chat/OlivePromptAssembler.h"

namespace OliveFocusProfileTests
{
	static constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FSettingsSnapshot
	{
		FString CustomJson;
		int32 SchemaVersion = 1;
		FString DefaultProfile;
	};

	static FSettingsSnapshot SaveSettings()
	{
		FSettingsSnapshot Snapshot;
		if (UOliveAISettings* Settings = UOliveAISettings::Get())
		{
			Snapshot.CustomJson = Settings->CustomFocusProfilesJson;
			Snapshot.SchemaVersion = Settings->CustomFocusProfilesSchemaVersion;
			Snapshot.DefaultProfile = Settings->DefaultFocusProfile;
		}
		return Snapshot;
	}

	static void RestoreSettings(const FSettingsSnapshot& Snapshot)
	{
		if (UOliveAISettings* Settings = UOliveAISettings::Get())
		{
			Settings->CustomFocusProfilesJson = Snapshot.CustomJson;
			Settings->CustomFocusProfilesSchemaVersion = Snapshot.SchemaVersion;
			Settings->DefaultFocusProfile = Snapshot.DefaultProfile;
			Settings->SaveConfig();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOliveFocusProfileValidationTest,
	"OliveAI.FocusProfiles.Validation",
	OliveFocusProfileTests::TestFlags)

bool FOliveFocusProfileValidationTest::RunTest(const FString& Parameters)
{
	FOliveFocusProfileManager& Manager = FOliveFocusProfileManager::Get();
	Manager.Initialize();

	TArray<FString> Errors;

	FOliveFocusProfile LegacyAliasProfile;
	LegacyAliasProfile.Name = TEXT("Full Stack");
	LegacyAliasProfile.DisplayName = FText::FromString(TEXT("Legacy"));
	TestFalse(TEXT("Legacy alias name should be rejected"), Manager.ValidateProfile(LegacyAliasProfile, Errors));
	TestTrue(TEXT("Legacy alias rejection should report an error"), Errors.Num() > 0);

	Errors.Reset();
	FOliveFocusProfile UnknownCategoryProfile;
	UnknownCategoryProfile.Name = TEXT("CustomUnknownCategory");
	UnknownCategoryProfile.DisplayName = FText::FromString(TEXT("Unknown Category"));
	UnknownCategoryProfile.ToolCategories = { TEXT("does_not_exist") };
	TestFalse(TEXT("Unknown categories should be rejected"), Manager.ValidateProfile(UnknownCategoryProfile, Errors));

	Errors.Reset();
	FOliveFocusProfile UnknownToolProfile;
	UnknownToolProfile.Name = TEXT("CustomUnknownTool");
	UnknownToolProfile.DisplayName = FText::FromString(TEXT("Unknown Tool"));
	UnknownToolProfile.ExcludedTools = { TEXT("tool.does_not_exist") };
	TestFalse(TEXT("Unknown excluded tools should be rejected"), Manager.ValidateProfile(UnknownToolProfile, Errors));

	Errors.Reset();
	FOliveFocusProfile ValidProfile;
	ValidProfile.Name = TEXT("CustomValidProfile");
	ValidProfile.DisplayName = FText::FromString(TEXT("Valid"));
	ValidProfile.ToolCategories = { TEXT("project") };
	ValidProfile.ExcludedTools = { TEXT("project.search") };
	TestTrue(TEXT("Known category/tool combination should validate"), Manager.ValidateProfile(ValidProfile, Errors));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOliveFocusProfilePersistenceTest,
	"OliveAI.FocusProfiles.PersistenceRoundtrip",
	OliveFocusProfileTests::TestFlags)

bool FOliveFocusProfilePersistenceTest::RunTest(const FString& Parameters)
{
	using namespace OliveFocusProfileTests;
	const FSettingsSnapshot Snapshot = SaveSettings();

	UOliveAISettings* Settings = UOliveAISettings::Get();
	TestNotNull(TEXT("Settings should exist"), Settings);
	if (!Settings)
	{
		return false;
	}

	Settings->CustomFocusProfilesJson.Empty();
	Settings->CustomFocusProfilesSchemaVersion = 1;
	Settings->SaveConfig();

	FOliveFocusProfileManager& Manager = FOliveFocusProfileManager::Get();
	Manager.Initialize();

	TArray<FString> Errors;
	FOliveFocusProfile Profile;
	Profile.Name = TEXT("CustomRoundtrip");
	Profile.DisplayName = FText::FromString(TEXT("Roundtrip"));
	Profile.Description = FText::FromString(TEXT("Roundtrip test profile"));
	Profile.ToolCategories = { TEXT("project") };
	Profile.ExcludedTools = { TEXT("project.search") };
	Profile.SystemPromptAddition = TEXT("Roundtrip prompt addition.");

	TestTrue(TEXT("Upsert should succeed"), Manager.UpsertCustomProfile(Profile, Errors));
	TestTrue(TEXT("Custom profile should exist after upsert"), Manager.HasProfile(TEXT("CustomRoundtrip")));

	Manager.Initialize();
	const TOptional<FOliveFocusProfile> Reloaded = Manager.GetProfile(TEXT("CustomRoundtrip"));
	TestTrue(TEXT("Custom profile should reload from persisted config"), Reloaded.IsSet());
	if (Reloaded.IsSet())
	{
		TestEqual(TEXT("Reloaded profile name should match"), Reloaded->Name, FString(TEXT("CustomRoundtrip")));
		TestEqual(TEXT("Reloaded prompt addition should match"), Reloaded->SystemPromptAddition, FString(TEXT("Roundtrip prompt addition.")));
	}

	RestoreSettings(Snapshot);
	Manager.Initialize();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOliveFocusProfileCorruptPayloadTest,
	"OliveAI.FocusProfiles.PersistenceCorruptAndUnsupported",
	OliveFocusProfileTests::TestFlags)

bool FOliveFocusProfileCorruptPayloadTest::RunTest(const FString& Parameters)
{
	using namespace OliveFocusProfileTests;
	const FSettingsSnapshot Snapshot = SaveSettings();

	UOliveAISettings* Settings = UOliveAISettings::Get();
	TestNotNull(TEXT("Settings should exist"), Settings);
	if (!Settings)
	{
		return false;
	}

	FOliveFocusProfileManager& Manager = FOliveFocusProfileManager::Get();

	Settings->CustomFocusProfilesJson = TEXT("{invalid json");
	Settings->CustomFocusProfilesSchemaVersion = 1;
	Settings->SaveConfig();
	Manager.Initialize();
	TestFalse(TEXT("Corrupt payload should not create unexpected profile"), Manager.HasProfile(TEXT("BrokenProfile")));
	TestTrue(TEXT("Built-in profile should still exist after corrupt payload"), Manager.HasProfile(TEXT("Everything")));

	Settings->CustomFocusProfilesJson = TEXT("{\"version\":999,\"profiles\":[{\"name\":\"FutureProfile\",\"display_name\":\"Future\",\"description\":\"\",\"tool_categories\":[],\"excluded_tools\":[],\"system_prompt_addition\":\"\",\"icon_name\":\"\",\"sort_order\":100}]}");
	Settings->CustomFocusProfilesSchemaVersion = 999;
	Settings->SaveConfig();
	Manager.Initialize();
	TestFalse(TEXT("Unsupported schema should be ignored"), Manager.HasProfile(TEXT("FutureProfile")));

	RestoreSettings(Snapshot);
	Manager.Initialize();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOliveFocusProfileNamingAndPromptTest,
	"OliveAI.FocusProfiles.NamingAndPrompt",
	OliveFocusProfileTests::TestFlags)

bool FOliveFocusProfileNamingAndPromptTest::RunTest(const FString& Parameters)
{
	using namespace OliveFocusProfileTests;
	const FSettingsSnapshot Snapshot = SaveSettings();

	FOliveFocusProfileManager& Manager = FOliveFocusProfileManager::Get();
	Manager.Initialize();

	TestEqual(TEXT("Legacy name should normalize to canonical"), Manager.NormalizeProfileName(TEXT("Full Stack")), FString(TEXT("Everything")));

	const TArray<FOliveToolDefinition> LegacyTools = FOliveToolRegistry::Get().GetToolsForProfile(TEXT("Full Stack"));
	const TArray<FOliveToolDefinition> CanonicalTools = FOliveToolRegistry::Get().GetToolsForProfile(TEXT("Everything"));
	TestEqual(TEXT("Legacy and canonical profiles should expose identical tool count"), LegacyTools.Num(), CanonicalTools.Num());

	FOlivePromptAssembler& Assembler = FOlivePromptAssembler::Get();
	Assembler.ReloadTemplates();

	const FString BlueprintAddition = Assembler.GetProfilePromptAddition(TEXT("Blueprint"));
	TestTrue(TEXT("Blueprint profile prompt addition should not be empty"), !BlueprintAddition.IsEmpty());

	const FString Prompt = Assembler.AssembleSystemPromptWithBase(TEXT("BasePrompt"), TEXT("Blueprint"), {}, 4096);
	TestTrue(TEXT("Assembled prompt should include profile section"), Prompt.Contains(TEXT("## Focus Mode")));
	if (!BlueprintAddition.IsEmpty())
	{
		const FString Prefix = BlueprintAddition.Left(FMath::Min(BlueprintAddition.Len(), 32));
		TestTrue(TEXT("Assembled prompt should include profile-specific addition"), Prompt.Contains(Prefix));
	}

	TArray<FString> Errors;
	FOliveFocusProfile FallbackProfile;
	FallbackProfile.Name = TEXT("CustomPromptFallback");
	FallbackProfile.DisplayName = FText::FromString(TEXT("Fallback"));
	FallbackProfile.SystemPromptAddition = TEXT("FallbackPromptOnly");
	FallbackProfile.ToolCategories = { TEXT("project") };
	TestTrue(TEXT("Custom fallback profile should upsert"), Manager.UpsertCustomProfile(FallbackProfile, Errors));

	Assembler.ReloadTemplates();
	const FString FallbackAddition = Assembler.GetProfilePromptAddition(TEXT("CustomPromptFallback"));
	TestEqual(TEXT("Profile without file prompt should use system prompt addition"), FallbackAddition, FString(TEXT("FallbackPromptOnly")));

	RestoreSettings(Snapshot);
	Manager.Initialize();

	return true;
}
