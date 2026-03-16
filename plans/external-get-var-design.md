# External Variable Access for plan_json get_var/set_var

**Status:** Design
**Priority:** P0
**Complexity:** Medium (3 files, ~60 lines total)
**Date:** 2026-03-14

## Problem

`plan_json`'s `get_var` and `set_var` ops only support self-context variable access (`K2Node_VariableGet::SetSelfMember()`). When the AI casts to an external class (e.g., `Cast to Character`) and needs to access a C++ UPROPERTY on that class (e.g., `Mesh`), it cannot. The typical failure cascade:

1. AI writes `{"op":"call","target":"GetMesh","inputs":{"Target":"@cast.auto"}}` -- `GetMesh()` is an inline C++ accessor, NOT a UFUNCTION, so `FindFunctionEx` fails
2. The UPROPERTY auto-rewrite in `ResolveCallOp` rewrites to `get_var(Mesh)` -- but creates a self-context node (reads from the editing Blueprint, which has no `Mesh` property)
3. The Blueprint compiles with a "variable not found" error

**Root cause:** `CreateVariableGetNode` / `CreateVariableSetNode` hardcode `SetSelfMember()`. The resolver's UPROPERTY auto-rewrite path does not propagate the external class or preserve the `Target` input reference.

**Motivating pattern:** Weapon pickup/equip -- gun actor casts OtherActor to ACharacter, needs to call `AttachToComponent` on the Character's `Mesh` (a `USkeletalMeshComponent*` UPROPERTY). Without external get_var, this is impossible through plan_json.

## Solution

Three surgical changes across the three files:

1. **NodeFactory**: If `external_class` property is present, call `SetExternalMember(VarName, ExternalClass)` instead of `SetSelfMember(VarName)`. This creates a visible Target/self input pin that can be wired.
2. **Resolver**: When UPROPERTY auto-rewrite triggers AND the original `call` step has a Target input, extract the owning class from the PROPERTY MATCH message and set `external_class` on the resolved step properties. Also preserve the Target input.
3. **Executor**: No changes needed. The existing Phase 4 Target/self pin wiring already handles PN_Self pins on any node type.

### Why No Executor Changes

When `SetExternalMember` is used, `K2Node_Variable::CreatePinForSelf()` (line 200 in K2Node_Variable.cpp) creates a VISIBLE input pin named `PN_Self` with `PinFriendlyName = "Target"`. This is the SAME pin type and name that `UK2Node_CallFunction` exposes for its self pin.

The executor's `PhaseWireData` at line 3079 already handles `Target`/`self` key remapping:
```cpp
if (ResolvedPinKey.Equals(TEXT("Target"), ESearchCase::IgnoreCase)
    || ResolvedPinKey.Equals(TEXT("self"), ESearchCase::IgnoreCase))
{
    ResolvedPinKey = TEXT("self");
}
```

And `WireDataConnection` at line 3588 has the explicit self-pin wiring path that finds `PN_Self`, handles `@step.auto` pin hints, and uses `TryCreateConnection` with BREAK_OTHERS support. This path works for ANY node with a PN_Self pin, including `K2Node_VariableGet`/`K2Node_VariableSet`.

Verified: `CreatePinForSelf` creates the pin with `UEdGraphSchema_K2::PN_Self` as the pin name (K2Node_Variable.cpp:200), exactly what the executor searches for at line 3593.

## Detailed Changes

### Change 1: OliveNodeFactory.cpp -- `CreateVariableGetNode` and `CreateVariableSetNode`

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`
**Lines:** 403-425 (GetNode), 427-448 (SetNode)

**Current code (CreateVariableGetNode, lines 419-421):**
```cpp
UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Graph);
GetNode->VariableReference.SetSelfMember(VarName);
GetNode->AllocateDefaultPins();
```

**New code:**
```cpp
UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Graph);

