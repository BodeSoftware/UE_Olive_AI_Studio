# Run Regression Fixes Design

**Date:** 2026-03-12
**Scope:** 6 issues from two test runs. Mix of tool bugs, template gaps, and prompt/knowledge fixes.

---

## Issue 1: OnHealthChanged Dispatcher Collision with C++ Parent Class

**Classification:** TOOL BUG (P0)
**Impact:** 4 failed attempts across 2 runs. Entire damage notification system non-functional.
**Root cause:** Two independent failures.

### 1A: `AddEventDispatcher` does not check parent class properties

`OliveBlueprintWriter.cpp` line 1557-1566 only checks `BP->NewVariables` for duplicate dispatchers. It does NOT check the parent class chain for properties with the same name. When the C++ parent already declares `OnHealthChanged` as a `MulticastInlineDelegateProperty`, the dispatcher is silently created, then the compiler crashes with an internal error because two properties share the same name in the generated class scope.

Compare to `AddVariable` (line 839-851) which already has this check:
```cpp
// Check parent class chain for inherited properties with same name
if (Blueprint->ParentClass)
{
    FProperty* InheritedProp = Blueprint->ParentClass->FindPropertyByName(FName(*Variable.Name));
    if (InheritedProp) { /* return error */ }
}
```

**Fix:** Add the identical parent-class property check to `AddEventDispatcher`, immediately after the existing `NewVariables` loop (line 1566).

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveBlueprintWriter.cpp`
**Location:** After line 1566, before line 1568 (the transaction scope).

**Code to add (after the existing NewVariables duplicate check):**
```cpp
// Check parent class chain for inherited properties with same name.
// C++ parent classes may declare MulticastDelegateProperties that would
// collide with a Blueprint dispatcher of the same name, causing internal
// compiler errors ("Tried to create a property X in scope SKEL_Y, but
// another object already exists there").
if (Blueprint->ParentClass)
{
    FProperty* InheritedProp = Blueprint->ParentClass->FindPropertyByName(FName(*DispatcherName));
    if (InheritedProp)
    {
        UClass* OwningClass = InheritedProp->GetOwnerClass();

        // Determine if the inherited property is itself a multicast delegate
        bool bIsDelegate = CastField<FMulticastDelegateProperty>(InheritedProp) != nullptr;

        FString Suggestion = bIsDelegate
            ? FString::Printf(TEXT("Parent class '%s' already has dispatcher '%s'. "
                "Use call_delegate/bind_dispatcher to access it directly -- do NOT create a duplicate."),
                OwningClass ? *OwningClass->GetName() : TEXT("Unknown"), *DispatcherName)
            : FString::Printf(TEXT("Name '%s' conflicts with property on parent class '%s'. "
                "Use a different name (e.g., 'On%sChanged' or 'BP_%s')."),
                *DispatcherName, OwningClass ? *OwningClass->GetName() : TEXT("Unknown"),
                *DispatcherName, *DispatcherName);

        return FOliveBlueprintWriteResult::Error(Suggestion);
    }
}
```

**Estimated effort:** Junior, ~20 lines.

### 1B: `ResolveCallDelegateOp` only searches `NewVariables`, not parent class

`OliveBlueprintPlanResolver.cpp` lines 3130-3150 only iterates `BP->NewVariables` with `PC_MCDelegate` filter. When the dispatcher is declared in the C++ parent class (as a `MulticastDelegateProperty` on the `UClass`, not a `FBPVariableDescription`), the resolver fails with "DELEGATE_NOT_FOUND".

The same issue affects `ResolveBindDelegateOp` (lines 3282-3303) for the self-Blueprint search path.

**Fix:** After the `NewVariables` loop in both functions, add a parent-class `FMulticastDelegateProperty` search using `TFieldIterator`.

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

**Locations:**
1. `ResolveCallDelegateOp` -- after line 3150 (end of NewVariables loop), before the `if (!bFoundDispatcher)` check at line 3152.
2. `ResolveBindDelegateOp` -- after line 3303 (end of NewVariables loop, self-Blueprint search), before the `if (!bFoundDispatcher)` check at line 3305.

**Code to add (same pattern in both locations):**
```cpp
// If not found in NewVariables, search parent class chain for C++ declared
// MulticastDelegateProperties. These are valid dispatchers that can be
// called/bound from Blueprint but are NOT listed in NewVariables.
if (!bFoundDispatcher && BP && BP->ParentClass)
{
    for (TFieldIterator<FMulticastDelegateProperty> It(BP->ParentClass); It; ++It)
    {
        const FString PropName = It->GetFName().ToString();
        AvailableDispatchers.AddUnique(PropName);

        if (PropName == Step.Target)
        {
            bFoundDispatcher = true;
            // Mark as inherited for potential downstream use
            Out.Properties.Add(TEXT("inherited_dispatcher"), TEXT("true"));
        }
    }
}
```

Note: For `ResolveCallDelegateOp`, the `Out.Properties.Add(TEXT("delegate_name"), ...)` at line 3181 happens after this check, so we don't need to set it inside the new block. The existing code at line 3181 handles it.

For `ResolveBindDelegateOp`, the same pattern applies -- the existing post-check code sets properties correctly.

**Also collect inherited dispatchers for the "Available" list:** The `AvailableDispatchers` array already feeds into the error message. Adding inherited ones here means the error message will list them if the target still can't be found.

**Estimated effort:** Junior, ~15 lines per function, ~30 total.

### 1C: Auto-reroute in `ResolveCallOp` also only checks `NewVariables`

`OliveBlueprintPlanResolver.cpp` lines 1849-1878 -- the `call` -> `call_delegate` auto-reroute only searches `BP->NewVariables`. If the AI writes `{"op": "call", "target": "OnHealthChanged"}` and the dispatcher is inherited from C++, the auto-reroute misses it, and the step falls through to `FUNCTION_NOT_FOUND`.

**Fix:** After the `NewVariables` loop at line 1877, add a `TFieldIterator<FMulticastDelegateProperty>` search on `BP->ParentClass`.

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**Location:** After line 1877 (end of NewVariables dispatcher check), before line 1879 (the cast-target fallback comment).

**Code to add:**
```cpp
// Also check parent class chain for C++ declared MulticastDelegateProperties
if (BP->ParentClass)
{
    for (TFieldIterator<FMulticastDelegateProperty> It(BP->ParentClass); It; ++It)
    {
        const FString PropName = It->GetFName().ToString();
        if (PropName == Step.Target || PropName.Equals(Step.Target, ESearchCase::IgnoreCase))
        {
            UE_LOG(LogOlivePlanResolver, Log,
                TEXT("    ResolveCallOp step '%s': '%s' matches inherited dispatcher '%s'. Rerouting to call_delegate."),
                *Step.StepId, *Step.Target, *PropName);

            Out.ResolverNotes.Add(FOliveResolverNote{
                TEXT("op"), TEXT("call"), TEXT("call_delegate"),
                FString::Printf(TEXT("'%s' is an inherited event dispatcher from C++ parent"), *Step.Target)
            });

            return ResolveCallDelegateOp(Step, BP, Idx, Out, Errors);
        }
    }
}
```

**Estimated effort:** Junior, ~15 lines.

**Total for Issue 1:** ~65 lines across 2 files. All junior-level.

---

## Issue 2: Child BP Variable Duplication (Inherited Variable Visibility)

**Classification:** PROMPT/TOOL ENHANCEMENT (P1)
**Impact:** 6-8 wasted tool calls per run. Error messages are already good but the AI shouldn't need to discover them.

**Root cause:** `blueprint.create` success response (line 2241-2248) returns only `{asset_path, parent_class, type}`. It does not list inherited variables, components, or dispatchers. The AI has no way to know what the parent provides without doing a `blueprint.read` first.

**Fix:** Enrich the `blueprint.create` success response with a summary of inherited members. This prevents the AI from blindly adding variables that already exist.

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
**Location:** Lines 2240-2248 (success result builder inside the executor lambda).

**Implementation:**
After building the basic ResultData, enumerate parent-class properties and list inherited variables/dispatchers.

```cpp
// Enumerate inherited members so the AI knows what the parent provides
// and doesn't try to add duplicates.
UBlueprint* CreatedBP = Cast<UBlueprint>(
    StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath));
