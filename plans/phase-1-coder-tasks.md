# Phase 1 Coder Tasks — Blueprint MCP Tools Implementation

> **Date:** February 19, 2026
> **Design Document:** `plans/phase-1-blueprint-mcp-tools-design.md`
> **Status:** Ready for implementation

---

## Overview

This document breaks down the Phase 1 implementation into 12 discrete tasks for the coder agent. Each task has a clear scope, file list, and verification criteria. Tasks should be completed in order as they have dependencies.

**Total MCP Tools to Implement:** 42 (7 readers + 35 writers)

---

## Task 1: Write Pipeline Service (Foundation)

**Priority:** CRITICAL — All writer tools depend on this

### Files to Create
```
Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h
Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp
```

### Scope
1. Implement `FOliveWriteRequest` struct with all fields from design
2. Implement `FOliveWriteResult` struct with factory methods
3. Implement `FOliveWritePipeline` singleton
4. Implement all 6 pipeline stages:
   - `StageValidate()` - Calls `FOliveValidationEngine`
   - `StageConfirm()` - Tier routing (skip for `bFromMCP=true`)
   - `StageTransact()` - Opens `FScopedTransaction`
   - `StageExecute()` - Calls provided executor delegate
   - `StageVerify()` - Structure checks + optional compile
   - `StageReport()` - Assemble final result
5. Implement tier lookup from `UOliveAISettings`
6. Implement confirmation token generation/storage for built-in chat (can be minimal initially)

### Integration
- Add `#include "Blueprint/Pipeline/OliveWritePipeline.h"` paths
- Uses `FOliveValidationEngine`, `FOliveTransactionManager`, `FOliveCompileManager`

### Verification Criteria
- [ ] Compiles without errors
- [ ] Pipeline executes all 6 stages in order
- [ ] MCP requests (`bFromMCP=true`) skip confirmation stage
- [ ] Transaction wraps execute stage
- [ ] Compile errors captured in result

---

## Task 2: Blueprint Tool Schemas

**Priority:** HIGH — All tool registrations need schemas

### Files to Create
```
Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h
Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp
```

### Scope
1. Implement `OliveBlueprintSchemas` namespace with all schema builder functions
2. Implement common schema components:
   - `StringProp()`, `IntProp()`, `BoolProp()`, `ArrayProp()`, `ObjectProp()`
   - `TypeSpecSchema()` - For variable/parameter types
   - `FunctionParamSchema()` - For function parameters
   - `FunctionSignatureSchema()` - For function definitions
   - `VariableSchema()` - For variable definitions
3. Implement all 42 tool schemas (7 reader + 35 writer schemas)

### Schema Format
Follow JSON Schema Draft 7 for MCP compatibility:
```json
{
  "type": "object",
  "properties": {
    "path": {"type": "string", "description": "Blueprint asset path"}
  },
  "required": ["path"]
}
```

### Verification Criteria
- [ ] Compiles without errors
- [ ] All schemas serialize to valid JSON
- [ ] Required fields marked correctly
- [ ] Descriptions are clear for AI understanding

---

## Task 3: Reader Tool Handlers

**Priority:** HIGH — Enables AI to inspect Blueprints

### Files to Create
```
Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h
Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp (partial)
```

### Scope
1. Implement `FOliveBlueprintToolHandlers` singleton structure
2. Implement `RegisterAllTools()` and `UnregisterAllTools()`
3. Implement `RegisterReaderTools()` - Registers 7 reader tools
4. Implement helper methods:
   - `LoadBlueprintFromParams()` - Parse path and load Blueprint
   - `ParseTypeFromParams()` - JSON to `FOliveIRType`
5. Implement 7 reader handlers:
   - `HandleBlueprintRead()` - Full Blueprint IR
   - `HandleBlueprintReadFunction()` - Single function graph
   - `HandleBlueprintReadEventGraph()` - Event graph
   - `HandleBlueprintReadVariables()` - Variable list
   - `HandleBlueprintReadComponents()` - Component tree
   - `HandleBlueprintReadHierarchy()` - Inheritance chain
   - `HandleBlueprintListOverridableFunctions()` - Parent overridables

