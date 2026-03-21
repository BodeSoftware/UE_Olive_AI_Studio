// Copyright Bode Software. All Rights Reserved.

#include "Plan/OlivePlanValidator.h"
#include "Plan/OliveBlueprintPlanResolver.h"
#include "Writer/OliveNodeFactory.h"
#include "OliveClassResolver.h"
#include "Engine/Blueprint.h"
#include "Components/ActorComponent.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"

DEFINE_LOG_CATEGORY(LogOlivePlanValidator);

// ============================================================================
// Validate (public entry point)
// ============================================================================

FOlivePlanValidationResult FOlivePlanValidator::Validate(
	const FOliveIRBlueprintPlan& Plan,
	const TArray<FOliveResolvedStep>& ResolvedSteps,
	UBlueprint* Blueprint,
	const FOliveGraphContext& GraphContext)
{
	FOlivePlanValidationResult Result;

	if (!Blueprint || Plan.Steps.Num() == 0)
	{
		return Result; // Nothing to validate
	}

	// Build validation context
	FOlivePlanValidationContext Context{ Plan, ResolvedSteps, Blueprint, {} };
	for (int32 i = 0; i < Plan.Steps.Num(); ++i)
	{
		Context.StepIdToIndex.Add(Plan.Steps[i].StepId, i);
	}

	UE_LOG(LogOlivePlanValidator, Log,
		TEXT("Phase 0: Validating plan with %d steps for Blueprint '%s'"),
		Plan.Steps.Num(), *Blueprint->GetName());

	// Run all checks
	CheckComponentFunctionTargets(Context, Result);
	CheckExecWiringConflicts(Context, Result);
	CheckLatentInFunctionGraph(Context, ResolvedSteps, GraphContext, Result);
	CheckVariableExists(Context, Result);
	CheckExecSourceIsReturn(Context, Result);
	CheckCollisionOnTriggerComponent(Context, Result);

	if (Result.Errors.Num() > 0)
	{
		Result.bSuccess = false;
	}

	UE_LOG(LogOlivePlanValidator, Log,
		TEXT("Phase 0 %s: %d errors, %d warnings"),
		Result.bSuccess ? TEXT("passed") : TEXT("FAILED"),
		Result.Errors.Num(), Result.Warnings.Num());

	return Result;
}

// ============================================================================
// AutoFixExecConflicts
// ============================================================================

bool FOlivePlanValidator::AutoFixExecConflicts(
	FOliveIRBlueprintPlan& Plan,
	TArray<FOliveResolverNote>& OutNotes)
{
	// Build set of steps that have exec_outputs (multi-output wiring).
	TSet<FString> StepsWithExecOutputs;
	for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		if (Step.ExecOutputs.Num() > 0)
		{
			StepsWithExecOutputs.Add(Step.StepId);
		}
	}

	if (StepsWithExecOutputs.Num() == 0)
	{
		return false;
	}

	bool bFixed = false;

	for (FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		if (Step.ExecAfter.IsEmpty())
		{
			continue;
		}

		if (!StepsWithExecOutputs.Contains(Step.ExecAfter))
		{
			continue;
		}

		// Conflict found: exec_after targets a step that uses exec_outputs.
		// FIX RC5: Remove the redundant exec_after. The step should be wired
		// via exec_outputs on the target step instead.
		FOliveResolverNote Note;
		Note.Field = FString::Printf(TEXT("step '%s' exec_after"), *Step.StepId);
		Note.OriginalValue = Step.ExecAfter;
		Note.ResolvedValue = TEXT("(removed)");
		Note.Reason = FString::Printf(
			TEXT("exec_after:'%s' conflicts with '%s'.exec_outputs. "
			     "Removed redundant exec_after to prevent double-claiming the exec output pin."),
			*Step.ExecAfter, *Step.ExecAfter);
		OutNotes.Add(MoveTemp(Note));

		UE_LOG(LogOlivePlanValidator, Log,
			TEXT("AutoFixExecConflicts: Removed exec_after:'%s' from step '%s' (conflicted with exec_outputs)"),
			*Step.ExecAfter, *Step.StepId);

		Step.ExecAfter.Empty();
		bFixed = true;
	}

	return bFixed;
}

// ============================================================================
// Check 1: Component Function Target Guard
// ============================================================================

