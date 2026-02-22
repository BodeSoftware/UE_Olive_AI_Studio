// Copyright Bode Software. All Rights Reserved.

#include "Brain/OliveOperationHistory.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FOliveOperationHistoryStore::FOliveOperationHistoryStore()
{
}

// ==========================================
// Recording
// ==========================================

void FOliveOperationHistoryStore::RecordOperation(FOliveOperationRecord& Record)
{
	Record.Sequence = NextSequence++;

	if (Record.Timestamp == FDateTime())
	{
		Record.Timestamp = FDateTime::UtcNow();
	}

	Records.Add(Record);

	UE_LOG(LogOliveAI, Verbose, TEXT("OperationHistory: Recorded #%d %s [%s] %s"),
		Record.Sequence,
		*Record.ToolName,
		*Record.RunId,
		Record.Status == EOliveOperationStatus::Success ? TEXT("OK") : TEXT("FAIL"));
}

// ==========================================
// Summarization
// ==========================================

FString FOliveOperationHistoryStore::BuildPromptSummary(int32 TokenBudget) const
{
	if (Records.Num() == 0)
	{
		return TEXT("");
	}

	// Small budget: one-line summary
	if (TokenBudget < 500)
	{
		int32 Successes = 0, Failures = 0;
		TSet<FString> Assets;
		for (const FOliveOperationRecord& R : Records)
		{
			if (R.Status == EOliveOperationStatus::Success) Successes++;
			else if (R.Status == EOliveOperationStatus::Failed) Failures++;
			for (const FString& A : R.AffectedAssets) Assets.Add(A);
		}

		return FString::Printf(TEXT("Session: %d operations (%d succeeded, %d failed), %d assets affected."),
			Records.Num(), Successes, Failures, Assets.Num());
	}

	// Medium budget: grouped by asset
	if (TokenBudget < 2000)
	{
		FString Summary;
		Summary += TEXT("## Operation Summary\n");

		// Group by affected asset
		TMap<FString, TArray<const FOliveOperationRecord*>> ByAsset;
		for (const FOliveOperationRecord& R : Records)
		{
			if (R.AffectedAssets.Num() > 0)
			{
				for (const FString& Asset : R.AffectedAssets)
				{
					ByAsset.FindOrAdd(Asset).Add(&R);
				}
			}
			else
			{
				ByAsset.FindOrAdd(TEXT("(no asset)")).Add(&R);
			}
		}

		for (const auto& Pair : ByAsset)
		{
			int32 Ok = 0, Fail = 0;
			for (const FOliveOperationRecord* R : Pair.Value)
			{
				if (R->Status == EOliveOperationStatus::Success) Ok++;
				else Fail++;
			}

			Summary += FString::Printf(TEXT("- **%s**: %d ops (%d ok, %d failed)\n"),
				*Pair.Key, Pair.Value.Num(), Ok, Fail);

			// If budget allows, show tool names
			int32 CurrentTokens = EstimateTokens(Summary);
			if (CurrentTokens < TokenBudget - 200)
			{
				for (const FOliveOperationRecord* R : Pair.Value)
				{
					Summary += FString::Printf(TEXT("  - %s: %s\n"),
						*R->ToolName,
						R->Status == EOliveOperationStatus::Success ? TEXT("OK") : *R->ErrorMessage);

					if (EstimateTokens(Summary) >= TokenBudget - 100)
					{
						Summary += TEXT("  - ...(truncated)\n");
						break;
					}
				}
			}
		}

		return Summary;
	}

	// Large budget: per-operation detail
	FString Summary;
	Summary += TEXT("## Detailed Operation History\n");

	for (int32 i = 0; i < Records.Num(); i++)
	{
		const FOliveOperationRecord& R = Records[i];
		Summary += DetailRecord(R);

		if (EstimateTokens(Summary) >= TokenBudget - 100)
		{
			Summary += FString::Printf(TEXT("\n...(+%d more operations truncated)\n"),
				Records.Num() - i - 1);
			break;
		}
	}

	return Summary;
}

FString FOliveOperationHistoryStore::BuildWorkerContext(const FString& RunId, int32 UpToStep) const
{
	FString Context;
	Context += TEXT("Previous steps completed:\n");

	// Collect records for the specified run up to the step
	TMap<FString, TArray<const FOliveOperationRecord*>> ByAsset;

	for (const FOliveOperationRecord& R : Records)
	{
		if (R.RunId == RunId && R.StepIndex <= UpToStep && R.Status == EOliveOperationStatus::Success)
		{
			for (const FString& Asset : R.AffectedAssets)
			{
				ByAsset.FindOrAdd(Asset).Add(&R);
			}
		}
	}

	for (const auto& Pair : ByAsset)
	{
		Context += FString::Printf(TEXT("- **%s**:\n"), *Pair.Key);
		for (const FOliveOperationRecord* R : Pair.Value)
		{
			Context += FString::Printf(TEXT("  - %s\n"), *SummarizeRecord(*R));
		}
	}

	if (ByAsset.Num() == 0)
	{
		Context += TEXT("  (no previous operations)\n");
	}

	return Context;
}

