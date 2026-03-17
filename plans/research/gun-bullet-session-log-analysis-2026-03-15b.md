# Research: Gun/Bullet Session Log Analysis — 2026-03-15 (Run B)

## Question
Full analysis of the "successful" gun/bullet/shooting system run logged on 2026-03-15. Session covered
three Blueprints: BP_Bullet, BP_Gun, BP_ThirdPersonCharacter. Reportedly a success run — what was built,
what failed, what fixed itself, and what remains broken.

---

## Findings

### 1. Session Timing and Efficiency

- **Engine start:** 2026.03.15-23.49.08
- **Run launched:** 2026.03.15-23.56.23 (autonomous mode, tool filter applied)
- **Run completed:** 2026.03.16-00.00.35
- **Total agent wall-clock time:** ~4 minutes 12 seconds
- **Outcome:** `outcome=0` (success), line 3169
- **Auto-continues:** 0
- **Total tool calls (MCP):** ~42 (counted from MCP tools/call entries)
- **Community blueprint searches:** 5 (lines 1795–1827), done before the run started (pre-research phase)
- **Recipe lookup:** 1 (`olive.get_recipe` query "gun weapon pickup equip shoot projectile", line 1844)
- **Template lookup:** 1 (`blueprint.get_template` for `fps_shooter_bp_character_base`, pattern `AddWeapon`, line 1848)

Tool filter at line 1841: 52/85 tools exposed (filtered by 6 prefixes).

---

### 2. What Was Built

**BP_Bullet** (`/Game/Blueprints/BP_Bullet`, parent: Actor):
- Components: `CollisionSphere` (SphereComponent), `BulletMesh` (StaticMeshComponent), `ProjectileMovement` (ProjectileMovementComponent)
- Component hierarchy: BulletMesh reparented under CollisionSphere (lines 1959–1965)
- CollisionSphere: SphereRadius=10, CollisionProfileName="BlockAllDynamic" (line 2017)
- BulletMesh: StaticMesh=`/Engine/BasicShapes/Sphere`, RelativeScale3D=(0.1,0.1,0.1), CollisionProfileName="NoCollision" (line 2026)
- ProjectileMovement: InitialSpeed=3000, MaxSpeed=3000, ProjectileGravityScale=0 (line 2035)
- EventGraph: BeginPlay→SetLifeSpan(3.0); OnComponentHit(CollisionSphere)→DestroyActor (line 2071)

**BP_Gun** (`/Game/Blueprints/BP_Gun`, parent: Actor):
- Components: GunRoot (SceneComponent), GunMesh (StaticMeshComponent, child of GunRoot), PickupSphere (SphereComponent, child of GunRoot), MuzzlePoint (ArrowComponent, child of GunMesh)
- GunMesh: StaticMesh=`/Engine/BasicShapes/Cube`, RelativeScale3D=(0.5,0.15,0.15) (line 2053)
- MuzzlePoint: RelativeLocation=(X=50,Y=0,Z=0) (line 2062)
- PickupSphere: SphereRadius=150, CollisionProfileName="OverlapAllDynamic" (line 2044)
- Variables: `bIsEquipped` (bool, false), `FireRate` (float, 0.2), `bCanFire` (bool, true)
- Functions: `Fire` (fire rate gating + spawn bullet + SetTimer), `ResetCanFire` (set bCanFire=true)
- EventGraph: OnComponentBeginOverlap (PickupSphere) → branch on !bIsEquipped → CastTo<BP_ThirdPersonCharacter> → set bIsEquipped=true → AttachToComponent(parent=CharMesh) → SetCollisionEnabled(PickupSphere,NoCollision) → SetCollisionEnabled(GunMesh,NoCollision) → SetOwner(Character) → PrintString
- EventGraph also has: EquipGun call node (K2Node_CallFunction) wired with Self and Character cast reference

