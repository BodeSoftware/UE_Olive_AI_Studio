# Planner MCP Tool Access Design

**Status:** Ready for review
**Author:** Architect Agent
**Date:** 2026-03-07

## Overview

Give the Planner agent (the combined Researcher+Architect in the CLI-optimized path) access to MCP tools so it can read template data on demand, rather than cramming 27-31K chars of template overviews into the prompt upfront.

**Current flow:**
Scout (pure C++) builds `TemplateOverviews` (27-31K chars of structural data) -> RunPlanner inlines everything into one huge prompt -> single `claude --print` call (no tools, `--max-turns 1`).

**New flow:**
Scout (pure C++) builds compact `TemplateHeaders` (~300-400 chars each, ~3-5K total) -> RunPlannerWithTools launches `claude --print --max-turns 15` with MCP access -> Planner calls `blueprint.get_template` / `blueprint.list_templates` on demand -> produces Build Plan.

### Why This Matters

1. **Token efficiency**: 27-31K of template overviews means the Planner pays for ~8K input tokens it mostly ignores. Compact headers + on-demand reads means it only fetches what it needs.
2. **Better plans**: The Planner can read full function graph data (node-by-node wiring) for templates it finds relevant, instead of just seeing structural summaries. This directly addresses the "PrintString-only logic" quality problem.
3. **Scalability**: As the library grows, the upfront-dump approach becomes untenable. On-demand fetching scales to any library size.

---

## 1. Approach: Inline Process with Blocking Read Loop

The Planner does NOT use `FOliveCLIProviderBase::LaunchCLIProcess()`. That method is designed for async operation (background thread read loop, `AliveGuard`, `RequestGeneration` counters, callback-based completion). The Planner runs **synchronously on the game thread** inside `ExecuteCLIPath()`, which already uses a tick-pump blocking pattern.

Instead, the Planner spawns its own process inline, the same way the existing Tier 3 `--print` path does (lines 528-665 of `OliveAgentPipeline.cpp`), but with these differences:

1. **MCP access**: Uses a sandbox directory with `.mcp.json` so Claude discovers the MCP server.
2. **Multi-turn**: `--max-turns 15` instead of `--max-turns 1`, allowing tool calls.
3. **Tool filter**: Sets `FOliveMCPServer::SetToolFilter()` before launch, clears after.
4. **Stream-JSON parsing**: Reads `stream-json` output to extract the final text response (skipping tool_use/tool_result events).
5. **Timeout**: 180s hard limit (up from 120s) to accommodate tool round-trips.

### Why Not LaunchCLIProcess?

`LaunchCLIProcess` is:
- Async (spawns background thread, dispatches to game thread via AsyncTask)
- Callback-based (OnChunk, OnComplete, OnError)
- Owns process state on `FOliveCLIProviderBase` members (ProcessHandle, StdinWritePipe, etc.)
- Designed for the Builder's long-running lifecycle

The Planner needs:
- Synchronous (blocking with tick-pump, returns a result struct)
- No callbacks (caller reads the return value)
- Short-lived (15 turns max, 180s timeout)
- No shared state (pipeline is instantiated per-run)

Extracting a shared helper would require either converting LaunchCLIProcess to support both modes (complex, risky) or creating a "blocking subprocess" abstraction that both could use. The inline approach is simpler, self-contained, and matches the existing Tier 3 pattern. The Planner's process management code is ~80 lines -- small enough that duplication is acceptable.

---

## 2. File Changes

### Modified Files

```
Source/OliveAIEditor/Public/Brain/OliveAgentPipeline.h      -- Add RunPlannerWithTools(), SetupPlannerSandbox(), ParseStreamJsonFinalText()
Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp    -- Implement RunPlannerWithTools, modify ExecuteCLIPath to choose path
Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h         -- (no changes needed)
```

### No New Files

All changes are contained within the existing pipeline files. The Planner sandbox reuses the same `.mcp.json` format as the Builder's `SetupAutonomousSandbox()`.

---

## 3. Header Changes (OliveAgentPipeline.h)

Add three new private methods after the existing `RunPlanner`:

