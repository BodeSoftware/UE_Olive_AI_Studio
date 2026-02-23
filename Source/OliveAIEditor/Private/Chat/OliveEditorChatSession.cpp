// Copyright Bode Software. All Rights Reserved.

/**
 * OliveEditorChatSession.cpp
 *
 * Implementation of the editor-lifetime chat session singleton.
 * See OliveEditorChatSession.h for class documentation.
 */

#include "Chat/OliveEditorChatSession.h"
#include "Chat/OliveConversationManager.h"
#include "Settings/OliveAISettings.h"
#include "OliveAIEditorModule.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"

#define LOCTEXT_NAMESPACE "OliveEditorChatSession"

// ==========================================
// Singleton Access
// ==========================================

FOliveEditorChatSession& FOliveEditorChatSession::Get()
{
	static FOliveEditorChatSession Instance;
	return Instance;
}

// ==========================================
// Lifecycle
// ==========================================

void FOliveEditorChatSession::Initialize()
{
	if (bInitialized)
	{
		UE_LOG(LogOliveAI, Warning, TEXT("FOliveEditorChatSession::Initialize() called more than once -- ignoring"));
		return;
	}

	UE_LOG(LogOliveAI, Log, TEXT("Initializing EditorChatSession"));

	// Create the conversation manager
	ConversationManager = MakeShared<FOliveConversationManager>();

	// Wire the message queue into the ConversationManager so it can enqueue
	// messages when processing, and drain them when processing completes.
	ConversationManager->SetMessageQueue(&MessageQueue);

	// Wire the retry manager into the ConversationManager so SendToProvider()
	// routes through RetryManager->SendWithRetry() with automatic retry on
	// transient errors and rate-limit handling.
	ConversationManager->SetRetryManager(&RetryManager);

	// Apply retry settings from UOliveAISettings
	if (const UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		RetryManager.MaxRetries = Settings->MaxProviderRetries;
		RetryManager.MaxRetryAfterSeconds = Settings->MaxRetryAfterWaitSeconds;
	}

	// Wire the queue drain so pending messages are auto-sent after processing completes
	WireQueueDrain();

	bInitialized = true;
	UE_LOG(LogOliveAI, Log, TEXT("EditorChatSession initialized"));
}

void FOliveEditorChatSession::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}

	UE_LOG(LogOliveAI, Log, TEXT("Shutting down EditorChatSession"));

	// Unbind delegates before releasing the ConversationManager
	if (ConversationManager.IsValid())
	{
		if (ProcessingCompleteHandle.IsValid())
		{
			ConversationManager->OnProcessingComplete.Remove(ProcessingCompleteHandle);
			ProcessingCompleteHandle.Reset();
		}
		if (ErrorHandle.IsValid())
		{
			ConversationManager->OnError.Remove(ErrorHandle);
			ErrorHandle.Reset();
		}

		// Cancel any in-flight request
		ConversationManager->CancelCurrentRequest();
	}

	// Cancel any pending retries
	RetryManager.Cancel();

	// Clear the message queue
	MessageQueue.Clear();

	// Release the conversation manager
	ConversationManager.Reset();

	bInitialized = false;
	bPanelOpen = false;
	bHasBackgroundCompletion = false;
	BackgroundCompletionSummary.Empty();

	UE_LOG(LogOliveAI, Log, TEXT("EditorChatSession shutdown complete"));
}

// ==========================================
// Background Completion
// ==========================================

bool FOliveEditorChatSession::ConsumeBackgroundCompletion()
{
	if (!bHasBackgroundCompletion)
	{
		return false;
	}

	bHasBackgroundCompletion = false;
	BackgroundCompletionSummary.Empty();
	return true;
}

