# Implementation Plan: Results Improvement

Derived from approved plan: `plans/design/results-improvement-plan-v2.md`

All line numbers verified against current source on 2026-03-05.

---

## Phase A: Prompt/Content Changes (no C++ compilation needed)

These tasks modify only `.txt` content files and can ship immediately. All are independent of each other.

---

### Task A1: Deduplicate `node_routing.txt` — Remove "Three approaches" overlap

**File:** `Content/SystemPrompts/Knowledge/node_routing.txt`

**What:** Lines 1-6 ("## Graph Editing Tools" header + "Three approaches for adding logic to Blueprints. Pick whichever fits the task.") repeat the same content already in `cli_blueprint.txt` lines 4-10 (the "## Approach" section). Remove the overlap.

**Details:**
- Delete lines 1-6 (from `## Graph Editing Tools` through the blank line after "Pick whichever fits the task.")
- Keep lines 7 onward intact (the subsection headers `### plan_json`, `### add_node`, `### editor.run_python`, `### get_node_pins`, `### Mixing approaches`, `### Your UE5 knowledge is valid`)
- The file should now start with `### plan_json (blueprint.apply_plan_json, schema_version "2.0")`

**Token savings:** ~30 tokens

**Verify:** File starts with `### plan_json` and does not contain "Three approaches".

---

### Task A2: Merge unique `recipe_routing.txt` content into `cli_blueprint.txt`

**File (source):** `Content/SystemPrompts/Knowledge/recipe_routing.txt`
**File (target):** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**What:** The only content in `recipe_routing.txt` that is NOT already in `cli_blueprint.txt` is:
1. `olive.get_recipe(query)` has tested wiring patterns (line 2)
2. The "Pattern Research Sources" section (lines 10-15) with the 3-source comparison (library, factory, community) and "Always compare multiple references" guidance

Items 1 is partially covered by `cli_blueprint.txt` line 97 ("search blueprint.list_templates..."). Item 2 is partially covered by `cli_blueprint.txt` lines 76-80 (Templates section).

**Details:**
In `cli_blueprint.txt`, at the end of the `## Templates & Pattern Sources` section (after line 81, before line 82 which starts `## plan_json Scope`):

Add these 3 lines:
```
- Recipes: olive.get_recipe(query) has tested wiring patterns for common tasks. Useful when unsure about a specific pattern.
- When researching, compare multiple references before committing. For community blueprints especially, browse 5-10 results first.
```

**Do NOT delete `recipe_routing.txt` yet** -- it is still loaded by `BuildCLISystemPrompt()` (line 1435 of OliveCLIProviderBase.cpp) and removing it is a Phase B task (Task B2).

**Token change:** +30 tokens in cli_blueprint.txt (will be offset by removing recipe_routing loading later).

**Verify:** `cli_blueprint.txt` Templates section now mentions `olive.get_recipe` and the "compare multiple references" guidance.

---

### Task A3: Strengthen completion directive in stdin decomposition block

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**What:** Replace the weak ending of the decomposition block (line 497) with a stronger imperative.

**Current text (line 497):**
```cpp
EffectiveMessage += TEXT("After listing assets, research patterns with blueprint.list_templates(query=\"...\"), then build each asset fully before starting the next.\n");
```

**Replace with:**
```cpp
EffectiveMessage += TEXT("After listing assets, research patterns with blueprint.list_templates(query=\"...\").\n\n");
EffectiveMessage += TEXT("Build the COMPLETE system. For each Blueprint:\n");
EffectiveMessage += TEXT("1. Create structure (components, variables, functions)\n");
EffectiveMessage += TEXT("2. Write ALL graph logic with apply_plan_json for every function\n");
EffectiveMessage += TEXT("3. Compile to 0 errors\n");
EffectiveMessage += TEXT("Do not stop until every asset is fully built, wired, and compiled.\n");
```

