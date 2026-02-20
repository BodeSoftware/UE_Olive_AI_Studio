# Blueprint Type Map Completion and Phase 1 IR Schema Lock

**Design Document**
**Version:** 1.0
**Date:** 2025-02-19
**Author:** Architect Agent

---

## 1. Executive Summary

This design document addresses two related tasks for the Olive AI Studio plugin:

1. **Blueprint Type Map Finalization** - Complete the type detection and constraint system for all Blueprint types across Support Tiers 1 and 2
2. **Phase 1 IR Schema Lock** - Formalize the Intermediate Representation schema with documentation, examples, and validation rules

The existing implementation provides a solid foundation. This design identifies gaps and specifies the additions needed for completeness.

---

## 2. Current Implementation Analysis

### 2.1 What Already Exists

**Blueprint Type Detection (`OliveBlueprintTypes.h/cpp`):**
- `EOliveBlueprintType` enum with 14 types (13 + Unknown)
- `FOliveBlueprintCapabilities` with 11 capability flags
- `FOliveBlueprintTypeDetector::DetectType()` - Two-stage detection
- `FOliveBlueprintConstraints` with 4 validation methods
- `OliveBPConstraints` namespace with 13 constraint identifiers

**IR Types (`OliveAIRuntime/IR/`):**
- `FOliveIRType` - Complete type representation with 24 categories
- `FOliveIRPin`, `FOliveIRNode`, `FOliveIRGraph` - Graph primitives
- `FOliveIRVariable`, `FOliveIRComponent` - Blueprint elements
- `FOliveIRFunctionSignature`, `FOliveIRFunctionParam` - Function definitions
- `FOliveIRBlueprint` - Top-level Blueprint representation
- `FOliveIRCompileError`, `FOliveIRCompileResult` - Compilation structures
- Extended types: `FOliveIRWidgetNode`, `FOliveIRAnimState`, `FOliveIRAnimStateMachine`

### 2.2 Gaps Identified

| Gap | Description | Severity |
|-----|-------------|----------|
| **G1** | Editor Utility Widget not detected separately from Editor Utility Blueprint | Medium |
| **G2** | Actor Component SCS constraint missing SceneComponent check | High |
| **G3** | Control Rig Blueprint detection not implemented (class check) | Low (Tier 2) |
| **G4** | AnimNotify detection uses string matching instead of `IsChildOf()` | Medium |
| **G5** | GameplayAbility detection uses string matching instead of proper class check | Medium |
| **G6** | IR enum `EOliveIRBlueprintType` lacks Animation, Widget, ControlRig types | Medium |
| **G7** | Read/Write capability flags missing for Tier 2 partial support | Medium |
| **G8** | No IR schema documentation file exists | High |
| **G9** | No JSON schema validation for IR structures | Medium |
| **G10** | No IR versioning mechanism | Medium |

---

## 3. Blueprint Type Map Completion

### 3.1 Updated Type Detection Logic

The current `DetectType()` uses a two-stage approach which is correct. However, several parent class checks use string matching instead of proper class inheritance checks.

#### 3.1.1 Required Changes to `DetectType()`

```cpp
// Current (problematic):
if (ParentClassName.Contains(TEXT("AnimNotify")))

// Required (correct):
if (Blueprint->ParentClass->IsChildOf(UAnimNotify::StaticClass()))
```

**Full updated detection logic (pseudocode):**

