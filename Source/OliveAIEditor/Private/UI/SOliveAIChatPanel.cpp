// Copyright Bode Software. All Rights Reserved.

#include "UI/SOliveAIChatPanel.h"
#include "Async/Async.h"
#include "UI/SOliveAIMessageList.h"
#include "UI/SOliveAIContextBar.h"
#include "UI/SOliveAIInputField.h"
#include "Chat/OliveConversationManager.h"
#include "Providers/IOliveAIProvider.h"
#include "Settings/OliveAISettings.h"
#include "OliveAIEditorModule.h"
#include "Profiles/OliveFocusProfileManager.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
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
	// Initialize focus profiles from the source-of-truth manager (built-ins + future custom profiles).
	{
		const TArray<FString> ProfileNames = FOliveFocusProfileManager::Get().GetProfileNames();
		for (const FString& Name : ProfileNames)
		{
			FocusProfiles.Add(MakeShared<FString>(Name));
		}

		// Fallback if something went wrong during initialization.
		if (FocusProfiles.Num() == 0)
		{
			FocusProfiles.Add(MakeShared<FString>(TEXT("Auto")));
		}

		// Default to Auto if present, otherwise first item.
		CurrentFocusProfile = FocusProfiles[0];
		for (const TSharedPtr<FString>& ProfileOption : FocusProfiles)
		{
			if (ProfileOption.IsValid() && ProfileOption->Equals(TEXT("Auto"), ESearchCase::IgnoreCase))
			{
				CurrentFocusProfile = ProfileOption;
				break;
			}
		}
	}

	// Create conversation manager
	ConversationManager = MakeShared<FOliveConversationManager>();

	// Set up provider and profile from settings
	UOliveAISettings* Settings = UOliveAISettings::Get();
	if (Settings)
	{
		const FString NormalizedDefaultProfile = FOliveFocusProfileManager::Get().NormalizeProfileName(Settings->DefaultFocusProfile);
		bool bMatchedConfiguredDefault = false;
		for (const TSharedPtr<FString>& ProfileOption : FocusProfiles)
		{
			if (ProfileOption.IsValid() && ProfileOption->Equals(NormalizedDefaultProfile, ESearchCase::IgnoreCase))
			{
				CurrentFocusProfile = ProfileOption;
				bMatchedConfiguredDefault = true;
				break;
			}
		}

		bool bSettingsChanged = false;
		if (!Settings->DefaultFocusProfile.Equals(NormalizedDefaultProfile, ESearchCase::CaseSensitive))
		{
			Settings->DefaultFocusProfile = NormalizedDefaultProfile;
			bSettingsChanged = true;
		}

		if (!bMatchedConfiguredDefault && !Settings->DefaultFocusProfile.Equals(TEXT("Auto"), ESearchCase::CaseSensitive))
		{
			Settings->DefaultFocusProfile = TEXT("Auto");
			bSettingsChanged = true;
		}

		if (bSettingsChanged)
		{
			Settings->SaveConfig();
		}
	}

	// Initialize safety preset options
	SafetyPresetOptions.Add(MakeShared<FString>(TEXT("Careful")));
	SafetyPresetOptions.Add(MakeShared<FString>(TEXT("Fast")));
	SafetyPresetOptions.Add(MakeShared<FString>(TEXT("YOLO")));
	CurrentSafetyPreset = SafetyPresetOptions[static_cast<int32>(UOliveAISettings::Get()->SafetyPreset)];

	FString ProviderError;
	if (!ConfigureProviderFromSettings(ProviderError))
	{
		CurrentErrorMessage = ProviderError;
	}

	if (ConversationManager.IsValid() && CurrentFocusProfile.IsValid())
	{
		ConversationManager->SetFocusProfile(*CurrentFocusProfile);
	}

	// Bind conversation manager events
	ConversationManager->OnMessageAdded.AddSP(this, &SOliveAIChatPanel::HandleMessageAdded);
	ConversationManager->OnStreamChunk.AddSP(this, &SOliveAIChatPanel::HandleStreamChunk);
	ConversationManager->OnToolCallStarted.AddSP(this, &SOliveAIChatPanel::HandleToolCallStarted);
	ConversationManager->OnToolCallCompleted.AddSP(this, &SOliveAIChatPanel::HandleToolCallCompleted);
	ConversationManager->OnProcessingStarted.AddSP(this, &SOliveAIChatPanel::HandleProcessingStarted);
	ConversationManager->OnProcessingComplete.AddSP(this, &SOliveAIChatPanel::HandleProcessingComplete);
	ConversationManager->OnError.AddSP(this, &SOliveAIChatPanel::HandleError);
	ConversationManager->OnConfirmationRequired.AddSP(this, &SOliveAIChatPanel::HandleConfirmationRequired);

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
}

