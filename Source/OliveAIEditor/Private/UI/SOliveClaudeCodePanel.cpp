// Copyright Bode Software. All Rights Reserved.

/**
 * SOliveClaudeCodePanel implementation.
 *
 * Companion panel for Claude Code / MCP integration. Provides server status,
 * setup instructions, and a live activity feed of tool calls. Refreshes at 1Hz
 * for status and subscribes to MCP server delegates for real-time activity.
 */

#include "UI/SOliveClaudeCodePanel.h"
#include "MCP/OliveMCPServer.h"
#include "Settings/OliveAISettings.h"
#include "OliveAIEditorModule.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "OliveClaudeCodePanel"

const FName SOliveClaudeCodePanel::TabId(TEXT("OliveClaudeCodeTab"));

// ==========================================
// Lifecycle
// ==========================================

void SOliveClaudeCodePanel::Construct(const FArguments& InArgs)
{
	// Populate activity from existing history
	TArray<FOliveMCPServer::FRecentToolCall> History = FOliveMCPServer::Get().GetRecentToolCalls(MaxActivityEntries);
	for (const FOliveMCPServer::FRecentToolCall& Call : History)
	{
		TSharedPtr<FActivityEntry> Entry = MakeShared<FActivityEntry>();
		Entry->Timestamp = Call.Timestamp;
		Entry->ToolName = Call.ToolName;
		Entry->ClientId = Call.ClientId;
		Entry->bSuccess = Call.bSuccess;
		Entry->DurationMs = Call.DurationMs;
		Entry->bCompleted = true;
		ActivityItems.Add(Entry);
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SScrollBox)

			// Header
			+ SScrollBox::Slot()
			.Padding(FMargin(0, 0, 0, 4))
			[
				BuildHeader()
			]

			+ SScrollBox::Slot()
			.Padding(FMargin(0, 0, 0, 2))
			[
				SNew(SSeparator)
			]

			// Server Status
			+ SScrollBox::Slot()
			.Padding(FMargin(0, 4, 0, 4))
			[
				BuildServerStatus()
			]

			+ SScrollBox::Slot()
			.Padding(FMargin(0, 0, 0, 2))
			[
				SNew(SSeparator)
			]

			// Setup Instructions
			+ SScrollBox::Slot()
			.Padding(FMargin(0, 4, 0, 4))
			[
				BuildSetupInstructions()
			]

			+ SScrollBox::Slot()
			.Padding(FMargin(0, 0, 0, 2))
			[
				SNew(SSeparator)
			]

			// Activity Feed
			+ SScrollBox::Slot()
			.Padding(FMargin(0, 4, 0, 0))
			[
				BuildActivityFeed()
			]
		]
	];

	// Subscribe to MCP server events
	OnToolCalledHandle = FOliveMCPServer::Get().OnToolCalled.AddRaw(
		this, &SOliveClaudeCodePanel::HandleToolCalled);
	OnToolCompletedHandle = FOliveMCPServer::Get().OnToolCompleted.AddRaw(
		this, &SOliveClaudeCodePanel::HandleToolCompleted);

	// Start 1Hz status refresh timer
	if (GEditor && GEditor->GetEditorWorldContext().World())
	{
		GEditor->GetEditorWorldContext().World()->GetTimerManager().SetTimer(
			RefreshTimerHandle,
			FTimerDelegate::CreateRaw(this, &SOliveClaudeCodePanel::RefreshStatus),
			1.0f,
			true
		);
	}

	// Initial status refresh
	RefreshStatus();
}

SOliveClaudeCodePanel::~SOliveClaudeCodePanel()
{
	// Unsubscribe from MCP events
	FOliveMCPServer::Get().OnToolCalled.Remove(OnToolCalledHandle);
	FOliveMCPServer::Get().OnToolCompleted.Remove(OnToolCompletedHandle);

	// Clear timer
	if (RefreshTimerHandle.IsValid() && GEditor && GEditor->GetEditorWorldContext().World())
	{
		GEditor->GetEditorWorldContext().World()->GetTimerManager().ClearTimer(RefreshTimerHandle);
	}
}

// ==========================================
// Section Builders
// ==========================================

TSharedRef<SWidget> SOliveClaudeCodePanel::BuildHeader()
{
	return SNew(SBox)
		.Padding(FMargin(8, 6))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PanelTitle", "Claude Code Integration"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
		];
}