### Integration
- Modify `OliveToolRegistry.cpp` - Remove stub calls, add `FOliveBlueprintToolHandlers::Get().RegisterAllTools()`
- Modify `OliveAIEditorModule.cpp` - Call registration in `StartupModule()`

### Verification Criteria
- [ ] Compiles without errors
- [ ] `blueprint.read` returns valid IR for test Blueprint
- [ ] `blueprint.read_function` returns function graph with nodes
- [ ] `blueprint.read_variables` returns variable array
- [ ] Error handling for missing assets, wrong type

---

## Task 4: AnimGraph Serializer

**Priority:** MEDIUM — Required for Animation Blueprint support

### Files to Create
```
Source/OliveAIEditor/Blueprint/Public/Reader/OliveAnimGraphSerializer.h
Source/OliveAIEditor/Blueprint/Private/Reader/OliveAnimGraphSerializer.cpp
```

### Scope
1. Implement `FOliveAnimGraphSerializer` class
2. Implement state machine reading:
   - `ReadStateMachines()` - All state machines from AnimBP
   - `ReadStateMachine()` - Single state machine by name
   - `SerializeStateMachine()` - Convert to `FOliveIRAnimStateMachine`
3. Implement state reading:
   - `SerializeState()` - Convert to `FOliveIRAnimState`
   - `GetStateAnimationAsset()` - Extract animation reference
   - `IsConduitState()` - Check for conduit
4. Implement transition reading:
   - `GetTransitionsIn()` / `GetTransitionsOut()`
   - `SerializeTransitionRule()` - Convert rule to description
5. Implement summary methods:
   - `ReadAnimGraphSummary()` - Lightweight overview
   - `ReadAnimGraphFull()` - Complete graph IR

### Integration
- Modify `OliveBlueprintReader.h/.cpp`:
  - Add `TSharedPtr<FOliveAnimGraphSerializer> AnimGraphSerializer` member
  - Add `ReadAnimGraphStateMachines()` method
  - Integrate into `ReadBlueprintFull()` for AnimBP type

### Module Dependencies
Add to `OliveAIEditor.Build.cs` if not present:
```csharp
"AnimGraph",
"AnimationBlueprintEditor",
```

### Verification Criteria
- [ ] Compiles without errors
- [ ] Animation Blueprint read returns state machine data
- [ ] States include entry state and transitions
- [ ] Animation asset paths extracted correctly

---

## Task 5: Widget Tree Serializer

**Priority:** MEDIUM — Required for Widget Blueprint support

### Files to Create
```
Source/OliveAIEditor/Blueprint/Public/Reader/OliveWidgetTreeSerializer.h
Source/OliveAIEditor/Blueprint/Private/Reader/OliveWidgetTreeSerializer.cpp
```

### Scope
1. Implement `FOliveWidgetTreeSerializer` class
2. Implement tree reading:
   - `ReadWidgetTree()` - Full hierarchy from root
   - `ReadWidget()` - Single widget by name
   - `ReadWidgetTreeFlat()` - All widgets as flat array
   - `ReadWidgetTreeSummary()` - Names/types only
3. Implement recursive serialization:
   - `SerializeWidget()` - Convert to `FOliveIRWidgetNode`
   - `GetChildWidgets()` - Get children of panel
   - `IsPanel()` - Check if widget can have children
4. Implement property extraction:
   - `ExtractWidgetProperties()` - Common properties
   - `DetermineSlotType()` - Canvas, HBox, VBox, etc.
   - `ExtractSlotProperties()` - Slot-specific properties
5. Implement binding reading:
   - `HasBindings()` - Check for property bindings
   - `GetWidgetBindings()` - Extract binding expressions