```cpp
EOliveBlueprintType DetectType(const UBlueprint* Blueprint)
{
    // Stage 1: Check Blueprint asset class (most specific first)
    if (Blueprint->IsA<UAnimBlueprint>()) return AnimationBlueprint;
    if (Blueprint->IsA<UWidgetBlueprint>())
    {
        // Check if it's an Editor Utility Widget
        if (Blueprint->IsA<UEditorUtilityWidgetBlueprint>())
            return EditorUtilityWidget;  // NEW TYPE
        return WidgetBlueprint;
    }
    if (Blueprint->IsA<UControlRigBlueprint>()) return ControlRigBlueprint;  // NEEDS INCLUDE
    if (Blueprint->IsA<UEditorUtilityBlueprint>()) return EditorUtility;
    if (Blueprint->IsA<ULevelScriptBlueprint>()) return LevelScript;

    // Stage 2: Check BlueprintType enum
    switch (Blueprint->BlueprintType)
    {
    case BPTYPE_Normal:
        // Sub-classify by parent class using IsChildOf()
        if (ParentClass->IsChildOf(UAnimNotifyState::StaticClass()))
            return AnimNotifyState;
        if (ParentClass->IsChildOf(UAnimNotify::StaticClass()))
            return AnimNotify;
        if (ParentClass->IsChildOf(USceneComponent::StaticClass()))
            return ActorComponent;  // Can have SCS
        if (ParentClass->IsChildOf(UActorComponent::StaticClass()))
            return ActorComponent;  // Cannot have SCS (non-scene)
        if (ParentClass->IsChildOf(UGameplayAbility::StaticClass()))  // NEEDS GAS CHECK
            return GameplayAbility;
        if (ParentClass->IsChildOf(UEditorUtilityActor::StaticClass()) ||
            ParentClass->IsChildOf(UEditorUtilityObject::StaticClass()))
            return EditorUtility;
        return Normal;

    case BPTYPE_Interface: return Interface;
    case BPTYPE_FunctionLibrary: return FunctionLibrary;
    case BPTYPE_MacroLibrary: return MacroLibrary;
    case BPTYPE_LevelScript: return LevelScript;
    default: return Unknown;
    }
}
```

### 3.2 Updated Type Enum

Add `EditorUtilityWidget` as a distinct type:

```cpp
UENUM()
enum class EOliveBlueprintType : uint8
{
    // Standard K2 types (Tier 1)
    Normal,
    Interface,
    FunctionLibrary,
    MacroLibrary,
    ActorComponent,
    AnimNotify,
    AnimNotifyState,
    EditorUtility,
    EditorUtilityWidget,   // NEW - separate from EditorUtility
    GameplayAbility,

    // Extended systems (Tier 2)
    AnimationBlueprint,
    WidgetBlueprint,
    ControlRigBlueprint,

    // Level
    LevelScript,

    Unknown
};
```

### 3.3 ActorComponent SCS Constraint

Actor Components should only have SCS (Simple Construction Script / component hierarchy) if they inherit from `USceneComponent`. A non-scene component (e.g., `UMovementComponent`) cannot have child components attached.

**New capability flag needed:**

```cpp
struct FOliveBlueprintCapabilities
{
    // Existing flags...

    /** Whether this Component BP inherits from SceneComponent (can have SCS) */
    bool bIsSceneComponent = false;  // NEW
};
```

**New validation method:**

```cpp
static FOliveValidationResult ValidateAddComponentToComponent(
    const UBlueprint* Blueprint,
    const FString& ComponentClass);
```

**New constraint identifier:**

```cpp
namespace OliveBPConstraints
{
    /** Non-scene Actor Components cannot have child components */
    const FString ActorComponentNoSCS = TEXT("BP_CONSTRAINT_ACTORCOMP_NO_SCS");
}
```

### 3.4 Complete Capability Matrix

| Type | EventGraph | Functions | Variables | Components | Macros | Static Funcs | Special |
|------|------------|-----------|-----------|------------|--------|--------------|---------|
| Normal | Yes | Yes | Yes | Yes | Yes | No | - |
| Interface | No | Yes (sig only) | No | No | No | No | FunctionsMustBePublic |
| FunctionLibrary | No | Yes | No | No | No | Yes (required) | FunctionsMustBeStatic |
| MacroLibrary | No | No | No | No | Yes | No | - |
| ActorComponent | Yes | Yes | Yes | If SceneComponent | Yes | No | bIsSceneComponent |
| AnimNotify | Yes | Yes | Yes | No | Yes | No | - |
| AnimNotifyState | Yes | Yes | Yes | No | Yes | No | - |
| EditorUtility | Yes | Yes | Yes | If Actor-based | Yes | No | bEditorOnly |
| EditorUtilityWidget | Yes | Yes | Yes | No | Yes | No | bEditorOnly, bHasWidgetTree |
| GameplayAbility | Yes | Yes | Yes | No | Yes | No | - |
| AnimationBlueprint | Yes | Yes | Yes | No | Yes | No | bHasAnimGraph, bHasStateMachines |
| WidgetBlueprint | Yes | Yes | Yes | No | Yes | No | bHasWidgetTree |
| ControlRigBlueprint | No | Yes | Yes | No | No | No | Read-only for Phase 1 |
| LevelScript | Yes | Yes | Yes | No | Yes | No | - |

