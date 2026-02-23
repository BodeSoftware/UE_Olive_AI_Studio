# Phase B Coder Task: FOlivePlanExecutor

## Overview

Create `OlivePlanExecutor.h` and `OlivePlanExecutor.cpp` -- the multi-phase plan execution engine that replaces the lowerer for `schema_version: "2.0"` plans. The executor creates all nodes first, introspects their real pins via `FOlivePinManifest::Build()`, then wires exec connections, data connections, and defaults using ground-truth pin names from the manifests.

**Files to create:**
- `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`
- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

**NOT a singleton.** Instantiate on the stack per execution. Holds no persistent state.

---

## Critical Design Decisions (Do Not Deviate)

### 1. Direct Pin Connection, Not GraphWriter.ConnectPins

The executor does NOT use `FOliveGraphWriter::Get().ConnectPins()` for wiring. Instead, it:
- Finds `UEdGraphPin*` directly on the `UEdGraphNode*` using `Node->FindPin(FName(*PinName))`
- Calls `FOlivePinConnector::Get().Connect(SourcePin, TargetPin)` directly

**Rationale:** `GraphWriter.ConnectPins()` takes `"node_id.pin_name"` string refs and resolves the node ID via its internal cache. For reused event nodes (not created by AddNode), the node is not in the cache, so ConnectPins would fail. By using the `UEdGraphNode*` directly (stored in `Context.StepToNodePtr`), we bypass the cache entirely. The manifest gives us exact pin names, so direct `FindPin()` always works.

### 2. Direct SetPinDefault, Not GraphWriter.SetPinDefault

Same reasoning. For pin defaults, the executor:
- Gets `UEdGraphNode*` from context
- Calls `Node->FindPin(FName(*PinName))` to get the `UEdGraphPin*`
- Calls `Graph->GetSchema()->TrySetDefaultValue(*Pin, DefaultValue)` directly

### 3. GraphWriter.AddNode() IS Used for Node Creation

Node creation still goes through `FOliveGraphWriter::Get().AddNode()` because:
- It handles node ID generation and caching (needed for any future external references)
- It calls `FOliveNodeFactory::Get().CreateNode()` internally
- It marks the Blueprint as modified

### 4. Event Reuse

If a plan includes an `event` or `custom_event` step for an event that already exists, the executor reuses the existing node instead of failing. The existing `UEdGraphNode*` is found via `FBlueprintEditorUtils::FindOverrideForFunction()` (for native events) or by iterating graph nodes (for custom events).

### 5. Continue-on-Failure Semantics

- Phase 1 (Create Nodes): **FAIL-FAST** -- abort entire plan if any node fails
- Phase 3 (Wire Exec): **CONTINUE-ON-FAILURE** -- accumulate errors
- Phase 4 (Wire Data): **CONTINUE-ON-FAILURE** -- accumulate errors
- Phase 5 (Set Defaults): **CONTINUE-ON-FAILURE** -- accumulate errors
- Phase 6 (Auto-Layout): **ALWAYS SUCCEEDS** -- best-effort

---

## Dependencies (Already Implemented)

| File | What We Use |
|------|-------------|
| `Plan/OlivePinManifest.h` | `FOlivePinManifest`, `FOlivePinManifestEntry`, `::Build()`, `FindPinSmart()`, `FindExecInput()`, `FindExecOutput()`, `FindAllExecOutputs()`, `GetDataPins()`, `ToJson()` |
| `Plan/OliveFunctionResolver.h` | Not used directly by executor (used by resolver before we get the data) |
| `Plan/OliveBlueprintPlanResolver.h` | `FOliveResolvedStep` struct (input to Execute) |
| `IR/BlueprintPlanIR.h` | `FOliveIRBlueprintPlan`, `FOliveIRBlueprintPlanStep`, `FOliveIRBlueprintPlanResult`, `FOliveIRBlueprintPlanError`, `OliveNodeTypes`, `OlivePlanOps` |
| `Writer/OliveGraphWriter.h` | `FOliveGraphWriter::Get()`, `.AddNode()`, `.GetCachedNode()` |
| `Writer/OliveNodeFactory.h` | `OliveNodeTypes` constants |
| `Writer/OlivePinConnector.h` | `FOlivePinConnector::Get()`, `.Connect()` |
| `Writer/OliveBlueprintWriter.h` | `FOliveBlueprintWriteResult` |
| `IR/OliveIRTypes.h` | `EOliveIRTypeCategory` |

---

## Header: OlivePlanExecutor.h

Write this file exactly as specified. All structs and the class declaration go in this header.

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Plan/OlivePinManifest.h"
#include "IR/BlueprintPlanIR.h"

// Forward declarations
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
struct FOliveResolvedStep;

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePlanExecutor, Log, All);

/**
 * Execution context for multi-phase plan application.
 * Holds manifests built during node creation and provides lookup methods
 * for wiring phases. Constructed per-execution, not persistent.
 */
struct OLIVEAIEDITOR_API FOlivePlanExecutionContext
{
    /** The target Blueprint */
    UBlueprint* Blueprint = nullptr;

    /** The target graph */
    UEdGraph* Graph = nullptr;

    /** Asset path for GraphWriter operations */
    FString AssetPath;

    /** Graph name for GraphWriter operations */
    FString GraphName;

    /** Step ID -> Pin Manifest mapping (populated during Phase 1) */
    TMap<FString, FOlivePinManifest> StepManifests;

    /** Step ID -> Node ID mapping (populated during Phase 1) */
    TMap<FString, FString> StepToNodeMap;

    /** Step ID -> UEdGraphNode* mapping (populated during Phase 1) */
    TMap<FString, UEdGraphNode*> StepToNodePtr;

    /** Accumulated warnings (non-fatal) */
    TArray<FString> Warnings;

    /** Accumulated wiring errors (non-fatal, reported at end) */
    TArray<FOliveIRBlueprintPlanError> WiringErrors;

    /** Count of successfully created nodes */
    int32 CreatedNodeCount = 0;

    /** Count of successfully made connections */
    int32 SuccessfulConnectionCount = 0;

    /** Count of failed connections */
    int32 FailedConnectionCount = 0;

    /** Count of successfully set pin defaults */
    int32 SuccessfulDefaultCount = 0;

    /** Count of failed pin default sets */
    int32 FailedDefaultCount = 0;

    /** Get manifest for a step, or nullptr if step was not created */
    const FOlivePinManifest* GetManifest(const FString& StepId) const;

    /** Get the node pointer for a step, or nullptr */
    UEdGraphNode* GetNodePtr(const FString& StepId) const;

    /** Get the node ID for a step, or empty string */
    FString GetNodeId(const FString& StepId) const;
};

/**
 * Result of attempting to wire two pins using smart resolution.
 */
struct OLIVEAIEDITOR_API FOliveSmartWireResult
{
    bool bSuccess = false;

    /** How the source pin was matched (for diagnostics) */
    FString SourceMatchMethod;

    /** How the target pin was matched */
    FString TargetMatchMethod;

    /** Actual source pin name used */
    FString ResolvedSourcePin;

    /** Actual target pin name used */
    FString ResolvedTargetPin;

    /** Error message if failed */
    FString ErrorMessage;

    /** Suggestions if failed (actual available pin names) */
    TArray<FString> Suggestions;
};

