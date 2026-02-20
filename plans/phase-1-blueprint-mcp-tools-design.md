# Phase 1 Blueprint MCP Tools - Architectural Design

> **Date:** February 19, 2026
> **Author:** Architect
> **Status:** Ready for Implementation
> **Scope:** MCP Tool Handlers, Write Pipeline, AnimGraph/Widget Serializers

---

## 1. Executive Summary

This design document specifies the architecture for Phase 1 Blueprint MCP tools implementation. It bridges the existing `FOliveBlueprintReader`/`FOliveBlueprintWriter` infrastructure to the MCP tool registry via a new **Blueprint Tool Handlers** module and introduces a shared **Write Pipeline Service** that enforces the 6-stage safety pipeline for all write operations.

### Key Deliverables

1. **Blueprint Tool Handlers Module** - Dedicated handlers for all reader/writer MCP tools
2. **Write Pipeline Service** - 6-stage orchestration (Validate -> Confirm -> Transact -> Execute -> Verify -> Report)
3. **AnimGraph Serializer** - Specialized serializer for Animation Blueprint state machines
4. **Widget Tree Serializer** - Specialized serializer for Widget Blueprint widget hierarchy
5. **Confirmation Integration** - Hooks for tier-based confirmation routing

---

## 2. Module Structure

### 2.1 New Files to Create

```
Source/OliveAIEditor/
├── Blueprint/
│   ├── Public/
│   │   ├── MCP/                              [NEW DIRECTORY]
│   │   │   ├── OliveBlueprintToolHandlers.h  [NEW - Main tool handlers class]
│   │   │   └── OliveBlueprintSchemas.h       [NEW - JSON schema definitions]
│   │   ├── Reader/
│   │   │   ├── OliveAnimGraphSerializer.h    [NEW - AnimGraph specialization]
│   │   │   └── OliveWidgetTreeSerializer.h   [NEW - Widget tree specialization]
│   │   └── Pipeline/                         [NEW DIRECTORY]
│   │       └── OliveWritePipeline.h          [NEW - 6-stage write orchestrator]
│   └── Private/
│       ├── MCP/
│       │   ├── OliveBlueprintToolHandlers.cpp
│       │   └── OliveBlueprintSchemas.cpp
│       ├── Reader/
│       │   ├── OliveAnimGraphSerializer.cpp
│       │   └── OliveWidgetTreeSerializer.cpp
│       └── Pipeline/
│           └── OliveWritePipeline.cpp
```

### 2.2 Files to Modify

| File | Modification |
|------|--------------|
| `OliveToolRegistry.h` | Add `RegisterBlueprintTools()` declaration |
| `OliveToolRegistry.cpp` | Replace stubs with calls to tool handlers |
| `OliveBlueprintReader.h` | Add AnimGraph/WidgetTree read methods |
| `OliveBlueprintReader.cpp` | Integrate new serializers |

---

## 3. Interface Definitions

### 3.1 Write Pipeline Service

The core orchestration service that enforces the 6-stage safety pipeline for all write operations.

