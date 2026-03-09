# Resolver-Executor Contract: Single Resolution Authority

## Status: DESIGN REVIEW

## Problem

The resolver (`FOliveBlueprintPlanResolver::ResolveCallOp`) and executor (`FOlivePlanExecutor::PhaseCreateNodes` via `FOliveNodeFactory::CreateNodeByClass`) both call `FindFunction`/`FindFunctionEx` independently. The resolver resolves aliases, detects UPROPERTY matches, performs cast-target lookups, and validates pin signatures -- then the executor repeats the entire search through `CreateNodeByClass`. This causes:

1. **Double-aliasing** (GetForwardVector): resolver resolves correctly, executor re-fires alias map. Patched with `_resolved` flag but that's a band-aid.
2. **UPROPERTY detection gap**: `FindFunctionEx` failure path detects `PROPERTY MATCH:` in SearchedLocations, but the resolver only surfaces this as an error -- it doesn't auto-rewrite the op.
3. **Wasted work**: every `call` op runs `FindFunction` twice (once in resolver, once in NodeFactory).
4. **Future drift risk**: any resolution improvement must be duplicated in both places.

## Audit Results

### What the executor's FindFunction call uses

In `OliveNodeFactory.cpp:1848-1868` (`CreateNodeByClass` for `UK2Node_CallFunction`):

```cpp
const bool bSkipAlias = Properties.Contains(TEXT("_resolved"));
FOliveFunctionSearchResult SearchResult = FindFunctionEx(FuncName, FuncClassName, Blueprint, bSkipAlias);
```

Arguments:
- `FuncName` = `Properties["function_name"]` (already set by resolver)
- `FuncClassName` = `Properties["target_class"]` (already set by resolver)
- `Blueprint` = same Blueprint pointer available at resolve time
- `bSkipAlias` = derived from `_resolved` flag (always `true` for plan_json path)

**Critical finding**: The executor's `FindFunction` call uses ZERO graph-derived context. It only uses `Properties` (set by resolver) and the same `Blueprint*` the resolver already has. There is no runtime node context, no allocated pins, no Phase 1 outputs -- nothing that would make the executor's search produce different results.

The `_resolved` flag proves this was already understood -- it was a workaround for the fact that passing the *name* through Properties and re-resolving is lossy. The right fix is to pass the `UFunction*` itself.

### What the resolver already stores

`FOliveResolvedStep` (in `OliveBlueprintPlanResolver.h:92-144`) has:
- `UClass* ResolvedOwningClass` -- set by `ResolveCallOp` on success
- `Properties["function_name"]` -- canonical function name
- `Properties["target_class"]` -- owning class name
- `bIsPure`, `bIsLatent` -- derived from `UFunction*` flags
- `bIsInterfaceCall`, `bIsInterfaceEvent` -- match method signals

The resolver already has the `UFunction*` in hand (line 1456: `UFunction* Function = SearchResult.Function`) and extracts its name and class. It just doesn't carry the pointer forward.

### Where `UFunction*` comes from

The resolver calls `FindFunctionEx` on `FOliveNodeFactory::Get()` (line 1452). The returned `UFunction*` is on the `UClass` (not the graph), so it's stable across the resolver-executor gap. Both run on the same game thread in the same frame.

## Design

### 1. IR Change: Add `UFunction*` to `FOliveResolvedStep`

**File**: `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h`

Add one field to `FOliveResolvedStep`:

```cpp
/**
 * The resolved UFunction* for 'call' ops.
 * Set by ResolveCallOp when FindFunctionEx succeeds.
 * When non-null, the executor passes this directly to
 * UK2Node_CallFunction::SetFromFunction() and skips FindFunction entirely.
 * Lifetime: stable within a single plan execution (same frame, same UClass).
 * nullptr for non-call ops or when resolution failed (should not reach executor).
 */
UFunction* ResolvedFunction = nullptr;
```

This field lives in the Editor module (not Runtime IR), so no IR schema versioning needed. `FOliveResolvedStep` is already editor-only and not serialized.

### 2. Resolver Changes

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

#### 2a. Store `UFunction*` on success (3 locations)

In `ResolveCallOp`, after each successful `FindFunctionEx` path, add:

```cpp
Out.ResolvedFunction = Function;
```

There are 3 success paths in `ResolveCallOp`:
1. **Direct search** (line ~1456): main `FindFunctionEx` succeeds
2. **Alias fallback** (line ~1576): alias pin validation reroutes to fallback function
3. **Cast-target search** (line ~1715): function found on cast target class

