// Copyright Bode Software. All Rights Reserved.

/**
 * OliveProviderRetryManager.cpp
 *
 * Implementation of the provider retry manager. Handles error classification,
 * exponential backoff scheduling, rate-limit Retry-After, and countdown ticking.
 *
 * All timer operations use GEditor->GetTimerManager() which runs on the game thread.
 */

#include "Providers/OliveProviderRetryManager.h"
#include "Providers/IOliveAIProvider.h"
#include "OliveAIEditorModule.h"
#include "Editor.h"
#include "TimerManager.h"

// ==========================================
// FOliveProviderErrorInfo Factory Methods
// ==========================================

FOliveProviderErrorInfo FOliveProviderErrorInfo::Terminal(const FString& Msg, int32 StatusCode)
{
	FOliveProviderErrorInfo Info;
	Info.Message = Msg;
	Info.ErrorClass = EOliveProviderErrorClass::Terminal;
	Info.HttpStatusCode = StatusCode;
	Info.RetryAfterSeconds = -1;
	return Info;
}

FOliveProviderErrorInfo FOliveProviderErrorInfo::Transient(const FString& Msg, int32 StatusCode)
{
	FOliveProviderErrorInfo Info;
	Info.Message = Msg;
	Info.ErrorClass = EOliveProviderErrorClass::Transient;
	Info.HttpStatusCode = StatusCode;
	Info.RetryAfterSeconds = -1;
	return Info;
}

FOliveProviderErrorInfo FOliveProviderErrorInfo::RateLimited(const FString& Msg, int32 RetryAfter, int32 StatusCode)
{
	FOliveProviderErrorInfo Info;
	Info.Message = Msg;
	Info.ErrorClass = EOliveProviderErrorClass::RateLimited;
	Info.HttpStatusCode = StatusCode;
	Info.RetryAfterSeconds = RetryAfter;
	return Info;
}

// ==========================================
// Countdown tick interval constant
// ==========================================

namespace OliveRetryConstants
{
	/** How often the countdown tick fires (seconds) */
	static constexpr float CountdownTickIntervalSeconds = 1.0f;
}

// ==========================================
// FOliveProviderRetryManager
// ==========================================

FOliveProviderRetryManager::FOliveProviderRetryManager()
	: AliveFlag(MakeShared<bool>(true))
{
}

FOliveProviderRetryManager::~FOliveProviderRetryManager()
{
	// Signal to any outstanding timer callbacks that we are dead
	if (AliveFlag.IsValid())
	{
		*AliveFlag = false;
	}

	ClearTimers();
}

void FOliveProviderRetryManager::SetProvider(TSharedPtr<IOliveAIProvider> InProvider)
{
	// Cancel any pending retry before switching providers
	if (bRetryPending || bRequestActive)
	{
		Cancel();
	}

	Provider = InProvider;
}

void FOliveProviderRetryManager::SendWithRetry(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools,
	FOnOliveStreamChunk OnChunk,
	FOnOliveToolCall OnToolCall,
	FOnOliveComplete OnComplete,
	FOnOliveError OnError,
	const FOliveRequestOptions& Options)
{
	if (!Provider.IsValid())
	{
		UE_LOG(LogOliveAI, Error, TEXT("RetryManager::SendWithRetry called with no provider set"));
		if (OnError.IsBound())
		{
			OnError.Execute(TEXT("No provider configured"));
		}
		return;
	}

	// Cancel any previous in-flight request or pending retry
	Cancel();

	// Reset retry state for this new request
	CurrentAttempt = 0;
	bRetryPending = false;
	bRateLimitRetryUsed = false;
	RetryCountdownRemaining = 0.0f;

	// Cache all request parameters for potential retry
	CachedMessages = Messages;
	CachedTools = Tools;
	CachedOptions = Options;
	CachedOnChunk = OnChunk;
	CachedOnToolCall = OnToolCall;
	CachedOnComplete = OnComplete;
	CachedOnError = OnError;

	// Send the initial attempt
	ExecuteRetry();
}

