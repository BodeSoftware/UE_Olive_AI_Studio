// Copyright Bode Software. All Rights Reserved.

/**
 * OliveBlueprintPlanResolver.cpp
 *
 * Implements the stateless plan resolver that maps intent-level plan ops
 * (from OlivePlanOps vocabulary) to concrete Blueprint node types
 * (from OliveNodeTypes namespace). Uses FOliveNodeCatalog for function
 * disambiguation and Blueprint metadata for variable/event validation.
 */

#include "Plan/OliveBlueprintPlanResolver.h"
#include "Writer/OliveNodeFactory.h"
#include "Catalog/OliveNodeCatalog.h"
#include "OliveClassResolver.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "EdGraphSchema_K2.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOlivePlanResolver);

// ============================================================================
// Anonymous Namespace — Helpers
// ============================================================================

namespace
{
	/**
	 * Check whether a variable with the given name exists on the Blueprint's
	 * NewVariables array (direct variables, not inherited).
	 * @param Blueprint The Blueprint to check
	 * @param VariableName The variable name to search for
	 * @return True if the variable is found on this Blueprint directly
	 */
	bool BlueprintHasVariable(const UBlueprint* Blueprint, const FString& VariableName)
	{
		if (!Blueprint)
		{
			return false;
		}

		// Check explicit Blueprint variables
		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			if (Var.VarName.ToString() == VariableName)
			{
				return true;
			}
		}

		// Check SCS component variables (components ARE variables on the generated class)
		if (Blueprint->SimpleConstructionScript)
		{
			TArray<USCS_Node*> AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
			for (USCS_Node* Node : AllNodes)
			{
				if (Node && Node->GetVariableName().ToString() == VariableName)
				{
					return true;
				}
			}
		}

		return false;
	}

	/**
	 * Copy all user-specified properties from a plan step into the resolved
	 * step's Properties map, without overwriting properties that were already
	 * set by the per-op resolver.
	 * @param Step The source plan step
	 * @param OutProperties The target properties map (already partially populated)
	 */
	void MergeStepProperties(
		const FOliveIRBlueprintPlanStep& Step,
		TMap<FString, FString>& OutProperties)
	{
		for (const auto& Pair : Step.Properties)
		{
			if (!OutProperties.Contains(Pair.Key))
			{
				OutProperties.Add(Pair.Key, Pair.Value);
			}
		}
	}

	/** Minimum catalog search score to accept a single-result match automatically */
	static constexpr int32 MIN_AUTO_MATCH_SCORE = 50;

	/** Number of catalog search results to request for disambiguation */
	static constexpr int32 CATALOG_SEARCH_LIMIT = 5;
}

// ============================================================================
// FOliveGraphContext::BuildFromBlueprint
// ============================================================================

FOliveGraphContext FOliveGraphContext::BuildFromBlueprint(UBlueprint* Blueprint, const FString& GraphName)
{
	FOliveGraphContext Ctx;
	Ctx.GraphName = GraphName;

	if (!Blueprint)
	{
		return Ctx;
	}

	// Search UbergraphPages (EventGraph and other ubergraphs)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetFName() == FName(*GraphName))
		{
			Ctx.Graph = Graph;
			return Ctx; // EventGraph/ubergraph -- not a function graph
		}
	}

	// Search FunctionGraphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == FName(*GraphName))
		{
			Ctx.Graph = Graph;
			Ctx.bIsFunctionGraph = true;

			// Scan for FunctionEntry to get input param names
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
				{
					for (const auto& Pin : Entry->UserDefinedPins)
					{
						if (Pin.IsValid())
						{
							Ctx.InputParamNames.Add(Pin->PinName.ToString());
						}
					}
				}
				else if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
				{
					for (const auto& Pin : Result->UserDefinedPins)
					{
						if (Pin.IsValid())
						{
							Ctx.OutputParamNames.Add(Pin->PinName.ToString());
						}
					}
				}
			}

			UE_LOG(LogOlivePlanResolver, Log,
				TEXT("GraphContext: '%s' is function graph (%d inputs, %d outputs)"),
				*GraphName, Ctx.InputParamNames.Num(), Ctx.OutputParamNames.Num());
			return Ctx;
		}
	}

	// Search interface implementation graphs (ImplementedInterfaces[i].Graphs).
	// These are NOT in FunctionGraphs — UE stores them separately.
	for (FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		for (UEdGraph* Graph : InterfaceDesc.Graphs)
		{
			if (Graph && Graph->GetFName() == FName(*GraphName))
			{
				Ctx.Graph = Graph;
				Ctx.bIsFunctionGraph = true;

				// Scan for FunctionEntry to get input/output param names (same as regular function graphs)
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
					{
						for (const auto& Pin : Entry->UserDefinedPins)
						{
							if (Pin.IsValid())
							{
								Ctx.InputParamNames.Add(Pin->PinName.ToString());
							}
						}
					}
					else if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
					{
						for (const auto& Pin : Result->UserDefinedPins)
						{
							if (Pin.IsValid())
							{
								Ctx.OutputParamNames.Add(Pin->PinName.ToString());
							}
						}
					}
				}

				UE_LOG(LogOlivePlanResolver, Log,
					TEXT("GraphContext: '%s' is interface implementation graph (%d inputs, %d outputs)"),
					*GraphName, Ctx.InputParamNames.Num(), Ctx.OutputParamNames.Num());
				return Ctx;
			}
		}
	}

	// Search MacroGraphs
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph && Graph->GetFName() == FName(*GraphName))
		{
			Ctx.Graph = Graph;
			Ctx.bIsMacroGraph = true;
			return Ctx;
		}
	}

	// Graph not found -- could be a new graph about to be created
	UE_LOG(LogOlivePlanResolver, Log,
		TEXT("GraphContext: Graph '%s' not found in Blueprint '%s' -- using default context"),
		*GraphName, *Blueprint->GetName());
	return Ctx;
}

// ============================================================================
// Resolve (public entry point)
// ============================================================================

FOlivePlanResolveResult FOliveBlueprintPlanResolver::Resolve(
	const FOliveIRBlueprintPlan& Plan,
	UBlueprint* Blueprint,
	const FOliveGraphContext& GraphContext)
{
	FOlivePlanResolveResult Result;

	if (!Blueprint)
	{
		FOliveIRBlueprintPlanError Error = FOliveIRBlueprintPlanError::MakePlanError(
			TEXT("NULL_BLUEPRINT"),
			TEXT("Blueprint pointer is null — cannot resolve plan"),
			TEXT("Provide a valid asset_path to a loaded Blueprint"));
		Result.Errors.Add(MoveTemp(Error));
		Result.bSuccess = false;
		return Result;
	}

	if (Plan.Steps.Num() == 0)
	{
		FOliveIRBlueprintPlanError Error = FOliveIRBlueprintPlanError::MakePlanError(
			TEXT("EMPTY_PLAN"),
			TEXT("Plan has no steps to resolve"),
			TEXT("Add at least one step to the plan"));
		Result.Errors.Add(MoveTemp(Error));
		Result.bSuccess = false;
		return Result;
	}

	// ------------------------------------------------------------------
	// Pre-processing: Expand high-level inputs. This mutates the plan,
	// potentially inserting synthetic steps, so we work on a mutable copy.
	// ------------------------------------------------------------------
	FOliveIRBlueprintPlan MutablePlan = Plan;
	TArray<FOliveResolverNote> ExpansionNotes;

	// Pass 1: Expand dotless @refs to component/variable get_var steps (RC2/RC3 fix)
	ExpandComponentRefs(MutablePlan, Blueprint, GraphContext, ExpansionNotes);

	// Pass 2: Expand SpawnActor Location/Rotation -> synthesized MakeTransform step
	ExpandPlanInputs(MutablePlan, ExpansionNotes);

	// Pass 3: Expand branch conditions with non-boolean @refs to > 0 comparisons
	ExpandBranchConditions(MutablePlan, Blueprint, ExpansionNotes);

	// Pass 4: Rewrite C++ accessor calls (GetMesh, etc.) to GetComponentByClass
	RewriteAccessorCalls(MutablePlan, ExpansionNotes);

	Result.GlobalNotes = MoveTemp(ExpansionNotes);

	UE_LOG(LogOlivePlanResolver, Log, TEXT("Resolving plan with %d steps for Blueprint '%s'%s"),
		MutablePlan.Steps.Num(), *Blueprint->GetName(),
		Result.GlobalNotes.Num() > 0
			? *FString::Printf(TEXT(" (%d input expansions applied)"), Result.GlobalNotes.Num())
			: TEXT(""));

	// ------------------------------------------------------------------
	// Pre-scan: build cast target map for cross-step function resolution.
	// When a "call" step references a cast step's output (via @ref in inputs),
	// the call target function should be searched on the cast target class,
	// not just the editing Blueprint's class hierarchy.
	// ------------------------------------------------------------------
	TMap<FString, FString> CastTargetMap;
	for (const FOliveIRBlueprintPlanStep& PreScanStep : MutablePlan.Steps)
	{
		if (PreScanStep.Op == OlivePlanOps::Cast)
		{
			// Cast steps use Target for the class name (e.g., "BP_Gun"),
			// with TargetClass as a fallback field.
			FString CastTarget = PreScanStep.Target.IsEmpty()
				? PreScanStep.TargetClass : PreScanStep.Target;
			if (!CastTarget.IsEmpty())
			{
				CastTargetMap.Add(PreScanStep.StepId, CastTarget);
			}
		}
	}

	if (CastTargetMap.Num() > 0)
	{
		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("  Built CastTargetMap with %d entries for cross-step function resolution"),
			CastTargetMap.Num());
	}

	bool bAllSucceeded = true;

	for (int32 i = 0; i < MutablePlan.Steps.Num(); ++i)
	{
		const FOliveIRBlueprintPlanStep& Step = MutablePlan.Steps[i];
		FOliveResolvedStep Resolved;

		if (ResolveStep(Step, Blueprint, i, Resolved, Result.Errors, Result.Warnings, GraphContext, CastTargetMap))
		{
			Result.ResolvedSteps.Add(MoveTemp(Resolved));
		}
		else
		{
			bAllSucceeded = false;
			// Continue resolving remaining steps to collect all errors
		}
	}

	Result.bSuccess = bAllSucceeded;

	// Carry the mutated plan (with all expansions applied) in the result
	// so callers use it instead of the original pre-expansion plan.
	Result.ExpandedPlan = MoveTemp(MutablePlan);

	UE_LOG(LogOlivePlanResolver, Log, TEXT("Plan resolution %s: %d/%d steps resolved, %d errors, %d warnings"),
		Result.bSuccess ? TEXT("succeeded") : TEXT("failed"),
		Result.ResolvedSteps.Num(),
		Result.ExpandedPlan.Steps.Num(),
		Result.Errors.Num(),
		Result.Warnings.Num());

	return Result;
}

// ============================================================================
// ExpandPlanInputs — Pre-process plan to expand high-level inputs
// ============================================================================

bool FOliveBlueprintPlanResolver::ExpandPlanInputs(
	FOliveIRBlueprintPlan& Plan,
	TArray<FOliveResolverNote>& OutNotes)
{
	bool bExpanded = false;

	for (int32 i = 0; i < Plan.Steps.Num(); ++i)
	{
		FOliveIRBlueprintPlanStep& Step = Plan.Steps[i];

		if (Step.Op != OlivePlanOps::SpawnActor)
		{
			continue;
		}

		// Check for Location or Rotation in inputs
		const bool bHasLocation = Step.Inputs.Contains(TEXT("Location"));
		const bool bHasRotation = Step.Inputs.Contains(TEXT("Rotation"));

		if (!bHasLocation && !bHasRotation)
		{
			continue; // No expansion needed
		}

		// Explicit SpawnTransform takes priority -- do not double-expand.
		// If the AI also specified Location/Rotation alongside SpawnTransform,
		// the Location/Rotation are left as-is (they will fail at wiring time
		// with a clear "no pin named Location" error, which is correct behavior).
		if (Step.Inputs.Contains(TEXT("SpawnTransform")))
		{
			UE_LOG(LogOlivePlanResolver, Verbose,
				TEXT("ExpandPlanInputs: step '%s' has explicit SpawnTransform, skipping Location/Rotation expansion"),
				*Step.StepId);
			continue;
		}

		// Synthesize a MakeTransform step with a _synth_ prefix to avoid
		// collision with human-authored step IDs.
		const FString SyntheticStepId = FString::Printf(TEXT("_synth_maketf_%s"), *Step.StepId);

		FOliveIRBlueprintPlanStep MakeTransformStep;
		MakeTransformStep.StepId = SyntheticStepId;
		MakeTransformStep.Op = OlivePlanOps::MakeStruct;
		MakeTransformStep.Target = TEXT("Transform");

		// Build a human-readable description of what was transferred
		FString OriginalInputDesc;

		// Transfer Location input to the MakeTransform step
		if (bHasLocation)
		{
			const FString LocationValue = Step.Inputs[TEXT("Location")];
			MakeTransformStep.Inputs.Add(TEXT("Location"), LocationValue);
			Step.Inputs.Remove(TEXT("Location"));
			OriginalInputDesc += FString::Printf(TEXT("Location=%s"), *LocationValue);
		}

		// Transfer Rotation input to the MakeTransform step
		if (bHasRotation)
		{
			const FString RotationValue = Step.Inputs[TEXT("Rotation")];
			MakeTransformStep.Inputs.Add(TEXT("Rotation"), RotationValue);
			Step.Inputs.Remove(TEXT("Rotation"));
			if (!OriginalInputDesc.IsEmpty())
			{
				OriginalInputDesc += TEXT(", ");
			}
			OriginalInputDesc += FString::Printf(TEXT("Rotation=%s"), *RotationValue);
		}

		// Wire MakeTransform output -> SpawnActor.SpawnTransform using @step.auto syntax
		Step.Inputs.Add(TEXT("SpawnTransform"),
			FString::Printf(TEXT("@%s.auto"), *SyntheticStepId));

		// Insert the synthetic step BEFORE the spawn step so it is created first
		Plan.Steps.Insert(MakeTransformStep, i);
		++i; // Skip the inserted step in the iteration

		bExpanded = true;

		// Record resolver note for transparency
		FOliveResolverNote Note;
		Note.Field = TEXT("inputs");
		Note.OriginalValue = OriginalInputDesc;
		Note.ResolvedValue = FString::Printf(
			TEXT("Synthesized MakeTransform step '%s' -> SpawnTransform"), *SyntheticStepId);
		Note.Reason = TEXT("SpawnActor requires a Transform pin, not separate Location/Rotation. "
			"Synthesized a MakeTransform node to bridge the gap.");
		OutNotes.Add(MoveTemp(Note));

		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("ExpandPlanInputs: Expanded SpawnActor step '%s' -- synthesized MakeTransform step '%s' (inputs: %s)"),
			*Step.StepId, *SyntheticStepId, *OriginalInputDesc);
	}

	return bExpanded;
}