**BP_ThirdPersonCharacter** (`/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter`):
- Variable added: `EquippedGun` (object type BP_Gun_C) (line 2007)
- Function added: `EquipGun` (input: Gun:BP_Gun_C) → stores Gun into EquippedGun (lines 2965, 2976–3029)
- EventGraph: InputKey(LeftMouseButton).Pressed → IsValid(EquippedGun) → CastTo<BP_Gun>(EquippedGun) → Fire(Target=CastResult) (lines 2822, 2877)
- Connect: node_0.Pressed → node_5.exec (IsValid node), line 2952

All three BPs compiled SUCCESS (lines 3151–3166).

---

### 3. Tool Call Breakdown

| Tool | Count | Success | Failed |
|------|-------|---------|--------|
| blueprint.create | 2 | 2 | 0 |
| blueprint.add_component | 7 | 7 | 0 |
| blueprint.reparent_component | 4 | 4 | 0 |
| blueprint.add_variable | 4 | 4 | 0 |
| blueprint.add_function | 3 | 3 | 0 |
| blueprint.modify_component | 6 | 6 | 0 |
| blueprint.apply_plan_json | 12 | 7 | 5 |
| blueprint.add_node | 3 | 2 | 1 |
| blueprint.connect_pins | 4 | 4 | 0 |
| blueprint.compile | 3 | 3 | 0 |
| olive.get_recipe | 1 | 1 | 0 |
| blueprint.get_template | 1 | 1 | 0 |
| olive.search_community_blueprints | 5 | 5 | 0 |

**Overall tool success rate:** ~88% (37/42 calls succeeded)

**plan_json calls:** 12 total, 7 succeeded (58%). This is the lowest plan_json rate seen in recent runs.

---

### 4. Plan JSON Failures — Root Causes

**Failure 1 — Fire function, attempt 1 (line 2174):**
Plan used a `delay` op inside the `Fire` function graph. Phase 0 validator correctly caught and rejected it:
`LATENT_IN_FUNCTION — step 'delay' uses 'Delay' in function graph 'Fire'` (line 2172).
Agent self-corrected on attempt 2 by replacing Delay with K2_SetTimer (SetTimerByFunctionName alias).
This is a CONFIRMED WORKING validator check — working exactly as designed.

**Failure 2 — BP_Gun pickup event, attempt 1 (line 2393):**
Plan included step `set_gun_ref` calling function `SetEquippedGun` which does not exist on any class.
Full 7-step search trail exhausted with no match (lines 2389–2391). Resolver reported 10/11 steps
resolved, 1 error. Agent retried by removing that step.

**Failure 3 — BP_Gun pickup event, attempt 2 (line 2425):**
Same plan, modified to use `set_var` on `EquippedGun` instead. However this plan had a different
structural issue: the resolver reported `10/11 steps resolved, 1 errors, 2 warnings` at line 2424,
but the cause was different. The resolver was trying to cross-reference Mesh via cast step (from
BP_ThirdPersonCharacter) but the plan still had a data wire resolution failure at execution time.
The log at 2424 says plan resolution failed; this is actually a second resolve failure on a slightly
different plan. Rejected at resolve phase before execution.

**Failure 4 — BP_Gun pickup event, attempt 3 (line 2567):**
Phase 0 passed (0 errors, 1 warning: `COLLISION_ON_TRIGGER_COMPONENT` at line 2469). Execution
reached Phase 4 (data wiring) but 1 data connection failed (line 2555). The failed wire was
`@attach.auto` → `Parent` on K2_AttachToComponent — specifically the attempt to wire the GunRoot
component to the `Parent` socket param of AttachToComponent. Root cause: the plan used
`@_synth_getcomp_gunroot.auto` as the Parent input, but GunRoot is a SceneComponent and
K2_AttachToComponent's Parent pin expects a `SceneComponent` object reference. The Mesh→Parent
wire at line 2553 connected correctly (CharMesh→Parent), so the failure was on a different data pin.
Transaction rolled back (line 2565).

