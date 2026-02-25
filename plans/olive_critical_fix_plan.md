# Olive AI — Critical Fix Plan: Plan Rollback, Resolver Intelligence, Loop Detection

**Three fixes that address the systemic failure cascade found in production logs.**

Last updated: Feb 25, 2026
Effort: ~10-14 hours total

---

## Context

A user asked the AI to create a gun + bullet system. The bullet Blueprint failed and could never recover. Three separate problems cascaded into each other:

1. The function resolver matched `SetSpeed` to `WindDirectionalSourceComponent` (a wind effect class) instead of `ProjectileMovementComponent` (which was actually on the Blueprint). Confidence was 55% — basically a coin flip.

2. The plan created 6 nodes including the wrong SetSpeed node, then failed to compile. But the nodes were already committed to the graph. When the AI tried to fix its plan on the next attempt, the old zombie nodes were still there causing the same compile error. Replace mode didn't actually replace anything.

3. The self-correction loop detector saw the same compile error three times and killed the run. But the AI wasn't making the same mistake — it had already removed the bad step from its plan. The error was coming from zombie nodes left by the first attempt.

The AI's instincts were correct. It identified the problem and tried to fix it. The system blocked the fix at every turn.

These three fixes work together. Any one of them alone would have prevented total failure in this session, but all three are needed to handle the full range of future cases.

---

## Fix 1: Plan Rollback on Failure

### The Problem

When `apply_plan_json` executes, it creates nodes in Phase 1, wires them in Phases 3-4, sets defaults in Phase 5, then compiles in Stage 5 of the write pipeline. If compilation fails, the nodes are already committed to the graph. They stay there permanently.

This means:

- Failed plans leave zombie nodes in the graph
- Those zombies can have unwired pins that cause compile errors
- Subsequent plans that reuse event nodes (like BeginPlay) can't rewire them because the old connections are still attached
- Each retry adds MORE nodes on top of the existing mess
- The graph gets progressively more polluted with each correction attempt

Replace mode makes this worse, not better. When the AI says `mode: "replace"`, it expects a clean slate. What it actually gets is "add new nodes, reuse existing event nodes with their old connections intact, leave all previous nodes in place."

In the log, the AI's second attempt was a clean 3-step plan (BeginPlay → GetLifeSpan → SetLifeSpan). This plan is correct. But it failed because:
- BeginPlay's exec output was still wired to the old SetSpeed node from attempt 1
- The executor couldn't connect BeginPlay → SetLifeSpan ("Cannot connect pins: Replace existing output connections")
- The old SetSpeed node was still in the graph with an unwired Target pin
- Same compile error as before, even though the AI's new plan was correct

### The Fix

**When a plan fails at any phase after node creation (Phases 3-6, or compile), roll back the nodes that were created by this plan execution.**

The plan executor already tracks which nodes it created in Phase 1 — it logs "Created step 'X' → node 'node_N'" for each one. Use that tracking to remove those nodes if the overall execution fails.

Rollback means:
- Remove all nodes created by this plan execution (not pre-existing nodes like reused event nodes)
- Break any connections that were made to those nodes during Phases 3-4
- Leave the graph in the same state it was in before this plan ran

This way, when the AI retries, it's working with a clean graph — not a graph polluted by the previous attempt's debris.

**For replace mode specifically:** When mode is "replace" and the plan targets a specific graph (like EventGraph), the executor should clear all non-event, non-function-entry nodes from that graph before creating new ones. This is what "replace" should have always meant — start fresh.

### Why This Matters

This single fix would have made the bullet Blueprint succeed. The AI's second attempt had the right plan. If the first attempt's nodes had been rolled back, the second attempt would have found a clean BeginPlay node with no existing connections, wired it to SetLifeSpan, and compiled successfully.

More broadly, this makes self-correction actually work. Right now, every failed attempt makes the graph harder to fix. With rollback, every failed attempt is invisible — the AI always retries against a clean state. The graph only accumulates nodes from successful plans.

