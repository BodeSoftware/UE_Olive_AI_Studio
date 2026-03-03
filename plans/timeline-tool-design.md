# Timeline Tool Design: `blueprint.create_timeline`

## Overview

A single tool that creates a fully configured Timeline node in a Blueprint event graph, complete with a `UTimelineTemplate`, tracks, curve keys, and output pins ready for wiring.

**Research basis:** `plans/research/timeline-node-api-deep.md`

---

## 1. Tool Schema

### Tool Name
`blueprint.create_timeline`

### Description
Create a Timeline node in a Blueprint event graph with tracks and curve data. Returns the node ID and complete pin manifest so the AI can wire outputs with `connect_pins` or reference via `plan_json`.

### Parameters

| Param | Type | Required | Description |
|-------|------|----------|-------------|
| `path` | string | yes | Blueprint asset path |
| `graph` | string | no | Graph name (default: `"EventGraph"`). Must be an event graph (ubergraph). |
| `timeline_name` | string | no | Timeline variable name (e.g., `"FadeTimeline"`). Auto-generated via `FindUniqueTimelineName` if omitted. |
| `length` | number | no | Timeline length in seconds (default: `5.0`). |
| `auto_play` | boolean | no | Start playing on BeginPlay (default: `false`). |
| `loop` | boolean | no | Loop when finished (default: `false`). |
| `replicated` | boolean | no | Replicate to clients (default: `false`). |
| `ignore_time_dilation` | boolean | no | Ignore global time dilation (default: `false`). |
| `tracks` | array | yes | Array of track definitions (at least one). |

### Track Definition (items of `tracks` array)

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | yes | Track name. Becomes the output pin name. |
| `type` | enum | yes | One of: `"float"`, `"vector"`, `"color"`, `"event"`. |
| `keys` | array | yes | Array of keyframes. Format depends on track type. |
| `interp` | enum | no | Interpolation mode: `"linear"` (default), `"cubic"`, `"constant"`. Applies to all keys in the track. Not applicable to event tracks. |

### Key Formats

**Float track:** `[[time, value], ...]`
```json
{ "name": "Alpha", "type": "float", "keys": [[0.0, 0.0], [1.0, 1.0], [2.0, 0.0]] }
```

**Event track:** `[[time, 0.0], ...]` (value ignored for events, conventionally 0.0)
```json
{ "name": "OnHalfway", "type": "event", "keys": [[1.0, 0.0]] }
```

**Vector track:** `[[time, x, y, z], ...]`
```json
{ "name": "Position", "type": "vector", "keys": [[0.0, 0, 0, 0], [2.0, 100, 0, 200]] }
```

**Color track:** `[[time, r, g, b, a], ...]`
```json
{ "name": "Tint", "type": "color", "keys": [[0.0, 1, 1, 1, 1], [1.0, 1, 0, 0, 1]] }
```

### Required Fields
`path`, `tracks`

---

## 2. Schema Implementation

Add `BlueprintCreateTimeline()` to `OliveBlueprintSchemas` namespace in both the header (`.h`) and implementation (`.cpp`).

### Header Declaration (OliveBlueprintSchemas.h)

Add in the Graph Writer Tool Schemas section, after `BlueprintSetNodeProperty()`:

```cpp
/**
 * Schema for blueprint.create_timeline
 * Create a Timeline node with tracks and curve data.
 * Params: {path: string, graph?: string, timeline_name?: string, length?: number,
 *          auto_play?: bool, loop?: bool, replicated?: bool, ignore_time_dilation?: bool,
 *          tracks: [{name: string, type: "float"|"vector"|"color"|"event", keys: array, interp?: string}]}
 */
TSharedPtr<FJsonObject> BlueprintCreateTimeline();
```

### Schema Builder Implementation (OliveBlueprintSchemas.cpp)

A new `NumberProp` helper is needed since only `IntProp` exists today:

```cpp
static TSharedPtr<FJsonObject> NumberProp(const FString& Description, double DefaultValue)
{
    TSharedPtr<FJsonObject> Prop = MakeSchema(TEXT("number"));
    Prop->SetStringField(TEXT("description"), Description);
    Prop->SetNumberField(TEXT("default"), DefaultValue);
    return Prop;
}
```

This helper is file-local (`static`) in the `.cpp` -- no header change needed for it.

