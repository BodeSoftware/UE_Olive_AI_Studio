# Error Messages Design: Actionable Pin Wiring Failure Diagnostics

## Problem Statement

When the AI tries to connect incompatible pins, the current error messages are opaque. For example:

**Current (PinConnector):**
```
"Cannot connect pins: [schema message string]"
```

**Current (PlanExecutor Phase 4):**
```
"Pin connection failed (step_a.ReturnValue -> step_b.Location): Cannot connect pins and no conversion available: ..."
```

**Current (connect_pins tool handler):**
```
"BP_CONNECT_PINS_FAILED" / "Verify the pin references are valid and compatible"
```

The AI does not learn:
- What types are involved (Vector output vs Float input)
- Why the connection failed (no autocast exists for this pair)
- What specific alternatives exist (break_struct, split pin ~suffix, Conv_ function, Python)

## Scope: Post-Auto-Fix Error Path Only

Two parallel features are being designed:
1. **Autocast integration** -- fixes `OlivePinConnector::Connect()` to use `TryCreateConnection` (handles Conv_* automatically)
2. **SplitPin auto-detection** -- auto-splits struct pins when connecting to scalar inputs

This design covers what happens AFTER both auto-fixes fail -- the error messages that guide the AI when programmatic recovery is not possible. The enriched errors trigger only on the `CONNECT_RESPONSE_DISALLOW` path.

---

## Design

### 1. New Struct: `FOliveWiringDiagnostic`

A structured diagnostic that replaces bare error strings at the `OlivePinConnector` level. This is the foundational type -- all downstream consumers (PlanExecutor, connect_pins handler, self-correction policy) receive this instead of bare strings.

**File:** `Source/OliveAIEditor/Blueprint/Public/Writer/OlivePinConnector.h`

```cpp
/**
 * Categorizes the reason a pin connection failed.
 * Used to select the appropriate alternative suggestion strategy.
 */
enum class EOliveWiringFailureReason : uint8
{
    /** Pin types are completely incompatible (e.g., Exec -> Float) */
    TypesIncompatible,

    /** Struct output -> scalar input; needs decomposition (BreakVector, SplitPin) */
    StructToScalar,

    /** Scalar output -> struct input; needs composition (MakeVector) */
    ScalarToStruct,

    /** Object type mismatch; needs explicit cast */
    ObjectCastRequired,

    /** Container type mismatch (Array vs single, Set vs Array) */
    ContainerMismatch,

    /** Direction mismatch (output -> output, input -> input) */
    DirectionMismatch,

    /** Same-node connection attempt */
    SameNode,

    /** Pin is already connected and does not allow multiple */
    AlreadyConnected,

    /** Unknown / uncategorized failure */
    Unknown,
};

/**
 * A single actionable alternative the AI can try to resolve a wiring failure.
 */
struct FOliveWiringAlternative
{
    /** Brief label for what this alternative does (e.g., "Use break_struct op") */
    FString Label;

    /** Specific action the AI should take, in tool-call terms */
    FString Action;

    /** Confidence: "high" if this is the standard fix, "medium" if it may work,
     *  "low" if it is a fallback. Helps the AI prioritize. */
    FString Confidence;
};

/**
 * Structured diagnostic for a pin wiring failure.
 * Produced by FOlivePinConnector::Connect() on failure.
 * Contains: what went wrong, why, and what to try instead.
 */
struct OLIVEAIEDITOR_API FOliveWiringDiagnostic
{
    /** Categorized failure reason */
    EOliveWiringFailureReason Reason = EOliveWiringFailureReason::Unknown;

    /** Human-readable source pin type (e.g., "Vector", "Float", "Actor Object Reference") */
    FString SourceTypeName;

    /** Human-readable target pin type */
    FString TargetTypeName;

    /** Source pin name */
    FString SourcePinName;

    /** Target pin name */
    FString TargetPinName;

    /** The raw schema response message (preserved for logging) */
    FString SchemaMessage;

    /** Why the specific auto-fix paths failed (e.g., "No autocast function for Vector -> Float",
     *  "Pin is not splittable (MD_NativeDisableSplitPin)") */
    FString WhyAutoFixFailed;

    /** Ordered list of alternatives the AI can try */
    TArray<FOliveWiringAlternative> Alternatives;

    /** Serialize to JSON for inclusion in tool results */
    TSharedPtr<FJsonObject> ToJson() const;

    /** Format as a single human-readable string (for log and existing string-based paths) */
    FString ToHumanReadable() const;
};
```

