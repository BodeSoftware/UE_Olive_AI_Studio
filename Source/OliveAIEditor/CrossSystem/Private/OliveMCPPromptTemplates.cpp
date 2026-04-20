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
			TEXT("3. Search for related assets using project.get_relevant_context\n")
			TEXT("4. Identify the specific node, variable, or connection causing the issue\n")
			TEXT("5. Provide a fix with step-by-step instructions\n")
			TEXT("6. Explain why the error occurred to prevent recurrence");

		RegisterTemplate(Template);
	}

	// ==========================================
	// 6. start_task
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("start_task");
		Template.Description = TEXT("Orient yourself for a new task. Routes to the right workflow based on task type.");
		Template.Parameters = {
			{ TEXT("task_description"), TEXT("Description of the task to accomplish"), true },
			{ TEXT("task_type"), TEXT("Hint for task type: create, modify, fix, or research"), false }
		};
		Template.TemplateText =
			TEXT("You have a task to accomplish in an Unreal Engine project using Olive AI Studio's MCP tools.\n\n")
			TEXT("Task: {{task_description}}\n")
			TEXT("Task type hint: {{task_type}}\n\n")
			TEXT("Before building anything, orient yourself:\n\n")
			TEXT("1. Search the project for related assets: project.get_relevant_context with keywords from the task. ")
			TEXT("If the asset already exists, modify it rather than creating a duplicate.\n\n")
			TEXT("2. If creating something new, check for reference patterns: blueprint.list_templates with a query matching your task domain. ")
			TEXT("Library templates are extracted from real projects and show proven patterns. ")
			TEXT("Factory templates are hand-crafted starting points.\n\n")
			TEXT("3. Pick the right workflow for this task:\n")
			TEXT("   - Creating a new Blueprint or system: use the \"build_blueprint_feature\" prompt\n")
			TEXT("   - Modifying an existing Blueprint: use the \"modify_existing_blueprint\" prompt\n")
			TEXT("   - Fixing compile errors: use the \"fix_compile_errors\" prompt\n")
			TEXT("   - Researching patterns before deciding on an approach: use the \"research_reference_patterns\" prompt\n\n")
			TEXT("4. Use olive.build for any sequence of 3+ operations. ")
			TEXT("Do not call blueprint.create, blueprint.apply_plan_json, blueprint.compile individually when you can batch them.\n\n")
			TEXT("5. When building, compile after completing each function's graph logic. ")
			TEXT("Fix the first compile error before writing more logic.\n\n")
			TEXT("6. When done, verify your work compiles cleanly and contains the logic you intended. ")
			TEXT("Use the \"verify_and_finish\" prompt if available.\n\n")
			TEXT("Read before write. Compile after changes. Complete the full task.");

		RegisterTemplate(Template);
	}

	// ==========================================
	// 7. build_blueprint_feature
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("build_blueprint_feature");
		Template.Description = TEXT("Step-by-step workflow for creating a new Blueprint feature from scratch.");
		Template.Parameters = {
			{ TEXT("feature_description"), TEXT("Description of the feature to build"), true },
			{ TEXT("target_path"), TEXT("Target asset path for the new Blueprint (e.g., /Game/Blueprints/BP_MyActor)"), false },
			{ TEXT("parent_class"), TEXT("Parent class for the Blueprint (e.g., Actor, Character, ActorComponent)"), false }
		};
		Template.TemplateText =
			TEXT("Build a Blueprint feature in the current Unreal Engine project.\n\n")
			TEXT("Feature: {{feature_description}}\n")
			TEXT("Target path: {{target_path}}\n")
			TEXT("Parent class: {{parent_class}}\n\n")
			TEXT("Work through these stages. Use your judgment on how much research is needed -- ")
			TEXT("simple features need less, complex systems need more.\n\n")
			TEXT("ORIENT: Search the project for existing assets that relate to this feature (project.get_relevant_context). ")
			TEXT("Check if templates have relevant patterns (blueprint.list_templates with domain keywords). ")
			TEXT("For complex systems, read a template's function graph to see how similar logic is structured ")
			TEXT("(blueprint.get_template with pattern parameter).\n\n")
			TEXT("STRUCTURE + BUILD: Use olive.build to batch all operations (create, add_component, add_variable, ")
			TEXT("apply_plan_json, compile) into a single call whenever you have 3+ operations. ")
			TEXT("Do not call these tools individually when you can batch them.\n\n")
			TEXT("plan_json works well for standard function logic (3+ nodes). ")
			TEXT("Granular tools (add_node, connect_pins) work better for unusual node types or small edits. ")
			TEXT("Python (editor.run_python) handles anything the other tools cannot express. ")
			TEXT("Preview plan_json before applying it.\n\n")
			TEXT("For multi-Blueprint systems, identify all required assets up front, create them all, ")
			TEXT("then build each one to completion before starting the next.\n\n")
			TEXT("VERIFY: Compile after completing each function's logic. ")
			TEXT("Fix the first error before moving on. ")
			TEXT("When all functions are done, do a final compile to confirm zero errors.\n\n")
			TEXT("Events and custom_events are exec sources -- they have no exec input pin. ")
			TEXT("Wire FROM them, not TO them. Data wires use @step.auto syntax. ")
			TEXT("Exec wires use plain step_id with no @ prefix. ")
			TEXT("Component references use @ComponentName (no dot suffix).");

		RegisterTemplate(Template);
	}

	// ==========================================
	// 8. modify_existing_blueprint
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("modify_existing_blueprint");
		Template.Description = TEXT("Workflow for modifying an existing Blueprint's logic, variables, or components.");
		Template.Parameters = {
			{ TEXT("asset_path"), TEXT("Path to the Blueprint asset to modify"), true },
			{ TEXT("change_description"), TEXT("Description of the changes to make"), true }
		};
		Template.TemplateText =
			TEXT("Modify an existing Blueprint in the current Unreal Engine project.\n\n")
			TEXT("Asset: {{asset_path}}\n")
			TEXT("Change: {{change_description}}\n\n")
			TEXT("READ FIRST: Read the Blueprint to understand its current state ")
			TEXT("(blueprint.read with mode \"full\"). ")
			TEXT("If modifying a specific function, also read that function's graph ")
			TEXT("(blueprint.describe_function) to see existing nodes and wiring. ")
			TEXT("You need to know what is already there before changing it.\n\n")
			TEXT("PLAN: Decide what needs to change. If adding a new function, consider checking templates ")
			TEXT("for reference patterns (blueprint.list_templates with relevant keywords). ")
			TEXT("If the change involves unfamiliar functions, verify they exist ")
			TEXT("(blueprint.describe_function with the function name and target class).\n\n")
			TEXT("APPLY: Add any new variables or components needed first. ")
			TEXT("For graph logic changes, plan_json with the target function name adds new logic while ")
			TEXT("preserving existing nodes. The plan executor reuses existing event nodes ")
			TEXT("(BeginPlay, Tick) automatically and cleans up stale chains from previous attempts.\n\n")
			TEXT("If you need to wire to nodes that already exist in the graph (placed by a previous tool call ")
			TEXT("or by the user), use granular tools (connect_pins with node IDs from blueprint.read) ")
			TEXT("rather than plan_json, which only creates new nodes.\n\n")
			TEXT("VERIFY: Compile the Blueprint. Read back the modified function to confirm the graph matches ")
			TEXT("your intent. Fix any compile errors before declaring done.");

		RegisterTemplate(Template);
	}

	// ==========================================
	// 9. fix_compile_errors
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("fix_compile_errors");
		Template.Description = TEXT("Systematic approach to diagnosing and fixing Blueprint compile errors.");
		Template.Parameters = {
			{ TEXT("asset_path"), TEXT("Path to the Blueprint asset with compile errors"), true },
			{ TEXT("error_context"), TEXT("Additional context about the errors (e.g., paste of error messages)"), false }
		};
		Template.TemplateText =
			TEXT("Fix compilation errors in a Blueprint.\n\n")
			TEXT("Asset: {{asset_path}}\n")
			TEXT("Error context: {{error_context}}\n\n")
			TEXT("DIAGNOSE: Read the Blueprint (blueprint.read mode \"full\") and compile it (blueprint.compile) ")
			TEXT("to get a fresh error list. Compile errors cascade -- the first error often causes downstream ")
			TEXT("errors that disappear once the root cause is fixed.\n\n")
			TEXT("FOCUS ON THE FIRST ERROR: Identify the function graph where the first error occurs. ")
			TEXT("Read that specific function (blueprint.describe_function) to see the full node graph ")
			TEXT("including pin connections and default values.\n\n")
			TEXT("Common root causes:\n")
			TEXT("- Unwired required data pins (a function input has no connection and no default value)\n")
			TEXT("- Type mismatches (connecting incompatible pin types -- check if a cast or conversion is needed)\n")
			TEXT("- Missing variables (get_var/set_var referencing a variable that does not exist or was renamed)\n")
			TEXT("- Orphaned exec flow (nodes not connected to the execution chain from the entry point)\n")
			TEXT("- Function not found (the function was removed, renamed, or belongs to a different class)\n\n")
			TEXT("FIX: Apply the smallest change that resolves the error. Use plan_json to rewrite a function's ")
			TEXT("logic if the graph is fundamentally broken, or use granular tools ")
			TEXT("(connect_pins, set_pin_default, remove_node) for surgical fixes.\n\n")
			TEXT("ITERATE: Compile again after each fix. Address the next first error. Repeat until the Blueprint ")
			TEXT("compiles cleanly. Do not attempt to fix all errors at once -- each fix may resolve multiple ")
			TEXT("downstream errors.");

		RegisterTemplate(Template);
	}

	// ==========================================
	// 10. research_reference_patterns
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("research_reference_patterns");
		Template.Description = TEXT("Research workflow using library and reference templates to find proven patterns.");
		Template.Parameters = {
			{ TEXT("pattern_query"), TEXT("What pattern or technique to research (e.g., bow draw animation, inventory system)"), true },
			{ TEXT("domain"), TEXT("Optional domain to narrow the search (e.g., combat, UI, movement)"), false }
		};
		Template.TemplateText =
			TEXT("Research reference patterns and templates before building.\n\n")
			TEXT("Query: {{pattern_query}}\n")
			TEXT("Domain: {{domain}}\n\n")
			TEXT("Search for relevant patterns across multiple sources. Use specific keywords rather than ")
			TEXT("broad categories -- \"bow draw animation\" is better than \"weapon\".\n\n")
			TEXT("TEMPLATES: Search with blueprint.list_templates using your query. Results include library templates ")
			TEXT("(extracted from real projects, highest quality), factory templates ")
			TEXT("(hand-crafted parameterized patterns), and reference templates (architecture documentation).\n\n")
			TEXT("For promising matches, read the full template with blueprint.get_template. ")
			TEXT("Use the pattern parameter to read a specific function's node graph ")
			TEXT("(e.g., pattern=\"FireWeapon\") rather than loading the entire template.\n\n")
			TEXT("Library templates may use project-specific naming. ")
			TEXT("Adapt class names and variable names to fit the current project when using them as reference.\n\n")
			TEXT("RECIPES: Search olive.get_recipe for tested wiring patterns. ")
			TEXT("Recipes cover common tasks like interface calls, dispatcher binding, spawn patterns, ")
			TEXT("and input handling.\n\n")
			TEXT("COMMUNITY: Search olive.search_community_blueprints for broader examples. ")
			TEXT("Community blueprints have mixed quality -- compare several results before committing to a pattern. ")
			TEXT("Browse mode first to scan many results, then detail mode on promising matches.\n\n")
			TEXT("SYNTHESIZE: After gathering references, summarize what you found: which patterns are relevant, ")
			TEXT("what architecture they suggest, and how to adapt them to the current task. Then proceed to building.");

		RegisterTemplate(Template);
	}

	// ==========================================
	// 11. verify_and_finish
	// ==========================================
	{
		FOlivePromptTemplate Template;
		Template.Name = TEXT("verify_and_finish");
		Template.Description = TEXT("Verify a Blueprint is complete: compiles cleanly, graphs have logic, no unwired pins.");
		Template.Parameters = {
			{ TEXT("asset_path"), TEXT("Path to the Blueprint asset to verify"), true },
			{ TEXT("expected_functions"), TEXT("Comma-separated list of functions expected to contain graph logic"), false }
		};
		Template.TemplateText =
			TEXT("Verify a Blueprint is complete and report what was built.\n\n")
			TEXT("Asset: {{asset_path}}\n")
			TEXT("Expected functions: {{expected_functions}}\n\n")
			TEXT("COMPILE: Compile the Blueprint (blueprint.compile). If there are errors, ")
			TEXT("fix them using the fix_compile_errors workflow before proceeding.\n\n")
			TEXT("CHECK STRUCTURE: Read the Blueprint (blueprint.read mode \"full\"). Verify that:\n")
			TEXT("- All expected functions exist and contain graph logic (not just empty entry/result nodes)\n")
			TEXT("- All expected variables and components are present\n")
			TEXT("- Event handlers (BeginPlay, Tick, overlap events, etc.) are wired with logic if they were ")
			TEXT("part of the task\n\n")
			TEXT("CHECK GRAPHS: For each key function, read it back (blueprint.describe_function) and confirm ")
			TEXT("the execution flow makes sense. Look for:\n")
			TEXT("- Disconnected exec chains (nodes that exist but are not reachable from the entry point)\n")
			TEXT("- Data pins that should be wired but have no connection\n")
			TEXT("- Missing return values on functions that should output data\n\n")
			TEXT("REPORT: Summarize what was built:\n")
			TEXT("- Blueprint class and parent\n")
			TEXT("- Components added\n")
			TEXT("- Variables added\n")
			TEXT("- Functions implemented (brief description of each)\n")
			TEXT("- Any known limitations or things that could not be accomplished with available tools\n\n")
			TEXT("If something is incomplete, say what is missing and why. ")
			TEXT("Do not claim completion if graph logic is missing.");

		RegisterTemplate(Template);
	}
}
