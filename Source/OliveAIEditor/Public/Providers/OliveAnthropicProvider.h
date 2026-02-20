// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"
#include "Interfaces/IHttpRequest.h"

/**
 * Anthropic Direct Provider
 *
 * Implements direct Anthropic Messages API access.
 * Phase 0 scope keeps this implementation intentionally minimal:
 * - single request/response flow
 * - text responses
 * - tool use block parsing (non-streaming)
 */
class OLIVEAIEDITOR_API FOliveAnthropicProvider : public IOliveAIProvider
{
public:
	FOliveAnthropicProvider();
	virtual ~FOliveAnthropicProvider();

	virtual FString GetProviderName() const override { return TEXT("Anthropic"); }
	virtual TArray<FString> GetAvailableModels() const override;
	virtual FString GetRecommendedModel() const override { return TEXT("claude-sonnet-4-5"); }

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

private:
	TSharedPtr<FJsonObject> BuildRequestBody(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools) const;

	FString NormalizeModelName(const FString& InModel) const;

	void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess);
	void HandleError(const FString& ErrorMessage);

	FOliveProviderConfig Config;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> CurrentRequest;
	bool bIsBusy = false;
	FString LastError;
	FString AccumulatedResponse;

	FOnOliveStreamChunk OnChunkCallback;
	FOnOliveToolCall OnToolCallCallback;
	FOnOliveComplete OnCompleteCallback;
	FOnOliveError OnErrorCallback;

	static const FString AnthropicApiUrl;
	static const FString DefaultModel;
};