// Check for external class -- if present, create an external member
// reference instead of self-context. This creates a visible Target
// input pin that must be wired to provide the object context.
const FString* ExternalClassPtr = Properties.Find(TEXT("external_class"));
if (ExternalClassPtr && !ExternalClassPtr->IsEmpty())
{
    UClass* ExternalClass = FindClass(*ExternalClassPtr);
    if (ExternalClass)
    {
        GetNode->VariableReference.SetExternalMember(VarName, ExternalClass);
        UE_LOG(LogOliveNodeFactory, Log,
            TEXT("CreateVariableGetNode: external member '%s' on class '%s'"),
            *VarName.ToString(), *ExternalClass->GetName());
    }
    else
    {
        // Class not found -- fall back to self-context with warning
        UE_LOG(LogOliveNodeFactory, Warning,
            TEXT("CreateVariableGetNode: external_class '%s' not found, falling back to self-context for '%s'"),
            **ExternalClassPtr, *VarName.ToString());
        GetNode->VariableReference.SetSelfMember(VarName);
    }
}
else
{
    GetNode->VariableReference.SetSelfMember(VarName);
}

GetNode->AllocateDefaultPins();
```

**Same pattern for CreateVariableSetNode (lines 442-444):**
```cpp
UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Graph);

const FString* ExternalClassPtr = Properties.Find(TEXT("external_class"));
if (ExternalClassPtr && !ExternalClassPtr->IsEmpty())
{
    UClass* ExternalClass = FindClass(*ExternalClassPtr);
    if (ExternalClass)
    {
        SetNode->VariableReference.SetExternalMember(VarName, ExternalClass);
        UE_LOG(LogOliveNodeFactory, Log,
            TEXT("CreateVariableSetNode: external member '%s' on class '%s'"),
            *VarName.ToString(), *ExternalClass->GetName());
    }
    else
    {
        UE_LOG(LogOliveNodeFactory, Warning,
            TEXT("CreateVariableSetNode: external_class '%s' not found, falling back to self-context for '%s'"),
            **ExternalClassPtr, *VarName.ToString());
        SetNode->VariableReference.SetSelfMember(VarName);
    }
}
else
{
    SetNode->VariableReference.SetSelfMember(VarName);
}

