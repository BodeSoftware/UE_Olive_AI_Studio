# Tool Consolidation Contract (P5)

**Date:** 2026-04-19
**Status:** Driving document for P5 implementation.
**Scope:** Full mapping of legacy tool names → consolidated replacements with pre-filled params. This is the source of truth the implementers read while doing P5.

**Related:**
- Spec: `docs/superpowers/specs/2026-04-18-plugin-makeover-design.md` §3
- Plan: `docs/superpowers/plans/2026-04-18-pack-P5-tool-consolidation.md`
- Reference plugin inspiration: `reference/unreal-engine-mcp-main/` (borrowed ergonomics noted per family)

---

## 1. Alias mechanics

`FOliveToolRegistry` already has `FOliveToolAlias` defined in the header — a struct carrying a `NewToolName` and an optional `TransformParams` lambda. The registry's `ExecuteTool(Name, Params)` entry point must first check an `AliasMap`; if `Name` is aliased, it merges pre-filled params with caller-provided params (caller wins on conflicts) and recursively dispatches to `NewToolName`.

**API to confirm/add in `OliveToolRegistry.h`/`.cpp`:**

```cpp
void RegisterAlias(const FString& OldName, const FString& NewName,
                   TFunction<void(TSharedPtr<FJsonObject>&)> TransformParams = nullptr);
```

Aliases are silent: they do not appear in `tools/list`. They emit a one-line debug log on first use per session so operators can spot deprecation.

**Merging rule:** `TransformParams` operates on a cloned params object. Keys the caller supplied are preserved; only missing keys are filled in by the transform. The alias is a shim, not a rewriter that overrules the caller.

---

## 2. Blueprint (47 → 15)

### 2.1 Consolidated tools

| Tool | Purpose | Notes |
|---|---|---|
| `blueprint.create` | New Blueprint class | — |
| `blueprint.read` | Read Blueprint state | `include` flag array (see §2.2) |
| `blueprint.delete` | Remove entity | `entity` enum (see §2.3) |
| `blueprint.modify` | Change entity | `entity` + `action` (see §2.4) |
| `blueprint.add` | Add entity | `entity` enum (see §2.5) |
| `blueprint.connect_pins` | Wire pins | unchanged |
| `blueprint.disconnect_pins` | Unwire pins | unchanged |
| `blueprint.compile` | Compile | swallows `verify_completion` via `verify: bool = false` optional |
| `blueprint.apply_plan_json` | Apply plan | unchanged |
| `blueprint.preview_plan_json` | Preview plan | unchanged |
| `blueprint.describe_node_type` | Node-type doc | unchanged |
| `blueprint.list_overridable_functions` | Overridable funcs | unchanged |
| `blueprint.list_templates` | Template search | unchanged (P2 controls discoverability) |
| `blueprint.get_template` | Template read | unchanged |
| `blueprint.create_from_template` | Template apply | unchanged |

### 2.2 `blueprint.read` — reference-inspired include flags

Borrowed shape from Flopperam's `read_blueprint_content`:

**New params:**
- `path: string` (required)
- `include: array<string>` — any subset of `["components","variables","functions","hierarchy","event_graph","function_detail","pins","interfaces"]`. Default `["components","variables","functions"]`.
- `function_name: string` — required when `include` contains `"function_detail"` or `"pins"`.
- `node_id: string` — required when `include` contains `"pins"`.
- `trace_execution_flow: bool = false` — when true with `include=["event_graph"]`, returns execution-order traversal (reference-inspired).

**Alias table:**

| Old | include pre-fill | Extra param mapping |
|---|---|---|
| `blueprint.read` | — | pass-through |
| `blueprint.read_components` | `["components"]` | — |
| `blueprint.read_variables` | `["variables"]` | — |
| `blueprint.read_event_graph` | `["event_graph"]` | — |
| `blueprint.read_function` | `["function_detail"]` | old `function_name` → new `function_name` |
| `blueprint.read_hierarchy` | `["hierarchy"]` | — |
| `blueprint.describe_function` | `["function_detail"]` | — |
| `blueprint.get_node_pins` | `["pins"]` | old `node_id` → `node_id` |
| `blueprint.read_components` | `["components"]` | — |

### 2.3 `blueprint.delete` — entity dispatch

**Params:** `path`, `entity` (enum: `blueprint|node|component|variable|function|interface`), plus entity-specific fields:
- `node`: `node_id`, optional `function_name`
- `component`: `component_name`
- `variable`: `variable_name`
- `function`: `function_name`
- `interface`: `interface_path`

**Alias table:**

