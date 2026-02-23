// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveClaudeCodeProvider.h"
#include "Providers/OliveCLIToolCallParser.h"
#include "Providers/OliveCLIToolSchemaSerializer.h"
#include "Chat/OlivePromptAssembler.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveClaudeCode, Log, All);

// ==========================================
// FOliveClaudeReaderRunnable
// ==========================================

FOliveClaudeReaderRunnable::FOliveClaudeReaderRunnable(
	void* InReadPipe,
	FThreadSafeBool& InStopFlag,
	TFunction<void(const FString&)> InOnLine
)
	: ReadPipe(InReadPipe)
	, bStop(InStopFlag)
	, OnLine(InOnLine)
{
}

uint32 FOliveClaudeReaderRunnable::Run()
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

void FOliveClaudeReaderRunnable::Stop()
{
	bStop = true;
}

// ==========================================
// FOliveClaudeCodeProvider
// ==========================================

FOliveClaudeCodeProvider::FOliveClaudeCodeProvider()
	: bStopReading(false)
	, bIsBusy(false)
{
	// Prefer plugin directory so Claude can discover this plugin's .mcp.json.
	// We still grant full project access with --add-dir when launching.
	const FString PluginDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
	if (IFileManager::Get().DirectoryExists(*PluginDir))
	{
		WorkingDirectory = PluginDir;
	}
	else
	{
		WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	}

	UE_LOG(LogOliveClaudeCode, Log, TEXT("Claude Code working directory: %s"), *WorkingDirectory);
}

FOliveClaudeCodeProvider::~FOliveClaudeCodeProvider()
{
	KillClaudeProcess();
}

TArray<FString> FOliveClaudeCodeProvider::GetAvailableModels() const
{
	// Claude Code CLI uses whatever model the user's subscription supports
	return {
		TEXT("claude-opus-4-6"),
		TEXT("claude-sonnet-4-6"),
		TEXT("claude-sonnet-4-20250514"),
		TEXT("claude-opus-4-20250514"),
		TEXT("claude-haiku-3-5-20241022")
	};
}

FString FOliveClaudeCodeProvider::GetRecommendedModel() const
{
	// Prefer stable, current model ids; fall back to dated ids if needed.
	return TEXT("claude-sonnet-4-6");
}

void FOliveClaudeCodeProvider::Configure(const FOliveProviderConfig& Config)
{
	CurrentConfig = Config;
	CurrentConfig.ProviderName = TEXT("claudecode");
}

bool FOliveClaudeCodeProvider::ValidateConfig(FString& OutError) const
{
	// Check if claude is installed
	if (!IsClaudeCodeInstalled())
	{
		OutError = TEXT("Claude Code CLI not found. Install from: https://claude.ai/download");
		return false;
	}

	return true;
}

bool FOliveClaudeCodeProvider::IsClaudeCodeInstalled()
{
	FString ClaudePath = GetClaudeExecutablePath();
	return !ClaudePath.IsEmpty();
}

FString FOliveClaudeCodeProvider::GetClaudeExecutablePath()
{
	// We return the path to the Claude CLI JavaScript file, which we'll run with Node.js
	// This avoids issues with .cmd files on Windows

#if PLATFORM_WINDOWS
	// Look for the npm global install location
	FString NpmPath = FPaths::Combine(FPlatformProcess::UserDir(), TEXT("AppData/Roaming/npm"));
	FString ClaudeCliJs = FPaths::Combine(NpmPath, TEXT("node_modules/@anthropic-ai/claude-code/cli.js"));

	if (IFileManager::Get().FileExists(*ClaudeCliJs))
	{
		return ClaudeCliJs;
	}

	// Try finding via where command
	FString WhichOutput;
	int32 ReturnCode;
	FPlatformProcess::ExecProcess(TEXT("where"), TEXT("claude.cmd"), &ReturnCode, &WhichOutput, nullptr);
	if (ReturnCode == 0 && !WhichOutput.IsEmpty())
	{
		WhichOutput.TrimStartAndEndInline();
		int32 NewlineIdx;
		if (WhichOutput.FindChar('\n', NewlineIdx))
		{
			WhichOutput = WhichOutput.Left(NewlineIdx);
		}
		WhichOutput.TrimStartAndEndInline();

		// Convert .cmd path to cli.js path
		// e.g., C:\Users\X\AppData\Roaming\npm\claude.cmd -> C:\Users\X\AppData\Roaming\npm\node_modules\@anthropic-ai\claude-code\cli.js
		FString NpmDir = FPaths::GetPath(WhichOutput);
		FString CliJsPath = FPaths::Combine(NpmDir, TEXT("node_modules/@anthropic-ai/claude-code/cli.js"));
		if (IFileManager::Get().FileExists(*CliJsPath))
		{
			return CliJsPath;
		}
	}

	// Try common installation paths
	TArray<FString> CommonPaths = {
		FPaths::Combine(FPlatformProcess::UserDir(), TEXT("AppData/Roaming/npm/node_modules/@anthropic-ai/claude-code/cli.js")),
		FPaths::Combine(FPlatformProcess::UserDir(), TEXT("AppData/Local/Programs/claude/claude.exe")),
	};

	for (const FString& Path : CommonPaths)
	{
		if (IFileManager::Get().FileExists(*Path))
		{
			return Path;
		}
	}
#else
	FString WhichOutput;
	int32 ReturnCode;
	FPlatformProcess::ExecProcess(TEXT("/usr/bin/which"), TEXT("claude"), &ReturnCode, &WhichOutput, nullptr);
	if (ReturnCode == 0 && !WhichOutput.IsEmpty())
	{
		WhichOutput.TrimStartAndEndInline();
		return WhichOutput;
	}
#endif

	return FString();
}

