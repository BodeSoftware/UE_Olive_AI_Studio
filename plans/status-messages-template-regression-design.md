# Status Messages & Template Regression Design

Two targeted fixes for the agent pipeline: (1) status messages not appearing in the chat UI during the planning phase, and (2) the Builder proactively calling `get_template` despite a "fallback-only" directive.

---

## Issue 1: Status Messages Not Appearing

### Root Cause Analysis

The `EmitStatus` lambda in `OliveCLIProviderBase.cpp` (line 582) fires `CurrentOnChunk.Execute(StatusChunk)` with only `StatusChunk.Text` populated. The ConversationManager autonomous-path lambda (line 283) checks `!Chunk.Text.IsEmpty()` and broadcasts `OnStreamChunk.Broadcast(Chunk.Text)`. The chat panel's `HandleStreamChunk` calls `AppendToLastMessage(Chunk)`, which creates a streaming assistant message if none exists.

Mechanically, the data flow is intact -- `Text` is populated, no struct field filters are blocking, and `AddAssistantMessage` creates a streaming message on first chunk. However, the status messages are emitted synchronously on the game thread BEFORE `LaunchCLIProcess()`, meaning they arrive as isolated text chunks during a window where:

1. The chat panel may not have processed `OnProcessingStarted` yet (delegate broadcast order).
2. The streaming message created by the first `EmitStatus` chunk may get overwritten or finalized when the actual CLI process starts streaming.
3. The status text (e.g., `*Analyzing task...*`) gets concatenated into the assistant message body via `AppendToLastMessage`, mixing pipeline status with the actual LLM response text.

The practical failure mode: even if status text appears briefly, it gets buried inside the assistant message content as the CLI's actual streaming text follows. The user never sees it as a distinct status indicator.

### Option A: Dedicated Status Delegate on ConversationManager

**Description:** Add `FOnOliveStatusUpdate` multicast delegate to `FOliveConversationManager`. The autonomous OnChunk lambda detects status chunks (via a new `bIsStatusMessage` field on `FOliveStreamChunk`) and broadcasts to the status delegate instead of `OnStreamChunk`. The chat panel subscribes and renders status messages in its existing status bar (`GetStatusText()`), replacing the generic "Processing..." text.

**Change scope:**
- `IOliveAIProvider.h`: Add `bool bIsStatusMessage = false` to `FOliveStreamChunk`
- `OliveCLIProviderBase.cpp`: Set `StatusChunk.bIsStatusMessage = true` in `EmitStatus` lambda
- `OliveConversationManager.h`: Add `FOnOliveStatusUpdate` delegate (`DECLARE_MULTICAST_DELEGATE_OneParam(..., const FString&)`)
- `OliveConversationManager.cpp`: In autonomous OnChunk lambda, route status chunks to `OnStatusUpdate.Broadcast()` instead of `OnStreamChunk.Broadcast()` and skip accumulating into `CurrentStreamingContent`
- `SOliveAIChatPanel.h/.cpp`: Subscribe to `OnStatusUpdate`, store status text, display in `GetStatusText()` override

**Tradeoffs:**
- (+) Clean separation -- status messages never pollute the assistant message content
- (+) Status bar is the natural UX location for pipeline progress
- (+) Status text is transient (replaced by next status, cleared when processing ends)
- (-) New delegate adds a subscription point; every future consumer needs to know about it
- (-) Slightly more invasive than field-only changes

**Risk: Low.** The delegate pattern is well-established in the codebase (OnProcessingStarted, OnStreamChunk, etc.).

### Option B: Route Status Through Existing GetStatusText Polling

**Description:** Instead of emitting status through the chunk callback at all, store the status string on the CLIProviderBase and expose it via an accessor. The ConversationManager polls this (or the chat panel's `GetStatusText()` already polls -- it's a bound `TAttribute`). Set a `CurrentPipelineStatus` string on the provider, and `GetStatusText()` checks it.

