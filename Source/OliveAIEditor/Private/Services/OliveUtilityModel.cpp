// Copyright Bode Software. All Rights Reserved.

/**
 * OliveUtilityModel.cpp
 *
 * Lightweight utility model wrapper for quick single-shot LLM completions.
 * Used for keyword expansion and other sub-second tasks.
 * Creates a transient provider per call, sends one message, blocks with
 * tick-pumping until completion or timeout.
 */

#include "Services/OliveUtilityModel.h"
#include "Settings/OliveAISettings.h"
#include "Providers/IOliveAIProvider.h"
#include "Providers/OliveClaudeCodeProvider.h"
#include "Template/OliveTemplateSystem.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveUtilityModel, Log, All);

// ============================================================================
// ProviderEnumToName
// ============================================================================

FString FOliveUtilityModel::ProviderEnumToName(EOliveAIProvider Provider)
{
	switch (Provider)
	{
		case EOliveAIProvider::ClaudeCode:        return TEXT("claudecode");
		case EOliveAIProvider::OpenRouter:         return TEXT("openrouter");
		case EOliveAIProvider::ZAI:                return TEXT("zai");
		case EOliveAIProvider::Anthropic:          return TEXT("anthropic");
		case EOliveAIProvider::OpenAI:             return TEXT("openai");
		case EOliveAIProvider::Google:             return TEXT("google");
		case EOliveAIProvider::Ollama:             return TEXT("ollama");
		case EOliveAIProvider::OpenAICompatible:   return TEXT("openai_compatible");
		default:                                   return TEXT("");
	}
}

// ============================================================================
// IsAvailable
// ============================================================================

bool FOliveUtilityModel::IsAvailable()
{
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		return false;
	}

	// Must be enabled
	if (!Settings->bEnableLLMKeywordExpansion)
	{
		return false;
	}

	// CLI providers can't do simple HTTP completions
	if (Settings->UtilityModelProvider == EOliveAIProvider::ClaudeCode)
	{
		return false;
	}

	// Must have a model ID
	if (Settings->UtilityModelId.TrimStartAndEnd().IsEmpty())
	{
		return false;
	}

	// Resolve API key: explicit override first, then fall back to provider's key
	FString ResolvedKey = Settings->UtilityModelApiKey.TrimStartAndEnd();
	if (ResolvedKey.IsEmpty())
	{
		ResolvedKey = Settings->GetApiKeyForProvider(Settings->UtilityModelProvider).TrimStartAndEnd();
	}

	// Ollama doesn't need an API key
	if (Settings->UtilityModelProvider == EOliveAIProvider::Ollama)
	{
		return true;
	}

	// OpenAI Compatible may not need an API key if URL is set
	if (Settings->UtilityModelProvider == EOliveAIProvider::OpenAICompatible)
	{
		return !Settings->GetBaseUrlForProvider(EOliveAIProvider::OpenAICompatible).TrimStartAndEnd().IsEmpty();
	}

	return !ResolvedKey.IsEmpty();
}

// ============================================================================
// TrySendCompletion
// ============================================================================

