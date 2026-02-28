# Research: UE 5.5 Delegate Node APIs for Blueprint Graph

## Question

How to programmatically create `UK2Node_CallDelegate` and `UK2Node_AddDelegate` nodes
in a Blueprint graph. Covers class hierarchy, property lookup, initialization sequence,
and pin layout after `AllocateDefaultPins`.

---

## Findings

### 1. Class Hierarchy and Headers

All delegate node types inherit from `UK2Node_BaseMCDelegate`:

```
UK2Node
  └─ UK2Node_BaseMCDelegate          (abstract)
       ├─ UK2Node_CallDelegate        — "Call" / broadcast
       ├─ UK2Node_AddDelegate         — "Bind Event to"
       ├─ UK2Node_RemoveDelegate      — "Unbind Event from"
       └─ UK2Node_ClearDelegate       — "Unbind all Events from"
```

Header locations in UE 5.5 (`Engine/Source/Editor/BlueprintGraph/Classes/`):

| Class | Header |
|-------|--------|
| `UK2Node_BaseMCDelegate` | `K2Node_BaseMCDelegate.h` |
| `UK2Node_CallDelegate` | `K2Node_CallDelegate.h` |
| `UK2Node_AddDelegate` | `K2Node_AddDelegate.h` |
| `UK2Node_RemoveDelegate` | `K2Node_RemoveDelegate.h` |
| `UK2Node_ClearDelegate` | `K2Node_ClearDelegate.h` |

Implementation for all four is in one file:
`Engine/Source/Editor/BlueprintGraph/Private/K2Node_MCDelegate.cpp`

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Classes/K2Node_BaseMCDelegate.h`

---

### 2. The `SetFromProperty` API (How to Initialize)

`UK2Node_BaseMCDelegate` exposes a single public setter:

```cpp
// K2Node_BaseMCDelegate.h line 46
BLUEPRINTGRAPH_API void SetFromProperty(const FProperty* Property, bool bSelfContext, UClass* OwnerClass)
{
    DelegateReference.SetFromField<FProperty>(Property, bSelfContext, OwnerClass);
}
```

Parameters:
- `Property` — the `FMulticastDelegateProperty*` for the dispatcher
- `bSelfContext` — `true` when the dispatcher belongs to the same Blueprint you're editing (self-referential), `false` when from an external class. **For Blueprint-defined dispatchers, pass `true`.**
- `OwnerClass` — the class that owns the property. Use `SkeletonGeneratedClass` (preferred, see §4).

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Classes/K2Node_BaseMCDelegate.h` line 46

---

### 3. Confirmed Creation Sequence (from Engine Source)

**Pattern A — from `BlueprintDelegateNodeSpawner.cpp` (the spawner Epic uses for context menus):**

```cpp
// Engine's post-spawn lambda (BlueprintDelegateNodeSpawner.cpp line 105-117)
auto SetDelegateLambda = [](UEdGraphNode* NewNode, FFieldVariant InField)
{
    FMulticastDelegateProperty const* MCDProperty = CastField<FMulticastDelegateProperty>(InField.ToField());
    UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(NewNode);
    if ((DelegateNode != nullptr) && (MCDProperty != nullptr))
    {
        UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNodeChecked(NewNode);
        UClass* OwnerClass = MCDProperty->GetOwnerClass();
        DelegateNode->SetFromProperty(MCDProperty, false, OwnerClass);
    }
};
```

Note: The spawner passes `bSelfContext=false` here because it doesn't know the Blueprint at spawn time. In direct creation (Pattern B), where you have the Blueprint, `true` is appropriate for self-owned dispatchers.

**Pattern B — from `K2Node_BaseAsyncTask.cpp` (direct programmatic creation, most relevant):**

```cpp
// K2Node_BaseAsyncTask.cpp line 316-317 (intermediate compiler node)
UK2Node_AddDelegate* AddDelegateNode = CompilerContext.SpawnIntermediateNode<UK2Node_AddDelegate>(CurrentNode, SourceGraph);
AddDelegateNode->SetFromProperty(CurrentProperty, false, CurrentProperty->GetOwnerClass());
AddDelegateNode->AllocateDefaultPins();
```

**Pattern C — from `BPDelegateDragDropAction.h` (drag-drop from My Blueprint panel, cleanest template):**

```cpp
// BPDelegateDragDropAction.h line 66-80
template<class TNode> static void MakeMCDelegateNode(FNodeConstructionParams Params)
{
    check(Params.Graph && Params.Property);
    TNode* Node = NewObject<TNode>();
    FEdGraphSchemaAction_K2NewNode::SpawnNode<TNode>(
        Params.Graph,
        Params.GraphPosition,
        EK2NewNodeFlags::SelectNewNode,
        [&Params](TNode* NewInstance)
        {
            NewInstance->SetFromProperty(Params.Property, Params.bSelfContext, Params.Property->GetOwnerClass());
        }
    );
}
```

