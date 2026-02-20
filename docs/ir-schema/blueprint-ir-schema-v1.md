# Blueprint IR Schema v1.0

This document defines the Intermediate Representation (IR) schema for Blueprints in Olive AI Studio.

## Schema Version

**Version:** 1.0 (Locked for Phase 1)

The schema version is included in all IR JSON as the `schema_version` field.

## Design Rules (Locked and Enforced)

| Rule | Description | Example | Enforcement |
|------|-------------|---------|-------------|
| R1 | Node IDs must be `entry`, `result`, `result_<suffix>`, or `node_<number>` | `"node_1"`, `"entry"`, `"result_true"` | Strict validator rejects GUID-style IDs |
| R2 | Connections use `node_id.pin_name` format | `"node_1.ReturnValue"` | Validator rejects invalid format |
| R3 | Pin types use `EOliveIRTypeCategory` | `"Bool"`, `"Object"` | Serializer |
| R4 | Position fields are forbidden | Auto-layout on write | Strict validator rejects position fields |
| R5 | Variables must have `defined_in` field | `"defined_in": "self"` or `"defined_in": "Actor"` | Strict validator |
| R6 | Empty/null values omitted from JSON | Don't serialize `""` | Serializer |
| R7 | Arrays serialize even if empty | `"inputs": []` | Serializer |
| R8 | Schema version included in root | `"schema_version": "1.0"` | Validator checks compatibility |

### Forbidden Fields (Rule R4)

The following field names are forbidden and will be rejected by strict validation:

```
position_x, position_y, node_pos_x, node_pos_y, pos_x, pos_y,
location_x, location_y, node_position, graph_position, visual_position, editor_position
```

### Valid Node ID Patterns (Rule R1)

| Pattern | Use Case | Examples |
|---------|----------|----------|
| `entry` | Function entry point | `"entry"` |
| `result` | Single return node | `"result"` |
| `result_<suffix>` | Multiple return nodes | `"result_true"`, `"result_false"`, `"result_0"` |
| `node_<number>` | Regular nodes | `"node_1"`, `"node_2"`, `"node_42"` |

GUID-style IDs (e.g., `"A1B2C3D4-E5F6-7890-ABCD-EF1234567890"`) are **rejected**.

## Blueprint Type Support Tiers

### Tier 1 - Full Read/Write Support

| IR Type | Editor Types Mapped | Capabilities |
|---------|---------------------|--------------|
| `Normal` | Normal, AnimNotify, AnimNotifyState, GameplayAbility | Full event graph, functions, variables |
| `Interface` | Blueprint Interface | Function signatures only (no variables, components, event graph) |
| `FunctionLibrary` | Blueprint Function Library | Static functions only |
| `MacroLibrary` | Blueprint Macro Library | Macros only |
| `LevelScript` | Level Blueprint | Event graph, functions, variables (no components) |
| `ActorComponent` | Actor Component Blueprint | Event graph, functions, variables, SCS (if SceneComponent) |
| `EditorUtility` | Editor Utility Blueprint | Full capabilities, editor-only |
| `EditorUtilityWidget` | Editor Utility Widget | Event graph, widget tree (editor-only) |

### Tier 2 - Partial Support (Phase 1 Scope-Cut)

| IR Type | Read Support | Write Support | Notes |
|---------|--------------|---------------|-------|
| `AnimationBlueprint` | Full | Event graph only | Anim graph and state machines are **read-only** |
| `WidgetBlueprint` | Full | Event graph only | Widget tree is **read-only** |
| `ControlRigBlueprint` | Full | None (read-only) | Requires optional ControlRig plugin |

**Phase 1 Scope-Cut:** The capability flags `bCanWriteAnimGraph`, `bCanWriteStateMachine`, and `bCanWriteWidgetTree` are defined but **not wired to tool gating**. Tools that write to anim graphs, state machines, or widget trees do not exist in Phase 1. This is intentional - these systems require specialized graph manipulation beyond standard Blueprint K2 graphs.

**Future Work (Phase 2+):**
- Wire Tier 2 capability flags to tool execution
- Implement anim graph node creation/editing
- Implement state machine editing
- Implement widget tree manipulation

## Type Categories

```
Bool, Byte, Int, Int64, Float, Double, String, Name, Text
Vector, Vector2D, Rotator, Transform, Color, LinearColor
Object, Class, Interface, Struct, Enum
Exec, Wildcard
Array, Set, Map
Delegate, MulticastDelegate
Unknown
```