bool FOliveUtilityModel::TrySendCompletion(
	EOliveAIProvider ProviderType,
	const FString& ModelId,
	const FString& ApiKey,
	const FString& BaseUrl,
	int32 TimeoutSeconds,
	const FString& SystemPrompt,
	const FString& UserPrompt,
	FString& OutResponse,
	FString& OutError)
{
	// Create a transient provider instance
	const FString ProviderName = ProviderEnumToName(ProviderType);
	TSharedPtr<IOliveAIProvider> Provider = FOliveProviderFactory::CreateProvider(ProviderName);
	if (!Provider.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to create provider '%s'"), *ProviderName);
		return false;
	}

	// Configure the provider
	FOliveProviderConfig Config;
	Config.ProviderName = ProviderName;
	Config.ApiKey = ApiKey;
	Config.ModelId = ModelId;
	Config.BaseUrl = BaseUrl;
	Config.Temperature = 0.0f;
	Config.MaxTokens = 256;
	Config.TimeoutSeconds = TimeoutSeconds;
	Provider->Configure(Config);

	// Build messages: System + User
	TArray<FOliveChatMessage> Messages;

	FOliveChatMessage SystemMsg;
	SystemMsg.Role = EOliveChatRole::System;
	SystemMsg.Content = SystemPrompt;
	SystemMsg.Timestamp = FDateTime::UtcNow();
	Messages.Add(SystemMsg);

	FOliveChatMessage UserMsg;
	UserMsg.Role = EOliveChatRole::User;
	UserMsg.Content = UserPrompt;
	UserMsg.Timestamp = FDateTime::UtcNow();
	Messages.Add(UserMsg);

	// No tools for utility completions
	TArray<FOliveToolDefinition> EmptyTools;

	// Completion tracking
	TSharedPtr<FThreadSafeBool> bDone = MakeShared<FThreadSafeBool>(false);
	FString AccumulatedResponse;
	FString CompletionError;
	bool bSuccess = false;

	// Request options -- override MaxTokens and Timeout
	FOliveRequestOptions Options;
	Options.MaxTokens = 256;
	Options.Temperature = 0.0f;
	Options.TimeoutSeconds = TimeoutSeconds;

	// OnChunk: accumulate text
	FOnOliveStreamChunk OnChunk;
	OnChunk.BindLambda([&AccumulatedResponse](const FOliveStreamChunk& Chunk)
	{
		if (!Chunk.bIsToolCall && !Chunk.Text.IsEmpty())
		{
			AccumulatedResponse += Chunk.Text;
		}
	});

	// OnToolCall: ignored for utility completions
	FOnOliveToolCall OnToolCall;

	// OnComplete: set done flag
	FOnOliveComplete OnComplete;
	OnComplete.BindLambda([&bDone, &bSuccess, &AccumulatedResponse, &OutResponse](
		const FString& FullResponse, const FOliveProviderUsage& Usage)
	{
		// Some providers give the full response in OnComplete, others accumulate via chunks
		if (!FullResponse.IsEmpty() && AccumulatedResponse.IsEmpty())
		{
			AccumulatedResponse = FullResponse;
		}
		bSuccess = true;
		*bDone = true;
	});

	// OnError: capture error and set done flag
	FOnOliveError OnError;
	OnError.BindLambda([&bDone, &CompletionError](const FString& ErrorMessage)
	{
		CompletionError = ErrorMessage;
		*bDone = true;
	});

	// Fire the request
	Provider->SendMessage(Messages, EmptyTools, OnChunk, OnToolCall, OnComplete, OnError, Options);

	// Tick-pump loop: wait for completion or timeout.
	// FTSTicker::GetCoreTicker().Tick() processes HTTP callbacks on the game thread.
	const double StartTime = FPlatformTime::Seconds();
	const double MaxWaitTime = static_cast<double>(TimeoutSeconds);

	while (!(*bDone))
	{
		const double ElapsedTime = FPlatformTime::Seconds() - StartTime;
		if (ElapsedTime >= MaxWaitTime)
		{
			// Timeout -- cancel the request
			Provider->CancelRequest();
			OutError = FString::Printf(TEXT("Utility model timed out after %d seconds"), TimeoutSeconds);
			UE_LOG(LogOliveUtilityModel, Warning, TEXT("%s"), *OutError);
			return false;
		}

		FPlatformProcess::Sleep(0.01f);
		FTSTicker::GetCoreTicker().Tick(0.01f);
	}

	if (bSuccess)
	{
		OutResponse = AccumulatedResponse;
		return true;
	}

	OutError = CompletionError.IsEmpty() ? TEXT("Utility model request failed (unknown error)") : CompletionError;
	return false;
}

// ============================================================================
// TrySendCompletionViaCLI
// ============================================================================

