# Claude Code Integration Refactor -- Design Plan

**Author:** Architect Agent
**Date:** 2026-03-19
**Status:** Draft for review

## Summary

Transition Claude Code from an in-editor provider to an external MCP integration. Olive owns the MCP server, tools, validation, prompts, resources, and activity UI. Claude Code owns auth, model runtime, conversation, and agent loop. In-editor chat remains for API-based providers.

Seven phases, each independently shippable.

---

## Phase 1: Companion Panel + MCP Status UI + Activity Feed

**Goal:** Give users a dedicated panel for Claude Code integration -- MCP status, setup instructions, and live activity log.

### New Files

| File | Purpose |
|------|---------|
| `Source/OliveAIEditor/Public/UI/SOliveClaudeCodePanel.h` | Panel widget header |
| `Source/OliveAIEditor/Private/UI/SOliveClaudeCodePanel.cpp` | Panel widget implementation |

### Modified Files

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp` | Register second nomad tab, extend Tools menu |
| `Source/OliveAIEditor/Private/OliveAIEditorModule.h` | Add `ClaudeCodeTabId`, `SpawnClaudeCodeTab()` |
| `Source/OliveAIEditor/Public/MCP/OliveMCPServer.h` | Add `FOnMCPToolCompleted` delegate, `GetRecentToolCalls()` |
| `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp` | Fire tool-completed delegate, maintain recent-calls ring buffer |

### Interface: SOliveClaudeCodePanel

```cpp
// SOliveClaudeCodePanel.h
class OLIVEAIEDITOR_API SOliveClaudeCodePanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SOliveClaudeCodePanel) {}
    SLATE_END_ARGS()

    static const FName TabId;
    static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);

    void Construct(const FArguments& InArgs);
    virtual ~SOliveClaudeCodePanel();

private:
    // -- Section builders --
    TSharedRef<SWidget> BuildHeader();
    TSharedRef<SWidget> BuildServerStatus();    // Running/Stopped, port, client count
    TSharedRef<SWidget> BuildSetupInstructions();// Copyable .mcp.json, `claude mcp add` cmd
    TSharedRef<SWidget> BuildActivityFeed();     // Recent tool calls with timestamps

    // -- Status polling --
    /** Tick-driven status refresh (1Hz via FTimerHandle) */
    void RefreshStatus();
    FTimerHandle RefreshTimerHandle;

    // -- Activity feed --
    /** Handle tool call from MCP server (binds to OnToolCalled) */
    void HandleToolCalled(const FString& ToolName, const FString& ClientId,
                          const TSharedPtr<FJsonObject>& Arguments);
    /** Handle tool completion (binds to OnToolCompleted) */
    void HandleToolCompleted(const FString& ToolName, const FString& ClientId,
                             bool bSuccess, double DurationMs);

    // -- Clipboard helpers --
    FReply OnCopyMcpJson();
    FReply OnCopyMcpAddCommand();

    // -- State --
    struct FActivityEntry
    {
        FDateTime Timestamp;
        FString ToolName;
        FString ClientId;
        bool bSuccess = true;
        double DurationMs = 0.0;
        bool bCompleted = false; // false = in-flight
    };
    TArray<FActivityEntry> ActivityLog; // ring buffer, max 100
    static constexpr int32 MaxActivityEntries = 100;

    // -- Child widgets --
    TSharedPtr<SListView<TSharedPtr<FActivityEntry>>> ActivityListView;
    TArray<TSharedPtr<FActivityEntry>> ActivityItems; // for SListView
};
```

### MCP Server Additions

Add to `FOliveMCPServer`:

```cpp
// New delegate -- fires after tool execution completes
DECLARE_MULTICAST_DELEGATE_FourParams(FOnMCPToolCompleted,
    const FString& /* ToolName */,
    const FString& /* ClientId */,
    bool /* bSuccess */,
    double /* DurationMs */);

FOnMCPToolCompleted OnToolCompleted; // public, next to OnToolCalled

