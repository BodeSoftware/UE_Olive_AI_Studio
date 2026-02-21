// Copyright Bode Software. All Rights Reserved.

#include "Profiles/OliveFocusProfileManager.h"
#include "OliveAIEditorModule.h"

#define LOCTEXT_NAMESPACE "OliveFocusProfile"

FOliveFocusProfileManager& FOliveFocusProfileManager::Get()
{
	static FOliveFocusProfileManager Instance;
	return Instance;
}

void FOliveFocusProfileManager::Initialize()
{
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
	const FOliveFocusProfile* Profile = Profiles.Find(Name);
	if (Profile)
	{
		return *Profile;
	}
	return TOptional<FOliveFocusProfile>();
}

bool FOliveFocusProfileManager::HasProfile(const FString& Name) const
{
	return Profiles.Contains(Name);
}

const FOliveFocusProfile& FOliveFocusProfileManager::GetDefaultProfile() const
{
	static FOliveFocusProfile DefaultProfile;

	const FOliveFocusProfile* Auto = Profiles.Find(TEXT("Auto"));
	if (Auto)
	{
		return *Auto;
	}

	// Fallback
	DefaultProfile.Name = TEXT("Auto");
	DefaultProfile.DisplayName = LOCTEXT("ProfileAuto", "Auto");
	return DefaultProfile;
}

// ==========================================
// Tool Filtering
// ==========================================

TArray<FString> FOliveFocusProfileManager::GetToolCategoriesForProfile(const FString& ProfileName) const
{
	const FOliveFocusProfile* Profile = Profiles.Find(ProfileName);
	if (Profile)
	{
		return Profile->ToolCategories;
	}
	return TArray<FString>(); // Empty = all categories
}

TArray<FString> FOliveFocusProfileManager::GetExcludedToolsForProfile(const FString& ProfileName) const
{
	const FOliveFocusProfile* Profile = Profiles.Find(ProfileName);
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
	const FOliveFocusProfile* Profile = Profiles.Find(ProfileName);
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
	const FOliveFocusProfile* Profile = Profiles.Find(ProfileName);
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
	FOliveFocusProfile CustomProfile = Profile;
	CustomProfile.bIsBuiltIn = false;

	Profiles.Add(Profile.Name, CustomProfile);
	CustomProfileNames.AddUnique(Profile.Name);

	UE_LOG(LogOliveAI, Log, TEXT("Added custom focus profile: %s"), *Profile.Name);
}

bool FOliveFocusProfileManager::RemoveCustomProfile(const FString& Name)
{
	const FOliveFocusProfile* Profile = Profiles.Find(Name);
	if (!Profile || Profile->bIsBuiltIn)
	{
		return false;
	}

	Profiles.Remove(Name);
	CustomProfileNames.Remove(Name);

	UE_LOG(LogOliveAI, Log, TEXT("Removed custom focus profile: %s"), *Name);
	return true;
}

void FOliveFocusProfileManager::SaveCustomProfiles()
{
	// TODO: Save to config file
	// For Phase 0, custom profiles are not persisted
}

void FOliveFocusProfileManager::LoadCustomProfiles()
{
	// TODO: Load from config file
	// For Phase 0, custom profiles are not persisted
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
		Profile.SortOrder = 4;
		Profile.IconName = TEXT("Icons.CPlusPlus");
		Profile.bIsBuiltIn = true;
		Profiles.Add(Profile.Name, Profile);
	}

	// Full Stack
	{
		FOliveFocusProfile Profile;
		Profile.Name = TEXT("Full Stack");
		Profile.DisplayName = LOCTEXT("ProfileFullStack", "Full Stack");
		Profile.Description = LOCTEXT("ProfileFullStackDesc", "All tools including C++, Blueprint, AI, PCG, and cross-system operations.");
		Profile.ToolCategories = { TEXT("blueprint"), TEXT("cpp"), TEXT("behaviortree"), TEXT("blackboard"), TEXT("pcg"), TEXT("crosssystem"), TEXT("project") };
		Profile.SystemPromptAddition = TEXT("You have access to all tools across all domains: C++, Blueprint, Behavior Trees, PCG, and cross-system operations. Consider the best tool or combination of tools for each task. Use C++ for performance-critical code, Blueprints for rapid iteration, and cross-system tools for multi-asset operations.");
		Profile.SortOrder = 5;
		Profile.IconName = TEXT("Icons.Settings");
		Profile.bIsBuiltIn = true;
		Profiles.Add(Profile.Name, Profile);
	}
}

#undef LOCTEXT_NAMESPACE
