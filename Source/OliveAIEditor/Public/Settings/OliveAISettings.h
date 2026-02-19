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
	OpenRouter UMETA(DisplayName = "OpenRouter (API Key)"),
	Anthropic UMETA(DisplayName = "Anthropic (API Key)"),
	OpenAI UMETA(DisplayName = "OpenAI (API Key)"),
	Google UMETA(DisplayName = "Google AI (API Key)"),
	Ollama UMETA(DisplayName = "Ollama (Local)")
};

/**
 * Confirmation tier overrides
 */
UENUM(BlueprintType)
enum class EOliveConfirmationTier : uint8
{
	Tier1_AutoExecute UMETA(DisplayName = "Auto-Execute"),
	Tier2_PlanConfirm UMETA(DisplayName = "Plan and Confirm"),
	Tier3_Preview UMETA(DisplayName = "Non-Destructive Preview")
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

	/** The model to use. For OpenRouter, use format: provider/model (e.g., anthropic/claude-sonnet-4) */
	UPROPERTY(Config, EditAnywhere, Category="AI Provider",
		meta=(DisplayName="Model"))
	FString SelectedModel = TEXT("anthropic/claude-sonnet-4");

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

	/** Default focus profile when opening the chat */
	UPROPERTY(Config, EditAnywhere, Category="User Interface",
		meta=(DisplayName="Default Focus Profile"))
	FString DefaultFocusProfile = TEXT("Auto");

	// ==========================================
	// Confirmation Settings
	// ==========================================

	/** Override tier for variable operations */
	UPROPERTY(Config, EditAnywhere, Category="Confirmation",
		meta=(DisplayName="Variable Operations"))
	EOliveConfirmationTier VariableOperationsTier = EOliveConfirmationTier::Tier1_AutoExecute;

	/** Override tier for component operations */
	UPROPERTY(Config, EditAnywhere, Category="Confirmation",
		meta=(DisplayName="Component Operations"))
	EOliveConfirmationTier ComponentOperationsTier = EOliveConfirmationTier::Tier1_AutoExecute;

	/** Override tier for function creation */
	UPROPERTY(Config, EditAnywhere, Category="Confirmation",
		meta=(DisplayName="Function Creation"))
	EOliveConfirmationTier FunctionCreationTier = EOliveConfirmationTier::Tier2_PlanConfirm;

	/** Override tier for graph editing */
	UPROPERTY(Config, EditAnywhere, Category="Confirmation",
		meta=(DisplayName="Graph Editing"))
	EOliveConfirmationTier GraphEditingTier = EOliveConfirmationTier::Tier2_PlanConfirm;

	/** Override tier for refactoring operations */
	UPROPERTY(Config, EditAnywhere, Category="Confirmation",
		meta=(DisplayName="Refactoring"))
	EOliveConfirmationTier RefactoringTier = EOliveConfirmationTier::Tier3_Preview;

	/** Override tier for delete operations */
	UPROPERTY(Config, EditAnywhere, Category="Confirmation",
		meta=(DisplayName="Delete Operations"))
	EOliveConfirmationTier DeleteOperationsTier = EOliveConfirmationTier::Tier3_Preview;

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

	// ==========================================
	// Utility Functions
	// ==========================================

	/** Get the singleton settings instance */
	static UOliveAISettings* Get();

	/** Get the API key for the currently selected provider */
	FString GetCurrentApiKey() const;

	/** Get the base URL for the currently selected provider */
	FString GetCurrentBaseUrl() const;

	/** Check if the current provider is configured (has API key) */
	bool IsProviderConfigured() const;
};