/**
 * FOlivePlanExecutor
 *
 * Multi-phase plan execution engine with post-creation pin introspection.
 * Replaces the lowerer (FOliveBlueprintPlanLowerer) for schema_version "2.0" plans.
 *
 * Execution phases:
 *   Phase 1: Create all nodes + build pin manifests (FAIL-FAST)
 *   Phase 3: Wire exec connections using manifest (CONTINUE-ON-FAILURE)
 *   Phase 4: Wire data connections using manifest (CONTINUE-ON-FAILURE)
 *   Phase 5: Set pin defaults using manifest (CONTINUE-ON-FAILURE)
 *   Phase 6: Auto-layout using exec flow topology (ALWAYS SUCCEEDS)
 *
 * (Phase 2 is merged into Phase 1.)
 *
 * NOT a singleton. Instantiate fresh per execution.
 *
 * Thread Safety: Must be called on the game thread (UE graph APIs).
 */
class OLIVEAIEDITOR_API FOlivePlanExecutor
{
public:
    FOlivePlanExecutor() = default;
    ~FOlivePlanExecutor() = default;

    /**
     * Execute a resolved plan: create nodes, introspect pins, wire, set defaults, layout.
     *
     * Call this inside a write pipeline executor lambda. The pipeline owns the
     * FScopedTransaction; this executor does NOT create its own transaction.
     *
     * @param Plan           The parsed plan (for exec_after, inputs, exec_outputs topology)
     * @param ResolvedSteps  Output from FOliveBlueprintPlanResolver::Resolve()
     * @param Blueprint      The target Blueprint (already loaded and Modify'd by caller)
     * @param Graph          The target graph (already found/created by caller)
     * @param AssetPath      Asset path for GraphWriter cache operations
     * @param GraphName      Graph name for GraphWriter operations
     * @return Plan result with step_to_node_map, wiring errors, and pin manifests
     */
    FOliveIRBlueprintPlanResult Execute(
        const FOliveIRBlueprintPlan& Plan,
        const TArray<FOliveResolvedStep>& ResolvedSteps,
        UBlueprint* Blueprint,
        UEdGraph* Graph,
        const FString& AssetPath,
        const FString& GraphName);

private:
    // ====================================================================
    // Phase Methods
    // ====================================================================

    /** Phase 1: Create all nodes + build pin manifests. FAIL-FAST. */
    bool PhaseCreateNodes(
        const FOliveIRBlueprintPlan& Plan,
        const TArray<FOliveResolvedStep>& ResolvedSteps,
        FOlivePlanExecutionContext& Context);

    /** Phase 3: Wire exec connections. CONTINUE-ON-FAILURE. */
    void PhaseWireExec(
        const FOliveIRBlueprintPlan& Plan,
        FOlivePlanExecutionContext& Context);

    /** Phase 4: Wire data connections. CONTINUE-ON-FAILURE. */
    void PhaseWireData(
        const FOliveIRBlueprintPlan& Plan,
        FOlivePlanExecutionContext& Context);

    /** Phase 5: Set pin default values. CONTINUE-ON-FAILURE. */
    void PhaseSetDefaults(
        const FOliveIRBlueprintPlan& Plan,
        FOlivePlanExecutionContext& Context);

    /** Phase 6: Auto-layout based on exec flow topology. ALWAYS SUCCEEDS. */
    void PhaseAutoLayout(
        const FOliveIRBlueprintPlan& Plan,
        FOlivePlanExecutionContext& Context);

    // ====================================================================
    // Helper Methods
    // ====================================================================

    /** Wire a single exec connection between two steps */
    FOliveSmartWireResult WireExecConnection(
        const FString& SourceStepId,
        const FString& SourcePinHint,
        const FString& TargetStepId,
        FOlivePlanExecutionContext& Context);

    /** Wire a single data connection from a @ref source to a target step's input */
    FOliveSmartWireResult WireDataConnection(
        const FString& TargetStepId,
        const FString& TargetPinHint,
        const FString& SourceRef,
        FOlivePlanExecutionContext& Context);

    /** Find type-compatible output pin on source for auto-matching */
    const FOlivePinManifestEntry* FindTypeCompatibleOutput(
        const FOlivePinManifest& SourceManifest,
        EOliveIRTypeCategory TargetTypeCategory,
        const FString& TargetSubCategory);

    /** Parse "@stepId.pinHint" into components */
    bool ParseDataRef(
        const FString& Ref,
        FString& OutStepId,
        FString& OutPinHint);

    /** Find existing event/custom_event node in graph for reuse */
    UEdGraphNode* FindExistingEventNode(
        UEdGraph* Graph,
        UBlueprint* Blueprint,
        const FString& EventName,
        bool bIsCustomEvent);

    /** Build human-readable list of available pins for error messages */
    FString BuildPinSuggestionList(
        const FOlivePinManifest& Manifest,
        bool bInput,
        bool bExecOnly);

    /** Assemble FOliveIRBlueprintPlanResult from context */
    FOliveIRBlueprintPlanResult AssembleResult(
        const FOliveIRBlueprintPlan& Plan,
        const FOlivePlanExecutionContext& Context);

    // ====================================================================
    // Layout Constants
    // ====================================================================

    static constexpr int32 HORIZONTAL_SPACING = 350;
    static constexpr int32 VERTICAL_SPACING = 200;
    static constexpr int32 BRANCH_OFFSET = 250;
    static constexpr int32 PURE_NODE_OFFSET_Y = -120;
};
```

---

## Implementation: OlivePlanExecutor.cpp

### Includes

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "Plan/OlivePlanExecutor.h"
#include "Plan/OlivePinManifest.h"
#include "Plan/OliveBlueprintPlanResolver.h"
#include "Writer/OliveGraphWriter.h"
#include "Writer/OliveNodeFactory.h"
#include "Writer/OlivePinConnector.h"
#include "Writer/OliveBlueprintWriter.h"

// UE includes
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

DEFINE_LOG_CATEGORY(LogOlivePlanExecutor);
```

### Context Lookup Methods

```cpp
const FOlivePinManifest* FOlivePlanExecutionContext::GetManifest(const FString& StepId) const
{
    return StepManifests.Find(StepId);
}

UEdGraphNode* FOlivePlanExecutionContext::GetNodePtr(const FString& StepId) const
{
    UEdGraphNode* const* Found = StepToNodePtr.Find(StepId);
    return Found ? *Found : nullptr;
}

FString FOlivePlanExecutionContext::GetNodeId(const FString& StepId) const
{
    const FString* Found = StepToNodeMap.Find(StepId);
    return Found ? *Found : FString();
}
```

### Execute() Entry Point

Implement exactly as shown in the header specification section above. The orchestrator initializes context, runs phases 1/3/4/5/6 sequentially, and returns `AssembleResult()`. See the pseudocode above -- copy the logic structure verbatim.

Key details:
- Log at `Log` level for phase transitions, `Verbose` for per-step detail, `Error` for failures
- If Phase 1 returns false, immediately call `AssembleResult()` and return (no further phases)
- Track timing: wrap the entire Execute in a `double StartTime = FPlatformTime::Seconds()` and report in the result

### Phase 1: Create Nodes

