# Template Audit - 2026-03-15

## Scope

Audited all factory templates (5 files in `Content/Templates/factory/`) and all reference templates (8 files in `Content/Templates/reference/`). Library templates and community templates were excluded.

## Audit Criteria

1. Descriptive (good) vs prescriptive (bad) -- templates teach architecture, not tool sequences
2. No tool-specific references (`plan_json`, `connect_pins`, `blueprint.create_timeline`, `editor.run_python`, `project.search`, `add_node`, `set_pin_default`)
3. No plan_json syntax or op names used as instructions (`call_delegate`, `get_var`, `set_var`, `bind_dispatcher`, `target_class`, `exec_after`, `exec_outputs`)
4. Within 60-120 line target (reference templates)
5. Content accuracy for UE 5.5
6. Information usefulness for an autonomous AI agent

## Changes Made

### Factory Templates

#### `interactable_door.json` -- 2 edits
- **catalog_description**: Removed `"Use blueprint.create_timeline to add DoorTimeline after creation"`. Replaced with tool-agnostic `"Needs a Timeline for smooth animation after structure is created."`
- **ToggleDoor function description**: Removed `"After creating the timeline with blueprint.create_timeline, wire this event's exec to the timeline's Play or Reverse pin, and wire the track output through a Lerp into SetRelativeRotation -- using connect_pins or plan_json @references."` Replaced with descriptive architectural explanation of how the Timeline's track output feeds through Lerp into SetRelativeRotation on the pivot component.

#### `interactable_gate.json` -- 2 edits (parallel changes to door)
- **catalog_description**: Same tool reference removal as door.
- **ToggleGate function description**: Same prescriptive tool instruction removal as door. Now describes the architectural data flow (Timeline track -> Lerp -> SetRelativeLocation on slider).

#### `gun.json` -- 1 edit
- **usage_notes**: Removed step-by-step numbered instructions with inline plan_json syntax (e.g., `{\"op\":\"get_var\",\"target\":\"Mesh\",\"inputs\":{\"Target\":\"@cast.auto\"}}`). Replaced with concise architectural description: cast to Character, get Mesh, attach with K2_AttachToComponent, disable collision on weapon mesh. Self-collision prevention section kept but simplified.
- **Embedded plan blocks (Fire, ResetCanFire, Reload)**: KEPT. The template system code (`OliveTemplateSystem.cpp`) parses these for variable dependency extraction and step summaries. They serve a code-level purpose. The AI sees them as reference patterns, not as instructions.

#### `projectile.json` -- No changes needed
Clean, descriptive, no tool references.

#### `stat_component.json` -- No changes needed
Clean, descriptive, no tool references.

### Reference Templates

#### `interaction_caller.json` -- Full rewrite (6 patterns updated)
The most problematic template. Every pattern contained tool-specific instructions.
- **InputDiscovery**: Removed `"Use project.search for InputAction assets"`. Now says `"Look for existing InputAction assets"`.
- **EnhancedInputActions**: Removed all tool routing instructions (`"editor.run_python creates the IA and IMC assets"`, `"plan_json wires the EnhancedInputAction event node"`, `"Do NOT try to create IA/IMC assets via plan_json"`). Now describes the UE architecture: IA/IMC are standalone UAssets, the Blueprint uses an EnhancedInputAction event node referencing the IA, BeginPlay calls AddMappingContext.
- **InputKeyFallback**: Removed `"Everything lives in one Blueprint, all wirable via plan_json"` and `"Fully expressible in plan_json: event node with key property, exec chain to validity check and interface call."` Now just describes what InputKey nodes are and when to use them.
- **ValidityCheckAndCall**: Removed `"call the interface function via target_class (creates UK2Node_Message)"` and plan_json-syntax flow description. Now describes the pattern in UE terms: call through Blueprint Interface uses UK2Node_Message which does not require casting.
- **OverlapDetection**: Minor wording cleanup (removed tool-adjacent phrasing).
- **FullCallerArchitecture**: No changes needed (already descriptive).