TSharedRef<SWidget> SOliveClaudeCodePanel::BuildServerStatus()
{
	return SNew(SBox)
		.Padding(FMargin(8, 4))
		[
			SNew(SVerticalBox)

			// Section title
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0, 0, 0, 6))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ServerStatusTitle", "MCP Server Status"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
			]

			// Status row: indicator + text
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0, 0, 0, 4))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0, 0, 6, 0))
				[
					SAssignNew(StatusTextBlock, STextBlock)
					.Text(LOCTEXT("ServerStatusInitial", "Stopped"))
					.ColorAndOpacity(FSlateColor(FLinearColor::Red))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				]
			]

			// Port row
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0, 0, 0, 2))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(0, 0, 4, 0))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PortLabel", "Port:"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SAssignNew(PortTextBlock, STextBlock)
					.Text(LOCTEXT("PortNone", "--"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				]
			]

			// Client count row
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0, 0, 0, 6))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(0, 0, 4, 0))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ClientsLabel", "Connected Clients:"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SAssignNew(ClientCountTextBlock, STextBlock)
					.Text(LOCTEXT("ClientsNone", "0"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				]
			]

			// Start/Stop button
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.OnClicked(this, &SOliveClaudeCodePanel::OnToggleServer)
				[
					SAssignNew(ToggleButtonTextBlock, STextBlock)
					.Text(LOCTEXT("StartServer", "Start Server"))
				]
			]
		];
}

TSharedRef<SWidget> SOliveClaudeCodePanel::BuildSetupInstructions()
{
	// Resolve the plugin path for bridge instructions
	const FString PluginDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
	const FString BridgePath = FPaths::Combine(PluginDir, TEXT("mcp-bridge.js"));
	FString BridgePathDisplay = BridgePath;
	BridgePathDisplay.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Build the .mcp.json content
	const FString McpJsonContent = FString::Printf(
		TEXT("{\n")
		TEXT("  \"mcpServers\": {\n")
		TEXT("    \"olive_ai_studio\": {\n")
		TEXT("      \"command\": \"node\",\n")
		TEXT("      \"args\": [\"%s\"]\n")
		TEXT("    }\n")
		TEXT("  }\n")
		TEXT("}"),
		*BridgePathDisplay
	);

	// Build the claude mcp add command
	const FString McpAddCommand = FString::Printf(
		TEXT("claude mcp add olive_ai_studio -- node \"%s\""),
		*BridgePathDisplay
	);

	return SNew(SBox)
		.Padding(FMargin(8, 4))
		[
			SNew(SVerticalBox)

			// Section title
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0, 0, 0, 6))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SetupTitle", "Setup Instructions"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
			]

			// .mcp.json subsection
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0, 0, 0, 2))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("McpJsonLabel", "Option 1: Place .mcp.json in your project root"))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0, 0, 0, 4))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
					.Padding(FMargin(6, 4))
					[
						SNew(SMultiLineEditableText)
						.Text(FText::FromString(McpJsonContent))
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
						.AutoWrapText(true)
						.IsReadOnly(true)
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(FMargin(4, 0, 0, 0))
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyJson", "Copy"))
					.ToolTipText(LOCTEXT("CopyJsonTip", "Copy .mcp.json content to clipboard"))
					.OnClicked(this, &SOliveClaudeCodePanel::OnCopyMcpJson)
				]
			]

			// CLI command subsection
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0, 4, 0, 2))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CliLabel", "Option 2: Run this command in your terminal"))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
					.Padding(FMargin(6, 4))
					[
						SNew(SMultiLineEditableText)
						.Text(FText::FromString(McpAddCommand))
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
						.AutoWrapText(true)
						.IsReadOnly(true)
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Top)
				.Padding(FMargin(4, 0, 0, 0))
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyCli", "Copy"))
					.ToolTipText(LOCTEXT("CopyCliTip", "Copy claude mcp add command to clipboard"))
					.OnClicked(this, &SOliveClaudeCodePanel::OnCopyMcpAddCommand)
				]
			]
		];
}

TSharedRef<SWidget> SOliveClaudeCodePanel::BuildActivityFeed()
{
	return SNew(SVerticalBox)

		// Section title
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8, 0, 8, 6))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ActivityTitle", "Activity Feed"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
		]

		// Column headers
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8, 0, 8, 2))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(0.22f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ColTime", "Time"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(0.45f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ColTool", "Tool"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(0.15f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ColStatus", "Status"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(0.18f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ColDuration", "Duration"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]
		]

		// List
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(FMargin(8, 0, 8, 0))
		[
			SAssignNew(ActivityListView, SListView<TSharedPtr<FActivityEntry>>)
			.ListItemsSource(&ActivityItems)
			.OnGenerateRow(this, &SOliveClaudeCodePanel::GenerateActivityRow)
			.SelectionMode(ESelectionMode::None)
		];
}

