# Research: Error Recovery and Self-Correction Patterns for AI Tool Systems

## Question

How do Claude Code, Roo Code, Cursor, Aider, and other AI coding tools handle tool call failures and self-correction? What patterns reduce wasted turns when the Builder agent guesses wrong function names (e.g., `SetSpeed` instead of `MaxSpeed`)? What are the best practices for actionable error messages in LLM tool systems?

## Findings

---

### 1. How Claude Code Handles Tool Failures

Claude Code (the CLI) does not have a documented internal retry loop for tool failures — its recovery relies entirely on the quality of error messages sent back in the `tool_result` block. The model is Claude itself, which handles recovery through chain-of-thought reasoning on the error content. When a bash command fails, the exit code plus stdout/stderr is returned verbatim. When a file edit fails to apply, the system prompt instructs re-reading the file first and retrying the edit.

The key mechanism is: **the error message IS the recovery instruction**. Claude Code doesn't have special retry logic — it relies on Claude reading the error, inferring what to do, and calling the right tool next. This means poorly formatted or vague errors produce guessing; well-formatted errors with explicit next steps produce direct recovery.

A changelog note confirms: "fixed orphaned tool_result errors when sibling tools fail during streaming execution" — errors from individual tool calls are surfaced cleanly in the conversation.

Source: [Claude Code Docs](https://code.claude.com/docs/en/troubleshooting), changelog analysis

---

### 2. How Roo Code Handles Self-Correction

Roo Code has documented several failure modes and partial mitigations:

**Grace retry for tool format errors:** When the model fails to emit valid tool call XML, Roo Code silently retries before showing errors to the user (v3.36.x). This handles _format_ failures (malformed XML) not _semantic_ failures (wrong function name).

**No structured self-correction for semantic failures:** There is no automatic "read first, then retry" enforced by Roo Code for semantic tool failures. Error recovery is entirely the model's responsibility based on error message quality.

**Temperature-induced failures:** Issue #6156 documents that high temperature causes frequent tool failures ("Content appears to be truncated"), with a proposal for automatic temperature reduction on failure. Not merged as of March 2026.

**Error message quality gap:** Issue #9113 explicitly documents that `apply_diff` failure messages are unclear because they don't include the example of the correct tool format. The proposed fix: show a detailed explanation of the specific structural error AND include a correct usage example for the failed tool. This is the same problem Olive faces with `FUNCTION_NOT_FOUND` — the error names alternatives that are structurally unrelated to what the agent actually needs.

Source: [Roo Code Issue #9113](https://github.com/RooCodeInc/Roo-Code/issues/9113), [Issue #6156](https://github.com/RooCodeInc/Roo-Code/issues/6156), [Issue #556](https://github.com/RooCodeInc/Roo-Code/issues/556)

---

### 3. How Cursor Handles Failed Edits

Cursor's agent system prompt (leaked March 2025) contains an explicit read-before-write rule:

> "Unless you are appending some small easy to apply edit to a file, or creating a new file, you MUST read the contents or section of what you're editing before editing it."

And a hard cap on retries:

> "Do not loop more than 3 times on fixing linter errors on the same file. On the third time, you should stop and ask the user what to do next."

These are **prompt-engineering enforcements**, not tool gating. The model is instructed to read before write, but nothing technically prevents it from skipping the read. Users document that the agent does skip the read step, particularly when it's confident about a change.

When `edit_file` fails to apply, the instruction is: "you should try reapplying the edit" — a simple retry. The tool result includes a lint check of the actual diff, which the model can use to detect format problems and self-correct.

**Key observation:** Cursor enforces read-before-write via the system prompt, not the tool layer. The enforcement is soft and skippable. For Olive, this means prompt instructions alone will not reliably prevent the Builder from skipping `blueprint.describe` before calling a function.

Source: [Cursor System Prompt Gist](https://gist.github.com/sshh12/25ad2e40529b269a88b80e7cf1c38084), [Cursor Forum: Required First Tool Feature Request](https://forum.cursor.com/t/feature-request-required-first-tool-or-pre-tool-hook-for-mcp-enforcement/148730)

---

### 4. How Aider Handles Compiler/Lint Errors

Aider's "lint and fix" loop is the most sophisticated among coding tools:

**Format innovation:** Aider uses tree-sitter to show lint errors _within their containing function context_ rather than as raw line numbers. The problematic lines are highlighted (█) surrounded by 5–10 lines of context from the enclosing function/class. This matters because LLMs are poor at working with raw line numbers ("making off-by-one errors") but good at working with code blocks in context.

**Loop behavior:** After each edit, Aider lints modified files. If errors are found, it sends the LLM a report and requests fixes. It iterates until clean or until the user manually stops it. There is no hard cap documented in the lint loop docs (unlike Cursor's 3-attempt limit), though the `--auto-lint` flag can be disabled.

**Repo map for API discovery:** Aider uses a "repository map" — signature-only summaries of all functions across the codebase (declarations without bodies) — injected into context before writes. This gives the model the available API surface before it tries to use it, preventing wrong-name guesses. The map is 5–8x smaller than full file content. This is the read-before-write pattern, automated: the model gets API signatures upfront rather than having to call a read tool first.

**Relevance to Olive:** Aider's repo map approach is the structural equivalent of injecting `blueprint.describe` output into the plan prompt _before_ the Builder tries to write. The model never needs to "read before write" because the API surface is already present.

Source: [Aider Lint Docs](https://aider.chat/docs/usage/lint-test.html), [Aider Linting Article](https://aider.chat/2024/05/22/linting.html)

---

### 5. Actionable Error Messages for LLM Tool Systems

Research and practitioner reports agree on a four-part structure that dramatically improves recovery rates:

1. **Error type/code** — classifies the problem category
2. **Specific details** — what exact value failed, what was actually found
3. **Diagnosis** — why it failed with multiple possible causes listed
4. **Numbered recovery steps** — exact tool/function names to call, in order

**Before/after example from production deployment:**
- Poor: `"Error: Unknown element reference: e999"`
- Good: `"Element ref 'e999' is stale after navigation. Possible causes: (1) snapshot taken before this element existed, (2) typo in ref. Recovery: (1) Call browser_snapshot() (2) Find element by name (3) Retry with new ref"`

A practitioner reported improvement from ~20% to ~95% error recovery rate after switching to this format, with shorter conversations.

**Key principle from LangChain/agent design literature:** "Recovery plans beat generic descriptions. `'Element not found'` tells the LLM nothing, while `'Call get_elements(), find the element by name, retry with the correct ref'` is an actionable recovery plan."

**The tool name specificity rule:** Always name the exact tool to call next, not a vague instruction. `"Call blueprint.read with include_pins:true"` vs. `"Check the Blueprint"`. Olive's `OliveSelfCorrectionPolicy` already follows this for most error codes (e.g., `BP_CONNECT_PINS_FAILED` explicitly says "your next tool call MUST be blueprint.read").

Source: [LLM API Error Messages Article](https://dev.to/johnonline35/why-your-apis-error-messages-fail-when-called-by-an-llm-and-how-to-fix-them-5a5d), [LangChain Context Engineering](https://docs.langchain.com/oss/python/langchain/context-engineering)

---

### 6. "Read Before Write" Enforcement — Tool Gating vs. Prompt Engineering

No current tool enforces read-before-write at the _tool layer_. All implementations use prompt engineering (Cursor system prompt, Aider instructions). Tool gating via a "required first tool" or "pre-tool hook" is an open feature request in the Cursor forum (January 2026, still unimplemented).

The three enforcement approaches observed:

**Approach A — Prompt instruction (Cursor):** "You MUST read before editing." Soft, skippable, but costs nothing. Effective when the model is confident (reads first), fails when the model is overconfident (skips).

**Approach B — API surface upfront (Aider repo map):** The read step is eliminated by injecting API signatures into the system context before write. The model doesn't need to read because it already knows what's available. Eliminates the "skipped read" failure mode entirely.

**Approach C — Error-driven injection (Olive's current approach for ASSET_NOT_FOUND):** On failure, inject the relevant context into the error message (e.g., Olive auto-searches the project index and embeds matching asset paths in `ASSET_NOT_FOUND` responses). This means the model gets the data it needed on the second attempt rather than the first, burning one turn.

For the `FUNCTION_NOT_FOUND` case: Approach B would inject `ProjectileMovementComponent`'s callable functions into the plan context before execution. Approach C would inject them on failure. Currently, Olive uses neither — it generates fuzzy string matches from the node catalog (`SetSphereRadius` from `SphereComponent`) which are completely wrong class matches.

---

### 7. Progressive Context Injection on Failure

The Inferable.ai progressive context enrichment article articulates a principle directly applicable here:

> "Giving LLMs more context often makes them perform worse, not better... Let the LLM fetch context when it needs it."

The practical implication: don't inject all of `ProjectileMovementComponent`'s functions into every plan. Instead, when `FUNCTION_NOT_FOUND` is triggered for a call targeting a component, inject that specific component's callable function list into the error response. This provides focused, relevant context exactly when needed.

The "layered knowledge injection" approach from automated bug repair research (79% fix rate with Llama 3.3) uses three progressive layers:
1. Bug Knowledge Layer: just the failing code + error
2. Repository Knowledge Layer: related files, dependencies
3. Project Knowledge Layer: documentation

Translated to Olive: when `call SetSpeed target_class:ProjectileMovementComponent` fails:
- Layer 1 (current): error code + fuzzy name matches from the wrong class
- Layer 2 (proposed): actual callable functions on `UProjectileMovementComponent` from live reflection
- Layer 3 (optional): Blueprint context (variables, SCS components, other functions)

Source: [Progressive Context Enrichment](https://www.inferable.ai/blog/posts/llm-progressive-context-encrichment), [Layered Knowledge Injection](https://arxiv.org/html/2506.24015v1)

---

### 8. Reflection/Reasoning Loops Before Retry

**Research result:** The 2025 paper "Failure Makes the Agent Stronger" ([arxiv 2509.18847](https://arxiv.org/abs/2509.18847)) proposes **structured reflection** for tool-calling agents. The format:
1. Diagnose the failure using evidence from the previous step
2. Propose a correct, executable follow-up call

This is trainable with RL — they got large gains on BFCL v3 benchmark for multi-turn tool calling. The key insight: the reflection must be **structured and explicit**, not just "think harder." Unstructured prompts to reconsider produce inconsistent results.

**Counter-evidence:** The ACL 2025 paper "Understanding the Dark Side of LLMs' Intrinsic Self-Correction" shows intrinsic self-correction (telling the model "reconsider your answer") causes LLMs to waver and introduces cognitive biases on both simple factual queries and complex tasks. Without an external validator, the model cannot distinguish improving from degrading.

**Synthesis for Olive:** Structured reflection works when triggered by a _specific, classifiable failure_ with _external validation available_ (e.g., compile errors, API surface mismatch). It fails for ambiguous failures where the model must guess whether its previous answer was correct. The current `FOliveSelfCorrectionPolicy` already structures feedback well (error code + diagnosis + specific next action). Adding an explicit "diagnosis step" to the error message format would bring it closer to the structured reflection pattern.

Source: [Failure Makes the Agent Stronger](https://arxiv.org/abs/2509.18847), [Dark Side of Self-Correction](https://aclanthology.org/2025.acl-long.1314/), [Self-Correction Review](https://theelderscripts.com/self-correction-in-llm-calls-a-review/)

---

### 9. The Core Problem: Wrong Class Fuzzy Matches

The specific failure scenario described (Builder calls `SetSpeed` on `ProjectileMovementComponent`, gets fuzzy matches like `SetSphereRadius`) is a class-scope mismatch in the fuzzy match source. The current `OliveBlueprintPlanResolver::ResolveCallOp` uses `FOliveNodeCatalog::FuzzyMatch()` which searches across all registered node display names, not the specific class being targeted.

From `OliveBlueprintPlanResolver.cpp` line 1710–1718:
```cpp
// Use catalog fuzzy match for "did you mean?" suggestions
FOliveNodeCatalog& Catalog = FOliveNodeCatalog::Get();
if (Catalog.IsInitialized())
{
    TArray<FOliveNodeSuggestion> Suggestions = Catalog.FuzzyMatch(Step.Target, CATALOG_SEARCH_LIMIT);
    for (const FOliveNodeSuggestion& Suggestion : Suggestions)
    {
        Alternatives.Add(Suggestion.DisplayName);
    }
}
```

`SetSphereRadius` is a valid node in the catalog (from `UShapeComponent`), so the Levenshtein distance to `SetSpeed` is short. The catalog has no knowledge that the target class is `ProjectileMovementComponent`. The result is useless noise.

The fix requires scoping the fuzzy match to the target class. If `target_class` is known (from `Step.TargetClass` or via resolver's class resolution trail), search `TFieldIterator<UFunction>` on that UClass and return those as alternatives rather than catalog-wide fuzzy matches.

Source: `OliveBlueprintPlanResolver.cpp` lines 1707–1757, `OliveNodeFactory.cpp` lines 2416–2591

---

### 10. Pre-Action Diagnosis: Trajectory Graph Copilot

The Trajectory Graph Copilot paper ([OpenReview](https://openreview.net/forum?id=ighxnB6nJF)) proposes analyzing action sequences _before_ execution using a Graph Neural Network trained on past trajectories to flag risky actions. It achieved 14.69% improvement in task completion across four benchmarks.

For Olive, a lightweight version of this concept is already partially implemented: the `FOlivePlanValidator` runs Phase 0 structural checks before execution. The gap is that Phase 0 doesn't check function name validity against the target class. Adding a "pre-resolve validation" pass that verifies `call` ops against the live class reflection before the plan executes would catch `SetSpeed`-on-`ProjectileMovementComponent` before wasting a turn.

Source: [Trajectory Graph Copilot](https://openreview.net/forum?id=ighxnB6nJF)

---

## Current State Assessment: What Olive Has vs. What It Needs

**Already implemented well:**
- Three-tier error classification (A=Fixable, B=Unsupported, C=Ambiguous) in `ClassifyErrorCode()`
- Progressive disclosure (terse→full→escalation) in `BuildToolErrorMessage()`
- Code-specific guidance in `BuildToolErrorMessage()` (28 distinct error codes handled)
- Auto-search on `ASSET_NOT_FOUND` — already injects relevant asset paths from the project index
- Plan hash deduplication — detects identical plan resubmissions
- Granular fallback mode — switches from plan_json to step-by-step after repeated failures
- Search trail in `FindFunctionEx()` — lists every location searched (but this trail goes into logs, not into the error response)

**Missing or broken:**
- `FUNCTION_NOT_FOUND` fuzzy matches are scoped to the wrong class — the catalog returns unrelated functions
- `FindFunctionEx().SearchedLocations` trail is NOT currently included in the error message back to the Builder (it goes to `UE_LOG` only)
- No automatic injection of the target class's actual callable functions on `FUNCTION_NOT_FOUND`
- No pre-resolve validation of function names before execution — errors only surface after the plan fails
- The `SelfCorrectionPolicy` `FUNCTION_NOT_FOUND` handler is one line: "Use blueprint.search_nodes to find the correct function name." This is vague compared to the four-part structure recommended above.

---

## Recommendations

### Fix 1 (Highest Value): Class-Scoped Function List on FUNCTION_NOT_FOUND

When `FUNCTION_NOT_FOUND` fires and a `target_class` was resolved (or can be inferred from the Blueprint's SCS), inject the actual list of callable UFunctions on that class into the error response.

Format for the injected list:
```
FUNCTION_NOT_FOUND: 'SetSpeed' not found on UProjectileMovementComponent.
Searched: GeneratedClass, parent hierarchy (UProjectileMovementComponent), SCS components.

Available functions on UProjectileMovementComponent:
  - SetVelocityInLocalSpace(FVector)
  - StopSimulating(FHitResult)
  - MaxSpeed [property, float — use set_var not call]
  - InitialSpeed [property, float — use set_var not call]

Likely fix: MaxSpeed is a property, not a function. Use op:set_var, target:MaxSpeed.
If you need runtime velocity control, call SetVelocityInLocalSpace or set MaxSpeed via set_var.
```

This gives the Builder the full picture in one turn instead of 3–4 guessing turns. It replaces the current catalog-wide fuzzy match (which finds `SetSphereRadius`) with class-scoped live reflection.

**Implementation path:**
- In `ResolveCallOp`, when `FindFunctionEx` returns null and `target_class` is resolved:
  1. Run `TFieldIterator<UFunction>(ResolvedClass)` to collect callable functions
  2. Separately run `TFieldIterator<FProperty>(ResolvedClass)` to find non-function properties with names similar to the requested name (flag them as "use set_var not call")
  3. Include top 8–10 results in the error's `Suggestion` field
  4. Include the `SearchedLocations` trail from `FindFunctionEx` in the message body

### Fix 2 (High Value): Include SearchedLocations in Error Response

`FindFunctionEx()` already builds a human-readable search trail (e.g., "Searched: GeneratedClass, UProjectileMovementComponent, UMovementComponent, UActorComponent, KismetSystemLibrary"). This trail is currently logged but NOT included in the error sent to the Builder. Adding it to the error message tells the Builder exactly which scopes were searched, eliminating redundant search attempts.

In `ResolveCallOp`:
```cpp
ErrorMessage = FString::Printf(
    TEXT("Function '%s' not found. Searched: %s."),
    *Step.Target, *SearchResult.BuildSearchedLocationsString());
```
This is already correct — `BuildSearchedLocationsString()` is already called here. The issue is that `Suggestion` is currently populated with catalog fuzzy matches (useless) instead of class-scoped alternatives (useful). The search trail IS in `ErrorMessage`. The fix is in the `Suggestion` field.

### Fix 3 (Medium Value): Distinguish Property vs. Function in Error Message

When the Builder tries `call SetMaxSpeed` and the correct answer is `set_var MaxSpeed`, the current error says only "function not found." A property-name match should be explicitly flagged:

After the `FindFunctionEx` failure, check if any `FProperty` on the target class matches the requested function name (or a substring of it). If so, add: `"'MaxSpeed' exists as a property, not a function. Use op:set_var to write it or op:get_var to read it."` This is the single most common class of wrong-function-name error in the UE Blueprint context.

### Fix 4 (Medium Value): Upgrade SelfCorrectionPolicy FUNCTION_NOT_FOUND Handler

Current handler (line 717 of `OliveSelfCorrectionPolicy.cpp`):
```cpp
else if (ErrorCode == TEXT("FUNCTION_NOT_FOUND"))
{
    Guidance = TEXT("The function was not found. Use blueprint.search_nodes to find the correct function name. Check for K2_ prefixes and class membership.");
}
```

This is vague. The error message already contains the search trail and class context from `ResolveCallOp`. But `BuildToolErrorMessage()` in `SelfCorrectionPolicy` only appends generic guidance — it doesn't read the `ErrorMessage` content. The guidance should be conditional on attempt number and class context:

- Attempt 1: "Check the error details for the searched classes. If a property matches the function name, use set_var instead."
- Attempt 2: "Call blueprint.read with include_pins:true to see the Blueprint's current structure. The searched classes list in the error shows where was checked."
- Attempt 3: Escalation with `editor.run_python` fallback.

### Fix 5 (Lower Value): Prompt-Level Read-Before-Call Instruction

Add to the Builder's system prompt section on plan_json (and granular tool use):

> "When calling a function on a component class (target_class), if you are not certain the function exists, call blueprint.describe_node_type or blueprint.read first. Do NOT guess function names. Properties (MaxSpeed, InitialSpeed) are NOT callable — use set_var for them."

This is Cursor's approach: a soft read-before-write instruction in the system prompt. Alone it is insufficient, but combined with Fix 1 it catches the cases where the model skips the instruction.

### Fix 6 (Research, Not Immediate): Aider-Style API Map Injection

Longer term, consider injecting a compact "component API surface" block into the plan context when the Scout or Researcher identifies a component-heavy Blueprint. For each component type identified in the Blueprint's SCS, include the top 5–10 callable functions (not all — token budget matters). This eliminates the need for the Builder to call `blueprint.describe` at all.

This mirrors Aider's repo map: signatures without bodies, enough to know what's callable, not so much that attention is diluted.

### Anti-Pattern to Avoid

**Do NOT expand the fuzzy match pool** to return more cross-class candidates on `FUNCTION_NOT_FOUND`. The current problem is that fuzzy matches cross class boundaries. More candidates from more classes is strictly worse. The fix is to _scope_ matches to the relevant class, not to _expand_ them.

**Do NOT add unbounded reflection output** to error messages. Cap class-function injection at 8–10 entries. "Lost in the middle" research shows LLM accuracy drops 30%+ when relevant information is buried in long context. A focused list of the 8 most relevant functions on the correct class is more effective than a dump of all 40+ callable functions.
