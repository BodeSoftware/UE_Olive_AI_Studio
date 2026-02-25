// Copyright Bode Software. All Rights Reserved.

#include "Plan/OlivePlanValidator.h"
#include "Plan/OliveBlueprintPlanResolver.h"
#include "Engine/Blueprint.h"
#include "Components/ActorComponent.h"

DEFINE_LOG_CATEGORY(LogOlivePlanValidator);

// ============================================================================
// Validate (public entry point)
// ============================================================================

FOlivePlanValidationResult FOlivePlanValidator::Validate(
	const FOliveIRBlueprintPlan& Plan,
	const TArray<FOliveResolvedStep>& ResolvedSteps,
	UBlueprint* Blueprint)
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

		// Check for Target input with a @ref (meaning AI provided a component reference)
		const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
		const bool bHasTargetWired = TargetValue && TargetValue->StartsWith(TEXT("@"));

		if (bHasTargetWired)
		{
			continue; // AI correctly wired a component target
		}

		// ERROR: Component function on Actor BP without Target wire
		const FString* FunctionName = Resolved.Properties.Find(TEXT("function_name"));
		const FString* ClassName = Resolved.Properties.Find(TEXT("target_class"));

		const FString FuncDisplay = FunctionName ? *FunctionName : Resolved.StepId;
		const FString ClassDisplay = ClassName ? *ClassName : TEXT("ActorComponent");

		Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("COMPONENT_FUNCTION_ON_ACTOR"),
			Resolved.StepId,
			FString::Printf(TEXT("/steps/%d/target"), *PlanIndexPtr),
			FString::Printf(
				TEXT("Function '%s' belongs to component class '%s', but this Blueprint "
					 "inherits from Actor. Without a Target pin wired to a component "
					 "reference, this will target Self (the Actor), causing a compile error."),
				*FuncDisplay, *ClassDisplay),
			FString::Printf(
				TEXT("Add a GetComponentByClass step first, then wire its output to Target:\n"
					 "  {\"step_id\":\"get_comp\", \"op\":\"call\", \"target\":\"GetComponentByClass\", "
					 "\"inputs\":{\"ComponentClass\":\"%s\"}}\n"
					 "  Then on step '%s': \"inputs\":{\"Target\":\"@get_comp.auto\"}"),
				*ClassDisplay, *Resolved.StepId)));

		UE_LOG(LogOlivePlanValidator, Warning,
			TEXT("Phase 0: Step '%s' calls component function '%s' (%s) on Actor BP without Target wire"),
			*Resolved.StepId, *FuncDisplay, *ClassDisplay);
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
