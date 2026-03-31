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
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "InputAction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Self.h"
#include "K2Node_CallFunction.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_MakeContainer.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "UObject/UnrealType.h"
#include "Services/OliveBatchExecutionScope.h"
#include "OliveClassResolver.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

DEFINE_LOG_CATEGORY(LogOlivePlanExecutor);

namespace
{
    static void NotifyGraphChangedForNode(UEdGraphNode* Node)
    {
        if (UEdGraph* Graph = Node ? Node->GetGraph() : nullptr)
        {
            Graph->NotifyGraphChanged();
        }
    }

    /**
     * Compute the Levenshtein edit distance between two strings (case-insensitive).
     * Used to suggest close pin name matches in error messages.
     */
    static int32 ComputeLevenshteinDistance(const FString& A, const FString& B)
    {
        const FString LowerA = A.ToLower();
        const FString LowerB = B.ToLower();
        const int32 LenA = LowerA.Len();
        const int32 LenB = LowerB.Len();

        if (LenA == 0) return LenB;
        if (LenB == 0) return LenA;

        // Use a single row for space efficiency
        TArray<int32> Row;
        Row.SetNum(LenB + 1);
        for (int32 j = 0; j <= LenB; ++j)
        {
            Row[j] = j;
        }

        for (int32 i = 1; i <= LenA; ++i)
        {
            int32 Prev = Row[0];
            Row[0] = i;
            for (int32 j = 1; j <= LenB; ++j)
            {
                const int32 Temp = Row[j];
                if (LowerA[i - 1] == LowerB[j - 1])
                {
                    Row[j] = Prev;
                }
                else
                {
                    Row[j] = FMath::Min3(Prev, Row[j], Row[j - 1]) + 1;
                }
                Prev = Temp;
            }
        }

        return Row[LenB];
    }

    /**
     * Find the best Levenshtein match for a pin name hint from a list of available pins.
     * Returns the best matching pin name if the edit distance is within threshold,
     * or an empty string if no close match exists.
     *
     * @param Hint The name the AI used (possibly wrong)
     * @param AvailablePins Array of available pin manifest entries to search
     * @param OutDistance Receives the edit distance of the best match
     * @return Best matching pin name, or empty string if none within threshold
     */
    static FString FindBestPinMatch(
        const FString& Hint,
        const TArray<const FOlivePinManifestEntry*>& AvailablePins,
        int32& OutDistance)
    {
        FString BestMatch;
        int32 BestDist = MAX_int32;

        // Max edit distance threshold: allow up to 40% of the hint length, minimum 3
        const int32 MaxDist = FMath::Max(3, Hint.Len() * 2 / 5);

        for (const FOlivePinManifestEntry* Pin : AvailablePins)
        {
            // Check against internal pin name
            int32 Dist = ComputeLevenshteinDistance(Hint, Pin->PinName);
            if (Dist < BestDist)
            {
                BestDist = Dist;
                BestMatch = Pin->PinName;
            }

            // Also check against display name (may be different from internal name)
            if (!Pin->DisplayName.IsEmpty() && !Pin->DisplayName.Equals(Pin->PinName, ESearchCase::IgnoreCase))
            {
                Dist = ComputeLevenshteinDistance(Hint, Pin->DisplayName);
                if (Dist < BestDist)
                {
                    BestDist = Dist;
                    BestMatch = Pin->PinName; // Return internal name even if display name matched
                }
            }
        }

        OutDistance = BestDist;
        return (BestDist <= MaxDist) ? BestMatch : FString();
    }

    /**
     * Build an Alternatives array from available pins formatted as "PinName (TypeDisplayString)".
     * Also computes the best Levenshtein suggestion for the given hint.
     *
     * @param Hint The name the AI used (possibly wrong)
     * @param AvailablePins Pins to list as alternatives
     * @param OutAlternatives Receives formatted alternative strings
     * @param OutSuggestion Receives "Did you mean 'X'?" if a close match is found, or empty
     */
    static void BuildPinAlternativesAndSuggestion(
        const FString& Hint,
        const TArray<const FOlivePinManifestEntry*>& AvailablePins,
        TArray<FString>& OutAlternatives,
        FString& OutSuggestion)
    {
        OutAlternatives.Reset();
        OutAlternatives.Reserve(AvailablePins.Num());
        for (const FOlivePinManifestEntry* Pin : AvailablePins)
        {
            OutAlternatives.Add(FString::Printf(TEXT("%s (%s)"),
                *Pin->PinName, *Pin->TypeDisplayString));
        }

        int32 BestDist = MAX_int32;
        const FString BestMatch = FindBestPinMatch(Hint, AvailablePins, BestDist);
        if (!BestMatch.IsEmpty())
        {
            OutSuggestion = FString::Printf(TEXT("Did you mean '%s'?"), *BestMatch);
        }
    }
}

// ============================================================================
// EnsurePinNotOrphaned — Fix stale bOrphanedPin flags at point of use
// ============================================================================

/**
 * Check if a pin has bOrphanedPin=true and, if so, reconstruct the owning
 * node to clear orphaned flags, then re-find the pin by name.
 *
 * bOrphanedPin gets set by UE during node reconstruction (e.g., after
 * transaction rollback) when a pin existed on the old node but doesn't
 * match the reconstructed pin set.  Reused event nodes (BeginPlay, custom
 * events, FunctionEntry) survive transaction rollback because they existed
 * before the plan, but their pins can retain stale orphaned flags that
 * cause CanCreateConnection to reject wiring with "TypesIncompatible".
 *
 * This helper fixes the problem at the exact point of use: when we're
 * about to wire a pin, if it's orphaned, we reconstruct the node first.
 *
 * IMPORTANT: After ReconstructNode(), the old pin pointer is INVALID.
 * The caller MUST use the returned pointer instead.
 *
 * @param Pin The pin to check (may be nullptr)
 * @param PinName The FName of the pin (for re-finding after reconstruction)
 * @return The same pin if not orphaned, or the replacement pin after reconstruction, or nullptr on failure
 */
static UEdGraphPin* EnsurePinNotOrphaned(UEdGraphPin* Pin, const FName& PinName)
{
    if (!Pin || !Pin->bOrphanedPin)
    {
        return Pin; // Not orphaned or null — nothing to do
    }

    UEdGraphNode* OwnerNode = Pin->GetOwningNode();
    if (!OwnerNode)
    {
        return Pin; // Can't reconstruct without an owner
    }

    UE_LOG(LogOlivePlanExecutor, Warning,
        TEXT("Pin '%s' on node '%s' has bOrphanedPin=true — reconstructing node to clear orphaned flags"),
        *PinName.ToString(),
        *OwnerNode->GetNodeTitle(ENodeTitleType::ListView).ToString());

    // ReconstructNode rebuilds the pin array from scratch, clearing orphaned flags
    OwnerNode->ReconstructNode();
    NotifyGraphChangedForNode(OwnerNode);

    // Re-find the pin — the old pointer is now invalid
    UEdGraphPin* NewPin = OwnerNode->FindPin(PinName);
    if (NewPin)
    {
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("  -> Reconstruction successful, pin '%s' re-found (bOrphanedPin=%s)"),
            *PinName.ToString(),
            NewPin->bOrphanedPin ? TEXT("true (STILL ORPHANED)") : TEXT("false"));
    }
    else
    {
        UE_LOG(LogOlivePlanExecutor, Error,
            TEXT("  -> Reconstruction FAILED: pin '%s' no longer exists on node after ReconstructNode()"),
            *PinName.ToString());
    }

    return NewPin;
}

// ============================================================================
// ResolveSubPinSuffix — Static helper for SplitPin fallback
// ============================================================================

/**
 * Determine which sub-pin suffix to use when splitting a struct pin.
 * Checks for explicit hint from AI (e.g., @step.~Location_X -> "X"),
 * then falls back to smart defaults based on target pin name or type.
 *
 * @param SourcePinHint The AI's original pin hint (may contain sub-pin suffix)
 * @param StructPin The struct output pin that will be split
 * @param TargetPin The scalar input pin we're trying to connect to
 * @return Sub-pin suffix (e.g., "X", "Y", "Z", "Roll", "Pitch", "Yaw") or empty if undetermined
 */
