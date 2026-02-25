# Resilience + Prompt Slimming: Implementation Tasks

**Source plan:** `plans/olive-resilience-and-prompt-plan.md`
**Produced by:** Architect, 2026-02-24
**Execution order:** Phase 1a -> Phase 1b -> Phase 2 -> Phase 3

---

## Current State Summary

### OlivePlanExecutor.cpp (1599 lines)

`PhaseWireExec()` (lines 388-793) iterates plan steps and processes two kinds of exec wiring:

1. **exec_after** (lines 398-430): Wires from a predecessor step's primary exec output to this step's exec input. No self-loop check exists.
2. **exec_outputs** (lines 436-471): Iterates `Step.ExecOutputs` map. For each `{PinHint, TargetStepId}`, calls `WireExecConnection()`. **No self-loop check exists.** If `TargetStepId == Step.StepId`, the code tries to connect a node to itself, UE rejects it, `FailedConnectionCount` increments, and the overall result becomes `bPartial=true`.

After both explicit wiring blocks, there are two auto-chain blocks (function entry auto-chain at lines 498-637, event auto-chain at lines 649-793).

`AssembleResult()` (lines 1546-1598) sets `Result.bPartial = Result.bSuccess && (FailedConnectionCount > 0 || FailedDefaultCount > 0)`.

### OliveBlueprintToolHandlers.cpp (apply_plan_json handler)

The `PlanResult.bPartial` block is at lines 6546-6577. Current behavior:
- Sets `ResultData["success"] = false`, `ResultData["status"] = "partial_success"`, `ResultData["error_code"] = "PLAN_PARTIAL_SUCCESS"`
- Returns `FOliveWriteResult::ExecutionError("PLAN_PARTIAL_SUCCESS", ...)` with `bSuccess=false`
- Copies `CreatedNodeIds` from `StepToNodeMap` into the result

**Critical consequence:** The write pipeline at `OliveWritePipeline.cpp:194-205` sees `!ExecuteResult.bSuccess`, calls `Transaction->Cancel()`, and rolls back ALL nodes. The AI then receives `step_to_node_map` with node IDs that no longer exist.

### OliveSelfCorrectionPolicy.cpp

The `PLAN_PARTIAL_SUCCESS` case is at lines 273-281. Guidance says "Do NOT resubmit the entire plan -- the nodes already exist." This is currently a lie because the transaction rolls back. After Fix 2, this guidance would become correct, but since partial success will no longer trigger `HasToolFailure()` (it checks `bSuccess==false` at line 154), this case will never be reached.

### cli_blueprint.txt (70 lines)

Contains workflows, plan JSON schema, ops list, wiring syntax, function graph rules, function resolution, variable types (full 12-entry table), and 10 rules at the bottom. Several rules are now handled by code (SpawnActor transform expansion, auto-chain, node ID scoping, pin separator format, preview_plan_json advice).

### recipe_routing.txt (21 lines)

Contains recipe routing instructions, 6 quick rules, and 8 additional rules. Heavy duplication with `cli_blueprint.txt` and with code-handled behavior.

### OliveCrossSystemToolHandlers.cpp (recipe system)

`HandleGetRecipe()` (lines 1148-1259) takes `{category, name}` params. Uses rigid key-based lookup into `RecipeLibrary` TMap. `LoadRecipeLibrary()` (lines 1000-1121) loads from manifest-driven directory structure with format version checking and tool reference validation.

The manifest (`_manifest.json`) already has `tags` arrays per recipe -- these are stored but NOT used by the current handler.

### Recipe files (6 files)

All under `Content/SystemPrompts/Knowledge/recipes/blueprint/`. Each is 20-50 lines with WORKFLOW, EXAMPLE, GOTCHAS sections. Significant content overlap between files and with `cli_blueprint.txt`.

---

## Assumptions

1. **exec_after self-loops are also possible** but are much less likely (the AI would write `"exec_after":"spawn"` on the step whose `step_id` is `"spawn"`). Both exec_after and exec_outputs self-loops should be guarded for completeness, but the plan only specifies exec_outputs. I recommend guarding both.

2. **FOliveWriteResult::Success()** accepts a `TSharedPtr<FJsonObject>` and sets `bSuccess=true`. The pipeline sees `bSuccess=true` and does NOT cancel the transaction. This is the mechanism by which Fix 2 works.

3. **CreatedNodeIds on FOliveWriteResult** is a `TArray<FString>` that the pipeline's verification stage (Stage 5) uses for structural checks. It must be populated even on the success path.

4. **The self-correction system only triggers on `bSuccess==false`** (checked in `HasToolFailure()` at line 154). After Fix 2, partial success returns `bSuccess=true` in the ResultData, so `HasToolFailure()` returns false, and the `PLAN_PARTIAL_SUCCESS` self-correction case is unreachable. We should remove it (Option A from the plan).

