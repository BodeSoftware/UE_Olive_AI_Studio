// Copyright Bode Software. All Rights Reserved.

#include "Chat/OliveConversationManager.h"
#include "Chat/OliveMessageQueue.h"
#include "Chat/OlivePromptAssembler.h"
#include "Providers/OliveProviderRetryManager.h"
#include "Providers/OliveCLIProviderBase.h"
#include "MCP/OliveToolRegistry.h"
#include "MCP/OliveMCPServer.h"
#include "Index/OliveProjectIndex.h"
#include "Settings/OliveAISettings.h"
// Focus profile system removed -- replaced by EOliveChatMode (Code/Plan/Ask)
#include "OliveAIEditorModule.h"
#include "Brain/OliveToolExecutionContext.h"
#include "Chat/OliveRunManager.h"
#include "OliveSnapshotManager.h"
#include "Pipeline/OliveWritePipeline.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"

namespace
{
FString NormalizePlanText(const FString& Text)
{
	FString Normalized = Text;
	Normalized.ReplaceInline(TEXT("\r"), TEXT(" "));
	Normalized.ReplaceInline(TEXT("\n"), TEXT(" "));
	while (Normalized.ReplaceInline(TEXT("  "), TEXT(" ")) > 0)
	{
	}
	return Normalized.TrimStartAndEnd();
}

FString SummarizePlanText(const FString& Text, const int32 MaxLen = 240)
{
	const FString Normalized = NormalizePlanText(Text);
	if (Normalized.Len() <= MaxLen)
	{
		return Normalized;
	}

	return Normalized.Left(MaxLen) + TEXT("...");
}

bool MessageStartsNewPlanTask(const FString& Message)
{
	const FString Lower = Message.TrimStartAndEnd().ToLower();
	if (Lower.Contains(TEXT("switch to code mode"))
		|| Lower.Contains(TEXT("switch to code"))
		|| Lower.Contains(TEXT("do it"))
		|| Lower.Contains(TEXT("build it"))
		|| Lower.Contains(TEXT("implement it")))
	{
		return false;
	}

	static const TArray<FString> NewTaskPhrases = {
		TEXT("new task"),
		TEXT("different task"),
		TEXT("another task"),
		TEXT("instead"),
		TEXT("separate task"),
		TEXT("for a different"),
		TEXT("now plan")
	};

	for (const FString& Phrase : NewTaskPhrases)
	{
		if (Lower.StartsWith(Phrase) || Lower.Contains(TEXT(" ") + Phrase))
		{
			return true;
		}
	}

	return false;
}

void AppendUniquePlanItem(TArray<FString>& Items, const FString& Candidate, const int32 MaxItems = 6)
{
	FString Cleaned = Candidate.TrimStartAndEnd();
	if (Cleaned.IsEmpty())
	{
		return;
	}

	Cleaned.RemoveFromStart(TEXT("- "));
	Cleaned.RemoveFromStart(TEXT("* "));
	Cleaned.RemoveFromStart(TEXT("1. "));
	Cleaned.RemoveFromStart(TEXT("2. "));
	Cleaned.RemoveFromStart(TEXT("3. "));
	Cleaned.RemoveFromStart(TEXT("4. "));

	for (const FString& Existing : Items)
	{
		if (Existing.Equals(Cleaned, ESearchCase::IgnoreCase))
		{
			return;
		}
	}

	if (Items.Num() < MaxItems)
	{
		Items.Add(Cleaned);
	}
}

bool IsLikelyPlanQuestion(const FString& Line)
{
	const FString Lower = Line.ToLower();
	return Line.Contains(TEXT("?"))
		|| Lower.Contains(TEXT("question"))
		|| Lower.Contains(TEXT("need to decide"))
		|| Lower.Contains(TEXT("confirm whether"))
		|| Lower.Contains(TEXT("should we"));
}

bool IsLikelyPlanAssetLine(const FString& Line)
{
	const FString Lower = Line.ToLower();
	return Line.Contains(TEXT("/Game/"))
		|| Line.Contains(TEXT(".h"))
		|| Line.Contains(TEXT(".cpp"))
		|| Lower.Contains(TEXT("blueprint"))
		|| Lower.Contains(TEXT("widget"))
		|| Lower.Contains(TEXT("component"))
		|| Lower.Contains(TEXT("behavior tree"))
		|| Lower.Contains(TEXT("blackboard"))
		|| Lower.Contains(TEXT("pcg"))
		|| Lower.Contains(TEXT("asset"));
}

bool IsPlanToCodeHandoffCue(const FString& Message)
{
	static const TArray<FString> HandoffKeywords = {
		TEXT("implement"), TEXT("build it"), TEXT("go ahead"), TEXT("proceed"),
		TEXT("do it"), TEXT("start building"), TEXT("create it"), TEXT("use the plan"),
		TEXT("approve"), TEXT("approved"), TEXT("ship it")
	};

	const FString Lower = Message.ToLower();
	for (const FString& Keyword : HandoffKeywords)
	{
		if (Lower.Contains(Keyword))
		{
			return true;
		}
	}

	return false;
}

bool MessageContainsAnyKeyword(const FString& Message, const TArray<FString>& Keywords)
{
	FString Normalized = Message.ToLower();
	Normalized.ReplaceInline(TEXT("."), TEXT(" "));
	Normalized.ReplaceInline(TEXT(","), TEXT(" "));
	Normalized.ReplaceInline(TEXT(":"), TEXT(" "));
	Normalized.ReplaceInline(TEXT(";"), TEXT(" "));
	Normalized.ReplaceInline(TEXT("!"), TEXT(" "));
	Normalized.ReplaceInline(TEXT("?"), TEXT(" "));
	Normalized = FString::Printf(TEXT(" %s "), *Normalized);
	for (const FString& Keyword : Keywords)
	{
		const FString BoundedKeyword = FString::Printf(TEXT(" %s "), *Keyword);
		if (Normalized.Contains(BoundedKeyword))
		{
			return true;
		}
	}
	return false;
}

bool DetectWriteIntent(const FString& UserMessage)
{
	static const TArray<FString> WriteKeywords = {
		TEXT("add"),
		TEXT("create"),
		TEXT("edit"),
		TEXT("modify"),
		TEXT("update"),
		TEXT("change"),
		TEXT("rename"),
		TEXT("connect"),
		TEXT("wire"),
		TEXT("set"),
		TEXT("remove"),
		TEXT("delete"),
		TEXT("refactor"),
		TEXT("make"),
		TEXT("build"),
		TEXT("implement"),
		TEXT("spawn"),
		TEXT("fire")
	};
	return MessageContainsAnyKeyword(UserMessage, WriteKeywords);
}

bool IsFoundationalTool(const FString& ToolName)
{
	return ToolName == TEXT("blueprint.create")
		|| ToolName == TEXT("behaviortree.create")
		|| ToolName == TEXT("pcg.create_graph")
		|| ToolName == TEXT("cpp.create_class")
		|| ToolName == TEXT("blueprint.preview_plan_json");
}

bool DetectMultiAssetIntent(const FString& UserMessage)
{
	static const TArray<FString> MultiAssetPatterns = {
		TEXT(" and "), TEXT(" with "), TEXT("system"),
		TEXT("both"), TEXT("each"), TEXT("multiple")
	};

	const FString Lower = UserMessage.ToLower();
	if (!MessageContainsAnyKeyword(Lower,
		{TEXT("create"), TEXT("make"), TEXT("build"), TEXT("implement")}))
	{
		return false;
	}

	for (const FString& Pattern : MultiAssetPatterns)
	{
		if (Lower.Contains(Pattern))
			return true;
	}
	return false;
}

FOliveRequestOptions BuildRequestOptions()
{
	FOliveRequestOptions Options;
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		return Options;
	}

	Options.MaxTokens = FMath::Clamp(Settings->MaxTokens, 256, 8192);
	Options.TimeoutSeconds = FMath::Clamp(Settings->RequestTimeoutSeconds, 30, 300);
	Options.Temperature = FMath::Clamp(Settings->Temperature, 0.0f, 2.0f);
	return Options;
}
} // namespace

void FOliveConversationManager::FOlivePlanSessionState::Reset()
{
	bHasActivePlan = false;
	SessionId.Reset();
	GoalSummary.Reset();
	TargetSummary.Reset();
	ActiveModeContext.Reset();
	LatestPlanText.Reset();
	LatestPlanSummary.Reset();
	UserPlanAdjustments.Reset();
	KeyDecisions.Reset();
	OutstandingQuestions.Reset();
	PlannedAssetsOrArtifacts.Reset();
	LastUpdatedUtc = FDateTime();
}

bool FOliveConversationManager::FOlivePlanSessionState::IsEmpty() const
{
	return !bHasActivePlan
		&& SessionId.IsEmpty()
		&& GoalSummary.IsEmpty()
		&& TargetSummary.IsEmpty()
		&& ActiveModeContext.IsEmpty()
		&& LatestPlanText.IsEmpty()
		&& LatestPlanSummary.IsEmpty()
		&& UserPlanAdjustments.Num() == 0
		&& KeyDecisions.Num() == 0
		&& OutstandingQuestions.Num() == 0
		&& PlannedAssetsOrArtifacts.Num() == 0;
}

FOliveConversationManager::FOliveConversationManager()
{
	Brain = MakeShared<FOliveBrainLayer>();
	StartNewSession();

	// Wire retry policy from settings
	if (const UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		RetryPolicy.MaxCorrectionCyclesPerWorker = Settings->MaxCorrectionCyclesPerRun;
	}
}

