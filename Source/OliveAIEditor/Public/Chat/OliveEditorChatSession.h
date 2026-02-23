// Copyright Bode Software. All Rights Reserved.

/**
 * OliveEditorChatSession.h
 *
 * Editor-lifetime singleton that owns the ConversationManager, MessageQueue,
 * and RetryManager. Survives panel open/close cycles so that:
 * - In-flight operations are not cancelled when the panel is closed.
 * - Message history is preserved when the panel is reopened.
 * - Queued messages are not lost.
 *
 * Lifetime: Created on first access via Get(), destroyed when the editor module
 * calls Shutdown() in its ShutdownModule(). The FOliveAIEditorModule calls
 * Initialize() during startup and Shutdown() during teardown.
 *
 * Thread expectations: All public methods must be called on the game thread.
 */

#pragma once

#include "CoreMinimal.h"
#include "Chat/OliveMessageQueue.h"
#include "Providers/OliveProviderRetryManager.h"

class FOliveConversationManager;

/**
 * Editor Chat Session (Singleton)
 *
 * Owns the ConversationManager, MessageQueue, and RetryManager across
 * the full editor lifetime. The chat panel holds only a TWeakPtr to
 * the ConversationManager and rebinds delegates on construction.
 *
 * Background completion: when an operation finishes while the panel is
 * closed, the session stores a summary and fires a notification toast.
 * The panel can query HasBackgroundCompletion() on reopen and show a
 * banner to the user.
 */
class OLIVEAIEDITOR_API FOliveEditorChatSession
{
public:
	/**
	 * Get the singleton instance. Creates the instance on first access.
	 * @return Reference to the singleton session
	 */
	static FOliveEditorChatSession& Get();

	/**
	 * Initialize the session. Creates the ConversationManager, wires the
	 * MessageQueue drain and RetryManager delegates. Called once from
	 * module startup.
	 *
	 * Safe to call multiple times; subsequent calls are no-ops.
	 */
	void Initialize();

	/**
	 * Shutdown and release all resources. Called from module shutdown.
	 * After this call, Get() returns a defunct instance -- do not use
	 * the session after Shutdown().
	 */
	void Shutdown();

	// ==========================================
	// Subsystem Access
	// ==========================================

	/**
	 * Get the conversation manager.
	 * @return Shared pointer to the ConversationManager, or nullptr before Initialize()
	 */
	TSharedPtr<FOliveConversationManager> GetConversationManager() const { return ConversationManager; }

	/**
	 * Get the message queue (by reference).
	 * @return Mutable reference to the message queue
	 */
	FOliveMessageQueue& GetMessageQueue() { return MessageQueue; }

	/**
	 * Get the retry manager (by reference).
	 * @return Mutable reference to the retry manager
	 */
	FOliveProviderRetryManager& GetRetryManager() { return RetryManager; }

	// ==========================================
	// Background Completion
	// ==========================================

	/**
	 * Check if an operation completed while the panel was closed.
	 * @return True if a background completion is pending
	 */
	bool HasBackgroundCompletion() const { return bHasBackgroundCompletion; }

	/**
	 * Consume the background completion flag, resetting it to false.
	 * Typically called by the panel on reopen after reading the summary.
	 * @return True if there was a background completion to consume
	 */
	bool ConsumeBackgroundCompletion();

	/**
	 * Get the summary text of what completed in the background.
	 * @return Summary string (empty if no background completion)
	 */
	const FString& GetBackgroundCompletionSummary() const { return BackgroundCompletionSummary; }

	// ==========================================
	// Panel Lifecycle
	// ==========================================

	/**
	 * Notify that the chat panel has been opened.
	 * Switches to foreground mode (no toast notifications).
	 */
	void NotifyPanelOpened();

	/**
	 * Notify that the chat panel has been closed.
	 * Switches to background mode (completion triggers toast).
	 */
	void NotifyPanelClosed();

	/**
	 * Check if the chat panel is currently open.
	 * @return True if NotifyPanelOpened was called more recently than NotifyPanelClosed
	 */
	bool IsPanelOpen() const { return bPanelOpen; }

private:
	/** Private default constructor -- use Get() for singleton access */
	FOliveEditorChatSession() = default;

	/**
	 * Handle processing-complete event when the panel is closed.
	 * Stores a summary and shows a notification toast.
	 */
	void HandleBackgroundCompletion();

	/**
	 * Show an editor notification toast for a background completion.
	 * @param Summary Text to display in the notification
	 */
	void ShowCompletionToast(const FString& Summary);

	/**
	 * Wire the message queue drain to the ConversationManager's
	 * OnProcessingComplete delegate. When processing finishes and the
	 * queue has pending messages, the next message is automatically
	 * dequeued and sent.
	 */
	void WireQueueDrain();

	// ==========================================
	// Owned Subsystems
	// ==========================================

	/** The conversation manager -- created in Initialize(), released in Shutdown() */
	TSharedPtr<FOliveConversationManager> ConversationManager;

	/** FIFO queue for messages sent while processing */
	FOliveMessageQueue MessageQueue;

	/** Retry wrapper around the AI provider */
	FOliveProviderRetryManager RetryManager;

	// ==========================================
	// State
	// ==========================================

	/** Whether Initialize() has been called */
	bool bInitialized = false;

	/** Whether the chat panel is currently open */
	bool bPanelOpen = false;

	/** Whether an operation completed while the panel was closed */
	bool bHasBackgroundCompletion = false;

	/** Summary of the background-completed operation */
	FString BackgroundCompletionSummary;

	// ==========================================
	// Delegate Handles
	// ==========================================

	/** Handle for the ConversationManager's OnProcessingComplete delegate */
	FDelegateHandle ProcessingCompleteHandle;

	/** Handle for the ConversationManager's OnError delegate */
	FDelegateHandle ErrorHandle;
};
