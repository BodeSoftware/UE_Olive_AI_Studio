# Unreal Engine AI Agent Plugin — Short Plan (Architect Context)

## 1) What we’re building (Vision)
An **editor-only Unreal Engine plugin** that acts like **“Claude Code inside Unreal”**:

- A **dockable in-editor chat panel** for most users.
- A **local MCP server** (HTTP JSON-RPC on localhost) for power users to connect **external agents**.
- **Both interfaces share the same Tool Layer**, validation, transaction safety, and execution engine.

Core goal: the agent can **read + explain**, then **create + modify** UE assets (starting with Blueprints) safely using an **agentic loop** (plan → execute → compile/check → self-fix).

---

## 2) Non-negotiable principles
1. **Dual interface, one tool system**: Chat UI + MCP, same underlying tools.  
2. **Read-heavy, write-careful**: reading is cheap; writing is validated + transactional + verified.  
3. **Surgical edits**: add/connect nodes and pins instead of “regenerating whole graphs.”  
4. **Show the work**: live **Operation Feed** shows every tool call (and progress).  
5. **Fail safely**: structured errors, no asset corruption, undo support, graceful partial failures.  
6. **Single agent** with **Focus Profiles** (tool filtering), not multiple agents that lose context.

---

## 3) High-level architecture (what talks to what)
Inside the Unreal Editor process:

**Chat UI (Slate)** → **Conversation Manager** (streaming, history, provider calls)  
and/or  
**MCP Server (localhost HTTP JSON-RPC)** → (external agent connects)

Both route into:

- **Brain Layer (Planning Engine)**: builds plans, assembles context, sequences tool calls, runs the self-correct loop.
- **Confirmation Manager**: applies 3-tier safety gating for writes (in built-in chat UX).
- **Policy/Rule System (Control Layer)**: validates tool calls, enforces constraints, handles safety + rate limiting.
- **Tool Router**: dispatches to domain modules (Blueprint/BT/PCG/C++ later).
- **Shared Services**: transactions, asset loading/resolution, compile manager, project index, validation, error reporting.

---

## 4) Plugin/module structure
Plugin contains **two UE modules**:

- `AIAgentRuntime` (minimal): **IR structs only** (BlueprintIR, BehaviorTreeIR, PCGIR, CommonIR).
- `AIAgentEditor` (main): UI, providers, MCP server, brain/policy, tool registry, and per-domain modules.

No runtime/shipping behavior: **editor-only**.

---

## 5) Core UX (built-in chat)
A dockable Slate panel (Tools → AI Agent Chat) with:

- **Context Bar**: shows attached assets (auto + @mentions + drag-drop), removable tags, click to open.
- **Message history**: streaming markdown responses.
- **Operation Feed** (collapsible): shows each tool call live with status (in-progress/success/error), can stop mid-run, supports undo per operation when feasible.
- **Quick actions** depending on what’s open (Explain/Review/Fix Errors/etc.).
- **Focus Profile dropdown** (Auto/Blueprint/AI & Behavior/Level & PCG/C++ & Blueprint/etc.).

---

## 6) Safety model: 3-tier confirmation (built-in chat)
All write operations are assigned a tier:

- **Tier 1 (Auto)**: low risk / trivially undoable (add variable/component, set property, create empty asset).
- **Tier 2 (Plan → Confirm → Execute)**: multi-step edits (create function logic, wire event graph, modify existing logic).
- **Tier 3 (Non-destructive compare / impact analysis)**: high risk (refactors, reparenting, deletion, signature changes, bulk ops). Creates side-by-side alternatives or shows dependency impact first.

Users can override tiers per operation category in settings.

---

## 7) Context system (how the AI “sees” the project)
Layered context assembly managed by the Brain Layer:

1. **Auto-context**: currently open asset/editor + selected nodes + compile errors.
2. **@mentions**: persistent attached assets.
3. **Right-click “Ask AI about this”**: targeted, one-shot context.
4. **Drag/drop assets** into chat: persistent like @mentions.
5. **Project Index**: searchable asset map + deps + class hierarchy; queried on-demand.
6. Optional: viewport screenshot for visual tasks.

Large assets are **paginated**: blueprint read gives summary; detailed graph data is fetched per-function when needed.

---

