// Copyright Bode Software. All Rights Reserved.

#include "Chat/OliveConversationManager.h"
#include "Chat/OlivePromptAssembler.h"
#include "MCP/OliveToolRegistry.h"
#include "Index/OliveProjectIndex.h"
#include "Settings/OliveAISettings.h"
#include "Profiles/OliveFocusProfileManager.h"
#include "OliveAIEditorModule.h"
#include "Brain/OliveToolExecutionContext.h"
#include "Chat/OliveRunManager.h"
#include "Misc/Guid.h"

namespace
{
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
		TEXT("refactor")
	};
	return MessageContainsAnyKeyword(UserMessage, WriteKeywords);
}

bool DetectDangerIntent(const FString& UserMessage)
{
	static const TArray<FString> DangerKeywords = {
		TEXT("delete"),
		TEXT("remove"),
		TEXT("destroy"),
		TEXT("reparent"),
		TEXT("replace"),
		TEXT("bulk"),
		TEXT("refactor")
	};
	return MessageContainsAnyKeyword(UserMessage, DangerKeywords);
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

FOliveConversationManager::FOliveConversationManager()
{
	Brain = MakeShared<FOliveBrainLayer>();
	StartNewSession();
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

	UE_LOG(LogOliveAI, Log, TEXT("Started new conversation session: %s"), *SessionId.ToString());
}

void FOliveConversationManager::ClearHistory()
{
	CancelCurrentRequest();
	MessageHistory.Empty();
	PendingToolCalls.Empty();
	PendingToolResults.Empty();
	CurrentStreamingContent.Empty();
	bTurnHasExplicitWriteIntent = false;
	bTurnHasDangerIntent = false;
	bStopAfterToolResults = false;
	PendingConfirmationToken.Empty();
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
		UE_LOG(LogOliveAI, Warning, TEXT("Cannot send message while processing"));
		return;
	}

	if (!Provider.IsValid())
	{
		OnError.Broadcast(TEXT("No AI provider configured. Please configure a provider in settings."));
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
	bTurnHasDangerIntent = DetectDangerIntent(Message);
	bStopAfterToolResults = false;

	// Begin a Brain run
	if (Brain.IsValid())
	{
		Brain->BeginRun();
	}

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
	bWaitingForConfirmation = false;
	PendingToolCalls.Empty();
	PendingToolResults.Empty();
	PendingToolExecutions = 0;
	CurrentStreamingContent.Empty();
	PendingConfirmationToolCallId.Empty();
	PendingConfirmationToolName.Empty();
	PendingConfirmationArguments.Reset();
	PendingConfirmationToken.Empty();
	bTurnHasExplicitWriteIntent = false;
	bTurnHasDangerIntent = false;
	bStopAfterToolResults = false;
}

// ==========================================
// Context Management
// ==========================================

void FOliveConversationManager::SetActiveContext(const TArray<FString>& AssetPaths)
{
	ActiveContextPaths = AssetPaths;
}

void FOliveConversationManager::SetFocusProfile(const FString& ProfileName)
{
	ActiveFocusProfile = FOliveFocusProfileManager::Get().NormalizeProfileName(ProfileName);
}

// ==========================================
// Provider Management
// ==========================================

void FOliveConversationManager::SetProvider(TSharedPtr<IOliveAIProvider> InProvider)
{
	// Cancel any pending request with old provider
	CancelCurrentRequest();
	Provider = InProvider;
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

	const UOliveAISettings* Settings = UOliveAISettings::Get();
	const int32 MaxPromptTokens = Settings ? FMath::Max(512, Settings->MaxTokens) : 4000;

	FOlivePromptAssembler& PromptAssembler = FOlivePromptAssembler::Get();
	if (SystemPrompt.IsEmpty())
	{
		SystemMessage.Content = PromptAssembler.AssembleSystemPrompt(
			ActiveFocusProfile,
			ActiveContextPaths,
			MaxPromptTokens);
	}
	else
	{
		SystemMessage.Content = PromptAssembler.AssembleSystemPromptWithBase(
			SystemPrompt,
			ActiveFocusProfile,
			ActiveContextPaths,
			MaxPromptTokens);
	}

	return SystemMessage;
}

TArray<FOliveToolDefinition> FOliveConversationManager::GetAvailableTools()
{
	return FOliveToolRegistry::Get().GetToolsForProfile(ActiveFocusProfile);
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
	PromptDistiller.Distill(MessagesToSend, DistillConfig);

	// Get available tools via Tool Pack Manager (if initialized) for reduced schema cost
	TArray<FOliveToolDefinition> Tools;
	if (FOliveToolPackManager::Get().IsInitialized())
	{
		TArray<EOliveToolPack> Packs;
		Packs.Add(EOliveToolPack::ReadPack);

		const bool bInToolLoop = CurrentToolIteration > 0;
		if (bInToolLoop || bTurnHasExplicitWriteIntent)
		{
			Packs.Add(EOliveToolPack::WritePackBasic);
			if (bInToolLoop)
			{
				Packs.Add(EOliveToolPack::WritePackGraph);
			}
		}

		if (bTurnHasDangerIntent)
		{
			Packs.Add(EOliveToolPack::DangerPack);
		}

		Tools = FOliveToolPackManager::Get().GetCombinedPackTools(Packs, ActiveFocusProfile);
	}
	else
	{
		Tools = GetAvailableTools();
	}

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
	Provider->SendMessage(MessagesToSend, Tools, OnChunk, OnToolCall, OnComplete, OnErr, RequestOptions);
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
	if (Brain.IsValid() && Brain->GetState() == EOliveBrainState::WorkerActive)
	{
		Brain->SetWorkerPhase(PendingToolCalls.Num() > 0
			? EOliveWorkerPhase::ExecutingTools
			: EOliveWorkerPhase::Complete);
	}

	// Add assistant message to history
	FOliveChatMessage AssistantMessage;
	AssistantMessage.Role = EOliveChatRole::Assistant;
	AssistantMessage.Content = CurrentStreamingContent;
	AssistantMessage.Timestamp = FDateTime::UtcNow();
	AssistantMessage.ToolCalls = PendingToolCalls;
	AddMessage(AssistantMessage);

	// Check if we have tool calls to process
	if (PendingToolCalls.Num() > 0)
	{
		ProcessPendingToolCalls();
	}
	else
	{
		// Complete run if active
		if (bRunModeActive && FOliveRunManager::Get().HasActiveRun())
		{
			FOliveRunManager::Get().CompleteRun();
			bRunModeActive = false;
		}
		// Brain: transition to Completed → Idle
		if (Brain.IsValid() && Brain->GetState() != EOliveBrainState::Idle)
		{
			Brain->CompleteRun(EOliveRunOutcome::Completed);
			Brain->ResetToIdle();
		}
		// No tool calls, we're done
		bIsProcessing = false;
		OnProcessingComplete.Broadcast();
	}
}

void FOliveConversationManager::HandleError(const FString& ErrorMessage)
{
	UE_LOG(LogOliveAI, Error, TEXT("Conversation error: %s"), *ErrorMessage);

	bIsProcessing = false;

	// Brain: error state
	if (Brain.IsValid() && Brain->IsActive())
	{
		Brain->CompleteRun(EOliveRunOutcome::Failed);
		Brain->ResetToIdle();
	}

	PendingToolCalls.Empty();
	PendingToolResults.Empty();
	PendingConfirmationToken.Empty();
	bStopAfterToolResults = false;

	OnError.Broadcast(ErrorMessage);
	OnProcessingComplete.Broadcast();
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
	PendingToolExecutions = PendingToolCalls.Num();

	UE_LOG(LogOliveAI, Log, TEXT("Processing %d tool calls (iteration %d)"),
		PendingToolCalls.Num(), CurrentToolIteration);

	// Execute each tool call
	for (const FOliveStreamChunk& ToolCall : PendingToolCalls)
	{
		ExecuteToolCall(ToolCall);
	}

	// Tool results will be collected asynchronously
	// ContinueAfterToolResults will be called when all are done
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
	ToolContext.ActiveFocusProfile = FName(*ActiveFocusProfile);
	ToolContext.bRunModeActive = bRunModeActive;
	FOliveToolExecutionContextScope ContextScope(ToolContext);

	FOliveToolResult Result = FOliveToolRegistry::Get().ExecuteTool(ToolCall.ToolName, ToolCall.ToolArguments);

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

	// Check if this result requires user confirmation
	if (Result.bSuccess && Result.Data.IsValid() && Result.Data->HasField(TEXT("requires_confirmation")))
	{
		bool bRequiresConfirmation = Result.Data->GetBoolField(TEXT("requires_confirmation"));
		if (bRequiresConfirmation)
		{
			FString Plan = Result.Data->HasField(TEXT("plan"))
				? Result.Data->GetStringField(TEXT("plan"))
				: TEXT("This operation requires confirmation.");

			// Store pending state
			bWaitingForConfirmation = true;
			PendingConfirmationToolCallId = ToolCallId;
			PendingConfirmationToolName = ToolName;
			Result.Data->TryGetStringField(TEXT("confirmation_token"), PendingConfirmationToken);

			// Find the matching tool call arguments for re-execution
			for (const FOliveStreamChunk& TC : PendingToolCalls)
			{
				if (TC.ToolCallId == ToolCallId)
				{
					PendingConfirmationArguments = TC.ToolArguments;
					break;
				}
			}

			// Fire confirmation delegate for UI
			OnConfirmationRequired.Broadcast(ToolCallId, ToolName, Plan);

			UE_LOG(LogOliveAI, Log, TEXT("Confirmation required for tool '%s' (id: %s)"), *ToolName, *ToolCallId);
			return; // Pause the agentic loop
		}
	}

	// Use SelfCorrectionPolicy to evaluate the result
	FString ResultContent = Result.ToJsonString();
	{
		FOliveCorrectionDecision Decision = SelfCorrectionPolicy.Evaluate(
			ToolName, ResultContent, LoopDetector, RetryPolicy);

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
			if (Brain.IsValid())
			{
				Brain->CompleteRun(EOliveRunOutcome::Failed);
			}
			bStopAfterToolResults = true;
			break;

		case EOliveCorrectionAction::Continue:
		default:
			break;
		}
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

void FOliveConversationManager::ConfirmPendingOperation()
{
	if (!bWaitingForConfirmation)
	{
		return;
	}

	UE_LOG(LogOliveAI, Log, TEXT("User confirmed operation '%s' (id: %s)"),
		*PendingConfirmationToolName, *PendingConfirmationToolCallId);

	bWaitingForConfirmation = false;

	// Re-execute the tool with confirmation token from write pipeline
	TSharedPtr<FJsonObject> ConfirmedArgs = PendingConfirmationArguments.IsValid()
		? MakeShared<FJsonObject>(*PendingConfirmationArguments)
		: MakeShared<FJsonObject>();
	if (PendingConfirmationToken.IsEmpty())
	{
		FOliveToolResult MissingTokenResult = FOliveToolResult::Error(
			TEXT("PIPELINE_MISSING_TOKEN"),
			TEXT("Missing confirmation token for pending operation"),
			TEXT("Retry the operation so a fresh confirmation token can be generated"));

		FOliveChatMessage ToolResultMessage;
		ToolResultMessage.Role = EOliveChatRole::Tool;
		ToolResultMessage.ToolCallId = PendingConfirmationToolCallId;
		ToolResultMessage.ToolName = PendingConfirmationToolName;
		ToolResultMessage.Content = MissingTokenResult.ToJsonString();
		ToolResultMessage.Timestamp = FDateTime::UtcNow();

		PendingConfirmationToolCallId.Empty();
		PendingConfirmationToolName.Empty();
		PendingConfirmationArguments.Reset();
		PendingConfirmationToken.Empty();

		PendingToolResults.Add(ToolResultMessage);
		OnToolCallCompleted.Broadcast(ToolResultMessage.ToolName, ToolResultMessage.ToolCallId, MissingTokenResult);
		PendingToolExecutions--;
		if (PendingToolExecutions <= 0)
		{
			ContinueAfterToolResults();
		}
		return;
	}
	ConfirmedArgs->SetStringField(TEXT("confirmation_token"), PendingConfirmationToken);

	FOliveToolCallContext ToolContext;
	ToolContext.Origin = EOliveToolCallOrigin::EditorChat;
	ToolContext.SessionId = SessionId.ToString();
	ToolContext.ActiveFocusProfile = FName(*ActiveFocusProfile);
	FOliveToolExecutionContextScope ContextScope(ToolContext);

	FOliveToolResult Result = FOliveToolRegistry::Get().ExecuteTool(PendingConfirmationToolName, ConfirmedArgs);

	// Create result message
	FOliveChatMessage ToolResultMessage;
	ToolResultMessage.Role = EOliveChatRole::Tool;
	ToolResultMessage.ToolCallId = PendingConfirmationToolCallId;
	ToolResultMessage.ToolName = PendingConfirmationToolName;
	ToolResultMessage.Content = Result.ToJsonString();
	ToolResultMessage.Timestamp = FDateTime::UtcNow();

	// Clear pending state
	PendingConfirmationToolCallId.Empty();
	PendingConfirmationToolName.Empty();
	PendingConfirmationArguments.Reset();
	PendingConfirmationToken.Empty();

	// Add result and continue
	PendingToolResults.Add(ToolResultMessage);
	OnToolCallCompleted.Broadcast(ToolResultMessage.ToolName, ToolResultMessage.ToolCallId, Result);

	PendingToolExecutions--;
	if (PendingToolExecutions <= 0)
	{
		ContinueAfterToolResults();
	}
}

void FOliveConversationManager::DenyPendingOperation()
{
	if (!bWaitingForConfirmation)
	{
		return;
	}

	UE_LOG(LogOliveAI, Log, TEXT("User denied operation '%s' (id: %s)"),
		*PendingConfirmationToolName, *PendingConfirmationToolCallId);

	bWaitingForConfirmation = false;

	// Create denial result message
	FOliveChatMessage ToolResultMessage;
	ToolResultMessage.Role = EOliveChatRole::Tool;
	ToolResultMessage.ToolCallId = PendingConfirmationToolCallId;
	ToolResultMessage.ToolName = PendingConfirmationToolName;
	ToolResultMessage.Content = TEXT("{\"success\":false,\"error\":{\"code\":\"USER_DENIED\",\"message\":\"User denied this operation. Please ask the user how they would like to proceed.\"}}");
	ToolResultMessage.Timestamp = FDateTime::UtcNow();

	// Clear pending state
	PendingConfirmationToolCallId.Empty();
	PendingConfirmationToolName.Empty();
	PendingConfirmationArguments.Reset();
	PendingConfirmationToken.Empty();

	// Add result and continue
	PendingToolResults.Add(ToolResultMessage);

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

	if (bStopAfterToolResults)
	{
		bStopAfterToolResults = false;
		bIsProcessing = false;
		if (Brain.IsValid() && Brain->GetState() != EOliveBrainState::Idle)
		{
			Brain->CompleteRun(EOliveRunOutcome::Failed);
			Brain->ResetToIdle();
		}
		OnProcessingComplete.Broadcast();
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
// Token Management
// ==========================================

int32 FOliveConversationManager::EstimateTokens(const FString& Text) const
{
	// Rough estimation: ~4 characters per token for English
	return Text.Len() / 4;
}