```cpp
bool FOlivePlanExecutor::PhaseCreateNodes(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    FOlivePlanExecutionContext& Context)
{
    FOliveGraphWriter& Writer = FOliveGraphWriter::Get();

    // Build StepId -> original plan step lookup
    TMap<FString, const FOliveIRBlueprintPlanStep*> PlanStepLookup;
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        PlanStepLookup.Add(Step.StepId, &Step);
    }

    for (int32 i = 0; i < ResolvedSteps.Num(); ++i)
    {
        const FOliveResolvedStep& Resolved = ResolvedSteps[i];
        const FString& StepId = Resolved.StepId;
        const FString& NodeType = Resolved.NodeType;

        // ----------------------------------------------------------------
        // Event reuse check
        // ----------------------------------------------------------------
        bool bIsEventOp = (NodeType == OliveNodeTypes::Event);
        bool bIsCustomEventOp = (NodeType == OliveNodeTypes::CustomEvent);

        if (bIsEventOp || bIsCustomEventOp)
        {
            FString EventName;
            const FString* EventNamePtr = Resolved.Properties.Find(TEXT("event_name"));
            if (EventNamePtr)
            {
                EventName = *EventNamePtr;
            }

            if (!EventName.IsEmpty())
            {
                UEdGraphNode* ExistingNode = FindExistingEventNode(
                    Context.Graph, Context.Blueprint, EventName, bIsCustomEventOp);

                if (ExistingNode)
                {
                    // Reuse existing event node.
                    // Generate a node ID using the node's GUID so GraphWriter
                    // cache lookups are not needed for wiring (we use direct
                    // UEdGraphNode* from context instead).
                    FString ReuseNodeId = ExistingNode->NodeGuid.ToString();

                    FOlivePinManifest Manifest = FOlivePinManifest::Build(
                        ExistingNode, StepId, ReuseNodeId, NodeType);

                    Context.StepManifests.Add(StepId, MoveTemp(Manifest));
                    Context.StepToNodeMap.Add(StepId, ReuseNodeId);
                    Context.StepToNodePtr.Add(StepId, ExistingNode);
                    Context.CreatedNodeCount++;

                    Context.Warnings.Add(FString::Printf(
                        TEXT("Step '%s': %s '%s' already exists in graph, reusing existing node"),
                        *StepId,
                        bIsCustomEventOp ? TEXT("Custom event") : TEXT("Event"),
                        *EventName));

                    UE_LOG(LogOlivePlanExecutor, Log,
                        TEXT("Reused existing %s node '%s' for step '%s'"),
                        bIsCustomEventOp ? TEXT("custom event") : TEXT("event"),
                        *EventName, *StepId);

                    continue; // Skip to next step
                }
            }
        }

        // ----------------------------------------------------------------
        // Normal node creation via GraphWriter.AddNode()
        // ----------------------------------------------------------------
        // Temporary position; Phase 6 overrides with layout
        int32 PosX = i * 300;
        int32 PosY = 0;

        FOliveBlueprintWriteResult WriteResult = Writer.AddNode(
            Context.AssetPath,
            Context.GraphName,
            NodeType,
            Resolved.Properties,
            PosX,
            PosY);

        if (!WriteResult.bSuccess)
        {
            // FAIL-FAST
            FString ErrorMsg = WriteResult.Errors.Num() > 0
                ? WriteResult.Errors[0]
                : TEXT("Unknown node creation error");

            UE_LOG(LogOlivePlanExecutor, Error,
                TEXT("Phase 1 FAIL: Step '%s' (type: %s) failed: %s"),
                *StepId, *NodeType, *ErrorMsg);

            Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                TEXT("NODE_CREATION_FAILED"),
                StepId,
                FString::Printf(TEXT("/steps/%d"), i),
                FString::Printf(TEXT("Failed to create node for step '%s' (type: %s): %s"),
                    *StepId, *NodeType, *ErrorMsg),
                TEXT("Check the step's op, target, and properties")));

            return false;
        }

        // Retrieve UEdGraphNode* from GraphWriter cache
        FString NodeId = WriteResult.CreatedNodeId;
        UEdGraphNode* NodePtr = Writer.GetCachedNode(Context.AssetPath, NodeId);

        if (!NodePtr)
        {
            // Should never happen after successful AddNode, but guard
            UE_LOG(LogOlivePlanExecutor, Error,
                TEXT("Phase 1 FAIL: Step '%s' created (id: %s) but not in cache"),
                *StepId, *NodeId);

            Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                TEXT("NODE_CREATION_FAILED"),
                StepId,
                FString::Printf(TEXT("/steps/%d"), i),
                TEXT("Node created but not retrievable from cache"),
                TEXT("Internal error -- retry the plan")));

            return false;
        }

        // Build pin manifest from the real node (this is the key insight)
        FOlivePinManifest Manifest = FOlivePinManifest::Build(
            NodePtr, StepId, NodeId, NodeType);

        // Store in context
        Context.StepManifests.Add(StepId, MoveTemp(Manifest));
        Context.StepToNodeMap.Add(StepId, NodeId);
        Context.StepToNodePtr.Add(StepId, NodePtr);
        Context.CreatedNodeCount++;

        UE_LOG(LogOlivePlanExecutor, Verbose,
            TEXT("Created step '%s' -> node '%s': %d pins (%s)"),
            *StepId, *NodeId,
            Context.StepManifests[StepId].Pins.Num(),
            Context.StepManifests[StepId].bIsPure ? TEXT("pure") : TEXT("exec"));
    }

    return true;
}
```

### FindExistingEventNode

```cpp
UEdGraphNode* FOlivePlanExecutor::FindExistingEventNode(
    UEdGraph* Graph,
    UBlueprint* Blueprint,
    const FString& EventName,
    bool bIsCustomEvent)
{
    if (!Graph || !Blueprint)
    {
        return nullptr;
    }

    if (bIsCustomEvent)
    {
        // Search for UK2Node_CustomEvent with matching name
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node);
            if (CustomEvent && CustomEvent->CustomFunctionName.ToString() == EventName)
            {
                return CustomEvent;
            }
        }
    }
    else
    {
        // Search for UK2Node_Event override matching the event name
        // FBlueprintEditorUtils::FindOverrideForFunction searches across
        // all graphs, but we specifically want the one in our target graph.
        // Use FBlueprintEditorUtils which is the same check as OliveNodeFactory.
        if (Blueprint->ParentClass)
        {
            FName EventFName(*EventName);
            UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(
                Blueprint, Blueprint->ParentClass, EventFName);

            if (ExistingEvent && ExistingEvent->GetGraph() == Graph)
            {
                return ExistingEvent;
            }
        }
    }

    return nullptr;
}
```

### Phase 3: Wire Exec Connections

