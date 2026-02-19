// Copyright Bode Software. All Rights Reserved.

#include "UI/SOliveAIMessageList.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "OliveAIMessageList"

void SOliveAIMessageList::Construct(const FArguments& InArgs)
{
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
	// Create a tool indicator widget
	TSharedPtr<SHorizontalBox> ToolWidget;

	TSharedRef<SWidget> Indicator = SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SAssignNew(ToolWidget, SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 4, 0)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Refresh"))
				.DesiredSizeOverride(FVector2D(12, 12))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("ToolCalling", "Calling {0}..."), FText::FromString(ToolName)))
				.TextStyle(FAppStyle::Get(), "SmallText")
			]
		];

	ToolCallWidgets.Add(ToolCallId, Indicator);

	MessagesContainer->AddSlot()
		.AutoHeight()
		.Padding(16, 2, 0, 2)
		[
			Indicator
		];
}

void SOliveAIMessageList::UpdateToolCallStatus(const FString& ToolCallId, bool bSuccess, const FString& ResultSummary)
{
	// For now, tool status is shown through the messages
	// Future: Update the indicator widget directly
}

// ==========================================
// Navigation
// ==========================================

void SOliveAIMessageList::ClearMessages()
{
	Messages.Empty();
	MessagesContainer->ClearChildren();
	ToolCallWidgets.Empty();
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
					.TextStyle(FAppStyle::Get(), "SmallText")
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
					.TextStyle(FAppStyle::Get(), "SmallText")
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

#undef LOCTEXT_NAMESPACE
