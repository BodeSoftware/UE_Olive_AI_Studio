# Research: Bullet/Projectile Spawning Session Log Analysis (2026-03-15)

## Question
Analyze the log from a session where the AI was asked to make BP_ThirdPersonCharacter able to shoot bullets with BP_Gun. What happened with projectile spawning, and what issues remain?

## Session Overview

**Prompt:** "make @BP_ThirdPersonCharacter able to shoot bullets with @BP_Gun"
**Run type:** Autonomous (Claude Code CLI, max-turns 500)
**Run time:** 01:52:47 → 01:55:55 = approximately 3 minutes 8 seconds
**Tool calls:** 27
**Auto-continues:** 0
**Exit code:** 0

**Blueprints created/modified:**
1. `/Game/Blueprints/BP_Gun` — created from scratch (parent: Actor) — pickup + fire logic
2. `/Game/Blueprints/BP_Bullet` — created from scratch (parent: Actor) — projectile behavior
3. `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter` — modified — `EquippedGun` variable + shoot trigger

**Context note:** The session began with the user having previously run a separate session
(01:44 – 01:52) that built BP_Gun with components and a pickup plan. That earlier session
used MCP directly (not autonomous). The autonomous run (01:52+) found BP_Gun already
complete (2 graphs, confirmed via blueprint.read at line 2335) and was specifically asked
to add bullet shooting capability.

---

## Findings

### Phase 1: Pre-Autonomous Setup (01:44 – 01:52, non-autonomous)

Before the autonomous run launched, a separate agent built BP_Gun:

**Line 1853-1861:** Agent tried to read BP_Gun at wrong paths:
- `/Game/BP_Gun` — not found
- `/Game/ThirdPerson/Blueprints/BP_Gun` — not found

The AI self-corrected by creating the blueprint fresh at `/Game/Blueprints/BP_Gun`.

**Line 1863-1868:** `blueprint.create` — BP_Gun created (parent: Actor). SUCCESS.

**Line 1869-1877:** `blueprint.add_variable` — Added `EquippedGun` (type: Actor*) to
BP_ThirdPersonCharacter. SUCCESS. (Note: typed as generic `Actor*`, NOT `BP_Gun*`.)

**Line 1878-1932:** Batch of `blueprint.add_component` calls on BP_Gun:
- `GunMesh` (StaticMeshComponent) — SUCCESS
- `PickupSphere` (SphereComponent) — SUCCESS
- Reparent PickupSphere under GunMesh — SUCCESS
- Modify PickupSphere: SphereRadius=150, CollisionProfileName=OverlapAllDynamic (2/3 props set) — SUCCESS

