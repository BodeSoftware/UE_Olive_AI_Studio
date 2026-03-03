# Research: Timeline Node API (UE 5.5)

## Question
How do you programmatically create and configure Timeline nodes in UE 5.5 Blueprints — including setting timeline length, adding float/vector tracks, and setting play options?

---

## Findings

### 1. UK2Node_Timeline — What It Actually Is

`UK2Node_Timeline` is a thin **graph node wrapper**. It does NOT store the timeline data itself. Its key stored property is just `TimelineName` — a name pointing to the real data object.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Classes/K2Node_Timeline.h`

```cpp
UCLASS(MinimalAPI)
class UK2Node_Timeline : public UK2Node
{
    UPROPERTY()
    FName TimelineName;          // Variable name (e.g., "Timeline_0")

    UPROPERTY(Transient)         // TRANSIENT — mirrored from UTimelineTemplate
    uint32 bAutoPlay:1;

    UPROPERTY(Transient)         // TRANSIENT — mirrored from UTimelineTemplate
    uint32 bLoop:1;

    UPROPERTY(Transient)         // TRANSIENT
    uint32 bReplicated:1;

    UPROPERTY(Transient)         // TRANSIENT
    uint32 bIgnoreTimeDilation:1;

    UPROPERTY()
    FGuid TimelineGuid;          // Used for copy/paste identity matching
};
```

**Critical discovery:** `bAutoPlay`, `bLoop`, `bReplicated`, `bIgnoreTimeDilation` are all marked `UPROPERTY(Transient)` on the node. They are **caches**, not the source of truth. The source of truth is on `UTimelineTemplate`. This is why `set_node_property` can't set `TimelineLength` on the node — `TimelineLength` is not a property on `UK2Node_Timeline` at all.

---

### 2. Where Timeline Data Actually Lives — UTimelineTemplate

All editable timeline data lives in `UTimelineTemplate`, a `UObject` stored in `Blueprint->Timelines`.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Engine/TimelineTemplate.h`

```cpp
UCLASS(MinimalAPI)
class UTimelineTemplate : public UObject
{
    UPROPERTY(EditAnywhere, Category=TimelineTemplate)
    float TimelineLength;                          // The actual length setting

    UPROPERTY(EditAnywhere, Category=TimelineTemplate)
    TEnumAsByte<ETimelineLengthMode> LengthMode;   // TL_TimelineLength or TL_LastKeyFrame

    UPROPERTY(EditAnywhere, Category=TimelineTemplate)
    uint8 bAutoPlay:1;

    UPROPERTY(EditAnywhere, Category=TimelineTemplate)
    uint8 bLoop:1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=TimelineTemplate)
    uint8 bReplicated:1;

    UPROPERTY(EditAnywhere, Category=TimelineTemplate)
    uint8 bIgnoreTimeDilation:1;

    // Track arrays:
    TArray<FTTEventTrack>        EventTracks;
    TArray<FTTFloatTrack>        FloatTracks;
    TArray<FTTVectorTrack>       VectorTracks;
    TArray<FTTLinearColorTrack>  LinearColorTracks;

    // Display order (what AllocateDefaultPins iterates):
    TArray<FTTTrackId>           TrackDisplayOrder;  // WITH_EDITORONLY_DATA
};
```

`ETimelineLengthMode` is defined in `TimelineComponent.h`:
```cpp
enum ETimelineLengthMode : int
{
    TL_TimelineLength,   // Use TimelineLength field
    TL_LastKeyFrame      // Auto-extend to last key
};
```

`Blueprint->Timelines` is `TArray<TObjectPtr<UTimelineTemplate>>`.

`Blueprint->FindTimelineTemplateByVariableName(FName)` looks up by variable name, converting it to a template name internally via `UTimelineTemplate::TimelineVariableNameToTemplateName()`.

---

### 3. Timeline Track Types

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Engine/TimelineTemplate.h`

All tracks inherit from `FTTTrackBase` which has `ETrackType` enum:
```cpp
enum ETrackType
{
    TT_Event,          // 0 — fire an event at specific times
    TT_FloatInterp,    // 1 — float value over time
    TT_VectorInterp,   // 2 — FVector value over time
    TT_LinearColorInterp, // 3 — FLinearColor value over time
};
```

Track structs:
```cpp
struct FTTFloatTrack : public FTTPropertyTrack
{
    TObjectPtr<UCurveFloat> CurveFloat;    // The curve data
};

struct FTTVectorTrack : public FTTPropertyTrack
{
    TObjectPtr<UCurveVector> CurveVector;  // X/Y/Z curves: FloatCurves[3]
};

