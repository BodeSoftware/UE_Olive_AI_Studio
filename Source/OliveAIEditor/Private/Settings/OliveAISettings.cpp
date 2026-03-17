// Copyright Bode Software. All Rights Reserved.

#include "Settings/OliveAISettings.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "OliveAISettings"

namespace
{
FString GetProviderKey(EOliveAIProvider Provider)
{
	const UEnum* ProviderEnum = StaticEnum<EOliveAIProvider>();
	if (!ProviderEnum)
	{
		return TEXT("Unknown");
	}

	return ProviderEnum->GetNameStringByValue(static_cast<int64>(Provider));
}

bool LooksLikeModelForProvider(EOliveAIProvider Provider, const FString& InModelId)
{
	const FString ModelId = InModelId.TrimStartAndEnd().ToLower();
	if (ModelId.IsEmpty())
	{
		return false;
	}

	switch (Provider)
	{
	case EOliveAIProvider::OpenRouter:
		// OpenRouter uses "provider/model" format.
		return ModelId.Contains(TEXT("/"));
	case EOliveAIProvider::Anthropic:
		// Anthropic uses "claude-*" model ids.
		return ModelId.StartsWith(TEXT("claude-")) || ModelId.Contains(TEXT("claude"));
	case EOliveAIProvider::ClaudeCode:
		// Claude Code CLI models are also "claude-*" ids.
		return ModelId.StartsWith(TEXT("claude-")) || ModelId.Contains(TEXT("claude"));
	case EOliveAIProvider::Codex:
		// Codex CLI uses OpenAI models (o3, gpt-4.1, etc.)
		return ModelId.StartsWith(TEXT("o")) || ModelId.StartsWith(TEXT("gpt-")) || ModelId.Contains(TEXT("codex"));
	case EOliveAIProvider::OpenAI:
		// OpenAI commonly uses gpt-* and o* models.
		return ModelId.StartsWith(TEXT("gpt-")) || ModelId.StartsWith(TEXT("o1")) || ModelId.StartsWith(TEXT("o3"));
	case EOliveAIProvider::Google:
		// Gemini model ids are typically "gemini-*".
		return ModelId.StartsWith(TEXT("gemini"));
	case EOliveAIProvider::ZAI:
		// Z.ai commonly uses glm-* model ids.
		return ModelId.StartsWith(TEXT("glm")) || ModelId.Contains(TEXT("glm"));
	case EOliveAIProvider::Ollama:
		// Ollama model ids are arbitrary (local).
		return true;
	case EOliveAIProvider::OpenAICompatible:
		// Endpoint serves arbitrary models.
		return true;
	default:
		return true;
	}
}

TMap<FString, FString> ParseProviderModelOverrides(const FString& Json)
{
	TMap<FString, FString> Overrides;
	if (Json.IsEmpty())
	{
		return Overrides;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		return Overrides;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RootObject->Values)
	{
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
		{
			continue;
		}

		const FString ModelId = Pair.Value->AsString().TrimStartAndEnd();
		if (!ModelId.IsEmpty())
		{
			Overrides.Add(Pair.Key, ModelId);
		}
	}

	return Overrides;
}

FString SerializeProviderModelOverrides(const TMap<FString, FString>& Overrides)
{
	if (Overrides.Num() == 0)
	{
		return TEXT("");
	}

	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Overrides)
	{
		if (!Pair.Value.TrimStartAndEnd().IsEmpty())
		{
			RootObject->SetStringField(Pair.Key, Pair.Value.TrimStartAndEnd());
		}
	}

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(RootObject, Writer);
	return Json;
}
} // namespace

UOliveAISettings::UOliveAISettings()
{
}

void UOliveAISettings::PostInitProperties()
{
	Super::PostInitProperties();

	UE_LOG(LogOliveAI, Log, TEXT("OliveAISettings: DefaultChatMode = %s"),
		DefaultChatMode == EOliveChatModeConfig::Code ? TEXT("Code") :
		DefaultChatMode == EOliveChatModeConfig::Plan ? TEXT("Plan") : TEXT("Ask"));
}

#if WITH_EDITOR
FText UOliveAISettings::GetSectionText() const
{
	return LOCTEXT("SectionText", "Olive AI Studio");
}

FText UOliveAISettings::GetSectionDescription() const
{
	return LOCTEXT("SectionDescription", "Configure the AI-powered development assistant for Unreal Engine.");
}
#endif

UOliveAISettings* UOliveAISettings::Get()
{
	return GetMutableDefault<UOliveAISettings>();
}

FString UOliveAISettings::GetCurrentApiKey() const
{
	return GetApiKeyForProvider(Provider);
}