### 2. OlivePinConnector Changes

The Connect() method currently returns `FOliveBlueprintWriteResult` with bare error strings. We add a diagnostic field to the result and populate it on failure.

**Change 1:** Add `TOptional<FOliveWiringDiagnostic> WiringDiagnostic` to `FOliveBlueprintWriteResult`.

**File:** `Source/OliveAIEditor/Blueprint/Public/Writer/OliveBlueprintWriter.h`

```cpp
struct OLIVEAIEDITOR_API FOliveBlueprintWriteResult
{
    // ... existing fields ...

    /** Structured wiring diagnostic when a pin connection fails.
     *  Only populated by FOlivePinConnector::Connect() on CONNECT_RESPONSE_DISALLOW. */
    TOptional<FOliveWiringDiagnostic> WiringDiagnostic;

    // ... existing methods ...
};
```

This is a non-breaking addition. Existing callers that ignore this field continue to work. Callers that want richer errors inspect it.

**Change 2:** New private method on `FOlivePinConnector` that builds the diagnostic.

```cpp
private:
    /**
     * Build a structured wiring diagnostic for a failed connection.
     * Called when TryCreateConnection returns false AND CanCreateConnection
     * returns CONNECT_RESPONSE_DISALLOW.
     *
     * Probes: SearchForAutocastFunction, FindSpecializedConversionNode,
     * CanSplitStructPin, and manual Conv_* lookup to determine what failed
     * and what alternatives exist.
     */
    FOliveWiringDiagnostic BuildWiringDiagnostic(
        const UEdGraphPin* SourcePin,
        const UEdGraphPin* TargetPin,
        const FPinConnectionResponse& Response) const;

    /**
     * Given source and target pin types, suggest alternatives.
     * This is the "alternative suggestion engine" -- the core of this design.
     */
    TArray<FOliveWiringAlternative> SuggestAlternatives(
        const UEdGraphPin* SourcePin,
        const UEdGraphPin* TargetPin,
        EOliveWiringFailureReason Reason) const;
```

**Change 3:** Updated Connect() failure path.

The Connect() method (after autocast integration) will call `TryCreateConnection` first. If that returns false, it calls `CanCreateConnection` to get the response type. If `CONNECT_RESPONSE_DISALLOW`, it builds the diagnostic:

```cpp
// After TryCreateConnection fails:
FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);

FOliveWiringDiagnostic Diagnostic = BuildWiringDiagnostic(SourcePin, TargetPin, Response);
FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Error(
    Diagnostic.ToHumanReadable(), AssetPath);
Result.WiringDiagnostic = MoveTemp(Diagnostic);
return Result;
```

### 3. Alternative Suggestion Engine

The `SuggestAlternatives` method categorizes the failure and produces ordered alternatives. The logic is table-driven by `EOliveWiringFailureReason` and the pin type pair.

#### Failure Categorization (in BuildWiringDiagnostic)

```
SourceType     TargetType      Reason
----------     ----------      ------
PC_Struct      PC_Float/etc    StructToScalar
PC_Struct      PC_Double/etc   StructToScalar
PC_Float/etc   PC_Struct       ScalarToStruct
PC_Object      PC_Object       ObjectCastRequired  (when SubCategoryObject differs)
PC_Object      PC_Interface    ObjectCastRequired
Array<T>       T               ContainerMismatch
T              Array<T>        ContainerMismatch
Output         Output          DirectionMismatch
Input          Input           DirectionMismatch
same node      same node       SameNode
*              *               TypesIncompatible   (default)
```

#### Alternative Suggestions by Category

**StructToScalar** (e.g., Vector output -> Float input):

| Priority | Label | Action | Confidence |
|----------|-------|--------|------------|
| 1 | Use break_struct op | `Add a break_struct step for the struct, then wire the specific field (e.g., @break_step.X) to the Float input.` | high |
| 2 | Use ~PinName_X suffix | `In plan_json inputs, use @source_step.~ReturnValue_X to target the X sub-component. The ~ prefix triggers fuzzy match on split sub-pins.` | high |
| 3 | Use get_node_pins + connect_pins | `Call blueprint.get_node_pins on the source node to see all pins (including split sub-pins). Then use connect_pins with the exact sub-pin name.` | medium |
| 4 | Use editor.run_python | `Schema->SplitPin(Pin) creates sub-pins programmatically. Use editor.run_python to split the pin and wire the sub-pin.` | low |

