# Research: Codex CLI & Claude Code Autonomous Mode (March 2026)

## Question

How does OpenAI's Codex CLI operate (MCP integration, execution model, sandboxing, session management, context injection)?
How does Claude Code CLI's autonomous mode work programmatically (--session-id, --resume, --dangerously-skip-permissions, stream-json, .mcp.json discovery)?

---

## Part 1: Codex CLI (OpenAI)

Source: https://developers.openai.com/codex/cli and related pages, as of March 2026.

### 1.1 Overview

Codex CLI is OpenAI's terminal-based coding agent. Written in Rust. Installed via `npm i -g @openai/codex`. Works on macOS and Linux (experimental Windows/WSL). Requires a ChatGPT Plus/Pro/Business/Edu/Enterprise plan OR a direct API key (`CODEX_API_KEY`). Operates on a per-directory basis — defaults to working only within the git-tracked project directory. Git repo presence required by default (override with `--skip-git-repo-check`).

Current model: `gpt-5-codex` (alias for GPT-5.1-Codex-Max, a frontier agentic model fine-tuned on software engineering tasks).

### 1.2 MCP Server Integration

Codex stores MCP configuration in `~/.codex/config.toml` (user-level) and `.codex/config.toml` (project-level). Both the CLI and the IDE extension share this configuration.

**CLI management commands:**
```bash
codex mcp add <server-name> [--env VAR=VALUE] -- <command>
codex mcp login <server-name>     # OAuth flow
# In TUI: /mcp to see active servers
```

**Config format (config.toml) for STDIO servers:**
```toml
[mcp_servers.my-server]
command = "node"
args = ["./mcp-server.js"]
env = { MY_VAR = "value" }
cwd = "/optional/working/dir"
startup_timeout_sec = 30
tool_timeout_sec = 60
enabled = true
required = false
enabled_tools = ["tool_a", "tool_b"]  # allowlist
disabled_tools = ["tool_c"]           # denylist
```

**Config format for HTTP (streamable) servers:**
```toml
[mcp_servers.remote-server]
url = "https://my-server.example.com/mcp"
bearer_token_env_var = "MY_TOKEN_ENV"
http_headers = { "X-Custom" = "value" }
startup_timeout_sec = 10
tool_timeout_sec = 120
```

**CRITICAL NOTE:** Codex also exposes ITSELF as an MCP server via `codex mcp-server`. This allows the OpenAI Agents SDK to orchestrate Codex as a sub-agent tool. It exposes two tools: `codex()` (start new conversation) and `codex-reply()` (continue existing). This is the reverse direction compared to Olive's use case (Olive IS the MCP server; agents connect TO it).

Source: https://developers.openai.com/codex/mcp

### 1.3 Autonomous Execution Model

Codex uses an **approval-gated agentic loop**. The model receives a task, forms a plan, executes tools (file read/write, shell commands, web search, MCP tool calls), and requests approval at configurable points.

**Approval policy (--ask-for-approval / -a):**
- `untrusted` (default): approve before any write or shell exec
- `on-request`: approve only when the agent explicitly asks
- `never`: fully autonomous — no approvals required

**Shortcut flags:**
- `--full-auto`: combines `on-request` approvals + `workspace-write` sandbox
- `--dangerously-bypass-approvals-and-sandbox / --yolo`: skips everything

**Approval granularity in config.toml:**
```toml
[approval_policy]
sandbox_approval = "untrusted"    # escalation prompts
rules = "on-request"              # execution policy triggers
skill_approval = "on-request"     # skill script approvals
mcp_elicitations = "never"        # MCP system prompts
```

**Multi-turn loop:** The agent runs tool calls, receives results, re-prompts itself, and continues until the task is complete or it stops and awaits approval. No explicit "max-turns" limit is documented for interactive mode; `codex exec` has `--timeout-sec` for CI use.

Source: https://developers.openai.com/codex/cli/features, https://developers.openai.com/codex/cli/reference

### 1.4 Sandboxing & Permissions

**Sandbox levels (--sandbox / -s):**
- `read-only`: no command execution or writes
- `workspace-write` (--full-auto default): write access to the working directory only
- `danger-full-access`: unrestricted machine-wide

**Directory expansion:**
- `--add-dir <path>`: grant write access to additional directories (repeatable)

**Network access:**
Per-server network config in config.toml:
```toml
[sandbox_workspace_write]
network_access = true  # boolean, outbound network for workspace-write mode

[permissions.my-tool]
network = { allowed_domains = ["api.example.com"], proxy = "socks5://..." }
```

