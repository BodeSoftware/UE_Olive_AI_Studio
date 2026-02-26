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
Tests live in `Source/OliveAIEditor/Private/Tests/` (subdirs: `Brain/`, `Conversation/`, `EdgeCases/`, `FocusProfiles/`, `Integration/`, `Providers/`).

---

## Project Overview

Olive AI Studio is an editor-only UE 5.5+ plugin providing AI-powered development assistance via two interfaces: a dockable Slate chat panel and an MCP (JSON-RPC) server for external agents like Claude Code. Both share the same tool layer, validation engine, and write pipeline.

> **Development scope:** All work stays within `Plugins/UE_Olive_AI_Studio/`. The parent project (`UE_Olive_AI_Toolkit`) is for testing only.

### Development Scope Boundaries

**Allowed directories** (all relative to plugin root):
`Source/`, `Content/`, `Config/`, `plans/`, `.claude/`, root files (`*.uplugin`, `*.md`, etc.)

**FORBIDDEN — never access:**
- `../../Source/` (parent project source)
- `../../Content/` (parent project content)
- `../../Config/` (parent project config)
- `../../*.uproject` (parent project file)
- Anything outside the plugin directory

See `.claude/SCOPE.md` for enforcement details. If you think you need parent project files, ask the user first — 99% of the time you don't.

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
│       ├── BlueprintPlanIR.h      # Blueprint plan JSON IR (OlivePlanOps vocabulary)
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
    ├── Blueprint/           # Blueprint read/write/catalog/plan/template
    │   ├── Public/Template/ # FOliveTemplateSystem (factory + reference templates)
    │   ├── Public/Plan/     # FOlivePlanValidator, FOlivePlanExecutor, FOliveBlueprintPlanResolver
    │   └── Private/Template/# OliveTemplateSystem.cpp
    ├── BehaviorTree/        # BT/Blackboard
    ├── PCG/                 # PCG graphs
    ├── Cpp/                 # C++ integration
    └── CrossSystem/         # Multi-asset operations