The schema function itself builds a nested `tracks` array with a track-item object schema containing `name`, `type` (enum), `keys` (array of arrays), and optional `interp` (enum).

---

## 3. Tool Registration

In `RegisterGraphWriterTools()` inside `OliveBlueprintToolHandlers.cpp`, add after the `blueprint.set_node_property` registration block:

```cpp
// blueprint.create_timeline
Registry.RegisterTool(
    TEXT("blueprint.create_timeline"),
    TEXT("Create a Timeline node with tracks and curve data in a Blueprint event graph"),
    OliveBlueprintSchemas::BlueprintCreateTimeline(),
    FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCreateTimeline),
    {TEXT("blueprint"), TEXT("write"), TEXT("graph"), TEXT("timeline")},
    TEXT("blueprint")
);
RegisteredToolNames.Add(TEXT("blueprint.create_timeline"));
```

Update the log line count: `"Registered 7 graph writer tools"`.

### Header Declaration (OliveBlueprintToolHandlers.h)

Add in the Graph Writer Tool Handlers section (after `HandleBlueprintSetNodeProperty`):

```cpp
FOliveToolResult HandleBlueprintCreateTimeline(const TSharedPtr<FJsonObject>& Params);
```

---

## 4. Handler Implementation

### 4.1 Param Parsing and Pre-Pipeline Validation

The handler follows the standard pattern: parse params, validate cheaply, load Blueprint, build `FOliveWriteRequest`, bind executor lambda, call `ExecuteWithOptionalConfirmation`.

```
HandleBlueprintCreateTimeline(Params):
  1. Parse required: path, tracks array
  2. Parse optional: graph (default "EventGraph"), timeline_name, length, auto_play, loop, replicated, ignore_time_dilation
  3. Validate tracks array: at least 1 track, each track has name + type + keys
  4. Validate track types: must be "float", "vector", "color", or "event"
  5. Validate key shapes per type: float=[2], vector=[4], color=[5], event=[2]
  6. Validate no duplicate track names within the request
  7. Load Blueprint via LoadObject<UBlueprint>
  8. Pre-pipeline check: DoesSupportTimelines(Blueprint)
  9. Pre-pipeline check: if timeline_name provided, check for duplicates via FindTimelineTemplateByVariableName
  10. Build FOliveWriteRequest (OperationCategory = "graph_editing", bAutoCompile = true)
  11. Bind executor lambda, execute through pipeline
```

### 4.2 Executor Lambda (Inside Pipeline Transaction)

This is the critical code path. The executor lambda receives the Blueprint as `Target`.

```
Executor Lambda:
  1. Cast Target to UBlueprint*
  2. Find the target event graph by name
     - Search UbergraphPages by graph name
     - Validate it is a ubergraph (GT_Ubergraph)
  3. Generate timeline name if not provided:
     FName TLName = UserProvidedName.IsEmpty()
       ? FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint)
       : FName(*UserProvidedName);
  4. Create the K2Node_Timeline (NewObject on Graph)
     UK2Node_Timeline* TimelineNode = NewObject<UK2Node_Timeline>(Graph);
     TimelineNode->TimelineName = TLName;
     TimelineNode->CreateNewGuid();
  5. Create template via AddNewTimeline
     UTimelineTemplate* Template = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TLName);
     if (!Template) -> return ExecutionError("TIMELINE_CREATE_FAILED", ...)
  6. Configure template properties
     Template->Modify();
     Template->TimelineLength = Length;
     Template->bAutoPlay = bAutoPlay;
     Template->bLoop = bLoop;
     Template->bReplicated = bReplicated;
     Template->bIgnoreTimeDilation = bIgnoreTimeDilation;
  7. Add tracks (for each track in parsed tracks array):
     UClass* OwnerClass = Blueprint->GeneratedClass;
     -- See Section 4.3 for per-type track creation
  8. AllocateDefaultPins (reads template to create track output pins)
     TimelineNode->AllocateDefaultPins();
  9. Add node to graph
     Graph->AddNode(TimelineNode, false, false);
  10. Position the node (auto-layout: PosX=0, PosY=0 or user-provided)
  11. Cache node in GraphWriter for subsequent connect_pins calls
      FOliveGraphWriter& GW = FOliveGraphWriter::Get();
      FString NodeId = GW.GenerateNodeId(AssetPath);  -- PRIVATE, see 4.5
      GW.CacheNode(AssetPath, NodeId, TimelineNode);
  12. Build result data (see Section 5)
```

