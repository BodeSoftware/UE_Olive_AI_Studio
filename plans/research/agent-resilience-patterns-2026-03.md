# Research: Agent Resilience Patterns — Self-Correction, Plan Refinement, Error Routing, Context Management

## Question

How do Aura and other sophisticated agent frameworks (Devin, SWE-Agent, OpenHands, Claude Code) handle the following problems as they apply to an AI agent building Blueprint graphs via MCP tools:

1. Self-correction after partial failures (agent abandons instead of fixing specific errors)
2. Plan refinement based on discoveries made during research/execution
3. Progressive error handling with specific routing for codes like RATE_LIMITED
4. Context window management on long multi-Blueprint operations

---

## Findings

### Aura / Telos Architecture

Aura's "Telos 2.0" is proprietary and technically undocumented. Press releases claim it is a reasoning framework by MIT-trained researchers that achieves "25x error reduction" and ">99% accuracy on existing BP graphs." No architecture papers, source code, or engineering posts have been published. The public-facing documentation for Aura covers only user-facing workflow guidance: plan end-to-end before generating, use iterative review between accepts, break systems into small parts.

**Takeaway:** Telos is a black box. We cannot learn implementation details from Aura. Broaden to openly-documented frameworks.

Source: https://www.prnewswire.com/news-releases/ramen-vr-introduces-telos-the-breakthrough-ai-agent-for-unreal-blueprints-302561368.html
Source: https://www.tryaura.dev/updates/unreal-ai-agent-blueprints

---

### Sub-Topic 1: Self-Correction After Partial Failures

#### The Core Problem — Why Agents Abandon

Research confirms this is a widely-documented and structurally-caused failure. Claude Code GitHub issue #6159 describes the exact pattern: the agent generates a plan, completes several phases, then stops and reports success as if the entire task is done. This is not a random failure — it is a systematic tendency caused by:

- **No plan execution loop**: The agent has no mechanism to check its own remaining todo items before halting.
- **No forced continuation**: Without an explicit stop-hook that blocks exit until the plan is complete, the agent defaults to halting after any reasonable-looking partial result.
- **Context dilution**: As context grows, earlier plan steps receive less attention weight. The agent "forgets" it had more to do.

The proposed fix from that issue is a system-level stop hook:

```
Before allowing stop:
- Check if todo list has pending items
- If yes, inject: "System: Your plan is not yet complete.
  The following tasks remain: [list].
  Please continue with the next step."
- Block exit until list is complete OR user cancels.
```

Source: https://github.com/anthropics/claude-code/issues/6159

#### The "Fail Expensively" Anti-Pattern

OpenHands performance data shows that when agents fail, they take 3x longer (238.9 seconds vs 79 seconds for success). When stuck, agents enter expensive repetitive loops consuming compute until an external budget limit fires. The root cause is **lack of futility detection** — agents don't know when their current approach cannot succeed and have no protocol for switching strategies.

Source: https://github.com/SWE-Gym/SWE-Gym/blob/main/docs/OpenHands.md
Source: https://medium.com/@Nexumo_/i-tried-agent-self-correction-tool-errors-made-it-worse-d6ea76a17c1c

#### What Actually Works: The 4-Category Error Router

The most concrete pattern from production agent systems is a 4-category error router with different handlers per category:

| Category | Handler | Action |
|----------|---------|--------|
| Transient (network, rate limits) | System-level | Exponential backoff + retry, never involve LLM |
| LLM-Recoverable (tool failure, wrong param) | Agent loop | Return error to LLM with diagnosis, cap at 3 reformulations |
| User-Fixable (missing data, denied) | Human interrupt | Pause with interrupt(), do not retry |
| Unexpected (bugs, unsupported features) | Let bubble up | Do not retry; report and stop |

**Critical insight from production systems:** When a tool fails, do NOT retry the same call. Send the error back to the LLM so it can reformulate. The LLM regenerating a different approach from the error context outperforms mechanical retry. But cap reformulation attempts at 3 — beyond that, route to failure handler or escalate.

Source: https://dev.to/klement_gunndu/4-fault-tolerance-patterns-every-ai-agent-needs-in-production-jih

#### Checkpoint-Based Recovery (Partial Failure)

For multi-item batch operations, checkpointing prevents full restart after partial completion:
- Save state after every node boundary
- On crash/error at item 48 of 50, restart from item 48
- In production LangGraph systems this reduced unrecoverable failures from 23% to under 2%

Direct Olive relevance: the autonomous path already takes a snapshot before a run. The gap is that within a run, there is no per-Blueprint checkpoint — if the 4th of 5 Blueprints fails, the agent restarts the whole task.