Note: entries 1 and 2 are only suggested when the struct IS splittable (Vector, Rotator, LinearColor, Transform, Vector2D). When the struct is NOT splittable, only entries 3 and 4 are offered, and the diagnostic says "Struct type X does not support pin splitting."

**ScalarToStruct** (e.g., Float output -> Vector input):

| Priority | Label | Action | Confidence |
|----------|-------|--------|------------|
| 1 | Use make_struct op | `Add a make_struct step for the target type (e.g., make_struct target:Vector) to compose the struct from scalar inputs.` | high |
| 2 | Use Conv_ if available | `If a Conv_ function exists (e.g., Conv_DoubleToVector), add a call step: {"op":"call","target":"Conv_DoubleToVector","inputs":{"InDouble":"@source"}}` | medium |
| 3 | Use editor.run_python | `Compose the struct manually in Python.` | low |

For entry 2, the engine actually probes `SearchForAutocastFunction(TargetType, SourceType)` (reversed direction) to check if there is a reverse conversion. If there IS a Conv_ function (like `Conv_DoubleToVector`), it becomes priority 1 with "high" confidence and the specific function name is included.

**ObjectCastRequired** (e.g., Actor -> Character):

| Priority | Label | Action | Confidence |
|----------|-------|--------|------------|
| 1 | Add a cast step | `Add a cast step in plan_json: {"op":"cast","target":"Character","inputs":{"Object":"@source_step"}}. Wire the output cast pin to the target input.` | high |
| 2 | Use connect_pins after cast | `If using granular tools, add_node type:"Cast" properties:{"TargetType":"Character"}, wire source -> Cast input, then cast output -> target input.` | medium |

**ContainerMismatch** (e.g., Array<Actor> -> Actor):

| Priority | Label | Action | Confidence |
|----------|-------|--------|------------|
| 1 | Add an array operation | `To get a single element from an Array, add a call step for "Get" (array access by index) or "GetCopy" between the source and target.` | high |
| 2 | Use editor.run_python | `For complex container transformations, use Python.` | low |

**DirectionMismatch / SameNode:**

| Priority | Label | Action | Confidence |
|----------|-------|--------|------------|
| 1 | Fix pin direction | `Source must be an output pin (EGPD_Output) and target must be an input pin (EGPD_Input). Swap the source and target parameters.` | high |

**TypesIncompatible (generic fallback):**

| Priority | Label | Action | Confidence |
|----------|-------|--------|------------|
| 1 | Check your plan logic | `Types ${SourceType} and ${TargetType} have no known conversion path. This usually means the data flow is wrong at a design level. Reconsider which node outputs should feed this input.` | high |
| 2 | Use editor.run_python | `For unconventional type conversions, Python can bypass Blueprint type constraints.` | low |

#### Splittable Struct Detection

The engine checks `Schema->CanSplitStructPin(SourcePin)` and reports the sub-pin names that WOULD exist if the pin were split. For `Vector`:

```
Diagnostic.WhyAutoFixFailed =
    "No autocast for Vector -> Float. Pin is splittable into: "
    "ReturnValue_X (Float), ReturnValue_Y (Float), ReturnValue_Z (Float).";
```

The sub-pin names are computed from the struct's properties WITHOUT actually splitting the pin (this is a diagnostic probe). The known struct decompositions are hardcoded:

| Struct | Sub-pins |
|--------|----------|
| Vector | `_X`, `_Y`, `_Z` |
| Rotator | `_Roll`, `_Pitch`, `_Yaw` |
| Vector2D | `_X`, `_Y` |
| LinearColor | `_R`, `_G`, `_B`, `_A` |
| Transform | (not directly splittable into scalars; break_struct recommended) |

### 4. Integration Point: PlanExecutor Phase 4 (WireData)

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`, `WireDataConnection` method.

**Current behavior** (lines 2604-2611):

```cpp
Result.ErrorMessage = FString::Printf(
    TEXT("Pin connection failed (%s.%s -> %s.%s): %s"),
    *SourceStepId, *SourcePin->PinName,
    *TargetStepId, *TargetPin->PinName,
    ConnectResult.Errors.Num() > 0 ? *ConnectResult.Errors[0] : TEXT("Unknown"));