### 4.3 Track Creation Per Type

For each track, the sequence is: create track struct, call `SetTrackName(FName, Template)`, create curve `NewObject` on `Blueprint->GeneratedClass` with `RF_Public`, populate keys, append to template array, register display order.

**Float track:**
```cpp
FTTFloatTrack NewTrack;
NewTrack.SetTrackName(FName(*TrackName), Template);
NewTrack.CurveFloat = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
for (auto& Key : Keys) {
    FKeyHandle H = NewTrack.CurveFloat->FloatCurve.AddKey(Key.Time, Key.Value);
    if (InterpMode != RCIM_Linear)
        NewTrack.CurveFloat->FloatCurve.SetKeyInterpMode(H, InterpMode);
}
int32 Idx = Template->FloatTracks.Num();
Template->FloatTracks.Add(NewTrack);
FTTTrackId TrackId; TrackId.TrackType = FTTTrackBase::TT_FloatInterp; TrackId.TrackIndex = Idx;
Template->AddDisplayTrack(TrackId);
```

**Event track:**
```cpp
FTTEventTrack NewTrack;
NewTrack.SetTrackName(FName(*TrackName), Template);
NewTrack.CurveKeys = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
NewTrack.CurveKeys->bIsEventCurve = true;
for (auto& Key : Keys) {
    NewTrack.CurveKeys->FloatCurve.AddKey(Key.Time, 0.0f);
}
int32 Idx = Template->EventTracks.Num();
Template->EventTracks.Add(NewTrack);
FTTTrackId TrackId; TrackId.TrackType = FTTTrackBase::TT_Event; TrackId.TrackIndex = Idx;
Template->AddDisplayTrack(TrackId);
```

**Vector track:**
```cpp
FTTVectorTrack NewTrack;
NewTrack.SetTrackName(FName(*TrackName), Template);
NewTrack.CurveVector = NewObject<UCurveVector>(OwnerClass, NAME_None, RF_Public);
for (auto& Key : Keys) {
    NewTrack.CurveVector->FloatCurves[0].AddKey(Key.Time, Key.X);
    NewTrack.CurveVector->FloatCurves[1].AddKey(Key.Time, Key.Y);
    NewTrack.CurveVector->FloatCurves[2].AddKey(Key.Time, Key.Z);
}
int32 Idx = Template->VectorTracks.Num();
Template->VectorTracks.Add(NewTrack);
FTTTrackId TrackId; TrackId.TrackType = FTTTrackBase::TT_VectorInterp; TrackId.TrackIndex = Idx;
Template->AddDisplayTrack(TrackId);
```

**Color track:**
```cpp
FTTLinearColorTrack NewTrack;
NewTrack.SetTrackName(FName(*TrackName), Template);
NewTrack.CurveLinearColor = NewObject<UCurveLinearColor>(OwnerClass, NAME_None, RF_Public);
for (auto& Key : Keys) {
    NewTrack.CurveLinearColor->FloatCurves[0].AddKey(Key.Time, Key.R);
    NewTrack.CurveLinearColor->FloatCurves[1].AddKey(Key.Time, Key.G);
    NewTrack.CurveLinearColor->FloatCurves[2].AddKey(Key.Time, Key.B);
    NewTrack.CurveLinearColor->FloatCurves[3].AddKey(Key.Time, Key.A);
}
int32 Idx = Template->LinearColorTracks.Num();
Template->LinearColorTracks.Add(NewTrack);
FTTTrackId TrackId; TrackId.TrackType = FTTTrackBase::TT_LinearColorInterp; TrackId.TrackIndex = Idx;
Template->AddDisplayTrack(TrackId);
```

### 4.4 Interpolation Mode Mapping

Map user-provided string to `ERichCurveInterpMode`:

| Input string | Enum value | Notes |
|---|---|---|
| `"linear"` (default) | `RCIM_Linear` | Default for `FRichCurve::AddKey` -- no `SetKeyInterpMode` needed |
| `"cubic"` | `RCIM_Cubic` | Smooth tangent-based curves |
| `"constant"` | `RCIM_Constant` | Step function (hold value) |

Event tracks ignore the `interp` parameter (event timing is binary).

### 4.5 Node ID Generation and Caching

The handler needs to register the created node in `FOliveGraphWriter`'s cache so that subsequent `connect_pins` and `get_node_pins` calls can reference it by ID.

