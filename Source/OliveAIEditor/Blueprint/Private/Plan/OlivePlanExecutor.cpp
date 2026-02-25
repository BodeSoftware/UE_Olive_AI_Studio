// Copyright Bode Software. All Rights Reserved.

/**
 * OlivePlanExecutor.cpp
 *
 * Multi-phase plan execution engine with post-creation pin introspection.
 * Creates all nodes first, builds pin manifests from the real UEdGraphNode pins,
 * then wires exec connections, data connections, and defaults using ground-truth
 * pin names from the manifests.
 *
 * See OlivePlanExecutor.h for phase documentation and public API.
 */

#include "Plan/OlivePlanExecutor.h"
#include "Plan/OlivePinManifest.h"
#include "Plan/OliveBlueprintPlanResolver.h"
#include "Plan/OliveGraphLayoutEngine.h"
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
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Services/OliveBatchExecutionScope.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

DEFINE_LOG_CATEGORY(LogOlivePlanExecutor);

// ============================================================================
// FOlivePlanExecutionContext Lookup Methods
// ============================================================================

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

FString FOlivePlanExecutionContext::FindStepIdForNode(const UEdGraphNode* Node) const
{
    if (!Node)
    {
        return FString();
    }
    for (const auto& Pair : StepToNodePtr)
    {
        if (Pair.Value == Node)
        {
            return Pair.Key;
        }
    }
    return FString();
}

// ============================================================================
// FOlivePlanExecutor::Execute -- Entry Point
// ============================================================================

FOliveIRBlueprintPlanResult FOlivePlanExecutor::Execute(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& AssetPath,
    const FString& GraphName)
{
    const double StartTime = FPlatformTime::Seconds();

    UE_LOG(LogOlivePlanExecutor, Log,
        TEXT("Plan execution starting: %d steps, schema_version='%s', asset='%s', graph='%s'"),
        Plan.Steps.Num(), *Plan.SchemaVersion, *AssetPath, *GraphName);

    // Initialize execution context
    FOlivePlanExecutionContext Context;
    Context.Blueprint = Blueprint;
    Context.Graph = Graph;
    Context.AssetPath = AssetPath;
    Context.GraphName = GraphName;

    // Phase 1: Create all nodes + build pin manifests (FAIL-FAST)
    UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 1: Create Nodes"));
    const bool bNodesCreated = PhaseCreateNodes(Plan, ResolvedSteps, Context);

    if (!bNodesCreated)
    {
        UE_LOG(LogOlivePlanExecutor, Error,
            TEXT("Phase 1 FAILED: Node creation aborted. %d of %d nodes created before failure."),
            Context.CreatedNodeCount, Plan.Steps.Num());

        const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Plan execution failed after %.1f ms"), ElapsedMs);

        return AssembleResult(Plan, Context);
    }

    UE_LOG(LogOlivePlanExecutor, Log,
        TEXT("Phase 1 complete: %d nodes created"), Context.CreatedNodeCount);

    // Initialize reverse map and plan pointer for Phase 5.5
    for (const auto& Pair : Context.StepToNodeMap)
    {
        Context.NodeIdToStepId.Add(Pair.Value, Pair.Key);
    }
    Context.Plan = &Plan;

    // Phase 1.5: Auto-wire component function targets (CONTINUE-ON-FAILURE)
    UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 1.5: Auto-Wire Component Targets"));
    PhaseAutoWireComponentTargets(Plan, ResolvedSteps, Context);

    if (Context.AutoFixCount > 0)
    {
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 1.5 complete: %d component targets auto-wired"),
            Context.AutoFixCount);
    }
    else
    {
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 1.5 complete: no auto-wiring needed"));
    }

    // Capture Phase 1.5 fix count for Phase 5.5 delta logging
    const int32 Phase15Fixes = Context.AutoFixCount;

    // Phases 3-5.5: Wiring + defaults + pre-compile validation.
    // Wrap in FOliveBatchExecutionScope to suppress nested transactions from
    // FOlivePinConnector::Connect(). The caller's write pipeline owns the
    // outer transaction; inner transactions would create undo noise.
    {
        FOliveBatchExecutionScope BatchScope;

        // Phase 3: Wire exec connections (CONTINUE-ON-FAILURE)
        UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 3: Wire Exec Connections"));
        PhaseWireExec(Plan, Context);

        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 3 complete: %d exec connections succeeded, %d failed"),
            Context.SuccessfulConnectionCount, Context.FailedConnectionCount);

        // Phase 4: Wire data connections (CONTINUE-ON-FAILURE)
        // Store the exec-only counts before Phase 4 adds to them
        const int32 ExecConnectionsSucceeded = Context.SuccessfulConnectionCount;
        const int32 ExecConnectionsFailed = Context.FailedConnectionCount;

        UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 4: Wire Data Connections"));
        PhaseWireData(Plan, Context);

        const int32 DataConnectionsSucceeded = Context.SuccessfulConnectionCount - ExecConnectionsSucceeded;
        const int32 DataConnectionsFailed = Context.FailedConnectionCount - ExecConnectionsFailed;

        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 4 complete: %d data connections succeeded, %d failed"),
            DataConnectionsSucceeded, DataConnectionsFailed);

        // Phase 5: Set pin defaults (CONTINUE-ON-FAILURE)
        UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 5: Set Pin Defaults"));
        PhaseSetDefaults(Plan, Context);

        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 5 complete: %d defaults set, %d failed"),
            Context.SuccessfulDefaultCount, Context.FailedDefaultCount);

        // Phase 5.5: Pre-compile validation with auto-fix (CONTINUE-ON-FAILURE)
        UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 5.5: Pre-Compile Validation"));
        PhasePreCompileValidation(Context);

        const int32 Phase55Fixes = Context.AutoFixCount - Phase15Fixes;
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 5.5 complete: %d auto-fixes, %d unfixable issues"),
            Phase55Fixes, Context.PreCompileIssues.Num());
    } // BatchScope destructor restores previous batch state

    // Forward pre-compile issues to warnings so they appear in the tool result
    for (const FString& Issue : Context.PreCompileIssues)
    {
        Context.Warnings.Add(Issue);
    }

    // Phase 6: Auto-layout (ALWAYS SUCCEEDS)
    UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 6: Auto-Layout"));
    PhaseAutoLayout(Plan, Context);

    UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 6 complete: layout applied"));

    // Assemble and return result
    const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
    UE_LOG(LogOlivePlanExecutor, Log,
        TEXT("Plan execution complete in %.1f ms: %d nodes, %d connections (%d failed), %d defaults (%d failed), %d warnings"),
        ElapsedMs,
        Context.CreatedNodeCount,
        Context.SuccessfulConnectionCount, Context.FailedConnectionCount,
        Context.SuccessfulDefaultCount, Context.FailedDefaultCount,
        Context.Warnings.Num());

    return AssembleResult(Plan, Context);
}

