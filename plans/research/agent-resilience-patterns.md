# Research: Agent Resilience Patterns — Rate Limiting, Task Persistence, Discovery Quality

## Question

How do agent frameworks handle three specific failure modes in a tool-calling AI agent:
1. Blind retry on RATE_LIMITED errors (agent hits write rate limit, retries 6+ times without backoff)
2. Premature task abandonment after a single tool failure (agent moves to a different Blueprint instead of trying an alternative approach)
3. Poor template/context discovery (agent uses generic search terms and fails to find relevant templates before planning)

NeoStack (AIK) was the primary target; broadened to LangGraph, AutoGen, CrewAI, and Anthropic's own published patterns when NeoStack had no public documentation on these topics.

---

## Findings

### 1. What NeoStack (AIK) Does About These Problems

NeoStack has no public documentation on any of these three problems. Their approach to error recovery is:
- 500+ crash-prevention guards added reactively in v0.3.0 after launch crashes (null/type/state guards)
- v0.5.0 added a try/catch wrapper around all tool execution
- Error recovery is entirely the agent's (Claude Code's / Cursor's) responsibility
- No structured self-correction policy, no plan deduplication, no rate limit awareness
- No plan-preview-before-execute, no granular fallback mode

This is the gap Olive already addresses better. The research therefore focuses on the broader agent framework space.

Source: `plans/research/competitor-deep-dive-2026-03.md` (in-depth AIK analysis)

---

### 2. Rate Limiting: How the Industry Handles It

#### The core pattern: error classification before retry routing

The universal consensus across LangGraph, AutoGen, CrewAI, and AWS is that `RATE_LIMITED` (HTTP 429) is a **categorically different** error from tool errors, and must be routed differently:

```
429 / RATE_LIMITED  →  wait + same attempt (do NOT change approach)
5xx server errors   →  retry with exponential backoff (same attempt)
4xx client errors   →  do NOT retry, change approach entirely
```

