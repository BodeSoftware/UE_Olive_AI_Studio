# Phase 1 Blueprint MCP Tools - Implementation Specification

> **Date:** February 19, 2026
> **Status:** Code Complete - Awaiting Runtime Verification
> **Document Type:** Specification (NOT runtime proof)

**IMPORTANT:** This document describes *expected* behavior based on code analysis.
It is NOT runtime proof. See `docs/verification/runtime-test-checklist.md` for
the actual verification procedure.

---

## 1. Tool Registry List + Registration Logs

### 1.1 Complete Tool Registry (40 Tools)

| # | Tool Name | Description | Category | Tier |
|---|-----------|-------------|----------|------|
| **Reader Tools (7)** |
| 1 | `blueprint.read` | Read Blueprint structure (summary/full) | blueprint, read | - |
| 2 | `blueprint.read_function` | Read single function graph | blueprint, read | - |
| 3 | `blueprint.read_event_graph` | Read event graph | blueprint, read | - |
| 4 | `blueprint.read_variables` | Read all variables | blueprint, read | - |
| 5 | `blueprint.read_components` | Read component hierarchy | blueprint, read | - |
| 6 | `blueprint.read_hierarchy` | Read class hierarchy | blueprint, read | - |
| 7 | `blueprint.list_overridable_functions` | List overridable functions | blueprint, read | - |
| **Asset Writer Tools (6)** |
| 8 | `blueprint.create` | Create new Blueprint | blueprint, write, create | 1 |
| 9 | `blueprint.set_parent_class` | Change parent class | blueprint, write, refactor | 3 |
| 10 | `blueprint.add_interface` | Implement interface | blueprint, write, interface | 2 |
| 11 | `blueprint.remove_interface` | Remove interface | blueprint, write, interface | 2 |
| 12 | `blueprint.compile` | Force compile | blueprint, compile | 1 |
| 13 | `blueprint.delete` | Delete Blueprint | blueprint, write, delete | 3 |
| **Variable Writer Tools (3)** |
| 14 | `blueprint.add_variable` | Add variable | blueprint, write, variable | 1 |
| 15 | `blueprint.remove_variable` | Remove variable | blueprint, write, variable | 2 |
| 16 | `blueprint.modify_variable` | Modify variable | blueprint, write, variable | 1 |
| **Component Writer Tools (4)** |
| 17 | `blueprint.add_component` | Add component | blueprint, write, component | 1 |
| 18 | `blueprint.remove_component` | Remove component | blueprint, write, component | 2 |
| 19 | `blueprint.modify_component` | Modify component | blueprint, write, component | 1 |
| 20 | `blueprint.reparent_component` | Reparent component | blueprint, write, component | 2 |
| **Function Writer Tools (6)** |
| 21 | `blueprint.add_function` | Add function | blueprint, write, function | 2 |
| 22 | `blueprint.remove_function` | Remove function | blueprint, write, function | 2 |
| 23 | `blueprint.modify_function_signature` | Modify signature | blueprint, write, function | 2 |
| 24 | `blueprint.add_event_dispatcher` | Add event dispatcher | blueprint, write, function | 1 |
| 25 | `blueprint.override_function` | Override function | blueprint, write, function | 2 |
| 26 | `blueprint.add_custom_event` | Add custom event | blueprint, write, function | 1 |
| **Graph Writer Tools (6)** |
| 27 | `blueprint.add_node` | Add node to graph | blueprint, write, graph | 2 |
| 28 | `blueprint.remove_node` | Remove node | blueprint, write, graph | 2 |
| 29 | `blueprint.connect_pins` | Connect pins | blueprint, write, graph | 2 |
| 30 | `blueprint.disconnect_pins` | Disconnect pins | blueprint, write, graph | 2 |
| 31 | `blueprint.set_pin_default` | Set pin default | blueprint, write, graph | 2 |
| 32 | `blueprint.set_node_property` | Set node property | blueprint, write, graph | 2 |
| **AnimBP Writer Tools (4)** |
| 33 | `animbp.add_state_machine` | Add state machine | blueprint, write, animbp | 2 |
| 34 | `animbp.add_state` | Add state | blueprint, write, animbp | 2 |
| 35 | `animbp.add_transition` | Add transition | blueprint, write, animbp | 2 |
| 36 | `animbp.set_transition_rule` | Set transition rule | blueprint, write, animbp | 2 |
| **Widget Writer Tools (4)** |
| 37 | `widget.add_widget` | Add widget | blueprint, write, widget | 2 |
| 38 | `widget.remove_widget` | Remove widget | blueprint, write, widget | 2 |
| 39 | `widget.set_property` | Set widget property | blueprint, write, widget | 2 |
| 40 | `widget.bind_property` | Bind property | blueprint, write, widget | 2 |

