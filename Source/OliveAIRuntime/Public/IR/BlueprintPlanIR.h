// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "IR/OliveCompileIR.h"
#include "BlueprintPlanIR.generated.h"

/**
 * Closed vocabulary of plan step operations.
 * Each constant maps to an intent-level operation that the resolver
 * will convert to a concrete Blueprint node type via FOliveNodeCatalog.
 */
namespace OlivePlanOps
{
	/** Call a function (UFUNCTION, library function, etc.) */
	const FString Call = TEXT("call");

	/** Get a Blueprint variable value */
	const FString GetVar = TEXT("get_var");

	/** Set a Blueprint variable value */
	const FString SetVar = TEXT("set_var");

	/** Branch node (if/else on bool condition) */
	const FString Branch = TEXT("branch");

	/** Sequence node (execute multiple outputs in order) */
	const FString Sequence = TEXT("sequence");

	/** Cast to a specific class */
	const FString Cast = TEXT("cast");

	/** Bind to a native event (e.g., BeginPlay, Tick) */
	const FString Event = TEXT("event");

	/** Create a custom event */
	const FString CustomEvent = TEXT("custom_event");

	/** Standard for loop (start index, end index) */
	const FString ForLoop = TEXT("for_loop");

	/** For-each loop over an array */
	const FString ForEachLoop = TEXT("for_each_loop");

	/** Delay node */
	const FString Delay = TEXT("delay");

	/** IsValid check node */
	const FString IsValid = TEXT("is_valid");

	/** Print string to screen/log */
	const FString PrintString = TEXT("print_string");

	/** Spawn an actor in the world */
	const FString SpawnActor = TEXT("spawn_actor");

	/** Make (construct) a struct from individual pins */
	const FString MakeStruct = TEXT("make_struct");

	/** Break a struct into individual pins */
	const FString BreakStruct = TEXT("break_struct");

	/** Return node for function graphs */
	const FString Return = TEXT("return");

	/** Comment node (non-functional annotation) */
	const FString Comment = TEXT("comment");

	/**
	 * Check whether a given op string is in the closed vocabulary.
	 * @param Op The operation string to validate
	 * @return True if Op is a recognized plan operation
	 */
	OLIVEAIRUNTIME_API bool IsValidOp(const FString& Op);

	/**
	 * Get all valid op strings as a set for fast lookup.
	 * @return Immutable set of all recognized plan operation strings
	 */
	OLIVEAIRUNTIME_API const TSet<FString>& GetAllOps();
}

/**
 * A single step in a Blueprint plan.
 * Represents an intent-level operation (e.g., "call PrintString") that
 * will be resolved to a concrete Blueprint node type by the plan resolver.
 *
 * Steps reference each other via StepId strings for execution flow and
 * data connections. The "@stepId.pin_name" syntax in Inputs denotes a
 * data wire from another step's output pin.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBlueprintPlanStep
{
	GENERATED_BODY()

	/** Unique identifier for this step within the plan (e.g., "s1", "print_node") */
	UPROPERTY()
	FString StepId;

	/** Operation from closed vocabulary (see OlivePlanOps namespace) */
	UPROPERTY()
	FString Op;

	/** Primary target: function name, variable name, or class name depending on Op */
	UPROPERTY()
	FString Target;

	/** Optional class name for disambiguation when Target is ambiguous */
	UPROPERTY()
	FString TargetClass;

	/** Pin name to value mapping. Values are either literals or "@stepId.pin_name" references */
	UPROPERTY()
	TMap<FString, FString> Inputs;

	/** Additional node properties (pass-through to FOliveNodeFactory) */
	UPROPERTY()
	TMap<FString, FString> Properties;

	/** StepId whose exec output connects to this step's exec input */
	UPROPERTY()
	FString ExecAfter;

	/** Exec pin name to stepId mapping for multi-output nodes (e.g., Branch True/False) */
	UPROPERTY()
	TMap<FString, FString> ExecOutputs;

	/**
	 * Serialize this step to a JSON object.
	 * @return JSON representation of this plan step
	 */
	TSharedPtr<FJsonObject> ToJson() const;

	/**
	 * Deserialize a plan step from a JSON object.
	 * @param Json The JSON object to parse
	 * @return Parsed plan step (fields that are missing in JSON will have default values)
	 */
	static FOliveIRBlueprintPlanStep FromJson(const TSharedPtr<FJsonObject>& Json);
};

/**
 * A complete Blueprint plan containing an ordered list of steps.
 * This is the top-level structure that the LLM produces and the
 * plan resolver/lowerer consumes.
 *
 * Note: AssetPath, GraphTarget, and Mode come from the MCP tool params,
 * not from the plan JSON itself.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBlueprintPlan
{
	GENERATED_BODY()

	/** Schema version for forward compatibility (currently "1.0") */
	UPROPERTY()
	FString SchemaVersion = TEXT("1.0");

	/** Ordered list of plan steps */
	UPROPERTY()
	TArray<FOliveIRBlueprintPlanStep> Steps;

	/**
	 * Serialize this plan to a JSON object.
	 * @return JSON representation of the full plan
	 */
	TSharedPtr<FJsonObject> ToJson() const;

	/**
	 * Deserialize a plan from a JSON object.
	 * @param Json The JSON object to parse (expects "schema_version" and "steps" fields)
	 * @return Parsed plan
	 */
	static FOliveIRBlueprintPlan FromJson(const TSharedPtr<FJsonObject>& Json);
};

