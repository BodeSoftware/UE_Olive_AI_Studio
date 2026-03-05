# Utility Model + Template Pre-Search Design

**Date:** 2026-03-05
**Author:** Architect Agent
**Status:** Design Complete -- Ready for Implementation

---

## Problem Statement

The autonomous Claude Code agent consistently FAILS to search the library template system before building Blueprints, even when highly relevant templates exist (e.g., "bow and arrow" task when combatfs has bow/ranged templates). Root causes identified by 4 research reports:

1. `GetCatalogBlock()` is injected into the orchestrated `--append-system-prompt` (line 1480-1488 of `OliveCLIProviderBase.cpp`) but NOT into the autonomous sandbox CLAUDE.md
2. The stdin nudge (line 514) is generic -- "search blueprint.list_templates(query="...")" without any task-specific keywords
3. No system-level template retrieval -- the agent is expected to search on its own (every competitor does system-level retrieval)
4. AGENTS.md (394 lines, ~5,300 tokens) is copied to the sandbox and contains developer documentation irrelevant to the agent role

---

## Solution Overview

Five changes, ordered by implementation priority:

| # | Change | Impact | Files Changed |
|---|--------|--------|---------------|
| T1 | Utility Model Setting + API | Enables LLM keyword expansion | `OliveAISettings.h`, new `OliveUtilityModel.h/.cpp` |
| T2 | Template Pre-Search Injection | System-level retrieval before CLI launch | `OliveCLIProviderBase.cpp` |
| T3 | Catalog Block in Sandbox CLAUDE.md | Agent discovers template availability | `OliveCLIProviderBase.cpp` |
| T4 | AGENTS.md Deduplication | Saves ~5,300 tokens | `OliveCLIProviderBase.cpp` |
| T5 | Stronger Nudge Language | Imperative framing for remaining nudge | `OliveCLIProviderBase.cpp` |

**Dependencies:** T2 depends on T1 (uses utility model for keyword expansion, with fallback). T3/T4/T5 are independent. All can begin in parallel since T2's fallback path works without T1.

---

## T1: Utility Model Setting + API

### 1.1 New Settings (OliveAISettings.h)

Add a new "Utility Model" category after "Blueprint Plan" (line ~383). The utility model is a lightweight model for quick tasks like keyword expansion, error classification, etc.

```cpp
// ==========================================
// Utility Model Settings
// ==========================================

/** Provider for the utility model (used for keyword expansion, error classification, etc.)
 *  This should be a fast, cheap model. Leave as "None" to use regex-only keyword extraction. */
UPROPERTY(Config, EditAnywhere, Category="Utility Model",
    meta=(DisplayName="Utility Model Provider"))
EOliveAIProvider UtilityModelProvider = EOliveAIProvider::OpenRouter;

/** Model ID for utility tasks. Should be a fast/cheap model.
 *  Examples: "anthropic/claude-3-5-haiku-latest", "openai/gpt-4.1-nano", "google/gemini-2.0-flash" */
UPROPERTY(Config, EditAnywhere, Category="Utility Model",
    meta=(DisplayName="Utility Model ID"))
FString UtilityModelId = TEXT("anthropic/claude-3-5-haiku-latest");

/** API key override for the utility model (optional -- if empty, uses the key from the matching provider above).
 *  Useful if your utility model uses a different account or provider than your main model. */
UPROPERTY(Config, EditAnywhere, Category="Utility Model",
    meta=(DisplayName="Utility Model API Key (Optional)", PasswordField=true))
FString UtilityModelApiKey;

/** Timeout in seconds for utility model requests (keyword expansion, etc.) */
UPROPERTY(Config, EditAnywhere, Category="Utility Model",
    meta=(DisplayName="Utility Model Timeout (seconds)", ClampMin=5, ClampMax=30))
int32 UtilityModelTimeoutSeconds = 10;

/** Enable LLM-based keyword expansion for template pre-search.
 *  When disabled, falls back to basic tokenizer extraction (less accurate for synonyms). */
UPROPERTY(Config, EditAnywhere, Category="Utility Model",
    meta=(DisplayName="Enable LLM Keyword Expansion"))
bool bEnableLLMKeywordExpansion = true;
```

