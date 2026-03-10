// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Plan/OlivePinManifest.h"
#include "IR/BlueprintPlanIR.h"
#include "Writer/OliveWiringDiagnostic.h"
#include "Dom/JsonObject.h"

// Forward declarations
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
struct FOliveResolvedStep;

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePlanExecutor, Log, All);

/**
 * Records an auto-conversion node insertion during data wiring.
 * Logged and serialized so the AI sees what type coercions happened.
 * Created by PhaseWireData when PinConnector::Connect inserts an
 * intermediate conversion node between two incompatible pin types.
 */
struct OLIVEAIEDITOR_API FOliveConversionNote
{
    /** Step ID of the source (output) pin */
    FString SourceStep;

    /** Step ID of the target (input) pin */
    FString TargetStep;

    /** Pin name on the source node */
    FString SourcePinName;

    /** Pin name on the target node */
    FString TargetPinName;

    /** Human-readable type of the source pin (e.g., "Float") */
    FString FromType;

    /** Human-readable type of the target pin (e.g., "Double") */
    FString ToType;

    /** Class name of the inserted conversion node */
    FString ConversionNodeType;

    /**
     * Serialize this note to a JSON object for inclusion in tool results.
     * @return JSON representation of the conversion note
     */
    TSharedPtr<FJsonObject> ToJson() const
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("source_step"), SourceStep);
        Obj->SetStringField(TEXT("target_step"), TargetStep);
        Obj->SetStringField(TEXT("source_pin"), SourcePinName);
        Obj->SetStringField(TEXT("target_pin"), TargetPinName);
        Obj->SetStringField(TEXT("from_type"), FromType);
        Obj->SetStringField(TEXT("to_type"), ToType);
        Obj->SetStringField(TEXT("conversion_node"), ConversionNodeType);
        return Obj;
    }
};

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

    /** Step IDs that reuse pre-existing event nodes (do NOT remove on rollback) */
    TSet<FString> ReusedStepIds;

    /** Step IDs whose exec_after was successfully wired in Phase 3.
     *  Used to build TargetedStepIds accurately — only successfully wired
     *  steps are considered "targeted"; failed wires leave the step as an
     *  orphan so auto-chain can rescue it. */
    TSet<FString> SuccessfulExecAfterStepIds;

    /** Function names resolved in this plan (for stale error detection) */
    TSet<FString> ResolvedFunctionNames;

    /** Class names resolved in this plan (for stale error detection) */
    TSet<FString> ResolvedClassNames;

    /** Auto-conversion notes logged when PinConnector inserts conversion nodes */
    TArray<FOliveConversionNote> ConversionNotes;

    /** Pre-compile validation issues (Phase 5.5) -- things that couldn't be auto-fixed */
    TArray<FString> PreCompileIssues;

    /** Count of issues auto-fixed by executor (Phase 1.5 + Phase 5.5) */
    int32 AutoFixCount = 0;

    /** True if Phase 4 (data wiring) had any failures — forces rollback in AssembleResult */
    bool bHasDataWireFailure = false;

    /** StepId -> @ref string for spawn_actor steps with dynamic class references.
     *  Populated during Phase 1 from resolved step properties ("dynamic_class_ref").
     *  Consumed by Phase 4 to wire the Class pin on SpawnActor nodes. */
    TMap<FString, FString> DynamicClassRefs;

    /** Reverse map: NodeId -> StepId (built from StepToNodeMap during Execute) */
    TMap<FString, FString> NodeIdToStepId;

    /** Pointer to plan for Phase 5.5 exec recovery lookups. NOT owned. */
    const FOliveIRBlueprintPlan* Plan = nullptr;

    /** Get manifest for a step, or nullptr if step was not created */
    const FOlivePinManifest* GetManifest(const FString& StepId) const;

    /** Get the node pointer for a step, or nullptr */
    UEdGraphNode* GetNodePtr(const FString& StepId) const;

    /** Get the node ID for a step, or empty string */
    FString GetNodeId(const FString& StepId) const;

    /** Reverse lookup: get step ID for a given UEdGraphNode*, or empty string.
     *  Iterates StepToNodePtr (small map, typically <50 entries). */
    FString FindStepIdForNode(const UEdGraphNode* Node) const;
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

    /** Whether the failure was due to type incompatibility (vs pin not found).
     *  When true, error code should be DATA_WIRE_INCOMPATIBLE instead of DATA_PIN_NOT_FOUND. */
    bool bIsTypeIncompatible = false;
};

