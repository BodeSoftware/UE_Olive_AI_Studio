# Unified Provider Architecture: CLI Agents as Completion Endpoints

> **Status:** Proposal  
> **Author:** AI Architecture Review  
> **Date:** 2026-02-23  
> **Scope:** `FOliveClaudeCodeProvider`, `FOliveConversationManager`, `IOliveAIProvider`, Brain Layer integration  
> **Goal:** Make CLI agents (Claude Code, Codex, future) use the same execution path as API providers so all Brain Layer infrastructure works universally.

---

## 1. Problem Statement

### 1.1 Two Incompatible Execution Paths

Olive AI Studio has two fundamentally different execution architectures hiding behind the same `IOliveAIProvider` interface:

**API Providers** (Anthropic, OpenAI, OpenRouter, Google, Ollama, OpenAI-Compatible):
```
ConversationManager::SendToProvider()
  → Provider returns tool_use blocks via OnToolCall
  → ConversationManager::ProcessPendingToolCalls()
  → ConversationManager::ExecuteToolCall() via ToolRegistry
  → SelfCorrectionPolicy evaluates result
  → LoopDetector checks for retry spirals
  → PromptDistiller manages context growth
  → ContinueAfterToolResults() sends results back to provider
  → Repeat until finish_reason == "stop"
```

**Claude Code CLI** (`FOliveClaudeCodeProvider`):
```
ClaudeCodeProvider::SendMessage()
  → Spawns `claude --print --mcp-config ...`
  → Claude Code loads its OWN system prompt
  → Claude Code discovers ALL 95 tools via MCP tools/list
  → Claude Code runs its OWN internal agentic loop
  → Claude Code calls tools directly through mcp-bridge.js → MCP Server
  → ConversationManager only sees streamed text output
  → HandleComplete() called once when process exits
```

The comment in `ParseOutputLine` makes this explicit:

```cpp
// Claude Code CLI already executes MCP tools internally.
// Do NOT forward tool calls into Olive's provider-agnostic tool loop,
// otherwise operations execute twice and responses repeat.
```

### 1.2 Consequences

The following Brain Layer systems are **dead code** when using Claude Code CLI:

| System | Class | What It Does | Active on API Path | Active on Claude Code |
|--------|-------|-------------|-------------------|---------------------|
| Self-Correction | `FOliveSelfCorrectionPolicy` | Evaluates tool results, decides Continue/FeedBack/Stop | ✅ | ❌ |
| Loop Detection | `FOliveLoopDetector` | Catches repeated failures, oscillation patterns | ✅ | ❌ |
| Tool Pack Filtering | `FOliveToolPackManager` | Limits visible tools by intent (read/write/danger) | ✅ | ❌ |
| Prompt Distillation | `FOlivePromptDistiller` | Summarizes older tool results to save tokens | ✅ | ❌ |
| Operation History | `FOliveOperationHistoryStore` | Records tool calls with sequence, params, results | ✅ | ❌ |
| Confirmation Gate | `ConversationManager::ConfirmPendingOperation` | Pauses for user approval on destructive ops | ✅ | ❌ |
| Worker Phase Tracking | `FOliveBrainLayer::SetWorkerPhase` | Streaming → ExecutingTools → SelfCorrecting | ✅ | ❌ |

Claude Code also ignores:
- `ToolPackManager` pack filtering — it gets all 95 tools from `MCP tools/list`
- Your `--append-system-prompt` competes with Claude Code's own built-in system prompt (which covers file editing, bash, permissions — nothing about UE5)
- Error recovery is Claude Code's internal logic, not your `SelfCorrectionPolicy`

### 1.3 Design Quality Impact

Claude Code simultaneously designs AND executes because it runs its own loop:

1. **95 tool schemas flood the context** — Claude Code loads all of them from MCP, not the 30 filtered by `ToolPackManager`
2. **Step-by-step execution breaks design coherence** — each tool call is a round-trip inside Claude Code's loop; the model re-evaluates after each result, losing the holistic view
3. **Claude Code's own system prompt** eats context — it's designed for coding tasks (file editing, terminal), not UE5 Blueprints
4. **Error recovery degrades design** — Claude Code hits a pin-wiring failure, enters tactical fix mode (`connect_pins`, `add_node`), and the clean plan deteriorates into "whatever compiles"

A normal API call with the same model produces better Blueprint designs because the model's full attention is on design, not tool-calling mechanics.

### 1.4 Extensibility Problem