Source: https://dev.to/klement_gunndu/4-fault-tolerance-patterns-every-ai-agent-needs-in-production-jih

---

### Sub-Topic 2: Plan Refinement During Execution

#### Devin 2.0: Interactive Planning + Debug Loop

Devin 2.0 introduced Interactive Planning as an explicit phase before execution:
1. Agent parses the request and produces a preliminary plan in seconds
2. User can read and modify the plan before execution begins
3. Agent stores plan externally (Devin stores as markdown files) and checks back against it throughout execution
4. Dynamic replanning: after each major step, the planner re-evaluates remaining plan steps with new observations and can skip, add, or change future steps

The key insight is that the plan is **updated after each step**, not just used as a static dispatch list. This is the Plan-and-Act architecture (arxiv 2503.09572):

> "The Planner updates the plan after each Executor step rather than relying solely on the initial plan... The evolving plan serves as an implicit memory mechanism — the plan carries forward the relevant context, eliminating the need for explicit memory modules."

Source: https://cognition.ai/blog/devin-annual-performance-review-2025
Source: https://arxiv.org/html/2503.09572v3

#### Plan-Execute-Reflect (OpenSearch / ML-Commons)

Explicit three-phase loop:
1. **Plan**: LLM generates step-by-step plan
2. **Execute**: Execute one step, capture result
3. **Reflect**: Feed result + current plan back to planner LLM; planner can modify remaining steps

The planner LLM gets: (original task) + (execution history) + (current plan state) → either emits refined plan OR returns final result. This prevents the plan from becoming stale when the execution produces unexpected observations.

Source: https://github.com/opensearch-project/ml-commons/issues/3745

#### AdaPlanner: In-Plan vs Out-of-Plan Refiners

Research identifies two classes of plan failure requiring different refinement strategies:
- **In-Plan failure**: A step failed but the plan structure was sound → fix the step, continue
- **Out-of-Plan failure**: The plan structure itself is wrong (wrong decomposition, wrong sequence) → replan from current state

ADaPT goes further with recursive decomposition: when an executor fails, recursively decompose the failing step into smaller sub-steps rather than replanning the whole task.

Source: https://www.wollenlabs.com/blog-posts/navigating-modern-llm-agent-architectures-multi-agents-plan-and-execute-rewoo-tree-of-thoughts-and-react

#### Olive's Current Gap

Olive's `FOliveSelfCorrectionPolicy` is entirely reactive — it fires after a tool fails and injects corrective guidance for the next turn. There is no concept of plan reflection between steps when everything succeeds. When the agent finishes template research (a sequence of read-only tool calls), it has gathered information that should update its execution plan before it starts writing. Currently, no mechanism exists to prompt the agent to explicitly re-examine and refine its plan with that new information before proceeding.

---

### Sub-Topic 3: Progressive Error Handling and Error Code Routing

#### Existing System Audit

The current `FOliveSelfCorrectionPolicy` already implements a sophisticated version of this:
- 3-category classifier (`FixableMistake`, `UnsupportedFeature`, `Ambiguous`)
- Per-error-signature retry counting with oscillation detection
- Progressive disclosure (attempt 1: terse, attempt 2: full, attempt 3+: escalation + alternative strategies)
- Granular fallback mode when plan_json loops (switches to add_node/connect_pins)
- Stale error detection (cross-references compile errors against plan metadata)
- Plan deduplication (hash-based detection of identical re-submissions)

**What is missing:** RATE_LIMITED error routing. This is a transient, system-level failure — it is NOT an LLM-recoverable error and should never be fed back to the model as corrective guidance. The correct handler for RATE_LIMITED is a silent exponential backoff at the provider/transport layer, not a `FeedBackErrors` action.

#### Error Code Routing — What the Research Recommends

From MCP-specific resilience research (Octopus blog):

```
Retriable (system layer, no LLM involvement):
  HTTP 429 (rate limited)
  HTTP 5xx (server error)
  Connection timeout
  → Exponential backoff, max 3 retries, no LLM notification unless budget exhausted

Non-Retriable (feed to LLM):
  HTTP 4xx except 429 (client error — wrong params, auth failure)
  Tool logic failure (wrong node type, asset not found, pin mismatch)
  → Return to LLM with diagnostic + guidance

Non-Retriable (block immediately):
  USER_DENIED
  UNSUPPORTED_FEATURE
  → Do not retry, do not feed back as a guidance loop
```

