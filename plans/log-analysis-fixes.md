# Log Analysis Fixes -- Design Plan

**Date:** 2026-02-28
**Author:** Architect Agent
**Priority:** Issues 1 & 2 are CRITICAL (blocking autonomy), 3 is HIGH, 4 & 5 are MEDIUM

Fixes for 5 systemic issues discovered from analyzing a real autonomous run log. All changes are in existing files -- no new modules or files needed.

---

## Issue 1: Ghost K2Node_CallFunction (CRITICAL)

### Problem

When `blueprint.add_node` creates a `K2Node_CallFunction`, the AI passes properties like `function_name`/`function_class` or `MemberName`/`MemberParentClass`. But these are fields inside the `FMemberReference FunctionReference` nested struct on `UK2Node_CallFunction` -- they are NOT top-level reflected UProperties. `SetNodePropertiesViaReflection` searches `Node->GetClass()->FindPropertyByName()` which only finds direct properties, so it skips them all.

Result: Node created with 0 pins, reports SUCCESS, AI wastes entire remaining budget trying to wire a 0-pin node. The node generates permanent "Could not find a function named 'None'" compile errors.

### Root Cause

`CreateNodeByClass` does not special-case `UK2Node_CallFunction`. The `FMemberReference` struct has its own API (`SetFromFunction`, `SetExternalMember`) that must be used instead of property reflection.

### Fix: Option C (Guard + Support)

Two changes in `OliveNodeFactory.cpp`:

#### Change 1a: FMemberReference support in CreateNodeByClass

After the call to `SetNodePropertiesViaReflection` but BEFORE `AllocateDefaultPins`, add a special-case block for `UK2Node_CallFunction`. If the node is a `UK2Node_CallFunction` (or subclass like `UK2Node_CallParentFunction`), extract `function_name` and `function_class`/`target_class` from the Properties map (checking common aliases: `function_name`, `FunctionName`, `MemberName`, `target` for the name; `function_class`, `FunctionClass`, `MemberParentClass`, `target_class` for the class). Then:

1. Use `FOliveNodeFactory::FindFunctionEx(FunctionName, ClassName, Blueprint)` to resolve to a `UFunction*`.
2. If found, call `CallFuncNode->SetFromFunction(UFunction*)` which populates `FunctionReference` correctly.
3. If NOT found by `FindFunctionEx`, try `FOliveClassResolver::ResolveClass(ClassName)` to get a `UClass*`, then `UClass->FindFunctionByName(FName(*FunctionName))`.
4. If still not found, log a warning but continue -- `AllocateDefaultPins` will produce 0 pins, caught by the guard below.

**Key detail**: `SetFromFunction` must be called BEFORE `AllocateDefaultPins` because `UK2Node_CallFunction::AllocateDefaultPins` reads `FunctionReference` to determine which pins to create.

**Key detail**: `CreateNodeByClass` currently receives only `const FString& ClassName` and `const TMap<FString, FString>& Properties` -- it does NOT receive a `UBlueprint*`. The `CreateNode` method that calls it DOES have the Blueprint. Two options:
- **Option A**: Pass `Blueprint` down to `CreateNodeByClass` as an optional parameter (default nullptr). This is cleaner.
- **Option B**: Use `FindFunctionEx` with nullptr Blueprint, which searches library classes only. Less effective.

**Decision**: Option A. Add `UBlueprint* Blueprint = nullptr` parameter to `CreateNodeByClass`. The call site in `CreateNode` (line ~131) already has `Blueprint` in scope.

#### Change 1b: Zero-pin guard for K2Node_CallFunction

After `ReconstructNode()` (line 1693), BEFORE the success log and return, add a guard:

```
if (NewNode->IsA<UK2Node_CallFunction>() && NewNode->Pins.Num() == 0)
{
    // Ghost node -- FunctionReference not set correctly.
    // Remove the node and fail with actionable error.
    Graph->RemoveNode(NewNode);
    LastError = FString::Printf(
        TEXT("K2Node_CallFunction created with 0 pins -- function reference not resolved. "
             "Do NOT use blueprint.add_node for function calls. "
             "Use blueprint.apply_plan_json with a 'call' op instead. "
             "Example: {\"op\": \"call\", \"target\": \"%s\"}"),
        *Properties.FindRef(TEXT("function_name")));
    return nullptr;
}
```

This is defense-in-depth: even if Change 1a correctly sets the function, if something goes wrong and pins are still 0, we fail hard with guidance instead of creating a ghost.

### Files to Modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h` | Add `UBlueprint*` param to `CreateNodeByClass` signature |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | 1a: FMemberReference special-case; 1b: zero-pin guard; update `CreateNode` call site |

### Acceptance Criteria

1. `add_node` with `type=K2Node_CallFunction` and `properties={function_name: "PrintString", function_class: "KismetSystemLibrary"}` creates a node with correct pins (at minimum: execute, InString, ReturnValue exec, etc.)
2. `add_node` with `type=K2Node_CallFunction` and no function properties (or invalid function name) returns an error with `GHOST_NODE_PREVENTED` code and guidance to use plan_json
3. Existing plan_json flows are NOT affected (they use curated `CreateCallFunctionNode`, not `CreateNodeByClass`)
4. `CreateNode` call site passes `Blueprint` down to `CreateNodeByClass`

### Edge Cases

- AI passes `MemberName` instead of `function_name` -- property alias lookup handles this
- AI passes a valid function_name but wrong/missing function_class -- FindFunctionEx broad search resolves it
- AI passes a function that exists on a component class but not the Blueprint itself -- FindFunctionEx's SCS search handles this
- UK2Node_CallParentFunction (subclass) -- `IsA<UK2Node_CallFunction>()` catches it; `SetFromFunction` works the same way
- Non-CallFunction nodes with 0 pins (e.g., K2Node_Knot has 2 pins, but some exotic nodes might have 0 legitimately) -- guard is scoped to CallFunction only

### New Error Code

`GHOST_NODE_PREVENTED` -- used when zero-pin guard triggers after FMemberReference setup fails.

---

## Issue 2: Compile Error Masking (CRITICAL)

### Problem

`FOliveCompileManager::Compile()` reports SUCCESS when `Blueprint->Status == BS_Error`. The compile manager relies ENTIRELY on per-node `ErrorType`/`ErrorMsg` to detect errors (via `ExtractNodeErrors`). But some errors (like "Graph named 'Interact' already exists") are graph-level compiler errors that do NOT attach to any specific node -- they go to the Blueprint message log only.

The current `ParseCompileLog` method (line 309) checks `Blueprint->Status` but only sets warnings for `BS_Dirty`/`BS_Unknown`. For `BS_Error`, the comment says "Errors will be extracted from node messages" -- but the graph-level errors are NOT on nodes, so they are silently dropped.

### Root Cause

Two gaps:

1. **`ParseCompileLog` ignores BS_Error as a standalone signal**: When `Blueprint->Status == BS_Error` but NO node has `ErrorType == Error`, no errors are reported.
2. **No message log capture**: UE's Blueprint compiler logs errors to `FCompilerResultsLog` which may write to the message log system. The compile manager doesn't capture these.

### Fix: Status-Based Fallback + Compiler Output Capture

#### Change 2a: Blueprint::Status as authoritative fallback

In `FOliveCompileManager::Compile()` (line 74-75), currently:
```cpp
Result.bSuccess = (Result.Errors.Num() == 0);
```

