# Research: Gun+Bullet Creation Session Log Analysis (2026-03-15 13:55)

## Question
Analyze the 13:55 session (backup log 14:20:15) where the AI was asked to create a gun blueprint
that shoots bullets and can be picked up. Focus on collision setup (modify_component,
CollisionProfileName), mesh assignment (StaticMesh), errors, and diagnosis vs. what the AI should
have done.

## Session Overview

**Log file:** Saved/Logs/UE_Olive_AI_Toolkit-backup-2026.03.15-14.20.15.log
**Run start:** 13:55:28
**Run end:** 14:02:39
**Agent time:** ~7:11
**Total tool calls:** 28 (log line 2509: "28 tool calls logged")
**Auto-continues:** 0
**Exit code:** 0

**Blueprints created/modified:**
- /Game/Weapons/BP_Bullet (new, Actor parent)
- /Game/Weapons/BP_Gun (new, Actor parent)
- /Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter (modified, existing)

All three compiled SUCCESS.

---

## Findings

### 1. Full Tool Call Sequence

**Phase 1: Discovery (13:55:28-13:56:10, 9 calls)**

- olive.search_community_blueprints x5 (parallel batch) -- SUCCESS
- olive.get_recipe x2 ("weapon pickup equip attach socket character"; "shoot bullet spawn projectile muzzle") -- SUCCESS
- project.search ("IA_") -- SUCCESS
- project.get_relevant_context ("fire shoot gun weapon") -- SUCCESS

Source: log lines 1803-1863

**Phase 2: Create structure (13:56:31-13:56:56, 11 calls)**

- blueprint.create: BP_Bullet (Actor) -- SUCCESS (line 1867)
- blueprint.create: BP_Gun (Actor) -- SUCCESS (line 1874)
- blueprint.add_component: BP_Bullet/SphereComponent "CollisionSphere" -- SUCCESS (line 1885)
- blueprint.add_component: BP_Bullet/ProjectileMovementComponent "ProjectileMovement" -- SUCCESS (line 1894)
- blueprint.add_component: BP_Gun/StaticMeshComponent "GunMesh" -- SUCCESS (line 1904)
- blueprint.add_component: BP_Gun/SphereComponent "PickupSphere" -- SUCCESS (line 1913)
- blueprint.add_component: BP_Gun/ArrowComponent "MuzzlePoint" (parent: GunMesh) -- SUCCESS (line 1922)
- blueprint.add_variable: BP_Gun/bIsEquipped (bool) -- SUCCESS (line 1931)
- blueprint.add_variable: BP_Gun/OwnerCharacter (Character*) -- SUCCESS (line 1940)
- blueprint.add_function: BP_Gun/Fire (auto-compile) -- SUCCESS (line 1949)
- blueprint.add_variable: BP_ThirdPersonCharacter/EquippedGun (BP_Gun_C*) -- SUCCESS (line 1961)

**Phase 3: modify_component (13:56:55-13:56:56, 2 calls)**

- blueprint.modify_component: BP_Bullet/CollisionSphere -- properties: {SphereRadius:15, bGenerateOverlapEvents:false} -- SUCCESS 2/2 (line 1970)
- blueprint.modify_component: BP_Gun/PickupSphere -- properties: {SphereRadius:100} -- SUCCESS 1/1 (line 1979)

**Phase 4: Plan JSON graphs (13:59:59-14:02:25, 10 calls)**

