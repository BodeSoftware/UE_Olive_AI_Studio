# Olive AI → NeoStack-Style Architecture: Migration Plan

## Current Architecture vs. Target

### What You Have Now (CLI Path)

```
User Message
    ↓
ConversationManager.SendUserMessage()
    ↓
ConversationManager.SendToProvider()
    ↓ builds system prompt + tool schemas as TEXT
    ↓ injects iteration budgets, correction directives
    ↓ formats conversation as [User]/[Assistant]/[Tool Result] blocks
    ↓
CLIProviderBase.SendMessage()
    ↓ escapes system prompt, passes as --append-system-prompt "..."
    ↓ writes conversation prompt to stdin
    ↓ launches: node cli.js --print --output-format stream-json 
    ↓   --verbose --dangerously-skip-permissions
    ↓   --max-turns 1 --strict-mcp-config
    ↓
Claude Code (constrained to 1 turn, no MCP discovery)
    ↓ reads stdin prompt + system prompt
    ↓ outputs text with <tool_call> XML blocks
    ↓
CLIProviderBase.HandleResponseComplete()
    ↓ FOliveCLIToolCallParser::Parse() extracts <tool_call> blocks
    ↓
ConversationManager.HandleComplete()
    ↓ if tool calls found → ProcessPendingToolCalls()
    ↓   → ExecuteToolCall() via FOliveToolRegistry
    ↓   → HandleToolResult() → ContinueAfterToolResults()
    ↓   → loop back to SendToProvider()
    ↓ if text-only → re-prompt or finish
```

**Key constraints in this design:**
- `--max-turns 1`: Claude Code does ONE completion, exits
- `--strict-mcp-config`: Claude Code CANNOT discover your MCP tools
- Tool schemas serialized as text in system prompt (CLIToolSchemaSerializer)
- Tool calls parsed from `<tool_call>` XML in text output (CLIToolCallParser)
- ConversationManager owns the loop: re-launches Claude Code for each iteration
- Each iteration = new process spawn, new stdin delivery, new stdout parsing

### What NeoStack Does

```
User Message (from Claude Code / Gemini CLI terminal)
    ↓
Agent (Claude Code) discovers MCP server via .mcp.json
    ↓ MCP handshake: initialize → tools/list
    ↓ Agent sees all available tools with native JSON schemas
    ↓
Agent reasons about the task
    ↓ calls tools via MCP natively (tools/call)
    ↓ gets results back
    ↓ reasons about results
    ↓ calls more tools
    ↓ ... (unbounded loop, agent decides when done)
    ↓
Plugin MCP Server handles each tools/call
    ↓ FOliveToolRegistry.ExecuteTool()
    ↓ returns result
```

**Key differences:**
- NO process re-launching per turn
- NO text-based tool schema serialization
- NO `<tool_call>` XML parsing
- NO ConversationManager loop control
- Agent uses MCP protocol natively
- Agent decides how many turns to take
- Plugin is just a tool server

## The Gap: What Needs to Change

### Things That Already Work ✅

1. **OliveMCPServer** — Working HTTP-based MCP server with JSON-RPC 2.0, full MCP handshake, tools/list, tools/call, resources, prompts, client tracking, async tool execution, protocol version 2024-11-05.

2. **OliveToolRegistry** — All tools registered and executable via `ExecuteTool(ToolName, Arguments)`. Both MCP and ConversationManager use the same registry.

3. **All Tool Handlers** — Blueprint, BT, PCG, C++ handlers all work through the registry.

### Things That Need to Change ❌

#### 1. Claude Code Launch Arguments

**Current** (`GetCLIArguments`):
```
--print --output-format stream-json --verbose 
--dangerously-skip-permissions --max-turns 1 
--strict-mcp-config --append-system-prompt "..."
```

**Target:**
```
--print --output-format stream-json --verbose 
--dangerously-skip-permissions
```

Remove `--max-turns 1`, `--strict-mcp-config`, `--append-system-prompt`. Claude Code runs autonomously, discovers tools via MCP, manages its own loop.

