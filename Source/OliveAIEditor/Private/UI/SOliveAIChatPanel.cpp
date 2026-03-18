// Copyright Bode Software. All Rights Reserved.

#include "UI/SOliveAIChatPanel.h"
#include "Async/Async.h"
#include "UI/SOliveAIMessageList.h"
#include "UI/SOliveAIContextBar.h"
#include "UI/SOliveAIInputField.h"
#include "Chat/OliveConversationManager.h"
#include "Chat/OliveEditorChatSession.h"
#include "Providers/IOliveAIProvider.h"
#include "Settings/OliveAISettings.h"
#include "OliveAIEditorModule.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "ISettingsModule.h"
#include "UI/OliveResultCards.h"
#include "OliveBlueprintNavigator.h"
#include "OliveSnapshotManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Selection.h"

#define LOCTEXT_NAMESPACE "OliveAIChatPanel"

namespace
{
FString ProviderDisplayName(EOliveAIProvider ProviderType)
{
	switch (ProviderType)
	{
	case EOliveAIProvider::ClaudeCode:
		return TEXT("Claude Code (Local)");
	case EOliveAIProvider::Codex:
		return TEXT("Codex (Local)");
	case EOliveAIProvider::OpenRouter:
		return TEXT("OpenRouter");
	case EOliveAIProvider::ZAI:
		return TEXT("Z.ai");
	case EOliveAIProvider::Anthropic:
		return TEXT("Anthropic");
	case EOliveAIProvider::OpenAI:
		return TEXT("OpenAI");
	case EOliveAIProvider::Google:
		return TEXT("Google");
	case EOliveAIProvider::Ollama:
		return TEXT("Ollama");
	case EOliveAIProvider::OpenAICompatible:
		return TEXT("OpenAI Compatible");
	default:
		return TEXT("OpenRouter");
	}
}

EOliveAIProvider ProviderFromDisplayName(const FString& ProviderName)
{
	if (ProviderName.Equals(TEXT("Claude Code CLI"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Claude Code (Local)"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("claudecode"), ESearchCase::IgnoreCase))
	{
		return EOliveAIProvider::ClaudeCode;
	}
	if (ProviderName.Equals(TEXT("Codex CLI"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Codex (Local)"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("codex"), ESearchCase::IgnoreCase))
	{
		return EOliveAIProvider::Codex;
	}
	if (ProviderName.Equals(TEXT("OpenRouter"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("openrouter"), ESearchCase::IgnoreCase))
	{
		return EOliveAIProvider::OpenRouter;
	}
	if (ProviderName.Equals(TEXT("Z.ai"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("z.ai"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("zai"), ESearchCase::IgnoreCase))
	{
		return EOliveAIProvider::ZAI;
	}
	if (ProviderName.Equals(TEXT("Anthropic"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("anthropic"), ESearchCase::IgnoreCase))
	{
		return EOliveAIProvider::Anthropic;
	}
	if (ProviderName.Equals(TEXT("OpenAI"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("openai"), ESearchCase::IgnoreCase))
	{
		return EOliveAIProvider::OpenAI;
	}
	if (ProviderName.Equals(TEXT("Google"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("google"), ESearchCase::IgnoreCase))
	{
		return EOliveAIProvider::Google;
	}
	if (ProviderName.Equals(TEXT("Ollama"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("ollama"), ESearchCase::IgnoreCase))
	{
		return EOliveAIProvider::Ollama;
	}
	if (ProviderName.Equals(TEXT("OpenAI Compatible"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("openai_compatible"), ESearchCase::IgnoreCase))
	{
		return EOliveAIProvider::OpenAICompatible;
	}

	return EOliveAIProvider::OpenRouter;
}

TSharedPtr<FString> FindOptionByValue(TArray<TSharedPtr<FString>>& Options, const FString& Value)
{
	for (const TSharedPtr<FString>& Option : Options)
	{
		if (Option.IsValid() && Option->Equals(Value, ESearchCase::CaseSensitive))
		{
			return Option;
		}
	}
	return nullptr;
}
} // namespace

const FName SOliveAIChatPanel::TabId(TEXT("OliveAIChatPanel"));

TSharedRef<SDockTab> SOliveAIChatPanel::SpawnTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(LOCTEXT("TabTitle", "Olive AI Chat"))
		[
			SNew(SOliveAIChatPanel)
		];
}

void SOliveAIChatPanel::Construct(const FArguments& InArgs)
{
	// Borrow ConversationManager from the editor-lifetime session singleton.
	// The session owns the lifecycle; the panel holds a shared reference.
	FOliveEditorChatSession& Session = FOliveEditorChatSession::Get();
	Session.NotifyPanelOpened();
	ConversationManager = Session.GetConversationManager();

	if (!ConversationManager.IsValid())
	{
		UE_LOG(LogOliveAI, Error, TEXT("SOliveAIChatPanel: ConversationManager is null -- session may not be initialized"));
	}

	// Check for background completion (operation finished while panel was closed)
	if (Session.HasBackgroundCompletion())
	{
		// The toast already showed; consume the flag so it does not persist
		const FString Summary = Session.GetBackgroundCompletionSummary();
		Session.ConsumeBackgroundCompletion();
		UE_LOG(LogOliveAI, Log, TEXT("Panel reopened with background completion: %s"), *Summary);
	}

	// Chat mode is initialized from settings by ConversationManager::StartNewSession()
	// (no focus profile or safety preset initialization needed)

	RefreshProviderOptions();
	if (UOliveAISettings* CurrentSettings = UOliveAISettings::Get())
	{
		const FString SelectedProviderName = ProviderDisplayName(CurrentSettings->Provider);
		CurrentProviderOption = FindOptionByValue(ProviderOptions, SelectedProviderName);
		if (!CurrentProviderOption.IsValid())
		{
			CurrentProviderOption = MakeShared<FString>(SelectedProviderName);
			ProviderOptions.Add(CurrentProviderOption);
		}
		RefreshModelOptionsForProvider(CurrentSettings->Provider);
	}

	FString ProviderError;
	if (!ConfigureProviderFromSettings(ProviderError))
	{
		CurrentErrorMessage = ProviderError;
	}

	// Bind message queue events for queue depth indicator
	Session.GetMessageQueue().OnMessageQueued.AddSP(this, &SOliveAIChatPanel::HandleMessageQueued);
	Session.GetMessageQueue().OnQueueDrained.AddSP(this, &SOliveAIChatPanel::HandleQueueDrained);

	// Bind retry manager events for countdown UI
	Session.GetRetryManager().OnRetryScheduled.AddSP(this, &SOliveAIChatPanel::HandleRetryScheduled);
	Session.GetRetryManager().OnRetryCountdownTick.AddSP(this, &SOliveAIChatPanel::HandleRetryCountdownTick);
	Session.GetRetryManager().OnRetryAttemptStarted.AddSP(this, &SOliveAIChatPanel::HandleRetryAttemptStarted);

	// Initialize queue depth from current state (in case panel reopens with queued messages)
	QueuedMessageCount = Session.GetMessageQueue().GetQueueDepth();

	// Bind conversation manager events
	ConversationManager->OnMessageAdded.AddSP(this, &SOliveAIChatPanel::HandleMessageAdded);
	ConversationManager->OnStreamChunk.AddSP(this, &SOliveAIChatPanel::HandleStreamChunk);
	ConversationManager->OnToolCallStarted.AddSP(this, &SOliveAIChatPanel::HandleToolCallStarted);
	ConversationManager->OnToolCallCompleted.AddSP(this, &SOliveAIChatPanel::HandleToolCallCompleted);
	ConversationManager->OnProcessingStarted.AddSP(this, &SOliveAIChatPanel::HandleProcessingStarted);
	ConversationManager->OnProcessingComplete.AddSP(this, &SOliveAIChatPanel::HandleProcessingComplete);
	ConversationManager->OnError.AddSP(this, &SOliveAIChatPanel::HandleError);
	ConversationManager->OnModeChanged.AddSP(this, &SOliveAIChatPanel::HandleModeChanged);
	ConversationManager->OnModeSwitchDeferred.AddSP(this, &SOliveAIChatPanel::HandleModeSwitchDeferred);

	// Subscribe to editor events for auto-context
	SubscribeToEditorEvents();

	// Subscribe to run manager events
	FOliveRunManager::Get().OnRunStatusChanged.AddSP(this, &SOliveAIChatPanel::HandleRunStatusChanged);
	FOliveRunManager::Get().OnRunStepChanged.AddSP(this, &SOliveAIChatPanel::HandleRunStepChanged);

	// Build UI
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SVerticalBox)

			// Header
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildHeader()
			]

			// Context Bar
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 4, 0, 4)
			[
				BuildContextBar()
			]

			// Separator
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SSeparator)
			]

			// Message Area
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0, 4, 0, 4)
			[
				BuildMessageArea()
			]

			// Input Area
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 4, 0, 0)
			[
				BuildInputArea()
			]

			// Status Bar
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0, 4, 0, 0)
			[
				BuildStatusBar()
			]
		]
	];

	if (UOliveAISettings* CurrentSettings = UOliveAISettings::Get())
	{
		RefreshModelOptionsForProvider(CurrentSettings->Provider);
	}

	// Repopulate the message list from existing conversation history.
	// This handles the case where the panel was closed and reopened --
	// the ConversationManager (owned by the session) retains all messages.
	if (ConversationManager.IsValid() && MessageList.IsValid())
	{
		const TArray<FOliveChatMessage>& History = ConversationManager->GetMessageHistory();
		for (const FOliveChatMessage& Msg : History)
		{
			MessageList->AddMessage(Msg);
		}
		if (History.Num() > 0)
		{
			MessageList->ScrollToBottom();
		}
	}

	// If the ConversationManager is currently processing, reflect that in the UI.
	// The input field stays enabled so the user can queue messages.
	if (ConversationManager.IsValid() && ConversationManager->IsProcessing())
	{
		bIsProcessing = true;
	}
}

FReply SOliveAIChatPanel::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// Ctrl+Shift+M cycles through chat modes
	if (InKeyEvent.GetKey() == EKeys::M
		&& InKeyEvent.IsControlDown()
		&& InKeyEvent.IsShiftDown()
		&& !InKeyEvent.IsAltDown())
	{
		CycleMode();
		return FReply::Handled();
	}

	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

SOliveAIChatPanel::~SOliveAIChatPanel()
{
	UnsubscribeFromEditorEvents();
	FOliveRunManager::Get().OnRunStatusChanged.RemoveAll(this);
	FOliveRunManager::Get().OnRunStepChanged.RemoveAll(this);

	// Unbind message queue delegates
	FOliveEditorChatSession::Get().GetMessageQueue().OnMessageQueued.RemoveAll(this);
	FOliveEditorChatSession::Get().GetMessageQueue().OnQueueDrained.RemoveAll(this);

	// Unbind retry manager delegates
	FOliveEditorChatSession::Get().GetRetryManager().OnRetryScheduled.RemoveAll(this);
	FOliveEditorChatSession::Get().GetRetryManager().OnRetryCountdownTick.RemoveAll(this);
	FOliveEditorChatSession::Get().GetRetryManager().OnRetryAttemptStarted.RemoveAll(this);

	// Unbind all delegates from the ConversationManager.
	// The ConversationManager itself is NOT destroyed -- it lives in
	// FOliveEditorChatSession and survives panel close/reopen cycles.
	if (ConversationManager.IsValid())
	{
		ConversationManager->OnMessageAdded.RemoveAll(this);
		ConversationManager->OnStreamChunk.RemoveAll(this);
		ConversationManager->OnToolCallStarted.RemoveAll(this);
		ConversationManager->OnToolCallCompleted.RemoveAll(this);
		ConversationManager->OnProcessingStarted.RemoveAll(this);
		ConversationManager->OnProcessingComplete.RemoveAll(this);
		ConversationManager->OnError.RemoveAll(this);
		ConversationManager->OnModeChanged.RemoveAll(this);
		ConversationManager->OnModeSwitchDeferred.RemoveAll(this);
	}

	// Notify the session that the panel is closed. Operations in flight
	// will continue running; completions will trigger toast notifications.
	FOliveEditorChatSession::Get().NotifyPanelClosed();
}

// ==========================================
// Widget Construction
// ==========================================

TSharedRef<SWidget> SOliveAIChatPanel::BuildHeader()
{
	return SNew(SHorizontalBox)

		// Title
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PanelTitle", "Olive AI Chat"))
			.TextStyle(FAppStyle::Get(), "LargeText")
		]

		// Provider Selector
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4, 0)
		[
			BuildProviderSelector()
		]

		// Model Selector
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4, 0)
		[
			BuildModelSelector()
		]

