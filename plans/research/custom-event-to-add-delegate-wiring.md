# Research: CustomEvent → AddDelegate Delegate Pin Wiring

## Question

Does `UK2Node_CustomEvent` expose a `PC_Delegate` output pin that can be directly wired to `UK2Node_AddDelegate`'s delegate input pin? Or is an intermediate `UK2Node_CreateDelegate` node required?

---

## Findings

### 1. UK2Node_Event (base class): The "OutputDelegate" pin

`UK2Node_Event::AllocateDefaultPins()` unconditionally creates a `PC_Delegate` output pin as its **first pin**:

```cpp
// K2Node_Event.cpp line 345–348
void UK2Node_Event::AllocateDefaultPins()
{
    CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Delegate, DelegateOutputName);
    CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
    ...
}
```

The constant is defined at line 48:
```cpp
const FName UK2Node_Event::DelegateOutputName(TEXT("OutputDelegate"));
```

This is a static public member declared in `K2Node_Event.h`:
```cpp
BLUEPRINTGRAPH_API static const FName DelegateOutputName;
```

`UK2Node_CustomEvent` inherits from `UK2Node_Event` and does NOT override `AllocateDefaultPins()` — it gets this pin for free from the base class.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_Event.cpp` lines 345–348, 48
Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Classes/K2Node_Event.h` line 41

---

### 2. UK2Node_AddDelegate: The "Delegate" input pin

`UK2Node_AddDelegate::AllocateDefaultPins()` (inside `K2Node_MCDelegate.cpp`) creates the delegate input pin:

```cpp
// K2Node_MCDelegate.cpp lines 334–346
void UK2Node_AddDelegate::AllocateDefaultPins()
{
    Super::AllocateDefaultPins();  // creates exec-in, then, Target (PC_Object)

    UEdGraphNode::FCreatePinParams PinParams;
    PinParams.bIsReference = true;
    PinParams.bIsConst = true;
    if (UEdGraphPin* DelegatePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Delegate,
                                             FK2Node_BaseMCDelegateHelper::DelegatePinName, PinParams))
    {
        FMemberReference::FillSimpleMemberReference<UFunction>(GetDelegateSignature(),
            DelegatePin->PinType.PinSubCategoryMemberReference);
        DelegatePin->PinFriendlyName = NSLOCTEXT("K2Node", "PinFriendlyDelegatetName", "Event");
    }
}
```

The pin's **internal name** is `"Delegate"` (from `FK2Node_BaseMCDelegateHelper::DelegatePinName`, defined at line 50 as `FName(TEXT("Delegate"))`).
The pin's **friendly name** (what the user sees in the editor UI) is `"Event"`.

`UK2Node_BaseMCDelegate::GetDelegatePin()` retrieves it:
```cpp
UEdGraphPin* UK2Node_BaseMCDelegate::GetDelegatePin() const
{
    return FindPin(FK2Node_BaseMCDelegateHelper::DelegatePinName);
}
```

The delegate pin is typed `PC_Delegate`, `bIsConst = true`, `bIsReference = true`.
Its `PinSubCategoryMemberReference` is filled with the dispatcher's signature function via `FMemberReference::FillSimpleMemberReference`.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_MCDelegate.cpp` lines 50, 140–143, 334–346

---

### 3. Direct Connection: CustomEvent.OutputDelegate → AddDelegate.Delegate

**Yes — this is the exact pattern Epic uses internally.** Two separate places in the engine source confirm the direct wiring (no intermediate node required):

**Pattern A — `FEdGraphSchemaAction_K2AssignDelegate::AssignDelegate` (EdGraphSchema_K2_Actions.cpp lines 404–414):**
This is invoked when the user selects "Assign Delegate" from the right-click menu on an event dispatcher. It:
1. Creates the `UK2Node_AddDelegate` node
2. Creates a `UK2Node_CustomEvent` via `CreateFromFunction` (with matching signature)
3. Directly connects `CustomEvent.OutputDelegate` → `AddDelegate.Delegate`:

```cpp
UK2Node_CustomEvent* EventNode = UK2Node_CustomEvent::CreateFromFunction(
    FVector2D(Location.X - 150, Location.Y + 150),
    ParentGraph, FunctionName, DelegateProperty->SignatureFunction, bSelectNewNode);
if (EventNode)
{
    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
    UEdGraphPin* OutDelegatePin = EventNode->FindPinChecked(UK2Node_CustomEvent::DelegateOutputName);
    UEdGraphPin* InDelegatePin = BindNode->GetDelegatePin();
    K2Schema->TryCreateConnection(OutDelegatePin, InDelegatePin);
}
```

