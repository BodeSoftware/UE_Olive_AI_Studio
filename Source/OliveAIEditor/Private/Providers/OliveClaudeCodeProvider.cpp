// Copyright Bode Software. All Rights Reserved.

/**
 * OliveClaudeCodeProvider.cpp
 *
 * Claude Code CLI-specific implementation. All universal process management,
 * prompt building, and response handling live in FOliveCLIProviderBase.
 * This file contains only Claude-specific logic:
 * - Executable discovery and version detection
 * - CLI argument construction
 * - Stream-json output format parsing
 */

#include "Providers/OliveClaudeCodeProvider.h"
#include "Settings/OliveAISettings.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveClaudeCode, Log, All);

// ==========================================
// FOliveClaudeCodeProvider
// ==========================================

FOliveClaudeCodeProvider::FOliveClaudeCodeProvider()
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
	// Call base to store the config
	FOliveCLIProviderBase::Configure(Config);
	// Force provider name to "claudecode"
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

// ==========================================
// Static Helpers
// ==========================================

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
	if (ClaudePath.EndsWith(TEXT(".js")))
	{
		FString NodeArgs = FString::Printf(TEXT("\"%s\" --version"), *ClaudePath);
		FPlatformProcess::ExecProcess(TEXT("node"), *NodeArgs, &ReturnCode, &VersionOutput, nullptr);
	}
	else
	{
		FPlatformProcess::ExecProcess(*ClaudePath, TEXT("--version"), &ReturnCode, &VersionOutput, nullptr);
	}

	if (ReturnCode == 0)
	{
		VersionOutput.TrimStartAndEndInline();
		return VersionOutput;
	}

	return TEXT("Unknown");
}

// ==========================================
// FOliveCLIProviderBase Virtual Hook Overrides
// ==========================================

FString FOliveClaudeCodeProvider::GetExecutablePath() const
{
	return GetClaudeExecutablePath();
}

FString FOliveClaudeCodeProvider::GetCLIArguments(const FString& SystemPromptArg) const
{
	// --print: Output text instead of interactive UI
	// --output-format stream-json: for real-time JSON output (requires --verbose)
	// --verbose: Required when using stream-json output format
	// --dangerously-skip-permissions: avoid interactive prompts in editor context
	// --max-turns 1: single completion turn (ConversationManager owns the agentic loop)
	// --strict-mcp-config: ignore .mcp.json in the working directory -- prevents the CLI
	//   from discovering MCP tools on its own. ConversationManager is the sole orchestrator;
	//   tools are defined via system prompt text and parsed from <tool_call> XML blocks.
	// --append-system-prompt: inject domain-specific guidance and tool schemas
	return FString::Printf(
		TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --max-turns 1 --strict-mcp-config %s"),
		*SystemPromptArg
	);
}

FString FOliveClaudeCodeProvider::GetCLIArgumentsAutonomous() const
{
	// Autonomous MCP mode: Claude Code discovers tools via the MCP server and manages
	// its own agentic loop. Key differences from orchestrated GetCLIArguments():
	//
	// - NO --strict-mcp-config: allows Claude to discover the .mcp.json in the working
	//   directory and connect to the MCP server for tool calls.
	// - NO --append-system-prompt: AGENTS.md in the working directory provides domain
	//   context; tool schemas are discovered via MCP tools/list.
	// - --max-turns N: safety ceiling from settings, NOT orchestration. Each MCP tools/call
	//   counts as a turn. Complex multi-asset tasks easily need 40-60 tool calls.
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	const int32 MaxTurns = Settings ? Settings->AutonomousMaxTurns : 500;

	return FString::Printf(TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --max-turns %d"), MaxTurns);
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
		// In autonomous MCP mode, tool_use events are expected -- Claude Code calls
		// tools via the MCP server and reports each call in the stream-json output.
		// In orchestrated mode (--max-turns 1), these are unexpected but harmless.
		// Either way, emit a progress chunk so the UI shows tool activity.
		FString ToolName;
		JsonObject->TryGetStringField(TEXT("name"), ToolName);
		if (ToolName.IsEmpty())
		{
			// Some stream-json formats nest the name inside a content object
			const TSharedPtr<FJsonObject>* ContentObj;
			if (JsonObject->TryGetObjectField(TEXT("content"), ContentObj))
			{
				(*ContentObj)->TryGetStringField(TEXT("name"), ToolName);
			}
		}

		UE_LOG(LogOliveClaudeCode, Log, TEXT("Agent calling tool: %s"), *ToolName);

		FScopeLock Lock(&CallbackLock);
		FOliveStreamChunk Chunk;
		Chunk.Text = FString::Printf(TEXT("[Tool] Calling %s..."), *ToolName);
		CurrentOnChunk.ExecuteIfBound(Chunk);
	}
	else if (Type == TEXT("tool_result"))
	{
		// Informational: the MCP server already executed the tool and returned the result
		// to Claude Code. We just emit a brief status chunk for UI visibility.
		UE_LOG(LogOliveClaudeCode, Verbose, TEXT("Agent tool call completed"));

		FScopeLock Lock(&CallbackLock);
		FOliveStreamChunk Chunk;
		Chunk.Text = TEXT("[Tool] Complete");
		CurrentOnChunk.ExecuteIfBound(Chunk);
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
