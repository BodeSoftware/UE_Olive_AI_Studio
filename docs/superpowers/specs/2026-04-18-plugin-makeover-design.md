# Olive AI Studio Plugin Makeover — Design Spec

**Date:** 2026-04-18
**Status:** Approved through design; pending implementation plan.
**Scope:** Major simplification + expansion pass. Single unified overhaul, parallelized across coder agents via independent work packs.

## 1. Motivation

The plugin has grown to ~132 MCP tools, a 6-stage write pipeline with mode gating, three chat modes (Ask/Plan/Code), six knowledge-pack prompt files, a multi-tier self-correction policy, and a ~4000-line prompt assembly that injects template catalogs unconditionally. In practice the AI agent performs worse with more tools and more prose than with fewer.

The reference plugin `unreal-engine-mcp-main` ships ~43 focused tools, no chat UI, no modes, no templates — and for the scope it covers, it works well. It has one thing we lack (actor/level/material/world-builder tools) and is missing things that are genuinely valuable (transactions, rollback, resolver intelligence, param aliasing, 3-part errors).

This spec defines a mix: **reference-style simplicity** (smaller surface, no modes, no forced templates, new `level.*`/`material.*` tools, world-builders) + **NeoStack-style intelligence** (resolver, aliasing, resolver notes, auto-fix, transactions, snapshots). End state is a plugin that is measurably easier for an LLM to use while retaining the safety layer that distinguishes it from the reference.

## 2. End-State Vision

- Single-mode operation. No Ask/Plan/Code. `DefaultChatMode`, mode badge, mode gate pipeline stage, slash commands, and mode-conditional handler logic are removed.
- ~74 tools (down from ~132; exact count 74 per Section 3 table), organized into the same family hierarchy. `blueprint.read` swallows 7 specialized readers; `blueprint.delete` swallows 5 removers; `blueprint.modify` swallows 11 setters; `blueprint.add` swallows 7 creators. Similar consolidation for BT, PCG, Niagara, Cpp, Project.
- New `level.*` family (8 tools) for actor/level operations. New `material.*` family (5 tools). Both integrate with the write pipeline (transactions + snapshots).
- New `worldbuild/*` factory templates (pyramid, wall, staircase, arch, tower) under `Content/Templates/factory/worldbuild/`. No new C++ — uses the existing plan executor. Advanced builders (maze, bridge, castle, town) escalate to native `FOliveWorldBuilder` only if factory templates prove insufficient.
- Templates are optional. Catalog injection is gated by a new `bInjectTemplateCatalog` setting (default `true`). Prompt language presents templates as "one optional reference among several" rather than the primary path.
- Prompts slim by 40–60%. Six knowledge packs in `Content/SystemPrompts/Knowledge/` are deleted. Worker prompts are rewritten to drop mode language, drop template rails, and lean on tool schemas for specifics.
- Brain layer simplified. `FOliveSelfCorrectionPolicy` replaced with retry-once-on-transient-error. `FOliveRunManager` becomes a linear loop. `FOlivePromptDistiller` deleted. `FOliveLoopDetector` kept (real safety value). `FOliveBrainLayer` state machine kept (load-bearing for chat UI).
- NeoStack intelligence preserved. `FOliveBlueprintPlanResolver`, `NormalizeToolParams`, resolver notes, 3-part errors, auto-fix heuristics remain. This is what separates us from the reference.

## 3. Tool Surface

Target: ~74 tools (down from ~132). Exact breakdown:

| Family | Before | After | Notes |
|---|---:|---:|---|
| Blueprint | 47 | 15 | Consolidation-heavy |
| BehaviorTree + Blackboard | 19 | 7 | Merged families |
| PCG | 12 | 7 | Connect/disconnect merged |
| Niagara | 10 | 8 | Including 2 helpers |
| C++ | 11 | 6 | Read/list consolidated |
| Widget + AnimBP | 8 | 8 | Unchanged — cohesive small family |
| Project | 19 | 7 | Major dedup |
| Editor | 1 | 1 | `editor.run_python` stays |
| Olive | 3 | 2 | Drop `olive.get_recipe` |
| **NEW Level** | 0 | 8 | Reference-inspired |
| **NEW Material** | 0 | 5 | Reference-inspired |
| Test/dev | 2 | 0 | Remove |
| **Total** | **~132** | **~74** | |

