# Research: Aura Self-Correction, Retry, and Task Completion Behaviors

## Question

How does Aura (RamenVR/StudioSyndiworx) handle five specific agent reliability problems:
1. Agent abandoning failed Blueprint operations instead of retrying
2. Agent repeatedly retrying the same failed call (rate limit loop)
3. Completion enforcement across multi-Blueprint tasks
4. Pre-search / context preparation before starting work
5. Plan refinement after seeing templates or encountering errors

## Methodology

Sources: official documentation at `tryaura.dev` (documentation structure, blueprints, prompting tips, project understanding, public beta page), Ramen VR press releases (Telos launch, January 2026 launch, Aura 12.0 beta), v0.4.0 build notes, third-party reviews. Aura's internal architecture is not publicly documented. Technical behavior below is inferred from UX descriptions and stated capabilities. I flag where I am inferring rather than quoting.

Aura does NOT publish Discord archives, API documentation, or internal implementation details. No changelog was found with fix-level detail on agent behavior.

---

## Findings

### Q1: Agent Abandoning Failed Operations

**What Aura does:**

The recovery model for Blueprint failures is human-in-the-loop, not autonomous:

1. Telos generates Blueprint changes and streams them live in the Unreal editor.
2. The user reviews the diff using Unreal's Blueprint Diff tool.
3. The user chooses Keep or Reject.
4. If rejected, the user re-prompts with refined instructions.

The Coding Agent (C++/Python path) is explicitly described as "capable of self-correcting in real-time." Telos (the Blueprint-specific agent) is a separate agent path with no equivalent self-correction claim. The v0.4.0 changelog states "Generation errors reduced by approximately 50%" — this is a model quality improvement, not a runtime recovery mechanism.

Telos 2.0 claims ">99% accuracy on existing blueprint graphs" and a "25x reduction in error rates." These are positioned as "fewer failures occur," not "failures are automatically recovered from." The marketing rationale is accuracy-based prevention rather than recovery-based resilience.

Best-practice documentation says: "Avoid trying to one-shot a system. Break it into small components like: Data definitions, Inventory logic, UI layer, Interaction flow, Persistence." This is framed as user prompting advice, not agent decompose-and-retry logic.

No evidence of: automatic decomposition of failed operations, mandatory retry loops, or partial-success commit-and-retry patterns.

Source: `https://www.tryaura.dev/updates/unreal-ai-agent-blueprints`, `https://techintelpro.com/news/ai/ai-assistants/aura-ai-assistant-for-unreal-engine-launches-january-2026`

**Comparison to Olive:**

Olive has `FOliveSelfCorrectionPolicy` with 3-tier error classification (`FixableMistake` / `UnsupportedFeature` / `Ambiguous`), progressive error disclosure (terse→full→escalate across attempts), plan deduplication hash detection, and `BuildGranularFallbackMessage()` that forces the agent from batch-plan mode to step-by-step mode when plans repeatedly fail. This is substantially more mechanically sophisticated than anything Aura documents. Aura's equivalent is "reject the diff and re-prompt."

---

### Q2: Repeated Retry of Same Failed Call / Rate Limit Loops

**What Aura does:**

Nothing is documented. No loop detection, no backoff, no retry limits appear in any public documentation, changelog, or user report.

Aura operates through a hosted credit/subscription backend. Rate limiting is server-side and transparent to the agent (their server manages calls to Claude Sonnet/Opus). There is no mechanism the agent itself triggers against rate limits.

The credit consumption model acts as an economic soft deterrent: infinite retries would exhaust credits. This is economic, not technical loop prevention.

No public user reports of Aura getting stuck retrying the same call were found.

Inferred reason: Aura's backend handles API calls without exposing the retry layer to the agent. The agent sees successful or failed responses from Aura's server, not raw API rate limit errors.

Source: `https://www.tryaura.dev/documentation/` (inferred from credit/billing model description)

**Comparison to Olive:**

Olive has `FOliveLoopDetector` tracking error signatures in format `{tool_name}:{error_code}:{asset_path}`, per-error retry limits (`MaxRetriesPerError = 3`), a total correction budget (`MaxCorrectionCyclesPerWorker = 20`), and oscillation detection (A→B→C→A pattern detection over a 6-error sliding window with 3-cycle threshold). Olive is significantly more robust on this dimension. However, Olive also directly exposes the raw provider API — the agent can receive HTTP 429 errors. Aura abstracts this entirely via their hosted backend.

---

### Q3: Completion Enforcement

**What Aura does:**

No explicit task tracking or progress checklist mechanism is documented.

