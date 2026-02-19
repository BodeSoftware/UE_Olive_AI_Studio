// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Views/SListView.h"

class SMultiLineEditableTextBox;
class SMenuAnchor;
struct FOliveAssetInfo;

/**
 * Message Submit Delegate
 */
DECLARE_DELEGATE_OneParam(FOnOliveMessageSubmit, const FString& /* Message */);

/**
 * Asset Mentioned Delegate
 */
DECLARE_DELEGATE_OneParam(FOnOliveAssetMentioned, const FString& /* AssetPath */);

/**
 * Input Field Widget
 *
 * Multi-line text input with @mention autocomplete for assets.
 * Supports Enter to send, Shift+Enter for newline.
 */
class OLIVEAIEDITOR_API SOliveAIInputField : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOliveAIInputField) {}
		SLATE_EVENT(FOnOliveMessageSubmit, OnMessageSubmit)
		SLATE_EVENT(FOnOliveAssetMentioned, OnAssetMentioned)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// ==========================================
	// Control
	// ==========================================

	/** Enable or disable input */
	void SetEnabled(bool bEnabled);

	/** Check if enabled */
	bool IsInputEnabled() const { return bInputEnabled; }

	/** Clear the input */
	void Clear();

	/** Focus the input field */
	void Focus();

	/** Get current text */
	FString GetText() const;

	/** Set placeholder text */
	void SetPlaceholderText(const FText& Text);

private:
	// ==========================================
	// Input Handling
	// ==========================================

	/** Handle text changed */
	void OnTextChanged(const FText& NewText);

	/** Handle text committed */
	void OnTextCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/** Handle key down */
	FReply OnInputKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);

	/** Submit the current message */
	void SubmitMessage();

	// ==========================================
	// @Mention Handling
	// ==========================================

	/** Create the mention popup widget */
	TSharedRef<SWidget> CreateMentionPopup();

	/** Check for @mention trigger */
	void CheckForMentionTrigger(const FString& Text, int32 CursorPosition);

	/** Show the mention popup */
	void ShowMentionPopup(const FString& SearchQuery);

	/** Hide the mention popup */
	void HideMentionPopup();

	/** Handle mention selected */
	void OnMentionSelected(TSharedPtr<FOliveAssetInfo> AssetInfo);

	/** Handle mention list selection */
	void OnMentionSelectionChanged(TSharedPtr<FOliveAssetInfo> AssetInfo, ESelectInfo::Type SelectInfo);

	/** Handle mention item double clicked */
	void OnMentionItemDoubleClicked(TSharedPtr<FOliveAssetInfo> AssetInfo);

	/** Generate mention list row */
	TSharedRef<ITableRow> GenerateMentionRow(TSharedPtr<FOliveAssetInfo> Item, const TSharedRef<STableViewBase>& OwnerTable);

	/** Navigate mention selection */
	void NavigateMention(int32 Direction);

	/** Confirm current mention selection */
	void ConfirmMentionSelection();

	// ==========================================
	// State
	// ==========================================

	TSharedPtr<SMultiLineEditableTextBox> TextBox;
	TSharedPtr<SMenuAnchor> MentionAnchor;
	TSharedPtr<SListView<TSharedPtr<FOliveAssetInfo>>> MentionList;

	TArray<TSharedPtr<FOliveAssetInfo>> MentionResults;
	int32 MentionStartPosition = INDEX_NONE;
	bool bMentionPopupVisible = false;
	bool bInputEnabled = true;

	FOnOliveMessageSubmit OnMessageSubmit;
	FOnOliveAssetMentioned OnAssetMentioned;
};