// ============================================================================
// Phase 1: Create Nodes (FAIL-FAST)
// ============================================================================

bool FOlivePlanExecutor::PhaseCreateNodes(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    FOlivePlanExecutionContext& Context)
{
    FOliveGraphWriter& Writer = FOliveGraphWriter::Get();

    // Build StepId -> original plan step lookup for event reuse checks
    TMap<FString, const FOliveIRBlueprintPlanStep*> PlanStepLookup;
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        PlanStepLookup.Add(Step.StepId, &Step);
    }

    // Track which steps reuse pre-existing event nodes (must NOT be removed on cleanup)
    TSet<FString> ReusedStepIds;

    // Cleanup lambda: removes all nodes created so far if Phase 1 aborts mid-way.
    // This prevents orphan nodes from surviving in the graph when a retry creates
    // a fresh set.  Runs inside the write pipeline's FScopedTransaction as
    // defense-in-depth: if the transaction rollback works, these removals are also
    // rolled back (harmless); if it doesn't, the explicit cleanup catches orphans.
    auto CleanupCreatedNodes = [&Context, &ReusedStepIds]()
    {
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 1 cleanup: removing %d orphan nodes from graph (skipping %d reused)"),
            Context.StepToNodePtr.Num() - ReusedStepIds.Num(),
            ReusedStepIds.Num());

        for (const auto& Pair : Context.StepToNodePtr)
        {
            if (ReusedStepIds.Contains(Pair.Key))
            {
                continue; // Do not remove pre-existing event nodes
            }
            if (Pair.Value && Context.Graph)
            {
                Context.Graph->RemoveNode(Pair.Value);
            }
        }
        Context.StepToNodeMap.Empty();
        Context.StepToNodePtr.Empty();
        Context.StepManifests.Empty();
        Context.CreatedNodeCount = 0;
    };

    for (int32 i = 0; i < ResolvedSteps.Num(); ++i)
    {
        const FOliveResolvedStep& Resolved = ResolvedSteps[i];
        const FString& StepId = Resolved.StepId;
        const FString& NodeType = Resolved.NodeType;

        // Log step entry
        FString TargetDesc;
        if (const FString* FnName = Resolved.Properties.Find(TEXT("function_name")))
            TargetDesc = *FnName;
        else if (const FString* EvName = Resolved.Properties.Find(TEXT("event_name")))
            TargetDesc = *EvName;
        else if (const FString* VarName = Resolved.Properties.Find(TEXT("variable_name")))
            TargetDesc = *VarName;
        else if (const FString* ClassN = Resolved.Properties.Find(TEXT("actor_class")))
            TargetDesc = *ClassN;
        else
            TargetDesc = TEXT("(none)");

        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("  Step %d/%d: step_id='%s', type='%s', target='%s'"),
            i + 1, ResolvedSteps.Num(), *StepId, *NodeType, *TargetDesc);

        // ----------------------------------------------------------------
        // Event reuse check
        // ----------------------------------------------------------------
        const bool bIsEventOp = (NodeType == OliveNodeTypes::Event);
        const bool bIsCustomEventOp = (NodeType == OliveNodeTypes::CustomEvent);

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
                    const FString ReuseNodeId = ExistingNode->NodeGuid.ToString();

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

                    ReusedStepIds.Add(StepId);
                    continue; // Skip to next step
                }
            }
        }

        // ----------------------------------------------------------------
        // Normal node creation via GraphWriter.AddNode()
        // ----------------------------------------------------------------
        // Temporary position; Phase 6 overrides with layout
        const int32 PosX = i * 300;
        const int32 PosY = 0;

        FOliveBlueprintWriteResult WriteResult = Writer.AddNode(
            Context.AssetPath,
            Context.GraphName,
            NodeType,
            Resolved.Properties,
            PosX,
            PosY);

        if (!WriteResult.bSuccess)
        {
            // FAIL-FAST: abort entire plan if any node creation fails
            const FString ErrorMsg = WriteResult.Errors.Num() > 0
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

            CleanupCreatedNodes();
            return false;
        }

        // Retrieve UEdGraphNode* from GraphWriter cache
        const FString NodeId = WriteResult.CreatedNodeId;
        UEdGraphNode* NodePtr = Writer.GetCachedNode(Context.AssetPath, NodeId);

        if (!NodePtr)
        {
            // Should never happen after successful AddNode, but guard defensively
            UE_LOG(LogOlivePlanExecutor, Error,
                TEXT("Phase 1 FAIL: Step '%s' created (id: %s) but not in cache"),
                *StepId, *NodeId);

            Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                TEXT("NODE_CREATION_FAILED"),
                StepId,
                FString::Printf(TEXT("/steps/%d"), i),
                TEXT("Node created but not retrievable from cache"),
                TEXT("Internal error -- retry the plan")));

            CleanupCreatedNodes();
            return false;
        }

        // Build pin manifest from the real node (this is the key insight --
        // we introspect the actual pins after node creation for ground-truth data)
        FOlivePinManifest Manifest = FOlivePinManifest::Build(
            NodePtr, StepId, NodeId, NodeType);

        // Store in context
        Context.StepManifests.Add(StepId, MoveTemp(Manifest));
        Context.StepToNodeMap.Add(StepId, NodeId);
        Context.StepToNodePtr.Add(StepId, NodePtr);
        Context.CreatedNodeCount++;

        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("  -> Created step '%s' -> node '%s': %d pins (%s)"),
            *StepId, *NodeId,
            Context.StepManifests[StepId].Pins.Num(),
            Context.StepManifests[StepId].bIsPure ? TEXT("pure") : TEXT("exec"));
    }

    return true;
}

