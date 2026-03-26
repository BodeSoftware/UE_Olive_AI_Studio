// Copyright Bode Software. All Rights Reserved.

#include "UI/SOliveAIMessageList.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Styling/AppStyle.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Widgets/Notifications/SProgressBar.h"

#define LOCTEXT_NAMESPACE "OliveAIMessageList"

void SOliveAIMessageList::Construct(const FArguments& InArgs)
{
	OnNavigationAction = InArgs._OnNavigationAction;
	OnRunPause = InArgs._OnRunPause;
	OnRunResume = InArgs._OnRunResume;
	OnRunCancel = InArgs._OnRunCancel;
	OnRunRetryStep = InArgs._OnRunRetryStep;
	OnRunSkipStep = InArgs._OnRunSkipStep;
	OnRunRollback = InArgs._OnRunRollback;

	ChildSlot
	[
		SAssignNew(ScrollBox, SScrollBox)
		+ SScrollBox::Slot()
		[
			SAssignNew(MessagesContainer, SVerticalBox)
		]
	];
}

// ==========================================
// Message Management
// ==========================================

FGuid SOliveAIMessageList::AddUserMessage(const FString& Message)
{
	TSharedPtr<FOliveUIMessage> UIMessage = MakeShared<FOliveUIMessage>();
	UIMessage->Id = FGuid::NewGuid();
	UIMessage->Role = EOliveChatRole::User;
	UIMessage->Content = Message;
	UIMessage->Timestamp = FDateTime::UtcNow();

	Messages.Add(UIMessage);

	MessagesContainer->AddSlot()
		.AutoHeight()
		.Padding(0, 4)
		[
			BuildMessageWidget(UIMessage)
		];

	return UIMessage->Id;
}

FGuid SOliveAIMessageList::AddAssistantMessage(const FString& Message)
{
	TSharedPtr<FOliveUIMessage> UIMessage = MakeShared<FOliveUIMessage>();
	UIMessage->Id = FGuid::NewGuid();
	UIMessage->Role = EOliveChatRole::Assistant;
	UIMessage->Content = Message;
	UIMessage->Timestamp = FDateTime::UtcNow();
	UIMessage->bIsStreaming = true;

	Messages.Add(UIMessage);

	TSharedRef<SWidget> Widget = BuildMessageWidget(UIMessage);
	StreamingMessageWidget = Widget;

	MessagesContainer->AddSlot()
		.AutoHeight()
		.Padding(0, 4)
		[
			Widget
		];

	return UIMessage->Id;
}

FGuid SOliveAIMessageList::AddSystemMessage(const FString& Message)
{
	TSharedPtr<FOliveUIMessage> UIMessage = MakeShared<FOliveUIMessage>();
	UIMessage->Id = FGuid::NewGuid();
	UIMessage->Role = EOliveChatRole::System;
	UIMessage->Content = Message;
	UIMessage->Timestamp = FDateTime::UtcNow();

	Messages.Add(UIMessage);

	MessagesContainer->AddSlot()
		.AutoHeight()
		.Padding(0, 4)
		[
			BuildMessageWidget(UIMessage)
		];

	return UIMessage->Id;
}

FGuid SOliveAIMessageList::AddErrorMessage(const FString& Message)
{
	TSharedPtr<FOliveUIMessage> UIMessage = MakeShared<FOliveUIMessage>();
	UIMessage->Id = FGuid::NewGuid();
	UIMessage->Role = EOliveChatRole::System;
	UIMessage->ErrorMessage = Message;
	UIMessage->Timestamp = FDateTime::UtcNow();

	Messages.Add(UIMessage);

	MessagesContainer->AddSlot()
		.AutoHeight()
		.Padding(0, 4)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ErrorReporting.Box"))
			.Padding(8.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(0, 0, 8, 0)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Error"))
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Message))
					.AutoWrapText(true)
					.ColorAndOpacity(FLinearColor::Red)
				]
			]
		];

	return UIMessage->Id;
}

