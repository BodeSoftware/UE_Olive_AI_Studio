# Junior Engineer Tasks — Error Message Improvements

These two tasks come from `plans/error-messages-08g-design.md`. Read that file for full context.
Both are well-specified with minimal design decisions required.

---

## Task A — GetActorTransform Alias Fix (5 min, zero risk)

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`

Delete two lines from the alias map:

**Line ~2851** — remove this line entirely:
```cpp
Map.Add(TEXT("GetTransform"), TEXT("GetActorTransform"));
```
Reason: `GetTransform` is the actual C++ function name on AActor. Aliasing it away breaks it.

**Line ~3087** — remove this line entirely:
```cpp
Map.Add(TEXT("GetActorTransform"), TEXT("GetActorTransform"));
```
Reason: Self-referential no-op that overwrites the correct alias at line ~2852.

**Line ~2852** — keep this line, it is correct:
```cpp
Map.Add(TEXT("GetActorTransform"), TEXT("GetTransform"));
```

**Verification:** After removing those two lines, `GetActorTransform` should alias to `GetTransform` (the correct C++ name). Use grep to confirm no other `GetActorTransform` entries exist in the map that could interfere.

---

## Task B — Pin Listing in connect_pins Errors (30–40 min)

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp`

When a pin is not found, the error currently says `"Pin 'X' not found on node 'Y'"` with no guidance. Add a compact list of available pins so the AI knows what to use instead.

### Step 1: Add helper function

Add this helper in the **anonymous namespace** at the top of `OliveGraphWriter.cpp` (where other local helpers live):

```cpp
/**
 * Build a compact one-line listing of available pins on a node for a given direction.
 * Format: "PinName (Type), PinName (Type), ..."
 * Skips hidden pins. Caps at 15 entries.
 */
static FString BuildAvailablePinsList(const UEdGraphNode* Node, EEdGraphPinDirection DirectionFilter)
{
    if (!Node) return FString();

    TArray<FString> PinEntries;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->bHidden) continue;
        if (Pin->Direction != DirectionFilter) continue;

        FString TypeStr;
        const FName& Cat = Pin->PinType.PinCategory;
        if      (Cat == UEdGraphSchema_K2::PC_Exec)    TypeStr = TEXT("Exec");
        else if (Cat == UEdGraphSchema_K2::PC_Boolean)  TypeStr = TEXT("Bool");
        else if (Cat == UEdGraphSchema_K2::PC_Int)      TypeStr = TEXT("Int");
        else if (Cat == UEdGraphSchema_K2::PC_Int64)    TypeStr = TEXT("Int64");
        else if (Cat == UEdGraphSchema_K2::PC_Real)     TypeStr = TEXT("Float");
        else if (Cat == UEdGraphSchema_K2::PC_String)   TypeStr = TEXT("String");
        else if (Cat == UEdGraphSchema_K2::PC_Name)     TypeStr = TEXT("Name");
        else if (Cat == UEdGraphSchema_K2::PC_Text)     TypeStr = TEXT("Text");
        else if (Cat == UEdGraphSchema_K2::PC_Struct)
        {
            UScriptStruct* S = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
            TypeStr = S ? S->GetName() : TEXT("Struct");
        }
        else if (Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_Interface ||
                 Cat == UEdGraphSchema_K2::PC_SoftObject || Cat == UEdGraphSchema_K2::PC_Class ||
                 Cat == UEdGraphSchema_K2::PC_SoftClass)
        {
            UClass* C = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
            TypeStr = C ? C->GetName() : TEXT("Object");
        }
        else if (Cat == UEdGraphSchema_K2::PC_Byte || Cat == UEdGraphSchema_K2::PC_Enum)
        {
            UEnum* E = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
            TypeStr = E ? E->GetName() : TEXT("Byte");
        }
        else if (Cat == UEdGraphSchema_K2::PC_Delegate || Cat == UEdGraphSchema_K2::PC_MCDelegate)
        {
            TypeStr = TEXT("Delegate");
        }
        else if (Cat == UEdGraphSchema_K2::PC_Wildcard)
        {
            TypeStr = TEXT("Wildcard");
        }
        else
        {
            TypeStr = Cat.ToString();
        }

        if (Pin->PinType.IsArray())      TypeStr = TEXT("Array<") + TypeStr + TEXT(">");
        else if (Pin->PinType.IsSet())   TypeStr = TEXT("Set<")   + TypeStr + TEXT(">");
        else if (Pin->PinType.IsMap())   TypeStr = TEXT("Map<")   + TypeStr + TEXT(">");

        FString Label = Pin->GetDisplayName().ToString();
        if (Label.IsEmpty()) Label = Pin->GetName();

        PinEntries.Add(FString::Printf(TEXT("%s (%s)"), *Label, *TypeStr));
    }

    if (PinEntries.Num() > 15)
    {
        int32 Remaining = PinEntries.Num() - 15;
        PinEntries.SetNum(15);
        PinEntries.Add(FString::Printf(TEXT("...+%d more"), Remaining));
    }

    return FString::Join(PinEntries, TEXT(", "));
}
```

