// Copyright Bode Software. All Rights Reserved.

/**
 * OliveCLIProviderBase.cpp
 *
 * Implementation of the abstract CLI provider base class. Contains all universal
 * process management, prompt building, response parsing, and callback dispatching
 * shared by all CLI-based AI providers.
 */

#include "Providers/OliveCLIProviderBase.h"
#include "Providers/OliveCLIToolCallParser.h"
#include "Providers/OliveCLIToolSchemaSerializer.h"
#include "Settings/OliveAISettings.h"
#include "Chat/OlivePromptAssembler.h"
#include "Template/OliveTemplateSystem.h"
#include "MCP/OliveMCPServer.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Index/OliveProjectIndex.h"
#include "Services/OliveUtilityModel.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveCLIProvider, Log, All);

namespace
{
	/** Tier 1: nudge-kill after no tool call. Terminates process; auto-continue
	 *  relaunches with enriched continuation prompt. */
	constexpr double CLI_TOOL_IDLE_NUDGE_SECONDS = 120.0;

	/** Tier 2: hard kill. Safety net for genuinely hung processes that
	 *  fail to make progress even after a nudge-kill + relaunch. */
	constexpr double CLI_TOOL_IDLE_KILL_SECONDS = 300.0;

	/** Stdout idle: process produces no output at all (frozen/deadlocked). */
	constexpr double CLI_STDOUT_IDLE_SECONDS = 300.0;

	/** Determine tool prefixes needed for a given user message.
	 *  Used in autonomous mode to filter tools/list to only relevant domains. */
	TSet<FString> DetermineToolPrefixes(const FString& Message)
	{
		FString Lower = Message.ToLower();

		// Always-included core tools
		TSet<FString> Prefixes = {
			TEXT("project."),
			TEXT("olive."),
			TEXT("cross_system."),
		};

		bool bHasBlueprint = Lower.Contains(TEXT("blueprint")) || Lower.Contains(TEXT("actor"))
			|| Lower.Contains(TEXT("component")) || Lower.Contains(TEXT("variable"))
			|| Lower.Contains(TEXT("function")) || Lower.Contains(TEXT("event graph"));
		bool bHasBT = Lower.Contains(TEXT("behavior tree")) || Lower.Contains(TEXT("behaviour tree"))
			|| Lower.Contains(TEXT("blackboard")) || Lower.Contains(TEXT(" bt "))
			|| Lower.Contains(TEXT(" ai "));
		bool bHasPCG = Lower.Contains(TEXT("pcg")) || Lower.Contains(TEXT("procedural"));
		bool bHasCpp = Lower.Contains(TEXT("c++")) || Lower.Contains(TEXT("cpp"))
			|| Lower.Contains(TEXT("header")) || Lower.Contains(TEXT("source file"));

		int32 DomainCount = (bHasBlueprint ? 1 : 0) + (bHasBT ? 1 : 0)
			+ (bHasPCG ? 1 : 0) + (bHasCpp ? 1 : 0);

		// If multiple domains or none (ambiguous), return empty to show all tools
		if (DomainCount > 1)
		{
			return TSet<FString>(); // Empty = no filter
		}

		if (DomainCount == 0)
		{
			// Default: assume Blueprint (most common use case)
			Prefixes.Add(TEXT("blueprint."));
			Prefixes.Add(TEXT("animbp."));
			Prefixes.Add(TEXT("widget."));
			return Prefixes;
		}

		if (bHasBlueprint)
		{
			Prefixes.Add(TEXT("blueprint."));
			Prefixes.Add(TEXT("animbp."));
			Prefixes.Add(TEXT("widget."));
		}
		if (bHasBT)
		{
			Prefixes.Add(TEXT("bt."));
			Prefixes.Add(TEXT("blackboard."));
			// Also include Blueprint tools (BT tasks often reference BPs)
			Prefixes.Add(TEXT("blueprint."));
		}
		if (bHasPCG)
		{
			Prefixes.Add(TEXT("pcg."));
		}
		if (bHasCpp)
		{
			Prefixes.Add(TEXT("cpp."));
		}

		return Prefixes;
	}

	/** Check if a tool name represents a write/mutation operation.
	 *  Auto-continue should only trigger after write ops (genuine stalls after progress),
	 *  not after reads/recipes (the AI was thinking about what plan to write). */
	bool IsWriteOperation(const FString& ToolName)
	{
		FString OpPart = ToolName;
		int32 DotIdx;
		if (ToolName.FindChar(TEXT('.'), DotIdx))
		{
			OpPart = ToolName.Mid(DotIdx + 1);
		}

		return OpPart.StartsWith(TEXT("create")) || OpPart.StartsWith(TEXT("apply"))
			|| OpPart.StartsWith(TEXT("add")) || OpPart.StartsWith(TEXT("set_"))
			|| OpPart.StartsWith(TEXT("connect")) || OpPart.StartsWith(TEXT("disconnect"))
			|| OpPart.StartsWith(TEXT("remove")) || OpPart.StartsWith(TEXT("delete"))
			|| OpPart.StartsWith(TEXT("rename")) || OpPart.StartsWith(TEXT("reparent"))
			|| OpPart.StartsWith(TEXT("modify")) || OpPart.StartsWith(TEXT("override"))
			|| OpPart.Contains(TEXT("compile")) || OpPart.Contains(TEXT("batch_write"));
	}

	/** Check if a tool call is a scaffolding operation (structural setup before graph logic).
	 *  When the AI has done scaffolding work in a run, it's about to plan graph logic
	 *  and needs extended thinking time — the adaptive idle timeout uses this signal. */
	bool IsScaffoldingOperation(const FString& ToolName)
	{
		return ToolName.Contains(TEXT("add_component"))
			|| ToolName.Contains(TEXT("add_variable"))
			|| ToolName.Contains(TEXT("modify_component"))
			|| ToolName.Contains(TEXT("create_from_template"));
	}

	/** Info about a function graph that has no logic (just entry point). */
	struct FEmptyFunctionInfo
	{
		FString AssetPath;
		FString FunctionName;
		int32 NodeCount = 0;
		TArray<FString> Inputs;   // "ParamName:TypeName" pairs
		TArray<FString> Outputs;  // "ParamName:TypeName" pairs
		bool bHasCrossAssetDeps = false;
	};

	/** Info about an event graph with very sparse logic. */
	struct FSparseEventGraphInfo
	{
		FString AssetPath;
		FString GraphName;
		int32 NodeCount = 0;
	};

	/**
	 * Extract a human-readable type name from a pin's type info.
	 * Prefers the sub-category object name (e.g., "Vector", "MyEnum") over
	 * the raw pin category (e.g., "struct", "byte").
	 */
	FString GetPinTypeName(const UEdGraphPin* Pin)
	{
		if (UObject* SubCatObj = Pin->PinType.PinSubCategoryObject.Get())
		{
			return SubCatObj->GetName();
		}
		return Pin->PinType.PinCategory.ToString();
	}

