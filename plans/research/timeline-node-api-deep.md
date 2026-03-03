# Research: Timeline Node API — Deep Dive (UE 5.5)

## Question
Exhaustive research on programmatic creation and configuration of Timeline nodes in Blueprints, covering exact code paths from UE 5.5 engine source, pin structure, the editor's own creation path, modification after creation, edge cases, and compilation implications.

---

## Findings

### 1. UK2Node_Timeline — Complete Header

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Classes/K2Node_Timeline.h`

```cpp
UCLASS(MinimalAPI)
class UK2Node_Timeline : public UK2Node
{
    GENERATED_UCLASS_BODY()

    // The name of the timeline variable (e.g. "Timeline_0")
    UPROPERTY()
    FName TimelineName;

    // ALL of these are Transient caches — source of truth is on UTimelineTemplate
    UPROPERTY(Transient)
    uint32 bAutoPlay:1;

    UPROPERTY()
    FGuid TimelineGuid;  // Used for copy/paste matching

    UPROPERTY(Transient)
    uint32 bLoop:1;

    UPROPERTY(Transient)
    uint32 bReplicated:1;

    UPROPERTY(Transient)
    uint32 bIgnoreTimeDilation:1;

    // --- UEdGraphNode overrides ---
    virtual void AllocateDefaultPins() override;
    virtual void DestroyNode() override;
    virtual void PostPasteNode() override;
    virtual bool IsCompatibleWithGraph(const UEdGraph* TargetGraph) const override;
    virtual void OnRenameNode(const FString& NewName) override;
    virtual UObject* GetJumpTargetForDoubleClick() const override;

    // --- UK2Node overrides ---
    virtual bool NodeCausesStructuralBlueprintChange() const override { return true; }
    virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;

    // --- Pin accessors ---
    BLUEPRINTGRAPH_API UEdGraphPin* GetPlayPin() const;
    BLUEPRINTGRAPH_API UEdGraphPin* GetPlayFromStartPin() const;
    BLUEPRINTGRAPH_API UEdGraphPin* GetStopPin() const;
    BLUEPRINTGRAPH_API UEdGraphPin* GetUpdatePin() const;
    BLUEPRINTGRAPH_API UEdGraphPin* GetReversePin() const;
    BLUEPRINTGRAPH_API UEdGraphPin* GetReverseFromEndPin() const;
    BLUEPRINTGRAPH_API UEdGraphPin* GetFinishedPin() const;
    BLUEPRINTGRAPH_API UEdGraphPin* GetNewTimePin() const;
    BLUEPRINTGRAPH_API UEdGraphPin* GetSetNewTimePin() const;
    BLUEPRINTGRAPH_API UEdGraphPin* GetDirectionPin() const;
    BLUEPRINTGRAPH_API UEdGraphPin* GetTrackPin(const FName TrackName) const;

    BLUEPRINTGRAPH_API bool RenameTimeline(const FString& NewName);

private:
    void ExpandForPin(UEdGraphPin* TimelinePin, const FName PropertyName,
                      FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph);
};
```

**IMPORTANT NOTE on `GetTrackPin`:** This method is declared with `BLUEPRINTGRAPH_API` but has NO implementation in `K2Node_Timeline.cpp` (verified — the .cpp is 760 lines, no `GetTrackPin` definition exists). Use `TimelineNode->FindPin(TrackName)` instead for reliable track pin access. The declared-but-not-implemented function would cause a linker error if called.

---

### 2. UTimelineTemplate — Complete Header

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Engine/TimelineTemplate.h`

```cpp
UCLASS(MinimalAPI)
class UTimelineTemplate : public UObject
{
    GENERATED_UCLASS_BODY()

    // --- Editable properties ---
    UPROPERTY(EditAnywhere, Category=TimelineTemplate)
    float TimelineLength;                          // Default: 5.0f (set in constructor)

    UPROPERTY(EditAnywhere, Category=TimelineTemplate)
    TEnumAsByte<ETimelineLengthMode> LengthMode;   // NOT set in constructor (zero-initialized = TL_TimelineLength)

    UPROPERTY(EditAnywhere, Category=TimelineTemplate)
    uint8 bAutoPlay:1;

    UPROPERTY(EditAnywhere, Category=TimelineTemplate)
    uint8 bLoop:1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category=TimelineTemplate)
    uint8 bReplicated:1;                            // Default: false (set in constructor)

    UPROPERTY(EditAnywhere, Category=TimelineTemplate)
    uint8 bIgnoreTimeDilation:1;

    // --- Track storage ---
    UPROPERTY()
    TArray<FTTEventTrack> EventTracks;

    UPROPERTY()
    TArray<FTTFloatTrack> FloatTracks;

    UPROPERTY()
    TArray<FTTVectorTrack> VectorTracks;

    UPROPERTY()
    TArray<FTTLinearColorTrack> LinearColorTracks;

    UPROPERTY()
    TArray<FBPVariableMetaDataEntry> MetaDataArray;

    UPROPERTY(duplicatetransient)
    FGuid TimelineGuid;                            // Auto-assigned in PostInitProperties

    UPROPERTY()
    TEnumAsByte<ETickingGroup> TimelineTickGroup;

    // --- Find track by name ---
    int32 FindFloatTrackIndex(FName FloatTrackName) const;
    int32 FindVectorTrackIndex(FName VectorTrackName) const;
    int32 FindEventTrackIndex(FName EventTrackName) const;
    int32 FindLinearColorTrackIndex(FName ColorTrackName) const;
    ENGINE_API bool IsNewTrackNameValid(FName NewTrackName) const;

    // --- Name helpers ---
    FName GetUpdateFunctionName() const;
    FName GetFinishedFunctionName() const;
    FName GetVariableName() const;
    FName GetDirectionPropertyName() const;
    ENGINE_API static FString TimelineVariableNameToTemplateName(FName Name);  // appends "_Template"

    // --- Display order (WITH_EDITORONLY_DATA) ---
    ENGINE_API FTTTrackId GetDisplayTrackId(int32 DisplayTrackIndex);
    ENGINE_API int32 GetNumDisplayTracks() const;
    ENGINE_API void AddDisplayTrack(FTTTrackId NewTrackId);
    ENGINE_API void RemoveDisplayTrack(int32 DisplayTrackIndex);
    ENGINE_API void MoveDisplayTrack(int32 DisplayTrackIndex, int32 DirectionDelta);

    static const FString TemplatePostfix;  // == "_Template"

private:
    ENGINE_API void UpdateCachedNames();  // Rebuilds VariableName, DirectionPropertyName,
                                          // UpdateFunctionName, FinishedFunctionName from object name
    UPROPERTY()
    FName VariableName;

    UPROPERTY()
    FName DirectionPropertyName;

    UPROPERTY()
    FName UpdateFunctionName;       // "{TimelineName}__UpdateFunc"

    UPROPERTY()
    FName FinishedFunctionName;     // "{TimelineName}__FinishedFunc"

#if WITH_EDITORONLY_DATA
    UPROPERTY()
    TArray<FTTTrackId> TrackDisplayOrder;
#endif
};
```