void FOlivePlanValidator::CheckComponentFunctionTargets(
	const FOlivePlanValidationContext& Context,
	FOlivePlanValidationResult& Result)
{
	// If the Blueprint itself IS a component, component functions are valid on Self.
	const bool bBlueprintIsComponent =
		Context.Blueprint->ParentClass &&
		Context.Blueprint->ParentClass->IsChildOf(UActorComponent::StaticClass());

	if (bBlueprintIsComponent)
	{
		return;
	}

	for (int32 i = 0; i < Context.ResolvedSteps.Num(); ++i)
	{
		const FOliveResolvedStep& Resolved = Context.ResolvedSteps[i];

		if (!Resolved.ResolvedOwningClass)
		{
			continue; // No resolved class (non-call op or unresolved)
		}

		if (!Resolved.ResolvedOwningClass->IsChildOf(UActorComponent::StaticClass()))
		{
			continue; // Not a component function
		}

		// This is a component-only function on a non-component Blueprint.
		// Check if the AI wired the Target pin explicitly.
		const int32* PlanIndexPtr = Context.StepIdToIndex.Find(Resolved.StepId);
		if (!PlanIndexPtr)
		{
			continue;
		}

		const FOliveIRBlueprintPlanStep& PlanStep = Context.Plan.Steps[*PlanIndexPtr];

		// Check for Target input -- either a @ref or a string-literal component name
		const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
		bool bHasTargetWired = false;

		if (TargetValue && !TargetValue->IsEmpty())
		{
			if (TargetValue->StartsWith(TEXT("@")))
			{
				// @ref syntax -- AI provided an explicit component reference
				bHasTargetWired = true;
			}
			else if (Context.Blueprint->SimpleConstructionScript)
			{
				// String literal -- check if it matches an SCS component variable name.
				// This handles the case where the AI passes the component name directly
				// (e.g., "StaticMeshComp") instead of a @ref. Phase 4 data wiring treats
				// non-@ref strings as pin defaults, which won't wire a component reference,
				// BUT this is still the AI's expressed intent to target a component.
				// Phase 1.5 auto-wire will handle the actual wiring if exactly one match exists.
				const FString TargetStr = *TargetValue;
				for (USCS_Node* SCSNode : Context.Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (SCSNode && SCSNode->GetVariableName().ToString() == TargetStr)
					{
						bHasTargetWired = true;
						break;
					}
				}
			}
		}

		if (bHasTargetWired)
		{
			continue; // AI correctly targeted a component
		}

		// Count matching SCS components to determine if Phase 1.5 can auto-fix
		int32 MatchCount = 0;
		FString MatchedComponentName;
		if (Context.Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* SCSNode : Context.Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (SCSNode && SCSNode->ComponentClass &&
					SCSNode->ComponentClass->IsChildOf(Resolved.ResolvedOwningClass))
				{
					MatchCount++;
					if (MatchCount == 1)
					{
						MatchedComponentName = SCSNode->GetVariableName().ToString();
					}
				}
			}
		}

		const FString* FunctionName = Resolved.Properties.Find(TEXT("function_name"));
		const FString* ClassName = Resolved.Properties.Find(TEXT("target_class"));
		const FString FuncDisplay = FunctionName ? *FunctionName : Resolved.StepId;
		const FString ClassDisplay = ClassName ? *ClassName : TEXT("ActorComponent");

		if (MatchCount == 1)
		{
			// Exactly one match -- Phase 1.5 will auto-wire. Downgrade to warning.
			Result.Warnings.Add(FString::Printf(
				TEXT("Step '%s': Component function '%s' (%s) has no Target wire. "
					 "Will be auto-wired to component '%s'."),
				*Resolved.StepId, *FuncDisplay, *ClassDisplay, *MatchedComponentName));

			UE_LOG(LogOlivePlanValidator, Log,
				TEXT("Phase 0: Step '%s' missing Target -- will auto-wire to '%s' (1 match)"),
				*Resolved.StepId, *MatchedComponentName);
			continue; // Skip error emission -- Phase 1.5 will handle it
		}

		// MatchCount == 0 or > 1: can't auto-fix -- emit error
		FString SuggestionText;
		if (MatchCount == 0)
		{
			SuggestionText = FString::Printf(
				TEXT("No components of type '%s' found in the Blueprint's Components panel. "
					 "Add a get_var step for the component, or use GetComponentByClass:\n"
					 "  {\"step_id\":\"get_comp\", \"op\":\"call\", \"target\":\"GetComponentByClass\", "
					 "\"inputs\":{\"ComponentClass\":\"%s\"}}\n"
					 "  Then on step '%s': \"inputs\":{\"Target\":\"@get_comp.auto\"}"),
				*ClassDisplay, *ClassDisplay, *Resolved.StepId);
		}
		else
		{
			SuggestionText = FString::Printf(
				TEXT("%d components of type '%s' found. Ambiguous -- wire Target explicitly:\n"
					 "  {\"step_id\":\"get_comp\", \"op\":\"get_var\", \"target\":\"<component_name>\"}\n"
					 "  Then on step '%s': \"inputs\":{\"Target\":\"@get_comp.auto\"}"),
				MatchCount, *ClassDisplay, *Resolved.StepId);
		}

		Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("COMPONENT_FUNCTION_ON_ACTOR"),
			Resolved.StepId,
			FString::Printf(TEXT("/steps/%d/target"), *PlanIndexPtr),
			FString::Printf(
				TEXT("Function '%s' belongs to component class '%s', but this Blueprint "
					 "inherits from Actor. Without a Target pin wired to a component "
					 "reference, this will target Self (the Actor), causing a compile error."),
				*FuncDisplay, *ClassDisplay),
			SuggestionText));

		UE_LOG(LogOlivePlanValidator, Warning,
			TEXT("Phase 0: Step '%s' calls component function '%s' (%s) on Actor BP without Target wire (%d matches)"),
			*Resolved.StepId, *FuncDisplay, *ClassDisplay, MatchCount);
	}
}

