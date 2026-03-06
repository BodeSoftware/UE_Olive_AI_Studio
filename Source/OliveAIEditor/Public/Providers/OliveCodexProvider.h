// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/OliveCLIProviderBase.h"

/**
 * OpenAI Codex CLI Provider
 *
 * Implements the Codex CLI as an AI provider by inheriting from
 * FOliveCLIProviderBase and overriding the provider-specific hooks.
 *
 * Key differences from Claude Code:
 * - Uses `codex exec --json` instead of `claude --print --output-format stream-json`
 * - Connects directly to MCP HTTP server (no mcp-bridge.js needed)
 * - No --append-system-prompt; context via AGENTS.md and stdin only
 * - No --max-turns; relies on process timeouts for runaway protection
 * - JSONL output format: thread.started, turn.started, item.started/completed, turn.completed
 *
 * Requirements:
 * - Codex CLI installed (npm i -g @openai/codex)
 * - User authenticated via `codex login` (ChatGPT plan) or OPENAI_API_KEY env var
 */
class OLIVEAIEDITOR_API FOliveCodexProvider : public FOliveCLIProviderBase
{
public:
	FOliveCodexProvider();

	// IOliveAIProvider identity
	virtual FString GetProviderName() const override { return TEXT("Codex CLI"); }
	virtual TArray<FString> GetAvailableModels() const override;
	virtual FString GetRecommendedModel() const override;

	virtual bool ValidateConfig(FString& OutError) const override;
	virtual void Configure(const FOliveProviderConfig& Config) override;
	virtual void ValidateConnection(TFunction<void(bool, const FString&)> Callback) const override;

	// Static helpers
	static bool IsCodexInstalled();
	static FString GetCodexExecutablePath();
	static FString GetCodexVersion();
	static bool ExtractMcpToolNameFromJsonLine(const FString& Line, FString& OutToolName);

protected:
	// FOliveCLIProviderBase virtual hooks
	virtual FString GetExecutablePath() const override;
	virtual FString GetCLIArguments(const FString& SystemPromptArg) const override;
	virtual FString GetCLIArgumentsAutonomous() const override;
	virtual void ParseOutputLine(const FString& Line) override;
	virtual FString GetCLIName() const override { return TEXT("Codex"); }

	/**
	 * No-op: Codex discovers MCP via -c CLI flag, not config files.
	 * AGENTS.md is written by the base class.
	 */
	virtual void WriteProviderSpecificSandboxFiles(const FString& AgentContext) override;
};