FString FOliveOperationHistoryStore::BuildRunReport(const FString& RunId) const
{
	int32 Succeeded = 0, Failed = 0, Skipped = 0;
	GetRunStats(RunId, Succeeded, Failed, Skipped);

	const int32 Total = Succeeded + Failed + Skipped;

	FString Report;
	Report += FString::Printf(TEXT("## Run Report [%s]\n"), *RunId);
	Report += FString::Printf(TEXT("Total operations: %d\n"), Total);
	Report += FString::Printf(TEXT("Succeeded: %d | Failed: %d | Skipped: %d\n\n"), Succeeded, Failed, Skipped);

	// List failures
	for (const FOliveOperationRecord& R : Records)
	{
		if (R.RunId == RunId && R.Status == EOliveOperationStatus::Failed)
		{
			Report += FString::Printf(TEXT("FAILED: %s — %s\n"), *R.ToolName, *R.ErrorMessage);
		}
	}

	return Report;
}

FString FOliveOperationHistoryStore::BuildModelContext(int32 TokenBudget, int32 RawResultCount) const
{
	if (Records.Num() == 0)
	{
		return TEXT("");
	}

	// Identify distinct RunIds and find the current (most recent) run
	TArray<FString> RunIds;
	for (const FOliveOperationRecord& R : Records)
	{
		RunIds.AddUnique(R.RunId);
	}

	const FString CurrentRunId = RunIds.Last();

	// Collect records for current run vs previous runs
	TArray<const FOliveOperationRecord*> CurrentRunRecords;
	TArray<const FOliveOperationRecord*> PreviousRunRecords;

	for (const FOliveOperationRecord& R : Records)
	{
		if (R.RunId == CurrentRunId)
		{
			CurrentRunRecords.Add(&R);
		}
		else
		{
			PreviousRunRecords.Add(&R);
		}
	}

	FString Context;

	// Tier 1: Previous completed runs -> compressed summaries
	if (PreviousRunRecords.Num() > 0)
	{
		// Group previous records by RunId for compressed summaries
		TMap<FString, TArray<const FOliveOperationRecord*>> ByRun;
		for (const FOliveOperationRecord* R : PreviousRunRecords)
		{
			ByRun.FindOrAdd(R->RunId).Add(R);
		}

		Context += TEXT("### Previous Runs\n");
		for (const auto& Pair : ByRun)
		{
			int32 Ok = 0, Fail = 0;
			TSet<FString> Assets;
			TSet<FString> ToolNames;
			for (const FOliveOperationRecord* R : Pair.Value)
			{
				if (R->Status == EOliveOperationStatus::Success) Ok++;
				else if (R->Status == EOliveOperationStatus::Failed) Fail++;
				for (const FString& A : R->AffectedAssets) Assets.Add(A);
				ToolNames.Add(R->ToolName);
			}

			Context += FString::Printf(TEXT("Earlier: %d ops (%d ok, %d failed)"),
				Pair.Value.Num(), Ok, Fail);

			if (Assets.Num() > 0)
			{
				TArray<FString> AssetArr = Assets.Array();
				const int32 MaxShow = FMath::Min(3, AssetArr.Num());
				Context += TEXT(" on ");
				for (int32 i = 0; i < MaxShow; i++)
				{
					if (i > 0) Context += TEXT(", ");
					Context += AssetArr[i];
				}
				if (AssetArr.Num() > MaxShow)
				{
					Context += FString::Printf(TEXT(" (+%d more)"), AssetArr.Num() - MaxShow);
				}
			}
			Context += TEXT("\n");

			if (EstimateTokens(Context) >= TokenBudget - 200)
			{
				Context += TEXT("...(earlier runs truncated)\n");
				break;
			}
		}
		Context += TEXT("\n");
	}

	// Tier 2 & 3: Current run operations
	if (CurrentRunRecords.Num() > 0)
	{
		const int32 NumRaw = FMath::Min(RawResultCount, CurrentRunRecords.Num());
		const int32 SummaryEnd = CurrentRunRecords.Num() - NumRaw;

		// Tier 2: Older ops in current run -> one-line summaries
		if (SummaryEnd > 0)
		{
			Context += TEXT("### Current Run (Summary)\n");
			for (int32 i = 0; i < SummaryEnd; i++)
			{
				Context += FString::Printf(TEXT("[%d] %s\n"),
					i + 1, *SummarizeRecord(*CurrentRunRecords[i]));

				if (EstimateTokens(Context) >= TokenBudget - 300)
				{
					Context += FString::Printf(TEXT("...(%d more summarized ops truncated)\n"),
						SummaryEnd - i - 1);
					break;
				}
			}
			Context += TEXT("\n");
		}

		// Tier 3: Last N ops in current run -> full detail
		Context += TEXT("### Current Run (Recent Detail)\n");
		for (int32 i = SummaryEnd; i < CurrentRunRecords.Num(); i++)
		{
			Context += DetailRecord(*CurrentRunRecords[i]);

			if (EstimateTokens(Context) >= TokenBudget - 50)
			{
				Context += TEXT("\n...(truncated due to token budget)\n");
				break;
			}
		}
	}

	// Final truncation safety check
	const int32 MaxChars = TokenBudget * 4;
	if (Context.Len() > MaxChars)
	{
		Context = Context.Left(MaxChars - 30) + TEXT("\n...(truncated)\n");
	}

	return Context;
}

