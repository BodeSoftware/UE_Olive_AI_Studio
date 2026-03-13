# Research: NeoStack and Aura Overview — March 13, 2026

## Question

What are NeoStack and Aura? What do they do, how do they handle code/Blueprint generation, and do they use templates?

---

## Note on Prior Research

Both products have been deeply researched already. This report is a concise factual summary. For full technical depth see:

- `plans/research/neostack-deep-dive-2026-03.md` — NeoStack AIK full architecture, tool list, validation, changelog, user sentiment
- `plans/research/aura-and-competitors-2026-03.md` — Aura full architecture, Telos, Dragon Agent, UX patterns, pricing
- `plans/research/competitor-deep-dive-2026-03.md` — Side-by-side comparison with Ultimate Co-Pilot and Ludus AI
- `plans/research/neostack-error-handling-philosophy-2026-03.md` — AIK validation and error recovery approach
- `plans/research/blueprint-gen-speed-quality-tradeoffs-2026-03.md` — Speed vs quality analysis across all tools

---

## Findings

### 1. NeoStack AIK (Agent Integration Kit by Betide Studio)

**What it is:**
A one-time-purchase ($109.99) UE editor plugin that connects external AI coding agents directly to the editor. The plugin is a tool server — it does NOT include a built-in AI model. Users bring their own agent (Claude Code, Gemini CLI, OpenRouter, Copilot, Codex).

**Current version:** v0.5.6 (beta, as of February 21, 2026). Supports UE 5.5–5.7, Windows + macOS.

**Architecture:**
- Dual transport: ACP protocol (for Claude Code / Codex / Copilot CLI) + MCP HTTP server on port 9315 (for Cursor, external agents)
- When using Claude Code: the plugin spawns a real `claude` subprocess with ACP adapter; Claude Code reads CLAUDE.md natively
- When using Cursor/Claude Desktop: they connect via MCP SSE endpoint
- OpenRouter: native built-in chat panel, API key only, no subprocess

**How code/Blueprint generation works:**
The plugin exposes ~15 unified tools (consolidated from 27+ in v0.4) to the AI agent. The agent calls these tools directly to create/edit Blueprint nodes, wire pins, set properties. There is NO template system — generation is purely AI-driven, node by node. Each tool call has 500+ crash-prevention guards (null/type/state checks added reactively in v0.3.0 after launch crashes), but no semantic plan validation, no preview-before-execute, and no structured self-correction.

**Do they use templates?**
No. Neither factory templates, reference templates, nor library templates. The closest equivalent is the Profiles system — 5 built-in profiles (Full, Animation, Blueprint & Gameplay, Cinematics, VFX & Materials) that whitelist tool access and inject domain-specific instructions into the agent context. Context injection beyond that is done via @-mention asset readers (36+ readers generate structured asset descriptions when a user types @AssetName). No template copying, no pattern libraries.

**Coverage (150+ operations across ~15 tools):**
Blueprint, Animation (state machines, BlendSpace, Skeleton, Physics Assets), Materials, Niagara, Sequencer, IK Rigs, Behavior Trees / State Trees, Widget Blueprints, Data (Structs/Enums/DataTables), PCG, MetaSounds, EQS, viewport screenshots, Python scripting, text-to-3D generation.

**Key differentiator vs Olive:**
AIK is a pure tool-server — the intelligence is entirely in the external agent (Claude Code etc.). Olive is a tool-server AND reasoning layer, with plan IR, validation, self-correction, and template systems. AIK has broader asset coverage but no structured execution pipeline.

---

### 2. Aura (RamenVR / tryaura.dev)

**What it is:**
A subscription-based AI assistant for Unreal Engine with hosted cloud inference. Unlike AIK, Aura includes its own AI models and reasoning layer — users do not bring their own API keys (though they can use Claude Code through MCP). Supports UE 5.3–5.7, Windows only (Mac planned).

**Pricing:**
- Free trial: 2 weeks, $40 Aura credit
- Pro: $40/month, $40 credit/month
- Ultra: $200/month, $280 credit/month
- Enterprise: custom pricing, source access