The closest artifact is **Plan Mode**, which stores plans as editable markdown files in `/Saved/.Aura/plans/`. This gives users a persistent, human-readable record of what was planned. But there is no agent-side mechanism that tracks "I planned N steps, I completed M, I must now complete M+1 through N." Completion across a multi-Blueprint task is the user's responsibility.

From the v0.4.0 build notes: "Aura now plans multi-step actions more reliably, which helps Agent Mode stay on task and improves answer accuracy." This refers to in-session coherence (not drifting mid-turn), not cross-turn task tracking.

Parallel threads (up to 4 simultaneous) are positioned as a productivity feature — "you can move onto other things." This structurally puts multi-Blueprint completion tracking entirely on the user, who must watch 4 threads simultaneously.

Source: `https://www.tryaura.dev/documentation/` (prompting tips page), `https://www.ramenvr.com/studio-spotlight/aura-0-4-0-notes-from-the-build-room`

**Comparison to Olive:**

Olive also does not currently have an explicit completion enforcement mechanism for multi-Blueprint tasks — this is an identified gap for both products. The structural difference: Aura's plan markdown files in `/Saved/.Aura/plans/` give users a tangible work-tracking artifact they can manually check off. Olive's plan JSON is ephemeral (used for Blueprint execution and then discarded). Olive has no equivalent persistent human-readable work list.

---

### Q4: Pre-Search / Context Preparation

**What Aura does:**

Two distinct layers:

**Project-level indexing (automatic):** Aura indexes the project at startup using Cohere v4 embeddings. Users can ask "Tell me about this project" and get an informed answer without providing any context. The v0.4.0 notes cite "2-3x lower latency" from improved cache hit rates on this index. This is always-on background indexing, not a per-task pre-search pass.

**Per-task context injection (user-driven):** Users can provide context via drag-drop from Content Browser, @asset mentions (e.g., `@BP_Resource_Nodes`), image paste, and file references. The documentation states: "When Aura has specific files to reference it can construct better answers." This is explicitly user responsibility — "Give Aura relevant files as context when possible."

**No autonomous pre-task discovery pass.** There is no described behavior where, before starting a task, Aura autonomously searches for relevant templates, similar Blueprints, or related patterns. The agent does not do a "discovery pass" equivalent. The project index is queried reactively (to answer questions) rather than proactively (to pre-load relevant context before planning).

Source: `https://www.tryaura.dev/documentation/` (project-understanding page, prompting-tips page), `https://www.ramenvr.com/studio-spotlight/aura-0-4-0-notes-from-the-build-room`

**Comparison to Olive:**

Olive's library template system (`FOliveLibraryIndex`) provides agent-invoked three-tier discovery: catalog injected into system prompt → `list_templates(query)` → `get_template(id, pattern)`. The agent can and should search templates before planning. This is agent-initiated, not user-initiated. Olive's design is more capable on this dimension for template-driven work. However, Aura's always-on project indexing (Cohere embeddings, fast cache) is richer for understanding existing project structure — Olive's `FOliveProjectIndex` covers asset search but does not do semantic embedding of the full project graph.

---

### Q5: Plan Refinement Cycle

**What Aura does:**

This is the area where Aura has the clearest documented mechanism.

**Plan Mode** implements an explicit human-in-the-loop plan→review→refine→execute cycle:

1. User describes the feature in natural language.
2. Aura generates a discrete step-by-step plan.
3. Plan is written to `/Saved/.Aura/plans/` as an editable markdown file.
4. User reviews the plan file directly (it is a real file on disk, editable in any editor).
5. User can refine through continued conversation: "review and refine implementation strategy through discussion before committing to changes."
6. User approves.
7. Aura executes.

The documentation describes this as: "detailed, step-by-step implementation plans that you can review, refine, and approve before any changes are made."

**There is no autonomous refinement loop.** The agent does not: execute Plan Mode → notice it lacks template data → search templates → revise plan → re-present for approval. Refinement requires human input. The agent cannot self-improve a plan after generation without user prompting.

Source: `https://www.tryaura.dev/documentation/` (documentation structure page confirms Plan Mode behavior), `https://www.tryaura.dev/updates/aura-unreal-engine-ai-assistant-public-beta`

**Comparison to Olive:**

Olive's `blueprint.preview_plan_json` → `blueprint.apply_plan_json` cycle with fingerprint verification is mechanically equivalent to Aura's plan→approve→execute flow. The key differences:

- Aura stores plans as persistent human-editable markdown files on disk. Olive's preview plan is transient (JSON in the conversation, not persisted to disk).
- Aura makes Plan Mode the primary recommended workflow — it is a first-class UX feature with a dedicated mode switch. Olive's preview/apply cycle is optional (`bPlanJsonRequirePreviewForApply` setting).
- Aura's plan artifact is human-editable. Olive's plan JSON is tool-editable (the agent can regenerate it) but not surfaced as a file users edit directly.

