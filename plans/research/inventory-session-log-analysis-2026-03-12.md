# Research: Inventory System Session Log Analysis — Run 2026-03-12

## Question
Full analysis of the autonomous agent run that built a grid-style pickup inventory system for `BP_ThirdPersonCharacter`. The run used the new progressive idle timeout (120s nudge / 300s hard kill), directive continuation prompt with empty function graph scanning, empty function graph warning in `add_function`, CastTargetMap component pre-scan, and depth-first guidance in the knowledge pack.

---

## Findings

### 1. Timeline

Log file: `UE_Olive_AI_Toolkit-backup-2026.03.12-06.17.09.log` (5277 lines, 744 LogOliveAI entries)

| Timestamp | Event |
|---|---|
| 05:35:27 | Engine startup |
| 05:35:39 | OliveAI module started, session initialized |
| 05:35:41 | MCP server on port 3000 |
| 05:36:37 | Claude Code CLI validated (v2.1.74) |
| 05:37:37 | **Run started** [D015F738438C6F57D52178A6C205AE81] |
| 05:37:50 | Research phase begins: 5x `search_community_blueprints`, 2x `get_template`, 1x `list_templates`, 1x `get_recipe` |
| 05:39:13 | Second `get_template` call for `action_rpg_bp_rpg_item_pickup_base` (EventGraph pattern) |
| 05:40:30 | `project.snapshot` — **FAILED** (pre-existing BP, no IR snapshot support) |
| 05:40:31 | Asset creation phase: BP_PickupItem, WBP_InventorySlot, WBP_InventoryGrid |
| 05:40:45 | Components, variables, widget tree construction |
| 05:41:37 | Function creation phase |
| 05:42:14 | First compile: BP_PickupItem — **SUCCESS** |
| 05:42:29 | First `apply_plan_json` — FAILED (FUNCTION_NOT_FOUND: SetText, SetBrushColor — missing target_class) |
| 05:42:35 | Agent calls `describe_function` for SetText and SetBrushColor — correct self-correction |
| 05:42:43 | Second `apply_plan_json` — FAILED (ReceiveConstruct event not found in UserWidget) |
| 05:42:56 | Third `apply_plan_json` — FAILED (String→Text data wire, autocast gap) |
| 05:43:32 | Agent calls `describe_function Conv_StringToText` |
| 05:43:39 | Fourth `apply_plan_json` for EventGraph WBP_InventorySlot — **FAILED** (compile error: pure function GetItemDisplayText has no return op) |
| 05:44:38 | Agent creates `GetItemDisplayText` and `GetSlotColor` pure functions |
| 05:44:44 | Fifth `apply_plan_json` for `GetItemDisplayText` — **FAILED** (compile: EXEC_SOURCE_IS_RETURN, no return op wiring) |
| 05:44:45 | Sixth `apply_plan_json` for `GetSlotColor` — **FAILED** (same) |
| 05:44:53 | Agent reads the graph to understand node layout |
| 05:46:28 | Agent manually disconnects and re-wires EventGraph nodes |
| 05:46:36 | `apply_plan_json` GetItemDisplayText — **SUCCESS** |
| 05:46:37 | `apply_plan_json` GetSlotColor — **SUCCESS** |
| 05:46:46 | `widget.bind_property` ItemNameText → GetItemDisplayText — **SUCCESS** |
| 05:46:59 | `apply_plan_json` WBP_InventoryGrid AddSlot (CreateWidget path) — **FAILED** (no CreateWidget class pin support) |
| 05:47:16 | Agent creates `InitSlot` function on WBP_InventorySlot |
| 05:47:21 | `apply_plan_json` InitSlot — **SUCCESS** |
| 05:47:31 | `apply_plan_json` AddSlot (CreateWidget + AddChild + InitSlot) — **SUCCESS** |
| 05:47:44 | `set_pin_default` for WBP_InventorySlot class pin — **SUCCESS** |
| 05:47:51 | `apply_plan_json` AddToInventory (Array_Add + AddSlot) — **FAILED** (Array_Add undetermined type compile error) |
| 05:48–05:55 | Agent cycles through granular tools attempting to wire AddToInventory: add_node + connect_pins for FunctionEntry — 4 consecutive FAILED connect_pins (wrong semantic refs) |
| 05:55:58 | `apply_plan_json` AddToInventory (simpler: Array_Add only) — **SUCCESS** |
| 05:56:09 | `apply_plan_json` EventGraph overlap + cast + AddToInventory call — **SUCCESS** |
| 05:57:51 | `apply_plan_json` EventGraph with get_var ItemName/ItemColor on ThirdPersonCharacter — **FAILED** (VARIABLE_NOT_FOUND — those vars are on BP_PickupItem, not character) |
| 05:58:00 | Agent creates `GetItemInfo` function on BP_PickupItem (pure, multi-return) |
| 05:58:05 | `apply_plan_json` GetItemInfo — **SUCCESS** |
| 05:58:11 | `apply_plan_json` EventGraph (overlap + cast + GetItemInfo + AddToInventory + DestroyActor) — **SUCCESS** |
| 05:58:50 | Compile ThirdPersonCharacter — **FAILED** (Array_Add undetermined type — granular nodes still in graph) |
| 05:59:29 | Agent removes the orphan Array_Add nodes from EventGraph |
| 05:59:38 | Compile ThirdPersonCharacter — **SUCCESS** |
| 05:59:43 | Agent adds InputKey(I), FlipFlop, SetVisibility nodes |
| 06:00:24 | connect_pins InventoryWidget → SetVisibility.self — **FAILED** (TypesIncompatible: WBP_InventoryGrid_C* vs UWidget*) |
| 06:00:38 | Agent removes SetVisibility nodes |
| 06:00:42 | Agent tries AddToViewport — **FAILED** (FUNCTION_NOT_FOUND, no target_class) |
| 06:02:07 | Agent tries RemoveFromParent — **FAILED** (FUNCTION_NOT_FOUND, same) |
| 06:02:20 | Agent creates `ToggleInventory` function |
| 06:02:30 | `apply_plan_json` ToggleInventory (branch, AddToViewport, RemoveFromParent, SetVisibility) — **SUCCESS** |
| 06:02:49 | Agent adds `ToggleInventory` call node in EventGraph (3rd attempt, "K2Node_CallFunction" type worked) |
| 06:02:56 | Compile ThirdPersonCharacter — **SUCCESS** |
| 06:03:12 | Agent creates 3 child BPs: BP_Pickup_HealthPotion, BP_Pickup_Coin, BP_Pickup_GemStone |
| 06:03:18 | `add_variable modify_only` on child BPs — all **FAILED** (not supported on inherited-variable BPs) |
| 06:03:44 | `apply_plan_json` UserConstructionScript on each child BP (set_var for ItemName + ItemColor) — all **SUCCESS** |
| 06:04:00 | Final mass compile: all 7 BPs — all **SUCCESS** |
| 06:04:23 | **Run completed** outcome=0 (Completed) |
| 06:17:08 | Editor shutdown |