### Edge Cases

**What about nodes the AI created via separate tool calls before the plan?** Things like `add_component` and `add_variable` are separate operations that succeed independently. They should NOT be rolled back when a subsequent plan fails. Only nodes created within the plan execution itself get rolled back.

**What if Phase 3 (exec wiring) partially succeeds?** Some connections made, some failed. Roll back everything — nodes AND connections. A partially wired plan is worse than no plan because it creates confusing graph state for the next attempt.

**What about replace mode on a graph that has manually-placed nodes the user wants to keep?** Replace mode is inherently destructive. The AI chose it deliberately. If there's concern about data loss, the preview_plan_json flow (which exists but is optional) is the right safeguard — not silently failing to actually replace.

**What about event nodes that were reused, not created?** Don't delete reused event nodes during rollback — they existed before the plan. But DO disconnect any new connections that were made to them during this plan's execution. The event node stays, but its wiring returns to pre-plan state.

### Scope

Files affected:
- `OlivePlanExecutor.cpp` — Track created nodes, implement rollback on failure
- `OliveGraphWriter.cpp` — May need a RemoveNode/DisconnectNode utility if one doesn't already exist
- `OliveBlueprintToolHandlers.cpp` — Replace mode pre-cleanup logic

Effort: ~4-5 hours

---

## Fix 2: Blueprint-Aware Function Resolver

### The Problem

When the AI writes `"target": "SetSpeed"` without specifying a target class, the function resolver builds a search order and tries to find a function with that name. The current search order is:

1. Explicit target class (if the AI specified one in `target_class`)
2. Blueprint parent class hierarchy (Actor → Object)
3. Common library classes (KismetSystemLibrary, KismetMathLibrary, GameplayStatics, etc.)
4. BroadSearch (iterates every class in the engine, returns first match)

The problem: **the Blueprint's own component classes are not in the search order.**

BP_Bullet has a `ProjectileMovementComponent` on it. `UProjectileMovementComponent` has velocity/speed-related functions. But the resolver never looks at that class. It skips straight from common libraries to BroadSearch, which iterates every class in the engine and returns the first match — `WindDirectionalSourceComponent::SetSpeed` at confidence 55.

A developer looking at BP_Bullet would immediately know `SetSpeed` should target the projectile component. The resolver is completely blind to what's on the Blueprint.

The second part of the problem: **BroadSearch accepts any match regardless of relevance.** A confidence-55 match from a wind effect class on a bullet Blueprint should not be silently accepted. BroadSearch doesn't consider whether the matched class has any relationship to the Blueprint it's resolving for.

### The Fix

**Part A: Add the Blueprint's component classes to the search order.**

After searching the parent class hierarchy and before searching common libraries, scan the Blueprint's Simple Construction Script (SCS) and CDO for components. Extract each component's class. Add those classes to the search order.

The new search order becomes:

1. Explicit target class (if AI specified one)
2. Blueprint parent class hierarchy (Actor → Object)
3. **Component classes on this Blueprint** (ProjectileMovementComponent, SphereComponent, StaticMeshComponent, etc.)
4. Common library classes (KismetSystemLibrary, GameplayStatics, etc.)
5. BroadSearch (with improved scoring)