**Failure 5 — BP_Gun pickup event, attempt 4 (line 2746, SUCCESS):**
Agent revised plan to remove the GunRoot attachment (attach directly to character mesh instead) and
also added `SetCollisionEnabled(GunMesh, NoCollision)` (disable mesh collision after pickup). This
plan resolved 13/13 steps. Phase 0 passed with 1 warning (COLLISION_ON_TRIGGER_COMPONENT again, but
as a warning not an error). Pre-Phase cleanup removed 11 nodes from the previous failed attempt
(lines 2622–2633). Execution succeeded: 13 nodes, 15 connections, 0 failed, compile SUCCESS.

**Failure 6 — BP_ThirdPersonCharacter OnActorBeginOverlap (line 2783):**
Agent tried to create `OnActorBeginOverlap` event on BP_ThirdPersonCharacter. Node factory
searched parent class, interfaces, component delegates, and Enhanced Input — not found (lines
2767–2775). This is a C++ delegate event that requires `bGenerateOverlapEvents=true` and is
dispatched via the component system, not via a Blueprint node with that name directly. In the
ThirdPersonCharacter context it is available via the Event Graph via `Event ActorBeginOverlap` not
`OnActorBeginOverlap`. Agent self-corrected by abandoning this approach and using InputKey node
for mouse button trigger instead.

**Failure 7 — BP_ThirdPersonCharacter fire wiring, attempt 1 (line 2875):**
Plan: GetVar(EquippedGun) → IsValid → call Fire with `Target=@get_gun.auto`. Phase 4 had 1 data
wire fail: the `@get_gun.auto` → `self` pin on the Fire CallFunction node (line 2863). Root cause:
EquippedGun was declared as type `BP_Gun_C` (object), but the Target pin on a cross-BP `Fire` call
requires a cast-compatible reference. The plan tried to wire an `Actor*` (the EquippedGun variable)
directly as Target on the Fire function without a cast. Execution committed 2 connections, 1 failed.
Agent self-corrected by inserting an explicit CastTo<BP_Gun> step before calling Fire.

**Failure 8 — add_node CallFunction for EquipGun on BP_Gun (line 3046):**
The agent tried `blueprint.add_node` with type `CallFunction`, `function_name="EquipGun"`,
`function_class="BP_ThirdPersonCharacter_C"`. This failed (line 3046). Root cause is likely that
`BP_ThirdPersonCharacter` was not yet compiled with the new `EquipGun` function at the time the
node factory tried to resolve `BP_ThirdPersonCharacter_C::EquipGun`, or the class path format
was rejected. Agent fell back to a plan_json `call` op with `target_class="BP_ThirdPersonCharacter"`
which succeeded (line 3089).

**Failure 9 — BP_Gun SetGunOnCharacter custom_event plan (line 3034):**
The plan included `custom_event` target `SetGunOnCharacter` and `@self_ref.auto` input (a reference
to self). This failed almost immediately (0.10ms execution, line 3035) — the plan was rejected at
resolve time. The `@self_ref.auto` input is not a valid step reference. The agent pivoted immediately
to a different approach using add_node + connect_pins granular tools.

---

### 5. New Fixes in Action

**LATENT_IN_FUNCTION validator (Phase 0 Check 3) — CONFIRMED WORKING**
Fired correctly on the first Fire function attempt (line 2172). Agent self-corrected without needing
a retry loop, redesigning the timer approach immediately. This is the cleanest self-correction in
the log.

**CleanupPreviousPlanNodes — CONFIRMED WORKING**
Fired twice:
- Line 2622–2633: Cleaned 11 nodes from failed BP_Gun pickup attempt 3 before attempt 4
- Line 2894–2897: Cleaned 3 nodes from failed BP_ThirdPersonCharacter fire attempt 1 before attempt 2
Both cleanups were clean (removed N/N tracked nodes with 0 leftover).

**ExpandComponentRefs (@GunRoot, @PickupSphere, @GunMesh) — CONFIRMED WORKING**
Lines 2363–2368: Both `@GunRoot` and `@PickupSphere` bare refs detected by IR schema, then expanded
by resolver into `_synth_getcomp_gunroot` and `_synth_getcomp_pickupsphere` get_var steps.
Lines 2571–2576: Same pattern for `@PickupSphere` and `@GunMesh` in attempt 4.