**Total duration: 26 minutes 46 seconds** (05:37:37 → 06:04:23)

**Auto-continues: 0.** This was a single uninterrupted run. The progressive idle timeout (120s nudge / 300s hard kill) did **not trigger** — no idle gaps exceeded 120s. The longest LLM think time was approximately 90 seconds (05:40:30 → 05:40:31 create phase, and the 06:00:57 → 06:01:00 describe_node_type pause).

---

### 2. Tool Call Inventory

Total LogOliveAI entries: 744. Total MCP tool dispatches counted by tool result lines:

| Category | Tool | Count |
|---|---|---|
| **Research** | `olive.search_community_blueprints` | 5 |
| | `blueprint.get_template` | 2 |
| | `blueprint.list_templates` | 1 |
| | `olive.get_recipe` | 1 |
| | `blueprint.describe_function` | 6 |
| | `blueprint.describe_node_type` | 1 |
| | `blueprint.read` | 6 |
| | `blueprint.get_node_pins` | 4 |
| **Structural writes** | `project.snapshot` | 1 (FAILED) |
| | `blueprint.create` | 6 |
| | `blueprint.add_component` | 4 |
| | `blueprint.modify_component` | 4 |
| | `blueprint.add_variable` | 14 |
| | `blueprint.add_function` | 8 |
| | `blueprint.remove_function` | 1 |
| **Widget** | `widget.add_widget` | 7 |
| | `widget.set_property` | 7 (1 FAILED) |
| | `widget.bind_property` | 1 |
| **Graph edits (granular)** | `blueprint.add_node` | 13 (3 FAILED) |
| | `blueprint.connect_pins` | 14 (6 FAILED) |
| | `blueprint.disconnect_pins` | 2 |
| | `blueprint.remove_node` | 9 |
| | `blueprint.set_pin_default` | 1 |
| **Plan JSON** | `blueprint.apply_plan_json` | 18 (9 FAILED, 9 SUCCESS) |
| **Compile** | `blueprint.compile` | 15 (5 FAILED, 10 SUCCESS) |

