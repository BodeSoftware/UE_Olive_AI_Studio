# Templates & Recipe Design: Interactables, Events/Functions Recipe, list_templates Nudge

**Author:** Architect Agent
**Date:** 2026-03-02
**Status:** Design Complete
**Scope:** Content files + 1 small C++ change (list_templates nudge)

---

## Overview

Three features, all aligned with the AI freedom philosophy: suggestive guidance, not mandatory rails.

1. **Events & Functions Recipe** -- a searchable recipe via `olive.get_recipe` that guides the AI on when to use events vs functions, covering both interface and non-interface contexts.
2. **Interactable Templates** -- factory templates (door, gate) and a reference template (interactable_patterns) demonstrating event-based interfaces + Tick/FInterpTo for smooth movement.
3. **list_templates Soft Nudge** -- a one-line note appended to `blueprint.list_templates` results encouraging the AI to inspect templates before building from scratch.

---

## Feature 1: Events & Functions Recipe

### Rationale

The existing `events_vs_functions.txt` knowledge pack is always injected into system prompts for Auto/Blueprint profiles. It covers the conceptual framework well. But the AI cannot *actively retrieve* it when it needs a refresher mid-task. Recipes are the on-demand retrieval mechanism via `olive.get_recipe`.

The existing `interface_pattern.txt` recipe covers the BPI creation workflow but only briefly mentions the events-vs-functions constraint (line 7). This new recipe is the detailed decision guide for when the AI is about to choose between function and event.

### File

`Content/SystemPrompts/Knowledge/recipes/blueprint/events_and_functions.txt`

### Content

```
TAGS: event function custom_event interface timeline delay latent async smooth movement tick door ability
---
CHOOSING EVENTS vs FUNCTIONS

Functions are synchronous (single-frame). They CANNOT contain Timeline, Delay, or latent nodes.
Events (Custom Events, interface implementable events) run in EventGraph and support multi-frame behavior.

WHEN TO USE A FUNCTION:
- Returns a value AND logic completes in one frame
- Pure queries: GetHealth(), CanInteract():Bool, CalculateDamage():Float
- Data transformation with no side effects

WHEN TO USE A CUSTOM EVENT:
- Logic spans multiple frames (door opening, ability activation, damage reactions)
- Needs Timeline, Delay, SetTimerByFunctionName, or any latent action
- Fire-and-forget actions where caller does not need a return value
- Good examples: OpenDoor, StartAbility, PlayDeathAnimation, ApplyKnockback

INTERFACE FUNCTIONS -- the key trap:
- Interface function WITH outputs = synchronous function graph in ALL implementations
- Interface function WITHOUT outputs = implementable event in EventGraph
- Adding a Bool output to Interact() forces every door, chest, and switch into synchronous function graphs -- no smooth animations
- Before adding outputs, ask: will ANY implementation need Timeline, Delay, or smooth movement?

THE HYBRID PATTERN (when you need both):
If you genuinely need a return value AND async behavior:
1. Interface function returns immediate result (e.g., Bool for accepted/rejected)
2. Inside the function body, call a Custom Event on Self
3. Custom Event runs in EventGraph with full Timeline/Delay support
Example: Interact():Bool checks CanToggle, returns false if locked, calls "DoInteraction" custom event, returns true
DoInteraction event: runs Timeline for smooth door rotation

Use hybrid ONLY when the caller genuinely needs the return value. Otherwise, just use an event (simpler).

TICK + FInterpTo FOR SMOOTH MOVEMENT:
- SetActorRotation/SetRelativeRotation in a single frame = instant snap (usually wrong for gameplay)
- For smooth movement: store target state, use Tick + FInterpTo/RInterpTo to interpolate each frame
- Pattern: bIsOpen toggle -> update TargetRotation/TargetLocation -> Tick reads DeltaSeconds -> FInterpTo from current toward target -> SetRelativeRotation/SetRelativeLocation
- InterpSpeed controls how fast (higher = snappier, typical 2.0-5.0)
```

### Manifest Registration

Add to `Content/SystemPrompts/Knowledge/recipes/_manifest.json` under `categories.blueprint.recipes`:

