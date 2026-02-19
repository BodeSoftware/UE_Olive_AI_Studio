# Unreal Engine AI Agent Plugin — Implementation Plan

> **Status:** Pre-development  
> **Target Engine:** Unreal Engine 5.5+  
> **Architecture:** Model B — Built-in Chat UI + MCP Server (Editor-only)  
> **Author:** [Your Name]  
> **Last Updated:** February 2026

---

## Table of Contents

1. [Vision & Goals](#1-vision--goals)
2. [Key Design Decisions](#2-key-design-decisions)
3. [Architecture Overview](#3-architecture-overview)
4. [Chat UI & User Experience](#4-chat-ui--user-experience)
5. [Agent System & Focus Profiles](#5-agent-system--focus-profiles)
6. [Confirmation & Autonomy System](#6-confirmation--autonomy-system)
7. [Context System](#7-context-system)
8. [AI Provider Integration](#8-ai-provider-integration)
9. [AI Planning Engine (Brain Layer)](#9-ai-planning-engine-brain-layer)
10. [Policy & Rule System (Control Layer)](#10-policy--rule-system-control-layer)
11. [Phase 0 — Foundation](#11-phase-0--foundation)
12. [Phase 1 — Blueprints](#12-phase-1--blueprints)
13. [Phase 2 — Behavior Trees & Blackboards](#13-phase-2--behavior-trees--blackboards)
14. [Phase 3 — PCG Graphs](#14-phase-3--pcg-graphs)
15. [Phase 4 — C++ Integration](#15-phase-4--c-integration)
16. [Phase 5 — Cross-System Intelligence](#16-phase-5--cross-system-intelligence)
17. [Edge Cases & Failure Modes](#17-edge-cases--failure-modes)
18. [Testing Strategy](#18-testing-strategy)
19. [Timeline & Milestones](#19-timeline--milestones)

---

## 1. Vision & Goals

### What This Is

A dual-interface AI development assistant for Unreal Engine. It provides a built-in chat panel inside the editor for most users AND an MCP server for power users who want to connect external agents like Claude Code or Cursor. Both interfaces share the same tool layer, validation, and execution engine underneath.

Think of it as "Claude Code for Unreal Engine" — an AI that understands your project, can read and modify Blueprints, Behavior Trees, PCG graphs, and C++ code through natural conversation, with an agentic loop that plans, executes, checks, and self-corrects.

### What This Is Not

- Not locked to a specific AI provider — built-in chat supports OpenRouter, direct API keys (Anthropic, OpenAI, Google), and local models (Ollama). MCP supports any compatible external agent.
- Not a runtime/shipping component — editor-only
- Not a black box — users see what the AI is doing in real-time via the operation feed
- Not auto-pilot — hybrid confirmation system ensures the user stays in control

### Core Principles

1. **Dual interface.** Built-in chat for accessibility, MCP for extensibility. Same tools underneath.
2. **Read-heavy, write-careful.** Reading is cheap and safe. Every write goes through validation, transaction wrapping, and compilation verification.
3. **Surgical edits over recreation.** The AI makes granular changes (add one node, connect two pins) rather than regenerating entire graphs. This minimizes corruption risk.
4. **Show the work.** The operation feed shows every tool call in real-time. The user always knows what the AI is doing.
5. **Context is king.** Layered context system (auto + @mentions + right-click + drag-drop) gives the AI structural awareness without overwhelming the context window.
6. **Fail safely.** Every operation that can fail must fail gracefully with a structured error, a suggestion for the AI, and no asset corruption.
7. **Single smart agent, not many dumb ones.** One conversational agent with focus profiles, not 8 separate specialized agents that lose context between switches.

### Success Criteria

- An AI agent can create a Blueprint with components, variables, and wired-up function logic from a natural language description
- An AI agent can read any Blueprint in a project and accurately explain what it does
- An AI agent can modify existing Blueprints without corrupting them, with full undo support
- The agentic loop can plan, execute, compile, detect errors, and self-correct without user intervention
- All of the above works through both the built-in chat and external MCP clients
- Users can see every operation happening in real-time and stop/undo at any point

---

## 2. Key Design Decisions

These decisions were made after analyzing competing products (NeoStack AI, Ultimate Engine Co-Pilot, Aura) and evaluating tradeoffs.

| Decision | Choice | Rationale |
|----------|--------|-----------|
| **Architecture** | Model B: Built-in chat + MCP server | Built-in chat for most users (NeoStack proved this is needed). MCP for power users with Claude Code/Cursor. Both share the same tool layer. |
| **Confirmation** | Hybrid 3-tier system | Auto-execute for low-risk ops, plan-confirm for medium, non-destructive comparison for high-risk. Configurable per user. Balances speed and safety better than any competitor. |
| **Provider** | BYOK — multiple providers (built-in chat) + MCP (external agents) | No infrastructure costs. Built-in chat supports OpenRouter, direct Anthropic/OpenAI/Google APIs, and local Ollama. MCP lets Claude Code/Cursor/Codex connect directly. User picks what works for them. |
| **Autonomy** | Adaptive, tied to confirmation tiers | High for creation, medium for modification, low for deletion. Live operation feed shows work in real-time. Better than NeoStack's pure auto-execute or Aura's binary Ask/Agent. |
| **Context** | Layered: auto + @mentions + right-click + drag-drop | Auto-context as baseline, explicit methods for additional assets. Covers all approaches competitors use. |
| **Chat UX** | Dockable Slate tab panel with operation feed | Standard dockable panel like Output Log. Operation feed is the key differentiator — shows every tool call live. Quick action buttons for common tasks. |
| **Agent System** | Single agent with Focus profiles | One conversation, one agent. Focus dropdown filters tool set (Auto/Blueprint/AI & Behavior/etc.). No multi-agent switching. Evolve toward internal routing later. |
| **Agentic Loop** | Plan → Execute → Check → Fix → Repeat | Self-correcting loop similar to Claude Code. Compile after writes, parse errors, feed back to AI. This IS the "agent" behavior — not separate agents. |

### Competitive Positioning

| Feature | NeoStack | Ultimate Co-Pilot | Aura | Our Plugin |
|---------|----------|-------------------|------|------------|
| Built-in chat | ✅ OpenRouter | ✅ API key (added later) | ✅ Proprietary | ✅ OpenRouter |
| External agents (MCP) | ✅ | ✅ | ❌ | ✅ |
| Confirmation workflow | Auto-execute only | Non-destructive refactor | Ask/Agent mode toggle | Hybrid 3-tier |
| Operation visibility | Limited | Limited | Limited | Full operation feed |
| Pricing | $110 BYOK | One-time BYOK | Subscription + credits | One-time BYOK |
| Agent system | Profiles/subconfigs | Single | Editor/Coding split | Focus profiles |
| Self-correction | Depends on external agent | Limited | ✅ Server-side | ✅ Built-in agentic loop |

---

## 3. Architecture Overview

### System Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                      UNREAL EDITOR PROCESS                       │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │              Chat UI (Dockable Slate Panel)                 │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐  │  │
│  │  │ Message  │ │ Context  │ │ Operation│ │ Focus       │  │  │
│  │  │ History  │ │ Bar      │ │ Feed     │ │ Dropdown    │  │  │
│  │  └────┬─────┘ └────┬─────┘ └────┬─────┘ └──────┬───────┘  │  │
│  │       └─────────────┴────────────┴──────────────┘          │  │
│  └────────────────────────────┬───────────────────────────────┘  │
│                               │                                  │
│  ┌────────────────────────────▼───────────────────────────────┐  │
│  │              Conversation Manager                           │  │
│  │  Session state, message history, streaming, API calls       │  │
│  │  Provider abstraction (OpenRouter / Anthropic / OpenAI /    │  │
│  │  Google / Ollama)                                          │  │
│  └────────────────────────────┬───────────────────────────────┘  │
│                               │                                  │
│  ┌────────────────────────────▼───────────────────────────────┐  │
│  │              MCP Server (for external agents)               │  │
│  │  JSON-RPC transport (HTTP on localhost)                     │  │
│  │  Same tool/resource registry as internal chat               │  │
│  └────────────────────────────┬───────────────────────────────┘  │
│                               │                                  │
│  ┌────────────────────────────▼───────────────────────────────┐  │
│  │              AI Planning Engine (Brain Layer)               │  │
│  │  Agentic loop, context assembly, operation planning,        │  │
│  │  feedback loops, intent detection                           │  │
│  └────────────────────────────┬───────────────────────────────┘  │
│                               │                                  │
│  ┌────────────────────────────▼───────────────────────────────┐  │
│  │              Confirmation Manager                           │  │
│  │  Routes operations through appropriate tier                 │  │
│  │  (auto-execute / plan-confirm / non-destructive)            │  │
│  └────────────────────────────┬───────────────────────────────┘  │
│                               │                                  │
│  ┌────────────────────────────▼───────────────────────────────┐  │
│  │              Policy & Rule System (Control Layer)           │  │
│  │  Validation, constraints, safety, rate limiting             │  │
│  └────────────────────────────┬───────────────────────────────┘  │
│                               │                                  │
│  ┌────────────────────────────▼───────────────────────────────┐  │
│  │              Tool Router                                    │  │
│  │  Routes validated tool calls to correct subsystem            │  │
│  └──┬──────────┬──────────┬──────────┬────────────────────────┘  │
│     │          │          │          │                            │
│  ┌──▼───┐  ┌──▼───┐  ┌──▼───┐  ┌──▼────────┐                   │
│  │  BP  │  │  BT  │  │  PCG │  │  C++      │                   │
│  │Module│  │Module│  │Module│  │  Module    │                   │
│  └──┬───┘  └──┬───┘  └──┬───┘  └──┬────────┘                   │
│     │         │         │         │                              │
│  ┌──▼─────────▼─────────▼─────────▼───────────────────────────┐  │
│  │              Shared Services Layer                          │  │
│  │  • Transaction Manager    • Asset Resolver                  │  │
│  │  • Validation Engine      • IR Serializer                   │  │
│  │  • Project Index          • Dependency Tracker              │  │
│  │  • Compile Manager        • Error Reporter                  │  │
│  └────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
      │                                    │
      │ Built-in Chat                      │ MCP Protocol
      │ (User's chosen provider)           │ (HTTP on localhost)
      ▼                                    ▼
┌──────────────┐                   ┌─────────────────────┐
│  AI Provider │                   │  Any MCP Client     │
│  (user picks)│                   │  Claude Code        │
│              │                   │  Cursor / Windsurf  │
│  • OpenRouter│                   │  Codex CLI          │
│  • Anthropic │                   │  Gemini CLI         │
│  • OpenAI    │                   │  Custom agents      │
│  • Google    │                   └─────────────────────┘
│  • Ollama    │
└──────────────┘
```

### Module Structure

```
Plugins/
└── AIAgentPlugin/
    ├── AIAgentPlugin.uplugin
    ├── Source/
    │   ├── AIAgentRuntime/              # Minimal runtime module
    │   │   └── Public/IR/              # IR struct definitions
    │   │       ├── BlueprintIR.h
    │   │       ├── BehaviorTreeIR.h
    │   │       ├── PCGIR.h
    │   │       └── CommonIR.h
    │   │
    │   └── AIAgentEditor/              # Editor-only module
    │       ├── Public/
    │       │   ├── UI/                 # Chat panel, operation feed, context bar
    │       │   ├── Chat/              # Conversation manager, provider abstraction
    │       │   ├── MCP/               # MCP server, protocol, transport
    │       │   ├── Brain/             # AI Planning Engine, agentic loop
    │       │   ├── Confirmation/      # Confirmation tier routing
    │       │   ├── Context/           # Context assembly, @mentions, auto-context
    │       │   ├── Policy/            # Policy & Rule System
    │       │   ├── Services/          # Shared services
    │       │   ├── Blueprint/         # Blueprint module
    │       │   ├── BehaviorTree/      # BT module
    │       │   ├── PCG/               # PCG module
    │       │   ├── Cpp/               # C++ integration module
    │       │   └── Index/             # Project index
    │       └── Private/
    │           └── (mirrors Public)
    │
    ├── Content/
    │   └── SystemPrompts/             # Focus profile system prompts
    │
    └── Config/
        └── DefaultAIAgent.ini
```

### Technology Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Language | C++ | Required for deep UE API access |
| Transport (MCP) | Streamable HTTP on localhost | Uses `FHttpServerModule`, easier than stdio inside UE |
| Transport (Chat) | HTTPS to OpenRouter API | Standard REST API calls via `FHttpModule` |
| JSON | `FJsonObject` / `FJsonSerializer` | UE built-in, no external deps |
| Chat UI | Slate | Native UE editor toolkit, dockable tabs |
| External deps | None required | Everything needed is in UE already |
| Loading phase | `PostEngineInit` | Needs editor subsystems available |
| Build type | Editor-only | Does not ship with packaged builds |

---

## 4. Chat UI & User Experience

### Panel Layout

The chat panel is a dockable Slate tab, opened from **Tools > AI Agent Chat** (or toolbar button). It can dock anywhere the user wants — beside the Blueprint editor, at the bottom, as a sidebar. Default dock position: right side, similar to where Detail panels go.

```
┌─────────────────────────────────────────────────┐
│ AI Agent Chat                    [Focus: Auto ▾] │
├─────────────────────────────────────────────────┤
│ Context: BP_PlayerCharacter │ @BB_Enemy │ ✕     │  ← Context Bar
├─────────────────────────────────────────────────┤
│                                                 │
│  You: Create a health pickup that heals the     │
│  player for 25 HP on overlap                    │
│                                                 │
│  AI: I'll create BP_HealthPickup with the       │
│  following structure...                         │
│                                                 │
│  ┌─ Operation Feed ──────────────────────────┐  │
│  │ ✅ Created BP_HealthPickup (parent: AActor)│  │
│  │ ✅ Added StaticMeshComponent "MeshComp"    │  │
│  │ ✅ Added SphereComponent "OverlapSphere"   │  │
│  │ ✅ Added variable: HealAmount (float=25)   │  │
│  │ 🔄 Creating function: OnOverlapBegin...    │  │  ← Live progress
│  │    ✅ Added OnComponentBeginOverlap event   │  │
│  │    ✅ Added Cast to BP_PlayerCharacter      │  │
│  │    ✅ Connected exec flow                   │  │
│  │    ✅ Added CallFunction: Heal              │  │
│  │ ✅ Compiled successfully                    │  │
│  └───────────────────────────────────────────┘  │
│                                                 │
│  AI: Done! Created BP_HealthPickup with a       │
│  sphere overlap that heals the player.          │
│  [Open Blueprint]                               │
│                                                 │
├─────────────────────────────────────────────────┤
│ [Explain] [Review] [Fix Errors]  ← Quick Actions│
├─────────────────────────────────────────────────┤
│ [📎] Type a message or @ to mention assets...   │
│                                           [Send]│
└─────────────────────────────────────────────────┘
```

### UI Components

**Context Bar (top):**
- Shows all assets currently visible to the AI
- Auto-populated with whatever asset is open in the editor
- Shows @mentioned assets with ✕ to remove
- Clicking an item opens it in the editor
- Drag assets from Content Browser to add them

**Message History (main area):**
- Streaming text display with markdown rendering
- AI responses include inline operation feeds (collapsible)
- Clickable asset links that open in the editor
- Error messages styled distinctly (red border, suggestion text)
- Confirmation prompts for Tier 2/3 operations with [Approve] [Edit Plan] [Cancel] buttons

**Operation Feed (inline, collapsible):**
- Shows every tool call as it executes
- Each line: status icon (✅🔄❌⏸) + operation description
- Collapsible per-operation — expand to see details (parameters, return values)
- Click any line to undo just that operation
- [Stop] button to halt multi-step operations mid-execution
- Lines are color-coded: green=success, blue=in-progress, red=error, yellow=warning

**Quick Actions (above input):**
- Contextual buttons that change based on what's selected/open
- With Blueprint open: [Explain] [Review] [Fix Errors] [Add Feature]
- With BT open: [Explain] [Review] [Add Behavior]
- With nothing open: [Create Blueprint] [Create BT] [Search Project]
- Users can hide these if they prefer

**Input Area (bottom):**
- Multi-line text input
- @ mention autocomplete (type `@` to search assets by name)
- 📎 button for drag-drop file/asset attachment
- Send button / Enter to submit (Shift+Enter for newline)
- Shows character/token estimate for cost awareness

### Editor Integration Points

**Right-click context menu additions:**
- Blueprint node selected → "Ask AI about this" / "Refactor this" / "Explain this"
- Variable in My Blueprint panel → "Ask AI about this variable"
- Component in Components panel → "Ask AI to configure this"
- Asset in Content Browser → "Send to AI Chat"

**Auto-context triggers:**
- Opening/switching Blueprint tabs updates the context bar
- Selecting nodes in the graph highlights them in context
- Compilation errors auto-populate into context
- Switching to a different editor (BT, Material) updates context

**Navigation from chat:**
- Asset names in AI responses are clickable → opens asset in editor
- "Open Blueprint" links after creation
- Error references link to the specific node/line

**Notifications:**
- Toast notification when AI completes a long operation (if chat panel isn't focused)
- Taskbar flash on completion (configurable)
- Optional sound on completion (configurable)

---

## 5. Agent System & Focus Profiles

### Philosophy: One Agent, Focused

The plugin uses a single conversational agent with optional Focus profiles. The user talks to one AI in one continuous conversation. Context carries across the entire session — if you discuss your game's architecture, the AI remembers it for all subsequent tasks.

Focus profiles are lightweight tool-set filters, not separate agents. Switching profiles doesn't reset the conversation or lose context. It only changes which tools the AI can call and optionally adjusts the system prompt emphasis.

### Focus Profiles

| Profile | Tools Available | System Prompt Emphasis | When to Use |
|---------|----------------|----------------------|-------------|
| **Auto** (default) | All tools | Balanced — general UE development | Most of the time. AI figures out what tools to use. |
| **Blueprint** | Blueprint tools only | Blueprint patterns, node types, graph architecture | Deep BP work. Reduces noise from other tool schemas. |
| **AI & Behavior** | Blueprint + BT + Blackboard | Game AI patterns, BT design, Blackboard architecture | Building AI systems. Keeps Blueprint tools since BTs reference BPs. |
| **Level & PCG** | Blueprint + PCG tools | Procedural content, level design patterns | PCG graph work and level population. |
| **C++ & Blueprint** | Blueprint + C++ tools | C++ to Blueprint bridging, reflected API, code generation | Mixed C++/BP workflows. |
| **Everything** | All tools, explicit | All domains equally weighted | Complex cross-system tasks. Same as Auto functionally, signals intent. |

**Implementation:**
- Profiles are defined in config files (JSON) that specify: tool whitelist, system prompt additions, default context scope
- Switching profiles mid-conversation only changes the tool schema sent in subsequent API calls
- The conversation history and user context persist across profile switches
- Users can create custom profiles with their own tool selections

### The Agentic Loop

This is the core "agent" behavior — not multiple agents, but a single agent that operates autonomously with a self-correcting execution loop.

```
User Request
    │
    ▼
[1. Plan]          Brain Layer creates an execution plan
    │               "I'll need to create a BP, add 3 variables,
    │                create 2 functions, wire up the event graph"
    │
    ▼
[2. Confirm]       Confirmation Manager routes through appropriate tier
    │               Tier 1: auto-execute
    │               Tier 2: show plan, wait for approval
    │               Tier 3: preview non-destructively
    │
    ▼
[3. Execute]       Tools execute one by one
    │               Operation feed shows live progress
    │               Each operation: validate → transact → execute → report
    │
    ▼
[4. Check]         After execution batch:
    │               - Compile Blueprint
    │               - Check for structural issues
    │               - Verify dependencies
    │
    ├── Success ──→ Report results to user
    │
    └── Errors ──→ [5. Self-Correct]
                    │  AI reads compile errors
                    │  AI determines fix
                    │  Loop back to step 3
                    │  (max 3 correction attempts)
                    │
                    └── Still failing → Report errors to user
                                        with suggestions
```

**Self-correction rules:**
- Max 3 auto-correction attempts per error
- Same fix attempted twice = stop and ask user
- Correction attempts are shown in the operation feed
- User can intervene at any point during the loop by typing or clicking [Stop]

### Evolution Path

```
Phase 1-3:    Single agent + Focus profiles (tool filtering)
Phase 5+:     Brain Layer internally routes to domain-specific 
              sub-prompts (user doesn't see this — same conversation)
Future:       Parallel internal sub-tasks for complex multi-asset 
              operations (still one conversation to the user)
```

The architecture supports evolving toward internal multi-agent routing without changing the user experience. The Focus profiles and Brain Layer are designed to grow into this.

---

## 6. Confirmation & Autonomy System

### Three-Tier Confirmation

Every write operation is classified into a risk tier that determines the confirmation flow.

#### Tier 1: Auto-Execute (Low Risk)

Operations that are trivially undoable, low impact, and clearly intentional.

**Operations in this tier:**
- Adding a variable
- Adding a component
- Creating a new empty Blueprint/BT/Blackboard
- Renaming an asset, variable, or function
- Adding a function stub (signature only, empty body)
- Setting a property value
- Setting a pin default value
- Adding a Blackboard key

**UX:** Execute immediately. Show result in operation feed. One Ctrl+Z to undo.

#### Tier 2: Plan → Confirm → Execute (Medium Risk)

Operations that involve multiple steps, are harder to undo atomically, or modify existing logic.

**Operations in this tier:**
- Creating a function with graph logic (adding multiple nodes and connections)
- Wiring up an event graph
- Creating a multi-asset set (BP + BT + Blackboard)
- Adding/implementing an interface (creates stub functions)
- Modifying an existing function's logic
- Changing a function signature that has callers

**UX:** The AI presents a plan in natural language:

```
AI: Here's my plan for the TakeDamage function:

1. Create function "TakeDamage" with inputs: DamageAmount (float), 
   DamageType (TSubclassOf<UDamageType>)
2. Add a Branch checking if DamageAmount > 0
3. On True: subtract DamageAmount from Health variable
4. Call OnHealthChanged event dispatcher
5. Add a second Branch checking if Health <= 0
6. On True: call Die() function

[Approve] [Edit Plan] [Cancel]
```

User clicks [Approve] to execute, [Edit Plan] to modify the plan in text, or [Cancel] to abort. Once approved, execution proceeds with the operation feed showing live progress.

#### Tier 3: Non-Destructive Comparison (High Risk)

Operations that could break existing functionality, affect other assets, or are difficult to fully undo.

**Operations in this tier:**
- Refactoring existing function logic
- Reparenting a Blueprint
- Deleting assets
- Changing a variable type that has existing Get/Set nodes
- Modifying a Blueprint Interface function signature
- Bulk operations across multiple assets

**UX:** The AI creates the new version alongside the original:

For refactoring: creates a new function with the improved logic, leaving the original untouched. User can compare side-by-side in the editor, then choose to replace or discard.

For destructive operations: shows an impact analysis first:

```
AI: Deleting BP_EnemyBase would affect:
- BP_Goblin (child Blueprint)
- BP_Skeleton (child Blueprint)  
- BP_EnemySpawner (references BP_EnemyBase)
- Level_Forest (3 placed instances)

This cannot be easily undone. Proceed?

[Delete All] [Show Details] [Cancel]
```

### Tier Configuration

Users can override the default tier for any operation category in settings:

| Setting | Default | Options |
|---------|---------|---------|
| Variable operations | Tier 1 | Tier 1 / Tier 2 |
| Component operations | Tier 1 | Tier 1 / Tier 2 |
| Function creation | Tier 2 | Tier 1 / Tier 2 |
| Graph editing | Tier 2 | Tier 1 / Tier 2 |
| Refactoring | Tier 3 | Tier 2 / Tier 3 |
| Asset deletion | Tier 3 | Tier 2 / Tier 3 |
| Reparenting | Tier 3 | Tier 2 / Tier 3 |
| Multi-asset operations | Tier 2 | Tier 1 / Tier 2 / Tier 3 |
| Global override | Per-operation | All Tier 1 (yolo) / All Tier 2 (careful) / Per-operation |

Power users can set everything to Tier 1 for maximum speed. Cautious users can set everything to Tier 2/3. The default "per-operation" setting uses the recommended tier for each operation type.

---

## 7. Context System

### Layered Context Architecture

Context is assembled from multiple sources in priority order. The Brain Layer manages the total context budget and truncates intelligently when needed.

```
┌─────────────────────────────────────────────┐
│              Context Assembly                │
│                                             │
│  Layer 1: Auto-Context (passive)            │
│  ├── Currently open asset in editor         │
│  ├── Selected nodes in graph (if any)       │
│  ├── Current compilation errors             │
│  └── Active Focus profile constraints       │
│                                             │
│  Layer 2: @Mentions (explicit)              │
│  ├── User-attached assets via @mention      │
│  ├── Persists in context bar until removed  │
│  └── Can @mention multiple assets           │
│                                             │
│  Layer 3: Right-Click (targeted)            │
│  ├── "Ask AI about this" from context menu  │
│  ├── Attaches specific nodes/items          │
│  └── One-shot — used for the next message   │
│                                             │
│  Layer 4: Drag-Drop (quick)                 │
│  ├── Drag from Content Browser to chat      │
│  ├── Adds to context bar (persistent)       │
│  └── Same behavior as @mention              │
│                                             │
│  Layer 5: Project Index (ambient)           │
│  ├── Always available as MCP resource       │
│  ├── AI queries on-demand for project info  │
│  └── Class hierarchy, dependencies, search  │
│                                             │
│  Layer 6: Viewport (visual, optional)       │
│  ├── Screenshot of current viewport         │
│  ├── Triggered manually or by AI request    │
│  └── Lower priority — for visual tasks      │
│                                             │
└─────────────────────────────────────────────┘
```

### Context Budget Management

The Brain Layer tracks approximate token usage of assembled context and manages the budget:

**For small context (< 20% of model window):** Include everything verbatim — full IR of all context assets.

**For medium context (20-60%):** Include full IR of the primary asset (auto-context), summary IR of @mentioned assets, and full detail on demand.

**For large context (> 60%):** Summarize everything. Primary asset gets function signatures + variable list but not full graph nodes. @mentioned assets get name + type + interface only. AI can request detail on specific functions with `blueprint.read_function`.

**Pagination for huge assets:** A Blueprint with 500 nodes in one function doesn't get dumped into context. Instead:
- Blueprint-level read returns metadata, variables, components, function signatures
- AI calls `blueprint.read_function` for specific functions it needs to see
- Graph detail is loaded on-demand, not upfront

### @Mention Autocomplete

When the user types `@` in the input field:

1. Show a dropdown with fuzzy search across all project assets
2. Filter by asset type prefixes:
   - `@BP_` → Blueprints
   - `@BT_` → Behavior Trees
   - `@BB_` → Blackboards
   - `@M_` → Materials
   - `@PCG_` → PCG graphs
   - No prefix → search all types
3. Show asset icon, name, path, and type in the dropdown
4. Selected asset appears in the Context Bar and stays until removed
5. Multiple @mentions are supported

### Auto-Context Behavior

| Editor State | What's Auto-Attached |
|---|---|
| Blueprint editor open | The open Blueprint (summary IR) |
| Nodes selected in graph | The selected nodes + their connections |
| BT editor open | The open Behavior Tree + its Blackboard |
| PCG editor open | The open PCG graph |
| Compilation errors present | Error list with source references |
| Content Browser focused | Current folder path |
| Nothing specific open | Project index summary only |

Auto-context updates on editor state changes with a short debounce (500ms) to avoid rapid-fire context rebuilds when clicking around.

---

## 8. AI Provider Integration

### Built-in Chat: Multiple Provider Options

The built-in chat supports multiple AI providers through a provider abstraction layer. Users pick whichever option works best for them.

**Supported Providers:**

| Provider | API Key Needed | Models Available | Best For |
|----------|---------------|-----------------|----------|
| **OpenRouter** (recommended default) | OpenRouter key | 400+ models (Claude, GPT, Gemini, DeepSeek, Llama, etc.) | Most users. One key covers everything. Free models available for experimentation. |
| **Anthropic (Direct)** | Anthropic API key | Claude Opus, Sonnet, Haiku | Users who already have an Anthropic API key or want lowest latency to Claude. |
| **OpenAI (Direct)** | OpenAI API key | GPT-4o, GPT-4.1, o3, etc. | Users who prefer OpenAI or already have a key. |
| **Google (Direct)** | Google AI Studio key | Gemini Pro, Gemini Flash | Users who prefer Gemini or want free tier access. |
| **Ollama (Local)** | None (localhost) | Llama, Mistral, DeepSeek, etc. | Privacy-conscious users, offline use, no API costs. Requires Ollama installed. |

All providers go through the same abstraction:

```cpp
class IProviderClient
{
public:
    virtual void SendMessage(
        const FConversationContext& Context,
        const TArray<FToolDefinition>& Tools,
        FOnStreamChunk OnChunk,
        FOnToolCall OnToolCall,
        FOnComplete OnComplete,
        FOnError OnError
    ) = 0;
    
    virtual void CancelRequest() = 0;
};

class FOpenRouterClient : public IProviderClient { ... };
class FAnthropicClient : public IProviderClient { ... };
class FOpenAIClient : public IProviderClient { ... };
class FGoogleClient : public IProviderClient { ... };
class FOllamaClient : public IProviderClient { ... };
```

**Implementation priority:**
1. OpenRouter — Phase 0 (covers everything through one integration)
2. Anthropic Direct — Phase 0 (Claude is the recommended model, direct access is valuable)
3. OpenAI Direct — Phase 1
4. Google Direct — Phase 1
5. Ollama — Phase 2

**Configuration in settings:**
- Provider dropdown (OpenRouter / Anthropic / OpenAI / Google / Ollama)
- API key per provider (stored securely in platform keychain, not in config files)
- Model selection dropdown (populated based on provider)
- Recommended models highlighted: Claude Sonnet/Opus for best results
- Max tokens per request
- Temperature (default 0 for deterministic tool use)
- Ollama endpoint URL (default `http://localhost:11434`)

**Model recommendation in UI:**
When the user first configures the plugin, show a recommendation panel:

> **For best results, we recommend Claude Sonnet 4.5 or Claude Opus.**
> Claude models handle complex multi-step Blueprint operations significantly better than other models. You can use cheaper models like DeepSeek or GPT-4o Mini for simple questions and exploration.
> 
> **Quickest setup:** OpenRouter (one API key covers all models).
> **Best performance:** Anthropic direct with Claude Sonnet or Opus.

### External Agents: MCP Server

The MCP server runs on a configurable localhost port (default 3000) and accepts connections from any MCP-compatible client.

**What external agents get:**
- Same tool registry as the built-in chat
- Same resource registry (project index, node catalogs, etc.)
- Same validation and transaction wrapping
- Same operation feed (logged, available via MCP notifications)

**What external agents don't get:**
- The chat UI (they have their own)
- The confirmation system (external agents manage their own approval flow)
- The Focus profiles (external agents see all tools)
- The context assembly (external agents manage their own context)

**MCP Protocol Support:**
- Tools: Full tool registry with JSON Schema inputs
- Resources: Project index, asset metadata, node catalogs
- Prompts: Pre-built prompt templates (explain, review, plan)
- Notifications: Operation progress, compilation results

### System Prompt Architecture

The system prompt is assembled from components:

```
┌──────────────────────────────────┐
│ Base System Prompt               │  Always included. Core identity,
│ (identity, capabilities,        │  safety rules, response format,
│  general UE knowledge)          │  tool-calling instructions.
├──────────────────────────────────┤
│ Focus Profile Prompt             │  Added based on active profile.
│ (domain-specific patterns,      │  "You are currently focused on
│  best practices, common tasks)  │  Blueprint development..."
├──────────────────────────────────┤
│ Project Context                  │  Engine version, enabled plugins,
│ (from Project Index)            │  project structure summary.
├──────────────────────────────────┤
│ Active Context                   │  Current asset summaries from
│ (from Context System)           │  the context assembly layers.
└──────────────────────────────────┘
```

System prompts are stored as editable text files in the plugin's Content directory. Advanced users can customize them.

---

## 9. AI Planning Engine (Brain Layer)

The Brain Layer sits between the conversation/MCP interface and the actual tool execution. It provides intelligence around how operations are planned, contextualized, and executed.

### 9.1 Context Assembly

When the AI calls any tool, the Brain Layer enriches the request with relevant context before execution.

**How it works:**

1. Tool call arrives (e.g., `blueprint.add_node` on `BP_PlayerCharacter`)
2. Brain Layer queries the Project Index:
   - What is this Blueprint's parent class?
   - What interfaces does it implement?
   - What variables and functions already exist?
   - What other assets reference it?
3. Brain Layer attaches relevant context to the tool response

**Context categories (in priority order):**

1. **Immediate** — The asset being operated on (always included)
2. **Inherited** — Parent class API, interface requirements (included when modifying)
3. **Sibling** — Other assets of same type in same folder (included on request)
4. **Dependent** — Assets that reference this one (summarized, detail on request)
5. **Global** — Engine version, plugins, project conventions (always available)

### 9.2 Operation Planning

Some tool calls require multi-step execution. The Brain Layer handles sequencing.

**Example: `blueprint.add_interface`**

1. Adds the interface to the Blueprint
2. Identifies all required functions from the interface
3. Creates stub function graphs for each
4. Reports: "Added interface BPI_Damageable. Created 3 stub functions: TakeDamage, GetHealth, IsDead. These need implementation."

**Example: Complex function creation**

When the AI wants to create a function with logic, instead of one monolithic call, it produces a plan:

1. Create function signature
2. Add nodes one by one
3. Connect pins
4. Compile and check

The Brain Layer coordinates this sequence, maintains state between steps, and handles failures at any step.

### 9.3 Feedback Loops

After write operations, the Brain Layer triggers verification:

**Auto-compile (configurable):**
- After each complete function or significant change batch, compile the Blueprint
- Parse errors into structured data
- Return errors with the tool response so the AI can self-correct
- Track error patterns to detect when the AI is stuck in a loop (same error 3+ times = stop)

**Structural verification:**
- After connecting pins, verify type compatibility
- After adding nodes, verify execution flow continuity
- After modifying a variable type, check existing references

### 9.4 Operation History

Per-session log of all operations:

```json
{
  "session_id": "abc-123",
  "operations": [
    {
      "sequence": 1,
      "tool": "blueprint.create",
      "params": {"name": "BP_HealthPickup", "parent": "AActor"},
      "result": "success",
      "tier": 1,
      "snapshot_id": "snap_001",
      "timestamp": "2026-02-18T10:30:00Z"
    }
  ]
}
```

Enables: AI referencing what it already did, batch rollback, debugging.

### 9.5 Intent Detection

The Brain Layer infers intent from patterns and proactively assists:

- AI connects float output to int input → auto-insert conversion node if possible
- AI creates 5 variables but none are referenced → suggest they may need Get/Set nodes
- AI removes a function called by other Blueprints → warn about dependencies
- AI makes the same failing call 3 times → intervene with suggestion to try a different approach

### 9.6 Context Cache

Recently-read asset IRs are cached to avoid re-serializing the same Blueprint multiple times per session. Cache invalidation: any write to the asset, external modification, manual clear, or 10-minute timeout.

---

## 10. Policy & Rule System (Control Layer)

The Control Layer enforces rules, constraints, and safety guarantees. Every operation passes through it before execution.

### 10.1 Validation Pipeline

Every write tool call passes through this pipeline:

```
Tool Call Received
    │
    ▼
[1. Schema Validation]     Does the call match the JSON schema?
    │                       → Reject with parameter error if not
    ▼
[2. Asset Resolution]      Does the target asset exist and is it loaded?
    │                       → Reject with "asset not found" if not
    ▼
[3. Type Constraints]      Is this operation valid for this asset type?
    │                       → "Blueprint Interfaces cannot have variables"
    ▼
[4. Structural Rules]      Would this create an invalid state?
    │                       → "Pin types incompatible: float → bool"
    ▼
[5. Dependency Check]      Would this break other assets?
    │                       → Warning: "3 Blueprints call this function"
    ▼
[6. Policy Rules]          Does this violate project policies?
    │                       → "Max 50 variables per Blueprint exceeded"
    ▼
[7. Confirmation Routing]  Which tier does this operation belong to?
    │                       → Route to appropriate confirmation flow
    ▼
[8. Rate Limiting]         Too many operations too fast?
    │                       → Throttle with retry hint
    ▼
[Execute Operation]
    │
    ▼
[9. Post-Execution Check]  Did the operation produce a valid result?
    │                       → Compile check, structural integrity
    ▼
[Return Result]
```

### 10.2 Type Constraint Rules

Hard rules based on Blueprint type. Cannot be overridden.

**Blueprint Interface (BPTYPE_Interface):**
- Cannot have variables, components, event graphs, function bodies, or macros
- Functions must be public

**Blueprint Function Library (BPTYPE_FunctionLibrary):**
- Cannot have event graphs, components, or member variables
- All functions must be static

**Blueprint Macro Library (BPTYPE_MacroLibrary):**
- Can only contain macro graphs

**Animation Blueprint:**
- Must have a valid skeleton reference
- K2 nodes don't go in AnimGraph, anim nodes don't go in EventGraph
- Cannot add components (no SCS)

**Widget Blueprint:**
- Widget tree hierarchy must follow valid parent-child rules
- Root widget must be a panel type

### 10.3 Structural Rules

**Pin Connection Rules:**
- Exec pins only connect to exec pins
- Data pins must be type-compatible (with implicit conversion awareness)
- One output exec pin → one input exec pin
- One output data pin → multiple input data pins
- One input data pin → one output data pin only
- No circular execution flows
- No self-connections

**Node Validity Rules:**
- CallFunction nodes must reference an existing function
- Get/Set variable nodes must reference an existing variable
- Event nodes (BeginPlay, Tick) can only exist once per event graph
- Custom events must have unique names

**Function Signature Rules:**
- Valid FName (no spaces, special characters)
- Unique within the Blueprint
- Override functions must match parent signature

### 10.4 Dependency Rules (Warnings, Not Blocks)

- Delete a variable → "Variable 'Health' is referenced by 4 nodes in 2 functions"
- Delete a function → "Function 'GetHealth' is called by BP_HUD and BP_AIController"
- Change a function signature → "Will break 2 call sites in BP_EnemyBase"
- Reparent a Blueprint → "Overridden functions may not exist on new parent"

### 10.5 Project Policies (Configurable)

| Policy | Default | Description |
|--------|---------|-------------|
| `MaxVariablesPerBlueprint` | 50 | Warn on excess |
| `MaxNodesPerFunction` | 100 | Warn on overly large functions |
| `MaxFunctionsPerBlueprint` | 30 | Warn on excessive complexity |
| `RequireCategories` | false | Require categories on vars/functions |
| `EnforceNamingConvention` | true | BP_ prefix, BPI_ for interfaces, etc. |
| `AllowAIToDelete` | warn | block/warn/allow |
| `AllowAIToReparent` | warn | block/warn/allow |
| `AutoCompileAfterWrite` | true | Auto-compile after write operations |
| `MaxWriteOpsPerMinute` | 100 | Rate limit |

### 10.6 Safety Mechanisms

**Transaction wrapping:** Every write in `FScopedTransaction`. Rollback on failure. Transaction descriptions include tool name for readable undo history.

**Asset backup:** In-memory snapshot before first write per session. Exposed via `project.rollback`.

**Compile gating:** Failed compilations return errors but don't rollback changes. Warning flag set on asset.

**Infinite loop detection:** Same failing tool call 3 times → Brain Layer intervenes.

**Concurrent access protection:** Check if asset is open in editor via `UAssetEditorSubsystem`. Warn or queue if so.

**PIE protection:** Reject Blueprint writes during Play in Editor.

### 10.7 Error Response Format

```json
{
  "success": false,
  "error": {
    "code": "TYPE_CONSTRAINT_VIOLATION",
    "message": "Blueprint Interfaces cannot contain variables.",
    "details": {
      "blueprint": "/Game/Interfaces/BPI_Damageable",
      "blueprint_type": "Interface",
      "attempted_operation": "add_variable",
      "constraint": "interfaces_no_variables"
    },
    "suggestion": "Add the variable to the implementing Blueprint instead.",
    "severity": "error"
  }
}
```

Severities: `error` (not executed), `warning` (executed but review), `info` (success + context).

---

## 11. Phase 0 — Foundation

**Goal:** Plugin loads, MCP server runs, chat UI opens, agent can connect (internal or external), and project index is queryable. No asset-specific functionality yet.

**Estimated effort:** 4–5 weeks (increased from 3-4 due to chat UI)

### 11.1 Plugin Module Setup

- `.uplugin` with two modules: `AIAgentRuntime` (Runtime), `AIAgentEditor` (Editor)
- Runtime module: IR struct definitions only
- Editor module: `PostEngineInit`, depends on `UnrealEd`, `BlueprintGraph`, `Kismet`, `AssetRegistry`, `HttpServer`, `HTTP`, `Slate`, `EditorStyle`
- Editor preferences registration

### 11.2 Chat UI — Slate Panel

- Dockable tab registered with `FGlobalTabmanager`
- Slate widget hierarchy: panel → splitter (messages | context bar) → input area
- Message rendering with markdown support (bold, code blocks, lists)
- Streaming text display (append characters as they arrive)
- Operation feed widget (collapsible list with status icons)
- Context bar widget (horizontal asset tag list with remove buttons)
- Input widget with @mention autocomplete popup
- Quick action button bar
- Focus profile dropdown
- Settings gear icon

**Key classes:**
- `SAIAgentChatPanel` — main panel widget
- `SAIAgentMessageList` — scrolling message history
- `SAIAgentOperationFeed` — live tool call display
- `SAIAgentContextBar` — attached asset display
- `SAIAgentInputField` — input with @mention support
- `SAIAgentMentionPopup` — asset search autocomplete

### 11.3 Conversation Manager

- Manages chat session state: message history, system prompt, tool definitions
- Handles API calls to OpenRouter (or other providers via abstraction)
- Streaming response parsing (SSE for OpenRouter)
- Tool call detection and dispatch to Tool Router
- Multi-turn conversation management (accumulate messages)
- Token counting / context window management

### 11.4 Provider Abstraction

- `IProviderClient` interface
- `FOpenRouterClient` implementation (Phase 0 — covers all models via one key)
- `FAnthropicClient` implementation (Phase 0 — direct Claude access, recommended)
- `FOpenAIClient` implementation (Phase 1)
- `FGoogleClient` implementation (Phase 1)
- `FOllamaClient` implementation (Phase 2)
- Secure API key storage (platform keychain, not ini files)
- Model selection and configuration per provider
- Error handling (rate limits, auth failures, model unavailable)
- Usage tracking (approximate cost display)

### 11.5 MCP Server Core

- HTTP server on configurable localhost port (default 3000)
- JSON-RPC 2.0 message handling
- MCP protocol lifecycle: `initialize` → `initialized` → tool/resource calls
- Request routing to Tool Registry
- Game thread dispatch for all UE API calls
- Streaming support (SSE for long operations)

### 11.6 Tool Registry

```cpp
class FToolRegistry
{
public:
    void RegisterTool(const FString& Name, const FString& Description,
        const TSharedPtr<FJsonObject>& InputSchema,
        TFunction<FToolResult(const TSharedPtr<FJsonObject>&)> Handler);
    
    // For chat: returns tools filtered by active Focus profile
    TArray<FToolDefinition> GetToolsForProfile(const FString& ProfileName) const;
    
    // For MCP: returns all tools
    TArray<FToolDefinition> GetAllTools() const;
    
    FToolResult ExecuteTool(const FString& Name, const TSharedPtr<FJsonObject>& Params);
};
```

### 11.7 Shared Services

**Transaction Manager:** Wraps `FScopedTransaction`. Named transactions for undo history.

**Asset Resolver:** Load by path/name/class. Handle redirectors, missing assets. Check if being edited.

**IR Serializer Base:** Common type serialization (FName, FProperty, pin types, transforms, etc.)

**Validation Engine:** Rule registration, execution, structured error responses.

### 11.8 Project Index

- Built on startup from `FAssetRegistryModule`
- Incremental updates via asset registry delegates
- Stores: asset map, class hierarchy, dependency graph, interface map, engine config
- Exposed as MCP Resources and available to chat context system
- Search with fuzzy matching

### 11.9 Configuration System

Editor preferences under Project Settings → Plugins → AI Agent:
- OpenRouter API key
- Default model
- MCP server port and auto-start
- Enabled modules
- Confirmation tier overrides
- Policy overrides
- UI preferences (operation feed verbosity, notifications, sounds)

### 11.10 Focus Profile System

- Profile definitions in config (JSON files)
- Tool whitelist per profile
- System prompt additions per profile
- Default profiles shipped with plugin
- Custom profile creation UI

### 11.11 System Prompt Authoring

The system prompts are a critical deliverable — prompt quality directly determines how well the AI uses the tools. These need to be written, tested, and iterated on.

**Base System Prompt (required for Phase 0):**
- Agent identity and role (UE development assistant)
- Tool-calling conventions (when to read before writing, how to chain operations, when to compile)
- Response format guidelines (when to show plans, how to report results, when to ask clarifying questions)
- Safety/behavioral rules (always compile after changes, warn before deleting, never modify assets outside the current request scope, respect confirmation tiers)
- Error handling instructions (how to interpret validation errors, when to self-correct vs ask the user)
- Context usage guidelines (how to use project index, when to request more detail on an asset)
- UE-specific knowledge baseline (common patterns, naming conventions, architecture best practices)

**Focus Profile Prompts (one per profile, required for each phase that adds a profile):**
- Domain-specific patterns and best practices
- Common task workflows for that domain
- Domain-specific pitfalls and things to avoid
- Examples of good tool-call sequences for common requests

**Project Policy Injection (required for Phase 0):**
- How project-level policies (naming conventions, max complexity, team rules) get injected into the system prompt at runtime
- Format for user-defined rules that get appended to the prompt
- How engine version and enabled plugins affect prompt content

**Per-Request Prompt Assembly:**
- The Conversation Manager assembles the full prompt per API call by combining: base prompt + active focus profile prompt + project context + active asset context + conversation history
- Token budget management — what gets truncated first when context is too large
- Rules for what context is included verbatim vs summarized vs omitted

**Iteration process:**
- System prompts will require significant testing and iteration with real AI models
- Maintain a test suite of common requests and expected tool-call sequences
- Track prompt performance metrics (task completion rate, self-correction frequency, error rate)
- Version prompts alongside plugin versions

### Phase 0 Completion Criteria

- [ ] Plugin loads in UE 5.5+ without errors
- [ ] Chat panel opens, docks, renders messages
- [ ] User can enter OpenRouter API key and select a model
- [ ] User can send a message and receive a streaming response
- [ ] Operation feed renders (with stub/test operations)
- [ ] Context bar shows currently open asset
- [ ] @mention autocomplete searches project assets
- [ ] Focus profile dropdown works (filters tool list)
- [ ] Base system prompt written and tested with at least one model
- [ ] Per-request prompt assembly works (base + profile + project context + active context)
- [ ] Project policy injection works (user-defined rules appear in assembled prompt)
- [ ] Token budget management truncates gracefully when context is large
- [ ] MCP server starts and accepts external agent connections
- [ ] External agent can complete MCP handshake
- [ ] `tools/list` returns tool list (stubs)
- [ ] Project index builds and is queryable
- [ ] `project://search` finds assets
- [ ] Configuration UI visible in Project Settings
- [ ] All operations dispatch to game thread correctly

---

## 12. Phase 1 — Blueprints

**Goal:** Complete read/write support for all UEdGraph-based Blueprint types through both the chat UI and MCP. The AI can understand, create, and modify any Blueprint. The agentic loop works end-to-end.

**Estimated effort:** 6–8 weeks

**Depends on:** Phase 0 complete

### 12.1 Blueprint Type Map

#### Tier 1: Standard K2 (same pipeline, different constraints)

| Type | Identification | Key Constraints |
|------|---------------|-----------------|
| Normal Blueprint | `BPTYPE_Normal` | Full capabilities |
| Blueprint Interface | `BPTYPE_Interface` | No variables, components, event graph, function bodies |
| Function Library | `BPTYPE_FunctionLibrary` | Static functions only, no event graph/components/variables |
| Macro Library | `BPTYPE_MacroLibrary` | Macro graphs only |
| Anim Notify | `BPTYPE_Normal`, parent `UAnimNotify` | Typically just `Received_Notify` override |
| Anim Notify State | `BPTYPE_Normal`, parent `UAnimNotifyState` | `Received_NotifyBegin/Tick/End` |
| Actor Component | `BPTYPE_Normal`, parent `UActorComponent` | No SCS unless SceneComponent subclass |
| Editor Utility Widget | `UEditorUtilityWidgetBlueprint` | Widget BP, editor-only |
| Editor Utility BP | `BPTYPE_Normal`, parent `UEditorUtilityActor/Object` | Regular BP, editor-only |
| Gameplay Ability | `BPTYPE_Normal`, parent `UGameplayAbility` | Regular BP, GAS context matters |

#### Tier 2: Extended Graph Systems

| Type | Key Differences | Phase 1 Support |
|------|-----------------|-----------------|
| Animation Blueprint | AnimGraph, state machines, blend nodes | Read: full. Write: event graph full, state machines partial |
| Widget Blueprint | Widget tree separate from graphs | Read: full. Write: widget tree basic, event graph full |
| Control Rig Blueprint | Uses `URigVMGraph` | Read-only |

### 12.2 IR Schema

Blueprint-level IR and graph-level IR as documented in previous plan. Key design rules:

- Node IDs are simple (`node_1`, `node_2`) — not engine GUIDs
- Connections reference by `node_id.pin_name`
- Pin types use simplified category system
- Positions omitted (auto-layout after write)
- Inherited members marked with `"defined_in"`
- Only data the AI needs is included

(Full IR schema examples in Appendix A)

### 12.3 Reader Implementation

Core graph walker works for ALL UEdGraph types. Type-specific serializers for AnimGraph, WidgetTree.

**MCP Tools (Readers):**

| Tool | Description |
|------|-------------|
| `blueprint.read` | Full Blueprint IR |
| `blueprint.read_function` | Single function graph detail |
| `blueprint.read_event_graph` | Event graph detail |
| `blueprint.read_variables` | Variable list only |
| `blueprint.read_components` | Component tree only |
| `blueprint.read_hierarchy` | Inheritance chain |
| `blueprint.list_overridable_functions` | Parent's overridable functions |

### 12.4 Writer Implementation

Every write goes through: Validate → Confirm (tier routing) → Transact → Execute → Verify → Report.

**Asset-Level:** `blueprint.create`, `blueprint.set_parent_class`, `blueprint.add_interface`, `blueprint.remove_interface`, `blueprint.compile`, `blueprint.delete`

**Variables:** `blueprint.add_variable`, `blueprint.remove_variable`, `blueprint.modify_variable`

**Components:** `blueprint.add_component`, `blueprint.remove_component`, `blueprint.modify_component`, `blueprint.reparent_component`

**Functions:** `blueprint.add_function`, `blueprint.remove_function`, `blueprint.modify_function_signature`, `blueprint.add_event_dispatcher`, `blueprint.override_function`, `blueprint.add_custom_event`

**Graph Editing:** `blueprint.add_node`, `blueprint.remove_node`, `blueprint.connect_pins`, `blueprint.disconnect_pins`, `blueprint.set_pin_default`, `blueprint.set_node_property`

**Animation BP:** `animbp.add_state_machine`, `animbp.add_state`, `animbp.add_transition`, `animbp.set_transition_rule`

**Widget BP:** `widget.add_widget`, `widget.remove_widget`, `widget.set_property`, `widget.bind_property`

### 12.5 Node Type Catalog

Dynamically generated from the engine. Filterable by category, class, search term. Each entry includes: node class, human-readable name, pins with types, category. Exposed as MCP resource and used by Brain Layer for validation.

### 12.6 Agentic Loop Integration

Phase 1 is where the agentic loop gets real testing:

- AI creates Blueprint → adds variables → adds function → adds nodes → connects pins → compiles
- If compile fails → AI reads errors → determines fix → applies fix → recompiles
- Operation feed shows every step live in the chat
- Confirmation tiers route appropriately (variable adds = Tier 1, function logic = Tier 2)

### 12.7 Implementation Order

1. Blueprint Reader — Core (all Tier 1 types)
2. Compile tool
3. Blueprint Writer — Asset-level (create, delete, reparent)
4. Blueprint Writer — Variables
5. Blueprint Writer — Components
6. Blueprint Writer — Functions (signatures)
7. Blueprint Writer — Graph editing (add_node, connect_pins) — THE BIG ONE
8. Node Type Catalog
9. Agentic loop integration (compile → error parse → self-correct)
10. Confirmation tier integration with chat UI
11. Animation Blueprint Reader + partial Writer
12. Widget Blueprint Reader + basic Writer

### Phase 1 Completion Criteria

- [ ] Reader accurate for all Tier 1 Blueprint types
- [ ] Reader accurate for Animation and Widget Blueprints
- [ ] All write tools work for Tier 1 Blueprints
- [ ] Graph editing handles 20 most common node types
- [ ] Pin type validation catches incompatible connections
- [ ] All writes are transactional (Ctrl+Z works)
- [ ] Compile feedback returns structured errors
- [ ] Agentic loop self-corrects on compile errors (up to 3 attempts)
- [ ] Confirmation tiers route operations correctly in chat UI
- [ ] Operation feed shows live progress for all multi-step operations
- [ ] Type constraints enforced for Interface, FunctionLibrary, MacroLibrary
- [ ] Focus profile "Blueprint" correctly restricts tool set
- [ ] Blueprint focus profile prompt written and tested (domain patterns, common workflows, pitfalls)
- [ ] End-to-end: user types "create a health pickup" → Blueprint exists with working logic
- [ ] Same operations work via MCP from Claude Code

---

## 13. Phase 2 — Behavior Trees & Blackboards

**Goal:** Full read/write support for BT and Blackboard assets.

**Estimated effort:** 2–3 weeks

**Depends on:** Phase 0 complete

### Blackboard Tools

`blackboard.create`, `blackboard.read`, `blackboard.add_key`, `blackboard.remove_key`, `blackboard.modify_key`, `blackboard.set_parent`

### Behavior Tree Tools

`behaviortree.create`, `behaviortree.read`, `behaviortree.set_blackboard`, `behaviortree.add_composite`, `behaviortree.add_task`, `behaviortree.add_decorator`, `behaviortree.add_service`, `behaviortree.remove_node`, `behaviortree.move_node`, `behaviortree.set_node_property`

### Cross-References

Project Index tracks BT → Blackboard associations. Reading a BT auto-includes its Blackboard in context. Custom BT node types discoverable via node catalog.

### Focus Profile Update

"AI & Behavior" profile includes: all Blueprint tools + BT tools + Blackboard tools.

### Phase 2 Completion Criteria

- [ ] Blackboard full CRUD
- [ ] BT reader produces accurate tree IR
- [ ] All BT write operations work
- [ ] Custom BT nodes appear in catalog
- [ ] BT → Blackboard cross-reference works
- [ ] "AI & Behavior" focus profile works correctly
- [ ] "AI & Behavior" focus profile prompt written and tested
- [ ] End-to-end: "create a patrol behavior for my enemy" → BT + Blackboard created

---

## 14. Phase 3 — PCG Graphs

**Goal:** Read/write support for Procedural Content Generation graphs.

**Estimated effort:** 2–3 weeks

**Depends on:** Phase 0 complete

### PCG Tools

`pcg.create`, `pcg.read`, `pcg.add_node`, `pcg.remove_node`, `pcg.connect`, `pcg.disconnect`, `pcg.set_settings`, `pcg.add_subgraph`, `pcg.execute`

### PCG-Specific Concerns

- Node settings are deeply nested structs — serialize asset references as paths
- API changes between engine versions — version-gate and degrade gracefully
- Execution timeout (configurable, default 30 seconds)

### Focus Profile Update

"Level & PCG" profile includes: Blueprint tools + PCG tools.

### Phase 3 Completion Criteria

- [ ] PCG reader produces accurate data-flow IR
- [ ] Common PCG node types can be created and configured
- [ ] Connections respect PCG data type compatibility
- [ ] Graph execution returns meaningful summary
- [ ] "Level & PCG" focus profile works correctly
- [ ] "Level & PCG" focus profile prompt written and tested

---

## 15. Phase 4 — C++ Integration

**Goal:** AI understands C++ classes, bridges between C++ and Blueprints, optional source editing.

**Estimated effort:** 3–4 weeks

**Depends on:** Phase 0 complete, benefits from Phase 1

### Reflection Reader Tools

`cpp.read_class`, `cpp.list_blueprint_callable`, `cpp.list_overridable`, `cpp.read_enum`, `cpp.read_struct`

Uses UE runtime reflection — works for engine, plugin, and project classes.

### Source Reader Tools

`cpp.read_header`, `cpp.read_source`, `cpp.list_project_classes`

### Source Writer Tools (Optional)

`cpp.create_class`, `cpp.add_property`, `cpp.add_function`, `cpp.modify_source`, `cpp.compile`

### Bridge Context

C++ reflection data automatically enriches Blueprint operations: parent class API, available overrides, enum values, delegate signatures.

### Focus Profile Update

"C++ & Blueprint" profile includes: all Blueprint tools + C++ tools.

### Phase 4 Completion Criteria

- [ ] Reflection reader works for any loaded UClass
- [ ] Blueprint-visible API surface accurately reported
- [ ] Project source files readable
- [ ] C++ class creation generates correct boilerplate (optional)
- [ ] C++ context enriches Blueprint operations
- [ ] "C++ & Blueprint" focus profile works correctly
- [ ] "C++ & Blueprint" focus profile prompt written and tested

---

## 16. Phase 5 — Cross-System Intelligence

**Goal:** Multi-asset operations, dependency-aware editing, project-wide intelligence.

**Estimated effort:** 3–4 weeks

**Depends on:** Phases 1–4

### Multi-Asset Tools

`project.implement_interface`, `project.create_ai_character`, `project.refactor_rename`, `project.move_to_cpp`, `project.bulk_read`

### Snapshot & Rollback

`project.snapshot`, `project.list_snapshots`, `project.rollback`, `project.diff`

### Dependency-Aware Operations

Cross-system checks: BB key rename → BT impact, BP Interface change → implementing BPs, C++ base class change → BP overrides.

### Engine Awareness

Filter tools by enabled plugins and engine version. Suggest plugin requirements. Warn about deprecated APIs.

### Prompt Templates (MCP Prompts)

`explain_blueprint`, `review_blueprint`, `plan_feature`, `migrate_to_cpp`, `debug_compile_error`

### Internal Routing Evolution

Brain Layer begins using domain-specific sub-prompts internally for complex multi-asset requests. User still sees one conversation. This is the transition toward Approach 4 (internal routing) from the agent architecture.

### Phase 5 Completion Criteria

- [ ] Cross-asset dependency warnings work
- [ ] Multi-asset operations execute atomically
- [ ] Snapshot/rollback works across multiple assets
- [ ] Engine version awareness filters tools
- [ ] Prompt templates work
- [ ] "Create an AI enemy with patrol behavior" works end-to-end (BP + BT + BB)

---

## 17. Edge Cases & Failure Modes

### 17.1 Asset State Edge Cases

| Edge Case | Handling |
|-----------|---------|
| Blueprint open in editor during AI modification | Modify through editor transaction system so UI updates. Check via `IsAssetBeingEdited()`. |
| Blueprint dirty (unsaved) when AI reads | Read from in-memory (correct). Include `"unsaved_changes": true`. |
| Blueprint fails to compile after modification | Return structured errors. Don't auto-rollback. Set `"compile_status": "error"`. Feed to agentic loop for self-correction. |
| Asset path doesn't exist | Structured error with suggestion to use search. |
| Asset is a redirector | Follow transparently. Include `"redirected_from"`. |
| Circular Blueprint references | Track visited nodes. Max depth 20. |
| Hot Reload during operation | Re-resolve assets at execution time. Invalidate context cache. |
| Engine GC during operation | Root assets during operations via `FGCObject` or transaction references. |
| PIE (Play in Editor) active | Reject Blueprint writes. Clear error message. |

### 17.2 Graph Editing Edge Cases

| Edge Case | Handling |
|-----------|---------|
| AI tries nonexistent node type | Validate against node catalog. Return fuzzy match suggestions. |
| Incompatible pin connection | Check compatibility. Auto-insert conversion if possible. Otherwise reject with type info. |
| Orphaned execution flow | Post-op structural check. Warn but don't block. |
| Duplicate event node (second BeginPlay) | Validation rejects. |
| Node removal with connections | Auto-disconnect all pins first. Report broken connections. |
| Very large Blueprint (500+ nodes) | Paginate: summary first, detail on demand. |

### 17.3 Chat UI Edge Cases

| Edge Case | Handling |
|-----------|---------|
| User sends message while AI is still executing | Queue the message. Show "waiting for current operation" indicator. |
| API key invalid or expired | Clear error in chat. Link to settings to update key. |
| Model rate limited | Show retry countdown in chat. Auto-retry after delay. |
| Very long AI response | Stream rendering. Truncation warning if response is cut off. |
| User switches Focus profile mid-operation | Don't switch until current operation completes. Show warning. |
| User closes chat panel during operation | Operations continue. Toast notification on completion. Reopen panel to see results. |
| Context window exceeded | Brain Layer truncates context. Inform AI of truncation. |
| Network error during API call | Retry logic with exponential backoff. Show error after 3 retries. |

### 17.4 MCP Protocol Edge Cases

| Edge Case | Handling |
|-----------|---------|
| Agent disconnects mid-operation | Transaction system ensures atomic operations. |
| Multiple agents connected simultaneously | Concurrent reads OK. Writes serialized via mutex. |
| Malformed JSON | JSON-RPC parse error (-32700). |
| Unknown tool call | Method not found (-32601) with available tools list. |
| Port already in use | Try next port. Report actual port in editor UI. |

### 17.5 Confirmation System Edge Cases

| Edge Case | Handling |
|-----------|---------|
| User approves Tier 2 plan but asset changed externally between plan and execution | Re-validate before executing. If asset changed, re-plan and show updated plan. |
| User edits a Tier 2 plan with invalid modifications | Parse edited plan. Validate. Show errors inline if invalid. |
| AI proposes a plan that spans multiple tiers | Split into sub-plans. Auto-execute Tier 1 parts, confirm Tier 2 parts, preview Tier 3 parts. |
| User clicks Cancel on a partially-executed multi-step operation | Rollback to pre-operation snapshot. All-or-nothing for confirmed operations. |
| Tier 3 non-destructive comparison: user wants the new version | Replace original, delete comparison. Transaction wraps the swap. |

### 17.6 Agentic Loop Edge Cases

| Edge Case | Handling |
|-----------|---------|
| AI self-correction produces different error | Count as new error, reset correction counter for that specific error. |
| AI enters infinite correction loop (fixing A causes B, fixing B causes A) | Pattern detection: if same set of errors repeats, stop after 2 cycles and report to user. |
| Compilation succeeds but behavior is wrong | Plugin can't detect behavioral correctness — only structural/compilation validity. User review required. |
| AI requests tool that Focus profile doesn't include | Brain Layer explains the tool is not available in current profile. Suggest switching to appropriate profile. |
| Self-correction requires reading additional assets | Allow read operations during correction loop. Only count write retries toward the limit. |

---

## 18. Testing Strategy

### 18.1 Unit Tests

- IR Serializer: round-trip tests
- Validation Engine: each rule with valid/invalid inputs
- Asset Resolver: path resolution, redirectors, missing assets
- Transaction Manager: commit, rollback, nested
- Confirmation Manager: correct tier routing for all operation types
- Context Assembly: correct layering and budget management

### 18.2 Integration Tests

- Create Blueprint → read back → verify IR matches
- Create → add vars → add function → add nodes → connect → compile → verify success
- Create Interface → attempt add variable → verify rejection
- Full agentic loop: create BP → compile error → self-correct → compile success
- Confirmation flow: Tier 2 plan → approve → execute → verify
- Chat UI: send message → receive streaming response → tool calls execute → operation feed updates

### 18.3 Agent-in-the-Loop Tests

Real agent connected via built-in chat:
- "Create a health pickup actor" — end-to-end
- "Read BP_ThirdPersonCharacter and explain it" — accuracy test
- "Add double jump to the character" — modification test
- "Create an AI enemy with patrol behavior" — multi-asset test
- "Fix this compilation error" — self-correction test

Same tests via MCP with Claude Code.

### 18.4 Stress Tests

- 100 variables via rapid tool calls
- Blueprint with 500+ nodes
- 10 concurrent read requests (MCP)
- Rapid create/delete cycles (memory leaks, dangling references)
- Chat with 100+ message history (context window management)

### 18.5 UX Tests

- Chat panel docking in various positions
- @mention search performance with 10,000+ assets
- Operation feed rendering with 100+ operations
- Streaming text rendering smoothness
- Confirmation dialog responsiveness

---

## 19. Timeline & Milestones

| Phase | Duration | Key Milestone |
|-------|----------|---------------|
| **Phase 0** | Weeks 1–5 | Chat UI working, MCP server running, agent can connect, project index queryable |
| **Phase 1a** | Weeks 6–9 | Blueprint reader complete. Basic writers (create, variables, components). Chat sends tool calls. |
| **Phase 1b** | Weeks 10–13 | Graph editing. Node catalog. Agentic loop. Confirmation tiers. AnimBP/WidgetBP. |
| **Phase 2** | Weeks 14–16 | BT + Blackboard full support. "AI & Behavior" focus profile. |
| **Phase 3** | Weeks 17–19 | PCG graph support. "Level & PCG" focus profile. |
| **Phase 4** | Weeks 20–23 | C++ reflection + optional source editing. "C++ & Blueprint" focus profile. |
| **Phase 5** | Weeks 24–27 | Cross-system intelligence, multi-asset ops, polish. |

**Total: ~6.5 months for one developer**

### Risk Factors

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Slate chat UI more complex than expected | Medium | Delays Phase 0 by 1–2 weeks | Prototype UI early. Keep it simple — no custom rendering. |
| OpenRouter tool-calling format issues | Medium | Delays Phase 0 | Test with multiple models early. Have fallback to direct Claude API. |
| Graph editing has more node type edge cases | High | Delays Phase 1b by 2–4 weeks | Start with 20 most common nodes. Add incrementally. |
| Agentic loop self-correction unreliable | Medium | Reduces quality | Extensive testing. Limit correction attempts. Good error messages. |
| Confirmation UX slows users down | Medium | User frustration | Make tiers configurable. Default power users to more auto-execute. |
| Large projects slow down index/context | Medium | Performance issues | Lazy loading, pagination, caching. Profile with large test project. |

---

## Appendix A: IR Schema Examples

### Blueprint-Level IR

```json
{
  "name": "BP_PlayerCharacter",
  "path": "/Game/Characters/BP_PlayerCharacter",
  "type": "Normal",
  "parent_class": {"name": "ACharacterBase", "source": "cpp"},
  "capabilities": {
    "has_event_graph": true, "has_functions": true,
    "has_variables": true, "has_components": true,
    "has_anim_graph": false, "has_widget_tree": false
  },
  "interfaces": [{"name": "BPI_Damageable", "path": "/Game/Interfaces/BPI_Damageable"}],
  "compile_status": "success",
  "variables": [
    {
      "name": "Health", "type": {"category": "float"},
      "default_value": "100.0", "category": "Stats",
      "flags": {"blueprint_read_write": true, "replicated": true},
      "defined_in": "self"
    }
  ],
  "components": {
    "root": "DefaultSceneRoot",
    "tree": [
      {"name": "DefaultSceneRoot", "class": "USceneComponent", "children": [
        {"name": "MeshComp", "class": "USkeletalMeshComponent", "children": []},
        {"name": "CameraArm", "class": "USpringArmComponent", "children": [
          {"name": "Camera", "class": "UCameraComponent", "children": []}
        ]}
      ]}
    ]
  },
  "graphs": {
    "event_graphs": ["EventGraph"],
    "functions": ["TakeDamage", "GetHealth", "Heal"],
    "macros": []
  }
}
```

### Graph-Level IR

```json
{
  "blueprint": "BP_PlayerCharacter",
  "graph_name": "TakeDamage",
  "graph_type": "function",
  "signature": {
    "inputs": [{"name": "DamageAmount", "type": {"category": "float"}}],
    "outputs": [{"name": "Survived", "type": {"category": "bool"}}],
    "access": "public", "pure": false
  },
  "nodes": [
    {"id": "node_1", "type": "FunctionEntry", "title": "TakeDamage",
     "pins_out": [{"name": "exec", "type": "exec"}, {"name": "DamageAmount", "type": "float"}]},
    {"id": "node_2", "type": "Greater_FloatFloat",
     "pins_in": [{"name": "A", "type": "float", "connection": "node_1.DamageAmount"},
                 {"name": "B", "type": "float", "default": "0.0"}],
     "pins_out": [{"name": "ReturnValue", "type": "bool"}]},
    {"id": "node_3", "type": "Branch",
     "pins_in": [{"name": "exec", "type": "exec", "connection": "node_1.exec"},
                 {"name": "Condition", "type": "bool", "connection": "node_2.ReturnValue"}],
     "pins_out": [{"name": "True", "type": "exec"}, {"name": "False", "type": "exec"}]}
  ]
}
```

## Appendix B: MCP Tool Naming Convention

All tools: `{module}.{operation}`

- `blueprint.*` — Blueprint operations
- `animbp.*` — Animation Blueprint specializations
- `widget.*` — Widget Blueprint specializations
- `behaviortree.*` — Behavior Tree operations
- `blackboard.*` — Blackboard operations
- `pcg.*` — PCG graph operations
- `cpp.*` — C++ integration operations
- `project.*` — Cross-system and project-level operations

## Appendix C: IR Type System

| Category | Examples | Notes |
|----------|---------|-------|
| `bool` | | |
| `byte` | | Underlying for enums |
| `int` | `int32`, `int64` | |
| `float` | `float`, `double` | |
| `string` | `FString`, `FName`, `FText` | |
| `vector` | `FVector`, `FVector2D` | |
| `rotator` | `FRotator` | |
| `transform` | `FTransform` | |
| `color` | `FLinearColor`, `FColor` | |
| `object` | Any `UObject*` | `class` subfield for specific type |
| `class` | `TSubclassOf<T>` | `base_class` subfield |
| `struct` | Any `USTRUCT` | `struct_name` subfield |
| `enum` | Any `UENUM` | `enum_name` subfield |
| `delegate` | Single/multicast | Includes signature |
| `array` | `TArray<T>` | `element_type` subfield |
| `set` | `TSet<T>` | `element_type` subfield |
| `map` | `TMap<K,V>` | `key_type` and `value_type` |
| `exec` | Execution pins | Flow control, not data |
| `wildcard` | Template pins | Resolves on connection |

## Appendix D: Glossary

| Term | Definition |
|------|-----------|
| **IR** | Intermediate Representation — JSON format between plugin and AI |
| **MCP** | Model Context Protocol — standard between AI agents and tool servers |
| **SCS** | Simple Construction Script — UE's Blueprint component hierarchy |
| **K2** | Kismet 2 — UE's internal name for the Blueprint graph system |
| **CDO** | Class Default Object — UE's default instance of each class |
| **PIE** | Play in Editor — UE's in-editor play testing mode |
| **Brain Layer** | AI Planning Engine — context assembly, agentic loop, operation planning |
| **Control Layer** | Policy & Rule System — validation, constraints, safety |
| **T3D** | Unreal's text-based serialization format for graph nodes |
| **Focus Profile** | Tool-set filter that narrows the AI's available operations |
| **Operation Feed** | Live display of tool calls as they execute in the chat panel |
| **Confirmation Tier** | Risk classification that determines approval flow (1=auto, 2=confirm, 3=preview) |
| **BYOK** | Bring Your Own Key — user provides their own API key |
| **OpenRouter** | API aggregator providing access to 400+ AI models through one key |
| **Agentic Loop** | Plan → Execute → Check → Self-Correct → Repeat cycle |