**COLLISION_ON_TRIGGER_COMPONENT validator warning — CONFIRMED FIRING**
Lines 2469 and 2616: The new check correctly identified that SetCollisionEnabled was targeting
`PickupSphere` (a trigger sphere) and suggested the agent might want to target `GunMesh` instead.
The agent responded by also adding `SetCollisionEnabled(GunMesh,NoCollision)` in attempt 4.
However, this is a WARNING not an ERROR, so execution proceeded even on attempt 3 (which then
failed for a different reason). The agent clearly absorbed the warning message because attempt 4
added GunMesh collision disable explicitly.

**PreResolvedFunction — CONFIRMED WORKING**
Lines 2282, 2287, 2314, 2506, 2515, 2662, 2671, 2685, 2846, 2913: "Used pre-resolved function"
appears 10 times across successful plans. The resolver→executor contract is working.

**CastTargetMap for cross-BP function resolution — CONFIRMED WORKING**
Lines 2370, 2405, 2436, 2578, 2752, 2881: CastTargetMap built with 3 or 1 entries in multiple
plans. Line 2828 shows `Fire` resolved to `target_class='BP_Gun_C'` via CastTargetMap inference.
Line 2886 confirms same.

**FindTypeCompatibleOutput schema fallback — CONFIRMED WORKING**
Line 2933: `FindTypeCompatibleOutput schema fallback: matched 'EquippedGun' via ArePinTypesCompatible`
— this fired when wiring the EquippedGun object reference into the CastTo node's Object pin in
the successful ThirdPersonCharacter fire plan.

**PostPlacedNewNode / pin interactivity** — Not directly observable in log, but all plan_json
successes compiled clean and no pin-dragging issues were reported in this run (no GHOST_NODE_PREVENTED
or orphaned pin errors anywhere in the log). Pins were created and connected successfully in all
11 successful plan_json executions.

---

### 6. Collision Handling

**ExpandMissingCollisionDisable** — This specific fix was queried. There is no log line with that
exact string. The mechanism appears to instead be the `COLLISION_ON_TRIGGER_COMPONENT` Phase 0
validator warning, which fires as a hint rather than an auto-inject. The agent absorbed the hint
and manually added the GunMesh collision disable step in attempt 4. There is NO automatic injection
of collision-disable steps by the resolver in this run — the fix is in the form of a validator
warning that guides the agent's retry.

BulletMesh collision: `CollisionProfileName="NoCollision"` set via `modify_component` (line 2026).
This was done proactively by the agent without any warning. SUCCESS.

CollisionSphere collision: `CollisionProfileName="BlockAllDynamic"` (line 2017). SUCCESS.
PickupSphere: `CollisionProfileName="OverlapAllDynamic"` (line 2044). SUCCESS.

In the successful pickup plan (attempt 4), both collision disables were explicit:
- `disable_pickup`: SetCollisionEnabled on PickupSphere (via synthesized get_var)
- `disable_mesh_col`: SetCollisionEnabled on GunMesh (via synthesized get_var)
Both wired correctly (lines 2730–2731).

---

### 7. Mesh Assignment

**BP_Bullet BulletMesh:**
`modify_component` at line 2025–2032 set `StaticMesh="/Engine/BasicShapes/Sphere"` and
`RelativeScale3D="(X=0.1,Y=0.1,Z=0.1)"`. This call returned SUCCESS (line 2032–2033).
The bullet DOES have a mesh assigned this run, unlike the previous run 2026-03-15a.

**BP_Gun GunMesh:**
`modify_component` at line 2052–2059 set `StaticMesh="/Engine/BasicShapes/Cube"` with scale
(0.5, 0.15, 0.15). SUCCESS (line 2060).

Both mesh assignments succeeded via modify_component. This is a notable improvement from the
prior run's VARIABLE_NOT_FOUND / mesh assignment gap.

---

### 8. Attach/Socket Handling