### 3.1 Blueprint (47 → 16)

Kept: `blueprint.create`, `connect_pins`, `disconnect_pins`, `compile`, `apply_plan_json`, `preview_plan_json`, `describe_node_type`, `list_overridable_functions`, `list_templates`, `get_template`, `create_from_template`.

Consolidated:

- `blueprint.read` ← `read`, `read_components`, `read_event_graph`, `read_function`, `read_hierarchy`, `read_variables`, `describe_function`, `get_node_pins` (via `include=[components|variables|functions|hierarchy|event_graph|function_detail|pins]`).
- `blueprint.delete` ← `delete`, `remove_component`, `remove_function`, `remove_interface`, `remove_node`, `remove_variable` (via `entity` param).
- `blueprint.modify` ← `modify_component`, `modify_function`, `modify_function_signature`, `modify_variable`, `set_node_property`, `set_parent_class`, `set_pin_default`, `set_defaults`, `reparent_component`, `move_node`, `override_function` (via `entity` + `action` params).
- `blueprint.add` ← `add_node`, `add_variable`, `add_function`, `add_component`, `add_custom_event`, `add_event_dispatcher`, `add_interface` (via `entity` param).

Deleted: `scaffold`, `verify_completion` (fold into `compile`), `create_interface` (→ `create`), `create_timeline` (→ `add` with `entity=timeline`).

### 3.2 BehaviorTree + Blackboard (19 → 7)

- `behaviortree.create`, `behaviortree.read`, `behaviortree.add` (composite/task/decorator/service via `node_type`), `behaviortree.modify`, `behaviortree.remove`, `behaviortree.move`.
- `blackboard.modify` (action: `create`/`add_key`/`modify_key`/`remove_key`/`set_parent`, plus `read` via flag).

### 3.3 PCG (12 → 6)

`pcg.create`, `pcg.read`, `pcg.add` (node or subgraph), `pcg.modify`, `pcg.remove`, `pcg.connect` (disconnect via `break=true`), `pcg.execute`.

### 3.4 Niagara (10 → 8)

`niagara.create_system`, `niagara.read`, `niagara.add` (emitter or module), `niagara.modify`, `niagara.remove`, `niagara.compile`, plus helpers `niagara.describe_module`, `niagara.list_modules`.

### 3.5 C++ (11 → 6)

`cpp.read` (entity: class/enum/struct/header/source), `cpp.list` (kind: project/blueprint_callable/overridable), `cpp.create_class`, `cpp.add` (function or property), `cpp.modify_source`, `cpp.compile`.

### 3.6 Widget + AnimBP (8 → 8, unchanged)

`widget.add_widget`, `widget.remove_widget`, `widget.set_property`, `widget.bind_property`, `animbp.add_state`, `animbp.add_state_machine`, `animbp.add_transition`, `animbp.set_transition_rule`.

### 3.7 Project (19 → 7)

`project.search`, `project.read` (swallows `get_asset_info`, `get_class_hierarchy`, `get_config`, `get_dependencies`, `get_info`, `get_referencers`, `get_relevant_context`, `bulk_read`), `project.snapshot`, `project.rollback`, `project.diff`, `project.refactor_rename`, `project.index` (action: build/status).

Deleted: `create_ai_character`, `implement_interface`, `move_to_cpp`, `batch_write`, `list_snapshots`.

### 3.8 Editor (1 → 1)

`editor.run_python` — unchanged.

### 3.9 Olive (3 → 2)

`olive.build`, `olive.search_community_blueprints`. Delete `olive.get_recipe`.

### 3.10 NEW `level.*` (0 → 8)