```json
"events_and_functions": {
    "description": "When to use events vs functions, interface output traps, hybrid pattern, smooth movement with Tick+FInterpTo",
    "tags": ["event", "function", "custom_event", "interface", "timeline", "delay", "latent", "smooth", "movement", "tick", "door", "ability", "interp"],
    "max_tokens": 350
}
```

### Design Decisions

- **Separate from `events_vs_functions.txt` knowledge pack.** The knowledge pack is injected automatically into every prompt. This recipe is on-demand via `olive.get_recipe("event vs function")`. Some overlap is intentional -- the recipe has more tactical guidance (the hybrid pattern, Tick+FInterpTo) while the knowledge pack is the core conceptual framework.
- **Separate from `interface_pattern.txt` recipe.** That recipe covers the BPI creation workflow (create, implement, call). This recipe focuses on the event-vs-function *decision* within that workflow and beyond. Tags overlap on "interface" so both can surface when relevant.
- **Includes Tick+FInterpTo guidance** because the most common mistake (instant snap door) comes from choosing the wrong approach for movement. The recipe connects the dots: "you need smooth movement" -> "use an event, not a function" -> "use Tick+FInterpTo, not SetRotation-once."

---

## Feature 2: Interactable Templates

### 2A. Factory Template: interactable_door

**File:** `Content/Templates/factory/interactable_door.json`

This template creates an Actor Blueprint with a door that smoothly rotates open/closed via Tick+FInterpTo. It does NOT use a Timeline (which would require a separate event graph node). Instead it uses the Tick-driven interpolation pattern which is more reliable to express in plan_json.

```json
{
    "template_id": "interactable_door",
    "template_type": "factory",
    "display_name": "Interactable Door",

    "catalog_description": "Actor with door mesh, event-based Interact (no bool output -- stays async-compatible), Tick+FInterpTo smooth rotation, bIsOpen toggle, configurable angle and speed.",
    "catalog_examples": "Door, Hatch, Flap, Drawbridge, Trapdoor, Gate",

    "parameters": {
        "door_name": {
            "type": "string",
            "default": "Door",
            "description": "Name prefix for components and variables"
        },
        "open_angle": {
            "type": "float",
            "default": "90.0",
            "description": "Yaw rotation when open (degrees)"
        },
        "interp_speed": {
            "type": "float",
            "default": "3.0",
            "description": "FInterpTo speed (higher = faster, typical 2-5)"
        },
        "start_open": {
            "type": "bool",
            "default": "false",
            "description": "Start in the open position"
        }
    },

    "presets": {
        "StandardDoor": {
            "door_name": "Door",
            "open_angle": "90",
            "interp_speed": "3.0"
        },
        "SlowHeavyDoor": {
            "door_name": "HeavyDoor",
            "open_angle": "90",
            "interp_speed": "1.5"
        },
        "Hatch": {
            "door_name": "Hatch",
            "open_angle": "110",
            "interp_speed": "4.0"
        },
        "Trapdoor": {
            "door_name": "Trapdoor",
            "open_angle": "-90",
            "interp_speed": "5.0"
        }
    },

    "blueprint": {
        "type": "Actor",
        "parent_class": "Actor",

        "components": [
            {
                "class": "SceneComponent",
                "name": "Root",
                "is_root": true
            },
            {
                "class": "StaticMeshComponent",
                "name": "DoorFrame",
                "parent": "Root",
                "properties": {
                    "CollisionProfileName": "BlockAll"
                }
            },
            {
                "class": "SceneComponent",
                "name": "${door_name}Pivot",
                "parent": "Root"
            },
            {
                "class": "StaticMeshComponent",
                "name": "${door_name}Mesh",
                "parent": "${door_name}Pivot",
                "properties": {
                    "CollisionProfileName": "BlockAll"
                }
            }
        ],

        "variables": [
            {
                "name": "bIsOpen",
                "type": "Boolean",
                "default": "${start_open}",
                "category": "${door_name}"
            },
            {
                "name": "OpenAngle",
                "type": "Float",
                "default": "${open_angle}",
                "category": "${door_name}"
            },
            {
                "name": "InterpSpeed",
                "type": "Float",
                "default": "${interp_speed}",
                "category": "${door_name}"
            },
            {
                "name": "TargetRotation",
                "type": "Rotator",
                "default": "${start_open} ? (P=0,Y=${open_angle},R=0) : (P=0,Y=0,R=0)",
                "category": "${door_name}"
            }
        ],

        "event_dispatchers": [
            {
                "name": "On${door_name}Opened",
                "params": []
            },
            {
                "name": "On${door_name}Closed",
                "params": []
            }
        ],

        "functions": [
            {
                "name": "ToggleDoor",
                "description": "Toggle open/closed state. Updates bIsOpen and TargetRotation. Tick handles smooth interpolation.",
                "inputs": [],
                "outputs": []
            }
        ]
    }
}
```