#### 2. MCP Server Discovery

Claude Code needs to find your MCP server. Two options:

**Option A: `.mcp.json` in working directory** (recommended)
```json
{
  "mcpServers": {
    "olive-ai-studio": {
      "type": "http",
      "url": "http://localhost:3000/mcp"
    }
  }
}
```

Since you currently use `--strict-mcp-config` to PREVENT this, just remove that flag.

**Option B: Pass MCP config via CLI args**
```
--mcp-config '{"olive-ai-studio":{"type":"http","url":"http://localhost:3000/mcp"}}'
```

Option A is simpler. You already set `WorkingDirectory` to the plugin directory.

**Dynamic port issue:** MCP server tries ports 3000-3009. Write `.mcp.json` after successful bind with actual port.

#### 3. Process Lifecycle

**Current:** One process per "turn". ConversationManager spawns Claude Code → one completion → exits → re-spawns.

**Target:** One process per user request. Claude Code stays alive, makes MCP calls, exits when done. Your existing `ReadPipe` loop already handles long-running processes with idle timeout.

#### 4. Prompt Delivery

**Current:** System prompt stuffed into `--append-system-prompt` with tool schemas, format instructions, iteration budgets. User message via stdin with conversation history.

**Target:** User message piped via stdin (or `-p`). No tool schemas (discovered via MCP). Domain knowledge via `AGENTS.md` in working directory (Claude Code reads automatically).

#### 5. Output Parsing

In autonomous mode, Claude Code's stream-json includes MCP tool interaction events:
```json
{"type": "tool_use", "name": "blueprint.create", "input": {...}}
{"type": "tool_result", "content": "..."}
{"type": "assistant", "message": {"content": [{"type": "text", "text": "..."}]}}
{"type": "result", ...}
```

Your `ParseOutputLine` already handles `assistant` and `result`. Just add informational handling for `tool_use`/`tool_result` to show progress in the chat UI.

## What Gets Deleted vs. Kept

### Bypassed for CLI (kept for future API path)

| Component | Current Role | NeoStack CLI Path |
|-----------|-------------|-------------------|
| `CLIToolSchemaSerializer` | Serializes tool schemas into text | Not needed — MCP tools/list |
| `CLIToolCallParser` | Parses `<tool_call>` XML from output | Not needed — tools called via MCP |
| `BuildConversationPrompt()` | Formats history with tool results | Not needed — Claude manages context |
| `BuildCLISystemPrompt()` | Assembles knowledge + schemas + format | Replaced by AGENTS.md + MCP prompts |
| Iteration budget injection | Controls cost | Removed (use --max-turns N for ceiling) |
| Correction directives | Forces retry after failures | Not needed — agent self-corrects |
| Zero-tool re-prompts | Forces tool use on text-only | Not needed — agent drives itself |
| `PromptDistiller` | Summarizes old messages | Not needed — Claude manages context |

### Stays

| Component | Why |
|-----------|-----|
| `OliveMCPServer` | Core of new architecture |
| `OliveToolRegistry` | Same execution path |
| All tool handlers | Same via MCP |
| `OliveTransactionManager` | Undo/redo at tool level |
| `OliveSnapshotManager` | Before/after snapshots |
| `OliveProjectIndex` | Exposed via MCP resources |
| `ConversationManager` (API) | Reused for API providers later |
| Chat UI | Shows Claude's streaming output |
| Brain Layer | Tracks state (Idle → Working → Complete) |

## Implementation Plan

### Phase 1: MCP Discovery (Day 1)

**1a. Dynamic `.mcp.json` after server start**

