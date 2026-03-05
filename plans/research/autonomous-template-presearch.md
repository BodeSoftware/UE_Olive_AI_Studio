# Research: Autonomous Mode Pre-Search Template Injection

## Question
How can we automatically search library templates based on the user's message when an autonomous agent run starts, so the agent gets relevant template results in its context before it begins building?

---

## Findings

### 1. The Autonomous Run Startup Sequence

The full call chain from user message to CLI launch is:

1. `FOliveConversationManager::SendUserMessage()` → detects autonomous provider → calls `SendUserMessageAutonomous()`
2. `SendUserMessageAutonomous()` (line 224, `OliveConversationManager.cpp`) does:
   - Adds user message to history
   - Ensures MCP server is running
   - Creates auto-snapshot
   - Calls `CLIProvider->SetInitialContextAssets(ActiveContextPaths)` (line 378)
   - Calls `Provider->SendMessageAutonomous(Message, ...)` (line 385)
3. `FOliveCLIProviderBase::SendMessageAutonomous()` (line 429, `OliveCLIProviderBase.cpp`) does:
   - Calls `SetupAutonomousSandbox()` — writes CLAUDE.md + .mcp.json
   - Enriches message with @-mention asset state (`BuildAssetStateSummary`)
   - Appends a generic template nudge to `EffectiveMessage` (line 514):
     ```
     "Before building, research patterns from Library templates: search blueprint.list_templates(query=\"...\") ..."
     ```
   - Calls `LaunchCLIProcess(CLIArgs, EffectiveMessage, ...)`

**Key insight:** `EffectiveMessage` is the stdin content delivered directly to the CLI process. This is the most reliable injection point — Claude Code reads stdin as the imperative channel. The CLAUDE.md is loaded into Claude's system context, but is treated more as a reference document.

The current template nudge at line 514 is generic — it tells the agent *to* search but doesn't tell it *what* to search for. That is the gap.

---

### 2. Current Template Search API

`FOliveLibraryIndex::Search()` is at line 2619 in `OliveTemplateSystem.cpp`:

```cpp
TArray<TSharedPtr<FJsonObject>> FOliveLibraryIndex::Search(const FString& Query, int32 MaxResults) const
```

It returns a `TArray<TSharedPtr<FJsonObject>>` where each entry includes:
- `template_id`, `display_name`, `type`, `blueprint_type`, `parent_class`
- `function_count`, `catalog_description`, `source_project`
- `matched_functions` array (functions whose name/tags/description matched the query tokens)

The search is two-pass:
1. Inverted index exact token lookup (score +2 per match)
2. Substring scan across all index keys (score +1 per match)

Then `FOliveTemplateSystem::SearchTemplates()` wraps it, adding factory/reference templates and reserving 5 slots for them.

**Tokenizer behavior** (`Tokenize()`, line 2545):
- Splits on spaces, underscores, hyphens
- Lowercases all tokens
- Drops tokens with length < 2
- Drops stop words: `the`, `and`, `for`, `with`, `from`, `this`, `that`, `into`
- Deduplicates

So `"create a bow and arrow system for @BP_ThirdPersonCharacter"` tokenizes to:
`["create", "bow", "arrow", "system", "bp", "thirdpersoncharacter"]`

`"ranged"`, `"weapon"`, and `"combat"` are not in the message but would score well in the index. The tokenizer can't infer synonyms — it only produces the literal words present.

**What the search index covers** (`BuildSearchTokens()`, line 2582):
- template ID, display name, tags, inherited_tags, catalog description, source_project
- parent class name, interface names, component names, dispatcher names
- per-function: name, tags, description

So querying `"bow arrow ranged"` against the combatfs library would hit `combatfs_bp_arrow_parent`, `combatfs_ranged_component`, `combatfs_bp_arrow_quiver_parent`, and their functions tagged `ranged`, `bow`, `fire_arrow`, etc.

---

### 3. Where Keyword Extraction Would Happen

There are two possible locations:

**Option A — In `SendMessageAutonomous()` (OliveCLIProviderBase.cpp, ~line 514):**
The existing generic nudge at line 514 is already in the perfect slot. We call `FOliveTemplateSystem::Get().SearchTemplates(query)` here and append the results to `EffectiveMessage`. This runs on the game thread (confirmed: `SendMessageAutonomous` is called from game thread via `FOliveConversationManager`), and `FOliveLibraryIndex::Search()` is game-thread safe (read-only after `Initialize()`).

