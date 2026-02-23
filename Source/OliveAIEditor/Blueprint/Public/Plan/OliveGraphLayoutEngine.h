// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Forward declarations
struct FOliveIRBlueprintPlan;
struct FOliveIRBlueprintPlanStep;
struct FOlivePlanExecutionContext;
class UEdGraphNode;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveGraphLayout, Log, All);

/**
 * Layout entry for a single step in a Blueprint plan.
 * Holds the computed grid position (Column, Row) and final pixel position
 * (PosX, PosY). Semantic flags (bIsPure, bIsBranchFalseTarget) influence
 * how ComputePositions() translates grid coordinates to pixels.
 */
struct OLIVEAIEDITOR_API FOliveLayoutEntry
{
    /** The step ID this entry corresponds to */
    FString StepId;

    /** Column in the exec-flow grid (0 = leftmost root) */
    int32 Column = 0;

    /** Row within the column */
    int32 Row = 0;

    /** Final pixel X position for UEdGraphNode::NodePosX */
    int32 PosX = 0;

    /** Final pixel Y position for UEdGraphNode::NodePosY */
    int32 PosY = 0;

    /** Whether the node is a pure node (no exec pins) */
    bool bIsPure = false;

    /** Whether the node is a branch-false target (gets extra Y offset) */
    bool bIsBranchFalseTarget = false;
};

/**
 * FOliveGraphLayoutEngine
 *
 * Standalone, stateless layout engine for Blueprint plan graphs.
 * Computes node positions from exec-flow topology with three enhancements
 * over the original inline layout in OlivePlanExecutor:
 *
 *   1. Consumer-relative pure node placement
 *      Pure nodes are placed near the node that consumes their output,
 *      offset up and to the left, instead of piling up at column 0.
 *
 *   2. Branch offset
 *      Branch False/Else targets receive a BRANCH_OFFSET vertical offset
 *      so the two paths are visually separated.
 *
 *   3. Multi-chain vertical stacking
 *      Multiple independent exec chains (e.g., BeginPlay + Tick) are
 *      stacked vertically with CHAIN_GAP_ROWS spacing instead of
 *      overlapping at Y=0.
 *
 * Usage:
 *   TMap<FString, FOliveLayoutEntry> Layout =
 *       FOliveGraphLayoutEngine::ComputeLayout(Plan, Context);
 *   FOliveGraphLayoutEngine::ApplyLayout(Layout, Context);
 *
 * Thread Safety: Stateless, all methods are static. Must be called on
 *                the game thread (reads UEdGraphNode data via Context).
 */
class OLIVEAIEDITOR_API FOliveGraphLayoutEngine
{
public:
    /**
     * Compute layout positions for all steps in a plan.
     *
     * Builds an exec-flow graph from the plan's ExecAfter and ExecOutputs
     * fields, performs BFS column assignment, assigns rows with branch
     * awareness, places pure nodes relative to their consumers, and
     * computes final pixel positions with multi-chain stacking.
     *
     * @param Plan      The parsed Blueprint plan (provides step topology)
     * @param Context   Execution context (provides pin manifests for pure detection)
     * @return Map from StepId to FOliveLayoutEntry with computed positions
     */
    static TMap<FString, FOliveLayoutEntry> ComputeLayout(
        const FOliveIRBlueprintPlan& Plan,
        const FOlivePlanExecutionContext& Context);

    /**
     * Apply computed layout positions to the actual UEdGraphNode pointers.
     *
     * @param Layout    The layout map returned by ComputeLayout()
     * @param Context   Execution context (provides StepId -> UEdGraphNode* mapping)
     */
    static void ApplyLayout(
        const TMap<FString, FOliveLayoutEntry>& Layout,
        FOlivePlanExecutionContext& Context);

private:
    // ====================================================================
    // Internal Exec Graph Representation
    // ====================================================================

    /**
     * Adjacency representation of the exec flow topology.
     * Built once from Plan.Steps, then consumed by all layout phases.
     */
    struct FOliveExecGraph
    {
        /** StepId -> list of successor StepIds (exec flow forward) */
        TMap<FString, TArray<FString>> Successors;

        /** StepId -> predecessor StepId (exec flow backward; single predecessor per step) */
        TMap<FString, FString> Predecessor;

        /** All step IDs in the plan */
        TSet<FString> AllStepIds;

        /** Steps with no predecessor in exec flow, in plan order */
        TArray<FString> Roots;
    };

    // ====================================================================
    // Layout Phases (called in order by ComputeLayout)
    // ====================================================================