Adding a new CLI agent (OpenAI Codex, future agents) would require either:
- **Duplicating the bypass pattern** — another provider that ignores Brain Layer, bypasses tool packs, skips self-correction
- **Building agent-specific adapters** — custom proxy servers, MCP interception layers, timeout management

Neither scales. Every new CLI agent means more dead infrastructure and more divergent behavior.

---

## 2. Solution: CLI Agents as Completion Endpoints

### 2.1 Core Principle

**Stop treating CLI agents as agents. Treat them as completion endpoints.**

Every provider — API or CLI — becomes a dumb text generator that takes messages in and returns text + tool calls out. `ConversationManager` owns the agentic loop for ALL providers. There is no second loop.

```
                 ┌──────────────────────────────────┐
                 │       IOliveAIProvider interface   │
                 │  SendMessage(messages, tools,      │
                 │   OnChunk, OnToolCall, OnComplete) │
                 └────────────┬──────────────┬────────┘
                              │              │
                    API providers       CLI providers
                 (Anthropic, OpenAI,  (Claude Code, Codex,
                  OpenRouter, Google,  future agents)
                  Ollama, Compatible)       │
                              │              │
                     Direct HTTP API   Spawn CLI with:
                     tool schemas in   - NO MCP config
                     native format     - Single-turn (--max-turns 1)
                              │        - Tools described in prompt text
                              │        - Returns pure text
                              │              │
                              │         Parse tool calls from
                              │         text response into
                              │         FOliveStreamChunk structs
                              │              │
                              ▼              ▼
                 ┌──────────────────────────────────┐
                 │       ConversationManager          │
                 │  ┌─────────────────────────────┐  │
                 │  │ ProcessPendingToolCalls()     │  │
                 │  │ ExecuteToolCall() via Registry│  │
                 │  │ SelfCorrectionPolicy.Evaluate │  │
                 │  │ LoopDetector.RecordAttempt    │  │
                 │  │ PromptDistiller.Distill       │  │
                 │  │ OperationHistory.Record       │  │
                 │  │ ContinueAfterToolResults()   │  │
                 │  └─────────────────────────────┘  │
                 │          ↓ calls SendToProvider()  │
                 │          ↓ with tool results in    │
                 │          ↓ message history          │
                 │          ↓ (loops until stop)       │
                 └──────────────────────────────────┘
```

### 2.2 How CLI Providers Change

CLI providers spawn the agent with **no MCP server** and **single-turn mode**. The agent generates one completion and exits. Tool definitions are included as structured text in the system prompt. The provider parses tool calls from the text response and emits them through the standard `OnToolCall` delegate.

**Before (current Claude Code):**
```
claude --print --output-format stream-json --verbose \
  --dangerously-skip-permissions \
  --add-dir "/path/to/project" \
  --mcp-config "/path/.mcp.json" \       ← gives Claude Code all 95 tools
  --strict-mcp-config \                  ← Claude Code runs its own loop
  --append-system-prompt "..." \
  -p "user request"
```

**After (unified):**
```
claude --print --output-format stream-json --verbose \
  --dangerously-skip-permissions \
  --max-turns 1 \                        ← single completion, no internal loop
  --append-system-prompt "..." \         ← includes tool definitions as text
  -p "user request + tool call format"
```

No `--mcp-config`. No `--strict-mcp-config`. Claude Code has no tools to call — it outputs text containing structured tool-call blocks that your provider parses.

### 2.3 Tool Call Text Protocol

All CLI providers share a common text format for expressing tool calls. The model is instructed to output tool calls using XML-delimited blocks:

```
I'll create the gun blueprint and add the necessary components.

<tool_call id="tc_1">
{"name": "blueprint.create", "arguments": {"path": "/Game/Blueprints/BP_Gun", "parent_class": "Actor"}}
</tool_call>

<tool_call id="tc_2">
{"name": "blueprint.add_component", "arguments": {"blueprint_path": "/Game/Blueprints/BP_Gun", "component_class": "StaticMeshComponent", "component_name": "GunMesh"}}
</tool_call>

<tool_call id="tc_3">
{"name": "blueprint.add_variable", "arguments": {"blueprint_path": "/Game/Blueprints/BP_Gun", "variable_name": "FireRate", "variable_type": "Float", "default_value": "0.2"}}
</tool_call>
```

The provider parses these into `FOliveStreamChunk` structs — identical to what API providers produce from native tool_use blocks. `ConversationManager` can't tell the difference.