## Blueprint IR Structure

```json
{
  "schema_version": "1.0",
  "name": "BP_Example",
  "path": "/Game/Blueprints/BP_Example",
  "type": "Normal",
  "parent_class": {
    "name": "Actor",
    "source": "cpp"
  },
  "capabilities": {
    "has_event_graph": true,
    "has_functions": true,
    "has_variables": true,
    "has_components": true,
    "has_macros": true
  },
  "interfaces": [],
  "compile_status": "UpToDate",
  "variables": [],
  "components": [],
  "event_graph_names": ["EventGraph"],
  "function_names": [],
  "macro_names": [],
  "event_dispatchers": []
}
```

## Variable IR

```json
{
  "name": "Health",
  "type": {
    "category": "Float"
  },
  "default_value": "100.0",
  "defined_in": "self",
  "blueprint_read_write": true,
  "expose_on_spawn": false,
  "replicated": false
}
```

**Note:** The `defined_in` field is **required** in strict validation mode (Rule R5). Use `"self"` for variables defined in the current Blueprint, or the parent class name for inherited variables.

## Component IR

```json
{
  "name": "RootComponent",
  "component_class": "SceneComponent",
  "is_root": true,
  "children": [
    {
      "name": "Mesh",
      "component_class": "StaticMeshComponent",
      "properties": {
        "StaticMesh": "/Engine/BasicShapes/Cube"
      }
    }
  ]
}
```

## Graph IR

```json
{
  "name": "EventGraph",
  "graph_type": "EventGraph",
  "nodes": [],
  "node_count": 0,
  "connection_count": 0
}
```

## Node IR

```json
{
  "id": "node_1",
  "type": "CallFunction",
  "title": "Print String",
  "function_name": "PrintString",
  "owning_class": "KismetSystemLibrary",
  "category": "CallFunction",
  "input_pins": [
    {
      "name": "exec",
      "is_exec": true,
      "connection": "entry.exec"
    },
    {
      "name": "InString",
      "type": { "category": "String" },
      "default_value": "Hello"
    }
  ],
  "output_pins": [
    {
      "name": "exec",
      "is_exec": true
    }
  ]
}
```

## Pin IR

```json
{
  "name": "ReturnValue",
  "type": {
    "category": "Object",
    "class_name": "Actor"
  },
  "is_input": false,
  "is_exec": false,
  "connection": "node_2.Target",
  "default_value": ""
}
```

## Connection Format

Connections use the format: `node_id.pin_name`

Examples:
- `"entry.exec"` - Entry node's execution pin
- `"node_1.ReturnValue"` - Node 1's return value pin
- `"result_true.exec"` - True result node's execution pin

## Validation

Use `FOliveIRValidator` to validate IR JSON. **Strict mode is enabled by default** for Phase 1:

```cpp
// Strict validation (default) - enforces all locked rules
FOliveIRResult Result = FOliveIRValidator::ValidateBlueprintIR(JsonObject);

// Permissive validation - only checks required fields
FOliveIRResult Result = FOliveIRValidator::ValidateBlueprintIR(JsonObject, false);

if (!Result.bSuccess)
{
    UE_LOG(LogOliveAI, Error, TEXT("%s"), *Result.ErrorMessage);
    UE_LOG(LogOliveAI, Log, TEXT("Suggestion: %s"), *Result.Suggestion);
}
```

### Validation Error Codes

| Code | Rule | Description |
|------|------|-------------|
| `IR_INVALID_JSON` | - | JSON is null or malformed |
| `IR_MISSING_FIELD` | - | Required field (name, path, id) missing |
| `IR_INCOMPATIBLE_VERSION` | R8 | Schema version not compatible |
| `IR_INVALID_NODE_ID` | R1 | Node ID is not valid format (GUID-style rejected) |
| `IR_INVALID_CONNECTION` | R2 | Connection string not in `node_id.pin_name` format |
| `IR_FORBIDDEN_FIELD` | R4 | Position/layout field present |
| `IR_MISSING_DEFINED_IN` | R5 | Variable missing `defined_in` field |

## Examples

See the `examples/` directory for complete IR samples:
- `simple-blueprint.json` - Basic Actor Blueprint with variables and components
- `function-with-logic.json` - Function graph with branching logic
- `interface-blueprint.json` - Blueprint Interface with function signatures
