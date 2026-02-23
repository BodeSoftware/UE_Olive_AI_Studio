// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveClaudeCodeProvider.h"
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

		// Escape the prompt for command line
		FString EscapedPrompt = Prompt;
		EscapedPrompt.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscapedPrompt.ReplaceInline(TEXT("\""), TEXT("\\\""));

		// Escape system prompt for command line
		FString EscapedSystemPrompt = SystemPromptText;
		EscapedSystemPrompt.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		EscapedSystemPrompt.ReplaceInline(TEXT("\""), TEXT("\\\""));

		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		const FString MCPConfigPath = FPaths::Combine(WorkingDirectory, TEXT(".mcp.json"));
		const bool bHasMCPConfig = IFileManager::Get().FileExists(*MCPConfigPath);

		// Build arguments for claude
		// --print: Output text instead of interactive UI
		// --output-format stream-json: for real-time JSON output (requires --verbose)
		// --verbose: Required when using stream-json output format
		// --dangerously-skip-permissions: avoid interactive prompts in editor context
		// --add-dir: allow access to the full Unreal project even when running from plugin dir
		// --mcp-config: force-load plugin MCP bridge so olive-ai-studio tools are available
		// --append-system-prompt: inject domain-specific guidance (Worker_Blueprint.txt etc.)
		FString SystemPromptArg;
		if (!EscapedSystemPrompt.IsEmpty())
		{
			SystemPromptArg = FString::Printf(TEXT("--append-system-prompt \"%s\" "), *EscapedSystemPrompt);
		}

		FString ClaudeArgs;
		if (bHasMCPConfig)
		{
			ClaudeArgs = FString::Printf(
				TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --add-dir \"%s\" --mcp-config \"%s\" --strict-mcp-config %s-p \"%s\""),
				*ProjectDir,
				*MCPConfigPath,
				*SystemPromptArg,
				*EscapedPrompt
			);

			UE_LOG(LogOliveClaudeCode, Log, TEXT("Using MCP config: %s"), *MCPConfigPath);
		}
		else
		{
			ClaudeArgs = FString::Printf(
				TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --add-dir \"%s\" %s-p \"%s\""),
				*ProjectDir,
				*SystemPromptArg,
				*EscapedPrompt
			);

			UE_LOG(LogOliveClaudeCode, Warning, TEXT("No .mcp.json found at %s. Claude may not have Olive MCP tools."), *MCPConfigPath);
		}

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
				CurrentOnError.ExecuteIfBound(TEXT("Failed to create pipe for Claude process"));
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
			StdoutWrite,  // stdout pipe
			nullptr       // stdin pipe (not needed for -p mode)
		);

		if (!ProcessHandle.IsValid())
		{
			FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
			AsyncTask(ENamedThreads::GameThread, [this]()
			{
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(TEXT("Failed to spawn Claude process"));
			});
			return;
		}

		// Close write end of stdout pipe (we only read)
		FPlatformProcess::ClosePipe(nullptr, StdoutWrite);

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
				ParseOutputLine(OutputBuffer);
			});
		}

		// Cleanup
		FPlatformProcess::ClosePipe(StdoutRead, nullptr);

		// Get return code
		int32 ReturnCode;
		FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
		FPlatformProcess::CloseProc(ProcessHandle);

		// Signal completion
		AsyncTask(ENamedThreads::GameThread, [this, ReturnCode]()
		{
			FScopeLock Lock(&CallbackLock);
			bIsBusy = false;

			if (ReturnCode != 0 && AccumulatedResponse.IsEmpty())
			{
				CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("Claude process exited with code %d"), ReturnCode));
			}
			else
			{
				FOliveProviderUsage Usage;
				Usage.Model = TEXT("claude-code-cli");
				CurrentOnComplete.ExecuteIfBound(AccumulatedResponse, Usage);
			}
		});
	});
}

FString FOliveClaudeCodeProvider::BuildPrompt(const TArray<FOliveChatMessage>& Messages, const TArray<FOliveToolDefinition>& Tools) const
{
	// Extract last user message
	FString LastUserMessage;
	for (int32 i = Messages.Num() - 1; i >= 0; --i)
	{
		if (Messages[i].Role == EOliveChatRole::User)
		{
			LastUserMessage = Messages[i].Content;
			break;
		}
	}

	if (LastUserMessage.IsEmpty())
	{
		return TEXT("");
	}

	// Wrap the user request with a brief, forceful routing instruction.
	// This is the -p prompt — Claude Code treats it as the primary user request,
	// so routing guidance here is followed more reliably than in --append-system-prompt.
	const FString Wrapper =
		TEXT("You have Olive AI Studio MCP tools. Use them to complete this task.\n\n")
		TEXT("REQUIRED WORKFLOW:\n")
		TEXT("1. blueprint.create — create any needed Blueprints (use /Game/Blueprints/BP_ prefix)\n")
		TEXT("2. blueprint.add_component — add components to each Blueprint\n")
		TEXT("3. blueprint.add_variable — add variables (use simple type names: Float, Boolean, Vector, ")
		TEXT("\"TSubclassOf<Actor>\" for class refs)\n")
		TEXT("4. For ALL graph logic (event graphs, functions): use blueprint.preview_plan_json then ")
		TEXT("blueprint.apply_plan_json. Do NOT use individual add_node/connect_pins calls — plan JSON ")
		TEXT("handles node creation, wiring, and pin defaults in one atomic operation.\n")
		TEXT("5. blueprint.read — verify the result\n\n")
		TEXT("The plan JSON format is described in your system prompt. Key: use schema_version 2.0, ")
		TEXT("@step.auto for data wires, exec_after for exec flow.\n\n")
		TEXT("USER REQUEST: ");

	return Wrapper + LastUserMessage;
}