### Integration
- Modify `OliveBlueprintReader.h/.cpp`:
  - Add `TSharedPtr<FOliveWidgetTreeSerializer> WidgetTreeSerializer` member
  - Add `ReadWidgetTree()` method
  - Integrate into `ReadBlueprintFull()` for WidgetBP type

### Module Dependencies
Add to `OliveAIEditor.Build.cs` if not present:
```csharp
"UMG",
"UMGEditor",
```

### Verification Criteria
- [ ] Compiles without errors
- [ ] Widget Blueprint read returns widget tree
- [ ] Children correctly nested under parents
- [ ] Slot types identified correctly

---

## Task 6: Asset-Level Writer Tools

**Priority:** HIGH — Core write functionality

### Files to Modify
```
Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp
```

### Scope
1. Implement `RegisterAssetWriterTools()` method
2. Implement `BuildWriteRequest()` helper
3. Implement 6 asset-level handlers:
   - `HandleBlueprintCreate()` - Create new Blueprint
   - `HandleBlueprintSetParentClass()` - Reparent (Tier 3)
   - `HandleBlueprintAddInterface()` - Implement interface
   - `HandleBlueprintRemoveInterface()` - Remove interface
   - `HandleBlueprintCompile()` - Force compile
   - `HandleBlueprintDelete()` - Delete asset (Tier 3)

### Pattern to Follow
```cpp
FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintAddVariable(const TSharedPtr<FJsonObject>& Params)
{
    // 1. Build write request
    FOliveWriteRequest Request = BuildWriteRequest(
        TEXT("blueprint.add_variable"),
        Params,
        TEXT("variable"),  // Operation category for tier lookup
        LOCTEXT("AddVariable", "AI Agent: Add Variable")
    );
    Request.bFromMCP = true;  // Always true for MCP tools

    // 2. Define executor (what actually does the mutation)
    FOliveWriteExecutor Executor = FOliveWriteExecutor::CreateLambda(
        [this, Params](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
        {
            UBlueprint* Blueprint = Cast<UBlueprint>(Target);
            FOliveIRVariable VarIR = ParseVariableFromParams(Params);
            auto WriteResult = FOliveBlueprintWriter::Get().AddVariable(Req.AssetPath, VarIR);
            // Convert to pipeline result
            ...
        }
    );

    // 3. Execute through pipeline
    FOliveWriteResult Result = FOliveWritePipeline::Get().Execute(Request, Executor);

    // 4. Convert to tool result
    return Result.ToToolResult();
}
```

### Verification Criteria
- [ ] Compiles without errors
- [ ] `blueprint.create` creates Blueprint with correct parent
- [ ] `blueprint.compile` returns compile errors if any
- [ ] Undo works for all operations
- [ ] Delete requires Tier 3 (but still executes for MCP)

---

## Task 7: Variable Writer Tools

**Priority:** HIGH — Common AI operation

### Files to Modify
```
Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp
```

### Scope
1. Implement `RegisterVariableWriterTools()` method
2. Implement `ParseVariableFromParams()` helper
3. Implement 3 variable handlers:
   - `HandleBlueprintAddVariable()` - Add new variable
   - `HandleBlueprintRemoveVariable()` - Remove variable
   - `HandleBlueprintModifyVariable()` - Change properties

### Variable Spec Parsing
```json
{
  "variable": {
    "name": "Health",
    "type": {"base": "float"},
    "category": "Stats",
    "default_value": "100.0",
    "replicated": false,
    "expose_on_spawn": true
  }
}
```

### Verification Criteria
- [ ] Compiles without errors
- [ ] `blueprint.add_variable` creates variable with correct type
- [ ] Variable appears in `blueprint.read_variables` output
- [ ] Undo removes variable
- [ ] Type validation errors handled

---

## Task 8: Component Writer Tools

**Priority:** HIGH — Common AI operation

### Files to Modify
```
Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp
```

