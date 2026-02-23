// Copyright Bode Software. All Rights Reserved.

#include "Plan/OliveBlueprintPlanLowerer.h"
#include "Plan/OliveBlueprintPlanResolver.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOlivePlanLowerer);

namespace
{
	/**
	 * Build a properties sub-object from a resolved step's Properties map.
	 * Matches the format expected by OliveToolParamHelpers::ParseNodeProperties().
	 *
	 * @param Properties Key-value property pairs
	 * @return JSON object with string fields for each property
	 */
	TSharedPtr<FJsonObject> BuildPropertiesObject(const TMap<FString, FString>& Properties)
	{
		TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
		// Sort keys for deterministic output
		TArray<FString> SortedKeys;
		Properties.GetKeys(SortedKeys);
		SortedKeys.Sort();
		for (const FString& Key : SortedKeys)
		{
			PropsObj->SetStringField(Key, Properties[Key]);
		}
		return PropsObj;
	}

	/**
	 * Build a step-id-to-index map from the plan steps for quick lookup.
	 *
	 * @param Steps The plan steps
	 * @return Map of step_id -> index in the Steps array
	 */
	TMap<FString, int32> BuildStepIdToIndexMap(const TArray<FOliveIRBlueprintPlanStep>& Steps)
	{
		TMap<FString, int32> Map;
		for (int32 i = 0; i < Steps.Num(); ++i)
		{
			Map.Add(Steps[i].StepId, i);
		}
		return Map;
	}

	/**
	 * Generate a unique op ID for connection and default-value ops.
	 * Format: "__olive_{prefix}_{counter}" for deterministic naming.
	 * The "__olive_" prefix prevents collisions with user-defined step IDs
	 * which are stored in the same OpResultsById map.
	 *
	 * @param Prefix  Op type prefix (e.g., "exec", "data", "default")
	 * @param Counter Running counter, incremented after use
	 * @return Unique op ID string
	 */
	FString MakeConnectionOpId(const FString& Prefix, int32& Counter)
	{
		return FString::Printf(TEXT("__olive_%s_%d"), *Prefix, Counter++);
	}

	/**
	 * Parse a "@stepId.pinName" reference into its components.
	 *
	 * @param RefValue  The reference string (must start with "@")
	 * @param OutStepId Populated with the step ID portion
	 * @param OutPinName Populated with the pin name portion
	 * @return True if parsing succeeded
	 */
	bool ParseDataReference(const FString& RefValue, FString& OutStepId, FString& OutPinName)
	{
		// Strip the leading "@"
		FString Content = RefValue.Mid(1);
		return Content.Split(TEXT("."), &OutStepId, &OutPinName);
	}
}

