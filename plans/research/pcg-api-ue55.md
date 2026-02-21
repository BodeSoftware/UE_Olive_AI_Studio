# Research: PCG Plugin Public API (UE 5.5)

## Question
What are the public method signatures for UPCGGraph, UPCGNode, UPCGPin, UPCGSettings,
EPCGDataType, UPCGSubgraphSettings, UPCGComponent, and UPCGSubsystem in UE 5.5?

Source headers all located at:
`C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/PCG/Source/PCG/Public/`

---

## Findings

### 1. UPCGGraph (PCGGraph.h)

`UPCGGraph` extends `UPCGGraphInterface` which extends `UObject`.
The graph also owns the `InputNode` and `OutputNode` as serialized properties.

#### Key public methods

```cpp
// Node creation
UPCGNode* AddNode(UPCGSettingsInterface* InSettings);                               // does NOT manage ownership
UPCGNode* AddNodeOfType(TSubclassOf<UPCGSettings> InSettingsClass,
                         UPCGSettings*& DefaultNodeSettings);                       // UFUNCTION, Blueprint callable
UPCGNode* AddNodeInstance(UPCGSettings* InSettings);                                // node holds an instance (reference)
UPCGNode* AddNodeCopy(const UPCGSettings* InSettings,
                       UPCGSettings*& DefaultNodeSettings);                         // node holds a copy

// Template version (C++ only, no UFUNCTION):
template <typename T>
UPCGNode* AddNodeOfType(T*& DefaultNodeSettings);

// Node removal
void RemoveNode(UPCGNode* InNode);                                                  // UFUNCTION
void RemoveNodes(TArray<UPCGNode*>& InNodes);                                       // UFUNCTION, bulk

// Edge management
UPCGNode* AddEdge(UPCGNode* From, const FName& FromPinLabel,
                  UPCGNode* To,   const FName& ToPinLabel);                         // UFUNCTION, returns To node
bool RemoveEdge(UPCGNode* From, const FName& FromLabel,
                UPCGNode* To,   const FName& ToLabel);                              // UFUNCTION
bool AddLabeledEdge(UPCGNode* From, const FName& InboundLabel,
                    UPCGNode* To,   const FName& OutboundLabel);                    // non-UFUNCTION; returns true if To had edges removed
bool RemoveInboundEdges(UPCGNode* InNode, const FName& InboundLabel);
bool RemoveOutboundEdges(UPCGNode* InNode, const FName& OutboundLabel);

// Input/Output nodes (always present, created with graph)
UPCGNode* GetInputNode()  const { return InputNode; }                               // UFUNCTION
UPCGNode* GetOutputNode() const { return OutputNode; }                              // UFUNCTION

// Node enumeration
const TArray<UPCGNode*>& GetNodes() const { return Nodes; }
bool ForEachNode(TFunctionRef<bool(UPCGNode*)> Action) const;
bool ForEachNodeRecursively(TFunctionRef<bool(UPCGNode*)> Action) const;

// Search / contains
bool Contains(UPCGNode* Node) const;
bool Contains(const UPCGGraph* InGraph) const;                                      // recursive subgraph check
UPCGNode* FindNodeWithSettings(const UPCGSettingsInterface* InSettings,
                                bool bRecursive = false) const;

// Duplicate a node (no edges)
TObjectPtr<UPCGNode> ReconstructNewNode(const UPCGNode* InNode);

// User parameters (graph-level exposed variables)
void AddUserParameters(const TArray<FPropertyBagPropertyDesc>& InDescs,
                       const UPCGGraph* InOptionalOriginalGraph = nullptr);
void UpdateUserParametersStruct(TFunctionRef<void(FInstancedPropertyBag&)> Callback);

// Editor notifications
void ForceNotificationForEditor(EPCGChangeType ChangeType = EPCGChangeType::Structural); // WITH_EDITOR
```

#### Important notes
- `AddNode(UPCGSettingsInterface*)` does NOT set ownership — the caller must manage the settings object lifetime.
- `AddNodeOfType` creates a new default settings object and returns it via the out-param.
- `AddNodeInstance` takes an existing settings asset and makes the node reference it (shared ownership).
- `AddEdge` and `AddLabeledEdge` differ: `AddEdge` is the UFUNCTION Blueprint-safe version; `AddLabeledEdge` returns whether an existing edge was removed from a single-connection pin.
- The `Nodes` array does NOT include `InputNode` and `OutputNode` — those are stored separately.
- Changing graph structure should be wrapped in `FScopedTransaction` for undo support.

