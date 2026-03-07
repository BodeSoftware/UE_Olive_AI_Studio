# Pipeline Quality Fix Design

**Status:** Ready for implementation
**Author:** Architect Agent
**Date:** 2026-03-07
**Problem:** Builder produces PrintString-only logic because (1) the Architect never sees real template implementations, and (2) the Builder's execution directive suppresses template research.

---

## 1. Problem Diagnosis

### Root Cause 1: Architect works from summaries, not implementations

The Scout calls `FOliveUtilityModel::RunDiscoveryPass()` which returns `FOliveDiscoveryResult` with entries like:

```
- bp_melee_component: UActorComponent. Functions: MeleeAttack, SetupTrace, CalculateDamage (12 total)
```

This tells the Architect that a melee component exists and has a `MeleeAttack` function, but not HOW `MeleeAttack` is built (what nodes, what flow, what patterns). The Architect writes function descriptions like "call SetActorLocation" instead of describing real damage calculation chains with line traces, hit results, and apply-damage flows.

### Root Cause 2: Builder execution directive kills research agency

The old system's prompt told the Builder to "use `list_templates`/`get_template` for implementation patterns." The pipeline's `FormatForPromptInjection()` replaced this with:

```
Follow the Build Plan above. For each asset in Order:
1. Create structure (components, variables, interfaces, dispatchers)
2. Write ALL graph logic with apply_plan_json for every function/event
3. Compile to 0 errors before moving to the next asset
Do not stop until every asset in the plan is fully built and compiled.
```

This gives the Builder zero encouragement to research real implementations. It blindly follows a plan that was itself written without reference material.

---

## 2. Solution Overview

Three coordinated changes, all in existing files (no new files):

| Change | File | What |
|--------|------|------|
| A | `OliveAgentConfig.h` | Add `TemplateContent` field to `FOliveScoutResult` |
| B | `OliveAgentPipeline.cpp` | Scout auto-loads top template functions; Architect receives them; execution directive encourages research |
| C | `OliveAISettings.cpp` | `GetAgentModelConfig()` tries utility model before CLI fallback |

### What NOT to change

- Pipeline structure (Router -> Scout -> Researcher -> Architect -> Validator)
- `FOliveDiscoveryResult` or `FOliveUtilityModel` (those are shared infrastructure)
- The Architect system prompt's Build Plan format schema
- `OliveAgentPipeline.h` method signatures (no new public API)
- Any tool handlers, schemas, or write pipeline code

---

## 3. Change A: New field on `FOliveScoutResult`

**File:** `Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h`

Add one field to `FOliveScoutResult`:

```cpp
struct OLIVEAIEDITOR_API FOliveScoutResult
{
    // ... existing fields ...

    /**
     * Implementation reference content auto-loaded from top library template matches.
     * Contains actual function graph data (nodes, connections, pin values) for
     * 1-2 key functions from the most relevant library templates.
     * Pure C++ operation (no LLM call). Typically 200-800 tokens.
     * Empty if no library templates matched or discovery was disabled.
     */
    FString TemplateContent;

    // ... existing fields ...
};
```

**Placement:** After `DiscoveryBlock`, before `bSuccess`.

**Rationale:** Keeping this separate from `DiscoveryBlock` (which is summaries) lets the Architect's prompt clearly distinguish "here are summaries" from "here is actual implementation reference." It also lets `FormatForPromptInjection()` decide whether to include it in the Builder's prompt without re-parsing.

---

## 4. Change B: `OliveAgentPipeline.cpp` modifications

### B1: Scout auto-loads template content (~30 lines)

**Location:** `RunScout()`, after line 847 (`Result.DiscoveryBlock = FOliveUtilityModel::FormatDiscoveryForPrompt(DiscoveryResult);`), before Part 2 (project index search).

**Pseudocode:**