Change to:
```cpp
Result.bSuccess = (Result.Errors.Num() == 0) && (Blueprint->Status != BS_Error);

// If Blueprint reports error state but we found no per-node errors,
// add a synthetic error so the AI knows something is wrong.
if (Blueprint->Status == BS_Error && Result.Errors.Num() == 0)
{
    Result.Errors.Add(FOliveIRCompileError::MakeError(
        TEXT("Blueprint has compile errors that are not attached to specific nodes. "
             "This usually means a structural problem like duplicate graph names, "
             "interface conflicts, or circular dependencies."),
        TEXT("Use blueprint.read to examine the Blueprint structure. "
             "Check for duplicate function/graph names and interface conflicts. "
             "Consider using mode:'replace' in plan_json to rebuild the graph.")));
}
```

#### Change 2b: Capture compiler messages via FCompilerResultsLog redirection

Before calling `FKismetEditorUtilities::CompileBlueprint`, redirect compiler output to capture the messages. The UE 5.5 `FKismetEditorUtilities::CompileBlueprint` has an overload that takes an `FCompilerResultsLog*`. However, the current call uses the simple signature.

Actually, looking more carefully at UE 5.5's `FKismetEditorUtilities::CompileBlueprint`, the signature is:
```cpp
static void CompileBlueprint(UBlueprint* BlueprintObj,
    EBlueprintCompileOptions CompileOptions = EBlueprintCompileOptions::None,
    FCompilerResultsLog* pResults = nullptr);
```

The fix:

```cpp
// Create results log to capture compiler messages
FCompilerResultsLog ResultsLog;
ResultsLog.BeginEvent(TEXT("OliveCompile"));

// Perform compilation WITH results capture
FKismetEditorUtilities::CompileBlueprint(Blueprint, CompileOptions, &ResultsLog);

ResultsLog.EndEvent();

// ... existing parse ...

// NEW: Extract messages from the compiler results log
// FCompilerResultsLog inherits from FCompilerResultsLog and stores messages.
// Access via ResultsLog.Messages (TArray<TSharedRef<FTokenizedMessage>>)
for (const TSharedRef<FTokenizedMessage>& Msg : ResultsLog.Messages)
{
    if (Msg->GetSeverity() == EMessageSeverity::Error)
    {
        FString MsgText = Msg->ToText().ToString();

        // Check if this error was already captured via per-node extraction
        bool bAlreadyCaptured = false;
        for (const FOliveIRCompileError& Existing : OutResult.Errors)
        {
            if (Existing.Message.Contains(MsgText) || MsgText.Contains(Existing.Message))
            {
                bAlreadyCaptured = true;
                break;
            }
        }

        if (!bAlreadyCaptured)
        {
            FOliveIRCompileError Error;
            Error.Message = MsgText;
            Error.Severity = EOliveIRCompileErrorSeverity::Error;
            Error.Suggestion = GenerateSuggestion(Error);
            OutResult.Errors.Add(Error);
        }
    }
}
```

**IMPORTANT**: The coder MUST verify the exact UE 5.5 API for `FCompilerResultsLog`. Key things to check:
- Is it `FCompilerResultsLog` or `FCompilerResultLog` (no 's')?
- Check `Engine/Source/Editor/KismetCompiler/Public/KismetCompilerModule.h` or `Kismet2/CompilerResultsLog.h`
- Does it have `Messages` or `MessageMap` or some other accessor?
- Does `BeginEvent`/`EndEvent` exist or is it a different initialization pattern?

If `FCompilerResultsLog` is not readily available or its API differs, the fallback approach is Change 2a alone (BS_Error status check), which already catches the most critical case.

### Files to Modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Compile/OliveCompileManager.cpp` | 2a: BS_Error fallback in `Compile()`; 2b: Compiler results log capture |

### Acceptance Criteria

1. When Blueprint has status `BS_Error` but no per-node errors, `Compile()` returns `bSuccess = false` with at least one error message
2. Graph-level compile errors like "Graph named 'X' already exists" appear in the returned `Errors` array
3. Normal compilation (no errors) still returns `bSuccess = true`
4. Duplicate errors are not reported (dedup check prevents per-node errors from appearing twice)

### Edge Cases