**Pattern B — `K2Node_AssignDelegate.cpp` lines 107–113:**
Same pattern, used internally when a new `UK2Node_AssignDelegate` node is placed:
```cpp
UEdGraphPin* OutDelegatePin = EventNode->FindPinChecked(UK2Node_CustomEvent::DelegateOutputName);
K2Schema->TryCreateConnection(OutDelegatePin, InDelegatePin);
```

Both call `TryCreateConnection(CustomEvent.OutputDelegate, AddDelegate.Delegate)` directly. No intermediate `UK2Node_CreateDelegate` is used in these flows.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2_Actions.cpp` lines 388–421
Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_AssignDelegate.cpp` lines 95–116

---

### 4. Type Compatibility: How TryCreateConnection Accepts the Pair

The `PC_Delegate` compatibility check in `UEdGraphSchema_K2::ArePinsCompatible` (EdGraphSchema_K2.cpp lines 4587–4622):

```cpp
else if (PC_Delegate == Output.PinCategory || PC_MCDelegate == Output.PinCategory)
{
    const UFunction* OutFunction = FMemberReference::ResolveSimpleMemberReference<UFunction>(
        Output.PinSubCategoryMemberReference);
    const UFunction* InFunction = FMemberReference::ResolveSimpleMemberReference<UFunction>(
        Input.PinSubCategoryMemberReference);
    return !OutFunction || !InFunction || OutFunction->IsSignatureCompatibleWith(InFunction);
}
```

Key behavior: **if either function is null (unresolved), the pins are considered compatible**. This means:
- A freshly created `UK2Node_CustomEvent` with no parameters yet (whose `OutputDelegate` has an empty/unresolved `PinSubCategoryMemberReference`) will match any delegate input pin.
- Once the CustomEvent's `OutputDelegate` has a resolved signature (set by `UpdateDelegatePin()`), it must be signature-compatible with the AddDelegate's dispatcher signature.

`UpdateDelegatePin()` is called from `PinConnectionListChanged()` on the CustomEvent node — meaning the CustomEvent's delegate pin signature updates **after** the connection is made, by inspecting what it's connected to.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/EdGraphSchema_K2.cpp` lines 4587–4622
Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_Event.cpp` lines 293–333

---

### 5. What ReconstructNode Does with the Connection

When `UK2Node_CustomEvent::ReconstructNode()` is called after the connection exists, it reads back the delegate signature from the connected `UK2Node_BaseMCDelegate` node and calls `SetDelegateSignature()` to update the CustomEvent's output parameter pins to match:

```cpp
void UK2Node_CustomEvent::ReconstructNode()
{
    const UEdGraphPin* DelegateOutPin = FindPin(DelegateOutputName);
    const UEdGraphPin* LinkedPin = (DelegateOutPin && DelegateOutPin->LinkedTo.Num() && ...)
        ? FindFirstCompilerRelevantLinkedPin(DelegateOutPin->LinkedTo[0]) : nullptr;

    const UFunction* DelegateSignature = nullptr;
    if (LinkedPin)
    {
        if (const UK2Node_BaseMCDelegate* OtherNode = Cast<const UK2Node_BaseMCDelegate>(LinkedPin->GetOwningNode()))
        {
            DelegateSignature = OtherNode->GetDelegateSignature();
        }
        else if (LinkedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
        {
            DelegateSignature = FMemberReference::ResolveSimpleMemberReference<UFunction>(...);
        }
    }

    const bool bUseDelegateSignature = (nullptr == FindEventSignatureFunction()) && DelegateSignature;
    if (bUseDelegateSignature)
    {
        SetDelegateSignature(DelegateSignature);
    }
    Super::ReconstructNode();
}
```