**Rules files:** `~/.codex/rules` can define policy triggers for execution approval.

Source: https://developers.openai.com/codex/cli/reference, https://developers.openai.com/codex/config-reference

### 1.5 CLI Flags Reference (Key Flags)

```
codex [FLAGS] [PROMPT]           # interactive TUI
codex exec [FLAGS] [PROMPT]      # non-interactive (CI/pipelines), alias: codex e

Global flags:
  --model, -m <name>             Override model (e.g., gpt-5-codex)
  --oss                          Use local model via Ollama
  --profile, -p <name>           Load named config profile
  --ask-for-approval, -a         untrusted | on-request | never
  --sandbox, -s                  read-only | workspace-write | danger-full-access
  --dangerously-bypass-approvals-and-sandbox / --yolo
  --full-auto                    on-request + workspace-write shortcut
  --cd, -C <path>                Set working directory before execution
  --add-dir <path>               Grant write to additional directory (repeatable)
  --image, -i <file>             Attach image to prompt
  --config, -c <key=value>       Override config value (repeatable)
  --enable / --disable <flag>    Toggle feature flags
  --search                       Enable live web search
  --no-alt-screen                Disable TUI alternate screen

codex exec specific flags:
  --json                         Output newline-delimited JSON events (JSONL)
  --output-last-message, -o      Write final message to file
  --output-schema <schema>       Validate output against JSON Schema
  --ephemeral                    Skip session persistence to disk
  --skip-git-repo-check          Allow running outside a git repo
  --timeout-sec <n>              Kill after N seconds

Session management:
  codex resume [SESSION_ID]      Continue previous interactive session
  codex resume --last            Continue most recent session
  codex fork [SESSION_ID]        Branch conversation into new thread (TUI only, no --json as of Feb 2026)
  codex exec resume <SESSION_ID> Resume exec session non-interactively
  codex exec resume --last       Resume most recent exec session
  codex exec resume --all        Consider sessions from any directory
```

