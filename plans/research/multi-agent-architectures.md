# Research: Multi-Agent Architectures for AI Coding Tools

## Question

How do existing open-source tools (Roo Code, Aider, Devin, Claude Code, CrewAI/LangGraph) implement agent pipelines to produce higher quality output? What patterns, agent types, and token optimization strategies are used in practice?

---

## Findings

### 1. Roo Code — Boomerang/Orchestrator Architecture

#### Mode System Overview

Roo Code has five built-in modes, each with a restricted tool set:

| Mode | Tool Access | Role |
|------|------------|------|
| Code | read, edit, command, MCP (all tools) | Primary implementation |
| Architect | read, MCP, edit (markdown only) | System design, planning |
| Ask | read, MCP (no edit or command) | Q&A, explanation |
| Debug | read, edit, command, MCP (all tools) | Systematic troubleshooting |
| Orchestrator | new_task only (no read/write/command/MCP) | Workflow coordination |

The deliberate removal of edit/command/MCP from the Orchestrator is architecturally significant: it forces delegation and prevents the orchestrator from accumulating context from implementation work. The documented issue was that if the Orchestrator has edit/command rights, it tries to do the work itself rather than delegate.

#### Boomerang (new_task) Protocol

The `new_task` tool takes two parameters:
- `mode`: which specialized mode handles the subtask (e.g., `"code"`, `"architect"`)
- `message`: complete instructions plus all necessary context

**Context flows:**
- **Down**: entirely through the `message` parameter. The parent must pack in everything the subtask needs — prior results, scope boundaries, explicit completion criteria. There is no automatic context inheritance.
- **Up**: only via `attempt_completion` with a `result` string. The full subtask history (file reads, intermediate steps, diffs) stays in the subtask's context window. The orchestrator only sees the summary.

This is the key design insight: summaries propagate up, details stay isolated. The orchestrator sees a growing list of concise results, not a context poisoned with implementation noise.

#### Prompt Structure (per-mode)

