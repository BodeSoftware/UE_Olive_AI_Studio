# Research: Inventory System Session Log Analysis — 2026-03-12 (Run B)

## Question

Analyze the autonomous Blueprint agent test run that built a pickup inventory system for
BP_ThirdPersonCharacter. Run started 22:17:24, ended 22:41:26. Task requested:
- BP_PickupItem (actor with sphere overlap, rotating mesh, pickup logic)
- WBP_InventorySlot and WBP_Inventory (widget UI with grid, OwnerCharacter binding)
- BP_ThirdPersonCharacter modifications (Inventory array, AddToInventory function, OnInventoryChanged dispatcher, BeginPlay widget creation)
- 3 child pickup item variants (HealthPotion, Coin, Key)

Context: fixes applied before this run included mandatory research before building,
bAutoCompile=true for function/event/dispatcher creation, widget ownership knowledge (OwnerCharacter wiring),
and recipe routing fix.

---

## Findings

### 1. Overall Success

**Run duration:** 22:17:24 → 22:41:26 = **24:02 total.**
**Brain outcome=0 (success).** State: WorkerActive → Completed → Idle.
**0 auto-continues.** Single uninterrupted pass.

**Assets produced — all compile SUCCESS at end:**
- `BP_PickupItem`: Root + StaticMeshComponent (Cone) + SphereComponent (CollectionSphere, r=150, OverlapAllDynamic) + RotatingMovementComponent. EventGraph: OnComponentBeginOverlap → cast to BP_ThirdPersonCharacter → AddToInventory → DestroyActor.
- `WBP_InventorySlot`: Border + TextBlock; ItemName variable (expose_on_spawn); Construct event → SetText.
- `WBP_Inventory`: OwnerCharacter (BP_ThirdPersonCharacter) variable; OnInventoryChanged bind; RefreshInventory function; grid widget layout.
- `BP_ThirdPersonCharacter` (modified): Added Inventory (Array<string>), InventoryWidget (WBP_Inventory_C*), InventoryCount (int), InventoryString (string); OnInventoryChanged dispatcher; AddToInventory function; BeginPlay creates+shows widget.
- `BP_PickupItem_HealthPotion`, `BP_PickupItem_Coin`, `BP_PickupItem_Key`: Children of BP_PickupItem with per-type defaults set via BeginPlay.

**Final compile batch (22:41:03–22:41:06):** All 7 assets compiled SUCCESS with 0 errors.

---

### 2. Research Phase

**Pre-run discovery pass (Olive utility model before launching Claude):** 5 search_community_blueprints queries in 11.2s:
"inventory component system", "overlap pickup collect", "UI widget inventory", "item data asset", "sphere overlap begin"
— 8 entries found. This pre-pass ran correctly.

**Agent's own research phase after MCP connect (~115s of research before first blueprint.create):**

| Timestamp | Tool | Query/Action |
|-----------|------|--------------|
| 22:18:48 | get_recipe | "pickup inventory collect item overlap" |
| 22:18:49 | blueprint.read | BP_ThirdPersonCharacter summary |
| 22:18:49 | list_templates | "pickup item inventory collect" |
| 22:18:50 | get_recipe | "widget grid inventory UI create widget" |
| 22:19:54 | project.snapshot | pre_inventory_system (failed — unsaved BP) |
| 22:20:12 | blueprint.create | **First asset created** |

**Verdict: Research mandate is working correctly.** The agent called get_recipe twice, list_templates once, and read the target character before touching any Blueprint.

Additional mid-run research at 22:36:53: agent called get_recipe "widget inventory grid wrap box text block" before building WBP_InventorySlot. Healthy pattern.

Note: `project.snapshot` failed silently ("No read tool available for asset") and was correctly ignored by the agent. Expected behavior for unsaved assets.

---

### 3. Widget Ownership (OwnerCharacter wiring)

**CONFIRMED WORKING.** The agent:

1. Added `OwnerCharacter` variable (type: BP_ThirdPersonCharacter) to `WBP_Inventory` at 22:38:19.
2. Built the Construct event plan for WBP_Inventory (22:38:29) with:
   - `get_var OwnerCharacter`
   - cast to BP_ThirdPersonCharacter
   - `set_var OwnerCharacter` to store the cast result
   - `bind_dispatcher OnInventoryChanged` with `@cast_char.auto` wired to the `.self` pin

From the log (lines 3829–3916):
```
Resolving step 3: step_id='store_ref', op='set_var', target='OwnerCharacter'
Step 4/8: step_id='store_ref', type='SetVariable', target='OwnerCharacter'
Connected pins: AsBP Third Person Character -> OwnerCharacter
```

The widget ownership knowledge injection is working as intended. The cast-then-store-then-bind pattern was used correctly.

---

### 4. bAutoCompile for Function/Event/Dispatcher Creation

**CONFIRMED WORKING.** Every `blueprint.add_function` (function, event_dispatcher, regular function) triggered an immediate compile:

- 22:21:00 — `add_function OnInventoryChanged` (event_dispatcher) → SUCCESS (40ms)
- 22:21:00 — `add_function AddItem` → SUCCESS (14ms)
- 22:22:31 — `add_function AddItem` (recreation) → SUCCESS (17ms)
- 22:33:36 — `add_function AddToInventory` → SUCCESS (34ms)
- 22:38:20 — function on WBP_Inventory → SUCCESS (13ms)
- 22:39:17 — another function → SUCCESS (40ms)

The agent could immediately use function pins in subsequent plan_json calls without any manual compile step. No "pin not found" errors traceable to stale skeletons. The bAutoCompile fix eliminates the prior need to restart or manually compile before using newly-created function signatures.

One notable exception: `add_function` at 22:28:21 compiled and got 3 errors ("Variable is undetermined. Connect something to Add...") — this was an `Array_Add` issue (see section 6b), not a signature issue.

---

### 5. Continuation Behavior

**Zero auto-continues fired.** Brain went directly WorkerActive → Completed. Max-turns was 500; the agent used roughly 128 tool calls total. No idle timeout, no continuation prompt needed. The task was completed in a single contiguous run without any stall events.

---

### 6. Errors, Warnings, and Self-Corrections

#### 6a. StaticMesh ensure failure (non-fatal cosmetic bug)
At 22:20:39, `blueprint.modify_component` on ItemMesh (setting `StaticMesh` to `/Engine/BasicShapes/Cone.Cone`) triggered a UE handled-ensure:
```
Ensure condition failed: KnownStaticMesh == StaticMesh [StaticMeshComponent.cpp:741]
StaticMesh property overwritten without NotifyIfStaticMeshChanged()
```
Source file: `OliveComponentWriter.cpp:352` (via callstack at line 1921 in log).

The ensure fired, took 1.3s for crash report processing, but execution continued and the property was set (3/4 properties applied). Blueprint compiled fine. **This is a real bug** in OliveComponentWriter that will become fatal if Epic tightens the ensure.

#### 6b. Array_Add wildcard type failure (dominant failure, ~12 minutes wasted)

The agent tried to use `Array_Add` (KismetArrayLibrary) in a plan_json to append to an `Array<string>` variable. Despite the executor successfully wiring `Inventory (Array<string>) -> TargetArray` and `NewItemName -> NewItem` (confirmed by "Connected pins:" logs), the compiler still reported:

```
The type of Target Array is undetermined. Connect something to Add to imply a specific type.
The type of New Item is undetermined. Connect something to Add to imply a specific type.
```

This happened **three times** across the run (22:21:32, 22:22:51, 22:28:21). The agent tried:
1. Plan_json with Array_Add on dual arrays → FAILED (2 errors)
2. Simplified plan_json with single Array_Add → FAILED (2 errors)
3. `blueprint.modify_function_signature` to reshape the function → FAILED (BP_MODIFY_SIGNATURE_LIMITED)
4. Removed InventoryColors variable and AddItem function, recreated with 1 param, same Array_Add → FAILED again
5. Used `blueprint.add_node` + `blueprint.connect_pins` manually → partial attempt
6. **Final resolution:** Pivoted entirely away from arrays. Created `AddToInventory` using `Concat_StrStr` + integer counter. Succeeded cleanly.