`GenerateNodeId` and `CacheNode` are currently private on `FOliveGraphWriter`. Two options:

**Option A (Recommended): Call `GraphWriter.CacheExternalNode(AssetPath, TimelineNode)` -- new public method.** This method generates an ID, caches, and returns the ID. It is a thin wrapper around `GenerateNodeId + CacheNode`. This keeps the cache internals private while giving tool handlers a clean entry point for nodes created outside `AddNode`.

**Option B: Make `GenerateNodeId` and `CacheNode` public.** Simpler but exposes cache internals.

Go with **Option A**. Add to `FOliveGraphWriter`:

```cpp
/**
 * Cache an externally-created node and return a generated node ID.
 * Used by tool handlers that create nodes directly (e.g., create_timeline)
 * rather than through GraphWriter.AddNode().
 * @param BlueprintPath Full asset path
 * @param Node The node to cache
 * @return Generated node ID for subsequent tool calls
 */
FString CacheExternalNode(const FString& BlueprintPath, UEdGraphNode* Node);
```

Implementation:
```cpp
FString FOliveGraphWriter::CacheExternalNode(const FString& BlueprintPath, UEdGraphNode* Node)
{
    FString NodeId = GenerateNodeId(BlueprintPath);
    CacheNode(BlueprintPath, NodeId, Node);
    return NodeId;
}
```

---

## 5. Response Format

The tool returns a JSON object with the created node's details and complete pin manifest. This is critical -- the AI must know all pin names to wire the timeline to downstream nodes.

```json
{
  "success": true,
  "data": {
    "asset_path": "/Game/Blueprints/BP_Door",
    "graph": "EventGraph",
    "node_id": "node_42",
    "timeline_name": "DoorTimeline",
    "message": "Created timeline 'DoorTimeline' with 2 tracks (1 float, 1 event)",
    "tracks_created": [
      {"name": "Alpha", "type": "float", "keys": 3, "pin_name": "Alpha"},
      {"name": "OnFinish", "type": "event", "keys": 1, "pin_name": "OnFinish"}
    ],
    "template_properties": {
      "length": 2.0,
      "auto_play": false,
      "loop": false
    },
    "pins": {
      "inputs": [
        {"name": "Play", "direction": "input", "category": "exec", "is_exec": true},
        {"name": "PlayFromStart", "direction": "input", "category": "exec", "is_exec": true},
        {"name": "Stop", "direction": "input", "category": "exec", "is_exec": true},
        {"name": "Reverse", "direction": "input", "category": "exec", "is_exec": true},
        {"name": "ReverseFromEnd", "direction": "input", "category": "exec", "is_exec": true},
        {"name": "SetNewTime", "direction": "input", "category": "exec", "is_exec": true},
        {"name": "NewTime", "direction": "input", "category": "real", "is_exec": false}
      ],
      "outputs": [
        {"name": "Update", "direction": "output", "category": "exec", "is_exec": true},
        {"name": "Finished", "direction": "output", "category": "exec", "is_exec": true},
        {"name": "Direction", "direction": "output", "category": "byte", "is_exec": false},
        {"name": "Alpha", "direction": "output", "category": "real", "is_exec": false},
        {"name": "OnFinish", "direction": "output", "category": "exec", "is_exec": true}
      ]
    }
  }
}
```

The `pins` object is built by the existing `BuildPinManifest()` anonymous-namespace helper, same as `add_node` uses. The `tracks_created` array is supplementary metadata that helps the AI understand which pins correspond to which tracks.

---

## 6. Error Handling

### Error Codes