For the next iteration (after tool results), `ConversationManager` calls `SendToProvider()` again. The CLI provider spawns a **new** `claude --max-turns 1` process with the full conversation history (including tool results as messages). Each round is a fresh single-turn completion.

### 2.4 Tool Results in Conversation History

When `ConversationManager` calls `ContinueAfterToolResults()` → `SendToProvider()`, the message history now contains tool result messages. The CLI provider formats these into the prompt:

```
[Previous assistant response with tool calls]

[Tool Results]
tool_call tc_1 (blueprint.create): {"success": true, "data": {"asset_path": "/Game/Blueprints/BP_Gun", ...}}
tool_call tc_2 (blueprint.add_component): {"success": true, "data": {"component_name": "GunMesh", ...}}
tool_call tc_3 (blueprint.add_variable): {"success": true, "data": {"variable_name": "FireRate", ...}}

Continue with the next steps. Use <tool_call> blocks for any tools you need.
```

The model sees its previous tool calls and their results, then decides what to do next — exactly like an API provider's multi-turn loop.

---

## 3. Implementation Details

### 3.1 New Shared Utility: `FOliveCLIToolCallParser`

A shared parser that all CLI providers use to extract tool calls from text responses:

**File:** `Source/OliveAIEditor/Public/Providers/OliveCLIToolCallParser.h`

```cpp
/**
 * Parses tool call blocks from CLI agent text responses.
 * 
 * Supports multiple extraction strategies:
 * 1. XML-delimited: <tool_call id="tc_1">{"name":"...", "arguments":{...}}</tool_call>
 * 2. JSON array fallback: [{"name":"...", "arguments":{...}}, ...]
 * 3. Fenced code block: ```tool_call\n{...}\n```
 *
 * Used by all CLI providers (Claude Code, Codex, future agents) to convert
 * text-based tool call output into FOliveStreamChunk structs that
 * ConversationManager can process identically to API provider tool calls.
 */
class OLIVEAIEDITOR_API FOliveCLIToolCallParser
{
public:
    /**
     * Parse tool calls from response text.
     * Tries XML-delimited first, then JSON array, then fenced blocks.
     *
     * @param ResponseText  Full text response from CLI agent
     * @param OutToolCalls  Parsed tool calls as stream chunks
     * @param OutCleanText  Response text with tool call blocks removed
     * @return true if any tool calls were found
     */
    static bool Parse(
        const FString& ResponseText,
        TArray<FOliveStreamChunk>& OutToolCalls,
        FString& OutCleanText);

    /**
     * Build the tool call format instruction text.
     * Included in CLI provider system prompts to teach the model the format.
     *
     * @return Instruction text for the model
     */
    static FString GetFormatInstructions();

private:
    static bool TryParseXMLDelimited(const FString& Text, TArray<FOliveStreamChunk>& OutCalls, FString& OutClean);
    static bool TryParseJSONArray(const FString& Text, TArray<FOliveStreamChunk>& OutCalls, FString& OutClean);
    static bool TryParseFencedBlocks(const FString& Text, TArray<FOliveStreamChunk>& OutCalls, FString& OutClean);
    static FString GenerateToolCallId();
};
```

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIToolCallParser.cpp`

Key implementation details:

```cpp
bool FOliveCLIToolCallParser::Parse(
    const FString& ResponseText,
    TArray<FOliveStreamChunk>& OutToolCalls,
    FString& OutCleanText)
{
    // Try strategies in order of reliability
    if (TryParseXMLDelimited(ResponseText, OutToolCalls, OutCleanText))
    {
        return OutToolCalls.Num() > 0;
    }
    if (TryParseJSONArray(ResponseText, OutToolCalls, OutCleanText))
    {
        return OutToolCalls.Num() > 0;
    }
    if (TryParseFencedBlocks(ResponseText, OutToolCalls, OutCleanText))
    {
        return OutToolCalls.Num() > 0;
    }

    OutCleanText = ResponseText;
    return false;
}