```

**New behavior:**

```cpp
if (ConnectResult.WiringDiagnostic.IsSet())
{
    const FOliveWiringDiagnostic& Diag = ConnectResult.WiringDiagnostic.GetValue();
    Result.ErrorMessage = FString::Printf(
        TEXT("Pin connection failed (%s.%s [%s] -> %s.%s [%s]): %s"),
        *SourceStepId, *SourcePin->PinName, *Diag.SourceTypeName,
        *TargetStepId, *TargetPin->PinName, *Diag.TargetTypeName,
        *Diag.SchemaMessage);

    // Surface alternatives as suggestions in the wiring error
    for (const FOliveWiringAlternative& Alt : Diag.Alternatives)
    {
        Result.Suggestions.Add(FString::Printf(TEXT("[%s] %s: %s"),
            *Alt.Confidence, *Alt.Label, *Alt.Action));
    }
}
else
{
    // Fallback to bare error string (should not happen after autocast integration)
    Result.ErrorMessage = FString::Printf(
        TEXT("Pin connection failed (%s.%s -> %s.%s): %s"),
        *SourceStepId, *SourcePin->PinName,
        *TargetStepId, *TargetPin->PinName,
        ConnectResult.Errors.Num() > 0 ? *ConnectResult.Errors[0] : TEXT("Unknown"));
}
```

The wiring error that accumulates in `Context.WiringErrors` gets the alternatives appended to the `Suggestion` field:

```cpp
Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
    TEXT("DATA_WIRE_INCOMPATIBLE"),   // NEW: more specific error code
    Step.StepId,
    FString::Printf(TEXT("/steps/inputs/%s"), *PinKey),
    Result.ErrorMessage,
    Result.Suggestions.Num() > 0
        ? FString::Join(Result.Suggestions, TEXT("\n"))
        : TEXT("")));
```

**New error code:** `DATA_WIRE_INCOMPATIBLE` replaces `DATA_PIN_NOT_FOUND` for cases where the pins are found but types are incompatible. `DATA_PIN_NOT_FOUND` is still used when the pin name itself cannot be resolved.

### 5. Integration Point: connect_pins Tool Handler

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`, executor lambda around line 4778.

**Current behavior:**

```cpp
if (!WriteResult.bSuccess)
{
    FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
    return FOliveWriteResult::ExecutionError(
        TEXT("BP_CONNECT_PINS_FAILED"),
        ErrorMsg,
        TEXT("Verify the pin references are valid and compatible")
    );
}
```

**New behavior:**

```cpp
if (!WriteResult.bSuccess)
{
    FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");

    // Check for structured diagnostic
    if (WriteResult.WiringDiagnostic.IsSet())
    {
        const FOliveWiringDiagnostic& Diag = WriteResult.WiringDiagnostic.GetValue();

        // Build rich error result with diagnostic JSON
        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("source_type"), Diag.SourceTypeName);
        ErrorData->SetStringField(TEXT("target_type"), Diag.TargetTypeName);
        ErrorData->SetStringField(TEXT("source_pin"), Diag.SourcePinName);
        ErrorData->SetStringField(TEXT("target_pin"), Diag.TargetPinName);
        ErrorData->SetStringField(TEXT("failure_reason"),
            StaticEnum<EOliveWiringFailureReason>()->GetNameStringByValue(
                static_cast<int64>(Diag.Reason)));
        ErrorData->SetStringField(TEXT("why_autofix_failed"), Diag.WhyAutoFixFailed);

        TArray<TSharedPtr<FJsonValue>> AltArray;
        for (const FOliveWiringAlternative& Alt : Diag.Alternatives)
        {
            TSharedPtr<FJsonObject> AltObj = MakeShared<FJsonObject>();
            AltObj->SetStringField(TEXT("label"), Alt.Label);
            AltObj->SetStringField(TEXT("action"), Alt.Action);
            AltObj->SetStringField(TEXT("confidence"), Alt.Confidence);
            AltArray.Add(MakeShared<FJsonValueObject>(AltObj));
        }
        ErrorData->SetArrayField(TEXT("alternatives"), AltArray);

        FOliveWriteResult ErrResult = FOliveWriteResult::ExecutionError(
            TEXT("BP_CONNECT_PINS_INCOMPATIBLE"),
            Diag.ToHumanReadable(),
            Diag.Alternatives.Num() > 0
                ? Diag.Alternatives[0].Action
                : TEXT("Check pin types and plan logic"));
        ErrResult.ResultData = ErrorData;
        return ErrResult;
    }

    // Fallback for non-type errors (pin not found, etc.)
    return FOliveWriteResult::ExecutionError(
        TEXT("BP_CONNECT_PINS_FAILED"),
        ErrorMsg,
        TEXT("Call blueprint.get_node_pins on both nodes to verify pin names and types"));
}
```