| Code | When | Suggestion |
|------|------|------------|
| `VALIDATION_MISSING_PARAM` | Missing `path` or `tracks` | Standard param error |
| `VALIDATION_INVALID_TRACKS` | `tracks` array is empty, or a track is missing `name`/`type`/`keys` | "Each track needs name, type (float/vector/color/event), and keys" |
| `VALIDATION_INVALID_KEY_FORMAT` | Key array has wrong element count for its type | "Float keys: [time, value]. Vector keys: [time, x, y, z]. Color keys: [time, r, g, b, a]" |
| `VALIDATION_DUPLICATE_TRACK_NAME` | Two tracks in the request have the same name | "Track names must be unique within a timeline" |
| `ASSET_NOT_FOUND` | Blueprint does not exist | Standard |
| `TIMELINE_NOT_SUPPORTED` | Blueprint is not Actor-based or does not support event graphs | "Timelines only work in Actor-based Blueprints (not Widget BPs, Component BPs, Interfaces, etc.)" |
| `TIMELINE_GRAPH_NOT_FOUND` | Specified graph not found or is not an event graph | "Timelines can only be placed in event graphs (UbergraphPages), not function or macro graphs" |
| `TIMELINE_DUPLICATE_NAME` | A timeline with this name already exists in the Blueprint | "Choose a different name or omit timeline_name for auto-generation" |
| `TIMELINE_CREATE_FAILED` | `AddNewTimeline` returned nullptr for an unexpected reason | "Internal error creating timeline template. Compile the Blueprint and retry." |

### Pre-Pipeline Checks (Before Transaction)

These checks are cheap and avoid opening a transaction just to reject:

1. `tracks` array non-empty, valid structure
2. Track names unique
3. Key shapes correct per type
4. Blueprint loads successfully
5. `FBlueprintEditorUtils::DoesSupportTimelines(Blueprint)` -- rejects non-Actor BPs early
6. If `timeline_name` provided: `Blueprint->FindTimelineTemplateByVariableName(TLName) == nullptr`
7. Graph exists in `Blueprint->UbergraphPages` and schema type is `GT_Ubergraph`

### Inside Executor Lambda

If `AddNewTimeline` returns nullptr despite passing pre-checks, return `TIMELINE_CREATE_FAILED`. This is defensive -- it should not happen after pre-validation passes.

---

## 7. Headers Required

The handler implementation (`OliveBlueprintToolHandlers.cpp`) needs these additional includes:

```cpp
#include "K2Node_Timeline.h"                    // UK2Node_Timeline
#include "Engine/TimelineTemplate.h"            // UTimelineTemplate, FTTFloatTrack, etc.
#include "Curves/CurveFloat.h"                  // UCurveFloat
#include "Curves/CurveVector.h"                 // UCurveVector
#include "Curves/CurveLinearColor.h"            // UCurveLinearColor
#include "Components/TimelineComponent.h"       // ETimelineLengthMode
```

All modules (`Engine`, `BlueprintGraph`, `UnrealEd`) are already in `OliveAIEditor.Build.cs`. No Build.cs changes needed.

Check which of these are already included in the file. `Kismet2/BlueprintEditorUtils.h` is certainly already included (used by many handlers). `K2Node_Timeline.h` likely is not and must be added.

`Curves/RichCurve.h` is NOT needed as a direct include -- `FRichCurve::AddKey` and `SetKeyInterpMode` are accessible via the `CurveFloat.h` / `CurveVector.h` / `CurveLinearColor.h` includes which include `RichCurve.h` transitively. `ERichCurveInterpMode` is in `Curves/RealCurve.h` which is also transitive.

---

## 8. Plan JSON Op Assessment

### Should we add a `timeline` op to `OlivePlanOps`?

**Recommendation: Not now.** Defer to a follow-up if usage patterns show demand.

**Reasons against (for now):**

1. **Complexity mismatch.** Every existing plan op maps 1:1 to a single node. A `timeline` op creates 1 node + 1 template + N curve objects. The executor's `PhaseCreateNodes` loop assumes one node per step.

2. **Track data is structured.** Plan ops use flat `inputs` / `properties` string maps. Tracks require nested arrays-of-arrays (key data). Fitting this into the plan op schema would require special-case JSON parsing in `BlueprintPlanIR.cpp`.

3. **The tool already covers the use case.** The AI calls `blueprint.create_timeline` once, gets back pin names, and then either calls `connect_pins` or references the timeline node in a subsequent `plan_json` plan via `@node_42.Alpha` syntax.

4. **Wiring in plan_json already works.** After creating a timeline via the tool, a plan_json can reference its outputs by node ID. The existing `@step.pin_name` reference syntax handles this if the AI creates the timeline first, then builds the rest of the graph in a plan.

**If we add it later**, the op shape would be:
```json
{"op": "timeline", "id": "tl_1", "timeline_name": "Fade", "length": 2.0,
 "auto_play": true, "tracks": [...]}
```
And `PhaseCreateNodes` would need a special branch that delegates to the same creation logic this tool uses (factored into a shared helper).

---

