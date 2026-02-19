# Olive AI Studio - Project Context

> **Plugin:** Olive AI Studio
> **Engine:** Unreal Engine 5.5+
> **Architecture:** Dual-interface AI assistant (Built-in Chat + MCP Server)
> **Type:** Editor-only plugin
>
> **IMPORTANT - Development Scope:**
> This plugin is being developed within `b:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\`
> The parent project (`UE_Olive_AI_Toolkit`) is ONLY for testing - do NOT access parent directories.
> All development work stays within the plugin directory. Permissions are enforced in `.claude/settings.json`.

---

Implementation Plan
The full plan is at plans/ue-ai-agent-plugin-plan-v2. Read it before making architectural decisions.

## Project Overview

Olive AI Studio is an AI-powered development assistant for Unreal Engine. It provides:

1. **Built-in Chat UI** - Dockable Slate panel inside the editor for most users
2. **MCP Server** - JSON-RPC server for external agents (Claude Code, Cursor, etc.)

Both interfaces share the same tool layer, validation, and execution engine.

### Core Principles

1. **Dual interface.** Built-in chat for accessibility, MCP for extensibility.
2. **Read-heavy, write-careful.** Reading is cheap. Every write uses transactions.
3. **Surgical edits over recreation.** Granular changes, not regenerating entire graphs.
4. **Show the work.** Operation feed shows every tool call in real-time.
5. **Fail safely.** Structured errors, suggestions, no asset corruption.

---

## Module Architecture

```
Source/
├── OliveAIRuntime/          # Minimal runtime module
│   └── IR/                  # Intermediate Representation structs only
│
└── OliveAIEditor/           # Editor-only module (everything else)
    ├── UI/                  # Slate widgets (chat panel, operation feed)
    ├── Chat/                # Conversation manager, prompt assembly
    ├── Providers/           # API clients (OpenRouter, Anthropic)
    ├── MCP/                 # MCP server, JSON-RPC, tool registry
    ├── Services/            # Transaction manager, validation, asset resolver
    ├── Index/               # Project index (asset search, class hierarchy)
    ├── Profiles/            # Focus profiles (tool filtering)
    ├── Settings/            # Configuration (UDeveloperSettings)
    ├── Blueprint/           # Blueprint read/write (Phase 1)
    ├── BehaviorTree/        # BT/Blackboard (Phase 2)
    ├── PCG/                 # PCG graphs (Phase 3)
    └── Cpp/                 # C++ integration (Phase 4)
```

### Module Dependencies

**OliveAIRuntime:**
- `Core`, `CoreUObject`, `Json`, `JsonUtilities`

**OliveAIEditor:**
- Core: `Core`, `CoreUObject`, `Engine`
- Editor: `UnrealEd`, `EditorFramework`, `EditorStyle`, `EditorSubsystem`, `ToolMenus`
- UI: `Slate`, `SlateCore`, `InputCore`
- Network: `HTTP`, `HttpServer`, `Sockets`
- Data: `Json`, `JsonUtilities`
- Assets: `AssetRegistry`, `AssetTools`, `ContentBrowser`
- Blueprint: `BlueprintGraph`, `Kismet`, `KismetWidgets`, `GraphEditor`
- Config: `Projects`, `DeveloperSettings`, `Settings`

---

## Coding Standards

### Naming Conventions

| Type | Prefix | Example |
|------|--------|---------|
| Classes (UObject-derived) | `U` | `UOliveAISettings` |
| Classes (AActor-derived) | `A` | `AOliveDebugActor` |
| Structs | `F` | `FOliveToolResult` |
| Interfaces | `I` | `IOliveAIProvider` |
| Enums | `E` | `EOliveOperationStatus` |
| Slate widgets | `S` | `SOliveAIChatPanel` |
| Delegates | `F...Delegate` | `FOnOliveStreamChunk` |
| Multicast delegates | `FOn...` | `FOnOliveToolCallCompleted` |

### File Naming

- Header/source pairs: `OliveClassName.h` / `OliveClassName.cpp`
- All files prefixed with `Olive` to avoid conflicts
- Slate widgets: `SOliveWidgetName.h`

### Code Style

```cpp
// Use UPROPERTY/UFUNCTION for reflected members
UPROPERTY(Config, EditAnywhere, Category="AI Provider")
FString ApiKey;

// Use GENERATED_BODY() in all UCLASS/USTRUCT
USTRUCT()
struct FOliveToolResult
{
    GENERATED_BODY()

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Data;
};

// Prefer TSharedPtr for non-UObject shared ownership
TSharedPtr<IOliveAIProvider> Provider;

// Use delegates for callbacks
DECLARE_DELEGATE_OneParam(FOnOliveStreamChunk, const FOliveStreamChunk&);

// Always check pointers
if (Provider.IsValid())
{
    Provider->SendMessage(...);
}

// Use FScopedTransaction for undo support
{
    FScopedTransaction Transaction(LOCTEXT("AddVariable", "Add Variable"));
    Blueprint->Modify();
    // ... modifications
}
```

### Logging

```cpp
// Define log category in module
DECLARE_LOG_CATEGORY_EXTERN(LogOliveAI, Log, All);