```cpp
// Part 1.5: Auto-load key function content from top library template matches.
// This is a pure C++ operation (lazy-load JSON + format) -- no LLM call, ~0ms.
{
    const FOliveLibraryIndex& LibIndex = FOliveTemplateSystem::Get().GetLibraryIndex();

    if (LibIndex.IsInitialized())
    {
        // Budget: max 2 templates, max 2 functions each, to stay under ~800 tokens
        static constexpr int32 MAX_TEMPLATE_CONTENT_TEMPLATES = 2;
        static constexpr int32 MAX_TEMPLATE_CONTENT_FUNCTIONS = 2;

        int32 TemplatesLoaded = 0;
        FString ContentBlock;

        for (const FOliveDiscoveryEntry& Entry : DiscoveryResult.Entries)
        {
            if (TemplatesLoaded >= MAX_TEMPLATE_CONTENT_TEMPLATES)
            {
                break;
            }

            // Only auto-load library templates (factory/reference don't have node data)
            if (Entry.SourceType != TEXT("library"))
            {
                continue;
            }

            // Only load if there are matched functions (means the search was targeted)
            if (Entry.MatchedFunctions.Num() == 0)
            {
                continue;
            }

            // Load top matched functions
            int32 FunctionsLoaded = 0;
            for (const FString& FuncName : Entry.MatchedFunctions)
            {
                if (FunctionsLoaded >= MAX_TEMPLATE_CONTENT_FUNCTIONS)
                {
                    break;
                }

                FString FuncContent = LibIndex.GetFunctionContent(Entry.TemplateId, FuncName);

                // Skip not-found results
                if (FuncContent.IsEmpty() || FuncContent.StartsWith(FOliveLibraryIndex::GetFuncNotFoundSentinel()))
                {
                    continue;
                }

                if (ContentBlock.IsEmpty())
                {
                    ContentBlock += TEXT("## Implementation Reference\n\n");
                    ContentBlock += TEXT("These are real function implementations from library templates. ");
                    ContentBlock += TEXT("Study the node patterns, not just the names.\n\n");
                }

                ContentBlock += FuncContent;
                ContentBlock += TEXT("\n\n");
                FunctionsLoaded++;
            }

            if (FunctionsLoaded > 0)
            {
                TemplatesLoaded++;
            }
        }

        Result.TemplateContent = ContentBlock;

        if (!ContentBlock.IsEmpty())
        {
            UE_LOG(LogOliveAgentPipeline, Log,
                TEXT("  Scout: auto-loaded %d template function(s), %d chars"),
                TemplatesLoaded, ContentBlock.Len());
        }
    }
}
```

**Key design decisions:**

1. **Budget caps** (2 templates, 2 functions each): `GetFunctionContent()` returns node-level graph data -- typically 200-400 tokens per function. 4 functions max = ~800-1600 tokens, well within the Architect's context window.

2. **Library-only filter**: Factory templates have parameterized plan JSON (not real graph data). Reference templates have descriptive text (no nodes). Only library templates have the full node-level implementation data that the Architect needs.

3. **MatchedFunctions gate**: If `MatchedFunctions` is empty, the discovery hit was a broad keyword match (e.g., "melee" matched a tag). Without specific function names, we would have to guess which functions to load. Skip these and let the Builder use `get_template` interactively.

4. **No LLM call**: This is a pure C++ lazy-load from the `FOliveLibraryIndex` JSON cache. Sub-millisecond on cache hits, ~5ms on cold loads from disk.

### B2: Architect receives template content (~5 lines)

**Location:** `RunArchitect()`, after the template references block (line 1181), before "Send to LLM" (line 1183).

**Insert:**

```cpp
// Implementation reference content (actual function graphs from library templates)
if (!ScoutResult.TemplateContent.IsEmpty())
{
    ArchitectUserPrompt += TEXT("\n");
    ArchitectUserPrompt += ScoutResult.TemplateContent;
    ArchitectUserPrompt += TEXT("\n");
}
```

**Placement matters:** This goes AFTER the discovery summaries and AFTER the researcher analysis, so it appears as the deepest-detail reference material right before the Architect generates its plan.

### B3: Updated Architect system prompt (~10 lines)

**Location:** `BuildArchitectSystemPrompt()`, append after the existing `## Rules` section (before the closing `)`).

**Append to the return string:**