// ============================================================================
// ExpandBranchConditions — Synthesize > 0 comparisons for non-boolean branch inputs
// ============================================================================

bool FOliveBlueprintPlanResolver::ExpandBranchConditions(
	FOliveIRBlueprintPlan& Plan,
	UBlueprint* Blueprint,
	TArray<FOliveResolverNote>& OutNotes)
{
	if (!Blueprint)
	{
		return false;
	}

	// Build step lookup for source step analysis
	TMap<FString, const FOliveIRBlueprintPlanStep*> StepLookup;
	for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		StepLookup.Add(Step.StepId, &Step);
	}

	bool bExpanded = false;

	for (int32 i = 0; i < Plan.Steps.Num(); ++i)
	{
		FOliveIRBlueprintPlanStep& Step = Plan.Steps[i];

		if (Step.Op != OlivePlanOps::Branch)
		{
			continue;
		}

		// Check the Condition input
		FString* ConditionValue = Step.Inputs.Find(TEXT("Condition"));
		if (!ConditionValue || !ConditionValue->StartsWith(TEXT("@")))
		{
			continue;
		}

		// Parse the @ref
		FString RefBody = ConditionValue->Mid(1);
		int32 DotIdx;
		if (!RefBody.FindChar(TEXT('.'), DotIdx))
		{
			continue; // Bare ref, can't analyze source
		}

		FString SourceStepId = RefBody.Left(DotIdx);
		const FOliveIRBlueprintPlanStep** SourceStepPtr = StepLookup.Find(SourceStepId);
		if (!SourceStepPtr)
		{
			continue; // Source step not found, will fail at wiring time
		}

		const FOliveIRBlueprintPlanStep& SourceStep = **SourceStepPtr;

		// Check if the source step produces a non-boolean output.
		// For get_var: check the variable type on the Blueprint.
		bool bNeedsComparison = false;
		bool bIsFloatType = false;

		if (SourceStep.Op == OlivePlanOps::GetVar)
		{
			// Look up variable type
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				if (Var.VarName.ToString() == SourceStep.Target)
				{
					// Check if the variable is NOT boolean
					const FString Category = Var.VarType.PinCategory.ToString();
					if (Category != TEXT("bool"))
					{
						bNeedsComparison = true;
						// Determine if this is a float/double type for correct comparison function
						// PC_Real is the category for both float and double in UE 5.5
						bIsFloatType = (Category == TEXT("real") || Category == TEXT("double") || Category == TEXT("float"));
					}
					break;
				}
			}
		}

		if (!bNeedsComparison)
		{
			continue;
		}

		// Synthesize a > 0 comparison step
		FString SynthStepId = FString::Printf(TEXT("_synth_cmp_%s"), *Step.StepId);

		// Dispatch to correct comparison function based on variable type
		const FString ComparisonFunction = bIsFloatType
			? TEXT("Greater_DoubleDouble")
			: TEXT("Greater_IntInt");

		FOliveIRBlueprintPlanStep CompareStep;
		CompareStep.StepId = SynthStepId;
		CompareStep.Op = OlivePlanOps::Call;
		CompareStep.Target = ComparisonFunction;
		CompareStep.Inputs.Add(TEXT("A"), *ConditionValue); // Forward the original @ref
		CompareStep.Inputs.Add(TEXT("B"), TEXT("0"));

		// Rewrite the branch's Condition to point at the comparison result
		*ConditionValue = FString::Printf(TEXT("@%s.auto"), *SynthStepId);

		// Insert before the branch step
		Plan.Steps.Insert(CompareStep, i);
		StepLookup.Add(SynthStepId, &Plan.Steps[i]); // Update lookup
		++i; // Skip the inserted step

		bExpanded = true;

		FOliveResolverNote Note;
		Note.Field = FString::Printf(TEXT("step '%s' inputs.Condition"), *Step.StepId);
		Note.OriginalValue = FString::Printf(TEXT("@%s (non-boolean)"), *SourceStepId);
		Note.ResolvedValue = FString::Printf(TEXT("Synthesized %s > 0 comparison step '%s'"), *ComparisonFunction, *SynthStepId);
		Note.Reason = TEXT("Branch Condition requires Boolean. Source provides Integer/Float. Synthesized a > 0 comparison.");
		OutNotes.Add(MoveTemp(Note));

		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("ExpandBranchConditions: Synthesized '%s' step '%s' for branch '%s' (source: '%s')"),
			*ComparisonFunction, *SynthStepId, *Step.StepId, *SourceStepId);
	}

	return bExpanded;
}

// ============================================================================
// RewriteAccessorCalls — Rewrite C++ accessor calls to GetComponentByClass
// ============================================================================

bool FOliveBlueprintPlanResolver::RewriteAccessorCalls(
	FOliveIRBlueprintPlan& Plan,
	TArray<FOliveResolverNote>& OutNotes)
{
	// ACharacter has GetMesh(), GetCapsuleComponent(), GetCharacterMovement()
	// which are plain FORCEINLINE C++ accessors, NOT UFUNCTIONs. FindFunction
	// can't resolve them. Rewrite to GetComponentByClass with the appropriate
	// ComponentClass input, which is a UFUNCTION on AActor.
	struct FAccessorRewrite
	{
		const TCHAR* Accessor;
		const TCHAR* ComponentClass;
	};

	static const FAccessorRewrite Rewrites[] = {
		{ TEXT("GetMesh"),                  TEXT("SkeletalMeshComponent") },
		{ TEXT("GetCapsuleComponent"),       TEXT("CapsuleComponent") },
		{ TEXT("GetCharacterMovement"),      TEXT("CharacterMovementComponent") },
	};

	bool bRewrote = false;

	for (FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		if (Step.Op != OlivePlanOps::Call)
		{
			continue;
		}

		for (const FAccessorRewrite& Rewrite : Rewrites)
		{
			if (Step.Target.Equals(Rewrite.Accessor, ESearchCase::IgnoreCase))
			{
				const FString OldTarget = Step.Target;

				Step.Target = TEXT("GetComponentByClass");
				Step.Inputs.Add(TEXT("ComponentClass"), Rewrite.ComponentClass);

				FOliveResolverNote Note;
				Note.Field = TEXT("target");
				Note.OriginalValue = OldTarget;
				Note.ResolvedValue = FString::Printf(TEXT("GetComponentByClass(ComponentClass=%s)"), Rewrite.ComponentClass);
				Note.Reason = FString::Printf(TEXT("'%s' is a C++ FORCEINLINE accessor, not a UFUNCTION. "
					"Rewritten to GetComponentByClass which is a UFUNCTION on AActor."), Rewrite.Accessor);
				OutNotes.Add(MoveTemp(Note));

				UE_LOG(LogOlivePlanResolver, Log,
					TEXT("RewriteAccessorCalls: step '%s' target '%s' -> GetComponentByClass(ComponentClass=%s)"),
					*Step.StepId, *OldTarget, Rewrite.ComponentClass);

				bRewrote = true;
				break;
			}
		}
	}

	return bRewrote;
}

// ============================================================================
// ExpandComponentRefs — Pre-process dotless @refs to component/variable get_var steps
// ============================================================================