void FOliveProviderRetryManager::Cancel()
{
	ClearTimers();

	bRetryPending = false;
	RetryCountdownRemaining = 0.0f;

	// Cancel any in-flight request on the provider
	if (bRequestActive && Provider.IsValid())
	{
		Provider->CancelRequest();
	}

	bRequestActive = false;
}

float FOliveProviderRetryManager::GetRetryCountdownSeconds() const
{
	return bRetryPending ? RetryCountdownRemaining : 0.0f;
}

// ==========================================
// Error Classification
// ==========================================

FOliveProviderErrorInfo FOliveProviderRetryManager::ClassifyError(const FString& ErrorMessage, int32 HttpStatus)
{
	// -------------------------------------------------------
	// Tier 1: Parse structured prefix format
	// Format: [HTTP:{StatusCode}:RetryAfter={Seconds}] {Message}
	// or:     [HTTP:{StatusCode}] {Message}
	// -------------------------------------------------------
	if (ErrorMessage.StartsWith(TEXT("[HTTP:")))
	{
		int32 CloseBracket = INDEX_NONE;
		ErrorMessage.FindChar(TEXT(']'), CloseBracket);

		if (CloseBracket != INDEX_NONE)
		{
			// Extract the bracket contents: "HTTP:{StatusCode}:RetryAfter={Seconds}"
			FString BracketContents = ErrorMessage.Mid(1, CloseBracket - 1);
			FString RemainingMessage = ErrorMessage.Mid(CloseBracket + 1).TrimStart();

			// Parse status code
			int32 ParsedStatusCode = 0;
			int32 ParsedRetryAfter = -1;

			// Split by ':'
			TArray<FString> Parts;
			BracketContents.ParseIntoArray(Parts, TEXT(":"));

			// Parts[0] = "HTTP", Parts[1] = "{StatusCode}", Parts[2] = "RetryAfter={Seconds}" (optional)
			if (Parts.Num() >= 2)
			{
				ParsedStatusCode = FCString::Atoi(*Parts[1]);
			}

			for (int32 i = 2; i < Parts.Num(); ++i)
			{
				if (Parts[i].StartsWith(TEXT("RetryAfter=")))
				{
					FString RetryAfterStr = Parts[i].Mid(11); // len("RetryAfter=") = 11
					ParsedRetryAfter = FCString::Atoi(*RetryAfterStr);
				}
			}

			// Classify based on parsed status code
			if (ParsedStatusCode == 429 || ParsedRetryAfter >= 0)
			{
				// Default to 60s if RetryAfter header was missing from a 429
				int32 EffectiveRetryAfter = (ParsedRetryAfter >= 0) ? ParsedRetryAfter : 60;
				return FOliveProviderErrorInfo::RateLimited(RemainingMessage, EffectiveRetryAfter, ParsedStatusCode);
			}
			else if (ParsedStatusCode >= 500 && ParsedStatusCode < 600)
			{
				return FOliveProviderErrorInfo::Transient(RemainingMessage, ParsedStatusCode);
			}
			else if (ParsedStatusCode == 408) // Request Timeout
			{
				return FOliveProviderErrorInfo::Transient(RemainingMessage, ParsedStatusCode);
			}
			else if (ParsedStatusCode == 0)
			{
				// Status code 0 = connection failure / timeout / no HTTP response
				return FOliveProviderErrorInfo::Transient(RemainingMessage, 0);
			}
			else
			{
				// 4xx (other than 429/408) or unknown -- terminal
				return FOliveProviderErrorInfo::Terminal(RemainingMessage, ParsedStatusCode);
			}
		}
	}

	// -------------------------------------------------------
	// Tier 2: Heuristic string matching (case-insensitive)
	// -------------------------------------------------------
	FString LowerMessage = ErrorMessage.ToLower();

	// Rate limited
	if (LowerMessage.Contains(TEXT("rate limit")) || LowerMessage.Contains(TEXT("429")))
	{
		// Try to extract a retry-after value from the message
		int32 RetryAfter = 60; // Default

		// Look for patterns like "retry after 30" or "wait 45 seconds"
		int32 RetryIdx = LowerMessage.Find(TEXT("retry after"));
		if (RetryIdx != INDEX_NONE)
		{
			FString After = ErrorMessage.Mid(RetryIdx + 11).TrimStart();
			int32 Parsed = FCString::Atoi(*After);
			if (Parsed > 0)
			{
				RetryAfter = Parsed;
			}
		}
		else
		{
			int32 WaitIdx = LowerMessage.Find(TEXT("wait"));
			if (WaitIdx != INDEX_NONE)
			{
				FString After = ErrorMessage.Mid(WaitIdx + 4).TrimStart();
				int32 Parsed = FCString::Atoi(*After);
				if (Parsed > 0)
				{
					RetryAfter = Parsed;
				}
			}
		}

		return FOliveProviderErrorInfo::RateLimited(ErrorMessage, RetryAfter, 429);
	}

	// Transient network errors
	if (LowerMessage.Contains(TEXT("timeout")) ||
		LowerMessage.Contains(TEXT("timed out")) ||
		LowerMessage.Contains(TEXT("connection refused")) ||
		LowerMessage.Contains(TEXT("network error")) ||
		LowerMessage.Contains(TEXT("connection reset")) ||
		LowerMessage.Contains(TEXT("connection closed")) ||
		LowerMessage.Contains(TEXT("econnrefused")) ||
		LowerMessage.Contains(TEXT("econnreset")) ||
		LowerMessage.Contains(TEXT("etimedout")))
	{
		return FOliveProviderErrorInfo::Transient(ErrorMessage, 0);
	}

	// 5xx server errors (heuristic: look for 3-digit codes 500-599)
	if (LowerMessage.Contains(TEXT("500")) ||
		LowerMessage.Contains(TEXT("502")) ||
		LowerMessage.Contains(TEXT("503")) ||
		LowerMessage.Contains(TEXT("504")) ||
		LowerMessage.Contains(TEXT("internal server error")) ||
		LowerMessage.Contains(TEXT("bad gateway")) ||
		LowerMessage.Contains(TEXT("service unavailable")) ||
		LowerMessage.Contains(TEXT("gateway timeout")))
	{
		return FOliveProviderErrorInfo::Transient(ErrorMessage, HttpStatus > 0 ? HttpStatus : 500);
	}

	// Process crash / abnormal exit (e.g. command-line overflow, segfault)
	if (LowerMessage.Contains(TEXT("process exited with code")) ||
		LowerMessage.Contains(TEXT("process crashed")) ||
		LowerMessage.Contains(TEXT("process terminated")))
	{
		return FOliveProviderErrorInfo::Transient(ErrorMessage, 0);
	}

	// Use explicit HttpStatus hint if provided
	if (HttpStatus >= 500 && HttpStatus < 600)
	{
		return FOliveProviderErrorInfo::Transient(ErrorMessage, HttpStatus);
	}
	if (HttpStatus == 429)
	{
		return FOliveProviderErrorInfo::RateLimited(ErrorMessage, 60, 429);
	}
	if (HttpStatus == 408)
	{
		return FOliveProviderErrorInfo::Transient(ErrorMessage, 408);
	}

	// Terminal auth / client errors
	if (LowerMessage.Contains(TEXT("invalid api key")) ||
		LowerMessage.Contains(TEXT("401")) ||
		LowerMessage.Contains(TEXT("403")) ||
		LowerMessage.Contains(TEXT("unauthorized")) ||
		LowerMessage.Contains(TEXT("forbidden")) ||
		LowerMessage.Contains(TEXT("invalid request")) ||
		LowerMessage.Contains(TEXT("400")) ||
		LowerMessage.Contains(TEXT("not found")) ||
		LowerMessage.Contains(TEXT("404")))
	{
		return FOliveProviderErrorInfo::Terminal(ErrorMessage, HttpStatus > 0 ? HttpStatus : 0);
	}

	// Default: terminal (safe default -- do not retry unknown errors)
	return FOliveProviderErrorInfo::Terminal(ErrorMessage, HttpStatus);
}

