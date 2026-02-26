# Bug D Research: Event Dispatchers Missing Signature Function

## The Problem

```
Warning: No SignatureFunction in MulticastDelegateProperty 'OnBulletHit'
Warning: No SignatureFunction in MulticastDelegateProperty 'OnBulletDestroyed'
```

Even `OnBulletDestroyed` (zero params) triggers the warning. This is not a param-parsing issue — the dispatcher creation itself is fundamentally incomplete.

## Root Cause

Current `AddEventDispatcher` (line ~48514 of bundled_code.txt) does this:

```cpp
FEdGraphPinType PinType;
PinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;

bool bSuccess = FBlueprintEditorUtils::AddMemberVariable(
    Blueprint, FName(*DispatcherName), PinType);
```

**That's it.** It creates a `PC_MCDelegate` variable in `Blueprint->NewVariables` — a multicast delegate property — but never creates the **DelegateSignatureGraph** that UE requires.

### What UE Needs for an Event Dispatcher

In UE, a Blueprint event dispatcher consists of **two** things:

1. **A `PC_MCDelegate` variable** in `Blueprint->NewVariables` — the delegate property itself
2. **A `DelegateSignatureGraph`** in `Blueprint->DelegateSignatureGraphs` — a UEdGraph with a `UK2Node_FunctionEntry` that defines the delegate's parameter signature

When you click "Add Event Dispatcher" in the UE Blueprint Editor, the editor creates both. The signature graph is what the compiler reads to build the `UFunction` that becomes the `SignatureFunction` on the `FMulticastDelegateProperty`.

Our code only does step 1. The compiler finds the delegate property but has no graph to compile a signature function from, so it warns.

### Evidence from Existing Code

The reader code (line ~42072) confirms the architecture — dispatchers are `PC_MCDelegate` entries in `NewVariables`:
```cpp
if (VarDesc.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate) { ... }
```

The graph finder (line ~31471) searches `DelegateSignatureGraphs`:
```cpp
if (UEdGraph* Found = SearchArray(Blueprint->DelegateSignatureGraphs)) return Found;
```

The graph type detector (line ~43086) classifies them:
```cpp
if (OwningBlueprint->DelegateSignatureGraphs.Contains(const_cast<UEdGraph*>(Graph)))
    return TEXT("DelegateSignature");
```

But `AddEventDispatcher` never adds anything to `DelegateSignatureGraphs`.

## The Fix

### Architecture: Mirror the AddFunction Pattern

`AddFunction` (line ~48180) does:
1. `FBlueprintEditorUtils::CreateNewGraph(Blueprint, Name, UEdGraph, UEdGraphSchema_K2)`
2. `FBlueprintEditorUtils::AddFunctionGraph(Blueprint, NewGraph, true, nullptr)`
3. Find `UK2Node_FunctionEntry` in the new graph
4. Add `UserDefinedPins` for each parameter
5. `EntryNode->ReconstructNode()`

For event dispatchers, the equivalent is:
1. `FBlueprintEditorUtils::CreateNewGraph(Blueprint, Name, UEdGraph, UEdGraphSchema_K2)`
2. Add the graph to `Blueprint->DelegateSignatureGraphs` directly (there is no `AddDelegateGraph` helper in `FBlueprintEditorUtils`)
3. Find `UK2Node_FunctionEntry` in the new graph
4. Add `UserDefinedPins` for each parameter (same pattern as functions)
5. `EntryNode->ReconstructNode()`
6. Also create the `PC_MCDelegate` variable in `NewVariables` (or let MarkBlueprintAsStructurallyModified handle it)

### Key Question: Do We Need Both the Variable AND the Graph?

**Yes.** The variable in `NewVariables` is how Blueprint nodes reference the dispatcher (Call, Bind, Unbind nodes). The graph in `DelegateSignatureGraphs` is what the compiler uses to build the signature function. Both are required.

When UE compiles a Blueprint with a delegate signature graph, it generates a `UFunction` named `{DispatcherName}__DelegateSignature` on the generated class. This is the `SignatureFunction` that the compiler looks for on the `FMulticastDelegateProperty`.

