// Copyright Bode Software. All Rights Reserved.

#include "OliveMCPPromptTemplates.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOlivePromptTemplates);

// ==========================================
// FOlivePromptParam
// ==========================================

TSharedPtr<FJsonObject> FOlivePromptParam::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("description"), Description);
	Json->SetBoolField(TEXT("required"), bRequired);
	return Json;
}

// ==========================================
// FOlivePromptTemplate
// ==========================================

TSharedPtr<FJsonObject> FOlivePromptTemplate::ToMCPJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("description"), Description);

	TArray<TSharedPtr<FJsonValue>> ArgsArray;
	for (const FOlivePromptParam& Param : Parameters)
	{
		ArgsArray.Add(MakeShared<FJsonValueObject>(Param.ToJson()));
	}
	Json->SetArrayField(TEXT("arguments"), ArgsArray);

	return Json;
}

// ==========================================
// FOliveMCPPromptTemplates - Singleton
// ==========================================

FOliveMCPPromptTemplates& FOliveMCPPromptTemplates::Get()
{
	static FOliveMCPPromptTemplates Instance;
	return Instance;
}

void FOliveMCPPromptTemplates::Initialize()
{
	Templates.Empty();
	RegisterDefaultTemplates();
	UE_LOG(LogOlivePromptTemplates, Log, TEXT("Initialized %d prompt templates"), Templates.Num());
}

// ==========================================
// Public API
// ==========================================

TSharedPtr<FJsonObject> FOliveMCPPromptTemplates::GetPromptsList() const
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> PromptsArray;
	for (const auto& Pair : Templates)
	{
		PromptsArray.Add(MakeShared<FJsonValueObject>(Pair.Value.ToMCPJson()));
	}
	Result->SetArrayField(TEXT("prompts"), PromptsArray);

	return Result;
}

TSharedPtr<FJsonObject> FOliveMCPPromptTemplates::GetPrompt(const FString& Name, const TSharedPtr<FJsonObject>& Arguments) const
{
	const FOlivePromptTemplate* Template = Templates.Find(Name);
	if (!Template)
	{
		return nullptr;
	}

	// Apply arguments to the template text
	FString FilledText = ApplyArguments(Template->TemplateText, Arguments);

	// Build MCP prompts/get response
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("description"), Template->Description);

	// Build messages array
	TArray<TSharedPtr<FJsonValue>> Messages;

	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("role"), TEXT("user"));

	TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
	Content->SetStringField(TEXT("type"), TEXT("text"));
	Content->SetStringField(TEXT("text"), FilledText);
	Message->SetObjectField(TEXT("content"), Content);

	Messages.Add(MakeShared<FJsonValueObject>(Message));
	Result->SetArrayField(TEXT("messages"), Messages);

	return Result;
}

bool FOliveMCPPromptTemplates::HasTemplate(const FString& Name) const
{
	return Templates.Contains(Name);
}

void FOliveMCPPromptTemplates::RegisterTemplate(const FOlivePromptTemplate& Template)
{
	if (Template.Name.IsEmpty())
	{
		UE_LOG(LogOlivePromptTemplates, Warning, TEXT("Attempted to register prompt template with empty name"));
		return;
	}

	Templates.Add(Template.Name, Template);
	UE_LOG(LogOlivePromptTemplates, Verbose, TEXT("Registered prompt template: %s"), *Template.Name);
}

// ==========================================
// Private Implementation
// ==========================================

FString FOliveMCPPromptTemplates::ApplyArguments(const FString& TemplateText, const TSharedPtr<FJsonObject>& Arguments) const
{
	FString Result = TemplateText;

	if (!Arguments.IsValid())
	{
		// Remove unfilled placeholders
		// Simple approach: leave them as-is so the user sees what's missing
		return Result;
	}

	for (const auto& Pair : Arguments->Values)
	{
		FString Placeholder = FString::Printf(TEXT("{{%s}}"), *Pair.Key);
		FString Value;

		if (Pair.Value->TryGetString(Value))
		{
			Result = Result.Replace(*Placeholder, *Value);
		}
	}

	return Result;
}