**Architecture:**
Three layers: UE plugin (per-engine-version) + local Aura server process + hosted cloud backend. The plugin bridges the cloud models to the editor via the local server. MCP is exposed as an Alpha feature for external agents (Cursor, VS Code, Claude Code from outside Aura's own panel).

**Models:**
Claude Sonnet 4.6 (base tasks), Claude Haiku (cheap/fast tasks), Claude Opus 4.6 ("Super Mode" available on Ultra). Credits are consumed per subagent complexity.

**Three UX modes:**
- Ask: query-only, no changes
- Plan: generates markdown plan files to `/Saved/.Aura/plans/`, no execution
- Agent: live changes with Keep/Reject/Review diff UI (Blueprint Diff tool)

**Blueprint generation — Telos 2.0:**
Telos is Aura's proprietary Blueprint sub-agent, described as a "reasoning framework by MIT-trained researchers." Telos 2.0 (shipped with Aura 12.0, February 2026) claims:
- >99% accuracy on existing Blueprint graphs
- 25x reduction in error rates vs Telos 1.0
- 10x speed at 1/10 the cost vs competitors

Technical mechanism is not publicly disclosed. Based on the Dragon Agent using "Unreal Python" for execution, it is inferred (not confirmed) that Telos also routes through Unreal's Python scripting layer rather than direct C++ K2Node API calls.

**Dragon Agent (Aura 12.0):**
Fully autonomous multi-step editor agent that runs via Unreal Python. Can compile C++, reopen projects headlessly, batch-edit actors, detect unused assets, and orchestrate other agents including Claude Code — all without the engine needing to remain open. This is the equivalent of Olive's `editor.run_python` taken to full autonomy.

**Do they use templates?**
Not publicly. Aura does NOT expose a template system to users. The Plan mode produces markdown plans, but these are output artifacts, not input templates. Telos likely has internal patterns/fine-tuning baked into the model weights, but no user-facing template library has been documented.

**Known issue:**
At launch (January 2026): "AI refuses to work on existing Blueprints." Telos 2.0 claims >99% accuracy on existing graphs — Aura's marketing presents this as resolved, but independent verification is not available.

---

## Recommendations

1. **NeoStack is a tool-server, Aura is a full product.** They compete in different ways with Olive. NeoStack competes on breadth of asset coverage (animation, Niagara, Sequencer, etc.). Aura competes on product experience (modes, diff UI, plan files, hosted inference). Olive's advantage over NeoStack is the pipeline (plan IR, validation, self-correction, templates). Olive's advantage over Aura is transparency and extensibility — Aura is a black box, Olive exposes every step.

2. **Neither uses templates in Olive's sense.** NeoStack uses @-mention context readers. Aura uses hosted reasoning. Olive's factory templates, reference templates, and library template system are genuinely differentiated — no direct equivalent exists in either competitor.

3. **The Dragon Agent is the most serious Aura threat.** It is Olive's `editor.run_python` taken to full autonomy, with headless project compilation and multi-agent orchestration. If Aura's Python scripts produce reliable output, the fact that they bypass the C++ K2Node API entirely may matter less than the breadth of what they can do.

4. **AIK's ACP architecture is instructive.** Their approach of bundling the ACP adapter binary (so users do not manually install `@zed-industries/claude-code-acp`) solved a real onboarding friction problem. Olive's `mcp-bridge.js` serves a similar role but requires Node.js. Worth monitoring whether AIK's zero-setup approach gives them a meaningful adoption edge.

5. **Aura's Plan mode markdown files are a UX pattern worth borrowing.** Storing plans as human-readable markdown in `/Saved/.Aura/plans/` makes them inspectable, editable, and version-controllable. Olive's plan JSON is already stored structurally — surfacing it as markdown in a predictable folder would add transparency at low cost.

6. **Neither competitor has a preview-before-execute cycle.** Olive's `blueprint.preview_plan_json` → `blueprint.apply_plan_json` with fingerprint verification is unique. This is a significant safety and trust feature that neither NeoStack nor Aura appears to offer.

---

Sources:
- [NeoStack AI — Betide Studio](https://betide.studio/neostack)
- [Agent Integration Kit — aik.betide.studio](https://aik.betide.studio/)
- [Agent Integration Kit on Fab](https://www.fab.com/listings/0e725daa-0233-408a-9597-960d20b4d919)
- [Aura — tryaura.dev](https://www.tryaura.dev/)
- [Aura About page](https://www.tryaura.dev/about/)
- [Aura 12.0 Beta — Games Press](https://www.gamespress.com/en-US/Next-Evolution-of-Best-In-Class-Multi-agent-AI-Assistant-for-Unreal-En)
- [Telos announcement — PR Newswire](https://www.prnewswire.com/news-releases/ramen-vr-introduces-telos-the-breakthrough-ai-agent-for-unreal-blueprints-302561368.html)
- [Aura launch — PR Newswire](https://www.prnewswire.com/news-releases/aura-ai-assistant-for-unreal-engine-launches-vr-studio-ships-game-in-half-the-time-with-new-agent-capabilities-302651608.html)
- [Philip Conrod — NeoStack review](https://www.philipconrod.com/co-developing-video-games-using-the-latest-version-of-the-neostack-ai-unreal-game-engine-plugin-using-multiple-llms-via-openrouter/)
- [Aura Blueprint generation blog](https://www.tryaura.dev/updates/unreal-ai-agent-blueprints)
- [Unreal University — Aura review](https://www.unreal-university.blog/this-ai-can-now-generate-blueprints-in-unreal-engine/)
- [NeoStack listing — assetsue.com](https://assetsue.com/file/agent-integration-kit-neostack-ai)
