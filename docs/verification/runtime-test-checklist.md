# Phase 1 Runtime Verification Checklist

> **Purpose:** Manual verification procedure for Phase 1 Blueprint MCP Tools
> **Prerequisites:** UE 5.5+, Plugin compiled successfully

---

## Test Environment Setup

1. Open `UE_Olive_AI_Toolkit` project in Unreal Editor
2. Verify plugin loads: Window > Olive AI Studio should be available
3. Open Output Log (Window > Developer Tools > Output Log)
4. Filter log by "LogOliveBPTools" and "LogOliveAI"

---

## Test 1: Tool Registry + Registration Logs

**Objective:** Verify all 40 tools register on startup

**Steps:**
1. Restart editor
2. Check Output Log for registration messages

**Expected Output:**
```
LogOliveBPTools: Registering Blueprint MCP tools...
LogOliveBPTools: Registered 7 reader tools
LogOliveBPTools: Registered 6 asset writer tools
LogOliveBPTools: Registered 3 variable writer tools
LogOliveBPTools: Registered 4 component writer tools
LogOliveBPTools: Registered 6 function writer tools
LogOliveBPTools: Registered 6 graph writer tools
LogOliveBPTools: Registered 4 AnimBP writer tools
LogOliveBPTools: Registered 4 widget writer tools
LogOliveBPTools: Registered 40 Blueprint MCP tools
```

**Pass Criteria:**
- [ ] All 8 registration log lines appear
- [ ] Final count shows 40 tools
- [ ] No errors in log

**Capture:** Screenshot of Output Log

---

## Test 2: MCP Server Connection

**Objective:** Verify MCP server accepts connections

**Steps:**
1. Check Output Log for MCP server startup
2. Note the port number (default 3000)
3. Use curl or MCP client to connect:
   ```bash
   curl -X POST http://localhost:3000/jsonrpc \
     -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
   ```

**Expected Output:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tools": [
      {"name": "blueprint.read", ...},
      ... (40 tools total)
    ]
  }
}
```

**Pass Criteria:**
- [ ] MCP server responds
- [ ] tools/list returns 40 tools
- [ ] Each tool has name, description, inputSchema

**Capture:** Full JSON response saved to `test-results/tools-list.json`

---

## Test 3: Reader Tools (7 tests)

### Test 3.1: blueprint.read

**Setup:** Create test Blueprint at `/Game/TestBP/BP_TestCharacter` with:
- Parent: Character
- 2 variables (Health: float, Name: string)
- 1 custom function (CalculateDamage)

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "blueprint.read",
    "arguments": {
      "path": "/Game/TestBP/BP_TestCharacter",
      "mode": "summary"
    }
  }
}
```

**Pass Criteria:**
- [ ] Response has `"success": true`
- [ ] `name` = "BP_TestCharacter"
- [ ] `parent_class` = "Character"
- [ ] `variables` array has 2 entries
- [ ] `functions` array includes "CalculateDamage"

### Test 3.2: blueprint.read (invalid path)

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/call",
  "params": {
    "name": "blueprint.read",
    "arguments": {
      "path": "/Game/DoesNotExist"
    }
  }
}
```

**Pass Criteria:**
- [ ] Response has `"success": false`
- [ ] `error.code` = "ASSET_NOT_FOUND"
- [ ] `error.message` contains path
- [ ] `error.suggestion` is present

### Test 3.3-3.7: Other readers

Repeat similar tests for:
- [ ] `blueprint.read_function`
- [ ] `blueprint.read_event_graph`
- [ ] `blueprint.read_variables`
- [ ] `blueprint.read_components`
- [ ] `blueprint.read_hierarchy`
- [ ] `blueprint.list_overridable_functions`

**Capture:** All request/response pairs saved to `test-results/readers/`

---

## Test 4: Writer Tools + Undo/Redo

### Test 4.1: blueprint.add_variable

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "method": "tools/call",
  "params": {
    "name": "blueprint.add_variable",
    "arguments": {
      "path": "/Game/TestBP/BP_TestCharacter",
      "variable": {
        "name": "TestVar",
        "type": {"category": "float"},
        "default_value": "42.0"
      }
    }
  }
}
```

**Pass Criteria:**
- [ ] Response has `"success": true`
- [ ] `created_item` = "TestVar"
- [ ] Variable appears in Blueprint editor

### Test 4.2: Undo/Redo Verification

**Steps:**
1. After Test 4.1, check Edit menu
2. Click Edit > Undo (or Ctrl+Z)
3. Verify variable is removed
4. Click Edit > Redo (or Ctrl+Y)
5. Verify variable is restored

