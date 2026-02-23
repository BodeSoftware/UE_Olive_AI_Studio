# Phase A Task: OlivePinManifest

## Overview

Create two files that implement pin introspection manifests -- ground-truth pin metadata built by reading a real `UEdGraphNode*` after creation. This replaces all pin-name guessing with actual introspection data.

**Files to create:**
- `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePinManifest.h`
- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePinManifest.cpp`

**Depends on (read-only, do NOT modify):**
- `Source/OliveAIRuntime/Public/IR/OliveIRTypes.h` (for `EOliveIRTypeCategory`)
- `Source/OliveAIEditor/Blueprint/Public/Reader/OlivePinSerializer.h` (for `MapPinCategory` pattern)
- `EdGraphSchema_K2.h` (for `PC_Exec`, `PC_Boolean`, etc. constants and `TypeToText`)
- `EdGraph/EdGraphPin.h` (for `UEdGraphPin`, `EGPD_Input`, `EGPD_Output`)

**Directory already exists:** `Source/OliveAIEditor/Blueprint/Public/Plan/` (contains `OliveBlueprintPlanLowerer.h` and `OliveBlueprintPlanResolver.h`). The `Private/Plan/` directory does NOT exist yet -- create it.

---

## File 1: OlivePinManifest.h

Write this file EXACTLY as specified. This is the complete header.

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/OliveIRTypes.h"  // EOliveIRTypeCategory

// Forward declarations
class UEdGraphNode;
class UEdGraphPin;
class FJsonObject;
class FJsonValue;
struct FEdGraphPinType;

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePinManifest, Log, All);

/**
 * Introspected pin data from a real UEdGraphNode after creation.
 * This is ground truth -- these are the ACTUAL pin names, not guesses.
 */
struct OLIVEAIEDITOR_API FOlivePinManifestEntry
{
    /** Exact internal pin name as returned by UEdGraphPin::GetName() */
    FString PinName;

    /** Display name as returned by UEdGraphPin::GetDisplayName().ToString() */
    FString DisplayName;

    /** Pin direction: true = input, false = output */
    bool bIsInput = true;

    /** Pin category from UEdGraphPin::PinType (e.g., "exec", "bool", "float", "object") */
    FString PinCategory;

    /** Pin subcategory (class name for objects, struct name for structs) */
    FString PinSubCategory;

    /** Full type string for AI-readable description (e.g., "bool", "Actor Object Reference") */
    FString TypeDisplayString;

    /** Whether this is an execution (flow control) pin */
    bool bIsExec = false;

    /** Whether this pin is hidden (e.g., self pin on static functions) */
    bool bIsHidden = false;

    /** Whether this pin has a default value */
    bool bHasDefaultValue = false;

    /** Current default value (for input pins) */
    FString DefaultValue;

    /** Whether this pin is currently connected */
    bool bIsConnected = false;

    /** IR type category for type-compatibility matching */
    EOliveIRTypeCategory IRTypeCategory = EOliveIRTypeCategory::Unknown;
};

/**
 * Complete pin manifest for a single created node.
 * Built by introspecting the real UEdGraphNode* after CreateNode().
 *
 * This is the contract between node creation (Phase 1) and
 * wiring (Phases 3-5). All pin references in wiring phases
 * resolve against this manifest, never against AI-guessed names.
 */
struct OLIVEAIEDITOR_API FOlivePinManifest
{
    /** The step ID from the plan that created this node */
    FString StepId;

    /** The node ID assigned by GraphWriter (e.g., "node_0") */
    FString NodeId;

    /** The node type (e.g., "CallFunction", "Branch") */
    FString NodeType;

    /** For CallFunction: the resolved function name */
    FString ResolvedFunctionName;

    /** Whether this node has exec flow (has at least one exec pin) */
    bool bHasExecPins = false;

    /** Whether this is a pure node (no exec pins) */
    bool bIsPure = false;

    /** All pins on this node */
    TArray<FOlivePinManifestEntry> Pins;

    // ====================================================================
    // Query Methods
    // ====================================================================

    /**
     * Find the primary exec input pin (the "execute" pin).
     * For most nodes, there is exactly one. Returns nullptr if pure node.
     */
    const FOlivePinManifestEntry* FindExecInput() const;

    /**
     * Find the primary exec output pin (the "then" pin).
     * For most nodes, there is exactly one. Returns nullptr if pure node.
     */
    const FOlivePinManifestEntry* FindExecOutput() const;

    /**
     * Find all exec output pins (e.g., Branch has "True" and "False").
     * @return Array of pointers to exec output pin entries
     */
    TArray<const FOlivePinManifestEntry*> FindAllExecOutputs() const;

    /**
     * Find a data input pin by exact name.
     * @param Name Pin name to search for
     * @return Pointer to the pin entry, or nullptr
     */
    const FOlivePinManifestEntry* FindDataInputByName(const FString& Name) const;

    /**
     * Find a data output pin by exact name.
     * @param Name Pin name to search for
     * @return Pointer to the pin entry, or nullptr
     */
    const FOlivePinManifestEntry* FindDataOutputByName(const FString& Name) const;

    /**
     * Find a pin using the smart fallback chain:
     * exact name -> display name -> case-insensitive -> fuzzy -> type-match.
     *
     * @param Hint The name hint from the plan (may be approximate)
     * @param bIsInput Whether to search input or output pins
     * @param TypeHint Optional: expected type category for type-based fallback
     * @param OutMatchMethod Set to the method that matched (for diagnostics)
     * @return Pointer to the best matching pin entry, or nullptr
     */
    const FOlivePinManifestEntry* FindPinSmart(
        const FString& Hint,
        bool bIsInput,
        EOliveIRTypeCategory TypeHint = EOliveIRTypeCategory::Unknown,
        FString* OutMatchMethod = nullptr) const;

    /**
     * Get all non-hidden, non-exec data pins in a given direction.
     * @param bInput True for input pins, false for output pins
     * @return Array of pointers to data pin entries
     */
    TArray<const FOlivePinManifestEntry*> GetDataPins(bool bInput) const;

    /**
     * Serialize manifest to JSON for inclusion in apply result.
     * Useful for debugging and for the AI to understand what was created.
     */
    TSharedPtr<FJsonObject> ToJson() const;

    // ====================================================================
    // Static Builder
    // ====================================================================

    /**
     * Build a manifest by introspecting a real UEdGraphNode.
     * This is the core factory method -- it reads every pin on the node
     * and populates the manifest with ground-truth data.
     *
     * @param Node The created node to introspect
     * @param StepId The plan step ID that created this node
     * @param NodeId The GraphWriter node ID assigned to this node
     * @param NodeType The OliveNodeTypes constant
     * @return Populated manifest
     */
    static FOlivePinManifest Build(
        UEdGraphNode* Node,
        const FString& StepId,
        const FString& NodeId,
        const FString& NodeType);

private:
    // ====================================================================
    // Internal Helpers
    // ====================================================================

    /**
     * Convert a UE FEdGraphPinType to our EOliveIRTypeCategory.
     * Uses the same mapping as FOlivePinSerializer::MapPinCategory.
     */
    static EOliveIRTypeCategory ConvertPinTypeToIRCategory(const FEdGraphPinType& PinType);

    /**
     * Compute Levenshtein edit distance between two strings.
     * Used by FindPinSmart for fuzzy matching.
     */
    static int32 LevenshteinDistance(const FString& A, const FString& B);
};
```