```cpp
// Add to FOliveMCPServer::Start(), after successful bind:
void FOliveMCPServer::WriteMcpConfigFile(int32 Port)
{
    FString PluginDir = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
    FString ConfigPath = FPaths::Combine(PluginDir, TEXT(".mcp.json"));
    
    FString Content = FString::Printf(
        TEXT("{\n  \"mcpServers\": {\n    \"olive-ai-studio\": {\n"
             "      \"type\": \"http\",\n"
             "      \"url\": \"http://localhost:%d/mcp\"\n"
             "    }\n  }\n}"), Port);
    
    FFileHelper::SaveStringToFile(Content, *ConfigPath);
    UE_LOG(LogOliveAI, Log, TEXT("Wrote .mcp.json with port %d"), Port);
}
```

**1b. Ensure MCP server auto-starts on editor load**

Verify this in `OliveAIEditorModule::StartupModule()`.

### Phase 2: Autonomous Launch Path (Day 1-2)

**2a. Add autonomous CLI arguments**

```cpp
FString FOliveClaudeCodeProvider::GetCLIArgumentsAutonomous() const
{
    return TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions");
    // NO --max-turns, NO --strict-mcp-config, NO --append-system-prompt
}
```

**2b. New SendMessageAutonomous in CLIProviderBase**

The key insight: your existing `SendMessage` infrastructure (process creation, pipe management, ReadPipe loop, ParseOutputLine) mostly works. The changes are:

1. Don't build system prompt or conversation history
2. Don't serialize tool schemas
3. Just pipe the user's message as stdin
4. Let the process run until it exits naturally

```cpp
void FOliveCLIProviderBase::SendMessageAutonomous(
    const FString& UserPrompt,
    FOnOliveStreamChunk OnChunk,
    FOnOliveComplete OnComplete,
    FOnOliveError OnError)
{
    // Reuse existing process infrastructure
    // But: simpler prompt (just user message), different CLI args, no tool parsing
    
    bIsBusy = true;
    AccumulatedResponse.Empty();
    
    // Store callbacks...
    // Spawn process with GetCLIArgumentsAutonomous()...
    // Write UserPrompt to stdin...
    // Existing ReadPipe loop handles the rest
    // Process exits when Claude Code finishes → HandleResponseComplete()
}
```

**2c. Update ParseOutputLine for autonomous events**

```cpp
// In FOliveClaudeCodeProvider::ParseOutputLine:
else if (Type == TEXT("tool_use"))
{
    FString ToolName;
    JsonObject->TryGetStringField(TEXT("name"), ToolName);
    UE_LOG(LogOliveClaudeCode, Log, TEXT("Agent calling tool: %s"), *ToolName);
    
    // Show in chat UI
    FOliveStreamChunk Chunk;
    Chunk.Text = FString::Printf(TEXT("🔧 Calling %s..."), *ToolName);
    CurrentOnChunk.ExecuteIfBound(Chunk);
}
else if (Type == TEXT("tool_result"))
{
    // Tool completed via MCP — UI can show status
    UE_LOG(LogOliveClaudeCode, Log, TEXT("Tool result received"));
}
```

### Phase 3: ConversationManager Routing (Day 2-3)

