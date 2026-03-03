# Research: UE 5.5 Autocast / Auto-Conversion API for Blueprint Pin Wiring

## Question
How does UE 5.5's Blueprint system automatically convert between incompatible pin types? What APIs are available and how should Olive's programmatic wiring use them?

## Findings

### 1. SearchForAutocastFunction

**Location:** `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Classes/EdGraphSchema_K2.h` line 1128, implementation in `EdGraphSchema_K2.cpp` line 2558.

**Current (5.2+) signature:**
```cpp
struct FSearchForAutocastFunctionResults
{
    FName TargetFunction;
    UClass* FunctionOwner = nullptr;
};

[[nodiscard]] virtual TOptional<FSearchForAutocastFunctionResults>
    SearchForAutocastFunction(
        const FEdGraphPinType& OutputPinType,
        const FEdGraphPinType& InputPinType) const;
```

The deprecated (pre-5.2) variant returns `bool` with out-params. Use the `TOptional` version.

**How it works (3 layers):**

1. **Hardcoded special cases** (lines 2568-2632):
   - `Set -> Array`: `UBlueprintSetLibrary::Set_ToArray`
   - `Interface -> Object` (when target is UObject): `UKismetSystemLibrary::Conv_InterfaceToObject`
   - `Object -> Class` (child-of check): `UGameplayStatics::GetObjectClass`
   - `Object -> String`: `UKismetSystemLibrary::GetDisplayName`
   - `Class -> String`: `UKismetSystemLibrary::GetClassDisplayName`
   - `Rotator -> Transform`: `UKismetMathLibrary::MakeTransform`

2. **FAutocastFunctionMap** (lines 2636-2665): A global map built by scanning ALL `UBlueprintFunctionLibrary` subclasses for functions with the `BlueprintAutocast` UFUNCTION metadata. The map key is a string encoding of `{InputPinType};{OutputPinType}`. If no exact match, retries with `PC_Float` -> `PC_Double` substitution (float/double interop).

3. **What qualifies as an autocast function** (line 2378):
   ```cpp
   static bool IsAutocastFunction(const UFunction* Function)
   {
       const FName BlueprintAutocast(TEXT("BlueprintAutocast"));
       return Function
           && Function->HasMetaData(BlueprintAutocast)
           && Function->HasAllFunctionFlags(FUNC_Static | FUNC_Native | FUNC_Public | FUNC_BlueprintPure)
           && Function->GetReturnProperty()
           && GetFirstInputProperty(Function);
   }
   ```
   Must be: static, native, public, pure, have exactly one input param and a return value, and tagged `BlueprintAutocast`.

**What it returns:** A function name (`FName`) and its owning class (`UClass*`). NOT a `UFunction*`, NOT a node. The caller creates a `UK2Node_CallFunction` from these.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp` lines 2290-2669

### 2. FindSpecializedConversionNode

**Location:** Same header, line 1142.

**Current (5.2+) signature:**
```cpp
struct FFindSpecializedConversionNodeResults
{
    class UK2Node* TargetNode = nullptr;
};

[[nodiscard]] virtual TOptional<FFindSpecializedConversionNodeResults>
    FindSpecializedConversionNode(
        const FEdGraphPinType& OutputPinType,
        const UEdGraphPin& InputPin,
        bool bCreateNode) const;
```

**What it handles** (lines 2696-2855 — NOT covered by SearchForAutocast):
- **Scalar -> Array**: Creates `UK2Node_MakeArray`
- **Object -> CallFunction self pin** (alternate object property): Creates `UK2Node_VariableGet`
- **Enum -> Name/String**: Creates `UK2Node_GetEnumeratorName` or `UK2Node_GetEnumeratorNameAsString`
- **Byte -> Enum**: Creates `UK2Node_CastByteToEnum` (with `bSafe = true`)
- **Dynamic cast** (Interface->Object, Object->Object downcast): Creates `UK2Node_DynamicCast` with `SetPurity(true)`
- **Soft/Hard object/class conversions**: Creates `UK2Node_ConvertAsset`

Returns a `UK2Node*` template (not yet added to graph). `bCreateNode=false` is a probe-only check.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp` lines 2696-2855

### 3. CreateAutomaticConversionNodeAndConnections (the one-stop shop)