**Design rationale:**
- Uses the existing `EOliveAIProvider` enum, so users can pick any of the 8 supported providers
- API key defaults to the provider's existing key (no double-configuration needed)
- Default model `anthropic/claude-3-5-haiku-latest` is fast and cheap across all providers that carry it
- The setting is completely optional -- `bEnableLLMKeywordExpansion = false` or utility model failure both fall back to the basic tokenizer

### 1.2 Utility Model API (New Class)

**New files:**
- `Source/OliveAIEditor/Public/Services/OliveUtilityModel.h`
- `Source/OliveAIEditor/Private/Services/OliveUtilityModel.cpp`

The utility model provides a **synchronous-on-game-thread** simple completion API. It creates a transient provider instance, sends a single non-streaming request, and blocks until the response arrives (within the timeout). This is acceptable because:
- Utility calls happen before CLI launch (no UI freeze -- the chat panel already shows "Starting autonomous run...")
- The timeout is 10 seconds max
- Fallback to tokenizer is immediate if anything goes wrong

```cpp
// OliveUtilityModel.h

#pragma once

#include "CoreMinimal.h"

/**
 * FOliveUtilityModel
 *
 * Lightweight wrapper for quick single-shot LLM completions.
 * Used for keyword expansion, error classification, and other
 * sub-second utility tasks that don't need streaming or tool calling.
 *
 * NOT a singleton -- stateless helper class with static methods.
 * Each call creates a transient provider, sends one request, and waits.
 *
 * Thread safety: Must be called from the game thread (creates HTTP requests
 * via FHttpModule which requires game thread for delegate dispatch).
 */
class OLIVEAIEDITOR_API FOliveUtilityModel
{
public:
    /**
     * Send a simple completion request to the configured utility model.
     * Blocks the calling thread (game thread) until the response arrives
     * or the timeout expires.
     *
     * @param SystemPrompt   System-level instructions for the model
     * @param UserPrompt     The user-facing query
     * @param OutResponse    Filled with the model's text response on success
     * @param OutError       Filled with error message on failure
     * @return True if the completion succeeded, false on timeout/error/unconfigured
     */
    static bool SendSimpleCompletion(
        const FString& SystemPrompt,
        const FString& UserPrompt,
        FString& OutResponse,
        FString& OutError
    );

    /**
     * Check if the utility model is configured and available.
     * Returns false if the provider has no API key or the setting is disabled.
     */
    static bool IsAvailable();

    /**
     * Extract search keywords from a user message using the utility model.
     * Falls back to basic tokenizer extraction if the utility model is
     * unavailable or the request fails.
     *
     * @param UserMessage    The user's task description
     * @param MaxKeywords    Maximum number of keywords to return
     * @return Array of lowercase search terms (always non-empty -- fallback guarantees output)
     */
    static TArray<FString> ExtractSearchKeywords(
        const FString& UserMessage,
        int32 MaxKeywords = 12
    );

private:
    /**
     * Basic keyword extraction using the library index tokenizer.
     * Strips @-mentions, removes action verbs and noise words, returns tokens.
     * This is the fallback when the utility model is unavailable.
     */
    static TArray<FString> ExtractKeywordsBasic(const FString& UserMessage);

    /**
     * Build the keyword expansion prompt for the utility model.
     */
    static FString BuildKeywordExpansionPrompt(const FString& UserMessage);

    /** Action verbs to strip from user messages (not useful as search terms) */
    static const TSet<FString>& GetActionVerbStopWords();
};
```

### 1.3 Implementation Details

#### `IsAvailable()`

```
1. Get UOliveAISettings::Get()
2. Check bEnableLLMKeywordExpansion == true
3. Check UtilityModelProvider is NOT ClaudeCode (CLI providers can't do simple completions)
4. Resolve API key: UtilityModelApiKey if non-empty, else fall back to the provider's
   existing key (AnthropicApiKey for Anthropic, OpenRouterApiKey for OpenRouter, etc.)
5. Return true only if API key is non-empty and model ID is non-empty
```

