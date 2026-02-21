// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"
#include "Interfaces/IHttpRequest.h"

/**
 * OpenAI-Compatible Provider
 *
 * A generic AI provider client for any OpenAI-compatible API endpoint.
 * Works with LM Studio, vLLM, Together AI, Groq, Fireworks, text-generation-webui,
 * and any other server that implements the OpenAI Chat Completions API.
 *
 * Key differences from the OpenAI provider:
 * - Base URL is fully user-configurable (not hardcoded)
 * - API key is optional (skip Authorization header if empty)
 * - No hardcoded model list — user types the model name
 * - Error messages reference "your endpoint" / "the server" instead of "OpenAI"
 */
class OLIVEAIEDITOR_API FOliveOpenAICompatibleProvider : public IOliveAIProvider
{
public:
	FOliveOpenAICompatibleProvider();
	virtual ~FOliveOpenAICompatibleProvider();

	// ==========================================
	// IOliveAIProvider Interface
	// ==========================================

	virtual FString GetProviderName() const override { return TEXT("OpenAI Compatible"); }
	virtual TArray<FString> GetAvailableModels() const override;
	virtual FString GetRecommendedModel() const override { return FString(); }

	virtual void Configure(const FOliveProviderConfig& InConfig) override;
	virtual bool ValidateConfig(FString& OutError) const override;
	virtual const FOliveProviderConfig& GetConfig() const override { return Config; }

	virtual void SendMessage(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools,
		FOnOliveStreamChunk OnChunk,
		FOnOliveToolCall OnToolCall,
		FOnOliveComplete OnComplete,
		FOnOliveError OnError
	) override;

	virtual void CancelRequest() override;
	virtual bool IsBusy() const override { return bIsBusy; }
	virtual FString GetLastError() const override { return LastError; }
	virtual void ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const override;

private:
	// ==========================================
	// Request Building
	// ==========================================

	/** Build the request body JSON */
	TSharedPtr<FJsonObject> BuildRequestBody(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools
	);

	/** Convert messages to JSON format */
	TArray<TSharedPtr<FJsonValue>> ConvertMessagesToJson(const TArray<FOliveChatMessage>& Messages);

	/** Convert tools to OpenAI function calling format */
	TArray<TSharedPtr<FJsonValue>> ConvertToolsToJson(const TArray<FOliveToolDefinition>& Tools);

	// ==========================================
	// URL Helpers
	// ==========================================

	/** Get the full chat completions URL, appending /chat/completions if needed */
	FString GetCompletionsUrl() const;

	// ==========================================
	// Response Handling
	// ==========================================

	/** Handle HTTP response complete */
	void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);

	/** Process SSE data chunks */
	void ProcessSSEData(const FString& Data);

	/** Process a single SSE line */
	void ProcessSSELine(const FString& Line);

	/** Parse a streamed chunk */
	void ParseStreamChunk(const TSharedPtr<FJsonObject>& ChunkJson);

	/** Handle streaming completion */
	void CompleteStreaming();

	/** Handle error */
	void HandleError(const FString& ErrorMessage);

	// ==========================================
	// Tool Call Parsing
	// ==========================================

	/** Parse tool call from delta */
	void ParseToolCallDelta(const TSharedPtr<FJsonObject>& Delta);

	/** Finalize pending tool calls */
	void FinalizePendingToolCalls();

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

	// ==========================================
	// Streaming State
	// ==========================================

	/** Buffer for incomplete SSE data */
	FString SSEBuffer;

	/** Accumulated full response text */
	FString AccumulatedResponse;

	/** Pending tool calls being built */
	TMap<int32, FOliveStreamChunk> PendingToolCalls;

	/** Usage statistics */
	FOliveProviderUsage CurrentUsage;

	// ==========================================
	// Callbacks
	// ==========================================

	FOnOliveStreamChunk OnChunkCallback;
	FOnOliveToolCall OnToolCallCallback;
	FOnOliveComplete OnCompleteCallback;
	FOnOliveError OnErrorCallback;
};