**Change scope:**
- `OliveCLIProviderBase.h`: Add `FString CurrentPipelineStatus` field + `GetPipelineStatus()` accessor
- `OliveCLIProviderBase.cpp`: Set `CurrentPipelineStatus` in the pipeline section, clear after `LaunchCLIProcess`
- `SOliveAIChatPanel.cpp`: In `GetStatusText()`, check `ConversationManager->GetCurrentProvider()->GetPipelineStatus()` when processing

**Tradeoffs:**
- (+) Zero delegate changes, zero struct changes
- (+) Leverages existing Slate polling (`GetStatusText()` is already a `TAttribute` binding)
- (-) Coupling: chat panel reaches through ConversationManager to the provider
- (-) The IOliveAIProvider interface does not expose this; need either a downcast or a method on ConversationManager itself
- (-) Polling frequency depends on Slate tick rate (~60fps is fine, but it's indirect)

**Risk: Low-Medium.** Works but creates an awkward dependency chain.

### Option C: Emit Status as Assistant Message via OnStreamChunk, Separated by Completion

**Description:** Before emitting status, broadcast a `CompleteLastMessage` signal (or use a chunk with `bIsComplete = true`) so the status text starts as its own message bubble. After the pipeline completes and before CLI launch, complete the status message and let the CLI stream create a fresh assistant message.

**Change scope:**
- `OliveConversationManager.h/.cpp`: Add `CompleteCurrentStreaming()` method that broadcasts a signal to finalize the current message
- `OliveCLIProviderBase.cpp`: After all `EmitStatus` calls and before `LaunchCLIProcess`, emit a completion marker
- `SOliveAIMessageList.cpp`: Handle the completion marker to call `CompleteLastMessage()`

**Tradeoffs:**
- (+) Status appears as a visible message bubble in the chat
- (+) No new delegate types needed
- (-) Status messages persist in the chat history as assistant messages (visual clutter)
- (-) Multiple message bubbles for a single response flow looks fragmented
- (-) The status text (markdown-italic like `*Analyzing task...*`) looks odd as a standalone assistant message

**Risk: Medium.** UX is debatable -- persistent status messages in chat history may confuse users.

### Option D: Log-Only Status (No UI Change)

**Description:** Replace `EmitStatus` with `UE_LOG(LogOliveAI, Display, ...)`. Status appears in the Output Log but not in the chat panel.

**Change scope:**
- `OliveCLIProviderBase.cpp`: Replace `EmitStatus` lambda body with `UE_LOG`

**Tradeoffs:**
- (+) Trivial change, zero risk of breaking anything
- (-) Users don't see the Output Log during normal use; the 30-120s silence persists in the chat UI
- (-) Defeats the purpose of the feature

**Risk: None.** But also provides minimal user value.

### Recommendation: Option A (Dedicated Status Delegate)

Option A provides the cleanest UX and cleanest code separation. Status messages belong in the status bar, not in the message stream. The implementation is small (one bool field, one delegate, two subscriber changes) and follows existing patterns. The chat panel's `GetStatusText()` already returns "Processing..." during this phase -- replacing it with actual pipeline stage text ("Analyzing task...", "Build plan ready. Launching builder...") is a natural enhancement.

**Key implementation detail:** The ConversationManager should store the latest status text (e.g., `FString CurrentStatusText`) and expose it so `GetStatusText()` can return it when set. This avoids adding a delegate entirely -- the existing polling in `GetStatusText()` just needs a richer source. This is effectively a hybrid of A and B: store the status on ConversationManager (not the provider), and let the existing `GetStatusText()` polling display it. No new delegate needed.

**Simplified recommendation (A+B hybrid):**
- Add `FString PipelineStatusText` to `FOliveConversationManager`
- In the autonomous OnChunk lambda, detect `bIsStatusMessage` and store to `PipelineStatusText` instead of broadcasting
- `SOliveAIChatPanel::GetStatusText()` checks `ConversationManager->PipelineStatusText` before returning "Processing..."
- Clear `PipelineStatusText` when processing completes
- Still add `bIsStatusMessage` to `FOliveStreamChunk` so the routing is clean

This gives status bar display with minimal code change and no new delegates.

---

## Issue 2: Builder Calling get_template Proactively

### Root Cause Analysis

The Planner is told at line 2244: "Read these with `blueprint.get_template` before writing the Build Plan." And again at line 2276: "call `blueprint.get_template` on each matched template to study their function implementations." The Planner dutifully fetches templates and produces template-informed function descriptions in the Build Plan.

The Builder then receives:
- Section 2.5: Template IDs with "fallback only" label
- Section 3: Build Plan with detailed function descriptions that reference template patterns
- Section 5: Step 4b explicitly mentions `get_template` as a fallback

The Builder, seeing template IDs in its context AND function descriptions that clearly derive from template study, interprets this as a signal that template verification is part of the workflow. The "fallback-only" label conflicts with the template-rich context. LLMs resolve conflicting signals by following the stronger contextual cue -- and the presence of template IDs + template-informed descriptions is a stronger cue than a single-line prohibition.

### Option A: Strip Template IDs from Builder's View

**Description:** Remove Section 2.5 entirely from `FormatForPromptInjection()`. The Builder receives the Build Plan (which contains all the information from templates, distilled by the Planner) but never sees the template IDs. No IDs visible means no way to call `get_template` proactively.

**Change scope:**
- `OliveAgentPipeline.cpp`: Remove the Section 2.5 block (lines 448-474) from `FormatForPromptInjection()`
- `OliveAgentPipeline.cpp`: Remove step 4b from Section 5 (lines 556-557) that mentions `get_template` as fallback

**Tradeoffs:**
- (+) Eliminates the root cause entirely -- Builder cannot fetch what it cannot name
- (+) Simple deletion, minimal risk of introducing new bugs
- (-) Builder loses the genuine fallback path. If plan_json fails on a function, the Builder has no way to recover by studying the original template pattern
- (-) Reliance on the Planner producing sufficiently detailed descriptions increases

**Risk: Low** for the change itself. **Medium** for downstream quality -- the fallback path was designed to catch Planner-to-Builder translation loss.

### Option B: Strengthen the Prohibition

**Description:** Keep Section 2.5 but reword it more aggressively: "Do NOT call `blueprint.get_template` proactively. The Build Plan already contains all template knowledge. Only call it if plan_json returns an error you cannot fix from the error message alone."

**Change scope:**
- `OliveAgentPipeline.cpp`: Rewrite Section 2.5 text (lines 451-454)
- `OliveAgentPipeline.cpp`: Rewrite step 4b text (lines 556-557)

**Tradeoffs:**
- (+) Preserves the fallback path
- (+) Minimal code change
- (-) LLMs are unreliable at following prohibitions when surrounding context contradicts them. The Planner-informed function descriptions still implicitly signal that templates were important enough to read. This may not solve the problem.
- (-) Requires prompt engineering iteration to get the wording right

**Risk: Medium.** May not solve the root issue. Prompt-level fixes are inherently fragile.

### Option C: Make Planner Template Fetching Optional, Not Mandatory

**Description:** Change the Planner directive from "Read these with `blueprint.get_template` before writing" to "These templates are available if you need reference. Use `get_template` selectively for functions you're unsure about." The Planner produces less template-saturated descriptions, reducing the implicit cue to the Builder.

**Change scope:**
- `OliveAgentPipeline.cpp`: Rewrite line 2244 and lines 2276-2278 (Planner system prompt)

**Tradeoffs:**
- (+) Addresses the root cause upstream -- less template saturation in the Build Plan
- (+) Reduces Planner token usage (fewer tool calls = faster pipeline)
- (-) Planner may produce worse function descriptions for complex patterns that genuinely need template study
- (-) The Planner already has the template IDs in context, so it may still fetch them anyway (just less reliably)
- (-) Does not fully address the Builder side -- Builder still sees template IDs in Section 2.5

**Risk: Medium.** Degrades Planner quality for complex tasks. Partial fix for the Builder side.

### Option D: Strip IDs from Builder, Keep Planner Prescriptive (Recommended)

**Description:** Keep the Planner's current behavior (mandatory template reading) so the Build Plan contains high-quality, template-informed function descriptions. But strip template IDs from the Builder's view entirely (remove Section 2.5) and rewrite step 4b to reference error-recovery strategies that do not involve templates.

The Builder gets the full benefit of template research (via the Planner's detailed descriptions) without seeing the source material. If plan_json fails, the Builder should use `blueprint.read` to inspect what was created and the error message to fix it -- not re-fetch templates.

**Change scope:**
- `OliveAgentPipeline.cpp`: Remove Section 2.5 block (lines 448-474) from `FormatForPromptInjection()`
- `OliveAgentPipeline.cpp`: Rewrite step 4b (lines 556-557) to: `"b. If plan_json fails, read the error message carefully. Use blueprint.read to inspect what was created. Fix the first error and retry before moving on."`

**Tradeoffs:**
- (+) Eliminates the root cause without degrading Planner quality
- (+) The Builder's error-recovery path (read error + inspect + fix) is more robust than template-copying anyway
- (+) Reduces Builder prompt size slightly (Section 2.5 removed)
- (+) The Planner's template-informed descriptions ARE the distilled knowledge -- re-fetching templates is redundant
- (-) In rare cases where the Planner's description is insufficient AND the template would have helped, the Builder has no escape hatch. However, this is mitigable: the Builder can still call `blueprint.describe` on existing assets, call `get_recipe`, or use `editor.run_python`.
- (-) Does not remove `get_template` from the Builder's available MCP tools. The Builder can still discover and call it independently. But without IDs in prompt, the activation energy is much higher.

**Risk: Low.** This is the safest option that actually addresses the root cause. The Planner retains full template access; the Builder loses only the prompt cue to use it proactively.

### Option E: Template Fetch Budget

**Description:** Add "Budget: 0 proactive template calls" to Section 2.5.

**Change scope:**
- `OliveAgentPipeline.cpp`: Add budget text to Section 2.5

**Tradeoffs:**
- (+) Minimal change
- (-) LLM budget adherence is inconsistent
- (-) Still shows template IDs, which triggers the proactive pattern

**Risk: High.** Unreliable. Budget framing helps in some contexts but the contradicting signals remain.

### Recommendation: Option D (Strip IDs from Builder, Keep Planner Prescriptive)

Option D correctly identifies the information asymmetry that should exist between Planner and Builder. The Planner is a researcher -- it reads templates to produce a high-quality plan. The Builder is an executor -- it follows the plan. Showing the Builder the Planner's sources creates a "source-checking" impulse that wastes tokens and time.

The key insight: **the Build Plan IS the distillation of template knowledge.** Re-fetching templates is always redundant when the Build Plan exists. The only scenario where templates add value to the Builder is when the Build Plan's description is wrong -- and in that case, the error message from plan_json will be more diagnostic than the template anyway.

Step 4b should redirect to the error-recovery tools the Builder already has: `blueprint.read` (inspect created nodes), error messages (from plan_json), and `blueprint.compile` (catch type errors). These are more targeted than template re-study.

---

## Implementation Order

1. **Issue 2 first** (template regression) -- pure text changes in `FormatForPromptInjection()`, no struct or delegate changes, immediately testable with the next Builder run
2. **Issue 1 second** (status messages) -- requires the `bIsStatusMessage` field addition and ConversationManager/chat panel plumbing

Both issues are independent and can be implemented in parallel if two coders are available, but Issue 2 is simpler and higher-impact (saves 1-3 wasted tool calls per run).

---

## Files Modified (Summary)

| Issue | File | Change |
|-------|------|--------|
| 1 | `IOliveAIProvider.h` | Add `bIsStatusMessage` to `FOliveStreamChunk` |
| 1 | `OliveCLIProviderBase.cpp` | Set `bIsStatusMessage = true` in `EmitStatus` |
| 1 | `OliveConversationManager.h` | Add `FString PipelineStatusText` field + accessor |
| 1 | `OliveConversationManager.cpp` | Route status chunks to `PipelineStatusText`, clear on complete |
| 1 | `SOliveAIChatPanel.cpp` | `GetStatusText()` returns `PipelineStatusText` when set |
| 2 | `OliveAgentPipeline.cpp` | Remove Section 2.5, rewrite step 4b in `FormatForPromptInjection()` |