FGuid SOliveAIMessageList::AddMessage(const FOliveChatMessage& ChatMessage)
{
	switch (ChatMessage.Role)
	{
	case EOliveChatRole::User:
		return AddUserMessage(ChatMessage.Content);
	case EOliveChatRole::Assistant:
		{
			FGuid Id = AddAssistantMessage(ChatMessage.Content);

			// Mark as not streaming if content is already complete
			if (Messages.Num() > 0)
			{
				Messages.Last()->bIsStreaming = false;
			}

			return Id;
		}
	case EOliveChatRole::System:
		return AddSystemMessage(ChatMessage.Content);
	case EOliveChatRole::Tool:
		// Tool messages are displayed as part of the conversation flow
		return AddSystemMessage(FString::Printf(TEXT("[%s result]: %s"), *ChatMessage.ToolName, *ChatMessage.Content));
	default:
		return AddSystemMessage(ChatMessage.Content);
	}
}

void SOliveAIMessageList::AppendToLastMessage(const FString& Chunk)
{
	if (Messages.Num() == 0)
	{
		// No message to append to, create new one
		AddAssistantMessage(Chunk);
		return;
	}

	TSharedPtr<FOliveUIMessage> LastMessage = Messages.Last();

	// Only append to streaming assistant messages
	if (LastMessage->Role != EOliveChatRole::Assistant || !LastMessage->bIsStreaming)
	{
		// Create new message for this response
		AddAssistantMessage(Chunk);
		return;
	}

	LastMessage->Content += Chunk;

	// Refresh the display
	RefreshDisplay();
}

void SOliveAIMessageList::CompleteLastMessage()
{
	if (Messages.Num() > 0)
	{
		Messages.Last()->bIsStreaming = false;
	}
	StreamingMessageWidget.Reset();
}

void SOliveAIMessageList::UpdateMessage(const FGuid& MessageId, const FString& NewContent)
{
	for (TSharedPtr<FOliveUIMessage>& Message : Messages)
	{
		if (Message->Id == MessageId)
		{
			Message->Content = NewContent;
			RefreshDisplay();
			break;
		}
	}
}

// ==========================================
// Tool Call Display
// ==========================================

void SOliveAIMessageList::AddToolCallIndicator(const FString& ToolName, const FString& ToolCallId)
{
	FOliveToolCallWidgetState WidgetState;

	TSharedRef<SWidget> Indicator = SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)

			// Status icon (spinner initially)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 4, 0)
			[
				SAssignNew(WidgetState.StatusIcon, SImage)
				.Image(FAppStyle::GetBrush("Icons.Refresh"))
				.DesiredSizeOverride(FVector2D(12, 12))
			]

			// Status text
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SAssignNew(WidgetState.StatusText, STextBlock)
				.Text(FText::Format(LOCTEXT("ToolCalling", "Calling {0}..."), FText::FromString(ToolName)))
				.Font(FAppStyle::Get().GetFontStyle("SmallText"))
			]

			// Summary text (hidden initially)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(8, 0, 0, 0)
			[
				SAssignNew(WidgetState.SummaryText, STextBlock)
				.Font(FAppStyle::Get().GetFontStyle("SmallText"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility(EVisibility::Collapsed)
			]
		];

	WidgetState.Status = EOliveToolCallStatus::Running;
	ToolCallWidgetStates.Add(ToolCallId, WidgetState);

	MessagesContainer->AddSlot()
		.AutoHeight()
		.Padding(16, 2, 0, 2)
		[
			Indicator
		];
}