FString FOliveCLIToolCallParser::GetFormatInstructions()
{
    return
        TEXT("When you want to call a tool, output a <tool_call> block:\n")
        TEXT("<tool_call id=\"tc_1\">\n")
        TEXT("{\"name\": \"tool.name\", \"arguments\": {\"param\": \"value\"}}\n")
        TEXT("</tool_call>\n\n")
        TEXT("You can output multiple <tool_call> blocks in one response.\n")
        TEXT("After tools execute, you'll receive results and can continue.\n")
        TEXT("When you're done (no more tools needed), respond normally without any <tool_call> blocks.\n");
}
```

The XML-delimited format is chosen because:
- LLMs are extremely reliable at producing content in XML delimiters
- It's unambiguous (no collision with JSON in tool arguments)
- The `id` attribute provides tool call correlation
- It's easy to strip from visible text

### 3.2 New Shared Utility: `FOliveCLIToolSchemaSerializer`

Converts `FOliveToolDefinition` arrays into text descriptions for CLI provider prompts:

**File:** `Source/OliveAIEditor/Public/Providers/OliveCLIToolSchemaSerializer.h`

```cpp
/**
 * Serializes tool definitions into text format for CLI provider prompts.
 * 
 * Produces a compact representation that models can understand:
 * - Tool name and description
 * - Required and optional parameters with types
 * - Grouped by category for readability
 *
 * The output is designed to minimize token usage while preserving
 * all information needed for the model to correctly call tools.
 */
class OLIVEAIEDITOR_API FOliveCLIToolSchemaSerializer
{
public:
    /**
     * Serialize tool definitions to text.
     * Groups by category, includes parameter schemas.
     *
     * @param Tools  Tool definitions (already filtered by ToolPackManager)
     * @param bCompact  If true, omit descriptions for brevity
     * @return Text representation of tools for inclusion in prompts
     */
    static FString Serialize(
        const TArray<FOliveToolDefinition>& Tools,
        bool bCompact = false);

    /**
     * Estimate token count of the serialized output.
     * Uses 4 chars/token heuristic.
     */
    static int32 EstimateTokens(const TArray<FOliveToolDefinition>& Tools);

private:
    static FString SerializeSingleTool(const FOliveToolDefinition& Tool, bool bCompact);
    static FString SerializeSchema(const TSharedPtr<FJsonObject>& Schema, int32 Indent);
};
```

Example output (what the model sees in its prompt):

```
## Available Tools

### blueprint
- blueprint.create(path: string [required], parent_class: string [default: "Actor"])
  Create a new Blueprint asset.
- blueprint.read(blueprint_path: string [required])
  Read Blueprint structure, components, variables, and graph.
- blueprint.add_component(blueprint_path: string [required], component_class: string [required], component_name: string, attach_to: string)
  Add a component to a Blueprint.
- blueprint.add_variable(blueprint_path: string [required], variable_name: string [required], variable_type: string [required], default_value: string, category: string)
  Add a variable to a Blueprint.
- blueprint.apply_plan_json(blueprint_path: string [required], plan_json: object [required])
  Apply a complete graph plan to a Blueprint's EventGraph.
...

### project
- project.search(query: string [required], asset_types: array)
  Search project assets by name or path.