```cpp
void FOlivePlanExecutor::PhaseWireExec(
    const FOliveIRBlueprintPlan& Plan,
    FOlivePlanExecutionContext& Context)
{
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        // ----------------------------------------------------------------
        // exec_after: wire from ExecAfter step's primary exec output
        //             to this step's exec input
        // ----------------------------------------------------------------
        if (!Step.ExecAfter.IsEmpty())
        {
            FOliveSmartWireResult Result = WireExecConnection(
                Step.ExecAfter,     // source step
                FString(),          // empty hint = primary exec output
                Step.StepId,        // target step
                Context);

            if (Result.bSuccess)
            {
                Context.SuccessfulConnectionCount++;
            }
            else
            {
                Context.FailedConnectionCount++;
                Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("EXEC_PIN_NOT_FOUND"),
                    Step.StepId,
                    FString::Printf(TEXT("/steps/exec_after")),
                    Result.ErrorMessage,
                    Result.Suggestions.Num() > 0
                        ? FString::Printf(TEXT("Available exec pins: %s"),
                            *FString::Join(Result.Suggestions, TEXT(", ")))
                        : TEXT("")));
            }
        }

        // ----------------------------------------------------------------
        // exec_outputs: wire from this step's named exec output
        //               to each target step's exec input
        // ----------------------------------------------------------------
        for (const auto& ExecOut : Step.ExecOutputs)
        {
            const FString& PinHint = ExecOut.Key;    // e.g., "True", "False", "Then 0"
            const FString& TargetStepId = ExecOut.Value;

            FOliveSmartWireResult Result = WireExecConnection(
                Step.StepId,        // source step
                PinHint,            // named exec output
                TargetStepId,       // target step
                Context);

            if (Result.bSuccess)
            {
                Context.SuccessfulConnectionCount++;
            }
            else
            {
                Context.FailedConnectionCount++;
                Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("EXEC_PIN_NOT_FOUND"),
                    Step.StepId,
                    FString::Printf(TEXT("/steps/exec_outputs/%s"), *PinHint),
                    Result.ErrorMessage,
                    Result.Suggestions.Num() > 0
                        ? FString::Printf(TEXT("Available exec outputs: %s"),
                            *FString::Join(Result.Suggestions, TEXT(", ")))
                        : TEXT("")));
            }
        }
    }
}
```

### WireExecConnection Helper

This is the core exec wiring method. Uses manifests to find real pin names, then directly connects `UEdGraphPin*` objects.

```cpp
FOliveSmartWireResult FOlivePlanExecutor::WireExecConnection(
    const FString& SourceStepId,
    const FString& SourcePinHint,
    const FString& TargetStepId,
    FOlivePlanExecutionContext& Context)
{
    FOliveSmartWireResult Result;

    // Get manifests
    const FOlivePinManifest* SourceManifest = Context.GetManifest(SourceStepId);
    const FOlivePinManifest* TargetManifest = Context.GetManifest(TargetStepId);

    if (!SourceManifest)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Source step '%s' not found in manifests"), *SourceStepId);
        return Result;
    }
    if (!TargetManifest)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Target step '%s' not found in manifests"), *TargetStepId);
        return Result;
    }

    // Resolve target exec input pin
    const FOlivePinManifestEntry* TargetExecIn = TargetManifest->FindExecInput();
    if (!TargetExecIn)
    {
        if (TargetManifest->bIsPure)
        {
            // Pure nodes don't need exec wiring -- this is a warning, not error
            Result.bSuccess = true;
            Result.TargetMatchMethod = TEXT("pure_skip");

            UE_LOG(LogOlivePlanExecutor, Verbose,
                TEXT("Skipping exec wire to pure node step '%s'"), *TargetStepId);

            // Add a warning to context
            Context.Warnings.Add(FString::Printf(
                TEXT("Step '%s' is a pure node; exec wire from '%s' skipped (PURE_NODE_EXEC_SKIP)"),
                *TargetStepId, *SourceStepId));
            return Result;
        }

        Result.ErrorMessage = FString::Printf(
            TEXT("No exec input pin found on target step '%s' (type: %s)"),
            *TargetStepId, *TargetManifest->NodeType);
        return Result;
    }

    // Resolve source exec output pin
    const FOlivePinManifestEntry* SourceExecOut = nullptr;

    if (SourcePinHint.IsEmpty())
    {
        // No hint -- use the primary (first) exec output
        SourceExecOut = SourceManifest->FindExecOutput();
    }
    else
    {
        // Specific pin requested -- use FindPinSmart on exec outputs
        // First, try FindPinSmart which searches non-exec pins only.
        // We need to search exec output pins specifically.
        TArray<const FOlivePinManifestEntry*> AllExecOuts = SourceManifest->FindAllExecOutputs();

        // Try exact name match first
        for (const FOlivePinManifestEntry* ExecOut : AllExecOuts)
        {
            if (ExecOut->PinName == SourcePinHint)
            {
                SourceExecOut = ExecOut;
                break;
            }
        }

        // Try display name match
        if (!SourceExecOut)
        {
            for (const FOlivePinManifestEntry* ExecOut : AllExecOuts)
            {
                if (ExecOut->DisplayName == SourcePinHint)
                {
                    SourceExecOut = ExecOut;
                    break;
                }
            }
        }

        // Try case-insensitive match
        if (!SourceExecOut)
        {
            for (const FOlivePinManifestEntry* ExecOut : AllExecOuts)
            {
                if (ExecOut->PinName.Equals(SourcePinHint, ESearchCase::IgnoreCase) ||
                    ExecOut->DisplayName.Equals(SourcePinHint, ESearchCase::IgnoreCase))
                {
                    SourceExecOut = ExecOut;
                    break;
                }
            }
        }

        // Try substring match (e.g., "True" matches "Then True" or vice versa)
        if (!SourceExecOut)
        {
            for (const FOlivePinManifestEntry* ExecOut : AllExecOuts)
            {
                if (ExecOut->PinName.Contains(SourcePinHint, ESearchCase::IgnoreCase) ||
                    SourcePinHint.Contains(ExecOut->PinName, ESearchCase::IgnoreCase) ||
                    ExecOut->DisplayName.Contains(SourcePinHint, ESearchCase::IgnoreCase) ||
                    SourcePinHint.Contains(ExecOut->DisplayName, ESearchCase::IgnoreCase))
                {
                    SourceExecOut = ExecOut;
                    break;
                }
            }
        }
    }

    if (!SourceExecOut)
    {
        // Build suggestions
        TArray<const FOlivePinManifestEntry*> AllExecOuts = SourceManifest->FindAllExecOutputs();
        for (const FOlivePinManifestEntry* Pin : AllExecOuts)
        {
            Result.Suggestions.Add(Pin->PinName);
        }

        Result.ErrorMessage = FString::Printf(
            TEXT("No exec output pin matching '%s' on source step '%s' (type: %s). Available: %s"),
            *SourcePinHint, *SourceStepId, *SourceManifest->NodeType,
            *FString::Join(Result.Suggestions, TEXT(", ")));
        return Result;
    }

    // ----------------------------------------------------------------
    // Make the connection using direct UEdGraphPin* access
    // ----------------------------------------------------------------
    UEdGraphNode* SourceNode = Context.GetNodePtr(SourceStepId);
    UEdGraphNode* TargetNode = Context.GetNodePtr(TargetStepId);

    if (!SourceNode || !TargetNode)
    {
        Result.ErrorMessage = TEXT("Node pointer missing from context");
        return Result;
    }

    UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*SourceExecOut->PinName));
    UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetExecIn->PinName));

    if (!SourcePin)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Source exec pin '%s' not found on UEdGraphNode (step: %s)"),
            *SourceExecOut->PinName, *SourceStepId);
        return Result;
    }
    if (!TargetPin)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Target exec pin '%s' not found on UEdGraphNode (step: %s)"),
            *TargetExecIn->PinName, *TargetStepId);
        return Result;
    }

    FOlivePinConnector& Connector = FOlivePinConnector::Get();
    FOliveBlueprintWriteResult ConnectResult = Connector.Connect(SourcePin, TargetPin, false);

    if (ConnectResult.bSuccess)
    {
        Result.bSuccess = true;
        Result.ResolvedSourcePin = SourceExecOut->PinName;
        Result.ResolvedTargetPin = TargetExecIn->PinName;
        Result.SourceMatchMethod = SourcePinHint.IsEmpty() ? TEXT("exec_primary") : TEXT("exec_named");
        Result.TargetMatchMethod = TEXT("exec_primary");

        UE_LOG(LogOlivePlanExecutor, Verbose,
            TEXT("Exec wire: %s.%s -> %s.%s"),
            *SourceStepId, *SourceExecOut->PinName,
            *TargetStepId, *TargetExecIn->PinName);
    }
    else
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Pin connection failed (%s.%s -> %s.%s): %s"),
            *SourceStepId, *SourceExecOut->PinName,
            *TargetStepId, *TargetExecIn->PinName,
            ConnectResult.Errors.Num() > 0 ? *ConnectResult.Errors[0] : TEXT("Unknown"));
    }

    return Result;
}
```