---

## Summary Comparison Matrix

| Problem | Aura's Approach | Olive's Current State | Gap Direction |
|---|---|---|---|
| Failed op recovery | Human rejects diff, re-prompts; Coding Agent self-corrects for C++ | `FOliveSelfCorrectionPolicy` with 3-tier error classification, progressive disclosure, granular fallback | Olive is ahead |
| Retry loop detection | No documented mechanism; credit model is economic deterrent | `FOliveLoopDetector` with signature tracking, per-error limits, oscillation detection | Olive is ahead |
| Task completion | Plan markdown files in `/Saved/.Aura/plans/`; user tracks manually | No persistent task checklist; plan JSON is ephemeral | Neither has automation; Aura has better UX artifact |
| Context preparation | User-driven @mention + always-on project index; no agent pre-search | Agent-searchable library index; user @mention not yet exposed | Olive ahead on agent-initiated search; Aura ahead on full project indexing |
| Plan refinement | Plan Mode = human edits markdown → approves → executes; no autonomous re-plan | Optional preview/apply cycle; review gate exists but is optional | Aura has stronger UX gate; Olive has no persistent plan artifact |

---

## Recommendations

**For the architect:**

1. **Persistent plan artifact.** Aura's `/Saved/.Aura/plans/` markdown files are a simple but effective mechanism that gives users task-tracking without any autonomous tracking logic. Olive should consider writing executed plan JSON (or a human-readable summary) to a persistent file alongside the Blueprint asset. This costs nothing to implement and addresses the "agent completed 1 of 2 Blueprints" problem at the UX level without requiring autonomous task-tracking logic.

2. **Forced review gate for multi-asset tasks.** The "started 2 BPs, abandoned 1" failure mode is a completion problem. Aura's Plan Mode forces human review before execution — if the plan shows 2 Blueprints to create, the user sees both before anything starts. Olive's `bPlanJsonRequirePreviewForApply` is optional. For cross-system tasks (CrossSystem module), a mandatory plan preview before executing multi-asset work would catch this. The gate already exists — consider making it mandatory for tasks spanning more than one asset.

3. **Rate-limit backoff is not something Aura solves architecturally.** They abstract it via their hosted backend. For Olive's direct-provider model, the existing retry logic is appropriate and more transparent. The "6 identical retries" failure is a missing backoff policy in Olive's provider error handling, not an architecture gap versus Aura.

4. **Agent-initiated pre-search is a differentiation point.** Aura does not do autonomous pre-task template discovery. Olive's `list_templates` + `get_template` search capability, when properly guided in the system prompt, is a genuine advantage. The research finding: ensure the system prompt explicitly instructs the agent to search templates before planning, not just before executing.

5. **Telos 2.0's self-correction advantage is accuracy, not recovery.** Their approach to "fewer failures" is investing in a proprietary reasoning framework (MIT-trained, RLHF, claimed fine-tune on Blueprint data). They prevent errors rather than recover from them. Olive's approach (catch errors and feed back to the model) is complementary and remains necessary because no model reaches 100% accuracy. The gap is not architectural — it is model quality. Olive cannot directly replicate Telos 2.0's accuracy without similar fine-tuning investment.

Sources:
- [Aura Documentation](https://www.tryaura.dev/documentation/)
- [Aura Blueprint Agent Tutorial](https://www.tryaura.dev/updates/unreal-ai-agent-blueprints)
- [Aura Public Beta Announcement](https://www.tryaura.dev/updates/aura-unreal-engine-ai-assistant-public-beta)
- [Aura 0.4.0 Build Notes](https://www.ramenvr.com/studio-spotlight/aura-0-4-0-notes-from-the-build-room)
- [Aura 12.0 Beta Press Release](https://www.gamespress.com/en-US/Next-Evolution-of-Best-In-Class-Multi-agent-AI-Assistant-for-Unreal-En)
- [Telos Launch Press Release](https://www.prnewswire.com/news-releases/ramen-vr-introduces-telos-the-breakthrough-ai-agent-for-unreal-blueprints-302561368.html)
- [Aura Launch Press Release](https://www.prnewswire.com/news-releases/aura-ai-assistant-for-unreal-engine-launches-vr-studio-ships-game-in-half-the-time-with-new-agent-capabilities-302651608.html)
- [Aura 12.0 Technical Overview](https://briefglance.com/articles/aura-120-redefines-unreal-engine-workflow-with-autonomous-ai)