### Scope
1. Implement `RegisterComponentWriterTools()` method
2. Implement 4 component handlers:
   - `HandleBlueprintAddComponent()` - Add component
   - `HandleBlueprintRemoveComponent()` - Remove component
   - `HandleBlueprintModifyComponent()` - Set properties
   - `HandleBlueprintReparentComponent()` - Change parent

### Integration
- Uses `FOliveComponentWriter` for mutations

### Verification Criteria
- [ ] Compiles without errors
- [ ] `blueprint.add_component` creates component in SCS
- [ ] Component hierarchy correct after operations
- [ ] Non-SceneComponent blocks child add (validation)

---

## Task 9: Function Writer Tools

**Priority:** HIGH — Core graph editing

### Files to Modify
```
Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp
```

### Scope
1. Implement `RegisterFunctionWriterTools()` method
2. Implement `ParseFunctionSignatureFromParams()` helper
3. Implement 6 function handlers:
   - `HandleBlueprintAddFunction()` - Create function
   - `HandleBlueprintRemoveFunction()` - Delete function
   - `HandleBlueprintModifyFunctionSignature()` - Change signature
   - `HandleBlueprintAddEventDispatcher()` - Create dispatcher
   - `HandleBlueprintOverrideFunction()` - Override parent function
   - `HandleBlueprintAddCustomEvent()` - Add custom event

### Function Signature Parsing
```json
{
  "signature": {
    "name": "CalculateDamage",
    "category": "Combat",
    "description": "Calculate damage based on stats",
    "inputs": [
      {"name": "BaseDamage", "type": {"base": "float"}},
      {"name": "Target", "type": {"base": "object", "class": "Actor"}}
    ],
    "outputs": [
      {"name": "FinalDamage", "type": {"base": "float"}}
    ],
    "pure": false,
    "static": false
  }
}
```

### Verification Criteria
- [ ] Compiles without errors
- [ ] `blueprint.add_function` creates function graph
- [ ] Signature parameters appear in graph entry/return nodes
- [ ] Override function creates parent call node

---

## Task 10: Graph Writer Tools

**Priority:** HIGH — Node-level editing

### Files to Modify
```
Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp
```

### Scope
1. Implement `RegisterGraphWriterTools()` method
2. Implement pin reference parsing ("NodeId.PinName" format)
3. Implement 6 graph handlers:
   - `HandleBlueprintAddNode()` - Add node by type
   - `HandleBlueprintRemoveNode()` - Remove node by ID
   - `HandleBlueprintConnectPins()` - Connect two pins
   - `HandleBlueprintDisconnectPins()` - Break connection
   - `HandleBlueprintSetPinDefault()` - Set default value
   - `HandleBlueprintSetNodeProperty()` - Set node property

### Integration
- Uses `FOliveGraphWriter`, `FOliveNodeFactory`, `FOlivePinConnector`

### Node Types
Use `FOliveNodeCatalog` to look up node types:
```cpp
// Request adds a "Print String" node
TOptional<FOliveNodeCatalogEntry> Entry = FOliveNodeCatalog::Get().FindByName("Print String");
```

### Verification Criteria
- [ ] Compiles without errors
- [ ] `blueprint.add_node` creates node and returns ID
- [ ] `blueprint.connect_pins` creates connection
- [ ] Pin type mismatch returns structured error
- [ ] Compile after graph changes works

---

## Task 11: AnimBP Writer Tools

**Priority:** MEDIUM — Animation Blueprint editing

### Files to Create (if needed)
```
Source/OliveAIEditor/Blueprint/Public/Writer/OliveAnimGraphWriter.h
Source/OliveAIEditor/Blueprint/Private/Writer/OliveAnimGraphWriter.cpp
```

### Files to Modify
```
Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp
```

### Scope
1. Implement `RegisterAnimBPWriterTools()` method
2. Implement (or create) `FOliveAnimGraphWriter` if needed
3. Implement 4 AnimBP handlers:
   - `HandleAnimBPAddStateMachine()` - Create state machine
   - `HandleAnimBPAddState()` - Add state to machine
   - `HandleAnimBPAddTransition()` - Add transition between states
   - `HandleAnimBPSetTransitionRule()` - Configure transition rule

