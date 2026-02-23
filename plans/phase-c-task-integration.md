# Phase C: Integration -- Coder Task

## Overview

Phase C wires the Phase A components (FOlivePinManifest, FOliveFunctionResolver) and Phase B component (FOlivePlanExecutor) into the existing codebase so the v2.0 plan execution path is operational end-to-end.

**Prerequisite**: Phase A and Phase B files must compile successfully before starting Phase C.

## Task Execution Order (Dependencies)

```
C1 (FunctionResolver into PlanResolver) -- no dependencies on C2-C5
C3 (BatchScope into PlanExecutor)       -- no dependencies on C1,C2,C4,C5
C2 (PlanExecutor into ApplyPlanJson)    -- depends on C1 and C3 being done first
C5 (PreviewPlanJson for v2.0)           -- depends on C1 being done first
C4 (Worker prompt update)               -- no code dependencies, do last
```

Recommended order: C1 -> C3 -> C2 -> C5 -> C4. Build after C3, build again after C2+C5.

---

## Task C1: Wire FOliveFunctionResolver into OliveBlueprintPlanResolver::ResolveCallOp

### File to modify

`B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Blueprint\Private\Plan\OliveBlueprintPlanResolver.cpp`

### New include needed

Add this include after the existing includes (after line 6, before line 7):

```cpp
// EXISTING (line 6):
#include "Catalog/OliveNodeCatalog.h"
// ADD THIS:
#include "Plan/OliveFunctionResolver.h"
// EXISTING (line 7):
#include "Engine/Blueprint.h"
```

### What to change

Replace the entire `ResolveCallOp` method body (lines 283-409). The current code does a direct catalog search. The new code uses `FOliveFunctionResolver::Resolve()` as primary, falling back to catalog search only when the resolver returns nothing and no explicit TargetClass was given.

### Current code (lines 283-409)

```cpp
bool FOliveBlueprintPlanResolver::ResolveCallOp(
	const FOliveIRBlueprintPlanStep& Step,
	UBlueprint* BP,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors,
	TArray<FString>& Warnings)
{
	Out.NodeType = OliveNodeTypes::CallFunction;

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

	Out.Properties.Add(TEXT("function_name"), Step.Target);

	// If TargetClass is explicitly provided, use it directly for disambiguation
	if (!Step.TargetClass.IsEmpty())
	{
		Out.Properties.Add(TEXT("target_class"), Step.TargetClass);

		UE_LOG(LogOlivePlanResolver, Verbose,
			TEXT("Step '%s': Resolved call '%s' on class '%s'"),
			*Step.StepId, *Step.Target, *Step.TargetClass);
		return true;
	}

	// No TargetClass — search the catalog for disambiguation
	FOliveNodeCatalog& Catalog = FOliveNodeCatalog::Get();

	if (!Catalog.IsInitialized())
	{
		// Catalog not ready — accept the call as-is without disambiguation
		Warnings.Add(FString::Printf(
			TEXT("Step '%s': Node catalog not initialized, accepting function '%s' without disambiguation"),
			*Step.StepId, *Step.Target));
		return true;
	}

	TArray<FOliveNodeTypeInfo> SearchResults = Catalog.Search(Step.Target, CATALOG_SEARCH_LIMIT);

	// Filter to only function-call results that match the target name
	TArray<FOliveNodeTypeInfo> MatchingFunctions;
	for (const FOliveNodeTypeInfo& Info : SearchResults)
	{
		// Match if the function name matches (case-insensitive) or the display name matches
		if (Info.FunctionName.Equals(Step.Target, ESearchCase::IgnoreCase) ||
			Info.DisplayName.Equals(Step.Target, ESearchCase::IgnoreCase))
		{
			MatchingFunctions.Add(Info);
		}
	}

	if (MatchingFunctions.Num() == 1)
	{
		// Exact single match — use it
		const FOliveNodeTypeInfo& Match = MatchingFunctions[0];
		if (!Match.FunctionClass.IsEmpty())
		{
			Out.Properties.Add(TEXT("target_class"), Match.FunctionClass);
		}
		// Use the matched function name in case casing differs
		Out.Properties[TEXT("function_name")] = Match.FunctionName.IsEmpty() ? Step.Target : Match.FunctionName;

		UE_LOG(LogOlivePlanResolver, Verbose,
			TEXT("Step '%s': Resolved call '%s' via catalog (class: %s)"),
			*Step.StepId, *Step.Target, *Match.FunctionClass);
		return true;
	}

	if (MatchingFunctions.Num() > 1)
	{
		// Ambiguous — return error with alternatives
		TArray<FString> Alternatives;
		for (const FOliveNodeTypeInfo& Info : MatchingFunctions)
		{
			FString Alt = Info.FunctionName;
			if (!Info.FunctionClass.IsEmpty())
			{
				Alt = FString::Printf(TEXT("%s (class: %s)"), *Info.FunctionName, *Info.FunctionClass);
			}
			Alternatives.Add(MoveTemp(Alt));
		}

		FOliveIRBlueprintPlanError Error = FOliveIRBlueprintPlanError::MakeStepError(
			TEXT("AMBIGUOUS_TARGET"),
			Step.StepId,
			FString::Printf(TEXT("/steps/%d/target"), Idx),
			FString::Printf(TEXT("Function '%s' is ambiguous — %d candidates found"), *Step.Target, MatchingFunctions.Num()),
			TEXT("Specify 'target_class' to disambiguate"));
		Error.Alternatives = MoveTemp(Alternatives);
		Errors.Add(MoveTemp(Error));
		return false;
	}

	// No exact name match in catalog — try fuzzy match for suggestions, but still accept the call.
	// The function may exist but not be in the catalog (e.g., Blueprint-defined functions,
	// dynamically loaded plugins). The factory will validate at creation time.
	TArray<FOliveNodeSuggestion> Suggestions = Catalog.FuzzyMatch(Step.Target, CATALOG_SEARCH_LIMIT);

	if (Suggestions.Num() > 0 && Suggestions[0].Score >= MIN_AUTO_MATCH_SCORE)
	{
		// High-confidence fuzzy match — add a warning but accept
		Warnings.Add(FString::Printf(
			TEXT("Step '%s': Function '%s' not found in catalog. Did you mean '%s'?"),
			*Step.StepId, *Step.Target, *Suggestions[0].DisplayName));
	}
	else if (Suggestions.Num() > 0)
	{
		Warnings.Add(FString::Printf(
			TEXT("Step '%s': Function '%s' not found in catalog. Closest matches: %s"),
			*Step.StepId, *Step.Target, *Suggestions[0].DisplayName));
	}

	UE_LOG(LogOlivePlanResolver, Verbose,
		TEXT("Step '%s': Accepted call '%s' without catalog match (will validate at creation)"),
		*Step.StepId, *Step.Target);

	return true;
}
```