**Location:** Header line 534, implementation line 2896.

```cpp
virtual bool CreateAutomaticConversionNodeAndConnections(
    UEdGraphPin* A,
    UEdGraphPin* B) const override;
```

**This is the key method.** It does everything:

```cpp
bool UEdGraphSchema_K2::CreateAutomaticConversionNodeAndConnections(
    UEdGraphPin* PinA, UEdGraphPin* PinB) const
{
    UEdGraphPin* InputPin = nullptr;
    UEdGraphPin* OutputPin = nullptr;
    if (!CategorizePinsByDirection(PinA, PinB, InputPin, OutputPin))
        return false;

    UK2Node* TemplateConversionNode = nullptr;

    // Step 1: Try autocast function (pure Conv_* functions)
    if (auto AutocastResult = SearchForAutocastFunction(OutputPin->PinType, InputPin->PinType))
    {
        UK2Node_CallFunction* TemplateNode = NewObject<UK2Node_CallFunction>();
        TemplateNode->FunctionReference.SetExternalMember(
            AutocastResult->TargetFunction, AutocastResult->FunctionOwner);
        TemplateConversionNode = TemplateNode;
    }
    // Step 2: Try specialized conversion (MakeArray, DynamicCast, etc.)
    else if (auto ConversionResult = FindSpecializedConversionNode(
                 OutputPin->PinType, *InputPin, true))
    {
        TemplateConversionNode = ConversionResult->TargetNode;
    }

    if (TemplateConversionNode)
    {
        FVector2D Pos = CalculateAveragePositionBetweenNodes(InputPin, OutputPin);
        UK2Node* ConversionNode = FEdGraphSchemaAction_K2NewNode::SpawnNodeFromTemplate<UK2Node>(
            InputPin->GetOwningNode()->GetGraph(), TemplateConversionNode, Pos);
        AutowireConversionNode(InputPin, OutputPin, ConversionNode);
        return true;
    }
    return false;
}
```

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp` lines 2896-2938

### 4. AutowireConversionNode

**Location:** Header line 1165, implementation line 2857.

```cpp
void UEdGraphSchema_K2::AutowireConversionNode(
    UEdGraphPin* InputPin,
    UEdGraphPin* OutputPin,
    UEdGraphNode* ConversionNode) const;
```

Iterates ConversionNode's pins and connects:
- The first conversion input pin type-compatible with `OutputPin` gets wired to `OutputPin`
- The first conversion output pin type-compatible with `InputPin` gets wired to `InputPin`

Uses `ArePinTypesCompatible()` to match, then `TryCreateConnection()` to wire. Stops after one successful connection in each direction.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp` lines 2857-2894

### 5. ArePinTypesCompatible

**Location:** Header line 1091, implementation line 4454.

```cpp
virtual bool ArePinTypesCompatible(
    const FEdGraphPinType& Output,
    const FEdGraphPinType& Input,
    const UClass* CallingContext = NULL,
    bool bIgnoreArray = false) const;
```

Checks **direct compatibility** (can wire without conversion). Key rules:
- Container types must match (unless `bIgnoreArray` or wildcard)
- `PC_Real` (float/double): always compatible with each other (implicit bytecode conversion)
- Struct: must be same or child-of AND have `TheSameLayout`
- Object/Class: child-of or implements-interface checks
- Also checks `FStructConversionTable::Get().GetConversionFunction()` for registered struct conversions

**This does NOT check autocast availability.** It only checks direct wiring. Autocast is checked separately via `SearchForAutocastFunction` and `FindSpecializedConversionNode`.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp` lines 4454-4600+

### 6. The Connection Response Chain

When `CanCreateConnection()` is called (line 2254):
```cpp
const bool bCanAutocast = SearchForAutocastFunction(...).IsSet();
const bool bCanAutoConvert = FindSpecializedConversionNode(..., false).IsSet();
if (bCanAutocast || bCanAutoConvert)
    return FPinConnectionResponse(CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE, ...);
```

Then in base `UEdGraphSchema::TryCreateConnection()` (EdGraphSchema.cpp line 497):
```cpp
case CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE:
    bModified = CreateAutomaticConversionNodeAndConnections(PinA, PinB);
    break;