// Recent tool call record (for initial population of activity feed)
struct FRecentToolCall
{
    FDateTime Timestamp;
    FString ToolName;
    FString ClientId;
    bool bSuccess;
    double DurationMs;
};
TArray<FRecentToolCall> GetRecentToolCalls(int32 MaxCount = 50) const;

// Private:
TArray<FRecentToolCall> RecentToolCalls; // ring buffer, max 100
mutable FCriticalSection RecentToolCallsLock;
```

Fire `OnToolCompleted` at the end of `HandleToolsCallAsync` game-thread lambda (line ~834), right before `SendJsonResponse`. Fire on game thread (already there).

### Module Registration

In `OliveAIEditorModule.h`, add:
```cpp
static const FName ClaudeCodeTabId; // = "OliveClaudeCodeTab"
TSharedRef<SDockTab> SpawnClaudeCodeTab(const FSpawnTabArgs& Args);
```

In `RegisterUI()`, add a second `RegisterNomadTabSpawner` for `ClaudeCodeTabId`. In `ExtendToolsMenu()`, add a second menu entry "Olive AI -- Claude Code" below the chat entry. In `UnregisterUI()`, unregister the tab.

### Setup Instructions Content

The panel shows:
1. **Server status** -- green dot + "Running on port 3001" or red dot + "Stopped"
2. **Connected clients** -- count + names (from `GetConnectedClients()`)
3. **Setup: .mcp.json** -- formatted JSON block with copy button. Content: current `.mcp.json` with correct bridge path.
4. **Setup: CLI command** -- `claude mcp add olive-ai-studio -- node "<plugin_path>/mcp-bridge.js"` with copy button
5. **Activity feed** -- scrollable list, newest-first. Each row: timestamp, tool name, status icon (spinner/check/X), duration.

### Implementation Order [coder]

1. Add `FOnMCPToolCompleted` delegate + `RecentToolCalls` ring buffer to `FOliveMCPServer` (header + cpp)
2. Fire delegate in `HandleToolsCallAsync` completion path
3. Create `SOliveClaudeCodePanel.h` -- widget skeleton with `Construct()` and section builders
4. Implement `BuildServerStatus()` -- reads `FOliveMCPServer::Get().GetState()`, `GetActualPort()`, `GetConnectedClientCount()`
5. Implement `BuildSetupInstructions()` -- static text + copy buttons via `FPlatformApplicationMisc::ClipboardCopy()`
6. Implement `BuildActivityFeed()` -- `SListView` bound to `ActivityItems`, subscribe to `OnToolCalled` + `OnToolCompleted`
7. Register tab in module, add menu entry
8. Test: open panel, verify status display, connect Claude Code, verify activity feed populates

### Edge Cases

- MCP server not started: show "Server Stopped" with "Start Server" button that calls `FOliveMCPServer::Get().Start()`
- Bridge path varies per install: use `FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin("UE_Olive_AI_Studio")->GetBaseDir())` to resolve
- Panel opened before any MCP activity: populate from `GetRecentToolCalls()` on construct

---

## Phase 2: Tool Contract Hardening

**Goal:** Richer tool metadata and error payloads. Benefits ALL providers, not just Claude.

### Modified Files

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h` | Extend `FOliveToolDefinition` and `FOliveToolResult` |
| `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp` | Serialize new fields in `ToMCPJson()` and `ToJson()` |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` | Add `blueprint.verify_completion` schema |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Add `HandleVerifyCompletion()`, enrich plan_json error payloads |
| `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h` | Add `DetectUnwiredRequiredDataPins()` |
| `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp` | Implement unwired-pin detection in Stage 5 |

### FOliveToolDefinition Extensions

```cpp
// Add to FOliveToolDefinition:

/** Detailed usage guidance for AI agents. Multi-line. Appears in tools/list
 *  as "annotations.usage_guidance" (MCP spec extension). */
FString UsageGuidance;

