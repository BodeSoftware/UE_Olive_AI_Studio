// Copyright Bode Software. All Rights Reserved.

#include "Brain/OliveSelfCorrectionPolicy.h"

bool FOliveSelfCorrectionPolicy::ShouldRetry(const FOliveToolResult& Result, int32 Attempt) const
{
	if (Result.bSuccess) return false;
	if (Attempt != 1) return false;
	if (Result.Messages.Num() == 0) return false;
	return IsTransient(Result.Messages[0].Code);
}

bool FOliveSelfCorrectionPolicy::IsTransient(const FString& ErrorCode)
{
	if (ErrorCode.Equals(TEXT("TIMEOUT"), ESearchCase::IgnoreCase)) return true;
	if (ErrorCode.Equals(TEXT("RATE_LIMIT"), ESearchCase::IgnoreCase)) return true;
	if (ErrorCode.StartsWith(TEXT("HTTP_5"), ESearchCase::IgnoreCase)) return true;
	if (ErrorCode.Contains(TEXT("TRANSIENT"), ESearchCase::IgnoreCase)) return true;
	return false;
}