// ==========================================
// Retry Scheduling
// ==========================================

void FOliveProviderRetryManager::ScheduleRetry(float DelaySeconds)
{
	bRetryPending = true;
	RetryCountdownRemaining = DelaySeconds;

	// Capture alive flag for safe lambda callbacks
	TWeakPtr<bool> WeakAlive = AliveFlag;

	// Schedule the actual retry
	if (GEditor)
	{
		GEditor->GetTimerManager()->SetTimer(
			RetryTimerHandle,
			FTimerDelegate::CreateLambda([this, WeakAlive]()
			{
				TSharedPtr<bool> Alive = WeakAlive.Pin();
				if (!Alive.IsValid() || !(*Alive))
				{
					return;
				}
				ExecuteRetry();
			}),
			DelaySeconds,
			false /* bLoop */
		);

		// Schedule countdown tick every ~1s
		GEditor->GetTimerManager()->SetTimer(
			CountdownTickHandle,
			FTimerDelegate::CreateLambda([this, WeakAlive]()
			{
				TSharedPtr<bool> Alive = WeakAlive.Pin();
				if (!Alive.IsValid() || !(*Alive))
				{
					return;
				}
				TickCountdown();
			}),
			OliveRetryConstants::CountdownTickIntervalSeconds,
			true /* bLoop */
		);
	}
	else
	{
		// Fallback: if no GEditor (shouldn't happen in editor), execute immediately
		UE_LOG(LogOliveAI, Warning, TEXT("RetryManager: GEditor not available for timer scheduling, retrying immediately"));
		ExecuteRetry();
	}
}