void SOliveAIMessageList::UpdateToolCallStatus(const FString& ToolCallId, bool bSuccess, const FString& ResultSummary)
{
	FOliveToolCallWidgetState* WidgetState = ToolCallWidgetStates.Find(ToolCallId);
	if (!WidgetState)
	{
		return;
	}

	WidgetState->Status = bSuccess ? EOliveToolCallStatus::Completed : EOliveToolCallStatus::Failed;

	// Update icon: checkmark for success, error for failure
	if (WidgetState->StatusIcon.IsValid())
	{
		const FSlateBrush* NewBrush = bSuccess
			? FAppStyle::GetBrush("Icons.Check")
			: FAppStyle::GetBrush("Icons.Error");
		WidgetState->StatusIcon->SetImage(NewBrush);
	}

	// Update status text
	if (WidgetState->StatusText.IsValid())
	{
		FString StatusLabel = bSuccess ? TEXT("Completed") : TEXT("Failed");
		// Extract tool name from existing text and update
		WidgetState->StatusText->SetText(FText::FromString(StatusLabel));
		WidgetState->StatusText->SetColorAndOpacity(
			bSuccess ? FLinearColor(0.2f, 0.8f, 0.4f) : FLinearColor::Red);
	}

	// Show summary
	if (WidgetState->SummaryText.IsValid() && !ResultSummary.IsEmpty())
	{
		WidgetState->SummaryText->SetText(FText::FromString(ResultSummary));
		WidgetState->SummaryText->SetVisibility(EVisibility::Visible);
	}
}

void SOliveAIMessageList::AddConfirmationWidget(const FString& ToolCallId, const FString& Plan, FOnConfirmationAction OnAction)
{
	TSharedPtr<SVerticalBox> ConfirmWidget;

	// Store action delegate as shared ptr so it survives lambda capture
	TSharedRef<FOnConfirmationAction> ActionDelegate = MakeShared<FOnConfirmationAction>(OnAction);

	TSharedRef<SWidget> ConfirmationUI = SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.BorderBackgroundColor(FLinearColor(0.3f, 0.3f, 0.1f, 1.0f))
		.Padding(8.0f)
		[
			SAssignNew(ConfirmWidget, SVerticalBox)

			// Plan description
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Plan))
				.AutoWrapText(true)
			]

			// Buttons
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0, 0, 8, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Confirm", "Confirm"))
					.ButtonColorAndOpacity(FLinearColor(0.2f, 0.6f, 0.2f))
					.OnClicked_Lambda([ActionDelegate, ConfirmWidget]()
					{
						ActionDelegate->ExecuteIfBound(true);
						// Replace buttons with status text
						if (ConfirmWidget.IsValid())
						{
							ConfirmWidget->ClearChildren();
							ConfirmWidget->AddSlot()
								.AutoHeight()
								[
									SNew(STextBlock)
									.Text(LOCTEXT("Confirmed", "Confirmed"))
									.ColorAndOpacity(FLinearColor(0.2f, 0.8f, 0.4f))
								];
						}
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Deny", "Deny"))
					.ButtonColorAndOpacity(FLinearColor(0.6f, 0.2f, 0.2f))
					.OnClicked_Lambda([ActionDelegate, ConfirmWidget]()
					{
						ActionDelegate->ExecuteIfBound(false);
						// Replace buttons with status text
						if (ConfirmWidget.IsValid())
						{
							ConfirmWidget->ClearChildren();
							ConfirmWidget->AddSlot()
								.AutoHeight()
								[
									SNew(STextBlock)
									.Text(LOCTEXT("Denied", "Denied"))
									.ColorAndOpacity(FLinearColor(0.8f, 0.2f, 0.2f))
								];
						}
						return FReply::Handled();
					})
				]
			]
		];

	MessagesContainer->AddSlot()
		.AutoHeight()
		.Padding(16, 4, 0, 4)
		[
			ConfirmationUI
		];

	ScrollToBottom();
}

// ==========================================
// Navigation
// ==========================================

void SOliveAIMessageList::ClearMessages()
{
	Messages.Empty();
	MessagesContainer->ClearChildren();
	ToolCallWidgetStates.Empty();
	StreamingMessageWidget.Reset();
}

void SOliveAIMessageList::ScrollToBottom()
{
	if (ScrollBox.IsValid())
	{
		ScrollBox->ScrollToEnd();
	}
}

// ==========================================
// Private Methods
// ==========================================

