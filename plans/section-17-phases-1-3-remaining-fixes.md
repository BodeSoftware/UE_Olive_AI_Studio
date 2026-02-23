# Section 17 (Phases 1-3) Remaining Fixes Review

Date: 2026-02-22  
Scope: completeness follow-up for `plans/section-17-remaining-implementation-phases.md` phases 1-3.

## Intentional/Reasonable Design Drift Found

1. Queue overflow behavior appears intentionally changed in Phase 3 design.
- Evidence: `plans/phase3-chat-ux-resilience-design.md` explicitly defines bounded queue depth and dropping oldest on overflow.
- Refs: `plans/phase3-chat-ux-resilience-design.md:122`, `plans/phase3-chat-ux-resilience-design.md:123`, `plans/phase3-chat-ux-resilience-design.md:905`, `Source/OliveAIEditor/Public/Chat/OliveMessageQueue.h:27`, `Source/OliveAIEditor/Private/Chat/OliveMessageQueue.cpp:22`.
- Conflict: Section 17 exit criterion says input should "never drop user input".
- Conclusion: This is likely a spec mismatch between Section 17 and later Phase 3 design, not an accidental omission.

2. "Invalidate active context/cache snapshots" may be partially N/A in current architecture.
- Evidence: no dedicated long-lived "chat context cache" object was found in current chat/brain path; prompt assembly builds context from project index each send.
- Refs: `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp:387`, `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp:118`, `Source/OliveAIEditor/Private/Index/OliveProjectIndex.cpp:132`.
- Conclusion: requirement likely predates/refers to planned cache layers that were not fully introduced yet.

## Remaining Items That Should Still Be Fixed

1. Add visible long-response truncation warning in chat UX.
- Why still required: explicitly in Section 17 and Phase 3 design; provider finish reason is already captured but not surfaced.
- Current gap:
  - Providers set finish reason: `Source/OliveAIEditor/Private/Providers/OliveOpenAIProvider.cpp:395`, `Source/OliveAIEditor/Private/Providers/OliveOpenRouterProvider.cpp:382`, `Source/OliveAIEditor/Private/Providers/OliveGoogleProvider.cpp:597`.
  - Conversation completion ignores finish reason for warning/UI: `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp:535`.
- Fix needed: wire truncation detection in `HandleComplete` and UI signal/warning path.

2. Bound rate-limit retries (429) to match design intent.
- Why still required: Phase 3 design states "retry once" for rate limit; current code can repeatedly reschedule.
- Current gap: no guard to stop repeated rate-limit retries.
- Refs: `plans/phase3-chat-ux-resilience-design.md:243`, `Source/OliveAIEditor/Private/Providers/OliveProviderRetryManager.cpp:510`, `Source/OliveAIEditor/Private/Providers/OliveProviderRetryManager.cpp:519`.
- Fix needed: track/ration rate-limit retry count per request and cap to one retry (or configurable bounded count).

3. Ensure redirect metadata is returned consistently for Blueprint read tools.
- Why still required: Phase 1 asks for redirected metadata on relevant reads; one reader path bypasses resolver.
- Current gap: `blueprint.list_overridable_functions` uses raw path read and does not attach `redirected_from`.
- Refs: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp:1257`, `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp:1270`, `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp:1284`.
- Fix needed: resolve path via `FOliveAssetResolver`, read via resolved path, add `redirected_from`.

4. Strengthen GC safety for hot-reload re-resolved asset pointer.
- Why still required: pipeline roots original target asset, but if hot-reload re-resolves to a different UObject pointer, that pointer is not explicitly rooted for the operation scope.
- Current gap:
  - Root original: `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp:157`
  - Swap to fresh pointer: `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp:425`, `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp:431`
- Fix needed: root whichever pointer is actually mutated (original and/or re-resolved live asset) for full operation scope.

5. Resolve spec decision: queue drop policy vs "never drop user input".
- Why still required: this is blocking "complete" status for Phase 3 exit criteria.
- Options:
  - Option A: keep bounded queue/drop-oldest and update Section 17 wording/exit criteria.
  - Option B: preserve all user input (no drops), and move bounding to UI warnings/backpressure only.
- Refs: `plans/section-17-remaining-implementation-phases.md:72`, `plans/phase3-chat-ux-resilience-design.md:122`, `plans/phase3-chat-ux-resilience-design.md:905`.

## Suggested Priority

1. Truncation warning wiring  
2. Rate-limit retry cap  
3. Queue policy decision (spec alignment)  
4. Redirect metadata consistency  
5. GC rooting of re-resolved live asset