// ============================================================================
// FindExistingEventNode
// ============================================================================

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
        // Search for UK2Node_Event override matching the event name.
        // Use FBlueprintEditorUtils which is the same check as OliveNodeFactory.
        if (Blueprint->ParentClass)
        {
            const FName EventFName(*EventName);
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

// ============================================================================
// Phase 1.5: Auto-Wire Component Targets
// ============================================================================

void FOlivePlanExecutor::PhaseAutoWireComponentTargets(
    const FOliveIRBlueprintPlan& Plan,
    const TArray<FOliveResolvedStep>& ResolvedSteps,
    FOlivePlanExecutionContext& Context)
{
    UBlueprint* BP = Context.Blueprint;
    if (!BP || !BP->SimpleConstructionScript)
    {
        return;
    }

    // Skip if Blueprint itself is a component (self IS the component)
    if (BP->ParentClass && BP->ParentClass->IsChildOf(UActorComponent::StaticClass()))
    {
        return;
    }

    for (int32 i = 0; i < ResolvedSteps.Num(); ++i)
    {
        const FOliveResolvedStep& Resolved = ResolvedSteps[i];

        // Only care about call ops with a resolved owning class that is a component
        if (!Resolved.ResolvedOwningClass ||
            !Resolved.ResolvedOwningClass->IsChildOf(UActorComponent::StaticClass()))
        {
            continue;
        }

        // Check if AI already provided a Target input as @ref
        if (i < Plan.Steps.Num())
        {
            const FOliveIRBlueprintPlanStep& PlanStep = Plan.Steps[i];
            const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
            if (TargetValue && TargetValue->StartsWith(TEXT("@")))
            {
                continue; // AI provided a target reference -- don't override
            }
        }

        // Get the created node
        UEdGraphNode* CreatedNode = Context.GetNodePtr(Resolved.StepId);
        if (!CreatedNode)
        {
            continue;
        }

        // Find the self/Target pin
        UEdGraphPin* SelfPin = CreatedNode->FindPin(UEdGraphSchema_K2::PN_Self);
        if (!SelfPin || SelfPin->LinkedTo.Num() > 0)
        {
            continue; // Already wired or no self pin
        }

        // Find matching SCS components
        UClass* RequiredClass = Resolved.ResolvedOwningClass;
        TArray<USCS_Node*> MatchingNodes;
        for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
        {
            if (SCSNode && SCSNode->ComponentClass &&
                SCSNode->ComponentClass->IsChildOf(RequiredClass))
            {
                MatchingNodes.Add(SCSNode);
            }
        }

        if (MatchingNodes.Num() == 0)
        {
            // No matching components -- Phase 5.5 or compile will catch this
            continue;
        }

        if (MatchingNodes.Num() > 1)
        {
            Context.Warnings.Add(FString::Printf(
                TEXT("Step '%s': %d components match type '%s'. "
                     "Wire Target explicitly to disambiguate."),
                *Resolved.StepId, MatchingNodes.Num(),
                *RequiredClass->GetName()));
            continue;
        }

        // Exactly one match -- inject a VariableGet node and wire it
        USCS_Node* MatchedSCS = MatchingNodes[0];
        FName ComponentVarName = MatchedSCS->GetVariableName();

        UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Context.Graph);
        GetNode->VariableReference.SetSelfMember(ComponentVarName);
        Context.Graph->AddNode(GetNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
        GetNode->AllocateDefaultPins();

        // Find the first non-exec output pin on the getter (the component reference)
        UEdGraphPin* GetOutputPin = nullptr;
        for (UEdGraphPin* Pin : GetNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output &&
                Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                GetOutputPin = Pin;
                break;
            }
        }

        if (!GetOutputPin)
        {
            Context.Warnings.Add(FString::Printf(
                TEXT("Step '%s': Auto-wire failed -- VariableGet for '%s' has no output pin"),
                *Resolved.StepId, *ComponentVarName.ToString()));
            // Remove the orphan getter node we just created
            Context.Graph->RemoveNode(GetNode);
            continue;
        }

        // Wire the getter output to the self pin
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        if (Schema->TryCreateConnection(GetOutputPin, SelfPin))
        {
            Context.SuccessfulConnectionCount++;
            Context.AutoFixCount++;

            Context.Warnings.Add(FString::Printf(
                TEXT("Step '%s': Auto-wired Target <- component '%s' (%s)"),
                *Resolved.StepId, *ComponentVarName.ToString(),
                *RequiredClass->GetName()));

            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("Phase 1.5: Auto-wired '%s' Target <- component '%s' (%s)"),
                *Resolved.StepId, *ComponentVarName.ToString(),
                *RequiredClass->GetName());
        }
        else
        {
            Context.Warnings.Add(FString::Printf(
                TEXT("Step '%s': Auto-wire Target failed for component '%s'"),
                *Resolved.StepId, *ComponentVarName.ToString()));
            // Remove the orphan getter node
            Context.Graph->RemoveNode(GetNode);
        }
    }
}

