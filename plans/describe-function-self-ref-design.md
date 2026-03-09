# Design: `blueprint.describe_function` Tool + `@self` Reference Fix

Two independent changes. No new files.

---

## Change 1: `blueprint.describe_function` MCP Tool

### Root Cause

The AI Builder frequently guesses function names and pin names from LLM training memory. `blueprint.describe_node_type` answers a different question -- it describes K2Node *class* structure (generic CallFunction pins), not the UFunction-specific signature. When the AI is uncertain whether `SetActorLocation` or `SetWorldLocation` is correct, or what the exact pin names are, it has no tool to ask. This causes avoidable FUNCTION_NOT_FOUND and DATA_PIN_NOT_FOUND errors.

### Proposed Solution

Expose `FOliveNodeFactory::FindFunctionEx()` as a new read-only MCP tool. Reuse the same signature formatting logic from `BuildFunctionPinReference()` in OliveAgentPipeline.cpp (lines 3527-3584).

### Files Modified

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` | Add `BlueprintDescribeFunction()` schema method |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Add `HandleDescribeFunction()` handler; register in `RegisterReaderTools()` |
| `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h` | Declare `HandleDescribeFunction()` method |

### Schema (OliveBlueprintSchemas.cpp)

Add after `BlueprintDescribeNodeType()` (around line 395):

```cpp
TSharedPtr<FJsonObject> BlueprintDescribeFunction()
{
    TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
    TSharedPtr<FJsonObject> Props = MakeProperties();

    Props->SetObjectField(TEXT("function_name"), StringProp(
        TEXT("Function name to look up (e.g., 'SetActorLocation', 'ApplyDamage', 'GetVelocity'). "
             "Accepts aliases (e.g., 'SetTimer' resolves to 'K2_SetTimer').")));

    Props->SetObjectField(TEXT("target_class"), StringProp(
        TEXT("Optional class to search first (e.g., 'CharacterMovementComponent', 'ACharacter'). "
             "If omitted, searches alias map, common libraries, and all UBlueprintFunctionLibrary subclasses.")));

    Props->SetObjectField(TEXT("path"), StringProp(
        TEXT("Optional Blueprint asset path for context-aware search. "
             "Enables searching the Blueprint's own functions, parent hierarchy, SCS components, and interfaces.")));

    Schema->SetStringField(TEXT("description"),
        TEXT("Look up a UFunction by name and return its exact signature: parameter names, types, "
             "by-ref flags, return type, pure/latent markers, and owning class. "
             "On failure, returns fuzzy suggestions and UPROPERTY detection. "
             "Use this BEFORE writing plan_json to verify function names and pin names."));
    Schema->SetObjectField(TEXT("properties"), Props);
    AddRequired(Schema, {TEXT("function_name")});
    return Schema;
}
```

### Registration (OliveBlueprintToolHandlers.cpp, in `RegisterReaderTools()`)

Add after the `blueprint.describe_node_type` registration block (after line 520):

```cpp
// blueprint.describe_function
Registry.RegisterTool(
    TEXT("blueprint.describe_function"),
    TEXT("Look up a UFunction by name and return its exact pin signature, or fuzzy suggestions on failure"),
    OliveBlueprintSchemas::BlueprintDescribeFunction(),
    FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleDescribeFunction),
    {TEXT("blueprint"), TEXT("read"), TEXT("discovery")},
    TEXT("blueprint")
);
RegisteredToolNames.Add(TEXT("blueprint.describe_function"));
```

Update the tool count log from `3` to `4`.

### Handler Declaration (OliveBlueprintToolHandlers.h)

Add after `HandleDescribeNodeType` declaration (after line 84):

```cpp
/** Look up a UFunction and return its signature or fuzzy suggestions */
FOliveToolResult HandleDescribeFunction(const TSharedPtr<FJsonObject>& Params);
```

### Handler Implementation (OliveBlueprintToolHandlers.cpp)

Add after `HandleDescribeNodeType` (after line 1773). Logic outline:

```cpp
FOliveToolResult FOliveBlueprintToolHandlers::HandleDescribeFunction(const TSharedPtr<FJsonObject>& Params)
{
    // 1. Validate params -- function_name required
    FString FunctionName;
    if (!Params || !Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
    {
        return FOliveToolResult::Error("VALIDATION_MISSING_PARAM", ...);
    }

    // 2. Optional: target_class, path (for Blueprint context)
    FString TargetClass;
    Params->TryGetStringField(TEXT("target_class"), TargetClass);

    FString AssetPath;
    Params->TryGetStringField(TEXT("path"), AssetPath);

    // 3. Load Blueprint if path provided (for context-aware search)
    UBlueprint* Blueprint = nullptr;
    if (!AssetPath.IsEmpty())
    {
        // Use FOliveAssetResolver::LoadBlueprint() or direct LoadObject
        // If load fails, proceed without Blueprint context (don't error)
    }

    // 4. Call FindFunctionEx()
    FOliveFunctionSearchResult SearchResult =
        FOliveNodeFactory::Get().FindFunctionEx(FunctionName, TargetClass, Blueprint);

    // 5a. SUCCESS PATH -- format signature
    if (SearchResult.IsValid())
    {
        UFunction* Func = SearchResult.Function;
        TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());

        ResultData->SetStringField(TEXT("function_name"), Func->GetName());
        ResultData->SetStringField(TEXT("owning_class"), SearchResult.MatchedClassName);
        ResultData->SetStringField(TEXT("match_method"),
            /* convert EOliveFunctionMatchMethod to string */);

        // Check if alias resolved
        if (!FunctionName.Equals(Func->GetName(), ESearchCase::IgnoreCase))
        {
            ResultData->SetStringField(TEXT("resolved_from"), FunctionName);
        }

        // Format signature -- reuse pattern from BuildFunctionPinReference
        // Iterate TFieldIterator<FProperty>(Func), skip CPF_ReturnParm first pass
        TArray<TSharedPtr<FJsonValue>> ParamsArray;
        FProperty* ReturnProp = nullptr;

        for (TFieldIterator<FProperty> It(Func); It; ++It)
        {
            FProperty* Param = *It;
            if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                ReturnProp = Param;
                continue;
            }
            if (!Param->HasAnyPropertyFlags(CPF_Parm))
                continue;

            // Skip hidden WorldContextObject
            if (Param->GetName() == TEXT("WorldContextObject"))
                continue;

            TSharedPtr<FJsonObject> ParamObj = MakeShareable(new FJsonObject());
            ParamObj->SetStringField(TEXT("name"), Param->GetName());
            ParamObj->SetStringField(TEXT("type"), Param->GetCPPType());
            ParamObj->SetStringField(TEXT("direction"),
                Param->HasAnyPropertyFlags(CPF_OutParm) ? TEXT("out") : TEXT("in"));

            if (Param->HasAllPropertyFlags(CPF_OutParm | CPF_ReferenceParm))
            {
                ParamObj->SetBoolField(TEXT("by_ref"), true);
            }

            ParamsArray.Add(MakeShareable(new FJsonValueObject(ParamObj)));
        }

        ResultData->SetArrayField(TEXT("parameters"), ParamsArray);

        if (ReturnProp)
        {
            ResultData->SetStringField(TEXT("return_type"), ReturnProp->GetCPPType());
        }

        ResultData->SetBoolField(TEXT("is_pure"),
            Func->HasAnyFunctionFlags(FUNC_BlueprintPure));
        ResultData->SetBoolField(TEXT("is_latent"),
            Func->HasMetaData(FBlueprintMetadata::MD_Latent));

        // Also include a compact one-line signature for quick reference
        // e.g., "SetActorLocation(NewLocation: FVector, bSweep: bool, ...) [latent]"
        ResultData->SetStringField(TEXT("signature"), /* build one-liner */);

        return FOliveToolResult::Success(ResultData);
    }

    // 5b. FAILURE PATH -- fuzzy suggestions + UPROPERTY detection + search trail
    TSharedPtr<FJsonObject> ErrorData = MakeShareable(new FJsonObject());
    ErrorData->SetStringField(TEXT("searched_function"), FunctionName);

    // Search trail
    TArray<TSharedPtr<FJsonValue>> TrailArray;
    TArray<FString> PropertyMatches;
    TArray<FString> SearchTrail;

    for (const FString& Location : SearchResult.SearchedLocations)
    {
        if (Location.StartsWith(TEXT("PROPERTY MATCH:")))
        {
            PropertyMatches.Add(Location);
        }
        else
        {
            SearchTrail.Add(Location);
            TrailArray.Add(MakeShareable(new FJsonValueString(Location)));
        }
    }
    ErrorData->SetArrayField(TEXT("search_trail"), TrailArray);

    // Fuzzy suggestions from NodeFactory
    const TArray<FString>& FuzzySuggestions = FOliveNodeFactory::Get().LastFuzzySuggestions;
    if (FuzzySuggestions.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> SuggestArray;
        for (const FString& S : FuzzySuggestions)
        {
            SuggestArray.Add(MakeShareable(new FJsonValueString(S)));
        }
        ErrorData->SetArrayField(TEXT("suggestions"), SuggestArray);
    }

    // UPROPERTY detection
    if (PropertyMatches.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> PropArray;
        for (const FString& PM : PropertyMatches)
        {
            PropArray.Add(MakeShareable(new FJsonValueString(PM)));
        }
        ErrorData->SetArrayField(TEXT("property_matches"), PropArray);
    }

    FString Suggestion = TEXT("Check the search trail and suggestions. ");
    if (PropertyMatches.Num() > 0)
    {
        Suggestion += TEXT("The name matches a UPROPERTY -- use get_var/set_var instead of call.");
    }
    else if (FuzzySuggestions.Num() > 0)
    {
        Suggestion += FString::Printf(TEXT("Did you mean: %s?"),
            *FString::Join(FuzzySuggestions, TEXT(", ")));
    }

    return FOliveToolResult::Error(
        TEXT("FUNCTION_NOT_FOUND"),
        FString::Printf(TEXT("Function '%s' not found"), *FunctionName),
        Suggestion,
        ErrorData  // attach structured data
    );
}
```

### Edge Cases

1. **Alias resolution**: If `FunctionName` is an alias (e.g., `SetTimer` -> `K2_SetTimer`), include `resolved_from` in the response so the AI knows the real name.
2. **Blueprint-scoped functions**: If `path` points to a Blueprint with user-defined functions, those are searchable via `FunctionGraph` match method.
3. **Interface functions**: Returns `InterfaceSearch` match method -- the AI should know this requires `UK2Node_Message`.
4. **UPROPERTY detection**: `FindFunctionEx` already populates `PROPERTY MATCH:` entries in `SearchedLocations` (added in error-messages-08g). Surface these prominently.
5. **WorldContextObject**: Skip in the parameter list (it's a hidden pin in Blueprint).
6. **Thread safety**: `FindFunctionEx` accesses UE reflection. This runs on the game thread via MCP dispatch, which is correct.
7. **No Blueprint context**: When `path` is omitted, the search still covers alias map, common libraries, and universal library scan. Only Blueprint-specific functions (user-defined, SCS components, parent hierarchy) are missed.

### Difference from `describe_node_type`

| Aspect | `describe_node_type` | `describe_function` |
|--------|---------------------|---------------------|
| Input | K2Node class name or short name | UFunction name |
| Resolves | K2Node UClass via StaticFindFirstObject | UFunction via 7+1 step search |
| Output | Generic node pins (e.g., CallFunction shows only exec+self+target) | Function-specific parameters with exact names and types |
| Use case | "What pins does a Branch node have?" | "Does SetActorLocation exist? What are its params?" |
| Context | Stateless (scratch Blueprint) | Blueprint-aware (finds user functions, components) |

---

## Change 2: `@self` Reference Fix in Plan Resolver

### Root Cause

The AI sometimes writes `"target": "@self"` or `"some_input": "@self"` in plan_json inputs. There are two code paths where this is processed:

1. **Resolver (`ExpandComponentRefs`)** -- handles bare `@refs` (no dot). Currently checks SCS components, Blueprint variables, and function params. `@self` matches none of these, so it passes through unchanged.

2. **Executor (`PhaseWireData` -> `WireDataConnection` -> `ParseDataRef`)** -- requires a dot in the `@ref` format (`@stepId.pinHint`). Bare `@self` has no dot, so `ParseDataRef` returns false, generating `"Invalid @ref format: '@self'. Expected '@stepId.pinHint'"`.

However, `@self.pinHint` (with a dot) already works correctly (line 2591 of OlivePlanExecutor.cpp) -- it creates a `UK2Node_Self` and wires it. The problem is only with the **dotless** `@self` form.

In Blueprint, the `Target` (self) pin auto-wires to the owning actor by default. Writing `"target": "@self"` is redundant -- the function call node already targets self when Target is unconnected.

### Proposed Solution

Handle `@self` in `ExpandComponentRefs` in the resolver. This is the correct place because:
- It runs before the executor, so we catch it early
- It already handles all other bare `@ref` patterns
- The resolver is the "forgive and rewrite" layer; the executor is the "execute precisely" layer

Two cases:

**Case A: Bare `@self` (no dot)** -- e.g., `"target": "@self"`
- Strip the input entirely (remove it from the step's Inputs map). The Target pin auto-wires to self by default.
- Emit a resolver note explaining the no-op.

**Case B: `@self.PinHint` (with dot)** -- e.g., `"target": "@self.ReturnValue"`
- Already works via the executor's `@self` handler at line 2591. No change needed.

### Files Modified

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | Add `@self` guard in `ExpandComponentRefs` |

### Code Location

In `ExpandComponentRefs()`, in the `else` block for "No dot" bare refs (line 1003-1107), add a `@self` check **before** the SCS component check (before line 1007):

```cpp
else
{
    // No dot -- this is a bare @ComponentName or @VarName or @self

    // Handle @self -- Target auto-wires to self by default, so this is a no-op.
    // Strip it from the inputs to prevent ParseDataRef from choking on it.
    if (RefBody.Equals(TEXT("self"), ESearchCase::IgnoreCase))
    {
        RewrittenInputs.Add(PinName, FString()); // empty = mark for removal
        bExpanded = true;

        FOliveResolverNote Note;
        Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
        Note.OriginalValue = Value;
        Note.ResolvedValue = TEXT("(removed)");
        Note.Reason = TEXT("@self is redundant -- Target pins auto-wire to self by default. Stripped.");
        OutNotes.Add(MoveTemp(Note));

        UE_LOG(LogOlivePlanResolver, Verbose,
            TEXT("ExpandComponentRefs: Stripped redundant @self reference from step '%s' input '%s'"),
            *Step.StepId, *PinName);
        continue;
    }

    // Check if it matches an SCS component
    if (SCSComponentNames.Contains(RefBody))
    { ... existing code ... }
```

Then in the "Apply rewrites" section (line 1110-1114), modify to handle removal:

```cpp
// Apply rewrites
for (const auto& Rewrite : RewrittenInputs)
{
    if (Rewrite.Value.IsEmpty())
    {
        Step.Inputs.Remove(Rewrite.Key);  // @self removal
    }
    else
    {
        Step.Inputs[Rewrite.Key] = Rewrite.Value;
    }
}
```

### Edge Cases

1. **`@self` on non-Target pins** -- e.g., `"damage_causer": "@self"`. This is semantically correct (pass self as a parameter), but stripping it would lose the intent. However, in practice the AI almost exclusively uses `@self` for `target`. For non-target pins where the AI genuinely wants to pass self as a value, `@self.auto` is the correct form and already works via `ParseDataRef` + the executor's `@self` handler. Stripping bare `@self` is still the right call because:
   - If the AI meant "pass self as value", it should write `@self.auto` (dotted form)
   - Bare `@self` on non-target pins is ambiguous and almost always a mistake
   - The resolver note documents what happened so the AI can correct if needed

2. **Case sensitivity** -- Use `ESearchCase::IgnoreCase` to catch `@Self`, `@SELF`.

3. **`@self.X` with dot** -- NOT handled here (the `DotIndex != INDEX_NONE` branch runs). Already works via the executor at line 2591. No change needed.

4. **Empty RewrittenInputs value convention** -- The current code never uses empty string as a rewrite value, so using it as a removal sentinel is safe. But explicitly check before the existing `Step.Inputs[Rewrite.Key] = Rewrite.Value` to avoid inserting empty strings.

---

## Assignment Recommendation

### Change 1: `blueprint.describe_function` -- Senior

**Rationale:**
- Touches 3 code sites (schema, registration, handler)
- Handler is ~100-120 lines with UFunction reflection iteration
- Requires understanding `FindFunctionEx` return semantics, `PROPERTY MATCH:` format, `LastFuzzySuggestions`
- Must correctly format CPP types, detect by-ref vs out-only, handle `WorldContextObject` skip
- The error path is as important as the success path (structured suggestions)
- Pattern reuse from `BuildFunctionPinReference` requires judgment about what to copy vs simplify

### Change 2: `@self` reference fix -- Junior

**Rationale:**
- Single code site, single file
- Guard clause pattern (if X, strip and continue)
- Follows established pattern in the same function (bare `@ComponentName` handling)
- Small surface area, clear test case: write `"target": "@self"` in plan_json, verify no error
- Only edge case is the rewrite-map removal sentinel, which is straightforward

---

## Implementation Order

1. **Change 2 first** (`@self` fix) -- it's smaller, independently testable, and removes a class of errors that the AI encounters frequently. The junior can complete this quickly.

2. **Change 1 second** (`describe_function`) -- builds on understanding of the handler/schema pattern. Once available, it reduces the need for the `@self` workaround in the first place, because the AI can verify function signatures before writing plan_json.

---

## Testing

### Change 1 Tests

Manual via MCP:
- `blueprint.describe_function(function_name="SetActorLocation")` -- should return params: NewLocation (FVector), bSweep (bool), bTeleport (bool)
- `blueprint.describe_function(function_name="SetTimer")` -- should resolve alias to K2_SetTimer, show `resolved_from`
- `blueprint.describe_function(function_name="MaxWalkSpeed")` -- should return FUNCTION_NOT_FOUND with PROPERTY MATCH
- `blueprint.describe_function(function_name="NonexistentFunc")` -- should return search trail + fuzzy suggestions
- `blueprint.describe_function(function_name="MyCustomFunc", path="/Game/BP_Test")` -- should find user-defined function

### Change 2 Tests

Manual via plan_json:
- Step with `"target": "@self"` -- should silently strip, no error
- Step with `"target": "@self.auto"` -- should work via existing executor path (no change)
- Step with `"damage_causer": "@self"` -- should strip with resolver note
- Verify resolver note appears in preview output
