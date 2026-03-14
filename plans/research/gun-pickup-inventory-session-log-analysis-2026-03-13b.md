# Research: Session Log Analysis — Gun+Pickup+Inventory Run (2026-03-13b)

## Question

Analyze the full 6547-line session log from 2026-03-13 19:28 covering two autonomous Claude Code runs. Document
what was built, which recently-deployed fixes held, what new failures occurred (with exact line numbers and root
causes), tool call efficiency, and self-correction behavior. Diagnosis only — no code changes.

---

## Findings

### Run Overview

**Run 1** — Gun + Bullet + Pickup system
- Launched: line 1820 (22:31:13), completed: line 3252 (22:40:45)
- Duration: ~9 min 32 sec
- Tool calls: 50 (confirmed line 3252)
- Discovery pass: 5 queries, 8 community results, 19.9 sec (lines 1817–1818)
- Templates consulted: `get_template fps_shooter_bp_character_base` (twice — once with wrong `"id"` key, once correct), `list_templates` (gun/bullet queries)
- Recipes consulted: 1 (`olive.get_recipe` line 1834)
- Assets created: BP_Bullet (Actor, line 1852+), BP_Gun (Actor, line 2099+), modifications to BP_ThirdPersonCharacter
- Final compile: BP_ThirdPersonCharacter SUCCESS at line 3247 (33.87 ms). BP_Gun and BP_Bullet compiled successfully during run.
- Exit code 0, outcome=0

**Run 2** — Item hierarchy + Inventory UI
- Launched: line 3430 (22:54+ after user re-opened chat), completed: line 6225 (23:24:16)
- Duration: ~29 min (including 1 auto-continue)
- Tool calls: ~100+ total (29 in auto-continue segment alone, line 6225)
- Discovery pass: multiple `olive.search_community_blueprints` calls before first blueprint.create
- Auto-continue: 1 occurrence at line 5441 (240-second idle timeout)
- Assets targeted: BP_Item (Actor), BP_Gun (reparent to BP_Item — abandoned), WBP_InventorySlot (UserWidget), WBP_InventoryGrid (UserWidget), BP_ThirdPersonCharacter modifications
- Final compile: all 4 BPs compiled SUCCESS (lines 6196–6223)
- Exit code 0, outcome=0

---

### Fix Verification Results

#### Fix 1: Undraggable Pins After ReconstructNode
**Status: CONFIRMED FIXED**

Zero undraggable-pin reports across all 6547 lines. The `NotifyGraphChanged()` call added after `ReconstructNode()` during pin wiring resolved this completely. No regressions observed.

#### Fix 2: Native C++ CDO Component Reading
**Status: CONFIRMED WORKING**

At line 3759, `blueprint.read` on BP_ThirdPersonCharacter returned 6 components including C++ inherited ones
(CapsuleComponent, CharacterMovementComponent, Mesh, etc.). The agent used `get_var "Mesh"` successfully in multiple
plans (e.g., line 2949 — `get_var Mesh` as Parent for `K2_AttachToComponent`). `blueprint.read` component count
was accurate throughout Run 2 as well.

Note: `FindFunction("GetMesh")` still fails (the function-call side was not part of this fix). The agent worked
around it correctly by using `get_var "Mesh"` instead of calling `GetMesh()`.

#### Fix 3: Component-to-Component Attachment
**Status: CONFIRMED FIXED**

