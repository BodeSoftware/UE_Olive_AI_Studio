# MCP Context Injection — Key Facts (2026-03-20)

Full report: `plans/research/claude-code-context-injection-mcp-resources-2026-03.md`

## Claude Code Launch-Time Injection
- `--append-system-prompt "..."` / `--append-system-prompt-file ./file.txt` — append to system prompt
- `--system-prompt "..."` / `--system-prompt-file ./file.txt` — replace entire system prompt
- stdin pipe: `cat file | claude -p "query"` — injected as user turn
- CLAUDE.md / AGENTS.md — auto-loaded, injected every call
- NO `--context` flag exists

## MCP Resources — On-Demand, Not Automatic
- Resources are NOT auto-included in Claude Code context
- `@server:protocol://resource/path` syntax triggers `resources/read` on user @-mention
- `resources/subscribe` NOT implemented in Claude Code — issue #7252 closed "Not Planned"
- Resources appear in `@` autocomplete menu in interactive mode

## Tool Results — Primary Injection Path (Most Reliable)
- Tool return values go directly into conversation context
- Content types: text, image, audio, resource_link, EmbeddedResource
- EmbeddedResource: `{ "type": "resource", "resource": { "uri": "...", "mimeType": "...", "text": "..." } }`
  — inline content, `audience`/`priority` annotations supported
- This is how NeoStack's @-mention asset readers work (tool call → text/resource result)

## Channels — Active Push (Research Preview, Restricted)
- Server declares: `capabilities.experimental["claude/channel"] = {}`
- Server pushes: `notifications/claude/channel` with `{ content, meta }`
- Claude receives: `<channel source="name" ...>content</channel>` in session
- REQUIRES: claude.ai login, `--channels` opt-in flag, Anthropic-approved allowlist
- NOT suitable for Olive production use (allowlist restriction, no API key support)

## Sampling — NOT Context Injection
- `sampling/createMessage` = server asks client to run a NEW LLM completion
- Does NOT inject into existing Claude Code session
- `includeContext` parameter soft-deprecated in 2025-11-25 spec
- Requires human-in-loop approval per spec

## Roots
- `roots/list` = server asks client what filesystem directories are in scope
- Useful for Olive MCP server to discover Claude Code's working directory
- NOT a context injection mechanism

## Prompts
- MCP prompts appear as `/server:prompt-name` slash commands
- User-triggered, not automatic

## Cursor Comparison
- Cursor does NOT support MCP resources at all (tools only)
- Cursor does support MCP prompts (slash commands) and elicitation (structured forms)
