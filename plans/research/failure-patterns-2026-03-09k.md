# Research: Four Failure Patterns — Session Log 2026-03-09 (Run 09k analysis)

## Question
Investigate four specific failure patterns observed in a test run log:
1. GetForwardVector resolver/executor mismatch
2. BuildFunctionPinReference and SetFloatPropertyByName
3. Reference-before-declaration (TrailParticle)
4. break_struct pin name format

---

## Investigation 1: GetForwardVector Resolver/Executor Mismatch

### What the AI wrote

The plan_json step in question (step_id `get_fwd`, step 8) was:
```json
{"step_id": "get_fwd", "op": "call", "target": "GetForwardVector", "inputs": {"InRot": "@get_rot.auto"}}
```

The intent is `KismetMathLibrary::GetForwardVector(InRot: FRotator) -> FVector`, which takes a Rotator and returns the forward vector for that rotation. This is distinct from `Actor::GetActorForwardVector()` which takes no input — it returns the actor's own forward vector.

### What the resolver did (correct)

The resolver ran its alias-fallback logic correctly:

```
[07.05.58:203][42] LogOlivePlanResolver: Resolving step 8: step_id='get_fwd', op='call', target='GetForwardVector'
[07.05.58:203][42] LogOliveNodeFactory: FindFunction('GetForwardVector'): alias resolved -> 'GetActorForwardVector'
[07.05.58:203][42] LogOlivePlanResolver: ResolveCallOp: 'GetForwardVector' -> function_name='GetActorForwardVector', target_class='Actor'
[07.05.58:203][42] LogOlivePlanResolver: ResolveCallOp: alias-resolved 'GetForwardVector' -> 'Actor::GetActorForwardVector' but step has unmatched input pins: [InRot]. Attempting fallback search for original name 'GetForwardVector'.
[07.05.58:203][42] LogOlivePlanResolver: ResolveCallOp: Alias fallback succeeded -- 'GetForwardVector' rerouted from 'Actor::GetActorForwardVector' to 'KismetMathLibrary::GetForwardVector' (input pins [InRot] matched)
```

The resolver correctly:
1. Hit the alias map: `GetForwardVector` -> `GetActorForwardVector`
2. Detected that the step has `InRot` as an input pin that doesn't match `GetActorForwardVector`'s signature
3. Fell back to searching for the original name `GetForwardVector` without alias
4. Found `KismetMathLibrary::GetForwardVector` and confirmed `InRot` matched
5. Stored `function_name='GetForwardVector'`, `target_class='KismetMathLibrary'` in the resolved step

Resolver output was correct. The resolved step carried `KismetMathLibrary::GetForwardVector`.

### What the executor did (wrong)

```
[07.05.58:204][42] LogOlivePlanExecutor: Step 9/17: step_id='get_fwd', type='CallFunction', target='GetForwardVector'
[07.05.58:204][42] LogOliveNodeFactory: FindFunction('GetForwardVector'): alias resolved -> 'GetActorForwardVector'
[07.05.58:204][42] LogOliveNodeFactory: FindFunction('GetForwardVector'): alias resolved -> 'GetActorForwardVector'
[07.05.58:204][42] LogOliveNodeFactory: Created node of type 'CallFunction' at (2400, 0)
```

The executor called `FindFunction` **twice** on `'GetForwardVector'` — and both times the alias map fired and returned `GetActorForwardVector`. Neither call reached the library search. A node was created for `GetActorForwardVector`, not for `KismetMathLibrary::GetForwardVector`.

Then during Phase 4 data wiring:
```
[07.05.58:206][42] LogOlivePinManifest: Warning: FindPinSmart failed for hint 'InRot' (direction=input). Available pins:
[07.05.58:206][42] LogOlivePinManifest: Warning:   - 'self' (display: 'Target', type: Actor Object Reference)
[07.05.58:206][42] LogOlivePlanExecutor: Warning: Data wire FAILED: No input pin matching 'InRot' on step 'get_fwd' (type: CallFunction). Available: self (Actor Object Reference)
```

The node has only a `self` (Target) pin — that's `GetActorForwardVector`'s signature. The `InRot` pin doesn't exist on it. Wire fails, plan rolls back.

### Root Cause

**This is a code bug.** The resolver and executor use separate `FindFunction` calls that do not share state. The resolver's alias-fallback logic correctly determined the right function (`KismetMathLibrary::GetForwardVector`) and stored `function_name='GetForwardVector'` with `target_class='KismetMathLibrary'`. However, the executor does not pass `target_class` into `FindFunction`. It calls `FindFunction('GetForwardVector', ...)` bare — which immediately hits the alias map and returns `GetActorForwardVector`, never reaching the library search where the correct function lives.

