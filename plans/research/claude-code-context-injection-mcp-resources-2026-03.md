# Research: Claude Code Context Injection & MCP Resources

## Question

How does Claude Code CLI support context injection from external tools? What mechanisms
exist for MCP servers to provide context to Claude Code — either passively (resources) or
actively (push)? How do @mentions work? What does the MCP spec say about resources,
sampling, and roots?

---

## Findings

### 1. CLI Flags for Context Injection

Claude Code offers four official flags for modifying what goes into the system prompt.
These are the primary mechanism for injecting context at process launch time.

| Flag | Effect |
|------|--------|
| `--system-prompt "..."` | Replaces the entire default system prompt |
| `--system-prompt-file ./path.txt` | Replaces with file contents |
| `--append-system-prompt "..."` | Appends text to the default prompt (preferred) |
| `--append-system-prompt-file ./path.txt` | Appends file contents to the default prompt |

`--system-prompt` and `--system-prompt-file` are mutually exclusive. Append flags can
combine with either replacement flag. Both work in interactive and non-interactive
(`-p`) modes.

There is **no `--context` flag**. Context injection at launch is done entirely through
these four system prompt flags.

Source: https://code.claude.com/docs/en/cli-reference

**Secondary mechanisms (not flags):**

- **stdin piping**: `cat file.txt | claude -p "query"` — pipe arbitrary content into
  the user turn. This is how content is injected into the *conversation*, not the system
  prompt.
- **CLAUDE.md files**: Loaded from the current directory and parent directories at
  startup. Content is injected into the system prompt on every API call (persistent
  context, survives `/clear`).
- **Hooks / Skills**: Deterministic scripts that run at specific events and inject live
  project data (git status, build outputs, file listings) into prompts before Claude
  sees them. These are not MCP — they are Claude Code's own extension system.

---

### 2. MCP Resources — Full Specification

Resources are a server-side primitive for exposing data. They are **application-controlled**
(not model-controlled like tools). The MCP spec explicitly states different clients handle
resources differently.

**Capability declaration (server side):**
```json
{
  "capabilities": {
    "resources": {
      "subscribe": true,
      "listChanged": true
    }
  }
}
```
Both `subscribe` and `listChanged` are optional.

**Protocol messages:**

- `resources/list` — client discovers available resources (paginated)
- `resources/templates/list` — client discovers URI templates for parameterized resources
- `resources/read` — client fetches a specific resource by URI
- `resources/subscribe` — client subscribes to changes on a specific resource
- `notifications/resources/updated` — server notifies client of a changed resource
- `notifications/resources/list_changed` — server notifies client the resource list changed

**Resource content types:**
- Text: `{ "uri": "...", "mimeType": "text/plain", "text": "content" }`
- Binary: `{ "uri": "...", "mimeType": "image/png", "blob": "base64data" }`

**Resource annotations** (optional metadata on each resource):
- `audience`: `["user"]`, `["assistant"]`, or `["user", "assistant"]`
- `priority`: 0.0–1.0 (1.0 = most important)
- `lastModified`: ISO 8601 timestamp

Source: https://modelcontextprotocol.io/specification/2025-11-25/server/resources

---

### 3. How Claude Code Handles MCP Resources

Claude Code does NOT automatically include MCP resources in the AI context. Resources
are on-demand — they must be explicitly requested.

**The @ mention system:**

Typing `@` in Claude Code's interactive mode opens an autocomplete menu. MCP server
resources appear alongside local files in this menu. The syntax for MCP resources is:

```
@server:protocol://resource/path
```

Examples:
- `@github:issue://123`
- `@docs:file://api/authentication`
- `@postgres:schema://users`

When a resource is @-mentioned, Claude Code fetches it via `resources/read` and injects
the content directly into the conversation as an attachment. This is on-demand, not
automatic.

**Resource subscriptions — NOT IMPLEMENTED in Claude Code:**