**Pass Criteria:**
- [ ] Undo menu shows "Olive AI: Add Variable 'TestVar'"
- [ ] Undo removes the variable
- [ ] Redo restores the variable
- [ ] Blueprint state is consistent

**Capture:** Screenshots of before/after undo/redo

### Test 4.3: blueprint.add_node + connect_pins

**Steps:**
1. Call `blueprint.add_node` to add PrintString node
2. Note returned `node_id`
3. Call `blueprint.connect_pins` to connect to BeginPlay
4. Verify connection in graph editor

**Pass Criteria:**
- [ ] Node created with valid ID returned
- [ ] Connection successful
- [ ] Graph shows connection visually
- [ ] Undo removes both operations

**Capture:** Screenshot of graph with connection

---

## Test 5: Structured Compile Errors

### Test 5.1: Invalid Pin Connection

**Setup:** Create broken Blueprint with disconnected Branch node

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 20,
  "method": "tools/call",
  "params": {
    "name": "blueprint.compile",
    "arguments": {
      "path": "/Game/TestBP/BP_Broken"
    }
  }
}
```

**Pass Criteria:**
- [ ] Response has `"success": false`
- [ ] `error.code` = "BP_COMPILE_ERROR"
- [ ] `compile_result.errors` array is non-empty
- [ ] Each error has `severity`, `message`, `node_id` or `graph`

### Test 5.2: Type Mismatch Connection

**Request:** Try connecting float output to boolean input

**Pass Criteria:**
- [ ] Response has `"success": false`
- [ ] `error.code` = "GRAPH_PIN_INCOMPATIBLE"
- [ ] `error.suggestion` mentions conversion

**Capture:** Full error responses saved to `test-results/errors/`

---

## Test 6: Batch Read Stability

**Objective:** Verify sequential reads don't crash, errors are isolated

**Steps:**
1. Create test script with 10 read requests:
   - 5 valid Blueprints
   - 3 non-existent paths
   - 2 non-Blueprint assets (textures, etc.)

2. Execute all requests sequentially

3. Verify results

**Pass Criteria:**
- [ ] All 10 requests complete (no hang/crash)
- [ ] Valid paths return success
- [ ] Invalid paths return structured errors
- [ ] Editor remains responsive
- [ ] No memory leaks (check Task Manager)

**Capture:** Full batch results saved to `test-results/batch-read.json`

---

## Test 7: AnimBP + Widget Tools

### Test 7.1: AnimBP State Machine

**Setup:** Create Animation Blueprint at `/Game/TestBP/ABP_Test`

**Steps:**
1. Call `animbp.add_state_machine` with name "Locomotion"
2. Call `animbp.add_state` with name "Idle"
3. Call `animbp.add_state` with name "Walk"
4. Call `animbp.add_transition` from "Idle" to "Walk"
5. Open AnimGraph and verify

**Pass Criteria:**
- [ ] State machine created
- [ ] Both states visible
- [ ] Transition arrow shown
- [ ] Undo removes all in reverse order

### Test 7.2: Widget Add/Remove

**Setup:** Create Widget Blueprint at `/Game/TestBP/WBP_Test`

**Steps:**
1. Call `widget.add_widget` with class "Button"
2. Call `widget.set_property` to set text
3. Verify in Widget Designer

**Pass Criteria:**
- [ ] Button widget created
- [ ] Property set correctly
- [ ] Widget visible in hierarchy

**Capture:** Screenshots of AnimGraph and Widget Designer

---

## Results Summary

| Test | Pass/Fail | Notes |
|------|-----------|-------|
| 1. Tool Registry | | |
| 2. MCP Connection | | |
| 3.1 blueprint.read | | |
| 3.2 read (invalid) | | |
| 3.3-3.7 Other readers | | |
| 4.1 add_variable | | |
| 4.2 Undo/Redo | | |
| 4.3 add_node + connect | | |
| 5.1 Compile error | | |
| 5.2 Type mismatch | | |
| 6. Batch read | | |
| 7.1 AnimBP | | |
| 7.2 Widget | | |

**Overall Status:** [ ] PASS / [ ] FAIL

**Tester:** _______________
**Date:** _______________

---

## Artifacts Checklist

Save all test artifacts to `docs/verification/test-results/`:

- [ ] `tools-list.json` - Full tools/list response
- [ ] `readers/*.json` - All reader test responses
- [ ] `writers/*.json` - All writer test responses
- [ ] `errors/*.json` - All error responses
- [ ] `batch-read.json` - Batch read results
- [ ] `screenshots/` - All captured screenshots
- [ ] `undo-redo.mp4` - Screen recording of undo/redo (optional)