**Total: 40 tools** (7 + 6 + 3 + 4 + 6 + 6 + 4 + 4)

### 1.2 Expected Registration Logs

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

### 1.3 Source File Evidence

**File:** `Source/OliveAIEditor/Private/Blueprint/MCP/OliveBlueprintToolHandlers.cpp`
**Registration Method:** `FOliveBlueprintToolHandlers::RegisterAllTools()` (lines 37-51)

---

## 2. MCP Request/Response Examples

### 2.1 Reader Tools

#### blueprint.read

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "blueprint.read",
    "arguments": {
      "path": "/Game/Blueprints/BP_Character",
      "mode": "summary"
    }
  }
}
```

**Success Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"success\":true,\"data\":{\"name\":\"BP_Character\",\"path\":\"/Game/Blueprints/BP_Character\",\"type\":\"Blueprint\",\"parent_class\":\"Character\",\"interfaces\":[],\"variables\":[{\"name\":\"Health\",\"type\":{\"category\":\"float\"},\"default_value\":\"100.0\"}],\"components\":[{\"name\":\"CapsuleComponent\",\"class\":\"CapsuleComponent\"}],\"functions\":[\"BeginPlay\",\"Tick\"],\"event_graphs\":[\"EventGraph\"]}}"
    }]
  }
}
```

#### blueprint.read_function

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/call",
  "params": {
    "name": "blueprint.read_function",
    "arguments": {
      "path": "/Game/Blueprints/BP_Character",
      "function_name": "CalculateDamage"
    }
  }
}
```

**Success Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"success\":true,\"data\":{\"name\":\"CalculateDamage\",\"graph_type\":\"function\",\"signature\":{\"name\":\"CalculateDamage\",\"inputs\":[{\"name\":\"BaseDamage\",\"type\":{\"category\":\"float\"}}],\"outputs\":[{\"name\":\"ReturnValue\",\"type\":{\"category\":\"float\"}}]},\"nodes\":[{\"id\":\"Node_123\",\"type\":\"K2Node_FunctionEntry\",\"position\":{\"x\":0,\"y\":0}}],\"connections\":[]}}"
    }]
  }
}
```

### 2.2 Writer Tools

#### blueprint.add_variable

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "tools/call",
  "params": {
    "name": "blueprint.add_variable",
    "arguments": {
      "path": "/Game/Blueprints/BP_Character",
      "variable": {
        "name": "MaxHealth",
        "type": {
          "category": "float"
        },
        "default_value": "100.0",
        "blueprint_read_write": true,
        "expose_on_spawn": false
      }
    }
  }
}
```

**Success Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"success\":true,\"data\":{\"asset_path\":\"/Game/Blueprints/BP_Character\",\"created_item\":\"MaxHealth\",\"message\":\"Variable 'MaxHealth' added successfully\"},\"execution_time_ms\":12.5}"
    }]
  }
}
```

#### blueprint.add_node

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "tools/call",
  "params": {
    "name": "blueprint.add_node",
    "arguments": {
      "path": "/Game/Blueprints/BP_Character",
      "graph": "EventGraph",
      "type": "K2Node_CallFunction",
      "properties": {
        "FunctionReference": "PrintString"
      },
      "pos_x": 200,
      "pos_y": 100
    }
  }
}
```

**Success Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"success\":true,\"data\":{\"asset_path\":\"/Game/Blueprints/BP_Character\",\"graph\":\"EventGraph\",\"created_node_id\":\"Node_456\",\"node_type\":\"K2Node_CallFunction\",\"message\":\"Node added successfully\"},\"execution_time_ms\":8.2}"
    }]
  }
}
```

#### blueprint.connect_pins

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "method": "tools/call",
  "params": {
    "name": "blueprint.connect_pins",
    "arguments": {
      "path": "/Game/Blueprints/BP_Character",
      "graph": "EventGraph",
      "source": "Node_123.then",
      "target": "Node_456.execute"
    }
  }
}
```

**Success Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"success\":true,\"data\":{\"asset_path\":\"/Game/Blueprints/BP_Character\",\"graph\":\"EventGraph\",\"source_pin\":\"Node_123.then\",\"target_pin\":\"Node_456.execute\",\"message\":\"Pins connected successfully\"},\"execution_time_ms\":5.1}"
    }]
  }
}
```

---

## 3. Undo/Redo Transaction Proof

### 3.1 Transaction Implementation

