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
	// Set working directory to project root
	// This is where Claude Code should run to have access to the full project
	WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

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
		TEXT("claude-sonnet-4-20250514"),
		TEXT("claude-opus-4-20250514"),
		TEXT("claude-haiku-3-5-20241022")
	};
}

FString FOliveClaudeCodeProvider::GetRecommendedModel() const
{
	return TEXT("claude-sonnet-4-20250514");
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
	FOnOliveError OnError
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

	// Build the prompt from messages
	FString Prompt = BuildPrompt(Messages);

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

		// Build arguments for claude
		// --print: Output text instead of interactive UI
		// --output-format stream-json: for real-time JSON output (requires --verbose)
		// --verbose: Required when using stream-json output format
		// --dangerously-skip-permissions: avoid interactive prompts in editor context
		FString ClaudeArgs = FString::Printf(
			TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions -p \"%s\""),
			*EscapedPrompt
		);

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

FString FOliveClaudeCodeProvider::BuildPrompt(const TArray<FOliveChatMessage>& Messages) const
{
	// For Claude CLI, we just send the last user message
	// The conversation history is managed by Claude Code internally
	for (int32 i = Messages.Num() - 1; i >= 0; --i)
	{
		if (Messages[i].Role == EOliveChatRole::User)
		{
			return Messages[i].Content;
		}
	}

	return TEXT("");
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
		// Tool call
		FScopeLock Lock(&CallbackLock);
		FOliveStreamChunk Chunk;
		Chunk.bIsToolCall = true;
		Chunk.ToolName = JsonObject->GetStringField(TEXT("name"));
		Chunk.ToolCallId = JsonObject->GetStringField(TEXT("id"));

		const TSharedPtr<FJsonObject>* InputObj;
		if (JsonObject->TryGetObjectField(TEXT("input"), InputObj))
		{
			Chunk.ToolArguments = *InputObj;
		}

		CurrentOnToolCall.ExecuteIfBound(Chunk);
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