		// New Chat Button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("NewChat", "New Chat"))
			.OnClicked(this, &SOliveAIChatPanel::OnNewChatClicked)
		]

		// Settings Button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked(this, &SOliveAIChatPanel::OnSettingsClicked)
			.ToolTipText(LOCTEXT("SettingsTooltip", "Open Settings"))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Settings"))
			]
		];
}

TSharedRef<SWidget> SOliveAIChatPanel::BuildProviderSelector()
{
	return SAssignNew(ProviderComboBox, SComboBox<TSharedPtr<FString>>)
		.OptionsSource(&ProviderOptions)
		.InitiallySelectedItem(CurrentProviderOption)
		.IsEnabled_Lambda([this]() { return !bIsProcessing; })
		.OnSelectionChanged(this, &SOliveAIChatPanel::OnProviderChanged)
		.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
		{
			return SNew(STextBlock)
				.Text(Item.IsValid() ? FText::FromString(*Item) : FText::GetEmpty());
		})
		.ToolTipText(LOCTEXT("ProviderPickerTooltip", "Select the active AI provider"))
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return CurrentProviderOption.IsValid() ? FText::FromString(*CurrentProviderOption) : LOCTEXT("ProviderUnset", "Provider");
			})
		];
}

TSharedRef<SWidget> SOliveAIChatPanel::BuildModelSelector()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SBox)
			.WidthOverride(220.0f)
			[
				SAssignNew(ModelTextBox, SEditableTextBox)
				.IsEnabled_Lambda([this]() { return !bIsProcessing; })
				.HintText(LOCTEXT("ModelHint", "Model"))
				.ToolTipText(LOCTEXT("ModelPickerTooltip", "Set model id for the selected provider"))
				.OnTextCommitted(this, &SOliveAIChatPanel::OnModelCommitted)
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f, 0.0f, 0.0f)
		[
			SAssignNew(ModelComboBox, SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&ModelOptions)
			.IsEnabled_Lambda([this]() { return !bIsProcessing; })
			.OnSelectionChanged(this, &SOliveAIChatPanel::OnModelSuggestionSelected)
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
			{
				return SNew(STextBlock)
					.Text(Item.IsValid() ? FText::FromString(*Item) : FText::GetEmpty());
			})
			.ToolTipText(LOCTEXT("ModelSuggestionTooltip", "Suggested models for this provider"))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ModelSuggestions", "Suggestions"))
			]
		];
}