**New error code:** `BP_CONNECT_PINS_INCOMPATIBLE` (type mismatch, structured diagnostic available) vs existing `BP_CONNECT_PINS_FAILED` (pin not found, other errors).

### 6. Integration Point: Self-Correction Policy

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`, `BuildToolErrorMessage` method.

**Current behavior** (lines 728-742): Generic guidance for `BP_CONNECT_PINS_FAILED` that says "call blueprint.read with include_pins:true."

**New behavior** -- add a block for the new error code and enhance the existing one:

```cpp
else if (ErrorCode == TEXT("BP_CONNECT_PINS_INCOMPATIBLE"))
{
    Guidance = TEXT("Pin types are incompatible and no automatic conversion exists. "
        "The error response includes 'alternatives' with specific fixes ordered by confidence. "
        "Try the first 'high' confidence alternative. "
        "Common patterns:\n"
        "- Struct -> Scalar (e.g., Vector -> Float): use break_struct op or ~PinName_X suffix\n"
        "- Scalar -> Struct (e.g., Float -> Vector): use make_struct op or Conv_ call\n"
        "- Object type mismatch: add a cast step\n"
        "- If all alternatives fail, use editor.run_python");
}
else if (ErrorCode == TEXT("BP_CONNECT_PINS_FAILED"))
{
    // Keep existing guidance (pin not found, wrong format, etc.)
    Guidance = TEXT("Pin connection failed. ...");  // existing text unchanged
}
```

Also add `BP_CONNECT_PINS_INCOMPATIBLE` to ClassifyErrorCode:

```cpp
// Category A: FixableMistake (default) -- the diagnostic already tells the AI exactly
// what to do, so retrying with the suggested alternative is likely to succeed.
```

No change needed since Category A is the default. The existing fallthrough handles it.

**Additionally,** update `DATA_WIRE_INCOMPATIBLE` guidance in the plan-specific error path:

```cpp
else if (ErrorCode == TEXT("DATA_WIRE_INCOMPATIBLE"))
{
    Guidance = TEXT("Two pins in the plan have incompatible types and no autocast exists. "
        "The wiring_errors array contains specific alternatives. "
        "Common fix: add a break_struct or make_struct intermediate step, "
        "or change the input to use a ~suffix for a split sub-pin component "
        "(e.g., '@get_loc.~ReturnValue_X' for Vector.X).");
}
```

### 7. Error Code Summary

| Code | When | Category |
|------|------|----------|
| `DATA_PIN_NOT_FOUND` | Pin name resolution failed (no matching pin on manifest) | A (existing) |
| `DATA_WIRE_INCOMPATIBLE` | Pins found but types incompatible after autocast+split attempts | A (new) |
| `BP_CONNECT_PINS_FAILED` | connect_pins tool: pin not found, format error, other | A (existing) |
| `BP_CONNECT_PINS_INCOMPATIBLE` | connect_pins tool: types incompatible with structured diagnostic | A (new) |

### 8. JSON Output Examples

#### Example 1: Vector -> Float (plan_json wiring error)

**Current:**
```json
{
    "error_code": "DATA_PIN_NOT_FOUND",
    "step_id": "set_speed",
    "location": "/steps/inputs/NewSpeed",
    "message": "Pin connection failed (get_vel.ReturnValue -> set_speed.NewSpeed): Cannot connect pins: ...",
    "suggestion": "Available pins: NewSpeed (Float)"
}
```

**Proposed:**
```json
{
    "error_code": "DATA_WIRE_INCOMPATIBLE",
    "step_id": "set_speed",
    "location": "/steps/inputs/NewSpeed",
    "message": "Pin connection failed (get_vel.ReturnValue [Vector] -> set_speed.NewSpeed [Float]): Types Vector and Float are not directly compatible. No autocast function exists for this pair.",
    "suggestion": "[high] Use break_struct op: Add a break_struct step for the Vector, then wire @break_step.X (or Y/Z) to the Float input.\n[high] Use ~suffix: Change input to '@get_vel.~ReturnValue_X' to target the X sub-component via fuzzy split-pin match.\n[medium] Use get_node_pins + connect_pins: Call blueprint.get_node_pins on the source node to see split sub-pin names, then use connect_pins."
}
```

#### Example 2: Actor -> Character (connect_pins tool)

**Current:**
```json
{
    "success": false,
    "error": {
        "code": "BP_CONNECT_PINS_FAILED",
        "message": "Cannot connect pins: ...",
        "suggestion": "Verify the pin references are valid and compatible"
    }
}
```

**Proposed:**
```json
{
    "success": false,
    "error": {
        "code": "BP_CONNECT_PINS_INCOMPATIBLE",
        "message": "Cannot connect Actor Object Reference output to Character Object Reference input. Object downcast required.",
        "suggestion": "Add a cast step: {\"op\":\"cast\",\"target\":\"Character\",\"inputs\":{\"Object\":\"@source_step\"}}. Wire the cast output to the target input."
    },
    "data": {
        "source_type": "Actor Object Reference",
        "target_type": "Character Object Reference",
        "source_pin": "ReturnValue",
        "target_pin": "InActor",
        "failure_reason": "ObjectCastRequired",
        "why_autofix_failed": "Object types are not in a parent-child relationship in the required direction. Actor is a parent of Character, but the pin expects the more derived type.",
        "alternatives": [
            {
                "label": "Add a cast step",
                "action": "Add a cast step in plan_json: {\"op\":\"cast\",\"target\":\"Character\",\"inputs\":{\"Object\":\"@source_step\"}}. Wire the cast output to the target input.",
                "confidence": "high"
            },
            {
                "label": "Use cast node via add_node",
                "action": "blueprint.add_node type:\"Cast\" properties:{\"TargetType\":\"Character\"}, then connect_pins source -> Cast Object input, Cast output -> target.",
                "confidence": "medium"
            }
        ]
    }
}
```

#### Example 3: Completely incompatible types (Exec -> Float)

**Proposed:**
```json
{
    "success": false,
    "error": {
        "code": "BP_CONNECT_PINS_INCOMPATIBLE",
        "message": "Cannot connect Exec output to Float input. These pin categories are fundamentally incompatible.",
        "suggestion": "Exec pins carry execution flow, not data. You likely meant to connect a data output pin instead. Call blueprint.get_node_pins to see all available pins on the source node."
    },
    "data": {
        "source_type": "Exec",
        "target_type": "Float",
        "failure_reason": "TypesIncompatible",
        "alternatives": [
            {
                "label": "Check your pin selection",
                "action": "You are connecting an execution flow pin to a data pin. Use blueprint.get_node_pins on the source node to find the correct data output pin.",
                "confidence": "high"
            }
        ]
    }
}
```

---

## File Structure

### New files: None

All changes go in existing files.

### Modified files:

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Public/Writer/OlivePinConnector.h` | Add `EOliveWiringFailureReason`, `FOliveWiringAlternative`, `FOliveWiringDiagnostic` structs. Add `BuildWiringDiagnostic()` and `SuggestAlternatives()` private methods. |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OlivePinConnector.cpp` | Implement `BuildWiringDiagnostic()`, `SuggestAlternatives()`, `ToJson()`, `ToHumanReadable()`. Update `Connect()` failure path. |
| `Source/OliveAIEditor/Blueprint/Public/Writer/OliveBlueprintWriter.h` | Add `TOptional<FOliveWiringDiagnostic> WiringDiagnostic` to `FOliveBlueprintWriteResult`. Requires forward-declare or include of `OlivePinConnector.h`. |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` | Update `WireDataConnection()` failure path (around line 2604) to extract diagnostic and surface alternatives in wiring errors. |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Update connect_pins executor lambda to emit `BP_CONNECT_PINS_INCOMPATIBLE` with diagnostic JSON. |
| `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | Add guidance blocks for `BP_CONNECT_PINS_INCOMPATIBLE` and `DATA_WIRE_INCOMPATIBLE` in `BuildToolErrorMessage()`. |

### Include dependency note:

`FOliveBlueprintWriteResult` is defined in `OliveBlueprintWriter.h`. Adding `TOptional<FOliveWiringDiagnostic>` requires `FOliveWiringDiagnostic` to be forward-declarable or the type must be fully defined. Since `TOptional` requires a complete type, `OliveBlueprintWriter.h` must include `OlivePinConnector.h`. This is acceptable because `OliveBlueprintWriter.h` already includes `OlivePinConnector.h` indirectly (OlivePinConnector.h includes OliveBlueprintWriter.h currently).

**Wait -- circular dependency.** `OlivePinConnector.h` currently includes `OliveBlueprintWriter.h`. Adding the reverse include would create a cycle.

**Resolution:** Extract the diagnostic structs into a new header:

| File | Contents |
|------|----------|
| `Source/OliveAIEditor/Blueprint/Public/Writer/OliveWiringDiagnostic.h` | `EOliveWiringFailureReason`, `FOliveWiringAlternative`, `FOliveWiringDiagnostic` |

This breaks the cycle: `OliveBlueprintWriter.h` includes `OliveWiringDiagnostic.h` (lightweight, no Blueprint includes), `OlivePinConnector.h` includes `OliveBlueprintWriter.h` (as before).

So there IS one new header file:

| New File | Path |
|----------|------|
| Header | `Source/OliveAIEditor/Blueprint/Public/Writer/OliveWiringDiagnostic.h` |
| Implementation (ToJson/ToHumanReadable) | Inlined in `OlivePinConnector.cpp` or a small `OliveWiringDiagnostic.cpp` |

Given the implementation is small (~50 lines for ToJson + ToHumanReadable), putting it in `OlivePinConnector.cpp` is fine. No separate .cpp file needed.

---

## Implementation Order

1. **T1: Create `OliveWiringDiagnostic.h`** (15 min)
   - Define the enum, structs, and forward declarations
   - Pure data types, no UE API calls

2. **T2: Add `WiringDiagnostic` to `FOliveBlueprintWriteResult`** (5 min)
   - Add the `TOptional<FOliveWiringDiagnostic>` field
   - Add `#include "OliveWiringDiagnostic.h"` to `OliveBlueprintWriter.h`