Each already has `UFunction* Function` in scope. Just add `Out.ResolvedFunction = Function;` after the existing `Out.ResolvedOwningClass = OwningClass;` line.

#### 2b. UPROPERTY auto-rewrite: `call` to `set_var`/`get_var`

After the main `FindFunctionEx` call fails (line ~1762+), before building the FUNCTION_NOT_FOUND error, check `SearchedLocations` for `PROPERTY MATCH:`:

```cpp
// --- UPROPERTY auto-rewrite ---
// When FindFunctionEx fails but detects a matching UPROPERTY, the AI
// intended to set/get a property, not call a function. Rewrite the op.
if (!SearchResult.IsValid())
{
    for (const FString& Location : SearchResult.SearchedLocations)
    {
        if (Location.StartsWith(TEXT("PROPERTY MATCH:")))
        {
            // Extract property name from the PROPERTY MATCH message.
            // Format: "PROPERTY MATCH: 'PropName' is a Type property on ClassName..."
            // Parse the name between single quotes.
            int32 FirstQuote = Location.Find(TEXT("'"));
            int32 SecondQuote = Location.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstQuote + 1);
            if (FirstQuote != INDEX_NONE && SecondQuote != INDEX_NONE)
            {
                FString PropertyName = Location.Mid(FirstQuote + 1, SecondQuote - FirstQuote - 1);

                // Determine set vs get based on Step.Target prefix and presence of Inputs
                bool bIsSet = Step.Target.StartsWith(TEXT("Set"), ESearchCase::IgnoreCase)
                           || Step.Inputs.Num() > 0;

                Out.NodeType = bIsSet ? OliveNodeTypes::VariableSet : OliveNodeTypes::VariableGet;
                Out.Properties.Add(TEXT("variable_name"), PropertyName);
                Out.bIsPure = !bIsSet;

                Out.ResolverNotes.Add(FOliveResolverNote{
                    TEXT("op"),
                    OlivePlanOps::Call,
                    bIsSet ? OlivePlanOps::SetVar : OlivePlanOps::GetVar,
                    FString::Printf(TEXT("'%s' is a property, not a function. Rewritten from 'call' to '%s'."),
                        *Step.Target, bIsSet ? *OlivePlanOps::SetVar : *OlivePlanOps::GetVar)
                });

                Warnings.Add(FString::Printf(
                    TEXT("Step '%s': '%s' auto-rewritten from call to %s (UPROPERTY detected)"),
                    *Step.StepId, *Step.Target, bIsSet ? TEXT("set_var") : TEXT("get_var")));

                return true; // Step is now resolved
            }
        }
    }

    // ... existing FUNCTION_NOT_FOUND error path continues ...
}
```

**Important**: This rewrite happens in-place on `Out`. The executor will see `NodeType == VariableSet` or `VariableGet` and never attempt `FindFunction`. The step's `Inputs` map still contains the value to set -- the executor's Phase 4 (SetDefaults) or Phase 3 (WireData) will handle it the same as any other `set_var`.

### 3. Executor Changes

**File**: `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

#### 3a. Pass `ResolvedFunction` to NodeFactory via Properties

The executor calls `Writer.AddNode()` which goes through `CreateNodeByClass`. We need to get the `UFunction*` into that path. Two options:

**Option A (recommended): New overload on NodeFactory**

Add a property `_resolved_function_ptr` that carries the pointer as a string-encoded address. This is ugly but avoids changing `AddNode` signatures.

**Option B (cleaner): Direct SetFromFunction after AddNode**

Instead of trying to pass `UFunction*` through AddNode, skip the function lookup in NodeFactory entirely when the pointer is available, and call `SetFromFunction` directly in the executor:

In `PhaseCreateNodes`, replace the current block (lines 871-888):

```cpp
// For call ops that the resolver already resolved, use the UFunction*
// directly instead of re-resolving through FindFunction.
TMap<FString, FString> NodeProperties = Resolved.Properties;

if ((NodeType == OliveNodeTypes::CallFunction || NodeType == OliveNodeTypes::CallDelegate)
    && Resolved.ResolvedFunction != nullptr)
{
    // Signal NodeFactory to skip FindFunction entirely.
    // We'll call SetFromFunction ourselves after node creation.
    NodeProperties.Add(TEXT("_skip_function_resolve"), TEXT("true"));
}

FOliveBlueprintWriteResult WriteResult = Writer.AddNode(
    Context.AssetPath,
    Context.GraphName,
    NodeType,
    NodeProperties,
    PosX,
    PosY);