**Total tool calls: ~128** (approximate from line count patterns).
**plan_json success rate: 9/18 = 50%** (versus prior bow-arrow runs of 55–78%).
**Overall tool success rate: ~110/128 = ~86%**.

---

### 3. What Worked

**Blueprints created:** 7 total
- `BP_PickupItem` — Actor, full logic, compiled SUCCESS
- `WBP_InventorySlot` — Widget, full logic + property bindings, compiled SUCCESS
- `WBP_InventoryGrid` — Widget, AddSlot function with CreateWidget+AddChild+InitSlot, compiled SUCCESS
- `BP_ThirdPersonCharacter` — Modified, CollectionSphere overlap + cast + AddToInventory + ToggleInventory via InputKey(I), compiled SUCCESS
- `BP_Pickup_HealthPotion` — Child BP, default values set via UserConstructionScript, compiled SUCCESS
- `BP_Pickup_Coin` — Child BP, same, compiled SUCCESS
- `BP_Pickup_GemStone` — Child BP, same, compiled SUCCESS

**Function graphs with logic (apply_plan_json SUCCESS):**
- `WBP_InventorySlot::GetItemDisplayText` — get_var + Conv_StringToText + return
- `WBP_InventorySlot::GetSlotColor` — get_var + return
- `WBP_InventorySlot::InitSlot` — set_var(ItemName) + set_var(ItemColor)
- `WBP_InventoryGrid::AddSlot` — CreateWidget + GetOwningPlayer + InitSlot + AddChild
- `BP_ThirdPersonCharacter::AddToInventory` — Array_Add (string array)
- `BP_ThirdPersonCharacter::EventGraph` — overlap event + cast + GetItemInfo + AddToInventory + DestroyActor
- `BP_ThirdPersonCharacter::ToggleInventory` — branch + AddToViewport + RemoveFromParent + SetVisibility
- `BP_PickupItem::GetItemInfo` — get_var(ItemName) + get_var(ItemColor) + return(multi)
- `BP_Pickup_*::UserConstructionScript` — set_var(ItemName) + set_var(ItemColor) for each child

**Fix verifications:**

**CastTargetMap component inference CONFIRMED FIRING** (line 3739):
```
ResolveCallOp: 'AddToViewport' -> function_name='AddToViewport', target_class='UserWidget' (via cast target 'WBP_InventoryGrid_C' from step 'cast_widget')
```
This confirms the fix works. The `ToggleInventory` plan successfully resolved `AddToViewport` and `RemoveFromParent` through the WBP_InventoryGrid_C cast target class. Both appear in the executor log as pre-resolved (lines 3772, 3777).