```

**CRITICAL FINDING:** `FPinConnectionResponse::CanSafeConnect()` returns true ONLY for `CONNECT_RESPONSE_MAKE` and `CONNECT_RESPONSE_MAKE_WITH_PROMOTION`. It does NOT include `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE`. This means:

- `Schema->TryCreateConnection(PinA, PinB)` DOES handle auto-conversion automatically (it sees `MAKE_WITH_CONVERSION_NODE` and calls `CreateAutomaticConversionNodeAndConnections`).
- `Response.CanSafeConnect()` does NOT detect auto-conversion-possible cases.

### 7. SplitPin API

**Location:** Header line 574, implementation line 7240.

```cpp
virtual void SplitPin(UEdGraphPin* Pin, bool bNotify = true) const override;
```

**How it works:**
1. Gets `UScriptStruct*` from `Pin->PinType.PinSubCategoryObject`
2. Hides the parent pin (`Pin->bHidden = true`)
3. Creates a temporary "proto expand node" via `CreateSplitPinNode()`
4. For each visible proto pin matching Pin's direction, creates a sub-pin named `{ParentPinName}_{SubPinName}`
5. Sets `SubPin->ParentPin = Pin` and adds to `Pin->SubPins`
6. Destroys the proto node
7. For input pins, parses default values (Vector: "X,Y,Z", Rotator special order Y,Z,X -> X,Y,Z)

**Sub-pin naming convention:**
- `{PinName}_{ComponentName}` e.g., `Location_X`, `Location_Y`, `Location_Z`
- For Rotator: sub-pin names are the proto pin names from the BreakRotator node

**Pre-check:**
```cpp
bool CanSplitStructPin(const UEdGraphPin& Pin) const;
// Returns: Pin.GetOwningNode()->CanSplitPin(&Pin) && PinHasSplittableStructType(&Pin)
```

**Requirements for splitting:**
- Pin must be a struct type (`PC_Struct`) with a valid `PinSubCategoryObject` (UScriptStruct*)
- The struct must NOT have `MD_NativeDisableSplitPin` metadata
- The owning node must allow splitting (`CanSplitPin` virtual)

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp` lines 7240-7366

### 8. The Wire Drag Path (Blueprint Editor)

When a user drags a wire from an output to an incompatible input in the Blueprint editor, the path is:

1. **SGraphPanel** detects pin release over another pin
2. Calls `Schema->CanCreateConnection(PinA, PinB)` which returns `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE` if autocast is available
3. Calls `Schema->TryCreateConnection(PinA, PinB)`
4. Base `UEdGraphSchema::TryCreateConnection` dispatches to `CreateAutomaticConversionNodeAndConnections`
5. That method creates a conversion node and auto-wires it

For the case of dragging a wire and releasing into empty space, the editor opens a context menu filtered by compatible actions (this is a separate path via `GetGraphContextActions`).

**There is NO Vector->Float auto-decomposition when dragging.** Vector to Float does NOT have a `BlueprintAutocast` function. The user must either:
- Manually add a BreakVector node
- Split the pin (which creates sub-pins for X, Y, Z)

### 9. Common Autocast Functions (BlueprintAutocast metadata)

From `KismetMathLibrary.h`, `KismetStringLibrary.h`, `KismetTextLibrary.h`, `KismetSystemLibrary.h`:

**Numeric:**
| From | To | Function |
|------|----|----------|
| int32 | double | `Conv_IntToDouble` |
| int32 | int64 | `Conv_IntToInt64` |
| int32 | uint8 | `Conv_IntToByte` |
| int32 | bool | `Conv_IntToBool` |
| int32 | FVector | `Conv_IntToVector` |
| int32 | FIntVector | `Conv_IntToIntVector` |
| int64 | int32 | `Conv_Int64ToInt` |
| int64 | uint8 | `Conv_Int64ToByte` |
| int64 | double | `Conv_Int64ToDouble` |
| double | int32 | `FTrunc` |
| double | int64 | `FTrunc64` / `Conv_DoubleToInt64` |
| double | FVector | `Conv_DoubleToVector` |
| double | FVector2D | `Conv_DoubleToVector2D` |
| double | FLinearColor | `Conv_DoubleToLinearColor` |
| uint8 | int32 | `Conv_ByteToInt` |
| uint8 | int64 | `Conv_ByteToInt64` |
| uint8 | double | `Conv_ByteToDouble` |
| bool | int32 | `Conv_BoolToInt` |
| bool | double | `Conv_BoolToDouble` |
| bool | uint8 | `Conv_BoolToByte` |