```
\n
- If Implementation Reference content is provided, study the function graph patterns
  (node types, wiring flow, variable usage) and base your function descriptions on
  those real patterns. Do not simplify to PrintString unless the user explicitly asks for stubs.
- When describing function logic, reference specific patterns you observed
  (e.g., "line trace -> branch on hit -> apply damage" not "deal damage to target")
```

**Rationale:** The Architect is a lightweight agent (2048 max tokens, 30s timeout). The instruction must be short and directive. "Study the function graph patterns" is the key behavioral change -- it tells the Architect to derive plan details from observed implementations rather than from general UE knowledge.

### B4: Updated execution directive in `FormatForPromptInjection()` (~15 lines)

**Location:** `FormatForPromptInjection()`, replace lines 362-368 (Section 5: Execution directive).

**Replace with:**

```cpp
// Section 5: Execution directive
Output += TEXT("## Execution\n\n");
Output += TEXT("Follow the Build Plan above. For each asset in Order:\n");
Output += TEXT("1. Create structure (components, variables, interfaces, dispatchers)\n");
Output += TEXT("2. For each function/event:\n");
Output += TEXT("   a. If library templates were referenced above, use `blueprint.get_template(template_id, pattern=\"FunctionName\")` ");
Output += TEXT("to study how similar functions are built. Base your plan_json on real node patterns.\n");
Output += TEXT("   b. Write graph logic with apply_plan_json. Use the actual node types, function calls, ");
Output += TEXT("and wiring patterns from the reference -- do not simplify to PrintString.\n");
Output += TEXT("3. Compile to 0 errors before moving to the next asset\n");
Output += TEXT("Do not stop until every asset in the plan is fully built and compiled.\n");
```

**Key behavioral change:** Step 2a explicitly tells the Builder to call `get_template` for implementation reference BEFORE writing `plan_json`. This restores the research agency that was removed when the pipeline replaced the old system.

### B5: "Intentionally simple" detection (~10 lines)

**Location:** `FormatForPromptInjection()`, immediately before Section 5 (execution directive).

**Insert a conditional that checks the Router's reasoning and the user message for simplicity intent:**

```cpp
// Detect whether the user explicitly wants simple/stub logic
bool bWantsSimpleLogic = false;
{
    // Check Router reasoning (it captures user intent keywords)
    FString Combined = Router.Reasoning.ToLower();
    // Also check the Architect's plan for stub indicators
    Combined += TEXT(" ") + Architect.BuildPlan.ToLower();

    static const TArray<FString> SimpleIndicators = {
        TEXT("placeholder"), TEXT("stub"), TEXT("just printstring"),
        TEXT("print string only"), TEXT("skeleton"), TEXT("empty logic"),
        TEXT("no logic"), TEXT("structure only"), TEXT("stub it out"),
        TEXT("just the structure"), TEXT("mockup"), TEXT("prototype only")
    };

    for (const FString& Indicator : SimpleIndicators)
    {
        if (Combined.Contains(Indicator))
        {
            bWantsSimpleLogic = true;
            break;
        }
    }
}
```

Then in Section 5, wrap the research encouragement in a conditional:

```cpp
// Section 5: Execution directive
Output += TEXT("## Execution\n\n");
Output += TEXT("Follow the Build Plan above. For each asset in Order:\n");
Output += TEXT("1. Create structure (components, variables, interfaces, dispatchers)\n");

if (bWantsSimpleLogic)
{
    Output += TEXT("2. Write graph logic as described in the plan. The user wants ");
    Output += TEXT("placeholder/simple logic -- PrintString stubs are acceptable.\n");
}
else
{
    Output += TEXT("2. For each function/event:\n");
    Output += TEXT("   a. If library templates were referenced above, use `blueprint.get_template(template_id, pattern=\"FunctionName\")` ");
    Output += TEXT("to study how similar functions are built. Base your plan_json on real node patterns.\n");
    Output += TEXT("   b. Write graph logic with apply_plan_json. Use the actual node types, function calls, ");
    Output += TEXT("and wiring patterns from the reference -- do not simplify to PrintString.\n");
}