5. **Phase 3 can reuse existing manifest tags** from `_manifest.json`. The tags field is already parsed and could be used for keyword matching, reducing the amount of format change needed.

6. **Tool name change** (`olive.get_recipe` -> `olive.reference`): The plan suggests this but says "or keep the name." I recommend keeping `olive.get_recipe` as the tool name for backward compatibility and changing only the schema and behavior. This avoids breaking any prompt references or tool pack configurations. Decision deferred to user.

---

## Phase 1a: Self-Loop Guard in PhaseWireExec

### Task 1a-1: Add self-loop guard in exec_outputs wiring

**Objective:** Prevent `WireExecConnection()` from being called when a step's exec output targets itself.

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

**Exact location:** Lines 436-471, inside the `exec_outputs` loop. Insert the guard immediately after line 439 (`const FString& TargetStepId = ExecOut.Value;`) and before line 441 (`UE_LOG ... Exec wire`).

**What to add:**
```cpp
// Self-loop guard: skip if exec output targets the same step
if (TargetStepId == Step.StepId)
{
    UE_LOG(LogOlivePlanExecutor, Warning,
        TEXT("  Skipping self-loop on step '%s' (exec_output '%s' -> self)"),
        *Step.StepId, *PinHint);
    Context.Warnings.Add(FString::Printf(
        TEXT("Step '%s': exec_output '%s' targets itself (self-loop skipped)"),
        *Step.StepId, *PinHint));
    continue;
}
```

**Constraints:**
- Must NOT increment `FailedConnectionCount`. This is a warning, not an error.
- Must add to `Context.Warnings`, not `Context.WiringErrors`.
- The `continue` skips the `WireExecConnection()` call entirely.

**Also guard exec_after (recommended):** In the `exec_after` block (lines 398-430), insert a similar guard after line 398 (`if (!Step.ExecAfter.IsEmpty())`) and before line 400 (`UE_LOG ... Exec wire`):
```cpp
if (Step.ExecAfter == Step.StepId)
{
    UE_LOG(LogOlivePlanExecutor, Warning,
        TEXT("  Skipping self-loop on step '%s' (exec_after references self)"),
        *Step.StepId);
    Context.Warnings.Add(FString::Printf(
        TEXT("Step '%s': exec_after references itself (self-loop skipped)"),
        *Step.StepId));
    // Do not attempt wiring, do not increment FailedConnectionCount
}
else
{
    // ... existing exec_after wiring code (lines 400-430) ...
}
```

Note: wrapping the existing code in an `else` block is a clean approach. Alternatively, use `continue` but since exec_after is NOT inside a for loop (it is a simple if block inside the outer step loop), `continue` would skip to the next plan step which would also skip exec_outputs processing for this step. That is wrong. The guard must use an `if/else` pattern, not `continue`, because the exec_after block and exec_outputs block are sequential within the same step iteration.

Wait -- looking more carefully, the outer loop is `for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)` and both exec_after and exec_outputs are processed for each step. A `continue` on exec_after would skip exec_outputs for that step. So the correct pattern for exec_after is:

```cpp
if (!Step.ExecAfter.IsEmpty())
{
    if (Step.ExecAfter == Step.StepId)
    {
        // Log warning, add to Context.Warnings
        // Do NOT call WireExecConnection, do NOT increment FailedConnectionCount
    }
    else
    {
        // Existing exec_after wiring code
    }
}
```

**Acceptance criteria:**
- A plan with `"exec_outputs":{"Then":"spawn"}` on a step with `step_id:"spawn"` produces a warning, not a wiring error
- `FailedConnectionCount` is 0 for the self-loop case
- `bPartial` remains false (assuming no other wiring failures)
- The warning text is visible in the result JSON under `warnings`
- All other exec wires in the same plan still succeed normally

---

## Phase 1b: Partial Success Commits Instead of Rolling Back

### Task 1b-1: Change partial success to return Success instead of ExecutionError

**Objective:** Make partial success commit the transaction so nodes persist in the graph.

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

**Exact location:** Lines 6546-6577, the `if (PlanResult.bPartial)` block.

