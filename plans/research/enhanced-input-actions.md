# Research: Enhanced Input Actions (UInputAction, UInputMappingContext)

## Question
Can Olive AI Studio create and configure Enhanced Input Action (EIA) assets programmatically?
What classes, APIs, and modules are involved? Do existing tools cover this, and what new tools (if any) are needed?

---

## Findings

### 1. UInputAction — Class Definition

**Header:** `C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/EnhancedInput/Source/EnhancedInput/Public/InputAction.h`
**Module:** `EnhancedInput`
**Base class:** `UDataAsset` (not UBlueprint — this is a plain data asset)

Key UPROPERTY fields:

```cpp
UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = Action, AssetRegistrySearchable)
EInputActionValueType ValueType = EInputActionValueType::Boolean;

UPROPERTY(EditAnywhere, Instanced, BlueprintReadWrite, Category = Action)
TArray<TObjectPtr<UInputTrigger>> Triggers;

UPROPERTY(EditAnywhere, Instanced, BlueprintReadWrite, Category = Action, meta=(DisplayAfter="Triggers"))
TArray<TObjectPtr<UInputModifier>> Modifiers;

UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Description")
FText ActionDescription = FText::GetEmpty();

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Action)
bool bTriggerWhenPaused = false;

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input Consumption")
bool bConsumeInput = true;
```

**EInputActionValueType** enum (in `InputActionValue.h`):
```cpp
enum class EInputActionValueType : uint8
{
    Boolean,   // "Digital (bool)"  — button press
    Axis1D,    // float — e.g., throttle
    Axis2D,    // FVector2D — e.g., mouse XY
    Axis3D,    // FVector — e.g., 3D motion
};
```

**Creating a UInputAction programmatically:**

The factory from `InputEditorModule.cpp` shows the minimal pattern:
```cpp
// Option A: via IAssetTools (preferred for editor asset creation with proper registration)
IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UInputAction::StaticClass(), nullptr);
UInputAction* IA = Cast<UInputAction>(NewAsset);
IA->ValueType = EInputActionValueType::Boolean;
// Triggers and Modifiers arrays are empty by default — valid for simple press actions

// Option B: via the engine's factory object (same result, bypasses dialog)
// UInputAction_Factory* Factory = NewObject<UInputAction_Factory>();
// Factory->InputActionClass = UInputAction::StaticClass();
// UInputAction* IA = Cast<UInputAction>(Factory->FactoryCreateNew(
//     UInputAction::StaticClass(), Package, Name, RF_Transactional, nullptr, GWarn));
```

`UInputAction_Factory` is defined in `InputEditorModule.cpp` (private to `InputEditor` module). Do NOT instantiate it — use `AssetTools.CreateAsset()` with `nullptr` factory, which invokes the registered factory internally.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/EnhancedInput/Source/InputEditor/Private/InputEditorModule.cpp` lines 163–216

---

### 2. UInputMappingContext — Class Definition

**Header:** `C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/EnhancedInput/Source/EnhancedInput/Public/InputMappingContext.h`
**Module:** `EnhancedInput`
**Base class:** `UDataAsset`

The `Mappings` array is `protected` — not directly writable from outside. Use the public API:

```cpp
// Add a key mapping to an IMC (returns reference to the new mapping):
FEnhancedActionKeyMapping& MapKey(const UInputAction* Action, FKey ToKey);

// Remove one mapping:
void UnmapKey(const UInputAction* Action, FKey Key);

// Remove all mappings for an action:
void UnmapAllKeysFromAction(const UInputAction* Action);

// Remove all mappings:
void UnmapAll();

// Read all mappings (const):
const TArray<FEnhancedActionKeyMapping>& GetMappings() const { return Mappings; }
FEnhancedActionKeyMapping& GetMapping(SizeType Index);
```

**Creating a UInputMappingContext programmatically:**
```cpp
IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UInputMappingContext::StaticClass(), nullptr);
UInputMappingContext* IMC = Cast<UInputMappingContext>(NewAsset);

