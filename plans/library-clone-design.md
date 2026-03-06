# Library Clone Tool Design — `blueprint.create_from_library`

## 1. Overview

A new MCP tool that clones a library template into a real Blueprint asset in the user's project. Library templates contain full Blueprint structure (variables, components, dispatchers, functions with complete node graphs) extracted from real UE projects. The tool recreates this structure in the user's project, handling the fundamental problem that source-project-specific types, classes, and assets do not exist in the target project.

### Design Philosophy

This is NOT a literal `memcpy` of a Blueprint. It is a **structural transplant with adaptive resolution**. Every element (variable type, function call, component class, cast target) is resolved against the user's project at clone time. Elements that cannot resolve are handled gracefully: demoted to a weaker type, skipped with a warning, or flagged for manual remapping.

The tool is the most complex Blueprint tool in the plugin because it must handle the full combinatorial space of UE Blueprint node types while bridging two completely separate project contexts.

---

## 2. Tool Schema

```json
{
  "name": "blueprint.create_from_library",
  "description": "Clone a library template into a real Blueprint asset. Creates the asset with all structure (variables, components, dispatchers) and optionally recreates node graphs. Handles missing dependencies from the source project gracefully.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "template_id": {
        "type": "string",
        "description": "Library template ID (e.g., 'combatfs_arrow_component')"
      },
      "path": {
        "type": "string",
        "description": "Target asset path (e.g., '/Game/Blueprints/BP_MyArrowComponent')"
      },
      "mode": {
        "type": "string",
        "enum": ["structure", "portable", "full"],
        "default": "portable",
        "description": "Clone depth. 'structure': variables + components + function signatures only. 'portable': structure + engine-resolvable graph nodes (skips source-project-specific calls). 'full': everything, leaving broken refs as warnings."
      },
      "remap": {
        "type": "object",
        "description": "Optional class/type remapping. Keys are source project class names (e.g., 'BP_ArrowParent_C'), values are target project equivalents (e.g., 'BP_MyArrowBase' or 'Actor'). Applied before resolution.",
        "additionalProperties": { "type": "string" }
      },
      "graphs": {
        "type": "array",
        "items": { "type": "string" },
        "description": "Optional whitelist of graph names to clone (e.g., ['EventGraph', 'ArrowStuckInWall']). If omitted, clones all graphs that the mode permits."
      },
      "parent_class_override": {
        "type": "string",
        "description": "Override the parent class instead of using the template's. Useful when the template's parent is a source-project Blueprint."
      }
    },
    "required": ["template_id", "path"]
  }
}
```

### Registration

- **Tool name:** `blueprint.create_from_library`
- **Tags:** `{blueprint, write, create, template, library}`
- **Family:** `blueprint`
- **Confirmation tier:** Tier 1 (auto-execute). Same as `blueprint.create`. Creates a new asset; trivially undoable.

---

## 3. Dependency Resolution Strategy

### 3.1 Resolution Pipeline

Every type name, class name, and function name in the template goes through a 4-stage resolution pipeline:

```
Source Name
    |
    v
[1. Remap Map] -- User-provided explicit mapping
    |
    v
[2. Alias Map] -- FOliveNodeFactory::GetAliasMap() for function names
    |
    v
[3. Live Resolution] -- FOliveClassResolver::Resolve() for classes,
    |                    FOliveNodeFactory::FindFunction() for functions,
    |                    FindStruct/FindEnum for types
    v
[4. Fallback Strategy] -- Mode-dependent (skip / demote / create placeholder)
```

### 3.2 Per-Dependency-Type Handling

| # | Dependency Type | Resolution | Fallback (portable) | Fallback (full) |
|---|----------------|------------|---------------------|-----------------|
| 1 | **Parent class** | `FOliveClassResolver::Resolve(name)`. If Blueprint name, check `depends_on` chain. | If unresolvable: use `parent_class_override` if provided, else walk `depends_on` chain to first native ancestor (the "root native" strategy). | Same as portable. Parent MUST resolve or the entire clone fails. |
| 2 | **Variable types** | Resolve class/struct/enum names via ClassResolver/FindStruct. | Object vars with unresolvable class: demote to base `UObject*`. Struct vars with unknown struct: demote to `FString` + warning. Enum: demote to `uint8`. | Same demotion + add `design_warning`. |
| 3 | **Function calls** | `FindFunctionEx(function_name, owning_class, Blueprint)`. owning_class resolved first. | If owning_class unresolvable AND function not in libraries: skip node + warning. If owning_class resolves but function not found: skip node + warning with suggestions from `LastFuzzySuggestions`. | Create node anyway. It will have no pins and be non-functional (ghost behavior). Warn prominently. |
| 4 | **Component types** | `FOliveClassResolver::Resolve(class_name)`. | Unresolvable: skip component + warning. Variables referencing it become `UObject*`. | Create as `USceneComponent` placeholder + warning. |
| 5 | **Asset references** | Pin defaults pointing to source project assets (meshes, materials, etc.) | Clear the default + warning listing the original path. | Same. Cannot resolve cross-project asset paths. |
| 6 | **Inheritance chains** | `ResolveInheritanceChain()` walks `depends_on`. For each ancestor: check if it already exists as a user asset (via remap or ClassResolver). | Create ancestors recursively ONLY if `parent_class_override` is NOT set and mode permits. Otherwise use root native ancestor. | Same. Recursive chain creation is a separate future enhancement (see Section 12). |
| 7 | **Cross-template refs** | Variables/casts referencing other project BPs (e.g., `BP_PlayerCharacter_C`). | Resolve via ClassResolver (user may have their own BP with that name). If not found: demote variable to base class, skip cast node. | Same demotion, but cast node created with unresolved class (will show as error node in BP editor). |
| 8 | **Interface deps** | `FOliveClassResolver::Resolve(interface_name)`. | Skip interface + warning. Functions from that interface become orphaned custom events. | Same. |

### 3.3 The Root Native Ancestor Strategy

When a template's parent class is a source-project Blueprint (e.g., `BP_ArrowParent`), we need a concrete class to create the new Blueprint. The strategy:

