# Research: Niagara VFX Support Feasibility (UE 5.5)

## Question

What would be required to add a Niagara VFX module to the Olive AI Studio plugin (UE 5.5)? Research the C++ API surface, graph structure, programmatic creation/editing, module discovery, Python scripting options, and overall feasibility.

---

## Findings

### 1. Module Structure and Build.cs Dependencies

Niagara ships as a UE plugin at `Engine/Plugins/FX/Niagara/`. It has multiple source modules with distinct roles:

| Module | Type | Loading Phase | Role |
|---|---|---|---|
| `Niagara` | Runtime | PreDefault | Core runtime: `UNiagaraSystem`, `UNiagaraEmitter`, `UNiagaraComponent`, `UNiagaraScript`, data interfaces |
| `NiagaraCore` | Runtime | PostConfigInit | Low-level Niagara types, VVM, parameter stores |
| `NiagaraEditor` | Editor | Default | Node graph, ViewModels, stack utilities, factories, asset tools |
| `NiagaraEditorWidgets` | Editor | Default | Stack UI widgets (not needed by Olive) |

**For a new `OliveAIEditor/Niagara/` sub-module, the Build.cs additions would be:**

```csharp
// Required: core runtime objects
"Niagara",
"NiagaraCore",

// Required: graph editing, AddScriptModuleToStack, factories, GetFilteredScriptAssets
"NiagaraEditor",
```

The `NiagaraEditor` module loads at phase `Default`, while `OliveAIEditor` is `PostEngineInit` — the ordering is safe; Niagara will be ready by the time Olive initializes.

The PCG module is guarded by an availability check in `OliveAIEditorModule.cpp`. Niagara should use the same pattern since it's a plugin and could in theory be disabled.

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Niagara.uplugin`, `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Niagara.Build.cs`, `NiagaraEditor.Build.cs`

---

### 2. Key Header Locations

| Header | Module | Key Contents |
|---|---|---|
| `Niagara/Classes/NiagaraSystem.h` | Niagara | `UNiagaraSystem`, `AddEmitterHandle`, `RemoveEmitterHandle`, `RequestCompile`, `GetEmitterHandles` |
| `Niagara/Classes/NiagaraEmitter.h` | Niagara | `UNiagaraEmitter`, emitter data, renderer list, sim target |
| `Niagara/Classes/NiagaraScript.h` | Niagara | `UNiagaraScript`, `ENiagaraScriptUsage`, `ENiagaraScriptLibraryVisibility`, `GetUsage()` |
| `Niagara/Public/NiagaraCommon.h` | Niagara | `ENiagaraScriptUsage` enum (all 12 values), `FNiagaraVariable`, `FNiagaraTypeDefinition` |
| `NiagaraEditor/Public/NiagaraGraph.h` | NiagaraEditor | `UNiagaraGraph` (extends `UEdGraph`) |
| `NiagaraEditor/Public/NiagaraNode.h` | NiagaraEditor | `UNiagaraNode` (extends `UEdGraphNode`) |
| `NiagaraEditor/Public/NiagaraNodeFunctionCall.h` | NiagaraEditor | `UNiagaraNodeFunctionCall` — the "module node" in the stack graph |
| `NiagaraEditor/Public/NiagaraNodeOutput.h` | NiagaraEditor | `UNiagaraNodeOutput` — stack entry point for each execution context |
| `NiagaraEditor/Public/NiagaraEditorUtilities.h` | NiagaraEditor | `FNiagaraEditorUtilities::GetFilteredScriptAssets()`, `GetScriptOutputNode()`, `GetScriptFromSystem()` |
| `NiagaraEditor/Public/ViewModels/Stack/NiagaraStackGraphUtilities.h` | NiagaraEditor | `FNiagaraStackGraphUtilities::AddScriptModuleToStack()`, `RemoveModuleFromStack()`, `GetOrderedModuleNodes()`, `SetLinkedValueHandleForFunctionInput()` |
| `NiagaraEditor/Public/NiagaraSystemFactoryNew.h` | NiagaraEditor | `UNiagaraSystemFactoryNew::InitializeSystem()` |
| `NiagaraEditor/Public/NiagaraEmitterFactoryNew.h` | NiagaraEditor | `UNiagaraEmitterFactoryNew::InitializeEmitter()` |

---

### 3. Niagara Graph Structure — Critical Difference from Blueprint

Niagara uses a **parameter-map stack model**, not a free-form node graph like Blueprint K2. This is the most important architectural distinction.

**Conceptual model:**

```
UNiagaraSystem
  └── UNiagaraEmitter (via FNiagaraEmitterHandle)
        └── UNiagaraScript (one per execution stage: EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate...)
              └── UNiagaraGraph (extends UEdGraph)
                    ├── UNiagaraNodeInput   ("Parameter Map" source)
                    ├── UNiagaraNodeFunctionCall  (= a "module" in the stack)
                    ├── UNiagaraNodeFunctionCall  (= another module)
                    └── UNiagaraNodeOutput  (= stack execution context terminal)
