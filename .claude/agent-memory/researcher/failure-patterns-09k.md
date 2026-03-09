# Failure Patterns — 2026-03-09 Run (Log Analysis)

Research report: `plans/research/failure-patterns-2026-03-09k.md`

## Bug 1: GetForwardVector — Resolver/Executor Target Class Mismatch (HIGH)
- Resolver alias-fallback logic works correctly: detects `GetActorForwardVector` doesn't match `InRot` input, falls back to `KismetMathLibrary::GetForwardVector`
- Executor calls `FindFunction('GetForwardVector', '')` bare — alias map fires again, returns wrong function
- Root cause: executor does not pass `ResolvedTargetClass` from the resolved step to FindFunction
- Fix: when `ResolvedStep.ResolvedTargetClass` is set, pass it to FindFunction to bypass alias map

## Bug 2: break_struct Not Initialized With Struct Type (HIGH)
- `break_struct` op creates node with 1 pin (pure only) — struct type never applied
- `UK2Node_BreakStruct::SetStructType(Struct)` must be called BEFORE `AllocateDefaultPins()`
- Workaround: use `call BreakHitResult` (KismetSystemLibrary function) — works correctly
- Tilde prefix `~HitActor` in step output refs is plan-json convention, not a literal pin name — strip `~` before FindPinSmart

## SetFloatPropertyByName — Removed in UE5 (Knowledge Issue)
- Does NOT exist in UE 5.5 — removed from KismetSystemLibrary
- Closest surviving: `SetFieldPathPropertyByName`, `SetVector3fPropertyByName`, `SetTransformPropertyByName`
- BuildFunctionPinReference did NOT see it in the Build Plan — Builder hallucinated it during execution
- Add to alias map as a dead-end with "removed in UE5" error, or add to self-correction known-bad list

## TrailParticle — Variable Never Created (Builder Omission)
- Builder skipped `add_variable TrailParticle` on BP_ArrowProjectile despite Plan referencing it
- Rate-limiting at 06:57:44 (bHasHit failed WriteRateLimit twice) may have disrupted sequencing
- Resolver correctly emits "Variable not found" warning but does not block
- Recommended fix: Phase 0 validator should block on `get_var` referencing undeclared variables

## Phase 0 Gap: Variable Existence Not Checked
- Phase 0 currently only checks COMPONENT_FUNCTION_ON_ACTOR and EXEC_WIRING_CONFLICT
- Should add: for each `get_var`/`set_var` step, verify variable exists on Blueprint (NewVariables + SCS)
- Resolver already does this check and warns — Phase 0 should elevate to blocking error