namespace OliveConversationManagerInternal
{
/** Build distilled operation history context for injection into the system prompt */
FString BuildOperationHistoryContext(const FOliveOperationHistoryStore& HistoryStore)
{
	if (HistoryStore.GetTotalRecordCount() == 0)
	{
		return TEXT("");
	}

	const UOliveAISettings* Settings = UOliveAISettings::Get();
	const int32 RawResults = Settings ? Settings->PromptDistillationRawResults : 2;
	const int32 TokenBudget = 2000;

	return HistoryStore.BuildModelContext(TokenBudget, RawResults);
}
}

FOliveConversationManager::~FOliveConversationManager()
{
	CancelCurrentRequest();
}

// ==========================================
// Session Management
// ==========================================

void FOliveConversationManager::StartNewSession()
{
	ClearHistory();
	SessionId = FGuid::NewGuid();
	CurrentToolIteration = 0;
	TotalTokensUsed = 0;

	// Initialize chat mode from settings
	if (const UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		ActiveChatMode = ChatModeFromConfig(Settings->DefaultChatMode);
	}
	else
	{
		ActiveChatMode = EOliveChatMode::Code;
	}
	DeferredChatMode.Reset();
	if (!ActivePlanSession.IsValid())
	{
		ActivePlanSession = MakeUnique<FOlivePlanSessionState>();
	}
	if (!ActivePlanSession->IsEmpty())
	{
		UE_LOG(LogOliveAI, Log, TEXT("Reset plan session state for new conversation session"));
	}
	ActivePlanSession->Reset();

	// Reset provider session so the next message starts a fresh CLI session
	if (Provider.IsValid())
	{
		Provider->ResetSession();
	}

	UE_LOG(LogOliveAI, Log, TEXT("Started new conversation session: %s (mode: %s)"),
		*SessionId.ToString(), LexToString(ActiveChatMode));
}

void FOliveConversationManager::ClearHistory()
{
	CancelCurrentRequest();
	MessageHistory.Empty();
	PendingToolCalls.Empty();
	PendingToolResults.Empty();
	ActiveToolCallArgs.Reset();
	CurrentStreamingContent.Empty();
	bTurnHasExplicitWriteIntent = false;
	bStopAfterToolResults = false;
	CurrentBatchFailureCount = 0;
	CurrentBatchCorrectionSummary.Empty();
	bHasPendingCorrections = false;
	CorrectionRepromptCount = 0;

	// Clear any queued messages so they are not sent into a fresh session
	if (Queue)
	{
		Queue->Clear();
	}

	// Clear any deferred mode switch
	DeferredChatMode.Reset();

	if (ActivePlanSession.IsValid() && !ActivePlanSession->IsEmpty())
	{
		UE_LOG(LogOliveAI, Log, TEXT("Reset plan session state while clearing history"));
		ActivePlanSession->Reset();
	}
}

// ==========================================
// Autonomous Mode
// ==========================================

bool FOliveConversationManager::IsAutonomousProvider() const
{
	if (!Provider.IsValid())
	{
		return false;
	}

	const UOliveAISettings* Settings = UOliveAISettings::Get();
	const bool bAutonomousEnabled = Settings && Settings->bUseAutonomousMCPMode;

	return bAutonomousEnabled && Provider->SupportsAutonomousMode();
}

void FOliveConversationManager::SendUserMessageAutonomous(const FString& Message)
{
	FString ContinuationContext;
	if (bRequestExecutingApprovedPlan)
	{
		ContinuationContext = BuildPlanExecutionContext();
		if (!ContinuationContext.IsEmpty())
		{
			UE_LOG(LogOliveAI, Log, TEXT("Injected plan execution context into autonomous request"));
		}
	}
	else if (RequestChatMode == EOliveChatMode::Plan)
	{
		ContinuationContext = BuildPlanContinuationContext();
		if (!ContinuationContext.IsEmpty())
		{
			UE_LOG(LogOliveAI, Log, TEXT("Injected plan continuation context into autonomous request"));
		}
	}

	// 1. Add user message to conversation history for UI display
	FOliveChatMessage UserMessage;
	UserMessage.Role = EOliveChatRole::User;
	UserMessage.Content = Message;
	UserMessage.Timestamp = FDateTime::UtcNow();
	AddMessage(UserMessage);

	// 2. Begin processing
	bIsProcessing = true;
	OnProcessingStarted.Broadcast();

	// 3. Ensure MCP server is running so Claude Code can discover tools
	if (!FOliveMCPServer::Get().IsRunning())
	{
		UE_LOG(LogOliveAI, Log, TEXT("Starting MCP server for autonomous mode"));
		FOliveMCPServer::Get().Start();
	}

	// 4. Auto-snapshot before autonomous run for one-click rollback safety net.
	//    This is especially important because MCP-origin tool calls bypass
	//    Tier 2/3 confirmation flows.
	if (ActiveContextPaths.Num() > 0)
	{
		const FString TruncatedMessage = Message.Left(60);
		const FString SnapshotLabel = FString::Printf(TEXT("Pre-autonomous: %s"), *TruncatedMessage);
		FOliveToolResult SnapshotResult = FOliveSnapshotManager::Get().CreateSnapshot(
			SnapshotLabel, ActiveContextPaths, TEXT("Auto-snapshot before autonomous provider run"));

		if (SnapshotResult.bSuccess)
		{
			UE_LOG(LogOliveAI, Log, TEXT("Auto-snapshot created before autonomous run"));
		}
		else
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Failed to create auto-snapshot before autonomous run"));
		}
	}
	else
	{
		UE_LOG(LogOliveAI, Log, TEXT("Skipping auto-snapshot: no active context paths"));
	}

	// 5. Propagate the current chat mode to the MCP server so that tool calls
	//    from the internal autonomous agent respect Plan/Ask mode gating.
	FOliveMCPServer::Get().SetChatModeForInternalAgent(RequestChatMode);

	// 6. Begin a Brain run
	if (Brain.IsValid())
	{
		Brain->BeginRun();
	}

	// Reset orphan baselines for delta tracking during this run
	FOliveWritePipeline::Get().ClearOrphanBaselines();
	FOliveWritePipeline::Get().bRunActive = true;

	// 7. Set up callbacks with WeakSelf pattern (matches existing orchestrated path)
	TWeakPtr<FOliveConversationManager> WeakSelf = AsShared();

	FOnOliveStreamChunk OnChunk;
	OnChunk.BindLambda([WeakSelf](const FOliveStreamChunk& Chunk)
	{
		if (TSharedPtr<FOliveConversationManager> This = WeakSelf.Pin())
		{
			if (!Chunk.Text.IsEmpty())
			{
				This->CurrentStreamingContent += Chunk.Text;
				This->OnStreamChunk.Broadcast(Chunk.Text);
			}
		}
	});

	FOnOliveComplete OnComplete;
	OnComplete.BindLambda([WeakSelf](const FString& FullResponse, const FOliveProviderUsage& Usage)
	{
		// Clear internal agent mode so subsequent external MCP calls default to Code
		FOliveMCPServer::Get().ClearChatModeForInternalAgent();

		if (TSharedPtr<FOliveConversationManager> This = WeakSelf.Pin())
		{
			// Update token usage
			This->TotalTokensUsed += Usage.TotalTokens;

			// Add assistant response to history
			FOliveChatMessage AssistantMessage;
			AssistantMessage.Role = EOliveChatRole::Assistant;
			AssistantMessage.Content = !FullResponse.IsEmpty() ? FullResponse : This->CurrentStreamingContent;
			AssistantMessage.Timestamp = FDateTime::UtcNow();
			This->AddMessage(AssistantMessage);
			if (This->RequestChatMode == EOliveChatMode::Plan)
			{
				This->UpdatePlanSessionFromAssistantMessage(AssistantMessage);
			}

			// Brain: complete run (CompleteRun already transitions to Idle)
			if (This->Brain.IsValid() && This->Brain->GetState() != EOliveBrainState::Idle)
			{
				This->Brain->CompleteRun(EOliveRunOutcome::Completed);
			}

			This->bIsProcessing = false;
			This->ResetRequestContext();
			This->CurrentStreamingContent.Empty();
			This->OnProcessingComplete.Broadcast();

			// Apply deferred mode switch if pending
			if (This->DeferredChatMode.IsSet())
			{
				This->ActiveChatMode = This->DeferredChatMode.GetValue();
				This->DeferredChatMode.Reset();
				This->OnModeChanged.Broadcast(This->ActiveChatMode);
				UE_LOG(LogOliveAI, Log, TEXT("Applied deferred mode switch to %s"), LexToString(This->ActiveChatMode));
			}

			// Drain the next queued message if any are waiting
			This->DrainNextQueuedMessage();
		}
	});

	FOnOliveError OnErr;
	OnErr.BindLambda([WeakSelf](const FString& ErrorMessage)
	{
		// Clear internal agent mode so subsequent external MCP calls default to Code
		FOliveMCPServer::Get().ClearChatModeForInternalAgent();

		if (TSharedPtr<FOliveConversationManager> This = WeakSelf.Pin())
		{
			UE_LOG(LogOliveAI, Error, TEXT("Autonomous run error: %s"), *ErrorMessage);

			// Brain: error state (CompleteRun already transitions to Idle)
			if (This->Brain.IsValid() && This->Brain->IsActive())
			{
				This->Brain->CompleteRun(EOliveRunOutcome::Failed);
			}

			This->bIsProcessing = false;
			This->ResetRequestContext();
			This->CurrentStreamingContent.Empty();

			This->OnError.Broadcast(ErrorMessage);
			This->OnProcessingComplete.Broadcast();

			// Apply deferred mode switch if pending
			if (This->DeferredChatMode.IsSet())
			{
				This->ActiveChatMode = This->DeferredChatMode.GetValue();
				This->DeferredChatMode.Reset();
				This->OnModeChanged.Broadcast(This->ActiveChatMode);
				UE_LOG(LogOliveAI, Log, TEXT("Applied deferred mode switch to %s"), LexToString(This->ActiveChatMode));
			}

			// Drain the next queued message if any are waiting
			This->DrainNextQueuedMessage();
		}
	});

	// 8. Pass @-mentioned asset context to the CLI provider for initial prompt enrichment.
	//    This injects a pre-read asset state summary so the AI doesn't waste turns
	//    re-reading assets the user has already pointed at via @-mentions.
	if (ActiveContextPaths.Num() > 0)
	{
		// IOliveAIProvider has no SetInitialContextAssets -- it lives on the CLI base class.
		// Use static_cast since IsAutonomousProvider() already confirmed this is a CLI provider.
		FOliveCLIProviderBase* CLIProvider = static_cast<FOliveCLIProviderBase*>(Provider.Get());
		CLIProvider->SetInitialContextAssets(ActiveContextPaths);
	}

	// 9. Launch autonomous provider -- tools are discovered via MCP, no orchestration
	const FString ProviderName = Provider->GetProviderName();
	UE_LOG(LogOliveAI, Log, TEXT("Launching autonomous run (%s) for message: %.80s%s"),
		*ProviderName, *Message.Left(80), Message.Len() > 80 ? TEXT("...") : TEXT(""));

	Provider->SendMessageAutonomous(Message, ContinuationContext, OnChunk, OnComplete, OnErr);
}

