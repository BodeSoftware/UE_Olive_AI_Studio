# Phase 3: Chat UX Resilience - Design Document

**Author:** Architect Agent
**Date:** 2026-02-22
**Status:** Draft
**Scope:** Keep chat usable and predictable under contention, network faults, and long operations.

---

## 1. Problem Summary

The current chat system has several fragility points:

1. **Lost input**: `SendUserMessage()` silently rejects messages when `bIsProcessing` is true (line 165-169 of OliveConversationManager.cpp). The user gets a log warning but no UI feedback and the message is discarded.

2. **No retry on network failure**: Provider errors (timeouts, 5xx, rate limits) surface as a terminal error message. The user must manually re-send. Rate-limit 429 responses with `Retry-After` headers are parsed but only shown as text.

3. **Panel destruction kills operations**: `SOliveAIChatPanel::~SOliveAIChatPanel()` destroys the `ConversationManager` TSharedPtr. If the panel is the sole owner, the destructor calls `CancelCurrentRequest()`, aborting in-flight work.

4. **No truncation visibility**: `FOlivePromptDistiller::Distill()` silently rewrites older tool results. The model has no way to know context was trimmed.

5. **Focus profile switch during operation**: `OnFocusProfileSelected()` calls `ConversationManager->SetFocusProfile()` immediately, which changes the tool set mid-operation, causing inconsistent tool availability.

6. **No response truncation detection**: If a provider returns a response cut short by `max_tokens` (finish_reason = "length"), the user sees truncated text with no warning.

---

## 2. Architecture Overview

Six new/modified subsystems, designed as **additive changes** with minimal modification to existing files.

```
User Input
    |
    v
[Message Queue]  <-- NEW: FOliveMessageQueue
    |
    v
[Conversation Manager]  (modified: dequeue instead of direct send)
    |
    v
[Provider Retry Manager]  <-- NEW: FOliveProviderRetryManager (wraps provider)
    |                               ^
    v                               |
[Provider]  --error(retryable)--->--+
    |
    v (on success / terminal error)
[Conversation Manager callbacks]
    |
    v
[Chat Panel UI]  (modified: queue indicator, retry countdown, truncation warnings)
```

### Lifetime Decoupling

```
SOliveAIChatPanel
    |
    | weak ref (TWeakPtr)
    v
FOliveConversationManager  <-- owned by FOliveEditorChatSession (singleton)
    |
    | owns
    v
FOliveProviderRetryManager
    |
    | wraps
    v
IOliveAIProvider
```

The `FOliveEditorChatSession` singleton keeps the ConversationManager alive across panel open/close cycles. The panel only holds a `TWeakPtr` and rebinds delegates on construction.

---

## 3. New Files

| File | Purpose |
|------|---------|
| `Source/OliveAIEditor/Public/Chat/OliveMessageQueue.h` | Queues user messages while processing |
| `Source/OliveAIEditor/Private/Chat/OliveMessageQueue.cpp` | Implementation |
| `Source/OliveAIEditor/Public/Chat/OliveEditorChatSession.h` | Singleton session that owns ConversationManager |
| `Source/OliveAIEditor/Private/Chat/OliveEditorChatSession.cpp` | Implementation |
| `Source/OliveAIEditor/Public/Providers/OliveProviderRetryManager.h` | Retry/backoff wrapper around IOliveAIProvider |
| `Source/OliveAIEditor/Private/Providers/OliveProviderRetryManager.cpp` | Implementation |

## 4. Modified Files

| File | Changes |
|------|---------|
| `OliveConversationManager.h` | Add queue integration, truncation note injection, deferred profile switch |
| `OliveConversationManager.cpp` | Modify `SendUserMessage()`, `HandleError()`, `HandleComplete()`, add new methods |
| `SOliveAIChatPanel.h` | Change ConversationManager ownership to TWeakPtr, add queue/retry UI state |
| `SOliveAIChatPanel.cpp` | Bind to session singleton, queue indicator in status bar, retry countdown, truncation badge |
| `IOliveAIProvider.h` | Add `FOliveProviderErrorInfo` struct for structured error classification |
| `OliveOpenRouterProvider.cpp` | Return structured error info instead of plain strings (all providers similarly) |
| `OlivePromptDistiller.h/.cpp` | Return truncation metadata; inject truncation note into messages |
| `OliveAISettings.h` | Add retry configuration properties |
| `OliveBrainState.h` | Add `MessageQueued` sub-state indication (informational, not a state transition) |