// ============================================================================
// Phase 3: Wire Exec Connections (CONTINUE-ON-FAILURE)
// ============================================================================

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
            // Self-loop guard: skip if exec_after references the same step
            if (Step.ExecAfter == Step.StepId)
            {
                UE_LOG(LogOlivePlanExecutor, Warning,
                    TEXT("  Skipping self-loop on step '%s' (exec_after references self)"),
                    *Step.StepId);
                Context.Warnings.Add(FString::Printf(
                    TEXT("Step '%s': exec_after references itself (self-loop skipped)"),
                    *Step.StepId));
                // Do not attempt wiring, do not increment FailedConnectionCount
            }
            else
            {
                UE_LOG(LogOlivePlanExecutor, Log,
                    TEXT("  Exec wire: '%s' -> '%s' (exec_after)"),
                    *Step.ExecAfter, *Step.StepId);

                FOliveSmartWireResult Result = WireExecConnection(
                    Step.ExecAfter,     // source step
                    FString(),          // empty hint = primary exec output
                    Step.StepId,        // target step
                    Context);

                if (Result.bSuccess)
                {
                    UE_LOG(LogOlivePlanExecutor, Log, TEXT("    -> OK"));
                    Context.SuccessfulConnectionCount++;
                }
                else
                {
                    UE_LOG(LogOlivePlanExecutor, Warning,
                        TEXT("    -> FAILED: %s"), *Result.ErrorMessage);
                    Context.FailedConnectionCount++;
                    Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                        TEXT("EXEC_PIN_NOT_FOUND"),
                        Step.StepId,
                        TEXT("/steps/exec_after"),
                        Result.ErrorMessage,
                        Result.Suggestions.Num() > 0
                            ? FString::Printf(TEXT("Available exec pins: %s"),
                                *FString::Join(Result.Suggestions, TEXT(", ")))
                            : TEXT("")));
                }
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

            // Self-loop guard: skip if exec output targets the same step
            if (TargetStepId == Step.StepId)
            {
                UE_LOG(LogOlivePlanExecutor, Warning,
                    TEXT("  Skipping self-loop on step '%s' (exec_output '%s' -> self)"),
                    *Step.StepId, *PinHint);
                Context.Warnings.Add(FString::Printf(
                    TEXT("Step '%s': exec_output '%s' targets itself (self-loop skipped)"),
                    *Step.StepId, *PinHint));
                continue;
            }

            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("  Exec wire: '%s'.%s -> '%s' (exec_output)"),
                *Step.StepId, *PinHint, *TargetStepId);

            FOliveSmartWireResult Result = WireExecConnection(
                Step.StepId,        // source step
                PinHint,            // named exec output
                TargetStepId,       // target step
                Context);

            if (Result.bSuccess)
            {
                UE_LOG(LogOlivePlanExecutor, Log, TEXT("    -> OK"));
                Context.SuccessfulConnectionCount++;
            }
            else
            {
                UE_LOG(LogOlivePlanExecutor, Warning,
                    TEXT("    -> FAILED: %s"), *Result.ErrorMessage);
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

    // ----------------------------------------------------------------
    // Build the set of step IDs that are already targeted by another
    // step's exec_after or exec_outputs.  These are NOT orphans.
    // Hoisted here so both function-entry and event auto-chain can use it.
    // ----------------------------------------------------------------
    TSet<FString> TargetedStepIds;
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        // exec_outputs values are target step IDs
        for (const auto& ExecOut : Step.ExecOutputs)
        {
            TargetedStepIds.Add(ExecOut.Value);
        }
    }
    // Steps that have exec_after set already have an incoming exec wire
    // from their declared predecessor.
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        if (!Step.ExecAfter.IsEmpty())
        {
            TargetedStepIds.Add(Step.StepId);
        }
    }

    // ----------------------------------------------------------------
    // Auto-chain: Wire function entry node to first impure orphan step.
    //
    // In function graphs, the UK2Node_FunctionEntry "then" pin must be
    // wired to the first executable step.  When the plan omits explicit
    // exec_after for the root step, the entry node is left dangling.
    // This block detects that case and auto-wires it.
    // ----------------------------------------------------------------
    UK2Node_FunctionEntry* EntryNode = nullptr;
    for (UEdGraphNode* Node : Context.Graph->Nodes)
    {
        EntryNode = Cast<UK2Node_FunctionEntry>(Node);
        if (EntryNode)
        {
            break;
        }
    }

    UE_LOG(LogOlivePlanExecutor, Log, TEXT("Auto-chain: FunctionEntry node %s"),
        EntryNode ? TEXT("found") : TEXT("not found - skipping"));

    if (EntryNode)
    {
        // Find the first impure orphan step:
        //   - Has no exec_after
        //   - Is not targeted by any other step's exec_outputs
        //   - Is impure (has an exec input pin on its created node)
        const FOliveIRBlueprintPlanStep* OrphanStep = nullptr;
        for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
        {
            if (!Step.ExecAfter.IsEmpty())
            {
                continue;
            }
            if (TargetedStepIds.Contains(Step.StepId))
            {
                continue;
            }

            // Check that this step's node is impure (has exec input)
            const FOlivePinManifest* Manifest = Context.GetManifest(Step.StepId);
            if (!Manifest || Manifest->bIsPure)
            {
                continue;
            }

            // Skip event/custom_event nodes -- they are entry points themselves
            if (Step.Op == OlivePlanOps::Event || Step.Op == OlivePlanOps::CustomEvent)
            {
                continue;
            }

            OrphanStep = &Step;
            break;
        }

        if (OrphanStep)
        {
            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("Auto-chain: orphan step '%s' found, wiring from function entry"),
                *OrphanStep->StepId);

            // Wire EntryNode's "then" exec output -> OrphanStep's exec input
            UEdGraphPin* EntryExecOut = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
            if (!EntryExecOut)
            {
                // Fallback: search for any exec output pin on the entry node
                for (UEdGraphPin* Pin : EntryNode->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Output &&
                        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        EntryExecOut = Pin;
                        break;
                    }
                }
            }

            UEdGraphNode* OrphanNode = Context.GetNodePtr(OrphanStep->StepId);
            const FOlivePinManifest* OrphanManifest = Context.GetManifest(OrphanStep->StepId);
            const FOlivePinManifestEntry* OrphanExecIn = OrphanManifest ? OrphanManifest->FindExecInput() : nullptr;
            UEdGraphPin* OrphanExecInPin = nullptr;
            if (OrphanNode && OrphanExecIn)
            {
                OrphanExecInPin = OrphanNode->FindPin(FName(*OrphanExecIn->PinName));
            }

            if (EntryExecOut && OrphanExecInPin)
            {
                // Check the entry pin is not already connected (don't override existing wires)
                if (EntryExecOut->LinkedTo.Num() == 0)
                {
                    FOlivePinConnector& Connector = FOlivePinConnector::Get();
                    FOliveBlueprintWriteResult ConnectResult = Connector.Connect(
                        EntryExecOut, OrphanExecInPin, false);

                    if (ConnectResult.bSuccess)
                    {
                        Context.SuccessfulConnectionCount++;
                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("Auto-chained function entry 'then' -> step '%s' exec input"),
                            *OrphanStep->StepId);
                    }
                    else
                    {
                        Context.FailedConnectionCount++;
                        const FString ErrorMsg = ConnectResult.Errors.Num() > 0
                            ? ConnectResult.Errors[0]
                            : TEXT("Unknown connection error");
                        UE_LOG(LogOlivePlanExecutor, Warning,
                            TEXT("Failed to auto-chain function entry to step '%s': %s"),
                            *OrphanStep->StepId, *ErrorMsg);

                        Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakePlanError(
                            TEXT("ENTRY_AUTOCHAIN_FAILED"),
                            FString::Printf(
                                TEXT("Failed to wire function entry to first orphan step '%s': %s"),
                                *OrphanStep->StepId, *ErrorMsg),
                            TEXT("Add explicit exec_after to the first step in the function")));
                    }
                }
                else
                {
                    UE_LOG(LogOlivePlanExecutor, Verbose,
                        TEXT("Function entry 'then' pin already connected, skipping auto-chain"));
                }
            }
            else
            {
                UE_LOG(LogOlivePlanExecutor, Warning,
                    TEXT("Auto-chain: Could not resolve pins for entry -> step '%s' wiring"),
                    *OrphanStep->StepId);
            }
        }
        else
        {
            UE_LOG(LogOlivePlanExecutor, Verbose,
                TEXT("Auto-chain: no orphan steps found -- all steps have explicit exec wiring or are pure/events"));
        }
    }

    // ----------------------------------------------------------------
    // Auto-chain: Wire event nodes to their first impure follower step.
    //
    // In event graphs (EventGraph), events like BeginPlay or
    // custom_event nodes are entry points that need their exec output
    // wired to the first impure step that follows them in plan order.
    // When the AI omits exec_after on the follower (as the prompt
    // tells it to), the event is left dangling.  This mirrors the
    // function-entry auto-chain above but works per-event.
    // ----------------------------------------------------------------
    for (int32 EventIdx = 0; EventIdx < Plan.Steps.Num(); ++EventIdx)
    {
        const FOliveIRBlueprintPlanStep& EventStep = Plan.Steps[EventIdx];

        // Only process event and custom_event ops
        if (EventStep.Op != OlivePlanOps::Event && EventStep.Op != OlivePlanOps::CustomEvent)
        {
            continue;
        }

        // Skip if this event already has outgoing exec wires declared in
        // exec_outputs (the AI explicitly wired it)
        if (EventStep.ExecOutputs.Num() > 0)
        {
            continue;
        }

        // Skip if the event node's exec output pin is already connected
        // (e.g., by a prior step's exec_after pointing at this event's
        // follower, or by reuse of an existing event node)
        UEdGraphNode* EventNode = Context.GetNodePtr(EventStep.StepId);
        const FOlivePinManifest* EventManifest = Context.GetManifest(EventStep.StepId);
        if (!EventNode || !EventManifest)
        {
            continue;
        }

        const FOlivePinManifestEntry* EventExecOutEntry = EventManifest->FindExecOutput();
        if (!EventExecOutEntry)
        {
            continue;
        }

        UEdGraphPin* EventExecOutPin = EventNode->FindPin(FName(*EventExecOutEntry->PinName));
        if (!EventExecOutPin)
        {
            // Fallback: search for any exec output pin on the event node
            for (UEdGraphPin* Pin : EventNode->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output &&
                    Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    EventExecOutPin = Pin;
                    break;
                }
            }
        }

        if (!EventExecOutPin || EventExecOutPin->LinkedTo.Num() > 0)
        {
            // Already connected or no exec output -- skip
            continue;
        }

        // Walk forward from EventIdx+1 looking for the first impure orphan
        // step that belongs to this event's chain (stop at the next event boundary)
        const FOliveIRBlueprintPlanStep* FollowerStep = nullptr;
        for (int32 FollowerIdx = EventIdx + 1; FollowerIdx < Plan.Steps.Num(); ++FollowerIdx)
        {
            const FOliveIRBlueprintPlanStep& Candidate = Plan.Steps[FollowerIdx];

            // Stop scanning at the next event/custom_event -- that starts a new chain
            if (Candidate.Op == OlivePlanOps::Event || Candidate.Op == OlivePlanOps::CustomEvent)
            {
                break;
            }

            // Must be an orphan (no exec_after, not targeted by other step)
            if (!Candidate.ExecAfter.IsEmpty())
            {
                continue;
            }
            if (TargetedStepIds.Contains(Candidate.StepId))
            {
                continue;
            }

            // Must be impure (has exec input)
            const FOlivePinManifest* CandManifest = Context.GetManifest(Candidate.StepId);
            if (!CandManifest || CandManifest->bIsPure)
            {
                continue;
            }

            FollowerStep = &Candidate;
            break;
        }

        if (!FollowerStep)
        {
            UE_LOG(LogOlivePlanExecutor, Verbose,
                TEXT("Auto-chain: event '%s' has no impure orphan follower -- skipping"),
                *EventStep.StepId);
            continue;
        }

        // Resolve the follower's exec input pin
        UEdGraphNode* FollowerNode = Context.GetNodePtr(FollowerStep->StepId);
        const FOlivePinManifest* FollowerManifest = Context.GetManifest(FollowerStep->StepId);
        const FOlivePinManifestEntry* FollowerExecIn = FollowerManifest ? FollowerManifest->FindExecInput() : nullptr;
        UEdGraphPin* FollowerExecInPin = nullptr;
        if (FollowerNode && FollowerExecIn)
        {
            FollowerExecInPin = FollowerNode->FindPin(FName(*FollowerExecIn->PinName));
        }

        if (!FollowerExecInPin)
        {
            UE_LOG(LogOlivePlanExecutor, Warning,
                TEXT("Auto-chain: Could not resolve exec input pin for follower step '%s' after event '%s'"),
                *FollowerStep->StepId, *EventStep.StepId);
            continue;
        }

        // Wire event exec output -> follower exec input
        FOlivePinConnector& Connector = FOlivePinConnector::Get();
        FOliveBlueprintWriteResult ConnectResult = Connector.Connect(
            EventExecOutPin, FollowerExecInPin, false);

        if (ConnectResult.bSuccess)
        {
            Context.SuccessfulConnectionCount++;
            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("Auto-chained event '%s' exec output -> step '%s' exec input"),
                *EventStep.StepId, *FollowerStep->StepId);
        }
        else
        {
            Context.FailedConnectionCount++;
            const FString ErrorMsg = ConnectResult.Errors.Num() > 0
                ? ConnectResult.Errors[0]
                : TEXT("Unknown connection error");
            UE_LOG(LogOlivePlanExecutor, Warning,
                TEXT("Failed to auto-chain event '%s' to follower step '%s': %s"),
                *EventStep.StepId, *FollowerStep->StepId, *ErrorMsg);

            Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakePlanError(
                TEXT("EVENT_AUTOCHAIN_FAILED"),
                FString::Printf(
                    TEXT("Failed to wire event '%s' to first follower step '%s': %s"),
                    *EventStep.StepId, *FollowerStep->StepId, *ErrorMsg),
                TEXT("Add explicit exec_after on the first step following the event")));
        }
    }
}