### Replacement code

```cpp
bool FOliveBlueprintPlanResolver::ResolveCallOp(
	const FOliveIRBlueprintPlanStep& Step,
	UBlueprint* BP,
	int32 Idx,
	FOliveResolvedStep& Out,
	TArray<FOliveIRBlueprintPlanError>& Errors,
	TArray<FString>& Warnings)
{
	Out.NodeType = OliveNodeTypes::CallFunction;

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

	return true;
}
```

### What NOT to change

- Do NOT modify the `Resolve()` method or any other per-op resolvers (ResolveGetVarOp, ResolveSetVarOp, etc.)
- Do NOT modify the `ComputePlanFingerprint` or `ComputePlanDiff` methods
- Do NOT remove the anonymous namespace helper `MIN_AUTO_MATCH_SCORE` or `CATALOG_SEARCH_LIMIT` -- they are still used by the fallback catalog search in the new code
- The `#include "Catalog/OliveNodeCatalog.h"` must remain (used in the fallback path)

---

## Task C3: Add FOliveBatchExecutionScope to OlivePlanExecutor

### File to modify

`B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Blueprint\Private\Plan\OlivePlanExecutor.cpp`

### New include needed

Add after the existing UE includes block (after line 31, before the JSON includes):

```cpp
// EXISTING (line 31):
#include "Kismet2/BlueprintEditorUtils.h"
// ADD THIS:
#include "Services/OliveBatchExecutionScope.h"
// EXISTING (line 33):
// JSON
```

### What to change

Wrap phases 3-5 in an `FOliveBatchExecutionScope` to suppress nested transactions from `FOlivePinConnector::Connect()`. The scope must start BEFORE Phase 3 and end AFTER Phase 5 (but before Phase 6, which is non-mutating).

In the `Execute()` method, find the current code at lines 104-133:

```cpp
	// Phase 3: Wire exec connections (CONTINUE-ON-FAILURE)
	UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 3: Wire Exec Connections"));
	PhaseWireExec(Plan, Context);

	UE_LOG(LogOlivePlanExecutor, Log,
		TEXT("Phase 3 complete: %d exec connections succeeded, %d failed"),
		Context.SuccessfulConnectionCount, Context.FailedConnectionCount);

	// Phase 4: Wire data connections (CONTINUE-ON-FAILURE)
	// Store the exec-only counts before Phase 4 adds to them
	const int32 ExecConnectionsSucceeded = Context.SuccessfulConnectionCount;
	const int32 ExecConnectionsFailed = Context.FailedConnectionCount;

	UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 4: Wire Data Connections"));
	PhaseWireData(Plan, Context);

	const int32 DataConnectionsSucceeded = Context.SuccessfulConnectionCount - ExecConnectionsSucceeded;
	const int32 DataConnectionsFailed = Context.FailedConnectionCount - ExecConnectionsFailed;

	UE_LOG(LogOlivePlanExecutor, Log,
		TEXT("Phase 4 complete: %d data connections succeeded, %d failed"),
		DataConnectionsSucceeded, DataConnectionsFailed);

	// Phase 5: Set pin defaults (CONTINUE-ON-FAILURE)
	UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 5: Set Pin Defaults"));
	PhaseSetDefaults(Plan, Context);

	UE_LOG(LogOlivePlanExecutor, Log,
		TEXT("Phase 5 complete: %d defaults set, %d failed"),
		Context.SuccessfulDefaultCount, Context.FailedDefaultCount);
```

Replace with:

```cpp
	// Phases 3-5: Wiring + defaults.
	// Wrap in FOliveBatchExecutionScope to suppress nested transactions from
	// FOlivePinConnector::Connect(). The caller's write pipeline owns the
	// outer transaction; inner transactions would create undo noise.
	{
		FOliveBatchExecutionScope BatchScope;

		// Phase 3: Wire exec connections (CONTINUE-ON-FAILURE)
		UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 3: Wire Exec Connections"));
		PhaseWireExec(Plan, Context);

		UE_LOG(LogOlivePlanExecutor, Log,
			TEXT("Phase 3 complete: %d exec connections succeeded, %d failed"),
			Context.SuccessfulConnectionCount, Context.FailedConnectionCount);

		// Phase 4: Wire data connections (CONTINUE-ON-FAILURE)
		// Store the exec-only counts before Phase 4 adds to them
		const int32 ExecConnectionsSucceeded = Context.SuccessfulConnectionCount;
		const int32 ExecConnectionsFailed = Context.FailedConnectionCount;

		UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 4: Wire Data Connections"));
		PhaseWireData(Plan, Context);

		const int32 DataConnectionsSucceeded = Context.SuccessfulConnectionCount - ExecConnectionsSucceeded;
		const int32 DataConnectionsFailed = Context.FailedConnectionCount - ExecConnectionsFailed;

		UE_LOG(LogOlivePlanExecutor, Log,
			TEXT("Phase 4 complete: %d data connections succeeded, %d failed"),
			DataConnectionsSucceeded, DataConnectionsFailed);

		// Phase 5: Set pin defaults (CONTINUE-ON-FAILURE)
		UE_LOG(LogOlivePlanExecutor, Log, TEXT("Phase 5: Set Pin Defaults"));
		PhaseSetDefaults(Plan, Context);

		UE_LOG(LogOlivePlanExecutor, Log,
			TEXT("Phase 5 complete: %d defaults set, %d failed"),
			Context.SuccessfulDefaultCount, Context.FailedDefaultCount);
	} // BatchScope destructor restores previous batch state

```

### Important notes

- The `FOliveBatchExecutionScope` supports nesting. If the caller (HandleBlueprintApplyPlanJson) already has a BatchScope active, this inner one is harmless -- the destructor restores the previous state. However, adding it here makes the executor self-contained: it works correctly regardless of whether the caller remembered to create a batch scope.
- Phase 1 (node creation) is intentionally NOT inside the batch scope. `FOliveGraphWriter::AddNode()` creates its own transaction via `OLIVE_SCOPED_TRANSACTION`, and those should be suppressed by the outer pipeline's batch scope (which IS created by the caller). This inner batch scope in phases 3-5 is specifically for the direct `FOlivePinConnector::Connect()` calls that bypass `FOliveGraphWriter`.
- Phase 6 (layout) does not create transactions so it does not need the scope.