	/**
	 * Scan modified assets for empty function graphs and sparse event graphs.
	 * MUST be called on the game thread (loads UObject packages).
	 *
	 * @param AssetPaths            Asset paths to scan
	 * @param OutEmptyFunctions     Populated with functions that have no logic (<=2 nodes)
	 * @param OutSparseEventGraphs  Populated with event graphs that have <=3 nodes
	 */
	void ScanEmptyFunctionGraphs(
		const TArray<FString>& AssetPaths,
		TArray<FEmptyFunctionInfo>& OutEmptyFunctions,
		TArray<FSparseEventGraphInfo>& OutSparseEventGraphs)
	{
		// Collect all modified asset class names for cross-dep detection
		TSet<FString> ModifiedAssetClassNames;

		for (const FString& AssetPath : AssetPaths)
		{
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
			if (!BP) continue;
			if (BP->GeneratedClass)
			{
				ModifiedAssetClassNames.Add(BP->GeneratedClass->GetName());
			}
		}

		for (const FString& AssetPath : AssetPaths)
		{
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
			if (!BP) continue;

			// Scan function graphs
			for (UEdGraph* Graph : BP->FunctionGraphs)
			{
				if (!Graph) continue;
				if (Graph->Nodes.Num() > 2) continue;  // Has logic beyond entry/result stubs

				FEmptyFunctionInfo Info;
				Info.AssetPath = AssetPath;
				Info.FunctionName = Graph->GetName();
				Info.NodeCount = Graph->Nodes.Num();

				// Extract function signature from FunctionEntry/FunctionResult nodes
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
					{
						for (UEdGraphPin* Pin : Entry->Pins)
						{
							if (Pin && !Pin->bHidden && Pin->Direction == EGPD_Output
								&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
							{
								FString TypeName = GetPinTypeName(Pin);
								Info.Inputs.Add(FString::Printf(TEXT("%s:%s"),
									*Pin->PinName.ToString(), *TypeName));

								// Cross-asset dep check: does this param reference another
								// modified Blueprint's class?
								if (UObject* PinClass = Pin->PinType.PinSubCategoryObject.Get())
								{
									if (ModifiedAssetClassNames.Contains(PinClass->GetName()))
									{
										Info.bHasCrossAssetDeps = true;
									}
								}
							}
						}
					}
					else if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
					{
						for (UEdGraphPin* Pin : Result->Pins)
						{
							if (Pin && !Pin->bHidden && Pin->Direction == EGPD_Input
								&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
							{
								FString TypeName = GetPinTypeName(Pin);
								Info.Outputs.Add(FString::Printf(TEXT("%s:%s"),
									*Pin->PinName.ToString(), *TypeName));
							}
						}
					}
				}

				OutEmptyFunctions.Add(MoveTemp(Info));
			}

			// Scan event graphs (UbergraphPages) for sparse logic
			for (UEdGraph* Graph : BP->UbergraphPages)
			{
				if (!Graph) continue;
				if (Graph->Nodes.Num() <= 3)
				{
					FSparseEventGraphInfo EventInfo;
					EventInfo.AssetPath = AssetPath;
					EventInfo.GraphName = Graph->GetName();
					EventInfo.NodeCount = Graph->Nodes.Num();
					OutSparseEventGraphs.Add(MoveTemp(EventInfo));
				}
			}
		}

		// Sort empty functions: non-cross-asset first (simpler deps), then by
		// total parameter count ascending (simpler signatures first).
		OutEmptyFunctions.Sort([](const FEmptyFunctionInfo& A, const FEmptyFunctionInfo& B)
		{
			if (A.bHasCrossAssetDeps != B.bHasCrossAssetDeps)
				return !A.bHasCrossAssetDeps;  // non-cross-asset first
			return A.Inputs.Num() + A.Outputs.Num() < B.Inputs.Num() + B.Outputs.Num();
		});
	}

	/** Quick heuristic for messages that imply write/mutation intent. */
	bool MessageImpliesMutation(const FString& Message)
	{
		const FString Lower = Message.ToLower();
		return Lower.Contains(TEXT("add")) || Lower.Contains(TEXT("create"))
			|| Lower.Contains(TEXT("edit")) || Lower.Contains(TEXT("modify"))
			|| Lower.Contains(TEXT("update")) || Lower.Contains(TEXT("change"))
			|| Lower.Contains(TEXT("rename")) || Lower.Contains(TEXT("connect"))
			|| Lower.Contains(TEXT("wire")) || Lower.Contains(TEXT("set "))
			|| Lower.Contains(TEXT("remove")) || Lower.Contains(TEXT("delete"))
			|| Lower.Contains(TEXT("refactor")) || Lower.Contains(TEXT("make "))
			|| Lower.Contains(TEXT("build")) || Lower.Contains(TEXT("implement"))
			|| Lower.Contains(TEXT("spawn"));
	}
}

// ==========================================
// FOliveCLIReaderRunnable
// ==========================================

FOliveCLIReaderRunnable::FOliveCLIReaderRunnable(
	void* InReadPipe,
	FThreadSafeBool& InStopFlag,
	TFunction<void(const FString&)> InOnLine
)
	: ReadPipe(InReadPipe)
	, bStop(InStopFlag)
	, OnLine(InOnLine)
{
}

uint32 FOliveCLIReaderRunnable::Run()
{
	FString LineBuffer;

	while (!bStop)
	{
		FString Output = FPlatformProcess::ReadPipe(ReadPipe);
		if (!Output.IsEmpty())
		{
			LineBuffer += Output;

			// Process complete lines
			int32 NewlineIndex;
			while (LineBuffer.FindChar('\n', NewlineIndex))
			{
				FString Line = LineBuffer.Left(NewlineIndex);
				LineBuffer = LineBuffer.Mid(NewlineIndex + 1);

				Line.TrimStartAndEndInline();
				if (!Line.IsEmpty())
				{
					OnLine(Line);
				}
			}
		}
		else
		{
			// Small sleep to avoid busy loop when no data
			FPlatformProcess::Sleep(0.01f);
		}
	}

	// Process any remaining data
	if (!LineBuffer.IsEmpty())
	{
		OnLine(LineBuffer);
	}

	return 0;
}

void FOliveCLIReaderRunnable::Stop()
{
	bStop = true;
}

// ==========================================
// FOliveCLIProviderBase
// ==========================================

FOliveCLIProviderBase::~FOliveCLIProviderBase()
{
	// Signal all captured lambdas that `this` is about to become invalid.
	// Must happen BEFORE KillProcess() because KillProcess may not wait for
	// all queued game-thread AsyncTasks to drain.
	*AliveGuard = false;

	// Clean up MCP tool call delegate to prevent dangling callback
	if (ToolCallDelegateHandle.IsValid())
	{
		FOliveMCPServer::Get().OnToolCalled.Remove(ToolCallDelegateHandle);
		ToolCallDelegateHandle.Reset();
	}

	KillProcess();
}

void FOliveCLIProviderBase::Configure(const FOliveProviderConfig& Config)
{
	CurrentConfig = Config;
}

void FOliveCLIProviderBase::ParseOutputLine(const FString& Line)
{
	// Default no-op. Subclasses override for provider-specific format parsing.
	// The base class accumulates all non-parsed lines as plain text chunks.
	FScopeLock Lock(&CallbackLock);
	FOliveStreamChunk Chunk;
	Chunk.Text = Line;
	AccumulatedResponse += Line + TEXT("\n");
	CurrentOnChunk.ExecuteIfBound(Chunk);
}

FString FOliveCLIProviderBase::GetWorkingDirectory() const
{
	return WorkingDirectory;
}

bool FOliveCLIProviderBase::RequiresNodeRunner() const
{
	FString ExePath = GetExecutablePath();
	return ExePath.EndsWith(TEXT(".js"));
}

FString FOliveCLIProviderBase::GetCLIName() const
{
	return TEXT("CLI");
}

FString FOliveCLIProviderBase::GetCLIArgumentsAutonomous() const
{
	// Default: delegate to the standard argument builder with no system prompt.
	// Subclasses should override with autonomous-specific flags (e.g., no --strict-mcp-config,
	// higher --max-turns ceiling for self-directed tool loops).
	return GetCLIArguments(TEXT(""));
}

void FOliveCLIProviderBase::SendMessage(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools,
	FOnOliveStreamChunk OnChunk,
	FOnOliveToolCall OnToolCall,
	FOnOliveComplete OnComplete,
	FOnOliveError OnError,
	const FOliveRequestOptions& Options
)
{
	if (bIsBusy)
	{
		OnError.ExecuteIfBound(TEXT("Request already in progress"));
		return;
	}

	// Validate
	FString ValidationError;
	if (!ValidateConfig(ValidationError))
	{
		OnError.ExecuteIfBound(ValidationError);
		return;
	}

	// Store callbacks (including OnToolCall which is orchestrated-specific)
	{
		FScopeLock Lock(&CallbackLock);
		CurrentOnChunk = OnChunk;
		CurrentOnToolCall = OnToolCall;
		CurrentOnComplete = OnComplete;
		CurrentOnError = OnError;
	}

	bIsBusy = true;
	++RequestGeneration;
	AccumulatedResponse.Empty();

	// Build prompt and system prompt on the game thread (prompt assembler accesses UObject settings)
	FString Prompt = BuildConversationPrompt(Messages, Tools);
	FString SystemPromptText = BuildCLISystemPrompt(Prompt, Tools);

	// Escape system prompt for command line
	FString EscapedSystemPrompt = SystemPromptText;
	EscapedSystemPrompt.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscapedSystemPrompt.ReplaceInline(TEXT("\""), TEXT("\\\""));

	// Build the system prompt argument fragment
	FString SystemPromptArg;
	if (!EscapedSystemPrompt.IsEmpty())
	{
		SystemPromptArg = FString::Printf(TEXT("--append-system-prompt \"%s\" "), *EscapedSystemPrompt);
	}

	// Get provider-specific CLI arguments
	FString CLIArgs = GetCLIArguments(SystemPromptArg);

	UE_LOG(LogOliveCLIProvider, Log, TEXT("System prompt injected: %d chars"), SystemPromptText.Len());

	// Delegate to shared process lifecycle, with orchestrated completion handler
	LaunchCLIProcess(CLIArgs, Prompt, [this](int32 ReturnCode)
	{
		HandleResponseComplete(ReturnCode);
	});
}

void FOliveCLIProviderBase::SetupAutonomousSandbox()
{
	// Create sandbox in Saved/ (gitignored by default, UE convention for runtime files)
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	AutonomousSandboxDir = FPaths::Combine(ProjectDir, TEXT("Saved/OliveAI/AgentSandbox"));
	IFileManager::Get().MakeDirectory(*AutonomousSandboxDir, true);

	// --- Build agent context (shared across all CLI providers) ---
	const FString PluginDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
	const FString KnowledgeDir = FPaths::Combine(PluginDir, TEXT("Content/SystemPrompts/Knowledge"));

	FString BlueprintKnowledge;
	if (!FFileHelper::LoadFileToString(BlueprintKnowledge, *FPaths::Combine(KnowledgeDir, TEXT("cli_blueprint.txt"))))
	{
		UE_LOG(LogOliveCLIProvider, Warning, TEXT("Failed to load cli_blueprint.txt knowledge pack"));
	}

	FString RecipeRouting;
	if (!FFileHelper::LoadFileToString(RecipeRouting, *FPaths::Combine(KnowledgeDir, TEXT("recipe_routing.txt"))))
	{
		UE_LOG(LogOliveCLIProvider, Warning, TEXT("Failed to load recipe_routing.txt knowledge pack"));
	}

	FString DesignPatterns;
	if (!FFileHelper::LoadFileToString(DesignPatterns, *FPaths::Combine(KnowledgeDir, TEXT("blueprint_design_patterns.txt"))))
	{
		UE_LOG(LogOliveCLIProvider, Warning, TEXT("Failed to load blueprint_design_patterns.txt knowledge pack"));
	}

	FString EventsVsFunctions;
	if (!FFileHelper::LoadFileToString(EventsVsFunctions, *FPaths::Combine(KnowledgeDir, TEXT("events_vs_functions.txt"))))
	{
		UE_LOG(LogOliveCLIProvider, Warning, TEXT("Failed to load events_vs_functions.txt knowledge pack"));
	}

	FString NodeRouting;
	if (!FFileHelper::LoadFileToString(NodeRouting, *FPaths::Combine(KnowledgeDir, TEXT("node_routing.txt"))))
	{
		UE_LOG(LogOliveCLIProvider, Warning, TEXT("Failed to load node_routing.txt knowledge pack"));
	}

	FString AgentContext;
	AgentContext += TEXT("# Olive AI Studio - Agent Context\n\n");
	AgentContext += TEXT("You are an AI assistant integrated with Unreal Engine 5.5 via Olive AI Studio.\n");
	AgentContext += TEXT("Your job is to help users create and modify game assets (Blueprints, Behavior Trees, PCG graphs, etc.) using the MCP tools provided.\n\n");
	AgentContext += TEXT("## Critical Rules\n");
	AgentContext += TEXT("- You are NOT a plugin developer. Do NOT modify plugin source code.\n");
	AgentContext += TEXT("- Use ONLY the MCP tools to create and edit game assets.\n");
	AgentContext += TEXT("- All asset paths should be under `/Game/` (the project's Content directory).\n");
	AgentContext += TEXT("- When creating Blueprints, use `blueprint.create` (with optional template_id for templates) -- never try to create .uasset files manually.\n");
	AgentContext += TEXT("- Complete the FULL task: create structures, wire graph logic, compile, and verify. Do not stop partway.\n");
	AgentContext += TEXT("- After each compile pass, ask yourself: 'Have I built everything the user asked for?' If not, continue building the next part.\n");
	AgentContext += TEXT("- Before finishing, verify you built EVERY part the user asked for — don't stop after the first Blueprint compiles.\n");
	AgentContext += TEXT("- Batch independent tool calls (add_variable, add_component) in a single response when possible.\n");
	AgentContext += TEXT("- After creating from a template (blueprint.create with template_id), check the result for the list of created functions. Write plan_json for EACH function -- they are empty stubs. Do NOT call blueprint.read or read_function after template creation.\n\n");

	AgentContext += TEXT("## Planning\n\n");
	AgentContext += TEXT("For multi-asset tasks, plan before building. Ask:\n");
	AgentContext += TEXT("- \"Does this thing exist in the world with its own transform?\" -> separate Blueprint\n");
	AgentContext += TEXT("- \"Is it a value on an existing actor?\" -> variable\n");
	AgentContext += TEXT("- \"Is it a capability attached to many actors?\" -> component\n\n");
	AgentContext += TEXT("Common decomposition: weapons, projectiles, doors, keys, vehicles = always separate actors.\n");
	AgentContext += TEXT("After listing your assets, identify how they communicate (interfaces, dispatchers, casts,\n");
	AgentContext += TEXT("overlap events). See Blueprint Design Patterns for details.\n\n");

	AgentContext += TEXT("## Research\n\n");
	AgentContext += TEXT("Research tools help you verify assumptions before writing graph logic:\n");
	AgentContext += TEXT("- `blueprint.list_templates(query=\"...\")` -- search library/factory templates for patterns\n");
	AgentContext += TEXT("- `blueprint.get_template(id, pattern=\"FuncName\")` -- read specific function implementations\n");
	AgentContext += TEXT("- `blueprint.describe_function(function_name, target_class)` -- verify function exists and get pin signatures\n");
	AgentContext += TEXT("- `blueprint.describe_node_type(type)` -- check K2Node properties and pins\n");
	AgentContext += TEXT("- `project.search(query)` -- find existing assets by name\n");
	AgentContext += TEXT("- `olive.get_recipe(query)` -- tested wiring patterns for common tasks\n\n");
	AgentContext += TEXT("Research when you are unsure. Skip research when you are confident in your UE5 knowledge.\n\n");

	AgentContext += TEXT("## Building\n\n");
	AgentContext += TEXT("Three approaches -- use whichever fits, mix freely:\n");
	AgentContext += TEXT("1. plan_json -- batch declarative, best for standard logic (3+ nodes)\n");
	AgentContext += TEXT("2. Granular tools (add_node, connect_pins) -- any UK2Node, best for edge cases\n");
	AgentContext += TEXT("3. editor.run_python -- full UE editor API, best for anything tools can't express\n\n");
	AgentContext += TEXT("Build one asset at a time: structure -> function signatures -> compile structure ->\n");
	AgentContext += TEXT("graph logic -> compile to 0 errors -> next asset.\n\n");

	AgentContext += TEXT("## Self-Correction\n");
	AgentContext += TEXT("- Fix the FIRST compile error before moving on\n");
	AgentContext += TEXT("- After a plan_json failure, all nodes from that plan are rolled back.\n");
	AgentContext += TEXT("  Do NOT reference node IDs from a failed plan.\n");
	AgentContext += TEXT("- If one approach fails twice, try a different tool or technique\n");
	AgentContext += TEXT("- If something genuinely cannot be done, tell the user what and why\n\n");

	if (!BlueprintKnowledge.IsEmpty())
	{
		AgentContext += TEXT("---\n\n");
		AgentContext += BlueprintKnowledge;
		AgentContext += TEXT("\n\n");
	}

	if (!RecipeRouting.IsEmpty())
	{
		AgentContext += TEXT("---\n\n");
		AgentContext += RecipeRouting;
		AgentContext += TEXT("\n\n");
	}

	if (!DesignPatterns.IsEmpty())
	{
		AgentContext += TEXT("---\n\n");
		AgentContext += DesignPatterns;
		AgentContext += TEXT("\n\n");
	}

	if (!EventsVsFunctions.IsEmpty())
	{
		AgentContext += TEXT("---\n\n");
		AgentContext += EventsVsFunctions;
		AgentContext += TEXT("\n\n");
	}

	if (!NodeRouting.IsEmpty())
	{
		AgentContext += TEXT("---\n\n");
		AgentContext += NodeRouting;
		AgentContext += TEXT("\n\n");
	}

	// --- Prescriptive guidance for non-Anthropic providers ---
	// Claude models deeply understand UE5 APIs and tool schemas from training.
	// Other models (GPT, Gemini) need explicit rules to avoid common failure patterns
	// observed in testing: pin name guessing, rate limit hammering, granular-tool spirals.
	if (!IsAnthropicProvider())
	{
		UE_LOG(LogOliveCLIProvider, Log, TEXT("Non-Anthropic provider detected (%s) — appending prescriptive tool guidance to AGENTS.md"), *GetCLIName());
		AgentContext += TEXT("\n---\n\n");
		AgentContext += BuildPrescriptiveGuidance();
	}
	else
	{
		UE_LOG(LogOliveCLIProvider, Log, TEXT("Anthropic provider (%s) — skipping prescriptive guidance"), *GetCLIName());
	}

	// --- Write AGENTS.md (read by all CLI providers: Claude, Codex, Gemini, etc.) ---
	const FString SandboxAgentsPath = FPaths::Combine(AutonomousSandboxDir, TEXT("AGENTS.md"));
	FFileHelper::SaveStringToFile(AgentContext, *SandboxAgentsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	// --- Provider-specific sandbox files (e.g., .mcp.json for Claude, no-op for Codex) ---
	WriteProviderSpecificSandboxFiles(AgentContext);

	UE_LOG(LogOliveCLIProvider, Log, TEXT("Autonomous sandbox created at: %s"), *AutonomousSandboxDir);
}

void FOliveCLIProviderBase::WriteProviderSpecificSandboxFiles(const FString& AgentContext)
{
	// Default: no-op. Subclasses override to write provider-specific files.
}

FString FOliveCLIProviderBase::BuildPrescriptiveGuidance() const
{
	FString G;

	G += TEXT("# MANDATORY Tool Usage Rules\n\n");
	G += TEXT("These rules are NON-NEGOTIABLE. Violating them causes broken Blueprints.\n\n");

	// === Rule 1: plan_json is mandatory for multi-node graphs ===
	G += TEXT("## Rule 1: Use plan_json for Graph Logic (NOT add_node + connect_pins)\n\n");
	G += TEXT("For ANY graph logic with 2+ connected nodes, you MUST use `blueprint.preview_plan_json` followed by `blueprint.apply_plan_json`.\n");
	G += TEXT("plan_json handles ALL pin wiring automatically via `@step.auto` syntax.\n");
	G += TEXT("Do NOT use `add_node` + `connect_pins` for standard logic — you WILL get pin names wrong.\n\n");
	G += TEXT("Only use add_node + connect_pins for:\n");
	G += TEXT("- Wiring a SINGLE connection between existing nodes\n");
	G += TEXT("- Node types outside plan_json ops vocabulary\n\n");

	// === Rule 2: plan_json example ===
	G += TEXT("## Rule 2: plan_json Format (Follow Exactly)\n\n");
	G += TEXT("```json\n");
	G += TEXT("{\n");
	G += TEXT("  \"schema_version\": \"2.0\",\n");
	G += TEXT("  \"steps\": [\n");
	G += TEXT("    {\"step_id\": \"evt\", \"op\": \"event\", \"target\": \"BeginPlay\"},\n");
	G += TEXT("    {\"step_id\": \"get_hp\", \"op\": \"get_var\", \"target\": \"Health\"},\n");
	G += TEXT("    {\"step_id\": \"check\", \"op\": \"branch\", \"inputs\": {\"Condition\": \"@get_hp.auto\"}, \"exec_after\": \"evt\"},\n");
	G += TEXT("    {\"step_id\": \"print\", \"op\": \"print_string\", \"inputs\": {\"InString\": \"Alive!\"}, \"exec_after\": \"check.true\"}\n");
	G += TEXT("  ]\n");
	G += TEXT("}\n");
	G += TEXT("```\n\n");
	G += TEXT("Key syntax:\n");
	G += TEXT("- `@step_id.auto` — auto-wire output of that step (plan_json resolves the correct pin)\n");
	G += TEXT("- `@step_id.PinName` — wire a specific output pin\n");
	G += TEXT("- `exec_after` — exec wiring: step_id, or step_id.true / step_id.false for branches\n");
	G += TEXT("- `@entry.ParamName` — wire from function input parameter\n");
	G += TEXT("- String literals go directly: `\"InString\": \"Hello\"`\n");
	G += TEXT("- Numeric literals: `\"Amount\": \"100.0\"`\n\n");
	G += TEXT("Available ops: event, custom_event, call, get_var, set_var, branch, sequence, cast,\n");
	G += TEXT("for_loop, for_each_loop, while_loop, do_once, flip_flop, gate, delay, is_valid,\n");
	G += TEXT("print_string, spawn_actor, make_struct, break_struct, return, comment,\n");
	G += TEXT("call_delegate, call_dispatcher, bind_dispatcher\n\n");
	G += TEXT("IMPORTANT: Always call `blueprint.preview_plan_json` first, then `blueprint.apply_plan_json`\n");
	G += TEXT("with the fingerprint from the preview. NEVER call both in the same response.\n\n");

	// === Rule 3: NEVER guess pin names ===
	G += TEXT("## Rule 3: NEVER Guess Pin Names\n\n");
	G += TEXT("If you must use `connect_pins`, you MUST call `blueprint.get_node_pins` first.\n");
	G += TEXT("Pin names in Unreal Engine are NOT the same as function parameter names.\n\n");
	G += TEXT("Common mistakes:\n");
	G += TEXT("- VariableGet nodes have NO exec pins (no `then`, no `execute`). They only have data output.\n");
	G += TEXT("- VariableSet nodes have exec pins (`execute` input, `then` output) plus data pins.\n");
	G += TEXT("- CallFunction `then` is the exec output. `execute` is the exec input.\n");
	G += TEXT("- Pin names often have spaces: `Return Value`, `Inventory Items`, `World Context Object`.\n");
	G += TEXT("- Output pin names do NOT match input parameter names on other nodes.\n\n");

	// === Rule 4: Batching limits ===
	G += TEXT("## Rule 4: Limit Write Operations Per Turn\n\n");
	G += TEXT("The server enforces a rate limit of 30 write operations per 60 seconds.\n");
	G += TEXT("If you batch too many writes, they will be rejected with RATE_LIMITED.\n\n");
	G += TEXT("Rules:\n");
	G += TEXT("- Maximum 8 write tool calls per turn (add_variable, add_component, add_node, etc.)\n");
	G += TEXT("- Read operations (blueprint.read, describe_function, get_node_pins) are unlimited\n");
	G += TEXT("- If you get RATE_LIMITED, wait the suggested seconds, then retry\n");
	G += TEXT("- Do NOT use shell sleep commands to wait — just make fewer calls per turn\n\n");

	// === Rule 5: Common UE class name pitfalls ===
	G += TEXT("## Rule 5: Correct UE5 Class Names\n\n");
	G += TEXT("Common wrong names (these WILL fail):\n");
	G += TEXT("- `SystemLibrary` is wrong — use `KismetSystemLibrary`\n");
	G += TEXT("- `MathLibrary` is wrong — use `KismetMathLibrary`\n");
	G += TEXT("- `StringLibrary` is wrong — use `KismetStringLibrary`\n");
	G += TEXT("- `ArrayLibrary` is wrong — use `KismetArrayLibrary`\n");
	G += TEXT("- `GameplayStatics` (correct as-is)\n");
	G += TEXT("- `WidgetBlueprintLibrary` (correct as-is)\n\n");
	G += TEXT("When unsure, call `blueprint.describe_function(function_name)` WITHOUT target_class.\n");
	G += TEXT("The server searches all known library classes automatically.\n\n");

	// === Rule 6: Workflow order ===
	G += TEXT("## Rule 6: Build Order (Follow Strictly)\n\n");
	G += TEXT("1. `blueprint.create` — create the Blueprint\n");
	G += TEXT("2. `blueprint.add_variable` / `blueprint.add_component` — add structure\n");
	G += TEXT("3. `blueprint.add_function` — create function signatures (with parameters/return types)\n");
	G += TEXT("4. `blueprint.compile` — compile structure before adding graph logic\n");
	G += TEXT("5. `blueprint.preview_plan_json` then `blueprint.apply_plan_json` — add graph logic per function\n");
	G += TEXT("6. `blueprint.compile` — final compile, fix errors\n\n");
	G += TEXT("NEVER skip step 4. Compiling structure first ensures variables and functions resolve in plan_json.\n\n");

	// === Rule 7: Error recovery ===
	G += TEXT("## Rule 7: Error Recovery\n\n");
	G += TEXT("When plan_json fails:\n");
	G += TEXT("- All nodes from that plan are ROLLED BACK (you cannot reference them)\n");
	G += TEXT("- Read the error message carefully — it tells you exactly what went wrong\n");
	G += TEXT("- Fix the issue in your plan and try again\n");
	G += TEXT("- Do NOT fall back to add_node + connect_pins as a workaround\n\n");
	G += TEXT("When connect_pins fails with 'pin not found':\n");
	G += TEXT("- The error shows available pins — use THOSE exact names\n");
	G += TEXT("- Call `blueprint.get_node_pins` to see all pins on a node\n");
	G += TEXT("- Do NOT retry with the same wrong pin name\n\n");

	return G;
}

void FOliveCLIProviderBase::SendMessageAutonomous(
	const FString& UserMessage,
	FOnOliveStreamChunk OnChunk,
	FOnOliveComplete OnComplete,
	FOnOliveError OnError)
{
	if (bIsBusy)
	{
		OnError.ExecuteIfBound(TEXT("Request already in progress"));
		return;
	}

	// Capture auto-continue state before consuming the flag.
	// bIsAutoContinuation is set by HandleResponseCompleteAutonomous before
	// dispatching an auto-continue. All other entry paths leave it false.
	const bool bWasAutoContinuation = bIsAutoContinuation;
	if (bIsAutoContinuation)
	{
		bIsAutoContinuation = false; // Consume the flag
	}
	else
	{
		AutoContinueCount = 0; // User-initiated = fresh budget
	}

	// Validate
	FString ValidationError;
	if (!ValidateConfig(ValidationError))
	{
		OnError.ExecuteIfBound(ValidationError);
		return;
	}

	// Store callbacks (no OnToolCall -- tools go through MCP server in autonomous mode)
	{
		FScopeLock Lock(&CallbackLock);
		CurrentOnChunk = OnChunk;
		CurrentOnToolCall.Unbind();
		CurrentOnComplete = OnComplete;
		CurrentOnError = OnError;
	}

	bIsBusy = true;
	++RequestGeneration;
	AccumulatedResponse.Empty();

	// Set up autonomous sandbox with agent-specific CLAUDE.md and .mcp.json
	// so the CLI reads the correct role context instead of the developer CLAUDE.md
	SetupAutonomousSandbox();

	// Enrich continuation messages with context from the previous run.
	// This must happen AFTER SetupAutonomousSandbox (which writes CLAUDE.md)
	// but BEFORE LaunchCLIProcess (which delivers the message via stdin).
	// bWasAutoContinuation is true when we're auto-continuing (timeout, zero-tool, reviewer) —
	// those messages are already enriched by BuildContinuationPrompt() at the call site.
	const bool bIsContinuation = bWasAutoContinuation || IsContinuationMessage(UserMessage);
	FString EffectiveMessage = UserMessage;
	if (!bWasAutoContinuation && LastRunContext.bValid && IsContinuationMessage(UserMessage))
	{
		EffectiveMessage = BuildContinuationPrompt(UserMessage);
		UE_LOG(LogOliveCLIProvider, Log,
			TEXT("Continuation detected: enriched prompt with %d modified assets from previous run"),
			LastRunContext.ModifiedAssetPaths.Num());
	}

	// Inject @-mentioned asset state into the initial prompt so the AI
	// doesn't need to re-read assets it's already been pointed at.
	// Must run on the game thread (BuildAssetStateSummary loads UObjects).
	if (InitialContextAssetPaths.Num() > 0 && !bIsContinuation)
	{
		FString AssetState = BuildAssetStateSummary(InitialContextAssetPaths);
		if (!AssetState.IsEmpty())
		{
			EffectiveMessage += TEXT("\n\n");
			EffectiveMessage += AssetState;
			EffectiveMessage += TEXT("\n**Do NOT re-read these assets** -- their current state is shown above. Focus on making the requested changes.\n");

			UE_LOG(LogOliveCLIProvider, Log,
				TEXT("Injected @-mention asset state for %d assets into initial prompt"),
				InitialContextAssetPaths.Num());
		}
		InitialContextAssetPaths.Empty(); // Consume -- only inject once per user message
	}

	// Pre-populate related asset context: extract keywords from the user message
	// and search the project index for potentially relevant existing assets.
	// This gives the AI awareness of what already exists without needing a search tool call.
	if (!bIsContinuation && FOliveProjectIndex::Get().IsReady())
	{
		TArray<FString> Keywords = ExtractKeywordsFromMessage(UserMessage);
		if (Keywords.Num() > 0)
		{
			TSet<FString> AlreadySeen;
			TArray<FOliveAssetInfo> RelatedAssets;

			for (const FString& Keyword : Keywords)
			{
				TArray<FOliveAssetInfo> Results = FOliveProjectIndex::Get().SearchAssets(Keyword, 5);
				for (const FOliveAssetInfo& Result : Results)
				{
					if (!AlreadySeen.Contains(Result.Path))
					{
						AlreadySeen.Add(Result.Path);
						RelatedAssets.Add(Result);
					}
				}
			}

			if (RelatedAssets.Num() > 0)
			{
				// Cap at 10 to avoid prompt bloat
				const int32 MaxRelated = FMath::Min(RelatedAssets.Num(), 10);
				EffectiveMessage += TEXT("\n\n## Existing Assets That May Be Relevant\n");
				for (int32 i = 0; i < MaxRelated; ++i)
				{
					EffectiveMessage += FString::Printf(TEXT("- %s (%s)\n"),
						*RelatedAssets[i].Path, *RelatedAssets[i].AssetClass.ToString());
				}
				EffectiveMessage += TEXT("Use project.search if you need more details on any of these.\n");

				UE_LOG(LogOliveCLIProvider, Log,
					TEXT("Injected %d related assets from keyword search (keywords: %s)"),
					MaxRelated, *FString::Join(Keywords, TEXT(", ")));
			}
		}
	}

	// Helper to emit a status message through the stream callback so the chat UI
	// shows progress during the discovery phase.
	auto EmitStatus = [this](const FString& StatusText)
	{
		FScopeLock Lock(&CallbackLock);
		if (CurrentOnChunk.IsBound())
		{
			FOliveStreamChunk StatusChunk;
			StatusChunk.Text = StatusText + TEXT("\n");
			CurrentOnChunk.Execute(StatusChunk);
		}
	};

	// Template discovery pass -- pre-search library/factory/community templates
	// using utility model for smart keyword generation.
	if (!bIsContinuation)
	{
		const UOliveAISettings* DiscoverySettings = UOliveAISettings::Get();
		if (DiscoverySettings && DiscoverySettings->bEnableTemplateDiscoveryPass)
		{
			EmitStatus(TEXT("*Searching for relevant templates and assets...*"));

			const FString& DiscoveryInput =
				(LastRunContext.bValid && !LastRunContext.OriginalMessage.IsEmpty())
				? LastRunContext.OriginalMessage
				: UserMessage;
			FOliveDiscoveryResult Discovery = FOliveUtilityModel::RunDiscoveryPass(DiscoveryInput);
			FString DiscoveryBlock = FOliveUtilityModel::FormatDiscoveryForPrompt(Discovery);

			if (!DiscoveryBlock.IsEmpty())
			{
				EffectiveMessage += TEXT("\n\n");
				EffectiveMessage += DiscoveryBlock;

				UE_LOG(LogOliveCLIProvider, Log,
					TEXT("Discovery pass: %d results in %.1fs (LLM=%s, queries: %s)"),
					Discovery.Entries.Num(),
					Discovery.ElapsedSeconds,
					Discovery.bUsedLLM ? TEXT("yes") : TEXT("no"),
					*FString::Join(Discovery.SearchQueries, TEXT("; ")));
			}
		}
	}

	// Structured decomposition directive.
	if (!bIsContinuation && MessageImpliesMutation(UserMessage))
	{
		EmitStatus(TEXT("*Launching builder...*"));

		EffectiveMessage += TEXT("\n\n## Task Approach\n\n");
		EffectiveMessage += TEXT("Think through what Blueprints you need:\n");
		EffectiveMessage += TEXT("- Separate actor for anything with its own transform (weapons, projectiles, etc.)\n");
		EffectiveMessage += TEXT("- Component for reusable capabilities\n");
		EffectiveMessage += TEXT("- Variable for simple values on existing actors\n\n");
		EffectiveMessage += TEXT("Then build each one fully: structure -> graph logic -> compile to 0 errors -> next.\n");
		EffectiveMessage += TEXT("If unsure whether a UE function exists (e.g., component-specific functions), verify with blueprint.describe_function before writing plan_json.\n");
	}

	// Guardrail: for write-oriented tasks, require at least one tool call before final text.
	if (!bIsContinuation && MessageImpliesMutation(UserMessage))
	{
		EffectiveMessage += TEXT("\n\n## Tool Execution Requirement\n");
		EffectiveMessage += TEXT("Before any final explanation text, execute at least one MCP tool call that makes concrete progress.\n");
		EffectiveMessage += TEXT("If no tool call is needed, explicitly explain why in one sentence.\n");
	}

	// Initialize run context tracking for this new run
	LastRunContext.Reset();
	LastRunContext.OriginalMessage = UserMessage;
	bLastRunTimedOut = false;
	bLastRunWasRuntimeLimit = false;

	// Set tool filter based on message content (autonomous mode only).
	// Uses the original user message (not continuation prompt) for consistent filtering.
	TSet<FString> ToolPrefixes = DetermineToolPrefixes(LastRunContext.OriginalMessage);
	if (ToolPrefixes.Num() > 0)
	{
		FOliveMCPServer::Get().SetToolFilter(ToolPrefixes);
	}
	// else: empty set = no filter, show all tools

	// Autonomous mode: no system prompt escaping, no BuildCLISystemPrompt.
	// The CLI discovers tools via MCP and reads the sandbox CLAUDE.md for domain context.
	FString CLIArgs = GetCLIArgumentsAutonomous();

	UE_LOG(LogOliveCLIProvider, Log, TEXT("Launching autonomous CLI with args: %s"), *CLIArgs);

	// Subscribe to MCP tool call events for activity-based timeout tracking
	// and tool call logging for continuation context.
	// Uses the AliveGuard pattern to safely update the atomic timestamp from
	// the game thread (OnToolCalled fires on game thread) while the background
	// read loop checks it via std::atomic<double>.
	LastToolCallTimestamp.store(FPlatformTime::Seconds());
	ScaffoldingOpCount.store(0);
	RecipeCallCount.store(0);
	if (ToolCallDelegateHandle.IsValid())
	{
		FOliveMCPServer::Get().OnToolCalled.Remove(ToolCallDelegateHandle);
	}
	TSharedPtr<FThreadSafeBool> Guard = AliveGuard;
	ToolCallDelegateHandle = FOliveMCPServer::Get().OnToolCalled.AddLambda(
		[this, Guard](const FString& ToolName, const FString& ClientId, const TSharedPtr<FJsonObject>& Arguments)
		{
			if (*Guard)
			{
				LastToolCallTimestamp.store(FPlatformTime::Seconds());

				// Track complexity signals for adaptive idle timeout
				if (IsScaffoldingOperation(ToolName)) { ScaffoldingOpCount.fetch_add(1); }
				if (ToolName == TEXT("olive.get_recipe")) { RecipeCallCount.fetch_add(1); }

				// Track tool call for continuation context
				FAutonomousRunContext::FToolCallEntry Entry;
				Entry.ToolName = ToolName;

				// Extract asset_path from arguments (most tools have this)
				if (Arguments.IsValid())
				{
					FString AssetPath;
					if (Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) ||
						Arguments->TryGetStringField(TEXT("path"), AssetPath))
					{
						Entry.AssetPath = AssetPath;
						if (!LastRunContext.ModifiedAssetPaths.Contains(AssetPath))
						{
							LastRunContext.ModifiedAssetPaths.Add(AssetPath);
						}
					}
				}

				// Track recipe and template fetches for continuation prompt
				if (ToolName == TEXT("olive.get_recipe") && Arguments.IsValid())
				{
					FString RecipeName;
					if (Arguments->TryGetStringField(TEXT("name"), RecipeName) && !RecipeName.IsEmpty())
					{
						LastRunContext.FetchedRecipeNames.AddUnique(RecipeName);
					}
				}
				else if (ToolName == TEXT("blueprint.get_template") && Arguments.IsValid())
				{
					FString TemplateId;
					if (Arguments->TryGetStringField(TEXT("template_id"), TemplateId) && !TemplateId.IsEmpty())
					{
						LastRunContext.FetchedTemplateIds.AddUnique(TemplateId);
					}
				}

				// Cap tool call log at 50 entries to keep continuation prompts bounded
				if (LastRunContext.ToolCallLog.Num() < 50)
				{
					LastRunContext.ToolCallLog.Add(MoveTemp(Entry));
				}
			}
		});

	// Delegate to shared process lifecycle, with autonomous completion handler.
	// Pass the sandbox directory so the CLI launches from there instead of the plugin source dir.
	LaunchCLIProcess(CLIArgs, EffectiveMessage, [this](int32 ReturnCode)
	{
		HandleResponseCompleteAutonomous(ReturnCode);
	}, AutonomousSandboxDir);
}

void FOliveCLIProviderBase::LaunchCLIProcess(
	const FString& CLIArgs,
	const FString& StdinContent,
	TFunction<void(int32)> OnProcessExit,
	const FString& WorkingDirectoryOverride)
{
	// Capture the current generation at launch time. All async dispatches in this
	// process lifecycle check against this value to discard stale completions.
	const uint32 ThisGeneration = RequestGeneration.load();

	// Capture CLIName for use in the background lambda (avoids calling virtual in destructor race)
	const FString CLIName = GetCLIName();

	// Capture alive-guard by value (shared copy). The guard survives past `this`
	// destruction, allowing queued lambdas to detect that the provider is gone
	// and bail out before touching any member.
	TSharedPtr<FThreadSafeBool> Guard = AliveGuard;

	// Spawn process on background thread
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, Guard, CLIArgs, StdinContent, CLIName, ThisGeneration, OnProcessExit = MoveTemp(OnProcessExit), WorkingDirectoryOverride]()
	{
		// Early-exit if provider was destroyed before we even started
		if (!*Guard) return;

		FString ExePath = GetExecutablePath();
		if (ExePath.IsEmpty())
		{
			AsyncTask(ENamedThreads::GameThread, [this, Guard, CLIName]()
			{
				if (!*Guard) return;
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("%s CLI not found"), *CLIName));
			});
			return;
		}

		// Determine executable and args based on whether Node.js runner is needed
		FString Executable;
		FString Args;

		if (ExePath.EndsWith(TEXT(".js")))
		{
			// Run with Node.js directly - much more reliable than .cmd files
			Executable = TEXT("node");
			Args = FString::Printf(TEXT("\"%s\" %s"), *ExePath, *CLIArgs);
		}
		else if (ExePath.EndsWith(TEXT(".exe")))
		{
			// Direct executable
			Executable = ExePath;
			Args = CLIArgs;
		}
		else
		{
			// Fallback for other cases (Unix binaries, etc.)
			Executable = ExePath;
			Args = CLIArgs;
		}

		const FString ProcessWorkDir = WorkingDirectoryOverride.IsEmpty() ? GetWorkingDirectory() : WorkingDirectoryOverride;
		UE_LOG(LogOliveCLIProvider, Log, TEXT("Running: %s %s"), *Executable, *Args);
		UE_LOG(LogOliveCLIProvider, Log, TEXT("Working Directory: %s"), *ProcessWorkDir);

		// Create pipes for process communication
		void* StdoutRead = nullptr;
		void* StdoutWrite = nullptr;

		if (!FPlatformProcess::CreatePipe(StdoutRead, StdoutWrite))
		{
			AsyncTask(ENamedThreads::GameThread, [this, Guard, CLIName]()
			{
				if (!*Guard) return;
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("Failed to create stdout pipe for %s process"), *CLIName));
			});
			return;
		}

		// Create stdin pipe for delivering content instead of the -p CLI argument.
		// This avoids the Windows ~32KB command-line length limit that causes crashes
		// when the conversation history grows large during agentic loop iterations.
		// bWritePipeLocal=true makes the write end non-inheritable (parent keeps it)
		// and the read end inheritable (child gets it).
		void* StdinRead = nullptr;
		void* StdinWrite = nullptr;

		if (!FPlatformProcess::CreatePipe(StdinRead, StdinWrite, /*bWritePipeLocal=*/true))
		{
			FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
			AsyncTask(ENamedThreads::GameThread, [this, Guard, CLIName]()
			{
				if (!*Guard) return;
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("Failed to create stdin pipe for %s process"), *CLIName));
			});
			return;
		}

		// Spawn the process
		uint32 ProcessId;
		ProcessHandle = FPlatformProcess::CreateProc(
			*Executable,
			*Args,
			false,  // bLaunchDetached
			true,   // bLaunchHidden
			true,   // bLaunchReallyHidden
			&ProcessId,
			0,      // PriorityModifier
			*ProcessWorkDir,
			StdoutWrite,  // stdout pipe (child writes, parent reads)
			StdinRead     // stdin pipe (child reads, parent writes)
		);

		if (!ProcessHandle.IsValid())
		{
			FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
			FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
			AsyncTask(ENamedThreads::GameThread, [this, Guard, CLIName]()
			{
				if (!*Guard) return;
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("Failed to spawn %s process"), *CLIName));
			});
			return;
		}

		// Close write end of stdout pipe (we only read from it)
		FPlatformProcess::ClosePipe(nullptr, StdoutWrite);

		// Close read end of stdin pipe (we only write to it)
		FPlatformProcess::ClosePipe(StdinRead, nullptr);
		StdinRead = nullptr;

		// Deliver content via stdin if non-empty. The FString overload appends a
		// trailing newline, which is harmless since CLI processes read stdin to EOF.
		if (!StdinContent.IsEmpty())
		{
			FPlatformProcess::WritePipe(StdinWrite, StdinContent);
			UE_LOG(LogOliveCLIProvider, Log, TEXT("Stdin content delivered: %d chars"), StdinContent.Len());
		}

		// Close write end of stdin to signal EOF. Without this the child blocks
		// forever waiting for more input.
		FPlatformProcess::ClosePipe(nullptr, StdinWrite);
		StdinWrite = nullptr;

		// Read output inline
		bStopReading = false;
		FString OutputBuffer;
		double LastOutputTime = FPlatformTime::Seconds();
		const double ProcessStartTime = FPlatformTime::Seconds();
		int32 AccumulatedOutputChars = 0; // All stdout chars received this run
		bool bNudgeKillIssued = false; // True after tier-1 nudge-kill fired

		// Read max runtime from settings (0 = no limit). This is primarily a cost-control
		// safety net for autonomous mode, but applies universally since orchestrated turns
		// complete in seconds and will never hit a reasonable limit.
		const UOliveAISettings* RuntimeSettings = UOliveAISettings::Get();
		const double MaxRuntimeSeconds = RuntimeSettings ? static_cast<double>(RuntimeSettings->AutonomousMaxRuntimeSeconds) : 300.0;

		// Nudge threshold from settings (AutonomousIdleToolSeconds), clamped to a
		// minimum of 60s so users can't accidentally make the agent unusable.
		const double NudgeSeconds = RuntimeSettings
			? FMath::Max(static_cast<double>(RuntimeSettings->AutonomousIdleToolSeconds), 60.0)
			: CLI_TOOL_IDLE_NUDGE_SECONDS;

		while (FPlatformProcess::IsProcRunning(ProcessHandle) && !bStopReading)
		{
			FString Chunk = FPlatformProcess::ReadPipe(StdoutRead);
			if (!Chunk.IsEmpty())
			{
				LastOutputTime = FPlatformTime::Seconds();
				AccumulatedOutputChars += Chunk.Len();
				OutputBuffer += Chunk;

				// Process complete lines
				int32 NewlineIdx;
				while (OutputBuffer.FindChar('\n', NewlineIdx))
				{
					FString Line = OutputBuffer.Left(NewlineIdx).TrimStartAndEnd();
					OutputBuffer = OutputBuffer.Mid(NewlineIdx + 1);

					if (!Line.IsEmpty())
					{
						// Dispatch line parsing to game thread via virtual ParseOutputLine
						AsyncTask(ENamedThreads::GameThread, [this, Guard, Line, ThisGeneration]()
						{
							if (!*Guard) return;
							FScopeLock Lock(&CallbackLock);
							if (!bIsBusy || RequestGeneration != ThisGeneration) return;
							ParseOutputLine(Line);
						});
					}
				}
			}
			else
			{
				// Stdout idle timeout: kill if process produces no output at all
				// (frozen/deadlocked). Uses a generous 300s since the process may
				// be genuinely thinking with tool calls flowing through MCP.
				const double StdoutIdleTimeout = CLI_STDOUT_IDLE_SECONDS;

				if (FPlatformTime::Seconds() - LastOutputTime > StdoutIdleTimeout)
				{
					UE_LOG(LogOliveCLIProvider, Warning,
						TEXT("%s process idle for %.0f seconds (limit=%.0fs) - terminating"),
						*CLIName, FPlatformTime::Seconds() - LastOutputTime, StdoutIdleTimeout);
					bLastRunTimedOut = true;
					bStopReading = true;
					if (*Guard)
					{
						FPlatformProcess::TerminateProc(ProcessHandle, true);
					}
					break;
				}
				FPlatformProcess::Sleep(0.01f);
			}

			// Two-tier activity-based timeout: catches "thinking but not acting"
			// scenarios where stdout is flowing (so the stdout idle timeout above
			// doesn't trigger) but no MCP tool calls are being made.
			//
			// Tier 1 (nudge-kill): terminates the process after NudgeSeconds of no
			//   tool call. bLastRunTimedOut triggers auto-continue, which relaunches
			//   with an enriched continuation prompt.
			// Tier 2 (hard-kill): if the process is STILL running (shouldn't happen
			//   normally, but covers edge cases) after CLI_TOOL_IDLE_KILL_SECONDS,
			//   terminates unconditionally.
			//
			// LastToolCallTimestamp is std::atomic<double>, written on game thread by
			// OnToolCalled delegate, read here on background thread -- safe without lock.
			const double LastToolCall = LastToolCallTimestamp.load();
			if (LastToolCall > 0.0)
			{
				const double TimeSinceLastTool = FPlatformTime::Seconds() - LastToolCall;

				if (TimeSinceLastTool > CLI_TOOL_IDLE_KILL_SECONDS)
				{
					// Tier 2: hard kill -- genuinely hung
					UE_LOG(LogOliveCLIProvider, Warning,
						TEXT("%s process: no MCP tool call in %.0f seconds (hard limit=%.0fs) - terminating"),
						*CLIName, TimeSinceLastTool, CLI_TOOL_IDLE_KILL_SECONDS);
					bLastRunTimedOut = true;
					bStopReading = true;
					if (*Guard)
					{
						FPlatformProcess::TerminateProc(ProcessHandle, true);
					}
					break;
				}
				else if (TimeSinceLastTool > NudgeSeconds && !bNudgeKillIssued)
				{
					// Tier 1: nudge-kill -- agent thinking too long, relaunch
					// with enriched prompt via auto-continue
					UE_LOG(LogOliveCLIProvider, Warning,
						TEXT("%s process: no MCP tool call in %.0f seconds (nudge limit=%.0fs) - nudge kill (will auto-continue)"),
						*CLIName, TimeSinceLastTool, NudgeSeconds);
					bNudgeKillIssued = true;
					bLastRunTimedOut = true;
					bStopReading = true;
					if (*Guard)
					{
						FPlatformProcess::TerminateProc(ProcessHandle, true);
					}
					break;
				}
			}

			// Total runtime limit (cost control for autonomous mode).
			// Checked every iteration regardless of data flow. A value of 0 disables
			// the limit. Orchestrated turns complete in seconds and won't hit this.
			if (MaxRuntimeSeconds > 0.0 && (FPlatformTime::Seconds() - ProcessStartTime) > MaxRuntimeSeconds)
			{
				UE_LOG(LogOliveCLIProvider, Warning, TEXT("%s process exceeded total runtime limit (%.0f seconds) - terminating"), *CLIName, MaxRuntimeSeconds);
				bLastRunTimedOut = true;
				bLastRunWasRuntimeLimit = true;
				bStopReading = true;
				if (*Guard)
				{
					FPlatformProcess::TerminateProc(ProcessHandle, true);
				}
				break;
			}
		}

		// Read any remaining output after process exit
		while (true)
		{
			FString Chunk = FPlatformProcess::ReadPipe(StdoutRead);
			if (!Chunk.IsEmpty())
			{
				OutputBuffer += Chunk;
			}
			else
			{
				break;
			}
		}

		// Process remaining buffer
		if (!OutputBuffer.IsEmpty())
		{
			AsyncTask(ENamedThreads::GameThread, [this, Guard, OutputBuffer, ThisGeneration]()
			{
				if (!*Guard) return;
				FScopeLock Lock(&CallbackLock);
				if (!bIsBusy || RequestGeneration != ThisGeneration) return;
				ParseOutputLine(OutputBuffer);
			});
		}

		// Cleanup stdout pipe (local handle, safe regardless of provider lifetime)
		FPlatformProcess::ClosePipe(StdoutRead, nullptr);

		// Get return code and close process handle.
		// Guard check: if the provider was destroyed, the destructor's KillProcess()
		// already called TerminateProc + CloseProc on the member ProcessHandle.
		// Skip cleanup here to avoid double-close.
		int32 ReturnCode = -1;
		if (*Guard)
		{
			FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
			FPlatformProcess::CloseProc(ProcessHandle);
		}

		// Signal completion via caller-provided exit handler
		AsyncTask(ENamedThreads::GameThread, [this, Guard, ReturnCode, ThisGeneration, OnProcessExit]()
		{
			if (!*Guard) return;
			FScopeLock Lock(&CallbackLock);
			if (!bIsBusy || RequestGeneration != ThisGeneration) return;
			OnProcessExit(ReturnCode);
		});
	});
}