**Constructor behavior:**
```cpp
UTimelineTemplate::UTimelineTemplate(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    TimelineLength = 5.0f;   // Only TimelineLength and bReplicated are explicitly set
    bReplicated = false;
}
```

**PostInitProperties behavior:**
```cpp
void UTimelineTemplate::PostInitProperties()
{
    Super::PostInitProperties();
    if (HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad) == false)
    {
        TimelineGuid = FGuid::NewGuid();    // Auto-generates GUID
        UpdateCachedNames();                 // Builds cached FNames from object name
    }
}
```

This means `TimelineGuid` is auto-assigned on construction, and `UpdateCachedNames()` derives all function/property names from the template's UObject name. You do NOT need to set these manually.

---

### 3. ETimelineLengthMode

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Components/TimelineComponent.h`

```cpp
enum ETimelineLengthMode : int
{
    TL_TimelineLength,   // 0 — Use the TimelineLength field value
    TL_LastKeyFrame      // 1 — Auto-extend to the time of the last key in any track
};
```

Default for `UTimelineTemplate::LengthMode` is `TL_TimelineLength` (zero-initialized, since the constructor does not set it).

Default for `FTimeline::LengthMode` (runtime component) is `TL_LastKeyFrame` (line 275 of TimelineComponent.h).

---

### 4. Track Struct Hierarchy — Complete Details

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Engine/TimelineTemplate.h` and `TimelineTemplate.cpp`

#### FTTTrackBase (base)

```cpp
USTRUCT()
struct FTTTrackBase
{
    enum ETrackType { TT_Event = 0, TT_FloatInterp = 1, TT_VectorInterp = 2, TT_LinearColorInterp = 3 };

private:
    UPROPERTY()
    FName TrackName;            // PRIVATE — must use SetTrackName()

public:
    UPROPERTY()
    bool bIsExternalCurve;      // Default: false

#if WITH_EDITORONLY_DATA
    UPROPERTY()
    bool bIsExpanded;           // Default: true
    UPROPERTY()
    bool bIsCurveViewSynchronized;  // Default: true
#endif

    FName GetTrackName() const { return TrackName; }
    ENGINE_API virtual void SetTrackName(FName NewTrackName, UTimelineTemplate* OwningTimeline);
};
```

#### FTTPropertyTrack (base for float, vector, color)

```cpp
USTRUCT()
struct FTTPropertyTrack : public FTTTrackBase
{
    FName GetPropertyName() const { return PropertyName; }
    ENGINE_API virtual void SetTrackName(FName NewTrackName, UTimelineTemplate* OwningTimeline) override final;

private:
    UPROPERTY()
    FName PropertyName;     // Auto-generated: "{TimelineName}_{TrackName}_{TimelineGuid}"
};
```

`FTTPropertyTrack::SetTrackName` auto-builds the `PropertyName` from the timeline variable name, track name, and GUID. It sanitizes the string to be valid as a C++ identifier (A-Z, a-z, 0-9, _ only). This is the property the compiled class will have.

#### FTTEventTrack

```cpp
USTRUCT()
struct FTTEventTrack : public FTTTrackBase    // NOTE: inherits FTTTrackBase, NOT FTTPropertyTrack
{
private:
    UPROPERTY()
    FName FunctionName;         // Auto-generated: "{TimelineName}__{TrackName}__EventFunc"

public:
    UPROPERTY()
    TObjectPtr<class UCurveFloat> CurveKeys;    // Must set bIsEventCurve = true on this curve

    FName GetFunctionName() const { return FunctionName; }
    ENGINE_API virtual void SetTrackName(FName NewTrackName, UTimelineTemplate* OwningTimeline) override final;
};
```

