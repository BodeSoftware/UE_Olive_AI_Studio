// Copyright Bode Software. All Rights Reserved.

#include "Providers/IOliveAIProvider.h"
#include "Providers/OliveOpenRouterProvider.h"
#include "Providers/OliveAnthropicProvider.h"
#include "Providers/OliveClaudeCodeProvider.h"
#include "Providers/OliveCodexProvider.h"
#include "Providers/OliveOpenAIProvider.h"
#include "Providers/OliveOllamaProvider.h"
#include "Providers/OliveOpenAICompatibleProvider.h"
#include "Providers/OliveGoogleProvider.h"
#include "Providers/OliveZAIProvider.h"
#include "Settings/OliveAISettings.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"

// ==========================================
// FOliveChatMessage
// ==========================================

FString FOliveChatMessage::RoleToString(EOliveChatRole InRole)
{
	switch (InRole)
	{
	case EOliveChatRole::System:
		return TEXT("system");
	case EOliveChatRole::User:
		return TEXT("user");
	case EOliveChatRole::Assistant:
		return TEXT("assistant");
	case EOliveChatRole::Tool:
		return TEXT("tool");
	default:
		return TEXT("user");
	}
}

EOliveChatRole FOliveChatMessage::StringToRole(const FString& RoleStr)
{
	if (RoleStr.Equals(TEXT("system"), ESearchCase::IgnoreCase))
	{
		return EOliveChatRole::System;
	}
	if (RoleStr.Equals(TEXT("assistant"), ESearchCase::IgnoreCase))
	{
		return EOliveChatRole::Assistant;
	}
	if (RoleStr.Equals(TEXT("tool"), ESearchCase::IgnoreCase))
	{
		return EOliveChatRole::Tool;
	}
	return EOliveChatRole::User;
}

TSharedPtr<FJsonObject> FOliveChatMessage::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	FString RoleString = RoleToString(Role);
	Json->SetStringField(TEXT("role"), RoleString);

	if (Role == EOliveChatRole::Tool)
	{
		// Tool response format
		Json->SetStringField(TEXT("tool_call_id"), ToolCallId);
		Json->SetStringField(TEXT("content"), Content);
	}
	else if (Role == EOliveChatRole::Assistant && ToolCalls.Num() > 0)
	{
		// Assistant with tool calls
		if (!Content.IsEmpty())
		{
			Json->SetStringField(TEXT("content"), Content);
		}
		else
		{
			Json->SetField(TEXT("content"), MakeShared<FJsonValueNull>());
		}

		// Add tool calls
		TArray<TSharedPtr<FJsonValue>> ToolCallsArray;
		for (const FOliveStreamChunk& ToolCall : ToolCalls)
		{
			TSharedPtr<FJsonObject> ToolCallJson = MakeShared<FJsonObject>();
			ToolCallJson->SetStringField(TEXT("id"), ToolCall.ToolCallId);
			ToolCallJson->SetStringField(TEXT("type"), TEXT("function"));

			TSharedPtr<FJsonObject> FunctionJson = MakeShared<FJsonObject>();
			FunctionJson->SetStringField(TEXT("name"), ToolCall.ToolName);

			// Serialize arguments
			if (ToolCall.ToolArguments.IsValid())
			{
				FString ArgsString;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsString);
				FJsonSerializer::Serialize(ToolCall.ToolArguments.ToSharedRef(), Writer);
				FunctionJson->SetStringField(TEXT("arguments"), ArgsString);
			}
			else
			{
				FunctionJson->SetStringField(TEXT("arguments"), TEXT("{}"));
			}

			ToolCallJson->SetObjectField(TEXT("function"), FunctionJson);
			ToolCallsArray.Add(MakeShared<FJsonValueObject>(ToolCallJson));
		}

		Json->SetArrayField(TEXT("tool_calls"), ToolCallsArray);
	}
	else
	{
		// Regular message
		Json->SetStringField(TEXT("content"), Content);
	}

	return Json;
}