void FOliveCLIProviderBase::HandleResponseComplete(int32 ReturnCode)
{
	// Called under CallbackLock, while bIsBusy is true.
	// Bridges CLI text output to ConversationManager's agentic loop by parsing
	// <tool_call> blocks from accumulated text and emitting them via OnToolCall.

	if (ReturnCode != 0 && AccumulatedResponse.IsEmpty())
	{
		bIsBusy = false;
		// CRITICAL: Keep this exact error format -- OliveProviderRetryManager::ClassifyError
		// matches "process exited with code" to detect process crashes.
		CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("%s process exited with code %d"), *GetCLIName(), ReturnCode));
		return;
	}

	// Parse tool calls from accumulated text
	TArray<FOliveStreamChunk> ParsedToolCalls;
	FString CleanedText;
	bool bHasToolCalls = FOliveCLIToolCallParser::Parse(AccumulatedResponse, ParsedToolCalls, CleanedText);

	if (bHasToolCalls)
	{
		// Emit each tool call via OnToolCall -- ConversationManager collects these
		for (const FOliveStreamChunk& ToolCall : ParsedToolCalls)
		{
			UE_LOG(LogOliveCLIProvider, Log, TEXT("Parsed tool call: %s (id: %s)"), *ToolCall.ToolName, *ToolCall.ToolCallId);
			CurrentOnToolCall.ExecuteIfBound(ToolCall);
		}
	}

	FOliveProviderUsage Usage;
	Usage.Model = CurrentConfig.ModelId.IsEmpty() ? FString::Printf(TEXT("%s-cli"), *GetCLIName().ToLower()) : CurrentConfig.ModelId;
	Usage.FinishReason = bHasToolCalls ? TEXT("tool_calls") : TEXT("stop");
	bIsBusy = false;
	CurrentOnComplete.ExecuteIfBound(CleanedText, Usage);
}