SetNode->AllocateDefaultPins();
```

**Notes:**
- `FindClass` is already available on `FOliveNodeFactory` (used elsewhere, e.g., `CreateSpawnActorNode`).
- Fallback to `SetSelfMember` on class-not-found is safe -- the node will compile with a "variable not found" warning, which is the same behavior as today. No worse than the status quo.
- `AllocateDefaultPins()` MUST be called AFTER `SetExternalMember`/`SetSelfMember`, because `CreatePinForSelf` checks `IsSelfContext()` to determine whether the self pin should be hidden or visible.

### Change 2: OliveBlueprintPlanResolver.cpp -- UPROPERTY auto-rewrite in `ResolveCallOp`

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**Lines:** 2040-2089 (UPROPERTY auto-rewrite block)

**Current behavior:** When `PROPERTY MATCH` is detected, the resolver rewrites the step to `get_var`/`set_var` with `variable_name`. But it does NOT:
1. Set `external_class` on the resolved step properties
2. Preserve the `Target` input from the original `call` step

**Problem:** The `PROPERTY MATCH` message already contains both the property name AND the owning class name. Format (from OliveNodeFactory.cpp line 3197-3199):
```
"PROPERTY MATCH: 'Mesh' is a USkeletalMeshComponent* property on Character, not a callable function."
```

The class name is between "on " and the comma.

**Also:** The original call step may have `"Target": "@cast.auto"` in its Inputs. When rewriting from `call` to `get_var`, we must check if the owning class differs from the editing Blueprint's class hierarchy. If it does, this is an external property access and we need to set `external_class` and ensure the Target input is preserved on the rewritten step.

**Detection logic for when external_class is needed:**

The UPROPERTY match scans the editing Blueprint's GeneratedClass, parent chain, SCS components, AND the specified target_class. Not all matches require external access. External access is needed when:
- The property is on a class that is NOT in the editing Blueprint's class hierarchy (e.g., `Mesh` on `ACharacter` when editing a `BP_Gun` that inherits from `AActor`)
- AND the step has a `Target` input (meaning the AI is referencing an external object)

The simplest correct heuristic: **if the original `call` step has a Target input, assume external access.** The AI wrote `Target` because it knows it's calling on an external object. If the AI writes `get_var(Mesh)` without Target, it means self-context (and might fail at compile, which is the existing behavior).

**New code (replace lines 2040-2089):**

```cpp
// --- UPROPERTY auto-rewrite ---
// When FindFunctionEx fails but detects a matching UPROPERTY, the AI
// intended to set/get a property, not call a function. Rewrite the op.
for (const FString& Location : SearchResult.SearchedLocations)
{
    if (Location.StartsWith(TEXT("PROPERTY MATCH:")))
    {
        // Extract property name from the PROPERTY MATCH message.
        // Format: "PROPERTY MATCH: 'PropName' is a Type property on ClassName..."
        // Parse the name between single quotes.
        int32 FirstQuote = Location.Find(TEXT("'"));
        int32 SecondQuote = Location.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstQuote + 1);
        if (FirstQuote == INDEX_NONE || SecondQuote == INDEX_NONE)
        {
            continue;
        }

        FString PropertyName = Location.Mid(FirstQuote + 1, SecondQuote - FirstQuote - 1);

        // Determine set vs get based on Step.Target prefix and presence of non-target Inputs.
        // Exclude "Target" and "self" keys -- those are object references, not value inputs.
        int32 NonTargetInputCount = 0;
        bool bHasTargetInput = false;
        FString TargetInputValue;  // preserve for external wiring
        for (const auto& Pair : Step.Inputs)
        {
            if (Pair.Key.Equals(TEXT("Target"), ESearchCase::IgnoreCase) ||
                Pair.Key.Equals(TEXT("self"), ESearchCase::IgnoreCase))
            {
                bHasTargetInput = true;
                TargetInputValue = Pair.Value;
            }
            else
            {
                NonTargetInputCount++;
            }
        }
        bool bIsSet = Step.Target.StartsWith(TEXT("Set"), ESearchCase::IgnoreCase)
                   || NonTargetInputCount > 0;

        Out.NodeType = bIsSet ? OliveNodeTypes::SetVariable : OliveNodeTypes::GetVariable;
        Out.Properties.Add(TEXT("variable_name"), PropertyName);
        Out.bIsPure = !bIsSet;

        // --- External class detection ---
        // If the original call step had a Target input, the AI is accessing
        // a property on an external object (e.g., cast output). Extract the
        // owning class from the PROPERTY MATCH message and set external_class.
        if (bHasTargetInput && !TargetInputValue.IsEmpty())
        {
            // Extract class name from "...property on ClassName, not..."
            // Find " on " before the class name
            int32 OnIdx = Location.Find(TEXT(" on "), ESearchCase::IgnoreCase);
            if (OnIdx != INDEX_NONE)
            {
                FString AfterOn = Location.Mid(OnIdx + 4); // skip " on "
                // Class name ends at comma or period
                int32 EndIdx = AfterOn.Find(TEXT(","));
                if (EndIdx == INDEX_NONE)
                {
                    EndIdx = AfterOn.Find(TEXT("."));
                }
                if (EndIdx != INDEX_NONE)
                {
                    FString ClassName = AfterOn.Left(EndIdx).TrimStartAndEnd();
                    if (!ClassName.IsEmpty())
                    {
                        Out.Properties.Add(TEXT("external_class"), ClassName);

                        Out.ResolverNotes.Add(FOliveResolverNote{
                            TEXT("external_class"),
                            TEXT("self"),
                            ClassName,
                            FString::Printf(TEXT("Property '%s' is on external class '%s'. "
                                "Target input preserved for wiring."),
                                *PropertyName, *ClassName)
                        });
                    }
                }
            }
        }

        Out.ResolverNotes.Add(FOliveResolverNote{
            TEXT("op"),
            OlivePlanOps::Call,
            bIsSet ? OlivePlanOps::SetVar : OlivePlanOps::GetVar,
            FString::Printf(TEXT("'%s' is a property, not a function. Rewritten from 'call' to '%s'."),
                *Step.Target, bIsSet ? *OlivePlanOps::SetVar : *OlivePlanOps::GetVar)
        });

        Warnings.Add(FString::Printf(
            TEXT("Step '%s': '%s' auto-rewritten from call to %s (UPROPERTY detected%s)"),
            *Step.StepId, *Step.Target, bIsSet ? TEXT("set_var") : TEXT("get_var"),
            bHasTargetInput ? TEXT(", external") : TEXT("")));

        return true; // Step is now resolved
    }
}
```

**Key points:**
- The `Target` input is NOT removed or modified. It stays in `Step.Inputs` and flows naturally to `PhaseWireData`.
- The resolver only adds `external_class` to `Out.Properties` -- the factory uses it, the executor ignores it (it only cares about Inputs, not Properties).
- This is a REWRITE of the existing lines 2040-2089. The code prior to the `bHasTargetInput` check is structurally the same, just with the variable `bHasTargetInput` and `TargetInputValue` tracking added.

### Change 2b: ResolveGetVarOp -- Support explicit `external_class` in plan step Properties

When the AI writes `get_var` directly (not via call auto-rewrite) with an external class, the resolver should pass it through. This is a 3-line addition.

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**Location:** Inside `ResolveGetVarOp`, after line 2334 (`Out.Properties.Add(TEXT("variable_name"), Step.Target);`)

**Add after line 2334:**
```cpp
// Pass through external_class if the AI specified it in Properties.
// This enables explicit external variable access: {"op":"get_var","target":"Mesh","properties":{"external_class":"Character"}}
const FString* ExternalClass = Step.Properties.Find(TEXT("external_class"));
if (ExternalClass && !ExternalClass->IsEmpty())
{
    Out.Properties.Add(TEXT("external_class"), *ExternalClass);
}
```

**Same pattern for ResolveSetVarOp**, after line 2454 (`Out.Properties.Add(TEXT("variable_name"), Step.Target);`):
```cpp
const FString* ExternalClass = Step.Properties.Find(TEXT("external_class"));
if (ExternalClass && !ExternalClass->IsEmpty())
{
    Out.Properties.Add(TEXT("external_class"), *ExternalClass);
}
```

### Change 2c: Accessor-to-Property Detection in ResolveCallOp (Pre-UPROPERTY Rewrite)

The existing UPROPERTY auto-rewrite depends on `FindFunctionEx` failing AND its internal PROPERTY MATCH scan finding the property. But there's a gap: the PROPERTY MATCH scan in `FindFunctionEx` only scans the editing Blueprint's class hierarchy and SCS components. When the AI calls `GetMesh` on a cast-to-Character step, the editing Blueprint might be a `BP_Gun` (inherits from AActor). `AActor` does NOT have a `Mesh` property, so PROPERTY MATCH won't fire.

**The fix:** Before the UPROPERTY auto-rewrite, add a pre-check that scans the CAST TARGET CLASS for the property. This uses the same CastTargetMap fallback path that already exists for functions.

**Location:** In `ResolveCallOp`, between the cast-target function search (lines 1936-2037) and the UPROPERTY auto-rewrite (line 2040). Insert a new block.

**New code (insert between lines 2037-2040):**
```cpp
// --- Accessor-to-property detection on cast target class ---
// When the AI calls Get{Property} or Set{Property} on a cast target, and
// FindFunctionEx failed, check if the property exists on the cast target class.
// This catches C++ inline accessors like GetMesh() -> Mesh UPROPERTY on Character.
if (CastTargetMap.Num() > 0)
{
    for (const auto& InputPair : Step.Inputs)
    {
        const FString& InputValue = InputPair.Value;
        if (!InputValue.StartsWith(TEXT("@"))) continue;
        if (!InputPair.Key.Equals(TEXT("Target"), ESearchCase::IgnoreCase)
            && !InputPair.Key.Equals(TEXT("self"), ESearchCase::IgnoreCase)) continue;

        // Parse the @ref step ID
        FString RefBody = InputValue.Mid(1);
        FString RefStepId;
        int32 DotIdx;
        if (RefBody.FindChar(TEXT('.'), DotIdx))
            RefStepId = RefBody.Left(DotIdx);
        else
            RefStepId = RefBody;

        const FString* CastClassName = CastTargetMap.Find(RefStepId);
        if (!CastClassName || CastClassName->IsEmpty()) continue;

        FOliveClassResolveResult ClassResolve = FOliveClassResolver::Resolve(*CastClassName);
        if (!ClassResolve.IsValid()) continue;

        // Strip Get/Set prefix to find property candidate
        FString PropertyCandidate = Step.Target;
        bool bIsSetPrefix = false;
        if (PropertyCandidate.StartsWith(TEXT("Set")) && PropertyCandidate.Len() > 3
            && FChar::IsUpper(PropertyCandidate[3]))
        {
            PropertyCandidate = PropertyCandidate.Mid(3);
            bIsSetPrefix = true;
        }
        else if (PropertyCandidate.StartsWith(TEXT("Get")) && PropertyCandidate.Len() > 3
                 && FChar::IsUpper(PropertyCandidate[3]))
        {
            PropertyCandidate = PropertyCandidate.Mid(3);
        }

        // Also try the raw name (e.g., AI wrote "Mesh" directly as a call target)
        TArray<FString> Candidates = { PropertyCandidate, Step.Target };

        // Scan cast target class for matching BlueprintVisible property
        for (TFieldIterator<FProperty> PropIt(ClassResolve.Class, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
        {
            const FProperty* Prop = *PropIt;
            if (!Prop) continue;
            if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly)) continue;

            for (const FString& Cand : Candidates)
            {
                if (Prop->GetName().Equals(Cand, ESearchCase::IgnoreCase))
                {
                    // Found the property on the cast target class.
                    // Determine set vs get.
                    int32 NonTargetInputCount = 0;
                    for (const auto& P : Step.Inputs)
                    {
                        if (!P.Key.Equals(TEXT("Target"), ESearchCase::IgnoreCase)
                            && !P.Key.Equals(TEXT("self"), ESearchCase::IgnoreCase))
                            NonTargetInputCount++;
                    }
                    bool bIsSet = bIsSetPrefix || NonTargetInputCount > 0;

                    Out.NodeType = bIsSet ? OliveNodeTypes::SetVariable : OliveNodeTypes::GetVariable;
                    Out.Properties.Add(TEXT("variable_name"), Prop->GetName());
                    Out.Properties.Add(TEXT("external_class"), ClassResolve.Class->GetName());
                    Out.bIsPure = !bIsSet;

                    Out.ResolverNotes.Add(FOliveResolverNote{
                        TEXT("op"),
                        OlivePlanOps::Call,
                        bIsSet ? OlivePlanOps::SetVar : OlivePlanOps::GetVar,
                        FString::Printf(TEXT("'%s' is a UPROPERTY '%s' on cast target '%s'. "
                            "Rewritten from call to external %s."),
                            *Step.Target, *Prop->GetName(),
                            *ClassResolve.Class->GetName(),
                            bIsSet ? TEXT("set_var") : TEXT("get_var"))
                    });

                    Warnings.Add(FString::Printf(
                        TEXT("Step '%s': '%s' -> external %s('%s') on %s (UPROPERTY on cast target)"),
                        *Step.StepId, *Step.Target,
                        bIsSet ? TEXT("set_var") : TEXT("get_var"),
                        *Prop->GetName(), *ClassResolve.Class->GetName()));

                    UE_LOG(LogOlivePlanResolver, Log,
                        TEXT("    ResolveCallOp: '%s' -> external %s('%s') on %s (UPROPERTY on cast target)"),
                        *Step.Target,
                        bIsSet ? TEXT("set_var") : TEXT("get_var"),
                        *Prop->GetName(), *ClassResolve.Class->GetName());

                    return true;
                }
            }
        }
    }
}
```

**Why this is needed:** Without this block, when editing `BP_Gun` and calling `GetMesh` with `Target=@cast_to_character.auto`:
1. `FindFunctionEx("GetMesh", "", BP_Gun)` fails -- `GetMesh` is not a UFUNCTION
2. Cast-target function search (lines 1936-2037): `FindFunctionEx("GetMesh", "Character", nullptr)` -- also fails because `GetMesh` is not a UFUNCTION on any class
3. PROPERTY MATCH scan in `FindFunctionEx` only scans BP_Gun's class hierarchy + its SCS components -- AActor doesn't have `Mesh`, so no PROPERTY MATCH
4. Falls through to FUNCTION_NOT_FOUND error

With this block, after step 2 fails, we check: "Is `GetMesh` actually `Get` + `Mesh` where `Mesh` is a UPROPERTY on ACharacter?" Yes -- rewrite to external get_var.

## Data Flow

```
AI writes: {"op":"call", "target":"GetMesh", "inputs":{"Target":"@cast.auto"}}

                                  |
                                  v
              ResolveCallOp: FindFunctionEx("GetMesh") -> FAIL
                                  |
                                  v
              Cast-target function search -> FAIL (not a UFUNCTION)
                                  |
                                  v
        [NEW] Accessor-to-property on cast target:
              Strip "Get" -> "Mesh", scan ACharacter -> FOUND
                                  |
                                  v
              Out.NodeType = GetVariable
              Out.Properties["variable_name"] = "Mesh"
              Out.Properties["external_class"] = "Character"
              Out.bIsPure = true
              Step.Inputs["Target"] = "@cast.auto" (PRESERVED)
                                  |
                                  v
              Phase 1: CreateVariableGetNode(Props{variable_name="Mesh", external_class="Character"})
              -> SetExternalMember(FName("Mesh"), ACharacter::StaticClass())
              -> AllocateDefaultPins() creates VISIBLE PN_Self input pin
                                  |
                                  v
              Phase 4: PhaseWireData processes Step.Inputs["Target"] = "@cast.auto"
              -> ResolvedPinKey remapped to "self" (line 3082)
              -> WireDataConnection enters self-pin path (line 3588)
              -> Finds PN_Self on the GetVariable node
              -> Wires cast output to PN_Self via TryCreateConnection
                                  |
                                  v
              Result: UK2Node_VariableGet for "Mesh" on Character,
              with Target pin wired to cast output. Compiles correctly.
