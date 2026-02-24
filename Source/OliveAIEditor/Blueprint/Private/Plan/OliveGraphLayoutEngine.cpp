// Copyright Bode Software. All Rights Reserved.

/**
 * OliveGraphLayoutEngine.cpp
 *
 * Standalone Blueprint graph layout engine. Extracts and enhances the
 * inline layout logic that was in OlivePlanExecutor::PhaseAutoLayout.
 *
 * Three key improvements over the original:
 *   1. Consumer-relative pure node placement
 *   2. Branch False/Else Y offset
 *   3. Multi-chain vertical stacking
 *
 * See OliveGraphLayoutEngine.h for full class documentation.
 */

#include "Plan/OliveGraphLayoutEngine.h"
#include "Plan/OlivePinManifest.h"
#include "Plan/OlivePlanExecutor.h"  // For FOlivePlanExecutionContext
#include "IR/BlueprintPlanIR.h"
#include "EdGraph/EdGraphNode.h"

DEFINE_LOG_CATEGORY(LogOliveGraphLayout);

// ============================================================================
// Public API
// ============================================================================

TMap<FString, FOliveLayoutEntry> FOliveGraphLayoutEngine::ComputeLayout(
    const FOliveIRBlueprintPlan& Plan,
    const FOlivePlanExecutionContext& Context)
{
    // Handle empty plan
    if (Plan.Steps.Num() == 0)
    {
        UE_LOG(LogOliveGraphLayout, Verbose, TEXT("ComputeLayout: Empty plan, returning empty layout"));
        return TMap<FString, FOliveLayoutEntry>();
    }

    // Phase 1: Build exec-flow adjacency graph
    FOliveExecGraph ExecGraph = BuildExecGraph(Plan);

    UE_LOG(LogOliveGraphLayout, Verbose,
        TEXT("ComputeLayout: %d steps, %d roots"),
        ExecGraph.AllStepIds.Num(), ExecGraph.Roots.Num());

    // Phase 2: BFS column assignment from roots
    TMap<FString, int32> ColumnMap = AssignColumns(ExecGraph);

    UE_LOG(LogOliveGraphLayout, Verbose,
        TEXT("ComputeLayout: %d steps assigned to columns"),
        ColumnMap.Num());

    // Phase 3: Row assignment with branch awareness
    TMap<FString, FOliveLayoutEntry> Layout = AssignRows(Plan, ExecGraph, ColumnMap);

    // Phase 4: Place pure nodes relative to their consumers
    PlacePureNodes(Plan, Context, ExecGraph, ColumnMap, Layout);

    // Phase 5: Convert grid positions to pixel coordinates
    ComputePositions(Plan, Context, ExecGraph, Layout);

    return Layout;
}

void FOliveGraphLayoutEngine::ApplyLayout(
    const TMap<FString, FOliveLayoutEntry>& Layout,
    FOlivePlanExecutionContext& Context)
{
    for (const auto& Pair : Layout)
    {
        UEdGraphNode* Node = Context.GetNodePtr(Pair.Key);
        if (Node)
        {
            Node->NodePosX = Pair.Value.PosX;
            Node->NodePosY = Pair.Value.PosY;
        }
    }
}

// ============================================================================
// BuildExecGraph
// ============================================================================

FOliveGraphLayoutEngine::FOliveExecGraph FOliveGraphLayoutEngine::BuildExecGraph(
    const FOliveIRBlueprintPlan& Plan)
{
    FOliveExecGraph Graph;

    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        Graph.AllStepIds.Add(Step.StepId);
        Graph.Successors.FindOrAdd(Step.StepId);

        // ExecAfter: this step executes after another step
        if (!Step.ExecAfter.IsEmpty() && Step.ExecAfter != Step.StepId)
        {
            Graph.Successors.FindOrAdd(Step.ExecAfter).AddUnique(Step.StepId);
            Graph.Predecessor.Add(Step.StepId, Step.ExecAfter);
        }

        // ExecOutputs: this step's named exec outputs connect to other steps
        for (const auto& ExecOut : Step.ExecOutputs)
        {
            // Skip self-loops (e.g., LLM mistakenly wiring a step's exec output back to itself)
            if (ExecOut.Value == Step.StepId)
            {
                UE_LOG(LogOliveGraphLayout, Warning,
                    TEXT("BuildExecGraph: Skipping self-loop on step '%s' (exec_output '%s' -> self)"),
                    *Step.StepId, *ExecOut.Key);
                continue;
            }
            Graph.Successors.FindOrAdd(Step.StepId).AddUnique(ExecOut.Value);
            Graph.Predecessor.FindOrAdd(ExecOut.Value) = Step.StepId;
        }
    }

    // Find roots (steps with no predecessor in exec flow), preserving plan order
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        if (!Graph.Predecessor.Contains(Step.StepId))
        {
            // Only add non-pure steps as roots. Pure steps will be handled
            // separately in PlacePureNodes. However, we cannot check purity
            // here (no Context available), so we add all non-predecessored
            // steps. PlacePureNodes will handle re-categorization later.
            Graph.Roots.Add(Step.StepId);
        }
    }

    return Graph;
}