void FOliveCLIProviderBase::HandleResponseCompleteAutonomous(int32 ReturnCode)
{
	// Called under CallbackLock, while bIsBusy is true.
	// Autonomous mode: no <tool_call> parsing needed -- tools were executed via MCP server.
	// Simply emit the accumulated response text.

	// Clean up MCP tool call delegate
	if (ToolCallDelegateHandle.IsValid())
	{
		FOliveMCPServer::Get().OnToolCalled.Remove(ToolCallDelegateHandle);
		ToolCallDelegateHandle.Reset();
	}

	// Clear tool filter (must happen before potential auto-continue which re-sets it)
	FOliveMCPServer::Get().ClearToolFilter();

	// Capture run context for potential continuation.
	// bLastRunTimedOut is set on the background thread before process termination;
	// we read it here on the game thread after the process has exited.
	LastRunContext.bValid = true;
	if (bLastRunTimedOut)
	{
		if (bLastRunWasRuntimeLimit)
			LastRunContext.Outcome = FAutonomousRunContext::EOutcome::RuntimeLimit;
		else
			LastRunContext.Outcome = FAutonomousRunContext::EOutcome::IdleTimeout;
	}
	// else stays Completed (the default set in Reset())

	// Unified timeout handler: any idle timeout -> decomposition nudge.
	// One nudge restart is allowed; if it also times out, report to user.
	if (bLastRunTimedOut && AutoContinueCount < MaxAutoContinues)
	{
		AutoContinueCount++;

		UE_LOG(LogOliveCLIProvider, Log,
			TEXT("Run timed out (attempt %d/%d) — relaunching with decomposition nudge"),
			AutoContinueCount, MaxAutoContinues);

		bIsBusy = false;

		FOnOliveStreamChunk SavedOnChunk = CurrentOnChunk;
		FOnOliveComplete SavedOnComplete = CurrentOnComplete;
		FOnOliveError SavedOnError = CurrentOnError;

		bIsAutoContinuation = true;

		AsyncTask(ENamedThreads::GameThread, [this,
			SavedOnChunk, SavedOnComplete, SavedOnError]()
		{
			if (!(*AliveGuard))
			{
				return;
			}

			FString NudgePrompt = BuildContinuationPrompt(
				TEXT("The task is too large to plan at once. Break the remaining work into small steps and execute them one at a time. Start with the first step now."));
			SendMessageAutonomous(NudgePrompt, SavedOnChunk, SavedOnComplete, SavedOnError);
		});

		return;
	}

	// Log activity stats for diagnostic purposes
	const double LastTool = LastToolCallTimestamp.load();
	UE_LOG(LogOliveCLIProvider, Log,
		TEXT("Autonomous run complete (exit code %d): last tool call %.1fs ago, accumulated %d chars, %d tool calls logged"),
		ReturnCode,
		LastTool > 0.0 ? FPlatformTime::Seconds() - LastTool : -1.0,
		AccumulatedResponse.Len(),
		LastRunContext.ToolCallLog.Num());

	// Guardrail: successful exit with no MCP tool activity is treated as a failed autonomous run.
	// Retry once with a strict nudge, then report explicit error instead of silent text-only success.
	if (ReturnCode == 0 && LastRunContext.ToolCallLog.Num() == 0)
	{
		if (AutoContinueCount < MaxAutoContinues)
		{
			AutoContinueCount++;

			UE_LOG(LogOliveCLIProvider, Warning,
				TEXT("Autonomous run produced no tool calls (attempt %d/%d) - retrying with strict tool-use nudge"),
				AutoContinueCount, MaxAutoContinues);

			bIsBusy = false;

			FOnOliveStreamChunk SavedOnChunk = CurrentOnChunk;
			FOnOliveComplete SavedOnComplete = CurrentOnComplete;
			FOnOliveError SavedOnError = CurrentOnError;

			bIsAutoContinuation = true;

			AsyncTask(ENamedThreads::GameThread, [this, SavedOnChunk, SavedOnComplete, SavedOnError]()
			{
				if (!(*AliveGuard))
				{
					return;
				}

				FString NudgePrompt = BuildContinuationPrompt(
					TEXT("You made zero MCP tool calls. Make your first tool call immediately. Do not provide explanation-only text."));
				SendMessageAutonomous(NudgePrompt, SavedOnChunk, SavedOnComplete, SavedOnError);
			});

			return;
		}

		bIsBusy = false;
		CurrentOnError.ExecuteIfBound(FString::Printf(
			TEXT("%s run completed without MCP tool calls. The task was not executed. Retry with a more explicit actionable request."),
			*GetCLIName()));
		return;
	}

	if (ReturnCode != 0 && AccumulatedResponse.IsEmpty())
	{
		bIsBusy = false;
		// CRITICAL: Keep this exact error format -- OliveProviderRetryManager::ClassifyError
		// matches "process exited with code" to detect process crashes.
		CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("%s process exited with code %d"), *GetCLIName(), ReturnCode));
		return;
	}

	FOliveProviderUsage Usage;
	Usage.Model = CurrentConfig.ModelId.IsEmpty() ? FString::Printf(TEXT("%s-cli"), *GetCLIName().ToLower()) : CurrentConfig.ModelId;
	Usage.FinishReason = TEXT("stop");
	bIsBusy = false;
	CurrentOnComplete.ExecuteIfBound(AccumulatedResponse, Usage);
}