FString UOliveAISettings::GetApiKeyForProvider(EOliveAIProvider InProvider) const
{
	switch (InProvider)
	{
		case EOliveAIProvider::ClaudeCode:
			return TEXT(""); // Claude Code CLI uses subscription, no API key needed
		case EOliveAIProvider::Codex:
			return TEXT(""); // Codex CLI uses codex login or OPENAI_API_KEY env var
		case EOliveAIProvider::OpenRouter:
			return OpenRouterApiKey;
		case EOliveAIProvider::ZAI:
			return ZaiApiKey;
		case EOliveAIProvider::Anthropic:
			return AnthropicApiKey;
		case EOliveAIProvider::OpenAI:
			return OpenAIApiKey;
		case EOliveAIProvider::Google:
			return GoogleApiKey;
		case EOliveAIProvider::Ollama:
			return TEXT(""); // Ollama doesn't need an API key
		case EOliveAIProvider::OpenAICompatible:
			return OpenAICompatibleApiKey;
		default:
			return TEXT("");
	}
}

FString UOliveAISettings::GetBaseUrlForProvider(EOliveAIProvider InProvider) const
{
	switch (InProvider)
	{
		case EOliveAIProvider::ClaudeCode:
			return TEXT(""); // Claude Code CLI is a local process, no URL
		case EOliveAIProvider::Codex:
			return TEXT(""); // Codex CLI is a local process, no URL
		case EOliveAIProvider::OpenRouter:
			return TEXT("https://openrouter.ai/api/v1/chat/completions");
		case EOliveAIProvider::ZAI:
			return bZaiUseCodingEndpoint
				? TEXT("https://api.z.ai/api/coding/paas/v4")
				: TEXT("https://api.z.ai/api/paas/v4");
		case EOliveAIProvider::Anthropic:
			return TEXT("https://api.anthropic.com/v1/messages");
		case EOliveAIProvider::OpenAI:
			return TEXT("https://api.openai.com/v1/chat/completions");
		case EOliveAIProvider::Google:
			return TEXT("https://generativelanguage.googleapis.com/v1beta/models/");
		case EOliveAIProvider::Ollama:
			return OllamaUrl;
		case EOliveAIProvider::OpenAICompatible:
			return OpenAICompatibleUrl;
		default:
			return TEXT("");
	}
}

FString UOliveAISettings::GetCurrentBaseUrl() const
{
	return GetBaseUrlForProvider(Provider);
}

bool UOliveAISettings::IsProviderConfigured() const
{
	// CLI providers use subscription/login, check if installed
	if (Provider == EOliveAIProvider::ClaudeCode || Provider == EOliveAIProvider::Codex)
	{
		// Will be validated by the provider itself
		return true;
	}

	// Ollama doesn't need an API key
	if (Provider == EOliveAIProvider::Ollama)
	{
		return !OllamaUrl.IsEmpty();
	}

	// OpenAI Compatible needs a URL (key is optional)
	if (Provider == EOliveAIProvider::OpenAICompatible)
	{
		return !OpenAICompatibleUrl.IsEmpty();
	}

	return !GetCurrentApiKey().IsEmpty();
}

FString UOliveAISettings::GetSelectedModelForProvider(EOliveAIProvider InProvider) const
{
	const FString ProviderKey = GetProviderKey(InProvider);
	const TMap<FString, FString> Overrides = ParseProviderModelOverrides(ProviderModelOverridesJson);

	if (const FString* FoundModel = Overrides.Find(ProviderKey))
	{
		const FString NormalizedModel = FoundModel->TrimStartAndEnd();
		if (!NormalizedModel.IsEmpty())
		{
			return NormalizedModel;
		}
	}

	if (InProvider == Provider)
	{
		const FString LegacyModel = SelectedModel.TrimStartAndEnd();
		if (!LegacyModel.IsEmpty() && LooksLikeModelForProvider(InProvider, LegacyModel))
		{
			return LegacyModel;
		}
	}

	return TEXT("");
}

void UOliveAISettings::SetSelectedModelForProvider(EOliveAIProvider InProvider, const FString& InModel)
{
	const FString ProviderKey = GetProviderKey(InProvider);
	const FString NormalizedModel = InModel.TrimStartAndEnd();
	TMap<FString, FString> Overrides = ParseProviderModelOverrides(ProviderModelOverridesJson);

	if (NormalizedModel.IsEmpty())
	{
		Overrides.Remove(ProviderKey);
	}
	else
	{
		Overrides.Add(ProviderKey, NormalizedModel);
	}

	ProviderModelOverridesJson = SerializeProviderModelOverrides(Overrides);

	if (InProvider == Provider)
	{
		SelectedModel = NormalizedModel;
	}
}

#undef LOCTEXT_NAMESPACE