- apply_plan_json: BP_Bullet/EventGraph -- OnComponentHit -> DestroyActor -- SUCCESS (line 2058)
- apply_plan_json: BP_Gun/EventGraph -- pickup attempt 1 (get_var Mesh on BP_Gun) -- FAILED (line 2083)
- blueprint.add_function: BP_ThirdPersonCharacter/NotifyEquipped custom event -- SUCCESS (line 2094)
- apply_plan_json: BP_Gun/EventGraph -- pickup attempt 2 (NotifyEquipped unresolvable) -- FAILED (line 2121)
- apply_plan_json: BP_ThirdPersonCharacter/EventGraph -- NotifyEquipped -> set EquippedGun -- SUCCESS (line 2170)
- apply_plan_json: BP_Gun/Fire -- GetMuzzle->GetOwner->CastToPawn->GetControlRotation->SpawnActor -- SUCCESS (line 2282)
- apply_plan_json: BP_Gun/EventGraph -- pickup attempt 3 (cast to BP_ThirdPersonCharacter) -- SUCCESS (line 2415)
- blueprint.add_node: BP_ThirdPersonCharacter/EventGraph -- InputKey(LeftMouseButton) -- SUCCESS (line 2423)
- apply_plan_json: BP_ThirdPersonCharacter/EventGraph -- GetEquippedGun->IsValid->Fire -- SUCCESS (line 2484)
- blueprint.connect_pins: node_1.Pressed -> node_3.exec -- SUCCESS (line 2495)
- blueprint.compile: BP_ThirdPersonCharacter -- SUCCESS (line 2505)

---

### 2. Collision Setup -- What the AI Did

CollisionProfileName was NOT attempted in this session at all. Zero calls used it.

**What the AI actually did:**

BP_Bullet/CollisionSphere (log line 1966):
```
properties: {"SphereRadius": 15, "bGenerateOverlapEvents": false}
result: SUCCESS 2/2 properties set
```

BP_Gun/PickupSphere (log line 1975):
```
properties: {"SphereRadius": 100}
result: SUCCESS 1/1 properties set
```

**Why this is broken at runtime:**

A fresh SphereComponent on an Actor-based Blueprint defaults to CollisionEnabled = NoCollision.
Without CollisionProfileName being set:
- BP_Gun's PickupSphere does not generate any overlaps -- OnComponentBeginOverlap never fires -- pickup is broken.
- BP_Bullet's CollisionSphere does not hit anything -- OnComponentHit never fires -- destruction is broken.

Additionally, the AI set bGenerateOverlapEvents: false on CollisionSphere, which is the wrong field
for the OnComponentHit path. OnComponentHit requires bSimulationGeneratesHitEvents=true plus a
blocking collision profile. bGenerateOverlapEvents is for the OnComponentBeginOverlap path. Setting
it false is harmless but reveals that the AI conflates the two event types.

**What the AI should have done:**

For BP_Bullet/CollisionSphere:
```
{CollisionProfileName: "BlockAllDynamic", bSimulationGeneratesHitEvents: true, SphereRadius: 15}
```

For BP_Gun/PickupSphere:
```
{CollisionProfileName: "OverlapAllDynamic", SphereRadius: 100}
```

**Open question:** A different session (same date, different run) showed modify_component failing
0/1 and 1/2 when CollisionProfileName was attempted on SphereComponent and StaticMeshComponent.
This session did not attempt it, so that failure mode is unverified here. The question of whether
modify_component actually supports CollisionProfileName needs a dedicated test.

---

### 3. Mesh Setup -- What the AI Did

- blueprint.add_component: BP_Gun/StaticMeshComponent "GunMesh" -- SUCCESS (line 1904)
- No follow-up to assign a mesh asset: no modify_component for StaticMesh, no set_pin_default,
  no editor.run_python.
- BP_Bullet: no mesh component added at all (SphereComponent + ProjectileMovementComponent only).

**Consequence:** GunMesh exists in the SCS hierarchy but is empty. The gun is invisible at runtime.

**What the AI should have done:**

StaticMesh is an object-reference property. modify_component may not support it reliably.
The correct approach is editor.run_python:
```python
import unreal
component = ...  # get the GunMesh SCS node
component.set_editor_property('static_mesh', unreal.load_asset('/Engine/BasicShapes/Cube'))
```

The AI could also have noted the gap, flagged it to the user, or searched for available SM_ assets
via project.search before giving up.

---

### 4. Key Failures and Errors

**Failure A: get_var "Mesh" not found on BP_Gun (attempts 1 and 2)**

