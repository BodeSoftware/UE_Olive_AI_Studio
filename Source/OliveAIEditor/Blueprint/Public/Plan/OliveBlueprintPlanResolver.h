// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintPlanIR.h"
#include "IR/CommonIR.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePlanResolver, Log, All);

/**
 * Captured context about the target graph for plan resolution and validation.
 * Built once before plan execution, consumed by resolver, validator, and executor.
 *
 * For function graphs, captures the parameter names from FunctionEntry/FunctionResult
 * so the resolver can map get_var/set_var of param names to virtual FunctionInput/FunctionOutput
 * steps instead of variable getter/setter nodes.
 */
struct OLIVEAIEDITOR_API FOliveGraphContext
{
	/** Name of the graph (e.g., "EventGraph", "Fire", "ApplyHitDamage") */
	FString GraphName;

	/** True if this is a function graph (not EventGraph, not macro) */
	bool bIsFunctionGraph = false;

	/** True if this is a macro graph */
	bool bIsMacroGraph = false;

	/** Function input parameter names -- only populated for function graphs */
	TArray<FString> InputParamNames;

	/** Function output/return parameter names -- only populated for function graphs */
	TArray<FString> OutputParamNames;

	/** The actual UEdGraph pointer (for direct inspection if needed) */
	UEdGraph* Graph = nullptr;

	/**
	 * Build context from a Blueprint and graph name.
	 * Searches UbergraphPages, FunctionGraphs, and MacroGraphs.
	 * For function graphs, scans UK2Node_FunctionEntry and UK2Node_FunctionResult
	 * to extract UserDefinedPins as input/output parameter names.
	 *
	 * @param Blueprint The Blueprint containing the graph
	 * @param GraphName The name of the target graph
	 * @return Populated context (bIsFunctionGraph/bIsMacroGraph set based on graph location)
	 */
	static FOliveGraphContext BuildFromBlueprint(UBlueprint* Blueprint, const FString& GraphName);
};

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
	 * Whether this step resolves to a pure node (no exec pins).
	 * Set by the resolver based on op type or UFunction flags.
	 * Used by CollapseExecThroughPureSteps to remove pure steps from exec chains.
	 */
	bool bIsPure = false;

	/**
	 * Whether this step uses a latent function (e.g., Delay, AI MoveTo).
	 * Latent functions cannot be used in function graphs.
	 * Set by the resolver from UFunction::HasMetaData("Latent") or op type.
	 */
	bool bIsLatent = false;

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

	/**
	 * The plan after all pre-processing expansions (ExpandComponentRefs,
	 * ExpandPlanInputs, ExpandBranchConditions). This is the plan that should
	 * be passed to the executor, NOT the original plan.
	 * Empty if resolution failed.
	 */
	FOliveIRBlueprintPlan ExpandedPlan;
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
		UBlueprint* Blueprint,
		const FOliveGraphContext& GraphContext = FOliveGraphContext());

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
	 * Post-resolve pass: collapse exec chains through pure steps.
	 *
	 * Pure nodes (get_var, make_struct, break_struct, pure function calls, etc.)
	 * have no exec pins in the Blueprint graph. When the AI writes exec_after or
	 * exec_outputs chains that flow through pure steps, the wiring phase will fail
	 * because those nodes have no exec input/output pins.
	 *
	 * This method rewires exec references to skip pure steps entirely:
	 *   exec_after pointing to a pure step → re-targeted to nearest impure predecessor
	 *   exec_outputs pointing to a pure step → re-targeted to nearest impure successor
	 *   Pure steps' own exec_after/exec_outputs → cleared
	 *
	 * Must be called AFTER Resolve() and BEFORE the executor or Phase 0 validator.
	 *
	 * @param Plan           The plan to modify in place
	 * @param ResolvedSteps  Resolved steps from Resolve() (for bIsPure flags)
	 * @param OutNotes       Resolver notes appended for each collapse (transparency)
	 * @return True if any exec references were collapsed
	 */
	static bool CollapseExecThroughPureSteps(
		FOliveIRBlueprintPlan& Plan,
		const TArray<FOliveResolvedStep>& ResolvedSteps,
		TArray<FOliveResolverNote>& OutNotes);

	/**
	 * Pre-process plan to expand dotless @refs (component/variable names) into
	 * explicit get_var steps. When the AI writes "@ComponentName" in an input,
	 * this method synthesizes a get_var step for that component and rewrites
	 * the reference to "@_synth_getcomp_xxx.auto".
	 *
	 * Also handles function parameter references in function graph context:
	 * "@ParamName.PinHint" where ParamName matches a function input parameter
	 * is rewritten to use the FunctionInput virtual step.
	 *
	 * Must be called BEFORE schema validation or after schema validation
	 * has been relaxed for dotless @refs. Runs after plan parsing, before Resolve().
	 *
	 * @param Plan The plan to expand (modified in place)
	 * @param Blueprint The target Blueprint (for SCS component lookup)
	 * @param GraphContext Graph context (for function parameter detection)
	 * @param OutNotes Resolver notes for transparency (appended, not cleared)
	 * @return True if any expansions were made
	 */
	static bool ExpandComponentRefs(
		FOliveIRBlueprintPlan& Plan,
		UBlueprint* Blueprint,
		const FOliveGraphContext& GraphContext,
		TArray<FOliveResolverNote>& OutNotes);

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

	/**
	 * Pre-process plan to expand branch conditions that reference non-boolean sources.
	 *
	 * When the AI writes a branch step whose Condition input is an @ref pointing
	 * to a get_var step that produces a non-boolean value (Integer or Float/Double),
	 * this method synthesizes a "> 0" comparison step (Greater_IntInt or
	 * Greater_DoubleDouble) and rewires the branch's Condition to the comparison result.
	 *
	 * This bridges the gap between the AI's intent ("branch on ammo count") and UE's
	 * requirement that Branch.Condition be a Boolean pin. Without this, the data wire
	 * fails at type matching because Integer/Float cannot auto-convert to Boolean.
	 *
	 * Must be called AFTER ExpandPlanInputs and BEFORE Resolve's per-step resolution.
	 *
	 * @param Plan The plan to expand (modified in place)
	 * @param Blueprint The target Blueprint (for variable type lookup)
	 * @param OutNotes Resolver notes for transparency (appended, not cleared)
	 * @return True if any expansions were made
	 */
	static bool ExpandBranchConditions(
		FOliveIRBlueprintPlan& Plan,
		UBlueprint* Blueprint,
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
		TArray<FString>& OutWarnings,
		const FOliveGraphContext& GraphContext);

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
		TArray<FString>& Warnings,
		const FOliveGraphContext& GraphContext);

	/** Resolve a "set_var" op -- set variable node */
	static bool ResolveSetVarOp(
		const FOliveIRBlueprintPlanStep& Step,
		UBlueprint* BP,
		int32 Idx,
		FOliveResolvedStep& Out,
		TArray<FOliveIRBlueprintPlanError>& Errors,
		TArray<FString>& Warnings,
		const FOliveGraphContext& GraphContext);

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

	/**
	 * Resolve a "call_delegate" op -- broadcast an event dispatcher.
	 * Validates that the target names a multicast delegate (PC_MCDelegate)
	 * variable on the Blueprint and sets up properties for
	 * FOliveNodeFactory::CreateCallDelegateNode.
	 */
	static bool ResolveCallDelegateOp(
		const FOliveIRBlueprintPlanStep& Step,
		UBlueprint* BP,
		int32 Idx,
		FOliveResolvedStep& Out,
		TArray<FOliveIRBlueprintPlanError>& Errors);
};