// ============================================================================
// AssignColumns
// ============================================================================

TMap<FString, int32> FOliveGraphLayoutEngine::AssignColumns(
    const FOliveExecGraph& ExecGraph)
{
    TMap<FString, int32> ColumnMap;
    TQueue<FString> BfsQueue;

    // Seed BFS from all roots at column 0
    for (const FString& Root : ExecGraph.Roots)
    {
        ColumnMap.Add(Root, 0);
        BfsQueue.Enqueue(Root);
    }

    // Safety bound: in a DAG the max BFS iterations is O(V*E). Use a generous
    // upper bound to prevent infinite loops from unexpected cycles in the plan.
    const int32 MaxIterations = ExecGraph.AllStepIds.Num() * ExecGraph.AllStepIds.Num() + 100;
    int32 Iterations = 0;

    while (!BfsQueue.IsEmpty())
    {
        if (++Iterations > MaxIterations)
        {
            UE_LOG(LogOliveGraphLayout, Error,
                TEXT("AssignColumns: BFS exceeded %d iterations (possible cycle in exec graph). Aborting column assignment."),
                MaxIterations);
            break;
        }

        FString Current;
        BfsQueue.Dequeue(Current);

        const int32 CurrentCol = ColumnMap[Current];
        const TArray<FString>* Succs = ExecGraph.Successors.Find(Current);
        if (!Succs)
        {
            continue;
        }

        for (const FString& Succ : *Succs)
        {
            const int32 NewCol = CurrentCol + 1;
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
                BfsQueue.Enqueue(Succ); // Re-process successors with updated column
            }
        }
    }

    return ColumnMap;
}

// ============================================================================
// AssignRows
// ============================================================================

TMap<FString, FOliveLayoutEntry> FOliveGraphLayoutEngine::AssignRows(
    const FOliveIRBlueprintPlan& Plan,
    const FOliveExecGraph& ExecGraph,
    const TMap<FString, int32>& ColumnMap)
{
    TMap<FString, FOliveLayoutEntry> Layout;

    // Build a plan-index lookup for sorting steps within columns
    TMap<FString, int32> PlanIndex;
    for (int32 i = 0; i < Plan.Steps.Num(); ++i)
    {
        PlanIndex.Add(Plan.Steps[i].StepId, i);
    }

    // Build a lookup from StepId -> FOliveIRBlueprintPlanStep for predecessor inspection
    TMap<FString, const FOliveIRBlueprintPlanStep*> StepLookup;
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        StepLookup.Add(Step.StepId, &Step);
    }

    // Group steps by column
    TMap<int32, TArray<FString>> ColumnToSteps;
    for (const auto& Pair : ColumnMap)
    {
        ColumnToSteps.FindOrAdd(Pair.Value).Add(Pair.Key);
    }

    // Sort steps within each column by plan index (preserves author intent)
    for (auto& Pair : ColumnToSteps)
    {
        Pair.Value.Sort([&PlanIndex](const FString& A, const FString& B)
        {
            const int32* IdxA = PlanIndex.Find(A);
            const int32* IdxB = PlanIndex.Find(B);
            // Steps not found in PlanIndex (shouldn't happen) sort to the end
            const int32 SafeA = IdxA ? *IdxA : MAX_int32;
            const int32 SafeB = IdxB ? *IdxB : MAX_int32;
            return SafeA < SafeB;
        });
    }

    // Assign rows within each column and detect branch-false targets
    for (const auto& ColPair : ColumnToSteps)
    {
        const int32 Column = ColPair.Key;
        const TArray<FString>& Steps = ColPair.Value;

        for (int32 Row = 0; Row < Steps.Num(); ++Row)
        {
            const FString& StepId = Steps[Row];

            FOliveLayoutEntry Entry;
            Entry.StepId = StepId;
            Entry.Column = Column;
            Entry.Row = Row;

            // Check if this step is a branch-false target
            const FString* PredId = ExecGraph.Predecessor.Find(StepId);
            if (PredId)
            {
                const FOliveIRBlueprintPlanStep** PredStep = StepLookup.Find(*PredId);
                if (PredStep && *PredStep)
                {
                    // Search the predecessor's ExecOutputs for the key that maps to this step
                    for (const auto& ExecOut : (*PredStep)->ExecOutputs)
                    {
                        if (ExecOut.Value == StepId)
                        {
                            const FString& Key = ExecOut.Key;
                            if (Key.Equals(TEXT("False"), ESearchCase::IgnoreCase) ||
                                Key.Equals(TEXT("Else"), ESearchCase::IgnoreCase))
                            {
                                Entry.bIsBranchFalseTarget = true;
                            }
                            break;
                        }
                    }
                }
            }

            Layout.Add(StepId, MoveTemp(Entry));
        }
    }

    return Layout;
}