TSharedRef<SWidget> SOliveAIMessageList::BuildMessageWidget(TSharedPtr<FOliveUIMessage> Message)
{
	FSlateColor RoleColor = GetRoleColor(Message->Role);
	FText RoleName = GetRoleName(Message->Role);

	// Different styling based on role
	const FSlateBrush* BackgroundBrush = nullptr;
	FMargin ContentPadding(8.0f);

	if (Message->Role == EOliveChatRole::User)
	{
		BackgroundBrush = FAppStyle::GetBrush("ToolPanel.GroupBorder");
	}
	else if (Message->Role == EOliveChatRole::Assistant)
	{
		BackgroundBrush = FAppStyle::GetBrush("ToolPanel.DarkGroupBorder");
	}
	else
	{
		BackgroundBrush = FAppStyle::GetBrush("ToolPanel.GroupBorder");
	}

	return SNew(SBorder)
		.BorderImage(BackgroundBrush)
		.Padding(ContentPadding)
		[
			SNew(SVerticalBox)

			// Header (role name + timestamp)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.Text(RoleName)
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					.ColorAndOpacity(RoleColor)
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNullWidget::NullWidget
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Message->Timestamp.ToString(TEXT("%H:%M"))))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			// Content
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text_Lambda([Message]() { return FText::FromString(Message->Content); })
				.AutoWrapText(true)
			]

			// Streaming indicator
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox)
				.Visibility_Lambda([Message]()
				{
					return Message->bIsStreaming ? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Typing", "..."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		];
}

FSlateColor SOliveAIMessageList::GetRoleColor(EOliveChatRole Role) const
{
	switch (Role)
	{
	case EOliveChatRole::User:
		return FLinearColor(0.2f, 0.6f, 1.0f); // Blue
	case EOliveChatRole::Assistant:
		return FLinearColor(0.2f, 0.8f, 0.4f); // Green
	case EOliveChatRole::System:
		return FLinearColor(0.7f, 0.7f, 0.7f); // Gray
	case EOliveChatRole::Tool:
		return FLinearColor(0.8f, 0.6f, 0.2f); // Orange
	default:
		return FSlateColor::UseForeground();
	}
}

FText SOliveAIMessageList::GetRoleName(EOliveChatRole Role) const
{
	switch (Role)
	{
	case EOliveChatRole::User:
		return LOCTEXT("RoleUser", "You");
	case EOliveChatRole::Assistant:
		return LOCTEXT("RoleAssistant", "Olive");
	case EOliveChatRole::System:
		return LOCTEXT("RoleSystem", "System");
	case EOliveChatRole::Tool:
		return LOCTEXT("RoleTool", "Tool");
	default:
		return FText::GetEmpty();
	}
}

const FSlateBrush* SOliveAIMessageList::GetRoleIcon(EOliveChatRole Role) const
{
	// Use appropriate icons based on role
	switch (Role)
	{
	case EOliveChatRole::User:
		return FAppStyle::GetBrush("Icons.User");
	case EOliveChatRole::Assistant:
		return FAppStyle::GetBrush("Icons.Help");
	default:
		return nullptr;
	}
}

void SOliveAIMessageList::RefreshDisplay()
{
	// Rebuild the last message widget if streaming
	// For simplicity, we're using Text_Lambda which updates automatically
	// In a more complex implementation, we'd rebuild specific widgets
}

// ==========================================
// Result Cards
// ==========================================

void SOliveAIMessageList::AddResultCard(const FString& ToolCallId, const FString& ToolName, const FOliveToolResult& Result)
{
	if (!Result.Data.IsValid())
	{
		return;
	}

	FOliveResultCardData CardData = FOliveResultCardData::FromToolResult(ToolName, Result.Data);
	TSharedRef<SWidget> CardWidget = BuildResultCardWidget(CardData);

	MessagesContainer->AddSlot()
		.AutoHeight()
		.Padding(16, 4, 0, 4)
		[
			CardWidget
		];

	ScrollToBottom();
}

TSharedRef<SWidget> SOliveAIMessageList::BuildResultCardWidget(const FOliveResultCardData& CardData)
{
	switch (CardData.CardType)
	{
	case EOliveResultCardType::BlueprintReadSummary:
		return BuildBlueprintReadCard(CardData);
	case EOliveResultCardType::BlueprintWriteResult:
		return BuildBlueprintWriteCard(CardData);
	case EOliveResultCardType::CompileErrors:
		return BuildCompileErrorsCard(CardData);
	case EOliveResultCardType::SnapshotInfo:
		return BuildSnapshotCard(CardData);
	case EOliveResultCardType::RawJson:
	default:
		return BuildRawJsonCard(CardData);
	}
}

TSharedRef<SWidget> SOliveAIMessageList::BuildBlueprintReadCard(const FOliveResultCardData& CardData)
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.BorderBackgroundColor(FLinearColor(0.15f, 0.18f, 0.22f, 1.0f))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			// Title
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(CardData.Title))
					.TextStyle(FAppStyle::Get(), "NormalText")
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ReadSuccess", "Read"))
					.ColorAndOpacity(FLinearColor(0.2f, 0.8f, 0.4f))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
				]
			]

			// Stats grid
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("VarCount", "Variables: {0}"), FText::AsNumber(CardData.VariableCount)))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("FuncCount", "Functions: {0}"), FText::AsNumber(CardData.FunctionCount)))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("GraphCount", "Graphs: {0}"), FText::AsNumber(CardData.GraphCount)))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("CompCount", "Components: {0}"), FText::AsNumber(CardData.ComponentCount)))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
				]
			]

			// Parent class
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(SBox)
				.Visibility(CardData.ParentClass.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("ParentClass", "Parent: {0}"), FText::FromString(CardData.ParentClass)))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			// Navigation actions
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 4, 0, 0)
			[
				BuildNavigationActions(CardData.Actions)
			]

			// Raw JSON expander
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 4, 0, 0)
			[
				BuildRawJsonExpander(CardData.RawJson)
			]
		];
}