---

### 2. UPCGNode (PCGNode.h)

`UPCGNode` extends `UObject`. The owning graph manages its lifetime.

#### Key public methods

```cpp
// Owning graph
UPCGGraph* GetGraph() const;                                                        // UFUNCTION

// Settings access
UPCGSettingsInterface* GetSettingsInterface() const { return SettingsInterface.Get(); }
UPCGSettings* GetSettings() const;                                                  // UFUNCTION - goes through interface
void SetSettingsInterface(UPCGSettingsInterface* InSettingsInterface,
                          bool bUpdatePins = true);

// Convenience edge helpers (delegates to owning graph)
UPCGNode* AddEdgeTo(FName FromPinLabel, UPCGNode* To, FName ToPinLabel);            // UFUNCTION
bool RemoveEdgeTo(FName FromPinLabel, UPCGNode* To, FName ToPinLabel);              // UFUNCTION

// Pin access
UPCGPin*       GetInputPin(const FName& Label);
const UPCGPin* GetInputPin(const FName& Label) const;
UPCGPin*       GetOutputPin(const FName& Label);
const UPCGPin* GetOutputPin(const FName& Label) const;

const TArray<TObjectPtr<UPCGPin>>& GetInputPins()  const { return InputPins; }
const TArray<TObjectPtr<UPCGPin>>& GetOutputPins() const { return OutputPins; }

TArray<FPCGPinProperties> InputPinProperties()  const;
TArray<FPCGPinProperties> OutputPinProperties() const;

bool IsInputPinConnected(const FName& Label)  const;
bool IsOutputPinConnected(const FName& Label) const;

bool HasInboundEdges() const;
int32 GetInboundEdgesNum() const;

// Pin rename (keeps existing edges)
void RenameInputPin(const FName& InOldLabel, const FName& InNewLabel,
                    bool bInBroadcastUpdate = true);
void RenameOutputPin(const FName& InOldLabel, const FName& InNewLabel,
                     bool bInBroadcastUpdate = true);

// Node identity
bool IsInstance() const;                                                            // true if settings are shared (not owned)
FText GetNodeTitle(EPCGNodeTitleType TitleType) const;
bool HasAuthoredTitle() const { return NodeTitle != NAME_None; }

// Editor-only position
void GetNodePosition(int32& OutPositionX, int32& OutPositionY) const;               // WITH_EDITOR UFUNCTION
void SetNodePosition(int32 InPositionX, int32 InPositionY);                         // WITH_EDITOR UFUNCTION

// Post-creation hook (call after changing settings on a new node)
void UpdateAfterSettingsChangeDuringCreation();
```

#### Serialized editable properties

```cpp
UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Node)
FName NodeTitle = NAME_None;

// Editor-only:
int32 PositionX;
int32 PositionY;
FString NodeComment;
```

---

### 3. UPCGPin (PCGPin.h)

`UPCGPin` extends `UObject` and is owned by a `UPCGNode`.

#### Key public methods and properties

```cpp
// Properties struct (label, type, multi-connection flags, required/advanced status)
UPROPERTY(BlueprintReadOnly, EditAnywhere, Category = Settings, meta = (ShowOnlyInnerProperties))
FPCGPinProperties Properties;

// Owning node
UPROPERTY(BlueprintReadOnly, Category = Properties)
TObjectPtr<UPCGNode> Node = nullptr;

// Connected edges
UPROPERTY(BlueprintReadOnly, TextExportTransient, Category = Properties)
TArray<TObjectPtr<UPCGEdge>> Edges;

// Connection helpers
bool AllowsMultipleConnections() const;
bool AllowsMultipleData() const;
bool IsCompatible(const UPCGPin* OtherPin) const;
bool CanConnect(const UPCGPin* OtherPin) const;

bool AddEdgeTo(UPCGPin* OtherPin, TSet<UPCGNode*>* InTouchedNodes = nullptr);
bool BreakEdgeTo(UPCGPin* OtherPin, TSet<UPCGNode*>* InTouchedNodes = nullptr);
bool BreakAllEdges(TSet<UPCGNode*>* InTouchedNodes = nullptr);
bool BreakAllIncompatibleEdges(TSet<UPCGNode*>* InTouchedNodes = nullptr);

bool IsConnected() const;                                                           // UFUNCTION
bool IsOutputPin() const;                                                           // UFUNCTION
int32 EdgeCount() const;

// Current data type (may differ from Properties.AllowedTypes if dynamic)
EPCGDataType GetCurrentTypes() const;
EPCGTypeConversion GetRequiredTypeConversion(const UPCGPin* InOtherPin) const;
```

