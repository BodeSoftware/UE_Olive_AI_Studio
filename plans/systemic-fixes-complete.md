# Systemic Fixes: Categories, Not Whack-a-Mole

## The Pattern

The gun task took 10 min instead of 3 because of two failures that share a root cause: **the system assumes the AI will use the exact right name and the exact right op for everything, and has no fallbacks when it doesn't.**

Every future task will hit the same walls with different functions. Here are the five categories and the fix for each.

---

## 1. FindFunction Only Searches 11 Classes

**The problem:** `FindFunction` has a hardcoded list of 11 library classes. Any function not on those classes fails. The AI tried `K2_SetTimerByFunctionName` — it's on `UKismetSystemLibrary`, which IS in the list, but the function wasn't found (likely a UE 5.5 rename or it's actually a latent K2Node, not a plain UFunction).

**What will break next:** Every function on a class not in the list:
- `UNavigationSystemV1` — AI MoveTo, FindPathToLocation
- `UWidgetBlueprintLibrary` — CreateWidget, SetInputMode  
- `UAbilitySystemBlueprintLibrary` — GAS functions
- `UDataTableFunctionLibrary` — GetDataTableRow
- `UAIBlueprintHelperLibrary` — SpawnAIFromClass, GetAIController
- `UPhysicsHandleComponent` — Grab, Release
- Any project-specific `UBlueprintFunctionLibrary` subclass
- Any plugin that exposes Blueprint functions

**The fix: Universal fallback search.** After the 11 hardcoded classes fail, iterate ALL loaded classes that inherit from `UBlueprintFunctionLibrary`. This is one code change that covers every current and future function library.

```cpp
// After the hardcoded library search fails, before returning nullptr:

// Step 6: Universal Blueprint Function Library search
for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
{
    UClass* TestClass = *ClassIt;
    if (TestClass->IsChildOf(UBlueprintFunctionLibrary::StaticClass()) 
        && !LibraryClassesSet.Contains(TestClass))  // skip already-searched
    {
        UFunction* Func = TryClassWithK2(TestClass);
        if (Func)
        {
            UE_LOG(LogOliveNodeFactory, Log, 
                TEXT("FindFunction('%s'): found in library class '%s' (universal search)"),
                *ResolvedName, *TestClass->GetName());
            return Func;
        }
    }
}
```

**Cost:** O(n) over loaded function library classes. ~50-100 classes typically. Only runs after 5 cheaper searches fail. Acceptable.

**Also:** For timer specifically, the alias is wrong. `SetTimerByFunctionName` maps to `K2_SetTimer` but the real function is `K2_SetTimerByFunctionName`. Fix the alias AND add the universal fallback so the next wrong alias doesn't block for 7 minutes.

Wrong aliases to fix now:
```cpp
// WRONG:
Map.Add(TEXT("SetTimerByFunctionName"), TEXT("K2_SetTimer"));
Map.Add(TEXT("SetTimer"), TEXT("K2_SetTimer"));
// RIGHT:
Map.Add(TEXT("SetTimerByFunctionName"), TEXT("K2_SetTimerByFunctionName"));
Map.Add(TEXT("SetTimer"), TEXT("K2_SetTimerByFunctionName"));
Map.Add(TEXT("K2_SetTimerByFunctionName"), TEXT("K2_SetTimerByFunctionName")); // pass-through for AI using internal name
Map.Add(TEXT("ClearTimer"), TEXT("K2_ClearTimerHandle"));
Map.Add(TEXT("ClearTimerByFunctionName"), TEXT("K2_ClearAndInvalidateTimerByFunctionName"));
```

---

## 2. `call` Op Doesn't Fall Back to `call_delegate`

**The problem:** The `call_delegate` op exists, works perfectly, and IS documented in the prompt. The AI still used `op: "call"` with `target: "OnFired"`. When `call` fails FindFunction, it errors without checking if the target is an event dispatcher.

**What will break next:** Every time the AI calls an event dispatcher via `call` instead of `call_delegate`. Which will be often — AI models default to the most general op.

**The fix: Auto-reroute in ResolveCallOp.** When FindFunction fails, before erroring, check if the target matches an event dispatcher name on the Blueprint. If yes, internally reroute to `ResolveCallDelegateOp`.

```cpp
// In ResolveCallOp, after FindFunction returns nullptr:

// Before erroring: check if this is an event dispatcher
if (BP)
{
    for (const FBPVariableDescription& Var : BP->NewVariables)
    {
        if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate
            && Var.VarName.ToString() == Step.Target)
        {
            UE_LOG(LogOlivePlanResolver, Log,
                TEXT("    ResolveCallOp: '%s' not found as function, but matches event dispatcher. Rerouting to call_delegate."),
                *Step.Target);
            
            // Reroute to call_delegate
            return ResolveCallDelegateOp(Step, BP, Idx, Out, Errors);
        }
    }
}

// Then fall through to the existing error with "did you mean?" suggestions
```

**Cost:** One linear scan of Blueprint variables. Only runs on call failures. Zero risk — if the dispatcher isn't found, it falls through to the existing error path.

**This pattern generalizes.** Consider the same approach for:
- Target matches a Blueprint-defined function name → reroute to local call (already handled by FN-5)
- Target matches an interface function → reroute to interface message call (FN-4)

The `call` op becomes a smart dispatcher that tries multiple resolution strategies before failing.

---

## 3. K2Node Properties Can't Be Set Through Reflection

**The problem:** `add_node` and `set_node_property` use `SetNodePropertiesViaReflection()` which only finds UProperties exposed through the reflection system. But many important K2Node properties are plain C++ members set through specific APIs:

- `UK2Node_CallDelegate` → `DelegatePropertyName` (needs `SetFromProperty()`)
- `UK2Node_ComponentBoundEvent` → `DelegatePropertyName`, `ComponentPropertyName` (needs `InitializeComponentBoundEventParams()`)
- `UK2Node_Timeline` → `TimelineName` (needs direct member set + graph creation)

**What will break next:** Any `add_node` call that needs to configure these nodes. The AI will try, get "Property not found", and spend 4+ minutes on workarounds.

**The fix: K2Node property bridge.** In `SetNodePropertiesViaReflection`, after the standard UProperty lookup fails, check if the node is a known K2Node subclass with a manual property API.

```cpp
// In SetNodePropertiesViaReflection, when UProperty lookup fails for a property:

if (!bFoundProperty)
{
    // K2Node-specific property bridge
    if (UK2Node_CallDelegate* DelegateNode = Cast<UK2Node_CallDelegate>(Node))
    {
        if (PropertyName == TEXT("DelegatePropertyName") || PropertyName == TEXT("delegate_name"))
        {
            // Find the delegate FMulticastDelegateProperty on the BP class
            // and call SetFromProperty
            bFoundProperty = SetDelegateNodeProperty(DelegateNode, PropertyValue, Blueprint);
        }
    }
    else if (UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
    {
        if (PropertyName == TEXT("DelegatePropertyName") || PropertyName == TEXT("ComponentPropertyName"))
        {
            bFoundProperty = SetComponentBoundEventProperty(BoundEvent, PropertyName, PropertyValue, Blueprint);
        }
    }
    else if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
    {
        if (PropertyName == TEXT("TimelineName"))
        {
            TimelineNode->TimelineName = PropertyValue;
            bFoundProperty = true;
        }
    }
}
```

**This is a bridging pattern, not infinite special-cases.** Only ~5-6 K2Node subclasses have non-reflected properties that matter for Blueprint creation. The rest work fine through reflection or through dedicated plan_json ops.

**The specific nodes to bridge:**
1. `UK2Node_CallDelegate` — DelegatePropertyName (SetFromProperty)
2. `UK2Node_ComponentBoundEvent` — DelegatePropertyName, ComponentPropertyName (InitializeComponentBoundEventParams)  
3. `UK2Node_Timeline` — TimelineName
4. `UK2Node_InputAction` — InputAction reference (for legacy input)
5. `UK2Node_SetFieldsInStruct` — StructType

---

## 4. No Plan-JSON Op for Component Events

**The problem:** When the AI wants `OnComponentBeginOverlap` or `OnComponentHit`, there's no plan_json op for it. The `event` op handles native events (BeginPlay, Tick) and the EventNameMap. Component delegate events are a different node type entirely (`UK2Node_ComponentBoundEvent`).

**What will break next:** Any Blueprint with collision detection, overlap triggers, physics interactions, component hit events. These are in probably 40%+ of all gameplay Blueprints.

**The fix: Extend the `event` op to handle component events.** The resolver already differentiates between native events and other cases. Add component event detection:

```cpp
// In ResolveEventOp, after native event lookup fails:

// Check if this is a component delegate event
// Pattern: "OnComponentHit", "OnComponentBeginOverlap(CollisionComp)", etc.
FString ComponentName;
FString DelegateName = Step.Target;

// Check if component name is specified in properties
if (Step.Properties.Contains(TEXT("component_name")))
{
    ComponentName = Step.Properties[TEXT("component_name")];
}

// Try to find a matching delegate on SCS components
if (BP && BP->SimpleConstructionScript)
{
    for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
    {
        if (!SCSNode || !SCSNode->ComponentClass) continue;
        
        // If component specified, only check that one
        if (!ComponentName.IsEmpty() && SCSNode->GetVariableName().ToString() != ComponentName)
            continue;
        
        // Search for delegate property matching the event name
        for (TFieldIterator<FMulticastDelegateProperty> It(SCSNode->ComponentClass); It; ++It)
        {
            if (It->GetName() == DelegateName || 
                It->GetName() == (TEXT("On") + DelegateName))
            {
                // Found it — set up as ComponentBoundEvent
                Out.NodeType = OliveNodeTypes::ComponentBoundEvent; // need to add this
                Out.Properties.Add(TEXT("DelegatePropertyName"), It->GetName());
                Out.Properties.Add(TEXT("ComponentPropertyName"), SCSNode->GetVariableName().ToString());
                return true;
            }
        }
    }
}
```

**Plan syntax would be:**
```json
{"step_id": "on_hit", "op": "event", "target": "OnComponentHit", "properties": {"component_name": "CollisionSphere"}}
```

The `event` op becomes smart enough to handle native events, component events, and (with fix #2) could even detect dispatcher binds.

---

## 5. No Fallback When AI Uses K2_ Prefix That Doesn't Match

**The problem:** The K2_ prefix handling in `TryClassWithK2` goes both directions — adds K2_ if missing, strips K2_ if present. But the AI sometimes uses names like `K2_SetTimerByFunctionName` where the actual UE5 function name is something different (renamed in 5.5, or it's a latent node with special handling). The alias map handles some of these, but it's manually maintained and has bugs (timer alias points to wrong name).

**What will break next:** Any UE 5.5 function rename the alias map doesn't cover. Any function the AI knows by its C++ internal name rather than its Blueprint-exposed name.

**The fix: Fuzzy matching fallback.** After all exact searches fail, do a substring match on function names across searched classes. If the AI says `K2_SetTimerByFunctionName` and the actual name is `SetTimerByFunctionName`, a contains/prefix match finds it.

```cpp
// After all searches fail, before returning nullptr:
// Fuzzy fallback: search for functions containing the core name
FString CoreName = ResolvedName;
CoreName.RemoveFromStart(TEXT("K2_"));

for (UClass* SearchClass : AllSearchedClasses)
{
    for (TFieldIterator<UFunction> FuncIt(SearchClass); FuncIt; ++FuncIt)
    {
        FString FuncName = FuncIt->GetName();
        if (FuncName.Contains(CoreName) || CoreName.Contains(FuncName))
        {
            // Candidate found — log it as a suggestion
            UE_LOG(LogOliveNodeFactory, Log,
                TEXT("FindFunction('%s'): fuzzy match candidate '%s::%s'"),
                *FunctionName, *SearchClass->GetName(), *FuncName);
            
            // If it's a high-confidence match (same length or only K2_ difference), use it
            if (FuncName.Len() == CoreName.Len() || 
                FuncName == TEXT("K2_") + CoreName ||
                TEXT("K2_") + FuncName == CoreName)
            {
                return *FuncIt;
            }
            
            // Otherwise add to suggestions for error message
            Suggestions.Add(FString::Printf(TEXT("%s::%s"), *SearchClass->GetName(), *FuncName));
        }
    }
}
```

---

## Summary: 5 Fixes That Cover Categories

| # | Fix | What It Covers | Effort |
|---|-----|---------------|--------|
| 1 | Universal FindFunction fallback | Every function library (plugins, project, engine) | 30 min |
| 2 | `call` → `call_delegate` auto-reroute | Every event dispatcher the AI calls wrong | 30 min |
| 3 | K2Node property bridge | CallDelegate, ComponentBoundEvent, Timeline, InputAction | 2-3 hours |
| 4 | `event` op handles component events | OnComponentHit, OnBeginOverlap, all component delegates | 1-2 hours |
| 5 | Fuzzy FindFunction fallback + fix timer alias | Every K2_ mismatch, every UE5.5 rename | 1 hour |

**Total: ~1 day of work.**

**What this doesn't cover** (acceptable gaps):
- Timeline creation (complex — needs graph creation, track setup). Better handled through `add_node` with the property bridge (#3).
- Animation Montage nodes (very specialized, low frequency)  
- Enhanced Input binding (already has dedicated handling in `EnhancedInputAction` node type)

**The principle:** Each fix covers a CATEGORY of future failures, not a single function. Fix #1 alone would have prevented the timer failure AND every future function-not-found for library functions. Fix #2 would have prevented the dispatcher failure AND every future dispatcher call. Together they eliminate the two root causes that turned a 3-minute task into 10 minutes.
# Concrete Additions: Alias Map, Event Names, Plan-JSON Ops

These are specific entries to add to existing maps. No new systems, no architecture changes — just filling gaps in what's already there.

---

## 1. Alias Map Fixes (wrong entries)

These are currently pointing to the wrong function name:

```cpp
// WRONG → RIGHT

// Timer functions — K2_SetTimer doesn't exist. The real names:
Map.Add(TEXT("SetTimer"), TEXT("K2_SetTimerByFunctionName"));             // was K2_SetTimer
Map.Add(TEXT("SetTimerByFunctionName"), TEXT("K2_SetTimerByFunctionName")); // was K2_SetTimer
Map.Add(TEXT("SetTimerByName"), TEXT("K2_SetTimerByFunctionName"));        // was K2_SetTimer
Map.Add(TEXT("ClearTimer"), TEXT("K2_ClearAndInvalidateTimerByFunctionName")); // was K2_ClearTimer

// Pass-through for AI using the internal name directly
Map.Add(TEXT("K2_SetTimerByFunctionName"), TEXT("K2_SetTimerByFunctionName"));
```

**NOTE:** Verify these exact names against UE 5.5. Run in editor:
`UKismetSystemLibrary::StaticClass()->FindFunctionByName("K2_SetTimerByFunctionName")`
If null, the function may be a latent K2Node (not a UFunction) and needs special handling in the node catalog instead.

---

## 2. Alias Map Additions (new entries)

### Timers (complete set)
```cpp
Map.Add(TEXT("SetTimerByEvent"), TEXT("K2_SetTimerByEvent"));
Map.Add(TEXT("ClearTimerByFunctionName"), TEXT("K2_ClearAndInvalidateTimerByFunctionName"));
Map.Add(TEXT("ClearTimerByHandle"), TEXT("K2_ClearTimerHandle"));
Map.Add(TEXT("PauseTimer"), TEXT("K2_PauseTimer"));
Map.Add(TEXT("UnpauseTimer"), TEXT("K2_UnPauseTimer"));
Map.Add(TEXT("IsTimerActive"), TEXT("K2_IsTimerActive"));
Map.Add(TEXT("GetTimerElapsedTime"), TEXT("K2_GetTimerElapsedTime"));
Map.Add(TEXT("GetTimerRemainingTime"), TEXT("K2_GetTimerRemainingTime"));
Map.Add(TEXT("TimerExists"), TEXT("K2_TimerExists"));
```

### Damage
```cpp
Map.Add(TEXT("ApplyPointDamage"), TEXT("ApplyPointDamage"));
Map.Add(TEXT("ApplyRadialDamage"), TEXT("ApplyRadialDamage"));
Map.Add(TEXT("ApplyRadialDamageWithFalloff"), TEXT("ApplyRadialDamageWithFalloff"));
```

### Widget / UI
```cpp
Map.Add(TEXT("CreateWidget"), TEXT("Create"));  // UWidgetBlueprintLibrary::Create
Map.Add(TEXT("AddToViewport"), TEXT("AddToViewport"));
Map.Add(TEXT("RemoveFromViewport"), TEXT("RemoveFromViewport"));
Map.Add(TEXT("RemoveFromParent"), TEXT("RemoveFromParent"));
Map.Add(TEXT("SetInputModeGameOnly"), TEXT("SetInputModeGameOnly"));
Map.Add(TEXT("SetInputModeUIOnly"), TEXT("SetInputModeUIOnly"));
Map.Add(TEXT("SetInputModeGameAndUI"), TEXT("SetInputModeGameAndUI"));
Map.Add(TEXT("SetShowMouseCursor"), TEXT("SetShowMouseCursor"));
```

### AI / Navigation
```cpp
Map.Add(TEXT("SimpleMoveToLocation"), TEXT("SimpleMoveToLocation"));
Map.Add(TEXT("SimpleMoveToActor"), TEXT("SimpleMoveToActor"));
Map.Add(TEXT("GetAIController"), TEXT("GetAIController"));
Map.Add(TEXT("SpawnAIFromClass"), TEXT("SpawnAIFromClass"));
Map.Add(TEXT("GetBlackboardValueAsObject"), TEXT("GetBlackboardValueAsObject"));
Map.Add(TEXT("SetBlackboardValueAsObject"), TEXT("SetBlackboardValueAsObject"));
```

### Animation
```cpp
Map.Add(TEXT("PlayMontage"), TEXT("PlayMontage"));
Map.Add(TEXT("PlayAnimMontage"), TEXT("PlayAnimMontage"));
Map.Add(TEXT("StopAnimMontage"), TEXT("StopAnimMontage"));
Map.Add(TEXT("GetAnimInstance"), TEXT("GetAnimInstance"));
Map.Add(TEXT("GetMesh"), TEXT("GetMesh"));
```

### Camera
```cpp
Map.Add(TEXT("GetFollowCamera"), TEXT("GetFollowCamera"));
Map.Add(TEXT("SetViewTargetWithBlend"), TEXT("SetViewTargetWithBlend"));
Map.Add(TEXT("GetPlayerCameraManager"), TEXT("GetPlayerCameraManager"));
```

### Object Lifecycle
```cpp
Map.Add(TEXT("SetLifeSpan"), TEXT("SetLifeSpan"));
Map.Add(TEXT("GetLifeSpan"), TEXT("GetLifeSpan"));
Map.Add(TEXT("SetActorHiddenInGame"), TEXT("SetActorHiddenInGame"));
Map.Add(TEXT("SetActorEnableCollision"), TEXT("SetActorEnableCollision"));
Map.Add(TEXT("SetActorTickEnabled"), TEXT("SetActorTickEnabled"));
Map.Add(TEXT("SetActorTickInterval"), TEXT("SetActorTickInterval"));
```

### Tags
```cpp
Map.Add(TEXT("ActorHasTag"), TEXT("ActorHasTag"));
Map.Add(TEXT("ComponentHasTag"), TEXT("ComponentHasTag"));
```

### Physics / Movement (expand existing)
```cpp
Map.Add(TEXT("AddRadialForce"), TEXT("AddRadialForce"));
Map.Add(TEXT("AddRadialImpulse"), TEXT("AddRadialImpulse"));
Map.Add(TEXT("GetPhysicsLinearVelocity"), TEXT("GetPhysicsLinearVelocity"));
Map.Add(TEXT("SetPhysicsAngularVelocity"), TEXT("SetPhysicsAngularVelocityInDegrees"));
Map.Add(TEXT("WakeRigidBody"), TEXT("WakeRigidBody"));
```

### Overlap / Collision (expand existing)
```cpp
Map.Add(TEXT("GetOverlappingActors"), TEXT("GetOverlappingActors"));
Map.Add(TEXT("GetOverlappingComponents"), TEXT("GetOverlappingComponents"));
Map.Add(TEXT("IsOverlappingActor"), TEXT("IsOverlappingActor"));
Map.Add(TEXT("SetCollisionResponseToChannel"), TEXT("SetCollisionResponseToChannel"));
Map.Add(TEXT("SetCollisionResponseToAllChannels"), TEXT("SetCollisionResponseToAllChannels"));
Map.Add(TEXT("SetGenerateOverlapEvents"), TEXT("SetGenerateOverlapEvents"));
Map.Add(TEXT("IgnoreActorWhenMoving"), TEXT("IgnoreActorWhenMoving"));
```

### Sound
```cpp
Map.Add(TEXT("PlaySound2D"), TEXT("PlaySound2D"));
Map.Add(TEXT("SpawnSound2D"), TEXT("SpawnSound2D"));
Map.Add(TEXT("SpawnSoundAtLocation"), TEXT("SpawnSoundAtLocation"));
```

### Math (expand existing)
```cpp
Map.Add(TEXT("MapRange"), TEXT("MapRangeClamped"));
Map.Add(TEXT("MapRangeClamped"), TEXT("MapRangeClamped"));
Map.Add(TEXT("MapRangeUnclamped"), TEXT("MapRangeUnclamped"));
Map.Add(TEXT("FInterpTo"), TEXT("FInterpTo"));
Map.Add(TEXT("RInterpTo"), TEXT("RInterpTo"));
Map.Add(TEXT("VInterpTo"), TEXT("VInterpTo"));
Map.Add(TEXT("NearlyEqual"), TEXT("NearlyEqual_FloatFloat"));
Map.Add(TEXT("Sign"), TEXT("SignOfFloat"));
Map.Add(TEXT("DegreesToRadians"), TEXT("DegreesToRadians"));
Map.Add(TEXT("RadiansToDegrees"), TEXT("RadiansToDegrees"));
Map.Add(TEXT("FindLookAtRotation"), TEXT("FindLookAtRotation"));
Map.Add(TEXT("GetDirectionUnitVector"), TEXT("GetDirectionUnitVector"));
```

### Conversion
```cpp
Map.Add(TEXT("IntToFloat"), TEXT("Conv_IntToDouble"));
Map.Add(TEXT("Conv_IntToFloat"), TEXT("Conv_IntToDouble"));
Map.Add(TEXT("NameToString"), TEXT("Conv_NameToString"));
Map.Add(TEXT("StringToName"), TEXT("Conv_StringToName"));
Map.Add(TEXT("TextToString"), TEXT("Conv_TextToString"));
Map.Add(TEXT("StringToText"), TEXT("Conv_StringToText"));
Map.Add(TEXT("ObjectToString"), TEXT("Conv_ObjectToString"));
Map.Add(TEXT("VectorToString"), TEXT("Conv_VectorToString"));
Map.Add(TEXT("RotatorToString"), TEXT("Conv_RotatorToString"));
```

### Save/Load
```cpp
Map.Add(TEXT("SaveGameToSlot"), TEXT("SaveGameToSlot"));
Map.Add(TEXT("LoadGameFromSlot"), TEXT("LoadGameFromSlot"));
Map.Add(TEXT("DoesSaveGameExist"), TEXT("DoesSaveGameExist"));
Map.Add(TEXT("CreateSaveGameObject"), TEXT("CreateSaveGameObject"));
Map.Add(TEXT("DeleteGameInSlot"), TEXT("DeleteGameInSlot"));
```

---

## 3. Event Name Map Additions

These go in the `EventNameMap` in `ResolveEventOp`:

```cpp
// Display name aliases (how they appear in editor palette)
{ TEXT("EventTick"),                    TEXT("ReceiveTick") },
{ TEXT("EventBeginPlay"),               TEXT("ReceiveBeginPlay") },
{ TEXT("EventEndPlay"),                 TEXT("ReceiveEndPlay") },
{ TEXT("EventAnyDamage"),               TEXT("ReceiveAnyDamage") },
{ TEXT("EventHit"),                     TEXT("ReceiveHit") },
{ TEXT("EventActorBeginOverlap"),       TEXT("ReceiveActorBeginOverlap") },
{ TEXT("EventActorEndOverlap"),         TEXT("ReceiveActorEndOverlap") },

// Space-separated variants (common AI pattern)
{ TEXT("Event BeginPlay"),              TEXT("ReceiveBeginPlay") },
{ TEXT("Event Tick"),                   TEXT("ReceiveTick") },
{ TEXT("Event End Play"),               TEXT("ReceiveEndPlay") },

// Pass-through for internal names (AI sometimes uses Receive prefix directly)
{ TEXT("ReceiveBeginPlay"),             TEXT("ReceiveBeginPlay") },
{ TEXT("ReceiveTick"),                  TEXT("ReceiveTick") },
{ TEXT("ReceiveEndPlay"),               TEXT("ReceiveEndPlay") },
{ TEXT("ReceiveActorBeginOverlap"),     TEXT("ReceiveActorBeginOverlap") },
{ TEXT("ReceiveActorEndOverlap"),       TEXT("ReceiveActorEndOverlap") },
{ TEXT("ReceiveAnyDamage"),             TEXT("ReceiveAnyDamage") },
{ TEXT("ReceiveHit"),                   TEXT("ReceiveHit") },
{ TEXT("ReceivePointDamage"),           TEXT("ReceivePointDamage") },
{ TEXT("ReceiveRadialDamage"),          TEXT("ReceiveRadialDamage") },
{ TEXT("ReceiveDestroyed"),             TEXT("ReceiveDestroyed") },

// Pawn/Character specific events
{ TEXT("Possessed"),                    TEXT("ReceivePossessed") },
{ TEXT("UnPossessed"),                  TEXT("ReceiveUnPossessed") },
{ TEXT("ControllerChanged"),            TEXT("ReceiveControllerChanged") },
{ TEXT("Landed"),                       TEXT("OnLanded") },
{ TEXT("OnLanded"),                     TEXT("OnLanded") },
{ TEXT("OnJumped"),                     TEXT("OnJumped") },
{ TEXT("MovementModeChanged"),          TEXT("OnMovementModeChanged") },
{ TEXT("OnMovementModeChanged"),        TEXT("OnMovementModeChanged") },
```

---

## 4. Plan-JSON Ops — Nothing New Needed

Current ops already cover everything:

| What | Op | Status |
|------|----|--------|
| Function calls | `call` | ✅ exists |
| Event dispatchers | `call_delegate` | ✅ exists (AI just used wrong op) |
| Component events | `event` | ✅ exists (passes through to NodeFactory) |
| Enhanced Input | `event` with IA_ prefix | ✅ exists |
| All flow control | `branch`, `sequence`, loops, etc | ✅ exists |
| Structs | `make_struct`, `break_struct` | ✅ exists |

**No new ops needed.** The gap was the AI using `call` instead of `call_delegate`, fixed by auto-reroute in ResolveCallOp.

---

## 5. Verify At Runtime

All alias entries assume UE 5.5 function names. Before adding each batch, verify:

```cpp
UClass* Class = UKismetSystemLibrary::StaticClass();
UFunction* Func = Class->FindFunctionByName("K2_SetTimerByFunctionName");
UE_LOG(LogTemp, Log, TEXT("Found: %s"), Func ? TEXT("YES") : TEXT("NO"));
```

Do this for any K2_ prefixed name — UE5 is inconsistent about which functions use the prefix.

---

## Summary

| Category | Entries to Add |
|----------|---------------|
| Alias fixes (wrong targets) | 5 |
| Alias additions (new functions) | ~80 |
| Event name map additions | ~25 |
| Plan-JSON ops | 0 (all covered) |