FString FOliveClaudeCodeProvider::GetClaudeCodeVersion()
{
	FString ClaudePath = GetClaudeExecutablePath();
	if (ClaudePath.IsEmpty())
	{
		return TEXT("Not installed");
	}

	FString VersionOutput;
	int32 ReturnCode;
	FPlatformProcess::ExecProcess(*ClaudePath, TEXT("--version"), &ReturnCode, &VersionOutput, nullptr);

	if (ReturnCode == 0)
	{
		VersionOutput.TrimStartAndEndInline();
		return VersionOutput;
	}

	return TEXT("Unknown");
}

void FOliveClaudeCodeProvider::SendMessage(
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

	// Store callbacks
	{
		FScopeLock Lock(&CallbackLock);
		CurrentOnChunk = OnChunk;
		CurrentOnToolCall = OnToolCall;
		CurrentOnComplete = OnComplete;
		CurrentOnError = OnError;
	}

	bIsBusy = true;
	AccumulatedResponse.Empty();

	// Build prompt and system prompt on the game thread (prompt assembler accesses UObject settings)
	FString Prompt = BuildPrompt(Messages, Tools);
	FString SystemPromptText = BuildSystemPrompt(Prompt, Tools);

	// Spawn claude process with --print flag for JSON output
	// Use --dangerously-skip-permissions to avoid interactive prompts
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, Prompt, SystemPromptText]()
	{
		FString ClaudePath = GetClaudeExecutablePath();
		if (ClaudePath.IsEmpty())
		{
			AsyncTask(ENamedThreads::GameThread, [this]()
			{
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(TEXT("Claude Code CLI not found"));
			});
			return;
		}

		// Escape system prompt for command line
		FString EscapedSystemPrompt = SystemPromptText;
		EscapedSystemPrompt.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscapedSystemPrompt.ReplaceInline(TEXT("\""), TEXT("\\\""));

		// Build arguments for claude
		// --print: Output text instead of interactive UI
		// --output-format stream-json: for real-time JSON output (requires --verbose)
		// --verbose: Required when using stream-json output format
		// --dangerously-skip-permissions: avoid interactive prompts in editor context
		// --max-turns 1: single completion turn (ConversationManager owns the agentic loop)
		// --strict-mcp-config: ignore .mcp.json in the working directory — prevents the CLI
		//   from discovering MCP tools on its own. ConversationManager is the sole orchestrator;
		//   tools are defined via system prompt text and parsed from <tool_call> XML blocks.
		// --append-system-prompt: inject domain-specific guidance and tool schemas
		FString SystemPromptArg;
		if (!EscapedSystemPrompt.IsEmpty())
		{
			SystemPromptArg = FString::Printf(TEXT("--append-system-prompt \"%s\" "), *EscapedSystemPrompt);
		}

		FString ClaudeArgs = FString::Printf(
			TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --max-turns 1 --strict-mcp-config %s"),
			*SystemPromptArg
		);

		UE_LOG(LogOliveClaudeCode, Log, TEXT("System prompt injected: %d chars"), SystemPromptText.Len());

		// Determine executable and args
		FString Executable;
		FString Args;

		if (ClaudePath.EndsWith(TEXT(".js")))
		{
			// Run with Node.js directly - much more reliable than .cmd files
			Executable = TEXT("node");
			Args = FString::Printf(TEXT("\"%s\" %s"), *ClaudePath, *ClaudeArgs);
		}
		else if (ClaudePath.EndsWith(TEXT(".exe")))
		{
			// Direct executable
			Executable = ClaudePath;
			Args = ClaudeArgs;
		}
		else
		{
			// Fallback for other cases (shouldn't happen)
			Executable = ClaudePath;
			Args = ClaudeArgs;
		}

		UE_LOG(LogOliveClaudeCode, Log, TEXT("Running: %s %s"), *Executable, *Args);
		UE_LOG(LogOliveClaudeCode, Log, TEXT("Working Directory: %s"), *WorkingDirectory);

		// Create pipes for process communication
		void* StdoutRead = nullptr;
		void* StdoutWrite = nullptr;

		if (!FPlatformProcess::CreatePipe(StdoutRead, StdoutWrite))
		{
			AsyncTask(ENamedThreads::GameThread, [this]()
			{
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(TEXT("Failed to create stdout pipe for Claude process"));
			});
			return;
		}

		// Create stdin pipe for delivering the prompt instead of the -p CLI argument.
		// This avoids the Windows ~32KB command-line length limit that causes crashes
		// when the conversation history grows large during agentic loop iterations.
		// bWritePipeLocal=true makes the write end non-inheritable (parent keeps it)
		// and the read end inheritable (child gets it).
		void* StdinRead = nullptr;
		void* StdinWrite = nullptr;

		if (!FPlatformProcess::CreatePipe(StdinRead, StdinWrite, /*bWritePipeLocal=*/true))
		{
			FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
			AsyncTask(ENamedThreads::GameThread, [this]()
			{
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(TEXT("Failed to create stdin pipe for Claude process"));
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
			*WorkingDirectory,
			StdoutWrite,  // stdout pipe (child writes, parent reads)
			StdinRead     // stdin pipe (child reads, parent writes)
		);

		if (!ProcessHandle.IsValid())
		{
			FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
			FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
			AsyncTask(ENamedThreads::GameThread, [this]()
			{
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(TEXT("Failed to spawn Claude process"));
			});
			return;
		}

		// Close write end of stdout pipe (we only read from it)
		FPlatformProcess::ClosePipe(nullptr, StdoutWrite);

		// Close read end of stdin pipe (we only write to it)
		FPlatformProcess::ClosePipe(StdinRead, nullptr);
		StdinRead = nullptr;

		// Deliver the prompt via stdin. The FString overload appends a trailing newline,
		// which is harmless since Claude Code CLI reads stdin to EOF.
		FPlatformProcess::WritePipe(StdinWrite, Prompt);

		UE_LOG(LogOliveClaudeCode, Log, TEXT("Prompt delivered via stdin: %d chars"), Prompt.Len());

		// Close write end of stdin to signal EOF. Without this the child blocks forever
		// waiting for more input.
		FPlatformProcess::ClosePipe(nullptr, StdinWrite);
		StdinWrite = nullptr;

		// Read output
		bStopReading = false;
		FString OutputBuffer;

		while (FPlatformProcess::IsProcRunning(ProcessHandle) && !bStopReading)
		{
			FString Chunk = FPlatformProcess::ReadPipe(StdoutRead);
			if (!Chunk.IsEmpty())
			{
				OutputBuffer += Chunk;

				// Process complete lines
				int32 NewlineIdx;
				while (OutputBuffer.FindChar('\n', NewlineIdx))
				{
					FString Line = OutputBuffer.Left(NewlineIdx).TrimStartAndEnd();
					OutputBuffer = OutputBuffer.Mid(NewlineIdx + 1);

					if (!Line.IsEmpty())
					{
						// Dispatch line parsing to game thread
						AsyncTask(ENamedThreads::GameThread, [this, Line]()
						{
							FScopeLock Lock(&CallbackLock);
							if (!bIsBusy) return;
							ParseOutputLine(Line);
						});
					}
				}
			}
			else
			{
				FPlatformProcess::Sleep(0.01f);
			}
		}

		// Read any remaining output
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
			AsyncTask(ENamedThreads::GameThread, [this, OutputBuffer]()
			{
				FScopeLock Lock(&CallbackLock);
				if (!bIsBusy) return;
				ParseOutputLine(OutputBuffer);
			});
		}

		// Cleanup
		FPlatformProcess::ClosePipe(StdoutRead, nullptr);

		// Get return code
		int32 ReturnCode;
		FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
		FPlatformProcess::CloseProc(ProcessHandle);

		// Signal completion — delegate to HandleResponseComplete for tool call parsing
		AsyncTask(ENamedThreads::GameThread, [this, ReturnCode]()
		{
			FScopeLock Lock(&CallbackLock);
			if (!bIsBusy) return;
			HandleResponseComplete(ReturnCode);
		});
	});
}

