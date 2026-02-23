// Copyright Bode Software. All Rights Reserved.

/**
 * OliveProviderRetryManager.h
 *
 * Wraps an IOliveAIProvider with automatic retry logic for transient errors
 * and rate-limit handling. Does NOT implement IOliveAIProvider -- the
 * ConversationManager calls SendWithRetry() instead of Provider->SendMessage().
 *
 * Retry policy:
 * - Transient errors: exponential backoff (1s, 2s, 4s) up to MaxRetries (3).
 * - Rate-limited (429): wait Retry-After seconds, then retry once. If
 *   Retry-After > MaxRetryAfterSeconds (120), treat as terminal.
 * - Terminal errors: no retry, immediate error propagation.
 *
 * Thread expectations: All public methods must be called on the game thread.
 * Timer callbacks are dispatched on the game thread via FTimerManager.
 */

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"

// ==========================================
// Error Classification
// ==========================================

/**
 * Classification of provider errors for retry decisions.
 */
enum class EOliveProviderErrorClass : uint8
{
	/** Non-retryable: auth failure, invalid request, etc. */
	Terminal,

	/** Retryable: network timeout, 5xx, connection refused */
	Transient,

	/** Rate limited: has Retry-After semantics */
	RateLimited,

	/** Response truncated by max_tokens (finish_reason = "length") */
	Truncated
};

/**
 * Structured error information from a provider response.
 * Replaces the plain FString error used elsewhere.
 */
struct OLIVEAIEDITOR_API FOliveProviderErrorInfo
{
	/** Human-readable error message */
	FString Message;

	/** Error classification */
	EOliveProviderErrorClass ErrorClass = EOliveProviderErrorClass::Terminal;

	/** HTTP status code (0 if not an HTTP error) */
	int32 HttpStatusCode = 0;

	/** Retry-After value in seconds (for rate-limited errors). -1 if not applicable. */
	int32 RetryAfterSeconds = -1;

	/**
	 * Create a terminal (non-retryable) error.
	 * @param Msg Human-readable error message
	 * @param StatusCode HTTP status code (0 if not HTTP-related)
	 * @return Configured error info
	 */
	static FOliveProviderErrorInfo Terminal(const FString& Msg, int32 StatusCode = 0);

	/**
	 * Create a transient (retryable) error.
	 * @param Msg Human-readable error message
	 * @param StatusCode HTTP status code (0 if not HTTP-related)
	 * @return Configured error info
	 */
	static FOliveProviderErrorInfo Transient(const FString& Msg, int32 StatusCode = 0);

	/**
	 * Create a rate-limited error with Retry-After semantics.
	 * @param Msg Human-readable error message
	 * @param RetryAfter Seconds to wait before retrying
	 * @param StatusCode HTTP status code (default 429)
	 * @return Configured error info
	 */
	static FOliveProviderErrorInfo RateLimited(const FString& Msg, int32 RetryAfter, int32 StatusCode = 429);
};

// ==========================================
// Retry Event Delegates
// ==========================================

/** Fired when a retry is scheduled (carries attempt info and delay) */
DECLARE_MULTICAST_DELEGATE_ThreeParams(
	FOnOliveRetryScheduled,
	int32 /* AttemptNumber (1-based) */,
	int32 /* MaxAttempts */,
	float /* DelaySeconds */);

/** Fired every ~1s during retry countdown (carries seconds remaining) */
DECLARE_MULTICAST_DELEGATE_OneParam(
	FOnOliveRetryCountdownTick,
	float /* SecondsRemaining */);

/** Fired when a retry attempt begins sending to the provider */
DECLARE_MULTICAST_DELEGATE(FOnOliveRetryAttemptStarted);

// ==========================================
// FOliveProviderRetryManager
// ==========================================

/**
 * Provider Retry Manager
 *
 * Wraps an IOliveAIProvider and adds automatic retry with exponential
 * backoff for transient errors, and Retry-After handling for rate limits.
 *
 * Design:
 * - Does NOT implement IOliveAIProvider (it is not a drop-in replacement).
 * - The ConversationManager calls SendWithRetry() instead of Provider->SendMessage().
 * - Retry state is fully encapsulated here; consumers do not need to know
 *   about retry internals beyond the event delegates.
 * - Uses GEditor->GetTimerManager() for countdown delays (game-thread safe).
 * - AliveFlag (TSharedPtr<bool>) pattern ensures timer callbacks are safe
 *   even if this object is destroyed while a timer is pending.
 */
class OLIVEAIEDITOR_API FOliveProviderRetryManager
{
public:
	FOliveProviderRetryManager();
	~FOliveProviderRetryManager();

	// ==========================================
	// Provider Access
	// ==========================================

	/**
	 * Set the underlying provider to wrap.
	 * @param InProvider The provider instance to delegate requests to
	 */
	void SetProvider(TSharedPtr<IOliveAIProvider> InProvider);

	/**
	 * Get the underlying provider.
	 * @return The wrapped provider, or nullptr if not set
	 */
	TSharedPtr<IOliveAIProvider> GetProvider() const { return Provider; }

	// ==========================================
	// Request API
	// ==========================================