// ============================================================================
// Check 2: Exec Wiring Conflict Detection
// ============================================================================

void FOlivePlanValidator::CheckExecWiringConflicts(
	const FOlivePlanValidationContext& Context,
	FOlivePlanValidationResult& Result)
{
	// Build set of steps that have exec_outputs (multi-output wiring).
	TMap<FString, const FOliveIRBlueprintPlanStep*> StepsWithExecOutputs;
	for (const FOliveIRBlueprintPlanStep& Step : Context.Plan.Steps)
	{
		if (Step.ExecOutputs.Num() > 0)
		{
			StepsWithExecOutputs.Add(Step.StepId, &Step);
		}
	}

	if (StepsWithExecOutputs.Num() == 0)
	{
		return;
	}

	// Check every step's exec_after for conflicts with exec_outputs
	for (int32 i = 0; i < Context.Plan.Steps.Num(); ++i)
	{
		const FOliveIRBlueprintPlanStep& Step = Context.Plan.Steps[i];

		if (Step.ExecAfter.IsEmpty())
		{
			continue;
		}

		const FOliveIRBlueprintPlanStep** ConflictPtr =
			StepsWithExecOutputs.Find(Step.ExecAfter);

		if (!ConflictPtr)
		{
			continue; // exec_after target doesn't use exec_outputs
		}

		const FOliveIRBlueprintPlanStep& ConflictStep = **ConflictPtr;

		// Build readable description of the exec_outputs
		FString ExecOutputsDesc;
		for (const auto& ExecOut : ConflictStep.ExecOutputs)
		{
			if (!ExecOutputsDesc.IsEmpty())
			{
				ExecOutputsDesc += TEXT(", ");
			}
			ExecOutputsDesc += FString::Printf(TEXT("%s -> %s"),
				*ExecOut.Key, *ExecOut.Value);
		}

		Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("EXEC_WIRING_CONFLICT"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/exec_after"), i),
			FString::Printf(
				TEXT("Step '%s' has exec_after:'%s', but '%s' already uses exec_outputs "
					 "to wire its output pins (%s). The primary exec output pin would be "
					 "double-claimed, leaving '%s' disconnected from the exec flow."),
				*Step.StepId, *Step.ExecAfter,
				*ConflictStep.StepId, *ExecOutputsDesc,
				*Step.StepId),
			FString::Printf(
				TEXT("Remove exec_after:'%s' from step '%s'. Instead, add '%s' to "
					 "'%s'.exec_outputs for the appropriate branch, then chain "
					 "subsequent steps via exec_after from '%s'."),
				*Step.ExecAfter, *Step.StepId,
				*Step.StepId, *ConflictStep.StepId,
				*Step.StepId)));

		UE_LOG(LogOlivePlanValidator, Warning,
			TEXT("Phase 0: Step '%s' exec_after conflicts with '%s' exec_outputs (%s)"),
			*Step.StepId, *ConflictStep.StepId, *ExecOutputsDesc);
	}
}

// ============================================================================
// Check 3: Latent-in-Function-Graph Guard
// ============================================================================

