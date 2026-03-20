# CLAUDE.md

Olive AI Studio is an MCP server running inside Unreal Engine 5.5+ Editor. It exposes 40+ tools for creating and editing Blueprints, Behavior Trees, PCG graphs, and C++ classes. You connect to it via MCP and work with a live UE project -- every tool call mutates real assets in the editor.

## Connection

The `.mcp.json` in this directory configures the bridge automatically:
```json
{ "mcpServers": { "olive-ai-studio": { "command": "node", "args": ["mcp-bridge.js"] } } }
```
Or add manually: `claude mcp add olive-ai-studio -- node "<path-to-plugin>/mcp-bridge.js"`

The bridge auto-discovers the MCP server on ports 3000-3009. Unreal Editor must be running.

## Tool Categories

- **blueprint.*** -- Read, create, modify Blueprints. Includes plan_json for batch graph editing, templates for pattern reference, and granular node/pin tools.
- **behaviortree.*** -- Create and wire Behavior Trees and Blackboard keys.
- **pcg.*** -- Build PCG graphs with nodes and edges.
- **cpp.*** -- Create C++ classes, add properties/functions, trigger live coding.
- **project.*** -- Search assets, read project context, batch operations.
- **editor.*** -- Run Python scripts in the editor, manage snapshots, compile.
- **olive.*** -- Search community blueprints (150K+), get tested recipes for common patterns.

## Three Approaches to Blueprint Graphs

1. **plan_json** (`blueprint.preview_plan_json` then `blueprint.apply_plan_json`) -- Declare intent ("call SetActorLocation"), the resolver finds the right node. Best for 3+ nodes of standard logic. Atomic: all-or-nothing.
2. **Granular tools** (`blueprint.add_node`, `blueprint.connect_pins`, `blueprint.set_pin_default`) -- Place any UK2Node subclass. Best for node types outside plan_json's vocabulary, small edits, or wiring to existing nodes.
3. **Python** (`editor.run_python`) -- Full `unreal` module access. Auto-snapshot before execution. Best for operations no other tool covers.

Mix freely. If one approach hits a wall, switch to another.

## Key Practices

- **Read before write.** Always read a Blueprint before modifying it. Know what exists.
- **Compile after changes.** Use `blueprint.compile` after writing graph logic. Fix the first error before moving on.
- **Preview before apply.** For plan_json, call `preview_plan_json` first, then `apply_plan_json` with the fingerprint. Never batch both in one response.
- **Search before create.** Use `project.search` to check if the asset already exists. Modify rather than duplicate.
- **Complete the task.** Empty function shells are worthless. Write the graph logic, compile, verify.

## Discovery

- **Workflow guidance:** Use `prompts/get` for task-specific workflows (`start_task`, `build_blueprint_feature`, `modify_existing_blueprint`, `fix_compile_errors`, `research_reference_patterns`, `verify_and_finish`).
- **Domain knowledge:** Use `resources/read` for reference material (`olive://knowledge/blueprint-patterns`, `olive://knowledge/events-vs-functions`, etc.).
- **Templates:** `blueprint.list_templates(query="...")` searches factory, reference, and library templates. `blueprint.get_template(id, pattern="FuncName")` reads specific function graphs as reference.
- **Recipes:** `olive.get_recipe(query="...")` returns tested wiring patterns for common tasks (interfaces, spawning, input handling, etc.).
- **Community:** `olive.search_community_blueprints(query="...")` searches 150K+ community examples.

## Plan JSON Quick Reference

```json
{"schema_version":"2.0","steps":[
  {"step_id":"evt","op":"event","target":"BeginPlay"},
  {"step_id":"call1","op":"call","target":"PrintString","inputs":{"InString":"Hello"},"exec_after":"evt"}
]}
```

Ops: `event`, `custom_event`, `call`, `get_var`, `set_var`, `branch`, `sequence`, `cast`, `for_loop`, `for_each_loop`, `while_loop`, `do_once`, `flip_flop`, `gate`, `delay`, `is_valid`, `print_string`, `spawn_actor`, `make_struct`, `break_struct`, `return`, `comment`, `call_delegate`, `call_dispatcher`, `bind_dispatcher`

Data wires use `@step.auto` / `@step.PinName` / `@step.~hint`. Exec wires use plain `exec_after:"step_id"` (no @ prefix). Events are exec sources only -- wire FROM them, never TO them.