**The canonical direct-creation sequence (no spawner, matching Olive's `OliveNodeFactory` pattern):**

```cpp
// Step 1: find the FMulticastDelegateProperty (see §4)
FMulticastDelegateProperty* DelegateProp = FindFProperty<FMulticastDelegateProperty>(
    Blueprint->SkeletonGeneratedClass, FName(*DispatcherName));
UClass* OwnerClass = Blueprint->SkeletonGeneratedClass;

// Step 2: create and initialize
UK2Node_CallDelegate* Node = NewObject<UK2Node_CallDelegate>(Graph);
Node->SetFromProperty(DelegateProp, /*bSelfContext=*/true, OwnerClass);
Node->AllocateDefaultPins();
Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
```

The order is strict: `SetFromProperty` MUST be called before `AllocateDefaultPins`, because `AllocateDefaultPins` reads `DelegateReference` to resolve the signature function and build data pins.

Sources:
- `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/BlueprintDelegateNodeSpawner.cpp`
- `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_BaseAsyncTask.cpp` line 316
- `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/Kismet/Private/BPDelegateDragDropAction.h` line 66

---

### 4. How Event Dispatchers Are Stored on a Blueprint

Event dispatchers defined in the Blueprint editor are stored in **two places**:

**4a. `Blueprint->DelegateSignatureGraphs`**

Each dispatcher gets its own `UEdGraph` in this array. The graph name IS the dispatcher name. The graph contains a `UEdGraphNode_FunctionEntry` that defines the signature parameters.

```cpp
// BlueprintEditor.cpp line 9451 — how the editor adds a new dispatcher
Blueprint->DelegateSignatureGraphs.Add(NewGraph);
FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
```

Utility functions in `FBlueprintEditorUtils` (both `UNREALED_API`):
```cpp
// Get names of all dispatchers on a Blueprint
static void GetDelegateNameList(const UBlueprint* Blueprint, TSet<FName>& DelegatesNames);

// Get a specific dispatcher graph by name
static UEdGraph* GetDelegateSignatureGraphByName(UBlueprint* Blueprint, FName DelegateName);
```

**4b. `FMulticastDelegateProperty` on the skeleton/generated class**

After the Blueprint is compiled (or the skeleton is generated), each dispatcher becomes an `FMulticastDelegateProperty` on `SkeletonGeneratedClass`. The property name matches the dispatcher name. The compiler uses the suffix `__DelegateSignature` (from `HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX`, defined in `Engine/Source/Runtime/Core/Public/Delegates/Delegate.h` line 197) on the `UFunction` it generates for the signature, but the **property name is plain** (no suffix).

**How to find the `FMulticastDelegateProperty`:**

```cpp
// Preferred: use SkeletonGeneratedClass — reflects uncompiled changes
FMulticastDelegateProperty* Prop = FindFProperty<FMulticastDelegateProperty>(
    Blueprint->SkeletonGeneratedClass, FName(*DispatcherName));

// Fallback: GeneratedClass — only reflects last compiled state
if (!Prop && Blueprint->GeneratedClass)
{
    Prop = FindFProperty<FMulticastDelegateProperty>(
        Blueprint->GeneratedClass, FName(*DispatcherName));
}
```

Why `SkeletonGeneratedClass` first: The comment in `UK2Node_BaseMCDelegate::GetDelegateSignature()` (line 116-131 of `K2Node_MCDelegate.cpp`) explains it explicitly — "the generated class may not have the delegate yet (hasn't been compiled with it), or it could be out of date."

**Alternative via `NewVariables`:** The resolver in this project's `OliveBlueprintPlanResolver.cpp` uses a third approach for validation — checking `Blueprint->NewVariables` for entries with `PinCategory == UEdGraphSchema_K2::PC_MCDelegate`. This is correct for detecting dispatcher existence at plan resolution time (before the property exists), but you cannot use `NewVariables` to get the `FMulticastDelegateProperty` needed by `SetFromProperty`. The `FMulticastDelegateProperty` only exists on the skeleton/generated class.

Sources:
- `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_MCDelegate.cpp` line 116-138
- `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/Kismet/Private/BlueprintEditor.cpp` line 9441-9454
- `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/Core/Public/Delegates/Delegate.h` line 197

---

### 5. Pin Layout After `AllocateDefaultPins`

**`UK2Node_BaseMCDelegate::AllocateDefaultPins()` (all variants, base pins):**
```cpp
// K2Node_MCDelegate.cpp line 95-113
CreatePin(EGPD_Input,  PC_Exec,   PN_Execute);   // exec in:  "execute" / ""
CreatePin(EGPD_Output, PC_Exec,   PN_Then);      // exec out: "then"
CreatePin(EGPD_Input,  PC_Object, PN_Self);      // "Target" — typed to the owning class
```
The Target pin's `PinFriendlyName` is "Target". Its `PinSubCategoryObject` is set to `PropertyOwnerClass->GetAuthoritativeClass()`.

**`UK2Node_CallDelegate::AllocateDefaultPins()` — adds dispatcher parameter pins:**
```cpp
// K2Node_MCDelegate.cpp line 463-468
Super::AllocateDefaultPins(); // base exec + Target pins
CreatePinsForFunctionInputs(GetDelegateSignature()); // input-only params of the signature
```
So for a dispatcher `OnFired(float Damage)`, the `call_dispatcher` node will have:
- `execute` (exec in)
- `then` (exec out)
- `Target` (object in, typed to owning class)
- `Damage` (float in) — one pin per dispatcher parameter

**`UK2Node_AddDelegate::AllocateDefaultPins()` — adds the "Event" delegate input pin:**
```cpp
// K2Node_MCDelegate.cpp line 334-346
Super::AllocateDefaultPins(); // base exec + Target pins
// Then adds a REFERENCE to a single-cast delegate matching the signature:
UEdGraphNode::FCreatePinParams PinParams;
PinParams.bIsReference = true;
PinParams.bIsConst = true;
UEdGraphPin* DelegatePin = CreatePin(EGPD_Input, PC_Delegate, TEXT("Delegate"), PinParams);
FMemberReference::FillSimpleMemberReference<UFunction>(GetDelegateSignature(),
    DelegatePin->PinType.PinSubCategoryMemberReference);
DelegatePin->PinFriendlyName = NSLOCTEXT("K2Node", "PinFriendlyDelegatetName", "Event");
```
So `bind_dispatcher` (UK2Node_AddDelegate) pins are:
- `execute` (exec in)
- `then` (exec out)
- `Target` (object in, typed to owning class)
- `Delegate` (delegate in, friendly name "Event") — connected to a Custom Event node

**`GetDelegatePin()`** — utility method on base class to find the "Delegate" pin by name `FK2Node_BaseMCDelegateHelper::DelegatePinName` (= `"Delegate"`). Only meaningful on AddDelegate/RemoveDelegate — CallDelegate has no such pin.

Sources:
- `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_MCDelegate.cpp` line 95-503

---

### 6. Property Flags — What `call_dispatcher` vs `bind_dispatcher` Requires

From `BlueprintActionDatabase.cpp` line 738-755:
```cpp
FMulticastDelegateProperty* DelegateProperty = CastFieldChecked<FMulticastDelegateProperty>(Property);

// AddDelegate and RemoveDelegate require BlueprintAssignable
if (DelegateProperty->HasAnyPropertyFlags(CPF_BlueprintAssignable))
{
    // UK2Node_AddDelegate and UK2Node_RemoveDelegate are valid
}

// CallDelegate requires BlueprintCallable
if (DelegateProperty->HasAnyPropertyFlags(CPF_BlueprintCallable))
{
    // UK2Node_CallDelegate is valid
}
```

Blueprint-defined dispatchers are automatically `BlueprintAssignable | BlueprintCallable`, so both operations are always valid for Blueprint-owned dispatchers. The distinction matters for dispatchers inherited from C++ parent classes.

The validator in `UK2Node_CallDelegate::ValidateNodeDuringCompilation` also asserts `CPF_BlueprintCallable`:
```cpp
// K2Node_MCDelegate.cpp line 482-492
if (!Property->HasAllPropertyFlags(CPF_BlueprintCallable))
{
    MessageLog.Error(...); // "Event Dispatcher is not 'BlueprintCallable'"
}
```

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/BlueprintActionDatabase.cpp` line 738-758

---

### 7. What's Already Implemented in This Project

**`call_delegate` is fully implemented.** The codebase already has:

- `OlivePlanOps::CallDelegate = TEXT("call_delegate")` in `BlueprintPlanIR.h` line 84
- `OliveNodeTypes::CallDelegate = TEXT("CallDelegate")` in `OliveNodeFactory.h` line 90
- `FOliveNodeFactory::CreateCallDelegateNode()` in `OliveNodeFactory.cpp` line 938-1031 — searches `SkeletonGeneratedClass` then `GeneratedClass` via `TFieldIterator<FMulticastDelegateProperty>`, calls `SetFromProperty(DelegateProp, true, OwnerClass)` + `AllocateDefaultPins()`
- `FOliveBlueprintPlanResolver::ResolveCallDelegateOp()` in `OliveBlueprintPlanResolver.cpp` line 1775-1856 — validates via `NewVariables` (PinCategory == PC_MCDelegate), emits friendly error with available dispatcher list

**`bind_dispatcher` (`UK2Node_AddDelegate`) is NOT implemented** — no op, no node type, no factory method.

---

### 8. How `bind_dispatcher` Connects to a Custom Event

The typical usage pattern for `UK2Node_AddDelegate` in a graph:

```
[Custom Event: OnFiredHandler] --exec--> [Bind Event to OnFired]
                                             ^
                                             | "Event" pin (delegate ref)
                              [Custom Event node's delegate output]
```

In Blueprint IR terms, this requires two plan steps:
1. `custom_event` — creates a custom event with the same signature as the dispatcher
2. `bind_dispatcher` — creates `UK2Node_AddDelegate`, with the "Event" (Delegate) pin wired to the custom event's output

The Delegate pin (`GetDelegatePin()`) on `UK2Node_AddDelegate` expects a `PC_Delegate` type that matches the dispatcher signature. The custom event's implicit "delegate" output pin carries this type.

From `K2Node_BaseAsyncTask.cpp` line 325 — Epic wires it via:
```cpp
FBaseAsyncTaskHelper::CreateDelegateForNewFunction(
    AddDelegateNode->GetDelegatePin(),
    CurrentCENode->GetFunctionName(),
    CurrentNode, SourceGraph, CompilerContext);
```

For the plan executor (non-compiler context), the simpler approach is `TryCreateConnection` from the custom event's `FindPin(TEXT("OutputDelegate"))` (or whichever pin it uses) to `AddDelegateNode->GetDelegatePin()`.

---

## Recommendations

1. **`call_delegate` is fully implemented and correct.** No changes needed. The existing `OliveNodeFactory::CreateCallDelegateNode` uses the right sequence: `SkeletonGeneratedClass`-first lookup → `SetFromProperty(prop, true, ownerClass)` → `AllocateDefaultPins()` → `Graph->AddNode()`.

2. **To implement `bind_dispatcher`**, follow the exact same pattern as `call_dispatcher` but use `UK2Node_AddDelegate`. Required additions:
   - Add `OlivePlanOps::BindDelegate = TEXT("bind_dispatcher")` in `BlueprintPlanIR.h`
   - Add `OliveNodeTypes::BindDelegate = TEXT("BindDelegate")` in `OliveNodeFactory.h`
   - Add `FOliveNodeFactory::CreateBindDelegateNode()` — identical to `CreateCallDelegateNode` except `NewObject<UK2Node_AddDelegate>` instead of `UK2Node_CallDelegate`
   - Add `FOliveBlueprintPlanResolver::ResolveBindDelegateOp()` — same validation as `ResolveCallDelegateOp`, same `Properties.Add("delegate_name", ...)` output. Set `bIsPure = false`.
   - Register in `InitializeNodeCreators()` and `RequiredPropertiesMap`

3. **Required `#include` additions** for `bind_dispatcher`:
   - `#include "K2Node_AddDelegate.h"` — not yet included in `OliveNodeFactory.cpp` (only `K2Node_CallDelegate.h` is included)

4. **The `bind_dispatcher` "Event" pin wiring challenge**: Wiring the `Delegate` pin of `UK2Node_AddDelegate` to a `custom_event` requires the custom event to expose its delegate reference pin. Check whether `UK2Node_CustomEvent` exposes a `PC_Delegate` output pin after `AllocateDefaultPins`. If it does not (custom events are typically entry nodes with exec-out only), then `bind_dispatcher` needs to use `UK2Node_CreateDelegate` as an intermediate node instead of direct wiring. This warrants a separate investigation before implementing.

5. **`bSelfContext` correctness**: Always pass `true` for dispatchers defined on the Blueprint being edited. Pass `false` only for dispatchers inherited from external C++ classes. The existing `CreateCallDelegateNode` already does this correctly.

6. **Compile requirement for `call_delegate`**: If the Blueprint has not yet been compiled after the dispatcher was added, `SkeletonGeneratedClass` may not yet have the `FMulticastDelegateProperty`. The existing code handles this via the `TFieldIterator` fallback chain, but a plan that adds a dispatcher AND immediately calls it in the same `apply_plan_json` may fail on the node creation step. Consider pre-compiling (or at minimum calling `FKismetEditorUtilities::CompileBlueprint`) if the dispatcher was just added in the same plan.

7. **`NewVariables` vs property lookup**: The resolver correctly uses `NewVariables` (with `PinCategory == PC_MCDelegate`) to detect dispatcher existence at plan-resolve time (before node creation). The factory correctly uses `TFieldIterator<FMulticastDelegateProperty>` on the skeleton class at node-creation time. Do not mix these — `NewVariables` cannot give you the `FMulticastDelegateProperty` pointer.