---

## Task C2: Wire FOlivePlanExecutor into HandleBlueprintApplyPlanJson

### File to modify

`B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Blueprint\Private\MCP\OliveBlueprintToolHandlers.cpp`

### New includes needed

Add after the existing `#include "Plan/OliveBlueprintPlanLowerer.h"` (line 32):

```cpp
// EXISTING (line 32):
#include "Plan/OliveBlueprintPlanLowerer.h"
// ADD THESE:
#include "Plan/OlivePlanExecutor.h"
#include "Plan/OlivePinManifest.h"
#include "Plan/OliveFunctionResolver.h"
```

### What to change in HandleBlueprintApplyPlanJson

The function currently has this flow (starting at line 5919):
1. Validate params (lines 5919-5957) -- KEEP
2. Validate plan schema (lines 5960-5987) -- KEEP
3. Parse plan (lines 5989-5992) -- KEEP
4. Load Blueprint (lines 5994-6005) -- KEEP
5. Check preview requirement (lines 6007-6017) -- KEEP
6. Find target graph (lines 6019-6030) -- KEEP
7. Drift detection (lines 6032-6049) -- KEEP
8. **Resolve + Lower** (lines 6051-6081) -- CHANGE: version-gated
9. Build write request (lines 6083-6097) -- KEEP (with minor change)
10. Build executor delegate (lines 6099-6228) -- CHANGE: version-gated executor lambda
11. Execute through pipeline (lines 6230-6260) -- CHANGE: enhanced result handling

#### Change 1: Replace section 8 (Resolve + Lower) -- lines 6051-6081

Find this block:

```cpp
	// ------------------------------------------------------------------
	// 8. Resolve + Lower
	// ------------------------------------------------------------------
	FOlivePlanResolveResult ResolveResult = FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint);
	if (!ResolveResult.bSuccess)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("phase"), TEXT("resolve"));
		ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(ResolveResult.Errors));
		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("PLAN_RESOLVE_FAILED"),
			FString::Printf(TEXT("Plan resolution failed with %d error(s)"), ResolveResult.Errors.Num()),
			ResolveResult.Errors.Num() > 0 ? ResolveResult.Errors[0].Suggestion : TEXT(""));
		Result.Data = ErrorData;
		return Result;
	}

	FOlivePlanLowerResult LowerResult = FOliveBlueprintPlanLowerer::Lower(
		ResolveResult.ResolvedSteps, Plan, GraphTarget, AssetPath);
	if (!LowerResult.bSuccess)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("phase"), TEXT("lower"));
		ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(LowerResult.Errors));
		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("PLAN_LOWER_FAILED"),
			FString::Printf(TEXT("Plan lowering failed with %d error(s)"), LowerResult.Errors.Num()),
			LowerResult.Errors.Num() > 0 ? LowerResult.Errors[0].Suggestion : TEXT(""));
		Result.Data = ErrorData;
		return Result;
	}
```

Replace with:

```cpp
	// ------------------------------------------------------------------
	// 8. Resolve (always) + Lower (v1.0 only)
	// ------------------------------------------------------------------
	const bool bIsV2Plan = (Plan.SchemaVersion == TEXT("2.0"));

	FOlivePlanResolveResult ResolveResult = FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint);
	if (!ResolveResult.bSuccess)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("phase"), TEXT("resolve"));
		ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(ResolveResult.Errors));
		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("PLAN_RESOLVE_FAILED"),
			FString::Printf(TEXT("Plan resolution failed with %d error(s)"), ResolveResult.Errors.Num()),
			ResolveResult.Errors.Num() > 0 ? ResolveResult.Errors[0].Suggestion : TEXT(""));
		Result.Data = ErrorData;
		return Result;
	}

	// v1.0 path: lower to batch ops (v2.0 skips this entirely)
	FOlivePlanLowerResult LowerResult;
	if (!bIsV2Plan)
	{
		LowerResult = FOliveBlueprintPlanLowerer::Lower(
			ResolveResult.ResolvedSteps, Plan, GraphTarget, AssetPath);
		if (!LowerResult.bSuccess)
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("phase"), TEXT("lower"));
			ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(LowerResult.Errors));
			FOliveToolResult Result = FOliveToolResult::Error(
				TEXT("PLAN_LOWER_FAILED"),
				FString::Printf(TEXT("Plan lowering failed with %d error(s)"), LowerResult.Errors.Num()),
				LowerResult.Errors.Num() > 0 ? LowerResult.Errors[0].Suggestion : TEXT(""));
			Result.Data = ErrorData;
			return Result;
		}
	}
```

#### Change 2: Replace sections 9-10-11 (Write request + Executor lambda + Pipeline) -- lines 6083-6260

Find this entire block (from `// 9. Build write request` through the end of the function). Replace the full block from line 6083 to line 6260 with the version-gated code below:

```cpp
	// ------------------------------------------------------------------
	// 9. Build write request
	// ------------------------------------------------------------------
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.apply_plan_json");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::Format(
		NSLOCTEXT("OliveBPTools", "ApplyPlanJson", "AI Agent: Apply Plan JSON ({0} steps, v{1})"),
		FText::AsNumber(Plan.Steps.Num()),
		FText::FromString(Plan.SchemaVersion));
	Request.OperationCategory = TEXT("plan_apply");
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true;
	Request.bSkipVerification = false;

	// ------------------------------------------------------------------
	// 10. Build executor delegate (version-gated)
	// ------------------------------------------------------------------
	FOliveWriteExecutor Executor;

	if (bIsV2Plan)
	{
		// ============================================================
		// v2.0 PATH: FOlivePlanExecutor with pin introspection
		// ============================================================

		// Capture resolved steps and plan by value for the lambda.
		// ResolvedSteps is small (one struct per step). Plan is also small.
		TArray<FOliveResolvedStep> CapturedResolvedSteps = ResolveResult.ResolvedSteps;
		FOliveIRBlueprintPlan CapturedPlan = Plan;

		Executor.BindLambda(
			[CapturedResolvedSteps = MoveTemp(CapturedResolvedSteps),
			 CapturedPlan = MoveTemp(CapturedPlan),
			 AssetPath, GraphTarget]
			(const FOliveWriteRequest& InRequest, UObject* TargetAsset) -> FOliveWriteResult
			{
				UBlueprint* BP = Cast<UBlueprint>(TargetAsset);
				if (!BP)
				{
					return FOliveWriteResult::ExecutionError(
						TEXT("INVALID_TARGET"),
						TEXT("Target asset is not a valid Blueprint"),
						TEXT("Ensure the asset_path points to a Blueprint"));
				}

				// Suppress inner transactions -- the pipeline owns the outer transaction
				FOliveBatchExecutionScope BatchScope;

				BP->Modify();

				// Ensure target graph exists inside the pipeline transaction
				bool bGraphCreatedInTxn = false;
				UEdGraph* ExecutionGraph = FindOrCreateFunctionGraph(BP, GraphTarget, bGraphCreatedInTxn);
				if (!ExecutionGraph)
				{
					return FOliveWriteResult::ExecutionError(
						TEXT("GRAPH_NOT_FOUND"),
						FString::Printf(TEXT("Graph '%s' not found and could not be created"), *GraphTarget),
						TEXT("EventGraph must already exist; other names are created as function graphs."));
				}

				// Execute the multi-phase plan
				FOlivePlanExecutor PlanExecutor;
				FOliveIRBlueprintPlanResult PlanResult = PlanExecutor.Execute(
					CapturedPlan, CapturedResolvedSteps,
					BP, ExecutionGraph, AssetPath, GraphTarget);

				// Build the result data JSON
				TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();

				// step_to_node_map
				TSharedPtr<FJsonObject> MapObj = MakeShared<FJsonObject>();
				for (const auto& Pair : PlanResult.StepToNodeMap)
				{
					MapObj->SetStringField(Pair.Key, Pair.Value);
				}
				ResultData->SetObjectField(TEXT("step_to_node_map"), MapObj);
				ResultData->SetNumberField(TEXT("applied_ops_count"), PlanResult.AppliedOpsCount);
				ResultData->SetStringField(TEXT("schema_version"), TEXT("2.0"));

				// Serialize wiring errors (for AI self-correction)
				if (PlanResult.Errors.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> ErrorsArr;
					ErrorsArr.Reserve(PlanResult.Errors.Num());
					for (const FOliveIRBlueprintPlanError& Err : PlanResult.Errors)
					{
						ErrorsArr.Add(MakeShared<FJsonValueObject>(Err.ToJson()));
					}
					ResultData->SetArrayField(TEXT("wiring_errors"), ErrorsArr);
				}

				// Serialize warnings
				if (PlanResult.Warnings.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> WarningsArr;
					WarningsArr.Reserve(PlanResult.Warnings.Num());
					for (const FString& Warn : PlanResult.Warnings)
					{
						WarningsArr.Add(MakeShared<FJsonValueString>(Warn));
					}
					ResultData->SetArrayField(TEXT("warnings"), WarningsArr);
				}

				// Pin manifests for failed wiring steps (AI self-correction data).
				// Include manifests only for steps that had wiring errors, plus
				// all steps referenced by those errors, to keep payload small.
				// Actually, for simplicity and maximum AI utility, include ALL
				// manifests when there are any wiring failures. The payload is
				// small (one manifest per step, each is a list of pin names).
				const bool bHasWiringErrors =
					(PlanResult.Errors.Num() > 0) &&
					PlanResult.bSuccess; // Only include manifests on partial success, not total failure
				if (bHasWiringErrors)
				{
					// We need the execution context to get manifests, but the executor
					// consumed it internally. Instead, re-serialize from the PlanResult.
					// However, PlanResult does not currently carry manifests.
					// The executor logged everything. For now, we note that the AI
					// should use blueprint.read to get pin data for self-correction.
					ResultData->SetStringField(TEXT("self_correction_hint"),
						TEXT("Some wiring failed. Use blueprint.read on the target graph to see "
							 "actual pin names on created nodes, then use granular connect_pins/set_pin_default "
							 "to fix failed connections. See wiring_errors for details."));
				}

				if (!PlanResult.bSuccess)
				{
					// Node creation failed entirely
					return FOliveWriteResult::ExecutionError(
						TEXT("PLAN_EXECUTION_FAILED"),
						FString::Printf(TEXT("Plan execution failed: %d of %d nodes created"),
							static_cast<int32>(PlanResult.StepToNodeMap.Num()),
							CapturedPlan.Steps.Num()),
						PlanResult.Errors.Num() > 0 ? PlanResult.Errors[0].Suggestion : TEXT(""),
						ResultData);
				}

				FOliveWriteResult SuccessResult = FOliveWriteResult::Success(ResultData);

				// Collect created node IDs for the pipeline's verification stage
				TArray<FString> CreatedNodeIds;
				CreatedNodeIds.Reserve(PlanResult.StepToNodeMap.Num());
				for (const auto& Pair : PlanResult.StepToNodeMap)
				{
					CreatedNodeIds.Add(Pair.Value);
				}
				SuccessResult.CreatedNodeIds = MoveTemp(CreatedNodeIds);

				return SuccessResult;
			});
	}
	else
	{
		// ============================================================
		// v1.0 PATH: Existing lowerer + batch dispatch (unchanged)
		// ============================================================
		TArray<FOliveLoweredOp> CapturedOps = LowerResult.Ops;
		TMap<FString, int32> CapturedStepMap = LowerResult.StepToFirstOpIndex;

		Executor.BindLambda(
			[CapturedOps = MoveTemp(CapturedOps), CapturedStepMap = MoveTemp(CapturedStepMap), AssetPath, GraphTarget]
			(const FOliveWriteRequest& InRequest, UObject* TargetAsset) -> FOliveWriteResult
			{
				UBlueprint* BP = Cast<UBlueprint>(TargetAsset);
				if (!BP)
				{
					return FOliveWriteResult::ExecutionError(
						TEXT("INVALID_TARGET"),
						TEXT("Target asset is not a valid Blueprint"),
						TEXT("Ensure the asset_path points to a Blueprint"));
				}

				// Suppress inner transactions -- the pipeline owns the outer transaction
				FOliveBatchExecutionScope BatchScope;

				BP->Modify();

				// Ensure target graph exists inside the pipeline transaction so creation
				// participates in rollback if later ops fail.
				bool bGraphCreatedInTxn = false;
				UEdGraph* ExecutionGraph = FindOrCreateFunctionGraph(BP, GraphTarget, bGraphCreatedInTxn);
				if (!ExecutionGraph)
				{
					return FOliveWriteResult::ExecutionError(
						TEXT("GRAPH_NOT_FOUND"),
						FString::Printf(TEXT("Graph '%s' not found and could not be created"), *GraphTarget),
						TEXT("EventGraph must already exist; other names are created as function graphs."));
				}

				TMap<FString, TSharedPtr<FJsonObject>> OpResultsById;
				TMap<FString, FString> StepToNodeMap;
				TArray<FString> CreatedNodeIds;
				int32 AppliedCount = 0;

				for (int32 i = 0; i < CapturedOps.Num(); ++i)
				{
					if (!CapturedOps[i].Params.IsValid())
					{
						return FOliveWriteResult::ExecutionError(
							TEXT("INVALID_OP_PARAMS"),
							FString::Printf(TEXT("Op %d has invalid null params (id='%s', tool='%s')"),
								i, *CapturedOps[i].Id, *CapturedOps[i].ToolName),
							TEXT("Regenerate the plan and retry."));
					}

					// Copy params so template resolution can mutate them
					TSharedPtr<FJsonObject> OpParams = MakeShared<FJsonObject>();
					for (const auto& Field : CapturedOps[i].Params->Values)
					{
						OpParams->Values.Add(Field.Key, Field.Value);
					}

					// Resolve ${opId.field} template references
					FString TemplateError;
					if (!FOliveGraphBatchExecutor::ResolveTemplateReferences(OpParams, OpResultsById, TemplateError))
					{
						return FOliveWriteResult::ExecutionError(
							TEXT("TEMPLATE_RESOLVE_FAILED"),
							FString::Printf(TEXT("Template resolution failed at op %d (id='%s'): %s"),
								i, *CapturedOps[i].Id, *TemplateError),
							TEXT("Check that referenced step IDs exist and produced node_id results"));
					}

					// Dispatch to writer
					FOliveBlueprintWriteResult WriteResult = FOliveGraphBatchExecutor::DispatchWriterOp(
						CapturedOps[i].ToolName, AssetPath, OpParams);

					if (!WriteResult.bSuccess)
					{
						FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
						return FOliveWriteResult::ExecutionError(
							TEXT("OP_FAILED"),
							FString::Printf(TEXT("Op %d failed (id='%s', tool='%s'): %s"),
								i, *CapturedOps[i].Id, *CapturedOps[i].ToolName, *ErrorMsg),
							TEXT("Check the plan step definition for this operation"));
					}

					AppliedCount++;

					// Build result data for template resolution by later ops
					TSharedPtr<FJsonObject> OpData = MakeShared<FJsonObject>();
					if (!WriteResult.CreatedNodeId.IsEmpty())
					{
						OpData->SetStringField(TEXT("node_id"), WriteResult.CreatedNodeId);
						CreatedNodeIds.Add(WriteResult.CreatedNodeId);
					}
					if (!WriteResult.CreatedItemName.IsEmpty())
					{
						OpData->SetStringField(TEXT("item_name"), WriteResult.CreatedItemName);
					}

					// Store result for template resolution
					if (!CapturedOps[i].Id.IsEmpty())
					{
						OpResultsById.Add(CapturedOps[i].Id, OpData);

						// If this is an add_node op (has a step mapping), record in StepToNodeMap
						if (CapturedStepMap.Contains(CapturedOps[i].Id) && !WriteResult.CreatedNodeId.IsEmpty())
						{
							StepToNodeMap.Add(CapturedOps[i].Id, WriteResult.CreatedNodeId);
						}
					}
				}

				// Build success result
				TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();

				// Serialize step_to_node_map
				TSharedPtr<FJsonObject> MapObj = MakeShared<FJsonObject>();
				for (const auto& Pair : StepToNodeMap)
				{
					MapObj->SetStringField(Pair.Key, Pair.Value);
				}
				ResultData->SetObjectField(TEXT("step_to_node_map"), MapObj);
				ResultData->SetNumberField(TEXT("applied_ops_count"), AppliedCount);

				FOliveWriteResult SuccessResult = FOliveWriteResult::Success(ResultData);
				SuccessResult.CreatedNodeIds = MoveTemp(CreatedNodeIds);
				return SuccessResult;
			});
	}

	// ------------------------------------------------------------------
	// 11. Execute through write pipeline
	// ------------------------------------------------------------------
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult PipelineResult = Pipeline.Execute(Request, Executor);

	// Convert to tool result and inject data from executor result
	FOliveToolResult ToolResult = PipelineResult.ToToolResult();

	if (PipelineResult.bSuccess && PipelineResult.ResultData.IsValid() && ToolResult.Data.IsValid())
	{
		// Forward all fields from the executor's ResultData into the tool result
		const TSharedPtr<FJsonObject>* StepMapObj = nullptr;
		if (PipelineResult.ResultData->TryGetObjectField(TEXT("step_to_node_map"), StepMapObj))
		{
			ToolResult.Data->SetObjectField(TEXT("step_to_node_map"), *StepMapObj);
		}

		double AppliedOps = 0;
		if (PipelineResult.ResultData->TryGetNumberField(TEXT("applied_ops_count"), AppliedOps))
		{
			ToolResult.Data->SetNumberField(TEXT("applied_ops_count"), AppliedOps);
		}

		// v2.0-specific result fields
		if (bIsV2Plan)
		{
			FString SchemaVersion;
			if (PipelineResult.ResultData->TryGetStringField(TEXT("schema_version"), SchemaVersion))
			{
				ToolResult.Data->SetStringField(TEXT("schema_version"), SchemaVersion);
			}

			const TArray<TSharedPtr<FJsonValue>>* WiringErrors = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("wiring_errors"), WiringErrors))
			{
				ToolResult.Data->SetArrayField(TEXT("wiring_errors"), *WiringErrors);
			}

			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("warnings"), Warnings))
			{
				ToolResult.Data->SetArrayField(TEXT("warnings"), *Warnings);
			}

			FString SelfCorrectionHint;
			if (PipelineResult.ResultData->TryGetStringField(TEXT("self_correction_hint"), SelfCorrectionHint))
			{
				ToolResult.Data->SetStringField(TEXT("self_correction_hint"), SelfCorrectionHint);
			}
		}
	}
	else if (!PipelineResult.bSuccess && PipelineResult.ResultData.IsValid())
	{
		// On failure, still forward wiring_errors and warnings if present (v2.0)
		if (bIsV2Plan && ToolResult.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* WiringErrors = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("wiring_errors"), WiringErrors))
			{
				ToolResult.Data->SetArrayField(TEXT("wiring_errors"), *WiringErrors);
			}

			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("warnings"), Warnings))
			{
				ToolResult.Data->SetArrayField(TEXT("warnings"), *Warnings);
			}
		}
	}

	UE_LOG(LogOliveBPTools, Log,
		TEXT("Plan apply for '%s' graph '%s': success=%s, schema_version=%s"),
		*AssetPath, *GraphTarget, PipelineResult.bSuccess ? TEXT("true") : TEXT("false"),
		*Plan.SchemaVersion);

	return ToolResult;
}
```

