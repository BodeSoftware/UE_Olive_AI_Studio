# Research: Niagara Phase 2 — Parameter Overrides, Renderer Config, Emitter Properties

## Question

How do we programmatically set Niagara module parameter overrides, renderer properties, and emitter-level properties (SimTarget, bounds, etc.) in UE 5.5, without requiring the ViewModel stack?

---

## Findings

### 1. The Parameter Override Mechanism — Two Paths

When you change a module parameter value in the Niagara editor stack, exactly one of two things happens internally:

**Path A: Rapid Iteration Parameters (RIP)**
For numeric types (`float`, `FVector3f`, `FVector2f`, `FVector4f`, `FLinearColor`, `int32`, `FMatrix44f`) the value is stored directly in `UNiagaraScript::RapidIterationParameters` — a `FNiagaraParameterStore`. No override node is added to the graph. The engine re-reads these values each tick without recompilation, hence "rapid iteration."

**Path B: Override Pin / Override Node**
For enum types, bool, UObject/data interface references, and any type that `IsRapidIterationType()` returns `false` for, the value is stored as a `DefaultValue` string on an `UEdGraphPin` in a `UNiagaraNodeParameterMapSet` override node that is inserted before the module's `UNiagaraNodeFunctionCall` in the graph.

`FNiagaraStackGraphUtilities::IsRapidIterationType()` is the gate:
```cpp
// NiagaraEditor/Private/ViewModels/Stack/NiagaraStackGraphUtilities.cpp:2624
bool FNiagaraStackGraphUtilities::IsRapidIterationType(const FNiagaraTypeDefinition& InputType)
{
    if (InputType.IsStatic()) return true;
    return InputType != FNiagaraTypeDefinition::GetBoolDef() && (!InputType.IsEnum()) &&
        InputType != FNiagaraTypeDefinition::GetParameterMapDef() && !InputType.IsUObject();
}
```

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Private/ViewModels/Stack/NiagaraStackGraphUtilities.cpp`

---

### 2. The Gold Standard — `NiagaraEmitterFactoryNew.cpp`'s `SetRapidIterationParameter`

This is exactly the pattern Olive Phase 2 should copy. Epic uses it internally to set default values at emitter creation time, with no ViewModel required.

```cpp
// NiagaraEditor/Private/NiagaraEmitterFactoryNew.cpp:113–125
template<typename ValueType>
void SetRapidIterationParameter(FString UniqueEmitterName, UNiagaraScript& TargetScript,
    UNiagaraNodeFunctionCall& TargetFunctionCallNode,
    FName InputName, FNiagaraTypeDefinition InputType, ValueType Value)
{
    FNiagaraParameterHandle InputHandle =
        FNiagaraParameterHandle::CreateModuleParameterHandle(InputName);
    FNiagaraParameterHandle AliasedInputHandle =
        FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, &TargetFunctionCallNode);
    FNiagaraVariable RapidIterationParameter =
        FNiagaraStackGraphUtilities::CreateRapidIterationParameter(
            UniqueEmitterName, TargetScript.GetUsage(),
            AliasedInputHandle.GetParameterHandleString(), InputType);
    RapidIterationParameter.SetValue(Value);
    bool bAddParameterIfMissing = true;
    TargetScript.RapidIterationParameters.SetParameterData(
        RapidIterationParameter.GetData(), RapidIterationParameter, bAddParameterIfMissing);
}
```

Usage example (from the same file, lines 207–214):
```cpp
// SpawnRate = 10.0f on the SpawnRate module in EmitterUpdate stage
SetRapidIterationParameter(
    NewEmitter->GetUniqueEmitterName(),
    *EmitterData->EmitterUpdateScriptProps.Script,
    *SpawnRateNode,
    "SpawnRate",
    FNiagaraTypeDefinition::GetFloatDef(),
    10.0f);

// Velocity = FVector3f(0,0,100) on AddVelocity module in ParticleSpawn stage
SetRapidIterationParameter(
    NewEmitter->GetUniqueEmitterName(),
    *EmitterData->SpawnScriptProps.Script,
    *AddVelocityNode,
    "Velocity",
    FNiagaraTypeDefinition::GetVec3Def(),
    FVector3f(0.0f, 0.0f, 100.0f));