// Wire the action to a key:
FKey JumpKey = EKeys::SpaceBar;
FEnhancedActionKeyMapping& Mapping = IMC->MapKey(IA_Jump, JumpKey);
// Optionally add per-mapping modifiers/triggers to the returned Mapping reference
```

Source: `InputMappingContext.h` lines 53–85; `InputEditorModule.cpp` lines 88–154

---

### 3. FEnhancedActionKeyMapping — The Key→Action Binding Struct

**Header:** `EnhancedActionKeyMapping.h`
**Module:** `EnhancedInput`

```cpp
USTRUCT(BlueprintType)
struct ENHANCEDINPUT_API FEnhancedActionKeyMapping
{
    TObjectPtr<const UInputAction> Action = nullptr;  // The action
    FKey Key;                                          // The key (e.g., EKeys::SpaceBar)
    TArray<TObjectPtr<UInputTrigger>> Triggers;        // Per-mapping triggers (applied before action triggers)
    TArray<TObjectPtr<UInputModifier>> Modifiers;      // Per-mapping modifiers (applied before action modifiers)
    // ...
};
```

The `Mapping` reference returned by `MapKey()` can be used to add per-mapping triggers and modifiers. Key names are `FKey` constants: `EKeys::SpaceBar`, `EKeys::W`, `EKeys::MouseX`, etc.

Source: `EnhancedActionKeyMapping.h` lines 82–265

---

### 4. UEnhancedInputLocalPlayerSubsystem — Runtime Registration

**Header:** `EnhancedInputSubsystems.h` / `EnhancedInputSubsystemInterface.h`
**Module:** `EnhancedInput`

Used at runtime (not editor) to activate an IMC for a local player:

```cpp
// In BeginPlay (Blueprint or C++):
if (APlayerController* PC = Cast<APlayerController>(GetController()))
{
    if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
        ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
    {
        FModifyContextOptions Options;
        Subsystem->AddMappingContext(IMC, Priority, Options);
    }
}
```

This is a runtime Blueprint call, not an editor asset operation. In Blueprint graphs, it maps to a node calling `AddMappingContext` on the subsystem. Priority (int32) determines order; higher priority = evaluated first.

---

### 5. UK2Node_EnhancedInputAction — The Blueprint Event Node

**Header:** `C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/EnhancedInput/Source/InputBlueprintNodes/Public/K2Node_EnhancedInputAction.h`
**Module:** `InputBlueprintNodes`

Key property:
```cpp
UPROPERTY()
TObjectPtr<const UInputAction> InputAction;  // MUST be set before AllocateDefaultPins()
```

**This node IS already supported** by our `K2Node_EnhancedInputAction` path in `OliveNodeFactory`. It is placed in event graphs (ubergraphs only — `IsCompatibleWithGraph` checks `GT_Ubergraph`).

**Pin names** (exec output pins, named after `ETriggerEvent` enum values):
- `Triggered` — fires every frame while held (most common)
- `Started` — fires once when key is first pressed
- `Ongoing` — fires every frame during evaluation (before Triggered)
- `Canceled` — fires if action was interrupted
- `Completed` — fires when action ends (key released after Triggered)

`VisibleEventPinsByDefault` in `UEnhancedInputEditorSettings` controls which pins appear collapsed. By default, only `Triggered` is visible (bitmask = 1).

**Data output pins:**
- `ActionValue` — type depends on `InputAction->ValueType` (bool/float/Vector2D/Vector)
- `ElapsedSeconds` — double (advanced, hidden by default)
- `TriggeredSeconds` — double (advanced, hidden by default)
- `InputAction` — UInputAction object reference (advanced, hidden by default)

**Critical:** `InputAction` property must be set before calling `AllocateDefaultPins()`. The node reads from `InputAction` to determine the `ActionValue` pin type. If `InputAction` is null, the node compiles with a warning and no data pins are generated.

Source: `K2Node_EnhancedInputAction.cpp` lines 46–496

---

### 6. UK2Node_EnhancedInputActionEvent — The Intermediate Event Node

**Header:** `K2Node_EnhancedInputActionEvent.h` (private — `InputBlueprintNodes` module)
**Base:** `UK2Node_Event`

```cpp
UPROPERTY()
TObjectPtr<const UInputAction> InputAction;