#### FPCGPinProperties struct (key fields)

```cpp
UPROPERTY(BlueprintReadWrite, EditAnywhere) FName Label = NAME_None;
UPROPERTY(BlueprintReadWrite, EditAnywhere) EPCGPinUsage Usage = EPCGPinUsage::Normal;
UPROPERTY(BlueprintReadWrite, EditAnywhere) EPCGDataType AllowedTypes = EPCGDataType::Any;
UPROPERTY(BlueprintReadWrite, EditAnywhere) bool bAllowMultipleData = true;
UPROPERTY(BlueprintReadWrite, EditAnywhere) EPCGPinStatus PinStatus = EPCGPinStatus::Normal;

// Status helpers:
bool IsAdvancedPin() const;     void SetAdvancedPin();
bool IsRequiredPin() const;     void SetRequiredPin();
bool IsNormalPin() const;       void SetNormalPin();
bool IsOverrideOrUserParamPin() const;
bool AllowsMultipleConnections() const;
void SetAllowMultipleConnections(bool bAllow);
```

#### Default pin label constants (PCGCommon.h, PCGPinConstants namespace)

```cpp
const FName DefaultInputLabel  = TEXT("In");
const FName DefaultOutputLabel = TEXT("Out");
const FName DefaultParamsLabel = TEXT("Overrides");
const FName DefaultDependencyOnlyLabel = TEXT("Dependency Only");
```

---

### 4. UPCGSettings (PCGSettings.h)

Base class for all PCG node settings. Inherits: `UPCGSettings : UPCGSettingsInterface : UPCGData : UObject`.

#### Class hierarchy

```
UObject
  UPCGData
    UPCGSettingsInterface   (abstract - adds bEnabled, bDebug)
      UPCGSettings          (abstract - base for all node types)
        UPCGTrivialSettings (used internally for Input/Output nodes)
        UPCGSubgraphSettings
        ... (all concrete node settings)
      UPCGSettingsInstance  (wraps an external UPCGSettings asset by pointer)
```

#### Key virtual methods to override when creating custom settings

```cpp
// Pin definitions (override to declare what pins your node has)
virtual TArray<FPCGPinProperties> InputPinProperties()  const;
virtual TArray<FPCGPinProperties> OutputPinProperties() const;

// Must implement — returns the IPCGElement that executes this node
virtual FPCGElementPtr CreateElement() const PURE_VIRTUAL(...);

// Editor metadata
virtual FName GetDefaultNodeName() const { return NAME_None; }
virtual FText GetDefaultNodeTitle() const;
virtual FLinearColor GetNodeTitleColor() const { return FLinearColor::White; }
virtual EPCGSettingsType GetType() const { return EPCGSettingsType::Generic; }
virtual FText GetNodeTooltipText() const { return FText::GetEmpty(); }

// Data type
virtual EPCGDataType GetDataType() const override { return EPCGDataType::Settings; }
```

#### EPCGSettingsType enum values (determines node palette category)

```cpp
InputOutput, Spatial, Density, Blueprint, Metadata, Filter, Sampler, Spawner,
Subgraph, Debug, Generic, Param, HierarchicalGeneration, ControlFlow, PointOps,
GraphParameters, Reroute, GPU, DynamicMesh
```

---

### 5. EPCGDataType (PCGCommon.h)

This is a bitmask enum (`uint32`). Cannot be used directly in Blueprints due to size;
Blueprints use `EPCGExclusiveDataType` (`uint8`) instead.