---

## 5. Interface Definitions

### 5.1 FOliveMessageQueue

```cpp
// OliveMessageQueue.h

#pragma once

#include "CoreMinimal.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FOnOliveMessageQueued, int32 /* QueueDepth */);
DECLARE_MULTICAST_DELEGATE(FOnOliveQueueDrained);

/**
 * Queues user messages while the conversation manager is processing.
 * Messages are dequeued FIFO when processing completes.
 *
 * Invariants:
 * - Queue depth is bounded by MaxQueuedMessages (default 5).
 * - Oldest messages are dropped if queue overflows (with warning).
 * - Queue is drained one message at a time (each triggers a full
 *   agentic loop cycle before the next is dequeued).
 */
class OLIVEAIEDITOR_API FOliveMessageQueue
{
public:
    FOliveMessageQueue();

    /** Enqueue a user message. Returns false if queue is full (oldest dropped). */
    bool Enqueue(const FString& Message);

    /** Dequeue the next message. Returns empty string if queue is empty. */
    FString Dequeue();

    /** Check if queue has pending messages */
    bool HasPending() const;

    /** Get number of queued messages */
    int32 GetQueueDepth() const;

    /** Clear all queued messages */
    void Clear();

    /** Fired when a message is enqueued (carries new queue depth) */
    FOnOliveMessageQueued OnMessageQueued;

    /** Fired when queue becomes empty after draining */
    FOnOliveQueueDrained OnQueueDrained;

    /** Maximum number of queued messages before oldest is dropped */
    int32 MaxQueuedMessages = 5;

private:
    TArray<FString> Queue;
};
```

### 5.2 FOliveProviderRetryManager

```cpp
// OliveProviderRetryManager.h

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"

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
 * Replaces the plain FString error currently used.
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

    /** Convenience factory methods */
    static FOliveProviderErrorInfo Terminal(const FString& Msg, int32 StatusCode = 0);
    static FOliveProviderErrorInfo Transient(const FString& Msg, int32 StatusCode = 0);
    static FOliveProviderErrorInfo RateLimited(const FString& Msg, int32 RetryAfter, int32 StatusCode = 429);
};

/**
 * Events for retry progress.
 */
DECLARE_MULTICAST_DELEGATE_ThreeParams(
    FOnOliveRetryScheduled,
    int32 /* AttemptNumber (1-based) */,
    int32 /* MaxAttempts */,
    float /* DelaySeconds */);

DECLARE_MULTICAST_DELEGATE_OneParam(
    FOnOliveRetryCountdownTick,
    float /* SecondsRemaining */);

DECLARE_MULTICAST_DELEGATE(FOnOliveRetryAttemptStarted);

/**
 * Provider Retry Manager
 *
 * Wraps an IOliveAIProvider and adds automatic retry with exponential
 * backoff for transient errors, and Retry-After handling for rate limits.
 *
 * Design:
 * - Does NOT implement IOliveAIProvider (it is not a drop-in replacement).
 * - The Conversation Manager calls SendWithRetry() instead of Provider->SendMessage().
 * - Retry state is fully encapsulated here; the ConversationManager does not
 *   need to know about retry internals.
 * - Uses FTimerManager for countdown delays (game-thread safe).
 *
 * Retry policy:
 * - Transient errors: exponential backoff (1s, 2s, 4s) up to MaxRetries (3).
 * - Rate-limited (429): wait Retry-After seconds, then retry once. If
 *   Retry-After > MaxRetryAfterSeconds (120), treat as terminal.
 * - Terminal errors: no retry, immediate error propagation.
 */
class OLIVEAIEDITOR_API FOliveProviderRetryManager
{
public:
    FOliveProviderRetryManager();

    /** Set the underlying provider to wrap */
    void SetProvider(TSharedPtr<IOliveAIProvider> InProvider);

    /** Get the underlying provider */
    TSharedPtr<IOliveAIProvider> GetProvider() const { return Provider; }

    /**
     * Send a message with automatic retry on transient failures.
     * Signature mirrors IOliveAIProvider::SendMessage but the OnError
     * callback is only called for terminal/exhausted errors.
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

    /** Cancel any in-flight or scheduled retry */
    void Cancel();

    /** Check if a retry is scheduled (waiting for backoff timer) */
    bool IsRetryPending() const { return bRetryPending; }

    /** Get seconds remaining until next retry attempt */
    float GetRetryCountdownSeconds() const;

    // ==========================================
    // Configuration
    // ==========================================

    /** Maximum retry attempts for transient errors (default 3) */
    int32 MaxRetries = 3;

    /** Base backoff delay in seconds (doubles each attempt) */
    float BaseBackoffSeconds = 1.0f;

    /** Maximum Retry-After we will honor (default 120s). Beyond this, treat as terminal. */
    int32 MaxRetryAfterSeconds = 120;

    // ==========================================
    // Events
    // ==========================================

    /** Fired when a retry is scheduled */
    FOnOliveRetryScheduled OnRetryScheduled;

    /** Fired every ~1s during retry countdown */
    FOnOliveRetryCountdownTick OnRetryCountdownTick;

    /** Fired when a retry attempt begins */
    FOnOliveRetryAttemptStarted OnRetryAttemptStarted;

private:
    /** Classify an error string from the provider into a structured error info */
    FOliveProviderErrorInfo ClassifyError(const FString& ErrorMessage, int32 HttpStatus = 0);

    /** Schedule a retry after the given delay */
    void ScheduleRetry(float DelaySeconds);

    /** Execute the retry attempt */
    void ExecuteRetry();

    /** Tick the countdown (called by timer) */
    void TickCountdown();

    /** The wrapped provider */
    TSharedPtr<IOliveAIProvider> Provider;

    /** Current retry state */
    int32 CurrentAttempt = 0;
    bool bRetryPending = false;
    float RetryCountdownRemaining = 0.0f;

    /** Cached request parameters for retry */
    TArray<FOliveChatMessage> CachedMessages;
    TArray<FOliveToolDefinition> CachedTools;
    FOliveRequestOptions CachedOptions;

    /** Cached callbacks */
    FOnOliveStreamChunk CachedOnChunk;
    FOnOliveToolCall CachedOnToolCall;
    FOnOliveComplete CachedOnComplete;
    FOnOliveError CachedOnError;

    /** Timer handles */
    FTimerHandle RetryTimerHandle;
    FTimerHandle CountdownTickHandle;

    /** Alive flag for safe async callbacks */
    TSharedPtr<bool> AliveFlag = MakeShared<bool>(true);
};
```

