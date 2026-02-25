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
#include "Chat/OlivePromptAssembler.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveCLIProvider, Log, All);

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
	FString Prompt = BuildConversationPrompt(Messages, Tools);
	FString SystemPromptText = BuildCLISystemPrompt(Prompt, Tools);

	// Capture CLIName for use in the background lambda (avoids calling virtual in destructor race)
	const FString CLIName = GetCLIName();

	// Spawn process on background thread
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, Prompt, SystemPromptText, CLIName]()
	{
		FString ExePath = GetExecutablePath();
		if (ExePath.IsEmpty())
		{
			AsyncTask(ENamedThreads::GameThread, [this, CLIName]()
			{
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("%s CLI not found"), *CLIName));
			});
			return;
		}

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

		const FString ProcessWorkDir = GetWorkingDirectory();
		UE_LOG(LogOliveCLIProvider, Log, TEXT("Running: %s %s"), *Executable, *Args);
		UE_LOG(LogOliveCLIProvider, Log, TEXT("Working Directory: %s"), *ProcessWorkDir);

		// Create pipes for process communication
		void* StdoutRead = nullptr;
		void* StdoutWrite = nullptr;

		if (!FPlatformProcess::CreatePipe(StdoutRead, StdoutWrite))
		{
			AsyncTask(ENamedThreads::GameThread, [this, CLIName]()
			{
				FScopeLock Lock(&CallbackLock);
				bIsBusy = false;
				CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("Failed to create stdout pipe for %s process"), *CLIName));
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
			AsyncTask(ENamedThreads::GameThread, [this, CLIName]()
			{
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
			AsyncTask(ENamedThreads::GameThread, [this, CLIName]()
			{
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

		// Deliver the prompt via stdin. The FString overload appends a trailing newline,
		// which is harmless since CLI processes read stdin to EOF.
		FPlatformProcess::WritePipe(StdinWrite, Prompt);

		UE_LOG(LogOliveCLIProvider, Log, TEXT("Prompt delivered via stdin: %d chars"), Prompt.Len());

		// Close write end of stdin to signal EOF. Without this the child blocks forever
		// waiting for more input.
		FPlatformProcess::ClosePipe(nullptr, StdinWrite);
		StdinWrite = nullptr;

		// Read output inline (same approach as the original SendMessage)
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
						// Dispatch line parsing to game thread via virtual ParseOutputLine
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
			AsyncTask(ENamedThreads::GameThread, [this, OutputBuffer]()
			{
				FScopeLock Lock(&CallbackLock);
				if (!bIsBusy) return;
				ParseOutputLine(OutputBuffer);
			});
		}

		// Cleanup stdout pipe
		FPlatformProcess::ClosePipe(StdoutRead, nullptr);

		// Get return code
		int32 ReturnCode;
		FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
		FPlatformProcess::CloseProc(ProcessHandle);

		// Signal completion -- delegate to HandleResponseComplete for tool call parsing
		AsyncTask(ENamedThreads::GameThread, [this, ReturnCode]()
		{
			FScopeLock Lock(&CallbackLock);
			if (!bIsBusy) return;
			HandleResponseComplete(ReturnCode);
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
		Prompt += TEXT("- If the task is creating NEW Blueprints, start with blueprint.create (do NOT search first).\n");
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