1. Walk the `depends_on` chain via `ResolveInheritanceChain()`.
2. For each ancestor in root-to-leaf order, try `FOliveClassResolver::Resolve(ParentClassName)`.
3. If found (user has a class with that name), use it.
4. If not found, continue to the next ancestor.
5. Eventually reach the root, whose `parent_class.source == "cpp"` (e.g., `Actor`, `ActorComponent`). This always resolves.
6. Use the deepest resolvable ancestor.

The function definitions and variables from intermediate unresolvable ancestors are merged into the cloned Blueprint (flattened inheritance).

---

## 4. Node Recreation Algorithm

### 4.1 Per-Graph Pipeline

For each graph in the template (EventGraph, functions), the clone tool runs a 6-phase pipeline that mirrors the PlanExecutor but operates on library template node data instead of plan_json steps.

```
Phase 1: Pre-classify nodes
Phase 2: Create nodes
Phase 3: Wire exec connections
Phase 4: Wire data connections
Phase 5: Set pin defaults
Phase 6: Auto-layout
```

### 4.2 Phase 1: Pre-classify Nodes

Before creating anything, scan all nodes in the graph and classify each:

```cpp
enum class ECloneNodeDisposition
{
    Create,         // Node can be fully recreated
    Skip,           // Node requires unresolvable dependency, skip in this mode
    Placeholder,    // Create a comment node documenting what was here
};
```

Classification rules:
- `CallFunction`: Resolve `owning_class` + `function` via the resolution pipeline. If both resolve: `Create`. If function not found but owning_class is engine: `Create` (FindFunction will handle it). If owning_class is unresolvable source-project class: `Skip` in portable, `Create` in full.
- `VariableGet` / `VariableSet`: If the variable name is in the template's own variables (or inherited): `Create`. If it references an external class variable: `Skip` unless the class resolves.
- `Cast`: Resolve `TargetClass`. If resolves: `Create`. If not: `Skip` in portable, `Create` in full (will be broken).
- `Event` / `CustomEvent`: Always `Create`.
- `FunctionEntry` / `FunctionResult`: Always `Create` (structural).
- `Branch` / `Sequence` / `Timeline` / other control flow: Always `Create`.
- `ControlRigGraphNode`: Always `Skip` (Control Rig is a specialized subsystem; these are not standard K2 nodes).

Build a `TMap<FString, ECloneNodeDisposition> NodeDispositions` keyed by node ID (`node_0`, `node_1`, etc.).

### 4.3 Phase 2: Create Nodes

For each node where disposition is `Create`, map the library node type to an `OliveNodeTypes` constant and create via `FOliveNodeFactory::Get().CreateNode()`.

**Type Mapping Table:**

| Library `type` | OliveNodeTypes | Properties to pass |
|---------------|----------------|-------------------|
| `CallFunction` | `CallFunction` | `function_name` = `node.function`, `target_class` = resolved `owning_class` |
| `VariableGet` | `GetVariable` | `variable_name` = `node.variable` |
| `VariableSet` | `SetVariable` | `variable_name` = `node.variable` |
| `Branch` | `Branch` | (none) |
| `Sequence` | `Sequence` | (none, default 2 outputs; if more seen in pins_out, set `num_outputs`) |
| `Cast` | `Cast` | `target_class` = resolved `TargetClass` from properties |
| `Event` | `Event` | `event_name` = extract from `node.function` or `node.title` |
| `CustomEvent` | `CustomEvent` | `event_name` = `node.function` or first word of `node.title` |
| `FunctionEntry` | (skip) | Already exists when function graph is created |
| `FunctionResult` | (skip) | Already exists when function graph is created |
| `Timeline` | (skip in structure/portable) | Track data not in library format. `full` mode creates empty shell. See Section 15. |
| `ControlRigGraphNode` | (skip) | Not a standard K2Node |