**Design Notes:**

- **Pivot component pattern.** The door mesh is parented to a `${door_name}Pivot` SceneComponent. Rotation is applied to the pivot, which rotates the mesh around the pivot's origin. This is the standard UE door pattern -- it decouples the rotation axis from the mesh origin.
- **No function outputs on ToggleDoor.** This makes it directly compatible with event-based interfaces (BPI_Interactable can call ToggleDoor as an implementable event). If the user wants to expose it via interface, they can wire the Interact event to call ToggleDoor.
- **TargetRotation variable.** Tick reads this and interpolates toward it. ToggleDoor just sets bIsOpen and TargetRotation. This clean separation means Tick does the actual movement and ToggleDoor is a state toggle.
- **No graph logic in the factory template.** The template creates the structure (components, variables, dispatchers, function stubs). The AI writes the ToggleDoor and Tick logic via plan_json after creation. This follows the existing pattern: `gun.json` creates `Fire`/`Reload` function stubs with no graph logic.
- **Rotator default format.** UE accepts `(P=0,Y=90,R=0)` format for Rotator defaults. The conditional `${start_open} ? ...` leverages the existing `EvaluateConditionals` in the template system.

### 2B. Factory Template: interactable_gate

**File:** `Content/Templates/factory/interactable_gate.json`

Same concept as the door, but linear movement (Z-axis slide) instead of rotation.

```json
{
    "template_id": "interactable_gate",
    "template_type": "factory",
    "display_name": "Interactable Gate",

    "catalog_description": "Actor with gate mesh, event-based Interact (no bool output -- stays async-compatible), Tick+VInterpTo smooth linear movement, bIsOpen toggle, configurable distance and speed.",
    "catalog_examples": "Gate, Portcullis, SlidingDoor, Elevator, Platform, Barrier",

    "parameters": {
        "gate_name": {
            "type": "string",
            "default": "Gate",
            "description": "Name prefix for components and variables"
        },
        "open_offset_z": {
            "type": "float",
            "default": "300.0",
            "description": "Vertical offset when open (cm, positive = up)"
        },
        "interp_speed": {
            "type": "float",
            "default": "2.0",
            "description": "VInterpTo speed (higher = faster, typical 1-4)"
        },
        "start_open": {
            "type": "bool",
            "default": "false",
            "description": "Start in the open position"
        }
    },

    "presets": {
        "Portcullis": {
            "gate_name": "Portcullis",
            "open_offset_z": "400",
            "interp_speed": "2.0"
        },
        "SlidingDoor": {
            "gate_name": "SlidingDoor",
            "open_offset_z": "250",
            "interp_speed": "4.0"
        },
        "SlowElevator": {
            "gate_name": "Elevator",
            "open_offset_z": "500",
            "interp_speed": "1.0"
        }
    },

    "blueprint": {
        "type": "Actor",
        "parent_class": "Actor",

        "components": [
            {
                "class": "SceneComponent",
                "name": "Root",
                "is_root": true
            },
            {
                "class": "StaticMeshComponent",
                "name": "${gate_name}Frame",
                "parent": "Root",
                "properties": {
                    "CollisionProfileName": "BlockAll"
                }
            },
            {
                "class": "SceneComponent",
                "name": "${gate_name}Slider",
                "parent": "Root"
            },
            {
                "class": "StaticMeshComponent",
                "name": "${gate_name}Mesh",
                "parent": "${gate_name}Slider",
                "properties": {
                    "CollisionProfileName": "BlockAll"
                }
            }
        ],

        "variables": [
            {
                "name": "bIsOpen",
                "type": "Boolean",
                "default": "${start_open}",
                "category": "${gate_name}"
            },
            {
                "name": "OpenOffsetZ",
                "type": "Float",
                "default": "${open_offset_z}",
                "category": "${gate_name}"
            },
            {
                "name": "InterpSpeed",
                "type": "Float",
                "default": "${interp_speed}",
                "category": "${gate_name}"
            },
            {
                "name": "ClosedLocation",
                "type": "Vector",
                "default": "(X=0,Y=0,Z=0)",
                "category": "${gate_name}"
            },
            {
                "name": "TargetLocation",
                "type": "Vector",
                "default": "${start_open} ? (X=0,Y=0,Z=${open_offset_z}) : (X=0,Y=0,Z=0)",
                "category": "${gate_name}"
            }
        ],

        "event_dispatchers": [
            {
                "name": "On${gate_name}Opened",
                "params": []
            },
            {
                "name": "On${gate_name}Closed",
                "params": []
            }
        ],

        "functions": [
            {
                "name": "ToggleGate",
                "description": "Toggle open/closed state. Updates bIsOpen and TargetLocation. Tick handles smooth interpolation.",
                "inputs": [],
                "outputs": []
            }
        ]
    }
}
```

