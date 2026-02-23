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

};
