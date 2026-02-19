// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Providers/IOliveAIProvider.h"

class SScrollBox;
class SVerticalBox;

/**
 * UI Message Data
 *
 * Internal representation of a message for display.
 */
struct FOliveUIMessage
{
	/** Unique message ID */
	FGuid Id;

	/** Role (user, assistant, system, tool) */
	EOliveChatRole Role;

	/** Message content */
	FString Content;

	/** Timestamp */
	FDateTime Timestamp;

	/** Is this message still being streamed */
	bool bIsStreaming = false;

	/** Tool calls made by this message */
	TArray<FOliveStreamChunk> ToolCalls;

	/** Error message if any */
	FString ErrorMessage;
};

/**
 * Message List Widget
 *
 * Displays the conversation history with proper styling for
 * different message types. Supports streaming updates.
 */
class OLIVEAIEDITOR_API SOliveAIMessageList : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOliveAIMessageList) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// ==========================================
	// Message Management
	// ==========================================

	/** Add a user message */
	FGuid AddUserMessage(const FString& Message);

	/** Add an assistant message */
	FGuid AddAssistantMessage(const FString& Message);

	/** Add a system message */
	FGuid AddSystemMessage(const FString& Message);

	/** Add an error message */
	FGuid AddErrorMessage(const FString& Message);

	/** Add a message from chat message struct */
	FGuid AddMessage(const FOliveChatMessage& ChatMessage);

	/** Append text to the last message (for streaming) */
	void AppendToLastMessage(const FString& Chunk);

	/** Mark the last message as complete (no longer streaming) */
	void CompleteLastMessage();

	/** Update message content */
	void UpdateMessage(const FGuid& MessageId, const FString& NewContent);

	// ==========================================
	// Tool Call Display
	// ==========================================

	/** Add a tool call indicator to the current message */
	void AddToolCallIndicator(const FString& ToolName, const FString& ToolCallId);

	/** Update tool call status */
	void UpdateToolCallStatus(const FString& ToolCallId, bool bSuccess, const FString& ResultSummary);

	// ==========================================
	// Navigation
	// ==========================================

	/** Clear all messages */
	void ClearMessages();

	/** Scroll to bottom */
	void ScrollToBottom();

	/** Get message count */
	int32 GetMessageCount() const { return Messages.Num(); }

private:
	/** Build a message widget */
	TSharedRef<SWidget> BuildMessageWidget(TSharedPtr<FOliveUIMessage> Message);

	/** Get color for role */
	FSlateColor GetRoleColor(EOliveChatRole Role) const;

	/** Get role display name */
	FText GetRoleName(EOliveChatRole Role) const;

	/** Get role icon */
	const FSlateBrush* GetRoleIcon(EOliveChatRole Role) const;

	/** Refresh the message display */
	void RefreshDisplay();

	// ==========================================
	// State
	// ==========================================

	TArray<TSharedPtr<FOliveUIMessage>> Messages;
	TSharedPtr<SScrollBox> ScrollBox;
	TSharedPtr<SVerticalBox> MessagesContainer;

	/** Map of tool call ID to widget for status updates */
	TMap<FString, TSharedPtr<SWidget>> ToolCallWidgets;

	/** Currently streaming message widget */
	TWeakPtr<SWidget> StreamingMessageWidget;
};