// ==========================================
// Message Handling
// ==========================================

void FOliveConversationManager::SendUserMessage(const FString& Message)
{
	if (Message.IsEmpty())
	{
		return;
	}

	if (bIsProcessing)
	{
		if (Queue)
		{
			// Enqueue the message for later delivery instead of discarding it.
			// The message is added to the UI/history immediately so the user sees it.
			FOliveChatMessage QueuedUserMessage;
			QueuedUserMessage.Role = EOliveChatRole::User;
			QueuedUserMessage.Content = Message;
			QueuedUserMessage.Timestamp = FDateTime::UtcNow();
			AddMessage(QueuedUserMessage);

			Queue->Enqueue(Message);
			UE_LOG(LogOliveAI, Log, TEXT("Message queued while processing (depth: %d)"), Queue->GetQueueDepth());
		}
		else
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Cannot send message while processing (no queue configured)"));
		}
		return;
	}

	if (!Provider.IsValid())
	{
		OnError.Broadcast(TEXT("No AI provider configured. Please configure a provider in settings."));
		return;
	}

	if (ActiveChatMode == EOliveChatMode::Plan)
	{
		UpdatePlanSessionFromUserMessage(Message);
	}

	BeginRequestContext(Message);

	// Route to autonomous path for Claude Code CLI when autonomous MCP mode is enabled.
	// This bypasses the entire orchestrated loop (system message assembly, tool schema
	// serialization, prompt distillation, iteration budgets, correction directives,
	// self-correction policy, and loop detection). Claude Code discovers tools via MCP
	// and manages its own agentic loop.
	if (IsAutonomousProvider())
	{
		// Ask mode blocks autonomous execution entirely
		if (ActiveChatMode == EOliveChatMode::Ask)
		{
			OnError.Broadcast(TEXT("Ask mode is read-only. Switch to Code or Plan mode to run autonomous operations."));
			return;
		}

		SendUserMessageAutonomous(Message);
		return;
	}

	// Add user message
	FOliveChatMessage UserMessage;
	UserMessage.Role = EOliveChatRole::User;
	UserMessage.Content = Message;
	UserMessage.Timestamp = FDateTime::UtcNow();
	AddMessage(UserMessage);

	// Reset tool iteration counter and Brain components
	CurrentToolIteration = 0;
	LoopDetector.Reset();
	SelfCorrectionPolicy.Reset();
	bTurnHasExplicitWriteIntent = DetectWriteIntent(Message);

	// Dynamic iteration budget for multi-asset tasks
	if (bTurnHasExplicitWriteIntent && DetectMultiAssetIntent(Message))
	{
		MaxToolIterations = FMath::Max(MaxToolIterations, 20);
		UE_LOG(LogOliveAI, Log,
			TEXT("Multi-asset task detected. Increased MaxToolIterations to %d"),
			MaxToolIterations);
	}
	else
	{
		MaxToolIterations = 10;
	}

	bStopAfterToolResults = false;
	bHasPendingCorrections = false;
	CorrectionRepromptCount = 0;
	ZeroToolRepromptCount = 0;

	// Begin a Brain run
	if (Brain.IsValid())
	{
		Brain->BeginRun();
	}

	// Reset orphan baselines for delta tracking during this run
	FOliveWritePipeline::Get().ClearOrphanBaselines();
	FOliveWritePipeline::Get().bRunActive = true;

	// Start a run if run mode is active
	if (bRunModeActive && !FOliveRunManager::Get().HasActiveRun())
	{
		FOliveRunManager::Get().StartRun(TEXT("AI Operation"));
	}

	// Send to provider
	SendToProvider();
}

void FOliveConversationManager::CancelCurrentRequest()
{
	// Clear internal agent mode so MCP calls revert to Code default
	FOliveMCPServer::Get().ClearChatModeForInternalAgent();

	// Cancel any pending retry before cancelling the provider request
	if (RetryManager)
	{
		RetryManager->Cancel();
	}

	if (Provider.IsValid() && bIsProcessing)
	{
		Provider->CancelRequest();
	}

	// Brain: cancel
	if (Brain.IsValid())
	{
		Brain->RequestCancel();
		Brain->ResetToIdle();
	}

	bIsProcessing = false;
	ResetRequestContext();
	PendingToolCalls.Empty();
	PendingToolResults.Empty();
	PendingToolExecutions = 0;
	ActiveToolCallArgs.Reset();
	CurrentStreamingContent.Empty();
	bTurnHasExplicitWriteIntent = false;
	bStopAfterToolResults = false;
	CurrentBatchFailureCount = 0;
	CurrentBatchCorrectionSummary.Empty();
	bHasPendingCorrections = false;
	CorrectionRepromptCount = 0;

	// Apply deferred mode switch if pending
	if (DeferredChatMode.IsSet())
	{
		ActiveChatMode = DeferredChatMode.GetValue();
		DeferredChatMode.Reset();
		OnModeChanged.Broadcast(ActiveChatMode);
		UE_LOG(LogOliveAI, Log, TEXT("Applied deferred mode switch to %s"), LexToString(ActiveChatMode));
	}
}

// ==========================================
// Context Management
// ==========================================

void FOliveConversationManager::SetActiveContext(const TArray<FString>& AssetPaths)
{
	ActiveContextPaths = AssetPaths;
}

void FOliveConversationManager::SetChatMode(EOliveChatMode NewMode)
{
	if (bIsProcessing)
	{
		DeferredChatMode = NewMode;
		UE_LOG(LogOliveAI, Log, TEXT("Mode switch to %s deferred until processing completes"),
			LexToString(NewMode));
		// Fire delegate so UI can show "Mode will switch after current operation"
		OnModeSwitchDeferred.Broadcast(NewMode);
		return;
	}

	const EOliveChatMode OldMode = ActiveChatMode;
	ActiveChatMode = NewMode;
	DeferredChatMode.Reset();
	UE_LOG(LogOliveAI, Log, TEXT("Chat mode: %s -> %s"), LexToString(OldMode), LexToString(NewMode));
	OnModeChanged.Broadcast(NewMode);
}

// ==========================================
// Provider Management
// ==========================================

void FOliveConversationManager::SetProvider(TSharedPtr<IOliveAIProvider> InProvider)
{
	// Cancel any pending request with old provider
	CancelCurrentRequest();
	Provider = InProvider;

	// Keep the retry manager in sync with the active provider
	if (RetryManager)
	{
		RetryManager->SetProvider(InProvider);
	}
}

// ==========================================
// Configuration
// ==========================================

void FOliveConversationManager::SetSystemPrompt(const FString& Prompt)
{
	SystemPrompt = Prompt;
}

// ==========================================
// Internal Message Handling
// ==========================================

void FOliveConversationManager::AddMessage(const FOliveChatMessage& Message)
{
	MessageHistory.Add(Message);
	OnMessageAdded.Broadcast(Message);
}

FOliveChatMessage FOliveConversationManager::BuildSystemMessage()
{
	FOliveChatMessage SystemMessage;
	SystemMessage.Role = EOliveChatRole::System;
	SystemMessage.Timestamp = FDateTime::UtcNow();
	const EOliveChatMode EffectiveChatMode = bIsProcessing ? RequestChatMode : ActiveChatMode;

	const UOliveAISettings* Settings = UOliveAISettings::Get();
	const int32 MaxPromptTokens = Settings ? FMath::Max(512, Settings->MaxTokens) : 4000;

	FOlivePromptAssembler& PromptAssembler = FOlivePromptAssembler::Get();
	if (SystemPrompt.IsEmpty())
	{
		SystemMessage.Content = PromptAssembler.AssembleSystemPrompt(
			EffectiveChatMode,
			ActiveContextPaths,
			MaxPromptTokens);
	}
	else
	{
		SystemMessage.Content = PromptAssembler.AssembleSystemPromptWithBase(
			SystemPrompt,
			EffectiveChatMode,
			ActiveContextPaths,
			MaxPromptTokens);
	}

	// Add distilled operation history
	FString HistoryContext = OliveConversationManagerInternal::BuildOperationHistoryContext(HistoryStore);
	if (!HistoryContext.IsEmpty())
	{
		SystemMessage.Content += TEXT("\n\n## Recent Operations\n") + HistoryContext;
	}

	return SystemMessage;
}

