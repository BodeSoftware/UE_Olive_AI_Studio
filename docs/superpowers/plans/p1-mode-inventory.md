# P1 Mode Removal — Inventory

Generated 2026-04-18. Source of truth: grep against `Source/` for mode-related symbols.

## Impact Surface (19 files)

### Headers
- `Source/OliveAIEditor/Public/Brain/OliveBrainState.h` — `EOliveChatMode` enum, `LexToString`, `ChatModeFromConfig`
- `Source/OliveAIEditor/Public/Brain/OliveToolExecutionContext.h` — `ChatMode` field
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` — `EOliveChatModeConfig` enum, `DefaultChatMode` property
- `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h` — `SetChatMode`, `GetChatMode`, `ActiveChatMode`, `RequestChatMode`, `DeferredChatMode`, delegates
- `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h` — `GetModeSuffix`, mode params
- `Source/OliveAIEditor/Public/MCP/OliveMCPServer.h` — `SetChatModeForInternalAgent`, `InternalAgentChatMode`
- `Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h` — `GetToolsForMode`
- `Source/OliveAIEditor/Public/UI/SOliveAIChatPanel.h` — `HandleModeChanged`, `HandleModeSwitchDeferred`
- `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h` — `EOliveWriteStage::ModeGate`, `FOliveWriteRequest::ChatMode`, `StageModeGate()`

### Sources
- `Source/OliveAIEditor/Private/Settings/OliveAISettings.cpp` — logs `DefaultChatMode`
- `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` — 62 hits; extensive mode logic
- `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp` — mode suffix
- `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp` — internal agent chat mode
- `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp` — `GetToolsForMode`
- `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp` — 30 hits; mode badge, slash commands
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` — uses `bFromMCP` (MUST KEEP — used for confirmation bypass)
- `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp` — Stage 2 Mode Gate implementation

### Tests
- `Source/OliveAIEditor/Private/Tests/Brain/OliveChatModeTests.cpp` — DELETE entirely (82 hits)
- `Source/OliveAIEditor/Private/Tests/Conversation/OliveConversationManagerTests.cpp` — strip mode test blocks (15 hits)

## Config

- `Config/DefaultOliveAI.ini:21` — `DefaultChatMode=Code` (leave; UE ignores unknown ini entries)

## Notes

1. **KEEP `bFromMCP` field** — it gates MCP vs in-editor confirmation flows in ~40 call sites across `OliveBlueprintToolHandlers.cpp`. Not exclusively mode-related.
2. **KEEP the `OnModeChanged` delegate bodies only if removed safely** — the chat panel may listen.
3. Broadcast delegates `FOnOliveChatModeChanged`, `FOnOliveChatModeSwitchDeferred` — remove both.
4. `GetToolsForMode(Mode)` in `OliveToolRegistry` — simplify to mode-less `GetAllTools()` or replace callers. Used by Ask mode to filter write tools.