if (!WriteResult.bSuccess) { /* existing error handling */ }

const FString NodeId = WriteResult.CreatedNodeId;
UEdGraphNode* NodePtr = Writer.GetCachedNode(Context.AssetPath, NodeId);

// Apply resolved function reference directly
if (NodePtr && Resolved.ResolvedFunction != nullptr
    && (NodeType == OliveNodeTypes::CallFunction || NodeType == OliveNodeTypes::CallDelegate))
{
    if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(NodePtr))
    {
        CallNode->SetFromFunction(Resolved.ResolvedFunction);
        CallNode->ReconstructNode();

        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("  -> Applied ResolvedFunction '%s::%s' directly (skipped FindFunction)"),
            *Resolved.ResolvedFunction->GetOwnerClass()->GetName(),
            *Resolved.ResolvedFunction->GetName());
    }
}
```

**Wait -- this won't work.** `CreateNodeByClass` calls `SetFromFunction` BEFORE `AllocateDefaultPins`. If we do it after, we'd need `ReconstructNode()` which is expensive and could have side effects.

#### 3b. Revised approach: Pass pointer through NodeFactory

Add a new optional parameter path. In `OliveNodeFactory.h`, add:

```cpp
/**
 * Provide a pre-resolved UFunction* for the next CreateNodeByClass call.
 * When set, CreateNodeByClass will use this instead of calling FindFunction.
 * Consumed (reset to nullptr) after use.
 */
void SetPreResolvedFunction(UFunction* InFunction);
```

In `OliveNodeFactory.cpp`:

```cpp
// New member
UFunction* PreResolvedFunction = nullptr;

void FOliveNodeFactory::SetPreResolvedFunction(UFunction* InFunction)
{
    PreResolvedFunction = InFunction;
}
```

In `CreateNodeByClass`, the CallFunction block becomes:

```cpp
if (!FuncName.IsEmpty())
{
    UFunction* ResolvedFunc = PreResolvedFunction;
    PreResolvedFunction = nullptr; // Consume

    if (!ResolvedFunc)
    {
        // Fallback: resolve via FindFunctionEx (for non-plan callers like add_node tool)
        const bool bSkipAlias = Properties.Contains(TEXT("_resolved"));
        FOliveFunctionSearchResult SearchResult = FindFunctionEx(FuncName, FuncClassName, Blueprint, bSkipAlias);
        // ... existing retry + error handling ...
        ResolvedFunc = SearchResult.Function;
    }

    if (ResolvedFunc)
    {
        UK2Node_CallFunction* CallFuncNode = CastChecked<UK2Node_CallFunction>(NewNode);
        CallFuncNode->SetFromFunction(ResolvedFunc);
        // ... existing logging ...
    }
}
```

In the executor's `PhaseCreateNodes`:

```cpp
// Pass resolved function pointer directly to NodeFactory
if ((NodeType == OliveNodeTypes::CallFunction || NodeType == OliveNodeTypes::CallDelegate)
    && Resolved.ResolvedFunction != nullptr)
{
    FOliveNodeFactory::Get().SetPreResolvedFunction(Resolved.ResolvedFunction);
}

// Remove the _resolved flag injection -- no longer needed
FOliveBlueprintWriteResult WriteResult = Writer.AddNode(
    Context.AssetPath,
    Context.GraphName,
    NodeType,
    Resolved.Properties,
    PosX,
    PosY);
