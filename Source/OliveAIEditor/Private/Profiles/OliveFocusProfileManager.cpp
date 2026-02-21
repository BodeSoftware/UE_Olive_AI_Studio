// Copyright Bode Software. All Rights Reserved.

#include "Profiles/OliveFocusProfileManager.h"
#include "OliveAIEditorModule.h"
#include "MCP/OliveToolRegistry.h"
#include "Settings/OliveAISettings.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "OliveFocusProfile"

namespace OliveFocusProfileConstants
{
	static const FString AutoProfileName = TEXT("Auto");
	static const FString EverythingProfileName = TEXT("Everything");
	static const FString LegacyFullStackProfileName = TEXT("Full Stack");
}

FOliveFocusProfileManager& FOliveFocusProfileManager::Get()
{
	static FOliveFocusProfileManager Instance;
	return Instance;
}

void FOliveFocusProfileManager::Initialize()
{
	Profiles.Empty();
	CustomProfileNames.Empty();

	RegisterDefaultProfiles();
	LoadCustomProfiles();

	UE_LOG(LogOliveAI, Log, TEXT("Focus Profile Manager initialized with %d profiles"), Profiles.Num());
}

// ==========================================
// Profile Access
// ==========================================

TArray<FOliveFocusProfile> FOliveFocusProfileManager::GetAllProfiles() const
{
	TArray<FOliveFocusProfile> Result;
	Profiles.GenerateValueArray(Result);

	// Sort by SortOrder
	Result.Sort([](const FOliveFocusProfile& A, const FOliveFocusProfile& B)
	{
		return A.SortOrder < B.SortOrder;
	});

	return Result;
}

TArray<FString> FOliveFocusProfileManager::GetProfileNames() const
{
	TArray<FString> Names;
	for (const auto& Pair : Profiles)
	{
		Names.Add(Pair.Key);
	}

	// Sort by profile sort order
	Names.Sort([this](const FString& A, const FString& B)
	{
		const FOliveFocusProfile* ProfileA = Profiles.Find(A);
		const FOliveFocusProfile* ProfileB = Profiles.Find(B);
		if (ProfileA && ProfileB)
		{
			return ProfileA->SortOrder < ProfileB->SortOrder;
		}
		return A < B;
	});

	return Names;
}

TOptional<FOliveFocusProfile> FOliveFocusProfileManager::GetProfile(const FString& Name) const
{
	const FString NormalizedName = NormalizeProfileName(Name);
	const FOliveFocusProfile* Profile = Profiles.Find(NormalizedName);
	if (Profile)
	{
		return *Profile;
	}
	return TOptional<FOliveFocusProfile>();
}

bool FOliveFocusProfileManager::HasProfile(const FString& Name) const
{
	return Profiles.Contains(NormalizeProfileName(Name));
}

FString FOliveFocusProfileManager::NormalizeProfileName(const FString& InName) const
{
	const FString Trimmed = InName.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return Trimmed;
	}

	if (Trimmed.Equals(OliveFocusProfileConstants::LegacyFullStackProfileName, ESearchCase::IgnoreCase))
	{
		return OliveFocusProfileConstants::EverythingProfileName;
	}

	return Trimmed;
}

bool FOliveFocusProfileManager::IsLegacyProfileName(const FString& InName) const
{
	return InName.TrimStartAndEnd().Equals(OliveFocusProfileConstants::LegacyFullStackProfileName, ESearchCase::IgnoreCase);
}

const FOliveFocusProfile& FOliveFocusProfileManager::GetDefaultProfile() const
{
	static FOliveFocusProfile DefaultProfile;

	const FOliveFocusProfile* Auto = Profiles.Find(OliveFocusProfileConstants::AutoProfileName);
	if (Auto)
	{
		return *Auto;
	}

	// Fallback
	DefaultProfile.Name = OliveFocusProfileConstants::AutoProfileName;
	DefaultProfile.DisplayName = LOCTEXT("ProfileAuto", "Auto");
	return DefaultProfile;
}

// ==========================================
// Tool Filtering
// ==========================================

TArray<FString> FOliveFocusProfileManager::GetToolCategoriesForProfile(const FString& ProfileName) const
{
	const FString NormalizedName = NormalizeProfileName(ProfileName);
	const FOliveFocusProfile* Profile = Profiles.Find(NormalizedName);
	if (Profile)
	{
		return Profile->ToolCategories;
	}
	return TArray<FString>(); // Empty = all categories
}