## 9. File Changes Summary

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h` | Add `BlueprintCreateTimeline()` declaration |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` | Add `NumberProp()` static helper + `BlueprintCreateTimeline()` implementation |
| `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h` | Add `HandleBlueprintCreateTimeline()` declaration |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Add handler implementation + registration in `RegisterGraphWriterTools()` |
| `Source/OliveAIEditor/Blueprint/Public/Writer/OliveGraphWriter.h` | Add `CacheExternalNode()` public method |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp` | Implement `CacheExternalNode()` |

**No new files.** The tool lives entirely within existing files following established patterns.

---

## 10. Confirmation Tier

`OperationCategory = "graph_editing"` -- same as `add_node`. This maps to Tier 2 (plan-confirm) for the built-in chat, and auto-execute for MCP clients.

`bAutoCompile = true` because timeline creation is a structural change. The compile in Stage 5 will verify the template is correctly formed.

---

## 11. Node Factory Integration

**Decision: Do NOT add a `CreateTimelineNode()` to `OliveNodeFactory`.**

Rationale:
- Timeline creation requires coordinated creation of TWO objects (node + template + curves). The NodeFactory pattern is designed for single-node creation.
- The `add_node` generic path with `K2Node_Timeline` would create a broken node (no template). It is better to have a dedicated tool that handles the full lifecycle.
- Adding the tool directly in the handler follows the same pattern as `HandleBlueprintCreateInterface` -- complex creation that exceeds what a simple node creator can handle.

**However**, add `"Timeline"` to `OliveNodeTypes` namespace for documentation completeness and potential future use:

```cpp
// In OliveNodeFactory.h, OliveNodeTypes namespace:
const FString Timeline = TEXT("Timeline");
```

This constant is informational. It is NOT registered in `NodeCreators` and `add_node` with `type: "Timeline"` will NOT work (the universal fallback would create a broken node without a template). The `create_timeline` tool is the only correct path.

**Optionally**, register a NodeCreator that returns `nullptr` with a helpful error directing to `create_timeline`:
```cpp
NodeCreators.Add(OliveNodeTypes::Timeline, [](UBlueprint*, UEdGraph*, const TMap<FString, FString>&) -> UEdGraphNode* {
    // Intentional nullptr -- timeline nodes require template creation.
    // Direct the AI to blueprint.create_timeline instead.
    return nullptr;
});
```
This prevents the universal fallback from creating ghost timeline nodes. The NodeFactory error message would say: *"Timeline nodes cannot be created via add_node. Use blueprint.create_timeline instead."* -- set via `LastError` before returning nullptr.

---

## 12. Edge Cases

### 1. Blueprint has no GeneratedClass yet
`AddNewTimeline` calls `check(nullptr != Blueprint->GeneratedClass)`. This will crash on Blueprints that have never been compiled.

**Mitigation:** Pre-check in the handler: if `Blueprint->GeneratedClass == nullptr`, force a compile first via `FOliveCompileManager::Get().Compile(Blueprint)`, then re-check. If still null, return `TIMELINE_CREATE_FAILED` with suggestion "Compile the Blueprint first."

### 2. Very long timeline names
`FBlueprintEditorUtils::FindUniqueTimelineName` generates names like `Timeline_0`, `Timeline_1`. User-provided names pass through `SetTrackName` which sanitizes for C++ identifier validity.

**Mitigation:** None needed beyond the existing UE sanitization. The template's `UpdateCachedNames()` handles all naming.

### 3. Tracks with same name as fixed pins
If a user names a track `"Update"` or `"Finished"`, the resulting pin name would conflict with the built-in pins. `AllocateDefaultPins` creates fixed pins first, then track pins. Two pins with the same name on the same node would cause wiring ambiguity.

**Mitigation:** Pre-validate track names against the reserved set: `Play`, `PlayFromStart`, `Stop`, `Reverse`, `ReverseFromEnd`, `Update`, `Finished`, `SetNewTime`, `NewTime`, `Direction`. Return `VALIDATION_RESERVED_TRACK_NAME` error.

### 4. Empty keys array
A track with zero keys is technically valid (empty curve). The pin is created, and the timeline plays but the output value stays at default. This is a valid use case (user adds keys later via the editor).

**Decision:** Allow empty keys but emit a warning in the result: `"Track 'Alpha' has no keys -- output will be constant 0."` Do not reject.

### 5. Keys not in time order
`FRichCurve::AddKey` inserts in sorted order regardless of input order. No special handling needed.

### 6. Undo/Redo
The write pipeline wraps everything in `FScopedTransaction` (Stage 3). `AddNewTimeline` marks the Blueprint with `RF_Transactional` on the template. Curve objects are created with `RF_Public` (not `RF_Transactional`), which is correct -- they live inside the GeneratedClass. The transaction system tracks `Blueprint->Modify()` + `Graph->Modify()` + `Template->Modify()`. Undo will reverse all of these, including removing the template from `Blueprint->Timelines`. The node's `DestroyNode()` also cleans up the template (defense-in-depth).

---

## 13. Implementation Order

The coder should implement in this sequence:

### Task 1: GraphWriter.CacheExternalNode() (~10 min)
- Add `CacheExternalNode()` declaration to `OliveGraphWriter.h`
- Implement in `OliveGraphWriter.cpp` (3 lines wrapping `GenerateNodeId + CacheNode`)
- This unblocks Task 4

### Task 2: Schema (~15 min)
- Add `NumberProp` static helper in `OliveBlueprintSchemas.cpp`
- Implement `BlueprintCreateTimeline()` schema function
- Add declaration to `OliveBlueprintSchemas.h`

### Task 3: OliveNodeTypes::Timeline + NodeCreator guard (~5 min)
- Add `Timeline` constant to `OliveNodeTypes` namespace in `OliveNodeFactory.h`
- Add guard creator in `InitializeNodeCreators()` that returns nullptr with a helpful error

### Task 4: Handler implementation (~45 min)
- Add handler declaration to `OliveBlueprintToolHandlers.h`
- Add includes in `OliveBlueprintToolHandlers.cpp` (K2Node_Timeline, TimelineTemplate, Curve*)
- Implement `HandleBlueprintCreateTimeline`:
  - Param parsing with validation
  - Pre-pipeline checks (DoesSupportTimelines, duplicate name, reserved track names, graph check)
  - Executor lambda with full creation sequence
  - Result building with `BuildPinManifest` + tracks_created array
- Register in `RegisterGraphWriterTools()`

### Task 5: Build and test (~10 min)
- Build the plugin
- Manual test: Create a timeline with 1 float track + 1 event track in an Actor BP
- Verify pin manifest in the response
- Verify undo works
- Verify compile succeeds

---

## 14. Usage Example

Typical AI workflow for a door that opens over 1 second:

```
1. blueprint.create_timeline({
     path: "/Game/BP_Door",
     timeline_name: "DoorTimeline",
     length: 1.0,
     tracks: [{name: "DoorAlpha", type: "float", keys: [[0.0, 0.0], [1.0, 1.0]]}]
   })
   --> Returns node_id: "node_5", pins include "DoorAlpha" output, "PlayFromStart" input