```cpp
// OliveWritePipeline.h
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/OliveValidationEngine.h"
#include "Services/OliveTransactionManager.h"
#include "Settings/OliveAISettings.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveWritePipeline, Log, All);

/**
 * Stage of the write pipeline
 */
enum class EOliveWriteStage : uint8
{
    Validate,       // Stage 1: Input validation and precondition checks
    Confirm,        // Stage 2: Tier routing and confirmation (for built-in chat)
    Transact,       // Stage 3: Open transaction and mark objects dirty
    Execute,        // Stage 4: Perform the mutation
    Verify,         // Stage 5: Structural checks and optional compilation
    Report          // Stage 6: Generate structured result
};

/**
 * Confirmation requirement from tier routing
 */
UENUM()
enum class EOliveConfirmationRequirement : uint8
{
    None,           // Tier 1: Auto-execute
    PlanConfirm,    // Tier 2: Show plan, require confirmation
    PreviewOnly     // Tier 3: Generate preview, no execution without explicit approval
};

/**
 * Write operation request - input to the pipeline
 */
struct OLIVEAIEDITOR_API FOliveWriteRequest
{
    /** Tool name for logging and tier lookup */
    FString ToolName;

    /** Tool parameters as JSON */
    TSharedPtr<FJsonObject> Params;

    /** Target asset path (resolved before pipeline) */
    FString AssetPath;

    /** Target asset (loaded before pipeline, may be null for create operations) */
    UObject* TargetAsset = nullptr;

    /** Operation description for transaction naming */
    FText OperationDescription;

    /** Operation category for tier lookup */
    FString OperationCategory;

    /** Whether this request came from MCP (skips confirmation) or built-in chat */
    bool bFromMCP = false;

    /** Whether to auto-compile after write (from settings) */
    bool bAutoCompile = true;

    /** Whether to skip verification (for batch operations) */
    bool bSkipVerification = false;
};

/**
 * Write operation result - output from the pipeline
 */
struct OLIVEAIEDITOR_API FOliveWriteResult
{
    /** Overall success status */
    bool bSuccess = false;

    /** Which stage completed last (for debugging) */
    EOliveWriteStage CompletedStage = EOliveWriteStage::Validate;

    /** Confirmation requirement (if stopped at Confirm stage) */
    EOliveConfirmationRequirement ConfirmationRequired = EOliveConfirmationRequirement::None;

    /** Plan description (if confirmation required) */
    FString PlanDescription;

    /** Preview data (if Tier 3 preview generated) */
    TSharedPtr<FJsonObject> PreviewData;

    /** Validation messages */
    TArray<FOliveIRMessage> ValidationMessages;

    /** Operation result data */
    TSharedPtr<FJsonObject> ResultData;

    /** Compile result (if verification included compilation) */
    TOptional<FOliveIRCompileResult> CompileResult;

    /** Execution time in milliseconds */
    double ExecutionTimeMs = 0.0;

    /** Created item name/ID (for creation operations) */
    FString CreatedItem;

    /** Created node IDs (for graph operations) */
    TArray<FString> CreatedNodeIds;

    /** Convert to tool result format */
    FOliveToolResult ToToolResult() const;

    /** Factory methods */
    static FOliveWriteResult ValidationError(const FOliveValidationResult& Result);
    static FOliveWriteResult ConfirmationNeeded(EOliveConfirmationRequirement Requirement, const FString& Plan);
    static FOliveWriteResult ExecutionError(const FString& Code, const FString& Message, const FString& Suggestion = TEXT(""));
    static FOliveWriteResult Success(const TSharedPtr<FJsonObject>& Data = nullptr);
};

/**
 * Delegate for the Execute stage - provided by each tool handler
 */
DECLARE_DELEGATE_RetVal_TwoParams(FOliveWriteResult, FOliveWriteExecutor, const FOliveWriteRequest&, UObject*);

/**
 * Write Pipeline Service
 *
 * Orchestrates the 6-stage write safety pipeline:
 * 1. Validate - Input validation, schema checking, precondition verification
 * 2. Confirm  - Tier routing, generate plan if Tier 2/3, await confirmation
 * 3. Transact - Open FScopedTransaction, call Modify() on target objects
 * 4. Execute  - Run the actual mutation via the provided executor delegate
 * 5. Verify   - Structural checks, optional compilation, consistency validation
 * 6. Report   - Assemble structured result with timing and diagnostics
 *
 * MCP clients skip stage 2 (Confirm) as they manage their own confirmation UX.
 * Built-in chat clients receive a ConfirmationNeeded result at stage 2 and must
 * re-submit with confirmation token to proceed.
 */
class OLIVEAIEDITOR_API FOliveWritePipeline
{
public:
    /** Get singleton instance */
    static FOliveWritePipeline& Get();

    /**
     * Execute a write operation through the full pipeline
     * @param Request The write request with all parameters
     * @param Executor Delegate that performs the actual mutation
     * @return Write result indicating success, failure, or confirmation needed
     */
    FOliveWriteResult Execute(const FOliveWriteRequest& Request, FOliveWriteExecutor Executor);

    /**
     * Execute with explicit confirmation (after user confirmed Tier 2/3)
     * @param Request The original write request
     * @param ConfirmationToken Token from previous confirmation response
     * @param Executor Delegate that performs the actual mutation
     * @return Write result
     */
    FOliveWriteResult ExecuteConfirmed(
        const FOliveWriteRequest& Request,
        const FString& ConfirmationToken,
        FOliveWriteExecutor Executor);

    /**
     * Generate a preview without executing (for Tier 3 operations)
     * @param Request The write request
     * @return Preview result with simulated changes
     */
    FOliveWriteResult GeneratePreview(const FOliveWriteRequest& Request);

private:
    FOliveWritePipeline() = default;

    // Non-copyable
    FOliveWritePipeline(const FOliveWritePipeline&) = delete;
    FOliveWritePipeline& operator=(const FOliveWritePipeline&) = delete;

    // ============================================================================
    // Pipeline Stages
    // ============================================================================

    /**
     * Stage 1: Validate inputs and preconditions
     */
    FOliveWriteResult StageValidate(const FOliveWriteRequest& Request);

    /**
     * Stage 2: Route confirmation tier
     * @return ConfirmationNeeded result if tier requires confirmation, otherwise empty
     */
    TOptional<FOliveWriteResult> StageConfirm(const FOliveWriteRequest& Request);

    /**
     * Stage 3: Open transaction (returns transaction wrapper)
     */
    TUniquePtr<FOliveTransactionManager::FScopedOliveTransaction> StageTransact(
        const FOliveWriteRequest& Request,
        UObject* TargetAsset);

    /**
     * Stage 4: Execute the mutation
     */
    FOliveWriteResult StageExecute(
        const FOliveWriteRequest& Request,
        UObject* TargetAsset,
        FOliveWriteExecutor& Executor);

    /**
     * Stage 5: Verify result (structural checks + optional compile)
     */
    FOliveWriteResult StageVerify(
        const FOliveWriteRequest& Request,
        UObject* TargetAsset,
        const FOliveWriteResult& ExecuteResult);

    /**
     * Stage 6: Assemble final report
     */
    FOliveWriteResult StageReport(
        const FOliveWriteRequest& Request,
        const FOliveWriteResult& VerifyResult,
        double TotalTimeMs);

    // ============================================================================
    // Tier Routing
    // ============================================================================

    /**
     * Determine confirmation tier for an operation
     */
    EOliveConfirmationTier GetOperationTier(const FString& OperationCategory) const;

    /**
     * Convert tier to confirmation requirement
     */
    EOliveConfirmationRequirement TierToRequirement(EOliveConfirmationTier Tier) const;

    /**
     * Generate a human-readable plan description for Tier 2
     */
    FString GeneratePlanDescription(const FOliveWriteRequest& Request) const;

    // ============================================================================
    // Verification
    // ============================================================================

    /**
     * Run structural verification on a Blueprint
     */
    bool VerifyBlueprintStructure(UBlueprint* Blueprint, TArray<FOliveIRMessage>& OutMessages) const;

    /**
     * Compile and gather errors
     */
    FOliveIRCompileResult CompileAndGatherErrors(UBlueprint* Blueprint) const;

    // ============================================================================
    // State
    // ============================================================================

    /** Pending confirmations: Token -> Request */
    TMap<FString, FOliveWriteRequest> PendingConfirmations;

    /** Lock for pending confirmations map */
    FCriticalSection ConfirmationLock;

    /** Generate unique confirmation token */
    FString GenerateConfirmationToken();
};
```

### 3.2 Blueprint Tool Handlers

The main class that registers and handles all Blueprint MCP tools.

