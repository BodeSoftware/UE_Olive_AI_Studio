# Research: Blueprint Interface (BPI) Creation and Usage APIs — UE 5.5

## Question
How do we programmatically create Blueprint Interface assets, add functions to them, implement
them on existing Blueprints, and use interface-related nodes (UK2Node_Message,
DoesImplementInterface, K2Node_EnhancedInputAction) in the Olive AI Studio plugin?
Also: how to fix the add_node ghost-node problem for UK2Node_VariableGet/Set.

---

## Findings

### 1. Creating a Blueprint Interface Asset

**Factory class:** `UBlueprintInterfaceFactory` in
`Engine/Source/Editor/UnrealEd/Classes/Factories/BlueprintInterfaceFactory.h`

```cpp
#include "Factories/BlueprintInterfaceFactory.h"
```

`UBlueprintInterfaceFactory` inherits from `UBlueprintFactory`. Its constructor sets:
```cpp
ParentClass = UInterface::StaticClass();
BlueprintType = BPTYPE_Interface;
SupportedClass = UBlueprint::StaticClass();
```

Its `FactoryCreateNew` calls:
```cpp
FKismetEditorUtilities::CreateBlueprint(
    UInterface::StaticClass(),  // ParentClass
    InParent,                   // Outer (package)
    Name,
    BPTYPE_Interface,
    UBlueprint::StaticClass(),
    UBlueprintGeneratedClass::StaticClass(),
    CallingContext);
```

So the correct programmatic approach is:
```cpp
// Option A: Use the factory (asset registry-friendly, matches content browser behavior)
UBlueprintInterfaceFactory* Factory = NewObject<UBlueprintInterfaceFactory>();
UBlueprint* BPI = Cast<UBlueprint>(
    Factory->FactoryCreateNew(
        UBlueprint::StaticClass(),
        Package,
        AssetName,
        RF_Public | RF_Standalone,
        nullptr,
        GWarn,
        NAME_None));

// Option B: Call FKismetEditorUtilities directly
UBlueprint* BPI = FKismetEditorUtilities::CreateBlueprint(
    UInterface::StaticClass(),
    Package,
    AssetName,
    BPTYPE_Interface,
    UBlueprint::StaticClass(),
    UBlueprintGeneratedClass::StaticClass());
```

Both produce a `UBlueprint` with `BlueprintType == BPTYPE_Interface` and
`ParentClass == UInterface::StaticClass()`.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/UnrealEd/Private/Factories/EditorFactories.cpp`, lines 6982–7043

---

### 2. Adding Functions (with Signatures) to a Blueprint Interface

Interface BPs store their function signatures as `UEdGraph` objects in `Blueprint->FunctionGraphs`.
These graphs contain only an entry node (`UK2Node_FunctionEntry`) and a result node
(`UK2Node_FunctionResult`) — there is no implementation body.

**Key: use the same `AddFunctionGraph` path the Blueprint Editor uses for interface BPs:**

```cpp
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_EditablePinBase.h"

// 1. Create the graph
UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
    BPI,
    FName(*FunctionName),
    UEdGraph::StaticClass(),
    UEdGraphSchema_K2::StaticClass());

// 2. Register as a function graph (bIsUserCreated=true, nullptr signature
//    means no existing UFunction to copy signature from)
FBlueprintEditorUtils::AddFunctionGraph<UClass>(
    BPI,
    FuncGraph,
    /*bIsUserCreated=*/ true,
    /*SignatureFromObject=*/ nullptr);

// 3. Find the entry node and add input/output pins
TWeakObjectPtr<UK2Node_EditablePinBase> EntryNodePtr, ResultNodePtr;
FBlueprintEditorUtils::GetEntryAndResultNodes(FuncGraph, EntryNodePtr, ResultNodePtr);

UK2Node_EditablePinBase* EntryNode = EntryNodePtr.Get();

// 4. Build the FEdGraphPinType for each parameter
FEdGraphPinType PinType;
PinType.PinCategory = UEdGraphSchema_K2::PC_Float;  // or PC_Int, PC_Boolean, PC_Object...
// For object references:
// PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
// PinType.PinSubCategoryObject = UActorComponent::StaticClass();

// 5. Add input pins on the entry node
EntryNode->CreateUserDefinedPin(
    FName(TEXT("MyParam")),
    PinType,
    EGPD_Output,      // entry node outputs feed into graph
    /*bUseUniqueName=*/ true);