UPROPERTY()
ETriggerEvent TriggerEvent;
```

`UK2Node_EnhancedInputAction` is the user-visible node. During compilation, `ExpandNode()` spawns one `UK2Node_EnhancedInputActionEvent` per connected exec pin. We do not create `UK2Node_EnhancedInputActionEvent` directly.

---

### 7. Module Dependencies

For our `OliveAIEditor.Build.cs` to use these APIs:

| Need | Module to add |
|------|---------------|
| `UInputAction`, `UInputMappingContext`, `FEnhancedActionKeyMapping`, `EInputActionValueType` | `"EnhancedInput"` |
| `UK2Node_EnhancedInputAction` | `"InputBlueprintNodes"` |
| Editor-only asset creation (factory classes) | `"InputEditor"` (optional — we bypass factories with `IAssetTools`) |

The `UInputAction_Factory` and `UInputMappingContext_Factory` classes are in the `InputEditor` private `.cpp` file — they are not declared in any public header. Do NOT add a dependency on `InputEditor` to create these assets; use `AssetTools.CreateAsset(..., UInputAction::StaticClass(), nullptr)` instead.

Check: `EnhancedInput.Build.cs` (module `EnhancedInput`), `InputBlueprintNodes.Build.cs` (module `InputBlueprintNodes`).

---

### 8. Can `blueprint.create` Handle This?

**No.** `blueprint.create` delegates to `FOliveBlueprintWriter::CreateBlueprint()` which calls `FKismetEditorUtilities::CreateBlueprint()`. That function specifically creates `UBlueprint` assets with a generated class hierarchy. `UInputAction` and `UInputMappingContext` are `UDataAsset` subclasses — they have no Blueprint graph, no generated class, and no `FKismetEditorUtilities` involvement.

Passing `"InputAction"` as `parent_class` to `blueprint.create` would fail because `UInputAction` is not a valid `UBlueprint` parent class.

Source: `OliveBlueprintToolHandlers.cpp` lines 1778–1968

---

### 9. Can `editor.run_python` Handle This?

**Yes — this is the best current option** and requires no new C++ code.

UE's Python scripting API exposes all `UCLASS(BlueprintType)` / `UPROPERTY(BlueprintReadWrite)` types. `UInputAction` and `UInputMappingContext` are both `BlueprintType` and have `BlueprintReadWrite` properties. The Python API also wraps `IAssetTools`.

Python example (runnable via `editor.run_python`):

```python
import unreal

# Create a UInputAction asset
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
ia = asset_tools.create_asset(
    asset_name="IA_Jump",
    package_path="/Game/Input",
    asset_class=unreal.InputAction,
    factory=None
)
ia.value_type = unreal.InputActionValueType.BOOLEAN
unreal.EditorAssetLibrary.save_asset(ia.get_path_name())

# Create a UInputMappingContext asset
imc = asset_tools.create_asset(
    asset_name="IMC_Default",
    package_path="/Game/Input",
    asset_class=unreal.InputMappingContext,
    factory=None
)
# Map jump action to space bar
imc.map_key(ia, unreal.Key.space_bar)
unreal.EditorAssetLibrary.save_asset(imc.get_path_name())
```

Note: Python class name is `unreal.InputAction` (not `unreal.UInputAction`). Python property names are snake_case versions of C++ names (`value_type` for `ValueType`, etc.). The `map_key` method is a direct expose of `UInputMappingContext::MapKey()`.

**Caveat:** Python availability requires `IPythonScriptPlugin` to be loaded (guarded in our startup). The AI should confirm the Python tool is available before relying on it exclusively.

---

### 10. Does Any Cross-System Tool Handle Generic Asset Creation?

**No.** `OliveCrossSystemToolHandlers.cpp` registers: `project.bulk_read`, batch tools, snapshot tools, index tools, recipe tools, and community blueprint search. None create non-Blueprint data assets.

The BT writer (`OliveBlackboardWriter.cpp`, `OliveBehaviorTreeWriter.cpp`) uses the exact pattern needed for EIA creation:
```cpp
IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UBlackboardData::StaticClass(), nullptr);
```
This same pattern works identically for `UInputAction::StaticClass()` and `UInputMappingContext::StaticClass()`.

---

### 11. Minimum Viable EIA Setup — Full Sequence

For a character Blueprint to respond to a "Jump" key press:

**Step 1: Create IA_Jump asset**
```
path: /Game/Input/IA_Jump
class: UInputAction
ValueType = Boolean (default)
Triggers = [] (empty = default Triggered behavior)
```

**Step 2: Create IMC_Default asset**
```
path: /Game/Input/IMC_Default
class: UInputMappingContext
MapKey(IA_Jump, EKeys::SpaceBar)
```

**Step 3: Wire BeginPlay in character Blueprint**
```
Event BeginPlay
  → GetPlayerController
  → GetLocalPlayer
  → GetSubsystem<UEnhancedInputLocalPlayerSubsystem>
  → AddMappingContext(IMC_Default, Priority=0)