```

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Private/NiagaraEmitterFactoryNew.cpp`

---

### 3. Getting the Right Script for a Stage

Each stage maps to a specific `UNiagaraScript*` via `FVersionedNiagaraEmitterData`:

| Stage | Property on `FVersionedNiagaraEmitterData` |
|---|---|
| `EmitterSpawnScript` | `EmitterSpawnScriptProps.Script` |
| `EmitterUpdateScript` | `EmitterUpdateScriptProps.Script` |
| `ParticleSpawnScript` | `SpawnScriptProps.Script` |
| `ParticleUpdateScript` | `UpdateScriptProps.Script` |

To get the data:
```cpp
const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
FVersionedNiagaraEmitterData* EmitterData = Handles[EmitterIndex].GetEmitterData();
// Use EmitterData->SpawnScriptProps.Script, etc.
```

For system-level stages: `System->GetSystemSpawnScript()` / `System->GetSystemUpdateScript()`.

The emitter's unique name (required by `CreateRapidIterationParameter`) is:
```cpp
Handles[EmitterIndex].GetInstance().Emitter->GetUniqueEmitterName()
```

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitter.h`

---

### 4. FNiagaraParameterHandle — How It Maps to Overrides

`FNiagaraParameterHandle` wraps a namespaced parameter name like `"module.SpawnRate"` or `"Emitter.SpawnRate.SpawnRate"` (after aliasing). There are two key static constructors used in the pattern:

```cpp
// Creates "module.InputName" — the un-aliased module-scope handle
FNiagaraParameterHandle::CreateModuleParameterHandle(FName InName)

// Aliases "module.InputName" → "Emitter.FunctionCallDisplayName.InputName"
// Takes the module node's display name (FunctionDisplayName) to build the full path
FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
    const FNiagaraParameterHandle& ModuleParameterHandle,
    const UNiagaraNodeFunctionCall* ModuleNode)
```

The aliased handle string is what's used as the key in `RapidIterationParameters`. The rapid iteration variable name is then further transformed by `FNiagaraParameterUtilities::ConvertVariableToRapidIterationConstantName()` which bakes in the emitter name and script usage.

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Public/ViewModels/Stack/NiagaraParameterHandle.h`

---

### 5. FNiagaraTypeDefinition — Supported Types

The type definitions map directly to C++ types:

| FNiagaraTypeDefinition | C++ value type | Notes |
|---|---|---|
| `GetFloatDef()` | `float` | |
| `GetVec2Def()` | `FVector2f` | Must be float variant, NOT double |
| `GetVec3Def()` | `FVector3f` | |
| `GetVec4Def()` | `FVector4f` | |
| `GetColorDef()` | `FLinearColor` | |
| `GetIntDef()` | `int32` | |
| `GetBoolDef()` | `FNiagaraBool` | Bool uses override-pin path, NOT RIP |
| `GetMatrix4Def()` | `FMatrix44f` | |

For enums: NOT RIP-eligible. Must use the override-pin path (Path B above).
For UObject/DataInterface: NOT RIP-eligible. Use `SetObjectAssetValueForFunctionInput` or `SetDataInterfaceValueForFunctionInput` on the override pin.

**CRITICAL**: The factory template function has a `static_assert(!TIsUECoreVariant<ValueType, double>::Value)` — always use `FVector3f`, not `FVector3d` / `FVector`. Niagara's internal types are float-precision.

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Public/NiagaraTypes.h` lines 982–991, 1168–1177

---

### 6. FNiagaraVariable — Setting Values

```cpp
// float
FNiagaraVariable Var(FNiagaraTypeDefinition::GetFloatDef(), NAME_None);
Var.SetValue(42.0f);                  // template<typename T> void SetValue(const T& Data)

// FVector3f
FNiagaraVariable Var(FNiagaraTypeDefinition::GetVec3Def(), NAME_None);
Var.SetValue(FVector3f(1.0f, 2.0f, 3.0f));

// int32
FNiagaraVariable Var(FNiagaraTypeDefinition::GetIntDef(), NAME_None);
Var.SetValue(100);