// 6. Add output pins on the result node (if any — pure events have no outputs)
UK2Node_EditablePinBase* ResultNode = ResultNodePtr.Get();
if (ResultNode)
{
    FEdGraphPinType ReturnType;
    ReturnType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    ResultNode->CreateUserDefinedPin(FName(TEXT("ReturnValue")), ReturnType, EGPD_Input, true);
}

// 7. Mark modified
FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BPI);
```

**Important behavior notes:**
- Interface graphs are read-only in the Blueprint editor (`bGraphReadOnly = true`) — no body nodes allowed.
- Interface functions with **no output pins** are implemented as events in the implementing BP.
- Interface functions with **output pins** are implemented as functions in the implementing BP.
- `FBlueprintEditorUtils::IsInterfaceBlueprint()` simply checks `BlueprintType == BPTYPE_Interface`.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/UnrealEd/Public/Kismet2/BlueprintEditorUtils.h`, lines 329–424
Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Classes/K2Node_EditablePinBase.h`, lines 178

---

### 3. Adding an Interface to an Existing Blueprint (`ImplementNewInterface`)

```cpp
#include "Kismet2/BlueprintEditorUtils.h"

// Option A: By class name (short name — deprecated path, but still works via redirect)
bool bSuccess = FBlueprintEditorUtils::ImplementNewInterface(
    Blueprint,
    FName(TEXT("BPI_Interactable")));

// Option B: By full path (preferred in UE 5.5)
bool bSuccess = FBlueprintEditorUtils::ImplementNewInterface(
    Blueprint,
    FTopLevelAssetPath(TEXT("/Game/Interfaces/BPI_Interactable")));
```

What `ImplementNewInterface` does internally (source: BlueprintEditorUtils.cpp, line 6482):
1. Finds the `UClass` via `FindObject<UClass>(InterfaceClassPathName)`.
2. Checks `Blueprint->ImplementedInterfaces` for duplicates.
3. For each `UFunction` in the interface where `CanKismetOverrideFunction(Function) == true` AND
   `!FunctionCanBePlacedAsEvent(Function)` (i.e., it has outputs), creates a new `UEdGraph`.
4. Calls `FBlueprintEditorUtils::AddInterfaceGraph(Blueprint, NewGraph, InterfaceClass)` which
   calls `CreateFunctionGraphTerminators(*Graph, InterfaceClass)` on the schema.
5. Creates a new `FBPInterfaceDescription` with the graphs and appends to
   `Blueprint->ImplementedInterfaces`.
6. Calls `MarkBlueprintAsStructurallyModified`.

**Struct layout (Blueprint.h, line 261):**
```cpp
USTRUCT()
struct FBPInterfaceDescription
{
    UPROPERTY()
    TSubclassOf<class UInterface> Interface;   // The interface class

    UPROPERTY()
    TArray<TObjectPtr<UEdGraph>> Graphs;       // Implementation graphs per function
};
```

**Gotcha:** `ImplementNewInterface` checks for graph name conflicts with existing function graphs.
If a Blueprint already has a function named the same as an interface function, it fails with an
error. This is already handled in our `INTERFACE_GRAPH_CONFLICT` error code.

**Where stub graphs are stored:** `Blueprint->ImplementedInterfaces[i].Graphs[]` — NOT in
`Blueprint->FunctionGraphs`. They are the override implementations.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Engine/Blueprint.h`, line 261
Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/UnrealEd/Private/Kismet2/BlueprintEditorUtils.cpp`, line 6482

---

### 4. DoesImplementInterface Node — Class Pin Format

`UKismetSystemLibrary::DoesImplementInterface` signature:
```cpp
// Engine/Source/Runtime/Engine/Classes/Kismet/KismetSystemLibrary.h, line 226
static ENGINE_API bool DoesImplementInterface(
    const UObject* TestObject,
    TSubclassOf<UInterface> Interface);   // This becomes a PC_Class pin
```

The `Interface` parameter is a `TSubclassOf<UInterface>`, which in Blueprint produces a
`PC_Class` pin with `PinSubCategoryObject = UInterface::StaticClass()`.

**Critical: PC_Class pins must use DefaultObject, NOT DefaultValue.**

From `EdGraphSchema_K2.cpp` line 4233:
```
// For PC_Class pins:
// "Should have an object set but no string"
// String NewDefaultValue -> validation error
// UseDefaultObject must be a UClass* subclass of PinSubCategoryObject
```

**Two ways to set the Interface pin:**

Option A: `TrySetDefaultObject` (direct, correct approach):
```cpp
const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
UEdGraphPin* InterfacePin = Node->FindPin(TEXT("Interface"));

