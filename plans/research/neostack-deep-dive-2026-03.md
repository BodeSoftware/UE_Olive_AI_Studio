# Research: NeoStack AIK Deep Dive — March 2026

## Question
Comprehensive investigation of NeoStack AIK (Agent Integration Kit by Betide Studio) as our primary competitor: architecture, tool design, validation pipeline, context injection, agent loop, asset coverage, model support, recent release trajectory, and user sentiment.

---

## Findings

### 1. Product Identity and Positioning

**NeoStack AI** is the brand name. **Agent Integration Kit (AIK)** is the product name as sold on FAB. They are the same product.

- Developer: Betide Studio (known for EOS Integration Kit, Steam Integration Kit)
- FAB release: January 20, 2026 at $109.99 (one-time, no subscription)
- Documentation: `aik.betide.studio` (distinct from their NeoAI product at `neodocs.betide.studio` which is a different, older subscription product)
- Access: Windows + macOS, UE 5.5/5.6/5.7
- Support: Discord (`discord.gg/Fcj68FJzAj`), same-day resolution claimed

**Positioning statement:** "The most complete AI integration for Unreal Engine — 27+ specialized tools, 36+ asset type readers, 150+ distinct operations." (Note: the "27+" language is pre-v0.5.0 consolidation — the current tool count is ~15.)

**Pricing model:** Bring-your-own AI subscriptions. Plugin does not proxy tokens. Claude Code ($20/mo Max subscription), Gemini CLI (free tier), or OpenRouter (API key). The $109.99 one-time cost is the only AIK expense.