The pickup logic in BP_Gun EventGraph attaches the gun to the character's mesh via:
- `K2_AttachToComponent` (Actor::K2_AttachToComponent)
- Target: the character (cast result)
- Parent: `@cast.auto → get_mesh_ref.Mesh` → BP_ThirdPersonCharacter's `Mesh` component (SkeletalMeshComponent)

**Socket name:** The log shows Phase 5 set 8 defaults (line 2735), but the specific pin defaults
for AttachmentRule and SocketName are not individually logged. From the plan JSON params
(truncated in the log) we cannot confirm if a socket name like `hand_r` was provided.
The plan used `K2_AttachToComponent` with `@cast.auto` as `get_mesh_ref` Target — attaching
to the character's Mesh component, but no socket name is visible in the logged fragment.

This is the **same gap identified in run 2026-03-13**: gun will attach to the character mesh root
but not to a specific bone/socket. Cosmetically the gun will not appear in the correct hand position.

The K2_AttachToComponent call at line 2506–2508 created node_28 with 10 pins. The `SocketName` pin
exists but whether a default was set to `"hand_r"` is not confirmed from this log.

---

### 9. Shooting Logic

**Fire function in BP_Gun:**
1. get_var(bCanFire) → branch(True→proceed, False→skip/nothing)
2. set_var(bCanFire=false)
3. get_var(MuzzlePoint) → GetWorldLocation → GetWorldRotation → MakeTransform
4. SpawnActor(BP_Bullet) using MuzzlePoint transform
5. get_var(FireRate) → K2_SetTimer(FunctionName="ResetCanFire", Time=FireRate, bLooping=false)

This is a solid fire rate gate pattern. The timer calls `ResetCanFire` which sets bCanFire=true
after FireRate seconds, re-enabling fire.

**MuzzlePoint location:** CONFIRMED WORKING. Steps get_muzzle→get_loc→get_rot→make_tf→spawn
all succeeded (lines 2277–2307). GetWorldLocation and GetWorldRotation alias correctly to
K2_GetComponentLocation and K2_GetComponentRotation on SceneComponent. The spawn transform is
derived from MuzzlePoint's world transform. Bullet will spawn at the correct muzzle position.

