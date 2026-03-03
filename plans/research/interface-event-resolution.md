# Research: Interface Event Resolution Gap in Plan JSON

## Question
When the AI uses `op: "event", target: "Interact"` in plan_json on a Blueprint that implements a Blueprint Interface with a no-output `Interact` function (an "implementable event"), the resolver passes through to `CreateEventNode` which fails because `Interact` is not found via `FindFunctionByName` on the parent class, nor as an SCS component delegate, nor as an Enhanced Input Action.

## Findings

### 1. Current ResolveEventOp Search Order

`FOliveBlueprintPlanResolver::ResolveEventOp()` at line 1887 of `OliveBlueprintPlanResolver.cpp` searches in this exact order:

1. **Empty target guard** -- returns error if `Step.Target` is empty
2. **Enhanced Input Action** (line 1973) -- if target starts with `IA_`, resolves to `OliveNodeTypes::EnhancedInputAction`
3. **Native event name map** (line 1918) -- static map of ~30 entries (`BeginPlay` -> `ReceiveBeginPlay`, etc.)
4. **SCS component delegate scan** (line 2016) -- for non-native events, scans all SCS nodes for matching `FMulticastDelegateProperty`
5. **Pass-through** (line 2131) -- if none of the above match, sets `event_name` property and falls through to `CreateEventNode` in the executor

**The gap**: There is NO check for interface functions anywhere in ResolveEventOp. The function name "Interact" is not in the native event map, has no `IA_` prefix, and is not an SCS delegate. It falls through to the pass-through path at line 2131.

### 2. CreateEventNode Search Order

`FOliveNodeFactory::CreateEventNode()` at line 376 of `OliveNodeFactory.cpp` then searches:

1. **Parent class** (line 401) -- `Blueprint->ParentClass->FindFunctionByName(EventName)` -- FAILS because "Interact" is defined on the interface, not the parent class
2. **SCS component delegates** (line 411) -- same scan as resolver -- no match
3. **Enhanced Input Action** (line 488) -- no match
4. **Returns nullptr** (line 531) with error: "Event '%s' not found in parent class, as a component delegate, or as an Enhanced Input Action"

**Neither the resolver nor the factory ever check `Blueprint->ImplementedInterfaces`.**

### 3. How UE Stores Interface Events vs Interface Functions

The critical distinction is in `UEdGraphSchema_K2::FunctionCanBePlacedAsEvent()` at line 939 of `EdGraphSchema_K2.cpp`:

```cpp
bool UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(const UFunction* InFunction)
{
    // Must be override-able, non-static, non-const, not thread-safe
    if (!InFunction || !CanKismetOverrideFunction(InFunction)
        || InFunction->HasAnyFunctionFlags(FUNC_Static|FUNC_Const)
        || FBlueprintEditorUtils::HasFunctionBlueprintThreadSafeMetaData(InFunction))
        return false;

    // ForceAsFunction metadata overrides
    if (InFunction->HasAllFunctionFlags(FUNC_BlueprintEvent)
        && InFunction->HasMetaData(FBlueprintMetadata::MD_ForceAsFunction))
        return false;

    // The key test: no output parameters = can be an event
    return !HasFunctionAnyOutputParameter(InFunction);
}
```

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp:939`

**The rule**: Interface functions with NO output parameters (`FunctionCanBePlacedAsEvent` returns true) are implemented as `UK2Node_Event` nodes in the EventGraph. Interface functions WITH output parameters become function graphs stored in `ImplementedInterfaces[i].Graphs`.

`ConformInterfaceByName()` (line 7268 of `BlueprintEditorUtils.cpp`) confirms this:

```cpp
// Line 7371 -- only creates interface function graphs for NON-event functions:
if( UEdGraphSchema_K2::CanKismetOverrideFunction(Function)
    && !UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function) )
{
    // Creates graph in CurrentInterfaceDesc.Graphs
    // ... NewGraph->bAllowDeletion = false;
    // ... NewGraph->InterfaceGuid = ...
    // ... FBlueprintEditorUtils::AddInterfaceGraph(...)
}
```

This means: for a no-output interface function like `Interact`, `ConformImplementedInterfaces` does NOT create any graph in `ImplementedInterfaces[i].Graphs`. The function exists only as a `UFunction` on the interface class. The user manually adds a `UK2Node_Event` in the EventGraph to implement it.

### 4. How UE Creates Interface Event Nodes

`UBlueprintEventNodeSpawner` (line 216 of `BlueprintEventNodeSpawner.cpp`) creates the node:

```cpp
K2EventNode->EventReference.SetFromField<UFunction>(InEventFunc, false);
K2EventNode->bOverrideFunction = true;
```

The `false` second parameter means "not self context" -- the EventReference points to the interface class, not to `Self`. The `bOverrideFunction = true` flag marks it as an override.

`UK2Node_Event::IsInterfaceEventNode()` (line 509 of `K2Node_Event.cpp`) checks:

```cpp
bool UK2Node_Event::IsInterfaceEventNode() const
{
    return (EventReference.GetMemberParentClass(GetBlueprintClassFromNode()) != nullptr)
        && EventReference.GetMemberParentClass(GetBlueprintClassFromNode())->IsChildOf(UInterface::StaticClass());
}
```

So the key properties of an interface event node:
- It is a `UK2Node_Event` (NOT `UK2Node_FunctionEntry`)
- It lives in the EventGraph (NOT in `ImplementedInterfaces[i].Graphs`)
- `EventReference.SetFromField<UFunction>(InterfaceFunc, false)` -- references the interface function
- `bOverrideFunction = true`

### 5. How to Detect Interface Events

Given a Blueprint and a function name like "Interact", the detection logic should be:

```cpp
// Step 1: Get all interface classes (both directly implemented and inherited)
TArray<UClass*> InterfaceClasses;
FBlueprintEditorUtils::FindImplementedInterfaces(Blueprint, /*bGetAllInterfaces=*/true, InterfaceClasses);