The `EquipGun` function in Run 1 correctly wired `K2_AttachToComponent` with `Target=@Gun.auto` (the spawned
gun actor) and `Parent=@get_mesh.auto` (the character's Mesh component), with socket name `hand_r`. This was the
critical prior regression. Lines 2924–2968 show it working as expected.

#### Fix 4: Discovery-First Workflow
**Status: CONFIRMED WORKING**

Both runs performed a full discovery pass before any `blueprint.create` call. Run 1 used `olive.search_community_blueprints` with 5 queries (lines 1810–1818). Run 2 also searched community BPs before starting. The agent consulted templates and recipes before making any writes.

#### Fix 5: plan_json Error Redirect
**Status: NOT CLEARLY EXERCISED AS A NEW MESSAGE**

The redirect mechanism was in place but the log doesn't show a case where an agent received the restructured
"FUNCTION_NOT_FOUND" error message and changed its approach because of the new format. No regressions observed —
the agent self-corrected on function not-found failures through normal retry.

---

### Issue 1: get_template Wrong Parameter Key (self-corrected)

**Lines:** 1838–1845
**What happened:** Agent called `blueprint.get_template` with `"id":"fps_shooter_bp_character_base"` instead of
`"template_id":"fps_shooter_bp_character_base"`. Tool returned FAILED (line 1841). Agent waited ~28 seconds,
then retried with the correct `"template_id"` key and succeeded (line 1844).

Second occurrence in Run 2 at lines 3543–3544 (similar pattern).

**Root cause:** Schema mismatch. The tool parameter is `template_id` but `id` is a natural alias an LLM reaches for.
The tool schema does not accept `id` as an alias.

**Classification:** Knowledge gap. Not a tool bug — `NormalizeToolParams` handles common aliases but `id→template_id`
is not currently in the alias table.

**Suggested fix:** Add `"id"` as an alias for `"template_id"` in `NormalizeToolParams()` in OliveToolRegistry.cpp,
analogous to how `path` is aliased from `asset_path`.

---

### Issue 2: blueprint.set_parent_class DefaultSceneRoot Conflict

**Lines:** 3645–3813
**What happened:** Agent attempted to make BP_Gun a child of BP_Item (newly created Actor) by calling
`blueprint.set_parent_class`. Three attempts all failed with UE internal compiler ensures and errors about
DefaultSceneRoot and component property references.

Error pattern from log lines ~3650–3680: compiler errors about duplicate DefaultSceneRoot, invalid component
references, and broken SCS nodes. Each attempt triggered `blueprint.compile` → FAILED with multiple errors
about component hierarchy corruption.

**Root cause:** When both BP_Item and BP_Gun are Actor subclasses, each has an auto-generated DefaultSceneRoot
SCS node. Reparenting BP_Gun under BP_Item causes two conflicting DefaultSceneRoot objects in the same class
hierarchy. UE's Kismet compiler does not handle this gracefully — it produces internal ensures.

**Classification:** Tool limitation. `blueprint.set_parent_class` has no guard for this Actor→Actor reparenting case.

**Agent recovery:** After 3 failed attempts (~9 minutes wasted), the agent abandoned the BP_Gun-as-child-of-BP_Item
design. It kept both as independent Actors and instead created a `GetItemName()` pure function on BP_Item to allow
ThirdPersonCharacter to access item data via function calls. This was a correct architectural recovery.

**Suggested fix:** Add a pre-flight check in the `set_parent_class` handler: if new parent is `AActor` (or
Actor-derived) and target BP already has a DefaultSceneRoot SCS node, return a structured error:
`"PARENT_CLASS_CONFLICT: Cannot reparent Actor BP to another Actor BP directly. Both have DefaultSceneRoot.
Consider using composition (add_component) or a shared interface instead."` Save the user ~9 minutes of compiler
chaos.

---

### Issue 3: modify_component Fails Immediately After add_component

**Lines:** 4034–4092
**What happened:** After removing and re-adding GunMesh, MuzzlePoint, and PickupSphere via `blueprint.add_component`,
all three subsequent `blueprint.modify_component` calls returned "Component not found." (lines 4034–4055). Agent
compiled BP_Gun (line 4056–4065: SUCCESS), then retried `modify_component` — all three succeeded immediately
(lines 4066–4092).

**Root cause:** `blueprint.modify_component` looks up SCS nodes by name in the BP's SCS. Newly added SCS nodes
are not indexed until after a compile. The node exists in the SCS array but the name lookup in the tool handler
fails before compilation updates internal state.

**Classification:** Tool timing issue. The tool layer indexes SCS nodes from a state that isn't current until
compilation.

**Agent recovery:** Empirical — agent inferred "compile first" from the error and got unblocked. This cost one
compile cycle (~500 ms) and one retry round.

**Suggested fix:** Either (a) make the `modify_component` handler search the raw SCS `AllNodes` array directly
(bypassing the compiled-state index), or (b) add to the `modify_component` error message:
`"COMPONENT_NOT_FOUND_PRE_COMPILE: If you just added this component, compile the Blueprint first then retry."`

---

### Issue 4: Array_Add Wildcard Type Undetermined (4 failures)

**Lines:** 4421–4599, 4700–4740
**What happened:** Four consecutive attempts to use Array operations on an inventory array all failed with:
`"The type of Target Array is undetermined. Connect something to Add to imply a specific type."`

Attempt 1 (lines 4421–4460): `blueprint.apply_plan_json` with Array_Add step — executor connected the array
variable to the Array_Add node's TargetArray pin but UE reported the type as still undetermined.

Attempts 2–3 (lines 4461–4550): Granular `add_node type="K2Node_CallArrayFunction"` + `connect_pins` —
same result. Pin connected but wildcard not resolved.

Attempt 4 (lines 4551–4599): Different approach with `set_var` on the array element — also failed.

Secondary cluster (lines 4700–4740): One more attempt with `add_node + connect_pins` on Array_Add before the
agent gave up.

**Root cause:** `Array_Add` is `UK2Node_CallArrayFunction`, which uses wildcard pins. The Blueprint editor
resolves the wildcard automatically by calling `UK2Node_CallArrayFunction::ReconstructNode()` after pin connection.
`TryCreateConnection()` in the tool layer connects the pin but does NOT call `ReconstructNode()` on the target
node. Without `ReconstructNode()`, UE's compiler sees an untyped wildcard and refuses to compile.

This is not a wiring failure — the pin IS connected. The missing step is the post-connection `ReconstructNode()`
call that propagates the concrete type through the wildcard.

**Classification:** Tool layer bug. `FOlivePinConnector::Connect()` calls `TryCreateConnection()` which does not
trigger wildcard reconstruction on the target node. The Blueprint editor handles this via
`SGraphEditor::OnPinConnectionListChanged → UK2Node::PinConnectionListChanged → ReconstructNode()`.

**Agent recovery:** The agent made a significant architectural pivot. After the 4th failure it redesigned:
- Added `AddSlot(ItemRef)` function to WBP_InventoryGrid (lines 4847–4900) which handles array logic internally
- Added `InitSlot(ItemRef)` function to WBP_InventorySlot (lines 4901–4960)
- Modified ThirdPersonCharacter's `AddItemToInventory` to call `AddSlot` on the widget instead of directly
  manipulating an array (lines 5000–5063)

This workaround bypassed Array_Add entirely by moving array management into widget functions. Time wasted: ~12
minutes.

**Suggested fix:** In `FOlivePinConnector::Connect()` (or `FOlivePlanExecutor::PhaseWireData()`), after calling
`TryCreateConnection()`, check if the target node is a `UK2Node_CallArrayFunction`. If so, call
`TargetNode->ReconstructNode()` to propagate the concrete type. The correct call is:
```cpp
if (UK2Node_CallArrayFunction* ArrayNode = Cast<UK2Node_CallArrayFunction>(TargetPin->GetOwningNode()))
{
    ArrayNode->ReconstructNode();
}
```
More broadly: after any successful pin connection in PhaseWireData, if `TargetPin->GetOwningNode()` is a wildcard
node (`PinConnectionListChanged` needs calling), call `ReconstructNode()`. The safest approach is checking
`TargetPin->GetOwningNode()->ShouldShowNodeProperties()` or the wildcard pin flag.

---

### Issue 5: SetVisibility Alias Collision (Widget vs SceneComponent)

**Lines:** 5148–5259, 5572–5672
**What happened:** Plans using `call SetVisibility` resolved to `SceneComponent::SetVisibility` via the alias map.
The executor tried to set pin `InVisibility` (which the agent intended for `UWidget::SetVisibility`) but
`SceneComponent::SetVisibility` uses pin name `bNewVisibility` — pin not found → rollback.

Log evidence (lines ~5155–5160):
```
alias-resolved 'SetVisibility' -> 'SceneComponent::SetVisibility' but step has unmatched input pins: [InVisibility].
Attempting fallback search... found no match. Keeping alias result.
```

Second cluster at lines 5572–5672: same failure, same root cause.

**Agent recovery:** Added `"target_class":"UserWidget"` to both `SetVisibility` call steps. Resolver then
bypassed the alias and searched UserWidget directly, finding `UWidget::SetVisibility` with pin `InVisibility`.
Succeeded after the fix.

**Root cause:** The alias map entry maps `SetVisibility` → `SceneComponent::SetVisibility`. This is correct for
component visibility but wrong for widget visibility. The fallback search in the resolver tries to find a better
match when input pins don't match the alias target, but it searches without a pin-compatibility constraint and
finds nothing better.

**Classification:** Alias map specificity failure. The resolver's fallback search is not pin-aware — it doesn't
reject candidates whose pin names don't match the requested inputs.

**Suggested fix (option A):** Remove `SetVisibility` from the alias map entirely. It is ambiguous between
`SceneComponent::SetVisibility(bNewVisibility)` and `UWidget::SetVisibility(InVisibility)`. Without the alias,
FindFunction's universal library scan finds the correct one based on `target_class`.

**Suggested fix (option B):** Make the resolver fallback search pin-aware. When the alias result's pins don't
match `Step.Inputs`, reject it and continue searching with pin-name as a secondary filter.

**Knowledge fix:** Add to `cli_blueprint.txt` or `Worker_Blueprint.txt`:
`"Always specify target_class when calling SetVisibility — the default resolves to SceneComponent version
(pin: bNewVisibility). Widget visibility uses pin 'InVisibility', requires target_class='UserWidget'."`

---

### Issue 6: VARIABLE_NOT_FOUND for Cross-Blueprint get_var (3 failures)

**Lines:** 5260–5428
**What happened:** Plans targeting ThirdPersonCharacter's EventGraph tried `get_var ItemName` to read the item
name after casting a pickup to BP_Item. Phase 0 validator rejected with:
`"VARIABLE_NOT_FOUND -- step 'get_name' references 'ItemName' on 'BP_ThirdPersonCharacter'"`

The agent tried adding `"inputs":{"self":"@cast_item.auto"}` and `"inputs":{"Target":"@cast_item.auto"}` to
the `get_var` step. Both rejected — Phase 0 `VARIABLE_NOT_FOUND` check does not use `CastTargetMap` for
variable lookup; it checks only the BP being written to (ThirdPersonCharacter).

Three distinct plans failed (lines 5260–5428), each wasting 2–4 minutes.

**Root cause:** `VARIABLE_NOT_FOUND` is correct behavior. `ItemName` lives on BP_Item, not BP_ThirdPersonCharacter.
The agent's plan design was wrong: `get_var` in plan_json only works for variables on the current Blueprint.
To read a variable from a cast result of a different Blueprint, you need either an accessor function call or
direct property access via `K2Node_VariableGet` with the variable's owning class — and plan_json's `get_var`
op doesn't support foreign classes.

**Classification:** Agent knowledge gap. The agent didn't know that `get_var` is scoped to the current BP.
Phase 0 correctly blocks this.

**Agent recovery (lines 5991–6095):**
1. Added `GetItemName()` pure function to BP_Item (returns ItemName string) — lines 5991–6050
2. Used `add_node type="K2Node_CallFunction" properties={"function_name":"GetItemName","target_class":"BP_Item_C"}`
   + `connect_pins` to wire the cast result as Target and the return string to the `AddItemToInventory` parameter
3. Worked correctly — compile SUCCESS

Time wasted: ~12 minutes.

**Suggested fix:** Add to `cli_blueprint.txt` under "Common Mistakes":
```
WRONG: get_var "ItemName" in ThirdPersonCharacter plan after casting to BP_Item
  WHY WRONG: get_var only reads variables on the current Blueprint's class.
             ItemName lives on BP_Item, not ThirdPersonCharacter.
RIGHT: Add a pure getter function (GetItemName) to BP_Item, then call it with the cast result as Target.
       Or use K2Node_CallFunction with target_class="BP_Item_C" to call the getter.
```

**Phase 0 enhancement (optional):** When `VARIABLE_NOT_FOUND` fires on a step whose inputs include a `@cast_N.auto`
reference, add to the error message: `"If 'ItemName' is a variable on the cast target class, you cannot access
it via get_var. Add a pure getter function to that class and call it with the cast result as Target."` This
would save the agent the 3 retry cycles.

---

### Issue 7: add_node type="CallFunction" Fails for Self-Blueprint Functions

**Lines:** 5787–5833
**What happened:** `add_node type="CallFunction" properties={"function_name":"ToggleInventory"}` failed with
`"ToggleInventory not found... Closest matches: ToggleActive, ToggleVisibility"` (line 5787–5788). The function
`ToggleInventory` exists on BP_ThirdPersonCharacter.

Agent then used `add_node type="K2Node_CallFunction" properties={"function_name":"ToggleInventory"}` (the
CreateNodeByClass path, line 5816). This path sets `FunctionReference.SetSelfMember("ToggleInventory")` which
correctly looks up the function on the current Blueprint. Succeeded (line 5833).

**Root cause:** `add_node type="CallFunction"` routes through the standard `FindFunction()` pipeline. At the
time of the call, FindFunction searches: alias map → specified class → BP GeneratedClass → parent hierarchy →
SCS components → interfaces → library classes → universal UBlueprintFunctionLibrary scan. For a function
defined in `FunctionGraphs` on the current Blueprint, the `GeneratedClass` search step should find it — but
the function may not yet be on `GeneratedClass` if the BP hasn't been compiled after `add_function` added it.

**Classification:** Timing issue. After `add_function` creates a new function graph, the function does not
appear on `GeneratedClass` until after a compile. `FindFunction()` step 3 checks `GeneratedClass +
FunctionGraphs` — FunctionGraphs are checked as a fallback, but self-functions created in the current session
may not be on the compiled class yet.

**Agent recovery:** Used `type="K2Node_CallFunction"` which bypasses FindFunction and uses SetSelfMember.
Correct workaround.

**Suggested fix:** In `FindFunction()` step 3, when searching the current Blueprint's `FunctionGraphs`, also
check if a graph name matches the function name directly (i.e., check `Blueprint->FunctionGraphs` by graph name,
not just by iterating compiled functions). This would catch functions added in the current session but not yet
compiled.

---

### Issue 8: VariableGet with variable_class Creates 0-Pin Ghost Node

**Lines:** 5965–5984
**What happened:** `add_node type="VariableGet" properties={"variable_name":"ItemName","variable_class":"BP_Item_C"}`
created a node with 0 pins. Log warning (line ~5975): `"RecreatePinForVariable: 'ItemName' pin not found"`

**Root cause:** `CreateNodeByClass` for `UK2Node_VariableGet` calls `VariableReference.SetSelfMember("ItemName")`.
`SetSelfMember` marks the reference as self-context — it looks for `ItemName` on the current Blueprint
(ThirdPersonCharacter). Since ItemName is on BP_Item, not ThirdPersonCharacter, pin creation fails. The
`variable_class` property is ignored by the current CreateNodeByClass path.

**Classification:** Tool limitation. The zero-pin ghost node is the same class of bug as the `CreateNodeByClass`
ghost node fix from prior sessions, but for a different trigger: foreign-class variable references.

**Agent recovery:** Abandoned this approach and used `K2Node_CallFunction(GetItemName)` instead.

**Suggested fix (if foreign-class VariableGet support is desired):** In `CreateNodeByClass`, for
`UK2Node_VariableGet` and `UK2Node_VariableSet`, check if `variable_class` is provided and different from the
current Blueprint. If so, use `SetExternalMember(FName(variable_name), ForeignClass)` instead of `SetSelfMember`.
If not desired, add a guard that returns a clear error: `"VariableGet can only read variables on the current
Blueprint class. Use a getter function on the foreign class instead."`

---

### Issue 9: widget.set_property — FMargin Format Not Documented

**Lines:** 4260–4278
**What happened:** `widget.set_property` with `Padding="16"` failed with:
`"ImportText (Padding): Missing opening parenthesis: 16"`

Agent did not retry. The padding properties were skipped.

**Root cause:** FMargin requires tuple syntax: `"(16,16,16,16)"` or `"(All=16)"`. A plain scalar is not valid.
The tool schema does not document the FMargin format.

**Classification:** Documentation gap. The tool works correctly but the required format is undocumented.

**Suggested fix:** Add to the `widget.set_property` tool description or schema hints:
`"Struct properties like Padding require tuple format: '(16,16,16,16)' for (Left,Top,Right,Bottom) or
'(All=16)'. Simple scalars are not valid for FMargin properties."`

---

### Issue 10: widget.set_property — Dotted Sub-Property Path Not Supported

**Lines:** 4246–4249
**What happened:** `widget.set_property` with property path `Font.Size` failed with:
`"Property 'Font.Size' not found on widget class 'TextBlock'"`

Agent did not retry.

**Root cause:** The `widget.set_property` handler uses a flat property name lookup; it does not walk nested struct
members via dot notation. `Font` is a `FSlateFontInfo` struct property on TextBlock, and `Size` is a field within
that struct.

**Classification:** Tool limitation. Dotted paths are not supported.

**Suggested fix:** Either (a) implement sub-property path walking (split on `.`, find the struct property, use
`FProperty::ImportText_Direct` on the sub-field), or (b) document the limitation in the tool schema and suggest
using `editor.run_python` for sub-struct property editing.

---

### Self-Correction Behavior

**Strong recovery on architectural problems:**
- BP_Item/BP_Gun inheritance failure (Issue 2): agent tried 3 times then correctly pivoted to composition +
  accessor functions. Final design was architecturally sound.
- Array_Add wildcard (Issue 4): after 4 failures agent redesigned the entire inventory system to avoid Array_Add.
  The resulting design (widget-level AddSlot functions) is more encapsulated than the original plan.
- VARIABLE_NOT_FOUND (Issue 6): after 3 failures agent added GetItemName() to BP_Item. Correct UE pattern.

**Quick self-correction on parameter errors:**
- `get_template "id"` → `"template_id"` (1 retry, ~28 sec)
- `SetVisibility` without `target_class` → added `"target_class":"UserWidget"` (1 retry, ~3 min)
- `add_node CallFunction` → `K2Node_CallFunction` (1 retry, ~2 min)

**Patterns not self-corrected:**
- FMargin scalar format (Issue 9): silently skipped — not retried
- Font.Size dotted path (Issue 10): silently skipped — not retried
- These two were both widget styling properties; agent deprioritized them and moved on

**Auto-continue behavior (line 5441):** One 240-second idle timeout triggered. The auto-continue nudge
successfully resumed the run — the agent completed all remaining BPs and achieved final SUCCESS. No second
auto-continue was needed.

---

### Tool Call Efficiency

**Run 1 (50 tool calls, 9:32 min):**
- Roughly 5.3 calls/min
- Discovery + template/recipe: ~8 calls (16%)
- Blueprint reads: ~10 calls (20%)
- Plan writes (plan_json preview+apply): ~12 calls (24%)
- Compiles: ~8 calls (16%)
- Granular tools (add_node, connect_pins, set_pin_default): ~8 calls (16%)
- Miscellaneous (project.snapshot, project.search): ~4 calls (8%)

**Run 2 (~100+ tool calls, ~29 min):**
- Wasted calls from known failures:
  - Issue 2 (set_parent_class): ~18 calls wasted (~9 min)
  - Issue 4 (Array_Add): ~20 calls wasted (~12 min)
  - Issue 6 (VARIABLE_NOT_FOUND): ~15 calls wasted (~12 min)
  - Issue 5 (SetVisibility): ~6 calls wasted (~3 min)
  - Issue 7 (CallFunction): ~2 calls wasted (~2 min)
- Total wasted: ~61 calls out of ~100 = ~61% waste rate due to known tool/knowledge gaps

If all five issues had been fixed, Run 2 would have been roughly 40–45 minutes shorter.

---

### Positive Observations Not Previously Noted

1. **PreResolvedFunction** working silently throughout — no double-FindFunction log noise
2. **CleanupStaleEventChains** firing correctly — no orphaned event chains from failed plan retries persisted
3. **bAutoCompile** (`blueprint.compile` after plan_json) — agent correctly compiled after each plan batch
4. **K2_AttachToComponent with socket** — worked first time with correct `hand_r` socket name (line ~2955)
5. **blueprint.read on ACharacter** returns accurate component list — C++ CDO fix confirmed working
6. **WBP_InventoryGrid AddSlot design** — agent produced a clean, UE-idiomatic widget component API as its
   workaround for the Array_Add failure. Better design than the original plan.
7. **Three PIE sessions** ran without crash post-run (lines 6232–6413) — no runtime errors from agent-generated BPs

---

## Recommendations

1. **Array_Add wildcard reconstruction (Priority: HIGH)** — Add `UK2Node_CallArrayFunction` detection in
   `FOlivePinConnector::Connect()` or `FOlivePlanExecutor::PhaseWireData()`. After successful connection to any
   array node's TargetArray pin, call `TargetNode->ReconstructNode()`. This single fix eliminates the dominant
   failure pattern in any task involving arrays. Estimated savings: ~12 min per run.

2. **set_parent_class Actor→Actor guard (Priority: HIGH)** — In the `set_parent_class` tool handler, check if
   both the target BP and the proposed parent are Actor-derived with DefaultSceneRoot SCS nodes. If so, return a
   structured error before attempting reparenting. Prevents ~9 min of compiler chaos. Include a suggestion about
   interfaces or composition.

3. **SetVisibility alias removal or pin-aware fallback (Priority: MEDIUM)** — Either remove `SetVisibility` from
   the alias map (ambiguous between SceneComponent and Widget versions) or make the resolver's alias-override
   fallback pin-name-aware. Add knowledge note about always specifying `target_class` for `SetVisibility`. Saves
   ~3 min per occurrence.

4. **get_var cross-Blueprint error guidance (Priority: MEDIUM)** — When `VARIABLE_NOT_FOUND` fires on a step
   whose inputs reference a cast result, append guidance: "If the variable is on the cast target class, add a pure
   getter function to that class and call it with Target=@cast_N.auto". Add to `cli_blueprint.txt` Common Mistakes.
   Saves ~12 min per occurrence.

5. **modify_component pre-compile error message (Priority: MEDIUM)** — When `modify_component` returns
   "Component not found" for a component that was just added, append: "If you just added this component via
   add_component, compile the Blueprint first, then retry." Saves 1–2 retry cycles.

6. **template_id alias in NormalizeToolParams (Priority: LOW)** — Add `"id"` → `"template_id"` alias mapping in
   `NormalizeToolParams()`. Saves one 28-second retry per occurrence of the LLM using the natural name.

7. **widget.set_property FMargin documentation (Priority: LOW)** — Document FMargin tuple format requirement in
   the tool schema. Add to error message: "Struct properties require tuple format, e.g., '(16,16,16,16)'".
   Prevents silently skipped styling properties.

8. **FindFunction self-BP function search (Priority: LOW)** — In FindFunction step 3, search `Blueprint->FunctionGraphs`
   by graph name in addition to the compiled GeneratedClass. This covers functions added in the current session
   before the next compile.

9. **K2Node_VariableGet foreign class guard (Priority: LOW)** — In `CreateNodeByClass` for `UK2Node_VariableGet`,
   if `variable_class` refers to a class other than the current BP, return a clear error instead of producing a
   0-pin ghost node.

10. **Run efficiency note** — Run 1 (50 calls, 9:32) is the best efficiency baseline observed. Run 2 was dragged
    to ~100 calls by 5 distinct known gaps. Fixing items 1 and 2 alone would recover ~21 minutes from Run 2.
    The agent's architectural recovery decisions (AddSlot function, GetItemName function) were sound — these
    workarounds are superior designs that should be documented as canonical patterns in reference templates.

Source: `docs/logs/UE_Olive_AI_Toolkit.log` (lines 1–6546, 2026-03-13)