### 5.3 FOliveEditorChatSession

```cpp
// OliveEditorChatSession.h

#pragma once

#include "CoreMinimal.h"
#include "Chat/OliveConversationManager.h"
#include "Chat/OliveMessageQueue.h"
#include "Providers/OliveProviderRetryManager.h"

/**
 * Editor Chat Session (Singleton)
 *
 * Owns the ConversationManager, MessageQueue, and RetryManager.
 * Survives panel open/close cycles so that:
 * - In-flight operations are not cancelled when the panel is closed.
 * - Message history is preserved when the panel is reopened.
 * - Queued messages are not lost.
 *
 * Lifetime: Created on first access, destroyed when the editor module shuts down.
 * The FOliveAIEditorModule calls Shutdown() in its ShutdownModule().
 */
class OLIVEAIEDITOR_API FOliveEditorChatSession
{
public:
    static FOliveEditorChatSession& Get();

    /** Initialize the session (called once from module startup or first access) */
    void Initialize();

    /** Shutdown and release resources (called from module shutdown) */
    void Shutdown();

    /** Get the conversation manager */
    TSharedPtr<FOliveConversationManager> GetConversationManager() const { return ConversationManager; }

    /** Get the message queue */
    FOliveMessageQueue& GetMessageQueue() { return MessageQueue; }

    /** Get the retry manager */
    FOliveProviderRetryManager& GetRetryManager() { return RetryManager; }

    /** Check if an operation completed while the panel was closed */
    bool HasBackgroundCompletion() const { return bHasBackgroundCompletion; }

    /** Consume the background completion flag (resets it) */
    bool ConsumeBackgroundCompletion();

    /** Get the summary of what completed in the background */
    const FString& GetBackgroundCompletionSummary() const { return BackgroundCompletionSummary; }

    // ==========================================
    // Panel Lifecycle
    // ==========================================

    /** Notify that the chat panel has been opened (rebind UI delegates) */
    void NotifyPanelOpened();

    /** Notify that the chat panel has been closed (switch to background mode) */
    void NotifyPanelClosed();

    /** Check if the panel is currently open */
    bool IsPanelOpen() const { return bPanelOpen; }

private:
    FOliveEditorChatSession() = default;

    /** Handle processing complete while panel is closed */
    void HandleBackgroundCompletion();

    /** Show editor notification toast for background completion */
    void ShowCompletionToast(const FString& Summary);

    /** Wire the message queue drain to processing-complete */
    void WireQueueDrain();

    TSharedPtr<FOliveConversationManager> ConversationManager;
    FOliveMessageQueue MessageQueue;
    FOliveProviderRetryManager RetryManager;

    bool bInitialized = false;
    bool bPanelOpen = false;
    bool bHasBackgroundCompletion = false;
    FString BackgroundCompletionSummary;

    /** Delegate handles for listening to ConversationManager events */
    FDelegateHandle ProcessingCompleteHandle;
    FDelegateHandle ErrorHandle;
};
```

