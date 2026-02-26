# NeoStack Migration: Detailed Implementation Plan

## 1. Current State Analysis

### 1.1 The Orchestrated CLI Path (What Exists Today)

The current Claude Code integration is a **plugin-orchestrated** loop where the plugin spawns a new Claude Code process for each "turn," serializes tool schemas as text into the system prompt, and parses `<tool_call>` XML blocks from stdout.

**Data flow (per turn):**
```
ConversationManager.SendUserMessage()
  -> SendToProvider()
    -> BuildSystemMessage() + GetAvailableTools() + PromptDistiller.Distill()
    -> Provider->SendMessage(Messages, Tools, callbacks)
      -> CLIProviderBase.SendMessage()
        -> BuildConversationPrompt() -- formats [User]/[Assistant]/[Tool Result] blocks + "Next Action Required"
        -> BuildCLISystemPrompt()    -- project context + recipe routing + cli_blueprint + template catalog + tool schemas + format instructions
        -> GetCLIArguments()         -- "--print --output-format stream-json --verbose --dangerously-skip-permissions --max-turns 1 --strict-mcp-config --append-system-prompt ..."
        -> Spawn process, write prompt to stdin, read stdout
        -> ParseOutputLine() per line (game thread)
        -> HandleResponseComplete() -- FOliveCLIToolCallParser::Parse() extracts <tool_call> blocks
      <- OnToolCall for each parsed tool call
      <- OnComplete with cleaned text
    -> HandleComplete()
      -> ProcessPendingToolCalls()
        -> ExecuteToolCall() via FOliveToolRegistry
        -> HandleToolResult() with SelfCorrectionPolicy
        -> ContinueAfterToolResults()
          -> SendToProvider() (loop back)
```

### 1.2 Components by Category

#### Components That ARE the Orchestration (bypass/remove for CLI path):

| File | Class/Function | Role | Lines |
|------|---------------|------|-------|
| `Private/Providers/OliveCLIProviderBase.cpp` | `BuildConversationPrompt()` | Formats history as [User]/[Assistant]/[Tool Result] blocks with `<tool_call>` reconstruction | ~90 lines |
| `Private/Providers/OliveCLIProviderBase.cpp` | `BuildCLISystemPrompt()` | Assembles knowledge + tool schemas + format instructions for `--append-system-prompt` | ~80 lines |
| `Private/Providers/OliveCLIProviderBase.cpp` | `HandleResponseComplete()` | Calls `FOliveCLIToolCallParser::Parse()` to extract `<tool_call>` XML | ~35 lines |
| `Public/Providers/OliveCLIToolCallParser.h` + `.cpp` | `FOliveCLIToolCallParser` | Parses `<tool_call id="...">JSON</tool_call>` blocks from text | Entire file |
| `Public/Providers/OliveCLIToolSchemaSerializer.h` + `.cpp` | `FOliveCLIToolSchemaSerializer` | Converts `FOliveToolDefinition[]` to compact text | Entire file |
| `Private/Providers/OliveClaudeCodeProvider.cpp` | `GetCLIArguments()` | Returns `--max-turns 1 --strict-mcp-config --append-system-prompt "..."` | ~8 lines |
| `Private/Chat/OliveConversationManager.cpp` | `SendToProvider()` | Builds system message, distills history, injects iteration budget, sends to provider | ~180 lines |
| `Private/Chat/OliveConversationManager.cpp` | `HandleComplete()` → `ProcessPendingToolCalls()` → `ExecuteToolCall()` → `HandleToolResult()` → `ContinueAfterToolResults()` → `SendToProvider()` | The entire agentic loop | ~500 lines |
| `Private/Chat/OliveConversationManager.cpp` | Correction directives, zero-tool re-prompts, iteration budgets, batch failure tracking | Per-turn control logic | ~150 lines |
| `Public/Brain/OlivePromptDistiller.h` + `.cpp` | `FOlivePromptDistiller` | Summarizes old messages for token economy | Entire file |
| `Public/Brain/OliveSelfCorrectionPolicy.h` + `.cpp` | `FOliveSelfCorrectionPolicy` | Evaluates tool results, builds enriched error messages | Entire file |
| `Public/Brain/OliveRetryPolicy.h` + `.cpp` | `FOliveLoopDetector` | Detects repeated error patterns, oscillation | Entire file |

#### Components That STAY (shared by both paths):

| File | Class | Role |
|------|-------|------|
| `Private/MCP/OliveMCPServer.cpp` | `FOliveMCPServer` | HTTP JSON-RPC MCP server -- core of NeoStack |
| `Public/MCP/OliveToolRegistry.h` + `.cpp` | `FOliveToolRegistry` | Tool registration and execution |
| All tool handlers | `FOliveBlueprintToolHandlers`, `FOliveBTToolHandlers`, etc. | Tool implementations |
| `Public/Chat/OliveConversationManager.h` | `FOliveConversationManager` | Stays for API providers (future) |
| `Public/Brain/OliveBrainLayer.h` | `FOliveBrainLayer` | State machine (simplified for autonomous) |
| `Public/Chat/OliveEditorChatSession.h` | `FOliveEditorChatSession` | Session singleton |
| `Private/UI/SOliveAIChatPanel.cpp` | `SOliveAIChatPanel` | Chat UI |
| `Private/MCP/OliveMCPServer.cpp` | All MCP handlers | tools/list, tools/call, resources, prompts |

### 1.3 The MCP Bridge (Current State)

- `.mcp.json` in plugin root currently points to `mcp-bridge.js` (stdio-to-HTTP bridge)
- `mcp-bridge.js` auto-discovers server on ports 3000-3009 and forwards JSON-RPC
- The bridge is required because Claude Code expects stdio MCP, but our server is HTTP
- Claude Code CLI currently has `--strict-mcp-config` which BLOCKS MCP discovery
- The MCP server already works, handles all tools, dispatches to game thread

### 1.4 Critical Constraint: The MCP Server Is Already a Working Tool Server

The MCP server (`HandleToolsCallAsync`) already:
- Dispatches to game thread via `AsyncTask(ENamedThreads::GameThread, ...)`
- Sets `FOliveToolCallContext` with `Origin = MCP`
- Calls `FOliveToolRegistry::Get().ExecuteTool()`
- Returns MCP-formatted results with `content[]` array and `isError` flag
- Sends progress notifications via event buffer

This is the NeoStack target. The plugin is already a tool server -- we just need to let Claude Code use it.

---

## 2. Phase-by-Phase Implementation