#### FTTFloatTrack, FTTVectorTrack, FTTLinearColorTrack

```cpp
struct FTTFloatTrack : public FTTPropertyTrack
{
    TObjectPtr<class UCurveFloat> CurveFloat;
};

struct FTTVectorTrack : public FTTPropertyTrack
{
    TObjectPtr<class UCurveVector> CurveVector;
};

struct FTTLinearColorTrack : public FTTPropertyTrack
{
    TObjectPtr<class UCurveLinearColor> CurveLinearColor;
};
```

#### FTTTrackId (display order entry)

```cpp
USTRUCT()
struct FTTTrackId
{
    UPROPERTY()
    int32 TrackType;    // Maps to FTTTrackBase::ETrackType values (0-3)
    UPROPERTY()
    int32 TrackIndex;   // Index into the corresponding track array
};
```

---

### 5. Curve Object Creation and Key Manipulation

#### UCurveFloat

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/CurveFloat.h`

```cpp
UCLASS(BlueprintType, MinimalAPI)
class UCurveFloat : public UCurveBase
{
    UPROPERTY()
    FRichCurve FloatCurve;      // The actual curve data

    UPROPERTY()
    bool bIsEventCurve;          // Set to true for event track curves
};
```

#### UCurveVector

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/CurveVector.h`

```cpp
UCLASS(BlueprintType, MinimalAPI)
class UCurveVector : public UCurveBase
{
    UPROPERTY()
    FRichCurve FloatCurves[3];    // [0]=X, [1]=Y, [2]=Z
};
```

#### UCurveLinearColor

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/CurveLinearColor.h`

```cpp
UCLASS(BlueprintType, collapsecategories, hidecategories=(FilePath), MinimalAPI)
class UCurveLinearColor : public UCurveBase
{
    UPROPERTY()
    FRichCurve FloatCurves[4];    // [0]=R, [1]=G, [2]=B, [3]=A
};
```

#### FRichCurve::AddKey

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/RichCurve.h` (line 231)

```cpp
ENGINE_API virtual FKeyHandle AddKey(
    float InTime,
    float InValue,
    const bool bUnwindRotation = false,
    FKeyHandle KeyHandle = FKeyHandle()
) final override;
```

Keys are inserted in sorted time order. Default interpolation mode is `RCIM_Linear`. After adding keys, you can optionally change interp mode:

```cpp
FKeyHandle Handle = Curve->FloatCurve.AddKey(0.0f, 0.0f);
Curve->FloatCurve.SetKeyInterpMode(Handle, RCIM_Cubic);
```

#### ERichCurveInterpMode

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/RealCurve.h` (line 12)

```cpp
enum ERichCurveInterpMode : int
{
    RCIM_Linear,      // Linear interpolation between keys
    RCIM_Constant,    // Step function (holds value until next key)
    RCIM_Cubic,       // Cubic (tangent-based smooth curves)
    RCIM_None         // Hidden, no interpolation
};
```

#### Creating curve objects — exact pattern from STimelineEditor::CreateNewTrack

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/Kismet/Private/STimelineEditor.cpp` (line 1348)

```cpp
// The outer for curve objects is Blueprint->GeneratedClass (not the Blueprint itself)
UClass* OwnerClass = Blueprint->GeneratedClass;

// Float track
NewTrack.CurveFloat = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);

// Event track
NewTrack.CurveKeys = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
NewTrack.CurveKeys->bIsEventCurve = true;

// Vector track
NewTrack.CurveVector = NewObject<UCurveVector>(OwnerClass, NAME_None, RF_Public);

// Color track
NewTrack.CurveLinearColor = NewObject<UCurveLinearColor>(OwnerClass, NAME_None, RF_Public);
```

**Critical: `RF_Public` is required.** The comment in engine source says: "Needs to be marked public so that it can be referenced from timeline instances in the level."

**Critical: The outer must be `Blueprint->GeneratedClass`** — not the Blueprint itself, not the template. The STimelineEditor uses `Blueprint->GeneratedClass` as the outer for all curve objects.

---