TSharedRef<SWidget> SOliveAIMessageList::BuildBlueprintWriteCard(const FOliveResultCardData& CardData)
{
	FLinearColor BorderColor = CardData.bSuccess
		? FLinearColor(0.1f, 0.3f, 0.1f, 1.0f)
		: FLinearColor(0.3f, 0.1f, 0.1f, 1.0f);

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.BorderBackgroundColor(BorderColor)
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			// Title + status
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(CardData.Title))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock)
					.Text(CardData.bSuccess ? LOCTEXT("WriteOK", "Done") : LOCTEXT("WriteFail", "Failed"))
					.ColorAndOpacity(CardData.bSuccess ? FLinearColor(0.2f, 0.8f, 0.4f) : FLinearColor::Red)
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
				]
			]

			// Operation description
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(STextBlock)
				.Text(FText::FromString(CardData.OperationDescription))
				.AutoWrapText(true)
				.Font(FAppStyle::Get().GetFontStyle("SmallText"))
			]

			// Created item
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(SBox)
				.Visibility(CardData.CreatedItemName.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("Created", "Created: {0}"), FText::FromString(CardData.CreatedItemName)))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					.ColorAndOpacity(FLinearColor(0.2f, 0.8f, 0.4f))
				]
			]

			// Execution time
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 2)
			[
				SNew(SBox)
				.Visibility(CardData.ExecutionTimeMs > 0 ? EVisibility::Visible : EVisibility::Collapsed)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("ExecTime", "Time: {0}ms"), FText::AsNumber(FMath::RoundToInt(CardData.ExecutionTimeMs))))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			// Navigation actions
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[ BuildNavigationActions(CardData.Actions) ]

			// Raw JSON
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[ BuildRawJsonExpander(CardData.RawJson) ]
		];
}