```cpp
// OliveBlueprintToolHandlers.h
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

// Forward declarations
class FOliveWritePipeline;
class FOliveBlueprintReader;
class FOliveBlueprintWriter;
class FOliveGraphWriter;
class FOliveCompileManager;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveBPTools, Log, All);

/**
 * Blueprint Tool Handlers
 *
 * Registers and handles all Blueprint-related MCP tools.
 * Acts as a bridge between the MCP tool registry and the
 * Blueprint reader/writer infrastructure.
 *
 * Tool Categories:
 * - Readers: blueprint.read, read_function, read_event_graph, etc.
 * - Asset Writers: blueprint.create, set_parent_class, add_interface, etc.
 * - Variable Writers: blueprint.add_variable, remove_variable, modify_variable
 * - Component Writers: blueprint.add_component, remove_component, etc.
 * - Function Writers: blueprint.add_function, override_function, etc.
 * - Graph Writers: blueprint.add_node, connect_pins, set_pin_default, etc.
 * - AnimBP Writers: animbp.add_state_machine, add_state, add_transition, etc.
 * - Widget Writers: widget.add_widget, remove_widget, bind_property, etc.
 */
class OLIVEAIEDITOR_API FOliveBlueprintToolHandlers
{
public:
    /** Get singleton instance */
    static FOliveBlueprintToolHandlers& Get();

    /**
     * Register all Blueprint tools with the tool registry
     * Called during module startup after core services are initialized
     */
    void RegisterAllTools();

    /**
     * Unregister all Blueprint tools
     * Called during module shutdown
     */
    void UnregisterAllTools();

private:
    FOliveBlueprintToolHandlers() = default;

    // Non-copyable
    FOliveBlueprintToolHandlers(const FOliveBlueprintToolHandlers&) = delete;
    FOliveBlueprintToolHandlers& operator=(const FOliveBlueprintToolHandlers&) = delete;

    // ============================================================================
    // Tool Registration Helpers
    // ============================================================================

    void RegisterReaderTools();
    void RegisterAssetWriterTools();
    void RegisterVariableWriterTools();
    void RegisterComponentWriterTools();
    void RegisterFunctionWriterTools();
    void RegisterGraphWriterTools();
    void RegisterAnimBPWriterTools();
    void RegisterWidgetWriterTools();

    // ============================================================================
    // Reader Tool Handlers
    // ============================================================================

    FOliveToolResult HandleBlueprintRead(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintReadFunction(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintReadEventGraph(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintReadVariables(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintReadComponents(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintReadHierarchy(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintListOverridableFunctions(const TSharedPtr<FJsonObject>& Params);

    // ============================================================================
    // Asset Writer Tool Handlers
    // ============================================================================

    FOliveToolResult HandleBlueprintCreate(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintSetParentClass(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintAddInterface(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintRemoveInterface(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintCompile(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintDelete(const TSharedPtr<FJsonObject>& Params);

    // ============================================================================
    // Variable Writer Tool Handlers
    // ============================================================================

    FOliveToolResult HandleBlueprintAddVariable(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintRemoveVariable(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintModifyVariable(const TSharedPtr<FJsonObject>& Params);

    // ============================================================================
    // Component Writer Tool Handlers
    // ============================================================================

    FOliveToolResult HandleBlueprintAddComponent(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintRemoveComponent(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintModifyComponent(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintReparentComponent(const TSharedPtr<FJsonObject>& Params);

    // ============================================================================
    // Function Writer Tool Handlers
    // ============================================================================

    FOliveToolResult HandleBlueprintAddFunction(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintRemoveFunction(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintModifyFunctionSignature(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintAddEventDispatcher(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintOverrideFunction(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintAddCustomEvent(const TSharedPtr<FJsonObject>& Params);

    // ============================================================================
    // Graph Writer Tool Handlers
    // ============================================================================

    FOliveToolResult HandleBlueprintAddNode(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintRemoveNode(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintConnectPins(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintDisconnectPins(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintSetPinDefault(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintSetNodeProperty(const TSharedPtr<FJsonObject>& Params);

    // ============================================================================
    // AnimBP Writer Tool Handlers
    // ============================================================================

    FOliveToolResult HandleAnimBPAddStateMachine(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleAnimBPAddState(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleAnimBPAddTransition(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleAnimBPSetTransitionRule(const TSharedPtr<FJsonObject>& Params);

    // ============================================================================
    // Widget Writer Tool Handlers
    // ============================================================================

    FOliveToolResult HandleWidgetAddWidget(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleWidgetRemoveWidget(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleWidgetSetProperty(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleWidgetBindProperty(const TSharedPtr<FJsonObject>& Params);

    // ============================================================================
    // Common Helpers
    // ============================================================================

    /**
     * Parse required path parameter and load Blueprint
     * @param Params Tool parameters
     * @param OutBlueprint Loaded Blueprint (output)
     * @param OutError Error result if failed (output)
     * @return True if Blueprint loaded successfully
     */
    bool LoadBlueprintFromParams(
        const TSharedPtr<FJsonObject>& Params,
        UBlueprint*& OutBlueprint,
        FOliveToolResult& OutError);

    /**
     * Build a write request from tool parameters
     */
    FOliveWriteRequest BuildWriteRequest(
        const FString& ToolName,
        const TSharedPtr<FJsonObject>& Params,
        const FString& OperationCategory,
        const FText& Description);

    /**
     * Parse type specification from JSON to FOliveIRType
     */
    FOliveIRType ParseTypeFromParams(const TSharedPtr<FJsonObject>& TypeJson);

    /**
     * Parse function signature from JSON to FOliveIRFunctionSignature
     */
    FOliveIRFunctionSignature ParseFunctionSignatureFromParams(const TSharedPtr<FJsonObject>& SigJson);

    /**
     * Parse variable definition from JSON to FOliveIRVariable
     */
    FOliveIRVariable ParseVariableFromParams(const TSharedPtr<FJsonObject>& VarJson);

    // ============================================================================
    // Registered Tool Names (for cleanup)
    // ============================================================================

    TArray<FString> RegisteredToolNames;
};
```

### 3.3 JSON Schema Definitions

Centralized schema definitions for all Blueprint tools.

```cpp
// OliveBlueprintSchemas.h
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Blueprint Tool Schema Builder
 *
 * Provides JSON Schema definitions for all Blueprint MCP tools.
 * Schemas follow JSON Schema Draft 7 format for MCP compatibility.
 */
namespace OliveBlueprintSchemas
{
    // ============================================================================
    // Common Schema Components
    // ============================================================================

    /** Create a string property schema */
    TSharedPtr<FJsonObject> StringProp(const FString& Description, bool bRequired = false);

    /** Create an integer property schema */
    TSharedPtr<FJsonObject> IntProp(const FString& Description, int32 Min = 0, int32 Max = INT32_MAX);

    /** Create a boolean property schema */
    TSharedPtr<FJsonObject> BoolProp(const FString& Description, bool DefaultValue = false);

    /** Create an array property schema */
    TSharedPtr<FJsonObject> ArrayProp(const FString& Description, TSharedPtr<FJsonObject> ItemSchema);

    /** Create an object property schema */
    TSharedPtr<FJsonObject> ObjectProp(const FString& Description, TSharedPtr<FJsonObject> Properties);

    /** Type specification schema (for variable/parameter types) */
    TSharedPtr<FJsonObject> TypeSpecSchema();

    /** Function parameter schema */
    TSharedPtr<FJsonObject> FunctionParamSchema();

    /** Function signature schema */
    TSharedPtr<FJsonObject> FunctionSignatureSchema();

    /** Variable definition schema */
    TSharedPtr<FJsonObject> VariableSchema();

    // ============================================================================
    // Reader Tool Schemas
    // ============================================================================

    TSharedPtr<FJsonObject> BlueprintRead();
    TSharedPtr<FJsonObject> BlueprintReadFunction();
    TSharedPtr<FJsonObject> BlueprintReadEventGraph();
    TSharedPtr<FJsonObject> BlueprintReadVariables();
    TSharedPtr<FJsonObject> BlueprintReadComponents();
    TSharedPtr<FJsonObject> BlueprintReadHierarchy();
    TSharedPtr<FJsonObject> BlueprintListOverridableFunctions();

    // ============================================================================
    // Asset Writer Tool Schemas
    // ============================================================================

    TSharedPtr<FJsonObject> BlueprintCreate();
    TSharedPtr<FJsonObject> BlueprintSetParentClass();
    TSharedPtr<FJsonObject> BlueprintAddInterface();
    TSharedPtr<FJsonObject> BlueprintRemoveInterface();
    TSharedPtr<FJsonObject> BlueprintCompile();
    TSharedPtr<FJsonObject> BlueprintDelete();

    // ============================================================================
    // Variable Writer Tool Schemas
    // ============================================================================

    TSharedPtr<FJsonObject> BlueprintAddVariable();
    TSharedPtr<FJsonObject> BlueprintRemoveVariable();
    TSharedPtr<FJsonObject> BlueprintModifyVariable();

    // ============================================================================
    // Component Writer Tool Schemas
    // ============================================================================

    TSharedPtr<FJsonObject> BlueprintAddComponent();
    TSharedPtr<FJsonObject> BlueprintRemoveComponent();
    TSharedPtr<FJsonObject> BlueprintModifyComponent();
    TSharedPtr<FJsonObject> BlueprintReparentComponent();

    // ============================================================================
    // Function Writer Tool Schemas
    // ============================================================================

    TSharedPtr<FJsonObject> BlueprintAddFunction();
    TSharedPtr<FJsonObject> BlueprintRemoveFunction();
    TSharedPtr<FJsonObject> BlueprintModifyFunctionSignature();
    TSharedPtr<FJsonObject> BlueprintAddEventDispatcher();
    TSharedPtr<FJsonObject> BlueprintOverrideFunction();
    TSharedPtr<FJsonObject> BlueprintAddCustomEvent();

    // ============================================================================
    // Graph Writer Tool Schemas
    // ============================================================================

    TSharedPtr<FJsonObject> BlueprintAddNode();
    TSharedPtr<FJsonObject> BlueprintRemoveNode();
    TSharedPtr<FJsonObject> BlueprintConnectPins();
    TSharedPtr<FJsonObject> BlueprintDisconnectPins();
    TSharedPtr<FJsonObject> BlueprintSetPinDefault();
    TSharedPtr<FJsonObject> BlueprintSetNodeProperty();

    // ============================================================================
    // AnimBP Writer Tool Schemas
    // ============================================================================

    TSharedPtr<FJsonObject> AnimBPAddStateMachine();
    TSharedPtr<FJsonObject> AnimBPAddState();
    TSharedPtr<FJsonObject> AnimBPAddTransition();
    TSharedPtr<FJsonObject> AnimBPSetTransitionRule();

    // ============================================================================
    // Widget Writer Tool Schemas
    // ============================================================================

    TSharedPtr<FJsonObject> WidgetAddWidget();
    TSharedPtr<FJsonObject> WidgetRemoveWidget();
    TSharedPtr<FJsonObject> WidgetSetProperty();
    TSharedPtr<FJsonObject> WidgetBindProperty();
}
```