- Blueprint with ONLY node-level errors (common case) -- no change in behavior, Change 2a's guard doesn't fire because `Result.Errors.Num() > 0`
- Blueprint with BOTH node-level AND graph-level errors -- both captured, dedup prevents doubles
- Blueprint that is `BS_Error` from a previous compilation (stale state) -- this is correct behavior, the error IS real
- `FCompilerResultsLog` API unavailable -- coder falls back to Change 2a alone, which is still a significant improvement

---

## Issue 3: Interface Auto-Generated Graph Conflict (HIGH)

### Problem

When `blueprint.add_interface` adds an interface (e.g., `BPI_Interactable` with an `Interact` function), UE's `FBlueprintEditorUtils::ImplementNewInterface` automatically creates function graph stubs for each interface function. If the Blueprint already has a user-created function with the same name (e.g., a custom `Interact` function graph), there's a name collision causing a permanent compile error: "Graph named 'Interact' already exists."

### Root Cause

`FOliveBlueprintWriter::AddInterface` (line 399) checks for duplicate interface implementation but does NOT check for function name conflicts before calling `ImplementNewInterface`.

### Fix: Pre-flight Name Collision Check

In `FOliveBlueprintWriter::AddInterface`, after finding the interface class and confirming it's not already implemented (line 442), add a pre-flight check:

```cpp
// Check for function name collisions BEFORE adding the interface.
// ImplementNewInterface auto-creates function graph stubs for each interface
// function. If a graph with the same name already exists, UE produces a
// permanent compile error ("Graph named 'X' already exists").
TArray<FString> ConflictingNames;
for (TFieldIterator<UFunction> FuncIt(InterfaceClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
{
    UFunction* InterfaceFunc = *FuncIt;
    if (!InterfaceFunc || !InterfaceFunc->HasAnyFunctionFlags(FUNC_BlueprintEvent))
    {
        continue;
    }

    const FName FuncName = InterfaceFunc->GetFName();

    // Check existing function graphs
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == FuncName)
        {
            ConflictingNames.Add(FuncName.ToString());
            break;
        }
    }

    // Also check UbergraphPages (event graphs can have sub-graphs)
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph && Graph->GetFName() == FuncName)
        {
            ConflictingNames.Add(FuncName.ToString());
            break;
        }
    }
}

if (ConflictingNames.Num() > 0)
{
    return FOliveBlueprintWriteResult::Error(
        FString::Printf(
            TEXT("Cannot add interface '%s': function graph(s) named '%s' already exist on this "
                 "Blueprint. Adding the interface would create duplicate graphs causing permanent "
                 "compile errors. Remove or rename the conflicting function(s) first using "
                 "blueprint.remove_function, then add the interface."),
            *InterfacePath,
            *FString::Join(ConflictingNames, TEXT("', '"))));
}
```

### Files to Modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveBlueprintWriter.cpp` | Pre-flight collision check in `AddInterface()`, between duplicate-interface check and the transaction |

### Acceptance Criteria

1. Adding an interface whose functions DON'T conflict with existing graphs succeeds normally
2. Adding an interface whose function names conflict with existing function graphs returns an error with clear guidance
3. The error message lists ALL conflicting function names, not just the first one
4. The error suggests `blueprint.remove_function` as the remedy

### Edge Cases

- Interface with multiple functions, only some conflicting -- error lists only the conflicting ones
- Interface with no BlueprintEvent functions (C++ only interfaces) -- no conflict possible, skip check
- Interface function name matches an event graph name (UbergraphPages) -- caught by the UbergraphPages scan
- Interface already implemented (caught by existing check at line 434, returns before our new check runs) -- no change needed

### New Error Code

`INTERFACE_GRAPH_CONFLICT` -- add to error message prefix for self-correction policy pattern matching.

---

## Issue 4: Interface Function Calls in plan_json via Cast Output (MEDIUM)

### Problem

