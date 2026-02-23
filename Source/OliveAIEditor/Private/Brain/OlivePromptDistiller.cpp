// Copyright Bode Software. All Rights Reserved.

#include "Brain/OlivePromptDistiller.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

FOlivePromptDistiller::FOlivePromptDistiller()
{
}

FOliveDistillationResult FOlivePromptDistiller::Distill(TArray<FOliveChatMessage>& Messages, const FOliveDistillationConfig& Config) const
{
	FOliveDistillationResult Result;

	if (Messages.Num() == 0)
	{
		return Result;
	}

	// Find all tool result message indices (Role == Tool)
	TArray<int32> ToolResultIndices;
	for (int32 i = 0; i < Messages.Num(); i++)
	{
		if (Messages[i].Role == EOliveChatRole::Tool)
		{
			ToolResultIndices.Add(i);
		}
	}

	if (ToolResultIndices.Num() == 0)
	{
		return Result;
	}

	// Determine which tool results to keep verbatim
	// Keep the last Config.RecentPairsToKeep results verbatim (unless oversized)
	const int32 NumToKeep = FMath::Min(Config.RecentPairsToKeep, ToolResultIndices.Num());
	const int32 FirstKeptIndex = ToolResultIndices.Num() - NumToKeep;

	for (int32 j = 0; j < ToolResultIndices.Num(); j++)
	{
		const int32 MsgIdx = ToolResultIndices[j];
		FOliveChatMessage& Msg = Messages[MsgIdx];

		const bool bIsRecent = (j >= FirstKeptIndex);
		const bool bIsOversized = (Msg.Content.Len() > Config.MaxResultChars);

		// Summarize if: older than the keep window, OR oversized even if recent
		if (!bIsRecent || bIsOversized)
		{
			const int32 OriginalLen = Msg.Content.Len();
			const FString Summary = SummarizeToolResult(Msg.ToolName, Msg.Content);
			Msg.Content = Summary;

			Result.MessagesSummarized++;
			if (bIsOversized)
			{
				Result.ToolResultsTruncated++;
			}
			// Estimate tokens saved from this summarization
			const int32 OriginalTokens = FMath::CeilToInt(OriginalLen / CharsPerToken);
			const int32 SummaryTokens = EstimateTokens(Summary);
			Result.TokensSaved += FMath::Max(0, OriginalTokens - SummaryTokens);
		}
	}

	if (Result.MessagesSummarized > 0)
	{
		UE_LOG(LogOliveAI, Verbose, TEXT("PromptDistiller: Summarized %d/%d tool results (%d truncated, ~%d tokens saved)"),
			Result.MessagesSummarized, ToolResultIndices.Num(), Result.ToolResultsTruncated, Result.TokensSaved);
	}

	// Pass 2: Enforce total character budget for tool results
	if (Config.MaxTotalResultChars > 0)
	{
		int32 TotalResultChars = 0;
		for (int32 i = 0; i < Messages.Num(); i++)
		{
			if (Messages[i].Role == EOliveChatRole::Tool)
			{
				TotalResultChars += Messages[i].Content.Len();
			}
		}

		if (TotalResultChars > Config.MaxTotalResultChars)
		{
			for (int32 i = 0; i < Messages.Num() && TotalResultChars > Config.MaxTotalResultChars; i++)
			{
				if (Messages[i].Role != EOliveChatRole::Tool)
				{
					continue;
				}

				// Skip already-summarized messages (they start with '[toolname]')
				if (Messages[i].Content.StartsWith(TEXT("[")))
				{
					continue;
				}

				const int32 OriginalLen = Messages[i].Content.Len();
				Messages[i].Content = SummarizeToolResult(Messages[i].ToolName, Messages[i].Content);
				const int32 NewLen = Messages[i].Content.Len();
				TotalResultChars -= (OriginalLen - NewLen);

				Result.MessagesSummarized++;
				const int32 OriginalTokens = FMath::CeilToInt(OriginalLen / CharsPerToken);
				const int32 NewTokens = EstimateTokens(Messages[i].Content);
				Result.TokensSaved += FMath::Max(0, OriginalTokens - NewTokens);
			}

			UE_LOG(LogOliveAI, Verbose, TEXT("PromptDistiller: Budget pass summarized tool results (remaining %d chars, budget %d)"),
				TotalResultChars, Config.MaxTotalResultChars);
		}
	}

	// Pass 3: Truncate old assistant messages that are excessively long
	if (Config.MaxAssistantChars > 0)
	{
		// Find the last assistant message index — always keep it verbatim
		int32 LastAssistantIdx = -1;
		for (int32 i = Messages.Num() - 1; i >= 0; --i)
		{
			if (Messages[i].Role == EOliveChatRole::Assistant)
			{
				LastAssistantIdx = i;
				break;
			}
		}

		for (int32 i = 0; i < Messages.Num(); i++)
		{
			if (Messages[i].Role == EOliveChatRole::Assistant
				&& i != LastAssistantIdx
				&& Messages[i].Content.Len() > Config.MaxAssistantChars)
			{
				const int32 OriginalLen = Messages[i].Content.Len();
				Messages[i].Content = Messages[i].Content.Left(Config.MaxAssistantChars)
					+ TEXT("\n[... truncated for context budget ...]");

				Result.MessagesSummarized++;
				const int32 SavedChars = OriginalLen - Messages[i].Content.Len();
				Result.TokensSaved += FMath::Max(0, FMath::CeilToInt(SavedChars / CharsPerToken));
			}
		}
	}

	return Result;
}