// ============================================================================
// BuildConsumerMap
// ============================================================================

TMap<FString, FString> FOliveGraphLayoutEngine::BuildConsumerMap(
    const FOliveIRBlueprintPlan& Plan,
    const FOlivePlanExecutionContext& Context)
{
    TMap<FString, FString> ConsumerMap; // PureStepId -> ConsumerStepId

    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        for (const auto& InputPair : Step.Inputs)
        {
            const FString& Value = InputPair.Value;
            if (!Value.StartsWith(TEXT("@")))
            {
                continue;
            }

            // Parse "@stepId" or "@stepId.pinHint"
            // Extract stepId: everything between '@' and '.' (or end of string)
            FString RefStepId;
            const int32 DotIdx = Value.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromStart, 1);
            if (DotIdx != INDEX_NONE)
            {
                RefStepId = Value.Mid(1, DotIdx - 1);
            }
            else
            {
                RefStepId = Value.Mid(1);
            }

            if (RefStepId.IsEmpty())
            {
                continue;
            }

            // Check if the referenced step is a pure node
            const FOlivePinManifest* Manifest = Context.GetManifest(RefStepId);
            if (Manifest && Manifest->bIsPure)
            {
                // First consumer wins
                if (!ConsumerMap.Contains(RefStepId))
                {
                    ConsumerMap.Add(RefStepId, Step.StepId);
                }
            }
        }
    }

    UE_LOG(LogOliveGraphLayout, Verbose,
        TEXT("BuildConsumerMap: Found %d pure-node-to-consumer mappings"),
        ConsumerMap.Num());

    return ConsumerMap;
}

// ============================================================================
// PlacePureNodes
// ============================================================================

void FOliveGraphLayoutEngine::PlacePureNodes(
    const FOliveIRBlueprintPlan& Plan,
    const FOlivePlanExecutionContext& Context,
    const FOliveExecGraph& ExecGraph,
    const TMap<FString, int32>& ColumnMap,
    TMap<FString, FOliveLayoutEntry>& OutLayout)
{
    // Build consumer map to know where each pure node's output goes
    TMap<FString, FString> ConsumerMap = BuildConsumerMap(Plan, Context);

    // Find the max column for orphan placement
    int32 MaxColumn = 0;
    for (const auto& Pair : ColumnMap)
    {
        MaxColumn = FMath::Max(MaxColumn, Pair.Value);
    }

    // Track row assignment for orphaned pure nodes at MaxColumn + 1
    int32 OrphanRow = 0;
    int32 PureNodeCount = 0;

    for (const FString& StepId : ExecGraph.AllStepIds)
    {
        // Skip steps already placed by AssignColumns/AssignRows
        if (ColumnMap.Contains(StepId))
        {
            continue;
        }

        const FOlivePinManifest* Manifest = Context.GetManifest(StepId);
        if (!Manifest)
        {
            continue;
        }

        const bool bIsPure = Manifest->bIsPure;

        FOliveLayoutEntry Entry;
        Entry.StepId = StepId;
        Entry.bIsPure = bIsPure;

        const FString* ConsumerId = ConsumerMap.Find(StepId);
        if (ConsumerId && OutLayout.Contains(*ConsumerId))
        {
            // Pure node with a known consumer: associate with consumer's grid position.
            // Actual pixel positions are computed in ComputePositions() using
            // consumer-relative offsets.
            const FOliveLayoutEntry& ConsumerEntry = OutLayout[*ConsumerId];
            Entry.Column = ConsumerEntry.Column;
            Entry.Row = ConsumerEntry.Row;

            PureNodeCount++;
        }
        else
        {
            // No consumer found (or consumer not in layout): place at far right
            Entry.Column = MaxColumn + 1;
            Entry.Row = OrphanRow++;
        }

        OutLayout.Add(StepId, MoveTemp(Entry));
    }

    UE_LOG(LogOliveGraphLayout, Verbose,
        TEXT("PlacePureNodes: %d consumer-relative, %d orphaned"),
        PureNodeCount, OrphanRow);
}