### Phase 4: Wire Data Connections

```cpp
void FOlivePlanExecutor::PhaseWireData(
    const FOliveIRBlueprintPlan& Plan,
    FOlivePlanExecutionContext& Context)
{
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        for (const auto& InputPair : Step.Inputs)
        {
            const FString& PinKey = InputPair.Key;      // AI's name for target input pin
            const FString& PinValue = InputPair.Value;   // "@stepId.pinHint" or literal

            // Only process @ref values in this phase; literals are Phase 5
            if (!PinValue.StartsWith(TEXT("@")))
            {
                continue;
            }

            FOliveSmartWireResult Result = WireDataConnection(
                Step.StepId, PinKey, PinValue, Context);

            if (Result.bSuccess)
            {
                Context.SuccessfulConnectionCount++;
            }
            else
            {
                Context.FailedConnectionCount++;
                Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("DATA_PIN_NOT_FOUND"),
                    Step.StepId,
                    FString::Printf(TEXT("/steps/inputs/%s"), *PinKey),
                    Result.ErrorMessage,
                    Result.Suggestions.Num() > 0
                        ? FString::Printf(TEXT("Available pins: %s"),
                            *FString::Join(Result.Suggestions, TEXT(", ")))
                        : TEXT("")));
            }
        }
    }
}
```

### WireDataConnection Helper

```cpp
FOliveSmartWireResult FOlivePlanExecutor::WireDataConnection(
    const FString& TargetStepId,
    const FString& TargetPinHint,
    const FString& SourceRef,
    FOlivePlanExecutionContext& Context)
{
    FOliveSmartWireResult Result;

    // 1. Parse @ref
    FString SourceStepId, SourcePinHint;
    if (!ParseDataRef(SourceRef, SourceStepId, SourcePinHint))
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Invalid @ref format: '%s'. Expected '@stepId.pinHint'"), *SourceRef);
        return Result;
    }

    // 2. Get manifests
    const FOlivePinManifest* SourceManifest = Context.GetManifest(SourceStepId);
    const FOlivePinManifest* TargetManifest = Context.GetManifest(TargetStepId);

    if (!SourceManifest)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Source step '%s' not found (referenced by %s)"), *SourceStepId, *SourceRef);
        return Result;
    }
    if (!TargetManifest)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Target step '%s' not found"), *TargetStepId);
        return Result;
    }

    // 3. Resolve TARGET input pin
    FString TargetMatchMethod;
    const FOlivePinManifestEntry* TargetPin = TargetManifest->FindPinSmart(
        TargetPinHint, /*bIsInput=*/true,
        EOliveIRTypeCategory::Unknown, &TargetMatchMethod);

    if (!TargetPin)
    {
        // Build suggestions from available data input pins
        TArray<const FOlivePinManifestEntry*> DataInputs = TargetManifest->GetDataPins(/*bInput=*/true);
        for (const FOlivePinManifestEntry* Pin : DataInputs)
        {
            Result.Suggestions.Add(FString::Printf(TEXT("%s (%s)"),
                *Pin->PinName, *Pin->TypeDisplayString));
        }

        Result.ErrorMessage = FString::Printf(
            TEXT("No input pin matching '%s' on step '%s' (type: %s)"),
            *TargetPinHint, *TargetStepId, *TargetManifest->NodeType);
        return Result;
    }

    // 4. Resolve SOURCE output pin
    const FOlivePinManifestEntry* SourcePin = nullptr;
    FString SourceMatchMethod;

    if (SourcePinHint == TEXT("auto"))
    {
        // TYPE-BASED AUTO-MATCH
        SourcePin = FindTypeCompatibleOutput(
            *SourceManifest, TargetPin->IRTypeCategory, TargetPin->PinSubCategory);
        SourceMatchMethod = TEXT("type_auto");

        if (!SourcePin)
        {
            // Build suggestions
            TArray<const FOlivePinManifestEntry*> DataOutputs = SourceManifest->GetDataPins(/*bInput=*/false);
            for (const FOlivePinManifestEntry* Pin : DataOutputs)
            {
                Result.Suggestions.Add(FString::Printf(TEXT("%s (%s)"),
                    *Pin->PinName, *Pin->TypeDisplayString));
            }

            Result.ErrorMessage = FString::Printf(
                TEXT("@%s.auto: No output pin on step '%s' matches target type '%s'"),
                *SourceStepId, *SourceStepId, *TargetPin->TypeDisplayString);
            return Result;
        }
    }
    else if (SourcePinHint.StartsWith(TEXT("~")))
    {
        // FUZZY MATCH with ~ prefix
        FString FuzzyHint = SourcePinHint.Mid(1);
        SourcePin = SourceManifest->FindPinSmart(
            FuzzyHint, /*bIsInput=*/false,
            TargetPin->IRTypeCategory, &SourceMatchMethod);
        SourceMatchMethod = TEXT("fuzzy_") + SourceMatchMethod;
    }
    else
    {
        // STANDARD: smart resolution
        SourcePin = SourceManifest->FindPinSmart(
            SourcePinHint, /*bIsInput=*/false,
            TargetPin->IRTypeCategory, &SourceMatchMethod);
    }

    if (!SourcePin)
    {
        TArray<const FOlivePinManifestEntry*> DataOutputs = SourceManifest->GetDataPins(/*bInput=*/false);
        for (const FOlivePinManifestEntry* Pin : DataOutputs)
        {
            Result.Suggestions.Add(FString::Printf(TEXT("%s (%s)"),
                *Pin->PinName, *Pin->TypeDisplayString));
        }

        Result.ErrorMessage = FString::Printf(
            TEXT("No output pin matching '%s' on source step '%s' (type: %s)"),
            *SourcePinHint, *SourceStepId, *SourceManifest->NodeType);
        return Result;
    }

    // 5. Make the connection
    UEdGraphNode* SourceNode = Context.GetNodePtr(SourceStepId);
    UEdGraphNode* TargetNode = Context.GetNodePtr(TargetStepId);

    if (!SourceNode || !TargetNode)
    {
        Result.ErrorMessage = TEXT("Node pointer missing from context");
        return Result;
    }

    UEdGraphPin* RealSourcePin = SourceNode->FindPin(FName(*SourcePin->PinName));
    UEdGraphPin* RealTargetPin = TargetNode->FindPin(FName(*TargetPin->PinName));

    if (!RealSourcePin)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Source pin '%s' not found on UEdGraphNode (step: %s)"),
            *SourcePin->PinName, *SourceStepId);
        return Result;
    }
    if (!RealTargetPin)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Target pin '%s' not found on UEdGraphNode (step: %s)"),
            *TargetPin->PinName, *TargetStepId);
        return Result;
    }

    FOlivePinConnector& Connector = FOlivePinConnector::Get();
    FOliveBlueprintWriteResult ConnectResult = Connector.Connect(RealSourcePin, RealTargetPin, false);

    if (ConnectResult.bSuccess)
    {
        Result.bSuccess = true;
        Result.ResolvedSourcePin = SourcePin->PinName;
        Result.ResolvedTargetPin = TargetPin->PinName;
        Result.SourceMatchMethod = SourceMatchMethod;
        Result.TargetMatchMethod = TargetMatchMethod;

        UE_LOG(LogOlivePlanExecutor, Verbose,
            TEXT("Data wire: %s.%s -> %s.%s (source: %s, target: %s)"),
            *SourceStepId, *SourcePin->PinName,
            *TargetStepId, *TargetPin->PinName,
            *SourceMatchMethod, *TargetMatchMethod);
    }
    else
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Pin connection failed (%s.%s -> %s.%s): %s"),
            *SourceStepId, *SourcePin->PinName,
            *TargetStepId, *TargetPin->PinName,
            ConnectResult.Errors.Num() > 0 ? *ConnectResult.Errors[0] : TEXT("Unknown"));
    }

    return Result;
}
```