```

**The `UNiagaraNodeOutput` is the key object for the stack.** Each execution stage (EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, etc.) has its own `UNiagaraNodeOutput` node in the graph, identified by `ScriptType` (an `ENiagaraScriptUsage` value). Modules in the stack are `UNiagaraNodeFunctionCall` nodes chained via a parameter-map pin chain ending at the `UNiagaraNodeOutput`.

`ENiagaraScriptUsage` enum values (relevant to modules):
- `EmitterSpawnScript`, `EmitterUpdateScript` — emitter lifecycle stages
- `ParticleSpawnScript`, `ParticleUpdateScript` — per-particle stages
- `SystemSpawnScript`, `SystemUpdateScript` — system-level stages
- `Module` — the usage of a reusable module asset (not a stack stage itself)

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Public/NiagaraCommon.h` (lines 1099–1160), `NiagaraNodeOutput.h`

---

### 4. Creating a Niagara System Programmatically

**Factory approach (correct method):**

```cpp
#include "NiagaraSystemFactoryNew.h"
#include "AssetToolsModule.h"

// Get factory
UNiagaraSystemFactoryNew* Factory = NewObject<UNiagaraSystemFactoryNew>();

// Create the asset via asset tools (handles saving, registry, etc.)
IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
UObject* NewAsset = AssetTools.CreateAsset(
    FPaths::GetBaseFilename(AssetPath),
    FPaths::GetPath(AssetPath),
    UNiagaraSystem::StaticClass(),
    Factory
);
UNiagaraSystem* System = Cast<UNiagaraSystem>(NewAsset);

// Initialize with default nodes (creates system-level graphs)
UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/true);
```

The factory's `FactoryCreateNew` internally calls `InitializeSystem` with default nodes, so using the factory directly via `AssetTools.CreateAsset` is the standard path. The static `InitializeSystem` is also `NIAGARAEDITOR_API` and callable directly if you already have an existing system.

Source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraSystemFactoryNew.h`

---

### 5. Adding an Emitter to a System

**Low-level C++ path (no ViewModel required):**

```cpp
// UNiagaraSystem::AddEmitterHandle — defined in NiagaraSystem.h
NIAGARA_API FNiagaraEmitterHandle AddEmitterHandle(
    UNiagaraEmitter& SourceEmitter,
    FName EmitterName,
    FGuid EmitterVersion   // use emitter->GetLatestEmitterData()->Version.VersionGuid
);

// Usage:
UNiagaraEmitter* SourceEmitter = /* create or load */;
System->Modify();
FNiagaraEmitterHandle Handle = System->AddEmitterHandle(
    *SourceEmitter,
    FName("MyEmitter"),
    SourceEmitter->GetLatestEmitterData()->Version.VersionGuid
);
System->RefreshSystemParametersFromEmitter(Handle);
System->RequestCompile(/*bForce=*/false);
```

**ViewModel path (higher-level, preferred when editor is open):**

```cpp
// FNiagaraSystemViewModel::AddEmitter — defined in NiagaraSystemViewModel.h
NIAGARAEDITOR_API TSharedPtr<FNiagaraEmitterHandleViewModel> AddEmitter(
    UNiagaraEmitter& Emitter,
    FGuid EmitterVersion
);
// or:
NIAGARAEDITOR_API TSharedPtr<FNiagaraEmitterHandleViewModel> AddEmitterFromAssetData(
    const FAssetData& AssetData
);
```

Creating an emitter from scratch:

```cpp
// UNiagaraEmitterFactoryNew::InitializeEmitter — static, NiagaraEditor_API
UNiagaraEmitter* NewEmitter = NewObject<UNiagaraEmitter>(Package, Name, RF_Public|RF_Standalone|RF_Transactional);
UNiagaraEmitterFactoryNew::InitializeEmitter(NewEmitter, /*bAddDefaultModulesAndRenderers=*/true);
```

Source: `NiagaraSystem.h` (lines 289, 308–321), `NiagaraSystemViewModel.h` (lines 241–245), `NiagaraEmitterFactoryNew.h`

---

### 6. Adding/Removing Modules — The Core Write Operation

**This is the primary API for stack manipulation:**

```cpp
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "NiagaraEditorUtilities.h"

