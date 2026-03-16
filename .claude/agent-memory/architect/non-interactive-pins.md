---
name: non-interactive-pins-root-cause
description: Root cause of non-draggable Blueprint pins created by Olive AI -- missing RF_Transactional and CreateNewGuid on type-specific node creators
type: project
---

Root cause: Type-specific node creators in OliveNodeFactory.cpp (CreateCallFunctionNode, CreateVariableGetNode, etc.) skip three engine-required finalization steps: `SetFlags(RF_Transactional)`, `CreateNewGuid()`, `PostPlacedNewNode()`. Only `CreateNodeByClass()` does all three.

**Why:** Without `RF_Transactional`, undo/redo corrupts pin state after `ReconstructNode` (which runs during compile). Without `CreateNewGuid()`, all nodes share zero GUID, confusing compiler pin remapping. The fix is a 4-line post-creation block in the central `CreateNode()` method.

**How to apply:** Design at `plans/non-interactive-pins-design.md`. Single call site fix in `CreateNode()` after creator returns, before `SetNodePosition()`. Guarded against double-call for `CreateNodeByClass` path.

**Key engine source findings:**
- `UEdGraph::AddNode` always calls `NotifyGraphChanged` -- `bFromUI` only sets `bUserInvoked` (cosmetic)
- `SGraphPanel::OnGraphChanged(GRAPHACTION_AddNode)` defers widget creation via `RegisterActiveTimer(0.f,...)`
- `SGraphPanel::OnGraphChanged(GRAPHACTION_Default)` does full purge + rebuild (`PurgeVisualRepresentation` + `Update`)
- Compile path: Stage IX calls `ReconstructNode` on all nodes, then `BroadcastChanged()` triggers full widget rebuild
- `FGraphEditorSummoner::OnTabRefreshed` calls `GraphEditor->NotifyGraphChanged()` which fires `GRAPHACTION_Default`