// ============================================================================
// WireExecConnection Helper
// ============================================================================

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
        // Specific pin requested -- search among exec output pins
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
        // Build suggestions from all available exec outputs
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

// ============================================================================
// Phase 4: Wire Data Connections (CONTINUE-ON-FAILURE)
// ============================================================================

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

// ============================================================================
// WireDataConnection Helper
// ============================================================================

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
        UE_LOG(LogOlivePlanExecutor, Warning,
            TEXT("  Data wire FAILED: %s"), *Result.ErrorMessage);
        return Result;
    }

    UE_LOG(LogOlivePlanExecutor, Verbose,
        TEXT("  Data wire: step '%s'.%s <- @%s.%s"),
        *TargetStepId, *TargetPinHint, *SourceStepId, *SourcePinHint);

    // 2. Get manifests
    const FOlivePinManifest* SourceManifest = Context.GetManifest(SourceStepId);
    const FOlivePinManifest* TargetManifest = Context.GetManifest(TargetStepId);

    if (!SourceManifest)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Source step '%s' not found (referenced by %s)"), *SourceStepId, *SourceRef);
        UE_LOG(LogOlivePlanExecutor, Warning,
            TEXT("  Data wire FAILED: %s"), *Result.ErrorMessage);
        return Result;
    }
    if (!TargetManifest)
    {
        Result.ErrorMessage = FString::Printf(
            TEXT("Target step '%s' not found"), *TargetStepId);
        UE_LOG(LogOlivePlanExecutor, Warning,
            TEXT("  Data wire FAILED: %s"), *Result.ErrorMessage);
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
        UE_LOG(LogOlivePlanExecutor, Warning,
            TEXT("  Data wire FAILED: %s. Available: %s"),
            *Result.ErrorMessage,
            Result.Suggestions.Num() > 0
                ? *FString::Join(Result.Suggestions, TEXT(", "))
                : TEXT("(none)"));
        return Result;
    }

    UE_LOG(LogOlivePlanExecutor, Verbose,
        TEXT("    Target pin resolved: '%s' via %s"),
        *TargetPin->PinName, *TargetMatchMethod);

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
            UE_LOG(LogOlivePlanExecutor, Warning,
                TEXT("  Data wire FAILED: %s. Available: %s"),
                *Result.ErrorMessage,
                Result.Suggestions.Num() > 0
                    ? *FString::Join(Result.Suggestions, TEXT(", "))
                    : TEXT("(none)"));
            return Result;
        }
    }
    else if (SourcePinHint.StartsWith(TEXT("~")))
    {
        // FUZZY MATCH with ~ prefix
        const FString FuzzyHint = SourcePinHint.Mid(1);
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
        UE_LOG(LogOlivePlanExecutor, Warning,
            TEXT("  Data wire FAILED: %s. Available: %s"),
            *Result.ErrorMessage,
            Result.Suggestions.Num() > 0
                ? *FString::Join(Result.Suggestions, TEXT(", "))
                : TEXT("(none)"));
        return Result;
    }

    UE_LOG(LogOlivePlanExecutor, Verbose,
        TEXT("    Source pin resolved: '%s' via %s"),
        *SourcePin->PinName, *SourceMatchMethod);

    // 5. Make the connection using direct UEdGraphPin* access
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
    FOliveBlueprintWriteResult ConnectResult = Connector.Connect(RealSourcePin, RealTargetPin, /*bAllowConversion=*/true);

    if (ConnectResult.bSuccess)
    {
        Result.bSuccess = true;
        Result.ResolvedSourcePin = SourcePin->PinName;
        Result.ResolvedTargetPin = TargetPin->PinName;
        Result.SourceMatchMethod = SourceMatchMethod;
        Result.TargetMatchMethod = TargetMatchMethod;

        // Check if auto-conversion inserted a node.
        // When PinConnector inserts a conversion node, the source pin is
        // connected to the conversion node's input (not the original target).
        //
        // ASSUMPTION: After PinConnector::Connect with conversion, the conversion node's
        // input pin is the first (and typically only new) entry in LinkedTo. This holds
        // because InsertConversionNode connects Source->ConversionInput then
        // ConversionOutput->Target, so Source.LinkedTo[0] is the conversion input pin.
        // If InsertConversionNode ever changes link ordering, this detection will break.
        // Verify if modifying PinConnector.
        const bool bConversionInserted = (RealSourcePin->LinkedTo.Num() > 0 &&
            RealSourcePin->LinkedTo[0] != RealTargetPin);

        if (bConversionInserted)
        {
            FOliveConversionNote Note;
            Note.SourceStep = SourceStepId;
            Note.TargetStep = TargetStepId;
            Note.SourcePinName = SourcePin->PinName;
            Note.TargetPinName = TargetPin->PinName;
            Note.FromType = SourcePin->TypeDisplayString;
            Note.ToType = TargetPin->TypeDisplayString;

            // The conversion node is the intermediate between source and target.
            // RealSourcePin->LinkedTo[0] is the conversion node's input pin.
            UEdGraphNode* ConvNode = RealSourcePin->LinkedTo[0]->GetOwningNode();
            if (ConvNode)
            {
                Note.ConversionNodeType = ConvNode->GetClass()->GetName();
            }

            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("Auto-conversion: %s.%s (%s) -> %s.%s (%s) via %s"),
                *SourceStepId, *Note.SourcePinName, *Note.FromType,
                *TargetStepId, *Note.TargetPinName, *Note.ToType,
                *Note.ConversionNodeType);

            Context.Warnings.Add(FString::Printf(
                TEXT("Auto-conversion inserted between %s.%s and %s.%s: %s -> %s (via %s)"),
                *SourceStepId, *Note.SourcePinName,
                *TargetStepId, *Note.TargetPinName,
                *Note.FromType, *Note.ToType,
                *Note.ConversionNodeType));

            Context.ConversionNotes.Add(MoveTemp(Note));
        }

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