// Load the interface class (the _C generated class of the BPI asset)
UClass* InterfaceClass = LoadObject<UClass>(nullptr,
    TEXT("/Game/Interfaces/BPI_Interactable.BPI_Interactable_C"));

K2Schema->TrySetDefaultObject(*InterfacePin, InterfaceClass);
```

Option B: `TrySetDefaultValue` with the full object path (also works — the schema parses it):
```cpp
// TrySetDefaultValue calls GetPinDefaultValuesFromString which for PC_Class/PC_Object
// calls FSoftObjectPath::TryLoad() — so a valid asset path works
K2Schema->TrySetDefaultValue(*InterfacePin,
    TEXT("/Game/Interfaces/BPI_Interactable.BPI_Interactable_C"));
```

The format for the `_C` generated class path is: `{AssetPath}.{AssetName}_C`
Example: `/Game/Interfaces/BPI_Interactable.BPI_Interactable_C`

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp`, lines 4233–4270, 4781–4819

---

### 5. UK2Node_Message — Interface Function Calls

`UK2Node_Message` is a subclass of `UK2Node_CallFunction`. The key difference:
- `UK2Node_CallFunction` calls a function directly on a specific class.
- `UK2Node_Message` dispatches through the interface — creates a cast-to-interface at compile time.
  Its Self pin is generic `UObject*` (not the specific interface type), which is the "Call
  Interface Function" node you see in the Blueprint editor.

**This is already implemented in `OliveNodeFactory.cpp` (line 302).** The existing code is correct:

```cpp
// OliveNodeFactory.cpp line 302 — already correct implementation
UK2Node_Message* MessageNode = NewObject<UK2Node_Message>(Graph);
// SetFromField with bIsConsideredSelfContext=false preserves interface class as MemberParent
MessageNode->FunctionReference.SetFromField<UFunction>(
    Function,
    /*bIsConsideredSelfContext=*/ false);
MessageNode->AllocateDefaultPins();
Graph->AddNode(MessageNode, false, false);
```

The `FunctionReference.SetFromField<UFunction>(Function, false)` sets:
- `MemberName` = function name
- `MemberParent` = the interface class (NOT nullptr, since bIsConsideredSelfContext=false)

This is how `BlueprintActionDatabase.cpp` spawns message nodes from the context menu (confirmed
pattern).