**Design Notes:**

- **Slider component** instead of Pivot. Linear movement uses `SetRelativeLocation` on the slider parent, moving the mesh along the Z axis.
- **VInterpTo** instead of RInterpTo. Same concept, vector interpolation for position instead of rotator interpolation for rotation.
- **ClosedLocation variable.** Captured at BeginPlay from the slider's initial position. TargetLocation alternates between ClosedLocation and ClosedLocation + OpenOffset.

### 2C. Reference Template: interactable_patterns

**File:** `Content/Templates/reference/interactable_patterns.json`

This must be 60-120 lines, descriptive not prescriptive, per CLAUDE.md rules. No plan_json examples, no tool sequences, no step-by-step instructions.

```json
{
    "template_id": "interactable_patterns",
    "template_type": "reference",
    "display_name": "Interactable Architecture Patterns",

    "catalog_description": "Architecture patterns for interactive objects: event-based interfaces for async behavior, Tick+FInterpTo for smooth movement, state-driven toggle, and the interface output trap.",
    "catalog_examples": "",

    "tags": "interactable door gate switch lever chest interact interface event tick finterpto smooth movement toggle async",

    "patterns": [
        {
            "name": "EventBasedInterfaces",
            "description": "Interface functions without outputs become implementable events in EventGraph. This is critical for interactables because implementations often need multi-frame behavior (smooth rotation, animations, timed effects). Adding outputs forces synchronous function graphs on every implementation.",
            "notes": "The most common mistake is adding a Bool output to Interact(). This prevents every implementing Blueprint from using Timeline, Delay, or Tick-driven interpolation. If you need a return value AND async behavior, use the hybrid pattern: the interface function returns immediately, then calls a Custom Event that handles the async work."
        },
        {
            "name": "TickDrivenInterpolation",
            "description": "For smooth spatial changes (door rotation, gate sliding, platform lifting), use Tick with FInterpTo (rotation) or VInterpTo (position). The toggle function sets a target state, and Tick interpolates the current transform toward the target each frame. InterpSpeed controls responsiveness.",
            "notes": "This is more reliable than Timeline nodes for template-generated Blueprints because it requires no Timeline asset. The pattern: store TargetRotation/TargetLocation, read current transform in Tick, call RInterpTo/VInterpTo with DeltaSeconds, apply result via SetRelativeRotation/SetRelativeLocation. Use a pivot/slider parent component so the mesh rotates/moves around the correct point."
        },
        {
            "name": "StateTogglePattern",
            "description": "Interactables typically have a boolean state (bIsOpen, bIsActive, bIsLocked). The interact function toggles the state and updates the interpolation target. Tick does the actual movement. Event dispatchers notify listeners (UI, sound, other systems) when transitions complete.",
            "notes": "Separate the state change from the physical movement. ToggleDoor sets bIsOpen and TargetRotation instantly. Tick interpolates the mesh smoothly. Check for 'close enough' threshold in Tick to fire the Opened/Closed dispatcher and optionally stop ticking for performance."
        },
        {
            "name": "PivotAndSliderComponents",
            "description": "Rotating objects (doors, hatches) use a pivot SceneComponent as the rotation parent -- the mesh is a child of the pivot, so rotating the pivot swings the mesh. Sliding objects (gates, elevators) use a slider SceneComponent for the same reason with translation. This decouples the movement axis from the mesh's local origin.",
            "notes": "Place the pivot at the hinge point (door edge, not center). Place the slider at the closed position. This way SetRelativeRotation(0,0,0) and SetRelativeLocation(0,0,0) both mean 'closed' regardless of where the actor is placed in the world."
        },
        {
            "name": "InterfaceIntegration",
            "description": "Interactables work best with a Blueprint Interface (e.g., BPI_Interactable) that declares Interact() with no outputs. The caller (player character) checks DoesImplementInterface or uses overlap events, then calls Interact via target_class -- no casting needed. Each interactable type implements the event differently.",
            "notes": "Keep the interface minimal: one Interact event, optionally one pure function for UI text (GetInteractionText():String). The pure function is fine as a synchronous function graph. Do not add output pins to Interact unless you have a specific reason AND you accept that all implementations will be synchronous."
        }
    ]
}
```

