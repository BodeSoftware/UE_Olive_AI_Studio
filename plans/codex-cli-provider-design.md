# Codex CLI Provider Design

**Date:** 2026-03-05
**Status:** Draft
**Scope:** Add OpenAI Codex CLI as a second CLI-based provider alongside Claude Code

---

## 1. Research Findings

### 1.1 Codex CLI Overview

OpenAI Codex CLI (`@openai/codex`, v0.111.0 as of writing) is a Rust binary distributed via npm. It is a direct competitor to Claude Code CLI. Key facts:

- **Install:** `npm i -g @openai/codex` or `brew install --cask codex`
- **Auth:** ChatGPT subscription (Plus/Pro/Team/Enterprise) via browser login, or OpenAI API key
- **Binary distribution:** Rust executable wrapped in a Node.js launcher (`bin/codex.js`), with platform-specific optional deps (`@openai/codex-win32-x64`, etc.)
- **Config:** `~/.codex/config.toml` (TOML, not JSON), `~/.codex/rules/` for per-user rules
- **Models:** Configurable via `-m model` or `config.toml`. Default: `gpt-5.3-codex` (as of user's config)

### 1.2 Non-Interactive Mode: `codex exec`

The equivalent of Claude Code's `--print` is the `exec` subcommand:

```
codex exec [OPTIONS] [PROMPT]
```

Key flags:
| Codex Flag | Claude Equivalent | Notes |
|---|---|---|
| `exec` subcommand | `--print` | Non-interactive mode |
| `--json` | `--output-format stream-json` | JSONL output to stdout |
| `--dangerously-bypass-approvals-and-sandbox` | `--dangerously-skip-permissions` | Skip all confirmation prompts |
| `-C <DIR>` | Working directory | Set working directory |
| `--add-dir <DIR>` | (none) | Additional writable directories |
| `-m <MODEL>` | `--model` | Model selection |
| `--skip-git-repo-check` | (none) | Allow running outside git repo |
| `--ephemeral` | (none) | Don't persist session files |
| `-c key=value` | (none) | Override config.toml values |
| `-s <MODE>` / `--sandbox` | (none) | Sandbox policy (read-only, workspace-write, danger-full-access) |
| `--full-auto` | (none) | Convenience: `-a on-request --sandbox workspace-write` |

**Stdin:** If prompt not provided as argument (or `-` is used), instructions are read from stdin. This matches our stdin pipe delivery pattern.

**No `--max-turns` equivalent:** Codex does not expose a turn limit flag. The agent runs until completion or timeout. This means we rely entirely on our process-level timeouts for runaway protection.

**No `--append-system-prompt` equivalent:** Codex does not support injecting system prompts via CLI flags. Context is provided via:
1. Project instructions files (see Section 1.4)
2. Stdin prompt content
3. MCP server-provided context

**No `--strict-mcp-config`:** Codex always discovers MCP servers from its configuration. There is no way to suppress MCP discovery.

### 1.3 MCP Support

Codex has **native MCP support** with two transport types:

1. **Stdio servers:** `codex mcp add <name> -- <command> [args...]` -- launches a child process
2. **Streamable HTTP servers:** `codex mcp add <name> --url <URL>` -- connects directly to HTTP endpoint

**Critical insight:** Codex can connect directly to our HTTP MCP server at `http://localhost:3000/mcp` without needing `mcp-bridge.js`. The bridge is only needed for Claude Code (which expects stdio MCP servers). Codex supports streamable HTTP natively.

MCP servers are registered in `~/.codex/config.toml`:
```toml
[mcp_servers.olive-ai-studio]
url = "http://localhost:3000/mcp"
```

Or per-project via `.codex/` directory (if it follows that convention).

### 1.4 Project Instructions

Codex reads `AGENTS.md` in the project root, just like Claude Code. However:
- Codex does NOT read `CLAUDE.md` (that is Claude-specific)
- Codex has its own `~/.codex/rules/` system for user-level rules
- The `.codex/` project directory may contain project-specific config

For our sandbox setup, we need to write `AGENTS.md` (already done, shared with Claude) but NOT `CLAUDE.md`. The sandbox can be simplified for Codex since it already reads `AGENTS.md`.

### 1.5 Output Format: `--json` JSONL

When `--json` is used with `exec`, Codex outputs JSON Lines to stdout. The format differs from Claude's stream-json. Based on the OpenAI Codex source and documentation, the JSONL events follow this pattern:

```jsonl
{"type":"message","role":"assistant","content":"thinking text..."}
{"type":"function_call","name":"shell","arguments":"{\"command\":\"ls -la\"}"}
{"type":"function_call_output","output":"file1.txt\nfile2.txt"}
{"type":"message","role":"assistant","content":"final response text"}
```

**Key differences from Claude's stream-json:**
- Claude: `{"type":"assistant","message":{"content":[{"type":"text","text":"..."}]}}`
- Codex: `{"type":"message","role":"assistant","content":"..."}`
- Claude tool events: `{"type":"tool_use","name":"..."}`
- Codex tool events: `{"type":"function_call","name":"..."}`
- Claude results: `{"type":"result",...}`
- Codex: process exit signals completion (no explicit result event)

**IMPORTANT:** The exact JSONL schema should be verified by running a real Codex exec session with `--json` and capturing output. The format may have evolved. The coder MUST verify the actual format before implementing `ParseOutputLine`.

### 1.6 Sandbox and Permissions

Codex has a granular sandbox model:
- `read-only` -- can only read files
- `workspace-write` -- can write within the workspace
- `danger-full-access` -- no restrictions

For our use case (autonomous mode), we want `--dangerously-bypass-approvals-and-sandbox` since all actual mutations go through our MCP tools, not through Codex's shell execution. The AI should not be running shell commands -- it should use MCP tools exclusively.

---

## 2. Architecture Decision

### Subclass of FOliveCLIProviderBase

Create `FOliveCodexProvider` as a new subclass of `FOliveCLIProviderBase`, following the exact same pattern as `FOliveClaudeCodeProvider`. The base class already provides:

- Process lifecycle management (LaunchCLIProcess)
- Stdin pipe delivery
- Stdout read loop with idle/runtime timeouts
- MCP tool call tracking (OnToolCalled delegate)
- Continuation/auto-continue system
- Asset state summary building
- Decomposition directive injection

The subclass only needs to override the 5 virtual hooks:
1. `GetExecutablePath()` -- find codex binary
2. `GetCLIArguments()` -- orchestrated mode flags (if we support it)
3. `GetCLIArgumentsAutonomous()` -- autonomous mode flags
4. `ParseOutputLine()` -- JSONL format parsing
5. `GetCLIName()` -- "Codex"

Plus the IOliveAIProvider identity methods:
- `GetProviderName()` -- "Codex CLI"
- `GetAvailableModels()` -- OpenAI model list
- `GetRecommendedModel()` -- "gpt-5.3-codex" or "o3"
- `ValidateConfig()` -- check codex is installed
- `ValidateConnection()` -- run codex --version

### Why NOT a separate class hierarchy

The base class was specifically designed for this multi-CLI pattern. The header already documents "Claude Code, Codex CLI, Gemini CLI, etc." in its comment block. All the heavy lifting (pipe management, timeouts, AliveGuard, continuation prompts) is shared. A separate hierarchy would duplicate 95% of the code.

---

## 3. Claude-Specific Code Audit

### 3.1 Sandbox Setup: `SetupAutonomousSandbox()`

**Problem:** This method writes `CLAUDE.md` and copies `AGENTS.md`. The CLAUDE.md file is Claude-specific -- Codex ignores it. The method is called from `SendMessageAutonomous()` which is in the base class.

**Fix:** Make `SetupAutonomousSandbox()` virtual. The base class implementation writes the MCP config and AGENTS.md. The Claude subclass adds the CLAUDE.md write. The Codex subclass can either use the base implementation as-is, or override to write Codex-specific config.

However, there is a subtlety: the `.mcp.json` file written by the sandbox is **Claude Code specific** -- Codex does not read `.mcp.json`. Codex reads its MCP server config from `~/.codex/config.toml` or per-project `.codex/` directory.

**Revised approach:** `SetupAutonomousSandbox()` becomes virtual with a base implementation that:
1. Creates the sandbox directory
2. Writes AGENTS.md (shared across all CLIs)
3. Calls a new virtual `WriteProviderSpecificSandboxFiles()` hook

The Claude subclass writes `.mcp.json` and `CLAUDE.md`.
The Codex subclass writes nothing additional (MCP is configured via CLI flags).

### 3.2 MCP Server Discovery

**Problem:** Claude Code discovers MCP servers via `.mcp.json` in the working directory. Codex discovers them via `~/.codex/config.toml` or `codex mcp add` commands.

**Fix for Codex autonomous mode:** Instead of relying on file-based discovery, we pass the MCP server URL directly via CLI config override:
```
codex exec --json -c 'mcp_servers.olive.url="http://localhost:3000/mcp"' --dangerously-bypass-approvals-and-sandbox ...
```

This is cleaner than writing config files and avoids polluting the user's global config.

**Verification needed:** Confirm that `-c mcp_servers.olive.url="..."` correctly registers an MCP server. If not, we may need to use `codex mcp add` before launch, or write a project-level `.codex/config.toml`.

### 3.3 IsAutonomousProvider() Check

**Problem:** `OliveConversationManager.cpp` line 221:
```cpp
return bAutonomousEnabled && Provider->GetProviderName() == TEXT("Claude Code CLI");
```

This hardcodes Claude Code as the only autonomous-capable provider.

**Fix:** Check for any CLI provider instead:
```cpp
return bAutonomousEnabled && (
    Provider->GetProviderName() == TEXT("Claude Code CLI") ||
    Provider->GetProviderName() == TEXT("Codex CLI"));
```

Or better: add a virtual `bool SupportsAutonomousMode() const` to `IOliveAIProvider` (default false), override to true in `FOliveCLIProviderBase`. Then:
```cpp
return bAutonomousEnabled && Provider->SupportsAutonomousMode();
```

### 3.4 Provider Factory

**Problem:** `FOliveProviderFactory::CreateProvider()` and `GetAvailableProviders()` need Codex entries.

### 3.5 Error String Format

**Problem:** `HandleResponseComplete` and `HandleResponseCompleteAutonomous` use `GetCLIName()` in the error string. This is already virtual, so Codex returning "Codex" will produce "Codex process exited with code N" -- correct.

The critical format `"process exited with code"` is preserved because it comes from the base class. No issue.

### 3.6 Orchestrated Mode (SendMessage)

**Problem:** The orchestrated path uses `--append-system-prompt` for Claude, which Codex does not support.

**Fix:** For orchestrated mode with Codex, the system prompt must be prepended to the stdin content rather than passed as a CLI flag. Alternatively, we can choose to only support autonomous mode for Codex initially (simpler, and autonomous mode is the primary use case).

**Recommendation:** Phase 1 -- autonomous mode only. Phase 2 (optional) -- orchestrated mode via stdin prompt injection. The orchestrated path is rarely used now that autonomous mode works well.

### 3.7 `bUseAutonomousMCPMode` Setting

**Problem:** This setting currently only applies to Claude Code. When Codex is selected, autonomous mode should also be the default behavior.

**Fix:** The `IsAutonomousProvider()` check in ConversationManager should evaluate this setting for all CLI providers, not just Claude. The virtual `SupportsAutonomousMode()` approach handles this naturally.

---

## 4. File Changes

### 4.1 New Files

```
Source/OliveAIEditor/Public/Providers/OliveCodexProvider.h       # Header
Source/OliveAIEditor/Private/Providers/OliveCodexProvider.cpp     # Implementation
```

### 4.2 Modified Files

| File | Change |
|---|---|
| `Source/OliveAIEditor/Public/Providers/IOliveAIProvider.h` | Add `virtual bool SupportsAutonomousMode() const { return false; }` to `IOliveAIProvider` |
| `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h` | Override `SupportsAutonomousMode()` to return `true`. Make `SetupAutonomousSandbox()` virtual. Add `virtual void WriteProviderSpecificSandboxFiles()` |
| `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | Refactor `SetupAutonomousSandbox()` to call `WriteProviderSpecificSandboxFiles()`. Move `.mcp.json` and `CLAUDE.md` writing to Claude's override. |
| `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp` | Override `WriteProviderSpecificSandboxFiles()` with the `.mcp.json` + `CLAUDE.md` logic extracted from base |
| `Source/OliveAIEditor/Public/Providers/OliveClaudeCodeProvider.h` | Declare `WriteProviderSpecificSandboxFiles()` override |
| `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` | Add `Codex` entry to `EOliveAIProvider` enum |
| `Source/OliveAIEditor/Private/Settings/OliveAISettings.cpp` | Handle `Codex` in switch statements (GetCurrentApiKey, GetSelectedModelForProvider, etc.) |
| `Source/OliveAIEditor/Private/Providers/IOliveAIProvider.cpp` | Add Codex to `CreateProvider()` and `GetAvailableProviders()` |
| `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` | Replace hardcoded provider name check with `SupportsAutonomousMode()` |
| `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp` | Add Codex to the provider dropdown (enum-to-string mapping) |
| `Source/OliveAIEditor/Private/Services/OliveUtilityModel.cpp` | Add Codex to the `!= ClaudeCode` checks (Codex is also a CLI provider, not an HTTP API provider) |

### 4.3 No Changes Needed

| File | Why |
|---|---|
| `mcp-bridge.js` | Codex does NOT use the bridge. It connects directly via HTTP. |
| `OliveMCPServer.h/.cpp` | MCP server is protocol-compliant. Codex connects like any client. |
| `OliveCLIToolCallParser.h/.cpp` | Only used in orchestrated mode (Phase 2, if implemented). |
| `Content/SystemPrompts/` | Knowledge packs are content-agnostic. Same packs work for both CLIs. |
| `AGENTS.md` | Already written for a generic "AI agent" role. Works with both CLIs. |

---

## 5. Settings Changes

### 5.1 EOliveAIProvider Enum

```cpp
UENUM(BlueprintType)
enum class EOliveAIProvider : uint8
{
    ClaudeCode UMETA(DisplayName = "Claude Code CLI (No API Key)"),
    Codex      UMETA(DisplayName = "Codex CLI (ChatGPT / API Key)"),
    OpenRouter UMETA(DisplayName = "OpenRouter (API Key)"),
    // ... rest unchanged
};
```

### 5.2 API Key Handling

Codex can authenticate two ways:
1. **ChatGPT subscription** -- browser login via `codex login`, stored in `~/.codex/auth.json`. No API key needed.
2. **OpenAI API key** -- set via `OPENAI_API_KEY` environment variable or config.toml.

For settings, treat Codex like Claude Code: no API key field in settings. Users authenticate via `codex login` (ChatGPT plan) or environment variable (API key). `ValidateConfig()` checks if `codex` binary exists, `ValidateConnection()` runs `codex --version`.

### 5.3 bUseAutonomousMCPMode

The existing `bUseAutonomousMCPMode` setting already has the right semantics -- it applies to all CLI providers. The `IsAutonomousProvider()` fix (using `SupportsAutonomousMode()`) makes this work automatically for Codex.

### 5.4 Model Selection

Codex model is configurable via `-m <MODEL>` flag. We should respect `UOliveAISettings::SelectedModel` for the Codex provider. Available models include:
- `gpt-5.3-codex` (default for Codex)
- `o3`
- `o4-mini`
- `gpt-4.1`

The model list should be retrieved from the provider or hardcoded initially.

---

## 6. Interface Definitions

### 6.1 FOliveCodexProvider Header

```cpp
// OliveCodexProvider.h

#pragma once

#include "CoreMinimal.h"
#include "Providers/OliveCLIProviderBase.h"

/**
 * OpenAI Codex CLI Provider
 *
 * Implements the Codex CLI as an AI provider by inheriting from
 * FOliveCLIProviderBase and overriding the provider-specific hooks.
 *
 * Codex-specific responsibilities:
 * - Executable path discovery (npm global, brew, direct binary)
 * - CLI argument construction (exec --json, sandbox flags)
 * - JSONL output format parsing (differs from Claude's stream-json)
 * - Direct HTTP MCP connection (no bridge needed)
 * - Version detection and installation checks
 *
 * Key differences from Claude Code:
 * - Uses `codex exec --json` instead of `claude --print --output-format stream-json`
 * - Connects directly to MCP HTTP server (no mcp-bridge.js)
 * - No --append-system-prompt; context via AGENTS.md and stdin only
 * - No --max-turns; relies on process timeouts for runaway protection
 * - No --strict-mcp-config; MCP servers always discoverable
 *
 * Requirements:
 * - Codex CLI installed (npm -g @openai/codex or brew install)
 * - User authenticated via `codex login` (ChatGPT plan) or OPENAI_API_KEY env var
 */
class OLIVEAIEDITOR_API FOliveCodexProvider : public FOliveCLIProviderBase
{
public:
    FOliveCodexProvider();

    // IOliveAIProvider identity
    virtual FString GetProviderName() const override { return TEXT("Codex CLI"); }
    virtual TArray<FString> GetAvailableModels() const override;
    virtual FString GetRecommendedModel() const override;

    virtual bool ValidateConfig(FString& OutError) const override;
    virtual void Configure(const FOliveProviderConfig& Config) override;
    virtual void ValidateConnection(TFunction<void(bool, const FString&)> Callback) const override;

    // Static helpers
    static bool IsCodexInstalled();
    static FString GetCodexExecutablePath();
    static FString GetCodexVersion();

protected:
    // FOliveCLIProviderBase virtual hooks
    virtual FString GetExecutablePath() const override;
    virtual FString GetCLIArguments(const FString& SystemPromptArg) const override;
    virtual FString GetCLIArgumentsAutonomous() const override;
    virtual void ParseOutputLine(const FString& Line) override;
    virtual FString GetCLIName() const override { return TEXT("Codex"); }

    /**
     * Whether the executable requires Node.js to run.
     * Codex ships as a native binary but the npm entry point is a .js launcher.
     * Override to handle both cases.
     */
    virtual bool RequiresNodeRunner() const override;

    /**
     * Write Codex-specific sandbox files.
     * Currently a no-op: Codex discovers MCP via CLI flag, not .mcp.json.
     * AGENTS.md is written by the base class.
     */
    virtual void WriteProviderSpecificSandboxFiles() override;
};
```

### 6.2 IOliveAIProvider Addition

```cpp
// In IOliveAIProvider.h, add to the public interface:

/**
 * Whether this provider supports autonomous MCP mode.
 * CLI-based providers (Claude Code, Codex) return true.
 * API-based providers return false (default).
 */
virtual bool SupportsAutonomousMode() const { return false; }
```

### 6.3 FOliveCLIProviderBase Changes

```cpp
// In OliveCLIProviderBase.h:

// Change SupportsAutonomousMode override:
virtual bool SupportsAutonomousMode() const override { return true; }

// Make sandbox setup virtual:
virtual void SetupAutonomousSandbox();

// New virtual hook for provider-specific sandbox files:
virtual void WriteProviderSpecificSandboxFiles();
```

---

## 7. Data Flow

### 7.1 Autonomous Mode (Primary)

```
User -> ConversationManager -> IsAutonomousProvider() checks SupportsAutonomousMode()
     -> SendMessageAutonomous(message)
     -> FOliveCodexProvider::SendMessageAutonomous() [inherited from base]
        -> SetupAutonomousSandbox()
           -> Creates sandbox dir
           -> Writes AGENTS.md (base)
           -> Calls WriteProviderSpecificSandboxFiles() (no-op for Codex)
        -> GetCLIArgumentsAutonomous()
           -> Returns: exec --json --dangerously-bypass-approvals-and-sandbox
                       --skip-git-repo-check --ephemeral
                       -C <sandbox_dir> --add-dir <project_dir>
                       -c 'mcp_servers.olive.url="http://localhost:<port>/mcp"'
        -> LaunchCLIProcess(args, stdinContent, onExit, sandboxDir)
           -> Spawns codex.exe with args
           -> Writes prompt to stdin
           -> Read loop dispatches ParseOutputLine() per JSONL line
           -> Timeouts: idle stdout, idle tool call, total runtime
        -> HandleResponseCompleteAutonomous(returnCode)
```

### 7.2 MCP Connection Flow (Codex)

```
codex.exe launches
  -> Reads -c mcp_servers.olive.url="http://localhost:3000/mcp"
  -> Connects directly to HTTP MCP server (no bridge)
  -> Sends initialize, tools/list, tools/call via HTTP POST
  -> MCP server dispatches to game thread via AsyncTask
  -> Results flow back via HTTP response
```

Compare with Claude Code:
```
claude.exe launches
  -> Reads .mcp.json in working directory
  -> Spawns `node mcp-bridge.js` as stdio child
  -> Bridge connects to HTTP MCP server
  -> stdio JSON-RPC <-> HTTP JSON-RPC translation
```

---

## 8. Edge Cases and Error Handling

### 8.1 Codex Not Installed

`GetCodexExecutablePath()` returns empty string. `ValidateConfig()` returns false with helpful message. Same pattern as Claude.

### 8.2 Not Authenticated

Codex may fail with an auth error when the user hasn't run `codex login`. The process will exit with a non-zero code and (hopefully) an error message in stdout. The base class error handler (`HandleResponseCompleteAutonomous`) handles this -- non-zero exit with no accumulated output triggers the error callback.

### 8.3 MCP Server Port

The MCP server URL in the CLI config must use the actual running port. Read `UOliveAISettings::MCPServerPort` and use the actual port from `FOliveMCPServer::Get().GetPort()` (in case auto-discovery incremented it). This must be resolved at sandbox setup time, not at provider creation time.

### 8.4 JSONL Format Verification

**CRITICAL:** The exact JSONL format output by `codex exec --json` must be verified empirically. The format described in Section 1.5 is based on reasonable inference from the Codex source and API patterns, but may not be accurate. The coder MUST:

1. Run `codex exec --json "say hello"` and capture raw stdout
2. Run `codex exec --json "list files in current directory"` (triggers tool use) and capture stdout
3. Document the exact event types and JSON structure
4. Implement `ParseOutputLine()` based on actual output

If the format cannot be verified before implementation, implement `ParseOutputLine()` with the base class fallback (treat all lines as text) and add format-specific parsing later.

### 8.5 No --max-turns Equivalent

Unlike Claude Code, Codex has no `--max-turns` flag. For autonomous mode this is acceptable (we have process-level timeouts). For orchestrated mode, this is a problem -- the AI could run indefinitely. This reinforces the recommendation to support autonomous mode only for Phase 1.

### 8.6 Codex Binary Path Discovery (Windows)

Codex installs in several locations:
1. `where codex` -- finds `codex.cmd` or `codex.exe` on PATH
2. npm global: `%APPDATA%/npm/node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/codex/codex.exe`
3. Homebrew (macOS): in PATH after `brew install --cask codex`
4. Direct download: user-defined location

Unlike Claude Code, Codex ships as a native binary, so we should prefer finding `codex.exe` directly rather than going through the Node.js launcher. This avoids Node.js as a dependency for Codex.

### 8.7 Stdin Encoding

Codex reads stdin as UTF-8. Our `FPlatformProcess::WritePipe` writes UTF-8 by default. No issue expected, but verify with non-ASCII content in prompts.

---

## 9. Implementation Order

### Phase 1: Core Provider (MVP)

**Goal:** Codex CLI works in autonomous mode with MCP tools.

1. **T1: IOliveAIProvider virtual method** (~5 min)
   - Add `virtual bool SupportsAutonomousMode() const { return false; }` to `IOliveAIProvider`
   - Override in `FOliveCLIProviderBase` to return `true`

2. **T2: Refactor SetupAutonomousSandbox** (~30 min)
   - Make `SetupAutonomousSandbox()` virtual in base
   - Extract `.mcp.json` + `CLAUDE.md` writing into new `WriteProviderSpecificSandboxFiles()` virtual (Claude overrides, base no-op)
   - Move the override to `FOliveClaudeCodeProvider`
   - Verify Claude Code still works identically after refactor

3. **T3: Fix IsAutonomousProvider** (~5 min)
   - Change hardcoded name check to `Provider->SupportsAutonomousMode()`

4. **T4: Add EOliveAIProvider::Codex enum value** (~15 min)
   - Add to enum in OliveAISettings.h
   - Handle in all switch statements in OliveAISettings.cpp
   - Handle in SOliveAIChatPanel.cpp provider dropdown
   - Handle in OliveUtilityModel.cpp CLI provider checks

5. **T5: Implement FOliveCodexProvider** (~2 hours)
   - Header: `OliveCodexProvider.h`
   - Implementation: `OliveCodexProvider.cpp`
   - Executable discovery: `GetCodexExecutablePath()` -- where.exe, npm global, common paths
   - Version detection: `GetCodexVersion()` -- run `codex --version`
   - CLI arguments for autonomous mode
   - `ParseOutputLine()` -- **verify JSONL format first**, implement accordingly
   - `WriteProviderSpecificSandboxFiles()` -- no-op (MCP via CLI flag)
   - Constructor: set WorkingDirectory

6. **T6: Register in ProviderFactory** (~10 min)
   - Add Codex to `CreateProvider()` case
   - Add to `GetAvailableProviders()` (guarded by `IsCodexInstalled()`)

7. **T7: Verify JSONL format** (~30 min)
   - Run actual Codex exec sessions with `--json`
   - Capture and document the exact event format
   - Adjust `ParseOutputLine()` implementation

### Phase 2: Polish (Optional, After Phase 1 Verified)

8. **T8: Orchestrated mode support** -- Prepend system prompt to stdin instead of using `--append-system-prompt`. Lower priority.

9. **T9: Model list refinement** -- Query available models dynamically or update hardcoded list based on Codex releases.

10. **T10: Codex-specific sandbox files** -- If Codex adds project-level MCP config support (e.g., `.codex/config.toml`), write that file in `WriteProviderSpecificSandboxFiles()` instead of using `-c` CLI flags.

### Dependencies

- T1, T4 are independent and can be done in parallel.
- T2 must be done before T5 (Codex provider needs the refactored base).
- T3 depends on T1.
- T5 depends on T2.
- T6 depends on T4, T5.
- T7 should be done early to inform T5.

**Recommended order:** T7 (verify format) -> T1 + T4 (parallel) -> T2 -> T3 -> T5 -> T6

---

## 10. Key Design Decisions Summary

| Decision | Rationale |
|---|---|
| Subclass of FOliveCLIProviderBase | Base class was designed for this. 95% code reuse. |
| Autonomous mode only (Phase 1) | No --max-turns or --append-system-prompt in Codex. Autonomous mode is the primary use case. |
| Direct HTTP MCP (no bridge) | Codex supports streamable HTTP natively. Simpler, fewer moving parts. |
| MCP config via -c CLI flag | Avoids writing config files. Dynamic port binding. Clean. |
| SupportsAutonomousMode() virtual | Extensible for future CLI providers (Gemini CLI, etc.). Better than hardcoded name checks. |
| WriteProviderSpecificSandboxFiles() hook | Clean separation of sandbox concerns. Base writes shared files, subclass writes provider-specific files. |
| No API key field in settings | Codex authenticates via `codex login` or OPENAI_API_KEY env var, not via our settings. |
| AGENTS.md shared | Already written for generic agent role. Works with both CLIs. |

---

## 11. Risk Assessment

| Risk | Likelihood | Mitigation |
|---|---|---|
| JSONL format differs from expectation | Medium | T7 verifies before implementation. Fallback to text-only parsing. |
| `-c mcp_servers.X.url=...` doesn't work | Low | Alternative: `codex mcp add` before launch, or write `.codex/config.toml` |
| No turn limit causes runaway costs | Low | Process-level timeouts (idle + runtime) already handle this. |
| Codex auth issues are opaque | Medium | Capture stderr in addition to stdout for better error messages. Consider: Codex may output auth errors to stderr, not stdout. |
| Codex CLI updates break JSONL format | Low | JSONL parsing is isolated in ParseOutputLine(). Easy to update. |
