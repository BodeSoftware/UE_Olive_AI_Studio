// Copyright Bode Software. All Rights Reserved.

/**
 * OliveCodexProvider.cpp
 *
 * OpenAI Codex CLI-specific implementation. All universal process management,
 * prompt building, and response handling live in FOliveCLIProviderBase.
 * This file contains only Codex-specific logic:
 * - Executable discovery and version detection
 * - CLI argument construction (exec --json)
 * - JSONL output format parsing
 * - Direct HTTP MCP connection (no bridge)
 */

#include "Providers/OliveCodexProvider.h"
#include "Settings/OliveAISettings.h"
#include "MCP/OliveMCPServer.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveCodex, Log, All);

// ==========================================
// FOliveCodexProvider
// ==========================================

FOliveCodexProvider::FOliveCodexProvider()
{
	// Use project directory as working dir (Codex doesn't need .mcp.json discovery)
	WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	UE_LOG(LogOliveCodex, Log, TEXT("Codex CLI working directory: %s"), *WorkingDirectory);
}

TArray<FString> FOliveCodexProvider::GetAvailableModels() const
{
	return {
		TEXT("o3"),
		TEXT("o4-mini"),
		TEXT("gpt-4.1"),
		TEXT("codex-mini-latest")
	};
}

FString FOliveCodexProvider::GetRecommendedModel() const
{
	return TEXT("o3");
}

void FOliveCodexProvider::Configure(const FOliveProviderConfig& Config)
{
	FOliveCLIProviderBase::Configure(Config);
	CurrentConfig.ProviderName = TEXT("codex");
}

bool FOliveCodexProvider::ValidateConfig(FString& OutError) const
{
	if (!IsCodexInstalled())
	{
		OutError = TEXT("Codex CLI not found. Install via: npm i -g @openai/codex");
		return false;
	}
	return true;
}

// ==========================================
// Static Helpers
// ==========================================

bool FOliveCodexProvider::IsCodexInstalled()
{
	return !GetCodexExecutablePath().IsEmpty();
}

FString FOliveCodexProvider::GetCodexExecutablePath()
{
#if PLATFORM_WINDOWS
	// 1. where.exe — finds codex.exe/.cmd anywhere on PATH
	FString WhichOutput;
	int32 ReturnCode;
	FPlatformProcess::ExecProcess(TEXT("where"), TEXT("codex"), &ReturnCode, &WhichOutput, nullptr);
	if (ReturnCode == 0 && !WhichOutput.IsEmpty())
	{
		// where may return multiple lines — scan all of them
		TArray<FString> Lines;
		WhichOutput.ParseIntoArrayLines(Lines);
		for (FString& Line : Lines)
		{
			Line.TrimStartAndEndInline();
			// Prefer a native .exe directly on PATH
			if (Line.EndsWith(TEXT(".exe")))
			{
				return Line;
			}
		}

		// No .exe found — resolve from the npm shim location to the native binary.
		// npm installs the platform-specific binary as a nested dependency:
		// {npm_dir}/node_modules/@openai/codex/node_modules/@openai/codex-win32-x64/vendor/.../codex.exe
		for (const FString& Line : Lines)
		{
			if (Line.EndsWith(TEXT(".cmd")) || !Line.Contains(TEXT(".")))
			{
				FString NpmDir = FPaths::GetPath(Line);
				// Check nested node_modules first (npm install structure)
				FString NestedBinary = FPaths::Combine(
					NpmDir,
					TEXT("node_modules/@openai/codex/node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/codex/codex.exe"));
				if (IFileManager::Get().FileExists(*NestedBinary))
				{
					return NestedBinary;
				}
				// Flat node_modules (hoisted)
				FString FlatBinary = FPaths::Combine(
					NpmDir,
					TEXT("node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/codex/codex.exe"));
				if (IFileManager::Get().FileExists(*FlatBinary))
				{
					return FlatBinary;
				}
			}
		}
	}

	// 2. npm global install via %APPDATA% — direct path check
	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	if (!AppData.IsEmpty())
	{
		// Nested node_modules structure
		FString NestedBinary = FPaths::Combine(
			AppData,
			TEXT("npm/node_modules/@openai/codex/node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/codex/codex.exe"));
		if (IFileManager::Get().FileExists(*NestedBinary))
		{
			return NestedBinary;
		}
		// Flat/hoisted structure
		FString FlatBinary = FPaths::Combine(
			AppData,
			TEXT("npm/node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/codex/codex.exe"));
		if (IFileManager::Get().FileExists(*FlatBinary))
		{
			return FlatBinary;
		}
	}
#else
	FString WhichOutput;
	int32 ReturnCode;
	FPlatformProcess::ExecProcess(TEXT("/usr/bin/which"), TEXT("codex"), &ReturnCode, &WhichOutput, nullptr);
	if (ReturnCode == 0 && !WhichOutput.IsEmpty())
	{
		WhichOutput.TrimStartAndEndInline();
		return WhichOutput;
	}
#endif

	return FString();
}

