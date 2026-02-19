// Copyright Bode Software. All Rights Reserved.

#include "OliveAIEditorCommands.h"

#define LOCTEXT_NAMESPACE "FOliveAIEditorCommands"

void FOliveAIEditorCommands::RegisterCommands()
{
	UI_COMMAND(
		OpenChatPanel,
		"Olive AI Chat",
		"Open the Olive AI Chat panel",
		EUserInterfaceActionType::Button,
		FInputChord(EModifierKey::Control | EModifierKey::Shift, EKeys::O)
	);

	UI_COMMAND(
		ToggleMCPServer,
		"Toggle MCP Server",
		"Start or stop the MCP server for external agent connections",
		EUserInterfaceActionType::ToggleButton,
		FInputChord()
	);

	UI_COMMAND(
		OpenSettings,
		"Olive AI Settings",
		"Open Olive AI Studio settings",
		EUserInterfaceActionType::Button,
		FInputChord()
	);

	UI_COMMAND(
		ClearChatHistory,
		"Clear Chat",
		"Clear the current chat history",
		EUserInterfaceActionType::Button,
		FInputChord()
	);

	UI_COMMAND(
		NewChatSession,
		"New Session",
		"Start a new chat session",
		EUserInterfaceActionType::Button,
		FInputChord()
	);
}

#undef LOCTEXT_NAMESPACE