// ==========================================
// Activity Feed Row Generation
// ==========================================

TSharedRef<ITableRow> SOliveClaudeCodePanel::GenerateActivityRow(
	TSharedPtr<FActivityEntry> Item,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	// Format timestamp as HH:MM:SS
	const FString TimeStr = Item->Timestamp.ToString(TEXT("%H:%M:%S"));

	// Status text and color
	FText StatusText;
	FSlateColor StatusColor;
	if (!Item->bCompleted)
	{
		StatusText = LOCTEXT("ToolStatusRunning", "Running...");
		StatusColor = FSlateColor(FLinearColor(1.0f, 0.8f, 0.2f)); // Yellow
	}
	else if (Item->bSuccess)
	{
		StatusText = LOCTEXT("ToolStatusSuccess", "OK");
		StatusColor = FSlateColor(FLinearColor(0.2f, 0.8f, 0.2f)); // Green
	}
	else
	{
		StatusText = LOCTEXT("ToolStatusFailed", "Failed");
		StatusColor = FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)); // Red
	}

	// Duration text
	FText DurationText;
	if (Item->bCompleted)
	{
		if (Item->DurationMs < 1000.0)
		{
			DurationText = FText::Format(LOCTEXT("DurationMs", "{0}ms"),
				FText::AsNumber(FMath::RoundToInt(Item->DurationMs)));
		}
		else
		{
			FNumberFormattingOptions FmtOpts;
			FmtOpts.SetMaximumFractionalDigits(1);
			FmtOpts.SetMinimumFractionalDigits(1);
			DurationText = FText::Format(LOCTEXT("DurationSec", "{0}s"),
				FText::AsNumber(Item->DurationMs / 1000.0, &FmtOpts));
		}
	}
	else
	{
		DurationText = LOCTEXT("DurationPending", "--");
	}

	return SNew(STableRow<TSharedPtr<FActivityEntry>>, OwnerTable)
		[
			SNew(SHorizontalBox)

			// Timestamp
			+ SHorizontalBox::Slot()
			.FillWidth(0.22f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(0, 2))
			[
				SNew(STextBlock)
				.Text(FText::FromString(TimeStr))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]

			// Tool name
			+ SHorizontalBox::Slot()
			.FillWidth(0.45f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(0, 2))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->ToolName))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			]

			// Status
			+ SHorizontalBox::Slot()
			.FillWidth(0.15f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(0, 2))
			[
				SNew(STextBlock)
				.Text(StatusText)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(StatusColor)
			]

			// Duration
			+ SHorizontalBox::Slot()
			.FillWidth(0.18f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(0, 2))
			[
				SNew(STextBlock)
				.Text(DurationText)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]
		];
}

// ==========================================
// Status Polling
// ==========================================

void SOliveClaudeCodePanel::RefreshStatus()
{
	const FOliveMCPServer& Server = FOliveMCPServer::Get();
	const bool bRunning = Server.IsRunning();

	if (StatusTextBlock.IsValid())
	{
		if (bRunning)
		{
			StatusTextBlock->SetText(LOCTEXT("ServerRunning", "Running"));
			StatusTextBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.2f, 0.8f, 0.2f)));
		}
		else
		{
			StatusTextBlock->SetText(LOCTEXT("ServerStopped", "Stopped"));
			StatusTextBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)));
		}
	}

	if (PortTextBlock.IsValid())
	{
		if (bRunning)
		{
			PortTextBlock->SetText(FText::AsNumber(Server.GetActualPort()));
		}
		else
		{
			PortTextBlock->SetText(LOCTEXT("PortNone", "--"));
		}
	}

	if (ClientCountTextBlock.IsValid())
	{
		ClientCountTextBlock->SetText(FText::AsNumber(Server.GetConnectedClientCount()));
	}

	if (ToggleButtonTextBlock.IsValid())
	{
		if (bRunning)
		{
			ToggleButtonTextBlock->SetText(LOCTEXT("StopServer", "Stop Server"));
		}
		else
		{
			ToggleButtonTextBlock->SetText(LOCTEXT("StartServer", "Start Server"));
		}
	}
}

// ==========================================
// Activity Feed Handlers
// ==========================================