Output += TEXT("3. Compile to 0 errors before moving to the next asset\n");
Output += TEXT("Do not stop until every asset in the plan is fully built and compiled.\n");
```

**Why detect in FormatForPromptInjection and not in RunRouter:** The Router already classifies complexity, but "simple logic" and "simple task" are different things. A user might want a complex multi-asset system with stub logic ("build me a weapon system but just PrintString for now"). This is an output quality directive, not a complexity classification.

### B6: Include `TemplateContent` in `FormatForPromptInjection()` for the Builder (~5 lines)

**Location:** `FormatForPromptInjection()`, after Section 2 (Reference Templates) and before Section 3 (Build Plan).

**Insert:**

```cpp
// Section 2.5: Implementation Reference (from Scout's auto-loaded template content)
if (!Scout.TemplateContent.IsEmpty() && !bWantsSimpleLogic)
{
    Output += Scout.TemplateContent;
    Output += TEXT("\n");
}
```

**Rationale:** The Builder gets both the summaries (Section 2 -- what templates exist) and the implementation details (Section 2.5 -- how key functions are actually built). This is redundant with the Architect having seen the same content, but serves a different purpose: the Builder needs to see the actual node patterns to write correct `plan_json`, while the Architect used them to plan the architecture.

**Token budget:** The `bWantsSimpleLogic` gate prevents wasting tokens when the user wants stubs. The max 4 functions (~800-1600 tokens) stays well within prompt budget.

**Important:** Move the `bWantsSimpleLogic` detection block to BEFORE Section 2.5 so it is available for both this gate and the Section 5 gate.

---

## 5. Change C: `GetAgentModelConfig()` utility model fallback

**File:** `Source/OliveAIEditor/Private/Settings/OliveAISettings.cpp`

**Location:** `GetAgentModelConfig()`, at line 388-393 where CLI providers trigger immediate `bIsCLIFallback = true`.

**Current code:**

```cpp
// Skip CLI providers (handled as fallback tier)
if (TargetProvider == EOliveAIProvider::ClaudeCode
    || TargetProvider == EOliveAIProvider::Codex)
{
    Config.bIsCLIFallback = true;
    return Config;
}
```

**Replace with:**

```cpp
// CLI providers cannot do HTTP completions. Try utility model first.
if (TargetProvider == EOliveAIProvider::ClaudeCode
    || TargetProvider == EOliveAIProvider::Codex)
{
    // Attempt to use the utility model provider instead (fast, cheap, HTTP-capable)
    const EOliveAIProvider UtilProvider = UtilityModelProvider;

    // Only use utility model if it's NOT itself a CLI provider and has a valid config
    if (UtilProvider != EOliveAIProvider::ClaudeCode
        && UtilProvider != EOliveAIProvider::Codex
        && !UtilityModelId.IsEmpty())
    {
        FString UtilApiKey = UtilityModelApiKey;
        if (UtilApiKey.IsEmpty())
        {
            UtilApiKey = GetApiKeyForProvider(UtilProvider);
        }

        const bool bUtilNeedsKey = UtilProvider != EOliveAIProvider::Ollama
                                && UtilProvider != EOliveAIProvider::OpenAICompatible;

        if (!bUtilNeedsKey || !UtilApiKey.IsEmpty())
        {
            Config.Provider = UtilProvider;
            Config.ModelId = UtilityModelId;
            Config.ApiKey = UtilApiKey;
            Config.BaseUrl = GetBaseUrlForProvider(UtilProvider);
            Config.bIsValid = true;
            Config.bIsCLIFallback = false;
            return Config;
        }
    }

    // No utility model available -- fall through to CLI --print
    Config.bIsCLIFallback = true;
    return Config;
}
```

**Impact:** For CLI-only users (ClaudeCode/Codex main provider) who have a utility model configured (the default -- `UtilityModelProvider = OpenRouter`, `UtilityModelId = "anthropic/claude-3-5-haiku-latest"`), the pipeline agents will use the utility model's HTTP endpoint instead of launching a full `claude --print` CLI process per agent call.

This changes agent call latency from ~3-5s per `claude --print` invocation (process launch + model routing) to ~0.5-1s per HTTP call to a fast model. With 3-5 agent calls per pipeline, this saves 10-20 seconds.

**No behavior change** for users who already have an HTTP main provider (Anthropic, OpenRouter, etc.) -- they were already using Tier 2 successfully.

---

## 6. Data Flow Diagram

```
User Message
    |
    v
