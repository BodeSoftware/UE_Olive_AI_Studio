# Research: Aura (RamenVR) Deep Dive + Full UE AI Competitor Landscape

## Question
Deep analysis of Aura by RamenVR: architecture, blueprint generation approach, UX patterns, pricing, limitations, and what they do well. Plus a full sweep of every other UE AI competitor and relevant patterns from general AI coding tools.

---

## Findings

### 1. Aura (RamenVR / tryaura.dev) — Deep Dive

#### 1.1 Company and Product Timeline

Ramen VR is the developer. The product is at tryaura.dev and branded as "Aura the Unreal AI Agent."

Timeline:
- November 2024: Telos announced — "proprietary reasoning framework developed by MIT-trained AI researchers," 15–30x faster Blueprint question answering than competitors in internal benchmarks.
- January 2, 2026: Public launch. Initially invite-only; all new users get a 2-week free trial with $40 credit.
- January 29, 2026: Moved from soft launch to public beta (open signup).
- February 2026: Aura 12.0 Beta ships. Dragon Agent, Telos 2.0, animation/rigging tools, Claude Code integration officially documented.

Real-world validation: Sinn Studio (VR) shipped "Zombonks" in early access in 5 months, claiming Aura halved production time.

#### 1.2 Architecture — How It Communicates with Unreal Engine

**Three-layer stack:**

1. **Plugin (per-engine-version):** Installed via a dedicated installer for each UE version (5.3–5.7). Enabled via Edit > Plugins. Hosts a **local Aura server** that runs alongside the editor process.
2. **Local server:** Bridges the Aura cloud/model backend with the UE editor. This is where MCP tool calls arrive and get dispatched.
3. **Cloud backend:** Model inference (Sonnet 4.6 and others) runs remotely. Requests go up to cloud, results come back through local server to editor.

**MCP Integration:**
Aura exposes its tools via MCP (Model Context Protocol), allowing external agents (Claude Code, Cursor, VS Code) to call Aura's UE tools directly. This is documented as Alpha-stage functionality. Basic asset inspection is included with all subscriptions; subagent calls (Blueprint edits, Python execution) consume credits.

**Primary execution mechanism:**
The Dragon Agent (introduced in 12.0) executes complex multi-step editor actions "using Unreal Python" — meaning it calls the `IPythonScriptPlugin` internally to perform graph edits, asset manipulation, and batch operations. This is the same mechanism Olive's `editor.run_python` tool uses. Critically, this means Aura's Agent mode is NOT using a custom C++ K2Node API — it's using Python to call UE's editor automation layer.

**Blueprint-specific agent (Telos/Telos 2.0):**
Telos is a specialized sub-agent for Blueprint graph construction. Whether Telos uses Python or has its own C++ tooling is not publicly disclosed. Given that the Dragon Agent explicitly uses "Unreal Python," it is likely that Telos also routes through Python or a similar scripting layer rather than direct K2Node API calls. This is inferred from public statements — not confirmed.

**Headless mode:** Dragon Agent can compile C++ and reopen UE projects without the editor being open, implying a separate process that launches and closes UE as needed.

#### 1.3 UX Patterns — Three Modes

Aura has three distinct interaction modes:

| Mode | What It Does | Makes Changes? |
|------|-------------|---------------|
| **Ask** | Explains concepts, answers questions about the project | No |
| **Plan** | Generates a detailed step-by-step markdown plan stored in `/Saved/.Aura/plans/` | No (plans only) |
| **Agent** | Executes: creates/edits Blueprints, runs Python, writes C++, generates 3D assets | Yes |