bool FOliveBlueprintPlanResolver::ExpandComponentRefs(
	FOliveIRBlueprintPlan& Plan,
	UBlueprint* Blueprint,
	const FOliveGraphContext& GraphContext,
	TArray<FOliveResolverNote>& OutNotes)
{
	if (!Blueprint)
	{
		return false;
	}

	// Build set of existing step IDs for collision detection
	TSet<FString> ExistingStepIds;
	for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		ExistingStepIds.Add(Step.StepId);
	}

	// Build set of SCS component variable names for fast lookup
	TSet<FString> SCSComponentNames;
	if (Blueprint->SimpleConstructionScript)
	{
		TArray<USCS_Node*> AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
		for (USCS_Node* Node : AllNodes)
		{
			if (Node)
			{
				SCSComponentNames.Add(Node->GetVariableName().ToString());
			}
		}
	}

	// Build set of Blueprint variable names (NewVariables, not SCS)
	// This catches bare @refs like @MuzzlePoint where the variable was added
	// via add_variable (not add_component) and therefore is NOT in SCS.
	TSet<FString> BlueprintVariableNames;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		BlueprintVariableNames.Add(Var.VarName.ToString());
	}

	// Build set of function input parameter names (for function graph context)
	TSet<FString> FunctionInputParams;
	if (GraphContext.bIsFunctionGraph)
	{
		for (const FString& ParamName : GraphContext.InputParamNames)
		{
			FunctionInputParams.Add(ParamName);
		}
	}

	// Track synthesized steps to insert (step, insertion index)
	struct FSyntheticStepInsert
	{
		FOliveIRBlueprintPlanStep Step;
		int32 InsertBeforeIndex;
	};
	TArray<FSyntheticStepInsert> Inserts;

	// Track already-synthesized component get_var steps to avoid duplicates
	// Key: component name -> synthesized step ID
	TMap<FString, FString> SynthesizedComponentSteps;

	bool bExpanded = false;

	for (int32 i = 0; i < Plan.Steps.Num(); ++i)
	{
		FOliveIRBlueprintPlanStep& Step = Plan.Steps[i];

		// Scan inputs for dotless @refs
		TMap<FString, FString> RewrittenInputs;
		for (const auto& InputPair : Step.Inputs)
		{
			const FString& PinName = InputPair.Key;
			const FString& Value = InputPair.Value;

			if (!Value.StartsWith(TEXT("@")))
			{
				continue;
			}

			FString RefBody = Value.Mid(1); // strip @

			// Check if this has a dot -- if so, check if the part before
			// the dot is a function parameter or component name (not a step ID)
			int32 DotIndex = INDEX_NONE;
			RefBody.FindChar(TEXT('.'), DotIndex);

			if (DotIndex != INDEX_NONE)
			{
				// Has a dot -- check if the step ID part matches a function param
				// (RC3: @ParamName.PinHint where ParamName is a function input)
				FString RefStepId = RefBody.Left(DotIndex);

				// If it's already a valid step ID, skip
				if (ExistingStepIds.Contains(RefStepId))
				{
					continue;
				}

				// Handle @entry.X or @GraphName.X -- "entry" and the graph name are
				// aliases for the FunctionEntry node in a function graph. Rewrite to
				// a synthesized get_var step for the referenced parameter.
				if (GraphContext.bIsFunctionGraph
					&& (RefStepId.Equals(TEXT("entry"), ESearchCase::IgnoreCase)
						|| RefStepId.Equals(GraphContext.GraphName, ESearchCase::IgnoreCase)))
				{
					FString PinHint = RefBody.Mid(DotIndex + 1);

					// Resolve the parameter name to canonical casing from GraphContext
					FString ParamTarget = PinHint;
					for (const FString& ParamName : GraphContext.InputParamNames)
					{
						if (ParamName.Equals(PinHint, ESearchCase::IgnoreCase))
						{
							ParamTarget = ParamName; // Use canonical casing
							break;
						}
					}

					// Use a synth key that matches what bare @ParamName uses, so that
					// @entry.X and bare @X referencing the same param share one synth step.
					FString SynthKey = ParamTarget;
					FString* ExistingSynthId = SynthesizedComponentSteps.Find(SynthKey);
					if (!ExistingSynthId)
					{
						FString SynthStepId = FString::Printf(TEXT("_synth_param_%s"), *ParamTarget.ToLower());

						// Check if this synth step already exists (e.g., from a bare @ParamName reference)
						if (!ExistingStepIds.Contains(SynthStepId))
						{
							FOliveIRBlueprintPlanStep SynthStep;
							SynthStep.StepId = SynthStepId;
							SynthStep.Op = OlivePlanOps::GetVar;
							SynthStep.Target = ParamTarget;

							Inserts.Add({ MoveTemp(SynthStep), i });
							ExistingStepIds.Add(SynthStepId);
						}

						SynthesizedComponentSteps.Add(SynthKey, SynthStepId);

						FOliveResolverNote Note;
						Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
						Note.OriginalValue = Value;
						Note.ResolvedValue = FString::Printf(
							TEXT("Rewritten @%s.%s -> @_synth_param_%s.auto (FunctionInput step)"),
							*RefStepId, *PinHint, *ParamTarget.ToLower());
						Note.Reason = TEXT("@entry is an alias for the function entry node. "
							"Synthesized a get_var step for the parameter.");
						OutNotes.Add(MoveTemp(Note));

						UE_LOG(LogOlivePlanResolver, Log,
							TEXT("ExpandComponentRefs: Synthesized get_var step '_synth_param_%s' for @%s.%s alias (referenced by step '%s')"),
							*ParamTarget.ToLower(), *RefStepId, *PinHint, *Step.StepId);
					}

					FString SynthId = SynthesizedComponentSteps[SynthKey];
					RewrittenInputs.Add(PinName, FString::Printf(TEXT("@%s.auto"), *SynthId));
					bExpanded = true;
					continue;
				}

				// Check if it's a function input parameter
				if (FunctionInputParams.Contains(RefStepId))
				{
					// Check if we already synthesized a FunctionInput step for this param
					FString* ExistingSynthId = SynthesizedComponentSteps.Find(RefStepId);
					if (!ExistingSynthId)
					{
						// Synthesize a get_var step that will be resolved to FunctionInput
						FString SynthStepId = FString::Printf(TEXT("_synth_param_%s"), *RefStepId.ToLower());

						FOliveIRBlueprintPlanStep SynthStep;
						SynthStep.StepId = SynthStepId;
						SynthStep.Op = OlivePlanOps::GetVar;
						SynthStep.Target = RefStepId;

						Inserts.Add({ MoveTemp(SynthStep), i });
						SynthesizedComponentSteps.Add(RefStepId, SynthStepId);
						ExistingStepIds.Add(SynthStepId);

						FOliveResolverNote Note;
						Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
						Note.OriginalValue = Value;
						Note.ResolvedValue = FString::Printf(TEXT("Synthesized get_var step '%s' for function parameter '%s'"), *SynthStepId, *RefStepId);
						Note.Reason = TEXT("@ref referenced a function parameter name, not a step_id. Synthesized a get_var step to access the parameter.");
						OutNotes.Add(MoveTemp(Note));

						UE_LOG(LogOlivePlanResolver, Log,
							TEXT("ExpandComponentRefs: Synthesized get_var step '%s' for function param '%s' (referenced by step '%s')"),
							*SynthStepId, *RefStepId, *Step.StepId);
					}

					// Rewrite the @ref to point at the synthesized step
					FString PinHint = RefBody.Mid(DotIndex + 1);
					FString SynthId = SynthesizedComponentSteps[RefStepId];
					RewrittenInputs.Add(PinName, FString::Printf(TEXT("@%s.%s"), *SynthId, *PinHint));
					bExpanded = true;
					continue;
				}

				// Check if it's a component name with a dot (e.g., @Mesh.auto)
				if (SCSComponentNames.Contains(RefStepId))
				{
					FString* ExistingSynthId = SynthesizedComponentSteps.Find(RefStepId);
					if (!ExistingSynthId)
					{
						FString SynthStepId = FString::Printf(TEXT("_synth_getcomp_%s"), *RefStepId.ToLower());

						FOliveIRBlueprintPlanStep SynthStep;
						SynthStep.StepId = SynthStepId;
						SynthStep.Op = OlivePlanOps::GetVar;
						SynthStep.Target = RefStepId;

						Inserts.Add({ MoveTemp(SynthStep), i });
						SynthesizedComponentSteps.Add(RefStepId, SynthStepId);
						ExistingStepIds.Add(SynthStepId);

						FOliveResolverNote Note;
						Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
						Note.OriginalValue = Value;
						Note.ResolvedValue = FString::Printf(TEXT("Synthesized get_var step '%s' for component '%s'"), *SynthStepId, *RefStepId);
						Note.Reason = TEXT("@ref referenced a component name, not a step_id. Synthesized a get_var step to access the component.");
						OutNotes.Add(MoveTemp(Note));

						UE_LOG(LogOlivePlanResolver, Log,
							TEXT("ExpandComponentRefs: Synthesized get_var step '%s' for component '%s' (referenced by step '%s')"),
							*SynthStepId, *RefStepId, *Step.StepId);
					}

					FString PinHint = RefBody.Mid(DotIndex + 1);
					FString SynthId = SynthesizedComponentSteps[RefStepId];
					RewrittenInputs.Add(PinName, FString::Printf(TEXT("@%s.%s"), *SynthId, *PinHint));
					bExpanded = true;
					continue;
				}

				// Check if it's a Blueprint variable with a dot (e.g., @MuzzlePoint.WorldLocation)
				if (BlueprintVariableNames.Contains(RefStepId))
				{
					FString* ExistingSynthId = SynthesizedComponentSteps.Find(RefStepId);
					if (!ExistingSynthId)
					{
						FString SynthStepId = FString::Printf(TEXT("_synth_getvar_%s"), *RefStepId.ToLower());

						FOliveIRBlueprintPlanStep SynthStep;
						SynthStep.StepId = SynthStepId;
						SynthStep.Op = OlivePlanOps::GetVar;
						SynthStep.Target = RefStepId;

						Inserts.Add({ MoveTemp(SynthStep), i });
						SynthesizedComponentSteps.Add(RefStepId, SynthStepId);
						ExistingStepIds.Add(SynthStepId);

						FOliveResolverNote Note;
						Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
						Note.OriginalValue = Value;
						Note.ResolvedValue = FString::Printf(TEXT("Synthesized get_var step '%s' for variable '%s'"), *SynthStepId, *RefStepId);
						Note.Reason = TEXT("@ref referenced a Blueprint variable name, not a step_id. Synthesized a get_var step to access the variable.");
						OutNotes.Add(MoveTemp(Note));

						UE_LOG(LogOlivePlanResolver, Log,
							TEXT("ExpandComponentRefs: Synthesized get_var step '%s' for variable '%s' (referenced by step '%s')"),
							*SynthStepId, *RefStepId, *Step.StepId);
					}

					FString PinHint = RefBody.Mid(DotIndex + 1);
					FString SynthId = SynthesizedComponentSteps[RefStepId];
					RewrittenInputs.Add(PinName, FString::Printf(TEXT("@%s.%s"), *SynthId, *PinHint));
					bExpanded = true;
					continue;
				}
			}
			else
			{
				// No dot -- this is a bare @ComponentName (e.g., "@InteractionSphere")
				// Check if it matches an SCS component
				if (SCSComponentNames.Contains(RefBody))
				{
					FString* ExistingSynthId = SynthesizedComponentSteps.Find(RefBody);
					if (!ExistingSynthId)
					{
						FString SynthStepId = FString::Printf(TEXT("_synth_getcomp_%s"), *RefBody.ToLower());

						FOliveIRBlueprintPlanStep SynthStep;
						SynthStep.StepId = SynthStepId;
						SynthStep.Op = OlivePlanOps::GetVar;
						SynthStep.Target = RefBody;

						Inserts.Add({ MoveTemp(SynthStep), i });
						SynthesizedComponentSteps.Add(RefBody, SynthStepId);
						ExistingStepIds.Add(SynthStepId);

						FOliveResolverNote Note;
						Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
						Note.OriginalValue = Value;
						Note.ResolvedValue = FString::Printf(TEXT("Synthesized get_var step '%s' for component '%s'"), *SynthStepId, *RefBody);
						Note.Reason = TEXT("Bare @ref with no dot referenced a component name. Synthesized a get_var step and used .auto pin matching.");
						OutNotes.Add(MoveTemp(Note));

						UE_LOG(LogOlivePlanResolver, Log,
							TEXT("ExpandComponentRefs: Synthesized get_var step '%s' for bare component ref '@%s' (referenced by step '%s')"),
							*SynthStepId, *RefBody, *Step.StepId);
					}

					// Rewrite to @synthStep.auto (type-match the output)
					FString SynthId = SynthesizedComponentSteps[RefBody];
					RewrittenInputs.Add(PinName, FString::Printf(TEXT("@%s.auto"), *SynthId));
					bExpanded = true;
					continue;
				}

				// Check Blueprint variables (for bare @VarName refs like @MuzzlePoint)
				if (BlueprintVariableNames.Contains(RefBody))
				{
					FString* ExistingSynthId = SynthesizedComponentSteps.Find(RefBody);
					if (!ExistingSynthId)
					{
						FString SynthStepId = FString::Printf(TEXT("_synth_getvar_%s"), *RefBody.ToLower());

						FOliveIRBlueprintPlanStep SynthStep;
						SynthStep.StepId = SynthStepId;
						SynthStep.Op = OlivePlanOps::GetVar;
						SynthStep.Target = RefBody;

						Inserts.Add({ MoveTemp(SynthStep), i });
						SynthesizedComponentSteps.Add(RefBody, SynthStepId);
						ExistingStepIds.Add(SynthStepId);

						FOliveResolverNote Note;
						Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
						Note.OriginalValue = Value;
						Note.ResolvedValue = FString::Printf(TEXT("Synthesized get_var step '%s' for variable '%s'"), *SynthStepId, *RefBody);
						Note.Reason = TEXT("Bare @ref with no dot referenced a Blueprint variable name. Synthesized a get_var step and used .auto pin matching.");
						OutNotes.Add(MoveTemp(Note));

						UE_LOG(LogOlivePlanResolver, Log,
							TEXT("ExpandComponentRefs: Synthesized get_var step '%s' for bare variable ref '@%s' (referenced by step '%s')"),
							*SynthStepId, *RefBody, *Step.StepId);
					}

					FString SynthId = SynthesizedComponentSteps[RefBody];
					RewrittenInputs.Add(PinName, FString::Printf(TEXT("@%s.auto"), *SynthId));
					bExpanded = true;
					continue;
				}

				// Check if it's a function input parameter (bare, no dot)
				if (FunctionInputParams.Contains(RefBody))
				{
					FString* ExistingSynthId = SynthesizedComponentSteps.Find(RefBody);
					if (!ExistingSynthId)
					{
						FString SynthStepId = FString::Printf(TEXT("_synth_param_%s"), *RefBody.ToLower());

						FOliveIRBlueprintPlanStep SynthStep;
						SynthStep.StepId = SynthStepId;
						SynthStep.Op = OlivePlanOps::GetVar;
						SynthStep.Target = RefBody;

						Inserts.Add({ MoveTemp(SynthStep), i });
						SynthesizedComponentSteps.Add(RefBody, SynthStepId);
						ExistingStepIds.Add(SynthStepId);

						FOliveResolverNote Note;
						Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
						Note.OriginalValue = Value;
						Note.ResolvedValue = FString::Printf(TEXT("Synthesized get_var step '%s' for function param '%s'"), *SynthStepId, *RefBody);
						Note.Reason = TEXT("Bare @ref referenced a function parameter name. Synthesized a get_var step.");
						OutNotes.Add(MoveTemp(Note));
					}

					FString SynthId = SynthesizedComponentSteps[RefBody];
					RewrittenInputs.Add(PinName, FString::Printf(TEXT("@%s.auto"), *SynthId));
					bExpanded = true;
					continue;
				}
			}
		}

		// Apply rewrites
		for (const auto& Rewrite : RewrittenInputs)
		{
			Step.Inputs[Rewrite.Key] = Rewrite.Value;
		}
	}

	// Insert synthesized steps (in reverse order to preserve indices)
	// Sort inserts by insertion index descending so earlier inserts don't shift later ones
	Inserts.Sort([](const FSyntheticStepInsert& A, const FSyntheticStepInsert& B)
	{
		return A.InsertBeforeIndex > B.InsertBeforeIndex;
	});

	for (const FSyntheticStepInsert& Insert : Inserts)
	{
		Plan.Steps.Insert(Insert.Step, Insert.InsertBeforeIndex);
	}

	if (bExpanded)
	{
		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("ExpandComponentRefs: Expanded %d component/param references, inserted %d synthetic steps"),
			OutNotes.Num(), Inserts.Num());
	}

	return bExpanded;
}