### Phase 0: Feature Flag and Settings (Prerequisite)

Add a setting to toggle between orchestrated and autonomous mode. This is the safety net -- if autonomous mode has issues, users flip back.

**Files to modify:**
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`

**Changes:**
Add to the "AI Provider" section:
```cpp
/** Use autonomous MCP mode for Claude Code CLI.
 *  When enabled, Claude Code discovers tools via MCP and manages its own loop.
 *  When disabled, the plugin orchestrates each turn (legacy behavior). */
UPROPERTY(Config, EditAnywhere, Category="AI Provider",
    meta=(DisplayName="Autonomous MCP Mode (Claude Code)"))
bool bUseAutonomousMCPMode = true;
```

**Complexity:** Small

---

### Phase 0.5: Tool Resilience Hardening (Critical Prerequisite)

**Why this is Phase 0.5, not optional:** In the orchestrated path, `FOliveSelfCorrectionPolicy` enriches bare tool errors with progressive disclosure, blueprint context, and retry guidance. In autonomous mode, the AI has ONLY the raw tool error message to self-correct from. If error messages are vague or parameters are rejected due to trivial aliasing issues, the autonomous agent burns turns. Tool resilience is the foundation autonomous mode needs.

**Audit findings (from codebase exploration):**

1. **`plan_json` / `plan` aliasing is NOT implemented** — the migration plan claimed "Both plan_json and plan work" but this is FALSE. The handlers only accept `plan_json`. An agent sending `{"plan": {...}}` gets a cryptic missing-parameter error.

2. **Non-blueprint tools have ZERO parameter aliasing** — BT, PCG, Cpp, and CrossSystem tools reject any parameter name that isn't exact. `NormalizeBlueprintParams()` only runs for `blueprint.*` tools.

3. **Unguarded `GetStringField()` across BT, PCG, Cpp handlers** — ~50 parameters use `GetStringField()` (returns empty string on missing) instead of `TryGetStringField()` + validation. Missing parameters silently pass empty strings downstream, producing confusing errors like "asset not found: " instead of clear "missing required parameter 'path'".

4. **Type parsing gaps** — `ParseTypeFromParams` doesn't accept `FString`, `FVector`, `FRotator`, `FTransform`, `str`, `vec`, `vec3` etc. BT `ParseKeyType` is case-sensitive (rejects "Bool", "BOOL").

5. **Error messages missing suggestions** — `OliveGraphBatchExecutor.cpp`, BT, PCG, Cpp, and CrossSystem handlers return bare error messages without suggestions. Blueprint handlers are the gold standard (code + message + suggestion).

#### 0.5A: Generalize parameter normalization

**Files to modify:**
- `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp` -- extend `NormalizeBlueprintParams()` into `NormalizeToolParams()` with per-family alias maps

**Changes:**
- Rename `NormalizeBlueprintParams()` → `NormalizeToolParams()` (or add a new encompassing function)
- Add alias maps for each tool family:
  - `blueprint.*`: existing path aliases + `plan_json`←`plan`/`steps`, `function_name`←`name`/`function`, `graph_target`←`graph`/`target_graph`, `parent_class`←`parent`/`base_class`, `template_id`←`template`/`id`
  - `bt.*`/`blackboard.*`: `path`←`asset_path`/`asset`, `key_type`←`type`
  - `pcg.*`: `path`←`asset_path`/`asset`, `settings_class`←`type`/`node_type`/`class`
  - `cpp.*`: `class_name`←`name`/`class`, `file_path`←`path`/`file`, `module_name`←`module`, `property_name`←`name`, `property_type`←`type`, `function_name`←`name`/`function`, `anchor_text`←`anchor`/`search_text`
  - `project.*`: `asset_path`←`path`/`asset`, `paths`←`assets`/`asset_paths`

#### 0.5B: Add required parameter validation to BT/PCG/Cpp/CrossSystem handlers

**Files to modify:**
- `Source/OliveAIEditor/BehaviorTree/Private/OliveBTToolHandlers.cpp` -- ~15 parameters need guards
- `Source/OliveAIEditor/PCG/Private/OlivePCGToolHandlers.cpp` -- ~10 parameters need guards
- `Source/OliveAIEditor/Cpp/Private/OliveCppToolHandlers.cpp` -- ~15 parameters need guards
- `Source/OliveAIEditor/CrossSystem/Private/OliveCrossSystemToolHandlers.cpp` -- ~5 parameters need guards

**Pattern:** Replace `Params->GetStringField(TEXT("param"))` with:
```cpp
FString ParamValue;
if (!Params->TryGetStringField(TEXT("param"), ParamValue) || ParamValue.IsEmpty())
{
    return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
        TEXT("Required parameter 'param' is missing"),
        TEXT("Provide 'param' as a string. Example: \"param\": \"value\""));
}
```

#### 0.5C: Expand type parsing aliases

**Files to modify:**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` -- `ParseTypeFromParams()` alias map
- `Source/OliveAIEditor/BehaviorTree/Private/OliveBTToolHandlers.cpp` -- `ParseKeyType()` case-insensitivity

**Type aliases to add in ParseTypeFromParams:**
- `str`, `fstring` → `string`
- `fname` → `name`
- `ftext` → `text`
- `fvector`, `vec`, `vec3` → `vector`
- `frotator`, `rot` → `rotator`
- `ftransform` → `transform`
- `flinearcolor` → `linear_color`
- `fcolor` → `color`
- `fvector2d`, `vec2`, `vec2d` → `vector2d`
- `float32` → `float`
- `float64` → `double`
- `uint8` → `byte`

**BT ParseKeyType fix:** Add `TypeStr = TypeStr.ToLower()` at the top (matches Blueprint parser pattern).

#### 0.5D: Improve error messages in batch executor and non-blueprint handlers

**Files to modify:**
- `Source/OliveAIEditor/Blueprint/Private/Batch/OliveGraphBatchExecutor.cpp` -- 6 error returns need suggestions
- BT/PCG/Cpp handler error returns -- add suggestion strings to all `Error()` calls

**Pattern:** Every `Error()` call should have 3 parts: code, message, suggestion.

**Complexity:** Medium (spread across many files but each change is mechanical)

---

### Phase 1: Dynamic .mcp.json + MCP Server Startup Guarantee

**Goal:** After the MCP server starts, write a `.mcp.json` pointing to the actual port. Ensure the MCP server is always running when Claude Code is the active provider.

#### 1A: Write .mcp.json after server bind

