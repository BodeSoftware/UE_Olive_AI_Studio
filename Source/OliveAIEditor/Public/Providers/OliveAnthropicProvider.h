// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"
#include "Interfaces/IHttpRequest.h"

/**
 * Anthropic Direct Provider
 *
 * Implements direct Anthropic Messages API access with SSE streaming.
 * Supports real-time text streaming and tool use via content block deltas.
 *
 * Anthropic SSE format uses event/data line pairs:
 *   event: content_block_delta
 *   data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}}
 */
class OLIVEAIEDITOR_API FOliveAnthropicProvider : public IOliveAIProvider
{
public:
	FOliveAnthropicProvider();
	virtual ~FOliveAnthropicProvider();

	// ==========================================
	// IOliveAIProvider Interface
	// ==========================================

	virtual FString GetProviderName() const override { return TEXT("Anthropic"); }
	virtual TArray<FString> GetAvailableModels() const override;
	virtual FString GetRecommendedModel() const override { return DefaultModel; }

	virtual void Configure(const FOliveProviderConfig& InConfig) override;
	virtual bool ValidateConfig(FString& OutError) const override;
	virtual const FOliveProviderConfig& GetConfig() const override { return Config; }

	virtual void SendMessage(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools,
		FOnOliveStreamChunk OnChunk,
		FOnOliveToolCall OnToolCall,
		FOnOliveComplete OnComplete,
		FOnOliveError OnError,
		const FOliveRequestOptions& Options = FOliveRequestOptions()
	) override;

	virtual void CancelRequest() override;
	virtual bool IsBusy() const override { return bIsBusy; }
	virtual FString GetLastError() const override { return LastError; }
	virtual void ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const override;

private:
	// ==========================================
	// Request Building
	// ==========================================

	/** Build the request body JSON (Anthropic Messages API format) */
	TSharedPtr<FJsonObject> BuildRequestBody(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools,
		const FOliveRequestOptions& Options) const;

	/** Normalize model name (strip "anthropic/" prefix if present) */
	FString NormalizeModelName(const FString& InModel) const;

	// ==========================================
	// Response Handling
	// ==========================================

	/** Handle HTTP response complete */
	void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);

	/** Process raw SSE data from the progress callback */
	void ProcessSSEData(const FString& Data);

	/** Process a single SSE line (event: or data: prefix) */
	void ProcessSSELine(const FString& Line);

	/** Parse a stream event with its type and JSON payload */
	void ParseStreamEvent(const FString& EventType, const TSharedPtr<FJsonObject>& Data);

	/** Finalize all pending tool calls and fire callbacks */
	void FinalizePendingToolCalls();

	/** Complete the streaming response */
	void CompleteStreaming();

	/** Handle error */
	void HandleError(const FString& ErrorMessage);

	// ==========================================
	// State
	// ==========================================

	/** Provider configuration */
	FOliveProviderConfig Config;

	/** Current HTTP request */
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> CurrentRequest;

	/** Is a request in progress */
	bool bIsBusy = false;

	/** Last error message */
	FString LastError;

	/** Weak flag to detect if this provider has been destroyed during async operations */
	TSharedPtr<bool> AliveFlag = MakeShared<bool>(true);

	// ==========================================
	// Streaming State
	// ==========================================

	/** Buffer for incomplete SSE data (full response content for progress delta calculation) */
	FString SSEBuffer;

	/** Current SSE event type (Anthropic sends event: lines before data: lines) */
	FString CurrentEventType;

	/** Accumulated full response text */
	FString AccumulatedResponse;

	/** Pending tool calls being assembled, keyed by content block index */
	TMap<int32, FOliveStreamChunk> PendingToolCalls;

	/** Accumulated partial JSON for tool arguments per block index */
	TMap<int32, FString> PendingToolArgsJson;

	/** Current content block index being processed */
	int32 CurrentBlockIndex = -1;

	/** Usage statistics accumulated from message_start and message_delta */
	FOliveProviderUsage CurrentUsage;

	// ==========================================
	// Callbacks
	// ==========================================

	FOnOliveStreamChunk OnChunkCallback;
	FOnOliveToolCall OnToolCallCallback;
	FOnOliveComplete OnCompleteCallback;
	FOnOliveError OnErrorCallback;

	// ==========================================
	// Constants
	// ==========================================

	static const FString AnthropicApiUrl;
	static const FString DefaultModel;
};