```cpp
    /**
     * CLI-optimized pipeline: Planner with MCP tool access.
     * Launches claude --print with --max-turns 15 and a filtered tool set
     * (blueprint.get_template, blueprint.list_templates, blueprint.describe).
     * The Planner reads template data on demand instead of receiving it all upfront.
     *
     * Falls back to RunPlanner() (no tools) if:
     * - MCP server is not running
     * - Claude CLI is not installed
     * - Process spawn fails
     *
     * @param UserMessage         The user's original task description
     * @param ScoutResult         Discovery and asset results from the pure-C++ Scout
     * @param ContextAssetPaths   @-mentioned asset paths for inline IR loading
     * @return Architect result with Build Plan
     */
    FOliveArchitectResult RunPlannerWithTools(
        const FString& UserMessage,
        const FOliveScoutResult& ScoutResult,
        const TArray<FString>& ContextAssetPaths);

    /**
     * Set up a minimal sandbox directory for the Planner's CLI process.
     * Creates {ProjectDir}/Saved/OliveAI/PlannerSandbox/ with:
     * - .mcp.json (MCP server connection via mcp-bridge.js)
     * - CLAUDE.md (minimal Planner-specific instructions)
     *
     * Reuses the same directory across runs (no cleanup needed -- files are overwritten).
     *
     * @return Absolute path to the sandbox directory, or empty string on failure
     */
    static FString SetupPlannerSandbox();

    /**
     * Extract the final text content from stream-json output.
     * Parses each line as JSON. Collects text from "assistant" type messages
     * (content[].type=="text"), ignoring tool_use, tool_result, and system events.
     * Falls back to treating the entire output as plain text if no JSON is found.
     *
     * @param StreamOutput  Raw stdout from the CLI process (newline-delimited JSON)
     * @return The extracted text response (Build Plan)
     */
    static FString ParseStreamJsonFinalText(const FString& StreamOutput);
```

---

## 4. Implementation Details

### 4.1 ExecuteCLIPath Changes

The existing `ExecuteCLIPath` calls `RunPlanner()` at line 1792. Change this to try `RunPlannerWithTools()` first, falling back to `RunPlanner()`:

```cpp
// --- Stage 2: Planner (MCP-enabled if possible, otherwise single-shot) ---
{
    // Try MCP-enabled Planner first (reads templates on demand).
    // Falls back to single-shot if MCP server isn't running or CLI not available.
    if (FOliveMCPServer::Get().IsRunning() && FOliveClaudeCodeProvider::IsClaudeCodeInstalled())
    {
        Result.Architect = RunPlannerWithTools(UserMessage, Result.Scout, ContextAssetPaths);
    }

    // Fallback: single-shot Planner with all template data inlined
    if (!Result.Architect.bSuccess)
    {
        UE_LOG(LogOliveAgentPipeline, Log,
            TEXT("  Planner (MCP) %s, falling back to single-shot"),
            Result.Architect.ElapsedSeconds > 0.0 ? TEXT("failed") : TEXT("skipped"));
        Result.Architect = RunPlanner(UserMessage, Result.Scout, ContextAssetPaths);
    }

    UE_LOG(LogOliveAgentPipeline, Log, ...);
}
```

### 4.2 RunPlannerWithTools Implementation

