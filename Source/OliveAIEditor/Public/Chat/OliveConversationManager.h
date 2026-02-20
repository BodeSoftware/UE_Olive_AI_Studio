// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"
#include "MCP/OliveToolRegistry.h"

/**
 * Conversation Events
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveChatMessageAdded, const FOliveChatMessage&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveChatStreamChunkReceived, const FString&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnOliveChatToolCallStarted, const FString& /* ToolName */, const FString& /* ToolCallId */);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnOliveChatToolCallCompleted, const FString& /* ToolName */, const FString& /* ToolCallId */, const FOliveToolResult&);
DECLARE_MULTICAST_DELEGATE(FOnOliveChatProcessingStarted);
DECLARE_MULTICAST_DELEGATE(FOnOliveChatProcessingComplete);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveChatError, const FString&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnOliveChatConfirmationRequired, const FString& /* ToolCallId */, const FString& /* ToolName */, const FString& /* Plan */);

/**
 * Conversation Manager
 *
 * Manages the conversation state, message history, and coordinates
 * between the provider and tool execution. Handles the agentic loop
 * for tool calling.
 */
class OLIVEAIEDITOR_API FOliveConversationManager : public TSharedFromThis<FOliveConversationManager>
{
public:
	FOliveConversationManager();
	~FOliveConversationManager();

	// ==========================================
	// Session Management
	// ==========================================

	/** Start a new conversation session */
	void StartNewSession();

	/** Clear conversation history */
	void ClearHistory();

	/** Get current session ID */
	FGuid GetSessionId() const { return SessionId; }

	// ==========================================
	// Message Handling
	// ==========================================

	/**
	 * Send a user message
	 * @param Message User's message text
	 */
	void SendUserMessage(const FString& Message);

	/**
	 * Cancel the current request
	 */
	void CancelCurrentRequest();

	/**
	 * Check if processing a request
	 */
	bool IsProcessing() const { return bIsProcessing; }

	// ==========================================
	// History Access
	// ==========================================

	/** Get the full message history */
	const TArray<FOliveChatMessage>& GetMessageHistory() const { return MessageHistory; }

	/** Get message count */
	int32 GetMessageCount() const { return MessageHistory.Num(); }

	// ==========================================
	// Context Management
	// ==========================================

	/**
	 * Set active context (asset paths to include in system prompt)
	 * @param AssetPaths Paths to assets in context
	 */
	void SetActiveContext(const TArray<FString>& AssetPaths);

	/**
	 * Get active context paths
	 */
	const TArray<FString>& GetActiveContext() const { return ActiveContextPaths; }

	/**
	 * Set focus profile for tool filtering
	 * @param ProfileName Profile name
	 */
	void SetFocusProfile(const FString& ProfileName);

	/**
	 * Get current focus profile
	 */
	const FString& GetFocusProfile() const { return ActiveFocusProfile; }

	// ==========================================
	// Provider Management
	// ==========================================

	/**
	 * Set the AI provider
	 * @param InProvider Provider to use
	 */
	void SetProvider(TSharedPtr<IOliveAIProvider> InProvider);

	/**
	 * Get current provider
	 */
	TSharedPtr<IOliveAIProvider> GetProvider() const { return Provider; }

	// ==========================================
	// Events
	// ==========================================

	/** Fired when a message is added to history */
	FOnOliveChatMessageAdded OnMessageAdded;

	/** Fired for each text chunk during streaming */
	FOnOliveChatStreamChunkReceived OnStreamChunk;

	/** Fired when a tool call starts */
	FOnOliveChatToolCallStarted OnToolCallStarted;

	/** Fired when a tool call completes */
	FOnOliveChatToolCallCompleted OnToolCallCompleted;

	/** Fired when processing starts */
	FOnOliveChatProcessingStarted OnProcessingStarted;

	/** Fired when processing completes */
	FOnOliveChatProcessingComplete OnProcessingComplete;

	/** Fired on error */
	FOnOliveChatError OnError;

	/** Fired when a tool call requires user confirmation */
	FOnOliveChatConfirmationRequired OnConfirmationRequired;

