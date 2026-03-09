# Coder Agent Memory

## FOliveToolResult struct
- Field for result data is `Data` (TSharedPtr<FJsonObject>), NOT `ResultData`
- The architect's plan used `ResultData` which doesn't exist -- always verify struct fields before using them
- Defined in `Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h` line 48

## FOliveStreamChunk struct
- No `bIsFirst` field -- architect's plan was wrong about this
- Fields: Text, bIsToolCall, ToolCallId, ToolName, ToolArguments, bIsComplete, FinishReason
- Defined in `Source/OliveAIEditor/Public/Providers/IOliveAIProvider.h` line 54

## IPluginManager
- Include: `#include "Interfaces/IPluginManager.h"`
- `IPluginManager::Get().FindPlugin(TEXT("PluginName"))` returns `TSharedPtr<IPlugin>`
- Use `Plugin->GetBaseDir()` to get plugin root directory

## BoolProp helper in OliveBlueprintSchemas.cpp
- Signature: `BoolProp(const FString& Description, bool DefaultValue)` at line 81
- Returns `TSharedPtr<FJsonObject>` with type=boolean, description, default fields

## Knowledge pack files
- Located at `Content/SystemPrompts/Knowledge/`
- Files: blueprint_authoring.txt, blueprint_design_patterns.txt, cli_blueprint.txt, events_vs_functions.txt, node_routing.txt, recipe_routing.txt, recipes/