---

## File 2: OlivePinManifest.cpp

### File Path

`Source/OliveAIEditor/Blueprint/Private/Plan/OlivePinManifest.cpp`

You MUST create the directory `Source/OliveAIEditor/Blueprint/Private/Plan/` first.

### Includes (exact list, in this order)

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "OlivePinManifest.h"

// UE includes
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

DEFINE_LOG_CATEGORY(LogOlivePinManifest);
```

**Why `K2Node_CallFunction.h`:** The Build() method needs to check if the node is a `UK2Node_CallFunction` to extract `ResolvedFunctionName` via `UK2Node_CallFunction::GetTargetFunction()`.

### Implementation Notes for Each Method

---

### `FOlivePinManifest::Build(Node, StepId, NodeId, NodeType)`

This is the most important method. Here is the exact algorithm:

```
1. Guard: if Node is nullptr, return empty manifest with a warning log.

2. Initialize manifest fields:
   Manifest.StepId = StepId
   Manifest.NodeId = NodeId
   Manifest.NodeType = NodeType

3. If the node is a UK2Node_CallFunction (use Cast<UK2Node_CallFunction>(Node)):
   - Get the function via CallFuncNode->GetTargetFunction()
   - If function is valid: Manifest.ResolvedFunctionName = Function->GetName()