void FOlivePlanValidator::CheckLatentInFunctionGraph(
	const FOlivePlanValidationContext& Context,
	const TArray<FOliveResolvedStep>& ResolvedSteps,
	const FOliveGraphContext& GraphContext,
	FOlivePlanValidationResult& Result)
{
	if (!GraphContext.bIsFunctionGraph)
	{
		return; // Only relevant for function graphs
	}

	for (int32 i = 0; i < ResolvedSteps.Num(); ++i)
	{
		if (!ResolvedSteps[i].bIsLatent)
		{
			continue;
		}

		const FString& StepId = Context.Plan.Steps.IsValidIndex(i)
			? Context.Plan.Steps[i].StepId
			: ResolvedSteps[i].StepId;

		// Determine the latent function name for the error message
		FString LatentName = TEXT("latent action");
		if (Context.Plan.Steps.IsValidIndex(i)
			&& Context.Plan.Steps[i].Op == OlivePlanOps::Delay)
		{
			LatentName = TEXT("Delay");
		}
		else if (const FString* FN = ResolvedSteps[i].Properties.Find(TEXT("function_name")))
		{
			LatentName = *FN;
		}

		Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("LATENT_IN_FUNCTION"),
			StepId,
			FString::Printf(TEXT("/steps/%d"), i),
			FString::Printf(
				TEXT("Step '%s' uses latent function '%s' which cannot be used in function graph '%s'. "
					 "Latent calls (Delay, AI MoveTo, etc.) are only allowed in EventGraph. "
					 "If you need latent behavior, use a Custom Event in EventGraph instead of a function."),
				*StepId, *LatentName, *GraphContext.GraphName),
			TEXT("Move this logic to a custom_event in EventGraph, or use SetTimerByFunction as a non-latent alternative.")));

		UE_LOG(LogOlivePlanValidator, Warning,
			TEXT("Phase 0: LATENT_IN_FUNCTION -- step '%s' uses '%s' in function graph '%s'"),
			*StepId, *LatentName, *GraphContext.GraphName);
	}
}

// ============================================================================
// Check 4: Variable Existence Guard
// ============================================================================

namespace
{
	/** Duplicated from OliveBlueprintPlanResolver.cpp anonymous namespace.
	 *  Checks NewVariables, SCS component variable names, and WidgetTree variables. */
	bool ValidatorBlueprintHasVariable(const UBlueprint* Blueprint, const FString& VariableName)
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

		// Check WidgetTree variables (UMG Widget Blueprints)
		const UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint);
		if (WidgetBP && WidgetBP->WidgetTree)
		{
			TArray<UWidget*> AllWidgets;
			WidgetBP->WidgetTree->GetAllWidgets(AllWidgets);
			for (const UWidget* Widget : AllWidgets)
			{
				if (Widget && Widget->bIsVariable && Widget->GetName() == VariableName)
				{
					return true;
				}
			}
		}

		return false;
	}
}