if (CreatedBP && CreatedBP->ParentClass)
{
    TArray<TSharedPtr<FJsonValue>> InheritedVars;
    TArray<TSharedPtr<FJsonValue>> InheritedDispatchers;

    for (TFieldIterator<FProperty> It(CreatedBP->ParentClass); It; ++It)
    {
        // Skip private/internal properties
        if (!It->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly))
            continue;
        if (It->GetName().StartsWith(TEXT("UberGraphFrame")) ||
            It->GetName().StartsWith(TEXT("bCanEverTick")))
            continue;

        if (CastField<FMulticastDelegateProperty>(*It))
        {
            InheritedDispatchers.Add(MakeShared<FJsonValueString>(It->GetName()));
        }
        else
        {
            // Build "Name (Type)" string for concise listing
            FString TypeName = It->GetCPPType();
            InheritedVars.Add(MakeShared<FJsonValueString>(
                FString::Printf(TEXT("%s (%s)"), *It->GetName(), *TypeName)));
        }
    }

    if (InheritedVars.Num() > 0)
    {
        // Cap at 30 to avoid bloating the response
        if (InheritedVars.Num() > 30)
        {
            int32 Total = InheritedVars.Num();
            InheritedVars.SetNum(30);
            InheritedVars.Add(MakeShared<FJsonValueString>(
                FString::Printf(TEXT("... and %d more"), Total - 30)));
        }
        ResultData->SetArrayField(TEXT("inherited_variables"), InheritedVars);
    }
    if (InheritedDispatchers.Num() > 0)
    {
        ResultData->SetArrayField(TEXT("inherited_dispatchers"), InheritedDispatchers);
    }

    // Also list inherited SCS components from parent BPs
    UBlueprint* ParentBP = Cast<UBlueprint>(
        CreatedBP->ParentClass->ClassGeneratedBy);
    if (ParentBP && ParentBP->SimpleConstructionScript)
    {
        TArray<TSharedPtr<FJsonValue>> InheritedComponents;
        TArray<USCS_Node*> AllNodes = ParentBP->SimpleConstructionScript->GetAllNodes();
        for (USCS_Node* Node : AllNodes)
        {
            if (Node && Node->ComponentTemplate)
            {
                InheritedComponents.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("%s (%s)"),
                        *Node->GetVariableName().ToString(),
                        *Node->ComponentClass->GetName())));
            }
        }
        if (InheritedComponents.Num() > 0)
        {
            ResultData->SetArrayField(TEXT("inherited_components"), InheritedComponents);
        }
    }
}
```

**Key detail:** The Blueprint object already exists at this point (we just created it in the executor lambda). The `WriteResult.AssetPath` gives us the path to load it.

However, there's a subtlety: inside the executor lambda, we're within a `FScopedTransaction`. The Blueprint was just created. Its `GeneratedClass` may not be compiled yet. `ParentClass` should be set though, since `CreateBlueprint` sets it.

**Alternative (simpler):** Instead of the lambda, add the inherited members to the result AFTER the pipeline returns, where we have access to the created Blueprint. This avoids concerns about loading inside the transaction.

Move the enrichment to after line 2253 (after `ExecuteWithOptionalConfirmation`), where `Result.CreatedItem` contains the asset path:

```cpp
// After line 2253:
if (Result.bSuccess && !AssetPath.IsEmpty())
{
    UBlueprint* CreatedBP = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (CreatedBP && CreatedBP->ParentClass && Result.ResultData.IsValid())
    {
        // ... enumerate inherited members and add to Result.ResultData ...
    }
}
```

Wait -- `Result` is `FOliveWriteResult` and `ResultData` is already set. We then convert to `FOliveToolResult` at line 2255. The best place is to add to `ToolResult.Data` after line 2255.

**Revised location:** After line 2255 (`FOliveToolResult ToolResult = Result.ToToolResult();`), before line 2257.

**Estimated effort:** Senior, ~50 lines. The parent-class reflection iteration needs care around property flags and capping.

---

## Issue 3: AttachToActor vs AttachToComponent

**Classification:** TEMPLATE/KNOWLEDGE (P1)
**Impact:** Gun attaches to capsule root instead of mesh socket, causing incorrect positioning.

**Root cause:** The gun factory template has no guidance about attachment patterns. The AI defaults to `AttachToActor` (simpler, wrong) instead of `AttachToComponent` (correct for socket-based attachment).

**Current alias map state** (OliveNodeFactory.cpp line 3244-3246):
```
AttachToActor -> K2_AttachToActor
AttachActorToComponent -> K2_AttachToComponent
AttachToComponent -> K2_AttachToComponent
```
Aliases are already correct. No code fix needed there.

### Fix 3A: Update gun template with attachment guidance

**File:** `Content/Templates/factory/gun.json`

Add a `notes` field at the template level (or extend the existing `catalog_description`) with attachment guidance.

**Add to the template JSON, after `catalog_examples`:**
```json
"usage_notes": "After spawning the gun, attach it to the character's skeletal mesh component using K2_AttachToComponent (NOT K2_AttachToActor). Pass the character's Mesh component as InParent, a socket name (e.g., 'weapon_r' or 'hand_r') as SocketName, and SnapToTarget as LocationRule/RotationRule/ScaleRule. Using AttachToActor attaches to the capsule root, which causes incorrect positioning.",
```

### Fix 3B: Update spawn_actor recipe

**File:** `Content/SystemPrompts/Knowledge/recipes/blueprint/spawn_actor.txt`

Add a line about attachment:
```
After spawning, if the new actor needs to follow another actor (e.g., weapon on character), use K2_AttachToComponent -- NOT K2_AttachToActor. AttachToComponent lets you specify a socket on the target mesh.
SetOwner on the spawned actor establishes the ownership chain (needed for GetInstigator to work).
```

**Estimated effort:** Junior, template/text edits only.

---

## Issue 4: SetInstigator Not Blueprint-Callable

**Classification:** KNOWLEDGE (P2)
**Impact:** 1 plan failure per run. AI wastes a tool call, then the retry drops the step.

**Root cause:** `AActor::SetInstigator` exists in C++ but is NOT a `UFUNCTION`. It's a plain C++ setter. There is no Blueprint-callable equivalent.

The proper UE5 pattern for setting instigator on a spawned actor:
1. `SetOwner(SpawningActor)` -- makes `GetInstigator()` walk the owner chain
2. `SpawnActorDeferred` with `Instigator` parameter (not available via `spawn_actor` op)
3. `SetInstigator` via `editor.run_python` (C++ reflection call)

The most practical fix is to add an alias that redirects `SetInstigator` to guidance, and update the projectile reference template.

### Fix 4A: Add knowledge to projectile_patterns reference template

**File:** `Content/Templates/reference/projectile_patterns.json`

Update the `ApplyDamageOnHit` pattern's notes:
```json
"notes": "Get the instigator controller via GetInstigatorController on the projectile. For directional damage use ApplyPointDamage instead, which also requires HitFromDirection (FVector) and HitInfo (FHitResult) from the collision event. Store a damage variable (e.g., BulletDamage) on the projectile and reference it in the damage call. NOTE: SetInstigator is NOT Blueprint-callable. Instead, call SetOwner on the spawned projectile immediately after SpawnActor -- GetInstigator walks the owner chain."
```

### Fix 4B: Add alias entry for SetInstigator

Not a true alias (there's no target function), but we can add a knowledge note. The best approach is to add `SetInstigator` to the "Common Mistakes" section in `cli_blueprint.txt`.

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

Add to the Common Mistakes section:
```
WRONG: call SetInstigator on spawned actor (SetInstigator is C++ only, NOT a UFUNCTION)
RIGHT: call SetOwner(SpawningActor) -- GetInstigator() walks the owner chain automatically
WHY: SetInstigator does not exist as a Blueprint-callable function. SetOwner is the standard UE5 pattern.
```

### Fix 4C: Add SetInstigator -> SetOwner alias with resolver note

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`