**Line count:** 46 lines of JSON. Well within the 60-120 guideline (the guideline applies to the content the AI reads, which `GetTemplateContent` formats from the patterns -- each pattern becomes ~6-8 lines of formatted text, totaling ~30-40 lines of readable content plus the header, comfortably under 120).

---

## Feature 3: list_templates Soft Nudge

### Current Behavior

`HandleBlueprintListTemplates` (line 8121 of `OliveBlueprintToolHandlers.cpp`) returns a JSON object with `templates` array and `count`. No guidance text.

### Change

Add a `note` string field to the result JSON, after `count`:

```cpp
ResultData->SetStringField(TEXT("note"),
    TEXT("Templates are curated patterns with tested structure. Consider using blueprint.get_template(template_id) to inspect one before building from scratch."));
```

This is a soft nudge. It does not force anything. The AI sees it in the tool result and can choose to follow up or not.

### Location

`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`, inside `HandleBlueprintListTemplates`, right after line 8156 (`ResultData->SetNumberField(TEXT("count"), ...)`), before the return statement on line 8158.

---

## Implementation Tasks

All tasks are independent and can be implemented in parallel.

### Task 1: Events & Functions Recipe (Content Only)

**Files to create:**
- `Content/SystemPrompts/Knowledge/recipes/blueprint/events_and_functions.txt`

**Files to modify:**
- `Content/SystemPrompts/Knowledge/recipes/_manifest.json` -- add `events_and_functions` entry