...
```

This replaces the 95 full JSON Schema definitions that MCP `tools/list` returns. The compact text format uses ~3-5x fewer tokens than JSON Schema while conveying the same information.

### 3.3 Changes to `FOliveClaudeCodeProvider`

The provider's `SendMessage` changes from "spawn with MCP and let it run" to "spawn without MCP in single-turn mode":

**Key changes in `SendMessage`:**

```cpp
void FOliveClaudeCodeProvider::SendMessage(
    const TArray<FOliveChatMessage>& Messages,
    const TArray<FOliveToolDefinition>& Tools,  // now actually used
    FOnOliveStreamChunk OnChunk,
    FOnOliveToolCall OnToolCall,                 // now actually fires
    FOnOliveComplete OnComplete,
    FOnOliveError OnError,
    const FOliveRequestOptions& Options)
{
    // Store OnToolCall — we'll actually use it now
    CurrentOnToolCall = OnToolCall;
    
    // Build tool schema text from the FILTERED tool list
    // (ConversationManager already filtered via ToolPackManager)
    FString ToolSchemaText = FOliveCLIToolSchemaSerializer::Serialize(Tools);
    
    // Build system prompt with tool definitions + call format
    FString SystemPromptText = BuildSystemPrompt(Messages, Tools);
    SystemPromptText += TEXT("\n\n") + ToolSchemaText;
    SystemPromptText += TEXT("\n\n") + FOliveCLIToolCallParser::GetFormatInstructions();
    
    // Build the user-facing prompt from message history
    FString Prompt = BuildPrompt(Messages, Tools);
    
    // Spawn claude WITHOUT MCP, single-turn
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, Prompt, SystemPromptText]()
    {
        FString ClaudePath = GetClaudeExecutablePath();
        // ... error check ...
        
        FString EscapedPrompt = EscapeForCommandLine(Prompt);
        FString EscapedSystemPrompt = EscapeForCommandLine(SystemPromptText);
        
        // NO --mcp-config, NO --strict-mcp-config
        // YES --max-turns 1 (single completion)
        FString ClaudeArgs = FString::Printf(
            TEXT("--print --output-format stream-json --verbose ")
            TEXT("--dangerously-skip-permissions ")
            TEXT("--max-turns 1 ")
            TEXT("--append-system-prompt \"%s\" ")
            TEXT("-p \"%s\""),
            *EscapedSystemPrompt,
            *EscapedPrompt
        );
        
        // ... spawn process, read output ...
    });
}
```

**Key changes in `ParseOutputLine` / completion handling:**

```cpp
void FOliveClaudeCodeProvider::HandleResponseComplete()
{
    // Parse tool calls from the accumulated text response
    TArray<FOliveStreamChunk> ParsedToolCalls;
    FString CleanedText;
    
    bool bHasToolCalls = FOliveCLIToolCallParser::Parse(
        AccumulatedResponse, ParsedToolCalls, CleanedText);
    
    if (bHasToolCalls)
    {
        // Emit tool calls via OnToolCall — ConversationManager will process them
        for (const FOliveStreamChunk& ToolCall : ParsedToolCalls)
        {
            FScopeLock Lock(&CallbackLock);
            CurrentOnToolCall.ExecuteIfBound(ToolCall);
        }
    }
    
    // Complete with appropriate finish reason
    FOliveProviderUsage Usage;
    Usage.FinishReason = bHasToolCalls ? TEXT("tool_calls") : TEXT("stop");
    
    FScopeLock Lock(&CallbackLock);
    CurrentOnComplete.ExecuteIfBound(CleanedText, Usage);
}
```

**Key changes in `BuildPrompt`:**

The prompt builder now formats the full conversation history (including tool results) as text:

```cpp
FString FOliveClaudeCodeProvider::BuildPrompt(
    const TArray<FOliveChatMessage>& Messages,
    const TArray<FOliveToolDefinition>& Tools) const
{
    FString Prompt;
    
    for (const FOliveChatMessage& Msg : Messages)
    {
        if (Msg.Role == EOliveChatRole::System)
        {
            continue; // Handled via --append-system-prompt
        }
        else if (Msg.Role == EOliveChatRole::User)
        {
            Prompt += FString::Printf(TEXT("[User]\n%s\n\n"), *Msg.Content);
        }
        else if (Msg.Role == EOliveChatRole::Assistant)
        {
            Prompt += FString::Printf(TEXT("[Assistant]\n%s\n\n"), *Msg.Content);
        }
        else if (Msg.Role == EOliveChatRole::Tool)
        {
            // Format tool results so the model can see what happened
            Prompt += FString::Printf(
                TEXT("[Tool Result: %s (id: %s)]\n%s\n\n"),
                *Msg.ToolName, *Msg.ToolCallId, *Msg.Content);
        }
    }
    
    return Prompt;
}
```

### 3.4 Changes to `BuildSystemPrompt`

The system prompt is simplified. Remove the mechanical instructions about "call tools one at a time" and "follow this exact order." The model doesn't call tools directly anymore — it expresses intent via `<tool_call>` blocks, and `ConversationManager` sequences execution.

**Before (~3KB of tool-calling logistics):**
```
IMPORTANT: Asset paths MUST end with the asset name...
ALWAYS use blueprint.apply_plan_json for graph logic...
After apply_plan_json, if wiring_errors exist, use blueprint.read...
Follow this exact order, one call at a time...
```

**After (design-focused):**
```
You are a UE5.5 Blueprint architect. You design and build Blueprints using
available tools. Think through the complete design before calling any tools.

## Design Approach
1. Plan the full Blueprint: parent class, components, variables, graph logic
2. Call tools to create and configure the Blueprint
3. Use blueprint.apply_plan_json for graph logic — design the complete plan
   JSON in one shot, don't fix individual pins

## Plan JSON Reference
[existing schema reference, kept compact]