### 6. AllocateDefaultPins — Exact Pin Creation Order

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_Timeline.cpp` (line 119)

```cpp
void UK2Node_Timeline::AllocateDefaultPins()
{
    bCanRenameNode = 1;

    // --- FIXED exec input pins (always created) ---
    CreatePin(EGPD_Input, PC_Exec, FName("Play"));
    CreatePin(EGPD_Input, PC_Exec, FName("PlayFromStart"));
    CreatePin(EGPD_Input, PC_Exec, FName("Stop"));
    CreatePin(EGPD_Input, PC_Exec, FName("Reverse"));
    CreatePin(EGPD_Input, PC_Exec, FName("ReverseFromEnd"));

    // --- FIXED exec output pins ---
    CreatePin(EGPD_Output, PC_Exec, FName("Update"));
    CreatePin(EGPD_Output, PC_Exec, FName("Finished"));

    // --- SetNewTime exec input + NewTime data input ---
    CreatePin(EGPD_Input, PC_Exec, FName("SetNewTime"));
    UEdGraphPin* NewPositionPin = CreatePin(EGPD_Input, PC_Real, PC_Float, FName("NewTime"));
    K2Schema->SetPinAutogeneratedDefaultValue(NewPositionPin, TEXT("0.0"));

    // --- Direction data output (enum) ---
    CreatePin(EGPD_Output, PC_Byte, FTimeline::GetTimelineDirectionEnum(), FName("Direction"));

    // --- DYNAMIC track pins (from template) ---
    UBlueprint* Blueprint = GetBlueprint();   // calls FindBlueprintForNodeChecked internally
    UTimelineTemplate* Timeline = Blueprint->FindTimelineTemplateByVariableName(TimelineName);
    if (Timeline)
    {
        PreloadObject(Timeline);

        for (int32 i = 0; i < Timeline->GetNumDisplayTracks(); ++i)
        {
            FTTTrackId TrackId = Timeline->GetDisplayTrackId(i);

            if (TrackId.TrackType == FTTTrackBase::TT_Event)
            {
                // Event track → exec output pin
                CreatePin(EGPD_Output, PC_Exec, EventTrack.GetTrackName());
            }
            else if (TrackId.TrackType == FTTTrackBase::TT_FloatInterp)
            {
                // Float track → PC_Real/PC_Float output pin
                CreatePin(EGPD_Output, PC_Real, PC_Float, FloatTrack.GetTrackName());
            }
            else if (TrackId.TrackType == FTTTrackBase::TT_VectorInterp)
            {
                // Vector track → PC_Struct/FVector output pin
                CreatePin(EGPD_Output, PC_Struct, TBaseStructure<FVector>::Get(),
                          VectorTrack.GetTrackName());
            }
            else if (TrackId.TrackType == FTTTrackBase::TT_LinearColorInterp)
            {
                // Color track → PC_Struct/FLinearColor output pin
                CreatePin(EGPD_Output, PC_Struct, TBaseStructure<FLinearColor>::Get(),
                          LinearColorTrack.GetTrackName());
            }
        }

        // Cache template flags to node transient properties
        bAutoPlay = Timeline->bAutoPlay;
        bLoop = Timeline->bLoop;
        bReplicated = Timeline->bReplicated;
        bIgnoreTimeDilation = Timeline->bIgnoreTimeDilation;
    }

    Super::AllocateDefaultPins();
}
```

**Key observation:** `AllocateDefaultPins` calls `GetBlueprint()` which calls `FindBlueprintForNodeChecked()`. This means the node MUST already be associated with a graph that has a `UBlueprint` as its outer chain. If the node is on a transient graph with `GetTransientPackage()` as outer, this will crash.

---

### 7. Complete Pin Layout Reference

| Pin Name | Category | SubCategory | Direction | Type | Source |
|----------|----------|-------------|-----------|------|--------|
| `Play` | PC_Exec | — | Input | Exec | Fixed |
| `PlayFromStart` | PC_Exec | — | Input | Exec | Fixed |
| `Stop` | PC_Exec | — | Input | Exec | Fixed |
| `Reverse` | PC_Exec | — | Input | Exec | Fixed |
| `ReverseFromEnd` | PC_Exec | — | Input | Exec | Fixed |
| `SetNewTime` | PC_Exec | — | Input | Exec | Fixed |
| `NewTime` | PC_Real | PC_Float | Input | Float | Fixed |
| `Update` | PC_Exec | — | Output | Exec | Fixed |
| `Finished` | PC_Exec | — | Output | Exec | Fixed |
| `Direction` | PC_Byte | ETimelineDirection | Output | Enum | Fixed |
| `{TrackName}` | PC_Exec | — | Output | Exec | Per Event Track |
| `{TrackName}` | PC_Real | PC_Float | Output | Float | Per Float Track |
| `{TrackName}` | PC_Struct | FVector | Output | FVector | Per Vector Track |
| `{TrackName}` | PC_Struct | FLinearColor | Output | FLinearColor | Per Color Track |

**Total fixed pins:** 10 (5 exec inputs + 1 float input + 2 exec outputs + 1 enum output + SetNewTime exec input)

**Exact FName values for pin names** (from file-scope static FNames in K2Node_Timeline.cpp):

```cpp
static FName PlayPinName(TEXT("Play"));
static FName PlayFromStartPinName(TEXT("PlayFromStart"));
static FName StopPinName(TEXT("Stop"));
static FName UpdatePinName(TEXT("Update"));
static FName ReversePinName(TEXT("Reverse"));
static FName ReverseFromEndPinName(TEXT("ReverseFromEnd"));
static FName FinishedPinName(TEXT("Finished"));
static FName NewTimePinName(TEXT("NewTime"));
static FName SetNewTimePinName(TEXT("SetNewTime"));
static FName DirectionPinName(TEXT("Direction"));
```

---

### 8. The Blueprint Editor's Own Creation Path

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_Timeline.cpp` (line 722, `GetMenuActions`)

When a user picks "Add Timeline..." from the right-click menu, this is the exact sequence:

```cpp
// GetMenuActions registers a UBlueprintNodeSpawner with a custom lambda:
auto CustomizeTimelineNodeLambda = [](UEdGraphNode* NewNode, bool bIsTemplateNode)
{
    UK2Node_Timeline* TimelineNode = CastChecked<UK2Node_Timeline>(NewNode);

    UBlueprint* Blueprint = TimelineNode->GetBlueprint();
    if (Blueprint != nullptr)
    {
        // Step 1: Generate unique name (e.g. "Timeline_0", "Timeline_1")
        TimelineNode->TimelineName = FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint);

        // Step 2: Create the UTimelineTemplate (only for real nodes, not template previews)
        if (!bIsTemplateNode &&
            FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineNode->TimelineName))
        {
            TimelineNode->ErrorMsg.Empty();
            TimelineNode->bHasCompilerMessage = false;
        }
    }
};
```