**Root cause:** `Array_Add` is a `FUNC_CustomThunk` with `ArrayParm` metadata — a wildcard generic node. Blueprint's type system requires the wildcard to be resolved via `PropagateArrayTypeInfo()` on `UK2Node_CallArrayFunction`. The `TryCreateConnection` path (which plan_json executor uses) connects the link but does not call `Node->NotifyPinConnectionListChanged(Pin)` or `Node->ReconstructNode()` afterward. Without this, the array pin never gets its concrete type and the compiler fails.

**Time wasted:** approximately 12 minutes (22:21 to 22:33).

#### 6c. AddToViewport resolution failure (self-corrected in 1 retry)
At 22:34:10, the agent called AddToViewport without `target_class`:
```
FindFunction('AddToViewport', class=''): FAILED — searched 13 classes
```
The agent immediately corrected on the next call (22:34:16) with `"target_class":"UserWidget"` → resolved to `UserWidget::AddToViewport`. Took 6s total. The suggestion in the FUNCTION_NOT_FOUND message was not useful here (top suggestions were `AddToInventory`, `AddTorqueInRadians`).

#### 6d. Graph name collision (self-corrected, 1 round trip)
At 22:33:14: "Graph named 'AddItem' already exists in 'BP_ThirdPersonCharacter'. Another one cannot be generated from AddItem." Agent had tried to recreate a function it hadn't fully removed yet. Agent called `remove_function` first (22:33:27), then created `AddToInventory` (22:33:36). ~1 minute wasted.

#### 6e. Cast pin name mismatch (self-corrected in 1 retry)
At 22:36:34: agent used `node_1.AsBPThirdPersonCharacter` (no spaces). Error returned available pins including "As BP Third Person Character". Agent retried with spaces at 22:36:40 — success. Error message → self-correction worked in 6 seconds.

#### 6f. modify_only silent failures (9 silent failures, 3–4 minutes)
After creating 3 child BPs (BP_PickupItem_HealthPotion, BP_PickupItem_Coin, BP_PickupItem_Key), the agent tried `"modify_only":true` on `blueprint.add_variable` to override inherited variable defaults. All 9 calls failed (`failed` with no error text) in ~330ms each.

The agent correctly inferred the pattern after all 9 failed and pivoted to using `apply_plan_json` with `set_var` in BeginPlay. All 3 child BPs compiled SUCCESS.

**Problem:** `modify_only:true` likely doesn't support inherited variables (they exist on parent, not child's NewVariables). The tool returned `failed` with no message — the agent had to discover this by exhaustion, wasting 9 sequential calls.

#### 6g. Padding format failure (1×, agent moved on)
At 22:37:27: `widget.set_property` for `Padding` with value `"8.0"` → "Missing opening parenthesis: 8.0". `FMargin` requires `"(Left=8.0,Top=8.0,Right=8.0,Bottom=8.0)"` or equivalent. The agent did not retry this property.

---

### 7. Inefficiencies

| Inefficiency | Time wasted | Category |
|---|---|---|
| Array_Add wildcard type failure loop | ~12 min | Tool layer bug |
| modify_only silent failures (9×) | ~3 min | Missing error message |
| AddItem graph name collision | ~1 min | Agent error, 1 round trip |
| project.snapshot on unsaved asset | ~5s | Expected limitation |
| AddToViewport class miss | ~6s | Missing alias |
| Cast pin name mismatch | ~6s | Self-corrected quickly |

Total waste: ~16 minutes out of 24. Most of this is the Array_Add problem.

---

### 8. Tool Call Accounting (approximate)