In the alias map, add:
```cpp
Map.Add(TEXT("SetInstigator"), TEXT("SetOwner"));
```

This makes `SetInstigator` calls auto-resolve to `SetOwner`. The resolver note system will show the translation.

Actually, this is slightly wrong -- `SetInstigator(APawn*)` takes a Pawn while `SetOwner(AActor*)` takes an Actor. The pin types differ. But `SetOwner` is the correct UE5 pattern and the AI can wire the correct actor.

**Estimated effort:** Junior, ~10 lines across 3 files.

---

## Issue 5: Bullet Collision with Spawning Player

**Classification:** TEMPLATE GAP (P1)
**Impact:** Every spawned projectile immediately self-destructs by hitting the player.

**Root cause:** The projectile template already sets `CollisionProfileName: BlockAllDynamic` on the collision sphere (line 117). The collision profile is correct. What's missing is guidance about self-collision avoidance.

The actual fix in UE5 for this is calling `MoveIgnoreActorAdd(GetOwner())` or `IgnoreActorWhenMoving(GetOwner(), true)` in the projectile's BeginPlay, so the projectile ignores its spawner.

### Fix 5A: Add self-collision avoidance to projectile_patterns reference template

**File:** `Content/Templates/reference/projectile_patterns.json`

Add a new pattern:
```json
{
    "name": "IgnoreSpawnerCollision",
    "description": "Projectiles must ignore their spawning actor to avoid immediate self-collision. The root collision component calls IgnoreActorWhenMoving or MoveIgnoreActorAdd with the Owner actor.",
    "notes": "Call SetOwner on the projectile at spawn time (from the gun/spawner), then in the projectile's BeginPlay: GetOwner -> IsValid check -> CollisionSphere.IgnoreActorWhenMoving(Owner, true). Without this, BlockAllDynamic causes instant self-hit."
}
```