- `level.list_actors` — enumerate actors in current level.
- `level.find_actors` — pattern/class filter.
- `level.spawn_actor` — spawn through write pipeline (transaction + snapshot).
- `level.delete_actor` — transacted delete.
- `level.set_transform` — loc/rot/scale.
- `level.set_physics` — per-component physics config.
- `level.apply_material` — to actor mesh component.
- `level.get_actor_materials` — query material slots.

### 3.11 NEW `material.*` (0 → 5)

- `material.list` — available materials with search path + engine filter.
- `material.apply_to_component` — Blueprint component material assignment.
- `material.set_parameter_color` — scalar/vector color parameter set.
- `material.create_instance` — dynamic material instance creation.
- `material.read` — parameters, texture refs, parent material.

### 3.12 NEW `worldbuild/*` (factory templates only, not tools)

JSONs under `Content/Templates/factory/worldbuild/`:
- `pyramid.json`, `wall.json`, `staircase.json`, `arch.json`, `tower.json`.

Invoked via existing `blueprint.create_from_template`. No new tools. Advanced builders (`maze`, `bridge`, `castle`, `town`, `house`, `mansion`, `aqueduct`) are out of scope for this pass — escalate to native `FOliveWorldBuilder` C++ only if factory templates prove insufficient in follow-up work.

## 4. Prompt + Brain Changes

### 4.1 Prompts rewritten

| File | Action | Target |
|---|---|---|
| `BaseSystemPrompt.txt` | Rewrite | ≤40% current |
| `Base.txt` | Rewrite | ≤40% current |
| `Worker_Blueprint.txt` | Rewrite | ≤30% current |
| `Worker_BehaviorTree.txt` | Trim | ≤60% current |
| `Worker_PCG.txt` | Trim | ≤60% current |
| `Worker_Niagara.txt` | Trim | ≤60% current |
| `Worker_Cpp.txt` | Trim | ≤60% current |
| `Worker_Integration.txt` | Trim | ≤60% current |
| `Orchestrator.txt` | Review/trim | ≤60% current |
| `ToolCallFormat.txt` | Keep | Minor edits only |

Universal rules:
- Delete every reference to Ask/Plan/Code mode.
- Delete "always check templates first" language. Templates become "one optional reference among several."
- Delete duplication of tool schemas. Schemas carry their own descriptions.
- Drop "recipes" language.
- Approach choice presented as flat options: "plan_json, granular tools, or python — whichever fits."
- Keep safety rails: read-before-write, compile-after-changes, fix-first-error-first.
- Keep NeoStack transparency notes: "resolver may auto-translate; check resolver_notes."

### 4.2 Knowledge packs deleted

All six files in `Content/SystemPrompts/Knowledge/` are deleted:
`blueprint_authoring.txt`, `blueprint_design_patterns.txt`, `cli_blueprint.txt`, `events_vs_functions.txt`, `node_routing.txt`, `recipe_routing.txt`.

The critical anti-patterns section from `cli_blueprint.txt` (WRONG/RIGHT/WHY for `exec-into-custom-event`, `tilde-on-struct-returns`, `@-prefix-on-exec_after`) is preserved in the rewritten `Worker_Blueprint.txt`.

`FOlivePromptAssembler` loader code that pulls knowledge packs is removed.

### 4.3 Template catalog injection

- New setting `bInjectTemplateCatalog` on `UOliveAISettings` (default `true`, editor-exposed).
- `FOliveTemplateSystem::GetCatalogBlock()` call in `FOlivePromptAssembler` wrapped in a setting check.
- Catalog text rewritten: "Templates available (optional references). Use only if one matches your task."

### 4.4 Brain simplification

**`FOliveSelfCorrectionPolicy` — replaced:**
- Delete progressive error disclosure, 3-tier error classification, `PreviousPlanHashes`.
- New implementation: `bool ShouldRetry(const FOliveToolResult& Result, int32 AttemptCount)` — returns `true` exactly once for transient errors (timeout, HTTP 5xx, rate-limit), `false` otherwise. ~30 lines vs ~400 today.
- Tool errors pass through verbatim. The 3-part error format (code + message + suggestion) carries all context the LLM needs.