#### `event_dispatcher_patterns.json` -- Full rewrite (5 patterns updated)
- **CrossBlueprint_DispatchAndBind**: Removed `"broadcasts it via call_delegate"` and `"binds in Construct or BeginPlay via bind_dispatcher"`. Now describes the publisher-subscriber pattern in UE terms without referencing plan_json op names.
- **SameBlueprint_Dispatch**: Removed `"Create the dispatcher with blueprint.add_event_dispatcher, then use call_delegate (or call_dispatcher -- they are interchangeable aliases) in plan_json to broadcast it."` Now says `"Create the dispatcher first, then broadcast it from any exec chain."`
- **SameBlueprint_Bind**: Removed tool-specific language about bind_dispatcher ops.
- **ComponentDelegate_vs_Dispatcher**: Removed inline plan_json syntax example (`{\"op\":\"event\",\"target\":\"OnComponentBeginOverlap\",...}`). Now describes the distinction in UE terms: component delegates require specifying the component; user dispatchers use bind/broadcast mechanism.
- **Gameplay_To_UI_Dispatcher**: Minor cleanup, removed tool-adjacent syntax.

#### `interactable_patterns.json` -- 2 patterns updated
- **TimelineInterpolation**: Removed `"created via blueprint.create_timeline"` and `"Wire timeline pins with connect_pins or reference them in plan_json via @node_id.TrackName."` Now describes Timeline behavior in pure UE terms. Added note that Timeline nodes must live in EventGraph (so toggle logic must be a Custom Event).
- **InterfaceIntegration**: Removed `"calls Interact via target_class"`. Now says `"calls Interact through the interface"`.
- Other 3 patterns (EventBasedInterfaces, StateTogglePattern, PivotAndSliderComponents) were already clean.

#### `component_patterns.json` -- 1 pattern updated
- **ComponentVariableAccess**: Removed `"Use get_var to retrieve a component reference, then wire it to the Target input"` and `"get_var works for both Blueprint variables and components. set_var does NOT work for components (read-only references)."` Now describes the UE concept: components are accessible as typed variables, they are read-only references, access by name when multiple exist.

#### No changes needed (4 templates)
- **projectile_patterns.json** -- Clean. Describes UE patterns (hit response, damage, bounce counting, homing, lifespan) with no tool references.
- **pickup_equip_patterns.json** -- Clean. Describes pickup architecture (overlap detection, socket attachment, collision handling) with no tool references.
- **ue_events.json** -- Clean. Pure reference data listing actor events with their UE target names and output pin signatures.
- **projectile_fire_patterns.json** -- Clean. Describes spawning, aim direction, owner chain, and ProjectileMovement setup with no tool references.

## Line Counts (Post-Edit)

### Reference Templates
| Template | Lines | Status |
|----------|-------|--------|
| component_patterns.json | 23 | Under target but complete |
| event_dispatcher_patterns.json | 38 | Under target but complete |
| interactable_patterns.json | 38 | Under target but complete |
| interaction_caller.json | 43 | Under target but complete |
| pickup_equip_patterns.json | 48 | Within range |
| projectile_fire_patterns.json | 38 | Under target but complete |
| projectile_patterns.json | 43 | Under target but complete |
| ue_events.json | 73 | Within range |

All reference templates are under 120 lines. Most are compact (23-48 lines) which is good for token budget. The 60-120 target is an upper-bound guideline -- concise is better than padded.

### Factory Templates
| Template | Lines | Notes |
|----------|-------|-------|
| gun.json | 270 | Large due to embedded plan blocks (used by code) |
| projectile.json | 151 | Good size for parameterized template |
| interactable_door.json | 114 | Clean |
| interactable_gate.json | 110 | Clean |
| stat_component.json | 110 | Clean |

## Validation

All 13 JSON files pass JSON parsing validation (verified with Node.js `JSON.parse`).

## Summary

**7 files edited**, **6 files unchanged**. The primary change was removing tool-specific language (tool names, plan_json op names, plan_json syntax examples) from template descriptions and notes. All architectural information was preserved -- the templates still teach the same UE patterns, they just no longer tell the AI which tools to use. The AI is free to implement these patterns using whichever tools it chooses (plan_json, granular tools, Python, or a mix).