// Step 1: Get the UNiagaraScript for the target stage.
// e.g., get the ParticleSpawn script for an emitter:
UNiagaraScript* Script = FNiagaraEditorUtilities::GetScriptFromSystem(
    *System, EmitterHandle.GetId(), ENiagaraScriptUsage::ParticleSpawnScript, FGuid()
);
// (FGuid() means "first script of this usage type")

// Step 2: Get the UNiagaraNodeOutput from the script's source graph.
UNiagaraNodeOutput* OutputNode = FNiagaraEditorUtilities::GetScriptOutputNode(*Script);

// Step 3: Load the module script asset.
UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr,
    TEXT("/Niagara/Modules/Spawn/SpawnRate.SpawnRate"));

// Step 4: Add the module to the stack at a given index (INDEX_NONE = append).
FScopedTransaction Transaction(INVTEXT("Add Niagara Module"));
UNiagaraNodeFunctionCall* NewModuleNode =
    FNiagaraStackGraphUtilities::AddScriptModuleToStack(
        ModuleScript,
        *OutputNode,
        INDEX_NONE,   // TargetIndex
        FString()     // SuggestedName (empty = use script's name)
    );

// Step 5: Compile.
Script->GetSource()->MarkNotSynchronized(TEXT("ModuleAdded"));
System->RequestCompile(/*bForce=*/false);
```

**Remove a module:**

```cpp
FNiagaraStackGraphUtilities::RemoveModuleFromStack(
    *System, EmitterHandle.GetId(), *ModuleNode
);
```

**Get ordered module nodes for a stage:**

```cpp
TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
FNiagaraStackGraphUtilities::GetOrderedModuleNodes(*OutputNode, ModuleNodes);
```

Source: `NiagaraStackGraphUtilities.h` (lines 54–278), `NiagaraStackGraphUtilities.cpp` (lines 2275–2430)

**CRITICAL:** `AddScriptModuleToStack` works directly on the `UNiagaraGraph` — no `FNiagaraSystemViewModel` is required. It creates `UNiagaraNodeFunctionCall` nodes via `FGraphNodeCreator<UNiagaraNodeFunctionCall>`, sets `FunctionScript`, and calls an internal `ConnectModuleNode()` to chain it into the parameter-map chain. This is stable C++ below the ViewModel layer.

---

### 7. Setting Module Parameters (Inputs)

Module parameter setting is more complex than in Blueprint. The override system works through a `UNiagaraNodeParameterMapSet` node that sits between the module node's parameter map input and the parameter map chain.

```cpp
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

// Get the aliased parameter handle for the input you want to set
// e.g., "Module.SpawnRate" on a SpawnRate module
FNiagaraParameterHandle AliasedHandle(TEXT("Module.SpawnRate"));

// Get/create the override pin for this input on the module node
UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
    *ModuleNode,
    AliasedHandle,
    FNiagaraTypeDefinition::GetFloatDef(),
    ScriptVariableId,        // from the module's UNiagaraScriptVariable
    FGuid()                  // PreferredOverrideNodeGuid — FGuid() = auto-generate
);

// Set a literal value by setting the pin's default value
OverridePin.DefaultValue = TEXT("50.0");

