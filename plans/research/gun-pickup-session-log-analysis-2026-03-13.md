# Research: Gun/Pickup Session Log Analysis — 2026-03-13

## Question
Diagnose all issues from the autonomous AI agent run on 2026-03-13 (log: `docs/logs/UE_Olive_AI_Toolkit.log`, 3308 lines). The user reported: wrong attachment (actor-to-component instead of component-to-component), unused event dispatchers, nodes that cannot be dragged, pickup not using existing interactable logic, and discovery needing improvement.

---

## Run Overview

- **Session start:** 04:23:04, **end:** 04:34:27 (plus user-side inspection to ~04:41)
- **Task:** "Create a gun BP_Gun that @BP_ThirdPersonCharacter can pickup and equip. It should..."
- **Auto-continue triggered:** YES — first agent attempt timed out at 240s (04:28:08). Second instance relaunched immediately (no gap).
- **Total tool calls:** 28 (logged by CLIProvider)
- **All BPs compiled SUCCESS:** BP_Bullet, BP_Gun, BP_ThirdPersonCharacter — zero compile errors across all runs
- **Assets created:** BP_Bullet (`projectile` template + `Bullet` preset), BP_Gun (`gun` template)
- **Final run outcome:** Exit code 0

### Timeline Summary

| Time | Action |
|------|--------|
| 04:23:04 | Run 1 launched. Pre-built snapshot taken. 2 assets injected as context. |
| 04:23:04–04:23:15 | Discovery phase: 5× `olive.search_community_blueprints`, 2× `olive.get_recipe` |
| 04:23:34–04:24:09 | Template research: `blueprint.list_templates`, 3× `blueprint.get_template` (gun, projectile, projectile_patterns) |
| 04:24:07–04:24:09 | BP_Bullet created from `projectile` template (Bullet preset). BP_Gun created from `gun` template. Both compile SUCCESS. |
| 04:28:08 | **240s idle timeout fired** — no tool call for 4 minutes. Run 1 killed, auto-continue triggered (attempt 1/2). |
| 04:28:08 | Run 2 relaunched with decomposition nudge. Same tool filter (6 prefixes, 53/86 tools). |
| 04:28:40–04:29:48 | Second agent applies plan_json to BP_Gun (UserConstructionScript, ResetCanFire, Fire graph already existed). BP_Bullet gets SetLifeSpan + OnComponentBeginOverlap logic. |
| 04:30:16–04:30:31 | Reads BP_ThirdPersonCharacter summary and components — **gets 0 components back (load warning)**. |
| 04:30:46–04:30:47 | Adds `EquippedGun` variable and `PickupSphere` component to BP_ThirdPersonCharacter. |
| 04:32:34 | Applies pickup overlap plan_json to BP_ThirdPersonCharacter EventGraph (7-step: cast, is_valid, set_var, attach). |
| 04:33:05–04:33:21 | Adds HandleFire + HandleReload functions, populates them via plan_json. |
| 04:33:34–04:33:52 | Adds InputKey nodes (LMB, R) and tries to wire call-function nodes — 4 FAILURES before finding K2Node_CallFunction workaround. |
| 04:33:58–04:34:06 | Connects pins. Compiles all 3 BPs — all SUCCESS. Run 2 exits cleanly. |

---

## Findings

### Issue 1: Wrong Attachment — Actor.AttachToComponent Used Instead of GunMesh.AttachToComponent

**Log lines (line 2543–2624):**
```
params: {"asset_path":".../BP_ThirdPersonCharacter","graph_target":"EventGraph",
  "plan_json":{"steps":[
    {"step_id":"overlap", "op":"event", ...},
    {"step_id":"cast", "op":"cast", "target":"BP_Gun", ...},
    {"step_id":"get_gun", "op":"get_var", "target":"EquippedGun"},
    {"step_id":"check_gun", "op":"is_valid", ...},
    {"step_id":"set_gun", "op":"set_var", "target":"EquippedGun"},
    {"step_id":"get_mesh", "op":"call", "target":"GetMesh"},   ← FAILED silently
    {"step_id":"attach", "op":"call", "target":"AttachToComponent",
      "inputs":{"Target":"@cast.auto"}}                        ← self pin = BP_Gun cast result
  ]}}
```

