// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"
#include "MCP/OliveToolRegistry.h"
#include "Chat/OliveRunManager.h"
#include "Brain/OliveBrainLayer.h"
#include "Brain/OliveOperationHistory.h"
#include "Brain/OlivePromptDistiller.h"
#include "Brain/OliveRetryPolicy.h"
#include "Brain/OliveSelfCorrectionPolicy.h"

class FOliveMessageQueue;
class FOliveProviderRetryManager;

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

/** Fired when the active chat mode changes (Code/Plan/Ask) */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveChatModeChanged, EOliveChatMode /* NewMode */);

/** Fired when a mode switch is deferred because processing is active */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveChatModeSwitchDeferred, EOliveChatMode /* PendingMode */);

/**
 * Conversation Manager
 *
 * Manages the conversation state, message history, and coordinates
 * between the provider and tool execution. Handles the agentic loop
 * for tool calling. Owns the active chat mode (Code/Plan/Ask).
 * Mode switches during processing are deferred until the current
 * operation completes.
 */
class OLIVEAIEDITOR_API FOliveConversationManager : public TSharedFromThis<FOliveConversationManager>
{
public:
	FOliveConversationManager();
	~FOliveConversationManager();

	// ==========================================
	// Plan Session State (for Plan mode continuity)
	// ==========================================

	/**
	 * Plan Session State
	 * Stores compact structured state for ongoing planning conversations.
	 * Enables Plan mode to behave like a persistent planning session
	 * where follow-ups revise the active plan.
	 */
	struct FOlivePlanSessionState
	{
		bool bHasActivePlan = false;
		FString SessionId;
		FString GoalSummary;
		FString TargetSummary;
		FString ActiveModeContext;
		FString LatestPlanText;
		FString LatestPlanSummary;
		TArray<FString> UserPlanAdjustments;
		TArray<FString> KeyDecisions;
		TArray<FString> OutstandingQuestions;
		TArray<FString> PlannedAssetsOrArtifacts;
		FDateTime LastUpdatedUtc;

		void Reset();
		bool IsEmpty() const;
	};

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

	// ==========================================
	// Chat Mode
	// ==========================================

	/**
	 * Set the active chat mode. If the manager is currently processing,
	 * the switch is deferred until the current operation completes.
	 * @param NewMode The mode to switch to
	 */
	void SetChatMode(EOliveChatMode NewMode);

	/**
	 * Get the current active chat mode
	 * @return Active chat mode (Code, Plan, or Ask)
	 */
	EOliveChatMode GetChatMode() const { return ActiveChatMode; }

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

	/** Fired when the active chat mode changes */
	FOnOliveChatModeChanged OnModeChanged;

	/** Fired when a mode switch is deferred because processing is active */
	FOnOliveChatModeSwitchDeferred OnModeSwitchDeferred;

	// ==========================================
	// Run Mode
	// ==========================================

	/** Enable run mode for multi-step operations */
	void EnableRunMode(const FString& RunName);

	/** Disable run mode */
	void DisableRunMode();

	/** Check if run mode is active */
	bool IsRunModeActive() const { return bRunModeActive; }

	// ==========================================
	// Plan Session Management
	// ==========================================

	/**
	 * Update plan session from a user message
	 * Classifies the message as continuation or new task and updates session state
	 */
	void UpdatePlanSessionFromUserMessage(const FString& Message);

	/**
	 * Update plan session from an assistant message
	 * Extracts and stores plan artifact from AI responses in Plan mode
	 */
	void UpdatePlanSessionFromAssistantMessage(const FOliveChatMessage& AssistantMessage);

	/**
	 * Build continuation context for stateless providers
	 * Returns compact planning context to prepend to follow-up messages
	 */
	FString BuildPlanContinuationContext() const;

	/**
	 * Build execution context for Plan->Code handoff
	 * Returns approved plan summary to frame Code mode execution
	 */
	FString BuildPlanExecutionContext() const;

	/**
	 * Check if message should continue the active plan
	 * Returns true if message appears to continue current planning thread
	 */
	bool ShouldContinueActivePlan(const FString& Message) const;

	// ==========================================
	// Configuration
	// ==========================================

	/** Set system prompt */
	void SetSystemPrompt(const FString& Prompt);

	/** Get system prompt */
	const FString& GetSystemPrompt() const { return SystemPrompt; }

	/** Set maximum tool call iterations (to prevent infinite loops) */
	void SetMaxToolIterations(int32 Max) { MaxToolIterations = Max; }

	// ==========================================
	// Message Queue Integration
	// ==========================================

	/**
	 * Set the retry manager (owned externally by FOliveEditorChatSession).
	 * When set, SendToProvider() routes through RetryManager->SendWithRetry()
	 * instead of calling Provider->SendMessage() directly.
	 * @param InRetryManager Pointer to the retry manager. Lifetime must exceed this object's.
	 */
	void SetRetryManager(FOliveProviderRetryManager* InRetryManager);