// Set a linked parameter binding:
FNiagaraStackGraphUtilities::SetLinkedValueHandleForFunctionInput(
    OverridePin,
    FNiagaraParameterHandle(TEXT("User.SpawnRate")),
    KnownParameters
);
```

**Important:** For simple literal values (float, int, bool, vector), setting `UEdGraphPin::DefaultValue` directly works. For complex types or linked parameters, use `SetLinkedValueHandleForFunctionInput` or `SetDynamicInputForFunctionInput`. Getting the `InputScriptVariableId` requires reading the module's `UNiagaraGraph::GetScriptVariables()`.

Source: `NiagaraStackGraphUtilities.h` (lines 200–225)

---

### 8. Module Discovery / Catalog

Olive's Blueprint module has `FOliveNodeCatalog`. The Niagara equivalent is querying the asset registry for `UNiagaraScript` assets with `ENiagaraScriptUsage::Module` usage and `ENiagaraScriptLibraryVisibility::Library` visibility.

**The canonical API is `FNiagaraEditorUtilities::GetFilteredScriptAssets()`:**

```cpp
#include "NiagaraEditorUtilities.h"

FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions Options;
Options.ScriptUsageToInclude = ENiagaraScriptUsage::Module;
Options.TargetUsageToMatch = ENiagaraScriptUsage::ParticleSpawnScript; // optional: filter by stage
Options.bIncludeNonLibraryScripts = false;      // false = library only
Options.bIncludeDeprecatedScripts = false;

TArray<FAssetData> ModuleAssets;
FNiagaraEditorUtilities::GetFilteredScriptAssets(Options, ModuleAssets);
```

Internally this uses `FAssetRegistryModule` to query `UNiagaraScript` assets where `LibraryVisibility == ENiagaraScriptLibraryVisibility::Library` (this property is marked `AssetRegistrySearchable`, so it's in the registry without loading assets).

For finer control, use `FARFilter` directly:

```cpp
FARFilter Filter;
Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
Filter.TagsAndValues.Add(FName("LibraryVisibility"),
    FString::FromInt((int32)ENiagaraScriptLibraryVisibility::Library));
// TagsAndValues can also filter by ScriptUsage, Description, Category
```

Each `UNiagaraScript` asset exposes `AssetRegistrySearchable` properties: `ScriptDescription`, `Category` (a display-name string like "Spawn", "Forces", "Color"), `Keywords`, `LibraryVisibility`, and module dependency info.

The built-in engine module assets live at paths like:
- `/Niagara/Modules/Spawn/SpawnRate.SpawnRate`
- `/Niagara/Modules/Forces/Gravity.Gravity`
- `/Niagara/Modules/Update/Color/ColorFromCurve.ColorFromCurve`

Source: `NiagaraEditorUtilities.h` (lines 200–230), `NiagaraScript.h` (lines 73–151, 765)

---

### 9. Reading a Niagara System's Current State

```cpp
// Get all emitter handles
TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
for (const FNiagaraEmitterHandle& Handle : Handles)
{
    UNiagaraEmitter* Emitter = Handle.GetEmitter();
    FString Name = Handle.GetName().ToString();
    bool bEnabled = Handle.IsValid() && Handle.GetIsEnabled();

    // Get a specific script from this emitter via the system
    UNiagaraScript* PSScript = FNiagaraEditorUtilities::GetScriptFromSystem(
        *System, Handle.GetId(), ENiagaraScriptUsage::ParticleSpawnScript, FGuid());

    if (PSScript)
    {
        UNiagaraNodeOutput* OutputNode = FNiagaraEditorUtilities::GetScriptOutputNode(*PSScript);
        TArray<UNiagaraNodeFunctionCall*> Modules;
        FNiagaraStackGraphUtilities::GetOrderedModuleNodes(*OutputNode, Modules);
        // Modules[0] = first module in the Particle Spawn stack
    }
}

// Read renderer list
TArray<UNiagaraRendererProperties*>& Renderers =
    Emitter->GetLatestEmitterData()->GetRenderers();