4. Iterate Node->Pins:
   For each UEdGraphPin* Pin in Node->Pins:

   a. Skip criteria (SKIP the pin entirely, do NOT add to manifest):
      - Pin is nullptr
      - Pin->bHidden is true AND Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
        (We skip hidden data pins like WorldContextObject and self pins,
         but we NEVER skip hidden exec pins -- some nodes have hidden exec pins
         that are still wirable)

      NOTE: Do NOT skip based on pin name. Some legitimate pins have names
      like "self" that are hidden but we want to exclude. The bHidden flag
      handles this correctly.

   b. Create FOlivePinManifestEntry:
      Entry.PinName = Pin->GetName()
      Entry.DisplayName = Pin->GetDisplayName().ToString()
      Entry.bIsInput = (Pin->Direction == EGPD_Input)
      Entry.PinCategory = Pin->PinType.PinCategory.ToString()
      Entry.bIsExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
      Entry.bIsHidden = Pin->bHidden
      Entry.bIsConnected = Pin->HasAnyConnections()
      Entry.IRTypeCategory = ConvertPinTypeToIRCategory(Pin->PinType)

      // Subcategory: extract from PinSubCategoryObject or PinSubCategory
      if (Pin->PinType.PinSubCategoryObject.IsValid())
      {
          Entry.PinSubCategory = Pin->PinType.PinSubCategoryObject->GetName();
      }
      else
      {
          Entry.PinSubCategory = Pin->PinType.PinSubCategory.ToString();
      }

      // Type display string using UE's built-in formatter
      Entry.TypeDisplayString = UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString()

      // Default value: prefer DefaultValue, fall back to AutogeneratedDefaultValue
      Entry.bHasDefaultValue = (!Pin->DefaultValue.IsEmpty() || !Pin->AutogeneratedDefaultValue.IsEmpty())
      Entry.DefaultValue = Pin->DefaultValue.IsEmpty() ? Pin->AutogeneratedDefaultValue : Pin->DefaultValue

   c. Add Entry to Manifest.Pins

5. After iterating all pins:
   Manifest.bHasExecPins = Manifest.Pins.ContainsByPredicate(
       [](const FOlivePinManifestEntry& E) { return E.bIsExec; })
   Manifest.bIsPure = !Manifest.bHasExecPins

6. Log the result:
   UE_LOG(LogOlivePinManifest, Verbose,
       TEXT("Built manifest for step '%s' (node '%s', type '%s'): %d pins (%s)"),
       *StepId, *NodeId, *NodeType, Manifest.Pins.Num(),
       Manifest.bIsPure ? TEXT("pure") : TEXT("exec"))