**Current code (lines 6546-6577):**
```cpp
if (PlanResult.bPartial)
{
    const int32 TotalFailures = PlanResult.ConnectionsFailed + PlanResult.DefaultsFailed;
    FString PartialMessage = FString::Printf(
        TEXT("%d nodes created but %d connections FAILED. "
             "See wiring_errors and pin_manifests for details."),
        PlanResult.StepToNodeMap.Num(),
        TotalFailures);

    ResultData->SetStringField(TEXT("message"), PartialMessage);
    ResultData->SetStringField(TEXT("status"), TEXT("partial_success"));
    ResultData->SetBoolField(TEXT("success"), false);
    ResultData->SetStringField(TEXT("error_code"), TEXT("PLAN_PARTIAL_SUCCESS"));

    FOliveWriteResult PartialResult = FOliveWriteResult::ExecutionError(
        TEXT("PLAN_PARTIAL_SUCCESS"),
        PartialMessage,
        TEXT("Use blueprint.read on the target graph with include_pins:true to see actual pin names, "
             "then fix failed connections with connect_pins/set_pin_default."));
    PartialResult.ResultData = ResultData;

    // Still provide created node IDs for the pipeline's verification stage
    TArray<FString> CreatedNodeIds;
    CreatedNodeIds.Reserve(PlanResult.StepToNodeMap.Num());
    for (const auto& Pair : PlanResult.StepToNodeMap)
    {
        CreatedNodeIds.Add(Pair.Value);
    }
    PartialResult.CreatedNodeIds = MoveTemp(CreatedNodeIds);

    return PartialResult;
}
```

**Replace with:**
```cpp
if (PlanResult.bPartial)
{
    const int32 TotalFailures = PlanResult.ConnectionsFailed + PlanResult.DefaultsFailed;
    FString PartialMessage = FString::Printf(
        TEXT("%d nodes created, %d connections succeeded, %d connections FAILED. "
             "Nodes are committed. Use wiring_errors and step_to_node_map to fix "
             "failed connections with connect_pins/set_pin_default."),
        PlanResult.StepToNodeMap.Num(),
        PlanResult.ConnectionsSucceeded,
        TotalFailures);

    ResultData->SetStringField(TEXT("message"), PartialMessage);
    ResultData->SetStringField(TEXT("status"), TEXT("partial_success"));
    ResultData->SetBoolField(TEXT("success"), true);
    // Deliberately NOT setting error_code -- this is not an error

    FOliveWriteResult PartialResult = FOliveWriteResult::Success(ResultData);

    // Provide created node IDs for the pipeline's verification stage
    TArray<FString> CreatedNodeIds;
    CreatedNodeIds.Reserve(PlanResult.StepToNodeMap.Num());
    for (const auto& Pair : PlanResult.StepToNodeMap)
    {
        CreatedNodeIds.Add(Pair.Value);
    }
    PartialResult.CreatedNodeIds = MoveTemp(CreatedNodeIds);

    return PartialResult;
}
```

**Key changes:**
1. `ResultData["success"]` changed from `false` to `true`
2. `ResultData["error_code"]` field removed entirely
3. `FOliveWriteResult::ExecutionError(...)` replaced with `FOliveWriteResult::Success(ResultData)`
4. Message updated to mention that nodes are committed
5. `CreatedNodeIds` still populated for Stage 5 verification

**Constraints:**
- The `ResultData` already contains `step_to_node_map`, `wiring_errors`, `pin_manifests`, `conversion_notes`, `self_correction_hint` -- all of these STAY. Only the success/error_code fields change.
- The self_correction_hint at lines 6519-6528 is set BEFORE this block (it checks `bHasWiringErrors && PlanResult.bSuccess` at line 6522). Since `PlanResult.bSuccess` is true for partial success (all nodes created), the hint IS included. This is correct behavior -- the AI sees the hint in the success result.

**Gotcha:** Check that `FOliveWriteResult::Success()` does not set `bSuccess` to `true` on the passed-in ResultData. Looking at the Success factory method, it creates a new FOliveWriteResult with `bSuccess=true` and stores the Data pointer. The ResultData JSON object already has `success:true` set explicitly above, so there is no conflict. But verify that Success() does not also mutate the JSON -- it should not, based on the pattern.

### Task 1b-2: Remove PLAN_PARTIAL_SUCCESS case from self-correction

**Objective:** Remove dead code -- since partial success now returns `bSuccess=true`, `HasToolFailure()` returns false, and the PLAN_PARTIAL_SUCCESS guidance is never reached.

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

**Exact location:** Lines 273-281.

**Current code:**
```cpp
else if (ErrorCode == TEXT("PLAN_PARTIAL_SUCCESS"))
{
    Guidance = TEXT("All nodes were created but some wiring failed. "
        "Look at the wiring_errors array in the result. "
        "Use blueprint.read on the target graph with include_pins:true "
        "to see actual pin names, then use blueprint.connect_pins or "
        "blueprint.set_pin_default to fix each failed connection. "
        "Do NOT resubmit the entire plan -- the nodes already exist.");
}
```

**Action:** Delete this entire `else if` block (lines 273-281). The case before it (`PLAN_RESOLVE_FAILED` etc. at lines 269-272) connects directly to the next case after it (`GRAPH_DRIFT` at lines 282-289).