TArray<FOliveToolDefinition> FOliveConversationManager::GetAvailableTools()
{
	return FOliveToolRegistry::Get().GetToolsForMode(bIsProcessing ? RequestChatMode : ActiveChatMode);
}

void FOliveConversationManager::SendToProvider()
{
	if (!Provider.IsValid())
	{
		HandleError(TEXT("No provider configured"));
		return;
	}

	bIsProcessing = true;
	CurrentStreamingContent.Empty();
	PendingToolCalls.Empty();

	OnProcessingStarted.Broadcast();

	// Build messages array with system message at start
	TArray<FOliveChatMessage> MessagesToSend;
	MessagesToSend.Add(BuildSystemMessage());
	MessagesToSend.Append(MessageHistory);

	// Distill conversation history to save tokens
	FOliveDistillationConfig DistillConfig;
	if (const UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		DistillConfig.RecentPairsToKeep = Settings->PromptDistillationRawResults;
	}
	DistillConfig.MaxTotalResultChars = 80000; // ~20K tokens // TODO: make configurable via UOliveAISettings
	DistillConfig.MaxAssistantChars = 4000;
	const FOliveDistillationResult DistillResult = PromptDistiller.Distill(MessagesToSend, DistillConfig);

	// Inject a context truncation note so the model knows earlier messages were summarized.
	// Insert just before the final user message to maximize visibility in the context window.
	if (DistillResult.DidTruncate())
	{
		FString TruncationNote = FString::Printf(
			TEXT("[CONTEXT NOTE: %d older messages were summarized to save tokens. "
				 "%d tool results were truncated. If you need details from earlier "
				 "in the conversation, ask the user to re-provide the relevant context.]"),
			DistillResult.MessagesSummarized,
			DistillResult.ToolResultsTruncated);

		FOliveChatMessage TruncationMessage;
		TruncationMessage.Role = EOliveChatRole::System;
		TruncationMessage.Content = TruncationNote;
		TruncationMessage.Timestamp = FDateTime::UtcNow();

		// Find the last user message and insert the note just before it
		int32 InsertIndex = MessagesToSend.Num(); // fallback: append at end
		for (int32 i = MessagesToSend.Num() - 1; i >= 0; --i)
		{
			if (MessagesToSend[i].Role == EOliveChatRole::User)
			{
				InsertIndex = i;
				break;
			}
		}
		MessagesToSend.Insert(TruncationMessage, InsertIndex);

		UE_LOG(LogOliveAI, Log, TEXT("Injected context truncation note (%d summarized, %d truncated, ~%d tokens saved)"),
			DistillResult.MessagesSummarized, DistillResult.ToolResultsTruncated, DistillResult.TokensSaved);
	}

	// Inject iteration budget awareness
	if (CurrentToolIteration > 0)
	{
		const int32 RemainingIterations = MaxToolIterations - CurrentToolIteration;
		FString BudgetNote;

		if (RemainingIterations <= 3)
		{
			BudgetNote = FString::Printf(
				TEXT("[ITERATION BUDGET: %d/%d used, only %d remaining. "
					 "CRITICAL: Focus on completing the most important remaining work. "
					 "If there are multiple assets to create, prioritize creating them "
					 "over perfecting existing ones.]"),
				CurrentToolIteration, MaxToolIterations, RemainingIterations);
		}
		else if (RemainingIterations <= 6)
		{
			BudgetNote = FString::Printf(
				TEXT("[ITERATION BUDGET: %d/%d used, %d remaining. "
					 "Plan remaining tool calls efficiently.]"),
				CurrentToolIteration, MaxToolIterations, RemainingIterations);
		}

		if (!BudgetNote.IsEmpty())
		{
			FOliveChatMessage BudgetMessage;
			BudgetMessage.Role = EOliveChatRole::System;
			BudgetMessage.Content = BudgetNote;
			BudgetMessage.Timestamp = FDateTime::UtcNow();

			int32 InsertIndex = MessagesToSend.Num();
			for (int32 i = MessagesToSend.Num() - 1; i >= 0; --i)
			{
				if (MessagesToSend[i].Role == EOliveChatRole::User)
				{
					InsertIndex = i;
					break;
				}
			}
			MessagesToSend.Insert(BudgetMessage, InsertIndex);
		}
	}

	const FOliveChatMessage* LastUserMessage = nullptr;
	for (int32 i = MessagesToSend.Num() - 1; i >= 0; --i)
	{
		if (MessagesToSend[i].Role == EOliveChatRole::User)
		{
			LastUserMessage = &MessagesToSend[i];
			break;
		}
	}

	if (LastUserMessage)
	{
		FString PlanContext;
		if (bRequestExecutingApprovedPlan)
		{
			PlanContext = BuildPlanExecutionContext();
		}
		else if (RequestChatMode == EOliveChatMode::Plan)
		{
			PlanContext = BuildPlanContinuationContext();
		}

		if (!PlanContext.IsEmpty())
		{
			FOliveChatMessage PlanContextMessage;
			PlanContextMessage.Role = EOliveChatRole::System;
			PlanContextMessage.Content = PlanContext;
			PlanContextMessage.Timestamp = FDateTime::UtcNow();

			int32 InsertIndex = MessagesToSend.Num();
			for (int32 i = MessagesToSend.Num() - 1; i >= 0; --i)
			{
				if (MessagesToSend[i].Role == EOliveChatRole::User)
				{
					InsertIndex = i;
					break;
				}
			}

			MessagesToSend.Insert(PlanContextMessage, InsertIndex);
			UE_LOG(LogOliveAI, Log, TEXT("Injected plan context into provider request"));
		}
	}

	// Get available tools filtered by focus profile.
	// Tool pack gating was removed in the AI Freedom update -- all tools allowed
	// by the active focus profile are now sent on every iteration. This lets the
	// AI discover and plan with write tools from the first turn without waiting
	// for intent classification.
	TArray<FOliveToolDefinition> Tools = GetAvailableTools();

	// Create callbacks (capture shared this)
	TWeakPtr<FOliveConversationManager> WeakSelf = AsShared();

	FOnOliveStreamChunk OnChunk;
	OnChunk.BindLambda([WeakSelf](const FOliveStreamChunk& Chunk)
	{
		if (TSharedPtr<FOliveConversationManager> This = WeakSelf.Pin())
		{
			This->HandleStreamChunk(Chunk);
		}
	});

	FOnOliveToolCall OnToolCall;
	OnToolCall.BindLambda([WeakSelf](const FOliveStreamChunk& ToolCall)
	{
		if (TSharedPtr<FOliveConversationManager> This = WeakSelf.Pin())
		{
			This->HandleToolCall(ToolCall);
		}
	});

	FOnOliveComplete OnComplete;
	OnComplete.BindLambda([WeakSelf](const FString& Response, const FOliveProviderUsage& Usage)
	{
		if (TSharedPtr<FOliveConversationManager> This = WeakSelf.Pin())
		{
			This->HandleComplete(Response, Usage);
		}
	});

	FOnOliveError OnErr;
	OnErr.BindLambda([WeakSelf](const FString& Error)
	{
		if (TSharedPtr<FOliveConversationManager> This = WeakSelf.Pin())
		{
			This->HandleError(Error);
		}
	});

	const FOliveRequestOptions RequestOptions = BuildRequestOptions();

	// Route through the retry manager if available; otherwise call the provider directly.
	if (RetryManager)
	{
		RetryManager->SendWithRetry(MessagesToSend, Tools, OnChunk, OnToolCall, OnComplete, OnErr, RequestOptions);
	}
	else
	{
		Provider->SendMessage(MessagesToSend, Tools, OnChunk, OnToolCall, OnComplete, OnErr, RequestOptions);
	}
}

// ==========================================
// Provider Callbacks
// ==========================================

void FOliveConversationManager::HandleStreamChunk(const FOliveStreamChunk& Chunk)
{
	if (!Chunk.Text.IsEmpty())
	{
		CurrentStreamingContent += Chunk.Text;
		OnStreamChunk.Broadcast(Chunk.Text);
	}
}

void FOliveConversationManager::HandleToolCall(const FOliveStreamChunk& ToolCall)
{
	PendingToolCalls.Add(ToolCall);
}