```
2556: Warning: FindFunction('GetMesh' ...): FAILED
2559: ResolveCallOp: 'AttachToComponent' -> function_name='K2_AttachToComponent', target_class='Actor'
2589: Step 'get_mesh', type='GetVariable', target='Mesh'   ← UPROPERTY auto-rewrite kicked in
2594: step 'attach', type='CallFunction', target='K2_AttachToComponent'
2623: Connected pins: Mesh -> Parent
2622: Data wire OK: @cast.auto -> step 'attach'.self (explicit Target)
```

**What happened:**
The AI plan requested `GetMesh` as a `call` op (function call). `FindFunction('GetMesh')` failed entirely (line 2556) — this is expected since GetMesh was removed from RewriteAccessorCalls but is not a visible UFunction in the search path for BP_ThirdPersonCharacter. The resolver then fell back to its UPROPERTY-detection path, silently rewriting the `call` op to a `get_var` op targeting the `Mesh` variable (line 2589 shows `type='GetVariable', target='Mesh'`).

The attachment step then uses `Target:"@cast.auto"` — meaning `K2_AttachToComponent` is called on the **BP_Gun actor** (the cast result), not on BP_Gun's GunMesh component. The **Parent** pin gets connected to the character's `Mesh` variable (the character's SkeletalMeshComponent), which is correct for the socket end.

**Root cause:** The AI intended "attach the gun's mesh to the character's mesh at a hand socket" — component-to-component. What was built is "attach the gun **actor** to the character's mesh component" via `K2_AttachToComponent` on the Actor class with Target = BP_Gun cast result.

The correct call would be `GunMesh.AttachToComponent(CharacterMesh, EAttachmentRule, SocketName)` — i.e., call `K2_AttachToComponent` on the **GunMesh** SceneComponent of BP_Gun, wiring `Target = @cast.GunMesh` or a `get_var` step on the cast result. The AI never retrieved the GunMesh component from BP_Gun at all.

**Severity:** Functional bug — the gun attaches at the actor pivot, not the hand socket. Physics/rendering will appear wrong.

**Fix category:** Prompt/knowledge. The gun template should document that equip attachment requires calling AttachToComponent on the gun's **mesh component**, not the gun actor. The `gun` template's `catalog_description` or a reference template should note: "To equip: call `GunMesh.AttachToComponent(CharacterMesh, KeepRelative, 'hand_r')` — NOT actor.AttachToComponent."

---

### Issue 2: Event Dispatchers Created and Never Bound From Outside

**Log lines (lines 1919–1921, 1944–1951):**
```
1919: Added event dispatcher 'OnFired' to '/Game/Blueprints/BP_Gun' with 1 params
1920: Added event dispatcher 'OnAmmoEmpty' to '/Game/Blueprints/BP_Gun' with 0 params
1921: Added event dispatcher 'OnReloaded' to '/Game/Blueprints/BP_Gun' with 1 params

1945: step_id='s_call_on_fired', op='call_delegate', target='OnFired'   ← called inside BP_Gun.Fire()
1950: step_id='s_call_empty', op='call_delegate', target='OnAmmoEmpty'  ← called inside BP_Gun.Fire()
2146: step_id='s_call_reloaded', op='call_delegate', target='OnReloaded' ← called inside BP_Gun.Reload()
```

**What happened:** The gun template creates three event dispatchers (`OnFired`, `OnAmmoEmpty`, `OnReloaded`) and the gun's `Fire`/`Reload` functions call them (`call_delegate`). However, the agent **never** bound any handler in BP_ThirdPersonCharacter to these dispatchers. No `bind_dispatcher` op was used. The dispatchers fire correctly inside BP_Gun, but have zero listeners — they are dead signals.

For example, `OnAmmoEmpty` should drive a UI reload prompt or auto-trigger reload. `OnFired` should update an ammo HUD widget. None of this was wired.

**Why:** The AI was told in the second agent run only to "continue" the work — the decomposition nudge likely focused it on wiring inputs (LMB, R). It never circled back to bind the dispatchers on the character side. Additionally, `bind_dispatcher` is in the op vocabulary but the agent did not use it. The gun template doesn't explain that dispatchers need to be bound from the actor that owns the gun reference.

**Severity:** Medium — dispatchers exist, compile clean, fire correctly from within the gun, but have no effect. The design intent (e.g., empty ammo feedback) is silently dropped.

**Fix category:** Template (gun factory template should include a comment/note in its catalog_description that dispatchers must be bound after equip), and prompt/knowledge (agent should know that creating dispatchers in a gun implies binding them on the equipping actor's EquipGun function).

---

### Issue 3: Nodes Created via K2Node_CallFunction Cannot Be Dragged (Ghost Node Pattern)

**Log lines (lines 2811–2864):**
```
2813: Warning: FindFunction('HandleFire' ...): FAILED (searched 21 classes)
2814: blueprint.add_node -> FAILED
...
2837: Warning: SetNodePropertiesViaReflection: Property 'function_name' not found on K2Node_CallFunction
2838: CreateNodeByClass: Set 0/1 properties on K2Node_CallFunction
2839: CreateNodeByClass: Set FunctionReference for 'HandleFire' via 'HandleFire' (match method: 3, class: BP_ThirdPersonCharacter_C)
2840: Successfully created K2Node_CallFunction with 3 pins, 0 properties set, 1 skipped
```

**What happened:** After four failed attempts with `type:"CallFunction"` and `type:"Call Function"`, the agent discovered `type:"K2Node_CallFunction"` routes through `CreateNodeByClass`. This path:
1. Creates the raw `UK2Node_CallFunction` instance
2. Fails to set `function_name` via reflection (property doesn't exist on the UClass — it's stored in the FMemberReference, not as a UPROPERTY named `function_name`)
3. Falls into the special-case `SetFunctionReference` path, which succeeds (match method 3 = `GeneratedClass`)
4. Reports "3 pins" — exec-in, exec-out, and a return/self pin

**The dragging problem:** `CreateNodeByClass` uses the raw `NewObject<UK2Node_CallFunction>()` → set reference → `AllocateDefaultPins()` path. However, the node is created with the `FunctionReference` set to a **local function** (`HandleFire`) on the same Blueprint. In the Blueprint editor's live graph, this creates a node that references the function correctly and has 3 pins. This should be draggable.

But: the log shows `0 properties set, 1 skipped`. The `function_name` property was not set via reflection — only `SetFunctionReference` was called. This means the node was **not** initialized through the normal `K2_CallFunction` curated path, which calls `SetFromFunction(UFunction*)` before `AllocateDefaultPins`. Instead it went through a late-binding path.

The user reports nodes cannot have pins dragged. This is consistent with a node that:
- Was correctly created in memory
- Has pins allocated
- But the pins may have stale/cached type information because the function reference was resolved lazily

**Deeper root cause:** The `blueprint.add_node` tool with `type:"CallFunction"` fails when the function is a **local Blueprint function** (not in the hardcoded library classes). The node catalog lookup for `CallFunction` type tries `FindFunction('HandleFire')` which searches library classes — it never searches the Blueprint's own `FunctionGraphs`. This is why the first 4 attempts fail.

When the fallback `K2Node_CallFunction` path succeeds, the pin count is 3 (exec-in, exec-out, self) — a local function call node. This is structurally correct. The dragging issue may be a separate rendering/validation problem in the editor when the Blueprint is in an "unsaved" in-memory state.

**A separate factor that could cause undraggable pins:** The nodes created by the second agent run (after the 240s timeout killed run 1) reuse the in-memory Blueprint objects that were partially modified by run 1. The `LoadPackage: SkipPackage` / `Failed to find object 'ObjectRedirector'` warnings (lines 2244–2246, 2302, 2351, 2412, etc.) indicate that **the in-memory objects are not flushed to disk** between the two runs. The second run operates on an already-modified-but-not-saved Blueprint. This could leave stale pin caches.

**Severity:** High (UX regression). The user reports this as a regression, which indicates this has regressed from a previously working state.

**Fix category:** Tool bug. The `blueprint.add_node` curated map for `CallFunction`/`K2Node_CallFunction` does not support **local Blueprint function** calls. It should route through `plan_json` with `op:"call"` instead, or the curated map should search the Blueprint's own `FunctionGraphs`. The knowledge pack (`cli_blueprint.txt`) and error message for `blueprint.add_node` with type `CallFunction` should redirect the agent to use `plan_json` `op:"call"` for local function calls.

---

### Issue 4: Pickup Does Not Use Existing Interactable Logic

**Log lines (lines 1796, 1843–1845, 1865):**
```
1796: Injected 10 related assets from keyword search (keywords: gun, bp_gun, bp_thirdpersoncharacter, can, pickup)
1843: Discovery pass: 8 results in 11.1s (LLM=yes, queries: gun weapon blueprint; ranged combat component; projectile bullet spawn; pickup equip inventory; damage apply hit)
1845: 8 results in 11.1s
1865: olive.get_recipe params: {"query":"pickup equip attach actor socket"}
```

**What happened:** The discovery phase ran 5× `search_community_blueprints` with queries that did **not** include "interactable" or "interface". The recipe query for pickup was `"pickup equip attach actor socket"` — no mention of interface or interaction system. No search was ever made for "BPI_Interactable", "interact", "IInteractable", or similar. The agent invented a SphereComponent overlap approach from scratch.

**The user reports** there is existing interactable logic in the project. This logic was never found because:
1. The discovery pass queries focused on `pickup equip inventory` (physical overlap pattern) rather than `interactable interface interact`
2. The 10 context-injected assets from the keyword search on `pickup` did not include the interactable interface
3. The `project.search` calls (lines 2487–2501) searched only for "ThirdPersonCharacter" and "BP_ThirdPerson" — never searched for "interact", "BPI", or "interface"

**Severity:** High (wrong approach). The agent reinvented the wheel with a custom overlap sphere instead of using the existing interaction system. This creates duplicate/conflicting pickup logic and ignores project conventions.

**Fix category:** Prompt/knowledge + discovery. The discovery pass should include a "find existing project conventions" query. The AGENTS.md sandbox or recipe_routing knowledge pack should guide the agent to search for existing gameplay interfaces (BPI_*, I*) before inventing new pickup patterns. A recipe for "pickup equip" should note: "First search for existing BPI_Interactable or interaction interfaces in the project."

---

### Issue 5: blueprint.read Returns 0 Components for BP_ThirdPersonCharacter

**Log lines (lines 2492–2508):**
```
2493: Warning: Failed to find object 'ObjectRedirector None./Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter'
2494: Read 0 components from Blueprint 'BP_ThirdPersonCharacter'
2495: Read Blueprint summary: BP_ThirdPersonCharacter (Type: Blueprint, Variables: 1, Components: 0)
...
2505: Warning: Failed to find object 'ObjectRedirector None./Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter'
2506: Read 0 components from Blueprint 'BP_ThirdPersonCharacter'
```

**What happened:** The agent read BP_ThirdPersonCharacter's `summary` and `components` sections but got 0 components back. The `Failed to find object 'ObjectRedirector'` warning is a benign UE warning about a non-existent redirector — the Blueprint is still found and returned. However, `OliveComponentReader` reads 0 components.

BP_ThirdPersonCharacter inherits from ACharacter, which has components defined in C++ (CapsuleComponent, ArrowComponent, CharacterMovement, Mesh, Camera, SpringArm). These are C++-native components and may not appear in the SCS (SimpleConstructionScript) that `OliveComponentReader` inspects — depending on whether `OliveComponentReader` also walks the parent C++ CDO.

**Impact:** The agent could not discover the character's existing `Mesh` (SkeletalMeshComponent) from the read results. Instead it found `Mesh` through the resolver's UPROPERTY fallback when `GetMesh()` failed (line 2589 shows the resolver used variable name `'Mesh'`). The agent worked around it, but it was flying blind. If the agent had needed to know the socket names or component hierarchy, it would have had no data.

**Severity:** Medium (agent worked around it, but the blind read degrades reliability for complex component wiring tasks involving inherited C++ components).

**Fix category:** Tool bug. `OliveComponentReader` may not be walking the CDO's native component list for ACharacter/APawn parent classes. It likely only reads the `SimpleConstructionScript` node array, which holds Blueprint-added components. Native components from C++ parents are not in the SCS and require CDO inspection.

---

### Issue 6: blueprint.add_node Fails 4 Times for Local Function Calls Before Finding K2Node_CallFunction Workaround

**Log lines (lines 2811–2864):**
```
2813: type="CallFunction", function_name="HandleFire" -> FAILED (FindFunction failed, 21 classes searched)
2818: type="CallFunction", function_name="HandleReload" -> FAILED
2822: type="Call Function", function_name="HandleFire" -> FAILED (3.4ms)
2826: type="Call Function", function_name="HandleReload" -> FAILED
2829: type="K2Node_CallFunction", function_name="HandleFire" -> SUCCESS (via CreateNodeByClass)
2847: type="K2Node_CallFunction", function_name="HandleReload" -> SUCCESS
```

**What happened:** The agent spent ~18 seconds (4:33:37 to 4:33:52) making 4 failed attempts to call `blueprint.add_node` with function names that are **local to the Blueprint**. The `FindFunction` path in the curated `CallFunction` node type does not search `FunctionGraphs` of the Blueprint itself — only external classes. This is a known gap.

The agent self-corrected by trying `K2Node_CallFunction` as a raw class name, which hits the `CreateNodeByClass` path and works. But it took 3 retries (attempts at "CallFunction", "Call Function", "K2Node_CallFunction") to find this.

**Note:** The error message returned for these failures should hint to the agent that for local functions, `plan_json` with `op:"call"` is the right approach. Currently the error message is just the FindFunction failure trail (with 21 searched classes), which gives no routing guidance.

**Fix category:** Tool bug + prompt/knowledge. The error message for `blueprint.add_node` type `CallFunction` when function is not found should say: "For calling local Blueprint functions, use `blueprint.apply_plan_json` with `op: 'call'` instead of `blueprint.add_node`. The `add_node` tool does not search local Blueprint function graphs." This would eliminate the retry loop.

---

### Issue 7: 240-Second Idle Timeout (Run 1) Lost Work and Re-Did It

**Log line 2213:**
```
2213: Warning: Claude process: no MCP tool call in 240 seconds (nudge limit=240s) - nudge kill (will auto-continue)
2215: Run timed out (attempt 1/2) — relaunching with decomposition nudge
```

**What happened:** Run 1 created BP_Bullet and BP_Gun successfully, then went silent for 4 minutes. The timeout killed it and relaunched as run 2 with a decomposition nudge. Run 2 then re-applied the same gun construction script logic (SetCurrentAmmo, SetCanFire) that the gun template had already embedded. This is the `CleanupPreviousPlanNodes` logic correctly removing the template's plan nodes and replacing with the agent's own (line 2304–2305). No rework of the template-baked functions — only the ConstructionScript was touched twice.

The 4-minute gap corresponds to the time between BP_Gun creation (04:24:09) and the next tool call in run 2 (04:28:40). During this time, run 1 was presumably reasoning about how to modify BP_ThirdPersonCharacter — a complex multi-step plan. The planning time exceeded the 240s idle threshold.

**Severity:** Low-medium — the work was not lost (the templates baked the logic), but the duplicate ConstructionScript apply was unnecessary. The bigger concern is that the timeout occurs during the reasoning phase, not stuck I/O, suggesting 240s may be too short for tasks involving multi-BP modification with significant planning.

**Fix category:** Configuration. Consider raising the nudge limit for tasks that involve multi-asset modification (or detecting the reasoning context from the prompt). The 4-minute gap is not unusual for a complex multi-BP task.

---

### Issue 8: `blueprint.read` Called with section:"components" Returns 0 on Inherited-Component Blueprints

This is a re-emphasis of Issue 5 in isolation — the agent called `blueprint.read` twice (summary then components) looking for the character mesh (to use as the attachment parent). Getting back "0 components" on both calls, the agent proceeded without knowing the socket name or mesh path. The result is the `AttachToComponent` call has no socket name set (only `Mesh -> Parent` was connected, no socket name default was set in Phase 5).

**Log lines:**
```
2625: Phase 5: Set Pin Defaults
2626: Phase 5 complete: 4 defaults set, 0 failed
```

The 4 defaults set were likely `EAttachmentRule` parameters. The socket name (e.g., `hand_r`) was not set — confirming the gun will attach to the character mesh root, not the hand socket.

**Fix category:** Tool bug (OliveComponentReader not reading inherited C++ components) + prompt/knowledge (agent should try `project.snapshot` or `blueprint.read section:"variables"` to discover the Mesh variable when components returns empty).

---

### Issue 9: `LoadPackage: SkipPackage` Warnings on All Second-Run Blueprint Accesses

**Log lines (e.g., 2244–2246, 2302, 2351, 2412, 2493, 2505, 2527, 2535, 2565, 2677, 2740, 2793, 2804, 2834, 2852, 2870, 2883):**
```
LoadPackage: SkipPackage: /Game/Blueprints/BP_Gun ... The package to load does not exist on disk or in the loader
Failed to find object 'ObjectRedirector None./Game/Blueprints/BP_Gun'
```

These warnings appear on every single Blueprint access in run 2 (and on BP_ThirdPersonCharacter as well). They occur because the Blueprints were created in memory during run 1 but were **not saved to disk** before the 240s kill happened. The engine autosave ran at 04:30:39 and saved both BP_Bullet and BP_Gun, but by then run 2 was already in progress and had already made these calls.

**Impact:** The "SkipPackage" and "ObjectRedirector" warnings are benign — Olive finds the in-memory objects correctly. However they confirm that the agent is always working against in-memory state, and that disk-sync is only guaranteed after autosave (every ~6 minutes by default). If the editor were to crash mid-run, all work from run 2 would be lost.

**Severity:** Low for correctness, but worth flagging for robustness.

---

### Issue 10: ResetCanFire Graph Re-Applied Unnecessarily in Run 2

**Log lines (2304–2305):**
```
2304: CleanupPreviousPlanNodes: removing node 'K2Node_VariableSet_0' (K2Node_VariableSet) from entry '...::ResetCanFire::ResetCanFire'
2305: removed 1/1 tracked nodes for entry '/Game/Blueprints/BP_Gun::ResetCanFire::ResetCanFire'
```

The `ResetCanFire` function was already correctly populated by the `gun` template in run 1 (a simple `SetVariable bCanFire = true`). Run 2 re-applied the same plan_json to `ResetCanFire`, causing `CleanupPreviousPlanNodes` to remove and recreate the same node. The end result is identical, but it was wasted work.

**Root cause:** Run 2's context (the 2468-char stdin prompt from `BuildContinuationPrompt`) did not tell the agent which graphs were already complete. The agent re-verified and re-applied graphs it had already done.

**Fix category:** Prompt/context. The continuation prompt should summarize which graphs were already populated (tool log summary). The existing `BuildAssetStateSummary()` is called during continuation — verify it includes function completion status.

---

## Summary Table

| # | Issue | Severity | Fix Category |
|---|-------|----------|-------------|
| 1 | Wrong attachment: actor.AttachToComponent instead of GunMesh.AttachToComponent | High | Template + Prompt/knowledge |
| 2 | Dispatchers OnFired/OnAmmoEmpty/OnReloaded created but never bound from character | Medium | Template + Prompt/knowledge |
| 3 | Nodes created via K2Node_CallFunction have undraggable pins | High | Tool bug (CreateNodeByClass pin init) |
| 4 | Pickup ignores existing interactable interface in project | High | Prompt/knowledge + Discovery |
| 5 | blueprint.read returns 0 components for BP_ThirdPersonCharacter (C++ inherited) | Medium | Tool bug (OliveComponentReader) |
| 6 | blueprint.add_node fails 4× for local BP functions before K2Node workaround | Medium | Tool bug + Error message |
| 7 | 240s idle timeout killed run 1 mid-plan; run 2 re-did ConstructionScript | Low-Medium | Configuration |
| 8 | Gun attaches to mesh root, not hand socket — socket name never set | High | Prompt/knowledge + Template |
| 9 | LoadPackage SkipPackage warnings on every BP access in run 2 | Low | Informational only |
| 10 | ResetCanFire re-applied unnecessarily in run 2 | Low | Continuation context |

---

## Recommendations

1. **Attachment knowledge (gun template).** The `gun` factory template's catalog_description or a reference template `gun_patterns` should explicitly document: "To equip the gun to a character: call `K2_AttachToComponent` on the GunMesh **component** (not the gun actor), with `Parent = CharacterMesh`, `SocketName = 'hand_r'`." The AI consistently gets this wrong because `K2_AttachToComponent` resolves on `Actor` and the AI wires the actor cast result as `Target`, not the mesh component.

2. **Dispatcher binding pattern.** The `gun` template should note that its three dispatchers (`OnFired`, `OnAmmoEmpty`, `OnReloaded`) need `bind_dispatcher` calls in the equipping actor's EquipGun function. Alternatively, add a reference template entry describing the bind-on-equip pattern. Without this, dispatchers are always dead on first-agent runs.

3. **blueprint.add_node — local function error message.** When `FindFunction` fails in the `CallFunction` curated type path, the error should say: "HandleFire is a local Blueprint function. Use `blueprint.apply_plan_json` with `op: 'call', target: 'HandleFire'` to call local functions — `blueprint.add_node` type CallFunction only resolves external/library functions." This would collapse the 4-retry loop into 0.

4. **OliveComponentReader — C++ native components.** `OliveComponentReader` should be enhanced to also walk the CDO's registered component list for inherited C++ components (CapsuleComponent, Mesh, SpringArm, etc.). For ACharacter parents, reading 0 components is actively misleading and causes the agent to proceed without socket/component information.

5. **Discovery pass — project conventions search.** The discovery pass queries should include at minimum one query for existing project interfaces: e.g., `"interactable interface BPI pickup interaction"`. The current 5-query discovery never searches for project-specific interfaces/conventions. A recipe for `pickup equip` should begin with: "Search for existing interaction interfaces (BPI_Interactable, IInteractable, etc.) before implementing a new pickup pattern."

6. **gun template — socket name hint.** The attachment step in any gun equip pattern should set `SocketName = 'hand_r'` as the default. The template should document that this is the standard UE ThirdPerson skeleton socket. Currently no socket name is set at all — the gun snaps to the mesh origin.

7. **CreateNodeByClass pin dragging regression.** The `K2Node_CallFunction` nodes created via `CreateNodeByClass` show "3 pins, 0 properties set, 1 skipped." A node created this way may have its `FunctionReference` set to a `SelfMember` reference (match method 3 = GeneratedClass). This is normally correct but the path bypasses `SetFromField<UFunction>` with an actual `UFunction*`. If the function is not found at graph-open time (e.g., a compile is pending), the pins can appear "locked" in the editor. The fix is to route local-function call nodes through `plan_json op:call` which uses the fully-resolved `SetPreResolvedFunction` path, or to call `ReconstructNode()` on the `K2Node_CallFunction` nodes after creation in `CreateNodeByClass`.

8. **Idle timeout tuning.** For multi-Blueprint tasks, the 240s nudge limit may be too aggressive. The agent spent 4 minutes planning the BP_ThirdPersonCharacter modifications after finishing BP_Gun/BP_Bullet creation. Consider raising to 360s for tasks with `@` asset mentions (multi-asset intent) or detecting that the agent is in a reasoning plateau via token-output rate rather than pure silence.

9. **Continuation context completeness.** The decomposition nudge prompt sent to run 2 (2468 chars) should include a machine-readable summary of which functions/graphs were already populated vs empty, so the agent does not re-verify and re-apply complete graphs (as happened with ResetCanFire).

10. **Warn when dispatchers are created without bind_dispatcher.** Phase 5.5 (pre-compile validation) or the Write Pipeline Stage 6 could detect the pattern "dispatcher created in this session, but no bind_dispatcher op present in any plan in this session for a referencing BP" and emit a warning. This is a soft check that would remind the agent to bind the dispatcher after equip.
