// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Chat/OliveConversationManager.h"

class SOliveAIMessageList;
class SOliveAIContextBar;
class SOliveAIInputField;
class SMultiLineEditableTextBox;
template<typename> class SComboBox;

/**
 * Olive AI Chat Panel
 *
 * Main chat interface panel that docks into the Unreal Editor.
 * Contains message list, context bar, input field, and configuration.
 */
class OLIVEAIEDITOR_API SOliveAIChatPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOliveAIChatPanel) {}
	SLATE_END_ARGS()

	/** Tab ID for nomad tab spawner */
	static const FName TabId;

	/** Spawn the tab with this panel */
	static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);

	/** Construct the widget */
	void Construct(const FArguments& InArgs);

	/** Destructor */
	virtual ~SOliveAIChatPanel();

private:
	// ==========================================
	// Widget Construction
	// ==========================================

	/** Build the header area (title, settings button) */
	TSharedRef<SWidget> BuildHeader();

	/** Build the context bar */
	TSharedRef<SWidget> BuildContextBar();

	/** Build the message area */
	TSharedRef<SWidget> BuildMessageArea();

	/** Build the input area */
	TSharedRef<SWidget> BuildInputArea();

	/** Build the status bar */
	TSharedRef<SWidget> BuildStatusBar();

	/** Build the focus profile dropdown */
	TSharedRef<SWidget> BuildFocusDropdown();

	// ==========================================
	// Event Handlers
	// ==========================================

	/** Handle send message button click */
	FReply OnSendMessageClicked();

	/** Handle message submitted from input field */
	void OnMessageSubmitted(const FString& Message);

	/** Handle focus profile changed */
	void OnFocusProfileChanged(TSharedPtr<FString> NewProfile, ESelectInfo::Type SelectInfo);

	/** Handle settings button clicked */
	FReply OnSettingsClicked();

	/** Handle new chat button clicked */
	FReply OnNewChatClicked();

	// ==========================================
	// Conversation Manager Callbacks
	// ==========================================

	void HandleMessageAdded(const FOliveChatMessage& Message);
	void HandleStreamChunk(const FString& Chunk);
	void HandleToolCallStarted(const FString& ToolName, const FString& ToolCallId);
	void HandleToolCallCompleted(const FString& ToolName, const FString& ToolCallId, const FOliveToolResult& Result);
	void HandleProcessingStarted();
	void HandleProcessingComplete();
	void HandleError(const FString& ErrorMessage);

	// ==========================================
	// Context Updates
	// ==========================================

	/** Handle context asset removed */
	void OnContextAssetRemoved(const FString& AssetPath);

	/** Handle asset opened in editor (auto-context) */
	void OnAssetOpened(UObject* Asset);

	/** Update context bar with current selection */
	void UpdateContextFromSelection();

	// ==========================================
	// Status
	// ==========================================

	/** Get status text */
	FText GetStatusText() const;

	/** Get provider status color */
	FSlateColor GetStatusColor() const;

	/** Is the send button enabled */
	bool IsSendEnabled() const;

	// ==========================================
	// State
	// ==========================================

	/** Conversation manager */
	TSharedPtr<FOliveConversationManager> ConversationManager;

	/** Focus profile options */
	TArray<TSharedPtr<FString>> FocusProfiles;

	/** Currently selected profile */
	TSharedPtr<FString> CurrentFocusProfile;

	// ==========================================
	// Child Widgets
	// ==========================================

	TSharedPtr<SOliveAIMessageList> MessageList;
	TSharedPtr<SOliveAIContextBar> ContextBar;
	TSharedPtr<SOliveAIInputField> InputField;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> FocusDropdown;

	// ==========================================
	// State Flags
	// ==========================================

	bool bIsProcessing = false;
	FString CurrentErrorMessage;
};
