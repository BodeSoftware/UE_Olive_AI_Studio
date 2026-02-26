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

DEFINE_LOG_CATEGORY_STATIC(LogOliveCLIProvider, Log, All);

namespace
{
	/** Maximum seconds with no stdout output before killing a hung CLI process */
	constexpr double CLI_IDLE_TIMEOUT_SECONDS = 120.0;
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

	// --- Write .mcp.json with absolute path to mcp-bridge.js ---
	const FString PluginDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
	const FString BridgePath = FPaths::Combine(PluginDir, TEXT("mcp-bridge.js"));
	// Normalize to forward slashes for JSON
	FString BridgePathJson = BridgePath.Replace(TEXT("\\"), TEXT("/"));

	const FString McpConfig = FString::Printf(
		TEXT("{\n")
		TEXT("  \"mcpServers\": {\n")
		TEXT("    \"olive-ai-studio\": {\n")
		TEXT("      \"command\": \"node\",\n")
		TEXT("      \"args\": [\"%s\"]\n")
		TEXT("    }\n")
		TEXT("  }\n")
		TEXT("}\n"),
		*BridgePathJson
	);

	const FString McpConfigPath = FPaths::Combine(AutonomousSandboxDir, TEXT(".mcp.json"));
	FFileHelper::SaveStringToFile(McpConfig, *McpConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	// --- Write CLAUDE.md with agent role context ---
	// Read AGENTS.md from plugin dir for domain-specific workflow guidance
	FString AgentsContent;
	const FString AgentsPath = FPaths::Combine(PluginDir, TEXT("AGENTS.md"));
	FFileHelper::LoadFileToString(AgentsContent, *AgentsPath);

	FString ClaudeMd;
	ClaudeMd += TEXT("# Olive AI Studio - Agent Context\n\n");
	ClaudeMd += TEXT("You are an AI assistant integrated with Unreal Engine 5.5 via Olive AI Studio.\n");
	ClaudeMd += TEXT("Your job is to help users create and modify game assets (Blueprints, Behavior Trees, PCG graphs, etc.) using the MCP tools provided.\n\n");
	ClaudeMd += TEXT("## Critical Rules\n");
	ClaudeMd += TEXT("- You are NOT a plugin developer. Do NOT modify plugin source code.\n");
	ClaudeMd += TEXT("- Use ONLY the MCP tools to create and edit game assets.\n");
	ClaudeMd += TEXT("- All asset paths should be under `/Game/` (the project's Content directory).\n");
	ClaudeMd += TEXT("- When creating Blueprints, use `blueprint.create` or `blueprint.create_from_template` -- never try to create .uasset files manually.\n");
	ClaudeMd += TEXT("- Always preview before applying plan JSON: call `blueprint.preview_plan_json` first, then `blueprint.apply_plan_json` in a separate turn.\n");
	ClaudeMd += TEXT("- Do not re-preview an unchanged plan. If preview succeeds, apply in the next turn. Revising and re-previewing is fine.\n");
	ClaudeMd += TEXT("- Complete the FULL task: create structures, wire graph logic, compile, and verify. Do not stop partway.\n");
	ClaudeMd += TEXT("- Once ALL Blueprints compile with 0 errors and 0 warnings, the task is COMPLETE. Immediately stop and report what you built.\n");
	ClaudeMd += TEXT("- Before writing your first plan_json for each Blueprint, call olive.get_recipe to look up the correct wiring pattern. Skip only if create_from_template already provided the logic.\n");
	ClaudeMd += TEXT("- Use schema_version \"2.0\" for all plan_json calls (v2.0 has automatic pin resolution).\n");
	ClaudeMd += TEXT("- After `create_from_template`, always `blueprint.read` the result before modifying it.\n");
	ClaudeMd += TEXT("- If `apply_plan_json` fails, re-read the graph, fix the plan, and retry once. Fall back to add_node/connect_pins only after a second failure.\n\n");

	// Append the full AGENTS.md content which has workflow patterns, plan JSON format, etc.
	if (!AgentsContent.IsEmpty())
	{
		ClaudeMd += TEXT("---\n\n");
		ClaudeMd += AgentsContent;
	}

	const FString ClaudeMdPath = FPaths::Combine(AutonomousSandboxDir, TEXT("CLAUDE.md"));
	FFileHelper::SaveStringToFile(ClaudeMd, *ClaudeMdPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	// --- Write AGENTS.md (copy from plugin dir) ---
	// Claude Code reads AGENTS.md for agent-specific workflow guidance.
	// The plugin's AGENTS.md is already written for the agent role (tool usage, plan JSON, etc.)
	if (!AgentsContent.IsEmpty())
	{
		const FString SandboxAgentsPath = FPaths::Combine(AutonomousSandboxDir, TEXT("AGENTS.md"));
		FFileHelper::SaveStringToFile(AgentsContent, *SandboxAgentsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	UE_LOG(LogOliveCLIProvider, Log, TEXT("Autonomous sandbox created at: %s"), *AutonomousSandboxDir);
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

	// Autonomous mode: no system prompt escaping, no BuildCLISystemPrompt.
	// The CLI discovers tools via MCP and reads the sandbox CLAUDE.md for domain context.
	FString CLIArgs = GetCLIArgumentsAutonomous();

	UE_LOG(LogOliveCLIProvider, Log, TEXT("Launching autonomous CLI with args: %s"), *CLIArgs);

	// Subscribe to MCP tool call events for activity-based timeout tracking.
	// Uses the AliveGuard pattern to safely update the atomic timestamp from
	// the game thread (OnToolCalled fires on game thread) while the background
	// read loop checks it via std::atomic<double>.
	LastToolCallTimestamp.store(FPlatformTime::Seconds());
	if (ToolCallDelegateHandle.IsValid())
	{
		FOliveMCPServer::Get().OnToolCalled.Remove(ToolCallDelegateHandle);
	}
	TSharedPtr<FThreadSafeBool> Guard = AliveGuard;
	ToolCallDelegateHandle = FOliveMCPServer::Get().OnToolCalled.AddLambda(
		[this, Guard](const FString& ToolName, const FString& ClientId)
		{
			if (*Guard)
			{
				LastToolCallTimestamp.store(FPlatformTime::Seconds());
			}
		});

	// Delegate to shared process lifecycle, with autonomous completion handler.
	// Pass the sandbox directory so the CLI launches from there instead of the plugin source dir.
	LaunchCLIProcess(CLIArgs, UserMessage, [this](int32 ReturnCode)
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

		// Read max runtime from settings (0 = no limit). This is primarily a cost-control
		// safety net for autonomous mode, but applies universally since orchestrated turns
		// complete in seconds and will never hit a reasonable limit.
		const UOliveAISettings* RuntimeSettings = UOliveAISettings::Get();
		const double MaxRuntimeSeconds = RuntimeSettings ? static_cast<double>(RuntimeSettings->AutonomousMaxRuntimeSeconds) : 300.0;

		while (FPlatformProcess::IsProcRunning(ProcessHandle) && !bStopReading)
		{
			FString Chunk = FPlatformProcess::ReadPipe(StdoutRead);
			if (!Chunk.IsEmpty())
			{
				LastOutputTime = FPlatformTime::Seconds();
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
				if (FPlatformTime::Seconds() - LastOutputTime > CLI_IDLE_TIMEOUT_SECONDS)
				{
					UE_LOG(LogOliveCLIProvider, Warning, TEXT("%s process idle for %.0f seconds - terminating"), *CLIName, CLI_IDLE_TIMEOUT_SECONDS);
					bStopReading = true;
					if (*Guard)
					{
						FPlatformProcess::TerminateProc(ProcessHandle, true);
					}
					break;
				}
				FPlatformProcess::Sleep(0.01f);
			}

			// Activity-based timeout: kill if no MCP tool call in AutonomousIdleToolSeconds.
			// This catches "thinking but not acting" scenarios where stdout is flowing
			// (so idle timeout doesn't trigger) but no tool calls are being made.
			// LastToolCallTimestamp is std::atomic<double>, written on game thread by
			// OnToolCalled delegate, read here on background thread -- safe without lock.
			const double LastToolCall = LastToolCallTimestamp.load();
			if (LastToolCall > 0.0)
			{
				const double IdleToolTimeout = RuntimeSettings ? static_cast<double>(RuntimeSettings->AutonomousIdleToolSeconds) : 120.0;
				if (IdleToolTimeout > 0.0 && (FPlatformTime::Seconds() - LastToolCall) > IdleToolTimeout)
				{
					UE_LOG(LogOliveCLIProvider, Warning,
						TEXT("%s process: no MCP tool call in %.0f seconds - terminating"),
						*CLIName, IdleToolTimeout);
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

	// Log activity stats for diagnostic purposes
	const double LastTool = LastToolCallTimestamp.load();
	UE_LOG(LogOliveCLIProvider, Log,
		TEXT("Autonomous run complete (exit code %d): last tool call %.1fs ago, accumulated %d chars"),
		ReturnCode,
		LastTool > 0.0 ? FPlatformTime::Seconds() - LastTool : -1.0,
		AccumulatedResponse.Len());

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
		Prompt += TEXT("- If the task is creating NEW Blueprints, check if a template fits first (blueprint.create_from_template). Otherwise use blueprint.create.\n");
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