### 3.4 AnimGraph Serializer

Specialized serializer for Animation Blueprint state machines and anim graphs.

```cpp
// OliveAnimGraphSerializer.h
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintIR.h"

// Forward declarations
class UAnimBlueprint;
class UAnimGraphNode_Base;
class UAnimGraphNode_StateMachine;
class UAnimStateNodeBase;
class UAnimStateTransitionNode;
class UEdGraph;
class UAnimationStateMachineGraph;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveAnimSerializer, Log, All);

/**
 * AnimGraph Serializer
 *
 * Specialized serializer for Animation Blueprint structures.
 * Handles:
 * - AnimGraph root and sub-graphs
 * - State machine hierarchies
 * - States (including conduits)
 * - Transitions with rules
 * - Animation node properties
 *
 * This serializer produces dedicated structured payloads rather than
 * falling back to generic graph serialization, enabling AI agents to
 * understand animation flow and state machine structure.
 */
class OLIVEAIEDITOR_API FOliveAnimGraphSerializer
{
public:
    FOliveAnimGraphSerializer();
    ~FOliveAnimGraphSerializer() = default;

    // ============================================================================
    // High-Level Read Methods
    // ============================================================================

    /**
     * Read all state machines from an Animation Blueprint
     * @param AnimBlueprint The Animation Blueprint to read from
     * @return Array of state machine IR structures
     */
    TArray<FOliveIRAnimStateMachine> ReadStateMachines(const UAnimBlueprint* AnimBlueprint);

    /**
     * Read a specific state machine by name
     * @param AnimBlueprint The Animation Blueprint
     * @param StateMachineName Name of the state machine
     * @return The state machine IR, or empty optional if not found
     */
    TOptional<FOliveIRAnimStateMachine> ReadStateMachine(
        const UAnimBlueprint* AnimBlueprint,
        const FString& StateMachineName);

    /**
     * Read the AnimGraph as a simplified IR (without full node data)
     * @param AnimBlueprint The Animation Blueprint
     * @return AnimGraph summary with state machine references
     */
    TSharedPtr<FJsonObject> ReadAnimGraphSummary(const UAnimBlueprint* AnimBlueprint);

    /**
     * Read full AnimGraph with all node data
     * @param AnimBlueprint The Animation Blueprint
     * @return Full AnimGraph IR including state machine details
     */
    FOliveIRGraph ReadAnimGraphFull(const UAnimBlueprint* AnimBlueprint);

private:
    // ============================================================================
    // State Machine Reading
    // ============================================================================

    /**
     * Serialize a state machine graph to IR
     */
    FOliveIRAnimStateMachine SerializeStateMachine(
        const UAnimationStateMachineGraph* StateMachineGraph,
        const UAnimBlueprint* OwningBlueprint);

    /**
     * Find all state machine nodes in the AnimGraph
     */
    TArray<UAnimGraphNode_StateMachine*> FindStateMachineNodes(const UAnimBlueprint* AnimBlueprint);

    // ============================================================================
    // State Reading
    // ============================================================================

    /**
     * Serialize a single state to IR
     */
    FOliveIRAnimState SerializeState(
        const UAnimStateNodeBase* StateNode,
        const UAnimationStateMachineGraph* OwningGraph);

    /**
     * Get the animation asset referenced by a state (if any)
     */
    FString GetStateAnimationAsset(const UAnimStateNodeBase* StateNode);

    /**
     * Check if a state is a conduit
     */
    bool IsConduitState(const UAnimStateNodeBase* StateNode) const;

    // ============================================================================
    // Transition Reading
    // ============================================================================

    /**
     * Get all transitions into a state
     */
    TArray<FString> GetTransitionsIn(
        const UAnimStateNodeBase* StateNode,
        const UAnimationStateMachineGraph* Graph);

    /**
     * Get all transitions out of a state
     */
    TArray<FString> GetTransitionsOut(
        const UAnimStateNodeBase* StateNode,
        const UAnimationStateMachineGraph* Graph);

    /**
     * Serialize a transition rule to a description string
     */
    FString SerializeTransitionRule(const UAnimStateTransitionNode* TransitionNode);

    // ============================================================================
    // AnimGraph Node Reading
    // ============================================================================

    /**
     * Serialize an AnimGraph node to generic IR node format
     */
    FOliveIRNode SerializeAnimNode(const UAnimGraphNode_Base* AnimNode);

    /**
     * Extract animation-specific properties from a node
     */
    TMap<FString, FString> ExtractAnimNodeProperties(const UAnimGraphNode_Base* AnimNode);
};
```