TArray<FString> FOliveFocusProfileManager::GetExcludedToolsForProfile(const FString& ProfileName) const
{
	const FString NormalizedName = NormalizeProfileName(ProfileName);
	const FOliveFocusProfile* Profile = Profiles.Find(NormalizedName);
	if (Profile)
	{
		return Profile->ExcludedTools;
	}
	return TArray<FString>();
}

bool FOliveFocusProfileManager::IsToolAllowedForProfile(
	const FString& ProfileName,
	const FString& ToolName,
	const FString& ToolCategory) const
{
	const FString NormalizedName = NormalizeProfileName(ProfileName);
	const FOliveFocusProfile* Profile = Profiles.Find(NormalizedName);
	if (!Profile)
	{
		// Unknown profile, allow all
		return true;
	}

	// Check if specifically excluded
	if (Profile->ExcludedTools.Contains(ToolName))
	{
		return false;
	}

	// If no categories specified, allow all
	if (Profile->ToolCategories.Num() == 0)
	{
		return true;
	}

	// Check if category is allowed
	return Profile->ToolCategories.Contains(ToolCategory);
}

FString FOliveFocusProfileManager::GetSystemPromptAddition(const FString& ProfileName) const
{
	const FString NormalizedName = NormalizeProfileName(ProfileName);
	const FOliveFocusProfile* Profile = Profiles.Find(NormalizedName);
	if (Profile)
	{
		return Profile->SystemPromptAddition;
	}
	return TEXT("");
}

// ==========================================
// Custom Profiles
// ==========================================

void FOliveFocusProfileManager::AddCustomProfile(const FOliveFocusProfile& Profile)
{
	TArray<FString> Errors;
	if (!UpsertCustomProfile(Profile, Errors))
	{
		UE_LOG(LogOliveAI, Warning, TEXT("Failed to add custom focus profile '%s': %s"),
			*Profile.Name,
			*FString::Join(Errors, TEXT("; ")));
	}
}

bool FOliveFocusProfileManager::UpsertCustomProfile(const FOliveFocusProfile& Profile, TArray<FString>& OutErrors)
{
	OutErrors.Reset();

	FOliveFocusProfile Candidate = Profile;
	Candidate.Name = NormalizeProfileName(Candidate.Name);
	Candidate.bIsBuiltIn = false;
	Candidate.SchemaVersion = CustomProfileSchemaVersion;

	if (Candidate.SortOrder <= 0)
	{
		int32 MaxSortOrder = 0;
		for (const auto& Pair : Profiles)
		{
			if (Pair.Value.bIsBuiltIn)
			{
				MaxSortOrder = FMath::Max(MaxSortOrder, Pair.Value.SortOrder);
			}
		}
		Candidate.SortOrder = MaxSortOrder + 1;
	}

	if (!ValidateProfile(Candidate, OutErrors))
	{
		return false;
	}

	Profiles.Add(Candidate.Name, Candidate);
	CustomProfileNames.AddUnique(Candidate.Name);
	SaveCustomProfiles();

	UE_LOG(LogOliveAI, Log, TEXT("Upserted custom focus profile: %s"), *Candidate.Name);
	return true;
}

bool FOliveFocusProfileManager::ValidateProfile(const FOliveFocusProfile& Profile, TArray<FString>& OutErrors) const
{
	OutErrors.Reset();

	const FString NormalizedName = NormalizeProfileName(Profile.Name);
	if (NormalizedName.IsEmpty())
	{
		OutErrors.Add(TEXT("Profile name is required."));
		return false;
	}

	for (const auto& Pair : Profiles)
	{
		const FString ExistingName = Pair.Key;
		const FOliveFocusProfile& ExistingProfile = Pair.Value;

		if (ExistingName.Equals(NormalizedName, ESearchCase::IgnoreCase))
		{
			const bool bSameCustomProfile = !ExistingProfile.bIsBuiltIn && ExistingName.Equals(Profile.Name, ESearchCase::IgnoreCase);
			if (!bSameCustomProfile)
			{
				OutErrors.Add(FString::Printf(TEXT("Profile name '%s' conflicts with an existing profile."), *NormalizedName));
			}

			if (ExistingProfile.bIsBuiltIn && !bSameCustomProfile)
			{
				OutErrors.Add(FString::Printf(TEXT("Built-in profile '%s' cannot be overridden."), *ExistingName));
			}
		}
	}

	if (IsLegacyProfileName(Profile.Name))
	{
		OutErrors.Add(TEXT("Profile name 'Full Stack' is reserved as a legacy alias. Use 'Everything'."));
	}

	TSet<FString> KnownCategories;
	TSet<FString> KnownTools;
	for (const FOliveToolDefinition& Definition : FOliveToolRegistry::Get().GetAllTools())
	{
		if (!Definition.Category.IsEmpty())
		{
			KnownCategories.Add(Definition.Category.ToLower());
		}
		KnownTools.Add(Definition.Name.ToLower());
	}

	for (const FString& Category : Profile.ToolCategories)
	{
		const FString CategoryLower = Category.ToLower();
		if (!KnownCategories.Contains(CategoryLower))
		{
			OutErrors.Add(FString::Printf(TEXT("Unknown tool category '%s'."), *Category));
		}
	}

	for (const FString& ToolName : Profile.ExcludedTools)
	{
		const FString ToolLower = ToolName.ToLower();
		if (!KnownTools.Contains(ToolLower))
		{
			OutErrors.Add(FString::Printf(TEXT("Unknown excluded tool '%s'."), *ToolName));
		}
	}

	return OutErrors.Num() == 0;
}