### Fix 5B: Add guidance to spawn_actor recipe

Already covered in Fix 3B above -- the `SetOwner` line serves double duty (instigator chain + collision ignore setup).

### Fix 5C: Add guidance to gun factory template

**File:** `Content/Templates/factory/gun.json`

In the `usage_notes` field (added in Fix 3A), append:
```
After SpawnActor, call SetOwner(self) on the spawned projectile. The projectile's BeginPlay should then call IgnoreActorWhenMoving(GetOwner(), true) on its collision component to prevent self-collision.
```

**Estimated effort:** Junior, text edits only.

---

## Issue 6: Retry Plan Drops Working Steps

**Classification:** PROMPT/KNOWLEDGE (P2)
**Impact:** Spawned bullet never gets its damage value set after retry.

**Root cause:** When the AI retries a failed plan, it rewrites the entire plan from scratch. The self-correction guidance (`BuildRollbackAwareMessage`, line 968-978) says:

> "Fix the plan and resubmit with apply_plan_json"

But it does NOT say "only remove or fix the failing step -- keep all other steps intact." The AI interprets "fix" as "rewrite," and in the process drops working steps.

### Fix 6A: Add minimal-modification guidance to `BuildRollbackAwareMessage`

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`
**Location:** Line 968-978 (`BuildRollbackAwareMessage`)

Add to the `REQUIRED ACTION` section:
```
"REQUIRED ACTION: Fix ONLY the failing step and resubmit the COMPLETE plan with apply_plan_json.\n"
"CRITICAL: Keep ALL working steps intact. Only remove or modify the step that caused the error. "
"Do NOT simplify by dropping steps that were working -- that causes missing functionality.\n"
```

### Fix 6B: Add same guidance to `BuildToolErrorMessage` for plan-specific errors

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`
**Location:** Around line 912-920 (the `apply_plan_json` or `preview_plan_json` guidance block)

