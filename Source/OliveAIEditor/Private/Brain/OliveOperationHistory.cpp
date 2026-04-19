// Copyright Bode Software. All Rights Reserved.

#include "Brain/OliveOperationHistory.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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
// Context
// ==========================================

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
		Line += FString::Printf(TEXT(" - %s"), *Record.ErrorMessage);
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