7. Return Manifest
```

---

### `FOlivePinManifest::ConvertPinTypeToIRCategory(PinType)`

Use the SAME static map pattern as `FOlivePinSerializer::MapPinCategory` (see `OlivePinSerializer.cpp` lines 287-334). Copy the same mapping. Do NOT call PinSerializer -- we want zero coupling to the reader subsystem.

```cpp
EOliveIRTypeCategory FOlivePinManifest::ConvertPinTypeToIRCategory(const FEdGraphPinType& PinType)
{
    // Replicate the mapping from FOlivePinSerializer::MapPinCategory
    // to avoid coupling to the reader subsystem
    static const TMap<FName, EOliveIRTypeCategory> CategoryMap = []()
    {
        TMap<FName, EOliveIRTypeCategory> Map;

        // Execution
        Map.Add(UEdGraphSchema_K2::PC_Exec, EOliveIRTypeCategory::Exec);

        // Primitives
        Map.Add(UEdGraphSchema_K2::PC_Boolean, EOliveIRTypeCategory::Bool);
        Map.Add(UEdGraphSchema_K2::PC_Byte, EOliveIRTypeCategory::Byte);
        Map.Add(UEdGraphSchema_K2::PC_Int, EOliveIRTypeCategory::Int);
        Map.Add(UEdGraphSchema_K2::PC_Int64, EOliveIRTypeCategory::Int64);
        Map.Add(UEdGraphSchema_K2::PC_Float, EOliveIRTypeCategory::Float);
        Map.Add(UEdGraphSchema_K2::PC_Double, EOliveIRTypeCategory::Double);
        Map.Add(UEdGraphSchema_K2::PC_Real, EOliveIRTypeCategory::Float);

        // Strings
        Map.Add(UEdGraphSchema_K2::PC_String, EOliveIRTypeCategory::String);
        Map.Add(UEdGraphSchema_K2::PC_Name, EOliveIRTypeCategory::Name);
        Map.Add(UEdGraphSchema_K2::PC_Text, EOliveIRTypeCategory::Text);

        // Complex types
        Map.Add(UEdGraphSchema_K2::PC_Struct, EOliveIRTypeCategory::Struct);
        Map.Add(UEdGraphSchema_K2::PC_Object, EOliveIRTypeCategory::Object);
        Map.Add(UEdGraphSchema_K2::PC_Class, EOliveIRTypeCategory::Class);
        Map.Add(UEdGraphSchema_K2::PC_Interface, EOliveIRTypeCategory::Interface);
        Map.Add(UEdGraphSchema_K2::PC_Enum, EOliveIRTypeCategory::Enum);

        // Delegates
        Map.Add(UEdGraphSchema_K2::PC_Delegate, EOliveIRTypeCategory::Delegate);
        Map.Add(UEdGraphSchema_K2::PC_MCDelegate, EOliveIRTypeCategory::MulticastDelegate);

        // Soft references (map to underlying types)
        Map.Add(UEdGraphSchema_K2::PC_SoftObject, EOliveIRTypeCategory::Object);
        Map.Add(UEdGraphSchema_K2::PC_SoftClass, EOliveIRTypeCategory::Class);

        // Wildcard
        Map.Add(UEdGraphSchema_K2::PC_Wildcard, EOliveIRTypeCategory::Wildcard);

        return Map;
    }();

    // Handle container types
    if (PinType.IsArray())
    {
        return EOliveIRTypeCategory::Array;
    }
    if (PinType.IsSet())
    {
        return EOliveIRTypeCategory::Set;
    }
    if (PinType.IsMap())
    {
        return EOliveIRTypeCategory::Map;
    }

    const EOliveIRTypeCategory* Found = CategoryMap.Find(PinType.PinCategory);
    if (Found)
    {
        // Special handling for struct subcategories that map to specific types
        if (*Found == EOliveIRTypeCategory::Struct && PinType.PinSubCategoryObject.IsValid())
        {
            FString StructName = PinType.PinSubCategoryObject->GetName();
            if (StructName == TEXT("Vector") || StructName == TEXT("Vector3f"))
            {
                return EOliveIRTypeCategory::Vector;
            }
            if (StructName == TEXT("Vector2D") || StructName == TEXT("Vector2f"))
            {
                return EOliveIRTypeCategory::Vector2D;
            }
            if (StructName == TEXT("Rotator") || StructName == TEXT("Rotator3f"))
            {
                return EOliveIRTypeCategory::Rotator;
            }
            if (StructName == TEXT("Transform") || StructName == TEXT("Transform3f"))
            {
                return EOliveIRTypeCategory::Transform;
            }
            if (StructName == TEXT("Color"))
            {
                return EOliveIRTypeCategory::Color;
            }
            if (StructName == TEXT("LinearColor"))
            {
                return EOliveIRTypeCategory::LinearColor;
            }
        }
        return *Found;
    }

    return EOliveIRTypeCategory::Unknown;
}
```

---

### `FOlivePinManifest::LevenshteinDistance(A, B)`

Standard dynamic-programming Levenshtein distance. Use a single-row optimization to avoid allocating an NxM matrix.

```cpp
int32 FOlivePinManifest::LevenshteinDistance(const FString& A, const FString& B)
{
    const int32 LenA = A.Len();
    const int32 LenB = B.Len();

    if (LenA == 0) return LenB;
    if (LenB == 0) return LenA;

    // Single-row DP: only need current and previous row
    TArray<int32> PrevRow;
    TArray<int32> CurrRow;
    PrevRow.SetNumUninitialized(LenB + 1);
    CurrRow.SetNumUninitialized(LenB + 1);

    // Initialize first row
    for (int32 j = 0; j <= LenB; ++j)
    {
        PrevRow[j] = j;
    }

    for (int32 i = 1; i <= LenA; ++i)
    {
        CurrRow[0] = i;
        for (int32 j = 1; j <= LenB; ++j)
        {
            int32 Cost = (A[i - 1] == B[j - 1]) ? 0 : 1;
            CurrRow[j] = FMath::Min3(
                PrevRow[j] + 1,       // deletion
                CurrRow[j - 1] + 1,   // insertion
                PrevRow[j - 1] + Cost  // substitution
            );
        }
        Swap(PrevRow, CurrRow);
    }

    return PrevRow[LenB];
}
```

---

### `FOlivePinManifest::FindExecInput()`

```
Return the FIRST pin in Pins where:
  bIsExec == true AND bIsInput == true AND bIsHidden == false