#### `SendSimpleCompletion()`

```
1. Check IsAvailable() -- if false, set OutError and return false
2. Read settings: provider type, model ID, API key, timeout
3. Create a transient provider via FOliveProviderFactory::CreateProvider(ProviderName)
4. Configure it with: API key, model ID, Temperature=0.0, MaxTokens=256
5. Build a single-turn message array: [System(SystemPrompt), User(UserPrompt)]
6. Create a TSharedPtr<FThreadSafeBool> CompletionFlag
7. Call provider->SendMessage(Messages, EmptyTools, OnChunk, OnToolCall, OnComplete, OnError)
   where OnComplete captures the response text and sets CompletionFlag=true
8. Pump the game thread via FPlatformProcess::Sleep(0.01) + FTSTicker::GetCoreTicker().Tick()
   in a loop, up to TimeoutSeconds, checking CompletionFlag
9. If CompletionFlag set: OutResponse = captured text, return true
10. If timeout: provider->CancelRequest(), OutError = "Utility model timed out", return false
```

**Why synchronous with tick-pumping?** The utility model call happens during `SendMessageAutonomous()` which already runs on the game thread. We need the result before proceeding to `LaunchCLIProcess()`. Using `FTSTicker::GetCoreTicker().Tick()` allows HTTP callbacks to fire while we wait. This pattern is already used elsewhere in UE editor code (e.g., `FAssetToolsModule::Get().PromptToLoadOnStartup()`).

**Alternative considered: Async with completion callback.** This would require splitting `SendMessageAutonomous()` into two phases (before-search and after-search) with a continuation lambda. The complexity is not justified since the utility model call is sub-2-second in practice, and the UI already shows a "Starting..." state.

#### `ExtractSearchKeywords()`

```
1. Try LLM expansion first:
   a. Build system prompt (see below)
   b. Build user prompt from the user message
   c. Call SendSimpleCompletion with 10s timeout
   d. Parse response: split on commas/newlines, lowercase, trim, deduplicate
   e. If result has >= 3 keywords: return it

2. If LLM fails or returns < 3 keywords, fall back:
   a. Call ExtractKeywordsBasic(UserMessage)
   b. Return those tokens
```

#### `ExtractKeywordsBasic()` (Fallback)

```
1. Strip @-mentions: Replace "@" with " "
2. Tokenize using FOliveLibraryIndex::Tokenize() (public static method)
   - Splits on spaces/underscores/hyphens
   - Lowercases, drops < 2 chars, drops stop words, deduplicates
3. Remove action verb stop words: {"create", "build", "make", "add", "implement",
   "write", "modify", "change", "update", "fix", "remove", "delete", "set",
   "get", "wire", "connect"}
4. Return remaining tokens (capped at MaxKeywords)
```

#### LLM Keyword Expansion Prompt

System prompt:
```
You are a search keyword generator for an Unreal Engine Blueprint template library.
Given a user's task description, output 8-12 search keywords that would find relevant
Blueprint templates. Include:
- Direct terms from the task
- UE5 synonyms (e.g., "gun" -> "weapon", "fire", "projectile", "ammo")
- Related Blueprint concepts (e.g., "door" -> "interactable", "overlap", "timeline")
- Component types (e.g., "health" -> "stat", "damage", "combat")
Output ONLY a comma-separated list of lowercase keywords. No explanations.
```

User prompt: `{UserMessage}`

Example input: "create a bow and arrow system for @BP_ThirdPersonCharacter"
Example LLM output: "bow, arrow, ranged, weapon, fire, projectile, quiver, combat, character, component, aim"

Example input: "make a health system with damage and regeneration"
Example LLM output: "health, damage, regeneration, stat, combat, component, heal, buff, status, hit"

### 1.4 Error Handling