```

## Edge Cases

### 1. Property doesn't exist on the target class
If the AI writes `{"op":"get_var","target":"FakeProperty","properties":{"external_class":"Character"}}`, `FindClass("Character")` succeeds but there is no `FakeProperty` on ACharacter. The `SetExternalMember` call still succeeds (it's just setting a reference), but `AllocateDefaultPins -> CreatePinForVariable` will fail to find the property and return false. The node will have only a self pin and no output pin, causing a compile error. This is the same failure mode as `SetSelfMember` with a bad variable name -- no regression.

### 2. Class not found for external_class
`FindClass` returns nullptr. We fall back to `SetSelfMember` with a warning log. The node may compile or not depending on whether the variable exists on self. This is defense-in-depth -- the resolver should only emit `external_class` for classes it has already resolved via `FOliveClassResolver::Resolve`.

### 3. Self-context get_var (no external_class)
Existing behavior unchanged. `CreateVariableGetNode` only checks `Properties.Find("external_class")` -- if not present, falls through to `SetSelfMember`.

### 4. AI writes get_var with Target but no external_class
`PhaseWireData` processes `Target` -> `self` pin wiring. But `CreateVariableGetNode` used `SetSelfMember`, so the self pin is hidden. `WireDataConnection` will find the PN_Self pin (hidden pins are still findable via `FindPin`), and `TryCreateConnection` will succeed. However, the node is configured for self-context, so the pin connection would be meaningless (the compiler reads from self regardless). This is a degenerate case -- the AI shouldn't write `Target` on a self-context get_var, and the self pin is hidden precisely because it's auto-provided.

### 5. PROPERTY MATCH fires on self-context (editing Blueprint's own class hierarchy)
Example: AI calls `GetMesh` while editing a Blueprint that inherits from `ACharacter`. `FindFunctionEx` scans ACharacter's properties and finds `Mesh`. PROPERTY MATCH fires. The UPROPERTY auto-rewrite has `bHasTargetInput = false` (AI didn't write Target because it's self-context). No `external_class` is set. `CreateVariableGetNode` uses `SetSelfMember("Mesh")` -- correct, the variable exists on self.

### 6. get_var for SCS components on external objects
SCS component variables (e.g., `CollisionComp`) are only valid on self-context. External objects don't expose another Blueprint's SCS components as properties. The existing `BlueprintHasVariable` check would warn, and `SetExternalMember` would create a pin that fails at compile. This is correct behavior -- you can't access another Blueprint's SCS components by name.

### 7. set_var with external_class
Same mechanism as get_var. `CreateVariableSetNode` gets the same `external_class` treatment. AI writes `{"op":"set_var","target":"Health","properties":{"external_class":"Character"},"inputs":{"Target":"@cast.auto","value":"100"}}`. Phase 4 wires both the Target (self pin) and the value input. Note: most C++ UPROPERTYs are read-only from Blueprint, so set_var on external properties will often compile-error. This is expected and not our problem to solve.

### 8. Interaction with Phase 0 VARIABLE_NOT_FOUND validation
Phase 0 (`FOlivePlanValidator`) checks that get_var/set_var targets exist on the Blueprint. For external get_var, the variable does NOT exist on the editing Blueprint. We need to ensure Phase 0 skips the check when `external_class` is present.

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp`
**Required change:** In the VARIABLE_NOT_FOUND check, skip validation when the resolved step has `external_class` property set.

