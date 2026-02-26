# Autonomous Mode Efficiency: Prompt Fixes + Defensive Auto-Pin Resolution

## Context

A log analysis of a "create a gun blueprint that shoots bullets" task revealed the AI took **10 minutes, 80+ tool calls, and never finished**. Root cause investigation identified three distinct failures and one systemic issue:

- **Error 1 (duplicate BeginPlay):** AI used `create_from_template` then tried to add a BeginPlay event that the template already created — because it never read the blueprint after creation.
- **Error 2 (invented node type "FunctionInput"):** AI wrote plan_json targeting a function graph without calling `get_recipe` to learn the correct vocabulary for function entry points.
- **Error 3 (auto pin resolution failure):** `apply_plan_json` failed with "Source pin 'auto' not found on node 'node_10'" because the AI sent `schema_version: "1.0"` (v1 lowerer path has no `auto` resolution). Root cause: **AGENTS.md line 30 shows a `"schema_version":"1.0"` example** — the only concrete plan_json example the AI sees in autonomous mode.
- **Systemic: unguided thrashing** — when plan_json failed, the AI fell back to individual `add_node`/`connect_pins` calls with no recovery strategy, burning 50+ calls doing manual wiring.

The v2.0 executor (`FOlivePlanExecutor`) already handles `auto` pin resolution correctly via `FindTypeCompatibleOutput()`. The AI just never uses v2.0 because the example it pattern-matches on says v1.0.

**Log file:** `docs/logs/UE_Olive_AI_Toolkit.log`

---

## Changes

### 1. AGENTS.md — Fix schema version + add workflow rules

**File:** `AGENTS.md`

**1a. Fix schema_version example (line 30)**

Change:
```json
{"schema_version":"1.0","steps":[
```
To:
```json
{"schema_version":"2.0","steps":[
```

This is the root cause of error 3. The AI pattern-matches on this example and sends v1.0, which takes the lowerer path that can't resolve `auto` pins. The v2.0 executor already handles this correctly.

**1b. Add "read after create_from_template" rule**

After line 15 (the `create_from_template` workflow step), add:
```
3. After creation, `blueprint.read` the result to see what the template already built before adding more logic.
```

Prevents error 1 (duplicate BeginPlay). The auto-read-mode improvement (discussed but deferred) would also help here.

**1c. Add recipe guidance to routing**

After line 21 (the "Small edit" workflow), add a line:
```
**Unfamiliar pattern:** Call `olive.get_recipe` before writing plan_json to look up the correct approach.
```

Prevents error 2 (invented node types).

**1d. Add fallback strategy to Important Rules**

After line 73 (the "Don't re-preview unchanged plans" rule), add:
```
- **If apply_plan_json fails:** re-read the graph, fix the plan based on the error, and retry once. If it fails a second time, fall back to step-by-step `add_node`/`connect_pins`.
```

Prevents unguided thrashing. The retry-once-then-fallback pattern gives one shot at self-correction while maintaining an escape hatch for genuine UE quirks.

**1e. Soften the "NEVER use add_node" rule (line 76)**

Change:
```
- **Never use add_node one-at-a-time for 3+ nodes** -- use plan_json instead.
```
To:
```
- **Prefer plan_json for 3+ nodes.** Only fall back to add_node/connect_pins after plan_json has failed twice.
```

The absolute "NEVER" contradicts the fallback rule in 1d.

---

### 2. SetupAutonomousSandbox hardcoded section — Safety net

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`
**Function:** `SetupAutonomousSandbox()` (line 221)
**Target:** The hardcoded Critical Rules section (lines 260-267)

Add three lines to the Critical Rules block (after line 267, before the `---` separator):

```cpp
ClaudeMd += TEXT("- Use schema_version \"2.0\" for all plan_json calls (v2.0 has automatic pin resolution).\n");
ClaudeMd += TEXT("- After `create_from_template`, always `blueprint.read` the result before modifying it.\n");
ClaudeMd += TEXT("- If `apply_plan_json` fails, re-read the graph, fix the plan, and retry once. Fall back to add_node/connect_pins only after a second failure.\n");
```

This is a safety net in case AGENTS.md is missing or stale. The AGENTS.md content is appended after this section (line 270-274), so both sources reinforce the same rules.

---

### 3. recipe_routing.txt — Fix contradiction

**File:** `Content/SystemPrompts/Knowledge/recipe_routing.txt`
**Line 9**

Change:
```
- NEVER use add_node one-at-a-time for 3+ nodes -- use plan_json
```
To:
```
- Prefer plan_json for 3+ nodes; fall back to add_node only after plan_json fails twice
```

Keeps the orchestrated (non-autonomous) path consistent with AGENTS.md.

---

### 4. OliveGraphWriter.cpp — Defensive auto pin resolution

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp`
**Function:** `ConnectPins()` (line 479)