**The disconnect**: The resolver stores a `ResolvedFunctionName` that can differ from what `FindFunction` would return by default. The executor must use both `ResolvedFunctionName` AND `ResolvedTargetClass` when creating the node. If `target_class` is 'KismetMathLibrary', the executor should call `FindFunction(name, target_class='KismetMathLibrary')` not `FindFunction(name, '')`.

This same failure occurred on two successive apply_plan_json calls at 07:05:58 and 07:06:15. The resolver fixed itself correctly both times; the executor failed both times identically.

**Verified identical failure on retry**: The second attempt (07:06:15) also has the orphaned exec pin bug (fire_evt.then -> check_aim.execute: TypesIncompatible due to bOrphanedPin), compounding the failure. But the GetForwardVector node mismatch is independent of that.

### Classification: CODE BUG in executor node creation. Resolver passes target_class correctly; executor ignores it.

---

## Investigation 2: BuildFunctionPinReference and SetFloatPropertyByName

### Pipeline timing

- `06:56:57:374` — Planner (MCP) succeeded, 11621 char plan, 3 assets
- `06:56:57:377` — CLI pipeline complete
- `06:56:57:378` onward — `BuildFunctionPinReference` fires (the `FindFunction` calls starting at line 1896)
- `06:56:57:835` — Stdin delivered to Builder: 22929 chars

The batch of `FindFunction` warning calls at lines 1899–1956 (timestamps 06:56:57:381 through 06:56:57:496) is exactly `BuildFunctionPinReference` running after the pipeline completes and before the Builder launches.

### Did BuildFunctionPinReference extract SetFloatPropertyByName?

The `BuildFunctionPinReference` extraction patterns are:
1. `\bcall\s+([A-Z][A-Za-z0-9_]+)` — matches "call SetFloatPropertyByName"
2. `\b([A-Z][A-Za-z0-9_]+)\s*\(` — matches "SetFloatPropertyByName("
3. `(?:->|via|using)\s+([A-Z][A-Za-z0-9_]+)` — matches "-> SetFloatPropertyByName" etc.

**SetFloatPropertyByName does NOT appear in the BuildFunctionPinReference batch.** Looking at lines 1896–1956, which is the exhaustive FindFunction batch from that function, `SetFloatPropertyByName` is not among the names attempted. The names tried include: `AttachToComponent`, `IsValid`, `DestroyActor`, `ProjectileMovement`, `CreateArrow`, `SetMaxWalkSpeed`, `SpawnedArrow`, `EquipBow`, `IA_Aim`, `GetLaunchOrigin`, `BowRef`, `InitBow`, `ArrowDamage`, `InitArrow`, `NockArrow`, `Self`, `CharacterMovement`, `GetForwardVector`, `SetProjectileMovementEnabled`, `SetVelocityInLocalSpace`, `StopMovementImmediately`, `UnequipBow`, `DetachFromActor`, `ApplyHitDamage`, `OwnerCharacter`, `ShooterRef`, `OnArrowHit`, `StickToSurface`, `HitInfo`, `GetWorldTransform`, `FunctionResult`, `ArrowSpeed`, `OnBowFired`, `StringOrigin`, `ArrowMesh`, `CollisionSphere`, `BP_Bow`, `GravityScale`, `BP_ThirdPersonCharacter`, `TrailParticle`, `SpawnActor`, `ArrowSpeedMultiplier`, `BP_ArrowProjectile`, `ArrowGravity`, `StartAim`, `ArrowAttachPoint`, `BowMesh`, `AimSetup`, `StopAim`, `FireArrow`, `SetArrowVelocity`, `LaunchConditions`, `CanFireArrow`, `ArrowDamageBonus`, `IA_Fire`, `IA_EquipBow`, `Character`, `GetActorLocation`, `GetComponentByClass`, `Boolean`.

`SetFloatPropertyByName` is absent from this list. Two possibilities:

**Possibility A: The Planner's Build Plan did not mention SetFloatPropertyByName in extractable form.** The regex requires a capital letter start and either "call X", "X(", or "-> X". If the Planner wrote it in lowercase context or buried it in a description without the PascalCase patterns, the regex would miss it.

**Possibility B: "Set" was in the exclusion list.** The `ExcludeNames` set contains `TEXT("Set")` — but that is the bare word "Set", not "SetFloatPropertyByName". The filter removes exact single-word matches, not prefixes. "SetFloatPropertyByName" would NOT be excluded by the "Set" entry.