// ============================================================================
// ResolveStep — dispatcher
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveStep(
	const FOliveIRBlueprintPlanStep& Step,
	UBlueprint* Blueprint,
	int32 StepIndex,
	FOliveResolvedStep& OutResolved,
	TArray<FOliveIRBlueprintPlanError>& OutErrors,
	TArray<FString>& OutWarnings,
	const FOliveGraphContext& GraphContext,
	const TMap<FString, FString>& CastTargetMap)
{
	OutResolved.StepId = Step.StepId;

	// Remap "entry" alias to "event" -- the AI may use "entry" to mean
	// "the entry point of this graph". Not in the OlivePlanOps vocabulary,
	// so we silently remap here for robustness.
	FString EffectiveOp = Step.Op;
	if (EffectiveOp == TEXT("entry"))
	{
		EffectiveOp = OlivePlanOps::Event;

		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("  Step '%s': remapped op 'entry' -> 'event' (alias)"),
			*Step.StepId);
	}

	const FString& Op = EffectiveOp;

	UE_LOG(LogOlivePlanResolver, Log,
		TEXT("  Resolving step %d: step_id='%s', op='%s', target='%s'"),
		StepIndex, *Step.StepId, *Op, *Step.Target);

	bool bResult = false;

	if (Op == OlivePlanOps::Call)
	{
		bResult = ResolveCallOp(Step, Blueprint, StepIndex, OutResolved, OutErrors, OutWarnings, CastTargetMap);
	}
	else if (Op == OlivePlanOps::GetVar)
	{
		bResult = ResolveGetVarOp(Step, Blueprint, StepIndex, OutResolved, OutErrors, OutWarnings, GraphContext);
	}
	else if (Op == OlivePlanOps::SetVar)
	{
		bResult = ResolveSetVarOp(Step, Blueprint, StepIndex, OutResolved, OutErrors, OutWarnings, GraphContext);
	}
	else if (Op == OlivePlanOps::Event)
	{
		// In function graphs, "event" targeting the graph name (or generic names like "entry")
		// maps to the FunctionEntry node, not a Blueprint event node.
		if (GraphContext.bIsFunctionGraph)
		{
			// Check if the target matches the function name or is a generic entry alias
			const bool bTargetsFunction = Step.Target.Equals(GraphContext.GraphName, ESearchCase::IgnoreCase)
				|| Step.Target.Equals(TEXT("entry"), ESearchCase::IgnoreCase)
				|| Step.Target.Equals(TEXT("Entry"), ESearchCase::IgnoreCase)
				|| Step.Target.IsEmpty();

			if (bTargetsFunction)
			{
				bResult = ResolveSimpleOp(Step, OliveNodeTypes::FunctionInput, OutResolved);
				if (bResult)
				{
					OutResolved.Properties.Add(TEXT("function_name"), GraphContext.GraphName);

					FOliveResolverNote Note;
					Note.Field = TEXT("op");
					Note.OriginalValue = FString::Printf(TEXT("event:%s"), *Step.Target);
					Note.ResolvedValue = TEXT("FunctionInput");
					Note.Reason = FString::Printf(
						TEXT("Graph '%s' is a function graph. Mapped event op to FunctionEntry node."),
						*GraphContext.GraphName);
					OutResolved.ResolverNotes.Add(MoveTemp(Note));
				}
			}
			else
			{
				// Target doesn't match the function name -- fall through to normal event resolution
				// (could be a component delegate event in a function graph, though unusual)
				bResult = ResolveEventOp(Step, Blueprint, StepIndex, OutResolved, OutErrors);
			}
		}
		else
		{
			bResult = ResolveEventOp(Step, Blueprint, StepIndex, OutResolved, OutErrors);
		}
	}
	else if (Op == OlivePlanOps::CustomEvent)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::CustomEvent, OutResolved);
		if (bResult)
		{
			OutResolved.Properties.Add(TEXT("event_name"), Step.Target);
		}
	}
	else if (Op == OlivePlanOps::Branch)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::Branch, OutResolved);
	}
	else if (Op == OlivePlanOps::Sequence)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::Sequence, OutResolved);
	}
	else if (Op == OlivePlanOps::ForLoop)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::ForLoop, OutResolved);
	}
	else if (Op == OlivePlanOps::ForEachLoop)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::ForEachLoop, OutResolved);
	}
	else if (Op == OlivePlanOps::WhileLoop)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::WhileLoop, OutResolved);
	}
	else if (Op == OlivePlanOps::DoOnce)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::DoOnce, OutResolved);
	}
	else if (Op == OlivePlanOps::FlipFlop)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::FlipFlop, OutResolved);
	}
	else if (Op == OlivePlanOps::Gate)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::Gate, OutResolved);
	}
	else if (Op == OlivePlanOps::Delay)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::Delay, OutResolved);
		if (bResult)
		{
			OutResolved.bIsLatent = true;
		}
	}
	else if (Op == OlivePlanOps::IsValid)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::IsValid, OutResolved);
	}
	else if (Op == OlivePlanOps::PrintString)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::PrintString, OutResolved);
	}
	else if (Op == OlivePlanOps::SpawnActor)
	{
		UE_LOG(LogOlivePlanResolver, Log, TEXT("    ResolveSpawnActorOp: target='%s'"), *Step.Target);
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::SpawnActor, OutResolved);
		if (bResult)
		{
			OutResolved.Properties.Add(TEXT("actor_class"), Step.Target);
			UE_LOG(LogOlivePlanResolver, Log, TEXT("    ResolveSpawnActorOp: actor_class='%s' resolved successfully"), *Step.Target);
		}
	}
	else if (Op == OlivePlanOps::Cast)
	{
		bResult = ResolveCastOp(Step, StepIndex, OutResolved, OutErrors);
	}
	else if (Op == OlivePlanOps::MakeStruct)
	{
		bResult = ResolveStructOp(Step, OliveNodeTypes::MakeStruct, StepIndex, OutResolved, OutErrors);
	}
	else if (Op == OlivePlanOps::BreakStruct)
	{
		bResult = ResolveStructOp(Step, OliveNodeTypes::BreakStruct, StepIndex, OutResolved, OutErrors);
	}
	else if (Op == OlivePlanOps::Return)
	{
		// Map to the existing UK2Node_FunctionResult node.
		// The step's inputs will wire data to the FunctionResult's input pins.
		// exec_after wires the exec chain to the FunctionResult.
		OutResolved.StepId = Step.StepId;
		OutResolved.NodeType = OliveNodeTypes::FunctionOutput;
		OutResolved.bIsPure = false; // FunctionResult has exec input
		OutResolved.ResolverNotes.Add(FOliveResolverNote{
			TEXT("op"),
			TEXT("return"),
			TEXT("FunctionOutput"),
			TEXT("return op maps to existing FunctionResult node — inputs wire to output parameter pins")
		});
		bResult = true;
	}
	else if (Op == OlivePlanOps::Comment)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::Comment, OutResolved);
		if (bResult)
		{
			OutResolved.Properties.Add(TEXT("text"), Step.Target);
		}
	}
	else if (Op == OlivePlanOps::CallDelegate || Op == OlivePlanOps::CallDispatcher)
	{
		bResult = ResolveCallDelegateOp(Step, Blueprint, StepIndex, OutResolved, OutErrors);
	}
	else if (Op == OlivePlanOps::BindDispatcher)
	{
		bResult = ResolveBindDelegateOp(Step, Blueprint, StepIndex, OutResolved, OutErrors);
	}
	else
	{
		// Unknown op — should have been caught by schema validation, but handle gracefully
		FOliveIRBlueprintPlanError Error = FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("UNKNOWN_OP"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/op"), StepIndex),
			FString::Printf(TEXT("Unknown operation '%s'"), *Op),
			TEXT("Use one of the recognized ops: call, get_var, set_var, branch, sequence, event, custom_event, for_loop, for_each_loop, while_loop, do_once, flip_flop, gate, delay, is_valid, print_string, spawn_actor, cast, make_struct, break_struct, return, comment, call_delegate, call_dispatcher, bind_dispatcher"));
		OutErrors.Add(MoveTemp(Error));
		return false;
	}

	// Merge any user-specified properties from the step (without overwriting resolver-set ones)
	if (bResult)
	{
		MergeStepProperties(Step, OutResolved.Properties);

		// Set purity flag for non-call ops. For 'call' ops, ResolveCallOp already
		// sets bIsPure from the UFunction's FUNC_BlueprintPure flag.
		if (Op != OlivePlanOps::Call)
		{
			OutResolved.bIsPure = (Op == OlivePlanOps::GetVar
				|| Op == OlivePlanOps::MakeStruct
				|| Op == OlivePlanOps::BreakStruct
				|| Op == OlivePlanOps::IsValid
				|| Op == OlivePlanOps::Comment);
		}
	}

	return bResult;
}

// ============================================================================
// ResolveCallOp
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveCallOp(
	const FOliveIRBlueprintPlanStep& Step,
	UBlueprint* BP,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors,
	TArray<FString>& Warnings,
	const TMap<FString, FString>& CastTargetMap)
{
	Out.NodeType = OliveNodeTypes::CallFunction;

	UE_LOG(LogOlivePlanResolver, Verbose, TEXT("    ResolveCallOp: target='%s'"), *Step.Target);

	if (Step.Target.IsEmpty())
	{
		Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("MISSING_TARGET"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			TEXT("'call' op requires a 'target' specifying the function name"),
			TEXT("Set 'target' to the function name (e.g., \"PrintString\", \"SetActorLocation\")")));
		return false;
	}

	// --- Direct function resolution via FOliveNodeFactory::FindFunctionEx ---
	// FindFunctionEx handles: alias lookup, K2 prefix, specified class, GeneratedClass,
	// parent class hierarchy, SCS component classes, implemented interfaces,
	// and common library classes. On failure it also collects search history.
	FOliveFunctionSearchResult SearchResult = FOliveNodeFactory::Get().FindFunctionEx(Step.Target, Step.TargetClass, BP);

	if (SearchResult.IsValid())
	{
		UFunction* Function = SearchResult.Function;
		EOliveFunctionMatchMethod MatchMethod = SearchResult.MatchMethod;

		// Found a concrete UFunction* -- use its canonical name and owning class
		const FString ResolvedFunctionName = Function->GetName();
		UClass* OwningClass = Function->GetOwnerClass();
		const FString ResolvedClassName = OwningClass ? OwningClass->GetName() : FString();

		Out.Properties.Add(TEXT("function_name"), ResolvedFunctionName);
		if (!ResolvedClassName.IsEmpty())
		{
			Out.Properties.Add(TEXT("target_class"), ResolvedClassName);
		}
		Out.ResolvedOwningClass = OwningClass;
		Out.bIsPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
		Out.bIsLatent = Function->HasMetaData(TEXT("Latent"));

		// Mark interface calls so the executor creates UK2Node_Message
		if (MatchMethod == EOliveFunctionMatchMethod::InterfaceSearch)
		{
			Out.bIsInterfaceCall = true;
			Out.Properties.Add(TEXT("is_interface_call"), TEXT("true"));

			Out.ResolverNotes.Add(FOliveResolverNote{
				TEXT("match_method"),
				TEXT("standard_call"),
				TEXT("interface_message"),
				FString::Printf(TEXT("Function '%s' found on implemented interface '%s' -- will create interface message call node"),
					*ResolvedFunctionName, *ResolvedClassName)
			});

			// Interface message calls are never pure (UK2Node_Message::IsNodePure returns false)
			Out.bIsPure = false;
		}

		// Emit a resolver note if the canonical name differs from what the AI provided
		if (!ResolvedFunctionName.Equals(Step.Target, ESearchCase::IgnoreCase))
		{
			Out.ResolverNotes.Add(FOliveResolverNote{
				TEXT("target"),
				Step.Target,
				FString::Printf(TEXT("%s::%s"), *ResolvedClassName, *ResolvedFunctionName),
				TEXT("Function name resolved to canonical UE name")
			});

			Warnings.Add(FString::Printf(
				TEXT("Step '%s': '%s' resolved to '%s::%s'"),
				*Step.StepId, *Step.Target,
				*ResolvedClassName, *ResolvedFunctionName));
		}

		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("    ResolveCallOp: '%s' -> function_name='%s', target_class='%s'"),
			*Step.Target, *ResolvedFunctionName, *ResolvedClassName);

		return true;
	}

	// --- Function NOT found -- check if target is an event dispatcher before erroring ---
	// If the AI used "call" but the target is actually an event dispatcher, silently
	// reroute to call_delegate. This is zero-cost on the happy path (only runs after
	// FindFunctionEx fails) and avoids a confusing FUNCTION_NOT_FOUND for dispatchers.
	if (BP)
	{
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				const FString VarName = Var.VarName.ToString();
				if (VarName == Step.Target || VarName.Equals(Step.Target, ESearchCase::IgnoreCase))
				{
					UE_LOG(LogOlivePlanResolver, Log,
						TEXT("    ResolveCallOp step '%s': '%s' not found as function, matches event dispatcher '%s'. Rerouting to call_delegate."),
						*Step.StepId, *Step.Target, *VarName);

					// Add resolver note for transparency
					Out.ResolverNotes.Add(FOliveResolverNote{
						TEXT("op"),
						TEXT("call"),
						TEXT("call_delegate"),
						FString::Printf(TEXT("'%s' is an event dispatcher, not a function"), *Step.Target)
					});

					return ResolveCallDelegateOp(Step, BP, Idx, Out, Errors);
				}
			}
		}
	}

	// --- Function NOT found -- check if an input references a cast step ---
	// When the AI writes a plan like: cast to BP_Gun -> call Interact with
	// self from cast output, the function "Interact" exists on BP_Gun (via
	// its interface), not on the editing Blueprint. We scan the step's inputs
	// for @refs that point to cast steps and search the cast target class.
	if (CastTargetMap.Num() > 0)
	{
		for (const auto& InputPair : Step.Inputs)
		{
			const FString& InputValue = InputPair.Value;
			if (!InputValue.StartsWith(TEXT("@")))
			{
				continue;
			}

			// Parse the @ref to extract the source step ID.
			// Format: "@step_id" or "@step_id.pin_hint"
			FString RefBody = InputValue.Mid(1);
			FString RefStepId;
			int32 DotIdx;
			if (RefBody.FindChar(TEXT('.'), DotIdx))
			{
				RefStepId = RefBody.Left(DotIdx);
			}
			else
			{
				RefStepId = RefBody;
			}

			// Check if the referenced step is a cast op
			const FString* CastClassName = CastTargetMap.Find(RefStepId);
			if (!CastClassName || CastClassName->IsEmpty())
			{
				continue;
			}

			// Resolve the cast target class name to a UClass*
			FOliveClassResolveResult ClassResolve = FOliveClassResolver::Resolve(*CastClassName);
			if (!ClassResolve.IsValid())
			{
				UE_LOG(LogOlivePlanResolver, Warning,
					TEXT("    ResolveCallOp: cast target class '%s' (from step '%s') could not be resolved -- skipping cast-target fallback"),
					**CastClassName, *RefStepId);
				continue;
			}

			// Search the cast target class for the function
			FOliveFunctionSearchResult CastSearchResult =
				FOliveNodeFactory::Get().FindFunctionEx(
					Step.Target, ClassResolve.Class->GetName(), nullptr);

			if (CastSearchResult.IsValid())
			{
				// Found the function on the cast target class!
				UFunction* Function = CastSearchResult.Function;
				const FString ResolvedFunctionName = Function->GetName();
				UClass* OwningClass = Function->GetOwnerClass();
				const FString ResolvedClassName = OwningClass ? OwningClass->GetName() : FString();

				Out.Properties.Add(TEXT("function_name"), ResolvedFunctionName);
				if (!ResolvedClassName.IsEmpty())
				{
					Out.Properties.Add(TEXT("target_class"), ResolvedClassName);
				}
				Out.ResolvedOwningClass = OwningClass;
				Out.bIsPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
				Out.bIsLatent = Function->HasMetaData(TEXT("Latent"));

				// Mark interface calls so the executor creates UK2Node_Message
				if (CastSearchResult.MatchMethod == EOliveFunctionMatchMethod::InterfaceSearch)
				{
					Out.bIsInterfaceCall = true;
					Out.Properties.Add(TEXT("is_interface_call"), TEXT("true"));
				}

				Out.ResolverNotes.Add(FOliveResolverNote{
					TEXT("search_scope"),
					TEXT("editing_blueprint"),
					FString::Printf(TEXT("cast_target:%s"), **CastClassName),
					FString::Printf(TEXT("Function '%s' found on cast target class '%s' (from step '%s' via input '%s')"),
						*ResolvedFunctionName, **CastClassName, *RefStepId, *InputPair.Key)
				});

				// Emit note if canonical name differs from AI-provided name
				if (!ResolvedFunctionName.Equals(Step.Target, ESearchCase::IgnoreCase))
				{
					Out.ResolverNotes.Add(FOliveResolverNote{
						TEXT("target"),
						Step.Target,
						FString::Printf(TEXT("%s::%s"), *ResolvedClassName, *ResolvedFunctionName),
						TEXT("Function name resolved to canonical UE name via cast target class")
					});
				}

				UE_LOG(LogOlivePlanResolver, Log,
					TEXT("    ResolveCallOp: '%s' -> function_name='%s', target_class='%s' (via cast target '%s' from step '%s')"),
					*Step.Target, *ResolvedFunctionName, *ResolvedClassName, **CastClassName, *RefStepId);

				return true;
			}
		}
	}

	// --- Function NOT found -- error with search history and suggestions ---
	TArray<FString> Alternatives;

	// Use catalog fuzzy match for "did you mean?" suggestions
	FOliveNodeCatalog& Catalog = FOliveNodeCatalog::Get();
	if (Catalog.IsInitialized())
	{
		TArray<FOliveNodeSuggestion> Suggestions = Catalog.FuzzyMatch(Step.Target, CATALOG_SEARCH_LIMIT);
		for (const FOliveNodeSuggestion& Suggestion : Suggestions)
		{
			Alternatives.Add(Suggestion.DisplayName);
		}
	}

	// Build detailed error message including search trail
	FString ErrorMessage = FString::Printf(
		TEXT("Function '%s' not found. Searched: %s."),
		*Step.Target, *SearchResult.BuildSearchedLocationsString());

	// Build actionable suggestion
	FString Suggestion;
	if (Alternatives.Num() > 0)
	{
		Suggestion = FString::Printf(
			TEXT("Did you mean: %s."),
			*FString::Join(Alternatives, TEXT(", ")));
	}

	if (!Step.TargetClass.IsEmpty())
	{
		Suggestion += Suggestion.IsEmpty() ? TEXT("") : TEXT(" ");
		Suggestion += FString::Printf(
			TEXT("Check if '%s' is the correct target_class, or omit it to search all scopes."),
			*Step.TargetClass);
	}
	else
	{
		Suggestion += Suggestion.IsEmpty() ? TEXT("") : TEXT(" ");
		Suggestion += TEXT("Specify target_class to narrow the search, or use blueprint.search_nodes to find available functions.");
	}

	FOliveIRBlueprintPlanError Error = FOliveIRBlueprintPlanError::MakeStepError(
		TEXT("FUNCTION_NOT_FOUND"),
		Step.StepId,
		FString::Printf(TEXT("/steps/%d/target"), Idx),
		ErrorMessage,
		Suggestion);
	Error.Alternatives = MoveTemp(Alternatives);
	Errors.Add(MoveTemp(Error));

	UE_LOG(LogOlivePlanResolver, Warning,
		TEXT("    ResolveCallOp FAILED: function '%s' could not be resolved (target_class='%s'). Searched: %s"),
		*Step.Target, *Step.TargetClass, *SearchResult.BuildSearchedLocationsString());

	return false;
}