With this change, `SetSpeed` is found on `UProjectileMovementComponent` in step 3, at confidence 100 (exact match on a class that's actually on the Blueprint). BroadSearch never runs.

This is the same information the `COMPONENT_FUNCTION_ON_ACTOR` validator already uses — it already scans for component classes to check if Target is wired. The resolver just needs to use the same information for function lookup.

**Part B: Relevance-aware scoring in BroadSearch.**

When BroadSearch does run (because nothing matched in steps 1-4), it currently assigns confidence 50 for function library matches and 55 for gameplay class matches, regardless of whether the class is related to the Blueprint.

Change BroadSearch scoring to account for relevance:

- Function found on a component class that IS on this Blueprint → 90 (this is essentially a component match that step 3 should have caught, but handles inheritance edge cases)
- Function found on a Blueprint Function Library → 70 (libraries are designed to be called from anywhere, this is a reasonable match)
- Function found on a gameplay class (Actor, Component, etc.) that is NOT on this Blueprint → 40 (probably wrong — this class has nothing to do with this Blueprint)

Then add a minimum acceptance threshold of ~60. Matches below 60 are rejected — the resolver returns failure with the low-confidence candidates listed as suggestions. The AI gets: "SetSpeed not found. Did you mean WindDirectionalSourceComponent::SetSpeed (40%)?" — and knows that's wrong.

### Why This Scoring Works

Library functions like `Delay` (KismetSystemLibrary) score 70 and pass the threshold. They're meant to be called from any Blueprint.

Random matches like `WindDirectionalSourceComponent::SetSpeed` score 40 and get rejected. The AI gets a clear "not found" error instead of a silently wrong resolution.

Component matches that somehow got missed by step 3 score 90 and pass easily.

### Edge Cases

**Two components on the Blueprint both have a function with the same name.** Example: BP has both a StaticMeshComponent and SkeletalMeshComponent, both have `SetMaterial`. First one in the search order wins. This isn't perfect, but it's still correct — both are valid targets, and the AI's Target input disambiguates at wiring time.

**Function is on a parent class of the component, not the component itself.** `SetRelativeLocation` is on `USceneComponent`, not directly on `UStaticMeshComponent`. No problem — `FindFunctionByName` walks the inheritance chain automatically. Searching `StaticMeshComponent` finds `SceneComponent` functions too.

**AI hallucinated a function that doesn't exist anywhere.** Falls through all steps, BroadSearch finds nothing (or finds only low-confidence irrelevant matches), resolver returns failure with suggestions. AI gets a clean error and can self-correct. Far better than silently accepting a wrong match.

**Blueprint has no components (e.g., a data-only Blueprint or function library).** Step 3 finds nothing, search continues to steps 4-5. No change in behavior for Blueprints without components.

**AI provides an explicit target_class that's wrong.** Step 1 searches the wrong class, finds nothing. Steps 2-5 continue. If the function exists on a component, step 3 catches it. If not, it falls through normally. The explicit target_class is a hint, not a hard constraint (for steps 2+).

### Additional Context: Target Input Hints

There's a further improvement possible but not required for this fix. The AI sometimes provides Target inputs that hint at which component it intends to call the function on. In the log, the BP_Gun plan had `"Target": "MuzzlePoint"` — an ArrowComponent. If the resolver could look at the Target input and extract the referenced component's class, it could search that class first.

This is more complex to implement (requires resolving @ref chains or looking up component names) and is not needed for the basic fix. The component-aware search order handles the common case. But it's worth noting as a future enhancement that would make the resolver even smarter.

### Scope

Files affected:
- `OliveFunctionResolver.cpp` — GetSearchOrder (add component class extraction), BroadSearch (relevance-aware scoring, minimum threshold)
- Possibly `OliveBlueprintReader.cpp` or equivalent — may need a utility to extract component classes from a Blueprint's SCS

Effort: ~3-4 hours

---

## Fix 3: Smarter Loop Detection

### The Problem

The self-correction loop detector works by building an error signature from the compile error message and counting how many times it sees the same signature. After N attempts (currently 3) with the same signature, it stops the run.

In the log, the compile error was: *"This blueprint (self) is not a WindDirectionalSourceComponent, therefore 'Target' must have a connection."*

This error appeared 3 times. The loop detector correctly identified it as a repeated error and stopped. But the AI's second and third attempts didn't contain the step causing the error. The AI had already removed `SetSpeed` from its plan. The error was coming from a zombie node left by the first attempt — not from anything the AI did wrong on retries.

The loop detector can't distinguish between:

- **"Same mistake repeated"** — the AI keeps submitting the same bad plan and getting the same error. Correct to stop.
- **"Stale error from previous commit"** — a zombie node from a previous failed plan keeps causing the same error, even though the AI's new plan is different. Wrong to stop.

### The Fix

**After a plan fails with compile errors, compare the current plan's steps against the error to determine if the error is "owned" by this plan or "stale" from a previous one.**

The logic:

1. When a compile error occurs, extract the node or function name from the error message. In this case, the error mentions "WindDirectionalSourceComponent" and "Target" — which corresponds to the `SetSpeed` call node.

2. Check if any step in the current plan resolves to the function or node mentioned in the error. Look at the resolved function names and target classes.

3. If the error maps to a step in the current plan → this is a real repeated mistake. Count it toward the loop detector's limit.

4. If the error does NOT map to any step in the current plan → this is a stale error from a previous plan's committed nodes. Don't count it as a repeat. Instead, inject a different correction message telling the AI that there's a leftover node causing problems, and suggest using `blueprint.remove_node` to clean it up, or that the system will handle it (if Fix 1's rollback is in place).