void FOliveClaudeCodeProvider::HandleResponseComplete(int32 ReturnCode)
{
	// Called under CallbackLock, while bIsBusy is true.
	// Bridges CLI text output to ConversationManager's agentic loop by parsing
	// <tool_call> blocks from accumulated text and emitting them via OnToolCall.

	if (ReturnCode != 0 && AccumulatedResponse.IsEmpty())
	{
		bIsBusy = false;
		CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("Claude process exited with code %d"), ReturnCode));
		return;
	}

	// Parse tool calls from accumulated text
	TArray<FOliveStreamChunk> ParsedToolCalls;
	FString CleanedText;
	bool bHasToolCalls = FOliveCLIToolCallParser::Parse(AccumulatedResponse, ParsedToolCalls, CleanedText);

	if (bHasToolCalls)
	{
		// Emit each tool call via OnToolCall — ConversationManager collects these
		for (const FOliveStreamChunk& ToolCall : ParsedToolCalls)
		{
			UE_LOG(LogOliveClaudeCode, Log, TEXT("Parsed tool call: %s (id: %s)"), *ToolCall.ToolName, *ToolCall.ToolCallId);
			CurrentOnToolCall.ExecuteIfBound(ToolCall);
		}
	}

	FOliveProviderUsage Usage;
	Usage.Model = TEXT("claude-code-cli");
	Usage.FinishReason = bHasToolCalls ? TEXT("tool_calls") : TEXT("stop");
	bIsBusy = false;
	CurrentOnComplete.ExecuteIfBound(CleanedText, Usage);
}