### 3.5 Widget Tree Serializer

Specialized serializer for Widget Blueprint widget hierarchies.

```cpp
// OliveWidgetTreeSerializer.h
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintIR.h"

// Forward declarations
class UWidgetBlueprint;
class UWidget;
class UPanelWidget;
class UWidgetTree;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveWidgetSerializer, Log, All);

/**
 * Widget Tree Serializer
 *
 * Specialized serializer for Widget Blueprint widget hierarchies.
 * Handles:
 * - Widget tree structure with parent/child relationships
 * - Widget properties (visibility, transform, anchors, etc.)
 * - Slot properties (canvas slots, box slots, etc.)
 * - Widget bindings (dynamic properties)
 * - Named slots for compound widgets
 *
 * This serializer produces FOliveIRWidgetNode structures that capture
 * the full widget hierarchy, enabling AI agents to understand and
 * manipulate UMG layouts.
 */
class OLIVEAIEDITOR_API FOliveWidgetTreeSerializer
{
public:
    FOliveWidgetTreeSerializer();
    ~FOliveWidgetTreeSerializer() = default;

    // ============================================================================
    // High-Level Read Methods
    // ============================================================================

    /**
     * Read the entire widget tree from a Widget Blueprint
     * @param WidgetBlueprint The Widget Blueprint to read from
     * @return Root widget node with all children recursively populated
     */
    TOptional<FOliveIRWidgetNode> ReadWidgetTree(const UWidgetBlueprint* WidgetBlueprint);

    /**
     * Read a specific widget by name
     * @param WidgetBlueprint The Widget Blueprint
     * @param WidgetName Name of the widget to read
     * @return The widget IR, or empty optional if not found
     */
    TOptional<FOliveIRWidgetNode> ReadWidget(
        const UWidgetBlueprint* WidgetBlueprint,
        const FString& WidgetName);

    /**
     * Read widget tree as flat list (for simpler processing)
     * @param WidgetBlueprint The Widget Blueprint
     * @return Array of all widgets in depth-first order
     */
    TArray<FOliveIRWidgetNode> ReadWidgetTreeFlat(const UWidgetBlueprint* WidgetBlueprint);

    /**
     * Get widget tree summary (names and types only, no properties)
     * @param WidgetBlueprint The Widget Blueprint
     * @return JSON summary of widget structure
     */
    TSharedPtr<FJsonObject> ReadWidgetTreeSummary(const UWidgetBlueprint* WidgetBlueprint);

private:
    // ============================================================================
    // Widget Tree Traversal
    // ============================================================================

    /**
     * Recursively serialize a widget and all its children
     */
    FOliveIRWidgetNode SerializeWidget(const UWidget* Widget);

    /**
     * Get children of a panel widget
     */
    TArray<UWidget*> GetChildWidgets(const UWidget* Widget);

    /**
     * Check if a widget is a panel (can have children)
     */
    bool IsPanel(const UWidget* Widget) const;

    // ============================================================================
    // Widget Property Reading
    // ============================================================================

    /**
     * Extract common widget properties (visibility, render transform, etc.)
     */
    TMap<FString, FString> ExtractWidgetProperties(const UWidget* Widget);

    /**
     * Extract slot-specific properties (anchors for canvas, alignment for box, etc.)
     */
    FString DetermineSlotType(const UWidget* Widget);

    /**
     * Read slot properties for a widget
     */
    TMap<FString, FString> ExtractSlotProperties(const UWidget* Widget);

    // ============================================================================
    // Widget Binding Reading
    // ============================================================================

    /**
     * Check if a widget has any property bindings
     */
    bool HasBindings(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget);

    /**
     * Get all property bindings for a widget
     * Returns map of property name -> binding expression
     */
    TMap<FString, FString> GetWidgetBindings(
        const UWidgetBlueprint* WidgetBlueprint,
        const UWidget* Widget);

    // ============================================================================
    // Named Slot Reading
    // ============================================================================

    /**
     * Check if a widget has named slots (UserWidget, etc.)
     */
    bool HasNamedSlots(const UWidget* Widget) const;

    /**
     * Get named slot information
     */
    TMap<FString, FString> GetNamedSlots(const UWidget* Widget);
};
```

---

## 4. Data Flow

### 4.1 Read Operation Flow

```
MCP Request (JSON-RPC)
    |
    v
FOliveToolRegistry::ExecuteTool()
    |
    v
FOliveBlueprintToolHandlers::HandleBlueprintRead()
    |
    +---> FOliveBlueprintReader::ReadBlueprintSummary() (summary mode)
    |           |
    |           +---> FOliveComponentReader::ReadComponents()
    |           +---> FOliveGraphReader::ReadGraph() [names only]
    |           +---> FOliveAnimGraphSerializer::ReadAnimGraphSummary() [if AnimBP]
    |           +---> FOliveWidgetTreeSerializer::ReadWidgetTreeSummary() [if WidgetBP]
    |
    +---> FOliveBlueprintReader::ReadBlueprintFull() (full mode)
            |
            +---> [All summary reads]
            +---> FOliveGraphReader::ReadGraph() [with nodes]
            +---> FOliveAnimGraphSerializer::ReadStateMachines() [if AnimBP]
            +---> FOliveWidgetTreeSerializer::ReadWidgetTree() [if WidgetBP]
    |
    v
FOliveIRBlueprint.ToJson()
    |
    v
FOliveToolResult::Success(Data)
    |
    v
JSON-RPC Response
```

### 4.2 Write Operation Flow (Through Pipeline)

```
MCP Request (JSON-RPC)
    |
    v
FOliveToolRegistry::ExecuteTool()
    |
    v
FOliveBlueprintToolHandlers::HandleBlueprintAddVariable()
    |
    +---> BuildWriteRequest(toolName, params, "variable", description)
    |
    v
FOliveWritePipeline::Execute(request, executor)
    |
    +===> Stage 1: VALIDATE
    |       +---> FOliveValidationEngine::ValidateOperation()
    |       +---> Schema validation
    |       +---> Asset existence check
    |       +---> Blueprint type constraints
    |
    +===> Stage 2: CONFIRM (skipped for MCP)
    |       [Built-in chat: Check tier, possibly return ConfirmationNeeded]
    |
    +===> Stage 3: TRANSACT
    |       +---> FScopedOliveTransaction(description)
    |       +---> Blueprint->Modify()
    |
    +===> Stage 4: EXECUTE
    |       +---> executor.Execute()  // Calls FOliveBlueprintWriter::AddVariable()
    |       |
    |       v
    |   FOliveBlueprintWriter::AddVariable()
    |       +---> Create FBPVariableDescription
    |       +---> Add to Blueprint->NewVariables
    |       +---> FBlueprintEditorUtils::MarkBlueprintAsModified()
    |
    +===> Stage 5: VERIFY
    |       +---> VerifyBlueprintStructure() [check variable exists]
    |       +---> FOliveCompileManager::Compile() [if bAutoCompile]
    |
    +===> Stage 6: REPORT
            +---> Assemble FOliveWriteResult
            +---> ToToolResult()
    |
    v
JSON-RPC Response
```