FString FOlivePromptDistiller::SummarizeToolResult(const FString& ToolName, const FString& ResultContent) const
{
	const bool bSuccess = IsSuccessResult(ResultContent);
	const FString Brief = ExtractBrief(ResultContent);

	return FString::Printf(TEXT("[%s] %s: %s"),
		*ToolName,
		bSuccess ? TEXT("SUCCESS") : TEXT("FAILED"),
		*Brief);
}

int32 FOlivePromptDistiller::EstimateTokenCount(const TArray<FOliveChatMessage>& Messages) const
{
	int32 Total = 0;
	for (const FOliveChatMessage& Msg : Messages)
	{
		Total += EstimateTokens(Msg.Content);
		// Add overhead for role, tool_call_id etc.
		Total += 10;
	}
	return Total;
}

int32 FOlivePromptDistiller::EstimateTokens(const FString& Text) const
{
	return FMath::CeilToInt(Text.Len() / CharsPerToken);
}

FString FOlivePromptDistiller::ExtractBrief(const FString& JsonContent) const
{
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		// Not valid JSON — truncate raw content
		if (JsonContent.Len() > 100)
		{
			return JsonContent.Left(100) + TEXT("...");
		}
		return JsonContent;
	}

	// Try to extract meaningful fields in priority order
	FString Brief;

	// Check for asset_path or blueprint_path
	FString AssetPath;
	if (JsonObj->TryGetStringField(TEXT("asset_path"), AssetPath) ||
		JsonObj->TryGetStringField(TEXT("blueprint_path"), AssetPath))
	{
		Brief += AssetPath;
	}

	// Check for a message field
	FString Message;
	if (JsonObj->TryGetStringField(TEXT("message"), Message))
	{
		if (!Brief.IsEmpty()) Brief += TEXT(" - ");
		Brief += Message;
	}

	// Check for description
	FString Description;
	if (JsonObj->TryGetStringField(TEXT("description"), Description))
	{
		if (!Brief.IsEmpty()) Brief += TEXT(" - ");
		Brief += Description;
	}

	// Check for error info
	const TSharedPtr<FJsonObject>* ErrorObj;
	if (JsonObj->TryGetObjectField(TEXT("error"), ErrorObj))
	{
		FString ErrMsg;
		if ((*ErrorObj)->TryGetStringField(TEXT("message"), ErrMsg))
		{
			if (!Brief.IsEmpty()) Brief += TEXT(" - ");
			Brief += ErrMsg;
		}
	}

	// Check for compile_result
	const TSharedPtr<FJsonObject>* CompileResult;
	if (JsonObj->TryGetObjectField(TEXT("compile_result"), CompileResult))
	{
		bool bCompileSuccess = true;
		(*CompileResult)->TryGetBoolField(TEXT("success"), bCompileSuccess);
		if (!Brief.IsEmpty()) Brief += TEXT(" - ");
		Brief += bCompileSuccess ? TEXT("compiled OK") : TEXT("compile FAILED");
	}

	// Fallback: count of top-level fields
	if (Brief.IsEmpty())
	{
		TArray<FString> FieldNames;
		JsonObj->Values.GetKeys(FieldNames);
		Brief = FString::Printf(TEXT("result with %d fields: %s"),
			FieldNames.Num(),
			*FString::Join(FieldNames, TEXT(", ")));
	}

	// Truncate if still too long
	if (Brief.Len() > 200)
	{
		Brief = Brief.Left(200) + TEXT("...");
	}

	return Brief;
}

bool FOlivePromptDistiller::IsSuccessResult(const FString& JsonContent) const
{
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		return false;
	}

	bool bSuccess = false;
	if (JsonObj->TryGetBoolField(TEXT("success"), bSuccess))
	{
		return bSuccess;
	}

	// No explicit success field — assume success if no error field
	return !JsonObj->HasField(TEXT("error"));
}