```cpp
void FOliveConversationManager::SendUserMessage(const FString& Message)
{
    // ... existing setup (write intent detection, etc.) ...
    
    if (IsAutonomousProvider())
    {
        SendUserMessageAutonomous(Message);
    }
    else
    {
        // Existing orchestrated path
        AddMessage(UserMsg);
        SendToProvider();
    }
}

bool FOliveConversationManager::IsAutonomousProvider() const
{
    // For now: Claude Code CLI = autonomous
    // Later: API providers = orchestrated
    return Provider.IsValid() && 
           Provider->GetProviderName() == TEXT("claudecode");
}

void FOliveConversationManager::SendUserMessageAutonomous(const FString& Message)
{
    bIsProcessing = true;
    OnProcessingStarted.Broadcast();
    
    // Ensure MCP server is running
    if (FOliveMCPServer::Get().GetState() != EOliveMCPServerState::Running)
    {
        FOliveMCPServer::Get().Start();
    }
    
    // Brain: mark as working
    if (Brain.IsValid())
    {
        Brain->BeginRun(Message);
    }
    
    TWeakPtr<FOliveConversationManager> WeakSelf = AsShared();
    
    Provider->SendMessageAutonomous(
        Message,
        // OnChunk: stream to UI
        FOnOliveStreamChunk::CreateLambda([WeakSelf](const FOliveStreamChunk& Chunk) {
            if (auto This = WeakSelf.Pin())
                This->OnStreamChunk.Broadcast(Chunk.Text);
        }),
        // OnComplete: process exited
        FOnOliveComplete::CreateLambda([WeakSelf](const FString& Response, const FOliveProviderUsage& Usage) {
            if (auto This = WeakSelf.Pin())
            {
                This->bIsProcessing = false;
                if (This->Brain.IsValid())
                    This->Brain->CompleteRun(EOliveRunOutcome::Completed);
                This->OnProcessingComplete.Broadcast();
            }
        }),
        // OnError
        FOnOliveError::CreateLambda([WeakSelf](const FString& Error) {
            if (auto This = WeakSelf.Pin())
            {
                This->bIsProcessing = false;
                This->HandleError(Error);
            }
        })
    );
}
```

### Phase 4: AGENTS.md (Day 2)

Restructure to ~60 lines of focused domain knowledge:

```markdown
# Olive AI Studio — UE 5.5 Blueprint Agent

You have MCP tools for creating/modifying Unreal Engine assets.

## Tool Categories
- `blueprint.*` — Blueprints and their graphs
- `behaviortree.*` — Behavior Trees  
- `pcg.*` — PCG graphs
- `cpp.*` — C++ source files
- `project.*` — Asset search, project structure

## Blueprint Creation Pattern
1. `blueprint.create` with parent_class and asset_path
2. `blueprint.add_component` for each component needed
3. `blueprint.add_variable` for each variable
4. `blueprint.apply_plan_json` for EventGraph logic

## Plan JSON Format
```json
{
  "steps": [
    {"step_id": "evt", "op": "event", "target": "BeginPlay"},
    {"step_id": "print", "op": "call", "target": "PrintString",
     "exec_after": "evt", "inputs": {"InString": "Hello"}}
  ]
}
```

## Asset Paths
Always use `/Game/...` format: `/Game/Blueprints/BP_MyActor`

## Tips  
- Read before modifying: `blueprint.read` shows current state
- Preview before applying: `blueprint.preview_plan_json`
- Every impure node needs `exec_after` chaining
- Both `asset_path` and `path` work for asset paths
- Both `plan_json` and `plan` work for plan objects
```

### Phase 5: Tool Resilience (Day 3-4)

Critical because there are no correction directives in autonomous mode:

1. **Parameter aliasing** in handlers (~5 lines each)
2. **Type parsing** improvements in `ParseTypeFromParams`
3. **Actionable error messages** with exact fix instructions
4. **exec_after auto-chaining** in PlanExecutor

### Phase 6: Cost Controls (Day 4-5)

Start with `--max-turns 30` and existing idle timeout. Add total runtime limit:

```cpp
constexpr double CLI_MAX_RUNTIME_SECONDS = 300.0; // 5 minutes
```

## Risk Mitigation

| Risk | Mitigation |
|------|-----------|
| Claude Code MCP support issues | Keep existing path as fallback behind feature flag |
| Uncapped costs | `--max-turns 30` + runtime timeout |
| Port discovery fails | Dynamic `.mcp.json` + startup validation |
| Existing API path breaks | Zero changes to existing path — autonomous is additive |
| Tool errors without correction directives | Better error messages are the fix |
| Output format changes | Stream-json parser is already robust |

## Why This Makes API Providers Easy Later

Once CLI autonomous works:
1. Tools are battle-tested against unconstrained agent
2. MCP server is production-hardened  
3. Error messages are self-documenting
4. AGENTS.md is tight and effective

Adding API providers = ConversationManager's existing orchestrated path + same tools via ToolRegistry + AGENTS.md content as system prompt. The tools don't care who's calling them.