TSharedRef<SWidget> SOliveAIChatPanel::BuildContextBar()
{
	return SAssignNew(ContextBar, SOliveAIContextBar)
		.OnAssetRemoved(this, &SOliveAIChatPanel::OnContextAssetRemoved);
}

TSharedRef<SWidget> SOliveAIChatPanel::BuildMessageArea()
{
	return SAssignNew(MessageList, SOliveAIMessageList)
		.OnNavigationAction(this, &SOliveAIChatPanel::HandleNavigationAction)
		.OnRunPause_Lambda([](){ FOliveRunManager::Get().PauseRun(); })
		.OnRunResume_Lambda([](){ FOliveRunManager::Get().ResumeRun(); })
		.OnRunCancel_Lambda([](){ FOliveRunManager::Get().CancelRun(); })
		.OnRunRetryStep_Lambda([](int32 Idx){ FOliveRunManager::Get().RetryStep(Idx); })
		.OnRunSkipStep_Lambda([](int32 Idx){ FOliveRunManager::Get().SkipStep(Idx); })
		.OnRunRollback_Lambda([](const FString& Id){ FOliveRunManager::Get().RollbackToCheckpoint(Id); });
}

TSharedRef<SWidget> SOliveAIChatPanel::BuildInputArea()
{
	return SNew(SHorizontalBox)

		// Mode Badge
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0, 0, 4, 0)
		[
			BuildModeBadge()
		]

		// Input Field
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SAssignNew(InputField, SOliveAIInputField)
			.OnMessageSubmit(this, &SOliveAIChatPanel::OnMessageSubmitted)
			.OnSlashCommand(this, &SOliveAIChatPanel::HandleSlashCommand)
			.OnAssetMentioned_Lambda([this](const FString& AssetPath)
			{
				// Add mentioned asset to context
				if (ContextBar.IsValid())
				{
					FString DisplayName = FPaths::GetBaseFilename(AssetPath);
					ContextBar->AddContextAsset(AssetPath, DisplayName, false);
				}
			})
		]

		// Send Button (text changes to "Queue" when processing)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4, 0, 0, 0)
		[
			SNew(SButton)
			.Text_Lambda([this]()
			{
				return bIsProcessing
					? LOCTEXT("Queue", "Queue")
					: LOCTEXT("Send", "Send");
			})
			.IsEnabled(this, &SOliveAIChatPanel::IsSendEnabled)
			.OnClicked(this, &SOliveAIChatPanel::OnSendMessageClicked)
		];
}

TSharedRef<SWidget> SOliveAIChatPanel::BuildStatusBar()
{
	return SNew(SHorizontalBox)

		// Spinner (visible during validation)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0, 0, 4, 0)
		[
			SNew(SCircularThrobber)
			.Radius(6.0f)
			.Visibility_Lambda([this]() -> EVisibility
			{
				return bIsValidating ? EVisibility::Visible : EVisibility::Collapsed;
			})
		]

		// Status indicator (check/X/circle — hidden during validation)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0, 0, 4, 0)
		[
			SNew(SImage)
			.Image_Lambda([this]() -> const FSlateBrush*
			{
				if (bIsValidating)
				{
					return FAppStyle::GetBrush("NoBrush");
				}
				if (!CurrentErrorMessage.IsEmpty() || (!bValidationSuccess && !ValidationMessage.IsEmpty()))
				{
					return FAppStyle::GetBrush("Icons.ErrorWithColor");
				}
				if (bValidationSuccess)
				{
					return FAppStyle::GetBrush("Icons.SuccessWithColor");
				}
				return FAppStyle::GetBrush("Icons.FilledCircle");
			})
			.ColorAndOpacity(this, &SOliveAIChatPanel::GetStatusColor)
			.DesiredSizeOverride(FVector2D(12, 12))
			.Visibility_Lambda([this]() -> EVisibility
			{
				return bIsValidating ? EVisibility::Collapsed : EVisibility::Visible;
			})
		]

		// Status text
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &SOliveAIChatPanel::GetStatusText)
			.TextStyle(FAppStyle::Get(), "SmallText")
		];
}

// ==========================================
// Event Handlers
// ==========================================

FReply SOliveAIChatPanel::OnSendMessageClicked()
{
	if (InputField.IsValid())
	{
		FString Message = InputField->GetText();
		OnMessageSubmitted(Message);
	}
	return FReply::Handled();
}