| Old | entity pre-fill | Field renames |
|---|---|---|
| `blueprint.delete` | `"blueprint"` | — |
| `blueprint.remove_node` | `"node"` | — |
| `blueprint.remove_component` | `"component"` | — |
| `blueprint.remove_function` | `"function"` | — |
| `blueprint.remove_interface` | `"interface"` | — |
| `blueprint.remove_variable` | `"variable"` | — |

### 2.4 `blueprint.modify` — entity + action + rich kwargs

**Params:** `path`, `entity`, `action`, then entity-specific payload.

**Entities:**
- `component`: `action` in `{rename, reparent, set_properties}`
- `function`: `action` in `{rename, set_signature, override_virtual}`
- `variable`: `action` in `{rename, set_type, set_default, set_properties}`
- `node`: `action` in `{move, set_property}`
- `pin_default`: sets a pin's default value (`node_id`, `pin_name`, `value`)
- `blueprint`: `action` in `{set_parent_class, set_defaults}`

**Reference-inspired enhancement — `entity=variable, action=set_properties`:**

Flopperam's `set_blueprint_variable_properties` carries 21 optional kwargs. Our consolidated `blueprint.modify` with `entity=variable, action=set_properties` must accept:

```json
{
  "path": "...",
  "entity": "variable",
  "action": "set_properties",
  "variable_name": "Health",
  "properties": {
    "tooltip": "...",
    "category": "Stats",
    "is_public": true,
    "is_read_only": false,
    "is_editable": true,
    "is_instance_editable": true,
    "expose_on_spawn": false,
    "is_private": false,
    "is_replicated": false,
    "replication_condition": "None",
    "is_save_game": false,
    "is_advanced_display": false,
    "is_multiline": false,
    "ui_min": 0.0,
    "ui_max": 100.0,
    "clamp_min": 0.0,
    "clamp_max": 100.0,
    "bitmask": false,
    "bitmask_enum": "",
    "units": "",
    "delta_value": 1.0
  }
}
```

The underlying implementation reuses the existing `HandleModifyBlueprintVariable` body; we just widen the accepted `properties` map.

**Alias table:**

| Old | entity + action pre-fill | Notes |
|---|---|---|
| `blueprint.modify_component` | `entity: "component", action: "set_properties"` | — |
| `blueprint.modify_function` | `entity: "function", action: "set_signature"` | — |
| `blueprint.modify_function_signature` | `entity: "function", action: "set_signature"` | — |
| `blueprint.modify_variable` | `entity: "variable", action: "set_properties"` | Old `properties` param merges into new `properties` map |
| `blueprint.set_node_property` | `entity: "node", action: "set_property"` | — |
| `blueprint.set_parent_class` | `entity: "blueprint", action: "set_parent_class"` | — |
| `blueprint.set_pin_default` | `entity: "pin_default", action: ""` (no action needed) | — |
| `blueprint.set_defaults` | `entity: "blueprint", action: "set_defaults"` | — |
| `blueprint.reparent_component` | `entity: "component", action: "reparent"` | — |
| `blueprint.move_node` | `entity: "node", action: "move"` | — |
| `blueprint.override_function` | `entity: "function", action: "override_virtual"` | — |

### 2.5 `blueprint.add` — entity dispatch, rich node-type set

**Params:** `path`, `entity`, then entity-specific payload.

**Entities:**
- `node`: `node_type` (enum), `location` (optional `[x,y]`), `graph_name` (optional), entity-specific fields.
- `variable`: `variable_name`, `variable_type`, optional `default_value`, optional `properties` map (same rich shape as §2.4)
- `function`: `function_name`, optional `inputs`, `outputs`, `is_pure`, `category`
- `component`: `component_class`, `component_name`, optional `parent_component`, `properties`
- `custom_event`: `event_name`, optional inputs
- `event_dispatcher`: `dispatcher_name`, optional `signature`
- `interface`: `interface_path`
- `timeline`: `timeline_name`, tracks array

**Reference-inspired — `entity=node`:**

Flopperam supports 23 node types on their single `add_node`. Ours must accept at least this breadth in `node_type`:

- Event nodes: `event`, `custom_event`, `event_begin_play`, `event_tick`, `event_destroyed`, `component_bound_event`
- Flow control: `branch`, `sequence`, `switch_on_int`, `switch_on_enum`, `switch_on_string`, `for_loop`, `for_each`, `while_loop`, `do_once`, `flip_flop`, `gate`, `delay`, `multi_gate`
- Variables: `variable_get`, `variable_set`, `self`
- Data ops: `make_array`, `make_struct`, `break_struct`, `make_map`
- Casts: `dynamic_cast`, `class_dynamic_cast`
- Calls: `call_function`, `call_interface_function`, `call_parent_function`, `call_delegate`
- Spawn/async: `spawn_actor`, `spawn_actor_from_class`, `async_spawn`
- IO: `print_string`, `print_text`
- Other: `comment`, `knot`, `timeline`, `literal`

