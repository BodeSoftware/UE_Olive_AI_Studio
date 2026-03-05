# Agent Planning System Design: Multi-Asset Decomposition

## Problem Statement

When users request multi-entity game systems ("create a bow and arrow for @BP_ThirdPersonCharacter"), the autonomous agent consistently creates only partial systems. For "bow and arrow", it creates BP_Arrow (using the projectile factory template) and adds bow variables/functions to the character -- but never creates a separate BP_Bow actor. This pattern persists across:

- Multiple prompt-engineering attempts in CLAUDE.md
- stdin nudges like "plan ALL assets" and "identify every distinct game entity"
- Removing the template catalog
- Reverting to earlier commits

The failure is not a regression. It is a structural gap in how the agent receives its task.

## Root Cause Analysis

The agent fails at multi-asset decomposition for three compounding reasons:

**1. The stdin message is a bare task with no structure.**
The user types "create a bow and arrow for @BP_ThirdPersonCharacter", and the agent receives roughly:

```
create a bow and arrow for @BP_ThirdPersonCharacter

### Current Asset State
**BP_ThirdPersonCharacter** (parent: Character)
- Components: ...
- Variables: ...

Before building, research patterns from Library templates...
```

There is no explicit step asking the agent to enumerate assets. The research nudge directs it toward templates, which immediately anchors it on the `projectile` factory template (whose examples list includes "Arrow"). Once anchored, the agent builds the Arrow and folds everything else into the character.

**2. Template anchoring creates a gravity well.**
The agent's first MCP call is typically `blueprint.list_templates(query="bow arrow projectile")`. It finds the `projectile` factory template, sees the "Arrow" preset, and this becomes the design. The template is a single-asset pattern (one projectile Blueprint), which frames the entire task as "one projectile + character modifications." The Bow as a separate actor never enters consideration.

**3. Knowledge packs model the right pattern but only for one domain.**
`blueprint_design_patterns.txt` Section 6 has a perfect "Assets to Create" list for an interaction system. But it is tagged for interfaces/overlap/interaction. The agent reads it as domain-specific guidance, not as a general planning methodology. There is no equivalent decomposition example for weapon systems, vehicle systems, inventory systems, etc.

**4. Nudges in CLAUDE.md are treated as suggestions.**
The autonomous agent reads the sandbox CLAUDE.md once at startup. Lines like "plan ALL assets before building" compete with hundreds of other lines of context. The agent follows the research nudge (which is in stdin, the imperative channel) more reliably than CLAUDE.md instructions. But the research nudge sends it straight to templates, skipping decomposition entirely.

## Recommended Approach: Structured Decomposition Directive in stdin

### Why This Will Work When Nudges Didn't

Previous nudges failed because they were **advisory** ("plan ALL assets") rather than **structural** (a formatted block the agent must fill in). The key insight is:

1. **stdin is the imperative channel** -- the agent follows stdin directives more reliably than CLAUDE.md content (this is documented in the CLI provider architecture notes)
2. **Structured output formats work** -- the agent reliably produces structured formats when given a template to fill (e.g., it always produces valid plan_json because the format is specified with examples)
3. **The "Assets to Create" pattern already works** -- Section 6 of `blueprint_design_patterns.txt` proves the agent CAN decompose systems when shown the pattern. It just needs the pattern applied to every task, not just interaction systems.

The approach: inject a **mandatory decomposition block** into the stdin message (the `EffectiveMessage`) that requires the agent to list all assets before calling any tool. This is not a suggestion in CLAUDE.md -- it is a structured directive in the same channel as the user's task.

### Design

#### Change 1: Decomposition Directive in stdin (OliveCLIProviderBase.cpp)

Replace the current research nudge (lines 512-515) with a decomposition-first directive:

**Current** (line 514):
```
Before building, research patterns from Library templates: search blueprint.list_templates(query="...") ...
```