**Files to modify:**
- `Source/OliveAIEditor/Public/MCP/OliveMCPServer.h` -- add `WriteMcpConfigFile()` declaration
- `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp` -- implement `WriteMcpConfigFile()`, call it in `Start()` after successful bind

**Details:**
- New public method `void WriteMcpConfigFile()` on `FOliveMCPServer`
- Writes to `{PluginDir}/.mcp.json` with `{"mcpServers":{"olive-ai-studio":{"command":"node","args":["mcp-bridge.js"]}}}`
- NOTE: Keep the stdio bridge format, NOT direct HTTP. Reason: Claude Code's `--mcp-config` and `.mcp.json` primarily support `command`-type servers (stdio). The bridge translates stdio to HTTP. Direct `"type": "http"` support in Claude Code is newer and not guaranteed.
- Actually: Claude Code DOES support `"type": "url"` in `.mcp.json` for Streamable HTTP MCP. But the bridge is battle-tested. Decision: Write BOTH formats -- a `command` entry using the bridge (backward compat) and note the URL for direct HTTP if desired. Actually, simplest approach: keep the `command` bridge entry but ensure port is correct. The bridge auto-discovers ports 3000-3009 anyway, so the `.mcp.json` doesn't need to change per-port. However, writing the actual port into `.mcp.json` is still useful for direct-HTTP future use.
- Add `void CleanupMcpConfigFile()` called from `Stop()` -- removes the file so stale port info doesn't persist.

**Also modify:** `.mcp.json` -- this file is checked into the repo. After this phase, it gets overwritten dynamically at runtime. The checked-in version remains as fallback.

#### 1B: Ensure MCP server auto-starts

**Files to check (no change expected):**
- `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp` -- `bAutoStartMCPServer` default is already `true`
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` -- `bAutoStartMCPServer = true` already

This is already correct. No changes needed.

**Complexity:** Small

---

### Phase 2: Autonomous Launch Path in CLIProviderBase

**Goal:** Add a parallel `SendMessage` codepath that launches Claude Code WITHOUT `--max-turns 1`, `--strict-mcp-config`, or `--append-system-prompt`. Claude Code runs until it decides to stop, calling tools via MCP.

#### 2A: New virtual hook on ClaudeCodeProvider

**Files to modify:**
- `Source/OliveAIEditor/Public/Providers/OliveClaudeCodeProvider.h` -- add `GetCLIArgumentsAutonomous()` declaration
- `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp` -- implement it

**New method:**
```cpp
virtual FString GetCLIArgumentsAutonomous() const;
```
Returns: `--print --output-format stream-json --verbose --dangerously-skip-permissions`

No `--strict-mcp-config`, no `--append-system-prompt`.

Add `--max-turns 50` as cost ceiling (configurable via settings). This is NOT the same as `--max-turns 1` -- it's a safety cap, not orchestration. 50 is needed because each MCP `tools/call` counts as a turn, and complex multi-asset tasks (e.g., "create an inventory system with BP_Item, BP_Inventory, and BP_PickupActor") can easily require 40-60 tool calls across reads, creates, component additions, variable additions, plan applications, and compiles.

#### 2B: SendMessageAutonomous on CLIProviderBase

**Files to modify:**
- `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h` -- add `SendMessageAutonomous()` declaration + new virtual `GetCLIArgumentsAutonomous()`
- `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` -- implement `SendMessageAutonomous()`

**Key differences from `SendMessage()`:**
1. Does NOT call `BuildConversationPrompt()` or `BuildCLISystemPrompt()`
2. Does NOT serialize tool schemas (Claude discovers them via MCP)
3. Passes user message directly to stdin (may optionally include `CLAUDE.md`-style context preamble)
4. Uses `GetCLIArgumentsAutonomous()` instead of `GetCLIArguments(SystemPromptArg)`
5. Does NOT call `HandleResponseComplete()` (no `<tool_call>` parsing needed -- tools go through MCP)
6. Process runs until natural exit; completions signal via `OnComplete`
7. On `tool_use`/`tool_result` events in stdout, emit progress chunks to UI (informational only)

**Method signature:**
```cpp
void SendMessageAutonomous(
    const FString& UserMessage,
    FOnOliveStreamChunk OnChunk,
    FOnOliveComplete OnComplete,
    FOnOliveError OnError
);
```

Note: No `Messages`, no `Tools` parameter. This is a one-shot "give the agent a task, let it run."

**Implementation approach: Extract shared `LaunchCLIProcess()` helper.**

The pipe management, AliveGuard pattern, read loop, idle timeout, and process cleanup are ~200 lines of delicate async code in `SendMessage()`. Copying this creates a maintenance hazard — subtle race condition fixes would need to be applied in two places. Extract the common parts into a shared helper:

```cpp
/** Shared process lifecycle: spawn CLI, pipe stdin, read stdout, handle exit.
 *  Both SendMessage() and SendMessageAutonomous() call this. */