Source: [betide.studio/neostack](https://betide.studio/neostack), [aik.betide.studio](https://aik.betide.studio/), [assetsue.com](https://assetsue.com/file/agent-integration-kit-neostack-ai)

---

### 2. Architecture

#### Communication Channels (Two Paths)

AIK uses **two parallel communication paths** simultaneously:

**Path A — ACP (in-editor chat panel):**
- ACP = "Agent Communication Protocol" — a stdio subprocess protocol from Zed Industries
- Claude Code, Gemini CLI, Codex CLI, GitHub Copilot CLI are all connected via ACP adapter binaries that ship with the plugin
- The adapter converts between the CLI tool's native protocol and the plugin's tool dispatch layer
- The plugin spawns the CLI process as a subprocess; the ACP adapter runs between them
- One process per user task (not per turn) — Claude Code exits when the task is complete
- `@zed-industries/claude-code-acp` and the Codex adapter are **bundled** with the plugin since v0.5.0 — users do not install them manually

**Path B — MCP HTTP server (external agents like Cursor):**
- HTTP JSON-RPC server, always-on, auto-starts with the editor
- Default port: **9315** (from prior research — not publicly confirmed on docs page, but consistent with memory)
- Dual transport on same port: SSE (`/sse`, MCP 2024-11-05) for agents that still use SSE; Streamable HTTP (`/mcp`, MCP 2025-03-26) for newer agents
- Claude Code STILL uses SSE transport (the ACP path is preferred and separate); SSE endpoint used when Cursor or Claude Desktop connects directly
- `.mcp.json` written dynamically to project root after server binds, with actual port

**Key architectural insight:** When you use AIK's in-editor chat panel with Claude Code, **you are using ACP, not MCP**. The MCP server exists for Cursor and other external IDEs. This is why Claude Code "reads CLAUDE.md natively" — it's running as a real Claude Code subprocess, not receiving a system prompt injection through MCP.

Source: `plans/research/neostack-architecture.md` (prior research session), AIK docs at `aik.betide.studio/agents`

#### Agent Lifecycle (ACP path)

```
User submits prompt
  → Plugin spawns CLI subprocess with ACP adapter
  → ACP adapter sends task to CLI agent (Claude Code, etc.)
  → Agent calls tools (dispatched to plugin's game thread)
  → Results returned to agent
  → Agent exits when task complete
  → Plugin captures tool audit log
```

This is single-session, not multi-turn — each prompt starts fresh. Chat handoff (v0.5.0) uses AI-generated context summaries to simulate continuity across sessions.

#### Tool Audit Log

Every operation is logged to `Saved/Logs/AIK_ToolAudit.log`:
- Timestamp
- Operation status (OK / FAIL)
- Action type
- Asset name
- Result details

This is used for crash reporting and post-hoc debugging, not real-time validation.

Source: `aik.betide.studio/troubleshooting`

---

### 3. Tool Design — v0.5.0 Consolidation

The most important architectural decision in AIK's history: **v0.5.0 (Feb 16, 2026) consolidated from 27+ tools to ~15 "intelligent single tools."**

**The philosophy:** "Agents pick correct tools more reliably" when there are fewer, broader tools. One `edit_rigging` replaces multiple specialized IK/retarget/skeleton variants. The tool detects internally which sub-operation to perform.

**The ~15 tool names are NOT publicly documented.** The documentation describes them only at the category level:
- Blueprint editing (create, read, modify, graph editing)
- Animation Blueprint
- Material graph
- Niagara VFX
- Level Sequencer
- IK Rigs & Retargeters
- Behavior Trees / State Trees
- Widget Blueprints
- Data structures (Structs, Enums, DataTables, Chooser Tables)
- PCG & MetaSounds
- Python scripting execution
- C++ live coding
- Viewport screenshots
- Log reading
- Image/3D generation

The `edit_rigging` consolidation example from v0.5.0 notes suggests the naming convention is likely `edit_{domain}` for write operations and `read_{asset_type}` for read operations.

**What can be inferred from the "36+ asset type readers" claim:** This is likely a separate read tool that dispatches internally based on asset type, or a family of overloaded read tools that share a common schema. The "27+ tools → 15 tools" vs "36+ asset readers" distinction suggests readers are not counted in the "tool" number — they may be sub-operations or resources rather than tools.

**Subconfigs (v0.3.0):** Per-profile tool access control — specific tools can be disabled per profile so the agent cannot see or call them. This is a whitelist: empty list = all tools visible; non-empty list = only listed tools visible. Global tool disabling in Project Settings overrides all profiles.

Source: `aik.betide.studio/changelog` (v0.5.0 entry), `aik.betide.studio/profiles`

---

### 4. Validation — "500+ Checks"

AIK's "500+ crash reduction checks" were added in v0.3.0 (Feb 8, 2026) and are described as **pre-execution validation** wrapping all tool execution.

**What's known:**
- Added in response to 100+ crashes at launch (plugin was extremely unstable on FAB release Jan 20)
- v0.3.0 adds "500+ crash reduction checks" — described as "Blueprint health validation after edits"
- v0.5.0 adds "crash protection wrapping tool execution" — suggests a catch/exception layer around every tool call
- "Blueprint health validation after edits" implies post-write compile check or structural check, not just pre-execution

**What's inferred from source (not documented):**
- The 500 checks almost certainly include: null pointer guards on asset handles, graph validity checks before node insertion, pin type compatibility checks before wiring, asset load success verification, and game thread enforcement
- "Crash protection wrapping" (v0.5.0) suggests a try/catch or UE `DEFINE_LOG_CATEGORY` + structured exception around every tool body
- This is a reactive approach (prevent crashes) not a semantic validation approach (prevent logical errors)
- Compare with Olive's write pipeline stages 1 (Validate) and 6 (Verify): Olive validates intent before execution AND verifies structural correctness after — AIK's approach is primarily defensive crash prevention

**Self-correction:** Not explicitly documented. The v0.3.2 notes mention "one-shot task capability via improved prompting" — suggesting self-correction happens via prompt engineering rather than a structured retry policy. No evidence of a multi-turn self-correction loop comparable to Olive's `FOliveSelfCorrectionPolicy`.

Source: `aik.betide.studio/changelog` (v0.3.0, v0.5.0 entries)

---

### 5. Context Injection — What the LLM Sees

AIK's context injection has three layers:

**Layer 1 — Profile system prompt additions:**
Each profile can inject custom instructions appended to the agent's system prompt. For ACP agents (Claude Code), this likely goes through the adapter as `--append-system-prompt` or equivalent. For MCP clients, this becomes part of the tool descriptions or a prompt resource.

**Layer 2 — @ mention attachment:**
Users type `@asset_name` in the chat input. A dropdown shows matching assets. Selected assets are read and their content is attached to the user's message. The format is structured (not raw JSON) but the exact serialization is not documented publicly.

**Layer 3 — Right-click node attachment:**
In the Blueprint editor, right-clicking a node shows "Attach to Agent Prompt" — the node's data is added to the next message's context.

**Layer 4 — Project indexing:**
"Automatic project indexing for context-aware suggestions" — this is background indexing similar to Olive's `FOliveProjectIndex`. The exact index data structure and query interface are not documented publicly.

**What the LLM does NOT see (inferred):**
- AIK does not appear to have a structured knowledge injection system equivalent to Olive's `FOlivePromptAssembler` capability block. The system prompts appear to be profile-level custom text + native CLI tool documentation.
- No equivalent to Olive's `FOliveMCPPromptTemplates` for structured domain knowledge injection.
- Claude Code reads `CLAUDE.md` natively — so AIK relies on this for domain knowledge. No `AGENTS.md` is needed since Claude Code reads CLAUDE.md.

Source: `aik.betide.studio/chat-interface`, `aik.betide.studio/profiles`, `aik.betide.studio/troubleshooting`

---

### 6. Agent Loop — How Tasks Execute

**For ACP agents (Claude Code, Gemini CLI, Codex):**
- Agent receives task + context attachments
- Agent reads assets using read tools
- Agent calls write tools (which execute on game thread)
- Plugin dispatches to game thread via `AsyncTask(ENamedThreads::GameThread, ...)`
- Agent compiles and iterates based on tool results
- Agent exits when task complete
- Plugin captures result and shows toast notification (v0.3.0)

**"One-shot task capability" (v0.3.2):** This improvement came from prompt engineering ("improved prompting"). The AI is prompted to complete the full task in one session rather than partial completion. This is equivalent to Olive's `MaxAutoContinues` mechanism but implemented via prompt injection rather than process management.

**No plan-validate-execute cycle:** AIK does not have an equivalent to Olive's `blueprint.preview_plan_json` + `blueprint.apply_plan_json`. The agent executes tools directly. There is no user-visible plan step before execution. The "Plan Mode" mentioned in v0.1.11 appears to be a UI display feature (showing agent thinking), not a semantic pre-execution plan review.

**Plan panel (v0.5.0):** New UI element showing "agent thinking and steps" — this is a thinking content display for Claude Code's extended thinking, not a semantic plan review. The user can see what the agent is reasoning about but cannot approve/reject before execution.

**Error recovery:** Claude Code's native self-correction handles errors within its process lifecycle. AIK does not add a separate self-correction policy — the agent handles its own retries within the single process lifetime. When it fails and exits, the next user message starts a fresh session with context from the previous tool audit log (if attached).

Source: `aik.betide.studio/changelog` (v0.1.11, v0.3.2, v0.5.0 entries), `aik.betide.studio/chat-interface`

---

### 7. Asset Coverage — Full Map

As of v0.5.6 (Feb 21, 2026):

| Asset Type | Coverage | Notes |
|-----------|----------|-------|
| Blueprints (Actor, Object, etc.) | Full | Variables, components, functions, events, macros, interfaces, SCS nodes, full graph editing |
| Animation Blueprints | Full | State machines, transitions, conduits, AnimGraph subgraphs, Linked Anim Layers |
| Widget Blueprints | Full | UMG widgets, layouts, property configuration, event bindings |
| Materials | Full | Material expressions, parameter nodes, wiring, Material Functions |
| Niagara VFX | Full | Emitters, modules, renderers, parameters, dynamic inputs, custom HLSL |
| Level Sequencer | Full (rewritten v0.2.0) | Bindings, tracks, keyframes, camera cuts, audio |
| Behavior Trees | Full (rewritten v0.3.2) | Tasks, decorators, services, any node property editable |
| State Trees | Full | Evaluators, transitions |
| IK Rigs & Retargeters | Full | Solvers, goals, bone settings, retarget chains |
| Animation (BlendSpace) | Full (v0.4.0) | 1D/2D, samples, axis parameters |
| Animation (AnimSequence) | Full (v0.4.0) | Bone track keyframes, notifies, curves, sync markers |
| Skeleton | Full (v0.4.0) | Sockets, virtual bones, retarget settings |
| Physics Asset | Full (v0.4.0) | Ragdoll, collision shapes, constraints |
| Control Rig | Full (v0.3.2) | Hierarchy + RigVM graph |
| Animation Montage | Full (v0.2.0) | Sections, notifies, blend settings |
| Enhanced Input | Full (v0.2.0) | InputActions, InputMappingContexts |
| Pose Search / Motion Matching | Full | Schemas, feature channels, normalization sets |
| PCG Graphs | Supported | Procedural content generation |
| MetaSounds | Supported | Audio synthesis graphs |
| Data Structures | Full | Structs, Enums, DataTables, Data Assets, Chooser Tables |
| Gameplay Tags | Supported (v0.1.33) | |
| EQS (Environment Query) | Full (v0.5.0) | Asset creation and editing with node configuration |
| C++ / Live Coding | Supported | Direct live coding integration |
| Python Scripting | Supported | Direct editor automation |
| Viewport Screenshots | Full (v0.2.0) | With camera info |
| Asset Screenshots | Supported | Visual context capture |
| Agent Camera Control | Full (v0.5.0) | Move, focus, orbit, switch view modes |
| Log Reading | Supported | |
| Text-to-Image | Supported | Generates textures as assets |
| Text-to-3D (Meshy) | Supported | Generates meshes (fixed v0.3.2) |

**Coverage comparison with Olive:**
- AIK covers everything Olive covers (Blueprint, BT, PCG, C++, Python) PLUS: Materials, Niagara, Sequencer, IK Rigs, Animation (all types), Widget BP, MetaSounds, EQS, image/3D generation, viewport camera control
- Olive has deeper Blueprint coverage (plan-JSON resolver, phase validation, template system, library templates) but narrower asset breadth

Source: `aik.betide.studio/changelog` (all versions), `betide.studio/neostack`

---

### 8. Model Support — Which Models Work

**Official recommendations (from AIK docs and user reports):**
- **Claude Code (Claude Max subscription):** "Best value," "best results by far" — the primary intended integration
- **Gemini CLI (free tier):** "Free tier with Google AI" — works but quality varies
- **Codex CLI (ChatGPT/OpenAI):** Supported via ACP adapter
- **GitHub Copilot CLI:** Supported (major stability improvements in v0.5.5)
- **Cursor:** Via MCP integration
- **Kimi CLI:** Added v0.5.0
- **OpenRouter:** Built-in native client — any model, any provider
- **Local LLMs:** Via OpenRouter or direct integration (experimental)

**Real-world model performance (from Philip Conrod review, Feb 2026):**
> "Claude Code...works best for creating Blueprint logic while the other models produce less than desired results"

**Betide's own statement (assetsue.com listing):**
> "Claude delivers the best results by far" among available AI models

**Reasoning depth controls (v0.5.0):** off / low / medium / high / max — suggests they're exposing Claude's extended thinking budget controls. Token counting and cost tracking added simultaneously.

**Implication:** AIK is effectively a Claude Code delivery system with other agents as secondary options. The ACP path with Claude Code is the primary design target. This mirrors Olive's situation with `FOliveClaudeCodeProvider` as the default.

Source: `aik.betide.studio/agents`, `assetsue.com` listing, Philip Conrod article

---

### 9. Release Velocity and Trajectory

AIK shipped from v0.1.10 (early access) to v0.5.6 in under 6 weeks. The trajectory by category:

**Phase 1 (Jan 20 – Feb 7):** Basic stability, crash triage, Niagara/Sequencer initial support
**Phase 2 (Feb 7–10):** Complete Blueprint coverage (v0.3.0), Complete Animation coverage (v0.4.0)
**Phase 3 (Feb 16–21):** UI overhaul, tool consolidation, zero-setup install, EQS, vision capabilities (v0.5.0–v0.5.6)

**Direction they are heading (inferred):**
1. **Ease of setup** — every release reduces friction (bundled adapters, in-process Claude installer, Bun lockfile recovery)
2. **Fewer broader tools** — the 27+ → 15 consolidation is a core philosophy they will continue
3. **Vision/spatial capabilities** — viewport capture + agent camera control suggest they want the AI to "see" the scene
4. **Multi-session continuity** — chat handoff with AI-generated summaries, session sidebar, load Claude Code history
5. **Cost transparency** — per-model cost breakdowns suggest price-sensitive user base

**What they are NOT doing (conspicuously absent):**
- No plan-preview-before-execute cycle
- No library template system
- No semantic operation history
- No multi-turn self-correction policy with error classification
- No structured capability knowledge injection beyond profile custom instructions

Source: `aik.betide.studio/changelog`

---

### 10. User Sentiment

**Positive signals:**
- 100+ crash fixes shipped within days of launch — responsive to feedback
- Same-day Discord resolution claimed and corroborated
- "This tool is evolving fast, i mean FAST" (Ultimate Co-Pilot forum user, similar product sentiment)
- Philip Conrod (developer blogger): praised rapid iteration and 100% Blueprint coverage
- Users note the one-time pricing model favorably vs subscription competitors

**Negative signals:**
- Launched on FAB January 20 with 100+ crashes — extremely unstable at release
- "Claude delivers the best results by far" is also a warning: non-Claude users get "less than desired results"
- No trial version available on FAB — must purchase to evaluate
- Plugin still labeled Beta despite v0.5.6
- Users in adjacent threads (Aura forum) show a shared pain point: "AI refuses to work on existing blueprints" — not confirmed for AIK but suggests Blueprint editing reliability is hard across all products

**Support model:**
- Discord-first support (community + developer)
- "Remote sessions" offered for complex setup issues
- Tool audit log required for crash reports

Source: `assetsue.com`, Philip Conrod article, Aura forum thread, `aik.betide.studio/troubleshooting`

---

### 11. Competitor Context — Aura 12.0 (March 2, 2026)

Aura (by RamenVR) shipped v12.0 on March 2, 2026 — three days ago. This is now the most technically sophisticated competing product and represents a different architectural philosophy.

**Telos 2.0 (Blueprint agent):**
- Claims >99% accuracy on existing Blueprint graphs
- Claims 10x speed improvement vs "competitors" (unnamed)
- Claims 25x error rate reduction
- "Advanced node search for project and custom nodes"
- Proprietary reasoning framework optimized for Blueprint structure

**Dragon Agent:**
- Runs via Unreal Python (not MCP tools)
- Can operate without Unreal Engine open
- Autonomous C++ compilation
- Batch editing actors, components
- Asset detection and naming convention enforcement
- Integrates with Claude Code for build-test-iterate loops

**Architecture insight:** Aura uses a **multi-agent approach** where specialized agents hand off to each other. The Dragon Agent can autonomously open/close UE projects, compile C++, and run the full editor lifecycle. This is more ambitious than AIK's "Claude Code subprocess" approach.

**Pricing:** Subscription (credit-based). $40 free credits for trial. Monthly subscription implied but amount not disclosed publicly. RamenVR positions this as a professional tool for studios ($40 trial credit, case study with "VR Studio ships game in half the time").

**How Aura differs from AIK:**
- AIK: bring-your-own agent, one-time purchase, general-purpose
- Aura: proprietary agent models (Telos, Dragon), subscription, game-development-specialized

Source: `gamespress.com/en-US/Next-Evolution...`, `briefglance.com/articles/aura-120...`, prior competitive research

---

### 12. Gaps vs. Olive

| Capability | AIK | Olive |
|------------|-----|-------|
| Blueprint asset breadth | Similar | Similar |
| Blueprint graph depth | Tool-based (imperative) | plan-JSON + resolver (declarative) |
| Pre-execution plan review | No | Yes (preview_plan_json) |
| Self-correction policy | Native agent handling | Structured (FOliveSelfCorrectionPolicy) |
| Library template system | No | Yes (325+ extracted templates) |
| Reference templates | No | Yes |
| Materials | Yes | No |
| Niagara VFX | Yes | No |
| Level Sequencer | Yes | No |
| Animation (all types) | Yes | Partial (AnimBP only) |
| IK Rigs | Yes | No |
| Widget Blueprint | Yes | Partial |
| MetaSounds | Yes | No |
| EQS | Yes | No |
| Vision (viewport capture) | Yes | No |
| In-editor chat panel | Yes | Yes |
| Multi-session continuity | AI summary handoff | Conversation history |
| Tool consolidation | ~15 tools | 98 tools |
| Pricing model | One-time $109.99 | TBD |
| UE 5.7 support | Yes | No (5.5 target) |

---

## Recommendations

1. **Tool consolidation is urgent, not optional.** AIK shipped with 27+ tools, discovered it hurt agent accuracy, and consolidated to 15 in v0.5.0. At 98 tools, Olive is at risk of the same accuracy degradation. The prior competitive analysis identified ~27-30 as achievable without capability loss. This should be a near-term architect task.

2. **The plan-JSON approach is our primary differentiator and must be communicated.** AIK has no equivalent. Users complain about "AI refusing to work on existing Blueprints" — Olive's preview_plan_json + apply_plan_json with fingerprint verification and phase validation addresses exactly this. The difficulty is communicating this advantage in positioning.

3. **Validation philosophy difference is real and worth maintaining.** AIK's 500 checks are crash-prevention guards (defensive). Olive's write pipeline is semantic validation (prescriptive). Both are needed — Olive's approach is more correct, but AIK gets credit for the "500 checks" marketing number. We should consider whether Olive's validation count can be surfaced similarly.

4. **Library template system has no competitor equivalent.** The 325+ extracted real Blueprint templates with inheritance resolution, search, and function-level retrieval is unique. This should be a primary marketing point when positioning against AIK.

5. **Aura 12.0 is now the most technically ambitious competitor.** The Dragon Agent (operates without UE open, via Python) and Telos 2.0 (>99% accuracy claims) represent a qualitative leap. If Aura's accuracy claims are even partially true, Telos 2.0 is a direct threat to Olive's Blueprint quality advantage. Monitor Aura closely.

6. **Vision capabilities are a gap.** Both AIK and Aura can capture and reason about the viewport. Olive has no equivalent. This is high-value for scene building and debugging but low priority for Blueprint logic.

7. **ACP vs. MCP transport is worth studying.** AIK's ACP path (stdio subprocess) produces better results than the MCP path because Claude Code runs natively. Olive's MCP bridge approach (`mcp-bridge.js`) converts stdio↔HTTP. This works but may introduce latency and reliability issues that the native ACP approach avoids. The architect should evaluate whether an ACP-style adapter for the autonomous provider would improve reliability.

8. **AIK's "zero-setup" bundled adapters (v0.5.0) raise the setup bar.** New users expect click-to-install. Olive's requirement to install Claude Code CLI separately is a friction point. The in-process Claude installer AIK added in v0.5.6 further reduces friction. Olive should at minimum document installation clearly.

9. **The one-time pricing model ($109.99) is a strong user preference signal.** Subscription fatigue is real. If Olive charges subscription pricing, the comparison to AIK's one-time model will be a conversion blocker for price-sensitive developers.

10. **Self-correction architecture matters more than quantity of checks.** AIK's reactive crash prevention vs. Olive's structured error classification and progressive disclosure are different philosophies. Olive's approach produces better agent outcomes over multiple turns; AIK's approach produces fewer hard crashes. Neither is wrong — Olive should not abandon its structured approach to copy AIK's defensive check count.

---

## Sources

- [Agent Integration Kit documentation](https://aik.betide.studio/)
- [AIK changelog](https://aik.betide.studio/changelog)
- [AIK profiles](https://aik.betide.studio/profiles)
- [AIK comparison page](https://aik.betide.studio/comparison)
- [AIK chat interface](https://aik.betide.studio/chat-interface)
- [AIK troubleshooting](https://aik.betide.studio/troubleshooting)
- [AIK agents overview](https://aik.betide.studio/agents)
- [AIK Codex agent page](https://aik.betide.studio/agents/codex)
- [betide.studio/neostack marketing page](https://betide.studio/neostack)
- [Agent Integration Kit on assetsue.com](https://assetsue.com/file/agent-integration-kit-neostack-ai)
- [Philip Conrod user review (Feb 2026)](https://www.philipconrod.com/co-developing-video-games-using-the-latest-version-of-the-neostack-ai-unreal-game-engine-plugin-using-multiple-llms-via-openrouter/)
- [Aura AI launch press release](https://www.prnewswire.com/news-releases/aura-ai-assistant-for-unreal-engine-launches-vr-studio-ships-game-in-half-the-time-with-new-agent-capabilities-302651608.html)
- [Aura 12.0 Games Press announcement](https://www.gamespress.com/en-US/Next-Evolution-of-Best-In-Class-Multi-agent-AI-Assistant-for-Unreal-En)
- [Aura 12.0 BriefGlance technical summary](https://briefglance.com/articles/aura-120-redefines-unreal-engine-workflow-with-autonomous-ai)
- [Ultimate Engine Co-Pilot forum thread](https://forums.unrealengine.com/t/ultimate-engine-co-pilot-formerly-ultimate-blueprint-generator-the-ai-co-pilot-for-unreal-engine/2618922)
- [Aura forum thread on Epic](https://forums.unrealengine.com/t/aura-ai-agent-for-unreal-editor/2689209)
- Prior research: `plans/research/competitive-tool-analysis.md` (Feb 27, 2026)
- Prior research: `plans/research/neostack-architecture.md` (prior session — referenced in MEMORY.md)
