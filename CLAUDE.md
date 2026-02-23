# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

**Build command:**
```bash
"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex
```
- Invoke UBT directly from bash — `cmd.exe /c` does NOT work well with spaces in paths
- Incremental builds take ~5-6 seconds

**Log files for diagnosing failures:**
- Project log: `../../Saved/Logs/UE_Olive_AI_Toolkit.log`
- UBT log: `C:/Users/<User>/AppData/Local/UnrealBuildTool/Log.txt`

**Tests:** UE Automation Framework, filter by `OliveAI.*` in Session Frontend > Automation.
Tests live in `Source/OliveAIEditor/Private/Tests/` (subdirs: `Brain/`, `Conversation/`, `FocusProfiles/`).

---

## Project Overview

Olive AI Studio is an editor-only UE 5.5+ plugin providing AI-powered development assistance via two interfaces: a dockable Slate chat panel and an MCP (JSON-RPC) server for external agents like Claude Code. Both share the same tool layer, validation engine, and write pipeline.

> **Development scope:** All work stays within `Plugins/UE_Olive_AI_Studio/`. The parent project (`UE_Olive_AI_Toolkit`) is for testing only.

---

## Architecture

### Module Layout

```
Source/
├── OliveAIRuntime/          # Minimal runtime module (LoadingPhase: Default)
│   └── IR/                  # Intermediate Representation structs
│       ├── OliveIRTypes.h         # Base IR types
│       ├── CommonIR.h             # FOliveIRGraph, FOliveIRNode
│       ├── BlueprintIR.h          # Blueprint-specific IR
│       ├── BehaviorTreeIR.h       # BT IR
│       ├── PCGIR.h                # PCG IR
│       ├── CppIR.h                # C++ IR
│       ├── OliveCompileIR.h       # Compile result IR
│       └── OliveIRSchema.h        # IR schema validation
│
└── OliveAIEditor/           # Editor-only module (LoadingPhase: PostEngineInit)
    ├── UI/                  # Slate widgets (chat panel, operation feed)
    ├── Chat/                # Conversation manager, prompt assembly, editor session
    ├── Brain/               # Brain layer state machine, loop detection, self-correction
    ├── Providers/           # API clients (8 providers)
    ├── MCP/                 # MCP server, JSON-RPC, tool registry
    ├── Services/            # Validation engine, transactions, asset resolver
    ├── Index/               # Project index (asset search, class hierarchy)
    ├── Profiles/            # Focus profiles + tool packs
    ├── Settings/            # UDeveloperSettings (UOliveAISettings)
    ├── Blueprint/           # Blueprint read/write/catalog
    ├── BehaviorTree/        # BT/Blackboard
    ├── PCG/                 # PCG graphs
    ├── Cpp/                 # C++ integration
    └── CrossSystem/         # Multi-asset operations
```

`OliveAIEditor.Build.cs` adds recursive include paths for sub-modules: `Blueprint`, `BehaviorTree`, `PCG`, `Cpp`, `CrossSystem`, `Brain`.

### Startup Order (OliveAIEditorModule.cpp)

`OliveAIEditor` uses `PostEngineInit` loading phase but calls `OnPostEngineInit()` directly (not via delegate) because the delegate has already fired by that point. Initialization order:

1. Register editor commands + UI extensions
2. Initialize `FOliveEditorChatSession` singleton (owns `ConversationManager`)
3. `FOliveValidationEngine::Get().RegisterCoreRules()`
4. `FOliveProjectIndex::Get().Initialize()`
5. `FOliveToolRegistry::Get().RegisterBuiltInTools()` (project tools)
6. `FOliveNodeCatalog::Get().Initialize()` → `FOliveBlueprintToolHandlers::Get().RegisterAllTools()`
7. BT tools → PCG tools (guarded by availability check) → C++ tools → CrossSystem tools
8. `FOliveFocusProfileManager::Get().Initialize()` (must come after all tool registration)
9. `FOliveToolPackManager::Get().Initialize()`
10. Prompt assembler + MCP prompt templates
11. `FOliveMCPServer::Get().Start()` (if `bAutoStartMCPServer`)

### Key Singletons (all `static Foo& Get()`)