Check the existing guidance text for plan tools. Find the line that says "Fix the plan and resubmit." and extend it.

**Estimated effort:** Junior, ~10 lines of text changes.

---

## Implementation Order

### Phase 1: Critical Bug Fix (Issue 1) -- 1 PR

Must-fix. Blocks all C++ parent dispatcher workflows.

1. **1A**: Parent class property check in `AddEventDispatcher` (OliveBlueprintWriter.cpp)
2. **1B**: Parent class dispatcher search in `ResolveCallDelegateOp` and `ResolveBindDelegateOp` (OliveBlueprintPlanResolver.cpp)
3. **1C**: Parent class dispatcher search in `ResolveCallOp` auto-reroute (OliveBlueprintPlanResolver.cpp)

### Phase 2: Create Response Enrichment (Issue 2) -- same PR as Phase 1

Should-fix. Prevents 6-8 wasted calls per run.

4. **2**: Add inherited_variables/inherited_dispatchers/inherited_components to `blueprint.create` success response (OliveBlueprintToolHandlers.cpp)

### Phase 3: Template and Knowledge Fixes (Issues 3, 4, 5, 6) -- 1 PR

These are all text/JSON edits with no code risk.

5. **3A**: Gun template `usage_notes` with AttachToComponent guidance (gun.json)
6. **3B**: Spawn_actor recipe attachment + SetOwner guidance (spawn_actor.txt)
7. **4A**: Projectile patterns SetInstigator clarification (projectile_patterns.json)
8. **4B**: Common Mistakes SetInstigator entry (cli_blueprint.txt)
9. **4C**: SetInstigator -> SetOwner alias (OliveNodeFactory.cpp)
10. **5A**: IgnoreSpawnerCollision pattern (projectile_patterns.json)
11. **5C**: Gun template self-collision guidance (gun.json)
12. **6A**: Minimal-modification guidance in BuildRollbackAwareMessage (OliveSelfCorrectionPolicy.cpp)
13. **6B**: Same guidance in BuildToolErrorMessage for plan errors (OliveSelfCorrectionPolicy.cpp)