### 5.4 Truncation Detection Structs

```cpp
// Added to IOliveAIProvider.h or a shared types header

/**
 * Result metadata from prompt distillation.
 * Used to inject truncation notes into model-visible context.
 */
struct FOliveDistillationResult
{
    /** Number of messages that were summarized */
    int32 MessagesSummarized = 0;

    /** Number of tool results truncated */
    int32 ToolResultsTruncated = 0;

    /** Estimated tokens saved by distillation */
    int32 TokensSaved = 0;

    /** Whether any truncation occurred */
    bool DidTruncate() const
    {
        return MessagesSummarized > 0 || ToolResultsTruncated > 0;
    }
};
```

---

## 6. Data Flow

### 6.1 Message Queue Flow

```
User types message and hits Send
    |
    v
SOliveAIChatPanel::OnMessageSubmitted(Message)
    |
    v
Is ConversationManager processing?
    |                           |
    NO                          YES
    |                           |
    v                           v
ConversationManager         MessageQueue.Enqueue(Message)
  ->SendUserMessage()          |
                               v
                        UI: "Message queued (1 ahead)"
                        Status bar shows queue indicator
                               |
                               v
                        [later: OnProcessingComplete fires]
                               |
                               v
                        MessageQueue.HasPending()?
                               |
                               YES
                               |
                               v
                        Auto-dequeue next message
                        ConversationManager->SendUserMessage(Dequeued)
```

### 6.2 Provider Retry Flow

```
ConversationManager::SendToProvider()
    |
    v
RetryManager.SendWithRetry(messages, tools, callbacks)
    |
    v
Provider->SendMessage(messages, tools, ...)
    |
    v
Error received?
    |           |               |
    NO          Transient       RateLimited         Terminal
    |           |               |                   |
    v           v               v                   v
OnComplete  attempt < 3?    RetryAfter <= 120s?   OnError (final)
            |       |       |           |
            YES     NO      YES         NO
            |       |       |           |
            v       v       v           v
         Schedule  OnError  Schedule   OnError
         backoff   (final)  timer at   (final)
         (exp)              RetryAfter
            |               |
            v               v
         UI: "Retry 2/3    UI: "Rate limited.
          in 2s..."         Retrying in 45s..."
            |               |
            v               v
         ExecuteRetry()     ExecuteRetry()
```

### 6.3 Panel Lifetime Decoupling

```
Panel opens:
    FOliveEditorChatSession::Get() (creates if needed)
    Panel gets TWeakPtr<ConversationManager>
    Panel binds delegates via AddSP(this, ...)
    Session.NotifyPanelOpened()

Panel closes:
    ~SOliveAIChatPanel() unbinds delegates (RemoveAll)
    Session.NotifyPanelClosed()
    ConversationManager still alive (owned by Session)
    Operations continue running

If operation completes while panel closed:
    Session.HandleBackgroundCompletion()
    -> Shows FNotificationInfo toast: "Olive AI completed: [summary]"
    -> Sets bHasBackgroundCompletion = true

Panel reopens:
    Session.NotifyPanelOpened()
    Panel rebinds to existing ConversationManager
    If HasBackgroundCompletion: show banner "Operation completed while panel was closed"
    MessageHistory is still intact (owned by ConversationManager)
```

### 6.4 Context Truncation Note Injection

```
ConversationManager::SendToProvider()
    |
    v
PromptDistiller.Distill(messages, config) -> FOliveDistillationResult
    |
    v
result.DidTruncate()?
    |           |
    NO          YES
    |           |
    v           v
  (skip)     Inject truncation note as last system-adjacent message:
             "[CONTEXT NOTE: {N} older messages were summarized to save
              tokens. {M} tool results were truncated. If you need
              details from earlier in the conversation, ask the user
              to re-provide the relevant context.]"
```

### 6.5 Focus Profile Switch Guard