### Implementation

Replace the current `AddEventDispatcher` with:

```cpp
FOliveBlueprintWriteResult FOliveBlueprintWriter::AddEventDispatcher(
    const FString& AssetPath,
    const FString& DispatcherName,
    const TArray<FOliveIRFunctionParam>& Params)
{
    if (IsPIEActive())
    {
        return FOliveBlueprintWriteResult::Error(TEXT("Cannot modify Blueprints while Play-In-Editor is active"));
    }

    FString Error;
    UBlueprint* Blueprint = LoadBlueprintForEditing(AssetPath, Error);
    if (!Blueprint)
    {
        return FOliveBlueprintWriteResult::Error(Error);
    }

    // Check for existing dispatcher with same name
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName.ToString() == DispatcherName &&
            Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
        {
            return FOliveBlueprintWriteResult::Error(
                FString::Printf(TEXT("Event dispatcher '%s' already exists"), *DispatcherName));
        }
    }

    OLIVE_SCOPED_TRANSACTION(FText::Format(
        NSLOCTEXT("OliveBPWriter", "AddEventDispatcher", "Add Event Dispatcher '{0}' to '{1}'"),
        FText::FromString(DispatcherName),
        FText::FromString(Blueprint->GetName())));

    Blueprint->Modify();

    // ====================================================================
    // Step 1: Create the delegate signature graph
    // This is what was MISSING — without it, UE has no source for the
    // SignatureFunction and the compiler warns.
    // ====================================================================
    UEdGraph* SigGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        FName(*DispatcherName),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass()
    );

    if (!SigGraph)
    {
        return FOliveBlueprintWriteResult::Error(
            FString::Printf(TEXT("Failed to create delegate signature graph for '%s'"), *DispatcherName));
    }

    // Add to the Blueprint's delegate signature graphs list
    // (No AddDelegateGraph helper exists — direct array add is what the editor does)
    Blueprint->DelegateSignatureGraphs.Add(SigGraph);
    SigGraph->bAllowDeletion = false;
    SigGraph->bAllowRenaming = true;

    // ====================================================================
    // Step 2: Set up the entry node with parameters
    // The entry node's UserDefinedPins define the delegate signature,
    // exactly like function parameters on UK2Node_FunctionEntry.
    // ====================================================================
    UK2Node_FunctionEntry* EntryNode = nullptr;
    for (UEdGraphNode* Node : SigGraph->Nodes)
    {
        EntryNode = Cast<UK2Node_FunctionEntry>(Node);
        if (EntryNode)
        {
            break;
        }
    }

    // If CreateNewGraph didn't auto-create an entry node, create one manually
    if (!EntryNode)
    {
        FGraphNodeCreator<UK2Node_FunctionEntry> EntryCreator(*SigGraph);
        EntryNode = EntryCreator.CreateNode();
        EntryNode->NodePosX = 0;
        EntryNode->NodePosY = 0;
        EntryCreator.Finalize();
    }

    if (EntryNode)
    {
        // Add parameters to the delegate signature
        for (const FOliveIRFunctionParam& Param : Params)
        {
            FEdGraphPinType PinType = CreatePinTypeFromParam(Param);

            TSharedPtr<FUserPinInfo> NewPinInfo = MakeShareable(new FUserPinInfo());
            NewPinInfo->PinName = FName(*Param.Name);
            NewPinInfo->PinType = PinType;
            NewPinInfo->DesiredPinDirection = EGPD_Output; // Entry outputs = delegate params

            EntryNode->UserDefinedPins.Add(NewPinInfo);
        }

        EntryNode->ReconstructNode();
    }

    // ====================================================================
    // Step 3: Create the PC_MCDelegate variable
    // This links the delegate property to the signature graph by name.
    // The compiler matches them via the name convention:
    //   Variable name: "OnBulletHit"
    //   Graph name: "OnBulletHit"
    //   Generated signature function: "OnBulletHit__DelegateSignature"
    // ====================================================================
    FEdGraphPinType DelegatePinType;
    DelegatePinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    // PinSubCategoryObject could optionally reference the signature,
    // but the compiler resolves it by name matching the graph.

    FBlueprintEditorUtils::AddMemberVariable(
        Blueprint, FName(*DispatcherName), DelegatePinType);

    // ====================================================================
    // Step 4: Structural modification triggers recompile
    // ====================================================================
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    UE_LOG(LogOliveBPWriter, Log,
        TEXT("Added event dispatcher '%s' to '%s' with %d params"),
        *DispatcherName, *AssetPath, Params.Num());

    return FOliveBlueprintWriteResult::Success(AssetPath, DispatcherName);
}
```

