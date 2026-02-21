// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Providers/IOliveAIProvider.h"
#include "UI/OliveResultCards.h"
#include "Chat/OliveRunManager.h"

class SScrollBox;
class SVerticalBox;
class STextBlock;
class SImage;

/** Tool call display status */
enum class EOliveToolCallStatus : uint8
{
	Running,
	Completed,
	Failed
};

/** State for a tool call indicator widget */
struct FOliveToolCallWidgetState
{
	TSharedPtr<SImage> StatusIcon;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<STextBlock> SummaryText;
	EOliveToolCallStatus Status = EOliveToolCallStatus::Running;
};

/** Delegate for navigation actions from result cards */
DECLARE_DELEGATE_OneParam(FOnOliveNavigationAction, const FOliveNavigationAction&);

/** Run mode control delegates */
DECLARE_DELEGATE(FOnRunPause);
DECLARE_DELEGATE(FOnRunResume);
DECLARE_DELEGATE(FOnRunCancel);
DECLARE_DELEGATE_OneParam(FOnRunRetryStep, int32);
DECLARE_DELEGATE_OneParam(FOnRunSkipStep, int32);
DECLARE_DELEGATE_OneParam(FOnRunRollback, const FString&);

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
		SLATE_EVENT(FOnOliveNavigationAction, OnNavigationAction)
		SLATE_EVENT(FOnRunPause, OnRunPause)
		SLATE_EVENT(FOnRunResume, OnRunResume)
		SLATE_EVENT(FOnRunCancel, OnRunCancel)
		SLATE_EVENT(FOnRunRetryStep, OnRunRetryStep)
		SLATE_EVENT(FOnRunSkipStep, OnRunSkipStep)
		SLATE_EVENT(FOnRunRollback, OnRunRollback)
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

	/** Update tool call status (spinner -> checkmark/error, show summary) */
	void UpdateToolCallStatus(const FString& ToolCallId, bool bSuccess, const FString& ResultSummary);

	// ==========================================
	// Result Cards
	// ==========================================

	/** Add a typed result card for a tool result */
	void AddResultCard(const FString& ToolCallId, const FString& ToolName, const FOliveToolResult& Result);

	// ==========================================
	// Run Mode Display
	// ==========================================

	/** Add a run header when a new run starts */
	void AddRunHeader(const FOliveRun& Run);

	/** Update a run step display */
	void UpdateRunStep(const FOliveRun& Run, int32 StepIndex);

	/** Update overall run status display */
	void UpdateRunStatus(const FOliveRun& Run);

	// ==========================================
	// Confirmation Display
	// ==========================================

	/** Delegate for confirmation action (true = confirm, false = deny) */
	DECLARE_DELEGATE_OneParam(FOnConfirmationAction, bool);

	/** Add a confirmation widget with confirm/deny buttons */
	void AddConfirmationWidget(const FString& ToolCallId, const FString& Plan, FOnConfirmationAction OnAction);

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
	// Result Card Builders
	// ==========================================

	TSharedRef<SWidget> BuildResultCardWidget(const FOliveResultCardData& CardData);
	TSharedRef<SWidget> BuildBlueprintReadCard(const FOliveResultCardData& CardData);
	TSharedRef<SWidget> BuildBlueprintWriteCard(const FOliveResultCardData& CardData);
	TSharedRef<SWidget> BuildCompileErrorsCard(const FOliveResultCardData& CardData);
	TSharedRef<SWidget> BuildSnapshotCard(const FOliveResultCardData& CardData);
	TSharedRef<SWidget> BuildRawJsonCard(const FOliveResultCardData& CardData);
	TSharedRef<SWidget> BuildNavigationActions(const TArray<FOliveNavigationAction>& Actions);
	TSharedRef<SWidget> BuildRawJsonExpander(const TSharedPtr<FJsonObject>& JsonData);

	// ==========================================
	// Run Mode Builders
	// ==========================================

	TSharedRef<SWidget> BuildRunHeaderWidget(const FOliveRun& Run);
	TSharedRef<SWidget> BuildRunStepWidget(const FOliveRunStep& Step, int32 StepIndex);
	TSharedRef<SWidget> BuildRunControlsWidget(const FOliveRun& Run);

	// ==========================================
	// State
	// ==========================================

	TArray<TSharedPtr<FOliveUIMessage>> Messages;
	TSharedPtr<SScrollBox> ScrollBox;
	TSharedPtr<SVerticalBox> MessagesContainer;

	/** Map of tool call ID to widget state for status updates */
	TMap<FString, FOliveToolCallWidgetState> ToolCallWidgetStates;

	/** Navigation action delegate */
	FOnOliveNavigationAction OnNavigationAction;

	/** Run mode delegates */
	FOnRunPause OnRunPause;
	FOnRunResume OnRunResume;
	FOnRunCancel OnRunCancel;
	FOnRunRetryStep OnRunRetryStep;
	FOnRunSkipStep OnRunSkipStep;
	FOnRunRollback OnRunRollback;

	/** Run mode UI containers */
	TSharedPtr<SVerticalBox> RunStepsContainer;
	TSharedPtr<SWidget> RunControlsWidget;
	TMap<int32, TSharedPtr<SWidget>> RunStepWidgets;

	/** Currently streaming message widget */
	TWeakPtr<SWidget> StreamingMessageWidget;
};