The critical implementation requirement: RATE_LIMITED must be intercepted at the `IOliveAIProvider` / `FOliveProviderRetryManager` layer before the result reaches `FOliveSelfCorrectionPolicy`. If it leaks through to the policy, the model will try to "fix" a transient infrastructure failure, wasting context and turns.

Source: https://octopus.com/blog/mcp-timeout-retry
Source: https://sparkco.ai/blog/mastering-retry-logic-agents-a-deep-dive-into-2025-best-practices

#### Circuit Breaker for MCP Tools

For persistent services (not short-lived scripts), a circuit breaker pattern should open after 3 consecutive failures of the same MCP endpoint. While Olive's MCP server is in-process, individual tool handlers could benefit from this if they call external services (e.g., the OpenRouter provider path during network instability).

The pybreaker/purgatory pattern: track consecutive failures per endpoint → open circuit after threshold → reject calls immediately while open → probe with a single test call after timeout → close on success.

---

### Sub-Topic 4: Context Window Management on Long Operations

#### The Attention Decay Problem (Confirmed)

Research confirms: in transformer models, system prompt tokens at the beginning of the context window receive ~50% attention weight at 2K tokens, dropping to ~1% at 80K tokens. For a complex multi-Blueprint task with a 2-minute gap between tool calls, the agent's working context about the task and its constraints decays significantly.

Source: https://community.openai.com/t/solving-agent-system-prompt-drift-in-long-sessions-a-300-token-fix/1375139

#### SCAN Method (Prompt Drift Prevention)

The SCAN technique from the OpenAI community is a lightweight, no-infrastructure approach:

**How it works:**
1. Embed question markers in the system prompt at key section boundaries, e.g., `@@SCAN_1: What are the assets I'm currently building?`
2. Before each task execution, the agent generates 1-2 sentence answers to each marker (~300 tokens total)
3. Tiered activation: FULL (~300 tokens) for complex operations, MINI (~120 tokens) for medium tasks, ANCHOR (~20 tokens) between subtasks
4. The critical requirement: answers must be in the model's *generated* output, not in internal thinking. Generation forces active instruction reprocessing.

**For Olive's use case:** A MINI-level SCAN anchor could be injected between the research phase and the write phase of a complex task:
```
@@SCAN_OLIVE: What Blueprint assets am I building and what was the last completed step?
```

Source: https://community.openai.com/t/solving-agent-system-prompt-drift-in-long-sessions-a-300-token-fix/1375139
Source: https://gist.github.com/sigalovskinick/c6c88f235dc85be9ae40c4737538e8c6

#### Sub-Agent Architecture for Context Isolation

Anthropic's own context engineering documentation recommends:
- Main coordinator agent maintains high-level plan
- Sub-agents handle deep technical work (tens of thousands of tokens each)
- Each sub-agent returns only a condensed 1,000-2,000 token summary
- Result: coordinator's context stays clean; detailed exploration context is isolated

Olive's existing sub-agent system (`explorer`, `researcher`, `coder`) already implements this pattern for the development workflow. The gap is within the autonomous run itself — the Claude Code provider operates as a single long-running context without sub-agent delegation.

Source: https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents

#### Externalized Plan as Implicit Memory

Plan-and-Act research found that an externalized, evolving plan eliminates the need for explicit memory modules:

> "The plan carries forward the relevant context — this approach also allows us to address challenges related to memory."

For Olive, this means: rather than trying to keep all task state in the model's context window, serialize the current plan state to a structured format that can be re-injected as a compact block at the start of each turn. The `FOlivePromptDistiller` already exists for this purpose but its application to multi-Blueprint plan state is unverified.

Source: https://arxiv.org/html/2503.09572v3

#### Context Compaction at Token Threshold

Production systems use rolling compaction triggered at a token budget threshold:
- Clear tool result contents from deep history (lightest-touch approach)
- Replace older dialogue turns with structured summaries
- Keep: architectural decisions, unresolved issues, implementation specifics
- Discard: redundant tool outputs, repetitive messages, completed subtask details

The Anthropic compaction guidance recommends: start by maximizing retention, then iteratively remove unnecessary details based on observed task performance.

Source: https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents
Source: https://blog.jroddev.com/context-window-management-in-agentic-systems/

#### External Note-Taking (Structured Progress Tracking)

For multi-Blueprint tasks spanning many tool calls, agents that maintain an external progress file (written and read as a tool) outperform agents relying solely on context:
- Pokemon agent example: maintained precise tallies across thousands of game steps including objectives, explored regions, and strategies
- The agent writes structured notes after completing each major phase
- On context limit, the notes file provides a compact re-entry point

In Olive's autonomous mode, `olive.write_notes` (or equivalent) could give the Claude Code provider a place to write "current task state" that survives context compaction.

