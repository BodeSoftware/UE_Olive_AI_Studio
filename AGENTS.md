# Olive AI Studio -- Unreal Engine 5.5

You have MCP tools for creating and modifying Unreal Engine 5.5 assets. Tool schemas describe parameters; this file covers workflow patterns and rules that schemas cannot express.

## Tool Categories
- `blueprint.*` -- Create, read, and modify Blueprint assets (graph logic via Plan JSON)
- `bt.*` / `blackboard.*` -- Behavior Trees and Blackboards
- `pcg.*` -- PCG graphs
- `cpp.*` -- C++ classes and source files
- `project.*` -- Asset search, bulk operations, cross-system tools

## Blueprint Workflow

**Create new Blueprint:**
1. Check templates first: `blueprint.list_templates` -- if one fits, use `blueprint.create_from_template`
2. Templates create STRUCTURE only (components, variables, empty function stubs, event dispatchers). After template creation, write plan_json for EACH function listed in the result. Call `olive.get_recipe` first for each function's wiring pattern.
3. Do NOT call `blueprint.read` or `blueprint.read_function` after template creation -- the functions are empty stubs waiting for your logic.
4. Otherwise: `blueprint.create` -> `blueprint.add_component` / `blueprint.add_variable` -> `blueprint.apply_plan_json`

**Modify existing Blueprint:**
1. `project.search` (find exact path) -> `blueprint.read` (understand current state) -> write tools

**Small edit (1-2 nodes):** `blueprint.read_event_graph` -> `blueprint.add_node` + `blueprint.connect_pins`

**Before your first plan_json for each function:** Call `olive.get_recipe` with the pattern you need (e.g., "fire weapon", "spawn projectile", "health component"). Recipes contain tested wiring patterns. This applies AFTER template creation too -- templates create empty function stubs, you write the logic.

**Multi-asset (2+ Blueprints):** Complete ONE asset at a time. For each asset: create structure (components, variables, functions), wire all plan_json for its functions, then compile. Move to the next asset only after the current one compiles clean.

## Plan JSON Format

Plan JSON is the primary way to build graph logic. Always preview before applying: `blueprint.preview_plan_json` then `blueprint.apply_plan_json` (never in the same response).

```json
{"schema_version":"2.0","steps":[
  {"step_id":"evt","op":"event","target":"BeginPlay"},
  {"step_id":"get_hp","op":"get_var","target":"Health"},
  {"step_id":"check","op":"branch","inputs":{"Condition":"@get_hp.auto"},"exec_after":"evt",
   "exec_outputs":{"True":"heal","False":"die"}},
  {"step_id":"heal","op":"call","target":"PrintString","inputs":{"InString":"Alive"}},
  {"step_id":"die","op":"call","target":"PrintString","inputs":{"InString":"Dead"}}
]}
```

### Step Fields
- `step_id` -- unique identifier (e.g., "s1", "evt", "spawn")
- `op` -- operation from closed vocabulary (see below)
- `target` -- function name, variable name, event name, or class name depending on op
- `inputs` -- pin values: `"PinName":"literal"` for defaults, `"PinName":"@step_id.auto"` for data wires
- `exec_after` -- step_id of the preceding impure node in execution flow
- `exec_outputs` -- for Branch/Cast: `{"True":"step_a","False":"step_b"}`

### Operations
`event`, `custom_event`, `call`, `call_delegate`, `get_var`, `set_var`, `branch`, `sequence`, `cast`, `for_loop`, `for_each_loop`, `while_loop`, `do_once`, `flip_flop`, `gate`, `delay`, `is_valid`, `print_string`, `spawn_actor`, `make_struct`, `break_struct`, `return`, `comment`

- `call_delegate` broadcasts an event dispatcher. Use for dispatchers created via `blueprint.add_event_dispatcher`.

### Execution Wiring Rules
- **Impure nodes** (call, call_delegate, set_var, branch, sequence, for_loop, delay, spawn_actor, print_string, cast, do_once, flip_flop, gate, while_loop) MUST have `exec_after` chaining them to a preceding impure step or event.
- **Pure nodes** (get_var, make_struct, break_struct, is_valid) do NOT use `exec_after`.
- `exec_after` and `exec_outputs` are **mutually exclusive on the source step**. If a step uses `exec_outputs` (Branch, Cast), downstream steps chain from those targets, not from the branch step itself.
- Data-provider steps (get_var, make_struct) should appear BEFORE steps that reference them via `@step_id`.

### Data Wiring Syntax
- `@step_id.auto` -- auto-match by type (use ~80% of the time)
- `@step_id.~hint` -- fuzzy prefix match on pin name
- `@step_id.PinName` -- exact pin name
- `@ComponentName` -- auto-expanded to a get_var for that component
- No `@` prefix = literal default value for the pin

### Function Resolution
Use natural names: `Destroy` resolves to `K2_DestroyActor`, `Print` to `PrintString`, `GetWorldTransform` to `K2_GetComponentToWorld`. K2_ prefixes and common aliases are resolved automatically.

## Asset Paths
Always use `/Game/...` format ending with the asset name: `/Game/Blueprints/BP_Gun` (not `/Game/Blueprints/`). For existing assets, use `project.search` to find exact paths. For new assets, choose the path directly.

## Important Rules
- **Read before modifying** any existing asset -- never guess at current state.
- **Preview before apply** -- always call `blueprint.preview_plan_json` before `blueprint.apply_plan_json`. Never batch both in the same response.
- **Don't re-preview unchanged plans**: If preview succeeds, apply in the next turn. Revising and re-previewing is fine. If apply fails partially, fix with `connect_pins`/`set_pin_default` rather than re-planning from scratch.
- **If apply_plan_json fails:** re-read the graph, fix the plan based on the error, and retry once. If it fails a second time, fall back to step-by-step `add_node`/`connect_pins`.
- **Fix compile errors** before declaring done. If `apply_plan_json` returns errors, use `wiring_errors` and `pin_manifests` from the result to correct.
- **Keep plans under 12 steps.** Split complex logic into multiple functions.
- **Prefer plan_json for 3+ nodes.** Only fall back to add_node/connect_pins after plan_json has failed twice.
- **Complete the full task** -- create structures, wire graphs, compile, verify. Do not stop partway.
- **Always call `olive.get_recipe` before your first `apply_plan_json`** for each function. Recipes contain tested wiring patterns. This includes functions created by templates (they are empty stubs).
- **Done condition:** Once ALL Blueprints compile with 0 errors and 0 warnings, the task is complete. Stop immediately and report what you built (asset paths, key features, any notes). Do not add cosmetic changes, extra previews, or verification reads after a clean compile.
- Component overlap/hit events: use `op:"event"` with `target:"OnComponentBeginOverlap"` (auto-detects component). Add `"component_name":"MyComp"` in `properties` if ambiguous.