void FOliveConversationManager::HandleComplete(const FString& FullResponse, const FOliveProviderUsage& Usage)
{
	// Update token usage
	TotalTokensUsed += Usage.TotalTokens;

	// Update Brain worker phase
	if (Brain.IsValid() && Brain->GetState() == EOliveBrainState::Active)
	{
		Brain->SetWorkerPhase(PendingToolCalls.Num() > 0
			? EOliveWorkerPhase::ExecutingTools
			: EOliveWorkerPhase::Complete);
	}

	// Add assistant message to history
	FOliveChatMessage AssistantMessage;
	AssistantMessage.Role = EOliveChatRole::Assistant;
	AssistantMessage.Content = !FullResponse.IsEmpty() ? FullResponse : CurrentStreamingContent;
	const FString NormalizedFinishReason = Usage.FinishReason.ToLower();
	const bool bResponseTruncated =
		NormalizedFinishReason == TEXT("length") ||
		NormalizedFinishReason == TEXT("max_tokens");
	if (bResponseTruncated)
	{
		AssistantMessage.Content += TEXT("\n\n[WARNING: This response was truncated due to token limits. The model may have had more to say.]");
		UE_LOG(LogOliveAI, Warning, TEXT("Provider response truncated (finish_reason=%s)"), *Usage.FinishReason);
	}
	AssistantMessage.Timestamp = FDateTime::UtcNow();
	AssistantMessage.ToolCalls = PendingToolCalls;
	AddMessage(AssistantMessage);
	if (RequestChatMode == EOliveChatMode::Plan)
	{
		UpdatePlanSessionFromAssistantMessage(AssistantMessage);
	}

	// Check if we have tool calls to process
	if (PendingToolCalls.Num() > 0)
	{
		ProcessPendingToolCalls();
	}
	else
	{
		// Detect text-only response when the task requires tool calls.
		// This fires on ANY iteration (not just the first) because the AI
		// can stop prematurely after e.g. blueprint.create without adding
		// components, variables, or graph logic.
		if (bTurnHasExplicitWriteIntent
			&& ZeroToolRepromptCount < MaxZeroToolReprompts)
		{
			ZeroToolRepromptCount++;

			FString ForceToolPrompt = FString::Printf(
				TEXT("[SYSTEM: You responded with text but the task is NOT complete. "
					 "You MUST continue calling tools. The task requires creating "
					 "components, variables, and wiring graph logic — not just creating "
					 "empty Blueprints. Output <tool_call> blocks now. "
					 "Re-prompt %d/%d.]"),
				ZeroToolRepromptCount, MaxZeroToolReprompts);

			FOliveChatMessage RepromptMessage;
			RepromptMessage.Role = EOliveChatRole::User;
			RepromptMessage.Content = ForceToolPrompt;
			RepromptMessage.Timestamp = FDateTime::UtcNow();
			AddMessage(RepromptMessage);

			// Log what the AI actually said so we can debug text-only responses
			UE_LOG(LogOliveAI, Warning,
				TEXT("AI text-only response (first %d chars): %.500s"),
				FMath::Min(500, AssistantMessage.Content.Len()),
				*AssistantMessage.Content.Left(500));

			UE_LOG(LogOliveAI, Warning,
				TEXT("AI responded text-only on iteration %d with write intent. "
					 "Re-prompting to force tool use (%d/%d)"),
				CurrentToolIteration, ZeroToolRepromptCount, MaxZeroToolReprompts);

			SendToProvider();
			return;
		}

		// Check if AI responded text-only despite unresolved failures
		if (bHasPendingCorrections && CorrectionRepromptCount < MaxCorrectionReprompts)
		{
			CorrectionRepromptCount++;

			FString RepromptText = FString::Printf(
				TEXT("[SYSTEM: You responded with text but there are still unresolved tool failures "
					 "from a previous batch. You MUST call the appropriate tools to fix these errors "
					 "before completing. Re-prompt %d/%d.]"),
				CorrectionRepromptCount, MaxCorrectionReprompts);

			FOliveChatMessage RepromptMessage;
			RepromptMessage.Role = EOliveChatRole::User;
			RepromptMessage.Content = RepromptText;
			RepromptMessage.Timestamp = FDateTime::UtcNow();
			AddMessage(RepromptMessage);

			UE_LOG(LogOliveAI, Warning,
				TEXT("AI text-only with pending corrections (first %d chars): %.500s"),
				FMath::Min(500, AssistantMessage.Content.Len()),
				*AssistantMessage.Content.Left(500));

			UE_LOG(LogOliveAI, Warning,
				TEXT("AI responded text-only with unresolved corrections. Re-prompting (%d/%d)"),
				CorrectionRepromptCount, MaxCorrectionReprompts);

			// Re-enter the agentic loop
			SendToProvider();
			return;
		}

		// Determine final outcome based on whether corrections were resolved
		EOliveRunOutcome FinalOutcome = bHasPendingCorrections
			? EOliveRunOutcome::PartialSuccess
			: EOliveRunOutcome::Completed;

		if (bHasPendingCorrections)
		{
			UE_LOG(LogOliveAI, Warning,
				TEXT("Completing with PartialSuccess: unresolved corrections remain after %d re-prompts"),
				CorrectionRepromptCount);
		}

		// Complete run if active
		if (bRunModeActive && FOliveRunManager::Get().HasActiveRun())
		{
			FOliveRunManager::Get().CompleteRun();
			bRunModeActive = false;
		}
		// Brain: transition to Completed/PartialSuccess -> Idle (CompleteRun already transitions)
		if (Brain.IsValid() && Brain->GetState() != EOliveBrainState::Idle)
		{
			Brain->CompleteRun(FinalOutcome);
		}

		// Reset correction state
		bHasPendingCorrections = false;
		CorrectionRepromptCount = 0;

		// No tool calls, we're done
		bIsProcessing = false;
		ResetRequestContext();
		OnProcessingComplete.Broadcast();

		// Apply deferred mode switch if pending
		if (DeferredChatMode.IsSet())
		{
			ActiveChatMode = DeferredChatMode.GetValue();
			DeferredChatMode.Reset();
			OnModeChanged.Broadcast(ActiveChatMode);
			UE_LOG(LogOliveAI, Log, TEXT("Applied deferred mode switch to %s"), LexToString(ActiveChatMode));
		}

		// Drain the next queued message if any are waiting
		DrainNextQueuedMessage();
	}
}

void FOliveConversationManager::UpdatePlanSessionFromUserMessage(const FString& Message)
{
	if (ActiveChatMode != EOliveChatMode::Plan || Message.TrimStartAndEnd().IsEmpty())
	{
		return;
	}

	if (!ActivePlanSession.IsValid())
	{
		ActivePlanSession = MakeUnique<FOlivePlanSessionState>();
	}

	const FString PlanMessage = NormalizePlanText(Message);
	const bool bContinuePlan = ShouldContinueActivePlan(PlanMessage);
	if (!ActivePlanSession->bHasActivePlan || !bContinuePlan)
	{
		if (!ActivePlanSession->IsEmpty())
		{
			UE_LOG(LogOliveAI, Log, TEXT("Reset active plan session before starting new plan thread"));
		}
		ActivePlanSession->Reset();
		ActivePlanSession->bHasActivePlan = true;
		ActivePlanSession->SessionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		ActivePlanSession->GoalSummary = SummarizePlanText(PlanMessage, 160);
		ActivePlanSession->TargetSummary = ActivePlanSession->GoalSummary;
		ActivePlanSession->LatestPlanSummary = ActivePlanSession->GoalSummary;
		UE_LOG(LogOliveAI, Log, TEXT("Started plan session %s: %.120s"),
			*ActivePlanSession->SessionId, *ActivePlanSession->GoalSummary);
	}
	else
	{
		AppendUniquePlanItem(ActivePlanSession->UserPlanAdjustments, SummarizePlanText(PlanMessage, 160), 8);
		UE_LOG(LogOliveAI, Log, TEXT("Updated plan session %s from user follow-up"),
			*ActivePlanSession->SessionId);
	}

	ActivePlanSession->ActiveModeContext = LexToString(ActiveChatMode);
	ActivePlanSession->LastUpdatedUtc = FDateTime::UtcNow();
}

