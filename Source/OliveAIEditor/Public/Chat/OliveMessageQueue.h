// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Delegate fired when a message is enqueued.
 * @param QueueDepth The number of messages currently in the queue after enqueue.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveMessageQueued, int32 /* QueueDepth */);

/**
 * Delegate fired when the queue becomes empty after draining.
 */
DECLARE_MULTICAST_DELEGATE(FOnOliveQueueDrained);

/**
 * FIFO message queue for user messages sent while the ConversationManager is processing.
 *
 * When the user sends a message during an active agentic loop, the message is
 * queued here instead of being discarded. The ConversationManager dequeues one
 * message at a time after each processing cycle completes, feeding it into the
 * next agentic loop iteration.
 *
 * Invariants:
 * - Queue depth is unbounded to avoid dropping user input.
 * - MaxQueuedMessages is treated as a soft warning threshold for UX telemetry.
 * - Queue is drained one message at a time (each triggers a full agentic loop
 *   cycle before the next is dequeued).
 *
 * Thread safety: This class is designed to be used on the game thread only.
 * No internal synchronization is provided.
 */
class OLIVEAIEDITOR_API FOliveMessageQueue
{
public:
	FOliveMessageQueue();

	/**
	 * Enqueue a user message at the back of the queue.
	 *
	 * Queue is intentionally unbounded to preserve all user input.
	 * If depth exceeds MaxQueuedMessages, a warning is logged as a soft-pressure
	 * signal, but no messages are dropped.
	 *
	 * @param Message The user message text to enqueue. Must not be empty.
	 * @return True if the message was enqueued.
	 */
	bool Enqueue(const FString& Message);

	/**
	 * Dequeue the next (oldest) message from the front of the queue.
	 *
	 * If the queue becomes empty after this call, OnQueueDrained is broadcast.
	 *
	 * @return The next queued message, or an empty string if the queue is empty.
	 */
	FString Dequeue();

	/**
	 * Check whether the queue has any pending messages.
	 *
	 * @return True if there is at least one message in the queue.
	 */
	bool HasPending() const;

	/**
	 * Get the number of messages currently in the queue.
	 *
	 * @return The current queue depth (0 to MaxQueuedMessages).
	 */
	int32 GetQueueDepth() const;

	/**
	 * Remove all messages from the queue.
	 *
	 * Does NOT fire OnQueueDrained since the queue was not drained naturally.
	 */
	void Clear();

	/** Fired when a message is enqueued. Carries the new queue depth. */
	FOnOliveMessageQueued OnMessageQueued;

	/** Fired when the queue becomes empty after a Dequeue() call. */
	FOnOliveQueueDrained OnQueueDrained;

	/** Soft warning threshold for queued depth. Messages are not dropped at this limit. */
	int32 MaxQueuedMessages = 5;

private:
	/** Internal FIFO storage. Index 0 is the front (oldest). */
	TArray<FString> Queue;
};
