# Research: Pickup/Equip Pattern Analysis Across Templates and Knowledge Packs

## Question
Comprehensive analysis of all pickup, equip, attach, weapon, inventory, and item interaction patterns across factory templates, reference templates, library templates, and knowledge packs — to inform writing a new `pickup_equip_patterns.json` reference template.

---

## Findings

### 1. Factory Templates

#### `gun.json` — `usage_notes` field
Source: `Content/Templates/factory/gun.json`

This is the most complete pickup/equip description in the entire template system. It lives in `usage_notes` (not in a named pattern), so it is only visible when the agent specifically calls `get_template("gun")`. Content:

1. Cast overlapping actor to Character
2. `get_var Mesh` on the character (with `Target: @cast.auto`)
3. Call `K2_AttachToComponent` with `Parent=@get_mesh.auto`, `SocketName='hand_r'` (or `'weapon_r'`), all rules=`SnapToTarget`
4. `SetCollisionEnabled(NoCollision)` on the GunMesh (explicitly clarifies: NOT the PickupSphere — the mesh is what collides, the sphere is just the overlap trigger)
5. Optionally disable PickupSphere overlap

**What it teaches:** Correct attachment target (character mesh), correct socket names, correct collision component to disable.

**What is missing:**
- Does NOT specify which event to use for the overlap (ReceiveActorBeginOverlap vs OnComponentBeginOverlap). The session log analysis (run 09, 13 March) showed the agent used the wrong event (ActorBeginOverlap on the character instead of OnComponentBeginOverlap on the pickup's sphere) — this gap in the template directly caused that failure.
- Does NOT say which actor owns the overlap event (the item or the character).
- Does NOT mention `bIsEquipped` state flag or character-side `EquippedWeapon` reference variable.
- Does NOT cover drop/un-equip.
- Does NOT cover the case where the character already has a weapon equipped (swap logic).

---

#### `interactable_door.json`, `interactable_gate.json`, `stat_component.json`, `projectile.json`
Source: `Content/Templates/factory/`

None of these have any pickup, equip, or attachment content. Irrelevant to this analysis.

---

### 2. Reference Templates

#### `interactable_patterns.json`
Source: `Content/Templates/reference/interactable_patterns.json`

Covers the `Interact()` interface pattern, Timeline interpolation, StateToggle, PivotAndSlider, and InterfaceIntegration. No attachment, no equip, no pickup sphere pattern. The `InterfaceIntegration` and `EventBasedInterfaces` patterns are highly relevant for pickup items that implement BPI_Interactable, but no connection is made to the pickup-specific case.

**Relevant but incomplete:** Teaches the "Interact() with no outputs" design, which is the correct approach for a pickup item that calls character methods. Does not teach what happens *inside* the interact implementation for weapon pickup.

---

#### `interaction_caller.json`
Source: `Content/Templates/reference/interaction_caller.json`

Has the `OverlapDetection` pattern — SphereComponent on the **character** detecting interactable actors entering range, storing in `NearestInteractable`. Also `ValidityCheckAndCall` and `FullCallerArchitecture`.

**Critical observation:** This template describes the interaction pattern from the **character's perspective** — the character has an overlap sphere and calls the interface. This is the "press E to interact" model, NOT the "walk into pickup and auto-collect" model. These are two distinct patterns:

- **Interaction model:** Character has overlap sphere → presses input → calls interface on item
- **Auto-pickup model:** Item has overlap sphere → character walks in → item collects itself / calls character function

The system currently teaches only the interaction model in `interaction_caller.json`. The auto-pickup model (more common for weapons/ammo/health) is described nowhere as a standalone pattern.

---

#### `component_patterns.json`
Source: `Content/Templates/reference/component_patterns.json`

Teaches component event signatures (`OnComponentBeginOverlap`, `OnComponentEndOverlap`, `OnComponentHit`). Notes that `Generate Overlap Events` must be true. Does NOT connect this to pickup or equip use cases.

---

#### `ue_events.json`
Source: `Content/Templates/reference/ue_events.json`

Lists `ActorBeginOverlap` and `ActorEndOverlap` as available events. Does NOT describe `OnComponentBeginOverlap`. Does NOT explain when to use one versus the other.

**This is a meaningful gap:** The template that specifically documents UE events lists the actor-level event but not the component-level event. The agent learning from this template will prefer `ActorBeginOverlap` — which is incorrect for pickup spheres. This directly corresponds to the failure in run 09 (gun session, March 13).

---

#### `event_dispatcher_patterns.json`
Source: `Content/Templates/reference/event_dispatcher_patterns.json`

Has a critical pattern `ComponentDelegate_vs_Dispatcher` that explicitly distinguishes component delegate events from user dispatchers. Correctly states that `OnComponentBeginOverlap` uses `{"op":"event","target":"OnComponentBeginOverlap","properties":{"component_name":"..."}}` — NOT `bind_dispatcher`.

**Positive finding:** This is correct and useful. However, it is framed as "don't use bind_dispatcher for this" rather than "use OnComponentBeginOverlap for pickup spheres." An agent building a pickup from scratch would need to reason across multiple templates to arrive at the correct conclusion.

---

#### `projectile_patterns.json`
Source: `Content/Templates/reference/projectile_patterns.json`

No pickup/equip content. Has `IgnoreSpawnerCollision` which is relevant for spawn-and-attach weapon patterns but not for static-pickup patterns.

---

### 3. Knowledge Pack: `cli_blueprint.txt`

Source: `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

Contains **two relevant entries**:

**Entry 1 — Equip/attach example (in the "Accessing properties on external actors" section, lines 131-133):**
```
Equip/attach example: get_var Mesh → K2_AttachToComponent with Parent=@get_mesh.auto, SocketName='hand_r'
```
This is a correct code-level example. But it is buried inside a general section about external actor property access, not labeled as a pickup pattern.

**Entry 2 — Common Mistakes section (lines 187-189):**
```
WRONG: Disabling collision on the pickup trigger (SphereComponent) when equipping a weapon
RIGHT: Disable collision on the MESH component (StaticMeshComponent)
WHY: Pickup spheres are overlap triggers — they don't block movement.
```
This is the best collision guidance in the entire system. It is correct, specific, and explained.

**What is missing from the knowledge pack:**
- No guidance on which actor should own the pickup overlap event.
- No guidance on `ReceiveActorBeginOverlap` vs `OnComponentBeginOverlap` — the session log (run 09) showed the agent used `ActorBeginOverlap` on the character instead of the item's component event.
- No guidance on the `bIsEquipped` state guard.
- No guidance on cross-BP reference storage (character stores `EquippedWeapon` ref, item stores `bIsEquipped`).

---

### 4. Knowledge Pack: `blueprint_design_patterns.txt`

Source: `Content/SystemPrompts/Knowledge/blueprint_design_patterns.txt`

Contains several relevant sections:

**Section 0 — Decomposition:**
- "Weapons are ALWAYS separate actors attached to characters, never just variables/functions on the character" — correct and firm.
- "Gun System is typically THREE assets (Gun actor + Bullet projectile + character input)" — correct decomposition.

**Section 3 — Overlap Events:**
- Correctly shows `OnComponentBeginOverlap` with `component_name` property for persistent detection.
- Calls out: "Use OnComponentBeginOverlap/EndOverlap to track who's nearby. Store the reference."
- Shows the plan_json pattern for BeginOverlap storing a `NearbyCharacter` variable.
- The example is from the **item's perspective** (item stores a `NearbyCharacter` reference from its sphere) — this is correct for the auto-pickup case.

**Section 4 — Cast:**
```
"Overlap gives you OtherActor and you need DATA from a specific class (not a shared behavior)
e.g., read weapon stats from a weapon pickup, check if the overlapping actor has a specific variable"
```
Correctly shows the cast-after-overlap pattern for weapon pickups.

**Section 6 — Complete Interaction System Example:**
Says "Pickups (collectibles, ammo, power-ups) follow the same pattern — they ARE interactables that call a behavior on the overlapping actor (AddToInventory, AddAmmo, GiveWeapon)." This correctly frames pickups as a subtype of interactables. However, the implementation steps (1-10) show the press-E model, not the auto-pickup model. The pickup use case is mentioned but the implementation diverges.

---

### 5. Library Templates: `fps_shooter` Project

#### `fps_shooter_bp_inventory_item_base` (the item base class)
Source: `Content/Templates/library/fps_shooter/fps_shooter_bp_inventory_item_base.json`

**Component structure:** DefaultSceneRoot → Mesh (StaticMeshComponent) → Sphere (SphereComponent)

**Overlap events:** Uses `BndEvt__Sphere1_K2Node_ComponentBoundEvent_0` (= `OnComponentBeginOverlap` bound to "Sphere") and `BndEvt__Sphere1_K2Node_ComponentBoundEvent_1` (= `OnComponentEndOverlap`). Both events are on the **item itself**, not the character.

**On BeginOverlap:** Stores the character reference in `PlayerCast` (a `BP_CharacterBase_C` variable). Calls `SetCollisionResponseToChannel` on the Sphere component to block/ignore visibility. Calls `SetRenderCustomDepth` (highlight effect).

**On Collect (StoreThisItem function):** Calls `SetActorHiddenInGame(true)` → `SetCollisionEnabled(NoCollision)` on the Sphere → final inventory handling. The Sphere gets NoCollision on collect.

**Interface:** Implements `BPI_Interact` (`BPI_Interaction` event) and `BPI_ItemInterface` (`BPI_UseItem`). The `BPI_Interaction` event receives a `CharacterReference` parameter (the character is passed through the interface), which is stored to `PlayerCast`. No cast needed.

**Key pattern findings:**
- Overlap event is `OnComponentBeginOverlap` on a **SphereComponent**, bound to the **item actor's** EventGraph. This is the correct pattern.
- Collision is disabled on the **Sphere** (overlap trigger) on collect, NOT on the Mesh. This is different from the gun.json recommendation which disables the Mesh.
- The item hides itself and disables its sphere — it does NOT destroy itself immediately (supports respawn via `ClassCanRespawn`).
- The `CharacterReference` comes through the BPI_Interaction interface call (pressed E model), not from the overlap event's `OtherActor`. This is the press-E interactable model.

**Conflict with gun.json:** gun.json says disable mesh collision; fps_shooter says disable sphere collision. Both are valid for different reasons:
- For a **pickup that stays attached** (weapon): disable mesh collision (sphere is irrelevant once attached)
- For a **pickup that disappears** after collect: disable sphere collision to prevent re-triggering while hidden

---

#### `fps_shooter_bp_ammo_pick_up_base` (ammo pickup)
Source: `Content/Templates/library/fps_shooter/fps_shooter_bp_ammo_pick_up_base.json`

**Overlap event:** Uses `ReceiveActorBeginOverlap` (actor-level event, NOT component-level). This is the **wrong** approach per best practices — it fires for any collision component on the actor, not just the dedicated pickup sphere. However, it immediately casts `OtherActor` to `BP_CharacterBase` and proceeds only on success.

**Key variables read from character:** Gets `EquippedWeapon` from the character (typed as `BP_WeaponBase_C`). Checks the weapon type to determine whether to give ammo to the right weapon.

**Findings:** This real-world template uses `ReceiveActorBeginOverlap` — confirming that the agent's failure in session log run 09 was partly reinforced by real examples in the library. The ammo pickup also does NOT disable anything on the pickup after collection — it calls `K2_DestroyActor` directly.

---

#### `fps_shooter_bp_weapon_base` (weapon actor)
Source: `Content/Templates/library/fps_shooter/fps_shooter_bp_weapon_base.json`

**Variables relevant to pickup/equip:**
- `WeaponSocketName` (Name) — the socket on the character's skeletal mesh for FPP
- `WeaponSocketName_TPS` (Name) — for TPS
- `LeftHandIK_SocketName` (Name) — IK socket
- `CanPickUpDuplicates` (bool)
- `PlayerCast` (BP_CharacterBase_C reference)
- `EquippedSlotLimit` (int)
- FPP/TPP montage refs, FiringSound, MuzzleEffect

**Functions:** Has `AddAndEquip` (custom event) and `EquipWeapon` (called on the character — `owning_class: SKEL_BP_CharacterBase_C`). Has `AttachToComponent` calls with `SocketName` coming from the `WeaponSocketName` variable. Uses `K2_AttachToComponent` on `SceneComponent`.

**Pattern:** The weapon calls `EquipWeapon` on the character — the weapon is responsible for triggering its own equip. The character has an `EquipWeapon` function that handles the attachment. The weapon stores a socket name variable that is passed to the attach call.

**What can be inferred:**
- Socket names are stored as variables on the weapon, not hardcoded in the overlap event.
- The weapon attaches to the character's skeletal mesh using a stored socket name.
- This is a more decoupled design than the gun.json approach.

---

### 6. Library Templates: `action_rpg` Project

#### `action_rpg_bp_rpg_item_pickup_base`
Source: `Content/Templates/library/action_rpg/action_rpg_bp_rpg_item_pickup_base.json`

**Component structure:** ItemMesh (StaticMeshComponent) + CollectCollision (SphereComponent, radius=150) + PointLight + Widget (WidgetComponent for world-space pickup UI) + Capsule (CapsuleComponent)

**Overlap event:** `BndEvt__CollectCollision_K2Node_ComponentBoundEvent_0` — explicitly bound to `CollectCollision` (the SphereComponent). This is `OnComponentBeginOverlap`, correct approach.

**On BeginOverlap:** Stores `PlayerCollecting` variable → calls `AnimateCollection` (visual collection animation) → calls `GiveItem` (gives the item to the character).

**Variables:** `PlayerCollecting` (Actor, not typed to character class), `ItemType` (RPGItem), `Count` (int).

**Pattern observations:**
- Uses a dedicated "CollectCollision" name for the pickup sphere (better naming than generic "Sphere").
- Stores the collecting actor as generic `Actor` type — casts happen inside `GiveItem`.
- Has a WidgetComponent for world-space UI ("Press E to pick up").
- No `bIsEquipped` flag — this is a consumable pickup, not an equippable weapon.

---

### 7. Library Templates: `combatfs` Project

#### `combatfs_bp_weapon_parent`
Source: `Content/Templates/library/combatfs/combatfs_bp_weapon_parent.json`

This is the most sophisticated equip implementation found. It implements `I_WeaponInterface` which has:
- `Attach Weapon to Hand` — attaches to main hand socket
- `Attach Weapon to Back` — attaches to back/unequipped socket
- `Attach Weapon - Swap Hand` — swaps between main and off-hand
- `AttachWeaponToUniqueBone` — custom bone attachment

**Key pattern:** Uses `S_EquipmentInfo` struct (from `EquipmentComponent`) that contains:
- `EquippedMainHandSocket` (Name)
- `EquippedOffHandSocket` (Name)
- `UnEquippedMainHandSocket` (Name)
- `UnEquippedOffHandSocket` (Name)

All `K2_AttachToComponent` calls take `SocketName` from the struct fields. This decouples socket names from the weapon logic entirely — they come from an equipment data struct.

**SetCollisionEnabled** is called with `NoCollision` on the weapon's primitive component during the attach sequence.

**Pattern in "Attach Weapon to Hand" function:** `K2_AttachToComponent` (SceneComponent owning class) → SocketName from struct → `SetCollisionEnabled(NoCollision)` on the weapon's mesh.

**Observations:**
- Uses `GetAllSocketNames` during setup to validate socket existence.
- `GetOverlappingActors` and `IsOverlappingActor` are used for proximity queries — NOT for the pickup event (which is in the parent `bp_pickup_item`).
- The combatfs approach is component-driven: `EquipmentComponent` holds equipment state, `InventoryComponent` holds inventory.

#### `combatfs_inventory_component`
Source: `Content/Templates/library/combatfs/combatfs_inventory_component.json`

Uses `InventoryData` as `TArray<S_SlotInfo>` — slot-based inventory. 55 functions. Handles `WB_PlayerMenu` widget reference. Replicated. This is too complex for a general-purpose reference template but confirms the pattern: inventory is a component, not a character variable.

---

## Gaps and Contradictions Between Sources

### Gap 1: Overlap event type — no authoritative guidance

The system teaches three conflicting approaches and never explains when to use each:

| Source | Event Used | Owner |
|--------|-----------|-------|
| `ue_events.json` | `ActorBeginOverlap` (only one documented) | Actor |
| `fps_shooter_bp_ammo_pick_up_base` (library) | `ReceiveActorBeginOverlap` | Item actor |
| `fps_shooter_bp_inventory_item_base` (library) | `OnComponentBeginOverlap` on Sphere | Item actor |
| `action_rpg_bp_rpg_item_pickup_base` (library) | `OnComponentBeginOverlap` on CollectCollision | Item actor |
| `interaction_caller.json` (reference) | `OnComponentBeginOverlap` on character's sphere | Character |
| `blueprint_design_patterns.txt` (knowledge) | `OnComponentBeginOverlap` | Implied item |

**Best practice:** `OnComponentBeginOverlap` on a dedicated SphereComponent on the **item actor**. This gives the `OverlappedComponent` parameter (identifying which sphere fired), fires only for that sphere's radius, and keeps pickup logic self-contained on the item.

### Gap 2: Which actor owns the overlap — never stated explicitly

All sources either imply this or mix models. No source says: "The overlap sphere belongs to the item, not the character. The item actor is responsible for detecting when a character enters its range and handling the pickup."

### Gap 3: Collision component to disable after equip — contradictory guidance

| Source | What Gets Disabled |
|--------|-------------------|
| `gun.json` usage_notes | GunMesh (StaticMeshComponent) |
| `cli_blueprint.txt` Common Mistakes | Mesh component (StaticMeshComponent) |
| `fps_shooter_bp_inventory_item_base` | Sphere (SphereComponent), NoCollision |
| `combatfs_bp_weapon_parent` | Primitive component (the weapon mesh) |

**Correct answer (context-dependent):**
- For weapons that **stay in the world** (attach to character): disable mesh collision (prevents clipping with character body) AND disable sphere overlap (prevents re-triggering).
- For consumable pickups that **disappear** on collect: disable sphere, then hide/destroy. No mesh collision concern.

The new template must state both cases.

### Gap 4: bIsEquipped state flag — mentioned nowhere

No template or knowledge file documents the pattern of maintaining a `bIsEquipped` boolean on the item actor. This flag:
- Guards against duplicate pickups (second character walks into the already-equipped item's lingering overlap sphere)
- Enables drop/swap logic (check bIsEquipped before re-equipping)
- Is used in real implementations

### Gap 5: Character-side equipped item reference — partially documented

`fps_shooter_bp_ammo_pick_up_base` reads `EquippedWeapon` from the character. The gun.json usage_notes doesn't mention it. No reference template explains: "The character stores an `EquippedWeapon` variable (typed to the weapon BP or as Actor) so other systems (ammo pickups, UI) can access the currently held weapon."

### Gap 6: Auto-pickup vs press-E pickup — never distinguished

The system mixes two architecturally different patterns without labeling them:

**Auto-pickup (walk into it):** Item overlap sphere → cast OtherActor → call equip/collect → destroy or disable item. No interface needed.

**Interaction pickup (press E):** Character overlap sphere → store `NearestInteractable` → player presses input → character calls `BPI_Interact::Interact()` on item → item handles collect logic.

`interaction_caller.json` describes Pattern B. The library examples (fps_shooter_bp_ammo_pick_up_base) implement Pattern A with a twist — also supporting Pattern B via BPI_Interaction. The new template must distinguish these.

### Gap 7: Socket name knowledge — partially documented

`gun.json` mentions `hand_r` and `weapon_r`. The knowledge pack's attach example hardcodes `hand_r`. No template documents other common UE5 character socket names (`hand_l`, `spine_01`, `neck_01`, `foot_r` for grounding). No template explains how to discover valid socket names (blueprint.read → check components → look for SkeletalMeshComponent socket list, or use GetAllSocketNames at runtime).

### Gap 8: Rules parameters for K2_AttachToComponent — only in the knowledge pack

The knowledge pack's equip example shows `LocationRule`, `RotationRule`, `ScaleRule` = `SnapToTarget`. No reference template explains these EAttachmentRule values. `KeepRelativeOffset` vs `SnapToTarget` vs `KeepWorldTransform` is a meaningful choice and the wrong one produces visually broken attachments.

---

## Patterns Missing Entirely

1. **Drop/un-equip pattern** — no source covers dropping a weapon. Collision re-enable, detach, restore physics, clear character reference.
2. **Weapon swap pattern** — equipping when already holding a weapon. Check `EquippedWeapon != null` → drop current → attach new.
3. **Loot-style auto-collect without destroy** — item stays in world but `bIsEquipped = true` prevents re-pickup. Used for persistent world items.
4. **Multi-slot inventory** — all examples assume single equipped weapon. The combatfs `InventoryComponent` handles slots but it is in a 55-function component too complex to summarize.
5. **Socket name discovery pattern** — how to find valid attachment points. `GetAllSocketNames` or `blueprint.read` to inspect the character mesh.
6. **Physics restore on drop** — `SetSimulatePhysics(true)` + `SetCollisionEnabled(QueryAndPhysics)` when dropping.
7. **Spawn-then-equip** — weapon spawned by the character (not picked up from world). Pattern: `SpawnActor` → `K2_AttachToComponent` with `Target=@spawn.auto`. Different from overlap-based pickup.

---

## Recommendations

1. **The new `pickup_equip_patterns.json` must cover two distinct pickup architectures** as separate named patterns: `AutoPickup` (walk-into-sphere on item) and `InteractionPickup` (character sphere + press E + BPI). The current system conflates them.

2. **Pattern 1 (AutoPickup) must state explicitly:** The SphereComponent (`PickupSphere`) lives on the **item actor**, not the character. The event is `OnComponentBeginOverlap` with `component_name: "PickupSphere"`. The item detects the character, not vice versa.

3. **The overlap event guidance in `ue_events.json` needs updating.** It currently lists only `ActorBeginOverlap` — the stronger guidance for pickup spheres (`OnComponentBeginOverlap`) should be added, or at minimum `ue_events.json` should cross-reference `component_patterns.json`.

4. **The collision disable guidance is correct in `cli_blueprint.txt` but needs to be in the new template as a named pattern** — not buried in a "Common Mistakes" section. The distinction (mesh vs sphere) should appear in the architecture description with the WHY.

5. **The new template must document the `bIsEquipped` state guard.** Without it, agents consistently omit this flag and the weapon can be re-picked up or the inventory can double-add.

6. **The `EquippedWeapon` character variable must be documented.** Pickup items routinely need to check or set it (ammo pickups read it to find the right weapon; the character needs it for drop logic). The fps_shooter library confirms this is universal.

7. **K2_AttachToComponent rules parameters should be a named pattern.** `SnapToTarget` is correct for weapons. `KeepRelativeOffset` is correct for IK targets. The new template should show the correct rules for a weapon pickup without requiring the agent to guess.

8. **Add a spawn-then-equip pattern** (weapon spawned by character code, not from world pickup). This is the other half of the weapon lifecycle (character starts with a weapon, or gets one from a loadout screen). Pattern: `SpawnActor → "Target": "@spawn.auto" → K2_AttachToComponent`.

9. **The gun.json `usage_notes` content should be migrated into the new reference template** as named patterns. Currently this guidance is only visible when `get_template("gun")` is called, which only happens when the agent already knows about the gun template. The guidance belongs in a standalone pickup/equip template visible through `list_templates(query="pickup equip attach weapon")`.

10. **The new template should be 60-120 lines targeting the reference template spec.** Patterns to include: `PickupActorArchitecture`, `AutoPickupEvent`, `InteractionPickupComparison`, `AttachToSocket`, `CollisionHandling`, `StateVariables`, `WeaponSwapGuard`. No embedded plan_json — descriptive only.
