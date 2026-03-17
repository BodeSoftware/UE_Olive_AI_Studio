// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "OliveAISettings.generated.h"

/**
 * AI Provider type
 */
UENUM(BlueprintType)
enum class EOliveAIProvider : uint8
{
	ClaudeCode UMETA(DisplayName = "Claude Code CLI (No API Key)"),
	Codex      UMETA(DisplayName = "Codex CLI (ChatGPT / API Key)"),
	OpenRouter UMETA(DisplayName = "OpenRouter (API Key)"),
	ZAI        UMETA(DisplayName = "Z.ai (API Key)"),
	Anthropic UMETA(DisplayName = "Anthropic (API Key)"),
	OpenAI UMETA(DisplayName = "OpenAI (API Key)"),
	Google UMETA(DisplayName = "Google AI (API Key)"),
	Ollama UMETA(DisplayName = "Ollama (Local)"),
	OpenAICompatible UMETA(DisplayName = "OpenAI Compatible (Custom Endpoint)")
};

/**
 * Default chat mode for the built-in chat panel.
 * Mirrors EOliveChatMode (runtime enum in OliveBrainState.h) for Config serialization.
 */
UENUM(BlueprintType)
enum class EOliveChatModeConfig : uint8
{
	Code UMETA(DisplayName = "Code (Autonomous)"),
	Plan UMETA(DisplayName = "Plan (Review First)"),
	Ask  UMETA(DisplayName = "Ask (Read-Only)")
};

/**
 * Olive AI Studio Settings
 *
 * Configuration settings for the AI-powered development assistant.
 * Access via Project Settings > Plugins > Olive AI Studio.
 */
