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
#include "Plan/OliveFunctionResolver.h"
#include "Engine/Blueprint.h"
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

		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			if (Var.VarName.ToString() == VariableName)
			{
				return true;
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
// Resolve (public entry point)
// ============================================================================

FOlivePlanResolveResult FOliveBlueprintPlanResolver::Resolve(
	const FOliveIRBlueprintPlan& Plan,
	UBlueprint* Blueprint)
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

	UE_LOG(LogOlivePlanResolver, Log, TEXT("Resolving plan with %d steps for Blueprint '%s'"),
		Plan.Steps.Num(), *Blueprint->GetName());

	bool bAllSucceeded = true;

	for (int32 i = 0; i < Plan.Steps.Num(); ++i)
	{
		const FOliveIRBlueprintPlanStep& Step = Plan.Steps[i];
		FOliveResolvedStep Resolved;

		if (ResolveStep(Step, Blueprint, i, Resolved, Result.Errors, Result.Warnings))
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

	UE_LOG(LogOlivePlanResolver, Log, TEXT("Plan resolution %s: %d/%d steps resolved, %d errors, %d warnings"),
		Result.bSuccess ? TEXT("succeeded") : TEXT("failed"),
		Result.ResolvedSteps.Num(),
		Plan.Steps.Num(),
		Result.Errors.Num(),
		Result.Warnings.Num());

	return Result;
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
	TArray<FString>& OutWarnings)
{
	OutResolved.StepId = Step.StepId;

	const FString& Op = Step.Op;

	UE_LOG(LogOlivePlanResolver, Log,
		TEXT("  Resolving step %d: step_id='%s', op='%s', target='%s'"),
		StepIndex, *Step.StepId, *Op, *Step.Target);

	bool bResult = false;

	if (Op == OlivePlanOps::Call)
	{
		bResult = ResolveCallOp(Step, Blueprint, StepIndex, OutResolved, OutErrors, OutWarnings);
	}
	else if (Op == OlivePlanOps::GetVar)
	{
		bResult = ResolveGetVarOp(Step, Blueprint, StepIndex, OutResolved, OutErrors);
	}
	else if (Op == OlivePlanOps::SetVar)
	{
		bResult = ResolveSetVarOp(Step, Blueprint, StepIndex, OutResolved, OutErrors);
	}
	else if (Op == OlivePlanOps::Event)
	{
		bResult = ResolveEventOp(Step, Blueprint, StepIndex, OutResolved, OutErrors);
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
	else if (Op == OlivePlanOps::Delay)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::Delay, OutResolved);
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
		// Return nodes are auto-created by function graphs.
		// Emit a comment node as a placeholder annotation.
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::Comment, OutResolved);
		if (bResult)
		{
			OutResolved.Properties.Add(TEXT("text"), TEXT("Return (auto-created by function graph)"));
		}
		OutWarnings.Add(FString::Printf(
			TEXT("Step '%s': 'return' op mapped to Comment node — return nodes are auto-created by function graphs"),
			*Step.StepId));
	}
	else if (Op == OlivePlanOps::Comment)
	{
		bResult = ResolveSimpleOp(Step, OliveNodeTypes::Comment, OutResolved);
		if (bResult)
		{
			OutResolved.Properties.Add(TEXT("text"), Step.Target);
		}
	}
	else
	{
		// Unknown op — should have been caught by schema validation, but handle gracefully
		FOliveIRBlueprintPlanError Error = FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("UNKNOWN_OP"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/op"), StepIndex),
			FString::Printf(TEXT("Unknown operation '%s'"), *Op),
			TEXT("Use one of the recognized ops: call, get_var, set_var, branch, sequence, event, custom_event, for_loop, for_each_loop, delay, is_valid, print_string, spawn_actor, cast, make_struct, break_struct, return, comment"));
		OutErrors.Add(MoveTemp(Error));
		return false;
	}

	// Merge any user-specified properties from the step (without overwriting resolver-set ones)
	if (bResult)
	{
		MergeStepProperties(Step, OutResolved.Properties);
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
	TArray<FString>& Warnings)
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

	// --- Smart function resolution via FOliveFunctionResolver ---
	FOliveFunctionMatch Match = FOliveFunctionResolver::Resolve(
		Step.Target, Step.TargetClass, BP);

	if (Match.IsValid())
	{
		// Resolver found a concrete UFunction* -- use its canonical name and class
		const FString ResolvedFunctionName = Match.Function->GetName();
		const FString ResolvedClassName = Match.OwningClass ? Match.OwningClass->GetName() : FString();

		Out.Properties.Add(TEXT("function_name"), ResolvedFunctionName);
		if (!ResolvedClassName.IsEmpty())
		{
			Out.Properties.Add(TEXT("target_class"), ResolvedClassName);
		}

		// Emit a warning if the resolution was not exact (so the AI learns the correct name)
		if (Match.Confidence < 90)
		{
			Warnings.Add(FString::Printf(
				TEXT("Step '%s': '%s' resolved to '%s::%s' (confidence: %d, method: %s)"),
				*Step.StepId, *Step.Target,
				*ResolvedClassName, *ResolvedFunctionName,
				Match.Confidence,
				*FOliveFunctionResolver::MatchMethodToString(Match.MatchMethod)));
		}

		UE_LOG(LogOlivePlanResolver, Verbose,
			TEXT("Step '%s': Resolved call '%s' -> '%s::%s' (confidence: %d, method: %s)"),
			*Step.StepId, *Step.Target,
			*ResolvedClassName, *ResolvedFunctionName,
			Match.Confidence,
			*FOliveFunctionResolver::MatchMethodToString(Match.MatchMethod));

		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("    ResolveCallOp: '%s' -> function_name='%s', target_class='%s'"),
			*Step.Target, *ResolvedFunctionName, *ResolvedClassName);

		return true;
	}

	// --- Resolver found nothing. Gather suggestions for error reporting. ---

	// If TargetClass was explicitly provided but function was not found, this is an error.
	if (!Step.TargetClass.IsEmpty())
	{
		TArray<FOliveFunctionMatch> Candidates = FOliveFunctionResolver::GetCandidates(Step.Target, CATALOG_SEARCH_LIMIT);
		TArray<FString> Alternatives;
		for (const FOliveFunctionMatch& C : Candidates)
		{
			if (C.IsValid())
			{
				Alternatives.Add(FString::Printf(TEXT("%s::%s (confidence: %d)"),
					C.OwningClass ? *C.OwningClass->GetName() : TEXT("?"),
					*C.Function->GetName(), C.Confidence));
			}
		}

		FOliveIRBlueprintPlanError Error = FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("FUNCTION_NOT_FOUND"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			FString::Printf(TEXT("Function '%s' not found on class '%s'"), *Step.Target, *Step.TargetClass),
			Alternatives.Num() > 0
				? FString::Printf(TEXT("Did you mean: %s"), *FString::Join(Alternatives, TEXT(", ")))
				: TEXT("Check the function name and class name"));
		Error.Alternatives = MoveTemp(Alternatives);
		Errors.Add(MoveTemp(Error));

		UE_LOG(LogOlivePlanResolver, Warning,
			TEXT("    ResolveCallOp FAILED: function '%s' could not be resolved (target_class='%s')"),
			*Step.Target, *Step.TargetClass);

		return false;
	}

	// No TargetClass and resolver couldn't find it.
	// Still accept the call as-is -- the function may exist but not be discoverable
	// (e.g., Blueprint-defined functions, dynamically loaded plugins).
	// The factory will validate at creation time.
	Out.Properties.Add(TEXT("function_name"), Step.Target);

	// Try to get candidates for a helpful warning
	TArray<FOliveFunctionMatch> Candidates = FOliveFunctionResolver::GetCandidates(Step.Target, CATALOG_SEARCH_LIMIT);
	if (Candidates.Num() > 0 && Candidates[0].IsValid())
	{
		Warnings.Add(FString::Printf(
			TEXT("Step '%s': Function '%s' not found by resolver. Did you mean '%s::%s'?"),
			*Step.StepId, *Step.Target,
			Candidates[0].OwningClass ? *Candidates[0].OwningClass->GetName() : TEXT("?"),
			*Candidates[0].Function->GetName()));
	}
	else
	{
		// Fall back to catalog fuzzy for suggestion text
		FOliveNodeCatalog& Catalog = FOliveNodeCatalog::Get();
		if (Catalog.IsInitialized())
		{
			TArray<FOliveNodeSuggestion> Suggestions = Catalog.FuzzyMatch(Step.Target, CATALOG_SEARCH_LIMIT);
			if (Suggestions.Num() > 0)
			{
				Warnings.Add(FString::Printf(
					TEXT("Step '%s': Function '%s' not found by resolver. Closest catalog match: '%s'"),
					*Step.StepId, *Step.Target, *Suggestions[0].DisplayName));
			}
		}
	}

	UE_LOG(LogOlivePlanResolver, Verbose,
		TEXT("Step '%s': Accepted call '%s' without definitive match (will validate at creation)"),
		*Step.StepId, *Step.Target);

	UE_LOG(LogOlivePlanResolver, Log,
		TEXT("    ResolveCallOp: '%s' -> function_name='%s', target_class='' (unresolved, accepted as-is)"),
		*Step.Target, *Step.Target);

	return true;
}