3. **T3: Implement `BuildWiringDiagnostic` + `SuggestAlternatives`** (45 min)
   - This is the bulk of the work: categorizing failures and building alternatives
   - Add private methods to `OlivePinConnector.h`
   - Implement in `OlivePinConnector.cpp`
   - Implement `ToJson()` and `ToHumanReadable()` on `FOliveWiringDiagnostic`
   - **Must be done AFTER the autocast integration** -- the method sits in the `CONNECT_RESPONSE_DISALLOW` branch that only executes when TryCreateConnection fails

4. **T4: Wire diagnostic into PlanExecutor Phase 4** (20 min)
   - Update `WireDataConnection()` to extract diagnostic from `ConnectResult.WiringDiagnostic`
   - Use new `DATA_WIRE_INCOMPATIBLE` error code for type mismatches
   - Pass alternatives through to wiring error suggestions

5. **T5: Wire diagnostic into connect_pins handler** (20 min)
   - Update the executor lambda to check `WriteResult.WiringDiagnostic`
   - Emit `BP_CONNECT_PINS_INCOMPATIBLE` with diagnostic JSON in result data

6. **T6: Update self-correction policy** (10 min)
   - Add guidance for `BP_CONNECT_PINS_INCOMPATIBLE` and `DATA_WIRE_INCOMPATIBLE`
   - Verify ClassifyErrorCode defaults to Category A (no change needed)