// Reading back raw bytes (for SetParameterData)
Var.GetData()                         // const uint8*
Var.AllocateData()                    // ensures VarData is sized

// FNiagaraBool (for booleans via override pin)
FNiagaraBool B; B.SetValue(true);
```

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Public/NiagaraTypes.h` lines 1720–1810

---

### 7. Override-Pin Path (for Bool, Enum, Non-RIP types)

When `IsRapidIterationType()` is false, the value must be set as a string on an override pin. The override pin is on a `UNiagaraNodeParameterMapSet` node that gets inserted before the module node. This is the graph-level approach:

```cpp
// Get or create the override pin
FNiagaraParameterHandle InputHandle =
    FNiagaraParameterHandle::CreateModuleParameterHandle(FName("MyBoolParam"));
FNiagaraParameterHandle AliasedHandle =
    FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, &ModuleNode);

// InputType and InputScriptVariableId needed:
FNiagaraTypeDefinition InputType = FNiagaraTypeDefinition::GetBoolDef();
// Get the variable GUID from the called graph's metadata:
UNiagaraGraph* CalledGraph = ModuleNode.GetCalledGraph();
if (CalledGraph)
{
    FNiagaraVariable InputVar(InputType, FName("MyBoolParam"));
    TOptional<FNiagaraVariableMetaData> Meta = CalledGraph->GetMetaData(InputVar);
    FGuid VarGuid = Meta.IsSet() ? Meta->GetVariableGuid() : FGuid();

    UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
        ModuleNode, AliasedHandle, InputType, VarGuid, FGuid());

    // Set the string value (for bool: "true" or "false", for enum: integer string)
    const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
    FNiagaraVariable LocalVar(InputType, NAME_None);
    FNiagaraBool BoolVal; BoolVal.SetValue(true);
    LocalVar.SetData((const uint8*)&BoolVal);
    FString PinDefault;
    NiagaraSchema->TryGetPinDefaultValueFromNiagaraVariable(LocalVar, PinDefault);

    OverridePin.Modify();
    OverridePin.DefaultValue = PinDefault;
    Cast<UNiagaraNode>(OverridePin.GetOwningNode())->MarkNodeRequiresSynchronization(
        TEXT("Parameter override set"), true);
}
```

This path is SIGNIFICANTLY more complex than RIP. **Recommendation: only implement for bool and enum in Phase 2; defer DataInterface/UObject to Phase 3.**

Source: `NiagaraStackFunctionInput.cpp` lines 2162–2200 (SetLocalValue's non-RIP branch)
Header: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Public/EdGraphSchema_Niagara.h` line 147

---

### 8. Discovering Module Parameters (for read_module_parameters tool)

To enumerate the inputs on a module node:

```cpp
// Returns all FNiagaraVariable inputs (without needing a compile constant resolver)
void FNiagaraStackGraphUtilities::GetStackFunctionInputs(
    const UNiagaraNodeFunctionCall& FunctionCallNode,
    TArray<FNiagaraVariable>& OutInputVariables,
    ENiagaraGetStackFunctionInputPinsOptions Options,  // AllInputs or ModuleInputsOnly
    bool bIgnoreDisabled = false);
```

To read a current RIP value back:
```cpp
FNiagaraParameterHandle InputHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(FName("SpawnRate"));
FNiagaraParameterHandle Aliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, &ModuleNode);
FNiagaraVariable RipVar = FNiagaraStackGraphUtilities::CreateRapidIterationParameter(
    UniqueEmitterName, Script->GetUsage(), Aliased.GetParameterHandleString(), InputType);
