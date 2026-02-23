// Copyright Bode Software. All Rights Reserved.

/**
 * OliveMessageQueue.cpp
 *
 * Implementation of the FIFO message queue for buffering user messages
 * while the ConversationManager is busy processing an agentic loop.
 */

#include "Chat/OliveMessageQueue.h"
#include "OliveAIEditorModule.h"

FOliveMessageQueue::FOliveMessageQueue()
{
	// Pre-allocate to avoid reallocation for typical usage
	Queue.Reserve(MaxQueuedMessages);
}

bool FOliveMessageQueue::Enqueue(const FString& Message)
{
	Queue.Add(Message);

	const int32 Depth = Queue.Num();
	UE_LOG(LogOliveAI, Log, TEXT("Message queued (depth: %d/%d)"), Depth, MaxQueuedMessages);
	if (Depth > MaxQueuedMessages)
	{
		UE_LOG(LogOliveAI, Warning,
			TEXT("Message queue depth exceeded soft threshold (%d > %d). No input dropped."),
			Depth, MaxQueuedMessages);
	}

	OnMessageQueued.Broadcast(Depth);

	return true;
}

FString FOliveMessageQueue::Dequeue()
{
	if (Queue.Num() == 0)
	{
		return FString();
	}

	// Remove and return the front (oldest) element
	FString Message = MoveTemp(Queue[0]);
	Queue.RemoveAt(0);

	UE_LOG(LogOliveAI, Log, TEXT("Message dequeued (remaining: %d)"), Queue.Num());

	// If queue is now empty, notify listeners
	if (Queue.Num() == 0)
	{
		OnQueueDrained.Broadcast();
	}

	return Message;
}

bool FOliveMessageQueue::HasPending() const
{
	return Queue.Num() > 0;
}

int32 FOliveMessageQueue::GetQueueDepth() const
{
	return Queue.Num();
}

void FOliveMessageQueue::Clear()
{
	const int32 ClearedCount = Queue.Num();
	Queue.Empty();

	if (ClearedCount > 0)
	{
		UE_LOG(LogOliveAI, Log, TEXT("Message queue cleared (%d messages discarded)"), ClearedCount);
	}
}