for (UNiagaraRendererProperties* Renderer : Renderers)
{
    FString RendererClass = Renderer->GetClass()->GetName();
    // e.g., "NiagaraSpriteRendererProperties", "NiagaraMeshRendererProperties"
}
```

Source: `NiagaraSystem.h` (lines 289, 340), `NiagaraStackGraphUtilities.h` (lines 54–57)

---

### 10. Compilation After Edits

Niagara has two compile steps:

1. **Mark scripts dirty** — when you modify a script's graph:
   ```cpp
   Script->GetSource()->MarkNotSynchronized(TEXT("Reason"));
   ```

2. **Request system compile** — triggers full async compilation:
   ```cpp
   System->RequestCompile(/*bForce=*/false);
   // Optionally poll:
   System->PollForCompilationComplete(/*bFlushRequestCompile=*/true);
   ```
   `PollForCompilationComplete(true)` blocks the game thread until done — use with care in editor only.

For emitter-only changes (editing standalone `UNiagaraEmitter` assets):
```cpp
UNiagaraSystem::RequestCompileForEmitter(FVersionedNiagaraEmitter{Emitter, VersionGuid});
```

Source: `NiagaraSystem.h` (lines 418–436, 524)

---

### 11. Python Scripting Path

**Python exposure for Niagara is very limited.** The `unreal.NiagaraSystem` Python class exposes only configuration properties (shadow, culling, scalability, warmup settings) and one method: `create_system_conversion_context()`.

The `NiagaraSystemConversionContext` and `NiagaraEmitterConversionContext` classes in the `CascadeToNiagaraConverter` plugin (`Engine/Plugins/FX/CascadeToNiagaraConverter`) are `BlueprintInternalUseOnly` (i.e., Python-callable via Blueprint exposure), and provide:
- `UNiagaraSystemConversionContext::AddEmptyEmitter(FString)` → adds an emitter
- `UNiagaraEmitterConversionContext::FindOrAddModuleScript(ScriptName, Args, Category)` → adds a module
- `UNiagaraEmitterConversionContext::SetParameterDirectly(ParamName, Input, Category)` → sets a parameter
- `UNiagaraEmitterConversionContext::AddRenderer(Name, Properties)` → adds a renderer
- `UNiagaraEmitterConversionContext::Finalize()` → applies all pending changes

However, these are designed for the Cascade→Niagara batch converter workflow, require the `CascadeToNiagaraConverter` plugin to be enabled, and use `UCLASS(BlueprintInternalUseOnly)` which means they are NOT exposed to Python by default (despite `BlueprintCallable` on methods — `BlueprintInternalUseOnly` suppresses Python reflection).

**Practical conclusion:** `editor.run_python` is NOT a viable path for programmatic Niagara stack manipulation in UE 5.5. The Python API surface for Niagara is essentially read-only on asset configuration properties. Stack editing requires C++.

Source: `NiagaraStackGraphUtilitiesAdapterLibrary.h` (lines 600–890), Epic Python API docs for `unreal.NiagaraSystem` (5.5)

---

### 12. Key Differences from Blueprint: Stack vs. Node Graph

| Aspect | Blueprint K2 | Niagara |
|---|---|---|
| Graph model | Free-form node graph (any node to any node) | Linear parameter-map chain (Input → M1 → M2 → Output) |
| Node type | `UK2Node` subclasses (100+) | `UNiagaraNodeFunctionCall` (uniform — all modules are this type) |
| Module identity | Node class name | `UNiagaraScript` asset reference on `UNiagaraNodeFunctionCall::FunctionScript` |
| Multiple "graphs" | One EventGraph + FunctionGraphs | One `UNiagaraGraph` per stage (ParticleSpawn, ParticleUpdate, etc.) per emitter |
| Stage gating | Execution pins (exec flow) | `ENiagaraScriptUsage` on the `UNiagaraNodeOutput` — modules only valid in compatible stages |
| Parameter setting | Pin default values / connections | `UNiagaraNodeParameterMapSet` override nodes, or rapid iteration parameters |
| Compilation | Kismet compiler (fast, in-process) | HLSL transpilation (slow, async background thread) |
| Reading module inputs | Read pin default values | Parameter map history traversal (`FNiagaraParameterMapHistory`) |
| Undo/redo | `FScopedTransaction` + `Modify()` | Same — but also `Script->GetSource()->MarkNotSynchronized()` |
| "Node catalog" | `FOliveNodeCatalog` (fuzzy class name matching) | Asset registry query for `UNiagaraScript` with `Module` usage |

**The single biggest implication:** In Blueprint, the AI can wire any two nodes together. In Niagara, the AI only ever calls `AddScriptModuleToStack(ModuleScript, OutputNode, TargetIndex)` — it picks a module asset and a stack position. Parameter setting is the main complexity, not graph topology.

---

### 13. Renderer Manipulation

Renderers are stored on `UNiagaraEmitter` as `TArray<UNiagaraRendererProperties*>`. The renderer classes are in the `Niagara` runtime module:

- `UNiagaraSpriteRendererProperties` — billboard particles
- `UNiagaraMeshRendererProperties` — mesh particles
- `UNiagaraRibbonRendererProperties` — ribbons/trails
- `UNiagaraLightRendererProperties` — lights
- `UNiagaraDecalRendererProperties` — decals

Adding a renderer:
```cpp
UNiagaraSpriteRendererProperties* SpriteRenderer =
    NewObject<UNiagaraSpriteRendererProperties>(Emitter);
