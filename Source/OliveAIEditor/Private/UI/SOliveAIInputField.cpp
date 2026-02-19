// Copyright Bode Software. All Rights Reserved.

#include "UI/SOliveAIInputField.h"
#include "Index/OliveProjectIndex.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "OliveAIInputField"

void SOliveAIInputField::Construct(const FArguments& InArgs)
{
	OnMessageSubmit = InArgs._OnMessageSubmit;
	OnAssetMentioned = InArgs._OnAssetMentioned;

	ChildSlot
	[
		SNew(SVerticalBox)

		// Main input area
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(4.0f)
			[
				SAssignNew(MentionAnchor, SMenuAnchor)
				.Placement(MenuPlacement_AboveAnchor)
				.OnGetMenuContent(this, &SOliveAIInputField::CreateMentionPopup)
				[
					SAssignNew(TextBox, SMultiLineEditableTextBox)
					.HintText(LOCTEXT("Placeholder", "Type a message... (use @ to mention assets)"))
					.OnTextChanged(this, &SOliveAIInputField::OnTextChanged)
					.OnTextCommitted(this, &SOliveAIInputField::OnTextCommitted)
					.OnKeyDownHandler(this, &SOliveAIInputField::OnInputKeyDown)
					.AutoWrapText(true)
					.AllowMultiLine(true)
					.ModiferKeyForNewLine(EModifierKey::Shift)
				]
			]
		]
	];
}

TSharedRef<SWidget> SOliveAIInputField::CreateMentionPopup()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Menu.Background"))
		.Padding(4.0f)
		[
			SNew(SBox)
			.MaxDesiredWidth(400.0f)
			.MaxDesiredHeight(300.0f)
			[
				SAssignNew(MentionList, SListView<TSharedPtr<FOliveAssetInfo>>)
				.ListItemsSource(&MentionResults)
				.OnGenerateRow(this, &SOliveAIInputField::GenerateMentionRow)
				.OnSelectionChanged(this, &SOliveAIInputField::OnMentionSelectionChanged)
				.OnMouseButtonDoubleClick(this, &SOliveAIInputField::OnMentionItemDoubleClicked)
				.SelectionMode(ESelectionMode::Single)
			]
		];
}

// ==========================================
// Control
// ==========================================

void SOliveAIInputField::SetEnabled(bool bEnabled)
{
	bInputEnabled = bEnabled;
	if (TextBox.IsValid())
	{
		TextBox->SetEnabled(bEnabled);
	}
}

void SOliveAIInputField::Clear()
{
	if (TextBox.IsValid())
	{
		TextBox->SetText(FText::GetEmpty());
	}
}

void SOliveAIInputField::Focus()
{
	if (TextBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(TextBox, EFocusCause::SetDirectly);
	}
}

FString SOliveAIInputField::GetText() const
{
	if (TextBox.IsValid())
	{
		return TextBox->GetText().ToString();
	}
	return TEXT("");
}

void SOliveAIInputField::SetPlaceholderText(const FText& Text)
{
	if (TextBox.IsValid())
	{
		TextBox->SetHintText(Text);
	}
}

// ==========================================
// Input Handling
// ==========================================

void SOliveAIInputField::OnTextChanged(const FText& NewText)
{
	if (!bInputEnabled)
	{
		return;
	}

	FString Text = NewText.ToString();
	int32 CursorPosition = Text.Len(); // Simplified - actual cursor position would need more work

	CheckForMentionTrigger(Text, CursorPosition);
}

void SOliveAIInputField::OnTextCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	// Hide mention popup on commit
	HideMentionPopup();

	if (CommitType == ETextCommit::OnEnter && !NewText.IsEmpty())
	{
		SubmitMessage();
	}
}