static FString ResolveSubPinSuffix(
    const FString& SourcePinHint,
    const UEdGraphPin* StructPin,
    const UEdGraphPin* TargetPin)
{
    // Known struct -> component name mappings (engine-stable)
    struct FStructComponentMap
    {
        const TCHAR* StructName;
        TArray<FString> Components;
    };

    // Build component lists for known structs
    static const TMap<FString, TArray<FString>> KnownStructComponents = []()
    {
        TMap<FString, TArray<FString>> Map;
        Map.Add(TEXT("Vector"),      { TEXT("X"), TEXT("Y"), TEXT("Z") });
        Map.Add(TEXT("Vector2D"),    { TEXT("X"), TEXT("Y") });
        Map.Add(TEXT("Vector4"),     { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W") });
        Map.Add(TEXT("IntVector"),   { TEXT("X"), TEXT("Y"), TEXT("Z") });
        Map.Add(TEXT("IntPoint"),    { TEXT("X"), TEXT("Y") });
        Map.Add(TEXT("Rotator"),     { TEXT("Roll"), TEXT("Pitch"), TEXT("Yaw") });
        Map.Add(TEXT("LinearColor"), { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") });
        Map.Add(TEXT("Color"),       { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") });
        return Map;
    }();

    // Collect all known suffixes for fast lookup
    static const TSet<FString> AllKnownSuffixes = []()
    {
        TSet<FString> Set;
        for (const auto& Pair : KnownStructComponents)
        {
            for (const FString& Comp : Pair.Value)
            {
                Set.Add(Comp);
            }
        }
        return Set;
    }();

    // Get struct name from the pin's subcategory object
    FString StructName;
    if (StructPin->PinType.PinSubCategoryObject.IsValid())
    {
        StructName = StructPin->PinType.PinSubCategoryObject->GetName();
    }

    // Look up components for this struct type
    const TArray<FString>* StructComponents = KnownStructComponents.Find(StructName);

    // 1. Explicit hint from AI: check if hint ends with _Suffix
    if (!SourcePinHint.IsEmpty())
    {
        FString CleanHint = SourcePinHint;
        // Strip leading ~ for fuzzy hints
        if (CleanHint.StartsWith(TEXT("~")))
        {
            CleanHint = CleanHint.Mid(1);
        }

        // Check for _Suffix pattern at the end of the hint
        int32 LastUnderscore = INDEX_NONE;
        CleanHint.FindLastChar(TEXT('_'), LastUnderscore);
        if (LastUnderscore != INDEX_NONE && LastUnderscore < CleanHint.Len() - 1)
        {
            FString CandidateSuffix = CleanHint.Mid(LastUnderscore + 1);

            // Check against known suffixes (case-insensitive)
            for (const FString& Known : AllKnownSuffixes)
            {
                if (Known.Equals(CandidateSuffix, ESearchCase::IgnoreCase))
                {
                    UE_LOG(LogOlivePlanExecutor, Verbose,
                        TEXT("  ResolveSubPinSuffix: explicit hint '%s' -> suffix '%s'"),
                        *SourcePinHint, *Known);
                    return Known;
                }
            }
        }
    }

    // 2. Target pin name exact match: if target pin name IS a known component
    if (TargetPin)
    {
        FString TargetName = TargetPin->PinName.ToString();

        // Direct match against known suffixes
        for (const FString& Known : AllKnownSuffixes)
        {
            if (Known.Equals(TargetName, ESearchCase::IgnoreCase))
            {
                // Verify this suffix belongs to the struct type (if known)
                if (StructComponents)
                {
                    if (StructComponents->ContainsByPredicate(
                        [&Known](const FString& C) { return C.Equals(Known, ESearchCase::IgnoreCase); }))
                    {
                        UE_LOG(LogOlivePlanExecutor, Verbose,
                            TEXT("  ResolveSubPinSuffix: target pin '%s' exact match -> suffix '%s'"),
                            *TargetName, *Known);
                        return Known;
                    }
                }
                else
                {
                    // Unknown struct but target name matches a known suffix -- use it
                    UE_LOG(LogOlivePlanExecutor, Verbose,
                        TEXT("  ResolveSubPinSuffix: target pin '%s' matches known suffix '%s' (unknown struct '%s')"),
                        *TargetName, *Known, *StructName);
                    return Known;
                }
            }
        }

        // 3. Target pin name ends with a component suffix
        for (const FString& Known : AllKnownSuffixes)
        {
            if (TargetName.EndsWith(Known, ESearchCase::IgnoreCase) && TargetName.Len() > Known.Len())
            {
                // Verify this suffix belongs to the struct type (if known)
                if (StructComponents)
                {
                    if (StructComponents->ContainsByPredicate(
                        [&Known](const FString& C) { return C.Equals(Known, ESearchCase::IgnoreCase); }))
                    {
                        UE_LOG(LogOlivePlanExecutor, Verbose,
                            TEXT("  ResolveSubPinSuffix: target pin '%s' ends with -> suffix '%s'"),
                            *TargetName, *Known);
                        return Known;
                    }
                }
                else
                {
                    UE_LOG(LogOlivePlanExecutor, Verbose,
                        TEXT("  ResolveSubPinSuffix: target pin '%s' ends with suffix '%s' (unknown struct '%s')"),
                        *TargetName, *Known, *StructName);
                    return Known;
                }
            }
        }
    }

    // 4. Default first component for known struct types
    if (StructComponents && StructComponents->Num() > 0)
    {
        const FString& DefaultSuffix = (*StructComponents)[0];
        UE_LOG(LogOlivePlanExecutor, Verbose,
            TEXT("  ResolveSubPinSuffix: defaulting to first component '%s' for struct '%s'"),
            *DefaultSuffix, *StructName);
        return DefaultSuffix;
    }

    // 5. Truly unknown struct -- no split attempted
    UE_LOG(LogOlivePlanExecutor, Verbose,
        TEXT("  ResolveSubPinSuffix: unknown struct '%s', cannot determine sub-pin suffix"),
        *StructName);
    return FString();
}

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

    // Collect resolved class/function/variable names for stale error detection (T5).
    // Include the graph name itself — compile errors reference the enclosing graph.
    if (!GraphName.IsEmpty())
    {
        Context.ResolvedFunctionNames.Add(GraphName);
    }
    for (const FOliveResolvedStep& Step : ResolvedSteps)
    {
        if (const FString* FN = Step.Properties.Find(TEXT("function_name")))
        {
            Context.ResolvedFunctionNames.Add(*FN);
        }
        if (const FString* TC = Step.Properties.Find(TEXT("target_class")))
        {
            Context.ResolvedClassNames.Add(*TC);
        }
        // Variable names from get_var/set_var — compile errors may reference
        // pin names derived from these (e.g. "Wildcard on pin bFired").
        if (const FString* VN = Step.Properties.Find(TEXT("variable_name")))
        {
            Context.ResolvedFunctionNames.Add(*VN);
        }
    }

    // Pre-Phase 1: Clean up orphaned node chains from previous plan_json attempts
    CleanupStaleEventChains(Graph, Plan);

    // Pre-Phase 1: Clean up nodes from previous plan_json calls to the same entry points.
    // When the agent retries plan_json after a compile error, each successful call commits
    // nodes that persist. This removes those duplicates before creating new ones.
    {
        FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();
        TSet<FString> EntryKeys;

        for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
        {
            if (Step.Op == OlivePlanOps::Event || Step.Op == OlivePlanOps::CustomEvent)
            {
                FString EntryName = Step.Target;
                const FString* CompName = Step.Properties.Find(TEXT("component_name"));
                if (CompName && !CompName->IsEmpty())
                {
                    EntryName += TEXT("_") + *CompName;
                }
                FString Key = AssetPath + TEXT("::") + GraphName + TEXT("::") + EntryName;
                EntryKeys.Add(Key);
            }
        }

        // Function graph fallback: if no explicit event ops, use graph name as entry point
        if (EntryKeys.Num() == 0)
        {
            FString Key = AssetPath + TEXT("::") + GraphName + TEXT("::") + GraphName;
            EntryKeys.Add(Key);
        }

        int32 TotalCleaned = 0;
        for (const FString& Key : EntryKeys)
        {
            TotalCleaned += GraphWriter.CleanupPreviousPlanNodes(Key, Graph);
        }

        if (TotalCleaned > 0)
        {
            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("Pre-Phase 1: Cleaned up %d nodes from previous plan_json calls"), TotalCleaned);
        }
    }

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

        FOliveIRBlueprintPlanResult EarlyResult = AssembleResult(Plan, Context);

        // Sanitize reused nodes on early failure (same as end-of-Execute path)
        if (!EarlyResult.bSuccess && Context.ReusedStepIds.Num() > 0)
        {
            for (const FString& StepId : Context.ReusedStepIds)
            {
                UEdGraphNode* Node = Context.GetNodePtr(StepId);
                if (!Node) continue;

                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin && Pin->bOrphanedPin)
                    {
                        Pin->bOrphanedPin = false;
                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("Post-failure cleanup: cleared bOrphanedPin on '%s' pin '%s'"),
                            *StepId, *Pin->PinName.ToString());
                    }
                }
            }
        }

        return EarlyResult;
    }

    UE_LOG(LogOlivePlanExecutor, Log,
        TEXT("Phase 1 complete: %d nodes created"), Context.CreatedNodeCount);

    // Post-Phase 1: Record created nodes for future duplicate cleanup.
    // On the next plan_json call to the same entry point, these nodes will be removed
    // before creating new ones, preventing duplicates from retry loops.
    {
        FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();

        // Collect UObject FNames of created nodes (excluding reused event/entry/result nodes)
        TArray<FName> CreatedNodeNames;
        for (const auto& Pair : Context.StepToNodePtr)
        {
            if (Context.ReusedStepIds.Contains(Pair.Key))
            {
                continue;
            }
            if (Pair.Value)
            {
                CreatedNodeNames.Add(Pair.Value->GetFName());
            }
        }

        // Build entry keys and record
        bool bHasEventOps = false;
        for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
        {
            if (Step.Op == OlivePlanOps::Event || Step.Op == OlivePlanOps::CustomEvent)
            {
                bHasEventOps = true;
                FString EntryName = Step.Target;
                const FString* CompName = Step.Properties.Find(TEXT("component_name"));
                if (CompName && !CompName->IsEmpty())
                {
                    EntryName += TEXT("_") + *CompName;
                }
                FString Key = AssetPath + TEXT("::") + GraphName + TEXT("::") + EntryName;
                GraphWriter.RecordPlanNodes(Key, CreatedNodeNames);
            }
        }

        // Function graph fallback
        if (!bHasEventOps)
        {
            FString Key = AssetPath + TEXT("::") + GraphName + TEXT("::") + GraphName;
            GraphWriter.RecordPlanNodes(Key, CreatedNodeNames);
        }
    }

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

    // Phase 1.25: Pre-create dispatcher signature pins on custom_event nodes.
    // Must run after Phase 1 (both custom_event and bind_dispatcher nodes exist)
    // and before Phase 4 (data wiring needs the signature pins).
    UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 1.25: Pre-Create Dispatcher Pins"));
    PhasePreCreateDispatcherPins(Plan, Context);

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

        if (DataConnectionsFailed > 0)
        {
            Context.bHasDataWireFailure = true;
            UE_LOG(LogOlivePlanExecutor, Warning,
                TEXT("Phase 4: %d data wire(s) failed — marking for rollback"),
                DataConnectionsFailed);
        }

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

    FOliveIRBlueprintPlanResult Result = AssembleResult(Plan, Context);

    // Only broadcast changes on success. On failure, the pipeline will cancel
    // the transaction (rollback). Broadcasting before rollback leaves the
    // Blueprint editor with stale cached state referencing nodes that will be
    // undone — which can cause access violations during autosave/GC.
    if (Result.bSuccess && Blueprint)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        Blueprint->BroadcastChanged();

        if (Graph)
        {
            Graph->NotifyGraphChanged();
        }
    }

    // Defense-in-depth: clear bOrphanedPin on reused nodes inside the transaction.
    // NOTE: This cleanup runs INSIDE the pipeline's FScopedTransaction, so if the
    // transaction is cancelled (rollback), these changes are also rolled back --
    // making this ineffective for the primary use case.  The REAL fix is in
    // EnsurePinNotOrphaned(), which clears orphaned flags at the point of use
    // in WireExecConnection/WireDataConnection.  We keep this as a fallback for
    // cases where the transaction commits (partial success) but pins are still
    // orphaned from a prior rollback.
    if (!Result.bSuccess && Context.ReusedStepIds.Num() > 0)
    {
        for (const FString& StepId : Context.ReusedStepIds)
        {
            UEdGraphNode* Node = Context.GetNodePtr(StepId);
            if (!Node) continue;

            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->bOrphanedPin)
                {
                    Pin->bOrphanedPin = false;
                    UE_LOG(LogOlivePlanExecutor, Log,
                        TEXT("Post-failure cleanup: cleared bOrphanedPin on '%s' pin '%s'"),
                        *StepId, *Pin->PinName.ToString());
                }
            }
        }
    }

    return Result;
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
        else if (const FString* IAName = Resolved.Properties.Find(TEXT("input_action_name")))
            TargetDesc = *IAName;
        else if (const FString* VarName = Resolved.Properties.Find(TEXT("variable_name")))
            TargetDesc = *VarName;
        else if (const FString* ClassN = Resolved.Properties.Find(TEXT("actor_class")))
            TargetDesc = *ClassN;
        else if (const FString* DelegName = Resolved.Properties.Find(TEXT("delegate_name")))
        {
            TargetDesc = *DelegName;
            if (const FString* CompName = Resolved.Properties.Find(TEXT("component_name")))
            {
                TargetDesc += FString::Printf(TEXT(" on %s"), **CompName);
            }
        }
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
        const bool bIsEnhancedInputOp = (NodeType == OliveNodeTypes::EnhancedInputAction);
        const bool bIsComponentBoundEventOp = (NodeType == OliveNodeTypes::ComponentBoundEvent);

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
                    // Register with FScopedTransaction so rollback restores
                    // pin state (especially LinkedTo arrays) on this node.
                    ExistingNode->Modify();

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
                    Context.ReusedStepIds.Add(StepId);
                    continue; // Skip to next step
                }
            }
        }

        // ----------------------------------------------------------------
        // Enhanced Input Action reuse check
        // ----------------------------------------------------------------
        if (bIsEnhancedInputOp)
        {
            const FString* ActionNamePtr = Resolved.Properties.Find(TEXT("input_action_name"));
            if (ActionNamePtr && !ActionNamePtr->IsEmpty())
            {
                UEdGraphNode* ExistingNode = FindExistingEnhancedInputNode(
                    Context.Graph, *ActionNamePtr);

                if (ExistingNode)
                {
                    ExistingNode->Modify();

                    const FString ReuseNodeId = ExistingNode->NodeGuid.ToString();

                    FOlivePinManifest Manifest = FOlivePinManifest::Build(
                        ExistingNode, StepId, ReuseNodeId, NodeType);

                    Context.StepManifests.Add(StepId, MoveTemp(Manifest));
                    Context.StepToNodeMap.Add(StepId, ReuseNodeId);
                    Context.StepToNodePtr.Add(StepId, ExistingNode);
                    Context.CreatedNodeCount++;

                    Context.Warnings.Add(FString::Printf(
                        TEXT("Step '%s': Enhanced Input Action '%s' already exists in graph, reusing existing node"),
                        *StepId, **ActionNamePtr));

                    UE_LOG(LogOlivePlanExecutor, Log,
                        TEXT("Reused existing Enhanced Input Action node '%s' for step '%s'"),
                        **ActionNamePtr, *StepId);

                    ReusedStepIds.Add(StepId);
                    Context.ReusedStepIds.Add(StepId);
                    continue; // Skip to next step
                }
            }
        }

        // ----------------------------------------------------------------
        // Component Bound Event reuse check
        // ----------------------------------------------------------------
        if (bIsComponentBoundEventOp)
        {
            const FString* DelegateNamePtr = Resolved.Properties.Find(TEXT("delegate_name"));
            const FString* ComponentNamePtr = Resolved.Properties.Find(TEXT("component_name"));
            if (DelegateNamePtr && !DelegateNamePtr->IsEmpty()
                && ComponentNamePtr && !ComponentNamePtr->IsEmpty())
            {
                UEdGraphNode* ExistingNode = FindExistingComponentBoundEventNode(
                    Context.Graph, *DelegateNamePtr, *ComponentNamePtr);

                if (ExistingNode)
                {
                    ExistingNode->Modify();

                    const FString ReuseNodeId = ExistingNode->NodeGuid.ToString();

                    FOlivePinManifest Manifest = FOlivePinManifest::Build(
                        ExistingNode, StepId, ReuseNodeId, NodeType);

                    Context.StepManifests.Add(StepId, MoveTemp(Manifest));
                    Context.StepToNodeMap.Add(StepId, ReuseNodeId);
                    Context.StepToNodePtr.Add(StepId, ExistingNode);
                    Context.CreatedNodeCount++;

                    Context.Warnings.Add(FString::Printf(
                        TEXT("Step '%s': ComponentBoundEvent '%s' on '%s' already exists in graph, reusing existing node"),
                        *StepId, **DelegateNamePtr, **ComponentNamePtr));

                    UE_LOG(LogOlivePlanExecutor, Log,
                        TEXT("Reused existing ComponentBoundEvent node '%s' on '%s' for step '%s'"),
                        **DelegateNamePtr, **ComponentNamePtr, *StepId);

                    ReusedStepIds.Add(StepId);
                    Context.ReusedStepIds.Add(StepId);
                    continue; // Skip to next step
                }
            }
        }

        // ----------------------------------------------------------------
        // Virtual step: FunctionInput -- maps to existing FunctionEntry node
        // ----------------------------------------------------------------
        if (NodeType == OliveNodeTypes::FunctionInput)
        {
            UK2Node_FunctionEntry* EntryNode = nullptr;
            for (UEdGraphNode* Node : Context.Graph->Nodes)
            {
                EntryNode = Cast<UK2Node_FunctionEntry>(Node);
                if (EntryNode) break;
            }

            if (!EntryNode)
            {
                UE_LOG(LogOlivePlanExecutor, Error,
                    TEXT("Phase 1 FAIL: Step '%s' (FunctionInput) -- no FunctionEntry node in graph '%s'"),
                    *StepId, *Context.GraphName);

                Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("NODE_CREATION_FAILED"),
                    StepId,
                    FString::Printf(TEXT("/steps/%d"), i),
                    FString::Printf(TEXT("No FunctionEntry node found in graph '%s' for FunctionInput step '%s'"),
                        *Context.GraphName, *StepId),
                    TEXT("Ensure this is a function graph (not EventGraph)")));

                CleanupCreatedNodes();
                return false;
            }

            EntryNode->Modify();

            const FString ReuseNodeId = EntryNode->NodeGuid.ToString();
            FOlivePinManifest Manifest = FOlivePinManifest::Build(
                EntryNode, StepId, ReuseNodeId, NodeType);

            // Patch pin types from UserDefinedPins. Before the Blueprint is compiled,
            // graph pins on FunctionEntry may report as Wildcard. UserDefinedPins
            // always store the correct FEdGraphPinType regardless of compile state.
            for (const TSharedPtr<FUserPinInfo>& UDPin : EntryNode->UserDefinedPins)
            {
                if (!UDPin.IsValid()) continue;

                const FString PinName = UDPin->PinName.ToString();

                for (FOlivePinManifestEntry& Entry : Manifest.Pins)
                {
                    if (Entry.PinName == PinName && Entry.IRTypeCategory == EOliveIRTypeCategory::Wildcard)
                    {
                        Entry.IRTypeCategory = FOlivePinManifest::ConvertPinTypeToIRCategory(UDPin->PinType);
                        Entry.PinCategory = UDPin->PinType.PinCategory.ToString();
                        if (UDPin->PinType.PinSubCategoryObject.IsValid())
                        {
                            Entry.PinSubCategory = UDPin->PinType.PinSubCategoryObject->GetName();
                        }
                        else if (!UDPin->PinType.PinSubCategory.IsNone())
                        {
                            Entry.PinSubCategory = UDPin->PinType.PinSubCategory.ToString();
                        }
                        Entry.TypeDisplayString = UEdGraphSchema_K2::TypeToText(UDPin->PinType).ToString();

                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("  -> Patched FunctionInput pin '%s' type: Wildcard -> %s (%s)"),
                            *PinName, *Entry.TypeDisplayString, *Entry.PinCategory);
                        break;
                    }
                }
            }

            Context.StepManifests.Add(StepId, MoveTemp(Manifest));
            Context.StepToNodeMap.Add(StepId, ReuseNodeId);
            Context.StepToNodePtr.Add(StepId, EntryNode);
            Context.CreatedNodeCount++;
            ReusedStepIds.Add(StepId);
            Context.ReusedStepIds.Add(StepId);

            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("  -> Virtual FunctionInput step '%s' -> FunctionEntry node '%s'"),
                *StepId, *ReuseNodeId);
            continue;
        }

        // ----------------------------------------------------------------
        // Virtual step: FunctionOutput -- maps to existing FunctionResult node
        // ----------------------------------------------------------------
        if (NodeType == OliveNodeTypes::FunctionOutput)
        {
            UK2Node_FunctionResult* ResultNode = nullptr;
            for (UEdGraphNode* Node : Context.Graph->Nodes)
            {
                ResultNode = Cast<UK2Node_FunctionResult>(Node);
                if (ResultNode) break;
            }

            if (!ResultNode)
            {
                UE_LOG(LogOlivePlanExecutor, Error,
                    TEXT("Phase 1 FAIL: Step '%s' (FunctionOutput) -- no FunctionResult node in graph '%s'"),
                    *StepId, *Context.GraphName);

                Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("NODE_CREATION_FAILED"),
                    StepId,
                    FString::Printf(TEXT("/steps/%d"), i),
                    FString::Printf(TEXT("No FunctionResult node found in graph '%s' for FunctionOutput step '%s'"),
                        *Context.GraphName, *StepId),
                    TEXT("Ensure the function has return values defined")));

                CleanupCreatedNodes();
                return false;
            }

            ResultNode->Modify();

            const FString ReuseNodeId = ResultNode->NodeGuid.ToString();
            FOlivePinManifest Manifest = FOlivePinManifest::Build(
                ResultNode, StepId, ReuseNodeId, NodeType);

            // Patch pin types from UserDefinedPins. Before the Blueprint is compiled,
            // graph pins on FunctionResult may report as Wildcard. UserDefinedPins
            // always store the correct FEdGraphPinType regardless of compile state.
            for (const TSharedPtr<FUserPinInfo>& UDPin : ResultNode->UserDefinedPins)
            {
                if (!UDPin.IsValid()) continue;

                const FString PinName = UDPin->PinName.ToString();

                for (FOlivePinManifestEntry& Entry : Manifest.Pins)
                {
                    if (Entry.PinName == PinName && Entry.IRTypeCategory == EOliveIRTypeCategory::Wildcard)
                    {
                        Entry.IRTypeCategory = FOlivePinManifest::ConvertPinTypeToIRCategory(UDPin->PinType);
                        Entry.PinCategory = UDPin->PinType.PinCategory.ToString();
                        if (UDPin->PinType.PinSubCategoryObject.IsValid())
                        {
                            Entry.PinSubCategory = UDPin->PinType.PinSubCategoryObject->GetName();
                        }
                        else if (!UDPin->PinType.PinSubCategory.IsNone())
                        {
                            Entry.PinSubCategory = UDPin->PinType.PinSubCategory.ToString();
                        }
                        Entry.TypeDisplayString = UEdGraphSchema_K2::TypeToText(UDPin->PinType).ToString();

                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("  -> Patched FunctionOutput pin '%s' type: Wildcard -> %s (%s)"),
                            *PinName, *Entry.TypeDisplayString, *Entry.PinCategory);
                        break;
                    }
                }
            }

            Context.StepManifests.Add(StepId, MoveTemp(Manifest));
            Context.StepToNodeMap.Add(StepId, ReuseNodeId);
            Context.StepToNodePtr.Add(StepId, ResultNode);
            Context.CreatedNodeCount++;
            ReusedStepIds.Add(StepId);
            Context.ReusedStepIds.Add(StepId);

            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("  -> Virtual FunctionOutput step '%s' -> FunctionResult node '%s'"),
                *StepId, *ReuseNodeId);
            continue;
        }

        // ----------------------------------------------------------------
        // Normal node creation via GraphWriter.AddNode()
        // ----------------------------------------------------------------
        // Temporary position; Phase 6 overrides with layout
        const int32 PosX = i * 300;
        const int32 PosY = 0;

        // Pass resolved function pointer directly to NodeFactory so it can
        // skip FindFunction entirely. This eliminates double-resolution and
        // double-aliasing (e.g. "GetForwardVector" re-aliased after resolver
        // already resolved it correctly).
        // Only for CallFunction — CallDelegate uses property-based resolution
        // (FMulticastDelegateProperty), not FindFunction, so PreResolvedFunction
        // would leak and pollute the next CreateNode call.
        if (NodeType == OliveNodeTypes::CallFunction
            && Resolved.ResolvedFunction != nullptr)
        {
            FOliveNodeFactory::Get().SetPreResolvedFunction(Resolved.ResolvedFunction);
        }

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

        // Stash dynamic class references for Phase 4 wiring (spawn_actor with @ref target)
        const FString* DynClassRef = Resolved.Properties.Find(TEXT("dynamic_class_ref"));
        if (DynClassRef && !DynClassRef->IsEmpty())
        {
            Context.DynamicClassRefs.Add(StepId, *DynClassRef);
        }

        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("  -> Created step '%s' -> node '%s': %d pins (%s)"),
            *StepId, *NodeId,
            Context.StepManifests[StepId].Pins.Num(),
            Context.StepManifests[StepId].bIsPure ? TEXT("pure") : TEXT("exec"));
    }

    return true;
}

