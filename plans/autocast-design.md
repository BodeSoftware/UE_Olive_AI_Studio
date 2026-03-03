# Autocast / Auto-Conversion Integration Design

## Status: Draft
## Author: Architect Agent
## Date: 2026-03-03
## Depends on: plans/research/autocast-api.md

---

## Problem Statement

`FOlivePinConnector::Connect()` has a critical bug: it checks `CanSafeConnect()` to determine if two pins can wire, but `CanSafeConnect()` returns `false` for `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE`. This means **every autocast opportunity (Int->Double, String->Name, Object->Interface, etc.) silently fails**. The fallback path calls `CreateConversionNode()` which is a stub that always returns `nullptr`.

Additionally, there is no SplitPin integration for the common case of wiring a Vector output to a Float input (accessing X, Y, or Z components). This forces the AI to manually insert `break_struct` steps, adding complexity to plans.

---

## Scope

### In Scope
1. **Fix OlivePinConnector::Connect** -- Replace broken custom conversion path with UE's `TryCreateConnection`
2. **Delete dead code** -- Remove 4 methods that poorly reimplement engine logic
3. **Add SplitPin fallback in PlanExecutor** -- When data wiring fails and the source is a splittable struct, auto-split and connect the appropriate sub-pin
4. **Track both autocast and SplitPin in FOliveConversionNote** -- Unified reporting
5. **Enable autocast for `connect_pins` tool** -- The granular tool should also benefit

### Out of Scope
- Nested struct splits (e.g., splitting a Transform's Location sub-pin further into X/Y/Z) -- deferred
- User-facing SplitPin tool (manual pin splitting from AI) -- separate feature
- Changes to plan_json ops vocabulary -- the AI can still use `break_struct` explicitly

---

## Design

### Task 1: Refactor OlivePinConnector::Connect (Critical Bug Fix)

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OlivePinConnector.cpp`

**Current flow (broken):**
```
CanCreateConnection() -> CanSafeConnect() -> if true: TryCreateConnection
                                          -> if false + bAllowConversion: GetConversionOptions -> InsertConversionNode -> CreateConversionNode (returns nullptr!)
                                          -> if false + !bAllowConversion: return error
```

**New flow:**
```
CanCreateConnection() -> classify response
  DISALLOW:                          -> return error (incompatible types)
  MAKE_WITH_CONVERSION_NODE:
    !bAllowConversion?               -> return error ("conversion needed but not allowed")
    bAllowConversion?                -> record pre-state, TryCreateConnection, detect conversion node
  MAKE / BREAK_OTHERS / PROMOTION:   -> TryCreateConnection (direct wire)
```

**Changes to `Connect()`:**

Replace lines 62-127 (the `CanSafeConnect()` branch + `bAllowConversion` branch) with:

```cpp
FOliveBlueprintWriteResult FOlivePinConnector::Connect(
    UEdGraphPin* SourcePin,
    UEdGraphPin* TargetPin,
    bool bAllowConversion)
{
    // ... [existing pin validation stays, lines 38-60] ...

    const UEdGraphSchema_K2* K2Schema = GetK2Schema();
    if (!K2Schema) { /* ... existing error ... */ }

    FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);

    // Classify the response
    const bool bNeedsConversion =
        (Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE);
    const bool bCanConnect =
        Response.CanSafeConnect() || bNeedsConversion;

    if (!bCanConnect)
    {
        return FOliveBlueprintWriteResult::Error(
            FString::Printf(TEXT("Cannot connect pins: %s"), *Response.Message.ToString()),
            AssetPath);
    }

    if (bNeedsConversion && !bAllowConversion)
    {
        return FOliveBlueprintWriteResult::Error(
            FString::Printf(TEXT("Type conversion needed (%s -> %s) but not allowed. "
                "Pass bAllowConversion=true or use compatible types."),
                *GetPinTypeDescription(SourcePin->PinType),
                *GetPinTypeDescription(TargetPin->PinType)),
            AssetPath);
    }

    // Record pre-state for conversion detection
    // After TryCreateConnection with MAKE_WITH_CONVERSION_NODE, SourcePin->LinkedTo[0]
    // will be the conversion node's input, NOT TargetPin.
    const int32 SourceLinkCountBefore = SourcePin->LinkedTo.Num();

    UBlueprint* Blueprint = GetOwningBlueprint(SourcePin);

    OLIVE_SCOPED_TRANSACTION(FText::Format(
        NSLOCTEXT("OlivePinConnector", "ConnectPins", "Connect Pins: {0} -> {1}"),
        FText::FromString(SourcePin->GetName()),
        FText::FromString(TargetPin->GetName())));

    if (Blueprint) { Blueprint->Modify(); }

    // TryCreateConnection handles everything: direct wires, promotions,
    // AND conversion node insertion (for MAKE_WITH_CONVERSION_NODE).
    bool bSuccess = K2Schema->TryCreateConnection(SourcePin, TargetPin);

    if (bSuccess)
    {
        if (Blueprint) { FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint); }

        UE_LOG(LogOlivePinConnector, Log, TEXT("Connected pins: %s -> %s%s"),
            *SourcePin->GetName(), *TargetPin->GetName(),
            bNeedsConversion ? TEXT(" (with conversion)") : TEXT(""));

        FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath);

        // Flag that conversion was inserted (caller can detect the intermediate node)
        if (bNeedsConversion)
        {
            Result.AddWarning(FString::Printf(
                TEXT("Auto-conversion inserted: %s -> %s"),
                *GetPinTypeDescription(SourcePin->PinType),
                *GetPinTypeDescription(TargetPin->PinType)));
        }

        return Result;
    }
    else
    {
        return FOliveBlueprintWriteResult::Error(
            TEXT("TryCreateConnection returned false"), AssetPath);
    }
}
```

**Key behavioral changes:**
- `bAllowConversion=false` callers (GraphWriter::ConnectPins, exec wiring) are **unaffected** -- they still get an error if conversion would be needed. No change in behavior.
- `bAllowConversion=true` callers (WireDataConnection, auto-wire dispatcher) now **actually get autocast working** -- `TryCreateConnection` creates and wires the conversion node automatically.
- The `OLIVE_SCOPED_TRANSACTION` stays in the same position; when called from within `FOliveBatchExecutionScope`, the macro is a no-op (existing behavior).

### Task 2: Delete Dead Code from OlivePinConnector

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OlivePinConnector.cpp`