**Total estimated time:** ~2 hours

**Dependency:** T3 depends on the autocast integration being complete (the `Connect()` method must already use `TryCreateConnection`). T4 and T5 depend on T2 and T3. T6 is independent of T4/T5.

---

## Edge Cases

### 1. Pin not found (pre-existing error path)
The diagnostic only fires when both pins are successfully resolved but types are incompatible. Pin-not-found errors continue to use the existing `DATA_PIN_NOT_FOUND` / `BP_CONNECT_PINS_FAILED` codes unchanged.

### 2. Direction mismatch
When both pins are outputs (or both inputs), UE's `CanCreateConnection` returns DISALLOW. The diagnostic catches this with `DirectionMismatch` reason and suggests swapping source/target.

### 3. Already-connected single-connection pins
Some input pins (e.g., exec inputs) allow only one connection. If the pin is already connected, DISALLOW is returned. The diagnostic reports `AlreadyConnected` and suggests disconnecting the existing connection first.

### 4. Struct types that cannot be split
Some struct pins have `MD_NativeDisableSplitPin` metadata. The diagnostic checks `CanSplitStructPin` and only suggests split-based alternatives when splitting is available. When not splittable, the `WhyAutoFixFailed` field explains this.

### 5. Custom struct types (not Vector/Rotator/etc.)
For user-defined structs, the sub-pin name prediction is not hardcoded. The diagnostic says "Use break_struct op to decompose" without listing specific sub-pin names. The AI must then read the pin manifest after decomposition.