void SOliveClaudeCodePanel::HandleToolCalled(const FString& ToolName, const FString& ClientId,
                                              const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FActivityEntry> Entry = MakeShared<FActivityEntry>();
	Entry->Timestamp = FDateTime::UtcNow();
	Entry->ToolName = ToolName;
	Entry->ClientId = ClientId;
	Entry->bCompleted = false;

	// Insert at front (newest first)
	ActivityItems.Insert(Entry, 0);

	// Trim if over capacity
	while (ActivityItems.Num() > MaxActivityEntries)
	{
		ActivityItems.RemoveAt(ActivityItems.Num() - 1);
	}

	if (ActivityListView.IsValid())
	{
		ActivityListView->RequestListRefresh();
	}
}

void SOliveClaudeCodePanel::HandleToolCompleted(const FString& ToolName, const FString& ClientId,
                                                 bool bSuccess, double DurationMs)
{
	// Find the matching in-flight entry (search from front since newest is first)
	bool bFound = false;
	for (TSharedPtr<FActivityEntry>& Entry : ActivityItems)
	{
		if (!Entry->bCompleted && Entry->ToolName == ToolName && Entry->ClientId == ClientId)
		{
			Entry->bSuccess = bSuccess;
			Entry->DurationMs = DurationMs;
			Entry->bCompleted = true;
			bFound = true;
			break;
		}
	}

	// If no matching in-flight entry, add a completed entry (can happen if panel
	// was opened after the tool call started)
	if (!bFound)
	{
		TSharedPtr<FActivityEntry> Entry = MakeShared<FActivityEntry>();
		Entry->Timestamp = FDateTime::UtcNow();
		Entry->ToolName = ToolName;
		Entry->ClientId = ClientId;
		Entry->bSuccess = bSuccess;
		Entry->DurationMs = DurationMs;
		Entry->bCompleted = true;

		ActivityItems.Insert(Entry, 0);

		while (ActivityItems.Num() > MaxActivityEntries)
		{
			ActivityItems.RemoveAt(ActivityItems.Num() - 1);
		}
	}

	if (ActivityListView.IsValid())
	{
		ActivityListView->RequestListRefresh();
	}
}

// ==========================================
// Clipboard Helpers
// ==========================================

FReply SOliveClaudeCodePanel::OnCopyMcpJson()
{
	const FString PluginDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
	FString BridgePath = FPaths::Combine(PluginDir, TEXT("mcp-bridge.js"));
	BridgePath.ReplaceInline(TEXT("\\"), TEXT("/"));

	const FString McpJson = FString::Printf(
		TEXT("{\n")
		TEXT("  \"mcpServers\": {\n")
		TEXT("    \"olive_ai_studio\": {\n")
		TEXT("      \"command\": \"node\",\n")
		TEXT("      \"args\": [\"%s\"]\n")
		TEXT("    }\n")
		TEXT("  }\n")
		TEXT("}"),
		*BridgePath
	);

	FPlatformApplicationMisc::ClipboardCopy(*McpJson);
	return FReply::Handled();
}

FReply SOliveClaudeCodePanel::OnCopyMcpAddCommand()
{
	const FString PluginDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
	FString BridgePath = FPaths::Combine(PluginDir, TEXT("mcp-bridge.js"));
	BridgePath.ReplaceInline(TEXT("\\"), TEXT("/"));

	const FString Command = FString::Printf(
		TEXT("claude mcp add olive_ai_studio -- node \"%s\""),
		*BridgePath
	);

	FPlatformApplicationMisc::ClipboardCopy(*Command);
	return FReply::Handled();
}

// ==========================================
// Server Control
// ==========================================

FReply SOliveClaudeCodePanel::OnToggleServer()
{
	FOliveMCPServer& Server = FOliveMCPServer::Get();

	if (Server.IsRunning())
	{
		Server.Stop();
		UE_LOG(LogOliveAI, Log, TEXT("MCP Server stopped from companion panel"));
	}
	else
	{
		// Use the configured port from settings, defaulting to 3000
		int32 Port = 3000;
		if (const UOliveAISettings* Settings = UOliveAISettings::Get())
		{
			Port = Settings->MCPServerPort;
		}

		const bool bStarted = Server.Start(Port);
		if (bStarted)
		{
			UE_LOG(LogOliveAI, Log, TEXT("MCP Server started from companion panel on port %d"), Server.GetActualPort());
		}
		else
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Failed to start MCP Server from companion panel"));
		}
	}

	// Immediate refresh
	RefreshStatus();

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