UCLASS(Config=Editor, DefaultConfig, meta=(DisplayName="Olive AI Studio"))
class OLIVEAIEDITOR_API UOliveAISettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UOliveAISettings();

	//~ Begin UObject Interface
	virtual void PostInitProperties() override;
	//~ End UObject Interface

	//~ Begin UDeveloperSettings Interface
	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("OliveAIStudio"); }
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif
	//~ End UDeveloperSettings Interface

	// ==========================================
	// AI Provider Settings
	// ==========================================

	/** The AI provider to use for built-in chat. Claude Code CLI requires no API key if you have a Claude Max subscription. */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Provider"))
	EOliveAIProvider Provider = EOliveAIProvider::ClaudeCode;

	/** OpenRouter API key. Get one at https://openrouter.ai/keys */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="OpenRouter API Key", PasswordField=true))
	FString OpenRouterApiKey;

	/** Z.ai API key */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Z.ai API Key", PasswordField=true))
	FString ZaiApiKey;

	/** Use Z.ai's coding endpoint variant (if supported by your account) */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Z.ai: Use Coding Endpoint"))
	bool bZaiUseCodingEndpoint = false;

	/** Anthropic API key for direct Claude access. Get one at https://console.anthropic.com */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Anthropic API Key", PasswordField=true))
	FString AnthropicApiKey;

	/** OpenAI API key. Get one at https://platform.openai.com */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="OpenAI API Key", PasswordField=true))
	FString OpenAIApiKey;

	/** Google AI Studio API key */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Google AI Key", PasswordField=true))
	FString GoogleApiKey;

	/** Ollama server URL for local models */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Ollama URL"))
	FString OllamaUrl = TEXT("http://localhost:11434");

	/** Custom OpenAI-compatible endpoint URL (e.g., http://localhost:1234/v1) */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Custom Endpoint URL"))
	FString OpenAICompatibleUrl;

	/** API key for custom endpoint (optional) */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Custom Endpoint API Key", PasswordField=true))
	FString OpenAICompatibleApiKey;

	/** The model to use. For OpenRouter, use format: provider/model (e.g., anthropic/claude-sonnet-4) */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Model"))
	FString SelectedModel = TEXT("anthropic/claude-sonnet-4");

	/** Internal JSON map of provider enum key -> last selected model id */
	UPROPERTY(Config)
	FString ProviderModelOverridesJson;

	/** Temperature for responses. 0 = deterministic, higher = more creative */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Temperature", ClampMin=0.0, ClampMax=2.0, UIMin=0.0, UIMax=2.0))
	float Temperature = 0.0f;

	/** Maximum tokens in the response */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Max Tokens", ClampMin=256, ClampMax=128000))
	int32 MaxTokens = 4096;

	/** Request timeout in seconds */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Timeout (seconds)", ClampMin=30, ClampMax=600))
	int32 RequestTimeoutSeconds = 120;

	/** Maximum retry attempts for transient network failures (0 = no retry) */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Max Retries", ClampMin=0, ClampMax=5))
	int32 MaxProviderRetries = 3;

	/** Maximum Retry-After seconds to honor for rate limits (beyond this, fail immediately) */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Max Rate Limit Wait (seconds)", ClampMin=0, ClampMax=300))
	int32 MaxRetryAfterWaitSeconds = 120;

	/** Use autonomous MCP mode for Claude Code CLI.
	 *  When enabled, Claude Code discovers tools via MCP and manages its own loop.
	 *  When disabled, the plugin orchestrates each turn (legacy behavior). */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Autonomous MCP Mode (Claude Code)"))
	bool bUseAutonomousMCPMode = true;

	/** Maximum total runtime for autonomous CLI mode (seconds). 0 = no limit.
	 *  Acts as a cost-control safety net. The activity and idle timeouts catch hung processes. */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Autonomous Max Runtime (seconds)", ClampMin=0, ClampMax=3600))
	int32 AutonomousMaxRuntimeSeconds = 1800;

	/** Maximum seconds with no MCP tool call before killing an autonomous CLI process.
	 *  This catches "thinking but not acting" -- the AI produces stdout but makes no progress.
	 *  Set to 0 to disable. The idle stdout timeout (120s) still catches fully hung processes. */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Autonomous Tool Idle Timeout (seconds)", ClampMin=0, ClampMax=600))
	int32 AutonomousIdleToolSeconds = 240;

	/** Crash-only safety ceiling for autonomous CLI mode — not a task budget.
	 *  Loop detection handles stuck runs; raise this only if legitimate tasks are being cut short.
	 *  Each MCP tools/call counts as one turn. */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Autonomous Max Turns", ClampMin=1, ClampMax=1000))
	int32 AutonomousMaxTurns = 500;

	// ==========================================
	// MCP Server Settings
	// ==========================================

	/** Automatically start the MCP server when the editor loads */
	UPROPERTY(Config, EditAnywhere, Category="MCP Server",
		meta=(DisplayName="Auto-Start Server"))
	bool bAutoStartMCPServer = true;

	/** Port for the MCP server. External agents connect to this port. */
	UPROPERTY(Config, EditAnywhere, Category="MCP Server",
		meta=(DisplayName="Server Port", ClampMin=1024, ClampMax=65535))
	int32 MCPServerPort = 3000;

	/** Maximum number of concurrent MCP client connections */
	UPROPERTY(Config, EditAnywhere, Category="MCP Server",
		meta=(DisplayName="Max Connections", ClampMin=1, ClampMax=10))
	int32 MaxMCPConnections = 5;

	// ==========================================
	// User Interface Settings
	// ==========================================

	/** Show the operation feed in chat messages */
	UPROPERTY(Config, EditAnywhere, Category="User Interface",
		meta=(DisplayName="Show Operation Feed"))
	bool bShowOperationFeed = true;

	/** Show quick action buttons below the chat input */
	UPROPERTY(Config, EditAnywhere, Category="User Interface",
		meta=(DisplayName="Show Quick Actions"))
	bool bShowQuickActions = true;

	/** Show desktop notification when AI completes a long operation */
	UPROPERTY(Config, EditAnywhere, Category="User Interface",
		meta=(DisplayName="Notify on Completion"))
	bool bNotifyOnCompletion = true;

	/** Play a sound when AI completes a long operation */
	UPROPERTY(Config, EditAnywhere, Category="User Interface",
		meta=(DisplayName="Sound on Completion"))
	bool bPlaySoundOnCompletion = false;

	/** Auto-scroll to the bottom when new messages arrive */
	UPROPERTY(Config, EditAnywhere, Category="User Interface",
		meta=(DisplayName="Auto-Scroll Chat"))
	bool bAutoScrollChat = true;

	/** Default chat mode when opening the chat panel.
	 *  Code: full autonomous execution. Plan: read + plan, writes require confirmation. Ask: read-only. */
	UPROPERTY(Config, EditAnywhere, Category="Chat",
		meta=(DisplayName="Default Chat Mode"))
	EOliveChatModeConfig DefaultChatMode = EOliveChatModeConfig::Code;

	// ==========================================
	// Policy Settings
	// ==========================================

	/** Maximum variables per Blueprint before warning */
	UPROPERTY(Config, EditAnywhere, Category="Policy",
		meta=(DisplayName="Max Variables Per Blueprint", ClampMin=10, ClampMax=200))
	int32 MaxVariablesPerBlueprint = 50;

	/** Maximum nodes per function before warning */
	UPROPERTY(Config, EditAnywhere, Category="Policy",
		meta=(DisplayName="Max Nodes Per Function", ClampMin=20, ClampMax=500))
	int32 MaxNodesPerFunction = 100;

	/** Enforce naming conventions (BP_ prefix, etc.) */
	UPROPERTY(Config, EditAnywhere, Category="Policy",
		meta=(DisplayName="Enforce Naming Conventions"))
	bool bEnforceNamingConventions = true;

	/** Auto-compile Blueprints after modifications */
	UPROPERTY(Config, EditAnywhere, Category="Policy",
		meta=(DisplayName="Auto-Compile After Write"))
	bool bAutoCompileAfterWrite = true;

	/** Maximum write operations allowed per minute (0 = unlimited). Rate-limits AI tool calls to prevent runaway loops. */
	UPROPERTY(Config, EditAnywhere, Category="Policy",
		meta=(DisplayName="Max Write Ops Per Minute", ClampMin=0, ClampMax=120))
	int32 MaxWriteOpsPerMinute = 120;

	/** Steps between automatic checkpoints in Run Mode (0 = manual only) */
	UPROPERTY(Config, EditAnywhere, Category="Policy",
		meta=(DisplayName="Checkpoint Interval (Steps)", ClampMin=0, ClampMax=50))
	int32 CheckpointIntervalSteps = 5;

	// ==========================================
	// Brain Layer Settings
	// ==========================================

	/** Maximum number of operations allowed in a single project.batch_write call */
	UPROPERTY(Config, EditAnywhere, Category = "Brain Layer", meta = (ClampMin = 1, ClampMax = 1000))
	int32 BatchWriteMaxOps = 200;

	/** Maximum number of assets returned by project.get_relevant_context */
	UPROPERTY(Config, EditAnywhere, Category = "Brain Layer", meta = (ClampMin = 1, ClampMax = 50))
	int32 RelevantContextMaxAssets = 10;

	/** Number of recent tool result pairs to keep at full detail in distilled prompts */
	UPROPERTY(Config, EditAnywhere, Category = "Brain Layer", meta = (ClampMin = 1, ClampMax = 5))
	int32 PromptDistillationRawResults = 2;

	/** Maximum correction cycles before the brain layer stops retrying a failed operation */
	UPROPERTY(Config, EditAnywhere, Category = "Brain Layer", meta = (ClampMin = 1, ClampMax = 20))
	int32 MaxCorrectionCyclesPerRun = 5;

	// ==========================================
	// Blueprint Plan JSON
	// ==========================================

	/** Enable intent-level Blueprint plan JSON tools (preview + apply). */
	UPROPERTY(Config, EditAnywhere, Category="Blueprint Plan",
		meta=(DisplayName="Enable Plan JSON Tools"))
	bool bEnableBlueprintPlanJsonTools = true;

	/** Maximum steps allowed in a single plan JSON */
	UPROPERTY(Config, EditAnywhere, Category="Blueprint Plan",
		meta=(DisplayName="Plan Max Steps", ClampMin=1, ClampMax=512))
	int32 PlanJsonMaxSteps = 128;

	/** When true, blueprint.apply_plan_json requires a preview_fingerprint from a prior preview call */
	UPROPERTY(Config, EditAnywhere, Category="Blueprint Plan",
		meta=(DisplayName="Require Preview Before Apply"))
	bool bPlanJsonRequirePreviewForApply = true;

	// ==========================================
	// Utility Model Settings
	// ==========================================

	/** Provider for the utility model (keyword expansion, error classification, etc.)
	 *  Should be a fast, cheap model. Leave as "None" to use the main provider as fallback. */
	UPROPERTY(Config, EditAnywhere, Category="Utility Model",
		meta=(DisplayName="Utility Model Provider"))
	EOliveAIProvider UtilityModelProvider = EOliveAIProvider::OpenRouter;

	/** Model ID for utility tasks. Should be fast/cheap.
	 *  Examples: "anthropic/claude-3-5-haiku-latest", "openai/gpt-4.1-nano", "google/gemini-2.0-flash" */
	UPROPERTY(Config, EditAnywhere, Category="Utility Model",
		meta=(DisplayName="Utility Model ID"))
	FString UtilityModelId = TEXT("anthropic/claude-3-5-haiku-latest");

	/** API key override for the utility model (optional — if empty, uses the key from the matching provider).
	 *  Useful if your utility model uses a different account or provider than your main model. */
	UPROPERTY(Config, EditAnywhere, Category="Utility Model",
		meta=(DisplayName="Utility Model API Key (Optional)", PasswordField=true))
	FString UtilityModelApiKey;

	/** Timeout in seconds for utility model requests */
	UPROPERTY(Config, EditAnywhere, Category="Utility Model",
		meta=(DisplayName="Utility Model Timeout (seconds)", ClampMin=5, ClampMax=30))
	int32 UtilityModelTimeoutSeconds = 10;

	/** Enable LLM-based keyword expansion for template pre-search.
	 *  When disabled, falls back to basic tokenizer extraction (less accurate for synonyms). */
	UPROPERTY(Config, EditAnywhere, Category="Utility Model",
		meta=(DisplayName="Enable LLM Keyword Expansion"))
	bool bEnableLLMKeywordExpansion = true;

	/** Enable template discovery pass before autonomous agent runs.
	 *  Pre-searches library, factory, and community templates using smart keywords,
	 *  then injects curated results into the agent's prompt for better template awareness. */
	UPROPERTY(Config, EditAnywhere, Category="Utility Model",
		meta=(DisplayName="Enable Template Discovery Pass"))
	bool bEnableTemplateDiscoveryPass = true;

	// ==========================================
	// Utility Functions
	// ==========================================

	/** Get the singleton settings instance */
	static UOliveAISettings* Get();

	/** Get the API key for the currently selected provider */
	FString GetCurrentApiKey() const;

	/** Get the API key for a specific provider (not necessarily the active one) */
	FString GetApiKeyForProvider(EOliveAIProvider InProvider) const;

	/** Get the base URL for a specific provider (not necessarily the active one) */
	FString GetBaseUrlForProvider(EOliveAIProvider InProvider) const;

	/** Get the base URL for the currently selected provider */
	FString GetCurrentBaseUrl() const;

	/** Check if the current provider is configured (has API key) */
	bool IsProviderConfigured() const;

	/** Get selected model for a specific provider, with legacy fallback for current provider */
	FString GetSelectedModelForProvider(EOliveAIProvider InProvider) const;

	/** Set selected model for a specific provider (updates legacy SelectedModel for active provider) */
	void SetSelectedModelForProvider(EOliveAIProvider InProvider, const FString& InModel);
};