### Key design decisions in this code

1. **FOliveWriteResult::ExecutionError with ResultData**: The v2.0 executor lambda passes `ResultData` as the 4th argument to `ExecutionError()`. This ensures wiring errors and partial results are available even on failure. **Verify that `FOliveWriteResult::ExecutionError()` has a 4th parameter overload accepting `TSharedPtr<FJsonObject> ResultData`**. If it does not, the coder must add one, or set `Result.ResultData = ResultData` after construction. Check the current signature in `OliveWritePipeline.h`.

2. **Pin manifests in result**: The current design passes manifests as a `self_correction_hint` string rather than serialized JSON manifests. This is because `FOliveIRBlueprintPlanResult` does not carry manifests (it would require a new field on a USTRUCT in the Runtime module, which is a heavier change). The hint tells the AI to use `blueprint.read` for ground-truth pin data. This is pragmatic for Phase C and can be enhanced in a follow-up.

3. **v1.0 path is a verbatim copy**: The v1.0 executor lambda is an exact copy of the current code (lines 6107-6228 in the original). This ensures zero behavioral change for v1.0 plans.

### What NOT to change

- Do NOT modify the v1.0 executor lambda logic at all
- Do NOT modify the validation, plan parsing, Blueprint loading, preview check, graph finding, or drift detection sections (sections 1-7)
- Do NOT remove the `#include "Plan/OliveBlueprintPlanLowerer.h"` -- still needed for v1.0 path

