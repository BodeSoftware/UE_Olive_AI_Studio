// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Chat/OliveConversationManager.h"
#include "Chat/OliveRunManager.h"

class SOliveAIMessageList;
class SOliveAIContextBar;
class SOliveAIInputField;
class SMultiLineEditableTextBox;
class SEditableTextBox;
class SComboButton;
template<typename> class SComboBox;
struct FOliveNavigationAction;
enum class EOliveAIProvider : uint8;

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

	/** Build the safety preset toggle */
	TSharedRef<SWidget> BuildSafetyPresetToggle();

	/** Build provider selector */
	TSharedRef<SWidget> BuildProviderSelector();

	/** Build model selector */
	TSharedRef<SWidget> BuildModelSelector();

	// ==========================================
	// Event Handlers
	// ==========================================

	/** Handle send message button click */
	FReply OnSendMessageClicked();

	/** Handle message submitted from input field */
	void OnMessageSubmitted(const FString& Message);

	/** Handle focus profile changed (from menu selection) */
	void OnFocusProfileSelected(const FString& ProfileName);

	/** Build the focus profile menu content (primary + advanced sections) */
	TSharedRef<SWidget> BuildFocusProfileMenuContent();

	/** Handle settings button clicked */
	FReply OnSettingsClicked();

	/** Handle new chat button clicked */
	FReply OnNewChatClicked();

	/** Handle safety preset selection changed */
	void OnSafetyPresetChanged(TSharedPtr<FString> NewPreset, ESelectInfo::Type SelectInfo);

	/** Handle provider selection changed */
	void OnProviderChanged(TSharedPtr<FString> NewProvider, ESelectInfo::Type SelectInfo);

	/** Handle model suggestion selection */
	void OnModelSuggestionSelected(TSharedPtr<FString> NewModel, ESelectInfo::Type SelectInfo);

	/** Handle model text committed */
	void OnModelCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/** Get color for current safety preset */
	FSlateColor GetSafetyPresetColor() const;

	/** Refresh available provider list */
	void RefreshProviderOptions();

	/** Refresh model suggestions for provider */
	void RefreshModelOptionsForProvider(EOliveAIProvider ProviderType);

	/** Apply provider/model selection and reconfigure */
	void ApplyProviderAndModelSelection(EOliveAIProvider ProviderType, const FString& ModelId, bool bSaveConfig);

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
	void HandleConfirmationRequired(const FString& ToolCallId, const FString& ToolName, const FString& Plan);

	// ==========================================
	// Message Queue Callbacks
	// ==========================================

	/** Handle a message being enqueued (update queue depth display) */
	void HandleMessageQueued(int32 QueueDepth);

	/** Handle the queue becoming empty after draining */
	void HandleQueueDrained();

	// ==========================================
	// Deferred Profile Callbacks
	// ==========================================

	/** Handle a deferred focus profile being applied after processing completes */
	void HandleDeferredProfileApplied(const FString& ProfileName);

	// ==========================================
	// Retry Manager Callbacks
	// ==========================================

	/** Handle a retry being scheduled (update countdown display) */
	void HandleRetryScheduled(int32 Attempt, int32 MaxAttempts, float DelaySeconds);

	/** Handle countdown tick during retry wait */
	void HandleRetryCountdownTick(float SecondsRemaining);

	/** Handle a retry attempt starting */
	void HandleRetryAttemptStarted();

	// ==========================================
	// Context Updates
	// ==========================================

	/** Handle context asset removed */
	void OnContextAssetRemoved(const FString& AssetPath);

	/** Handle asset opened in editor (auto-context) */
	void OnAssetOpened(UObject* Asset);

	/** Update context bar with current selection */
	void UpdateContextFromSelection();

	/** Subscribe to editor events for auto-context */
	void SubscribeToEditorEvents();

	/** Unsubscribe from editor events */
	void UnsubscribeFromEditorEvents();

	/** Debounced version of UpdateContextFromSelection */
	void UpdateContextFromSelectionDebounced();

	/** Handle run status changes */
	void HandleRunStatusChanged(const FOliveRun& Run);

	/** Handle run step changes */
	void HandleRunStepChanged(const FOliveRun& Run, int32 StepIndex);

	/** Handle navigation actions from result cards */
	void HandleNavigationAction(const FOliveNavigationAction& Action);

	// ==========================================
	// Status
	// ==========================================

	/** Get status text */
	FText GetStatusText() const;

	/** Get provider status color */
	FSlateColor GetStatusColor() const;

	/** Is the send button enabled */
	bool IsSendEnabled() const;

	/** Refresh provider selection/config from current plugin settings */
	bool ConfigureProviderFromSettings(FString& OutError);

	// ==========================================
	// State
	// ==========================================

	/**
	 * Conversation manager -- borrowed from FOliveEditorChatSession.
	 * The session singleton owns the lifecycle; we hold a shared reference
	 * for convenient access. The panel MUST NOT reset or destroy this pointer.
	 */
	TSharedPtr<FOliveConversationManager> ConversationManager;

	/** Focus profile options */
	TArray<TSharedPtr<FString>> FocusProfiles;

	/** Currently selected profile */
	TSharedPtr<FString> CurrentFocusProfile;

	/** Safety preset options */
	TArray<TSharedPtr<FString>> SafetyPresetOptions;
	TSharedPtr<FString> CurrentSafetyPreset;

	/** Provider options */
	TArray<TSharedPtr<FString>> ProviderOptions;
	TSharedPtr<FString> CurrentProviderOption;

	/** Model options */
	TArray<TSharedPtr<FString>> ModelOptions;
	TSharedPtr<FString> CurrentModelOption;

	// ==========================================
	// Child Widgets
	// ==========================================

	TSharedPtr<SOliveAIMessageList> MessageList;
	TSharedPtr<SOliveAIContextBar> ContextBar;
	TSharedPtr<SOliveAIInputField> InputField;
	TSharedPtr<SComboButton> FocusDropdown;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ProviderComboBox;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ModelComboBox;
	TSharedPtr<SEditableTextBox> ModelTextBox;

	/** Editor event handles */
	FDelegateHandle OnAssetEditorOpenedHandle;
	FDelegateHandle OnEditorSelectionChangedHandle;
	FTimerHandle SelectionDebounceTimer;
	TWeakObjectPtr<UObject> LastActiveAsset;

	// ==========================================
	// State Flags
	// ==========================================

	bool bIsProcessing = false;
	FString CurrentErrorMessage;

	/** Current message queue depth (updated via HandleMessageQueued/HandleQueueDrained) */
	int32 QueuedMessageCount = 0;

	/** Whether a retry is currently pending (waiting for backoff/rate-limit timer) */
	bool bIsRetryPending = false;

	/** Current retry attempt number (1-based) */
	int32 RetryAttempt = 0;

	/** Max retry attempts for the current retry sequence */
	int32 RetryMaxAttempts = 0;

	/** Seconds remaining until next retry attempt */
	float RetryCountdownSeconds = 0.0f;

	/** Whether the current retry is due to rate limiting (changes display text) */
	bool bIsRateLimited = false;

	/** Warning message shown when a focus profile switch is deferred (empty = no warning) */
	FString DeferredProfileWarning;

	/** Whether a connection validation is in progress */
	bool bIsValidating = false;

	/** Last validation result message */
	FString ValidationMessage;

	/** Whether last validation succeeded */
	bool bValidationSuccess = false;
};