| Singleton | Responsibility |
|-----------|---------------|
| `FOliveToolRegistry` | Central tool registry (thread-safe via `FRWLock`) |
| `FOliveMCPServer` | HTTP JSON-RPC server (ports 3000–3009) |
| `FOliveProjectIndex` | Asset registry wrapper / search |
| `FOliveNodeCatalog` | Blueprint node catalog with fuzzy matching |
| `FOliveWritePipeline` | 6-stage write safety pipeline |
| `FOliveValidationEngine` | Validation rule registry |
| `FOliveTransactionManager` | `FScopedTransaction` wrapper |
| `FOliveFocusProfileManager` | Focus profile filtering |
| `FOliveToolPackManager` | Dynamic tool pack gating per turn |
| `FOliveEditorChatSession` | Editor-lifetime session (owns ConversationManager) |

### Write Pipeline (6 Stages)

1. **Validate** — input validation, preconditions
2. **Confirm** — tier routing (Tier 1: auto, Tier 2: plan+confirm, Tier 3: preview). MCP clients skip this.
3. **Transact** — `FScopedTransaction` + `Modify()`
4. **Execute** — actual mutation via `FOliveWriteExecutor` delegate
5. **Verify** — structural checks + optional compile (`bAutoCompile`) + orphaned exec-flow detection
6. **Report** — structured result with timing

Result chain: `FOliveWriteResult` → `.ToToolResult()` → `FOliveToolResult`. Factory methods: `Success()`, `ValidationError()`, `ExecutionError()`, `ConfirmationNeeded()`.

### Brain Layer

`FOliveBrainLayer` is a state machine: `Idle` → `Planning` → `WorkerActive` → `AwaitingConfirmation` → `Cancelling` → `Completed` / `Error`. Worker phases: `Streaming`, `ExecutingTools`, `Compiling`, `SelfCorrecting`, `Complete`. Includes `FOliveLoopDetector` (infinite loop detection) and `FOliveSelfCorrectionPolicy` (retry after tool failures).

### Editor Chat Session

`FOliveEditorChatSession` is an editor-lifetime singleton that owns the `ConversationManager`. The chat panel (`SOliveAIChatPanel`) holds only a `TWeakPtr` — closing the panel does NOT cancel in-flight operations. Background completions trigger toast notifications.

### MCP Transport

HTTP JSON-RPC 2.0 server on `/mcp` endpoint. `mcp-bridge.js` converts stdio ↔ HTTP for Claude Code CLI. Auto-discovers ports 3000–3009. Protocol version `2024-11-05`. Tool calls dispatch to game thread via `AsyncTask(ENamedThreads::GameThread, ...)`.

### Large Graph Paging

Threshold: `OLIVE_LARGE_GRAPH_THRESHOLD = 500` nodes. Page size: `OLIVE_GRAPH_PAGE_SIZE = 100` (max 200). `ReadGraphSummary()` returns metadata only; `ReadGraphPage()` builds the full `NodeIdMap` but serializes only a page slice.

---

## Coding Standards

### Naming Conventions

| Type | Prefix | Example |
|------|--------|---------|
| Classes (UObject) | `U` | `UOliveAISettings` |
| Classes (AActor) | `A` | `AOliveDebugActor` |
| Structs | `F` | `FOliveToolResult` |
| Interfaces | `I` | `IOliveAIProvider` |
| Enums | `E` | `EOliveOperationStatus` |
| Slate widgets | `S` | `SOliveAIChatPanel` |
| Delegates | `F...Delegate` / `FOn...` | `FOnOliveStreamChunk` |

- All files prefixed with `Olive` (e.g., `OliveClassName.h` / `OliveClassName.cpp`)
- Log category: `LogOliveAI`
- Return `FOliveToolResult` with structured errors (code + message + suggestion), not bare booleans

### Tool Handler Pattern

validate params → load Blueprint → build `FOliveWriteRequest` → bind executor lambda → `ExecuteWithOptionalConfirmation`

Schema implementations: `OliveBlueprintSchemas.cpp`. Tool registrations: `RegisterReaderTools()`, etc.
Note: `OliveBlueprintToolHandlers.cpp` is 3000+ lines with anonymous namespace helpers.

---

## Key Design Decisions

### Confirmation Tiers

| Tier | Risk | Operations | UX |
|------|------|------------|-----|
| 1 | Low | Add variable, add component, create empty BP | Auto-execute |
| 2 | Medium | Create function with logic, wire event graph | Plan → Confirm |
| 3 | High | Refactor, delete, reparent | Non-destructive preview |

### Providers (8 implemented)

`ClaudeCode` (default, no API key), `OpenRouter`, `ZAI`, `Anthropic`, `OpenAI`, `Google`, `Ollama`, `OpenAICompatible`. Interface: `IOliveAIProvider` with `SendMessage()` (streaming), `CancelRequest()`, `ValidateConnection()`. Error classification uses parseable prefix: `[HTTP:{code}:RetryAfter={s}]`.