/** When to use this tool vs alternatives. Helps AI pick the right tool. */
FString WhenToUse;
```

Serialization: In `ToMCPJson()`, if `UsageGuidance` is non-empty, add it under an `annotations` object:
```json
{
  "name": "blueprint.apply_plan_json",
  "description": "...",
  "annotations": {
    "usage_guidance": "Use for 3+ nodes of standard logic...",
    "when_to_use": "Standard function logic, event handlers, control flow"
  }
}
```

### FOliveToolResult Extensions

```cpp
// Add to FOliveToolResult:

/** Optional next-step guidance for the AI. Contextual suggestion
 *  based on what just happened. Serialized as "next_step" in JSON output. */
FString NextStepGuidance;
```

In `ToJson()` / `ToJsonString()`, serialize as `"next_step": "..."` when non-empty.

### blueprint.verify_completion Tool

New read tool that checks a Blueprint is "done":

**Schema:**
```json
{
  "asset_path": { "type": "string", "description": "Blueprint to verify" },
  "expected_functions": { "type": "array", "items": { "type": "string" }, "description": "Function names expected to exist" },
  "expected_variables": { "type": "array", "items": { "type": "string" }, "description": "Variable names expected to exist" }
}
```

**Handler logic** (`HandleVerifyCompletion`):
1. Load Blueprint, compile it
2. Check compile success (no errors)
3. Check no orphaned exec flows (reuse `DetectOrphanedExecFlows`)
4. Check expected functions exist and have non-trivial graphs (>1 node beyond entry/result)
5. Check expected variables exist
6. Check for unwired required data pins (new helper)
7. Return structured report: `{ "complete": bool, "issues": [...], "summary": "..." }`

Tags: `["read"]` -- this is a read-only verification tool.

### DetectUnwiredRequiredDataPins

Add to `FOliveWritePipeline`:
```cpp
/** Detect data input pins that are required but have no connection and no default.
 *  Skips: self pins, hidden pins, exec pins, pins with non-empty DefaultValue.
 *  @param Graph Graph to check
 *  @param OutMessages Warning messages
 *  @return Count of unwired required pins */
int32 DetectUnwiredRequiredDataPins(const UEdGraph* Graph, TArray<FOliveIRMessage>& OutMessages) const;
```

Call this in `StageVerify` alongside the existing orphaned-exec check, gated behind `bAutoCompile` (same condition). Also call from `HandleVerifyCompletion`.

### Plan JSON Error Enrichment

In `OlivePlanExecutor.cpp`, when a step fails during `PhaseCreateNodes` or `PhaseWireExec`/`PhaseWireData`, enrich the error with:
- `"failed_step_id"`: the step ID that failed
- `"failed_step_op"`: the op type
- `"suggestion"`: contextual fix (e.g., "Function not found on this class. Use blueprint.describe_function to check available functions.")

This is adding `NextStepGuidance` to the `FOliveWriteResult` returned by the executor lambda.

### Implementation Order

1. [coder] Extend `FOliveToolDefinition` -- add fields, update `ToMCPJson()`
2. [coder] Extend `FOliveToolResult` -- add `NextStepGuidance`, update `ToJson()`
3. [coder] Add `DetectUnwiredRequiredDataPins()` to write pipeline
4. [coder] Wire unwired-pin check into `StageVerify`
5. [coder] Add `blueprint.verify_completion` schema + handler + registration
6. [coder] Enrich plan_json error payloads with step context + suggestions
7. [creative_lead] Write `UsageGuidance` and `WhenToUse` text for all 40+ tools (batch task, can be done in parallel with code)

---

## Phase 3: CLAUDE.md Rewrite

**Goal:** Replace the current 300+ line CLAUDE.md (which targets in-editor usage) with a slim orientation doc for external Claude Code.

### Modified Files

| File | Change |
|------|--------|
| `CLAUDE.md` (plugin root) | Rewrite for external Claude Code orientation |

### Content Structure [creative_lead]

The new CLAUDE.md should contain ONLY:

1. **What is Olive AI Studio** (2-3 sentences) -- MCP server providing UE5 development tools
2. **How to connect** -- `.mcp.json` reference, `claude mcp add` command
3. **Available tool categories** -- one-line summary per category (blueprint, behaviortree, pcg, cpp, project, editor). NOT full tool listings.
4. **Key concepts** -- plan_json, templates, compile cycle (3-4 sentences each)
5. **Where to find more** -- "Use MCP prompts (prompts/get) for workflow guidance, MCP resources (resources/read) for domain knowledge"

Target: under 80 lines. Everything else moves to MCP prompts (Phase 4) or MCP resources (Phase 5).

### What Moves Where

| Current CLAUDE.md Content | New Home |
|--------------------------|----------|
| Tool schemas / parameters | Already in tools/list (built-in) |
| Blueprint workflow guidance | MCP prompt: `build_blueprint_feature` |
| Compile-fix workflow | MCP prompt: `fix_compile_errors` |
| Architecture patterns | MCP resource: `olive://knowledge/blueprint-patterns` |
| Event vs function rules | MCP resource: `olive://knowledge/events-vs-functions` |
| Pin wiring expectations | MCP resource: `olive://knowledge/pin-wiring` |
| Multi-asset patterns | MCP resource: `olive://knowledge/multi-asset` |

