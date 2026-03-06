# Research: NeoStack AIK — Rate Limiting, Plan Failures, Agent Thinking Time, Discovery Latency

## Question

How does NeoStack AIK handle four specific operational problems that Olive AI Studio is experiencing:
1. Write rate limiting — agent blindly retrying a rate-limited call 6 times
2. Agent giving up after plan_json failure — abandoning the failed Blueprint instead of self-correcting
3. Agent thinking gaps — ~2 minute gaps between tool calls with no mechanism to nudge progress
4. Discovery latency — 9.1s LLM call just to generate a search query for template discovery

---

## Findings

### 1. Rate Limiting / Tool Call Throttling

#### What NeoStack does

NeoStack AIK does **not expose a configurable write rate limit** to the user. There is no `MaxWriteOpsPerMinute` setting, no token bucket, and no internal throttle gate on Blueprint write tool calls. This is inferred from:

- No mention of rate limiting in any changelog version (v0.1.10 through v0.5.6)
- No documentation page on aik.betide.studio covers throttle or rate limit mechanisms
- The v0.5.0 changelog entry "tool execution is now wrapped in crash protection — if something goes wrong, the editor stays alive and the operation is rolled back cleanly" describes crash isolation, not rate limiting

Their crash-protection wrapper (v0.5.0) functions as a circuit breaker for _editor crashes_, not as a write throttle. It catches exceptions per-tool-call and rolls back the failing operation atomically. This is coarser than a rate limit — it is a per-call try/catch, not a sliding-window counter.

The "500+ additional checks" added in v0.3.0 are null/type/state guards that prevent crashes from bad input, not write-frequency controls.

**Their answer to runaway writes: tool consolidation, not rate limiting.** By collapsing 27+ tools to ~15 in v0.5.0, each consolidated tool like `edit_graph` handles a full graph edit operation internally (likely multiple UE API calls per invocation). The agent makes fewer tool calls to accomplish the same work — not because it is throttled, but because each call does more.

#### What Olive does (the bug)

Olive's `FOliveWriteRateLimitRule` returns error code `RATE_LIMITED` when `WriteTimestamps.Num() >= MaxWriteOpsPerMinute`. The suggestion string tells the agent: "Retry after N seconds, or increase 'Max Write Ops Per Minute' in Project Settings."

`FOliveSelfCorrectionPolicy::ClassifyErrorCode()` does NOT have a special case for `RATE_LIMITED`. It falls through to the default `FixableMistake` category (line 949 in OliveSelfCorrectionPolicy.cpp). A FixableMistake gets standard retry treatment: `BuildToolErrorMessage()` is called, `LoopDetector.RecordAttempt()` records it, and the agent is told to try again. Since `RATE_LIMITED` has a consistent signature and a consistent error message across retries, the loop detector won't fire until `MaxRetriesPerError` is hit — explaining the 6 blind retries.

The RetryAfter seconds ARE present in the suggestion string, but the self-correction pipeline does not extract or act on them. The agent receives "Retry after N seconds" as plain text in an enriched error message, but the plugin itself does not back-pressure or delay the next invocation. The agent's interpretation of "retry after N seconds" in an LLM tool-call loop is "retry now."

