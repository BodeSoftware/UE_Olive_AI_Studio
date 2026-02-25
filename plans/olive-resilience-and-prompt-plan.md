# Olive AI: Resilience + Prompt Slimming Plan

## Goal

Make the executor resilient to common AI mistakes (code), then remove the prompt rules those code fixes make unnecessary, then replace the rigid recipe system with a lightweight reference lookup the AI can use when it needs help.

Three phases, done in order — each enables the next.

---

## Phase 1: Two Code Fixes

These make the system tolerant of AI mistakes that currently cascade into full failures. Once in, several prompt rules become unnecessary.

### Fix 1: Self-Loop Guard in PhaseWireExec

**Problem:** When the AI writes `"exec_outputs":{"True":"spawn"}` on the `spawn` step (wiring an exec output back to itself), `WireExecConnection` tries to connect a node to itself. UE rejects it, it counts as a `FailedConnectionCount`, which triggers `bPartial=true`, which triggers `PLAN_PARTIAL_SUCCESS`, which triggers a full transaction rollback. An entire 10-node plan gets nuked because of one nonsensical wire.

**File:** `OlivePlanExecutor.cpp` — `FOlivePlanExecutor::PhaseWireExec()`

**Location:** Inside the `exec_outputs` loop, approximately where the code iterates `for (const auto& ExecOut : Step.ExecOutputs)`. Currently at bundled_code.txt line ~37117.

**What to do:** After extracting `TargetStepId` from `ExecOut.Value`, add a check before calling `WireExecConnection`:

```
if TargetStepId == Step.StepId:
    - Log a warning: "Skipping self-loop on step 'X' (exec_output 'True' -> self)"
    - Add to Context.Warnings (NOT Context.WiringErrors, NOT FailedConnectionCount)
    - continue (skip the WireExecConnection call)
```

**Key detail:** This must NOT increment `FailedConnectionCount`. If it does, it still triggers partial success → rollback. It should be treated as a warning the AI can see in the result, not an error.

**Note:** The layout engine (`BuildExecGraph`, bundled_code line ~35648) already has a self-loop skip, but that's only for layout ordering — it doesn't prevent the actual wiring attempt in Phase 3.

**Test:** Submit a plan where one step's `exec_outputs` points to itself. Verify: no failure counted, warning appears in result, all other wires succeed, transaction commits.

---

### Fix 2: Partial Success Commits Instead of Rolling Back

**Problem:** When `apply_plan_json` creates all nodes but some wiring fails (e.g., 1 out of 6 connections), the current flow is:

1. Executor sets `bPartial=true` on the result (bundled_code line ~38250)
2. Tool handler converts this to `FOliveWriteResult::ExecutionError("PLAN_PARTIAL_SUCCESS", ...)` with `bSuccess=false` (bundled_code line ~30301)
3. Write pipeline sees `!ExecuteResult.bSuccess` → calls `Transaction->Cancel()` → all nodes rolled back (bundled_code line ~32397)
4. Self-correction tells the AI "nodes already exist, fix wiring with connect_pins" (bundled_code line ~68821)
5. But nodes DON'T exist — they were rolled back. AI tries to use node IDs that no longer exist → cascading failure → loop detection → Error state

The self-correction guidance is a lie. The nodes are gone.

**File:** `OliveBlueprintToolHandlers.cpp` — the `apply_plan_json` handler, inside the `if (PlanResult.bPartial)` block.

**Location:** Bundled_code line ~30289, the block that starts with `if (PlanResult.bPartial)`.

**What to do:** Instead of returning `FOliveWriteResult::ExecutionError(...)`, return a success result with warnings:

```
When PlanResult.bPartial is true:
    - Keep all the ResultData fields (step_to_node_map, wiring_errors, pin_manifests, etc.)
    - Set ResultData "success" = true  (was false)
    - Set ResultData "status" = "partial_success"  (keep this for AI visibility)
    - Remove the error_code field (or keep it as informational, not as an error)
    - Return FOliveWriteResult::Success() with the ResultData attached
    - Do NOT return FOliveWriteResult::ExecutionError()
```

**Effect on pipeline:** The write pipeline sees `bSuccess=true` → transaction commits → nodes persist in the graph. The AI gets back `step_to_node_map` with real node IDs and `wiring_errors` showing exactly which connections failed. It can then use `blueprint.connect_pins` to fix just the broken wires.