void SOliveAIChatPanel::OnMessageSubmitted(const FString& Message)
{
	if (Message.IsEmpty() || !ConversationManager.IsValid())
	{
		return;
	}

	if (UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		const FString PendingModel = ModelTextBox.IsValid() ? ModelTextBox->GetText().ToString().TrimStartAndEnd() : TEXT("");
		if (!PendingModel.IsEmpty())
		{
			Settings->SetSelectedModelForProvider(Settings->Provider, PendingModel);
			Settings->SaveConfig();
		}
	}

	// Only recreate the provider if there isn't one or the provider type changed.
	// Recreating on every send destroys CLI session state (thread IDs, resume flags).
	{
		TSharedPtr<IOliveAIProvider> CurrentProvider = ConversationManager->GetProvider();
		UOliveAISettings* Settings = UOliveAISettings::Get();
		const FString WantedName = Settings ? ProviderDisplayName(Settings->Provider) : TEXT("");
		const bool bNeedsNewProvider = !CurrentProvider.IsValid()
			|| CurrentProvider->GetProviderName() != WantedName;

		if (bNeedsNewProvider)
		{
			FString ProviderError;
			if (!ConfigureProviderFromSettings(ProviderError))
			{
				HandleError(ProviderError);
				return;
			}
		}
		else if (Settings && CurrentProvider.IsValid())
		{
			FString ModelId = Settings->GetSelectedModelForProvider(Settings->Provider);
			if (ModelId.IsEmpty())
			{
				ModelId = CurrentProvider->GetRecommendedModel();
				if (ModelId.IsEmpty())
				{
					const TArray<FString> AvailableModels = CurrentProvider->GetAvailableModels();
					if (AvailableModels.Num() > 0)
					{
						ModelId = AvailableModels[0];
					}
				}

				if (!ModelId.IsEmpty())
				{
					Settings->SetSelectedModelForProvider(Settings->Provider, ModelId);
					Settings->SaveConfig();
				}
			}

			FOliveProviderConfig ProviderConfig;
			ProviderConfig.ProviderName = WantedName;
			ProviderConfig.ApiKey = Settings->GetCurrentApiKey();
			ProviderConfig.BaseUrl = Settings->GetCurrentBaseUrl();
			ProviderConfig.ModelId = ModelId;
			ProviderConfig.Temperature = Settings->Temperature;
			ProviderConfig.MaxTokens = Settings->MaxTokens;
			ProviderConfig.TimeoutSeconds = Settings->RequestTimeoutSeconds;
			CurrentProvider->Configure(ProviderConfig);

			if (ModelTextBox.IsValid())
			{
				ModelTextBox->SetText(FText::FromString(ModelId));
			}
		}
	}

	// Update context from context bar
	if (ContextBar.IsValid())
	{
		ConversationManager->SetActiveContext(ContextBar->GetContextAssetPaths());
	}

	// Send message
	ConversationManager->SendUserMessage(Message);

	// Clear input
	if (InputField.IsValid())
	{
		InputField->Clear();
	}
}

FReply SOliveAIChatPanel::OnSettingsClicked()
{
	// Open project settings to Olive AI section
	FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Editor", "Plugins", "Olive AI Studio");
	return FReply::Handled();
}

FReply SOliveAIChatPanel::OnNewChatClicked()
{
	if (ConversationManager.IsValid())
	{
		ConversationManager->StartNewSession();
	}

	if (MessageList.IsValid())
	{
		MessageList->ClearMessages();
	}

	CurrentErrorMessage.Empty();
	return FReply::Handled();
}

bool SOliveAIChatPanel::ConfigureProviderFromSettings(FString& OutError)
{
	OutError.Empty();

	if (!ConversationManager.IsValid())
	{
		OutError = TEXT("Conversation manager is not initialized.");
		return false;
	}

	UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		OutError = TEXT("Olive AI settings are unavailable.");
		return false;
	}

	const FString ProviderName = ProviderDisplayName(Settings->Provider);

	TSharedPtr<IOliveAIProvider> Provider = FOliveProviderFactory::CreateProvider(ProviderName);
	if (!Provider.IsValid())
	{
		OutError = FString::Printf(TEXT("Provider '%s' is unavailable."), *ProviderName);
		ConversationManager->SetProvider(nullptr);
		return false;
	}

	FString ModelId = Settings->GetSelectedModelForProvider(Settings->Provider);
	if (ModelId.IsEmpty())
	{
		ModelId = Provider->GetRecommendedModel();
		if (ModelId.IsEmpty())
		{
			const TArray<FString> AvailableModels = Provider->GetAvailableModels();
			if (AvailableModels.Num() > 0)
			{
				ModelId = AvailableModels[0];
			}
		}

		if (!ModelId.IsEmpty())
		{
			Settings->SetSelectedModelForProvider(Settings->Provider, ModelId);
			Settings->SaveConfig();
		}
	}

	FOliveProviderConfig ProviderConfig;
	ProviderConfig.ProviderName = ProviderName;
	ProviderConfig.ApiKey = Settings->GetCurrentApiKey();
	ProviderConfig.BaseUrl = Settings->GetCurrentBaseUrl();
	ProviderConfig.ModelId = ModelId;
	ProviderConfig.Temperature = Settings->Temperature;
	ProviderConfig.MaxTokens = Settings->MaxTokens;
	ProviderConfig.TimeoutSeconds = Settings->RequestTimeoutSeconds;
	Provider->Configure(ProviderConfig);

	if (ModelTextBox.IsValid())
	{
		ModelTextBox->SetText(FText::FromString(ModelId));
	}

	ConversationManager->SetProvider(Provider);

	// Trigger async connection validation
	bIsValidating = true;
	ValidationMessage = TEXT("Checking connection...");
	Provider->ValidateConnection([this, ModelId](bool bSuccess, const FString& Message)
	{
		// Must dispatch to game thread if called from HTTP thread
		AsyncTask(ENamedThreads::GameThread, [this, bSuccess, Message]()
		{
			bIsValidating = false;
			bValidationSuccess = bSuccess;
			ValidationMessage = Message;
			if (!bSuccess)
			{
				UE_LOG(LogOliveAI, Warning, TEXT("Provider validation failed: %s"), *Message);
			}
			else
			{
				UE_LOG(LogOliveAI, Log, TEXT("Provider validation passed: %s"), *Message);
			}

		});
	});

	return true;
}

// ==========================================
// Conversation Manager Callbacks
// ==========================================

void SOliveAIChatPanel::HandleMessageAdded(const FOliveChatMessage& Message)
{
	if (!MessageList.IsValid())
	{
		return;
	}

	MessageList->AddMessage(Message);
	MessageList->ScrollToBottom();
}

