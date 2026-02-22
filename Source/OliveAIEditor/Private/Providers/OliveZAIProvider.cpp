// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveZAIProvider.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"

const FString FOliveZAIProvider::DefaultBaseUrl = TEXT("https://api.z.ai/api/paas/v4");
const FString FOliveZAIProvider::DefaultModel = TEXT("glm-4.6");

FOliveZAIProvider::FOliveZAIProvider()
{
	FOliveProviderConfig DefaultConfig;
	DefaultConfig.ProviderName = TEXT("zai");
	DefaultConfig.BaseUrl = DefaultBaseUrl;
	DefaultConfig.ModelId = DefaultModel;
	Inner.Configure(DefaultConfig);
}

TArray<FString> FOliveZAIProvider::GetAvailableModels() const
{
	// Keep this list small and non-authoritative; users can type any model their account supports.
	return {
		TEXT("glm-4.7"),
		TEXT("glm-4.6"),
		TEXT("glm-4.5"),
		TEXT("glm-4.5-air"),
		TEXT("glm-4.5-flash")
	};
}

void FOliveZAIProvider::Configure(const FOliveProviderConfig& InConfig)
{
	FOliveProviderConfig Effective = InConfig;

	// Normalize provider identifier; UI passes a display name, but tools may pass an id.
	if (Effective.ProviderName.IsEmpty())
	{
		Effective.ProviderName = TEXT("zai");
	}

	// Default base URL if not provided
	if (Effective.BaseUrl.IsEmpty())
	{
		Effective.BaseUrl = DefaultBaseUrl;
	}

	// Default model if not provided
	if (Effective.ModelId.IsEmpty())
	{
		Effective.ModelId = DefaultModel;
	}

	Inner.Configure(Effective);
}

bool FOliveZAIProvider::ValidateConfig(FString& OutError) const
{
	const FOliveProviderConfig& Config = Inner.GetConfig();

	if (Config.ApiKey.IsEmpty())
	{
		OutError = TEXT("Z.ai API key is required. Add it in Project Settings > Plugins > Olive AI Studio.");
		return false;
	}

	if (Config.BaseUrl.IsEmpty())
	{
		OutError = TEXT("Base URL is required. Leave it empty to use the default Z.ai endpoint.");
		return false;
	}

	if (Config.ModelId.IsEmpty())
	{
		OutError = TEXT("Model ID is required (e.g., glm-4.6).");
		return false;
	}

	return true;
}

void FOliveZAIProvider::SendMessage(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools,
	FOnOliveStreamChunk OnChunk,
	FOnOliveToolCall OnToolCall,
	FOnOliveComplete OnComplete,
	FOnOliveError OnError,
	const FOliveRequestOptions& Options)
{
	// Validate config with Z.ai-specific requirements, then delegate to the OpenAI-compatible implementation.
	FString Error;
	if (!ValidateConfig(Error))
	{
		OnError.ExecuteIfBound(Error);
		return;
	}

	Inner.SendMessage(Messages, Tools, OnChunk, OnToolCall, OnComplete, OnError, Options);
}

FString FOliveZAIProvider::NormalizeBaseUrlForModels(const FString& InBaseUrl)
{
	FString BaseUrl = InBaseUrl;

	// Remove trailing slashes
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1);
	}

	// If the user pasted a full completions URL, strip to the API root
	if (BaseUrl.EndsWith(TEXT("/chat/completions")))
	{
		BaseUrl.LeftChopInline(FString(TEXT("/chat/completions")).Len());
	}

	return BaseUrl;
}

void FOliveZAIProvider::ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const
{
	FString Error;
	if (!ValidateConfig(Error))
	{
		Callback(false, Error);
		return;
	}

	const FOliveProviderConfig& Config = Inner.GetConfig();
	const FString BaseUrl = NormalizeBaseUrlForModels(Config.BaseUrl);
	const FString ModelsUrl = BaseUrl + TEXT("/models");

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ModelsUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Config.ApiKey));

	Request->OnProcessRequestComplete().BindLambda(
		[Callback, ModelsUrl](FHttpRequestPtr /*Req*/, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				Callback(false, FString::Printf(TEXT("Cannot connect to Z.ai (%s). Check your internet connection."), *ModelsUrl));
				return;
			}

			const int32 Code = Response->GetResponseCode();
			if (Code >= 200 && Code < 300)
			{
				Callback(true, TEXT("Connected to Z.ai. API key accepted."));
			}
			else if (Code == 401)
			{
				Callback(false, TEXT("Invalid Z.ai API key (HTTP 401). Check your key and account permissions."));
			}
			else
			{
				// Non-2xx still implies the host is reachable; some deployments may not support /models.
				Callback(true, FString::Printf(TEXT("Z.ai reachable (HTTP %d). Models endpoint may not be supported."), Code));
			}
		});

	Request->ProcessRequest();
}