### 3.5 Tier 2 Read/Write Capability Flags

For partial support types (Animation BP, Widget BP, Control Rig), add write capability flags:

```cpp
struct FOliveBlueprintCapabilities
{
    // Existing flags...

    // Tier 2 write limitations
    bool bCanWriteEventGraph = true;      // True for AnimBP, WidgetBP; false for ControlRig
    bool bCanWriteAnimGraph = false;       // Partial for AnimBP
    bool bCanWriteStateMachine = false;    // Partial for AnimBP
    bool bCanWriteWidgetTree = false;      // Partial for WidgetBP

    // Editor-only flag
    bool bEditorOnly = false;
};
```

### 3.6 Required Includes and Conditional Compilation

Some types require optional module headers:

```cpp
// OliveBlueprintTypes.cpp

// Standard includes (always available)
#include "Animation/AnimBlueprint.h"
#include "Blueprint/WidgetBlueprint.h"
#include "EditorUtilityBlueprint.h"
#include "EditorUtilityWidgetBlueprint.h"  // NEW
#include "Engine/LevelScriptBlueprint.h"
#include "Animation/AnimNotify.h"          // For class checks
#include "Animation/AnimNotifyState.h"     // For class checks

// GAS (GameplayAbilities plugin - optional)
#if WITH_GAMEPLAY_ABILITIES
#include "Abilities/GameplayAbility.h"
#endif

// Control Rig (optional)
#if WITH_CONTROLRIG
#include "ControlRigBlueprint.h"
#endif
```

**Build.cs additions:**

```cs
// OliveAIEditor.Build.cs
PrivateDependencyModuleNames.AddRange(new string[]
{
    // Existing...
    "UMGEditor",  // For UEditorUtilityWidgetBlueprint
});

// Optional plugins
if (Target.bBuildWithEditorOnlyData)
{
    PrivateDependencyModuleNames.Add("GameplayAbilities");  // Optional
    PrivateDependencyModuleNames.Add("ControlRig");         // Optional
}
```

---

## 4. Phase 1 IR Schema Lock

### 4.1 Schema Documentation Structure

Create a formal schema specification at:
`docs/ir-schema/blueprint-ir-schema-v1.md`

With supporting files:
- `docs/ir-schema/examples/simple-blueprint.json`
- `docs/ir-schema/examples/function-with-logic.json`
- `docs/ir-schema/examples/interface-blueprint.json`
- `Source/OliveAIRuntime/Public/IR/OliveIRSchema.h` (version constants, validation)

### 4.2 IR Schema Version

```cpp
// OliveIRSchema.h

namespace OliveIR
{
    /** Current IR schema version */
    constexpr int32 SchemaVersionMajor = 1;
    constexpr int32 SchemaVersionMinor = 0;

    /** Schema version string for JSON */
    const FString SchemaVersion = TEXT("1.0");

    /** Minimum supported schema version for reading */
    constexpr int32 MinSupportedMajor = 1;
    constexpr int32 MinSupportedMinor = 0;
}
```

### 4.3 IR Design Rules (Locked for Phase 1)

These rules are now **locked** and must not change without a major version bump:

| Rule | Description | Example |
|------|-------------|---------|
| **R1** | Node IDs are simple strings | `"node_1"`, `"node_2"`, `"entry"` |
| **R2** | Connections use `node_id.pin_name` format | `"node_1.ReturnValue"` |
| **R3** | Pin types use `EOliveIRTypeCategory` | `"Bool"`, `"Object"`, `"Struct"` |
| **R4** | Positions are omitted (auto-layout on write) | No `x`, `y` fields |
| **R5** | Inherited members have `defined_in` field | `"defined_in": "Actor"` |
| **R6** | Empty/null values omitted from JSON | Don't serialize `""` or `null` |
| **R7** | Arrays serialize even if empty | `"inputs": []` |
| **R8** | Schema version included in root | `"schema_version": "1.0"` |

### 4.4 Updated FOliveIRBlueprint

Add schema version to top-level IR:

```cpp
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBlueprint
{
    GENERATED_BODY()

    /** IR schema version */
    UPROPERTY()
    FString SchemaVersion = TEXT("1.0");  // NEW

    // Existing fields...
};
```

### 4.5 IR Validation

Add schema validation utilities:

```cpp
// OliveIRSchema.h

/**
 * Validates IR JSON against schema rules
 */
class OLIVEAIRUNTIME_API FOliveIRValidator
{
public:
    /**
     * Validate a Blueprint IR JSON object
     * @param Json The JSON to validate
     * @return Validation result with any errors
     */
    static FOliveIRResult ValidateBlueprintIR(const TSharedPtr<FJsonObject>& Json);

    /**
     * Validate a Graph IR JSON object
     * @param Json The JSON to validate
     * @return Validation result with any errors
     */
    static FOliveIRResult ValidateGraphIR(const TSharedPtr<FJsonObject>& Json);

    /**
     * Validate a Node IR JSON object
     * @param Json The JSON to validate
     * @return Validation result with any errors
     */
    static FOliveIRResult ValidateNodeIR(const TSharedPtr<FJsonObject>& Json);

    /**
     * Check if a connection string is valid format
     * @param Connection The connection string (e.g., "node_1.pin_name")
     * @return True if valid format
     */
    static bool IsValidConnectionString(const FString& Connection);

    /**
     * Parse a connection string into node ID and pin name
     * @param Connection The connection string
     * @param OutNodeId Output node ID
     * @param OutPinName Output pin name
     * @return True if parsed successfully
     */
    static bool ParseConnectionString(
        const FString& Connection,
        FString& OutNodeId,
        FString& OutPinName);

    /**
     * Check schema version compatibility
     * @param Version The version string to check
     * @return True if compatible with current schema
     */
    static bool IsSchemaVersionCompatible(const FString& Version);
};
```

### 4.6 Full IR Examples

#### Example 1: Simple Actor Blueprint

```json
{
  "schema_version": "1.0",
  "name": "BP_HealthPickup",
  "path": "/Game/Blueprints/BP_HealthPickup",
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
  "variables": [
    {
      "name": "HealAmount",
      "type": {
        "category": "Float"
      },
      "default_value": "50.0",
      "defined_in": "self",
      "blueprint_read_write": true,
      "expose_on_spawn": true
    }
  ],
  "components": [
    {
      "name": "DefaultSceneRoot",
      "component_class": "SceneComponent",
      "is_root": true,
      "children": [
        {
          "name": "PickupMesh",
          "component_class": "StaticMeshComponent",
          "properties": {
            "StaticMesh": "/Engine/BasicShapes/Sphere"
          }
        },
        {
          "name": "CollisionSphere",
          "component_class": "SphereComponent",
          "properties": {
            "SphereRadius": "100.0"
          }
        }
      ]
    }
  ],
  "event_graph_names": ["EventGraph"],
  "function_names": ["ApplyHealing"],
  "macro_names": [],
  "event_dispatchers": []
}
```

#### Example 2: Function Graph with Logic

```json
{
  "name": "ApplyHealing",
  "graph_type": "Function",
  "inputs": [
    {
      "name": "Target",
      "type": { "category": "Object", "class_name": "Actor" },
      "is_input": true
    }
  ],
  "outputs": [
    {
      "name": "bSuccess",
      "type": { "category": "Bool" },
      "is_input": false
    }
  ],
  "access": "public",
  "is_pure": false,
  "is_static": false,
  "nodes": [
    {
      "id": "entry",
      "type": "FunctionEntry",
      "title": "Apply Healing",
      "category": "FunctionEntry",
      "output_pins": [
        { "name": "exec", "is_exec": true },
        { "name": "Target", "type": { "category": "Object", "class_name": "Actor" } }
      ]
    },
    {
      "id": "node_1",
      "type": "CallFunction",
      "title": "Is Valid",
      "function_name": "IsValid",
      "owning_class": "KismetSystemLibrary",
      "category": "IsValid",
      "input_pins": [
        { "name": "Object", "type": { "category": "Object" }, "connection": "entry.Target" }
      ],
      "output_pins": [
        { "name": "ReturnValue", "type": { "category": "Bool" } }
      ]
    },
    {
      "id": "node_2",
      "type": "Branch",
      "title": "Branch",
      "category": "Branch",
      "input_pins": [
        { "name": "exec", "is_exec": true, "connection": "entry.exec" },
        { "name": "Condition", "type": { "category": "Bool" }, "connection": "node_1.ReturnValue" }
      ],
      "output_pins": [
        { "name": "True", "is_exec": true },
        { "name": "False", "is_exec": true }
      ]
    },
    {
      "id": "node_3",
      "type": "CallFunction",
      "title": "Add Health",
      "function_name": "AddHealth",
      "owning_class": "HealthComponent",
      "category": "CallFunction",
      "input_pins": [
        { "name": "exec", "is_exec": true, "connection": "node_2.True" },
        { "name": "Amount", "type": { "category": "Float" }, "default_value": "50.0" }
      ],
      "output_pins": [
        { "name": "exec", "is_exec": true }
      ]
    },
    {
      "id": "result_true",
      "type": "FunctionResult",
      "title": "Return Node",
      "category": "FunctionResult",
      "input_pins": [
        { "name": "exec", "is_exec": true, "connection": "node_3.exec" },
        { "name": "bSuccess", "type": { "category": "Bool" }, "default_value": "true" }
      ]
    },
    {
      "id": "result_false",
      "type": "FunctionResult",
      "title": "Return Node",
      "category": "FunctionResult",
      "input_pins": [
        { "name": "exec", "is_exec": true, "connection": "node_2.False" },
        { "name": "bSuccess", "type": { "category": "Bool" }, "default_value": "false" }
      ]
    }
  ],
  "node_count": 6,
  "connection_count": 6
}
```

