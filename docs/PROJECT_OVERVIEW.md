# Olive AI Studio - Project Overview

**Version:** 0.1.0-alpha
**Engine:** Unreal Engine 5.5+
**Author:** Bode Software
**License:** MIT
**Module Type:** Editor-only plugin

---

## Table of Contents

1. [What Is Olive AI Studio?](#what-is-olive-ai-studio)
2. [Architecture](#architecture)
3. [Module Layout](#module-layout)
4. [Startup & Initialization](#startup--initialization)
5. [Key Singletons](#key-singletons)
6. [Intermediate Representation (IR) System](#intermediate-representation-ir-system)
7. [MCP Server & JSON-RPC Transport](#mcp-server--json-rpc-transport)
8. [Tool Registry & Tool Catalog (95 Tools)](#tool-registry--tool-catalog-95-tools)
9. [Write Pipeline (6 Stages)](#write-pipeline-6-stages)
10. [Blueprint Subsystem](#blueprint-subsystem)
11. [Behavior Tree Subsystem](#behavior-tree-subsystem)
12. [PCG Subsystem](#pcg-subsystem)
13. [C++ Subsystem](#c-subsystem)
14. [Cross-System Operations](#cross-system-operations)
15. [Brain Layer (State Machine)](#brain-layer-state-machine)
16. [AI Provider System (8 Providers)](#ai-provider-system-8-providers)
17. [Chat System](#chat-system)
18. [Focus Profiles & Tool Packs](#focus-profiles--tool-packs)
19. [Validation Engine](#validation-engine)
20. [Services Layer](#services-layer)
21. [Project Index](#project-index)
22. [UI Layer (Slate)](#ui-layer-slate)
23. [System Prompts & Knowledge](#system-prompts--knowledge)
24. [Settings Reference](#settings-reference)
25. [File Inventory](#file-inventory)
26. [Configuration Files](#configuration-files)
27. [Build System](#build-system)

---

## What Is Olive AI Studio?

Olive AI Studio is an editor-only Unreal Engine 5.5+ plugin that provides AI-powered development assistance through two interfaces:

1. **Dockable Slate Chat Panel** — An in-editor chat window where developers converse with an AI assistant that can read, create, and modify Unreal assets (Blueprints, Behavior Trees, PCG graphs, C++ classes) in real time.

2. **MCP Server** — A JSON-RPC 2.0 HTTP server that exposes the same 95 tools to external AI agents (such as Claude Code CLI), enabling programmatic control of the editor from outside the engine.

Both interfaces share the same tool layer, validation engine, write pipeline, and safety system. Every mutation goes through a 6-stage write pipeline with confirmation tiers, transactions, compile verification, and rate limiting.

### Key Capabilities

- **Blueprint CRUD** — Create, read, modify, and delete Blueprints including variables, components, functions, event graphs, Animation Blueprints, and Widget Blueprints
- **Blueprint Plan JSON** — Intent-level graph construction: describe *what* you want (call function X, branch on Y) and the system resolves nodes, wires pins, and auto-layouts
- **Behavior Tree Editing** — Create/edit Behavior Trees and Blackboards with full node hierarchy management
- **PCG Graph Editing** — Create/edit Procedural Content Generation graphs with node/edge manipulation
- **C++ Integration** — Read class reflection data, create classes, add properties/functions, trigger live coding
- **Cross-System Operations** — Multi-asset workflows, snapshots, rollbacks, rename refactoring, Blueprint-to-C++ migration
- **Project Intelligence** — Asset search, dependency analysis, class hierarchy traversal, project configuration queries
- **Self-Correction** — Brain layer with loop detection, retry policies, and compile error → fix feedback loops

---

## Architecture

```
                    ┌─────────────────────────┐
                    │    External AI Agent     │
                    │   (Claude Code CLI)      │
                    └────────┬────────────────┘
                             │ stdio
                    ┌────────┴────────────────┐
                    │    mcp-bridge.js         │
                    │  (stdio ↔ HTTP bridge)   │
                    └────────┬────────────────┘
                             │ HTTP POST /mcp
          ┌──────────────────┼──────────────────────┐
          │                  │                       │
┌─────────┴──────────┐  ┌───┴──────────────┐  ┌────┴──────────────┐
│ SOliveAIChatPanel  │  │ FOliveMCPServer  │  │ FOliveToolRegistry│
│  (Slate UI)        │  │ (JSON-RPC 2.0)   │  │  (95 tools)       │
└─────────┬──────────┘  └───┬──────────────┘  └────┬──────────────┘
          │                  │                       │
          │  ┌───────────────┘                       │
          │  │                                       │
    ┌─────┴──┴───────────────────────────────────────┴─────────┐
    │              FOliveConversationManager                    │
    │   (message history, tool dispatch, brain layer, retry)   │
    └─────────────────────────┬────────────────────────────────┘
                              │
    ┌─────────────────────────┼─────────────────────────┐
    │                         │                          │
┌───┴──────────┐  ┌──────────┴──────────┐  ┌───────────┴────────────┐
│ Validation   │  │  Write Pipeline     │  │   Reader Layer         │
│ Engine       │  │  (6-stage safety)   │  │   (IR serialization)   │
│ (22 rules)   │  │                     │  │                        │
└──────────────┘  └──────────┬──────────┘  └────────────────────────┘
                              │
    ┌────────────┬────────────┼────────────┬────────────┐
    │            │            │            │            │
┌───┴────┐ ┌────┴───┐ ┌─────┴────┐ ┌─────┴───┐ ┌─────┴──────┐
│Blueprint│ │  BT    │ │  PCG     │ │  C++    │ │CrossSystem │
│Writer   │ │Writer  │ │Writer    │ │Writer   │ │Operations  │
└─────────┘ └────────┘ └──────────┘ └─────────┘ └────────────┘
```

### Design Principles

- **IR-First** — All asset data flows through a typed Intermediate Representation (`FOliveIR*` structs) before serialization to JSON. The IR layer lives in a minimal runtime module for potential future runtime use.
- **Pipeline Safety** — Every write goes through Validate → Confirm → Transact → Execute → Verify → Report. No shortcuts.
- **Tool Parity** — Chat panel and MCP server expose the same tools with the same schemas. MCP clients skip the Confirm stage.
- **Domain Isolation** — Blueprint, BT, PCG, C++, and CrossSystem are separate sub-modules with their own readers, writers, schemas, and tool handlers.
- **Singleton Coordination** — Key services are singletons with thread-safe access patterns (`FRWLock`, `FCriticalSection`).

---

## Module Layout

The plugin contains two UE modules:

### OliveAIRuntime (Minimal Runtime Module)

- **Loading Phase:** `Default`
- **Dependencies:** `Core`, `CoreUObject`, `Json`, `JsonUtilities`
- **Purpose:** Houses the Intermediate Representation type system (`IR/` directory). Kept minimal so IR types could theoretically be used at runtime in the future.

```
Source/OliveAIRuntime/
├── OliveAIRuntime.Build.cs
├── Public/
│   ├── OliveAIRuntimeModule.h
│   └── IR/
│       ├── OliveIRTypes.h          # Base types (FOliveIRType, FOliveIRResult)
│       ├── CommonIR.h              # FOliveIRGraph, FOliveIRNode, FOliveIRPin
│       ├── BlueprintIR.h           # FOliveIRBlueprint, FOliveIRBlueprintCapabilities
│       ├── BlueprintPlanIR.h       # FOliveIRBlueprintPlan, FOliveIRBlueprintPlanStep
│       ├── BehaviorTreeIR.h        # FOliveIRBehaviorTree, FOliveIRBTNode
│       ├── PCGIR.h                 # FOliveIRPCGGraph, FOliveIRPCGNode
│       ├── CppIR.h                 # FOliveIRCppClass, FOliveIRCppFunction
│       ├── OliveCompileIR.h        # FOliveIRCompileResult, FOliveIRCompileError
│       └── OliveIRSchema.h         # FOliveIRValidator, schema rules R1-R8
└── Private/
    ├── OliveAIRuntimeModule.cpp
    └── IR/                          # Serialization implementations for each IR type
        ├── OliveIRTypes.cpp
        ├── CommonIR.cpp
        ├── BlueprintIR.cpp
        ├── BlueprintPlanIR.cpp
        ├── BehaviorTreeIR.cpp
        ├── PCGIR.cpp
        ├── CppIR.cpp
        ├── OliveCompileIR.cpp
        └── OliveIRSchema.cpp
```

### OliveAIEditor (Editor-Only Module)

- **Loading Phase:** `PostEngineInit`
- **Dependencies:** 30+ modules including `UnrealEd`, `BlueprintGraph`, `Kismet`, `AnimGraph`, `UMG`, `PCG`, `HTTP`, `HTTPServer`, `Json`, `AssetRegistry`, `LiveCoding`, and more
- **Sub-modules:** Blueprint, BehaviorTree, PCG, Cpp, CrossSystem (each with recursive include paths)

```
Source/OliveAIEditor/
├── OliveAIEditor.Build.cs
├── Public/                      # Core editor module headers
│   ├── Brain/                   # State machine, correction, retry, tool packs
│   ├── Chat/                    # Conversation, prompt assembly, run manager
│   ├── Index/                   # Project asset index
│   ├── MCP/                     # Server, tool registry, JSON-RPC
│   ├── Profiles/                # Focus profile manager
│   ├── Providers/               # 8 AI provider implementations
│   ├── Services/                # Validation, transactions, asset resolver
│   ├── Settings/                # UOliveAISettings (UDeveloperSettings)
│   └── UI/                      # Slate chat panel widgets
├── Private/                     # Core implementations
│   ├── Tests/                   # UE Automation tests
│   │   ├── Brain/
│   │   ├── Conversation/
│   │   ├── FocusProfiles/
│   │   ├── EdgeCases/           # (placeholder)
│   │   └── Integration/         # (placeholder)
│   └── ...
├── Blueprint/                   # Sub-module: Blueprint read/write/plan
│   ├── Public/
│   │   ├── Catalog/             # Node catalog with fuzzy search
│   │   ├── Compile/             # Compile manager
│   │   ├── MCP/                 # Tool handlers & schemas
│   │   ├── Pipeline/            # Write pipeline
│   │   ├── Plan/                # Plan JSON executor, resolver, lowerer
│   │   ├── Reader/              # Blueprint/graph/node/pin serializers
│   │   └── Writer/              # Blueprint/graph/node/component writers
│   └── Private/
├── BehaviorTree/                # Sub-module: BT/Blackboard
├── PCG/                         # Sub-module: PCG graphs
├── Cpp/                         # Sub-module: C++ reflection/source
└── CrossSystem/                 # Sub-module: Multi-asset, snapshots, prompts
```

---

## Startup & Initialization

`OliveAIEditor` uses `PostEngineInit` loading phase and calls `OnPostEngineInit()` directly (the delegate has already fired by this point). Initialization order:

| Step | Action |
|------|--------|
| 1 | Register editor commands + UI extensions |
| 2 | Initialize `FOliveEditorChatSession` singleton (owns `ConversationManager`) |
| 3 | `FOliveValidationEngine::Get().RegisterCoreRules()` |
| 4 | `FOliveProjectIndex::Get().Initialize()` |
| 5 | `FOliveToolRegistry::Get().RegisterBuiltInTools()` (6 project tools) |
| 6 | `FOliveNodeCatalog::Get().Initialize()` → `FOliveBlueprintToolHandlers::Get().RegisterAllTools()` |
| 7 | BT tools → PCG tools (guarded by availability check) → C++ tools → CrossSystem tools |
| 8 | `FOliveFocusProfileManager::Get().Initialize()` (after all tool registration) |
| 9 | `FOliveToolPackManager::Get().Initialize()` |
| 10 | Prompt assembler + MCP prompt templates |
| 11 | `FOliveMCPServer::Get().Start()` (if `bAutoStartMCPServer`) |

---

## Key Singletons

All accessed via `static Foo& Get()`:

| Singleton | Responsibility |
|-----------|---------------|
| `FOliveToolRegistry` | Central tool registry with 95 tools (thread-safe via `FRWLock`) |
| `FOliveMCPServer` | HTTP JSON-RPC server on ports 3000–3009 |
| `FOliveProjectIndex` | Asset registry wrapper, search, class hierarchy (`FTickableEditorObject`) |
| `FOliveNodeCatalog` | Blueprint node catalog with fuzzy matching and category indices |
| `FOliveWritePipeline` | 6-stage write safety pipeline |
| `FOliveValidationEngine` | 22 validation rules (thread-safe via `FRWLock`) |
| `FOliveTransactionManager` | `FScopedTransaction` wrapper with nesting support |
| `FOliveFocusProfileManager` | Focus profile filtering for tool visibility |
| `FOliveToolPackManager` | Dynamic tool pack gating per turn |
| `FOliveEditorChatSession` | Editor-lifetime session (owns `ConversationManager`) |
| `FOlivePromptAssembler` | System prompt assembly with template composition |
| `FOliveRunManager` | Run mode with steps, checkpoints, and rollback |
| `FOliveBlueprintReader` | Blueprint IR serialization (summary + full + paged) |
| `FOliveBlueprintWriter` | Blueprint asset/variable/function mutations |
| `FOliveGraphWriter` | Blueprint graph node/pin operations with node ID cache |
| `FOliveNodeFactory` | Node creation factory with 24 supported node types |
| `FOlivePinConnector` | Pin connection with compatibility checking and conversion |
| `FOliveCompileManager` | Blueprint compilation with error pattern matching |
| `FOliveAssetResolver` | Asset path resolution with redirector following |
| `FOliveProviderRetryManager` | Provider retry with exponential backoff |

---

## Intermediate Representation (IR) System

The IR system provides a typed, JSON-serializable representation of all Unreal asset types. It lives in the `OliveAIRuntime` module.

### Type Hierarchy

```
OliveIRTypes.h (base)
├── CommonIR.h (graph/node/pin)
│   ├── BlueprintIR.h
│   ├── BehaviorTreeIR.h
│   ├── PCGIR.h
│   └── CppIR.h
├── OliveCompileIR.h (compile results)
│   └── BlueprintPlanIR.h
└── OliveIRSchema.h (validation)
```

### Core Types

| Struct | Purpose | Key Fields |
|--------|---------|------------|
| `FOliveIRType` | Type descriptor | Category (28 types), ClassName, bIsReference |
| `FOliveIRPin` | Pin on a graph node | Name, Type, bIsInput, bIsExec, DefaultValue, Connections |
| `FOliveIRNode` | Graph node | Id, Type, Category (40 categories), InputPins, OutputPins |
| `FOliveIRGraph` | Function/event graph | Name, GraphType, Nodes, NodeCount, ConnectionCount |
| `FOliveIRVariable` | Blueprint variable | Name, Type, DefaultValue, flags (replicated, save game, etc.) |
| `FOliveIRComponent` | Component in hierarchy | Name, ComponentClass, bIsRoot, Children, Properties |
| `FOliveIRFunctionSignature` | Function metadata | Name, Inputs, Outputs, access/purity/const flags |
| `FOliveIRBlueprint` | Complete Blueprint | 12 types, ParentClass, Capabilities, Interfaces, Variables, Components, Graphs |

### Domain-Specific IR

| Domain | Key Types |
|--------|-----------|
| **Blueprint Plan** | `FOliveIRBlueprintPlanStep` (StepId, Op, Target, Inputs, ExecAfter), `FOliveIRBlueprintPlan`, `FOliveIRBlueprintPlanResult` |
| **Behavior Tree** | `FOliveIRBTNode` (recursive children/decorators/services), `FOliveIRBehaviorTree`, `FOliveIRBlackboard`, `FOliveIRBlackboardKey` |
| **PCG** | `FOliveIRPCGNode` (with position), `FOliveIRPCGGraph`, `FOliveIRPCGEdge`, 17 data types |
| **C++** | `FOliveIRCppClass` (properties, functions, flags), `FOliveIRCppEnum`, `FOliveIRCppStruct`, `FOliveIRCppSourceFile` |
| **Compile** | `FOliveIRCompileResult` (success, errors, warnings, timing) |

### Plan JSON Operations (Closed Vocabulary)

The Blueprint Plan IR uses a fixed set of operations in `namespace OlivePlanOps`:

`call`, `get_var`, `set_var`, `branch`, `sequence`, `cast`, `event`, `custom_event`, `for_loop`, `for_each_loop`, `delay`, `is_valid`, `print_string`, `spawn_actor`, `make_struct`, `break_struct`, `return`, `comment`

### Schema Validation (Rules R1-R8)

`FOliveIRValidator` enforces structural rules on IR data. Schema version: `1.0`. Plan schema version: up to `2.0`.

---

## MCP Server & JSON-RPC Transport

### Server

`FOliveMCPServer` runs an HTTP JSON-RPC 2.0 server on the `/mcp` endpoint.

| Property | Value |
|----------|-------|
| Protocol version | `2024-11-05` |
| Port range | 3000–3009 (auto-discovers first available) |
| Max clients | 10 |
| Client timeout | 300 seconds |
| Event buffer | 500 events, 300s retention |
| Thread safety | Tool calls dispatch to game thread via `AsyncTask(ENamedThreads::GameThread)` |

**States:** `Stopped` → `Starting` → `Running` → `Stopping` → `Error`

### MCP Bridge (mcp-bridge.js)

A zero-dependency Node.js script that converts Claude Code CLI's stdio JSON-RPC to HTTP requests against the MCP server:

- Auto-discovers server on ports 3000–3009 (500ms timeout per port)
- Accepts `--port` and `--host` CLI args
- Logging to `stderr` (stdout reserved for protocol)
- Configuration: `.mcp.json` at project root

### JSON-RPC Error Codes

| Code | Name |
|------|------|
| -32700 | Parse Error |
| -32600 | Invalid Request |
| -32601 | Method Not Found |
| -32602 | Invalid Params |
| -32603 | Internal Error |
| -32000 | Tool Not Found |
| -32001 | Tool Execution Error |
| -32002 | Resource Not Found |
| -32003 | Not Initialized |
| -32004 | Server Busy |
| -32005 | Timeout |

---

## Tool Registry & Tool Catalog (95 Tools)

All tools are registered in `FOliveToolRegistry` and exposed via both the chat panel and MCP server. Each tool has a name, description, JSON Schema input schema, handler function, tags, and category.

### Blueprint Tools (30)

#### Reader Tools (7)
| Tool | Description |
|------|-------------|
| `blueprint.read` | Read Blueprint structure (summary or full) |
| `blueprint.read_function` | Read a specific function graph |
| `blueprint.read_event_graph` | Read the event graph |
| `blueprint.read_variables` | Read variable list |
| `blueprint.read_components` | Read component hierarchy |
| `blueprint.read_hierarchy` | Read inheritance hierarchy |
| `blueprint.list_overridable_functions` | List parent functions that can be overridden |

#### Asset Writer Tools (6)
| Tool | Description |
|------|-------------|
| `blueprint.create` | Create a new Blueprint asset |
| `blueprint.set_parent_class` | Change parent class |
| `blueprint.add_interface` | Add a Blueprint interface |
| `blueprint.remove_interface` | Remove a Blueprint interface |
| `blueprint.compile` | Compile a Blueprint |
| `blueprint.delete` | Delete a Blueprint asset |

#### Variable Writer Tools (3)
| Tool | Description |
|------|-------------|
| `blueprint.add_variable` | Add a variable |
| `blueprint.remove_variable` | Remove a variable |
| `blueprint.modify_variable` | Modify a variable (type, default, flags) |

#### Component Writer Tools (4)
| Tool | Description |
|------|-------------|
| `blueprint.add_component` | Add a component |
| `blueprint.remove_component` | Remove a component |
| `blueprint.modify_component` | Modify component properties |
| `blueprint.reparent_component` | Reparent a component in hierarchy |

#### Function Writer Tools (6)
| Tool | Description |
|------|-------------|
| `blueprint.add_function` | Add a function |
| `blueprint.remove_function` | Remove a function |
| `blueprint.modify_function_signature` | Modify function inputs/outputs |
| `blueprint.add_event_dispatcher` | Add an event dispatcher |
| `blueprint.override_function` | Override a parent class function |
| `blueprint.add_custom_event` | Add a custom event |

#### Graph Writer Tools (6)
| Tool | Description |
|------|-------------|
| `blueprint.add_node` | Add a node to a graph |
| `blueprint.remove_node` | Remove a node from a graph |
| `blueprint.connect_pins` | Connect two pins |
| `blueprint.disconnect_pins` | Disconnect two pins |
| `blueprint.set_pin_default` | Set a pin's default value |
| `blueprint.set_node_property` | Set a node property |

#### Plan JSON Tools (2, settings-gated)
| Tool | Description |
|------|-------------|
| `blueprint.preview_plan_json` | Preview a batch plan JSON without executing |
| `blueprint.apply_plan_json` | Apply a batch plan JSON to a graph |

### Animation Blueprint Tools (4)

| Tool | Description |
|------|-------------|
| `animbp.add_state_machine` | Add a state machine to AnimGraph |
| `animbp.add_state` | Add a state to a state machine |
| `animbp.add_transition` | Add a transition between states |
| `animbp.set_transition_rule` | Set transition rule/condition |

### Widget Blueprint Tools (4)

| Tool | Description |
|------|-------------|
| `widget.add_widget` | Add a widget to a Widget Blueprint |
| `widget.remove_widget` | Remove a widget |
| `widget.set_property` | Set a widget property |
| `widget.bind_property` | Bind a widget property to a function |

### Behavior Tree Tools (10)

| Tool | Description |
|------|-------------|
| `behaviortree.create` | Create a Behavior Tree asset |
| `behaviortree.read` | Read a Behavior Tree |
| `behaviortree.set_blackboard` | Set the Blackboard for a BT |
| `behaviortree.add_composite` | Add a composite node (Selector/Sequence) |
| `behaviortree.add_task` | Add a task node |
| `behaviortree.add_decorator` | Add a decorator |
| `behaviortree.add_service` | Add a service |
| `behaviortree.remove_node` | Remove a node |
| `behaviortree.move_node` | Move a node in the tree |
| `behaviortree.set_node_property` | Set a node property |

### Blackboard Tools (6)

| Tool | Description |
|------|-------------|
| `blackboard.create` | Create a Blackboard asset |
| `blackboard.read` | Read a Blackboard |
| `blackboard.add_key` | Add a key |
| `blackboard.remove_key` | Remove a key |
| `blackboard.modify_key` | Modify a key |
| `blackboard.set_parent` | Set parent Blackboard |

### PCG Tools (9)

| Tool | Description |
|------|-------------|
| `pcg.create` | Create a PCG graph |
| `pcg.read` | Read a PCG graph |
| `pcg.add_node` | Add a node |
| `pcg.remove_node` | Remove a node |
| `pcg.connect` | Connect nodes |
| `pcg.disconnect` | Disconnect nodes |
| `pcg.set_settings` | Set node settings |
| `pcg.add_subgraph` | Add a subgraph node |
| `pcg.execute` | Execute a PCG graph |

### C++ Tools (13)

#### Readers (8)
| Tool | Description |
|------|-------------|
| `cpp.read_class` | Read a C++ class via reflection |
| `cpp.list_blueprint_callable` | List BlueprintCallable functions |
| `cpp.list_overridable` | List overridable functions |
| `cpp.read_enum` | Read a C++ enum |
| `cpp.read_struct` | Read a C++ struct |
| `cpp.read_header` | Read a header file |
| `cpp.read_source` | Read a source file |
| `cpp.list_project_classes` | List all project C++ classes |

#### Writers (5)
| Tool | Description |
|------|-------------|
| `cpp.create_class` | Create a new C++ class |
| `cpp.add_property` | Add a UPROPERTY to a class |
| `cpp.add_function` | Add a UFUNCTION to a class |
| `cpp.modify_source` | Modify a source file |
| `cpp.compile` | Trigger live coding compile |

### Project Tools (19)

#### Index Tools (6)
| Tool | Description |
|------|-------------|
| `project.search` | Search for assets by name |
| `project.get_asset_info` | Get detailed asset info |
| `project.get_class_hierarchy` | Get class inheritance hierarchy |
| `project.get_dependencies` | Get asset dependencies |
| `project.get_referencers` | Get assets referencing an asset |
| `project.get_config` | Get project configuration |

#### Cross-System Tools (13)
| Tool | Description |
|------|-------------|
| `project.bulk_read` | Read multiple assets at once |
| `project.implement_interface` | Add interface implementation |
| `project.refactor_rename` | Rename asset/symbol across project |
| `project.create_ai_character` | Create full AI character (BP + BT + BB) |
| `project.move_to_cpp` | Move Blueprint logic to C++ |
| `project.snapshot` | Take asset snapshot |
| `project.list_snapshots` | List available snapshots |
| `project.rollback` | Rollback to a snapshot |
| `project.diff` | Diff against a snapshot |
| `project.batch_write` | Execute multiple writes atomically |
| `project.index_build` | Rebuild the project index |
| `project.index_status` | Get project index status |
| `project.get_relevant_context` | Get relevant context for a task |

### Tool Count Summary

| Category | Count |
|----------|-------|
| `blueprint.*` | 30 |
| `animbp.*` | 4 |
| `widget.*` | 4 |
| `behaviortree.*` | 10 |
| `blackboard.*` | 6 |
| `pcg.*` | 9 |
| `cpp.*` | 13 |
| `project.*` | 19 |
| **Total** | **95** |

---

## Write Pipeline (6 Stages)

Every mutation operation flows through `FOliveWritePipeline`:

```
FOliveWriteRequest
    │
    ▼
┌─────────┐   ┌─────────┐   ┌──────────┐   ┌─────────┐   ┌────────┐   ┌────────┐
│ Validate │──▶│ Confirm │──▶│ Transact │──▶│ Execute │──▶│ Verify │──▶│ Report │
└─────────┘   └─────────┘   └──────────┘   └─────────┘   └────────┘   └────────┘
     │              │              │              │              │           │
 Validation    Tier routing    FScopedTrans.  Actual       Structural   FOliveWrite
 engine +      (T1:auto,      + Modify()     mutation     checks +     Result →
 preconditions T2:plan+conf,                 via delegate optional     ToolResult
               T3:preview)                                compile
```

| Stage | Purpose |
|-------|---------|
| **Validate** | Run validation engine rules, check preconditions |
| **Confirm** | Route by confirmation tier (Tier 1: auto, Tier 2: plan+confirm, Tier 3: preview). MCP clients skip this. |
| **Transact** | Open `FScopedTransaction` + call `Modify()` on target |
| **Execute** | Invoke the `FOliveWriteExecutor` delegate (the actual mutation) |
| **Verify** | Structural checks + optional compile (`bAutoCompile`) + orphaned exec-flow detection |
| **Report** | Build `FOliveWriteResult` with timing, then convert via `.ToToolResult()` |

### Confirmation Tiers

| Tier | Risk Level | Example Operations | UX |
|------|-----------|-------------------|-----|
| Tier 1 | Low | Add variable, add component, create empty BP | Auto-execute |
| Tier 2 | Medium | Create function with logic, wire event graph | Plan → Confirm |
| Tier 3 | High | Refactor, delete, reparent | Non-destructive preview |

### Write Result

`FOliveWriteResult` includes: success flag, completed stage, confirmation requirement, plan description, preview data, validation messages, result data, compile result, execution time, created items/node IDs. Factory methods: `Success()`, `ValidationError()`, `ExecutionError()`, `ConfirmationNeeded()`.

---

## Blueprint Subsystem

The largest sub-module, handling all Blueprint reading, writing, and plan-based construction.

### Reader Layer

| Class | Responsibility |
|-------|---------------|
| `FOliveBlueprintReader` | Top-level reader: summary, full, variables, components, functions, hierarchy, overridables |
| `FOliveGraphReader` | Graph-level: full read, summary, paged read (threshold: 500 nodes, page size: 100) |
| `FOliveNodeSerializer` | Individual node → `FOliveIRNode` |
| `FOlivePinSerializer` | Pin → `FOliveIRPin` with type mapping |
| `FOliveComponentReader` | Component hierarchy → `FOliveIRComponent` tree |
| `FOliveAnimGraphSerializer` | Animation Blueprint state machines → `FOliveIRAnimStateMachine` |
| `FOliveWidgetTreeSerializer` | Widget Blueprint widget tree → `FOliveIRWidgetNode` |

### Writer Layer

| Class | Responsibility |
|-------|---------------|
| `FOliveBlueprintWriter` | Asset operations (create, delete, duplicate, reparent), variable CRUD, function CRUD, compilation |
| `FOliveGraphWriter` | Node add/remove, pin connect/disconnect/defaults, node ID cache management |
| `FOliveNodeFactory` | 24 node types with creator functions (Branch, Sequence, ForLoop, CallFunction, etc.) |
| `FOlivePinConnector` | Pin connection with type compatibility checking and conversion node insertion |
| `FOliveComponentWriter` | Component add/remove/modify/reparent |
| `FOliveAnimGraphWriter` | State machine/state/transition creation |
| `FOliveWidgetWriter` | Widget add/remove/property set/bind |

### Plan JSON System

The Plan JSON system enables intent-level graph construction. Instead of individual add_node/connect_pins calls, the AI describes operations at a semantic level:

```json
{
  "schema_version": "2.0",
  "steps": [
    { "id": "s1", "op": "event", "target": "BeginPlay" },
    { "id": "s2", "op": "call", "target": "PrintString", "inputs": { "InString": "Hello" }, "exec_after": "s1" }
  ]
}
```

**Resolution Pipeline:**

| Phase | Class | Purpose |
|-------|-------|---------|
| 1 | `FOliveBlueprintPlanResolver` | Validate plan, resolve step targets to UE types |
| 2 | `FOliveFunctionResolver` | Resolve function names (exact → K2 prefix → alias → catalog fuzzy → broad search) |
| 3 | `FOlivePlanExecutor` | Create nodes, build pin manifests, wire exec/data, set defaults, auto-layout |
| 4 | `FOlivePinManifest` | Per-node pin inventory with smart lookup (exact → display → case-insensitive → fuzzy → type-match) |
| 5 | `FOliveGraphLayoutEngine` | Automatic node positioning |

**Data Wire Syntax:**
- `@step_id.auto` — auto-match by type
- `@step_id.~hint` — fuzzy pin name match
- `@step_id.PinName` — exact pin name

### Blueprint Type System

`FOliveBlueprintTypeDetector` identifies 14 Blueprint types and returns capability flags:

| Type | Variables | Components | Event Graph | Functions | AnimGraph | Widget Tree |
|------|-----------|------------|-------------|-----------|-----------|-------------|
| Normal | Yes | Yes | Yes | Yes | No | No |
| Interface | No | No | No | Yes (signatures only) | No | No |
| FunctionLibrary | No | No | No | Yes (static only) | No | No |
| AnimationBlueprint | Yes | No | Yes | Yes | Yes | No |
| WidgetBlueprint | Yes | No | Yes | Yes | No | Yes |
| ActorComponent | Yes | No | Yes | Yes | No | No |

### Node Catalog

`FOliveNodeCatalog` indexes all available Blueprint node types with:
- Full-text fuzzy search with scoring
- Category, class, and tag indices
- JSON export for AI context injection
- Pin template data for each node type

---

## Behavior Tree Subsystem

| Class | Responsibility |
|-------|---------------|
| `FOliveBTNodeCatalog` | BT node type catalog |
| `FOliveBehaviorTreeReader` | BT → `FOliveIRBehaviorTree` with recursive node hierarchy |
| `FOliveBlackboardReader` | Blackboard → `FOliveIRBlackboard` with key list |
| `FOliveBTNodeSerializer` | Individual BT node → `FOliveIRBTNode` |
| `FOliveBehaviorTreeWriter` | Create BT, set blackboard, add/remove/move nodes |
| `FOliveBlackboardWriter` | Create blackboard, add/remove/modify keys, set parent |
| `FOliveBTNodeFactory` | BT node creation (composite, task, decorator, service) |

---

## PCG Subsystem

| Class | Responsibility |
|-------|---------------|
| `FOlivePCGNodeCatalog` | PCG node type catalog |
| `FOlivePCGReader` | PCG graph → `FOliveIRPCGGraph` with nodes and edges |
| `FOlivePCGWriter` | Create graph, add/remove nodes, connect/disconnect, set settings, add subgraph, execute |
| `FOlivePCGAvailability` | Runtime check for PCG plugin availability |

---

## C++ Subsystem

| Class | Responsibility |
|-------|---------------|
| `FOliveCppReflectionReader` | Read UClass/UEnum/UStruct via UE reflection → `FOliveIRCppClass` etc. |
| `FOliveCppSourceReader` | Read .h/.cpp source files → `FOliveIRCppSourceFile` |
| `FOliveCppSourceWriter` | Create classes, add properties/functions, modify source, trigger live coding |

---

## Cross-System Operations

| Class | Responsibility |
|-------|---------------|
| `FOliveMultiAssetOperations` | Bulk read, interface implementation, rename refactoring, AI character creation, BP→C++ migration |
| `FOliveSnapshotManager` | Asset snapshot/restore, listing, diffing |
| `FOliveGraphBatchExecutor` | Atomic batch execution of multiple write operations |
| `FOliveMCPPromptTemplates` | Prompt template loading from `Content/SystemPrompts/` |

---

## Brain Layer (State Machine)

`FOliveBrainLayer` manages AI execution state through a state machine:

```
     ┌────────────────────────────────────────────┐
     │                                            │
     ▼                                            │
   Idle ──▶ Planning ──▶ WorkerActive ──▶ Completed
     │                       │     │              ▲
     │                       │     │              │
     │                       ▼     ▼              │
     │              AwaitingConfirmation           │
     │                       │                    │
     │                       ▼                    │
     └───────────── Cancelling ──────────────────┘
                       │
                       ▼
                     Error
```

### Worker Phases (during `WorkerActive`)

`Streaming` → `ExecutingTools` → `Compiling` → `SelfCorrecting` → `Complete`

### Self-Correction System

| Component | Purpose |
|-----------|---------|
| `FOliveSelfCorrectionPolicy` | Evaluates tool/compile failures → `Continue`, `FeedBackErrors`, or `StopWorker` |
| `FOliveLoopDetector` | Detects repeated errors and A→B→A oscillation patterns |
| `FOliveRetryPolicy` | Configurable limits: 3 retries/error, 5 correction cycles/worker, 2 worker failures |

### Operation History

`FOliveOperationHistoryStore` records every tool call with:
- Sequence number, run ID, worker domain, step index
- Tool name, parameters, result, status
- Affected assets and timestamps
- Token-budget-aware prompt summary generation (3-tier detail levels)

### Tool Packs

`FOliveToolPackManager` gates tool visibility per turn based on intent:

| Pack | Tools | Activated When |
|------|-------|----------------|
| `read_pack` | 18 reader tools | Always available |
| `write_pack_basic` | 18 basic write tools | Turn has explicit write intent |
| `write_pack_graph` | 16 graph editing tools | Turn has explicit write intent |
| `danger_pack` | 4 destructive tools | Turn has danger intent |

### Prompt Distiller

`FOlivePromptDistiller` manages context window pressure:
- Keeps recent N message pairs at full detail (default: 2)
- Truncates older tool results to 4000 chars
- Estimates tokens at 4 chars/token
- Never touches System or User messages

---

## AI Provider System (8 Providers)

All providers implement `IOliveAIProvider`:

```cpp
interface IOliveAIProvider {
    SendMessage(messages, tools, onChunk, onToolCall, onComplete, onError)
    CancelRequest()
    ValidateConnection(callback)
    GetAvailableModels() / GetRecommendedModel()
    Configure(config) / GetConfig()
}
```

| Provider | Class | API Key Required | Notes |
|----------|-------|-----------------|-------|
| Claude Code | `FOliveClaudeCodeProvider` | No | Default, uses CLI |
| OpenRouter | `FOliveOpenRouterProvider` | Yes | Multi-model gateway |
| Z.ai | `FOliveZAIProvider` | Yes | Optional coding endpoint |
| Anthropic | `FOliveAnthropicProvider` | Yes | Direct Claude API |
| OpenAI | `FOliveOpenAIProvider` | Yes | GPT models |
| Google | `FOliveGoogleProvider` | Yes | Gemini models |
| Ollama | `FOliveOllamaProvider` | No | Local models |
| OpenAI Compatible | `FOliveOpenAICompatibleProvider` | Optional | Custom endpoint |

### Retry Manager

`FOliveProviderRetryManager` wraps any provider with automatic retry:

| Error Class | Behavior |
|-------------|----------|
| `Terminal` | No retry (auth failures, invalid requests) |
| `Transient` | Exponential backoff (1s, 2s, 4s) up to max retries |
| `RateLimited` | Wait for `Retry-After` header (max 120s), then retry once |
| `Truncated` | Terminal (context too large) |

Error classification uses parseable prefix: `[HTTP:{code}:RetryAfter={s}]`

---

## Chat System

### Conversation Manager

`FOliveConversationManager` is the central coordinator:

- Owns message history, brain layer, loop detector, self-correction policy
- Dispatches tool calls to `FOliveToolRegistry`
- Manages confirmation flow (pending confirmation token)
- Tracks turn intent flags (`bTurnHasExplicitWriteIntent`, `bTurnHasDangerIntent`)
- Token tracking (100K context budget)
- Deferred focus profile switching

**Key delegates:** `OnMessageAdded`, `OnStreamChunk`, `OnToolCallStarted`, `OnToolCallCompleted`, `OnProcessingStarted`, `OnProcessingComplete`, `OnError`, `OnConfirmationRequired`, `OnDeferredProfileApplied`

### Editor Chat Session

`FOliveEditorChatSession` is an editor-lifetime singleton that:
- Owns the `ConversationManager`, `MessageQueue`, and `RetryManager`
- Survives panel open/close (chat panel holds only a `TWeakPtr`)
- Supports background completions with toast notifications

### Run Manager

`FOliveRunManager` provides structured multi-step execution:
- Start/pause/resume/cancel runs
- Step tracking with tool call recording
- Automatic checkpoints (configurable interval, default every 5 steps)
- Rollback to checkpoint via snapshot manager
- Step-level retry and skip

### Message Queue

`FOliveMessageQueue` — unbounded FIFO for queuing user messages while the AI is processing. Soft warning threshold at 5 messages.

---

## Focus Profiles & Tool Packs

### Focus Profiles

Focus profiles filter tool visibility and inject domain-specific system prompt additions:

| Profile | Tool Categories | Description |
|---------|----------------|-------------|
| Auto | All | No filtering |
| Blueprint | Blueprint, Widget, AnimBP | Blueprint-focused development |
| AI & Behavior | Blueprint, BT, Blackboard | AI character development |
| Level & PCG | Blueprint, PCG | Level design and procedural content |
| C++ & Blueprint | Blueprint, C++ | Mixed C++/Blueprint development |

Custom profiles can be created, saved, and loaded (schema version 1, stored in settings JSON).

### Tool Packs

Tool packs (`Config/OliveToolPacks.json`) define named sets of tools:

```json
{
    "read_pack": ["project.search", "blueprint.read", ...],
    "write_pack_basic": ["blueprint.create", "blueprint.add_variable", ...],
    "write_pack_graph": ["blueprint.add_node", "blueprint.connect_pins", ...],
    "danger_pack": ["blueprint.delete", "blueprint.set_parent_class", ...]
}
```

`FOliveToolPackManager` gates packs per turn based on intent detection from the conversation manager.

---

## Validation Engine

`FOliveValidationEngine` runs 22 rules before every write operation:

### Core Rules
| Rule | Purpose |
|------|---------|
| `PIEProtection` | Block writes during Play-In-Editor |
| `SchemaValidation` | Validate tool parameters against JSON Schema |
| `AssetExists` | Verify target asset exists (for non-create operations) |
| `WriteRateLimit` | Sliding window rate limiter (default 30 ops/min) |
| `DuplicateLayer` | Detect duplicate operations |
| `CppOnlyMode` | Block non-C++ operations when in C++-only mode |

### Blueprint Rules
| Rule | Purpose |
|------|---------|
| `BPAssetType` | Verify asset is a Blueprint |
| `BPNodeIdFormat` | Validate node ID format |
| `BPNaming` | Enforce naming conventions |

### Behavior Tree Rules
| Rule | Purpose |
|------|---------|
| `BTAssetType` | Verify asset is a Behavior Tree |
| `BTNodeExists` | Verify target node exists |
| `BBKeyUnique` | Prevent duplicate Blackboard keys |

### PCG Rules
| Rule | Purpose |
|------|---------|
| `PCGAssetType` | Verify asset is a PCG graph |
| `PCGNodeClass` | Verify PCG node class exists |

### C++ Rules
| Rule | Purpose |
|------|---------|
| `CppPathSafety` | Prevent path traversal attacks |
| `CppClassExists` | Verify C++ class exists |
| `CppEnumExists` | Verify C++ enum exists |
| `CppStructExists` | Verify C++ struct exists |
| `CppCompileGuard` | Guard against compile during active compilation |

### Cross-System Rules
| Rule | Purpose |
|------|---------|
| `BulkReadLimit` | Cap bulk read operations |
| `SnapshotExists` | Verify snapshot exists before rollback |
| `RefactorSafety` | Safety checks for rename refactoring |

---

## Services Layer

| Service | Purpose |
|---------|---------|
| `FOliveTransactionManager` | Wraps `FScopedTransaction` with nesting support. `OLIVE_SCOPED_TRANSACTION` macro skips inner transactions during batch execution. |
| `FOliveAssetResolver` | Resolves asset paths with redirector following (max depth 10), editor state detection, class filtering |
| `FOliveBatchExecutionScope` | RAII scope that sets thread-local flag to suppress inner transactions during atomic batch operations |
| `FOliveErrorBuilder` | Static utility for building structured JSON error/success responses with 13 standardized error codes |
| `OliveToolParamHelpers` | Namespace with typed parameter extraction helpers (`GetRequiredString`, `GetOptionalBool`, etc.) |

---

## Project Index

`FOliveProjectIndex` (`FTickableEditorObject`, ticks at 0.5s) maintains a live index of the project:

- **Asset Index:** `TMap<FString, FOliveAssetInfo>` — path → metadata (class, parent, interfaces, deps, referencers, type flags)
- **Class Hierarchy:** `TMap<FName, FOliveClassHierarchyNode>` — with parent/child relationships
- **Incremental Updates:** Queued from asset registry events, processed on tick
- **Search:** Full-text asset search with scoring
- **JSON Export:** Search results, asset info, class hierarchy, project config — all serialized for AI consumption
- **Project Map:** Exportable snapshot of the entire index

---

## UI Layer (Slate)

### Chat Panel (`SOliveAIChatPanel`)

Dockable editor tab registered as `OliveAIChatPanel`. Contains:
- **Message List** (`SOliveAIMessageList`) — Scrolling message display with user/assistant/system/error messages, tool call indicators with status updates, result cards, run headers, and confirmation widgets
- **Context Bar** (`SOliveAIContextBar`) — Shows active context assets and focus profile
- **Input Field** (`SOliveAIInputField`) — Text input with send/cancel controls
- **Provider/Model Selectors** — Combo boxes for switching providers and models
- **Status Display** — Processing indicator, retry countdown, rate limit warnings, queue depth

### Key UI Features
- Streaming message display (chunks appended as they arrive)
- Tool call tracking (running/completed/failed status with icons)
- Confirmation dialogs for Tier 2/3 operations
- Run mode visualization (step headers, pause/resume/cancel controls)
- Background completion toast notifications when panel is closed

---

## System Prompts & Knowledge

### Prompt Architecture

```
BaseSystemPrompt.txt          ← Root identity + capabilities
    + Base.txt                ← 11 universal rules (read before write, compile after change, etc.)
    + ProfileBlueprint.txt    ← Focus profile overlay (if applicable)
    + blueprint_authoring.txt ← Knowledge pack (reliability rules)
    + {CONTEXT_PLACEHOLDER}   ← Active asset context (runtime injected)
    + {TOOLS_PLACEHOLDER}     ← Available tools (runtime injected)
```

### Worker Prompts (used in Run Mode)

| Prompt | Domain | Key Content |
|--------|--------|-------------|
| `Worker_Blueprint.txt` | Blueprint | Plan JSON v2.0 format, graph editing workflow, self-correction patterns |
| `Worker_BehaviorTree.txt` | BT | Tree structure patterns, common node classes, Blackboard setup |
| `Worker_PCG.txt` | PCG | Standard graph flow, common nodes, distribution patterns |
| `Worker_Cpp.txt` | C++ | UCLASS/UPROPERTY/UFUNCTION specifiers, naming conventions |
| `Worker_Integration.txt` | Cross-system | BP+BT, BP+PCG, BP+C++ integration patterns |

### Knowledge Packs

| File | Content |
|------|---------|
| `blueprint_authoring.txt` | 8 reliability rules for safe Blueprint editing |

### Orchestrator Prompt (Unused)

`Orchestrator.txt` — Multi-domain planning orchestrator. Currently unused (Brain Layer uses single-loop approach).

---

## Settings Reference

`UOliveAISettings` (`UDeveloperSettings`, section: Project > Plugins > Olive AI Studio)

### AI Provider Settings

| Setting | Default | Range |
|---------|---------|-------|
| Provider | ClaudeCode | 8 options |
| SelectedModel | `anthropic/claude-sonnet-4` | — |
| Temperature | 0.0 | 0–2 |
| MaxTokens | 4096 | 256–128000 |
| RequestTimeoutSeconds | 120 | 30–600 |
| MaxProviderRetries | 3 | 0–5 |
| MaxRetryAfterWaitSeconds | 120 | 0–300 |

### MCP Server Settings

| Setting | Default | Range |
|---------|---------|-------|
| bAutoStartMCPServer | true | — |
| MCPServerPort | 3000 | 1024–65535 |
| MaxMCPConnections | 5 | 1–10 |

### UI Settings

| Setting | Default |
|---------|---------|
| bShowOperationFeed | true |
| bShowQuickActions | true |
| bNotifyOnCompletion | true |
| bPlaySoundOnCompletion | false |
| bAutoScrollChat | true |
| DefaultFocusProfile | "Auto" |

### Confirmation Settings

| Setting | Default |
|---------|---------|
| VariableOperationsTier | Tier1_AutoExecute |
| ComponentOperationsTier | Tier1_AutoExecute |
| FunctionCreationTier | Tier2_PlanConfirm |
| GraphEditingTier | Tier2_PlanConfirm |
| RefactoringTier | Tier3_Preview |
| DeleteOperationsTier | Tier3_Preview |
| SafetyPreset | Careful |

### Safety Presets

| Preset | Effect |
|--------|--------|
| Careful | Default tiers (conservative) |
| Fast | Reduces confirmation requirements |
| YOLO | Minimal confirmation |

### Policy Settings

| Setting | Default | Range |
|---------|---------|-------|
| MaxVariablesPerBlueprint | 50 | 10–200 |
| MaxNodesPerFunction | 100 | 20–500 |
| bEnforceNamingConventions | true | — |
| bAutoCompileAfterWrite | true | — |
| MaxWriteOpsPerMinute | 30 | 0–120 |
| CheckpointIntervalSteps | 5 | 0–50 |

### Brain Layer Settings

| Setting | Default | Range |
|---------|---------|-------|
| BatchWriteMaxOps | 200 | 1–1000 |
| RelevantContextMaxAssets | 10 | 1–50 |
| PromptDistillationRawResults | 2 | 1–5 |
| MaxCorrectionCyclesPerRun | 5 | 1–20 |

### Blueprint Plan JSON Settings

| Setting | Default | Range |
|---------|---------|-------|
| bEnableBlueprintPlanJsonTools | false | — |
| PlanJsonMaxSteps | 128 | 1–512 |
| bPlanJsonRequirePreviewForApply | true | — |
| bEnforcePlanFirstGraphRouting | true | — |
| PlanFirstGraphRoutingThreshold | 3 | 1–50 |

---

## File Inventory

### By Extension

| Extension | Count | Description |
|-----------|-------|-------------|
| `.cpp` | 107 | C++ implementation files |
| `.h` | 105 | C++ header files |
| `.md` | 33 | Documentation, plans, agent prompts |
| `.txt` | 10 | System prompts (7 active + 3 .bak) |
| `.json` | 9 | Config, schemas, examples |
| `.png` | 5 | Icon + chat images |
| `.cs` | 2 | UBT Build.cs files |
| `.js` | 1 | MCP bridge |
| `.ini` | 1 | Plugin config |
| `.uplugin` | 1 | Plugin manifest |

**Total tracked files: ~283**

### Source File Distribution

| Sub-module | .h files | .cpp files | Total |
|------------|----------|------------|-------|
| OliveAIRuntime/IR | 9 | 9 | 18 |
| OliveAIEditor (core) | 28 | 31 | 59 |
| Blueprint | 25 | 25 | 50 |
| BehaviorTree | 6 | 6 | 12 |
| PCG | 5 | 5 | 10 |
| Cpp | 5 | 5 | 10 |
| CrossSystem | 6 | 6 | 12 |
| Tests | 0 | 3 | 3 |
| **Total** | **84+** | **90+** | **174+** |

---

## Configuration Files

| File | Purpose |
|------|---------|
| `UE_Olive_AI_Studio.uplugin` | Plugin manifest (version, modules, plugin dependencies) |
| `Config/DefaultOliveAI.ini` | Default settings (provider, MCP, UI, confirmation, policy, brain, plan) |
| `Config/OliveToolPacks.json` | Tool pack definitions (read, write_basic, write_graph, danger) |
| `.mcp.json` | MCP server configuration for Claude Code CLI |
| `package.json` | Node.js package for mcp-bridge.js (no dependencies) |

---

## Build System

### Build Command

```bash
"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" \
    UE_Olive_AI_ToolkitEditor Win64 Development \
    "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" \
    -WaitMutex
```

Incremental builds take ~5-6 seconds.

### Module Dependencies

**OliveAIRuntime** (minimal):
- `Core`, `CoreUObject`, `Json`, `JsonUtilities`

**OliveAIEditor** (30+ dependencies):
- **Editor:** UnrealEd, EditorFramework, EditorStyle, EditorSubsystem, ToolMenus, LevelEditor, MainFrame, WorkspaceMenuStructure
- **UI:** Slate, SlateCore, InputCore, PropertyEditor
- **Networking:** HTTP, HTTPServer, Sockets, Networking
- **Data:** Json, JsonUtilities
- **Assets:** AssetRegistry, AssetTools, ContentBrowser
- **Blueprint:** BlueprintGraph, Kismet, KismetWidgets, GraphEditor, Blutility
- **Animation:** AnimGraph, AnimGraphRuntime
- **Widget:** UMG, UMGEditor
- **AI:** AIModule, GameplayTasks
- **PCG:** PCG
- **Config:** Projects, DeveloperSettings, Settings, SettingsEditor
- **C++:** GameProjectGeneration, LiveCoding, SourceCodeAccess

### Plugin Dependencies

- `EditorScriptingUtilities`
- `PCG`

### Log Files

| Log | Path |
|-----|------|
| Project log | `../../Saved/Logs/UE_Olive_AI_Toolkit.log` |
| UBT log | `C:/Users/<User>/AppData/Local/UnrealBuildTool/Log.txt` |

### Tests

UE Automation Framework tests in `Source/OliveAIEditor/Private/Tests/`:
- `Brain/OliveBrainLayerTests.cpp`
- `Conversation/OliveConversationManagerTests.cpp`
- `FocusProfiles/OliveFocusProfileTests.cpp`
- Filter: `OliveAI.*` in Session Frontend > Automation
