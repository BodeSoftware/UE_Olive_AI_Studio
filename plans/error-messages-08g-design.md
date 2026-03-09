# Error Messages 08g: UPROPERTY Detection + Pin Listing + Alias Fix

## Problem Statement

From run 08g analysis, 6-7 of 10 plan_json failures stem from the AI getting generic "not found" errors and wasting 3-4 retry turns. Three targeted improvements reduce first-failure-to-fix from ~4 turns to ~1 turn.

**Current failure modes:**
1. AI calls `SetSpeed`, `Set InitialSpeed` -- these are UPROPERTYs, not functions. Error says "Function 'SetSpeed' not found" with class-scoped suggestions (from error-recovery design), but FindFunctionEx itself has no UPROPERTY awareness. The UPROPERTY detection only exists downstream in `BuildScopedSuggestions` in the resolver -- FindFunctionEx's own `SearchedLocations` and error trail says nothing about properties.
2. `connect_pins` says "Pin 'Normal' not found on node 'X'" with no listing of what pins ARE available. The AI guesses.
3. `GetActorTransform` alias maps to itself (no-op) instead of the C++ function name `GetTransform`.

---

## Change 1: FindFunctionEx UPROPERTY Detection

### Current State

`FindFunctionEx()` in `OliveNodeFactory.cpp` (line 2640) delegates to `FindFunction()`, and on failure builds a `SearchedLocations` trail listing every class searched. It appends fuzzy suggestions from `LastFuzzySuggestions` (line 2817). But it does NOT check if the failed name matches a UPROPERTY.

The downstream consumer `ResolveCallOp()` in `OliveBlueprintPlanResolver.cpp` already has UPROPERTY detection via `FOliveClassAPIHelper::BuildScopedSuggestions()` (lines 1799-1808). However, this only fires in the plan_json resolver path. Other callers of `FindFunctionEx` (like `CreateCallFunctionNode` at line 1826) do NOT benefit.

### Proposed Change

Add a UPROPERTY cross-check to `FindFunctionEx()` itself, after the search trail is built but before returning. This ensures ALL callers get UPROPERTY awareness, not just the plan resolver.

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`

**Location:** After line 2821 (after fuzzy suggestions are appended), before the `return Result;` at line 2823.

**Logic:**

```cpp
// --- UPROPERTY cross-check ---
// When a function name looks like a property setter/getter (e.g., "SetSpeed",
// "GetMaxHealth"), check if the stripped name matches a BlueprintVisible property
// on any of the searched classes. This catches the common AI mistake of trying
// to call a C++ UPROPERTY as if it were a BlueprintCallable function.