```cpp
FOliveArchitectResult FOliveAgentPipeline::RunPlannerWithTools(
    const FString& UserMessage,
    const FOliveScoutResult& ScoutResult,
    const TArray<FString>& ContextAssetPaths)
{
    FOliveArchitectResult Result;
    const double StartTime = FPlatformTime::Seconds();

    // --- Build user prompt (compact: no template overviews, just headers) ---
    FString PlannerUserPrompt;
    PlannerUserPrompt.Reserve(4096);

    // Task description
    PlannerUserPrompt += TEXT("## Task\n\n");
    PlannerUserPrompt += UserMessage;
    PlannerUserPrompt += TEXT("\n\n");

    // Existing assets with inline IR data (same as RunPlanner)
    // [... identical to RunPlanner lines 1848-1931 ...]

    // Template discovery -- compact headers only, NOT full overviews
    if (!ScoutResult.DiscoveryBlock.IsEmpty())
    {
        PlannerUserPrompt += TEXT("\n");
        PlannerUserPrompt += ScoutResult.DiscoveryBlock;
        PlannerUserPrompt += TEXT("\n");
    }

    // Template references as compact headers (~300-400 chars each)
    if (ScoutResult.TemplateReferences.Num() > 0)
    {
        PlannerUserPrompt += TEXT("\n## Available Templates\n\n");
        PlannerUserPrompt += TEXT("Use `blueprint.get_template` to read full function details.\n\n");
        for (const FOliveTemplateReference& Ref : ScoutResult.TemplateReferences)
        {
            PlannerUserPrompt += TEXT("- **") + Ref.DisplayName + TEXT("** (`") + Ref.TemplateId + TEXT("`)");
            if (!Ref.ParentClass.IsEmpty())
            {
                PlannerUserPrompt += TEXT(" -- Parent: ") + Ref.ParentClass;
            }
            if (Ref.MatchedFunctions.Num() > 0)
            {
                PlannerUserPrompt += TEXT(" -- Functions: ") + FString::Join(Ref.MatchedFunctions, TEXT(", "));
            }
            PlannerUserPrompt += TEXT("\n");
        }
    }

    // NOTE: TemplateOverviews is intentionally NOT included. The Planner
    // fetches full details on-demand via get_template tool calls.

    // --- Build system prompt ---
    FString SystemPrompt = BuildPlannerSystemPrompt();

    // Append tool usage instructions
    SystemPrompt += TEXT("\n\n## Available Tools\n\n");
    SystemPrompt += TEXT("You have access to these read-only MCP tools:\n");
    SystemPrompt += TEXT("- `blueprint.get_template(template_id, pattern?)` -- Read a template's structure or a specific function's node graph\n");
    SystemPrompt += TEXT("- `blueprint.list_templates(query?)` -- Search for templates by keyword\n");
    SystemPrompt += TEXT("- `blueprint.describe(path)` -- Read an existing Blueprint's structure\n");
    SystemPrompt += TEXT("\nBefore writing the Build Plan, call get_template on 1-3 relevant templates to study their function implementations.\n");
    SystemPrompt += TEXT("Base your function descriptions on the patterns you observe in templates.\n");
    SystemPrompt += TEXT("When done researching, output ONLY the Build Plan (## Build Plan header to end).\n");

    // --- Set MCP tool filter (only read tools) ---
    // Save and restore the previous filter since the Builder may have set one
    static const TSet<FString> PlannerToolPrefixes = {
        TEXT("blueprint.get_template"),
        TEXT("blueprint.list_templates"),
        TEXT("blueprint.describe")
    };
    FOliveMCPServer::Get().SetToolFilter(PlannerToolPrefixes);

    // --- Set up sandbox directory ---
    const FString SandboxDir = SetupPlannerSandbox();
    if (SandboxDir.IsEmpty())
    {
        UE_LOG(LogOliveAgentPipeline, Warning, TEXT("Failed to create Planner sandbox"));
        FOliveMCPServer::Get().ClearToolFilter();
        return Result;  // bSuccess = false triggers fallback
    }

    // --- Spawn CLI process ---
    const FString ClaudePath = FOliveClaudeCodeProvider::GetClaudeExecutablePath();
    const bool bIsJs = ClaudePath.EndsWith(TEXT(".js")) || ClaudePath.EndsWith(TEXT(".mjs"));

    // Key differences from the Builder's autonomous args:
    // --max-turns 15: enough to read 3-5 templates, not enough for adventures
    // --output-format stream-json: parseable output for text extraction
    // NO --strict-mcp-config: allows .mcp.json discovery
    static constexpr int32 PLANNER_MAX_TURNS = 15;
    const FString BaseArgs = FString::Printf(
        TEXT("--print --output-format stream-json --verbose --dangerously-skip-permissions --max-turns %d"),
        PLANNER_MAX_TURNS);

    FString Executable;
    FString Args;
    if (bIsJs)
    {
        Executable = TEXT("node");
        Args = FString::Printf(TEXT("\"%s\" %s"), *ClaudePath, *BaseArgs);
    }
    else
    {
        Executable = ClaudePath;
        Args = BaseArgs;
    }

    // Combine system + user prompt for stdin delivery
    FString StdinContent = SystemPrompt + TEXT("\n\n---\n\n") + PlannerUserPrompt;

    // Create pipes
    void* StdoutRead = nullptr;
    void* StdoutWrite = nullptr;
    void* StdinRead = nullptr;
    void* StdinWrite = nullptr;

    bool bPipesOk = FPlatformProcess::CreatePipe(StdoutRead, StdoutWrite)
        && FPlatformProcess::CreatePipe(StdinRead, StdinWrite, true);

    if (!bPipesOk)
    {
        if (StdoutRead) FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
        FOliveMCPServer::Get().ClearToolFilter();
        return Result;
    }

    uint32 ProcessId = 0;
    FProcHandle ProcHandle = FPlatformProcess::CreateProc(
        *Executable, *Args,
        false, true, true,
        &ProcessId, 0,
        *SandboxDir,       // Working directory with .mcp.json
        StdoutWrite,
        StdinRead
    );

    if (!ProcHandle.IsValid())
    {
        FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
        FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
        FOliveMCPServer::Get().ClearToolFilter();
        return Result;
    }

    // Close pipe ends we don't use
    FPlatformProcess::ClosePipe(nullptr, StdoutWrite);
    StdoutWrite = nullptr;
    FPlatformProcess::ClosePipe(StdinRead, nullptr);
    StdinRead = nullptr;

    // Write prompt via stdin, then close
    FPlatformProcess::WritePipe(StdinWrite, StdinContent);
    FPlatformProcess::ClosePipe(nullptr, StdinWrite);
    StdinWrite = nullptr;

    UE_LOG(LogOliveAgentPipeline, Log,
        TEXT("  Planner (MCP): launched with %d char prompt, max %d turns, PID=%u"),
        StdinContent.Len(), PLANNER_MAX_TURNS, ProcessId);

    // --- Read loop with tick-pumping ---
    // Unlike the Builder's async read loop, this runs inline on the game thread.
    // The MCP server handles tool calls on the game thread via tick-pumping,
    // so tool_call -> tool_result round-trips happen during our Sleep/Tick cycle.
    static constexpr double PLANNER_TIMEOUT_SECONDS = 180.0;

    FString RawOutput;
    bool bTimedOut = false;

    while (FPlatformProcess::IsProcRunning(ProcHandle))
    {
        // Read any available stdout
        RawOutput += FPlatformProcess::ReadPipe(StdoutRead);

        // Check timeout
        if (FPlatformTime::Seconds() - StartTime >= PLANNER_TIMEOUT_SECONDS)
        {
            FPlatformProcess::TerminateProc(ProcHandle, true);
            bTimedOut = true;
            UE_LOG(LogOliveAgentPipeline, Warning,
                TEXT("  Planner (MCP): timed out after %.0fs"), PLANNER_TIMEOUT_SECONDS);
            break;
        }

        // Pump game thread ticker (processes MCP HTTP requests + tool calls)
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FPlatformProcess::Sleep(0.01f);
    }

    // Read remaining output
    if (!bTimedOut)
    {
        RawOutput += FPlatformProcess::ReadPipe(StdoutRead);
    }
    FPlatformProcess::ClosePipe(StdoutRead, nullptr);

    int32 ReturnCode = -1;
    FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
    FPlatformProcess::CloseProc(ProcHandle);

    // --- Clear tool filter ---
    FOliveMCPServer::Get().ClearToolFilter();

    // --- Extract final text from stream-json ---
    if (ReturnCode == 0 || !RawOutput.IsEmpty())
    {
        FString PlanText = ParseStreamJsonFinalText(RawOutput);
        if (!PlanText.IsEmpty())
        {
            Result.BuildPlan = PlanText;
            Result.bSuccess = true;
            ParseBuildPlan(Result.BuildPlan, Result);

            UE_LOG(LogOliveAgentPipeline, Log,
                TEXT("  Planner (MCP): success, %d char plan, %d assets, %.1fs"),
                Result.BuildPlan.Len(), Result.AssetOrder.Num(),
                FPlatformTime::Seconds() - StartTime);
        }
    }

    if (!Result.bSuccess)
    {
        UE_LOG(LogOliveAgentPipeline, Warning,
            TEXT("  Planner (MCP): failed (code=%d, output=%d chars, timedOut=%s)"),
            ReturnCode, RawOutput.Len(), bTimedOut ? TEXT("true") : TEXT("false"));
    }

    Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
    return Result;
}
```