### Implementation Order

1. [creative_lead] Draft new CLAUDE.md
2. [creative_lead] Identify all content that moves to prompts/resources (input to Phases 4-5)

---

## Phase 4: MCP Workflow Prompts

**Goal:** Add workflow prompt templates that Claude fetches via `prompts/get` for task-specific guidance.

### Modified Files

| File | Change |
|------|--------|
| `Source/OliveAIEditor/CrossSystem/Private/OliveMCPPromptTemplates.cpp` | Add 6 new templates in `RegisterDefaultTemplates()` |

### New Prompt Templates

All registered in `RegisterDefaultTemplates()`. No new files needed.

#### 1. `start_task`
Meta-prompt that returns a workflow routing directive based on task type.

**Parameters:**
- `task_description` (required) -- what the user wants to do
- `task_type` (optional) -- hint: "create", "modify", "fix", "research"

**Template text:**
Routes to the right sub-workflow. Includes:
- Read-first mandate (always read before writing)
- Tool category overview (which tools for which job)
- Compile-after-write practice
- Points to other prompts for specific workflows

#### 2. `build_blueprint_feature`
**Parameters:**
- `feature_description` (required)
- `target_path` (optional) -- asset path if modifying existing
- `parent_class` (optional) -- e.g., "Character", "Actor"

**Template text:**
1. Search for related assets (`project.search`)
2. Check templates (`blueprint.list_templates`)
3. Read reference templates if relevant (`blueprint.get_template`)
4. Create or load Blueprint
5. Add variables/components first
6. Build function graphs with plan_json (preview then apply)
7. Compile and verify (`blueprint.compile`, `blueprint.verify_completion`)

#### 3. `modify_existing_blueprint`
**Parameters:**
- `asset_path` (required)
- `change_description` (required)

**Template text:**
1. Read current state (`blueprint.read mode=full`)
2. Identify target function/graph
3. Read specific function (`blueprint.describe_function`) if editing
4. Plan changes (plan_json preview)
5. Apply and compile
6. Verify no regressions

#### 4. `fix_compile_errors`
**Parameters:**
- `asset_path` (required)
- `error_context` (optional) -- paste of error message

**Template text:**
1. Read Blueprint (`blueprint.read mode=full`)
2. Compile to get fresh errors (`blueprint.compile`)
3. Read the failing function graph
4. Fix the FIRST error only (cascading errors)
5. Recompile
6. Repeat until clean

#### 5. `research_reference_patterns`
**Parameters:**
- `pattern_query` (required) -- what to look for
- `domain` (optional) -- "combat", "UI", "movement", etc.

**Template text:**
1. Search library templates (`blueprint.list_templates query="..."`)
2. Get template details for matches (`blueprint.get_template`)
3. Read function implementations for relevant patterns
4. Summarize findings before building

#### 6. `verify_and_finish`
**Parameters:**
- `asset_path` (required)
- `expected_functions` (optional)