bool FOliveUtilityModel::TrySendCompletionViaCLI(
	int32 TimeoutSeconds,
	const FString& SystemPrompt,
	const FString& UserPrompt,
	FString& OutResponse,
	FString& OutError)
{
	FString ClaudePath = FOliveClaudeCodeProvider::GetClaudeExecutablePath();
	if (ClaudePath.IsEmpty())
	{
		OutError = TEXT("Claude Code CLI not found");
		return false;
	}

	// Build the combined prompt (system + user in one string for --print mode)
	FString CombinedPrompt = FString::Printf(TEXT("%s\n\n%s"), *SystemPrompt, *UserPrompt);

	// Build CLI arguments for --print mode
	// --print (-p): one-shot completion, output to stdout
	// --output-format text: plain text output (no JSON wrapping)
	// --max-turns 1: single turn, no tool use
	const bool bIsJs = ClaudePath.EndsWith(TEXT(".js"));

	// Escape the prompt for command line (replace double quotes)
	FString EscapedPrompt = CombinedPrompt.Replace(TEXT("\""), TEXT("\\\""));

	FString Args;
	if (bIsJs)
	{
		Args = FString::Printf(
			TEXT("\"%s\" --print --output-format text --max-turns 1 \"%s\""),
			*ClaudePath, *EscapedPrompt);
	}
	else
	{
		Args = FString::Printf(
			TEXT("--print --output-format text --max-turns 1 \"%s\""),
			*EscapedPrompt);
	}

	const FString ExePath = bIsJs ? TEXT("node") : ClaudePath;

	UE_LOG(LogOliveUtilityModel, Log,
		TEXT("Attempting CLI completion via claude --print (timeout: %ds)"),
		TimeoutSeconds);

	FString StdOut;
	FString StdErr;
	int32 ReturnCode = -1;

	// ExecProcess blocks until completion. For a quick keyword extraction (~2-5s)
	// this is acceptable. The TimeoutSeconds parameter is informational here --
	// ExecProcess does not have a built-in timeout mechanism.
	const bool bLaunched = FPlatformProcess::ExecProcess(
		*ExePath, *Args, &ReturnCode, &StdOut, &StdErr);

	if (!bLaunched)
	{
		OutError = TEXT("Failed to launch Claude Code CLI process");
		return false;
	}

	if (ReturnCode != 0)
	{
		OutError = FString::Printf(TEXT("Claude CLI exited with code %d: %s"),
			ReturnCode, *StdErr.Left(200));
		UE_LOG(LogOliveUtilityModel, Warning, TEXT("%s"), *OutError);
		return false;
	}

	StdOut.TrimStartAndEndInline();
	if (StdOut.IsEmpty())
	{
		OutError = TEXT("Claude CLI returned empty response");
		return false;
	}

	OutResponse = StdOut;
	UE_LOG(LogOliveUtilityModel, Log,
		TEXT("CLI completion succeeded (%d chars)"), OutResponse.Len());
	return true;
}

// ============================================================================
// SendSimpleCompletion
// ============================================================================