### 4.3 SetupPlannerSandbox

Minimal sandbox -- just `.mcp.json` and a short `CLAUDE.md`. Unlike the Builder's sandbox, the Planner does not need knowledge packs (cli_blueprint.txt, recipe_routing.txt, etc.) because the system prompt already contains all planning instructions.

```cpp
FString FOliveAgentPipeline::SetupPlannerSandbox()
{
    const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    const FString SandboxDir = FPaths::Combine(ProjectDir, TEXT("Saved/OliveAI/PlannerSandbox"));
    IFileManager::Get().MakeDirectory(*SandboxDir, true);

    // .mcp.json -- same format as Builder's sandbox
    const FString PluginDir = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
    const FString BridgePath = FPaths::Combine(PluginDir, TEXT("mcp-bridge.js"));
    FString BridgePathJson = BridgePath.Replace(TEXT("\\"), TEXT("/"));

    const FString McpConfig = FString::Printf(
        TEXT("{\n")
        TEXT("  \"mcpServers\": {\n")
        TEXT("    \"olive-ai-studio\": {\n")
        TEXT("      \"command\": \"node\",\n")
        TEXT("      \"args\": [\"%s\"]\n")
        TEXT("    }\n")
        TEXT("  }\n")
        TEXT("}\n"),
        *BridgePathJson);

    const FString McpConfigPath = FPaths::Combine(SandboxDir, TEXT(".mcp.json"));
    FFileHelper::SaveStringToFile(McpConfig, *McpConfigPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    // CLAUDE.md -- minimal, Planner-specific
    const FString ClaudeMd =
        TEXT("# Planner Agent\n\n")
        TEXT("You are an Unreal Engine Blueprint architect. Your job is to research templates ")
        TEXT("and produce a Build Plan.\n\n")
        TEXT("## Rules\n")
        TEXT("- Use ONLY the MCP tools provided (blueprint.get_template, blueprint.list_templates, blueprint.describe)\n")
        TEXT("- Do NOT create or modify any files\n")
        TEXT("- Do NOT use bash, read, write, or any other tools\n")
        TEXT("- After researching, output the Build Plan as your final text response\n");

    const FString ClaudeMdPath = FPaths::Combine(SandboxDir, TEXT("CLAUDE.md"));
    FFileHelper::SaveStringToFile(ClaudeMd, *ClaudeMdPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    return SandboxDir;
}
```

