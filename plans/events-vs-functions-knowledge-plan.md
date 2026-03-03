# Plan: Events vs Functions Decision Knowledge

## Problem

The AI defaults to functions with return values when events would be the better choice. This has cascading consequences:

1. Interface functions with return values (e.g., `Interact():Bool`) create synchronous function graphs
2. Synchronous function graphs cannot use Timelines, Delays, or any latent/async nodes
3. The AI is then forced into instant operations (SetRelativeRotation) instead of smooth patterns (Timeline + Lerp)
4. When prompted to add smooth movement later, the AI can't modify the existing function graph — it creates a separate conflicting system in EventGraph

This has been observed in 3+ consecutive test sessions. Prompt changes to examples and notes have not changed the AI's behavior. The AI has a strong training prior that interface functions "should" return a success boolean.

## Root Cause

The AI lacks a clear decision framework for when to use events vs functions, both in general Blueprint design and specifically for interfaces. It doesn't understand the practical consequences of the choice — particularly that function graphs are synchronous-only.

## Solution: Knowledge Note

Add a knowledge note to `Content/SystemPrompts/Knowledge/blueprint_authoring.txt` (or a new dedicated file) that covers three areas:

### 1. General: Events vs Functions

```
## When to Use Events vs Functions

**Use a Function when:**
- You need to return a value to the caller
- The logic is synchronous (runs and completes in one frame)
- Examples: GetHealth(), CalculateDamage(), IsAlive(), CanInteract()

**Use an Event when:**
- The logic needs Timelines, Delays, or any async/latent behavior
- The logic is fire-and-forget (caller doesn't need a return value)
- The logic may span multiple frames (animations, interpolation, sequences)
- Examples: Interact(), OnDeath(), OpenDoor(), StartAbility()

**Key constraint:** Function graphs are synchronous — they cannot contain Timeline nodes, Delay nodes, or any latent action. If your implementation needs any of these, it must be an event or called from an event.
```

### 2. Interface: Events vs Functions

```
## Interface Events vs Interface Functions

When defining functions in a Blueprint Interface:

**No outputs → Implementable Event**
- Implementation lives in the EventGraph
- Can use Timelines, Delays, async patterns
- Caller fires and forgets
- Use for: interactions, triggers, gameplay callbacks, anything that might animate or interpolate

**Has outputs → Implementable Function**
- Implementation lives in a synchronous function graph
- CANNOT use Timelines, Delays, or latent nodes
- Caller waits for the return value
- Use for: queries, checks, data retrieval

**Common mistake:** Adding a bool Success output to Interact(). This forces a synchronous function graph, preventing smooth door opening, animation playback, or any multi-frame behavior. If you don't need the return value, remove the outputs to make it an event.
```

### 3. The Hybrid Pattern: Function + Event

```
## Hybrid Pattern: Interface Function That Needs Async Behavior

If an interface function genuinely needs BOTH a return value AND async behavior, use this pattern:

1. Interface function returns immediately with a synchronous result (e.g., success/failure)
2. Interface function calls a Custom Event on self
3. The Custom Event handles the async work (Timeline, Delay, interpolation)

Example — Interact():Bool with smooth door movement:

**Interface function (Interact):**
  - Check if door can be toggled → if not, return false
  - Call Custom Event "DoOpenClose" 
  - Return true

**Custom Event (DoOpenClose) in EventGraph:**
  - Timeline → Lerp → SetRelativeRotation
  - Handles the actual smooth animation over multiple frames

This way the caller gets an immediate bool response, and the smooth movement runs independently in the EventGraph.

**When to use this pattern:**
- The caller needs to know if the action was accepted (return bool)
- The actual behavior needs Timelines or Delays
- Examples: Interact() that returns whether interaction started, TakeDamage() that returns actual damage dealt but plays async hit reaction

**When NOT to use this pattern:**
- If the caller doesn't need the return value → just make it an event (simpler)
- If the logic is purely synchronous → just use a function (simpler)
```

## Implementation

### File Changes

**Option A: Add to existing `blueprint_authoring.txt`**
- Append the three sections above
- Pro: AI already reads this file
- Con: File gets longer

**Option B: New file `Content/SystemPrompts/Knowledge/events_vs_functions.txt`**
- Dedicated knowledge file
- Add to recipe routing so it gets loaded for Blueprint tasks
- Pro: Clean separation, easy to find and update
- Con: Need to wire it into the knowledge loading

**Recommendation:** Option B — create a dedicated file. This is important enough to be its own knowledge topic, and it keeps `blueprint_authoring.txt` focused. Wire it into recipe routing for any Blueprint creation or interface task.

### Recipe Routing

In `Content/SystemPrompts/Knowledge/recipe_routing.txt`, add routing for this knowledge:

```
- If the task involves creating interfaces → load events_vs_functions.txt
- If the task involves creating functions or events → load events_vs_functions.txt
- If the task involves Timelines, Delays, or smooth movement → load events_vs_functions.txt
```

### System-Level Reinforcement (Option C from earlier analysis)

In addition to the knowledge note, implement the Phase 5.5 warning upgrade:

When Phase 5.5 detects `Unwired FunctionResult data pin` on an interface implementation function:
- Current: Logs as a generic warning buried in success response
- New: Return as a visible warning in the tool result, with specific guidance:

```
"Warning: Interface function 'Interact' has unwired return value 'bSuccess' (bool). 
If you don't need a return value, removing outputs from the interface makes this an 
implementable event — which supports Timelines, Delays, and smooth movement patterns. 
If you do need the return value, consider the hybrid pattern: function returns immediately, 
then calls a Custom Event for async work."
```

This fires after the fact but gives the AI the information to self-correct on the next iteration.

## What This Does NOT Do

- Does not block or restrict the AI from adding return values to interfaces
- Does not auto-strip outputs from interface functions
- Does not force any specific pattern
- Gives the AI the knowledge to make the right choice, plus a visible nudge when it makes the wrong one

## Priority

High. This is the root cause of the "no smooth movement" problem that has persisted across 3+ test sessions. It's also cheap — the knowledge note is ~40 lines of text, the Phase 5.5 change is ~10 lines of code.

## Success Criteria

- AI creates Interact() without outputs when smooth movement is likely needed
- AI uses the hybrid pattern when it genuinely needs both return values and async behavior
- Phase 5.5 warning fires and the AI acknowledges it when it adds unnecessary return values
- Smooth movement (Timeline or Tick+Lerp) is used in door/gate implementations without user prompting
