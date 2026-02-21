# Phase B Plan: 

This document covers only:

- Phase B: Chat UX + Context Completion (Blueprint-first, big-task friendly, YOLO optional)

Non-goals for these phases:
- New provider integrations (OpenAI/Google/Ollama): Phase C
- Full Brain-layer orchestration beyond minimal “Run” scaffolding: Phase E

---

## Product Requirements (What “Elite” Means)

- Blueprint/non-code users can complete large tasks without needing to understand tool schemas or asset paths.
- The UI makes it obvious what happened: what changed, where, and how to undo.
- Big tasks are resumable and recoverable (checkpoints, retry/skip, rollback).
- “YOLO” mode exists for power users, without removing hard safety rails.
- Hybrid projects (C++ + BP) behave predictably: when both exist, the assistant doesn’t thrash or duplicate work.

---


## Phase B: Chat UX + Context Completion (Blueprint-First)

### B0. UX Principle: “Land Me Where It Changed”

Every write result must offer one-click navigation:
- Open asset editor
- Focus graph
- Select/zoom relevant nodes (when available)
- Show compile errors inline and jump to source

This is non-negotiable for non-code users.

---

### B1. Context From Reality (Open Asset + Selection)

Deliverables:
- Auto-context:
  - currently open asset (Blueprint/BT/PCG)
  - selected nodes/items (Blueprint nodes, BT nodes, PCG nodes)
  - current compile errors (summary)
- Debounced updates (avoid thrash).

Primary touchpoints:
- `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp` (complete selection hook TODO)
- `Source/OliveAIEditor/Private/UI/SOliveAIContextBar.cpp`

Acceptance criteria:
- Opening a Blueprint updates context bar automatically.
- Selecting nodes updates the context payload used for the next request.

---

### B2. Typed Result Cards (Replace Raw JSON Feel)

Deliverables:
- Render tool results as typed cards when possible:
  - Blueprint read summary card
  - Write result card (what changed)
  - Compile errors card (clickable list)
  - Snapshot/rollback card
- Keep a “show raw JSON” expander for debugging.

Primary touchpoints:
- `Source/OliveAIEditor/Private/UI/SOliveAIMessageList.cpp`

Acceptance criteria:
- Most common Blueprint operations are understandable without reading JSON.

---

### B3. Big Tasks: “Run Mode” (Plan + Checkpoints + Recovery)

Problem:
- Large tasks need a workflow beyond a flat chat transcript.

Deliverables (minimal viable “Run Mode” for B phase):
- A run object stored per session:
  - name, created time
  - steps (each step references tool call(s), status, duration)
  - checkpoints (snapshot IDs)
- UI:
  - hierarchical operation view: Run -> Step -> Tool calls
  - controls: pause/resume, cancel, retry step, skip step, rollback to checkpoint
- Checkpoint strategy:
  - snapshot before first write in a run
  - snapshot every N steps (configurable) OR per asset group

Primary touchpoints:
- UI:
  - `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp`
  - `Source/OliveAIEditor/Private/UI/SOliveAIMessageList.cpp`
- Cross-system:
  - `Source/OliveAIEditor/CrossSystem/Private/OliveSnapshotManager.cpp`
  - `Source/OliveAIEditor/CrossSystem/Private/OliveMultiAssetOperations.cpp`

Acceptance criteria:
- A multi-step Blueprint task shows step-by-step progress with at least one checkpoint.
- Failure offers one-click rollback to last checkpoint.

---

### B4. YOLO Mode (Fast Preset) With Visible Guardrails

Goal:
- Users can opt into speed without losing safety.

Deliverables:
- Add “Safety Preset” concept:
  - `Careful` (default): Tier 2 for graph edits, Tier 3 for destructive
  - `Fast`: Tier 1 for low-risk categories, Tier 2 for graph edits
  - `YOLO`: Tier 1 for most categories, Tier 2 or explicit confirm for destructive
- UI toggle in chat header showing current preset (badge).
- Always keep hard rails:
  - PIE protection
  - transactions/undo
  - path safety

Primary touchpoints:
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`
- `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
- `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp`

Acceptance criteria:
- Switching preset changes confirmation behavior immediately.
- YOLO is always visually obvious and reversible.

---

### B5. Blueprint-Specific UX: Node/Graph Navigation

Deliverables:
- On successful graph edits, provide:
  - “Open graph”
  - “Select nodes involved”
  - “Zoom to fit”
- On compile errors, provide:
  - “Open graph at error” when resolvable
  - fallback: open Blueprint and show compile panel

Primary touchpoints:
- `Source/OliveAIEditor/Private/Services/OliveAssetResolver.cpp` (open editor)
- Blueprint editors/utilities integration (new helper likely needed under `Source/OliveAIEditor/Blueprint/`)

Acceptance criteria:
- Users never have to manually search for the changed graph after an operation.

---

## Edge Cases Checklist (Must Handle in A/B)

- PIE active:
  - reads ok, writes blocked with clear reason and next step
- Asset open in editor:
  - allow reads; writes warn and proceed safely or queue (define behavior)
- Rename/refactor impacts:
  - show referencers warnings in preview and results
- Multi-asset run partial failure:
  - step-level retry and checkpoint rollback
- Large Blueprint graphs:
  - pagination and targeted reads (avoid dumping huge IR into prompt)
- Hybrid C++ + BP:
  - if both implementations exist, prefer referencing existing API
  - avoid duplicating functionality without explicit user intent

---

## Phase A Exit Criteria (Gate to Phase B)

- Tier 3 preview is real and token-gated.
- Compile errors are structured.
- Schema validation rejects malformed inputs reliably.
- Rate limiting exists and is enforced.
- MCP progress transport is chosen and minimally functional (or explicit “poll events” endpoint exists).

## Phase B Exit Criteria

- Auto-context from open asset + selection works.
- Typed result cards exist for common Blueprint workflows.
- Run Mode supports big tasks with checkpoints + rollback.
- YOLO preset exists with visible guardrails.
- Post-write navigation (open/focus/select) works for Blueprint graph edits.