| Failure | Handling |
|---------|----------|
| Utility model not configured | `IsAvailable()` returns false, fallback to basic tokenizer |
| API key invalid / 401 | `SendSimpleCompletion` returns false, fallback to basic tokenizer |
| Timeout (>10s) | Cancel request, fallback to basic tokenizer |
| Empty/gibberish response | Response has < 3 keywords after parsing, fallback to basic tokenizer |
| Network error | `OnError` fires, fallback to basic tokenizer |
| Provider creation fails | `CreateProvider` returns nullptr, fallback to basic tokenizer |

**Key guarantee:** `ExtractSearchKeywords()` ALWAYS returns a non-empty array. The basic tokenizer fallback is deterministic and always produces tokens from any non-empty input.

---

## T2: Template Pre-Search Injection

### 2.1 Integration Point

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`
**Location:** Lines 509-515 in `SendMessageAutonomous()` -- replace the existing generic nudge block.

The current code at lines 509-515:
```cpp
// Nudge pattern research in the imperative channel (stdin).
// CLAUDE.md workflow steps are treated as optional; stdin directives are followed
// more reliably. Only inject on initial messages, not continuations.
if (!IsContinuationMessage(UserMessage))
{
    EffectiveMessage += TEXT("\n\nBefore building, research patterns from Library templates: search blueprint.list_templates(query=\"...\") for proven reference patterns from real projects, specific function templates may be available by blueprint.get_template(id, pattern=\"FuncName\") to study matching functions. Supplement with olive.search_community_blueprints if needed -- community examples are mixed quality, compare several before using. These are references -- adapt, simplify, or combine patterns to fit the user's needs. Then build the complete system using the MCP tools.\n");
}
```

### 2.2 New Code (Replaces Lines 509-515)

```cpp
// Pre-search template library for task-relevant patterns and inject results
// into the imperative channel (stdin). This replaces the generic "go search"
// nudge with actual search results, so the agent has specific templates to
// reference immediately. Only inject on initial messages, not continuations.
if (!IsContinuationMessage(UserMessage))
{
    // Extract search keywords -- uses LLM expansion if utility model is
    // configured, falls back to basic tokenizer extraction otherwise.
    TArray<FString> Keywords = FOliveUtilityModel::ExtractSearchKeywords(UserMessage, 12);
    FString SearchQuery = FString::Join(Keywords, TEXT(" "));

    UE_LOG(LogOliveCLIProvider, Log,
        TEXT("Template pre-search keywords: %s"), *SearchQuery);

    TArray<TSharedPtr<FJsonObject>> TemplateSuggestions;
    if (!SearchQuery.IsEmpty())
    {
        TemplateSuggestions = FOliveTemplateSystem::Get().SearchTemplates(SearchQuery, 8);
    }

    if (TemplateSuggestions.Num() > 0)
    {
        EffectiveMessage += TEXT("\n\n## Relevant Library Templates (Pre-searched)\n\n");
        EffectiveMessage += TEXT("These templates match your task. BEFORE building, call ");
        EffectiveMessage += TEXT("`blueprint.get_template(template_id=\"...\", pattern=\"FuncName\")` ");
        EffectiveMessage += TEXT("on at least one relevant function to study real implementation patterns.\n\n");

        for (const TSharedPtr<FJsonObject>& Entry : TemplateSuggestions)
        {
            FString Id, Desc, SourceProj, ParentClass;
            Entry->TryGetStringField(TEXT("template_id"), Id);
            Entry->TryGetStringField(TEXT("catalog_description"), Desc);
            Entry->TryGetStringField(TEXT("source_project"), SourceProj);
            Entry->TryGetStringField(TEXT("parent_class"), ParentClass);

            EffectiveMessage += FString::Printf(TEXT("- **%s**"), *Id);
            if (!SourceProj.IsEmpty())
            {
                EffectiveMessage += FString::Printf(TEXT(" [%s]"), *SourceProj);
            }
            if (!ParentClass.IsEmpty())
            {
                EffectiveMessage += FString::Printf(TEXT(" (parent: %s)"), *ParentClass);
            }
            if (!Desc.IsEmpty())
            {
                EffectiveMessage += FString::Printf(TEXT(": %s"), *Desc);
            }
            EffectiveMessage += TEXT("\n");

            // Show matched function names -- the most actionable signal
            const TArray<TSharedPtr<FJsonValue>>* MatchedFuncs;
            if (Entry->TryGetArrayField(TEXT("matched_functions"), MatchedFuncs) && MatchedFuncs)
            {
                TArray<FString> FuncNames;
                for (const TSharedPtr<FJsonValue>& FuncVal : *MatchedFuncs)
                {
                    TSharedPtr<FJsonObject> FuncObj = FuncVal->AsObject();
                    FString FuncName;
                    if (FuncObj && FuncObj->TryGetStringField(TEXT("name"), FuncName))
                    {
                        FuncNames.Add(FuncName);
                    }
                }
                if (FuncNames.Num() > 0)
                {
                    EffectiveMessage += FString::Printf(
                        TEXT("  Functions: %s\n"),
                        *FString::Join(FuncNames, TEXT(", ")));
                }
            }
        }

        EffectiveMessage += TEXT("\n**REQUIRED**: Study at least one relevant function above ");
        EffectiveMessage += TEXT("before writing any plan_json. Use ");
        EffectiveMessage += TEXT("`blueprint.get_template(template_id=\"<id>\", pattern=\"<FuncName>\")` ");
        EffectiveMessage += TEXT("to read specific functions. These are references -- adapt ");
        EffectiveMessage += TEXT("patterns to fit the task, do not copy blindly.\n");
    }
    else
    {
        // No pre-search results -- fall back to generic nudge
        EffectiveMessage += TEXT("\n\nBefore building, search for relevant patterns: ");
        EffectiveMessage += TEXT("call `blueprint.list_templates(query=\"...\")` with task-specific keywords. ");
        EffectiveMessage += TEXT("Study matching functions with `blueprint.get_template(template_id, pattern=\"FuncName\")` ");
        EffectiveMessage += TEXT("before writing plan_json.\n");
    }
}
```

### 2.3 Token Cost

Pre-search block with 8 results: ~400-500 tokens (header + 8 template entries with descriptions and function names + footer instruction).

Fallback (no results): ~60 tokens.

Both are negligible against a 200k context window.

### 2.4 New Include

Add to `OliveCLIProviderBase.cpp` includes (after line 16):
```cpp
#include "Services/OliveUtilityModel.h"
```

---

## T3: Catalog Block in Sandbox CLAUDE.md

### 3.1 Integration Point

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`
**Function:** `SetupAutonomousSandbox()`, lines 392-412 (after the design patterns knowledge pack)