## 8) Provider & connectivity
**Built-in chat** supports BYOK provider selection via an abstraction layer:

- Phase 0 priority: **OpenRouter** + **Anthropic direct** (streaming/tool calling).
- Later: OpenAI direct, Google direct, then local Ollama.

**MCP server**:
- Runs on localhost (default port ~3000), HTTP + JSON-RPC 2.0.
- External agents get **tools/resources** and progress notifications.
- External agents manage their own confirmations/context; built-in chat handles confirmation UX.

---

## 9) The “Agent” behavior: Brain Layer loop
The plugin’s agent behavior is a loop:

1. **Plan** steps
2. **Confirm** if Tier 2/3
3. **Execute** tool calls (live feed)
4. **Check** (compile/verify structure)
5. **Self-correct** using compile errors (max ~3 attempts; stop if repeating)

This is the core automation mechanism (not multiple agents).

---

# Phased delivery roadmap (what gets built when)

## Phase 0 — Foundation (plugin boots + infra)
Deliverables:
- Plugin loads, **Chat UI works**, streaming text works.
- Provider abstraction implemented (OpenRouter + Anthropic first).
- **MCP server online** (handshake, list tools/resources).
- **Tool Registry + Tool Router** skeleton.
- **Shared Services**: transaction manager, asset resolver, validation engine base.
- **Project Index**: asset map, deps, hierarchy, search; incremental updates via Asset Registry.
- Focus profiles exist (tool filtering) + system prompts loaded/assembled.

Done when: you can chat, stream, see stub operation feed, search assets via index, and connect an external MCP client.

---

## Phase 1 — Blueprints (the big milestone)
Goal: full read/write for Blueprint assets via both Chat + MCP.

Key deliverables:
- **Blueprint IR** (summary + function-level graph detail).
- Readers: blueprint.read, read_function, variables/components/hierarchy, etc.
- Writers: create, variables, components, functions (signatures), **graph editing** (add node, connect pins, set defaults).
- **Node Type Catalog** generated from engine, used for validation/tool guidance.
- Compile + structured error parsing + agentic self-fix loop.

Done when: user types “create a health pickup” and the agent creates a working Blueprint with nodes wired and compiling.

---

## Phase 2 — Behavior Trees + Blackboards
Deliverables:
- CRUD for Blackboard keys
- BT graph read/write (add/move/remove composite/task/decorator/service, set properties)
- Cross-reference BT ↔ Blackboard in index/context
- “AI & Behavior” focus profile

Done when: “create a patrol behavior” produces BT + BB that’s usable.

---

## Phase 3 — PCG Graphs
Deliverables:
- PCG create/read/add_node/connect/set_settings/execute
- Version gating (PCG APIs vary by UE version)
- “Level & PCG” profile

Done when: common PCG graphs can be assembled/configured + executed with meaningful summaries.

---

## Phase 4 — C++ integration
Deliverables:
- Reflection readers (UClass/enum/struct, blueprint-callable lists, overridables)
- Source readers (headers/cpp) and optional writers (create class/add UPROPERTY/UFUNCTION)
- Bounded source patch writer (`cpp.modify_source`)
- Bridge context: C++ reflection enriches Blueprint edits
- “C++ & Blueprint” profile

Done when: the agent can accurately describe C++ surfaces relevant to BPs, and optionally generate/modify code safely.

---

## Phase 5 — Cross-system intelligence
Deliverables:
- Multi-asset operations (implement interface across project, rename/refactor with dependency awareness)
- Blueprint-to-C++ migration bootstrap (`project.move_to_cpp`, plan + scaffold)
- Snapshot/diff/rollback across multiple assets
- Engine/plugin awareness (filter tools, warn on missing deps, deprecations)
- Prompt templates exposed via MCP (review/debug/migrate/etc.)

Done when: “create an AI enemy with patrol behavior” creates/updates BP + BT + BB coherently with dependency warnings.

---

## Architect must know — implementation constraints
- All writes wrapped in `FScopedTransaction` and run on game thread.
- Strong validation rules (type constraints, pin compatibility, interface restrictions, PIE lockout).
- Large graphs require pagination + targeted reads to keep context manageable.
- Operation feed needs structured progress events for both chat UI and MCP notifications.
- Don’t overbuild node support: start with common nodes and expand iteratively.