FOlivePlanLowerResult FOliveBlueprintPlanLowerer::Lower(
	const TArray<FOliveResolvedStep>& ResolvedSteps,
	const FOliveIRBlueprintPlan& Plan,
	const FString& GraphName,
	const FString& AssetPath)
{
	FOlivePlanLowerResult Result;

	// Validate inputs
	if (ResolvedSteps.Num() != Plan.Steps.Num())
	{
		Result.Errors.Add(FOliveIRBlueprintPlanError::MakePlanError(
			TEXT("LOWERER_MISMATCH"),
			FString::Printf(TEXT("ResolvedSteps count (%d) does not match Plan.Steps count (%d)"),
				ResolvedSteps.Num(), Plan.Steps.Num()),
			TEXT("Ensure the resolver output matches the plan input")));
		return Result;
	}

	if (ResolvedSteps.Num() == 0)
	{
		Result.Errors.Add(FOliveIRBlueprintPlanError::MakePlanError(
			TEXT("EMPTY_PLAN"),
			TEXT("Plan contains no steps to lower"),
			TEXT("Add at least one step to the plan")));
		return Result;
	}

	// Build step_id -> plan index map for lookup
	const TMap<FString, int32> StepIdToIndex = BuildStepIdToIndexMap(Plan.Steps);

	// Build step_id -> resolved step index map (same ordering, but for validation)
	TMap<FString, int32> StepIdToResolvedIndex;
	for (int32 i = 0; i < ResolvedSteps.Num(); ++i)
	{
		StepIdToResolvedIndex.Add(ResolvedSteps[i].StepId, i);
	}

	int32 ExecConnectionCounter = 0;
	int32 DataConnectionCounter = 0;
	int32 DefaultValueCounter = 0;

	// ============================================================================
	// Phase 1: Emit blueprint.add_node ops
	// ============================================================================
	for (int32 StepIndex = 0; StepIndex < ResolvedSteps.Num(); ++StepIndex)
	{
		const FOliveResolvedStep& Resolved = ResolvedSteps[StepIndex];

		// Record the index of this step's first (add_node) op
		Result.StepToFirstOpIndex.Add(Resolved.StepId, Result.Ops.Num());

		FOliveLoweredOp AddNodeOp;
		AddNodeOp.Id = Resolved.StepId;
		AddNodeOp.ToolName = TEXT("blueprint.add_node");

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), AssetPath);
		Params->SetStringField(TEXT("graph"), GraphName);
		Params->SetStringField(TEXT("type"), Resolved.NodeType);

		// Auto-layout: simple horizontal grid
		Params->SetNumberField(TEXT("pos_x"), StepIndex * AutoLayoutHorizontalSpacing);
		Params->SetNumberField(TEXT("pos_y"), 0);

		// Properties sub-object (matches ParseNodeProperties format)
		if (Resolved.Properties.Num() > 0)
		{
			Params->SetObjectField(TEXT("properties"), BuildPropertiesObject(Resolved.Properties));
		}

		AddNodeOp.Params = Params;
		Result.Ops.Add(MoveTemp(AddNodeOp));
	}

	// ============================================================================
	// Phase 2: Emit exec connection ops (ExecAfter + ExecOutputs)
	// ============================================================================
	for (int32 StepIndex = 0; StepIndex < Plan.Steps.Num(); ++StepIndex)
	{
		const FOliveIRBlueprintPlanStep& PlanStep = Plan.Steps[StepIndex];

		// ExecAfter: connect the source step's "then" pin to this step's "execute" pin
		if (!PlanStep.ExecAfter.IsEmpty())
		{
			if (!StepIdToIndex.Contains(PlanStep.ExecAfter))
			{
				Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
					TEXT("INVALID_EXEC_REF"),
					PlanStep.StepId,
					FString::Printf(TEXT("/steps/%d/exec_after"), StepIndex),
					FString::Printf(TEXT("exec_after references unknown step '%s'"), *PlanStep.ExecAfter),
					TEXT("Ensure exec_after refers to a valid step_id defined earlier in the plan")));
				continue;
			}

			FOliveLoweredOp ConnectOp;
			ConnectOp.Id = MakeConnectionOpId(TEXT("exec"), ExecConnectionCounter);
			ConnectOp.ToolName = TEXT("blueprint.connect_pins");

			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("path"), AssetPath);
			Params->SetStringField(TEXT("graph"), GraphName);
			// source: the ExecAfter step's "then" output pin
			Params->SetStringField(TEXT("source"),
				FString::Printf(TEXT("${%s.node_id}.then"), *PlanStep.ExecAfter));
			// target: this step's "execute" input pin
			Params->SetStringField(TEXT("target"),
				FString::Printf(TEXT("${%s.node_id}.execute"), *PlanStep.StepId));

			ConnectOp.Params = Params;
			Result.Ops.Add(MoveTemp(ConnectOp));
		}

		// ExecOutputs: connect specific exec output pins to target steps
		if (PlanStep.ExecOutputs.Num() > 0)
		{
			// Sort keys for deterministic iteration
			TArray<FString> SortedExecPins;
			PlanStep.ExecOutputs.GetKeys(SortedExecPins);
			SortedExecPins.Sort();

			for (const FString& ExecPinName : SortedExecPins)
			{
				const FString& TargetStepId = PlanStep.ExecOutputs[ExecPinName];

				if (!StepIdToIndex.Contains(TargetStepId))
				{
					Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
						TEXT("INVALID_EXEC_OUTPUT_REF"),
						PlanStep.StepId,
						FString::Printf(TEXT("/steps/%d/exec_outputs/%s"), StepIndex, *ExecPinName),
						FString::Printf(TEXT("exec_outputs '%s' references unknown step '%s'"), *ExecPinName, *TargetStepId),
						TEXT("Ensure exec_outputs target refers to a valid step_id")));
					continue;
				}

				FOliveLoweredOp ConnectOp;
				ConnectOp.Id = MakeConnectionOpId(TEXT("exec"), ExecConnectionCounter);
				ConnectOp.ToolName = TEXT("blueprint.connect_pins");

				TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("path"), AssetPath);
				Params->SetStringField(TEXT("graph"), GraphName);
				// source: this step's specific exec output pin (e.g., "True", "False")
				Params->SetStringField(TEXT("source"),
					FString::Printf(TEXT("${%s.node_id}.%s"), *PlanStep.StepId, *ExecPinName));
				// target: the target step's exec input pin
				Params->SetStringField(TEXT("target"),
					FString::Printf(TEXT("${%s.node_id}.execute"), *TargetStepId));

				ConnectOp.Params = Params;
				Result.Ops.Add(MoveTemp(ConnectOp));
			}
		}
	}

	// ============================================================================
	// Phase 3: Emit data connection ops (@ref inputs)
	// ============================================================================
	for (int32 StepIndex = 0; StepIndex < Plan.Steps.Num(); ++StepIndex)
	{
		const FOliveIRBlueprintPlanStep& PlanStep = Plan.Steps[StepIndex];

		if (PlanStep.Inputs.Num() == 0)
		{
			continue;
		}

		// Sort input keys for deterministic iteration
		TArray<FString> SortedInputKeys;
		PlanStep.Inputs.GetKeys(SortedInputKeys);
		SortedInputKeys.Sort();

		for (const FString& InputPinName : SortedInputKeys)
		{
			const FString& InputValue = PlanStep.Inputs[InputPinName];

			// Only handle @references in this phase
			if (!InputValue.StartsWith(TEXT("@")))
			{
				continue;
			}

			FString SourceStepId;
			FString SourcePinName;
			if (!ParseDataReference(InputValue, SourceStepId, SourcePinName))
			{
				Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
					TEXT("INVALID_DATA_REF"),
					PlanStep.StepId,
					FString::Printf(TEXT("/steps/%d/inputs/%s"), StepIndex, *InputPinName),
					FString::Printf(TEXT("Cannot parse data reference '%s' — expected format '@stepId.pinName'"), *InputValue),
					TEXT("Use the format @stepId.pinName to reference another step's output pin")));
				continue;
			}

			if (!StepIdToIndex.Contains(SourceStepId))
			{
				Result.Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
					TEXT("INVALID_DATA_REF"),
					PlanStep.StepId,
					FString::Printf(TEXT("/steps/%d/inputs/%s"), StepIndex, *InputPinName),
					FString::Printf(TEXT("Data reference '@%s.%s' references unknown step '%s'"), *SourceStepId, *SourcePinName, *SourceStepId),
					TEXT("Ensure the referenced step_id exists in the plan")));
				continue;
			}

			FOliveLoweredOp ConnectOp;
			ConnectOp.Id = MakeConnectionOpId(TEXT("data"), DataConnectionCounter);
			ConnectOp.ToolName = TEXT("blueprint.connect_pins");

			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("path"), AssetPath);
			Params->SetStringField(TEXT("graph"), GraphName);
			// source: the referenced step's output pin
			Params->SetStringField(TEXT("source"),
				FString::Printf(TEXT("${%s.node_id}.%s"), *SourceStepId, *SourcePinName));
			// target: this step's input pin
			Params->SetStringField(TEXT("target"),
				FString::Printf(TEXT("${%s.node_id}.%s"), *PlanStep.StepId, *InputPinName));

			ConnectOp.Params = Params;
			Result.Ops.Add(MoveTemp(ConnectOp));
		}
	}

	// ============================================================================
	// Phase 4: Emit set_pin_default ops for literal input values
	// ============================================================================
	for (int32 StepIndex = 0; StepIndex < Plan.Steps.Num(); ++StepIndex)
	{
		const FOliveIRBlueprintPlanStep& PlanStep = Plan.Steps[StepIndex];

		if (PlanStep.Inputs.Num() == 0)
		{
			continue;
		}

		// Sort input keys for deterministic iteration
		TArray<FString> SortedInputKeys;
		PlanStep.Inputs.GetKeys(SortedInputKeys);
		SortedInputKeys.Sort();

		for (const FString& InputPinName : SortedInputKeys)
		{
			const FString& InputValue = PlanStep.Inputs[InputPinName];

			// Skip @references — those were handled in Phase 3
			if (InputValue.StartsWith(TEXT("@")))
			{
				continue;
			}

			FOliveLoweredOp DefaultOp;
			DefaultOp.Id = MakeConnectionOpId(TEXT("default"), DefaultValueCounter);
			DefaultOp.ToolName = TEXT("blueprint.set_pin_default");

			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("path"), AssetPath);
			Params->SetStringField(TEXT("graph"), GraphName);
			// pin: the step's node_id + pin name (resolved via template ref)
			Params->SetStringField(TEXT("pin"),
				FString::Printf(TEXT("${%s.node_id}.%s"), *PlanStep.StepId, *InputPinName));
			Params->SetStringField(TEXT("value"), InputValue);

			DefaultOp.Params = Params;
			Result.Ops.Add(MoveTemp(DefaultOp));
		}
	}

	// If any errors accumulated, lowering failed
	if (Result.Errors.Num() > 0)
	{
		Result.bSuccess = false;
		UE_LOG(LogOlivePlanLowerer, Warning, TEXT("Plan lowering completed with %d error(s)"), Result.Errors.Num());
	}
	else
	{
		Result.bSuccess = true;
		UE_LOG(LogOlivePlanLowerer, Log, TEXT("Plan lowered successfully: %d ops (%d add_node, %d connections, %d defaults)"),
			Result.Ops.Num(),
			ResolvedSteps.Num(),
			ExecConnectionCounter + DataConnectionCounter,
			DefaultValueCounter);
	}

	return Result;
}