/**
 * FOlivePlanExecutor
 *
 * Multi-phase plan execution engine with post-creation pin introspection.
 * Replaces the lowerer (FOliveBlueprintPlanLowerer) for schema_version "2.0" plans.
 *
 * Execution phases:
 *   Phase 1:   Create all nodes + build pin manifests (FAIL-FAST)
 *   Phase 1.5: Auto-wire component function Target pins (CONTINUE-ON-FAILURE)
 *   Phase 3:   Wire exec connections using manifest (CONTINUE-ON-FAILURE)
 *   Phase 4:   Wire data connections using manifest (CONTINUE-ON-FAILURE)
 *   Phase 5:   Set pin defaults using manifest (CONTINUE-ON-FAILURE)
 *   Phase 5.5: Pre-compile validation with auto-fix (CONTINUE-ON-FAILURE)
 *   Phase 6:   Auto-layout using exec flow topology (ALWAYS SUCCEEDS)
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
    // Pre-Execution Cleanup
    // ====================================================================

    /**
     * Pre-execution cleanup: remove orphaned node chains from events the current plan targets.
     *
     * When plan_json fails and the agent retries on the same graph, nodes from previous
     * successful plans accumulate as unconnected chains hanging off event nodes. This pass
     * finds those chains and removes them before creating new nodes.
     *
     * Algorithm:
     * 1. For each event/custom_event step in the plan, find the existing event node in the graph
     * 2. BFS forward from the event node through exec output pins (follow LinkedTo)
     * 3. Collect all reachable nodes (capped at 200)
     * 4. Check isolation: if any collected node connects to a node NOT in the set, skip cleanup
     * 5. If isolated: remove all collected nodes EXCEPT the event node itself
     *
     * Safety: never deletes event nodes, caps chain traversal, only removes fully isolated chains.
     *
     * @param Graph  The target graph to clean up
     * @param Plan   The plan being executed (used to identify target events)
     */
    void CleanupStaleEventChains(UEdGraph* Graph, const FOliveIRBlueprintPlan& Plan);

    // ====================================================================
    // Phase Methods
    // ====================================================================

    /** Phase 1: Create all nodes + build pin manifests. FAIL-FAST. */
    bool PhaseCreateNodes(
        const FOliveIRBlueprintPlan& Plan,
        const TArray<FOliveResolvedStep>& ResolvedSteps,
        FOlivePlanExecutionContext& Context);

    /**
     * Phase 1.5: Auto-wire component function Target pins.
     * For each CallFunction node targeting a component-class function:
     * if the Target/Self pin is unwired AND the Blueprint has exactly one
     * SCS component of the required class, inject a UK2Node_VariableGet
     * for that component and wire its output to the Self pin.
     *
     * Skips if: Blueprint is itself a component, AI already provided Target
     * input as @ref, Target pin is already connected, or multiple/zero
     * matching components exist.
     *
     * CONTINUE-ON-FAILURE per step; failure for one step does not block others.
     */
    void PhaseAutoWireComponentTargets(
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

    /**
     * Phase 5.5: Pre-compile validation with auto-fix.
     * Runs after all wiring phases, before auto-layout.
     *
     * Check 1: Orphaned impure nodes (exec input not wired).
     *   Auto-fix: If the orphan's plan step has exec_after set and the source
     *   step's primary exec output is unwired, reconnect them. This recovers
     *   from pin-name-drift failures in Phase 3.
     *
     * Check 2: Unwired Self/Target on component function calls.
     *   Reports as PreCompileIssue only (Phase 1.5 should have caught these).
     *   This is defense-in-depth.
     *
     * CONTINUE-ON-FAILURE. Never blocks execution.
     */
    void PhasePreCompileValidation(
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
        const FString& TargetSubCategory,
        const FString& TargetPinName = FString());

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

    /**
     * Find existing UK2Node_EnhancedInputAction in graph for reuse.
     * Matches by UInputAction asset name (case-insensitive).
     * @param Graph The graph to search
     * @param InputActionName The IA_ name to search for (e.g., "IA_Interact")
     * @return The existing node, or nullptr if not found
     */
    UEdGraphNode* FindExistingEnhancedInputNode(
        UEdGraph* Graph,
        const FString& InputActionName);

    /**
     * Find existing UK2Node_ComponentBoundEvent in graph for reuse.
     * Matches by delegate property name and component property name.
     * @param Graph The graph to search
     * @param DelegateName The delegate property name (e.g., "OnComponentBeginOverlap")
     * @param ComponentName The component variable name (e.g., "CollisionComp")
     * @return The existing node, or nullptr if not found
     */
    UEdGraphNode* FindExistingComponentBoundEventNode(
        UEdGraph* Graph,
        const FString& DelegateName,
        const FString& ComponentName);

    /** Build human-readable list of available pins for error messages */
    FString BuildPinSuggestionList(
        const FOlivePinManifest& Manifest,
        bool bInput,
        bool bExecOnly);

    /** Assemble FOliveIRBlueprintPlanResult from context */
    FOliveIRBlueprintPlanResult AssembleResult(
        const FOliveIRBlueprintPlan& Plan,
        const FOlivePlanExecutionContext& Context);

};