FString FOliveClaudeCodeProvider::BuildPrompt(const TArray<FOliveChatMessage>& Messages, const TArray<FOliveToolDefinition>& Tools) const
{
	// Format the full conversation history for the CLI prompt.
	// ConversationManager sends the complete MessageHistory on each call,
	// including tool results from previous iterations of the agentic loop.
	FString Prompt;
	for (const FOliveChatMessage& Msg : Messages)
	{
		if (Msg.Role == EOliveChatRole::System)
		{
			continue; // System prompt handled via --append-system-prompt
		}
		else if (Msg.Role == EOliveChatRole::User)
		{
			Prompt += FString::Printf(TEXT("[User]\n%s\n\n"), *Msg.Content);
		}
		else if (Msg.Role == EOliveChatRole::Assistant)
		{
			Prompt += FString::Printf(TEXT("[Assistant]\n%s\n\n"), *Msg.Content);
		}
		else if (Msg.Role == EOliveChatRole::Tool)
		{
			Prompt += FString::Printf(
				TEXT("[Tool Result: %s (id: %s)]\n%s\n\n"),
				*Msg.ToolName, *Msg.ToolCallId, *Msg.Content);
		}
	}
	return Prompt;
}

FString FOliveClaudeCodeProvider::BuildSystemPrompt(const FString& UserTask, const TArray<FOliveToolDefinition>& Tools) const
{
	FString SystemPrompt;

	// Design-focused identity
	SystemPrompt += TEXT("You are an Unreal Engine 5.5 Blueprint specialist.\n");
	SystemPrompt += TEXT("Think through the complete design before calling tools.\n\n");

	// Plan JSON reference material — the AI needs this for constructing plans
	SystemPrompt += TEXT("## Plan JSON Format (v2.0)\n");
	SystemPrompt += TEXT("Use this format with blueprint.preview_plan_json and blueprint.apply_plan_json:\n");
	SystemPrompt += TEXT("```json\n");
	SystemPrompt += TEXT("{\"schema_version\":\"2.0\",\"steps\":[\n");
	SystemPrompt += TEXT("  {\"step_id\":\"evt\",\"op\":\"event\",\"target\":\"BeginPlay\"},\n");
	SystemPrompt += TEXT("  {\"step_id\":\"spawn\",\"op\":\"spawn_actor\",\"target\":\"Actor\",");
	SystemPrompt += TEXT("\"inputs\":{\"Location\":\"@get_loc.auto\"},\"exec_after\":\"evt\"},\n");
	SystemPrompt += TEXT("  {\"step_id\":\"print\",\"op\":\"call\",\"target\":\"PrintString\",");
	SystemPrompt += TEXT("\"inputs\":{\"InString\":\"Done\"},\"exec_after\":\"spawn\"}\n");
	SystemPrompt += TEXT("]}\n");
	SystemPrompt += TEXT("```\n\n");

	// Available ops
	SystemPrompt += TEXT("## Available Ops\n");
	SystemPrompt += TEXT("event, custom_event, call, get_var, set_var, branch, sequence, cast, ");
	SystemPrompt += TEXT("for_loop, for_each_loop, delay, is_valid, print_string, spawn_actor, ");
	SystemPrompt += TEXT("make_struct, break_struct, return, comment\n\n");

	// Data wire syntax
	SystemPrompt += TEXT("## Data Wires (@ref syntax)\n");
	SystemPrompt += TEXT("- @step.auto — auto-match by type (use this ~80% of the time)\n");
	SystemPrompt += TEXT("- @step.~hint — fuzzy match with hint (e.g. @get_loc.~Location)\n");
	SystemPrompt += TEXT("- @step.PinName — smart name match\n");
	SystemPrompt += TEXT("- Literal values (no @) set pin defaults: \"InString\":\"Hello\"\n\n");

	// Exec flow
	SystemPrompt += TEXT("## Exec Flow\n");
	SystemPrompt += TEXT("- exec_after: step whose exec output connects to this step's exec input\n");
	SystemPrompt += TEXT("- exec_outputs: {\"True\":\"step_a\",\"False\":\"step_b\"} for Branch etc.\n\n");

	// Function resolution
	SystemPrompt += TEXT("## Function Resolution\n");
	SystemPrompt += TEXT("For op:call, use natural names. The system resolves K2_ prefixes, aliases ");
	SystemPrompt += TEXT("(Destroy->K2_DestroyActor, Print->PrintString), and fuzzy matching automatically.\n\n");

	// Key rules
	SystemPrompt += TEXT("## Rules\n");
	SystemPrompt += TEXT("- Asset paths end with asset name: /Game/Blueprints/BP_Gun\n");
	SystemPrompt += TEXT("- Use blueprint.apply_plan_json for graph logic (not individual add_node/connect_pins)\n");
	SystemPrompt += TEXT("- Read before write on existing assets\n");
	SystemPrompt += TEXT("- Variable types: Float, Boolean, Integer, Vector, Rotator, String\n");
	SystemPrompt += TEXT("- Variable type format: prefer {\"category\":\"float\"}; shorthand \"Float\" is also accepted\n");
	SystemPrompt += TEXT("- Component classes: StaticMeshComponent, SphereComponent, BoxComponent, ");
	SystemPrompt += TEXT("CapsuleComponent, ArrowComponent, ProjectileMovementComponent\n");
	SystemPrompt += TEXT("- WORKFLOW: Always call blueprint.preview_plan_json first. Pass the returned preview_fingerprint to blueprint.apply_plan_json.\n");
	SystemPrompt += TEXT("- STEP ORDER: In plan JSON, data-provider steps (get_var, GetPlayerController, pure function calls) MUST appear BEFORE any step that references them via @ref. The validator resolves in forward-pass order.\n");
	SystemPrompt += TEXT("- TARGET ASSET: The 'path' parameter must match the Blueprint that owns the component or graph being modified. If you added a component to BP_Gun, modify it on BP_Gun, not on another Blueprint.\n\n");

	// Serialize tool definitions from the filtered tools array
	if (Tools.Num() > 0)
	{
		SystemPrompt += FOliveCLIToolSchemaSerializer::Serialize(Tools, /*bCompact=*/true);
		SystemPrompt += TEXT("\n");
	}

	// Tool call format instructions
	SystemPrompt += FOliveCLIToolCallParser::GetFormatInstructions();

	return SystemPrompt;
}

