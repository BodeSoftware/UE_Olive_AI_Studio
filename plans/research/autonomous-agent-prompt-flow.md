# Research: Autonomous Agent Prompt Flow

## Question
Map the complete prompt lifecycle for an autonomous AI agent — every piece of text/context it receives,
when it receives it, how it is assembled, and the approximate token cost of each piece.

## Findings

---

### 1. Pre-Launch: Sandbox Setup (`SetupAutonomousSandbox`)

Source: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` lines 323–427

Before the process launches, `SetupAutonomousSandbox()` creates a directory at:
```
{ProjectDir}/Saved/OliveAI/AgentSandbox/
```

The following files are written into that directory. Claude Code uses the sandbox as its
working directory, so it reads all of them before accepting the first user message.

#### 1a. `.mcp.json`

```json
{
  "mcpServers": {
    "olive-ai-studio": {
      "command": "node",
      "args": ["{PluginDir}/mcp-bridge.js"]
    }
  }
}
```

Token cost: ~50 tokens. Purpose: tells Claude Code to connect to the Olive MCP server via
`mcp-bridge.js`. The bridge auto-discovers the actual port (3000–3009). This is what causes
the `tools/list` request to fire at startup.

#### 1b. `CLAUDE.md`

The CLAUDE.md written to the sandbox is NOT the developer CLAUDE.md. It is assembled inline
in `SetupAutonomousSandbox()` from three parts:

**Part 1 — Hardcoded role preamble (~400 chars):**
```
# Olive AI Studio - Agent Context

You are an AI assistant integrated with Unreal Engine 5.5 via Olive AI Studio.
Your job is to help users create and modify game assets (Blueprints, Behavior Trees,
PCG graphs, etc.) using the MCP tools provided.

