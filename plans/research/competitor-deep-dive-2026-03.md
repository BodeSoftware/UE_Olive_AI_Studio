# Research: Competitor Deep Dive — March 2026

## Question

Deep investigation of NeoStack AIK (Betide Studio), Aura (RamenVR), Ultimate Engine Co-Pilot, and Ludus AI.
Covers: agent communication architecture, prompt architecture, pre-execution validation, error recovery, asset coverage, multi-asset task handling, agent loop, changelog velocity, user sentiment, pricing, and model support.

NOTE: The companion file `competitive-tool-analysis.md` (Feb 2026) covers tool consolidation philosophy, MCP spec status, open-source competitors (flopperam, chongdashu, gimmeDG), and Olive's own tool inventory. This file does NOT repeat that material — it goes deeper on the four commercial products.

---

## Findings

### 1. NeoStack AIK (Agent Integration Kit) by Betide Studio

#### 1.1 Version History and Changelog Velocity

Source: [aik.betide.studio/changelog](https://aik.betide.studio/changelog), [assetsue.com AIK listing](https://assetsue.com/file/agent-integration-kit-neostack-ai), [Philip Conrod review](https://www.philipconrod.com/co-developing-video-games-using-the-latest-version-of-the-neostack-ai-unreal-game-engine-plugin-using-multiple-llms-via-openrouter/)

| Version | Date | Key Changes |
|---------|------|-------------|
| v0.5.6 | Feb 21, 2026 | In-process Claude installer, Bun lockfile recovery, macOS quarantine/chmod fix |
| v0.5.5 | Feb 18, 2026 | UE 5.5 stability improvements for rigging/VFX, Copilot CLI + Gemini CLI reliability, IME input fix |
| **v0.5.0** | **Feb 16, 2026** | **UI overhaul, onboarding wizard, session sidebar. Consolidated 27+ tools → ~15 unified tools. Viewport camera control + screenshot capture. Zero-setup install with bundled ACP adapters (Win/Mac/Linux). Conversation handoff between agents. EQS asset support. Crash protection wraps all tool execution.** |
| v0.4.0 | Feb 10, 2026 | Complete animation system: BlendSpace, AnimSequence, Skeleton editing, Physics Assets, ragdoll automation, Linked Anim Layers |
| v0.3.2 | Feb 9, 2026 | Full Control Rig, rewritten Behavior Tree tools, 3D model generation |
| v0.3.1 | Feb 9, 2026 | Session resume fixed ("no need to tell the agent what you already have"), task notifications (toast/audio), Enhanced Input support |
| v0.3.0 | Feb 8, 2026 | "Almost 500+ additional checks added to reduce crashes." Composite Graphs, Comments, Macros, Local Variables, Component editing, Reparenting, Blueprint Interfaces |
| v0.2.0 | Feb 7, 2026 | Animation Montage, Enhanced Input, Viewport screenshot |
| v0.1.x | Jan 20–Feb 6, 2026 | Initial FAB release, progressive feature additions |

Observation: AIK shipped at minimum one release per day in February 2026. The 500+ crash-prevention checks were added all at once in v0.3.0 — implying they were written reactively after a launch plagued by crashes, not designed upfront.

#### 1.2 Agent Communication Architecture — ACP vs MCP

Source: [AIK Gemini CLI docs](https://aik.betide.studio/agents/gemini-cli), [AIK Codex docs](https://aik.betide.studio/agents/codex), [betide.studio/neostack](https://betide.studio/neostack), [Zed ACP spec](https://zed.dev/acp)

AIK uses **two parallel transport layers running simultaneously on the same server**:

**Transport 1: ACP (Agent Client Protocol)** — for Claude Code, Gemini CLI, Codex, Cursor, GitHub Copilot
- ACP is an open protocol proposed by Zed Industries (Apache licensed), originally designed to connect coding agents to editors via JSON-RPC
- The ACP adapter binary is **bundled with the plugin** — users do not install `@zed-industries/claude-code-acp` manually; it ships pre-packaged
- ACP provides: multi-file editing, full codebase context, code reviewing tools
- Lifecycle: `session/initialize` → `session/new` → `session/prompt` → tool calls → results

**Transport 2: MCP (Model Context Protocol) over SSE** — for Gemini CLI, Cursor, external agents
- Server runs on **port 9315** by default
- Primary endpoint: `GET /sse` (Server-Sent Events)
- Gemini CLI config: `gemini mcp add unreal-editor http://localhost:9315/sse --transport sse`
- MCP protocol version: likely `2024-11-05` (inferred from Gemini CLI SSE compatibility requirement)
- OpenRouter uses a **native built-in client** — no external agent process needed, just an API key

**Key insight:** The ACP and MCP paths are NOT the same thing. Claude Code and Codex connect via ACP (which provides editor-level integration including file context); Gemini CLI primarily uses MCP/SSE. The native OpenRouter path bypasses both and drives the plugin directly from a chat window inside UE.

The server auto-starts when the Unreal Editor launches with the plugin enabled. It writes a `.mcp.json` equivalent to the filesystem for tool discovery, but the exact file format is not publicly documented.

#### 1.3 Tool Architecture (v0.5.0+)

Source: [betide.studio/neostack](https://betide.studio/neostack), [AIK comparison page](https://aik.betide.studio/comparison)

Current tool count: **~15 unified tools** (consolidated from 27+ in v0.4.x and earlier).
Documented broad tool categories from Gemini CLI docs:
- `edit_rigging`
- `edit_animation_asset`
- `edit_character_asset`
- `edit_ai_tree`
- `edit_graph`
- `generate_asset`
- `read_asset` (confirmed as a safe read-only tool used for verification)

The exact 15 tool names and their schemas are **proprietary and not publicly documented**. AIK's philosophy (inferred from v0.5.0 release notes) is "intelligent single tools that figure out what you mean automatically" — meaning each tool likely has broad asset-type detection internally rather than separate tools per asset type.

Coverage claims (150+ distinct operations across the ~15 tools):
- Blueprint (variables, components, functions, events, macros, interfaces, SCS nodes, full graph editing)
- Animation (state machines, transitions, conduits, AnimGraph subgraphs, Linked Anim Layers, BlendSpace, AnimSequence, Skeleton, Physics Assets)
- Materials (expressions, parameter nodes, wiring, Material Functions)
- Niagara (emitters, modules, renderers, parameters, dynamic inputs, custom HLSL)
- Sequencer (bindings, tracks, keyframes, camera cuts, audio, bulk transforms)
- Rigging (IK Rigs, Retargeters, solvers, goals, FK/IK chains)
- AI Logic (Behavior Trees, State Trees, tasks, decorators, evaluators)
- Widget Blueprints (UMG widgets, layout panels, event bindings)
- Data (Structs, Enums, DataTables, Data Assets, Chooser Tables with JSON import)
- Generation (text-to-image textures, text-to-3D meshes, Pose Search, Motion Matching, PCG, MetaSounds)
- Utility (Python scripting, viewport screenshots, rendered mesh previews)
- Environment Query System (EQS) — added v0.5.0

Additionally: **36+ asset type readers** — these are @mention-triggered context providers, not write tools. When a user types `@BP_MyActor`, the relevant reader generates a structured description of that asset for injection into the LLM context.

#### 1.4 Prompt Architecture and Context Injection

Source: [AIK profiles page](https://aik.betide.studio/profiles), [betide.studio/neostack](https://betide.studio/neostack), inferred from Gemini CLI docs

**Profiles system** — AIK's primary context injection mechanism:
- 5 built-in profiles: Full Toolkit, Animation, Blueprint & Gameplay, Cinematics, VFX & Materials
- Each profile is a whitelist of which tools the agent can see (empty list = all tools; populated list = restricted)
- Profiles also contain **"custom instructions"** — additional text injected into the agent's context for domain-specific guidance
- Individual tool descriptions can be overridden per-profile for domain-specific context
- Profile selection persists across sessions; cannot change during an active agent session
- Profiles are stored as configuration in UE plugin settings (not as CLAUDE.md or AGENTS.md files)

**Automatic project indexing** — mentioned in documentation but not technically described. Inferred to scan the content browser for asset names and types to populate @mention completions.

**@-mention context attachment** — users can attach assets to their prompt by typing `@BP_MyActor`. This triggers the corresponding asset reader, which generates a description of that asset's structure. Right-clicking Blueprint nodes in the editor also allows attaching targeted node context.

**AIK does NOT appear to use CLAUDE.md or AGENTS.md** — the domain knowledge is entirely managed through the Profiles system and the plugin's own context injection. Claude Code natively reads CLAUDE.md, so whatever Claude Code reads is the agent's knowledge — but AIK itself does not seem to generate or manage CLAUDE.md files.

**Session resume** (added v0.3.1): The plugin tracks what the agent has already seen/done in a session, so users don't need to re-brief the agent about prior work. The technical mechanism is not documented (likely conversation history stored in plugin state).

**Conversation handoff** (added v0.5.0): Users can switch between different connected agents mid-conversation. The context carries over across the handoff. Technical mechanism undocumented.

#### 1.5 Pre-Execution Validation ("500+ Checks")

Source: [Philip Conrod review (v0.3.1)](https://www.philipconrod.com/co-developing-video-games-using-the-latest-version-of-the-neostack-ai-unreal-game-engine-plugin-using-multiple-llms-via-openrouter/), [assetsue.com listing](https://assetsue.com/file/agent-integration-kit-neostack-ai)

The 500+ checks were added in v0.3.0 (Feb 8, 2026) specifically to address crash storms at launch.

What the checks are: **Not publicly documented.** The description says "almost 500+ additional checks added to reduce crashes." Given the context (blueprint editing, crash storms at launch), these are most likely:
- Input validation (null checks on asset references before write operations)
- Type safety checks (pin type compatibility before wiring)
- State checks (is the editor in a valid state to accept this operation?)
- Asset existence checks (does the target asset exist before attempting modification?)
- Structural checks (is this graph type compatible with this operation?)

The "500+" number appears to be a count of individual guard conditions in C++, not 500 distinct semantic rules. Inferred from source: similar to how Olive's write pipeline has validation stages — AIK's are likely scattered guard clauses rather than a unified rule registry.

**Crash protection in v0.5.0**: "Crash protection wraps tool execution" — this is a separate addition from the 500 checks, implying try/catch or structured exception handling around tool calls to prevent full editor crashes on unhandled errors.

#### 1.6 Error Recovery

No specific documentation found on AIK's error recovery strategy. What is known:
- Full undo support for all editor operations — users can undo AI-generated changes
- Same-day Discord issue resolution ("most issues reported on Discord resolved same-day")
- The session resume feature (v0.3.1) means the agent knows what it already tried — implicitly enabling retry on different approach

Inferred: AIK relies on the LLM's own retry capability (since Claude Code and Gemini CLI have their own agentic loops) rather than a plugin-level self-correction system.

#### 1.7 Multi-Asset Task Handling

No documented decomposition strategy. AIK relies on the agent (Claude Code, Gemini CLI) to decompose tasks internally. The plugin provides the tools; the agent decides the order. Given that AIK supports 8 concurrent sessions (multi-session chat), it's possible to run parallel agents on different assets, though no documented workflow exists for this.

#### 1.8 Pricing, Model Support, User Sentiment

**Pricing:** One-time $109.99 on Fab marketplace (no subscription, no expiring credits). Betide Studio direct purchase includes a 5% discount.

**Model support:**
- Claude Code (recommended: "delivers best results by far")
- Gemini CLI (free tier via Google AI Studio — "generous free tier")
- OpenRouter (400+ models, native client, API key only)
- Codex CLI (requires ChatGPT subscription or `OPENAI_API_KEY`)
- Cursor (ACP)
- GitHub Copilot CLI (ACP)

**User sentiment:**
- Positive: model flexibility, rapid iteration, full Blueprint coverage, same-day support, one-time price
- Negative: Claude Code dependency for best results ("other models produce less than desired results" for Blueprint logic), beta stability (still labeled Beta), flickering UI bug (in progress at time of research)
- Philip Conrod (Feb 2026, v0.3.1): praised 100% Blueprint coverage and rapid developer responsiveness; plans multi-week testing before full verdict
- Users note "Claude delivers best results by far" — suggests the tool's prompt architecture is optimized for Claude's reasoning style, and alternatives work but underperform

---

### 2. Aura by RamenVR

#### 2.1 Architecture Overview

Source: [tryaura.dev/documentation](https://www.tryaura.dev/documentation/), [tryaura.dev/about](https://www.tryaura.dev/about/), [tryaura.dev/updates/unreal-ai-agent-blueprints](https://www.tryaura.dev/updates/unreal-ai-agent-blueprints), [prnewswire Telos announcement](https://www.prnewswire.com/news-releases/ramen-vr-introduces-telos-the-breakthrough-ai-agent-for-unreal-blueprints-302561368.html), [Aura 12.0 press release](https://markets.financialcontent.com/wral/article/bizwire-2026-3-2-next-evolution-of-best-in-class-multi-agent-ai-assistant-for-unreal-engine-releases-with-aura-120-beta)

Aura is architecturally fundamentally different from AIK. It is:

1. **Hosted service + local plugin** (not a purely local tool)
2. **Subscription-based** (credit consumption, not one-time purchase)
3. **Proprietary model layer** ("Telos") on top of foundation models (confirmed: Claude Sonnet 4.6, Opus 4.6, Haiku)
4. **Multi-agent system** with specialized subagents per domain

**Installation:** Engine-level plugin (not per-project). Users install for a specific UE version (e.g., UE 5.6), and it becomes available to all projects on that version. Adds an "Aura" button to the editor toolbar.

**Local server:** A local server runs alongside the plugin and communicates with the chat interface. Architecture details not disclosed.

**Engine compatibility:** UE 5.3, 5.4, 5.5, 5.6, 5.7

**Platforms:** Windows only (Mac planned)

#### 2.2 Operational Modes — Agent Loop

Source: [tryaura.dev/documentation](https://www.tryaura.dev/documentation/), [tryaura.dev/updates/unreal-ai-agent-blueprints](https://www.tryaura.dev/updates/unreal-ai-agent-blueprints)

Aura has three distinct modes:

**Ask Mode:** Analysis and consultation without making changes. The AI reads the project, answers questions, reviews blueprints for errors ("detecting typos, hanging nodes"), and provides guidance.

**Plan Mode:** Creates a detailed implementation plan stored as markdown files in `/Saved/.Aura/plans`. The plan is shown to the user for review before execution begins. This is Aura's equivalent of Olive's Tier 2 confirmation.

**Agent Mode:** Makes actual modifications. The UI livestreams changes as they happen. After completion, the user can:
- "Keep Current Changes"
- "Reject"
- "Review" (opens Blueprint Diff tool)

This human-in-the-loop review step is prominently documented and recommended. Users are advised to submit prompts using a trigger/condition format and review changes incrementally.

**Coding Agent (subagent):** Creates, edits, and fixes C++ and Blueprint files. Claims real-time self-correction.

#### 2.3 Telos — The Proprietary Reasoning Framework

Source: [prnewswire Telos announcement](https://www.prnewswire.com/news-releases/ramen-vr-introduces-telos-the-breakthrough-ai-agent-for-unreal-blueprints-302561368.html), [Aura 12.0 press release](https://www.gamespress.com/en-US/Next-Evolution-of-Best-In-Class-Multi-agent-AI-Assistant-for-Unreal-En), [briefglance.com Aura 12.0](https://briefglance.com/articles/aura-120-redefines-unreal-engine-workflow-with-autonomous-ai)

Telos is described as "a proprietary reasoning framework developed by MIT-trained AI researchers." Key claims (all from press releases, unverified externally):

- Telos 2.0: "10X the speed and 1/10th the cost of competitors"
- ">99% accuracy when processing existing blueprint graphs"
- "25-fold reduction in error rates" vs Telos 1.0
- "16% performance improvement on challenging benchmarks"
- "Enhanced node search capabilities for custom and project nodes"
- Benchmark: 30 trials across 6 prompts, 5 repetitions each — "15-30x faster than competing AI agents"

What Telos actually is: **Not disclosed.** The phrase "proprietary reasoning framework" could mean:
- A fine-tuned or RLHF-tuned version of a foundation model
- A structured prompting / chain-of-thought system applied on top of Claude/GPT-4
- A custom retrieval-augmented generation system specific to Blueprint semantics
- A hybrid — some fine-tuning + custom prompting

The use of "MIT-trained AI researchers" and performance claims of this magnitude (25x error reduction) suggest fine-tuning or RLHF is involved, but this is inference. RamenVR has not disclosed the architecture.

**Foundation models used:** Confirmed via documentation — Claude Sonnet 4.6, Opus 4.6, Haiku. Telos likely wraps one or more of these with additional orchestration. Credit consumption varies by subagent.

**43 Unreal-native tools** total (Telos 1.0 announcement). The tool list across domains includes Level Design, 3D Modeling, Asset Generation, Code Generation, Blueprint generation, and more.

#### 2.4 Dragon Agent (Aura 12.0, March 2, 2026)

Source: [Aura 12.0 press release](https://markets.financialcontent.com/wral/article/bizwire-2026-3-2-next-evolution-of-best-in-class-multi-agent-ai-assistant-for-unreal-engine-releases-with-aura-120-beta), [briefglance.com](https://briefglance.com/articles/aura-120-redefines-unreal-engine-workflow-with-autonomous-ai)

The Dragon Agent is Aura's new fully autonomous agent, released March 2, 2026 in beta.

**Key capabilities:**
- Executes complex multi-step actions via **Unreal Python**
- "Autonomous agentic loops" — can run without UE being open
- Batch editing actors, detecting unused project assets, enforcing naming conventions
- Autonomously compiles C++ code, closes and reopens UE projects as needed
- Integrates with other AI models (explicitly: Claude Code) to build, test, and iterate
- Tasks that "traditionally consume hours of manual labor" reduced to seconds

**Architecture insight:** The Dragon Agent's use of Unreal Python as its execution layer is significant. Rather than exposing proprietary editor APIs, it leverages UE's existing Python scripting interface — meaning any action a human can do via Python, the Dragon Agent can do. This is equivalent to Olive's `editor.run_python` but fully autonomous.

**Multi-agent integration:** The Dragon Agent can invoke Claude Code. This means Aura is positioning itself as an orchestrator of other AI tools, not a replacement. This is a sophisticated architecture choice.

#### 2.5 MCP Integration

Source: [tryaura.dev/documentation](https://www.tryaura.dev/documentation/), [briefglance.com Aura 12.0](https://briefglance.com/articles/aura-120-redefines-unreal-engine-workflow-with-autonomous-ai)

Aura exposes an MCP server. The documentation references "Aura's MCP tools from external agents" — meaning Claude Code, VS Code, and Cursor can connect to Aura's MCP server to use its tools externally.

Aura also built a dedicated MCP server for an AccelByte backend SDK integration (AccelByte Online Subsystem), reducing backend integration time from one week to approximately one hour. This demonstrates Aura's strategy of building vertical MCP integrations beyond just editor control.

**The MCP implementation details (port, endpoints, tool schema) are not publicly documented.**

#### 2.6 Blueprint Editing — What Works and What Doesn't

Source: [unreal-university.blog Aura review](https://www.unreal-university.blog/this-ai-can-now-generate-blueprints-in-unreal-engine/), [Epic forums thread](https://forums.unrealengine.com/t/aura-ai-agent-for-unreal-editor/2689209), [unreal-university.blog Aura assistant review](https://www.unreal-university.blog/aura-the-ai-assistant-for-unreal-engine/)

**What works:**
- Overlap detection, character casting, launch calculations — generated complete logic
- Telos processes existing blueprint graphs with claimed >99% accuracy
- "Core mechanics typically working correctly"

**Documented failures:**
- "AI sometimes struggles to properly reference existing project assets" — spawn actor nodes can't find custom assets
- Generated blueprints often have "incorrect collision bounds or component scales"
- "Blueprint generation can take considerable time, especially for complex logic"
- Works best with modular requests rather than multi-system integrations
- User complaint from Epic forums: "AI refuses to work on existing blueprints" even with settings enabled — developer acknowledged and this is "many bugs and performance improvements" in progress
- Complexity ceiling: "very complex multi-system integrations may require breaking down requests into smaller, manageable components"

**The "refuses existing blueprints" issue:** This was a reported problem at launch (January 2026). Whether Telos 2.0 / Aura 12.0 resolved it is not confirmed in available sources.

#### 2.7 Pricing and Access Model

Source: [tryaura.dev/about](https://www.tryaura.dev/about/)

Aura uses a subscription + credit model:

| Tier | Price | Credits | Features |
|------|-------|---------|----------|
| Free Trial | $0/mo | $40 Aura credit (one-time) | 2-week access, unlimited MCP |
| Pro | $40/month | $40 Aura credit/mo | 3D model gen, Super Mode (high reasoning), unlimited MCP |
| Ultra | $200/month | $280 Aura credit/mo | Everything in Pro |
| Enterprise | Custom | Custom | Source code access, dedicated onboarding, roadmap priority |

Credits are consumed by agent operations. Telos and specialized subagents consume more credits than standard chat. "Super Mode" implies a high-reasoning model (likely Opus 4.6) costs more credits.

**Training policy:** Aura may train on explicit conversation data but explicitly does NOT use asset or game data for training. Enterprise defaults training to off.

**Access:** Invite-only during early beta; public beta started late January 2026 with $40 free credit. 2-week free trial available at signup.

#### 2.8 User Sentiment

Source: [Epic forums thread](https://forums.unrealengine.com/t/aura-ai-agent-for-unreal-editor/2689209)

**Positive:**
- "Amazing job. Can't wait to check this out." (InfiniteSkyGames)
- "Should save a lot of time to handle Blueprints" (NAT_AI)
- "Think this product will become huge just like Cursor did" (Cute_Lil_Cracker)

**Negative:**
- "AI refuses to work on existing blueprints" despite settings enabled (Cute_Lil_Cracker) — primary complaint
- "When the AI can edit existing blueprints and then you guys have a new subscriber" (feature request framed as blocking conversion)
- Plugin installation issues in UE 5.7 (Russty81)
- Desktop application not launching properly
- Registration requires invitation code — access barrier

**Platform gap:** Windows only; Mac users are excluded. This was confirmed as a known limitation.

---

### 3. Ultimate Engine Co-Pilot (formerly Ultimate Blueprint Generator)

Source: [gamedevcore.com/docs/bpgenerator-ultimate](https://www.gamedevcore.com/docs/bpgenerator-ultimate), [Epic forums thread pages 1-3](https://forums.unrealengine.com/t/ultimate-blueprint-generator-the-ai-co-pilot-for-unreal-engine/2618922), [digitaljournal.com press release](https://www.digitaljournal.com/pr/news/indnewswire/ultimate-ai-copilot-unreal-engine-1228403129.html)

#### 3.1 Architecture

Three execution paths:

1. **Native Editor Integration** (v0.3.5+): Direct plugin UI inside Unreal Editor; AI calls UE APIs directly
2. **Claude Desktop/Cursor MCP**: Python MCP server (`mcp_main_server.py`) bridges to Unreal plugin
3. **Local LLM**: Ollama, LM Studio via OpenAI-compatible `/v1/chat/completions` endpoint

**MCP server:** Python-based. Config points to `mcp_main_server.py` in the plugin installation directory, with environment variables `UNREAL_HOST` and `UNREAL_PORT` for plugin communication. Uses `"unreal-handshake"` and `"filesystem-access"` server names in MCP config.

**Blueprint generation:** The AI generates "complex, multi-node functions and events from a single prompt" and places them in clean layouts. Users can also select an existing node and have the Co-Pilot insert new logic after it — node-level extension of existing graphs.

**Self-validation loop:** Developer added a validation loop that prevents silent failures (AI reports success when zero nodes were created). Previously `generate_blueprint_logic` could produce zero nodes while reporting success.

**Key quote from developer:** "It won't code GTA 6 for you" — positions the tool for task-by-task augmentation, not fully autonomous development.

#### 3.2 Model Support

Providers: OpenAI, Google Gemini (free tier), Claude, DeepSeek, Ollama, LM Studio (local). Any OpenAI-compatible endpoint is supported.

Local LLM support added January 26, 2026. Tested with: Ollama minimax m2, Qwen3 Coder 30B.

#### 3.3 Pricing

One-time purchase with lifetime updates. Price increases as functionality expands. No subscription fees. Users bring their own API keys/provider access.

Current version at time of research: v0.7.5 (March 1, 2026), including full Blueprints, Animation Blueprints, Materials, Enhanced Input, 3D Model Generation, Image Generation, MCP/Claude Desktop integration.

**DMCA disruption:** The plugin was temporarily unavailable on Fab due to a DMCA dispute at some point during development. Estimated 10-14 business day restoration window mentioned in forums.

#### 3.4 User Sentiment

**Positive:**
- "10x workflow improvement" with v0.1.1 update
- Developer responsiveness (quick bug fixes)
- Free Gemini API key option (no cost after one-time plugin purchase)
- Users with basic UE knowledge praise it as reducing intimidation

**Negative:**
- `generate_blueprint_logic` tool created "zero nodes" while reporting success — developer acknowledged this was a bug (now fixed)
- UE 5.7 compatibility issues required fixes
- Python reinstallation required after Windows update broke setup
- Local LLM support "experimental" with undefined stability
- No trial version available — pricing barrier for evaluation
- Full project scanning "consumes excessive tokens" with external APIs

**Unmet requests:**
- GameplayTags support (confirmed as planned)
- Project-wide architecture scanning (developer acknowledged complexity)
- Blueprint correction/addition to existing graphs (confirmed as upcoming)

---

### 4. Ludus AI

Source: [ludusengine.com](https://ludusengine.com/), [smythos.com review](https://smythos.com/ai-trends/ludus-ai-in-unreal-engine/), [Philip Conrod Ludus review](https://www.philipconrod.com/ludus-ai-agent-assisted-game-development-tools-for-unreal-game-engine/), [sourceforge reviews](https://sourceforge.net/software/product/Ludus-AI/)

#### 4.1 Architecture

**Four components:**
- **LudusDocs:** AI trained on UE5 documentation; answers questions about the engine
- **LudusChat:** Text-to-scene generation ("add a sci-fi crate near the player")
- **LudusBlueprint:** Blueprint analysis, explanation, and generation (Open Beta as of July 29, 2025)
- **LudusCode:** C++ assistance with UE-specific knowledge (enterprise tier)

**Integration points:** Plugin inside UE, plus VS Code, JetBrains Rider, Visual Studio extensions. Cloud-based; self-hosted enterprise option.

**Blueprint agent** (Open Beta): Tested successfully on Actor, Pawn, Game Mode, Level Blueprint, Editor Utility, and Function Library types. Widget (UMG) is partial. Material, Niagara, Behaviour Tree, Control Rig, and MetaSound blueprints are **not yet supported**.

**Real-world test (Philip Conrod, 2025):** Upgraded a UE 4.26 RPG project to 5.6, tasked Ludus with identifying and repairing broken blueprints. The agent completed all repairs autonomously in approximately 5 minutes — work that previously took many manual hours. The fixed project ran flawlessly.

#### 4.2 Performance Claims

- Blueprint generation: 210 tokens/second (vs 28-40 for competitors — source: neodocs.betide.studio, so this may be a competitor's marketing claim)
- "89% correct solutions" for UE tasks
- 69% code quality score for production-ready output

#### 4.3 Pricing

- Free: 400 monthly credits
- Indie: Paid tier (price not confirmed)
- Pro: Paid tier (14-day trial with 20,000 test credits)
- Enterprise: Custom, self-hosted option

#### 4.4 Limitations

- Requires 8-12 months of UE experience to use effectively (analysis from smythos.com review)
- Cannot generate production-ready 3D models
- Blueprint code generation was "planned soon" as of the smythos.com article (now in open beta)
- Material, Niagara, BT, Control Rig, MetaSound support not yet available
- Still requires human intervention for debugging, console optimization, gameplay balancing

---

### 5. Open-Source Baseline: chongdashu/unreal-mcp

Source: [github.com/chongdashu/unreal-mcp](https://github.com/chongdashu/unreal-mcp)

Three-layer design:
1. **C++ Plugin (UnrealMCP):** Native TCP server for MCP communication on port 55557. Handles command execution via UE editor subsystems.
2. **Python MCP Server:** Manages TCP socket connections to the C++ plugin via FastMCP library. Loads tools dynamically from a `tools/` directory.
3. **Tool Modules:** Modular Python tools — each file in `tools/` exposes a set of MCP tools.

**Exposed tools (relevant selection):**
- Actor management: create/delete, transform, property queries, level enumeration
- Blueprint development: class creation with components, physics property management, compilation, spawning
- Node graph: event node insertion (BeginPlay, Tick), function call node creation and connection, variable definition
- Editor control: viewport focus, camera adjustment

**Status:** Explicitly EXPERIMENTAL, 1,500 stars, breaking changes expected.

This project represents the floor of what "MCP for Unreal" looks like without significant investment. It is useful as a comparison baseline — Olive's approach is architecturally far more sophisticated.

---

## Competitive Comparison Matrix

| Feature | NeoStack AIK | Aura | Ultimate Co-Pilot | Ludus AI | Olive AI Studio |
|---------|-------------|------|-------------------|----------|----------------|
| **Architecture** | Plugin + ACP/MCP server | Hosted + local plugin + Claude API | Plugin + Python MCP bridge | Cloud service + plugin | Plugin + MCP/ACP server |
| **Transport** | ACP (Claude/Codex/Copilot) + SSE MCP (Gemini) + native (OpenRouter) | Proprietary (local server + Aura cloud) | Native plugin + Python MCP | Cloud REST | HTTP JSON-RPC + ACP |
| **Blueprint graph editing** | Yes (full) | Yes (Telos) | Yes (node-by-node + natural language) | Partial (Beta) | Yes (plan_json + granular) |
| **Materials** | Yes | Unknown | Yes | No | No |
| **Animation** | Full (BlendSpace, Montage, PhysicsAsset, Skeleton, IK) | Via Dragon Agent (Unreal Python) | Animation Blueprint | No | AnimBP (state machines) |
| **Niagara** | Yes | Unknown | No | No | No |
| **Sequencer** | Yes | Unknown | No | No | No |
| **Behavior Tree** | Yes (rewritten v0.3.2) | Unknown | No | No | Yes (full) |
| **PCG** | Yes | Unknown | No | No | Yes |
| **C++ integration** | Yes (live coding) | Yes (Coding Agent, self-correcting) | External file read/write | Yes (enterprise) | Yes (full) |
| **Autonomous mode** | Via connected agent (Claude Code autonomy) | Dragon Agent (fully autonomous) | No | No | Yes (FOliveRunManager) |
| **Plan/Preview** | No explicit plan step | Plan Mode (markdown file) | No | No | Yes (preview_plan_json) |
| **Pre-exec validation** | 500+ checks + crash protection | Unknown | Self-validation loop | Unknown | 6-stage pipeline |
| **Error recovery** | Agent's own loop + undo | Real-time self-correction (Coding Agent claim) | Fixed validation loop | Unknown | FOliveSelfCorrectionPolicy |
| **Template system** | No | No | No | No | Yes (factory/reference/library) |
| **Model support** | Any via OpenRouter; Claude recommended | Claude (Sonnet/Opus/Haiku), Gemini | OpenAI, Gemini, Claude, DeepSeek, local LLMs | Cloud (model unspecified), enterprise self-host | 8 providers |
| **Pricing** | $110 one-time | $40-$200/month subscription | One-time (price rising with features) | $10+/month (free tier) | N/A (editor plugin) |
| **Windows** | Yes | Yes | Yes | Yes | Yes |
| **Mac** | Yes | No (planned) | No | Yes (cloud) | No |
| **UE versions** | 5.5-5.7 | 5.3-5.7 | 5.4-5.7 | 5.1-5.7 | 5.5+ |

---

## What Each Competitor Does Better Than Olive

### NeoStack AIK Does Better:
1. **Breadth of asset types** — Materials, Niagara, Sequencer, IK Rigs, MetaSounds, EQS are all supported. Olive's Blueprint depth is greater, but AIK's coverage is wider.
2. **Zero-setup install** — Bundled ACP adapters, in-process Claude installer, automatic lockfile recovery. Olive requires more manual setup.
3. **Multi-agent session support** — Up to 8 concurrent agent sessions. Olive is single-session.
4. **@-mention asset context** — Users attach specific assets to prompts with `@`. Olive requires more verbose asset path specification.
5. **Profiles system** — Whitelist-based tool restriction with custom instruction injection per profile. Olive has focus profiles but they are less granular.
6. **Conversation handoff** — Switch between connected agents mid-conversation. Olive does not support this.
7. **Velocity** — 1+ releases per day in February 2026. Betide ships extremely fast.

### Aura Does Better:
1. **Telos proprietary reasoning** — If the benchmark claims are accurate, Telos provides superior Blueprint generation accuracy at lower cost than raw API usage. Olive has no equivalent specialized reasoning layer.
2. **Plan Mode with stored plans** — Plans stored as markdown files in `/Saved/.Aura/plans` are reviewable assets, not transient confirmations. Olive's preview is transient.
3. **Dragon Agent fully autonomous operation** — Can run without UE being open, invoke Claude Code, compile C++ autonomously. Olive's autonomous mode requires the editor open.
4. **Real-time self-correction (Coding Agent claim)** — If real, this is in-loop correction without returning to the user. Olive's self-correction triggers a new conversation turn.
5. **Multi-agent specialization** — Separate agents per domain (Blueprints agent, C++ agent, Python agent, art agent). Olive uses one agent with tool packs.
6. **Case study evidence** — Aura has published case studies with real numbers (Sinn Studio: 5x content output, 50% time reduction). Olive has none.

### Ultimate Engine Co-Pilot Does Better:
1. **Scene population** — Spawning hundreds of Static Meshes, Blueprints, or C++ Actors in the viewport from a single prompt. Olive has no level design tooling.
2. **Node insertion into existing graphs** — Select any existing node and insert new logic after it. Olive requires recreating nodes from scratch or full plan replacement.
3. **Free tier** — Gemini free API tier works with the plugin. Olive has no free tier equivalent.

### Ludus AI Does Better:
1. **Documentation AI** — LudusDocs is specifically trained on UE5 docs. Olive has no Q&A documentation assistant.
2. **IDE integration** — Works inside VS Code, Rider, Visual Studio. Olive is editor-only.
3. **Broken blueprint repair** — The 5-minute repair of an entire UE 4.26→5.6 migrated project's broken blueprints is impressive. Olive has no equivalent "diagnose and repair" workflow.

---

## What Olive Does Better Than Each Competitor

### Better Than NeoStack AIK:
1. **Plan-JSON with resolver** — The `preview_plan_json` + `apply_plan_json` cycle with the `FOliveBlueprintPlanResolver` is architecturally superior to tool-by-tool graph editing. 2 calls vs 20+ calls for a complete function. No competitor does this.
2. **Template system** — Factory templates (parameterized), reference templates (architectural guidance), and library templates (325 extracted real Blueprints with full node graphs). No competitor has an equivalent.
3. **FOliveValidationEngine** with structured rule registry vs AIK's scattered guard conditions.
4. **Blueprint depth** — Interface event resolution, component bound events, delegate binding, timeline nodes, all plan ops — Olive's Blueprint coverage is deeper even if AIK's breadth is wider.
5. **Persistent library index** — 325 real Blueprint templates with lazy loading, LRU cache, inheritance resolution, inverted search. No competitor has an extracted real-world Blueprint corpus.
6. **Snapshot/rollback** — One-click project rollback via FOliveSnapshotManager. AIK only has undo.

### Better Than Aura:
1. **No subscription model** — Olive is a plugin, not a hosted service. No credits, no billing, no rate limits based on spending.
2. **Transparent tool layer** — Olive's tool schemas are visible to the architect and configurable. Aura's 43 tools are proprietary black boxes.
3. **Write pipeline** — 6-stage pipeline with structured result types. Aura's error recovery is proprietary.
4. **Open model support** — Olive works with 8 providers including local LLMs. Aura requires Aura-hosted Claude/Gemini.
5. **No data leaves the project** — Olive never sends asset data to external services beyond what the LLM API requires. Aura's cloud processing raises questions even though they claim no asset training.

### Better Than Ultimate Co-Pilot:
1. **Plan-JSON** — Single-call graph creation vs sequential node-by-node generation.
2. **Compilation feedback** — Olive captures per-node compile errors and feeds them back to self-correction. Co-Pilot's validation loop is simpler.
3. **BT and PCG support** — Co-Pilot has neither.

### Better Than Ludus AI:
1. **Complete Blueprint graph editing** — Ludus is still in beta for Blueprint generation; Olive is production-complete.
2. **Behavior Tree and PCG** — Ludus covers neither.
3. **C++ integration at depth** — Olive's C++ toolset (create class, add function, add property, compile, live reload) is more complete than Ludus's basic code assistance.

---

## Patterns Worth Studying or Adopting

### From NeoStack AIK:

**@-mention asset context attachment (HIGH PRIORITY)**
Users type `@BP_MyActor` and the relevant asset reader generates structured context automatically. This is far more ergonomic than requiring users to specify asset paths. Implementation: detect `@` tokens in chat input, resolve to asset paths via `FOliveProjectIndex`, inject asset context automatically.

**In-process bundled setup (MEDIUM)**
AIK bundles ACP adapter binaries directly in the plugin. Zero external dependencies for the user. Olive's bridge (`mcp-bridge.js`) requires Node.js. Moving to a bundled approach or integrating `bun` directly would improve setup UX.

**Crash-protection wrapper around every tool call (MEDIUM)**
AIK's v0.5.0 added try/catch wrapping around all tool execution to prevent full editor crashes. Olive's write pipeline should ensure equivalent protection — tool failures should produce structured error results, never unhandled exceptions.

**Session sidebar (LOW)**
AIK v0.5.0 added a session sidebar showing conversation history. Olive's chat panel likely already has this, but a dedicated session management UI (list of past runs, named sessions) would improve UX.

### From Aura:

**Plan stored as reviewable file (MEDIUM)**
Aura stores its Plan Mode output in `/Saved/.Aura/plans/*.md`. This means the plan is an artifact the developer can review, modify, and version control. Olive's `preview_plan_json` is transient. Consider writing previewed plans to disk for transparency and debugging.

**Multi-agent specialization with credit gating (LOW for now)**
Aura's use of cheaper vs more expensive models (Haiku vs Opus) for different subagent types based on complexity is credit-aware. Olive's `FOliveUtilityModel` already does lightweight/heavyweight routing — but Aura's approach is more granular (43 tools across 4+ specialized subagents).

**"Ask Mode" as first-class operation (MEDIUM)**
Aura's Ask Mode explicitly reassures users that no changes will be made. Olive's read tools do this implicitly but there is no explicit UX framing that separates "analysis" from "modification." Making this distinction clearer in the UI could reduce user anxiety.

**Dragon Agent pattern — Python as the escape hatch (ALREADY DONE)**
Olive already has `editor.run_python` with `FOlivePythonToolHandlers`. Aura's Dragon Agent is essentially this pattern taken to full autonomy. Olive's implementation is solid; the gap is that Olive's autonomous loop is less tightly integrated than Dragon Agent's compile → reopen → iterate cycle.

### From Ultimate Co-Pilot:

**Node insertion after selected node (LOW)**
Users select an existing node and say "add X after this." This requires the AI to understand the graph's current exec flow and insert new nodes at the correct position. Worth noting for future `insert_after_node` plan op.

**Scene population tools (NOT A PRIORITY for Olive's current scope)**
Spawning hundreds of actors from natural language is a level design feature. It's not Olive's focus, but worth tracking as the competitive landscape expands.

---

## Recommendations

1. **The @-mention asset context pattern from AIK is the highest-value UX improvement to consider.** Detecting `@AssetName` in chat input and automatically injecting structured asset context would significantly reduce the cognitive overhead of directing the AI to specific assets.

2. **Aura's plan-as-file approach should be evaluated.** Writing `preview_plan_json` output to `/Saved/OliveAI/plans/{timestamp}.json` would give users a reviewable, versioned artifact and make debugging much easier. Low implementation cost; high transparency value.

3. **Olive's template system is a genuine moat that no competitor has matched.** The 325 extracted real-Blueprint library with inheritance resolution, lazy loading, and inverted search is unique. Continue investing here. The auto_tagger and prepare_library pipelines should be documented for future extraction runs.

4. **Aura's Telos 2.0 performance claims deserve scrutiny.** The claim of ">99% accuracy when processing existing blueprint graphs" is remarkable. If accurate, it implies they have a fine-tuned model or a specialized graph representation format that outperforms raw API calls. Olive's plan_json resolver achieves high accuracy through structural validation rather than model quality — this is more transparent and reproducible.

5. **AIK's 500+ pre-execution checks being added reactively (after crashes at launch) vs Olive's upfront validation pipeline is a meaningful architectural difference.** Olive's approach is correct. The recommendation is to ensure the validation pipeline is tested against every new plan op added, rather than discovering failures in production.

6. **The Dragon Agent + Claude Code integration pattern (Aura orchestrating Claude Code) is worth watching.** If Olive's MCP server can be discovered and used by Claude Code's agentic loop while also being the server for Olive's own chat session, this creates an interesting multi-agent topology. The current architecture already supports this partially.

7. **Aura's subscription model is a structural risk for them.** Users are price-sensitive (forum comment: "when the AI can edit existing blueprints and then you guys have a new subscriber" — implying they are not currently paying). Olive's no-subscription approach is a defensible differentiator, especially for indie developers.

8. **AIK's tool consolidation (27+ → 15) validates the direction already in `competitive-tool-analysis.md`.** No need to maintain fine-grained separate tools per asset subtype when a single "edit" tool with internal routing is more ergonomic and less confusing to the AI.

---

## Sources

- [Agent Integration Kit homepage](https://aik.betide.studio/)
- [AIK Gemini CLI documentation](https://aik.betide.studio/agents/gemini-cli)
- [AIK Codex CLI documentation](https://aik.betide.studio/agents/codex)
- [AIK changelog](https://aik.betide.studio/changelog)
- [AIK Profiles documentation](https://aik.betide.studio/profiles)
- [AIK comparison page](https://aik.betide.studio/comparison)
- [betide.studio NeoStack page](https://betide.studio/neostack)
- [assetsue.com AIK listing](https://assetsue.com/file/agent-integration-kit-neostack-ai)
- [Philip Conrod: NeoStack AIK review (v0.3.1)](https://www.philipconrod.com/co-developing-video-games-using-the-latest-version-of-the-neostack-ai-unreal-game-engine-plugin-using-multiple-llms-via-openrouter/)
- [Zed Agent Client Protocol specification](https://zed.dev/acp)
- [Aura homepage](https://www.tryaura.dev/)
- [Aura documentation](https://www.tryaura.dev/documentation/)
- [Aura About page (pricing)](https://www.tryaura.dev/about/)
- [Aura Blueprint updates page](https://www.tryaura.dev/updates/unreal-ai-agent-blueprints)
- [Aura: AI Agent for Unreal Editor — Epic forums thread](https://forums.unrealengine.com/t/aura-ai-agent-for-unreal-editor/2689209)
- [Ramen VR introduces Telos press release](https://www.prnewswire.com/news-releases/ramen-vr-introduces-telos-the-breakthrough-ai-agent-for-unreal-blueprints-302561368.html)
- [Aura 12.0 press release (GamesPress)](https://www.gamespress.com/en-US/Next-Evolution-of-Best-In-Class-Multi-agent-AI-Assistant-for-Unreal-En)
- [Aura 12.0 — Morningstar/BusinessWire](https://markets.financialcontent.com/wral/article/bizwire-2026-3-2-next-evolution-of-best-in-class-multi-agent-ai-assistant-for-unreal-engine-releases-with-aura-120-beta)
- [Aura 12.0 — BriefGlance](https://briefglance.com/articles/aura-120-redefines-unreal-engine-workflow-with-autonomous-ai)
- [Aura AI Assistant launches — PR Newswire](https://www.prnewswire.com/news-releases/aura-ai-assistant-for-unreal-engine-launches-vr-studio-ships-game-in-half-the-time-with-new-agent-capabilities-302651608.html)
- [Unreal University: Aura AI Assistant review](https://www.unreal-university.blog/aura-the-ai-assistant-for-unreal-engine/)
- [Unreal University: AI Blueprint Generation review](https://www.unreal-university.blog/this-ai-can-now-generate-blueprints-in-unreal-engine/)
- [Ultimate Engine Co-Pilot documentation](https://www.gamedevcore.com/docs/bpgenerator-ultimate)
- [Ultimate Co-Pilot Epic forums thread](https://forums.unrealengine.com/t/ultimate-engine-co-pilot-formerly-ultimate-blueprint-generator-the-ai-co-pilot-for-unreal-engine/2618922)
- [Ultimate Co-Pilot Epic forums thread page 2](https://forums.unrealengine.com/t/ultimate-blueprint-generator-the-ai-co-pilot-for-unreal-engine/2618922?page=2)
- [Ludus AI homepage](https://ludusengine.com/)
- [Ludus AI — smythos.com review](https://smythos.com/ai-trends/ludus-ai-in-unreal-engine/)
- [Philip Conrod: Ludus AI review](https://www.philipconrod.com/ludus-ai-agent-assisted-game-development-tools-for-unreal-game-engine/)
- [chongdashu/unreal-mcp GitHub](https://github.com/chongdashu/unreal-mcp)