Although the MCP spec supports `resources/subscribe` and `notifications/resources/updated`,
Claude Code does NOT implement this. There is an open GitHub issue
(#7252, closed as "Not Planned" on January 5, 2026) requesting that Claude Code
subscribe to MCP resources after reading them and auto-update context when
`notifications/resources/updated` fires. The issue was closed due to inactivity with no
commitment from the Anthropic team.

Source: https://github.com/anthropics/claude-code/issues/7252

---

### 4. Tool Results as Context Injection (Most Reliable Path)

The most reliable way to inject context into Claude Code from an MCP server is via
**tool return values**. When Claude calls an MCP tool, the result goes directly into
the conversation as a tool result message. The LLM sees it immediately on the next turn.

**Tool result content types (from MCP spec 2025-11-25):**

1. **Text**: `{ "type": "text", "text": "content" }` — most common
2. **Image**: `{ "type": "image", "data": "base64", "mimeType": "image/png" }`
3. **Audio**: `{ "type": "audio", "data": "base64", "mimeType": "audio/wav" }`
4. **Resource Link**: `{ "type": "resource_link", "uri": "...", "name": "...", "mimeType": "..." }`
   — provides a URI Claude can follow with `resources/read`, not inline content
5. **Embedded Resource**: `{ "type": "resource", "resource": { "uri": "...", "mimeType": "...", "text": "..." } }`
   — inline resource content embedded directly in the tool result

**EmbeddedResource** is the key content type for injecting structured context via tools.
It includes the full resource content inline, so Claude sees it immediately without a
separate `resources/read` call. Annotations (`audience`, `priority`, `lastModified`) are
supported on embedded resources.

```json
{
  "type": "resource",
  "resource": {
    "uri": "file:///project/context.md",
    "mimeType": "text/markdown",
    "text": "## Project context\n...",
    "annotations": {
      "audience": ["user", "assistant"],
      "priority": 0.9
    }
  }
}
```

Source: https://modelcontextprotocol.io/specification/2025-11-25/server/tools

This is the approach NeoStack AIK uses for its @-mention asset readers: the user types
`@AssetName`, which triggers a tool call, and the tool returns a richly structured
text/resource describing the asset — injecting it into context without the user having
to read a file themselves.

---

### 5. Channels — Active Context Push (Research Preview)

Claude Code v2.1.80+ has a **Channels** feature (research preview) that allows MCP
servers to push events into a running Claude Code session. This is the only mechanism
for active (server-initiated) context injection.

**How it works:**

1. The MCP server declares a custom capability:
   ```json
   { "capabilities": { "experimental": { "claude/channel": {} } } }
   ```
2. The server emits `notifications/claude/channel` notifications:
   ```typescript
   await mcp.notification({
     method: 'notifications/claude/channel',
     params: {
       content: 'build failed on main: https://ci.example.com/run/1234',
       meta: { severity: 'high', run_id: '1234' }
     }
   })
   ```
3. Claude Code receives this and injects it into the session as an XML-wrapped event:
   ```
   <channel source="your-channel" severity="high" run_id="1234">
   build failed on main: https://ci.example.com/run/1234
   </channel>
   ```
4. Claude processes the event and optionally calls a reply tool on the same server.

**Important limitations:**
- Requires `claude.ai` login — Console/API key auth is NOT supported
- The server must be opt-in via `--channels` flag at startup:
  `claude --channels plugin:your-channel@your-marketplace`
- During research preview, servers must be on an Anthropic-approved allowlist;
  development testing uses `--dangerously-load-development-channels server:<name>`
- Events only arrive while the Claude Code session is open
- Server `instructions` field (set in Server constructor) is added to Claude's system
  prompt — use this to tell Claude how to handle channel events

Source: https://code.claude.com/docs/en/channels + https://code.claude.com/docs/en/channels-reference

---

### 6. MCP Sampling — Server-Initiated LLM Calls (Not Context Push)

Sampling (`sampling/createMessage`) is often confused with context injection. It is NOT
a way to push context into an existing Claude Code session. Instead, sampling lets an
MCP server ask the client to run a new LLM completion — a server-initiated agentic loop.

**What sampling does:**
- Server sends `sampling/createMessage` with a messages array + optional tools
- Client routes the request to the LLM (with human-in-loop approval per spec)
- LLM generates a response (possibly using tools in a multi-turn loop)
- Client returns the result to the server

**What sampling does NOT do:**
- It does NOT inject content into the existing conversation where Claude Code is running
- It does NOT modify Claude's current context window
- The MCP spec explicitly requires human-in-loop approval for sampling requests

**`includeContext` parameter** — soft-deprecated in 2025-11-25 spec:
The `thisServer` and `allServers` values (which would inject MCP resource context into
a sampling request) are deprecated. Servers should omit `includeContext` (defaults to
`"none"`). The spec's note: "Servers SHOULD avoid using these values."

Source: https://modelcontextprotocol.io/specification/2025-11-25/client/sampling

**Conclusion for Olive:** Sampling is not a useful mechanism for injecting context into
a Claude Code agent session. It's for server-side agentic loops, not session-level
context management.

---

### 7. MCP Roots — Filesystem Boundaries

The `roots` capability lets servers ask the client which filesystem directories are in
scope. This is **server → client** direction: the server calls `roots/list` to discover
what directories the client has access to.

**Capability declaration (client side):**
```json
{ "capabilities": { "roots": { "listChanged": true } } }
```

**Use:** A server can use roots to scope file operations, understand the project, or
populate its own resources list. This is NOT a mechanism for injecting content into
Claude's context — it's a filesystem boundary negotiation primitive.

Source: https://modelcontextprotocol.io/specification/2025-11-25/client (roots section)

Claude Code does declare the `roots` capability and exposes the working directory. Servers
can call `roots/list` to find out what directory Claude Code is operating in.

---

### 8. MCP Prompts — User-Triggered Templates

MCP servers can expose prompts via `prompts/list` and `prompts/get`. Prompts are
**user-controlled**: they appear as slash commands or menu options that the user
explicitly invokes. They are NOT automatically included in context.

In Claude Code's interactive mode, prompts from connected MCP servers appear as
`/server-name:prompt-name` slash commands. When invoked, the prompt template is expanded
and injected as a user message.

This is different from tools (model-controlled) and resources (application-controlled).

Source: https://modelcontextprotocol.io/specification/2025-11-25/server (prompts section)

---

### 9. Cursor Comparison — Resources Not Yet Supported

As of early 2026, **Cursor does NOT support MCP resources**. Their documentation states:
"Resources are not yet supported in Cursor, though we are hoping to add resource support
in future releases."

Cursor's MCP integration is tools-only. Context injection in Cursor goes through tool
return values.

Cursor does support MCP **prompts**, which appear as `/prompt-name` slash commands.
Cursor also supports MCP **elicitation** — server-initiated structured form UIs that
request input from the user during tool execution.

Source: https://webrix.ai/blog/cursor-mcp-features-blog-post

---

### 10. Summary: Practical Context Injection Mechanisms

| Mechanism | Direction | Reliable? | Notes |
|-----------|-----------|-----------|-------|
| `--append-system-prompt` / `--system-prompt` | Launch-time | Yes | One-shot at startup |
| `--append-system-prompt-file` | Launch-time | Yes | File contents appended |
| stdin pipe (`cat file \| claude -p`) | Launch-time | Yes | Injected as user turn |
| CLAUDE.md / AGENTS.md | Persistent | Yes | Auto-loaded from working dir + parents |
| MCP tool result (text) | On-demand (tool call) | Yes | Most reliable live injection path |
| MCP tool result (EmbeddedResource) | On-demand (tool call) | Yes | Inline resource content |
| MCP `@mention` (resources/read) | On-demand (user action) | Yes | User must type @name |
| MCP Channels push | Active push | Research preview | Requires allowlist + claude.ai login |
| MCP resources/subscribe | Active push | Not implemented | GitHub issue closed "Not Planned" |
| MCP sampling | Server-initiated | Limited | New LLM call, not session injection |
| MCP roots | Server discovery | Yes | Server learns client paths, not injection |
| MCP prompts | User-triggered | Yes | Slash command expansion |

---

## Recommendations

1. **Tool results are the primary injection path.** For the Olive MCP server, any context
   Claude Code needs (asset state, previous compile errors, project knowledge) should be
   returned in tool results — either as text or as `EmbeddedResource` content with
   `audience: ["assistant"]` annotation. This is the most robust, universally supported
   mechanism.

2. **CLAUDE.md / AGENTS.md is the right place for static persistent context.** Tool
   schemas, vocabulary, known patterns, and project conventions belong here. Every tool
   call session starts with this context. Olive already uses this correctly.

3. **Do NOT rely on MCP resources for automatic context injection.** Claude Code does not
   automatically read MCP resources. Users must `@mention` them, or Claude must explicitly
   call `resources/read` as a tool action. Resources are useful as a UI affordance (type
   `@` and pick from a list) but not as a passive injection mechanism.

4. **`resources/subscribe` is a dead end.** The GitHub issue was closed "Not Planned."
   Do not design any feature that depends on subscription-based resource updates in
   Claude Code.

5. **Channels are interesting but too restricted for production use.** The requirement
   for `claude.ai` login and an Anthropic-approved allowlist makes Channels unsuitable
   for Olive's use case (UE editor integration). The `--permission-prompt-tool` flag
   (already in use for Olive) is a better mechanism for bidirectional communication.

6. **For injecting session-specific context at launch, use `--append-system-prompt-file`.**
   The Olive pipeline could write a temporary context file (current project state,
   recently modified assets, compile errors) and pass it via `--append-system-prompt-file`
   at the start of each Claude Code run. This is low-complexity and fully reliable.

7. **The NeoStack @-mention pattern is tools, not resources.** NeoStack's "36+ asset
   type readers" work by triggering tool calls that return structured context — not by
   exposing MCP resources. Olive's equivalent (returning structured Blueprint state in
   tool results) already matches this pattern.

