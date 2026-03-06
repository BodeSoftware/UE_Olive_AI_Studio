# Design: AI Agent Results Improvement

Strategic design spec for improving Olive AI Studio's agent output quality, informed by competitor analysis, current system audit, and observed agent behavior.

---

## 1. Competitor Analysis

### 1.1 NeoStack AIK (Agent Integration Kit) by Betide Studio

**Status as of March 2026:** v0.5.6+, shipping updates every 1-3 days. Our primary competitor and the tool we should study most closely.

**Architecture: ACP vs MCP**

NeoStack uses what they call ACP (Agent Communication Protocol), which is their wrapper around standard MCP. The key difference is philosophical, not protocol-level:
- AIK acts as a *middleware layer* between the LLM and UE, with heavy pre-processing. The LLM describes intent; AIK translates to UE operations.
- Olive exposes UE operations directly via MCP tools. The LLM calls specific tools with specific parameters.

AIK's approach is closer to our plan_json philosophy than to our granular tools. Their v0.5.0 consolidation from 27+ tools to 15 "intelligent single tools that figure out what you mean automatically" confirms the direction we were already heading: fewer, smarter tools beat many granular ones.

**Asset Discovery and Context Injection**

AIK pre-populates context aggressively:
- When the user mentions an asset, AIK reads it and injects the full structure into the prompt before the LLM starts thinking.
- They scan the project index for related assets (e.g., if you mention BP_Gun, they also surface BP_Bullet if it exists).
- Their "500+ pre-execution checks" validate the LLM's intent before executing, catching errors that would otherwise consume a retry cycle.

Olive's approach: We inject @-mentioned asset state (good), but we do NOT proactively discover related assets. The agent must call `project.search` to find them. This costs 1-2 tool calls per run that AIK avoids.

**Multi-Asset Task Handling**

AIK does not appear to have an explicit decomposition step. Their tool consolidation means the LLM describes a system in natural language, and AIK's middleware decomposes it internally. This is more opaque but potentially faster for simple cases.

Olive's explicit decomposition directive (the "ASSETS:" list in stdin) forces the LLM to think before acting. Log evidence shows this works: the bow-and-arrow run correctly identified BP_Bow, BP_Arrow, and BP_ThirdPersonCharacter modifications before starting. However, the directive costs ~150 tokens and adds one "thinking" turn before the first tool call.

**Error Recovery**

AIK's pre-execution validation is their primary error prevention. They validate before executing, catching mismatched pin types, missing variables, wrong component targets, etc. Their error recovery when validation passes but execution fails is not publicly documented.

Olive's approach is reactive: execute, detect failure, enrich error message, retry. Our self-correction policy (3-tier classification: FixableMistake, UnsupportedFeature, Ambiguous) is more sophisticated than anything publicly documented from AIK, but we pay the cost of the failed execution + retry cycle.

**Asset Type Breadth**

AIK covers: Blueprints, Materials, Animation (BlendSpace, AnimSequence, Montage, Control Rig, IK Rig), VFX (Niagara), Sequencer, Behavior Trees/State Trees, Enhanced Input, PCG, MetaSounds, Viewport screenshots, Python/C++ execution.

Olive covers: Blueprints (deep), Behavior Trees, PCG, C++, Python, Animation Blueprints (partial), Widget Blueprints. We lack: Materials, Niagara, Sequencer, MetaSounds, IK Rigs, Control Rigs, standard Animation assets (we have AnimBP state machines but not AnimSequence/Montage creation).

**Key Takeaway from NeoStack:** Their philosophy of "intelligent single tools" that resolve intent internally is validated. Our plan_json system is this exact philosophy applied to Blueprint graphs. Where we should learn from them: (1) more aggressive pre-population of context, (2) pre-execution validation to prevent failures rather than recovering from them, (3) broader asset type coverage is a market expectation.

### 1.2 Aura by RamenVR

**Status:** Public beta, credit-based pricing. Supports Claude Sonnet 4.5.

**Architecture:** In-editor chat UI with two modes:
- Agent mode: direct changes to UE assets
- Ask mode: analysis only, no mutations

**Observed Issues:**
- "AI refuses to work on existing blueprints" reported on Epic Forums. This suggests over-conservative safety guards that block legitimate operations.
- Blueprint support is present but reportedly unstable.
- Credit-based pricing creates friction for iteration-heavy workflows.