**Line 1955-1989 (FIRST FAILURE — plan_json attempt 1):** AI tried a 12-step pickup
plan on BP_Gun's EventGraph. It included `get_char_mesh` which tried `get_var("Mesh")`
on BP_Gun (which doesn't have a Mesh variable). The resolver generated a warning:

> "Step 'get_char_mesh': Variable 'Mesh' not found on Blueprint 'BP_Gun' or parents or generated class"

The resolver then correctly deduced this was a cross-BP get_var (from the cast step
referencing BP_ThirdPersonCharacter_C). But a different step failed: the plan had 11/12
steps resolve with 1 error, causing a plan resolution failure (line 1987):

```
LogOlivePlanResolver: Plan resolution failed: 11/12 steps resolved, 1 errors, 2 warnings
```

The error was in `set_gun_ref` step: `"Value": "@self.auto"` — the AI used `@self.auto`
as the input value for a SetVar(EquippedGun) step. The IR schema caught `'@self.auto'`
references unknown step `'self'` as a warning (line 1959), but the resolver found a
missing required field: `set_gun_ref` needed the gun actor reference, and `@self.auto`
doesn't resolve to a valid step output.

**Line 1998-2140 (plan_json attempt 2 — SUCCESS):** The AI revised the plan, replacing
`@self.auto` with `@overlap.~Other Actor` (the overlapping actor from the
OnComponentBeginOverlap event). This time, 12/12 steps resolved with 0 errors, 2 warnings.
Phase 0 passed with 1 warning (COLLISION_ON_TRIGGER_COMPONENT — SetCollisionEnabled
targeting PickupSphere, which is a trigger). The plan executed and BP_Gun compiled
SUCCESS at line 2136.

This plan successfully wired:
- `OnComponentBeginOverlap` on PickupSphere
- Branch on `bIsEquipped`
- Cast to BP_ThirdPersonCharacter
- Set `bIsEquipped = true`
- SetCollisionEnabled on PickupSphere and GunMesh (disable both)
- Get character's `Mesh` (SkeletalMeshComponent) via cross-BP get_var
- `K2_AttachToComponent` — attached gun to character mesh
- Set `EquippedGun` on character to gun reference

**Line 2143-2145:** Agent also compiled BP_ThirdPersonCharacter separately — SUCCESS.

---

### Phase 2: Autonomous Run — Bullet System (01:52:47 – 01:55:55)

**Line 2339:** Auto-snapshot created with 2 assets (BP_ThirdPersonCharacter, BP_Gun).
Note: BP_Gun snapshot warning: "No package files found" — BP_Gun was not yet saved to disk.

**Line 2390:** Discovery pass: 5 queries, 8 results in 10.4 seconds:
- "gun projectile shoot", "ranged weapon component", "bullet spawn fire", "line trace aiming", "ammo reload system"

**Line 2401:** 52/85 tools available (filtered by 6 prefixes). Tool filter set before launch.

**Lines 2405-2431: Research phase (6 template lookups):**
1. `olive.get_recipe` for "spawn projectile bullet shoot gun" — SUCCESS
2. `blueprint.get_template` — fps_shooter_bp_character_base, pattern="AddWeapon" — SUCCESS
3. `blueprint.list_templates` — "projectile bullet shoot fire" — SUCCESS
4. `blueprint.get_template` — "projectile" — SUCCESS
5. `blueprint.get_template` — "gun", pattern="Fire" — SUCCESS
6. `blueprint.get_template` — "projectile_patterns" — SUCCESS
7. `blueprint.get_template` — "fps_shooter_bp_projectile_base" — SUCCESS

The AI did thorough research before building. This is the expected discovery-first pattern.

---

### BP_Bullet Creation (01:53:42 – 01:54:20)

**Line 2434:** `blueprint.create` — BP_Bullet (parent: Actor). SUCCESS.

**Line 2450-2464:** Added components:
- `CollisionSphere` (SphereComponent) — SUCCESS (with a benign "LoadPackage: SkipPackage" warning as BP_Bullet isn't on disk yet)
- `BulletMesh` (StaticMeshComponent, parent: CollisionSphere) — SUCCESS
- `ProjectileMovement` (ProjectileMovementComponent) — SUCCESS

**Line 2478:** Added variable `BulletDamage` (float, default 25.0) — SUCCESS.

**Lines 2492-2494:** Triggered a compile of BP_Gun (bAutoCompile) — SUCCESS.

**Line 2499-2503:** `blueprint.modify_component` on ProjectileMovement:
- InitialSpeed=5000, MaxSpeed=5000, ProjectileGravityScale=0 — ALL 3 PROPERTIES SET. SUCCESS.

**Line 2508-2512:** Modify CollisionSphere:
- SphereRadius=10, CollisionProfileName="BlockAllDynamic" — 1/2 properties set. SUCCESS (partial).
- CollisionProfileName was the one that failed to set.

**Lines 2517-2525 (FAILURE — modify BulletMesh):** Agent tried:
```json
{"path": "/Game/Blueprints/BP_Bullet", "name": "BulletMesh", "properties": {"CollisionProfileName": "NoCollision"}}
```
Result: **0/1 properties set. FAILED.**
```
LogOliveWritePipeline: Error: Execution failed for tool 'blueprint.modify_component' (BP_MODIFY_COMPONENT_FAILED): No properties were successfully modified
```
The agent moved on after this failure (no retry observed).

**Lines 2526-2614 (BP_Bullet EventGraph — BeginPlay plan):**

Plan: BeginPlay → SetLifeSpan(3.0) → GetOwner → IsValid → ignore_col → ???

The full 6-step plan is truncated in the params line (2527) but the execution log shows:
- `begin` (event BeginPlay) — node_0 (CallFunction, not shown as event in log — likely FunctionEntry)
- `set_life` (SetLifeSpan) — node_0
- `get_owner` (GetOwner) — node_1
- `valid` (IsValid) — node_2
- `get_col` (get_var CollisionSphere) — node_3
- Final call — node_4

6 nodes, all created. Compiled SUCCESS.

**Lines 2615-2669 (BP_Bullet OnComponentHit plan):**

Plan: OnComponentHit on CollisionSphere → DestroyActor

- `hit` (ComponentBoundEvent on CollisionSphere) — node_5
- `destroy` (DestroyActor) — node_6

Compiled SUCCESS.

---

### BP_Gun Fire Function (01:54:27 – 01:54:28)

**Line 2671 — plan_json for BP_Gun's "Fire" function:**

```json
{
  "steps": [
    {"step_id":"get_muzzle", "op":"get_var", "target":"MuzzlePoint"},
    {"step_id":"get_tf",    "op":"call",    "target":"GetWorldTransform", "inputs":{"Target":"@get_muzzle.auto"}},
    {"step_id":"spawn",     "op":"spawn_actor", "target":"/Game/Blueprints/BP_Bullet", "inputs":{"SpawnTransform":"@get_tf.auto"}},
    {"step_id":"get_owner", "op":"call",    "target":"GetOwner"},
    {"step_id":"set_owner", "op":"call",    "target":"SetOwner", "inputs":{"Target":"@spawn.auto", "NewOwner":"@get_owner.auto"}}
  ]
}
```

**Key observations on the Fire plan:**

1. **`MuzzlePoint` variable — does not exist on BP_Gun.** The resolver at line 2675 processed `get_muzzle` as get_var("MuzzlePoint"). The resolver logged no VARIABLE_NOT_FOUND error here, but the Phase 0 validator only checks for VARIABLE_NOT_FOUND when the variable is on the current BP's scope. `MuzzlePoint` was never added to BP_Gun (no `blueprint.add_variable` or `blueprint.add_component` call for a muzzle). The plan succeeded at line 2687: "Plan resolution succeeded: 5/5 steps resolved, 0 errors, 1 warnings." The resolver's 1 warning is not printed in the log.

   The GetVariable node (node_12) was created with 1 pin (line 2698). A VariableGet node for a non-existent variable creates a "ghost node" with no output data pin — or the variable was successfully created as part of a prior plan. Looking at the data in lines 2741-2744: the data wire `@get_muzzle.auto -> step 'get_tf'.self` succeeded, and `@get_tf.auto -> spawn.SpawnTransform` also succeeded. This means the `MuzzlePoint` get_var node was created and wired. However, no `MuzzlePoint` scene component or variable was ever defined on BP_Gun. The ghost-node guard (zero-pin guard) would have fired if the node had 0 pins — it had 1 pin (line 2698), suggesting the variable was treated as having an unresolved output pin.

   **Bottom line: The `get_muzzle` node references a MuzzlePoint variable that does not exist on BP_Gun. At runtime, this returns a null/invalid scene component reference. GetWorldTransform on a null Target returns a zero transform. The bullet spawns at world origin (0,0,0) with default rotation.**

2. **SpawnActor class resolution warnings (lines 2706-2712):** When creating the SpawnActor node, `CreateSpawnActorNode` tries to find BP_Bullet's class via several module paths (Script/Engine, Script/CoreUObject, etc.) before succeeding. The warnings are cosmetic — the class was ultimately resolved (the node was created with 9 pins at line 2715, which is correct for SpawnActor).

3. **No velocity/direction set after spawn.** The plan has no step to set the projectile's initial velocity direction. The `ProjectileMovementComponent` was configured with `InitialSpeed=5000` on the component (line 2503), but `InitialSpeed` alone only sets the *magnitude* — the direction is derived from the actor's initial velocity vector or its forward vector at spawn. Since the bullet is spawned at an identity transform (zero origin, zero rotation), it will launch in the default direction (world +X) rather than toward wherever the player is aiming.

4. **No GetForwardVector or GetActorForwardVector call.** The Fire plan does not compute any aiming direction. Normally you'd want:
   - Get muzzle world location + rotation
   - SpawnActor at that transform (so the bullet inherits the muzzle's facing direction)
   - OR get the character's aim rotation and set it on the SpawnTransform

5. **SetOwner wiring is correct but semantically questionable.** The plan gets the gun's owner (`GetOwner` on the gun actor itself) and calls `SetOwner` on the spawned bullet using `@spawn.auto` as Target and `@get_owner.auto` as NewOwner. This sets the bullet's owner to whoever owns the gun. This is functionally correct for damage attribution, but the step executes with `get_owner` and `set_owner` both as pure/exec respectively. The log at line 2743: `Data wire OK: @spawn.auto -> step 'set_owner'.self` means the bullet actor return value is passed as Target to SetOwner — correct.

6. **Exec chain: spawn → set_owner only.** The function entry auto-chains to `spawn` (line 2736-2738). The exec order is: FunctionEntry → spawn → set_owner. There is NO `get_tf` step in the exec chain — it is pure (line 2703: "2 pins (pure)"). Correct for `GetWorldTransform` which is a pure function.

7. **The Fire function compiled with 0 errors, 0 warnings (line 2755).** The blueprint compiler did not catch the missing MuzzlePoint because `K2Node_VariableGet` for an unknown variable produces a structural error only when it has no owning blueprint context, not during plan_json (the node is created on the live graph). However, **the variable get will produce a runtime null** — the Kismet compiler may or may not have validated this depending on whether the variable was synthesized as a "missing" type.

---

### BP_ThirdPersonCharacter — Shoot Input (01:54:33 – 01:55:45)

**Line 2760-2775:** Agent added `InputAction` node (K2Node_InputAction) with `InputActionName="LeftMouseButton"` to EventGraph. This node was created with 3 pins. The `InputActionName` on a legacy `K2Node_InputAction` uses the old input system (pre-Enhanced Input), not an `UInputAction` data asset. This works but is the legacy approach.

**Lines 2776-2840 (first attempt — FAILED):**

Plan for BP_ThirdPersonCharacter:
```json
steps: [OnShoot custom_event, get_var(EquippedGun), is_valid, call Fire on BP_Gun_C]
```

Phase 4 (data wiring) log:
```
Phase 4 complete: 1 data connections succeeded, 1 failed
Warning: Phase 4: 1 data wire(s) failed — marking for rollback
```

The successful connection was `EquippedGun -> InputObject` (the is_valid input pin). The failed connection was the `Target` wire for `call_fire` — trying to wire `@get_gun.auto` (which is `Actor*`) into the `Target` pin of `BP_Gun_C::Fire`. The `EquippedGun` variable type is `Actor*` (set at line 1870-1877), but `Fire` expects `BP_Gun_C*`. These types are incompatible without a cast. The pipeline rolled back (line 2837).

**Lines 2841-2925 (second attempt — SUCCESS):**

Plan revised to add an explicit cast step:
```json
steps: [OnShoot, get_var(EquippedGun), is_valid, cast_to(BP_Gun), call Fire on BP_Gun_C]
```

The cast step bridges the type gap: `Actor*` → `BP_Gun*`. Data wiring at line 2909-2910:
- `EquippedGun -> InputObject` (is_valid input) — matched via `ArePinTypesCompatible` schema fallback
- `EquippedGun -> Object` (cast input)
- `@cast_gun.auto -> call_fire.self` (the cast output → Fire Target)

All 3 data connections succeeded. Compiled with 1 warning (line 2921, warning text not logged).

**Lines 2926-2935:** Agent removed the old `InputAction` node (node_0, the `K2Node_InputAction`).

**Lines 2936-2946:** Added `InputKey` node with `key="LeftMouseButton"` instead. This is the correct legacy input node. Created with 3 pins.

**Lines 2969-2981:** `blueprint.connect_pins` — wired `node_10.Pressed` → `node_7.exec` (the is_valid exec input). SUCCESS. This connects LMB press to the IsValid → Cast → Fire chain.

**BUT:** The `OnShoot` custom event (node_5) is NOT connected to the LMB input. The connect_pins call connected the InputKey `Pressed` pin directly to `node_7` (IsValid) — bypassing the custom event entirely. The custom event `OnShoot` is now a floating, unconnected orphan node in the graph.

**Lines 2982-2988:** `blueprint.compile` — BP_ThirdPersonCharacter compiled SUCCESS, 0 errors, 0 warnings.

The final shooting chain in BP_ThirdPersonCharacter is:
```
InputKey(LMB).Pressed → IsValid(EquippedGun) → [IsValid] → Cast(Actor→BP_Gun) → [CastSucceeded] → Fire()
```
This is functional — pressing LMB calls Fire on the equipped gun if it's valid and castable.

---

## Summary of Issues Found

### Issue 1: MuzzlePoint Does Not Exist (Critical)
**Severity: High — Causes incorrect spawn location**

The Fire function in BP_Gun uses `get_var("MuzzlePoint")` (line 2675). No `MuzzlePoint` variable or component was ever added to BP_Gun. The AI planned to use a scene component as a muzzle point, but never created it.

**Effect at runtime:** GetVariable for MuzzlePoint returns null/invalid. `GetWorldTransform` on a null SceneComponent reference returns identity transform (origin 0,0,0). Bullet spawns at world origin with zero rotation, not at the gun barrel.

**Root cause in session:** The AI separated the Fire function from the "add muzzle component" step. The muzzle point setup likely never happened because the AI considered BP_Gun's structure already complete when it started the autonomous run (it read BP_Gun with 2 graphs at line 2335 and did not add components during the autonomous phase).

### Issue 2: No Spawn Direction / Aiming (Critical)
**Severity: High — Bullet travels in wrong direction**

The spawn transform used for BP_Bullet is `@get_tf.auto` (the world transform of MuzzlePoint). Even if MuzzlePoint existed, this gives the *position* of the muzzle but not necessarily the *aiming rotation*. The AI used `GetWorldTransform` which returns position + rotation of the component. If MuzzlePoint were a SceneComponent correctly set up facing forward on the gun, and the gun were correctly oriented, this would produce the right spawn transform.

However, because MuzzlePoint is null, this is moot — the spawn transform is identity. The `ProjectileMovementComponent` with `InitialSpeed=5000` will launch the bullet in its forward direction (world +X by default) regardless of where the player is aiming.

No `GetActorForwardVector`, `GetControlRotation`, or camera direction calculation is included in the plan. A proper implementation would:
1. Get the character's aim/camera rotation
2. Compute spawn location (muzzle or camera trace start)
3. Set SpawnTransform to that location + rotation

### Issue 3: CollisionProfileName Fails on BulletMesh (Low)
**Severity: Low — Visual/physics only, does not prevent shooting**

Line 2517-2525: `modify_component` on `BulletMesh` with `CollisionProfileName="NoCollision"` failed (0/1 properties set). The modify_component tool could not set `CollisionProfileName` on a StaticMeshComponent. This is the same issue seen in prior sessions — `CollisionProfileName` is a string property that requires special handling via `SetCollisionProfileName()` rather than a simple UPROPERTY setter. BulletMesh will use its default collision profile.

Additionally, line 2512: CollisionSphere's `CollisionProfileName="BlockAllDynamic"` also failed (1/2 set — SphereRadius succeeded, profile failed). So neither collision profile was set via modify_component.

### Issue 4: EquippedGun Typed as Actor* Instead of BP_Gun* (Low)
**Severity: Low — Requires cast, which AI correctly added**

Line 1870: `EquippedGun` was added as type `Actor*` instead of `BP_Gun*`. This required the AI to add an explicit cast step in its second ThirdPersonCharacter plan (which it did correctly). It works, but it's an extra node and a weaker type.

### Issue 5: OnShoot Custom Event is Orphaned (Low)
**Severity: Low — Dead code, no functional impact**

The second ThirdPersonCharacter plan created an `OnShoot` custom event (node_5) but the final input wiring connected `InputKey.Pressed → IsValid` directly, bypassing OnShoot. OnShoot sits unconnected in the graph. This wastes a node and could be confusing.

### Issue 6: Legacy InputKey vs Enhanced Input (Low)
**Severity: Low — Works but not future-proof**

The AI used `K2Node_InputKey` with `key="LeftMouseButton"` — the old pre-Enhanced-Input system. The project uses Enhanced Input (EnhancedInput plugin is mounted at line 66). Ideally the AI would use `K2Node_EnhancedInputAction` with an `UInputAction` data asset. The legacy key works for basic testing but conflicts with the project's input architecture.

### Issue 7: CollisionProfileName on SphereComponent (Informational)
**Severity: Informational — Blueprint compiled anyway**

The validator at line 2031 fired `COLLISION_ON_TRIGGER_COMPONENT` warning for the pickup plan's `SetCollisionEnabled` on PickupSphere. The plan went ahead (it was only a warning). The plan executed correctly — disabling the overlap sphere after pickup is correct behavior. The validator's warning is a false positive in this specific pickup-equip context.

---

## Compilation Results Summary

| Blueprint | Compile | Errors | Warnings |
|-----------|---------|--------|----------|
| BP_Gun (pickup plan) | SUCCESS | 0 | 0 |
| BP_Gun (Fire plan) | SUCCESS | 0 | 0 |
| BP_Bullet (BeginPlay plan) | SUCCESS | 0 | 0 |
| BP_Bullet (OnComponentHit plan) | SUCCESS | 0 | 0 |
| BP_ThirdPersonCharacter (attempt 1) | ROLLED BACK | — | — |
| BP_ThirdPersonCharacter (attempt 2) | SUCCESS | 0 | 1 |
| BP_ThirdPersonCharacter (final compile) | SUCCESS | 0 | 0 |

The single compile warning on BP_ThirdPersonCharacter attempt 2 (line 2921) was not logged with text — likely related to the Actor*→BP_Gun_C type mismatch being implicitly accepted.

---

## Recommendations

1. **MuzzlePoint must be created before Fire logic.** The agent needs to add a SceneComponent (typically an ArrowComponent or SceneComponent named MuzzlePoint) to BP_Gun, positioned at the barrel tip, before using `get_var("MuzzlePoint")` in the Fire function. The VARIABLE_NOT_FOUND Phase 0 check should have caught this — investigate why it did not fire for `MuzzlePoint` in the Fire plan.

2. **SpawnTransform must incorporate aiming direction.** For a character-based shooter, the correct pattern is: get character's `GetControlRotation()` → `MakeRotator` → combine with muzzle world location → `MakeTransform` → use as SpawnTransform. Alternatively, use camera line trace for the direction. Simply getting the muzzle component's world transform only works correctly if the muzzle component always faces the correct direction (which requires the gun to be properly attached + socket-aligned).

3. **`get_var` VARIABLE_NOT_FOUND phase 0 check gap.** `MuzzlePoint` was used in a `get_var` op within a function graph targeting BP_Gun, yet no Phase 0 error fired. The Phase 0 VARIABLE_NOT_FOUND check needs to be audited — it may be checking NewVariables and SCS on the outer BP, but the resolver at line 2675 did not flag a "not found" error for `MuzzlePoint` at all (unlike line 1982 which did fire a warning for `Mesh` on BP_Gun). Likely the resolver silently accepted `MuzzlePoint` as an unresolved variable without surfacing the issue.

4. **`CollisionProfileName` on StaticMeshComponent and SphereComponent fails silently.** Two out of three `modify_component` calls with `CollisionProfileName` failed (lines 2512, 2525). This is a known issue. The agent should use `set_collision_profile` or a dedicated tool rather than `modify_component` for collision profiles.

5. **`EquippedGun` should be typed as `BP_Gun*` not `Actor*`.** The knowledge base or prompt should guide the AI to use the correct concrete type for object references when the target class is known. This avoids needing the extra cast step.

6. **OnShoot custom_event should be the entry point, wired from InputKey.** The AI disconnected its own abstraction. If `OnShoot` is created, the InputKey should wire to it, and it should wire forward to IsValid. The AI skipped this by connecting InputKey directly to IsValid.

7. **Enhanced Input should be preferred over legacy InputKey.** For projects that have EnhancedInput mounted, the AI should use `blueprint.add_node` with type `EnhancedInputAction` and an existing `UInputAction` asset (or create one first via `editor.run_python`).

8. **Autonomous run needs a "MuzzlePoint" reminder in Fire logic knowledge.** The `gun` reference template (which the AI did look up at line 2422-2424, pattern="Fire") should document that the muzzle point must be a SceneComponent on the gun actor, and it must exist before the Fire plan references it.

9. **Total session was fast and largely successful.** 3 minutes 8 seconds for 3 complete Blueprints with 0 compile errors is a strong result. The critical failure (wrong spawn location/direction) is a logic error, not a tool error. All tool calls succeeded except the 2 modify_component/CollisionProfileName failures and 1 initial plan rollback.

10. **PIE run was very brief (01:26:50 – 01:26:51, 1.4 seconds).** The user likely noticed immediately that bullets were not appearing as expected and stopped PIE. The two PIE sessions both lasted under 2 seconds.