**Constraints:**
- Verify there are no other references to `PLAN_PARTIAL_SUCCESS` in the codebase besides the handler and this self-correction case. If there are references in prompt text or knowledge files, note them for Phase 2 cleanup.
- The error code string `"PLAN_PARTIAL_SUCCESS"` may still appear in logs from old runs. This is fine -- it's just a string, not a compile-time reference.

**Acceptance criteria:**
- Code compiles cleanly after removal
- When partial success occurs, self-correction does NOT trigger (the AI sees the success result with wiring_errors and acts on them directly)
- The transaction is NOT rolled back -- nodes persist in the graph
- The AI can call `blueprint.connect_pins` with node IDs from `step_to_node_map` and they resolve correctly

---

## Phase 2: Slim the CLI System Prompt

### Task 2-1: Slim cli_blueprint.txt

**Objective:** Remove rules now handled by code, shrink from ~70 lines to ~45 lines. Target ~250 words.

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**Lines to REMOVE:**

1. **Line 42** -- `The first impure step auto-chains from the entry node -- do NOT give it an exec_after.`
   Why: Auto-chain in PhaseWireExec handles this. If the AI includes exec_after, it is harmless (redundant wire). If it omits it, auto-chain wires it.

2. **Line 66** -- `SpawnActor uses SpawnTransform (Transform type), NOT separate Location/Rotation pins.`
   Why: `ExpandPlanInputs` auto-synthesizes MakeTransform if the AI uses Location/Rotation directly.

3. **Line 67** -- `Pin references use DOT separator: node_id.pin_name (NOT colon).`
   Why: The executor error message already tells the AI this when it gets it wrong. Self-correction handles it.

4. **Line 68** -- `Node IDs are scoped per graph -- a node from "Fire" function does NOT exist in "EventGraph".`
   Why: With Fix 2, nodes persist and the AI gets real IDs back. The scenario where the AI guesses at K2Node IDs happens when nodes are rolled back -- which Fix 2 prevents. Also, the `BP_REMOVE_NODE_FAILED` self-correction already provides this guidance.

5. **Line 63** -- `preview_plan_json is optional. Prefer calling apply_plan_json directly.`
   Why: Duplicated in recipe_routing.txt and already stated in the tool description.

6. **Lines 51-57** -- The variable type table. Keep only the tricky entries (object, blueprint, class, array, enum). Remove basic types (Float, Bool, Int, String, Vector, Rotator, Transform) that don't need examples since shorthand works.

**Lines to KEEP (all other lines remain):**

- Lines 1-2: Role + think-before-calling
- Lines 4-19: All workflows (CREATE, MODIFY, SMALL EDIT, MULTI-ASSET)
- Lines 21-38: Plan JSON schema, ops, wires sections
- Lines 39-44: Function graph rules (keep lines 39-41, 43-44; remove line 42)
- Lines 46-49: Function resolution section
- Lines 50, 58: Variable types header + remaining entries
- Lines 60-62, 64-65, 69: Remaining rules

**Rewritten variable types section (replacing lines 50-57):**
```
## Variable Types
Shorthand works for basics: "Float", "Boolean", "Integer", "String", "Vector", "Rotator", "Transform"
Object ref: {"category":"object","class_name":"Actor"} (use UE class name)
Blueprint ref: {"category":"object","class_name":"BP_Gun_C"} (append _C)
Class ref (TSubclassOf): {"category":"class","class_name":"Actor"}
Array: {"category":"array","value_type":{"category":"float"}}
Enum: {"category":"byte","enum_name":"ECollisionChannel"}
```

**Constraints:**
- This is a pure text edit -- no C++ recompile needed
- The file is loaded at startup by `FOlivePromptAssembler`; changes take effect on next editor launch
- Do NOT change the overall structure (headings, formatting) -- just remove specific lines/sections
- Keep the file well-organized and readable

**Acceptance criteria:**
- File is ~45 lines, ~250 words
- All removed rules are verifiably handled by code
- The retained content is sufficient for a competent AI to follow the workflow

### Task 2-2: Slim recipe_routing.txt

**Objective:** Remove rules duplicated with cli_blueprint.txt or handled by code. Shrink from 21 lines to ~12 lines. Target ~100 words.

**File:** `Content/SystemPrompts/Knowledge/recipe_routing.txt`

**Lines to REMOVE:**

1. **Line 1** -- `Call olive.get_recipe(category, name) before starting an unfamiliar task type.`
   Why: Phase 3 changes this to keyword search; also, the AI should NOT waste a turn calling get_recipe proactively.

2. **Line 2** -- `BLUEPRINT: create, modify, fix_wiring, variables_components, edit_existing_graph, multi_asset`
   Why: Rigid category listing goes away with Phase 3.