void FOliveClaudeCodeProvider::ParseOutputLine(const FString& Line)
{
	// Claude --print --output-format streaming-json outputs JSON lines
	// Each line is a JSON object with a "type" field

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		// Not JSON, might be plain text output
		if (!Line.StartsWith(TEXT("{")))
		{
			// Treat as text chunk
			FScopeLock Lock(&CallbackLock);
			FOliveStreamChunk Chunk;
			Chunk.Text = Line;
			AccumulatedResponse += Line + TEXT("\n");
			CurrentOnChunk.ExecuteIfBound(Chunk);
		}
		return;
	}

	FString Type = JsonObject->GetStringField(TEXT("type"));

	if (Type == TEXT("assistant"))
	{
		// Claude Code format: {"type":"assistant","message":{"content":[{"type":"text","text":"..."}]}}
		const TSharedPtr<FJsonObject>* MessageObj;
		if (JsonObject->TryGetObjectField(TEXT("message"), MessageObj))
		{
			const TArray<TSharedPtr<FJsonValue>>* ContentArray;
			if ((*MessageObj)->TryGetArrayField(TEXT("content"), ContentArray))
			{
				for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
				{
					const TSharedPtr<FJsonObject>* ContentObj;
					if (ContentValue->TryGetObject(ContentObj))
					{
						FString ContentType = (*ContentObj)->GetStringField(TEXT("type"));
						if (ContentType == TEXT("text"))
						{
							FString Text = (*ContentObj)->GetStringField(TEXT("text"));
							if (!Text.IsEmpty())
							{
								FScopeLock Lock(&CallbackLock);
								FOliveStreamChunk Chunk;
								Chunk.Text = Text;
								AccumulatedResponse += Text;
								CurrentOnChunk.ExecuteIfBound(Chunk);
							}
						}
					}
				}
			}
		}
	}
	else if (Type == TEXT("result"))
	{
		// Result message - indicates completion
		FScopeLock Lock(&CallbackLock);
		FOliveStreamChunk Chunk;
		Chunk.bIsComplete = true;
		Chunk.FinishReason = TEXT("stop");
		CurrentOnChunk.ExecuteIfBound(Chunk);
	}
	else if (Type == TEXT("tool_use") || Type == TEXT("tool_call"))
	{
		// With --max-turns 1, Claude Code should not execute tools internally.
		// If this fires, something unexpected is happening with the CLI.
		FString ToolName;
		JsonObject->TryGetStringField(TEXT("name"), ToolName);
		UE_LOG(LogOliveClaudeCode, Warning,
			TEXT("Unexpected internal tool call (--max-turns 1 should prevent this): %s"), *ToolName);
	}
	else if (Type == TEXT("message_stop"))
	{
		// Message stop indicator (result type is handled above)
		FScopeLock Lock(&CallbackLock);
		FOliveStreamChunk Chunk;
		Chunk.bIsComplete = true;
		Chunk.FinishReason = TEXT("stop");
		CurrentOnChunk.ExecuteIfBound(Chunk);
	}
	else if (Type == TEXT("error"))
	{
		// Error
		FString ErrorMsg = JsonObject->GetStringField(TEXT("message"));
		if (ErrorMsg.IsEmpty())
		{
			ErrorMsg = JsonObject->GetStringField(TEXT("error"));
		}

		FScopeLock Lock(&CallbackLock);
		LastError = ErrorMsg;
		CurrentOnError.ExecuteIfBound(ErrorMsg);
	}
}

void FOliveClaudeCodeProvider::CancelRequest()
{
	bStopReading = true;

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

	KillClaudeProcess();
}

bool FOliveClaudeCodeProvider::SpawnClaudeProcess()
{
	// Implementation moved to SendMessage for single-shot execution
	return true;
}

void FOliveClaudeCodeProvider::KillClaudeProcess()
{
	bStopReading = true;

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

bool FOliveClaudeCodeProvider::SendToProcess(const FString& Input)
{
	if (!StdinWritePipe)
	{
		return false;
	}

	FString InputWithNewline = Input + TEXT("\n");
	return FPlatformProcess::WritePipe(StdinWritePipe, InputWithNewline);
}

void FOliveClaudeCodeProvider::ReadProcessOutput()
{
	// Handled in reader thread
}

// ==========================================
// Connection Validation
// ==========================================

void FOliveClaudeCodeProvider::ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const
{
	if (IsClaudeCodeInstalled())
	{
		FString Version = GetClaudeCodeVersion();
		Callback(true, FString::Printf(TEXT("Claude Code CLI detected. Version: %s"), *Version));
	}
	else
	{
		Callback(false, TEXT("Claude Code CLI not found. Install it from https://docs.anthropic.com/claude-code"));
	}
}