**@entry parameter synthesis CONFIRMED WORKING** extensively. Lines like:
```
Synthesized get_var step '_synth_param_newitemname' for @entry.~NewItemName alias
Synthetic param name match: step '_synth_param_newitemname' -> pin 'NewItemName'
```
This fired in at least 6 separate plan_json executions and connected correctly every time it reached Phase 4.

**PreResolvedFunction CONFIRMED** firing 15+ times across the run, e.g.:
```
CreateCallFunctionNode: Used pre-resolved function 'TextBlock::SetText'
CreateCallFunctionNode: Used pre-resolved function 'WBP_InventoryGrid_C::AddSlot'
CreateCallFunctionNode: Used pre-resolved function 'KismetArrayLibrary::Array_Add'
```

**Widget @ref synthesis CONFIRMED** (lines 2364–2365):
```
Synthesized get_var step '_synth_widget_itemnametext' for bare widget ref '@ItemNameText'
Synthesized get_var step '_synth_widget_slotborder' for bare widget ref '@SlotBorder'
```

**RecordPlanNodes / CleanupPreviousPlanNodes CONFIRMED** (lines 3405, 3438). Rollback of failed AddToInventory attempts correctly cleared tracked nodes.

**Depth-first ordering CONFIRMED** — the agent completely finished WBP_InventorySlot before moving to WBP_InventoryGrid, then handled ThirdPersonCharacter last. This is a clear improvement from breadth-first behavior in prior runs.

---

### 4. What Failed

**plan_json failures (9 total):**

| # | Target | Failure Mode | Root Cause |
|---|---|---|---|
| 1 | WBP_InventorySlot EventGraph | FUNCTION_NOT_FOUND: SetText, SetBrushColor | Missing `target_class` — agent self-corrected with describe_function |
| 2 | WBP_InventorySlot EventGraph | FUNCTION_NOT_FOUND: ReceiveConstruct | `ReceiveConstruct` is not a valid Blueprint event name; correct name is `Construct` |
| 3 | WBP_InventorySlot EventGraph | Data wire FAILED: String→Text | `@get_name.auto` was String, SetText needs Text; `Conv_StringToText` was missing from plan |
| 4 | WBP_InventorySlot GetItemDisplayText | Compile FAILED: EXEC_SOURCE_IS_RETURN | Agent tried to target pure function graph with return op but didn't wire exec chain from entry |
| 5 | WBP_InventorySlot GetSlotColor | Same | Same |
| 6 | WBP_InventoryGrid AddSlot | Compile FAILED: no WidgetType class pin connection | `CreateWidget` class pin could not be set via plan (class pin wiring is complex) |
| 7 | BP_ThirdPersonCharacter AddToInventory | Compile FAILED: Array_Add undetermined type (×2) | `Array_Add` has wildcard pins — type inference from connected `Inventory` array didn't propagate; fixed by simpler plan omitting AddSlot call in same function |
| 8 | BP_ThirdPersonCharacter EventGraph (overlap) | Phase 0: VARIABLE_NOT_FOUND — ItemName, ItemColor | Agent planned to read BP_PickupItem vars on ThirdPersonCharacter instead — correct `get_var` on the cast result, not self |
| 9 | BP_ThirdPersonCharacter AddToInventory (3rd attempt, full version) | Compile FAILED: Array_Add undetermined type | Same issue as #7 — Inventory array gets the string type only when NewItemName is connected; agent's plan routed through is_valid before wiring types |

**connect_pins failures (6 total):**

- 4x "FunctionEntry" / "entry" / "AddToInventory" semantic ref failures: agent tried to connect from the function entry node but didn't know the correct stable node ID. The FunctionEntry node shows as `node_0: AddToInventory [K2Node_FunctionEntry]` in the graph — the agent needed to read first to get the real ID.
- 2x InventoryWidget → SetVisibility.self TypesIncompatible: `WBP_InventoryGrid_C*` vs `UWidget*` — correct class mismatch. Agent replaced these with AddToViewport/RemoveFromParent approach.

**add_node failures (3 total):**