**What the editor does NOT do in this lambda:**
- It does NOT set TimelineLength (defaults to 5.0f from constructor)
- It does NOT set LengthMode (defaults to TL_TimelineLength)
- It does NOT add any tracks (empty timeline by default)
- It does NOT call AllocateDefaultPins (the spawner framework does that automatically)

The user then opens the timeline editor (STimelineEditor) to add tracks and configure properties manually.

**There is NO `PostPlacedNewNode` override** on `UK2Node_Timeline` — verified by reading the full .cpp.

---

### 9. FBlueprintEditorUtils::AddNewTimeline — Complete Implementation

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/UnrealEd/Private/Kismet2/BlueprintEditorUtils.cpp` (line 8000)

```cpp
UTimelineTemplate* FBlueprintEditorUtils::AddNewTimeline(UBlueprint* Blueprint, const FName& TimelineVarName)
{
    // Guard: non-Actor BPs silently return nullptr
    if (!FBlueprintEditorUtils::DoesSupportTimelines(Blueprint))
    {
        return nullptr;
    }

    // Guard: duplicate name silently returns nullptr (logs a message)
    UTimelineTemplate* Timeline = Blueprint->FindTimelineTemplateByVariableName(TimelineVarName);
    if (Timeline != nullptr)
    {
        UE_LOG(LogBlueprint, Log, TEXT("AddNewTimeline: Blueprint '%s' already contains "
            "a timeline called '%s'"), *Blueprint->GetPathName(), *TimelineVarName.ToString());
        return nullptr;
    }

    Blueprint->Modify();
    check(nullptr != Blueprint->GeneratedClass);

    // Create the template object
    const FName TimelineTemplateName = *UTimelineTemplate::TimelineVariableNameToTemplateName(TimelineVarName);
    Timeline = NewObject<UTimelineTemplate>(Blueprint->GeneratedClass, TimelineTemplateName, RF_Transactional);

    // Register with the Blueprint
    Blueprint->Timelines.Add(Timeline);

    // Validate child blueprints
    FBlueprintEditorUtils::ValidateBlueprintChildVariables(Blueprint, TimelineVarName);

    // Mark as structurally modified (triggers recompile)
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    return Timeline;
}
```

**Key observations:**
1. The template's UObject **outer** is `Blueprint->GeneratedClass` (not the Blueprint itself).
2. The template's **object name** is `"{VarName}_Template"` (e.g., `"Timeline_0_Template"`).
3. `RF_Transactional` flag is set — this enables undo/redo for the template object itself.
4. `MarkBlueprintAsStructurallyModified` is called, which triggers a recompile.
5. The method returns `nullptr` on failure (unsupported BP type or duplicate name) — **no error is thrown, no exception**.

---

### 10. DoesSupportTimelines — Exact Check

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/UnrealEd/Private/Kismet2/BlueprintEditorUtils.cpp` (line 3491)

```cpp
bool FBlueprintEditorUtils::DoesSupportTimelines(const UBlueprint* Blueprint)
{
    return FBlueprintEditorUtils::IsActorBased(Blueprint) &&
           FBlueprintEditorUtils::DoesSupportEventGraphs(Blueprint);
}

bool FBlueprintEditorUtils::DoesSupportEventGraphs(const UBlueprint* Blueprint)
{
    return Blueprint->BlueprintType == BPTYPE_Normal
        || Blueprint->BlueprintType == BPTYPE_LevelScript;
}
```

`IsActorBased` checks if `Blueprint->ParentClass->IsChildOf(AActor::StaticClass())`.

So the full check is: *Actor-derived parent class AND (BPTYPE_Normal OR BPTYPE_LevelScript).*

Widget BPs, Component BPs, Function Libraries, Interfaces, and Macro Libraries are all excluded.

---

### 11. IsCompatibleWithGraph — Graph Placement Rules

Source: `K2Node_Timeline.cpp` (line 308)

Timeline nodes can only be placed in:
1. **Event graphs (ubergraphs)** of Blueprints that support event graphs and timelines.
2. **Composite graphs** whose outer chain contains an ubergraph (i.e., a collapsed graph inside an event graph).

They CANNOT be placed in:
- Function graphs
- Macro graphs
- Animation graphs
- Any non-event-graph

The implementation walks the composite graph outer chain to check if there's an ubergraph ancestor.

---

### 12. Blueprint->FindTimelineTemplateByVariableName

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Private/Blueprint.cpp` (line 1199)

```cpp
UTimelineTemplate* UBlueprint::FindTimelineTemplateByVariableName(const FName& TimelineName)
{
    const FName TimelineTemplateName = *UTimelineTemplate::TimelineVariableNameToTemplateName(TimelineName);
    UTimelineTemplate* Timeline = FindObjectByName(Timelines, TimelineTemplateName);

    // Backwards compatibility: also searches by raw variable name (pre-4.14 format)
    if (!Timeline)
    {
        Timeline = FindObjectByName(Timelines, TimelineName);
    }
    return Timeline;
}
```

---

### 13. Track Addition — Exact Sequence from STimelineEditor

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/Kismet/Private/STimelineEditor.cpp` (line 1348)

This is what happens when a user clicks "Add Float Track" in the timeline editor:

```cpp
void STimelineEditor::CreateNewTrack(FTTTrackBase::ETrackType Type)
{
    // 1. Generate unique track name
    FName TrackName;
    do {
        TrackName = MakeUniqueObjectName(TimelineObj, UTimelineTemplate::StaticClass(),
                                          FName(TEXT("NewTrack")));
    } while (!TimelineObj->IsNewTrackNameValid(TrackName));

    // 2. Get node and class references
    UK2Node_Timeline* TimelineNode = FBlueprintEditorUtils::FindNodeForTimeline(Blueprint, TimelineObj);
    UClass* OwnerClass = Blueprint->GeneratedClass;

    // 3. Transaction + Modify
    const FScopedTransaction Transaction(LOCTEXT("TimelineEditor_AddNewTrack", "Add new track"));
    TimelineNode->Modify();
    TimelineObj->Modify();

    // 4. Build track ID for display order
    FTTTrackId NewTrackId;
    NewTrackId.TrackType = Type;

    // 5. Create track struct + curve object
    if (Type == FTTTrackBase::TT_Event)
    {
        NewTrackId.TrackIndex = TimelineObj->EventTracks.Num();

        FTTEventTrack NewTrack;
        NewTrack.SetTrackName(TrackName, TimelineObj);
        NewTrack.CurveKeys = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
        NewTrack.CurveKeys->bIsEventCurve = true;
        TimelineObj->EventTracks.Add(NewTrack);
    }
    else if (Type == FTTTrackBase::TT_FloatInterp)
    {
        NewTrackId.TrackIndex = TimelineObj->FloatTracks.Num();

        FTTFloatTrack NewTrack;
        NewTrack.SetTrackName(TrackName, TimelineObj);
        NewTrack.CurveFloat = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
        TimelineObj->FloatTracks.Add(NewTrack);
    }
    else if (Type == FTTTrackBase::TT_VectorInterp)
    {
        NewTrackId.TrackIndex = TimelineObj->VectorTracks.Num();

        FTTVectorTrack NewTrack;
        NewTrack.SetTrackName(TrackName, TimelineObj);
        NewTrack.CurveVector = NewObject<UCurveVector>(OwnerClass, NAME_None, RF_Public);
        TimelineObj->VectorTracks.Add(NewTrack);
    }
    else if (Type == FTTTrackBase::TT_LinearColorInterp)
    {
        NewTrackId.TrackIndex = TimelineObj->LinearColorTracks.Num();

        FTTLinearColorTrack NewTrack;
        NewTrack.SetTrackName(TrackName, TimelineObj);
        NewTrack.CurveLinearColor = NewObject<UCurveLinearColor>(OwnerClass, NAME_None, RF_Public);
        TimelineObj->LinearColorTracks.Add(NewTrack);
    }

    // 6. Register display order
    TimelineObj->AddDisplayTrack(NewTrackId);

    // 7. Reconstruct node to rebuild pins
    TimelineNode->ReconstructNode();
}
```

**Key pattern:** The track index in `FTTTrackId` is set BEFORE adding to the array (using `.Num()` which gives the next index). Then `AddDisplayTrack` registers it. Then `ReconstructNode` rebuilds pins.

---

