# Research: UAnimStateTransitionNode API in Unreal Engine 5.5

## Question
How does UAnimStateTransitionNode work in Unreal Engine 5.5?

Specifically:
1. What methods exist for getting pins on UAnimStateTransitionNode? (The old API used GetPreviousStatePin() and GetNextStatePin() but these don't exist in UE 5.5)
2. How to properly connect transitions between animation states in UE 5.5
3. What replaced CreatePinsForTransition() if it doesn't exist

## Findings

### Pin Access Methods (UE 5.5)

The old methods `GetPreviousStatePin()` and `GetNextStatePin()` **do not exist** in UE 5.5.

Instead, UAnimStateTransitionNode uses the standard pin array from UEdGraphNode and provides these methods:

```cpp
// From AnimStateTransitionNode.h lines 140-141
virtual UEdGraphPin* GetInputPin() const override { return Pins[0]; }
virtual UEdGraphPin* GetOutputPin() const override { return Pins[1]; }
```

**Source:** `C:\Program Files\Epic Games\UE_5.5\Engine\Source\Editor\AnimGraph\Public\AnimStateTransitionNode.h`

These methods are overridden from the base class `UAnimStateNodeBase`:

```cpp
// From AnimStateNodeBase.h lines 33-37
// @return the input pin for this state
ANIMGRAPH_API virtual UEdGraphPin* GetInputPin() const { return NULL; }

// @return the output pin for this state
ANIMGRAPH_API virtual UEdGraphPin* GetOutputPin() const { return NULL; }
```

**Source:** `C:\Program Files\Epic Games\UE_5.5\Engine\Source\Editor\AnimGraph\Public\AnimStateNodeBase.h`

### Pin Creation (AllocateDefaultPins)

`CreatePinsForTransition()` **does not exist** in UE 5.5.

Instead, pins are created in the standard UEdGraphNode pattern via `AllocateDefaultPins()`:

```cpp
// From AnimStateTransitionNode.cpp lines 93-99
void UAnimStateTransitionNode::AllocateDefaultPins()
{
    UEdGraphPin* Inputs = CreatePin(EGPD_Input, TEXT("Transition"), TEXT("In"));
    Inputs->bHidden = true;
    UEdGraphPin* Outputs = CreatePin(EGPD_Output, TEXT("Transition"), TEXT("Out"));
    Outputs->bHidden = true;
}
```

**Source:** `C:\Program Files\Epic Games\UE_5.5\Engine\Source\Editor\AnimGraph\Private\AnimStateTransitionNode.cpp`

This creates:
- Pin 0: Input pin (hidden, connected to previous state's output)
- Pin 1: Output pin (hidden, connected to next state's input)

### Connecting Transitions Between States

The correct method to connect a transition between two states is `CreateConnections()`:

```cpp
// From AnimStateTransitionNode.cpp lines 316-331
void UAnimStateTransitionNode::CreateConnections(UAnimStateNodeBase* PreviousState, UAnimStateNodeBase* NextState)
{
    // Previous to this
    Pins[0]->Modify();
    Pins[0]->LinkedTo.Empty();

    PreviousState->GetOutputPin()->Modify();
    Pins[0]->MakeLinkTo(PreviousState->GetOutputPin());

    // This to next
    Pins[1]->Modify();
    Pins[1]->LinkedTo.Empty();

    NextState->GetInputPin()->Modify();
    Pins[1]->MakeLinkTo(NextState->GetInputPin());
}
```

**Source:** `C:\Program Files\Epic Games\UE_5.5\Engine\Source\Editor\AnimGraph\Private\AnimStateTransitionNode.cpp`

**Connection Flow:**
1. Input pin (Pins[0]) connects to the previous state's output pin
2. Output pin (Pins[1]) connects to the next state's input pin

### Getting Connected States

To retrieve the states connected to a transition:

```cpp
// From AnimStateTransitionNode.cpp lines 269-291
UAnimStateNodeBase* UAnimStateTransitionNode::GetPreviousState() const
{
    if (Pins[0]->LinkedTo.Num() > 0)
    {
        return Cast<UAnimStateNodeBase>(Pins[0]->LinkedTo[0]->GetOwningNode());
    }
    else
    {
        return NULL;
    }
}

UAnimStateNodeBase* UAnimStateTransitionNode::GetNextState() const
{
    if (Pins[1]->LinkedTo.Num() > 0)
    {
        return Cast<UAnimStateNodeBase>(Pins[1]->LinkedTo[0]->GetOwningNode());
    }
    else
    {
        return NULL;
    }
}
```

**Source:** `C:\Program Files\Epic Games\UE_5.5\Engine\Source\Editor\AnimGraph\Private\AnimStateTransitionNode.cpp`

### Bound Graph (Transition Logic)

Each transition node has a `BoundGraph` that contains the transition rule (boolean logic):

```cpp
// From AnimStateTransitionNode.h line 26
UPROPERTY()
TObjectPtr<class UEdGraph> BoundGraph;
```

Access via:
```cpp
// From AnimStateTransitionNode.h line 138
virtual UEdGraph* GetBoundGraph() const override { return BoundGraph; }
```

**Source:** `C:\Program Files\Epic Games\UE_5.5\Engine\Source\Editor\AnimGraph\Public\AnimStateTransitionNode.h`

The BoundGraph is created automatically in `PostPlacedNewNode()` via `CreateBoundGraph()`.

## Recommendations

### For Blueprint Graph Writers (OliveAnimGraphWriter)

1. **Pin Access:** Always use `GetInputPin()` and `GetOutputPin()` - never try to access Pins[0] or Pins[1] directly from outside the class.

2. **Creating Transitions:**
   ```cpp
   // Create the transition node
   UAnimStateTransitionNode* TransitionNode = NewObject<UAnimStateTransitionNode>(StateMachineGraph);
   TransitionNode->AllocateDefaultPins();
   
   // Connect it to states
   TransitionNode->CreateConnections(FromState, ToState);
   
   // Access the transition rule graph
   UEdGraph* RuleGraph = TransitionNode->GetBoundGraph();
   ```

3. **Getting States:** Use `GetPreviousState()` and `GetNextState()` to retrieve connected states, not the old pin getter methods.

4. **Thread Safety:** All pin modifications use `Modify()` for undo support - always wrap in FScopedTransaction.

### Gotchas

- **Pin indices are fixed:** Input is always Pins[0], output is always Pins[1]
- **Pins are hidden:** The pins have `bHidden = true` so they won't appear in the visual editor
- **BoundGraph is auto-created:** You don't need to manually create the transition rule graph; it's done in `PostPlacedNewNode()`
- **State interface:** Both UAnimStateNode and UAnimStateTransitionNode inherit from UAnimStateNodeBase, which provides the GetInputPin/GetOutputPin interface

### Version Differences

The API in UE 5.5 is **significantly different** from older versions:
- **Removed:** `GetPreviousStatePin()`, `GetNextStatePin()`, `CreatePinsForTransition()`
- **Added/Changed:** Pin access is now via inherited `GetInputPin()`/`GetOutputPin()` from UAnimStateNodeBase
- **Connection:** `CreateConnections(UAnimStateNodeBase*, UAnimStateNodeBase*)` is the standard way to wire transitions

If supporting multiple UE versions, use preprocessor checks:
```cpp
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
    // Use GetInputPin() / GetOutputPin()
#else
    // Use old API if it existed
#endif
```

## Summary

| Old API (pre-5.5?) | UE 5.5 API | Purpose |
|-------------------|-----------|---------|
| `GetPreviousStatePin()` | `GetInputPin()` | Get the pin connected to previous state |
| `GetNextStatePin()` | `GetOutputPin()` | Get the pin connected to next state |
| `CreatePinsForTransition()` | `AllocateDefaultPins()` | Create the transition node's pins |
| N/A | `CreateConnections(prev, next)` | Wire transition between states |
| N/A | `GetPreviousState()` | Get the state node this transitions from |
| N/A | `GetNextState()` | Get the state node this transitions to |