struct FTTLinearColorTrack : public FTTPropertyTrack
{
    TObjectPtr<UCurveLinearColor> CurveLinearColor;
};

struct FTTEventTrack : public FTTTrackBase
{
    TObjectPtr<UCurveFloat> CurveKeys;    // bIsEventCurve = true
};
```

Track name is PRIVATE (`TrackName` field on `FTTTrackBase`). Set it via:
```cpp
Track.SetTrackName(FName(TEXT("Alpha")), TimelineTemplate);
```

`SetTrackName` on `FTTPropertyTrack` also sets the internal `PropertyName` used by the compiler to generate the Timeline component property.

---

### 4. How Curve Data Is Stored

**Float track:** `UCurveFloat` has `FRichCurve FloatCurve` field.
**Vector track:** `UCurveVector` has `FRichCurve FloatCurves[3]` (index 0=X, 1=Y, 2=Z).

`FRichCurve` key manipulation API (from `RichCurve.h`):
```cpp
// Add a key at Time with Value (returns key handle):
FKeyHandle AddKey(float InTime, float InValue, bool bUnwindRotation=false, FKeyHandle KeyHandle=FKeyHandle());

// Or simpler update-or-add:
FKeyHandle UpdateOrAddKey(float InTime, float InValue, bool bUnwindRotation=false, float KeyTimeTolerance=UE_KINDA_SMALL_NUMBER);

// Typical two-keyframe linear ramp (0 to 1 over 1 second):
Curve.FloatCurve.AddKey(0.f, 0.f);
Curve.FloatCurve.AddKey(1.f, 1.f);
```

Curve objects are created as `NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public)`. The `RF_Public` flag is required so they can be referenced from timeline instances in levels.

---

### 5. FBlueprintEditorUtils Timeline Methods

Module: `UnrealEd`
Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/UnrealEd/Public/Kismet2/BlueprintEditorUtils.h`

```cpp
// Generate a unique "Timeline_0", "Timeline_1", etc. name:
static FName FBlueprintEditorUtils::FindUniqueTimelineName(const UBlueprint* Blueprint);

// Create a UTimelineTemplate and register it with the Blueprint:
static UTimelineTemplate* FBlueprintEditorUtils::AddNewTimeline(UBlueprint* Blueprint, const FName& TimelineVarName);

// Find the UK2Node_Timeline in the event graph that owns a given template:
static UK2Node_Timeline* FBlueprintEditorUtils::FindNodeForTimeline(UBlueprint* Blueprint, UTimelineTemplate* Timeline);

// Remove a timeline and its template:
static void FBlueprintEditorUtils::RemoveTimeline(UBlueprint* Blueprint, UTimelineTemplate* Timeline, bool bDontRecompile=false);
```

`AddNewTimeline` internally:
1. Calls `FBlueprintEditorUtils::DoesSupportTimelines(Blueprint)` — returns false for non-Actor blueprints and non-Normal/LevelScript types.
2. Creates `NewObject<UTimelineTemplate>(Blueprint->GeneratedClass, TimelineTemplateName, RF_Transactional)`.
3. Adds to `Blueprint->Timelines`.
4. Calls `MarkBlueprintAsStructurallyModified`.

**Timeline nodes are only valid in Actor-based Blueprints** (class inherits from AActor) with `BPTYPE_Normal` or `BPTYPE_LevelScript`. They cannot go in component BPs, function libraries, or interfaces.

---

### 6. The Full Node Creation Sequence

From `K2Node_Timeline.cpp` `GetMenuActions()` — this is what the editor does when a user picks "Add Timeline" from the right-click menu:

```cpp
// Step 1: Generate unique name
TimelineNode->TimelineName = FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint);

// Step 2: Create the template in the Blueprint
FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineNode->TimelineName);
```

`AllocateDefaultPins()` (which is called automatically when the node is placed into a graph) then calls `Blueprint->FindTimelineTemplateByVariableName(TimelineName)` and creates output pins for every track in the template's display order.

So the full programmatic sequence is:

```cpp
// 1. Wrap in transaction + Modify
FScopedTransaction Transaction(LOCTEXT("CreateTimeline", "Create Timeline"));
Blueprint->Modify();
Graph->Modify();

// 2. Generate a unique name
FName TimelineName = FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint);

// 3. Create the graph node
UK2Node_Timeline* TimelineNode = NewObject<UK2Node_Timeline>(Graph);
TimelineNode->TimelineName = TimelineName;
TimelineNode->CreateNewGuid();
TimelineNode->PostPlacedNewNode();

// 4. Create the UTimelineTemplate in the Blueprint BEFORE AllocateDefaultPins
UTimelineTemplate* TimelineTemplate = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineName);

// 5. Configure template properties BEFORE AllocateDefaultPins
TimelineTemplate->Modify();
TimelineTemplate->TimelineLength = 1.0f;
TimelineTemplate->LengthMode = TL_TimelineLength;
TimelineTemplate->bAutoPlay = false;
TimelineTemplate->bLoop = false;

// 6. Add tracks BEFORE AllocateDefaultPins (so pins get created)
{
    FTTFloatTrack NewTrack;
    NewTrack.SetTrackName(FName(TEXT("Alpha")), TimelineTemplate);
    NewTrack.CurveFloat = NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public);
    // Add keys: 0.0 at t=0, 1.0 at t=1.0
    NewTrack.CurveFloat->FloatCurve.AddKey(0.f, 0.f);
    NewTrack.CurveFloat->FloatCurve.AddKey(1.f, 1.f);
    TimelineTemplate->FloatTracks.Add(NewTrack);

    // Register display order
    FTTTrackId TrackId;
    TrackId.TrackType = FTTTrackBase::TT_FloatInterp;
    TrackId.TrackIndex = TimelineTemplate->FloatTracks.Num() - 1;
    TimelineTemplate->AddDisplayTrack(TrackId);
}

// 7. AllocateDefaultPins — now creates track output pins from the template
TimelineNode->AllocateDefaultPins();

// 8. Add the node to the graph
Graph->AddNode(TimelineNode, true, false);

// 9. Mirror settings back to node (AllocateDefaultPins already does this, but be explicit)
TimelineNode->bAutoPlay = TimelineTemplate->bAutoPlay;
TimelineNode->bLoop = TimelineTemplate->bLoop;

// 10. Mark as structurally modified
FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
```

---

### 7. Modifying an Existing Timeline Node's Properties

This is what the editor does when you change Length/AutoPlay/Loop in the Timeline editor (STimelineEditor.cpp):

**TimelineLength:**
```cpp
// Source: STimelineEditor.cpp line 1730
TimelineTemplate->Modify();
TimelineTemplate->TimelineLength = 1.0f;
FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
```

**bAutoPlay (must sync to both template AND node):**
```cpp
// Source: STimelineEditor.cpp lines 1595-1612
TimelineTemplate->Modify();
TimelineTemplate->bAutoPlay = true;
TimelineNode->bAutoPlay = true;  // sync the transient cache
FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
```

**bLoop (same pattern):**
```cpp
TimelineTemplate->bLoop = true;
TimelineNode->bLoop = true;
FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
```

**Adding a track to an existing node (after AllocateDefaultPins):**
```cpp
// Source: STimelineEditor.cpp lines 1368-1424
TimelineNode->Modify();
TimelineTemplate->Modify();

FTTFloatTrack NewTrack;
NewTrack.SetTrackName(FName(TEXT("Alpha")), TimelineTemplate);
NewTrack.CurveFloat = NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public);
NewTrack.CurveFloat->FloatCurve.AddKey(0.f, 0.f);
NewTrack.CurveFloat->FloatCurve.AddKey(1.f, 1.f);
TimelineTemplate->FloatTracks.Add(NewTrack);

FTTTrackId TrackId;
TrackId.TrackType = FTTTrackBase::TT_FloatInterp;
TrackId.TrackIndex = TimelineTemplate->FloatTracks.Num() - 1;
TimelineTemplate->AddDisplayTrack(TrackId);

// REQUIRED: reconstruct the node so new pins appear
TimelineNode->ReconstructNode();
FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
```

---

### 8. Output Pins on UK2Node_Timeline

After `AllocateDefaultPins()`, the node has these pins (in order):

**Exec inputs:**
- `Play` (PC_Exec, Input)
- `PlayFromStart` (PC_Exec, Input)
- `Stop` (PC_Exec, Input)
- `Reverse` (PC_Exec, Input)
- `ReverseFromEnd` (PC_Exec, Input)
- `SetNewTime` (PC_Exec, Input)

**Data inputs:**
- `NewTime` (PC_Real/PC_Float, Input)

**Exec outputs:**
- `Update` (PC_Exec, Output)
- `Finished` (PC_Exec, Output)
- Per event track: `TrackName` (PC_Exec, Output)

**Data outputs:**
- `Direction` (PC_Byte, Output — ETimelineDirection enum)
- Per float track: `TrackName` (PC_Real/PC_Float, Output)
- Per vector track: `TrackName` (PC_Struct/FVector, Output)
- Per color track: `TrackName` (PC_Struct/FLinearColor, Output)