```cpp
UENUM(meta = (Bitflags))
enum class EPCGDataType : uint32
{
    None         = 0,
    Point        = 1 << 1,
    Spline       = 1 << 2,
    LandscapeSpline = 1 << 3,
    PolyLine     = Spline | LandscapeSpline,    // DisplayName = "Curve"
    Landscape    = 1 << 4,
    Texture      = 1 << 5,
    RenderTarget = 1 << 6,
    BaseTexture  = Texture | RenderTarget,
    Surface      = Landscape | BaseTexture,
    Volume       = 1 << 7,
    Primitive    = 1 << 8,
    DynamicMesh  = 1 << 10,
    Concrete     = Point | PolyLine | Surface | Volume | Primitive | DynamicMesh,
    Composite    = 1 << 9,
    Spatial      = Composite | Concrete,
    Param        = 1 << 27,                     // DisplayName = "Attribute Set"
    PointOrParam = Point | Param,
    Settings     = 1 << 28,
    Other        = 1 << 29,
    Any          = (1 << 30) - 1
};
```

Use `ENUM_CLASS_FLAGS(EPCGDataType)` is already declared — bitwise ops (`|`, `&`, `~`) work normally.

For FPCGPinProperties, set `AllowedTypes` to the bitmask of types the pin accepts, e.g.:
- `EPCGDataType::Point` — accepts only point data
- `EPCGDataType::Spatial` — accepts any spatial data
- `EPCGDataType::Any` — accepts everything (default)

---

### 6. UPCGSubgraphSettings (PCGSubgraph.h)

Class hierarchy: `UPCGSubgraphSettings : UPCGBaseSubgraphSettings : UPCGSettings`

This is the settings class for a "Subgraph" node — a node that embeds another PCG graph.

#### Setting the referenced subgraph

```cpp
// The correct public API to set the subgraph (sets up editor callbacks properly)
virtual void SetSubgraph(UPCGGraphInterface* InGraph);   // defined on UPCGBaseSubgraphSettings

// Query the current subgraph
virtual UPCGGraphInterface* GetSubgraphInterface() const override;   // on UPCGSubgraphSettings
UPCGGraph* GetSubgraph() const;                                      // on UPCGBaseSubgraphSettings — returns raw graph

// Query if it is a "dynamic" (not-inlined) execution
virtual bool IsDynamicGraph() const override;
```

#### Key stored properties

```cpp
// The owned graph instance (holds parameter overrides for this invocation)
UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = Properties, Instanced, meta = (NoResetToDefault))
TObjectPtr<UPCGGraphInstance> SubgraphInstance;

// Runtime override — can swap the subgraph at execution time
UPROPERTY(BlueprintReadOnly, Category = Properties, meta = (PCG_Overridable))
TObjectPtr<UPCGGraphInterface> SubgraphOverride;
```

#### Usage pattern

```cpp
// Create a subgraph node pointing to MyPCGGraph:
UPCGSettings* RawSettings = nullptr;
UPCGNode* SubgraphNode = Graph->AddNodeOfType(UPCGSubgraphSettings::StaticClass(), RawSettings);
UPCGSubgraphSettings* SubgraphSettings = Cast<UPCGSubgraphSettings>(RawSettings);
SubgraphSettings->SetSubgraph(MyPCGGraphInterface);   // triggers pin refresh + editor callbacks
```

Note: `UPCGGraphInstance` wraps a `UPCGGraphInterface` and stores parameter overrides.
`UPCGGraphInterface` is the abstract base; both `UPCGGraph` and `UPCGGraphInstance` implement it.
You can pass either to `SetSubgraph()`.

---

### 7. UPCGComponent (PCGComponent.h)

`UPCGComponent : UActorComponent`. Attached to any actor. Holds a `UPCGGraphInstance` and
is the primary trigger for executing a PCG graph in the world.

#### Key public methods

```cpp
// Graph assignment
void SetGraph(UPCGGraphInterface* InGraph);                                         // NetMulticast Reliable UFUNCTION
void SetGraphLocal(UPCGGraphInterface* InGraph);                                    // local-only version

UPCGGraph*         GetGraph()         const;
UPCGGraphInstance* GetGraphInstance() const { return GraphInstance; }

// Generation
void GenerateLocal(bool bForce);                                                    // UFUNCTION, delayed
void Generate(bool bForce);                                                         // NetMulticast Reliable UFUNCTION

// Cleanup
void CleanupLocal(bool bRemoveComponents, bool bSave = false);                      // UFUNCTION, delayed
void Cleanup(bool bRemoveComponents, bool bSave = false);                           // NetMulticast Reliable UFUNCTION
void CleanupLocalImmediate(bool bRemoveComponents, bool bCleanupLocalComponents = false);
void CancelGeneration();

// State
bool IsGenerating() const { return CurrentGenerationTask != InvalidPCGTaskId; }
bool IsCleaningUp() const { return CurrentCleanupTask != InvalidPCGTaskId; }
bool IsPartitioned() const;
bool IsLocalComponent() const { return bIsComponentLocal; }

// Output
const FPCGDataCollection& GetGeneratedGraphOutput() const { return GeneratedGraphOutput; }

// Subsystem access
UPCGSubsystem* GetSubsystem() const;

// Task ID (for chaining async work)
FPCGTaskId GetGenerationTaskId() const { return CurrentGenerationTask; }
FPCGTaskId GenerateLocalGetTaskId(bool bForce);
```