// ============================================================================
// ResolveGetVarOp
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveGetVarOp(
	const FOliveIRBlueprintPlanStep& Step,
	UBlueprint* BP,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors,
	TArray<FString>& Warnings,
	const FOliveGraphContext& GraphContext)
{
	Out.NodeType = OliveNodeTypes::GetVariable;

	UE_LOG(LogOlivePlanResolver, Verbose, TEXT("    ResolveGetVarOp: variable='%s'"), *Step.Target);

	// If we're in a function graph, check if target matches a function input parameter
	if (GraphContext.bIsFunctionGraph && !Step.Target.IsEmpty())
	{
		for (const FString& ParamName : GraphContext.InputParamNames)
		{
			if (ParamName.Equals(Step.Target, ESearchCase::IgnoreCase))
			{
				// This is a function input parameter, not a class variable
				Out.StepId = Step.StepId;
				Out.NodeType = OliveNodeTypes::FunctionInput;
				Out.Properties.Add(TEXT("param_name"), ParamName);
				Out.bIsPure = true;

				Out.ResolverNotes.Add(FOliveResolverNote{
					TEXT("target"),
					Step.Target,
					FString::Printf(TEXT("FunctionInput(%s)"), *ParamName),
					TEXT("Matched function input parameter -- will map to FunctionEntry output pin")
				});

				UE_LOG(LogOlivePlanResolver, Log,
					TEXT("    ResolveGetVarOp: '%s' matched function input param '%s'"),
					*Step.Target, *ParamName);
				return true;
			}
		}
	}

	if (Step.Target.IsEmpty())
	{
		Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("MISSING_TARGET"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			TEXT("'get_var' op requires a 'target' specifying the variable name"),
			TEXT("Set 'target' to the Blueprint variable name")));
		return false;
	}

	Out.Properties.Add(TEXT("variable_name"), Step.Target);

	// Warn if variable not found on Blueprint directly (may be inherited)
	if (BP && !BlueprintHasVariable(BP, Step.Target))
	{
		// Check parent Blueprint chain
		UBlueprint* ParentBP = BP->ParentClass ? Cast<UBlueprint>(BP->ParentClass->ClassGeneratedBy) : nullptr;
		bool bFoundInParent = false;
		while (ParentBP)
		{
			if (BlueprintHasVariable(ParentBP, Step.Target))
			{
				bFoundInParent = true;
				break;
			}
			ParentBP = ParentBP->ParentClass ? Cast<UBlueprint>(ParentBP->ParentClass->ClassGeneratedBy) : nullptr;
		}

		if (!bFoundInParent)
		{
			// Check native C++ properties on the generated class
			// (catches variables inherited from native parents, e.g., AActor::bHidden)
			bool bFoundOnGeneratedClass = false;
			if (BP->GeneratedClass)
			{
				FProperty* Prop = BP->GeneratedClass->FindPropertyByName(FName(*Step.Target));
				bFoundOnGeneratedClass = (Prop != nullptr);
			}

			if (bFoundOnGeneratedClass)
			{
				UE_LOG(LogOlivePlanResolver, Verbose,
					TEXT("Step '%s': Variable '%s' found on generated class (native property)"),
					*Step.StepId, *Step.Target);
			}
			else
			{
				// Variable not found anywhere. Emit a warning (not error) because
				// another step in the plan may create it, or the generated class
				// may not be fully compiled yet. The node factory will still attempt
				// creation via SetSelfMember which may succeed at compile time.
				Warnings.Add(FString::Printf(
					TEXT("Step '%s': Variable '%s' not found on Blueprint '%s'. "
						 "If this is a typo, the node will fail at compile. "
						 "Components use their variable name from the Components panel."),
					*Step.StepId, *Step.Target, *BP->GetName()));

				UE_LOG(LogOlivePlanResolver, Warning,
					TEXT("Step '%s': Variable '%s' not found on Blueprint '%s' or parents or generated class"),
					*Step.StepId, *Step.Target, *BP->GetName());
			}
		}
		else
		{
			UE_LOG(LogOlivePlanResolver, Verbose, TEXT("    Variable '%s' found on Blueprint (inherited)"), *Step.Target);
		}
	}
	else if (BP)
	{
		UE_LOG(LogOlivePlanResolver, Verbose, TEXT("    Variable '%s' found on Blueprint"), *Step.Target);
	}

	return true;
}

// ============================================================================
// ResolveSetVarOp
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveSetVarOp(
	const FOliveIRBlueprintPlanStep& Step,
	UBlueprint* BP,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors,
	TArray<FString>& Warnings,
	const FOliveGraphContext& GraphContext)
{
	Out.NodeType = OliveNodeTypes::SetVariable;

	UE_LOG(LogOlivePlanResolver, Verbose, TEXT("    ResolveSetVarOp: variable='%s'"), *Step.Target);

	// If we're in a function graph, check if target matches a function output parameter
	if (GraphContext.bIsFunctionGraph && !Step.Target.IsEmpty())
	{
		for (const FString& ParamName : GraphContext.OutputParamNames)
		{
			if (ParamName.Equals(Step.Target, ESearchCase::IgnoreCase))
			{
				Out.StepId = Step.StepId;
				Out.NodeType = OliveNodeTypes::FunctionOutput;
				Out.Properties.Add(TEXT("param_name"), ParamName);
				Out.bIsPure = false; // FunctionResult has exec input

				Out.ResolverNotes.Add(FOliveResolverNote{
					TEXT("target"),
					Step.Target,
					FString::Printf(TEXT("FunctionOutput(%s)"), *ParamName),
					TEXT("Matched function output parameter -- will map to FunctionResult input pin")
				});

				UE_LOG(LogOlivePlanResolver, Log,
					TEXT("    ResolveSetVarOp: '%s' matched function output param '%s'"),
					*Step.Target, *ParamName);
				return true;
			}
		}
	}

	if (Step.Target.IsEmpty())
	{
		Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("MISSING_TARGET"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			TEXT("'set_var' op requires a 'target' specifying the variable name"),
			TEXT("Set 'target' to the Blueprint variable name")));
		return false;
	}

	Out.Properties.Add(TEXT("variable_name"), Step.Target);

	// Same inherited-variable check as get_var
	if (BP && !BlueprintHasVariable(BP, Step.Target))
	{
		UBlueprint* ParentBP = BP->ParentClass ? Cast<UBlueprint>(BP->ParentClass->ClassGeneratedBy) : nullptr;
		bool bFoundInParent = false;
		while (ParentBP)
		{
			if (BlueprintHasVariable(ParentBP, Step.Target))
			{
				bFoundInParent = true;
				break;
			}
			ParentBP = ParentBP->ParentClass ? Cast<UBlueprint>(ParentBP->ParentClass->ClassGeneratedBy) : nullptr;
		}

		if (!bFoundInParent)
		{
			// Check if the name matches a component in the SCS.
			// This catches a common AI mistake: using set_var for a component name.
			// NOTE: We keep this SCS walk even though BlueprintHasVariable() now also
			// checks SCS components. We need the component CLASS NAME for the error
			// message, which BlueprintHasVariable() doesn't return. If we later refactor
			// BlueprintHasVariable to return component class info, this can be simplified.
			if (BP->SimpleConstructionScript)
			{
				FString MatchedComponentClass;
				TArray<USCS_Node*> NodesToSearch;
				for (USCS_Node* RootNode : BP->SimpleConstructionScript->GetRootNodes())
				{
					NodesToSearch.Add(RootNode);
				}
				while (NodesToSearch.Num() > 0)
				{
					USCS_Node* Current = NodesToSearch.Pop();
					if (!Current) continue;
					if (Current->GetVariableName().ToString() == Step.Target)
					{
						// Found a component with this name
						if (Current->ComponentClass)
						{
							FString ClassName = Current->ComponentClass->GetName();
							// Strip the U prefix for display (UArrowComponent -> ArrowComponent)
							if (ClassName.StartsWith(TEXT("U")))
							{
								ClassName = ClassName.Mid(1);
							}
							MatchedComponentClass = ClassName;
						}
						break;
					}
					for (USCS_Node* Child : Current->GetChildNodes())
					{
						NodesToSearch.Add(Child);
					}
				}

				if (!MatchedComponentClass.IsEmpty())
				{
					// This is a component, not a variable -- reject with actionable guidance
					// Build a clean step_id suggestion from the component name
					FString CleanName = Step.Target;
					CleanName = CleanName.ToLower();

					Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
						TEXT("COMPONENT_NOT_VARIABLE"),
						Step.StepId,
						FString::Printf(TEXT("/steps/%d/target"), Idx),
						FString::Printf(
							TEXT("'%s' is a component (class: %s). Components are read-only references "
								 "and cannot be assigned with set_var. To READ this component, use get_var. "
								 "To MODIFY a property on it, first get_var, then call the setter: "
								 "{\"step_id\":\"get_%s\", \"op\":\"get_var\", \"target\":\"%s\"}, "
								 "then {\"op\":\"call\", \"target\":\"SetRelativeLocation\", "
								 "\"inputs\":{\"Target\":\"@get_%s.auto\", \"NewLocation\":\"...\"}}"),
							*Step.Target, *MatchedComponentClass,
							*CleanName, *Step.Target, *CleanName),
						TEXT("Use get_var to read the component reference, then call "
							 "setter functions with Target wired to the get_var output.")));

					UE_LOG(LogOlivePlanResolver, Warning,
						TEXT("Step '%s': '%s' is a component (%s), not a variable — rejected with guidance"),
						*Step.StepId, *Step.Target, *MatchedComponentClass);

					return false;
				}
			}

			// Not a component either -- check native C++ properties on the generated class
			bool bFoundOnGeneratedClass = false;
			if (BP->GeneratedClass)
			{
				FProperty* Prop = BP->GeneratedClass->FindPropertyByName(FName(*Step.Target));
				bFoundOnGeneratedClass = (Prop != nullptr);
			}

			if (bFoundOnGeneratedClass)
			{
				UE_LOG(LogOlivePlanResolver, Verbose,
					TEXT("Step '%s': Variable '%s' found on generated class (native property)"),
					*Step.StepId, *Step.Target);
			}
			else
			{
				// Variable not found anywhere. Emit a warning (not error) because
				// another step in the plan may create it, or the generated class
				// may not be fully compiled yet. The node factory will still attempt
				// creation via SetSelfMember which may succeed at compile time.
				Warnings.Add(FString::Printf(
					TEXT("Step '%s': Variable '%s' not found on Blueprint '%s'. "
						 "If this is a typo, the node will fail at compile. "
						 "Components use their variable name from the Components panel."),
					*Step.StepId, *Step.Target, *BP->GetName()));

				UE_LOG(LogOlivePlanResolver, Warning,
					TEXT("Step '%s': Variable '%s' not found on Blueprint '%s' or parents or generated class"),
					*Step.StepId, *Step.Target, *BP->GetName());
			}
		}
		else
		{
			UE_LOG(LogOlivePlanResolver, Verbose, TEXT("    Variable '%s' found on Blueprint (inherited)"), *Step.Target);
		}
	}
	else if (BP)
	{
		UE_LOG(LogOlivePlanResolver, Verbose, TEXT("    Variable '%s' found on Blueprint"), *Step.Target);
	}

	return true;
}

