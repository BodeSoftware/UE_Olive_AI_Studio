# Run 09r Fixes Design

**Author:** Architect
**Date:** 2026-03-09
**Implementation order:** Fix 1 -> Fix 2 -> Fix 3

---

## Fix 1: Restructure FUNCTION_NOT_FOUND Error Format

**Priority:** P0 (highest impact)
**Assignee:** Junior
**Estimated lines changed:** ~40
**Files modified:** 1

### Problem

The FUNCTION_NOT_FOUND error puts suggestions AFTER a long search trail string. The agent sees:

```
message: "Function 'SetVelocityInLocalSpace' not found. Searched: specified class + Blueprint GeneratedClass + ... + K2_ fuzzy match (18 searched classes)."
suggestion: "Did you mean: SetPhysicsAngularVelocityInRadians, ..."
```

The `message` field is a dense wall of text. The `suggestion` field has the actionable content but the agent ignores it -- likely because it reads the `message`, sees no clear alternative, and retries with a different guess.

### Root Cause

In `OliveBlueprintPlanResolver.cpp` lines 1742-1886, the error is constructed with `Message` containing the search trail and `Suggestion` containing the alternatives. Both `FOliveIRBlueprintPlanError::ToJson()` serializes these as separate JSON fields, so the agent sees them as:

```json
{
  "error_code": "FUNCTION_NOT_FOUND",
  "message": "Function 'X' not found. Searched: [200+ chars of trail]",
  "suggestion": "Did you mean: A, B, C. Specify target_class to narrow..."
}
```

### Fix

Restructure so `message` is short and `suggestion` leads with alternatives.

**File: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`**

**Change 1 (line 1746-1748):** Replace the ErrorMessage construction.

Current:
```cpp
FString ErrorMessage = FString::Printf(
    TEXT("Function '%s' not found. Searched: %s."),
    *Step.Target, *SearchResult.BuildSearchedLocationsString());
```

New:
```cpp
// Short, scannable message -- suggestions go in the Suggestion field
FString ErrorMessage;
if (!Step.TargetClass.IsEmpty())
{
    ErrorMessage = FString::Printf(
        TEXT("Function '%s' not found on class '%s'."),
        *Step.Target, *Step.TargetClass);
}
else
{
    ErrorMessage = FString::Printf(
        TEXT("Function '%s' not found on any searched class."),
        *Step.Target);
}
```

**Change 2 (lines 1822-1871):** Restructure the Suggestion block so alternatives lead, advice follows, and search trail goes at the end.

Current flow:
1. ScopedSuggestions or catalog fuzzy -> sets `Suggestion`
2. Append target_class advice

New flow:
1. Build alternatives list (same logic, no change to ScopedSuggestions or catalog lookup)
2. Build `Suggestion` with alternatives FIRST
3. Append verification advice (describe_function)
4. Append UPROPERTY advice if BuildScopedSuggestions detected property matches
5. Append search trail as a `--- Debug trail ---` section at the end

Replace lines 1822-1871 with:

```cpp
// Build actionable suggestion -- alternatives come FIRST
FString Suggestion;

if (SuggestionClass)
{
    // Class-scoped suggestions via FOliveClassAPIHelper
    const FString ScopedSuggestions = FOliveClassAPIHelper::BuildScopedSuggestions(
        SuggestionClass, Step.Target);

    if (!ScopedSuggestions.IsEmpty())
    {
        Suggestion = FString::Printf(TEXT("On class '%s': %s"),
            *SuggestionClass->GetName(), *ScopedSuggestions);
    }
}