In a plan_json, the AI writes:
```json
{"step_id": "cast", "op": "cast", "target": "BP_Gun"},
{"step_id": "interact", "op": "call", "target": "Interact",
 "inputs": {"self": "@cast.As BP_Gun"}}
```

`ResolveCallOp` searches for `Interact` on the Blueprint being edited (BP_ThirdPersonCharacter), which does NOT implement `BPI_Interactable`. The function is on BP_Gun (via its interface). But the resolver doesn't know about the cast target class -- it only searches the editing Blueprint's class hierarchy.

### Root Cause

`ResolveCallOp` calls `FindFunctionEx(Step.Target, Step.TargetClass, BP)` where `BP` is always the Blueprint being edited. It does not inspect data inputs to discover that the call's target object flows from a cast node whose target class might have the function.

### Fix: Cast-Aware Function Resolution in ResolveCallOp

After `FindFunctionEx` fails and before the event-dispatcher fallback check (line ~1175), add a cast-target resolution pass:

```cpp
// --- Function NOT found on editing Blueprint -- check if inputs reference a cast step ---
// If one of this step's inputs (commonly "self" or "Target") references a cast step
// (via @cast_step.AsCastClass), search the cast target class for the function.
if (!SearchResult.IsValid() && BP)
{
    for (const auto& InputPair : Step.Inputs)
    {
        const FString& InputValue = InputPair.Value;
        if (!InputValue.StartsWith(TEXT("@")))
        {
            continue;
        }

        // Parse the @ref to get the source step ID
        FString RefBody = InputValue.Mid(1);
        int32 DotIdx;
        FString RefStepId;
        if (RefBody.FindChar(TEXT('.'), DotIdx))
        {
            RefStepId = RefBody.Left(DotIdx);
        }
        else
        {
            RefStepId = RefBody;
        }

        // Find the referenced step in the plan to check if it's a cast op
        for (const FOliveIRBlueprintPlanStep& OtherStep : Plan.Steps)
        {
            if (OtherStep.StepId == RefStepId && OtherStep.Op == OlivePlanOps::Cast)
            {
                // Found a cast step -- resolve its target class
                FString CastClassName = OtherStep.Target.IsEmpty()
                    ? OtherStep.TargetClass : OtherStep.Target;

                if (!CastClassName.IsEmpty())
                {
                    UClass* CastClass = FOliveClassResolver::ResolveClass(CastClassName);
                    if (CastClass)
                    {
                        // Search the cast target class for the function
                        FOliveFunctionSearchResult CastSearch =
                            FOliveNodeFactory::Get().FindFunctionEx(
                                Step.Target, CastClass->GetName(), nullptr);

                        if (CastSearch.IsValid())
                        {
                            // Found the function on the cast target class!
                            SearchResult = CastSearch;

                            Out.ResolverNotes.Add(FOliveResolverNote{
                                TEXT("search_scope"),
                                TEXT("editing_blueprint"),
                                FString::Printf(TEXT("cast_target:%s"), *CastClassName),
                                FString::Printf(TEXT("Function '%s' found on cast target class '%s' (from step '%s')"),
                                    *Step.Target, *CastClassName, *RefStepId)
                            });
                            break;
                        }
                    }
                }
            }
        }

        if (SearchResult.IsValid()) break;
    }

    // If found via cast, apply the result (same code path as the primary success path)
    if (SearchResult.IsValid())
    {
        // ... populate Out.Properties, Out.ResolvedOwningClass, etc.
        // (identical to the success block at lines 1117-1172)
    }
}
```

**Key consideration**: `ResolveCallOp` does NOT currently have access to the full plan (all steps). It receives only the single `FOliveIRBlueprintPlanStep`. To look up the cast step, we need the full plan.