// ============================================================================
// ResolveEventOp
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveEventOp(
	const FOliveIRBlueprintPlanStep& Step,
	UBlueprint* BP,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors)
{
	Out.NodeType = OliveNodeTypes::Event;

	UE_LOG(LogOlivePlanResolver, Log, TEXT("    ResolveEventOp: target='%s'"), *Step.Target);

	if (Step.Target.IsEmpty())
	{
		Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("MISSING_TARGET"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			TEXT("'event' op requires a 'target' specifying the event name"),
			TEXT("Set 'target' to the event name (e.g., \"BeginPlay\", \"Tick\", \"ActorBeginOverlap\", \"OnComponentBeginOverlap\", \"OnComponentHit\", \"IA_Interact\")")));

		UE_LOG(LogOlivePlanResolver, Warning,
			TEXT("    ResolveEventOp FAILED: event '%s' not mapped. Parent class: '%s'"),
			*Step.Target, BP ? *BP->ParentClass->GetName() : TEXT("null"));

		return false;
	}

	// Map user-friendly event names to their internal UFunction equivalents.
	// In UE5, Blueprint-overridable native events use a "Receive" prefix
	// (e.g., BeginPlay -> ReceiveBeginPlay) but the editor displays them without it.
	// Names already in Receive* form pass through unchanged (no double-mapping).
	static const TMap<FString, FString> EventNameMap = {
		{ TEXT("BeginPlay"),          TEXT("ReceiveBeginPlay") },
		{ TEXT("EndPlay"),            TEXT("ReceiveEndPlay") },
		{ TEXT("Tick"),               TEXT("ReceiveTick") },
		{ TEXT("ActorBeginOverlap"),  TEXT("ReceiveActorBeginOverlap") },
		{ TEXT("ActorEndOverlap"),    TEXT("ReceiveActorEndOverlap") },
		{ TEXT("AnyDamage"),          TEXT("ReceiveAnyDamage") },
		{ TEXT("Hit"),                TEXT("ReceiveHit") },
		{ TEXT("PointDamage"),        TEXT("ReceivePointDamage") },
		{ TEXT("RadialDamage"),       TEXT("ReceiveRadialDamage") },
		{ TEXT("Destroyed"),          TEXT("ReceiveDestroyed") },

		// Display-name aliases (how events appear in the Blueprint editor)
		{ TEXT("EventTick"),               TEXT("ReceiveTick") },
		{ TEXT("EventBeginPlay"),          TEXT("ReceiveBeginPlay") },
		{ TEXT("EventEndPlay"),            TEXT("ReceiveEndPlay") },
		{ TEXT("EventAnyDamage"),          TEXT("ReceiveAnyDamage") },
		{ TEXT("EventHit"),                TEXT("ReceiveHit") },
		{ TEXT("EventActorBeginOverlap"),  TEXT("ReceiveActorBeginOverlap") },
		{ TEXT("EventActorEndOverlap"),    TEXT("ReceiveActorEndOverlap") },

		// Space variants (common AI patterns)
		{ TEXT("Event BeginPlay"),         TEXT("ReceiveBeginPlay") },
		{ TEXT("Event Tick"),              TEXT("ReceiveTick") },
		{ TEXT("Event End Play"),          TEXT("ReceiveEndPlay") },

		// Pass-through (AI sometimes uses the internal name directly)
		{ TEXT("ReceiveBeginPlay"),        TEXT("ReceiveBeginPlay") },
		{ TEXT("ReceiveTick"),             TEXT("ReceiveTick") },
		{ TEXT("ReceiveEndPlay"),          TEXT("ReceiveEndPlay") },
		{ TEXT("ReceiveActorBeginOverlap"), TEXT("ReceiveActorBeginOverlap") },
		{ TEXT("ReceiveActorEndOverlap"),  TEXT("ReceiveActorEndOverlap") },
		{ TEXT("ReceiveAnyDamage"),        TEXT("ReceiveAnyDamage") },
		{ TEXT("ReceiveHit"),              TEXT("ReceiveHit") },
		{ TEXT("ReceivePointDamage"),      TEXT("ReceivePointDamage") },
		{ TEXT("ReceiveRadialDamage"),     TEXT("ReceiveRadialDamage") },
		{ TEXT("ReceiveDestroyed"),        TEXT("ReceiveDestroyed") },

		// Pawn/Character events
		{ TEXT("Possessed"),               TEXT("ReceivePossessed") },
		{ TEXT("UnPossessed"),             TEXT("ReceiveUnPossessed") },
		{ TEXT("ControllerChanged"),       TEXT("ReceiveControllerChanged") },
		{ TEXT("Landed"),                  TEXT("OnLanded") },
		{ TEXT("OnLanded"),                TEXT("OnLanded") },
		{ TEXT("OnJumped"),                TEXT("OnJumped") },
		{ TEXT("MovementModeChanged"),     TEXT("OnMovementModeChanged") },
		{ TEXT("OnMovementModeChanged"),   TEXT("OnMovementModeChanged") },
	};

	// ----------------------------------------------------------------
	// Enhanced Input Action detection: targets starting with "IA_"
	// resolve to UK2Node_EnhancedInputAction instead of UK2Node_Event.
	// This avoids the NodeFactory fallback path and gives the executor
	// the correct node type for reuse checks and pin manifests.
	// ----------------------------------------------------------------
	if (Step.Target.StartsWith(TEXT("IA_")))
	{
		Out.NodeType = OliveNodeTypes::EnhancedInputAction;
		Out.Properties.Add(TEXT("input_action_name"), Step.Target);

		FOliveResolverNote Note;
		Note.Field = TEXT("node_type");
		Note.OriginalValue = TEXT("Event");
		Note.ResolvedValue = OliveNodeTypes::EnhancedInputAction;
		Note.Reason = FString::Printf(
			TEXT("Target '%s' starts with 'IA_', resolved as Enhanced Input Action event."),
			*Step.Target);
		Out.ResolverNotes.Add(MoveTemp(Note));

		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("Step '%s': Enhanced Input Action detected — target='%s', "
				 "resolving as EnhancedInputAction node type"),
			*Step.StepId, *Step.Target);

		return true;
	}

	FString ResolvedEventName = Step.Target;
	const bool bIsNativeEvent = EventNameMap.Contains(Step.Target);
	if (bIsNativeEvent)
	{
		ResolvedEventName = *EventNameMap.Find(Step.Target);
		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("Step '%s': Mapped event name '%s' -> '%s'"),
			*Step.StepId, *Step.Target, *ResolvedEventName);
	}

	// ----------------------------------------------------------------
	// Component delegate event detection via SCS inspection.
	// For non-native events (names not in EventNameMap), check if
	// the target matches a multicast delegate property on any SCS
	// component. This resolves to ComponentBoundEvent node type
	// which gives the executor proper reuse detection and node
	// creation through the dedicated factory method.
	//
	// When a component_name property is explicitly provided, we
	// require a match on that specific component.
	// ----------------------------------------------------------------
	if (!bIsNativeEvent && BP && BP->SimpleConstructionScript)
	{
		FString ComponentNameHint;
		if (Step.Properties.Contains(TEXT("component_name")))
		{
			ComponentNameHint = Step.Properties[TEXT("component_name")];
		}

		const FName EventFName(*Step.Target);

		FString MatchedDelegateName;
		FString MatchedComponentName;
		bool bFoundMatch = false;

		TArray<USCS_Node*> AllSCSNodes = BP->SimpleConstructionScript->GetAllNodes();
		for (USCS_Node* SCSNode : AllSCSNodes)
		{
			if (!SCSNode || !SCSNode->ComponentClass)
			{
				continue;
			}

			// If component_name hint was provided, only search that component
			if (!ComponentNameHint.IsEmpty()
				&& !SCSNode->GetVariableName().ToString().Equals(ComponentNameHint, ESearchCase::IgnoreCase))
			{
				continue;
			}

			for (TFieldIterator<FMulticastDelegateProperty> It(SCSNode->ComponentClass); It; ++It)
			{
				const FString PropName = It->GetName();

				// Match: exact name, or with/without "On" prefix
				if (PropName.Equals(Step.Target, ESearchCase::IgnoreCase)
					|| PropName.Equals(TEXT("On") + Step.Target, ESearchCase::IgnoreCase)
					|| (TEXT("On") + PropName).Equals(Step.Target, ESearchCase::IgnoreCase))
				{
					MatchedDelegateName = PropName;
					MatchedComponentName = SCSNode->GetVariableName().ToString();
					bFoundMatch = true;
					break;
				}
			}

			if (bFoundMatch)
			{
				break;
			}
		}

		if (bFoundMatch)
		{
			Out.NodeType = OliveNodeTypes::ComponentBoundEvent;
			Out.Properties.Add(TEXT("delegate_name"), MatchedDelegateName);
			Out.Properties.Add(TEXT("component_name"), MatchedComponentName);

			Out.ResolverNotes.Add(FOliveResolverNote{
				TEXT("event_type"),
				FString::Printf(TEXT("event: %s"), *Step.Target),
				FString::Printf(TEXT("component_bound_event: %s on %s"), *MatchedDelegateName, *MatchedComponentName),
				TEXT("Detected as component delegate event via SCS inspection")
			});

			UE_LOG(LogOlivePlanResolver, Log,
				TEXT("Step '%s': Resolved as ComponentBoundEvent — delegate='%s' on component='%s'"),
				*Step.StepId, *MatchedDelegateName, *MatchedComponentName);

			return true;
		}

		// If component_name was explicitly specified but no match found, give a targeted error
		if (!ComponentNameHint.IsEmpty())
		{
			// Build a list of available delegates on the specified component
			TArray<FString> AvailableDelegates;
			for (USCS_Node* SCSNode : AllSCSNodes)
			{
				if (SCSNode && SCSNode->GetVariableName().ToString().Equals(ComponentNameHint, ESearchCase::IgnoreCase))
				{
					for (TFieldIterator<FMulticastDelegateProperty> It(SCSNode->ComponentClass); It; ++It)
					{
						AvailableDelegates.Add(It->GetName());
					}
					break;
				}
			}

			FString Suggestion;
			if (AvailableDelegates.Num() > 0)
			{
				Suggestion = FString::Printf(
					TEXT("Available delegates on '%s': %s"),
					*ComponentNameHint, *FString::Join(AvailableDelegates, TEXT(", ")));
			}
			else
			{
				Suggestion = FString::Printf(
					TEXT("Check that component '%s' exists in the Blueprint SCS and the delegate name is correct "
						 "(e.g., 'OnComponentBeginOverlap', not 'BeginOverlap')."),
					*ComponentNameHint);
			}

			Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
				TEXT("COMPONENT_EVENT_NOT_FOUND"),
				Step.StepId,
				FString::Printf(TEXT("/steps/%d/target"), Idx),
				FString::Printf(TEXT("Event '%s' not found on component '%s'."), *Step.Target, *ComponentNameHint),
				Suggestion));

			return false;
		}
	}

	// Native event or unresolved non-native event — pass through to NodeFactory
	Out.Properties.Add(TEXT("event_name"), ResolvedEventName);

	if (!bIsNativeEvent)
	{
		// Non-native event that didn't match any SCS delegate. NodeFactory's
		// CreateEventNode has its own fallback SCS scan, so we pass through
		// and let it try (it may find a match on parent class components, etc.).
		FOliveResolverNote Note;
		Note.Field = TEXT("event_name");
		Note.OriginalValue = Step.Target;
		Note.ResolvedValue = ResolvedEventName;
		Note.Reason = TEXT("Not a native event override and no SCS delegate match found at resolve time. "
			"Will be resolved by NodeFactory fallback.");
		Out.ResolverNotes.Add(MoveTemp(Note));

		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("Step '%s': Non-native event '%s' — no SCS delegate match, falling through to NodeFactory"),
			*Step.StepId, *ResolvedEventName);
	}

	UE_LOG(LogOlivePlanResolver, Verbose,
		TEXT("Step '%s': Resolved event '%s'"),
		*Step.StepId, *ResolvedEventName);

	return true;
}

