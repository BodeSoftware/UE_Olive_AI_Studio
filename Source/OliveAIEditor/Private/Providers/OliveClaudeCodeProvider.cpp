// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveClaudeCodeProvider.h"
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

	// Build the prompt from messages and currently available tools
	FString Prompt = BuildPrompt(Messages, Tools);

	// Spawn claude process with --print flag for JSON output
	// Use --dangerously-skip-permissions to avoid interactive prompts
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, Prompt]()
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
		FString ClaudeArgs;
		if (bHasMCPConfig)
		{
			ClaudeArgs = FString::Printf(
				TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --add-dir \"%s\" --mcp-config \"%s\" --strict-mcp-config -p \"%s\""),
				*ProjectDir,
				*MCPConfigPath,
				*EscapedPrompt
			);

			UE_LOG(LogOliveClaudeCode, Log, TEXT("Using MCP config: %s"), *MCPConfigPath);
		}
		else
		{
			ClaudeArgs = FString::Printf(
				TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --add-dir \"%s\" -p \"%s\""),
				*ProjectDir,
				*EscapedPrompt
			);

			UE_LOG(LogOliveClaudeCode, Warning, TEXT("No .mcp.json found at %s. Claude may not have Olive MCP tools."), *MCPConfigPath);
		}

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
	// Claude CLI in print mode is stateless here, so we build a compact instruction prompt
	// around the latest user request and available Olive MCP tools.
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

	FString ToolNames;
	for (const FOliveToolDefinition& Tool : Tools)
	{
		if (!ToolNames.IsEmpty())
		{
			ToolNames += TEXT(", ");
		}
		ToolNames += Tool.Name;
	}

	const FString Header =
		TEXT("You are Olive AI running inside Unreal Editor.\n")
		TEXT("For Unreal asset operations, use available MCP tools instead of giving manual editor steps.\n")
		TEXT("Do not claim you cannot edit .uasset files directly when tools exist; call the appropriate tool.\n")
		TEXT("Keep responses concise. Do not narrate internal step-by-step tool troubleshooting.\n")
		TEXT("After completing operations, return one short final outcome summary.\n");

	if (!ToolNames.IsEmpty())
	{
		return FString::Printf(
			TEXT("%s\nAvailable tools: %s\n\nUser request:\n%s"),
			*Header,
			*ToolNames,
			*LastUserMessage);
	}

	return FString::Printf(TEXT("%s\nUser request:\n%s"), *Header, *LastUserMessage);
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