FString FOliveCodexProvider::GetCodexVersion()
{
	FString CodexPath = GetCodexExecutablePath();
	if (CodexPath.IsEmpty())
	{
		return TEXT("Not installed");
	}

	FString VersionOutput;
	int32 ReturnCode;
	FPlatformProcess::ExecProcess(*CodexPath, TEXT("--version"), &ReturnCode, &VersionOutput, nullptr);

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

FString FOliveCodexProvider::GetExecutablePath() const
{
	return GetCodexExecutablePath();
}

FString FOliveCodexProvider::GetCLIArguments(const FString& SystemPromptArg) const
{
	// Orchestrated mode is not fully supported for Codex (no --max-turns, no --append-system-prompt).
	// Fall through to autonomous mode args as a reasonable default.
	return GetCLIArgumentsAutonomous();
}

FString FOliveCodexProvider::GetCLIArgumentsAutonomous() const
{
	// Codex exec: non-interactive mode with JSONL output
	// --json: JSONL event stream to stdout
	// --dangerously-bypass-approvals-and-sandbox: all mutations go through MCP, not shell
	// --skip-git-repo-check: sandbox dir is not a git repo
	// --ephemeral: don't persist session files
	// -C <sandbox>: explicit working directory so Codex reads AGENTS.md from there
	// --add-dir <project>: grant access to project Content/ for MCP tool operations
	// -c mcp_servers.olive.url=...: direct HTTP MCP connection (no bridge)

	const int32 MCPPort = FOliveMCPServer::Get().GetActualPort();
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	const UOliveAISettings* Settings = UOliveAISettings::Get();
	FString ModelArg;
	if (Settings)
	{
		const FString Model = Settings->GetSelectedModelForProvider(EOliveAIProvider::Codex);
		if (!Model.IsEmpty())
		{
			ModelArg = FString::Printf(TEXT("-m %s "), *Model);
		}
	}

	return FString::Printf(
		TEXT("exec --json --dangerously-bypass-approvals-and-sandbox --skip-git-repo-check --ephemeral %s")
		TEXT("-C \"%s\" --add-dir \"%s\" ")
		TEXT("-c \"mcp_servers.olive.url=\\\"http://localhost:%d/mcp\\\"\""),
		*ModelArg,
		*AutonomousSandboxDir,
		*ProjectDir,
		MCPPort
	);
}

void FOliveCodexProvider::ParseOutputLine(const FString& Line)
{
	// Codex --json outputs JSONL events:
	// {"type":"thread.started","thread_id":"..."}
	// {"type":"turn.started"}
	// {"type":"item.started","item":{"id":"...","type":"agent_message"|"command_execution",...}}
	// {"type":"item.completed","item":{"id":"...","type":"agent_message","text":"..."}}
	// {"type":"item.completed","item":{"id":"...","type":"command_execution","command":"...","aggregated_output":"...","exit_code":0}}
	// {"type":"turn.completed","usage":{...}}

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		// Not JSON — treat as plain text
		if (!Line.StartsWith(TEXT("{")))
		{
			FScopeLock Lock(&CallbackLock);
			FOliveStreamChunk Chunk;
			Chunk.Text = Line;
			AccumulatedResponse += Line + TEXT("\n");
			CurrentOnChunk.ExecuteIfBound(Chunk);
		}
		return;
	}

	FString Type;
	if (!JsonObject->TryGetStringField(TEXT("type"), Type))
	{
		return;
	}

	if (Type == TEXT("item.completed"))
	{
		const TSharedPtr<FJsonObject>* ItemObj;
		if (!JsonObject->TryGetObjectField(TEXT("item"), ItemObj))
		{
			return;
		}

		FString ItemType;
		(*ItemObj)->TryGetStringField(TEXT("type"), ItemType);

		if (ItemType == TEXT("agent_message"))
		{
			FString Text;
			if ((*ItemObj)->TryGetStringField(TEXT("text"), Text) && !Text.IsEmpty())
			{
				FScopeLock Lock(&CallbackLock);
				FOliveStreamChunk Chunk;
				Chunk.Text = Text;
				AccumulatedResponse += Text;
				CurrentOnChunk.ExecuteIfBound(Chunk);
			}
		}
		else if (ItemType == TEXT("command_execution"))
		{
			// Codex executed a shell command (should be rare — MCP tools are preferred)
			FString Command;
			(*ItemObj)->TryGetStringField(TEXT("command"), Command);
			UE_LOG(LogOliveCodex, Log, TEXT("Agent executed command: %s"), *Command);

			FScopeLock Lock(&CallbackLock);
			FOliveStreamChunk Chunk;
			Chunk.Text = FString::Printf(TEXT("[Shell] %s"), *Command);
			CurrentOnChunk.ExecuteIfBound(Chunk);
		}
		else if (ItemType == TEXT("mcp_call"))
		{
			// MCP tool call completed
			FString ToolName;
			(*ItemObj)->TryGetStringField(TEXT("name"), ToolName);
			UE_LOG(LogOliveCodex, Log, TEXT("Agent MCP tool: %s"), *ToolName);

			FScopeLock Lock(&CallbackLock);
			FOliveStreamChunk Chunk;
			Chunk.Text = FString::Printf(TEXT("[Tool] %s"), *ToolName);
			CurrentOnChunk.ExecuteIfBound(Chunk);
		}
	}
	else if (Type == TEXT("item.started"))
	{
		const TSharedPtr<FJsonObject>* ItemObj;
		if (JsonObject->TryGetObjectField(TEXT("item"), ItemObj))
		{
			FString ItemType;
			(*ItemObj)->TryGetStringField(TEXT("type"), ItemType);

			if (ItemType == TEXT("command_execution"))
			{
				FString Command;
				(*ItemObj)->TryGetStringField(TEXT("command"), Command);
				UE_LOG(LogOliveCodex, Verbose, TEXT("Agent starting command: %s"), *Command);
			}
		}
	}
	else if (Type == TEXT("turn.completed"))
	{
		FScopeLock Lock(&CallbackLock);
		FOliveStreamChunk Chunk;
		Chunk.bIsComplete = true;
		Chunk.FinishReason = TEXT("stop");
		CurrentOnChunk.ExecuteIfBound(Chunk);
	}
	else if (Type == TEXT("error"))
	{
		FString ErrorMsg;
		JsonObject->TryGetStringField(TEXT("message"), ErrorMsg);
		if (ErrorMsg.IsEmpty())
		{
			JsonObject->TryGetStringField(TEXT("error"), ErrorMsg);
		}

		FScopeLock Lock(&CallbackLock);
		LastError = ErrorMsg;
		CurrentOnError.ExecuteIfBound(ErrorMsg);
	}
	// thread.started, turn.started — informational, no action needed
}