## Key Patterns
- Gun: StaticMeshComponent, ProjectileMovement on bullet, FireRate variable
- Pickup: SphereComponent for overlap, bIsActive variable
- Door: TimelineComponent, InterpToMovement or SetActorLocation
```

The system prompt now teaches *design patterns* instead of tool-calling mechanics. The model's attention goes toward "what should this Blueprint look like" rather than "how do I format this tool call."

### 3.5 No Changes to `ConversationManager`

This is the key advantage. `FOliveConversationManager` doesn't change at all. The agentic loop already works correctly:

1. `SendToProvider()` — sends messages + tools to whatever provider is active
2. `HandleToolCall()` — collects tool calls into `PendingToolCalls`
3. `HandleComplete()` — if `PendingToolCalls.Num() > 0`, calls `ProcessPendingToolCalls()`
4. `ExecuteToolCall()` — runs via `FOliveToolRegistry::Get().ExecuteTool()`
5. `HandleToolResult()` — applies `SelfCorrectionPolicy`, records in `OperationHistory`
6. `ContinueAfterToolResults()` — sends tool results back via `SendToProvider()`
7. Repeat until `FinishReason == "stop"`

All of this already works. The only thing that was broken was that Claude Code CLI bypassed steps 2-6. Now it doesn't.

### 3.6 No Changes to Brain Layer

`FOliveBrainLayer` state machine already supports this flow:

```
Idle → WorkerActive (BeginRun)
  WorkerPhase: Streaming → ExecutingTools → SelfCorrecting → Streaming → ...
WorkerActive → Completed (CompleteRun)
Completed → Idle (ResetToIdle)
```

The `Planning` state (currently unused, reserved for Phase E2) remains available for the future design-pass optimization described in Section 5.

---

## 4. Adding a New CLI Provider

To demonstrate the architecture's extensibility, here's what adding OpenAI Codex CLI would require:

**File:** `Source/OliveAIEditor/Public/Providers/OliveCodexProvider.h`

```cpp
class OLIVEAIEDITOR_API FOliveCodexProvider : public IOliveAIProvider
{
public:
    virtual FString GetProviderName() const override { return TEXT("Codex CLI"); }
    
    virtual void SendMessage(
        const TArray<FOliveChatMessage>& Messages,
        const TArray<FOliveToolDefinition>& Tools,
        FOnOliveStreamChunk OnChunk,
        FOnOliveToolCall OnToolCall,
        FOnOliveComplete OnComplete,
        FOnOliveError OnError,
        const FOliveRequestOptions& Options) override;
    
    // ... standard interface methods ...

private:
    // Only provider-specific code: how to spawn the CLI and parse its output
    bool SpawnCodexProcess(const FString& Prompt, const FString& SystemPrompt);
    void ParseCodexOutput(const FString& Line);
    void HandleCodexComplete();
    
    // Shared with all CLI providers:
    // - FOliveCLIToolCallParser::Parse() for extracting tool calls
    // - FOliveCLIToolSchemaSerializer::Serialize() for tool definitions
    // - BuildPrompt() pattern for formatting conversation history
};
```

**What's provider-specific (~200 lines):**
- `SpawnCodexProcess()` — CLI path, flags, auth method
- `ParseCodexOutput()` — Codex-specific output format
- Model name list, version detection

**What's shared (zero new code):**
- `FOliveCLIToolCallParser` — tool call extraction from text
- `FOliveCLIToolSchemaSerializer` — tool definitions in text
- `ConversationManager` — the entire agentic loop
- All Brain Layer infrastructure — self-correction, loop detection, distillation
- `ToolPackManager` — tool filtering

**Estimated effort:** 1 day for a new CLI provider. Compared to building custom MCP proxies, bridge scripts, and agent-specific adapters.

---

## 5. Future: Design Pass Optimization

Once the unified architecture is working, the `Planning` state in `FOliveBrainLayer` can be used for a design-quality optimization:

```
User Request
    │
    ▼
Brain: Idle → Planning
    │
    ▼
Design Pass (any provider, NO tools in prompt)
    │ System prompt: "You are a UE5 Blueprint architect."
    │ Output: complete plan JSON + asset/variable lists
    │ No tool schemas = clean context window for design thinking
    ▼
preview_plan_json (C++ side, no AI)
    │ Validates via existing FOliveBlueprintPlanResolver
    │ Errors? → focused repair prompt → Design Pass (max 2 retries)
    ▼
Brain: Planning → WorkerActive
    │
    ▼
Execution Pass (any provider, WITH tools)
    │ Simplified prompt: "Execute this pre-designed plan."
    │ create → add_component → add_variable → apply_plan_json → read
    ▼