void LaunchCLIProcess(
    const FString& CLIArgs,           // fully-built CLI argument string
    const FString& StdinContent,       // written to stdin then EOF (empty = no stdin)
    TFunction<void(int32)> OnProcessExit  // called on game thread with return code
);
```

**What goes in `LaunchCLIProcess()`:**
- Callback storage + generation counter + AccumulatedResponse reset
- AliveGuard + CLIName capture
- Background AsyncTask entry
- `GetExecutablePath()` + `.js`/`.exe` dispatch
- Stdout + stdin pipe creation
- `CreateProc()` + failure handling
- Child-side pipe end cleanup
- Stdin delivery (if `StdinContent` non-empty) + EOF close
- Stdout read loop with idle timeout
- Post-exit drain
- `GetProcReturnCode` + `CloseProc`
- Completion dispatch via `OnProcessExit`

**What stays in callers:**
- `SendMessage()`: `BuildConversationPrompt()`, `BuildCLISystemPrompt()`, system prompt escaping, `GetCLIArguments(SystemPromptArg)`, `HandleResponseComplete()` as exit handler
- `SendMessageAutonomous()`: `GetCLIArgumentsAutonomous()`, user message as stdin, `HandleResponseCompleteAutonomous()` as exit handler

**Refactoring `SendMessage()` to use the helper:** This is a refactor-then-extend pattern. First, extract the helper from existing `SendMessage()` code (verify it still works). Then add `SendMessageAutonomous()` as a thin caller of the same helper. This way the existing path is tested before the new one is added.

#### 2C: Update ParseOutputLine for autonomous mode events

**Files to modify:**
- `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp` -- update `ParseOutputLine()`

In autonomous mode, Claude Code's stream-json output includes MCP tool interaction events:
```json
{"type": "tool_use", "name": "blueprint.create", "input": {...}}
{"type": "tool_result", "content": "..."}
```

These are informational -- the actual tool execution happens via MCP server, not through the provider. ParseOutputLine should:
- On `tool_use`: emit a progress chunk to UI (e.g., "Calling blueprint.create...")
- On `tool_result`: emit a brief status chunk (e.g., "Tool completed")
- Continue handling `assistant` and `result` types as before

**Important:** The existing code logs a WARNING for `tool_use` because `--max-turns 1` was supposed to prevent it. In autonomous mode, `tool_use` is expected. The coder needs to handle both modes cleanly -- check a flag or just always handle it gracefully.

#### 2D: Autonomous completion handling

**Files to modify:**
- `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` -- new `HandleResponseCompleteAutonomous()`

When the autonomous process exits:
- Do NOT parse `<tool_call>` blocks (tools went through MCP)
- Emit final accumulated text via `OnComplete`
- If exit code != 0 and no output, emit error

**Complexity:** Large (2B is the biggest piece)

---

### Phase 3: ConversationManager Routing

**Goal:** When the active provider is Claude Code and autonomous mode is enabled, bypass the entire orchestrated loop. Route through a simpler path.

**Files to modify:**
- `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h` -- add `IsAutonomousProvider()`, `SendUserMessageAutonomous()`
- `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` -- implement routing

#### 3A: Provider detection

```cpp
bool FOliveConversationManager::IsAutonomousProvider() const
{
    if (!Provider.IsValid()) return false;
    // Check provider name AND settings flag
    const UOliveAISettings* Settings = UOliveAISettings::Get();
    bool bAutonomousEnabled = Settings && Settings->bUseAutonomousMCPMode;
    return bAutonomousEnabled && Provider->GetProviderName() == TEXT("Claude Code CLI");
}
```

#### 3B: Route in SendUserMessage

At the top of `SendUserMessage()`, after the provider check:
```cpp
if (IsAutonomousProvider())
{
    SendUserMessageAutonomous(Message);
    return;
}
// ... existing orchestrated path
```

#### 3C: SendUserMessageAutonomous implementation

This is MUCH simpler than the orchestrated path:
1. Add user message to history (for UI display)
2. Set `bIsProcessing = true`
3. Ensure MCP server is running
4. **Auto-snapshot before autonomous run** — `FOliveSnapshotManager::Get().CreateSnapshot()` with label "Pre-autonomous: {truncated message}". This gives users one-click undo if the autonomous agent makes a mess. Especially important because Tier 3 confirmations (delete, reparent) are bypassed in MCP mode.
5. `Brain->BeginRun()`
6. Call `Provider->SendMessageAutonomous(Message, OnChunk, OnComplete, OnError)` (via interface method)
7. OnChunk: stream text to UI
8. OnComplete: `bIsProcessing = false`, `Brain->CompleteRun()`, `OnProcessingComplete.Broadcast()`
9. OnError: `bIsProcessing = false`, `HandleError()`

**What gets bypassed:**
- `BuildSystemMessage()` -- no system message assembly
- `GetAvailableTools()` -- tools discovered via MCP
- `PromptDistiller.Distill()` -- Claude manages its own context
- Iteration budget injection -- Claude controls its own turns
- Correction directives -- Claude self-corrects via tool results
- `ProcessPendingToolCalls()` -- tools execute via MCP server
- `SelfCorrectionPolicy.Evaluate()` -- not needed
- `LoopDetector` -- not needed
- Zero-tool re-prompts -- not needed
- Batch failure tracking -- not needed

**What still works:**
- Message history (user message + final assistant response)
- Brain state machine (Idle -> WorkerActive -> Completed)
- UI streaming
- Background completion notifications
- Message queue (queue during processing, drain after)
- Focus profile (used for MCP tools/list filtering if we want)

#### 3D: Interface change for autonomous sending

The existing `IOliveAIProvider` interface has `SendMessage(Messages, Tools, ...)`. We need autonomous support. Two approaches:

**Option A: Add SendMessageAutonomous to IOliveAIProvider interface**
- Pro: Clean interface
- Con: All 8 providers need a default impl that errors

**Option B: Dynamic cast to FOliveCLIProviderBase in ConversationManager**
- Pro: No interface change
- Con: Tight coupling

**Decision: Option A** with a default implementation that calls `OnError("Autonomous mode not supported by this provider")`. Only CLIProviderBase overrides it. This future-proofs for Gemini CLI, Codex CLI, etc.

**Files to modify:**
- `Source/OliveAIEditor/Public/Providers/IOliveAIProvider.h` -- add `SendMessageAutonomous()` with default error impl

**Complexity:** Medium

---

### Phase 4: AGENTS.md Overhaul

**Goal:** Transform AGENTS.md from a stale copy of CLAUDE.md into a focused ~60-80 line domain knowledge document that Claude Code reads automatically.

**Key insight:** Claude Code reads BOTH `AGENTS.md` AND `CLAUDE.md` from the working directory. It also discovers tools via MCP `tools/list`, which already includes good schema descriptions. Therefore:

- **AGENTS.md** = tool USAGE guide — workflow patterns, Plan JSON format, exec_after rules, gotchas. Things the tool schemas CAN'T express.
- **CLAUDE.md** = project context — engine version, directory structure, coding conventions, architecture. (Already fine, no changes needed.)
- **Tool schemas** = parameter docs — what each tool accepts and returns. (Already good, don't duplicate in AGENTS.md.)

**Files to modify:**
- `AGENTS.md` (plugin root) -- rewrite entirely

**Content structure (focus on what schemas can't express):**
1. One-paragraph intro: "You have MCP tools for Unreal Engine 5.5 asset manipulation"
2. Tool category overview (5 lines — just the namespaces, not parameter docs)
3. **Multi-tool workflow sequences** — the critical create → add_component → add_variable → apply_plan_json pipeline
4. **Plan JSON format and rules** — step structure, exec_after chaining, pure vs impure nodes, event types. This is the #1 thing the agent needs that schemas can't express.
5. **Asset path conventions** — `/Game/...` format, why relative paths fail
6. **Gotchas and common mistakes:**
   - Read before modify
   - Preview before apply
   - Every impure node needs `exec_after` chaining
   - Don't batch `preview_plan_json` and `apply_plan_json` in the same response
7. Available factory templates (`blueprint.list_templates`)

**What gets REMOVED from AGENTS.md:**
- Full architecture documentation (stays in CLAUDE.md)
- Build commands, module layout, file locations, coding standards
- Parameter documentation (already in tool schemas)
- Agent system documentation

**Complexity:** Small (content writing, not code)

---

### Phase 5: MCP Server Enhancements for Autonomous Mode

**Goal:** Small improvements to make the MCP server work better as the primary tool interface.

#### 5A: Tool progress notifications

**Files to modify:**
- `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp` -- enhance `HandleToolsCallAsync()`

Add timing info to completion notifications. The MCP server already sends `tools/progress` notifications with "started" and "completed"/"failed" status. Enhance with:
- Execution duration in milliseconds
- Brief result summary (first 200 chars of tool result)

#### 5B: Better error formatting in tool results

**Files to check:**
- Tool handlers already return structured `FOliveToolResult` with error codes, messages, and suggestions
- The MCP server wraps these in `{"content":[{"type":"text","text":"..."}]}` format
- Verify that `isError: true` is set correctly (it is -- line 648 in OliveMCPServer.cpp)

No changes likely needed here -- error formatting is already good.

**Complexity:** Small

---

### Phase 6: UI Integration for Autonomous Mode

**Goal:** The chat panel needs to show autonomous mode activity (tool calls happening via MCP).

**Files to modify:**
- `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp` -- handle autonomous progress chunks

In autonomous mode, `ParseOutputLine` emits progress chunks (e.g., "Calling blueprint.create..."). The chat panel already handles stream chunks via `OnStreamChunk`. No special handling needed -- the chunks just display as streaming text.

**Optional enhancement:** Subscribe to `FOliveMCPServer::OnToolCalled` to show real-time tool execution status in the operation feed, regardless of whether the call came from autonomous Claude Code or an external MCP client.

**Files to modify (optional):**
- `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp` -- bind to `FOliveMCPServer::OnToolCalled`

**Complexity:** Small

---

### Phase 7: Cost Controls

**Goal:** Prevent runaway costs in autonomous mode.

#### 7A: Max turns ceiling

Already handled in Phase 2A: `--max-turns 50` as safety cap (configurable via `AutonomousMaxTurns` setting).

#### 7B: Total runtime timeout

**Files to modify:**
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` -- add `AutonomousMaxRuntimeSeconds`
- `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` -- in `SendMessageAutonomous()`, add a separate timeout alongside the idle timeout