void FOliveConversationManager::UpdatePlanSessionFromAssistantMessage(const FOliveChatMessage& AssistantMessage)
{
	if (ActiveChatMode != EOliveChatMode::Plan
		|| AssistantMessage.Role != EOliveChatRole::Assistant
		|| AssistantMessage.Content.TrimStartAndEnd().IsEmpty())
	{
		return;
	}

	if (!ActivePlanSession.IsValid())
	{
		ActivePlanSession = MakeUnique<FOlivePlanSessionState>();
	}

	if (!ActivePlanSession->bHasActivePlan)
	{
		ActivePlanSession->bHasActivePlan = true;
		ActivePlanSession->SessionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	ActivePlanSession->ActiveModeContext = LexToString(ActiveChatMode);
	ActivePlanSession->LatestPlanText = AssistantMessage.Content.TrimStartAndEnd();
	ActivePlanSession->LatestPlanSummary = SummarizePlanText(AssistantMessage.Content, 320);
	if (ActivePlanSession->GoalSummary.IsEmpty())
	{
		ActivePlanSession->GoalSummary = ActivePlanSession->LatestPlanSummary;
	}
	if (ActivePlanSession->TargetSummary.IsEmpty())
	{
		ActivePlanSession->TargetSummary = ActivePlanSession->LatestPlanSummary;
	}

	TArray<FString> Lines;
	AssistantMessage.Content.ParseIntoArrayLines(Lines, true);
	for (const FString& RawLine : Lines)
	{
		const FString Line = RawLine.TrimStartAndEnd();
		if (Line.IsEmpty())
		{
			continue;
		}

		if (IsLikelyPlanQuestion(Line))
		{
			AppendUniquePlanItem(ActivePlanSession->OutstandingQuestions, Line, 6);
			continue;
		}

		if (IsLikelyPlanAssetLine(Line))
		{
			AppendUniquePlanItem(ActivePlanSession->PlannedAssetsOrArtifacts, Line, 8);
		}

		if (Line.StartsWith(TEXT("-")) || Line.StartsWith(TEXT("*")) || Line.StartsWith(TEXT("1."))
			|| Line.StartsWith(TEXT("2.")) || Line.StartsWith(TEXT("3.")) || Line.StartsWith(TEXT("4.")))
		{
			AppendUniquePlanItem(ActivePlanSession->KeyDecisions, Line, 8);
		}
	}
	ActivePlanSession->LastUpdatedUtc = FDateTime::UtcNow();
	UE_LOG(LogOliveAI, Log, TEXT("Updated plan session %s from assistant response"),
		*ActivePlanSession->SessionId);
}

FString FOliveConversationManager::BuildPlanContinuationContext() const
{
	if (!ActivePlanSession.IsValid()
		|| !ActivePlanSession->bHasActivePlan
		|| ActivePlanSession->LatestPlanSummary.IsEmpty()
		|| ActivePlanSession->LatestPlanText.IsEmpty())
	{
		return TEXT("");
	}

	TArray<FString> Lines;
	Lines.Add(TEXT("## Active Plan Context"));
	if (!ActivePlanSession->GoalSummary.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Goal: %s"), *ActivePlanSession->GoalSummary));
	}
	if (!ActivePlanSession->LatestPlanSummary.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Latest plan: %s"), *ActivePlanSession->LatestPlanSummary));
	}
	if (ActivePlanSession->UserPlanAdjustments.Num() > 0)
	{
		const FString Adjustments = FString::Join(ActivePlanSession->UserPlanAdjustments, TEXT("; "));
		Lines.Add(FString::Printf(TEXT("Recent user adjustments: %s"), *Adjustments));
	}
	if (ActivePlanSession->OutstandingQuestions.Num() > 0)
	{
		const FString Questions = FString::Join(ActivePlanSession->OutstandingQuestions, TEXT("; "));
		Lines.Add(FString::Printf(TEXT("Open questions: %s"), *Questions));
	}
	Lines.Add(TEXT("Revise the active plan instead of starting a new task."));

	return FString::Join(Lines, TEXT("\n"));
}

FString FOliveConversationManager::BuildPlanExecutionContext() const
{
	if (!ActivePlanSession.IsValid() || !ActivePlanSession->bHasActivePlan)
	{
		return TEXT("");
	}

	TArray<FString> Lines;
	Lines.Add(TEXT("## Approved Plan To Execute"));
	if (!ActivePlanSession->GoalSummary.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Goal: %s"), *ActivePlanSession->GoalSummary));
	}
	if (!ActivePlanSession->LatestPlanSummary.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("Plan summary: %s"), *ActivePlanSession->LatestPlanSummary));
	}
	if (ActivePlanSession->UserPlanAdjustments.Num() > 0)
	{
		const FString Adjustments = FString::Join(ActivePlanSession->UserPlanAdjustments, TEXT("; "));
		Lines.Add(FString::Printf(TEXT("Approved adjustments: %s"), *Adjustments));
	}
	if (ActivePlanSession->PlannedAssetsOrArtifacts.Num() > 0)
	{
		const FString Assets = FString::Join(ActivePlanSession->PlannedAssetsOrArtifacts, TEXT("; "));
		Lines.Add(FString::Printf(TEXT("Planned assets/artifacts: %s"), *Assets));
	}
	if (ActivePlanSession->KeyDecisions.Num() > 0)
	{
		const FString Decisions = FString::Join(ActivePlanSession->KeyDecisions, TEXT("; "));
		Lines.Add(FString::Printf(TEXT("Key decisions: %s"), *Decisions));
	}
	Lines.Add(TEXT("Implement this active approved plan without restarting discovery."));

	return FString::Join(Lines, TEXT("\n"));
}

bool FOliveConversationManager::ShouldExecuteApprovedPlan(const FString& Message) const
{
	return ActivePlanSession.IsValid()
		&& ActivePlanSession->bHasActivePlan
		&& IsPlanToCodeHandoffCue(Message);
}

void FOliveConversationManager::BeginRequestContext(const FString& Message)
{
	bRequestExecutingApprovedPlan = ShouldExecuteApprovedPlan(Message);
	RequestChatMode = bRequestExecutingApprovedPlan ? EOliveChatMode::Code : ActiveChatMode;

	if (bRequestExecutingApprovedPlan && ActivePlanSession.IsValid())
	{
		ActivePlanSession->ActiveModeContext = TEXT("Code (executing approved plan)");
		ActivePlanSession->LastUpdatedUtc = FDateTime::UtcNow();
	}
}

void FOliveConversationManager::ResetRequestContext()
{
	RequestChatMode = ActiveChatMode;
	bRequestExecutingApprovedPlan = false;
}

bool FOliveConversationManager::ShouldContinueActivePlan(const FString& Message) const
{
	if (!ActivePlanSession.IsValid() || !ActivePlanSession->bHasActivePlan)
	{
		return false;
	}

	const FString Trimmed = Message.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return false;
	}

	if (MessageStartsNewPlanTask(Trimmed))
	{
		return false;
	}

	if (ShouldExecuteApprovedPlan(Trimmed))
	{
		return true;
	}

	const FString Lower = Trimmed.ToLower();
	if (Lower.StartsWith(TEXT("it "))
		|| Lower.StartsWith(TEXT("it'"))
		|| Lower.StartsWith(TEXT("also "))
		|| Lower.StartsWith(TEXT("and also"))
		|| Lower.StartsWith(TEXT("continue"))
		|| Lower.StartsWith(TEXT("keep "))
		|| Lower.StartsWith(TEXT("for it"))
		|| Lower.StartsWith(TEXT("switch to code"))
		|| Lower.StartsWith(TEXT("switch to code mode"))
		|| Lower.StartsWith(TEXT("do it"))
		|| Lower.StartsWith(TEXT("build it"))
		|| Lower.StartsWith(TEXT("implement it"))
		|| Lower.Contains(TEXT(" also "))
		|| Lower.Contains(TEXT(" it "))
		|| Lower.Contains(TEXT(" approved plan"))
		|| Lower.Contains(TEXT(" use the plan")))
	{
		return true;
	}

	static const TArray<FString> ContinuationKeywords = {
		TEXT("also"), TEXT("update"), TEXT("change"), TEXT("revise"), TEXT("tweak"),
		TEXT("follow-up"), TEXT("same plan"), TEXT("that plan"), TEXT("this plan")
	};
	if (MessageContainsAnyKeyword(Lower, ContinuationKeywords))
	{
		return true;
	}

	if (!ActivePlanSession->GoalSummary.IsEmpty())
	{
		TArray<FString> GoalTokens;
		ActivePlanSession->GoalSummary.ToLower().ParseIntoArrayWS(GoalTokens);
		int32 MatchingGoalTokens = 0;
		for (const FString& Token : GoalTokens)
		{
			if (Token.Len() >= 5 && Lower.Contains(Token))
			{
				MatchingGoalTokens++;
			}
		}

		if (MatchingGoalTokens >= 2)
		{
			return true;
		}
	}

	return false;
}

void FOliveConversationManager::HandleError(const FString& ErrorMessage)
{
	UE_LOG(LogOliveAI, Error, TEXT("Conversation error: %s"), *ErrorMessage);

	bIsProcessing = false;
	ResetRequestContext();

	// Brain: error state (CompleteRun already transitions to Idle)
	if (Brain.IsValid() && Brain->IsActive())
	{
		Brain->CompleteRun(EOliveRunOutcome::Failed);
	}

	PendingToolCalls.Empty();
	PendingToolResults.Empty();
	bStopAfterToolResults = false;

	OnError.Broadcast(ErrorMessage);
	OnProcessingComplete.Broadcast();

	// Apply deferred mode switch if pending
	if (DeferredChatMode.IsSet())
	{
		ActiveChatMode = DeferredChatMode.GetValue();
		DeferredChatMode.Reset();
		OnModeChanged.Broadcast(ActiveChatMode);
		UE_LOG(LogOliveAI, Log, TEXT("Applied deferred mode switch to %s"), LexToString(ActiveChatMode));
	}

	// Drain the next queued message if any are waiting
	DrainNextQueuedMessage();
}

// ==========================================
// Tool Execution
// ==========================================