```
In plan_json terms: event(BeginPlay) → call(GetPlayerController) → cast(APlayerController) → call(GetSubsystem, target_class=UEnhancedInputLocalPlayerSubsystem) → call(AddMappingContext, IMC_Default, 0)

**Step 4: Add enhanced input event node in character event graph**
```
UK2Node_EnhancedInputAction with InputAction = IA_Jump
Connect Triggered exec pin → Jump logic
```
In plan_json: `{"op": "event", "target": "IA_Jump"}` — BUT this only works if the `event` op resolver is extended to recognize EIA assets. Currently `ResolveEventOp` only checks `ImplementedInterfaces` and built-in events. See below.

---

### 12. Plan JSON Compatibility for EIA Event Nodes

The existing `event` op in `OlivePlanOps` maps to either `UK2Node_Event` or `UK2Node_ComponentBoundEvent`. It does NOT handle `UK2Node_EnhancedInputAction`.

To support `{"op": "event", "target": "IA_Jump"}` in plan_json, one of these would be needed:
- Extend `ResolveEventOp` to detect `UInputAction` asset references and route to a new `CreateEnhancedInputActionNode()` factory method, OR
- Add a new op like `enhanced_input_event` with an `input_action` property, OR
- Use `add_node` with `node_type="K2Node_EnhancedInputAction"` and a `properties` block setting `InputAction` — this already works via `CreateNodeByClass` if `InputAction` property assignment is handled.

The `add_node` / granular tool approach is the lowest-friction path since `UK2Node_EnhancedInputAction` is a standard UK2Node and our `CreateNodeByClass` supports arbitrary node types.

---

## Recommendations

### What works today (no new C++ needed)

1. **`editor.run_python`** can create `UInputAction` and `UInputMappingContext` assets and call `MapKey()` on them. This is the complete solution for asset creation tasks. The AI should use Python for Steps 1 and 2 of the minimum viable setup.

2. **`UK2Node_EnhancedInputAction` via `add_node`** — the granular `blueprint.add_node` tool with `node_type="K2Node_EnhancedInputAction"` should work if `CreateNodeByClass` correctly sets the `InputAction` UPROPERTY before `AllocateDefaultPins()`. This needs verification — the `InputAction` property is a `TObjectPtr<const UInputAction>` and must be loaded from the asset registry first.

3. **`blueprint.apply_plan_json`** can wire BeginPlay → GetPlayerController → AddMappingContext using standard `call` ops. `UEnhancedInputLocalPlayerSubsystem::AddMappingContext` is a `UFUNCTION(BlueprintCallable)` so `FindFunction` will locate it.

### What requires new C++ (if we want first-class tools)

4. **New tool: `input.create_action`** — Thin tool wrapping `IAssetTools::CreateAsset(UInputAction)` with params: `path`, `value_type` (Boolean/Axis1D/Axis2D/Axis3D). Follows the same pattern as `OliveBlackboardWriter::CreateBlackboard()`. Module dependency: add `"EnhancedInput"` to `OliveAIEditor.Build.cs`.

5. **New tool: `input.create_mapping_context`** — Creates `UInputMappingContext` asset with a `mappings` array param: `[{action_path, key}]`. Calls `IMC->MapKey()` in a loop.

6. **Plan_json `event` op extension** — To support `{"op": "event", "target": "IA_Jump"}`, extend `ResolveEventOp` to check if the target string matches a loaded `UInputAction` asset path, then return a step routing to `UK2Node_EnhancedInputAction`.

### Gotchas

- `UInputAction` and `UInputMappingContext` are `UDataAsset` subclasses — **not** Blueprints. `blueprint.create` cannot create them.
- `UK2Node_EnhancedInputAction` **requires** `InputAction` to be set before `AllocateDefaultPins()`. A null `InputAction` produces a node with no data output pins, which causes a compile error.
- `UK2Node_EnhancedInputAction` is **ubergraph-only** (`IsCompatibleWithGraph` enforces `GT_Ubergraph`). Do not place in function graphs.
- The node only fires when the Blueprint's `SupportsInputEvents()` returns true. Actor/Pawn/Character Blueprints support this; plain Object Blueprints do not.
- `FEnhancedActionKeyMapping::Triggers` and `Modifiers` (per-mapping, set on the IMC) are applied **before** `UInputAction::Triggers` and `Modifiers` (per-action, set on the IA asset).
- `EKeys::SpaceBar` in Python is `unreal.Key.space_bar` (snake_case). Other keys follow the same pattern: `W` → `unreal.Key.w`, `LeftMouseButton` → `unreal.Key.left_mouse_button`.
- The `InputBlueprintNodes` module must be added to `OliveAIEditor.Build.cs` to use `UK2Node_EnhancedInputAction` in C++. It is currently listed under `AnimGraph`/`AnimGraphRuntime` dependencies but not `InputBlueprintNodes` — verify before writing any C++ that touches this node type.

### Module additions required (if building C++ tools)

```csharp
// OliveAIEditor.Build.cs — add to PrivateDependencyModuleNames:
"EnhancedInput",          // UInputAction, UInputMappingContext
"InputBlueprintNodes",    // UK2Node_EnhancedInputAction (if needed in C++)
```

No `InputEditor` dependency is needed — asset creation bypasses the factory classes using `AssetTools.CreateAsset(..., nullptr)`.