```cpp
/** Maximum total runtime for autonomous CLI mode (seconds). 0 = no limit. */
UPROPERTY(Config, EditAnywhere, Category="AI Provider",
    meta=(DisplayName="Autonomous Max Runtime (seconds)", ClampMin=0, ClampMax=1800))
int32 AutonomousMaxRuntimeSeconds = 300;
```

The existing `CLI_IDLE_TIMEOUT_SECONDS` (120s) handles hung processes. Add a total runtime check:
```cpp
if (TotalRuntime > MaxRuntimeSeconds)
{
    UE_LOG(..., TEXT("Autonomous process exceeded runtime limit (%d seconds)"), MaxRuntimeSeconds);
    bStopReading = true;
    FPlatformProcess::TerminateProc(ProcessHandle, true);
}
```

**Complexity:** Small

---

## 3. Task Breakdown for Coder

### Task 0: Tool Resilience Hardening (Phase 0.5)
**Scope:** Parameter aliasing, required param validation, type parsing, error messages
**Files:** `OliveToolRegistry.cpp`, `OliveBlueprintToolHandlers.cpp`, `OliveBTToolHandlers.cpp`, `OlivePCGToolHandlers.cpp`, `OliveCppToolHandlers.cpp`, `OliveCrossSystemToolHandlers.cpp`, `OliveGraphBatchExecutor.cpp`
**What:**
- **0A:** Generalize `NormalizeBlueprintParams()` into `NormalizeToolParams()` in `OliveToolRegistry.cpp`. Add per-family alias maps:
  - Blueprint: existing path aliases + `plan_json`←`plan`/`steps`, `function_name`←`name`, `parent_class`←`parent`/`base_class`, `template_id`←`template`/`id`
  - BT/Blackboard: `path`←`asset_path`/`asset`, `key_type`←`type`
  - PCG: `path`←`asset_path`/`asset`, `settings_class`←`type`/`node_type`/`class`
  - Cpp: `class_name`←`name`/`class`, `file_path`←`path`/`file`, `module_name`←`module`, `property_name`←`name`, `property_type`←`type`, `function_name`←`name`, `anchor_text`←`anchor`
  - Project: `asset_path`←`path`/`asset`, `paths`←`assets`/`asset_paths`
- **0B:** Replace all unguarded `GetStringField()` calls for required params in BT (~15), PCG (~10), Cpp (~15), CrossSystem (~5) handlers with `TryGetStringField()` + empty check + 3-part error return (code + message + suggestion)
- **0C:** Expand `ParseTypeFromParams()` alias map: add `str`/`fstring`→`string`, `fname`→`name`, `ftext`→`text`, `fvector`/`vec`/`vec3`→`vector`, `frotator`/`rot`→`rotator`, `ftransform`→`transform`, `flinearcolor`→`linear_color`, `fcolor`→`color`, `fvector2d`/`vec2`→`vector2d`, `float32`→`float`, `float64`→`double`, `uint8`→`byte`. Fix BT `ParseKeyType()` to add `TypeStr.ToLower()` at the top.
- **0D:** Add suggestion strings to all bare `Error()` calls in `OliveGraphBatchExecutor.cpp` (6 returns) and BT/PCG/Cpp handlers
**Dependencies:** None (must complete before autonomous mode testing)
**Complexity:** Medium-Large (many files, but each change is mechanical)

### Task 1: Feature Flag
**Scope:** Add `bUseAutonomousMCPMode` setting
**Files:** `OliveAISettings.h`
**What:** Add UPROPERTY bool in AI Provider section, default `true`
**Dependencies:** None
**Complexity:** Small

### Task 2: Dynamic .mcp.json
**Scope:** Write .mcp.json after MCP server starts, clean up on stop
**Files:** `OliveMCPServer.h`, `OliveMCPServer.cpp`
**What:**
- Add `WriteMcpConfigFile()` public method
- Call it at end of `Start()` after `State = Running`
- Write JSON with the bridge command format (preserves backward compat with existing `.mcp.json` format)
- Add `CleanupMcpConfigFile()` private method, call from `Stop()` -- deletes the file
- Plugin dir resolved same way as `FOliveClaudeCodeProvider` constructor
**Dependencies:** None
**Complexity:** Small