Return nullptr if no match found.
```

This handles: "execute" on CallFunction, the exec input on Branch, etc.

---

### `FOlivePinManifest::FindExecOutput()`

```
Return the FIRST pin in Pins where:
  bIsExec == true AND bIsInput == false AND bIsHidden == false

Return nullptr if no match found.
```

This handles: "then" on CallFunction, "Then" on Delay, etc. For Branch nodes this returns "True" (the first exec output) -- callers who need specific exec outputs use FindAllExecOutputs().

---

### `FOlivePinManifest::FindAllExecOutputs()`

```
Collect ALL pins in Pins where:
  bIsExec == true AND bIsInput == false AND bIsHidden == false

Return as TArray<const FOlivePinManifestEntry*>.
```

For Branch: returns ["True", "False"]. For Sequence: returns ["Then 0", "Then 1", ...].

---

### `FOlivePinManifest::FindDataInputByName(Name)`

```
Return the FIRST pin in Pins where:
  bIsExec == false AND bIsInput == true AND bIsHidden == false
  AND PinName == Name

Return nullptr if no match.
```

---

### `FOlivePinManifest::FindDataOutputByName(Name)`

```
Return the FIRST pin in Pins where:
  bIsExec == false AND bIsInput == false AND bIsHidden == false
  AND PinName == Name