```

`OliveAIEditor.Build.cs` adds recursive include paths for sub-modules: `Blueprint`, `BehaviorTree`, `PCG`, `Cpp`, `CrossSystem`, `Brain`. Notable module dependencies include `AnimGraph`/`AnimGraphRuntime` (Animation Blueprint support), `UMG`/`UMGEditor` (Widget Blueprint support), and `LiveCoding`/`SourceCodeAccess` (C++ hot reload integration).

### Startup Order (OliveAIEditorModule.cpp)

`OliveAIEditor` uses `PostEngineInit` loading phase. `StartupModule()` handles steps 1–3; `OnPostEngineInit()` is called directly (not via delegate, since it has already fired) for the rest.

**StartupModule():**
1. `RegisterCommands()` + `RegisterUI()`
2. `FOliveEditorChatSession::Get().Initialize()`

**OnPostEngineInit():**
3. `FOliveValidationEngine::Get().RegisterCoreRules()`
4. `FOliveProjectIndex::Get().Initialize()`
5. `FOliveToolRegistry::Get().RegisterBuiltInTools()` (project tools)
6. `FOliveNodeCatalog::Get().Initialize()` → `FOliveBlueprintToolHandlers::Get().RegisterAllTools()` (includes `RegisterTemplateTools()`)
7. `FOliveBTNodeCatalog::Get().Initialize()` → `FOliveBTToolHandlers::Get().RegisterAllTools()` → `RegisterBehaviorTreeRules()`
8. PCG tools (guarded by availability check): `FOlivePCGNodeCatalog` → `FOlivePCGToolHandlers`
9. `FOliveCppToolHandlers::Get().RegisterAllTools()` → `RegisterCppRules()`
10. `FOliveCrossSystemToolHandlers::Get().RegisterAllTools()` → `RegisterCrossSystemRules()`
11. `FOliveTemplateSystem::Get().Initialize()` (scans `Content/Templates/` for factory + reference JSON templates)
12. `FOliveFocusProfileManager::Get().Initialize()` (must come after all tool registration)
13. `FOliveToolPackManager::Get().Initialize()`
14. `FOlivePromptAssembler::Get().Initialize()` + `FOliveMCPPromptTemplates::Get().Initialize()`
15. `FOliveMCPServer::Get().Start()` (if `bAutoStartMCPServer`)

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
| `FOliveEditorChatSession` | Editor-lifetime session (owns ConversationManager, MessageQueue, RetryManager) |
| `FOliveRunManager` | Multi-step agentic run orchestration |
| `FOliveCompileManager` | Blueprint compilation management |
| `FOlivePromptAssembler` | System prompt assembly + capability knowledge injection |
| `FOliveSnapshotManager` | Snapshot/rollback management |
| `FOliveMCPPromptTemplates` | MCP prompt template registry |
| `FOliveBTNodeCatalog` | Behavior Tree node catalog |
| `FOliveTemplateSystem` | Factory + reference template registry; loaded from `Content/Templates/` |

### Write Pipeline (6 Stages)

1. **Validate** — input validation, preconditions
2. **Confirm** — tier routing (Tier 1: auto, Tier 2: plan+confirm, Tier 3: preview). MCP clients skip this.
3. **Transact** — `FScopedTransaction` + `Modify()`
4. **Execute** — actual mutation via `FOliveWriteExecutor` delegate
5. **Verify** — structural checks + optional compile (`bAutoCompile`) + orphaned exec-flow detection
6. **Report** — structured result with timing

Result chain: `FOliveWriteResult` → `.ToToolResult()` → `FOliveToolResult`. Factory methods: `Success()`, `ValidationError()`, `ExecutionError()`, `ConfirmationNeeded()`.

### Brain Layer

`FOliveBrainLayer` is a state machine: `Idle` → `Planning` → `WorkerActive` → `AwaitingConfirmation` → `Cancelling` → `Completed` / `Error`. Worker phases: `Streaming`, `ExecutingTools`, `Compiling`, `SelfCorrecting`, `Complete`. Run outcomes: `Completed`, `PartialSuccess`, `Failed`, `Cancelled`.

Related Brain subsystems (in `Public/Brain/`):
- `FOliveLoopDetector` (in `OliveRetryPolicy.h`) — infinite loop detection, owned by `FOliveConversationManager`
- `FOliveSelfCorrectionPolicy` — retry evaluation after tool failures; includes plan content deduplication (`PreviousPlanHashes`), progressive error disclosure (terse→full→escalate), and 3-tier error classification (A=FixableMistake, B=UnsupportedFeature, C=Ambiguous)
- `FOlivePromptDistiller` — context window distillation
- `FOliveOperationHistory` — operation history tracking
- `FOliveToolExecutionContext` — per-tool execution context

### Editor Chat Session

`FOliveEditorChatSession` is an editor-lifetime singleton that owns three subsystems: `ConversationManager`, `MessageQueue` (FIFO queue for messages during processing), and `RetryManager`. The chat panel (`SOliveAIChatPanel`) holds only a `TWeakPtr` — closing the panel does NOT cancel in-flight operations. Background completions trigger toast notifications.

### MCP Transport

HTTP JSON-RPC 2.0 server on `/mcp` endpoint. Protocol version `2024-11-05`. Handles: `initialize`, `tools/list`, `tools/call`, `resources/list`, `resources/read`, `prompts/list`, `prompts/get`, `ping`. Also exposes `GET /mcp/events` for poll-based notifications. Tool calls dispatch to game thread via `AsyncTask(ENamedThreads::GameThread, ...)`.

`mcp-bridge.js` converts stdio ↔ HTTP for Claude Code CLI. The bridge auto-discovers ports 3000–3009. Server port is configurable in settings (`ClampMin=1024, ClampMax=65535`).

Bridge config (`.mcp.json`): `{ "mcpServers": { "olive-ai-studio": { "command": "node", "args": ["mcp-bridge.js"] } } }`. Claude Code reads this to auto-connect.

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
Note: `OliveBlueprintToolHandlers.cpp` is 7000+ lines with anonymous namespace helpers. Registration methods: `RegisterReaderTools()`, `RegisterWriterTools()`, `RegisterPlanTools()`, `RegisterTemplateTools()`.

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

### Claude Code CLI Prompt Routing (Critical)

The Claude Code provider has two prompt channels with different behavior:
- `stdin` prompt (`BuildPrompt`) is the **imperative channel** ("what to do now")
- `--append-system-prompt` (`BuildSystemPrompt`) is **reference context**

To avoid CLI regressions (search loops, text-only stops), keep these rules:
- `BuildPrompt()` must include conversation history (`[User]`, `[Assistant]`, `[Tool Result]`) **and** reconstruct prior `<tool_call ...>` blocks from `AssistantMessage.ToolCalls`.
- `BuildPrompt()` must append a `## Next Action Required` directive:
  - first iteration: route create vs modify explicitly
  - continuation turns: continue tool execution, avoid repeated identical searches
