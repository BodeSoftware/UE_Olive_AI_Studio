// Copyright Bode Software. All Rights Reserved.

#include "UI/OliveResultCards.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveResultCards, Log, All);

FOliveResultCardData FOliveResultCardData::FromToolResult(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& ResultJson)
{
	FOliveResultCardData Card;
	Card.RawJson = ResultJson;

	if (!ResultJson.IsValid())
	{
		Card.CardType = EOliveResultCardType::RawJson;
		Card.Title = TEXT("Result");
		return Card;
	}

	if (ResultJson->HasField(TEXT("success")))
	{
		Card.bSuccess = ResultJson->GetBoolField(TEXT("success"));
	}

	if (ResultJson->HasField(TEXT("asset_path")))
	{
		Card.AssetPath = ResultJson->GetStringField(TEXT("asset_path"));
	}
	else if (ResultJson->HasField(TEXT("path")))
	{
		Card.AssetPath = ResultJson->GetStringField(TEXT("path"));
	}

	// ==========================================
	// Blueprint Read Summary
	// ==========================================
	if (ToolName == TEXT("blueprint.read") || ToolName.Contains(TEXT("blueprint.read")))
	{
		Card.CardType = EOliveResultCardType::BlueprintReadSummary;
		Card.Title = TEXT("Blueprint Summary");

		const TArray<TSharedPtr<FJsonValue>>* TempArray;
		if (ResultJson->TryGetArrayField(TEXT("variables"), TempArray))
			Card.VariableCount = TempArray->Num();
		if (ResultJson->TryGetArrayField(TEXT("functions"), TempArray))
			Card.FunctionCount = TempArray->Num();
		if (ResultJson->TryGetArrayField(TEXT("graphs"), TempArray))
			Card.GraphCount = TempArray->Num();
		if (ResultJson->TryGetArrayField(TEXT("components"), TempArray))
			Card.ComponentCount = TempArray->Num();

		FString ParentClassStr;
		if (ResultJson->TryGetStringField(TEXT("parent_class"), ParentClassStr))
			Card.ParentClass = ParentClassStr;

		Card.Subtitle = FString::Printf(TEXT("%d vars, %d funcs, %d graphs, %d components"),
			Card.VariableCount, Card.FunctionCount, Card.GraphCount, Card.ComponentCount);

		if (!Card.AssetPath.IsEmpty())
		{
			FOliveNavigationAction OpenAction;
			OpenAction.Label = TEXT("Open in Editor");
			OpenAction.AssetPath = Card.AssetPath;
			Card.Actions.Add(OpenAction);
		}
		return Card;
	}

	// ==========================================
	// Compile Errors (check before blueprint write — any tool can have these)
	// ==========================================
	const TSharedPtr<FJsonObject>* CompileResultObj;
	if (ResultJson->TryGetObjectField(TEXT("compile_result"), CompileResultObj))
	{
		bool bCompileSuccess = true;
		(*CompileResultObj)->TryGetBoolField(TEXT("success"), bCompileSuccess);

		const TArray<TSharedPtr<FJsonValue>>* ErrorsArray = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* WarningsArray = nullptr;

		bool bHasErrors = (*CompileResultObj)->TryGetArrayField(TEXT("errors"), ErrorsArray) && ErrorsArray->Num() > 0;
		bool bHasWarnings = (*CompileResultObj)->TryGetArrayField(TEXT("warnings"), WarningsArray) && WarningsArray->Num() > 0;

		if (!bCompileSuccess || bHasErrors)
		{
			Card.CardType = EOliveResultCardType::CompileErrors;
			Card.Title = TEXT("Compile Result");
			Card.bSuccess = bCompileSuccess;

			if (bHasErrors && ErrorsArray)
			{
				for (const TSharedPtr<FJsonValue>& ErrVal : *ErrorsArray)
				{
					if (ErrVal->AsObject().IsValid())
					{
						FOliveIRCompileError CompileErr = FOliveIRCompileError::FromJson(ErrVal->AsObject());
						Card.Errors.Add(CompileErr);

						if (!CompileErr.NodeId.IsEmpty() && !Card.AssetPath.IsEmpty())
						{
							FOliveNavigationAction GoToError;
							GoToError.Label = FString::Printf(TEXT("Go to: %s"),
								CompileErr.NodeName.IsEmpty() ? *CompileErr.NodeId : *CompileErr.NodeName);
							GoToError.AssetPath = Card.AssetPath;
							GoToError.NodeIds.Add(CompileErr.NodeId);
							GoToError.bIsCompileError = true;
							GoToError.CompileError = CompileErr;
							Card.Actions.Add(GoToError);
						}
					}
				}
			}

			if (bHasWarnings && WarningsArray)
			{
				for (const TSharedPtr<FJsonValue>& WarnVal : *WarningsArray)
				{
					if (WarnVal->AsObject().IsValid())
					{
						Card.Warnings.Add(FOliveIRCompileError::FromJson(WarnVal->AsObject()));
					}
				}
			}

			Card.Subtitle = FString::Printf(TEXT("%d errors, %d warnings"),
				Card.Errors.Num(), Card.Warnings.Num());
			return Card;
		}
	}

	// ==========================================
	// Blueprint Write Result
	// ==========================================
	if (ToolName.StartsWith(TEXT("blueprint.")) && ToolName != TEXT("blueprint.read"))
	{
		Card.CardType = EOliveResultCardType::BlueprintWriteResult;
		Card.Title = Card.bSuccess ? TEXT("Operation Complete") : TEXT("Operation Failed");

		FString Description;
		if (ResultJson->TryGetStringField(TEXT("description"), Description))
		{
			Card.OperationDescription = Description;
		}
		else
		{
			Card.OperationDescription = ToolName;
			Card.OperationDescription.ReplaceInline(TEXT("blueprint."), TEXT(""));
			Card.OperationDescription.ReplaceInline(TEXT("_"), TEXT(" "));
		}

		FString CreatedItem;
		if (ResultJson->TryGetStringField(TEXT("created_item"), CreatedItem) ||
			ResultJson->TryGetStringField(TEXT("name"), CreatedItem))
		{
			Card.CreatedItemName = CreatedItem;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodeIdsArray;
		if (ResultJson->TryGetArrayField(TEXT("created_node_ids"), NodeIdsArray))
		{
			for (const TSharedPtr<FJsonValue>& Val : *NodeIdsArray)
			{
				Card.CreatedNodeIds.Add(Val->AsString());
			}
		}

		double TimeMsVal = 0.0;
		if (ResultJson->TryGetNumberField(TEXT("execution_time_ms"), TimeMsVal))
		{
			Card.ExecutionTimeMs = TimeMsVal;
		}

		Card.Subtitle = Card.CreatedItemName.IsEmpty()
			? Card.OperationDescription
			: FString::Printf(TEXT("Created: %s"), *Card.CreatedItemName);

		FString GraphName;
		ResultJson->TryGetStringField(TEXT("graph_name"), GraphName);

		if (!Card.AssetPath.IsEmpty())
		{
			if (Card.CreatedNodeIds.Num() > 0)
			{
				FOliveNavigationAction SelectNodes;
				SelectNodes.Label = TEXT("Select Nodes");
				SelectNodes.AssetPath = Card.AssetPath;
				SelectNodes.NodeIds = Card.CreatedNodeIds;
				SelectNodes.GraphName = GraphName;
				Card.Actions.Add(SelectNodes);
			}

			if (!GraphName.IsEmpty())
			{
				FOliveNavigationAction OpenGraph;
				OpenGraph.Label = TEXT("Open Graph");
				OpenGraph.AssetPath = Card.AssetPath;
				OpenGraph.GraphName = GraphName;
				Card.Actions.Add(OpenGraph);
			}
			else
			{
				FOliveNavigationAction OpenAsset;
				OpenAsset.Label = TEXT("Open in Editor");
				OpenAsset.AssetPath = Card.AssetPath;
				Card.Actions.Add(OpenAsset);
			}
		}
		return Card;
	}

	// ==========================================
	// Snapshot Info
	// ==========================================
	if (ToolName.Contains(TEXT("snapshot")))
	{
		Card.CardType = EOliveResultCardType::SnapshotInfo;
		Card.Title = TEXT("Snapshot");

		ResultJson->TryGetStringField(TEXT("snapshot_id"), Card.SnapshotId);
		ResultJson->TryGetStringField(TEXT("name"), Card.SnapshotName);

		const TArray<TSharedPtr<FJsonValue>>* AssetsArray;
		if (ResultJson->TryGetArrayField(TEXT("assets"), AssetsArray))
		{
			Card.AssetCount = AssetsArray->Num();
			for (const TSharedPtr<FJsonValue>& Val : *AssetsArray)
			{
				Card.ChangedAssets.Add(Val->AsString());
			}
		}

		int32 CountVal = 0;
		if (ResultJson->TryGetNumberField(TEXT("asset_count"), CountVal))
		{
			Card.AssetCount = CountVal;
		}

		Card.Subtitle = FString::Printf(TEXT("%s (%d assets)"), *Card.SnapshotName, Card.AssetCount);

		if (!Card.SnapshotId.IsEmpty())
		{
			FOliveNavigationAction RollbackAction;
			RollbackAction.Label = TEXT("Rollback");
			RollbackAction.SnapshotId = Card.SnapshotId;
			Card.Actions.Add(RollbackAction);
		}
		return Card;
	}

	// ==========================================
	// Fallback: Raw JSON
	// ==========================================
	Card.CardType = EOliveResultCardType::RawJson;
	Card.Title = ToolName;
	Card.Subtitle = Card.bSuccess ? TEXT("Success") : TEXT("Failed");
	return Card;
}