### 4.4 ParseStreamJsonFinalText

Extracts the final text from `--output-format stream-json`. The format emits JSON lines:
- `{"type":"assistant","message":{"content":[{"type":"text","text":"..."}]}}` -- text chunks
- `{"type":"tool_use",...}` / `{"type":"tool_result",...}` -- tool activity (skip)
- `{"type":"result",...}` -- completion marker (skip)

The Planner may produce text across multiple `assistant` messages (before and after tool calls). We want only the **last text block** that contains the Build Plan (starts with `## Build Plan`). If no Build Plan header is found, concatenate all text blocks.

```cpp
FString FOliveAgentPipeline::ParseStreamJsonFinalText(const FString& StreamOutput)
{
    TArray<FString> TextBlocks;
    FString CurrentBlock;

    TArray<FString> Lines;
    StreamOutput.ParseIntoArrayLines(Lines);

    for (const FString& Line : Lines)
    {
        FString Trimmed = Line.TrimStartAndEnd();
        if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("{")))
        {
            continue;
        }

        TSharedPtr<FJsonObject> JsonObject;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
        if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
        {
            continue;
        }

        FString Type;
        if (!JsonObject->TryGetStringField(TEXT("type"), Type))
        {
            continue;
        }

        if (Type == TEXT("assistant"))
        {
            // Start of a new assistant message -- save any accumulated block
            if (!CurrentBlock.IsEmpty())
            {
                TextBlocks.Add(CurrentBlock);
                CurrentBlock.Empty();
            }

            // Extract text content
            const TSharedPtr<FJsonObject>* MessageObj;
            if (JsonObject->TryGetObjectField(TEXT("message"), MessageObj))
            {
                const TArray<TSharedPtr<FJsonValue>>* ContentArray;
                if ((*MessageObj)->TryGetArrayField(TEXT("content"), ContentArray))
                {
                    for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
                    {
                        const TSharedPtr<FJsonObject>* ContentObj;
                        if (ContentValue->TryGetObject(ContentObj))
                        {
                            FString ContentType = (*ContentObj)->GetStringField(TEXT("type"));
                            if (ContentType == TEXT("text"))
                            {
                                CurrentBlock += (*ContentObj)->GetStringField(TEXT("text"));
                            }
                        }
                    }
                }
            }
        }
        else if (Type == TEXT("tool_use") || Type == TEXT("tool_call"))
        {
            // Tool call boundary -- save any accumulated text
            if (!CurrentBlock.IsEmpty())
            {
                TextBlocks.Add(CurrentBlock);
                CurrentBlock.Empty();
            }
        }
        // tool_result, result, message_stop, error -- skip
    }

    // Save final block
    if (!CurrentBlock.IsEmpty())
    {
        TextBlocks.Add(CurrentBlock);
    }

    if (TextBlocks.Num() == 0)
    {
        // No JSON parsed -- treat raw output as plain text (fallback)
        return StreamOutput.TrimStartAndEnd();
    }

    // Look for the block containing "## Build Plan"
    for (int32 i = TextBlocks.Num() - 1; i >= 0; --i)
    {
        if (TextBlocks[i].Contains(TEXT("## Build Plan")))
        {
            // Extract from "## Build Plan" to end of block
            int32 PlanStart = INDEX_NONE;
            TextBlocks[i].FindChar('#', PlanStart);
            int32 HeaderIdx = TextBlocks[i].Find(TEXT("## Build Plan"));
            if (HeaderIdx != INDEX_NONE)
            {
                return TextBlocks[i].Mid(HeaderIdx).TrimStartAndEnd();
            }
            return TextBlocks[i].TrimStartAndEnd();
        }
    }

    // No "## Build Plan" header found -- return the last text block
    return TextBlocks.Last().TrimStartAndEnd();
}
```

### 4.5 Tool Filter Mechanism

The MCP tool filter currently works by **prefix matching** on tool names. The filter `{"blueprint."}` allows all `blueprint.*` tools. For the Planner, we need to be more selective -- only read-oriented tools, not write tools like `blueprint.create` or `blueprint.apply_plan_json`.

**Problem**: `SetToolFilter` uses prefix matching. A prefix of `blueprint.get_template` would match only that one tool, not `blueprint.list_templates`.

