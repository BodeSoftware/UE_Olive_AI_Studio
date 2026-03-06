// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"
#include "IOliveAIProvider.generated.h"

/**
 * Provider Configuration
 *
 * Settings for connecting to an AI provider.
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveProviderConfig
{
	GENERATED_BODY()

	/** Provider identifier ("openrouter", "anthropic", "openai", "google", "ollama") */
	UPROPERTY()
	FString ProviderName;

	/** API key (encrypted at rest in future) */
	UPROPERTY()
	FString ApiKey;

	/** Model identifier (e.g., "anthropic/claude-sonnet-4") */
	UPROPERTY()
	FString ModelId;

	/** Base URL for API (for self-hosted or proxy) */
	UPROPERTY()
	FString BaseUrl;

	/** Sampling temperature (0.0 = deterministic) */
	UPROPERTY()
	float Temperature = 0.0f;

	/** Maximum tokens to generate */
	UPROPERTY()
	int32 MaxTokens = 4096;

	/** Request timeout in seconds */
	UPROPERTY()
	int32 TimeoutSeconds = 120;
};

/**
 * Stream Chunk
 *
 * A piece of streamed response from the provider.
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveStreamChunk
{
	GENERATED_BODY()

	/** Text content (for regular response) */
	UPROPERTY()
	FString Text;

	/** Whether this chunk is a tool call */
	UPROPERTY()
	bool bIsToolCall = false;

	/** Tool call ID (for tracking multi-tool responses) */
	UPROPERTY()
	FString ToolCallId;

	/** Tool name being called */
	UPROPERTY()
	FString ToolName;

	/** Tool call arguments as JSON */
	TSharedPtr<FJsonObject> ToolArguments;

	/** Whether this is the final chunk */
	UPROPERTY()
	bool bIsComplete = false;

	/** Finish reason (if complete) */
	UPROPERTY()
	FString FinishReason;
};

/**
 * Provider Usage Statistics
 *
 * Token usage and cost estimation.
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveProviderUsage
{
	GENERATED_BODY()

	/** Tokens in the prompt */
	UPROPERTY()
	int32 PromptTokens = 0;

	/** Tokens in the completion */
	UPROPERTY()
	int32 CompletionTokens = 0;

	/** Total tokens used */
	UPROPERTY()
	int32 TotalTokens = 0;

	/** Estimated cost in USD */
	UPROPERTY()
	double EstimatedCostUSD = 0.0;

	/** Model used */
	UPROPERTY()
	FString Model;

	/** Finish reason from the provider (e.g. "stop", "length", "tool_calls", "MAX_TOKENS") */
	UPROPERTY()
	FString FinishReason;
};

/**
 * Chat Message Role
 */
UENUM()
enum class EOliveChatRole : uint8
{
	System,
	User,
	Assistant,
	Tool
};

/**
 * Chat Message
 *
 * A message in the conversation history.
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveChatMessage
{
	GENERATED_BODY()

	/** Message role */
	UPROPERTY()
	EOliveChatRole Role = EOliveChatRole::User;

	/** Message content */
	UPROPERTY()
	FString Content;

	/** Timestamp */
	UPROPERTY()
	FDateTime Timestamp;

	/** For tool responses: the tool call ID this responds to */
	UPROPERTY()
	FString ToolCallId;

	/** For tool responses: the tool name */
	UPROPERTY()
	FString ToolName;

	/** For assistant messages: any tool calls made */
	TArray<FOliveStreamChunk> ToolCalls;

	/** Convert role to string */
	static FString RoleToString(EOliveChatRole InRole);

	/** Convert string to role */
	static EOliveChatRole StringToRole(const FString& RoleStr);

	/** Convert to JSON for API */
	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * Per-request options that override provider config defaults.
 * Zero/negative sentinel values mean "use config default".
 */
struct OLIVEAIEDITOR_API FOliveRequestOptions
{
	/** Max tokens to generate. 0 = use config default. */
	int32 MaxTokens = 0;

	/** Sampling temperature. Negative = use config default. */
	float Temperature = -1.0f;

	/** Request timeout in seconds. 0 = use config default. */
	int32 TimeoutSeconds = 0;

	/** Whether these are default (all sentinels) */
	bool IsDefault() const
	{
		return MaxTokens <= 0 && Temperature < 0.0f && TimeoutSeconds <= 0;
	}
};

// ==========================================
// Delegates
// ==========================================

/** Called for each streamed chunk */
DECLARE_DELEGATE_OneParam(FOnOliveStreamChunk, const FOliveStreamChunk&);

/** Called when a tool call is detected in the stream */
DECLARE_DELEGATE_OneParam(FOnOliveToolCall, const FOliveStreamChunk&);