### 6. Wildcard pins
Wildcard pins (`PC_Wildcard`) accept any type and should never reach the diagnostic path (TryCreateConnection handles them). If they do, the diagnostic falls through to `TypesIncompatible` with a note that this is unexpected.

### 7. Array/Set/Map container mismatches
The diagnostic detects when only the container type differs (e.g., `Array<Actor>` vs `Actor`). It suggests `MakeArray` for single-to-array and `Get`/`Array_Get` for array-to-single.

### 8. Multiple errors in a single plan
Each wiring error gets its own diagnostic independently. The `WiringErrors` array in `FOlivePlanExecutionContext` can contain multiple `DATA_WIRE_INCOMPATIBLE` entries, each with its own alternatives. The AI sees all of them in the plan result.

---

## Composition with Autocast and SplitPin Features

### Execution order in Connect():

```
1. TryCreateConnection()              <-- autocast integration handles Conv_* here
   |
   +-- success? -> return Success
   |
   +-- failure? -> CanCreateConnection() to get response type
       |
       +-- MAKE_WITH_CONVERSION_NODE? -> should not happen (TryCreateConnection handles it)
       |   (defensive: retry TryCreateConnection, then fall through)
       |
       +-- DISALLOW? -> SplitPin auto-detection
           |
           +-- struct output + scalar input + splittable? -> split + connect sub-pin
           |   |
           |   +-- success? -> return Success (with ConversionNote about split)
           |   |
           |   +-- failure? -> BuildWiringDiagnostic()   <-- THIS DESIGN
           |
           +-- not splittable? -> BuildWiringDiagnostic()   <-- THIS DESIGN
```

The diagnostic is the LAST resort. It only fires when:
1. Direct connection failed
2. Autocast (TryCreateConnection) could not find a Conv_* function
3. SplitPin auto-detection either was not applicable or failed

This ensures the diagnostic never fires for cases that CAN be auto-fixed, and always fires for cases that CANNOT.

---

## Coder Notes

1. **Do NOT modify the existing `GetConversionOptions()` or `InsertConversionNode()` code as part of this task.** Those methods are being replaced by the autocast integration. This design adds NEW code paths, it does not modify the old ones.

2. **The `FOliveWiringDiagnostic::ToHumanReadable()` format matters.** It is what appears in log files and in the `Errors` array of `FOliveBlueprintWriteResult`. Format:
   ```
   Cannot connect {SourceType} to {TargetType}: {Reason}. {WhyAutoFixFailed}
   Alternatives:
   - [high] {Label}: {Action}
   - [medium] {Label}: {Action}
   ```

3. **The `SuggestAlternatives` method should NOT call any UE graph mutation APIs.** It is a pure diagnostic function. It may call `Schema->CanSplitStructPin()` and `Schema->SearchForAutocastFunction()` for probing, but must not modify any graph state.

4. **Thread safety:** All diagnostic code runs on the game thread (same thread as `Connect()`). No additional synchronization needed.

5. **The `EOliveWiringFailureReason` enum does NOT need `UENUM` macro** since it is only used in C++ structs, not in UPROPERTIES. If the coder wants to use `StaticEnum<>()` for string conversion in the tool handler, they should add the `UENUM()` macro. Otherwise, a simple switch-case string conversion is fine.

6. **Include `EdGraph/EdGraphSchema.h`** in `OlivePinConnector.cpp` for the `ECanCreateConnectionResponse` enum values (CONNECT_RESPONSE_DISALLOW, etc.). This header is lightweight and safe.