SOliveAIChatPanel::~SOliveAIChatPanel()
{
	UnsubscribeFromEditorEvents();
	FOliveRunManager::Get().OnRunStatusChanged.RemoveAll(this);
	FOliveRunManager::Get().OnRunStepChanged.RemoveAll(this);

	if (ConversationManager.IsValid())
	{
		ConversationManager->OnMessageAdded.RemoveAll(this);
		ConversationManager->OnStreamChunk.RemoveAll(this);
		ConversationManager->OnToolCallStarted.RemoveAll(this);
		ConversationManager->OnToolCallCompleted.RemoveAll(this);
		ConversationManager->OnProcessingStarted.RemoveAll(this);
		ConversationManager->OnProcessingComplete.RemoveAll(this);
		ConversationManager->OnError.RemoveAll(this);
		ConversationManager->OnConfirmationRequired.RemoveAll(this);
	}
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

		// Focus Profile Dropdown
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4, 0)
		[
			BuildFocusDropdown()
		]

		// Safety Preset
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4, 0)
		[
			BuildSafetyPresetToggle()
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

TSharedRef<SWidget> SOliveAIChatPanel::BuildFocusDropdown()
{
	return SAssignNew(FocusDropdown, SComboBox<TSharedPtr<FString>>)
		.OptionsSource(&FocusProfiles)
		.InitiallySelectedItem(CurrentFocusProfile)
		.OnSelectionChanged(this, &SOliveAIChatPanel::OnFocusProfileChanged)
		.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
		{
			if (!Item.IsValid())
			{
				return SNew(STextBlock).Text(FText::GetEmpty());
			}

			const TOptional<FOliveFocusProfile> Profile = FOliveFocusProfileManager::Get().GetProfile(*Item);
			const FText Display = Profile.IsSet() ? Profile->DisplayName : FText::FromString(*Item);
			const FText Tooltip = Profile.IsSet() ? Profile->Description : FText::GetEmpty();

			return SNew(STextBlock)
				.Text(Display)
				.ToolTipText(Tooltip);
		})
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				if (!CurrentFocusProfile.IsValid())
				{
					return FText::GetEmpty();
				}

				const TOptional<FOliveFocusProfile> Profile = FOliveFocusProfileManager::Get().GetProfile(*CurrentFocusProfile);
				return Profile.IsSet() ? Profile->DisplayName : FText::FromString(*CurrentFocusProfile);
			})
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

		// Input Field
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SAssignNew(InputField, SOliveAIInputField)
			.OnMessageSubmit(this, &SOliveAIChatPanel::OnMessageSubmitted)
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

		// Send Button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4, 0, 0, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("Send", "Send"))
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

	// Refresh provider from current settings so key/provider changes apply immediately.
	FString ProviderError;
	if (!ConfigureProviderFromSettings(ProviderError))
	{
		HandleError(ProviderError);
		return;
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

void SOliveAIChatPanel::OnFocusProfileChanged(TSharedPtr<FString> NewProfile, ESelectInfo::Type SelectInfo)
{
	if (!NewProfile.IsValid())
	{
		return;
	}

	const FString NormalizedProfile = FOliveFocusProfileManager::Get().NormalizeProfileName(*NewProfile);
	for (const TSharedPtr<FString>& ProfileOption : FocusProfiles)
	{
		if (ProfileOption.IsValid() && ProfileOption->Equals(NormalizedProfile, ESearchCase::IgnoreCase))
		{
			CurrentFocusProfile = ProfileOption;
			break;
		}
	}

	if (ConversationManager.IsValid())
	{
		ConversationManager->SetFocusProfile(NormalizedProfile);
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

	FString ProviderName;
	switch (Settings->Provider)
	{
	case EOliveAIProvider::ClaudeCode:
		ProviderName = TEXT("Claude Code CLI");
		break;
	case EOliveAIProvider::OpenRouter:
		ProviderName = TEXT("OpenRouter");
		break;
	case EOliveAIProvider::Anthropic:
		ProviderName = TEXT("Anthropic");
		break;
	case EOliveAIProvider::OpenAI:
		ProviderName = TEXT("OpenAI");
		break;
	case EOliveAIProvider::Google:
		ProviderName = TEXT("Google");
		break;
	case EOliveAIProvider::Ollama:
		ProviderName = TEXT("Ollama");
		break;
	case EOliveAIProvider::OpenAICompatible:
		ProviderName = TEXT("OpenAI Compatible");
		break;
	default:
		ProviderName = TEXT("OpenRouter");
		break;
	}

	TSharedPtr<IOliveAIProvider> Provider = FOliveProviderFactory::CreateProvider(ProviderName);
	if (!Provider.IsValid())
	{
		OutError = FString::Printf(TEXT("Provider '%s' is unavailable."), *ProviderName);
		ConversationManager->SetProvider(nullptr);
		return false;
	}

	FOliveProviderConfig ProviderConfig;
	ProviderConfig.ProviderName = ProviderName;
	ProviderConfig.ApiKey = Settings->GetCurrentApiKey();
	ProviderConfig.BaseUrl = Settings->GetCurrentBaseUrl();
	ProviderConfig.ModelId = Settings->SelectedModel;
	ProviderConfig.Temperature = Settings->Temperature;
	ProviderConfig.MaxTokens = Settings->MaxTokens;
	ProviderConfig.TimeoutSeconds = Settings->RequestTimeoutSeconds;
	Provider->Configure(ProviderConfig);

	ConversationManager->SetProvider(Provider);

	// Trigger async connection validation
	bIsValidating = true;
	ValidationMessage = TEXT("Checking connection...");
	Provider->ValidateConnection([this](bool bSuccess, const FString& Message)
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

	if (InputField.IsValid())
	{
		InputField->SetEnabled(false);
	}
}

void SOliveAIChatPanel::HandleProcessingComplete()
{
	bIsProcessing = false;

	if (MessageList.IsValid())
	{
		MessageList->CompleteLastMessage();
	}

	if (InputField.IsValid())
	{
		InputField->SetEnabled(true);
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

void SOliveAIChatPanel::HandleConfirmationRequired(const FString& ToolCallId, const FString& ToolName, const FString& Plan)
{
	if (!MessageList.IsValid() || !ConversationManager.IsValid())
	{
		return;
	}

	TWeakPtr<FOliveConversationManager> WeakManager = ConversationManager;

	SOliveAIMessageList::FOnConfirmationAction OnAction;
	OnAction.BindLambda([WeakManager](bool bConfirmed)
	{
		if (TSharedPtr<FOliveConversationManager> Manager = WeakManager.Pin())
		{
			if (bConfirmed)
			{
				Manager->ConfirmPendingOperation();
			}
			else
			{
				Manager->DenyPendingOperation();
			}
		}
	});

	FString DisplayPlan = FString::Printf(TEXT("[%s] %s"), *ToolName, *Plan);
	MessageList->AddConfirmationWidget(ToolCallId, DisplayPlan, OnAction);
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
		if (bValidationSuccess && !ValidationMessage.IsEmpty())
		{
			return FText::Format(LOCTEXT("StatusReadyValidated", "{0} - {1}"), FText::FromString(ProviderName), FText::FromString(ValidationMessage));
		}
		return FText::Format(LOCTEXT("StatusReady", "Ready - {0}"), FText::FromString(ProviderName));
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
	return !bIsProcessing && ConversationManager.IsValid() && ConversationManager->GetProvider().IsValid();
}

// ==========================================
// Safety Preset Toggle
// ==========================================

TSharedRef<SWidget> SOliveAIChatPanel::BuildSafetyPresetToggle()
{
	return SNew(SComboBox<TSharedPtr<FString>>)
		.OptionsSource(&SafetyPresetOptions)
		.InitiallySelectedItem(CurrentSafetyPreset)
		.OnSelectionChanged(this, &SOliveAIChatPanel::OnSafetyPresetChanged)
		.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
		{
			return SNew(STextBlock).Text(FText::FromString(*Item));
		})
		.ToolTipText(LOCTEXT("SafetyTooltip", "Safety Preset: Controls confirmation requirements for write operations"))
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return CurrentSafetyPreset.IsValid() ? FText::FromString(*CurrentSafetyPreset) : FText::GetEmpty();
			})
			.ColorAndOpacity(this, &SOliveAIChatPanel::GetSafetyPresetColor)
		];
}

void SOliveAIChatPanel::OnSafetyPresetChanged(TSharedPtr<FString> NewPreset, ESelectInfo::Type SelectInfo)
{
	CurrentSafetyPreset = NewPreset;
	if (NewPreset.IsValid())
	{
		EOliveSafetyPreset Preset = EOliveSafetyPreset::Careful;
		if (*NewPreset == TEXT("Fast"))
		{
			Preset = EOliveSafetyPreset::Fast;
		}
		else if (*NewPreset == TEXT("YOLO"))
		{
			Preset = EOliveSafetyPreset::YOLO;
		}
		UOliveAISettings::Get()->SetSafetyPreset(Preset);
	}
}

FSlateColor SOliveAIChatPanel::GetSafetyPresetColor() const
{
	if (!CurrentSafetyPreset.IsValid())
	{
		return FLinearColor::White;
	}

	if (*CurrentSafetyPreset == TEXT("Careful"))
	{
		return FLinearColor(0.2f, 0.8f, 0.2f);
	}
	else if (*CurrentSafetyPreset == TEXT("Fast"))
	{
		return FLinearColor(0.9f, 0.8f, 0.1f);
	}
	else if (*CurrentSafetyPreset == TEXT("YOLO"))
	{
		return FLinearColor(0.9f, 0.2f, 0.2f);
	}

	return FLinearColor::White;
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
