// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintPlanIR.h"
#include "IR/CommonIR.h"
#include "Dom/JsonObject.h"

class UBlueprint;

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePlanResolver, Log, All);

/**
 * Records a single resolver translation for transparency.
 * These are serialized into the tool result so the AI can see
 * what the resolver did silently. They also appear in the UE log.
 */
struct OLIVEAIEDITOR_API FOliveResolverNote
{
	/** The plan field that was transformed (e.g., "target", "inputs.Location") */
	FString Field;

	/** What the AI wrote */
	FString OriginalValue;

	/** What the resolver produced */
	FString ResolvedValue;

	/** Human-readable explanation of why */
	FString Reason;

	/** Serialize this note to a JSON object for inclusion in tool results */
	TSharedPtr<FJsonObject> ToJson() const
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("field"), Field);
		Obj->SetStringField(TEXT("original"), OriginalValue);
		Obj->SetStringField(TEXT("resolved"), ResolvedValue);
		Obj->SetStringField(TEXT("reason"), Reason);
		return Obj;
	}
};

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

	/** Per-step resolver notes explaining transformations applied to this step */
	TArray<FOliveResolverNote> ResolverNotes;

	/**
	 * The owning class of the resolved function (for call ops only).
	 * Set by the resolver when a UFunction* match is found.
	 * NOT serialized — only valid during the same execution context.
	 * Used by Phase 0 validation to check class hierarchy.
	 */
	UClass* ResolvedOwningClass = nullptr;
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

	/** Plan-level resolver notes (e.g., synthetic step insertions from ExpandPlanInputs) */
	TArray<FOliveResolverNote> GlobalNotes;
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
	 * @return 8-char lowercase hex fingerprint (SHA1-based, truncated for LLM friendliness)
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

	/**
	 * Pre-process plan to expand high-level inputs into concrete Blueprint operations.
	 * Currently handles: SpawnActor Location/Rotation -> MakeTransform synthesis.
	 *
	 * When the AI writes a spawn_actor step with "Location" or "Rotation" in its
	 * Inputs, this method synthesizes a make_struct(Transform) step and rewires
	 * the SpawnTransform pin. This lets the AI think in natural terms (Location/Rotation)
	 * while the resolver handles UE's Transform pin requirement.
	 *
	 * If the step already has a "SpawnTransform" input, no expansion occurs
	 * (explicit wins over implicit).
	 *
	 * @param Plan The plan to expand (modified in place)
	 * @param OutNotes Resolver notes for transparency (appended, not cleared)
	 * @return True if any expansions were made
	 */
	static bool ExpandPlanInputs(
		FOliveIRBlueprintPlan& Plan,
		TArray<FOliveResolverNote>& OutNotes);

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
		TArray<FOliveIRBlueprintPlanError>& Errors,
		TArray<FString>& Warnings);

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