**Solution**: Use exact tool name matching. Add a new `SetToolFilterExact()` method to `FOliveMCPServer` that filters by exact tool names rather than prefixes:

```cpp
// OliveMCPServer.h -- add alongside SetToolFilter
/**
 * Set a tool filter using exact tool name matching.
 * Only tools whose names appear in the set will be visible.
 * More precise than SetToolFilter (prefix-based).
 *
 * @param AllowedNames Exact tool names to allow
 */
void SetToolFilterExact(const TSet<FString>& AllowedNames);
```

```cpp
// OliveMCPServer.cpp
void FOliveMCPServer::SetToolFilterExact(const TSet<FString>& AllowedNames)
{
    FScopeLock Lock(&ToolFilterLock);
    ToolFilterExactNames = AllowedNames;
    UE_LOG(LogOliveAI, Log, TEXT("MCP tool filter (exact) set: %d tools"), AllowedNames.Num());
}
```

The tool listing handler checks both filters:
```cpp
// In HandleToolsList -- add exact name check after prefix check
if (ToolFilterExactNames.Num() > 0)
{
    if (!ToolFilterExactNames.Contains(Tool.Name))
    {
        continue;  // Filtered out
    }
}
```

**Alternative (simpler)**: If adding `SetToolFilterExact` feels heavy, we can use the existing prefix filter with an extra guard. Since the Planner's three tools all have distinct prefixes (`blueprint.get_template`, `blueprint.list_templates`, `blueprint.describe`), we can pass those as three prefix entries. The existing `SetToolFilter` iterates prefixes and checks `Tool.Name.StartsWith(Prefix)`, so `blueprint.get_template` as a prefix matches only `blueprint.get_template` (nothing else starts with that string). This is slightly hacky but works.

**Recommendation**: Use the simpler approach (three entries in the existing prefix filter). It works correctly because no other tool name starts with `blueprint.get_template`, `blueprint.list_templates`, or `blueprint.describe`. Add `SetToolFilterExact` only if we need it later for other use cases.

---

## 5. Data Flow

```
ExecuteCLIPath()
  |
  +-- Scout (pure C++, no change)
  |     |-- DiscoveryBlock (compact headers)
  |     |-- TemplateReferences (template_id + matched functions)
  |     |-- RelevantAssets (project index hits)
  |
  +-- RunPlannerWithTools()
  |     |
  |     |-- Build prompt (task + existing assets IR + compact template headers)
  |     |   NOTE: No TemplateOverviews inlined (saves ~25K chars)
  |     |
  |     |-- SetToolFilter (read-only tools)
  |     |-- SetupPlannerSandbox() -> Saved/OliveAI/PlannerSandbox/
  |     |     |-- .mcp.json (points to mcp-bridge.js)
  |     |     |-- CLAUDE.md (minimal Planner instructions)
  |     |
  |     |-- Spawn claude --print --max-turns 15 --output-format stream-json
  |     |     CWD = PlannerSandbox (has .mcp.json)
  |     |
  |     |-- Blocking read loop with tick-pump:
  |     |     while (proc running && !timeout)
  |     |       ReadPipe -> accumulate stdout
  |     |       FTSTicker::Tick(0.01f)    <- processes MCP HTTP requests
  |     |       Sleep(0.01f)
  |     |
  |     |-- MCP tool calls happen during tick-pump:
  |     |     Claude -> mcp-bridge.js -> HTTP -> MCP server -> game thread handler
  |     |     The tool handler runs during Tick(), returns result to MCP server,
  |     |     which sends HTTP response back to mcp-bridge, which pipes to Claude.
  |     |
  |     |-- ParseStreamJsonFinalText(stdout) -> extract Build Plan text
  |     |-- ClearToolFilter()
  |     |-- ParseBuildPlan() -> extract structured data
  |     |
  |     +-- Return FOliveArchitectResult
  |
  +-- Fallback: if RunPlannerWithTools failed, try RunPlanner() (single-shot)
  |
  +-- Validator (pure C++, no change)
  |
  +-- Return FOliveAgentPipelineResult
```

---

## 6. MCP Tool Call Flow During Tick-Pump

This is the critical part that makes this design work. The MCP server runs on the game thread via the HTTP listener. When the Planner's CLI process makes a tool call:

1. **Claude CLI** decides to call `blueprint.get_template(template_id="bp_melee_component", pattern="MeleeAttack")`
2. **mcp-bridge.js** converts the MCP stdio protocol to HTTP POST to `localhost:3001/mcp`
3. **MCP server** receives the HTTP request. Because the server's HTTP listener processes requests during `FTSTicker::Tick()`, the request is handled during our tick-pump loop.
4. **Tool handler** runs on the game thread (we are already on the game thread). `FOliveTemplateSystem::Get().GetLibraryIndex().GetFunctionContent()` returns the function data.
5. **MCP server** sends the JSON-RPC response back via HTTP.
6. **mcp-bridge.js** pipes the response back to Claude's stdin.
7. **Claude** processes the tool result and continues (either calls another tool or produces text).