void FOliveMCPPromptTemplates::RegisterDefaultTemplates()
{
	// ==========================================
	// 1. explain_blueprint
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("explain_blueprint");
		Template.Description = TEXT("Get a comprehensive explanation of a Blueprint asset including its purpose, variables, functions, and components");
		Template.Parameters = {
			{ TEXT("asset_path"), TEXT("Path to the Blueprint asset (e.g., /Game/Blueprints/BP_Player)"), true }
		};
		Template.TemplateText =
			TEXT("Read the Blueprint at {{asset_path}} using blueprint.read with mode 'full'. ")
			TEXT("Then provide a comprehensive explanation of:\n")
			TEXT("1. What this Blueprint does (purpose and behavior)\n")
			TEXT("2. Its class hierarchy and interfaces\n")
			TEXT("3. Key variables and their purposes\n")
			TEXT("4. Important functions and event handlers\n")
			TEXT("5. Component setup\n")
			TEXT("6. Any notable patterns or potential issues");

		RegisterTemplate(Template);
	}

	// ==========================================
	// 2. review_blueprint
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("review_blueprint");
		Template.Description = TEXT("Review a Blueprint for quality, best practices, and potential improvements");
		Template.Parameters = {
			{ TEXT("asset_path"), TEXT("Path to the Blueprint asset to review"), true },
			{ TEXT("focus_areas"), TEXT("Specific areas to focus the review on (e.g., performance, replication)"), false }
		};
		Template.TemplateText =
			TEXT("Review the Blueprint at {{asset_path}} for quality and best practices. ")
			TEXT("Read it using blueprint.read with mode 'full'.\n\n")
			TEXT("Focus on:\n")
			TEXT("{{focus_areas}}\n\n")
			TEXT("Provide feedback on:\n")
			TEXT("1. Architecture and design patterns\n")
			TEXT("2. Performance considerations\n")
			TEXT("3. Blueprint vs C++ decisions\n")
			TEXT("4. Naming conventions\n")
			TEXT("5. Error handling\n")
			TEXT("6. Replication setup (if applicable)\n")
			TEXT("7. Specific suggestions for improvement");

		RegisterTemplate(Template);
	}

	// ==========================================
	// 3. plan_feature
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("plan_feature");
		Template.Description = TEXT("Plan the implementation of a new feature with architecture, steps, and recommendations");
		Template.Parameters = {
			{ TEXT("feature_description"), TEXT("Description of the feature to implement"), true },
			{ TEXT("relevant_assets"), TEXT("Existing assets that relate to this feature (comma-separated paths)"), false }
		};
		Template.TemplateText =
			TEXT("Plan the implementation of the following feature:\n\n")
			TEXT("{{feature_description}}\n\n")
			TEXT("Relevant existing assets to examine: {{relevant_assets}}\n\n")
			TEXT("Provide:\n")
			TEXT("1. High-level architecture (which assets to create/modify)\n")
			TEXT("2. Step-by-step implementation plan\n")
			TEXT("3. Blueprint vs C++ recommendations for each component\n")
			TEXT("4. Required data structures (variables, structs)\n")
			TEXT("5. Event flow and communication between systems\n")
			TEXT("6. Testing strategy");

		RegisterTemplate(Template);
	}

	// ==========================================
	// 4. migrate_to_cpp
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("migrate_to_cpp");
		Template.Description = TEXT("Analyze a Blueprint and plan its migration to C++ with a hybrid approach");
		Template.Parameters = {
			{ TEXT("asset_path"), TEXT("Path to the Blueprint asset to analyze for C++ migration"), true }
		};
		Template.TemplateText =
			TEXT("Analyze the Blueprint at {{asset_path}} for migration to C++.\n\n")
			TEXT("1. Read the Blueprint fully using blueprint.read\n")
			TEXT("2. Identify which logic should move to C++ (performance-critical, complex math, reusable systems)\n")
			TEXT("3. Identify which logic should stay in Blueprint (rapid iteration, designer-facing, simple event responses)\n")
			TEXT("4. Propose C++ class structure using cpp.create_class\n")
			TEXT("5. List UPROPERTY and UFUNCTION declarations needed\n")
			TEXT("6. Show how Blueprint would extend the C++ base class\n")
			TEXT("7. Provide migration steps in order");

		RegisterTemplate(Template);
	}

	// ==========================================
	// 5. debug_compile_error
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("debug_compile_error");
		Template.Description = TEXT("Diagnose and fix a Blueprint compilation error with step-by-step guidance");
		Template.Parameters = {
			{ TEXT("error_message"), TEXT("The compilation error message to debug"), true },
			{ TEXT("asset_path"), TEXT("Path to the Blueprint asset with the error (if known)"), false }
		};
		Template.TemplateText =
			TEXT("Help debug this compilation error:\n\n")
			TEXT("```\n")
			TEXT("{{error_message}}\n")
			TEXT("```\n\n")
			TEXT("Asset involved: {{asset_path}}\n\n")
			TEXT("Steps:\n")
			TEXT("1. Parse the error message to identify the root cause\n")
			TEXT("2. If an asset path is provided, read it to understand the context\n")
			TEXT("3. Search for related assets using project.search\n")
			TEXT("4. Identify the specific node, variable, or connection causing the issue\n")
			TEXT("5. Provide a fix with step-by-step instructions\n")
			TEXT("6. Explain why the error occurred to prevent recurrence");

		RegisterTemplate(Template);
	}
}