// ============================================================================
// FindTypeCompatibleOutput
// ============================================================================

const FOlivePinManifestEntry* FOlivePlanExecutor::FindTypeCompatibleOutput(
    const FOlivePinManifest& SourceManifest,
    EOliveIRTypeCategory TargetTypeCategory,
    const FString& TargetSubCategory)
{
    TArray<const FOlivePinManifestEntry*> DataOutputs = SourceManifest.GetDataPins(false);

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
                return Pin;
            }
        }
        // Return first match; caller logs ambiguity warning if needed
        UE_LOG(LogOlivePlanExecutor, Warning,
            TEXT("Type auto-match: %d output pins match type, using first ('%s')"),
            TypeMatches.Num(), *TypeMatches[0]->PinName);
        return TypeMatches[0];
    }

    // No matches
    return nullptr;
}

// ============================================================================
// ParseDataRef
// ============================================================================

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

    const FString Body = Ref.Mid(1); // Strip "@"

    int32 DotIndex;
    if (!Body.FindChar(TEXT('.'), DotIndex))
    {
        return false;
    }

    OutStepId = Body.Left(DotIndex);
    OutPinHint = Body.Mid(DotIndex + 1);

    return !OutStepId.IsEmpty() && !OutPinHint.IsEmpty();
}

// ============================================================================
// Phase 5: Set Pin Defaults (CONTINUE-ON-FAILURE)
// ============================================================================

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

            // For set_var steps, UE names the data input pin after the variable
            // (e.g., "bCanFire"), not "value". Remap the generic "value" key to
            // the actual variable name stored in Step.Target.
            FString ResolvedPinKey = PinKey;
            if (Step.Op == OlivePlanOps::SetVar
                && PinKey.Equals(TEXT("value"), ESearchCase::IgnoreCase)
                && !Step.Target.IsEmpty())
            {
                ResolvedPinKey = Step.Target;
                UE_LOG(LogOlivePlanExecutor, Verbose,
                    TEXT("Step '%s': Remapped generic 'value' pin key to variable name '%s' for set_var"),
                    *Step.StepId, *Step.Target);
            }

            // Resolve the target input pin using the manifest
            FString MatchMethod;
            const FOlivePinManifestEntry* PinEntry = Manifest->FindPinSmart(
                ResolvedPinKey, /*bIsInput=*/true,
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

// ============================================================================
// Phase 5.5: Pre-Compile Validation with Auto-Fix
// ============================================================================

void FOlivePlanExecutor::PhasePreCompileValidation(
    FOlivePlanExecutionContext& Context)
{
    if (!Context.Graph || !Context.Plan)
    {
        return;
    }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

    for (UEdGraphNode* Node : Context.Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }

        UK2Node* K2Node = Cast<UK2Node>(Node);
        if (!K2Node)
        {
            continue;
        }

        // ================================================================
        // Check 1: Orphaned impure nodes -- has exec input, none wired
        // ================================================================
        bool bHasExecInput = false;
        bool bExecInputWired = false;

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->bHidden)
            {
                continue;
            }
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                Pin->Direction == EGPD_Input)
            {
                bHasExecInput = true;
                if (Pin->LinkedTo.Num() > 0)
                {
                    bExecInputWired = true;
                }
            }
        }

        if (bHasExecInput && !bExecInputWired &&
            !K2Node->IsA<UK2Node_Event>() &&
            !K2Node->IsA<UK2Node_CustomEvent>())
        {
            // This node has an exec input that is not wired.
            // Attempt auto-fix: if this node was created by a plan step that
            // has exec_after set, and the source step's exec output is unwired,
            // reconnect them. This catches cases where Phase 3 failed due to
            // pin name drift but the plan intent is clear.
            bool bAutoFixed = false;

            const FString StepId = Context.FindStepIdForNode(Node);
            if (!StepId.IsEmpty())
            {
                // Find the plan step
                for (const FOliveIRBlueprintPlanStep& Step : Context.Plan->Steps)
                {
                    if (Step.StepId != StepId || Step.ExecAfter.IsEmpty())
                    {
                        continue;
                    }

                    // Find the source node
                    UEdGraphNode* SourceNode = Context.GetNodePtr(Step.ExecAfter);
                    if (!SourceNode)
                    {
                        break;
                    }

                    // Find the first unwired exec output on the source node
                    UEdGraphPin* SourceExecOut = nullptr;
                    for (UEdGraphPin* Pin : SourceNode->Pins)
                    {
                        if (Pin && !Pin->bHidden &&
                            Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                            Pin->Direction == EGPD_Output &&
                            Pin->LinkedTo.Num() == 0)
                        {
                            SourceExecOut = Pin;
                            break; // Take first unwired exec output
                        }
                    }

                    if (!SourceExecOut)
                    {
                        break; // Source has no unwired exec output
                    }

                    // Find this node's exec input pin
                    UEdGraphPin* TargetExecIn = Node->FindPin(UEdGraphSchema_K2::PN_Execute);
                    if (!TargetExecIn)
                    {
                        // Fallback: find any unwired exec input pin
                        for (UEdGraphPin* Pin : Node->Pins)
                        {
                            if (Pin && !Pin->bHidden &&
                                Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                                Pin->Direction == EGPD_Input &&
                                Pin->LinkedTo.Num() == 0)
                            {
                                TargetExecIn = Pin;
                                break;
                            }
                        }
                    }

                    if (TargetExecIn && Schema->TryCreateConnection(SourceExecOut, TargetExecIn))
                    {
                        bAutoFixed = true;
                        Context.AutoFixCount++;
                        Context.SuccessfulConnectionCount++;

                        Context.Warnings.Add(FString::Printf(
                            TEXT("Phase 5.5: Auto-fixed orphaned exec on step '%s' "
                                 "<- '%s' (exec_after recovery)"),
                            *StepId, *Step.ExecAfter));

                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("Phase 5.5: Auto-fixed orphaned exec on '%s' <- '%s'"),
                            *StepId, *Step.ExecAfter);
                    }

                    break; // Found the matching step, stop searching
                }
            }

            if (!bAutoFixed)
            {
                FString NodeDesc = FString::Printf(TEXT("%s (%s)"),
                    *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                    *StepId);
                Context.PreCompileIssues.Add(FString::Printf(
                    TEXT("ORPHAN_NODE: '%s' has exec pins but is not connected "
                         "to any execution chain. It will never execute."),
                    *NodeDesc));

                UE_LOG(LogOlivePlanExecutor, Warning,
                    TEXT("Phase 5.5: Orphaned node '%s' could not be auto-fixed"),
                    *NodeDesc);
            }
        }

        // ================================================================
        // Check 2: Unwired Self/Target on component function calls
        // (Defense-in-depth for Phase 1.5 misses)
        // ================================================================
        UK2Node_CallFunction* CallFunc = Cast<UK2Node_CallFunction>(K2Node);
        if (CallFunc)
        {
            UFunction* Func = CallFunc->GetTargetFunction();
            if (Func && !Func->HasAnyFunctionFlags(FUNC_Static))
            {
                UEdGraphPin* SelfPin = CallFunc->FindPin(UEdGraphSchema_K2::PN_Self);
                if (SelfPin && !SelfPin->bHidden &&
                    SelfPin->LinkedTo.Num() == 0 &&
                    SelfPin->DefaultObject == nullptr)
                {
                    UClass* FuncClass = Func->GetOwnerClass();
                    if (FuncClass &&
                        FuncClass->IsChildOf(UActorComponent::StaticClass()))
                    {
                        const FString StepId2 = Context.FindStepIdForNode(Node);
                        FString NodeDesc = FString::Printf(TEXT("%s (step: %s)"),
                            *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                            StepId2.IsEmpty() ? TEXT("unknown") : *StepId2);
                        Context.PreCompileIssues.Add(FString::Printf(
                            TEXT("UNWIRED_TARGET: '%s' calls component function '%s' "
                                 "but Target pin is not wired. Will cause compile error."),
                            *NodeDesc, *Func->GetName()));

                        UE_LOG(LogOlivePlanExecutor, Warning,
                            TEXT("Phase 5.5: Unwired component Target on '%s' (function '%s')"),
                            *NodeDesc, *Func->GetName());
                    }
                }
            }
        }
    }
}

