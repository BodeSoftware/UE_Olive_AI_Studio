# Section 17 Remaining Work - Phased Plan

## Already Covered (Excluded Here)

- PIE write rejection in write paths
- Basic transaction wrapping for write operations
- `unsaved_changes` in Blueprint IR
- Basic compile result capture (`compile_result`)
- JSON-RPC parse/method error handling (`-32700`, `-32601`)
- Port fallback attempts when MCP preferred port is in use
- Pin connection compatibility checks with conversion attempts in connector paths

## Phase 1 - Asset Safety and Runtime Stability

Goal: close asset-state failure gaps that can corrupt flow or hide state from the agent.

- Add `redirected_from` to relevant read tool responses when resolver follows redirectors.
- Add explicit `compile_status` field to write results (`success|warning|error|dirty`) alongside `compile_result`.
- Implement circular Blueprint-reference traversal guards with visited-set + depth cap (20) in cross-reference readers/walkers.
- Add hot-reload resilience hooks:
  - re-resolve critical UObject references immediately before mutation
  - invalidate any active context/cache snapshots used by chat/brain assembly
- Add GC safety for long-running write operations:
  - operation-scoped reference rooting (or equivalent guarded lifetime strategy)
  - explicit release at operation end

Exit criteria:

- Redirected assets return original and resolved path metadata.
- Compile failures always expose `compile_status: "error"`.
- Circular reference traversal cannot recurse indefinitely.
- Hot reload during active operation does not execute stale pointers.

## Phase 2 - Graph Edit Integrity

Goal: make graph mutations robust against invalid authoring intents.

- Validate node type against node catalog before add-node execution.
- Return fuzzy node suggestions for unknown types.
- Add post-op orphaned-exec-flow detection and warning payloads.
- Enforce duplicate native-event prevention (for example second `BeginPlay`) with structured validation errors.
- Expand node-removal report payloads to include broken/disconnected link summaries.
- Add large-graph read mode:
  - summary-first payload for large graphs (500+ nodes)
  - detail-on-demand paging mechanism

Exit criteria:

- Unknown node type errors include top suggestions.
- Duplicate native events are blocked pre-mutation.
- Large graphs do not force full-detail payloads by default.

## Phase 3 - Chat UX Resilience

Goal: keep chat usable and predictable under contention, network faults, and long operations.

- Queue user messages while a run is in progress; surface "waiting for current operation" status.
- Add provider retry manager with exponential backoff (up to 3 retries) for transient network failures.
- Add rate-limit handling UX:
  - parse `Retry-After`
  - show retry countdown
  - auto-retry at expiry
- Add long-response truncation detection and visible warning.
- Prevent focus profile switching mid-operation (defer switch + warning banner).
- Decouple operation execution lifetime from panel lifetime:
  - closing panel must not cancel in-flight work
  - completion notification/toast for background completion
- When prompt/context trimming occurs, inject explicit truncation note into model-visible context.

Exit criteria:

- Sending messages during processing never drops user input.
- Transient network failures automatically retry with bounded attempts.
- Closing the panel does not terminate active operations.

## Phase 4 - MCP Multi-Agent and Protocol Hardening

Goal: enforce correct behavior under concurrent external clients.

- Add write serialization for MCP tool execution (global write mutex or equivalent queue).
- Preserve concurrent read capability while writes are serialized.
- Improve unknown tool-call error payloads to include discoverability hint payload (tool list pointer and/or top candidates).
- Surface actual bound MCP port in editor UI (not log-only) after fallback binding.
- Add disconnect-mid-operation handling/reporting for async tool calls (ensure clear final state event semantics).

Exit criteria:

- Concurrent agents cannot interleave write mutations.
- UI always shows the real MCP port bound at runtime.

## Phase 5 - Confirmation and Agentic Loop Edge Cases

Goal: make confirmation and self-correction behavior deterministic and safe for complex runs.

- Add plan-to-execution drift detection:
  - snapshot/fingerprint target before confirmation
  - revalidate at execute time
  - force re-plan when drift detected
- Add editable Tier 2 plan parser/validator with inline error reporting.
- Add mixed-tier plan splitter:
  - Tier 1 auto steps
  - Tier 2 confirm steps
  - Tier 3 preview steps
- Add cancel semantics for partially executed confirmed multi-step operations:
  - pre-op snapshot
  - all-or-nothing rollback path
- Complete Tier 3 "accept preview version" path with transactional swap + cleanup.
- Refactor self-correction retry accounting to be per error signature (not one global counter).
- Add deterministic oscillation stop policy matching section-17 behavior (A/B repeat stop after bounded cycles).
- Add profile-aware unavailable-tool feedback (suggest focus profile switch instead of generic missing-tool text).
- Count only write retries toward correction cap; allow read steps during self-correction without consuming write budget.

Exit criteria:

- Confirmed operations detect drift before mutating.
- Retry policy no longer penalizes unrelated new errors.
- Loop detection stops A/B oscillations with clear user-facing report.