| Tool | ~Count | Notes |
|------|--------|-------|
| blueprint.create | 10 | 7 assets + 3 child BPs |
| blueprint.add_variable | 24 | includes 9 modify_only failures |
| blueprint.add_component | 5 | |
| blueprint.modify_component | 2 | |
| blueprint.add_function | 8 | includes 2 failures |
| blueprint.apply_plan_json | 18 | 9 success, ~9 failures |
| blueprint.compile | 9 | end-of-run batch |
| blueprint.read | 5 | |
| blueprint.connect_pins | 7 | includes 1 failure |
| blueprint.add_node | 2 | |
| blueprint.remove_function | 2 | |
| blueprint.remove_variable | 1 | |
| blueprint.remove_node | 4 | |
| blueprint.get_node_pins | 3 | |
| widget.add_widget | 2 | |
| widget.set_property | 7 | includes 1 failure |
| olive.get_recipe | 3 | |
| blueprint.list_templates | 1 | |
| olive.search_community_blueprints | 5 | pre-run discovery |
| project.snapshot | 1 | failed |
| blueprint.modify_function_signature | 1 | failed |
| **Total** | **~128** | **~86% success rate** |

---

## Recommendations

1. **Fix Array_Add wildcard type propagation (highest priority).** After the executor connects data pins to a `UK2Node_CallArrayFunction` node, call `Node->ReconstructNode()` to propagate concrete array element type. Without this, Array_Add (and any wildcard array operation) will fail at compile time even though the wiring looks correct. This single fix would have saved 12 minutes in this run and was previously flagged in run 09-12a analysis.

2. **Fix OliveComponentWriter StaticMesh direct assignment.** `OliveComponentWriter.cpp:352` sets `StaticMesh` via direct UPROPERTY assignment. Replace with `UStaticMeshComponent::SetStaticMesh(MeshAsset)` (the correct API). The handled-ensure at `StaticMeshComponent.cpp:741` will become a crash if Epic hardens this check.

3. **Fix modify_only error message for inherited variables.** When `blueprint.add_variable` with `modify_only:true` fails because the variable is inherited from a parent class, return: "Variable 'X' exists on parent class 'Y' — cannot set default here. To override the value at runtime, use set_var in this Blueprint's BeginPlay." Currently returns `failed` with no explanation.

4. **Add UserWidget common functions to alias map or hardcoded library set.** `AddToViewport`, `RemoveFromParent`, `AddToViewportWithZOrder`, `SetVisibility` on UserWidget are extremely common operations. Add a class hint so FindFunction resolves them without requiring `target_class` from the agent.

5. **Add FMargin format documentation to widget knowledge.** Widget property `Padding`, `Margin`, and similar FMargin fields require `"(Left=X,Top=Y,Right=Z,Bottom=W)"` format. Document in widget recipe or cli_blueprint.txt. Bare float ("8.0") fails.

6. **Add ReceiveConstruct/Construct to widget event alias map.** Map `"Construct"` → `"ReceiveConstruct"` in the event resolver (same pattern as BeginPlay → ReceiveBeginPlay). Current fallthrough works but is noisy and slower.

7. **Agent architecture is performing well.** 7 assets, all compile SUCCESS, 0 human intervention, 0 auto-continues, ~128 tool calls in 24 minutes. Research phase, bAutoCompile, widget ownership wiring — all working as designed. The dominant remaining failure mode is Array_Add (tool layer bug, fix in executor).

8. **The Array_Add problem is reproducible and well-understood.** The executor wires correctly but skips the mandatory type propagation step that the Blueprint editor does via `Node->PinConnectionListChanged()` + `Node->ReconstructNode()`. This is the same root cause as prior run analyses — it has not been fixed yet.

9. **Consider knowledge note for Array_Add.** Until the executor fix lands, add to cli_blueprint.txt: "Array_Add on typed arrays (Array<string>, Array<Object>) requires explicit reconstruction. Prefer using string concatenation or a typed helper function instead of Array_Add if possible. If you must use Array_Add, compile and verify immediately."

10. **Child BP variable override pattern works via BeginPlay set_var.** The agent discovered the correct workaround for `modify_only` failure. This pattern (BeginPlay + set_var for per-instance overrides) should be documented as the preferred child BP customization approach in the knowledge pack, since `modify_only` appears to be unsupported for inherited variables.