3. **Line 14** -- `preview_plan_json is optional -- prefer calling apply_plan_json directly`
   Why: Duplicate of cli_blueprint.txt.

4. **Line 15** -- `SpawnActor uses SpawnTransform (Transform), NOT separate Location/Rotation pins`
   Why: Handled by code (ExpandPlanInputs).

5. **Line 16** -- `Object variable refs need class_name: {"category":"object","class_name":"BP_Gun_C"} (append _C)`
   Why: Already in cli_blueprint.txt variable types section.

6. **Line 17** -- `Pin refs use DOT: node_id.pin_name (NOT colon)`
   Why: Handled by self-correction.

7. **Line 18** -- `Node IDs are scoped per graph -- don't use node_ids across different graphs`
   Why: Handled by self-correction + Fix 2.

8. **Line 20** -- `exec_after takes step_ids from YOUR plan, NOT K2Node IDs from blueprint.read`
   Why: Fix 2 eliminates the scenario. Also covered by INVALID_EXEC_REF self-correction.

9. **Line 21** -- `Never invent node IDs -- only use IDs returned by tool results`
   Why: Fix 2 eliminates the scenario.

**Replacement content:**
```
## Routing
- Use olive.get_recipe(query) to look up patterns when stuck or encountering errors.
- NEW blueprint: create + components/variables + apply_plan_json (ALL graph nodes in one call)
- MODIFY existing: project.search (find path) -> blueprint.read -> then write tools
- SMALL EDIT (1-2 nodes): read_event_graph -> add_node + connect_pins
- MULTI-ASSET (2+ blueprints): create ALL structures first -> wire each graph -> budget iterations per asset
- FIX wiring: use wiring_errors and pin_manifests from the apply result
- NEVER call blueprint.read before blueprint.create
- NEVER use add_node one-at-a-time for 3+ nodes -- use plan_json
- Keep plans under 12 steps; split complex logic into multiple functions
```

**Note:** Line 1 above references `olive.get_recipe(query)` anticipating Phase 3's schema change. If Phase 3 is deferred, use the current `olive.get_recipe(category, name)` syntax instead. Since Phase 2 depends on Phase 1 but Phase 3 is independent, the coder should ask which format to use when implementing this line.

**Constraints:**
- Same as Task 2-1 -- pure text edit, no recompile
- Keep the `## Routing` header for structure

**Acceptance criteria:**
- File is ~12 lines, ~100 words
- No content duplicated with cli_blueprint.txt
- No rules that are handled by code

---

## Phase 3: Refactor Recipe Tool to Keyword-Searchable Reference Lookup

### Task 3-1: Restructure recipe .txt files to TAGS format

**Objective:** Convert the 6 recipe files into compact, single-topic reference entries with TAGS headers. Optionally split multi-topic recipes into separate files.

**Directory:** `Content/SystemPrompts/Knowledge/recipes/blueprint/`

**Current files to refactor:**
- `create.txt` (24 lines) -> keep, add TAGS header, trim duplicated gotchas
- `modify.txt` (25 lines) -> keep, add TAGS header, trim duplicated gotchas
- `fix_wiring.txt` (30 lines) -> keep, add TAGS header, trim duplicated gotchas
- `variables_components.txt` (30 lines) -> keep, add TAGS header, trim
- `edit_existing_graph.txt` (33 lines) -> keep, add TAGS header, trim
- `multi_asset.txt` (50 lines) -> keep, add TAGS header, trim

**New format for each file:**
```
TAGS: keyword1 keyword2 keyword3
---
Content here (50-100 words max, one focused topic)
```

**Example rewrite for `create.txt`:**
```
TAGS: create new blueprint plan_json actor component variable spawn
---
CREATE new Blueprint:
1. blueprint.create -> {path:"/Game/Blueprints/BP_Name", parent_class:"Actor"}
2. add_component + add_variable (batch in one turn)
3. blueprint.apply_plan_json (ALL graph logic in one call)

Do NOT call blueprint.read before create -- the asset doesn't exist yet.
Data-provider steps (get_var, pure calls) MUST appear BEFORE steps that @ref them.
```

**New files to add (from topics currently embedded in other recipes):**
- `spawn_actor.txt` -- focused on SpawnActor + SpawnTransform pattern
- `function_graph.txt` -- focused on function graph entry, auto-chain, graph_target
- `object_variable_type.txt` -- focused on variable type format for objects, classes, arrays

These new entries address the plan's examples in Phase 3.

**Constraints:**
- Tags must be lowercase, space-separated
- Content must be self-contained (no cross-references to other recipe files)
- Each file 50-100 words max (the existing files are 100-200 words -- trim aggressively)
- The `---` separator between TAGS and content is required for parsing