### Task 3: Autonomous CLI Arguments
**Scope:** Add `GetCLIArgumentsAutonomous()` to provider hierarchy
**Files:** `OliveCLIProviderBase.h`, `OliveClaudeCodeProvider.h`, `OliveClaudeCodeProvider.cpp`
**What:**
- Add `virtual FString GetCLIArgumentsAutonomous() const` to `FOliveCLIProviderBase` (default: same as GetCLIArguments with empty system prompt arg)
- Override in `FOliveClaudeCodeProvider` to return `--print --output-format stream-json --verbose --dangerously-skip-permissions --max-turns 50`
- NOTE: `--max-turns 50` is a safety ceiling, NOT orchestration. Each MCP tools/call counts as a turn. Complex multi-asset tasks easily need 40-60 calls.
**Dependencies:** None
**Complexity:** Small

### Task 4: Extract LaunchCLIProcess + SendMessageAutonomous
**Scope:** Refactor SendMessage to use shared helper, then add autonomous path
**Files:** `OliveCLIProviderBase.h`, `OliveCLIProviderBase.cpp`
**What — two-step refactor-then-extend:**

**Step 1: Extract `LaunchCLIProcess()` from `SendMessage()`**
- Extract the ~200 lines of process management infrastructure into a shared helper:
  ```cpp
  void LaunchCLIProcess(
      const FString& CLIArgs,
      const FString& StdinContent,       // empty = no stdin write
      TFunction<void(int32)> OnProcessExit  // called on game thread
  );
  ```
- Moves into helper: callback storage, generation counter, AliveGuard capture, background AsyncTask, GetExecutablePath, .js/.exe dispatch, pipe creation, CreateProc, stdin delivery, read loop with idle timeout, post-exit drain, GetProcReturnCode, CloseProc, completion dispatch
- Stays in `SendMessage()`: `BuildConversationPrompt()`, `BuildCLISystemPrompt()`, system prompt escaping, `GetCLIArguments(SystemPromptArg)`, passes `HandleResponseComplete` as exit handler
- **Verify existing orchestrated path still works before proceeding to Step 2**

**Step 2: Add `SendMessageAutonomous()`**
- Thin method that calls `LaunchCLIProcess()` with:
  - `GetCLIArgumentsAutonomous()` for CLI args
  - `UserMessage` as stdin content
  - `HandleResponseCompleteAutonomous()` as exit handler
- Add `HandleResponseCompleteAutonomous(int32 ReturnCode)`: emit `AccumulatedResponse` via `OnComplete`, no `<tool_call>` parsing
**Dependencies:** Task 3
**Complexity:** Large

### Task 5: ParseOutputLine Autonomous Events
**Scope:** Handle `tool_use` and `tool_result` stream events gracefully
**Files:** `OliveClaudeCodeProvider.cpp`
**What:**
- In `ParseOutputLine()`, change `tool_use`/`tool_call` handling from WARNING to informational:
  - Extract tool name from JSON
  - Emit progress chunk: "Calling {tool_name}..." (via `CurrentOnChunk`)
  - Log at Log level, not Warning
- Add `tool_result` handling:
  - Emit brief status chunk: "Tool completed"
  - Log at Verbose level
- These events are informational only -- the actual execution happens via MCP server
**Dependencies:** None
**Complexity:** Small

### Task 6: IOliveAIProvider Interface Extension
**Scope:** Add `SendMessageAutonomous` to provider interface
**Files:** `IOliveAIProvider.h`
**What:**
- Add virtual method with default implementation that errors
- Non-breaking -- all existing providers inherit the default
**Dependencies:** None
**Complexity:** Small

### Task 7: ConversationManager Autonomous Routing
**Scope:** Add autonomous path to ConversationManager with auto-snapshot safety net
**Files:** `OliveConversationManager.h`, `OliveConversationManager.cpp`
**What:**
- Add `bool IsAutonomousProvider() const` private method
  - Returns true when: provider is valid AND `GetProviderName() == "Claude Code CLI"` AND `UOliveAISettings::Get()->bUseAutonomousMCPMode`
- Add `void SendUserMessageAutonomous(const FString& Message)` private method:
  1. Add user message to history (for UI display)
  2. Set `bIsProcessing = true`, `OnProcessingStarted.Broadcast()`
  3. Ensure MCP server is running: `if (!FOliveMCPServer::Get().IsRunning()) FOliveMCPServer::Get().Start()`
  4. **Auto-snapshot:** `FOliveSnapshotManager::Get().CreateSnapshot()` with label "Pre-autonomous: {first 60 chars of message}". This is the safety net for bypassed Tier 3 confirmations — users get one-click rollback if the autonomous agent makes a mess.
  5. `Brain->BeginRun()`
  6. Set up callbacks with WeakSelf pattern:
     - OnChunk → `OnStreamChunk.Broadcast(Chunk.Text)`
     - OnComplete → `bIsProcessing = false`, add assistant message to history, `Brain->CompleteRun(Completed)`, `Brain->ResetToIdle()`, `OnProcessingComplete.Broadcast()`, apply deferred profile, drain queue
     - OnError → `bIsProcessing = false`, `Brain->CompleteRun(Failed)`, `Brain->ResetToIdle()`, `OnError.Broadcast()`, `OnProcessingComplete.Broadcast()`, drain queue
  7. Call `Provider->SendMessageAutonomous(Message, OnChunk, OnComplete, OnError)`
- Modify `SendUserMessage()` to check `IsAutonomousProvider()` early and branch
  - The busy check, queue check, and provider check should happen before the branch
  - The write intent detection, multi-asset intent, etc. are NOT needed for autonomous (agent manages itself)
**Dependencies:** Tasks 1, 4, 6
**Complexity:** Medium