void FOliveEditorChatSession::HandleBackgroundCompletion()
{
	if (!ConversationManager.IsValid())
	{
		return;
	}

	// Build a simple summary from the last assistant message
	const TArray<FOliveChatMessage>& History = ConversationManager->GetMessageHistory();
	FString Summary = TEXT("AI processing completed.");

	// Walk backwards to find the last assistant message
	for (int32 i = History.Num() - 1; i >= 0; --i)
	{
		if (History[i].Role == EOliveChatRole::Assistant && !History[i].Content.IsEmpty())
		{
			// Truncate to a reasonable toast length
			constexpr int32 MaxSummaryLength = 120;
			Summary = History[i].Content.Left(MaxSummaryLength);
			if (History[i].Content.Len() > MaxSummaryLength)
			{
				Summary += TEXT("...");
			}
			break;
		}
	}

	BackgroundCompletionSummary = Summary;
	bHasBackgroundCompletion = true;

	ShowCompletionToast(Summary);
}

void FOliveEditorChatSession::ShowCompletionToast(const FString& Summary)
{
	FNotificationInfo Info(FText::Format(
		LOCTEXT("BackgroundComplete", "Olive AI: {0}"),
		FText::FromString(Summary)
	));

	Info.bUseLargeFont = false;
	Info.bUseThrobber = false;
	Info.bFireAndForget = true;
	Info.FadeOutDuration = 1.0f;
	Info.ExpireDuration = 5.0f;

	FSlateNotificationManager::Get().AddNotification(Info);

	UE_LOG(LogOliveAI, Log, TEXT("Background completion toast: %s"), *Summary);
}

// ==========================================
// Panel Lifecycle
// ==========================================

void FOliveEditorChatSession::NotifyPanelOpened()
{
	bPanelOpen = true;
	UE_LOG(LogOliveAI, Verbose, TEXT("Chat panel opened"));
}

void FOliveEditorChatSession::NotifyPanelClosed()
{
	bPanelOpen = false;
	UE_LOG(LogOliveAI, Verbose, TEXT("Chat panel closed"));
}

// ==========================================
// Queue Drain Wiring
// ==========================================

void FOliveEditorChatSession::WireQueueDrain()
{
	if (!ConversationManager.IsValid())
	{
		UE_LOG(LogOliveAI, Warning, TEXT("WireQueueDrain called but ConversationManager is null"));
		return;
	}

	// Listen to OnProcessingComplete to:
	// 1. Auto-dequeue next message if queue has pending items
	// 2. Fire background completion notification if panel is closed
	ProcessingCompleteHandle = ConversationManager->OnProcessingComplete.AddLambda([this]()
	{
		// Background completion check
		if (!bPanelOpen)
		{
			HandleBackgroundCompletion();
		}

		// Queue drain: if there are pending messages, send the next one
		if (MessageQueue.HasPending())
		{
			const FString NextMessage = MessageQueue.Dequeue();
			if (!NextMessage.IsEmpty() && ConversationManager.IsValid())
			{
				UE_LOG(LogOliveAI, Log, TEXT("Auto-dequeuing message from queue (remaining: %d)"), MessageQueue.GetQueueDepth());
				ConversationManager->SendUserMessage(NextMessage);
			}
		}
	});

	// Listen to OnError to also drain the queue (an error terminates processing,
	// so we should still attempt the next queued message, or at least notify
	// the user if the panel is closed).
	ErrorHandle = ConversationManager->OnError.AddLambda([this](const FString& ErrorMessage)
	{
		if (!bPanelOpen)
		{
			// Store the error as a background completion so the user sees it on reopen
			BackgroundCompletionSummary = FString::Printf(TEXT("Error: %s"), *ErrorMessage.Left(100));
			bHasBackgroundCompletion = true;

			ShowCompletionToast(BackgroundCompletionSummary);
		}

		// DESIGN NOTE: We do NOT auto-dequeue the next message on error.
		// The error may be systemic (e.g., auth failure) and auto-sending
		// the next message would just produce another error. The user should
		// review the error first. Architect should review this decision.
	});
}

#undef LOCTEXT_NAMESPACE