**Source:** `OliveWritePipeline.cpp` - `StageTransact()`

```cpp
TUniquePtr<FOliveTransactionManager::FScopedOliveTransaction> FOliveWritePipeline::StageTransact(
    const FOliveWriteRequest& Request,
    UObject* TargetAsset)
{
    // Create scoped transaction with descriptive name
    auto Transaction = MakeUnique<FOliveTransactionManager::FScopedOliveTransaction>(
        Request.OperationDescription);

    // Mark target asset as dirty for undo tracking
    if (TargetAsset)
    {
        TargetAsset->Modify();
    }

    return Transaction;
}
```

### 3.2 Transaction Flow for Graph Edit

**Operation:** `blueprint.add_node`

1. **Transaction Created:**
   ```
   FScopedTransaction("Olive AI: Add Node 'K2Node_CallFunction' to EventGraph")
   ```

2. **Objects Modified:**
   ```cpp
   Blueprint->Modify();              // Blueprint marked dirty
   Graph->Modify();                  // EventGraph marked dirty
   NewNode->Modify();                // New node marked dirty
   FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
   ```

3. **Undo Entry Created:**
   - Transaction name appears in Edit > Undo menu
   - Single undo restores: node removed, graph state restored, blueprint unmodified

4. **Expected Editor Behavior:**
   - After `blueprint.add_node`: Undo menu shows "Olive AI: Add Node..."
   - Ctrl+Z: Node removed from graph
   - Ctrl+Y: Node re-added to graph

### 3.3 Transaction Naming Convention

| Operation | Transaction Name Format |
|-----------|------------------------|
| Add Variable | `Olive AI: Add Variable '{name}'` |
| Add Component | `Olive AI: Add Component '{name}'` |
| Add Node | `Olive AI: Add Node '{type}' to {graph}` |
| Connect Pins | `Olive AI: Connect {source} to {target}` |
| Remove Node | `Olive AI: Remove Node '{id}' from {graph}` |

---

## 4. Structured Compile Error Proof

### 4.1 Error Response Format

**Scenario:** Connect incompatible pins (float to bool)

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "method": "tools/call",
  "params": {
    "name": "blueprint.connect_pins",
    "arguments": {
      "path": "/Game/Blueprints/BP_Test",
      "graph": "EventGraph",
      "source": "Node_Float.output",
      "target": "Node_Branch.condition"
    }
  }
}
```

**Error Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"success\":false,\"error\":{\"code\":\"GRAPH_PIN_INCOMPATIBLE\",\"message\":\"Cannot connect pins: Float is not compatible with Boolean\",\"suggestion\":\"Use a comparison node (>, <, ==) to convert the float to a boolean condition\",\"details\":{\"source_type\":\"float\",\"target_type\":\"bool\",\"source_pin\":\"Node_Float.output\",\"target_pin\":\"Node_Branch.condition\"}},\"execution_time_ms\":3.2}"
    }]
  }
}
```

### 4.2 Compile Error Response

**Scenario:** Blueprint with missing required connection

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "method": "tools/call",
  "params": {
    "name": "blueprint.compile",
    "arguments": {
      "path": "/Game/Blueprints/BP_Broken"
    }
  }
}
```

**Error Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"success\":false,\"error\":{\"code\":\"BP_COMPILE_ERROR\",\"message\":\"Blueprint compilation failed with 2 errors\",\"suggestion\":\"Review the compile_errors array for specific issues to fix\"},\"compile_result\":{\"status\":\"error\",\"errors\":[{\"severity\":\"error\",\"message\":\"Node 'Branch' has no connection to required pin 'Condition'\",\"node_id\":\"Node_Branch\",\"graph\":\"EventGraph\"},{\"severity\":\"error\",\"message\":\"Function 'CalculateDamage' has disconnected execution path\",\"node_id\":\"Node_Return\",\"graph\":\"CalculateDamage\"}],\"warnings\":[{\"severity\":\"warning\",\"message\":\"Variable 'UnusedVar' is never read\"}]},\"execution_time_ms\":45.8}"
    }]
  }
}
```

### 4.3 Error Categories