Accessor methods on `UK2Node_Timeline` (all `BLUEPRINTGRAPH_API`):
```cpp
UEdGraphPin* GetPlayPin() const;
UEdGraphPin* GetPlayFromStartPin() const;
UEdGraphPin* GetStopPin() const;
UEdGraphPin* GetUpdatePin() const;
UEdGraphPin* GetReversePin() const;
UEdGraphPin* GetReverseFromEndPin() const;
UEdGraphPin* GetFinishedPin() const;
UEdGraphPin* GetNewTimePin() const;
UEdGraphPin* GetSetNewTimePin() const;
UEdGraphPin* GetDirectionPin() const;
UEdGraphPin* GetTrackPin(const FName TrackName) const;
```

---

### 9. Blueprint Compatibility Constraint

Timeline nodes are **only supported** in Blueprints where both conditions hold:
1. `Blueprint->ParentClass->IsChildOf(AActor::StaticClass())` — must be Actor-based
2. `Blueprint->BlueprintType == BPTYPE_Normal || BPTYPE_LevelScript` — not macros, interfaces, or function libraries

Source: `BlueprintEditorUtils.cpp` lines 3280–3495.

If `AddNewTimeline` is called on an incompatible Blueprint it silently returns `nullptr`. No error is reported.

---

### 10. Required Headers and Modules

| Type | Header | Module |
|------|--------|--------|
| `UK2Node_Timeline` | `K2Node_Timeline.h` | `BlueprintGraph` |
| `UTimelineTemplate` | `Engine/TimelineTemplate.h` | `Engine` |
| `FTTFloatTrack`, etc. | `Engine/TimelineTemplate.h` | `Engine` |
| `UCurveFloat` | `Curves/CurveFloat.h` | `Engine` |
| `UCurveVector` | `Curves/CurveVector.h` | `Engine` |
| `UCurveLinearColor` | `Curves/CurveLinearColor.h` | `Engine` |
| `FRichCurve` | `Curves/RichCurve.h` | `Engine` |
| `FBlueprintEditorUtils::AddNewTimeline` | `Kismet2/BlueprintEditorUtils.h` | `UnrealEd` |
| `ETimelineLengthMode` | `Components/TimelineComponent.h` | `Engine` |

---

## Recommendations

### For the coder implementing a `timeline` plan op:

1. **Never use `set_node_property` for timeline configuration.** `TimelineLength`, `bAutoPlay`, and `bLoop` do not exist as settable properties on `UK2Node_Timeline`. They live on `UTimelineTemplate`. The whole mental model of "set a property on the node" is wrong for timelines.

2. **The creation sequence is order-sensitive:**
   - Set `TimelineNode->TimelineName` first.
   - Call `FBlueprintEditorUtils::AddNewTimeline()` to create the template.
   - Populate template data (tracks, length, flags).
   - Call `AddDisplayTrack()` for each track to register display order.
   - Only then call `AllocateDefaultPins()` — it reads the template's display order to create track output pins.

3. **Track names use `SetTrackName(name, template)` not direct field assignment.** `TrackName` is private on `FTTTrackBase`. The `SetTrackName` method on `FTTPropertyTrack` also sets the internal `PropertyName` used by the compiler.

4. **Curve objects need `RF_Public`.** `NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public)`. Without `RF_Public`, the object cannot be referenced from timeline instances placed in levels.

5. **bAutoPlay and bLoop must be set on BOTH the template AND the node.** The node properties are `Transient` caches. When configuring a new node, setting them on the template before `AllocateDefaultPins` is sufficient (since `AllocateDefaultPins` copies them). When modifying an existing node post-construction, both must be updated.

6. **After adding tracks to an existing node, call `TimelineNode->ReconstructNode()`.** This rebuilds the pin list. `MarkBlueprintAsModified` alone is not enough.

7. **Use `MarkBlueprintAsStructurallyModified` for creation, `MarkBlueprintAsModified` for property-only changes.** `AddNewTimeline` already calls the structural one; track additions also require it.

8. **Timelines only work on Actor-based Blueprints.** Proactively check `FBlueprintEditorUtils::DoesSupportTimelines(Blueprint)` before attempting to create one and return a clear error if false.

9. **`GetTrackPin(FName TrackName)` retrieves a track's output pin by name.** This is the correct way for the AI to wire up a track's output (e.g., connecting Alpha to SetActorScale3D).

10. **`FindNodeForTimeline(Blueprint, Template)` finds the node from the template.** Useful if you have the template but need the graph node to call `ReconstructNode`.