#### Key serialized properties

```cpp
UPROPERTY(BlueprintReadWrite, EditAnywhere) int Seed = 42;
UPROPERTY(BlueprintReadWrite, EditAnywhere) bool bActivated = true;
UPROPERTY(BlueprintReadWrite, EditAnywhere) bool bIsComponentPartitioned = false;
UPROPERTY(BlueprintReadOnly,  EditAnywhere) EPCGComponentGenerationTrigger GenerationTrigger;
UPROPERTY(BlueprintReadWrite, EditAnywhere) TArray<FName> PostGenerateFunctionNames;
```

#### Delegates

```cpp
FOnPCGGraphStartGeneratingExternal  OnPCGGraphStartGeneratingExternal;
FOnPCGGraphCancelledExternal        OnPCGGraphCancelledExternal;
FOnPCGGraphGeneratedExternal        OnPCGGraphGeneratedExternal;
FOnPCGGraphCleanedExternal          OnPCGGraphCleanedExternal;
```

---

### 8. UPCGSubsystem (PCGSubsystem.h)

`UPCGSubsystem : UTickableWorldSubsystem`. World-level singleton. Manages the graph executor,
partition actors, and the runtime generation scheduler.

#### Getting the subsystem

```cpp
// From a World pointer:
static UPCGSubsystem* GetInstance(UWorld* World);

// Editor shortcut (PIE world if active, else editor world):
static UPCGSubsystem* GetActiveEditorInstance();          // WITH_EDITOR only

// When no World is available (component unregistering):
static UPCGSubsystem* GetSubsystemForCurrentWorld();

// Standard UE pattern via UPCGComponent:
UPCGSubsystem* Subsystem = Component->GetSubsystem();
```

#### Key scheduling/execution methods

```cpp
// Schedule a component's graph for generation
FPCGTaskId ScheduleComponent(UPCGComponent* PCGComponent, EPCGHiGenGrid Grid,
                              bool bForce, const TArray<FPCGTaskId>& InDependencies);

// Schedule cleanup
FPCGTaskId ScheduleCleanup(UPCGComponent* PCGComponent, bool bRemoveComponents,
                            const TArray<FPCGTaskId>& Dependencies);

// Schedule a graph directly (used for dynamic subgraph execution)
FPCGTaskId ScheduleGraph(UPCGGraph* Graph, UPCGComponent* SourceComponent,
                          FPCGElementPtr PreGraphElement, FPCGElementPtr InputElement,
                          const TArray<FPCGTaskId>& Dependencies,
                          const FPCGStack* InFromStack, bool bAllowHierarchicalGeneration);

// Generic async task scheduling
FPCGTaskId ScheduleGeneric(TFunction<bool()> InOperation,
                            UPCGComponent* SourceComponent,
                            const TArray<FPCGTaskId>& TaskExecutionDependencies);

// Retrieve output data from a scheduled task
bool GetOutputData(FPCGTaskId InTaskId, FPCGDataCollection& OutData);
void ClearOutputData(FPCGTaskId InTaskId);

// Cancel
void CancelGeneration(UPCGComponent* Component);
void CancelGeneration(UPCGGraph* Graph);
void CancelAllGeneration();

// State queries
bool IsGraphCurrentlyExecuting(UPCGGraph* Graph);
bool IsAnyGraphCurrentlyExecuting() const;
bool IsInitialized() const { return GraphExecutor != nullptr; }
bool IsGraphCacheDebuggingEnabled() const;

// Cache
void FlushCache();

// Editor-only
void NotifyGraphChanged(UPCGGraph* InGraph, EPCGChangeType ChangeType);  // WITH_EDITOR
FPCGTaskId ScheduleRefresh(UPCGComponent* SourceComponent, bool bForceRefresh); // WITH_EDITOR
```