8. **Sampling is not useful for Olive's architecture.** The human-in-loop requirement
   and the fact that sampling creates a new LLM call (rather than injecting into the
   existing session) makes it unsuitable for context injection. Skip it.

9. **Roots (`roots/list`) is useful for Olive's server to discover Claude Code's working
   directory.** If Olive needs to know which project folder Claude Code is operating in,
   the server can call `roots/list` to discover it — useful for resolving relative asset
   paths without requiring the user to configure anything.

10. **The `EmbeddedResource` content type in tool results is underused in the UE tool
    space.** Returning Blueprint node data, compile errors, or asset content as an
    `EmbeddedResource` with appropriate `audience` and `priority` annotations could give
    Claude Code richer context than plain text and potentially enable future client-side
    filtering or display improvements.

---

## Sources

- [Claude Code CLI Reference](https://code.claude.com/docs/en/cli-reference)
- [Claude Code MCP Documentation](https://code.claude.com/docs/en/mcp)
- [Claude Code Channels](https://code.claude.com/docs/en/channels)
- [Claude Code Channels Reference](https://code.claude.com/docs/en/channels-reference)
- [MCP Specification 2025-11-25 Overview](https://modelcontextprotocol.io/specification/2025-11-25)
- [MCP Specification — Resources](https://modelcontextprotocol.io/specification/2025-11-25/server/resources)
- [MCP Specification — Tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools)
- [MCP Specification — Sampling](https://modelcontextprotocol.io/specification/2025-11-25/client/sampling)
- [MCP Specification — Roots](https://modelcontextprotocol.io/specification/2025-11-25/client)
- [GitHub Issue #7252: MCP resource subscriptions for Claude Code](https://github.com/anthropics/claude-code/issues/7252) — closed "Not Planned"
- [Cursor MCP Features — Resources, Prompts, Elicitation](https://webrix.ai/blog/cursor-mcp-features-blog-post)
- [@ Referencing in Claude Code — Steve Kinney](https://stevekinney.com/courses/ai-development/referencing-files-in-claude-code)
