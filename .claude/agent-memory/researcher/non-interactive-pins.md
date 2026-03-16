---
name: non-interactive-pins-root-cause
description: Root cause analysis for programmatically created Blueprint node pins being non-draggable until editor restart
type: project
---

Research report: `plans/research/non-interactive-pins-ue55.md` (2026-03-14)

**Root cause:** `CreateNodeByClass` does NOT call `PostPlacedNewNode()`. For most nodes this is
a no-op, but for `UK2Node_FunctionResult` it skips `SyncWithEntryNode()` + `SyncWithPrimaryResultNode()`,
and for `UK2Node_CallFunction` it skips `FunctionReference.RefreshGivenNewSelfScope()`.

**Mechanism for non-interactive pins:**
- `SGraphPin::OnMouseButtonDown` gates drag on: (1) `IsEditingEnabled()` — requires `OwnerNodePtr`
  valid AND `OwnerGraphPanelPtr` valid; (2) `!GraphPinObj->bNotConnectable`; (3) `GetOwnerPanel()` valid.
- `OwnerNodePtr` is set during `SGraphNode::CreatePinWidgets()` → `SGraphPin::SetOwner()`.
- `OwnerGraphPanelPtr` is set during `SGraphPanel::AddGraphNode()` → `SGraphNode::SetOwner()`.
- If Slate holds `SGraphPin` widgets pointing to OLD `UEdGraphPin*` objects (the ones created by
  first `AllocateDefaultPins`, destroyed and replaced by `ReconstructNode`), those old pins may
  have `bOrphanedPin=true` / `bNotConnectable=true` → drag silently blocked.

**The "restart fixes it" symptom** is explained by: on restart, `Update()` creates fresh widgets
from the current (post-reconstruct) pin state. No stale SGraphPin→UEdGraphPin pointer exists.

**Key epic creation sequence (canonical):**
`AddNode(bFromUI=true)` → `CreateNewGuid()` → `PostPlacedNewNode()` → `AllocateDefaultPins()` → NO ReconstructNode.

**Our sequence:** `CreateNewGuid` → `AllocateDefaultPins` → `AddNode(false,false)` → `ReconstructNode` → `NotifyGraphChanged()` — diverges in 3 ways.

**Fix:** Call `PostPlacedNewNode()` after `AllocateDefaultPins()`; remove `ReconstructNode()` from
`CreateNodeByClass` base case; remove the explicit `Graph->NotifyGraphChanged()` from
`CreateNodeByClass` (it's already called by `ReconstructNode` via `NotifyNodeChanged`, and again
by `RefreshBlueprintEditorState` in `OliveGraphWriter::AddNode`).

**Why:** `bNotConnectable` on reconstructed pins after `ReconstructNode` → old pin widgets held
by Slate have `bNotConnectable=true` or `bOrphanedPin=true`, blocking drag silently.