Brain: WorkerActive → Completed → Idle
```

This uses the same `ConversationManager` loop for both passes. The design pass just runs with an empty tools array (pure text generation). The execution pass runs with tools. Both work identically across API and CLI providers.

This is a **separate effort** that builds on the unified architecture. It doesn't block or depend on the CLI provider changes — it benefits from them.

---

## 6. Implementation Plan

### Phase 1: Shared Utilities (1-2 days)

Create `FOliveCLIToolCallParser` and `FOliveCLIToolSchemaSerializer`.

**Files to create:**
- `Source/OliveAIEditor/Public/Providers/OliveCLIToolCallParser.h`
- `Source/OliveAIEditor/Private/Providers/OliveCLIToolCallParser.cpp`
- `Source/OliveAIEditor/Public/Providers/OliveCLIToolSchemaSerializer.h`
- `Source/OliveAIEditor/Private/Providers/OliveCLIToolSchemaSerializer.cpp`

**Tests:**
- Parse XML-delimited tool calls (single, multiple, nested JSON)
- Parse with malformed blocks (graceful fallback)
- Serialize tool definitions (full and compact modes)
- Round-trip: serialize → include in prompt → model outputs → parse back

### Phase 2: Modify `FOliveClaudeCodeProvider` (2-3 days)

**Files to modify:**
- `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp`
  - `SendMessage()` — remove MCP config, add `--max-turns 1`, include tool schemas in prompt
  - `BuildPrompt()` — format full conversation history with tool results
  - `BuildSystemPrompt()` — replace tool-calling logistics with design guidance
  - `ParseOutputLine()` — keep text streaming as-is
  - New `HandleResponseComplete()` — parse tool calls, emit via `OnToolCall`
  - Remove the "Do NOT forward tool calls" bypass logic
- `Source/OliveAIEditor/Public/Providers/OliveClaudeCodeProvider.h`
  - Add `HandleResponseComplete()` declaration
  - Store `CurrentOnToolCall` delegate (currently unused)

**Verification:**
- Claude Code provider now emits `OnToolCall` events
- `ConversationManager::ProcessPendingToolCalls` fires for Claude Code
- `SelfCorrectionPolicy` evaluates Claude Code tool results
- `LoopDetector` tracks Claude Code retry patterns
- `OperationHistory` records Claude Code tool calls
- `ToolPackManager` filtering applied to Claude Code (via tools array in `SendMessage`)

### Phase 3: Update System Prompts (1 day)

**Files to modify:**
- `Content/SystemPrompts/Worker_Blueprint.txt` — add design patterns, remove tool-calling logistics
- `Content/SystemPrompts/Base.txt` — include tool call format instructions (via `GetFormatInstructions()`)

**New file:**
- `Content/SystemPrompts/ToolCallFormat.txt` — the `<tool_call>` format reference, loaded by `FOliveCLIToolCallParser::GetFormatInstructions()`

### Phase 4: Resolver Fixes (1-2 days, parallel with above)

These reduce the AI's need for UE-specific trivia regardless of provider:

**Files to modify:**
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
  - `ResolveEventOp()` — add event name mapping (`BeginPlay` → `ReceiveBeginPlay`)
  - Add `ExpandSpawnActorInputs()` — auto-synthesize MakeTransform when AI provides Location/Rotation
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp`
  - Add alias entries: `GetActorTransform` → `K2_GetActorTransform`, `MakeTransform` → `KismetMathLibrary::MakeTransform`
- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
  - `PhaseWireData()` — evaluate enabling `bAllowConversion = true` for auto Vector→Transform conversion

---

## 7. Trade-offs and Risks

### 7.1 Process Spawn Overhead

Each agentic loop iteration spawns a new `claude` process (~1-2s startup). A typical Blueprint creation takes 4-5 rounds:

| Round | What Happens | Overhead |
|-------|-------------|----------|
| 1 | Model outputs: create + add_components + add_variables | ~1.5s spawn |
| 2 | Model outputs: apply_plan_json | ~1.5s spawn |
| 3 | Model outputs: read to verify (or fix if errors) | ~1.5s spawn |
| 4 | Model outputs: done (no tool calls) | ~1.5s spawn |

**Total overhead: ~6s** across the whole task. Compared to the 10+ minutes of retry loops currently observed, this is a net improvement. The overhead is startup latency, not wasted computation.

**Mitigation:** If spawn overhead becomes a complaint, investigate keeping a warm `claude` process with `--max-turns` set higher (e.g., 3) to batch some iterations. This trades some loop control for speed.

### 7.2 Context Reconstruction Per Spawn