---

## File Summary

| File | Changes |
|------|---------|
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveBlueprintWriter.cpp` | 1A: parent property check in AddEventDispatcher |
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | 1B: parent dispatcher search in ResolveCallDelegateOp + ResolveBindDelegateOp. 1C: same in ResolveCallOp auto-reroute |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | 2: inherited member listing in blueprint.create response |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | 4C: SetInstigator alias |
| `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | 6A+6B: minimal-modification guidance |
| `Content/Templates/factory/gun.json` | 3A+5C: attachment + self-collision notes |
| `Content/Templates/reference/projectile_patterns.json` | 4A+5A: SetInstigator note + IgnoreSpawner pattern |
| `Content/SystemPrompts/Knowledge/recipes/blueprint/spawn_actor.txt` | 3B: attachment + SetOwner guidance |
| `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | 4B: SetInstigator common mistake |

---

## Risk Assessment

| Change | Risk | Rationale |
|--------|------|-----------|
| 1A (AddEventDispatcher parent check) | LOW | Same pattern as existing AddVariable check, pre-pipeline validation |
| 1B (resolver parent dispatcher) | LOW | Additive search, only activates when NewVariables search misses |
| 1C (call -> call_delegate auto-reroute) | LOW | Additive, only runs after FindFunctionEx fails |
| 2 (create response enrichment) | MEDIUM | Reflection iteration on parent class; cap at 30 entries. Risk of slow response for deep hierarchies. Mitigate with property flag filtering. |
| 3-6 (text/template/alias) | NONE | No behavior change, purely informational |

---

## Notes for Coder

1. Issue 1A-1C are structurally identical -- parent class property/delegate iteration. Copy the exact same `TFieldIterator<FMulticastDelegateProperty>` pattern used in `ResolveBindDelegateOp` cross-Blueprint path (lines 3244-3267).

2. For Issue 2, the `FOliveWriteResult.ResultData` might not be directly accessible from outside the lambda. The simplest approach: after `FOliveToolResult ToolResult = Result.ToToolResult();` (line 2255), check `ToolResult.bSuccess`, load the Blueprint, iterate parent class, and add fields to `ToolResult.Data`.

3. The `SetInstigator -> SetOwner` alias (4C) is a slight semantic mismatch -- different parameter types. But it's the accepted UE5 pattern and will succeed at the `FindFunction` level. The AI will need to wire the correct actor (not a Pawn specifically), which is fine since `SetOwner` takes `AActor*`.

4. For Issue 5, the projectile template already has `BlockAllDynamic`. The missing piece is runtime self-collision avoidance, which can't be configured as a component property -- it needs code in BeginPlay. The guidance in templates/recipes is the correct fix level.