// ==========================================
// Queries
// ==========================================

TArray<FOliveOperationRecord> FOliveOperationHistoryStore::GetRunHistory(const FString& RunId) const
{
	TArray<FOliveOperationRecord> Result;
	for (const FOliveOperationRecord& R : Records)
	{
		if (R.RunId == RunId)
		{
			Result.Add(R);
		}
	}
	return Result;
}

TArray<FOliveOperationRecord> FOliveOperationHistoryStore::GetStepHistory(const FString& RunId, int32 StepIndex) const
{
	TArray<FOliveOperationRecord> Result;
	for (const FOliveOperationRecord& R : Records)
	{
		if (R.RunId == RunId && R.StepIndex == StepIndex)
		{
			Result.Add(R);
		}
	}
	return Result;
}

void FOliveOperationHistoryStore::GetRunStats(const FString& RunId,
	int32& OutSucceeded, int32& OutFailed, int32& OutSkipped) const
{
	OutSucceeded = 0;
	OutFailed = 0;
	OutSkipped = 0;

	for (const FOliveOperationRecord& R : Records)
	{
		if (R.RunId == RunId)
		{
			switch (R.Status)
			{
			case EOliveOperationStatus::Success: OutSucceeded++; break;
			case EOliveOperationStatus::Failed: OutFailed++; break;
			case EOliveOperationStatus::Skipped: OutSkipped++; break;
			default: break;
			}
		}
	}
}

void FOliveOperationHistoryStore::Clear()
{
	Records.Empty();
	NextSequence = 1;
}

// ==========================================
// Private Helpers
// ==========================================

int32 FOliveOperationHistoryStore::EstimateTokens(const FString& Text) const
{
	return FMath::CeilToInt(Text.Len() / 4.0f);
}

FString FOliveOperationHistoryStore::SummarizeRecord(const FOliveOperationRecord& Record) const
{
	const TCHAR* StatusStr = TEXT("OK");
	switch (Record.Status)
	{
	case EOliveOperationStatus::Failed: StatusStr = TEXT("FAILED"); break;
	case EOliveOperationStatus::Skipped: StatusStr = TEXT("SKIPPED"); break;
	case EOliveOperationStatus::Cancelled: StatusStr = TEXT("CANCELLED"); break;
	case EOliveOperationStatus::RequiresConfirmation: StatusStr = TEXT("PENDING"); break;
	default: break;
	}

	FString Line = FString::Printf(TEXT("[%s] %s"), StatusStr, *Record.ToolName);

	if (!Record.ErrorMessage.IsEmpty() && Record.Status == EOliveOperationStatus::Failed)
	{
		Line += FString::Printf(TEXT(" — %s"), *Record.ErrorMessage);
	}

	return Line;
}

FString FOliveOperationHistoryStore::DetailRecord(const FOliveOperationRecord& Record) const
{
	FString Detail;
	Detail += FString::Printf(TEXT("\n### #%d %s\n"), Record.Sequence, *Record.ToolName);
	Detail += FString::Printf(TEXT("Status: %s\n"),
		Record.Status == EOliveOperationStatus::Success ? TEXT("Success") : TEXT("Failed"));

	if (Record.AffectedAssets.Num() > 0)
	{
		Detail += TEXT("Assets: ");
		Detail += FString::Join(Record.AffectedAssets, TEXT(", "));
		Detail += TEXT("\n");
	}

	// Include key params (abbreviated)
	if (Record.Params.IsValid())
	{
		FString ParamsStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ParamsStr);
		FJsonSerializer::Serialize(Record.Params.ToSharedRef(), Writer);

		if (ParamsStr.Len() > 200)
		{
			ParamsStr = ParamsStr.Left(200) + TEXT("...");
		}
		Detail += FString::Printf(TEXT("Params: %s\n"), *ParamsStr);
	}

	if (!Record.ErrorMessage.IsEmpty())
	{
		Detail += FString::Printf(TEXT("Error: %s\n"), *Record.ErrorMessage);
	}

	return Detail;
}
