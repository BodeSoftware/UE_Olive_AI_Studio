// Copyright Bode Software. All Rights Reserved.

#include "Brain/OliveRetryPolicy.h"
#include "OliveAIEditorModule.h"
#include "Misc/CRC.h"

void FOliveLoopDetector::RecordAttempt(const FString& ErrorSignature, const FString& AttemptedFix)
{
	TArray<FString>& Fixes = AttemptHistory.FindOrAdd(ErrorSignature);
	Fixes.Add(AttemptedFix);
	RecentErrors.Add(ErrorSignature);
	TotalAttempts++;

	UE_LOG(LogOliveAI, Verbose, TEXT("Loop detector: recorded attempt #%d for signature '%s'"),
		Fixes.Num(), *ErrorSignature);
}

int32 FOliveLoopDetector::GetAttemptCount(const FString& ErrorSignature) const
{
	const TArray<FString>* Fixes = AttemptHistory.Find(ErrorSignature);
	return Fixes ? Fixes->Num() : 0;
}

bool FOliveLoopDetector::IsLooping(const FString& ErrorSignature, const FOliveRetryPolicy& Policy) const
{
	const TArray<FString>* Fixes = AttemptHistory.Find(ErrorSignature);
	if (!Fixes)
	{
		return false;
	}

	return Fixes->Num() >= Policy.MaxRetriesPerError;
}

bool FOliveLoopDetector::IsOscillating() const
{
	if (RecentErrors.Num() < OscillationWindowSize)
	{
		return false;
	}

	// Check the last OscillationWindowSize errors for cycling patterns
	const int32 WindowStart = RecentErrors.Num() - OscillationWindowSize;

	// Count unique signatures in the window
	TSet<FString> UniqueInWindow;
	for (int32 i = WindowStart; i < RecentErrors.Num(); i++)
	{
		UniqueInWindow.Add(RecentErrors[i]);
	}

	// If we have OscillationThreshold or more different signatures cycling,
	// check if any signature appears multiple times (indicates cycling)
	if (UniqueInWindow.Num() >= 2 && UniqueInWindow.Num() <= OscillationThreshold)
	{
		// Check if there's a repeating pattern
		TMap<FString, int32> Counts;
		for (int32 i = WindowStart; i < RecentErrors.Num(); i++)
		{
			Counts.FindOrAdd(RecentErrors[i])++;
		}

		// Oscillation: multiple signatures each appearing 2+ times
		int32 RepeatedSignatures = 0;
		for (const auto& Pair : Counts)
		{
			if (Pair.Value >= 2)
			{
				RepeatedSignatures++;
			}
		}

		return RepeatedSignatures >= 2;
	}

	return false;
}

bool FOliveLoopDetector::IsBudgetExhausted(const FOliveRetryPolicy& Policy) const
{
	return TotalAttempts >= Policy.MaxCorrectionCyclesPerWorker;
}

void FOliveLoopDetector::Reset()
{
	AttemptHistory.Empty();
	RecentErrors.Empty();
	TotalAttempts = 0;
}

FString FOliveLoopDetector::BuildToolErrorSignature(
	const FString& ToolName,
	const FString& ErrorCode,
	const FString& AssetPath)
{
	return FString::Printf(TEXT("%s:%s:%s"), *ToolName, *ErrorCode, *AssetPath);
}

FString FOliveLoopDetector::BuildCompileErrorSignature(
	const FString& AssetPath,
	const FString& TopErrorMessage)
{
	return FString::Printf(TEXT("%s:%s"), *AssetPath, *HashString(TopErrorMessage));
}

FString FOliveLoopDetector::BuildLoopReport() const
{
	FString Report;

	Report += TEXT("## Loop Detected\n\n");
	Report += TEXT("The same errors keep recurring. Here's what was tried:\n\n");

	// List what was tried per error
	for (const auto& Pair : AttemptHistory)
	{
		Report += FString::Printf(TEXT("**Error:** `%s`\n"), *Pair.Key);
		Report += FString::Printf(TEXT("  Attempts: %d\n"), Pair.Value.Num());

		// Show last attempted fix
		if (Pair.Value.Num() > 0)
		{
			Report += FString::Printf(TEXT("  Last fix tried: %s\n"), *Pair.Value.Last());
		}
		Report += TEXT("\n");
	}

	if (IsOscillating())
	{
		Report += TEXT("**Pattern:** Errors are oscillating between different issues.\n\n");
	}

	// Get suggestions based on the most recent error
	if (RecentErrors.Num() > 0)
	{
		TArray<FString> Suggestions = GetSuggestionsForError(RecentErrors.Last());
		if (Suggestions.Num() > 0)
		{
			Report += TEXT("**Suggested next steps:**\n");
			for (const FString& Suggestion : Suggestions)
			{
				Report += FString::Printf(TEXT("- %s\n"), *Suggestion);
			}
		}
	}

	return Report;
}

TArray<FString> FOliveLoopDetector::GetSuggestionsForError(const FString& ErrorSignature)
{
	TArray<FString> Suggestions;

	// Parse the error signature to determine suggestions
	if (ErrorSignature.Contains(TEXT("COMPILE")))
	{
		Suggestions.Add(TEXT("Read the Blueprint with `blueprint.read` to check current state"));
		Suggestions.Add(TEXT("Check if the variable/function types are correct"));
		Suggestions.Add(TEXT("Try removing the failing nodes and re-adding them"));
	}
	else if (ErrorSignature.Contains(TEXT("NOT_FOUND")))
	{
		Suggestions.Add(TEXT("Use `project.search` to verify the asset exists"));
		Suggestions.Add(TEXT("Check the asset path for typos"));
		Suggestions.Add(TEXT("The asset may need to be created first"));
	}
	else if (ErrorSignature.Contains(TEXT("VALIDATION")))
	{
		Suggestions.Add(TEXT("Read the target asset to understand its current structure"));
		Suggestions.Add(TEXT("Check parameter types and names match the schema"));
		Suggestions.Add(TEXT("Verify naming conventions are followed"));
	}
	else if (ErrorSignature.Contains(TEXT("PIN")) || ErrorSignature.Contains(TEXT("connect")))
	{
		Suggestions.Add(TEXT("Read the function graph to check available pins"));
		Suggestions.Add(TEXT("Verify pin names and types are compatible"));
		Suggestions.Add(TEXT("Check if a type conversion node is needed"));
	}
	else
	{
		Suggestions.Add(TEXT("Read the affected asset to understand current state"));
		Suggestions.Add(TEXT("Try a different approach to achieve the same goal"));
		Suggestions.Add(TEXT("Ask the user for guidance on how to proceed"));
	}

	return Suggestions;
}

FString FOliveLoopDetector::HashString(const FString& Input)
{
	const uint32 CRC = FCrc::StrCrc32(*Input);
	return FString::Printf(TEXT("%08X"), CRC);
}