TSharedRef<SWidget> SOliveAIMessageList::BuildCompileErrorsCard(const FOliveResultCardData& CardData)
{
	TSharedPtr<SVerticalBox> ErrorsList;

	TSharedRef<SWidget> Card = SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.BorderBackgroundColor(FLinearColor(0.3f, 0.1f, 0.1f, 1.0f))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			// Header
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("CompileHeader", "Compile: {0} errors, {1} warnings"),
					FText::AsNumber(CardData.Errors.Num()), FText::AsNumber(CardData.Warnings.Num())))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				.ColorAndOpacity(FLinearColor::Red)
			]

			// Errors list
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(ErrorsList, SVerticalBox)
			]

			// Warnings (expandable)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 4, 0, 0)
			[
				SNew(SBox)
				.Visibility(CardData.Warnings.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed)
				[
					SNew(SExpandableArea)
					.HeaderContent()
					[
						SNew(STextBlock)
						.Text(FText::Format(LOCTEXT("WarningsHeader", "{0} Warnings"), FText::AsNumber(CardData.Warnings.Num())))
						.Font(FAppStyle::Get().GetFontStyle("SmallText"))
						.ColorAndOpacity(FLinearColor(0.9f, 0.8f, 0.1f))
					]
					.InitiallyCollapsed(true)
					.BodyContent()
					[
						SNew(SVerticalBox)
						// Warnings will be populated below
					]
				]
			]

			// Navigation actions
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[ BuildNavigationActions(CardData.Actions) ]

			// Raw JSON
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[ BuildRawJsonExpander(CardData.RawJson) ]
		];

	// Populate errors
	if (ErrorsList.IsValid())
	{
		for (const FOliveIRCompileError& Error : CardData.Errors)
		{
			ErrorsList->AddSlot()
				.AutoHeight()
				.Padding(0, 2)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0, 0, 4, 0)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Error"))
						.DesiredSizeOverride(FVector2D(12, 12))
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Error.Message))
						.AutoWrapText(true)
						.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					]
				];
		}
	}

	return Card;
}

TSharedRef<SWidget> SOliveAIMessageList::BuildSnapshotCard(const FOliveResultCardData& CardData)
{
	TSharedPtr<SVerticalBox> AssetsList;

	TSharedRef<SWidget> Card = SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.BorderBackgroundColor(FLinearColor(0.1f, 0.15f, 0.3f, 1.0f))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("SnapHeader", "Snapshot: {0}"), FText::FromString(CardData.SnapshotName)))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("SnapAssets", "{0} assets"), FText::AsNumber(CardData.AssetCount)))
				.Font(FAppStyle::Get().GetFontStyle("SmallText"))
			]

			+ SVerticalBox::Slot().AutoHeight()
			[ SAssignNew(AssetsList, SVerticalBox) ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[ BuildNavigationActions(CardData.Actions) ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[ BuildRawJsonExpander(CardData.RawJson) ]
		];

	// Show up to 5 changed assets
	if (AssetsList.IsValid())
	{
		int32 ShowCount = FMath::Min(CardData.ChangedAssets.Num(), 5);
		for (int32 i = 0; i < ShowCount; i++)
		{
			AssetsList->AddSlot().AutoHeight().Padding(8, 1)
			[
				SNew(STextBlock)
				.Text(FText::FromString(CardData.ChangedAssets[i]))
				.Font(FAppStyle::Get().GetFontStyle("SmallText"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
		}
		if (CardData.ChangedAssets.Num() > 5)
		{
			AssetsList->AddSlot().AutoHeight().Padding(8, 1)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("MoreAssets", "+{0} more"), FText::AsNumber(CardData.ChangedAssets.Num() - 5)))
				.Font(FAppStyle::Get().GetFontStyle("SmallText"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
		}
	}

	return Card;
}

TSharedRef<SWidget> SOliveAIMessageList::BuildRawJsonCard(const FOliveResultCardData& CardData)
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(CardData.Title))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock)
					.Text(FText::FromString(CardData.Subtitle))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					.ColorAndOpacity(CardData.bSuccess ? FLinearColor(0.2f, 0.8f, 0.4f) : FLinearColor::Red)
				]
			]

			+ SVerticalBox::Slot().AutoHeight()
			[ BuildRawJsonExpander(CardData.RawJson) ]
		];
}