This means: if the CustomEvent was created with matching parameters (via `CreateFromFunction`), those pins survive reconstruction. If the CustomEvent was created empty and connected, `ReconstructNode` will adopt the connected dispatcher's signature as the CustomEvent's pin layout.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_CustomEvent.cpp` lines 425–456

---

### 6. Role of UK2Node_CreateDelegate (The Alternative Path)

`UK2Node_CreateDelegate` is a **pure node** titled "Create Event" in the editor. It has:
- Input: `PC_Object` pin (named `"self"` internally, friendly name `"Object"`) — the object scope for the function reference
- Output: `PC_Delegate` pin (named `"OutputDelegate"`, friendly name `"Event"`)

Its output IS a `PC_Delegate` pin and CAN be connected to `AddDelegate.Delegate` — but this is a different pattern. The "Create Event" node stores only a **function name reference** (`SelectedFunctionName` FName), not an actual event node connection. It is used for **binding existing functions** to delegates without creating a new custom event node.

`K2Node_BaseAsyncTask` explicitly comments that the `CreateDelegate` node is a workaround:
```cpp
// WORKAROUND, so we can create delegate from nonexistent function by avoiding check at expanding step
// instead simply: Schema->TryCreateConnection(AddDelegateNode->GetDelegatePin(),
//     CurrentCENode->FindPinChecked(UK2Node_CustomEvent::DelegateOutputName));
```

So the **preferred/simple path is the direct CustomEvent connection**. The CreateDelegate path is a workaround used in async task expansion specifically to avoid a validation check during intermediate compilation.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_BaseAsyncTask.cpp` lines 259–277
Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Private/K2Node_CreateDelegate.cpp` lines 35–56

---

### 7. Pin Visibility: Is the OutputDelegate Pin Hidden?

`UK2Node_Event::AllocateDefaultPins()` does NOT set `bHidden = true` on the `OutputDelegate` pin. The `FadeNodeWhenDraggingOffPin` logic in `EdGraphSchema_K2.cpp` specifically highlights `UK2Node_Event` nodes when dragging off a delegate input pin, confirming the pin is visible and draggable in the editor.

However, in practice this pin is often tucked below the node's exec pin and parameters when an event node has user-defined output pins. It is always present.

---

## Exact Pin Names Summary

| Node | Pin FName | Pin Direction | Pin Category | Notes |
|---|---|---|---|---|
| `UK2Node_CustomEvent` (and base `UK2Node_Event`) | `"OutputDelegate"` | Output | `PC_Delegate` | `UK2Node_Event::DelegateOutputName` constant. Present unconditionally. |
| `UK2Node_AddDelegate` | `"Delegate"` | Input | `PC_Delegate` | `FK2Node_BaseMCDelegateHelper::DelegatePinName`. Friendly name = `"Event"`. `bIsConst=true`, `bIsReference=true`. Access via `GetDelegatePin()`. |

---

## Recommendations

1. **Direct connection works. No intermediate node needed.** Wire `CustomEvent.OutputDelegate` → `AddDelegate.Delegate` using `TryCreateConnection()`. This is exactly what `FEdGraphSchemaAction_K2AssignDelegate` does — confirmed from two separate call sites.

2. **Pin access API:**
   - CustomEvent's output: `EventNode->FindPinChecked(UK2Node_CustomEvent::DelegateOutputName)` — or equivalently `UK2Node_Event::DelegateOutputName`, they are the same static constant.
   - AddDelegate's input: `AddDelegateNode->GetDelegatePin()` — returns `FindPin(FName("Delegate"))`.

3. **Signature matching is lenient during connection.** `ArePinsCompatible` returns `true` if either function reference is unresolved. A brand-new CustomEvent with empty `PinSubCategoryMemberReference` on its `OutputDelegate` will match any AddDelegate input. The signature is reconciled after connection via `UpdateDelegatePin` / `ReconstructNode`.

4. **Use `UK2Node_CustomEvent::CreateFromFunction` to pre-populate parameters.** Pass `DelegateProperty->SignatureFunction` to create a CustomEvent whose output parameters already match the dispatcher signature. Then connect `OutputDelegate` → `Delegate`. The CustomEvent will NOT adopt the dispatcher's signature on reconstruct (because `FindEventSignatureFunction()` returns non-null when set via `SetDelegateSignature`).

5. **Don't use `UK2Node_CreateDelegate` for the `bind_dispatcher` op.** It's a workaround for a different compile-time constraint in async task expansion. For plan_json `bind_dispatcher`, the correct pattern is CustomEvent → AddDelegate directly.

6. **`TryCreateConnection` (not `CanSafeConnect`) is the right call.** The `PC_Delegate` connection path is normal `CONNECT_RESPONSE_MAKE`. No conversion node, no promotion. `TryCreateConnection` handles the full pin-link lifecycle including calling `PinConnectionListChanged` which triggers `UpdateDelegatePin`.

7. **After connecting, call `ReconstructNode` on the CustomEvent if you want its parameter pins updated from the dispatcher.** Only needed if the CustomEvent was created without pre-populating parameters.