	/**
	 * Send a message with automatic retry on transient failures.
	 * Signature mirrors IOliveAIProvider::SendMessage() but the OnError
	 * callback is only called for terminal or retry-exhausted errors.
	 *
	 * Calling this while a previous request is in-flight cancels the previous request.
	 *
	 * @param Messages Conversation history
	 * @param Tools Available tools for the model to call
	 * @param OnChunk Called for each text chunk during streaming
	 * @param OnToolCall Called when a tool call is detected in the stream
	 * @param OnComplete Called when the response is complete
	 * @param OnError Called on terminal error or after all retries exhausted
	 * @param Options Per-request options overriding provider defaults
	 */
	void SendWithRetry(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools,
		FOnOliveStreamChunk OnChunk,
		FOnOliveToolCall OnToolCall,
		FOnOliveComplete OnComplete,
		FOnOliveError OnError,
		const FOliveRequestOptions& Options = FOliveRequestOptions()
	);

	/**
	 * Cancel any in-flight request or scheduled retry.
	 * Safe to call even if nothing is pending.
	 */
	void Cancel();

	// ==========================================
	// State Queries
	// ==========================================

	/**
	 * Check if a retry is scheduled (waiting for backoff timer).
	 * @return true if waiting for a retry timer to fire
	 */
	bool IsRetryPending() const { return bRetryPending; }

	/**
	 * Get seconds remaining until next retry attempt.
	 * @return Countdown seconds, or 0.0 if no retry is pending
	 */
	float GetRetryCountdownSeconds() const;

	// ==========================================
	// Configuration
	// ==========================================

	/** Maximum retry attempts for transient errors (default 3) */
	int32 MaxRetries = 3;

	/** Base backoff delay in seconds (doubles each attempt: 1s, 2s, 4s) */
	float BaseBackoffSeconds = 1.0f;

	/** Maximum Retry-After seconds to honor (default 120). Beyond this, treat as terminal. */
	int32 MaxRetryAfterSeconds = 120;

	// ==========================================
	// Events
	// ==========================================

	/** Fired when a retry is scheduled (attempt number, max attempts, delay) */
	FOnOliveRetryScheduled OnRetryScheduled;

	/** Fired every ~1s during retry countdown (seconds remaining) */
	FOnOliveRetryCountdownTick OnRetryCountdownTick;

	/** Fired when a retry attempt begins sending to the provider */
	FOnOliveRetryAttemptStarted OnRetryAttemptStarted;

	// ==========================================
	// Error Classification (public for testing)
	// ==========================================

	/**
	 * Classify an error string from the provider into a structured error info.
	 *
	 * Uses a two-tier approach:
	 * 1. Parse structured prefix: [HTTP:{StatusCode}:RetryAfter={Seconds}] {Message}
	 * 2. Heuristic string matching fallback for unstructured error messages
	 *
	 * @param ErrorMessage The error string from the provider
	 * @param HttpStatus Optional HTTP status code hint (0 if unknown)
	 * @return Classified error information
	 */
	FOliveProviderErrorInfo ClassifyError(const FString& ErrorMessage, int32 HttpStatus = 0);

private:
	/**
	 * Schedule a retry after the given delay.
	 * Sets up FTimerHandle for the retry and a tick timer for countdown events.
	 * @param DelaySeconds Seconds to wait before retrying
	 */
	void ScheduleRetry(float DelaySeconds);

	/**
	 * Execute a retry attempt by re-sending the cached request to the provider.
	 * Called when the retry timer fires.
	 */
	void ExecuteRetry();

	/**
	 * Tick the countdown timer. Called every ~1s while a retry is pending.
	 * Broadcasts OnRetryCountdownTick with remaining seconds.
	 */
	void TickCountdown();

	/**
	 * Clear all active timer handles.
	 * Called during Cancel() and destructor.
	 */
	void ClearTimers();

	/**
	 * Internal error handler bound to the provider's OnError callback.
	 * Classifies the error and decides whether to retry or propagate.
	 * @param ErrorMessage The error string from the provider
	 */
	void HandleProviderError(const FString& ErrorMessage);

	/** The wrapped provider */
	TSharedPtr<IOliveAIProvider> Provider;

	// ==========================================
	// Retry State
	// ==========================================

	/** Current retry attempt (0 = initial attempt, 1 = first retry, etc.) */
	int32 CurrentAttempt = 0;

	/** Whether a retry timer is currently pending */
	bool bRetryPending = false;

	/** Whether this request has already consumed its single rate-limit retry */
	bool bRateLimitRetryUsed = false;

	/** Countdown seconds remaining until next retry */
	float RetryCountdownRemaining = 0.0f;

	/** Whether a request is currently active (sent to provider, awaiting response) */
	bool bRequestActive = false;

	// ==========================================
	// Cached Request Parameters (for retry)
	// ==========================================

	TArray<FOliveChatMessage> CachedMessages;
	TArray<FOliveToolDefinition> CachedTools;
	FOliveRequestOptions CachedOptions;

	/** Cached caller callbacks (forwarded on success/terminal failure) */
	FOnOliveStreamChunk CachedOnChunk;
	FOnOliveToolCall CachedOnToolCall;
	FOnOliveComplete CachedOnComplete;
	FOnOliveError CachedOnError;

	// ==========================================
	// Timer Handles
	// ==========================================

	FTimerHandle RetryTimerHandle;
	FTimerHandle CountdownTickHandle;

	/**
	 * Alive flag for safe async callbacks.
	 * Shared with timer lambdas. Set to false in destructor so pending
	 * timer callbacks become no-ops instead of accessing freed memory.
	 */
	TSharedPtr<bool> AliveFlag;
};