Let me verify the current Phase 0 code.

## Phase 0 Validator Change

### Change 3: OlivePlanValidator.cpp -- Skip VARIABLE_NOT_FOUND for external variables

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp`
**Location:** Inside `CheckVariableNotFound`, after line 508 (`const FString& VariableName = *VarNamePtr;`), before the self-context checks begin at line 510.

**Insert after line 508:**
```cpp
// Skip external variable access -- the property exists on the external
// class, not on the editing Blueprint. The resolver already validated
// the class and property via FOliveClassResolver + TFieldIterator.
const FString* ExternalClassPtr = Resolved.Properties.Find(TEXT("external_class"));
if (ExternalClassPtr && !ExternalClassPtr->IsEmpty())
{
    continue;
}
```

**Rationale:** The VARIABLE_NOT_FOUND check scans the editing Blueprint's NewVariables, SCS, parent chain, GeneratedClass, and WidgetTree. None of these will contain properties from an external class (e.g., `Mesh` on ACharacter when editing BP_Gun). Without this guard, every external get_var/set_var would trigger a false-positive VARIABLE_NOT_FOUND error. The resolver already validated that the property exists via `TFieldIterator<FProperty>` on the resolved class.

Note: The existing "cross-BP variable" note at line 621 (which detects variables on cast target classes) becomes dead code for external get_var steps, since we skip before reaching it. This is correct -- the note was a workaround for the lack of external get_var support, telling the AI to "add a pure getter function instead." Now the AI can access the property directly.

## Implementation Order

**4 files, ~120 lines total.**

1. **OliveNodeFactory.cpp** (~20 lines per function, ~40 total) -- Add `external_class` property handling to `CreateVariableGetNode` and `CreateVariableSetNode`. This is a pure extension with no behavioral change for existing code. Start here because it's the foundation the other changes depend on.

2. **OlivePlanValidator.cpp** (~6 lines) -- Skip VARIABLE_NOT_FOUND for steps with `external_class` property. Do this early so it doesn't block testing.

3. **OliveBlueprintPlanResolver.cpp** (~70 lines total):
   - **Change 2c** first: Add accessor-to-property detection on cast target class (insert between lines 2037-2040). This is the critical path for the `GetMesh` scenario.
   - **Change 2** second: Extend the existing UPROPERTY auto-rewrite to detect `bHasTargetInput` and extract `external_class` from the PROPERTY MATCH message. This handles the case where the property IS found on the editing Blueprint's class hierarchy (e.g., `GetMesh` when editing a Character-derived Blueprint with a cast to another Character).
   - **Change 2b** third: Pass through `external_class` from step Properties in `ResolveGetVarOp` and `ResolveSetVarOp`. This enables the AI to use the feature directly without going through `call` auto-rewrite.

4. **Build and test** -- Verify existing get_var behavior unchanged, then test the GetMesh-on-cast-to-Character scenario end to end.

## Testing Checklist

- [ ] `{"op":"call","target":"GetMesh","inputs":{"Target":"@cast.auto"}}` on BP_Gun (AActor child) with cast to Character -- should create external VariableGet for Mesh wired to cast output
- [ ] `{"op":"get_var","target":"Mesh","properties":{"external_class":"Character"},"inputs":{"Target":"@cast.auto"}}` -- explicit external get_var
- [ ] `{"op":"get_var","target":"MyHealth"}` (no external_class) -- existing self-context behavior unchanged
- [ ] `{"op":"call","target":"SetActorLocation","inputs":{"NewLocation":"..."}}` -- existing call resolution unaffected
- [ ] Phase 0 validator does not reject external get_var steps
- [ ] Compile succeeds when external property exists on target class
- [ ] Compile fails gracefully when external property does NOT exist (bad property name)