**Struct conversions:**
| From | To | Function |
|------|----|----------|
| FVector | FLinearColor | `Conv_VectorToLinearColor` |
| FVector | FTransform | `Conv_VectorToTransform` |
| FVector | FVector2D | `Conv_VectorToVector2D` |
| FVector | FRotator | `Conv_VectorToRotator` |
| FVector | FQuat | `Conv_VectorToQuaternion` |
| FVector | FIntVector | `FTruncVector` |
| FVector2D | FVector | `Conv_Vector2DToVector` |
| FVector2D | FIntPoint | `Conv_Vector2DToIntPoint` |
| FVector4 | FVector | `Conv_Vector4ToVector` |
| FVector4 | FRotator | `Conv_Vector4ToRotator` |
| FRotator | FVector | `Conv_RotatorToVector` |
| FRotator | FTransform | `Conv_RotatorToTransform` |
| FRotator | FQuat | `Conv_RotatorToQuaternion` |
| FQuat | FRotator | `Quat_Rotator` |
| FLinearColor | FVector | `Conv_LinearColorToVector` |
| FLinearColor | FColor | `LinearColor_ToRGBE` / `Conv_LinearColorToColor` |
| FColor | FLinearColor | `Conv_ColorToLinearColor` |
| FIntVector | FVector | `Conv_IntVectorToVector` |
| FIntPoint | FVector2D | `Conv_IntPointToVector2D` |
| FTransform | FMatrix | `Conv_TransformToMatrix` |

**String conversions (KismetStringLibrary):** Almost everything converts to FString via `Conv_*ToString`.

**Text conversions (KismetTextLibrary):** Most types also convert to FText.

**Important: NO autocast exists for:**
- `FVector -> float/double` (must use BreakVector + pick component)
- `FRotator -> float/double` (must use BreakRotator)
- `float/double -> FVector` (single double -> FVector exists: `Conv_DoubleToVector`)
- `FString -> int32` and `FString -> double` DO exist but are NOT tagged `BlueprintAutocast` in KismetStringLibrary... Actually they ARE: `Conv_StringToInt` and `Conv_StringToDouble` at lines 104 and 116.

### 10. Current Olive Implementation Status

**OlivePinConnector** (`Source/OliveAIEditor/Blueprint/Private/Writer/OlivePinConnector.cpp`):

The `Connect()` method checks `CanSafeConnect()` first (line 65). If that fails and `bAllowConversion=true`, it calls `GetConversionOptions()` then `InsertConversionNode()`.

**CRITICAL BUG:** `CreateConversionNode()` at line 577 is **NOT IMPLEMENTED** -- it always returns `nullptr` with a warning log. This means every auto-conversion attempt silently fails. The existing code in `GetConversionOptions()` builds ad-hoc string names for conversions but never actually creates nodes.

**CRITICAL BUG #2:** The code checks `CanSafeConnect()` which does NOT include `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE`. When types are autocast-compatible, `CanSafeConnect()` returns false, so the code falls through to the broken `bAllowConversion` path.

**The fix is simple:** Just call `Schema->TryCreateConnection(SourcePin, TargetPin)` directly. The base schema's `TryCreateConnection` already handles `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE` by calling `CreateAutomaticConversionNodeAndConnections`, which does everything correctly:
1. Finds the right conversion (autocast function or specialized node)
2. Creates the template node
3. Spawns it into the graph via `SpawnNodeFromTemplate`
4. Auto-wires it via `AutowireConversionNode`

### 11. How to Programmatically Insert a Conversion (Recommended Approach)

**Approach A: Let TryCreateConnection handle it (simplest, recommended)**
```cpp
const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
bool bSuccess = Schema->TryCreateConnection(OutputPin, InputPin);
// Returns true if connected (directly, with conversion, or with promotion)
```
`TryCreateConnection` internally calls `CanCreateConnection`, and if the response is `MAKE_WITH_CONVERSION_NODE`, it calls `CreateAutomaticConversionNodeAndConnections` which spawns and wires the conversion node. This is the same code path the editor uses.