Return nullptr if no match.
```

---

### `FOlivePinManifest::FindPinSmart(Hint, bIsInput, TypeHint, OutMatchMethod)`

This is the core resolution algorithm. Implement EXACTLY this fallback chain:

```cpp
const FOlivePinManifestEntry* FOlivePinManifest::FindPinSmart(
    const FString& Hint,
    bool bIsInput,
    EOliveIRTypeCategory TypeHint,
    FString* OutMatchMethod) const
{
    // Collect candidate pins: match direction, exclude exec, exclude hidden
    // We build this filtered list once and reuse across all stages
    TArray<const FOlivePinManifestEntry*> Candidates;
    for (const FOlivePinManifestEntry& Entry : Pins)
    {
        if (Entry.bIsInput == bIsInput && !Entry.bIsExec && !Entry.bIsHidden)
        {
            Candidates.Add(&Entry);
        }
    }

    if (Candidates.IsEmpty())
    {
        return nullptr;
    }

    // ================================================================
    // Stage 1: EXACT NAME MATCH
    // ================================================================
    for (const FOlivePinManifestEntry* Entry : Candidates)
    {
        if (Entry->PinName == Hint)
        {
            if (OutMatchMethod) *OutMatchMethod = TEXT("exact");
            return Entry;
        }
    }

    // ================================================================
    // Stage 2: DISPLAY NAME MATCH
    // ================================================================
    for (const FOlivePinManifestEntry* Entry : Candidates)
    {
        if (Entry->DisplayName == Hint)
        {
            if (OutMatchMethod) *OutMatchMethod = TEXT("display_name");
            return Entry;
        }
    }

    // ================================================================
    // Stage 3: CASE-INSENSITIVE MATCH
    // ================================================================
    for (const FOlivePinManifestEntry* Entry : Candidates)
    {
        if (Entry->PinName.Equals(Hint, ESearchCase::IgnoreCase) ||
            Entry->DisplayName.Equals(Hint, ESearchCase::IgnoreCase))
        {
            if (OutMatchMethod) *OutMatchMethod = TEXT("case_insensitive");
            return Entry;
        }
    }

    // ================================================================
    // Stage 4: FUZZY MATCH (Levenshtein distance + substring)
    // ================================================================
    {
        const FOlivePinManifestEntry* Best = nullptr;
        int32 BestScore = 0;
        FString LowerHint = Hint.ToLower();

        for (const FOlivePinManifestEntry* Entry : Candidates)
        {
            int32 Score = 0;
            FString LowerPinName = Entry->PinName.ToLower();
            FString LowerDisplayName = Entry->DisplayName.ToLower();

            // Substring bonus: if either contains the other
            if (LowerPinName.Contains(LowerHint) || LowerHint.Contains(LowerPinName))
            {
                Score += 60;
            }
            if (LowerDisplayName.Contains(LowerHint) || LowerHint.Contains(LowerDisplayName))
            {
                Score += 50;
            }

            // Levenshtein distance penalty (lower distance = higher score)
            int32 LevenDist = LevenshteinDistance(LowerHint, LowerPinName);
            if (LevenDist <= 3)
            {
                Score += FMath::Max(0, 40 - LevenDist * 10);
            }

            if (Score > BestScore)
            {
                BestScore = Score;
                Best = Entry;
            }
        }

        if (Best != nullptr && BestScore >= 40)
        {
            if (OutMatchMethod) *OutMatchMethod = TEXT("fuzzy");
            UE_LOG(LogOlivePinManifest, Verbose,
                TEXT("Fuzzy matched pin hint '%s' -> '%s' (score=%d)"),
                *Hint, *Best->PinName, BestScore);
            return Best;
        }
    }

    // ================================================================
    // Stage 5: TYPE-BASED MATCH (last resort)
    // ================================================================
    if (TypeHint != EOliveIRTypeCategory::Unknown)
    {
        TArray<const FOlivePinManifestEntry*> TypeMatches;
        for (const FOlivePinManifestEntry* Entry : Candidates)
        {
            if (Entry->IRTypeCategory == TypeHint)
            {
                TypeMatches.Add(Entry);
            }
        }

        if (TypeMatches.Num() == 1)
        {
            if (OutMatchMethod) *OutMatchMethod = TEXT("type_match");
            UE_LOG(LogOlivePinManifest, Verbose,
                TEXT("Type-matched pin hint '%s' -> '%s' (type category match)"),
                *Hint, *TypeMatches[0]->PinName);
            return TypeMatches[0];
        }
        // If multiple type matches: ambiguous, fall through to failure
    }

    // ================================================================
    // Stage 6: FAILURE
    // ================================================================
    UE_LOG(LogOlivePinManifest, Warning,
        TEXT("FindPinSmart failed for hint '%s' (direction=%s). Available pins:"),
        *Hint, bIsInput ? TEXT("input") : TEXT("output"));
    for (const FOlivePinManifestEntry* Entry : Candidates)
    {
        UE_LOG(LogOlivePinManifest, Warning,
            TEXT("  - '%s' (display: '%s', type: %s)"),
            *Entry->PinName, *Entry->DisplayName, *Entry->TypeDisplayString);
    }

    return nullptr;
}
```

---

### `FOlivePinManifest::GetDataPins(bInput)`

```
Collect all pins where:
  bIsExec == false AND bIsInput == bInput AND bIsHidden == false