This works because `FTSTicker::Tick()` processes both the HTTP listener and any queued `AsyncTask(GameThread, ...)` lambdas. The MCP server's HTTP handler is registered on the core ticker.

**Key insight**: The existing Tier 3 (single-shot `--print`) already calls `FPlatformProcess::Sleep(0.05f)` in its read loop but does NOT call `FTSTicker::Tick()`. That is why it cannot support MCP tool calls. Adding `FTSTicker::Tick(0.01f)` to the read loop is the one change that enables the MCP server to process requests during the Planner's execution.

---

## 7. Edge Cases and Error Handling

### 7.1 MCP Server Not Running

Detected upfront: `FOliveMCPServer::Get().IsRunning()`. If false, skip `RunPlannerWithTools` entirely and fall back to `RunPlanner()` (single-shot with inlined overviews).

### 7.2 Claude CLI Not Installed

Detected upfront: `FOliveClaudeCodeProvider::IsClaudeCodeInstalled()`. If false, skip. The existing `SendAgentCompletion` Tier 3 path already handles this -- `RunPlanner()` fallback will also fail at Tier 3 but may succeed at Tier 1 or 2 (API providers).

### 7.3 Process Spawn Failure

Pipe creation or `CreateProc` failure. Clear tool filter, return `bSuccess=false`, triggering fallback.

### 7.4 Timeout (180s)

Process killed via `TerminateProc`. Any partial output is still parsed -- if it contains a `## Build Plan`, that partial plan is used. Otherwise `bSuccess=false` triggers fallback.

### 7.5 Claude Ignores Tool Filter

If Claude discovers tools via `tools/list` but the filter hides write tools, Claude may try to call a hidden tool. The MCP server returns a `MethodNotFound` error for filtered tools, which Claude handles gracefully (logs the error, continues with text).

### 7.6 Claude Uses No Tools

Claude might produce a Build Plan without calling any tools (decides the prompt context is sufficient). This is fine -- `ParseStreamJsonFinalText` extracts the text response regardless.

### 7.7 Claude Goes on Adventures

`--max-turns 15` prevents runaway tool loops. After 15 turns, Claude is forced to produce a final response. Additionally, the 180s timeout acts as a hard ceiling.

### 7.8 Tool Filter Race Condition

The tool filter is global on `FOliveMCPServer`. If the Builder's autonomous process is also running (which it should not be -- the pipeline runs BEFORE the Builder), there would be a conflict. This is not a real concern because:
1. The pipeline runs synchronously before `SendMessageAutonomous()` calls `LaunchCLIProcess()`.
2. The filter is cleared before `RunPlannerWithTools` returns.

### 7.9 mcp-bridge.js Cold Start

The mcp-bridge auto-discovers MCP server ports 3000-3009. Its first connection takes ~1-2s (Node.js startup + port scan). This is included in the 180s timeout budget.

---

## 8. Prompt Changes

### 8.1 Planner System Prompt Addition

Append to the existing `BuildPlannerSystemPrompt()` return value when the MCP path is active. This is done inline in `RunPlannerWithTools`, NOT by modifying the shared `BuildPlannerSystemPrompt()` function (which is also used by the fallback single-shot path).

The appended text:

```
## Available Tools

You have access to these read-only MCP tools:
- `blueprint.get_template(template_id, pattern?)` -- Read a template's structure or a specific function's node graph
- `blueprint.list_templates(query?)` -- Search for templates by keyword
- `blueprint.describe(path)` -- Read an existing Blueprint's structure

Before writing the Build Plan, call get_template on 1-3 relevant templates to study their function implementations.
Base your function descriptions on the patterns you observe in templates.
When done researching, output ONLY the Build Plan (## Build Plan header to end).
```

### 8.2 Scout Changes: Template Headers

The Scout already builds `TemplateReferences` (compact list of template_id + display_name + parent_class + matched_functions). For the MCP path, the Planner prompt uses these references instead of `TemplateOverviews`.

No Scout code changes needed -- the data is already there. The difference is purely in how `RunPlannerWithTools` assembles the prompt (it skips `TemplateOverviews` and formats `TemplateReferences` as compact headers).

---

## 9. Settings

No new settings needed. The MCP Planner activates automatically when:
1. The pipeline is in CLI-only mode (`IsCLIOnlyMode() == true`)
2. The MCP server is running
3. Claude CLI is installed

If desired in the future, a `bEnablePlannerMCPTools` bool could gate this, but for now the automatic detection with fallback is sufficient.

