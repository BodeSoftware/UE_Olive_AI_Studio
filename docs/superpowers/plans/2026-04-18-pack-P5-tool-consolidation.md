# Pack P5 — Tool Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce the MCP tool surface from ~132 to ~61 (plus `level.*` 8 and `material.*` 5 from other packs = ~74 total). Consolidate read/modify/remove/add tool families into action-parameterized tools. Keep old names as `NormalizeToolParams` aliases for one release so external MCP clients do not break.

**Architecture:** For each family (Blueprint/BT/PCG/Niagara/Cpp/Project), rewrite registrations so that many similar tools become one consolidated tool with `entity` / `action` / `include` parameters. The underlying handler logic is preserved — the consolidation is a dispatch layer. Old tool names are registered as aliases that dispatch to the new consolidated tool with pre-filled params.

**Tech Stack:** UE 5.5 C++, JSON schemas, `FOliveToolRegistry`, `NormalizeToolParams` aliasing.

**Related spec:** `docs/superpowers/specs/2026-04-18-plugin-makeover-design.md` §3.

**Blocker:** P2 (prompts) must run **after** P5 because rewritten prompts reference new tool names.

**Companion contract doc to produce first:** `docs/superpowers/specs/2026-04-18-tool-consolidation-contract.md` — a full table of `old_name → new_name + pre-filled params`. This is your source of truth.

---

## Consolidation map

### Blueprint (47 → 15)

| New tool | Entity/action | Swallows |
|---|---|---|
| `blueprint.create` | — | `blueprint.create`, `blueprint.scaffold`, `blueprint.create_interface` |
| `blueprint.read` | `include=[components,variables,functions,hierarchy,event_graph,function_detail,pins]` | `blueprint.read`, `read_components`, `read_event_graph`, `read_function`, `read_hierarchy`, `read_variables`, `describe_function`, `get_node_pins` |
| `blueprint.delete` | `entity=[blueprint,node,component,variable,function,interface]` | `blueprint.delete`, `remove_component`, `remove_function`, `remove_interface`, `remove_node`, `remove_variable` |
| `blueprint.modify` | `entity=[component,function,function_signature,variable,node,pin_default,defaults,parent_class]` + `action` | `modify_component`, `modify_function`, `modify_function_signature`, `modify_variable`, `set_node_property`, `set_parent_class`, `set_pin_default`, `set_defaults`, `reparent_component`, `move_node`, `override_function` |
| `blueprint.add` | `entity=[node,variable,function,component,custom_event,event_dispatcher,interface,timeline]` | `add_node`, `add_variable`, `add_function`, `add_component`, `add_custom_event`, `add_event_dispatcher`, `add_interface`, `create_timeline` |
| `blueprint.connect_pins` | (unchanged) | `connect_pins` |
| `blueprint.disconnect_pins` | (unchanged) | `disconnect_pins` |
| `blueprint.compile` | (unchanged; swallows `verify_completion`) | `compile`, `verify_completion` |
| `blueprint.apply_plan_json` | (unchanged) | `apply_plan_json` |
| `blueprint.preview_plan_json` | (unchanged) | `preview_plan_json` |
| `blueprint.describe_node_type` | (unchanged) | `describe_node_type` |
| `blueprint.list_overridable_functions` | (unchanged) | `list_overridable_functions` |
| `blueprint.list_templates` | (unchanged) | `list_templates` |
| `blueprint.get_template` | (unchanged) | `get_template` |
| `blueprint.create_from_template` | (unchanged) | `create_from_template` |

### BT + Blackboard (19 → 7)

| New tool | Entity | Swallows |
|---|---|---|
| `behaviortree.create` | — | `behaviortree.create` |
| `behaviortree.read` | — | `behaviortree.read` |
| `behaviortree.add` | `node_type=[composite,task,decorator,service]` | `add_composite`, `add_task`, `add_decorator`, `add_service`, `add_node` |
| `behaviortree.modify` | `entity=[node,decorator,blackboard]` | `modify_node`, `set_node_property`, `set_decorator`, `set_blackboard` |
| `behaviortree.remove` | — | `remove_node` |
| `behaviortree.move` | — | `move_node` |
| `blackboard.modify` | `action=[create,add_key,modify_key,remove_key,set_parent,read]` | `blackboard.*` (all 6) |