// Step 2: Search each interface for the function
for (UClass* InterfaceClass : InterfaceClasses)
{
    // For BPI, redirect to skeleton class
    const UClass* SearchClass = InterfaceClass;
    if (InterfaceClass->ClassGeneratedBy)
    {
        if (UBlueprint* InterfaceBP = Cast<UBlueprint>(InterfaceClass->ClassGeneratedBy))
        {
            SearchClass = InterfaceBP->SkeletonGeneratedClass;
        }
    }

    UFunction* Func = SearchClass->FindFunctionByName(FName(*EventName));
    if (Func && UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Func))
    {
        // This is an interface event!
        // InterfaceClass is the owning interface class.
        // Func is the UFunction to reference.
        break;
    }
}
```

Source: Pattern derived from `ConformInterfaceByName()` in `BlueprintEditorUtils.cpp:7357-7371` and `IsFunctionAvailableAsEvent()` in `BlueprintEditorCommands.cpp:120-156`.

**Important**: For Blueprint Interfaces (not native C++ interfaces), the function is on the `SkeletonGeneratedClass`, not the `Interface` class directly. See line 7358-7362 of `BlueprintEditorUtils.cpp`:

```cpp
const UClass* InterfaceClass = CurrentInterfaceDesc.Interface;
if (InterfaceClass && InterfaceClass->ClassGeneratedBy)
{
    InterfaceClass = CastChecked<UBlueprint>(InterfaceClass->ClassGeneratedBy)->SkeletonGeneratedClass;
}
```

### 6. Existing Interface Handling in the Resolver

The resolver has significant interface support for **`call` ops** via `FindFunction` (which has `InterfaceSearch` match method), but NO interface support for **`event` ops**.

Relevant existing code:
- `ResolveCallOp` (line ~1343): When `FindFunction` returns `InterfaceSearch` match method, marks the resolved step with `bIsInterfaceCall = true` for `UK2Node_Message` creation
- `FOliveGraphContext::BuildFromBlueprint()` (line 165): Searches `ImplementedInterfaces[i].Graphs` for graph context (for functions WITH outputs that have their own graph)
- Plan executor (line 3133): Detects interface implementation graphs for pre-compile validation warnings

None of this helps with the `event` op path.

### 7. Existing ConformImplementedInterfaces Guard

In `FindOrCreateFunctionGraph()` (line 7102 of `OliveBlueprintToolHandlers.cpp`), there is an existing guard:

```cpp
// If this graph name matches an interface function, force conformance and retry
if (bIsInterfaceFunction)
{
    FBlueprintEditorUtils::ConformImplementedInterfaces(Blueprint);
    UEdGraph* ConformedGraph = FindGraphByName(Blueprint, GraphName);
    if (ConformedGraph) return ConformedGraph;
}
```

This guard handles the case where `graph_target` is set to an interface function name (a function WITH outputs). It forces conformance to materialize the graph. This is completely unrelated to interface events (no outputs), which live in EventGraph, not their own graph.

### 8. What Graph Should the Plan Executor Target?

**Interface events belong in EventGraph.** When the AI writes:
```json
{
  "graph_target": "EventGraph",
  "steps": [
    {"step_id": "evt", "op": "event", "target": "Interact"},
    {"step_id": "s1", "op": "call", "target": "PrintString", "exec_after": "evt"}
  ]
}
```

The `graph_target: "EventGraph"` is correct. The `UK2Node_Event` for the interface event should be created in EventGraph, just like native events (`BeginPlay`, `Tick`). The resolver should NOT redirect to any interface graph.

### 9. Duplicate Event Prevention

`FBlueprintEditorUtils::FindOverrideForFunction()` (line 2919 of `BlueprintEditorUtils.cpp`) can find existing interface event nodes:

```cpp
UK2Node_Event* FBlueprintEditorUtils::FindOverrideForFunction(
    const UBlueprint* Blueprint, const UClass* SignatureClass, FName SignatureName)
{
    TArray<UK2Node_Event*> AllEvents;
    FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(Blueprint, AllEvents);
    for (UK2Node_Event* EventNode : AllEvents)
    {
        if (EventNode->bOverrideFunction == true
            && EventNode->EventReference.GetMemberName() == SignatureName)
        {
            const UClass* MemberParentClass = EventNode->EventReference.GetMemberParentClass(...);
            if (MemberParentClass && MemberParentClass->IsChildOf(SignatureClass))
                return EventNode;
        }
    }
    return nullptr;
}
```

To check for an existing interface event, pass the interface class as `SignatureClass` (not `ParentClass`).

### 10. Required Node Creation

The correct way to create an interface event node:

```cpp
UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
EventNode->EventReference.SetFromField<UFunction>(InterfaceFunction, /*bIsSelfContext=*/false);
EventNode->bOverrideFunction = true;
EventNode->AllocateDefaultPins();
Graph->AddNode(EventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
```

Note: `SetFromField<UFunction>` (from `BlueprintEventNodeSpawner`) vs `SetExternalMember` (from current `CreateEventNode`). Both work, but `SetFromField` is the canonical path used by the engine. The difference:
- `SetFromField<UFunction>(Func, false)` -- resolves the member name, parent class, and GUID from the UFunction automatically
- `SetExternalMember(FName, UClass*)` -- manually specifies name and class

Either works, but `SetFromField` is safer because it also sets the `MemberGuid` for Blueprint Interfaces (important for rename-resilience).

## Recommendations

### Fix Location: Two places need changes

**1. ResolveEventOp (resolver)** -- Add interface event detection between the SCS delegate scan and the pass-through. This is the primary fix.

Insert after line 2128 (after the SCS delegate `if (!bIsNativeEvent && BP && BP->SimpleConstructionScript)` block) and before line 2130 (the pass-through):

```
// Check: Interface event (no-output interface function implemented as UK2Node_Event in EventGraph)
Search ImplementedInterfaces for matching function name.
If found AND FunctionCanBePlacedAsEvent returns true:
  - Set a new property like "interface_class" or "interface_event_class" on the resolved step
  - Set event_name to the UFunction's actual name
  - Keep NodeType as OliveNodeTypes::Event (it IS a UK2Node_Event)
  - Add a resolver note explaining the resolution
```

**2. CreateEventNode (node factory)** -- Add an interface function search fallback after the parent class check (line 401) fails and before the SCS delegate scan. When the resolver has already tagged this as an interface event, use that info directly. Otherwise, do the search.

The key addition:
```cpp
// After line 401 (FindFunctionByName on ParentClass fails):
if (!EventFunction)
{
    // Check implemented interfaces
    TArray<UClass*> InterfaceClasses;
    FBlueprintEditorUtils::FindImplementedInterfaces(Blueprint, true, InterfaceClasses);
    for (UClass* IClass : InterfaceClasses)
    {
        const UClass* SearchClass = IClass;
        if (IClass->ClassGeneratedBy)
        {
            if (UBlueprint* IBP = Cast<UBlueprint>(IClass->ClassGeneratedBy))
                SearchClass = IBP->SkeletonGeneratedClass;
        }
        UFunction* Func = SearchClass->FindFunctionByName(EventName);
        if (Func && UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Func))
        {
            // Found interface event -- create with interface class reference
            // Check for existing first
            UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(
                Blueprint, IClass, EventName);
            if (Existing) { /* return existing or error */ }

            UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
            EventNode->EventReference.SetFromField<UFunction>(Func, false);
            EventNode->bOverrideFunction = true;
            EventNode->AllocateDefaultPins();
            Graph->AddNode(EventNode, false, false);
            return EventNode;
        }
    }
}
```

### Architectural Notes

- **No new op type needed.** Interface events are `UK2Node_Event` in the EventGraph, same as native events. The existing `event` op and `OliveNodeTypes::Event` are correct.
- **No graph_target redirect needed.** The AI's `graph_target: "EventGraph"` is correct for interface events.
- **Resolver tagging is optional but recommended.** The resolver can detect interface events and add a property (e.g., `"interface_class_path": "/Game/BPI_Interactable.BPI_Interactable_C"`) so the factory can skip redundant searching. But even without resolver changes, fixing CreateEventNode alone would fix the issue.
- **BPI skeleton class access**: For Blueprint Interfaces, always use `SkeletonGeneratedClass` from the BPI's `ClassGeneratedBy` Blueprint. The `Interface` field on `FBPInterfaceDescription` may point to the generated class which works for `FindFunctionByName`, but `SkeletonGeneratedClass` is what `ConformInterfaceByName` uses internally.
- **Duplicate detection**: Use `FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, InterfaceClass, EventName)` to check if the event already exists. Pass the interface class, NOT the parent class.
- **`SetFromField` vs `SetExternalMember`**: Use `SetFromField<UFunction>(Func, false)` for interface events. This correctly sets the `MemberGuid` which Blueprint Interfaces use for rename tracking. `SetExternalMember` only sets name and class.
- **Include needed**: `#include "EdGraphSchema_K2.h"` for `FunctionCanBePlacedAsEvent()`, already included by both files.
- **Header for FindImplementedInterfaces**: `#include "Kismet2/BlueprintEditorUtils.h"`, already included by both files.

### Risk Assessment

- **Low risk.** This adds a new search step in the existing event resolution path. No existing behavior changes -- the interface search only triggers when the parent class search and SCS delegate search both fail.
- **Edge case: Interface function WITH outputs.** If someone writes `op: "event", target: "SomeInterfaceFunction"` where the function has outputs, `FunctionCanBePlacedAsEvent` returns false and the search skips it. The AI should use `graph_target: "SomeInterfaceFunction"` (targeting the interface function's own graph) with `op: "event", target: "entry"` instead, which already works via the function graph path in the resolver.
- **Edge case: Parent class AND interface both define same function name.** The parent class check runs first (already the case), so parent class wins. This matches UE's own override priority.