**Template text:**
1. Run `blueprint.verify_completion`
2. If issues found, fix them
3. Final compile
4. Report done with summary of what was built

### Implementation Order [creative_lead + coder]

1. [creative_lead] Write template text for all 6 prompts (content task)
2. [coder] Add the 6 `RegisterTemplate()` calls in `RegisterDefaultTemplates()` -- straightforward, ~120 lines of template registration code

---

## Phase 5: MCP Resources (Domain Knowledge)

**Goal:** Serve domain knowledge as MCP resources that Claude fetches on demand via `resources/read`.

### Modified Files

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp` | Add new resources to `HandleResourcesList`, add handlers to `HandleResourcesRead` |

### New Resources

All served from existing `Content/SystemPrompts/Knowledge/` files. No new C++ classes needed -- just read files from disk.

| URI | Source File | Description |
|-----|------------|-------------|
| `olive://knowledge/events-vs-functions` | `Knowledge/events_vs_functions.txt` | When to use events vs functions in Blueprints |
| `olive://knowledge/blueprint-patterns` | `Knowledge/blueprint_design_patterns.txt` | Common Blueprint architecture patterns |
| `olive://knowledge/blueprint-authoring` | `Knowledge/blueprint_authoring.txt` | Blueprint authoring best practices |
| `olive://knowledge/node-routing` | `Knowledge/node_routing.txt` | How to pick the right node type |
| `olive://knowledge/pin-wiring` | Synthesized from `Knowledge/cli_blueprint.txt` (wiring section) | Pin wiring expectations and gotchas |
| `olive://knowledge/recipe/{name}` | `Knowledge/recipes/blueprint/{name}.txt` | Specific recipes (create, modify, fix_wiring, etc.) |

### Implementation Approach

In `HandleResourcesRead`, add a `olive://knowledge/` prefix handler:

```cpp
else if (Uri.StartsWith(TEXT("olive://knowledge/")))
{
    // Map URI to file path under Content/SystemPrompts/Knowledge/
    FString RelPath = Uri.RightChop(18); // strip "olive://knowledge/"
    RelPath.ReplaceInline(TEXT("/"), TEXT("\\"));  // normalize for FPaths

    // Check for recipe sub-path
    FString KnowledgeDir = FPaths::Combine(
        IPluginManager::Get().FindPlugin(TEXT("UE_Olive_AI_Studio"))->GetBaseDir(),
        TEXT("Content/SystemPrompts/Knowledge"));

    FString FilePath;
    if (RelPath.StartsWith(TEXT("recipe")))
    {
        // olive://knowledge/recipe/create -> recipes/blueprint/create.txt
        FString RecipeName = RelPath.RightChop(7); // strip "recipe/"
        FilePath = FPaths::Combine(KnowledgeDir, TEXT("recipes/blueprint"), RecipeName + TEXT(".txt"));
    }
    else
    {
        // olive://knowledge/events-vs-functions -> events_vs_functions.txt
        FString FileName = RelPath.Replace(TEXT("-"), TEXT("_"));
        FilePath = FPaths::Combine(KnowledgeDir, FileName + TEXT(".txt"));
    }

    if (FPaths::FileExists(FilePath))
    {
        FFileHelper::LoadFileToString(ContentText, *FilePath);
        MimeType = TEXT("text/plain");
    }
    else
    {
        // return resource-not-found error
    }
}
```

In `HandleResourcesList`, enumerate `olive://knowledge/*` resources. Two approaches:
- **Option A (recommended):** Hardcode the list with self-descriptive names and descriptions. Simple, stable, MCP clients see exactly what is available.
- Option B: Scan disk. More flexible but descriptions would be generic.

Use Option A. Add ~10 resource entries with good descriptions so Claude's resource selection is informed.

### Resource Descriptions (for HandleResourcesList)