Remove these method **implementations** entirely:
- `GetConversionOptions()` (lines 170-281) -- ad-hoc reimplementation of engine logic
- `InsertConversionNode()` (lines 283-379) -- uses the broken `CreateConversionNode()`
- `CreateConversionNode()` (lines 577-615) -- always returns nullptr
- `CanAutoConvert()` (lines 519-575) -- ad-hoc reimplementation

**File:** `Source/OliveAIEditor/Blueprint/Public/Writer/OlivePinConnector.h`

Remove from the **public interface**:
- `GetConversionOptions()` declaration (lines 83-86)
- `InsertConversionNode()` declaration (lines 95-99)

Remove from the **private section**:
- `CanAutoConvert()` declaration (lines 171-173)
- `CreateConversionNode()` declaration (lines 184-189)

**Keep:**
- `GetPinTypeDescription()` -- still used for error messages in the new `Connect()`
- `ValidatePin()`, `GetOwningBlueprint()`, `GetAssetPathForPin()`, `GetK2Schema()` -- still used
- `CanConnect()` -- still needed by external callers

### Task 3: Fix CanConnect to Detect Autocast-Compatible Pins

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OlivePinConnector.cpp`

The current `CanConnect()` also uses `CanSafeConnect()` and therefore returns `false` for autocast-compatible pairs. Fix it:

```cpp
bool FOlivePinConnector::CanConnect(
    const UEdGraphPin* SourcePin,
    const UEdGraphPin* TargetPin,
    FString& OutReason) const
{
    // ... [existing pin validation stays] ...

    const UEdGraphSchema_K2* K2Schema = GetK2Schema();
    if (!K2Schema) { /* ... existing ... */ }

    FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);

    if (Response.CanSafeConnect() ||
        Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE)
    {
        return true;
    }
    else
    {
        OutReason = Response.Message.ToString();
        return false;
    }
}
```

This is a minor change but ensures `CanConnect` reports accurately.

### Task 4: Enable Autocast for `connect_pins` Tool

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp`