bool FOliveFocusProfileManager::RemoveCustomProfile(const FString& Name)
{
	const FString NormalizedName = NormalizeProfileName(Name);
	const FOliveFocusProfile* Profile = Profiles.Find(NormalizedName);
	if (!Profile || Profile->bIsBuiltIn)
	{
		return false;
	}

	Profiles.Remove(NormalizedName);
	CustomProfileNames.Remove(NormalizedName);
	SaveCustomProfiles();

	UE_LOG(LogOliveAI, Log, TEXT("Removed custom focus profile: %s"), *NormalizedName);
	return true;
}

void FOliveFocusProfileManager::SaveCustomProfiles()
{
	UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		return;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), static_cast<double>(CustomProfileSchemaVersion));

	TArray<TSharedPtr<FJsonValue>> ProfilesArray;
	for (const FString& CustomName : CustomProfileNames)
	{
		const FOliveFocusProfile* Profile = Profiles.Find(CustomName);
		if (!Profile || Profile->bIsBuiltIn)
		{
			continue;
		}

		TSharedPtr<FJsonObject> ProfileJson = MakeShared<FJsonObject>();
		ProfileJson->SetStringField(TEXT("name"), Profile->Name);
		ProfileJson->SetStringField(TEXT("display_name"), Profile->DisplayName.ToString());
		ProfileJson->SetStringField(TEXT("description"), Profile->Description.ToString());
		ProfileJson->SetStringField(TEXT("system_prompt_addition"), Profile->SystemPromptAddition);
		ProfileJson->SetStringField(TEXT("icon_name"), Profile->IconName);
		ProfileJson->SetNumberField(TEXT("sort_order"), static_cast<double>(Profile->SortOrder));
		ProfileJson->SetStringField(TEXT("prompt_template_file"), Profile->PromptTemplateFile);
		ProfileJson->SetNumberField(TEXT("schema_version"), static_cast<double>(Profile->SchemaVersion));

		TArray<TSharedPtr<FJsonValue>> CategoriesArray;
		for (const FString& Category : Profile->ToolCategories)
		{
			CategoriesArray.Add(MakeShared<FJsonValueString>(Category));
		}
		ProfileJson->SetArrayField(TEXT("tool_categories"), CategoriesArray);

		TArray<TSharedPtr<FJsonValue>> ExcludedToolsArray;
		for (const FString& Tool : Profile->ExcludedTools)
		{
			ExcludedToolsArray.Add(MakeShared<FJsonValueString>(Tool));
		}
		ProfileJson->SetArrayField(TEXT("excluded_tools"), ExcludedToolsArray);

		ProfilesArray.Add(MakeShared<FJsonValueObject>(ProfileJson));
	}

	Root->SetArrayField(TEXT("profiles"), ProfilesArray);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		Settings->CustomFocusProfilesSchemaVersion = CustomProfileSchemaVersion;
		Settings->CustomFocusProfilesJson = Output;
		Settings->SaveConfig();
	}
}

