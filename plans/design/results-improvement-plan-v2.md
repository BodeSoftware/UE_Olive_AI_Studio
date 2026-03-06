# Olive AI Studio — Results Improvement Plan v2

Synthesized from: 4 researcher reports (NeoStack, Aura, competitors, best practices), 1 codebase audit, and creative lead analysis. March 5, 2026.

---

## Executive Summary

Our competitive position is stronger than initially assumed. NeoStack's "500+ checks" are crash guards, not semantic validation — our write pipeline and Phase 0 validation are more sophisticated. Aura's Telos 2.0 claims ">99% accuracy" but independent testing shows manual cleanup is still needed. Our plan-JSON + library template system has no competitor equivalent.

**Where we're behind:** Asset type breadth (no Materials, Niagara, Sequencer). Context injection for autonomous agents (missing knowledge packs). Completion reliability (agent sometimes stops after scaffolding).

**Where we're ahead:** Plan validation pipeline, self-correction policy, library templates (325+), multi-provider support, snapshot/rollback.

**The biggest ROI improvements are prompt/context changes, not code changes.** The agent already has good tools — it just doesn't always have the right guidance loaded.

---

## Priority 1: Fix Knowledge Pack Gaps (HIGH impact, LOW effort)

### Problem

The codebase audit revealed a critical asymmetry:

| Knowledge Pack | Orchestrated (chat panel) | Autonomous (Claude Code) | CLI System Prompt |
|---------------|--------------------------|-------------------------|-------------------|
| `cli_blueprint.txt` | Via profile packs | Loaded in sandbox | Via `GetKnowledgePackById` |
| `recipe_routing.txt` | Via profile packs | Loaded in sandbox | Via `GetKnowledgePackById` |
| `blueprint_design_patterns.txt` | Via profile packs | Loaded in sandbox | NOT loaded |
| `events_vs_functions.txt` | Via profile packs | **NOT loaded** | **NOT loaded** |
| `node_routing.txt` | Via profile packs | **NOT loaded** | **NOT loaded** |
| `blueprint_authoring.txt` | Via profile packs | NOT loaded | Explicitly skipped |

The autonomous agent (Claude Code, Codex) is missing `events_vs_functions.txt` and `node_routing.txt`. This means:
- No guidance on when to use events vs functions (Timeline constraint)
- No guidance on mixing plan_json + add_node + editor.run_python
- No awareness of `get_node_pins` for property-changed pin reconstruction

The CLI system prompt path (`BuildCLISystemPrompt`) is also missing `blueprint_design_patterns.txt`, `events_vs_functions.txt`, and `node_routing.txt`.

### Fix

**A. Add missing packs to `SetupAutonomousSandbox()`** in `OliveCLIProviderBase.cpp`:
- Load `events_vs_functions.txt` (+~300 tokens)
- Load `node_routing.txt` (+~250 tokens)

**B. Add missing packs to `BuildCLISystemPrompt()`**:
- Load `blueprint_design_patterns.txt` via `GetKnowledgePackById`
- Load `events_vs_functions.txt` via `GetKnowledgePackById`
- Load `node_routing.txt` via `GetKnowledgePackById`

**Token cost:** +550 tokens to autonomous sandbox, +800 tokens to CLI system prompt.

**Why it matters:** Prevents a class of silent failures (Timeline in function graph, wrong event/function choice). The Phase 5.5 pre-compile check catches some of these at execution time, but the agent should know BEFORE planning — prevention > recovery.

---

## Priority 2: Strengthen Completion Directive (HIGH impact, LOW effort)

### Problem

The current stdin ending is:
```
After listing assets, research patterns with blueprint.list_templates(query="..."),
then build each asset fully before starting the next.
```

This is suggestive, not imperative. The agent sometimes stops after creating structure (components, variables, function stubs) without writing graph logic.

The sandbox `AGENTS.md` has better completion rules (lines 362-364):
```
- Complete the FULL task: create structures, wire graph logic, compile, and verify.
- After each compile pass, ask yourself: 'Have I built everything the user asked for?'
- Before finishing, verify you built EVERY part the user asked for
```

But these are in the sandbox reference (lower attention weight), not in stdin (highest attention weight).

### Fix

Replace the decomposition block ending with a stronger imperative at the END of stdin (highest attention position):

```
Build the COMPLETE system. For each Blueprint:
1. Create structure (components, variables, functions)
2. Write ALL graph logic with apply_plan_json for every function
3. Compile to 0 errors
Do not stop until every asset is fully built, wired, and compiled.
```

**Token cost:** Net neutral (~40 tokens, replaces existing text of similar length).

---

## Priority 3: Latent-in-Function-Graph Validation (HIGH impact, MEDIUM effort)

### Problem

The Phase 0 validator already has `CheckLatentInFunctionGraph` (lines 357-406 in OlivePlanValidator.cpp), which catches `delay` ops and resolved latent functions in function graphs. This is good.

