// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"
#include "Providers/OliveOpenAICompatibleProvider.h"

/**
 * Z.ai Provider
 *
 * OpenAI-compatible provider client for Z.ai (https://z.ai/).
 *
 * Notes:
 * - Uses OpenAI Chat Completions-compatible schema.
 * - Base URL should be the API root; the underlying OpenAI-compatible provider appends /chat/completions if needed.
 */
class OLIVEAIEDITOR_API FOliveZAIProvider : public IOliveAIProvider
{
public:
	FOliveZAIProvider();
	virtual ~FOliveZAIProvider() override = default;

	// ==========================================
	// IOliveAIProvider Interface
	// ==========================================

	virtual FString GetProviderName() const override { return TEXT("Z.ai"); }
	virtual TArray<FString> GetAvailableModels() const override;
	virtual FString GetRecommendedModel() const override { return DefaultModel; }

	virtual void Configure(const FOliveProviderConfig& InConfig) override;
	virtual bool ValidateConfig(FString& OutError) const override;
	virtual const FOliveProviderConfig& GetConfig() const override { return Inner.GetConfig(); }

	virtual void SendMessage(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools,
		FOnOliveStreamChunk OnChunk,
		FOnOliveToolCall OnToolCall,
		FOnOliveComplete OnComplete,
		FOnOliveError OnError,
		const FOliveRequestOptions& Options = FOliveRequestOptions()
	) override;

	virtual void CancelRequest() override { Inner.CancelRequest(); }
	virtual bool IsBusy() const override { return Inner.IsBusy(); }
	virtual FString GetLastError() const override { return Inner.GetLastError(); }
	virtual void ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const override;

private:
	static FString NormalizeBaseUrlForModels(const FString& InBaseUrl);

private:
	FOliveOpenAICompatibleProvider Inner;

	static const FString DefaultBaseUrl;
	static const FString DefaultModel;
};