### 3.2 New Code (Insert After Line 412)

Insert immediately after the `DesignPatterns` append block (line 412, `ClaudeMd += TEXT("\n\n");`) and before the CLAUDE.md file write (line 414):

```cpp
// Append template catalog block so the agent knows what templates exist.
// The orchestrated path gets this via BuildCLISystemPrompt() -> GetCatalogBlock().
// Without this, the autonomous agent has no awareness of available templates.
if (FOliveTemplateSystem::Get().HasTemplates())
{
    const FString& Catalog = FOliveTemplateSystem::Get().GetCatalogBlock();
    if (!Catalog.IsEmpty())
    {
        ClaudeMd += TEXT("---\n\n");
        ClaudeMd += Catalog;
        ClaudeMd += TEXT("\n\n");
    }
}
```

### 3.3 Token Cost

The catalog block is compact -- approximately 750-1,500 tokens depending on how many factory/reference/library projects exist. The combatfs library contributes a single line: "combatfs: 325 templates (2,847 functions total)". Factory templates list names + descriptions + extractable functions. This is the same block already injected in the orchestrated path and is well within budget.

---

## T4: AGENTS.md Deduplication

### 4.1 Current Behavior

Lines 375-424 of `SetupAutonomousSandbox()`:
1. Loads `AGENTS.md` from plugin root (line 376-377)
2. Writes it to `AgentSandbox/AGENTS.md` (lines 420-424)