```
olive://knowledge/events-vs-functions
  "When to implement logic as Event Graphs (BeginPlay, Tick, custom events) vs Function Graphs in Blueprints. Decision criteria and common mistakes."

olive://knowledge/blueprint-patterns
  "Reusable UE5 Blueprint architecture patterns: component composition, interface communication, event dispatchers, inheritance strategies."

olive://knowledge/blueprint-authoring
  "Blueprint authoring rules: naming conventions, variable organization, function decomposition, compile-error-first debugging."

olive://knowledge/node-routing
  "Guide for choosing the right Blueprint node type: K2Node subclasses, macro vs function, pure vs impure, latent actions."

olive://knowledge/recipe/create
  "Step-by-step recipe for creating a new Blueprint from scratch."

olive://knowledge/recipe/modify
  "Step-by-step recipe for modifying an existing Blueprint's logic."

olive://knowledge/recipe/fix_wiring
  "How to diagnose and fix pin wiring issues in Blueprint graphs."

olive://knowledge/recipe/spawn_actor
  "Pattern for spawning actors with class references and transform setup."

olive://knowledge/recipe/interface_pattern
  "Blueprint Interface implementation pattern: creating BPIs, adding to classes, calling interface messages."

olive://knowledge/recipe/multi_asset
  "Multi-asset workflow: creating related Blueprints, cross-asset references, build order."
```

### Implementation Order

1. [coder] Add knowledge resource entries to `HandleResourcesList()` (~60 lines)
2. [coder] Add `olive://knowledge/` handler to `HandleResourcesRead()` (~40 lines)
3. [creative_lead] Review/refine knowledge file content for external consumption (some files assume in-editor context)

---

## Phase 6: Remove Claude Code from Provider Dropdown

**Goal:** Gate Claude Code provider behind a legacy flag. Default: hidden.

### Modified Files

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` | Add `bEnableLegacyClaudeCodeProvider` |
| `Source/OliveAIEditor/Private/Providers/IOliveAIProvider.cpp` | Gate `GetAvailableProviders()` Claude Code entry |
| `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp` | No change needed (reads from factory) |

### Settings Addition

```cpp
// Add to UOliveAISettings, under "AI Provider" category:

/** Enable Claude Code as an in-editor chat provider (legacy).
 *  When disabled (default), Claude Code is used exclusively as an external MCP integration
 *  via the companion panel. The recommended workflow is: open Claude Code in your terminal,
 *  connect via MCP, and use the companion panel for monitoring. */
UPROPERTY(Config, EditAnywhere, Category="AI Provider",
    meta=(DisplayName="Enable Legacy Claude Code Provider"))