void SOliveAIChatPanel::HandleStreamChunk(const FString& Chunk)
{
	if (MessageList.IsValid())
	{
		MessageList->AppendToLastMessage(Chunk);
	}
}

void SOliveAIChatPanel::HandleToolCallStarted(const FString& ToolName, const FString& ToolCallId)
{
	if (MessageList.IsValid())
	{
		MessageList->AddToolCallIndicator(ToolName, ToolCallId);
	}
}

void SOliveAIChatPanel::HandleToolCallCompleted(const FString& ToolName, const FString& ToolCallId, const FOliveToolResult& Result)
{
	if (MessageList.IsValid())
	{
		FString Summary = Result.bSuccess ? TEXT("Done") : TEXT("Failed");
		MessageList->UpdateToolCallStatus(ToolCallId, Result.bSuccess, Summary);
		MessageList->AddResultCard(ToolCallId, ToolName, Result);
	}
}

void SOliveAIChatPanel::HandleProcessingStarted()
{
	bIsProcessing = true;
	CurrentErrorMessage.Empty();

	// Input field stays enabled so the user can queue additional messages.
	// The Send button text changes to "Queue" to communicate queuing behavior.
}

void SOliveAIChatPanel::HandleProcessingComplete()
{
	bIsProcessing = false;
	bIsRetryPending = false;
	RetryAttempt = 0;
	RetryMaxAttempts = 0;
	RetryCountdownSeconds = 0.0f;
	bIsRateLimited = false;

	if (MessageList.IsValid())
	{
		MessageList->CompleteLastMessage();
	}

	if (InputField.IsValid())
	{
		InputField->Focus();
	}
}

void SOliveAIChatPanel::HandleError(const FString& ErrorMessage)
{
	CurrentErrorMessage = ErrorMessage;

	if (MessageList.IsValid())
	{
		MessageList->AddErrorMessage(ErrorMessage);
	}
}

// ==========================================
// Message Queue Callbacks
// ==========================================

void SOliveAIChatPanel::HandleMessageQueued(int32 QueueDepth)
{
	QueuedMessageCount = QueueDepth;
	// Status text auto-updates via GetStatusText() polling
}

void SOliveAIChatPanel::HandleQueueDrained()
{
	QueuedMessageCount = 0;
}

// ==========================================
// Retry Manager Callbacks
// ==========================================

void SOliveAIChatPanel::HandleRetryScheduled(int32 Attempt, int32 MaxAttempts, float DelaySeconds)
{
	bIsRetryPending = true;
	RetryAttempt = Attempt;
	RetryMaxAttempts = MaxAttempts;
	RetryCountdownSeconds = DelaySeconds;

	// Detect rate limiting: when Attempt == MaxAttempts, the retry manager is
	// doing a single rate-limit retry rather than exponential backoff.
	bIsRateLimited = (Attempt == MaxAttempts && DelaySeconds > 10.0f);

	// Status text auto-updates via GetStatusText() polling
}

void SOliveAIChatPanel::HandleRetryCountdownTick(float SecondsRemaining)
{
	RetryCountdownSeconds = SecondsRemaining;
	// Status text auto-updates via GetStatusText() polling
}

void SOliveAIChatPanel::HandleRetryAttemptStarted()
{
	bIsRetryPending = false;
	RetryCountdownSeconds = 0.0f;
	// The processing state is still active; status returns to "Processing..."
}

// ==========================================
// Context Updates
// ==========================================

void SOliveAIChatPanel::OnContextAssetRemoved(const FString& AssetPath)
{
	// Update conversation manager context
	if (ConversationManager.IsValid() && ContextBar.IsValid())
	{
		ConversationManager->SetActiveContext(ContextBar->GetContextAssetPaths());
	}
}

void SOliveAIChatPanel::OnAssetOpened(UObject* Asset)
{
	if (!Asset || !ContextBar.IsValid())
	{
		return;
	}

	// Get asset path
	FString AssetPath = Asset->GetPathName();
	FString DisplayName = Asset->GetName();

	ContextBar->SetAutoContext(AssetPath, DisplayName);

	// Update conversation manager
	if (ConversationManager.IsValid())
	{
		ConversationManager->SetActiveContext(ContextBar->GetContextAssetPaths());
	}
}