**Acceptance criteria:**
- Every .txt file under `recipes/blueprint/` has a `TAGS: ...` header line followed by `---`
- No file exceeds 120 words of content (after the --- separator)
- Tags cover the key terms an AI would search for when stuck on that topic

### Task 3-2: Update _manifest.json format

**Objective:** Update the manifest to reflect the new file structure. The format_version should increment to "2.0" to signal the TAGS-based format.

**File:** `Content/SystemPrompts/Knowledge/recipes/_manifest.json`

**Changes:**
- `"format_version": "1.0"` -> `"format_version": "2.0"`
- Add entries for new files (`spawn_actor`, `function_graph`, `object_variable_type`)
- Keep existing entries but note that tags are now in-file (the manifest tags become secondary/optional)

**Constraints:**
- The `LoadRecipeLibrary()` method checks `format_version.StartsWith("1.")` and rejects non-1.x versions. Task 3-3 must update this check BEFORE this manifest change is deployed. However, since both are in the same Phase 3 delivery, coordinate: update code first, then manifest.

### Task 3-3: Refactor LoadRecipeLibrary to parse TAGS format

**Objective:** Update `LoadRecipeLibrary()` to parse the new `TAGS: ... / --- / content` format and store tags per entry.

**File:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`

**Location:** Lines 1000-1121 (`LoadRecipeLibrary()`)

**Changes needed:**

1. **Accept format_version 2.x** (line 1038): Change `!FormatVersion.StartsWith(TEXT("1."))` to `!FormatVersion.StartsWith(TEXT("1.")) && !FormatVersion.StartsWith(TEXT("2."))`.

2. **Parse TAGS from file content**: After loading the file content (line 1085-1088), parse the TAGS header:
```cpp
// Parse TAGS header if present
TArray<FString> Tags;
FString ActualContent = Content;
const int32 SepIndex = Content.Find(TEXT("---\n"));
if (SepIndex != INDEX_NONE)
{
    const FString Header = Content.Left(SepIndex).TrimStartAndEnd();
    if (Header.StartsWith(TEXT("TAGS:"), ESearchCase::IgnoreCase))
    {
        const FString TagLine = Header.Mid(5).TrimStartAndEnd();
        TagLine.ParseIntoArray(Tags, TEXT(" "), true);
        // Lowercase all tags
        for (FString& Tag : Tags)
        {
            Tag = Tag.ToLower();
        }
    }
    ActualContent = Content.Mid(SepIndex + 4); // Skip "---\n"
}
RecipeLibrary.Add(Key, ActualContent);
RecipeTags.Add(Key, Tags);
```

3. **Add RecipeTags member** to the class header.

**File:** `Source/OliveAIEditor/CrossSystem/Public/MCP/OliveCrossSystemToolHandlers.h`

Add after line 77 (`TArray<FString> RecipeCategories;`):
```cpp
/** Tags per recipe entry: Key = "category/name", Value = tag array */
TMap<FString, TArray<FString>> RecipeTags;
```

**Constraints:**
- Must be backward-compatible: files without TAGS header still load correctly (empty Tags array, full Content)
- The `---` separator might use `\r\n` on Windows -- handle both `---\n` and `---\r\n`

### Task 3-4: Update schema from {category, name} to {query}

**Objective:** Change the tool schema to accept a free-text query string.

**File:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemSchemas.cpp`

**Location:** Lines 301-314 (`RecipeGetRecipe()`)

**Replace with:**
```cpp
TSharedPtr<FJsonObject> RecipeGetRecipe()
{
    TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
    TSharedPtr<FJsonObject> Props = MakeProperties();

    Props->SetObjectField(TEXT("query"),
        OliveBlueprintSchemas::StringProp(TEXT("Free-text search query (e.g. 'spawn actor transform', 'variable type object class', 'function graph entry')")));

    Schema->SetObjectField(TEXT("properties"), Props);
    AddRequired(Schema, { TEXT("query") });
    return Schema;
}
```

**Also update schema header declaration:**

**File:** `Source/OliveAIEditor/CrossSystem/Public/MCP/OliveCrossSystemSchemas.h`

**Line 72:** Change comment from `/** Schema for olive.get_recipe: {category?: string, name?: string} -- both optional */` to `/** Schema for olive.get_recipe: {query: string} -- free-text keyword search */`

### Task 3-5: Update tool registration description

**Objective:** Update the tool description to reflect keyword search behavior.

**File:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`

**Location:** Lines 1127-1143 (`RegisterRecipeTools()`)

**Replace the description (lines 1129-1134) with:**
```cpp
TEXT("Search for patterns, examples, and gotchas for Blueprint workflows. "
     "Query with keywords related to your task or error "
     "(e.g. 'spawn actor transform', 'variable type object', 'function graph entry'). "
     "Returns the most relevant reference entry."),