FString FOliveClaudeCodeProvider::BuildSystemPrompt(const FString& UserTask, const TArray<FOliveToolDefinition>& Tools) const
{
	// Keep the system prompt focused on REFERENCE MATERIAL only — the plan JSON format
	// and data wire syntax that the AI needs to look up when constructing plans.
	// Routing instructions go in the -p prompt (BuildPrompt) where Claude Code follows them.
	//
	// This prompt is ~3KB instead of ~10KB, staying well within useful context.

	const FString SystemPrompt =
		TEXT("You are an Unreal Engine 5.5 Blueprint specialist with MCP tools.\n\n")

		TEXT("## Plan JSON Format (v2.0)\n")
		TEXT("Use this format with blueprint.preview_plan_json and blueprint.apply_plan_json:\n")
		TEXT("```json\n")
		TEXT("{\"schema_version\":\"2.0\",\"steps\":[\n")
		TEXT("  {\"step_id\":\"evt\",\"op\":\"event\",\"target\":\"BeginPlay\"},\n")
		TEXT("  {\"step_id\":\"spawn\",\"op\":\"spawn_actor\",\"target\":\"Actor\",")
		TEXT("\"inputs\":{\"Location\":\"@get_loc.auto\"},\"exec_after\":\"evt\"},\n")
		TEXT("  {\"step_id\":\"print\",\"op\":\"call\",\"target\":\"PrintString\",")
		TEXT("\"inputs\":{\"InString\":\"Done\"},\"exec_after\":\"spawn\"}\n")
		TEXT("]}\n")
		TEXT("```\n\n")

		TEXT("## Available Ops\n")
		TEXT("event, custom_event, call, get_var, set_var, branch, sequence, cast, ")
		TEXT("for_loop, for_each_loop, delay, is_valid, print_string, spawn_actor, ")
		TEXT("make_struct, break_struct, return, comment\n\n")

		TEXT("## Data Wires (@ref syntax)\n")
		TEXT("- @step.auto — auto-match by type (use this ~80% of the time)\n")
		TEXT("- @step.~hint — fuzzy match with hint (e.g. @get_loc.~Location)\n")
		TEXT("- @step.PinName — smart name match\n")
		TEXT("- Literal values (no @) set pin defaults: \"InString\":\"Hello\"\n\n")

		TEXT("## Exec Flow\n")
		TEXT("- exec_after: step whose exec output connects to this step's exec input\n")
		TEXT("- exec_outputs: {\"True\":\"step_a\",\"False\":\"step_b\"} for Branch etc.\n\n")

		TEXT("## Function Resolution\n")
		TEXT("For op:call, use natural names. The system resolves K2_ prefixes, aliases ")
		TEXT("(Destroy->K2_DestroyActor, Print->PrintString), and fuzzy matching automatically.\n\n")

		TEXT("## Key Rules\n")
		TEXT("- Read before write: always blueprint.read first\n")
		TEXT("- Create BPs at /Game/Blueprints/BP_Name\n")
		TEXT("- Variable types: Float, Boolean, Integer, Vector, Rotator, String, ")
		TEXT("\"TSubclassOf<Actor>\", Name\n")
		TEXT("- Component class names: StaticMeshComponent, SphereComponent, ")
		TEXT("ArrowComponent, ProjectileMovementComponent, BoxComponent, CapsuleComponent\n")
		TEXT("- After apply_plan_json, if wiring_errors exist, use blueprint.read to see ")
		TEXT("actual pin names, then fix with blueprint.connect_pins\n");

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
		// Claude Code CLI already executes MCP tools internally.
		// Do NOT forward tool calls into Olive's provider-agnostic tool loop,
		// otherwise operations execute twice and responses repeat.
		FString ToolName;
		JsonObject->TryGetStringField(TEXT("name"), ToolName);
		UE_LOG(LogOliveClaudeCode, Verbose, TEXT("Claude internal tool call: %s"), *ToolName);
	}
	else if (Type == TEXT("result") || Type == TEXT("message_stop"))
	{
		// Completion
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
	KillClaudeProcess();
	bIsBusy = false;
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