void FOliveConversationManager::ProcessPendingToolCalls()
{
	// Check iteration limit
	CurrentToolIteration++;
	if (CurrentToolIteration > MaxToolIterations)
	{
		HandleError(FString::Printf(TEXT("Maximum tool iterations (%d) reached. Stopping to prevent infinite loop."),
			MaxToolIterations));
		return;
	}

	// Run mode: check pause, begin step, auto-checkpoint
	if (bRunModeActive && FOliveRunManager::Get().HasActiveRun())
	{
		const FOliveRun* Run = FOliveRunManager::Get().GetActiveRun();
		if (Run && Run->Status == EOliveRunStatus::Paused)
		{
			return; // Don't process while paused
		}
		FOliveRunManager::Get().BeginStep(
			FString::Printf(TEXT("Tool iteration %d"), CurrentToolIteration));
		if (FOliveRunManager::Get().ShouldCheckpoint() && ActiveContextPaths.Num() > 0)
		{
			FOliveRunManager::Get().CreateCheckpoint(ActiveContextPaths);
		}
	}

	// Clear previous results
	PendingToolResults.Empty();
	bSkipRemainingBatch = false;
	FailedFoundationalTool.Empty();
	FailedAssetPaths.Empty();
	CurrentBatchFailureCount = 0;
	CurrentBatchCorrectionSummary.Empty();

	// Snapshot: move pending calls into a local so callbacks can safely
	// mutate the member array (ContinueAfterToolResults clears it;
	// HandleToolCall may re-populate it via a new provider response).
	TArray<FOliveStreamChunk> CallsToProcess = MoveTemp(PendingToolCalls);
	PendingToolCalls.Reset();

	// Defensive dedupe by tool_call id. Some provider outputs can repeat the same
	// call block, and executing duplicates can create unintended duplicate writes.
	TSet<FString> SeenToolCallIds;
	TArray<FOliveStreamChunk> UniqueCalls;
	UniqueCalls.Reserve(CallsToProcess.Num());
	for (const FOliveStreamChunk& TC : CallsToProcess)
	{
		if (!TC.ToolCallId.IsEmpty())
		{
			if (SeenToolCallIds.Contains(TC.ToolCallId))
			{
				UE_LOG(LogOliveAI, Warning, TEXT("Skipping duplicate tool call id '%s' (%s)"),
					*TC.ToolCallId, *TC.ToolName);
				continue;
			}
			SeenToolCallIds.Add(TC.ToolCallId);
		}
		UniqueCalls.Add(TC);
	}
	CallsToProcess = MoveTemp(UniqueCalls);

	// Build ID->Args lookup for HandleToolResult's confirmation flow
	ActiveToolCallArgs.Reset();
	for (const FOliveStreamChunk& TC : CallsToProcess)
	{
		if (!TC.ToolCallId.IsEmpty())
		{
			ActiveToolCallArgs.Add(TC.ToolCallId, TC.ToolArguments);
		}
	}

	PendingToolExecutions = CallsToProcess.Num();

	UE_LOG(LogOliveAI, Log, TEXT("Processing %d tool calls (iteration %d)"),
		CallsToProcess.Num(), CurrentToolIteration);

	// Execute each tool call from the local snapshot
	for (const FOliveStreamChunk& ToolCall : CallsToProcess)
	{
		// If a previous tool in this batch triggered stop, skip remaining tools
		if (bStopAfterToolResults)
		{
			FOliveChatMessage SkipMessage;
			SkipMessage.Role = EOliveChatRole::Tool;
			SkipMessage.ToolCallId = ToolCall.ToolCallId;
			SkipMessage.ToolName = ToolCall.ToolName;
			SkipMessage.Content = TEXT("{\"success\":false,\"error\":{\"code\":\"SKIPPED\",\"message\":\"Skipped: a previous tool call in this batch triggered a stop.\"}}");
			SkipMessage.Timestamp = FDateTime::UtcNow();
			PendingToolResults.Add(SkipMessage);

			PendingToolExecutions--;
			continue;
		}

		// If a foundational tool in this batch failed, skip remaining dependent tools
		if (bSkipRemainingBatch)
		{
			FOliveChatMessage SkipMessage;
			SkipMessage.Role = EOliveChatRole::Tool;
			SkipMessage.ToolCallId = ToolCall.ToolCallId;
			SkipMessage.ToolName = ToolCall.ToolName;
			SkipMessage.Content = FString::Printf(
				TEXT("{\"success\":false,\"error\":{\"code\":\"SKIPPED\",\"message\":\"Skipped: prerequisite tool '%s' failed. Fix it and retry.\"}}")
				, *FailedFoundationalTool);
			SkipMessage.Timestamp = FDateTime::UtcNow();
			PendingToolResults.Add(SkipMessage);
			PendingToolExecutions--;
			continue;
		}

		// Skip tools targeting an asset path that already failed with ASSET_NOT_FOUND
		// in this batch. Intentionally bypasses HandleToolResult so skipped tools
		// do not consume the self-correction budget.
		if (!FailedAssetPaths.IsEmpty())
		{
			FString ToolAssetPath;
			if (ToolCall.ToolArguments.IsValid())
			{
				if (!ToolCall.ToolArguments->TryGetStringField(TEXT("path"), ToolAssetPath))
				{
					ToolCall.ToolArguments->TryGetStringField(TEXT("asset_path"), ToolAssetPath);
				}
			}

			if (!ToolAssetPath.IsEmpty() && FailedAssetPaths.Contains(ToolAssetPath))
			{
				FOliveChatMessage SkipMessage;
				SkipMessage.Role = EOliveChatRole::Tool;
				SkipMessage.ToolCallId = ToolCall.ToolCallId;
				SkipMessage.ToolName = ToolCall.ToolName;
				SkipMessage.Content = FString::Printf(
					TEXT("{\"success\":false,\"error\":{\"code\":\"SKIPPED_ASSET_NOT_FOUND\","
						 "\"message\":\"Skipped: asset '%s' was not found by a prior tool in this batch. "
						 "Use project.search to find the correct path, then retry.\"}}"),
					*ToolAssetPath);
				SkipMessage.Timestamp = FDateTime::UtcNow();
				PendingToolResults.Add(SkipMessage);
				PendingToolExecutions--;
				continue;
			}
		}

		ExecuteToolCall(ToolCall);

		// ExecuteToolCall() is synchronous today and may finalize the batch
		// via HandleToolResult()->ContinueAfterToolResults() when the last
		// pending execution completes. Avoid double-finalizing/sending.
		if (PendingToolExecutions <= 0)
		{
			return;
		}
	}

	// If all tools were skipped (bStopAfterToolResults was set before the loop ran any),
	// finalize immediately since no async HandleToolResult will fire
	if (PendingToolExecutions <= 0)
	{
		ContinueAfterToolResults();
	}
}

void FOliveConversationManager::ExecuteToolCall(const FOliveStreamChunk& ToolCall)
{
	OnToolCallStarted.Broadcast(ToolCall.ToolName, ToolCall.ToolCallId);

	UE_LOG(LogOliveAI, Log, TEXT("Executing tool: %s (id: %s)"),
		*ToolCall.ToolName, *ToolCall.ToolCallId);

	// Execute tool with Editor Chat origin context
	FOliveToolCallContext ToolContext;
	ToolContext.Origin = EOliveToolCallOrigin::EditorChat;
	ToolContext.SessionId = SessionId.ToString();
	ToolContext.RunId = Brain.IsValid() ? Brain->GetCurrentRunId() : TEXT("");
	ToolContext.ChatMode = RequestChatMode;
	ToolContext.bRunModeActive = bRunModeActive;
	FOliveToolExecutionContextScope ContextScope(ToolContext);

	FOliveToolResult Result = FOliveToolRegistry::Get().ExecuteTool(ToolCall.ToolName, ToolCall.ToolArguments);

	// Log full arguments on failure for debugging
	if (!Result.bSuccess)
	{
		FString ArgsStr;
		if (ToolCall.ToolArguments.IsValid())
		{
			auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ArgsStr);
			FJsonSerializer::Serialize(ToolCall.ToolArguments.ToSharedRef(), Writer);
			Writer->Close();
		}
		UE_LOG(LogOliveAI, Warning, TEXT("Tool '%s' FAILED with args: %s"), *ToolCall.ToolName, *ArgsStr);
	}

	// Record in operation history
	{
		FOliveOperationRecord Record;
		Record.RunId = Brain.IsValid() ? Brain->GetCurrentRunId() : TEXT("");
		Record.ToolName = ToolCall.ToolName;
		Record.Params = ToolCall.ToolArguments;
		Record.Result = Result.Data;
		Record.Status = Result.bSuccess ? EOliveOperationStatus::Success : EOliveOperationStatus::Failed;
		if (!Result.bSuccess && Result.Data.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrorObj;
			if (Result.Data->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				Record.ErrorMessage = (*ErrorObj)->GetStringField(TEXT("message"));
			}
		}
		HistoryStore.RecordOperation(Record);
	}

	// Handle result
	HandleToolResult(ToolCall.ToolCallId, ToolCall.ToolName, Result);
}

