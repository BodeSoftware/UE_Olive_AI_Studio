// Copyright Bode Software. All Rights Reserved.

#include "UI/SOliveAIChatPanel.h"
#include "UI/SOliveAIMessageList.h"
#include "UI/SOliveAIContextBar.h"
#include "UI/SOliveAIInputField.h"
#include "Chat/OliveConversationManager.h"
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
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "ISettingsModule.h"

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
	// Initialize focus profiles
	FocusProfiles.Add(MakeShared<FString>(TEXT("Auto")));
	FocusProfiles.Add(MakeShared<FString>(TEXT("Blueprint")));
	FocusProfiles.Add(MakeShared<FString>(TEXT("AI & Behavior")));
	FocusProfiles.Add(MakeShared<FString>(TEXT("Level & PCG")));
	FocusProfiles.Add(MakeShared<FString>(TEXT("C++ & Blueprint")));
	CurrentFocusProfile = FocusProfiles[0];

	// Create conversation manager
	ConversationManager = MakeShared<FOliveConversationManager>();

	// Set up provider from settings
	UOliveAISettings* Settings = UOliveAISettings::Get();
	if (Settings)
	{
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
		default:
			ProviderName = TEXT("OpenRouter");
			break;
		}

		TSharedPtr<IOliveAIProvider> Provider = FOliveProviderFactory::CreateProvider(ProviderName);
		if (Provider.IsValid())
		{
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
		}
	}

	// Set default system prompt
	ConversationManager->SetSystemPrompt(
		TEXT("You are Olive AI, an expert AI assistant for Unreal Engine development integrated directly into the editor.\n\n")
		TEXT("## Your Capabilities\n")
		TEXT("- Search the project for assets by name or type\n")
		TEXT("- Read and understand Blueprint structures\n")
		TEXT("- Get project configuration and class hierarchies\n\n")
		TEXT("## Guidelines\n")
		TEXT("- Be concise and helpful\n")
		TEXT("- Use tools to gather information before answering\n")
		TEXT("- If uncertain, ask clarifying questions\n")
	);

	// Bind conversation manager events
	ConversationManager->OnMessageAdded.AddSP(this, &SOliveAIChatPanel::HandleMessageAdded);
	ConversationManager->OnStreamChunk.AddSP(this, &SOliveAIChatPanel::HandleStreamChunk);
	ConversationManager->OnToolCallStarted.AddSP(this, &SOliveAIChatPanel::HandleToolCallStarted);
	ConversationManager->OnToolCallCompleted.AddSP(this, &SOliveAIChatPanel::HandleToolCallCompleted);
	ConversationManager->OnProcessingStarted.AddSP(this, &SOliveAIChatPanel::HandleProcessingStarted);
	ConversationManager->OnProcessingComplete.AddSP(this, &SOliveAIChatPanel::HandleProcessingComplete);
	ConversationManager->OnError.AddSP(this, &SOliveAIChatPanel::HandleError);

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
	if (ConversationManager.IsValid())
	{
		ConversationManager->OnMessageAdded.RemoveAll(this);
		ConversationManager->OnStreamChunk.RemoveAll(this);
		ConversationManager->OnToolCallStarted.RemoveAll(this);
		ConversationManager->OnToolCallCompleted.RemoveAll(this);
		ConversationManager->OnProcessingStarted.RemoveAll(this);
		ConversationManager->OnProcessingComplete.RemoveAll(this);
		ConversationManager->OnError.RemoveAll(this);
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
			return SNew(STextBlock).Text(FText::FromString(*Item));
		})
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return CurrentFocusProfile.IsValid() ? FText::FromString(*CurrentFocusProfile) : FText::GetEmpty();
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
	return SAssignNew(MessageList, SOliveAIMessageList);
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

		// Status indicator
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0, 0, 4, 0)
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush("Icons.FilledCircle"))
			.ColorAndOpacity(this, &SOliveAIChatPanel::GetStatusColor)
			.DesiredSizeOverride(FVector2D(8, 8))
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
	CurrentFocusProfile = NewProfile;
	if (ConversationManager.IsValid() && NewProfile.IsValid())
	{
		ConversationManager->SetFocusProfile(*NewProfile);
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
		FString Summary = Result.bSuccess ? TEXT("Success") : TEXT("Failed");
		MessageList->UpdateToolCallStatus(ToolCallId, Result.bSuccess, Summary);
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
	// TODO: Get currently selected asset in content browser or open editor
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

	if (ConversationManager.IsValid() && ConversationManager->GetProvider().IsValid())
	{
		FString ProviderName = ConversationManager->GetProvider()->GetProviderName();
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

#undef LOCTEXT_NAMESPACE