**Note:** This is technically a C++ file edit, but it is purely a string content change and does not affect any logic. Can be tested without recompilation by rebuilding once and observing autonomous behavior.

**Token change:** Net ~+40 tokens (replaces ~20 tokens, adds ~60).

**Verify:** Build succeeds. The stdin prompt visible in logs shows the new imperative block.

---

### Task A4: Add verify-before-done guidance to `cli_blueprint.txt`

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**What:** Add one line to the `## Rules` section (currently lines 87-97) about verifying complex function graphs.

**Details:**
After line 93 (`- After a plan_json failure, all nodes from that plan are rolled back...`), add:
```
- After applying plan_json to a complex function (5+ steps), read the function back with blueprint.read_function to verify the graph looks correct. Disconnected data wires don't always cause compile errors.
```

**Token change:** +30 tokens.

**Verify:** The Rules section now contains 9 bullet points (was 8), including the read-back guidance.

---

### Task A5: Add batching guidance to sandbox Critical Rules

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**What:** Add one line to the Critical Rules block in `SetupAutonomousSandbox()`. The block runs from line 357 to line 365.

**Details:**
After line 364 (`- Before finishing, verify you built EVERY part...`), add:
```cpp
AgentContext += TEXT("- Batch independent tool calls (add_variable, add_component) in a single response when possible.\n");
```

This is adding it before the final `\n` on line 365 (template stub instruction), so insert between lines 364 and 365.

**Token change:** +15 tokens.

**Verify:** Build succeeds. The sandbox AGENTS.md file contains the batching guidance.

---

### Task A6: Trim stdin decomposition — remove template research duplication