Line 540 currently passes `bAllowConversion=false`:
```cpp
FOliveBlueprintWriteResult Result = Connector.Connect(SourcePin, TargetPin, /*bAllowConversion=*/false);
```

Change to `true`:
```cpp
FOliveBlueprintWriteResult Result = Connector.Connect(SourcePin, TargetPin, /*bAllowConversion=*/true);
```

**Rationale:** When the AI uses the granular `connect_pins` tool, it has already decided these pins should be connected. Refusing to autocast forces the AI to manually insert conversion nodes, which is busywork. The Blueprint editor autoconnects with conversion when the user drags wires -- our tool should behave the same way.

### Task 5: SplitPin Fallback in WireDataConnection

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

When `Connector.Connect()` fails in `WireDataConnection()`, and the source pin is a splittable struct while the target pin is a compatible scalar, automatically split the source pin and connect the appropriate sub-pin.

**Location:** After line 2541 (`Connector.Connect` call), in the failure branch (currently lines 2604-2611).

**New logic inserted before the existing failure branch:**

```cpp
if (!ConnectResult.bSuccess)
{
    // --------------------------------------------------------
    // SplitPin fallback: Struct output -> Scalar input
    // When a struct output (Vector, Rotator, etc.) fails to
    // connect to a scalar input (Float, Double), split the
    // struct pin and connect the appropriate sub-pin.
    // --------------------------------------------------------
    bool bSplitPinRecovery = false;

    if (RealSourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
        && RealSourcePin->SubPins.Num() == 0  // Not already split
        && !RealSourcePin->bHidden)            // Not hidden
    {
        const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
        if (K2Schema->CanSplitStructPin(*RealSourcePin))
        {
            // Determine which sub-pin to target
            FString SubPinSuffix = ResolveSubPinSuffix(
                SourcePinHint, RealSourcePin, RealTargetPin);

            if (!SubPinSuffix.IsEmpty())
            {
                // Perform the split
                K2Schema->SplitPin(RealSourcePin, /*bNotify=*/true);

                // Find the sub-pin
                FString SubPinName = FString::Printf(TEXT("%s_%s"),
                    *RealSourcePin->PinName.ToString(), *SubPinSuffix);

                UEdGraphPin* SubPin = nullptr;
                for (UEdGraphPin* SP : RealSourcePin->SubPins)
                {
                    if (SP && SP->PinName.ToString().Equals(SubPinName, ESearchCase::IgnoreCase))
                    {
                        SubPin = SP;
                        break;
                    }
                }

                if (SubPin)
                {
                    // Re-attempt connection with the sub-pin
                    FOliveBlueprintWriteResult SplitConnectResult =
                        Connector.Connect(SubPin, RealTargetPin, /*bAllowConversion=*/true);

                    if (SplitConnectResult.bSuccess)
                    {
                        bSplitPinRecovery = true;
                        Result.bSuccess = true;
                        Result.ResolvedSourcePin = SubPin->PinName.ToString();
                        Result.ResolvedTargetPin = TargetPin->PinName;
                        Result.SourceMatchMethod = SourceMatchMethod + TEXT("_split_pin");
                        Result.TargetMatchMethod = TargetMatchMethod;

                        // Log conversion note for SplitPin
                        FOliveConversionNote Note;
                        Note.SourceStep = SourceStepId;
                        Note.TargetStep = TargetStepId;
                        Note.SourcePinName = SourcePin->PinName;
                        Note.TargetPinName = TargetPin->PinName;
                        Note.FromType = SourcePin->TypeDisplayString;
                        Note.ToType = TargetPin->TypeDisplayString;
                        Note.ConversionNodeType = FString::Printf(
                            TEXT("SplitPin(%s)"), *SubPinSuffix);

                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("SplitPin recovery: %s.%s -> %s.%s via sub-pin %s"),
                            *SourceStepId, *SourcePin->PinName,
                            *TargetStepId, *TargetPin->PinName,
                            *SubPinName);

                        Context.Warnings.Add(FString::Printf(
                            TEXT("SplitPin: connected %s.%s (sub-pin %s) -> %s.%s"),
                            *SourceStepId, *SourcePin->PinName,
                            *SubPinSuffix,
                            *TargetStepId, *TargetPin->PinName));

                        Context.ConversionNotes.Add(MoveTemp(Note));
                    }
                }
            }
        }
    }

    if (!bSplitPinRecovery)
    {
        // Original failure path
        Result.ErrorMessage = FString::Printf(
            TEXT("Pin connection failed (%s.%s -> %s.%s): %s"),
            *SourceStepId, *SourcePin->PinName,
            *TargetStepId, *TargetPin->PinName,
            ConnectResult.Errors.Num() > 0 ? *ConnectResult.Errors[0] : TEXT("Unknown"));
    }
}
```