Source: `Source/OliveAIEditor/Private/Services/OliveValidationEngine.cpp` lines 1383–1428
Source: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` lines 905–950

---

### 2. Agent Giving Up After Plan Failures

#### What NeoStack does

NeoStack has two documented mechanisms relevant here:

**Blueprint health validation after every edit (v0.5.0):** The changelog states "the plugin now validates Blueprint health after every edit, catching corruption before it causes problems later." This is a post-write structural validation step. If a write produces a corrupted/invalid state, the agent is notified immediately with the health report rather than discovering the problem later at compile time. This front-loads error detection, giving the agent a tighter correction loop.

**Crash protection with clean rollback (v0.5.0):** "Tool execution is now wrapped in crash protection — if something goes wrong, the editor stays alive and the operation is rolled back cleanly." This means a failed write rolls back atomically and the agent is told what went wrong. The editor state is always consistent — there is no partial-failure state for the agent to navigate.

**"Agents can now one-shot tasks" (v0.3.2):** The changelog credits "significant prompting improvements (ongoing)" for enabling single-call task completion. The mechanism is unpublished — this is likely system prompt engineering that structures the agent's output as a complete batch plan rather than iterative partial edits. NeoStack does not document what these prompting improvements contain.

**No "must complete" enforcement** is publicly documented. There is no documented mechanism that forces the agent to retry a specific failed asset. NeoStack's architecture delegates recovery entirely to the external agent (Claude Code, Codex, etc.). The agent receives a structured error + rollback confirmation and is expected to decide whether to retry.

**No plan preview/apply cycle:** AIK's `edit_graph` (or equivalent) is likely a single-call commit. There is no fingerprint-verified two-step preview like Olive's `blueprint.preview_plan_json` → `blueprint.apply_plan_json`. This means the agent has no "confirmation checkpoint" to skip, but also no structured mechanism to reason about a plan before executing it.

**Key architectural difference:** AIK outsources all retry/correction intelligence to the attached external agent (Claude Code's own agentic loop). Olive internalizes correction logic in `FOliveSelfCorrectionPolicy`. This means AIK's behavior on a failed blueprint edit is entirely determined by what Claude Code decides to do — if Claude Code decides to move on, AIK cannot stop it.

Sources: aik.betide.studio/changelog (v0.5.0, v0.3.2)

#### What Olive does (the bug)

When `blueprint.apply_plan_json` fails with connection errors, `FOliveSelfCorrectionPolicy` classifies `PLAN_EXECUTION_FAILED` as `Ambiguous`. On the first attempt, `Ambiguous` gets standard retry treatment (BuildToolErrorMessage). If the loop detector fires (repeated identical errors), it falls through to `bAllowGranularFallback` — switching to granular tool guidance. However, this requires the agent to stay focused on the same asset in the _next_ turn. If the autonomous agent (Claude Code) decides to move on to a different task before the self-correction triggers, Olive's correction logic never fires because it only runs on the _result_ of the tool call, not between turns.

The "abandoned the Blueprint" failure is an agent-level decision that happens between turns, outside Olive's control. The self-correction policy corrects within a turn; it cannot reach back and reassign the agent's next goal.

---

### 3. Agent Thinking Time Between Calls

#### What NeoStack does

NeoStack has no documented mechanism for injecting mid-session nudges or progress pings. The changelog does document:

- **"Reduced edge-case session hangs and tool dead-ends" (v0.5.5):** Implies there were cases of agents stalling; v0.5.5 improved backend request/session handling for "long-running agent workflows." No technical mechanism described — likely a timeout + reconnect at the transport layer.
- **"Fixed OpenRouter sessions hanging indefinitely" (v0.5.0):** OpenRouter native client had hangs that blocked progress. Fixed, but mechanism not disclosed.
- **"Reasoning effort controls" (v0.5.0):** Users can set reasoning depth (off / low / medium / high / max) per model. This is a `thinking` budget parameter passed to the LLM. Reducing reasoning depth shortens time-to-first-token and reduces inter-call gaps, at the cost of solution quality.
- **Task completion notifications (v0.3.0):** Toast, taskbar flash, and sound when agent finishes. This is notification infrastructure, not gap reduction.

NeoStack's approach to thinking time appears to be: control it via reasoning depth settings passed to the model, not via plugin-level timeout/nudge mechanisms.

The "plan panel" added in v0.5.0 ("see what the agent is planning — a dedicated plan panel shows the agent's thinking and steps as it works") is a UI transparency feature — it renders the agent's `<thinking>` content in real time. It makes thinking visible to the user but does not alter the agent's behavior.

**No plugin-level idle timeout or progress nudge is documented.** The 2-minute gaps are purely LLM inference time (extended thinking) that NeoStack cannot shorten without adjusting the model or reasoning budget.

Sources: aik.betide.studio/changelog (v0.5.5, v0.5.0, v0.3.0)

#### What Olive could do (inference from AIK's approach)

Expose a reasoning depth / thinking budget control that maps to the model's `budget_tokens` or `thinking` parameter. For Claude Code (autonomous path), this is the `--reasoning-effort` flag. The Philip Conrod review noted "Codex Reasoning parameter now supported" (v0.3.0) and "reasoning effort controls are more reliable" (v0.5.6) — indicating these parameters are configurable per-agent-session.

---

### 4. Discovery Latency

#### What NeoStack does

NeoStack's context injection mechanism operates differently from Olive's template search pipeline:

**Profile-based pre-filtering (not search):** Profiles (Animation, Blueprint & Gameplay, Cinematics, VFX & Materials, Full Toolkit) filter the tool list to domain-relevant tools _at session initialization_. Each profile also overrides tool descriptions to inject domain-specific context. The documentation example: the Animation profile overrides `edit_blueprint` descriptions to "emphasize AnimGraphs and State Machines rather than general Blueprint editing."

This means context is injected as **modified tool descriptions in the tools/list response** — not via a separate search call. When the agent sees `tools/list`, the tool descriptions already contain domain guidance. No LLM call is needed to generate a query; the context arrives with the schema.

**Profile constraint:** "You cannot switch profiles during an active agent session." Profiles are bound at initialization, meaning context injection is a one-time setup cost at session start, not a per-turn query.

**@-mention asset readers (36+ types):** When the user types `@AssetName` in chat, the plugin auto-reads the named asset and injects structured context into the next message. This is user-initiated, synchronous, and local (no LLM call). It trades user friction (explicit mention) for zero latency on the plugin side.

**Blueprint health readback (v0.5.0):** `read_asset` is called by the agent after edits for verification. This is a structured tool call, not an LLM-generated search query. The agent's workflow is: write → read_asset → compare — no search LLM overhead.

**Key architectural difference:** AIK does not have an AI-assisted template search step. There is no equivalent of Olive's "call an LLM to generate the best search query for templates." Discovery is either: (a) pre-injected via profile tool descriptions, (b) user-mentioned via @-mention, or (c) the agent calls read_asset on a known path. The 9.1s discovery latency Olive experiences would not exist in AIK because AIK has no discovery-query LLM step.

Sources: aik.betide.studio/profiles, aik.betide.studio/changelog (v0.5.0)

#### What Olive could do (inference from AIK's approach)

Olive's template discovery invokes a utility model to generate a search query. AIK avoids this by injecting relevant context at session start (profiles), using @-mention for user-directed focus, and relying on the agent's existing knowledge for tool selection. The LLM-query-for-search-query step is Olive-specific and not adopted by any documented competitor. Alternatives:
- Inject the template catalog directly into the system prompt (no search query generation needed; agent decides what to request)
- Accept the user's message as the search query directly (the user's words are already the best available query)
- Make the utility model call optional (only when the agent explicitly signals it wants template context)

---

## Summary Comparison Table

| Problem | NeoStack AIK approach | Olive current behavior | Gap |
|---|---|---|---|
| Write rate limiting | No write rate limit implemented; per-call crash-protection rollback; tool consolidation reduces call frequency | `RATE_LIMITED` → `FixableMistake` → blind retry loop | `RATE_LIMITED` should be its own category with explicit "wait N seconds, do not retry immediately" guidance |
| Plan failure / giving up | Crash-protection atomic rollback + Blueprint health validation feeds result back to agent; agent decides recovery | Self-correction fires within a turn; cannot prevent between-turn abandonment | Olive needs agent-level task persistence (goal tracking) OR the system prompt must instruct the agent to complete each asset before moving on |
| Agent thinking time | Reasoning depth slider (off/low/medium/high/max) passed to model; no plugin-level nudge | No reasoning budget control; no idle timeout nudge | Expose reasoning effort parameter in settings and pass to model |
| Discovery latency | Profile pre-injects context into tool descriptions at session start; @-mention for user-directed focus; no search-query LLM call | 9.1s utility model call to generate template search query | Query generation is a Olive-specific antipattern; use user message as direct query or pre-inject catalog |

---

## Recommendations

**Rate limiting (highest priority fix):**
Add `RATE_LIMITED` as a `UnsupportedFeature` or new `Transient` category in `ClassifyErrorCode()`. Do NOT retry immediately. The enriched message should contain the explicit wait time prominently: `[WAIT REQUIRED: N seconds before next write — do NOT retry immediately]`. Optionally, have the plugin itself enforce the wait by queuing the next tool call rather than rejecting it.

**Plan failure / agent abandonment:**
Olive cannot prevent Claude Code (autonomous path) from deciding to move on between turns. Two mitigations: (1) inject a "must-complete" constraint into the per-turn system prompt directive — "if a blueprint.apply_plan_json call failed, your NEXT action must fix that specific blueprint before proceeding" — and (2) implement task persistence in `FOliveRunManager` that tracks incomplete assets and re-surfaces them in continuation prompts.

**Thinking time:**
Add a `ReasoningEffort` setting (None / Low / Medium / High / Max) to `UOliveAISettings`. For Claude Code autonomous path, map to `--reasoning-effort` CLI flag. For OpenRouter/Anthropic providers, map to the `thinking.budget_tokens` parameter. This gives users a lever to trade quality for speed.

**Discovery latency:**
The utility model call for query generation should not be a required step. Use the user's original message words as the direct search query — they are already intent-expressing. The LLM query-generation step adds 9 seconds of latency with marginal improvement over the user's own words in most cases. Reserve the utility model call for cases where the system explicitly needs a reformulated query (e.g., the user's message is very long and needs distillation).

---

## Source Inventory

- [aik.betide.studio/changelog](https://aik.betide.studio/changelog) — Full changelog v0.1.10 through v0.5.6
- [aik.betide.studio/profiles](https://aik.betide.studio/profiles) — Profile system and tool description injection
- [betide.studio/neostack](https://betide.studio/neostack) — Marketing overview and tool list
- [Philip Conrod review](https://www.philipconrod.com/co-developing-video-games-using-the-latest-version-of-the-neostack-ai-unreal-game-engine-plugin-using-multiple-llms-via-openrouter/) — Third-party usage review
- `Source/OliveAIEditor/Private/Services/OliveValidationEngine.cpp` — `FOliveWriteRateLimitRule`
- `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` — `ClassifyErrorCode()`, `Evaluate()`
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` — `MaxWriteOpsPerMinute`
- `plans/research/competitor-deep-dive-2026-03.md` — Prior AIK research (architecture, tool list, ACP/MCP transport)