### Why This Distinction Matters

With Fix 1 (plan rollback), stale errors become much less common — failed plans clean up after themselves. But they can still happen in edge cases:

- The AI used granular tools (add_node + connect_pins) before a plan, and those nodes are causing issues
- A previous plan partially succeeded (some steps compiled fine, some didn't) and rollback only removed the failing plan's nodes
- The Blueprint had pre-existing issues before the AI started working on it

In these cases, the loop detector needs to know that the AI isn't making the same mistake — the error is environmental, not behavioral.

### Implementation Approach

**Step 1: Enrich the compile error with node identity.**

The Phase 5.5 pre-compile validation already detects issues like "Unwired component Target on 'Set Speed (step: unknown)'" — it even says "step: unknown" because the node isn't in the current plan's step map. This "step: unknown" signal already exists but isn't used by the self-correction system.

When the plan executor reports warnings from Phase 5.5, include the step ownership info. A warning tagged with "step: unknown" means it's a zombie node. A warning tagged with a step ID (like "step: set_speed") means it's owned by the current plan.

**Step 2: Pass ownership info to the self-correction policy.**

When `FOliveSelfCorrectionPolicy::Evaluate` processes a compile failure, it should check if the error is associated with a known step from the current plan. If the plan result includes "step: unknown" warnings that match the compile error, classify this as a stale error.

**Step 3: Handle stale errors differently.**

For stale errors:
- Don't increment the per-signature retry counter
- Inject a correction message that explains the situation: "The compile error is caused by a leftover node from a previous plan, not by your current plan. The old node 'SetSpeed (WindDirectionalSourceComponent)' has an unwired Target pin. Use blueprint.remove_node to remove it, or resubmit your plan with mode: 'replace' to clear the graph."
- If Fix 1 (rollback) is active, this situation should be rare — but the message gives the AI a manual escape route when it does occur

For real repeated mistakes:
- Same behavior as today — increment counter, stop after N attempts

### Edge Cases

**The error message doesn't clearly identify which node caused it.** Some compile errors are vague ("Accessing None pointer"). In this case, we can't determine ownership. Default to current behavior (count it as a potential repeat). This is conservative but safe.

**The AI's plan IS causing the error, but it's a different step than last time.** Example: first plan fails on step A, second plan fixes step A but introduces a new error on step B. Both errors are different, so the loop detector treats them as separate signatures. This already works correctly.

**Both stale errors AND new errors from the current plan.** Multiple compile errors, some from zombie nodes, some from the current plan. Count the ones owned by the current plan toward the loop limit. Report the stale ones separately with cleanup guidance.

**The Blueprint was broken before the AI started.** If the user's Blueprint already had compile errors, every plan will "fail" with those pre-existing errors. The loop detector should ideally baseline the Blueprint's compile state before the first plan attempt (record any pre-existing errors). Errors that existed before the AI touched anything should never count toward the loop limit. This is an enhancement beyond the core fix but worth considering.

### Scope

Files affected:
- `OlivePlanExecutor.cpp` — Tag Phase 5.5 warnings with step ownership (already partially done with "step: unknown" vs step ID)
- `OliveSelfCorrectionPolicy.cpp` — Check step ownership when classifying compile errors as stale vs repeated
- `OliveBlueprintToolHandlers.cpp` — Pass Phase 5.5 ownership info through to the tool result

Effort: ~3-5 hours

---

## Implementation Order

| Order | Fix | Effort | Impact |
|-------|-----|--------|--------|
| 1 | Plan Rollback on Failure | ~4-5 hours | Eliminates zombie nodes entirely. Makes self-correction actually work. Highest impact single fix. |
| 2 | Blueprint-Aware Resolver | ~3-4 hours | Prevents wrong function matches. The SetSpeed→Wind problem never happens. |
| 3 | Smarter Loop Detection | ~3-5 hours | Prevents false-positive loop kills when stale errors exist. Safety net for edge cases Fix 1 doesn't cover. |

**Rationale for this order:**

Fix 1 (rollback) alone would have made the log session succeed. The AI's second plan was correct — it just needed a clean graph to work with. This is the highest-impact fix and should go first.

Fix 2 (resolver) prevents the wrong match that started the cascade. Even without rollback, the AI would have gotten a clean "SetSpeed not found" error instead of a silently wrong match. It would have self-corrected by searching for the right function name. This is the root cause fix.

Fix 3 (loop detection) is the safety net. With Fix 1 in place, stale errors become rare. With Fix 2 in place, the specific SetSpeed→Wind problem doesn't happen. But there will be other edge cases where stale errors or environmental issues trigger the loop detector incorrectly. Fix 3 handles those.

Together, these three fixes address the cascade at every level: prevent the wrong resolution (Fix 2), clean up when plans fail anyway (Fix 1), and don't punish the AI for environmental problems (Fix 3).

---

## Verification

### The Bullet Test (reproduces the exact log failure)

| Step | Without fixes | With fixes |
|------|--------------|------------|
| AI calls SetSpeed on BP_Bullet | Resolves to WindDirectionalSourceComponent (wrong) | Resolves to ProjectileMovementComponent (correct via component search) — OR fails clearly if SetSpeed doesn't exist on PMC |
| Plan creates nodes and fails compile | 6 zombie nodes left in graph | Nodes rolled back, graph is clean |
| AI retries with simpler plan | Can't rewire BeginPlay (old connection stuck), same compile error | Clean graph, BeginPlay is free, plan succeeds |
| Loop detector | Kills run after 3 identical errors | Recognizes stale error, doesn't count it |

### The Edge Case Tests

| Scenario | Expected behavior |
|----------|------------------|
| AI calls `Delay` (library function, no component involved) | Found on KismetSystemLibrary at confidence 70, accepted |
| AI calls `SetSpeed` on BP with no ProjectileMovement | Not found on any component, BroadSearch finds Wind at 40, rejected. Clean error with suggestion. |
| AI calls `SetMaterial` on BP with both Static and Skeletal mesh | Found on first component in search order, accepted. Target input disambiguates at wiring. |
| Plan fails in Phase 3 (exec wiring) | All nodes from this plan rolled back, graph returns to pre-plan state |
| Plan partially succeeds then fails compile | All nodes from this plan rolled back, even the ones that wired correctly |
| Blueprint had pre-existing compile errors before AI started | Pre-existing errors don't count toward loop detector limit |
| AI uses granular tools (add_node) that create bad nodes, then plan fails from those | Granular tool nodes are NOT rolled back (they're separate operations). Loop detector recognizes compile errors from those nodes as environmental. |