Log lines: 2075, 2110, 2299
Resolver warning: "Variable 'Mesh' not found on Blueprint 'BP_Gun' or parents or generated class"

The pickup EventGraph plan included a step: get_var "Mesh" to get the character's SkeletalMesh for
the attach socket. The step was not correctly scoped to the character context initially.

The resolver warned but did not block. Phase 0 VARIABLE_NOT_FOUND did NOT fire for this step. The
plan was not rejected on this basis -- it failed for a different reason (Failure B) on attempt 2.
By attempt 3, the agent dropped the attach step entirely, simplifying the pickup logic.

Note: In attempt 3 (log lines 2345-2396), the same step DID resolve correctly. The step had
target pointing to the cast_char output (BP_ThirdPersonCharacter_C), and the wiring succeeded:
"Data wire OK: @cast_char.auto -> step 'get_mesh'.self (explicit Target)" at line 2396.
The warning at 2299 fired before the resolver applied the cast target to the step context.

**Failure B: NotifyEquipped unresolvable as cross-BP function (attempt 2)**

Log lines: 2117-2119
Error: "ResolveCallOp FAILED: function 'NotifyEquipped' could not be resolved (target_class='')"

Root cause: The plan cast target was "Character" (engine base class). NotifyEquipped is a custom
event on BP_ThirdPersonCharacter_C, which is not visible by searching Character or its parent chain.

Fix (attempt 3): Cast target changed from "Character" to "BP_ThirdPersonCharacter". The resolver
then inferred target_class via CastTargetMap (log line 2307):
"target_class='BP_ThirdPersonCharacter_C' (via cast target 'BP_ThirdPersonCharacter' from step 'cast_char')"

Time cost: ~57 seconds (13:59:59 -> 14:01:08 -> 14:01:41).

**Warning: TryFindType short class name in validator (line 2310)**

"LogClass: Warning: Short type name 'BP_ThirdPersonCharacter_C' provided for TryFindType.
Please convert it to a path name (suggested: '/Game/.../BP_ThirdPersonCharacter.BP_ThirdPersonCharacter_C')"
Source: FOlivePlanValidator::CheckVariableExists() at OlivePlanValidator.cpp:516

Validation still passed (line 2322: "Phase 0 passed: 0 errors, 0 warnings"). This is a latent bug
in the validator's class resolution: it should use the full path for TryFindType, not the short name.

**Warning: SpawnActor class resolution noise (lines 2239-2245)**

7x "Failed to find object 'Class /Script/XXX./Game/Weapons/BP_Bullet'" warnings. Benign -- the
resolver searches multiple script modules before falling through to LoadObject (which succeeds).
The SpawnActor node was created successfully at line 2247. This noise occurs because BP_Bullet
was not yet saved to disk when the Fire graph was being built.

---

### 5. Structural Observations

**What the AI got right:**
- Thorough discovery (5 community searches + 2 recipe reads + input action search)
- EquippedGun typed as BP_Gun_C* immediately (no Actor*->BP_Gun cast needed; improvement over prior sessions)
- Self-corrected NotifyEquipped on attempt 3 (changed cast target from Character to BP_ThirdPersonCharacter)
- Fire function architecture: GetMuzzle -> GetOwner -> CastToPawn -> GetControlRotation -> SpawnActor(BP_Bullet)
- All 3 BPs compile SUCCESS