After `FindPin(SourceNode, SourcePinName)` returns nullptr (line 479-480), add auto-resolution fallback before the error return:

```cpp
UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName);
if (!SourcePin && SourcePinName == TEXT("auto"))
{
    // Resolve "auto" to first non-exec output pin (defensive fallback for v1.0 lowerer path)
    for (UEdGraphPin* TestPin : SourceNode->Pins)
    {
        if (TestPin && TestPin->Direction == EGPD_Output
            && TestPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
            && !TestPin->bHidden)
        {
            SourcePin = TestPin;
            UE_LOG(LogOliveGraphWriter, Log,
                TEXT("Auto-resolved source pin 'auto' on node '%s' to '%s'"),
                *SourceNodeId, *TestPin->GetName());
            break;
        }
    }
}
if (!SourcePin)
{
    return FOliveBlueprintWriteResult::Error(
        FString::Printf(TEXT("Source pin '%s' not found on node '%s'"), *SourcePinName, *SourceNodeId),
        BlueprintPath);
}
```

Apply the same pattern for the **target pin** at line 496-501 (using `EGPD_Input` instead of `EGPD_Output`).

With change #1a in place, the AI should send v2.0 plans and never hit the v1 lowerer path. This is purely defensive — if someone sends a v1.0 plan with `@step.auto` references, it won't hard-fail.

---

## Files Modified (summary)

| File | Change | Lines affected |
|------|--------|---------------|
| `AGENTS.md` | Fix v1.0→v2.0 example, add 3 workflow rules, soften add_node rule | ~5 lines changed/added |
| `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | Add 3 safety-net rules to hardcoded section | 3 lines added (~line 267) |
| `Content/SystemPrompts/Knowledge/recipe_routing.txt` | Soften add_node rule to match AGENTS.md | 1 line changed (line 9) |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp` | Auto pin resolution fallback in ConnectPins | ~25 lines added (~line 479 and ~496) |

---

## What This Does NOT Cover (Parked)

These items were discussed and explicitly deferred:

- **Self-correction policy in autonomous mode** — piping MCP tool results through `FOliveSelfCorrectionPolicy` requires touching `OliveMCPServer`, `OliveConversationManager`, and the autonomous flow. Prompt-based fallback gets 80% of the benefit. Revisit after measuring prompt impact.
- **Auto read mode** (`blueprint.read` auto-upgrading to full for small blueprints) — good optimization, cuts 5 round-trips per blueprint read, but it's infrastructure work beyond the scope of these targeted fixes.
- **Tool count reduction** (104 → ~30-40 per task) — real impact on thinking time, but requires rethinking tool pack gating for autonomous mode. Separate effort.
- **`get_node_pins` deprecation** — `read_event_graph` returns strictly more data. Fewer tools = less AI confusion. Separate cleanup task.

---

## Verification

1. **Build:** Run UBT to confirm the C++ change compiles cleanly.
2. **Sandbox check:** Launch an autonomous run and inspect `Saved/OliveAI/AgentSandbox/CLAUDE.md` — verify the hardcoded rules include schema_version 2.0 guidance and the AGENTS.md content appended below shows the v2.0 example.
3. **Functional test:** Send "create a gun blueprint that shoots bullets" via autonomous mode. Verify in the log:
   - The AI calls `blueprint.read` after `create_from_template`
   - plan_json calls use `schema_version: "2.0"`
   - The v2 executor path runs (look for `FOlivePlanExecutor` log lines instead of `PlanLowerer`)
   - No "Source pin 'auto' not found" errors
4. **Defensive test:** Manually send a v1.0 plan_json with `@step.auto` references via MCP to verify the GraphWriter auto-resolution fallback works (log should show "Auto-resolved source pin 'auto'..." instead of an error).