### Task 6: ResolveSubPinSuffix Helper

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` (anonymous namespace or private method)

**Signature:**
```cpp
/**
 * Determine which sub-pin suffix to use when splitting a struct pin.
 * Checks for explicit hint from AI (e.g., @step.~Location_X -> "X"),
 * then falls back to smart defaults based on target pin name or type.
 *
 * @param SourcePinHint The AI's original pin hint (may contain sub-pin suffix)
 * @param StructPin The struct output pin that will be split
 * @param TargetPin The scalar input pin we're trying to connect to
 * @return Sub-pin suffix (e.g., "X", "Y", "Z", "Roll", "Pitch", "Yaw") or empty if undetermined
 */
static FString ResolveSubPinSuffix(
    const FString& SourcePinHint,
    const UEdGraphPin* StructPin,
    const UEdGraphPin* TargetPin);
```

**Resolution order:**

1. **Explicit hint from AI:** If `SourcePinHint` contains an underscore suffix that matches a known component name, use it.
   - Example: `SourcePinHint = "~Location_X"` -> strip the `~` and the base pin name -> suffix = `"X"`
   - Parse: if the hint contains `_X`, `_Y`, `_Z`, `_R`, `_G`, `_B`, `_A`, `_Roll`, `_Pitch`, `_Yaw`, extract that suffix.

2. **Target pin name match:** If the target pin's name matches a known component (case-insensitive), use it.
   - Example: target pin named `"X"` or `"Pitch"` -> use that suffix directly
   - Covers the common case where the AI wires `@get_loc.auto` to a pin literally named `X`

3. **Target pin name as partial match:** Check if target pin name ends with a component suffix.
   - Example: target pin `"TargetX"` -> suffix `"X"`

4. **Default first component:** If the struct is a known type, use the first component.
   - `Vector` / `Vector2D` / `IntVector` / `IntPoint` -> `"X"`
   - `Rotator` -> `"Roll"`
   - `LinearColor` -> `"R"`

5. **Truly unknown struct:** Return empty string (no split attempted). The original error propagates.

**Known struct component maps (hardcoded, engine-stable):**

| Struct | Components |
|--------|-----------|
| `Vector` | X, Y, Z |
| `Vector2D` | X, Y |
| `Vector4` | X, Y, Z, W |
| `IntVector` | X, Y, Z |
| `IntPoint` | X, Y |
| `Rotator` | Roll, Pitch, Yaw |
| `LinearColor` | R, G, B, A |
| `Color` | R, G, B, A |

**Implementation note:** Get struct name from `StructPin->PinType.PinSubCategoryObject->GetName()`. The object is a `UScriptStruct*`.

### Task 7: Update Conversion Note Detection in WireDataConnection

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

The existing conversion detection logic at lines 2561-2595 uses a heuristic:
```cpp
const bool bConversionInserted = (RealSourcePin->LinkedTo.Num() > 0 &&
    RealSourcePin->LinkedTo[0] != RealTargetPin);