### FindTypeCompatibleOutput

```cpp
const FOlivePinManifestEntry* FOlivePlanExecutor::FindTypeCompatibleOutput(
    const FOlivePinManifest& SourceManifest,
    EOliveIRTypeCategory TargetTypeCategory,
    const FString& TargetSubCategory)
{
    TArray<const FOlivePinManifestEntry*> DataOutputs = SourceManifest.GetDataPins(/*bInput=*/false);

    // Filter by type category
    TArray<const FOlivePinManifestEntry*> TypeMatches;
    for (const FOlivePinManifestEntry* Pin : DataOutputs)
    {
        if (Pin->IRTypeCategory == TargetTypeCategory)
        {
            TypeMatches.Add(Pin);
        }
    }

    if (TypeMatches.Num() == 1)
    {
        return TypeMatches[0];
    }

    if (TypeMatches.Num() > 1 && !TargetSubCategory.IsEmpty())
    {
        // Sub-filter by subcategory (struct/class name)
        TArray<const FOlivePinManifestEntry*> SubMatches;
        for (const FOlivePinManifestEntry* Pin : TypeMatches)
        {
            if (Pin->PinSubCategory == TargetSubCategory)
            {
                SubMatches.Add(Pin);
            }
        }
        if (SubMatches.Num() == 1)
        {
            return SubMatches[0];
        }
    }

    if (TypeMatches.Num() > 1)
    {
        // Prefer "ReturnValue" pin if present (most common output)
        for (const FOlivePinManifestEntry* Pin : TypeMatches)
        {
            if (Pin->PinName == TEXT("ReturnValue"))
            {
                Context.Warnings.Add(...); // SEE DESIGN NOTE BELOW
                return Pin;
            }
        }

        // No "ReturnValue" -- return first match, log warning
        // NOTE: This method does not have access to Context for warnings.
        // The caller (WireDataConnection) should log the ambiguity warning.
        UE_LOG(LogOlivePlanExecutor, Warning,
            TEXT("Type auto-match: %d output pins match type, using first ('%s')"),
            TypeMatches.Num(), *TypeMatches[0]->PinName);
        return TypeMatches[0];
    }

    // No matches
    return nullptr;
}
```

**DESIGN NOTE:** `FindTypeCompatibleOutput` does not have access to `Context` (it is a const method on the manifest, not on context). Remove the `Context.Warnings.Add(...)` line shown above. Instead, if there is ambiguity, let the caller (`WireDataConnection`) add the warning. The method simply returns the best match or nullptr.

Here is the corrected version to implement:

```cpp
const FOlivePinManifestEntry* FOlivePlanExecutor::FindTypeCompatibleOutput(
    const FOlivePinManifest& SourceManifest,
    EOliveIRTypeCategory TargetTypeCategory,
    const FString& TargetSubCategory)
{
    TArray<const FOlivePinManifestEntry*> DataOutputs = SourceManifest.GetDataPins(false);

    TArray<const FOlivePinManifestEntry*> TypeMatches;
    for (const FOlivePinManifestEntry* Pin : DataOutputs)
    {
        if (Pin->IRTypeCategory == TargetTypeCategory)
        {
            TypeMatches.Add(Pin);
        }
    }

    if (TypeMatches.Num() == 1)
    {
        return TypeMatches[0];
    }

    if (TypeMatches.Num() > 1 && !TargetSubCategory.IsEmpty())
    {
        TArray<const FOlivePinManifestEntry*> SubMatches;
        for (const FOlivePinManifestEntry* Pin : TypeMatches)
        {
            if (Pin->PinSubCategory == TargetSubCategory)
            {
                SubMatches.Add(Pin);
            }
        }
        if (SubMatches.Num() == 1)
        {
            return SubMatches[0];
        }
    }

    if (TypeMatches.Num() > 1)
    {
        // Prefer "ReturnValue" pin
        for (const FOlivePinManifestEntry* Pin : TypeMatches)
        {
            if (Pin->PinName == TEXT("ReturnValue"))
            {
                return Pin;
            }
        }
        // Return first match (caller logs ambiguity warning)
        return TypeMatches[0];
    }

    return nullptr;
}
```

### ParseDataRef

```cpp
bool FOlivePlanExecutor::ParseDataRef(
    const FString& Ref,
    FString& OutStepId,
    FString& OutPinHint)
{
    // Expected format: "@stepId.pinHint" or "@stepId.auto" or "@stepId.~hint"
    if (!Ref.StartsWith(TEXT("@")))
    {
        return false;
    }

    FString Body = Ref.Mid(1); // Strip "@"

    int32 DotIndex;
    if (!Body.FindChar(TEXT('.'), DotIndex))
    {
        return false;
    }

    OutStepId = Body.Left(DotIndex);
    OutPinHint = Body.Mid(DotIndex + 1);

    return !OutStepId.IsEmpty() && !OutPinHint.IsEmpty();
}
```

### Phase 5: Set Pin Defaults