**What Aura Gets Right:**
- Two-mode separation (agent vs. ask) is a clean UX concept. Olive's approach is always-agent, which is simpler but means the AI sometimes builds when the user just wanted analysis.
- Their focus on Claude Sonnet 4.5 "consistent results" suggests they've tuned their prompts for a specific model rather than trying to be model-agnostic.

**Key Takeaway from Aura:** Their struggles with "AI refuses to work on existing blueprints" is a cautionary tale about over-restrictive guardrails. This validates Olive's "AI freedom over guardrails" philosophy. Keep flexible.

### 1.3 General AI Coding Tools

**Cursor: Context Injection Patterns**
- Cursor indexes the entire codebase and retrieves relevant files automatically based on the user's query. The user never manually tells Cursor which files to read.
- "@-mention" syntax lets users pin specific files/symbols into context.
- Cursor's agent mode does file discovery autonomously, reading files as needed.
- Lesson for Olive: Our @-mention system (e.g., @BP_ThirdPersonCharacter) works similarly but we inject the asset state immediately rather than letting the agent discover it. This is correct for UE (reading a Blueprint is not free -- it requires game thread access and serialization).

**Windsurf (Cascade): Agent Flow**
- Cascade maintains a "plan" that the user can review before execution.
- The plan is visible in the UI, creating transparency about what the AI will do.
- Lesson for Olive: Our preview_plan_json serves a similar function but it's developer-facing (the AI previews, not the user). Consider surfacing the agent's decomposition plan in the UI.

**Copilot Workspace: Task Decomposition**
- Breaks tasks into sub-tasks with explicit dependency ordering.
- Each sub-task has its own verification step.
- Lesson for Olive: Our "build one asset fully, then the next" approach is simpler but effective. Copilot's explicit dependency graph is overkill for typical UE Blueprint tasks (usually 2-4 assets with linear dependencies).

**Aider: CLI Agent Patterns**
- Aider uses git as its undo mechanism -- every edit is a commit, and the user can revert.
- This is analogous to Olive's snapshot system, but with finer granularity.
- Aider's "architect mode" where one model plans and another implements is interesting but adds latency.

---

## 2. Current System Audit

### 2.1 What the Agent Sees (Context Budget)

The autonomous agent's context is assembled from multiple sources. Approximate token costs:

| Source | Tokens | Location |
|--------|--------|----------|
| AGENTS.md (sandbox) | ~2,250 | Sandbox file, read at startup |
| Template catalog | ~400-800 | Appended to AGENTS.md context |
| MCP tool schemas | ~3,000-6,000 | tools/list response (51 tools in filtered mode) |
| Stdin prompt (user message + asset state + decomposition directive) | ~500-800 | Delivered via stdin |
| **Total base cost** | **~6,500-10,000** | Before the agent does anything |

Note: Claude Code's MCP Tool Search may lazy-load tool schemas (95% reduction when >10% of context), so the actual tool schema cost may be much lower with Claude Code. With Codex or other providers, the full cost applies.

### 2.2 Knowledge Pack Coverage Gaps

The autonomous sandbox includes:
- `cli_blueprint.txt` -- core Blueprint building guidance
- `recipe_routing.txt` -- template/recipe discovery
- `blueprint_design_patterns.txt` -- cross-Blueprint communication, interfaces, dispatchers

The autonomous sandbox does NOT include:
- `events_vs_functions.txt` -- critical decision framework for when to use events vs functions
- `node_routing.txt` -- guidance on plan_json vs add_node vs editor.run_python

These two packs are loaded by the PromptAssembler and used by the orchestrated (chat panel) path, but `SetupAutonomousSandbox()` only loads three specific packs by name. This means the autonomous agent (Claude Code, Codex) is missing ~800 tokens of important decision-making guidance.

**Impact:** The agent may make incorrect event-vs-function decisions (e.g., putting Timeline logic in a function graph, which silently fails in UE). The `events_vs_functions.txt` pack contains the critical constraint: "Function graphs are synchronous-only. They CANNOT contain Timeline nodes, Delay nodes, or any latent action."

### 2.3 Observed Agent Behavior (From Logs)

Analysis of the bow-and-arrow run (50 tool calls, 8 failures, ~5 minutes):