```

### Task 3-6: Rewrite HandleGetRecipe to use keyword matching

**Objective:** Replace the rigid category/name lookup with keyword matching against tags and content.

**File:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`

**Location:** Lines 1148-1259 (entire `HandleGetRecipe()` method)

**Replace with keyword search implementation:**

```
Pseudocode:
1. Extract "query" param (required, return error if missing)
2. Lowercase the query, split into terms
3. For each entry in RecipeLibrary:
   a. Count how many query terms appear in RecipeTags[key]
   b. If no tag matches, count how many query terms appear as substrings in the content (lower weight)
4. Sort entries by match score (tag matches weighted 2x, content matches weighted 1x)
5. Return top 1-3 entries (above a minimum threshold score of 1)
6. If no matches above threshold, return all entries in a summary list with their tag sets
```

**Scoring detail:**
- Each query term that matches a tag exactly: +2 points
- Each query term that appears as substring in content (case-insensitive): +1 point
- Minimum score to return as a match: 1 point
- Maximum results to return: 3

**Result format:**
```json
{
  "query": "spawn actor transform",
  "matches": [
    {"key": "blueprint/create", "score": 4, "tags": ["create","spawn",...], "content": "..."},
    {"key": "blueprint/spawn_actor", "score": 6, "tags": ["spawn","actor","transform",...], "content": "..."}
  ]
}
```

If no matches: return all available entries as a summary (key + tags only, no content) so the AI can refine its query.

**Constraints:**
- The old `{category, name}` schema is GONE -- there is no backward compatibility path for the old format. Any prompt references to the old calling convention must be updated (covered by Task 2-2).
- Performance: linear scan of all entries is fine (we have <20 entries). No need for indexing.
- Thread safety: RecipeLibrary and RecipeTags are populated at startup and read-only thereafter. No locking needed.

**Acceptance criteria:**
- `olive.get_recipe({"query":"spawn actor transform"})` returns the spawn_actor.txt entry
- `olive.get_recipe({"query":"variable type object"})` returns the variables_components.txt or object_variable_type.txt entry
- `olive.get_recipe({"query":"xyzzy nonexistent"})` returns a summary of all entries, not an error
- The old `{category, name}` params are rejected with a clear error if passed

---

## Risks and Edge Cases

### Phase 1a Risks

1. **Self-loop in exec_after vs exec_outputs**
   Risk: Only guarding exec_outputs but not exec_after leaves a gap.
   Mitigation: Guard both (exec_after self-loop is `Step.ExecAfter == Step.StepId`).

2. **Auto-chain creates a self-loop**
   Risk: Could the auto-chain logic ever wire an event to itself?
   Analysis: No -- auto-chain finds the first *orphan* impure step AFTER the event. An event node would skip itself because `Step.Op == OlivePlanOps::Event` is explicitly skipped at line 545-548.

### Phase 1b Risks

1. **Verification stage behavior on partial success**
   Risk: Stage 5 (Verify) in the pipeline runs compile + structural checks. With partial success now being `bSuccess=true`, the pipeline proceeds to Stage 5. If compilation fails because of broken wires (unconnected exec pins), the pipeline may report compile errors on top of the partial success.
   Mitigation: This is actually DESIRED behavior. The AI gets both the wiring errors AND the compile result, giving it maximum information for self-correction. The compile errors will point to the same broken connections that `wiring_errors` already lists.

2. **Self-correction no longer triggers for partial success**
   Risk: Without the `PLAN_PARTIAL_SUCCESS` self-correction case, the AI only has the `wiring_errors` in the result to guide it. Is that enough?
   Analysis: Yes. The result contains `wiring_errors` (with specific pin names and suggestions), `pin_manifests` (with all actual pins), `step_to_node_map` (with real node IDs), and `self_correction_hint`. This is MORE information than the old self-correction guidance provided.

3. **Undo behavior changes**
   Risk: Before Fix 2, a partial success rolled back, so the user could not undo to see the partial nodes. After Fix 2, partial nodes persist and are in the undo stack.
   Analysis: This is correct and desirable. The user can undo the partial result if they want.

### Phase 2 Risks

1. **AI quality regression**
   Risk: Removing prompt rules might cause the AI to make mistakes more often.
   Mitigation: Every removed rule is backed by a code guardrail that either auto-fixes the mistake or provides clear error feedback for self-correction. Test with the standard gun+bullet creation task.

2. **Different provider behavior**
   Risk: Claude Code vs Anthropic API vs OpenRouter might respond differently to the slimmed prompts.
   Mitigation: The removed rules are specifically about edge cases that code now handles. Core workflow guidance is preserved.

### Phase 3 Risks