void FOliveProviderRetryManager::ExecuteRetry()
{
	// Clear countdown state
	ClearTimers();
	bRetryPending = false;
	RetryCountdownRemaining = 0.0f;

	if (!Provider.IsValid())
	{
		UE_LOG(LogOliveAI, Error, TEXT("RetryManager::ExecuteRetry: Provider is no longer valid"));
		if (CachedOnError.IsBound())
		{
			CachedOnError.Execute(TEXT("Provider disconnected during retry"));
		}
		return;
	}

	// Log the attempt
	if (CurrentAttempt > 0)
	{
		UE_LOG(LogOliveAI, Log, TEXT("RetryManager: Executing retry attempt %d/%d"), CurrentAttempt, MaxRetries);
		OnRetryAttemptStarted.Broadcast();
	}

	bRequestActive = true;

	// Capture alive flag for the error callback lambda
	TWeakPtr<bool> WeakAlive = AliveFlag;

	// Build the error callback that routes through our retry logic
	FOnOliveError RetryAwareOnError;
	RetryAwareOnError.BindLambda([this, WeakAlive](const FString& ErrorMessage)
	{
		TSharedPtr<bool> Alive = WeakAlive.Pin();
		if (!Alive.IsValid() || !(*Alive))
		{
			return;
		}
		HandleProviderError(ErrorMessage);
	});

	// Build a completion callback that clears our active flag then forwards
	FOnOliveComplete RetryAwareOnComplete;
	RetryAwareOnComplete.BindLambda([this, WeakAlive](const FString& FullResponse, const FOliveProviderUsage& Usage)
	{
		TSharedPtr<bool> Alive = WeakAlive.Pin();
		if (!Alive.IsValid() || !(*Alive))
		{
			return;
		}

		bRequestActive = false;
		CurrentAttempt = 0;
		bRateLimitRetryUsed = false;

		if (CachedOnComplete.IsBound())
		{
			CachedOnComplete.Execute(FullResponse, Usage);
		}
	});

	// Send to the provider -- OnChunk and OnToolCall are forwarded directly
	Provider->SendMessage(
		CachedMessages,
		CachedTools,
		CachedOnChunk,
		CachedOnToolCall,
		RetryAwareOnComplete,
		RetryAwareOnError,
		CachedOptions
	);
}