const uint8* Data = Script->RapidIterationParameters.GetParameterData(RipVar);
if (Data) { float Val = *reinterpret_cast<const float*>(Data); }
```

Source: `NiagaraStackGraphUtilities.h` lines 122–127

---

### 9. Renderer Properties — Direct UPROPERTY Manipulation

Renderer properties (`UNiagaraSpriteRendererProperties`, `UNiagaraMeshRendererProperties`, etc.) are plain `UObject` subclasses of `UNiagaraRendererProperties`. All visual configuration is exposed as `UPROPERTY` fields — **no special setter API is required**. Use `FindFProperty<>()` + property reflection, or simply cast and set fields directly:

```cpp
// Access renderers on emitter (from FVersionedNiagaraEmitterData)
const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
for (UNiagaraRendererProperties* Renderer : Renderers)
{
    Renderer->Modify();  // Must call before mutation
    if (UNiagaraSpriteRendererProperties* Sprite =
        Cast<UNiagaraSpriteRendererProperties>(Renderer))
    {
        Sprite->Material = MyMaterial;                          // UMaterialInterface*
        Sprite->Alignment = ENiagaraSpriteAlignment::Automatic;
        Sprite->FacingMode = ENiagaraSpriteFacingMode::FaceCamera;
        Sprite->SortMode = ENiagaraSortMode::None;
        // etc. — all are UPROPERTY fields
    }
    Renderer->SetIsEnabled(true);  // NIAGARA_API virtual
}
```

To add a new renderer:
```cpp
UNiagaraSpriteRendererProperties* NewRenderer =
    NewObject<UNiagaraSpriteRendererProperties>(Emitter, "Renderer");
Emitter->AddRenderer(NewRenderer, EmitterData->Version.VersionGuid);  // NIAGARA_API
```

Key renderer headers:
- `NiagaraSpriteRendererProperties.h` — Material, Alignment, FacingMode, SortMode, SubImageSize
- `NiagaraMeshRendererProperties.h` — Mesh, Material overrides
- `NiagaraRibbonRendererProperties.h` — Material, UV settings
- `NiagaraLightRendererProperties.h` — Radius, Color binding

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Public/NiagaraSpriteRendererProperties.h`
Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitter.h` lines 924–926

---

### 10. Emitter-Level Properties

All emitter configuration lives in `FVersionedNiagaraEmitterData` (accessed via `Emitter->GetLatestEmitterData()` or `Handles[i].GetEmitterData()`). All are `UPROPERTY` fields — set them directly after calling `Emitter->Modify()`:

```cpp
FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
Emitter->Modify();

// Simulation target (CPU vs GPU)
EmitterData->SimTarget = ENiagaraSimTarget::CPUSim;     // or GPUComputeSim

// Bounds mode
EmitterData->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
EmitterData->FixedBounds = FBox(FVector(-200), FVector(200));

// Allocation
EmitterData->AllocationMode = EParticleAllocationMode::AutomaticEstimate;
EmitterData->PreAllocationCount = 500;

// Determinism / seeding
EmitterData->bDeterminism = true;
EmitterData->RandomSeed = 42;

// GPU particles limit
EmitterData->MaxGPUParticlesSpawnPerFrame = 10000;
```

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitter.h` lines 279–352

---

### 11. Required Headers for Phase 2

In addition to Phase 1 headers, Phase 2 needs:

```cpp
// For RIP parameter setting (already included in writer)
#include "NiagaraParameterHandle.h"          // FNiagaraParameterHandle
#include "NiagaraStackGraphUtilities.h"      // CreateRapidIterationParameter, GetStackFunctionInputs
#include "NiagaraTypes.h"                    // FNiagaraTypeDefinition, FNiagaraVariable

// For override-pin path (bool/enum)
#include "EdGraphSchema_Niagara.h"           // UEdGraphSchema_Niagara::TryGetPinDefaultValueFromNiagaraVariable
#include "NiagaraGraph.h"                    // GetMetaData()

// For renderer manipulation
#include "NiagaraRendererProperties.h"       // UNiagaraRendererProperties base
#include "NiagaraSpriteRendererProperties.h" // UNiagaraSpriteRendererProperties
// (add mesh/ribbon/light as needed)
```

All headers are in the `NiagaraEditor` module (already a Phase 1 Build.cs dependency).

---

### 12. Compilation After Parameter Changes

**RIP changes do NOT require recompilation** — they are read at runtime from the parameter store. They are effective immediately after calling `SetParameterData()`.

**Override-pin changes require recompile** — calling `MarkNodeRequiresSynchronization()` marks the node dirty, and `System->RequestCompile(false)` must be called afterward.

**Renderer changes require recompile** — call `System->RequestCompile(false)` after modifying renderer properties.

**Emitter-level property changes (SimTarget, bounds)** — call `System->RequestCompile(false)`.

---

### 13. Module Parameter ID Strategy

