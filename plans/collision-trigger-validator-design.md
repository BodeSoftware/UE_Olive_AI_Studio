# Check 6: COLLISION_ON_TRIGGER_COMPONENT Validator Design

**Severity:** Warning (non-blocking)
**Error code:** `COLLISION_ON_TRIGGER_COMPONENT`

## Problem

The AI repeatedly calls `SetCollisionEnabled` (and similar) on a SphereComponent that serves as an overlap trigger, when it should be targeting the StaticMeshComponent (the physical mesh). This happens in weapon pickup / equip flows. The check must NOT fire for legitimate uses (e.g., projectile SphereComponent that genuinely needs collision toggled).

## Heuristic (4 conditions, ALL must be true)

1. **Collision function detected:** A `call` step targets `SetCollisionEnabled`, `SetCollisionResponseToAllChannels`, or `SetCollisionProfileName`.

2. **Target is a trigger-shaped component:** The Target input resolves (via `@ref` trace-back or literal name) to an SCS component whose class is `USphereComponent` or `UCapsuleComponent`.

3. **Blueprint also has a mesh component:** The SCS contains at least one `UStaticMeshComponent` or `USkeletalMeshComponent`.

4. **Pickup/equip context signal:** At least ONE of the following is true anywhere in the plan:
   - Another step calls `AttachToComponent` or `AttachActorToComponent`
   - Another step calls `SetCollisionEnabled` / `SetCollisionResponseToAllChannels` / `SetCollisionProfileName` targeting a DIFFERENT component (double-collision-change pattern)
   - A `set_var` step targets a variable whose name contains `equip`, `pickup`, `collect`, `grab`, or `held` (case-insensitive)
   - An `event` step targets `OnComponentBeginOverlap` or `OnActorBeginOverlap`
   - The plan contains a `call` to `DestroyActor` or `SetActorHiddenInGame` (common pickup cleanup)

If all 4 conditions are met, emit a warning. If condition 4 fails (no pickup context signals), do NOT emit -- this avoids false positives on projectiles, physics objects, etc.

## Warning Message Format

```
Step '{StepId}': SetCollisionEnabled targets '{ComponentName}' ({ComponentClass}),
which is typically an overlap trigger. Did you mean to target '{MeshComponentName}'
({MeshComponentClass})? In pickup/equip patterns, the mesh component usually needs
collision changes, not the trigger volume.
```

## Implementation Spec

### File Changes

**`OlivePlanValidator.h`** -- add declaration:

```cpp
/**
 * Check 6: Collision-on-trigger heuristic.
 * Warns when SetCollisionEnabled (or similar) targets a sphere/capsule trigger
 * component while the Blueprint also has a mesh component and the plan context
 * suggests a pickup/equip/attach pattern.
 *
 * Severity: Warning (non-blocking). The AI can proceed but gets a nudge
 * toward the correct component.
 */
static void CheckCollisionOnTriggerComponent(
    const FOlivePlanValidationContext& Context,
    FOlivePlanValidationResult& Result);
```

**`OlivePlanValidator.cpp`** -- add includes:

```cpp
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
```

Add call in `Validate()`:

```cpp
CheckCollisionOnTriggerComponent(Context, Result);
```

### Algorithm (pseudocode)