**What the AI missed:**
- CollisionProfileName on both SphereComponents (system is functionally broken at runtime)
- StaticMesh asset assignment for GunMesh (gun is invisible)
- bSimulationGeneratesHitEvents on CollisionSphere (hit events won't fire even with a profile)
- Socket name for K2_AttachToComponent (gun attaches to character mesh root, not hand_r)
- Legacy InputKey node used instead of Enhanced Input

---

### 6. Session Metrics

| Metric | Value |
|--------|-------|
| Agent time | ~7:11 |
| Total tool calls | 28 |
| Auto-continues | 0 |
| Exit code | 0 |
| plan_json calls | 7 |
| plan_json successes | 5 (71.4%) |
| plan_json failures | 2 |
| modify_component calls | 2 |
| modify_component success rate | 100% |
| CollisionProfileName attempts | 0 |
| StaticMesh assignment attempts | 0 |
| BP compile SUCCESS | 3/3 |

Note: tool success rate is 100% but functional success rate at runtime is approximately 30%
(mesh is empty, collision is broken, socket is wrong, input is legacy).

---

## Recommendations

1. **CollisionProfileName is a knowledge gap, not a tool failure.** The AI simply omitted it.
   Add to cli_blueprint.txt: "After add_component SphereComponent, ALWAYS set CollisionProfileName.
   A fresh SphereComponent has CollisionEnabled=NoCollision by default and will fire no events.
   Use CollisionProfileName='OverlapAllDynamic' for trigger spheres (OnComponentBeginOverlap),
   CollisionProfileName='BlockAllDynamic' for physics collision (OnComponentHit)."

2. **Verify modify_component supports CollisionProfileName on SphereComponent.** A separate session
   showed failures on this. Run a targeted test: create BP with SphereComponent, call modify_component
   with CollisionProfileName='OverlapAllDynamic', then blueprint.read to confirm the value was stored.
   If it fails, document the fallback as editor.run_python.

3. **StaticMesh assignment needs a dedicated fallback path.** Add to knowledge: "To assign a StaticMesh
   asset to a StaticMeshComponent, use editor.run_python. modify_component does not handle object-reference
   properties. Example: component.set_editor_property('static_mesh', unreal.load_asset('/Engine/BasicShapes/Cube'))"

4. **Phase 0 VARIABLE_NOT_FOUND does not fire for external-target get_var steps.** When get_var has a
   target pointing to a cast output (not self), the validator skips existence checking. The resolver warns
   but does not block. Consider extending Phase 0 to resolve external-target steps using CastTargetMap
   and then validate the variable on that resolved class.

5. **OlivePlanValidator.cpp:516 calls TryFindType with short class name.** Fix to use full asset path.
   The correct format is '/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter_C'.

6. **Cross-BP call: always cast to specific Blueprint class, not engine parent.** Knowledge note:
   "When calling a custom event or function defined on a specific Blueprint (not on the engine base class),
   cast to the specific Blueprint class name (e.g., BP_ThirdPersonCharacter), not the engine parent
   (Character). The resolver infers target_class from the cast target via CastTargetMap."

7. **Hit vs Overlap events require different flags and profiles.** Knowledge to add:
   "OnComponentHit: requires CollisionProfileName that blocks (e.g., BlockAllDynamic) AND
   bSimulationGeneratesHitEvents=true. OnComponentBeginOverlap: requires CollisionProfileName that
   overlaps (e.g., OverlapAllDynamic) AND bGenerateOverlapEvents=true. These are completely separate paths."

8. **K2_AttachToComponent InSocketName must be set.** When attaching a weapon to a character mesh,
   InSocketName must be set (e.g., 'hand_r'). Without it the actor attaches to the component root.

9. **Legacy InputKey: recurring issue across sessions.** The agent searched for IA_ assets (found none)
   and fell back to InputKey. Knowledge update: "If no Enhanced Input action exists for the needed input,
   create one via editor.run_python using unreal.AssetToolsHelpers. Do NOT fall back to InputKey nodes."

10. **High tool success rate masks low runtime success rate.** 28/28 tool calls succeeded (100%), but
    the resulting system does not work at runtime (no collision events, invisible gun, wrong attachment).
    This pattern -- compile success with runtime failure -- is caused by missing property setup that the
    tools accept silently (SphereRadius is accepted, but CollisionProfileName is never attempted).
    Consider adding a post-build checklist prompt: after creating a pickup/weapon system, verify that
    all SphereComponents have non-NoCollision profiles and all mesh components have assets assigned.
