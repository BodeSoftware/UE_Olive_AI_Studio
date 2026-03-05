# Log Improvements Design

**Date:** 2026-03-03
**Status:** Ready for implementation
**Scope:** 4 items, 8 tasks, 3 C++ files changed, 3 content files created/updated

---

## Item 1: Fix modify_component Property Success Reporting

### Problem

`HandleBlueprintModifyComponent` in `OliveBlueprintToolHandlers.cpp` line 3449 reports `modified_properties_count` using `Properties.Num()` (the requested count), not the actual success count. The AI sees "4 modified" when only 3 succeeded.

### Root Cause

`FOliveComponentWriter::ModifyComponent()` tracks `SuccessCount` locally (line 328) and logs it (line 352), but never returns it through the result struct. The tool handler has no way to recover the number except by inferring it.

### Design Decision: Infer from Warnings (no struct change)

Adding a field to `FOliveBlueprintWriteResult` would be a cross-cutting change for one consumer. Instead, since `ModifyComponent()` adds exactly one warning per failed property (line 340), the actual success count is:

```
int32 ActualSuccessCount = Properties.Num() - WriteResult.Warnings.Num();
```

This inference is safe because:
- The only early return that adds a warning ("No properties specified") returns before the executor lambda runs (line 303-308, which precedes the tool handler's lambda)
- Inside the property loop (lines 330-342), each failure adds exactly one warning with prefix "Failed to set"
- No other code path in `ModifyComponent` adds warnings

### Task T1: Fix modified_properties_count and add failed_properties

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
**Lines:** 3445-3460 (inside the executor lambda for HandleBlueprintModifyComponent)
**Complexity:** Trivial (5 lines changed)

Replace the current success block:

```cpp
// Current (line 3449):
ResultData->SetNumberField(TEXT("modified_properties_count"), Properties.Num());
```

With:

```cpp
// New:
const int32 FailedCount = WriteResult.Warnings.Num();
const int32 ActualSuccessCount = Properties.Num() - FailedCount;
ResultData->SetNumberField(TEXT("modified_properties_count"), ActualSuccessCount);
ResultData->SetNumberField(TEXT("requested_properties_count"), Properties.Num());

if (FailedCount > 0)
{
    ResultData->SetNumberField(TEXT("failed_properties_count"), FailedCount);
}
```

The existing warnings array serialization (lines 3452-3459) already includes per-property failure messages, so no further changes needed there. The AI now sees:

```json
{
  "modified_properties_count": 3,
  "requested_properties_count": 4,
  "failed_properties_count": 1,
  "warnings": ["Failed to set 'BadProp': Property not found"]
}
```

### Edge Cases

- **Zero properties requested:** `ModifyComponent` returns early at line 303 with `bSuccess = true` + warning "No properties specified". The tool handler lambda never runs because the early return becomes the executor result. But wait -- the early return IS the executor result. Let me check... The executor lambda calls `Writer.ModifyComponent(...)` and checks `!WriteResult.bSuccess`. If 0 props, it returns success with a warning. The lambda continues to the success block. `Properties.Num() = 0`, `Warnings.Num() = 1` ("No properties specified"), so `ActualSuccessCount = -1`. **Fix:** Guard against this: `FMath::Max(0, Properties.Num() - FailedCount)`. Actually wait -- when Properties is empty, the tool handler should have validated that before reaching the executor. Let me check...

Looking at lines 3410-3429, the handler extracts properties from params but there's no empty-properties guard before the executor. So the zero case can reach the lambda. The fix:

```cpp
const int32 FailedCount = WriteResult.Warnings.Num();
const int32 ActualSuccessCount = FMath::Max(0, Properties.Num() - FailedCount);
```

- **All properties fail:** `ModifyComponent` sets `bSuccess = false` (line 346). The executor lambda returns `ExecutionError` at line 3438. The success block never runs. No issue.

---

## Item 2: Orphan Detection Delta Approach

### Problem

`DetectOrphanedExecFlows` runs in Stage 5 (Verify) of the write pipeline on every granular graph-edit operation (`add_node`, `connect_pins`, `remove_node`, `disconnect_pins`). It reports the FULL orphan count every time, so the AI sees 33 orphan warnings across a session where most are pre-existing.

### Design: Per-Run Baseline Snapshot

**When:** At the start of each brain run (when brain transitions to `WorkerActive`)
**What:** Snapshot per-graph orphan counts as `TMap<FString, int32>` (graph path -> orphan count)
**Where:** On `FOliveWritePipeline` (it already owns the detection logic)
**How:** `FOliveBrainLayer::OnStateChanged` delegate fires on transition; `FOliveWritePipeline` subscribes

The delta approach:
1. On `WorkerActive` entry, do NOT eagerly scan all graphs (expensive, which graphs?). Instead, use lazy snapshotting: the first time a graph is checked during this run, if no baseline exists for that graph, the current count becomes the baseline.
2. On subsequent checks of the same graph, only report NEW orphans (current - baseline).
3. On `blueprint.compile`, report the full absolute count (no delta filtering).
4. On run completion (any terminal state), clear the baseline map.

**Why lazy over eager:** We don't know which graphs will be edited. Eager scanning would need to scan ALL graphs in ALL open Blueprints, which is wasteful and still might miss the right graph.

### Task T2: Add orphan baseline storage to write pipeline

**File:** `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h`
**Complexity:** Small (add 3 fields + 2 methods in the State section)

Insert before the closing `};` of the class, in the State section (after line 389):

```cpp
// ============================================================================
// Orphan Detection Baseline (per-run delta tracking)
// ============================================================================

/**
 * Capture the current orphan count for a graph as the baseline.
 * Called lazily on first graph-edit check per run, NOT eagerly on run start.
 * Idempotent: does nothing if a baseline already exists for this graph.
 *
 * @param GraphPath Unique graph identifier (Blueprint path + graph name)
 * @param CurrentOrphanCount The current absolute orphan count
 */
void SetOrphanBaseline(const FString& GraphPath, int32 CurrentOrphanCount);

/**
 * Get the number of NEW orphans since the baseline was captured.
 * Returns the absolute count if no baseline exists (first check).
 *
 * @param GraphPath Unique graph identifier
 * @param CurrentOrphanCount The current absolute orphan count
 * @return Delta (new orphans since baseline), minimum 0
 */
int32 GetOrphanDelta(const FString& GraphPath, int32 CurrentOrphanCount) const;

/** Clear all orphan baselines (called on run end) */
void ClearOrphanBaselines();

/** Per-run orphan baselines: graph path -> orphan count at start of run */
TMap<FString, int32> OrphanBaselines;

/** Whether a brain run is currently active (baselines are valid) */
bool bRunActive = false;
```

### Task T3: Implement delta logic + brain subscription

**File:** `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
**Complexity:** Medium (30-40 new lines, modify existing orphan reporting block)

**Step 1:** Add the three new methods at the end of the file, before any existing helper sections:

```cpp
// ============================================================================
// Orphan Detection Baseline
// ============================================================================

void FOliveWritePipeline::SetOrphanBaseline(const FString& GraphPath, int32 CurrentOrphanCount)
{
    if (!OrphanBaselines.Contains(GraphPath))
    {
        OrphanBaselines.Add(GraphPath, CurrentOrphanCount);
        UE_LOG(LogOliveWritePipeline, Verbose,
            TEXT("Orphan baseline set for '%s': %d"), *GraphPath, CurrentOrphanCount);
    }
}

int32 FOliveWritePipeline::GetOrphanDelta(const FString& GraphPath, int32 CurrentOrphanCount) const
{
    if (const int32* Baseline = OrphanBaselines.Find(GraphPath))
    {
        return FMath::Max(0, CurrentOrphanCount - *Baseline);
    }
    // No baseline = first check for this graph = report full count
    return CurrentOrphanCount;
}

void FOliveWritePipeline::ClearOrphanBaselines()
{
    if (OrphanBaselines.Num() > 0)
    {
        UE_LOG(LogOliveWritePipeline, Verbose,
            TEXT("Clearing orphan baselines (%d graphs tracked)"), OrphanBaselines.Num());
    }
    OrphanBaselines.Reset();
    bRunActive = false;
}
```

**Step 2:** Modify the Stage 5 orphan detection block (lines 490-532). The current code calls `DetectOrphanedExecFlows` and just logs the count. Change it to use delta logic.

The key change is in the loop body where `DetectOrphanedExecFlows` is called (two sites: ubergraph pages at line 506, function graphs at line 521). At each site, after the call:

```cpp
// Current pattern (line 506-511):
int32 OrphanCount = DetectOrphanedExecFlows(Graph, StructuralMessages);
if (OrphanCount > 0)
{
    UE_LOG(...);
}
```

Replace with:

```cpp
// Collect orphan messages into a separate array first
TArray<FOliveIRMessage> OrphanMessages;
int32 AbsoluteOrphanCount = DetectOrphanedExecFlows(Graph, OrphanMessages);

if (AbsoluteOrphanCount > 0)
{
    const FString GraphPath = FString::Printf(TEXT("%s::%s"),
        *Blueprint->GetPathName(), *GraphName);

    if (bRunActive)
    {
        // Lazy baseline: first check captures baseline, returns full count
        // Subsequent checks return only new orphans
        SetOrphanBaseline(GraphPath, AbsoluteOrphanCount);
        int32 DeltaOrphanCount = GetOrphanDelta(GraphPath, AbsoluteOrphanCount);

        if (DeltaOrphanCount > 0)
        {
            // Only append the LAST DeltaOrphanCount messages
            // (newest orphans are at the end of the array)
            int32 StartIndex = FMath::Max(0, OrphanMessages.Num() - DeltaOrphanCount);
            for (int32 i = StartIndex; i < OrphanMessages.Num(); i++)
            {
                StructuralMessages.Add(OrphanMessages[i]);
            }
        }

        UE_LOG(LogOliveWritePipeline, Log,
            TEXT("Orphan delta for '%s': %d new (absolute: %d, baseline: %d)"),
            *GraphName, DeltaOrphanCount, AbsoluteOrphanCount,
            AbsoluteOrphanCount - DeltaOrphanCount);
    }
    else
    {
        // No active run -- report everything (compile, one-off tools)
        StructuralMessages.Append(OrphanMessages);
        UE_LOG(LogOliveWritePipeline, Log,
            TEXT("Detected %d orphaned exec flow(s) in graph '%s' of Blueprint '%s'"),
            AbsoluteOrphanCount, *GraphName, *Blueprint->GetName());
    }
}
```

**IMPORTANT:** The original code appends orphan messages directly into `StructuralMessages` by passing it to `DetectOrphanedExecFlows`. The modified version collects into a local array first, then selectively appends. This requires changing the call from `DetectOrphanedExecFlows(Graph, StructuralMessages)` to `DetectOrphanedExecFlows(Graph, OrphanMessages)`.

Apply this same pattern to BOTH sites (ubergraph at ~line 506 and function graph at ~line 521).

**Step 3:** Subscribe to brain state changes. This needs to happen during startup. The write pipeline is a singleton that exists for the editor's lifetime, so it can subscribe once.

The best place is `OliveAIEditorModule.cpp` in `OnPostEngineInit()`, after the brain layer is accessible but the placement depends on when the brain is created. Looking at the code, `FOliveBrainLayer` is created by `FOliveConversationManager`. The cleanest approach is to subscribe in the write pipeline's `Execute()` method on first call, or better yet, make the write pipeline listen to a global signal.

Actually, the simplest approach: have the `FOliveConversationManager` notify the write pipeline directly when it calls `Brain->BeginRun()` and `Brain->CompleteRun()`. But this creates coupling.

**Revised approach:** Use the `OnStateChanged` delegate on `FOliveBrainLayer`. The write pipeline subscribes lazily.

Add a subscription helper to the write pipeline header (public section):

```cpp
/** Subscribe to brain state changes for orphan baseline management.
 *  Called once during module startup after brain layer is available. */
void SubscribeToBrainStateChanges(TSharedPtr<FOliveBrainLayer> BrainLayer);
```

Implementation:

```cpp
void FOliveWritePipeline::SubscribeToBrainStateChanges(TSharedPtr<FOliveBrainLayer> BrainLayer)
{
    if (!BrainLayer.IsValid())
    {
        return;
    }

    BrainLayer->OnStateChanged.AddLambda([this](EOliveBrainState OldState, EOliveBrainState NewState)
    {
        if (NewState == EOliveBrainState::WorkerActive)
        {
            // New run starting -- enable delta tracking
            ClearOrphanBaselines();
            bRunActive = true;
            UE_LOG(LogOliveWritePipeline, Verbose, TEXT("Orphan delta tracking enabled (run started)"));
        }
        else if (NewState == EOliveBrainState::Completed ||
                 NewState == EOliveBrainState::Error ||
                 NewState == EOliveBrainState::Idle)
        {
            // Run ended -- clear baselines
            ClearOrphanBaselines();
        }
    });
}
```

**Where to call it:** In `OliveAIEditorModule.cpp`, `OnPostEngineInit()`. The brain layer is owned by the `FOliveConversationManager` which is owned by `FOliveEditorChatSession`. Both are initialized in `StartupModule()` (step 2). So by `OnPostEngineInit()`, the brain is available.

The coder needs to find where the brain is exposed. Let me check...

Actually, looking at the code, the brain is a `TSharedPtr<FOliveBrainLayer>` on the conversation manager. The coder should check how it's exposed. If there's no public getter, the subscription can happen inside `FOliveConversationManager::Initialize()` or wherever the brain is created, calling `FOliveWritePipeline::Get().SubscribeToBrainStateChanges(Brain)`.

**Alternative (simpler):** Skip the delegate subscription entirely. Instead, have the conversation manager call `FOliveWritePipeline::Get().ClearOrphanBaselines()` + set `bRunActive = true` right next to its `Brain->BeginRun()` calls (lines 270 and 468), and `ClearOrphanBaselines()` next to its `Brain->CompleteRun()` calls. This is 4 one-line insertions with zero API surface.

**Decision: Use the direct-call approach.** It's simpler, avoids delegate lifetime concerns, and the conversation manager already knows about the write pipeline (it calls `Execute` through tool handlers).

Insertions in `OliveConversationManager.cpp`:

1. **Line 270** (after `Brain->BeginRun()` in `SendMessageAutonomous`):
   ```cpp
   FOliveWritePipeline::Get().ClearOrphanBaselines();
   FOliveWritePipeline::Get().bRunActive = true;
   ```
   (Requires `#include "OliveWritePipeline.h"` at the top of the file)

2. **Line 468** (after `Brain->BeginRun()` in `SendMessage`):
   ```cpp
   FOliveWritePipeline::Get().ClearOrphanBaselines();
   FOliveWritePipeline::Get().bRunActive = true;
   ```

3. At each `Brain->CompleteRun(...)` call site (lines 307, 340, 935, 941, 977, 1659), add:
   ```cpp
   FOliveWritePipeline::Get().ClearOrphanBaselines();
   ```

   Actually this is too many sites. Better approach: do it in `FOliveBrainLayer::CompleteRun()` itself. But that creates a dependency from Brain -> WritePipeline which is backwards.

   **Final decision:** Put the cleanup in ONE place: the `CompleteRun` equivalent on the write pipeline side. Since `bRunActive` is already a flag, the write pipeline can self-clear when it detects the brain is no longer active. But that's polling.

   **Simplest correct approach:** Only set `bRunActive = true` at BeginRun (2 sites). Let `ClearOrphanBaselines` be called at the NEXT BeginRun. The baselines from run N are harmless during the idle period between runs, and they get wiped when run N+1 starts. The only cost is a few KB of stale data.

   This eliminates ALL CompleteRun-site modifications. Just 2 insertions total.

### Task T3 Summary of Changes

| File | Change | Lines |
|------|--------|-------|
| `OliveWritePipeline.h` | Add 3 methods + 2 fields (T2) | +20 |
| `OliveWritePipeline.cpp` | Implement 3 methods, modify 2 orphan detection sites | +40, ~20 modified |
| `OliveConversationManager.cpp` | Add `#include`, insert `ClearOrphanBaselines()` + `bRunActive = true` at 2 `BeginRun` sites | +5 |

### Edge Cases

- **AI fixes a pre-existing orphan:** Delta goes negative, clamped to 0. Some orphans become invisible. Acceptable -- the AI deliberately fixed them, so not surfacing them is correct.
- **Multiple graphs edited in one run:** Each graph gets its own baseline entry. The map key includes Blueprint path + graph name, so no collision.
- **No active run (one-off tool calls):** `bRunActive = false`, full orphan count reported. Same as current behavior.
- **`blueprint.compile` tool:** Does NOT go through the write pipeline Stage 5 (it calls `FOliveCompileManager::Compile()` directly at line 2297). No orphan detection at all in the compile path. This means compile already reports compile errors without orphan noise. Good -- no special handling needed.
- **Plan executor Phase 5.5:** Already scoped to plan_json nodes via `NodeIdToStepId`. Completely separate code path. No changes needed.

---

## Item 3: Character Interaction Caller Reference Template

### Problem

The AI wasted 5 tool calls revising its plan for a character interaction system because it had no reference for the overlap -> store ref -> input -> validity check -> interface call pattern, and specifically didn't know the Enhanced Input vs InputKey decision tree or the tool split for EIA.

### Design

New reference template at `Content/Templates/reference/interaction_caller.json`. Follows existing template conventions observed in `interactable_patterns.json`, `component_patterns.json`, and `ue_events.json`:

- 60-120 lines total
- Descriptive not prescriptive
- No embedded plan_json examples
- Each pattern: name, description (1-2 sentences), notes (2-4 sentences)
- Teaches architecture, not tool sequences

### Task T4: Create interaction_caller.json

**File:** `Content/Templates/reference/interaction_caller.json` (new file)
**Complexity:** Content only, no C++

Template structure (6 patterns):

1. **OverlapDetection** -- Overlap sphere/box on the character to detect interactable actors in range. Store the closest valid ref for input polling.
2. **InteractableDiscovery** -- `project.search` for `UInputAction` assets to detect Enhanced Input. Check `DoesImplementInterface` or class-based filtering to identify valid targets.
3. **EnhancedInputActions** -- Preferred input system. IA and IMC are asset-based (`UInputAction`, `UInputMappingContext`). These are **separate UAsset files** created via `editor.run_python`, not Blueprint variables. Wire the event node and BeginPlay `AddMappingContext` via `plan_json`.
4. **InputKeyFallback** -- InputKey node (deprecated but functional). Simpler: no asset creation needed, everything in plan_json. Use when project has no Enhanced Input setup.
5. **ValidityAndInterfaceCall** -- `IsValid` check on stored actor ref, then interface call via `target_class`. The full chain: event fires -> check validity -> call interface function.
6. **FullCallerPattern** -- The complete architecture: BeginPlay adds input mapping, Overlap stores nearest interactable ref, Input event checks validity and calls interface. Separate concerns: detection is continuous (overlap), action is event-driven (input).

Key content principles:
- Pattern 3 explicitly states: "IA/IMC assets are created with `editor.run_python`, NOT plan_json. Event nodes and BeginPlay wiring use plan_json."
- Pattern 4 exists to show InputKey as a valid alternative, not deprecated-and-forbidden.
- Pattern 2 explicitly states: "Use `project.search` for `UInputAction` to check whether the project uses Enhanced Input."

```json
{
    "template_id": "interaction_caller",
    "template_type": "reference",
    "display_name": "Character Interaction Caller Pattern",

    "catalog_description": "Full interaction caller pattern: overlap detection, stored actor ref, input handling (Enhanced Input preferred, InputKey fallback), validity check, interface call.",
    "catalog_examples": "",

    "tags": "interaction character overlap input action enhanced inputkey interface call interact detect store ref caller player",

    "patterns": [
        {
            "name": "OverlapDetection",
            "description": "A SphereComponent (or BoxComponent) on the character detects interactable actors entering range. OnComponentBeginOverlap and OnComponentEndOverlap maintain a reference to the nearest valid interactable.",
            "notes": "The overlap component should be a child of the root. Set its radius to interaction range (typically 150-300 units). Store the overlapping actor in a variable (e.g., NearestInteractable, type Actor, as object reference). Clear the variable on EndOverlap. For multiple overlapping actors, store a TArray and pick the closest on input."
        },
        {
            "name": "InputDiscovery",
            "description": "Before choosing an input approach, check what the project already uses. Use project.search for InputAction assets (type UInputAction). If results exist, use Enhanced Input Actions. If none exist, InputKey is acceptable.",
            "notes": "Enhanced Input is the standard for UE 5.x projects and is preferred when the project already has IA/IMC assets. InputKey still works and is simpler for quick prototyping. The AI should check, not assume."
        },
        {
            "name": "EnhancedInputActions",
            "description": "Enhanced Input uses separate UAsset files: InputAction (IA) for the action definition and InputMappingContext (IMC) for key bindings. These are NOT Blueprint variables -- they are standalone assets created with editor.run_python. The Blueprint then wires an EnhancedInputAction event node (plan_json) and adds the IMC in BeginPlay (plan_json).",
            "notes": "Tool split is critical: editor.run_python creates the IA and IMC assets (Python: unreal.AssetToolsHelpers.get_asset_tools().create_asset(...)). plan_json wires the EnhancedInputAction event node referencing the IA, and wires BeginPlay to call AddMappingContext with the IMC. Do NOT try to create IA/IMC assets via plan_json -- it cannot create non-Blueprint assets."
        },
        {
            "name": "InputKeyFallback",
            "description": "InputKey nodes bind directly to a key (E, F, etc.) without external assets. Everything lives in one Blueprint, all wirable via plan_json. Simpler but less flexible -- no rebinding, no per-context priority.",
            "notes": "Use when the project has no Enhanced Input setup, or for quick prototypes. The InputKey event node has a Key property set to the desired key name. Fully expressible in plan_json: event node with key property, exec chain to validity check and interface call."
        },
        {
            "name": "ValidityCheckAndCall",
            "description": "Before calling the interface, always check IsValid on the stored actor reference. Then call the interface function via target_class (creates UK2Node_Message, no cast needed). This two-step guard prevents null-reference errors when the player presses interact with nobody in range.",
            "notes": "Pattern: input event -> get_var NearestInteractable -> IsValid -> Branch -> True: call Interact with target_class=BPI_Interactable. The IsValid check is essential because the overlap variable may be null (nobody in range) or stale (actor destroyed). Do not skip it."
        },
        {
            "name": "FullCallerArchitecture",
            "description": "The complete caller has three independent concerns wired in EventGraph: (1) BeginPlay sets up input context (AddMappingContext for EIA), (2) Overlap events maintain the NearestInteractable reference, (3) Input event triggers the validity-check-and-call chain. Each concern is its own exec chain starting from its own event node.",
            "notes": "Separation matters because each concern has different lifetimes. Input setup runs once (BeginPlay). Detection is continuous (overlap). Action is instantaneous (input press). Keeping them as separate event chains avoids spaghetti and makes each independently testable."
        }
    ]
}
```

Target: ~80 lines (within 60-120 guideline).

---

## Item 4: Input Handling Recipe

### Problem

No recipe guidance on Enhanced Input vs InputKey. The AI defaults to InputKey because it's simpler, but EIA is the UE 5.x standard.

### Design

New recipe at `Content/SystemPrompts/Knowledge/recipes/blueprint/input_handling.txt`. Follows existing recipe conventions observed in `events_and_functions.txt`, `interface_pattern.txt`, and `modify.txt`:

- TAGS line + separator
- Concise, action-oriented
- Decision tree format
- Clear about tool boundaries

### Task T5: Create input_handling.txt

**File:** `Content/SystemPrompts/Knowledge/recipes/blueprint/input_handling.txt` (new file)
**Complexity:** Content only, no C++

```
TAGS: input enhanced action key inputkey mapping context eia imc keyboard mouse gamepad interact press
---
INPUT HANDLING -- Enhanced Input vs InputKey

DECISION: Check what the project uses FIRST.
  project.search type=InputAction to find existing IA assets.
  If results: use Enhanced Input Actions (project already has the infrastructure).
  If none: InputKey is acceptable (simpler, no asset creation needed).

ENHANCED INPUT ACTIONS (preferred for UE 5.x):
  Two separate assets needed (NOT Blueprint variables):
    - InputAction (IA): defines the action (e.g., IA_Interact)
    - InputMappingContext (IMC): binds keys to actions (e.g., IMC_Default)

  Tool split -- this is critical:
    editor.run_python: create IA and IMC assets (unreal.AssetToolsHelpers, unreal.InputAction, etc.)
    plan_json: wire EnhancedInputAction event node (references the IA asset)
    plan_json: wire BeginPlay -> AddMappingContext (references the IMC asset)

  Do NOT attempt to create IA/IMC assets via plan_json or add_node.
  These are standalone UAssets, not Blueprint nodes.

INPUTKEY (fallback, simpler):
  Single node, no external assets. Key name set as property.
  Everything expressible in plan_json: event op with InputKey target.
  Fine for prototypes or projects without Enhanced Input.

COMMON PATTERN for interaction:
  1. Input event (EIA or InputKey) fires
  2. get_var stored actor reference (from overlap detection)
  3. IsValid check (actor may have left range or been destroyed)
  4. Branch on IsValid result
  5. True path: call interface function via target_class (no cast needed)
```

### Task T6: Update _manifest.json

**File:** `Content/SystemPrompts/Knowledge/recipes/_manifest.json`
**Complexity:** Trivial (add one recipe entry)

Add to the `"blueprint"` category's `"recipes"` object, after the `"events_and_functions"` entry:

```json
"input_handling": {
  "description": "Enhanced Input Actions vs InputKey decision tree, tool split for asset creation vs wiring",
  "tags": ["input", "enhanced", "action", "inputkey", "mapping", "context", "keyboard", "mouse", "interact"],
  "max_tokens": 250
}
```

---

## Task Summary

| Task | Item | File(s) | What Changes | Complexity |
|------|------|---------|--------------|------------|
| T1 | 1 | `BlueprintToolHandlers.cpp` | Fix `modified_properties_count`, add `failed_properties_count` and `requested_properties_count` | Trivial |
| T2 | 2 | `OliveWritePipeline.h` | Add orphan baseline fields + method declarations | Small |
| T3 | 2 | `OliveWritePipeline.cpp`, `OliveConversationManager.cpp` | Implement delta logic, modify orphan reporting, add run lifecycle hooks | Medium |
| T4 | 3 | `Content/Templates/reference/interaction_caller.json` | New reference template (content only) | Content |
| T5 | 4 | `Content/SystemPrompts/Knowledge/recipes/blueprint/input_handling.txt` | New recipe (content only) | Content |
| T6 | 4 | `Content/SystemPrompts/Knowledge/recipes/_manifest.json` | Add recipe entry | Trivial |

### Implementation Order

1. **T1** (trivial, independent, immediate value)
2. **T4 + T5 + T6** (content only, no build required, can be done in parallel)
3. **T2 + T3** (coupled, must be done together, medium complexity)

### Build Verification

After T1 + T2 + T3:

```bash
"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex
```

T4, T5, T6 are content-only and do not need compilation. However, verify that the recipe loads correctly by checking the editor log for:
```
LogOliveCrossSystemTools: Loaded recipe: blueprint/input_handling
```

And that the template loads:
```
LogOliveTemplates: Loaded template: interaction_caller (reference)
```

### Gotchas

1. **T1:** The `FMath::Max(0, ...)` guard is essential. Without it, the edge case of 0 requested properties produces `modified_properties_count: -1`.

2. **T3:** The `DetectOrphanedExecFlows` call sites must change from passing `StructuralMessages` directly to collecting into a local `OrphanMessages` array. If the coder misses this, orphan messages will be double-reported (once in the local array, once appended to StructuralMessages by the function).

3. **T3:** The `bRunActive` field must be public (or provide a setter) since `OliveConversationManager.cpp` needs to set it. Making it public is fine for a pipeline-internal flag. Alternatively, make `ClearOrphanBaselines` also set `bRunActive = true` via a parameter: `void ClearOrphanBaselines(bool bStartNewRun = false)`. This keeps only one method call at each BeginRun site.

4. **T3:** The `OliveConversationManager.cpp` include for `OliveWritePipeline.h` -- verify the include path works. The conversation manager is in `Source/OliveAIEditor/Private/Chat/` and the write pipeline header is in `Source/OliveAIEditor/Blueprint/Public/Pipeline/`. Since `OliveAIEditor.Build.cs` adds recursive include paths for the Blueprint sub-module, the include should be `#include "OliveWritePipeline.h"` (the Build.cs recursive paths handle it).

5. **T4:** The template MUST stay under 120 lines. The draft above is ~80 lines. If the coder adds content, they must trim elsewhere.

6. **T6:** The recipe key `"input_handling"` must match the filename `input_handling.txt` exactly (case-sensitive on some platforms).