Return as TArray<const FOlivePinManifestEntry*>.
```

---

### `FOlivePinManifest::ToJson()`

Serialize the manifest to JSON. Format:

```json
{
    "step_id": "s1",
    "node_id": "node_0",
    "node_type": "CallFunction",
    "resolved_function": "PrintString",
    "is_pure": false,
    "has_exec_pins": true,
    "pins": [
        {
            "name": "execute",
            "display_name": "Execute",
            "direction": "input",
            "category": "exec",
            "is_exec": true,
            "type": "Exec"
        },
        {
            "name": "InString",
            "display_name": "In String",
            "direction": "input",
            "category": "string",
            "is_exec": false,
            "type": "String",
            "default_value": "Hello",
            "has_default": true
        }
    ]
}
```

Implementation:

```cpp
TSharedPtr<FJsonObject> FOlivePinManifest::ToJson() const
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

    Json->SetStringField(TEXT("step_id"), StepId);
    Json->SetStringField(TEXT("node_id"), NodeId);
    Json->SetStringField(TEXT("node_type"), NodeType);

    if (!ResolvedFunctionName.IsEmpty())
    {
        Json->SetStringField(TEXT("resolved_function"), ResolvedFunctionName);
    }

    Json->SetBoolField(TEXT("is_pure"), bIsPure);
    Json->SetBoolField(TEXT("has_exec_pins"), bHasExecPins);

    TArray<TSharedPtr<FJsonValue>> PinsArray;
    for (const FOlivePinManifestEntry& Entry : Pins)
    {
        // Skip hidden pins from JSON output (they clutter the AI's view)
        if (Entry.bIsHidden)
        {
            continue;
        }

        TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
        PinJson->SetStringField(TEXT("name"), Entry.PinName);
        PinJson->SetStringField(TEXT("display_name"), Entry.DisplayName);
        PinJson->SetStringField(TEXT("direction"), Entry.bIsInput ? TEXT("input") : TEXT("output"));
        PinJson->SetStringField(TEXT("category"), Entry.PinCategory);
        PinJson->SetBoolField(TEXT("is_exec"), Entry.bIsExec);
        PinJson->SetStringField(TEXT("type"), Entry.TypeDisplayString);

        if (!Entry.PinSubCategory.IsEmpty())
        {
            PinJson->SetStringField(TEXT("sub_category"), Entry.PinSubCategory);
        }

        if (Entry.bHasDefaultValue)
        {
            PinJson->SetBoolField(TEXT("has_default"), true);
            PinJson->SetStringField(TEXT("default_value"), Entry.DefaultValue);
        }

        if (Entry.bIsConnected)
        {
            PinJson->SetBoolField(TEXT("is_connected"), true);
        }

        PinsArray.Add(MakeShared<FJsonValueObject>(PinJson));
    }

    Json->SetArrayField(TEXT("pins"), PinsArray);

    return Json;
}
```

---

## UE API Reference (verified for UE 5.5)

The coder can rely on these APIs being present:

| API | Header | Usage |
|-----|--------|-------|
| `UEdGraphPin::GetName()` | `EdGraph/EdGraphPin.h` | Returns FString of internal pin name |
| `UEdGraphPin::GetDisplayName()` | `EdGraph/EdGraphPin.h` | Returns FText of display name |
| `UEdGraphPin::Direction` | `EdGraph/EdGraphPin.h` | `EGPD_Input` or `EGPD_Output` |
| `UEdGraphPin::PinType` | `EdGraph/EdGraphPin.h` | `FEdGraphPinType` struct |
| `UEdGraphPin::PinType.PinCategory` | `EdGraph/EdGraphPin.h` | `FName` -- compare to `UEdGraphSchema_K2::PC_*` |
| `UEdGraphPin::PinType.PinSubCategory` | `EdGraph/EdGraphPin.h` | `FName` subcategory |
| `UEdGraphPin::PinType.PinSubCategoryObject` | `EdGraph/EdGraphPin.h` | `TWeakObjectPtr<UObject>` |
| `UEdGraphPin::bHidden` | `EdGraph/EdGraphPin.h` | `bool` -- hidden pins |
| `UEdGraphPin::DefaultValue` | `EdGraph/EdGraphPin.h` | `FString` |
| `UEdGraphPin::AutogeneratedDefaultValue` | `EdGraph/EdGraphPin.h` | `FString` |
| `UEdGraphPin::HasAnyConnections()` | `EdGraph/EdGraphPin.h` | `bool` |
| `FEdGraphPinType::IsArray()` | `EdGraph/EdGraphPin.h` | `bool` |
| `FEdGraphPinType::IsSet()` | `EdGraph/EdGraphPin.h` | `bool` |
| `FEdGraphPinType::IsMap()` | `EdGraph/EdGraphPin.h` | `bool` |
| `UEdGraphSchema_K2::TypeToText(const FEdGraphPinType&)` | `EdGraphSchema_K2.h` | Returns `FText` |
| `UEdGraphSchema_K2::PC_Exec` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Boolean` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Byte` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Int` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Int64` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Float` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Double` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Real` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_String` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Name` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Text` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Struct` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Object` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Class` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Interface` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Enum` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Delegate` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_MCDelegate` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_SoftObject` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_SoftClass` | `EdGraphSchema_K2.h` | `static const FName` |
| `UEdGraphSchema_K2::PC_Wildcard` | `EdGraphSchema_K2.h` | `static const FName` |
| `UK2Node_CallFunction::GetTargetFunction()` | `K2Node_CallFunction.h` | Returns `UFunction*` |
| `UEdGraphNode::Pins` | `EdGraph/EdGraphNode.h` | `TArray<UEdGraphPin*>` |

---

## Build Verification

After implementation, run:
```bash
"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex
```

The build system auto-discovers files in `Blueprint/Public/Plan/` and `Blueprint/Private/Plan/` because `OliveAIEditor.Build.cs` adds recursive include paths for the `Blueprint` subdirectory.

---

## Checklist

- [ ] Created `Source/OliveAIEditor/Blueprint/Private/Plan/` directory
- [ ] `OlivePinManifest.h` matches the exact header specified above
- [ ] `OlivePinManifest.cpp` includes the exact includes listed above
- [ ] `Build()` correctly skips hidden non-exec pins
- [ ] `Build()` extracts `ResolvedFunctionName` for CallFunction nodes
- [ ] `ConvertPinTypeToIRCategory()` handles containers, struct subcategories
- [ ] `LevenshteinDistance()` uses single-row DP optimization
- [ ] `FindPinSmart()` implements all 5 stages in correct order
- [ ] `FindPinSmart()` logs available pins on failure (for debugging)
- [ ] `ToJson()` skips hidden pins from JSON output
- [ ] Build succeeds with no errors