#### Example 3: Blueprint Interface

```json
{
  "schema_version": "1.0",
  "name": "BPI_Damageable",
  "path": "/Game/Interfaces/BPI_Damageable",
  "type": "Interface",
  "capabilities": {
    "has_event_graph": false,
    "has_functions": true,
    "has_variables": false,
    "has_components": false,
    "has_macros": false
  },
  "variables": [],
  "components": [],
  "function_names": ["ApplyDamage", "GetHealth"],
  "graphs": [
    {
      "name": "ApplyDamage",
      "graph_type": "Function",
      "inputs": [
        {
          "name": "DamageAmount",
          "type": { "category": "Float" }
        },
        {
          "name": "DamageType",
          "type": { "category": "Class", "class_name": "DamageType" }
        }
      ],
      "outputs": [],
      "access": "public",
      "nodes": []
    },
    {
      "name": "GetHealth",
      "graph_type": "Function",
      "inputs": [],
      "outputs": [
        {
          "name": "CurrentHealth",
          "type": { "category": "Float" }
        }
      ],
      "access": "public",
      "is_const": true,
      "nodes": []
    }
  ]
}
```

### 4.7 Update EOliveIRBlueprintType

The IR-level enum needs to match the extended types for proper round-tripping:

```cpp
UENUM(BlueprintType)
enum class EOliveIRBlueprintType : uint8
{
    Normal,
    Interface,
    FunctionLibrary,
    MacroLibrary,
    LevelScript,

    // Extended types (Tier 2)
    AnimationBlueprint,   // NEW
    WidgetBlueprint,      // NEW
    ControlRigBlueprint,  // NEW (read-only)

    Unknown
};
```

Update conversion functions accordingly.

---

## 5. File Structure

### 5.1 New Files to Create

```
Source/OliveAIRuntime/Public/IR/
    OliveIRSchema.h         # Version constants, validator class

docs/ir-schema/
    blueprint-ir-schema-v1.md   # Full schema documentation
    examples/
        simple-blueprint.json
        function-with-logic.json
        interface-blueprint.json
        animation-blueprint.json
        widget-blueprint.json
```

### 5.2 Files to Modify

```
Source/OliveAIEditor/Blueprint/Public/
    OliveBlueprintTypes.h   # Add EditorUtilityWidget type, new capability flags

Source/OliveAIEditor/Blueprint/Private/
    OliveBlueprintTypes.cpp # Update DetectType(), add SceneComponent checks

Source/OliveAIRuntime/Public/IR/
    BlueprintIR.h           # Add SchemaVersion field, update enum
    CommonIR.h              # Add SchemaVersion to FOliveIRBlueprint

Source/OliveAIEditor/
    OliveAIEditor.Build.cs  # Add optional module dependencies
```

---

## 6. Implementation Order

The coder should implement in this order:

### Phase A: Blueprint Type Detection Fixes (1-2 hours)

1. Add proper includes for `AnimNotify`, `AnimNotifyState`, optional GAS/ControlRig
2. Update `DetectType()` to use `IsChildOf()` instead of string matching
3. Add `EditorUtilityWidget` type and detection
4. Add `bIsSceneComponent` flag to capabilities
5. Update `GetCapabilities()` for all types
6. Add `ValidateAddComponentToComponent()` method
7. Add `ActorComponentNoSCS` constraint
8. Update `ToIRType()` / `FromIRType()` conversions