```
User selects new focus profile
    |
    v
ConversationManager.IsProcessing()?
    |               |
    NO              YES
    |               |
    v               v
Apply           Store as PendingFocusProfile
immediately     UI: warning banner "Profile switch deferred
                     until current operation completes"
                    |
                    v
                [OnProcessingComplete fires]
                    |
                    v
                Apply PendingFocusProfile
                Clear warning banner
```

---

## 7. Detailed Changes per File

### 7.1 FOliveConversationManager (Modified)

**Header additions:**

```cpp
// New forward declaration
class FOliveMessageQueue;
class FOliveProviderRetryManager;

// New public methods:

    /** Set the retry manager (owned externally by Session) */
    void SetRetryManager(FOliveProviderRetryManager* InRetryManager);

    /** Set the message queue (owned externally by Session) */
    void SetMessageQueue(FOliveMessageQueue* InQueue);

    /** Attempt to dequeue and send the next queued message */
    void DrainNextQueuedMessage();

    /** Set deferred focus profile (applied when processing completes) */
    void SetDeferredFocusProfile(const FString& ProfileName);

    /** Get deferred focus profile (empty if none pending) */
    const FString& GetDeferredFocusProfile() const;

// New private members:

    /** Pointer to external retry manager (not owned) */
    FOliveProviderRetryManager* RetryManager = nullptr;

    /** Pointer to external message queue (not owned) */
    FOliveMessageQueue* Queue = nullptr;

    /** Deferred focus profile change */
    FString DeferredFocusProfile;
```

**Cpp modifications:**

1. **`SendUserMessage()`**: Remove the early-return when `bIsProcessing`. Instead, if processing, call `Queue->Enqueue(Message)` and return. The user message is added to UI immediately as a "queued" message.

2. **`SendToProvider()`**: Replace `Provider->SendMessage(...)` with `RetryManager->SendWithRetry(...)` if RetryManager is set, otherwise fall through to direct provider call (backward compat).

3. **`HandleComplete()`**: After processing completes (no more tool calls), call `DrainNextQueuedMessage()` to check for queued messages.

4. **`HandleError()`**: Same -- call `DrainNextQueuedMessage()` after error to process next queued message.

5. **`SetFocusProfile()`**: If `bIsProcessing`, store in `DeferredFocusProfile` instead of applying immediately. Log a warning.

6. **`SendToProvider()` (distillation section)**: After `PromptDistiller.Distill()`, check the returned `FOliveDistillationResult`. If truncation occurred, inject a context note message.

7. **`HandleComplete()` (truncation detection)**: Check if the assistant message's `FinishReason` was "length". If so, fire a new delegate `OnResponseTruncated` so the UI can show a warning.

### 7.2 SOliveAIChatPanel (Modified)

**Key change**: The panel no longer owns the ConversationManager. It obtains it from `FOliveEditorChatSession::Get()`.

**Constructor changes:**

```cpp
void SOliveAIChatPanel::Construct(const FArguments& InArgs)
{
    // Get session singleton (creates ConversationManager if needed)
    FOliveEditorChatSession& Session = FOliveEditorChatSession::Get();
    Session.NotifyPanelOpened();

    // Weak reference to conversation manager
    ConversationManager = Session.GetConversationManager();

    // Bind delegates...
    // (same as before, but using AddSP)

    // Bind retry manager events for countdown UI
    Session.GetRetryManager().OnRetryScheduled.AddSP(this, &SOliveAIChatPanel::HandleRetryScheduled);
    Session.GetRetryManager().OnRetryCountdownTick.AddSP(this, &SOliveAIChatPanel::HandleRetryCountdownTick);

    // Bind queue events
    Session.GetMessageQueue().OnMessageQueued.AddSP(this, &SOliveAIChatPanel::HandleMessageQueued);
    Session.GetMessageQueue().OnQueueDrained.AddSP(this, &SOliveAIChatPanel::HandleQueueDrained);

    // Check for background completion
    if (Session.HasBackgroundCompletion())
    {
        // Show banner or notification
        Session.ConsumeBackgroundCompletion();
    }

    // ... rest of construction
}
```

**Destructor changes:**

```cpp
SOliveAIChatPanel::~SOliveAIChatPanel()
{
    // Unbind delegates but DO NOT destroy ConversationManager
    UnsubscribeFromEditorEvents();
    // ... RemoveAll on all delegates ...

    FOliveEditorChatSession::Get().NotifyPanelClosed();
    // ConversationManager continues to live in the Session
}
```