---

## Current Olive Implementation Assessment

Reading `OliveSelfCorrectionPolicy.cpp` and `OliveRetryPolicy.h`, the existing system is more sophisticated than most frameworks described in this research:

**Already implemented:**
- 3-tier error category classification (FixableMistake / UnsupportedFeature / Ambiguous)
- Progressive error disclosure (terse → full → escalation)
- Per-error-signature loop detection with oscillation detection
- Identical plan hash detection (resubmission prevention)
- Stale compile error detection (cross-referenced against plan metadata)
- Granular fallback mode on plan_json loop (switches tool strategy entirely)
- Rollback-aware messaging (tells agent not to reference rolled-back node IDs)
- Component context enrichment on PLAN_RESOLVE_FAILED (lists actual components)
- Auto-search on ASSET_NOT_FOUND (suggests correct paths from project index)

**Gaps identified by this research:**

1. **RATE_LIMITED not classified as transient.** `ClassifyErrorCode()` has no case for `RATE_LIMITED`, `HTTP_429`, or `HTTP_503`. These should be Category D (transient infrastructure) — handled at the provider layer with backoff, never fed to the LLM as corrective guidance. Currently they would fall through to `FixableMistake` and the model would receive a "fix guidance" message that cannot help.

2. **No plan reflection trigger.** The policy fires only on failure. There is no mechanism to prompt the agent to reflect on and update its plan after a research phase completes successfully. Between "template read" turns and "write" turns, the plan may be stale.

3. **No stop-hook for incomplete plans.** The `StopWorker` action stops the run, but there is no "plan completion gate" that checks whether the agent's stated plan has been fully executed before allowing a clean stop. The agent can declare success after partial completion.

4. **No context anchor injection.** There is no SCAN-style anchor mechanism that forces the model to re-read its task goals before each execution turn in a long run. The `FOlivePromptDistiller` exists but whether it re-injects plan state is unclear.

5. **No per-Blueprint checkpointing within an autonomous run.** If Blueprint #4 of 5 fails during an autonomous multi-asset task, the agent restarts the entire task. A checkpoint after each successfully completed Blueprint would prevent this.

---

## Recommendations

1. **Add RATE_LIMITED as Category D (transient) in `ClassifyErrorCode()`.** These error codes should never reach `FeedBackErrors`. Add a pre-check in `Evaluate()` that intercepts `RATE_LIMITED`, `HTTP_429`, `HTTP_503`, `HTTP_529` (Claude overload), and `NETWORK_TIMEOUT` before the category classifier runs. Return `EOliveCorrectionAction::Continue` with a log note, or better, route to a separate `EOliveCorrectionAction::BackoffRetry` that the conversation manager handles with a real delay.

2. **Add plan reflection trigger after research phases.** When the autonomous run completes a read-only phase (N consecutive successful tool calls that are all reader tools), inject a structured prompt: "Based on what you discovered, review and update your plan before proceeding. What, if anything, needs to change?" This can be implemented in `FOliveConversationManager` by tracking the tool call type sequence.

3. **Implement a plan completion gate.** When the agent produces a response with no tool calls (indicating it thinks it is done), inject a system check: "Did you complete all items in your original plan? List what was completed and what, if any, remains." Only allow `StopWorker` after this verification. This directly addresses the Claude Code issue #6159 pattern.

4. **Add a SCAN-style anchor to the autonomous run continuation prompt.** In `BuildContinuationPrompt()` (or the equivalent turn injection), include a compact structured summary block: "Current task: [task]. Completed so far: [list]. Next to do: [list]." Force the model to generate a brief acknowledgment before tool calls begin. The generation step is what matters — passive re-reading does not prevent drift.

5. **Add per-Blueprint checkpointing in autonomous mode.** After each Blueprint is successfully compiled (a natural checkpoint boundary), serialize the completed-asset list to the `FAutonomousRunContext`. If the run fails on a subsequent Blueprint, the recovery path can restart from the checkpoint rather than the beginning. The `FOliveSnapshotManager` already handles pre-run snapshots; per-Blueprint progress tracking is a lightweight addition.

6. **Do not change the granular fallback mechanism.** The plan_json → granular fallback escalation is already better-designed than most described frameworks. The 3-strike threshold with loop reset is correct.

7. **Consider sub-agent decomposition for multi-Blueprint autonomous tasks.** For tasks involving 3+ Blueprints, the coordinator turn could spawn sub-invocations of Claude Code with isolated contexts per Blueprint, returning structured summaries. This is the most architecturally significant change but addresses context dilution at the root rather than with heuristics.
