# Research: Blueprint Generation — Speed vs Quality Tradeoffs in AI Tools (March 2026)

## Question

How do NeoStack AIK, Aura, and other UE AI tools handle Blueprint creation? Specifically: single-shot vs iterative, pin/function resolution, agent pipelines (planner→builder→reviewer), template/reference data strategies, failure/retry, and protocol choices. Also covers general AI coding agent patterns (Roo Code, Claude Code, Cursor/Windsurf) and academic research on tool call overhead.

---

## Findings

### 1. NeoStack AIK — Architecture and Blueprint Generation

#### Execution Model

NeoStack AIK is fundamentally a **tool-server** — it does NOT generate Blueprints itself. It exposes ~15 consolidated tools (post-v0.5.0 from 27+) through ACP/MCP, and then **defers entirely to an external agent** (Claude Code, Gemini CLI, Cursor, etc.) to decide how and when to call those tools.

This means:
- The Blueprint generation strategy is determined by the underlying agent, not AIK.
- If Claude Code is connected, Claude decides whether to call `edit_graph` 3 times or 30 times.
- AIK's role is tool fidelity (does `edit_graph` do what it says), not generation strategy.

**Single-shot vs iterative:** Always iterative. The agent loop is open-ended — it calls tools in whatever sequence it reasons about. There is no plan-then-execute or batch submission mechanism in AIK itself. The agent decides everything.