    /**
     * Build exec-flow adjacency graph from plan step topology.
     * Populates Successors, Predecessor, AllStepIds, and Roots.
     *
     * @param Plan The plan containing steps with ExecAfter and ExecOutputs
     * @return Populated exec graph
     */
    static FOliveExecGraph BuildExecGraph(const FOliveIRBlueprintPlan& Plan);

    /**
     * BFS column assignment from root nodes.
     * Each successor gets column = max(all predecessor columns) + 1.
     * Re-enqueues on column upgrade to propagate longest-path distances.
     *
     * @param ExecGraph The exec flow graph with roots and successors
     * @return Map from StepId to column index
     */
    static TMap<FString, int32> AssignColumns(const FOliveExecGraph& ExecGraph);

    /**
     * Assign rows within each column, with branch-aware ordering.
     * Steps are sorted by their plan index within each column.
     * Steps that arrive via a Branch node's "False"/"Else" output are
     * marked as bIsBranchFalseTarget for later Y offset.
     *
     * @param Plan       The plan (for step ordering and ExecOutputs inspection)
     * @param ExecGraph  The exec flow graph (for predecessor lookup)
     * @param ColumnMap  StepId -> column index from AssignColumns
     * @return Layout entries with Column, Row, and bIsBranchFalseTarget populated
     */
    static TMap<FString, FOliveLayoutEntry> AssignRows(
        const FOliveIRBlueprintPlan& Plan,
        const FOliveExecGraph& ExecGraph,
        const TMap<FString, int32>& ColumnMap);

    /**
     * Build a map from pure step IDs to their first consumer's step ID.
     * Scans all step inputs for "@stepId" references and checks if the
     * referenced step is pure via Context.GetManifest().
     *
     * @param Plan     The plan containing steps with input references
     * @param Context  Execution context for pin manifest lookups
     * @return Map from pure step ID -> consumer step ID (first consumer wins)
     */
    static TMap<FString, FString> BuildConsumerMap(
        const FOliveIRBlueprintPlan& Plan,
        const FOlivePlanExecutionContext& Context);

    /**
     * Place pure nodes relative to their consumer nodes.
     * Pure nodes with a known consumer get associated with the consumer
     * for later consumer-relative pixel positioning. Pure nodes without
     * a consumer are placed at MaxColumn + 1 in sequential rows.
     *
     * @param Plan       The plan (for step iteration)
     * @param Context    Execution context (for manifest lookups)
     * @param ExecGraph  The exec flow graph (for AllStepIds)
     * @param ColumnMap  Current column assignments (pure nodes not yet in here)
     * @param OutLayout  Layout entries to populate with pure node entries
     */
    static void PlacePureNodes(
        const FOliveIRBlueprintPlan& Plan,
        const FOlivePlanExecutionContext& Context,
        const FOliveExecGraph& ExecGraph,
        const TMap<FString, int32>& ColumnMap,
        TMap<FString, FOliveLayoutEntry>& OutLayout);

    /**
     * Convert column/row grid positions to pixel coordinates.
     * Processes each root's subtree independently and stacks them
     * vertically with CHAIN_GAP_ROWS spacing. Applies BRANCH_OFFSET
     * to branch-false targets and consumer-relative positioning to
     * pure nodes.
     *
     * @param Plan       The plan (for step topology during BFS)
     * @param Context    Execution context (for manifest lookups)
     * @param ExecGraph  The exec flow graph (for subtree traversal)
     * @param OutLayout  Layout entries with Column/Row set; PosX/PosY will be written
     */
    static void ComputePositions(
        const FOliveIRBlueprintPlan& Plan,
        const FOlivePlanExecutionContext& Context,
        const FOliveExecGraph& ExecGraph,
        TMap<FString, FOliveLayoutEntry>& OutLayout);

    // ====================================================================
    // Layout Constants
    // ====================================================================

    /** Horizontal pixel spacing between columns */
    static constexpr int32 HORIZONTAL_SPACING = 350;

    /** Vertical pixel spacing between rows */
    static constexpr int32 VERTICAL_SPACING = 200;

    /** Extra vertical offset for branch False/Else targets */
    static constexpr int32 BRANCH_OFFSET = 250;

    /** Vertical offset for pure nodes above their consumer */
    static constexpr int32 PURE_NODE_OFFSET_Y = -120;

    /** Number of empty rows between independent exec chains */
    static constexpr int32 CHAIN_GAP_ROWS = 2;
};