#### Editor-only delegates (WITH_EDITOR)

```cpp
FPCGOnPCGComponentUnregistered    OnPCGComponentUnregistered;
FPCGOnPCGComponentGenerationDone  OnPCGComponentGenerationDone;   // (UPCGSubsystem*, UPCGComponent*, EPCGGenerationStatus)
```

---

## API Changes / Deprecations in UE 5.5

The following were deprecated in 5.5 (marked with `UE_DEPRECATED(5.5, ...)`):

```cpp
// UPCGSubsystem — use FPCGGridDescriptor overloads instead:
UE_DEPRECATED(5.5) UPCGComponent* GetLocalComponent(uint32 GridSize, ...);
UE_DEPRECATED(5.5) APCGPartitionActor* GetRegisteredPCGPartitionActor(uint32 GridSize, ...);
UE_DEPRECATED(5.5) APCGPartitionActor* FindOrCreatePCGPartitionActor(const FGuid&, uint32 GridSize, ...);
UE_DEPRECATED(5.5) FPCGTaskId ForAllOverlappingCells(const FBox& ...) // use UPCGComponent* version

// UPCGSettings:
UE_DEPRECATED(5.5) virtual bool UseSeed() const     // implement the PCGSettings virtual override instead
UE_DEPRECATED(5.4) virtual FName AdditionalTaskName() // replaced by GetAdditionalTitleInformation()
```

---

## Module / Build Dependency

To use these APIs, add to your module's `Build.cs`:

```csharp
PrivateDependencyModuleNames.AddRange(new string[]
{
    "PCG",
    // For editor-only graph manipulation:
    "PCGEditor",   // optional, only if using PCG editor graph (UPCGEditorGraph)
});
```

The module name for the PCG plugin is `"PCG"` (not `"PCGRuntime"` or similar).
All the headers listed are in the `Public` folder and are safe to include directly.

---

## Recommendations

1. **Graph editing must happen on the game thread.** `UPCGGraph`, `UPCGNode`, and `UPCGPin` are `UObject` subclasses. All mutations require the game thread. Use `AsyncTask(ENamedThreads::GameThread, ...)` if dispatching from HTTP/MCP threads.

2. **Always wrap structural changes in a transaction.** Creating nodes, adding edges, setting subgraph references — all should be wrapped in `FScopedTransaction` and call `Graph->Modify()` (and `Node->Modify()` where applicable) before changes.

3. **Use `SetSubgraph()` not direct property assignment.** `UPCGSubgraphSettings::SetSubgraph(UPCGGraphInterface*)` sets up editor callbacks and refreshes pins. Directly assigning to `SubgraphInstance` bypasses this.

4. **Use `AddNodeOfType<T>(T*& OutSettings)` for typed creation.** The template overload (C++-only) is cleaner than `AddNodeOfType(TSubclassOf<>, UPCGSettings*&)` when the type is known at compile time.

5. **`AddEdge` vs `AddLabeledEdge`:** Prefer the UFUNCTION `AddEdge(From, FromPinLabel, To, ToPinLabel)` — it is the safe public API. `AddLabeledEdge` is used internally when you need the return value indicating whether an existing edge was displaced.

6. **`GetNodes()` excludes Input/Output nodes.** The `Nodes` array does not include `InputNode` or `OutputNode`. Use `GetInputNode()` and `GetOutputNode()` explicitly for those.

7. **`UPCGSubsystem` is not directly instantiated.** Use `UPCGSubsystem::GetInstance(World)` or `Component->GetSubsystem()`. In editor contexts, use `UPCGSubsystem::GetActiveEditorInstance()` (editor builds only).

8. **`EPCGDataType` is a `uint32` bitmask.** Do not cast to `uint8`. When setting `FPCGPinProperties::AllowedTypes`, use bitmask combinations like `EPCGDataType::Point | EPCGDataType::Param`.

9. **`EPCGDataType::Any` is the default.** For pins that accept everything, the default `FPCGPinProperties` already has `AllowedTypes = EPCGDataType::Any`.

10. **Triggering generation via `UPCGComponent::GenerateLocal(bool bForce)` is the standard pattern.** For editor-triggered regeneration use `Subsystem->ScheduleRefresh(Component, bForce)`. For tool-layer execution that needs a task ID, use `GenerateLocalGetTaskId(bool bForce)`.