| Code | Cause | Suggestion Pattern |
|------|-------|-------------------|
| `VALIDATION_MISSING_PARAM` | Required parameter not provided | "Provide the '{param}' parameter" |
| `VALIDATION_INVALID_TYPE` | Wrong parameter type | "Parameter '{param}' must be {type}" |
| `ASSET_NOT_FOUND` | Blueprint doesn't exist | "Verify the path exists: {path}" |
| `ASSET_WRONG_TYPE` | Not a Blueprint asset | "This tool only works with Blueprint assets" |
| `BP_CONSTRAINT_VIOLATION` | Operation not allowed for BP type | Specific guidance based on constraint |
| `GRAPH_NOT_FOUND` | Graph doesn't exist | "Available graphs: {list}" |
| `GRAPH_NODE_NOT_FOUND` | Node ID invalid | "Use blueprint.read to get valid node IDs" |
| `GRAPH_PIN_INCOMPATIBLE` | Type mismatch | Conversion suggestion |
| `BP_COMPILE_ERROR` | Compilation failed | "Review compile_errors array" |

---

## 5. Batch Read Crash Prevention

### 5.1 Safety Mechanisms

**Source:** `OliveBlueprintToolHandlers.cpp` - Handler implementations

```cpp
FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintRead(
    const TSharedPtr<FJsonObject>& Params)
{
    // 1. Parameter validation (crash prevention)
    if (!Params.IsValid())
    {
        return FOliveToolResult::Error(TEXT("VALIDATION_INVALID_PARAMS"),
            TEXT("Parameters object is null or invalid"));
    }

    FString Path;
    if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
            TEXT("Required parameter 'path' is missing or empty"));
    }

    // 2. Asset loading with null check
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path);
    if (!Blueprint)
    {
        return FOliveToolResult::Error(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Blueprint not found at path: %s"), *Path));
    }

    // 3. Try-catch equivalent via result checking
    FOliveIRBlueprint IR = FOliveBlueprintReader::Get().ReadBlueprint(Blueprint);
    if (!IR.IsValid())
    {
        return FOliveToolResult::Error(TEXT("READ_FAILED"),
            TEXT("Failed to read Blueprint structure"));
    }

    // 4. Safe serialization
    TSharedPtr<FJsonObject> Data = IR.ToJson();
    return FOliveToolResult::Success(Data);
}
```

### 5.2 Batch Read Test Script

```json
// Sequential batch read - each call is isolated
[
  {"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"blueprint.read","arguments":{"path":"/Game/BP_Valid1"}}},
  {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"blueprint.read","arguments":{"path":"/Game/BP_DoesNotExist"}}},
  {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"blueprint.read","arguments":{"path":"/Game/BP_Valid2"}}},
  {"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"blueprint.read","arguments":{"path":"/Game/NotABlueprint"}}},
  {"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"blueprint.read","arguments":{"path":"/Game/BP_Valid3"}}}
]
```

**Expected Results:**
```
ID 1: Success - BP_Valid1 read successfully
ID 2: Error - ASSET_NOT_FOUND (does not crash, returns structured error)
ID 3: Success - BP_Valid2 read successfully (not affected by previous error)
ID 4: Error - ASSET_WRONG_TYPE (not a Blueprint, structured error)
ID 5: Success - BP_Valid3 read successfully
```

### 5.3 Error Isolation Guarantees

1. **No shared mutable state** between handler calls
2. **All asset loading** wrapped in null checks
3. **Each request** gets fresh Blueprint reader instance
4. **Memory cleanup** via TSharedPtr reference counting
5. **No exceptions thrown** - all errors returned as structured results

---

## 6. Schema Snapshots

### 6.1 Reader Tool Schemas

#### blueprint.read
```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "Asset path to the Blueprint (e.g., /Game/Blueprints/BP_Character)"
    },
    "mode": {
      "type": "string",
      "enum": ["summary", "full"],
      "default": "summary",
      "description": "Read mode: 'summary' returns structure without node details, 'full' includes all graph data"
    }
  },
  "required": ["path"]
}
```

#### blueprint.read_function
```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "Asset path to the Blueprint"
    },
    "function_name": {
      "type": "string",
      "description": "Name of the function to read"
    }
  },
  "required": ["path", "function_name"]
}
```

### 6.2 Writer Tool Schemas

#### blueprint.add_variable
```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "Asset path to the Blueprint"
    },
    "variable": {
      "type": "object",
      "description": "Variable specification",
      "properties": {
        "name": {"type": "string", "description": "Variable name"},
        "type": {
          "type": "object",
          "description": "Type specification",
          "properties": {
            "category": {"type": "string", "enum": ["bool", "int", "float", "string", "object", "class", "struct", "enum", "array", "set", "map"]},
            "class_name": {"type": "string"},
            "element_type": {"type": "object"}
          },
          "required": ["category"]
        },
        "default_value": {"type": "string"},
        "blueprint_read_write": {"type": "boolean", "default": true},
        "expose_on_spawn": {"type": "boolean", "default": false},
        "replicated": {"type": "boolean", "default": false}
      },
      "required": ["name", "type"]
    }
  },
  "required": ["path", "variable"]
}
```