- 1x `AddToViewport` with no `function_class` — resolved to failure because ThirdPersonCharacter doesn't inherit UserWidget. Fixed by using cast target class in plan_json.
- 1x `RemoveFromParent` with `function_class=UserWidget` — same issue; ThirdPersonCharacter context means FindFunction doesn't find it.
- 2x `ToggleInventory` with `type="CallFunction"` and `type="Call Function"` (typo) — the actual fix was using `type="K2Node_CallFunction"` on the 3rd attempt.

**add_variable failures (6 total at lines 4598–4618):**

All 6 were `modify_only:true` attempts on child BPs (BP_Pickup_HealthPotion, BP_Pickup_Coin, BP_Pickup_GemStone) trying to set default values for inherited variables (`ItemName`, `ItemColor`). The tool correctly rejects `modify_only` on variables not directly owned by the child BP. Agent correctly pivoted to `apply_plan_json` on UserConstructionScript, which worked perfectly.

---

### 5. Timeout Behavior

**Progressive timeout did NOT trigger.** There were no auto-continue events. The run completed in a single pass (26:46). The longest thinking pause was ~90 seconds at the complexity spike around 06:00 (ToggleInventory design). This is well under the 120s nudge threshold.

**Directive continuation prompt was not needed** because the agent completed without timeouts. The empty-function-graph warning from `add_function` was not observed in the logs — this is expected since the agent's `apply_plan_json` calls followed shortly after each `add_function` call (within seconds to minutes).

---

### 6. New Issues / Regressions

**Issue 1: `ReceiveConstruct` event name (NEW)**
Agent on attempt 1 used `target="ReceiveConstruct"` as the Widget event name. This is the C++ function name. The Blueprint event name is `Construct`. The agent discovered this via trial and error (attempt 2 succeeded). This is a **knowledge gap** — Widget events have different Blueprint names from their C++ counterparts. Knowledge pack should note: Widget events are `Construct`, `PreConstruct`, `Tick`, `Destruct`.

**Issue 2: Array_Add wildcard type undetermined (RECURRENCE)**
Seen in run 09j/k as well. When `Array_Add` is used without its wildcard types being resolved by connected inputs at compile time, it fails. The specific failure: the plan executed Phase 4 data wiring correctly (Inventory → TargetArray, NewItemName → NewItem), but the compile still failed with "type undetermined." Root cause: the `Inventory` variable is a `TArray<FString>` but when `Array_Add` nodes are created and wired via plan_json, the wildcard type apparently doesn't propagate correctly before compile. The working fix was a simpler plan that omitted the conditional is_valid + AddSlot call in the same function (reducing complexity). The separately-working EventGraph version (line 3956–4005) called the local `AddToInventory` function rather than Array_Add directly, which avoided the issue entirely.

**Issue 3: FunctionEntry node ID unknown (RECURRING)**
The agent tried 4 consecutive connect_pins calls with semantic refs `{"semantic":"entry","pin":"then"}`, `{"node_id":"entry"}`, `{"node_id":"FunctionEntry"}`, `{"node_id":"AddToInventory"}` — all failed (line 3555: "node_0: AddToInventory [K2Node_FunctionEntry]"). The correct approach is to read the graph first to get the actual node ID (`node_0` in this case), or use plan_json's auto-chain behavior (which DOES work — see line 3456: "Auto-chain: orphan step 'add_item' found, wiring from function entry"). This is a **tool ergonomics gap** — `connect_pins` has no semantic-aware entry-node lookup. The agent should use plan_json instead of granular wiring for function entry connections.

**Issue 4: `CallFunction` node type name for self-functions (NEW)**
Agent failed twice with `type="CallFunction"` and `type="Call Function"` when trying to add a call to a locally-defined function (`ToggleInventory`). The third attempt used `type="K2Node_CallFunction"` which succeeded. This is inconsistent behavior — the agent should not need to know the internal node class name. Knowledge pack or tool error message should clarify that self-function calls via add_node require `type="K2Node_CallFunction"`.