void FOlivePlanValidator::CheckVariableExists(
	const FOlivePlanValidationContext& Context,
	FOlivePlanValidationResult& Result)
{
	for (int32 i = 0; i < Context.ResolvedSteps.Num(); ++i)
	{
		const FOliveResolvedStep& Resolved = Context.ResolvedSteps[i];

		// Only check get_var and set_var ops
		const int32* PlanIndexPtr = Context.StepIdToIndex.Find(Resolved.StepId);
		if (!PlanIndexPtr || !Context.Plan.Steps.IsValidIndex(*PlanIndexPtr))
		{
			continue;
		}

		const FOliveIRBlueprintPlanStep& PlanStep = Context.Plan.Steps[*PlanIndexPtr];
		if (PlanStep.Op != OlivePlanOps::GetVar && PlanStep.Op != OlivePlanOps::SetVar)
		{
			continue;
		}

		// Skip function parameter steps (FunctionInput/FunctionOutput)
		if (Resolved.NodeType == OliveNodeTypes::FunctionInput ||
			Resolved.NodeType == OliveNodeTypes::FunctionOutput)
		{
			continue;
		}

		// Extract variable name from resolved properties
		const FString* VarNamePtr = Resolved.Properties.Find(TEXT("variable_name"));
		if (!VarNamePtr || VarNamePtr->IsEmpty())
		{
			continue; // Missing variable name is caught by the resolver
		}
		const FString& VariableName = *VarNamePtr;

		// External variable access — property lives on another class, not the editing Blueprint.
		// The resolver sets external_class when get_var has a Target input referencing a cast step.
		// Verify the variable actually exists on that class rather than blindly skipping.
		const FString* ExternalClassPtr = Resolved.Properties.Find(TEXT("external_class"));
		if (ExternalClassPtr && !ExternalClassPtr->IsEmpty())
		{
			UClass* ExternalClass = UClass::TryFindTypeSlow<UClass>(**ExternalClassPtr);
			if (!ExternalClass)
			{
				// Try with _C suffix (Blueprint generated class naming)
				ExternalClass = UClass::TryFindTypeSlow<UClass>(
					FString::Printf(TEXT("%s_C"), **ExternalClassPtr));
			}
			if (ExternalClass)
			{
				FProperty* Prop = ExternalClass->FindPropertyByName(FName(*VariableName));
				if (Prop)
				{
					continue; // Validated: variable exists on external class
				}
				// Variable not found on external class — fall through to local Blueprint check.
				// If it's not found locally either, the VARIABLE_NOT_FOUND error below will fire.
			}
			else
			{
				continue; // Can't resolve class (likely BP-only, not yet compiled) — skip gracefully
			}
		}

		// Check on this Blueprint
		if (ValidatorBlueprintHasVariable(Context.Blueprint, VariableName))
		{
			continue; // Found directly
		}

		// Check parent Blueprint chain
		UBlueprint* ParentBP = Context.Blueprint->ParentClass
			? Cast<UBlueprint>(Context.Blueprint->ParentClass->ClassGeneratedBy)
			: nullptr;
		bool bFoundInParent = false;
		while (ParentBP)
		{
			if (ValidatorBlueprintHasVariable(ParentBP, VariableName))
			{
				bFoundInParent = true;
				break;
			}
			ParentBP = ParentBP->ParentClass
				? Cast<UBlueprint>(ParentBP->ParentClass->ClassGeneratedBy)
				: nullptr;
		}

		if (bFoundInParent)
		{
			continue; // Inherited variable -- valid
		}

		// Check native C++ properties on the generated class
		if (Context.Blueprint->GeneratedClass)
		{
			FProperty* Prop = Context.Blueprint->GeneratedClass->FindPropertyByName(FName(*VariableName));
			if (Prop)
			{
				continue; // Native property -- valid
			}
		}

		// Check WidgetTree variables (UMG Widget Blueprints)
		{
			bool bFoundInWidgetTree = false;
			const UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Context.Blueprint);
			if (WBP && WBP->WidgetTree)
			{
				TArray<UWidget*> AllWidgets;
				WBP->WidgetTree->GetAllWidgets(AllWidgets);
				for (const UWidget* Widget : AllWidgets)
				{
					if (Widget && Widget->bIsVariable && Widget->GetName() == VariableName)
					{
						bFoundInWidgetTree = true;
						break;
					}
				}
			}
			if (bFoundInWidgetTree)
			{
				continue; // WidgetTree variable -- valid
			}
		}

		// Variable not found anywhere -- build error with available variable list
		TArray<FString> AvailableVars;
		for (const FBPVariableDescription& Var : Context.Blueprint->NewVariables)
		{
			AvailableVars.Add(Var.VarName.ToString());
		}
		if (Context.Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* SCSNode : Context.Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (SCSNode)
				{
					AvailableVars.Add(SCSNode->GetVariableName().ToString());
				}
			}
		}
		// Add WidgetTree variables (UMG Widget Blueprints)
		{
			const UWidgetBlueprint* AvailWBP = Cast<UWidgetBlueprint>(Context.Blueprint);
			if (AvailWBP && AvailWBP->WidgetTree)
			{
				TArray<UWidget*> AllWidgets;
				AvailWBP->WidgetTree->GetAllWidgets(AllWidgets);
				for (const UWidget* Widget : AllWidgets)
				{
					if (Widget && Widget->bIsVariable)
					{
						AvailableVars.Add(Widget->GetName());
					}
				}
			}
		}
		AvailableVars.Sort();

		// Truncate to first 10 for readability
		FString AvailableList;
		const int32 MaxDisplay = FMath::Min(AvailableVars.Num(), 10);
		for (int32 j = 0; j < MaxDisplay; ++j)
		{
			if (!AvailableList.IsEmpty())
			{
				AvailableList += TEXT(", ");
			}
			AvailableList += AvailableVars[j];
		}
		if (AvailableVars.Num() > 10)
		{
			AvailableList += FString::Printf(TEXT(" (+%d more)"), AvailableVars.Num() - 10);
		}

		// Check whether any cast step in the plan resolved to a class that HAS
		// this variable. If so, emit a more specific "cross-BP access" error.
		FString CrossBPNote;
		for (const FOliveResolvedStep& OtherStep : Context.ResolvedSteps)
		{
			if (OtherStep.NodeType != OliveNodeTypes::Cast)
			{
				continue;
			}

			const FString* CastTargetClassNamePtr = OtherStep.Properties.Find(TEXT("target_class"));
			if (!CastTargetClassNamePtr || CastTargetClassNamePtr->IsEmpty())
			{
				continue;
			}

			FOliveClassResolveResult ClassResolve = FOliveClassResolver::Resolve(*CastTargetClassNamePtr);
			if (!ClassResolve.IsValid())
			{
				continue;
			}

			FProperty* Prop = ClassResolve.Class->FindPropertyByName(FName(*VariableName));
			if (Prop)
			{
				CrossBPNote = FString::Printf(
					TEXT(" Variable '%s' exists on cast target class '%s' (from step '%s'). "
						 "Use get_var with Target to access it: "
						 "{\"op\":\"get_var\",\"target\":\"%s\",\"inputs\":{\"Target\":\"@%s.auto\"}}"),
					*VariableName,
					**CastTargetClassNamePtr,
					*OtherStep.StepId,
					*VariableName,
					*OtherStep.StepId);
				break;
			}
		}

		// Special-case "self" — guide AI to use @self.auto in step inputs instead of get_var
		const bool bIsSelfRef = VariableName.Equals(TEXT("self"), ESearchCase::IgnoreCase)
			|| VariableName.Equals(TEXT("Self"), ESearchCase::IgnoreCase);

		FString Suggestion;
		if (bIsSelfRef)
		{
			Suggestion = TEXT("To reference the Blueprint itself, use \"Target\": \"@self.auto\" in step inputs "
				"(e.g., {\"op\":\"call\",\"target\":\"SetTimer\",\"inputs\":{\"Target\":\"@self.auto\"}}). "
				"Do not use get_var for self references.");
		}
		else if (!CrossBPNote.IsEmpty())
		{
			Suggestion = TEXT("Use get_var with Target to access external properties: "
				"{\"op\":\"get_var\",\"target\":\"VarName\",\"inputs\":{\"Target\":\"@cast_step.auto\"}}");
		}
		else
		{
			Suggestion = FString::Printf(
				TEXT("Add the variable first with blueprint.add_variable, or check the variable name. "
					 "Available variables: [%s]"),
				*AvailableList);
		}

		Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("VARIABLE_NOT_FOUND"),
			Resolved.StepId,
			FString::Printf(TEXT("/steps/%d/target"), *PlanIndexPtr),
			FString::Printf(
				TEXT("Variable '%s' not found on Blueprint '%s' or its parent classes. "
					 "get_var/set_var requires an existing variable. Components use their SCS variable name.%s"),
				*VariableName, *Context.Blueprint->GetName(), *CrossBPNote),
			Suggestion));

		UE_LOG(LogOlivePlanValidator, Warning,
			TEXT("Phase 0: VARIABLE_NOT_FOUND -- step '%s' references '%s' on '%s'"),
			*Resolved.StepId, *VariableName, *Context.Blueprint->GetName());
	}
}

