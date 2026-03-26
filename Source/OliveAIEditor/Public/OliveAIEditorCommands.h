// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Styling/AppStyle.h"

/**
 * Editor commands for Olive AI Studio
 */
class OLIVEAIEDITOR_API FOliveAIEditorCommands : public TCommands<FOliveAIEditorCommands>
{
public:
	FOliveAIEditorCommands()
		: TCommands<FOliveAIEditorCommands>(
			TEXT("OliveAI"),
			NSLOCTEXT("Contexts", "OliveAI", "Olive AI Studio"),
			NAME_None,
			FAppStyle::GetAppStyleSetName()
		)
	{
	}

	/** Initialize commands */
	virtual void RegisterCommands() override;

	/** Command to open the chat panel */
	TSharedPtr<FUICommandInfo> OpenChatPanel;

	/** Command to toggle the MCP server */
	TSharedPtr<FUICommandInfo> ToggleMCPServer;

	/** Command to open settings */
	TSharedPtr<FUICommandInfo> OpenSettings;

	/** Command to clear chat history */
	TSharedPtr<FUICommandInfo> ClearChatHistory;

	/** Command to start a new chat session */
	TSharedPtr<FUICommandInfo> NewChatSession;
};