// ============================================================================
// ResolveGetVarOp
// ============================================================================

bool FOliveBlueprintPlanResolver::ResolveGetVarOp(
	const FOliveIRBlueprintPlanStep& Step,
	UBlueprint* BP,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors)
{
	Out.NodeType = OliveNodeTypes::GetVariable;

	UE_LOG(LogOlivePlanResolver, Verbose, TEXT("    ResolveGetVarOp: variable='%s'"), *Step.Target);

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
			// Not an error — variable might be created by an earlier step in the plan,
			// or might be a native property not in NewVariables
			UE_LOG(LogOlivePlanResolver, Verbose,
				TEXT("Step '%s': Variable '%s' not found on Blueprint '%s' (may be inherited or created by another step)"),
				*Step.StepId, *Step.Target, *BP->GetName());

			UE_LOG(LogOlivePlanResolver, Warning,
				TEXT("    Variable '%s' not found on Blueprint '%s' or parents"),
				*Step.Target, *BP->GetName());
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
	TArray<FOliveIRBlueprintPlanError>& Errors)
{
	Out.NodeType = OliveNodeTypes::SetVariable;

	UE_LOG(LogOlivePlanResolver, Verbose, TEXT("    ResolveSetVarOp: variable='%s'"), *Step.Target);

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
			UE_LOG(LogOlivePlanResolver, Verbose,
				TEXT("Step '%s': Variable '%s' not found on Blueprint '%s' (may be inherited or created by another step)"),
				*Step.StepId, *Step.Target, *BP->GetName());

			UE_LOG(LogOlivePlanResolver, Warning,
				TEXT("    Variable '%s' not found on Blueprint '%s' or parents"),
				*Step.Target, *BP->GetName());
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
			TEXT("Set 'target' to the event name (e.g., \"BeginPlay\", \"Tick\", \"ActorBeginOverlap\")")));

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
	};

	FString ResolvedEventName = Step.Target;
	if (const FString* MappedName = EventNameMap.Find(Step.Target))
	{
		ResolvedEventName = *MappedName;
		UE_LOG(LogOlivePlanResolver, Log,
			TEXT("Step '%s': Mapped event name '%s' -> '%s'"),
			*Step.StepId, *Step.Target, *ResolvedEventName);
	}

	Out.Properties.Add(TEXT("event_name"), ResolvedEventName);

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