// ============================================================================
// Check 5: Exec Source Is Return Guard
// ============================================================================

void FOlivePlanValidator::CheckExecSourceIsReturn(
	const FOlivePlanValidationContext& Context,
	FOlivePlanValidationResult& Result)
{
	// Build set of steps that resolve to FunctionOutput (terminal nodes with no exec output)
	TSet<FString> FunctionOutputSteps;
	for (int32 i = 0; i < Context.ResolvedSteps.Num(); ++i)
	{
		if (Context.ResolvedSteps[i].NodeType == OliveNodeTypes::FunctionOutput)
		{
			FunctionOutputSteps.Add(Context.ResolvedSteps[i].StepId);
		}
	}

	if (FunctionOutputSteps.Num() == 0)
	{
		return;
	}

	// Check every step's exec_after for references to FunctionOutput steps.
	// NOTE: exec_outputs targeting a FunctionOutput step is valid — that wires
	// INTO the return node's exec input. Only exec_after is invalid, because it
	// tries to chain FROM the return node's (nonexistent) exec output.
	for (int32 i = 0; i < Context.Plan.Steps.Num(); ++i)
	{
		const FOliveIRBlueprintPlanStep& Step = Context.Plan.Steps[i];

		if (Step.ExecAfter.IsEmpty())
		{
			continue;
		}

		if (!FunctionOutputSteps.Contains(Step.ExecAfter))
		{
			continue;
		}

		Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("EXEC_SOURCE_IS_RETURN"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/exec_after"), i),
			FString::Printf(
				TEXT("Step '%s' has exec_after:'%s', but '%s' resolves to a FunctionResult node "
					 "(return/output parameter). FunctionResult is a terminal node — it has an "
					 "exec input but NO exec output pin. Nothing can chain after it."),
				*Step.StepId, *Step.ExecAfter, *Step.ExecAfter),
			FString::Printf(
				TEXT("Wire exec_after to the step BEFORE '%s' in the exec chain, not to the return step itself. "
					 "The return step should be the LAST step in the chain."),
				*Step.ExecAfter)));

		UE_LOG(LogOlivePlanValidator, Warning,
			TEXT("Phase 0: EXEC_SOURCE_IS_RETURN -- step '%s' exec_after targets FunctionOutput step '%s'"),
			*Step.StepId, *Step.ExecAfter);
	}
}