void FOliveConversationManager::HandleToolResult(
	const FString& ToolCallId,
	const FString& ToolName,
	const FOliveToolResult& Result)
{
	OnToolCallCompleted.Broadcast(ToolName, ToolCallId, Result);

	// Record tool call in run manager
	if (bRunModeActive && FOliveRunManager::Get().HasActiveRun())
	{
		FOliveRunManager::Get().RecordToolCall(ToolName, ToolCallId, Result.bSuccess,
			Result.bSuccess ? TEXT("Success") : TEXT("Failed"),
			0.0, Result.Data);
	}

	// Use SelfCorrectionPolicy to evaluate the result
	FString ResultContent = Result.ToJsonString();
	{
		// Extract asset context from original tool args for per-asset signature tracking
		FString AssetContext;
		if (const TSharedPtr<FJsonObject>* FoundArgs = ActiveToolCallArgs.Find(ToolCallId))
		{
			if (!(*FoundArgs)->TryGetStringField(TEXT("path"), AssetContext))
			{
				(*FoundArgs)->TryGetStringField(TEXT("asset_path"), AssetContext);
			}

			// Add finer-grained context for variable operations so batched writes
			// on different variable names do not collapse into one retry signature.
			if (ToolName == TEXT("blueprint.add_variable")
				|| ToolName == TEXT("blueprint.modify_variable")
				|| ToolName == TEXT("blueprint.remove_variable"))
			{
				FString VariableName;
				const TSharedPtr<FJsonObject>* VariableObj = nullptr;
				if ((*FoundArgs)->TryGetObjectField(TEXT("variable"), VariableObj) && VariableObj && (*VariableObj).IsValid())
				{
					(*VariableObj)->TryGetStringField(TEXT("name"), VariableName);
				}
				if (VariableName.IsEmpty())
				{
					(*FoundArgs)->TryGetStringField(TEXT("name"), VariableName);
				}
				if (VariableName.IsEmpty())
				{
					(*FoundArgs)->TryGetStringField(TEXT("variable_name"), VariableName);
				}
				if (!VariableName.IsEmpty())
				{
					AssetContext = AssetContext.IsEmpty()
						? FString::Printf(TEXT("var=%s"), *VariableName)
						: FString::Printf(TEXT("%s|var=%s"), *AssetContext, *VariableName);
				}
			}
		}

		// Look up tool call arguments for plan dedup
		TSharedPtr<FJsonObject> ToolCallArgsForEval;
		if (const TSharedPtr<FJsonObject>* FoundArgs = ActiveToolCallArgs.Find(ToolCallId))
		{
			ToolCallArgsForEval = *FoundArgs;
		}

		FOliveCorrectionDecision Decision = SelfCorrectionPolicy.Evaluate(
			ToolName, ResultContent, LoopDetector, RetryPolicy, AssetContext, ToolCallArgsForEval);

		switch (Decision.Action)
		{
		case EOliveCorrectionAction::FeedBackErrors:
			// Enrich the result content with retry instructions
			ResultContent = ResultContent + TEXT("\n\n") + Decision.EnrichedMessage;
			if (Brain.IsValid())
			{
				Brain->SetWorkerPhase(EOliveWorkerPhase::SelfCorrecting);
			}
			break;

		case EOliveCorrectionAction::StopWorker:
			// Loop detected — stop and report
			ResultContent = ResultContent + TEXT("\n\n") + Decision.LoopReport;
			UE_LOG(LogOliveAI, Warning, TEXT("Self-correction loop detected for tool '%s'. Stopping."), *ToolName);
			bStopAfterToolResults = true;
			// CompleteRun(Failed) is called once in ContinueAfterToolResults after all results are collected
			break;

		case EOliveCorrectionAction::Continue:
		default:
			break;
		}
	}

	// Track failures for batch-level correction directive
	if (!Result.bSuccess)
	{
		CurrentBatchFailureCount++;
		FString ErrorCode = TEXT("UNKNOWN");
		FString ErrorMsg = TEXT("Unknown error");
		if (Result.Data.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrorObj;
			if (Result.Data->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				(*ErrorObj)->TryGetStringField(TEXT("code"), ErrorCode);
				(*ErrorObj)->TryGetStringField(TEXT("message"), ErrorMsg);
			}
		}
		CurrentBatchCorrectionSummary += FString::Printf(
			TEXT("- %s (id: %s): %s - %s\n"),
			*ToolName, *ToolCallId, *ErrorCode, *ErrorMsg);

		// Track asset paths that failed with ASSET_NOT_FOUND so subsequent
		// tools in this batch targeting the same path can be skipped without
		// consuming the self-correction budget.
		if (ErrorCode == TEXT("ASSET_NOT_FOUND"))
		{
			FString FailedPath;
			if (const TSharedPtr<FJsonObject>* FoundArgs = ActiveToolCallArgs.Find(ToolCallId))
			{
				if (!(*FoundArgs)->TryGetStringField(TEXT("path"), FailedPath))
				{
					(*FoundArgs)->TryGetStringField(TEXT("asset_path"), FailedPath);
				}
			}
			if (!FailedPath.IsEmpty())
			{
				FailedAssetPaths.Add(FailedPath);
				UE_LOG(LogOliveAI, Log, TEXT("Recorded ASSET_NOT_FOUND path '%s' for batch skip."), *FailedPath);
			}
		}
	}

	// If a foundational tool failed, skip remaining dependent tools in this batch
	if (!Result.bSuccess && IsFoundationalTool(ToolName))
	{
		bSkipRemainingBatch = true;
		FailedFoundationalTool = ToolName;
		UE_LOG(LogOliveAI, Warning, TEXT("Foundational tool '%s' failed. Skipping remaining batch tools."), *ToolName);
	}

	// Create tool result message
	FOliveChatMessage ToolResultMessage;
	ToolResultMessage.Role = EOliveChatRole::Tool;
	ToolResultMessage.ToolCallId = ToolCallId;
	ToolResultMessage.ToolName = ToolName;
	ToolResultMessage.Content = ResultContent;
	ToolResultMessage.Timestamp = FDateTime::UtcNow();

	PendingToolResults.Add(ToolResultMessage);

	// Check if all tools completed
	PendingToolExecutions--;
	if (PendingToolExecutions <= 0)
	{
		ContinueAfterToolResults();
	}
}

void FOliveConversationManager::ContinueAfterToolResults()
{
	// Add all tool results to history
	for (const FOliveChatMessage& Result : PendingToolResults)
	{
		AddMessage(Result);
	}

	// Clear pending state
	PendingToolCalls.Empty();
	PendingToolResults.Empty();

	// Complete current run step
	if (bRunModeActive && FOliveRunManager::Get().HasActiveRun())
	{
		FOliveRunManager::Get().CompleteStep(true);
	}

	// Inject correction directive if there were failures in this batch
	if (CurrentBatchFailureCount > 0 && !bStopAfterToolResults)
	{
		bHasPendingCorrections = true;

		FString Directive = FString::Printf(
			TEXT("[CORRECTION REQUIRED: %d tool(s) failed in this batch. Failed operations:\n%s"
				 "You MUST retry the failed operations before proceeding to new work. "
				 "Read the asset state first if you are unsure of current values.]"),
			CurrentBatchFailureCount, *CurrentBatchCorrectionSummary);

		FOliveChatMessage DirectiveMessage;
		DirectiveMessage.Role = EOliveChatRole::User;
		DirectiveMessage.Content = Directive;
		DirectiveMessage.Timestamp = FDateTime::UtcNow();
		AddMessage(DirectiveMessage);

		UE_LOG(LogOliveAI, Log, TEXT("Injected correction directive for %d failed tools"), CurrentBatchFailureCount);
	}
	else if (CurrentBatchFailureCount == 0)
	{
		// All tools in this batch succeeded -- corrections are resolved
		bHasPendingCorrections = false;
		CorrectionRepromptCount = 0;

		// Refill re-prompt budget after successful work. Without this,
		// early empty AI responses can exhaust all re-prompts before real
		// work begins, leaving no defense against premature completion.
		ZeroToolRepromptCount = 0;

		// Allow text-only completion after meaningful multi-step work.
		// Without this, bTurnHasExplicitWriteIntent stays true for the entire
		// run and forces the AI to call tools even after all work is done,
		// wasting iterations on busywork. Require >= 4 iterations so the AI
		// must complete create, add components/variables, wire graphs, and
		// compile before it can wrap up.
		if (CurrentToolIteration >= 4)
		{
			bTurnHasExplicitWriteIntent = false;
		}
	}

	// Reset batch failure tracking
	CurrentBatchFailureCount = 0;
	CurrentBatchCorrectionSummary.Empty();

	if (bStopAfterToolResults)
	{
		bStopAfterToolResults = false;
		bIsProcessing = false;
		ResetRequestContext();
		if (Brain.IsValid() && Brain->GetState() != EOliveBrainState::Idle)
		{
			Brain->CompleteRun(EOliveRunOutcome::Failed); // Already transitions to Idle
		}
		OnProcessingComplete.Broadcast();

		// Apply deferred mode switch if pending
		if (DeferredChatMode.IsSet())
		{
			ActiveChatMode = DeferredChatMode.GetValue();
			DeferredChatMode.Reset();
			OnModeChanged.Broadcast(ActiveChatMode);
			UE_LOG(LogOliveAI, Log, TEXT("Applied deferred mode switch to %s"), LexToString(ActiveChatMode));
		}

		// Drain the next queued message if any are waiting
		DrainNextQueuedMessage();
		return;
	}

	// Send back to provider for next response
	SendToProvider();
}

// ==========================================
// Run Mode
// ==========================================

void FOliveConversationManager::EnableRunMode(const FString& RunName)
{
	bRunModeActive = true;
	UE_LOG(LogOliveAI, Log, TEXT("Run mode enabled: %s"), *RunName);
}

void FOliveConversationManager::DisableRunMode()
{
	bRunModeActive = false;
	if (FOliveRunManager::Get().HasActiveRun())
	{
		FOliveRunManager::Get().CancelRun();
	}
	UE_LOG(LogOliveAI, Log, TEXT("Run mode disabled"));
}

// ==========================================
// Message Queue Integration
// ==========================================

void FOliveConversationManager::SetRetryManager(FOliveProviderRetryManager* InRetryManager)
{
	RetryManager = InRetryManager;
	UE_LOG(LogOliveAI, Log, TEXT("Retry manager %s"), RetryManager ? TEXT("attached") : TEXT("detached"));
}

void FOliveConversationManager::SetMessageQueue(FOliveMessageQueue* InQueue)
{
	Queue = InQueue;
	UE_LOG(LogOliveAI, Log, TEXT("Message queue %s"), Queue ? TEXT("attached") : TEXT("detached"));
}

void FOliveConversationManager::DrainNextQueuedMessage()
{
	if (!Queue || !Queue->HasPending())
	{
		return;
	}

	// Safety check: only drain if we are NOT currently processing.
	// This should always be the case at call sites, but guard against
	// unexpected re-entrancy.
	if (bIsProcessing)
	{
		UE_LOG(LogOliveAI, Warning, TEXT("DrainNextQueuedMessage called while still processing -- skipping"));
		return;
	}

	const FString NextMessage = Queue->Dequeue();
	if (!NextMessage.IsEmpty())
	{
		UE_LOG(LogOliveAI, Log, TEXT("Draining queued message (remaining: %d)"), Queue->GetQueueDepth());
		SendUserMessage(NextMessage);
	}
}

// ==========================================
// Token Management
// ==========================================

int32 FOliveConversationManager::EstimateTokens(const FString& Text) const
{
	// Rough estimation: ~4 characters per token for English
	return Text.Len() / 4;
}