**New**:
```
## Required: Asset Decomposition

Before calling ANY tools, think through the complete system design:

1. **Identify every game entity** that needs its own Blueprint. A "game entity" is anything that:
   - Exists as a separate actor in the world (weapons, projectiles, pickups, doors, keys)
   - Has its own mesh, collision, or movement (NOT just a variable on another Blueprint)
   - Could be spawned, equipped, dropped, or destroyed independently

2. **List your planned assets** in this format:
   ```
   ASSETS:
   1. BP_Name — Type (Actor/Pawn/Character/Interface) — one-line purpose
   2. BP_Name — Type — purpose
   3. Modify @ExistingBP — what changes
   ```

3. Then research patterns (blueprint.list_templates, olive.search_community_blueprints) and build each asset fully before starting the next.

Example — "create a bow and arrow system":
```
ASSETS:
1. BP_Bow — Actor — skeletal/static mesh, attach to character, handles aiming, charging, spawns arrows
2. BP_Arrow — Actor — projectile with gravity arc, damage on hit, auto-destroy
3. Modify @BP_ThirdPersonCharacter — add bow reference variable, equip/fire input handling, attach point
```

Example — "create a door and key system":
```
ASSETS:
1. BPI_Lockable — Interface — Lock/Unlock/IsLocked functions
2. BP_Key — Actor — pickup, stores which doors it opens
3. BP_LockedDoor — Actor — implements BPI_Lockable, opens when unlocked
4. Modify @BP_ThirdPersonCharacter — key inventory array, interact input, overlap detection
```

Do NOT skip this step. Do NOT start with template searches. Think first, list assets, then build.
```

#### Change 2: Add "System Decomposition" Section to blueprint_design_patterns.txt

Add a new Section 0 (before the existing Section 1) that generalizes the "Assets to Create" methodology. This reinforces the stdin directive from the reference context channel.

**New Section 0** (prepend to `blueprint_design_patterns.txt`):

```
## 0. System Decomposition — ALWAYS Do This First

Before building any multi-asset system, identify every game entity that needs a Blueprint.

### The Separate Actor Test
Ask: "Does this thing exist in the world with its own transform?"
- YES (weapon, projectile, pickup, door, vehicle, AI character) → separate Blueprint
- NO (health value, ammo count, movement speed) → variable on an existing Blueprint
- MAYBE (inventory slot, buff effect) → component on an existing Blueprint

### Common Decomposition Mistakes
- "Bow and Arrow" is TWO actors (Bow + Arrow), not one actor + character functions
- "Gun System" is typically THREE assets (Gun actor + Bullet projectile + character input)
- "Vehicle" is at minimum TWO (Vehicle pawn + character enter/exit logic)
- Weapons are ALWAYS separate actors attached to characters, never just variables/functions on the character

### Asset List Format
Write this before touching any tools:
```
ASSETS:
1. BP_Name — Type — one-line purpose
2. ...
N. Modify @ExistingBP — changes needed
```

The last asset should be modifications to the existing character/controller.

### Communication Between Assets
After listing assets, identify how they communicate:
- Spawner→Spawned: SpawnActor + set variables on the spawned actor
- Parent→Child: attach via AttachToComponent, access via stored reference
- Peer→Peer: Blueprint Interface (multiple types) or direct cast (one known type)
- Notify: Event Dispatchers for one-to-many state broadcasts
```

#### Change 3: Harden the Research Nudge Position

Move the research nudge AFTER the decomposition directive, not as a replacement. The full stdin injection order becomes:

1. User's message + @-mention asset state (existing, unchanged)
2. **Decomposition directive** (new, Change 1)
3. Research nudge (existing, repositioned -- still in stdin, but comes after decomposition)

This ensures the agent's first cognitive step is "what assets do I need?" rather than "what templates can I find?"

### Why NOT the Other Approaches

**Two-phase CLI approach (rejected):** Requires capturing output from a first Claude Code invocation, parsing it, and feeding it into a second invocation. This adds complexity to the process lifecycle, requires a new intermediate format, and doubles the CLI startup cost. The agent's problem is not inability to plan -- it's that nobody asks it to plan before it starts templating.