Roo generates prompts dynamically per mode. The system prompt is assembled in this order:
1. Role definition (mode-specific identity and behavioral rules)
2. Environment context (OS, shell, git state, working directory)
3. Tool descriptions (filtered by mode's allowed groups)
4. Custom instructions (project `.roo/` files, user preferences)
5. Constraint information (token budget, context window state)

Each mode has a `roleDefinition` string (prepended to every system prompt) and a `whenToUse` field (used for automatic routing, not displayed in UI). Persistent model assignment per mode — each mode remembers its last-used model, enabling cheap-model-for-orchestration, expensive-model-for-coding patterns.

#### Context Condensation (Experimental)

Roo added "Intelligent Context Condensation" in late 2025 — automatic compression when the context window approaches capacity. The trigger is approximately 95% capacity (configurable via `CLAUDE_AUTOCOMPACT_PCT_OVERRIDE`). A hook system (`PostCompact`) lets users re-inject critical instructions after compaction.

Source: [Boomerang Tasks docs](https://docs.roocode.com/features/boomerang-tasks), [Using Modes docs](https://docs.roocode.com/basic-usage/using-modes/), [System Prompt Generation (DeepWiki)](https://deepwiki.com/RooVetGit/Roo-Code/2.5-system-prompt-generation), [PR #2934](https://github.com/RooCodeInc/Roo-Code/pull/2934), [Orchestrator issue](https://github.com/RooCodeInc/Roo-Code/issues/3400)

---

### 2. Aider — Architect/Editor Mode Split

Aider's primary contribution to multi-agent patterns is the **two-model inference split**:

1. **Architect model** (e.g., o1-preview, Claude Opus): Reasons about the coding problem. Describes the solution in natural language. No format constraints — it can think however it wants.
2. **Editor model** (e.g., DeepSeek, o1-mini, Claude Sonnet): Receives the Architect's description and converts it to well-formed code edits. Focused entirely on formatting and applying changes correctly.

**Why this works:** Splitting "code reasoning" from "code editing" lets each model specialize. The Architect is free from formatting concerns; the Editor is free from reasoning concerns. Benchmark result: o1-preview + DeepSeek achieved 85% on Aider's code editing benchmark vs. 79.7% for the previous single-model best.

**Communication pattern:** Natural language from Architect to Editor. No structured format required from the Architect — it outputs however is natural for a reasoning model. The Editor is prompted with the Architect's output plus the target files.

**Key insight for Olive:** This is essentially what Olive's plan_json already does — the AI reasons about "what" (plan_json ops), and the executor handles "how" (Blueprint API calls). The Architect/Editor split validates this separation. The next step is ensuring the Architect step has enough context to reason well without seeing implementation noise.

Source: [Aider architect/editor blog post](https://aider.chat/2024/09/26/architect.html), [Aider modes docs](https://aider.chat/docs/usage/modes.html)

---

### 3. Claude Code — Subagent Architecture

Claude Code's built-in subagent system is the most directly relevant to Olive's constraints (single-shot `--print` invocations).

#### Built-in Subagents

| Agent | Model | Tools | Purpose |
|-------|-------|-------|---------|
| Explore | Haiku (fast) | Read-only | File discovery, codebase search |
| Plan | Inherits | Read-only | Research before presenting a plan |
| General-purpose | Inherits | All | Complex multi-step tasks |
| Bash | Inherits | Terminal | Command execution in separate context |

**Key design:** The Explore agent uses Haiku specifically because exploration is high-volume, low-precision work. The expensive model stays in the main conversation for synthesis and decision-making.

#### Subagent Definition Format

```markdown
---
name: code-reviewer
description: Expert code reviewer. Proactively reviews code for quality, security. Use immediately after code changes.
tools: Read, Grep, Glob, Bash
model: sonnet
---

You are a senior code reviewer...
```

The `description` field drives automatic delegation — Claude routes to the agent when the task matches. "Use proactively after code changes" is a trigger phrase that causes automatic delegation without explicit user request.

#### Context Isolation Pattern

- Subagents have their own context windows. Verbose output (test runs, file analyses) stays in the subagent's window.
- Only the final `result` returns to the main conversation.
- Subagents cannot spawn other subagents (prevents nesting, which would re-introduce context bloat in the parent).
- Transcripts persist at `~/.claude/projects/{project}/{sessionId}/subagents/agent-{id}.jsonl` — enabling resume across sessions.

#### Tiered Model Routing

The explicit pattern: route to Haiku for exploration/routing tasks, inherit main model for reasoning, use Sonnet for analysis. The cost difference between Haiku and Opus is roughly 50:1. A well-designed router can save 80% of tokens on high-volume low-complexity operations.

Source: [Claude Code subagents docs](https://code.claude.com/docs/en/sub-agents)

---

### 4. Devin 2.0 — Interactive Planning + Parallel Execution

Devin's architecture centers on two innovations relevant to multi-agent pipelines:

**Interactive Planning:** Before execution, Devin researches the codebase and produces a preliminary plan. Users can modify the plan before Devin proceeds. This matches the Architect pattern but adds a human-in-the-loop checkpoint. The plan serves as a contract between the planning phase and the execution phase.

**MultiDevin (Parallel Agents):** One Devin instance can spawn sub-Devins for concurrent execution of independent subtasks. Each sub-agent gets its own isolated cloud IDE (separate VM). Results are collected and synthesized by the orchestrating Devin.

**Devin Wiki:** Automatic repository indexing that generates structured documentation (architecture diagrams, source links, component descriptions). This is pre-cached context that agents reference rather than re-exploring the codebase on each task — a practical solution to the repeated context gathering problem.

Source: [Cognition Devin 2.0 announcement](https://cognition.ai/blog/devin-2), [Coding Agents 101](https://devin.ai/agents101), [Devin 2.0 technical analysis](https://www.analyticsvidhya.com/blog/2025/04/devin-2-0/)

---

### 5. Production Context Engineering Patterns (Google ADK + General)

From the Google ADK production architecture and context engineering research:

#### Context as a Compiled View

The key mental model: context is a **compiled view over a richer stateful system**, not a mutable text buffer. Separate:
- **Storage**: durable session logs, searchable memory, large artifact objects
- **Presentation**: ephemeral per-call views assembled from storage
- **Processing**: explicit transformer pipeline that builds the view

This prevents the common failure where context balloons because every agent appends to a shared buffer.

#### Artifact Externalization

When a tool call returns 5,000 tokens, store it externally with a unique identifier. Keep only a 100-token summary in active context. Agents load artifacts on-demand via an explicit tool call. The content then drops from context after the call.

Target compression ratios:
- Historical conversation: 3:1 to 5:1
- Tool outputs: 10:1 to 20:1

At these ratios, production benchmarks show 40-60% cost reduction with maintained or improved task completion.

#### Tiered Context Structure

```
Tier 1: Current objective + immediate constraints (always present, highest attention)
Tier 2: Last 3-5 conversation turns + active session state
Tier 3: Historical summaries (compressed from older turns)
Tier 4: External storage (loaded on-demand via tool call)
```

Restructure at each turn to maintain this priority ordering. Models have recency bias — material near the end gets more attention than material buried in the middle.

#### Agents-as-Tools vs. Agent Transfer

Two multi-agent models:
- **Agents as Tools**: Specialized agents receive focused invocations with minimal context. Results return without history bleed. Used for contained subtasks.
- **Agent Transfer (Hierarchy)**: Full control handoff where sub-agents inherit scoped session views. ADK's `include_contents` parameter controls scope — defaults to "none" (minimal context) unless the sub-agent genuinely benefits from full history.

The `include_contents="none"` default is critical: sub-agents see only what they're explicitly given, preventing the "context inheritance tax" that plagues naive multi-agent systems.

#### Tool Quantity and Attention

Berkeley benchmarks finding: "every model performs worse when presented with excessive tools." Recommendation: filter from 46 tools to ~15 based on task type. For Olive, this maps directly to the Tool Pack system — only present tools relevant to the current turn's intent.

Source: [Google ADK architecture post](https://developers.googleblog.com/architecting-efficient-context-aware-multi-agent-framework-for-production/), [Context engineering production strategies](https://www.getmaxim.ai/articles/context-engineering-for-ai-agents-production-optimization-strategies/)

---

### 6. Reflection/Critic Agents — Quality Patterns

#### What the Research Shows

Self-reflection in LLM agents shows measurable quality improvement but with important caveats:

- The "CRITIC" pattern (external tool-verified outputs) shows 10-30 percentage point accuracy improvements. External verification (compile, test, type check) is more reliable than LLM self-evaluation.
- Simple self-reflection (ask model to critique its own output) produces marginal quality gains with significant cost and latency increase.
- Multi-turn self-refinement (Reflexion) shows diminishing returns quickly — first correction is highest value, subsequent corrections rarely justify the cost.
- Process reward models (PRM) — giving feedback at each reasoning step rather than only at the end — are more effective than end-state reflection.

**Practical rule:** Use external feedback signals (compile errors, test failures, structural checks) as the reflection trigger rather than asking the LLM to self-evaluate. LLMs are poor judges of their own outputs but good at incorporating concrete error messages.

#### Olive's Current Pattern vs. Best Practice

Olive's self-correction (FOliveSelfCorrectionPolicy) already follows best practice:
- Trigger is concrete tool failures and compile errors (external feedback), not self-evaluation
- Progressive disclosure (terse → full → escalate) avoids token waste on obvious errors
- Plan content deduplication (PreviousPlanHashes) prevents circular correction loops

The gap vs. best practice is the absence of a **pre-execution verification step** (a plan validator agent that catches structural issues before the costly execution attempt).

Source: [Self-Reflection in LLM Agents paper](https://arxiv.org/pdf/2405.06682), [Reflection agent prompting](https://www.akira.ai/blog/reflection-agent-prompting), [Hugging Face reflection trends 2026](https://huggingface.co/blog/aufklarer/ai-trends-2026-test-time-reasoning-reflective-agen)

---

### 7. SPARC Orchestration Pattern (Community Pattern)

The SPARC methodology (Specification → Pseudocode → Architecture → Refinement → Completion) is a community-developed pattern for Roo Code that maps well to complex coding tasks:

- **S — Specification**: Orchestrator gathers requirements, delegates to Ask mode for clarification
- **P — Pseudocode**: Architect mode produces algorithm/logic outlines without code
- **A — Architecture**: Architect mode designs component structure and interfaces
- **R — Refinement**: Code mode implements, Debug mode fixes, repeat until passing
- **C — Completion**: Orchestrator verifies outputs, generates documentation, prepares PR

This is a 5-agent sequential pipeline where each agent's `attempt_completion` output becomes the next agent's `message` input. The key is that each stage produces a concrete artifact (requirements doc, pseudocode, architecture diagram, implementation, test results) rather than a generic response.

Source: [SPARC Orchestration gist](https://gist.github.com/ruvnet/a206de8d484e710499398e4c39fa6299), [roocode-modes repository](https://github.com/enescingoz/roocode-modes)

---

### 8. Claude Code Power Patterns (Headless Mode)

Directly applicable to Olive's `--print` single-shot invocations:

**Session resumption via ID:** Capture session ID from `--print` output, resume with prior context in subsequent calls. Enables multi-step pipelines where each step is a separate process invocation but maintains continuity.

**Worktree parallelization:** Multiple Claude Code instances on separate git worktrees. Each operates without conflicts. Results collected and merged by an orchestrating process. The isolation boundary is the filesystem branch, not an in-process context.

**MCP lazy-loading:** 12 active MCP servers consume 66,000 tokens (one-third of 200K window). Lazy-loading or per-session server selection recovers this budget. For Olive's CLI provider, this means not advertising all tools in every invocation — only what's needed.

**Lean context discipline:** CLAUDE.md under 100 lines. Every line loads on session start and survives compaction. Olive's current knowledge injection (cli_blueprint.txt etc.) should be subject to the same discipline — content that changes the AI's behavior is worth the tokens; background knowledge that the AI could infer is not.

Source: [Roo Code power usage blog](https://ocdevel.com/blog/20250331-roo-code-power-usage)

---

## Agent Types Beyond the Current Pipeline

Based on the research, the following agent types appear in production systems and are directly relevant to Olive:

| Agent Type | Role | When to Use |
|-----------|------|-------------|
| **Scout/Explorer** | Read-only codebase search, Haiku-class | Before any write operation, to locate context |
| **Planner/Architect** | Designs approach, produces artifact (plan doc) | Tasks with 3+ implementation steps |
| **Validator/Preflight** | Structural check before execution | Before expensive tool calls (plan_json apply) |
| **Builder/Coder** | Implementation, full tool access | After Architect produces design |
| **Verifier/Critic** | External-feedback driven (compile, test, structural) | After Builder completes |
| **Summarizer** | Compresses prior context to 100-token digests | When context window exceeds 70% |
| **Router** | Classifies intent, selects pipeline depth | Entry point, cheap model |

The Router agent is underused in current tools. Cheap routing (Haiku-class) to classify "is this a read, a simple write, or a complex write" before spending tokens on expensive models represents the highest ROI optimization.

---

## Recommendations

### For the Scout → Researcher → Architect → Builder → Reviewer Pipeline

**1. The Boomerang protocol is the right pattern for the pipeline.** Each agent:
- Receives everything it needs in a single message (no automatic context inheritance)
- Returns only a concise summary artifact (not its full work history)
- Uses the `attempt_completion` equivalent to signal completion with a structured result

For Olive's CLI sub-agents (single-shot `--print` invocations), this means:
- The orchestrating agent packs its full context summary into the CLI `--print` prompt
- The CLI agent writes its result to a file or stdout
- The orchestrator reads only the result file, not the CLI agent's full exchange

**2. Strict tool access per agent stage.** Roo Code's insight that "if the Orchestrator has edit rights, it tries to do the work itself" applies directly. Olive's Scout should be read-only; the Architect should produce only a plan document (no tool writes); only the Builder gets write tools. This enforces the intended role separation.

**3. Add a Validator agent between Architect and Builder.** This maps to Olive's FOlivePlanValidator Phase 0, but elevated to a separate agent invocation. The Validator:
- Reads the Architect's plan
- Checks structural preconditions (does the target Blueprint exist? Are referenced functions resolvable?)
- Returns a validation report — pass or list of specific issues with suggestions
- Does NOT attempt to fix issues (that's Architect's job if re-routed)

This front-loads cheap structural checks before the expensive Builder execution.

**4. Use cheap models for Scout and Router, expensive for Architect and Builder.** Concrete model allocation:
- Scout (codebase exploration): Haiku-class (fast, cheap, tool-restricted to read-only)
- Router (intent classification): Haiku-class (single-turn classification)
- Architect (plan design): Opus-class or Sonnet (reasoning quality matters here)
- Builder (implementation): Sonnet or Opus (depends on task complexity)
- Reviewer/Verifier: External tools first (compile, structural checks), Sonnet only if LLM analysis is needed

**5. Externalize verbose tool outputs.** When a Blueprint read returns 8,000 tokens, store it in a temp file and pass only the path (or a 200-token summary) to the next agent in the chain. The Builder can load the full content via a read tool call. This prevents context bloat in the orchestrating agent.

**6. The Aider Architect/Editor split validates Olive's plan_json design.** The plan_json preview → apply cycle IS the architect/editor split. The AI reasons about "what" in the preview step; the executor handles "how" in the apply step. The recommendation is to lean into this — keep the plan_json format as the primary artifact that flows between agents (Architect produces it; Validator checks it; Builder executes it).

**7. For the Reviewer agent, prefer external feedback over LLM self-evaluation.** The Reviewer should run: compile (FOliveCompileManager), structural integrity checks (FOliveValidationEngine), orphaned exec detection. Only escalate to LLM analysis for issues that tools cannot detect (semantic intent mismatches, architectural inconsistencies). LLM-only review has weak signal.

**8. Pre-cache a project knowledge artifact.** Devin's "Wiki" pattern: generate a structured summary of the target Blueprint's current state, parent class, components, and implemented interfaces once at the start of the pipeline. Pass this artifact (not re-read from disk) to each subsequent agent. This eliminates repeated Blueprint reads across agents and keeps the per-agent context lean.

### Token Optimization Priorities (in order of impact)

1. **Model tiering** — biggest single impact. Haiku for Scout/Router is 50x cheaper than Opus.
2. **Tool quantity** — present only tools relevant to current agent's role. Read-only agents don't need write tool descriptions in their prompt.
3. **Artifact summarization** — large tool outputs stored externally, 100-token summaries in context.
4. **Pipeline short-circuit** — single-step requests (add one node, read asset state) should bypass the full pipeline entirely. Router classification prevents paying for Architect reasoning on trivial tasks.
5. **Context size discipline** — CLAUDE.md and knowledge injection files should be measured by impact per token, not completeness. Behavioral rules are worth the tokens; encyclopedic API reference is not.

### Known Risks

- **Self-reflection diminishing returns:** Adding a Reviewer LLM agent after Builder adds cost and latency for marginal quality gain. Favor compile+structural feedback loops over LLM criticism.
- **Context accumulation in orchestrator:** If the orchestrator receives full subtask histories rather than summaries, it will hit context limits on long tasks. Enforce summary-only returns from sub-agents.
- **Validator agent as blocker:** A mandatory Validator before every Builder invocation adds one full agent round-trip. Consider making it optional (only for multi-step plans above a threshold, matching Olive's `PlanFirstGraphRoutingThreshold` pattern).
- **Single-shot CLI constraint:** Olive's CLI sub-agents cannot be long-lived. Each agent must complete its work in one invocation. Tasks that require iteration belong in the main conversation loop, not delegated to CLI sub-agents.