if (Blueprint)
{
    FString PropertyCandidate = ResolvedName;
    // Strip common prefixes: "Set ", "Get ", "Set", "Get"
    // Note: "Set " with space first (handles "Set MaxSpeed" -> "MaxSpeed")
    if (PropertyCandidate.StartsWith(TEXT("Set ")) || PropertyCandidate.StartsWith(TEXT("Get ")))
    {
        PropertyCandidate = PropertyCandidate.Mid(4);
    }
    else if (PropertyCandidate.StartsWith(TEXT("Set")) && PropertyCandidate.Len() > 3 && FChar::IsUpper(PropertyCandidate[3]))
    {
        PropertyCandidate = PropertyCandidate.Mid(3);
    }
    else if (PropertyCandidate.StartsWith(TEXT("Get")) && PropertyCandidate.Len() > 3 && FChar::IsUpper(PropertyCandidate[3]))
    {
        PropertyCandidate = PropertyCandidate.Mid(3);
    }
    // Also try the original name (handles "MaxSpeed" directly)
    // plus "b" prefix for booleans (handles "SetOrientRotationToMovement" -> "bOrientRotationToMovement")
    TArray<FString> Candidates = { PropertyCandidate, ResolvedName };
    if (!PropertyCandidate.StartsWith(TEXT("b")))
    {
        Candidates.Add(TEXT("b") + PropertyCandidate);
    }

    // Classes to scan: Blueprint's GeneratedClass + parent hierarchy + SCS components
    TArray<UClass*> ClassesToScan;
    if (Blueprint->GeneratedClass) ClassesToScan.Add(Blueprint->GeneratedClass);
    if (Blueprint->ParentClass)
    {
        UClass* Walk = Blueprint->ParentClass;
        while (Walk && Walk != UObject::StaticClass())
        {
            ClassesToScan.AddUnique(Walk);
            Walk = Walk->GetSuperClass();
        }
    }
    if (Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (SCSNode && SCSNode->ComponentClass)
            {
                ClassesToScan.AddUnique(SCSNode->ComponentClass);
            }
        }
    }

    // Also scan specified class if provided
    if (!ClassName.IsEmpty())
    {
        UClass* SpecifiedClass = FindClass(ClassName);
        if (SpecifiedClass)
        {
            ClassesToScan.AddUnique(SpecifiedClass);
        }
    }

    for (UClass* ScanClass : ClassesToScan)
    {
        for (TFieldIterator<FProperty> PropIt(ScanClass, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
        {
            const FProperty* Prop = *PropIt;
            if (!Prop) continue;
            if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly)) continue;

            const FString PropName = Prop->GetName();
            for (const FString& Cand : Candidates)
            {
                if (PropName.Equals(Cand, ESearchCase::IgnoreCase))
                {
                    // Found a matching property! Add a prominent message to SearchedLocations.
                    FString TypeStr = FOliveClassAPIHelper::GetPropertyTypeString(Prop);
                    Result.SearchedLocations.Add(FString::Printf(
                        TEXT("PROPERTY MATCH: '%s' is a %s property on %s, not a callable function. "
                             "Use set_var/get_var op instead of call."),
                        *PropName, *TypeStr, *ScanClass->GetName()));
                    // Done -- one match is enough
                    goto PropertyScanDone;
                }
            }
        }
    }
    PropertyScanDone:;
}
```

### Why `goto` Is Acceptable Here

Breaking out of two nested loops. The alternative (a lambda + early return, or a bool flag) is more verbose for the same effect. This is a common C/C++ pattern in UE codebase itself.

### Edge Cases

1. **No Blueprint passed:** Skip entirely. `FindFunctionEx` is sometimes called without a Blueprint (e.g., from `ValidateNodeType`). The UPROPERTY check gracefully does nothing.

2. **Property name collision with a function on a different class:** The UPROPERTY check fires AFTER all function searches have failed. If a property matches, it's genuinely not a function.

3. **"Set" prefix on non-property names:** e.g., "SetTimer" gets stripped to "Timer". No property named "Timer" exists, so no false positive. The check requires an exact case-insensitive match.

4. **Multiple matching properties across different classes:** Only the first match is reported (breaks on first hit). This is fine -- one actionable message is enough.

5. **Performance:** Only runs on the failure path. `TFieldIterator` is O(n) per class but the property count per class is small (typically 10-50). The total scan across all classes is negligible.

### Header Change

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`

Add at the top (if not already included):
```cpp
#include "Writer/OliveClassAPIHelper.h"
```

This is needed for `FOliveClassAPIHelper::GetPropertyTypeString()`.
Add after line 5 (`#include "OliveClassResolver.h"`) in `OliveNodeFactory.cpp`.

---

## Change 2: connect_pins Lists Available Pins on Failure

### Current State

When `FindPin()` returns nullptr in `FOliveGraphWriter::ConnectPins()` (lines 497-501 and 531-535), the error is:

```
"Source pin 'Normal' not found on node 'node_3'"
```

No indication of what pins ARE available. The AI calls `blueprint.get_node_pins` to find out, wasting a turn.

Note: The PlanExecutor's `WireDataConnection()` already builds pin suggestions on failure (lines 2693-2698 for target, 2827-2832 for source). This improvement targets the `connect_pins` tool's GraphWriter path, which is a separate code path.

### Proposed Change