// ============================================================================
// ComputePositions
// ============================================================================

void FOliveGraphLayoutEngine::ComputePositions(
    const FOliveIRBlueprintPlan& Plan,
    const FOlivePlanExecutionContext& Context,
    const FOliveExecGraph& ExecGraph,
    TMap<FString, FOliveLayoutEntry>& OutLayout)
{
    // Build consumer map again for pure-node consumer-relative placement.
    // This is lightweight (just scanning inputs) so no need to cache.
    TMap<FString, FString> ConsumerMap = BuildConsumerMap(Plan, Context);

    // Track which steps have been positioned (to handle multi-chain stacking)
    TSet<FString> Positioned;

    // Process each root's subtree independently, stacking vertically
    int32 ChainYOffset = 0;

    for (const FString& Root : ExecGraph.Roots)
    {
        // Skip roots that are pure nodes (they are positioned relative to consumer)
        const FOlivePinManifest* RootManifest = Context.GetManifest(Root);
        if (RootManifest && RootManifest->bIsPure)
        {
            continue;
        }

        // Skip roots already positioned (could happen if a root is a successor
        // of another root via ExecOutputs, caught by the column assignment)
        if (Positioned.Contains(Root))
        {
            continue;
        }

        // BFS from this root to collect all steps in this subtree
        TArray<FString> SubtreeSteps;
        TQueue<FString> BfsQueue;
        TSet<FString> Visited;

        BfsQueue.Enqueue(Root);
        Visited.Add(Root);

        while (!BfsQueue.IsEmpty())
        {
            FString Current;
            BfsQueue.Dequeue(Current);
            SubtreeSteps.Add(Current);

            const TArray<FString>* Succs = ExecGraph.Successors.Find(Current);
            if (Succs)
            {
                for (const FString& Succ : *Succs)
                {
                    if (!Visited.Contains(Succ))
                    {
                        Visited.Add(Succ);
                        BfsQueue.Enqueue(Succ);
                    }
                }
            }
        }

        // Compute pixel positions for all steps in this subtree
        int32 MaxRowInSubtree = 0;

        for (const FString& StepId : SubtreeSteps)
        {
            FOliveLayoutEntry* Entry = OutLayout.Find(StepId);
            if (!Entry)
            {
                continue;
            }

            Entry->PosX = Entry->Column * HORIZONTAL_SPACING;
            Entry->PosY = Entry->Row * VERTICAL_SPACING + ChainYOffset;

            if (Entry->bIsBranchFalseTarget)
            {
                Entry->PosY += BRANCH_OFFSET;
            }

            MaxRowInSubtree = FMath::Max(MaxRowInSubtree, Entry->Row);
            Positioned.Add(StepId);
        }

        // Advance Y offset for the next independent chain
        ChainYOffset += (MaxRowInSubtree + 1 + CHAIN_GAP_ROWS) * VERTICAL_SPACING;
    }

    // Position orphaned non-pure steps that weren't in any subtree
    for (auto& Pair : OutLayout)
    {
        if (Positioned.Contains(Pair.Key))
        {
            continue;
        }

        // Skip pure nodes for now (handled in the consumer-relative pass below)
        if (Pair.Value.bIsPure)
        {
            continue;
        }

        Pair.Value.PosX = Pair.Value.Column * HORIZONTAL_SPACING;
        Pair.Value.PosY = Pair.Value.Row * VERTICAL_SPACING + ChainYOffset;
        Positioned.Add(Pair.Key);
    }

    // Consumer-relative pure node positioning pass
    // Must happen after all non-pure positions are finalized
    for (auto& Pair : OutLayout)
    {
        if (!Pair.Value.bIsPure)
        {
            continue;
        }

        const FString* ConsumerId = ConsumerMap.Find(Pair.Key);
        if (ConsumerId)
        {
            const FOliveLayoutEntry* ConsumerEntry = OutLayout.Find(*ConsumerId);
            if (ConsumerEntry)
            {
                // Position relative to consumer: offset left and above
                Pair.Value.PosX = ConsumerEntry->PosX - HORIZONTAL_SPACING / 2;
                Pair.Value.PosY = ConsumerEntry->PosY + PURE_NODE_OFFSET_Y;
                Positioned.Add(Pair.Key);
                continue;
            }
        }

        // Pure node without a positioned consumer: use grid position with chain offset
        if (!Positioned.Contains(Pair.Key))
        {
            Pair.Value.PosX = Pair.Value.Column * HORIZONTAL_SPACING;
            Pair.Value.PosY = Pair.Value.Row * VERTICAL_SPACING + ChainYOffset;
            Positioned.Add(Pair.Key);
        }
    }
}