bool FOliveUtilityModel::SendSimpleCompletion(
	const FString& SystemPrompt,
	const FString& UserPrompt,
	FString& OutResponse,
	FString& OutError)
{
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		OutError = TEXT("Settings not available");
		return false;
	}

	// Tier 1: Try the configured utility model provider
	if (IsAvailable())
	{
		FString ResolvedKey = Settings->UtilityModelApiKey.TrimStartAndEnd();
		if (ResolvedKey.IsEmpty())
		{
			ResolvedKey = Settings->GetApiKeyForProvider(Settings->UtilityModelProvider).TrimStartAndEnd();
		}

		const FString BaseUrl = Settings->GetBaseUrlForProvider(Settings->UtilityModelProvider);

		UE_LOG(LogOliveUtilityModel, Log,
			TEXT("Attempting utility model completion via %s (model: %s)"),
			*ProviderEnumToName(Settings->UtilityModelProvider),
			*Settings->UtilityModelId);

		if (TrySendCompletion(
			Settings->UtilityModelProvider,
			Settings->UtilityModelId,
			ResolvedKey,
			BaseUrl,
			Settings->UtilityModelTimeoutSeconds,
			SystemPrompt,
			UserPrompt,
			OutResponse,
			OutError))
		{
			UE_LOG(LogOliveUtilityModel, Log,
				TEXT("Utility model completion succeeded (%d chars)"),
				OutResponse.Len());
			return true;
		}

		UE_LOG(LogOliveUtilityModel, Warning,
			TEXT("Utility model failed: %s -- trying main provider fallback"),
			*OutError);
	}

	// Tier 2: Try the main chat provider
	if (Settings->Provider == EOliveAIProvider::ClaudeCode)
	{
		// ClaudeCode is a CLI -- use --print mode for one-shot completion
		UE_LOG(LogOliveUtilityModel, Log,
			TEXT("Attempting CLI completion via Claude Code --print mode"));

		FString CLIError;
		if (TrySendCompletionViaCLI(
			Settings->UtilityModelTimeoutSeconds,
			SystemPrompt,
			UserPrompt,
			OutResponse,
			CLIError))
		{
			return true;
		}

		UE_LOG(LogOliveUtilityModel, Warning,
			TEXT("CLI completion failed: %s"), *CLIError);
		OutError = CLIError;
	}
	else
	{
		FString MainKey = Settings->GetCurrentApiKey().TrimStartAndEnd();
		const bool bIsOllama = (Settings->Provider == EOliveAIProvider::Ollama);
		const bool bIsOpenAICompat = (Settings->Provider == EOliveAIProvider::OpenAICompatible);

		// Ollama doesn't need an API key; OpenAI Compatible needs a URL
		const bool bMainHasAuth = !MainKey.IsEmpty() || bIsOllama ||
			(bIsOpenAICompat && !Settings->GetCurrentBaseUrl().TrimStartAndEnd().IsEmpty());

		if (bMainHasAuth)
		{
			const FString MainModel = Settings->GetSelectedModelForProvider(Settings->Provider);
			if (!MainModel.IsEmpty())
			{
				UE_LOG(LogOliveUtilityModel, Log,
					TEXT("Attempting main provider fallback via %s (model: %s)"),
					*ProviderEnumToName(Settings->Provider),
					*MainModel);

				FString FallbackError;
				if (TrySendCompletion(
					Settings->Provider,
					MainModel,
					MainKey,
					Settings->GetCurrentBaseUrl(),
					Settings->UtilityModelTimeoutSeconds,
					SystemPrompt,
					UserPrompt,
					OutResponse,
					FallbackError))
				{
					UE_LOG(LogOliveUtilityModel, Log,
						TEXT("Main provider fallback succeeded (%d chars)"),
						OutResponse.Len());
					return true;
				}

				UE_LOG(LogOliveUtilityModel, Warning,
					TEXT("Main provider fallback also failed: %s"),
					*FallbackError);
				OutError = FallbackError;
			}
		}
	}

	// All tiers failed
	if (OutError.IsEmpty())
	{
		OutError = TEXT("No utility or main model provider available for simple completion");
	}
	return false;
}

// ============================================================================
// GetActionVerbStopWords
// ============================================================================

const TSet<FString>& FOliveUtilityModel::GetActionVerbStopWords()
{
	static const TSet<FString> StopWords = {
		TEXT("create"), TEXT("build"), TEXT("make"), TEXT("add"),
		TEXT("implement"), TEXT("write"), TEXT("modify"), TEXT("change"),
		TEXT("update"), TEXT("fix"), TEXT("remove"), TEXT("delete"),
		TEXT("set"), TEXT("get"), TEXT("wire"), TEXT("connect"),
		TEXT("please"), TEXT("want"), TEXT("need"), TEXT("should"),
		TEXT("using"), TEXT("blueprint"), TEXT("graph"), TEXT("system")
	};
	return StopWords;
}

// ============================================================================
// BuildKeywordExpansionPrompt
// ============================================================================