**What:** This is already handled by Task A3. The replacement text in A3 separates template research from the completion directive. The template research is already well-covered in `cli_blueprint.txt` lines 76-81 and `blueprint_design_patterns.txt`, so the stdin only needs a brief mention (preserved in A3's replacement text).

**Status:** No additional work needed beyond Task A3.

---

## Phase B: Code Changes (require compilation)

---

### Task B1: Add missing knowledge packs to `SetupAutonomousSandbox()`

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**What:** Load `events_vs_functions.txt` and `node_routing.txt` into the autonomous sandbox alongside the existing 3 packs.

**Details:**
After the `DesignPatterns` load block (lines 347-351), add two more load blocks:

```cpp
FString EventsVsFunctions;
if (!FFileHelper::LoadFileToString(EventsVsFunctions, *FPaths::Combine(KnowledgeDir, TEXT("events_vs_functions.txt"))))
{
    UE_LOG(LogOliveCLIProvider, Warning, TEXT("Failed to load events_vs_functions.txt knowledge pack"));
}

FString NodeRouting;
if (!FFileHelper::LoadFileToString(NodeRouting, *FPaths::Combine(KnowledgeDir, TEXT("node_routing.txt"))))
{
    UE_LOG(LogOliveCLIProvider, Warning, TEXT("Failed to load node_routing.txt knowledge pack"));
}
```

Then after the `DesignPatterns` append block (lines 381-386), add:

```cpp
if (!EventsVsFunctions.IsEmpty())
{
    AgentContext += TEXT("---\n\n");
    AgentContext += EventsVsFunctions;
    AgentContext += TEXT("\n\n");
}

if (!NodeRouting.IsEmpty())
{
    AgentContext += TEXT("---\n\n");
    AgentContext += NodeRouting;
    AgentContext += TEXT("\n\n");
}
```

**Dependencies:** Task A1 should be done first (to avoid duplicating the "Three approaches" content already in cli_blueprint.txt).

**Token cost:** +550 tokens to autonomous sandbox.

**Verify:** Build succeeds. Launch autonomous mode, inspect the generated `Saved/OliveAI/AgentSandbox/AGENTS.md` file. Confirm it contains "## Events vs Functions" and "### plan_json (blueprint.apply_plan_json" sections.

---

### Task B2: Add missing knowledge packs to `BuildCLISystemPrompt()`

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**What:** Add `blueprint_design_patterns`, `events_vs_functions`, and `node_routing` knowledge packs to the CLI system prompt path (used by orchestrated non-autonomous CLI mode).

**Details:**
After the `CLIBlueprint` block (lines 1445-1458), add:

```cpp
const FString DesignPatterns = Assembler.GetKnowledgePackById(TEXT("blueprint_design_patterns"));
if (!DesignPatterns.IsEmpty())
{
    SystemPrompt += DesignPatterns;
    SystemPrompt += TEXT("\n\n");
}

const FString EventsVsFunctions = Assembler.GetKnowledgePackById(TEXT("events_vs_functions"));
if (!EventsVsFunctions.IsEmpty())
{
    SystemPrompt += EventsVsFunctions;
    SystemPrompt += TEXT("\n\n");
}

const FString NodeRouting = Assembler.GetKnowledgePackById(TEXT("node_routing"));
if (!NodeRouting.IsEmpty())
{
    SystemPrompt += NodeRouting;
    SystemPrompt += TEXT("\n\n");
}
```

**Note:** `recipe_routing` is already loaded at line 1435. Leave it for now -- removing it would save ~100 tokens but risks breaking backward compat until Task A2 is verified working.

**Dependencies:** Task A1 (to avoid duplicated content in node_routing.txt).

**Token cost:** +800 tokens to CLI system prompt.

**Verify:** Build succeeds. In orchestrated CLI mode, check logs for the system prompt content. It should contain design patterns, events vs functions, and node routing sections.

---

### Task B3: Verify latent-in-function validation completeness (Priority 3)

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

**What:** Audit that `bIsLatent` is set for ALL latent-capable scenarios. This is a verification task, not necessarily a code change.

**Current state (verified):**
- Line 1265: `delay` op explicitly sets `bIsLatent = true` -- CORRECT
- Line 1414: `call` ops check `Function->HasMetaData(TEXT("Latent"))` -- CORRECT
- Line 1535: Cast-aware fallback also checks `HasMetaData(TEXT("Latent"))` -- CORRECT
- Line 1670: Cast target search also checks `HasMetaData(TEXT("Latent"))` -- CORRECT

**Assessment:** `bIsLatent` is already comprehensive. The `Latent` metadata check on UFunction covers all Blueprint-accessible latent functions (Delay, AI MoveTo, PlayMontage, etc.). The explicit `delay` op flag is redundant but harmless.

The validator at `OlivePlanValidator.cpp` lines 357-406 correctly checks `ResolvedSteps[i].bIsLatent` for all steps.

**Result:** NO CODE CHANGES NEEDED. Priority 3 is already complete. Mark as verified.

**Verify:** Create a test plan with a `delay` op targeting a function graph. Confirm the validator returns `LATENT_IN_FUNCTION` error.

---

### Task B4: Enhance `DATA_WIRE_INCOMPATIBLE` self-correction message — mention SplitPin

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

**What:** The `DATA_WIRE_INCOMPATIBLE` guidance (lines 741-747) mentions `break_struct` and `~suffix` but does not mention SplitPin as an explicit alternative. Add it.

**Details:**
Replace lines 741-747:
```cpp
else if (ErrorCode == TEXT("DATA_WIRE_INCOMPATIBLE"))
{
    Guidance = TEXT("Two pins in the plan have incompatible types and no autocast exists. "
        "The wiring_errors array contains specific alternatives. "
        "Common fix: add a break_struct or make_struct intermediate step, "
        "or change the input to use a ~suffix for a split sub-pin component "
        "(e.g., '@get_loc.~ReturnValue_X' for Vector.X).");
}
```

With:
```cpp
else if (ErrorCode == TEXT("DATA_WIRE_INCOMPATIBLE"))
{
    Guidance = TEXT("Two pins in the plan have incompatible types and no autocast exists. "
        "The wiring_errors array contains specific alternatives. "
        "Common fixes:\n"
        "- Vector -> Float: use ~suffix for sub-pin (e.g., '@get_loc.~ReturnValue_X'). "
        "SplitPin is also available as a fallback.\n"
        "- Scalar -> Struct: add a make_struct intermediate step\n"
        "- Struct -> individual values: add a break_struct step\n"
        "- If all else fails, try editor.run_python for direct pin manipulation");
}
```

**Dependencies:** None.

**Token change:** +30 tokens per error occurrence (failure path only).

**Verify:** Build succeeds. Trigger a DATA_WIRE_INCOMPATIBLE error and verify the enriched message includes SplitPin mention.

---

### Task B5: Enhance `PLAN_RESOLVE_FAILED` self-correction — component context

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

**What:** When `PLAN_RESOLVE_FAILED` fires and the error message mentions a component class, the guidance should list the Blueprint's actual SCS components. Currently it says generic "check plan_json syntax" advice (lines 711-718 for the generic `PLAN_RESOLVE_FAILED` case).

**Problem:** The `BuildToolErrorMessage` method does not have access to the Blueprint -- it only receives `AssetContext` (the asset path string). To include SCS component info, we need to load the Blueprint.

**Details:**
In the generic `PLAN_RESOLVE_FAILED` handler (lines 711-718), enhance the guidance to include component information when the error message suggests a component-related failure.

Replace lines 711-718:
```cpp
else if (ErrorCode == TEXT("PLAN_RESOLVE_FAILED") || ErrorCode == TEXT("PLAN_LOWER_FAILED") || ErrorCode == TEXT("PLAN_EXECUTION_FAILED"))
{
    Guidance = TEXT("The plan failed during resolution or execution. Check the error details for which step failed. "
        "Common mistakes: set_var on a component (use get_var to read, then call setter), "
        "invented function names (search with blueprint.search_nodes first), "
        "wrong pin names (use @step.auto instead of guessing). "
        "Call olive.get_recipe with a query describing what you need (e.g., 'component reference' or 'spawn actor') "
        "to get the correct pattern. Fix the failing step and resubmit the corrected plan.");
}
```

With:
```cpp
else if (ErrorCode == TEXT("PLAN_RESOLVE_FAILED") || ErrorCode == TEXT("PLAN_LOWER_FAILED") || ErrorCode == TEXT("PLAN_EXECUTION_FAILED"))
{
    Guidance = TEXT("The plan failed during resolution or execution. Check the error details for which step failed. "
        "Common mistakes: set_var on a component (use get_var to read, then call setter), "
        "invented function names (search with blueprint.search_nodes first), "
        "wrong pin names (use @step.auto instead of guessing). "
        "Fix the failing step and resubmit the corrected plan.");

    // Enrich with SCS component list when asset context is available
    if (!AssetContext.IsEmpty())
    {
        UBlueprint* BP = Cast<UBlueprint>(
            StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetContext));
        if (BP && BP->SimpleConstructionScript)
        {
            TArray<FString> ComponentNames;
            for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
            {
                if (SCSNode && SCSNode->ComponentClass)
                {
                    ComponentNames.Add(FString::Printf(TEXT("%s (%s)"),
                        *SCSNode->GetVariableName().ToString(),
                        *SCSNode->ComponentClass->GetName()));
                }
            }
            if (ComponentNames.Num() > 0)
            {
                Guidance += FString::Printf(
                    TEXT("\nThis Blueprint's components: %s. "
                         "If calling a component function, wire a get_var for the component to the Target input."),
                    *FString::Join(ComponentNames, TEXT(", ")));
            }
        }
    }
}
```

**Dependencies:** Need to add `#include "Engine/Blueprint.h"`, `#include "Engine/SimpleConstructionScript.h"`, `#include "Engine/SCS_Node.h"` at the top of the file. Check if they are already included.

**Important:** `StaticLoadObject` must be called on the game thread. `BuildToolErrorMessage` is called from `Evaluate()` which is called from `OliveConversationManager` on the game thread, so this is safe.

**Verify:** Build succeeds. Trigger a PLAN_RESOLVE_FAILED on a Blueprint with components and verify the error message lists the components.

---

## Phase C: Feature Work

---

### Task C1: Pre-populate related asset context (Priority 6)

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`
**Header (if needed):** `Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h`

**What:** After resolving @-mentioned assets and before launching the CLI process, extract keywords from the user message, search the project index, and append a brief summary of potentially related assets.

**Details:**
In `SendMessageAutonomous()`, after the @-mention asset injection block (lines 467-481) and before the decomposition directive (line 486), add a new block:

```cpp
// Pre-populate related asset context: extract keywords from user message
// and search the project index for potentially relevant existing assets.
if (!IsContinuationMessage(UserMessage) && FOliveProjectIndex::Get().IsReady())
{
    TArray<FString> Keywords = ExtractKeywordsFromMessage(UserMessage);
    if (Keywords.Num() > 0)
    {
        TSet<FString> AlreadyMentioned(InitialContextAssetPaths);
        TArray<FOliveAssetInfo> RelatedAssets;

        for (const FString& Keyword : Keywords)
        {
            TArray<FOliveAssetInfo> Results = FOliveProjectIndex::Get().SearchAssets(Keyword, 5);
            for (const FOliveAssetInfo& Result : Results)
            {
                if (!AlreadyMentioned.Contains(Result.Path))
                {
                    AlreadyMentioned.Add(Result.Path);
                    RelatedAssets.Add(Result);
                }
            }
        }

        if (RelatedAssets.Num() > 0)
        {
            // Cap at 10 to avoid prompt bloat
            const int32 MaxRelated = FMath::Min(RelatedAssets.Num(), 10);
            EffectiveMessage += TEXT("\n\n## Existing Assets That May Be Relevant\n");
            for (int32 i = 0; i < MaxRelated; ++i)
            {
                EffectiveMessage += FString::Printf(TEXT("- %s (%s)\n"),
                    *RelatedAssets[i].Path, *RelatedAssets[i].AssetClass.ToString());
            }
            EffectiveMessage += TEXT("Use project.search if you need more details on any of these.\n");
        }
    }
}
```

Also add a new private helper method `ExtractKeywordsFromMessage`:

In the header (`OliveCLIProviderBase.h`), add in the `protected:` section:
```cpp
/** Extract meaningful keywords from a user message for asset search. */
TArray<FString> ExtractKeywordsFromMessage(const FString& Message) const;
```

In the source file, implement:
```cpp
TArray<FString> FOliveCLIProviderBase::ExtractKeywordsFromMessage(const FString& Message) const
{
    // Simple tokenizer: split on spaces/punctuation, keep words 4+ chars,
    // filter out common stop words and UE jargon that would match too broadly.
    static const TSet<FString> StopWords = {
        TEXT("the"), TEXT("and"), TEXT("for"), TEXT("with"), TEXT("that"),
        TEXT("this"), TEXT("from"), TEXT("have"), TEXT("make"), TEXT("create"),
        TEXT("add"), TEXT("set"), TEXT("get"), TEXT("use"), TEXT("when"),
        TEXT("blueprint"), TEXT("actor"), TEXT("component"), TEXT("variable"),
        TEXT("function"), TEXT("event"), TEXT("unreal"), TEXT("game"),
        TEXT("player"), TEXT("system"), TEXT("class"), TEXT("type"),
    };

    TArray<FString> Words;
    Message.ParseIntoArray(Words, TEXT(" "), true);

    TArray<FString> Keywords;
    for (FString& Word : Words)
    {
        // Strip @-mention prefix and punctuation
        Word.RemoveFromStart(TEXT("@"));
        Word = Word.TrimStartAndEnd();

        // Remove trailing punctuation
        while (Word.Len() > 0 && !FChar::IsAlnum(Word[Word.Len() - 1]))
        {
            Word.LeftChopInline(1);
        }

        Word = Word.ToLower();

        if (Word.Len() >= 4 && !StopWords.Contains(Word))
        {
            Keywords.AddUnique(Word);
        }
    }

    // Cap keywords to avoid excessive searches
    if (Keywords.Num() > 5)
    {
        Keywords.SetNum(5);
    }

    return Keywords;
}
```

**Dependencies:** Requires `#include "Index/OliveProjectIndex.h"` in the .cpp file. Check if already included. Also needs `FOliveAssetInfo` -- check the struct's header.

**Note on `InitialContextAssetPaths`:** This TArray is emptied at line 480 (consumed). The related asset search runs BEFORE it is emptied (insert between lines 481 and 486), so the `AlreadyMentioned` set will correctly exclude @-mentioned assets. Actually wait -- line 480 empties it. We need to copy it before the empty call, or just use a separate dedup set. The implementation above initializes `AlreadyMentioned` from `InitialContextAssetPaths`, but by this point it may be empty (line 480 empties it). Fix: move the `InitialContextAssetPaths.Empty()` to AFTER this new block, or capture the paths before they're emptied. Simplest: just don't try to dedup against @-mentions (the overlap is minimal and harmless).

**Revised approach:** Initialize `AlreadyMentioned` as empty. The worst case is a related asset duplicates an @-mentioned one, which is benign information.

**Token cost:** +100-200 tokens per run (depends on matches).

**Verify:** Build succeeds. Send an autonomous message like "create a gun system". Check logs for the "Existing Assets That May Be Relevant" section in the effective prompt. Verify it surfaces existing Blueprints with "gun" in the name (if any exist).

---

## Dependency Graph

```
A1 ─┐
    ├──> B1 (sandbox packs need deduplicated node_routing)
    ├──> B2 (CLI system prompt needs deduplicated node_routing)
A2 ──── (independent, content only)
A3 ──── (independent, string change in .cpp)
A4 ──── (independent, content only)
A5 ──── (independent, string change in .cpp)

B3 ──── (independent verification, no code changes expected)
B4 ──── (independent)
B5 ──── (independent)

C1 ──── (independent of all above)
```

All Phase A tasks are independent of each other.
B1 and B2 depend on A1 (to avoid loading duplicate "Three approaches" content).
All Phase B and C tasks are independent of each other.

## Recommended Execution Order

1. **Batch 1 (parallel, content only):** A1, A2, A4
2. **Batch 2 (parallel, .cpp string changes):** A3, A5
3. **Batch 3 (parallel, code changes):** B1, B2, B3, B4, B5
4. **Batch 4:** C1

After Batch 3, do a single compilation to verify all changes together.

## Summary Table

| Task | Files Changed | Type | Tokens | Dependencies |
|------|---------------|------|--------|-------------|
| A1 | node_routing.txt | Content trim | -30 | None |
| A2 | cli_blueprint.txt | Content add | +30 | None |
| A3 | OliveCLIProviderBase.cpp (line 497) | String change | +40 | None |
| A4 | cli_blueprint.txt | Content add | +30 | None |
| A5 | OliveCLIProviderBase.cpp (line 364) | String change | +15 | None |
| B1 | OliveCLIProviderBase.cpp (lines 347-386) | Code | +550 | A1 |
| B2 | OliveCLIProviderBase.cpp (lines 1458+) | Code | +800 | A1 |
| B3 | (verification only) | Audit | 0 | None |
| B4 | OliveSelfCorrectionPolicy.cpp (lines 741-747) | Code | +30/err | None |
| B5 | OliveSelfCorrectionPolicy.cpp (lines 711-718) | Code | +50/err | None |
| C1 | OliveCLIProviderBase.cpp + .h | Feature | +200/run | None |