**Strengths:**
1. Decomposition directive worked -- agent listed 3 assets before starting
2. Template usage was smart -- used `projectile` factory template for BP_Arrow, built BP_Bow from scratch
3. Build order was correct -- BP_Arrow first (no deps), BP_Bow second, character last
4. Granular fallback worked -- when plan_json failed, agent switched to add_node + connect_pins
5. All three Blueprints compiled successfully

**Weaknesses:**
1. 16% failure rate (8/50 calls) -- each failure costs ~10 seconds of LLM thinking + retry
2. Component function ownership confusion -- called SetVelocityInLocalSpace on BP_Bow (doesn't have ProjectileMovementComponent)
3. Stale node reference after plan rollback -- tried connect_pins with IDs from a rolled-back plan
4. Missing function aliases -- GetActorTransform and AttachActorToComponent not in alias map
5. Sequential tool calling -- agent rarely batches independent calls (e.g., add_variable x6 sent one at a time)

### 2.4 Prompt Architecture Issues

**Redundancy across layers:**
- System decomposition guidance appears in THREE places: stdin directive, `blueprint_design_patterns.txt`, and `cli_blueprint.txt`. Each says the same thing differently. This wastes tokens and risks contradictions.
- Template discovery guidance appears in both `cli_blueprint.txt` and `recipe_routing.txt` with slightly different wording.
- The "three tool approaches" (plan_json, granular, python) are described in `cli_blueprint.txt`, `blueprint_authoring.txt` (not included in sandbox), and `node_routing.txt` (also not included).

**Stdin directive is too verbose:**
- The decomposition directive is ~150 tokens. The existing design spec (agent-behavior-improvements.md) already recommended trimming this. The current version repeats examples that exist in the design patterns knowledge pack.

**Missing end-of-prompt directive:**
- The stdin prompt ends with the decomposition directive, which is good placement (LLMs attend to the end). But the actual imperative is weak: "After listing assets, research patterns with blueprint.list_templates(query='...'), then build each asset fully before starting the next." This should be more forceful about completing the FULL task.

---

## 3. Proposed Changes

### Change 1: Include Missing Knowledge Packs in Autonomous Sandbox

**What:** Add `events_vs_functions.txt` and `node_routing.txt` to `SetupAutonomousSandbox()`.

**Where:** `OliveCLIProviderBase.cpp`, in `SetupAutonomousSandbox()`, after loading `blueprint_design_patterns.txt`.

**Rationale:** These packs contain decision-making guidance the agent currently lacks in autonomous mode. The events_vs_functions pack prevents a specific class of silent failures (Timeline in function graph). The node_routing pack helps the agent choose between plan_json, add_node, and editor.run_python based on the situation.

**Token cost:** +500 tokens to the AGENTS.md sandbox file.

**Risk:** Low. These packs have been stable and are already used by the orchestrated path.

**Priority:** HIGH -- prevents a class of silent failures.

### Change 2: Deduplicate Cross-Layer Guidance

**What:** Remove redundant content across the three knowledge packs, keeping each piece of information in exactly one place.

**Current redundancies:**
1. "Three tool approaches" described in `cli_blueprint.txt` lines 6-11 AND `node_routing.txt` lines 3-8 AND `blueprint_authoring.txt` lines 3-8.
   - **Keep in:** `cli_blueprint.txt` (the primary blueprint reference for autonomous mode)
   - **Remove from:** `node_routing.txt` (keep only the decision guidance that's unique to this file: get_node_pins usage, mixing approaches, "your UE5 knowledge is valid")
   - **Note:** `blueprint_authoring.txt` is not loaded in autonomous mode, so no change needed there.

2. "Template discovery" described in `cli_blueprint.txt` lines 76-97 AND `recipe_routing.txt` lines 3-14.
   - **Keep in:** `cli_blueprint.txt` (integrated with the workflow section)
   - **Trim from:** `recipe_routing.txt` (keep only the recipe-specific routing: `olive.get_recipe` usage, when to skip recipes)

3. "Multi-asset tasks" mentioned in `cli_blueprint.txt` line 24 AND `blueprint_design_patterns.txt` lines 3-32 AND stdin directive.
   - **Keep full version in:** `blueprint_design_patterns.txt` (the authoritative source with examples)
   - **Keep one-liner in:** `cli_blueprint.txt` line 24 (just the pointer: "MULTI-ASSET: see design patterns")
   - **Trim stdin:** Already proposed in existing design spec (Change 5)

**Token savings:** ~200 tokens net reduction after deduplication.

**Priority:** MEDIUM -- reduces token waste and eliminates potential contradictions.

### Change 3: Strengthen End-of-Prompt Completion Directive

**What:** Add a stronger completion directive at the END of the stdin prompt, after the decomposition block.

**Current ending:** "After listing assets, research patterns with blueprint.list_templates(query='...'), then build each asset fully before starting the next."

**Proposed ending:**
```
Build the COMPLETE system. For each Blueprint: create structure (components, variables, functions), write ALL graph logic with apply_plan_json, compile to 0 errors. Do not stop until every asset is fully built and compiled.
```

**Rationale:** The current ending is suggestive ("research patterns, then build each"). The proposed ending is imperative and placed at the very end of stdin -- the position with the highest attention weight. Log analysis shows the agent sometimes stops after scaffolding (creating structure) without writing graph logic, particularly on multi-asset tasks where it hits token limits.

**Token cost:** ~40 tokens (net neutral after trimming the decomposition block).

**Priority:** HIGH -- directly addresses the most common user complaint: "the AI created the Blueprints but didn't wire any logic."

### Change 4: Pre-populate Related Asset Context

**What:** When the user @-mentions an asset, also surface assets that reference or are referenced by it.

**Example:** User says "create a gun system for @BP_ThirdPersonCharacter". Currently we inject BP_ThirdPersonCharacter's state. We should also check if BP_Gun, BP_Weapon, BP_Projectile already exist in the project and mention them.

**Implementation approach:** In `SendMessageAutonomous()`, after resolving @-mentioned assets, do a quick `FOliveProjectIndex` search for assets whose names suggest relevance (e.g., if the message mentions "gun", search for assets containing "gun", "weapon", "projectile"). Include a brief summary: "Existing assets that may be relevant: /Game/BP_Gun (Actor, 3 functions), /Game/BP_Bullet (Actor, 1 function)."

**Rationale:** NeoStack does this automatically. It prevents the agent from creating duplicate assets and helps it understand the existing project structure. Currently the agent wastes 1-2 tool calls on project.search to discover existing assets.

**Token cost:** +100-200 tokens per run (depends on how many related assets exist).

**Risk:** Medium. False positives (surfacing irrelevant assets) could confuse the agent. Use conservative matching: only surface assets whose names contain keywords from the user's message.

**Priority:** MEDIUM -- saves tool calls and prevents duplicate creation.

### Change 5: Improve Tool Batching Guidance

**What:** Add explicit batching guidance to the AGENTS.md sandbox context.

**Current state:** The agent sends tool calls sequentially (e.g., add_variable x6, one at a time). The MCP server can handle parallel calls. The orchestrated path mentions batching but the autonomous sandbox does not.

**Proposed addition to AGENTS.md Critical Rules:**
```
- When calling multiple independent tools (add_variable, add_component), batch them in a single response. The MCP server handles parallel calls efficiently.
```

**Rationale:** The bow-and-arrow log shows 6 sequential add_variable calls (20:42:41-45, ~4 seconds). These could have been batched into one response, saving ~3 seconds of LLM thinking time between calls. Over a 50-call run, sequential calling adds ~30-60 seconds of unnecessary latency.

**Token cost:** +20 tokens.

**Priority:** LOW -- improves speed but not correctness. And batching behavior depends heavily on the LLM provider (Claude Code does batch, Codex tends not to).

### Change 6: Add Pre-Execution Hints to Plan Resolver

**What:** Before executing a plan, check for common mistakes that the resolver can detect and add hints to the error response rather than failing silently.

**Specific checks to add:**
1. **Component function on wrong Blueprint:** If a step calls a function that belongs to a component class not in the current Blueprint's SCS, add a hint: "SetVelocityInLocalSpace is a ProjectileMovementComponent function. This Blueprint has [list of components]. Did you mean to call this on a different Blueprint?"
2. **Event in function graph:** If a plan targets a function graph but contains event ops, hint: "Event ops are only valid in EventGraph. Function graphs use the auto-created entry node."
3. **Timeline in function graph:** If a plan references a Timeline-dependent pattern in a function graph, hint: "Timelines can only be used in EventGraph. Consider using a Custom Event instead."

**Rationale:** This is the "NeoStack approach" of pre-execution validation. Each check prevents one failure + retry cycle (~10 seconds per prevention). The COMPONENT_FUNCTION_ON_ACTOR check in OlivePlanValidator already does something similar -- extend this pattern.

**Token cost:** Zero (C++ change, hints appear only on failure).

**Priority:** HIGH -- prevents the most common observed failure pattern (component function ownership confusion).

### Change 7: Consolidate Recipe and Template Routing

**What:** Merge `recipe_routing.txt` content into `cli_blueprint.txt` and remove `recipe_routing.txt` from the autonomous sandbox.

**Rationale:** `recipe_routing.txt` is only 15 lines and its unique content (when to use `olive.get_recipe`, library vs factory vs community quality levels) fits naturally into the existing "Templates & Pattern Sources" section of `cli_blueprint.txt`. Having two separate files that both discuss template/recipe routing creates redundancy and costs ~200 tokens.

**Proposed merge:** Add the unique recipe_routing content (get_recipe usage, quality comparison) to cli_blueprint.txt's "Templates & Pattern Sources" section. Remove recipe_routing.txt from the sandbox loading in `SetupAutonomousSandbox()`.

**Token cost:** -100 tokens net (deduplication).

**Priority:** MEDIUM -- simplifies the knowledge pack architecture.

### Change 8: Add "Verify Before Done" Step to Completion Flow

**What:** Add guidance that the agent should read back at least one function's graph after applying plan_json to verify the logic was wired correctly.

**Current behavior:** The agent applies plan_json, sees "success", compiles (success), and moves on. But "success" from plan_json means nodes were created and compilation passed -- it does NOT mean the logic is correct. A function could have disconnected data wires that don't cause compile errors.

**Proposed addition to cli_blueprint.txt Rules section:**
```
- After applying plan_json to a complex function (5+ steps), read the function back with blueprint.read_function to verify the graph looks correct. Disconnected data wires don't always cause compile errors.
```

**Rationale:** This is the "compile after changes" safety practice extended to verification. The cost is 1 additional read call per complex function (~50ms, ~200 response tokens). The benefit is catching silent wiring failures before the user tries to use the Blueprint.

**Token cost:** +30 tokens in guidance, +200 tokens per verification read at runtime.

**Priority:** LOW -- nice to have but most plan_json results are correct when they compile. Only worth doing for complex functions.

### Change 9: Improve Self-Correction Error Messages

**What:** Make error messages from failed plan_json calls more actionable by including the specific step that failed and what it was trying to do.

**Current error format:** "2 data wires failed: step 'set_vel' input 'InVelocity' could not connect to step 'make_vec' output..."

**Proposed improvement:** Include a one-line suggestion at the end:
```
Suggestion: This function lives on ProjectileMovementComponent. Add target_class: "ProjectileMovementComponent" to the step, or wire a get_var step for the component to the Target input.
```

**Rationale:** The 3-tier error classification (A/B/C) already exists in `OliveSelfCorrectionPolicy`. Extending it with domain-specific suggestions (component ownership, interface resolution, pin type mismatches) reduces the LLM's guessing and speeds recovery.

**Token cost:** +50-100 tokens per error (only on failure).

**Priority:** HIGH -- directly reduces retry cycles.

---

## 4. Priority Ranking

| # | Change | Priority | Effort | Token Impact | Expected Improvement |
|---|--------|----------|--------|-------------|---------------------|
| 1 | Include missing knowledge packs | HIGH | 15 min | +500 | Prevents silent event/function failures |
| 3 | Strengthen completion directive | HIGH | 10 min | +40 | Prevents "stopped after scaffolding" |
| 6 | Pre-execution hints in resolver | HIGH | 2-4 hrs | 0 | Prevents ~2 failures per run |
| 9 | Improve self-correction messages | HIGH | 1-2 hrs | 0 (runtime only) | Faster recovery from failures |
| 2 | Deduplicate cross-layer guidance | MEDIUM | 30 min | -200 | Cleaner prompts, fewer contradictions |
| 4 | Pre-populate related asset context | MEDIUM | 1-2 hrs | +100-200 | Saves 1-2 tool calls per run |
| 7 | Consolidate recipe/template routing | MEDIUM | 20 min | -100 | Simpler architecture |
| 5 | Improve tool batching guidance | LOW | 5 min | +20 | Faster runs, not more correct |
| 8 | Verify-before-done step | LOW | 10 min | +30 | Catches rare silent failures |

**Recommended implementation order:** 1 -> 3 -> 6 -> 9 -> 2+7 (together) -> 4 -> 5 -> 8

**Net token budget change for HIGH priority items:** +540 tokens (dominated by Change 1).
**Net token budget change for all items:** +390 tokens.

---

## 5. Token Budget Impact

### Current Autonomous Agent Budget

| Component | Tokens | Notes |
|-----------|--------|-------|
| AGENTS.md preamble | ~150 | Role + critical rules |
| cli_blueprint.txt | ~600 | Core Blueprint guidance |
| recipe_routing.txt | ~200 | Template/recipe routing |
| blueprint_design_patterns.txt | ~1,300 | Communication patterns |
| Template catalog | ~400-800 | Factory + library template listing |
| Stdin (user + asset state + decomposition) | ~500-800 | Per-run content |
| MCP tool schemas | ~3,000-6,000 | 51 tools, may be lazy-loaded |
| **Total** | **~6,150-9,900** | |

### After All Changes

| Component | Tokens | Delta |
|-----------|--------|-------|
| AGENTS.md preamble | ~150 | 0 |
| cli_blueprint.txt (merged with recipe_routing) | ~700 | +100 (merge) |
| recipe_routing.txt | 0 | -200 (removed, merged) |
| blueprint_design_patterns.txt | ~1,300 | 0 |
| events_vs_functions.txt (NEW) | ~300 | +300 |
| node_routing.txt (NEW, trimmed) | ~200 | +200 |
| Template catalog | ~400-800 | 0 |
| Stdin (trimmed + completion directive) | ~400-700 | -100 |
| Related asset context (NEW) | ~100-200 | +100-200 |
| MCP tool schemas | ~3,000-6,000 | 0 |
| **Total** | **~6,550-10,150** | **+400** |

The +400 token increase is justified by the prevention of ~2-4 failures per run, each of which costs ~200-500 tokens in error messages and retry prompts. Net savings at runtime: ~400-1,600 tokens per run.

---

## 6. Verification Plan

### Test Prompt 1: "create a bow and arrow system for @BP_ThirdPersonCharacter"
(Regression test -- same as previous analysis)

**Success criteria:**
- Agent lists 3 assets before first tool call
- No component function ownership failures (SetVelocityInLocalSpace on wrong BP)
- No stale node reference after plan rollback
- Total tool calls < 42 (down from 50)
- Total failures < 4 (down from 8)
- All 3 BPs compile successfully

### Test Prompt 2: "create a door that opens smoothly when the player presses E"
(Tests events_vs_functions knowledge -- requires Timeline in EventGraph, not function)

**Success criteria:**
- Agent creates BPI_Interactable with Interact() as an event (no outputs), not a function
- Door implementation uses Timeline in EventGraph (not in a function graph)
- Agent does NOT put Timeline inside a function graph (this would silently fail)
- Player interaction uses overlap detection + input event pattern

### Test Prompt 3: "create a health system with a damage interface and a health bar widget"
(Tests multi-domain: Blueprint + Widget + Interface)

**Success criteria:**
- Agent creates BPI_Damageable interface
- Health component or health variables on the character
- Widget Blueprint for health bar (uses widget. tools)
- Event dispatcher for health changes
- Agent does not get confused by cross-domain tool selection

### Test Prompt 4: "add a double jump to @BP_ThirdPersonCharacter"
(Tests MODIFY workflow -- existing asset, no creation)

**Success criteria:**
- Agent does NOT create a new Blueprint
- Reads existing BP_ThirdPersonCharacter first
- Adds jump counter variable, modifies input handling
- Compiles with 0 errors
- Does not re-create components that already exist

### Test Prompt 5: "make me a gun" (deliberately ambiguous)

**Success criteria:**
- Agent asks itself "does this need multiple Blueprints?" and lists assets
- Creates at minimum BP_Gun + BP_Bullet (not just one BP)
- Does not over-build (no unnecessary interfaces for a simple gun)
- Handles ambiguity about where the gun should be placed (character? world?)

---

## 7. Competitor Patterns to Adopt vs. Differentiate

### Adopt from NeoStack
1. **Pre-execution validation** -- Extend our Phase 0 plan validation with more checks (component ownership, event/function graph constraints). This is AIK's biggest strength.
2. **Proactive context injection** -- Surface related assets before the agent has to search for them.
3. **Asset type breadth** -- Materials and Niagara are the most requested missing types. Not in this design spec's scope but should be on the roadmap.

### Adopt from Cursor
1. **Codebase indexing** -- Our FOliveProjectIndex serves a similar purpose. Ensure it's used for related asset discovery (Change 4).
2. **@-mention as context pin** -- Already implemented. Keep improving the asset state summary format.

### Adopt from Aider
1. **Snapshot as undo** -- Already implemented via FOliveSnapshotManager. Consider making snapshot creation more visible in the UI.

### Differentiate from All Competitors
1. **Plan JSON** -- Our declarative graph editing system is unique. No competitor has anything equivalent. This is our deepest competitive advantage. Protect and improve it.
2. **Library templates** -- 658 templates with 2,343 indexed functions from real shipped projects. No competitor has this depth of reference material available to the agent.
3. **Three-tier error classification** -- Our A/B/C error categorization with progressive disclosure is more sophisticated than any competitor's error handling.
4. **Multi-provider support** -- Claude Code, Codex, OpenRouter, Ollama, etc. AIK is Claude Code-dependent ("best results by far").

### Do NOT Adopt
1. **AIK's "single tool" consolidation** to 15 tools. Our tool decomposition (51 filtered tools) with Claude Code's lazy loading is already efficient. Consolidating to 15 would lose the semantic clarity of domain-prefixed tools.
2. **Aura's credit-based pricing model.** Creates friction that hurts iteration speed.
3. **Opaque middleware** that hides the AI's actions. Our transparent tool calling is better for debugging and user trust.

---

## 8. Edge Cases

### Ambiguous Prompts
"Make me a weapon" -- could be gun, sword, bow, etc. The agent should pick a reasonable default (gun is most common) and state its assumption: "I'll create a gun system with BP_Gun and BP_Bullet. If you wanted a different weapon type, let me know."

### Missing Templates
When no library or factory template matches the user's request, the agent should build from first principles using its UE5 knowledge. The template system is a reference, not a requirement. Current prompts correctly describe templates as "starting points, not scripts."

### Multi-Asset Dependency Cycles
If BP_A references BP_B and BP_B references BP_A (circular dependency), the agent should use forward declarations (object reference variables with a class name) and build in two passes: structure first (both BPs with variables and components), then logic (plan_json for each). Current decomposition guidance handles this implicitly by building "each asset fully" -- but forward references may require partial builds. Not currently addressed in prompts.

### Token Limit Exhaustion
Long multi-asset tasks may exhaust the context window before completing all Blueprints. The auto-continuation system handles this (builds continuation prompt with asset state summary). The key improvement would be ensuring the agent prioritizes completing graph logic over adding more structure -- an incomplete Blueprint with logic is more useful than a complete structure with no logic.

---

## 9. Long-Term Considerations

### Model-Specific Optimization
Current prompts are model-agnostic. Evidence from NeoStack suggests Claude produces "best results by far" for Blueprint logic. Consider:
- Model-specific stdin directives (Claude gets less hand-holding, Codex gets more)
- Model-specific token budgets (Claude has larger context, Codex is more constrained)
- This would live in the provider-specific `GetCLIArgumentsAutonomous()` overrides

### Streaming Plan Preview
Surface the agent's decomposition plan in the UI as it streams, letting the user intervene before tool calls start. This is the "Windsurf Cascade" pattern adapted for UE. Would require changes to the chat panel (SOliveAIChatPanel) and the run manager.

### Community Blueprint Quality Scoring
The olive.search_community_blueprints tool returns 150K+ results of mixed quality. Adding quality scoring (based on node count, compile status, community ratings) would help the agent select better references. This is a future feature, not a prompt change.

### Knowledge Pack Versioning
As we add more knowledge packs and the agent context grows, we need a strategy for managing token budget. Options:
- Demand-load packs based on the user's message content (similar to tool filtering)
- Compress packs (shorter descriptions, fewer examples)
- Move to the Speakeasy pattern: `search_knowledge` + `read_knowledge` tools instead of pre-loading

---

## Appendix A: File Locations for All Proposed Changes

| Change | Files to Modify |
|--------|----------------|
| 1. Include missing knowledge packs | `OliveCLIProviderBase.cpp` (SetupAutonomousSandbox) |
| 2. Deduplicate guidance | `cli_blueprint.txt`, `recipe_routing.txt`, `node_routing.txt` |
| 3. Completion directive | `OliveCLIProviderBase.cpp` (stdin construction in SendMessageAutonomous) |
| 4. Related asset context | `OliveCLIProviderBase.cpp` (SendMessageAutonomous), `OliveProjectIndex.h/cpp` |
| 5. Batching guidance | `OliveCLIProviderBase.cpp` (SetupAutonomousSandbox, Critical Rules section) |
| 6. Pre-execution hints | `OlivePlanValidator.h/cpp`, `OliveBlueprintPlanResolver.cpp` |
| 7. Consolidate routing | `cli_blueprint.txt`, `OliveCLIProviderBase.cpp` (remove recipe_routing load) |
| 8. Verify-before-done | `cli_blueprint.txt` |
| 9. Self-correction messages | `OliveSelfCorrectionPolicy.cpp` |

## Appendix B: Exact Prompt Text for Changes 1, 3, 7

### Change 1: events_vs_functions.txt inclusion

Add after the blueprint_design_patterns loading block in SetupAutonomousSandbox():
```cpp
FString EventsFunctions;
if (!FFileHelper::LoadFileToString(EventsFunctions, *FPaths::Combine(KnowledgeDir, TEXT("events_vs_functions.txt"))))
{
    UE_LOG(LogOliveCLIProvider, Warning, TEXT("Failed to load events_vs_functions.txt knowledge pack"));
}

FString NodeRouting;
if (!FFileHelper::LoadFileToString(NodeRouting, *FPaths::Combine(KnowledgeDir, TEXT("node_routing.txt"))))
{
    UE_LOG(LogOliveCLIProvider, Warning, TEXT("Failed to load node_routing.txt knowledge pack"));
}

// ... later, when assembling AgentContext:
if (!EventsFunctions.IsEmpty())
{
    AgentContext += TEXT("---\n\n");
    AgentContext += EventsFunctions;
    AgentContext += TEXT("\n\n");
}

if (!NodeRouting.IsEmpty())
{
    AgentContext += TEXT("---\n\n");
    AgentContext += NodeRouting;
    AgentContext += TEXT("\n\n");
}
```

### Change 3: Completion directive (replace current decomposition block)

```cpp
if (!IsContinuationMessage(UserMessage))
{
    EffectiveMessage += TEXT("\n\n## Task Plan\n\n");
    EffectiveMessage += TEXT("Before calling any tools, list every game entity that needs its own Blueprint:\n\n");
    EffectiveMessage += TEXT("ASSETS:\n");
    EffectiveMessage += TEXT("1. BP_Name - Type - purpose\n");
    EffectiveMessage += TEXT("2. ...\n\n");
    EffectiveMessage += TEXT("Separate actor test: \"Does it exist in the world with its own transform?\" If yes, separate Blueprint. ");
    EffectiveMessage += TEXT("Weapons, projectiles, pickups, doors = always separate actors.\n\n");
    EffectiveMessage += TEXT("Build the COMPLETE system. For each Blueprint: create structure (components, variables, functions), ");
    EffectiveMessage += TEXT("write ALL graph logic with apply_plan_json, compile to 0 errors. ");
    EffectiveMessage += TEXT("Do not stop until every asset is fully built and compiled.\n");
}
```

### Change 7: Merged cli_blueprint.txt "Templates & Pattern Sources" section

Replace existing lines 76-97 with:
```
## Templates & Pattern Sources
Templates are reference material, not scripts. Study patterns, then build your own approach.
- Library templates (highest quality): extracted from proven projects. Use blueprint.list_templates(query="...") to search, blueprint.get_template(id, pattern="FuncName") to read a function's full graph. Adapt naming to fit the user's project.
- Factory templates (good quality): parameterized templates for common patterns. Use blueprint.create with template_id for scaffolding. After creation, write plan_json for EACH function stub.
- Community blueprints (mixed quality): olive.search_community_blueprints(query) for 150K+ examples. Browse 5-10 results, compare patterns, use your judgment.
- Recipes: olive.get_recipe(query) has tested wiring patterns. Use when unsure about a specific pattern, skip for well-known operations.

Compare multiple references when available. Your UE5 knowledge is valid; if you know a better approach, use it.
Search templates mid-build if you're struggling with complex patterns (timelines, animation montages, AI perception).
```