### 14. Compilation — ExpandNode and ExpandTimelineNodes

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/KismetCompiler/Private/KismetCompiler.cpp` (line 3372)

At compile time, the Kismet compiler:

1. **`ExpandTimelineNodes()`** runs on every ubergraph. For each `UK2Node_Timeline` in the graph:
   - Creates a `UK2Node_VariableGet` intermediate node to get the timeline component
   - For each exec input pin (Play, PlayFromStart, Stop, Reverse, ReverseFromEnd, SetNewTime) that has connections, creates a `UK2Node_CallFunction` that calls the corresponding `UTimelineComponent` method
   - For Update and Finished exec outputs, creates event dispatcher bindings
   - For each event track, creates an event dispatcher binding

2. **`ExpandNode()`** (on UK2Node_Timeline itself) handles the data output pins:
   - For Direction, float tracks, vector tracks, and color tracks, creates `UK2Node_VariableGet` nodes that read the generated properties from the timeline component

**This means the timeline node is entirely expanded at compile time** — no special runtime handling is needed beyond what `UTimelineComponent` provides.

**Compilation does NOT require any special pre-processing from us.** As long as the timeline template exists in `Blueprint->Timelines` with the correct tracks and curves, and the node's `TimelineName` matches, the compiler handles everything.

---

### 15. DestroyNode Behavior

Source: `K2Node_Timeline.cpp` (line 202)

```cpp
void UK2Node_Timeline::DestroyNode()
{
    UTimelineTemplate* Timeline = Blueprint->FindTimelineTemplateByVariableName(TimelineName);
    if (Timeline)
    {
        FBlueprintEditorUtils::RemoveTimeline(Blueprint, Timeline, true);
        Timeline->Rename(NULL, GetTransientPackage(), REN_None);  // Move out of the way
    }
    Super::DestroyNode();
}
```

When the node is deleted, it automatically removes the associated template from `Blueprint->Timelines`. This is important because:
- If we delete a timeline node, the template is cleaned up automatically
- If we want to "replace" a timeline, deleting the old node will remove its template

---

### 16. PostPasteNode Behavior

Source: `K2Node_Timeline.cpp` (line 218)

On paste:
1. Searches for the `UTimelineTemplate` by `TimelineGuid` across all objects
2. Generates a new unique name via `FindUniqueTimelineName`
3. If the source template is found, duplicates it with `DuplicateObject<UTimelineTemplate>` using the new name
4. Reparents all internal curve objects to the new Blueprint
5. Sets `RF_Transactional` on the new template
6. Adds the new template to `Blueprint->Timelines`

---

### 17. Edge Cases and Gotchas

#### Duplicate Timeline Name
`AddNewTimeline` checks for existing templates by name. If a template with the same variable name already exists, it returns `nullptr` and logs: `"AddNewTimeline: Blueprint '%s' already contains a timeline called '%s'"`. No error/exception — just a silent null return.

#### Non-Actor Blueprints
`AddNewTimeline` returns `nullptr` silently. `DoesSupportTimelines` returns false for Widget BPs, Component BPs, Function Libraries, Interfaces, Macro Libraries.

#### Transaction Handling
`AddNewTimeline` calls `Blueprint->Modify()` internally. However, it does NOT create an `FScopedTransaction`. The caller is expected to wrap timeline creation in a transaction for undo/redo support.

#### Thread Safety
All of this MUST happen on the game thread:
- `NewObject` for templates and curve objects
- `Blueprint->Modify()`
- `MarkBlueprintAsStructurallyModified`
- `AllocateDefaultPins` and `ReconstructNode`
- Array manipulation on `Blueprint->Timelines`

#### MarkBlueprintAsStructurallyModified vs MarkBlueprintAsModified
- `AddNewTimeline` calls `MarkBlueprintAsStructurallyModified` (triggers recompile)
- Property-only changes (TimelineLength, bAutoPlay) need only `MarkBlueprintAsModified`
- Adding tracks to an existing timeline needs `MarkBlueprintAsStructurallyModified` (new pins = structural change)
- In practice, for a new timeline creation where we do everything at once, the single `MarkBlueprintAsStructurallyModified` call from `AddNewTimeline` covers the initial creation. But if we add tracks AFTER the initial `AddNewTimeline` call, we need another structural modification marker.

#### AddNewTimeline triggers MarkBlueprintAsStructurallyModified
This means calling `AddNewTimeline` BEFORE setting up tracks and then calling `AllocateDefaultPins` means the pins will be allocated for an EMPTY template first (from the structural modification triggering a reconstruct), and then we'll need to add tracks and call `ReconstructNode` again.

**Better approach:** Call `AddNewTimeline`, immediately configure the template (add tracks, set properties), then the structural modification cascade will pick up the complete template. Or defer the structural modification by doing: `AddNewTimeline` (which marks structural), then immediately add tracks before any graph operations process the modification.

In practice, within a single synchronous code path, the modification flag is just set — it doesn't trigger an immediate recompile. The recompile happens later (on tick or when explicitly requested). So the sequence `AddNewTimeline -> add tracks -> add display tracks -> AllocateDefaultPins` works correctly because the pins are allocated with the fully-configured template.

---

### 18. Interaction with Existing Olive Codebase

#### Current CreateNodeByClass path
If the AI calls `blueprint.add_node` with `node_type: "K2Node_Timeline"`, the current `CreateNodeByClass` will:
1. Resolve the class via `FindK2NodeClass`
2. Create with `NewObject<UEdGraphNode>(Graph, NodeClass)`
3. Set any provided properties via reflection (only `TimelineName` and `TimelineGuid` would work)
4. Call `AllocateDefaultPins()` — which will try to find a template by `TimelineName`
5. Since no template exists (nothing called `AddNewTimeline`), it will create only the 10 fixed pins with no track pins

This means the current generic path will create a **broken timeline node** — it has a name but no template, so it can never be compiled. The node will show "Add Timeline..." as its title (the title fallback when no template is found).

#### The `timeline` plan op
The plan ops vocabulary in `BlueprintPlanIR.h` does NOT currently include a `timeline` op. There is no handling in the resolver or executor for timeline creation.

#### NodeSerializer
The serializer already handles timeline nodes correctly (`SerializeTimeline`), reading `TimelineName` and the transient flag caches. This works for reading existing timelines.

---

## Recommended Implementation Sequence

### For a `blueprint.create_timeline` Tool

The recommended code path for creating a timeline programmatically, based on how the engine itself does it:

```cpp
// Pre-validation
if (!Blueprint || !Graph) { return error; }
if (!FBlueprintEditorUtils::DoesSupportTimelines(Blueprint))
{
    return error("Timelines only work in Actor-based Blueprints (BPTYPE_Normal or BPTYPE_LevelScript)");
}

// Ensure Graph is a ubergraph (event graph)
const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
if (!K2Schema || K2Schema->GetGraphType(Graph) != GT_Ubergraph)
{
    return error("Timelines can only be placed in event graphs");
}

// Transaction
FScopedTransaction Transaction(LOCTEXT("CreateTimeline", "Create Timeline"));
Blueprint->Modify();
Graph->Modify();

// 1. Generate or validate timeline name
FName TimelineName = /* user-provided or */ FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint);

// 2. Create the node
UK2Node_Timeline* TimelineNode = NewObject<UK2Node_Timeline>(Graph);
TimelineNode->TimelineName = TimelineName;
TimelineNode->CreateNewGuid();

// 3. Create the template (BEFORE AllocateDefaultPins)
UTimelineTemplate* Template = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineName);
if (!Template) { /* cleanup node and return error */ }

