# Research: Knowledge Injection — Pre-Pipeline vs Post-Pipeline

## Question
Before the multi-agent pipeline (Scout → Planner → Validator → Builder → Reviewer) was introduced in commit `0449a9b` (2026-03-07), how did the old single-pass system inject knowledge packs, recipes, and design patterns into the AI's context? What changed when the pipeline was introduced, and was anything lost in the split?

---

## Findings

### 1. The Old Single-Pass System (pre-0449a9b)

The pre-pipeline system had two prompt surfaces, both pointing at the same LLM (the Builder CLI):

**Surface A — AGENTS.md / sandbox context file** (read by the Builder CLI on startup via `SetupAutonomousSandbox()`):

This file was written to `Saved/OliveAI/AgentSandbox/AGENTS.md` and contained the following knowledge packs concatenated in order:

| Pack | File | Size |
|------|------|------|
| `cli_blueprint.txt` | `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | 10,004 bytes / 134 lines |
| `recipe_routing.txt` | `Content/SystemPrompts/Knowledge/recipe_routing.txt` | 1,807 bytes / 15 lines |
| `blueprint_design_patterns.txt` | `Content/SystemPrompts/Knowledge/blueprint_design_patterns.txt` | 13,241 bytes / 307 lines |
| `events_vs_functions.txt` | `Content/SystemPrompts/Knowledge/events_vs_functions.txt` | 2,546 bytes / 42 lines |
| `node_routing.txt` | `Content/SystemPrompts/Knowledge/node_routing.txt` | 1,962 bytes / 35 lines |

**Total: ~29,560 bytes (~7,400 tokens) of knowledge material written directly into the agent's persistent context file.**

Source: `OliveCLIProviderBase.cpp`, `SetupAutonomousSandbox()`, lines 341–431 (pre-pipeline version, confirmed via `git show beb671e`).

**Surface B — stdin prompt** (the `EffectiveMessage` delivered via stdin):

The pre-pipeline system also injected into stdin:
- `@-mention` asset state summaries (load-time Blueprint IR)
- Related assets from keyword search (up to 10 project assets)
- A structured "Required: Asset Decomposition" directive that told the Builder to enumerate assets before calling tools
- Template discovery results from `FOliveUtilityModel::RunDiscoveryPass()` (utility model searched for relevant library/factory/community templates)

**Key characteristic of the old system:** ALL knowledge reached the SAME LLM — the Builder. There was no knowledge-routing problem because there was only one agent.

---

### 2. The New Pipeline System (post-0449a9b)

The pipeline splits responsibilities across two separate CLI invocations:

**Planner agent** (spawned by `RunPlannerWithTools()` or `RunPlanner()`) — receives:

| What | How | What it gets |
|------|-----|--------------|
| Planner system prompt | `BuildPlannerSystemPrompt()` — hardcoded string | Architecture rules, Build Plan format, function description requirements |
| Available tools | Appended to system prompt | `blueprint.get_template`, `blueprint.list_templates`, `blueprint.describe`, `olive.get_recipe` |
| Task | stdin `PlannerUserPrompt` | User message + complexity + existing asset IR |
| Template references | stdin `PlannerUserPrompt` | Template IDs and matched functions from Scout |
| Blueprint Pattern Knowledge | stdin `PlannerUserPrompt` — hardcoded inline text | ~400-char summary: Events vs Functions rules + Plan JSON Ops Reference |
| CLAUDE.md sandbox | `SetupPlannerSandbox()` writes minimal CLAUDE.md | Role description only: "You are an Unreal Engine Blueprint architect. Research templates and produce a Build Plan." |

**What the Planner does NOT get:**
- `cli_blueprint.txt` (10KB) — the full plan_json reference with exec/data wire rules, variable types, component patterns, etc.
- `blueprint_design_patterns.txt` (13KB) — System Decomposition rules, cross-BP communication patterns, interface workflow, dispatcher workflow, overlap event patterns
- `events_vs_functions.txt` (2.5KB) — the FULL file. The Planner gets a ~300-char inline summary extracted from this file.
- `node_routing.txt` (2KB) — the full node routing guide covering `add_node`, `editor.run_python`, SplitPin, etc.
- `recipe_routing.txt` (1.8KB) — the full recipe routing guide. The Planner can call `olive.get_recipe` via MCP but doesn't know the routing heuristics upfront.

**Builder agent** (the Claude Code CLI spawned by `SendMessageAutonomous()`) — receives:

| What | How | What it gets |
|------|-----|--------------|
| AGENTS.md sandbox | `SetupAutonomousSandbox()` — UNCHANGED | Same 5 knowledge packs totaling ~29.5KB. `cli_blueprint.txt`, `recipe_routing.txt`, `blueprint_design_patterns.txt`, `events_vs_functions.txt`, `node_routing.txt`. |
| Pipeline output | `FormatForPromptInjection()` injected into stdin | Task Analysis, Reference Templates section, Build Plan, Validator warnings, Existing Assets, Execution directive |

**The Builder still receives ALL the knowledge packs** — `SetupAutonomousSandbox()` was not changed by the pipeline commit. The AGENTS.md content is identical to the pre-pipeline era.

Source: `OliveCLIProviderBase.cpp` `SetupAutonomousSandbox()` — present in both `beb671e` and current HEAD, unchanged.

---

### 3. What the Planner Gets vs What the Old System Had

| Knowledge Pack | Old System (Builder) | New Planner | New Builder |
|---|---|---|---|
| `cli_blueprint.txt` (10KB) | YES — AGENTS.md | NO | YES — AGENTS.md |
| `recipe_routing.txt` (1.8KB) | YES — AGENTS.md | NO (but can call `olive.get_recipe`) | YES — AGENTS.md |
| `blueprint_design_patterns.txt` (13KB) | YES — AGENTS.md | NO | YES — AGENTS.md |
| `events_vs_functions.txt` (2.5KB) | YES — AGENTS.md | ~300-char inline summary only | YES — AGENTS.md |
| `node_routing.txt` (2KB) | YES — AGENTS.md | NO | YES — AGENTS.md |
| Template catalog | YES — via `GetCapabilityKnowledge()` (API providers) / discovery pass (CLI) | YES — Scout injects `TemplateOverviews` in `RunPlanner`, template IDs in `RunPlannerWithTools` | YES — via FormatForPromptInjection Section 2/2.5 |
| Decomposition rules | YES — inline in stdin ("Required: Asset Decomposition" + AGENTS.md Section 0) | PARTIAL — only what's in the Build Plan format template | YES — AGENTS.md `blueprint_design_patterns.txt` Section 0 |

---

### 4. API Provider Path (non-CLI) — for Completeness

`FOlivePromptAssembler::AssembleSystemPromptInternal()` builds the system prompt for API-based providers (Anthropic, OpenRouter, etc.) and includes via `GetCapabilityKnowledge()`:

For profiles "Auto" and "Blueprint":
- `blueprint_authoring` (3,470 bytes)
- `recipe_routing` (1,807 bytes)
- `node_routing` (1,962 bytes)
- `blueprint_design_patterns` (13,241 bytes)
- `events_vs_functions` (2,546 bytes)
- Template catalog block from `FOliveTemplateSystem::GetCatalogBlock()`

The API provider path passes all these in the system prompt. The `BuildSharedSystemPreamble()` method was specifically designed (comment in header) so "Claude Code provider, future CLI providers, etc. call this to stay in sync with the knowledge packs that API providers get automatically." However, `SetupAutonomousSandbox()` does NOT call `BuildSharedSystemPreamble()` — it loads the files directly by name, which is functionally equivalent for the knowledge packs included.

Source: `OlivePromptAssembler.cpp` lines 279–323, 544–548.

---

### 5. What Changed in the Pipeline Introduction Commit

From the `git show 0449a9b` diff of `OliveCLIProviderBase.cpp`:

**Removed from stdin:**
- The "Required: Asset Decomposition" mandatory pre-tool enumeration directive (forced Builder to list all assets before any tool calls)
- The `FOliveUtilityModel::RunDiscoveryPass()` template pre-search (utility model found relevant templates, formatted results as "Reference Templates Found")
- The structured decomposition rules about weapon actors, "Separate Actor Test," etc.

**Added to stdin:**
- `FormatForPromptInjection()` output: Task Analysis, Reference Templates (Section 2.5 fallback only), Build Plan, Validator warnings, Existing Assets, Execution directive

**Unchanged:**
- `SetupAutonomousSandbox()` — AGENTS.md content is identical. The Builder still gets all 5 knowledge packs.

---

### 6. Recipe Access — Old vs New

**Old system:** Recipes were accessible via `olive.get_recipe` MCP tool (the Builder could call it at will). The `recipe_routing.txt` knowledge pack in AGENTS.md told the Builder WHEN to use recipes vs templates vs community blueprints.

**New Planner:** Can call `olive.get_recipe` via MCP (it's in the filtered tool set for `RunPlannerWithTools()`). However, the Planner does NOT get `recipe_routing.txt` upfront, so it doesn't know the routing heuristic: "Use it when unsure about a pattern, but skip it for simple/well-known operations."

**New Builder:** Same as old system — `olive.get_recipe` via MCP, `recipe_routing.txt` in AGENTS.md.

---

### 7. The `events_vs_functions.txt` Gap — Most Critical Loss

The full `events_vs_functions.txt` (2.5KB, 42 lines) covers:
- Synchronous vs latent function rules
- When to use Custom Events instead of Functions
- Interface function types (with/without outputs)
- Timeline, Delay, ForceAsFunction metadata
- Multi-cast delegates pattern
- Animation Montage pattern

The Planner gets only the 7-line inline summary in `BuilderPlannerUserPrompt`:
```
- USE A FUNCTION when: returns a value AND logic is synchronous (one frame)
- USE AN EVENT when: logic spans multiple frames OR no return value needed
- Functions CANNOT contain Timeline, Delay, or latent actions
- Interface: no outputs = implementable event, has outputs = synchronous function
- Hybrid pattern: function returns sync result + calls Custom Event for async work
```

This is the most significant knowledge gap. The Planner decides whether each function should be a Function or a Custom Event — which determines Blueprint structure. Getting this wrong causes compile failures that the Builder then has to fix. The session log analyses (08c, 08d, 08e, 08f) all show this as a recurring failure mode: "InitializeArrow called without Target wire," "NockArrow gutted," etc. — many of these stem from the Planner putting async logic in functions.

---

### 8. The `blueprint_design_patterns.txt` Gap

The Planner does NOT receive `blueprint_design_patterns.txt` (13KB). This file contains:
- Section 0: System Decomposition rules ("Separate Actor Test," asset list format)
- Section 1-N: Blueprint Interface complete workflow, Event Dispatcher complete workflow, Damage system, Overlap events, Pickup actors, Inventory, Character class hierarchy, NPC interaction

The Build Plan format in `BuildPlannerSystemPrompt()` partially covers decomposition (the `### Order` section), but lacks the "Separate Actor Test" rules and "Common Decomposition Mistakes" that were key to preventing scope misses (documented in run 08e: Planner created a component-based ranged system instead of separate weapon actors because it didn't have the decomposition rules).

