// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"
#include "Interfaces/IHttpRequest.h"

/**
 * Google Gemini Provider
 *
 * AI provider client for Google Generative Language API (Gemini).
 * Supports SSE streaming and tool calling via Google's unique message format.
 *
 * Key differences from OpenAI format:
 * - Uses `contents[]` instead of `messages[]`
 * - Roles: "user" and "model" (not "assistant")
 * - System messages go in top-level `systemInstruction` field
 * - Tool calls use `functionCall` / `functionResponse` parts
 * - API key is passed as URL query parameter, not Authorization header
 * - SSE data contains `candidates[0].content.parts[]` structure
 */
class OLIVEAIEDITOR_API FOliveGoogleProvider : public IOliveAIProvider
{
public:
	FOliveGoogleProvider();
	virtual ~FOliveGoogleProvider();

	// ==========================================
	// IOliveAIProvider Interface
	// ==========================================

	virtual FString GetProviderName() const override { return TEXT("Google"); }
	virtual TArray<FString> GetAvailableModels() const override;
	virtual FString GetRecommendedModel() const override { return TEXT("gemini-2.0-flash"); }

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

	/** Build the full request body JSON in Google Gemini format */
	TSharedPtr<FJsonObject> BuildRequestBody(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools
	);

	/**
	 * Convert FOliveChatMessage array to Google contents[] format.
	 * System messages are extracted and returned separately via OutSystemText.
	 */
	TArray<TSharedPtr<FJsonValue>> ConvertMessagesToContents(
		const TArray<FOliveChatMessage>& Messages,
		FString& OutSystemText
	);

	/** Convert tools to Google functionDeclarations format */
	TArray<TSharedPtr<FJsonValue>> ConvertToolsToJson(const TArray<FOliveToolDefinition>& Tools);

	/** Build the streaming API URL with model name and API key */
	FString BuildRequestUrl() const;

	// ==========================================
	// Response Handling
	// ==========================================

	/** Handle HTTP response complete */
	void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);

	/** Process raw SSE data from the progress callback */
	void ProcessSSEData(const FString& Data);

	/** Process a single SSE line */
	void ProcessSSELine(const FString& Line);

	/** Parse a streamed chunk from Google's candidate format */
	void ParseStreamChunk(const TSharedPtr<FJsonObject>& ChunkJson);

	/** Handle streaming completion */
	void CompleteStreaming();

	/** Handle error with provider-specific messages */
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

	// ==========================================
	// Streaming State
	// ==========================================

	/** Buffer for tracking how much of the response has been processed */
	FString SSEBuffer;

	/** Accumulated full response text */
	FString AccumulatedResponse;

	/** Usage statistics */
	FOliveProviderUsage CurrentUsage;

	/** Finish reason from the last candidate */
	FString LastFinishReason;

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

	static const FString GoogleApiBaseUrl;
	static const FString DefaultModel;
};