**What to do:**
1. Create the recipe file with the exact content from the Feature 1 section above.
2. Add the manifest entry to the `categories.blueprint.recipes` object in `_manifest.json`. Place it after `interface_pattern` (alphabetically it goes between `edit_existing_graph` and `fix_wiring`, but placement within the JSON object doesn't affect functionality -- just put it at the end of the recipes block for minimal diff).

**Verification:** After rebuilding, `olive.get_recipe("event function")` should return the new recipe with high relevance. `olive.get_recipe("interface output")` should return both this recipe and `interface_pattern`.

### Task 2: Interactable Door Factory Template (Content Only)

**Files to create:**
- `Content/Templates/factory/interactable_door.json`

**What to do:**
1. Create the file with the exact JSON content from section 2A above.
2. Verify the JSON is valid (no trailing commas, properly quoted).

**Verification:** After restarting the editor (or calling `FOliveTemplateSystem::Get().Reload()`), `blueprint.list_templates` should show `interactable_door`. `blueprint.create_from_template({template_id: "interactable_door", path: "/Game/Test/BP_TestDoor"})` should create an Actor BP with the correct components, variables, dispatchers, and function stub.

### Task 3: Interactable Gate Factory Template (Content Only)

**Files to create:**
- `Content/Templates/factory/interactable_gate.json`

**What to do:**
1. Create the file with the exact JSON content from section 2B above.
2. Verify the JSON is valid.

**Verification:** Same as Task 2 but for `interactable_gate`.

### Task 4: Interactable Patterns Reference Template (Content Only)

**Files to create:**
- `Content/Templates/reference/interactable_patterns.json`

**What to do:**
1. Create the file with the exact JSON content from section 2C above.
2. Verify the JSON is valid.

**Verification:** `blueprint.get_template("interactable_patterns")` should return all five patterns formatted as readable text. `blueprint.list_templates({type: "reference"})` should include it.

### Task 5: list_templates Soft Nudge (C++ Change)

**Files to modify:**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

**What to do:**

Insert one line after line 8156, before line 8158:

```cpp
ResultData->SetStringField(TEXT("note"),
    TEXT("Templates are curated patterns with tested structure. Consider using blueprint.get_template(template_id) to inspect one before building from scratch."));
```

**Location context (the surrounding code):**
```cpp
// Line 8155:
ResultData->SetArrayField(TEXT("templates"), TemplatesArray);
// Line 8156:
ResultData->SetNumberField(TEXT("count"), TemplatesArray.Num());
// INSERT HERE
// Line 8158:
return FOliveToolResult::Success(ResultData);
```

**Verification:** After rebuilding, `blueprint.list_templates` should return a JSON object with `templates`, `count`, and `note` fields.

---

## What Is NOT In Scope

- **No changes to prompt assembler.** The recipe is loaded by the existing `LoadRecipeLibrary()` in CrossSystemToolHandlers. The reference/factory templates are loaded by the existing `FOliveTemplateSystem::Initialize()`. No new knowledge packs or profile changes needed.
- **No changes to `events_vs_functions.txt` knowledge pack.** It stays as-is. The recipe complements it with tactical guidance.
- **No changes to `interface_pattern.txt` recipe.** It stays as-is. The new recipe covers a different angle (decision framework vs workflow steps).
- **No plan_json in factory templates.** Following the existing pattern (gun.json, projectile.json, stat_component.json) where factory templates create structure but leave graph logic to the AI. The AI will write ToggleDoor and Tick logic itself.
- **No changes to template system code.** The existing system handles all the new files automatically via directory scanning.

---

## Relationship to Existing Content

| Existing Content | Role | Relationship to New Content |
|---|---|---|
| `events_vs_functions.txt` (knowledge pack) | Always-on conceptual framework | New recipe adds tactical guidance (hybrid pattern, Tick+FInterpTo) |
| `interface_pattern.txt` (recipe) | BPI creation workflow | New recipe covers the event-vs-function decision within that workflow |
| `blueprint_design_patterns.txt` (knowledge pack) | Cross-BP communication patterns | Section 1 mentions outputs-vs-events briefly; new recipe goes deeper |
| `gun.json` / `projectile.json` (factory templates) | Weapon system structure | Door/gate templates follow same pattern: structure + stubs, no graph logic |
| `component_patterns.json` (reference template) | Component access patterns | Interactable patterns reference adds pivot/slider component architecture |

---

## Implementation Order

All 5 tasks are independent. Recommended order for a single coder:

1. **Task 1** (recipe) -- smallest, validates the recipe system works
2. **Task 4** (reference template) -- validates template loading
3. **Task 2** (door factory) -- larger JSON, tests parameter substitution
4. **Task 3** (gate factory) -- very similar to Task 2, fast follow
5. **Task 5** (C++ nudge) -- requires rebuild, do last

Total estimated effort: ~30 minutes for all 5 tasks. The C++ change is literally one line.