// ============================================================================
// ResolveSimpleOp
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveSimpleOp(
	const FOliveIRBlueprintPlanStep& Step,
	const FString& NodeType,
	FOliveResolvedStep& Out)
{
	Out.NodeType = NodeType;

	UE_LOG(LogOlivePlanResolver, Verbose,
		TEXT("Step '%s': Simple resolve to '%s'"),
		*Step.StepId, *NodeType);

	return true;
}

// ============================================================================
// ResolveCastOp
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveCastOp(
	const FOliveIRBlueprintPlanStep& Step,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors)
{
	Out.NodeType = OliveNodeTypes::Cast;

	// Cast requires a target class — check both Target and TargetClass
	FString CastTarget = Step.Target;
	if (CastTarget.IsEmpty())
	{
		CastTarget = Step.TargetClass;
	}

	if (CastTarget.IsEmpty())
	{
		Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("MISSING_TARGET"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			TEXT("'cast' op requires a 'target' specifying the class to cast to"),
			TEXT("Set 'target' to the class name (e.g., \"Character\", \"PlayerController\")")));
		return false;
	}

	Out.Properties.Add(TEXT("target_class"), CastTarget);

	UE_LOG(LogOlivePlanResolver, Verbose,
		TEXT("Step '%s': Resolved cast to '%s'"),
		*Step.StepId, *CastTarget);

	return true;
}

// ============================================================================
// ResolveStructOp
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveStructOp(
	const FOliveIRBlueprintPlanStep& Step,
	const FString& NodeType,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors)
{
	Out.NodeType = NodeType;

	// Struct ops require a struct type — check Target first, then Properties
	FString StructType = Step.Target;
	if (StructType.IsEmpty())
	{
		const FString* PropValue = Step.Properties.Find(TEXT("struct_type"));
		if (PropValue)
		{
			StructType = *PropValue;
		}
	}

	if (StructType.IsEmpty())
	{
		const FString OpName = (NodeType == OliveNodeTypes::MakeStruct) ? TEXT("make_struct") : TEXT("break_struct");
		Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("MISSING_TARGET"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			FString::Printf(TEXT("'%s' op requires a 'target' specifying the struct type"), *OpName),
			TEXT("Set 'target' to the struct name (e.g., \"Vector\", \"Rotator\", \"Transform\")")));
		return false;
	}

	Out.Properties.Add(TEXT("struct_type"), StructType);

	UE_LOG(LogOlivePlanResolver, Verbose,
		TEXT("Step '%s': Resolved %s for struct '%s'"),
		*Step.StepId, *NodeType, *StructType);

	return true;
}

// ============================================================================
// ResolveCallDelegateOp
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveCallDelegateOp(
	const FOliveIRBlueprintPlanStep& Step,
	UBlueprint* BP,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors)
{
	Out.NodeType = OliveNodeTypes::CallDelegate;

	UE_LOG(LogOlivePlanResolver, Log, TEXT("    ResolveCallDelegateOp: target='%s'"), *Step.Target);

	if (Step.Target.IsEmpty())
	{
		Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("MISSING_TARGET"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			TEXT("'call_delegate' op requires a 'target' specifying the event dispatcher name"),
			TEXT("Set 'target' to the dispatcher name (e.g., \"OnFired\", \"OnDamageReceived\")")));
		return false;
	}

	// Validate that the Blueprint has a multicast delegate variable with this name.
	// Event dispatchers are stored in NewVariables with PinCategory == PC_MCDelegate.
	bool bFoundDispatcher = false;
	TArray<FString> AvailableDispatchers;

	if (BP)
	{
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				const FString VarName = Var.VarName.ToString();
				AvailableDispatchers.Add(VarName);

				if (VarName == Step.Target)
				{
					bFoundDispatcher = true;
				}
			}
		}
	}

	if (!bFoundDispatcher)
	{
		FString SuggestionText;
		if (AvailableDispatchers.Num() > 0)
		{
			SuggestionText = FString::Printf(
				TEXT("Available dispatchers on this Blueprint: %s"),
				*FString::Join(AvailableDispatchers, TEXT(", ")));
		}
		else
		{
			SuggestionText = TEXT("This Blueprint has no event dispatchers. "
				"Use blueprint.add_function with function_type='event_dispatcher' to create one first.");
		}

		Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("DELEGATE_NOT_FOUND"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			FString::Printf(TEXT("Event dispatcher '%s' not found on Blueprint '%s'"),
				*Step.Target, BP ? *BP->GetName() : TEXT("null")),
			SuggestionText));

		UE_LOG(LogOlivePlanResolver, Warning,
			TEXT("    ResolveCallDelegateOp FAILED: dispatcher '%s' not found. Available: [%s]"),
			*Step.Target, *FString::Join(AvailableDispatchers, TEXT(", ")));
		return false;
	}

	Out.Properties.Add(TEXT("delegate_name"), Step.Target);
	Out.bIsPure = false; // Delegate broadcast has exec pins

	UE_LOG(LogOlivePlanResolver, Log,
		TEXT("    ResolveCallDelegateOp: '%s' resolved successfully"),
		*Step.Target);

	return true;
}

// ============================================================================
// ResolveBindDelegateOp
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveBindDelegateOp(
	const FOliveIRBlueprintPlanStep& Step,
	UBlueprint* BP,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors)
{
	Out.NodeType = OliveNodeTypes::BindDelegate;

	UE_LOG(LogOlivePlanResolver, Log, TEXT("    ResolveBindDelegateOp: target='%s'"), *Step.Target);

	if (Step.Target.IsEmpty())
	{
		Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("MISSING_TARGET"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			TEXT("'bind_dispatcher' op requires a 'target' specifying the event dispatcher name"),
			TEXT("Set 'target' to the dispatcher name (e.g., \"OnFired\", \"OnDamageReceived\")")));
		return false;
	}

	// Validate that the Blueprint has a multicast delegate variable with this name.
	// Event dispatchers are stored in NewVariables with PinCategory == PC_MCDelegate.
	bool bFoundDispatcher = false;
	TArray<FString> AvailableDispatchers;

	if (BP)
	{
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				const FString VarName = Var.VarName.ToString();
				AvailableDispatchers.Add(VarName);

				if (VarName == Step.Target)
				{
					bFoundDispatcher = true;
				}
			}
		}
	}

	if (!bFoundDispatcher)
	{
		FString SuggestionText;
		if (AvailableDispatchers.Num() > 0)
		{
			SuggestionText = FString::Printf(
				TEXT("Available dispatchers on this Blueprint: %s"),
				*FString::Join(AvailableDispatchers, TEXT(", ")));
		}
		else
		{
			SuggestionText = TEXT("This Blueprint has no event dispatchers. "
				"Use blueprint.add_function with function_type='event_dispatcher' to create one first.");
		}

		Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("DELEGATE_NOT_FOUND"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			FString::Printf(TEXT("Event dispatcher '%s' not found on Blueprint '%s'"),
				*Step.Target, BP ? *BP->GetName() : TEXT("null")),
			SuggestionText));

		UE_LOG(LogOlivePlanResolver, Warning,
			TEXT("    ResolveBindDelegateOp FAILED: dispatcher '%s' not found. Available: [%s]"),
			*Step.Target, *FString::Join(AvailableDispatchers, TEXT(", ")));
		return false;
	}

	Out.Properties.Add(TEXT("delegate_name"), Step.Target);
	Out.bIsPure = false; // Bind delegate has exec pins

	UE_LOG(LogOlivePlanResolver, Log,
		TEXT("    ResolveBindDelegateOp: '%s' resolved successfully"),
		*Step.Target);

	return true;
}

// ============================================================================
// InferMissingExecChain
// ============================================================================

bool FOliveBlueprintPlanResolver::InferMissingExecChain(
	FOliveIRBlueprintPlan& Plan,
	const TArray<FOliveResolvedStep>& ResolvedSteps,
	TArray<FOliveResolverNote>& OutNotes)
{
	// Build StepId -> bIsPure lookup from resolved steps
	TSet<FString> PureStepIds;
	for (const FOliveResolvedStep& RS : ResolvedSteps)
	{
		if (RS.bIsPure)
		{
			PureStepIds.Add(RS.StepId);
		}
	}

	// Build set of steps that already have incoming exec:
	//   - Steps with exec_after set (they follow another step)
	//   - Steps targeted by another step's exec_outputs values
	TSet<FString> HasIncomingExec;
	for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		if (!Step.ExecAfter.IsEmpty())
		{
			HasIncomingExec.Add(Step.StepId);
		}
		for (const auto& Pair : Step.ExecOutputs)
		{
			HasIncomingExec.Add(Pair.Value);
		}
	}

	// Empty string entries come from malformed exec_outputs (array coercion).
	// They pollute the set without providing useful data.
	HasIncomingExec.Remove(TEXT(""));

	int32 InferredCount = 0;
	FString PreviousImpureStepId;
	FString CurrentEventId;

	for (FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		// Skip pure steps entirely — they have no exec pins
		if (PureStepIds.Contains(Step.StepId))
		{
			continue;
		}

		// Event/custom_event are chain roots — update tracking but never chain from previous
		if (Step.Op == OlivePlanOps::Event || Step.Op == OlivePlanOps::CustomEvent)
		{
			PreviousImpureStepId = Step.StepId;
			CurrentEventId = Step.StepId;
			continue;
		}

		// Skip steps that already have incoming exec (exec_after set or targeted by exec_outputs)
		if (HasIncomingExec.Contains(Step.StepId))
		{
			PreviousImpureStepId = Step.StepId;
			continue;
		}

		// This is an orphaned impure step — chain it to the previous impure step
		if (!PreviousImpureStepId.IsEmpty())
		{
			Step.ExecAfter = PreviousImpureStepId;
			InferredCount++;

			FOliveResolverNote Note;
			Note.Field = FString::Printf(TEXT("step '%s' exec_after"), *Step.StepId);
			Note.OriginalValue = TEXT("(empty)");
			Note.ResolvedValue = PreviousImpureStepId;
			Note.Reason = FString::Printf(
				TEXT("Orphaned impure step inferred to follow '%s' within event '%s' chain."),
				*PreviousImpureStepId, *CurrentEventId);
			OutNotes.Add(MoveTemp(Note));
		}

		PreviousImpureStepId = Step.StepId;
	}

	if (InferredCount > 0)
	{
		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("InferMissingExecChain: inferred exec_after for %d orphaned impure step(s)"),
			InferredCount);
	}

	return InferredCount > 0;
}

// ============================================================================
// CollapseExecThroughPureSteps
// ============================================================================