```
CheckCollisionOnTriggerComponent(Context, Result):
    // --- Gate: need SCS ---
    SCS = Context.Blueprint->SimpleConstructionScript
    if !SCS: return

    // --- Collect SCS component info (single pass) ---
    TriggerComponents: TMap<FString, TPair<FString, UClass*>>   // varName -> (varName, class)
    MeshComponents:    TArray<TPair<FString, UClass*>>           // (varName, class)

    for each SCSNode in SCS->GetAllNodes():
        if !SCSNode || !SCSNode->ComponentClass: continue
        VarName = SCSNode->GetVariableName().ToString()
        Class = SCSNode->ComponentClass

        if Class->IsChildOf(USphereComponent) || Class->IsChildOf(UCapsuleComponent):
            TriggerComponents.Add(VarName, {VarName, Class})

        if Class->IsChildOf(UStaticMeshComponent) || Class->IsChildOf(USkeletalMeshComponent):
            MeshComponents.Add({VarName, Class})

    // --- Early exit: need BOTH trigger and mesh components ---
    if TriggerComponents.IsEmpty() || MeshComponents.IsEmpty(): return

    // --- Build pickup context signals from the plan (single pass over steps) ---
    bHasAttachCall = false
    bHasOverlapEvent = false
    bHasEquipVariable = false
    bHasDestroyOrHide = false
    CollisionStepCount = 0           // how many steps call collision functions
    CollisionTargetComponents: TSet<FString>  // which components are collision targets

    CollisionFunctions = {"SetCollisionEnabled", "SetCollisionResponseToAllChannels", "SetCollisionProfileName"}
    AttachFunctions = {"AttachToComponent", "AttachActorToComponent", "K2_AttachToComponent",
                       "AttachToActor", "K2_AttachToActor", "AttachComponentToComponent"}
    PickupVarKeywords = {"equip", "pickup", "collect", "grab", "held"}
    CleanupFunctions = {"DestroyActor", "K2_DestroyActor", "SetActorHiddenInGame"}

    for each (PlanStep, ResolvedStep) in zip(Context.Plan.Steps, Context.ResolvedSteps):
        FuncName = ResolvedStep.Properties.Find("function_name")
                   ?? PlanStep.Target  // fallback to raw target

        if PlanStep.Op == "call" && FuncName:
            if FuncName in CollisionFunctions:
                CollisionStepCount++
                // Resolve which component this targets
                TargetComp = ResolveTargetComponentName(PlanStep, Context)
                if !TargetComp.IsEmpty():
                    CollisionTargetComponents.Add(TargetComp)

            if FuncName in AttachFunctions:
                bHasAttachCall = true

            if FuncName in CleanupFunctions:
                bHasDestroyOrHide = true

        if PlanStep.Op == "event":
            if PlanStep.Target contains "Overlap" (case-insensitive):
                bHasOverlapEvent = true

        if PlanStep.Op == "set_var":
            LowerTarget = PlanStep.Target.ToLower()
            if any keyword in PickupVarKeywords is substring of LowerTarget:
                bHasEquipVariable = true

    // --- Check condition 4: pickup/equip context ---
    bHasPickupContext = bHasAttachCall || bHasOverlapEvent || bHasEquipVariable
                        || bHasDestroyOrHide
                        || (CollisionStepCount >= 2 && CollisionTargetComponents.Num() >= 2)

    if !bHasPickupContext: return

    // --- Find collision steps targeting trigger components and emit warnings ---
    for each (PlanStep, ResolvedStep) in zip(Context.Plan.Steps, Context.ResolvedSteps):
        if PlanStep.Op != "call": continue
        FuncName = ResolvedStep.Properties.Find("function_name") ?? PlanStep.Target
        if FuncName not in CollisionFunctions: continue

        TargetComp = ResolveTargetComponentName(PlanStep, Context)
        if TargetComp.IsEmpty(): continue
        if TargetComp not in TriggerComponents: continue

        // This collision call targets a trigger component in a pickup context
        TriggerInfo = TriggerComponents[TargetComp]
        MeshInfo = MeshComponents[0]  // suggest the first mesh component

        Result.Warnings.Add(formatted message with TriggerInfo and MeshInfo)
        UE_LOG(...)
```

### ResolveTargetComponentName helper

Anonymous namespace helper in the .cpp file. Traces the Target input back to a component variable name:

```
ResolveTargetComponentName(PlanStep, Context) -> FString:
    TargetValue = PlanStep.Inputs.Find("Target")
    if !TargetValue || TargetValue->IsEmpty(): return ""

    // Case 1: @ref syntax (e.g., "@get_sphere.auto")
    if TargetValue starts with "@":
        // Extract step ID: strip "@", split on "."
        RefStepId = substring between "@" and first "."
        // Find the referenced step
        RefIndex = Context.StepIdToIndex.Find(RefStepId)
        if !RefIndex: return ""
        RefStep = Context.Plan.Steps[*RefIndex]
        // If the referenced step is a get_var, its target IS the component name
        if RefStep.Op == "get_var":
            return RefStep.Target
        return ""

    // Case 2: Literal component variable name
    return *TargetValue
```

This is intentionally simple. It handles the two common patterns:
- `"Target": "@get_sphere.auto"` where `get_sphere` is `{"op": "get_var", "target": "SphereComp"}`
- `"Target": "SphereComp"` (literal name, handled by Phase 1.5 auto-wire)

It does NOT trace through multi-hop references. That is acceptable -- the heuristic is a nudge, not a guarantee.

## Edge Cases

| Scenario | Fires? | Why |
|----------|--------|-----|
| Projectile with SphereComponent, no mesh, calls SetCollisionEnabled | No | Condition 3 fails (no mesh component) |
| Projectile with SphereComponent + StaticMesh, calls SetCollisionEnabled | No | Condition 4 fails (no pickup signals) |
| Pickup actor, SetCollisionEnabled on SphereComp, has OnOverlap event | Yes | All 4 conditions met |
| Pickup actor, SetCollisionEnabled on StaticMeshComponent | No | Condition 2 fails (mesh is not trigger-shaped) |
| Actor with SphereComp + Mesh, SetCollisionEnabled on Sphere, but no overlap/attach/equip context | No | Condition 4 fails |
| Widget Blueprint (no SCS) | No | Early exit: no SCS |
| Component Blueprint | No | Early exit: no SCS (component BPs have no SCS) |

## Implementation Order

1. Add the private method declaration in `OlivePlanValidator.h` (1 line)
2. Add the 4 includes in `OlivePlanValidator.cpp`
3. Add the `ResolveTargetComponentName` anonymous namespace helper (~20 lines)
4. Add `CheckCollisionOnTriggerComponent` (~120 lines)
5. Add the call in `Validate()` (1 line)

Total: ~145 lines of new code. No new files. No changes to other modules.

## Not In Scope

- Blocking the operation (user explicitly requested warning-only)
- Auto-fixing the target (too complex, the warning message names the correct component)
- Checking inherited components from parent Blueprints (SCS covers the 99% case for user-created pickup actors)