**Aiming direction:** The spawn transform uses `GetWorldRotation` from MuzzlePoint, NOT
GetControlRotation from the player controller. This means bullets fire in the direction the
MuzzlePoint arrow is pointing (which is the gun's forward direction), not where the camera is
aiming. MuzzlePoint is positioned X=50 from GunMesh. Since GunMesh is parented to GunRoot on the
Actor (not on the character's skeletal mesh socket), the gun direction will follow the actor's
yaw but NOT the camera pitch. **Bullets will not follow crosshair aim.** This is the same
aiming gap from run 2026-03-15a — unresolved.

**Trigger chain:**
InputKey(LeftMouseButton).Pressed → IsValid(EquippedGun) → CastTo<BP_Gun> → Fire(Target=CastResult)
The InputKey node (node_0, line 2790) was connected to the IsValid node (node_5) at line 2952.
The entire fire chain is wired and compiled. Fire rate gating is present. This is correct and
functional.

**Self-correction on type mismatch:** In attempt 1 (line 2863), `EquippedGun` (type BP_Gun_C)
could not be directly wired as Target to `Fire` without a cast (1 data wire failed). Agent
self-corrected by inserting explicit CastTo<BP_Gun> on attempt 2, which succeeded (line 2934
shows EquippedGun→cast Object pin wired via schema fallback).

---

### 10. Remaining Issues and Gaps

**Issue 1 — No aiming direction from camera (CRITICAL, unresolved)**
Bullets spawn at MuzzlePoint and travel in the gun's local forward direction, not the camera
pitch. The agent did not wire GetControlRotation from the player controller. Without this, the
gun is cosmetically functional but physically wrong: bullets will fire horizontally even if the
player is aiming up or down. Severity: breaks gameplay.

**Issue 2 — No socket name on AttachToComponent**
The gun will attach to the character's SkeletalMeshComponent root, not to a specific bone like
`hand_r`. This is a cosmetic issue in PIE but means the gun floats at the mesh origin.

**Issue 3 — Gun attached to character mesh but gun is still a world Actor**
The pickup logic attaches the gun actor to the character mesh via K2_AttachToComponent. However
the gun BP is still an independent Actor spawned in the world. It will follow the character, but
collision, physics, and replication semantics may be wrong. There is no `DetachFromActor` or
`HideActor` on the original world placement to handle the transition cleanly.

**Issue 4 — InputKey (legacy) used instead of Enhanced Input**
Line 2786 shows `blueprint.add_node` with type `InputKey` and key `LeftMouseButton`. This is
the legacy input system. The project uses Enhanced Input (evidenced by `IA_*` asset searches at
lines 2768–2773). The InputKey node will work in PIE but is technically deprecated for UE 5.x
projects with Enhanced Input enabled.

**Issue 5 — COLLISION_ON_TRIGGER_COMPONENT fires as warning but plan still disables PickupSphere**
The validator warns that SetCollisionEnabled on a trigger SphereComponent is unusual. But in
this specific pickup design, disabling PickupSphere after equip IS the correct behavior (so the
gun is not picked up again). The warning is a false positive for this pattern. It should remain
a warning (not error) but the message could be refined to say "if you intend to prevent
re-pickup, this is correct."

**Issue 6 — Short type name warning for BP_ThirdPersonCharacter_C (cosmetic)**
Lines 2457–2460 and 2604–2607 show `LogClass: Warning: Short type name "BP_ThirdPersonCharacter_C"
provided for TryFindType. Please convert it to a path name`. This fires from `CheckVariableExists`
in `OlivePlanValidator.cpp:516`. The validator should use the full path when resolving cross-BP
class names. Not a functional failure (the warning is non-fatal) but produces log noise.

**Issue 7 — OnActorBeginOverlap event not resolvable on ThirdPersonCharacter (logged as error)**
Line 2774–2775: Agent tried `OnActorBeginOverlap` as an event name, which is not directly
available on Character BPs. The correct event is `ActorBeginOverlap` (without "On" prefix in
some contexts) or the actual Blueprint event name. The agent self-corrected, but this adds
~7 seconds of latency. The error message suggests using `project.search type=InputAction` which
is wrong advice for this case — the event is a C++ delegate, not an IA asset.

**Issue 8 — EquipGun being called from both BP_Gun EventGraph AND there is also a gun pickup in BP_Gun**
The BP_Gun EventGraph has the pickup logic (OnComponentBeginOverlap → attach → call EquipGun).
The character's EquipGun function stores the gun reference. However, the BP_Gun EventGraph at
lines 3089–3115 also has a separate plan that creates a standalone `EquipGun` call node with
Self wired as the Gun param. This appears to be a duplicate/orphaned attempt from the agent's
struggle to call EquipGun from within BP_Gun. The call at line 3089 succeeds (24ms), but it is
unclear whether this EquipGun call is actually connected in the exec chain. The connect_pins at
lines 3104 and 3117 wire `node_34.then → node_35.execute` and `node_25.AsBP Third Person
Character → node_35.self`. So the EquipGun call IS wired as exec continuation from the
AttachToComponent chain (node_34 is PrintString, node_35 is EquipGun). This is correct: the
full pickup chain ends with EquipGun being called on the character with Self as the Gun param.

---

### 11. Key New Behaviors Confirmed vs Prior Runs

| Behavior | Prior Run (03-15a) | This Run (03-15b) |
|---|---|---|
| Mesh assigned to BulletMesh | FAILED (VARIABLE_NOT_FOUND) | SUCCESS |
| Mesh assigned to GunMesh | SUCCESS | SUCCESS |
| LATENT_IN_FUNCTION catch | Not exercised | CONFIRMED (caught Delay in function) |
| COLLISION_ON_TRIGGER_COMPONENT | Not present | CONFIRMED (fires as warning) |
| CleanupPreviousPlanNodes | CONFIRMED | CONFIRMED (cleaned 11+3 nodes) |
| ExpandComponentRefs (@GunRoot etc) | CONFIRMED | CONFIRMED |
| Self-correcting Actor*→BP_Gun cast | CONFIRMED | CONFIRMED |
| MuzzlePoint world location | FAILED | SUCCESS |
| Aiming direction (GetControlRotation) | FAILED | FAILED (same gap) |
| Socket name on attach | FAILED | Unclear (not logged) |
| Enhanced Input | Used InputKey (legacy) | Used InputKey (legacy) |
| auto-continues | 3 | 0 |
| Agent time | ~3:08 | ~4:12 |

The 03-15b run is longer but more complete: it actually built the fire rate gate, ResetCanFire
function, and a full EquipGun function with proper typing. The 03-15a run had bullets spawning
at world origin (0,0,0). This run spawns at MuzzlePoint correctly.

---

## Recommendations

1. **Aiming direction is a persistent gap across all gun runs.** The resolver should have a
   knowledge note or the gun reference template should explicitly state that SpawnActor transform
   for projectiles should use GetControlRotation (PlayerController) for pitch, not GetWorldRotation
   from a mesh attachment point. Consider adding this to `projectile_fire_patterns.json` reference
   template.

2. **COLLISION_ON_TRIGGER_COMPONENT warning is mostly a false positive for pickup patterns.**
   The warning message should distinguish between "you are disabling the trigger that enables
   pickup (correct)" vs "you are disabling collision on a physics-simulating sphere (suspicious)."
   Add context: if `CollisionProfileName == "OverlapAllDynamic"` it IS the pickup trigger and
   disabling it on equip is intentional.

3. **The `CheckVariableExists` path in OlivePlanValidator.cpp:516 uses short type names for
   cross-BP class lookups.** It should convert `BP_ThirdPersonCharacter_C` to the full path
   `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter_C` before calling
   TryFindType. Source: OlivePlanValidator.cpp line 516.

4. **OnActorBeginOverlap error message is misleading.** The error tells the agent to use
   `project.search type=InputAction` which is wrong for a delegate event. It should say:
   "For Actor overlap events, use event name 'ActorBeginOverlap' (without 'On' prefix) or
   add a SphereComponent and use 'OnComponentBeginOverlap' with component_name property."

5. **Socket name for gun attachment is still not being set.** The gun reference template or
   knowledge pack should state explicitly: when attaching a weapon Actor to a character, always
   provide SocketName="hand_r" (or check available sockets via blueprint.describe). This has been
   a gap in three consecutive runs.

6. **The EquipGun call from within BP_Gun is architecturally unusual.** A pickup actor calling
   back into the character to equip itself creates circular dependency risk. The reference template
   for pickup/equip patterns should clarify whether the character or the pickup actor initiates
   the equip logic. Both work, but the current approach (pickup calls EquipGun on the character
   with Self as arg) is clean enough.

7. **plan_json rate of 58% (7/12) is below the target range.** The primary driver is the BP_Gun
   pickup event requiring 4 attempts. This is partly tooling (failed ExpandMissingCollision auto-
   inject) and partly agent planning (hallucinated SetEquippedGun function, failed data wire on
   GunRoot attach). Total time spent on pickup retries: ~36 seconds (23:58:17 → 23:58:54).

8. **The InputKey→Enhanced Input gap persists.** The agent did not search for IA_Shoot or
   IA_Fire assets. The knowledge pack should note that in Enhanced Input projects, LMB for fire
   should use an `event` op with an IA asset, not a legacy InputKey node. If no IA asset exists,
   it should suggest creating one via editor.run_python.

9. **No auto-continues in this run (vs 3 in 03-15a) confirms the architectural improvements to
   the BP_Gun structure.** The cleaner decomposition (separate Fire/ResetCanFire functions, proper
   variables) reduced dead-end retries significantly.

10. **CleanupPreviousPlanNodes is working correctly and silently.** It cleaned 14 total nodes
    across 2 cleanup events with 0 errors. This is a solid quality-of-life fix with no observed
    regressions.