### Phase B: IR Schema Infrastructure (1 hour)

1. Create `OliveIRSchema.h` with version constants
2. Add `FOliveIRValidator` class declaration
3. Add `SchemaVersion` field to `FOliveIRBlueprint`
4. Update `EOliveIRBlueprintType` enum with extended types
5. Update all `ToJson()` methods to include schema version

### Phase C: IR Validation Implementation (1-2 hours)

1. Implement `FOliveIRValidator::ValidateBlueprintIR()`
2. Implement `FOliveIRValidator::ValidateGraphIR()`
3. Implement `FOliveIRValidator::ValidateNodeIR()`
4. Implement connection string parsing utilities
5. Implement schema version compatibility check

### Phase D: Documentation (1 hour)

1. Create `docs/ir-schema/blueprint-ir-schema-v1.md`
2. Create example JSON files
3. Add inline documentation comments to IR structs

### Phase E: Testing (1 hour)

1. Test type detection for each Blueprint type
2. Test constraint validation
3. Test IR round-trip (serialize/deserialize)
4. Test schema validation

---

## 7. Test Cases

### 7.1 Type Detection Tests

| Test | Input | Expected Type |
|------|-------|---------------|
| TD-1 | Blueprint with parent AActor | Normal |
| TD-2 | Blueprint with parent UAnimNotify | AnimNotify |
| TD-3 | Blueprint with parent UAnimNotifyState | AnimNotifyState |
| TD-4 | Blueprint with parent UActorComponent | ActorComponent |
| TD-5 | Blueprint with parent USceneComponent | ActorComponent (bIsSceneComponent=true) |
| TD-6 | Blueprint with parent UGameplayAbility | GameplayAbility |
| TD-7 | UAnimBlueprint asset | AnimationBlueprint |
| TD-8 | UWidgetBlueprint asset | WidgetBlueprint |
| TD-9 | UEditorUtilityWidgetBlueprint asset | EditorUtilityWidget |
| TD-10 | UEditorUtilityBlueprint asset | EditorUtility |
| TD-11 | BPTYPE_Interface Blueprint | Interface |
| TD-12 | BPTYPE_FunctionLibrary Blueprint | FunctionLibrary |
| TD-13 | BPTYPE_MacroLibrary Blueprint | MacroLibrary |
| TD-14 | ULevelScriptBlueprint asset | LevelScript |

### 7.2 Constraint Validation Tests

| Test | Operation | Blueprint Type | Expected |
|------|-----------|----------------|----------|
| CV-1 | Add variable | Interface | Error: InterfaceNoVariables |
| CV-2 | Add component | FunctionLibrary | Error: FunctionLibraryNoComponents |
| CV-3 | Add event graph | Interface | Error: InterfaceNoEventGraph |
| CV-4 | Add non-static function | FunctionLibrary | Error: FunctionLibraryMustBeStatic |
| CV-5 | Add child component | ActorComponent (non-scene) | Error: ActorComponentNoSCS |
| CV-6 | Add child component | ActorComponent (scene) | Success |
| CV-7 | Add function | MacroLibrary | Error: MacroLibraryOnlyMacros |

### 7.3 IR Validation Tests

| Test | Input | Expected |
|------|-------|----------|
| IR-1 | Valid connection "node_1.exec" | Valid |
| IR-2 | Invalid connection "node1exec" | Error: Invalid format |
| IR-3 | Schema version "1.0" | Compatible |
| IR-4 | Schema version "2.0" | Error: Incompatible |
| IR-5 | Blueprint IR missing name | Error: Required field |
| IR-6 | Node IR missing id | Error: Required field |

---

## 8. Edge Cases and Error Handling

### 8.1 GAS Plugin Not Installed

When Gameplay Abilities plugin is not present:
- `UGameplayAbility::StaticClass()` will not compile
- Use `WITH_GAMEPLAY_ABILITIES` preprocessor guard
- Fall back to string-based detection with warning log
- Report as `Normal` type with warning

### 8.2 Control Rig Plugin Not Installed

When Control Rig plugin is not present:
- Use `WITH_CONTROLRIG` preprocessor guard
- Skip Control Rig detection entirely
- Any Control Rig Blueprint will be detected as `Unknown`