TSharedRef<SWidget> SOliveAIMessageList::BuildNavigationActions(const TArray<FOliveNavigationAction>& Actions)
{
	if (Actions.Num() == 0)
	{
		return SNullWidget::NullWidget;
	}

	TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox);

	for (const FOliveNavigationAction& Action : Actions)
	{
		FOliveNavigationAction CapturedAction = Action;
		Box->AddSlot()
			.AutoWidth()
			.Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.OnClicked_Lambda([this, CapturedAction]()
				{
					OnNavigationAction.ExecuteIfBound(CapturedAction);
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(CapturedAction.Label))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					.ColorAndOpacity(FLinearColor(0.3f, 0.6f, 1.0f))
				]
			];
	}

	return Box;
}

TSharedRef<SWidget> SOliveAIMessageList::BuildRawJsonExpander(const TSharedPtr<FJsonObject>& JsonData)
{
	if (!JsonData.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	// Serialize JSON to pretty string
	FString JsonString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonString);
	FJsonSerializer::Serialize(JsonData.ToSharedRef(), Writer);

	TSharedPtr<FString> JsonStringPtr = MakeShared<FString>(JsonString);

	return SNew(SExpandableArea)
		.InitiallyCollapsed(true)
		.HeaderContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ShowRawJson", "Show Raw JSON"))
			.Font(FAppStyle::Get().GetFontStyle("SmallText"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		.BodyContent()
		[
			SNew(SBox)
			.MaxDesiredHeight(200.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.Text(FText::FromString(*JsonStringPtr))
				.Font(FAppStyle::Get().GetFontStyle("SmallText"))
			]
		];
}

// ==========================================
// Run Mode Display
// ==========================================

void SOliveAIMessageList::AddRunHeader(const FOliveRun& Run)
{
	TSharedRef<SWidget> Header = BuildRunHeaderWidget(Run);

	MessagesContainer->AddSlot()
		.AutoHeight()
		.Padding(0, 8, 0, 4)
		[
			Header
		];

	ScrollToBottom();
}

void SOliveAIMessageList::UpdateRunStep(const FOliveRun& Run, int32 StepIndex)
{
	if (StepIndex < 0 || StepIndex >= Run.Steps.Num())
	{
		return;
	}

	const FOliveRunStep& Step = Run.Steps[StepIndex];

	// Check if we already have a widget for this step
	TSharedPtr<SWidget>* ExistingWidget = RunStepWidgets.Find(StepIndex);
	if (!ExistingWidget || !ExistingWidget->IsValid())
	{
		// Create new step widget and add to container
		TSharedRef<SWidget> StepWidget = BuildRunStepWidget(Step, StepIndex);
		RunStepWidgets.Add(StepIndex, StepWidget);

		if (RunStepsContainer.IsValid())
		{
			RunStepsContainer->AddSlot()
				.AutoHeight()
				[
					StepWidget
				];
		}

		ScrollToBottom();
	}
	// For existing widgets, the lambda bindings will pick up state changes
}

void SOliveAIMessageList::UpdateRunStatus(const FOliveRun& Run)
{
	// The run controls widget uses lambdas that check current state,
	// so the UI updates automatically via Slate's invalidation
}

TSharedRef<SWidget> SOliveAIMessageList::BuildRunHeaderWidget(const FOliveRun& Run)
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.BorderBackgroundColor(FLinearColor(0.12f, 0.14f, 0.18f, 1.0f))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			// Header row
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("RunHeader", "Run: {0}"), FText::FromString(Run.Name)))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Run.StartTime.ToString(TEXT("%H:%M:%S"))))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			// Steps container
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(RunStepsContainer, SVerticalBox)
			]

			// Controls
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 4, 0, 0)
			[
				BuildRunControlsWidget(Run)
			]
		];
}