### Notes
- AnimBP graph editing is more complex than K2 graphs
- May need to research `UAnimGraphNode_StateMachine`, `UAnimStateNode` APIs
- Transition rules involve creating graph nodes

### Verification Criteria
- [ ] Compiles without errors
- [ ] `animbp.add_state_machine` creates machine in AnimGraph
- [ ] States and transitions readable via reader
- [ ] Entry state set correctly

---

## Task 12: Widget Writer Tools

**Priority:** MEDIUM — Widget Blueprint editing

### Files to Create (if needed)
```
Source/OliveAIEditor/Blueprint/Public/Writer/OliveWidgetWriter.h
Source/OliveAIEditor/Blueprint/Private/Writer/OliveWidgetWriter.cpp
```

### Files to Modify
```
Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp
```

### Scope
1. Implement `RegisterWidgetWriterTools()` method
2. Implement (or create) `FOliveWidgetWriter` if needed
3. Implement 4 Widget handlers:
   - `HandleWidgetAddWidget()` - Add widget to tree
   - `HandleWidgetRemoveWidget()` - Remove widget
   - `HandleWidgetSetProperty()` - Set widget property
   - `HandleWidgetBindProperty()` - Create property binding

### Notes
- Widget tree editing uses `UWidgetTree` API
- Adding widgets requires specifying parent and slot
- Property bindings are complex (may be stub for now)

### Verification Criteria
- [ ] Compiles without errors
- [ ] `widget.add_widget` creates widget in tree
- [ ] Widget hierarchy readable via reader
- [ ] Property setting works for common properties

---

## Summary: File Creation Order

### New Files to Create
1. `Blueprint/Public/Pipeline/OliveWritePipeline.h`
2. `Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
3. `Blueprint/Public/MCP/OliveBlueprintSchemas.h`
4. `Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`
5. `Blueprint/Public/MCP/OliveBlueprintToolHandlers.h`
6. `Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
7. `Blueprint/Public/Reader/OliveAnimGraphSerializer.h`
8. `Blueprint/Private/Reader/OliveAnimGraphSerializer.cpp`
9. `Blueprint/Public/Reader/OliveWidgetTreeSerializer.h`
10. `Blueprint/Private/Reader/OliveWidgetTreeSerializer.cpp`
11. `Blueprint/Public/Writer/OliveAnimGraphWriter.h` (if needed)
12. `Blueprint/Private/Writer/OliveAnimGraphWriter.cpp` (if needed)
13. `Blueprint/Public/Writer/OliveWidgetWriter.h` (if needed)
14. `Blueprint/Private/Writer/OliveWidgetWriter.cpp` (if needed)

### Files to Modify
1. `OliveAIEditor.Build.cs` - Add module dependencies
2. `OliveAIEditorModule.cpp` - Register tools on startup
3. `MCP/OliveToolRegistry.cpp` - Remove stubs
4. `Reader/OliveBlueprintReader.h/.cpp` - Add AnimGraph/Widget integration

---

## How to Use This Document

1. **Start with Task 1** — Write Pipeline is the foundation
2. **Complete Task 2** before Task 3 — Schemas needed for registration
3. **Task 3** enables testing readers before implementing writers
4. **Tasks 4-5** can be done after Task 3 (specialized readers)
5. **Tasks 6-10** are the core writer implementations
6. **Tasks 11-12** are specialized and more complex

Each task should be implemented fully before moving to the next. Use `// TODO: Phase 1 Task X` comments if stubbing anything for later.

---

## Invoking the Coder

To implement a specific task, invoke the coder agent with:

```
Implement Task N from plans/phase-1-coder-tasks.md

Read the design document at plans/phase-1-blueprint-mcp-tools-design.md for interface definitions and data flow.

Focus on:
- Files listed under the task
- Verification criteria at the end
- Integration points with existing code
```