	/**
	 * Set the message queue (owned externally by FOliveEditorChatSession).
	 * When the queue is set, messages sent while processing are enqueued
	 * instead of being rejected.
	 * @param InQueue Pointer to the queue. Lifetime must exceed this object's.
	 */
	void SetMessageQueue(FOliveMessageQueue* InQueue);

	/**
	 * Attempt to dequeue and send the next queued message.
	 * Should be called after processing completes (HandleComplete/HandleError).
	 * Safe to call when bIsProcessing is false -- it will call SendUserMessage()
	 * with the dequeued message, which re-enters the normal processing flow.
	 */
	void DrainNextQueuedMessage();

private:
	// ==========================================
	// Autonomous Mode
	// ==========================================

	/** Check if the active provider supports autonomous MCP mode and it's enabled in settings. */
	bool IsAutonomousProvider() const;

	/**
	 * Autonomous path: launch Claude Code with MCP tool discovery, no orchestration loop.
	 * The provider process runs until natural completion; tools are called via MCP server.
	 * @param Message User's message text
	 */
	void SendUserMessageAutonomous(const FString& Message);
	bool ShouldExecuteApprovedPlan(const FString& Message) const;
	void BeginRequestContext(const FString& Message);
	void ResetRequestContext();

	// ==========================================
	// Internal Message Handling
	// ==========================================

	/** Add a message to history */
	void AddMessage(const FOliveChatMessage& Message);

	/** Build the system message with context */
	FOliveChatMessage BuildSystemMessage();

	/** Get tools filtered by current chat mode */
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

	/** Active context paths */
	TArray<FString> ActiveContextPaths;

	/** Is processing a request */
	bool bIsProcessing = false;

	/** Pointer to external retry manager (not owned). Set via SetRetryManager(). */
	FOliveProviderRetryManager* RetryManager = nullptr;

	/** Pointer to external message queue (not owned). Set via SetMessageQueue(). */
	FOliveMessageQueue* Queue = nullptr;

	/** Whether run mode is active */
	bool bRunModeActive = false;

	/** Brain Layer -- owns state machine, loop detection, operation history */
	TSharedPtr<FOliveBrainLayer> Brain;

	// ==========================================
	// Chat Mode State
	// ==========================================

	/** Current chat mode (Code/Plan/Ask). Initialized from DefaultChatMode setting. */
	EOliveChatMode ActiveChatMode = EOliveChatMode::Code;
	EOliveChatMode RequestChatMode = EOliveChatMode::Code;
	bool bRequestExecutingApprovedPlan = false;

	/** Deferred mode switch -- applied when processing completes */
	TOptional<EOliveChatMode> DeferredChatMode;

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

	/** Snapshot of tool call arguments by ID, built in ProcessPendingToolCalls.
	 *  Used by HandleToolResult after PendingToolCalls is moved to a local. */
	TMap<FString, TSharedPtr<FJsonObject>> ActiveToolCallArgs;

	/** Count of failed tool results in the current batch (for correction directive) */
	int32 CurrentBatchFailureCount = 0;

	/** Accumulated correction summary for the current batch */
	FString CurrentBatchCorrectionSummary;

	/** Whether there are unresolved failures that require the AI to retry */
	bool bHasPendingCorrections = false;

	/** Plan session state (owned by ConversationManager for Plan mode continuity) */
	TUniquePtr<FOlivePlanSessionState> ActivePlanSession;

	/** Number of times we have re-prompted the AI to address corrections in the current turn */
	int32 CorrectionRepromptCount = 0;

	/** Maximum re-prompts for unresolved corrections per turn */
	static constexpr int32 MaxCorrectionReprompts = 2;

	/** Turn-level intent flag: used for multi-asset iteration budget and zero-tool re-prompt guard. */
	bool bTurnHasExplicitWriteIntent = false;

	/** Zero-tool-call re-prompt counter for write-intent tasks.
	 * Fires on ANY iteration where the AI responds text-only, not just the first.
	 * Budget of 3 allows recovery from premature stops across the agentic loop. */
	int32 ZeroToolRepromptCount = 0;
	static constexpr int32 MaxZeroToolReprompts = 3;

	/** Stop the current tool loop after results are added to history */
	bool bStopAfterToolResults = false;

	/** Skip remaining tools in current batch (foundational tool failed), but continue agentic loop */
	bool bSkipRemainingBatch = false;

	/** Name of the failed foundational tool (for skip messages) */
	FString FailedFoundationalTool;

	/** Asset paths that failed with ASSET_NOT_FOUND in current batch -- used to skip subsequent tools targeting the same dead path */
	TSet<FString> FailedAssetPaths;

	// ==========================================
	// Brain Layer Components
	// ==========================================

	/** Operation history for this session */
	FOliveOperationHistoryStore HistoryStore;

	/** Prompt distiller for token efficiency */
	FOlivePromptDistiller PromptDistiller;

	/** Loop detector for current run */
	FOliveLoopDetector LoopDetector;

	/** Self-correction policy */
	FOliveSelfCorrectionPolicy SelfCorrectionPolicy;

	/** Retry policy configuration */
	FOliveRetryPolicy RetryPolicy;

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