/**
 * Structured error from plan validation, resolution, or execution.
 * Contains enough context for the LLM to understand what went wrong
 * and how to fix it, including a JSON pointer to the exact location
 * in the plan and suggested alternatives.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBlueprintPlanError
{
	GENERATED_BODY()

	/** Machine-readable error code (e.g., "INVALID_OP", "AMBIGUOUS_TARGET", "GRAPH_DRIFT") */
	UPROPERTY()
	FString ErrorCode;

	/** StepId of the step that caused the error (empty if plan-level error) */
	UPROPERTY()
	FString StepId;

	/** JSON pointer to the error location (e.g., "/steps/3/target") */
	UPROPERTY()
	FString LocationPointer;

	/** Human-readable error description */
	UPROPERTY()
	FString Message;

	/** Suggested fix for the LLM to apply */
	UPROPERTY()
	FString Suggestion;

	/** Alternative values when error is due to ambiguity (e.g., multiple matching functions) */
	UPROPERTY()
	TArray<FString> Alternatives;

	/**
	 * Serialize this error to a JSON object.
	 * @return JSON representation of the error
	 */
	TSharedPtr<FJsonObject> ToJson() const;

	/**
	 * Deserialize an error from a JSON object.
	 * @param Json The JSON object to parse
	 * @return Parsed plan error
	 */
	static FOliveIRBlueprintPlanError FromJson(const TSharedPtr<FJsonObject>& Json);

	/**
	 * Factory: create a step-level error.
	 * @param Code Machine-readable error code
	 * @param InStepId The step that caused the error
	 * @param InLocation JSON pointer to error location
	 * @param InMessage Human-readable message
	 * @param InSuggestion Optional repair hint
	 * @return Constructed error
	 */
	static FOliveIRBlueprintPlanError MakeStepError(
		const FString& Code,
		const FString& InStepId,
		const FString& InLocation,
		const FString& InMessage,
		const FString& InSuggestion = TEXT(""));

	/**
	 * Factory: create a plan-level error (not tied to a specific step).
	 * @param Code Machine-readable error code
	 * @param InMessage Human-readable message
	 * @param InSuggestion Optional repair hint
	 * @return Constructed error
	 */
	static FOliveIRBlueprintPlanError MakePlanError(
		const FString& Code,
		const FString& InMessage,
		const FString& InSuggestion = TEXT(""));
};

/**
 * Result of applying a Blueprint plan.
 * Contains the mapping from step IDs to created node IDs, compile results,
 * and any errors or warnings that occurred during execution.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBlueprintPlanResult
{
	GENERATED_BODY()

	/** Whether the plan was applied successfully */
	UPROPERTY()
	bool bSuccess = false;

	/** Maps each step ID to the node_id of the created node */
	UPROPERTY()
	TMap<FString, FString> StepToNodeMap;

	/** Number of write operations actually executed */
	UPROPERTY()
	int32 AppliedOpsCount = 0;

	/**
	 * Compilation result after plan execution.
	 * Not a UPROPERTY because TOptional is not supported by UHT.
	 * Only set when compilation was triggered (bAutoCompile or explicit).
	 */
	TOptional<FOliveIRCompileResult> CompileResult;

	/** Structured errors encountered during plan execution */
	UPROPERTY()
	TArray<FOliveIRBlueprintPlanError> Errors;

	/** Non-fatal warning messages */
	UPROPERTY()
	TArray<FString> Warnings;

	/**
	 * Serialize this result to a JSON object.
	 * @return JSON representation of the plan result
	 */
	TSharedPtr<FJsonObject> ToJson() const;

	/**
	 * Deserialize a plan result from a JSON object.
	 * @param Json The JSON object to parse
	 * @return Parsed plan result
	 */
	static FOliveIRBlueprintPlanResult FromJson(const TSharedPtr<FJsonObject>& Json);

	/**
	 * Factory: create a success result.
	 * @param InStepToNodeMap Mapping of step IDs to node IDs
	 * @param InAppliedOpsCount Number of ops executed
	 * @return Success result
	 */
	static FOliveIRBlueprintPlanResult Success(
		const TMap<FString, FString>& InStepToNodeMap,
		int32 InAppliedOpsCount);

	/**
	 * Factory: create a failure result.
	 * @param InErrors The errors that caused the failure
	 * @return Failure result
	 */
	static FOliveIRBlueprintPlanResult Failure(const TArray<FOliveIRBlueprintPlanError>& InErrors);
};