- Do not use fake role tags like `[System]` inside `stdin`; use plain structured sections (`## Next Action Required`).
- `BuildSystemPrompt()` should provide schema/reference guidance, not be the only place where routing intent lives.
- Search guidance must be scoped:
  - existing assets: use `project.search`
  - new assets: choose target path directly (do not force search-first)
- Batching guidance must be constrained:
  - batch independent calls
  - never batch `blueprint.preview_plan_json` and `blueprint.apply_plan_json` in the same response

### Focus Profiles + Tool Packs

Focus profiles filter tool visibility (3 profiles since Phase E migration):
- **Auto** — all tools visible
- **Blueprint** — blueprint, BT, blackboard, PCG, project, cross-system tools
- **C++** — C++ tools only

Legacy profiles ("AI & Behavior", "Level & PCG", "C++ & Blueprint") are auto-migrated via `MigrateToPhaseEProfile()`.

Tool packs (`Config/OliveToolPacks.json`) define named sets: `read_pack`, `write_pack_basic`, `write_pack_graph`, `danger_pack`. `FOliveToolPackManager` gates packs per turn based on intent flags (`bTurnHasExplicitWriteIntent`, `bTurnHasDangerIntent`).

### Blueprint Plan JSON

Intent-level graph editing system where the AI describes "what" it wants (e.g., "call SetActorLocation") and the plan resolver translates to concrete Blueprint nodes. Has its own preview/apply cycle with fingerprint verification.

- **IR:** `BlueprintPlanIR.h` defines `OlivePlanOps` namespace with closed vocabulary (`call`, `get_var`, `set_var`, `branch`, `sequence`, `cast`, `event`, `custom_event`, `for_loop`, etc.)
- **Tools:** `blueprint.preview_plan_json` (preview) → `blueprint.apply_plan_json` (apply with fingerprint)
- **Code:** `Blueprint/Public/Plan/` and `Blueprint/Private/Plan/`
- **Settings:** `bEnableBlueprintPlanJsonTools`, `PlanJsonMaxSteps = 128`, `bPlanJsonRequirePreviewForApply`, `bEnforcePlanFirstGraphRouting`, `PlanFirstGraphRoutingThreshold = 3`

**Plan Pipeline (resolver → validator → executor):**
1. `FOliveBlueprintPlanResolver::Resolve()` — alias resolution, SCS component variable recognition, pure-node collapse, `ExpandPlanInputs()` for auto-synthesis
2. `FOlivePlanValidator::Validate()` — Phase 0 structural checks: `COMPONENT_FUNCTION_ON_ACTOR` (unwired Target on Actor BP) and `EXEC_WIRING_CONFLICT` (exec_after targeting a step with exec_outputs)
3. `FOlivePlanExecutor::Execute()` — 7 phases: CreateNodes → AutoWireComponents (Phase 1.5) → WireExec → WireData → SetDefaults → PreCompileValidation (Phase 5.5) → AutoLayout. `FOlivePlanExecutionContext` carries `AutoFixCount`, `PreCompileIssues`, `NodeIdToStepId`.