**Agent mode UX flow:**
1. User prompts in text.
2. Aura "livestreams changes directly in chat" — changes appear incrementally as the agent works.
3. User can choose: Keep Current Changes, Reject, or Review (via Unreal's Blueprint Diff tool).
4. The diff view shows additions in green, removals in red.

**Plan mode insight:** Plans are saved as markdown files in `/Saved/.Aura/plans/`. This means the user can edit, version-control, and re-execute plans. It mirrors what Cursor does with `.cursor/plans/`. This is a UX pattern worth noting — externalized plans are human-readable and inspectable.

**Multi-thread context note:** "Context objects are global across all chat threads" — meaning active project context (indexed assets, class hierarchy) is shared, but chat history is per-thread. Up to ~4 parallel prompts supported.

#### 1.4 Blueprint Generation — Telos 2.0 Capabilities

**What it claims:**
- ">99% accuracy when processing existing blueprint graphs" (Telos 2.0, from 12.0 release notes)
- "16% performance improvement on hard-level benchmarks"
- "25x error rate reduction" vs Telos 1.0
- Handles: custom functions, interfaces, replication, adapts to coding style
- Can construct "entire systems in blueprints at 10x the speed and 1/10th the cost of competitors"

**What was observed in independent testing (Unreal University blog):**
- Generated functional Blueprint logic — "core mechanics typically working correctly"
- BUT: collision box significantly larger than visual mesh (scaling issue requiring manual fix)
- Asset references sometimes broken (spawn node couldn't locate project's enemy projectile asset)
- Performance delays for complex logic structures
- "Suitable for prototyping; needs oversight before production use"

**Workflow recommendation (from Aura's own docs):**
1. Plan first with Ask mode to design structure
2. Use clear, specific prompts with conditions/triggers
3. Break into modular, small chunks — avoid monolithic generation
4. Test immediately after each generation
5. Iterate through dialogue

This is Aura recommending against large single-shot generation — a tacit acknowledgment that large requests are fragile.

#### 1.5 Known Limitation — Existing Blueprint Editing

From the Epic Developer Community Forums thread (confirmed user complaint, January 2026):

> "even with the setting turned on the AI refuses to work on existing blueprints"

The developer acknowledged the bug: "settings sometimes aren't retained" and suggested retrying with a toggled setting in new threads. This was listed as a deal-breaker for at least one potential subscriber.

This is a critical gap. Aura shipped with Blueprint creation working but Blueprint editing on existing assets as a known-broken or flaky feature. Telos 2.0 claims to fix this with ">99% accuracy on existing blueprints" — but that's a self-reported benchmark from the February 2026 press release.

#### 1.6 Model Support

- **Primary:** Claude Sonnet 4.6 (confirmed from Aura docs — Pro tier is "~200 typical prompts using Sonnet 4.6")
- **Super Mode:** "High Reasoning" model (likely Claude Opus or equivalent) — requires Pro tier
- Multiple models available; specifics not publicly disclosed beyond the above
- No "bring your own key" model — credits go through Aura's backend, which handles model access

#### 1.7 Pricing (as of March 2026)

| Tier | Price | Credits Included | Notes |
|------|-------|-----------------|-------|
| Free Trial | $0 | $40 Aura credit (one-time, 2 weeks) | Unlimited MCP usage |
| Pro | $40/month | $40/month (~200 Sonnet prompts) | "RECOMMENDED" |
| Ultra | $200/month | $280/month | Everything in Pro |
| Enterprise | Custom | Custom | Source access, custom features, onboarding |

**Credit system details:**
- Credits reset monthly — no rollover
- Cost varies by: model selected (Opus > Sonnet > Haiku), context size, subagent usage
- Subagent calls (Blueprint agent, Python execution) consume standard credit amounts
- MCP usage (external agents querying Aura's tools) is "unlimited" — only active Blueprint/execution tool calls cost credits
- Overage purchases available above subscription limit

**Comparison:** NeoStack AIK is $109.99 one-time, no monthly fees, user brings own API keys. Aura is subscription + credit consumption. At $40/month = $480/year vs $109.99 lifetime for AIK.

#### 1.8 Technical Depth — What Telos Actually Is

From the Telos announcement press release:
- Described as a "proprietary reasoning framework" — NOT a fine-tuned model
- Built by "MIT-trained AI researchers"
- Claims: 43 Unreal-native tools (as of the Telos announcement, pre-12.0)
- No architectural disclosure: training methodology, model base, or internal routing are not public
- The 15–30x speed claim was measured against "competing AI agents" across 30 trials — methodology not disclosed

**Best inference from public evidence:** Telos is likely a specialized prompt-engineering + tool-calling framework built on top of a foundation model (Claude or similar), tuned specifically for UE Blueprint semantics, with a curated tool set for editor automation. Not a custom-trained model — a purpose-built agent system. This is architecturally similar to what Olive is building, but with more investment in the Blueprint-specific tooling layer.

#### 1.9 Windows Only

Documented explicitly: Windows only. Mac support stated as a "future goal."

---

### 2. Other UE AI Competitors

#### 2.1 Ultimate Engine Co-Pilot (formerly Ultimate Blueprint Generator)

**Source:** FAB marketplace, Epic forums thread (3 pages), Digital Journal PR article

**Architecture:**
- UE plugin with full C++ source code included (one-time license)
- Connects to external AI providers via API: OpenAI, Google Gemini, Claude (Anthropic), DeepSeek
- Supports MCP integration with desktop AI clients: Claude Desktop, Cursor
- Local LLM support via Ollama and LM Studio (experimental)
- Converts Blueprint logic to JSON format for AI processing — the AI receives a JSON representation of the graph, reasons about it, then the plugin converts the response back to Blueprint nodes

**Blueprint capabilities:**
- Generate Blueprint node sequences from natural language
- Add logic after a selected node ("select any node and add new logic seamlessly after it")
- Blueprint explanation/analysis (screenshot-based: analyzes the graph visually)
- Generate multiple functions/events from a single prompt
- Widget Blueprints, Animation Blueprint Event Graphs, material assignments
- Project-wide scanning (with token limit caveats)

**Known bugs:**
- `generate_blueprint_logic` tool reported completely broken by one user (produced success message but zero nodes). Root cause: Python installation issue on Windows after a Windows update. Fixed by reinstalling Python.

**Pricing:**
- One-time purchase on FAB, no subscription. "No subscriptions. Ever."
- Price not listed in current research (increases as features are added)
- Source code included for personal use and modification
- Per-seat licensing — no redistribution

**User feedback:**
- Positive: "10X productivity improvement," praised node generation + widget placement
- Negative: GameplayTags support missing (roadmap), project-wide scanning hits token limits, no trial version
- Developer is "active 16+ hours daily" on Discord — FAB forum is slow, Discord is the real support channel
- One user: "able to configure Claude Desktop integration in 15-20 minutes"

**Differentiator:** One-time pricing model is a strong differentiator vs subscription competitors. Source code access for modification is unusual.

#### 2.2 Kibibyte Labs — Blueprint Generator AI (Engine Assistant)

**Source:** FAB listing, Epic forum thread

**Architecture:**
- Pure interface plugin — does NOT host or include any AI models
- Sends user prompts to external AI service (OpenAI, Gemini, Anthropic, xAI, DeepSeek)
- Processes AI responses inside UE to generate actual Blueprint nodes (not text output)

**What it generates:**
- Blueprint functions (the core feature)
- Enums and Structs as new assets
- "Pinpoint Smart Search" — searches nodes for specific logic blocks
- Conversational chat for general UE questions

**Limitations reported:**
- Crashes and "prompt screen bugs out with nothing executed" (beta quality)
- Not recommended for non-coders without Blueprint experience
- Oculus-VR fork incompatibility (Fatal Error 25)
- Cannot yet generate complete Actor or Widget Blueprints (only functions)

**Quality note:** One user confirmed it generates actual working nodes, not text — "better than ChatGPT for avoiding Unreal-specific errors like branches in pure functions." The UE-specific validation layer adds value over raw ChatGPT.

**Pricing:** Available on FAB. Cost-efficient claim: ~3,300 function generations per $1 with OpenAI API.

#### 2.3 ClaudeAI Plugin (claudeaiplugin.com)

**Architecture:**
- In-editor plugin, requires Claude API key from Anthropic (user-supplied)
- Operates as an in-editor assistant directly

**Stated capabilities (marketing-level, no independent verification):**
- Blueprint Assistant: logic generation, analysis, optimization, node debugging (5 dedicated functions)
- Game Mode architecture (8 functions)
- 3D asset optimization (6 functions)
- UI/UMG design (12 functions)
- AI Behavior Trees (10 functions)
- Level Generation (3 functions)

**Technical assessment:** This appears to be a thin wrapper that sends UE context + user prompts to the Claude API and displays results in-editor. It does not appear to have a custom blueprint graph editing layer — it likely generates instructions or text that the user applies manually, or uses a basic node-generation approach.

**Pricing:** Not disclosed on the website. Available via Unreal Marketplace / FAB.

**Limitation:** Requires Windows 64-bit, UE 5.0+. No offline functionality.

#### 2.4 Ludus AI (ludusengine.com)

**Architecture:**
- Multi-component system: web app, IDE extension (VS, Rider, VSCode), and UE plugin
- Blueprint module in "Open Beta" as of July 29, 2025
- Self-hosted/enterprise option available

**Modules:**
- **LudusDocs:** LLM Q&A on UE5 docs
- **LudusChat:** Text-to-scene (describe → 3D model/texture generated in engine)
- **LudusBlueprint:** Blueprint analysis + generation (current status: Actor, Pawn, GameMode, LevelBP, EditorUtility, FunctionLibrary; partial Widget UMG; planned: Materials, Niagara, BT, ControlRig, MetaSound)
- **LudusCode:** C++ assistant (fine-tuned for UE macros/memory)

**User experience (forum reports):**
- Hallucination problem: "creates convincing but incorrect solutions when topics fall outside training data"
- Over-engineering: produces unnecessarily complex implementations
- Recommended usage: break problems into small discrete tasks, use for boilerplate only
- "You need 8–12 months of experience in the engine just to use the AI tools well" (user comment)

**Real user verdict:** "AI can be helpful but often times they just spit out gibberish" (from forum post about Ludus). This aligns with the over-engineering pattern.

**Pricing:** Free tier + paid tiers (specifics not disclosed). Enterprise = self-hosted option.

**Key differentiator vs others:** VS Code / Rider integration means it works outside the editor too. The LudusChat scene-generation (text → 3D model placed in level) is unique.

#### 2.5 Open-Source UE MCP Projects

These are the active open-source MCP server projects (as of March 2026), augmenting the existing `competitive-tool-analysis.md` coverage:

**mirno-ehf/ue5-mcp:**
- Stack: TypeScript + C++ (59% C++)
- UE plugin hosts local HTTP server; MCP wrapper in TypeScript bridges to AI clients
- Blueprint, material, and AnimBlueprint support claimed
- v1.0.0 shipped Feb 14, 2026; 48 commits on main
- Notable: can run headless (editor process spawned when needed)

**chongdashu/unreal-mcp:**
- Stack: C++ + Python (TCP on port 55557)
- Blueprint node operations: add event nodes, create function call nodes, connect nodes, create variables, create component/self references, compile
- 1,500+ GitHub stars — most-starred open UE MCP project
- Marked EXPERIMENTAL, breaking changes without notice
- Gap: no evidence of pin-connection primitives beyond basic function call nodes

**runeape-sats/unreal-mcp:**
- "Early alpha preview" — minimal maturity

**GenOrca/unreal-mcp:**
- Supports Python AND C++ for custom tool development
- MCP for Claude & AI agents

**kvick-games/UnrealMCP:**
- "Allow AI agents to control Unreal"
- Minimal public detail

**Assessment of open-source landscape:** None of these projects reach the quality bar of AIK or Aura for actual Blueprint graph editing. They are research/hobbyist tools. The C++ + Python architecture pattern (TCP socket bridge) is now a de facto standard for the open-source implementations.

#### 2.6 UnrealGenAISupport (prajwalshettydev/UnrealGenAISupport on GitHub)

**Architecture:**
- Open-source plugin for LLM API integration + MCP UE5 server
- Supports: OpenAI GPT, DeepSeek, Claude Sonnet 4.5, Gemini, Qwen, Kimi, Grok
- "Automatic scene generation from AI"
- Free, targeting FAB marketplace release

**Assessment:** API wrapper + scene generation. No evidence of blueprint graph editing capability. More of an LLM API integration sample.

#### 2.7 Epic's Own AI Assistant (UE 5.7)

Unreal Engine 5.7 ships with a built-in AI assistant: a slide-out panel for asking questions, generating C++ code, and step-by-step guidance. Based on the description this appears to be a Q&A assistant, not an agent that modifies assets. No Blueprint graph editing mentioned. This is a direct threat to the "ask" use case of every competitor including Olive, but not to the "agent" use case.

---

### 3. AI Coding Tool Patterns Relevant to UE Context

#### 3.1 Cursor Agent Mode — Relevant Patterns

**Context injection strategy:**
- Agents discover context autonomously via grep + semantic search — not manual file tagging
- `@Past Chats` reference for session continuity without duplicating history
- Static context via `.cursor/rules/` files (analogous to Olive's system prompt knowledge injection)
- Reusable workflows via `.cursor/commands/SKILL.md` — these are agent-invokable macros

**Plan Mode (Shift+Tab):**
- Agents research codebase first, ask clarifying questions, then create implementation plans
- Plans are editable markdown stored in `.cursor/plans/`
- Reverting and refining plans outperforms iterating through failed implementations (University of Chicago study cited)
- Planning model can differ from execution model (fast model for planning, powerful for execution)

**Self-correction:**
- Verifiable targets work best: write tests first, confirm failures, implement incrementally
- Debug Mode: generate hypotheses → instrument with logging → collect runtime data → targeted fix
- Hooks (`.cursor/hooks.json`): scripts running before/after agent actions for extended autonomy

**Parallel execution:**
- Up to 8 agents simultaneously via Git worktree isolation
- Cursor evaluates all parallel runs and recommends the best solution
- Competing plans generated simultaneously, best selected after all finish

**Relevance to Olive:** The Plan Mode pattern (editable markdown plans in a project folder) is exactly what Aura copied. Olive has `plans/` folder conventions but they are human-authored, not agent-generated. The hooks pattern (pre/post agent action scripts) is analogous to Olive's write pipeline stages.

#### 3.2 Windsurf Cascade — Relevant Patterns

**Planning architecture:**
- Background "planning agent" continuously refines the long-term plan while the "execution agent" handles immediate actions — two simultaneous model instances with different responsibilities
- "Megaplan" mode: asks clarifying questions to build comprehensive aligned plans before any execution
- Flows: multi-step reasoning chains with discrete steps shown before execution

**Context for large codebases:**
- Automatic context discovery without manual file tagging
- Designed for monorepos and multi-module projects
- Checkpoints set during multi-file edits to preserve project integrity

**Relevance to Olive:** The two-agent planning/execution split is interesting. Olive's Brain layer could benefit from an explicit "planner" sub-turn before the "executor" turn — currently planning and execution are conflated in the worker conversation.

#### 3.3 Aider — Self-Correction Loop Mechanics

**Pattern:**
1. Aider makes a code change
2. Automatically runs configured linter/test command
3. If command exits non-zero: feeds the full error output (stdout + stderr) to the LLM in the next turn
4. LLM receives: error output + context of what was changed + request to fix
5. Repeat until exit 0 or user interrupt

**What's NOT documented (confirmed gap in Aider docs):**
- Maximum retry count before halting
- Infinite loop prevention (there is a known GitHub issue: "if aider is unable to fix lint error, it will loop forever without adding or changing code")
- Backoff strategy

**Relevance to Olive:** Aider's loop has the same infinite-loop problem Olive solved with `FOliveLoopDetector` and `PreviousPlanHashes`. The key lesson: compile-error self-correction MUST have a retry limit and plan-deduplication hash check. Olive's 3-attempt progressive disclosure (terse → full → escalation) is architecturally superior to Aider's unlimited loop.

#### 3.4 Visual Graph Editing — AI Patterns

No major AI coding tool has solved visual graph editing well. The general landscape:

- **Flowise / LangFlow / n8n:** These are AI agent builder tools that are themselves node graphs. They use "natural language to flow conversion" — describe a workflow, get a node graph. But these are AI-native node graphs designed for LLM processing, not UE Blueprint semantics.
- **No established pattern** for LLM-driven editing of visual programming environments like UE Blueprints, Unreal Material graphs, or Niagara.
- The fundamental challenge: Blueprints lack a text format LLMs naturally process. Every solution (Olive's plan-JSON, flopperam's node-by-node, Aura's Python execution) is a workaround for this.

**The three approaches in use:**
1. **Declarative intermediate representation (Olive's plan-JSON):** LLM describes intent → resolver converts to K2Node API calls → one-shot execution with validation
2. **Imperative tool calls (flopperam, chongdashu):** LLM calls add_node, connect_pins, etc. sequentially → many round-trips, each with potential failure
3. **Python execution (Aura Dragon Agent, AIK's Python tool):** LLM generates Python script using `unreal.EditorAssetLibrary` / `unreal.AssetToolsHelpers` → Python executes in editor context → one-shot but brittle if script has errors

**Pattern 3 (Python) insight:** Python is the most expressive — it can do anything the editor Python API supports. The downside is that Python scripts that fail partway through leave the asset in a half-edited state. Olive's snapshot-before-Python approach (in `FOlivePythonToolHandlers`) directly addresses this.

---

### 4. Key Gaps and Differentiators Summary

| Feature | Aura | Ultimate Co-Pilot | AIK (NeoStack) | Olive |
|---------|------|-------------------|----------------|-------|
| In-editor chat panel | Yes (via plugin) | Yes | Yes | Yes |
| Agent mode (autonomous) | Yes | Partial | Yes (Claude Code) | Yes (autonomous mode) |
| Plan mode (human-readable) | Yes (markdown in /Saved/.Aura/plans/) | No | No | No |
| Blueprint graph editing | Yes (Telos 2.0) | Yes (JSON→nodes) | Yes (via Claude Code) | Yes (plan-JSON + granular) |
| Edit existing blueprints | Buggy (Jan 2026), claimed fixed (Telos 2.0) | Yes | Yes | Yes |
| Blueprint diff review | Yes (UE Diff tool) | No | No | No |
| MCP server | Yes (alpha) | Yes (Claude Desktop, Cursor) | Yes (primary interface) | Yes |
| External agent integration | Yes (Claude Code) | Yes (Claude Desktop) | Yes (Claude Code primary) | Yes (Claude Code via MCP bridge) |
| Bring your own key | No | Yes | Yes | Yes |
| Pricing model | Subscription + credits | One-time | One-time | N/A (Olive IS the plugin) |
| Windows only | Yes | Yes | Yes | Yes |
| Blueprint Behavior Trees | Not mentioned | Not mentioned | Yes | Yes |
| PCG graph editing | Not mentioned | Not mentioned | Yes | Yes |
| C++ editing | Yes | Not confirmed | Yes | Yes |
| Template system | No | No | No | Yes (factory + library) |
| Compile error self-correction | Yes (claimed) | No | Partial | Yes (3-tier policy) |
| Plan deduplication / loop detection | Unknown | Unknown | Unknown | Yes (PreviousPlanHashes + LoopDetector) |
| Snapshot / rollback | No | No | No | Yes |
| Python execution with auto-snapshot | Yes (Dragon Agent) | No | No | Yes |

---

## Recommendations

**What Aura does that we should study:**

1. **Blueprint Diff review UX.** Aura shows changes as a diff (green/red additions/removals) before the user confirms. This is a UX pattern Olive lacks entirely. The user sees exactly what nodes will be added/changed before applying. This is different from our "preview_plan_json" (which shows a plan description) — Aura appears to show an actual node-level diff. Even if we can't match this exactly, showing a structured summary of changes before apply is worth investing in.

2. **Plan Mode with externalized markdown.** Storing plans as human-readable markdown in `/Saved/.Aura/plans/` makes plans inspectable, version-controllable, and editable. Olive's plan-JSON is machine-readable JSON, not human-readable markdown. A parallel "explain what you're about to do in prose" step before execution would improve user confidence and debuggability. Cursor does the same thing.

3. **Mode switching (Ask / Plan / Agent) is clean UX.** Users understand the distinction between asking a question (no changes), seeing a plan (no changes), and running an agent (changes happen). Olive conflates these into a single conversation flow. An explicit mode indicator in the chat panel would reduce user anxiety about when changes happen.

4. **Claude Code as a first-class external agent via MCP.** Aura supports Claude Code calling Aura's tools via MCP. Olive already does this (the MCP bridge), but Aura markets it explicitly as a use case. The "external agent + in-editor tool server" pattern is validated by both Aura and AIK as the winning architecture.

**What Aura does badly that Olive should not replicate:**

1. **The "refuses to work on existing blueprints" bug** was a day-one regression that nearly killed user trust. Aura let a clearly broken feature ship. Olive's write pipeline stage 1 (Validate) and the resolver's `BlueprintHasVariable` checks are the right approach — validate before even attempting the edit.

2. **Credit-based pricing creates friction every session.** Users think about cost per prompt. The "how many credits does this use?" question destroys flow. AIK's one-time pricing is psychologically cleaner.

3. **Proprietary "Telos" branding creates expectation management problems.** When Telos fails, users feel deceived by the marketing. Olive should be honest about what the plan-JSON resolver can and cannot do rather than inventing proprietary names.

4. **Windows-only with per-engine-version installer** is a deployment friction point. Olive's approach (single plugin enabled via Edit > Plugins) is much simpler.

**What to watch:**

- Aura's multi-agent architecture (Dragon Agent + Telos + Animation agents) is a preview of where all tools are heading: specialized sub-agents for different domains, coordinated by an orchestrator. Olive's subagent system (architect, coder, researcher) is a development-time version of this pattern, not a runtime one.
- The explicit Claude Code integration by Aura (v12.0, February 2026) confirms that Claude Code as an external agent calling into a UE MCP server is the dominant architecture pattern for power users.
- The "plans stored as markdown" pattern (Cursor + Aura) is converging on a norm. Worth adding to Olive's design as a first-class concept.

---

## Sources

- [Aura homepage](https://www.tryaura.dev/)
- [Aura documentation](https://www.tryaura.dev/documentation/)
- [Aura about/pricing page](https://www.tryaura.dev/about/)
- [Aura Blueprint generation guide](https://www.tryaura.dev/updates/unreal-ai-agent-blueprints)
- [Aura public beta announcement](https://www.tryaura.dev/updates/aura-unreal-engine-ai-assistant-public-beta)
- [Aura launch announcement (January 2026)](https://www.prnewswire.com/news-releases/aura-ai-assistant-for-unreal-engine-launches-vr-studio-ships-game-in-half-the-time-with-new-agent-capabilities-302651608.html)
- [Aura 12.0 Beta announcement](https://markets.financialcontent.com/wral/article/bizwire-2026-3-2-next-evolution-of-best-in-class-multi-agent-ai-assistant-for-unreal-engine-releases-with-aura-120-beta)
- [Aura 12.0 technical details (BriefGlance)](https://briefglance.com/articles/aura-120-redefines-unreal-engine-workflow-with-autonomous-ai)
- [Ramen VR Telos announcement (PRNewswire)](https://www.prnewswire.com/news-releases/ramen-vr-introduces-telos-the-breakthrough-ai-agent-for-unreal-blueprints-302561368.html)
- [Aura Epic Forum thread](https://forums.unrealengine.com/t/aura-ai-agent-for-unreal-editor/2689209)
- [RamenVR studio page](https://www.ramenvr.com/our-work/aura-the-unreal-ai-agent)
- [Unreal University: Aura AI assistant review](https://www.unreal-university.blog/aura-the-ai-assistant-for-unreal-engine/)
- [Unreal University: Blueprint generation deep dive](https://www.unreal-university.blog/this-ai-can-now-generate-blueprints-in-unreal-engine/)
- [Top3D AI: Aura workflow article](https://www.top3d.ai/learn/aura-unreal-engine-no-code-workflow)
- [Ultimate Engine Co-Pilot Forum thread](https://forums.unrealengine.com/t/ultimate-engine-co-pilot-formerly-ultimate-blueprint-generator-the-ai-co-pilot-for-unreal-engine/2618922)
- [Ultimate Engine Co-Pilot FAB listing](https://www.fab.com/listings/8d776721-5da3-44ce-b7ef-be17a023be59)
- [Ultimate Engine Co-Pilot PR article](https://www.digitaljournal.com/pr/news/indnewswire/ultimate-ai-copilot-unreal-engine-1228403129.html)
- [Kibibyte Blueprint Generator AI Forum](https://forums.unrealengine.com/t/kibibyte-blueprint-generator-ai-kibibyte-labs-engine-assistant/2510675)
- [Kibibyte FAB listing](https://www.fab.com/listings/6aa00d98-0db0-4f13-8950-e21f0a0eda2c)
- [ClaudeAI Plugin](https://claudeaiplugin.com/)
- [Ludus AI](https://ludusengine.com/)
- [Ludus AI SmythOS review](https://smythos.com/ai-trends/ludus-ai-in-unreal-engine/)
- [Ludus AI Forum thread](https://forums.unrealengine.com/t/so-i-am-trying-ludus-ai-out-but/2463994)
- [mirno-ehf/ue5-mcp GitHub](https://github.com/mirno-ehf/ue5-mcp)
- [chongdashu/unreal-mcp GitHub](https://github.com/chongdashu/unreal-mcp)
- [flopperam/unreal-engine-mcp GitHub](https://github.com/flopperam/unreal-engine-mcp)
- [UE AI tools forum list](https://forums.unrealengine.com/t/ai-tools-list/2283839)
- [Cursor agent best practices](https://cursor.com/blog/agent-best-practices)
- [Windsurf Cascade docs](https://docs.windsurf.com/windsurf/cascade/cascade)
- [Aider lint/test self-correction docs](https://aider.chat/docs/usage/lint-test.html)
- [Aider infinite loop issue](https://github.com/paul-gauthier/aider/issues/1090)