2. blueprint.apply_plan_json({
     asset_path: "/Game/BP_Door",
     plan_json: {
       steps: [
         {id: "interact", op: "event", target: "Interact"},
         {id: "lerp", op: "call", target: "SetRelativeRotation",
          inputs: {NewRotation: "@make_rot.ReturnValue"},
          exec_after: "@interact"},
         {id: "make_rot", op: "call", target: "MakeRotator",
          inputs: {Yaw: "@node_5.DoorAlpha"}}
       ]
     }
   })

3. blueprint.connect_pins({
     path: "/Game/BP_Door",
     graph: "EventGraph",
     source: "interact.then",
     target: "node_5.PlayFromStart"
   })
```

The key insight: `@node_5.DoorAlpha` works in plan_json because the plan resolver resolves `@nodeId.pinName` references against the GraphWriter cache. The timeline node created in step 1 is already cached.

---

## 15. What This Does NOT Cover

- **Modifying existing timelines.** This tool creates new timelines only. Modifying tracks or keys on an existing timeline would be a separate `blueprint.modify_timeline` tool (future work).
- **Deleting timelines.** The existing `blueprint.remove_node` handles this -- `UK2Node_Timeline::DestroyNode()` cleans up the template automatically.
- **Reading timeline data.** The existing `blueprint.read` with `section: "graph"` already serializes timeline nodes via `SerializeTimeline` in the NodeSerializer.
- **External curve references.** The `bIsExternalCurve` flag on tracks allows referencing standalone curve assets. This is an advanced feature not needed for initial release.
