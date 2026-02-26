# Research: NeoStack Architecture (Agent Integration Kit by Betide Studio)

## Question

How does NeoStack (Betide Studio's Agent Integration Kit) handle MCP server tool discovery, Claude Code integration, .mcp.json configuration, AGENTS.md for domain knowledge, process lifecycle (single-process vs. re-launch), and what is their overall design philosophy? What does "NeoStack-style" mean for our Olive AI Studio migration?

## Findings

### Overview

NeoStack's product is called the **Agent Integration Kit (AIK)** — the NeoStack name is used for the brand/product line. It was released on the Epic FAB store January 20, 2026. Documentation lives at `https://aik.betide.studio/`. As of v0.5.0+ (February 2026), it is the most architecturally complete public example of this integration pattern.

It has 25 C++ classes, is editor-only, and supports UE 5.5, 5.6, 5.7 on Windows and macOS. One-time purchase ($109.99), no per-token middleman — users bring their own Claude Code, Gemini CLI, Codex, or Copilot CLI subscriptions.

Source: https://aik.betide.studio/ and https://assetsue.com/file/agent-integration-kit-neostack-ai

---

### 1. MCP Server Architecture

**Transport protocol:** AIK runs an HTTP MCP server inside the Unreal Editor process, on **port 9315** by default (configurable via Project Settings). It simultaneously supports two MCP transport protocols on the same port:

- **Streamable HTTP** (MCP spec 2025-03-26): `POST /mcp` and `GET /mcp` — for Gemini CLI and newer clients
- **HTTP+SSE** (MCP spec 2024-11-05): `GET /sse` and `POST /message` — for Claude Code and legacy clients

Client type is auto-detected based on how they connect. This dual-transport design is a direct response to the MCP spec evolution: SSE was deprecated as a standalone transport in 2025, but Claude Code still uses it as of early 2026.

**Key detail: Claude Code specifically uses the HTTP+SSE 2024-11-05 path** (`GET /sse`), not the newer Streamable HTTP path. Our current `OliveMCPServer` already uses protocol version `2024-11-05` and supports SSE — this is correct.

**Server lifecycle:** The MCP server starts automatically when Unreal Editor launches with the plugin enabled. There is no on-demand startup. The server is always-on while the editor is open.

Source: https://aik.betide.studio/getting-started/configuration

---

### 2. Claude Code Integration — ACP vs. MCP

**The key architectural distinction AIK makes: Claude Code connects to the MCP server via HTTP+SSE, NOT via ACP.**

AIK uses two different integration patterns:

**Pattern A — ACP (Agent Client Protocol):** Used for in-editor chat with Claude Code, Codex, and Copilot CLI. ACP is a JSON-RPC-over-stdio protocol where the agent runs as a subprocess of the editor. The editor spawns the agent process, manages its lifecycle, and communicates via stdin/stdout. This is what enables the "in-editor chat window" with streaming responses, context attachments, and `@mentions`.

The bundled `claude-code-acp` adapter (formerly `@zed-industries/claude-agent-acp`) wraps the Claude Agent SDK in an ACP-compatible interface. As of v0.5.0, the adapter is bundled inside the plugin rather than requiring separate npm installation.

**Pattern B — Pure MCP (HTTP):** Used for external agents like Cursor IDE and for Gemini CLI in non-ACP mode. The agent (e.g., Cursor) connects to `http://localhost:9315/mcp` directly using the MCP protocol. No ACP adapter is involved. Cursor cannot use the in-editor chat window at all because it lacks ACP support.

**For our Olive AI Studio use case:** Our Claude Code CLI provider is using Pattern A (ACP-like — stdio subprocess). In the NeoStack-style migration, Claude Code would be launched as a subprocess but allowed to run autonomously. It discovers tools NOT through ACP tool injection but through the MCP server (`http://localhost:{port}/mcp`). This is still a subprocess process but without per-turn re-launching.

Source: https://aik.betide.studio/agents/choosing-an-agent, https://aik.betide.studio/agents/claude-code

---

### 3. .mcp.json Configuration

**Format confirmed from Cursor documentation:**

```json
{
  "mcpServers": {
    "unreal-editor": {
      "url": "http://localhost:9315/mcp"
    }
  }
}
```

For Gemini CLI (SSE transport):
```json
{
  "mcpServers": {
    "unreal-editor": {
      "url": "http://localhost:9315/sse",
      "timeout": 60000
    }
  }
}
```

**Claude Code `.mcp.json` scope:** Based on the official Claude Code documentation (https://code.claude.com/docs/en/mcp), the `.mcp.json` placed at project root is a **project-scoped** configuration. It is version-controlled and shared with the team. Claude Code reads it automatically when run from that directory. When `--strict-mcp-config` is NOT passed, Claude Code will pick up this file.

**For AIK:** The plugin places its `.mcp.json` (or equivalent) in the project directory so that any externally-run Claude Code instance discovers the Unreal Editor MCP tools automatically. Users do not need to manually run `claude mcp add`.

**Dynamic port:** AIK's MCP server starts on port 9315. Our server tries ports 3000-3009. The existing `plans/neostack-migration-plan.md` already captures the correct solution: write `.mcp.json` after successful bind with the actual port number.

Source: https://aik.betide.studio/agents/cursor, https://aik.betide.studio/agents/gemini-cli, https://code.claude.com/docs/en/mcp

---

### 4. AGENTS.md / Domain Knowledge Injection

**AIK does NOT use AGENTS.md files.** Their mechanism is the **Profiles system**.

Each profile can inject custom instructions into the system prompt. Per the documentation:
> "Each profile can append custom instructions to the system prompt. These guide agent behavior through domain-specific knowledge — for example, the Animation profile includes guidance about motion matching workflows and Pose Search conventions."

Profiles also customize **tool description overrides** — they can rewrite the description of individual MCP tools to emphasize context relevant to that profile's domain (e.g., the Animation profile makes `edit_blueprint` emphasize AnimGraph editing over general Blueprint editing).

**Tool visibility filtering:** Profiles implement a whitelist. An empty tool list = all tools enabled. A non-empty tool list = only those tools are visible to the agent. The agent literally cannot see (or call) tools not in its profile.

**For AGENTS.md specifically:** The `AGENTS.md` standard (https://agents.md/) is an OpenAI Codex convention. Claude Code uses `CLAUDE.md`. AIK does not appear to generate or inject either file. Instead, their approach is:
1. System prompt injection via Profiles (in-editor chat path)
2. For external CLI agents (Cursor, Gemini), no domain knowledge injection — pure MCP tools

**Our existing CLAUDE.md approach is different from NeoStack's.** Our `CLAUDE.md` at the plugin root IS read by Claude Code CLI automatically (it is the CLAUDE.md convention). For the NeoStack-style migration, the `plans/neostack-migration-plan.md` recommends a concise `AGENTS.md` (~60 lines) placed in the working directory. Since we're talking about Claude Code specifically, this should be `CLAUDE.md` in the project root, not `AGENTS.md`.

Source: https://aik.betide.studio/profiles

---

### 5. Process Lifecycle

**AIK process lifecycle for Claude Code (ACP path):**

1. User opens Agent Chat panel in editor
2. User clicks "Connect" → editor spawns `claude-code-acp` as a subprocess (stdio)
3. ACP session established: `session/initialize`, `session/new`
4. User sends prompt → `session/prompt` to the subprocess
5. Agent reasons, calls MCP tools (via HTTP to the in-editor MCP server — separate from the ACP channel)
6. Agent streams responses back via `session/update`
7. **Session PERSISTS** until user disconnects or editor closes — NOT re-launched per request

**The critical difference from our current architecture:**

| | Our current approach | NeoStack-style |
|---|---|---|
| Process start | Per user turn | Per session (once) |
| Process end | After 1 completion | When user disconnects |
| Tool calls | Parsed from `<tool_call>` XML in stdout | Via MCP HTTP (`tools/call`) |
| Loop control | ConversationManager re-launches | Agent self-terminates |
| Context | Rebuilt each turn as text | Agent manages its own context |

**Important nuance from the changelog (v0.5.6, Feb 21, 2026):** AIK added "in-process installation" so Claude Code can install without opening a separate terminal. This suggests earlier versions had external process dependencies that were painful. By v0.5.0, "everything the plugin needs to launch agents is now bundled." This mirrors what our migration plan targets.

**For headless Claude Code (`--print` mode):** The `--print` flag makes Claude Code run non-interactively (returns output and exits). This is what we use. In NeoStack-style, Claude Code is still invoked with `--print` but WITHOUT `--max-turns 1` — it runs until the task is complete, calling MCP tools as many times as needed, then exits. One process per user request (not one per turn).

Source: https://aik.betide.studio/changelog, https://aik.betide.studio/agents/claude-code, https://acpserver.org/

---

### 6. Tool Consolidation Philosophy (v0.5.0)

A major architectural insight from the v0.5.0 changelog (Feb 16, 2026):

> "Tool Consolidation: Reduction from 27+ tools down to about 15 with unified interfaces (e.g., single `edit_rigging` replacing five specialized tools)"

The design philosophy is **fewer, broader tools** rather than many specific ones. One `edit_rigging` tool handles what five separate tools did before. This is the opposite of our current approach (many specific operations as individual tool registrations).

The rationale appears to be:
- Fewer tools = smaller `tools/list` response = less context consumed
- Broader tools = agent decides the sub-operation, not the tool name
- Unified interfaces = agent needs less knowledge about which specific tool to call

Claude Code documentation confirms this concern: "MCP Tool Search activates automatically when MCP tool descriptions consume more than 10% of the context window."

Our current approach has 150+ distinct operations. We should consider whether each operation needs to be a separate MCP tool or whether they can be grouped.

Source: https://aik.betide.studio/changelog, https://code.claude.com/docs/en/mcp

---

### 7. Session Continuity Feature (v0.5.0)

> "Agent Handoff Architecture: New capability to continue conversations across different agents via AI-summarized context"

This implies session state serialization: when switching from Claude Code to OpenRouter (or vice versa), the current conversation context is summarized and fed to the new agent. This is roughly equivalent to our `FOlivePromptDistiller` concept but done differently — at handoff time, not at every turn.

---

### 8. Audit Logging

AIK maintains an audit log at `Saved/Logs/AIK_ToolAudit.log` with format:
```
[timestamp] STATUS | operation_type | asset_name | details
```

This is structurally similar to what we already have via `LogOliveAI`.

---

### 9. Multi-Session Support

AIK supports up to 8 concurrent sessions with auto-save. Each session has its own agent connection. This is more advanced than our current single-session architecture.

---

### 10. Claude Code MCP Transport: SSE vs. HTTP

**Critical finding from official Claude Code docs (https://code.claude.com/docs/en/mcp):**

SSE transport (`GET /sse`) is marked as **deprecated** in MCP spec 2025-03-26. However, as of February 2026, **Claude Code still uses SSE** as its primary transport for HTTP MCP servers (confirmed by AIK supporting SSE on `GET /sse` specifically for Claude Code).

The newer Streamable HTTP transport (`POST /mcp`, `GET /mcp`) is used by Gemini CLI and newer clients. Claude Code will eventually migrate to Streamable HTTP.

**For our `.mcp.json`:** The `type: "http"` entry in our existing migration plan uses the URL `/mcp` which maps to the Streamable HTTP transport. However, if Claude Code still prefers SSE, we may need to use `/sse` endpoint. AIK supports both simultaneously on the same port — that is the safe approach.

Our current `OliveMCPServer` uses protocol `2024-11-05` with an SSE endpoint. The migration plan's `.mcp.json` with `"url": "http://localhost:PORT/mcp"` may need to be `"url": "http://localhost:PORT/sse"` for Claude Code until Streamable HTTP is universally supported.

Source: https://aik.betide.studio/getting-started/configuration, https://code.claude.com/docs/en/mcp

---

## Summary: What "NeoStack-Style" Means

1. **Plugin is a tool server only.** The plugin exposes tools via an always-on MCP HTTP server. It does not drive the agent loop.

2. **Agent runs autonomously.** Claude Code is launched as a subprocess (one per user request), discovers tools via `.mcp.json` → MCP `tools/list`, calls tools natively via `tools/call`, manages its own context and loop, exits when done.

3. **No prompt engineering for tool injection.** Tool schemas are not serialized into system prompts. The MCP protocol handles tool discovery and calling natively.

4. **Domain knowledge via concise context files.** Profiles inject focused system prompt additions. CLAUDE.md or AGENTS.md provides working-directory context. No elaborate prompt assembly machinery.

5. **Fewer, broader tools.** Consolidate from many specific operations to fewer unified tools that agents can parameterize.

6. **Both transport protocols simultaneously.** Support both SSE (for Claude Code) and Streamable HTTP (for Gemini CLI, Cursor) on the same port. Auto-detect by connection method.

---

## Recommendations

1. **Keep SSE endpoint.** Our existing `/sse` endpoint in `OliveMCPServer` is correct for Claude Code. The `.mcp.json` URL should point to `/sse` (not `/mcp`) until Claude Code fully adopts Streamable HTTP. AIK's Gemini documentation confirms SSE is still the Claude Code path. Dual-support both `/sse` and `/mcp` endpoints.

2. **Dynamic `.mcp.json` write on server start.** Write a `.mcp.json` to the plugin root after MCP server binds. Use `"url": "http://localhost:{PORT}/sse"` for Claude Code compatibility. This is the primary discovery mechanism — no `--strict-mcp-config`, no explicit `--mcp-config` flag needed.

3. **One process per request, not per turn.** Remove `--max-turns 1`. Keep `--print --output-format stream-json --verbose --dangerously-skip-permissions`. Claude Code exits naturally when the task is complete. The existing `ReadPipe` idle timeout handles runaway processes.

4. **Tool consolidation is worth considering.** Our 150+ operations registered as individual tools is a risk for context consumption. MCP Tool Search (auto-activates at 10% context threshold) helps, but grouping operations into ~15 unified tools with operation parameters is the cleaner approach.

5. **CLAUDE.md over AGENTS.md.** Claude Code reads `CLAUDE.md` natively. AGENTS.md is an OpenAI Codex convention. For Claude Code specifically, a tight `CLAUDE.md` in the working directory is the right mechanism. Keep it under 60 lines of dense domain knowledge (tool categories, common patterns, asset path format). The existing `plans/neostack-migration-plan.md` Phase 4 content is correct on this.

6. **AIK has no public source code.** Their GitHub (`https://github.com/betidestudio`) has only their networking plugins (EOS, Steam, Edgegap). The AIK source is proprietary. All architectural knowledge was derived from their public documentation.

7. **The existing `plans/neostack-migration-plan.md` is accurate.** The migration plan in our project already correctly identified the NeoStack pattern. This research confirms its analysis. No corrections needed to that plan.

---

## Sources

- [NeoStack AI Landing Page](https://betide.studio/neostack)
- [Agent Integration Kit Documentation](https://aik.betide.studio/)
- [AIK Configuration Page](https://aik.betide.studio/getting-started/configuration)
- [AIK Claude Code Setup](https://aik.betide.studio/agents/claude-code)
- [AIK Gemini CLI Setup](https://aik.betide.studio/agents/gemini-cli)
- [AIK Cursor Setup](https://aik.betide.studio/agents/cursor)
- [AIK Profiles](https://aik.betide.studio/profiles)
- [AIK Changelog](https://aik.betide.studio/changelog)
- [AIK Agent Comparison](https://aik.betide.studio/agents/choosing-an-agent)
- [Claude Code MCP Documentation](https://code.claude.com/docs/en/mcp)
- [ACP Specification](https://acpserver.org/)
- [ACP Introduction (Goose)](https://block.github.io/goose/blog/2025/10/24/intro-to-agent-client-protocol-acp/)
- [zed-industries/claude-agent-acp](https://github.com/zed-industries/claude-agent-acp)
- [AIK Product Listing](https://assetsue.com/file/agent-integration-kit-neostack-ai)
- [Philip Conrod NeoStack Review](https://www.philipconrod.com/co-developing-video-games-using-the-latest-version-of-the-neostack-ai-unreal-game-engine-plugin-using-multiple-llms-via-openrouter/)
