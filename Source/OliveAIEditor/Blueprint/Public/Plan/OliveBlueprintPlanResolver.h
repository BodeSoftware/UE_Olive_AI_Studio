// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintPlanIR.h"
#include "IR/CommonIR.h"

class FJsonObject;
class UBlueprint;

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePlanResolver, Log, All);

/**
 * Result of resolving a single plan step to a concrete node type.
 * Contains the OliveNodeTypes constant and all properties needed
 * by FOliveNodeFactory::CreateNode to instantiate the node.
 */
struct OLIVEAIEDITOR_API FOliveResolvedStep
{
	/** Step ID from the original plan step */
	FString StepId;

	/** Concrete node type constant (e.g., OliveNodeTypes::CallFunction) */
	FString NodeType;

	/** Properties ready for FOliveNodeFactory::CreateNode */
	TMap<FString, FString> Properties;
};

/**
 * Result of resolving an entire Blueprint plan.
 * If bSuccess is false, at least one step failed to resolve and the
 * Errors array contains structured diagnostics for each failure.
 */
struct OLIVEAIEDITOR_API FOlivePlanResolveResult
{
	/** Whether all steps resolved successfully */
	bool bSuccess = false;

	/** One resolved step per successfully resolved plan step */
	TArray<FOliveResolvedStep> ResolvedSteps;

	/** Structured errors for steps that failed to resolve */
	TArray<FOliveIRBlueprintPlanError> Errors;

	/** Non-fatal warnings (e.g., variable not found on Blueprint but may be inherited) */
	TArray<FString> Warnings;
};

/**
 * FOliveBlueprintPlanResolver
 *
 * Resolves intent-level plan operations (e.g., "call PrintString") to concrete
 * Blueprint node types (e.g., OliveNodeTypes::CallFunction with function_name property).
 *
 * This is a stateless service -- all state comes from the plan and Blueprint
 * passed to Resolve(). Given the same inputs, Resolve() always produces the
 * same output (deterministic).
 *
 * Resolution flow per step:
 * 1. Map the Op string (from OlivePlanOps vocabulary) to a per-op resolver
 * 2. The per-op resolver determines the concrete OliveNodeTypes constant
 * 3. Properties are assembled from Step.Target, Step.TargetClass, and Step.Properties
 * 4. For ambiguous ops (e.g., "call"), FOliveNodeCatalog is consulted for disambiguation
 *
 * Thread Safety: All methods are static and thread-safe (reads only from catalog/Blueprint).
 */
class OLIVEAIEDITOR_API FOliveBlueprintPlanResolver
{
public:
	/**
	 * Resolve all steps in a plan to concrete node types.
	 * @param Plan The validated plan (should pass schema validation first)
	 * @param Blueprint The target Blueprint (used for variable/event validation)
	 * @return Resolution result with resolved steps or errors
	 */
	static FOlivePlanResolveResult Resolve(
		const FOliveIRBlueprintPlan& Plan,
		UBlueprint* Blueprint);

	/**
	 * Compute a deterministic fingerprint from current graph state and plan content.
	 * Used for drift detection: if the graph changes between preview and apply,
	 * the fingerprint will no longer match, indicating the plan may be stale.
	 * @param CurrentGraph The current graph IR (from FOliveGraphReader)
	 * @param Plan The plan being previewed/applied
	 * @return Hex string fingerprint (SHA1-based)
	 */
	static FString ComputePlanFingerprint(
		const FOliveIRGraph& CurrentGraph,
		const FOliveIRBlueprintPlan& Plan);

	/**
	 * Compute a diff summary showing what the plan would add to the graph.
	 * @param CurrentGraph The current graph IR
	 * @param ResolvedSteps The resolved plan steps (from Resolve())
	 * @param Plan The original plan (for connection info from ExecAfter/ExecOutputs/Inputs)
	 * @return JSON object with diff summary including current counts, additions, and per-step details
	 */
	static TSharedPtr<FJsonObject> ComputePlanDiff(
		const FOliveIRGraph& CurrentGraph,
		const TArray<FOliveResolvedStep>& ResolvedSteps,
		const FOliveIRBlueprintPlan& Plan);

private:
	/**
	 * Resolve a single plan step to a concrete node type.
	 * @param Step The plan step to resolve
	 * @param Blueprint The target Blueprint
	 * @param StepIndex Index of this step in the plan (for error location pointers)
	 * @param OutResolved Populated on success with the resolved step data
	 * @param OutErrors Appended with errors if resolution fails
	 * @param OutWarnings Appended with non-fatal warnings
	 * @return True if resolution succeeded
	 */
	static bool ResolveStep(
		const FOliveIRBlueprintPlanStep& Step,
		UBlueprint* Blueprint,
		int32 StepIndex,
		FOliveResolvedStep& OutResolved,
		TArray<FOliveIRBlueprintPlanError>& OutErrors,
		TArray<FString>& OutWarnings);

	// ============================================================================
	// Per-Op Resolvers
	// ============================================================================

	/** Resolve a "call" op -- function call via catalog lookup */
	static bool ResolveCallOp(
		const FOliveIRBlueprintPlanStep& Step,
		UBlueprint* BP,
		int32 Idx,
		FOliveResolvedStep& Out,
		TArray<FOliveIRBlueprintPlanError>& Errors,
		TArray<FString>& Warnings);

	/** Resolve a "get_var" op -- get variable node */
	static bool ResolveGetVarOp(
		const FOliveIRBlueprintPlanStep& Step,
		UBlueprint* BP,
		int32 Idx,
		FOliveResolvedStep& Out,
		TArray<FOliveIRBlueprintPlanError>& Errors);

	/** Resolve a "set_var" op -- set variable node */
	static bool ResolveSetVarOp(
		const FOliveIRBlueprintPlanStep& Step,
		UBlueprint* BP,
		int32 Idx,
		FOliveResolvedStep& Out,
		TArray<FOliveIRBlueprintPlanError>& Errors);

	/** Resolve an "event" op -- native event override node */
	static bool ResolveEventOp(
		const FOliveIRBlueprintPlanStep& Step,
		UBlueprint* BP,
		int32 Idx,
		FOliveResolvedStep& Out,
		TArray<FOliveIRBlueprintPlanError>& Errors);

	/** Resolve a simple op that maps directly to an OliveNodeTypes constant */
	static bool ResolveSimpleOp(
		const FOliveIRBlueprintPlanStep& Step,
		const FString& NodeType,
		FOliveResolvedStep& Out);

	/** Resolve a "cast" op -- requires target_class */
	static bool ResolveCastOp(
		const FOliveIRBlueprintPlanStep& Step,
		int32 Idx,
		FOliveResolvedStep& Out,
		TArray<FOliveIRBlueprintPlanError>& Errors);

	/** Resolve a struct op (make_struct / break_struct) -- requires struct_type */
	static bool ResolveStructOp(
		const FOliveIRBlueprintPlanStep& Step,
		const FString& NodeType,
		int32 Idx,
		FOliveResolvedStep& Out,
		TArray<FOliveIRBlueprintPlanError>& Errors);
};