[Router] --> Complexity (Simple/Moderate/Complex)
    |
    v
[Scout]
    |-- RunDiscoveryPass() --> FOliveDiscoveryResult (summaries)
    |-- FormatDiscoveryForPrompt() --> DiscoveryBlock (markdown)
    |-- NEW: Auto-load top 2 library template functions --> TemplateContent
    |-- SearchAssets() --> RawAssets
    |-- LLM rank relevance --> RelevantAssets
    |
    v
[Researcher] (if not Simple)
    |-- ReadBlueprintSummary() for top 3 assets
    |-- LLM analyze --> ArchitecturalAnalysis
    |
    v
[Architect]
    |-- Receives: UserMessage + Complexity + RelevantAssets
    |--           + ArchitecturalAnalysis + DiscoveryBlock
    |--           + NEW: TemplateContent  <-- actual node-level patterns
    |-- LLM plan --> BuildPlan
    |
    v
[Validator] (C++ only)
    |-- TryResolveClass, TryResolveComponentClass, IsValidInterface
    |
    v
FormatForPromptInjection()
    |-- Section 1: Task Analysis (complexity)
    |-- Section 2: Reference Templates Found (summaries)
    |-- NEW Section 2.5: Implementation Reference (node patterns)
    |-- Section 3: Build Plan + Validator Warnings
    |-- Section 4: Existing Assets + Researcher Analysis
    |-- Section 5: UPDATED Execution Directive (encourages get_template)
    |
    v
[Builder] (Claude Code / API)
    |-- Has: real node patterns + explicit instruction to use get_template
    |-- Writes: plan_json based on actual patterns, not guesses