TSharedRef<SWidget> SOliveAIMessageList::BuildRunStepWidget(const FOliveRunStep& Step, int32 StepIndex)
{
	// Determine status icon and color
	const FSlateBrush* StatusBrush = nullptr;
	FLinearColor StatusColor = FLinearColor::Gray;

	switch (Step.Status)
	{
	case EOliveRunStepStatus::Pending:
		StatusBrush = FAppStyle::GetBrush("Icons.FilledCircle");
		StatusColor = FLinearColor(0.5f, 0.5f, 0.5f);
		break;
	case EOliveRunStepStatus::Running:
		StatusBrush = FAppStyle::GetBrush("Icons.Refresh");
		StatusColor = FLinearColor::Yellow;
		break;
	case EOliveRunStepStatus::Completed:
		StatusBrush = FAppStyle::GetBrush("Icons.Check");
		StatusColor = FLinearColor(0.2f, 0.8f, 0.4f);
		break;
	case EOliveRunStepStatus::Failed:
		StatusBrush = FAppStyle::GetBrush("Icons.Error");
		StatusColor = FLinearColor::Red;
		break;
	case EOliveRunStepStatus::Skipped:
		StatusBrush = FAppStyle::GetBrush("Icons.FilledCircle");
		StatusColor = FLinearColor(0.6f, 0.6f, 0.3f);
		break;
	case EOliveRunStepStatus::RolledBack:
		StatusBrush = FAppStyle::GetBrush("Icons.FilledCircle");
		StatusColor = FLinearColor(0.5f, 0.3f, 0.8f);
		break;
	}

	TSharedRef<SWidget> StepWidget = SNew(SHorizontalBox)

		// Status icon
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(24, 2, 4, 2)
		[
			SNew(SImage)
			.Image(StatusBrush)
			.ColorAndOpacity(StatusColor)
			.DesiredSizeOverride(FVector2D(12, 12))
		]

		// Description
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("StepDesc", "Step {0}: {1}"),
				FText::AsNumber(StepIndex + 1), FText::FromString(Step.Description)))
			.Font(FAppStyle::Get().GetFontStyle("SmallText"))
		]

		// Tool call count
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4, 0)
		[
			SNew(SBox)
			.Visibility(Step.ToolCalls.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("ToolCount", "{0} tools"), FText::AsNumber(Step.ToolCalls.Num())))
				.Font(FAppStyle::Get().GetFontStyle("SmallText"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]

		// Retry button (only for failed steps)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4, 0)
		[
			SNew(SBox)
			.Visibility(Step.Status == EOliveRunStepStatus::Failed ? EVisibility::Visible : EVisibility::Collapsed)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.OnClicked_Lambda([this, StepIndex]()
				{
					OnRunRetryStep.ExecuteIfBound(StepIndex);
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Retry", "Retry"))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					.ColorAndOpacity(FLinearColor(0.3f, 0.6f, 1.0f))
				]
			]
		]

		// Skip button (only for failed steps)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4, 0)
		[
			SNew(SBox)
			.Visibility(Step.Status == EOliveRunStepStatus::Failed ? EVisibility::Visible : EVisibility::Collapsed)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.OnClicked_Lambda([this, StepIndex]()
				{
					OnRunSkipStep.ExecuteIfBound(StepIndex);
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Skip", "Skip"))
					.Font(FAppStyle::Get().GetFontStyle("SmallText"))
					.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.3f))
				]
			]
		];

	return StepWidget;
}

TSharedRef<SWidget> SOliveAIMessageList::BuildRunControlsWidget(const FOliveRun& Run)
{
	return SNew(SHorizontalBox)

		// Pause button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0, 0, 4, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("PauseRun", "Pause"))
			.OnClicked_Lambda([this]()
			{
				OnRunPause.ExecuteIfBound();
				return FReply::Handled();
			})
		]

		// Resume button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0, 0, 4, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("ResumeRun", "Resume"))
			.OnClicked_Lambda([this]()
			{
				OnRunResume.ExecuteIfBound();
				return FReply::Handled();
			})
		]

		// Cancel button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("CancelRun", "Cancel"))
			.ButtonColorAndOpacity(FLinearColor(0.6f, 0.2f, 0.2f))
			.OnClicked_Lambda([this]()
			{
				OnRunCancel.ExecuteIfBound();
				return FReply::Handled();
			})
		];
}

#undef LOCTEXT_NAMESPACE