bool bEnableLegacyClaudeCodeProvider = false;
```

### Provider Factory Gate

In `FOliveProviderFactory::GetAvailableProviders()`:
```cpp
// Claude Code CLI - gated behind legacy flag
if (UOliveAISettings::Get()->bEnableLegacyClaudeCodeProvider)
{
    if (FOliveClaudeCodeProvider::IsClaudeCodeInstalled())
    {
        Providers.Add(TEXT("Claude Code CLI"));
    }
}
```

Also gate `CreateProvider()` -- if not enabled and provider name is "Claude Code", return nullptr (with log warning explaining the setting).

### Migration Path

- If user has `Provider = ClaudeCode` in saved config and flag is off: `PostInitProperties()` logs a warning and falls back to first available provider.
- Companion panel (Phase 1) shows a migration note: "Claude Code is now used as an external integration. Open Claude Code in your terminal and connect via MCP."

### Implementation Order [coder]

1. Add `bEnableLegacyClaudeCodeProvider` to settings
2. Gate `GetAvailableProviders()` + `CreateProvider()`
3. Add fallback logic in `PostInitProperties()` for existing configs
4. Add migration note text to companion panel

---

## Phase 7: Legacy Path Removal (Future Work)

**Goal:** Document what gets removed and the conditions for removal.

### Removal Conditions

Remove the legacy Claude Code provider path ONLY when:
1. External-only workflow has been validated for at least 4 weeks
2. No users report regressions via the legacy flag
3. Codex provider (which shares `FOliveCLIProviderBase`) is confirmed unaffected

### What Gets Removed

| Component | Files | Notes |
|-----------|-------|-------|
| `FOliveClaudeCodeProvider` | `OliveClaudeCodeProvider.h/.cpp` | ~450 lines. Thin wrapper around CLIProviderBase. |
| Claude Code entries in provider factory | `IOliveAIProvider.cpp` lines 137-141 | Remove create + display name mapping |
| `EOliveAIProvider::ClaudeCode` enum value | `OliveAISettings.h` line 15 | Breaking config change. Must migrate saved configs. |
| `bUseAutonomousMCPMode` setting | `OliveAISettings.h` line 159 | Only relevant to Claude Code in-editor. Codex has its own. |
| `bEnableLegacyClaudeCodeProvider` | `OliveAISettings.h` | Cleanup -- the gate itself |
| Claude Code-specific prompt routing | `OliveCLIProviderBase.cpp` | Review -- some may be Codex-shared |

### What Stays

| Component | Reason |
|-----------|--------|
| `FOliveCLIProviderBase` | Shared by Codex and future CLI agents |
| `mcp-bridge.js` | Still used for external Claude Code MCP connection |
| `FOliveMCPServer` | Core infrastructure |
| `SOliveClaudeCodePanel` | Becomes the primary Claude Code interface |
| All MCP prompts/resources | Serve external agents |

### Implementation Order

No code work for Phase 7 now. This section is a reference for when the team decides to execute it.

---

## Cross-Phase Integration Points

### Activity Feed Data Flow (Phase 1)

```
MCP HTTP Thread                    Game Thread                        UI
     |                                  |                              |
HandleToolsCallAsync ----AsyncTask----> ExecuteTool()                  |
     |                                  |                              |
     |                          OnToolCalled.Broadcast() -----------> HandleToolCalled()
     |                                  |                              |  (add in-flight entry)
     |                          [tool executes...]                     |
     |                                  |                              |
     |                          OnToolCompleted.Broadcast() --------> HandleToolCompleted()
     |                                  |                              |  (mark entry complete)
SendJsonResponse <---callback---------- |                              |
```

Both delegates fire on game thread. `SOliveClaudeCodePanel` subscribes in `Construct()`, unsubscribes in destructor.

### Tool Contract Data Flow (Phase 2)

```
tools/list  --> FOliveToolDefinition::ToMCPJson() --> includes annotations.usage_guidance
tools/call  --> FOliveToolResult::ToJson()        --> includes next_step when present
```

`NextStepGuidance` is set by:
- Plan executor on step failure (Phase 2)
- `blueprint.verify_completion` result (Phase 2)
- Individual tool handlers can set it contextually (future)

### Resource Discovery Flow (Phase 5)

```
Claude Code                          MCP Server
    |                                    |
    |--- resources/list ---------------->|
    |<-- [list with descriptions] -------|
    |                                    |
    |--- resources/read(uri) ----------->|
    |<-- [file content from disk] -------|
```

Resources are self-descriptive (good names + descriptions) so Claude can decide which to fetch based on the task.

---

## Migration Concerns

1. **Saved configs with Provider=ClaudeCode:** Phase 6 handles this with `PostInitProperties()` fallback. Users see a log warning + companion panel migration note.

2. **Existing .mcp.json files:** No change needed. External Claude Code already connects via mcp-bridge.js. The companion panel just makes setup easier.

3. **Autonomous mode setting:** `bUseAutonomousMCPMode` becomes meaningless once Claude Code is external-only (Phase 7). In Phase 6, it's ignored when the legacy flag is off.

4. **Tool filter (`SetToolFilter`):** Currently used by the in-editor autonomous mode to scope tool visibility. External Claude Code sees all tools. No change needed.

5. **Internal agent chat mode (`SetChatModeForInternalAgent`):** Only applies to in-editor autonomous runs. External Claude Code always gets Code mode. No change needed.

6. **`FOliveSelfCorrectionPolicy`:** Stays server-side. External Claude Code does its own retry logic; in-editor providers still use this. No change.