TArray<FString> FOliveCLIProviderBase::ExtractKeywordsFromMessage(const FString& Message) const
{
	// Simple tokenizer: split on spaces/punctuation, keep words 4+ chars,
	// filter out common stop words and UE jargon that would match too broadly.
	static const TSet<FString> StopWords = {
		TEXT("the"), TEXT("and"), TEXT("for"), TEXT("with"), TEXT("that"),
		TEXT("this"), TEXT("from"), TEXT("have"), TEXT("make"), TEXT("create"),
		TEXT("add"), TEXT("set"), TEXT("get"), TEXT("use"), TEXT("when"),
		TEXT("blueprint"), TEXT("actor"), TEXT("component"), TEXT("variable"),
		TEXT("function"), TEXT("event"), TEXT("unreal"), TEXT("game"),
		TEXT("player"), TEXT("system"), TEXT("class"), TEXT("type"),
		TEXT("need"), TEXT("want"), TEXT("like"), TEXT("should"), TEXT("would"),
		TEXT("could"), TEXT("into"), TEXT("also"), TEXT("each"), TEXT("every"),
		TEXT("some"), TEXT("other"), TEXT("then"), TEXT("than"), TEXT("just"),
	};

	TArray<FString> Words;
	Message.ParseIntoArray(Words, TEXT(" "), true);

	TArray<FString> Keywords;
	for (FString& Word : Words)
	{
		// Strip @-mention prefix and punctuation
		Word.RemoveFromStart(TEXT("@"));
		Word = Word.TrimStartAndEnd();

		// Remove trailing punctuation
		while (Word.Len() > 0 && !FChar::IsAlnum(Word[Word.Len() - 1]))
		{
			Word.LeftChopInline(1);
		}

		Word = Word.ToLower();

		if (Word.Len() >= 3 && !StopWords.Contains(Word))
		{
			Keywords.AddUnique(Word);
		}
	}

	// Cap keywords to avoid excessive searches
	if (Keywords.Num() > 5)
	{
		Keywords.SetNum(5);
	}

	return Keywords;
}