### 4.3 Confirmation Flow (Built-in Chat Only)

```
Chat UI Request
    |
    v
FOliveWritePipeline::Execute(request, executor)
    |
    +---> Stage 1: VALIDATE [pass]
    |
    +---> Stage 2: CONFIRM
    |       +---> GetOperationTier("function_creation") -> Tier2
    |       +---> GeneratePlanDescription()
    |       +---> Store request with confirmation token
    |       +---> Return ConfirmationNeeded result
    |
    v
Chat UI: Display plan, await user confirmation
    |
    [User clicks "Confirm"]
    |
    v
FOliveWritePipeline::ExecuteConfirmed(request, token, executor)
    |
    +---> Validate token matches pending request
    +---> Continue from Stage 3: TRANSACT
    +---> Stage 4: EXECUTE
    +---> Stage 5: VERIFY
    +---> Stage 6: REPORT
    |
    v
Chat UI: Display result
```

---

## 5. MCP Tool Specifications

### 5.1 Reader Tools

| Tool Name | Description | Input Schema | Output |
|-----------|-------------|--------------|--------|
| `blueprint.read` | Read Blueprint structure | `{path: string, mode?: "summary"|"full"}` | `FOliveIRBlueprint` |
| `blueprint.read_function` | Read single function graph | `{path: string, function_name: string}` | `FOliveIRGraph` |
| `blueprint.read_event_graph` | Read event graph | `{path: string, graph_name?: string}` | `FOliveIRGraph` |
| `blueprint.read_variables` | Read all variables | `{path: string}` | `{variables: FOliveIRVariable[]}` |
| `blueprint.read_components` | Read component tree | `{path: string}` | `{components: FOliveIRComponent[], root: string}` |
| `blueprint.read_hierarchy` | Read class hierarchy | `{path: string}` | `{hierarchy: string[]}` |
| `blueprint.list_overridable_functions` | List overridable functions | `{path: string}` | `{functions: FOliveIRFunctionSignature[]}` |

### 5.2 Writer Tools (Asset-Level)

| Tool Name | Description | Tier | Input Schema |
|-----------|-------------|------|--------------|
| `blueprint.create` | Create new Blueprint | 1 | `{path: string, parent_class: string, type?: string}` |
| `blueprint.set_parent_class` | Change parent class | 3 | `{path: string, new_parent: string}` |
| `blueprint.add_interface` | Implement interface | 2 | `{path: string, interface: string}` |
| `blueprint.remove_interface` | Remove interface | 2 | `{path: string, interface: string}` |
| `blueprint.compile` | Force compile | 1 | `{path: string}` |
| `blueprint.delete` | Delete Blueprint | 3 | `{path: string}` |

### 5.3 Writer Tools (Variables)

| Tool Name | Description | Tier | Input Schema |
|-----------|-------------|------|--------------|
| `blueprint.add_variable` | Add variable | 1 | `{path: string, variable: VariableSpec}` |
| `blueprint.remove_variable` | Remove variable | 2 | `{path: string, name: string}` |
| `blueprint.modify_variable` | Modify variable | 1 | `{path: string, name: string, changes: object}` |

### 5.4 Writer Tools (Components)

| Tool Name | Description | Tier | Input Schema |
|-----------|-------------|------|--------------|
| `blueprint.add_component` | Add component | 1 | `{path: string, class: string, name?: string, parent?: string}` |
| `blueprint.remove_component` | Remove component | 2 | `{path: string, name: string}` |
| `blueprint.modify_component` | Modify component | 1 | `{path: string, name: string, properties: object}` |
| `blueprint.reparent_component` | Change component parent | 2 | `{path: string, name: string, new_parent: string}` |

### 5.5 Writer Tools (Functions)

| Tool Name | Description | Tier | Input Schema |
|-----------|-------------|------|--------------|
| `blueprint.add_function` | Add function | 2 | `{path: string, signature: FunctionSignature}` |
| `blueprint.remove_function` | Remove function | 2 | `{path: string, name: string}` |
| `blueprint.modify_function_signature` | Modify signature | 2 | `{path: string, name: string, changes: object}` |
| `blueprint.add_event_dispatcher` | Add dispatcher | 1 | `{path: string, name: string, params?: Param[]}` |
| `blueprint.override_function` | Override parent function | 2 | `{path: string, function_name: string}` |
| `blueprint.add_custom_event` | Add custom event | 1 | `{path: string, name: string, params?: Param[]}` |

### 5.6 Writer Tools (Graph Editing)

| Tool Name | Description | Tier | Input Schema |
|-----------|-------------|------|--------------|
| `blueprint.add_node` | Add node to graph | 2 | `{path: string, graph: string, type: string, properties?: object, pos_x?: int, pos_y?: int}` |
| `blueprint.remove_node` | Remove node | 2 | `{path: string, graph: string, node_id: string}` |
| `blueprint.connect_pins` | Connect two pins | 2 | `{path: string, graph: string, source: string, target: string}` |
| `blueprint.disconnect_pins` | Disconnect pins | 2 | `{path: string, graph: string, source: string, target: string}` |
| `blueprint.set_pin_default` | Set pin default | 2 | `{path: string, graph: string, pin: string, value: string}` |
| `blueprint.set_node_property` | Set node property | 2 | `{path: string, graph: string, node_id: string, property: string, value: string}` |

### 5.7 Writer Tools (AnimBP)

| Tool Name | Description | Tier | Input Schema |
|-----------|-------------|------|--------------|
| `animbp.add_state_machine` | Add state machine | 2 | `{path: string, name: string}` |
| `animbp.add_state` | Add state to machine | 2 | `{path: string, machine: string, name: string, animation?: string}` |
| `animbp.add_transition` | Add transition | 2 | `{path: string, machine: string, from: string, to: string}` |
| `animbp.set_transition_rule` | Set transition rule | 2 | `{path: string, machine: string, from: string, to: string, rule: object}` |

### 5.8 Writer Tools (Widget BP)

| Tool Name | Description | Tier | Input Schema |
|-----------|-------------|------|--------------|
| `widget.add_widget` | Add widget | 2 | `{path: string, class: string, parent?: string, slot?: string, name?: string}` |
| `widget.remove_widget` | Remove widget | 2 | `{path: string, name: string}` |
| `widget.set_property` | Set widget property | 2 | `{path: string, widget: string, property: string, value: string}` |
| `widget.bind_property` | Bind to function | 2 | `{path: string, widget: string, property: string, function: string}` |

---

## 6. Integration Points

### 6.1 Module Startup Integration

The tool handlers must be registered during module startup.