void SOliveAIChatPanel::UpdateContextFromSelection()
{
	if (!GEditor || !ContextBar.IsValid()) return;

	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Subsystem) return;

	UObject* ActiveAsset = LastActiveAsset.Get();
	if (!ActiveAsset)
	{
		TArray<UObject*> EditedAssets = Subsystem->GetAllEditedAssets();
		for (UObject* Asset : EditedAssets)
		{
			IAssetEditorInstance* Editor = Subsystem->FindEditorForAsset(Asset, false);
			if (Editor)
			{
				ActiveAsset = Asset;
				break;
			}
		}
	}

	if (ActiveAsset)
	{
		ContextBar->SetAutoContext(ActiveAsset->GetPathName(), ActiveAsset->GetName());
		LastActiveAsset = ActiveAsset;
	}

	// Selected nodes: use the editor's global selection set (safe) instead of casting editor instances.
	UBlueprint* BP = Cast<UBlueprint>(ActiveAsset);
	if (BP)
	{
		TArray<FString> NodeNames;
		if (USelection* Selection = GEditor->GetSelectedObjects())
		{
			for (FSelectionIterator It(*Selection); It; ++It)
			{
				UEdGraphNode* Node = Cast<UEdGraphNode>(*It);
				if (!Node)
				{
					continue;
				}

				if (Node->GetTypedOuter<UBlueprint>() != BP)
				{
					continue;
				}

				NodeNames.Add(Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
		}

		if (NodeNames.Num() > 0)
		{
			ContextBar->SetSelectedNodes(NodeNames);
		}
		else
		{
			ContextBar->ClearSelectedNodes();
		}
	}
	else
	{
		ContextBar->ClearSelectedNodes();
	}

	if (ConversationManager.IsValid())
	{
		ConversationManager->SetActiveContext(ContextBar->GetContextAssetPaths());
	}
}

// ==========================================
// Status
// ==========================================

FText SOliveAIChatPanel::GetStatusText() const
{
	if (!CurrentErrorMessage.IsEmpty())
	{
		return FText::FromString(CurrentErrorMessage);
	}

	if (bIsProcessing)
	{
		// Retry countdown takes priority over generic "Processing..."
		if (bIsRetryPending)
		{
			int32 SecondsInt = FMath::CeilToInt32(RetryCountdownSeconds);
			if (bIsRateLimited)
			{
				return FText::Format(
					LOCTEXT("StatusRateLimited", "Rate limited. Retrying in {0}s..."),
					FText::AsNumber(SecondsInt));
			}
			else
			{
				return FText::Format(
					LOCTEXT("StatusRetry", "Retry {0}/{1} in {2}s..."),
					FText::AsNumber(RetryAttempt),
					FText::AsNumber(RetryMaxAttempts),
					FText::AsNumber(SecondsInt));
			}
		}

		if (QueuedMessageCount > 0)
		{
			return FText::Format(
				LOCTEXT("StatusProcessingQueued", "Processing... ({0} {0}|plural(one=message,other=messages) queued)"),
				FText::AsNumber(QueuedMessageCount));
		}
		return LOCTEXT("StatusProcessing", "Processing...");
	}

	if (bIsValidating)
	{
		return FText::FromString(ValidationMessage);
	}

	if (!bValidationSuccess && !ValidationMessage.IsEmpty())
	{
		return FText::FromString(ValidationMessage);
	}

	if (ConversationManager.IsValid() && ConversationManager->GetProvider().IsValid())
	{
		FString ProviderName = ConversationManager->GetProvider()->GetProviderName();
		FString ModelName = ConversationManager->GetProvider()->GetConfig().ModelId;
		const bool bHasModel = !ModelName.TrimStartAndEnd().IsEmpty();
		if (bValidationSuccess && !ValidationMessage.IsEmpty())
		{
			if (bHasModel)
			{
				return FText::Format(
					LOCTEXT("StatusReadyValidatedWithModel", "{0} ({1}) - {2}"),
					FText::FromString(ProviderName),
					FText::FromString(ModelName),
					FText::FromString(ValidationMessage));
			}
			return FText::Format(
				LOCTEXT("StatusReadyValidatedNoModel", "{0} - {1}"),
				FText::FromString(ProviderName),
				FText::FromString(ValidationMessage));
		}

		if (bHasModel)
		{
			return FText::Format(
				LOCTEXT("StatusReadyWithModel", "Ready - {0} ({1})"),
				FText::FromString(ProviderName),
				FText::FromString(ModelName));
		}
		return FText::Format(LOCTEXT("StatusReadyNoModel", "Ready - {0}"), FText::FromString(ProviderName));
	}

	return LOCTEXT("StatusNoProvider", "No provider configured");
}

FSlateColor SOliveAIChatPanel::GetStatusColor() const
{
	if (!CurrentErrorMessage.IsEmpty())
	{
		return FLinearColor::Red;
	}

	if (bIsProcessing)
	{
		if (bIsRetryPending && bIsRateLimited)
		{
			// Orange for rate-limited
			return FLinearColor(1.0f, 0.6f, 0.1f);
		}
		return FLinearColor::Yellow;
	}

	if (bIsValidating)
	{
		return FLinearColor::Yellow;
	}

	if (!bValidationSuccess && !ValidationMessage.IsEmpty())
	{
		return FLinearColor::Red;
	}

	if (ConversationManager.IsValid() && ConversationManager->GetProvider().IsValid())
	{
		return FLinearColor::Green;
	}

	return FLinearColor::Gray;
}

bool SOliveAIChatPanel::IsSendEnabled() const
{
	// Always enabled when the ConversationManager and provider are valid.
	// When processing, SendUserMessage() will enqueue the message via the
	// FOliveMessageQueue instead of rejecting it.
	return ConversationManager.IsValid() && ConversationManager->GetProvider().IsValid();
}

// ==========================================
// Mode Badge
// ==========================================

TSharedRef<SWidget> SOliveAIChatPanel::BuildModeBadge()
{
	return SNew(SButton)
		.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("FlatButton"))
		.OnClicked(this, &SOliveAIChatPanel::OnModeBadgeClicked)
		.ToolTipText(LOCTEXT("ModeBadgeTooltip", "Click to cycle mode (Code/Plan/Ask), or type /code /plan /ask.\nCtrl+Shift+M to cycle."))
		.ContentPadding(FMargin(8, 2))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.BorderBackgroundColor_Lambda([this]() -> FSlateColor
			{
				return FSlateColor(GetModeBadgeBackgroundColor());
			})
			.Padding(FMargin(6, 2))
			[
				SNew(STextBlock)
				.Text(this, &SOliveAIChatPanel::GetModeBadgeText)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(this, &SOliveAIChatPanel::GetModeBadgeColor)
			]
		];
}

FReply SOliveAIChatPanel::OnModeBadgeClicked()
{
	CycleMode();
	return FReply::Handled();
}

void SOliveAIChatPanel::CycleMode()
{
	if (!ConversationManager.IsValid())
	{
		return;
	}

	const EOliveChatMode Current = ConversationManager->GetChatMode();
	EOliveChatMode Next;
	switch (Current)
	{
	case EOliveChatMode::Code: Next = EOliveChatMode::Plan; break;
	case EOliveChatMode::Plan: Next = EOliveChatMode::Ask;  break;
	case EOliveChatMode::Ask:  Next = EOliveChatMode::Code; break;
	default:                   Next = EOliveChatMode::Code; break;
	}

	ConversationManager->SetChatMode(Next);
}

FText SOliveAIChatPanel::GetModeBadgeText() const
{
	if (!ConversationManager.IsValid())
	{
		return LOCTEXT("ModeBadgeCode", "CODE");
	}

	switch (ConversationManager->GetChatMode())
	{
	case EOliveChatMode::Code: return LOCTEXT("ModeBadgeCode", "CODE");
	case EOliveChatMode::Plan: return LOCTEXT("ModeBadgePlan", "PLAN");
	case EOliveChatMode::Ask:  return LOCTEXT("ModeBadgeAsk", "ASK");
	default:                   return LOCTEXT("ModeBadgeCode", "CODE");
	}
}

FSlateColor SOliveAIChatPanel::GetModeBadgeColor() const
{
	if (!ConversationManager.IsValid())
	{
		return FSlateColor(FLinearColor(0.298f, 0.686f, 0.314f)); // Green
	}

	switch (ConversationManager->GetChatMode())
	{
	case EOliveChatMode::Code: return FSlateColor(FLinearColor(0.298f, 0.686f, 0.314f)); // #4CAF50
	case EOliveChatMode::Plan: return FSlateColor(FLinearColor(1.0f, 0.757f, 0.027f));   // #FFC107
	case EOliveChatMode::Ask:  return FSlateColor(FLinearColor(0.129f, 0.588f, 0.953f)); // #2196F3
	default:                   return FSlateColor(FLinearColor(0.298f, 0.686f, 0.314f));
	}
}