**Issue 5: `modify_only` on child BP inherited variables (EXPECTED — not a regression)**
6 failures were correct tool rejections. The agent correctly understood the error and pivoted to UserConstructionScript. No fix needed — behavior is correct.

**Issue 6: `project.snapshot` still fails on pre-existing BPs (KNOWN)**
Line 1844–1847: `LogOliveSnapshot: Warning: No read tool available for asset: /Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter`. Known issue from prior runs.

**Issue 7: SetVisibility target type mismatch for InventoryWidget**
Agent tried to connect `InventoryWidget` (WBP_InventoryGrid_C*) to `SetVisibility.self` (Widget*). This is a type hierarchy issue — UWidget is the base but the pin expects strict type match. The `ToggleInventory` function plan correctly used cast targets, which resolved both `AddToViewport` and `RemoveFromParent` via `WBP_InventoryGrid_C`. The agent learned this pattern but wasted ~2 minutes on the granular approach first.

---

### 7. Breadth vs Depth

**Depth-first: CONFIRMED.** The agent followed depth-first ordering:
1. BP_PickupItem (create → components → variables → compile) — **complete**
2. WBP_InventorySlot (create → widget tree → variables → functions → logic → bindings) — **complete**
3. WBP_InventoryGrid (create → widget tree → AddSlot logic) — **complete**
4. BP_ThirdPersonCharacter (add component → add variables → add functions → EventGraph logic → ToggleInventory logic) — **complete**
5. Child BPs (create → default values) — **complete**

No breadth-first thrashing observed. This is a marked improvement from the bow-arrow runs where the agent would touch multiple BPs superficially before completing any one.

---

### 8. Specific Fix Verification

| Fix | Status | Evidence |
|---|---|---|
| CastTargetMap component inference | **CONFIRMED WORKING** | Line 3739: "AddToViewport via cast target 'WBP_InventoryGrid_C'" |
| add_function empty-graph warning | **NOT OBSERVED** — agent acted before warning would matter | No warning text in logs; agent followed up immediately with apply_plan_json |
| UNWIRED_REQUIRED_INPUT detection | **NOT TRIGGERED** | No UNWIRED log entries |
| Widget @ref resolution | **CONFIRMED** | Lines 2364–2365: synthesized `_synth_widget_itemnametext`, `_synth_widget_slotborder` |
| Progressive idle timeout | **NOT TRIGGERED** (run completed in single pass <120s idle gaps) | No auto-continue events |
| Directive continuation prompt | **NOT EXERCISED** | No timeouts, no continuation needed |
| Depth-first guidance | **CONFIRMED** | See section 7 |

---

### 9. Quantitative Summary

| Metric | Value |
|---|---|
| Total runs (initial + auto-continues) | **1** (no auto-continues) |
| Total tool calls | ~128 |
| Total LogOliveAI entries | 744 |
| Blueprints created | **7** (all with logic) |
| Functions created | **11** (add_function calls) |
| Functions with graph logic (apply_plan_json SUCCESS) | **9** |
| plan_json total / success / failed | 18 / 9 / 9 |
| plan_json success rate | **50%** |
| Overall tool success rate | **~86%** |
| Compile invocations / SUCCESS / FAILED | 15 / 10 / 5 |
| Final compile state | **ALL 7 BPs SUCCESS** |
| Total wall-clock time | **26:46** |
| Auto-continue count | **0** |
| FUNCTION_NOT_FOUND errors (plan_json) | 2 (both self-corrected with describe_function) |
| VARIABLE_NOT_FOUND errors (plan_json) | 1 (agent self-corrected by creating GetItemInfo) |
| Widcard type errors (compile) | 3 occurrences across 2 unique failures |
| Wrong event name (ReceiveConstruct) | 1 |

---

### 10. Recommendations

**Top 3 remaining issues to fix:**

**Recommendation 1 (HIGH): Alias map entry for Widget Blueprint event names**
The agent used `ReceiveConstruct` (C++ name) instead of `Construct` (Blueprint event name). This wasted one plan_json attempt plus thinking time. Widget event names differ from C++ names:
- `ReceiveConstruct` → `Construct`
- `ReceiveTick` → `Tick`
- `ReceiveDestruct` → `Destruct`
- `ReceiveNativeTick` (not exposed)

