// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/OliveCLIProviderBase.h"

/**
 * Claude Code CLI Provider
 *
 * Implements the Claude Code CLI as an AI provider by inheriting from
 * FOliveCLIProviderBase and overriding the provider-specific hooks.
 * All universal process management (stdin/stdout pipes, prompt building,
 * tool call parsing) lives in the base class.
 *
 * Claude-specific responsibilities:
 * - Executable path discovery (npm global install, .cmd resolution, etc.)
 * - CLI argument construction (--print, --output-format stream-json, etc.)
 * - Stream-json output format parsing (ParseOutputLine)
 * - Version detection and installation checks
 *
 * Requirements:
 * - Claude Code CLI installed and in PATH (or npm global install)
 * - User authenticated with Claude Max or API key
 */
class OLIVEAIEDITOR_API FOliveClaudeCodeProvider : public FOliveCLIProviderBase
{
public:
	FOliveClaudeCodeProvider();

	// ==========================================
	// IOliveAIProvider Interface (Claude-specific overrides)
	// ==========================================

	virtual FString GetProviderName() const override { return TEXT("Claude Code CLI"); }
	virtual TArray<FString> GetAvailableModels() const override;
	virtual FString GetRecommendedModel() const override;

	virtual bool ValidateConfig(FString& OutError) const override;
	virtual void Configure(const FOliveProviderConfig& Config) override;
	virtual void ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const override;

	// ==========================================
	// Claude Code Specific
	// ==========================================

	/**
	 * Check if Claude Code CLI is installed and accessible
	 */
	static bool IsClaudeCodeInstalled();

	/**
	 * Get the path to the claude executable.
	 * Searches npm global install, .cmd resolution, common install paths.
	 */
	static FString GetClaudeExecutablePath();

	/**
	 * Get Claude Code version string by running claude --version.
	 */
	static FString GetClaudeCodeVersion();

protected:
	// ==========================================
	// FOliveCLIProviderBase Virtual Hooks
	// ==========================================

	/** Claude is an Anthropic provider — skip prescriptive guidance in AGENTS.md */
	virtual bool IsAnthropicProvider() const override { return true; }

	/** Returns the Claude Code CLI executable path */
	virtual FString GetExecutablePath() const override;

	/**
	 * Builds Claude-specific CLI arguments for orchestrated mode:
	 * --print --output-format stream-json --verbose --dangerously-skip-permissions
	 * --max-turns 1 --strict-mcp-config <system-prompt-arg>
	 */
	virtual FString GetCLIArguments(const FString& SystemPromptArg) const override;

	/**
	 * Builds Claude-specific CLI arguments for autonomous MCP mode.
	 * No --strict-mcp-config (Claude discovers tools via MCP), no --append-system-prompt
	 * (AGENTS.md provides context), --max-turns 50 as a safety ceiling.
	 * @return Argument string for autonomous execution
	 */
	virtual FString GetCLIArgumentsAutonomous() const override;

	/**
	 * Parses Claude's stream-json output format.
	 * Handles: "assistant" (content text), "result" (completion),
	 * "tool_use"/"tool_call" (unexpected internal calls), "message_stop", "error".
	 */
	virtual void ParseOutputLine(const FString& Line) override;

	/** Returns "Claude" for error messages */
	virtual FString GetCLIName() const override { return TEXT("Claude"); }

	/**
	 * Writes .mcp.json (pointing to mcp-bridge.js) and CLAUDE.md into the sandbox.
	 * Codex connects to MCP directly via HTTP; Claude Code needs the stdio bridge.
	 */
	virtual void WriteProviderSpecificSandboxFiles(const FString& AgentContext) override;
};

/**
 * Reader thread for claude process stdout.
 *
 * @deprecated Use FOliveCLIReaderRunnable from OliveCLIProviderBase.h instead.
 * This typedef is retained for source compatibility with any external references.
 */
using FOliveClaudeReaderRunnable = FOliveCLIReaderRunnable;