bool FOliveCLIProviderBase::IsContinuationMessage(const FString& Message) const
{
	FString Lower = Message.ToLower().TrimStartAndEnd();

	// Exact match for common continuation phrases
	if (Lower == TEXT("continue") ||
		Lower == TEXT("keep going") ||
		Lower == TEXT("finish") ||
		Lower == TEXT("finish the task") ||
		Lower == TEXT("keep working") ||
		Lower == TEXT("resume"))
	{
		return true;
	}

	// Prefix match for phrases with additional context (e.g., "continue building the gun")
	if (Lower.StartsWith(TEXT("continue ")) ||
		Lower.StartsWith(TEXT("keep going")) ||
		Lower.StartsWith(TEXT("finish ")))
	{
		return true;
	}

	return false;
}

FString FOliveCLIProviderBase::BuildContinuationPrompt(const FString& UserMessage) const
{
	FString Prompt;

	// Header
	Prompt += TEXT("## Continuation of Previous Task\n\n");

	// Original task
	Prompt += TEXT("### Original Task\n");
	Prompt += LastRunContext.OriginalMessage;
	Prompt += TEXT("\n\n");

	// What was done
	Prompt += TEXT("### What Was Already Done\n");
	if (LastRunContext.ToolCallLog.Num() > 0)
	{
		// Group by asset for readability
		TMap<FString, TArray<FString>> ByAsset;
		for (const FAutonomousRunContext::FToolCallEntry& Entry : LastRunContext.ToolCallLog)
		{
			FString Key = Entry.AssetPath.IsEmpty() ? TEXT("(general)") : Entry.AssetPath;
			ByAsset.FindOrAdd(Key).Add(Entry.ToolName);
		}

		for (const auto& Pair : ByAsset)
		{
			Prompt += FString::Printf(TEXT("- %s: "), *Pair.Key);

			// Deduplicate consecutive identical tool names for brevity
			TArray<FString> Condensed;
			FString LastTool;
			int32 Count = 0;
			for (const FString& Tool : Pair.Value)
			{
				if (Tool == LastTool)
				{
					Count++;
				}
				else
				{
					if (!LastTool.IsEmpty())
					{
						Condensed.Add(Count > 1 ?
							FString::Printf(TEXT("%s x%d"), *LastTool, Count) : LastTool);
					}
					LastTool = Tool;
					Count = 1;
				}
			}
			if (!LastTool.IsEmpty())
			{
				Condensed.Add(Count > 1 ?
					FString::Printf(TEXT("%s x%d"), *LastTool, Count) : LastTool);
			}

			Prompt += FString::Join(Condensed, TEXT(", "));
			Prompt += TEXT("\n");
		}
	}
	else
	{
		Prompt += TEXT("No tool calls were recorded from the previous run.\n");
	}

	// Note previously fetched resources for context (not mandated)
	if (LastRunContext.FetchedRecipeNames.Num() > 0 || LastRunContext.FetchedTemplateIds.Num() > 0)
	{
		Prompt += TEXT("\n### Previously Fetched Resources\n");
		Prompt += TEXT("The previous run read these resources. Re-fetch only if you need them:\n");
		for (const FString& Name : LastRunContext.FetchedRecipeNames)
		{
			Prompt += FString::Printf(TEXT("- `olive.get_recipe` name=\"%s\"\n"), *Name);
		}
		for (const FString& Id : LastRunContext.FetchedTemplateIds)
		{
			Prompt += FString::Printf(TEXT("- `blueprint.get_template` template_id=\"%s\"\n"), *Id);
		}
	}

	// Run outcome
	Prompt += TEXT("\n### Previous Run Outcome\n");
	switch (LastRunContext.Outcome)
	{
	case FAutonomousRunContext::EOutcome::IdleTimeout:
		Prompt += TEXT("The previous run TIMED OUT (600s of no output). Break the remaining work into smaller steps and execute them one at a time.\n");
		break;
	case FAutonomousRunContext::EOutcome::RuntimeLimit:
		Prompt += TEXT("The previous run hit the runtime limit. The task is incomplete.\n");
		break;
	case FAutonomousRunContext::EOutcome::OutputStall:
		Prompt += TEXT("The previous run FROZE: you gathered all context and began writing a response, ");
		Prompt += TEXT("then stopped before calling any tools. Do NOT re-read anything — you already have ");
		Prompt += TEXT("everything you need. Start your FIRST tool call immediately. No preamble.\n");
		break;
	case FAutonomousRunContext::EOutcome::Completed:
		Prompt += TEXT("The previous run completed normally. ");
		Prompt += TEXT("The user wants you to continue or finish remaining work.\n");
		break;
	}

	// Asset state summary (pre-read on game thread so the AI doesn't need to re-read)
	FString AssetState = BuildAssetStateSummary();
	if (!AssetState.IsEmpty())
	{
		Prompt += TEXT("\n");
		Prompt += AssetState;
	}

	// Scan for empty function graphs and sparse event graphs to build a concrete directive.
	// This runs on the game thread (same as BuildAssetStateSummary above).
	TArray<FEmptyFunctionInfo> EmptyGraphs;
	TArray<FSparseEventGraphInfo> SparseEventGraphs;
	ScanEmptyFunctionGraphs(LastRunContext.ModifiedAssetPaths, EmptyGraphs, SparseEventGraphs);

	// Empty function graph listing with signatures
	if (EmptyGraphs.Num() > 0)
	{
		Prompt += TEXT("\n### Remaining Work: Empty Function Graphs\n\n");
		Prompt += TEXT("EMPTY (no logic -- write these with apply_plan_json):\n");

		/** Show full signatures for the first few, names-only for the rest. */
		constexpr int32 MaxDetailedEntries = 3;
		int32 CharBudget = 4000;

		for (int32 i = 0; i < EmptyGraphs.Num() && CharBudget > 0; i++)
		{
			const FEmptyFunctionInfo& Info = EmptyGraphs[i];
			FString AssetName = FPaths::GetBaseFilename(Info.AssetPath);

			FString Line;
			if (i < MaxDetailedEntries)
			{
				// Full detail: index, name, signature, cross-dep flag
				Line = FString::Printf(TEXT("%d. %s::%s"), i + 1, *AssetName, *Info.FunctionName);
				if (Info.Inputs.Num() > 0)
				{
					Line += TEXT(" (") + FString::Join(Info.Inputs, TEXT(", ")) + TEXT(")");
				}
				else
				{
					Line += TEXT(" (no inputs)");
				}
				if (Info.Outputs.Num() > 0)
				{
					Line += TEXT(" -> ") + FString::Join(Info.Outputs, TEXT(", "));
				}
				if (Info.bHasCrossAssetDeps)
				{
					Line += TEXT(" [cross-asset deps]");
				}
			}
			else
			{
				// Name only for entries beyond the detailed threshold
				Line = FString::Printf(TEXT("%d. %s::%s"), i + 1, *AssetName, *Info.FunctionName);
			}

			Line += TEXT("\n");
			CharBudget -= Line.Len();
			if (CharBudget > 0)
			{
				Prompt += Line;
			}
		}
	}

	// Sparse event graph note
	if (SparseEventGraphs.Num() > 0)
	{
		Prompt += TEXT("\nSPARSE EVENT GRAPHS (<=3 nodes -- likely need logic):\n");
		for (const FSparseEventGraphInfo& Info : SparseEventGraphs)
		{
			FString AssetName = FPaths::GetBaseFilename(Info.AssetPath);
			Prompt += FString::Printf(TEXT("- %s::%s (%d nodes)\n"),
				*AssetName, *Info.GraphName, Info.NodeCount);
		}
	}

	// Action directive -- concrete "do THIS first" when empty functions are known
	Prompt += TEXT("\n### Your Task Now\n\n");
	if (EmptyGraphs.Num() > 0)
	{
		FString FirstAsset = FPaths::GetBaseFilename(EmptyGraphs[0].AssetPath);
		Prompt += FString::Printf(
			TEXT("Write the logic for %s::%s FIRST using apply_plan_json.\n"),
			*FirstAsset, *EmptyGraphs[0].FunctionName);
		Prompt += TEXT("Then write the next empty function. Compile after each.\n");
		Prompt += TEXT("Do NOT re-read these assets -- their current state is shown above.\n");
	}
	else if (LastRunContext.ModifiedAssetPaths.Num() > 0)
	{
		Prompt += TEXT("All functions have graph logic. Compile each Blueprint and verify 0 errors.\n");
		Prompt += TEXT("If errors exist, fix them. If all clean, the task may be complete.\n");
		Prompt += TEXT("Do NOT re-read these assets -- their current state is shown above.\n");
	}
	else
	{
		Prompt += TEXT("1. Determine what still needs to be done to complete the original task.\n");
		Prompt += TEXT("2. Complete the remaining work.\n");
	}

	// Include the user's continuation message if it has substantive context.
	// Skip bare continuation phrases (continue, keep going, etc.) since
	// the sections above already convey the intent. Always include longer messages
	// (e.g., decomposition nudges from auto-continue, or user instructions like
	// "continue building the gun").
	FString Trimmed = UserMessage.TrimStartAndEnd();
	if (!Trimmed.IsEmpty() &&
		!Trimmed.Equals(TEXT("continue"), ESearchCase::IgnoreCase) &&
		!Trimmed.Equals(TEXT("keep going"), ESearchCase::IgnoreCase) &&
		!Trimmed.Equals(TEXT("finish"), ESearchCase::IgnoreCase) &&
		!Trimmed.Equals(TEXT("resume"), ESearchCase::IgnoreCase))
	{
		Prompt += TEXT("\n### Additional Instructions\n");
		Prompt += Trimmed;
		Prompt += TEXT("\n");
	}

	return Prompt;
}