The plugin's AGENTS.md is 394 lines of developer-scoped documentation (~5,300 tokens) containing build commands, architecture docs, coding standards, and file locations. It is described in the plugin's own CLAUDE.md as "a stale near-copy of this file (minus the Subagent section)" that "can be safely deleted."

The sandbox CLAUDE.md already contains the correct agent-role instructions, knowledge packs, and (after T3) the template catalog. AGENTS.md adds nothing useful and wastes 5,300 tokens.

### 4.2 Change

**Remove** the AGENTS.md loading and writing entirely. Delete lines 375-424 that handle AgentsContent:

**Lines to remove:**

1. Lines 375-377 (loading AGENTS.md):
```cpp
FString AgentsContent;
const FString AgentsPath = FPaths::Combine(PluginDir, TEXT("AGENTS.md"));
FFileHelper::LoadFileToString(AgentsContent, *AgentsPath);
```

2. Lines 417-424 (writing AGENTS.md to sandbox):
```cpp
// --- Write AGENTS.md (copy from plugin dir) ---
// Claude Code reads AGENTS.md for agent-specific workflow guidance.
// The plugin's AGENTS.md is already written for the agent role (tool usage, plan JSON, etc.)
if (!AgentsContent.IsEmpty())
{
    const FString SandboxAgentsPath = FPaths::Combine(AutonomousSandboxDir, TEXT("AGENTS.md"));
    FFileHelper::SaveStringToFile(AgentsContent, *SandboxAgentsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
```

**Net effect:** ~5,300 tokens freed from the agent's context window. The sandbox CLAUDE.md retains all operational guidance.

---

## T5: Stronger Nudge Language

### 5.1 Context

The pre-search injection (T2) already includes strong imperative language ("REQUIRED: Study at least one relevant function"). However, the FALLBACK nudge (when no pre-search results are found) should also use imperative framing.

The T2 fallback nudge is already updated in the code above (section 2.2). Additionally, the template catalog (T3) in CLAUDE.md should include a brief imperative note.

### 5.2 Catalog Block Wrapper

After inserting the catalog block (T3), add an imperative wrapper. Modify the T3 injection to:

```cpp
if (FOliveTemplateSystem::Get().HasTemplates())
{
    const FString& Catalog = FOliveTemplateSystem::Get().GetCatalogBlock();
    if (!Catalog.IsEmpty())
    {
        ClaudeMd += TEXT("---\n\n");
        ClaudeMd += Catalog;
        ClaudeMd += TEXT("\n");
        ClaudeMd += TEXT("**IMPORTANT**: Before building graph logic, search library templates ");
        ClaudeMd += TEXT("with `blueprint.list_templates(query=\"...\")` and study matching ");
        ClaudeMd += TEXT("functions with `blueprint.get_template(template_id, pattern=\"FuncName\")`. ");
        ClaudeMd += TEXT("Real project patterns prevent common mistakes.\n\n");
    }
}
```

This places the imperative at the END of the catalog block, which is the position research shows is most reliably read by LLMs ("end of context" position in the CLAUDE.md file).

---

## Data Flow Diagram

```
User Message: "create a bow and arrow system for @BP_ThirdPersonCharacter"
    |
    v
FOliveConversationManager::SendUserMessageAutonomous()
    |
    v