```cpp
// In OliveAIEditorModule.cpp, in StartupModule():

void FOliveAIEditorModule::StartupModule()
{
    // ... existing initialization ...

    // Register Blueprint tools (replaces stubs)
    FOliveBlueprintToolHandlers::Get().RegisterAllTools();

    UE_LOG(LogOliveAI, Log, TEXT("Blueprint MCP tools registered"));
}

void FOliveAIEditorModule::ShutdownModule()
{
    // ... existing shutdown ...

    // Unregister Blueprint tools
    FOliveBlueprintToolHandlers::Get().UnregisterAllTools();
}
```

### 6.2 Tool Registry Modification

Replace the stub registration with a forward call.

```cpp
// In OliveToolRegistry.cpp, modify RegisterBuiltInTools():

void FOliveToolRegistry::RegisterBuiltInTools()
{
    RegisterProjectTools();
    // Remove: RegisterBlueprintToolStubs();
    // Blueprint tools now registered by FOliveBlueprintToolHandlers

    UE_LOG(LogOliveAI, Log, TEXT("Registered %d built-in tools"), GetToolCount());
}
```

### 6.3 Reader Extensions

Add AnimGraph/WidgetTree integration to the main reader.

```cpp
// In OliveBlueprintReader.h, add:

class FOliveAnimGraphSerializer;
class FOliveWidgetTreeSerializer;

// ... inside class ...

/**
 * Read AnimGraph state machines from an Animation Blueprint
 * @param AnimBlueprint The Animation Blueprint to read from
 * @return Array of state machine IR structures
 */
TArray<FOliveIRAnimStateMachine> ReadAnimGraphStateMachines(const UAnimBlueprint* AnimBlueprint);

/**
 * Read widget tree from a Widget Blueprint
 * @param WidgetBlueprint The Widget Blueprint to read from
 * @return Root widget node with full hierarchy
 */
TOptional<FOliveIRWidgetNode> ReadWidgetTree(const UWidgetBlueprint* WidgetBlueprint);

// ... private members ...

TSharedPtr<FOliveAnimGraphSerializer> AnimGraphSerializer;
TSharedPtr<FOliveWidgetTreeSerializer> WidgetTreeSerializer;
```

### 6.4 Settings Integration

The write pipeline reads confirmation tiers from settings.

```cpp
// In OliveWritePipeline.cpp:

EOliveConfirmationTier FOliveWritePipeline::GetOperationTier(const FString& OperationCategory) const
{
    UOliveAISettings* Settings = UOliveAISettings::Get();
    if (!Settings)
    {
        return EOliveConfirmationTier::Tier2_PlanConfirm; // Safe default
    }

    if (OperationCategory == TEXT("variable"))
    {
        return Settings->VariableOperationsTier;
    }
    else if (OperationCategory == TEXT("component"))
    {
        return Settings->ComponentOperationsTier;
    }
    else if (OperationCategory == TEXT("function_creation"))
    {
        return Settings->FunctionCreationTier;
    }
    else if (OperationCategory == TEXT("graph_editing"))
    {
        return Settings->GraphEditingTier;
    }
    else if (OperationCategory == TEXT("refactoring"))
    {
        return Settings->RefactoringTier;
    }
    else if (OperationCategory == TEXT("delete"))
    {
        return Settings->DeleteOperationsTier;
    }

    return EOliveConfirmationTier::Tier2_PlanConfirm;
}
```

---

## 7. Error Handling

### 7.1 Error Categories

| Category | Code Prefix | Examples |
|----------|-------------|----------|
| Validation | `VALIDATION_` | `VALIDATION_MISSING_PARAM`, `VALIDATION_INVALID_TYPE` |
| Asset | `ASSET_` | `ASSET_NOT_FOUND`, `ASSET_LOCKED`, `ASSET_WRONG_TYPE` |
| Blueprint | `BP_` | `BP_CONSTRAINT_VIOLATION`, `BP_COMPILE_ERROR` |
| Graph | `GRAPH_` | `GRAPH_NOT_FOUND`, `GRAPH_NODE_NOT_FOUND`, `GRAPH_PIN_INCOMPATIBLE` |
| Pipeline | `PIPELINE_` | `PIPELINE_TRANSACTION_FAILED`, `PIPELINE_VERIFY_FAILED` |
| Permission | `PERMISSION_` | `PERMISSION_PIE_ACTIVE`, `PERMISSION_TIER_DENIED` |

### 7.2 Structured Error Response Format

```json
{
    "success": false,
    "error": {
        "code": "BP_CONSTRAINT_VIOLATION",
        "message": "Interface Blueprints cannot have member variables",
        "suggestion": "Use a regular Blueprint if you need variables, or add the variable to an implementing Blueprint.",
        "details": {
            "blueprint_type": "Interface",
            "attempted_operation": "add_variable",
            "variable_name": "MyVar"
        }
    },
    "execution_time_ms": 12.5
}
```

### 7.3 Pipeline Stage Error Handling

| Stage | On Error | Behavior |
|-------|----------|----------|
| Validate | Structured error | Return immediately, no transaction |
| Confirm | N/A | ConfirmationNeeded is not an error |
| Transact | Transaction error | Return error, no execution |
| Execute | Execution error | Cancel transaction, return error |
| Verify | Verify error | Complete transaction (already executed), report warnings |
| Report | N/A | Always succeeds |

---

## 8. Implementation Order

The coder should implement these in order, with each task building on the previous.

### Task 1: Write Pipeline Service (Foundation)

**Files:**
- `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h`
- `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`

**Scope:**
- Implement `FOliveWriteRequest` and `FOliveWriteResult` structs
- Implement `FOliveWritePipeline` singleton with all 6 stages
- Implement tier routing based on settings
- MCP requests (bFromMCP=true) skip confirmation stage
- Built-in chat confirmation flow (ExecuteConfirmed) can be stubbed initially

**Verification:**
- Unit test with mock executor that passes through all stages
- Test tier routing returns correct requirement per category
- Test MCP flow skips confirmation

### Task 2: Blueprint Tool Schemas