The `InterfaceSearch` match method in `FindFunction` already triggers this path. No new work
needed here.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Classes/K2Node_Message.h`
Source: `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`, lines 296–316

---

### 6. UK2Node_EnhancedInputAction

**Header:** `Engine/Plugins/EnhancedInput/Source/InputBlueprintNodes/Public/K2Node_EnhancedInputAction.h`
**Module:** `InputBlueprintNodes` (already in OliveAIEditor.Build.cs at line 116)

**Key UPROPERTY:**
```cpp
UPROPERTY()
TObjectPtr<const UInputAction> InputAction;  // IS a UPROPERTY — settable via reflection
```

**Exec output pins** (from `ForEachEventPinName` iterating `ETriggerEvent` enum, non-Hidden values):
- `Triggered` (shown by default, not advanced)
- `Started` (advanced/hidden by default unless configured)
- `Ongoing` (advanced/hidden)
- `Canceled` (advanced/hidden)
- `Completed` (advanced/hidden)

Default visibility is controlled by `UEnhancedInputEditorSettings::VisibleEventPinsByDefault`.
Only `Triggered` is visible by default. The others exist on the node but are advanced/hidden.

**Data output pins** (from `AllocateDefaultPins` lines 95–109):
- `ActionValue` — pin category/subcategory depends on the UInputAction's ValueType
  (Boolean→PC_Boolean, Axis1D→PC_Real, Axis2D→FVector2D, Axis3D→FVector)
- `ElapsedSeconds` — `PC_Real` (double), advanced/hidden by default
- `TriggeredSeconds` — `PC_Real` (double), advanced/hidden by default
- `InputAction` — `PC_Object` of `UInputAction` class, advanced/hidden (only present if InputAction is set)

**Using via `blueprint.add_node`:**
```
"node_type": "K2Node_EnhancedInputAction",
"properties": {
    "InputAction": "/Game/Input/Actions/IA_Jump"
}
```

`SetNodePropertiesViaReflection` handles this because `InputAction` IS a `UPROPERTY()`:
- It calls `StaticLoadObject(UInputAction::StaticClass(), nullptr, "/Game/Input/Actions/IA_Jump")`
- Sets the property via `ObjProp->SetObjectPropertyValue(ValuePtr, LoadedAction)`
- This happens BEFORE `AllocateDefaultPins()`, so the correct pins are generated

**Gotcha:** If `InputAction` is not set (null), `AllocateDefaultPins` still creates the exec pins
(Triggered, Started, etc.) but the `ActionValue` pin will use a generic/default type and the
`InputAction` output pin will be absent.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/EnhancedInput/Source/InputBlueprintNodes/Public/K2Node_EnhancedInputAction.h`
Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/EnhancedInput/Source/InputBlueprintNodes/Private/K2Node_EnhancedInputAction.cpp`, lines 63–112

---

### 7. K2Node_VariableGet/Set — Ghost Node Fix in CreateNodeByClass

**Root cause of the ghost node:** `UK2Node_Variable` stores variable identity in
`FMemberReference VariableReference` (a `UPROPERTY` struct). The individual fields
(`MemberName`, `MemberParent`, etc.) are protected members of `FMemberReference`. The `variable_name`
property name that AIs naturally use is NOT a top-level UPROPERTY on the node.

`SetNodePropertiesViaReflection` tries to find a `UPROPERTY` named `"variable_name"` — it does
not exist. So `VariableReference` is never set. Then `AllocateDefaultPins` calls
`GetPropertyForVariable()` which calls `ResolveMember<FProperty>(GetBlueprintClassFromNode())`.
At this point `GetBlueprintClassFromNode()` walks the outer chain and calls
`FindBlueprintForNode` — which can find the Blueprint because the node's outer IS the graph, which
will be in the Blueprint's FunctionGraphs/UbergraphPages. But `ResolveMember` with an empty
`MemberName` (NAME_None) returns null, so no pins are created.

**Fix pattern** (mirrors the `UK2Node_CallFunction` special-casing already in `CreateNodeByClass`):

```cpp
// In CreateNodeByClass, BEFORE AllocateDefaultPins, after SetNodePropertiesViaReflection:

if (NewNode->IsA<UK2Node_Variable>())
{
    // Extract variable_name from properties (check common aliases)
    FString VarName;
    const FString* VarNamePtr = Properties.Find(TEXT("variable_name"));
    if (!VarNamePtr) VarNamePtr = Properties.Find(TEXT("VariableName"));
    if (!VarNamePtr) VarNamePtr = Properties.Find(TEXT("MemberName"));
    if (VarNamePtr && !VarNamePtr->IsEmpty())
    {
        VarName = *VarNamePtr;
    }

    if (!VarName.IsEmpty())
    {
        UK2Node_Variable* VarNode = CastChecked<UK2Node_Variable>(NewNode);

        // Check if a target class is specified (for external member access)
        FString TargetClass;
        const FString* ClassPtr = Properties.Find(TEXT("variable_class"));
        if (!ClassPtr) ClassPtr = Properties.Find(TEXT("VariableClass"));

        if (!TargetClass.IsEmpty())
        {
            UClass* OwnerClass = FindClass(TargetClass);
            if (OwnerClass)
            {
                VarNode->VariableReference.SetExternalMember(FName(*VarName), OwnerClass);
            }
        }
        else
        {
            // Default: self-context (variable on the current Blueprint)
            VarNode->VariableReference.SetSelfMember(FName(*VarName));
        }
    }
    else
    {
        UE_LOG(LogOliveNodeFactory, Warning,
            TEXT("CreateNodeByClass: UK2Node_Variable subclass created without "
                 "variable_name property. Node will have 0 pins. "
                 "Pass 'variable_name' in properties."));
    }
}
```

`SetSelfMember(FName)` signature (from `MemberReference.h` line 187):
```cpp
ENGINE_API void SetSelfMember(FName InMemberName);
ENGINE_API void SetSelfMember(FName InMemberName, const FGuid& InMemberGuid);
```

`SetExternalMember` signature (line 177):
```cpp
ENGINE_API void SetExternalMember(FName InMemberName, TSubclassOf<class UObject> InMemberParentClass);
```

**After `AllocateDefaultPins`, the zero-pin guard should be extended** to also fire for
`UK2Node_Variable` subclasses, similar to the existing `UK2Node_CallFunction` guard.

**NOTE:** For `get_var`/`set_var` ops going through `OlivePlanOps`, the fix is NOT needed — those
routes call `CreateVariableGetNode`/`CreateVariableSetNode` directly (lines 2927–2931 in
OliveNodeFactory.cpp) which already call `SetSelfMember` correctly. The fix is only needed for
the `blueprint.add_node` tool (`CreateNodeByClass` path).

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Classes/K2Node_Variable.h`, line 56
Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Engine/MemberReference.h`, lines 177–188
Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_Variable.cpp`, lines 87–140
Source: `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`, lines 1686–1802

