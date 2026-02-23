// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintPlanIR.h"

struct FOliveResolvedStep;

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePlanLowerer, Log, All);

/**
 * A single lowered operation ready for dispatch via FOliveGraphBatchExecutor::DispatchWriterOp().
 * Uses the same param format as project.batch_write ops, including ${stepId.node_id} template
 * references for pin connections that depend on node IDs created by earlier ops.
 */
struct OLIVEAIEDITOR_API FOliveLoweredOp
{
	/** Op identifier -- matches step_id for add_node ops, generated for connection/default ops */
	FString Id;

	/** Tool name: blueprint.add_node, blueprint.connect_pins, blueprint.set_pin_default */
	FString ToolName;

	/** Fully formed params ready for DispatchWriterOp (includes ${stepId.node_id} template refs) */
	TSharedPtr<FJsonObject> Params;
};

/**
 * Result of lowering a resolved plan into concrete batch operations.
 * On success, Ops can be iterated and dispatched via FOliveGraphBatchExecutor.
 */
struct OLIVEAIEDITOR_API FOlivePlanLowerResult
{
	/** Whether lowering succeeded without errors */
	bool bSuccess = false;

	/** Ordered list of lowered operations ready for sequential dispatch */
	TArray<FOliveLoweredOp> Ops;

	/** Maps step_id to the index of its first op (the add_node op) in the Ops array */
	TMap<FString, int32> StepToFirstOpIndex;

	/** Structured errors encountered during lowering */
	TArray<FOliveIRBlueprintPlanError> Errors;
};

/**
 * FOliveBlueprintPlanLowerer
 *
 * Converts resolved plan steps into concrete batch operations in the same format
 * as project.batch_write. Output ops use FOliveGraphBatchExecutor::DispatchWriterOp()
 * param conventions and ${stepId.node_id} template references for cross-op dependencies.
 *
 * Lowering phases (deterministic, same inputs always produce identical output):
 *   Phase 1: Emit blueprint.add_node ops for each resolved step
 *   Phase 2: Emit blueprint.connect_pins ops for exec flow (ExecAfter + ExecOutputs)
 *   Phase 3: Emit blueprint.connect_pins ops for data wires (@ref inputs)
 *   Phase 4: Emit blueprint.set_pin_default ops for literal input values
 *
 * Thread Safety: All methods are static and operate on immutable inputs.
 */
class OLIVEAIEDITOR_API FOliveBlueprintPlanLowerer
{
public:
	/**
	 * Lower resolved steps into batch ops ready for dispatch.
	 *
	 * @param ResolvedSteps  Output from FOliveBlueprintPlanResolver::Resolve()
	 * @param Plan           The original plan (for exec_after, inputs, exec_outputs from each step)
	 * @param GraphName      Target graph name (injected into each op's params)
	 * @param AssetPath      Target asset path (injected into each op's params)
	 * @return Lowered ops ready for sequential dispatch, or errors if lowering failed
	 */
	static FOlivePlanLowerResult Lower(
		const TArray<FOliveResolvedStep>& ResolvedSteps,
		const FOliveIRBlueprintPlan& Plan,
		const FString& GraphName,
		const FString& AssetPath);

private:
	/** Horizontal spacing in pixels between auto-laid-out nodes */
	static constexpr int32 AutoLayoutHorizontalSpacing = 300;

	/** Vertical offset for branch targets */
	static constexpr int32 AutoLayoutVerticalSpacing = 200;
};