```

---

## 7. Edge Cases

### E1: No library templates found by discovery
`DiscoveryResult.Entries` has no library entries (all factory/reference/community, or fewer than 2 entries total). `TemplateContent` stays empty. The Architect works from general UE knowledge (same as current behavior). The execution directive's step 2a says "If library templates were referenced above" -- the conditional language means the Builder skips it when there are none.

### E2: MatchedFunctions is empty on a library hit
Discovery matched a library template by tag/description but could not identify specific functions. The Scout skips auto-loading for that entry (per the `Entry.MatchedFunctions.Num() == 0` gate). The Builder can still manually call `list_templates`/`get_template` since the discovery summaries mention the template.

### E3: GetFunctionContent returns not-found sentinel
The function name from discovery might not match exactly (e.g., discovery says "MeleeAttack" but the template's function is "PerformMeleeAttack"). The Scout checks for the `@@FUNC_NOT_FOUND@@` sentinel and skips that function. If all functions fail, `TemplateContent` stays empty for that template and the template counter does not increment.

### E4: Very large function graph (1000+ tokens)
`GetFunctionContent()` formats all nodes in the function. For complex functions, this could be 500-1500 tokens. The 4-function cap (2 templates x 2 functions) provides a natural budget ceiling of ~2000-4000 tokens worst case. This is acceptable for the Architect's context (it has a 2048 max_tokens OUTPUT budget, but its INPUT can be much larger -- the HTTP providers typically support 8k+ context).

If this becomes a problem in practice, the coder may add a character limit (e.g., 600 chars per function) with truncation + "... see full via get_template" suffix. But do not add this in the initial implementation -- measure first.

### E5: User wants stubs but also mentions templates
Example: "build me a weapon system like the combatfs melee component, but just stub out the logic for now." The `bWantsSimpleLogic` detector will match "stub" in the Router reasoning or Architect plan, and the execution directive will skip the research encouragement. This is correct behavior -- respect the user's explicit intent.

### E6: Utility model is also a CLI provider
If the user sets `UtilityModelProvider = ClaudeCode`, the new code checks `UtilProvider != EOliveAIProvider::ClaudeCode` and skips the utility model path, falling through to CLI `--print` as before. No infinite loop risk.

### E7: Template system not initialized
`FOliveTemplateSystem::Get().GetLibraryIndex().IsInitialized()` returns false if templates failed to load. The Scout gate (`if (LibIndex.IsInitialized())`) prevents any attempt to auto-load content. `TemplateContent` stays empty.

---

## 8. Implementation Order

### Phase 1: Struct change (1 minute)
Add `TemplateContent` field to `FOliveScoutResult` in `OliveAgentConfig.h`.

### Phase 2: Scout enhancement (5 minutes)
Add the template content auto-loading block to `RunScout()` in `OliveAgentPipeline.cpp`, after line 847. Include the log statement.

### Phase 3: Architect enhancement (3 minutes)
1. Add `TemplateContent` injection to `RunArchitect()` after line 1181.
2. Append the two new rule lines to `BuildArchitectSystemPrompt()`.

### Phase 4: Execution directive + simple detection (5 minutes)
1. Add `bWantsSimpleLogic` detection block before Section 5 in `FormatForPromptInjection()`.
2. Add Section 2.5 (template content for Builder) after Section 2, gated on `!bWantsSimpleLogic`.
3. Replace Section 5 with the conditional execution directive.

### Phase 5: Settings speed fix (3 minutes)
Replace the CLI-provider early-return in `GetAgentModelConfig()` with the utility model probe.

### Phase 6: Build verification (2 minutes)
Run incremental build. Fix any compilation errors. No new files, no new includes needed (all APIs already imported in `OliveAgentPipeline.cpp`).

---

## 9. Testing Checklist

These are manual tests (run the editor, trigger the pipeline):

1. **Happy path**: Ask "build me a melee weapon" with CombatFS templates loaded. Verify:
   - Scout log shows "auto-loaded N template function(s)"
   - Architect's BuildPlan mentions specific patterns (not generic "deal damage")
   - Builder calls `get_template` during execution
   - Built Blueprint has real logic (line traces, damage, etc.), not just PrintString

2. **No templates**: Ask "build me a custom UI widget" (no library templates for widgets). Verify:
   - Scout log does NOT show template auto-loading
   - Execution directive still works (Builder proceeds without template research)
   - No crash, no empty sections in prompt

3. **Stub request**: Ask "build me a weapon system, just stub it out for now." Verify:
   - Execution directive says "placeholder/simple logic -- PrintString stubs are acceptable"
   - Implementation Reference section is NOT included in Builder prompt
   - Builder uses PrintString (respects user intent)

4. **CLI speed**: With main provider = ClaudeCode and utility model = OpenRouter/Haiku. Verify:
   - Pipeline agents use HTTP (not CLI --print)
   - Total pipeline time < 10s (not 30s+)

5. **No utility model**: With main provider = ClaudeCode and no utility model API key. Verify:
   - Pipeline falls back to CLI --print (no crash)
   - Slower but functional

---

## 10. Summary for the Coder

You are modifying 2 existing files and making no new files:

**`Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h`**
- Add `FString TemplateContent` field to `FOliveScoutResult` (after `DiscoveryBlock`, before `bSuccess`)

**`Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp`**
- `RunScout()`: After line 847, add the template content auto-loading block (Section B1 above)
- `RunArchitect()`: After line 1181, inject `ScoutResult.TemplateContent` into the prompt (Section B2)
- `BuildArchitectSystemPrompt()`: Append 2 rule lines about studying Implementation Reference (Section B3)
- `FormatForPromptInjection()`: Restructure to add `bWantsSimpleLogic` detection, Section 2.5 (template content), and conditional Section 5 (execution directive) (Sections B4, B5, B6)

**`Source/OliveAIEditor/Private/Settings/OliveAISettings.cpp`**
- `GetAgentModelConfig()`: Replace the CLI early-return (lines 388-393) with utility model probe logic (Section C)

**Total estimated scope:** ~80-100 lines of new/modified code across 3 files. No new includes, no new dependencies, no API changes.