**Most likely: The Planner described the approach without naming the exact function.** The Planner was working from template data and the Build Plan likely described "set ProjectileMovement MaxWalkSpeed" or similar natural language, not the specific `SetFloatPropertyByName` API call. The Builder then hallucinated this function name when trying to implement "set a float property by name on the component."

**Confirmed absent from pipeline output**: `SetFloatPropertyByName` first appears at `06:59:16:518` — which is the Builder's first `apply_plan_json` call's resolver log. By that time, `BuildFunctionPinReference` ran at 06:56:57 and the Builder has been running for ~2.5 minutes. The function name originated entirely from the Builder's own inference.

**BuildFunctionPinReference would have flagged it if it was in the Build Plan.** `FindFunction` performs universal library search. At line 2441, when the resolver encounters `SetFloatPropertyByName` at 06:59:16, the same universal search runs and it also fails, confirming `SetFloatPropertyByName` does not exist in UE 5.5's UBlueprintFunctionLibrary hierarchy. (In UE 4 / early UE 5, `SetFloatPropertyByName` existed on `KismetSystemLibrary`. It was removed — replaced by reflection-based approaches. The closest surviving function is `SetFloatPropertyByName` on `KismetSystemLibrary`... actually the log shows the closest match is `SetFieldPathPropertyByName`, confirming removal.)

### Classification: PROMPT/KNOWLEDGE ISSUE. BuildFunctionPinReference did not see "SetFloatPropertyByName" in the Build Plan. The Builder hallucinated a removed UE4 function. The fix is: add `SetFloatPropertyByName` -> `SetFloatPropertyByName` (does not exist) to the alias map with a redirect to the actual pattern (direct property access via node factory), and/or add knowledge that this function was removed in UE5 and direct property assignment via set_var is the replacement.

---

## Investigation 3: Reference-before-declaration (TrailParticle)

### Exact timestamps

**When BuildFunctionPinReference probed TrailParticle** (during pipeline post-processing, before Builder launches):
```
[06.56.57:457][704] LogOliveNodeFactory: Warning: FindFunction('TrailParticle' [resolved='TrailParticle'], class=''): FAILED
```
This is at 06:56:57. This is `BuildFunctionPinReference` extracting "TrailParticle" from the Build Plan — because it appeared with a PascalCase pattern `TrailParticle(` or `call TrailParticle` in the plan text. Note: this is a probe-only call; it just tests existence, it does not affect execution.

**Builder add_variable calls**: The Builder launched at 06:56:57:545 and created variables. Looking at all `add_variable` calls for `BP_ArrowProjectile`:
- 06:57:38 — OwnerCharacter (on BP_Bow)
- 06:57:39 — ArrowDamageBonus (on BP_Bow)
- 06:57:40 — (various BP_Bow variables)
- 06:57:41 — GravityScale (on BP_ArrowProjectile)
- 06:57:42 — GravityScale (confirmed added, log line 2107: "Added variable 'GravityScale'")
- 06:57:43 — ShooterRef
- 06:57:43 — bHasHit (FAILED — WriteRateLimit)
- 06:57:48 — bHasHit retry (FAILED — WriteRateLimit)
- 06:58:24 — bHasHit (SUCCESS after cooldown)
- 06:58:25 — HitInfo

**TrailParticle add_variable**: There is NO `add_variable` call for `TrailParticle` in the log at any timestamp. The Builder never called `add_variable` with `name=TrailParticle`. TrailParticle was present in the Build Plan (BuildFunctionPinReference extracted it at 06:56:57), but the Builder skipped creating it as a variable.

**First plan_json reference to TrailParticle**:
```
[06.59.16:539][122] LogOlivePlanResolver: Resolving step 11: step_id='get_trail', op='get_var', target='TrailParticle'
[06.59.16:539][122] LogOlivePlanResolver: Warning: Step 'get_trail': Variable 'TrailParticle' not found on Blueprint 'BP_ArrowProjectile'
```
At 06:59:16, the plan_json references `get_var TrailParticle`, but the variable was never declared. By 06:59:58 the second attempt also fails (same error).

### Sequence Summary

| Time | Event |
|------|-------|
| 06:56:57 | BuildFunctionPinReference extracts "TrailParticle" from Build Plan, FindFunction probe fails (expected) |
| 06:56:57 | Builder launches |
| 06:57:38–06:58:37 | Builder adds variables + functions to BP_ArrowProjectile — but no TrailParticle |
| 06:59:16 | First apply_plan_json for InitArrow: get_var TrailParticle fails (variable not found) |
| 06:59:58 | Second apply_plan_json for InitArrow: same failure |