// Use appropriate levels
UE_LOG(LogOliveAI, Log, TEXT("MCP Server started on port %d"), Port);
UE_LOG(LogOliveAI, Warning, TEXT("Rate limited, retrying in %d seconds"), RetryAfter);
UE_LOG(LogOliveAI, Error, TEXT("Failed to connect: %s"), *ErrorMessage);
```

### Error Handling

```cpp
// Return structured errors, not just booleans
FOliveToolResult ExecuteTool(const FString& Name, const TSharedPtr<FJsonObject>& Params)
{
    FOliveToolResult Result;

    if (!Tools.Contains(Name))
    {
        Result.bSuccess = false;
        Result.Error = FOliveErrorBuilder::Error(
            TEXT("TOOL_NOT_FOUND"),
            FString::Printf(TEXT("Tool '%s' not found"), *Name),
            TEXT("Use tools/list to see available tools")
        );
        return Result;
    }

    // ... execute
}
```

---

## Key Design Decisions

### Confirmation Tiers

| Tier | Risk | Operations | UX |
|------|------|------------|-----|
| 1 | Low | Add variable, add component, create empty BP | Auto-execute |
| 2 | Medium | Create function with logic, wire event graph | Plan → Confirm |
| 3 | High | Refactor, delete, reparent | Non-destructive preview |

### Provider Architecture

```
IProviderClient (interface)
├── FOpenRouterClient    (Phase 0 - primary)
├── FAnthropicClient     (Phase 0 - direct Claude)
├── FOpenAIClient        (Phase 1)
├── FGoogleClient        (Phase 1)
└── FOllamaClient        (Phase 2 - local)
```

### Focus Profiles

Tool filtering mechanism - same agent, different tool sets:
- **Auto** - All tools (default)
- **Blueprint** - Blueprint + project tools
- **AI & Behavior** - Blueprint + BT + Blackboard
- **Level & PCG** - Blueprint + PCG
- **C++ & Blueprint** - Blueprint + C++

---

## Implementation Phases

| Phase | Focus | Key Deliverables |
|-------|-------|------------------|
| **0** | Foundation | Plugin structure, Chat UI, MCP server, Project Index |
| **1** | Blueprints | Read/write all BP types, graph editing, agentic loop |
| **2** | AI Systems | Behavior Trees, Blackboards |
| **3** | PCG | Procedural Content Generation graphs |
| **4** | C++ | Reflection reading, optional source editing |
| **5** | Cross-System | Multi-asset operations, snapshots, project intelligence |

---

## Common Patterns

### Slate Widget Pattern

```cpp
class SOliveMyWidget : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SOliveMyWidget) {}
        SLATE_ARGUMENT(FString, InitialValue)
        SLATE_EVENT(FOnClicked, OnButtonClicked)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs)
    {
        ChildSlot
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(STextBlock)
                .Text(FText::FromString(InArgs._InitialValue))
            ]
        ];
    }
};
```

### Tool Registration Pattern

```cpp
void RegisterTools()
{
    FOliveToolRegistry::Get().RegisterTool(
        TEXT("project.search"),
        TEXT("Search for assets by name"),
        BuildSearchSchema(),
        FOliveToolHandler::CreateLambda([](const TSharedPtr<FJsonObject>& Params) -> FOliveToolResult
        {
            FString Query = Params->GetStringField(TEXT("query"));
            auto Results = FOliveProjectIndex::Get().SearchAssets(Query);
            return FOliveToolResult::Success(SerializeResults(Results));
        }),
        { TEXT("project") }  // Tags for profile filtering
    );
}
```

### Async HTTP Pattern

```cpp
void SendRequest()
{
    TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(ApiUrl);
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
    Request->SetContentAsString(JsonBody);

    Request->OnProcessRequestComplete().BindLambda(
        [this](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
        {
            if (bSuccess && Resp->GetResponseCode() == 200)
            {
                ProcessResponse(Resp->GetContentAsString());
            }
            else
            {
                HandleError(Resp);
            }
        }
    );

    Request->ProcessRequest();
}
```

### Game Thread Dispatch

```cpp
// MCP server runs on HTTP thread - dispatch to game thread for UE API calls
void HandleToolCall(const TSharedPtr<FJsonObject>& Params, TFunction<void(FOliveToolResult)> Callback)
{
    AsyncTask(ENamedThreads::GameThread, [Params, Callback]()
    {
        FOliveToolResult Result = FOliveToolRegistry::Get().ExecuteTool(ToolName, Params);
        Callback(Result);
    });
}
```

---

## Claude Code MCP Integration

Claude Code can connect to the Olive AI Studio MCP server for direct Unreal Engine development assistance.

### Setup

1. Open your Unreal Engine project with the Olive AI Studio plugin enabled
2. The MCP server starts automatically on port 3000 (or next available 3001-3009)
3. Claude Code auto-discovers via `.mcp.json` configuration

### Available Tools

| Tool | Description | Status |
|------|-------------|--------|
| `project.search` | Fuzzy search for project assets | Available |
| `project.get_asset_info` | Get detailed asset metadata | Available |
| `project.get_class_hierarchy` | Get class inheritance tree | Available |
| `project.get_dependencies` | Get asset dependencies | Available |
| `project.get_referencers` | Get referencing assets | Available |
| `project.get_config` | Get project configuration | Available |
| `blueprint.read` | Read Blueprint as IR | Phase 1 |
| `blueprint.create` | Create new Blueprint | Phase 1 |

### Resources

| URI | Description |
|-----|-------------|
| `olive://project/config` | Project configuration JSON |
| `olive://project/class-hierarchy` | Class inheritance tree |
| `olive://project/search?q=<query>` | Asset search results |

### Example Usage

```
# In Claude Code, after connecting to MCP server:
Use the project.search tool to find "BP_Player"
```

---

## Development Workflows

## Subagent System

This project uses five specialized subagents. USE THEM — do not try to do everything in the main conversation.

### When to Use Each Agent

| Situation | Agent | Why |
|-----------|-------|-----|
| "Where is X defined?" / "What files are in Y?" | `explorer` | Fast and cheap. Don't waste Sonnet tokens on grep. |
| Need to understand a UE API or engine internals | `researcher` | Explore before you design |
| Looking up MCP spec, OpenRouter API, protocol details | `researcher` | Get the facts first |
| Investigating how competitors solve a problem | `researcher` | Know the landscape |
| Starting a new module or feature | `architect` | Design before code. Always. |
| Need to decide file structure, interfaces, data flow | `architect` | Architectural decisions are the architect's job |
| Writing .h or .cpp files | `coder` | Implementation is the coder's job |
| Creating Slate widgets | `coder` | UI implementation |
| Compilation errors | `debugger` | Don't guess — let the debugger diagnose |
| Runtime crash or unexpected behavior | `debugger` | Root cause analysis |
| Reviewing completed code | `architect` | Design compliance check |

### Workflow: Implementing a Feature

When implementing any new module or feature, ALWAYS follow this sequence:

**Step 0 — Research (if needed)**
If the module involves UE APIs, protocols, or systems that aren't well understood yet, use the `researcher` subagent to:
- Investigate the relevant UE APIs (classes, methods, threading requirements)
- Look up any external protocol specs (MCP, OpenRouter, JSON-RPC)
- Check how competitors handle the same problem (if relevant)
- Write findings to `plans/research/{topic}.md`

Skip this step if the APIs and approach are already well understood.

**Step 1 — Design (Architect)**
Use the `architect` subagent to:
- Read any research reports in `plans/research/` relevant to this module
- Review any existing code that this module will interact with
- Produce a design document at `plans/{module}-design.md` containing interface definitions, file structure, data flow, integration points, and implementation order
- Summarize the design for handoff

**Do not proceed to Step 2 until the user has reviewed and approved the design.**

**Step 2 — Implementation (Coder)**
Use the `coder` subagent to:
- Read the architect's design document
- Implement files in the order specified by the architect
- Write complete .h and .cpp files

**Step 3 — Debug (if needed)**
If compilation errors or runtime issues occur, use the `debugger` subagent to:
- Diagnose the root cause
- Apply minimal fixes
- Verify the fix doesn't introduce new issues

**Step 4 — Review (optional)**
Use the `architect` subagent to:
- Review the implemented code against the design
- Check interface compliance
- Update architectural memory with decisions made during implementation

### Important Rules

- **Never skip the architect for new modules.** Writing code without a design leads to rework.
- **The coder follows the architect's design.** If the design has a gap, the coder documents it with a `// DESIGN NOTE` comment rather than making architectural decisions.
- **The debugger does minimal fixes.** It doesn't refactor or redesign — it fixes the specific issue.
- **Architectural decisions get documented.** Every significant decision goes in `docs/design/decisions.md` with rationale.


## File Locations

| Component | Location |
|-----------|----------|
| Plugin manifest | `UE_Olive_AI_Studio.uplugin` |
| Implementation plan | `plans/ue-ai-agent-plugin-plan-v2.md` |
| Phase 0 details | `plans/phase-0-detailed-implementation.md` |
| MCP configuration | `.mcp.json` |
| Agent prompts | `.claude/agents/*.md` |
| Claude settings | `.claude/settings.json` |
| System prompts | `Content/SystemPrompts/*.txt` |
| Configuration | `Config/DefaultOliveAI.ini` |

---

## Testing

- Unit tests: `Source/OliveAIEditor/Tests/`
- Use UE Automation Framework
- Test tool execution, validation rules, JSON-RPC parsing
- Integration tests with mock HTTP responses

---

## Quick Reference

### Essential UE Headers

```cpp
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Http.h"
#include "HttpServerModule.h"
#include "Json.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Widgets/SCompoundWidget.h"
#include "Framework/Docking/TabManager.h"
```

### Essential Subsystems

```cpp
// Asset editing
UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();

// Asset registry
IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

// Blueprint utilities
FKismetEditorUtilities::CompileBlueprint(Blueprint);
```