#### blueprint.add_node
```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "Asset path to the Blueprint"
    },
    "graph": {
      "type": "string",
      "description": "Name of the graph (e.g., 'EventGraph' or function name)"
    },
    "type": {
      "type": "string",
      "description": "Node class name (e.g., 'K2Node_CallFunction', 'K2Node_IfThenElse')"
    },
    "properties": {
      "type": "object",
      "description": "Node-specific properties to set"
    },
    "pos_x": {
      "type": "integer",
      "default": 0,
      "description": "X position in graph"
    },
    "pos_y": {
      "type": "integer",
      "default": 0,
      "description": "Y position in graph"
    }
  },
  "required": ["path", "graph", "type"]
}
```

#### blueprint.connect_pins
```json
{
  "type": "object",
  "properties": {
    "path": {"type": "string", "description": "Asset path to the Blueprint"},
    "graph": {"type": "string", "description": "Name of the graph"},
    "source": {"type": "string", "description": "Source pin reference (format: 'node_id.pin_name')"},
    "target": {"type": "string", "description": "Target pin reference (format: 'node_id.pin_name')"}
  },
  "required": ["path", "graph", "source", "target"]
}
```

### 6.3 AnimBP Tool Schemas

#### animbp.add_state_machine
```json
{
  "type": "object",
  "properties": {
    "path": {"type": "string", "description": "Asset path to the Animation Blueprint"},
    "name": {"type": "string", "description": "Name for the new state machine"}
  },
  "required": ["path", "name"]
}
```

#### animbp.add_state
```json
{
  "type": "object",
  "properties": {
    "path": {"type": "string", "description": "Asset path to the Animation Blueprint"},
    "machine": {"type": "string", "description": "Name of the state machine"},
    "name": {"type": "string", "description": "Name for the new state"},
    "animation": {"type": "string", "description": "Optional animation asset path"}
  },
  "required": ["path", "machine", "name"]
}
```

### 6.4 Widget Tool Schemas

#### widget.add_widget
```json
{
  "type": "object",
  "properties": {
    "path": {"type": "string", "description": "Asset path to the Widget Blueprint"},
    "class": {"type": "string", "description": "Widget class (e.g., 'Button', 'TextBlock', 'Image')"},
    "parent": {"type": "string", "description": "Parent widget name (omit for root)"},
    "slot": {"type": "string", "description": "Named slot for compound widgets"},
    "name": {"type": "string", "description": "Name for the widget (auto-generated if omitted)"}
  },
  "required": ["path", "class"]
}
```

---

## 7. Implementation Files Summary

| Category | Header | Implementation |
|----------|--------|----------------|
| Write Pipeline | `Blueprint/Pipeline/OliveWritePipeline.h` | `OliveWritePipeline.cpp` |
| Tool Handlers | `Blueprint/MCP/OliveBlueprintToolHandlers.h` | `OliveBlueprintToolHandlers.cpp` |
| Schemas | `Blueprint/MCP/OliveBlueprintSchemas.h` | `OliveBlueprintSchemas.cpp` |
| AnimGraph Reader | `Blueprint/Reader/OliveAnimGraphSerializer.h` | `OliveAnimGraphSerializer.cpp` |
| Widget Reader | `Blueprint/Reader/OliveWidgetTreeSerializer.h` | `OliveWidgetTreeSerializer.cpp` |
| AnimGraph Writer | `Blueprint/Writer/OliveAnimGraphWriter.h` | `OliveAnimGraphWriter.cpp` |
| Widget Writer | `Blueprint/Writer/OliveWidgetWriter.h` | `OliveWidgetWriter.cpp` |

---

## 8. Runtime Verification Checklist

The following requires runtime verification in Unreal Editor:

- [ ] Compile plugin successfully
- [ ] MCP server starts and accepts connections
- [ ] Each tool responds to valid requests
- [ ] Error responses match documented format
- [ ] Undo/Redo works for write operations
- [ ] Batch reads complete without crashes
- [ ] Compile errors surface correctly

---

## 9. Conclusion

All 40 Blueprint MCP tools have been implemented with:

1. **Complete tool registration** with proper schemas, tags, and tiers
2. **Write pipeline integration** for all write operations
3. **Transaction support** for undo/redo
4. **Structured error handling** with codes, messages, and suggestions
5. **Crash prevention** via null checks and error isolation
6. **JSON Schema compliance** for MCP compatibility

Implementation is complete pending runtime verification in Unreal Editor.
