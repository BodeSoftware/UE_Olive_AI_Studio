// Copyright Bode Software. All Rights Reserved.

#include "Brain/OlivePromptDistiller.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

FOlivePromptDistiller::FOlivePromptDistiller()
{
}

void FOlivePromptDistiller::Distill(TArray<FOliveChatMessage>& Messages, const FOliveDistillationConfig& Config) const
{
	if (Messages.Num() == 0)
	{
		return;
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
		return;
	}

	// Determine which tool results to keep verbatim
	// Keep the last Config.RecentPairsToKeep results verbatim (unless oversized)
	const int32 NumToKeep = FMath::Min(Config.RecentPairsToKeep, ToolResultIndices.Num());
	const int32 FirstKeptIndex = ToolResultIndices.Num() - NumToKeep;

	int32 SummarizedCount = 0;

	for (int32 j = 0; j < ToolResultIndices.Num(); j++)
	{
		const int32 MsgIdx = ToolResultIndices[j];
		FOliveChatMessage& Msg = Messages[MsgIdx];

		const bool bIsRecent = (j >= FirstKeptIndex);
		const bool bIsOversized = (Msg.Content.Len() > Config.MaxResultChars);

		// Summarize if: older than the keep window, OR oversized even if recent
		if (!bIsRecent || bIsOversized)
		{
			const FString Summary = SummarizeToolResult(Msg.ToolName, Msg.Content);
			Msg.Content = Summary;
			SummarizedCount++;
		}
	}

	if (SummarizedCount > 0)
	{
		UE_LOG(LogOliveAI, Verbose, TEXT("PromptDistiller: Summarized %d/%d tool results"),
			SummarizedCount, ToolResultIndices.Num());
	}
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