	// ==========================================
	// Confirmation Flow
	// ==========================================

	/** Confirm pending operation and resume agentic loop */
	void ConfirmPendingOperation();

	/** Deny pending operation and resume agentic loop with denial result */
	void DenyPendingOperation();

	/** Check if waiting for user confirmation */
	bool IsWaitingForConfirmation() const { return bWaitingForConfirmation; }

	// ==========================================
	// Configuration
	// ==========================================

	/** Set system prompt */
	void SetSystemPrompt(const FString& Prompt);

	/** Get system prompt */
	const FString& GetSystemPrompt() const { return SystemPrompt; }

	/** Set maximum tool call iterations (to prevent infinite loops) */
	void SetMaxToolIterations(int32 Max) { MaxToolIterations = Max; }

private:
	// ==========================================
	// Internal Message Handling
	// ==========================================

	/** Add a message to history */
	void AddMessage(const FOliveChatMessage& Message);

	/** Build the system message with context */
	FOliveChatMessage BuildSystemMessage();

	/** Get tools for current focus profile */
	TArray<FOliveToolDefinition> GetAvailableTools();

	/** Send request to provider */
	void SendToProvider();

	// ==========================================
	// Provider Callbacks
	// ==========================================

	void HandleStreamChunk(const FOliveStreamChunk& Chunk);
	void HandleToolCall(const FOliveStreamChunk& ToolCall);
	void HandleComplete(const FString& FullResponse, const FOliveProviderUsage& Usage);
	void HandleError(const FString& ErrorMessage);

	// ==========================================
	// Tool Execution
	// ==========================================

	/** Process pending tool calls */
	void ProcessPendingToolCalls();

	/** Execute a single tool call */
	void ExecuteToolCall(const FOliveStreamChunk& ToolCall);

	/** Handle tool result and continue conversation */
	void HandleToolResult(const FString& ToolCallId, const FString& ToolName, const FOliveToolResult& Result);

	/** Continue conversation after tool results */
	void ContinueAfterToolResults();

	// ==========================================
	// State
	// ==========================================

	/** Current session ID */
	FGuid SessionId;

	/** Message history */
	TArray<FOliveChatMessage> MessageHistory;

	/** AI Provider */
	TSharedPtr<IOliveAIProvider> Provider;

	/** System prompt */
	FString SystemPrompt;

	/** Active focus profile */
	FString ActiveFocusProfile = TEXT("Auto");

	/** Active context paths */
	TArray<FString> ActiveContextPaths;

	/** Is processing a request */
	bool bIsProcessing = false;

	// ==========================================
	// Streaming State
	// ==========================================

	/** Currently streaming message */
	FString CurrentStreamingContent;

	/** Pending tool calls from current response */
	TArray<FOliveStreamChunk> PendingToolCalls;

	/** Pending tool results */
	TArray<FOliveChatMessage> PendingToolResults;

	/** Current iteration in tool calling loop */
	int32 CurrentToolIteration = 0;

	/** Maximum tool iterations */
	int32 MaxToolIterations = 10;

	/** Number of pending tool executions */
	int32 PendingToolExecutions = 0;

	// ==========================================
	// Confirmation State
	// ==========================================

	/** Whether we are waiting for user confirmation */
	bool bWaitingForConfirmation = false;

	/** Pending confirmation tool call ID */
	FString PendingConfirmationToolCallId;

	/** Pending confirmation tool name */
	FString PendingConfirmationToolName;

	/** Pending confirmation tool arguments */
	TSharedPtr<FJsonObject> PendingConfirmationArguments;

	// ==========================================
	// Compile Self-Correction
	// ==========================================

	/** Number of compile retries in current agentic loop */
	int32 CompileRetryCount = 0;

	/** Maximum compile retries before giving up */
	static constexpr int32 MaxCompileRetries = 3;

	// ==========================================
	// Token Management
	// ==========================================

	/** Estimate token count for text */
	int32 EstimateTokens(const FString& Text) const;

	/** Total tokens used in session */
	int32 TotalTokensUsed = 0;

	/** Maximum context tokens */
	int32 MaxContextTokens = 100000;
};