**Option B — In `SetupAutonomousSandbox()` (OliveCLIProviderBase.cpp, ~line 323):**
This writes the CLAUDE.md file. We could append a "Relevant Templates" section. However, CLAUDE.md is reference context, not imperative channel. It's less reliable for ensuring the agent acts on the data.

**Best location: `SendMessageAutonomous()`, replacing/augmenting the existing nudge at line 514.**

The problem: at the time `SendMessageAutonomous()` runs, the original `UserMessage` is available as a parameter. We can tokenize it and run `SearchTemplates()` immediately. The `InitialContextAssetPaths` injection (line 493-506) has already happened by line 514, so we'd insert the template pre-search after line 507, alongside the nudge.

---

### 4. Keyword Extraction Analysis

The current `DetermineToolPrefixes()` function (line 39, `OliveCLIProviderBase.cpp`) already does simple keyword detection for domain routing. A similar approach works for template search.

**Option 1: Pass the raw user message directly to `SearchTemplates()`**

`Tokenize("create a bow and arrow system for @BP_ThirdPersonCharacter")` → `["create", "bow", "arrow", "system", "bp", "thirdpersoncharacter"]`

This works but includes noisy tokens like `"create"` and `"system"`. The inverted index will score `"bow"` and `"arrow"` highest, which is correct. `"system"` may produce false positives.

**Option 2: Strip known noise before passing**

Pre-clean the message: remove `@`-mentions (they're asset references, not search terms), remove leading verbs like "create", "build", "make", "add", strip punctuation. Then pass to `Tokenize()`.

Simple regex-style filter: remove tokens in a domain-specific stop set `{"create", "build", "make", "add", "implement", "write", "add", "system", "using", "in", "my", "a", "an"}`.

**Option 3: Multi-query approach**

Extract @-mentioned class names as a second query. For `@BP_ThirdPersonCharacter`, search `"character"` and `"player"` separately, merge results. This finds component templates relevant to the target class.

**Recommendation:** Option 1 is sufficient. The existing `Tokenize()` already handles noise reduction (stop words, length filter). The index is robust enough that extra noisy tokens like `"create"` simply don't appear in the index and score 0. The `@`-mention format will produce tokens `"bp"` and `"thirdpersoncharacter"` which may score weakly against class names — that's acceptable. A simple pre-pass to strip `@` prefix is enough.

---

### 5. Four Approaches Evaluated

#### Approach A: Pre-search injection into stdin message (Recommended)

**How it works:**
1. In `SendMessageAutonomous()`, before calling `LaunchCLIProcess()`:
2. Strip `@` from the user message
3. Call `FOliveTemplateSystem::Get().SearchTemplates(CleanedMessage, 8)`
4. Format the top results as a compact Markdown block
5. Append to `EffectiveMessage` (replacing/augmenting the current generic nudge at line 514)

**Token cost:** A compact result block for 8 templates (ID + description + matched functions) is ~300-500 tokens. Negligible for a 100k context window.

**Latency:** `SearchTemplates()` on 658 templates takes microseconds (pure in-memory index scan). Zero latency impact.

**Reliability:** Stdin is the imperative channel. Results placed in stdin are reliably read. Claude Code processes the entire stdin before acting.

**Flexibility:** Works for any task type. If no templates match (score = 0), the array is empty and we fall back to the existing generic nudge.

**Risks:**
- If the user message is vague (e.g., "do it"), the search query produces garbage results. Guard: check `SearchResults.Num() > 0` before appending.
- The pre-search results become stale if the agent doesn't act on them. No risk — they're reference data, the agent can still call `list_templates` with a different query.

**Code location:** `OliveCLIProviderBase.cpp`, `SendMessageAutonomous()`, replacing the text block at line 509–515.

```cpp
// Pre-search template suggestions based on user message intent
if (!IsContinuationMessage(UserMessage))
{
    FString SearchQuery = UserMessage;
    SearchQuery.ReplaceInline(TEXT("@"), TEXT(" ")); // Strip @-mention prefix

    TArray<TSharedPtr<FJsonObject>> TemplateSuggestions =
        FOliveTemplateSystem::Get().SearchTemplates(SearchQuery, 8);

    if (TemplateSuggestions.Num() > 0)
    {
        EffectiveMessage += TEXT("\n\n## Relevant Library Templates (Pre-searched)\n");
        EffectiveMessage += TEXT("These templates were found by searching your task description. ");
        EffectiveMessage += TEXT("Use `blueprint.get_template(id, pattern=\"FuncName\")` to study specific functions.\n\n");

        for (const TSharedPtr<FJsonObject>& Entry : TemplateSuggestions)
        {
            FString Id, Desc, SourceProj;
            Entry->TryGetStringField(TEXT("template_id"), Id);
            Entry->TryGetStringField(TEXT("catalog_description"), Desc);
            Entry->TryGetStringField(TEXT("source_project"), SourceProj);

            EffectiveMessage += FString::Printf(TEXT("- **%s**"), *Id);
            if (!SourceProj.IsEmpty())
                EffectiveMessage += FString::Printf(TEXT(" [%s]"), *SourceProj);
            if (!Desc.IsEmpty())
                EffectiveMessage += FString::Printf(TEXT(": %s"), *Desc);
            EffectiveMessage += TEXT("\n");

            // Show matched function names (the most actionable signal)
            const TArray<TSharedPtr<FJsonValue>>* MatchedFuncs;
            if (Entry->TryGetArrayField(TEXT("matched_functions"), MatchedFuncs) && MatchedFuncs)
            {
                TArray<FString> FuncNames;
                for (const TSharedPtr<FJsonValue>& FuncVal : *MatchedFuncs)
                {
                    TSharedPtr<FJsonObject> FuncObj = FuncVal->AsObject();
                    FString FuncName;
                    if (FuncObj && FuncObj->TryGetStringField(TEXT("name"), FuncName))
                        FuncNames.Add(FuncName);
                }
                if (FuncNames.Num() > 0)
                    EffectiveMessage += FString::Printf(TEXT("  Functions: %s\n"), *FString::Join(FuncNames, TEXT(", ")));
            }
        }

        EffectiveMessage += TEXT("\nStudy relevant functions before building. These are references — adapt patterns to fit the task.\n");
    }
    else
    {
        // Fall back to generic nudge if no matches
        EffectiveMessage += TEXT("\n\nBefore building, search for relevant patterns: ");
        EffectiveMessage += TEXT("`blueprint.list_templates(query=\"...\")` with task-specific keywords.\n");
    }
}
```

---

#### Approach B: MCP Dynamic Resource

**How it works:** Add a `olive://template/suggestions` resource to `HandleResourcesList` that returns search results based on... what exactly? The resource is static at listing time. It would need to embed the current task context, which the MCP server doesn't have.