**Self-correction update needed:** The guidance at bundled_code line ~68821 currently says "Do NOT resubmit the entire plan -- the nodes already exist." This becomes TRUE after this fix. But since partial success is no longer an error, the self-correction `PLAN_PARTIAL_SUCCESS` case may not trigger at all (self-correction checks `bSuccess`). Two options:

- **Option A:** Remove the `PLAN_PARTIAL_SUCCESS` case from self-correction entirely — it's now a success, so the AI just reads the warnings in the normal result and acts on them.
- **Option B:** Keep it but trigger it differently — check for `status == "partial_success"` in the result data rather than `bSuccess == false`. This gives the AI explicit guidance on using connect_pins.

Option A is simpler. The result already contains `wiring_errors` with specific pin names and suggestions — the AI has everything it needs without special self-correction guidance.

**Test:** Submit a plan with one deliberately wrong data wire (e.g., connecting a float to a string pin). Verify: all nodes persist in the graph, step_to_node_map has correct IDs, wiring_errors shows the failure, transaction was NOT rolled back, AI can call connect_pins with the returned node IDs.

---

## Phase 2: Slim the CLI System Prompt

With Phase 1 in place, the executor handles mistakes gracefully. We can remove prompt rules that existed only to prevent those mistakes.

### Current CLI prompt assembly

The CLI system prompt is built in `FOliveCLIProviderBase::BuildSystemPrompt()` (bundled_code line ~77720). It loads these pieces in order:

1. **Project context** — from `Assembler.GetProjectContext()` (keep)
2. **Policy context** — from `Assembler.GetPolicyContext()` (keep)
3. **recipe_routing.txt** — via `GetKnowledgePackById("recipe_routing")` (~185 words)
4. **cli_blueprint.txt** — via `GetKnowledgePackById("cli_blueprint")` (~445 words)
5. **Tool schemas** — via `FOliveCLIToolSchemaSerializer::Serialize()` (keep as-is)
6. **Tool call format** — via `FOliveCLIToolCallParser::GetFormatInstructions()` (keep as-is)

Total knowledge content: ~630 words before tool schemas. Target: ~350 words.

### What to remove from cli_blueprint.txt

These are now handled by code and don't need prompt space:

| Rule to remove | Why it's safe to remove |
|---|---|
| "SpawnActor uses SpawnTransform, NOT separate Location/Rotation" | `ExpandPlanInputs` auto-synthesizes MakeTransform if the AI uses Location/Rotation. Works either way now. |
| "First impure step auto-chains from entry node — do NOT give it an exec_after" | `PhaseWireExec` auto-chain handles this. If the AI includes exec_after, it's harmless (just redundant). If it doesn't, auto-chain wires it. |
| "Node IDs are scoped per graph — a node from 'Fire' function does NOT exist in 'EventGraph'" | With Fix 2, nodes persist and the AI gets real IDs back. The scenario where the AI guesses at K2Node IDs happens when nodes are rolled back — which Fix 2 prevents. |
| "Pin references use DOT separator: node_id.pin_name (NOT colon)" | The executor error message already tells the AI this when it gets it wrong. Self-correction handles it. |
| "preview_plan_json is optional. Prefer calling apply_plan_json directly." | The CLI wrapper already says this. Duplicated in both files. |
| The full variable type table (all 12 entries) | Keep a compact 4-line version with just the tricky ones: object refs, BP refs (_C), class refs, arrays. Basic types (Float, Bool, Int, String, Vector) don't need examples. |

### What to keep in cli_blueprint.txt

- Workflow order: create → components/variables → apply_plan_json (3 lines)
- Plan JSON v2.0 schema: ops list, wire syntax (@step.auto, exec_after, exec_outputs) (5 lines)
- One compact JSON example showing the pattern (5 lines)
- Compact variable type quick-ref for the non-obvious types only (4 lines)
- Function graph rule: "entry node auto-created, no event step needed" (1 line)

Target: ~250 words, down from ~445.

### What to remove from recipe_routing.txt