**NOTE:** `codex fork` is TUI-only as of Feb 2026 (GitHub issue #11750). The internal `ThreadManager.fork_thread()` supports headless forking and the app-server exposes it via JSON-RPC, but the exec CLI surface does not yet support it with `--json` output.

Source: https://developers.openai.com/codex/cli/reference, https://developers.openai.com/codex/noninteractive

### 1.6 Session Management & Conversation Continuity

Sessions are stored locally (persistent). Each session has a unique ID. Sessions track:
- Full conversation transcript
- Plan history
- Prior approvals

```bash
# Resume most recent:
codex resume --last

# Resume specific session by ID:
codex resume <SESSION_ID>

# Non-interactive resume:
codex exec resume <SESSION_ID>
codex exec resume --last

# Fork into new thread (interactive only):
codex fork <SESSION_ID>
```

The `codex exec resume` command allows CI pipelines to pick up work mid-task. This is notably different from Claude Code's session model — Codex explicitly supports "resume a pipeline task" as a first-class non-interactive use case.

Source: https://developers.openai.com/codex/cli/features, GitHub issue #3817, #11750

### 1.7 Context Injection (AGENTS.md + config.toml)

**AGENTS.md hierarchy (three-tier, root-to-leaf order):**

1. **Global:** `~/.codex/AGENTS.override.md` → `~/.codex/AGENTS.md` (first non-empty wins)
2. **Repo root → current directory:** walks each dir, checks `AGENTS.override.md` → `AGENTS.md` → fallback filenames
3. **Merge:** files are concatenated root-to-leaf, joined with blank lines. Closer files appear later so they "override" earlier guidance in LLM attention terms.

At most one file per directory level is included. Empty files are skipped. Combined size capped (default 32 KiB, configurable).

**config.toml customization:**
```toml
project_doc_fallback_filenames = ["TEAM_GUIDE.md", ".agents.md"]
project_doc_max_bytes = 65536   # default: 32768
```

**Injection point:** Messages appear near the top of conversation history, before the user prompt, root-to-leaf order.

**Profile-level context:** Named profiles (`--profile`) in config.toml can include custom model settings, approval policies, and context — similar to NeoStack AIK's Profiles system.

Source: https://developers.openai.com/codex/guides/agents-md, https://developers.openai.com/codex/config-reference

---

## Part 2: Claude Code CLI Autonomous Mode

Source: https://code.claude.com/docs/en/cli-reference, https://code.claude.com/docs/en/headless, as of March 2026.

### 2.1 Print Mode (-p) — The Programmatic Entrypoint

All autonomous/programmatic use goes through `-p` (formerly called "headless mode"):

```bash
claude -p "task description" [FLAGS]
```

This runs non-interactively, exits when done. All session flags work with `-p`.

### 2.2 --output-format and stream-json Event Structure

Three formats:
- `text` (default): plain text response to stdout
- `json`: structured JSON with `result`, `session_id`, and metadata fields
- `stream-json`: newline-delimited JSON (NDJSON) — one event per line, emitted in real-time

**stream-json event types (with --verbose --include-partial-messages):**

| Event type | Subtype / Notes |
|------------|-----------------|
| `stream_event` | Wraps Anthropic API streaming events. `event.delta.type == "text_delta"` for text tokens. `event.delta.type == "input_json_delta"` for tool input tokens. |
| `system` | subtype: `"api_retry"` — emitted before an API retry |

**system/api_retry event fields:**
```json
{
  "type": "system",
  "subtype": "api_retry",
  "attempt": 1,
  "max_retries": 3,
  "retry_delay_ms": 2000,
  "error_status": 429,
  "error": "rate_limit",
  "uuid": "...",
  "session_id": "..."
}
```

**json output format (final result):**
```json
{
  "result": "Claude's final response text",
  "session_id": "uuid-of-session",
  "usage": { ... }
}
```

**Practical jq usage:**
```bash
# Stream text tokens only:
claude -p "query" --output-format stream-json --verbose --include-partial-messages | \
  jq -rj 'select(.type == "stream_event" and .event.delta.type? == "text_delta") | .event.delta.text'

# Extract session ID for resumption:
session_id=$(claude -p "Start a review" --output-format json | jq -r '.session_id')
claude -p "Continue" --resume "$session_id"
```

**NOTE:** The GitHub issue #24596 in the anthropics/claude-code repo explicitly flags that `--output-format stream-json` event type reference docs are missing. The above is inferred from available examples and Anthropic API streaming spec. Event types beyond text_delta and api_retry are not formally documented in the CLI docs.

Source: https://code.claude.com/docs/en/headless, https://github.com/anthropics/claude-code/issues/24596

### 2.3 --session-id and --resume

**--session-id `<uuid>`:**
- Specify an exact UUID for the session. Must be a valid UUID format.
- Use case: deterministic session IDs for multi-step CI pipelines where the orchestrator generates and tracks the ID.
- Example: `claude --session-id "550e8400-e29b-41d4-a716-446655440000" -p "task"`

**--resume / -r `<name-or-id>`:**
- Resume a specific session by UUID or human name.
- Can use `--name/-n` to assign human names to sessions for later resumption.
- Example: `claude -r "auth-refactor" "Finish this PR"`

**--continue / -c:**
- Load the most recent conversation in the current directory.
- Simpler than --resume — no ID needed.
- Example: `claude -c -p "Continue from where you left off"`

**--fork-session:**
- When resuming, create a NEW session ID instead of reusing the original.
- Use: `claude --resume abc123 --fork-session`

**--no-session-persistence:**
- Disable session saving to disk entirely.
- Print mode only. Sessions not resumable.

**Multi-step pipeline pattern:**
```bash
# Step 1: start session, capture ID
session=$(claude -p "Review codebase for issues" --output-format json | jq -r '.session_id')

# Step 2-N: continue in same session context
claude -p "Focus on database queries" --resume "$session" --output-format json
claude -p "Generate summary report" --resume "$session" --output-format json
```

Source: https://code.claude.com/docs/en/cli-reference

### 2.4 --dangerously-skip-permissions

Skips all interactive permission prompts. Claude proceeds autonomously without asking for confirmation before file edits, Bash commands, or web requests.

**Usage:**
```bash
claude --dangerously-skip-permissions -p "task"
```

**What it skips:** All permission gates for tool use — file reads/writes, Bash execution, web fetches.

**What it does NOT skip (per official docs):** Certain hard-coded safety behaviors (model-level). The flag name is intentionally verbose — Anthropic provides no short alias to force a conscious decision each invocation.

**Companion flag --allow-dangerously-skip-permissions:**
- ENABLES the option without immediately activating it.
- Allows composing with `--permission-mode`: `claude --permission-mode plan --allow-dangerously-skip-permissions`

**--permission-mode:**
Options: (not fully documented but includes) `plan` mode, default, etc.

**--allowedTools (preferred alternative for production):**
```bash
claude -p "Run tests and fix failures" \
  --allowedTools "Bash(pytest *),Read,Edit"
```
More surgical than `--dangerously-skip-permissions` — whitelists specific tool patterns using permission rule syntax. Trailing ` *` enables prefix matching: `Bash(git diff *)` allows any `git diff ...` command. The space before `*` matters.

**--disallowedTools:**
Removes tools from the model's context entirely (cannot be used at all).

**--tools:**
Restricts the ENTIRE set of built-in tools available. `""` = disable all, `"default"` = all, `"Bash,Edit,Read"` = only those three.

**--permission-prompt-tool:**
In non-interactive mode (`-p`), specify an MCP tool to handle permission prompts programmatically instead of blocking.

Source: https://code.claude.com/docs/en/cli-reference, https://code.claude.com/docs/en/headless

### 2.5 MCP Server Discovery via .mcp.json

Claude Code auto-discovers MCP servers from `.mcp.json` in the working directory. This is the project-scoped config file.

**Scope hierarchy (three levels):**
1. User: `~/.claude/settings.json` (global MCP servers)
2. Project: `.mcp.json` in the current directory (checked into source control — shared with team)
3. Local: `.claude/settings.local.json` (gitignored — personal overrides)

**--setting-sources:** Controls which scopes are loaded: `user,project,local`

**--mcp-config:** Override — load MCP servers from a specific JSON file or JSON string:
```bash
claude --mcp-config ./custom-mcp.json -p "task"
```

**--strict-mcp-config:** Only use servers from `--mcp-config`, ignore all other MCP configs (user + project):
```bash
claude --strict-mcp-config --mcp-config ./isolated-mcp.json -p "task"
```

**.mcp.json format (what Olive uses):**
```json
{
  "mcpServers": {
    "olive-ai-studio": {
      "command": "node",
      "args": ["mcp-bridge.js"]
    }
  }
}
```

**Server types supported:** stdio (command+args), HTTP/SSE (url).

**Connection flow:** Claude Code starts the MCP server process (stdio) or connects to the HTTP endpoint. Calls `initialize` + `tools/list` on startup. All subsequent tool calls route through MCP JSON-RPC.

Source: https://code.claude.com/docs/en/mcp, https://code.claude.com/docs/en/cli-reference

### 2.6 System Prompt & Context Injection

Four flags for system prompt customization (all work in both interactive and -p mode):

| Flag | Behavior |
|------|----------|
| `--system-prompt <text>` | REPLACES the entire default system prompt |
| `--system-prompt-file <path>` | REPLACES with file contents |
| `--append-system-prompt <text>` | APPENDS to default prompt |
| `--append-system-prompt-file <path>` | APPENDS file contents to default |

The two replacement flags are mutually exclusive. Append flags combine with either replacement flag.

**For most programmatic use:** Use `--append-system-prompt` or `--append-system-prompt-file`. Replacing the full prompt loses Claude Code's built-in capabilities (tool use instructions, safety behaviors).

**CLAUDE.md** is auto-loaded as project context (not a CLI flag — Claude Code reads it from the current directory automatically). This is the project-scoped equivalent of Codex's AGENTS.md hierarchy.

**Agent definition:** `--agent <name>` selects a configured subagent. `--agents <json>` defines subagents inline.

Source: https://code.claude.com/docs/en/cli-reference

---

## Part 3: Codex CLI vs Claude Code — Comparison for Olive Integration

| Feature | Codex CLI | Claude Code CLI |
|---------|-----------|-----------------|
| **MCP config location** | `~/.codex/config.toml` + `.codex/config.toml` | `.mcp.json` (project) + `~/.claude/settings.json` (user) |
| **MCP server types** | stdio + HTTP (streamable) | stdio + HTTP/SSE |
| **Context file** | `AGENTS.md` (hierarchical, root-to-leaf merge) | `CLAUDE.md` (single file, auto-loaded) |
| **Non-interactive mode** | `codex exec` + `--json` | `claude -p` + `--output-format json/stream-json` |
| **Session resume** | `codex exec resume <ID>` or `--last` | `claude -p --resume <ID>` or `--continue` |
| **Session fork** | `codex fork <ID>` (TUI only as of Feb 2026) | `claude --resume <ID> --fork-session` |
| **Skip permissions** | `--dangerously-bypass-approvals-and-sandbox` (`--yolo`) | `--dangerously-skip-permissions` |
| **Granular tool allowlist** | `enabled_tools` in config.toml per MCP server | `--allowedTools` with permission rule syntax |
| **Sandbox levels** | `read-only / workspace-write / danger-full-access` | Permission modes (plan / default) + tool allowlists |
| **Max budget** | `--timeout-sec` | `--max-budget-usd`, `--max-turns` |
| **Output streaming** | `--json` (JSONL events) | `--output-format stream-json` |
| **Expose self as MCP server** | YES (`codex mcp-server`) | NO (Claude Code is client only) |

---

## Recommendations

1. **Codex CLI is NOT a threat to Olive's MCP integration design.** Codex connects to MCP servers the same way Claude Code does (stdio or HTTP). Our `.mcp.json` + `mcp-bridge.js` pattern is already compatible. No changes needed for Codex compatibility.

2. **Codex's AGENTS.md hierarchy is superior to a single CLAUDE.md.** The root-to-leaf merge (global → repo → subdirectory) with a 32 KiB cap is well-designed for large projects. If we ever want to support Olive's context being injected into Codex sessions, we would write an `AGENTS.md` file in the project root describing the Olive MCP tools. Note: Codex reads AGENTS.md independently of CLAUDE.md — both agents use the same MCP server but get context differently.

3. **`--strict-mcp-config` on Claude Code is useful for isolated pipeline testing.** When running Olive's CI regression tests with Claude Code as the agent, use `--strict-mcp-config --mcp-config ./test-mcp.json` to prevent unintended MCP servers from interfering.

4. **`--allowedTools` is the right approach for production Olive runs, not `--dangerously-skip-permissions`.** Pattern: `--allowedTools "mcp__olive-ai-studio__*"` to allow all Olive tools while blocking filesystem access outside of what Olive provides. This is safer and auditable.

5. **stream-json event types lack complete official docs.** The `--output-format stream-json` output is NDJSON where each line is a JSON object. Only `stream_event` (wrapping Anthropic API events) and `system/api_retry` are documented. For an Olive orchestrator parsing Claude Code's output, filter on `.type == "stream_event"` and inspect `.event.delta.type` for `"text_delta"` tokens. Track `session_id` from the first `json` output for multi-step continuations.

6. **Codex's `--full-auto` flag is the rough equivalent of Olive's Code mode.** For architect: if we ever want Codex to run Olive tasks autonomously, `codex exec --full-auto --json "task"` is the invocation pattern. Use `workspace-write` sandbox (not `danger-full-access`) for safety.

7. **Session IDs are the correct mechanism for multi-step Olive pipeline orchestration.** Whether using Codex or Claude Code: capture the session ID after step 1, pass `--resume <ID>` for steps 2-N. This preserves conversation context without re-injecting the full system prompt each time.

8. **Codex's `codex exec resume --last` is a convenience feature Olive's orchestrator could emulate.** If the Olive plugin manages Claude Code sessions, tracking the "last session ID" for a given directory and automatically passing `--resume` would reduce boilerplate for multi-step builds.

9. **`--permission-prompt-tool` enables fully automated MCP-mediated permission handling.** An Olive MCP tool could act as a permission arbiter — Claude Code would call it when it needs to approve an action, allowing the Olive plugin to show a confirmation dialog in the UE editor. This is more integrated than `--dangerously-skip-permissions`.

10. **Codex exposing itself as an MCP server (`codex mcp-server`) is architecturally interesting.** This means a single Olive MCP server could in principle orchestrate both Claude Code AND Codex CLI as sub-agents, with both talking to Olive tools. Not an immediate priority but worth noting for multi-agent architecture designs.

---

Sources:
- [Codex CLI overview](https://developers.openai.com/codex/cli)
- [Codex MCP documentation](https://developers.openai.com/codex/mcp)
- [Codex CLI reference (flags)](https://developers.openai.com/codex/cli/reference)
- [Codex CLI features](https://developers.openai.com/codex/cli/features)
- [Codex non-interactive mode](https://developers.openai.com/codex/noninteractive)
- [Codex AGENTS.md guide](https://developers.openai.com/codex/guides/agents-md)
- [Codex config reference](https://developers.openai.com/codex/config-reference)
- [Codex Agents SDK integration cookbook](https://developers.openai.com/cookbook/examples/codex/codex_mcp_agents_sdk/building_consistent_workflows_codex_cli_agents_sdk)
- [Claude Code CLI reference](https://code.claude.com/docs/en/cli-reference)
- [Claude Code headless/programmatic mode](https://code.claude.com/docs/en/headless)
- [Claude Code MCP documentation](https://code.claude.com/docs/en/mcp)
- GitHub issue: [stream-json event types undocumented](https://github.com/anthropics/claude-code/issues/24596)
- GitHub issue: [codex fork non-interactive gap](https://github.com/openai/codex/issues/11750)