Source: [aik.betide.studio](https://aik.betide.studio/), [betide.studio/neostack](https://betide.studio/neostack), prior research at `plans/research/neostack-deep-dive-2026-03.md`

#### Communication Protocol

AIK runs two simultaneous paths:

| Path | Protocol | Used By |
|------|----------|---------|
| In-editor chat | ACP (Agent Communication Protocol, Zed Industries) | Claude Code, Gemini CLI, Codex CLI, GitHub Copilot CLI |
| External IDE | MCP HTTP JSON-RPC, SSE + Streamable HTTP | Cursor, Claude Desktop, VS Code |

ACP path: AIK spawns the CLI as a subprocess. The ACP adapter binary (bundled since v0.5.0) converts between the CLI's native protocol and AIK's tool dispatch. One subprocess per task, exits when complete. Claude Code running via ACP reads its own CLAUDE.md natively — system prompt injection is NOT how AIK works.

When the external IDE path is used (Cursor via MCP), the MCP server runs on port 9315, always-on.

Source: `plans/research/neostack-deep-dive-2026-03.md` (confirmed, do not repeat verbatim)

#### Blueprint Tool Mechanics

AIK's Blueprint tools (consolidated into `edit_graph` and related tools) operate at the **individual node/connection level**. From user documentation and testing reports:

- Add nodes, set node properties, connect pins, set variable defaults — each is a discrete tool call
- No "batch graph submission" or IR: the agent must call node creation + wiring as separate operations
- Context injection via @-mention: user explicitly attaches Blueprint nodes/assets to the AI's context; this is manual, not automatic

**Pin name and function resolution:** Done by the external agent. AIK provides no alias map, no function fuzzy matching, no auto-correction for wrong pin names. If the agent calls a function that doesn't exist or uses a wrong pin name, the operation fails and the agent must correct on its own (no structured self-correction layer).

**500+ validation checks** (v0.3.0 addition): These are defensive null/type/state guards to prevent editor crashes, not semantic plan validation. Added reactively after launch crashes. As of v0.5.0, all tool execution is also wrapped in try/catch.

Source: `plans/research/competitor-deep-dive-2026-03.md`, `plans/research/neostack-deep-dive-2026-03.md`

#### Agent Pipeline

There is no AIK-owned agent pipeline. AIK does not run a planner, builder, or reviewer. The external agent (Claude Code or other) does all reasoning. Any pipeline structure is what the user configures in their Claude Code CLAUDE.md or system prompt, or what Claude Code invents on its own.

**Profiles system** (v0.5.0) allows whitelisting tool access and injecting custom instructions per profile (Full, Animation, Blueprint & Gameplay, Cinematics, VFX & Materials). This is the closest thing to a routing layer — the profile restricts which tools the agent can see, reducing hallucination surface on irrelevant tools.

#### Template / Reference Data Strategy

No built-in template system. No reference data injection beyond what the user manually @-mentions. The 36+ asset type readers allow the user to attach any UE asset as context to their prompt — structured data about Materials, Blueprints, etc. injected into the conversation. This is reactive (user-driven) rather than automatic.

**Project indexing** (v0.5.0): Automatic project indexing for context-aware suggestions — details not published, but presumably populates a searchable asset registry the agent can query.

#### Failure / Retry Strategy

No structured retry or self-correction. The external agent handles all retries and error recovery. AIK's contribution is:
1. Rich error messages from tool execution
2. Tool audit log (`Saved/Logs/AIK_ToolAudit.log`) for post-hoc debugging
3. Crash protection via try/catch on all tool calls

#### Speed Characteristics

Speed is entirely determined by the external agent:
- Claude Code Max plan: 210 tokens/sec on Sonnet (from NeoAI docs — separate product, same pattern)
- Tool call overhead per node operation: one round-trip per operation (agent → tool dispatch → editor mutation → return result)
- Complex Blueprint (10+ nodes): likely 20-50+ tool calls, 2-10+ minutes depending on agent reasoning speed and retry rate

There are no published benchmarks for AIK-specific Blueprint generation times.

---

### 2. Aura (RamenVR / tryaura.dev) — Architecture and Blueprint Generation

#### Execution Model

Aura is a **full-stack proprietary system**: plugin + local server + cloud inference backend. It is the opposite philosophy from AIK — Aura controls everything including the model calls.

**Three modes:**
| Mode | Execution | Makes Changes |
|------|-----------|--------------|
| Ask | Cloud LLM, read-only | No |
| Plan | Cloud LLM → stores markdown in `/Saved/.Aura/plans/` | No |
| Agent | Cloud LLM + Dragon Agent (Unreal Python) | Yes |

**Blueprint-specific agent: Telos / Telos 2.0**
- A specialized sub-agent dedicated to Blueprint graph construction
- NOT general-purpose: optimized (and likely fine-tuned or RLHF-aligned) for UE Blueprint semantics
- Internal mechanism not disclosed; Aura describes it as "proprietary reasoning framework by MIT-trained researchers"
- Inference: likely routes through Python (same as Dragon Agent which explicitly uses "Unreal Python" for editor mutations)

**Key architectural finding:** Aura's Agent mode is built on Unreal Python, NOT custom C++ K2Node API calls. Dragon Agent uses `IPythonScriptPlugin` to call UE's editor automation layer. This is the same mechanism as Olive's `editor.run_python`. Telos likely follows the same path for actual Blueprint mutations.

Source: `plans/research/aura-and-competitors-2026-03.md`

#### Single-Shot vs Iterative

**Telos appears to be a single-shot generator** — the user issues a prompt, Telos generates the Blueprint logic, and streams the result back. The "livestream changes directly in chat" UX implies the graph is being built progressively (nodes appearing as Telos works), but it does not appear to be an open-ended tool-calling loop like AIK+Claude Code.

Evidence for single-shot approach:
- "Generates entire gameplay systems" phrasing in marketing implies holistic output
- Speed claims (10x speed vs competitors) are hard to achieve with open-ended iterative loops
- No mention of a planner-builder-reviewer sub-pipeline; Telos is described as a single specialized agent
- User-facing mode is "write a prompt → get result" not "approve each step"

The Plan mode (markdown plans to disk) is the closest thing to a two-step pipeline, but it is user-initiated — users generate a plan, inspect it, then trigger Agent mode. There is no automatic planner-then-builder.

**Inferred (not confirmed):** Telos likely generates a structured internal representation of the graph (what nodes to create, what to connect, what properties to set), then executes that in one pass via Python. This would explain the speed advantage and the "keep/reject/review diff" UX that implies a discrete atomic change.

Source: `plans/research/aura-and-competitors-2026-03.md`, [tryaura.dev/updates/unreal-ai-agent-blueprints](https://www.tryaura.dev/updates/unreal-ai-agent-blueprints), Unreal University blog review

#### Pin Name / Function Resolution

Not publicly disclosed. Given that Telos is a specialized Blueprint agent, it likely has:
- A curated knowledge base of valid UE function names, pin names, and node types
- Pre-validation before execution (otherwise crash rates would be high)
- A "25x error rate reduction" in Telos 2.0 implies Telos 1.0 had significant errors — function/pin hallucination is the most common cause

The 99% accuracy claim on existing Blueprint graphs suggests strong read-before-write behavior — Telos reads the existing graph before modifying it, which helps with pin name resolution.

#### Agent Pipeline

No explicit planner→builder→reviewer pipeline is described publicly. However, the three modes (Ask→Plan→Agent) form a user-operated version of this pipeline:
- Ask = researcher
- Plan = planner (output: markdown file)
- Agent = builder (reads plan? unclear)

The Dragon Agent (12.0) introduces **multi-step autonomous execution** for Python-heavy workflows, but Blueprint work still appears to be Telos-only with no visible subagent decomposition.

Source: `plans/research/aura-and-competitors-2026-03.md`, [gamespress.com](https://www.gamespress.com/en-US/Next-Evolution-of-Best-In-Class-Multi-agent-AI-Assistant-for-Unreal-En)

#### Template / Reference Data Strategy

**Project-level context:** "Deep contextual understanding of your Unreal project thanks to proprietary algorithms for extracting and storing data." (Marketing copy.) In practice: indexed at startup, searchable at query time.

**Aura reads existing Blueprints before editing** — the "99% accuracy on existing BP graphs" benchmark is specifically about reading/interpreting existing graphs, not generating from scratch. This implies a structured serialization of the Blueprint's current state is fed to the model before editing begins.

**No external template library** is mentioned. Context comes from the user's own project. Up to 4 parallel chat threads share the same project context.

#### Failure / Retry Strategy

**User-facing:** Keep/Reject/Review diff. If the user Rejects, they can re-prompt.

**Internal:** Not documented. Telos 2.0's "25x error reduction" and "16% improvement on hard benchmarks" implies v1 had a significant failure rate that was addressed in v2.

**Known failure modes (from independent reviews):**
- Collision box scaling errors (geometry not matching visual)
- Asset reference failures when spawning actors from other Blueprint assets
- "AI refuses to work on existing Blueprints" (known bug at launch, claimed fixed in Telos 2.0)
- Better-suited for prototyping than production-ready output

#### Speed Claims vs Reality

**Marketing:** 15–30x faster than competitors on Blueprint questions (30-trial benchmark, 6 prompts x 5 repetitions). Telos 2.0: 10x speed vs competitors for "entire gameplay systems," 1/10th the cost.

**Independent observation (Unreal University blog):** Simple turret system — mostly successful. Asset references required manual connection. Scaling required manual fix. Time not reported.

**Inference:** Speed advantage likely comes from:
1. Specialized fine-tuned model with fewer hallucination retries
2. Batch graph generation (one LLM call → full node graph) rather than per-node tool calls
3. Optimized project context injection (only relevant nodes/functions sent to model)

Source: [prnewswire.com/telos](https://www.prnewswire.com/news-releases/ramen-vr-introduces-telos-the-breakthrough-ai-agent-for-unreal-blueprints-302561368.html), [unreal-university.blog](https://www.unreal-university.blog/this-ai-can-now-generate-blueprints-in-unreal-engine/)

---

### 3. Other UE AI Tools — Blueprint Generation Patterns

#### Kibibyte Blueprint Generator AI

**Approach:** Single-shot function generation. User writes a prompt → one LLM call → returns a Blueprint function. Functions appear in 5-10 seconds. Uses Gemini (free) or OpenAI (~3,300 calls per $1).

**Mechanism:** The LLM generates a description or structured representation; the plugin converts it to Blueprint nodes. The conversion mechanism is not documented. Given the speed and simplicity, this is almost certainly a single API call with a structured output format.

**Quality:** Beta-level stability issues ("most the time it crashes or the prompt screen bugs out"). Individual function scope only — not system-level multi-Blueprint generation.

Source: [FAB listing](https://www.fab.com/listings/6aa00d98-0db0-4f13-8950-e21f0a0eda2c), [forums.unrealengine.com](https://forums.unrealengine.com/t/kibibyte-blueprint-generator-ai-kibibyte-labs-engine-assistant/2510675)

#### Ultimate Engine Co-Pilot (formerly Ultimate Blueprint Generator)

**Approach:** MCP-based, multi-turn conversational. Supports OpenAI, Claude, Gemini, DeepSeek, plus local LLMs (Ollama, LM Studio). Also exposes tools to Claude Desktop/Cursor via MCP.

**Blueprint mechanism:** Per forum: "add logic directly after [a selected node]" — suggesting graph editing is incremental and node-anchored. The AI creates and connects actual Blueprint nodes (not text code that gets compiled). Project-wide scan builds a knowledge base for context-aware architecture questions.

**Failure handling:** Developer warns "AI-generated content can sometimes be unpredictable or incorrect. Review before production use." No structured self-correction.

Source: [forums.unrealengine.com](https://forums.unrealengine.com/t/ultimate-blueprint-generator-the-ai-co-pilot-for-unreal-engine/2618922), [gamedevcore.com](https://www.gamedevcore.com/docs/bpgenerator-ultimate)

#### Open-Source UE MCP Servers (GitHub Ecosystem)

Several open-source projects expose Unreal Engine to AI via MCP. All follow the same pattern as AIK's MCP path: tool-server only, agent decides the workflow.

Notable ones:
- **chongdashu/unreal-mcp**: Python MCP server + C++ plugin, TCP socket on port 55557. Tools: add event nodes, create function calls, connect pins, add variables. Per-node iterative approach.
- **flopperam/unreal-engine-mcp**: Full Blueprint creation and modification via MCP.
- **ChiR24/Unreal_mcp**: TypeScript + C++ + Rust (WebAssembly), 36 MCP tools, native C++ Automation Bridge plugin.
- **VibeUE**: Blueprint and UMG guide with structured MCP tools.

**Common limitation:** None have plan validation, function alias maps, or structured error recovery. Pin name errors and function hallucination fail silently or require the agent to correct on its own.

Source: [github.com/chongdashu](https://github.com/chongdashu/unreal-mcp), [github.com/flopperam](https://github.com/flopperam/unreal-engine-mcp), [github.com/ChiR24](https://github.com/ChiR24/Unreal_mcp)

---

### 4. General AI Coding Agent Patterns — Relevant to Blueprint Generation

#### Claude Code — Agentic Loop Architecture

Claude Code's loop: **Gather context → Take action → Verify results → Repeat**. Each tool use returns information that feeds the next decision. "Chaining dozens of actions together."

**Context management:**
- Auto-compaction at ~95% context capacity; older tool outputs cleared first, conversation summarized
- Skills (on-demand loading), Subagents (isolated fresh context), CLAUDE.md (persistent cross-session)
- Subagents: spawned for delegation, get their own clean context, return summary to parent. "Teammates spawn within 20-30 seconds, produce results within first minute."

**Multi-agent performance:** Internal Anthropic testing: multi-agent (Opus as coordinator, Sonnet subagents) outperformed single-agent Opus by 90.2% on breadth-first research tasks. BUT: researchers found that "most coding tasks involve fewer truly parallelizable tasks than research" — parallel agents help research more than sequential coding.

**Token cost:** Subagents use ~4x tokens vs standard chat. Multi-agent systems use ~15x vs standard chat. Parallelism is expensive.

Source: [code.claude.com/how-claude-code-works](https://code.claude.com/docs/en/how-claude-code-works), [anthropic.com/engineering/multi-agent-research-system](https://www.anthropic.com/engineering/multi-agent-research-system)

#### Roo Code — Boomerang Orchestrator

**Key pattern:** Orchestrator has no file-read/write/command access. Deliberately stripped to prevent it from doing implementation work instead of delegating. "If Orchestrator has edit rights, it tries to do the work itself rather than delegate."

**Context isolation:** Each subtask gets a completely fresh context. Instructions flow DOWN through the `message` parameter (fully explicit, no automatic inheritance). Summaries return UP via `attempt_completion`. Full subtask history stays in subtask context — orchestrator never sees implementation noise.

**Why this matters for Blueprint generation:** If Olive ever uses a planner→builder split, the Planner should NOT have write access to Blueprint graphs. Otherwise it will try to build incrementally instead of planning.

Source: [docs.roocode.com/boomerang-tasks](https://docs.roocode.com/features/boomerang-tasks), `plans/research/multi-agent-architectures.md`

#### Cursor / Windsurf — Cascade Architecture

Both use a persistent single-agent model (Cascade / Windsurf Flow) that maintains context-aware state across multi-step tasks. Windsurf's approach:
- RAG retrieval + active Memories + context engine tracks edits and navigation
- Flows: multi-step reasoning chains, each step explainable and reversible
- MCP integration for external tools (connects to data sources, external services)

**Cursor 2.0 (March 2026):** Agent-first, event-driven system. Composer model (proprietary) trained with codebase-wide semantic search. Claims "4x faster than similarly intelligent models," under 30s per turn. Multi-agent workspace: up to 8 parallel agents for ensemble approach on complex problems.

Source: [techcrunch.com](https://techcrunch.com/2026/03/05/cursor-is-rolling-out-a-new-system-for-agentic-coding/), [cursor.com/product](https://cursor.com/product)

#### Aider — Architect/Editor Split

Two models: an expensive architect model produces code changes, a cheap editor model applies them mechanically. Full repo map (function signatures only, no bodies) is injected upfront at every turn — eliminates the need for read-before-write. Map is 5-8x smaller than full content and is ranked via PageRank on dependency graph.

Source: `plans/research/multi-agent-architectures.md`, `plans/research/context-management-approaches.md`

---

### 5. Academic Research — Tool Call Overhead and Batch IR Approaches

#### SimpleTool: Parallel Decoding for Function Calling (arxiv 2603.00030)

Proposes parallel token decoding for function calls — exploiting idle GPU compute during memory-bandwidth-bound decode phase. Achieves 3-6x speedup with negligible overhead. Sub-100ms function calling with 4B models + quantization. **Relevant:** Parallelizing independent tool calls (e.g., adding nodes that don't depend on each other) could reduce wall-clock time significantly.

Source: [arxiv.org/html/2603.00030](https://arxiv.org/html/2603.00030)

#### AgentDiet: Trajectory Reduction (arxiv 2509.23586)

Reflection step and agent step can be parallelized in latency-critical scenarios; reflection overhead is 5-10% of cost. Tool reduction: selectively reducing tool count significantly improves function-calling performance, execution time, and power efficiency.

Source: [arxiv.org/pdf/2509.23586](https://arxiv.org/pdf/2509.23586)

#### "When Do Tools and Planning Help LLMs?" (arxiv 2601.02663)

Key finding: tools and planning help for structured knowledge tasks (QA improved from 47.5% → 67.5%) but add latency "by orders of magnitude" (8s → 317s per example). For open-ended tasks, one-shot prompting is strongest. Tools should only be used when the task genuinely requires external state.

**For Blueprint generation:** This suggests that for simple/small Blueprints, a single-shot IR approach (generate the whole graph in one call) beats tool-per-node iteration. For complex multi-Blueprint systems with state dependencies, iterative tools with verification are necessary.

Source: [arxiv.org/html/2601.02663](https://arxiv.org/html/2601.02663)

---

### 6. Speed vs Quality: Empirical Observations

| Tool | Complex BP Time | Quality | Failure Mode |
|------|----------------|---------|-------------|
| Aura (Telos 2.0) | Claimed "seconds" for moderate tasks | Good for prototyping; asset refs fail | Asset references broken; scaling errors |
| NeoStack AIK + Claude Code | 2-20+ min (agent-dependent) | Highly variable; agent-dependent | Wrong pin names, missing function aliases |
| Kibibyte | 5-10 seconds per function | Beta; crashes common | Function-scope only; no system-level |
| Ultimate Co-Pilot | Not benchmarked | Iterative; needs review | Unpredictable, user warned |
| Olive (from our own session logs) | 15-36 min for 3-BP system | 77-88% tool success, compiles OK | plan_json 43-67% initial fail rate |

Olive's own failure patterns from session logs (`bow-arrow-session-log-analysis-2026-03-08*.md`):
- Hallucinated function names (ProjectileMovement setters): 3 retries each
- `_C` suffix on add_component calls
- K2Node_CallFunction property format errors
- describe_node_type catalog gaps (SetFieldOfView, K2_AttachToComponent) → function gutted on retry

---

## Recommendations

### For Olive's Architect

**1. Aura's likely speed advantage is batch IR, not fewer tool calls.**
The "10x speed" claim is most plausibly explained by Telos generating a complete graph representation in one LLM call (a structured JSON/IR of all nodes and connections), then executing it in one Python pass. Per-node-per-tool-call iteration cannot achieve "seconds" for moderate systems. Olive's `blueprint.apply_plan_json` is architecturally the closest thing we have to this pattern. The existing plan IR is our speed advantage — we need to make it robust, not deprecate it.

**2. NeoStack AIK is not a Blueprint generator — it is a tool server.**
AIK's quality and speed are entirely determined by the external agent (Claude Code). AIK contributes nothing to generation strategy. The 500+ validation checks prevent crashes but do not improve semantic correctness. This means comparing Olive to AIK directly is comparing systems at different layers — Olive bundles the tool server AND the generation strategy; AIK only bundles the tool server.

**3. The plan_json approach outperforms per-node iterative for medium-complexity graphs.**
Evidence: (a) Aura likely uses batch graph submission for speed; (b) arxiv 2601.02663 shows tool-per-step adds "orders of magnitude" latency; (c) Olive's own session logs show 15-20 min for a 3-BP system using plan_json — even with 43-67% initial failure rates, plan_json is faster than the alternative because it collapses N tool calls into 1 LLM call. Improving plan_json reliability is higher leverage than adding per-node tools.

**4. Pin name and function resolution is the dominant failure mode across all tools.**
Every tool in this landscape — AIK+Claude Code, open-source MCP servers, Olive — suffers from:
- Hallucinated function names (functions that don't exist on the target class)
- Wrong pin names (UE uses internal names that differ from display names)
- Missing _C suffix issues, property vs function confusion

Olive has an 180-entry alias map and `FindFunctionEx()` with a 7-step search order. This is ahead of competitors. Gaps remain in the catalog (describe_node_type misses for SetFieldOfView, K2_AttachToComponent). Priority: fill catalog gaps before adding new ops.

**5. Telos 2.0's "read before write" approach explains the 99% accuracy on existing graphs.**
Reading the current Blueprint state before editing it — and feeding that serialized state into context — is the single most impactful practice for existing-BP accuracy. This is what Olive's `blueprint.read_graph` tool enables. The Builder should always read before writing to an existing Blueprint.

**6. The Roo Code Orchestrator pattern is the right model for Olive's agent pipeline.**
Olive's Planner (CLI pipeline: Scout → Planner → Builder) matches the Roo Code philosophy:
- Orchestrator (Planner) has NO write access — it plans, it doesn't build
- Builder gets isolated context, clean slate
- Summary bubbles up (Reviewer → brief back to user)
The one deviation from Roo's pattern: Olive's Planner DOES read Blueprint data (via Scout). This is correct — it needs to see what exists. But the Planner's write restriction should be enforced.

**7. Multiple parallel agents for Blueprint generation is expensive and not clearly better for coding tasks.**
Anthropic's own research: multi-agent (15x token cost) helps research tasks (90% improvement) but coding tasks "involve fewer truly parallelizable tasks." For Blueprint generation, where steps are sequentially dependent (create node → wire it → compile), parallelism gives little benefit. Olive should NOT pursue parallel Blueprint builders for now. Invest in better single-agent execution quality instead.

**8. Aura's Plan mode is a UX pattern worth adopting.**
Plans stored as human-readable markdown in a versioned folder, user can inspect and edit before execution. Olive's `blueprint.preview_plan_json` achieves similar preview functionality but the output is JSON, not natural language. A plan-to-markdown rendering step for user review would improve UX at low cost.

**9. Tool count reduction improves performance.**
AgentDiet research confirms: fewer tools available = better function-calling performance. Olive's Focus Profiles + Tool Pack system already implements this philosophy. Ensure Blueprint-only sessions do not expose Python/BT/PCG/C++ tools unnecessarily. The tool pack system gates this correctly — verify it actually reduces the MCP tool list exposed to the agent.

**10. Aura's Dragon Agent = Olive's `editor.run_python` at full autonomy.**
For tasks outside plan_json's vocabulary (timeline tracks, complex animation, scene population), Olive's Python tool already matches Aura's core autonomous execution mechanism. The differentiation is that Olive's Python tool has snapshot/rollback, persistent logging, and auto-error wrapping. This is a genuine advantage — make it visible to users.

---

## Sources

- [NeoStack AIK Documentation](https://aik.betide.studio/)
- [Betide Studio NeoStack](https://betide.studio/neostack)
- [NeoStack AIK on assetsue.com](https://assetsue.com/file/agent-integration-kit-neostack-ai)
- [Aura Unreal AI Agent — About](https://www.tryaura.dev/about/)
- [Aura Blueprint Generation Updates](https://www.tryaura.dev/updates/unreal-ai-agent-blueprints)
- [Aura: AI Agent for Unreal Editor (UE Forums)](https://forums.unrealengine.com/t/aura-ai-agent-for-unreal-editor/2689209)
- [Aura 12.0 Press Release (Games Press)](https://www.gamespress.com/en-US/Next-Evolution-of-Best-In-Class-Multi-agent-AI-Assistant-for-Unreal-En)
- [Ramen VR Introduces Telos (PR Newswire)](https://www.prnewswire.com/news-releases/ramen-vr-introduces-telos-the-breakthrough-ai-agent-for-unreal-blueprints-302561368.html)
- [Aura 12.0 Technical Summary (BriefGlance)](https://briefglance.com/articles/aura-120-redefines-unreal-engine-workflow-with-autonomous-ai)
- [Unreal University — Aura Blueprint Review](https://www.unreal-university.blog/this-ai-can-now-generate-blueprints-in-unreal-engine/)
- [Kibibyte Blueprint Generator AI (UE Forums)](https://forums.unrealengine.com/t/kibibyte-blueprint-generator-ai-kibibyte-labs-engine-assistant/2510675)
- [Ultimate Engine Co-Pilot (UE Forums)](https://forums.unrealengine.com/t/ultimate-blueprint-generator-the-ai-co-pilot-for-unreal-engine/2618922)
- [chongdashu/unreal-mcp (GitHub)](https://github.com/chongdashu/unreal-mcp)
- [flopperam/unreal-engine-mcp (GitHub)](https://github.com/flopperam/unreal-engine-mcp)
- [Claude Code — How It Works](https://code.claude.com/docs/en/how-claude-code-works)
- [Anthropic — Multi-Agent Research System](https://www.anthropic.com/engineering/multi-agent-research-system)
- [Roo Code — Boomerang Tasks](https://docs.roocode.com/features/boomerang-tasks)
- [Cursor Product (Cursor 2.0)](https://cursor.com/product)
- [Cursor 2.0 — TechCrunch](https://techcrunch.com/2026/03/05/cursor-is-rolling-out-a-new-system-for-agentic-coding/)
- [SimpleTool: Parallel Decoding (arxiv 2603.00030)](https://arxiv.org/html/2603.00030)
- [AgentDiet: Trajectory Reduction (arxiv 2509.23586)](https://arxiv.org/pdf/2509.23586)
- [When Do Tools and Planning Help? (arxiv 2601.02663)](https://arxiv.org/html/2601.02663)
- Internal: `plans/research/neostack-deep-dive-2026-03.md`
- Internal: `plans/research/aura-and-competitors-2026-03.md`
- Internal: `plans/research/competitor-deep-dive-2026-03.md`
- Internal: `plans/research/multi-agent-architectures.md`
- Internal: `plans/research/context-management-approaches.md`
- Internal: `plans/research/error-recovery-patterns.md`
- Internal: `plans/research/bow-arrow-session-log-analysis-2026-03-08*.md` (session performance data)