### PCG (12 → 7)

| New tool | Swallows |
|---|---|
| `pcg.create` | `pcg.create`, `pcg.create_graph` |
| `pcg.read` | `pcg.read` |
| `pcg.add` | `pcg.add_node`, `pcg.add_subgraph` |
| `pcg.modify` | `pcg.modify_node`, `pcg.set_settings` |
| `pcg.remove` | `pcg.remove_node` |
| `pcg.connect` | `pcg.connect`, `pcg.connect_pins`, `pcg.disconnect` (via `break=true`) |
| `pcg.execute` | `pcg.execute` |

### Niagara (10 → 8)

| New tool | Swallows |
|---|---|
| `niagara.create_system` | (unchanged) |
| `niagara.read` | `niagara.read_system` |
| `niagara.add` | `niagara.add_emitter`, `niagara.add_module` |
| `niagara.modify` | `niagara.set_emitter_property`, `niagara.set_parameter` |
| `niagara.remove` | `niagara.remove_module` |
| `niagara.compile` | (unchanged) |
| `niagara.describe_module` | (unchanged — helper) |
| `niagara.list_modules` | (unchanged — helper) |

### C++ (11 → 6)

| New tool | Swallows |
|---|---|
| `cpp.read` | `cpp.read_class`, `read_enum`, `read_struct`, `read_header`, `read_source` (via `entity`) |
| `cpp.list` | `cpp.list_project_classes`, `list_blueprint_callable`, `list_overridable` (via `kind`) |
| `cpp.create_class` | (unchanged) |
| `cpp.add` | `cpp.add_function`, `cpp.add_property` |
| `cpp.modify_source` | (unchanged) |
| `cpp.compile` | (unchanged) |

### Project (19 → 7)

| New tool | Swallows |
|---|---|
| `project.search` | (unchanged) |
| `project.read` | `get_asset_info`, `get_class_hierarchy`, `get_config`, `get_dependencies`, `get_info`, `get_referencers`, `get_relevant_context`, `bulk_read` |
| `project.snapshot` | `project.snapshot`, `project.list_snapshots` (via `action=list`) |
| `project.rollback` | (unchanged) |
| `project.diff` | (unchanged) |
| `project.refactor_rename` | (unchanged) |
| `project.index` | `project.index_build`, `project.index_status` |

### Deletions (no alias, just removed)