1. **Keyword matching quality**
   Risk: Simple term matching might return irrelevant results for ambiguous queries.
   Mitigation: Tag-based matching gives strong signal (tags are curated). Content substring is a fallback. Returning top 3 with scores lets the AI judge relevance.

2. **Backward compatibility of schema change**
   Risk: Any cached schema or prompt that tells the AI to call `olive.get_recipe(category, name)` will break.
   Mitigation: The tool description clearly states the new `{query}` format. The self-correction for unknown params will guide the AI. Update recipe_routing.txt (Task 2-2) to reference the new format.

3. **Manifest format_version gate**
   Risk: If Task 3-2 (manifest update) is deployed before Task 3-3 (code update), the recipe system rejects the manifest.
   Mitigation: Tasks must be deployed together in the same build. Task 3-3 code change goes first, then manifest update.

---

## Validation Strategy

### Phase 1a Validation

1. **Unit test (manual):** Create a plan JSON with a step that has `"exec_outputs":{"Then":"spawn"}` where `step_id` is `"spawn"`. Submit via `blueprint.apply_plan_json`. Verify:
   - Warning in result: `"Step 'spawn': exec_output 'Then' targets itself (self-loop skipped)"`
   - No wiring errors for the self-loop
   - `bPartial` is false (no failures)
   - All other wires succeed

2. **Edge case:** Plan with BOTH self-loop exec_output AND a normal exec_output on the same step (e.g., Branch with `"True":"branch_step"` self-loop and `"False":"other_step"` valid). Verify self-loop is skipped and valid wire succeeds.

### Phase 1b Validation

1. **Integration test (manual):** Submit a plan with one deliberately wrong data wire. Verify:
   - All nodes appear in the Blueprint graph (visually confirm in editor)
   - `step_to_node_map` contains valid node IDs
   - `wiring_errors` shows the specific failure
   - `status` is `"partial_success"`, `success` is `true`
   - Call `blueprint.connect_pins` with the returned node IDs -- verify it works

2. **Regression test:** Submit a plan with NO wiring errors. Verify the success path (lines 6579-6597) is unchanged and behaves identically.

3. **Regression test:** Submit a plan where Phase 1 fails (bad node type). Verify the error path (lines 6531-6542) is unchanged.

### Phase 2 Validation

1. **End-to-end:** Use Claude Code to create a gun+bullet multi-asset system. Verify the AI:
   - Does NOT need the removed rules to succeed
   - Follows the workflow correctly with the remaining prompt content
   - Recovers gracefully from any mistakes via code guardrails

2. **SpawnActor test:** Ask for a plan that uses Location/Rotation inputs on spawn_actor. Verify ExpandPlanInputs auto-synthesizes MakeTransform.

3. **Auto-chain test:** Ask for a function graph plan that omits exec_after on the first step. Verify auto-chain handles it.

### Phase 3 Validation

1. **Keyword search:** Call `olive.get_recipe({"query":"spawn actor transform"})`. Verify spawn_actor.txt is top result.

2. **Multi-word query:** Call with `{"query":"fix wiring error pins"}`. Verify fix_wiring.txt scores highest.

3. **No match:** Call with `{"query":"quantum entanglement"}`. Verify graceful fallback (summary of all entries).

4. **End-to-end:** In an AI session, have the AI hit an error. Verify it calls `olive.get_recipe` with relevant keywords and gets useful guidance.

---

## Implementation Order for Coder

1. **Task 1a-1** -- Self-loop guard (OlivePlanExecutor.cpp, ~15 lines)
2. **Task 1b-1** -- Partial success commits (OliveBlueprintToolHandlers.cpp, ~15 lines changed)
3. **Task 1b-2** -- Remove PLAN_PARTIAL_SUCCESS self-correction (OliveSelfCorrectionPolicy.cpp, ~8 lines deleted)
4. **Build + verify** compilation succeeds
5. **Task 2-1** -- Slim cli_blueprint.txt (text edit)
6. **Task 2-2** -- Slim recipe_routing.txt (text edit)
7. **Task 3-3** -- Update LoadRecipeLibrary for TAGS parsing + format_version gate (OliveCrossSystemToolHandlers.cpp + .h)
8. **Task 3-4** -- Update schema (OliveCrossSystemSchemas.cpp + .h)
9. **Task 3-5** -- Update tool registration description (OliveCrossSystemToolHandlers.cpp)
10. **Task 3-6** -- Rewrite HandleGetRecipe for keyword search (OliveCrossSystemToolHandlers.cpp)
11. **Build + verify** compilation succeeds
12. **Task 3-1** -- Restructure recipe .txt files (text edits)
13. **Task 3-2** -- Update _manifest.json (text edit)

Note: Tasks 3-3 through 3-6 are in the same cpp file and should be done as one unit. Tasks 3-1 and 3-2 (content files) go last because the code must support the new format before the content changes.