// 4. Configure template properties
Template->Modify();
Template->TimelineLength = UserLength;  // default 5.0f from constructor
Template->LengthMode = TL_TimelineLength; // or TL_LastKeyFrame
Template->bAutoPlay = UserAutoPlay;
Template->bLoop = UserLoop;
Template->bReplicated = UserReplicated;
Template->bIgnoreTimeDilation = UserIgnoreTimeDilation;

// 5. Add tracks (for each user-requested track)
UClass* OwnerClass = Blueprint->GeneratedClass;
check(OwnerClass);

// Example: add a float track
{
    FTTFloatTrack NewTrack;
    NewTrack.SetTrackName(FName(*TrackName), Template);
    NewTrack.CurveFloat = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);

    // Add curve keys
    for (auto& Key : UserKeys)
    {
        NewTrack.CurveFloat->FloatCurve.AddKey(Key.Time, Key.Value);
    }

    Template->FloatTracks.Add(NewTrack);

    FTTTrackId TrackId;
    TrackId.TrackType = FTTTrackBase::TT_FloatInterp;
    TrackId.TrackIndex = Template->FloatTracks.Num() - 1;
    Template->AddDisplayTrack(TrackId);
}

// 6. Allocate pins (reads template to create track output pins)
TimelineNode->AllocateDefaultPins();

// 7. Add to graph
Graph->AddNode(TimelineNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

// 8. Position the node
TimelineNode->NodePosX = PosX;
TimelineNode->NodePosY = PosY;

// 9. The transient caches are already set by AllocateDefaultPins
// but MarkBlueprintAsStructurallyModified was already called by AddNewTimeline

return success;
```

### Critical Ordering Rules

1. `TimelineNode->TimelineName` MUST be set BEFORE `AllocateDefaultPins`
2. `AddNewTimeline` MUST be called BEFORE `AllocateDefaultPins` (creates the template)
3. Tracks MUST be added to the template BEFORE `AllocateDefaultPins` (pins are created from tracks)
4. `SetTrackName(name, template)` MUST be used (not direct field assignment — `TrackName` is private)
5. `AddDisplayTrack(trackId)` MUST be called for each track (pins iterate display order, not track arrays)
6. Curve outer MUST be `Blueprint->GeneratedClass` with `RF_Public` flag

### What NOT to Do

- Do NOT call `set_node_property` for `TimelineLength`, `bAutoPlay`, `bLoop` — these are on the template, not the node
- Do NOT use `GetTrackPin()` — it's declared but not implemented. Use `FindPin(TrackName)` instead.
- Do NOT skip `AddDisplayTrack` — without it, the track exists in the array but gets no pin
- Do NOT create timeline nodes via the generic `CreateNodeByClass` path — it cannot create the template
- Do NOT place timeline nodes in function graphs — `IsCompatibleWithGraph` will reject them
- Do NOT set `TimelineGuid` manually — `PostInitProperties` on the template auto-generates it

### For a `timeline` Plan Op (if adding to OlivePlanOps)

A timeline plan op would need special handling because:
1. It creates TWO objects (node + template), unlike most ops which create one node
2. It needs to create child objects (curve UObjects) as subobjects
3. Track data (key points) requires array-of-struct JSON representation
4. The node must go in an event graph only (not function graphs)

Suggested JSON shape:
```json
{
    "op": "timeline",
    "id": "timeline_1",
    "timeline_name": "FadeTimeline",
    "length": 2.0,
    "auto_play": true,
    "loop": false,
    "tracks": [
        {
            "name": "Alpha",
            "type": "float",
            "keys": [[0.0, 0.0], [1.0, 1.0], [2.0, 0.0]]
        },
        {
            "name": "OnFinish",
            "type": "event",
            "keys": [[2.0, 0.0]]
        }
    ]
}
```

### Wiring Timeline Outputs

After creation, the AI needs to wire timeline outputs to downstream nodes. The pin names for wiring are:
- `"Update"` — exec output that fires every tick during playback
- `"Finished"` — exec output that fires when timeline completes
- `"Alpha"` (or whatever track name) — data output for float/vector/color tracks
- `"OnFinish"` (or whatever track name) — exec output for event tracks
- `"Play"`, `"PlayFromStart"` etc. — exec inputs to control the timeline

### Required Headers and Modules

| Header | Module | Used For |
|--------|--------|----------|
| `K2Node_Timeline.h` | BlueprintGraph | UK2Node_Timeline |
| `Engine/TimelineTemplate.h` | Engine | UTimelineTemplate, FTTFloatTrack, etc. |
| `Curves/CurveFloat.h` | Engine | UCurveFloat |
| `Curves/CurveVector.h` | Engine | UCurveVector |
| `Curves/CurveLinearColor.h` | Engine | UCurveLinearColor |
| `Curves/RichCurve.h` | Engine | FRichCurve, FRichCurveKey, AddKey |
| `Curves/RealCurve.h` | Engine | ERichCurveInterpMode |
| `Components/TimelineComponent.h` | Engine | ETimelineLengthMode, FTimeline |
| `Kismet2/BlueprintEditorUtils.h` | UnrealEd | AddNewTimeline, FindUniqueTimelineName, etc. |

All of these modules are already dependencies in `OliveAIEditor.Build.cs` (Engine, BlueprintGraph, UnrealEd).
