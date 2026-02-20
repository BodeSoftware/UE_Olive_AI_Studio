# Olive AI Studio - Codex (Codex CLI) Agent Instructions

> **Plugin:** Olive AI Studio (`UE_Olive_AI_Studio`)
> **Engine:** Unreal Engine 5.5+
> **Type:** Editor-only plugin (plus minimal runtime IR module)
> **Architecture:** Dual-interface AI assistant (Editor Chat UI + MCP server)
>
> **Development scope (IMPORTANT):**
> Work only inside `b:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\`.
> The parent project (`UE_Olive_AI_Toolkit`) is for testing only—avoid editing outside the plugin directory.

---

## Repository Map (high-level)

```
Source/
├── OliveAIRuntime/          # Minimal runtime module
│   └── IR/                  # Intermediate Representation structs only
└── OliveAIEditor/           # Editor-only module (everything else)
    ├── UI/                  # Slate widgets (chat panel, operation feed)
    ├── Chat/                # Conversation manager, prompt assembly
    ├── Providers/           # API clients (OpenRouter, Anthropic, etc.)
    ├── MCP/                 # MCP server, JSON-RPC, tool registry
    ├── Services/            # Validation, transactions, asset resolution
    ├── Index/               # Project index (search, hierarchy)
    ├── Profiles/            # Focus profiles (tool filtering)
    ├── Settings/            # UDeveloperSettings + config plumbing
    ├── Blueprint/           # Blueprint read/write
    ├── BehaviorTree/        # BT/Blackboard
    ├── PCG/                 # PCG graphs
    └── Cpp/                 # C++ integration
```

Other important files:
- `UE_Olive_AI_Studio.uplugin` (plugin manifest)
- `.mcp.json` (MCP discovery/config)
- `MCP-BRIDGE-README.md`, `mcp-bridge.js` (bridge utilities)
- `plans/` (design/implementation notes)
- `codex/ue-ai-agent-plugin-plan-long.md` (Codex-oriented master plan)
- `Content/SystemPrompts/` (system prompt assets used in-editor)
- `Config/DefaultOliveAI.ini` (default configuration)

---

## Core Principles (project)

1. **Dual interface.** Editor chat for accessibility; MCP for external agents.
2. **Read-heavy, write-careful.** Reading is cheap; writes are transactional.
3. **Surgical edits over recreation.** Prefer granular graph edits to wholesale regeneration.
4. **Show the work.** Surface tool calls/operations clearly (operation feed + structured results).
5. **Fail safely.** Structured errors and suggestions; never corrupt assets.

---

## Codex Workflow Expectations

Use this sequence when implementing features:

1. **Explore:** Locate existing patterns first (search, read nearby code).
2. **Design (when non-trivial):** Write/update a short plan/design note in `plans/` before heavy implementation.
3. **Implement:** Small, focused diffs; keep file moves/renames rare.
4. **Validate:** Prefer running the smallest relevant build/test step available; do not “fix drive-by” unrelated failures.
5. **Report:** Summarize what changed and how to verify it.

### Write Operations (tooling philosophy)

When implementing any “writer” behavior (Blueprint edits, asset mutation, etc.), the required pipeline is:

**Validate → Confirm (tier routing) → Transact → Execute → Verify → Report**

If a change can’t be safely verified, treat it as incomplete.

---

## Coding Standards (C++/UE)

Follow Unreal conventions and existing local style:

### Naming

| Kind | Prefix | Example |
|------|--------|---------|
| UObject classes | `U` | `UOliveAISettings` |
| Actor classes | `A` | `AOliveDebugActor` |
| Structs | `F` | `FOliveToolResult` |
| Interfaces | `I` | `IOliveAIProvider` |
| Enums | `E` | `EOliveOperationStatus` |
| Slate widgets | `S` | `SOliveAIChatPanel` |

### Files

- Header/source pairs: `OliveThing.h` / `OliveThing.cpp`
- Keep the `Olive` prefix to avoid conflicts

### Transactions / Undo

- Any asset mutation must use `FScopedTransaction` and `Modify()` on affected objects.
- Prefer minimal, reversible edits.

### Logging & errors

- Use the project’s log category (search for `LogOliveAI`).
- Return structured error payloads/results; avoid “silent false”.

---

## MCP Tooling (plugin side)

The plugin hosts an MCP/JSON-RPC server and exposes “tools” that external agents can call.

Reader goal:
- A core graph walker that works across *all* `UEdGraph` types
- Type-specific serializers for key systems (e.g., AnimGraph, WidgetTree)

Writer goal:
- All writes must go through the full safety pipeline (Validate/Confirm/Transact/Verify/Report)

If you add or change a tool:
- Keep the schema stable when possible (additive changes preferred)
- Update any tool registry + docs in the same PR/patch

---

## Practical Notes

- Temporary scratch files like `tmpclaude-*` exist in this repo; do not rely on them and avoid adding new “tmp*” artifacts to git.
- Prefer editing existing files in-place; avoid formatting churn.
- If you need new documentation, put it under `docs/` or `plans/` (whichever matches intent).