// ============================================================================
// CleanupStaleEventChains — Remove orphaned chains from previous plan_json
// ============================================================================

void FOlivePlanExecutor::CleanupStaleEventChains(UEdGraph* Graph, const FOliveIRBlueprintPlan& Plan)
{
    if (!Graph)
    {
        return;
    }

    // Gather event nodes targeted by this plan
    TArray<TPair<UEdGraphNode*, FString>> EventNodesToClean; // Node, EventName (for logging)

    UBlueprint* Blueprint = Cast<UBlueprint>(Graph->GetOuter());
    if (!Blueprint)
    {
        // Try one level deeper (some graphs are nested under function objects)
        Blueprint = Graph->GetTypedOuter<UBlueprint>();
    }

    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        UEdGraphNode* EventNode = nullptr;
        FString EventDesc;

        if (Step.Op == OlivePlanOps::Event)
        {
            EventNode = FindExistingEventNode(Graph, Blueprint, Step.Target, /*bIsCustomEvent=*/false);
            EventDesc = Step.Target;
        }
        else if (Step.Op == OlivePlanOps::CustomEvent)
        {
            EventNode = FindExistingEventNode(Graph, Blueprint, Step.Target, /*bIsCustomEvent=*/true);
            EventDesc = FString::Printf(TEXT("CustomEvent:%s"), *Step.Target);
        }

        if (EventNode)
        {
            EventNodesToClean.Add(TPair<UEdGraphNode*, FString>(EventNode, EventDesc));
        }
    }

    if (EventNodesToClean.Num() == 0)
    {
        return;
    }

    UE_LOG(LogOlivePlanExecutor, Log,
        TEXT("CleanupStaleEventChains: checking %d event(s) for orphaned chains"),
        EventNodesToClean.Num());

    static constexpr int32 MaxChainNodes = 200;
    int32 TotalRemoved = 0;

    for (const auto& Pair : EventNodesToClean)
    {
        UEdGraphNode* EventNode = Pair.Key;
        const FString& EventDesc = Pair.Value;

        // BFS forward from event node through exec output pins
        TSet<UEdGraphNode*> ChainNodes;
        TArray<UEdGraphNode*> Queue;

        // Seed: follow exec outputs from the event node
        for (UEdGraphPin* Pin : EventNode->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output)
            {
                continue;
            }

            // Only follow exec pins for the initial BFS traversal
            if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }

            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (LinkedPin && LinkedPin->GetOwningNode())
                {
                    UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
                    if (LinkedNode != EventNode && !ChainNodes.Contains(LinkedNode))
                    {
                        ChainNodes.Add(LinkedNode);
                        Queue.Add(LinkedNode);
                    }
                }
            }
        }

        // BFS: follow exec outputs from each discovered node
        int32 QueueIdx = 0;
        while (QueueIdx < Queue.Num() && ChainNodes.Num() < MaxChainNodes)
        {
            UEdGraphNode* Current = Queue[QueueIdx++];

            for (UEdGraphPin* Pin : Current->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output)
                {
                    continue;
                }

                if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }

                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (LinkedPin && LinkedPin->GetOwningNode())
                    {
                        UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
                        if (LinkedNode != EventNode && !ChainNodes.Contains(LinkedNode))
                        {
                            ChainNodes.Add(LinkedNode);
                            Queue.Add(LinkedNode);
                        }
                    }
                }
            }
        }

        if (ChainNodes.Num() == 0)
        {
            continue; // No chain hanging off this event — nothing to clean
        }

        if (ChainNodes.Num() >= MaxChainNodes)
        {
            UE_LOG(LogOlivePlanExecutor, Warning,
                TEXT("CleanupStaleEventChains: chain from '%s' hit %d node cap — skipping cleanup to be safe"),
                *EventDesc, MaxChainNodes);
            continue;
        }

        // Isolation check: every pin on every chain node must connect only to
        // other chain nodes or the event node itself. If any external connection
        // exists, the chain is NOT isolated and should not be removed.
        bool bIsIsolated = true;
        for (UEdGraphNode* ChainNode : ChainNodes)
        {
            if (!bIsIsolated)
            {
                break;
            }

            for (UEdGraphPin* Pin : ChainNode->Pins)
            {
                if (!Pin)
                {
                    continue;
                }

                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (!LinkedPin || !LinkedPin->GetOwningNode())
                    {
                        continue;
                    }

                    UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
                    if (LinkedNode != EventNode && !ChainNodes.Contains(LinkedNode))
                    {
                        // External connection found — chain is not isolated
                        bIsIsolated = false;
                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("CleanupStaleEventChains: chain from '%s' has external connection via '%s' pin '%s' -> '%s' — keeping chain"),
                            *EventDesc,
                            *ChainNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                            *Pin->PinName.ToString(),
                            *LinkedNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
                        break;
                    }
                }
            }
        }

        if (!bIsIsolated)
        {
            continue;
        }

        // Chain is isolated — remove all chain nodes (NOT the event node)
        Graph->Modify();

        int32 RemovedCount = 0;
        for (UEdGraphNode* ChainNode : ChainNodes)
        {
            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("CleanupStaleEventChains: removing stale node '%s' from '%s' chain"),
                *ChainNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                *EventDesc);

            ChainNode->BreakAllNodeLinks();
            Graph->RemoveNode(ChainNode);
            RemovedCount++;
        }

        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("CleanupStaleEventChains: removed %d stale nodes from '%s' chain"),
            RemovedCount, *EventDesc);

        TotalRemoved += RemovedCount;
    }

    if (TotalRemoved > 0)
    {
        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("CleanupStaleEventChains: total removed %d stale nodes across %d event chains"),
            TotalRemoved, EventNodesToClean.Num());
    }
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
        const FName EventFName(*EventName);

        // 1. Check parent class (native event overrides like ReceiveBeginPlay)
        if (Blueprint->ParentClass)
        {
            UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(
                Blueprint, Blueprint->ParentClass, EventFName);

            if (ExistingEvent && ExistingEvent->GetGraph() == Graph)
            {
                return ExistingEvent;
            }
        }

        // 2. Check implemented interfaces (interface events like Interact, Execute)
        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            if (!InterfaceDesc.Interface)
            {
                continue;
            }

            UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(
                Blueprint, InterfaceDesc.Interface, EventFName);

            if (ExistingEvent && ExistingEvent->GetGraph() == Graph)
            {
                return ExistingEvent;
            }
        }
    }

    return nullptr;
}

// ============================================================================
// FindExistingEnhancedInputNode
// ============================================================================

UEdGraphNode* FOlivePlanExecutor::FindExistingEnhancedInputNode(
    UEdGraph* Graph,
    const FString& InputActionName)
{
    if (!Graph)
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_EnhancedInputAction* IANode = Cast<UK2Node_EnhancedInputAction>(Node);
        if (IANode && IANode->InputAction)
        {
            // Match by asset name (case-insensitive)
            if (IANode->InputAction->GetName().Equals(InputActionName, ESearchCase::IgnoreCase))
            {
                return IANode;
            }
        }
    }

    return nullptr;
}

// ============================================================================
// FindExistingComponentBoundEventNode
// ============================================================================