The current writer uses `"emitter_N.StageName.module_M"` as the module ID. For Phase 2, the AI needs to reference both the module and the specific parameter name. The recommended schema for the tool is:

```
module_id:    "emitter_0.ParticleUpdate.module_2"
param_name:   "SpawnRate"          (the bare input name, without namespace)
value:        "42.0"               (as string, parsed by type)
value_type:   "float"              (optional hint for disambiguation)
```

The writer resolves `param_name` to the correct `FNiagaraTypeDefinition` by calling `GetStackFunctionInputs()` on the module node, matching by name, then dispatching to RIP or override-pin accordingly.

---

## Recommendations

1. **Implement RIP path first.** Float, vec2/3/4, color, and int32 cover 90% of practical module parameter use cases (SpawnRate, Velocity, Lifetime, Color, Size). This is 20 lines of code following the factory template exactly. The override-pin path is 5x more complex — defer to a later PR.

2. **Copy the factory template verbatim.** `NiagaraEmitterFactoryNew.cpp`'s `SetRapidIterationParameter` static template is the authoritative, tested pattern. Do not invent a different approach.

3. **The script for each stage is on FVersionedNiagaraEmitterData.** Fix the gap in the current writer: `EmitterData->EmitterSpawnScriptProps.Script`, `EmitterData->EmitterUpdateScriptProps.Script`, `EmitterData->SpawnScriptProps.Script`, `EmitterData->UpdateScriptProps.Script`. The current writer has a placeholder `GetGPUComputeScript()` for EmitterSpawn which is wrong.

4. **RIP changes are live without recompile. Communicate this.** The tool's response should say "parameter updated, no recompile needed" for RIP types. For override-pin/renderer/emitter-level changes, trigger compile.

5. **GetStackFunctionInputs for parameter discovery.** Use `FNiagaraStackGraphUtilities::GetStackFunctionInputs(..., ModuleInputsOnly)` to enumerate what parameters a module exposes. This is the basis for a `niagara.list_module_parameters` read tool.

6. **Renderer properties are plain UObject UPROPERTY fields.** No special API. Cast to the concrete type and set fields directly. Use reflection (`FindFProperty<>` + `ExportText`) if you want a generic string-value setter that works across all renderer types.

7. **FVector3f not FVector.** Niagara's internal vector type is `FVector3f` (float). Passing `FVector` (double in UE5) will compile but the `static_assert` in the factory template will catch it. Always use the `f`-suffix variants: `FVector3f`, `FVector2f`, `FVector4f`.

8. **UniqueEmitterName is mandatory for RIP.** The rapid iteration parameter key is namespaced by emitter name. Get it from `Handles[i].GetInstance().Emitter->GetUniqueEmitterName()`. Using the wrong name silently creates a parameter that the script never reads.

9. **GetOrCreateStackFunctionInputOverridePin signature requires a script variable GUID.** Source it from `ModuleNode.GetCalledGraph()->GetMetaData(InputVar)->GetVariableGuid()`. If the graph has no metadata entry for that variable, pass `FGuid()` as fallback — the override node will still be created but ParameterGuidMapping won't be set.

10. **AddParameterModuleToStack is a different op.** `FNiagaraStackGraphUtilities::AddParameterModuleToStack()` adds an Assignment node to set particle/emitter attributes (like `Particles.SpriteSize`). This is NOT for setting module inputs — it's for writing output parameters. Don't confuse it with the RIP path.

---

Sources:
- [Using Niagara in C++ (Epic Dev Community)](https://dev.epicgames.com/community/learning/tutorials/Gx5j/using-niagara-in-c)
- [Niagara API Reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/Niagara)
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Private/NiagaraEmitterFactoryNew.cpp`
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Private/ViewModels/Stack/NiagaraStackGraphUtilities.cpp`
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Private/ViewModels/Stack/NiagaraStackFunctionInput.cpp`
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Public/NiagaraTypes.h`
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitter.h`
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Public/NiagaraRendererProperties.h`
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Public/NiagaraSpriteRendererProperties.h`
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Public/ViewModels/Stack/NiagaraStackFunctionInput.h`
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Public/ViewModels/Stack/NiagaraParameterHandle.h`