FLinearColor SOliveAIChatPanel::GetModeBadgeBackgroundColor() const
{
	if (!ConversationManager.IsValid())
	{
		return FLinearColor(0.298f, 0.686f, 0.314f, 0.2f);
	}

	switch (ConversationManager->GetChatMode())
	{
	case EOliveChatMode::Code: return FLinearColor(0.298f, 0.686f, 0.314f, 0.2f); // Green at 20%
	case EOliveChatMode::Plan: return FLinearColor(1.0f, 0.596f, 0.0f, 0.2f);     // Amber at 20%
	case EOliveChatMode::Ask:  return FLinearColor(0.129f, 0.588f, 0.953f, 0.2f); // Blue at 20%
	default:                   return FLinearColor(0.298f, 0.686f, 0.314f, 0.2f);
	}
}

// ==========================================
// Slash Commands
// ==========================================

void SOliveAIChatPanel::HandleSlashCommand(const FString& Command)
{
	if (!ConversationManager.IsValid())
	{
		return;
	}

	FString Cmd = Command.ToLower().TrimStartAndEnd();

	if (Cmd == TEXT("/code"))
	{
		ConversationManager->SetChatMode(EOliveChatMode::Code);
	}
	else if (Cmd == TEXT("/plan"))
	{
		ConversationManager->SetChatMode(EOliveChatMode::Plan);
	}
	else if (Cmd == TEXT("/ask"))
	{
		ConversationManager->SetChatMode(EOliveChatMode::Ask);
	}
	else if (Cmd == TEXT("/mode"))
	{
		AddSystemMessage(FString::Printf(TEXT("Current mode: %s"), LexToString(ConversationManager->GetChatMode())));
	}
	else if (Cmd == TEXT("/status"))
	{
		const int32 ToolCount = FOliveToolRegistry::Get().GetToolsForMode(ConversationManager->GetChatMode()).Num();
		const bool bProcessing = ConversationManager->IsProcessing();
		AddSystemMessage(FString::Printf(TEXT("Mode: %s | Processing: %s | Tools: %d"),
			LexToString(ConversationManager->GetChatMode()),
			bProcessing ? TEXT("Yes") : TEXT("No"),
			ToolCount));
	}
}

// ==========================================
// Mode Change Handlers
// ==========================================

void SOliveAIChatPanel::HandleModeChanged(EOliveChatMode NewMode)
{
	// Insert a system message describing the new mode
	switch (NewMode)
	{
	case EOliveChatMode::Code:
		AddSystemMessage(TEXT("Switched to Code mode. All tools active. Executing autonomously."));
		break;
	case EOliveChatMode::Plan:
		AddSystemMessage(TEXT("Switched to Plan mode. Read tools active. Write tools blocked until you approve."));
		break;
	case EOliveChatMode::Ask:
		AddSystemMessage(TEXT("Switched to Ask mode. Read-only. No changes will be made."));
		break;
	}

	// Badge auto-updates via polling lambdas -- no explicit invalidation needed
}

void SOliveAIChatPanel::HandleModeSwitchDeferred(EOliveChatMode PendingMode)
{
	AddSystemMessage(FString::Printf(TEXT("Mode will switch to %s after the current operation finishes."),
		LexToString(PendingMode)));
}

void SOliveAIChatPanel::AddSystemMessage(const FString& Message)
{
	if (MessageList.IsValid())
	{
		MessageList->AddSystemMessage(Message);
	}
}

void SOliveAIChatPanel::OnProviderChanged(TSharedPtr<FString> NewProvider, ESelectInfo::Type SelectInfo)
{
	if (!NewProvider.IsValid() || bIsProcessing)
	{
		return;
	}

	UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		return;
	}

	const FString CurrentTypedModel = ModelTextBox.IsValid() ? ModelTextBox->GetText().ToString().TrimStartAndEnd() : TEXT("");
	if (!CurrentTypedModel.IsEmpty())
	{
		Settings->SetSelectedModelForProvider(Settings->Provider, CurrentTypedModel);
	}

	const EOliveAIProvider NewProviderType = ProviderFromDisplayName(*NewProvider);
	FString NewModel = Settings->GetSelectedModelForProvider(NewProviderType);
	if (NewModel.IsEmpty())
	{
		const TSharedPtr<IOliveAIProvider> TempProvider = FOliveProviderFactory::CreateProvider(ProviderDisplayName(NewProviderType));
		if (TempProvider.IsValid())
		{
			NewModel = TempProvider->GetRecommendedModel();
			if (NewModel.IsEmpty())
			{
				const TArray<FString> AvailableModels = TempProvider->GetAvailableModels();
				if (AvailableModels.Num() > 0)
				{
					NewModel = AvailableModels[0];
				}
			}
		}
	}

	CurrentProviderOption = NewProvider;
	ApplyProviderAndModelSelection(NewProviderType, NewModel, true);
}

void SOliveAIChatPanel::OnModelSuggestionSelected(TSharedPtr<FString> NewModel, ESelectInfo::Type SelectInfo)
{
	if (!NewModel.IsValid() || bIsProcessing)
	{
		return;
	}

	CurrentModelOption = NewModel;
	if (ModelTextBox.IsValid())
	{
		ModelTextBox->SetText(FText::FromString(*NewModel));
	}

	if (UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		ApplyProviderAndModelSelection(Settings->Provider, *NewModel, true);
	}
}

void SOliveAIChatPanel::OnModelCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	if (bIsProcessing)
	{
		return;
	}

	if (CommitType != ETextCommit::OnEnter && CommitType != ETextCommit::OnUserMovedFocus)
	{
		return;
	}

	const FString ModelId = NewText.ToString().TrimStartAndEnd();
	if (ModelId.IsEmpty())
	{
		return;
	}

	if (UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		ApplyProviderAndModelSelection(Settings->Provider, ModelId, true);
	}
}

void SOliveAIChatPanel::RefreshProviderOptions()
{
	ProviderOptions.Empty();

	const TArray<FString> AvailableProviders = FOliveProviderFactory::GetAvailableProviders();
	for (const FString& ProviderName : AvailableProviders)
	{
		ProviderOptions.Add(MakeShared<FString>(ProviderName));
	}

	if (const UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		const FString ActiveProviderName = ProviderDisplayName(Settings->Provider);
		if (!FindOptionByValue(ProviderOptions, ActiveProviderName).IsValid())
		{
			ProviderOptions.Add(MakeShared<FString>(ActiveProviderName));
		}
	}
}