FOliveCLIProviderBase::SendMessageAutonomous(UserMessage)
    |
    |-- SetupAutonomousSandbox()
    |       |-- Write .mcp.json
    |       |-- Write CLAUDE.md:
    |       |     - Role preamble (~100 tokens)
    |       |     - cli_blueprint.txt (~600 tokens)
    |       |     - recipe_routing.txt (~200 tokens)
    |       |     - blueprint_design_patterns.txt (~1,300 tokens)
    |       |     - [NEW] Template catalog block (~750-1,500 tokens)
    |       |     - [NEW] Imperative nudge (~50 tokens)
    |       |-- [REMOVED] No AGENTS.md copy (~-5,300 tokens)
    |
    |-- BuildContinuationPrompt (if continuation -- skip pre-search)
    |
    |-- Inject @-mention asset state (if any)
    |
    |-- [NEW] Template Pre-Search:
    |       |
    |       |-- FOliveUtilityModel::ExtractSearchKeywords(UserMessage)
    |       |     |
    |       |     |-- (if utility model available):
    |       |     |     LLM prompt: "extract search keywords for UE template library"
    |       |     |     Response: "bow, arrow, ranged, weapon, fire, projectile, quiver, combat"
    |       |     |
    |       |     |-- (fallback -- utility model unavailable or failed):
    |       |     |     FOliveLibraryIndex::Tokenize(UserMessage.Replace("@", " "))
    |       |     |     Result: ["bow", "arrow", "system", "bp", "thirdpersoncharacter"]
    |       |     |     Filter action verbs: ["bow", "arrow", "thirdpersoncharacter"]
    |       |     |
    |       |     v
    |       |     Keywords: ["bow", "arrow", "ranged", "weapon", ...]
    |       |
    |       |-- FOliveTemplateSystem::Get().SearchTemplates(keywords, 8)
    |       |     Returns: combatfs_ranged_component, combatfs_bp_arrow_parent,
    |       |              combatfs_bp_ranged_character, ...
    |       |
    |       |-- Format results as Markdown block + imperative instruction
    |       |-- Append to EffectiveMessage
    |
    v
LaunchCLIProcess(CLIArgs, EffectiveMessage, SandboxDir)
    |
    v
Claude Code receives stdin:
  "create a bow and arrow system for @BP_ThirdPersonCharacter

   ### Current Asset State
   **BP_ThirdPersonCharacter** (parent: Character)
   - Components: ...
   ...

   ## Relevant Library Templates (Pre-searched)

   These templates match your task. BEFORE building, call
   `blueprint.get_template(template_id="...", pattern="FuncName")` ...

   - **combatfs_ranged_component** [combatfs] (parent: ActorComponent): Handles ranged combat...
     Functions: FireArrow, DrawBow, ReleaseArrow, HandleAmmo
   - **combatfs_bp_arrow_parent** [combatfs] (parent: Actor): Arrow projectile base...
     Functions: InitArrow, OnHit, ApplyDamage
   ...

   **REQUIRED**: Study at least one relevant function above..."
```

---

## File-by-File Change List

### New Files

| File | Purpose |
|------|---------|
| `Source/OliveAIEditor/Public/Services/OliveUtilityModel.h` | Utility model API header |
| `Source/OliveAIEditor/Private/Services/OliveUtilityModel.cpp` | Utility model implementation |

### Modified Files

| File | Changes |
|------|---------|
| `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` | Add 5 UPROPERTY fields for utility model (after line ~383) |
| `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | 4 changes (see below) |

### OliveCLIProviderBase.cpp Changes (Detailed)

**Change 1: New include** (after line 16)
```cpp
#include "Services/OliveUtilityModel.h"
```

**Change 2: SetupAutonomousSandbox -- Remove AGENTS.md** (lines 375-377, 417-424)
- Delete `FString AgentsContent;` and the load call (lines 375-377)
- Delete the `AGENTS.md` write block (lines 417-424)

**Change 3: SetupAutonomousSandbox -- Add catalog block** (after line 412)
- Insert the catalog block + imperative wrapper (see T3 + T5 section above)
- Insert between the last knowledge pack append and the CLAUDE.md file write

**Change 4: SendMessageAutonomous -- Replace nudge with pre-search** (lines 509-515)
- Replace the entire `if (!IsContinuationMessage(UserMessage))` block with the pre-search injection code (see T2 section above)

---

## Implementation Order