void FOliveCodexProvider::WriteProviderSpecificSandboxFiles(const FString& AgentContext)
{
	// Codex reads instruction files from a .codex/ directory in the working directory.
	// Write the agent context there so Codex picks it up even without a git repo.
	const FString CodexDir = FPaths::Combine(AutonomousSandboxDir, TEXT(".codex"));
	IFileManager::Get().MakeDirectory(*CodexDir, true);

	// Codex reads .codex/AGENTS.md for project-level instructions
	const FString InstructionsPath = FPaths::Combine(CodexDir, TEXT("AGENTS.md"));
	FFileHelper::SaveStringToFile(AgentContext, *InstructionsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	// Also initialize a minimal git repo so Codex treats this as a project root
	// and reads AGENTS.md from the working directory
	const FString GitDir = FPaths::Combine(AutonomousSandboxDir, TEXT(".git"));
	if (!IFileManager::Get().DirectoryExists(*GitDir))
	{
		IFileManager::Get().MakeDirectory(*GitDir, true);
		// Minimal git structure: HEAD file pointing to main
		const FString HeadPath = FPaths::Combine(GitDir, TEXT("HEAD"));
		FFileHelper::SaveStringToFile(TEXT("ref: refs/heads/main\n"), *HeadPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

// ==========================================
// Connection Validation
// ==========================================

void FOliveCodexProvider::ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const
{
	if (IsCodexInstalled())
	{
		FString Version = GetCodexVersion();
		Callback(true, FString::Printf(TEXT("Codex CLI detected. Version: %s"), *Version));
	}
	else
	{
		Callback(false, TEXT("Codex CLI not found. Install via: npm i -g @openai/codex"));
	}
}