The handler dispatches on `node_type` to `FOliveNodeFactory::Create*` methods. Unknown `node_type` returns an error with a suggestion listing valid values.

**Alias table:**

| Old | entity pre-fill | Extra |
|---|---|---|
| `blueprint.add_node` | `"node"` | pass `node_type` through |
| `blueprint.add_variable` | `"variable"` | — |
| `blueprint.add_function` | `"function"` | — |
| `blueprint.add_component` | `"component"` | — |
| `blueprint.add_custom_event` | `"custom_event"` | — |
| `blueprint.add_event_dispatcher` | `"event_dispatcher"` | — |
| `blueprint.add_interface` | `"interface"` | — |
| `blueprint.create_timeline` | `"timeline"` | — |

### 2.6 Deletions (no alias)

- `blueprint.scaffold` — rarely used, deprecated.
- `blueprint.verify_completion` — folded into `blueprint.compile` via `verify: bool = false`.
- `blueprint.create_interface` — folded into `blueprint.create` with `parent_class: "Interface"`.

---

## 3. BehaviorTree + Blackboard (19 → 7)

**Consolidated tools:**
- `behaviortree.create`, `behaviortree.read`, `behaviortree.add`, `behaviortree.modify`, `behaviortree.remove`, `behaviortree.move`
- `blackboard.modify` (subsumes all 6 blackboard tools)

**Reference inspiration:** the reference has no BT tools, but the `add` / `modify` dispatch pattern is borrowed from their node-type dispatch shape.

### 3.1 `behaviortree.add`

`node_type` enum: `composite|task|decorator|service|node`. `node` is a generic fallback.

**Alias table:**

| Old | node_type pre-fill |
|---|---|
| `behaviortree.add_composite` | `"composite"` |
| `behaviortree.add_task` | `"task"` |
| `behaviortree.add_decorator` | `"decorator"` |
| `behaviortree.add_service` | `"service"` |
| `behaviortree.add_node` | `"node"` (pass-through — node already has its own class field) |

### 3.2 `behaviortree.modify`

`entity` enum: `node|decorator|blackboard_ref`. Action implicit.

**Alias table:**

| Old | entity pre-fill |
|---|---|
| `behaviortree.modify_node` | `"node"` |
| `behaviortree.set_node_property` | `"node"` |
| `behaviortree.set_decorator` | `"decorator"` |
| `behaviortree.set_blackboard` | `"blackboard_ref"` |

### 3.3 `blackboard.modify`

`action` enum: `create|add_key|modify_key|remove_key|set_parent|read`.

**Alias table:**

| Old | action pre-fill |
|---|---|
| `blackboard.create` | `"create"` |
| `blackboard.add_key` | `"add_key"` |
| `blackboard.modify_key` | `"modify_key"` |
| `blackboard.remove_key` | `"remove_key"` |
| `blackboard.set_parent` | `"set_parent"` |
| `blackboard.read` | `"read"` |

---

## 4. PCG (12 → 7)

**Consolidated tools:** `pcg.create`, `pcg.read`, `pcg.add`, `pcg.modify`, `pcg.remove`, `pcg.connect`, `pcg.execute`

**Alias table:**

| Old | Pre-fill |
|---|---|
| `pcg.create_graph` | aliases to `pcg.create` pass-through |
| `pcg.add_node` | `pcg.add` with `node_kind: "node"` |
| `pcg.add_subgraph` | `pcg.add` with `node_kind: "subgraph"` |
| `pcg.modify_node` | `pcg.modify` with `entity: "node"` |
| `pcg.set_settings` | `pcg.modify` with `entity: "settings"` |
| `pcg.remove_node` | `pcg.remove` pass-through |
| `pcg.connect_pins` | `pcg.connect` pass-through |
| `pcg.disconnect` | `pcg.connect` with `break: true` |

---

## 5. Niagara (10 → 8)

**Consolidated tools:** `niagara.create_system`, `niagara.read`, `niagara.add`, `niagara.modify`, `niagara.remove`, `niagara.compile`, `niagara.describe_module`, `niagara.list_modules`.

**Alias table:**