Source: [Sparkco AI — Mastering Retry Logic Agents](https://sparkco.ai/blog/mastering-retry-logic-agents-a-deep-dive-into-2025-best-practices), [Portkey — Retries, fallbacks, circuit breakers](https://portkey.ai/blog/retries-fallbacks-and-circuit-breakers-in-llm-apps/)

#### The specific gap in Olive's implementation

Reading `OliveSelfCorrectionPolicy.cpp`, `ClassifyErrorCode()` currently handles:
- `USER_DENIED`, `TEMPLATE_NOT_FOUND` → `UnsupportedFeature` (Category B: do not retry)
- Everything else falls through to `FixableMistake` (Category A: retry with guidance)

**`RATE_LIMITED` is not in the Category B list.** It falls through to `FixableMistake`, which means `BuildToolErrorMessage()` gets called and the agent gets an error message saying "retry". The agent then does exactly that — immediately, without any semantic signal to wait.

The validation engine (`OliveValidationEngine.cpp` lines 1411–1420) already computes the precise `RetryAfter` value in seconds and includes it in the suggestion text:
```
"Retry after %.0f seconds, or increase 'Max Write Ops Per Minute' in Project Settings..."
```

That information is present in the error message the agent receives. The problem is the error message's *framing* does not explicitly tell the agent to stop and wait — it says "retry after N seconds" which the agent interprets as permission to retry next turn rather than as a directive to pause all write activity.

#### How LangGraph handles it

LangGraph uses `RetryPolicy` configured at node level. For rate limit errors the recommended pattern is:

```python
def node_with_rate_limit_handling(state):
    for attempt in range(MAX_ATTEMPTS):
        try:
            return {"data": call_tool()}
        except RateLimitError as e:
            wait_seconds = e.retry_after or (2 ** attempt * 5)
            if attempt == MAX_ATTEMPTS - 1:
                return Command(update={"error": str(e)}, goto="wait_node")
            time.sleep(wait_seconds)
```

The key design insight: rate limit errors route to a **different node** (`wait_node`) rather than back into the retry loop. The wait node tells the agent to pause its entire plan and resume.

Source: [LangGraph error handling and retry policies](https://deepwiki.com/langchain-ai/langgraph/3.7-error-handling-and-retry-policies), [LangChain Forum — retries exhausted flow](https://forum.langchain.com/t/the-best-way-in-langgraph-to-control-flow-after-retries-exhausted/1574)

#### The prompt-level signal required

Agent frameworks generally agree: a rate limit error message must contain an **explicit directive that prevents plan continuation**, not just a timing suggestion. The difference:

- WEAK (current): `"Retry after 23 seconds"` — agent reads this as "I can retry in 23 seconds"
- STRONG (needed): `"RATE_LIMITED: Stop executing write operations. Wait 23 seconds, then resume where you left off. Do NOT call apply_plan_json or any write tool until the wait is complete."`

The directive must explicitly distinguish RATE_LIMITED from tool failures, because:
- Tool failure = change your plan
- Rate limit = keep your plan, just wait

Source: Sparkco AI backoff guide, Portkey circuit breaker pattern, DEV Community fault tolerance patterns

---

### 3. Task Persistence: How the Industry Handles Premature Abandonment

#### The Anthropic "incremental progress" pattern

Anthropic's engineering blog on long-running agents describes the exact abandonment problem and their solution: **external state tracking outside the conversation context**.

The pattern:
1. Before starting a complex task, create a feature list with explicit `status: pending` for each sub-goal
2. Agent must update the list after each step
3. Agent startup: read the list, pick the first pending item, work only on that item
4. Never declare the overall task complete until all items are `status: done`

This prevents the agent from mentally treating "one Blueprint is done" as "the task is done" when the task involved multiple Blueprints.

Source: [Anthropic — Effective Harnesses for Long-Running Agents](https://www.anthropic.com/engineering/effective-harnesses-for-long-running-agents)

#### The specific gap in Olive's implementation

The current `FOliveSelfCorrectionPolicy` handles the case where a tool fails and the agent should try again. What it does NOT handle is the case where a tool **partially succeeds** (or fails with a non-retriable error) and the agent should:
- Finish the current Blueprint before moving on
- Try a different approach (`add_node` instead of `apply_plan_json`) rather than abandoning
- Report "this sub-task is blocked, moving on with note" rather than silently skipping

Reading the `BuildGranularFallbackMessage()` function, there IS a forced tool switch from plan mode to granular mode. But that only triggers after `plan_json` fails repeatedly on the same Blueprint. The abandonment happens earlier — the agent moves to a *different Blueprint* before the fallback fires.

#### What works: explicit completion contracts in error messages

The strongest signal available when an agent is about to give up is the error message it just received. If that message says:

> "This tool failed. You should try an alternative approach before moving on. Specifically: try `add_node` + `connect_pins` instead of `apply_plan_json`."

...the agent will generally follow it. The key principle from Anthropic's context engineering guide: the LLM will follow explicit in-context instructions over trained defaults, as long as the instruction appears in the current context window.

Source: [Anthropic — Effective Context Engineering](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents)

#### The "don't give up" instruction pattern

From the Anthropic harness patterns and the 11-tips article, the most effective prompt-level instruction for preventing abandonment:

```
When a write tool fails:
1. Try at least one alternative approach before moving to a different asset
2. If plan_json wiring fails, try connecting the same nodes with connect_pins
3. If connect_pins fails with INCOMPATIBLE, report the incompatibility and ask for guidance
4. Do NOT move to a different Blueprint until you have either succeeded or exhausted
   all available approaches (plan_json, granular tools, Python)
5. If you move on from a Blueprint with unfinished work, leave an explicit note
   explaining what was attempted and what remains
```

Source: [Datagrid — 11 Tips for AI Agent Prompts](https://datagrid.com/blog/11-tips-ai-agent-prompt-engineering), [Anthropic harness patterns](https://www.anthropic.com/engineering/effective-harnesses-for-long-running-agents)

---

### 4. Discovery Quality: Pre-Task Context Injection

#### The hybrid strategy (Anthropic's own recommendation)

From Anthropic's context engineering blog:

> "Claude Code demonstrates this by providing CLAUDE.md files upfront while using grep/glob for runtime retrieval."

The hybrid approach:
- **Upfront (always in context):** Catalog-level data — template names, project structure, tool schemas
- **On-demand (just-in-time):** Full template content, specific Blueprint state, per-function graphs

This matches what Olive already does with `FOliveTemplateSystem::GetCatalogBlock()` + `blueprint.list_templates` + `blueprint.get_template`. The question is whether the catalog is sufficient to guide the agent toward the right search terms.

Source: [Anthropic — Effective Context Engineering](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents)

#### The root cause of poor search terms

The agent uses generic search terms ("weapon", "character") because it does not have enough signal about which specific templates exist. The catalog block tells it what categories exist but not which terms match which templates.

The Anthropic Tool Search Tool paper (quoted in their advanced tool use article) found that:

> "Instead of loading all tool definitions upfront, the Tool Search Tool discovers tools on-demand. Claude only sees the tools it actually needs for the current task."

Applied to Olive: the catalog injection at the top of the system prompt is a fixed token cost regardless of the task. As the task becomes more specific, that fixed catalog is both too broad (lots of noise) and too shallow (no specific template descriptions).

The improvement path is **task-specific catalog pre-filtering**: before the agent starts planning, inject only the most relevant catalog entries based on the user's task description.

Source: [Anthropic — Advanced Tool Use](https://www.anthropic.com/engineering/advanced-tool-use)

#### Agentic RAG: discovery before planning

A pre-task discovery phase where the agent gathers context BEFORE making any write calls is well-documented:

The pattern from Agentic RAG research (Arxiv 2501.09136) and the LangGraph best practices guide:
1. User message arrives
2. Agent runs a **read-only discovery phase**: search templates, read asset state, identify relevant patterns
3. Agent synthesizes a mini-plan from discovery results
4. Agent executes write operations

The discovery phase is the exact pattern Olive's system prompt already recommends ("read before write"). The gap is that the system prompt states this as a soft preference, not a hard sequence. Agents skip it under pressure of token limits or when they think they know the answer.

Source: [Arxiv — Agentic RAG Survey](https://arxiv.org/abs/2501.09136), [LangGraph best practices](https://www.swarnendu.de/blog/langgraph-best-practices/)

#### What competitors do for context injection

NeoStack AIK's @-mention system (36+ asset type readers) is the most directly comparable approach. When a user types `@BP_Gun`, the asset reader immediately generates structured context: component list, variable types, functions, event dispatchers. This context appears in the conversation before the agent's first turn.

The key advantage: the agent does not need to search for this context — it is injected deterministically based on user selection. The agent's first response has complete asset state and can plan without discovery calls.

Olive has a version of this via `BuildAssetStateSummary()` and `SetInitialContextAssets()`, but it only fires on continuation prompts, not on the initial task.

Source: `plans/research/competitor-deep-dive-2026-03.md`

---

## Recommendations

### Problem 1: Rate Limiting

**Root cause confirmed:** `RATE_LIMITED` falls through to `FixableMistake` in `ClassifyErrorCode()`. The `RetryAfter` value is computed and present in the error message but the framing does not prevent immediate retry.

**Recommended fix (two parts):**

Part A — Add a fourth error category `RateLimit` to `EOliveErrorCategory`, above `UnsupportedFeature` in priority. Map `RATE_LIMITED` to it in `ClassifyErrorCode()`. In `Evaluate()`, handle it before the Category B check.

Part B — The message text for `RATE_LIMITED` must include:
- Explicit stop directive: "Do NOT call any write tools until the wait is complete"
- Exact wait time: already computed in the validation error
- Resume instruction: "When the window clears, continue your plan where you left off — do NOT change your approach"
- Counter: how many ops were attempted (already in the message) so the agent understands why it was stopped

The agent should not count `RATE_LIMITED` toward the `MaxAttempts` retry counter, because it is not a mistake. `AttemptNumber` in `FOliveCorrectionDecision` should not increment for rate limit events.

**Do not** route `RATE_LIMITED` as `StopWorker` — the agent should resume, not stop. `FeedBackErrors` with the right message is correct.

### Problem 2: Task Persistence

**Root cause:** The self-correction policy only fires when a tool returns an error result. If the agent decides to stop on its own (no tool call made), there is nothing to intercept.

**Recommended fix:**

The most reliable place to enforce persistence is in the **error message text** returned for `DATA_WIRE_INCOMPATIBLE`, `PLAN_RESOLVE_FAILED`, and `BP_CONNECT_PINS_INCOMPATIBLE` — the errors that typically precede abandonment. These messages should explicitly state:

> "Try an alternative approach on this Blueprint before moving to other assets: (1) use `add_node` + `connect_pins` instead of `apply_plan_json`, or (2) use `editor.run_python`. Do NOT proceed to other Blueprints until you have made one genuine alternative attempt."

This is most effectively placed in `BuildToolErrorMessage()` as a conditional section when `AttemptNum == 1` for these specific error codes. On the first failure, the agent needs the strongest possible directive to stay on the current asset.

**Secondary recommendation:** The `BuildGranularFallbackMessage()` function already forces tool switching. Consider lowering its trigger threshold from "repeated failures" to "first failure after any wiring error" for `DATA_WIRE_INCOMPATIBLE` specifically, since wiring incompatibility on `apply_plan_json` is a known signal that granular tools may succeed.

### Problem 3: Discovery Quality

**Root cause:** The catalog block in the system prompt is static (built at prompt-assembly time) and not specific to the current task. Generic terms match too broadly; the agent does not get enough signal about which templates are most relevant.

**Recommended fix (two parts):**

Part A — **Task-keyed catalog pre-injection.** Before launching the CLI process, `FOliveCLIProviderBase::SetupAutonomousSandbox()` or `BuildCLISystemPrompt()` should run a lightweight template search using keywords extracted from the user message (the `ExtractKeywordsFromMessage()` method already exists). The top 3-5 matching template summaries should be injected into the system prompt's catalog block as "Relevant templates for this task:".

Part B — **Stronger discovery directive.** The current "read before write" guidance in the system prompt is phrased as a preference. It should be phrased as a required first step with specific tools named:

> "Before calling any write tool: (1) call `blueprint.list_templates` with a task-specific query to find matching reference patterns, (2) call `blueprint.get_template` on the most relevant result. Only begin writing after you have read at least one relevant template or confirmed no template exists."

This mirrors Anthropic's finding that explicit in-context directives override trained defaults. "Read before write" as a soft preference is ignored under pressure; "you must call list_templates first" is harder to skip.

**Note on Aura's approach:** Aura's Plan mode stores plans as `.md` files in `/Saved/.Aura/plans/` before executing, which forces a discovery→plan→execute sequence. A simplified version of this — requiring the agent to output its plan as a brief text summary before calling any write tool — would also enforce discovery quality. This is a larger UX change but worth considering for a "plan before act" mode.

---

## Sources

- [Anthropic — Effective Harnesses for Long-Running Agents](https://www.anthropic.com/engineering/effective-harnesses-for-long-running-agents)
- [Anthropic — Effective Context Engineering for AI Agents](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents)
- [Anthropic — Advanced Tool Use](https://www.anthropic.com/engineering/advanced-tool-use)
- [Sparkco AI — Mastering Retry Logic Agents 2025](https://sparkco.ai/blog/mastering-retry-logic-agents-a-deep-dive-into-2025-best-practices)
- [Portkey — Retries, Fallbacks, and Circuit Breakers in LLM Apps](https://portkey.ai/blog/retries-fallbacks-and-circuit-breakers-in-llm-apps/)
- [DEV Community — 4 Fault Tolerance Patterns Every AI Agent Needs](https://dev.to/klement_gunndu/4-fault-tolerance-patterns-every-ai-agent-needs-in-production-jih)
- [LangChain Forum — Controlling Flow After Retry Exhaustion](https://forum.langchain.com/t/the-best-way-in-langgraph-to-control-flow-after-retries-exhausted/1574)
- [DEV Community — Beginner's Guide to LangGraph Retry Policies](https://dev.to/aiengineering/a-beginners-guide-to-handling-errors-in-langgraph-with-retry-policies-h22)
- [EZThrottle — Stop Losing LangGraph Progress to 429 Errors](https://www.ezthrottle.network/blog/stop-losing-langgraph-progress)
- [Datagrid — 11 Tips for Production AI Agent Prompts](https://datagrid.com/blog/11-tips-ai-agent-prompt-engineering)
- [Arxiv — Agentic RAG Survey (2501.09136)](https://arxiv.org/abs/2501.09136)
- Internal codebase: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`
- Internal codebase: `Source/OliveAIEditor/Private/Services/OliveValidationEngine.cpp` (lines 1390–1425)
- Internal codebase: `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h`
- Internal codebase: `plans/research/competitor-deep-dive-2026-03.md`
