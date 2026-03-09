// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintPlanIR.h"
#include "Plan/OliveBlueprintPlanResolver.h" // For FOliveGraphContext (needed by default parameter)

class UBlueprint;

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePlanValidator, Log, All);

/**
 * Input context for Phase 0 plan validation rules.
 * Provides read-only access to the plan, resolved steps, and Blueprint.
 */
struct OLIVEAIEDITOR_API FOlivePlanValidationContext
{
	/** The original plan (for step-level data: Inputs, ExecAfter, ExecOutputs) */
	const FOliveIRBlueprintPlan& Plan;

	/** The resolved steps (for node types, properties, and ResolvedOwningClass) */
	const TArray<FOliveResolvedStep>& ResolvedSteps;

	/** The target Blueprint (for parent class checks) */
	UBlueprint* Blueprint;

	/** Lookup: StepId -> index into Plan.Steps */
	TMap<FString, int32> StepIdToIndex;
};

/**
 * Result of Phase 0 plan validation.
 */
struct OLIVEAIEDITOR_API FOlivePlanValidationResult
{
	/** Whether all checks passed */
	bool bSuccess = true;

	/** Errors that block execution (structural invariant violations) */
	TArray<FOliveIRBlueprintPlanError> Errors;

	/** Non-fatal warnings */
	TArray<FString> Warnings;
};

/**
 * FOlivePlanValidator
 *
 * Phase 0: Structural plan validation after resolution but before execution.
 * Checks plan invariants that cannot be detected during per-step resolution
 * or during execution (too late -- nodes already created).
 *
 * Current checks:
 *   - Component function target guard (COMPONENT_FUNCTION_ON_ACTOR)
 *   - Exec wiring conflict detection (EXEC_WIRING_CONFLICT)
 *   - Latent-in-function-graph guard (LATENT_IN_FUNCTION)
 *   - Variable existence guard (VARIABLE_NOT_FOUND)
 *
 * Extensible: add new checks as private static methods and call from Validate().
 *
 * Thread Safety: All methods are static and thread-safe (read-only access).
 */
class OLIVEAIEDITOR_API FOlivePlanValidator
{
public:
	/**
	 * Run all Phase 0 validation checks on a resolved plan.
	 * @param Plan The original plan (for Inputs, ExecAfter, ExecOutputs)
	 * @param ResolvedSteps The resolved steps from FOliveBlueprintPlanResolver::Resolve()
	 * @param Blueprint The target Blueprint
	 * @return Validation result with errors and warnings
	 */
	static FOlivePlanValidationResult Validate(
		const FOliveIRBlueprintPlan& Plan,
		const TArray<FOliveResolvedStep>& ResolvedSteps,
		UBlueprint* Blueprint,
		const FOliveGraphContext& GraphContext = FOliveGraphContext());

	/**
	 * Auto-fix exec_after/exec_outputs conflicts.
	 * When a step has exec_after targeting a step that also uses exec_outputs,
	 * the primary exec output pin would be double-claimed. This method removes
	 * the redundant exec_after, since exec_outputs already controls the flow.
	 *
	 * Must be called BEFORE Validate() to prevent EXEC_WIRING_CONFLICT errors.
	 *
	 * @param Plan The plan to fix (modified in place)
	 * @param OutNotes Resolver notes appended for each auto-fix (transparency)
	 * @return True if any conflicts were auto-fixed
	 */
	static bool AutoFixExecConflicts(
		FOliveIRBlueprintPlan& Plan,
		TArray<FOliveResolverNote>& OutNotes);

private:
	/**
	 * Check 1: Component function target guard.
	 * Rejects call ops where the resolved function belongs to a UActorComponent
	 * subclass, the Blueprint is NOT a component, and no Target input is wired.
	 */
	static void CheckComponentFunctionTargets(
		const FOlivePlanValidationContext& Context,
		FOlivePlanValidationResult& Result);

	/**
	 * Check 2: Exec wiring conflict detection.
	 * Rejects when exec_after targets a step that uses exec_outputs,
	 * causing the primary exec output pin to be double-claimed.
	 */
	static void CheckExecWiringConflicts(
		const FOlivePlanValidationContext& Context,
		FOlivePlanValidationResult& Result);

	/**
	 * Check 3: Latent-in-function-graph guard.
	 * Rejects latent actions (Delay, AI MoveTo, etc.) in function graphs.
	 * Latent functions require the calling context to suspend execution, which
	 * is only possible in event graph execution contexts, not function calls.
	 */
	static void CheckLatentInFunctionGraph(
		const FOlivePlanValidationContext& Context,
		const TArray<FOliveResolvedStep>& ResolvedSteps,
		const FOliveGraphContext& GraphContext,
		FOlivePlanValidationResult& Result);

	/**
	 * Check 4: Variable existence guard.
	 * Rejects get_var/set_var steps that reference variables not found on the
	 * Blueprint, its parent chain, SCS components, or native generated class.
	 */
	static void CheckVariableExists(
		const FOlivePlanValidationContext& Context,
		FOlivePlanValidationResult& Result);
};