| Old | Pre-fill |
|---|---|
| `niagara.read_system` | `niagara.read` pass-through |
| `niagara.add_emitter` | `niagara.add` with `kind: "emitter"` |
| `niagara.add_module` | `niagara.add` with `kind: "module"` |
| `niagara.set_emitter_property` | `niagara.modify` with `entity: "emitter"` |
| `niagara.set_parameter` | `niagara.modify` with `entity: "parameter"` |
| `niagara.remove_module` | `niagara.remove` pass-through |

---

## 6. C++ (11 → 6)

**Consolidated tools:** `cpp.read`, `cpp.list`, `cpp.create_class`, `cpp.add`, `cpp.modify_source`, `cpp.compile`.

**Alias table:**

| Old | Pre-fill |
|---|---|
| `cpp.read_class` | `cpp.read` with `entity: "class"` |
| `cpp.read_enum` | `cpp.read` with `entity: "enum"` |
| `cpp.read_struct` | `cpp.read` with `entity: "struct"` |
| `cpp.read_header` | `cpp.read` with `entity: "header"` |
| `cpp.read_source` | `cpp.read` with `entity: "source"` |
| `cpp.list_project_classes` | `cpp.list` with `kind: "project"` |
| `cpp.list_blueprint_callable` | `cpp.list` with `kind: "blueprint_callable"` |
| `cpp.list_overridable` | `cpp.list` with `kind: "overridable"` |
| `cpp.add_function` | `cpp.add` with `entity: "function"` |
| `cpp.add_property` | `cpp.add` with `entity: "property"` |

---

## 7. Project (19 → 7)

**Consolidated tools:** `project.search`, `project.read`, `project.snapshot`, `project.rollback`, `project.diff`, `project.refactor_rename`, `project.index`.

### 7.1 `project.read` — include flags

**Params:** `path` (optional — omit for project-wide info), `include: array<string>` — any subset of `["asset_info","class_hierarchy","config","dependencies","info","referencers","relevant_context","bulk"]`.

**Alias table:**

| Old | Pre-fill |
|---|---|
| `project.get_asset_info` | `project.read` with `include: ["asset_info"]` |
| `project.get_class_hierarchy` | `project.read` with `include: ["class_hierarchy"]` |
| `project.get_config` | `project.read` with `include: ["config"]` |
| `project.get_dependencies` | `project.read` with `include: ["dependencies"]` |
| `project.get_info` | `project.read` with `include: ["info"]` |
| `project.get_referencers` | `project.read` with `include: ["referencers"]` |
| `project.get_relevant_context` | `project.read` with `include: ["relevant_context"]` |
| `project.bulk_read` | `project.read` with `include: ["bulk"]` |

### 7.2 `project.snapshot`

Subsumes `project.list_snapshots` via `action: "list"` param. Default action `"create"`.

### 7.3 `project.index`

Subsumes `project.index_build` (action: `"build"`) and `project.index_status` (action: `"status"`). Default `"status"`.

### 7.4 Deletions (no alias)

- `project.create_ai_character` — too specialized.
- `project.implement_interface` — duplicate of `blueprint.add entity=interface`.
- `project.move_to_cpp` — low usage.
- `project.batch_write` — callers can loop.
- `project.list_snapshots` — folded into `project.snapshot`.

---

## 8. Olive + test cleanup

- `olive.get_recipe` — delete. Rails we're removing.
- `olive.build` — keep.
- `olive.search_community_blueprints` — keep (optional).
- `test.create`, `test.tool` — delete. Dev-only.

---

## 9. Final surface

| Family | After |
|---|---:|
| Blueprint | 15 |
| BT + Blackboard | 7 |
| PCG | 7 |
| Niagara | 8 |
| C++ | 6 |
| Widget | 4 |
| AnimBP | 4 |
| Project | 7 |
| Editor | 1 |
| Olive | 2 |
| Level (P4 merged) | 8 |
| Material (P6 merged) | 5 |
| **Total** | **74** |

Target from spec was ~74; actual is ~74. On mark.

---

## 10. Out of scope for P5

- Changing underlying handler behavior beyond the consolidation dispatch.
- Adding new capabilities beyond reference-inspired ergonomics (variable-properties richness, include flags, node-type breadth).
- Worldbuild/factory template changes (that was P7, deferred).
- Widget and AnimBP family consolidation.

## 11. Implementation order

1. Confirm / add `RegisterAlias` to registry (P5.T2).
2. Write consolidated handlers + schemas for each family, starting with Blueprint (biggest).
3. Flip old tool registrations to alias registrations, one family at a time.
4. Delete dead handler bodies after aliases verified.
5. Delete tools with no alias (final cleanup).
6. Verify final tool count.
