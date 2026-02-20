# Phase 1 Task — Blueprint Reader/Writer Implementation (Plan 12.3 / 12.4)

> **Date:** February 19, 2026  
> **Status:** Ready for implementation  
> **Scope:** `UE_Olive_AI_Studio` plugin only (`Source/OliveAIEditor`, `Source/OliveAIRuntime`, `docs/`, `plans/`)

---

## 1. Objective

Implement Blueprint MCP reader and writer tools for Phase 1, including:

- A core graph walker that supports all `UEdGraph` types
- Type-specific serializers for `AnimGraph` and `WidgetTree`
- Full Blueprint read tools
- Blueprint write tools with mandatory safety pipeline:
  - Validate -> Confirm (tier routing) -> Transact -> Execute -> Verify -> Report

---

## 2. Deliverables

### 2.1 Reader Foundation

1. Implement a graph traversal layer that can inspect all `UEdGraph`-derived graphs used by Blueprints.
2. Provide reusable graph-to-IR serialization primitives:
   - Nodes (identity, class, display metadata)
   - Pins (direction, type, links, defaults)
   - Graph-level metadata (schema, owning blueprint, graph category)
3. Add specialized serializer extensions for:
   - Animation Blueprint `AnimGraph`
   - Widget Blueprint `WidgetTree`

### 2.2 MCP Reader Tools

Implement and register these tools with stable, additive JSON schemas:

- `blueprint.read` (full Blueprint IR)
- `blueprint.read_function` (single function graph detail)
- `blueprint.read_event_graph` (event graph detail)
- `blueprint.read_variables` (variable list only)
- `blueprint.read_components` (component tree only)
- `blueprint.read_hierarchy` (inheritance chain)
- `blueprint.list_overridable_functions` (parent overridables)

### 2.3 Writer Safety Pipeline (Shared)

Implement a shared write pipeline service used by all writer tools:

1. Validate inputs + preconditions
2. Route confirmation tier (Tier 1/2/3 policy)
3. Open transaction (`FScopedTransaction`) and `Modify()` impacted objects
4. Execute mutation
5. Verify result (structural checks + compile where applicable)
6. Return structured report (status, mutation summary, verification output, errors/suggestions)

### 2.4 MCP Writer Tools

Implement and register the following writer tools using the shared pipeline.

**Asset-Level**
- `blueprint.create`
- `blueprint.set_parent_class`
- `blueprint.add_interface`
- `blueprint.remove_interface`
- `blueprint.compile`
- `blueprint.delete`

**Variables**
- `blueprint.add_variable`
- `blueprint.remove_variable`
- `blueprint.modify_variable`

**Components**
- `blueprint.add_component`
- `blueprint.remove_component`
- `blueprint.modify_component`
- `blueprint.reparent_component`

**Functions**
- `blueprint.add_function`
- `blueprint.remove_function`
- `blueprint.modify_function_signature`
- `blueprint.add_event_dispatcher`
- `blueprint.override_function`
- `blueprint.add_custom_event`

**Graph Editing**
- `blueprint.add_node`
- `blueprint.remove_node`
- `blueprint.connect_pins`
- `blueprint.disconnect_pins`
- `blueprint.set_pin_default`
- `blueprint.set_node_property`

**Animation BP**
- `animbp.add_state_machine`
- `animbp.add_state`
- `animbp.add_transition`
- `animbp.set_transition_rule`

**Widget BP**
- `widget.add_widget`
- `widget.remove_widget`
- `widget.set_property`
- `widget.bind_property`

---

## 3. Implementation Plan

### 3.1 Core Reader + IR

1. Define/confirm IR structs for generic graph serialization and specialized anim/widget payloads.
2. Build a generic graph walker service (`UEdGraph`-first, schema-aware, pagination-safe for large graphs).
3. Add serializer adapters for Blueprint function/event graphs, `AnimGraph`, and `WidgetTree`.
4. Add service-level tests or validation commands for deterministic read outputs on representative assets.

### 3.2 Reader MCP Integration

1. Add tool handlers for all reader tools.
2. Wire handlers into MCP tool registry.
3. Ensure consistent response envelopes (`ok`, `error`, `warnings`, `data`).
4. Update user-facing docs for tool signatures and examples.

### 3.3 Shared Writer Pipeline

1. Create a write orchestrator that enforces the six-stage safety pipeline.
2. Implement tier routing hooks and structured confirmation payload support.
3. Centralize transaction and post-write verification behavior.
4. Standardize structured error/suggestion output for model self-correction loops.

### 3.4 Writer MCP Integration (Phased)

1. Deliver writer tools in this order to minimize risk:
   - Asset-Level -> Variables -> Components -> Functions -> Graph Editing -> AnimBP -> Widget BP
2. Reuse common validation primitives (asset resolution, type checks, pin compatibility, hierarchy safety).
3. Verify each tool with compile-and-readback checks where applicable.
4. Update tool registry docs as each group lands.

---

## 4. Acceptance Criteria

1. Core graph walker reads all Blueprint graph types encountered in project assets without crashing.
2. `AnimGraph` and `WidgetTree` produce dedicated structured payloads, not generic fallback only.
3. All listed reader tools are callable through MCP and return valid structured outputs.
4. All listed writer tools route through the shared six-stage write pipeline.
5. Every write operation is transaction-wrapped and supports undo semantics.
6. Every write tool returns verification details (including compile results where relevant).
7. Tool registry and docs reflect all added tools and schemas.

---

## 5. Verification Checklist

- [ ] Run targeted build for `OliveAIEditor` after reader foundation work
- [ ] Validate each reader tool on at least one real Blueprint asset
- [ ] Validate write pipeline logs show all six stages for each writer operation
- [ ] Validate transaction/undo behavior for representative write tools
- [ ] Validate compile + readback for graph-changing operations
- [ ] Confirm MCP tool discovery lists all new reader/writer tools
- [ ] Update docs with final request/response schema snapshots

---

## 6. Out of Scope (This Task)

- Behavior Tree tool implementation
- PCG tool implementation
- C++ source/reflection tool implementation
- Multi-asset orchestration beyond single-tool operation boundaries