**Files:**
- `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h`
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`

**Scope:**
- Implement all schema builder functions
- Follow JSON Schema Draft 7 format
- Include comprehensive descriptions for AI understanding
- Define TypeSpec, FunctionSignature, Variable schemas as reusable components

**Verification:**
- Schemas can be serialized to valid JSON
- All required fields marked correctly

### Task 3: Reader Tool Handlers

**Files:**
- `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h`
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (partial)

**Scope:**
- Implement singleton structure
- Implement `RegisterReaderTools()` with all 7 reader tools
- Implement all reader handler methods
- Handler methods call existing `FOliveBlueprintReader` methods
- Update `OliveToolRegistry` to call handlers instead of stubs

**Verification:**
- `blueprint.read` returns valid Blueprint IR for test asset
- `blueprint.read_function` returns function graph data
- `blueprint.read_variables` returns variable list
- Error cases handled (asset not found, wrong type)

### Task 4: AnimGraph Serializer

**Files:**
- `Source/OliveAIEditor/Blueprint/Public/Reader/OliveAnimGraphSerializer.h`
- `Source/OliveAIEditor/Blueprint/Private/Reader/OliveAnimGraphSerializer.cpp`

**Scope:**
- Implement state machine reading
- Implement state serialization with animation asset references
- Implement transition reading with rule descriptions
- Integrate with `FOliveBlueprintReader`

**Verification:**
- Read Animation Blueprint returns state machine data
- States include transition in/out lists
- Animation asset references extracted

### Task 5: Widget Tree Serializer

**Files:**
- `Source/OliveAIEditor/Blueprint/Public/Reader/OliveWidgetTreeSerializer.h`
- `Source/OliveAIEditor/Blueprint/Private/Reader/OliveWidgetTreeSerializer.cpp`

**Scope:**
- Implement recursive widget tree reading
- Implement slot type detection
- Implement property extraction
- Integrate with `FOliveBlueprintReader`

**Verification:**
- Read Widget Blueprint returns widget hierarchy
- Children correctly nested under parents
- Slot types identified (Canvas, HBox, VBox, etc.)

### Task 6: Asset-Level Writer Tools

**Files:**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (extend)

**Scope:**
- Implement `RegisterAssetWriterTools()`
- Implement `HandleBlueprintCreate()` using `FOliveBlueprintWriter::CreateBlueprint()`
- Implement `HandleBlueprintSetParentClass()` using `FOliveBlueprintWriter::SetParentClass()`
- Implement interface add/remove handlers
- Implement compile handler
- Implement delete handler (Tier 3)
- All handlers use write pipeline

**Verification:**
- `blueprint.create` creates new Blueprint
- `blueprint.compile` compiles and returns errors
- Transaction/undo works

### Task 7: Variable Writer Tools

**Files:**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (extend)

**Scope:**
- Implement `RegisterVariableWriterTools()`
- Implement add/remove/modify variable handlers
- Parse variable specification from JSON to IR
- All handlers use write pipeline

**Verification:**
- `blueprint.add_variable` creates variable with correct type
- Variable visible in readback
- Undo removes variable

### Task 8: Component Writer Tools

**Files:**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (extend)

**Scope:**
- Implement `RegisterComponentWriterTools()`
- Implement add/remove/modify/reparent handlers
- Use `FOliveComponentWriter` for mutations
- Handle constraint validation (SceneComponent children)

**Verification:**
- `blueprint.add_component` creates component in SCS
- Component hierarchy correct after reparent
- Non-scene component blocks child add

### Task 9: Function Writer Tools

**Files:**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (extend)

**Scope:**
- Implement `RegisterFunctionWriterTools()`
- Implement add function with signature parsing
- Implement override function
- Implement custom event and event dispatcher
- Parse function signatures from JSON to IR

**Verification:**
- `blueprint.add_function` creates function graph
- Function signature reflected in IR readback
- Override creates correct function with parent call

### Task 10: Graph Writer Tools

**Files:**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (extend)

**Scope:**
- Implement `RegisterGraphWriterTools()`
- Implement add/remove node using `FOliveGraphWriter`
- Implement connect/disconnect pins
- Implement set_pin_default and set_node_property
- Handle pin reference parsing ("node_id.pin_name")

**Verification:**
- `blueprint.add_node` creates node and returns ID
- `blueprint.connect_pins` creates connection
- Compile after graph changes

### Task 11: AnimBP Writer Tools

**Files:**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (extend)

**Scope:**
- Implement `RegisterAnimBPWriterTools()`
- Implement state machine creation
- Implement state addition
- Implement transition creation
- Implement transition rule setting

**Note:** These are more complex; may need additional writer infrastructure in `Source/OliveAIEditor/Blueprint/Public/Writer/OliveAnimGraphWriter.h`.

**Verification:**
- `animbp.add_state_machine` creates machine
- States and transitions readable via reader tools

### Task 12: Widget Writer Tools

**Files:**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (extend)

**Scope:**
- Implement `RegisterWidgetWriterTools()`
- Implement widget add/remove
- Implement property setting
- Implement binding creation

**Note:** These are more complex; may need additional writer infrastructure in `Source/OliveAIEditor/Blueprint/Public/Writer/OliveWidgetWriter.h`.

**Verification:**
- `widget.add_widget` creates widget
- Widget hierarchy readable via reader tools

---

## 9. Dependencies

### 9.1 Module Dependencies to Add

In `OliveAIEditor.Build.cs`, ensure these are present:

```csharp
// Already present but verify:
"BlueprintGraph",
"Kismet",
"KismetWidgets",
"GraphEditor",

// May need to add for AnimBP/Widget:
"AnimGraph",
"UMG",
"UMGEditor",
"AnimationBlueprintEditor",
```

### 9.2 Internal Dependencies

| Component | Depends On |
|-----------|------------|
| `FOliveBlueprintToolHandlers` | `FOliveToolRegistry`, `FOliveWritePipeline`, `FOliveBlueprintReader`, `FOliveBlueprintWriter`, `FOliveGraphWriter` |
| `FOliveWritePipeline` | `FOliveValidationEngine`, `FOliveTransactionManager`, `FOliveCompileManager`, `UOliveAISettings` |
| `FOliveAnimGraphSerializer` | `FOliveNodeSerializer`, `FOlivePinSerializer` |
| `FOliveWidgetTreeSerializer` | (standalone, uses UMG reflection) |

---

## 10. Summary for Coder

### What to Build

1. **Write Pipeline Service** - 6-stage orchestration with tier routing
2. **Blueprint Tool Handlers** - Bridge from MCP tools to reader/writer
3. **AnimGraph Serializer** - State machine reading for Animation Blueprints
4. **Widget Tree Serializer** - Widget hierarchy reading for Widget Blueprints
5. **42 MCP Tools** - 7 readers + 35 writers across all categories

### Key Patterns to Follow

1. **All writer handlers use the write pipeline** - Never call `FOliveBlueprintWriter` directly from handlers
2. **MCP requests skip confirmation** - Check `Request.bFromMCP` in pipeline
3. **Structured errors with suggestions** - Every error includes actionable suggestion
4. **Transaction safety** - Pipeline handles transaction; handlers just provide executor delegate
5. **Reuse existing infrastructure** - `FOliveBlueprintReader`, `FOliveBlueprintWriter`, `FOliveGraphWriter`, `FOliveCompileManager` are already implemented

### What NOT to Build

- Confirmation UI (built-in chat handles this separately)
- New validation rules (use existing `FOliveValidationEngine`)
- New IR types (use existing `BlueprintIR.h` and `CommonIR.h`)

---

## 11. Change Log

| Date | Author | Change |
|------|--------|--------|
| 2026-02-19 | Architect | Initial design document |