**Planning template/recipe (rejected as primary):** A recipe loaded via `olive.get_recipe("system_decomposition")` would require the agent to voluntarily call it. The whole problem is that the agent skips planning and goes straight to templates. You cannot fix "agent skips step X" by adding another optional step X'.

**Enhanced knowledge packs alone (rejected as primary):** Knowledge packs are reference context (CLAUDE.md / --append-system-prompt). The agent treats these as suggestions. The fix must be in the imperative channel (stdin). Knowledge pack changes are included as reinforcement (Change 2), not the primary mechanism.

## Exact File Changes

### File 1: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**Location:** `SendMessageAutonomous()`, lines 509-515 (the `if (!IsContinuationMessage(UserMessage))` block)

**Change:** Replace the current research-only nudge with the decomposition directive followed by the research nudge.

The new text block is approximately 40 lines. It replaces the single `EffectiveMessage +=` call at line 514 with two appends:

1. The decomposition directive (Change 1 text above)
2. A shortened research nudge: `"After listing your assets, research patterns: blueprint.list_templates(query=\"...\") for proven reference patterns, olive.search_community_blueprints for community examples. Then build each asset fully before starting the next."`

The continuation check (`!IsContinuationMessage`) remains -- decomposition is only injected on the initial message, not on auto-continues.

### File 2: `Content/SystemPrompts/Knowledge/blueprint_design_patterns.txt`

**Location:** Before the existing first line (`TAGS: interface cast...`)

**Change:** Insert Section 0 (Change 2 text above) after the TAGS line and `---` separator, before the existing `## Cross-Blueprint Communication Patterns` heading. Update the TAGS line to include new keywords:

```
TAGS: decomposition planning assets entities weapon projectile interface cast dispatcher ...
```

### File 3: `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**Location:** The `MULTI-ASSET` line (line 24)

**Change:** Expand the one-liner:

Current:
```
MULTI-ASSET: Complete one asset fully (structure + logic + compile) before starting the next.
```

New:
```
MULTI-ASSET: First, list ALL Blueprints the system needs (see System Decomposition in design patterns). Then build each one fully (structure + logic + compile) before starting the next.
```

## Example: What the Agent Sees for "create a bow and arrow system for @BP_ThirdPersonCharacter"

### stdin (EffectiveMessage)

```
create a bow and arrow system for @BP_ThirdPersonCharacter

### Current Asset State

**BP_ThirdPersonCharacter** (parent: Character)
- Components: CapsulComponent (CapsuleComponent), ArrowComponent (ArrowComponent), Mesh (SkeletalMeshComponent), ...
- Variables: ...
- Functions: ...

**Do NOT re-read these assets** -- their current state is shown above. Focus on making the requested changes.

## Required: Asset Decomposition

Before calling ANY tools, think through the complete system design:

1. **Identify every game entity** that needs its own Blueprint. A "game entity" is anything that:
   - Exists as a separate actor in the world (weapons, projectiles, pickups, doors, keys)
   - Has its own mesh, collision, or movement (NOT just a variable on another Blueprint)
   - Could be spawned, equipped, dropped, or destroyed independently

2. **List your planned assets** in this format:
   ```
   ASSETS:
   1. BP_Name — Type (Actor/Pawn/Character/Interface) — one-line purpose
   2. BP_Name — Type — purpose
   3. Modify @ExistingBP — what changes
   ```

3. Then research patterns (blueprint.list_templates, olive.search_community_blueprints) and build each asset fully before starting the next.

Example — "create a bow and arrow system":
```
ASSETS:
1. BP_Bow — Actor — skeletal/static mesh, attach to character, handles aiming, charging, spawns arrows
2. BP_Arrow — Actor — projectile with gravity arc, damage on hit, auto-destroy
3. Modify @BP_ThirdPersonCharacter — add bow reference variable, equip/fire input handling, attach point
```

Example — "create a door and key system":
```
ASSETS:
1. BPI_Lockable — Interface — Lock/Unlock/IsLocked functions
2. BP_Key — Actor — pickup, stores which doors it opens
3. BP_LockedDoor — Actor — implements BPI_Lockable, opens when unlocked
4. Modify @BP_ThirdPersonCharacter — key inventory array, interact input, overlap detection
```

Do NOT skip this step. Do NOT start with template searches. Think first, list assets, then build.

After listing your assets, research patterns: blueprint.list_templates(query="...") for proven reference patterns, olive.search_community_blueprints for community examples. Then build each asset fully before starting the next.
```