### Checking FOliveWriteResult::ExecutionError signature

Before implementing, the coder MUST read:
- `B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Blueprint\Public\Pipeline\OliveWritePipeline.h`

Look for the `ExecutionError` factory method. If it only takes 3 parameters (code, message, suggestion), the coder must either:
- (a) Add a 4th parameter overload `static FOliveWriteResult ExecutionError(const FString& Code, const FString& Message, const FString& Suggestion, TSharedPtr<FJsonObject> ResultData)`, or
- (b) In the lambda, construct the result in two steps:

```cpp
FOliveWriteResult ErrorResult = FOliveWriteResult::ExecutionError(
    TEXT("PLAN_EXECUTION_FAILED"), Message, Suggestion);
ErrorResult.ResultData = ResultData;
return ErrorResult;
```

Option (b) is simpler and preferred.

---

## Task C5: Update HandleBlueprintPreviewPlanJson for v2.0

### File to modify

`B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Source\OliveAIEditor\Blueprint\Private\MCP\OliveBlueprintToolHandlers.cpp`

### What to change

The preview function (lines 5728-5917) currently always lowers the plan to batch ops and uses `BuildPlanSummary` / `CollectWarnings` which depend on `FOlivePlanLowerResult`. For v2.0 plans, we skip lowering and instead provide a v2.0-aware preview.

Find the code block starting at section 8 (line 5861):

```cpp
	// ------------------------------------------------------------------
	// 8. Lower to batch ops
	// ------------------------------------------------------------------
	FOlivePlanLowerResult LowerResult = FOliveBlueprintPlanLowerer::Lower(
		ResolveResult.ResolvedSteps, Plan, GraphTarget, AssetPath);
	if (!LowerResult.bSuccess)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("phase"), TEXT("lower"));
		ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(LowerResult.Errors));
		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("PLAN_LOWER_FAILED"),
			FString::Printf(TEXT("Plan lowering failed with %d error(s)"), LowerResult.Errors.Num()),
			LowerResult.Errors.Num() > 0 ? LowerResult.Errors[0].Suggestion : TEXT(""));
		Result.Data = ErrorData;
		return Result;
	}

	// ------------------------------------------------------------------
	// 9. Compute fingerprint and diff
	// ------------------------------------------------------------------
	FString Fingerprint = FOliveBlueprintPlanResolver::ComputePlanFingerprint(CurrentGraphIR, Plan);
	TSharedPtr<FJsonObject> Diff = FOliveBlueprintPlanResolver::ComputePlanDiff(
		CurrentGraphIR, ResolveResult.ResolvedSteps, Plan);

	// ------------------------------------------------------------------
	// 10. Build result
	// ------------------------------------------------------------------
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("preview_fingerprint"), Fingerprint);
	ResultData->SetObjectField(TEXT("diff"), Diff);
	ResultData->SetObjectField(TEXT("plan_summary"), BuildPlanSummary(Plan, LowerResult));
	ResultData->SetNumberField(TEXT("lowered_ops_count"), LowerResult.Ops.Num());

	if (bGraphWillBeCreated)
	{
		ResultData->SetBoolField(TEXT("will_create_graph"), true);
		ResultData->SetStringField(TEXT("new_graph_name"), GraphTarget);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings = CollectWarnings(ResolveResult, LowerResult);
	if (bGraphWillBeCreated)
	{
		Warnings.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("Function graph '%s' does not exist and will be created on apply"), *GraphTarget)));
	}
	if (Warnings.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("warnings"), Warnings);
	}

	UE_LOG(LogOliveBPTools, Log,
		TEXT("Plan preview for '%s' graph '%s': %d steps, %d lowered ops, fingerprint=%s, new_graph=%s"),
		*AssetPath, *GraphTarget, Plan.Steps.Num(), LowerResult.Ops.Num(), *Fingerprint,
		bGraphWillBeCreated ? TEXT("true") : TEXT("false"));

	return FOliveToolResult::Success(ResultData);
```

Replace with:

```cpp
	// ------------------------------------------------------------------
	// 8. Lower (v1.0 only) or skip (v2.0)
	// ------------------------------------------------------------------
	const bool bIsV2Plan = (Plan.SchemaVersion == TEXT("2.0"));

	FOlivePlanLowerResult LowerResult;
	if (!bIsV2Plan)
	{
		LowerResult = FOliveBlueprintPlanLowerer::Lower(
			ResolveResult.ResolvedSteps, Plan, GraphTarget, AssetPath);
		if (!LowerResult.bSuccess)
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("phase"), TEXT("lower"));
			ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(LowerResult.Errors));
			FOliveToolResult Result = FOliveToolResult::Error(
				TEXT("PLAN_LOWER_FAILED"),
				FString::Printf(TEXT("Plan lowering failed with %d error(s)"), LowerResult.Errors.Num()),
				LowerResult.Errors.Num() > 0 ? LowerResult.Errors[0].Suggestion : TEXT(""));
			Result.Data = ErrorData;
			return Result;
		}
	}

	// ------------------------------------------------------------------
	// 9. Compute fingerprint and diff
	// ------------------------------------------------------------------
	FString Fingerprint = FOliveBlueprintPlanResolver::ComputePlanFingerprint(CurrentGraphIR, Plan);
	TSharedPtr<FJsonObject> Diff = FOliveBlueprintPlanResolver::ComputePlanDiff(
		CurrentGraphIR, ResolveResult.ResolvedSteps, Plan);

	// ------------------------------------------------------------------
	// 10. Build result
	// ------------------------------------------------------------------
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("preview_fingerprint"), Fingerprint);
	ResultData->SetStringField(TEXT("schema_version"), Plan.SchemaVersion);
	ResultData->SetObjectField(TEXT("diff"), Diff);

	if (bIsV2Plan)
	{
		// v2.0: No lowered ops. Report step count and resolved function names.
		ResultData->SetNumberField(TEXT("resolved_steps_count"), ResolveResult.ResolvedSteps.Num());

		// Include per-step resolution summary so AI can verify resolved function names
		TArray<TSharedPtr<FJsonValue>> StepSummaries;
		StepSummaries.Reserve(ResolveResult.ResolvedSteps.Num());
		for (const FOliveResolvedStep& Resolved : ResolveResult.ResolvedSteps)
		{
			TSharedPtr<FJsonObject> StepObj = MakeShared<FJsonObject>();
			StepObj->SetStringField(TEXT("step_id"), Resolved.StepId);
			StepObj->SetStringField(TEXT("node_type"), Resolved.NodeType);

			// Include resolved function_name and target_class if present
			const FString* FnName = Resolved.Properties.Find(TEXT("function_name"));
			if (FnName && !FnName->IsEmpty())
			{
				StepObj->SetStringField(TEXT("resolved_function"), *FnName);
			}
			const FString* TargetCls = Resolved.Properties.Find(TEXT("target_class"));
			if (TargetCls && !TargetCls->IsEmpty())
			{
				StepObj->SetStringField(TEXT("resolved_class"), *TargetCls);
			}

			StepSummaries.Add(MakeShared<FJsonValueObject>(StepObj));
		}
		ResultData->SetArrayField(TEXT("resolved_steps"), StepSummaries);
		ResultData->SetStringField(TEXT("execution_mode"), TEXT("plan_executor_v2"));
	}
	else
	{
		// v1.0: Include lowered ops summary
		ResultData->SetObjectField(TEXT("plan_summary"), BuildPlanSummary(Plan, LowerResult));
		ResultData->SetNumberField(TEXT("lowered_ops_count"), LowerResult.Ops.Num());
		ResultData->SetStringField(TEXT("execution_mode"), TEXT("lowerer_v1"));
	}

	if (bGraphWillBeCreated)
	{
		ResultData->SetBoolField(TEXT("will_create_graph"), true);
		ResultData->SetStringField(TEXT("new_graph_name"), GraphTarget);
	}

	// Collect warnings from resolution (and lowering for v1.0)
	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (!bIsV2Plan)
	{
		Warnings = CollectWarnings(ResolveResult, LowerResult);
	}
	else
	{
		// v2.0: warnings come from resolution only
		for (const FString& Warn : ResolveResult.Warnings)
		{
			Warnings.Add(MakeShared<FJsonValueString>(Warn));
		}
	}

	if (bGraphWillBeCreated)
	{
		Warnings.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("Function graph '%s' does not exist and will be created on apply"), *GraphTarget)));
	}
	if (Warnings.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("warnings"), Warnings);
	}

	UE_LOG(LogOliveBPTools, Log,
		TEXT("Plan preview for '%s' graph '%s': %d steps, schema=%s, fingerprint=%s, new_graph=%s"),
		*AssetPath, *GraphTarget, Plan.Steps.Num(), *Plan.SchemaVersion, *Fingerprint,
		bGraphWillBeCreated ? TEXT("true") : TEXT("false"));

	return FOliveToolResult::Success(ResultData);
```

### What NOT to change

- Do NOT modify sections 1-7 (validation, parsing, Blueprint loading, graph finding, graph IR reading, resolution)
- The `BuildPlanSummary` and `CollectWarnings` anonymous-namespace functions remain -- they are used by the v1.0 path
- Schema definitions in `OliveBlueprintSchemas.cpp` are NOT changed in this task (the schema already accepts any schema_version string in plan_json)

---

## Task C4: Update Worker_Blueprint.txt Prompt

### File to modify

`B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\Content\SystemPrompts\Worker_Blueprint.txt`

### What to change

Replace the entire "Plan JSON Orchestration" section (lines 56-129) with an updated version that documents v2.0 as the recommended schema version, the new @ref syntax, and the enhanced resolution. The existing sections before line 56 ("Your Task", "Context From Previous Steps", etc.) and the section before line 56 ("Compilation") remain unchanged.

### Current text to replace (lines 56-129)

Everything from `## Plan JSON Orchestration` through the end of the file.

### Replacement text