// ============================================================================
// Check 6: Collision-on-Trigger Heuristic
// ============================================================================

namespace
{
	/**
	 * Traces a plan step's "Target" input back to a component variable name.
	 * Handles two patterns:
	 *   - @ref syntax: "@get_sphere.auto" -> looks up get_sphere step -> returns its Target
	 *   - Literal name: "SphereComp" -> returns as-is
	 *
	 * Intentionally simple (no multi-hop trace). This is a heuristic nudge, not a guarantee.
	 */
	FString ResolveTargetComponentName(
		const FOliveIRBlueprintPlanStep& PlanStep,
		const FOlivePlanValidationContext& Context)
	{
		const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
		if (!TargetValue || TargetValue->IsEmpty())
		{
			return FString();
		}

		// Case 1: @ref syntax (e.g., "@get_sphere.auto")
		if (TargetValue->StartsWith(TEXT("@")))
		{
			// Extract step ID: strip "@", split on "."
			FString RefPart = TargetValue->Mid(1); // Strip leading "@"
			FString RefStepId;
			FString PinPart;
			if (!RefPart.Split(TEXT("."), &RefStepId, &PinPart))
			{
				RefStepId = RefPart; // No "." -- entire thing is the step ID
			}

			const int32* RefIndexPtr = Context.StepIdToIndex.Find(RefStepId);
			if (!RefIndexPtr || !Context.Plan.Steps.IsValidIndex(*RefIndexPtr))
			{
				return FString();
			}

			const FOliveIRBlueprintPlanStep& RefStep = Context.Plan.Steps[*RefIndexPtr];
			if (RefStep.Op == OlivePlanOps::GetVar)
			{
				return RefStep.Target;
			}

			return FString();
		}

		// Case 2: Literal component variable name
		return *TargetValue;
	}
}