- `project.create_ai_character` (too specialized)
- `project.implement_interface` (dup of `blueprint.add entity=interface`)
- `project.move_to_cpp` (low usage)
- `project.batch_write` (callers can loop)
- `olive.get_recipe` (rails we're removing)
- `test.create`, `test.tool` (dev-only)

---

## File Structure

**Modify (primary):**
- `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp` — extend `NormalizeToolParams` alias table with every old → new mapping.
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` — rewrite `RegisterReaderTools`, `RegisterWriterTools`, `RegisterPlanTools`; add consolidated handlers.
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` — new consolidated schemas; keep old-name schemas registered as hidden aliases.
- `Source/OliveAIEditor/BehaviorTree/Private/MCP/OliveBTToolHandlers.cpp` — same pattern.
- `Source/OliveAIEditor/PCG/Private/MCP/OlivePCGToolHandlers.cpp` — same pattern.
- `Source/OliveAIEditor/Niagara/Private/MCP/OliveNiagaraToolHandlers.cpp` — same pattern.
- `Source/OliveAIEditor/Cpp/Private/MCP/OliveCppToolHandlers.cpp` — same pattern.
- `Source/OliveAIEditor/Private/MCP/OliveProjectToolHandlers.cpp` — same pattern.

**Create:**
- `docs/superpowers/specs/2026-04-18-tool-consolidation-contract.md` — full old → new mapping (produced in Task 1).

**Tests:**
- `Source/OliveAIEditor/Private/Tests/MCP/OliveToolConsolidationTests.cpp` — one test per old → new alias that proves dispatch still works.

---

## Tasks

### Task 1: Write the consolidation contract doc

**Files:**
- Create: `docs/superpowers/specs/2026-04-18-tool-consolidation-contract.md`

- [ ] **Step 1: Fill the table**

For every old tool name, produce one row:

```markdown
| Old name | New name | Pre-filled params | Handler param name changes |
|---|---|---|---|
| blueprint.read_components | blueprint.read | `{"include": ["components"]}` | — |
| blueprint.read_event_graph | blueprint.read | `{"include": ["event_graph"]}` | — |
| blueprint.remove_component | blueprint.delete | `{"entity": "component"}` | `component_name` → `name` |
| ... | ... | ... | ... |
```

Use the consolidation maps above as the source. Include every old name from `Grep` of `TEXT("[a-z_]+\.[a-z_]+")` in the source (excluding false positives like manifest filenames).

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-04-18-tool-consolidation-contract.md
git commit -m "P5: consolidation contract — old->new tool mapping"
```

---

### Task 2: Add alias-dispatch support to `FOliveToolRegistry`

**Files:**
- Modify: `Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h`
- Modify: `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp`

- [ ] **Step 1: Add RegisterAlias API**

In the registry header, add:

```cpp
/**
 * Register OldName as an alias of NewName. When OldName is invoked, the registry
 * merges PrefilledParams into the call's params (call params take precedence for
 * overlapping keys, except for pinned params), then dispatches to NewName.
 */
void RegisterAlias(const FString& OldName, const FString& NewName,
                   const TSharedPtr<FJsonObject>& PrefilledParams);
```

- [ ] **Step 2: Implement**

Internally store a map `TMap<FString, FOliveToolAlias>` where `FOliveToolAlias { FString NewName; TSharedPtr<FJsonObject> Prefilled; }`.

In `InvokeTool`: before looking up the handler, check if `ToolName` is in the alias map. If yes, merge prefilled into params (call-site wins on conflicts), then recursively call `InvokeTool(Alias.NewName, MergedParams)`.

In `GetToolNames()` expose both real and aliased names. Add a flag on the tool record to mark aliases as deprecated so clients can discover them but they don't clutter `tools/list` unless requested.

- [ ] **Step 3: Build + short unit test**

Write a trivial test that registers a fake tool and an alias, invokes the alias, and confirms the real handler ran.

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveToolRegistryAliasTest,
    "OliveAI.MCP.Registry.Alias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveToolRegistryAliasTest::RunTest(const FString& Parameters)
{
    FOliveToolRegistry& R = FOliveToolRegistry::Get();

    // Temporary tool
    R.RegisterTool(TEXT("_test.new"), TEXT("test"), MakeShared<FJsonObject>(),
        FOliveToolHandler::CreateLambda([](const TSharedPtr<FJsonObject>& P){
            FOliveToolResult Out = FOliveToolResult::Success();
            Out.Data = MakeShared<FJsonObject>();
            Out.Data->SetStringField(TEXT("entity"), P->GetStringField(TEXT("entity")));
            return Out;
        }),
        {TEXT("test")}, TEXT("test"));

    TSharedPtr<FJsonObject> Pref = MakeShared<FJsonObject>();
    Pref->SetStringField(TEXT("entity"), TEXT("component"));
    R.RegisterAlias(TEXT("_test.old"), TEXT("_test.new"), Pref);

    FOliveToolResult Res = R.InvokeTool(TEXT("_test.old"), MakeShared<FJsonObject>());
    TestTrue(TEXT("Alias dispatched to new tool"), Res.bSuccess);
    TestEqual(TEXT("Prefilled param propagated"), Res.Data->GetStringField(TEXT("entity")), FString(TEXT("component")));
    return true;
}
```

Build and run. Pass.

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp Source/OliveAIEditor/Private/Tests/MCP/OliveToolConsolidationTests.cpp
git commit -m "P5: add RegisterAlias API to tool registry"
```

---

### Task 3: Blueprint consolidation — implement new handlers

**Files:**
- Modify: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
- Modify: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`
- Modify: `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h`

- [ ] **Step 1: Write new consolidated handlers (do not delete old handlers yet)**

For each of `blueprint.read`, `blueprint.delete`, `blueprint.modify`, `blueprint.add`:
- Add a new handler function (e.g., `HandleBlueprintReadConsolidated`).
- Dispatch internally based on `include` / `entity` / `action` params.
- For `blueprint.read` with `include=["components"]`, the body delegates to the existing `HandleReadComponents` logic.
- For `blueprint.delete` with `entity="node"`, the body delegates to the existing `HandleRemoveNode` logic.

The dispatch is a switch statement over the param value. Reuse existing private helpers — do not duplicate their internals.

- [ ] **Step 2: Write new schemas**

In `OliveBlueprintSchemas.cpp`, add `BlueprintRead()`, `BlueprintDelete()`, `BlueprintModify()`, `BlueprintAdd()`. Each schema describes the full consolidated shape (with `include` / `entity` / `action` enums listing allowed values).

- [ ] **Step 3: Register new tools**

In `RegisterReaderTools()` add:

```cpp
Registry.RegisterTool(TEXT("blueprint.read"),
    TEXT("Read Blueprint state. Use 'include' to pick sections: components, variables, functions, hierarchy, event_graph, function_detail, pins."),
    OliveBlueprintSchemas::BlueprintRead(),
    FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintReadConsolidated),
    {TEXT("blueprint"), TEXT("read")}, TEXT("blueprint"));
```

Similar for `blueprint.delete`, `blueprint.modify`, `blueprint.add`.

- [ ] **Step 4: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Clean build. Both old and new tools are now registered.

- [ ] **Step 5: Quick manual verification**

Start editor. From the MCP bridge or a direct `InvokeTool` call, call `blueprint.read` with `include=["components"]` on a known Blueprint. Confirm it returns the same data as `blueprint.read_components`.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "P5: add consolidated blueprint.read/delete/modify/add handlers (old tools retained)"
```

---

### Task 4: Blueprint — register aliases and remove old registrations

**Files:**
- Modify: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

- [ ] **Step 1: Replace old `RegisterTool` calls with `RegisterAlias` calls**

For every tool listed in the Blueprint consolidation map that is being swallowed:
- Delete its `Registry.RegisterTool(...)` block.
- Replace with a `Registry.RegisterAlias(OldName, NewName, Prefilled)` call. Build `Prefilled` from the contract doc.

Example:

```cpp
TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
TArray<TSharedPtr<FJsonValue>> Include;
Include.Add(MakeShared<FJsonValueString>(TEXT("components")));
P->SetArrayField(TEXT("include"), Include);
Registry.RegisterAlias(TEXT("blueprint.read_components"), TEXT("blueprint.read"), P);
```

- [ ] **Step 2: Delete the corresponding old handler bodies** (private functions that are no longer referenced).

Use grep to confirm they are no longer called. Delete them.

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Clean build.

- [ ] **Step 4: Write alias round-trip tests**

For each old Blueprint tool name, one test:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveBPReadComponentsAliasTest,
    "OliveAI.MCP.Alias.Blueprint.ReadComponents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveBPReadComponentsAliasTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("path"), TEXT("/Game/Tests/BP_ForAliasTest.BP_ForAliasTest"));
    FOliveToolResult R = FOliveToolRegistry::Get().InvokeTool(TEXT("blueprint.read_components"), P);
    TestTrue(TEXT("Alias dispatches successfully"), R.bSuccess);
    TestTrue(TEXT("Returned data mentions components"),
        R.Data.IsValid() && R.Data->HasField(TEXT("components")));
    return true;
}
```

Add one test per alias. This is tedious but mechanical — use grep of the alias list as the worklist.

- [ ] **Step 5: Run all alias tests**

Session Frontend > Automation > `OliveAI.MCP.Alias.Blueprint.*`. All pass.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "P5: Blueprint — remove old tool registrations, add aliases, dead-code cleanup"
```

---

### Task 5: BT + Blackboard consolidation

**Files:**
- Modify: `Source/OliveAIEditor/BehaviorTree/Private/MCP/OliveBTToolHandlers.cpp`
- Modify: `Source/OliveAIEditor/BehaviorTree/Private/MCP/OliveBTSchemas.cpp` (if exists; else add entries in handlers)

Follow the same 3-step recipe as Blueprint: (1) add consolidated handler + schema, (2) register alias for each old name, (3) write alias round-trip tests.

- [ ] **Step 1: Implement consolidated handlers**

`behaviortree.add` switches on `node_type`. `behaviortree.modify` switches on `entity`. `blackboard.modify` switches on `action`.

- [ ] **Step 2: Register aliases + delete old registrations**

- [ ] **Step 3: Alias tests**

One test per old name.

- [ ] **Step 4: Build + run tests**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Session Frontend > `OliveAI.MCP.Alias.BT.*` and `.Blackboard.*`. All pass.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "P5: BT + Blackboard consolidation with aliases"
```

---

### Task 6: PCG consolidation

Same recipe.

- [ ] **Step 1-3:** Implement, register aliases, write tests.
- [ ] **Step 4:** Build + test.
- [ ] **Step 5:** Commit `P5: PCG consolidation with aliases`.

---

### Task 7: Niagara consolidation

Same recipe.

- [ ] **Step 1-3:** Implement, register aliases, write tests.
- [ ] **Step 4:** Build + test.
- [ ] **Step 5:** Commit `P5: Niagara consolidation with aliases`.

---

### Task 8: C++ consolidation

Same recipe.

- [ ] **Step 1-3:** Implement, register aliases, write tests.
- [ ] **Step 4:** Build + test.
- [ ] **Step 5:** Commit `P5: Cpp consolidation with aliases`.

---

### Task 9: Project consolidation + deletions

Same recipe, plus:

- [ ] **Step 1:** Fully remove the handlers AND registrations for the 4 deleted tools (`create_ai_character`, `implement_interface`, `move_to_cpp`, `batch_write`) — these have no alias.
- [ ] **Step 2:** Consolidate reads into `project.read` with an `entity` dispatch. Consolidate `project.index_build`/`project.index_status` into `project.index` with `action`.
- [ ] **Step 3:** For alias tests, include negative tests that calling a deleted tool returns `TOOL_NOT_FOUND`.
- [ ] **Step 4:** Build + test.
- [ ] **Step 5:** Commit `P5: Project consolidation + tool deletions`.

---

### Task 10: Cleanup — `olive.get_recipe`, `test.*`

**Files:**
- Modify: wherever `olive.get_recipe`, `test.create`, `test.tool` are registered.

- [ ] **Step 1: Remove registrations**

Delete the `Registry.RegisterTool(...)` calls for these three tools. Delete any now-unused handler functions.

- [ ] **Step 2: Build + test**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Full `OliveAI.*` suite green.

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "P5: remove olive.get_recipe and test.* tools"
```

---

### Task 11: Final count verification

**Files:** none changed.

- [ ] **Step 1: Count registered tools**

From the editor's Output Log after startup, look for `Registered N tools` lines. Sum across families. Expected: ≤ 80 (including level/material if those packs have landed).

Alternative: add a temporary log line in `OliveAIEditorModule::OnPostEngineInit()`:

```cpp
const TArray<FString> Names = FOliveToolRegistry::Get().GetToolNames(/*IncludeAliases*/ false);
UE_LOG(LogOliveAI, Display, TEXT("Registered %d real tools (aliases excluded)"), Names.Num());
```

Start editor; read log; confirm count.

- [ ] **Step 2: CLAUDE.md update**

In the "Key File Locations" and "Tool registry" sections, note the new consolidated surface. In a new "Tool aliasing" short section explain that old names dispatch to new ones for one release.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "P5: update CLAUDE.md for consolidated tool surface"
```

---

### Task 12: Update the P2 blocker note

**Files:** none changed.

- [ ] **Step 1: Inform the P2 pack author**

In `docs/superpowers/plans/2026-04-18-pack-P2-prompt-rewrite.md`, confirm the unblock. P2 can now proceed.

---

## Acceptance criteria

1. `ubt-build-5.5` green.
2. `OliveAI.*` suite green, including every `OliveAI.MCP.Alias.*` test.
3. `FOliveToolRegistry::GetToolNames(/*IncludeAliases*/ false)` reports ≤ 80 tools.
4. Every old tool name from the contract still dispatches successfully via alias.
5. Deleted tools (`create_ai_character`, `implement_interface`, `move_to_cpp`, `batch_write`, `olive.get_recipe`, `test.*`) return `TOOL_NOT_FOUND`.
6. `CLAUDE.md` and the contract doc updated.

## Out of scope

- Mode removal (P1).
- Prompt rewriting (P2 runs after this).
- Brain simplification (P3).
- `level.*` and `material.*` new modules (P4/P6).
- Renaming or restructuring any tools within Widget or AnimBP families — those remain unchanged.