1. **T4 (AGENTS.md dedup)** -- 5 minutes. Delete 2 blocks, immediate token savings. Zero risk.
2. **T3 + T5 (Catalog block + nudge)** -- 15 minutes. Insert one block with imperative text. Low risk.
3. **T1 (Utility model settings + API)** -- 2-3 hours. New header + impl. Requires HTTP tick-pumping pattern. Medium complexity.
4. **T2 (Pre-search injection)** -- 30 minutes. Depends on T1 but has fallback. Medium risk (the fallback tokenizer works without T1).

**Recommendation for coder:** Implement T4 -> T3+T5 -> T2 (with basic fallback only) -> T1 -> update T2 to use T1. This way, T2-T5 can ship immediately with the basic tokenizer fallback, and T1 adds LLM keyword expansion as an enhancement.

---

## Edge Cases

### Edge Case 1: Empty user message
`ExtractSearchKeywords("")` returns empty via both LLM and basic paths. `SearchTemplates("")` returns empty. The fallback generic nudge fires. No crash.

### Edge Case 2: "continue" / "keep going"
`IsContinuationMessage()` returns true, skipping the entire pre-search block. Continuations use `BuildContinuationPrompt()` which already has its own context injection. No pre-search needed.

### Edge Case 3: Utility model returns garbage
If the LLM returns "I'd be happy to help you with that!" instead of keywords, the comma-split + filter produces < 3 tokens, triggering fallback to basic tokenizer.

### Edge Case 4: No templates loaded
`FOliveTemplateSystem::Get().HasTemplates()` returns false. The catalog block is not injected into CLAUDE.md. `SearchTemplates()` returns empty. The fallback generic nudge fires.

### Edge Case 5: ClaudeCode selected as utility provider
`IsAvailable()` specifically rejects `EOliveAIProvider::ClaudeCode` because CLI providers cannot do simple non-streaming HTTP completions. Returns false, fallback to basic tokenizer.

### Edge Case 6: Ollama local model as utility
Works. The Ollama provider uses HTTP just like the others. The user sets `UtilityModelProvider = Ollama` and `UtilityModelId = "llama3.2:3b"`. The transient provider is configured with the Ollama URL from settings.

### Edge Case 7: Same provider for main + utility
Common case. User has `Provider = OpenRouter` for main and `UtilityModelProvider = OpenRouter` for utility. The utility model API key fallback reads `OpenRouterApiKey` from settings. No double-configuration needed.

### Edge Case 8: Rate limiting on utility model
If the utility model request hits a rate limit (429), `OnError` fires with the error message, `SendSimpleCompletion` returns false, and the fallback tokenizer fires. The main agent launch proceeds without delay.

---

## Token Budget Impact

| Component | Before | After | Delta |
|-----------|--------|-------|-------|
| Sandbox CLAUDE.md (role + knowledge) | ~2,200 | ~2,200 | 0 |
| AGENTS.md | ~5,300 | 0 | **-5,300** |
| Catalog block (CLAUDE.md) | 0 | ~1,000 | +1,000 |
| Imperative nudge (CLAUDE.md) | 0 | ~50 | +50 |
| Pre-search injection (stdin) | ~100 | ~450 | +350 |
| **Total context change** | | | **-3,900** |

Net savings of approximately 3,900 tokens even after adding the catalog block and pre-search results.

---

## Future Enhancements (Not in Scope)

1. **Static synonym map**: Add a hardcoded synonym expansion (e.g., "gun" -> ["weapon", "fire", "shoot"]) that runs alongside or instead of the LLM expansion. Lower latency, zero cost, but limited vocabulary.

2. **Embedding-based search**: Replace the inverted keyword index with vector embeddings for semantic matching. Requires an embedding model call per search -- more expensive but handles conceptual similarity ("make it explode" -> damage/VFX templates).

3. **Per-turn context refresh**: Re-run pre-search after each agent turn to surface templates relevant to what the agent is currently doing. Requires MCP server integration to inject context into tool results.

4. **Utility model for error classification**: Use the utility model to classify tool errors into actionable categories. The `EOliveErrorCategory` enum + `BuildToolErrorMessage()` in `OliveSelfCorrectionPolicy.cpp` could benefit from LLM-based error analysis.