```

This heuristic is correct and **still works** with the new `TryCreateConnection`-based `Connect()`. The engine's `CreateAutomaticConversionNodeAndConnections` wires `SourcePin -> ConversionNode -> TargetPin`, so `SourcePin->LinkedTo[0]` is the conversion node's input pin, not `TargetPin`.

**No change needed to the existing detection logic.** It works with the new PinConnector because the wiring topology is identical -- the engine does the same thing the old code tried (and failed) to do.

The only addition is the SplitPin path (Task 5), which creates its own `FOliveConversionNote` with `ConversionNodeType = "SplitPin(X)"` etc. This is handled within the SplitPin fallback block itself.

### Task 8: Add Include for SplitPin

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

No new includes needed. `EdGraphSchema_K2.h` (already included at line 28) provides:
- `UEdGraphSchema_K2::CanSplitStructPin()`
- `UEdGraphSchema_K2::SplitPin()`
- `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE` (from `EdGraph/EdGraphSchema.h`, included transitively)

`EdGraph/EdGraphPin.h` (already included at line 27) provides:
- `UEdGraphPin::SubPins`
- `UEdGraphPin::ParentPin`

---

## Data Flow

### Autocast Path (Task 1)

```
AI plan: { inputs: { "A": "@get_health.ReturnValue" } }   [Int32 -> Double]
    |
    v
PlanExecutor::WireDataConnection()
    |-- resolves source/target pins from manifests
    |-- calls Connector.Connect(IntPin, DoublePin, bAllowConversion=true)
    |
    v
PinConnector::Connect()
    |-- CanCreateConnection() returns CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE
    |-- bNeedsConversion=true, bAllowConversion=true -> proceed
    |-- K2Schema->TryCreateConnection(IntPin, DoublePin)
    |     |-- internally: CreateAutomaticConversionNodeAndConnections()
    |     |-- spawns UK2Node_CallFunction(Conv_IntToDouble)
    |     |-- wires IntPin -> Conv.Input, Conv.ReturnValue -> DoublePin
    |     |-- returns true
    |-- returns Success with warning "Auto-conversion inserted"
    |
    v
PlanExecutor::WireDataConnection() (back in caller)
    |-- bConversionInserted = (IntPin->LinkedTo[0] != DoublePin) = true
    |-- creates FOliveConversionNote { ConversionNodeType: "K2Node_CallFunction" }
    |-- appends to Context.ConversionNotes
```

### SplitPin Path (Task 5)

```
AI plan: { inputs: { "NewX": "@get_loc.auto" } }   [Vector -> Float, pin named "NewX"]
    |
    v
PlanExecutor::WireDataConnection()
    |-- auto-match finds source: "ReturnValue" (Vector type)
    |-- auto-match finds target: "NewX" (Float type)
    |-- calls Connector.Connect(VectorPin, FloatPin, bAllowConversion=true)
    |
    v
PinConnector::Connect()
    |-- CanCreateConnection() returns CONNECT_RESPONSE_DISALLOW (no autocast for Vector->Float)
    |-- returns Error("Cannot connect pins")
    |
    v
PlanExecutor::WireDataConnection() (back in caller, ConnectResult.bSuccess=false)
    |-- SplitPin fallback check:
    |     source is PC_Struct? yes
    |     CanSplitStructPin? yes
    |     ResolveSubPinSuffix("auto", VectorPin, FloatPin named "NewX"):
    |       hint has no explicit suffix -> check target name "NewX" -> ends with "X" -> suffix="X"
    |-- K2Schema->SplitPin(VectorPin) -> creates ReturnValue_X, ReturnValue_Y, ReturnValue_Z
    |-- finds ReturnValue_X sub-pin
    |-- Connector.Connect(ReturnValue_X, FloatPin, true) -> success (Float->Float, direct)
    |-- creates FOliveConversionNote { ConversionNodeType: "SplitPin(X)" }