### Root Cause

This is a **Builder omission / prompt knowledge issue**, not a code bug. TrailParticle was in the Planner's Build Plan (evidenced by BuildFunctionPinReference extracting it), but the Builder never issued an `add_variable` call for it before attempting the plan_json that uses it. The Builder planned to use a Niagara/particle component reference but either:

1. Skipped the variable declaration step due to rate-limiting confusion (bHasHit was rate-limited twice at 06:57:44 and 06:57:48, which may have disrupted the Builder's sequencing)
2. Assumed TrailParticle was a component (like a NiagaraComponent in SCS) rather than a variable — but the SCS only had ArrowMesh, CollisionSphere, ProjectileMovement added (lines 2027–2056), no Niagara component

The resolver correctly flagged `Variable 'TrailParticle' not found` with a warning — but the plan still executed (partially), creating a GetVariable node that would reference nothing. The write pipeline rolled back due to data wire failures (not specifically TrailParticle but combined).

**The rate limiting at 06:57:44 is notable**: bHasHit failed twice (WriteRateLimit), then required a ~40-second cooldown. This is the same interval during which the Builder would have been adding the remaining variables. It's plausible the Builder lost its sequencing context during the retry delay and skipped the TrailParticle add_variable in favor of moving forward.

### Classification: BUILDER OMISSION — prompt/knowledge issue. No code bug. The resolver's "Variable not found" warning is correct behavior. Possible contributing factor: rate limit disruption during variable creation sequence. The plan_json validator (Phase 0) does not check whether referenced variables exist — this would be a useful addition.

---

## Investigation 4: break_struct Pin Name Format

### The plan_json

The first attempt (07:00:56) used:
```json
{"step_id": "break_hit", "op": "break_struct", "target": "HitResult"}
```
With downstream wiring referencing `@break_hit.~HitActor`, `@break_hit.~ImpactNormal`, `@break_hit.~HitActor`, `@break_hit.~ImpactPoint`.

### What the executor logged

```
[07.00.56:532][422] LogOlivePlanExecutor: Step 5/11: step_id='break_hit', type='BreakStruct', target='(none)'
[07.00.56:532][422] LogOliveNodeFactory: Created node of type 'BreakStruct' at (1200, 0)
[07.00.56:532][422] LogOlivePlanExecutor: -> Created step 'break_hit' -> node 'node_4': 1 pins (pure)
```

The BreakStruct node was created with **1 pin** (pure). A properly initialized `UK2Node_BreakStruct` for FHitResult should expose all the struct's output pins (20+ pins on FHitResult). Getting only 1 pin means the struct type was never set on the node before `AllocateDefaultPins()`.

Then during data wiring:
```
[07.01.06:867][453] LogOlivePlanExecutor: Warning: Data wire FAILED: No output pin matching '~HitActor' on source step 'break_hit' (type: BreakStruct). Available: (none)
[07.01.06:868][453] LogOlivePlanExecutor: Warning: Data wire FAILED: No output pin matching '~ImpactNormal' on source step 'break_hit'
[07.01.06:868][453] LogOlivePlanExecutor: Warning: Data wire FAILED: No output pin matching '~HitActor' on source step 'break_hit'
[07.01.06:868][453] LogOlivePlanExecutor: Warning: Data wire FAILED: No output pin matching '~ImpactPoint' on source step 'break_hit'
```

Available pins: `(none)` — meaning even the single existing pin wasn't named. This is the BreakStruct's exec input pin only (no output data pins allocated because struct type unknown).

### Second attempt (07:01:28)

The Builder switched to `op: "call"` with `target: "BreakHitResult"` — calling it as a function instead. This succeeded at the resolver level (BreakHitResult is a UFunction on KismetSystemLibrary). The data wiring for the second plan_json attempt has no BreakStruct failures.

### Root Cause: TWO issues

**Issue A: break_struct node not being initialized with struct type.**
The `BreakStruct` node created has only 1 pin ("pure" marker) — the struct type from `target='HitResult'` was not applied. This is a code bug: `CreateNodeByClass` for `break_struct` must resolve the UScriptStruct from the target name and call `UK2Node_BreakStruct::SetStructType(Struct)` before `AllocateDefaultPins()`. Without it, the node has no output pins. The log says `target='(none)'` in the executor, which suggests the executor did not pass `target` to the factory at all.

**Issue B: Pin name format with tilde prefix.**
The AI used `~HitActor`, `~ImpactNormal`, `~ImpactPoint`. The tilde prefix is Olive's convention for "output pin from a break_struct step" (used in step references like `@break_hit.~HitActor`). But since the node was never initialized, there were no pins to match regardless of name format. The actual FHitResult break output pin names (from UE source) are: `Actor`, `ImpactNormal`, `ImpactPoint`, `Normal`, `Location`, `Component`, etc. — not prefixed with `~`. The tilde would need to be stripped at the plan executor's pin resolution layer.

The second attempt avoided both issues by using `call BreakHitResult` instead of `break_struct`, which works because `KismetSystemLibrary::BreakHitResult` is a real UFUNCTION with named parameters matching the field names.

### Classification

- **Issue A (no struct type set on node)**: CODE BUG. The `break_struct` op handler in OliveNodeFactory/OlivePlanExecutor does not set the struct type before allocating pins.
- **Issue B (tilde prefix stripping)**: PROMPT/KNOWLEDGE ISSUE if tilde is meant to be an internal reference convention only. The executor should strip `~` before `FindPinSmart`, but if that stripping already happens and the node simply has no pins (bug A), the tilde is irrelevant.

---

## Summary Table

| Investigation | Failure Type | Code Bug? | Prompt Issue? | Severity |
|---------------|-------------|-----------|---------------|----------|
| 1. GetForwardVector | Executor ignores resolved target_class from resolver | YES — executor calls FindFunction without passing target_class from resolved step | No | HIGH — causes persistent retry loop |
| 2. SetFloatPropertyByName | Hallucinated removed UE4 function | No — BuildFunctionPinReference did not see it in Build Plan | YES — Builder invented function name, not in Build Plan | MEDIUM — predictable from prior logs |
| 3. TrailParticle | Variable declared in plan but never created | Possibly — Phase 0 validator doesn't check variable existence | YES — Builder skipped add_variable | MEDIUM — rate limit disruption likely contributing |
| 4. break_struct | Node not initialized with struct type | YES — struct type not set before AllocateDefaultPins | Partial — tilde prefix convention ambiguous | HIGH — makes break_struct op unusable |

---

## Recommendations

### Fix 1 (GetForwardVector — HIGH priority)
The `ResolveCallOp` stores `ResolvedTargetClass` on `FOliveResolvedStep`. The executor node creation path must pass this class to `FindFunction` (or better: use `CreateNodeByClass` with a `target_class` hint that bypasses alias map lookups when a specific library class is already known). The alias-fallback feature in the resolver is working correctly; the executor just needs to honor the resolver's decision.

Specifically: in `OlivePlanExecutor.cpp`, when creating a `CallFunction` node, if `ResolvedStep.ResolvedTargetClass` is set AND differs from the alias-map-resolved class, use it directly as `target_class` in `FindFunction`. This prevents re-triggering the alias map.

### Fix 2 (SetFloatPropertyByName — MEDIUM priority)
Add to the alias map: `SetFloatPropertyByName` -> note that it does not exist in UE5. Options:
- Add an alias entry mapping it to `SetFloatPropertyByName_REMOVED` that returns a descriptive error at resolver time (before building nodes)
- Add it to the self-correction policy's known-hallucination list so the error message explicitly says "this function was removed in UE5; use direct property access or Set[Type]PropertyByName variants on KismetSystemLibrary"
- Inject this knowledge into the Builder system prompt's "known non-existent functions" section

### Fix 3 (TrailParticle — MEDIUM priority)
Add Phase 0 validation check: for each `get_var` step, verify the referenced variable exists on the Blueprint (both `NewVariables` and SCS nodes). This is the same check the resolver already does but with a warning — elevate it to a blocking error so the plan fails at preview time rather than at apply time. This gives the Builder a clear "variable not declared" error before committing to execution.

### Fix 4 (break_struct — HIGH priority)
In `OliveNodeFactory.cpp`, the `break_struct` op handler must:
1. Resolve the struct name from `target` (e.g., `HitResult` -> `FHitResult` UScriptStruct via `FindObject<UScriptStruct>`)
2. Call `UK2Node_BreakStruct::SetStructType(Struct)` BEFORE `AllocateDefaultPins()`
3. Strip leading `~` from pin name hints in `FindPinSmart` (or document that `~` is only for step-output references in plan JSON, not literal pin names)

`BreakHitResult` as a `call` op is a valid workaround for now; it should be documented in the plan_json knowledge section so the Builder reaches for it when `break_struct` fails.

### Additional: Phase 0 should check variable existence
For completeness, add a Phase 0 check: for `get_var`/`set_var` steps, verify the target variable exists on the Blueprint before execution. The resolver already emits a warning for this; Phase 0 should make it a blocking error with the same "Did you mean?" fuzzy match output the resolver provides.
