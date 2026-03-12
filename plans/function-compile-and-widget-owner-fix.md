# Function Compile & Widget Owner Fix

**Date:** 2026-03-12
**Status:** Ready for implementation
**Parallelizable:** Yes -- Task A and Task B are fully independent

---

## Task A: Function/Dispatcher Creation Must Compile (Code Fix)

### Problem

`blueprint.add_function`, `blueprint.add_custom_event`, `blueprint.add_event_dispatcher`,
`blueprint.modify_function_signature`, and `blueprint.override_function` all set
`bAutoCompile = false`. Although `FOliveBlueprintWriter::AddFunction()` calls
`FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified()` -- which internally runs
`RegenerateSkeletonOnly` -- the resulting skeleton class is insufficient for the
Blueprint editor to fully resolve the new function. Users cannot drag off pins of
the newly created function until they restart the engine (which forces a full recompile).

The fix is simple: set `bAutoCompile = true` for all structural operations that
add or modify function/event/dispatcher signatures. This triggers a full compile in
Stage 5 (Verify) of the write pipeline, producing a complete `GeneratedClass` and
`SkeletonGeneratedClass`.

### Why Full Compile, Not Skeleton-Only

`MarkBlueprintAsStructurallyModified` already does `RegenerateSkeletonOnly`. The fact
that pin dragging still fails means the skeleton-only pass is not enough for this case.
A full compile via `bAutoCompile = true` is the same path that `apply_plan_json` and
`add_interface` use successfully. The compile cost for an empty function graph is
negligible (sub-100ms).

### File to Modify

**`Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`**

### Exact Changes (5 locations)

**1. `HandleAddFunctionType_Function` -- line 4051**
```
BEFORE: Request.bAutoCompile = false;
AFTER:  Request.bAutoCompile = true;
```

**2. `HandleAddFunctionType_CustomEvent` -- line 4178**
```
BEFORE: Request.bAutoCompile = false;
AFTER:  Request.bAutoCompile = true;
```

**3. `HandleAddFunctionType_EventDispatcher` -- line 4273**
```
BEFORE: Request.bAutoCompile = false;
AFTER:  Request.bAutoCompile = true;
```

**4. `HandleAddFunctionType_Override` -- line 4343**
```
BEFORE: Request.bAutoCompile = false;
AFTER:  Request.bAutoCompile = true;
```

**5. `modify_function_signature` handler -- line 4575**
```
BEFORE: Request.bAutoCompile = false;
AFTER:  Request.bAutoCompile = true;
```

### What NOT to Change

- `blueprint.remove_function` (line 4473) -- `bAutoCompile = false` is correct here.
  Removing a function does not need pin resolution. The skeleton regen from
  `MarkBlueprintAsStructurallyModified` is sufficient for removal.

- `blueprint.create` (line 2217) -- `bAutoCompile = false` is correct. Creating an
  empty Blueprint does not add functions.

- Variable and component operations -- these do not affect function signatures.

### Verification

After the fix, this workflow should work without engine restart:
1. `blueprint.add_function` with signature (inputs/outputs)
2. Open the Blueprint in the editor
3. Drag off any pin on the function entry or result node -- should show pin menu
4. From another graph, right-click and search for the function name -- should appear

### Coder Assignment

Junior coder. Five single-line changes. No logic changes, no new code.

---

## Task B: Widget Ownership Knowledge (Recipe/Knowledge Fix)

### Problem

When the AI agent creates a Widget Blueprint (e.g., WBP_InventoryGrid) with an
`OwnerCharacter` variable marked `expose_on_spawn=true`, it never wires a character
reference. At runtime: "Accessed None trying to read property OwnerCharacter".

The existing knowledge covers dispatcher-based widget updates (Section "UI / Widget
Updates" in `blueprint_design_patterns.txt`) but lacks guidance on:
1. How widgets obtain references to game actors (the "widget ownership" pattern)
2. That `expose_on_spawn` variables must be set before `AddToViewport`
3. The simpler Construct-time pattern: GetPlayerCharacter + Cast

### Fix: Add Widget Ownership Section to `blueprint_design_patterns.txt`

The best location is in the existing "UI / Widget Updates" section in
`blueprint_design_patterns.txt`, immediately after the existing dispatcher pattern
guidance (after line 62, before "### Prefer Better Patterns" on line 64).

### File to Modify

**`Content/SystemPrompts/Knowledge/blueprint_design_patterns.txt`**

### Content to Insert (after line 62, before the blank line preceding "### Prefer Better Patterns")

```
### Widget Ownership — Connecting Widgets to Game Actors

Widgets need actor references to display game state. Three patterns, simplest first:

Pattern 1 — Construct-time resolution (simplest, single-player):
  In the widget's Construct event:
  1. GetPlayerCharacter(0) -> Cast to BP_ThirdPersonCharacter -> Set OwnerCharacter
  Best when: only one player, widget always refers to the local player's character.
  No setup needed by the widget creator.

Pattern 2 — expose_on_spawn variable (explicit, multi-widget safe):
  1. Widget has OwnerCharacter variable with expose_on_spawn = true
  2. Creator calls CreateWidget -> sets OwnerCharacter -> AddToViewport
  CRITICAL: expose_on_spawn variables are only settable BETWEEN CreateWidget and
  AddToViewport. If you AddToViewport first, the variable is never set.
  In plan_json on the creator's side:
    create_widget -> set_var OwnerCharacter on widget ref -> add_to_viewport
  Best when: different widget instances track different actors.

Pattern 3 — Event dispatcher binding (reactive, decoupled):
  Already covered above. Widget binds to actor's dispatcher in Construct.
  Best when: widget reacts to state CHANGES, not just reads initial state.

Common mistake: creating a widget with expose_on_spawn=true but never setting the
variable. The widget compiles clean but crashes at runtime with "Accessed None".
If a widget has an actor reference variable, it MUST be initialized — either in
Construct (Pattern 1) or by the creator (Pattern 2).

CreateWidget requires an owning player controller. Use GetPlayerController(0) or,
from within a widget, GetOwningPlayer(). Never pass None as the owning player.
```

### Also Update: Recipe Routing

**`Content/SystemPrompts/Knowledge/recipe_routing.txt`**

Add a line after the existing widget mention (line 2) to make widget ownership
discoverable:

```
- Widget Blueprints that need actor data: wire ownership in Construct (GetPlayerCharacter + Cast) or use expose_on_spawn set before AddToViewport. See blueprint_design_patterns Section "Widget Ownership".
```

Insert this as a new line after line 5 (the "MODIFY existing" line), since it applies
to both new and modified widget workflows.

### Verification

After the fix, when the agent is asked to create a widget that displays character
data, the knowledge base should guide it to either:
- Add a Construct event that gets the player character and casts
- Or properly wire expose_on_spawn variables before AddToViewport

### Coder Assignment

Junior coder. Text-only changes to two knowledge files. No C++ code.

---

## Implementation Order

Both tasks are independent and can be done in parallel:
- **Task A**: One coder edits `OliveBlueprintToolHandlers.cpp` (5 one-line changes)
- **Task B**: One coder edits `blueprint_design_patterns.txt` and `recipe_routing.txt`

Task A should be tested with a build + manual verification in the editor.
Task B requires no build -- just verify the files are syntactically correct and
the recipe system loads without errors.