```

---

## Edge Cases

### 1. Already-Split Pins
**Guard:** `RealSourcePin->SubPins.Num() == 0` check prevents double-splitting. If the pin is already split (e.g., by a previous connection in the same plan), we skip the split attempt and fall through to the normal error. The sub-pins already exist and could theoretically be found, but this would require a different lookup path -- deferred to a follow-up if observed in practice.

### 2. Nested Struct Splits
**Not supported.** UE's `SplitPin` is non-recursive in a single call. Splitting a Transform creates Location, Rotation, Scale sub-pins (all struct-typed). Splitting Location_X would require a second `SplitPin` call on the Location sub-pin. This is architecturally possible but adds complexity with diminishing returns. The AI should use `break_struct` in plans for nested decomposition.

### 3. Wildcard Pins
Wildcard pins (`PC_Wildcard`) have no concrete type. `CanSplitStructPin` returns `false` for wildcards, so the SplitPin fallback naturally skips them. Autocast via `TryCreateConnection` handles wildcards correctly (the engine resolves them).

### 4. Array/Set/Map Container Pins
`CanSplitStructPin` returns `false` for container types. The SplitPin fallback naturally skips them. Autocast handles some container conversions (e.g., Set->Array via `Set_ToArray`).

### 5. bAllowConversion=false Callers
All exec wiring calls pass `bAllowConversion=false`. These are unaffected by the refactor -- `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE` now returns a clean error message instead of falling through to the broken custom path. Net effect: same behavior (error), better error message.

### 6. Multiple Connections to Same Source Pin
When `RealSourcePin` already has connections (e.g., wired to another target in a previous iteration), `TryCreateConnection` handles this correctly -- data output pins allow multiple connections in UE Blueprints. The conversion detection heuristic (`LinkedTo[0] != TargetPin`) needs adjustment:

**Fix:** Check the **last** entry in `LinkedTo` instead of `[0]`, since new connections are appended:
```cpp
const bool bConversionInserted = (RealSourcePin->LinkedTo.Num() > SourceLinkCountBefore &&
    RealSourcePin->LinkedTo.Last() != RealTargetPin);
```

Wait -- this is subtler. When a conversion node is inserted:
- `SourcePin->LinkedTo` gets the conversion node's input added
- `TargetPin->LinkedTo` gets the conversion node's output added
- Neither `LinkedTo` array contains the other original pin

A more robust check:
```cpp
const bool bConversionInserted = bNeedsConversion && bSuccess;
```

But we don't have `bNeedsConversion` in the WireDataConnection scope. We need to either:
- (a) Probe `CanCreateConnection` before calling `Connect` to know if conversion was needed
- (b) Use the existing heuristic but check the newly-added link

**Decision:** Use approach (a). Add a probe call before `Connector.Connect()` in `WireDataConnection`:

```cpp
// Probe to detect if conversion will be needed (for ConversionNote tracking)
const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
FPinConnectionResponse ProbeResponse = K2Schema->CanCreateConnection(RealSourcePin, RealTargetPin);
const bool bWillNeedConversion =
    (ProbeResponse.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE);