**Approach B: Probe first, then connect (for reporting)**
```cpp
const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
FPinConnectionResponse Response = Schema->CanCreateConnection(OutputPin, InputPin);

switch (Response.Response)
{
case CONNECT_RESPONSE_MAKE:
case CONNECT_RESPONSE_BREAK_OTHERS_A:
case CONNECT_RESPONSE_BREAK_OTHERS_B:
case CONNECT_RESPONSE_BREAK_OTHERS_AB:
case CONNECT_RESPONSE_MAKE_WITH_PROMOTION:
    // Direct connection possible
    Schema->TryCreateConnection(OutputPin, InputPin);
    break;

case CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE:
    // Conversion needed - TryCreateConnection will handle it
    Schema->TryCreateConnection(OutputPin, InputPin);
    // Conversion node was auto-inserted
    break;

case CONNECT_RESPONSE_DISALLOW:
    // Cannot connect, even with conversion
    break;
}
```

**Approach C: Manual control (for custom positioning or logging)**
```cpp
const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

// Step 1: Find autocast function
auto AutocastResult = Schema->SearchForAutocastFunction(
    OutputPin->PinType, InputPin->PinType);

if (AutocastResult.IsSet())
{
    // Step 2: Create template node
    UK2Node_CallFunction* TemplateNode = NewObject<UK2Node_CallFunction>();
    TemplateNode->FunctionReference.SetExternalMember(
        AutocastResult->TargetFunction, AutocastResult->FunctionOwner);

    // Step 3: Spawn into graph
    FVector2D Position(
        (OutputPin->GetOwningNode()->NodePosX + InputPin->GetOwningNode()->NodePosX) / 2,
        (OutputPin->GetOwningNode()->NodePosY + InputPin->GetOwningNode()->NodePosY) / 2);

    UK2Node* ConversionNode = FEdGraphSchemaAction_K2NewNode::SpawnNodeFromTemplate<UK2Node>(
        OutputPin->GetOwningNode()->GetGraph(), TemplateNode, Position, false);

    // Step 4: Auto-wire
    Schema->AutowireConversionNode(InputPin, OutputPin, ConversionNode);
}
else
{
    // Step 2b: Try specialized conversion
    auto ConvResult = Schema->FindSpecializedConversionNode(
        OutputPin->PinType, *InputPin, true);
    if (ConvResult.IsSet() && ConvResult->TargetNode)
    {
        FVector2D Position = UEdGraphSchema_K2::CalculateAveragePositionBetweenNodes(
            InputPin, OutputPin);
        UK2Node* ConversionNode = FEdGraphSchemaAction_K2NewNode::SpawnNodeFromTemplate<UK2Node>(
            OutputPin->GetOwningNode()->GetGraph(), ConvResult->TargetNode, Position, false);
        Schema->AutowireConversionNode(InputPin, OutputPin, ConversionNode);
    }
}
```

### 12. SplitPin Programmatic Usage

```cpp
const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

// Check if splittable
if (Schema->CanSplitStructPin(*Pin))
{
    Schema->SplitPin(Pin, /*bNotify=*/true);

    // After splitting:
    // Pin->bHidden == true
    // Pin->SubPins contains the sub-pins
    // Sub-pin names: "{PinName}_X", "{PinName}_Y", "{PinName}_Z" for Vector
    // Sub-pin names: "{PinName}_Pitch", "{PinName}_Yaw", "{PinName}_Roll" for Rotator (via proto node)

    for (UEdGraphPin* SubPin : Pin->SubPins)
    {
        // SubPin->PinName is e.g. "Location_X"
        // SubPin->ParentPin == Pin
    }
}
```

**Vector sub-pin names:** `{PinName}_X`, `{PinName}_Y`, `{PinName}_Z`
**Rotator sub-pin names:** `{PinName}_Roll`, `{PinName}_Pitch`, `{PinName}_Yaw` (derived from BreakRotator proto node outputs, NOT from the default value order which is Y,Z,X)
**Vector2D sub-pin names:** `{PinName}_X`, `{PinName}_Y`
**LinearColor sub-pin names:** `{PinName}_R`, `{PinName}_G`, `{PinName}_B`, `{PinName}_A`