---

## 10. Implementation Order

1. **ParseStreamJsonFinalText** -- Pure function, unit-testable in isolation. Write and test first.
2. **SetupPlannerSandbox** -- Simple file I/O, write and verify the sandbox directory is created correctly.
3. **RunPlannerWithTools** -- The main method. Start with the process spawn + read loop, test with a simple prompt before adding tool filter logic.
4. **ExecuteCLIPath modification** -- Wire in the new MCP path with fallback. Test the full flow.
5. **Tuning** -- Adjust `PLANNER_MAX_TURNS` (15) and `PLANNER_TIMEOUT_SECONDS` (180) based on real-world behavior.

---

## 11. Testing Plan

### Manual Testing

1. **Happy path**: Task with relevant templates. Verify Planner calls `get_template`, produces a plan with function descriptions based on observed patterns.
2. **No templates**: Task with no template matches. Verify Planner produces a plan without tool calls.
3. **Fallback**: Stop MCP server, verify `RunPlannerWithTools` returns `bSuccess=false` and `RunPlanner` takes over.
4. **Timeout**: Set `PLANNER_TIMEOUT_SECONDS` to 5s temporarily, verify graceful termination.
5. **Tool filter**: While Planner is running, verify `tools/list` returns only the 3 allowed tools via a separate MCP client.

### Log Markers

The implementation should produce clear log output:
```
[OliveAgentPipeline] CLI pipeline starting for message: "Create a melee combat system" (0 context assets)
[OliveAgentPipeline]   Router: SKIPPED (CLI mode, defaulted to Moderate)
[OliveAgentPipeline]   Scout (CLI, no LLM): 3 assets, discovery 2400 chars, template overviews 0 chars, 5 refs (0.3s)
[OliveAgentPipeline]   Planner (MCP): launched with 4200 char prompt, max 15 turns, PID=12345
[OliveAgentPipeline]   Planner (MCP): success, 1800 char plan, 3 assets, 25.3s
[OliveAgentPipeline]   Validator: 0 issues, blocking=no (0.001s)
[OliveAgentPipeline] CLI pipeline complete: valid=true, 25.6s total (1 LLM call with tools)
```

---

## 12. Comparison: Before and After

| Aspect | Before (single-shot) | After (MCP tools) |
|--------|----------------------|---------------------|
| Template data in prompt | 27-31K chars (structural overviews) | 3-5K chars (compact headers) |
| Template detail available | Structural summaries only | Full function node graphs (on demand) |
| Token cost | ~8K input tokens for templates | ~1K input + ~2K per tool call (~5K total) |
| Cold starts | 1 (CLI launch) | 1 (CLI launch) + mcp-bridge (~1-2s) |
| Total time | ~8-15s | ~15-30s (more tool round-trips) |
| Plan quality | Based on summaries | Based on real implementation patterns |
| Fallback | N/A | Automatic to single-shot |

The time increase is a trade-off: ~10-15s more for significantly better plan quality. The Planner's plan quality directly impacts the Builder's output quality, so this is a worthwhile trade.

---

## Summary for the Coder

**What to build:**
1. `ParseStreamJsonFinalText()` -- static method on `FOliveAgentPipeline`. Parses `stream-json` lines, extracts text blocks, finds the one with `## Build Plan`.
2. `SetupPlannerSandbox()` -- static method. Creates `Saved/OliveAI/PlannerSandbox/` with `.mcp.json` + `CLAUDE.md`.
3. `RunPlannerWithTools()` -- new private method. Spawns CLI with `--max-turns 15`, tick-pump read loop, tool filter, stream-json extraction.
4. Modify `ExecuteCLIPath()` -- try `RunPlannerWithTools` first, fall back to `RunPlanner` on failure.

**Key patterns to follow:**
- The process spawn + pipe management follows the existing Tier 3 code in `SendAgentCompletion()` (lines 528-665).
- The tick-pump pattern is `FTSTicker::GetCoreTicker().Tick(0.01f)` + `FPlatformProcess::Sleep(0.01f)` -- same as everywhere else.
- Tool filter: use `FOliveMCPServer::Get().SetToolFilter()` with exact tool names as "prefixes" (they are unique enough). Clear with `ClearToolFilter()` before returning.
- The sandbox `.mcp.json` format is identical to `FOliveClaudeCodeProvider::WriteProviderSpecificSandboxFiles()`.

**Do NOT:**
- Touch `FOliveCLIProviderBase` or `LaunchCLIProcess`.
- Modify `BuildPlannerSystemPrompt()` (tool instructions are appended inline in `RunPlannerWithTools`).
- Add new settings or config.
- Change the Scout's data collection (it already has everything needed).