void SOliveAIChatPanel::RefreshModelOptionsForProvider(EOliveAIProvider ProviderType)
{
	ModelOptions.Empty();

	const FString ProviderName = ProviderDisplayName(ProviderType);
	const TSharedPtr<IOliveAIProvider> TempProvider = FOliveProviderFactory::CreateProvider(ProviderName);
	if (TempProvider.IsValid())
	{
		const TArray<FString> AvailableModels = TempProvider->GetAvailableModels();
		for (const FString& Model : AvailableModels)
		{
			ModelOptions.Add(MakeShared<FString>(Model));
		}
	}

	UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		return;
	}

	FString ActiveModel = Settings->GetSelectedModelForProvider(ProviderType);
	if (ActiveModel.IsEmpty() && TempProvider.IsValid())
	{
		ActiveModel = TempProvider->GetRecommendedModel();
	}
	if (ActiveModel.IsEmpty() && TempProvider.IsValid())
	{
		const TArray<FString> AvailableModels = TempProvider->GetAvailableModels();
		if (AvailableModels.Num() > 0)
		{
			ActiveModel = AvailableModels[0];
		}
	}

	if (!ActiveModel.IsEmpty())
	{
		CurrentModelOption = FindOptionByValue(ModelOptions, ActiveModel);
		if (!CurrentModelOption.IsValid())
		{
			CurrentModelOption = MakeShared<FString>(ActiveModel);
		}

		if (ModelTextBox.IsValid())
		{
			ModelTextBox->SetText(FText::FromString(ActiveModel));
		}
	}
}

void SOliveAIChatPanel::ApplyProviderAndModelSelection(EOliveAIProvider ProviderType, const FString& ModelId, bool bSaveConfig)
{
	UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		return;
	}

	FString FinalModelId = ModelId.TrimStartAndEnd();
	if (FinalModelId.IsEmpty())
	{
		if (const TSharedPtr<IOliveAIProvider> TempProvider = FOliveProviderFactory::CreateProvider(ProviderDisplayName(ProviderType)); TempProvider.IsValid())
		{
			FinalModelId = TempProvider->GetRecommendedModel();
			if (FinalModelId.IsEmpty())
			{
				const TArray<FString> AvailableModels = TempProvider->GetAvailableModels();
				if (AvailableModels.Num() > 0)
				{
					FinalModelId = AvailableModels[0];
				}
			}
		}
	}

	Settings->Provider = ProviderType;
	if (!FinalModelId.IsEmpty())
	{
		Settings->SetSelectedModelForProvider(ProviderType, FinalModelId);
	}

	if (bSaveConfig)
	{
		Settings->SaveConfig();
	}

	const FString ProviderName = ProviderDisplayName(ProviderType);
	CurrentProviderOption = FindOptionByValue(ProviderOptions, ProviderName);
	if (!CurrentProviderOption.IsValid())
	{
		CurrentProviderOption = MakeShared<FString>(ProviderName);
		ProviderOptions.Add(CurrentProviderOption);
	}

	RefreshModelOptionsForProvider(ProviderType);
	if (!FinalModelId.IsEmpty())
	{
		CurrentModelOption = FindOptionByValue(ModelOptions, FinalModelId);
		if (ModelTextBox.IsValid())
		{
			ModelTextBox->SetText(FText::FromString(FinalModelId));
		}
	}

	FString ProviderError;
	if (!ConfigureProviderFromSettings(ProviderError))
	{
		HandleError(ProviderError);
	}
	else
	{
		CurrentErrorMessage.Empty();
	}
}

// ==========================================
// Editor Event Subscriptions
// ==========================================

void SOliveAIChatPanel::SubscribeToEditorEvents()
{
	if (GEditor)
	{
		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (Subsystem)
		{
			OnAssetEditorOpenedHandle = Subsystem->OnAssetOpenedInEditor().AddLambda(
				[this](UObject* Asset, IAssetEditorInstance* /*EditorInstance*/)
				{
					OnAssetOpened(Asset);
				});
		}

		if (!OnEditorSelectionChangedHandle.IsValid())
		{
			OnEditorSelectionChangedHandle = USelection::SelectionChangedEvent.AddLambda([this](UObject* /*NewSelection*/)
			{
				UpdateContextFromSelectionDebounced();
			});
		}
	}
}

void SOliveAIChatPanel::UnsubscribeFromEditorEvents()
{
	if (GEditor)
	{
		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (Subsystem)
		{
			Subsystem->OnAssetOpenedInEditor().Remove(OnAssetEditorOpenedHandle);
		}

		if (OnEditorSelectionChangedHandle.IsValid())
		{
			USelection::SelectionChangedEvent.Remove(OnEditorSelectionChangedHandle);
			OnEditorSelectionChangedHandle.Reset();
		}
	}
}

void SOliveAIChatPanel::UpdateContextFromSelectionDebounced()
{
	if (GEditor)
	{
		GEditor->GetTimerManager()->SetTimer(
			SelectionDebounceTimer,
			FTimerDelegate::CreateSP(this, &SOliveAIChatPanel::UpdateContextFromSelection),
			0.3f, false);
	}
}

// ==========================================
// Run Mode Handlers
// ==========================================

void SOliveAIChatPanel::HandleRunStatusChanged(const FOliveRun& Run)
{
	if (MessageList.IsValid())
	{
		if (Run.Steps.Num() == 0)
		{
			// New run started
			MessageList->AddRunHeader(Run);
		}
		MessageList->UpdateRunStatus(Run);
	}
}

void SOliveAIChatPanel::HandleRunStepChanged(const FOliveRun& Run, int32 StepIndex)
{
	if (MessageList.IsValid())
	{
		MessageList->UpdateRunStep(Run, StepIndex);
	}
}

// ==========================================
// Navigation Dispatch
// ==========================================

void SOliveAIChatPanel::HandleNavigationAction(const FOliveNavigationAction& Action)
{
	if (Action.bIsCompileError)
	{
		FOliveBlueprintNavigator::NavigateToCompileError(Action.AssetPath, Action.CompileError);
	}
	else if (Action.NodeIds.Num() > 0)
	{
		FOliveBlueprintNavigator::SelectAndZoomToNodes(Action.AssetPath, Action.NodeIds);
	}
	else if (!Action.GraphName.IsEmpty())
	{
		FOliveBlueprintNavigator::OpenGraph(Action.AssetPath, Action.GraphName);
	}
	else if (!Action.AssetPath.IsEmpty())
	{
		FOliveBlueprintNavigator::OpenAsset(Action.AssetPath);
	}
	else if (!Action.SnapshotId.IsEmpty())
	{
		FOliveSnapshotManager::Get().RollbackSnapshot(Action.SnapshotId, {}, true, TEXT(""));
	}
}

#undef LOCTEXT_NAMESPACE