**Headers needed:**
- `EdGraphSchema_K2.h`
- `EdGraphSchema_K2_Actions.h` (for `FEdGraphSchemaAction_K2NewNode::SpawnNodeFromTemplate`)
- `K2Node_CallFunction.h` (if creating autocast nodes manually)

## Recommendations

### 1. Fix OlivePinConnector::Connect (Critical)

The current implementation is broken for auto-conversion. The fix is to use `TryCreateConnection` directly instead of checking `CanSafeConnect()` first. Here is the recommended approach:

```cpp
// Instead of:
//   FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);
//   if (Response.CanSafeConnect()) { ... }
//   else if (bAllowConversion) { ... custom broken code ... }

// Do this:
FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);
bool bNeedsConversion = (Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE);

if (bNeedsConversion && !bAllowConversion)
{
    return Error("Types incompatible, conversion needed but not allowed");
}

// TryCreateConnection handles ALL response types correctly,
// including MAKE_WITH_CONVERSION_NODE
bool bSuccess = K2Schema->TryCreateConnection(SourcePin, TargetPin);
```

This eliminates `CreateConversionNode()`, `GetConversionOptions()`, `CanAutoConvert()`, and `InsertConversionNode()` entirely. The engine already does all of this correctly.

### 2. Delete Dead Code

The following methods in OlivePinConnector can be removed:
- `GetConversionOptions()` -- ad-hoc reimplementation of engine logic
- `InsertConversionNode()` -- uses the broken `CreateConversionNode()`
- `CreateConversionNode()` -- returns nullptr
- `CanAutoConvert()` -- ad-hoc reimplementation

Replace with a thin wrapper around `TryCreateConnection`.

### 3. Detect Conversion Node Insertion (for FOliveConversionNote)

After calling `TryCreateConnection` when `bNeedsConversion` was true, detect what happened:

```cpp
// Before TryCreateConnection:
int32 SourceLinkCountBefore = SourcePin->LinkedTo.Num();

// After TryCreateConnection:
// If conversion was inserted, SourcePin links to the conversion node's input,
// NOT to TargetPin directly.
bool bConversionInserted = bNeedsConversion && bSuccess;
// The existing detection logic in PlanExecutor.cpp (line 2561) is correct:
// bConversionInserted = (SourcePin->LinkedTo.Num() > 0 && SourcePin->LinkedTo[0] != TargetPin)
```

### 4. Vector/Rotator to Float: Use SplitPin, Not Autocast

There is **no autocast** from Vector to float. The engine approach is `SplitPin`. For the plan executor, when an AI requests connecting a Vector output to a Float input with a specific component (X/Y/Z), the recommended approach is:
1. Call `Schema->SplitPin(VectorOutputPin)` to create sub-pins
2. Connect the appropriate sub-pin (`{PinName}_X`, etc.) to the Float input

Alternatively, the AI should use a `break_struct` plan op to decompose the vector first.

### 5. Thread Safety

All autocast APIs must be called on the game thread. `FAutocastFunctionMap` is a static singleton with no thread synchronization. `SpawnNodeFromTemplate` creates UObjects. No exceptions.

### 6. Include Headers

For the recommended approach, include:
```cpp
#include "EdGraphSchema_K2.h"       // Schema, ArePinTypesCompatible, TryCreateConnection
#include "EdGraph/EdGraphSchema.h"   // ECanCreateConnectionResponse enum
// SpawnNodeFromTemplate is NOT needed if using TryCreateConnection (it handles it internally)
```

### 7. FAutocastFunctionMap Is Internal

`FAutocastFunctionMap` is a file-scope `struct` inside `EdGraphSchema_K2.cpp` -- you cannot access it directly. You must go through `SearchForAutocastFunction()` on the schema. This is fine since `TryCreateConnection` wraps everything.

### 8. The Response Enum Is the Key Discriminator

When probing before connecting:
- `CONNECT_RESPONSE_MAKE` / `BREAK_OTHERS_*` / `MAKE_WITH_PROMOTION`: direct wire
- `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE`: autocast needed, `TryCreateConnection` will handle it
- `CONNECT_RESPONSE_DISALLOW`: no connection possible

This is more reliable than any manual type checking.