void FOlivePlanValidator::CheckCollisionOnTriggerComponent(
	const FOlivePlanValidationContext& Context,
	FOlivePlanValidationResult& Result)
{
	// --- Gate: need SCS ---
	USimpleConstructionScript* SCS = Context.Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return;
	}

	// --- Collect SCS component info (single pass) ---
	// TriggerComponents: varName -> (varName, class) for sphere/capsule components
	// MeshComponents: (varName, class) for static/skeletal mesh components
	TMap<FString, TPair<FString, UClass*>> TriggerComponents;
	TArray<TPair<FString, UClass*>> MeshComponents;

	for (USCS_Node* SCSNode : SCS->GetAllNodes())
	{
		if (!SCSNode || !SCSNode->ComponentClass)
		{
			continue;
		}

		const FString VarName = SCSNode->GetVariableName().ToString();
		UClass* CompClass = SCSNode->ComponentClass;

		if (CompClass->IsChildOf(USphereComponent::StaticClass()) ||
			CompClass->IsChildOf(UCapsuleComponent::StaticClass()))
		{
			TriggerComponents.Add(VarName, TPair<FString, UClass*>(VarName, CompClass));
		}

		if (CompClass->IsChildOf(UStaticMeshComponent::StaticClass()) ||
			CompClass->IsChildOf(USkeletalMeshComponent::StaticClass()))
		{
			MeshComponents.Add(TPair<FString, UClass*>(VarName, CompClass));
		}
	}

	// --- Early exit: need BOTH trigger and mesh components ---
	if (TriggerComponents.Num() == 0 || MeshComponents.Num() == 0)
	{
		return;
	}

	// --- Build pickup context signals from the plan (single pass over steps) ---
	static const TSet<FString> CollisionFunctions = {
		TEXT("SetCollisionEnabled"),
		TEXT("SetCollisionResponseToAllChannels"),
		TEXT("SetCollisionProfileName")
	};

	static const TSet<FString> AttachFunctions = {
		TEXT("AttachToComponent"),
		TEXT("AttachActorToComponent"),
		TEXT("K2_AttachToComponent"),
		TEXT("AttachToActor"),
		TEXT("K2_AttachToActor"),
		TEXT("AttachComponentToComponent")
	};

	static const TArray<FString> PickupVarKeywords = {
		TEXT("equip"), TEXT("pickup"), TEXT("collect"), TEXT("grab"), TEXT("held")
	};

	static const TSet<FString> CleanupFunctions = {
		TEXT("DestroyActor"),
		TEXT("K2_DestroyActor"),
		TEXT("SetActorHiddenInGame")
	};

	bool bHasAttachCall = false;
	bool bHasOverlapEvent = false;
	bool bHasEquipVariable = false;
	bool bHasDestroyOrHide = false;
	int32 CollisionStepCount = 0;
	TSet<FString> CollisionTargetComponents;

	for (int32 i = 0; i < Context.Plan.Steps.Num(); ++i)
	{
		const FOliveIRBlueprintPlanStep& PlanStep = Context.Plan.Steps[i];

		// Resolve function name from resolved properties, falling back to raw Target
		FString FuncName;
		if (i < Context.ResolvedSteps.Num())
		{
			const FString* ResolvedFuncName = Context.ResolvedSteps[i].Properties.Find(TEXT("function_name"));
			if (ResolvedFuncName)
			{
				FuncName = *ResolvedFuncName;
			}
		}
		if (FuncName.IsEmpty())
		{
			FuncName = PlanStep.Target;
		}

		if (PlanStep.Op == OlivePlanOps::Call && !FuncName.IsEmpty())
		{
			if (CollisionFunctions.Contains(FuncName))
			{
				CollisionStepCount++;
				const FString TargetComp = ResolveTargetComponentName(PlanStep, Context);
				if (!TargetComp.IsEmpty())
				{
					CollisionTargetComponents.Add(TargetComp);
				}
			}

			if (AttachFunctions.Contains(FuncName))
			{
				bHasAttachCall = true;
			}

			if (CleanupFunctions.Contains(FuncName))
			{
				bHasDestroyOrHide = true;
			}
		}

		if (PlanStep.Op == OlivePlanOps::Event)
		{
			if (PlanStep.Target.Contains(TEXT("Overlap"), ESearchCase::IgnoreCase))
			{
				bHasOverlapEvent = true;
			}
		}

		if (PlanStep.Op == OlivePlanOps::SetVar)
		{
			const FString LowerTarget = PlanStep.Target.ToLower();
			for (const FString& Keyword : PickupVarKeywords)
			{
				if (LowerTarget.Contains(Keyword))
				{
					bHasEquipVariable = true;
					break;
				}
			}
		}
	}

	// --- Check condition 4: pickup/equip context ---
	const bool bHasPickupContext = bHasAttachCall || bHasOverlapEvent || bHasEquipVariable
		|| bHasDestroyOrHide
		|| (CollisionStepCount >= 2 && CollisionTargetComponents.Num() >= 2);

	if (!bHasPickupContext)
	{
		return;
	}

	// --- Find collision steps targeting trigger components and emit warnings ---
	for (int32 i = 0; i < Context.Plan.Steps.Num(); ++i)
	{
		const FOliveIRBlueprintPlanStep& PlanStep = Context.Plan.Steps[i];
		if (PlanStep.Op != OlivePlanOps::Call)
		{
			continue;
		}

		FString FuncName;
		if (i < Context.ResolvedSteps.Num())
		{
			const FString* ResolvedFuncName = Context.ResolvedSteps[i].Properties.Find(TEXT("function_name"));
			if (ResolvedFuncName)
			{
				FuncName = *ResolvedFuncName;
			}
		}
		if (FuncName.IsEmpty())
		{
			FuncName = PlanStep.Target;
		}

		if (!CollisionFunctions.Contains(FuncName))
		{
			continue;
		}

		const FString TargetComp = ResolveTargetComponentName(PlanStep, Context);
		if (TargetComp.IsEmpty())
		{
			continue;
		}

		const TPair<FString, UClass*>* TriggerInfo = TriggerComponents.Find(TargetComp);
		if (!TriggerInfo)
		{
			continue;
		}

		// This collision call targets a trigger component in a pickup context -- emit warning
		const TPair<FString, UClass*>& MeshInfo = MeshComponents[0];

		const FString WarningMessage = FString::Printf(
			TEXT("Step '%s': %s targets '%s' (%s), which is typically an overlap trigger. "
				 "Did you mean to target '%s' (%s)? In pickup/equip patterns, the mesh component "
				 "usually needs collision changes, not the trigger volume."),
			*PlanStep.StepId,
			*FuncName,
			*TriggerInfo->Key,
			*TriggerInfo->Value->GetName(),
			*MeshInfo.Key,
			*MeshInfo.Value->GetName());

		Result.Warnings.Add(WarningMessage);

		UE_LOG(LogOlivePlanValidator, Warning,
			TEXT("Phase 0: COLLISION_ON_TRIGGER_COMPONENT -- %s"), *WarningMessage);
	}
}