```cpp
void FOlivePlanExecutor::PhaseSetDefaults(
    const FOliveIRBlueprintPlan& Plan,
    FOlivePlanExecutionContext& Context)
{
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        for (const auto& InputPair : Step.Inputs)
        {
            const FString& PinKey = InputPair.Key;
            const FString& PinValue = InputPair.Value;

            // Skip @ref values (handled in Phase 4)
            if (PinValue.StartsWith(TEXT("@")))
            {
                continue;
            }

            // This is a literal default value
            const FOlivePinManifest* Manifest = Context.GetManifest(Step.StepId);
            if (!Manifest)
            {
                Context.FailedDefaultCount++;
                Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("DEFAULT_PIN_NOT_FOUND"),
                    Step.StepId,
                    FString::Printf(TEXT("/steps/inputs/%s"), *PinKey),
                    FString::Printf(TEXT("Step '%s' not found in manifests"), *Step.StepId),
                    TEXT("")));
                continue;
            }

            // Resolve the target input pin using the manifest
            FString MatchMethod;
            const FOlivePinManifestEntry* PinEntry = Manifest->FindPinSmart(
                PinKey, /*bIsInput=*/true,
                EOliveIRTypeCategory::Unknown, &MatchMethod);

            if (!PinEntry)
            {
                Context.FailedDefaultCount++;

                TArray<FString> Suggestions;
                TArray<const FOlivePinManifestEntry*> DataInputs = Manifest->GetDataPins(true);
                for (const FOlivePinManifestEntry* Pin : DataInputs)
                {
                    Suggestions.Add(FString::Printf(TEXT("%s (%s)"),
                        *Pin->PinName, *Pin->TypeDisplayString));
                }

                Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("DEFAULT_PIN_NOT_FOUND"),
                    Step.StepId,
                    FString::Printf(TEXT("/steps/inputs/%s"), *PinKey),
                    FString::Printf(TEXT("No input pin matching '%s' on step '%s'"),
                        *PinKey, *Step.StepId),
                    FString::Printf(TEXT("Available data inputs: %s"),
                        *FString::Join(Suggestions, TEXT(", ")))));
                continue;
            }

            // Set the default value directly on the UEdGraphPin*
            UEdGraphNode* Node = Context.GetNodePtr(Step.StepId);
            if (!Node)
            {
                Context.FailedDefaultCount++;
                continue;
            }

            UEdGraphPin* RealPin = Node->FindPin(FName(*PinEntry->PinName));
            if (!RealPin)
            {
                Context.FailedDefaultCount++;
                Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("DEFAULT_PIN_NOT_FOUND"),
                    Step.StepId,
                    FString::Printf(TEXT("/steps/inputs/%s"), *PinKey),
                    FString::Printf(TEXT("Pin '%s' resolved but not found on node"), *PinEntry->PinName),
                    TEXT("")));
                continue;
            }

            // Use the schema to set the default value properly
            const UEdGraphSchema* Schema = Context.Graph->GetSchema();
            if (Schema)
            {
                Schema->TrySetDefaultValue(*RealPin, PinValue);
            }
            else
            {
                RealPin->DefaultValue = PinValue;
            }

            Context.SuccessfulDefaultCount++;

            UE_LOG(LogOlivePlanExecutor, Verbose,
                TEXT("Set default: %s.%s = '%s' (matched via %s)"),
                *Step.StepId, *PinEntry->PinName, *PinValue, *MatchMethod);
        }
    }
}
```

### Phase 6: Auto-Layout

Simple exec-flow-aware layout. This is a basic version; the full `FOliveGraphLayoutEngine` is Phase D.

```cpp
void FOlivePlanExecutor::PhaseAutoLayout(
    const FOliveIRBlueprintPlan& Plan,
    FOlivePlanExecutionContext& Context)
{
    // Build adjacency: StepId -> list of successor StepIds
    TMap<FString, TArray<FString>> Successors;
    TMap<FString, FString> Predecessor; // StepId -> predecessor via exec_after
    TSet<FString> AllStepIds;

    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        AllStepIds.Add(Step.StepId);
        Successors.FindOrAdd(Step.StepId);

        if (!Step.ExecAfter.IsEmpty())
        {
            Successors.FindOrAdd(Step.ExecAfter).AddUnique(Step.StepId);
            Predecessor.Add(Step.StepId, Step.ExecAfter);
        }

        for (const auto& ExecOut : Step.ExecOutputs)
        {
            Successors.FindOrAdd(Step.StepId).AddUnique(ExecOut.Value);
            Predecessor.FindOrAdd(ExecOut.Value) = Step.StepId;
        }
    }

    // Find root nodes (no predecessor in exec flow)
    TArray<FString> Roots;
    for (const FString& StepId : AllStepIds)
    {
        if (!Predecessor.Contains(StepId))
        {
            Roots.Add(StepId);
        }
    }

    // BFS column assignment
    TMap<FString, int32> ColumnMap;
    TQueue<FString> BfsQueue;

    for (const FString& Root : Roots)
    {
        ColumnMap.Add(Root, 0);
        BfsQueue.Enqueue(Root);
    }

    while (!BfsQueue.IsEmpty())
    {
        FString Current;
        BfsQueue.Dequeue(Current);

        int32 CurrentCol = ColumnMap[Current];
        const TArray<FString>& Succs = Successors.FindOrAdd(Current);

        for (const FString& Succ : Succs)
        {
            int32 NewCol = CurrentCol + 1;
            int32* ExistingCol = ColumnMap.Find(Succ);
            if (!ExistingCol)
            {
                ColumnMap.Add(Succ, NewCol);
                BfsQueue.Enqueue(Succ);
            }
            else if (*ExistingCol < NewCol)
            {
                // Take the max column (longest path from any root)
                *ExistingCol = NewCol;
                BfsQueue.Enqueue(Succ); // Re-process successors
            }
        }
    }

    // Assign rows within each column
    TMap<int32, TArray<FString>> ColumnToSteps;
    for (const auto& Pair : ColumnMap)
    {
        ColumnToSteps.FindOrAdd(Pair.Value).Add(Pair.Key);
    }

    // Also handle pure nodes not in exec flow
    for (const FString& StepId : AllStepIds)
    {
        if (!ColumnMap.Contains(StepId))
        {
            const FOlivePinManifest* Manifest = Context.GetManifest(StepId);
            if (Manifest && Manifest->bIsPure)
            {
                // Place pure nodes one column before their first consumer
                // For now, place at column 0 with offset
                ColumnMap.Add(StepId, 0);
                ColumnToSteps.FindOrAdd(0).Add(StepId);
            }
            else
            {
                // Orphaned non-pure step -- put at end
                int32 MaxCol = 0;
                for (const auto& Col : ColumnMap)
                {
                    MaxCol = FMath::Max(MaxCol, Col.Value);
                }
                ColumnMap.Add(StepId, MaxCol + 1);
                ColumnToSteps.FindOrAdd(MaxCol + 1).Add(StepId);
            }
        }
    }

    // Apply positions
    for (const auto& ColPair : ColumnToSteps)
    {
        int32 Column = ColPair.Key;
        const TArray<FString>& Steps = ColPair.Value;

        for (int32 Row = 0; Row < Steps.Num(); ++Row)
        {
            const FString& StepId = Steps[Row];
            UEdGraphNode* Node = Context.GetNodePtr(StepId);
            if (!Node)
            {
                continue;
            }

            const FOlivePinManifest* Manifest = Context.GetManifest(StepId);
            bool bIsPure = Manifest ? Manifest->bIsPure : false;

            int32 PosX = Column * HORIZONTAL_SPACING;
            int32 PosY = Row * VERTICAL_SPACING;

            // Pure nodes: offset up and slightly left
            if (bIsPure)
            {
                PosY += PURE_NODE_OFFSET_Y;
                PosX -= HORIZONTAL_SPACING / 3;
            }

            Node->NodePosX = PosX;
            Node->NodePosY = PosY;
        }
    }
}
```

### BuildPinSuggestionList

```cpp
FString FOlivePlanExecutor::BuildPinSuggestionList(
    const FOlivePinManifest& Manifest,
    bool bInput,
    bool bExecOnly)
{
    TArray<FString> Names;

    for (const FOlivePinManifestEntry& Entry : Manifest.Pins)
    {
        if (Entry.bIsHidden) continue;
        if (Entry.bIsInput != bInput) continue;
        if (bExecOnly && !Entry.bIsExec) continue;
        if (!bExecOnly && Entry.bIsExec) continue;

        Names.Add(FString::Printf(TEXT("%s (%s)"), *Entry.PinName, *Entry.TypeDisplayString));
    }

    return FString::Join(Names, TEXT(", "));
}
```