FString FOliveCLIProviderBase::BuildAssetStateSummary(const TArray<FString>& AssetPaths) const
{
	check(IsInGameThread());

	if (AssetPaths.Num() == 0)
	{
		return FString();
	}

	FString Summary;
	Summary += TEXT("### Current Asset State\n");

	for (const FString& AssetPath : AssetPaths)
	{
		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
		UBlueprint* Blueprint = Cast<UBlueprint>(Asset);

		if (!Blueprint)
		{
			Summary += FString::Printf(TEXT("\n**%s** — %s\n"),
				*AssetPath, Asset ? TEXT("non-Blueprint asset") : TEXT("not found"));
			continue;
		}

		Summary += FString::Printf(TEXT("\n**%s** (parent: %s)\n"),
			*AssetPath,
			Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("unknown"));

		// Components
		if (Blueprint->SimpleConstructionScript)
		{
			const TArray<USCS_Node*>& Nodes = Blueprint->SimpleConstructionScript->GetAllNodes();
			if (Nodes.Num() > 0)
			{
				Summary += TEXT("- Components: ");
				TArray<FString> CompEntries;
				for (const USCS_Node* Node : Nodes)
				{
					if (Node && Node->ComponentClass)
					{
						CompEntries.Add(FString::Printf(TEXT("%s (%s)"),
							*Node->GetVariableName().ToString(),
							*Node->ComponentClass->GetName()));
					}
				}
				Summary += FString::Join(CompEntries, TEXT(", "));
				Summary += TEXT("\n");
			}
		}

		// Variables
		if (Blueprint->NewVariables.Num() > 0)
		{
			Summary += TEXT("- Variables: ");
			TArray<FString> VarEntries;
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				VarEntries.Add(FString::Printf(TEXT("%s (%s)"),
					*Var.VarName.ToString(),
					*Var.VarType.PinCategory.ToString()));
			}
			Summary += FString::Join(VarEntries, TEXT(", "));
			Summary += TEXT("\n");
		}

		// Event Dispatchers
		if (Blueprint->DelegateSignatureGraphs.Num() > 0)
		{
			Summary += TEXT("- Event Dispatchers: ");
			TArray<FString> DispNames;
			for (const UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
			{
				if (Graph) { DispNames.Add(Graph->GetName()); }
			}
			Summary += FString::Join(DispNames, TEXT(", "));
			Summary += TEXT("\n");
		}

		// Functions with node counts
		if (Blueprint->FunctionGraphs.Num() > 0)
		{
			Summary += TEXT("- Functions:\n");
			for (const UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
			{
				if (!FuncGraph) continue;
				int32 NodeCount = FuncGraph->Nodes.Num();
				// 0-1 nodes means empty stub (entry node only)
				Summary += FString::Printf(TEXT("  - %s (%d nodes%s)\n"),
					*FuncGraph->GetName(),
					NodeCount,
					NodeCount <= 1 ? TEXT(" -- EMPTY, needs plan_json") : TEXT(""));
			}
		}

		// Event graph
		for (const UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph)
			{
				Summary += FString::Printf(TEXT("- EventGraph: %d nodes\n"), Graph->Nodes.Num());
			}
		}

		// Compile status
		if (Blueprint->Status == BS_Error)
		{
			Summary += TEXT("- Compile: ERROR\n");
		}
		else if (Blueprint->Status == BS_UpToDate)
		{
			Summary += TEXT("- Compile: OK\n");
		}
		else
		{
			Summary += TEXT("- Compile: needs recompile\n");
		}
	}

	return Summary;
}