---

### 8. BPI-Related Module Headers to Include

For any new tool handler dealing with BPIs, the following includes are needed:

```cpp
// BPI creation + ImplementNewInterface
#include "Factories/BlueprintInterfaceFactory.h"    // in UnrealEd module
#include "Kismet2/BlueprintEditorUtils.h"           // in UnrealEd module
#include "Kismet2/KismetEditorUtilities.h"          // in UnrealEd module

// Function graph editing (entry/result nodes, pin creation)
#include "K2Node_EditablePinBase.h"                 // in BlueprintGraph module
#include "K2Node_FunctionEntry.h"                   // in BlueprintGraph module
#include "K2Node_FunctionResult.h"                  // in BlueprintGraph module

// FBPInterfaceDescription struct
#include "Engine/Blueprint.h"                       // in Engine module

// FMemberReference (for VariableGet/Set fix)
#include "Engine/MemberReference.h"                 // in Engine module
```

The `UnrealEd` module is already in `OliveAIEditor.Build.cs`.

---

## Recommendations

1. **BPI creation tool**: Use `FKismetEditorUtilities::CreateBlueprint(UInterface::StaticClass(), ...)`
   with `BPTYPE_Interface`. Do NOT go through `UBlueprintInterfaceFactory` for programmatic use —
   that factory is designed for the asset browser and opens a dialog on `ConfigureProperties`. Call
   `CreateBlueprint` directly and save the asset.

2. **BPI function graph authoring**: Use `AddFunctionGraph<UClass>(BPI, Graph, true, nullptr)` then
   find the entry/result nodes via `GetEntryAndResultNodes` and call `CreateUserDefinedPin` on them.
   This is the correct path — it goes through the schema's `CreateFunctionGraphTerminators`.

3. **ImplementNewInterface**: Use `FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath(...))`
   — the `FTopLevelAssetPath` overload (UE 5.5 preferred). The short `FName` overload does a
   `TryConvertShortTypeNameToPathName` internally. Note: function stub graphs created by this call
   live in `Blueprint->ImplementedInterfaces[i].Graphs`, NOT in `Blueprint->FunctionGraphs`.

4. **DoesImplementInterface class pin**: Must use `TrySetDefaultObject` (not `TrySetDefaultValue`)
   because PC_Class pins require `DefaultObject`, not a string. The full `_C` class path works with
   `TrySetDefaultValue` only because the schema parses it via `FSoftObjectPath::TryLoad()` — but
   `TrySetDefaultObject` with `LoadObject<UClass>()` is cleaner and explicit.

5. **EnhancedInputAction via add_node**: Already works because `InputAction` IS a `UPROPERTY()`.
   Pass `"InputAction": "/Game/Input/IA_MyAction"` as a property. The pin names for exec outputs
   are the exact enum value names: `"Triggered"`, `"Started"`, `"Ongoing"`, `"Canceled"`,
   `"Completed"`. Non-Triggered pins are `bAdvancedView = true` by default.

6. **VariableGet/Set ghost node fix**: Add special-casing in `CreateNodeByClass` for
   `UK2Node_Variable` subclasses, mirroring the existing `UK2Node_CallFunction` block. Extract
   `"variable_name"` from properties and call `VarNode->VariableReference.SetSelfMember(FName(*VarName))`.
   Also extend the zero-pin guard to catch `UK2Node_Variable` nodes with 0 pins, with actionable
   error directing the AI to use `get_var`/`set_var` ops instead of `add_node`.

7. **Interface call via K2Node_Message**: Already fully implemented and correct. No changes needed.
   The `InterfaceSearch` match method correctly routes to `UK2Node_Message` creation.

8. **set_pin_default for class pins**: The existing `SetPinDefault` tool uses `TrySetDefaultValue`
   which works for PC_Class pins IF the AI passes a valid full asset path ending in `_C`. The AI
   should be informed in documentation/prompts that interface class pins take the path
   `"/Path/To/BPI_Asset.BPI_Asset_C"` (the generated class, not the asset itself).