// ==========================================
// FOliveProviderFactory
// ==========================================

TSharedPtr<IOliveAIProvider> FOliveProviderFactory::CreateProvider(const FString& ProviderName)
{
	if (ProviderName.Equals(TEXT("openrouter"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("OpenRouter"), ESearchCase::CaseSensitive))
	{
		return MakeShared<FOliveOpenRouterProvider>();
	}

	if (ProviderName.Equals(TEXT("zai"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("z.ai"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Z.ai"), ESearchCase::CaseSensitive) ||
		ProviderName.Equals(TEXT("ZAI"), ESearchCase::CaseSensitive))
	{
		return MakeShared<FOliveZAIProvider>();
	}

	if (ProviderName.Equals(TEXT("claudecode"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Claude Code"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Claude Code CLI"), ESearchCase::IgnoreCase))
	{
		const UOliveAISettings* Settings = UOliveAISettings::Get();
		if (Settings && !Settings->bEnableLegacyClaudeCodeProvider)
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Claude Code provider is disabled. Enable 'Enable Legacy Claude Code Provider' in Project Settings > Plugins > Olive AI Studio to use it as an in-editor provider. The recommended workflow is to use Claude Code as an external MCP integration."));
			return nullptr;
		}
		return MakeShared<FOliveClaudeCodeProvider>();
	}

	if (ProviderName.Equals(TEXT("codex"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Codex CLI"), ESearchCase::IgnoreCase))
	{
		return MakeShared<FOliveCodexProvider>();
	}

	if (ProviderName.Equals(TEXT("anthropic"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Anthropic"), ESearchCase::CaseSensitive))
	{
		return MakeShared<FOliveAnthropicProvider>();
	}

	if (ProviderName.Equals(TEXT("openai"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("OpenAI"), ESearchCase::CaseSensitive))
	{
		return MakeShared<FOliveOpenAIProvider>();
	}

	if (ProviderName.Equals(TEXT("google"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Google"), ESearchCase::CaseSensitive))
	{
		return MakeShared<FOliveGoogleProvider>();
	}

	if (ProviderName.Equals(TEXT("ollama"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Ollama"), ESearchCase::CaseSensitive))
	{
		return MakeShared<FOliveOllamaProvider>();
	}

	if (ProviderName.Equals(TEXT("openai_compatible"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("OpenAI Compatible"), ESearchCase::IgnoreCase))
	{
		return MakeShared<FOliveOpenAICompatibleProvider>();
	}

	return nullptr;
}

TArray<FString> FOliveProviderFactory::GetAvailableProviders()
{
	TArray<FString> Providers;

	// Claude Code CLI - gated behind legacy flag (default: disabled)
	// Recommended workflow: use Claude Code as external MCP integration via companion panel
	{
		const UOliveAISettings* Settings = UOliveAISettings::Get();
		if (Settings && Settings->bEnableLegacyClaudeCodeProvider)
		{
			if (FOliveClaudeCodeProvider::IsClaudeCodeInstalled())
			{
				Providers.Add(TEXT("Claude Code CLI"));
			}
		}
	}

	// Codex CLI - uses ChatGPT subscription or OPENAI_API_KEY
	if (FOliveCodexProvider::IsCodexInstalled())
	{
		Providers.Add(TEXT("Codex CLI"));
	}

	// OpenRouter - API key required
	Providers.Add(TEXT("OpenRouter"));

	// Z.ai - API key required
	Providers.Add(TEXT("Z.ai"));

	// Anthropic direct - API key required
	Providers.Add(TEXT("Anthropic"));

	// OpenAI direct - API key required
	Providers.Add(TEXT("OpenAI"));

	// Google AI - API key required
	Providers.Add(TEXT("Google"));

	// Ollama - local models, no API key required
	Providers.Add(TEXT("Ollama"));

	// OpenAI Compatible - custom endpoint (LM Studio, vLLM, Groq, etc.)
	Providers.Add(TEXT("OpenAI Compatible"));

	return Providers;
}