void FOliveFocusProfileManager::LoadCustomProfiles()
{
	UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings || Settings->CustomFocusProfilesJson.IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Settings->CustomFocusProfilesJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogOliveAI, Warning, TEXT("Failed to parse custom focus profiles JSON. Skipping custom profile load."));
		return;
	}

	const int32 Version = Root->GetIntegerField(TEXT("version"));
	if (Version != CustomProfileSchemaVersion)
	{
		UE_LOG(LogOliveAI, Warning, TEXT("Unsupported custom profile schema version %d (expected %d). Skipping custom profile load."),
			Version, CustomProfileSchemaVersion);
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* ProfilesArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("profiles"), ProfilesArray))
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& ProfileValue : *ProfilesArray)
	{
		const TSharedPtr<FJsonObject> ProfileJson = ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
		if (!ProfileJson.IsValid())
		{
			continue;
		}

		FOliveFocusProfile Profile;
		Profile.bIsBuiltIn = false;
		Profile.Name = NormalizeProfileName(ProfileJson->GetStringField(TEXT("name")));
		Profile.DisplayName = FText::FromString(ProfileJson->GetStringField(TEXT("display_name")));
		Profile.Description = FText::FromString(ProfileJson->GetStringField(TEXT("description")));
		Profile.SystemPromptAddition = ProfileJson->GetStringField(TEXT("system_prompt_addition"));
		Profile.IconName = ProfileJson->GetStringField(TEXT("icon_name"));
		Profile.SortOrder = ProfileJson->GetIntegerField(TEXT("sort_order"));
		Profile.PromptTemplateFile = ProfileJson->GetStringField(TEXT("prompt_template_file"));
		Profile.SchemaVersion = ProfileJson->HasField(TEXT("schema_version"))
			? ProfileJson->GetIntegerField(TEXT("schema_version"))
			: CustomProfileSchemaVersion;

		const TArray<TSharedPtr<FJsonValue>>* CategoriesArray = nullptr;
		if (ProfileJson->TryGetArrayField(TEXT("tool_categories"), CategoriesArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *CategoriesArray)
			{
				if (Value.IsValid() && Value->Type == EJson::String)
				{
					Profile.ToolCategories.Add(Value->AsString());
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* ExcludedArray = nullptr;
		if (ProfileJson->TryGetArrayField(TEXT("excluded_tools"), ExcludedArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *ExcludedArray)
			{
				if (Value.IsValid() && Value->Type == EJson::String)
				{
					Profile.ExcludedTools.Add(Value->AsString());
				}
			}
		}

		TArray<FString> Errors;
		if (ValidateProfile(Profile, Errors))
		{
			Profiles.Add(Profile.Name, Profile);
			CustomProfileNames.AddUnique(Profile.Name);
		}
		else
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Skipping invalid custom profile '%s': %s"),
				*Profile.Name,
				*FString::Join(Errors, TEXT("; ")));
		}
	}
}

int32 FOliveFocusProfileManager::GetCustomProfileSchemaVersion() const
{
	return CustomProfileSchemaVersion;
}

// ==========================================
// Default Profiles
// ==========================================