However, the resolver currently does NOT flag `bIsLatent` on all latent-capable ops. The validator check depends on `FOliveResolvedStep.bIsLatent` being set correctly during resolution.

### Verify

Confirm that `bIsLatent` is set for:
- `delay` op (should be set by resolver)
- `call` ops that resolve to latent functions (UFunction with `FUNC_BlueprintCosmetic`... actually `FUNC_Latent` flag — `HasAnyFunctionFlags(FUNC_Latent)`)
- Any other latent patterns

If `bIsLatent` is already comprehensive, this is already fixed. If not, extend the resolver to set it.

**Token cost:** Zero (C++ change, no prompt changes).

---

## Priority 4: Richer Self-Correction Error Messages (HIGH impact, MEDIUM effort)

### Problem

The self-correction policy already has 30+ domain-specific guidance strings (per the codebase audit). But some common failure patterns could have MORE specific suggestions:

**Current gap examples:**
1. When `PLAN_RESOLVE_FAILED` fires because a function belongs to a component class not on the Blueprint, the error says "check plan_json syntax" — it should say "Function X belongs to ComponentClass Y. This Blueprint has components: [list]. Wire a get_var for the component to the Target input."
2. When `DATA_WIRE_INCOMPATIBLE` fires for a Vector→Float mismatch, the guidance mentions break_struct but doesn't mention that SplitPin is available as a fallback.

### Fix

Enhance `BuildToolErrorMessage()` in `OliveSelfCorrectionPolicy.cpp` with:
- For `PLAN_RESOLVE_FAILED` + component function: include the Blueprint's SCS component list
- For `DATA_WIRE_INCOMPATIBLE`: mention SplitPin fallback explicitly
- For `COMPILE_FAILED`: include the specific node and pin that errored (already partially done, verify completeness)