// Fallback to catalog fuzzy match if no class-scoped suggestions
if (Suggestion.IsEmpty())
{
    FOliveNodeCatalog& Catalog = FOliveNodeCatalog::Get();
    if (Catalog.IsInitialized())
    {
        TArray<FOliveNodeSuggestion> CatalogSuggestions = Catalog.FuzzyMatch(Step.Target, CATALOG_SEARCH_LIMIT);
        for (const FOliveNodeSuggestion& CatalogSuggestion : CatalogSuggestions)
        {
            Alternatives.Add(CatalogSuggestion.DisplayName);
        }
    }

    if (Alternatives.Num() > 0)
    {
        Suggestion = FString::Printf(
            TEXT("Suggested alternatives: %s."),
            *FString::Join(Alternatives, TEXT(", ")));
    }
}

// Append verification advice
Suggestion += Suggestion.IsEmpty() ? TEXT("") : TEXT(" ");
Suggestion += TEXT("Verify with blueprint.describe_function(function_name, target_class) before retrying.");

// Check if BuildScopedSuggestions detected a property cross-match
// (this is separate from the UPROPERTY auto-rewrite which fires earlier)
if (SuggestionClass)
{
    // Scan for property matches that the class-scoped helper might have found
    for (TFieldIterator<FProperty> PropIt(SuggestionClass, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
    {
        const FProperty* Prop = *PropIt;
        if (!Prop || !Prop->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly))
        {
            continue;
        }
        // Check if the failed target is a plausible Set/Get attempt on this property
        FString CleanTarget = Step.Target;
        CleanTarget.RemoveFromStart(TEXT("Set"), ESearchCase::IgnoreCase);
        CleanTarget.RemoveFromStart(TEXT("Get"), ESearchCase::IgnoreCase);
        if (CleanTarget.Equals(Prop->GetName(), ESearchCase::IgnoreCase))
        {
            Suggestion += FString::Printf(
                TEXT(" NOTE: '%s' is a UPROPERTY on %s, not a function. Use op: set_var or op: get_var with target: '%s'."),
                *Prop->GetName(), *SuggestionClass->GetName(), *Prop->GetName());
            break;
        }
    }
}

// Target class advice (unchanged logic, condensed)
if (!Step.TargetClass.IsEmpty())
{
    Suggestion += FString::Printf(
        TEXT(" Check if '%s' is the correct target_class, or omit it to search all scopes."),
        *Step.TargetClass);
}
else
{
    Suggestion += TEXT(" Specify target_class to narrow the search.");
}

// Append search trail as debug context (after a clear delimiter)
Suggestion += FString::Printf(TEXT(" --- Search trail: %s"),
    *SearchResult.BuildSearchedLocationsString());
```

### What Changes for the Agent

Before:
```json
{
  "message": "Function 'SetVelocityInLocalSpace' not found. Searched: specified class + BP GeneratedClass + ... (18 classes).",
  "suggestion": "Did you mean: SetPhysicsAngularVelocityInRadians, ... Specify target_class to narrow the search."
}
```

After:
```json
{
  "message": "Function 'SetVelocityInLocalSpace' not found on any searched class.",
  "suggestion": "Suggested alternatives: SetPhysicsAngularVelocityInRadians, SetPhysicsLinearVelocity. Verify with blueprint.describe_function(function_name, target_class) before retrying. NOTE: 'VelocityInLocalSpace' is a UPROPERTY on UMovementComponent, not a function. Use op: set_var or op: get_var with target: 'VelocityInLocalSpace'. Specify target_class to narrow the search. --- Search trail: specified class + ..."
}
```

The agent now sees:
1. Short message stating the failure
2. Concrete alternatives FIRST in suggestion
3. Verification tool recommendation
4. UPROPERTY hint if applicable
5. Search trail only as debug context at the end

### Edge Cases

- **No alternatives found**: Suggestion starts with "Verify with blueprint.describe_function..." (still better than empty)
- **ScopedSuggestions vs catalog**: Priority unchanged (scoped first, catalog fallback)
- **UPROPERTY auto-rewrite already fired**: The code at lines 1700-1740 returns `true` before reaching this block, so the property note in the error is only for cases where auto-rewrite did NOT fire (property on a component class that wasn't in FindFunctionEx's scan path)

---

## Fix 2: Reframe describe_function in Prompts

**Priority:** P1
**Assignee:** Junior
**Estimated lines changed:** ~15
**Files modified:** 2

### Problem

`blueprint.describe_function` is framed as a "pin name verification" tool everywhere. The agent never calls it because it uses `@step.auto` for pin wiring. But it DOES need to verify whether functions exist (wrong function names cause FUNCTION_NOT_FOUND errors), and no prompt tells it to use describe_function for that purpose.

### Changes

**File 1: `Content/SystemPrompts/Knowledge/cli_blueprint.txt` (lines 104-113)**

Replace:
```
## Pin Name Verification

Verify pin names when possible -- check template references first, then Component API Reference, then describe_node_type. If none of these have the answer, trust your UE5 knowledge and use `@step.auto` for pin wiring -- the resolver handles standard pin patterns automatically. Don't simplify your design because you couldn't verify a pin name.

Priority order:
1. Template references (`blueprint.get_template` with pattern) -- highest quality, full node graphs
2. blueprint.describe_function(class, function) -- exact function signatures
3. Functions you added yourself -- pin names match the parameter names from `blueprint.add_function`
4. `@step.auto` -- automatic type-based pin matching, works for ~80% of connections
5. Your UE5 knowledge -- you know Unreal. If you know a pin name, use it.
```

With:
```
## Function Verification

Before using unfamiliar functions in plan_json (especially component-specific ones like SetVelocityInLocalSpace, SetSimulatePhysics, etc.), verify they exist:

1. Template references (`blueprint.get_template` with pattern) -- highest quality, full node graphs
2. `blueprint.describe_function(function_name, target_class)` -- confirms function exists, returns exact pin names
3. Your UE5 knowledge -- you know Unreal. Common functions (SetActorLocation, GetActorTransform, etc.) don't need verification.

For pin wiring, use `@step.auto` -- the resolver handles ~80% of connections automatically. Functions you created with `blueprint.add_function` use the parameter names you specified.
```

**File 2: `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`**

**Line 410:** Change:
```cpp
AgentContext += TEXT("- `blueprint.describe_function(class, function)` -- verify exact pin names\n");
```
To:
```cpp
AgentContext += TEXT("- `blueprint.describe_function(function_name, target_class)` -- verify function exists and get pin signatures\n");
```

**Line 663:** Change:
```cpp
EffectiveMessage += TEXT("Use blueprint.describe_function to verify pin names when unsure.\n");
```
To:
```cpp
EffectiveMessage += TEXT("If unsure whether a UE function exists (e.g., component-specific functions), verify with blueprint.describe_function before writing plan_json.\n");
```

### Design Constraint

describe_function is NOT a mandatory pre-step. It's for UNFAMILIAR functions -- ones the agent isn't confident about. Common UE functions (SetActorLocation, AddActorLocalOffset, etc.) should not require verification.

---

## Fix 3: SpawnActor Step-Reference Support

**Priority:** P0
**Assignee:** Senior
**Estimated lines changed:** ~50
**Files modified:** 2

### Problem

When the AI writes:
```json
{"op": "spawn_actor", "target": "@get_class.auto", "id": "spawn_step"}
```

The resolver passes `"@get_class.auto"` through as `actor_class` property. Then `CreateSpawnActorNode` calls `FindClass("@get_class.auto")` which fails because `@` references are step references, not class paths. The class should be wired dynamically via Phase 4 data wiring.

This is a legitimate UE Blueprint pattern: `SpawnActor from Class` with its Class pin wired from a variable, cast output, or other node.

### Architecture Decision: Option A (Resolver Intercept)

The resolver is the single resolution authority (per the resolver-executor contract). The resolver should detect `@` references in spawn_actor targets and handle them appropriately, rather than pushing unknown syntax to the executor.

### Changes

**File 1: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`**

In the `SpawnActor` handler block (lines 1267-1276), add `@` reference detection before setting `actor_class`:

Current:
```cpp
else if (Op == OlivePlanOps::SpawnActor)
{
    UE_LOG(LogOlivePlanResolver, Log, TEXT("    ResolveSpawnActorOp: target='%s'"), *Step.Target);
    bResult = ResolveSimpleOp(Step, OliveNodeTypes::SpawnActor, OutResolved);
    if (bResult)
    {
        OutResolved.Properties.Add(TEXT("actor_class"), Step.Target);
        UE_LOG(LogOlivePlanResolver, Log, TEXT("    ResolveSpawnActorOp: actor_class='%s' resolved successfully"), *Step.Target);
    }
}
```

New:
```cpp
else if (Op == OlivePlanOps::SpawnActor)
{
    UE_LOG(LogOlivePlanResolver, Log, TEXT("    ResolveSpawnActorOp: target='%s'"), *Step.Target);
    bResult = ResolveSimpleOp(Step, OliveNodeTypes::SpawnActor, OutResolved);
    if (bResult)
    {
        if (Step.Target.StartsWith(TEXT("@")))
        {
            // Dynamic class reference -- the class will be wired via Phase 4 data wiring.
            // Use AActor as placeholder so CreateSpawnActorNode can create a valid node.
            // Store the step reference so Phase 4 can wire the Class pin.
            OutResolved.Properties.Add(TEXT("actor_class"), TEXT("Actor"));
            OutResolved.Properties.Add(TEXT("dynamic_class_ref"), Step.Target);

            OutResolved.ResolverNotes.Add(FOliveResolverNote{
                TEXT("target"),
                Step.Target,
                TEXT("Actor (dynamic)"),
                FString::Printf(TEXT("spawn_actor target '%s' is a step reference. "
                    "Using AActor as placeholder; Class pin will be wired in Phase 4."),
                    *Step.Target)
            });

            UE_LOG(LogOlivePlanResolver, Log,
                TEXT("    ResolveSpawnActorOp: target '%s' is a step reference — using AActor placeholder, Phase 4 will wire Class pin"),
                *Step.Target);
        }
        else
        {
            OutResolved.Properties.Add(TEXT("actor_class"), Step.Target);
            UE_LOG(LogOlivePlanResolver, Log,
                TEXT("    ResolveSpawnActorOp: actor_class='%s' resolved successfully"), *Step.Target);
        }
    }
}
```

**File 2: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`**

In `PhaseWireData`, add handling for `dynamic_class_ref`. This must inject an additional data wire from the referenced step to the SpawnActor node's Class pin.

Add after the existing `for (const auto& InputPair : Step.Inputs)` loop, inside the outer `for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)` loop:

```cpp
// --- Dynamic class pin wiring for spawn_actor ---
// When the resolver detected a step-reference target (e.g., "@get_class.auto"),
// it stored the reference in the resolved step's properties as "dynamic_class_ref".
// Wire it now to the SpawnActor node's Class pin.
if (Step.Op == OlivePlanOps::SpawnActor)
{
    // Look up the resolved step to check for dynamic_class_ref
    const FOliveResolvedStep* Resolved = nullptr;
    for (const FOliveResolvedStep& RS : Context.ResolvedSteps)
    {
        if (RS.StepId == Step.StepId)
        {
            Resolved = &RS;
            break;
        }
    }

    if (Resolved)
    {
        const FString* DynRef = Resolved->Properties.Find(TEXT("dynamic_class_ref"));
        if (DynRef && !DynRef->IsEmpty())
        {
            UEdGraphNode* SpawnNode = Context.GetNodePtr(Step.StepId);
            if (SpawnNode)
            {
                // Find the Class pin on the SpawnActor node
                UK2Node_SpawnActorFromClass* TypedSpawn = Cast<UK2Node_SpawnActorFromClass>(SpawnNode);
                UEdGraphPin* ClassPin = TypedSpawn ? TypedSpawn->GetClassPin() : nullptr;

                if (ClassPin)
                {
                    // Clear the placeholder default so the wired value takes precedence
                    ClassPin->DefaultObject = nullptr;

                    // Wire from the referenced step's output
                    FOliveSmartWireResult WireResult = WireDataConnection(
                        Step.StepId, ClassPin->GetName(), *DynRef, Context);

                    if (WireResult.bSuccess)
                    {
                        Context.SuccessfulConnectionCount++;
                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("  Dynamic class wire OK: %s -> step '%s'.Class"),
                            **DynRef, *Step.StepId);

                        // ReconstructNode to update output pin types based on wired class
                        // (may change ReturnValue from AActor* to specific subclass)
                        TypedSpawn->ReconstructNode();
                    }
                    else
                    {
                        Context.FailedConnectionCount++;
                        Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                            TEXT("DYNAMIC_CLASS_WIRE_FAILED"),
                            Step.StepId,
                            TEXT("/steps/target"),
                            FString::Printf(TEXT("Could not wire dynamic class reference '%s' to SpawnActor Class pin: %s"),
                                **DynRef, *WireResult.ErrorMessage),
                            TEXT("Ensure the source step produces a TSubclassOf<AActor> output.")));
                    }
                }
            }
        }
    }
}
```

### How It Works End-to-End

1. AI writes: `{"op": "spawn_actor", "target": "@get_class.auto", "id": "spawn"}`
2. **Resolver** detects `@` prefix, sets `actor_class = "Actor"` (placeholder), stores `dynamic_class_ref = "@get_class.auto"`
3. **Phase 1 (CreateNodes):** `CreateSpawnActorNode` resolves `"Actor"` -> `AActor::StaticClass()`, creates the node with AActor as default class
4. **Phase 4 (WireData):** New code finds `dynamic_class_ref`, wires `@get_class.auto` output to the SpawnActor Class pin, clears placeholder default, calls `ReconstructNode()`
5. Result: SpawnActor node with Class pin wired from a variable/node, exactly like Blueprint editor drag-drop behavior

### Key Technical Details

- `UK2Node_SpawnActorFromClass::GetClassPin()` returns the Class input pin (PC_Class type, `TSubclassOf<AActor>`)
- `ClassPin->DefaultObject = nullptr` must be called before wiring to ensure the wired value takes precedence over the placeholder
- `ReconstructNode()` after wiring is important: it re-creates output pins based on the actual wired class (ExposeOnSpawn properties). However, when the class is wired dynamically (from a variable), the output type will be `AActor*` regardless -- this is correct UE behavior
- The `WireDataConnection` helper already handles `@ref` parsing, FindPinSmart, and type-compatible output matching. The Class pin name from `GetClassPin()->GetName()` should match what FindPinSmart expects

### Edge Cases

- **`@ref` pointing to a non-class output**: `WireDataConnection` will fail with type incompatibility (PC_Class vs whatever the source is). The `DYNAMIC_CLASS_WIRE_FAILED` error will tell the agent to ensure the source is `TSubclassOf<AActor>`.
- **Static class target with typo**: Unchanged behavior -- `FindClass` in `CreateSpawnActorNode` returns null, error reported.
- **Mixed static and dynamic in same plan**: Each spawn_actor step is independent. Some can be static (`target: "MyActorClass"`), others dynamic (`target: "@get_class.auto"`).
- **ReconstructNode side effects**: Minimal risk. It's called on the same game thread, in the same frame, before Phase 5 (SetDefaults). Any new pins from ExposeOnSpawn will be available for subsequent phases.

### Context Access Concern

The `PhaseWireData` function currently iterates `Plan.Steps` (the original plan steps), not `Context.ResolvedSteps`. It needs access to `Context.ResolvedSteps` to find the `dynamic_class_ref` property.

Check: Does `FOlivePlanExecutionContext` already have `ResolvedSteps`?

If not, the resolver results must be threaded through. Two options:
- **Option A**: Store `TArray<FOliveResolvedStep>` on `FOlivePlanExecutionContext` (set during Phase 1). This is the cleanest approach.
- **Option B**: Store `dynamic_class_ref` as a synthetic input on the original `FOliveIRBlueprintPlanStep` during resolution. This avoids new context fields but mutates the plan.

The coder should check `FOlivePlanExecutionContext` in `OlivePlanExecutor.h` for existing `ResolvedSteps` access. If it doesn't exist, use **Option A** -- add a `TMap<FString, TMap<FString, FString>> ResolvedProperties` field mapping StepId to the resolver's output properties.

### Alternative (Simpler) Implementation

If threading `ResolvedSteps` to `PhaseWireData` is complex, the resolver can instead inject a synthetic input on the original plan step:

In the resolver's spawn_actor `@` handler, add to `Step.Inputs`:
```cpp
// Inject synthetic input for Phase 4 to wire
// The plan step's Inputs are what PhaseWireData iterates
OutResolved.SyntheticInputs.Add(TEXT("Class"), Step.Target);  // "@get_class.auto"
```

Then in `PhaseWireData`, after processing `Step.Inputs`, also process `Resolved.SyntheticInputs` for the same step. This requires the same context access, so it's not simpler.

**Recommendation**: Check `FOlivePlanExecutionContext` first. If resolved steps are already stored there (likely, since Phase 1 creates nodes from them), just read the property. If not, the simplest approach is to store a `TMap<FString, FString> DynamicClassRefs` on the context, populated during Phase 1 when `dynamic_class_ref` is found in the resolved step's properties.

### Include Requirements

`OlivePlanExecutor.cpp` will need:
```cpp
#include "K2Node_SpawnActorFromClass.h"  // For GetClassPin()
```

Check if this is already included (it likely is, since Phase 1 creates spawn nodes).

---

## Implementation Order

| Order | Fix | Assignee | Risk | Dependencies |
|-------|-----|----------|------|-------------|
| 1 | Fix 1: Error format | Junior | Low | None |
| 2 | Fix 2: Prompt reframe | Junior | Low | None (can be parallel with Fix 1) |
| 3 | Fix 3: SpawnActor step-ref | Senior | Medium | None |

Fix 1 and Fix 2 are independent and can be implemented in parallel. Fix 3 is independent but requires more care (two-file change, context threading concern).

---

## Coder Summary

### Fix 1
- One file: `OliveBlueprintPlanResolver.cpp` lines 1742-1886
- Shorten `ErrorMessage` to remove search trail
- Restructure `Suggestion` to lead with alternatives, add describe_function advice, add UPROPERTY note, push search trail to end with `--- Search trail:` delimiter
- Add explicit UPROPERTY property-name scan when `SuggestionClass` is set (new ~15 lines)
- Do NOT change the ScopedSuggestions or catalog fuzzy match logic -- only restructure the output format

### Fix 2
- Two files: `cli_blueprint.txt` and `OliveCLIProviderBase.cpp`
- Rename section header, reframe describe_function from "pin verification" to "function verification"
- Two one-line changes in OliveCLIProviderBase.cpp (lines 410 and 663)

### Fix 3
- Two files: `OliveBlueprintPlanResolver.cpp` (resolver) and `OlivePlanExecutor.cpp` (Phase 4)
- Resolver: detect `@` prefix in spawn_actor target, use `"Actor"` placeholder, store `dynamic_class_ref`
- Executor: after normal data wiring, check spawn_actor steps for `dynamic_class_ref`, wire Class pin, clear placeholder default, ReconstructNode
- First task: check `FOlivePlanExecutionContext` for existing resolved step access; if absent, add `TMap<FString, FString> DynamicClassRefs`
- Include `K2Node_SpawnActorFromClass.h` if not already present