Fix: Add these to the alias map in `FindFunction`/`CreateEventNode` resolution, OR add a knowledge pack note in `events_vs_functions.txt` or `blueprint_authoring` about Widget event naming. Given that `ReceiveConstruct` is a UFUNCTION, adding it to the alias map so it resolves to `Construct` as a Blueprint event name is the cleanest fix.

**Recommendation 2 (HIGH): `connect_pins` semantic entry-node lookup OR better error message**
The agent wasted ~7 minutes trying 4 different node_id strings for the FunctionEntry node. The correct ID was `node_0` (visible from `blueprint.read`). Two fixes possible:
- **Option A**: Add semantic lookup in `connect_pins` so `source_ref={"semantic":"function_entry"}` resolves to `K2Node_FunctionEntry` in the current graph.
- **Option B**: When `connect_pins` fails because a node ID isn't found, include the available node list in the error response (currently logged at Debug level but not returned to agent). The log shows: "Available nodes: node_0: AddToInventory [K2Node_FunctionEntry], node_1: Add [K2Node_CallFunction]..."

Option B is simpler and already implemented in the log — it just needs to be surfaced in the tool result JSON. Option A is better long-term. Both would have avoided 4 failures and ~3-4 minutes of recovery.

**Recommendation 3 (MEDIUM): Add CallFunction/K2Node_CallFunction type normalization in add_node**
The agent failed twice with `type="CallFunction"` and `type="Call Function"` before succeeding with `type="K2Node_CallFunction"`. The `add_node` tool's type lookup should normalize `"CallFunction"` → `"K2Node_CallFunction"`. This is a known pattern from prior runs (bow-arrow runs had similar type name confusion). The tool already handles some aliases but `CallFunction` is not among them.

**Additional recommendations:**

**Recommendation 4 (LOW): Array_Add wildcard type propagation diagnostic**
The `Array_Add` wildcard type failure is subtle — Phase 4 data wiring reports success but compile still fails. When a `call` op targets `Array_Add` and the input is an `@get_inv.auto` (array variable), the resolver should add a warning: "Array_Add uses wildcard type — ensure TargetArray and NewItem are wired before compile." The fundamental issue may be that wildcard `Array_Add` needs `PROPAGATE_TYPE_TO_PINS` to be called after all connections, which may not be happening in the plan executor. Worth a deeper look at whether `ReconstructNode()` is needed after Array wildcard wiring.

**Recommendation 5 (LOW): `modify_only` on child BP should return clearer message**
Current failure message is terse. Agent correctly pivoted to UserConstructionScript, but a clearer message ("Variables inherited from parent class cannot be modified via add_variable. Use apply_plan_json on UserConstructionScript to set defaults.") would save one round-trip.

---

## Summary

This was the best inventory-system run to date. The agent:
- Created 7 Blueprints with full functional logic, all compiling successfully
- Demonstrated correct depth-first ordering (major improvement)
- Used CastTargetMap inference successfully for AddToViewport resolution
- Self-corrected FUNCTION_NOT_FOUND with describe_function (2×) and VARIABLE_NOT_FOUND by creating GetItemInfo
- Completed in a single run with no auto-continues (26:46 total)

The 50% plan_json success rate is lower than the best bow-arrow runs (77%), but the task complexity is higher (Widget blueprints, property bindings, multi-BP orchestration). The failures were all recoverable and the agent recovered from all of them correctly. The dominant remaining issues are: Widget event name aliasing, FunctionEntry node ID lookup in connect_pins, and CallFunction type normalization in add_node.

Sources:
- Log file: `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/docs/logs/UE_Olive_AI_Toolkit-backup-2026.03.12-06.17.09.log`
- OliveAI log entries: lines 1553–5255
- Plan execution logs: lines 2203–4824