### AssembleResult

```cpp
FOliveIRBlueprintPlanResult FOlivePlanExecutor::AssembleResult(
    const FOliveIRBlueprintPlan& Plan,
    const FOlivePlanExecutionContext& Context)
{
    FOliveIRBlueprintPlanResult Result;

    // Success = all nodes created (Phase 1 didn't fail-fast)
    bool bAllNodesCreated = (Context.CreatedNodeCount == Plan.Steps.Num());
    bool bHasWiringErrors = (Context.FailedConnectionCount > 0 || Context.FailedDefaultCount > 0);

    Result.bSuccess = bAllNodesCreated;
    Result.StepToNodeMap = Context.StepToNodeMap;
    Result.AppliedOpsCount = Context.CreatedNodeCount + Context.SuccessfulConnectionCount + Context.SuccessfulDefaultCount;
    Result.Errors = Context.WiringErrors;
    Result.Warnings = Context.Warnings;

    // Add partial success warning
    if (bAllNodesCreated && bHasWiringErrors)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("Partial success: %d nodes created, %d connections failed, %d defaults failed. "
                 "See wiring_errors and pin_manifests for repair instructions."),
            Context.CreatedNodeCount, Context.FailedConnectionCount, Context.FailedDefaultCount));
    }

    return Result;
}
```

---

## Integration with HandleBlueprintApplyPlanJson

This is Phase C work (not done in this file), but document the expected integration point so the executor is built correctly.

In `HandleBlueprintApplyPlanJson` (line ~6052 of OliveBlueprintToolHandlers.cpp), after the Resolve step succeeds, the handler currently does:

```cpp
FOlivePlanLowerResult LowerResult = FOliveBlueprintPlanLowerer::Lower(...);
```

For Phase C integration, this will become:

```cpp
if (Plan.SchemaVersion == TEXT("2.0"))
{
    // v2.0: Use new multi-phase executor with pin introspection
    // The executor runs INSIDE the write pipeline's executor lambda
    FOliveWriteExecutor Executor;
    Executor.BindLambda(
        [Plan, ResolvedSteps = ResolveResult.ResolvedSteps, AssetPath, GraphTarget]
        (const FOliveWriteRequest& InRequest, UObject* TargetAsset) -> FOliveWriteResult
        {
            UBlueprint* BP = Cast<UBlueprint>(TargetAsset);
            if (!BP) { return FOliveWriteResult::ExecutionError(...); }

            FOliveBatchExecutionScope BatchScope;
            BP->Modify();

            UEdGraph* ExecutionGraph = FindOrCreateFunctionGraph(BP, GraphTarget, ...);
            if (!ExecutionGraph) { return FOliveWriteResult::ExecutionError(...); }

            FOlivePlanExecutor PlanExecutor;
            FOliveIRBlueprintPlanResult PlanResult = PlanExecutor.Execute(
                Plan, ResolvedSteps, BP, ExecutionGraph, AssetPath, GraphTarget);

            // Convert to FOliveWriteResult
            TSharedPtr<FJsonObject> ResultData = PlanResult.ToJson();
            if (PlanResult.bSuccess)
            {
                FOliveWriteResult SuccessResult = FOliveWriteResult::Success(ResultData);
                return SuccessResult;
            }
            else
            {
                return FOliveWriteResult::ExecutionError(
                    TEXT("PLAN_EXECUTION_FAILED"),
                    FString::Printf(TEXT("Plan execution failed: %d errors"),
                        PlanResult.Errors.Num()),
                    PlanResult.Errors.Num() > 0 ? PlanResult.Errors[0].Suggestion : TEXT(""));
            }
        });

    FOliveWriteResult PipelineResult = Pipeline.Execute(Request, Executor);
    // ... convert to tool result ...
}
else
{
    // v1.0: Existing lowerer path (unchanged)
    FOlivePlanLowerResult LowerResult = FOliveBlueprintPlanLowerer::Lower(...);
    // ... existing code ...
}
```

The executor is constructed INSIDE the lambda, ensuring it runs within the pipeline's transaction scope. The `FOliveBatchExecutionScope` suppresses inner transactions from `FOlivePinConnector::Connect()`.

---

## Error Codes Used

| Code | Phase | Meaning |
|------|-------|---------|
| `NODE_CREATION_FAILED` | Phase 1 | Node factory returned failure |
| `EXEC_PIN_NOT_FOUND` | Phase 3 | No exec pin matching hint |
| `DATA_PIN_NOT_FOUND` | Phase 4 | No data pin matching hint |
| `DEFAULT_PIN_NOT_FOUND` | Phase 5 | No input pin for default value |
| `PURE_NODE_EXEC_SKIP` | Phase 3 | Warning: exec wire to pure node skipped |

---

## Build Verification

After creating both files, run the build:

```bash
"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex
```

The executor is not called by anything yet (integration is Phase C), so it just needs to compile. No runtime behavior changes.

---

## Checklist

- [ ] Header file created at `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`
- [ ] Implementation file created at `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
- [ ] `FOlivePlanExecutionContext` struct with all fields and lookup methods
- [ ] `FOliveSmartWireResult` struct
- [ ] `FOlivePlanExecutor` class with `Execute()` and all private methods
- [ ] `DEFINE_LOG_CATEGORY(LogOlivePlanExecutor)` in .cpp
- [ ] `DECLARE_LOG_CATEGORY_EXTERN(LogOlivePlanExecutor, Log, All)` in .h
- [ ] Phase 1: Creates nodes via `FOliveGraphWriter::Get().AddNode()`, retrieves `UEdGraphNode*` via `GetCachedNode()`, builds manifest via `FOlivePinManifest::Build()`
- [ ] Phase 1: Event reuse via `FindExistingEventNode()` using `FBlueprintEditorUtils::FindOverrideForFunction()` and `UK2Node_CustomEvent` iteration
- [ ] Phase 1: Fail-fast on any node creation error
- [ ] Phase 3: Exec wiring using manifest's `FindExecInput()` / `FindExecOutput()` / `FindAllExecOutputs()`
- [ ] Phase 3: Direct `FOlivePinConnector::Get().Connect()` with `UEdGraphPin*` from `Node->FindPin()`
- [ ] Phase 3: Pure node exec skip (warn, not error)
- [ ] Phase 4: Parses `@stepId.pinHint` via `ParseDataRef()`
- [ ] Phase 4: Handles `@step.auto` via `FindTypeCompatibleOutput()`
- [ ] Phase 4: Handles `@step.~hint` by stripping `~` and using `FindPinSmart()` with fuzzy
- [ ] Phase 4: Standard `@step.pinName` via `FindPinSmart()`
- [ ] Phase 5: Literal defaults via `Schema->TrySetDefaultValue()` on resolved pin
- [ ] Phase 6: BFS column assignment from exec flow, row assignment, pure node offset
- [ ] `AssembleResult()` builds `FOliveIRBlueprintPlanResult` with step_to_node_map, errors, warnings
- [ ] All connection/default failures logged but execution continues (CONTINUE-ON-FAILURE)
- [ ] Build compiles cleanly