**`FOliveRunManager` — simplified:**
- Delete multi-step orchestration, 5-step checkpointing, run-outcome state machine.
- New implementation: linear loop — send message → receive streamed response → execute tool calls → send results → repeat until model stops. One outcome: completed or cancelled.

**`FOliveBrainLayer` state machine — kept.** Idle → Active → Cancelling → Idle is load-bearing for the chat panel cancel button and toast notifications.

**`FOliveLoopDetector` — kept.** Infinite-loop detection is real safety value. Already small.

**`FOlivePromptDistiller` — deleted.** With smaller prompts there is no need for distillation.

**`FOliveOperationHistory`, `FOliveToolExecutionContext` — simplified during implementation.**

## 5. Mode Removal Mechanics

- `UOliveAISettings`: remove `DefaultChatMode` property.
- `SOliveAIChatPanel`: remove mode badge widget. Remove `/ask`, `/plan` slash command handlers from `FOliveEditorChatSession`. `/code` kept as a silent no-op alias for one release, then removed.
- `FOliveWritePipeline`: delete Stage 2 (Mode Gate). 6 stages → 5 stages.
- `FOliveWriteRequest`: drop `ChatMode` field.
- Tool dispatch: remove `ASK_MODE` / `PLAN_MODE` error codes, `bRequiresModeCheck`, and all mode-conditional branches from tool handlers and schemas.
- `EOliveChatMode` enum (`OliveChatMode.h`) deleted. Mode persistence in `FOliveConversationManager` deleted.
- Mode-related tests in `Source/OliveAIEditor/Private/Tests/Brain/` and `Tests/Conversation/` deleted.

## 6. Module Layout for New Families

```
Source/OliveAIEditor/
├── Level/                          # NEW
│   ├── Public/
│   │   ├── Reader/OliveLevelReader.h
│   │   ├── Writer/OliveLevelWriter.h
│   │   └── MCP/OliveLevelToolHandlers.h
│   └── Private/
│       ├── Reader/OliveLevelReader.cpp
│       ├── Writer/OliveLevelWriter.cpp
│       └── MCP/
│           ├── OliveLevelToolHandlers.cpp
│           └── OliveLevelSchemas.cpp
│
├── Material/                       # NEW
│   ├── Public/
│   │   ├── OliveMaterialReader.h
│   │   ├── OliveMaterialWriter.h
│   │   └── MCP/OliveMaterialToolHandlers.h
│   └── Private/
│       ├── OliveMaterialReader.cpp
│       ├── OliveMaterialWriter.cpp
│       └── MCP/
│           ├── OliveMaterialToolHandlers.cpp
│           └── OliveMaterialSchemas.cpp
```

Mirrors the existing `Blueprint/`, `BehaviorTree/`, `PCG/`, `Niagara/`, `Cpp/` submodule pattern. Both register in `OliveAIEditorModule.cpp::OnPostEngineInit()` after step 10 (`FOliveCrossSystemToolHandlers`). Include paths added to `OliveAIEditor.Build.cs`. Tool registration integrates with `FOliveWritePipeline` (snapshot + transaction + verify stages).

New factory template directory: `Content/Templates/factory/worldbuild/` with `pyramid.json`, `wall.json`, `staircase.json`, `arch.json`, `tower.json`. Reuses existing `blueprint.create_from_template` dispatch.

## 7. Work Breakdown (Parallel Coder Packs)

Seven work packs, sized for parallel execution with clear merge seams.