```

This is clean because:
- `SetPreResolvedFunction` is consumed immediately (reset to nullptr after use)
- Single-threaded (game thread only), so no race condition
- Non-plan callers (`add_node` tool) never call `SetPreResolvedFunction`, so they get the existing path
- `_resolved` flag and `bSkipAliasMap` parameter become dead code (can be cleaned up later)

### 4. Removing the `_resolved` Band-Aid

After this change, the `_resolved` property and `bSkipAliasMap` parameter on `FindFunction`/`FindFunctionEx` are no longer needed for the plan path. They should be kept temporarily for backward compatibility (the `add_node` granular tool doesn't go through the resolver), then deprecated in a future cleanup pass.

### 5. UPROPERTY Auto-Rewrite Details

When the resolver detects `PROPERTY MATCH:` in SearchedLocations:

**Input** (AI provides):
```json
{"op": "call", "target": "SetMaxWalkSpeed", "inputs": {"MaxWalkSpeed": "600"}}
```

**Resolver rewrites to**:
```
Out.NodeType = OliveNodeTypes::VariableSet
Out.Properties["variable_name"] = "MaxWalkSpeed"
Out.bIsPure = false
```

**Edge cases**:
- `SetMaxWalkSpeed` with inputs -> `set_var` for `MaxWalkSpeed`
- `GetMaxWalkSpeed` with no inputs -> `get_var` for `MaxWalkSpeed`
- `MaxWalkSpeed` (no prefix) with inputs -> `set_var` (inputs present = setter)
- `MaxWalkSpeed` (no prefix) with no inputs -> `get_var`

**Limitation**: This only fires when `FindFunctionEx` FAILS and detects a property match. If the function name happens to match a real function (unlikely for SetMaxWalkSpeed, but possible), it won't rewrite. This is correct behavior -- real functions should take precedence.

**Component variable scoping**: The UPROPERTY detection in FindFunctionEx already scans SCS component classes. The rewritten `set_var` step will need a `Target` pin wired to the component. The existing `ExpandMissingComponentTargets()` pre-pass handles this -- it detects variable names that belong to components and auto-injects `get_var` steps. However, `ExpandMissingComponentTargets` currently only checks `BlueprintHasVariable()` which looks at `NewVariables` and SCS node names (not their properties). A new step must be inserted that checks component class properties too.

**Decision**: Defer component-property scoping to a follow-up. For now, the UPROPERTY rewrite produces a `set_var` that may fail at execution time with `VARIABLE_NOT_FOUND`. The error message will guide the AI to use the correct component-scoped access pattern. This is still better than the current behavior (FUNCTION_NOT_FOUND with a hint buried in SearchedLocations).

## Risk Assessment

### UFunction* Lifetime
**Risk: LOW.** `UFunction*` pointers live on `UClass` objects, which are stable within a single frame. The resolver and executor run synchronously on the same game thread in the same `ExecuteWithOptionalConfirmation` call. Hot reload between resolve and execute is impossible.

### Threading
**Risk: NONE.** Both resolver and executor run on the game thread. `SetPreResolvedFunction` is consumed immediately in the same call stack.

### Cases Where Executor Could Resolve But Resolver Can't
**Risk: NONE identified.** The audit confirmed the executor's `FindFunction` call uses the same inputs (function name, class name, Blueprint pointer) that the resolver already provided. The `_resolved` flag on Properties proves the resolver's output was already being threaded through -- just as strings rather than a pointer.

### SetPreResolvedFunction Stale Pointer
**Risk: LOW.** The pointer is consumed (reset to nullptr) immediately after use. If `AddNode` throws before reaching the CallFunction block, the pointer would persist. Add a guard: reset `PreResolvedFunction` at the TOP of `CreateNodeByClass` (save it locally, then clear the member).

### Non-Plan Callers of NodeFactory
**Risk: NONE.** `SetPreResolvedFunction` is only called from the plan executor. The `add_node` tool, library cloner, and other callers of `CreateNodeByClass` never call it, so they get the existing `FindFunction` path unchanged.

## Task Breakdown

### Task 1: Add `ResolvedFunction` to `FOliveResolvedStep` and store it (Senior, ~15 lines)

**Files**:
- `Source/OliveAIEditor/Blueprint/Public/Plan/OliveBlueprintPlanResolver.h` -- add field
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` -- set in 3 success paths

### Task 2: Add `SetPreResolvedFunction` to NodeFactory (Senior, ~30 lines)

**Files**:
- `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h` -- add method + member
- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` -- implement, modify `CreateNodeByClass` CallFunction block

### Task 3: Executor uses `ResolvedFunction` (Junior, ~15 lines)

**Files**:
- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` -- call `SetPreResolvedFunction` before `AddNode`, remove `_resolved` property injection

### Task 4: UPROPERTY auto-rewrite in resolver (Senior, ~50 lines)

**Files**:
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` -- add auto-rewrite block before FUNCTION_NOT_FOUND error path

### Implementation Order

1. **Task 1** -- IR change (foundation)
2. **Task 2** -- NodeFactory accepts pre-resolved function
3. **Task 3** -- Executor threads the pointer through
4. Build + test plan_json with known alias-problematic functions (GetForwardVector)
5. **Task 4** -- UPROPERTY auto-rewrite
6. Build + test with CMC property calls (SetMaxWalkSpeed, SetMaxSpeed)
7. Cleanup: mark `_resolved` flag and `bSkipAliasMap` as deprecated in comments

**Total estimated new/modified lines**: ~110
**Files modified**: 4 (resolver .h, resolver .cpp, NodeFactory .h/.cpp, executor .cpp)
**No new files.**