### 8.3 SceneComponent Subclass Detection at Runtime

For ActorComponent Blueprints, the parent class must be checked at the UBlueprint level, not the generated class, because:
- The Blueprint may not be compiled yet
- We need the declared parent, not the instantiated class

```cpp
bool IsSceneComponentBlueprint(const UBlueprint* Blueprint)
{
    if (Blueprint && Blueprint->ParentClass)
    {
        return Blueprint->ParentClass->IsChildOf(USceneComponent::StaticClass());
    }
    return false;
}
```

### 8.4 IR Schema Versioning Strategy

- **Major version bump**: Breaking changes (field removed, type changed)
- **Minor version bump**: Additive changes (new optional field)
- Readers must check version and fail gracefully for incompatible major versions
- Readers should ignore unknown fields (forward compatibility)

---

## 9. Summary for Coder

### Key Changes to Make

1. **OliveBlueprintTypes.h**
   - Add `EditorUtilityWidget` to enum
   - Add `bIsSceneComponent`, `bCanWriteEventGraph`, `bCanWriteAnimGraph`, `bCanWriteStateMachine`, `bCanWriteWidgetTree`, `bEditorOnly` flags
   - Add `ActorComponentNoSCS` constraint

2. **OliveBlueprintTypes.cpp**
   - Replace string-based parent class checks with `IsChildOf()` calls
   - Add proper includes with conditional compilation guards
   - Implement SceneComponent detection for ActorComponent type
   - Update capability matrix for all types

3. **BlueprintIR.h**
   - Add extended types to `EOliveIRBlueprintType`
   - Add `SchemaVersion` field to `FOliveIRBlueprint`

4. **New: OliveIRSchema.h**
   - Version constants
   - `FOliveIRValidator` class

5. **New: docs/ir-schema/**
   - Schema documentation
   - Example JSON files

### Build Dependencies

Add to `OliveAIEditor.Build.cs`:
- `UMGEditor` (for EditorUtilityWidgetBlueprint)
- Optional: `GameplayAbilities`, `ControlRig`

### Critical Invariants

- Never use string matching for class type detection when `IsChildOf()` is available
- Always check optional plugin availability with preprocessor guards
- Always include schema version in serialized IR
- Connection strings must always match `node_id.pin_name` format
- All IR validation must return structured errors, never throw

---

## 10. Appendix: Complete Constraint Reference

| Constraint ID | Applies To | Restriction | Suggestion |
|---------------|------------|-------------|------------|
| BP_CONSTRAINT_INTERFACE_NO_VARS | Interface | No variables | Use regular BP |
| BP_CONSTRAINT_INTERFACE_NO_COMPONENTS | Interface | No components | Use regular BP |
| BP_CONSTRAINT_INTERFACE_NO_EVENT_GRAPH | Interface | No event graph | Implement in BP |
| BP_CONSTRAINT_FUNCLIB_NO_EVENT_GRAPH | FunctionLibrary | No event graph | Use regular BP |
| BP_CONSTRAINT_FUNCLIB_STATIC | FunctionLibrary | Functions must be static | Mark static or use regular BP |
| BP_CONSTRAINT_FUNCLIB_NO_VARS | FunctionLibrary | No variables | Use local vars or regular BP |
| BP_CONSTRAINT_FUNCLIB_NO_COMPONENTS | FunctionLibrary | No components | Use regular BP |
| BP_CONSTRAINT_MACROLIB_ONLY_MACROS | MacroLibrary | Only macros allowed | Use FunctionLibrary |
| BP_CONSTRAINT_ANIMBP_NO_COMPONENTS | AnimationBlueprint | No components | Add to owning Actor |
| BP_CONSTRAINT_LEVELSCRIPT_NO_COMPONENTS | LevelScript | No components | Place Actors in level |
| BP_CONSTRAINT_ACTORCOMP_NO_SCS | ActorComponent (non-scene) | No child components | Inherit from SceneComponent |
| BP_CONSTRAINT_INVALID_VAR_NAME | All | Invalid name format | Use alphanumeric + underscore |
| BP_CONSTRAINT_VAR_NAME_CONFLICT | All | Name already exists | Choose different name |
| BP_CONSTRAINT_COMPONENT_NOT_FOUND | All | Component class not found | Verify class name |