### Task 8: AGENTS.md Rewrite
**Scope:** Replace stale AGENTS.md with focused tool-usage guide
**Files:** `AGENTS.md`
**What:**
- Complete rewrite to ~60-80 lines focused on what tool schemas CAN'T express (Claude Code already gets parameter docs from `tools/list`):
  1. Intro: "You have MCP tools for creating/modifying UE 5.5 assets"
  2. Tool category namespaces (brief — not parameter docs)
  3. **Multi-tool workflow sequences:** create → add_component → add_variable → apply_plan_json
  4. **Plan JSON format and rules:** step structure, exec_after chaining, pure vs impure nodes, event types (this is the #1 thing schemas can't express)
  5. **Asset path conventions:** `/Game/...` format
  6. **Gotchas and common mistakes:**
     - Read before modify
     - Preview before apply
     - Every impure node needs `exec_after`
     - Don't batch preview and apply in the same response
  7. Available factory templates (`blueprint.list_templates`)
- **Do NOT duplicate** what tool schemas already say (parameter names, descriptions)
- Remove all architecture docs, build commands, file locations, coding standards (those stay in CLAUDE.md)
**Dependencies:** None
**Complexity:** Small

### Task 9: Cost Control Settings
**Scope:** Add runtime limit setting for autonomous mode
**Files:** `OliveAISettings.h`, `OliveCLIProviderBase.cpp`
**What:**
- Add `AutonomousMaxRuntimeSeconds` UPROPERTY to settings (default 300, range 0-1800)
- Add `AutonomousMaxTurns` UPROPERTY to settings (default 50, range 1-200) -- used for `--max-turns` flag
- In `SendMessageAutonomous()` / `LaunchCLIProcess()`, read settings and:
  - Pass max turns to `GetCLIArgumentsAutonomous()`
  - Add total runtime check in the read loop (alongside existing idle timeout)
  - On timeout: log, terminate process, emit error via OnError
**Dependencies:** Task 4
**Complexity:** Small

### Task 10: MCP Server Tool Progress Enhancement (Optional)
**Scope:** Add timing and summary to MCP tool progress notifications
**Files:** `OliveMCPServer.cpp`
**What:**
- In `HandleToolsCallAsync()`, capture start time before tool execution
- In completion notification, add `"duration_ms"` field
- Add first 200 chars of tool result text to completion notification params
**Dependencies:** None
**Complexity:** Small

---

## 4. Implementation Order

```
Phase 0.5: Task 0 (tool resilience hardening) -- FIRST, no dependencies
    |
Phase 0:   Task 1 (settings flag) -- parallel with T0
    |
Phase 1:   Task 2 (dynamic .mcp.json) -- parallel with T0/T1
    |      Task 3 (autonomous CLI args) -- parallel
    |      Task 5 (ParseOutputLine update) -- parallel
    |      Task 6 (interface extension) -- parallel
    |      Task 8 (AGENTS.md) -- parallel
    |
    v
Phase 2:   Task 4 (LaunchCLIProcess extract + SendMessageAutonomous) -- depends on Task 3
    |
Phase 3:   Task 7 (ConversationManager routing + auto-snapshot) -- depends on Tasks 1, 4, 6
    |
Phase 4:   Task 9 (cost controls) -- depends on Task 4
    |
Phase 5:   Task 10 (MCP progress) -- independent, do last
```

**Parallelism:** Tasks 0, 1, 2, 3, 5, 6, 8 can all run in the first batch. Task 4 follows Task 3. Task 7 follows Tasks 1, 4, 6. Task 9 follows Task 4.

**Critical path:** T0 → T3 → T4 → T7 → T9 (tool resilience must be solid before the plumbing connects).

---

## 5. Cleanup List (Post-Migration Dead Code)

These components become dead code for the CLI path but MUST be retained for the API provider path:

| Component | Status | Rationale |
|-----------|--------|-----------|
| `FOliveCLIToolCallParser` | **KEEP** | Used by `HandleResponseComplete()` in orchestrated path. Still needed if `bUseAutonomousMCPMode = false`. |
| `FOliveCLIToolSchemaSerializer` | **KEEP** | Used by `BuildCLISystemPrompt()` in orchestrated path. Still needed if autonomous mode is off. |
| `BuildConversationPrompt()` | **KEEP** | Orchestrated path still uses it. |
| `BuildCLISystemPrompt()` | **KEEP** | Orchestrated path still uses it. |
| `FOlivePromptDistiller` | **KEEP** | Used by `SendToProvider()` for API providers AND orchestrated CLI. |
| `FOliveSelfCorrectionPolicy` | **KEEP** | Used by `HandleToolResult()` in orchestrated path. |
| `FOliveLoopDetector` | **KEEP** | Used alongside SelfCorrectionPolicy. |
| `mcp-bridge.js` | **KEEP** | Still the transport bridge. Could be removed if Claude Code gains native HTTP MCP support AND we verify it works. |
| Iteration budget injection | **KEEP** | Used in `SendToProvider()` for orchestrated path. |
| Zero-tool re-prompt logic | **KEEP** | Orchestrated path. |
| Correction directives | **KEEP** | Orchestrated path. |

**Nothing gets deleted in this migration.** The autonomous path is purely additive. Dead code cleanup happens in a future phase once autonomous mode is stable and orchestrated CLI is deprecated.

The only file that gets REWRITTEN (not deleted) is `AGENTS.md`.

---

## 6. Risk Assessment

| Risk | Impact | Probability | Mitigation |
|------|--------|------------|------------|
| Claude Code MCP discovery fails | High | Low | Bridge already works; `.mcp.json` already exists; `--strict-mcp-config` removal is the only change needed |
| Claude Code exits prematurely | Medium | Medium | `--max-turns 50` gives budget; idle timeout kills hung processes; user can retry |
| Uncapped token costs | High | Low | `--max-turns 50` + runtime timeout (300s) + existing idle timeout (120s) |
| MCP server not running when Claude starts | High | Low | `SendUserMessageAutonomous()` ensures server is running before launch; `bAutoStartMCPServer = true` default |
| Port mismatch (.mcp.json stale) | Medium | Low | Bridge auto-discovers ports 3000-3009; dynamic .mcp.json written on each Start() |
| Existing orchestrated path breaks | High | Very Low | Zero changes to orchestrated path code; `bUseAutonomousMCPMode = false` restores old behavior |
| Tool results don't show in UI | Medium | Medium | Progress chunks from ParseOutputLine show tool activity; MCP server notifications available for richer UI |
| Confirmation flow doesn't work | Medium | Medium | MCP path (`Origin::MCP`) skips confirmation tiers 2-3. Accepted: agent is trusted, and auto-snapshot before autonomous runs provides one-click rollback. MCP-level confirmation is future work if needed. |
| Tool errors cause turn-burning | Medium | Medium | **Mitigated by Task 0** (tool resilience hardening). Parameter aliasing, validated inputs, and actionable error messages prevent the most common autonomous failure modes. |
| Context window management | Low | Medium | Claude Code manages its own context window. If it loses track, it can re-read assets. The `AGENTS.md` tells it to "read before modify." |

### Fallback Plan

1. User sets `bUseAutonomousMCPMode = false` in settings
2. Entire autonomous path is bypassed
3. Legacy orchestrated path works exactly as before
4. No code deletion means no irreversible changes

---

## 7. Testing Strategy

### Manual Test Cases

1. **Basic autonomous execution:** Send "Create a BP_TestActor with a health variable" via chat panel with autonomous mode ON. Verify:
   - Claude Code discovers tools via MCP
   - Blueprint is created
   - Variable is added
   - Process exits cleanly
   - UI shows streaming progress

2. **Fallback to orchestrated:** Set `bUseAutonomousMCPMode = false`, send same message. Verify legacy path still works.

3. **MCP server not running:** Stop MCP server manually, send message. Verify it auto-starts.

4. **Runtime timeout:** Set `AutonomousMaxRuntimeSeconds = 10`, send a complex task. Verify process terminates after 10s.

5. **Port discovery:** Change MCPServerPort to 3005, restart editor. Verify `.mcp.json` updates and Claude Code finds it.

6. **Cancel mid-execution:** Send a complex task, cancel while Claude is running. Verify process terminates cleanly.

7. **Queue behavior:** Send two messages rapidly while autonomous mode is processing. Verify second message queues and executes after first completes.

---

## 8. Architecture Diagram (After Migration)

```
                    +-----------------------+
                    |   Chat Panel (UI)     |
                    +-----------+-----------+
                                |
                    +-----------v-----------+
                    | ConversationManager   |
                    |   IsAutonomous()?     |
                    +----+------------+-----+
                         |            |
                   YES   |            |  NO (API/legacy CLI)
                         v            v
              +----------+--+  +------+--------+
              | Autonomous  |  | Orchestrated   |
              | Path        |  | Path           |
              +------+------+  +---+------+-----+
                     |             |      |
           stdin:    |    Messages+Tools  | <tool_call> XML
           "make X"  |             |      | parse + execute
                     v             v      v
              +------+------+  +--+------+--+
              | Claude Code |  | Provider    |
              | (long-lived)|  | (per-turn)  |
              +------+------+  +------+------+
                     |                |
              MCP    |                | OnToolCall
              calls  |                |
                     v                v
              +------+------+  +------+------+
              | MCP Server  |  | ToolRegistry|
              | HandleTools |  | ExecuteTool |
              | CallAsync() |  +------+------+
              +------+------+         |
                     |                |
                     v                v
              +------+----------------+------+
              |      FOliveToolRegistry      |
              |      .ExecuteTool()           |
              +------------------------------+
              |  Blueprint | BT | PCG | C++  |
              +------------------------------+
```

Both paths converge on the same `FOliveToolRegistry.ExecuteTool()`. The tool handlers, write pipeline, validation engine, and all subsystems are shared.

---

## 9. Implementation Status

**All 11 tasks completed and verified.** Final build: clean (0 errors).

| Task | Status | Summary |
|------|--------|---------|
| T0 | **DONE** | `NormalizeToolParams()` with per-family aliases, ~55 param validations across BT/PCG/Cpp/CrossSystem, 16 new type aliases, case-insensitive BT ParseKeyType, error suggestions in batch executor |
| T1 | **DONE** | `bUseAutonomousMCPMode = true` in `UOliveAISettings` |
| T2 | **DONE** | `WriteMcpConfigFile()` / `CleanupMcpConfigFile()` on `FOliveMCPServer` |
| T3 | **DONE** | `GetCLIArgumentsAutonomous()` virtual on base, override in ClaudeCode provider |
| T4 | **DONE** | `LaunchCLIProcess()` shared helper extracted (~250 lines), `SendMessageAutonomous()` + `HandleResponseCompleteAutonomous()` added |
| T5 | **DONE** | `tool_use` → progress chunk + Log level, `tool_result` → brief status + Verbose level |
| T6 | **DONE** | `IOliveAIProvider::SendMessageAutonomous()` with default error impl |
| T7 | **DONE** | `IsAutonomousProvider()`, `SendUserMessageAutonomous()` with auto-snapshot, routing branch in `SendUserMessage()` |
| T8 | **DONE** | AGENTS.md rewritten: 77 lines, workflow-focused, Plan JSON rules, no schema duplication |
| T9 | **DONE** | `AutonomousMaxRuntimeSeconds = 300`, `AutonomousMaxTurns = 50`, runtime timeout in `LaunchCLIProcess` read loop |
| T10 | **DONE** | `duration_ms` + `summary` fields in MCP tools/progress completion notifications |

### Files Modified (complete list)

**New behavior (autonomous path):**
- `OliveAISettings.h` — 3 new settings
- `IOliveAIProvider.h` — `SendMessageAutonomous()` interface method
- `OliveCLIProviderBase.h` — `LaunchCLIProcess()`, `SendMessageAutonomous()`, `HandleResponseCompleteAutonomous()`
- `OliveCLIProviderBase.cpp` — `LaunchCLIProcess()` extracted, `SendMessage()` refactored to use it, `SendMessageAutonomous()`, `HandleResponseCompleteAutonomous()`
- `OliveClaudeCodeProvider.h` — `GetCLIArgumentsAutonomous()` override
- `OliveClaudeCodeProvider.cpp` — `GetCLIArgumentsAutonomous()` impl, `ParseOutputLine()` tool_use/tool_result handling
- `OliveConversationManager.h` — `IsAutonomousProvider()`, `SendUserMessageAutonomous()`
- `OliveConversationManager.cpp` — autonomous routing branch + full implementation
- `OliveMCPServer.h` — `WriteMcpConfigFile()`, `CleanupMcpConfigFile()`
- `OliveMCPServer.cpp` — dynamic .mcp.json, tool progress timing/summary
- `AGENTS.md` — complete rewrite

**Tool resilience (T0):**
- `OliveToolRegistry.cpp` — `NormalizeToolParams()` with per-family aliases
- `OliveBlueprintToolHandlers.cpp` — expanded `ParseTypeFromParams()` aliases
- `OliveBTToolHandlers.h` + `.cpp` — param validation, `ParseKeyType()` case-insensitive, error suggestions
- `OlivePCGToolHandlers.cpp` — param validation, error suggestions
- `OliveCppToolHandlers.cpp` — param validation, error suggestions
- `OliveCrossSystemToolHandlers.cpp` — param validation, error suggestions
- `OliveGraphBatchExecutor.cpp` — error suggestions for all 6 batch ops