**Signature change required**: Add `const FOliveIRBlueprintPlan& Plan` parameter to `ResolveCallOp`. The caller (`ResolveStep`, line 891) already has the plan in scope (it's called from `Resolve()` which iterates `Plan.Steps`).

Actually, looking more carefully: `ResolveStep` (line 857) does NOT have the plan. It's called per-step from the main `Resolve()` method. Let me re-read the code structure.

The main entry point is `FOliveBlueprintPlanResolver::Resolve()`. Let me check its signature and what it passes to `ResolveStep`.

### Revised approach: Pass plan steps as context

The simplest approach that avoids threading the full plan through many layers:

In `Resolve()` (the main entry), build a `TMap<FString, FString> CastTargetMap` before resolving steps. This maps step_id -> target_class for all cast ops. Pass this map to `ResolveStep`, which passes it to `ResolveCallOp`.

```cpp
// Pre-scan: build cast target map for cross-step resolution
TMap<FString, FString> CastTargetMap;
for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
{
    if (Step.Op == OlivePlanOps::Cast)
    {
        FString CastTarget = Step.Target.IsEmpty() ? Step.TargetClass : Step.Target;
        if (!CastTarget.IsEmpty())
        {
            CastTargetMap.Add(Step.StepId, CastTarget);
        }
    }
}
```

Then in `ResolveCallOp`, when primary resolution fails, scan `Step.Inputs` for any `@ref` whose step_id is in `CastTargetMap`, resolve that class, and search it for the function.

### Files to Modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h` | Add `CastTargetMap` parameter to `ResolveCallOp` and `ResolveStep` |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | Build CastTargetMap in `Resolve()`, pass through, use in `ResolveCallOp` fallback |

### Acceptance Criteria

1. Plan with `cast -> call` chain where function is on cast target class resolves successfully
2. `ResolverNotes` include a note about finding the function via cast target
3. Normal calls (not from cast output) are completely unaffected
4. Cast steps with invalid/empty target class are gracefully skipped (no crash)
5. Multiple cast steps in the same plan each correctly populate the map

### Edge Cases

- The AI wires `@cast_step.auto` (no pin hint) to the self input -- the `@ref` parsing just needs the step ID, not the pin name
- Cast target class is a Blueprint, not a native class -- `FOliveClassResolver::ResolveClass` handles both
- Function found on an interface implemented by the cast target -- `FindFunctionEx` already searches interfaces
- Multiple inputs from different cast steps -- first match wins (function should be the same regardless)

---

## Issue 5: Function Entry Input References in plan_json (MEDIUM)

### Problem

The AI writes plans targeting function graphs (like `Pickup`) and tries to reference function input parameters via `@entry.NewOwner`. But "entry" is not a step_id in the plan -- it's the pre-existing `UK2Node_FunctionEntry` in the graph. The resolver synthesizes `_synth_param_` steps for bare `@ParamName` references (line 792), but does NOT handle `@entry.ParamName` syntax.

When the AI writes `@entry.NewOwner`, `ParseDataRef` splits it into `SourceStepId="entry"` and `SourcePinHint="NewOwner"`. Then `GetManifest("entry")` returns nullptr because "entry" was never registered as a step.

### Root Cause

The resolver's `ExpandMissingComponentTargets` handles:
- `@ParamName` (bare, no dot) -- synthesizes a `_synth_param_` step
- `@ParamName.PinHint` (param name before dot) -- synthesizes a `_synth_param_` step

But it does NOT handle `@entry.ParamName` where "entry" is a special alias for the function entry node.

### Fix: Handle @entry as alias for FunctionEntry node

Two changes:

#### Change 5a: In ExpandMissingComponentTargets, handle @entry.X references

In the `ExpandMissingComponentTargets` method (around line 571), add handling for `@entry.X` references. When in a function graph context:

```cpp
// Handle @entry.X -- "entry" is a special alias for the FunctionEntry node's outputs.
// Rewrite @entry.ParamName to use a synthesized FunctionInput step.
if (DotIndex != INDEX_NONE)
{
    FString RefStepId = RefBody.Left(DotIndex);

    // ... existing step-id check ...

    // Check if "entry" alias used in function graph
    if (GraphContext.bIsFunctionGraph
        && (RefStepId.Equals(TEXT("entry"), ESearchCase::IgnoreCase)
            || RefStepId.Equals(GraphContext.GraphName, ESearchCase::IgnoreCase)))
    {
        FString PinHint = RefBody.Mid(DotIndex + 1);

        // The pin hint should correspond to a function parameter name.
        // Find the matching parameter or just use the hint as the target.
        FString ParamTarget = PinHint;
        for (const FString& ParamName : GraphContext.InputParamNames)
        {
            if (ParamName.Equals(PinHint, ESearchCase::IgnoreCase))
            {
                ParamTarget = ParamName; // Use canonical casing
                break;
            }
        }

        // Synthesize a FunctionInput step (same pattern as bare @ParamName)
        FString SynthKey = TEXT("entry_") + ParamTarget.ToLower();
        FString* ExistingSynthId = SynthesizedComponentSteps.Find(SynthKey);
        if (!ExistingSynthId)
        {
            FString SynthStepId = FString::Printf(TEXT("_synth_param_%s"), *ParamTarget.ToLower());

            // Check if this synth step already exists from a bare @ParamName reference
            if (!ExistingStepIds.Contains(SynthStepId))
            {
                FOliveIRBlueprintPlanStep SynthStep;
                SynthStep.StepId = SynthStepId;
                SynthStep.Op = OlivePlanOps::GetVar;
                SynthStep.Target = ParamTarget;

                Inserts.Add({ MoveTemp(SynthStep), i });
                ExistingStepIds.Add(SynthStepId);
            }

            SynthesizedComponentSteps.Add(SynthKey, SynthStepId);

            FOliveResolverNote Note;
            Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
            Note.OriginalValue = Value;
            Note.ResolvedValue = FString::Printf(
                TEXT("Rewritten @entry.%s -> @%s.auto (FunctionInput step)"),
                *PinHint, *SynthStepId);
            Note.Reason = TEXT("@entry is an alias for the function entry node. "
                              "Synthesized a get_var step for the parameter.");
            OutNotes.Add(MoveTemp(Note));
        }

        FString SynthId = SynthesizedComponentSteps[SynthKey];
        RewrittenInputs.Add(PinName, FString::Printf(TEXT("@%s.auto"), *SynthId));
        bExpanded = true;
        continue;
    }
}
```

**Integration point**: This code goes inside the existing `if (DotIndex != INDEX_NONE)` block, AFTER the `ExistingStepIds.Contains(RefStepId)` check (which would skip it if "entry" happened to be a real step_id). The check must come BEFORE the `FunctionInputParams.Contains(RefStepId)` check since "entry" is not a parameter name.

#### Change 5b: Also handle @entry without dot (bare @entry)

If the AI writes just `@entry` (without a dot), this hits the bare-ref path. Add a check: if `RefBody == "entry"` and we're in a function graph, there's no specific pin to resolve. This is an error -- we should produce a helpful message:

This is actually already partially handled: if "entry" is not a step_id and not a variable name and not a function param name, it falls through and becomes an unresolved reference. The wiring phase then reports "Source step 'entry' not found." The improvement here is to add a specific error message recognizing the "entry" alias.

However, the more common case is `@entry.PinName`, not bare `@entry`. Change 5a handles the important case. Change 5b is nice-to-have.

### Files to Modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | Handle `@entry.X` in `ExpandMissingComponentTargets` |

### Acceptance Criteria

1. `@entry.ParamName` in a function graph plan resolves to the function entry node's output pin
2. `@FunctionName.ParamName` (using graph name as alias) also resolves correctly
3. In event graph plans, `@entry.X` where "entry" is not a step_id still reports the normal "step not found" error (no regression)
4. Multiple `@entry.X` references to different parameters each get their own synth step
5. Mix of `@entry.X` and bare `@ParamName` references to the same parameter share the same synth step (no duplicate nodes)

### Edge Cases

- `@entry.auto` -- "auto" is not a parameter name, but the synth step would just create a get_var targeting "auto". The get_var resolver would then fail with a sensible error about variable not found. This is acceptable.
- `@entry.X` where X is NOT a known parameter -- the synth step is created anyway, but when the get_var resolver runs, it will fail if the param doesn't exist. This is fine -- the error message from the resolver is clear.
- Case sensitivity -- `@Entry.newOwner` vs `@entry.NewOwner` -- both caught by case-insensitive compare on "entry", and parameter name resolved via case-insensitive match against `InputParamNames`.

---

## Implementation Order

Tasks are ordered by priority and dependency:

| Order | Task | Issue | Files | Est. Time | Dependencies |
|-------|------|-------|-------|-----------|-------------|
| T1 | Zero-pin guard for K2Node_CallFunction | 1b | OliveNodeFactory.cpp | 30 min | None |
| T2 | FMemberReference support in CreateNodeByClass | 1a | OliveNodeFactory.h, OliveNodeFactory.cpp | 1.5 hr | None |
| T3 | BS_Error fallback in CompileManager | 2a | OliveCompileManager.cpp | 30 min | None |
| T4 | Compiler results log capture | 2b | OliveCompileManager.cpp | 1 hr | T3 |
| T5 | Interface graph collision check | 3 | OliveBlueprintWriter.cpp | 45 min | None |
| T6 | Cast-aware function resolution | 4 | OliveBlueprintPlanResolver.h, .cpp | 1.5 hr | None |
| T7 | @entry alias in function graph plans | 5 | OliveBlueprintPlanResolver.cpp | 1 hr | None |

**Parallelizable**: T1+T3+T5+T7 can all run in parallel (different files, independent). T2 depends on having T1 in place (same code region). T4 depends on T3. T6 is independent.

**Critical path**: T1 -> T2 (Issue 1 complete), then T3 -> T4 (Issue 2 complete).

**Total estimated time**: ~6.5 hours for a single coder, ~3 hours parallelized with 2 coders.

---

## Verification Plan

### Issue 1 Verification
1. Open UE editor, connect Claude Code via MCP
2. Ask it to add a K2Node_CallFunction with `function_name: "PrintString"` to any Blueprint via `blueprint.add_node`
3. Verify the node has correct pins (InString, etc.)
4. Ask it to add a K2Node_CallFunction with no function properties
5. Verify it returns `GHOST_NODE_PREVENTED` error with plan_json guidance

### Issue 2 Verification
1. Create a Blueprint with a function named "Interact"
2. Manually add interface BPI_Interactable (which also defines Interact)
3. Compile via `FOliveCompileManager::Compile()`
4. Verify `bSuccess == false` and error message mentions duplicate graph name
5. Verify the error appears in the tool result when `bAutoCompile = true`

### Issue 3 Verification
1. Create a Blueprint with a function named "Interact"
2. Call `blueprint.add_interface` with `BPI_Interactable`
3. Verify it returns an error mentioning "Interact" as conflicting
4. Remove the function, then add the interface again
5. Verify success

### Issue 4 Verification
1. Create a plan_json with: cast to BP_Gun -> call Interact with self from cast output
2. Run against BP_ThirdPersonCharacter (which does NOT implement BPI_Interactable)
3. Verify the resolver finds Interact on BP_Gun's class via cast-target resolution
4. Verify the resolver note documents the cross-step resolution

### Issue 5 Verification
1. Create a function graph "Pickup" with parameter "NewOwner" (Actor type)
2. Write a plan_json targeting that function graph with `@entry.NewOwner`
3. Verify the resolver creates a synth `_synth_param_newowner` step
4. Verify wiring connects to the FunctionEntry node's NewOwner output pin
5. Test `@Pickup.NewOwner` as alias (graph name instead of "entry")