| Pack | Owner | Scope | Blockers |
|---|---|---|---|
| **P1** Mode removal | coder | Delete mode enum, settings field, UI badge, pipeline stage 2, handler mode checks, mode-related tests | — |
| **P2** Prompt + knowledge pack rewrite | creative_lead → coder | Rewrite 10 prompt files; delete 6 knowledge packs; update `FOlivePromptAssembler` loader; add `bInjectTemplateCatalog` setting; rewrite catalog injection language | P5 (new tool names) |
| **P3** Brain simplification | coder | Replace `FOliveSelfCorrectionPolicy` with retry-once; slim `FOliveRunManager` to linear loop; delete `FOlivePromptDistiller`; simplify `FOliveOperationHistory` | — |
| **P4** New `level.*` module | coder | Full submodule, 8 tools, schemas, handlers, tests | — (isolated) |
| **P5** Tool consolidation | coder | Collapse ~132 tools to ~74 across Blueprint/BT/PCG/Niagara/Cpp/Project. Keep old names as `NormalizeToolParams` aliases for one release. Update schemas, handlers, registry | — |
| **P6** New `material.*` module | coder_junior | Full submodule, 5 tools | — (isolated) |
| **P7** `worldbuild/` factory templates | coder_junior | 5 JSON files under `Content/Templates/factory/worldbuild/`. No C++ | — (isolated) |

**Parallel execution:** P4, P6, P7 are fully independent — start immediately in parallel. P1 and P3 are independent from each other and from the isolated packs. P5 is conflict-heavy (touches most tool files) and must land **before** P2 (which rewrites prompts referencing new tool names).

**Recommended merge order:** P4 + P6 + P7 (parallel) → P1 → P3 → P5 → P2.

## 8. Contracts + Merge Plan

**Serialization point: `OliveAIEditorModule.cpp`.** Every pack touches `OnPostEngineInit()`. Mitigation: introduce `OliveModuleRegistrations.inl` — each pack appends one `#include` line plus a single registration call, rather than editing function bodies. Avoids 7-way merge conflict.

**Serialization point: `OliveToolRegistry`.** P5 rewrites handler registrations for consolidated tools. Mitigation: P5 lands first; P4 and P6 register only new (non-overlapping) tools.

**Tool consolidation contract.** Before P5 starts, produce `docs/superpowers/specs/tool-consolidation-contract.md` listing every old tool name → new tool name + param mapping. `NormalizeToolParams` alias table gains the old names so external MCP clients don't break on day one. Aliases documented as deprecated, removal scheduled for next release.

**Testing gate per pack:**
- `ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject` green.
- `OliveAI.*` automation suite green in Session Frontend.
- For P4/P6: new Automation tests covering happy path + error path of each new tool.
- For P5: alias coverage test proving every old tool name dispatches to the new consolidated tool.

**Rollout:** Single main-branch overhaul. No `v2/` branch. Each pack ships as a PR to `main`. Post-final-merge validation: open test Blueprints and run end-to-end prompts through both the Slate chat panel and Claude Code CLI.

## 9. Out of Scope

- Pure MCP server mode (chat UI removed). Deferred.
- Clean-room module rewrite. Deferred.
- Advanced world builders (maze, bridge, castle, town, house, mansion, aqueduct) as native C++. Deferred.
- Hosted MCP bridge variant. Deferred.
- `Examples/` demo project and `Guides/` human tutorials. Deferred — separate polish pass after makeover lands.
- Widget and AnimBP family consolidation. Kept as-is; small enough that consolidation is not worth the churn.

## 10. Success Criteria

1. Plugin builds clean (`ubt-build-5.5`) and all non-deleted tests pass.
2. Tool count ≤ 80 in `FOliveToolRegistry` after P5 lands.
3. All six `Content/SystemPrompts/Knowledge/*.txt` files deleted.
4. No references to `EOliveChatMode`, `ASK_MODE`, `PLAN_MODE` remain in the codebase.
5. New `level.*` and `material.*` tools callable from both chat panel and MCP server, passing through write pipeline with transactions + snapshots.
6. `worldbuild/pyramid.json` etc. invokable via `blueprint.create_from_template` and produce valid Blueprints.
7. Prompt assembly output ≤ 50% of current size (measured per-turn against a fixed test prompt).
8. `FOliveSelfCorrectionPolicy` LOC reduced ≥ 70%.
9. External MCP clients calling old tool names still succeed via aliases (one release deprecation window).
10. End-to-end validation: successfully complete 3 test tasks (create a Blueprint, modify an existing one, spawn actors in a level) via each of: Claude Code CLI MCP, Slate chat panel.
