// Copyright Bode Software. All Rights Reserved.

#include "Settings/OliveAISettings.h"

#define LOCTEXT_NAMESPACE "OliveAISettings"

UOliveAISettings::UOliveAISettings()
{
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
	switch (Provider)
	{
		case EOliveAIProvider::ClaudeCode:
			return TEXT(""); // Claude Code CLI uses subscription, no API key needed
		case EOliveAIProvider::OpenRouter:
			return OpenRouterApiKey;
		case EOliveAIProvider::Anthropic:
			return AnthropicApiKey;
		case EOliveAIProvider::OpenAI:
			return OpenAIApiKey;
		case EOliveAIProvider::Google:
			return GoogleApiKey;
		case EOliveAIProvider::Ollama:
			return TEXT(""); // Ollama doesn't need an API key
		default:
			return TEXT("");
	}
}

FString UOliveAISettings::GetCurrentBaseUrl() const
{
	switch (Provider)
	{
		case EOliveAIProvider::ClaudeCode:
			return TEXT(""); // Claude Code CLI is a local process, no URL
		case EOliveAIProvider::OpenRouter:
			return TEXT("https://openrouter.ai/api/v1");
		case EOliveAIProvider::Anthropic:
			return TEXT("https://api.anthropic.com/v1");
		case EOliveAIProvider::OpenAI:
			return TEXT("https://api.openai.com/v1");
		case EOliveAIProvider::Google:
			return TEXT("https://generativelanguage.googleapis.com/v1beta");
		case EOliveAIProvider::Ollama:
			return OllamaUrl;
		default:
			return TEXT("");
	}
}

bool UOliveAISettings::IsProviderConfigured() const
{
	// Claude Code CLI uses subscription, check if installed
	if (Provider == EOliveAIProvider::ClaudeCode)
	{
		// Will be validated by the provider itself
		return true;
	}

	// Ollama doesn't need an API key
	if (Provider == EOliveAIProvider::Ollama)
	{
		return !OllamaUrl.IsEmpty();
	}

	return !GetCurrentApiKey().IsEmpty();
}

#undef LOCTEXT_NAMESPACE
