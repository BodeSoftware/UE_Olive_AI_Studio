// Copyright Bode Software. All Rights Reserved.

#include "Providers/IOliveAIProvider.h"
#include "Providers/OliveOpenRouterProvider.h"
#include "Providers/OliveClaudeCodeProvider.h"
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

	if (ProviderName.Equals(TEXT("claudecode"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Claude Code"), ESearchCase::IgnoreCase) ||
		ProviderName.Equals(TEXT("Claude Code CLI"), ESearchCase::IgnoreCase))
	{
		return MakeShared<FOliveClaudeCodeProvider>();
	}

	// TODO: Add Anthropic, OpenAI, Google, Ollama providers

	return nullptr;
}

TArray<FString> FOliveProviderFactory::GetAvailableProviders()
{
	TArray<FString> Providers;

	// Claude Code CLI - no API key needed, uses Claude Max subscription
	if (FOliveClaudeCodeProvider::IsClaudeCodeInstalled())
	{
		Providers.Add(TEXT("Claude Code CLI"));
	}

	// OpenRouter - API key required
	Providers.Add(TEXT("OpenRouter"));

	// Future: Anthropic, OpenAI, Google, Ollama

	return Providers;
}
