# Research: Competitive Tool Analysis — AI-UE5 Blueprint Editing (Feb 2026)

## Question
What does the current competitive landscape look like for AI-driven Unreal Engine Blueprint tools? What is the minimal effective tool set for Blueprint editing? Should Olive consolidate from ~98 tools to ~25-35, and what should be cut vs kept?

---

## Findings

### 1. Agent Integration Kit (AIK) — Current State as of February 2026

**Source:** [aik.betide.studio](https://aik.betide.studio/), changelog page fetched Feb 27, 2026

AIK is now at **v0.5.6** (Feb 21, 2026), having shipped 6+ versions in February 2026 alone. The velocity is extreme — every 1-3 days. Key release milestones:

| Version | Date | Key Change |
|---------|------|------------|
| v0.5.6 | Feb 21 | In-process Claude installer, Bun lockfile recovery |
| v0.5.5 | Feb 18 | UE 5.5 stability, GitHub Copilot CLI / Gemini CLI improvements |
| **v0.5.0** | **Feb 16** | **Complete UI rebuild, tools consolidated 27+ → 15, zero-setup install** |
| v0.4.0 | Feb 10 | 100% animation tool coverage (BlendSpace, AnimSequence) |
| v0.3.2 | Feb 9 | Full Control Rig support, rewritten Behavior Trees, 3D model generation |
| v0.3.0 | Feb 8 | 100% Blueprint editing coverage |
| v0.2.0 | Feb 7 | Animation Montage, Enhanced Input, Viewport Screenshot |

**The v0.5.0 consolidation is the critical data point.** Betide explicitly replaced multiple granular tools (separate `edit_ik_rig`, `edit_control_rig`, etc.) with "intelligent single tools that figure out what you mean automatically." The docs describe moving to 15 tools from 27+, covering: Blueprints, Materials, Animation systems, VFX, Sequencer, Behavior Trees/State Trees, IK Rigs, Enhanced Input, PCG, MetaSounds, Viewport, and Python/C++ execution.

**The exact 15 tool names are not publicly documented.** AIK's tool definitions are proprietary. The documentation describes them at the category level only.

**Validation:** The "500+ pre-execution checks" figure from the original research doc is confirmed as current. AIK has invested heavily in pre-execution validation to prevent the crash storms they experienced at launch.

**User feedback (synthesized):**
- Betide recommends Claude Code as "best results by far" — other LLMs produce "less than desired results" for Blueprint logic. This is a limitation users hit immediately when using OpenRouter alternatives.
- One user review (Philip Conrod, Feb 2026) praised the rapid iteration and 100% Blueprint coverage but noted the LLM dependency on Claude Code.
- The Aura competitor forum thread shows a related pattern: "AI refuses to work on existing blueprints" — a complaint about over-conservative behavior that AIK appears to have solved via its validator + discovery combination.
- Support is hands-on (same-day Discord resolution), suggesting the product is still beta in practice despite the polished changelog.

**Pricing:** One-time $109.99 (Fab marketplace). Competitors charge $25-375/month.

---

### 2. flopperam/unreal-engine-mcp — Current State

**Source:** [GitHub README](https://github.com/flopperam/unreal-engine-mcp/blob/main/README.md), [blueprint-graph-guide.md](https://github.com/flopperam/unreal-engine-mcp/blob/main/Guides/blueprint-graph-guide.md), [DeepWiki analysis](https://deepwiki.com/flopperam/unreal-engine-mcp), Feb 2026

As of Feb 14, 2026 (last update), the repo has **510 stars** and **30+ tools** total, split across:

- **Blueprint graph editing (11 tools):** `add_node`, `connect_nodes`, `disconnect_nodes`, `delete_node`, `set_node_property`, `create_variable`, `set_blueprint_variable_properties`, `create_function`, `add_function_input`, `add_function_output`, `delete_function`, `rename_function`
- **Blueprint analysis (4 tools):** `read_blueprint_content`, `analyze_blueprint_graph`, `get_blueprint_variable_details`, `get_blueprint_function_details`
- **Actor/level (5 tools):** `get_actors_in_level`, `find_actors_by_name`, `delete_actor`, `set_actor_transform`, `get_actor_material_info`
- **Physics/materials (6 tools):** `spawn_physics_blueprint_actor`, `set_physics_properties`, `get_available_materials`, `apply_material_to_actor`, `apply_material_to_blueprint`, `set_mesh_material_color`
- **World building macros (10+ tools):** `create_town`, `construct_house`, `create_castle_fortress`, `create_maze`, etc. — these are high-level composition tools that orchestrate multiple low-level calls

**23 node types for `add_node`** organized in 6 categories:
- Control Flow: `Branch`, `Comparison`, `Switch` (Byte/Enum/Integer), `ExecutionSequence`
- Data: `VariableGet`, `VariableSet`, `MakeArray`
- Casting: `DynamicCast`, `ClassDynamicCast`, `CastByteToEnum`
- Utility: `Print`, `CallFunction`, `Select`, `SpawnActor`
- Specialized: `Timeline`, `GetDataTableRow`, `AddComponentByClass`, `Self`, `Knot`
- Animation: `PlayAnimation`, `StopAnimation`

**Tool schema for `add_node`:**
```
Required: blueprint_name, node_type
Optional: pos_x, pos_y, message, event_type, variable_name,
          target_function, target_blueprint, function_name
```

**Tool schema for `connect_nodes` / `disconnect_nodes`:**
```
Required: blueprint_name, source_node_id, source_pin_name,
          target_node_id, target_pin_name
```

**Pin name resolution approach:** The guide says "inspect nodes in Blueprint editor when uncertain." The C++ side uses `Node->FindPin(PinName)` — exact match required. Common names: `execute` (exec input), `then` (exec output). NO fuzzy matching on flopperam's C++ side — pin names must match exactly as they appear in the UE editor.

**Validation:** Minimal. Pre-execution checks limited to: variables must pre-exist for VariableGet/Set, functions must exist for CallFunction, event names must be valid UE types. No crash-prevention harness like AIK. They rely on compile-and-check.

**Architecture:** Python FastMCP server → TCP socket (127.0.0.1:55557) → C++ plugin → UE5 K2Node API. The "world building" high-level tools are Python-side composition of many low-level calls — NOT separate C++ operations.

---

### 3. New Competitors Since Original Research Doc

**Source:** Epic forums, GitHub searches, Feb 2026

Several projects have launched or matured significantly:

**Aura (by RamenVR)** — In-editor chat UI with Agent mode (direct changes) and Ask mode (analysis only). Supports Claude Sonnet 4.5 with "consistent results." Blueprint support exists but has reported stability issues: "even with the setting turned on the AI refuses to work on existing blueprints." Credit-based pricing. Public beta, February 2026.
Source: [Epic Forums](https://forums.unrealengine.com/t/aura-ai-agent-for-unreal-editor/2689209), [Yahoo Finance launch announcement](https://finance.yahoo.com/news/aura-ai-assistant-unreal-engine-173400158.html)

**CLAUDIUS** — "Claude's Unreal Direct Interface & Unified Scripting." 120+ commands across 17 categories via HTTP REST API or JSON file polling. Blueprint category is present but appears shallow (add variable, add function, add component — no graph node editing). Project sustainability questioned in Feb 2026 (Discord went dark). File-based communication design is notable — designed specifically for Claude Code's file-reading capability.
Source: [Epic Forums](https://forums.unrealengine.com/t/claudius-code-claudius-ai-powered-editor-automation-framework/2689084)

**mirno-ehf/ue5-mcp** — v1.0.0 shipped Feb 14, 2026. TypeScript + C++ (59% C++, 41% TS). Covers "Blueprints, materials, and Anim Blueprints." 48 commits on main. No detailed tool schema publicly visible. Uses HTTP (not TCP) for the UE plugin side.
Source: [GitHub](https://github.com/mirno-ehf/ue5-mcp)

**chongdashu/unreal-mcp** — Now at 1,500 stars (up from 1,200 in the research doc). Blueprint scripting with event graph / node connections added. Marked EXPERIMENTAL. Blueprint tools documented but schema details are sparse publicly.
Source: [GitHub](https://github.com/chongdashu/unreal-mcp), [Blueprint tools doc](https://github.com/chongdashu/unreal-mcp/blob/main/Docs/Tools/blueprint_tools.md)

**gimmeDG/UnrealEngine5-mcp** — Notable for the most sophisticated pin resolution of any open-source project: 3-pass algorithm (exact match → case-insensitive match → first data output fallback), wildcard pin type resolution via `ReconstructNode()`, and explicit classification of pins as Required / Optional / NotConnectable. Also has `list_blueprint_nodes` with rich filtering (by type, event name, title substring, unconnected pins, sort order). 47 Blueprint operations, 20 of which are node graph specific.
Source: [DeepWiki](https://deepwiki.com/gimmeDG/UnrealEngine5-mcp/3.2.2-node-graph-system)

**Natfii/UnrealClaude** — UE 5.7-specific, 20+ MCP tools with a "dynamic context loader" that provides UE 5.7 API documentation on demand. Notable for the docs-on-demand pattern.
Source: [GitHub](https://github.com/Natfii/UnrealClaude)

---

### 4. MCP Spec Status and Tool Design Best Practices (Feb 2026)

**Source:** [MCP spec update blog](https://modelcontextprotocol.info/blog/mcp-next-version-update/), [Anthropic engineering blog](https://www.anthropic.com/engineering/code-execution-with-mcp), [Lunar.dev tool overload article](https://www.lunar.dev/post/why-is-there-mcp-tool-overload-and-how-to-solve-it-for-your-ai-agents), [Speakeasy dynamic toolsets](https://www.speakeasy.com/blog/how-we-reduced-token-usage-by-100x-dynamic-toolsets-v2), [Claude Code MCP Tool Search](https://claudefa.st/blog/tools/mcp-extensions/mcp-tool-search)

**MCP Spec status:**
- Current stable: `2025-11-25`; previous: `2025-06-18` (added structured tool outputs, OAuth, elicitation, security improvements)
- SEP-1686 (async task primitives) is NOT adopted yet — target is Q1 2026 for finalization, June 2026 for the next spec release
- Async operations still handled via tool-splitting pattern: `start_X` / `get_status` / `get_result`
- Elicitation (server asking user for input mid-execution) is in `2025-06-18` spec — relevant for confirmation dialogs

**The "too many tools" problem — hard numbers:**
- Platform hard limits: Claude 120 tools max, OpenAI 128 max, Cursor 80 max
- 5 MCP servers x 30 tools each = 150 tools = 30,000-60,000 tokens overhead (200-500 tokens/tool description)
- That's 25-30% of a 200k context window consumed before the user's first message
- Research shows: agents "experience decline in tool calling accuracy surprisingly early, after adding just a handful of tools"
- The MCP Tool Trap paper documents that accuracy drops are not linear — they onset quickly, then plateau

**Claude Code MCP Tool Search (critical new development):**
- Claude Code now implements **lazy loading** for MCP tool definitions
- Activates automatically when MCP tool descriptions would use >10% of context window
- Measured reduction: from ~134k tokens → ~5k tokens (95% reduction)
- Specific numbers: 39.8k tokens (19.9% of context) → ~5k tokens (2.5%)
- **This changes the calculus.** If Claude Code is the primary client, having 25 vs 90 tools is less important than it was 6 months ago — the lazy loader mitigates the context bloat

**Anthropic's code-first pattern:**
- Anthropic's engineering blog recommends structuring MCP servers so agents write code to call tools rather than calling tools directly
- Claimed 98% token reduction (150,000 → 2,000 tokens) for bulk operations
- Pattern: agents discover tools via filesystem, read definitions on demand, compose operations in code
- This is NOT what any UE5 MCP implementation currently does — all use direct tool calls

**Tool description quality research (arXiv 2602.14878, Feb 2026):**
- 97.1% of analyzed MCP tool descriptions "contain at least one smell" (vague purpose, missing limits, opaque params)
- Improving descriptions improved task success by median 5.85 percentage points
- BUT: augmented descriptions increased execution steps by 67.46% — more verbose tools cause more roundtrips
- The `Examples` component in tool descriptions does NOT statistically improve performance — omit it to save tokens

**Speakeasy dynamic toolsets:**
- Three-function pattern: `search_tools` (semantic search), `describe_tools` (schema on demand), `execute_tool`
- 96.7% token reduction for simple tasks, 91.2% for complex
- Works consistently from 40 to 400 tools — no degradation with scale
- This is the pattern AIK likely uses internally given their "15 tools" consolidation

---

### 5. Olive's Current Tool Inventory

**Source:** `OliveBlueprintToolHandlers.cpp`, `OliveBTToolHandlers.cpp`, `OlivePCGToolHandlers.cpp`, `OliveCppToolHandlers.cpp`, `OliveCrossSystemToolHandlers.cpp` — grep for RegisterTool calls, Feb 27, 2026

**Total unique tools: 98**

| Domain | Tools | Count |
|--------|-------|-------|
| Blueprint (read) | `blueprint.read`, `blueprint.read_function`, `blueprint.read_event_graph`, `blueprint.read_variables`, `blueprint.read_components`, `blueprint.read_hierarchy`, `blueprint.list_overridable_functions`, `blueprint.get_node_pins` | 8 |
| Blueprint (structure) | `blueprint.create`, `blueprint.delete`, `blueprint.compile`, `blueprint.set_parent_class`, `blueprint.add_interface`, `blueprint.remove_interface` | 6 |
| Blueprint (variables) | `blueprint.add_variable`, `blueprint.remove_variable`, `blueprint.modify_variable` | 3 |
| Blueprint (components) | `blueprint.add_component`, `blueprint.remove_component`, `blueprint.modify_component`, `blueprint.reparent_component` | 4 |
| Blueprint (functions/events) | `blueprint.add_function`, `blueprint.remove_function`, `blueprint.modify_function_signature`, `blueprint.add_event_dispatcher`, `blueprint.override_function`, `blueprint.add_custom_event` | 6 |
| Blueprint (graph low-level) | `blueprint.add_node`, `blueprint.remove_node`, `blueprint.connect_pins`, `blueprint.disconnect_pins`, `blueprint.set_node_property`, `blueprint.set_pin_default` | 6 |
| Blueprint (plan JSON) | `blueprint.preview_plan_json`, `blueprint.apply_plan_json` | 2 |
| Blueprint (templates) | `blueprint.create_from_template`, `blueprint.get_template`, `blueprint.list_templates` | 3 |
| Anim Blueprint | `animbp.add_state`, `animbp.add_state_machine`, `animbp.add_transition`, `animbp.set_transition_rule` | 4 |
| Widget Blueprint | `widget.add_widget`, `widget.bind_property`, `widget.remove_widget`, `widget.set_property` | 4 |
| Behavior Tree | `behaviortree.create`, `behaviortree.read`, `behaviortree.add_composite`, `behaviortree.add_task`, `behaviortree.add_decorator`, `behaviortree.add_service`, `behaviortree.remove_node`, `behaviortree.move_node`, `behaviortree.set_node_property`, `behaviortree.set_blackboard` | 10 |
| Blackboard | `blackboard.create`, `blackboard.read`, `blackboard.add_key`, `blackboard.remove_key`, `blackboard.modify_key`, `blackboard.set_parent` | 6 |
| PCG | `pcg.create`, `pcg.read`, `pcg.add_node`, `pcg.remove_node`, `pcg.connect`, `pcg.disconnect`, `pcg.set_settings`, `pcg.add_subgraph`, `pcg.execute` | 9 |
| C++ | `cpp.create_class`, `cpp.read_class`, `cpp.read_header`, `cpp.read_source`, `cpp.read_enum`, `cpp.read_struct`, `cpp.add_function`, `cpp.add_property`, `cpp.modify_source`, `cpp.compile`, `cpp.list_project_classes`, `cpp.list_blueprint_callable`, `cpp.list_overridable` | 13 |
| Cross-system / Project | `project.bulk_read`, `project.batch_write`, `project.get_relevant_context`, `project.index_build`, `project.index_status`, `project.snapshot`, `project.list_snapshots`, `project.rollback`, `project.diff`, `project.create_ai_character`, `project.implement_interface`, `project.move_to_cpp`, `project.refactor_rename`, `olive.get_recipe` | 14 |

---

### 6. Minimal Effective Tool Set for Blueprint Editing

Drawing from flopperam's 15 graph-editing tools, AIK's consolidation philosophy, and Olive's current capabilities:

**Irreducible read tools (3):**
- `blueprint.read` — full Blueprint structure (variables, components, functions, hierarchy). Could fold `read_function`, `read_variables`, `read_components`, `read_hierarchy` into this with a `section` parameter.
- `blueprint.read_event_graph` — graph topology (nodes + connections) is structurally distinct from the above
- `blueprint.get_node_pins` — required before connecting; can't fold into read_event_graph without inflating response size

**Irreducible write — structure (4):**
- `blueprint.create` — create new Blueprint
- `blueprint.compile` — compile is a distinct operation with error output
- `blueprint.add_variable` + `blueprint.modify_variable` (could be merged with upsert semantics)
- `blueprint.add_component` (SCS editing is structurally different from graph editing)

**Irreducible write — graph (2, or 6 granular):**
- **Option A (plan-first, Olive's current approach):** `blueprint.preview_plan_json` + `blueprint.apply_plan_json` — the entire graph is described declaratively; add_node/connect_pins/set_pin_default are internal implementation details the AI never sees
- **Option B (imperative, flopperam approach):** `blueprint.add_node`, `blueprint.remove_node`, `blueprint.connect_pins`, `blueprint.disconnect_pins`, `blueprint.set_node_property`, `blueprint.set_pin_default` — 6 tools

These two options are mutually exclusive philosophies. Option A (Olive's current) is architecturally superior but requires the resolver to work correctly; Option B is simpler but burns 5-10x more tokens per operation.

**Irreducible — templates (2):**
- `blueprint.list_templates` + `blueprint.get_template` — discovery and retrieval. `blueprint.create_from_template` is a convenience that could be folded into `blueprint.create` with a `template_id` param.

**Collapsible or removable:**
- `blueprint.list_overridable_functions` → fold into `blueprint.read` output
- `blueprint.add_interface` / `blueprint.remove_interface` → rarely used; could fold into `blueprint.modify` (hypothetical)
- `blueprint.add_event_dispatcher`, `blueprint.override_function`, `blueprint.add_custom_event` → these are structural, not graph. Could merge into `blueprint.add_function` with a `type` enum param.
- `blueprint.remove_function`, `blueprint.remove_variable`, `blueprint.remove_component` → could consolidate into `blueprint.remove` with a `target_type` param
- `blueprint.set_parent_class` → rare; could fold into `blueprint.create` or `blueprint.modify`
- `blueprint.reparent_component` → niche; fold into `blueprint.modify_component`
- `blueprint.modify_function_signature` → fold into `blueprint.add_function` (upsert) or `blueprint.modify`

**Minimum viable Blueprint-only set (Option A, plan-first):**
1. `blueprint.read` (with section param: all/graph/variables/components)
2. `blueprint.read_event_graph` (or fold into `read` with `section=graph`)
3. `blueprint.get_node_pins`
4. `blueprint.create`
5. `blueprint.compile`
6. `blueprint.add_variable`
7. `blueprint.add_component`
8. `blueprint.add_function` (type param covers custom_event, event_dispatcher, override)
9. `blueprint.preview_plan_json`
10. `blueprint.apply_plan_json`
11. `blueprint.list_templates`
12. `blueprint.get_template`

**That's 12 tools for full Blueprint editing capability.** Flopperam achieves similar coverage with 15 (4 read + 11 write), but their write tools are imperative (add_node, connect_pins, etc.) which requires many more calls per operation.

---

## Recommendations

### On Tool Consolidation

**The 25-35 target is achievable and validated by AIK's v0.5.0 consolidation.** AIK went from 27+ → 15 tools for its full scope (Blueprints + Materials + Animation + VFX + BT/ST + IK + Input + PCG + MetaSounds + Viewport + C++). Olive covers a smaller feature breadth for C++ and a larger depth for Blueprint. A target of 25-30 is realistic.

**The plan-JSON approach (Olive's key differentiator) changes the math.** Competitors using imperative tools need 6 separate Blueprint graph tools (add_node, connect, etc.) plus the AI must call them sequentially — often 20+ tool calls for a simple event graph. Olive's `preview_plan_json` + `apply_plan_json` collapse all of that into 2 calls. This is not just a token savings — it's a structural quality advantage. Do not remove or weaken the plan-JSON path.

**Claude Code's MCP Tool Search partially mitigates the tool-count problem** (95% token reduction when descriptions exceed 10% of context). However, this does NOT eliminate the tool selection accuracy problem — too many similarly-named tools still causes the model to select wrong tools or hallucinate parameters. Consolidation is still worthwhile for accuracy reasons, not just token reasons.

**Specific consolidation opportunities (low risk):**
- Merge `blueprint.read_function` + `blueprint.read_variables` + `blueprint.read_components` + `blueprint.read_hierarchy` + `blueprint.list_overridable_functions` into `blueprint.read` with a `section` parameter. That's -4 tools immediately.
- Merge `blueprint.add_custom_event` + `blueprint.add_event_dispatcher` + `blueprint.override_function` into `blueprint.add_function` with a `function_type` enum param. That's -2 tools.
- Merge `blueprint.remove_variable` + `blueprint.remove_component` + `blueprint.remove_function` + `blueprint.remove_node` into `blueprint.remove` with a `target_type` param. That's -3 tools.
- Merge `blueprint.create_from_template` into `blueprint.create` with an optional `template_id` param. That's -1 tool.
- Fold `blueprint.set_parent_class` into `blueprint.create` or a new `blueprint.modify` tool. That's -1 tool.

Those 5 changes alone bring Blueprint tools from 38 → ~27. With similar treatment of BT, PCG, C++, the full set could reach 40-50, and with careful cross-domain consolidation, 30-35 is achievable.

**Do not consolidate cross-domain tools prematurely.** The domain prefixes (`blueprint.`, `bt.`, `pcg.`, `cpp.`) provide the AI with scoping context that reduces selection errors. AIK uses a single `edit` tool for all domains because their server is closed — the tool routes internally by detecting what kind of asset it received. This is achievable but requires robust internal routing and is an architect decision, not a research one.

### On Validation Philosophy

AIK's 500+ pre-execution check approach is vindicated by user experience. The alternative (compile-and-check) wastes LLM turns on error correction. Olive's existing validation pipeline is architecturally correct. The open question is whether the 500 checks number represents a target to pursue — inferred from source: this likely includes many asset-type-specific checks, many of which Olive handles differently via the write pipeline's validate/verify stages.

### On Pin Name Resolution

flopperam requires exact pin names — the AI must already know them or inspect the graph first. gimmeDG's implementation is strictly better: 3-pass resolution (exact → case-insensitive → first-data-output fallback) plus explicit wildcard handling. Olive's plan-JSON approach sidesteps this entirely (the resolver handles pin matching internally), which is the correct long-term direction.

### On the MCP Spec

SEP-1686 (async tasks) will NOT be in the spec before June 2026. Current compile-and-wait behavior is correct. The elicitation feature (in `2025-06-18`) is worth noting — it enables a server to ask the user a question mid-operation, which could replace Olive's confirmation tier system with a more natural flow. This is a future design consideration, not an immediate action.

### On Competitor Differentiation

No competitor is doing what Olive does with plan-JSON + resolver + phase validation. flopperam, chongdashu, and gimmeDG all use imperative node-by-node editing. AIK likely uses a similar approach internally. The plan-JSON + template system is a genuine differentiation — the 60-90% token reduction cited in NodeToCode research applies here. Protect and improve this path rather than adding imperative fallback tools that dilute the message.

The main gap vs AIK is breadth: AIK covers Materials, Animation, Niagara, Sequencer, IK Rigs, MetaSounds. Olive covers Blueprint deeply + BT + PCG + C++. These are different products targeting different user segments.

---

## Sources

- [Agent Integration Kit homepage](https://aik.betide.studio/)
- [AIK changelog](https://aik.betide.studio/changelog)
- [AIK comparison page](https://aik.betide.studio/comparison)
- [flopperam/unreal-engine-mcp README](https://github.com/flopperam/unreal-engine-mcp/blob/main/README.md)
- [flopperam blueprint-graph-guide.md](https://github.com/flopperam/unreal-engine-mcp/blob/main/Guides/blueprint-graph-guide.md)
- [DeepWiki: flopperam/unreal-engine-mcp](https://deepwiki.com/flopperam/unreal-engine-mcp)
- [DeepWiki: gimmeDG node graph system](https://deepwiki.com/gimmeDG/UnrealEngine5-mcp/3.2.2-node-graph-system)
- [chongdashu/unreal-mcp](https://github.com/chongdashu/unreal-mcp)
- [chongdashu blueprint tools doc](https://github.com/chongdashu/unreal-mcp/blob/main/Docs/Tools/blueprint_tools.md)
- [mirno-ehf/ue5-mcp](https://github.com/mirno-ehf/ue5-mcp)
- [CLAUDIUS Epic Forum thread](https://forums.unrealengine.com/t/claudius-code-claudius-ai-powered-editor-automation-framework/2689084)
- [Aura Epic Forum thread](https://forums.unrealengine.com/t/aura-ai-agent-for-unreal-editor/2689209)
- [Anthropic: Code Execution with MCP](https://www.anthropic.com/engineering/code-execution-with-mcp)
- [MCP next version update (async/elicitation)](https://modelcontextprotocol.info/blog/mcp-next-version-update/)
- [Claude Code MCP Tool Search: 95% context reduction](https://claudefa.st/blog/tools/mcp-extensions/mcp-tool-search)
- [Speakeasy: 96% token reduction with dynamic toolsets](https://www.speakeasy.com/blog/how-we-reduced-token-usage-by-100x-dynamic-toolsets-v2)
- [Lunar: MCP tool overload numbers](https://www.lunar.dev/post/why-is-there-mcp-tool-overload-and-how-to-solve-it-for-your-ai-agents)
- [arXiv 2602.14878: MCP Tool Description Quality](https://arxiv.org/html/2602.14878v1)
- [Philip Conrod AIK review](https://www.philipconrod.com/co-developing-video-games-using-the-latest-version-of-the-neostack-ai-unreal-game-engine-plugin-using-multiple-llms-via-openroads/)
- [The MCP Tool Trap (Jentic)](https://jentic.com/blog/the-mcp-tool-trap)
- [Unreal University: AI Blueprint Generation](https://www.unreal-university.blog/this-ai-can-now-generate-blueprints-in-unreal-engine/)