**New handler methods:**

```cpp
    void HandleRetryScheduled(int32 Attempt, int32 MaxAttempts, float DelaySeconds);
    void HandleRetryCountdownTick(float SecondsRemaining);
    void HandleMessageQueued(int32 QueueDepth);
    void HandleQueueDrained();
    void HandleResponseTruncated();
```

**Status bar changes:**

The `GetStatusText()` method gains new states:

- During retry: "Retry 2/3 in 3s..." (yellow)
- During rate limit wait: "Rate limited. Retrying in 42s..." (orange)
- When messages queued: "Processing... (2 messages queued)" (yellow)
- On truncated response: append "[Response truncated]" badge to last message

**`OnFocusProfileSelected()` modification:**

```cpp
void SOliveAIChatPanel::OnFocusProfileSelected(const FString& ProfileName)
{
    if (ConversationManager.IsValid() && ConversationManager->IsProcessing())
    {
        // Defer the switch
        ConversationManager->SetDeferredFocusProfile(ProfileName);
        // Show warning in status bar
        DeferredProfileWarning = FString::Printf(
            TEXT("Profile switch to '%s' deferred until operation completes"), *ProfileName);
        return;
    }

    // ... existing immediate switch logic
}
```

**`IsSendEnabled()` modification:**

Always return true when ConversationManager is valid and provider is set (queue handles the "busy" case). The button text changes to "Queue" when processing.

### 7.3 IOliveAIProvider.h (Modified)

Add the `FOliveProviderErrorInfo` struct (defined in section 5.2 above). This is additive only.

No changes to the `IOliveAIProvider` interface itself -- the retry manager wraps the existing error string pattern and classifies it. Provider implementations will be updated incrementally to return structured errors, but the retry manager can classify based on string patterns and HTTP status codes in the interim.

### 7.4 Provider Implementations (Minimal Changes)

For each provider (OpenRouter, Anthropic, OpenAI, Google, Ollama, etc.), the `OnResponseReceived()` method currently does:

```cpp
if (StatusCode == 429)
{
    HandleError(FString::Printf(TEXT("Rate limited...")));
}
```

**Change**: Embed the status code and Retry-After in the error string using a parseable prefix format that the retry manager can extract:

```cpp
// Format: "[HTTP:429:RetryAfter=60] Rate limited..."
// The retry manager parses this prefix to extract classification data.
```

This approach avoids changing the IOliveAIProvider interface (which would require updating all 8+ provider implementations). The retry manager's `ClassifyError()` method parses these prefixed strings.

**Alternative** (cleaner but more work): Add an optional `FOliveProviderErrorInfo` output parameter to the error callback. This is deferred to avoid a breaking interface change across all providers. The prefix approach is a pragmatic interim solution.

### 7.5 FOlivePromptDistiller (Modified)

**Header change**: `Distill()` returns `FOliveDistillationResult` instead of `void`.

```cpp
// Before:
void Distill(TArray<FOliveChatMessage>& Messages, const FOliveDistillationConfig& Config) const;

// After:
FOliveDistillationResult Distill(TArray<FOliveChatMessage>& Messages, const FOliveDistillationConfig& Config) const;
```

The implementation tracks how many messages were summarized and returns that metadata.

### 7.6 UOliveAISettings (Modified)

Add to "AI Provider" category:

```cpp
    /** Maximum retry attempts for transient network failures (0 = no retry) */
    UPROPERTY(Config, EditAnywhere, Category="AI Provider",
        meta=(DisplayName="Max Retries", ClampMin=0, ClampMax=5))
    int32 MaxProviderRetries = 3;

    /** Maximum Retry-After seconds to honor for rate limits (beyond this, fail immediately) */
    UPROPERTY(Config, EditAnywhere, Category="AI Provider",
        meta=(DisplayName="Max Rate Limit Wait (seconds)", ClampMin=0, ClampMax=300))
    int32 MaxRetryAfterWaitSeconds = 120;
```

---

## 8. Error Classification Strategy

The `FOliveProviderRetryManager::ClassifyError()` method uses a two-tier approach:

**Tier 1: Structured prefix parsing** (new format from updated providers)

```
[HTTP:{StatusCode}:RetryAfter={Seconds}] {Message}
```

Pattern: `/^\[HTTP:(\d+)(?::RetryAfter=(\d+))?\]\s*(.*)/`

**Tier 2: Heuristic string matching** (backward compat with existing error strings)