/** Called when the response is complete */
DECLARE_DELEGATE_TwoParams(FOnOliveComplete, const FString& /* FullResponse */, const FOliveProviderUsage&);

/** Called on error */
DECLARE_DELEGATE_OneParam(FOnOliveError, const FString& /* ErrorMessage */);

/**
 * AI Provider Interface
 *
 * Abstract interface for AI provider clients.
 * Implementations: ClaudeCode, OpenRouter, ZAI, Anthropic, OpenAI, Google, Ollama, OpenAICompatible
 *
 * IMPORTANT: If your provider builds its own system prompt instead of using
 * ConversationManager::BuildSystemMessage(), you MUST call
 * FOlivePromptAssembler::Get().BuildSharedSystemPreamble(ProfileName)
 * and include its output in your system prompt. This ensures your provider
 * gets recipe routing, knowledge packs, and other cross-cutting context
 * that API providers receive automatically via AssembleSystemPromptInternal().
 * See FOliveClaudeCodeProvider::BuildSystemPrompt() for reference.
 */
class OLIVEAIEDITOR_API IOliveAIProvider
{
public:
	virtual ~IOliveAIProvider() = default;

	// ==========================================
	// Provider Identity
	// ==========================================

	/** Get provider name (e.g., "OpenRouter") */
	virtual FString GetProviderName() const = 0;

	/** Get list of available models */
	virtual TArray<FString> GetAvailableModels() const = 0;

	/** Get recommended model for this provider */
	virtual FString GetRecommendedModel() const = 0;

	// ==========================================
	// Configuration
	// ==========================================

	/** Configure the provider */
	virtual void Configure(const FOliveProviderConfig& Config) = 0;

	/** Validate current configuration */
	virtual bool ValidateConfig(FString& OutError) const = 0;

	/** Get current configuration */
	virtual const FOliveProviderConfig& GetConfig() const = 0;

	// ==========================================
	// Requests
	// ==========================================

	/**
	 * Send a message with streaming response
	 * @param Messages Conversation history
	 * @param Tools Available tools for the model to call
	 * @param OnChunk Called for each text chunk
	 * @param OnToolCall Called when a tool call is detected
	 * @param OnComplete Called when response is complete
	 * @param OnError Called on error
	 */
	virtual void SendMessage(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools,
		FOnOliveStreamChunk OnChunk,
		FOnOliveToolCall OnToolCall,
		FOnOliveComplete OnComplete,
		FOnOliveError OnError,
		const FOliveRequestOptions& Options = FOliveRequestOptions()
	) = 0;

	/**
	 * Send a message in autonomous mode. The agent discovers tools via MCP
	 * and manages its own agentic loop. The plugin does not orchestrate turns,
	 * parse tool calls, or inject system prompts.
	 *
	 * Default implementation rejects -- only CLI-based providers override this.
	 *
	 * @param UserMessage  The user's task description sent to stdin
	 * @param OnChunk      Called for each streamed progress chunk
	 * @param OnComplete   Called when the autonomous process finishes
	 * @param OnError      Called on error or if autonomous mode is unsupported
	 */
	virtual void SendMessageAutonomous(
		const FString& UserMessage,
		FOnOliveStreamChunk OnChunk,
		FOnOliveComplete OnComplete,
		FOnOliveError OnError)
	{
		OnError.ExecuteIfBound(TEXT("Autonomous mode not supported by this provider"));
	}

	/**
	 * Cancel any in-flight request
	 */
	virtual void CancelRequest() = 0;

	// ==========================================
	// Status
	// ==========================================

	/**
	 * Whether this provider supports autonomous MCP mode.
	 * CLI-based providers (Claude Code, Codex) return true.
	 * API-based providers return false (default).
	 */
	virtual bool SupportsAutonomousMode() const { return false; }

	/** Check if a request is in progress */
	virtual bool IsBusy() const = 0;

	/** Get last error message */
	virtual FString GetLastError() const = 0;

	/**
	 * Async connection validation. Tests if the provider is reachable and configured correctly.
	 * @param Callback Called with (bSuccess, Message) — message is error detail on failure or "Connected" on success
	 */
	virtual void ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const
	{
		// Default implementation: just validate config
		FString Error;
		bool bValid = ValidateConfig(Error);
		Callback(bValid, bValid ? TEXT("Configuration valid") : Error);
	}
};

/**
 * Provider Factory
 *
 * Creates provider instances by name.
 */
class OLIVEAIEDITOR_API FOliveProviderFactory
{
public:
	/**
	 * Create a provider by name
	 * @param ProviderName Provider identifier
	 * @return Provider instance, or nullptr if not found
	 */
	static TSharedPtr<IOliveAIProvider> CreateProvider(const FString& ProviderName);

	/**
	 * Get list of available provider names
	 */
	static TArray<FString> GetAvailableProviders();
};