**Verdict: Not viable.** MCP resources are stateless lookups by URI. There is no mechanism to pass the current user message into a resource query. The MCP server could expose a parameterized URI like `olive://template/suggestions?q=bow+arrow` but the agent would have to know to call it first, which is the same problem as the current `list_templates` situation.

A variant: register a special "task context" resource that the server always returns with the last-run search. But this creates a race condition (previous message's results) and adds server-side statefulness. Complex for little gain over Approach A.

---

#### Approach C: System prompt / CLAUDE.md injection

**How it works:** In `SetupAutonomousSandbox()`, add a "Relevant Templates" section to CLAUDE.md.

The CLAUDE.md is written at sandbox setup time, which happens inside `SendMessageAutonomous()` at line 476. The `UserMessage` parameter is available at that point, so keyword extraction is feasible.

However, CLAUDE.md is loaded into Claude Code's system context, which is processed before the stdin prompt. The stdin prompt (imperative channel) takes priority for directing action. Results in CLAUDE.md are more likely to be referenced as background knowledge than as a prompt to act.

Additionally, CLAUDE.md is written once per sandbox setup. The `SetupAutonomousSandbox()` function currently recreates the file on every call (not cached). The search would need to be thread-safe since `SetupAutonomousSandbox()` is called from within `SendMessageAutonomous()` on the game thread.

**Verdict: Lower reliability than Approach A. The stdin channel is more reliable for directing the agent's first action.** CLAUDE.md could be used as a secondary injection point (a static "recently matched templates" section), but there's no strong reason to prefer it over stdin.

---

#### Approach D: Tool-level smarts (auto-search on first tool call)

**How it works:** When `HandleBlueprintListTemplates` is called without a query, detect that this is the agent's first call and inject task-relevant suggestions based on some stored "current task" context.

This requires:
- Storing the current user message somewhere accessible to the tool handler (e.g., on `FOliveToolRegistry` or a new context object)
- Detecting "first call" — non-trivial
- Changing tool behavior based on session state, which breaks idempotency

**Verdict: Architecturally wrong.** Tool handlers should be stateless and deterministic given their input parameters. Injecting session context into tool behavior is a coupling antipattern. Also, this only helps if the agent *happens* to call `list_templates` without a query first — if it doesn't call it at all (the core problem), this doesn't help.

---

### 6. Token Cost Estimate

A compact 8-result block formatted as shown in Approach A:
- 8 template entries × ~40 chars ID/source = ~320 chars
- 8 descriptions × ~80 chars avg = ~640 chars
- 8 × 3 function names × ~15 chars = ~360 chars
- Headers + prose = ~200 chars
- **Total: ~1,520 chars ≈ ~380 tokens**

The existing generic nudge (line 509–515) is ~230 chars (~58 tokens). Net addition: ~320 tokens.

Against a 200k token context window, this is negligible.

---

### 7. CLAUDE.md vs stdin: Which Channel Gets Read?

From the code:
- CLAUDE.md is written to the sandbox directory, which is Claude Code's working directory. Claude Code natively reads `CLAUDE.md` from the cwd into its system context. It is reference context.
- stdin content is the user message piped directly into the process. It is the imperative channel — what the agent acts on first.

From CLAUDE.md in the project itself:
> `BuildPrompt()` must include... a `## Next Action Required` directive [...] `BuildSystemPrompt()` should provide schema/reference guidance, not be the only place where routing intent lives.

This confirms the architecture: **stdin is for action, CLAUDE.md is for reference.** Pre-searched template results belong in stdin.

---

### 8. Keyword Extraction — Edge Cases

| User Message | Tokenized | Expected Results |
|---|---|---|
| "create a bow and arrow system for @BP_ThirdPersonCharacter" | `["create", "bow", "arrow", "system", "bp", "thirdpersoncharacter"]` | `combatfs_ranged_component`, `combatfs_bp_arrow_parent`, `combatfs_bp_ranged_character` |
| "add a health component to my character" | `["add", "health", "component", "my", "character"]` | Any template with "health" or "combat_status" in tags/name |
| "make it explode when hit" | `["make", "explode", "when", "hit"]` | Low/no results — broad, no domain-specific terms |
| "implement the AI patrol system" | `["implement", "ai", "patrol", "system"]` | `combatfs_bp_patrol_path`, `combatfs_bp_ai_parent` |
| "continue" | (continuation — skipped by `IsContinuationMessage`) | No pre-search injected |

The empty-result case (`"make it explode when hit"`) gracefully falls back to the generic nudge. No special handling needed.

---

## Recommendations

1. **Implement Approach A.** It is simple, low-risk, and solves the problem directly. The implementation point is `OliveCLIProviderBase.cpp`, `SendMessageAutonomous()`, replacing the generic nudge block at lines 509–515 with a search-based injection that falls back to the generic nudge when no results are found.

2. **Strip `@` prefixes** before passing the user message to `SearchTemplates()`. The tokenizer will then see `"BP"` and `"ThirdPersonCharacter"` as tokens rather than `"@BP_ThirdPersonCharacter"` as a single unparseable string.

3. **Cap results at 8.** Empirically, 8 is enough to surface the directly relevant templates without burying the agent in noise. `SearchTemplates(query, 8)` with default `MaxResults=20` on `LibraryIndex.Search()` — pass 8 explicitly.

4. **Include matched function names in the injection block.** The `matched_functions` field in search results identifies which specific functions matched the query. Showing these explicitly (e.g., `Functions: FireArrow, DrawBow, ReleaseArrow`) is the most actionable signal — the agent can immediately call `get_template(id, pattern="FireArrow")` without guessing function names.

5. **Do not inject into CLAUDE.md (Approach B/C).** The stdin channel is more reliable for directing first-turn action. CLAUDE.md additions are valid as supplementary context but should not be the primary injection site.

6. **Do not make `list_templates` stateful (Approach D).** Tool handlers must remain stateless.

7. **No new keyword extraction logic needed.** `FOliveLibraryIndex::Tokenize()` is already public and handles all the required normalization. Reuse it. The only preprocessing needed is `SearchQuery.ReplaceInline(TEXT("@"), TEXT(" "))`.

8. **Gotcha: `SearchTemplates()` must be called on the game thread.** `FOliveLibraryIndex` is read-only after `Initialize()` (no locks on read path), so it's safe for concurrent reads, but UObjects involved in factory template loading are not. `SendMessageAutonomous()` is always called from the game thread via `FOliveConversationManager`, so this constraint is satisfied automatically.