Emitter->Modify();
Emitter->GetLatestEmitterData()->AddRenderer(SpriteRenderer, /*RenderIndex=*/-1);
```

Reading: iterate `Emitter->GetLatestEmitterData()->GetRenderers()`.

Source: `NiagaraRendererProperties.h`, `NiagaraSpriteRendererProperties.h`

---

### 14. Feasibility Assessment

**Overall feasibility: MEDIUM. Achievable, but with non-trivial complexity and one major constraint.**

**What is straightforward:**
- Creating `UNiagaraSystem` and `UNiagaraEmitter` assets via factories — same pattern as Blueprint
- Adding/removing modules via `FNiagaraStackGraphUtilities::AddScriptModuleToStack` — a single, well-exported NIAGARAEDITOR_API function that does not require ViewModels
- Enumerating available modules via `FNiagaraEditorUtilities::GetFilteredScriptAssets` — simple asset registry query
- Reading the current state of a system (emitter list, module list per stage, renderer list)
- Adding renderers via `NewObject` + `AddRenderer`
- Compilation via `System->RequestCompile`

**What is hard:**
- **Setting module parameter values** — requires understanding `FNiagaraParameterHandle` aliasing, `UNiagaraScriptVariable` GUIDs, and the `UNiagaraNodeParameterMapSet` override node pattern. This is significantly more complex than setting Blueprint pin defaults.
- **Compilation latency** — Niagara compiles to HLSL asynchronously. Unlike Blueprint (synchronous with `FKismetEditorUtilities::CompileBlueprint`), Niagara's compile may take seconds. `PollForCompilationComplete(true)` can block, but is only safe in editor automation contexts.
- **Stage compatibility** — modules must be placed in compatible stages (e.g., a Spawn module cannot go in an Update stack). The `bFixupTargetIndex` + `DependencyUtilities::FindBestIndexForModuleInStack` mechanism handles ordering, but the AI still needs to know which stage to target.
- **No Python path** — there is no viable `editor.run_python` fallback for stack manipulation in UE 5.5. Any Niagara manipulation must be C++.
- **VersionedNiagaraEmitter** — UE 5.4+ introduced emitter versioning (`FVersionedNiagaraEmitter`). Many APIs take `FVersionedNiagaraEmitter` (an emitter pointer + version GUID pair) rather than bare `UNiagaraEmitter*`. The version GUID is obtained via `Emitter->GetLatestEmitterData()->Version.VersionGuid`.

**One major architectural constraint:**

The Niagara module write operations live in `NiagaraEditor`, which is an `Editor`-type module loading at phase `Default`. The `NiagaraEditor` module is a large dependency that pulls in graph editor, Sequencer, and many other subsystems. This is acceptable since Olive is itself editor-only, but it's a heavier dependency than PCG.

---

### 15. Minimum Viable Feature Set

Based on complexity-to-value analysis:

**Phase 1 (MVP):**
- `niagara.create_system` — create a new `UNiagaraSystem` asset
- `niagara.read_system` — read emitter list, per-emitter module list per stage, renderer list
- `niagara.add_emitter` — add a module emitter (from existing asset) to a system
- `niagara.add_module` — add a module script to an emitter stack stage by module asset path
- `niagara.remove_module` — remove a module from a stack
- `niagara.list_modules` — enumerate available engine/project Niagara module scripts (using `GetFilteredScriptAssets`)
- `niagara.compile` — compile a system

**Phase 2 (parameter setting):**
- `niagara.set_module_input` — set a float/int/bool/vector literal on a module parameter
- `niagara.link_module_input` — link a module input to a user-exposed parameter

**Explicitly out of scope for an MVP:**
- Creating custom module scripts (editing `UNiagaraGraph` inside a `UNiagaraScript`) — this is a full-blown HLSL graph editor
- Simulation stages, GPU compute, scratch pad modules — too specialized
- Data interface configuration — each DI type has its own property model

---

## Recommendations

1. **Use `NiagaraEditor` module, not ViewModel layer.** `FNiagaraStackGraphUtilities::AddScriptModuleToStack(UNiagaraScript*, UNiagaraNodeOutput&, int32)` and `FNiagaraEditorUtilities::GetScriptOutputNode()` work without instantiating `FNiagaraSystemViewModel`. The ViewModel layer adds complexity and requires the Niagara editor toolkit to be open. The low-level graph APIs are stable and sufficient.

2. **Module catalog should be asset-registry-based.** Use `FNiagaraEditorUtilities::GetFilteredScriptAssets` with `ScriptUsageToInclude = ENiagaraScriptUsage::Module`. Cache the results at plugin startup in `FOliveNiagaraNodeCatalog`. Each `UNiagaraScript` has `AssetRegistrySearchable` tags for category, keywords, and library visibility — usable for search without loading assets.

3. **Stage targeting is mandatory for every add-module call.** The AI must specify which stage (EmitterSpawn / EmitterUpdate / ParticleSpawn / ParticleUpdate) to target. Design the tool schema with a required `stage` parameter (an enum). There is no implicit "default" stage.

4. **Niagara compile is async — handle it differently than Blueprint compile.** Do NOT call `PollForCompilationComplete(true)` on the game thread during normal tool execution (blocks UI). Instead, call `RequestCompile(false)` and return a "compile requested" status. If blocking compile is needed for automation, do it in a background thread or use a deferred callback via `System->OnSystemCompiled()`.

5. **Defer parameter setting to Phase 2.** The override pin pattern with `FNiagaraParameterHandle` aliasing requires reading `UNiagaraGraph::GetScriptVariables()` to get stable `FGuid` IDs per parameter. This is researchable but complex. MVP tools without parameter setting are still highly useful (structure creation, module addition, reading).

6. **Use `FScopedTransaction` + `Modify()` same as Blueprint.** The `AddScriptModuleToStack` implementation calls `Graph->Modify()` internally, but the emitter and system objects need `Modify()` separately before structural changes (adding emitters, renderers).

7. **Guard with availability check.** Mirror the PCG pattern: wrap all Niagara module initialization in a check for the `Niagara` plugin being loaded. The check: `FModuleManager::Get().IsModuleLoaded(TEXT("NiagaraEditor"))`.

8. **Python path is NOT viable.** Do not expose Niagara manipulation via `editor.run_python`. The Python API surface is property-only. All Niagara tool handlers must be C++.

9. **IR design should be flat and stack-aware.** Unlike `FOliveIRGraph` (Blueprint's rich node graph IR), Niagara IR should model:
   - `NiagaraSystemIR` → list of `NiagaraEmitterIR`
   - `NiagaraEmitterIR` → stage-keyed map of `NiagaraModuleIR` arrays + renderer list
   - `NiagaraModuleIR` → { asset_path, enabled, parameter_overrides }
   This maps 1:1 to what the C++ API can read and write.

10. **The Cascade-to-Niagara converter's `UNiagaraEmitterConversionContext` is a reference, not a dependency.** Do not take a dependency on `CascadeToNiagaraConverter`. Its Python-callable API is `BlueprintInternalUseOnly` and not Python-accessible anyway. Study its implementation pattern for insight on the clipboard-based finalize approach, but implement Niagara tools directly against `FNiagaraStackGraphUtilities`.

---

Sources:
- [UNiagaraSystem API (UE 5.7 docs)](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/Niagara/UNiagaraSystem)
- [unreal.NiagaraSystem Python 5.4](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/NiagaraSystem?application_version=5.4)
- [unreal.NiagaraParameterCollection Python 5.5](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/NiagaraParameterCollection?application_version=5.5)
- [Niagara Script Editor Reference (UE 5.7)](https://dev.epicgames.com/documentation/en-us/unreal-engine/script-editor-reference-for-niagara-effects-in-unreal-engine)
- [System and Emitter Module Reference (UE 5.7)](https://dev.epicgames.com/documentation/en-us/unreal-engine/system-and-emitter-module-reference-for-niagara-effects-in-unreal-engine)
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/`