bool FOliveBlueprintPlanResolver::CollapseExecThroughPureSteps(
	FOliveIRBlueprintPlan& Plan,
	const TArray<FOliveResolvedStep>& ResolvedSteps,
	TArray<FOliveResolverNote>& OutNotes)
{
	// Build StepId -> bIsPure lookup
	TSet<FString> PureStepIds;
	for (const FOliveResolvedStep& RS : ResolvedSteps)
	{
		if (RS.bIsPure)
		{
			PureStepIds.Add(RS.StepId);
		}
	}

	if (PureStepIds.Num() == 0)
	{
		return false; // No pure steps, nothing to collapse
	}

	// Check if any pure step participates in exec chains at all
	bool bAnyPureInExecChain = false;
	for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		if (PureStepIds.Contains(Step.StepId))
		{
			if (!Step.ExecAfter.IsEmpty() || Step.ExecOutputs.Num() > 0)
			{
				bAnyPureInExecChain = true;
				break;
			}
		}
		// Also check if any step references a pure step via exec_after or exec_outputs
		if (!bAnyPureInExecChain && PureStepIds.Contains(Step.ExecAfter))
		{
			bAnyPureInExecChain = true;
		}
		if (!bAnyPureInExecChain)
		{
			for (const auto& ExecOut : Step.ExecOutputs)
			{
				if (PureStepIds.Contains(ExecOut.Value))
				{
					bAnyPureInExecChain = true;
					break;
				}
			}
		}
	}

	if (!bAnyPureInExecChain)
	{
		return false; // Pure steps exist but none are in exec chains
	}

	UE_LOG(LogOlivePlanResolver, Log,
		TEXT("CollapseExecThroughPureSteps: %d pure step(s) found, checking exec chain participation"),
		PureStepIds.Num());

	// Build step_id -> plan step index map
	TMap<FString, int32> StepIndexMap;
	for (int32 i = 0; i < Plan.Steps.Num(); ++i)
	{
		StepIndexMap.Add(Plan.Steps[i].StepId, i);
	}

	// Build forward successor map: StepId -> [steps that have exec_after == StepId]
	TMap<FString, TArray<FString>> ForwardSuccessors;
	for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		if (!Step.ExecAfter.IsEmpty())
		{
			ForwardSuccessors.FindOrAdd(Step.ExecAfter).Add(Step.StepId);
		}
	}

	// Build reverse exec_outputs map: TargetStepId -> [(SourceStepId, PinName)]
	// Used for backward resolution when a pure step has no exec_after but is
	// targeted by another step's exec_outputs.
	TMap<FString, FString> ReverseExecOutputTarget; // target -> source step
	for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		for (const auto& ExecOut : Step.ExecOutputs)
		{
			ReverseExecOutputTarget.Add(ExecOut.Value, Step.StepId);
		}
	}

	// Lambda: check if a step is pure
	auto IsPure = [&PureStepIds](const FString& StepId) -> bool
	{
		return PureStepIds.Contains(StepId);
	};

	// Resolve backward: from a pure step, find the nearest impure predecessor.
	// Follows exec_after chain backward, falling back to reverse exec_outputs.
	TFunction<FString(const FString&, TSet<FString>&)> ResolveBackward;
	ResolveBackward = [&](const FString& PureStepId, TSet<FString>& Visited) -> FString
	{
		if (Visited.Contains(PureStepId))
		{
			return FString(); // Cycle protection
		}
		Visited.Add(PureStepId);

		const int32* Idx = StepIndexMap.Find(PureStepId);
		if (!Idx)
		{
			return FString();
		}

		// Try exec_after first (the step's declared predecessor)
		const FString& PredId = Plan.Steps[*Idx].ExecAfter;
		if (!PredId.IsEmpty())
		{
			if (!IsPure(PredId))
			{
				return PredId; // Found impure predecessor
			}
			return ResolveBackward(PredId, Visited);
		}

		// Fall back to reverse exec_outputs (step targeted by another step's exec_outputs)
		const FString* SourceStepId = ReverseExecOutputTarget.Find(PureStepId);
		if (SourceStepId && !SourceStepId->IsEmpty())
		{
			if (!IsPure(*SourceStepId))
			{
				return *SourceStepId; // Found impure source
			}
			return ResolveBackward(*SourceStepId, Visited);
		}

		return FString(); // No predecessor found
	};

	// Resolve forward: from a pure step, find the nearest impure successor.
	// Follows the forward successor map (steps that have exec_after == this step).
	TFunction<FString(const FString&, TSet<FString>&)> ResolveForward;
	ResolveForward = [&](const FString& PureStepId, TSet<FString>& Visited) -> FString
	{
		if (Visited.Contains(PureStepId))
		{
			return FString(); // Cycle protection
		}
		Visited.Add(PureStepId);

		// Check steps that have exec_after pointing to this pure step
		const TArray<FString>* Successors = ForwardSuccessors.Find(PureStepId);
		if (Successors && Successors->Num() > 0)
		{
			const FString& SuccId = (*Successors)[0];
			if (!IsPure(SuccId))
			{
				return SuccId; // Found impure successor
			}
			return ResolveForward(SuccId, Visited);
		}

		// Also check the pure step's own exec_outputs (AI shouldn't write these
		// on pure steps, but handle gracefully)
		const int32* Idx = StepIndexMap.Find(PureStepId);
		if (Idx)
		{
			for (const auto& ExecOut : Plan.Steps[*Idx].ExecOutputs)
			{
				if (!IsPure(ExecOut.Value))
				{
					return ExecOut.Value;
				}
				FString Result = ResolveForward(ExecOut.Value, Visited);
				if (!Result.IsEmpty())
				{
					return Result;
				}
			}
		}

		return FString(); // No successor found
	};

	bool bAnyCollapsed = false;

	// ------------------------------------------------------------------
	// Pass 1: For each non-pure step, resolve exec references to pure steps
	// ------------------------------------------------------------------
	for (int32 i = 0; i < Plan.Steps.Num(); ++i)
	{
		FOliveIRBlueprintPlanStep& Step = Plan.Steps[i];

		if (IsPure(Step.StepId))
		{
			continue; // Handle in pass 2
		}

		// Fix exec_after pointing to a pure step
		if (!Step.ExecAfter.IsEmpty() && IsPure(Step.ExecAfter))
		{
			TSet<FString> Visited;
			const FString NewPredecessor = ResolveBackward(Step.ExecAfter, Visited);

			FOliveResolverNote Note;
			Note.Field = FString::Printf(TEXT("step '%s' exec_after"), *Step.StepId);
			Note.OriginalValue = Step.ExecAfter;
			Note.ResolvedValue = NewPredecessor.IsEmpty() ? TEXT("(cleared)") : NewPredecessor;
			Note.Reason = FString::Printf(
				TEXT("exec_after referenced pure step '%s' which has no exec pins. "
				     "Collapsed to nearest impure predecessor."),
				*Step.ExecAfter);
			OutNotes.Add(MoveTemp(Note));

			UE_LOG(LogOlivePlanResolver, Log,
				TEXT("CollapseExec: step '%s' exec_after '%s' (pure) -> '%s'"),
				*Step.StepId, *Step.ExecAfter,
				NewPredecessor.IsEmpty() ? TEXT("(cleared)") : *NewPredecessor);

			Step.ExecAfter = NewPredecessor;
			bAnyCollapsed = true;
		}

		// Fix exec_outputs entries pointing to pure steps
		bool bOutputsModified = false;
		TMap<FString, FString> NewExecOutputs;
		for (const auto& ExecOut : Step.ExecOutputs)
		{
			if (IsPure(ExecOut.Value))
			{
				TSet<FString> Visited;
				const FString NewSuccessor = ResolveForward(ExecOut.Value, Visited);

				FOliveResolverNote Note;
				Note.Field = FString::Printf(TEXT("step '%s' exec_outputs.%s"), *Step.StepId, *ExecOut.Key);
				Note.OriginalValue = ExecOut.Value;
				Note.ResolvedValue = NewSuccessor.IsEmpty() ? TEXT("(removed)") : NewSuccessor;
				Note.Reason = FString::Printf(
					TEXT("exec_output targeted pure step '%s' which has no exec pins. "
					     "Collapsed to nearest impure successor."),
					*ExecOut.Value);
				OutNotes.Add(MoveTemp(Note));

				UE_LOG(LogOlivePlanResolver, Log,
					TEXT("CollapseExec: step '%s' exec_outputs.%s '%s' (pure) -> '%s'"),
					*Step.StepId, *ExecOut.Key, *ExecOut.Value,
					NewSuccessor.IsEmpty() ? TEXT("(removed)") : *NewSuccessor);

				if (!NewSuccessor.IsEmpty())
				{
					NewExecOutputs.Add(ExecOut.Key, NewSuccessor);
				}
				bOutputsModified = true;
				bAnyCollapsed = true;
			}
			else
			{
				NewExecOutputs.Add(ExecOut.Key, ExecOut.Value);
			}
		}
		if (bOutputsModified)
		{
			Step.ExecOutputs = MoveTemp(NewExecOutputs);
		}
	}

	// ------------------------------------------------------------------
	// Pass 2: Clear exec wiring on pure steps themselves
	// ------------------------------------------------------------------
	for (int32 i = 0; i < Plan.Steps.Num(); ++i)
	{
		FOliveIRBlueprintPlanStep& Step = Plan.Steps[i];

		if (!IsPure(Step.StepId))
		{
			continue;
		}

		if (!Step.ExecAfter.IsEmpty() || Step.ExecOutputs.Num() > 0)
		{
			FString OrigDesc;
			if (!Step.ExecAfter.IsEmpty())
			{
				OrigDesc += FString::Printf(TEXT("exec_after=%s"), *Step.ExecAfter);
			}
			for (const auto& ExecOut : Step.ExecOutputs)
			{
				if (!OrigDesc.IsEmpty())
				{
					OrigDesc += TEXT(", ");
				}
				OrigDesc += FString::Printf(TEXT("exec_outputs.%s=%s"), *ExecOut.Key, *ExecOut.Value);
			}

			FOliveResolverNote Note;
			Note.Field = FString::Printf(TEXT("step '%s' exec wiring"), *Step.StepId);
			Note.OriginalValue = OrigDesc;
			Note.ResolvedValue = TEXT("(cleared)");
			Note.Reason = TEXT("Pure node has no exec pins. Exec wiring removed; data wiring preserved.");
			OutNotes.Add(MoveTemp(Note));

			UE_LOG(LogOlivePlanResolver, Log,
				TEXT("CollapseExec: cleared exec wiring on pure step '%s' (%s)"),
				*Step.StepId, *OrigDesc);

			Step.ExecAfter.Empty();
			Step.ExecOutputs.Empty();
			bAnyCollapsed = true;
		}
	}

	if (bAnyCollapsed)
	{
		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("CollapseExecThroughPureSteps: collapsed %d exec reference(s)"),
			OutNotes.Num());
	}

	return bAnyCollapsed;
}

// ============================================================================
// ComputePlanFingerprint
// ============================================================================

FString FOliveBlueprintPlanResolver::ComputePlanFingerprint(
	const FOliveIRGraph& CurrentGraph,
	const FOliveIRBlueprintPlan& Plan)
{
	// Build a deterministic string from graph state + plan content.
	// Any change to the graph or plan will produce a different fingerprint.
	FString Combined;

	// 1. Graph statistics
	Combined += FString::Printf(TEXT("NC:%d|CC:%d|"), CurrentGraph.NodeCount, CurrentGraph.ConnectionCount);

	// 2. Sorted list of node types in the current graph
	TArray<FString> NodeTypes;
	NodeTypes.Reserve(CurrentGraph.Nodes.Num());
	for (const FOliveIRNode& Node : CurrentGraph.Nodes)
	{
		NodeTypes.Add(Node.Type);
	}
	NodeTypes.Sort();
	for (const FString& NodeType : NodeTypes)
	{
		Combined += NodeType;
		Combined += TEXT(",");
	}
	Combined += TEXT("|");

	// 3. Plan schema version
	Combined += TEXT("SV:");
	Combined += Plan.SchemaVersion;
	Combined += TEXT("|");

	// 4. Each step's identity: StepId + Op + Target (in order)
	for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		Combined += Step.StepId;
		Combined += TEXT(":");
		Combined += Step.Op;
		Combined += TEXT(":");
		Combined += Step.Target;
		Combined += TEXT(";");
	}

	// Hash via FSHA1 (available in all UE 5.x builds)
	const FString Utf8Input = Combined;
	FSHAHash Hash;
	FSHA1::HashBuffer(TCHAR_TO_UTF8(*Utf8Input), Utf8Input.Len(), Hash.Hash);

	return Hash.ToString().Left(8).ToLower();
}

// ============================================================================
// ComputePlanDiff
// ============================================================================

TSharedPtr<FJsonObject> FOliveBlueprintPlanResolver::ComputePlanDiff(
	const FOliveIRGraph& CurrentGraph,
	const TArray<FOliveResolvedStep>& ResolvedSteps,
	const FOliveIRBlueprintPlan& Plan)
{
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

	// Current graph counts
	Result->SetNumberField(TEXT("current_node_count"), CurrentGraph.NodeCount);
	Result->SetNumberField(TEXT("current_connection_count"), CurrentGraph.ConnectionCount);

	// Nodes to add = number of resolved steps
	Result->SetNumberField(TEXT("nodes_to_add"), ResolvedSteps.Num());

	// Count connections the plan will create:
	// - ExecAfter references (each creates one exec connection)
	// - ExecOutputs entries (each creates one exec connection)
	// - Input values starting with "@" (each creates one data connection)
	int32 ConnectionsToAdd = 0;

	for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		// ExecAfter: one exec wire from the referenced step to this step
		if (!Step.ExecAfter.IsEmpty())
		{
			ConnectionsToAdd++;
		}

		// ExecOutputs: each entry is an exec wire from this step's named output to another step
		ConnectionsToAdd += Step.ExecOutputs.Num();

		// Inputs: values starting with "@" are data wire references (e.g., "@s1.ReturnValue")
		for (const auto& InputPair : Step.Inputs)
		{
			if (InputPair.Value.StartsWith(TEXT("@")))
			{
				ConnectionsToAdd++;
			}
		}
	}

	Result->SetNumberField(TEXT("connections_to_add"), ConnectionsToAdd);

	// Per-step summary array
	TArray<TSharedPtr<FJsonValue>> StepsArray;
	StepsArray.Reserve(ResolvedSteps.Num());

	for (const FOliveResolvedStep& Resolved : ResolvedSteps)
	{
		TSharedPtr<FJsonObject> StepObj = MakeShareable(new FJsonObject());
		StepObj->SetStringField(TEXT("step_id"), Resolved.StepId);

		// Find the original plan step to get the Op
		const FOliveIRBlueprintPlanStep* OriginalStep = nullptr;
		for (const FOliveIRBlueprintPlanStep& PlanStep : Plan.Steps)
		{
			if (PlanStep.StepId == Resolved.StepId)
			{
				OriginalStep = &PlanStep;
				break;
			}
		}

		if (OriginalStep)
		{
			StepObj->SetStringField(TEXT("op"), OriginalStep->Op);
			if (!OriginalStep->Target.IsEmpty())
			{
				StepObj->SetStringField(TEXT("target"), OriginalStep->Target);
			}
		}

		StepObj->SetStringField(TEXT("node_type"), Resolved.NodeType);

		StepsArray.Add(MakeShareable(new FJsonValueObject(StepObj)));
	}

	Result->SetArrayField(TEXT("steps"), StepsArray);

	return Result;
}