### CLAUDE.md (sandbox, read once at startup)

Contains the existing content plus the new Section 0 in `blueprint_design_patterns.txt` which reinforces the decomposition methodology from the reference context channel.

## How This Handles Other Cases

### "Create a gun system with reloading"
Agent should produce:
```
ASSETS:
1. BP_Gun — Actor — mesh, fire/reload functions, ammo tracking, muzzle flash
2. BP_Bullet — Actor — projectile, damage on hit, auto-destroy
3. Modify @BP_ThirdPersonCharacter — gun reference, equip socket, fire/reload input, ammo UI variable
```

### "Create an inventory system"
Agent should produce:
```
ASSETS:
1. BPI_Pickupable — Interface — Pickup(Collector), GetDisplayInfo():Struct
2. BP_PickupBase — Actor — implements BPI_Pickupable, collision sphere, mesh, rotate animation
3. BP_HealthPickup — Actor (child of BP_PickupBase) — heals on pickup
4. BP_AmmoPickup — Actor (child of BP_PickupBase) — restores ammo on pickup
5. Modify @BP_ThirdPersonCharacter — inventory array, overlap detection, pickup input
```

### "Create a door and key system"
The stdin example already covers this case directly. The agent sees the exact pattern before making any tool calls.

### "Add double jump to @BP_ThirdPersonCharacter"
This is a single-asset modification. The agent would write:
```
ASSETS:
1. Modify @BP_ThirdPersonCharacter — double jump counter, override Jump input, reset on landing
```
No extra Blueprints needed. The decomposition step takes seconds and correctly identifies this as a single-asset task.

## Implementation Order

1. **Change 2** (blueprint_design_patterns.txt) -- content-only, no build needed, can test immediately via `blueprint.get_template("blueprint_design_patterns")`
2. **Change 3** (cli_blueprint.txt) -- content-only, no build needed
3. **Change 1** (OliveCLIProviderBase.cpp) -- C++ change, requires build. This is the critical change that puts the directive in the imperative channel.
4. **Test** -- run "create a bow and arrow system for @BP_ThirdPersonCharacter" and verify the agent outputs an ASSETS list containing BP_Bow before making any tool calls.

## Risk Assessment

**Low risk of over-constraining:** The directive only requires listing assets, not a specific number. Single-asset tasks produce a one-line list and proceed immediately. The decomposition step adds maybe 5-10 seconds of agent thinking time.

**Low risk of format rigidity:** The ASSETS format is simple text, not parsed by any code. The agent can deviate from the exact format and still benefit from the thinking step. What matters is that it THINKS about entities before searching templates.

**Medium risk of agent ignoring it:** stdin directives are the most reliable channel, but not 100%. If the agent still skips decomposition, the next escalation would be a two-phase approach (first turn = decomposition only, capped at `--max-turns 1`). But that is significantly more complex and should only be attempted if this approach fails.

## What This Does NOT Change

- The execution pipeline (plan_json resolve/validate/execute) -- untouched
- Template availability and catalog -- templates remain as tools, agent can still use them
- How tools work -- no MCP or tool registry changes
- AI freedom for HOW to build -- the agent still chooses plan_json vs granular vs Python
- Continuation prompts -- only the initial message gets the decomposition directive
- The orchestrated (non-autonomous) mode -- this only affects `SendMessageAutonomous()`