### Blueprint Templates

Factory templates create complete Blueprints from parameterized JSON. Reference templates provide pattern documentation the AI can read before writing plans.

- **Tools:** `blueprint.create_from_template`, `blueprint.get_template`, `blueprint.list_templates` (all in `RegisterTemplateTools()`)
- **Templates:** `Content/Templates/factory/` (e.g., `stat_component.json`) and `Content/Templates/reference/` (e.g., `component_patterns.json`, `ue_events.json`)
- **Template format:** JSON with `template_id`, `template_type`, `parameters` (with defaults), optional `presets`, `blueprint` section (type, variables, event_dispatchers, functions with embedded plan JSON), `catalog_description`
- **Parameter substitution:** `${param_name}` tokens, supports bool ternary conditionals like `${start_full} ? ${max_value} : 0`
- **Catalog injection:** `FOliveTemplateSystem::GetCatalogBlock()` is appended to capability knowledge by `FOlivePromptAssembler::GetCapabilityKnowledge()` so AI agents always see available templates

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
- `TWeakObjectPtr` has no `IsNull()` — use `.Get() != nullptr` or `IsValid()`
- `FEdGraphPinType::PinSubCategoryObject` is `TWeakObjectPtr<UObject>` — check with `.Get()`
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
| BP plan JSON | `Source/OliveAIEditor/Blueprint/Public/Plan/`, `Source/OliveAIRuntime/Public/IR/BlueprintPlanIR.h` |
| Plan validator | `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanValidator.h` |
| Node catalog | `Source/OliveAIEditor/Blueprint/Private/Catalog/OliveNodeCatalog.cpp` |
| Template system | `Source/OliveAIEditor/Blueprint/Public/Template/OliveTemplateSystem.h` |
| Template data | `Content/Templates/factory/`, `Content/Templates/reference/` |
| Provider interface | `Source/OliveAIEditor/Public/Providers/IOliveAIProvider.h` |
| Brain layer | `Source/OliveAIEditor/Public/Brain/OliveBrainLayer.h` |
| Retry/loop detection | `Source/OliveAIEditor/Public/Brain/OliveRetryPolicy.h` |
| Self-correction | `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h` |
| Editor chat session | `Source/OliveAIEditor/Public/Chat/OliveEditorChatSession.h` |
| Conversation manager | `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h` |
| Run manager | `Source/OliveAIEditor/Public/Chat/OliveRunManager.h` |
| Prompt assembler | `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h` |
| Compile manager | `Source/OliveAIEditor/Blueprint/Public/Compile/OliveCompileManager.h` |
| Snapshot manager | `Source/OliveAIEditor/CrossSystem/Public/OliveSnapshotManager.h` |
| MCP prompt templates | `Source/OliveAIEditor/CrossSystem/Public/OliveMCPPromptTemplates.h` |
| Asset resolver | `Source/OliveAIEditor/Public/Services/OliveAssetResolver.h` |
| IR types | `Source/OliveAIRuntime/Public/IR/OliveIRTypes.h` |
| Tool packs config | `Config/OliveToolPacks.json` |
| System prompts | `Content/SystemPrompts/` |
| MCP bridge | `mcp-bridge.js`, `.mcp.json` |
| Agent prompts | `.claude/agents/*.md` |
| Scope boundaries | `.claude/SCOPE.md` |
| Configuration | `Config/DefaultOliveAI.ini` |
| Tests | `Source/OliveAIEditor/Private/Tests/` |

> **Note:** `AGENTS.md` in the root is a stale near-copy of this file (minus the Subagent section). It can be safely deleted.