**Special Cases:**
- For `CallFunction` where `owning_class` is `"Self"` or matches the template's own class: omit `target_class` (let FindFunction search the BP itself).
- For `CallFunction` where the function is on an interface class: set `target_class` to the interface name (triggers `InterfaceSearch` in FindFunction, creating `UK2Node_Message`).
- For `Event` nodes: check if one already exists in the graph (reuse pattern from PlanExecutor's `FindExistingEventNode`).

**Node ID Mapping:**
Maintain `TMap<FString, FString> LibNodeToCreatedNode` mapping library node IDs (`node_0`) to the created graph node IDs. Also maintain `TMap<FString, UEdGraphNode*> LibNodeToPtr` for direct pin access during wiring.

### 4.4 Phase 3: Wire Exec Connections

For each created node, scan its `pins_out` for exec pins with `connections`:

```json
{
  "name": "then",
  "type": { "category": "exec" },
  "is_exec": true,
  "connections": ["node_5.execute"]
}
```

Parse `"node_5.execute"` -> source is current node's `"then"` pin, target is `node_5`'s `"execute"` pin.

- If `node_5` was skipped (disposition `Skip`), skip this wire.
- Find real `UEdGraphPin*` on both sides by name.
- Connect via `FOlivePinConnector::Get().Connect(SourcePin, TargetPin)`.
- On failure: log warning, continue.

### 4.5 Phase 4: Wire Data Connections

Same approach as exec, but for non-exec pins:

```json
{
  "name": "ReturnValue",
  "type": { "category": "float" },
  "connections": ["node_4.A"]
}
```

Connect `ReturnValue` output on source node to `A` input on `node_4`.

- Use `FOlivePinConnector::Connect(src, tgt, bAllowConversion=true)` to enable autocast.
- If target node was skipped, skip wire.
- Pin name matching: exact first, then case-insensitive, then type-compatible fallback.

### 4.6 Phase 5: Set Pin Defaults

For each created node's `pins_in`, if the pin has a `"default"` value and is not connected:

```json
{
  "name": "ElementIndex",
  "type": { "category": "int" },
  "default": "0"
}
```

Apply via `GraphWriter.SetPinDefault(AssetPath, GraphName, NodeId, PinName, DefaultValue)` or directly via `Pin->DefaultValue = DefaultStr`.

**Asset reference defaults** (e.g., `"/Game/FlexibleCombatSystem/Meshes/..."`) are cleared with a warning.

### 4.7 Phase 6: Auto-Layout

Use `FOliveGraphLayoutEngine` to lay out created nodes. Pass the exec topology for proper flow ordering.

---

## 5. Remapping System

### 5.1 Remap Map Structure

The `remap` parameter is a simple string-to-string map:

```json
{
  "remap": {
    "BP_ArrowParent_C": "BP_MyProjectile",
    "BP_PlayerCharacter_C": "BP_MyCharacter",
    "CombatStatusComponent_C": "BP_MyStatusComp",
    "S_ItemData": "S_MyItemData",
    "I_RangedWeaponInterface_C": "I_MyWeaponInterface"
  }
}
```

### 5.2 Remap Application Order

The remap map is consulted FIRST in the resolution pipeline:

1. Strip `_C` suffix from source name for matching (so both `BP_ArrowParent_C` and `BP_ArrowParent` match the same entry).
2. If remap provides a target name, resolve THAT name via `FOliveClassResolver::Resolve()`.
3. If the remapped name also fails to resolve, treat as unresolvable (same fallback as no remap).

### 5.3 Auto-Remap Suggestions

When the tool encounters unresolvable types, the result includes a `remap_suggestions` array:

```json
{
  "remap_suggestions": [
    {
      "source_name": "BP_PlayerCharacter_C",
      "used_in": ["variable: CharacterRef", "cast node in EventGraph", "function call: GetMovementComponent"],
      "suggestion": "Map this to your project's player character class"
    }
  ]
}
```

This allows the AI to prompt the user or make an intelligent remap choice and retry.

---

## 6. Inheritance Chain Handling

### 6.1 Simple Case: Native Parent

Template has `"parent_class": {"name": "Actor", "source": "cpp"}`, `depends_on: null`. Straightforward: create BP with Actor parent.

### 6.2 Blueprint Parent, Single Level

Template has `"parent_class": {"name": "BP_ArrowParent", "source": "blueprint"}`, `depends_on: "combatfs_bp_arrow_parent"`.

Resolution:
1. Check remap: `BP_ArrowParent` -> user-provided class? Use it.
2. Check ClassResolver: does `BP_ArrowParent` exist in the project? Use it.
3. Fall back: load `combatfs_bp_arrow_parent` template, check ITS parent. `BP_ArrowParent` -> `Actor` (native). Use `Actor`.
4. Accumulate `combatfs_bp_arrow_parent`'s variables and components as "inherited" items. Add them to the clone with `design_warning`: "Flattened from parent template BP_ArrowParent".

### 6.3 Deep Chain

Template A depends on B depends on C depends on D (native). Walk the chain, find the deepest resolvable ancestor. Flatten everything above that level.

### 6.4 Cycle Detection

`ResolveInheritanceChain()` already has cycle detection (max depth 10, visited set). The clone tool reuses this.

### 6.5 Recursive Chain Creation (NOT in V1)

Future enhancement: option to recursively create the entire chain (create D, then C inheriting D, then B inheriting C, then A inheriting B). This is complex and deferred. The root native strategy is sufficient for V1.

---

## 7. Error Handling

### 7.1 Fatal Errors (abort entire operation)

| Error Code | Condition |
|------------|-----------|
| `LIBRARY_TEMPLATE_NOT_FOUND` | `template_id` not in LibraryIndex |
| `LIBRARY_CLONE_PARENT_UNRESOLVABLE` | Parent class cannot be resolved even to a native ancestor |
| `LIBRARY_CLONE_CREATE_FAILED` | `FOliveBlueprintWriter::CreateBlueprint()` fails |
| `LIBRARY_CLONE_LOAD_FAILED` | Blueprint created but `LoadObject` returns nullptr |

### 7.2 Non-Fatal Errors (skip item, continue)

| Warning Category | Example |
|-----------------|---------|
| `variable_type_demoted` | "Variable 'ArrowActor' type demoted from BP_ArrowParent_C to Object (class not found in project)" |
| `component_skipped` | "Component 'ArrowComponent' skipped: class ArrowComponent_C not found" |
| `node_skipped` | "Node node_5 (CallFunction: GetArrowDamage on RangedComponent_C) skipped: owning class not found" |
| `wire_failed` | "Connection node_3.ReturnValue -> node_4.A failed: source node was skipped" |
| `wire_incompatible` | "Connection node_3.ReturnValue -> node_4.A failed: type incompatible (Float vs Vector)" |
| `default_cleared` | "Pin default cleared: original referenced /Game/FlexibleCombatSystem/Meshes/..." |
| `interface_skipped` | "Interface I_RangedWeaponInterface_C skipped: not found in project" |
| `cast_skipped` | "Cast to BP_PlayerCharacter_C skipped: class not found" |
| `dispatcher_skipped` | "Event dispatcher 'OnArrowFired' skipped: delegate params reference unresolvable type" |

### 7.3 Warning Aggregation

Warnings are grouped by category in the result to avoid flooding. If a single class causes 20+ skip warnings, they are collapsed:

```json
{
  "warnings_by_category": {
    "node_skipped": {
      "count": 15,
      "summary": "15 nodes skipped due to unresolvable owning classes",
      "unresolvable_classes": ["RangedComponent_C", "CombatStatusComponent_C"],
      "details": ["node_5: GetArrowDamage on RangedComponent_C", "..."]
    }
  }
}
```

---

## 8. Integration Points

### 8.1 New Files

| File | Purpose |
|------|---------|
| `Source/OliveAIEditor/Blueprint/Public/Template/OliveLibraryCloner.h` | `FOliveLibraryCloner` class declaration |
| `Source/OliveAIEditor/Blueprint/Private/Template/OliveLibraryCloner.cpp` | Implementation |

The cloner is NOT a singleton. It is instantiated per clone operation (like `FOlivePlanExecutor`). This avoids state leakage between clone attempts.

### 8.2 Existing Files Modified

| File | Change |
|------|--------|
| `OliveBlueprintToolHandlers.h` | Add `HandleBlueprintCreateFromLibrary` method declaration |
| `OliveBlueprintToolHandlers.cpp` | Add handler implementation + registration in `RegisterTemplateTools()` |
| `OliveBlueprintSchemas.h/.cpp` | Add `BlueprintCreateFromLibrary()` schema |
| `OliveTemplateSystem.h` | No changes needed (LibraryIndex already public) |

### 8.3 Shared Services Used

| Service | Usage |
|---------|-------|
| `FOliveLibraryIndex` | `FindTemplate()`, `LoadFullJson()`, `ResolveInheritanceChain()` |
| `FOliveClassResolver` | Resolve class names to `UClass*` |
| `FOliveNodeFactory` | `CreateNode()`, `FindFunction()`, `FindFunctionEx()`, `FindClass()`, `FindStruct()` |
| `FOlivePinConnector` | `Connect()` for pin wiring |
| `FOliveBlueprintWriter` | `CreateBlueprint()`, `AddVariable()`, `AddInterface()`, `AddEventDispatcher()`, `CreateFunction()` |
| `FOliveComponentWriter` | `AddComponent()`, `ModifyComponent()`, `SetRootComponent()` |
| `FOliveGraphLayoutEngine` | Auto-layout after node creation |
| `FOliveWritePipeline` | Tool handler wraps the clone in the standard 6-stage pipeline |
| `FOliveCompileManager` | Final compile after clone |

---

## 9. Edge Cases

### 9.1 Template references an interface that doesn't exist

**Handling:** Skip the `AddInterface()` call. Log `interface_skipped` warning. Any `Event` nodes in the template that correspond to interface events (identified by matching function names in the interface's `required_functions` list) are converted to `CustomEvent` nodes with the same name. This preserves the graph structure while making the events functional (though not polymorphic).

### 9.2 Parent class is a Blueprint that depends on another Blueprint

**Handling:** Walk the full `depends_on` chain via `ResolveInheritanceChain()`. Apply the root native ancestor strategy (Section 3.3). Flatten variables and components from all unresolvable ancestors.

### 9.3 Variable type is an enum/struct from the source project

**Handling:** Attempt `FOliveNodeFactory::FindStruct()` / `FindObject<UEnum>()`. If not found:
- Struct: demote to `FString` with warning "Variable 'X' type demoted from S_ItemData to String".
- Enum: demote to `uint8` (byte) with warning.
- Check remap map first -- user might map `S_ItemData` -> `S_MyItemData`.

### 9.4 Node calls a function from a plugin the user doesn't have

**Handling:** `FindFunctionEx()` will fail. In portable mode: skip the node. The warning includes the full search trail from `FOliveFunctionSearchResult::SearchedLocations` so the AI can diagnose what's missing.

### 9.5 Macro nodes referencing macros from the source project

**Handling:** Library templates from the extraction pipeline do NOT contain `MacroInstance` nodes (confirmed by grep: zero occurrences in the 658-template CombatFS library). The extractor strips or inlines them. If a future extraction includes them, they would be classified as `Skip` since macro references are always local.

### 9.6 Event dispatchers with custom signatures (delegate params)

**Handling:** `FOliveBlueprintWriter::AddEventDispatcher()` takes a dispatcher name and optional parameter list. If the dispatcher's parameter types reference source-project classes, resolve them through the pipeline. Unresolvable parameter types: demote to `UObject*` or skip the entire dispatcher with warning.

### 9.7 Component bound events on custom component types

**Handling:** `ComponentBoundEvent` nodes were not found in the CombatFS library (extraction strips them to their simpler delegate form). If encountered: classify as `Skip` unless the component class resolves. The delegate property name must match a real `FMulticastDelegateProperty` on the resolved component class.

### 9.8 Nodes with latent actions from project-specific classes

**Handling:** Latent nodes are just `CallFunction` nodes with `IsLatent: true`. Resolution is the same as any function call. If the owning class is unresolvable, the node is skipped in portable mode.

### 9.9 Graph has nodes that reference other graphs in the same BP

**Handling:** This is the "sibling function call" case. A `CallFunction` node in EventGraph calls `ArrowStuckInWall` which is defined as a function graph in the same template. Resolution order handles this naturally: the function graphs are created BEFORE node graphs are populated. When `FindFunction` runs on the already-partially-constructed Blueprint, it finds the function via `SkeletonGeneratedClass` (FunctionGraph search, step 3 in FindFunction).

**Critical implementation detail:** Functions must be created (empty, with signatures) BEFORE any graphs are populated with nodes. This is the same pattern `ApplyTemplate()` uses for factory templates.

### 9.10 Very large graphs (500+ nodes) and the paging system

**Handling:** Library templates already store graphs with paging metadata (`page`, `page_size`, `total_pages`). The clone tool must:
1. Call `LoadFullJson()` which loads the entire template.
2. If a graph has `total_pages > 1`, iterate all pages (they are stored inline in the full JSON -- paging only affects the read API, not storage).
3. Process nodes in batches of 100 to avoid editor thread stalls. Use `GEngine->Tick()` between batches if node count exceeds 500.

Actually, on review: the full JSON from `LoadFullJson()` contains ALL nodes regardless of paging. Paging is a read-time API concern. The clone tool reads from the full JSON directly, so no special paging handling is needed. The concern is performance for very large graphs -- addressed by batch processing.

### 9.11 The same template being cloned twice (naming conflicts)

**Handling:** Asset path uniqueness is the user's (or AI's) responsibility. If the path already exists, `FOliveBlueprintWriter::CreateBlueprint()` returns an error. The tool does NOT auto-suffix. The AI receives a clear error: "Asset already exists at /Game/Blueprints/BP_MyArrow. Choose a different path."

This is consistent with `blueprint.create` behavior.

### 9.12 ControlRigGraphNode (1261 occurrences in CombatFS)

These are Animation Blueprint specific nodes from the Control Rig plugin. They operate in a completely different graph paradigm (not K2 nodes). They are always skipped with a single summary warning: "N Control Rig nodes skipped (not supported outside Animation Blueprint context)".

### 9.13 Duplicate variable names from flattened inheritance

When flattening variables from parent templates, a child template may redefine a parent variable. The clone tool de-duplicates by name: if a variable name already exists (from a previously processed ancestor), the child's definition wins (overrides type/defaults).

### 9.14 Component hierarchy from source project

The template's `components.tree` includes hierarchical parent-child relationships and properties. Components with native classes (`StaticMeshComponent`, `SphereComponent`, etc.) are created normally. Components with source-project classes are skipped. Property values that reference source-project assets (e.g., `"StaticMesh": "/Game/FlexibleCombatSystem/..."`) are cleared with warnings.

### 9.15 Pin names containing spaces

Library template pin names often contain spaces (e.g., `"AsBP Player Character"`). Pin name matching must be exact (including spaces) when finding pins on created nodes. The existing `UEdGraphNode::FindPin()` handles this correctly.

---

## 10. Return Format

### 10.1 Success Result

```json
{
  "success": true,
  "asset_path": "/Game/Blueprints/BP_MyArrow",
  "template_id": "combatfs_arrow_component",
  "mode": "portable",
  "parent_class": "ActorComponent",
  "parent_class_note": "Original parent BP_ArrowParent resolved to native ancestor ActorComponent",
  "structure": {
    "variables_created": 12,
    "variables_demoted": 3,
    "variables_skipped": 0,
    "components_created": 0,
    "components_skipped": 0,
    "interfaces_added": 0,
    "interfaces_skipped": 1,
    "dispatchers_created": 0
  },
  "graphs": {
    "total": 17,
    "cloned": 14,
    "skipped": 3,
    "details": [
      {
        "name": "EventGraph",
        "nodes_created": 15,
        "nodes_skipped": 5,
        "connections_succeeded": 22,
        "connections_failed": 3,
        "defaults_set": 8
      },
      {
        "name": "ArrowStuckInWall",
        "nodes_created": 14,
        "nodes_skipped": 0,
        "connections_succeeded": 24,
        "connections_failed": 0,
        "defaults_set": 5
      }
    ]
  },
  "compile_result": {
    "success": true,
    "errors": [],
    "warnings": ["Variable 'ArrowActor' has type Object which may need to be specialized"]
  },
  "warnings": [
    "Variable 'ArrowActor' type demoted: BP_ArrowParent_C -> Object",
    "Variable 'RangedComponent' type demoted: RangedComponent_C -> Object",
    "Interface I_RangedWeaponInterface_C skipped: not found",
    "3 nodes skipped in EventGraph: owning class CombatStatusComponent_C not found"
  ],
  "remap_suggestions": [
    {
      "source_name": "BP_ArrowParent_C",
      "used_in": ["variable: ArrowActor (type)", "parent_class"],
      "suggestion": "Map to your project's arrow/projectile base class, or use Actor"
    },
    {
      "source_name": "CombatStatusComponent_C",
      "used_in": ["variable: CombatStatusComp (type)", "3 function calls"],
      "suggestion": "Map to your project's status component class if you have one"
    }
  ],
  "has_design_warnings": true
}
```

### 10.2 Partial Success

If graph cloning fails but structure was created, still return success with `has_design_warnings: true` and detailed per-graph failure info. The Blueprint exists and is usable, just incomplete.

### 10.3 Error Result

Standard `FOliveToolResult::Error(code, message, suggestion)` for fatal errors.

---

## 11. Implementation Classes

### 11.1 FOliveLibraryCloner

```cpp
// OliveLibraryCloner.h

#pragma once

#include "CoreMinimal.h"
#include "Template/OliveTemplateSystem.h"
#include "MCP/OliveToolRegistry.h"  // FOliveToolResult

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
struct FOliveLibraryTemplateInfo;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveLibraryCloner, Log, All);

/**
 * Disposition of a library node during clone classification.
 */
enum class ECloneNodeDisposition : uint8
{
    Create,         // Can be fully recreated
    Skip,           // Requires unresolvable dependency
    Placeholder,    // Create comment documenting original (future)
};

/**
 * Clone mode controlling how much of the template is recreated.
 */
enum class ELibraryCloneMode : uint8
{
    Structure,  // Variables, components, dispatchers, function signatures only
    Portable,   // Structure + engine-resolvable nodes
    Full,       // Everything, broken refs as warnings
};

/**
 * Per-node classification result during pre-classification phase.
 */
struct FCloneNodeClassification
{
    ECloneNodeDisposition Disposition = ECloneNodeDisposition::Skip;
    FString SkipReason;  // Why this node was skipped (for warnings)

    // Resolved data for Create nodes
    FString ResolvedNodeType;           // OliveNodeTypes constant
    TMap<FString, FString> Properties;  // Properties for CreateNode
};

/**
 * Per-graph clone result for detailed reporting.
 */
struct FCloneGraphResult
{
    FString GraphName;
    int32 NodesCreated = 0;
    int32 NodesSkipped = 0;
    int32 ConnectionsSucceeded = 0;
    int32 ConnectionsFailed = 0;
    int32 DefaultsSet = 0;
    TArray<FString> Warnings;
};

/**
 * Aggregate result of a library clone operation.
 */
struct FLibraryCloneResult
{
    bool bSuccess = false;
    FString AssetPath;
    FString ParentClass;
    FString ParentClassNote;

    // Structure counts
    int32 VariablesCreated = 0;
    int32 VariablesDemoted = 0;
    int32 VariablesSkipped = 0;
    int32 ComponentsCreated = 0;
    int32 ComponentsSkipped = 0;
    int32 InterfacesAdded = 0;
    int32 InterfacesSkipped = 0;
    int32 DispatchersCreated = 0;

    // Per-graph results
    TArray<FCloneGraphResult> GraphResults;

    // Aggregated warnings
    TArray<FString> Warnings;

    // Remap suggestions for unresolved types
    struct FRemapSuggestion
    {
        FString SourceName;
        TArray<FString> UsedIn;
        FString Suggestion;
    };
    TArray<FRemapSuggestion> RemapSuggestions;

    // Compile result
    bool bCompiled = false;
    bool bCompileSuccess = false;
    TArray<FString> CompileErrors;

    /** Convert to JSON for tool result */
    TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * FOliveLibraryCloner
 *
 * Clones a library template into a real Blueprint asset.
 * NOT a singleton -- instantiate fresh per clone operation.
 * Must be called on the game thread.
 *
 * Usage:
 *   FOliveLibraryCloner Cloner;
 *   FLibraryCloneResult Result = Cloner.Clone(TemplateId, AssetPath, Mode, RemapMap);
 */
class OLIVEAIEDITOR_API FOliveLibraryCloner
{
public:
    FOliveLibraryCloner() = default;
    ~FOliveLibraryCloner() = default;

    /**
     * Clone a library template into a new Blueprint asset.
     *
     * @param TemplateId        Library template ID
     * @param AssetPath         Target asset path (/Game/...)
     * @param Mode              Clone depth (structure/portable/full)
     * @param RemapMap          Source name -> target name mapping
     * @param GraphWhitelist    Optional: only clone these graphs (empty = all)
     * @param ParentClassOverride  Optional: override parent class
     * @return Clone result with detailed per-graph reporting
     */
    FLibraryCloneResult Clone(
        const FString& TemplateId,
        const FString& AssetPath,
        ELibraryCloneMode Mode,
        const TMap<FString, FString>& RemapMap,
        const TArray<FString>& GraphWhitelist = {},
        const FString& ParentClassOverride = TEXT(""));

private:
    // ================================================================
    // Resolution
    // ================================================================

    /** Resolve a class name through remap -> ClassResolver pipeline */
    UClass* ResolveClass(const FString& SourceName);

    /** Resolve a struct name through remap -> FindStruct pipeline */
    UScriptStruct* ResolveStruct(const FString& SourceName);

    /** Resolve a function via remap on owning class + FindFunction */
    UFunction* ResolveFunction(
        const FString& FunctionName,
        const FString& OwningClass,
        UBlueprint* Blueprint);

    /** Find the deepest resolvable parent class from the inheritance chain */
    FString ResolveParentClass(
        const FOliveLibraryTemplateInfo& TemplateInfo,
        FString& OutNote);

    /** Apply remap to a class name (strips _C, checks map, re-adds _C if needed) */
    FString ApplyRemap(const FString& SourceName) const;

    /** Track an unresolved type for remap suggestions */
    void TrackUnresolved(const FString& SourceName, const FString& UsageContext);

    // ================================================================
    // Structure Creation
    // ================================================================

    /** Create variables from template JSON, handling type demotion */
    void CreateVariables(
        UBlueprint* Blueprint,
        const FString& AssetPath,
        const TSharedPtr<FJsonObject>& TemplateJson,
        FLibraryCloneResult& Result);

    /** Create components from template JSON tree */
    void CreateComponents(
        UBlueprint* Blueprint,
        const FString& AssetPath,
        const TSharedPtr<FJsonObject>& TemplateJson,
        FLibraryCloneResult& Result);

    /** Add interfaces from template JSON */
    void AddInterfaces(
        UBlueprint* Blueprint,
        const FString& AssetPath,
        const TSharedPtr<FJsonObject>& TemplateJson,
        FLibraryCloneResult& Result);

    /** Create event dispatchers from template JSON */
    void CreateDispatchers(
        UBlueprint* Blueprint,
        const FString& AssetPath,
        const TSharedPtr<FJsonObject>& TemplateJson,
        FLibraryCloneResult& Result);

    /** Create function graphs (empty, with signatures) for all template functions */
    void CreateFunctionSignatures(
        UBlueprint* Blueprint,
        const FString& AssetPath,
        const TSharedPtr<FJsonObject>& TemplateJson,
        FLibraryCloneResult& Result);

    // ================================================================
    // Graph Cloning
    // ================================================================

    /** Clone a single graph's nodes and connections */
    FCloneGraphResult CloneGraph(
        UBlueprint* Blueprint,
        UEdGraph* Graph,
        const FString& AssetPath,
        const TSharedPtr<FJsonObject>& GraphJson);

    /** Phase 1: Classify all nodes in a graph */
    TMap<FString, FCloneNodeClassification> ClassifyNodes(
        UBlueprint* Blueprint,
        const TArray<TSharedPtr<FJsonValue>>& NodesArray);

    /** Phase 2: Create classified nodes */
    void CreateNodes(
        UBlueprint* Blueprint,
        UEdGraph* Graph,
        const TArray<TSharedPtr<FJsonValue>>& NodesArray,
        const TMap<FString, FCloneNodeClassification>& Classifications,
        FCloneGraphResult& Result);

    /** Phase 3: Wire exec connections */
    void WireExecConnections(
        const TArray<TSharedPtr<FJsonValue>>& NodesArray,
        FCloneGraphResult& Result);

    /** Phase 4: Wire data connections */
    void WireDataConnections(
        const TArray<TSharedPtr<FJsonValue>>& NodesArray,
        FCloneGraphResult& Result);

    /** Phase 5: Set pin defaults */
    void SetPinDefaults(
        const TArray<TSharedPtr<FJsonValue>>& NodesArray,
        FCloneGraphResult& Result);

    // ================================================================
    // Pin Helpers
    // ================================================================

    /** Find a pin on a node by name (exact, then case-insensitive) */
    UEdGraphPin* FindPinByName(
        UEdGraphNode* Node,
        const FString& PinName,
        EEdGraphPinDirection Direction);

    /** Check if a pin default value looks like an asset reference */
    bool IsAssetReference(const FString& DefaultValue) const;

    // ================================================================
    // State (per-clone operation)
    // ================================================================

    /** Clone mode */
    ELibraryCloneMode Mode = ELibraryCloneMode::Portable;

    /** User-provided remap map */
    TMap<FString, FString> RemapMap;

    /** Library node ID -> created UEdGraphNode* */
    TMap<FString, UEdGraphNode*> NodeMap;

    /** Library node ID -> created Olive node ID (for reporting) */
    TMap<FString, FString> NodeIdMap;

    /** Unresolved source names -> usage contexts (for remap suggestions) */
    TMap<FString, TArray<FString>> UnresolvedTypes;

    /** The Blueprint being populated */
    UBlueprint* TargetBlueprint = nullptr;
};
```

### 11.2 Module Boundary

**Depends on:**
- `OliveAIRuntime` — IR types (`FOliveIRVariable`, `FOliveIRType`, etc.)
- `OliveAIEditor/Blueprint/Template` — `FOliveLibraryIndex`, `FOliveLibraryTemplateInfo`
- `OliveAIEditor/Blueprint/Writer` — `FOliveNodeFactory`, `FOlivePinConnector`, `FOliveBlueprintWriter`, `FOliveGraphWriter`, `FOliveComponentWriter`
- `OliveAIEditor/Blueprint/Plan` — `FOliveGraphLayoutEngine`
- `OliveAIEditor/Blueprint` — `FOliveClassResolver`, `FOliveCompileManager`
- `OliveAIEditor/Services` — `FOliveValidationEngine`

**Depended on by:**
- `OliveBlueprintToolHandlers.cpp` — registers and handles the tool

**Public API:**
- `FOliveLibraryCloner::Clone()` — the only public entry point
- `FLibraryCloneResult` — the result struct

**Private:**
- All resolution, creation, and wiring methods
- Per-operation state

---

## 12. Data Flow Diagram

```
AI calls blueprint.create_from_library(template_id, path, mode, remap)
    |
    v
HandleBlueprintCreateFromLibrary (tool handler in OliveBlueprintToolHandlers.cpp)
    |
    +--> Validate params
    +--> Build FOliveWriteRequest
    +--> ExecuteWithOptionalConfirmation (Tier 1 = auto)
         |
         v
    FOliveWritePipeline::Execute()
         |
         +--> Stage 1: Validate (param checks)
         +--> Stage 2: Confirm (auto for Tier 1)
         +--> Stage 3: Transact (FScopedTransaction)
         +--> Stage 4: Execute
         |    |
         |    v
         |    FOliveLibraryCloner::Clone()
         |    |
         |    +--> LoadFullJson(template_id) from FOliveLibraryIndex
         |    +--> ResolveInheritanceChain(template_id)
         |    +--> ResolveParentClass (walk chain to native ancestor)
         |    +--> CreateBlueprint(path, parentClass)
         |    +--> LoadObject<UBlueprint>(path)
         |    +--> Blueprint->Modify()
         |    |
         |    +--> [Structure Phase]
         |    |    +--> CreateVariables (resolve types, demote if needed)
         |    |    +--> CreateComponents (resolve classes, skip if needed)
         |    |    +--> AddInterfaces (resolve, skip if needed)
         |    |    +--> CreateDispatchers
         |    |    +--> CreateFunctionSignatures (empty graphs with params)
         |    |    +--> CompileBlueprint (intermediate, for FindFunction)
         |    |
         |    +--> [Graph Phase] (if mode != Structure)
         |    |    for each graph in template:
         |    |    +--> CloneGraph()
         |    |         +--> Phase 1: ClassifyNodes
         |    |         |    for each node:
         |    |         |    +--> Resolve owning_class / function / variable
         |    |         |    +--> Assign disposition (Create/Skip)
         |    |         |
         |    |         +--> Phase 2: CreateNodes
         |    |         |    for each Create node:
         |    |         |    +--> Map type to OliveNodeTypes
         |    |         |    +--> FOliveNodeFactory::CreateNode()
         |    |         |    +--> Store in NodeMap
         |    |         |
         |    |         +--> Phase 3: WireExec
         |    |         |    for each exec pin with connections:
         |    |         |    +--> Find source & target UEdGraphPin*
         |    |         |    +--> FOlivePinConnector::Connect()
         |    |         |
         |    |         +--> Phase 4: WireData
         |    |         |    for each data pin with connections:
         |    |         |    +--> FOlivePinConnector::Connect(bAllowConversion=true)
         |    |         |
         |    |         +--> Phase 5: SetDefaults
         |    |         |    for each pin with default:
         |    |         |    +--> Pin->DefaultValue = value
         |    |         |
         |    |         +--> Phase 6: AutoLayout
         |    |              +--> FOliveGraphLayoutEngine
         |    |
         |    +--> FOliveCompileManager::Compile(Blueprint)
         |    +--> Assemble FLibraryCloneResult
         |
         +--> Stage 5: Verify (compile check)
         +--> Stage 6: Report (build FOliveToolResult)
```

---

## 13. Critical Ordering Constraints

These invariants MUST be maintained in the implementation. Violating any of them causes subtle, hard-to-debug failures.

```
┌─────────────────────────────────────────────────────────┐
│  1. CreateBlueprint(path, parentClass)                  │
│  2. CreateComponents()          ← before bound events   │
│  3. CreateVariables()           ← before get/set nodes  │
│  4. AddInterfaces()             ← before interface events│
│  5. CreateDispatchers()         ← before bind/call nodes │
│  6. CreateFunctionSignatures()  ← empty, with params    │
│  7. *** INTERMEDIATE COMPILE *** ← CRITICAL             │
│     FKismetEditorUtilities::CompileBlueprint()           │
│     This populates SkeletonGeneratedClass so that        │
│     FindFunction can resolve sibling function calls.     │
│     Without this, any CallFunction node targeting a      │
│     function defined in THIS Blueprint will fail.        │
│  8. CloneGraph() for each graph ← nodes + wiring        │
│  9. FINAL COMPILE               ← validation            │
└─────────────────────────────────────────────────────────┘
```

The intermediate compile (step 7) is not optional. It is the mechanism by which `FindFunction` step 3 (Blueprint `FunctionGraphs` + `SkeletonGeneratedClass`) works for self-referencing Blueprints. This is the same pattern that `FOliveTemplateSystem::ApplyTemplate()` uses for factory templates.

---

## 14. Broken Execution Chains from Skipped Nodes

When portable mode skips nodes, it can leave gaps in exec chains:

```
[Event] → [Branch] → [SKIPPED: ProjectCall] → [SetVariable] → [Timer]
```

Without repair, `Branch.Then` connects to nothing, and `SetVariable` + `Timer` are orphaned.

### Exec Chain Repair Strategy

After Phase 3 (WireExec), run an **exec gap repair** pass:

1. For each skipped node, collect its incoming exec sources and outgoing exec targets.
2. If the skipped node had exactly one exec-in and one exec-out: **bridge** — reconnect the source directly to the target.
3. If the skipped node had multiple exec-outs (e.g., a Branch with True/False): **cannot bridge** — log warning, leave the gap. The AI must handle this manually.
4. If a chain of consecutive nodes are all skipped: bridge across the entire gap to the first non-skipped node.

```
Before repair:  [Branch] → [SKIP_A] → [SKIP_B] → [SetVar]
After repair:   [Branch] ─────────────────────────→ [SetVar]
```

This doesn't produce perfect Blueprints, but it produces *compilable* ones where the surviving logic is connected. The tool result clearly reports:
- `"exec_gaps_bridged": 3` — chains that were automatically repaired
- `"exec_gaps_unbridgeable": 1` — multi-output skips the AI needs to fix

### Result Reporting for Skip Gaps

The per-graph result includes a `skip_impact` section:

```json
{
  "skip_impact": {
    "exec_gaps_bridged": 3,
    "exec_gaps_unbridgeable": 1,
    "unbridgeable_details": [
      "Branch node_12 (checking CombatStatus) had True/False outputs — both targets differ, cannot auto-bridge"
    ],
    "orphaned_chains": 0
  }
}
```

---

## 15. Timeline Node Handling

Timeline nodes in library templates contain NO track data (curves, events) — the extraction pipeline doesn't capture UTimeline internals. Creating an empty Timeline node produces a shell that:
- Takes up graph space
- Shows "No tracks" in the editor
- Has output pins (Play, Stop, Update, Finished) that connect to nothing useful

**Decision:** Skip Timeline nodes in `structure` and `portable` modes. Only create them in `full` mode (where the user explicitly wants everything, broken or not).

In portable mode, Timeline nodes get disposition `Skip` with warning: "Timeline 'FireTimeline' skipped — track data not available in library format. Recreate manually or use plan_json with delay/timer nodes."

If a Timeline node's exec output connected to downstream nodes, the exec gap repair (Section 14) bridges past it where possible.

---

## 16. Implementation Order

### Phase 1: Core Cloner (structure only)
**Files:** `OliveLibraryCloner.h`, `OliveLibraryCloner.cpp`
**Estimated:** ~800 lines

1. `FLibraryCloneResult` struct with `ToJson()`
2. `ResolveParentClass()` — root native ancestor strategy
3. `ResolveClass()` / `ApplyRemap()` — resolution pipeline
4. `CreateVariables()` — with type demotion logic
5. `CreateComponents()` — with hierarchy, property application, skip logic
6. `AddInterfaces()` — with skip logic
7. `CreateDispatchers()` — with param type resolution
8. `CreateFunctionSignatures()` — empty function graphs with input/output params
9. **Intermediate compile** (see Section 13)
10. `Clone()` orchestrator — structure-only path
11. `TrackUnresolved()` / remap suggestion generation

**Deliverable:** `mode: "structure"` fully working. Test with `combatfs_arrow_component` — verify variables created, types demoted where needed, remap suggestions generated.

### Phase 2: Graph Cloning (portable mode)
**Estimated:** ~1200 lines (the bulk of complexity)

1. `ClassifyNodes()` — pre-classification with resolution. Timeline → `Skip`.
2. `CreateNodes()` — type mapping table, special cases (self-calls, interface calls, event reuse)
3. `FindPinByName()` — exact + case-insensitive fallback
4. `WireExecConnections()`
5. **Exec gap repair pass** (Section 14)
6. `WireDataConnections()` — with autocast, skip-aware
7. `SetPinDefaults()` — with asset reference detection + clearing
8. Auto-layout integration

**Deliverable:** `mode: "portable"` fully working. Test with `combatfs_arrow_component` — verify engine-resolvable nodes created, project-specific nodes skipped, exec chains repaired.

### Phase 3: Tool Handler + Schema
**Estimated:** ~200 lines

1. `BlueprintCreateFromLibrary()` schema in `OliveBlueprintSchemas.cpp`
2. `HandleBlueprintCreateFromLibrary()` in `OliveBlueprintToolHandlers.cpp`
3. Registration in `RegisterTemplateTools()`
4. Pipeline integration (`FOliveWriteRequest`, confirmation tier)

**Deliverable:** Tool callable via MCP.

### Phase 4: Full Mode + Polish
**Estimated:** ~300 lines

1. `mode: "full"` — create all nodes regardless, with error annotations
2. Warning aggregation (collapse repeated warnings per unresolved class)
3. `graphs` whitelist parameter support
4. `has_design_warnings` and `remap_suggestions` in result

### Phase 5: Knowledge Update
1. Update `Content/SystemPrompts/Knowledge/` with library clone guidance
2. Update template catalog description to mention `create_from_library`
3. Optional: recipe for "adapt a library template" workflow

### Total Estimate: ~2500 lines across 2 new files + modifications to 3 existing files.

---

## 17. Prompt/Knowledge Updates

### 14.1 System Prompt Addition

Add to the template section in the Auto/Blueprint profile knowledge:

```
## Library Template Cloning

`blueprint.create_from_library` clones extracted Blueprint patterns into real assets.

Three modes:
- `structure`: Variables, components, function signatures. No node graphs. Use when you want the API surface and will write your own logic.
- `portable` (default): Structure + all nodes with engine-resolvable functions. Skips project-specific nodes. Best for most cases.
- `full`: Everything. Broken references become warnings. Use for learning/reference.

The `remap` parameter maps source project types to your project types:
{"BP_PlayerCharacter_C": "BP_MyCharacter", "S_ItemData": "S_MyData"}

Typical workflow:
1. `list_templates(query="...")` to find relevant templates
2. `get_template(id)` to inspect structure and graphs
3. `create_from_library(id, path, remap={...})` to clone
4. Fix any design warnings using granular tools or plan_json
```

### 14.2 Catalog Update

`GetCatalogBlock()` already mentions library templates and search. Add a one-liner:

```
Use blueprint.create_from_library(template_id, path) to clone a library template into a real Blueprint.
```

---

## 18. Testing Strategy

### Unit Tests (in `Source/OliveAIEditor/Private/Tests/`)

1. **Resolution tests:** `ApplyRemap()` with various name formats (`_C` suffix, no suffix, exact match, case insensitive)
2. **Root native ancestor:** Template chain A->B->C->Actor, verify correct parent is used
3. **Type demotion:** Object to base Object, Struct to String, Enum to Byte
4. **Node classification:** All library node types correctly classified per mode

### Integration Tests

1. Clone a simple template with all-native dependencies (should work perfectly)
2. Clone `combatfs_arrow_component` (ActorComponent, native parent, no interface deps, lots of functions)
3. Clone `combatfs_bp_arrow_parent` (has interface, has components with source-project refs)
4. Clone with explicit remap map
5. Clone with `parent_class_override`
6. Clone in each mode and verify node/skip counts

---

## 19. Future Enhancements (NOT in V1)

1. **Recursive chain creation:** Create the entire inheritance chain as real BPs.
2. **Smart remap inference:** Auto-detect likely remaps by matching variable names/types in the user's project.
3. **Selective graph cloning:** Clone individual functions from a template into an existing BP (not just new BPs).
4. **Diff-based update:** After cloning, if the source template is updated, show what changed and offer to re-clone.
5. **ControlRig node support:** For Animation Blueprint templates.