FString FOliveCLIProviderBase::BuildConversationPrompt(const TArray<FOliveChatMessage>& Messages, const TArray<FOliveToolDefinition>& Tools) const
{
	// Format the full conversation history for the CLI prompt.
	// ConversationManager sends the complete MessageHistory on each call,
	// including tool results from previous iterations of the agentic loop.
	FString Prompt;
	int32 UserMessageCount = 0;
	int32 ToolResultCount = 0;
	for (const FOliveChatMessage& Msg : Messages)
	{
		if (Msg.Role == EOliveChatRole::System)
		{
			continue; // System prompt handled via --append-system-prompt
		}
		else if (Msg.Role == EOliveChatRole::User)
		{
			UserMessageCount++;
			Prompt += FString::Printf(TEXT("[User]\n%s\n\n"), *Msg.Content);
		}
		else if (Msg.Role == EOliveChatRole::Assistant)
		{
			Prompt += TEXT("[Assistant]\n");
			if (!Msg.Content.IsEmpty())
			{
				Prompt += Msg.Content;
				Prompt += TEXT("\n");
			}

			// Reconstruct prior tool calls so follow-up iterations preserve the
			// assistant's own action trace, matching API providers' behavior.
			for (const FOliveStreamChunk& ToolCall : Msg.ToolCalls)
			{
				FString ArgsJson = TEXT("{}");
				if (ToolCall.ToolArguments.IsValid())
				{
					TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsJson);
					if (!FJsonSerializer::Serialize(ToolCall.ToolArguments.ToSharedRef(), Writer))
					{
						ArgsJson = TEXT("{}");
					}
				}

				const FString ToolCallId = ToolCall.ToolCallId.IsEmpty() ? TEXT("tc_history") : ToolCall.ToolCallId;
				Prompt += FString::Printf(TEXT("<tool_call id=\"%s\">\n"), *ToolCallId);
				Prompt += FString::Printf(TEXT("{\"name\":\"%s\",\"arguments\":%s}\n"), *ToolCall.ToolName, *ArgsJson);
				Prompt += TEXT("</tool_call>\n");
			}
			Prompt += TEXT("\n");
		}
		else if (Msg.Role == EOliveChatRole::Tool)
		{
			ToolResultCount++;
			Prompt += FString::Printf(
				TEXT("[Tool Result: %s (id: %s)]\n%s\n\n"),
				*Msg.ToolName, *Msg.ToolCallId, *Msg.Content);
		}
	}

	// Reinforce tool call format in the imperative stdin channel.
	// The system prompt (--append-system-prompt) is the reference channel; the model
	// doesn't always follow it. Repeating the format here ensures it's seen.
	Prompt += TEXT("## How to Call Tools\n");
	Prompt += TEXT("Output <tool_call> blocks (NOT plain text). Example:\n");
	Prompt += TEXT("<tool_call id=\"tc_1\">\n");
	Prompt += TEXT("{\"name\": \"blueprint.create\", \"arguments\": {\"path\": \"/Game/Blueprints/BP_Example\", \"parent_class\": \"Actor\"}}\n");
	Prompt += TEXT("</tool_call>\n");
	Prompt += TEXT("You can output multiple <tool_call> blocks. Every response MUST contain at least one.\n\n");

	// Force a concrete next action in the imperative stdin channel.
	Prompt += TEXT("## Next Action Required\n");
	if (UserMessageCount == 1 && ToolResultCount == 0)
	{
		Prompt += TEXT("- Respond ONLY with <tool_call> blocks. Do NOT respond with explanation text.\n");
		Prompt += TEXT("- If the task is creating NEW Blueprints, check if a template fits first (blueprint.create with template_id). Otherwise use blueprint.create with parent_class.\n");
		Prompt += TEXT("- If the task is modifying EXISTING assets, start with project.search to find exact paths.\n");
		Prompt += TEXT("- Batch only independent calls (e.g., create + add_component + add_variable).\n");
		Prompt += TEXT("- Do NOT batch blueprint.preview_plan_json and blueprint.apply_plan_json in the same response.\n\n");
	}
	else
	{
		Prompt += TEXT("- Tool results are above. Continue with <tool_call> blocks for the next required tools.\n");
		Prompt += TEXT("- Do not repeat identical project.search queries unless results changed.\n");
		Prompt += TEXT("- The task is NOT complete until all assets have components, variables, and graph logic wired AND compiled. Do NOT stop after only creating assets.\n\n");
	}

	return Prompt;
}

FString FOliveCLIProviderBase::BuildCLISystemPrompt(const FString& UserTask, const TArray<FOliveToolDefinition>& Tools) const
{
	FString SystemPrompt;

	// ==========================================
	// Cherry-picked preamble -- project context + policies + recipe routing ONLY.
	// We intentionally skip blueprint_authoring because it was written for the
	// API path (uses project.batch_write, "read before write" for creates, etc.)
	// and directly conflicts with the CLI wrapper's instructions.
	// ==========================================
	const FOlivePromptAssembler& Assembler = FOlivePromptAssembler::Get();

	const FString ProjectContext = Assembler.GetProjectContext();
	if (!ProjectContext.IsEmpty())
	{
		SystemPrompt += TEXT("## Project\n");
		SystemPrompt += ProjectContext;
		SystemPrompt += TEXT("\n\n");
	}

	const FString PolicyContext = Assembler.GetPolicyContext();
	if (!PolicyContext.IsEmpty())
	{
		SystemPrompt += TEXT("## Policies\n");
		SystemPrompt += PolicyContext;
		SystemPrompt += TEXT("\n\n");
	}

	// Fetch recipe_routing pack directly -- skip blueprint_authoring
	const FString RecipeRouting = Assembler.GetKnowledgePackById(TEXT("recipe_routing"));
	if (!RecipeRouting.IsEmpty())
	{
		SystemPrompt += RecipeRouting;
		SystemPrompt += TEXT("\n\n");
	}

	// ==========================================
	// Shared domain knowledge -- loaded from disk so recipes/prompts stay in sync
	// ==========================================
	const FString CLIBlueprint = Assembler.GetKnowledgePackById(TEXT("cli_blueprint"));
	if (!CLIBlueprint.IsEmpty())
	{
		SystemPrompt += CLIBlueprint;
		SystemPrompt += TEXT("\n\n");
	}
	else
	{
		// Fallback: minimal inline instructions if file missing
		UE_LOG(LogOliveCLIProvider, Warning,
			TEXT("cli_blueprint knowledge pack not found. Using minimal fallback."));
		SystemPrompt += TEXT("You are an Unreal Engine 5.5 Blueprint specialist.\n");
		SystemPrompt += TEXT("Use blueprint.create, add_component, add_variable, apply_plan_json.\n\n");
	}

	const FString DesignPatterns = Assembler.GetKnowledgePackById(TEXT("blueprint_design_patterns"));
	if (!DesignPatterns.IsEmpty())
	{
		SystemPrompt += DesignPatterns;
		SystemPrompt += TEXT("\n\n");
	}

	const FString EventsVsFunctions = Assembler.GetKnowledgePackById(TEXT("events_vs_functions"));
	if (!EventsVsFunctions.IsEmpty())
	{
		SystemPrompt += EventsVsFunctions;
		SystemPrompt += TEXT("\n\n");
	}

	const FString NodeRouting = Assembler.GetKnowledgePackById(TEXT("node_routing"));
	if (!NodeRouting.IsEmpty())
	{
		SystemPrompt += NodeRouting;
		SystemPrompt += TEXT("\n\n");
	}

	// ==========================================
	// Template catalog (factory + reference templates)
	// ==========================================
	if (FOliveTemplateSystem::Get().HasTemplates())
	{
		const FString& Catalog = FOliveTemplateSystem::Get().GetCatalogBlock();
		if (!Catalog.IsEmpty())
		{
			SystemPrompt += Catalog;
			SystemPrompt += TEXT("\n\n");
		}
	}

	// ==========================================
	// Tool schemas (CLI-specific: inline since no native tool calling)
	// ==========================================
	if (Tools.Num() > 0)
	{
		SystemPrompt += FOliveCLIToolSchemaSerializer::Serialize(Tools, /*bCompact=*/true);
		SystemPrompt += TEXT("\n");
	}

	// ==========================================
	// Tool call format instructions (CLI-specific)
	// ==========================================
	SystemPrompt += FOliveCLIToolCallParser::GetFormatInstructions();

	return SystemPrompt;
}

void FOliveCLIProviderBase::CancelRequest()
{
	bStopReading = true;
	++RequestGeneration; // Invalidate in-flight async tasks from old request

	// Clear tool filter to restore full tool visibility
	FOliveMCPServer::Get().ClearToolFilter();

	// Clear callbacks BEFORE killing process so the completion
	// lambda (if it races to fire) won't invoke stale delegates
	{
		FScopeLock Lock(&CallbackLock);
		CurrentOnComplete.Unbind();
		CurrentOnError.Unbind();
		CurrentOnChunk.Unbind();
		CurrentOnToolCall.Unbind();
		bIsBusy = false;
	}

	KillProcess();
}

void FOliveCLIProviderBase::KillProcess()
{
	bStopReading = true;

	// Clean up MCP tool call delegate to prevent dangling callback
	if (ToolCallDelegateHandle.IsValid())
	{
		FOliveMCPServer::Get().OnToolCalled.Remove(ToolCallDelegateHandle);
		ToolCallDelegateHandle.Reset();
	}

	if (ReaderThread)
	{
		ReaderThread->WaitForCompletion();
		delete ReaderThread;
		ReaderThread = nullptr;
	}

	if (ProcessHandle.IsValid())
	{
		FPlatformProcess::TerminateProc(ProcessHandle, true);
		FPlatformProcess::CloseProc(ProcessHandle);
	}

	if (StdinWritePipe)
	{
		FPlatformProcess::ClosePipe(nullptr, StdinWritePipe);
		StdinWritePipe = nullptr;
	}

	if (StdoutReadPipe)
	{
		FPlatformProcess::ClosePipe(StdoutReadPipe, nullptr);
		StdoutReadPipe = nullptr;
	}
}

bool FOliveCLIProviderBase::SendToProcess(const FString& Input)
{
	if (!StdinWritePipe)
	{
		return false;
	}

	FString InputWithNewline = Input + TEXT("\n");
	return FPlatformProcess::WritePipe(StdinWritePipe, InputWithNewline);
}