void FOliveProviderRetryManager::HandleProviderError(const FString& ErrorMessage)
{
	bRequestActive = false;

	FOliveProviderErrorInfo ErrorInfo = ClassifyError(ErrorMessage);

	switch (ErrorInfo.ErrorClass)
	{
	case EOliveProviderErrorClass::Transient:
	{
		CurrentAttempt++;
		if (CurrentAttempt <= MaxRetries)
		{
			// Exponential backoff: BaseBackoff * 2^(attempt-1)
			float DelaySeconds = BaseBackoffSeconds * FMath::Pow(2.0f, static_cast<float>(CurrentAttempt - 1));

			UE_LOG(LogOliveAI, Warning, TEXT("RetryManager: Transient error (attempt %d/%d), retrying in %.1fs: %s"),
				CurrentAttempt, MaxRetries, DelaySeconds, *ErrorInfo.Message);

			OnRetryScheduled.Broadcast(CurrentAttempt, MaxRetries, DelaySeconds);
			ScheduleRetry(DelaySeconds);
		}
		else
		{
			// Exhausted retries
			FString FinalError = FString::Printf(
				TEXT("Failed after %d attempts: %s. Check your network connection."),
				MaxRetries, *ErrorInfo.Message);

			UE_LOG(LogOliveAI, Error, TEXT("RetryManager: %s"), *FinalError);

			if (CachedOnError.IsBound())
			{
				CachedOnError.Execute(FinalError);
			}
		}
		break;
	}

	case EOliveProviderErrorClass::RateLimited:
	{
		int32 RetryAfter = ErrorInfo.RetryAfterSeconds > 0 ? ErrorInfo.RetryAfterSeconds : 60;
		if (bRateLimitRetryUsed)
		{
			const FString FinalError = FString::Printf(
				TEXT("Rate limit retry exhausted after waiting %d seconds. Please retry manually."),
				RetryAfter);

			UE_LOG(LogOliveAI, Error, TEXT("RetryManager: %s"), *FinalError);
			if (CachedOnError.IsBound())
			{
				CachedOnError.Execute(FinalError);
			}
			break;
		}

		if (RetryAfter > MaxRetryAfterSeconds)
		{
			// Too long to wait -- treat as terminal
			FString FinalError = FString::Printf(
				TEXT("Rate limited for too long (%d seconds). Please wait and retry manually."),
				RetryAfter);

			UE_LOG(LogOliveAI, Error, TEXT("RetryManager: %s"), *FinalError);

			if (CachedOnError.IsBound())
			{
				CachedOnError.Execute(FinalError);
			}
		}
		else
		{
			// Exactly one rate-limit retry is allowed per request.
			bRateLimitRetryUsed = true;
			CurrentAttempt++;
			float DelaySeconds = static_cast<float>(RetryAfter);

			UE_LOG(LogOliveAI, Warning, TEXT("RetryManager: Rate limited, retrying in %ds: %s"),
				RetryAfter, *ErrorInfo.Message);

			OnRetryScheduled.Broadcast(CurrentAttempt, CurrentAttempt, DelaySeconds);
			ScheduleRetry(DelaySeconds);
		}
		break;
	}

	case EOliveProviderErrorClass::Terminal:
	case EOliveProviderErrorClass::Truncated:
	default:
	{
		// No retry -- propagate immediately
		UE_LOG(LogOliveAI, Error, TEXT("RetryManager: Terminal error: %s"), *ErrorInfo.Message);

		if (CachedOnError.IsBound())
		{
			CachedOnError.Execute(ErrorMessage);
		}
		break;
	}
	}
}

// ==========================================
// Timer Management
// ==========================================

void FOliveProviderRetryManager::TickCountdown()
{
	RetryCountdownRemaining -= OliveRetryConstants::CountdownTickIntervalSeconds;

	if (RetryCountdownRemaining < 0.0f)
	{
		RetryCountdownRemaining = 0.0f;
	}

	OnRetryCountdownTick.Broadcast(RetryCountdownRemaining);
}

void FOliveProviderRetryManager::ClearTimers()
{
	if (GEditor)
	{
		if (RetryTimerHandle.IsValid())
		{
			GEditor->GetTimerManager()->ClearTimer(RetryTimerHandle);
			RetryTimerHandle.Invalidate();
		}
		if (CountdownTickHandle.IsValid())
		{
			GEditor->GetTimerManager()->ClearTimer(CountdownTickHandle);
			CountdownTickHandle.Invalidate();
		}
	}
}