| Rule to remove | Why |
|---|---|
| "SpawnActor uses SpawnTransform (Transform), NOT separate Location/Rotation pins" | Duplicate, also handled by code |
| "Object variable refs need class_name..." | Already in cli_blueprint.txt |
| "Pin refs use DOT..." | Duplicate |
| "Node IDs are scoped per graph..." | Duplicate |
| "exec_after takes step_ids from YOUR plan, NOT K2Node IDs from blueprint.read" | Fix 2 eliminates the scenario |
| "Never invent node IDs -- only use IDs returned by tool results" | Fix 2 eliminates the scenario |
| "preview_plan_json is optional..." | Duplicate |

### What to keep in recipe_routing.txt

- The routing table: which recipe to use for which task type (3 lines)
- "NEW blueprint → create + components/variables + apply_plan_json" (1 line)
- "MODIFY existing → project.search → blueprint.read → write tools" (1 line)
- "SMALL EDIT → read_event_graph → add_node + connect_pins" (1 line)
- "MULTI-ASSET → create ALL structures first → wire each graph" (1 line)
- "Keep plans under 12 steps" (1 line)
- "FIX wiring → use pin_manifests from apply result" (1 line)

Target: ~100 words, down from ~185.

### No C++ changes needed for Phase 2

This is purely text file edits to `cli_blueprint.txt` and `recipe_routing.txt`. No code, no recompile. Just edit the files and restart the editor.

---

## Phase 3: Reference Tool Replacing Recipes

### Current state

`olive.get_recipe` exists in code (registered, handler implemented, recipe loading from disk). It uses a rigid `category/name` lookup — the AI must know the exact category ("blueprint") and exact recipe name ("create", "modify", "fix_wiring", etc.) to get content. The routing table in `recipe_routing.txt` tells it which to call.

6 recipe files exist on disk under `Content/SystemPrompts/Knowledge/recipes/blueprint/`:
- create.txt, modify.txt, edit_existing_graph.txt, fix_wiring.txt, multi_asset.txt, variables_components.txt

Each is 100-200 words of workflow steps, examples, and gotchas.

### Problem with current approach

1. **The AI has to know what to search for.** It has to decide "I'm doing a create task" and call `get_recipe(blueprint, create)`. If it doesn't recognize the task type, it doesn't call anything.
2. **The recipes duplicate the system prompt.** Much of what's in create.txt is already in cli_blueprint.txt. The AI gets the same rules twice.
3. **Rigid structure.** If the AI hits a SpawnActor error, it can't search for "spawn actor transform" — it has to know that lives in the "create" recipe.
4. **Wastes a turn.** The AI spends one of its limited iterations calling get_recipe before it starts working.

### New approach: keyword-searchable reference lookup

**Rename/refactor** `olive.get_recipe` → `olive.reference` (or keep the name, change the behavior).

**Change the input** from `{category, name}` to `{query}` — a free-text search string.

**Change the matching** from exact key lookup to keyword matching against tags/content:
- Each reference entry has a set of keywords/tags
- The query is split into terms and matched against tags
- Return the best-matching entry (or top 2-3 if close)
- Fallback: if no tags match, do substring search on content

**Change the content format.** Reference entries become short, self-contained snippets (~50-100 words each) rather than full workflow docs:

```
Example reference entry (file: spawn_actor.txt):

TAGS: spawn actor transform location rotation
---
SpawnActor node uses a single SpawnTransform pin (Transform type).
To build the transform, add a make_struct step:
  {step_id:"tf", op:"make_struct", target:"Transform", inputs:{Location:"@loc.auto"}}
  {step_id:"spawn", op:"spawn_actor", inputs:{SpawnTransform:"@tf.auto"}}
If you pass Location/Rotation directly, the executor auto-expands them.
```

```
Example reference entry (file: function_graph.txt):

TAGS: function graph entry exec_after first step
---
Function graphs have an auto-created entry node. Do NOT add an event step.
First impure step needs no exec_after (auto-chains from entry).
Subsequent impure steps MUST have exec_after.
graph_target in apply_plan_json = function name, e.g. "Fire".
```

```
Example reference entry (file: object_variable_type.txt):

TAGS: variable type object class blueprint _C subclass
---
Object ref: {"category":"object","class_name":"Actor"}
Blueprint ref: {"category":"object","class_name":"BP_Gun_C"} (append _C)
Class ref (TSubclassOf): {"category":"class","class_name":"Actor"}
Array: {"category":"array","value_type":{"category":"float"}}
Shorthand works for basics: "Float", "Boolean", "Vector", "String"
```

### How the AI uses it

The AI does NOT need to call this before every task. It works when it needs it:

- AI tries to create a SpawnActor step and gets a wiring error → calls `olive.reference("spawn actor wiring")` → gets the pattern
- AI isn't sure how to type a variable → calls `olive.reference("object variable type blueprint class")` → gets the format
- AI needs to wire a function graph → calls `olive.reference("function graph entry")` → gets the rules

The tool description in the schema carries a one-liner:
```
"Search for patterns, examples, and gotchas when you encounter unfamiliar tasks or errors."
```

No routing table needed in the system prompt. The AI just searches when it's stuck.

### Implementation changes

**File:** `OliveCrossSystemToolHandlers.cpp`

1. **Tool registration:** Change schema from `{category?: string, name?: string}` to `{query: string}`. Update tool description.

2. **HandleGetRecipe → HandleReference:** Replace the category/name lookup with keyword search:
   - Split query into lowercase terms
   - For each reference entry, check how many query terms appear in its tags
   - Return the entry (or entries) with the most tag matches
   - If no tag matches, fall back to substring search on content
   - Return top 1-3 matches

3. **LoadRecipeLibrary → LoadReferenceLibrary:** Change the file format parsing:
   - Each .txt file starts with `TAGS: keyword1 keyword2 keyword3` then `---` then content
   - Store as `TMap<FString, FReferenceEntry>` where `FReferenceEntry` has `Tags` (TArray<FString>) and `Content` (FString)
   - Keep the directory scanning / manifest approach for organization, or simplify to just scanning all .txt files in a flat or shallow directory

4. **Recipe routing in system prompt:** The `recipe_routing.txt` entry "Call olive.get_recipe(category, name) before starting an unfamiliar task type" becomes unnecessary. Remove it or replace with a one-liner: "Use olive.reference(query) to look up patterns when stuck."

**File changes on disk:**

- Restructure `Content/SystemPrompts/Knowledge/recipes/blueprint/*.txt` into compact reference entries with TAGS headers
- Can keep the directory structure or flatten — doesn't matter for keyword search
- Each entry: 50-100 words max, one focused topic, one example if needed

### What this gets us

- **No routing table in the prompt** — saves ~100 words
- **AI searches when it needs to** — no wasted turns calling get_recipe proactively
- **Scales without prompt bloat** — add 50 reference entries, prompt stays the same size
- **Works across all providers** — tool is registered globally, any provider path can use it
- **Natural error recovery** — AI hits an error, searches for help, gets targeted advice

---

## Summary: Implementation Order

| Phase | What | Files Changed | Estimated Size |
|---|---|---|---|
| **1a** | Self-loop guard in PhaseWireExec | OlivePlanExecutor.cpp | ~8 lines |
| **1b** | Partial success commits | OliveBlueprintToolHandlers.cpp | ~15 lines |
| **1b** | Update/remove self-correction case | OliveSelfCorrectionPolicy.cpp | ~5 lines |
| **2** | Slim cli_blueprint.txt | cli_blueprint.txt (disk) | text edit |
| **2** | Slim recipe_routing.txt | recipe_routing.txt (disk) | text edit |
| **3** | Refactor recipe tool to reference search | OliveCrossSystemToolHandlers.cpp | ~60 lines |
| **3** | Restructure recipe files | recipes/*.txt (disk) | text edits |

Phase 1 is prerequisite for Phase 2. Phase 2 is independent of Phase 3 but they complement each other. Phase 3 can be done anytime after Phase 1.

---

## Testing Plan

### Phase 1 tests
- Submit a plan with a self-loop exec_output → verify warning (not error), other wires succeed, transaction commits
- Submit a plan with one wrong data wire → verify nodes persist, step_to_node_map returned, wiring_errors show the failure, AI can fix with connect_pins

### Phase 2 tests
- Run the gun+bullet creation task with the slimmed prompts → verify the AI still follows the right workflow without the removed rules
- Deliberately trigger a SpawnActor with Location/Rotation inputs → verify ExpandPlanInputs handles it (code guardrail works)
- Deliberately omit exec_after on first function step → verify auto-chain handles it

### Phase 3 tests
- Call `olive.reference("spawn actor transform")` → verify correct entry returned
- Call `olive.reference("variable type object")` → verify type format entry returned
- Call with a query that matches nothing → verify graceful empty result
- End-to-end: run a task where the AI hits an error, calls reference, and self-corrects using the returned pattern