FReply SOliveAIInputField::OnInputKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// Handle mention popup navigation
	if (bMentionPopupVisible)
	{
		if (InKeyEvent.GetKey() == EKeys::Up)
		{
			NavigateMention(-1);
			return FReply::Handled();
		}
		else if (InKeyEvent.GetKey() == EKeys::Down)
		{
			NavigateMention(1);
			return FReply::Handled();
		}
		else if (InKeyEvent.GetKey() == EKeys::Tab || InKeyEvent.GetKey() == EKeys::Enter)
		{
			ConfirmMentionSelection();
			return FReply::Handled();
		}
		else if (InKeyEvent.GetKey() == EKeys::Escape)
		{
			HideMentionPopup();
			return FReply::Handled();
		}
	}

	// Enter without shift submits
	if (InKeyEvent.GetKey() == EKeys::Enter && !InKeyEvent.IsShiftDown())
	{
		SubmitMessage();
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

void SOliveAIInputField::SubmitMessage()
{
	if (!bInputEnabled)
	{
		return;
	}

	FString Text = GetText();
	if (!Text.IsEmpty())
	{
		OnMessageSubmit.ExecuteIfBound(Text);
	}
}

// ==========================================
// @Mention Handling
// ==========================================

void SOliveAIInputField::CheckForMentionTrigger(const FString& Text, int32 CursorPosition)
{
	// Find the last @ before cursor
	int32 AtPosition = INDEX_NONE;
	Text.FindLastChar(TEXT('@'), AtPosition);

	if (AtPosition == INDEX_NONE)
	{
		HideMentionPopup();
		return;
	}

	// Check if there's a space after @, which means mention is complete
	int32 SpaceAfterAt = Text.Find(TEXT(" "), ESearchCase::IgnoreCase, ESearchDir::FromStart, AtPosition);
	if (SpaceAfterAt != INDEX_NONE && SpaceAfterAt < CursorPosition)
	{
		HideMentionPopup();
		return;
	}

	// Get search query (text after @)
	FString Query = Text.RightChop(AtPosition + 1);

	if (Query.Len() < 1)
	{
		HideMentionPopup();
		return;
	}

	MentionStartPosition = AtPosition;
	ShowMentionPopup(Query);
}

void SOliveAIInputField::ShowMentionPopup(const FString& SearchQuery)
{
	// Search for assets
	TArray<FOliveAssetInfo> Results = FOliveProjectIndex::Get().SearchAssets(SearchQuery, 10);

	MentionResults.Empty();
	for (const FOliveAssetInfo& Info : Results)
	{
		MentionResults.Add(MakeShared<FOliveAssetInfo>(Info));
	}

	if (MentionResults.Num() > 0)
	{
		bMentionPopupVisible = true;

		if (MentionList.IsValid())
		{
			MentionList->RequestListRefresh();
			if (MentionResults.Num() > 0)
			{
				MentionList->SetSelection(MentionResults[0], ESelectInfo::Direct);
			}
		}

		if (MentionAnchor.IsValid())
		{
			MentionAnchor->SetIsOpen(true);
		}
	}
	else
	{
		HideMentionPopup();
	}
}

void SOliveAIInputField::HideMentionPopup()
{
	bMentionPopupVisible = false;
	MentionStartPosition = INDEX_NONE;

	if (MentionAnchor.IsValid())
	{
		MentionAnchor->SetIsOpen(false);
	}
}

void SOliveAIInputField::OnMentionSelected(TSharedPtr<FOliveAssetInfo> AssetInfo)
{
	if (!AssetInfo.IsValid() || !TextBox.IsValid())
	{
		return;
	}

	// Get current text
	FString Text = TextBox->GetText().ToString();

	if (MentionStartPosition != INDEX_NONE && MentionStartPosition < Text.Len())
	{
		// Replace @query with @AssetName
		FString Before = Text.Left(MentionStartPosition);
		FString After = TEXT(" "); // Space after mention

		FString NewText = Before + TEXT("@") + AssetInfo->Name + After;
		TextBox->SetText(FText::FromString(NewText));

		// Notify about the mentioned asset
		OnAssetMentioned.ExecuteIfBound(AssetInfo->Path);
	}

	HideMentionPopup();
}

void SOliveAIInputField::OnMentionSelectionChanged(TSharedPtr<FOliveAssetInfo> AssetInfo, ESelectInfo::Type SelectInfo)
{
	// Selection changed, no action needed
}

void SOliveAIInputField::OnMentionItemDoubleClicked(TSharedPtr<FOliveAssetInfo> AssetInfo)
{
	OnMentionSelected(AssetInfo);
}

TSharedRef<ITableRow> SOliveAIInputField::GenerateMentionRow(TSharedPtr<FOliveAssetInfo> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<TSharedPtr<FOliveAssetInfo>>, OwnerTable)
		.Padding(FMargin(4, 2))
		[
			SNew(SHorizontalBox)

			// Asset icon
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 8, 0)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Blueprint"))
				.DesiredSizeOverride(FVector2D(16, 16))
			]

			// Asset name and path
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->Name))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->Path))
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			// Asset type
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->AssetClass.ToString()))
				.TextStyle(FAppStyle::Get(), "SmallText")
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

void SOliveAIInputField::NavigateMention(int32 Direction)
{
	if (!MentionList.IsValid() || MentionResults.Num() == 0)
	{
		return;
	}

	TArray<TSharedPtr<FOliveAssetInfo>> Selected = MentionList->GetSelectedItems();
	int32 CurrentIndex = 0;

	if (Selected.Num() > 0)
	{
		CurrentIndex = MentionResults.IndexOfByKey(Selected[0]);
	}

	int32 NewIndex = FMath::Clamp(CurrentIndex + Direction, 0, MentionResults.Num() - 1);
	MentionList->SetSelection(MentionResults[NewIndex], ESelectInfo::Direct);
	MentionList->RequestScrollIntoView(MentionResults[NewIndex]);
}

void SOliveAIInputField::ConfirmMentionSelection()
{
	if (!MentionList.IsValid())
	{
		return;
	}

	TArray<TSharedPtr<FOliveAssetInfo>> Selected = MentionList->GetSelectedItems();
	if (Selected.Num() > 0)
	{
		OnMentionSelected(Selected[0]);
	}
}

#undef LOCTEXT_NAMESPACE