| Pattern | Classification |
|---------|---------------|
| Contains "Rate limited" or "429" | RateLimited |
| Contains "timeout", "timed out", "connection refused", "Network error" | Transient |
| Contains "5xx" status codes (500-599) | Transient |
| Contains "Invalid API key", "401", "403" | Terminal |
| Contains "Invalid request", "400" | Terminal |
| Everything else | Terminal (safe default) |

---

## 9. Response Truncation Detection

When `HandleComplete()` fires, check the finish reason from the last streaming chunk:

```cpp
void FOliveConversationManager::HandleComplete(const FString& FullResponse, const FOliveProviderUsage& Usage)
{
    // ... existing code ...

    // Check for truncated response
    bool bResponseTruncated = false;
    for (const FOliveStreamChunk& TC : PendingToolCalls)
    {
        // Not applicable to tool calls
    }

    // The provider sets FinishReason on the final chunk.
    // We need to capture it during streaming.
    if (LastFinishReason == TEXT("length"))
    {
        bResponseTruncated = true;
        OnResponseTruncated.Broadcast();

        // Add warning to the assistant message
        AssistantMessage.Content += TEXT("\n\n[WARNING: This response was truncated due to token limits. The model may have had more to say.]");
    }

    // ... rest of existing code ...
}
```

**Requires**: Capturing `FinishReason` from the last SSE chunk. Currently, the `FOliveStreamChunk` struct already has `FinishReason` field. The ConversationManager needs to store the last received finish reason during `HandleStreamChunk()`.

New private member:
```cpp
FString LastFinishReason;
```

Updated in `HandleStreamChunk()`:
```cpp
void FOliveConversationManager::HandleStreamChunk(const FOliveStreamChunk& Chunk)
{
    if (!Chunk.FinishReason.IsEmpty())
    {
        LastFinishReason = Chunk.FinishReason;
    }
    // ... existing code ...
}
```

---

## 10. Edge Cases and Error Handling

| Scenario | Handling |
|----------|----------|
| User sends 6+ messages while processing (queue overflow) | Oldest queued message dropped. Warning shown: "Queue full, oldest message discarded." |
| Panel closed during retry countdown | Retry continues. Toast shown on completion/failure. |
| Panel reopened during retry countdown | Panel rebinds to retry events, shows current countdown. |
| Rate limit with Retry-After > MaxRetryAfterWaitSeconds | Treated as terminal. Error shown: "Rate limited for too long (X seconds). Please wait and retry manually." |
| Provider destroyed while retry is pending | AliveFlag check in timer callback. Timer cancelled in destructor. |
| New session started while messages are queued | `StartNewSession()` calls `Queue->Clear()`. |
| Focus profile switch deferred, then user cancels operation | Deferred profile applied on cancel completion. |
| Network failure on every retry attempt (all 3 exhausted) | Final error shown: "Failed after 3 attempts: [original error]. Check your network connection." |
| Distillation removes ALL older messages | Truncation note says "All prior context summarized" to help model understand limited history. |
| User sends empty message | Already handled by existing empty check in `SendUserMessage()`. |
| ConversationManager GC'd while retry timer active | RetryManager holds TSharedPtr to Provider, not ConversationManager. Callbacks use WeakPtr. |

---

## 11. Implementation Order

The coder should implement in this sequence, with each step independently compilable and testable:

### Step 1: FOliveMessageQueue (new file, standalone)
- Create `OliveMessageQueue.h` and `OliveMessageQueue.cpp`
- Pure data structure, no dependencies on other Olive systems
- Test: Enqueue/Dequeue/overflow behavior

### Step 2: FOliveEditorChatSession (new file)
- Create `OliveEditorChatSession.h` and `OliveEditorChatSession.cpp`
- Singleton that owns ConversationManager
- Wire Initialize/Shutdown to FOliveAIEditorModule
- At this point, ConversationManager creation moves from panel to session
- Panel gets weak reference

### Step 3: Panel Lifetime Decoupling
- Modify `SOliveAIChatPanel` to use session singleton
- Change ConversationManager from owned (`TSharedPtr` member) to borrowed (`TWeakPtr` or raw `TSharedPtr` from session)
- Modify constructor: get from session, bind delegates
- Modify destructor: unbind delegates, notify session, do NOT destroy manager
- Add `NotifyPanelOpened/Closed` calls
- Test: Close panel while idle, reopen, history preserved

### Step 4: Background Completion Notification
- In FOliveEditorChatSession: listen to OnProcessingComplete
- If panel closed when completion fires, store summary and show toast
- Use `FNotificationInfo` / `FSlateNotificationManager` for toast
- Test: Start operation, close panel, wait for completion, see toast