Each `claude --max-turns 1` invocation is stateless. The full conversation (system prompt + message history + tool results) is rebuilt every round.

**Why this is fine:** `PromptDistiller` already manages this for the API path. The same distillation applies — older tool results get summarized, recent ones kept verbatim. The CLI provider just formats the distilled messages as text instead of API message objects.

**Token cost:** Each spawn sends the full (distilled) context. For a 4-round task, you pay ~4x the context tokens vs a persistent connection. At current pricing, this is negligible compared to the output token savings from eliminating retry spirals.

### 7.3 Model Compliance with `<tool_call>` Format

LLMs occasionally produce malformed structured output. Risk mitigation:

1. **XML delimiters are high-reliability** — Claude and GPT are very consistent with XML-delimited output
2. **Multi-strategy parser** — falls back from XML to JSON array to fenced blocks
3. **Graceful degradation** — if no tool calls are parsed, the response is treated as `finish_reason: "stop"` (model decided to respond with text only)
4. **Format reinforcement** — tool call format instructions are in both system prompt and included at the end of tool result messages

### 7.4 `--max-turns 1` Availability

Claude Code's `--max-turns` flag may not exist in all versions or other CLI agents.

**Mitigation:** 
- Check flag availability during `ValidateConnection()`
- For agents without a max-turns flag: don't provide MCP config → agent has no tools to call → effectively single-turn
- If an agent somehow still tries to call tools internally, they'll fail (no MCP server) and the agent will fall back to text output

### 7.5 Backward Compatibility

Users currently on Claude Code CLI will see a behavior change:
- **Before:** Claude Code runs autonomously, user sees streaming text only
- **After:** ConversationManager runs the loop, user sees tool execution progress in the chat panel (tool call started/completed events)

This is strictly better UX — users get visibility into what's happening. The chat panel already shows tool execution progress for API providers.

---

## 8. Success Criteria

1. **All Brain Layer systems active for Claude Code:** Self-correction policy fires, loop detection catches spirals, tool packs filter tools, operation history records calls, prompt distillation manages context
2. **Same Blueprint quality:** Claude Code via unified path produces the same (or better) quality Blueprints as the Anthropic API provider path
3. **Retry loops eliminated:** The BP_Gun + BP_Bullet scenario from the log analysis completes in ≤5 tool iterations instead of 15+
4. **New CLI provider in ≤1 day:** Adding a Codex provider requires only the spawn logic and output parser, reusing all shared infrastructure
5. **No ConversationManager changes:** The agentic loop code is untouched — proving the architecture is provider-agnostic

---

## 9. Files Summary

### New Files
| File | Purpose |
|------|---------|
| `Providers/OliveCLIToolCallParser.h/.cpp` | Parse `<tool_call>` blocks from CLI text output |
| `Providers/OliveCLIToolSchemaSerializer.h/.cpp` | Serialize `FOliveToolDefinition` arrays to text |
| `Content/SystemPrompts/ToolCallFormat.txt` | Model instructions for `<tool_call>` format |

### Modified Files
| File | Change |
|------|--------|
| `Providers/OliveClaudeCodeProvider.cpp` | Remove MCP, add single-turn, emit OnToolCall, parse tool calls |
| `Providers/OliveClaudeCodeProvider.h` | Add `HandleResponseComplete()`, store OnToolCall |
| `Content/SystemPrompts/Worker_Blueprint.txt` | Design patterns instead of tool-calling logistics |
| `Content/SystemPrompts/Base.txt` | Include tool call format reference |
| `Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | Event name mapping, SpawnActor expansion |
| `Blueprint/Private/Plan/OliveFunctionResolver.cpp` | Additional function aliases |
| `Blueprint/Private/Plan/OlivePlanExecutor.cpp` | Evaluate `bAllowConversion = true` |

### Unchanged Files
| File | Why Unchanged |
|------|--------------|
| `Chat/OliveConversationManager.cpp/.h` | Agentic loop already works — CLI providers now feed into it |
| `Brain/OliveBrainLayer.cpp/.h` | State machine already supports the flow |
| `Brain/OliveSelfCorrectionPolicy.cpp/.h` | Already works — just wasn't being called for Claude Code |
| `Brain/OlivePromptDistiller.cpp/.h` | Already works for message distillation |
| `Brain/OliveToolPackManager.cpp/.h` | Already filters tools — now CLI providers receive filtered list |
| `MCP/OliveToolRegistry.cpp/.h` | Tool execution unchanged |
| All other providers | API providers are already correct |
