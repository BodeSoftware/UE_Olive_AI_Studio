// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Views/SListView.h"

class FJsonObject;

/**
 * SOliveClaudeCodePanel
 *
 * Companion panel for Claude Code integration. Shows:
 * - MCP server status (running/stopped, port, connected clients)
 * - Setup instructions with copyable .mcp.json and CLI command
 * - Live activity feed of recent tool calls with timestamps, status, and duration
 *
 * This panel is independent of the main chat panel. It provides visibility
 * into what external agents (Claude Code, Cursor, etc.) are doing via MCP.
 */
class OLIVEAIEDITOR_API SOliveClaudeCodePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOliveClaudeCodePanel) {}
	SLATE_END_ARGS()

	/** Tab ID for nomad tab spawner */
	static const FName TabId;

	/** Construct the widget */
	void Construct(const FArguments& InArgs);

	/** Destructor -- unsubscribes from delegates and clears timers */
	virtual ~SOliveClaudeCodePanel();

private:
	// ==========================================
	// Section Builders
	// ==========================================

	/** Build the header area with title */
	TSharedRef<SWidget> BuildHeader();

	/** Build the server status section (running/stopped, port, client count, start/stop button) */
	TSharedRef<SWidget> BuildServerStatus();

	/** Build setup instructions section with copyable .mcp.json and CLI command */
	TSharedRef<SWidget> BuildSetupInstructions();

	/** Build the activity feed section (SListView of recent tool calls) */
	TSharedRef<SWidget> BuildActivityFeed();

	// ==========================================
	// Status Polling
	// ==========================================

	/** Tick-driven status refresh (1Hz via FTimerHandle) */
	void RefreshStatus();

	/** Timer handle for 1Hz status refresh */
	FTimerHandle RefreshTimerHandle;

	// ==========================================
	// Activity Feed
	// ==========================================

	/** Activity entry for the feed */
	struct FActivityEntry
	{
		FDateTime Timestamp;
		FString ToolName;
		FString ClientId;
		bool bSuccess = true;
		double DurationMs = 0.0;
		bool bCompleted = false;
	};

	/**
	 * Handle tool call started from MCP server (binds to OnToolCalled).
	 * Adds an in-flight entry to the activity feed.
	 */
	void HandleToolCalled(const FString& ToolName, const FString& ClientId,
	                      const TSharedPtr<FJsonObject>& Arguments);

	/**
	 * Handle tool completion from MCP server (binds to OnToolCompleted).
	 * Updates the matching in-flight entry or adds a completed entry.
	 */
	void HandleToolCompleted(const FString& ToolName, const FString& ClientId,
	                         bool bSuccess, double DurationMs);

	/** Generate a single row widget for the activity list */
	TSharedRef<ITableRow> GenerateActivityRow(
		TSharedPtr<FActivityEntry> Item,
		const TSharedRef<STableViewBase>& OwnerTable);

	// ==========================================
	// Clipboard Helpers
	// ==========================================

	/** Copy .mcp.json content to clipboard */
	FReply OnCopyMcpJson();

	/** Copy `claude mcp add` CLI command to clipboard */
	FReply OnCopyMcpAddCommand();

	// ==========================================
	// Server Control
	// ==========================================

	/** Handle start/stop button click */
	FReply OnToggleServer();

	// ==========================================
	// State
	// ==========================================

	/** Activity log ring buffer, max MaxActivityEntries */
	TArray<TSharedPtr<FActivityEntry>> ActivityItems;

	/** Maximum activity entries to keep */
	static constexpr int32 MaxActivityEntries = 100;

	// ==========================================
	// Child Widgets
	// ==========================================

	/** The activity feed list view */
	TSharedPtr<SListView<TSharedPtr<FActivityEntry>>> ActivityListView;

	/** Cached status text blocks for dynamic updates */
	TSharedPtr<STextBlock> StatusTextBlock;
	TSharedPtr<STextBlock> PortTextBlock;
	TSharedPtr<STextBlock> ClientCountTextBlock;
	TSharedPtr<STextBlock> ToggleButtonTextBlock;

	// ==========================================
	// Delegate Handles
	// ==========================================

	FDelegateHandle OnToolCalledHandle;
	FDelegateHandle OnToolCompletedHandle;
};