// ============================================================================
// Phase 6: Auto-Layout (ALWAYS SUCCEEDS)
// ============================================================================

void FOlivePlanExecutor::PhaseAutoLayout(
    const FOliveIRBlueprintPlan& Plan,
    FOlivePlanExecutionContext& Context)
{
    TMap<FString, FOliveLayoutEntry> Layout = FOliveGraphLayoutEngine::ComputeLayout(Plan, Context);
    FOliveGraphLayoutEngine::ApplyLayout(Layout, Context);
}

// ============================================================================
// BuildPinSuggestionList
// ============================================================================

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

// ============================================================================
// AssembleResult
// ============================================================================

FOliveIRBlueprintPlanResult FOlivePlanExecutor::AssembleResult(
    const FOliveIRBlueprintPlan& Plan,
    const FOlivePlanExecutionContext& Context)
{
    FOliveIRBlueprintPlanResult Result;

    // Success = all nodes created (Phase 1 didn't fail-fast)
    const bool bAllNodesCreated = (Context.CreatedNodeCount == Plan.Steps.Num());
    const bool bHasWiringErrors = (Context.FailedConnectionCount > 0 || Context.FailedDefaultCount > 0);

    Result.bSuccess = bAllNodesCreated;
    Result.StepToNodeMap = Context.StepToNodeMap;
    Result.AppliedOpsCount = Context.CreatedNodeCount + Context.SuccessfulConnectionCount + Context.SuccessfulDefaultCount;
    Result.Errors = Context.WiringErrors;
    Result.Warnings = Context.Warnings;

    // Populate per-category counters
    Result.ConnectionsSucceeded = Context.SuccessfulConnectionCount;
    Result.ConnectionsFailed = Context.FailedConnectionCount;
    Result.DefaultsSucceeded = Context.SuccessfulDefaultCount;
    Result.DefaultsFailed = Context.FailedDefaultCount;

    // Partial success: all nodes created but some wiring/defaults failed
    Result.bPartial = Result.bSuccess &&
        (Context.FailedConnectionCount > 0 || Context.FailedDefaultCount > 0);

    // Serialize pin manifests for AI self-correction
    for (const auto& ManifestPair : Context.StepManifests)
    {
        Result.PinManifestJsons.Add(ManifestPair.Key, ManifestPair.Value.ToJson());
    }

    // Serialize conversion notes for transparency
    if (Context.ConversionNotes.Num() > 0)
    {
        Result.ConversionNotesJson.Reserve(Context.ConversionNotes.Num());
        for (const FOliveConversionNote& Note : Context.ConversionNotes)
        {
            Result.ConversionNotesJson.Add(Note.ToJson());
        }
    }

    // Add partial success warning
    if (bAllNodesCreated && bHasWiringErrors)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("Partial success: %d nodes created, %d connections failed, %d defaults failed. "
                 "See wiring_errors and pin_manifests for repair instructions."),
            Context.CreatedNodeCount, Context.FailedConnectionCount, Context.FailedDefaultCount));
    }

    // Auto-fix count for transparency
    if (Context.AutoFixCount > 0)
    {
        Result.Warnings.Add(FString::Printf(
            TEXT("Executor auto-fixed %d issue(s) (component target wiring, orphaned exec recovery)."),
            Context.AutoFixCount));
    }

    return Result;
}