```

Then after success:
```cpp
if (bWillNeedConversion)
{
    // Conversion node was inserted -- record it
    FOliveConversionNote Note;
    // ... populate from source/target ...
    // Find the conversion node: it's between source and target.
    // After insertion, SourcePin is linked to the conversion's input.
    // The new link is the one that wasn't there before.
    for (int32 i = SourceLinkCountBefore; i < RealSourcePin->LinkedTo.Num(); ++i)
    {
        UEdGraphNode* PossibleConv = RealSourcePin->LinkedTo[i]->GetOwningNode();
        if (PossibleConv && PossibleConv != TargetNode)
        {
            Note.ConversionNodeType = PossibleConv->GetClass()->GetName();
            break;
        }
    }
    Context.ConversionNotes.Add(MoveTemp(Note));
}
```

This replaces the existing heuristic at lines 2561-2595 and is robust against multi-connection scenarios.

### 7. SplitPin on Input Pins (Target Side)
The current design only splits **source (output)** pins. Splitting input pins is also valid in UE (e.g., splitting a Vector input to set X/Y/Z individually), but is not needed for the data wiring use case. If the AI wants to set individual components of a struct input, it should use `set_pin_default` with the appropriate value format. Deferred.

### 8. SplitPin + Autocast Combo
After splitting a Vector pin to get `ReturnValue_X` (Float), connecting to a Double input still needs autocast (Float->Double). The re-attempt `Connector.Connect(SubPin, RealTargetPin, /*bAllowConversion=*/true)` handles this because `bAllowConversion=true` triggers the new autocast path. This combo case works automatically.

---

## Risk Assessment

### Low Risk
- **Task 1 (Connect refactor):** The fix delegates to `TryCreateConnection` which is the exact same code path the Blueprint editor uses for manual wire drags. Extremely well-tested by Epic. Risk of regression: very low.
- **Task 2 (Dead code removal):** Removing code that was never executed (CreateConversionNode always returned nullptr). Zero functional change.
- **Task 3 (CanConnect fix):** Minor change, only affects reporting accuracy.
- **Task 4 (GraphWriter bAllowConversion):** Small behavioral change but strictly an improvement -- the user/AI already decided to connect these pins.

### Medium Risk
- **Task 5 (SplitPin fallback):** This creates sub-pins on the source node, which is a visible side effect. If the AI later needs the unsplit pin (e.g., to wire the full Vector to another Vector input), the parent pin is hidden but still functional for connections. UE handles connections to hidden parent pins correctly -- they work, but the pin is visually hidden in the editor. However, this only triggers when the original connection **already failed**, so the alternative is a wiring error anyway.
- **Task 6 (ResolveSubPinSuffix):** The suffix resolution heuristics could pick the wrong component. Mitigated by: (a) explicit AI hints take priority, (b) target pin name matching is straightforward, (c) default-to-X is reasonable for the most common case (position components).

### Mitigation
- SplitPin fallback only activates on connection failure (not proactively)
- Both autocast and SplitPin generate `FOliveConversionNote` entries visible in tool results, so the AI can see what happened and correct if wrong
- The AI can always use explicit `break_struct` plan ops to control decomposition precisely

---

## File Summary

| File | Changes |
|------|---------|
| `Blueprint/Private/Writer/OlivePinConnector.cpp` | **T1:** Rewrite `Connect()`. **T2:** Delete 4 methods. **T3:** Fix `CanConnect()`. |
| `Blueprint/Public/Writer/OlivePinConnector.h` | **T2:** Remove 4 declarations from public/private interface. |
| `Blueprint/Private/Writer/OliveGraphWriter.cpp` | **T4:** Change `bAllowConversion=false` to `true` on line 540. |
| `Blueprint/Private/Plan/OlivePlanExecutor.cpp` | **T5:** Add SplitPin fallback in `WireDataConnection()`. **T6:** Add `ResolveSubPinSuffix()` static helper. **T7:** Replace conversion detection heuristic with probe-based detection. |

No new files. No Build.cs changes. No IR struct changes. No new error codes (existing error messages are improved).

---

## Implementation Order

1. **Task 2: Delete dead code** -- removes noise, makes Task 1 easier to verify
2. **Task 1: Rewrite Connect()** -- the critical bug fix
3. **Task 3: Fix CanConnect()** -- trivial, while in the file
4. **Task 4: GraphWriter bAllowConversion** -- one-line change
5. **Task 7: Replace conversion detection heuristic** -- prepare PlanExecutor for new PinConnector behavior
6. **Task 6: ResolveSubPinSuffix** -- add the helper before it's called
7. **Task 5: SplitPin fallback** -- the main PlanExecutor enhancement, depends on Task 6
8. **Build + test** -- compile, test Int->Double autocast, Vector->Float split

Tasks 1-4 can be done as one batch (all in OlivePinConnector / OliveGraphWriter). Tasks 5-7 as a second batch (all in OlivePlanExecutor).

---

## Verification Checklist

After implementation, verify:

- [ ] `plan_json` with `Int32 -> Double` input wiring: conversion node auto-inserted, ConversionNote in result
- [ ] `plan_json` with `Vector -> Float` input wiring: pin split, sub-pin connected, ConversionNote with "SplitPin(X)"
- [ ] `plan_json` with explicit `@step.~ReturnValue_Z` hint: correct sub-pin (Z) selected after split
- [ ] `connect_pins` tool with `String -> Name`: autocast works
- [ ] `connect_pins` tool with `Object -> Interface`: autocast works (specialized conversion node)
- [ ] Exec wiring (`bAllowConversion=false`): no change in behavior, clean error if types mismatch
- [ ] Already-split pin: no double-split, graceful failure
- [ ] Undo: FScopedTransaction covers both autocast node insertion and pin splitting