FString FOliveUtilityModel::BuildKeywordExpansionPrompt()
{
	return TEXT(
		"You are a search keyword generator for an Unreal Engine Blueprint template library.\n"
		"Given a user's task description, output 8-12 search keywords that would find relevant\n"
		"Blueprint templates. Include:\n"
		"- Direct terms from the task\n"
		"- UE5 synonyms (e.g., \"gun\" -> \"weapon\", \"fire\", \"projectile\", \"ammo\")\n"
		"- Related Blueprint concepts (e.g., \"door\" -> \"interactable\", \"overlap\", \"timeline\")\n"
		"- Component types (e.g., \"health\" -> \"stat\", \"damage\", \"combat\")\n"
		"Output ONLY a comma-separated list of lowercase keywords. No explanations."
	);
}

// ============================================================================
// ExtractKeywordsBasic (Fallback)
// ============================================================================

TArray<FString> FOliveUtilityModel::ExtractKeywordsBasic(
	const FString& UserMessage,
	int32 MaxKeywords)
{
	// Strip @-mentions: replace "@" with " "
	FString Cleaned = UserMessage.Replace(TEXT("@"), TEXT(" "));

	// Tokenize using the library index's public static tokenizer
	// (splits on spaces/underscores/hyphens, lowercases, drops < 2 chars, drops stop words)
	TArray<FString> Tokens = FOliveLibraryIndex::Tokenize(Cleaned);

	// Remove action verb stop words
	const TSet<FString>& StopWords = GetActionVerbStopWords();
	Tokens.RemoveAll([&StopWords](const FString& Token)
	{
		return StopWords.Contains(Token);
	});

	// Cap at MaxKeywords
	if (Tokens.Num() > MaxKeywords)
	{
		Tokens.SetNum(MaxKeywords);
	}

	return Tokens;
}

// ============================================================================
// ExtractSearchKeywords
// ============================================================================

TArray<FString> FOliveUtilityModel::ExtractSearchKeywords(
	const FString& UserMessage,
	int32 MaxKeywords)
{
	if (UserMessage.TrimStartAndEnd().IsEmpty())
	{
		return TArray<FString>();
	}

	// Tier 1: Try LLM-based keyword expansion
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (Settings && Settings->bEnableLLMKeywordExpansion)
	{
		FString LLMResponse;
		FString LLMError;

		if (SendSimpleCompletion(
			BuildKeywordExpansionPrompt(),
			UserMessage,
			LLMResponse,
			LLMError))
		{
			// Parse LLM response: split on commas and newlines, lowercase, trim, dedup
			TArray<FString> RawTokens;
			LLMResponse.ParseIntoArray(RawTokens, TEXT(","));

			// Also split on newlines in case the model used them
			TArray<FString> Expanded;
			for (const FString& Token : RawTokens)
			{
				TArray<FString> SubTokens;
				Token.ParseIntoArray(SubTokens, TEXT("\n"));
				for (const FString& Sub : SubTokens)
				{
					FString Trimmed = Sub.TrimStartAndEnd().ToLower();
					// Remove any surrounding quotes
					Trimmed.ReplaceInline(TEXT("\""), TEXT(""));
					Trimmed.ReplaceInline(TEXT("'"), TEXT(""));
					Trimmed = Trimmed.TrimStartAndEnd();

					if (!Trimmed.IsEmpty() && Trimmed.Len() >= 2)
					{
						Expanded.AddUnique(Trimmed);
					}
				}
			}

			// Cap at MaxKeywords
			if (Expanded.Num() > MaxKeywords)
			{
				Expanded.SetNum(MaxKeywords);
			}

			// If we got at least 3 meaningful keywords, use them
			if (Expanded.Num() >= 3)
			{
				UE_LOG(LogOliveUtilityModel, Log,
					TEXT("LLM keyword expansion produced %d keywords"), Expanded.Num());
				return Expanded;
			}

			UE_LOG(LogOliveUtilityModel, Warning,
				TEXT("LLM returned only %d keywords (need >= 3), falling back to basic tokenizer"),
				Expanded.Num());
		}
		else
		{
			UE_LOG(LogOliveUtilityModel, Log,
				TEXT("LLM keyword expansion failed (%s), falling back to basic tokenizer"),
				*LLMError);
		}
	}

	// Tier 2: Basic tokenizer fallback
	return ExtractKeywordsBasic(UserMessage, MaxKeywords);
}