void FOliveFocusProfileManager::RegisterDefaultProfiles()
{
	// Auto (default)
	{
		FOliveFocusProfile Profile;
		Profile.Name = TEXT("Auto");
		Profile.DisplayName = LOCTEXT("ProfileAuto", "Auto");
		Profile.Description = LOCTEXT("ProfileAutoDesc", "All tools available. AI determines which to use based on context.");
		Profile.ToolCategories = {}; // Empty = all
		Profile.SortOrder = 0;
		Profile.IconName = TEXT("Icons.Help");
		Profile.bIsBuiltIn = true;
		Profiles.Add(Profile.Name, Profile);
	}

	// Blueprint
	{
		FOliveFocusProfile Profile;
		Profile.Name = TEXT("Blueprint");
		Profile.DisplayName = LOCTEXT("ProfileBlueprint", "Blueprint");
		Profile.Description = LOCTEXT("ProfileBlueprintDesc", "Focus on Blueprint development. Includes project and Blueprint tools.");
		Profile.ToolCategories = { TEXT("blueprint"), TEXT("project") };
		Profile.SystemPromptAddition = TEXT("You are focused on Blueprint development. Prioritize Blueprint-based solutions over C++ when possible. Consider component composition and event-driven design.");
		Profile.SortOrder = 1;
		Profile.IconName = TEXT("Icons.Blueprint");
		Profile.PromptTemplateFile = TEXT("ProfileBlueprint.txt");
		Profile.bIsBuiltIn = true;
		Profiles.Add(Profile.Name, Profile);
	}

	// AI & Behavior
	{
		FOliveFocusProfile Profile;
		Profile.Name = TEXT("AI & Behavior");
		Profile.DisplayName = LOCTEXT("ProfileAIBehavior", "AI & Behavior");
		Profile.Description = LOCTEXT("ProfileAIBehaviorDesc", "Focus on AI systems: Behavior Trees, Blackboards, and AI-related Blueprints.");
		Profile.ToolCategories = { TEXT("blueprint"), TEXT("behaviortree"), TEXT("blackboard"), TEXT("project") };
		Profile.SystemPromptAddition = TEXT("You are focused on AI system development. Consider Behavior Trees for decision-making, Blackboards for state management, and EQS for environmental queries. Prefer data-driven AI designs.");
		Profile.SortOrder = 2;
		Profile.IconName = TEXT("Icons.AI");
		Profile.PromptTemplateFile = TEXT("ProfileAIBehavior.txt");
		Profile.bIsBuiltIn = true;
		Profiles.Add(Profile.Name, Profile);
	}

	// Level & PCG
	{
		FOliveFocusProfile Profile;
		Profile.Name = TEXT("Level & PCG");
		Profile.DisplayName = LOCTEXT("ProfileLevelPCG", "Level & PCG");
		Profile.Description = LOCTEXT("ProfileLevelPCGDesc", "Focus on level design and procedural content generation.");
		Profile.ToolCategories = { TEXT("blueprint"), TEXT("pcg"), TEXT("project") };
		Profile.SystemPromptAddition = TEXT("You are focused on level design and procedural content generation. Consider PCG graphs for procedural placement, Blueprints for gameplay elements, and data-driven level construction.");
		Profile.SortOrder = 3;
		Profile.IconName = TEXT("Icons.Level");
		Profile.PromptTemplateFile = TEXT("ProfileLevelPCG.txt");
		Profile.bIsBuiltIn = true;
		Profiles.Add(Profile.Name, Profile);
	}

	// C++ Only
	{
		FOliveFocusProfile Profile;
		Profile.Name = TEXT("C++ Only");
		Profile.DisplayName = LOCTEXT("ProfileCppOnly", "C++ Only");
		Profile.Description = LOCTEXT("ProfileCppOnlyDesc", "Only C++ tools are available. Blueprint/PCG/AI tools are disabled.");
		Profile.ToolCategories = { TEXT("cpp") };
		Profile.SystemPromptAddition = TEXT("You are in C++ Only mode. Do not propose or use Blueprint/PCG/Behavior Tree tools. Implement solutions in native C++ only, using Unreal coding conventions.");
		Profile.SortOrder = 4;
		Profile.IconName = TEXT("Icons.CPlusPlus");
		Profile.bIsBuiltIn = true;
		Profiles.Add(Profile.Name, Profile);
	}

	// C++ & Blueprint
	{
		FOliveFocusProfile Profile;
		Profile.Name = TEXT("C++ & Blueprint");
		Profile.DisplayName = LOCTEXT("ProfileCppBP", "C++ & Blueprint");
		Profile.Description = LOCTEXT("ProfileCppBPDesc", "Mixed C++ and Blueprint workflows. For developers working with both.");
		Profile.ToolCategories = { TEXT("blueprint"), TEXT("cpp"), TEXT("project"), TEXT("crosssystem") };
		Profile.SystemPromptAddition = TEXT("You are working with both C++ and Blueprints. Consider exposing C++ functionality to Blueprint where appropriate. Use UPROPERTY, UFUNCTION with appropriate specifiers. Prefer native C++ for performance-critical code and Blueprint for rapid iteration.");
		Profile.SortOrder = 5;
		Profile.IconName = TEXT("Icons.CPlusPlus");
		Profile.PromptTemplateFile = TEXT("ProfileCppBlueprint.txt");
		Profile.bIsBuiltIn = true;
		Profiles.Add(Profile.Name, Profile);
	}

	// Everything
	{
		FOliveFocusProfile Profile;
		Profile.Name = OliveFocusProfileConstants::EverythingProfileName;
		Profile.DisplayName = LOCTEXT("ProfileEverything", "Everything");
		Profile.Description = LOCTEXT("ProfileEverythingDesc", "All tools including C++, Blueprint, AI, PCG, and cross-system operations.");
		Profile.ToolCategories = { TEXT("blueprint"), TEXT("cpp"), TEXT("behaviortree"), TEXT("blackboard"), TEXT("pcg"), TEXT("crosssystem"), TEXT("project") };
		Profile.SystemPromptAddition = TEXT("You have access to all tools across all domains: C++, Blueprint, Behavior Trees, PCG, and cross-system operations. Consider the best tool or combination of tools for each task. Use C++ for performance-critical code, Blueprints for rapid iteration, and cross-system tools for multi-asset operations.");
		Profile.SortOrder = 6;
		Profile.IconName = TEXT("Icons.Settings");
		Profile.bIsBuiltIn = true;
		Profiles.Add(Profile.Name, Profile);
	}
}

#undef LOCTEXT_NAMESPACE