> **Note:** `UEdGraphSchema_K2::PC_Exec` etc. should already be available. Grep the file for `PC_Exec` to confirm the header is included. If not, add `#include "EdGraphSchema_K2.h"`.

### Step 2: Update error sites

Find each pin-not-found error return in `OliveGraphWriter.cpp` and add the pin list. There are 6 sites:

**ConnectPins — source pin not found:**
```cpp
// Before:
return FOliveBlueprintWriteResult::Error(
    FString::Printf(TEXT("Source pin '%s' not found on node '%s'"), *SourcePinName, *SourceNodeId),
    BlueprintPath);

// After:
{
    FString Available = BuildAvailablePinsList(SourceNode, EGPD_Output);
    return FOliveBlueprintWriteResult::Error(
        FString::Printf(TEXT("Source pin '%s' not found on node '%s'. Available output pins: %s"),
            *SourcePinName, *SourceNodeId,
            Available.IsEmpty() ? TEXT("(none)") : *Available),
        BlueprintPath);
}
```

**ConnectPins — target pin not found:**
Same pattern, use `EGPD_Input` and `TargetNode`/`TargetPinName`/`TargetNodeId`.

**DisconnectPins — source pin not found:**
Same pattern as ConnectPins source, `EGPD_Output`.

**DisconnectPins — target pin not found:**
Same pattern as ConnectPins target, `EGPD_Input`.

**DisconnectAllFromPin — pin not found:**
Direction is unknown. List both:
```cpp
{
    FString OutPins = BuildAvailablePinsList(Node, EGPD_Output);
    FString InPins  = BuildAvailablePinsList(Node, EGPD_Input);
    FString All;
    if (!OutPins.IsEmpty()) All += TEXT("Outputs: ") + OutPins;
    if (!InPins.IsEmpty())
    {
        if (!All.IsEmpty()) All += TEXT(". ");
        All += TEXT("Inputs: ") + InPins;
    }
    return FOliveBlueprintWriteResult::Error(
        FString::Printf(TEXT("Pin '%s' not found on node '%s'. Available pins: %s"),
            *PinName, *NodeId,
            All.IsEmpty() ? TEXT("(none)") : *All),
        BlueprintPath);
}
```

**SetPinDefault — pin not found:**
Same as ConnectPins target, use `EGPD_Input`.

### Step 3: Build and verify

Run the build command and confirm it compiles clean:
```bash
"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex
```

Log files if build fails:
- `../../Saved/Logs/UE_Olive_AI_Toolkit.log`
- `C:/Users/mjoff/AppData/Local/UnrealBuildTool/Log.txt`

---

## Notes

- Do not modify any `.h` files
- Do not change any function signatures
- Do not refactor surrounding code — only add the helper and update the 6 error sites
- Task A and Task B are independent — either can be done first