### Step 5: Message Queue Integration
- Wire MessageQueue into ConversationManager (set via setter)
- Modify `SendUserMessage()`: enqueue if processing
- Modify `HandleComplete()` / `HandleError()`: drain queue
- Modify panel: show queue depth in status bar
- Change Send button text to "Queue" when processing
- Test: Send multiple messages during processing, verify FIFO delivery

### Step 6: FOliveProviderRetryManager (new file)
- Create `OliveProviderRetryManager.h` and `OliveProviderRetryManager.cpp`
- Error classification (heuristic string matching initially)
- Exponential backoff timer using FTimerManager
- Rate-limit Retry-After support
- Test: Mock transient errors, verify retry count and backoff timing

### Step 7: Retry Manager Integration
- Wire RetryManager into ConversationManager
- Modify `SendToProvider()` to use `RetryManager->SendWithRetry()`
- Wire retry events to panel UI (countdown display in status bar)
- Add retry settings to UOliveAISettings
- Test: Force a transient error (disconnect network), verify retry + UI countdown

### Step 8: Provider Error Prefix Format
- Update `FOliveOpenRouterProvider::OnResponseReceived()` to emit structured error prefix
- Update other providers similarly (Anthropic, OpenAI, Google, Ollama)
- Update RetryManager::ClassifyError() to parse prefix
- Test: 429 response correctly triggers rate-limit retry

### Step 9: Focus Profile Switch Guard
- Modify `ConversationManager::SetFocusProfile()` to defer when processing
- Modify panel `OnFocusProfileSelected()` to show deferred warning
- Apply deferred profile in `HandleComplete()`
- Test: Switch profile during operation, verify deferred application

### Step 10: Prompt Distillation Truncation Note
- Modify `FOlivePromptDistiller::Distill()` return type to `FOliveDistillationResult`
- In ConversationManager::SendToProvider(), inject truncation note message when distillation occurred
- Test: Fill conversation with many tool calls, verify truncation note appears in sent messages

### Step 11: Response Truncation Detection
- Add `LastFinishReason` tracking in ConversationManager
- Add `OnResponseTruncated` delegate
- Append warning text to truncated messages
- Show truncation badge in UI
- Test: Set very low max_tokens, verify truncation warning appears

---

## 12. Testing Strategy

### Unit Tests (Automation Framework)

- `FOliveMessageQueue`: Enqueue/dequeue ordering, overflow, clear
- `FOliveProviderRetryManager::ClassifyError()`: All error patterns correctly classified
- `FOliveDistillationResult`: Truncation metadata correctly computed

### Integration Tests

- Send message during processing: verify queued and delivered after completion
- Mock provider returning 429 with Retry-After: verify countdown and auto-retry
- Mock provider returning 500 three times: verify 3 retries then terminal error
- Close panel during operation: verify operation completes, toast shown
- Reopen panel: verify history intact, background completion banner shown

### Manual Tests

- Start a long multi-tool operation, close the panel tab, verify toast notification
- Send 3 messages rapidly while processing, verify all delivered in order
- Switch focus profile during tool execution, verify deferred application
- Trigger rate limit (low-credit OpenRouter key), verify countdown UI

---

## 13. Module Boundary Specification

### New Dependencies (none needed)

All new classes use only existing module dependencies:
- `Core`, `CoreUObject` (timers, delegates, shared pointers)
- `Slate`, `SlateCore` (notification toast)
- `HTTP` (already used by providers)

### Public API Surface

| Class | Visibility | Consumers |
|-------|-----------|-----------|
| `FOliveEditorChatSession` | Public | SOliveAIChatPanel, FOliveAIEditorModule |
| `FOliveMessageQueue` | Public | FOliveEditorChatSession, SOliveAIChatPanel |
| `FOliveProviderRetryManager` | Public | FOliveEditorChatSession, FOliveConversationManager |
| `FOliveProviderErrorInfo` | Public | RetryManager, Providers |
| `FOliveDistillationResult` | Public | ConversationManager, PromptDistiller |

### What This Phase Does NOT Change

- IR structs (no changes to OliveAIRuntime)
- MCP server (Phase 4 handles MCP resilience)
- Tool execution pipeline (no changes to WritePipeline, ToolRegistry)
- Provider interface contract (IOliveAIProvider unchanged)
- Brain state machine transitions (informational additions only)