Add a helper function to build a compact pin listing, and call it when `FindPin()` returns nullptr.

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp`

**New helper** (in the anonymous namespace or as a private static):

```cpp
namespace
{
    /**
     * Build a compact one-line listing of available pins on a node.
     * Format: "exec_out (Exec), ReturnValue (Float), OtherActor (Object), ..."
     * Skips hidden pins. Caps at 15 entries.
     */
    FString BuildAvailablePinsList(const UEdGraphNode* Node, EEdGraphPinDirection DirectionFilter)
    {
        if (!Node) return FString();

        TArray<FString> PinEntries;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bHidden) continue;
            if (Pin->Direction != DirectionFilter) continue;

            FString TypeStr;
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                TypeStr = TEXT("Exec");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
            {
                TypeStr = TEXT("Bool");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
            {
                TypeStr = TEXT("Int");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
            {
                TypeStr = TEXT("Int64");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
            {
                // UE 5.x unifies float/double under PC_Real
                TypeStr = TEXT("Float");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String)
            {
                TypeStr = TEXT("String");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
            {
                TypeStr = TEXT("Name");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
            {
                TypeStr = TEXT("Text");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
            {
                UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
                TypeStr = Struct ? Struct->GetName() : TEXT("Struct");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
                     Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface ||
                     Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
                     Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
                     Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass)
            {
                UClass* ObjClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
                TypeStr = ObjClass ? ObjClass->GetName() : TEXT("Object");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte ||
                     Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Enum)
            {
                UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
                TypeStr = Enum ? Enum->GetName() : TEXT("Byte");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate ||
                     Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
            {
                TypeStr = TEXT("Delegate");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
            {
                TypeStr = TEXT("Wildcard");
            }
            else
            {
                TypeStr = Pin->PinType.PinCategory.ToString();
            }

            // Append container prefix
            if (Pin->PinType.IsArray())
            {
                TypeStr = TEXT("Array<") + TypeStr + TEXT(">");
            }
            else if (Pin->PinType.IsSet())
            {
                TypeStr = TEXT("Set<") + TypeStr + TEXT(">");
            }
            else if (Pin->PinType.IsMap())
            {
                TypeStr = TEXT("Map<") + TypeStr + TEXT(">");
            }

            // Use display name if different from internal name (more readable for AI)
            FString PinLabel = Pin->GetDisplayName().ToString();
            if (PinLabel.IsEmpty() || PinLabel == Pin->GetName())
            {
                PinLabel = Pin->GetName();
            }

            PinEntries.Add(FString::Printf(TEXT("%s (%s)"), *PinLabel, *TypeStr));
        }

        // Cap at 15 to keep error message manageable
        const int32 MaxEntries = 15;
        if (PinEntries.Num() > MaxEntries)
        {
            const int32 Shown = MaxEntries;
            const int32 Remaining = PinEntries.Num() - Shown;
            PinEntries.SetNum(Shown);
            PinEntries.Add(FString::Printf(TEXT("...+%d more"), Remaining));
        }

        return FString::Join(PinEntries, TEXT(", "));
    }
}
```

**Modification points in `ConnectPins()`:**

**Source pin not found** (lines 497-502):

```cpp
if (!SourcePin)
{
    FString AvailablePins = BuildAvailablePinsList(SourceNode, EGPD_Output);
    return FOliveBlueprintWriteResult::Error(
        FString::Printf(TEXT("Source pin '%s' not found on node '%s'. Available output pins: %s"),
            *SourcePinName, *SourceNodeId,
            AvailablePins.IsEmpty() ? TEXT("(none)") : *AvailablePins),
        BlueprintPath);
}
```

**Target pin not found** (lines 531-536):

```cpp
if (!TargetPin)
{
    FString AvailablePins = BuildAvailablePinsList(TargetNode, EGPD_Input);
    return FOliveBlueprintWriteResult::Error(
        FString::Printf(TEXT("Target pin '%s' not found on node '%s'. Available input pins: %s"),
            *TargetPinName, *TargetNodeId,
            AvailablePins.IsEmpty() ? TEXT("(none)") : *AvailablePins),
        BlueprintPath);
}
```

**Also update `DisconnectPins()`** (lines 608-613 and 624-629):

Same pattern as ConnectPins. Source pin shows output pins, target shows input pins:

```cpp
// Line 608: Source pin not found in DisconnectPins
FString AvailablePins = BuildAvailablePinsList(SourceNode, EGPD_Output);
return FOliveBlueprintWriteResult::Error(
    FString::Printf(TEXT("Source pin '%s' not found on node '%s'. Available output pins: %s"),
        *SourcePinName, *SourceNodeId,
        AvailablePins.IsEmpty() ? TEXT("(none)") : *AvailablePins),
    BlueprintPath);

// Line 625: Target pin not found in DisconnectPins
FString AvailablePins = BuildAvailablePinsList(TargetNode, EGPD_Input);
return FOliveBlueprintWriteResult::Error(
    FString::Printf(TEXT("Target pin '%s' not found on node '%s'. Available input pins: %s"),
        *TargetPinName, *TargetNodeId,
        AvailablePins.IsEmpty() ? TEXT("(none)") : *AvailablePins),
    BlueprintPath);
```

**Also update `DisconnectAllFromPin()`** (line 692-697) -- direction unknown, list both:

For disconnect-all, the AI is specifying a single pin, direction is unknown. Use a helper overload or list both directions:

```cpp
// Line 692: Pin not found in DisconnectAllFromPin
// List all pins since we don't know the intended direction
FString OutputPins = BuildAvailablePinsList(Node, EGPD_Output);
FString InputPins = BuildAvailablePinsList(Node, EGPD_Input);
FString AllPins;
if (!OutputPins.IsEmpty()) AllPins += TEXT("Outputs: ") + OutputPins;
if (!InputPins.IsEmpty())
{
    if (!AllPins.IsEmpty()) AllPins += TEXT(". ");
    AllPins += TEXT("Inputs: ") + InputPins;
}
return FOliveBlueprintWriteResult::Error(
    FString::Printf(TEXT("Pin '%s' not found on node '%s'. Available pins: %s"),
        *PinName, *NodeId,
        AllPins.IsEmpty() ? TEXT("(none)") : *AllPins),
    BlueprintPath);
```

**Also update `SetPinDefault()`** (line 761-766) -- should list input pins only since set_pin_default only works on inputs:

```cpp
// Line 761: Pin not found in SetPinDefault
FString AvailablePins = BuildAvailablePinsList(Node, EGPD_Input);
return FOliveBlueprintWriteResult::Error(
    FString::Printf(TEXT("Pin '%s' not found on node '%s'. Available input pins: %s"),
        *PinName, *NodeId,
        AvailablePins.IsEmpty() ? TEXT("(none)") : *AvailablePins),
    BlueprintPath);
```

### Edge Cases

1. **Node with no visible pins:** Returns "(none)". Very rare but handles gracefully.

2. **Nodes with many pins (e.g., MakeStruct for a large struct):** Capped at 15 entries with "+N more" suffix. The AI gets enough context to pick the right pin without bloating the error message.

3. **Hidden pins (self pin, internal pins):** Excluded by the `bHidden` check. The AI only sees pins it can actually use.

4. **Sub-pins (from SplitPin):** Sub-pins have `ParentPin != nullptr`. They are not hidden by default, so they will appear. This is actually helpful -- if the AI sees split sub-pins listed, it knows to use them.

5. **Thread safety:** All pin access is on the game thread (same thread as ConnectPins). No synchronization needed.

### Header Changes

None. `BuildAvailablePinsList` is a file-local helper in the anonymous namespace.

### Additional Include

```cpp
#include "EdGraphSchema_K2.h"  // for pin category constants
```

This should already be included in `OliveGraphWriter.cpp` since it uses `UEdGraphSchema_K2::PC_Exec` etc. Verify before implementing.

---

## Change 3: GetActorTransform Alias Fix

### Current State

In `OliveNodeFactory.cpp`, the alias map has three entries for `GetActorTransform`:

**Line 2851:**
```cpp
Map.Add(TEXT("GetTransform"), TEXT("GetActorTransform"));
```

**Line 2852:**
```cpp
Map.Add(TEXT("GetActorTransform"), TEXT("GetTransform"));
```

**Line 3087:**
```cpp
Map.Add(TEXT("GetActorTransform"), TEXT("GetActorTransform"));  // self-referential no-op
```

The UE engine function is `AActor::GetTransform()` (C++ name), with `ScriptName = "GetActorTransform"`. `FindFunctionByName` searches by C++ name, so `"GetTransform"` would be found directly.

**Problem with current state:**

- Line 2852 correctly maps `GetActorTransform` -> `GetTransform` (the C++ name)
- BUT line 3087 overwrites line 2852 with `GetActorTransform` -> `GetActorTransform` (self-referential)
- Net effect: `GetActorTransform` alias resolves to `GetActorTransform`, which `FindFunctionByName` does NOT find (the C++ name is `GetTransform`)
- The function is only found because Step 3 (parent class search) or Step 7 (library search) tries `GetActorTransform` on AActor, where it exists as a ScriptName. Wait -- `FindFunctionByName` uses the C++ function name, not the ScriptName. Let me verify.

Actually, `FindFunctionByName` uses `FName` which is the C++ function name. `GetTransform` is the C++ name. The ScriptName `GetActorTransform` is only for Blueprint graph display. So `FindFunctionByName(TEXT("GetActorTransform"))` would NOT find `GetTransform()`.

However, the `TryClassWithK2` lambda also tries `K2_GetActorTransform` as a fallback (K2 prefix variant). This also would not match `GetTransform`.

So the net result is: if the AI says `GetActorTransform`, the alias resolves to `GetActorTransform` (no-op), FindFunction tries `GetActorTransform` and `K2_GetActorTransform` across all classes, finds nothing, and fails. The function is actually named `GetTransform` on AActor.

Line 2851 also has a bug: `GetTransform` -> `GetActorTransform`. This redirects from the correct name to the wrong name. If the AI says `GetTransform`, it gets remapped to `GetActorTransform`, which then fails unless line 2852 is present AND not overwritten. But line 3087 overwrites 2852. So `GetTransform` -> `GetActorTransform` -> `GetActorTransform` (no-op) -> not found.

Wait, but `GetTransform` would be found directly by `FindFunctionByName` since that IS the C++ name. The alias is checked BEFORE class search. So:
1. AI says `GetTransform`
2. Alias maps it to `GetActorTransform` (line 2851)
3. `ResolvedName = "GetActorTransform"`
4. `FindFunctionByName("GetActorTransform")` -- not found on any class
5. K2 variant `FindFunctionByName("K2_GetActorTransform")` -- not found
6. Fail

So line 2851 is actively harmful: it takes a working name (`GetTransform`) and breaks it.

### Proposed Fix

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`

**Line 2851:** Remove the line entirely (or change to a comment). `GetTransform` is the actual C++ name; it should NOT be aliased to anything.

```cpp
// REMOVED: Map.Add(TEXT("GetTransform"), TEXT("GetActorTransform"));
// GetTransform is the actual C++ function name on AActor. No alias needed.
```

**Line 2852:** Keep as-is. This correctly maps `GetActorTransform` (common AI name / display name) to `GetTransform` (C++ function name).

```cpp
Map.Add(TEXT("GetActorTransform"), TEXT("GetTransform"));  // line 2852 -- CORRECT
```

**Line 3087:** Remove the self-referential no-op that overwrites line 2852.

```cpp
// REMOVED: Map.Add(TEXT("GetActorTransform"), TEXT("GetActorTransform"));
// Was overwriting the correct alias at line 2852.
```

### Verification

After fix:
- AI says `GetActorTransform` -> alias resolves to `GetTransform` -> `FindFunctionByName("GetTransform")` finds `AActor::GetTransform()`. Correct.
- AI says `GetTransform` -> no alias match -> `FindFunctionByName("GetTransform")` finds `AActor::GetTransform()`. Correct.
- AI says `K2_GetActorTransform` -> no alias match -> `FindFunctionByName("K2_GetActorTransform")` not found -> K2 fallback removes prefix -> `FindFunctionByName("GetActorTransform")` not found -> continues to parent class search -> `GetActorTransform` not found as C++ name -> eventual fail. This is correct because `K2_GetActorTransform` doesn't exist.

### Risk Assessment

Minimal. Only affects the `GetTransform`/`GetActorTransform` pair. No other aliases are affected. The fix restores the intended behavior of line 2852 by removing the overwrite at line 3087.

---

## Summary of All Changes

### Files Modified

| File | Changes |
|------|---------|
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | (1) Add UPROPERTY cross-check in `FindFunctionEx()` after line 2821. (2) Remove self-referential alias at line 3087. (3) Remove harmful alias at line 2851. (4) Add `#include "Writer/OliveClassAPIHelper.h"` if not present. |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp` | (1) Add `BuildAvailablePinsList()` helper in anonymous namespace. (2) Update source pin-not-found error in `ConnectPins()` at line 499. (3) Update target pin-not-found error in `ConnectPins()` at line 533. (4) Update pin-not-found errors in `DisconnectPins()`, `DisconnectAllFromPin()`, and `SetPinDefault()`. |

### Files NOT Modified

- `OliveNodeFactory.h` -- no interface changes
- `OliveGraphWriter.h` -- no interface changes
- `OliveBlueprintPlanResolver.cpp` -- already has class-scoped suggestions via `BuildScopedSuggestions()`
- `OlivePlanExecutor.cpp` -- already has pin suggestion lists via `GetDataPins()`
- No new files created

### New Dependencies

- `OliveNodeFactory.cpp` needs `#include "Writer/OliveClassAPIHelper.h"` (for `GetPropertyTypeString()`)
- `OliveGraphWriter.cpp` needs `UEdGraphSchema_K2` constants (likely already included)

---

## Implementation Order

1. **Change 3 first** (alias fix) -- 2 minutes, zero risk, immediately testable
2. **Change 2 second** (pin listing) -- 20 minutes, self-contained in OliveGraphWriter.cpp
3. **Change 1 third** (UPROPERTY detection) -- 30 minutes, touches OliveNodeFactory.cpp

### Testing

**Change 3:**
- Create a Blueprint Actor
- In plan_json, use `{"op": "call", "target": "GetActorTransform"}` -- should succeed
- Also test `{"op": "call", "target": "GetTransform"}` -- should succeed

**Change 2:**
- Use `connect_pins` with a deliberately wrong pin name
- Verify error message lists available pins with types
- Test on a node with many pins (e.g., SpawnActor) to verify cap at 15

**Change 1:**
- In plan_json, use `{"op": "call", "target": "SetSpeed"}` on a Blueprint with a ProjectileMovementComponent
- Verify `FindFunctionEx` result's `SearchedLocations` contains a `PROPERTY MATCH` entry
- Test with `Set MaxSpeed` (space-separated) and `SetOrientRotationToMovement` (bool property)

---

## Coder Notes

1. **Change 1 uses `goto`** for breaking out of nested loops. This is acceptable. If the coder prefers a lambda + return pattern, that's fine too.

2. **The UPROPERTY check in Change 1 is additive** -- it appends to `SearchedLocations`, it does not replace any existing behavior. All existing fuzzy suggestions, search trail entries, etc. remain.

3. **`BuildAvailablePinsList` in Change 2 should handle the `PN_Self` pin** -- it IS hidden by default on most nodes (`bHidden = true`), so the existing `bHidden` filter already excludes it. But verify this assumption during implementation.

4. **Change 3 is literally 2 line deletions** plus a comment. If the coder wants to be extra safe, they can add a unit test that calls `GetAliasMap()` and checks `GetActorTransform` maps to `GetTransform`.

5. **The `#include` for OliveClassAPIHelper.h in OliveNodeFactory.cpp** -- check if it's already included transitively. If `OliveNodeFactory.cpp` already includes it (or a header that includes it), skip the explicit add. If not, add it near the top with the other Writer includes.