## Critical Rules
- You are NOT a plugin developer. Do NOT modify plugin source code.
- Use ONLY the MCP tools to create and edit game assets.
- All asset paths should be under /Game/ (the project's Content directory).
- When creating Blueprints, use blueprint.create (with optional template_id)...
- Complete the FULL task: create structures, wire graph logic, compile, and verify.
- Once ALL Blueprints compile with 0 errors and 0 warnings, the task is COMPLETE.
- After creating from a template, write plan_json for EACH function stub...
```

**Part 2 — `cli_blueprint.txt` (~2,400 chars, ~600 tokens):**
Loaded from `Content/SystemPrompts/Knowledge/cli_blueprint.txt`. Contains:
- Three tool approaches (plan_json, granular tools, editor.run_python)
- Workflow steps for CREATE vs MODIFY vs MULTI-ASSET
- Plan JSON ops list and wiring syntax reference
- Function graph vs EventGraph rules
- Variable types quick reference
- Templates and pattern sources guidance
- Rules (complete task, self-correct, no fake success)

**Part 3 — `recipe_routing.txt` (~800 chars, ~200 tokens):**
Loaded from `Content/SystemPrompts/Knowledge/recipe_routing.txt`. Contains:
- When to use `olive.get_recipe`
- Template search patterns
- Library template quality guidance

**Part 4 — `blueprint_design_patterns.txt` (~5,200 chars, ~1,300 tokens):**
Loaded from `Content/SystemPrompts/Knowledge/blueprint_design_patterns.txt`. Contains:
- Cross-blueprint communication decision tree (interface vs dispatcher vs cast)
- Complete Blueprint Interface workflow (Steps A–E with plan_json examples)
- Event Dispatcher workflow (create + broadcast + bind)
- Overlap events patterns (persistent detection vs one-shot)
- Direct cast guidance
- Input events (Enhanced Input + legacy)
- Complete worked example: interaction system

**Total CLAUDE.md:** approximately 9,000–10,000 chars (~2,250 tokens).

This is the PRIMARY knowledge source for the autonomous agent. It is read once at startup
and informs every decision the agent makes.

#### 1c. `AGENTS.md` (copy from plugin root)

`AGENTS.md` at the plugin root is copied to the sandbox. At time of research, this file
is described as a "stale near-copy of CLAUDE.md minus the Subagent section" (see plugin
CLAUDE.md note). It is approximately 15,000–20,000 chars (~4,000–5,000 tokens) and contains
the full plugin architecture documentation intended for plugin developers, NOT for the agent.

**Important finding:** The agent's `CLAUDE.md` already contains the correct agent-role
instructions. `AGENTS.md` provides duplicate and potentially confusing developer-scoped
context. This is a known issue — the plugin CLAUDE.md itself notes AGENTS.md "can be safely
deleted."

---

### 2. CLI Launch — Arguments and Stdin

Source: `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp` lines 215–230
Source: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` lines 474–613

#### CLI Arguments (Autonomous Mode)

```
node {cli.js} --print --output-format stream-json --verbose
              --dangerously-skip-permissions --max-turns {AutonomousMaxTurns}
```

Notable: NO `--strict-mcp-config` (allows `.mcp.json` discovery), NO `--append-system-prompt`
(no system prompt injection in autonomous mode). The working directory is the sandbox,
so Claude Code reads `CLAUDE.md` and `AGENTS.md` from there.

`AutonomousMaxTurns` defaults to 500 (from `UOliveAISettings`).

#### Stdin Message (First Run)

The user's original message is enriched in `SendMessageAutonomous()` before delivery:

**Step 1 — @-mention asset injection** (lines 492–506):
If `InitialContextAssetPaths` is non-empty (set by `SetInitialContextAssets()`), a full
asset state summary is prepended. Format:

```
### Current Asset State

**{AssetPath}** (parent: {ParentClass})
- Components: {comp} ({class}), ...
- Variables: {name} ({type}), ...
- Functions:
  - {funcname} ({N} nodes[-- EMPTY, needs plan_json])
- EventGraph: {N} nodes
- Compile: OK / ERROR / needs recompile
```

This is expensive — each Blueprint adds 200–500 chars depending on complexity.
The suffix `**Do NOT re-read these assets**` is appended.

**Step 2 — Pattern research nudge** (lines 512–514):
Appended to every initial (non-continuation) message:

```
Before building, research patterns from Library templates: search
blueprint.list_templates(query="...") for proven reference patterns from real
projects, specific function templates may be available by
blueprint.get_template(id, pattern="FuncName") to study matching functions.
Supplement with olive.search_community_blueprints if needed — community examples
are mixed quality, compare several before using. These are references — adapt,
simplify, or combine patterns to fit the user's needs. Then build the complete
system using the MCP tools.
```

~400 chars, ~100 tokens.

**Final stdin content structure:**
```
{Original user message}

### Current Asset State     [optional, if @-mentions present]
...

Before building, research patterns...  [always appended on initial messages]
```

---

### 3. MCP Tool Discovery — What the Agent Sees at Startup

Source: `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp` lines 428–590, 804–884

When `mcp-bridge.js` connects to the MCP server, Claude Code sends `initialize` then
`tools/list` and `resources/list`. These results go into Claude Code's own context
(Claude's native MCP handling, not our stdin).

#### `initialize` Response
```json
{
  "protocolVersion": "2024-11-05",
  "capabilities": {
    "tools": {"listChanged": true},
    "resources": {"listChanged": true},
    "prompts": {}
  },
  "serverInfo": {"name": "olive-ai-studio", "version": "0.1.0"}
}
```

#### `tools/list` Response

Returns all registered tools from `FOliveToolRegistry::Get().GetToolsListMCP()`.

In autonomous mode, a tool filter may be active. `DetermineToolPrefixes()` in
`OliveCLIProviderBase.cpp` (lines 39–101) does keyword analysis on the user message:
- Always included: `project.`, `olive.`, `cross_system.`
- If message mentions Blueprint terms: `blueprint.`, `animbp.`, `widget.`
- If message mentions BT/AI: `bt.`, `blackboard.`, `blueprint.`
- If message mentions PCG: `pcg.`
- If message mentions C++: `cpp.`
- Multi-domain or ambiguous: no filter (all tools visible)

The full tool list includes all registered tools with their names, descriptions, and JSON
schemas. At typical Blueprint-focused deployment this is approximately 40–60 tools, each
with a schema of 200–500 chars. Total tools/list response: approximately 15,000–30,000
chars (~4,000–7,500 tokens). This is held natively by Claude Code in its context window.

#### `resources/list` Response

Seven resources are listed (not auto-read, only available on request):
- `olive://project/search` — Project Asset Search
- `olive://project/config` — Project Configuration
- `olive://project/class-hierarchy` — Class Hierarchy
- `olive://blueprint/node-catalog` — Blueprint Node Catalog
- `olive://blueprint/node-catalog/search` — Node Catalog Search
- `olive://behaviortree/node-catalog` — BT Node Catalog
- `olive://behaviortree/node-catalog/search` — BT Node Catalog Search

These are NOT automatically injected into the agent's context. The agent must explicitly
call `resources/read` to use them.

#### `prompts/list` Response

Five prompt templates are registered (source: `OliveMCPPromptTemplates.cpp`):
1. `explain_blueprint` — reads a BP and explains it
2. `review_blueprint` — quality review with optional focus areas
3. `plan_feature` — architecture planning for a new feature
4. `migrate_to_cpp` — BP to C++ migration analysis
5. `debug_compile_error` — diagnose and fix compile errors

These are available via `prompts/get` but are NOT automatically injected. They are
reference prompts the user or agent can invoke explicitly.

---

### 4. What the API-Path (Orchestrated) Agent Sees

This section documents the non-autonomous (`SendMessage`) path for comparison.

Source: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` lines 258–321, 1335–1505

#### CLI Arguments (Orchestrated Mode)
```
--print --output-format stream-json --verbose --dangerously-skip-permissions
--max-turns 1 --strict-mcp-config --append-system-prompt "{SystemPromptText}"
```

`--strict-mcp-config` prevents MCP tool discovery. `--max-turns 1` means one completion
turn. The system prompt is passed via `--append-system-prompt`.

#### System Prompt Content (`BuildCLISystemPrompt`)

Assembled from (lines 1423–1505):

1. **Project context** from `GetProjectContext()`:
   ```
   ## Project
   - Project: {ProjectName}
   - Engine: {EngineVersion}
   - Key Plugins: ...
   - Assets: {N} indexed
   ```

2. **Policy context** from `GetPolicyContext()`:
   ```
   ## Policies
   - MaxVariablesPerBlueprint: {N}
   - MaxNodesPerFunction: {N}
   - EnforceNamingConventions: true/false
   - AutoCompileAfterWrite: true/false
   ```

3. **`recipe_routing` knowledge pack** (~800 chars)

4. **`cli_blueprint` knowledge pack** (~2,400 chars)

5. **Template catalog block** from `FOliveTemplateSystem::GetCatalogBlock()`:
   ```
   [AVAILABLE BLUEPRINT TEMPLATES]
   Search library templates with blueprint.list_templates(query="...")...
   Factory templates (create complete Blueprints, or extract individual functions):
   - gun: ...  Functions: Fire, Reload, ...
   - stat_component: ...
   - projectile: ...
   Reference templates (patterns -- view with blueprint.get_template):
   - component_patterns: ...
   - ue_events: ...
   Library projects:
   - combatfs: 325 templates (2,847 functions total)
   [/AVAILABLE BLUEPRINT TEMPLATES]
   ```
   Approximately 3,000–6,000 chars (~750–1,500 tokens) depending on templates loaded.

6. **Tool schemas** (inline JSON, since no native tool calling):
   Full tool schema JSON for all tools passed to the CLI. Very large — 20,000–40,000 chars.

7. **Tool call format instructions** from `FOliveCLIToolCallParser::GetFormatInstructions()`.

#### Stdin Content (`BuildConversationPrompt`)

```
[User]
{user message}

[Assistant]
{assistant response}
<tool_call id="tc_1">
{"name":"blueprint.create","arguments":{...}}
</tool_call>

[Tool Result: blueprint.create (id: tc_1)]
{tool result json}

[User]
{next user message}

...

## How to Call Tools
Output <tool_call> blocks (NOT plain text). Example:
<tool_call id="tc_1">
{"name": "blueprint.create", "arguments": {"path": "...", "parent_class": "..."}}
</tool_call>
You can output multiple <tool_call> blocks. Every response MUST contain at least one.

## Next Action Required
[First turn guidance or continuation guidance]
```

---

### 5. The Template Catalog Block — Detail

Source: `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp` lines 266–390
Source: `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h` / `OlivePromptAssembler.cpp`

**Who gets it:**
- API/orchestrated path: injected via `BuildCLISystemPrompt()` (into `--append-system-prompt`)
- Autonomous path: the catalog is NOT in the sandbox CLAUDE.md. The agent only learns about
  templates from `cli_blueprint.txt` (which describes how to use `blueprint.list_templates`
  and `blueprint.get_template`) and from the pattern research nudge in the stdin message.

**Format:** `[AVAILABLE BLUEPRINT TEMPLATES]...[/AVAILABLE BLUEPRINT TEMPLATES]`
- Lists factory template IDs with descriptions and extractable function names
- Lists reference template IDs with descriptions
- Lists library projects with template/function counts (e.g., "combatfs: 325 templates")

**This is a critical discrepancy:** The orchestrated path gets a full catalog pre-injected;
the autonomous path does not. The autonomous path relies on the nudge in stdin to prompt the
agent to call `blueprint.list_templates()` itself.

---

### 6. Per-Turn Context — What Changes

In autonomous MCP mode, each tool call/result cycle proceeds entirely inside Claude Code's
own context management. The MCP server does NOT inject additional per-turn context into
the tool responses beyond the tool result itself.

**What the agent sees after each tool call (via MCP):**

The tool result is returned as the MCP `tools/call` response body:
```json
{
  "content": [{"type": "text", "text": "{FOliveToolResult.ToJsonString()}"}],
  "isError": false
}
```

The tool result JSON structure (`FOliveToolResult.ToJsonString()`) includes:
- `success`: boolean
- `error_code` / `message` / `suggestion` on failure
- `data`: tool-specific result data (asset paths, node IDs, compile results, etc.)
- `timing_ms`: execution duration

**No active context injection per turn in autonomous mode.** The agent accumulates its own
context via Claude Code's native conversation management.

---

### 7. On-Error: Self-Correction Context

Source: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

**Important:** Self-correction (`FOliveSelfCorrectionPolicy`) is part of the non-autonomous
(orchestrated/API) path, managed by `FOliveConversationManager`. In autonomous mode, Claude
Code manages its own error recovery via MCP tool results and Claude's internal reasoning.

For the orchestrated path, when a tool fails, the ConversationManager calls
`FOliveSelfCorrectionPolicy::Evaluate()` and injects an enriched error message into the
conversation as a new user message. The progressive disclosure structure is:

**Compile failure message** (`BuildCompileErrorMessage`):
```
[COMPILE FAILED - Attempt N/M] The Blueprint failed to compile after executing '{tool}'.
Errors:
{compile_error_lines}
REQUIRED ACTION: Do NOT declare success. Fix the compile error before finishing.
1. Call blueprint.read on the affected graph with include_pins:true...
2. Focus on the FIRST error — later errors are often caused by the first one.
3. Use connect_pins or set_pin_default to fix the issue, then compile again.
```

**Tool failure message** (`BuildToolErrorMessage`), varies by error code:
- `VALIDATION_MISSING_PARAM`: "Check the tool schema for required parameters."
- `ASSET_NOT_FOUND`: "The asset path is wrong." + auto-search suggestions from project index
- `NODE_TYPE_UNKNOWN`: guidance to check node type
- Generic: "[TOOL FAILED - Attempt N/M] Tool '{name}' failed with error {code}: {message}"

**Error category routing:**
- **Category A (FixableMistake)**: standard retry message, loop detection active
- **Category B (UnsupportedFeature)**: `[UNSUPPORTED]` suffix, no retry encouragement
- **Category C (Ambiguous)**: standard retry on attempt 1, `[ESCALATION]` suffix on attempt 2+

**Plan deduplication:** If the agent submits an identical `apply_plan_json` plan twice,
it gets: `[IDENTICAL PLAN - Seen N time(s)] Previous error: {code} {message}. Change the
failing step's approach or call olive.get_recipe for the correct pattern.`

After 3 identical submissions: `StopWorker`.

**Loop detection escalation → Granular Fallback:**
When `IsLooping()` or `IsOscillating()` triggers AND `bAllowGranularFallback` is true,
the agent is forced into granular fallback mode (`BuildGranularFallbackMessage`). The loop
detector is reset so the agent gets a fresh error budget for granular tool attempts.

---

### 8. Continuation / Auto-Continue Context

Source: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` lines 957–1212

If the run times out or hits the idle limit:
- Up to `MaxAutoContinues = 3` auto-continue restarts are allowed
- Each restart calls `BuildContinuationPrompt()` with a decomposition nudge

`BuildContinuationPrompt()` assembles:
```
## Continuation of Previous Task

### Original Task
{OriginalMessage}

### What Was Already Done
- {asset_path}: {tool1}, {tool2 x3}, ...

### Previously Fetched Resources
- olive.get_recipe name="{name}"
- blueprint.get_template template_id="{id}"

### Previous Run Outcome
{outcome explanation: IdleTimeout / RuntimeLimit / OutputStall / Completed}

### Current Asset State
{BuildAssetStateSummary() for all modified assets}

### Your Task Now
**Do NOT re-read these assets** -- their current state is shown above.
1. Implement the remaining empty functions using blueprint.apply_plan_json.
2. Wire event graph logic if needed.
3. Compile each Blueprint and verify 0 errors.

### Additional Instructions
{user message if substantive, omitted for bare "continue"}
```

The tool call log is capped at 50 entries. Asset summary deduplicates by path and groups
tool calls per asset (e.g., `blueprint.create, add_component x2, apply_plan_json x3`).

Approximate size: 500–3,000 chars depending on how much was done.

---

### 9. Profile-Based Context (API Path Only)

Source: `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp` lines 279–323, 540–548

For the API (non-autonomous) path, `AssembleSystemPrompt()` includes capability knowledge
packs based on focus profile:

| Profile | Knowledge Packs |
|---------|----------------|
| Auto | `blueprint_authoring`, `recipe_routing`, `node_routing`, `blueprint_design_patterns`, `events_vs_functions` |
| Blueprint | `blueprint_authoring`, `recipe_routing`, `node_routing`, `blueprint_design_patterns`, `events_vs_functions` |
| C++ | (none currently) |

These packs (all under `Content/SystemPrompts/Knowledge/`) total approximately:
- `blueprint_authoring.txt`: ~1,500 chars
- `recipe_routing.txt`: ~800 chars
- `node_routing.txt`: unknown (file exists)
- `blueprint_design_patterns.txt`: ~5,200 chars
- `events_vs_functions.txt`: unknown (file exists)

Plus the template catalog block and asset context (token-budget limited to 4,000 tokens
total for the full system prompt).

---

## Timeline Summary

### Pre-Launch (before `node cli.js` runs)

| Component | Location | Est. Tokens |
|-----------|----------|-------------|
| Sandbox CLAUDE.md (hardcoded preamble) | Written to `AgentSandbox/CLAUDE.md` | ~100 |
| `cli_blueprint.txt` | Written to `AgentSandbox/CLAUDE.md` | ~600 |
| `recipe_routing.txt` | Written to `AgentSandbox/CLAUDE.md` | ~200 |
| `blueprint_design_patterns.txt` | Written to `AgentSandbox/CLAUDE.md` | ~1,300 |
| AGENTS.md (plugin developer docs) | Written to `AgentSandbox/AGENTS.md` | ~4,000–5,000 |
| `.mcp.json` | Written to `AgentSandbox/.mcp.json` | ~50 |
| **Total pre-launch** | | ~6,250–7,250 tokens |

### At Launch (Claude Code startup, before first user message)

| Component | Channel | Est. Tokens |
|-----------|---------|-------------|
| `tools/list` response (filtered) | MCP / native Claude context | ~2,000–5,000 |
| `resources/list` response | MCP / native Claude context | ~200 |
| `prompts/list` response | MCP / native Claude context | ~300 |
| **Total at launch** | | ~2,500–5,500 tokens |

### First User Message (stdin)

| Component | Channel | Est. Tokens |
|-----------|---------|-------------|
| User message | stdin | variable |
| @-mention asset state | stdin (optional) | ~50–200/asset |
| Pattern research nudge | stdin (always) | ~100 |

### Per-Turn (each tool call)

| Component | Channel | Est. Tokens |
|-----------|---------|-------------|
| MCP tool result | MCP response body | ~50–2,000 |
| MCP `tools/progress` notification | `GET /mcp/events` poll | ~100 |

### On-Error (orchestrated path only, not autonomous)

| Component | Channel | Est. Tokens |
|-----------|---------|-------------|
| Enriched error message | Injected user message | ~100–500 |
| Auto-search suggestions (ASSET_NOT_FOUND) | Injected user message | ~100–200 |

### On-Continuation

| Component | Channel | Est. Tokens |
|-----------|---------|-------------|
| Continuation prompt | stdin | ~200–1,000 |
| Asset state summary | stdin (if modified assets exist) | ~100–500 |

---

## Recommendations

1. **AGENTS.md duplication is a real problem.** The agent sandbox writes `AGENTS.md` from
   the plugin root, which contains developer-scoped documentation (~4,000–5,000 tokens) that
   contradicts the agent's role. The plugin CLAUDE.md itself calls this file stale. Consider
   writing a minimal agent-specific `AGENTS.md` to the sandbox instead of copying the
   developer one, or omitting it entirely since `CLAUDE.md` already covers the agent role.

2. **Template catalog is absent from autonomous mode.** The orchestrated path injects the
   full catalog via `--append-system-prompt`. The autonomous path relies on a stdin nudge to
   prompt the agent to search for templates itself. This is intentional (avoids bloating the
   sandbox CLAUDE.md) but means the agent must spend a tool call to discover available templates
   on every run. If template discovery latency is a concern, adding a compact catalog summary
   to the sandbox CLAUDE.md would solve it.

3. **`cli_blueprint.txt` and `blueprint_design_patterns.txt` are duplicated between the
   sandbox CLAUDE.md (autonomous) and `BuildCLISystemPrompt()` (orchestrated).** Changes to
   these files affect both paths, which is intentional single-source-of-truth design.
   However, `BuildCLISystemPrompt()` also loads `blueprint_authoring.txt` (the API-path pack)
   which the sandbox CLAUDE.md does not include. An agent on the orchestrated path gets
   `blueprint_authoring.txt` + `cli_blueprint.txt`; an agent on the autonomous path only
   gets `cli_blueprint.txt`. These are largely redundant but `blueprint_authoring.txt` has
   slightly different tool guidance optimized for the API tool-calling format.

4. **Self-correction is orchestrated-path only.** In autonomous mode, Claude Code manages
   its own error recovery using whatever is in the MCP tool result. There is no
   `FOliveSelfCorrectionPolicy` involvement. This means the enriched error messages (with
   auto-search suggestions, rollback warnings, IDENTICAL PLAN detection, etc.) only apply
   to the API-based providers. For the autonomous path, tool results must be self-explanatory.

5. **The `GetActiveContext()` system prompt injection (per-asset Blueprint state in the API
   system prompt) is not used in autonomous mode.** The equivalent is `BuildAssetStateSummary()`
   in the stdin message for @-mentioned assets and in continuation prompts. They are
   architecturally similar but the format differs slightly (the system prompt version is
   indented markdown with component/variable context; the stdin version is flat markdown with
   compile status and function node counts).

6. **Token budget concern with large template catalogs.** The library combatfs project has
   325 templates. `LibraryIndex.BuildCatalog()` generates a catalog line per project
   (not per template), so the library portion of the catalog block is compact. However, factory
   templates list their extractable functions inline, which can grow as more factory templates
   are added. Monitor this as templates are added.

7. **MCP tool filter provides significant context reduction.** For a Blueprint-focused message,
   the filter includes only `blueprint.`, `animbp.`, `widget.`, `project.`, `olive.`,
   `cross_system.` prefixes. This can reduce `tools/list` from 60+ tools to 30–40 tools,
   cutting the native tool context by ~30–50%.

8. **The pattern research nudge in stdin is always injected on non-continuation messages.**
   If the agent is doing a simple structural task (add a variable, rename a function), the
   template research nudge adds ~100 tokens of irrelevant context and may cause unnecessary
   `blueprint.list_templates` calls. Consider conditionally injecting it based on the
   complexity of the task (e.g., only if the message contains graph-building terms).
