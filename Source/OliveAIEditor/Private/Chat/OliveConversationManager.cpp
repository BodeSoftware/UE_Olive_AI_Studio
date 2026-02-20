// Copyright Bode Software. All Rights Reserved.

#include "Chat/OliveConversationManager.h"
#include "Chat/OlivePromptAssembler.h"
#include "MCP/OliveToolRegistry.h"
#include "Index/OliveProjectIndex.h"
#include "Settings/OliveAISettings.h"
#include "OliveAIEditorModule.h"
#include "Misc/Guid.h"

FOliveConversationManager::FOliveConversationManager()
{
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

	// Reset tool iteration counter and compile retry counter
	CurrentToolIteration = 0;
	CompileRetryCount = 0;

	// Send to provider
	SendToProvider();
}

void FOliveConversationManager::CancelCurrentRequest()
{
	if (Provider.IsValid() && bIsProcessing)
	{
		Provider->CancelRequest();
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
	ActiveFocusProfile = ProfileName;
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

	// Get available tools
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

	Provider->SendMessage(MessagesToSend, Tools, OnChunk, OnToolCall, OnComplete, OnErr);
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
		// No tool calls, we're done
		bIsProcessing = false;
		OnProcessingComplete.Broadcast();
	}
}

void FOliveConversationManager::HandleError(const FString& ErrorMessage)
{
	UE_LOG(LogOliveAI, Error, TEXT("Conversation error: %s"), *ErrorMessage);

	bIsProcessing = false;
	PendingToolCalls.Empty();
	PendingToolResults.Empty();

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

	// Execute tool through registry
	FOliveToolResult Result = FOliveToolRegistry::Get().ExecuteTool(ToolCall.ToolName, ToolCall.ToolArguments);

	// Handle result
	HandleToolResult(ToolCall.ToolCallId, ToolCall.ToolName, Result);
}

void FOliveConversationManager::HandleToolResult(
	const FString& ToolCallId,
	const FString& ToolName,
	const FOliveToolResult& Result)
{
	OnToolCallCompleted.Broadcast(ToolName, ToolCallId, Result);

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

	// Check for compile failure and enrich with retry hint
	FString ResultContent = Result.ToJsonString();
	if (Result.bSuccess && Result.Data.IsValid())
	{
		const TSharedPtr<FJsonObject>* CompileResult;
		if (Result.Data->TryGetObjectField(TEXT("compile_result"), CompileResult))
		{
			bool bCompileSuccess = (*CompileResult)->GetBoolField(TEXT("success"));
			if (!bCompileSuccess && CompileRetryCount < MaxCompileRetries)
			{
				CompileRetryCount++;

				// Enrich the tool result with retry hint
				FString CompileErrors;
				const TArray<TSharedPtr<FJsonValue>>* ErrorsArray;
				if ((*CompileResult)->TryGetArrayField(TEXT("errors"), ErrorsArray))
				{
					for (const auto& ErrVal : *ErrorsArray)
					{
						if (ErrVal->Type == EJson::String)
						{
							CompileErrors += ErrVal->AsString() + TEXT("\n");
						}
						else if (ErrVal->AsObject().IsValid())
						{
							CompileErrors += ErrVal->AsObject()->GetStringField(TEXT("message")) + TEXT("\n");
						}
					}
				}

				ResultContent = FString::Printf(
					TEXT("%s\n\n[COMPILE FAILED - Attempt %d/%d] The Blueprint failed to compile. Errors:\n%s\nPlease analyze the errors and fix the issue. You may re-call the write tool with corrections."),
					*ResultContent, CompileRetryCount, MaxCompileRetries, *CompileErrors);

				UE_LOG(LogOliveAI, Log, TEXT("Compile failed, retry %d/%d for tool '%s'"),
					CompileRetryCount, MaxCompileRetries, *ToolName);
			}
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

	// Re-execute the tool with confirmed flag
	TSharedPtr<FJsonObject> ConfirmedArgs = PendingConfirmationArguments.IsValid()
		? MakeShared<FJsonObject>(*PendingConfirmationArguments)
		: MakeShared<FJsonObject>();
	ConfirmedArgs->SetBoolField(TEXT("confirmed"), true);

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

	// Send back to provider for next response
	SendToProvider();
}

// ==========================================
// Token Management
// ==========================================

int32 FOliveConversationManager::EstimateTokens(const FString& Text) const
{
	// Rough estimation: ~4 characters per token for English
	return Text.Len() / 4;
}