```
## Plan JSON Orchestration

### Routing Policy
For graph editing tasks requiring 3 or more node operations (add_node, connect_pins, set_pin_default, etc.), you MUST use the plan JSON path over granular tools:

1. Call `blueprint.read` to get the current graph state
2. Call `blueprint.preview_plan_json` with an intent-level plan describing what you want to build
3. Review the preview response for warnings, resolution errors, or mismatches
4. Call `blueprint.apply_plan_json` with the plan and the `preview_fingerprint` from step 3

Fall back to granular tools (`blueprint.add_node`, `blueprint.connect_pins`, `blueprint.set_pin_default`) only when:
- The needed operation is not in the plan op vocabulary
- Making a single small edit (1-2 operations)
- Debugging or repairing a specific existing node (e.g., fixing one failed wire)

### Plan JSON Format (v2.0 -- RECOMMENDED)

Use `schema_version: "2.0"` for all new plans. The v2.0 executor creates all nodes first, introspects their actual pins, then wires everything using ground-truth pin names. You do NOT need to know exact UE pin names.

```json
{
  "schema_version": "2.0",
  "steps": [
    {
      "step_id": "evt",
      "op": "event",
      "target": "BeginPlay"
    },
    {
      "step_id": "get_health",
      "op": "get_var",
      "target": "Health"
    },
    {
      "step_id": "check",
      "op": "branch",
      "inputs": {
        "Condition": "@get_health.auto"
      },
      "exec_after": "evt",
      "exec_outputs": {
        "True": "on_alive",
        "False": "on_dead"
      }
    },
    {
      "step_id": "on_alive",
      "op": "call",
      "target": "PrintString",
      "inputs": {
        "InString": "Player is alive"
      }
    },
    {
      "step_id": "on_dead",
      "op": "call",
      "target": "PrintString",
      "inputs": {
        "InString": "Player is dead"
      }
    }
  ]
}
```

### Data Wire Syntax (@ref)

Values in `inputs` that start with `@` create data wire connections. Three styles:

| Syntax | Meaning | When to use |
|--------|---------|-------------|
| `@step.auto` | Auto-match by type | Default choice. The system finds the output pin whose type matches the target input. Use when the source step has one obvious output of the right type. |
| `@step.~hint` | Fuzzy match with hint | When the source has multiple outputs and you need to guide the match. The `~` prefix enables fuzzy name matching. Example: `@get_muzzle.~Location` |
| `@step.PinName` | Smart name match | When you know or can guess the pin name. The system tries exact, display name, case-insensitive, fuzzy, and type-based matching in sequence. Even if the name is slightly wrong, it usually resolves. |

For most data wires, use `@step.auto` -- it works when the source step has a single output of the correct type, which covers ~80% of cases.

### Literal Values in Inputs

Values in `inputs` that do NOT start with `@` are set as pin default values:
```json
"inputs": {
  "InString": "Hello World",
  "Duration": "2.0",
  "bPrintToScreen": "true"
}
```

### Exec Flow

- `exec_after`: Names the step whose primary exec output connects to THIS step's exec input. The system automatically finds the correct exec output/input pin names.
- `exec_outputs`: Maps named exec output pins to target step IDs. Used for multi-output nodes like Branch:
  ```json
  "exec_outputs": { "True": "step_on_true", "False": "step_on_false" }
  ```
  Pin names are matched fuzzily, so "True"/"False" work for Branch, and "Then 0"/"Then 1" work for Sequence.

### Available Plan Ops
call, get_var, set_var, branch, sequence, cast, event, custom_event, for_loop, for_each_loop, delay, is_valid, print_string, spawn_actor, make_struct, break_struct, return, comment

### Function Resolution (Enhanced)
When using `"op": "call"`, the system uses smart resolution for the `target` function name:
- Exact name matching across all loaded classes
- K2_ prefix handling (e.g., "GetActorLocation" automatically resolves to "K2_GetActorLocation")
- Common alias mapping (e.g., "Destroy" -> "K2_DestroyActor", "Print" -> "PrintString")
- Node catalog search with fuzzy matching
- Blueprint parent class hierarchy search

You do NOT need to add the K2_ prefix yourself -- just use the natural function name.
Optionally set `target_class` on the step to disambiguate if the function exists on multiple classes.

### Event Reuse
If your plan includes an event that already exists in the graph (e.g., BeginPlay), the system reuses the existing node instead of failing. This is normal and expected -- most plans start with an existing event.

### Plan JSON Rules
- `step_id` must be unique within the plan
- `exec_after` references connect execution flow between steps (like white wires in the editor)
- `@step.pinHint` in inputs creates data wire connections (like colored wires)
- Literal values in inputs set pin defaults directly
- `exec_outputs` maps named exec output pins to target step_ids
- No GUIDs or node_ids needed -- the system resolves everything
- Always read the target graph FIRST before generating a plan so you understand what exists
- If apply returns `wiring_errors`, use `blueprint.read` to see actual pin names, then fix with granular `blueprint.connect_pins` or `blueprint.set_pin_default`

### Self-Correction After Partial Success
If `blueprint.apply_plan_json` succeeds but returns `wiring_errors`, some connections failed. The nodes are created but not fully wired. To fix:
1. Read the `wiring_errors` array -- each entry tells you which step and pin failed
2. Call `blueprint.read` on the target graph to see actual pin names on created nodes
3. Use `blueprint.connect_pins` or `blueprint.set_pin_default` with the exact pin names from the read result
```

---

## Build Verification

After completing all tasks, run a build:

```bash
"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex
```

Expected new includes in the build:
- `OliveBlueprintPlanResolver.cpp` now includes `Plan/OliveFunctionResolver.h`
- `OlivePlanExecutor.cpp` now includes `Services/OliveBatchExecutionScope.h`
- `OliveBlueprintToolHandlers.cpp` now includes `Plan/OlivePlanExecutor.h`, `Plan/OlivePinManifest.h`, `Plan/OliveFunctionResolver.h`

---

## Checklist for the Coder

- [ ] C1: Add `#include "Plan/OliveFunctionResolver.h"` to OliveBlueprintPlanResolver.cpp
- [ ] C1: Replace `ResolveCallOp` body with FOliveFunctionResolver-based code
- [ ] C1: Verify the replacement compiles (the `CATALOG_SEARCH_LIMIT` constant is used in new code)
- [ ] C3: Add `#include "Services/OliveBatchExecutionScope.h"` to OlivePlanExecutor.cpp
- [ ] C3: Wrap phases 3-5 in `FOliveBatchExecutionScope` scope block
- [ ] C2: Add 3 new includes to OliveBlueprintToolHandlers.cpp
- [ ] C2: Check `FOliveWriteResult::ExecutionError` signature for optional ResultData param
- [ ] C2: Replace section 8 in HandleBlueprintApplyPlanJson with version-gated resolve+lower
- [ ] C2: Replace sections 9-11 with version-gated executor and enhanced result forwarding
- [ ] C2: Verify v1.0 path is byte-for-byte identical to original (modulo the enclosing if/else)
- [ ] C5: Replace sections 8-10 in HandleBlueprintPreviewPlanJson with version-gated code
- [ ] C5: v2.0 preview includes `resolved_steps` array with resolved function/class names
- [ ] C4: Replace Plan JSON Orchestration section in Worker_Blueprint.txt
- [ ] C4: Verify the replacement documents `schema_version: "2.0"`, `@step.auto`, `@step.~hint`
- [ ] Build succeeds with zero errors
- [ ] Test: v1.0 plan preview still works (unchanged behavior)
- [ ] Test: v1.0 plan apply still works (unchanged behavior)
- [ ] Test: v2.0 plan preview returns `resolved_steps` array
- [ ] Test: v2.0 plan apply creates nodes and wires them