---

### 9. What the `RunPlanner` vs `RunPlannerWithTools` Paths Differ On

Both paths use the same `BuildPlannerSystemPrompt()` and inject the same "Blueprint Pattern Knowledge" block.

Key difference: `RunPlanner` injects `TemplateOverviews` (the Scout's pre-fetched summaries, ~1.5K chars each, up to 3) directly into the user prompt. `RunPlannerWithTools` instead lists template IDs/names and says "Read these with `blueprint.get_template` before writing the Build Plan." The MCP path can thus get richer template data by fetching full function graphs, but the single-shot path gets the pre-summarized overview.

Neither path injects the knowledge packs from `Content/SystemPrompts/Knowledge/`.

---

## Recommendations

1. **The Planner has a significant knowledge deficit relative to the old system.** It receives ~700 chars of inline knowledge vs ~29.5KB in AGENTS.md that the old Builder received. The Planner makes architectural decisions (function vs event, separate actor vs component) that directly affect compile success, but it's operating on much less guidance than the old single-pass Builder.

2. **Highest priority gap: `events_vs_functions.txt`.** The full file (42 lines) should be injected into the Planner's system prompt, not just the 5-line inline summary. Latent op misclassification (putting Delay/Timeline in a function) causes gutted functions in the Builder phase and is the dominant failure mode across session log analyses 08c–08f.

3. **Second priority gap: System Decomposition rules from `blueprint_design_patterns.txt`.** At minimum, Section 0 (System Decomposition, Separate Actor Test, common mistakes) should be in the Planner's system prompt. Session 08e shows a scope miss caused by missing this knowledge.

4. **The Builder path is unchanged and correct.** `SetupAutonomousSandbox()` still injects all 5 knowledge packs into AGENTS.md. No regression there.

5. **`recipe_routing.txt` in Planner is low priority** because the Planner can call `olive.get_recipe` via MCP. The routing heuristic ("skip for simple operations") is a nice-to-have, not critical.

6. **`node_routing.txt` and `cli_blueprint.txt` in Planner are not needed.** These cover Build execution details (`add_node`, `connect_pins`, plan_json v2.0 format, wiring syntax) that the Builder handles. The Planner only produces a natural-language Build Plan — it doesn't write plan_json.

7. **Consider a `BuildPlannerKnowledgePreamble()` method on `FOlivePromptAssembler`** that returns: `events_vs_functions.txt` (full) + Section 0 of `blueprint_design_patterns.txt` (decomposition only). Keep it targeted — the Planner does NOT need the full 13KB design patterns file.

8. **The `BuildPlannerSystemPrompt()` keeps growing.** The Planner system prompt is already a long hardcoded string. Consider loading knowledge from files at runtime (same pattern as `SetupAutonomousSandbox()`) so content can be updated without recompiling.