### Focus Profiles + Tool Packs

Focus profiles filter tool visibility: Auto, Blueprint, AI & Behavior, Level & PCG, C++ & Blueprint.
Tool packs (`Config/OliveToolPacks.json`) define named sets: `read_pack`, `write_pack_basic`, `write_pack_graph`, `danger_pack`. `FOliveToolPackManager` gates packs per turn based on intent flags (`bTurnHasExplicitWriteIntent`, `bTurnHasDangerIntent`).

### Safety Presets

`UOliveAISettings` exposes presets: `Careful`, `Fast`, `YOLO` — with per-operation tier overrides. Rate limit: `MaxWriteOpsPerMinute = 30`. Brain layer settings: batch write max ops, context window, correction cycles. Checkpoint interval: every 5 steps.

---

## UE Compatibility Guardrails (Critical)

Unreal's startup popup ("rebuild from source") is generic. Treat it as a symptom, not a diagnosis.

### Required Workflow for Any Compile/Load Failure

1. Read the real errors from project log and UBT log (see Build & Test above)
2. Fix the first real compiler error(s), not downstream ones
3. Rebuild and confirm success before concluding resolved

### Header/API Safety Rules

- Never guess Unreal header names or member fields
- Verify symbols in engine source under `Engine/Plugins/.../Source/.../Public` before coding
- If a type is forward-declared but members are accessed, include the concrete header in `.cpp`
- Prefer version-accurate UE 5.5 APIs over legacy fields

### Known UE 5.5 API Drift

- PCG subgraph settings: `PCGSubgraph.h` (not `PCGSubgraphSettings.h`)
- `UPCGEdge::OtherPin` is obsolete → use `GetOtherPin(...)` with `PCGEdge.h`
- Always confirm current symbols in UE 5.5 source for Blueprint/editor APIs

---

## Subagent System

This project uses specialized subagents. USE THEM — do not try to do everything in the main conversation.

| Situation | Agent |
|-----------|-------|
| "Where is X defined?" / file lookups | `explorer` (fast, cheap) |
| UE API research, protocol specs | `researcher` |
| New module or feature design | `architect` (design before code, always) |
| Writing .h / .cpp files, Slate widgets | `coder` |
| Compilation errors, crashes | `debugger` |

### Feature Implementation Workflow

1. **Research** (if needed) → `researcher` subagent
2. **Design** → `architect` subagent → produces `plans/{module}-design.md` → **wait for user approval**
3. **Implement** → `coder` subagent (follows architect's design)
4. **Debug** (if needed) → `debugger` subagent (minimal fixes only)
5. **Review** (optional) → `architect` subagent

### Rules

- **Never skip the architect for new modules.**
- **One explorer at a time. NEVER spawn multiple.** Give it ALL questions in one invocation.
- The coder follows the architect's design. Gaps get `// DESIGN NOTE` comments, not ad-hoc decisions.
- The debugger does minimal fixes — no refactoring.
- Architectural decisions go in `docs/design/decisions.md`.

---

## Key File Locations

| Component | Path |
|-----------|------|
| Plugin manifest | `UE_Olive_AI_Studio.uplugin` |
| Module entry point | `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp` |
| Settings | `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` |
| Tool registry | `Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h` |
| MCP server | `Source/OliveAIEditor/Public/MCP/OliveMCPServer.h` |
| Write pipeline | `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h` |
| BP tool handlers | `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` |
| BP schemas | `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` |
| Node catalog | `Source/OliveAIEditor/Blueprint/Private/Catalog/OliveNodeCatalog.cpp` |
| Provider interface | `Source/OliveAIEditor/Public/Providers/IOliveAIProvider.h` |
| Brain layer | `Source/OliveAIEditor/Public/Brain/OliveBrainLayer.h` |
| Editor chat session | `Source/OliveAIEditor/Public/Chat/OliveEditorChatSession.h` |
| Conversation manager | `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h` |
| IR types | `Source/OliveAIRuntime/Public/IR/OliveIRTypes.h` |
| Tool packs config | `Config/OliveToolPacks.json` |
| System prompts | `Content/SystemPrompts/` |
| MCP bridge | `mcp-bridge.js`, `.mcp.json` |
| Agent prompts | `.claude/agents/*.md` |
| Configuration | `Config/DefaultOliveAI.ini` |
| Tests | `Source/OliveAIEditor/Private/Tests/` |