**Token cost:** +50-100 tokens per error (only on failure paths — doesn't affect successful runs).

---

## Priority 5: Deduplicate Cross-Layer Guidance (MEDIUM impact, LOW effort)

### Problem

Template discovery guidance appears in both `cli_blueprint.txt` (lines 76-81) and `recipe_routing.txt`. The "three tool approaches" appear in both `cli_blueprint.txt` (lines 5-11) and `node_routing.txt` (lines 3-8). Multi-asset guidance appears in `cli_blueprint.txt`, `blueprint_design_patterns.txt`, AND the stdin directive.

Each repetition wastes tokens and risks subtle contradictions.

### Fix

**A. Merge `recipe_routing.txt` unique content into `cli_blueprint.txt`:**
- The only unique content in `recipe_routing.txt` is the `olive.get_recipe` usage guidance and quality-level descriptions
- Add 3-4 lines to `cli_blueprint.txt` Templates section about recipes
- Remove `recipe_routing.txt` from sandbox loading

**B. Trim `node_routing.txt` overlap:**
- Remove the "Three approaches" intro (lines 3-8) — already in `cli_blueprint.txt`
- Keep only the unique content: `get_node_pins` usage, mixing approaches, "your UE5 knowledge is valid"

**C. Trim stdin decomposition block:**
- Remove template research instruction from stdin (it's in the knowledge pack)
- Keep only: asset listing directive + completion imperative

**Token savings:** ~200-300 tokens net reduction.

---

## Priority 6: Pre-populate Related Asset Context (MEDIUM impact, MEDIUM effort)

### Problem

When a user says "create a gun system for @BP_ThirdPersonCharacter", we inject the character's state but the agent has to call `project.search` 1-2 times to discover if BP_Gun, BP_Weapon, or BP_Projectile already exist.

NeoStack's `@mention` auto-discovery and Aura's "context objects are global across all threads" both solve this by proactively surfacing related assets.

### Fix

In `SendMessageAutonomous()`, after resolving @-mentioned assets:
1. Extract keywords from the user message (simple tokenizer — no LLM call needed)
2. Run `FOliveProjectIndex` search for assets matching those keywords
3. Append a brief summary: `"Existing assets that may be relevant: /Game/BP_Gun (Actor, 3 functions), /Game/BP_Bullet (Actor, 1 function)."`
4. Use conservative matching — only surface assets whose names contain keywords from the message

**Token cost:** +100-200 tokens per run (depends on matches).
**Saves:** 1-2 tool calls per run (~10-20 seconds of agent time).

---

## Priority 7: Verify-Before-Done for Complex Functions (LOW impact, LOW effort)

### Problem

The agent applies plan_json, sees "success", compiles (success), and moves on. But "success" means nodes were created and compilation passed — NOT that the logic is correct. Disconnected data wires don't always cause compile errors.

### Fix

Add one line to `cli_blueprint.txt` Rules section:
```
- After applying plan_json to a complex function (5+ steps), read the function back with blueprint.read_function to verify the graph looks correct. Disconnected data wires don't always cause compile errors.
```

**Token cost:** +30 tokens.

---

## Priority 8: Tool Batching Guidance (LOW impact, LOW effort)

### Problem

The agent sends independent tool calls sequentially (e.g., 6 `add_variable` calls one at a time). The MCP server handles parallel calls. This wastes agent thinking time between calls.

Note: NeoStack research confirms Claude Code naturally batches well when it has context. The issue is more pronounced with non-Claude providers.

### Fix

Add to sandbox Critical Rules:
```
- Batch independent tool calls (add_variable, add_component) in a single response when possible.
```

**Token cost:** +15 tokens.

---

## NOT Doing (Evaluated and Rejected)

### Tool Consolidation (98 → ~15 tools)
NeoStack consolidated 27→15. We have 98 tools. However:
- Claude Code lazy-loads tool schemas (95% reduction when >10% of context)
- Our domain-prefixed naming (`blueprint.X`, `bt.X`, `pcg.X`) provides semantic clarity
- Our focus profiles already filter tool visibility
- Consolidation would require major refactoring with unclear benefit given lazy loading

**Decision:** Not now. Monitor if tool count causes accuracy issues.

### Blueprint Diff Review UI (Aura pattern)
Aura shows green/red node diffs before confirming. Our `preview_plan_json` shows a description, not a visual diff. Adding a visual diff requires Slate widget work in the chat panel — significant effort for a UX improvement, not a results improvement.

**Decision:** Future UX feature, not in this plan.

### Plan Mode with Markdown Export (Aura/Cursor pattern)
Both Aura and Cursor externalize plans as editable markdown. Interesting for user trust but doesn't improve agent results directly.

**Decision:** Future feature, not in this plan.

### ACP Transport (NeoStack pattern)
NeoStack's ACP (stdio subprocess) path gives better results than MCP because Claude Code runs natively. Our MCP bridge approach adds latency. However, our bridge works reliably and changing transport is high risk.

**Decision:** Evaluate later if latency becomes a measurable problem.

---

## Implementation Order

```
Phase A (prompt changes only — can ship same day):
  1. Fix knowledge pack gaps (Priority 1A: autonomous sandbox)
  2. Strengthen completion directive (Priority 2)
  5. Deduplicate cross-layer guidance (Priority 5)
  7. Verify-before-done guidance (Priority 7)
  8. Batching guidance (Priority 8)

Phase B (code changes — 1-2 days):
  1B. Fix knowledge pack gaps (Priority 1B: CLI system prompt path)
  3. Verify latent-in-function validation (Priority 3)
  4. Richer self-correction messages (Priority 4)

Phase C (feature work — 2-3 days):
  6. Pre-populate related asset context (Priority 6)
```

---

## Verification Plan

### Test Prompt 1: "create a door that opens smoothly when the player presses E"
**Tests:** events_vs_functions knowledge (Timeline must be in EventGraph, not function)
**Success:** Door uses Timeline in EventGraph; no latent-in-function errors

### Test Prompt 2: "create a bow and arrow system for @BP_ThirdPersonCharacter"
**Tests:** Multi-asset decomposition, completion, template usage
**Success:** 3+ Blueprints created, ALL with graph logic (not just stubs), all compiled

### Test Prompt 3: "add a double jump to @BP_ThirdPersonCharacter"
**Tests:** MODIFY workflow, existing asset handling
**Success:** No new Blueprint created, modifies existing, compiles clean

### Test Prompt 4: "make me a gun" (deliberately ambiguous)
**Tests:** Decomposition with ambiguity, completion
**Success:** Creates BP_Gun + BP_Projectile minimum, full logic, compiled

### Test Prompt 5: "create a health system with a damage interface"
**Tests:** Interface events vs functions decision, cross-Blueprint communication
**Success:** Interface with no-output Interact (event, not function), health component, compiled

---

## Competitive Positioning Summary

| Capability | Olive (current) | Olive (after this plan) | NeoStack AIK | Aura |
|-----------|----------------|------------------------|-------------|------|
| Blueprint quality | Good | Better | Good (Claude-dependent) | Claims ">99%" (unverified) |
| Pre-execution validation | Phase 0 (3 checks) | Phase 0 + latent check | Crash guards only | Unknown |
| Self-correction | 3-tier + progressive | 3-tier + richer messages | None (native agent) | Unknown |
| Context injection | Partial (3/6 packs) | Full (6/6 packs) | Profile instructions | Cloud-side |
| Template system | 325+ library templates | Same | None | None |
| Completion reliability | Sometimes stops early | Stronger directive | "One-shot" prompting | Unknown |
| Asset breadth | BP, BT, PCG, C++, Python | Same | 20+ types | 15+ types |
| Multi-provider | 8 providers | Same | 5+ agents | Claude only |