## Key Design Decisions

### Why CreateNewGraph + DelegateSignatureGraphs.Add()?

There is no `FBlueprintEditorUtils::AddDelegateGraph()` helper. `AddFunctionGraph()` exists for functions, `AddMacroGraph()` for macros, but delegates are simpler — they're just graphs with an entry node, no return node, no function flags. The UE editor adds them directly to the array.

### Why Both the Graph AND the Variable?

- The **variable** (`PC_MCDelegate` in `NewVariables`) is what Blueprint nodes reference — "Call OnBulletHit", "Bind Event to OnBulletHit", etc.
- The **graph** (in `DelegateSignatureGraphs`) is what the compiler uses to generate the `{Name}__DelegateSignature` UFunction
- Name matching links them: variable named "OnBulletHit" → graph named "OnBulletHit" → compiled function "OnBulletHit__DelegateSignature"

### Why This Fixes the Zero-Param Case Too

`OnBulletDestroyed` has no params but still warns. That's because the warning isn't about param types — it's about the missing SignatureFunction entirely. Even a zero-param dispatcher needs a signature graph so the compiler can generate the (empty) signature function. With this fix, a zero-param dispatcher gets a graph with an entry node and no `UserDefinedPins`, which compiles to an empty-signature `UFunction`.

### Param Type Resolution

Parameters use the same `CreatePinTypeFromParam` → `ConvertIRType` pipeline as function parameters. This means the Phase 0A template type fix (making `ParseTemplateFuncParam` read `class_name`/`struct_name`/`enum_name`) also benefits dispatcher params.

## Risk Assessment

**Medium risk** — the core pattern mirrors how `AddFunction` works (which is proven), but there are unknowns:

1. **Does `CreateNewGraph` auto-create an entry node for delegate graphs?** It does for function graphs (via `AddFunctionGraph` which calls `CreateDefaultNodesForGraph`). Since we're not calling `AddFunctionGraph`, we may need to create the entry node manually. The code above handles both cases.

2. **Does the `bAllowDeletion` flag matter?** In UE, delegate signature graphs show as locked in the editor. Setting `bAllowDeletion = false` mirrors that behavior. Not critical for functionality.

3. **Does the variable need a `PinSubCategoryObject` reference?** For native delegates, the `PinSubCategoryObject` on the `FMulticastDelegateProperty` points to the signature function. For blueprint delegates, the compiler resolves by name. We're safe without it.

4. **Order: graph first or variable first?** The safest order is graph first, then variable, then `MarkBlueprintAsStructurallyModified`. This ensures when the compiler processes the structural modification, both pieces exist.

## Verification

After this fix:
- `OnBulletHit` and `OnBulletDestroyed` should both have entries in `Blueprint->DelegateSignatureGraphs`
- After compile, the generated class should have `OnBulletHit__DelegateSignature` and `OnBulletDestroyed__DelegateSignature` UFunctions
- The "No SignatureFunction" warnings should disappear
- Call/Bind/Unbind nodes for the dispatchers should work correctly

## Files Changed

- **OliveBlueprintWriter.cpp**: Replace `AddEventDispatcher` implementation (~40 lines → ~70 lines)
- No other files need changes — the reader already handles `PC_MCDelegate` variables, and the graph finder already searches `DelegateSignatureGraphs`