UEdGraphNode* FOlivePlanExecutor::FindExistingComponentBoundEventNode(
    UEdGraph* Graph,
    const FString& DelegateName,
    const FString& ComponentName)
{
    if (!Graph)
    {
        return nullptr;
    }

    const FName DelegateFName(*DelegateName);
    const FName ComponentFName(*ComponentName);

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node);
        if (BoundEvent
            && BoundEvent->DelegatePropertyName == DelegateFName
            && BoundEvent->ComponentPropertyName == ComponentFName)
        {
            return BoundEvent;
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

        // Check if AI already provided a Target input
        bool bHandled = false;
        if (i < Plan.Steps.Num())
        {
            const FOliveIRBlueprintPlanStep& PlanStep = Plan.Steps[i];
            const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
            if (TargetValue && !TargetValue->IsEmpty())
            {
                if (TargetValue->StartsWith(TEXT("@")))
                {
                    continue; // @ref syntax -- Phase 4 data wiring handles this
                }

                // String literal -- check if it matches an SCS component variable.
                // If so, wire to THAT specific component instead of auto-detecting.
                // Phase 4 (data wiring) treats non-@ref strings as pin defaults,
                // which is wrong for an object reference pin. We handle it here.
                for (USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
                {
                    if (SCSNode && SCSNode->GetVariableName().ToString() == *TargetValue)
                    {
                        // AI specified a component by name. Wire to it directly.
                        UEdGraphNode* CreatedNode = Context.GetNodePtr(Resolved.StepId);
                        if (CreatedNode)
                        {
                            UEdGraphPin* SelfPin = CreatedNode->FindPin(UEdGraphSchema_K2::PN_Self);
                            if (SelfPin && SelfPin->LinkedTo.Num() == 0)
                            {
                                FName CompVarName = SCSNode->GetVariableName();
                                UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Context.Graph);
                                GetNode->VariableReference.SetSelfMember(CompVarName);
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

                                if (GetOutputPin)
                                {
                                    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
                                    if (Schema->TryCreateConnection(GetOutputPin, SelfPin))
                                    {
                                        Context.SuccessfulConnectionCount++;
                                        Context.AutoFixCount++;
                                        Context.Warnings.Add(FString::Printf(
                                            TEXT("Step '%s': Wired Target <- component '%s' (from string-literal input)"),
                                            *Resolved.StepId, *CompVarName.ToString()));
                                        UE_LOG(LogOlivePlanExecutor, Log,
                                            TEXT("Phase 1.5: Wired '%s' Target <- component '%s' (string-literal)"),
                                            *Resolved.StepId, *CompVarName.ToString());
                                    }
                                    else
                                    {
                                        // Connection failed -- remove orphan getter node
                                        Context.Graph->RemoveNode(GetNode);
                                    }
                                }
                                else
                                {
                                    // No output pin -- remove orphan getter node
                                    Context.Graph->RemoveNode(GetNode);
                                }
                            }
                        }
                        bHandled = true; // Component matched -- skip the auto-detect path below
                        break;
                    }
                }
                // If string literal but not a component name, fall through to auto-detect
            }
        }

        if (bHandled)
        {
            continue;
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
// Phase 1.25: Pre-Create Dispatcher Signature Pins (CONTINUE-ON-FAILURE)
// ============================================================================

void FOlivePlanExecutor::PhasePreCreateDispatcherPins(
    const FOliveIRBlueprintPlan& Plan,
    FOlivePlanExecutionContext& Context)
{
    // Track which custom_event steps have already received a delegate signature
    // to avoid double-processing when multiple bind_dispatchers exist.
    TSet<FString> SignatureSetStepIds;

    for (const FOliveIRBlueprintPlanStep& BindStep : Plan.Steps)
    {
        if (BindStep.Op != OlivePlanOps::BindDispatcher)
        {
            continue;
        }

        // Get the UK2Node_AddDelegate created in Phase 1.
        // GetDelegateSignature() on this node returns the UFunction* that
        // describes the delegate's parameter list — works for both self
        // and cross-blueprint because the node was created with the correct
        // delegate property.
        UEdGraphNode* BindNode = Context.GetNodePtr(BindStep.StepId);
        UK2Node_AddDelegate* AddDelegateNode = Cast<UK2Node_AddDelegate>(BindNode);
        if (!AddDelegateNode)
        {
            UE_LOG(LogOlivePlanExecutor, Warning,
                TEXT("Phase 1.25: bind_dispatcher step '%s' — node not found or not UK2Node_AddDelegate"),
                *BindStep.StepId);
            continue;
        }

        UFunction* DelegateSig = AddDelegateNode->GetDelegateSignature();
        if (!DelegateSig)
        {
            UE_LOG(LogOlivePlanExecutor, Warning,
                TEXT("Phase 1.25: bind_dispatcher step '%s' — GetDelegateSignature() returned nullptr, "
                     "cannot pre-create signature pins on custom_event"),
                *BindStep.StepId);
            continue;
        }

        // Dispatcher name for name-match priority
        const FString& DispatcherName = BindStep.Target;

        // Two-pass matching: same logic as the Phase 4 delegate auto-wire post-pass.
        // Pass 1: prefer a custom_event whose Target contains the dispatcher name.
        // Pass 2: fall back to first unwired custom_event not already signature-set.
        UK2Node_CustomEvent* BestEventNode = nullptr;
        FString BestStepId;

        for (const FOliveIRBlueprintPlanStep& CandStep : Plan.Steps)
        {
            if (CandStep.Op != OlivePlanOps::CustomEvent)
            {
                continue;
            }

            // Skip custom_events that already received a signature from a previous bind_dispatcher
            if (SignatureSetStepIds.Contains(CandStep.StepId))
            {
                continue;
            }

            UEdGraphNode* EventNode = Context.GetNodePtr(CandStep.StepId);
            if (!EventNode)
            {
                continue;
            }

            UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(EventNode);
            if (!CustomEventNode)
            {
                continue;
            }

            // Name-match priority: custom_event target contains dispatcher name
            if (!DispatcherName.IsEmpty()
                && CandStep.Target.Contains(DispatcherName, ESearchCase::IgnoreCase))
            {
                BestEventNode = CustomEventNode;
                BestStepId = CandStep.StepId;
                break; // Strong name match — use immediately
            }

            // Fall back to first candidate
            if (!BestEventNode)
            {
                BestEventNode = CustomEventNode;
                BestStepId = CandStep.StepId;
            }
        }

        if (!BestEventNode)
        {
            UE_LOG(LogOlivePlanExecutor, Warning,
                TEXT("Phase 1.25: bind_dispatcher '%s' — no matching custom_event found in plan"),
                *BindStep.StepId);
            continue;
        }

        // Pre-create signature pins on the custom_event node.
        // SetDelegateSignature populates UserDefinedPins from the delegate's
        // parameter function. ReconstructNode materializes them as real pins.
        BestEventNode->SetDelegateSignature(DelegateSig);
        BestEventNode->ReconstructNode();
        NotifyGraphChangedForNode(BestEventNode);

        SignatureSetStepIds.Add(BestStepId);

        // Rebuild the pin manifest for this custom_event step.
        // The manifest was built in Phase 1 with only default pins (exec, then,
        // OutputDelegate). After ReconstructNode, the node has new output pins
        // matching the delegate signature. Phase 4 data wiring needs these.
        const FOlivePinManifest* ExistingManifest = Context.StepManifests.Find(BestStepId);
        const FString NodeType = ExistingManifest ? ExistingManifest->NodeType : TEXT("CustomEvent");
        const FString* NodeIdPtr = Context.StepToNodeMap.Find(BestStepId);
        const FString NodeId = NodeIdPtr ? *NodeIdPtr : TEXT("");

        FOlivePinManifest NewManifest = FOlivePinManifest::Build(
            BestEventNode, BestStepId, NodeId, NodeType);

        Context.StepManifests.Add(BestStepId, MoveTemp(NewManifest));

        UE_LOG(LogOlivePlanExecutor, Log,
            TEXT("Phase 1.25: Pre-created delegate signature pins on custom_event '%s' "
                 "for bind_dispatcher '%s' (delegate: %s, pin count: %d)"),
            *BestStepId, *BindStep.StepId, *DelegateSig->GetName(),
            BestEventNode->Pins.Num());
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
            else if (Step.ExecAfter.Equals(TEXT("entry"), ESearchCase::IgnoreCase))
            {
                // 'entry' refers to the FunctionEntry node in function graphs.
                // Find it and wire its Then pin to this step's exec input.
                UK2Node_FunctionEntry* EntryNode = nullptr;
                for (UEdGraphNode* Node : Context.Graph->Nodes)
                {
                    EntryNode = Cast<UK2Node_FunctionEntry>(Node);
                    if (EntryNode) break;
                }

                bool bEntryWired = false;
                if (EntryNode)
                {
                    UEdGraphPin* ThenPin = EnsurePinNotOrphaned(
                        EntryNode->FindPin(UEdGraphSchema_K2::PN_Then),
                        UEdGraphSchema_K2::PN_Then);
                    UEdGraphNode* TargetNode = Context.GetNodePtr(Step.StepId);
                    UEdGraphPin* TargetExecPin = TargetNode
                        ? EnsurePinNotOrphaned(
                              TargetNode->FindPin(UEdGraphSchema_K2::PN_Execute),
                              UEdGraphSchema_K2::PN_Execute)
                        : nullptr;

                    if (ThenPin && TargetExecPin)
                    {
                        ThenPin->BreakAllPinLinks(); // Break stub wire to FunctionResult
                        ThenPin->MakeLinkTo(TargetExecPin);
                        bEntryWired = true;
                        Context.SuccessfulConnectionCount++;
                        Context.SuccessfulExecAfterStepIds.Add(Step.StepId);
                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("  Exec wire: 'entry' -> '%s' (FunctionEntry.Then)"), *Step.StepId);
                    }
                }

                if (!bEntryWired)
                {
                    UE_LOG(LogOlivePlanExecutor, Warning,
                        TEXT("  exec_after 'entry': no FunctionEntry node found or wiring failed — auto-chain will handle"));
                    // Don't count as failure — auto-chain handles this case
                }
            }
            else
            {
                // Support "step_id.PinName" syntax (e.g., "branch.False", "loop.LoopBody")
                FString SourceStep = Step.ExecAfter;
                FString PinHint;
                int32 DotIndex;
                if (SourceStep.FindChar(TEXT('.'), DotIndex))
                {
                    PinHint = SourceStep.Mid(DotIndex + 1);
                    SourceStep = SourceStep.Left(DotIndex);
                }

                UE_LOG(LogOlivePlanExecutor, Log,
                    TEXT("  Exec wire: '%s' -> '%s' (exec_after%s)"),
                    *Step.ExecAfter, *Step.StepId,
                    PinHint.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", pin='%s'"), *PinHint));

                FOliveSmartWireResult Result = WireExecConnection(
                    SourceStep,         // source step (without pin suffix)
                    PinHint,            // pin hint ("False", "LoopBody", etc. or empty)
                    Step.StepId,        // target step
                    Context);

                if (Result.bSuccess)
                {
                    UE_LOG(LogOlivePlanExecutor, Log, TEXT("    -> OK"));
                    Context.SuccessfulConnectionCount++;
                    Context.SuccessfulExecAfterStepIds.Add(Step.StepId);
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
    // Steps whose exec_after was successfully wired have an incoming exec
    // connection.  Only mark these as targeted — steps with failed wiring
    // remain orphans so the auto-chain can rescue them.
    for (const FString& WiredStepId : Context.SuccessfulExecAfterStepIds)
    {
        TargetedStepIds.Add(WiredStepId);
    }

    // ----------------------------------------------------------------
    // Pre-auto-chain: Break direct FunctionEntry → FunctionResult stub wire.
    //
    // Interface implementation graphs (and some other sources) come with
    // FunctionEntry.PN_Then wired directly to FunctionResult.PN_Execute
    // as a placeholder.  If we're about to add new nodes between them,
    // the direct wire must be broken or both auto-chains will see
    // "already connected" and skip, leaving the new logic orphaned.
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
        // Break direct FunctionEntry → FunctionResult stub wire if present.
        UEdGraphPin* EntryThenPin = EnsurePinNotOrphaned(
            EntryNode->FindPin(UEdGraphSchema_K2::PN_Then),
            UEdGraphSchema_K2::PN_Then);
        if (EntryThenPin)
        {
            for (int32 i = EntryThenPin->LinkedTo.Num() - 1; i >= 0; --i)
            {
                UEdGraphPin* LinkedPin = EntryThenPin->LinkedTo[i];
                if (LinkedPin && Cast<UK2Node_FunctionResult>(LinkedPin->GetOwningNode()))
                {
                    EntryThenPin->BreakLinkTo(LinkedPin);
                    Context.AutoFixCount++;
                    UE_LOG(LogOlivePlanExecutor, Log,
                        TEXT("Phase 3: Broke pre-existing FunctionEntry -> FunctionResult stub wire"));
                }
            }
        }

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

            // Skip virtual FunctionInput/FunctionOutput steps -- they map to
            // FunctionEntry/FunctionResult nodes, not independent exec nodes.
            // Without this, the auto-chain picks the virtual step (same node
            // as EntryNode) and fails to wire, blocking the real first orphan.
            UEdGraphNode* StepNode = Context.GetNodePtr(Step.StepId);
            if (StepNode && (Cast<UK2Node_FunctionEntry>(StepNode) || Cast<UK2Node_FunctionResult>(StepNode)))
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
            UEdGraphPin* EntryExecOut = EnsurePinNotOrphaned(
                EntryNode->FindPin(UEdGraphSchema_K2::PN_Then),
                UEdGraphSchema_K2::PN_Then);
            if (!EntryExecOut)
            {
                // Fallback: search for any exec output pin on the entry node
                for (UEdGraphPin* Pin : EntryNode->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Output &&
                        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                        !Pin->bOrphanedPin)
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
                const FName OrphanPinFName(*OrphanExecIn->PinName);
                OrphanExecInPin = EnsurePinNotOrphaned(
                    OrphanNode->FindPin(OrphanPinFName), OrphanPinFName);
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

        const FName EventExecOutFName(*EventExecOutEntry->PinName);
        UEdGraphPin* EventExecOutPin = EnsurePinNotOrphaned(
            EventNode->FindPin(EventExecOutFName), EventExecOutFName);
        if (!EventExecOutPin)
        {
            // Fallback: search for any exec output pin on the event node
            for (UEdGraphPin* Pin : EventNode->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output &&
                    Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                    !Pin->bOrphanedPin)
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

            // Skip virtual FunctionInput/FunctionOutput steps (same fix as above)
            UEdGraphNode* CandNode = Context.GetNodePtr(Candidate.StepId);
            if (CandNode && (Cast<UK2Node_FunctionEntry>(CandNode) || Cast<UK2Node_FunctionResult>(CandNode)))
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
            const FName FollowerPinFName(*FollowerExecIn->PinName);
            FollowerExecInPin = EnsurePinNotOrphaned(
                FollowerNode->FindPin(FollowerPinFName), FollowerPinFName);
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

    // ----------------------------------------------------------------
    // Auto-chain: Wire last exec node to FunctionResult node.
    //
    // In function graphs, the UK2Node_FunctionResult has an exec input
    // pin that must be connected.  When the plan omits an explicit
    // "return" op, the FunctionResult node is orphaned.  This block
    // finds the last node in the exec chain (an impure node with at
    // least one incoming exec connection but no outgoing connections)
    // and wires its PN_Then to the FunctionResult's PN_Execute.
    // ----------------------------------------------------------------
    if (EntryNode)
    {
        // Find the FunctionResult node in this graph
        UK2Node_FunctionResult* ResultNode = nullptr;
        for (UEdGraphNode* Node : Context.Graph->Nodes)
        {
            ResultNode = Cast<UK2Node_FunctionResult>(Node);
            if (ResultNode)
            {
                break;
            }
        }

        if (ResultNode)
        {
            // Check if the FunctionResult's exec input is already connected
            UEdGraphPin* ResultExecIn = ResultNode->FindPin(UEdGraphSchema_K2::PN_Execute);
            if (!ResultExecIn)
            {
                // Fallback: search for any exec input pin on the result node
                for (UEdGraphPin* Pin : ResultNode->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Input &&
                        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        ResultExecIn = Pin;
                        break;
                    }
                }
            }

            // Declare LastExecNode at this scope so the data auto-wire block
            // below (outside the exec-wiring if block) can also use it.
            UEdGraphNode* LastExecNode = nullptr;
            UEdGraphPin* LastExecOutPin = nullptr;
            int32 LastStepIndex = -1;

            if (ResultExecIn && ResultExecIn->LinkedTo.Num() == 0)
            {
                // Find the "last" exec node in the chain:
                //   - Impure (has exec pins)
                //   - Has at least one incoming exec connection (part of the chain)
                //   - Has an outgoing exec pin (PN_Then) with zero connections
                //   - Is not the FunctionEntry or FunctionResult itself
                // If multiple candidates, pick the one with the highest plan step index.

                for (int32 StepIdx = 0; StepIdx < Plan.Steps.Num(); ++StepIdx)
                {
                    const FOliveIRBlueprintPlanStep& Step = Plan.Steps[StepIdx];

                    // Must be impure
                    const FOlivePinManifest* Manifest = Context.GetManifest(Step.StepId);
                    if (!Manifest || Manifest->bIsPure)
                    {
                        continue;
                    }

                    UEdGraphNode* StepNode = Context.GetNodePtr(Step.StepId);
                    if (!StepNode)
                    {
                        continue;
                    }

                    // Skip FunctionEntry/FunctionResult nodes themselves
                    if (Cast<UK2Node_FunctionEntry>(StepNode) || Cast<UK2Node_FunctionResult>(StepNode))
                    {
                        continue;
                    }

                    // Must have at least one incoming exec connection
                    bool bHasIncomingExec = false;
                    for (UEdGraphPin* Pin : StepNode->Pins)
                    {
                        if (Pin && Pin->Direction == EGPD_Input &&
                            Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                            Pin->LinkedTo.Num() > 0)
                        {
                            bHasIncomingExec = true;
                            break;
                        }
                    }
                    if (!bHasIncomingExec)
                    {
                        continue;
                    }

                    // Must have an outgoing exec pin with zero connections
                    UEdGraphPin* CandidateExecOut = StepNode->FindPin(UEdGraphSchema_K2::PN_Then);
                    if (!CandidateExecOut)
                    {
                        // Fallback: search for any unconnected exec output pin
                        for (UEdGraphPin* Pin : StepNode->Pins)
                        {
                            if (Pin && Pin->Direction == EGPD_Output &&
                                Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                                Pin->LinkedTo.Num() == 0)
                            {
                                CandidateExecOut = Pin;
                                break;
                            }
                        }
                    }

                    if (!CandidateExecOut || CandidateExecOut->LinkedTo.Num() > 0)
                    {
                        continue;
                    }

                    // This is a valid candidate; keep the one with the highest step index
                    if (StepIdx > LastStepIndex)
                    {
                        LastStepIndex = StepIdx;
                        LastExecNode = StepNode;
                        LastExecOutPin = CandidateExecOut;
                    }
                }

                if (LastExecNode && LastExecOutPin)
                {
                    FOlivePinConnector& Connector = FOlivePinConnector::Get();
                    FOliveBlueprintWriteResult ConnectResult = Connector.Connect(
                        LastExecOutPin, ResultExecIn, false);

                    if (ConnectResult.bSuccess)
                    {
                        Context.SuccessfulConnectionCount++;
                        Context.AutoFixCount++;
                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("Phase 3: Auto-wired last exec node '%s' to FunctionResult"),
                            *LastExecNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
                    }
                    else
                    {
                        Context.FailedConnectionCount++;
                        const FString ErrorMsg = ConnectResult.Errors.Num() > 0
                            ? ConnectResult.Errors[0]
                            : TEXT("Unknown connection error");
                        UE_LOG(LogOlivePlanExecutor, Warning,
                            TEXT("Failed to auto-wire last exec node to FunctionResult: %s"),
                            *ErrorMsg);

                        Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakePlanError(
                            TEXT("RESULT_AUTOCHAIN_FAILED"),
                            FString::Printf(
                                TEXT("Failed to wire last exec node to FunctionResult: %s"),
                                *ErrorMsg),
                            TEXT("Add an explicit 'return' step at the end of the function plan")));
                    }
                }
                else
                {
                    // No impure node found in the chain — all intermediate steps
                    // are pure (getters, math, etc.).  Wire FunctionEntry directly
                    // to FunctionResult so the function has a valid exec path.
                    UEdGraphPin* EntryExecOut = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
                    if (EntryExecOut && EntryExecOut->LinkedTo.Num() == 0)
                    {
                        FOlivePinConnector& DirectConnector = FOlivePinConnector::Get();
                        FOliveBlueprintWriteResult DirectResult = DirectConnector.Connect(
                            EntryExecOut, ResultExecIn, false);

                        if (DirectResult.bSuccess)
                        {
                            Context.SuccessfulConnectionCount++;
                            Context.AutoFixCount++;
                            UE_LOG(LogOlivePlanExecutor, Log,
                                TEXT("Phase 3: All-pure function — wired FunctionEntry directly to FunctionResult"));
                        }
                        else
                        {
                            UE_LOG(LogOlivePlanExecutor, Warning,
                                TEXT("Phase 3: Failed to wire FunctionEntry -> FunctionResult in all-pure function"));
                        }
                    }
                    else
                    {
                        UE_LOG(LogOlivePlanExecutor, Verbose,
                            TEXT("Auto-chain: no last-exec-node candidate found for FunctionResult wiring -- skipping"));
                    }
                }
            }
            else if (ResultExecIn)
            {
                UE_LOG(LogOlivePlanExecutor, Verbose,
                    TEXT("FunctionResult exec input already connected, skipping auto-chain"));
            }

            // ----------------------------------------------------------------
            // Auto-wire FunctionResult DATA pins (exec chain walkback).
            //
            // When the AI omits an explicit "return" op, data output pins on
            // the FunctionResult node are left unwired.  For each unwired pin,
            // walk backward along the exec chain from the FunctionResult,
            // checking each node for a type-compatible output.  The first
            // (closest) unambiguous match wins.  This handles cases where the
            // data producer is several nodes back (e.g., SET CurrentAmmo
            // followed by SET CanFire + Call OnReloaded before Return).
            // ----------------------------------------------------------------
            if (LastExecNode)
            {
                for (UEdGraphPin* ResultPin : ResultNode->Pins)
                {
                    if (!ResultPin || ResultPin->bHidden)
                    {
                        continue;
                    }
                    if (ResultPin->Direction != EGPD_Input)
                    {
                        continue;
                    }
                    if (ResultPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                    {
                        continue;
                    }
                    if (ResultPin->LinkedTo.Num() > 0)
                    {
                        continue; // Already wired
                    }

                    // Walk backward along the exec chain looking for a
                    // type-compatible output pin. Stop at FunctionEntry.
                    UEdGraphPin* MatchPin = nullptr;
                    UEdGraphNode* MatchNode = nullptr;
                    UEdGraphNode* WalkNode = LastExecNode;
                    constexpr int32 MaxWalkDepth = 64; // Safety limit
                    int32 WalkDepth = 0;

                    // Use schema compatibility check instead of exact match.
                    // Handles IS-A relationships (Actor -> Object), wildcard pins,
                    // and interface types.
                    const UEdGraphSchema_K2* K2SchemaWalk = GetDefault<UEdGraphSchema_K2>();

                    while (WalkNode && WalkDepth < MaxWalkDepth)
                    {
                        ++WalkDepth;

                        // Don't search FunctionEntry/FunctionResult
                        if (Cast<UK2Node_FunctionEntry>(WalkNode) || Cast<UK2Node_FunctionResult>(WalkNode))
                        {
                            break;
                        }

                        // Check this node for exactly one type-compatible output
                        UEdGraphPin* NodeMatch = nullptr;
                        bool bNodeAmbiguous = false;

                        for (UEdGraphPin* CandPin : WalkNode->Pins)
                        {
                            if (!CandPin || CandPin->bHidden)
                            {
                                continue;
                            }
                            if (CandPin->Direction != EGPD_Output)
                            {
                                continue;
                            }
                            if (CandPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                            {
                                continue;
                            }
                            if (!K2SchemaWalk->ArePinTypesCompatible(
                                    CandPin->PinType, ResultPin->PinType,
                                    Context.Blueprint ? Context.Blueprint->GeneratedClass : nullptr))
                            {
                                continue;
                            }

                            if (!NodeMatch)
                            {
                                NodeMatch = CandPin;
                            }
                            else
                            {
                                bNodeAmbiguous = true;
                                break;
                            }
                        }

                        if (NodeMatch && !bNodeAmbiguous)
                        {
                            MatchPin = NodeMatch;
                            MatchNode = WalkNode;
                            break; // Closest unambiguous match wins
                        }

                        // Walk backward: find exec input pin and follow its connection
                        UEdGraphNode* PredecessorNode = nullptr;
                        for (UEdGraphPin* Pin : WalkNode->Pins)
                        {
                            if (Pin && Pin->Direction == EGPD_Input &&
                                Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                                Pin->LinkedTo.Num() > 0)
                            {
                                PredecessorNode = Pin->LinkedTo[0]->GetOwningNode();
                                break;
                            }
                        }
                        WalkNode = PredecessorNode;
                    }

                    if (MatchPin && MatchNode)
                    {
                        FOlivePinConnector& DataConnector = FOlivePinConnector::Get();
                        FOliveBlueprintWriteResult DataConnResult = DataConnector.Connect(
                            MatchPin, ResultPin, /*bAllowConversion=*/true);

                        if (DataConnResult.bSuccess)
                        {
                            Context.SuccessfulConnectionCount++;
                            Context.AutoFixCount++;
                            UE_LOG(LogOlivePlanExecutor, Log,
                                TEXT("Phase 3: Auto-wired FunctionResult data pin '%s' <- '%s' pin '%s' (walked %d node(s) back)"),
                                *ResultPin->GetName(),
                                *MatchNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                                *MatchPin->GetName(),
                                WalkDepth);
                        }
                        else
                        {
                            UE_LOG(LogOlivePlanExecutor, Warning,
                                TEXT("Phase 3: Failed to auto-wire FunctionResult data pin '%s' <- '%s'"),
                                *ResultPin->GetName(), *MatchPin->GetName());
                        }
                    }
                }
            }
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

    // --------------------------------------------------------------------
    // Reserved alias: "entry" → wire directly from UK2Node_FunctionEntry.
    // The AI often writes exec_after:"entry" without an explicit event step
    // (especially for interface implementation graphs where the entry node
    // already exists).  When "entry" is not a known step ID, resolve it
    // directly to the graph's FunctionEntry node and wire its PN_Then pin.
    // --------------------------------------------------------------------
    if (!SourceManifest && SourceStepId.Equals(TEXT("entry"), ESearchCase::IgnoreCase))
    {
        UK2Node_FunctionEntry* EntryNode = nullptr;
        for (UEdGraphNode* Node : Context.Graph->Nodes)
        {
            EntryNode = Cast<UK2Node_FunctionEntry>(Node);
            if (EntryNode) break;
        }

        if (EntryNode && TargetManifest)
        {
            const FOlivePinManifestEntry* TargetExecIn = TargetManifest->FindExecInput();
            if (TargetExecIn)
            {
                UEdGraphPin* EntryExecOut = EnsurePinNotOrphaned(
                    EntryNode->FindPin(UEdGraphSchema_K2::PN_Then),
                    UEdGraphSchema_K2::PN_Then);
                if (!EntryExecOut)
                {
                    // Fallback: any exec output on the entry node
                    for (UEdGraphPin* Pin : EntryNode->Pins)
                    {
                        if (Pin && Pin->Direction == EGPD_Output &&
                            Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                            !Pin->bOrphanedPin)
                        {
                            EntryExecOut = Pin;
                            break;
                        }
                    }
                }

                UEdGraphNode* TargetNode = Context.GetNodePtr(TargetStepId);
                const FName TargetExecInFName(*TargetExecIn->PinName);
                UEdGraphPin* TargetPin = TargetNode
                    ? EnsurePinNotOrphaned(
                          TargetNode->FindPin(TargetExecInFName), TargetExecInFName)
                    : nullptr;

                if (EntryExecOut && TargetPin)
                {
                    FOlivePinConnector& Connector = FOlivePinConnector::Get();
                    FOliveBlueprintWriteResult ConnectResult = Connector.Connect(
                        EntryExecOut, TargetPin, false);

                    if (ConnectResult.bSuccess)
                    {
                        Result.bSuccess = true;
                        Result.ResolvedSourcePin = EntryExecOut->GetName();
                        Result.ResolvedTargetPin = TargetExecIn->PinName;
                        Result.SourceMatchMethod = TEXT("entry_alias");
                        Result.TargetMatchMethod = TEXT("exec_primary");

                        UE_LOG(LogOlivePlanExecutor, Log,
                            TEXT("Exec wire via 'entry' alias: FunctionEntry.%s -> %s.%s"),
                            *EntryExecOut->GetName(), *TargetStepId, *TargetExecIn->PinName);
                        return Result;
                    }
                }
            }

            // If we found the entry node but couldn't wire, fall through to
            // the normal error path with a more specific message.
            Result.ErrorMessage = FString::Printf(
                TEXT("Found FunctionEntry for 'entry' alias but could not wire to step '%s'"),
                *TargetStepId);
            return Result;
        }
    }

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

        // Try space-stripped case-insensitive match (e.g., "IsValid" -> "Is Valid")
        if (!SourceExecOut)
        {
            FString StrippedHint = SourcePinHint.Replace(TEXT(" "), TEXT(""));
            for (const FOlivePinManifestEntry* ExecOut : AllExecOuts)
            {
                FString StrippedPinName = ExecOut->PinName.Replace(TEXT(" "), TEXT(""));
                FString StrippedDisplayName = ExecOut->DisplayName.Replace(TEXT(" "), TEXT(""));

                if (StrippedPinName.Equals(StrippedHint, ESearchCase::IgnoreCase) ||
                    StrippedDisplayName.Equals(StrippedHint, ESearchCase::IgnoreCase))
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

    const FName SourcePinFName(*SourceExecOut->PinName);
    const FName TargetPinFName(*TargetExecIn->PinName);

    UEdGraphPin* SourcePin = SourceNode->FindPin(SourcePinFName);
    UEdGraphPin* TargetPin = TargetNode->FindPin(TargetPinFName);

    // Fix stale bOrphanedPin flags on reused event/entry nodes.
    // After a previous plan's transaction rollback, UE may mark pins as
    // orphaned during undo reconstruction. This causes CanCreateConnection
    // to reject the wire with "TypesIncompatible".
    SourcePin = EnsurePinNotOrphaned(SourcePin, SourcePinFName);
    TargetPin = EnsurePinNotOrphaned(TargetPin, TargetPinFName);

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

            // For set_var steps, UE names the data input pin after the variable
            // (e.g., "BounceCount"), not "value". Remap the generic "value" key
            // to the actual variable name so FindPinSmart can match it.
            FString ResolvedPinKey = PinKey;
            if (Step.Op == OlivePlanOps::SetVar
                && PinKey.Equals(TEXT("value"), ESearchCase::IgnoreCase)
                && !Step.Target.IsEmpty())
            {
                ResolvedPinKey = Step.Target;
            }

            // "Target" is the AI-friendly name for the hidden "self" pin.
            // When the AI explicitly provides Target=@some_step.auto, they want
            // to wire to the node's self pin (which is hidden by default).
            if (ResolvedPinKey.Equals(TEXT("Target"), ESearchCase::IgnoreCase)
                || ResolvedPinKey.Equals(TEXT("self"), ESearchCase::IgnoreCase))
            {
                ResolvedPinKey = TEXT("self");
            }

            FOliveSmartWireResult Result = WireDataConnection(
                Step.StepId, ResolvedPinKey, PinValue, Context);

            if (Result.bSuccess)
            {
                Context.SuccessfulConnectionCount++;
            }
            else
            {
                Context.FailedConnectionCount++;

                // Use DATA_WIRE_INCOMPATIBLE for type mismatches (diagnostic-enriched),
                // DATA_PIN_NOT_FOUND for pin resolution failures (existing behavior)
                const TCHAR* ErrorCode = Result.bIsTypeIncompatible
                    ? TEXT("DATA_WIRE_INCOMPATIBLE")
                    : TEXT("DATA_PIN_NOT_FOUND");

                FString SuggestionText;
                if (Result.bIsTypeIncompatible && Result.Suggestions.Num() > 0)
                {
                    // Alternatives are already formatted with [confidence] prefix
                    SuggestionText = FString::Join(Result.Suggestions, TEXT("\n"));
                }
                else if (Result.Suggestions.Num() > 0)
                {
                    SuggestionText = FString::Printf(TEXT("Available pins: %s"),
                        *FString::Join(Result.Suggestions, TEXT(", ")));
                }

                // Compute Levenshtein-based "Did you mean?" from available pins.
                // Extract pin names from formatted suggestions "PinName (Type)".
                if (Result.Suggestions.Num() > 0 && !ResolvedPinKey.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
                {
                    FString BestMatch;
                    int32 BestDist = MAX_int32;
                    const int32 MaxDist = FMath::Max(3, ResolvedPinKey.Len() * 2 / 5);

                    for (const FString& Suggestion : Result.Suggestions)
                    {
                        // Extract pin name: everything before " ("
                        FString CandidateName = Suggestion;
                        int32 ParenIdx = INDEX_NONE;
                        if (Suggestion.FindChar(TEXT('('), ParenIdx) && ParenIdx > 1)
                        {
                            CandidateName = Suggestion.Left(ParenIdx).TrimEnd();
                        }

                        const int32 Dist = ComputeLevenshteinDistance(ResolvedPinKey, CandidateName);
                        if (Dist < BestDist)
                        {
                            BestDist = Dist;
                            BestMatch = CandidateName;
                        }
                    }

                    if (BestDist <= MaxDist && !BestMatch.IsEmpty())
                    {
                        SuggestionText = FString::Printf(TEXT("Did you mean '%s'? %s"),
                            *BestMatch,
                            SuggestionText.IsEmpty() ? TEXT("") : *SuggestionText);
                    }
                }

                FOliveIRBlueprintPlanError WireError = FOliveIRBlueprintPlanError::MakeStepError(
                    ErrorCode,
                    Step.StepId,
                    FString::Printf(TEXT("/steps/inputs/%s"), *PinKey),
                    Result.ErrorMessage,
                    SuggestionText);

                // Populate Alternatives array with available pin names for structured access
                WireError.Alternatives = Result.Suggestions;

                Context.WiringErrors.Add(MoveTemp(WireError));
            }
        }
    }

    // ----------------------------------------------------------------
    // Phase 4.1: Reconstruct container nodes to propagate concrete types.
    //
    // Container nodes (Array_Add, MakeArray, MakeMap, MakeSet, etc.) use
    // wildcard pins that need ReconstructNode() after data wiring so the
    // compiler can propagate concrete types from connected pins. Without
    // this, the compiler sees "type undetermined" on wildcard pins even
    // though they are correctly wired.
    //
    // ReconstructNode() preserves existing pin connections via
    // MovePersistentDataFromOldPin(), so this is safe to call post-wiring.
    // ----------------------------------------------------------------
    for (const auto& Pair : Context.StepToNodePtr)
    {
        UEdGraphNode* Node = Pair.Value;
        if (!Node) continue;

        bool bNeedsReconstruct = false;

        if (Cast<UK2Node_CallArrayFunction>(Node))
        {
            bNeedsReconstruct = true;
        }
        else if (Cast<UK2Node_MakeContainer>(Node))
        {
            bNeedsReconstruct = true;
        }
        else if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
        {
            if (UFunction* Func = CallNode->GetTargetFunction())
            {
                if (Func->GetOuterUClass() && Func->GetOuterUClass()->IsChildOf(UKismetArrayLibrary::StaticClass()))
                {
                    bNeedsReconstruct = true;
                }
            }
        }

        if (bNeedsReconstruct)
        {
            Node->ReconstructNode();
            NotifyGraphChangedForNode(Node);
            UE_LOG(LogOlivePlanExecutor, Verbose,
                TEXT("  Reconstructed container node for step '%s' (%s)"),
                *Pair.Key, *Node->GetClass()->GetName());
        }
    }

    // ----------------------------------------------------------------
    // Dynamic class pin wiring for spawn_actor steps.
    //
    // When the resolver detected a step-reference target (e.g., "@get_class.auto"),
    // it stored the reference in DynamicClassRefs during Phase 1.
    // Wire it now to the SpawnActor node's Class pin.
    // ----------------------------------------------------------------
    for (const auto& DynPair : Context.DynamicClassRefs)
    {
        const FString& StepId = DynPair.Key;
        const FString& DynRef = DynPair.Value;

        UEdGraphNode* SpawnNode = Context.GetNodePtr(StepId);
        if (!SpawnNode)
        {
            continue;
        }

        UK2Node_SpawnActorFromClass* TypedSpawn = Cast<UK2Node_SpawnActorFromClass>(SpawnNode);
        UEdGraphPin* ClassPin = TypedSpawn ? TypedSpawn->GetClassPin() : nullptr;

        if (!ClassPin)
        {
            Context.FailedConnectionCount++;
            Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                TEXT("DYNAMIC_CLASS_WIRE_FAILED"),
                StepId,
                TEXT("/steps/target"),
                TEXT("Could not find Class pin on SpawnActor node."),
                TEXT("Ensure the node is a SpawnActorFromClass node.")));
            continue;
        }

        // Clear the placeholder default so the wired value takes precedence
        ClassPin->DefaultObject = nullptr;

        // Wire from the referenced step's output
        FOliveSmartWireResult WireResult = WireDataConnection(
            StepId, ClassPin->GetName(), DynRef, Context);

        if (WireResult.bSuccess)
        {
            Context.SuccessfulConnectionCount++;
            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("  Dynamic class wire OK: %s -> step '%s'.Class"),
                *DynRef, *StepId);

            // ReconstructNode to update output pin types based on wired class
            TypedSpawn->ReconstructNode();
            NotifyGraphChangedForNode(TypedSpawn);
        }
        else
        {
            Context.FailedConnectionCount++;
            Context.WiringErrors.Add(FOliveIRBlueprintPlanError::MakeStepError(
                TEXT("DYNAMIC_CLASS_WIRE_FAILED"),
                StepId,
                TEXT("/steps/target"),
                FString::Printf(TEXT("Could not wire dynamic class reference '%s' to SpawnActor Class pin: %s"),
                    *DynRef, *WireResult.ErrorMessage),
                TEXT("Ensure the source step produces a TSubclassOf<AActor> output.")));
        }
    }

    // ----------------------------------------------------------------
    // Auto-wire unwired data pins on call_delegate/call_dispatcher nodes.
    //
    // The AI often omits inputs on dispatcher calls. For each unwired
    // data input pin, walk backward along the exec chain looking for the
    // closest node with a type-compatible output (same algorithm as the
    // FunctionResult walkback in Phase 3).
    // ----------------------------------------------------------------
    for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
    {
        if (Step.Op != OlivePlanOps::CallDelegate && Step.Op != OlivePlanOps::CallDispatcher)
        {
            continue;
        }

        UEdGraphNode* DelegateNode = Context.GetNodePtr(Step.StepId);
        if (!DelegateNode)
        {
            continue;
        }

        for (UEdGraphPin* DelegatePin : DelegateNode->Pins)
        {
            if (!DelegatePin || DelegatePin->bHidden)
            {
                continue;
            }
            if (DelegatePin->Direction != EGPD_Input)
            {
                continue;
            }
            if (DelegatePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ||
                DelegatePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
            {
                continue;
            }
            if (DelegatePin->LinkedTo.Num() > 0)
            {
                continue; // Already wired by explicit @ref
            }
            // Skip "self" / target pins
            if (DelegatePin->PinName == UEdGraphSchema_K2::PN_Self)
            {
                continue;
            }

            // Walk backward along exec chain from this node
            UEdGraphPin* MatchPin = nullptr;
            UEdGraphNode* MatchNode = nullptr;
            int32 WalkDepth = 0;
            constexpr int32 MaxWalkDepth = 64;

            // Find exec predecessor of the delegate node
            UEdGraphNode* WalkNode = nullptr;
            for (UEdGraphPin* Pin : DelegateNode->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input &&
                    Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                    Pin->LinkedTo.Num() > 0)
                {
                    WalkNode = Pin->LinkedTo[0]->GetOwningNode();
                    break;
                }
            }

            // Use schema compatibility check instead of exact match.
            // Handles IS-A relationships (Actor -> Object), wildcard pins,
            // and interface types.
            const UEdGraphSchema_K2* K2SchemaWalk = GetDefault<UEdGraphSchema_K2>();

            while (WalkNode && WalkDepth < MaxWalkDepth)
            {
                ++WalkDepth;

                UEdGraphPin* NodeMatch = nullptr;
                bool bNodeAmbiguous = false;

                for (UEdGraphPin* CandPin : WalkNode->Pins)
                {
                    if (!CandPin || CandPin->bHidden) continue;
                    if (CandPin->Direction != EGPD_Output) continue;
                    if (CandPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
                    if (!K2SchemaWalk->ArePinTypesCompatible(
                            CandPin->PinType, DelegatePin->PinType,
                            Context.Blueprint ? Context.Blueprint->GeneratedClass : nullptr)) continue;

                    if (!NodeMatch)
                    {
                        NodeMatch = CandPin;
                    }
                    else
                    {
                        bNodeAmbiguous = true;
                        break;
                    }
                }

                if (NodeMatch && !bNodeAmbiguous)
                {
                    MatchPin = NodeMatch;
                    MatchNode = WalkNode;
                    break;
                }

                // Walk to predecessor
                UEdGraphNode* PredNode = nullptr;
                for (UEdGraphPin* Pin : WalkNode->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Input &&
                        Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec &&
                        Pin->LinkedTo.Num() > 0)
                    {
                        PredNode = Pin->LinkedTo[0]->GetOwningNode();
                        break;
                    }
                }
                WalkNode = PredNode;
            }

            if (MatchPin && MatchNode)
            {
                FOlivePinConnector& Connector = FOlivePinConnector::Get();
                FOliveBlueprintWriteResult ConnResult = Connector.Connect(
                    MatchPin, DelegatePin, /*bAllowConversion=*/true);

                if (ConnResult.bSuccess)
                {
                    Context.SuccessfulConnectionCount++;
                    Context.AutoFixCount++;
                    UE_LOG(LogOlivePlanExecutor, Log,
                        TEXT("Phase 4: Auto-wired dispatcher '%s' pin '%s' <- '%s' pin '%s' (walked %d node(s) back)"),
                        *Step.StepId,
                        *DelegatePin->GetName(),
                        *MatchNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                        *MatchPin->GetName(),
                        WalkDepth);
                }
            }
        }
    }

    // ----------------------------------------------------------------
    // Auto-wire bind_dispatcher Delegate pin to custom_event OutputDelegate.
    //
    // When a bind_dispatcher node has an unwired PC_Delegate input pin,
    // search the plan for a custom_event node with an unwired OutputDelegate
    // output pin and connect them. This is the canonical UE pattern used
    // by FEdGraphSchemaAction_K2AssignDelegate.
    //
    // Two-pass matching: first prefer a custom_event whose FunctionName
    // contains the dispatcher name (e.g., "HandleHealthChanged" matches
    // "OnHealthChanged"), then fall back to any unwired custom_event.
    // ----------------------------------------------------------------
    for (const FOliveIRBlueprintPlanStep& BindStep : Plan.Steps)
    {
        if (BindStep.Op != OlivePlanOps::BindDispatcher)
        {
            continue;
        }

        UEdGraphNode* BindNode = Context.GetNodePtr(BindStep.StepId);
        UK2Node_AddDelegate* AddDelegateNode = Cast<UK2Node_AddDelegate>(BindNode);
        if (!AddDelegateNode)
        {
            continue;
        }

        UEdGraphPin* DelegateInputPin = AddDelegateNode->GetDelegatePin();
        if (!DelegateInputPin || DelegateInputPin->LinkedTo.Num() > 0)
        {
            continue; // Already wired (explicitly by AI or previous pass)
        }

        // Dispatcher name for name-match priority
        const FString& DispatcherName = BindStep.Target;

        UEdGraphNode* BestEventNode = nullptr;
        UEdGraphPin* BestDelegateOutputPin = nullptr;
        FString BestStepId;

        for (const FOliveIRBlueprintPlanStep& CandStep : Plan.Steps)
        {
            if (CandStep.Op != OlivePlanOps::CustomEvent)
            {
                continue;
            }

            UEdGraphNode* EventNode = Context.GetNodePtr(CandStep.StepId);
            if (!EventNode)
            {
                continue;
            }

            UEdGraphPin* DelegateOutputPin = EventNode->FindPin(
                UK2Node_CustomEvent::DelegateOutputName, EGPD_Output);
            if (!DelegateOutputPin || DelegateOutputPin->LinkedTo.Num() > 0)
            {
                continue; // Already wired to another bind_dispatcher
            }

            // Name-match priority: custom_event target contains dispatcher name
            if (!DispatcherName.IsEmpty()
                && CandStep.Target.Contains(DispatcherName, ESearchCase::IgnoreCase))
            {
                BestEventNode = EventNode;
                BestDelegateOutputPin = DelegateOutputPin;
                BestStepId = CandStep.StepId;
                break; // Strong name match — use immediately
            }

            // Fall back to first unwired candidate
            if (!BestEventNode)
            {
                BestEventNode = EventNode;
                BestDelegateOutputPin = DelegateOutputPin;
                BestStepId = CandStep.StepId;
            }
        }

        if (BestDelegateOutputPin)
        {
            const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
            if (K2Schema->TryCreateConnection(BestDelegateOutputPin, DelegateInputPin))
            {
                Context.SuccessfulConnectionCount++;
                Context.AutoFixCount++;
                UE_LOG(LogOlivePlanExecutor, Log,
                    TEXT("Phase 4: Auto-wired delegate: custom_event '%s' OutputDelegate -> "
                         "bind_dispatcher '%s' Delegate"),
                    *BestStepId, *BindStep.StepId);
            }
            else
            {
                UE_LOG(LogOlivePlanExecutor, Warning,
                    TEXT("Phase 4: Failed to auto-wire delegate: custom_event '%s' -> bind_dispatcher '%s'"),
                    *BestStepId, *BindStep.StepId);
            }
        }
        else
        {
            UE_LOG(LogOlivePlanExecutor, Warning,
                TEXT("Phase 4: bind_dispatcher '%s' has unwired Delegate pin but no matching custom_event found in plan"),
                *BindStep.StepId);
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

    // 1b. Handle @self references — create a UK2Node_Self and wire directly
    if (SourceStepId.Equals(TEXT("self"), ESearchCase::IgnoreCase))
    {
        const FOlivePinManifest* TargetManifest = Context.GetManifest(TargetStepId);
        if (!TargetManifest)
        {
            Result.ErrorMessage = FString::Printf(
                TEXT("Target step '%s' not found"), *TargetStepId);
            return Result;
        }

        FString TargetMatchMethod;
        const FOlivePinManifestEntry* TargetPinEntry = TargetManifest->FindPinSmart(
            TargetPinHint, /*bIsInput=*/true,
            EOliveIRTypeCategory::Unknown, &TargetMatchMethod);
        if (!TargetPinEntry)
        {
            // Populate suggestions with available input pins for self-correction
            TArray<const FOlivePinManifestEntry*> DataInputs = TargetManifest->GetDataPins(/*bInput=*/true);
            for (const FOlivePinManifestEntry* Pin : DataInputs)
            {
                Result.Suggestions.Add(FString::Printf(TEXT("%s (%s)"),
                    *Pin->PinName, *Pin->TypeDisplayString));
            }
            Result.ErrorMessage = FString::Printf(
                TEXT("Target pin '%s' not found on step '%s' for @self wire"), *TargetPinHint, *TargetStepId);
            UE_LOG(LogOlivePlanExecutor, Warning,
                TEXT("  Data wire FAILED: %s. Available: %s"),
                *Result.ErrorMessage,
                Result.Suggestions.Num() > 0
                    ? *FString::Join(Result.Suggestions, TEXT(", "))
                    : TEXT("(none)"));
            return Result;
        }

        UEdGraphNode* TargetNode = Context.GetNodePtr(TargetStepId);
        if (!TargetNode)
        {
            Result.ErrorMessage = FString::Printf(
                TEXT("Target node for step '%s' not found in context"), *TargetStepId);
            return Result;
        }

        UEdGraphPin* RealTargetPin = TargetNode->FindPin(FName(*TargetPinEntry->PinName));
        if (!RealTargetPin)
        {
            Result.ErrorMessage = FString::Printf(
                TEXT("Target pin '%s' not found on node for @self wire"), *TargetPinEntry->PinName);
            return Result;
        }

        UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Context.Graph);
        Context.Graph->AddNode(SelfNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
        SelfNode->AllocateDefaultPins();

        UEdGraphPin* SelfOutputPin = nullptr;
        for (UEdGraphPin* Pin : SelfNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output)
            {
                SelfOutputPin = Pin;
                break;
            }
        }

        if (SelfOutputPin)
        {
            FOlivePinConnector& Connector = FOlivePinConnector::Get();
            FOliveBlueprintWriteResult ConnectResult = Connector.Connect(SelfOutputPin, RealTargetPin, /*bAllowConversion=*/true);
            if (ConnectResult.bSuccess)
            {
                Result.bSuccess = true;
                Result.SourceMatchMethod = TEXT("self_reference");
                Result.TargetMatchMethod = TargetMatchMethod;
                UE_LOG(LogOlivePlanExecutor, Log,
                    TEXT("  Data wire OK: @self -> step '%s'.%s (Self Reference node)"),
                    *TargetStepId, *TargetPinEntry->PinName);
                return Result;
            }
        }

        Result.ErrorMessage = TEXT("Failed to wire @self reference — could not connect Self node output");
        return Result;
    }

    // 1c. Special case: wiring to a node's hidden "self" pin (explicit Target input).
    //     When the AI writes "Target": "@spawn_step.auto", PhaseWireData remaps
    //     "Target" to "self". We bypass FindPinSmart (which excludes hidden pins)
    //     and directly find the self pin via UEdGraphSchema_K2::PN_Self.
    //     Uses TryCreateConnection directly (like Phase 1.5) to handle BREAK_OTHERS
    //     responses when Phase 1.5 already auto-wired a component to the self pin.
    if (TargetPinHint.Equals(TEXT("self"), ESearchCase::IgnoreCase))
    {
        UEdGraphNode* TargetNode = Context.GetNodePtr(TargetStepId);
        if (TargetNode)
        {
            UEdGraphPin* SelfPin = TargetNode->FindPin(UEdGraphSchema_K2::PN_Self);
            if (SelfPin)
            {
                // Parse source ref and wire to the self pin directly
                FString SourceStepId2, SourcePinHint2;
                if (ParseDataRef(SourceRef, SourceStepId2, SourcePinHint2))
                {
                    const FOlivePinManifest* SrcManifest = Context.GetManifest(SourceStepId2);
                    UEdGraphNode* SrcNode = Context.GetNodePtr(SourceStepId2);

                    if (SrcManifest && SrcNode)
                    {
                        // Find source output pin ("auto" = first non-exec non-hidden output)
                        UEdGraphPin* SrcOutputPin = nullptr;
                        if (SourcePinHint2 == TEXT("auto"))
                        {
                            for (UEdGraphPin* Pin : SrcNode->Pins)
                            {
                                if (Pin && Pin->Direction == EGPD_Output
                                    && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                                    && !Pin->bHidden)
                                {
                                    SrcOutputPin = Pin;
                                    break;
                                }
                            }
                        }
                        else
                        {
                            SrcOutputPin = SrcNode->FindPin(FName(*SourcePinHint2));
                        }

                        if (SrcOutputPin)
                        {
                            // Log if we're about to break an existing self pin connection
                            if (SelfPin->LinkedTo.Num() > 0)
                            {
                                UE_LOG(LogOlivePlanExecutor, Warning,
                                    TEXT("  Self pin on step '%s' already connected to %s — "
                                         "breaking existing connection for explicit Target wire"),
                                    *TargetStepId,
                                    *SelfPin->LinkedTo[0]->GetOwningNode()->GetName());
                            }

                            // Use TryCreateConnection directly (same as Phase 1.5).
                            // This handles BREAK_OTHERS responses by auto-breaking the existing
                            // connection, which is correct when the AI explicitly specifies Target.
                            const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
                            bool bConnected = K2Schema->TryCreateConnection(SrcOutputPin, SelfPin);
                            if (bConnected)
                            {
                                Result.bSuccess = true;
                                Result.SourceMatchMethod = TEXT("explicit_target");
                                Result.TargetMatchMethod = TEXT("self_pin_direct");
                                Result.ResolvedSourcePin = SrcOutputPin->GetName();
                                Result.ResolvedTargetPin = TEXT("self");
                                UE_LOG(LogOlivePlanExecutor, Log,
                                    TEXT("  Data wire OK: @%s.%s -> step '%s'.self (explicit Target)"),
                                    *SourceStepId2, *SourcePinHint2, *TargetStepId);
                                return Result;
                            }
                        }
                    }
                }

                // If we found the self pin but couldn't wire, provide a clear error
                Result.ErrorMessage = FString::Printf(
                    TEXT("Found self pin on step '%s' but could not wire from '%s'. "
                         "Ensure the source produces an actor/object reference."),
                    *TargetStepId, *SourceRef);
                return Result;
            }
        }

        // No self pin found — fall through to normal resolution
        // (might be a node without a self pin, e.g., a pure math node)
    }

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
        // NAME-BASED MATCH FOR SYNTHETIC FUNCTION PARAMETER STEPS
        // Synthetic steps like _synth_param_max encode the parameter name in
        // their StepId. When the FunctionEntry node has multiple output pins
        // of the same type (e.g., Current:float, Max:float), type-based
        // matching picks the first one, mis-wiring all same-typed params.
        // Extract the parameter name and match by name first.
        static const FString SynthParamPrefix = TEXT("_synth_param_");
        if (SourceStepId.StartsWith(SynthParamPrefix))
        {
            FString ParamName = SourceStepId.Mid(SynthParamPrefix.Len());
            TArray<const FOlivePinManifestEntry*> DataOutputs = SourceManifest->GetDataPins(/*bInput=*/false);
            for (const FOlivePinManifestEntry* Pin : DataOutputs)
            {
                if (Pin->PinName.Equals(ParamName, ESearchCase::IgnoreCase))
                {
                    SourcePin = Pin;
                    SourceMatchMethod = TEXT("synth_param_name");
                    UE_LOG(LogOlivePlanExecutor, Log,
                        TEXT("  Synthetic param name match: step '%s' -> pin '%s'"),
                        *SourceStepId, *Pin->PinName);
                    break;
                }
            }
        }

        // TYPE-BASED AUTO-MATCH (fallback if name match didn't find anything)
        if (!SourcePin)
        {
            SourcePin = FindTypeCompatibleOutput(
                *SourceManifest, TargetPin->IRTypeCategory, TargetPin->PinSubCategory,
                TargetPin->PinName);
            SourceMatchMethod = TEXT("type_auto");
        }

        // Schema-based fallback: when manifest-level matching fails,
        // use real UEdGraphPin objects + ArePinTypesCompatible for
        // IS-A relationships that manifest categories cannot express
        // (e.g., Actor Object Reference -> Object Wildcard).
        if (!SourcePin)
        {
            UEdGraphNode* FallbackSourceNode = Context.GetNodePtr(SourceStepId);
            UEdGraphNode* FallbackTargetNode = Context.GetNodePtr(TargetStepId);
            UEdGraphPin* RealTargetPin = FallbackTargetNode
                ? FallbackTargetNode->FindPin(FName(*TargetPin->PinName))
                : nullptr;

            if (FallbackSourceNode && RealTargetPin)
            {
                const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
                const FOlivePinManifestEntry* SchemaMatch = nullptr;
                bool bSchemaAmbiguous = false;

                TArray<const FOlivePinManifestEntry*> DataOutputs =
                    SourceManifest->GetDataPins(/*bInput=*/false);

                for (const FOlivePinManifestEntry* CandEntry : DataOutputs)
                {
                    UEdGraphPin* RealCandPin =
                        FallbackSourceNode->FindPin(FName(*CandEntry->PinName));
                    if (!RealCandPin)
                    {
                        continue;
                    }

                    if (K2Schema->ArePinTypesCompatible(
                            RealCandPin->PinType, RealTargetPin->PinType,
                            Context.Blueprint ? Context.Blueprint->GeneratedClass : nullptr))
                    {
                        if (!SchemaMatch)
                        {
                            SchemaMatch = CandEntry;
                        }
                        else
                        {
                            bSchemaAmbiguous = true;
                            break;
                        }
                    }
                }

                if (SchemaMatch && !bSchemaAmbiguous)
                {
                    SourcePin = SchemaMatch;
                    SourceMatchMethod = TEXT("type_auto_schema_fallback");
                    UE_LOG(LogOlivePlanExecutor, Log,
                        TEXT("  FindTypeCompatibleOutput schema fallback: "
                             "matched '%s' via ArePinTypesCompatible"),
                        *SchemaMatch->PinName);
                }
            }
        }

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

    const FName SourcePinFName(*SourcePin->PinName);
    const FName TargetPinFName(*TargetPin->PinName);

    UEdGraphPin* RealSourcePin = SourceNode->FindPin(SourcePinFName);
    UEdGraphPin* RealTargetPin = TargetNode->FindPin(TargetPinFName);

    // Fix stale bOrphanedPin flags (see EnsurePinNotOrphaned doc comment)
    RealSourcePin = EnsurePinNotOrphaned(RealSourcePin, SourcePinFName);
    RealTargetPin = EnsurePinNotOrphaned(RealTargetPin, TargetPinFName);

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

    // Probe to detect if conversion will be needed (for ConversionNote tracking).
    // This is more robust than post-hoc LinkedTo heuristics, especially when
    // the source pin already has connections from previous wiring iterations.
    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
    FPinConnectionResponse ProbeResponse = K2Schema->CanCreateConnection(RealSourcePin, RealTargetPin);
    const bool bWillNeedConversion =
        (ProbeResponse.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE);

    const int32 SourceLinkCountBefore = RealSourcePin->LinkedTo.Num();

    FOliveBlueprintWriteResult ConnectResult = Connector.Connect(RealSourcePin, RealTargetPin, /*bAllowConversion=*/true);

    if (ConnectResult.bSuccess)
    {
        Result.bSuccess = true;
        Result.ResolvedSourcePin = SourcePin->PinName;
        Result.ResolvedTargetPin = TargetPin->PinName;
        Result.SourceMatchMethod = SourceMatchMethod;
        Result.TargetMatchMethod = TargetMatchMethod;

        // Record conversion note if a conversion node was auto-inserted.
        // We use the probe result rather than post-hoc LinkedTo heuristics,
        // which is robust against multi-connection and link ordering changes.
        if (bWillNeedConversion)
        {
            FOliveConversionNote Note;
            Note.SourceStep = SourceStepId;
            Note.TargetStep = TargetStepId;
            Note.SourcePinName = SourcePin->PinName;
            Note.TargetPinName = TargetPin->PinName;
            Note.FromType = SourcePin->TypeDisplayString;
            Note.ToType = TargetPin->TypeDisplayString;

            // Find the conversion node: it's the new link that wasn't there before.
            // After TryCreateConnection with conversion, SourcePin is linked to the
            // conversion node's input (not TargetPin directly).
            for (int32 i = SourceLinkCountBefore; i < RealSourcePin->LinkedTo.Num(); ++i)
            {
                UEdGraphNode* PossibleConv = RealSourcePin->LinkedTo[i]->GetOwningNode();
                if (PossibleConv && PossibleConv != TargetNode)
                {
                    Note.ConversionNodeType = PossibleConv->GetClass()->GetName();
                    break;
                }
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
        // --------------------------------------------------------
        // SplitPin fallback: Struct output -> Scalar input
        // When a struct output (Vector, Rotator, etc.) fails to
        // connect to a scalar input (Float, Double), split the
        // struct pin and connect the appropriate sub-pin.
        // --------------------------------------------------------
        bool bSplitPinRecovery = false;

        if (RealSourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
            && RealSourcePin->SubPins.Num() == 0  // Not already split
            && !RealSourcePin->bHidden)            // Not hidden
        {
            if (K2Schema->CanSplitStructPin(*RealSourcePin))
            {
                // Determine which sub-pin to target
                FString SubPinSuffix = ResolveSubPinSuffix(
                    SourcePinHint, RealSourcePin, RealTargetPin);

                if (!SubPinSuffix.IsEmpty())
                {
                    // Perform the split
                    K2Schema->SplitPin(RealSourcePin, /*bNotify=*/true);

                    // Find the sub-pin: format is {ParentPinName}_{ComponentName}
                    FString SubPinName = FString::Printf(TEXT("%s_%s"),
                        *RealSourcePin->PinName.ToString(), *SubPinSuffix);

                    UEdGraphPin* SubPin = nullptr;
                    for (UEdGraphPin* SP : RealSourcePin->SubPins)
                    {
                        if (SP && SP->PinName.ToString().Equals(SubPinName, ESearchCase::IgnoreCase))
                        {
                            SubPin = SP;
                            break;
                        }
                    }

                    if (SubPin)
                    {
                        // Re-attempt connection with the sub-pin
                        FOliveBlueprintWriteResult SplitConnectResult =
                            Connector.Connect(SubPin, RealTargetPin, /*bAllowConversion=*/true);

                        if (SplitConnectResult.bSuccess)
                        {
                            bSplitPinRecovery = true;
                            Result.bSuccess = true;
                            Result.ResolvedSourcePin = SubPin->PinName.ToString();
                            Result.ResolvedTargetPin = TargetPin->PinName;
                            Result.SourceMatchMethod = SourceMatchMethod + TEXT("_split_pin");
                            Result.TargetMatchMethod = TargetMatchMethod;

                            // Log conversion note for SplitPin
                            FOliveConversionNote Note;
                            Note.SourceStep = SourceStepId;
                            Note.TargetStep = TargetStepId;
                            Note.SourcePinName = SourcePin->PinName;
                            Note.TargetPinName = TargetPin->PinName;
                            Note.FromType = SourcePin->TypeDisplayString;
                            Note.ToType = TargetPin->TypeDisplayString;
                            Note.ConversionNodeType = FString::Printf(
                                TEXT("SplitPin(%s)"), *SubPinSuffix);

                            UE_LOG(LogOlivePlanExecutor, Log,
                                TEXT("SplitPin recovery: %s.%s -> %s.%s via sub-pin %s"),
                                *SourceStepId, *SourcePin->PinName,
                                *TargetStepId, *TargetPin->PinName,
                                *SubPinName);

                            Context.Warnings.Add(FString::Printf(
                                TEXT("SplitPin: connected %s.%s (sub-pin %s) -> %s.%s"),
                                *SourceStepId, *SourcePin->PinName,
                                *SubPinSuffix,
                                *TargetStepId, *TargetPin->PinName));

                            Context.ConversionNotes.Add(MoveTemp(Note));
                        }
                    }
                }
            }
        }

        if (!bSplitPinRecovery)
        {
            // Enriched failure path with structured wiring diagnostic
            if (ConnectResult.WiringDiagnostic.IsSet())
            {
                const FOliveWiringDiagnostic& Diag = ConnectResult.WiringDiagnostic.GetValue();
                Result.bIsTypeIncompatible = true;
                Result.ErrorMessage = FString::Printf(
                    TEXT("Pin connection failed (%s.%s [%s] -> %s.%s [%s]): %s"),
                    *SourceStepId, *SourcePin->PinName, *Diag.SourceTypeName,
                    *TargetStepId, *TargetPin->PinName, *Diag.TargetTypeName,
                    *Diag.SchemaMessage);

                // Surface alternatives as suggestions in the wiring error
                for (const FOliveWiringAlternative& Alt : Diag.Alternatives)
                {
                    Result.Suggestions.Add(FString::Printf(TEXT("[%s] %s: %s"),
                        *Alt.Confidence, *Alt.Label, *Alt.Action));
                }
            }
            else
            {
                // Fallback to bare error string
                Result.ErrorMessage = FString::Printf(
                    TEXT("Pin connection failed (%s.%s -> %s.%s): %s"),
                    *SourceStepId, *SourcePin->PinName,
                    *TargetStepId, *TargetPin->PinName,
                    ConnectResult.Errors.Num() > 0 ? *ConnectResult.Errors[0] : TEXT("Unknown"));
            }
        }
    }

    return Result;
}

// ============================================================================
// FindTypeCompatibleOutput
// ============================================================================

const FOlivePinManifestEntry* FOlivePlanExecutor::FindTypeCompatibleOutput(
    const FOlivePinManifest& SourceManifest,
    EOliveIRTypeCategory TargetTypeCategory,
    const FString& TargetSubCategory,
    const FString& TargetPinName)
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

    // Wildcard fallback: if strict matching found nothing, accept Wildcard-typed pins.
    // Wildcard pins occur when pin types haven't been resolved yet, e.g., pre-compile
    // FunctionEntry output pins whose types are only known after compilation.
    if (TypeMatches.Num() == 0)
    {
        for (const FOlivePinManifestEntry* Pin : DataOutputs)
        {
            if (Pin->IRTypeCategory == EOliveIRTypeCategory::Wildcard)
            {
                TypeMatches.Add(Pin);
            }
        }

        if (TypeMatches.Num() > 0)
        {
            UE_LOG(LogOlivePlanExecutor, Log,
                TEXT("  FindTypeCompatibleOutput: No strict match for %s, using Wildcard fallback (%d candidates)"),
                *UEnum::GetValueAsString(TargetTypeCategory), TypeMatches.Num());
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
        // Name-based disambiguation: if target pin name matches a source pin name, prefer it
        if (!TargetPinName.IsEmpty())
        {
            for (const FOlivePinManifestEntry* Pin : TypeMatches)
            {
                if (Pin->PinName.Equals(TargetPinName, ESearchCase::IgnoreCase))
                {
                    UE_LOG(LogOlivePlanExecutor, Log,
                        TEXT("  Name-disambiguated: picked '%s' by matching target pin name"),
                        *Pin->PinName);
                    return Pin;
                }
            }
        }

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

    // Strip leading '~' from pin hint — Olive convention for struct break outputs
    // (e.g., "@break_hit.~HitActor") but UE pin names don't include the tilde.
    if (OutPinHint.StartsWith(TEXT("~")))
    {
        OutPinHint = OutPinHint.Mid(1);
    }

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

                TArray<const FOlivePinManifestEntry*> DataInputs = Manifest->GetDataPins(true);

                TArray<FString> Alternatives;
                FString DidYouMean;
                BuildPinAlternativesAndSuggestion(ResolvedPinKey, DataInputs, Alternatives, DidYouMean);

                FString SuggestionText;
                if (!DidYouMean.IsEmpty())
                {
                    SuggestionText = FString::Printf(TEXT("%s Available data inputs: %s"),
                        *DidYouMean, *FString::Join(Alternatives, TEXT(", ")));
                }
                else
                {
                    SuggestionText = FString::Printf(TEXT("Available data inputs: %s"),
                        *FString::Join(Alternatives, TEXT(", ")));
                }

                FOliveIRBlueprintPlanError DefaultError = FOliveIRBlueprintPlanError::MakeStepError(
                    TEXT("DEFAULT_PIN_NOT_FOUND"),
                    Step.StepId,
                    FString::Printf(TEXT("/steps/inputs/%s"), *PinKey),
                    FString::Printf(TEXT("No input pin matching '%s' on step '%s'"),
                        *PinKey, *Step.StepId),
                    SuggestionText);
                DefaultError.Alternatives = MoveTemp(Alternatives);

                Context.WiringErrors.Add(MoveTemp(DefaultError));
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

            // For class/interface pins (TSubclassOf<>, TSoftClassPtr<>, TScriptInterface<>),
            // resolve the name to a UClass* and set DefaultObject. TrySetDefaultValue
            // doesn't handle short class names on these pin types.
            if (RealPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class
                || RealPin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass
                || RealPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Interface)
            {
                FOliveClassResolveResult ResolveResult = FOliveClassResolver::Resolve(PinValue);
                if (ResolveResult.IsValid())
                {
                    RealPin->DefaultObject = ResolveResult.Class;
                    UE_LOG(LogOlivePlanExecutor, Verbose,
                        TEXT("Set class default: %s.%s = %s (resolved UClass*)"),
                        *Step.StepId, *PinEntry->PinName, *ResolveResult.Class->GetName());
                }
                else
                {
                    // Fallback: try Schema
                    const UEdGraphSchema* Schema = Context.Graph->GetSchema();
                    if (Schema)
                    {
                        Schema->TrySetDefaultValue(*RealPin, PinValue);
                    }
                    else
                    {
                        RealPin->DefaultValue = PinValue;
                    }
                    UE_LOG(LogOlivePlanExecutor, Warning,
                        TEXT("Class pin '%s' on step '%s': UClass '%s' not found, falling back to TrySetDefaultValue"),
                        *PinEntry->PinName, *Step.StepId, *PinValue);
                }
            }
            else
            {
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

        // Only validate nodes created/registered by the current plan.
        // Nodes from previous plan_json calls are not in the map and
        // must be skipped to avoid false "orphaned exec flow" warnings.
        const FString NodeId = Node->NodeGuid.ToString();
        if (!Context.NodeIdToStepId.Contains(NodeId))
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

        // ================================================================
        // Check 4: Unwired required object input pins
        // Only checks object-like pins (PC_Object, PC_Interface, etc.)
        // Uses CPF_RequiredParm to distinguish required vs optional params
        // ================================================================
        {
            // Get UFunction for CallFunction nodes (used for CPF_RequiredParm check)
            UK2Node_CallFunction* Check4CallFunc = Cast<UK2Node_CallFunction>(K2Node);
            UFunction* Check4Func = Check4CallFunc ? Check4CallFunc->GetTargetFunction() : nullptr;

            for (UEdGraphPin* Pin : K2Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input)
                {
                    continue;
                }

                // Skip exec pins
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }

                // Skip wildcard pins
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
                {
                    continue;
                }

                // Only check object-like pin categories
                const FName& Category = Pin->PinType.PinCategory;
                if (Category != UEdGraphSchema_K2::PC_Object &&
                    Category != UEdGraphSchema_K2::PC_Interface &&
                    Category != UEdGraphSchema_K2::PC_SoftObject &&
                    Category != UEdGraphSchema_K2::PC_SoftClass &&
                    Category != UEdGraphSchema_K2::PC_Class)
                {
                    continue;
                }

                // Skip hidden/not-connectable pins
                if (Pin->bHidden || Pin->bNotConnectable)
                {
                    continue;
                }

                // Skip Self pin (handled by Check 2 and Phase 1.5)
                if (Pin->PinName == UEdGraphSchema_K2::PN_Self)
                {
                    continue;
                }

                // Skip WorldContextObject (auto-wired by UE at compile time)
                if (Pin->PinName == TEXT("WorldContextObject"))
                {
                    continue;
                }

                // Skip advanced view pins
                if (Pin->bAdvancedView)
                {
                    continue;
                }

                // Skip if already wired or has a default
                if (Pin->LinkedTo.Num() > 0 || !Pin->DefaultValue.IsEmpty() || Pin->DefaultObject != nullptr)
                {
                    continue;
                }

                // Check underlying FProperty for optional status (CallFunction nodes only)
                if (Check4Func)
                {
                    bool bIsOptionalParam = false;
                    for (TFieldIterator<FProperty> PropIt(Check4Func); PropIt; ++PropIt)
                    {
                        if (PropIt->GetFName() == Pin->PinName &&
                            PropIt->HasAnyPropertyFlags(CPF_Parm) &&
                            !PropIt->HasAnyPropertyFlags(CPF_RequiredParm))
                        {
                            bIsOptionalParam = true;
                            break;
                        }
                    }
                    if (bIsOptionalParam)
                    {
                        continue; // Optional param -- skip
                    }
                }

                // This pin is unwired, has no default, and is required -- report it
                const FString StepId4 = Context.FindStepIdForNode(Node);
                FString NodeDesc = FString::Printf(TEXT("%s (step: %s)"),
                    *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                    StepId4.IsEmpty() ? TEXT("unknown") : *StepId4);

                FString TypeName = Pin->PinType.PinCategory.ToString();
                if (UObject* SubObj = Pin->PinType.PinSubCategoryObject.Get())
                {
                    TypeName = SubObj->GetName();
                }

                Context.PreCompileIssues.Add(FString::Printf(
                    TEXT("UNWIRED_REQUIRED_INPUT: '%s' has required object input '%s' (%s) "
                         "that is not wired and has no default. Wire it to a valid source or "
                         "use set_pin_default to set a value."),
                    *NodeDesc, *Pin->GetName(), *TypeName));

                UE_LOG(LogOlivePlanExecutor, Warning,
                    TEXT("Phase 5.5: Unwired required input '%s' (%s) on '%s'"),
                    *Pin->GetName(), *TypeName, *NodeDesc);
            }
        }
    }

    // ================================================================
    // Check 3: Unwired FunctionResult data pins (function graphs only)
    // ================================================================
    {
        // Detect function graph by presence of FunctionEntry node
        UK2Node_FunctionResult* ResultNode = nullptr;
        UK2Node_FunctionEntry* EntryNode = nullptr;
        for (UEdGraphNode* Node : Context.Graph->Nodes)
        {
            if (!EntryNode)
            {
                EntryNode = Cast<UK2Node_FunctionEntry>(Node);
            }
            if (!ResultNode)
            {
                ResultNode = Cast<UK2Node_FunctionResult>(Node);
            }
            if (EntryNode && ResultNode)
            {
                break;
            }
        }

        if (EntryNode && ResultNode)
        {
            // Check if this is an interface implementation graph — nudge toward events if so
            bool bIsInterfaceGraph = false;
            for (const FBPInterfaceDescription& Desc : Context.Blueprint->ImplementedInterfaces)
            {
                for (UEdGraph* InterfaceGraph : Desc.Graphs)
                {
                    if (InterfaceGraph == Context.Graph)
                    {
                        bIsInterfaceGraph = true;
                        break;
                    }
                }
                if (bIsInterfaceGraph) break;
            }

            for (UEdGraphPin* Pin : ResultNode->Pins)
            {
                if (!Pin || Pin->bHidden)
                {
                    continue;
                }
                if (Pin->Direction != EGPD_Input)
                {
                    continue;
                }
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }
                if (Pin->LinkedTo.Num() > 0)
                {
                    continue;
                }

                FString TypeName = Pin->PinType.PinCategory.ToString();
                if (UObject* SubObj = Pin->PinType.PinSubCategoryObject.Get())
                {
                    TypeName = SubObj->GetName();
                }

                Context.PreCompileIssues.Add(FString::Printf(
                    TEXT("UNWIRED_RETURN_PIN: Function output '%s' (%s) on FunctionResult is not wired. "
                         "Add a 'return' step with inputs mapping to output pins, or use connect_pins to wire it manually."),
                    *Pin->GetName(), *TypeName));

                if (bIsInterfaceGraph)
                {
                    Context.PreCompileIssues.Add(FString::Printf(
                        TEXT("INTERFACE_FUNCTION_HINT: Interface function '%s' has unwired return pin '%s' (%s). "
                             "Functions WITH outputs become synchronous function graphs -- no Timelines, Delays, or latent nodes allowed. "
                             "If this function's implementations need smooth movement, animations, or multi-frame behavior, "
                             "remove the outputs from the interface to make it an implementable event instead. "
                             "If you genuinely need BOTH a return value AND async behavior, use the hybrid pattern: "
                             "function returns immediately, then calls a Custom Event for the async work."),
                        *Context.GraphName, *Pin->GetName(), *TypeName));
                }

                UE_LOG(LogOlivePlanExecutor, Warning,
                    TEXT("Phase 5.5: Unwired FunctionResult data pin '%s' (%s) in function '%s'"),
                    *Pin->GetName(), *TypeName, *Context.GraphName);
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

    // Success = all nodes created AND no critical wiring errors (connection failures)
    const bool bAllNodesCreated = (Context.CreatedNodeCount == Plan.Steps.Num());
    const bool bHasCriticalWiringErrors = (Context.FailedConnectionCount > 0);

    Result.bSuccess = bAllNodesCreated && !bHasCriticalWiringErrors;
    Result.StepToNodeMap = Context.StepToNodeMap;
    Result.ReusedStepIds = Context.ReusedStepIds;
    Result.PlanClassNames = Context.ResolvedClassNames;
    Result.PlanFunctionNames = Context.ResolvedFunctionNames;
    Result.AppliedOpsCount = Context.CreatedNodeCount + Context.SuccessfulConnectionCount + Context.SuccessfulDefaultCount;
    Result.Errors = Context.WiringErrors;
    Result.Warnings = Context.Warnings;

    // Populate per-category counters
    Result.ConnectionsSucceeded = Context.SuccessfulConnectionCount;
    Result.ConnectionsFailed = Context.FailedConnectionCount;
    Result.DefaultsSucceeded = Context.SuccessfulDefaultCount;
    Result.DefaultsFailed = Context.FailedDefaultCount;

    // Partial = nodes created, no connection failures, but some defaults failed (cosmetic)
    Result.bPartial = bAllNodesCreated && !bHasCriticalWiringErrors
        && (Context.FailedDefaultCount > 0);

    // Data wire failures mean the graph is partially wired — force rollback
    // so residual nodes don't corrupt subsequent plans
    if (Context.bHasDataWireFailure)
    {
        Result.bSuccess = false;
        Result.bPartial = false;
    }

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

    // Add partial success warning when any wiring or default failures occurred
    const bool bHasAnyWiringErrors = (Context.FailedConnectionCount > 0 || Context.FailedDefaultCount > 0);
    if (bAllNodesCreated && bHasAnyWiringErrors)
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
