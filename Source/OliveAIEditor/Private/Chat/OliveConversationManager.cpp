// Copyright Bode Software. All Rights Reserved.

#include "Chat/OliveConversationManager.h"
#include "MCP/OliveToolRegistry.h"
#include "Index/OliveProjectIndex.h"
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

	// Reset tool iteration counter
	CurrentToolIteration = 0;

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
	PendingToolCalls.Empty();
	PendingToolResults.Empty();
	PendingToolExecutions = 0;
	CurrentStreamingContent.Empty();
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

	// Start with base system prompt
	FString FullPrompt = SystemPrompt;

	// Add project context if available
	if (FOliveProjectIndex::Get().IsReady())
	{
		const FOliveProjectConfig& Config = FOliveProjectIndex::Get().GetProjectConfig();
		FullPrompt += FString::Printf(TEXT("\n\n## Project Context\n"));
		FullPrompt += FString::Printf(TEXT("Project: %s\n"), *Config.ProjectName);
		FullPrompt += FString::Printf(TEXT("Engine: %s\n"), *Config.EngineVersion);
	}

	// Add active asset context
	if (ActiveContextPaths.Num() > 0)
	{
		FullPrompt += TEXT("\n\n## Active Context Assets\n");
		for (const FString& Path : ActiveContextPaths)
		{
			TOptional<FOliveAssetInfo> AssetInfo = FOliveProjectIndex::Get().GetAssetByPath(Path);
			if (AssetInfo.IsSet())
			{
				FullPrompt += FString::Printf(TEXT("- %s (%s): %s\n"),
					*AssetInfo->Name,
					*AssetInfo->AssetClass.ToString(),
					*Path);
			}
			else
			{
				FullPrompt += FString::Printf(TEXT("- %s\n"), *Path);
			}
		}
	}

	SystemMessage.Content = FullPrompt;
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

	// Create tool result message
	FOliveChatMessage ToolResultMessage;
	ToolResultMessage.Role = EOliveChatRole::Tool;
	ToolResultMessage.ToolCallId = ToolCallId;
	ToolResultMessage.ToolName = ToolName;
	ToolResultMessage.Content = Result.ToJsonString();
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
