// Copyright Bode Software. All Rights Reserved.

/**
 * OliveUtilityModel.cpp — Lightweight static helper for quick single-shot LLM completions.
 *
 * Implements the 3-tier fallback chain (utility provider → main provider → CLI)
 * and the keyword extraction pipeline (LLM expansion → basic tokenizer).
 */

#include "Services/OliveUtilityModel.h"
#include "Settings/OliveAISettings.h"
#include "Providers/IOliveAIProvider.h"
#include "Providers/OliveClaudeCodeProvider.h"
#include "Template/OliveTemplateSystem.h"
#include "MCP/OliveToolRegistry.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveUtilityModel, Log, All);

// =============================================================================
// SendSimpleCompletion — 3-tier fallback
// =============================================================================

bool FOliveUtilityModel::SendSimpleCompletion(
	const FString& SystemPrompt,
	const FString& UserPrompt,
	FString& OutResponse,
	FString& OutError)
{
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		OutError = TEXT("OliveAISettings not available");
		return false;
	}

	const float Timeout = static_cast<float>(Settings->UtilityModelTimeoutSeconds);

	// --- Tier 1: Configured utility model provider ---
	{
		const EOliveAIProvider UtilProvider = Settings->UtilityModelProvider;
		const FString& ModelId = Settings->UtilityModelId;

		// Resolve API key: use dedicated utility key if set, otherwise fall back to
		// the main provider's key if the utility provider type matches.
		FString ApiKey = Settings->UtilityModelApiKey;
		if (ApiKey.IsEmpty())
		{
			// Try to borrow the main provider's key if it's the same provider type
			if (UtilProvider == Settings->Provider)
			{
				ApiKey = Settings->GetCurrentApiKey();
			}
			else
			{
				// Look up the key from the per-provider settings fields
				switch (UtilProvider)
				{
				case EOliveAIProvider::OpenRouter:    ApiKey = Settings->OpenRouterApiKey; break;
				case EOliveAIProvider::ZAI:           ApiKey = Settings->ZaiApiKey; break;
				case EOliveAIProvider::Anthropic:     ApiKey = Settings->AnthropicApiKey; break;
				case EOliveAIProvider::OpenAI:        ApiKey = Settings->OpenAIApiKey; break;
				case EOliveAIProvider::Google:        ApiKey = Settings->GoogleApiKey; break;
				case EOliveAIProvider::Ollama:        ApiKey = TEXT(""); break; // No key needed
				case EOliveAIProvider::OpenAICompatible: ApiKey = Settings->OpenAICompatibleApiKey; break;
				default: break;
				}
			}
		}

		// Resolve base URL for the utility provider
		FString BaseUrl;
		switch (UtilProvider)
		{
		case EOliveAIProvider::OpenRouter:
			BaseUrl = TEXT("https://openrouter.ai/api/v1/chat/completions");
			break;
		case EOliveAIProvider::ZAI:
			BaseUrl = Settings->bZaiUseCodingEndpoint
				? TEXT("https://api.z.ai/api/coding/paas/v4")
				: TEXT("https://api.z.ai/api/paas/v4");
			break;
		case EOliveAIProvider::Anthropic:
			BaseUrl = TEXT("https://api.anthropic.com/v1/messages");
			break;
		case EOliveAIProvider::OpenAI:
			BaseUrl = TEXT("https://api.openai.com/v1/chat/completions");
			break;
		case EOliveAIProvider::Google:
			BaseUrl = TEXT("https://generativelanguage.googleapis.com/v1beta/models/");
			break;
		case EOliveAIProvider::Ollama:
			BaseUrl = Settings->OllamaUrl;
			break;
		case EOliveAIProvider::OpenAICompatible:
			BaseUrl = Settings->OpenAICompatibleUrl;
			break;
		default:
			break;
		}

		// Skip CLI providers as tier-1 (handled in tier 3)
		const bool bIsHTTPProvider = (UtilProvider != EOliveAIProvider::ClaudeCode
			&& UtilProvider != EOliveAIProvider::Codex);
		const bool bHasCredentials = !ApiKey.IsEmpty()
			|| UtilProvider == EOliveAIProvider::Ollama
			|| UtilProvider == EOliveAIProvider::OpenAICompatible;

		if (bIsHTTPProvider && bHasCredentials && !ModelId.IsEmpty())
		{
			FString Tier1Error;
			if (TrySendCompletion(UtilProvider, ModelId, ApiKey, BaseUrl, Timeout,
				SystemPrompt, UserPrompt, OutResponse, Tier1Error))
			{
				return true;
			}
			UE_LOG(LogOliveUtilityModel, Verbose,
				TEXT("Utility model tier 1 (%s) failed: %s"),
				*ProviderEnumToName(UtilProvider), *Tier1Error);
		}
	}

	// --- Tier 2: Main chat provider (if HTTP-based) ---
	{
		const EOliveAIProvider MainProvider = Settings->Provider;

		if (MainProvider != EOliveAIProvider::ClaudeCode && MainProvider != EOliveAIProvider::Codex)
		{
			const FString MainApiKey = Settings->GetCurrentApiKey();
			const FString MainBaseUrl = Settings->GetCurrentBaseUrl();
			const FString MainModel = Settings->GetSelectedModelForProvider(MainProvider);

			const bool bMainHasCredentials = !MainApiKey.IsEmpty()
				|| MainProvider == EOliveAIProvider::Ollama
				|| MainProvider == EOliveAIProvider::OpenAICompatible;

			if (bMainHasCredentials && !MainModel.IsEmpty())
			{
				FString Tier2Error;
				if (TrySendCompletion(MainProvider, MainModel, MainApiKey, MainBaseUrl, Timeout,
					SystemPrompt, UserPrompt, OutResponse, Tier2Error))
				{
					return true;
				}
				UE_LOG(LogOliveUtilityModel, Verbose,
					TEXT("Utility model tier 2 (%s) failed: %s"),
					*ProviderEnumToName(MainProvider), *Tier2Error);
			}
		}
	}

	// --- Tier 3: Claude Code CLI --print ---
	{
		if (FOliveClaudeCodeProvider::IsClaudeCodeInstalled())
		{
			FString Tier3Error;
			if (TrySendCompletionViaCLI(Timeout, SystemPrompt, UserPrompt, OutResponse, Tier3Error))
			{
				return true;
			}
			UE_LOG(LogOliveUtilityModel, Verbose,
				TEXT("Utility model tier 3 (Claude CLI) failed: %s"), *Tier3Error);
		}
	}

	OutError = TEXT("All utility model tiers exhausted. Configure an API key in Olive AI settings, or install Claude Code CLI.");
	return false;
}

// =============================================================================
// IsAvailable
// =============================================================================

bool FOliveUtilityModel::IsAvailable()
{
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings)
	{
		return false;
	}

	// Tier 1: Utility provider configured?
	{
		const EOliveAIProvider UtilProvider = Settings->UtilityModelProvider;
		if (UtilProvider != EOliveAIProvider::ClaudeCode && UtilProvider != EOliveAIProvider::Codex)
		{
			FString ApiKey = Settings->UtilityModelApiKey;
			if (ApiKey.IsEmpty())
			{
				// Check if the matching provider field has a key
				switch (UtilProvider)
				{
				case EOliveAIProvider::OpenRouter:    ApiKey = Settings->OpenRouterApiKey; break;
				case EOliveAIProvider::ZAI:           ApiKey = Settings->ZaiApiKey; break;
				case EOliveAIProvider::Anthropic:     ApiKey = Settings->AnthropicApiKey; break;
				case EOliveAIProvider::OpenAI:        ApiKey = Settings->OpenAIApiKey; break;
				case EOliveAIProvider::Google:        ApiKey = Settings->GoogleApiKey; break;
				case EOliveAIProvider::Ollama:        return true; // No key needed
				case EOliveAIProvider::OpenAICompatible: ApiKey = Settings->OpenAICompatibleApiKey; break;
				default: break;
				}
			}
			if (!ApiKey.IsEmpty() && !Settings->UtilityModelId.IsEmpty())
			{
				return true;
			}
		}
	}

	// Tier 2: Main provider is HTTP-based?
	if (Settings->Provider != EOliveAIProvider::ClaudeCode
		&& Settings->Provider != EOliveAIProvider::Codex
		&& Settings->IsProviderConfigured())
	{
		return true;
	}

	// Tier 3: Claude Code CLI installed?
	if (FOliveClaudeCodeProvider::IsClaudeCodeInstalled())
	{
		return true;
	}

	return false;
}

// =============================================================================
// TrySendCompletion — HTTP provider path
// =============================================================================

bool FOliveUtilityModel::TrySendCompletion(
	EOliveAIProvider ProviderType,
	const FString& ModelId,
	const FString& ApiKey,
	const FString& BaseUrl,
	float Timeout,
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
		OutError = FString::Printf(TEXT("Failed to create provider: %s"), *ProviderName);
		return false;
	}

	// Configure the provider for a quick, deterministic completion
	FOliveProviderConfig Config;
	Config.ProviderName = ProviderName;
	Config.ApiKey = ApiKey;
	Config.ModelId = ModelId;
	Config.BaseUrl = BaseUrl;
	Config.Temperature = 0.0f;
	Config.MaxTokens = 256;
	Config.TimeoutSeconds = FMath::CeilToInt32(Timeout);
	Provider->Configure(Config);

	// Build messages array: system + user
	TArray<FOliveChatMessage> Messages;

	if (!SystemPrompt.IsEmpty())
	{
		FOliveChatMessage SystemMsg;
		SystemMsg.Role = EOliveChatRole::System;
		SystemMsg.Content = SystemPrompt;
		SystemMsg.Timestamp = FDateTime::Now();
		Messages.Add(MoveTemp(SystemMsg));
	}

	{
		FOliveChatMessage UserMsg;
		UserMsg.Role = EOliveChatRole::User;
		UserMsg.Content = UserPrompt;
		UserMsg.Timestamp = FDateTime::Now();
		Messages.Add(MoveTemp(UserMsg));
	}

	// Request options override for utility model
	FOliveRequestOptions Options;
	Options.MaxTokens = 256;
	Options.Temperature = 0.0f;
	Options.TimeoutSeconds = FMath::CeilToInt32(Timeout);

	// Blocking completion with tick-pumping
	bool bCompleted = false;
	bool bSuccess = false;
	FString ResponseText;
	FString ErrorText;

	Provider->SendMessage(
		Messages,
		TArray<FOliveToolDefinition>(), // No tools
		// OnChunk — accumulate text
		FOnOliveStreamChunk::CreateLambda([&ResponseText](const FOliveStreamChunk& Chunk)
		{
			if (!Chunk.bIsToolCall && !Chunk.Text.IsEmpty())
			{
				ResponseText += Chunk.Text;
			}
		}),
		// OnToolCall — not expected for utility completions
		FOnOliveToolCall::CreateLambda([](const FOliveStreamChunk& /*ToolCall*/) {}),
		// OnComplete
		FOnOliveComplete::CreateLambda([&bCompleted, &bSuccess](const FString& /*FullResponse*/, const FOliveProviderUsage& /*Usage*/)
		{
			bSuccess = true;
			bCompleted = true;
		}),
		// OnError
		FOnOliveError::CreateLambda([&bCompleted, &ErrorText](const FString& Error)
		{
			ErrorText = Error;
			bCompleted = true;
		}),
		Options
	);

	// Pump the ticker until completion or timeout
	const double StartTime = FPlatformTime::Seconds();
	const double TimeoutLimit = static_cast<double>(Timeout);

	while (!bCompleted)
	{
		const double Elapsed = FPlatformTime::Seconds() - StartTime;
		if (Elapsed >= TimeoutLimit)
		{
			Provider->CancelRequest();
			OutError = FString::Printf(TEXT("Utility model timed out after %.1fs"), Elapsed);
			return false;
		}

		FTSTicker::GetCoreTicker().Tick(0.01f);
		FPlatformProcess::Sleep(0.01f);
	}

	if (bSuccess && !ResponseText.IsEmpty())
	{
		OutResponse = ResponseText.TrimStartAndEnd();
		return true;
	}

	OutError = ErrorText.IsEmpty() ? TEXT("Empty response from provider") : ErrorText;
	return false;
}

// =============================================================================
// TrySendCompletionViaCLI — Claude Code --print fallback
// =============================================================================

bool FOliveUtilityModel::TrySendCompletionViaCLI(
	float Timeout,
	const FString& SystemPrompt,
	const FString& UserPrompt,
	FString& OutResponse,
	FString& OutError)
{
	const FString ClaudePath = FOliveClaudeCodeProvider::GetClaudeExecutablePath();
	if (ClaudePath.IsEmpty())
	{
		OutError = TEXT("Claude Code CLI executable not found");
		return false;
	}

	// Build the combined prompt (system + user)
	FString CombinedPrompt;
	if (!SystemPrompt.IsEmpty())
	{
		CombinedPrompt = SystemPrompt + TEXT("\n\n") + UserPrompt;
	}
	else
	{
		CombinedPrompt = UserPrompt;
	}

	// Escape double quotes for command line
	FString EscapedPrompt = CombinedPrompt.Replace(TEXT("\""), TEXT("\\\""));

	// Determine if this is a Node.js script (rather than a direct binary)
	const bool bIsJs = ClaudePath.EndsWith(TEXT(".js")) || ClaudePath.EndsWith(TEXT(".mjs"));

	FString StdOut;
	FString StdErr;
	int32 ReturnCode = -1;

	// Build arguments
	const FString Args = FString::Printf(
		TEXT("--print --output-format text --max-turns 1 \"%s\""),
		*EscapedPrompt);

	// NOTE: FPlatformProcess::ExecProcess does not have a timeout parameter in UE 5.5.
	// The --max-turns 1 flag limits Claude's work, and the CLI itself has internal timeouts.
	// For true timeout enforcement, a CreateProc + async read approach would be needed.
	bool bLaunched = false;
	if (bIsJs)
	{
		// Launch via node
		const FString FullArgs = FString::Printf(TEXT("\"%s\" %s"), *ClaudePath, *Args);
		bLaunched = FPlatformProcess::ExecProcess(
			TEXT("node"),
			*FullArgs,
			&ReturnCode,
			&StdOut,
			&StdErr);
	}
	else
	{
		bLaunched = FPlatformProcess::ExecProcess(
			*ClaudePath,
			*Args,
			&ReturnCode,
			&StdOut,
			&StdErr);
	}

	if (!bLaunched)
	{
		OutError = TEXT("Failed to launch Claude Code CLI process");
		return false;
	}

	if (ReturnCode != 0)
	{
		OutError = FString::Printf(
			TEXT("Claude CLI exited with code %d: %s"),
			ReturnCode,
			StdErr.IsEmpty() ? TEXT("(no stderr)") : *StdErr.Left(500));
		return false;
	}

	const FString Trimmed = StdOut.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OutError = TEXT("Claude CLI returned empty output");
		return false;
	}

	OutResponse = Trimmed;
	return true;
}

// =============================================================================
// ExtractSearchKeywords — LLM expansion with basic fallback
// =============================================================================

TArray<FString> FOliveUtilityModel::ExtractSearchKeywords(const FString& UserMessage, int32 MaxKeywords)
{
	if (UserMessage.IsEmpty())
	{
		return TArray<FString>();
	}

	const UOliveAISettings* Settings = UOliveAISettings::Get();
	const bool bUseLLM = Settings && Settings->bEnableLLMKeywordExpansion;

	// Try LLM keyword expansion first
	if (bUseLLM)
	{
		FString Response;
		FString Error;
		const FString SystemPrompt = BuildKeywordExpansionPrompt();

		if (SendSimpleCompletion(SystemPrompt, UserMessage, Response, Error))
		{
			// Parse comma-separated keywords
			TArray<FString> Keywords;
			Response.ParseIntoArray(Keywords, TEXT(","), true);

			// Clean up each keyword
			TArray<FString> CleanKeywords;
			CleanKeywords.Reserve(Keywords.Num());

			for (FString& Keyword : Keywords)
			{
				Keyword = Keyword.TrimStartAndEnd().ToLower();
				// Remove any trailing periods, semicolons, etc.
				while (Keyword.Len() > 0 && !FChar::IsAlnum(Keyword[Keyword.Len() - 1]) && Keyword[Keyword.Len() - 1] != '_')
				{
					Keyword.LeftChopInline(1);
				}
				if (Keyword.Len() >= 2)
				{
					CleanKeywords.AddUnique(Keyword);
				}
			}

			// Accept LLM results if we got at least 3 keywords
			if (CleanKeywords.Num() >= 3)
			{
				if (CleanKeywords.Num() > MaxKeywords)
				{
					CleanKeywords.SetNum(MaxKeywords);
				}
				return CleanKeywords;
			}

			UE_LOG(LogOliveUtilityModel, Verbose,
				TEXT("LLM keyword extraction returned only %d keywords, falling back to basic"),
				CleanKeywords.Num());
		}
		else
		{
			UE_LOG(LogOliveUtilityModel, Verbose,
				TEXT("LLM keyword extraction failed: %s — falling back to basic"), *Error);
		}
	}

	// Fallback: basic tokenizer
	return ExtractKeywordsBasic(UserMessage, MaxKeywords);
}

// =============================================================================
// ExtractKeywordsBasic — Pure string processing, no LLM
// =============================================================================

TArray<FString> FOliveUtilityModel::ExtractKeywordsBasic(const FString& UserMessage, int32 MaxKeywords)
{
	// Tokenize using FOliveLibraryIndex's tokenizer (splits on spaces/underscores/hyphens,
	// lowercases, filters tokens < 2 chars)
	TArray<FString> Tokens = FOliveLibraryIndex::Tokenize(UserMessage);

	// Remove action verb stop words
	const TSet<FString>& StopWords = GetActionVerbStopWords();
	TArray<FString> Filtered;
	Filtered.Reserve(Tokens.Num());

	for (const FString& Token : Tokens)
	{
		if (!StopWords.Contains(Token))
		{
			Filtered.AddUnique(Token);
		}
	}

	// Clamp to max
	if (Filtered.Num() > MaxKeywords)
	{
		Filtered.SetNum(MaxKeywords);
	}

	return Filtered;
}

// =============================================================================
// BuildKeywordExpansionPrompt
// =============================================================================

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

// =============================================================================
// GetActionVerbStopWords
// =============================================================================

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

// =============================================================================
// ProviderEnumToName — Maps EOliveAIProvider to FOliveProviderFactory string
// =============================================================================

FString FOliveUtilityModel::ProviderEnumToName(EOliveAIProvider Provider)
{
	switch (Provider)
	{
	case EOliveAIProvider::ClaudeCode:        return TEXT("Claude Code CLI");
	case EOliveAIProvider::Codex:             return TEXT("Codex CLI");
	case EOliveAIProvider::OpenRouter:        return TEXT("OpenRouter");
	case EOliveAIProvider::ZAI:               return TEXT("Z.ai");
	case EOliveAIProvider::Anthropic:         return TEXT("Anthropic");
	case EOliveAIProvider::OpenAI:            return TEXT("OpenAI");
	case EOliveAIProvider::Google:            return TEXT("Google");
	case EOliveAIProvider::Ollama:            return TEXT("Ollama");
	case EOliveAIProvider::OpenAICompatible:  return TEXT("OpenAI Compatible");
	default:                                  return TEXT("Unknown");
	}
}

// =============================================================================
// BuildDiscoverySearchPrompt
// =============================================================================

FString FOliveUtilityModel::BuildDiscoverySearchPrompt()
{
	return TEXT(
		"You generate search queries for an Unreal Engine Blueprint template library.\n"
		"The library contains 325+ extracted Blueprint templates from a real combat game covering:\n"
		"melee weapons (swords, axes, shields), ranged weapons (bows, arrows, guns),\n"
		"magic/abilities, projectiles, components (stat, combat, ranged, melee),\n"
		"damage systems, AI behaviors, inventory, and character mechanics.\n\n"
		"Given a user's task, output 3-5 search queries (one per line) that would find\n"
		"relevant templates. Each query should be 2-3 words targeting a different facet:\n"
		"- The specific item/weapon type\n"
		"- The component/system category\n"
		"- Related mechanics (damage, ammo, aiming, etc.)\n"
		"- UE patterns (projectile, overlap, trace, etc.)\n\n"
		"Output ONLY the queries, one per line. No numbering, no explanations."
	);
}

// =============================================================================
// GenerateSearchQueries — LLM smart queries with basic fallback
// =============================================================================

TArray<FString> FOliveUtilityModel::GenerateSearchQueries(const FString& UserMessage)
{
	TArray<FString> Queries;

	const UOliveAISettings* Settings = UOliveAISettings::Get();
	const bool bUseLLM = Settings && Settings->bEnableLLMKeywordExpansion;

	if (bUseLLM)
	{
		FString Response;
		FString Error;
		const FString SystemPrompt = BuildDiscoverySearchPrompt();

		if (SendSimpleCompletion(SystemPrompt, UserMessage, Response, Error))
		{
			// Parse response: one query per line
			TArray<FString> Lines;
			Response.ParseIntoArrayLines(Lines);

			for (FString& Line : Lines)
			{
				Line = Line.TrimStartAndEnd();
				// Strip leading numbering like "1. " or "- "
				if (Line.Len() > 2 && FChar::IsDigit(Line[0]) && Line[1] == '.')
				{
					Line = Line.Mid(2).TrimStartAndEnd();
				}
				else if (Line.Len() > 1 && (Line[0] == '-' || Line[0] == '*'))
				{
					Line = Line.Mid(1).TrimStartAndEnd();
				}

				if (Line.Len() >= 3)
				{
					Queries.Add(Line);
				}
			}

			// Take at most 5
			if (Queries.Num() > 5)
			{
				Queries.SetNum(5);
			}

			if (Queries.Num() >= 2)
			{
				UE_LOG(LogOliveUtilityModel, Verbose,
					TEXT("Discovery LLM generated %d search queries"), Queries.Num());
				return Queries;
			}

			UE_LOG(LogOliveUtilityModel, Verbose,
				TEXT("Discovery LLM returned only %d queries, falling back to basic"),
				Queries.Num());
			Queries.Reset();
		}
		else
		{
			UE_LOG(LogOliveUtilityModel, Verbose,
				TEXT("Discovery LLM failed: %s — falling back to basic"), *Error);
		}
	}

	// Fallback: extract basic keywords and pair them into multi-word queries
	TArray<FString> Keywords = ExtractKeywordsBasic(UserMessage, 8);
	if (Keywords.Num() == 0)
	{
		return Queries;
	}

	// Pair keywords: [bow, arrow, ranged, projectile] -> ["bow arrow", "ranged projectile"]
	for (int32 i = 0; i < Keywords.Num(); i += 2)
	{
		if (i + 1 < Keywords.Num())
		{
			Queries.Add(Keywords[i] + TEXT(" ") + Keywords[i + 1]);
		}
		else
		{
			Queries.Add(Keywords[i]);
		}
	}

	// Ensure at least 2 queries, at most 5
	if (Queries.Num() > 5)
	{
		Queries.SetNum(5);
	}

	return Queries;
}

// =============================================================================
// ScoreEntry — Internal scoring for discovery results
// =============================================================================

void FOliveUtilityModel::ScoreEntry(FOliveDiscoveryEntry& Entry, int32 QueryHitCount)
{
	// Base score by source type
	if (Entry.SourceType == TEXT("library"))
	{
		Entry.InternalScore += 10;
	}
	else if (Entry.SourceType == TEXT("factory"))
	{
		Entry.InternalScore += 6;
	}
	else if (Entry.SourceType == TEXT("reference"))
	{
		Entry.InternalScore += 5;
	}
	else
	{
		Entry.InternalScore += 3; // community
	}

	// Matched functions signal relevance
	Entry.InternalScore += Entry.MatchedFunctions.Num() * 2;

	// Multi-query hits signal strong relevance
	if (QueryHitCount > 1)
	{
		Entry.InternalScore += (QueryHitCount - 1) * 3;
	}

	// Completeness signal
	if (Entry.TotalFunctions > 5)
	{
		Entry.InternalScore += 1;
	}
}

// =============================================================================
// RunDiscoveryPass — Orchestrates template discovery
// =============================================================================

FOliveDiscoveryResult FOliveUtilityModel::RunDiscoveryPass(const FString& UserMessage, int32 MaxResults)
{
	FOliveDiscoveryResult Result;
	const double StartTime = FPlatformTime::Seconds();

	// Generate search queries
	Result.SearchQueries = GenerateSearchQueries(UserMessage);
	Result.bUsedLLM = false;

	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (Settings && Settings->bEnableLLMKeywordExpansion)
	{
		// If we got queries and LLM is enabled, the LLM path was attempted
		// (GenerateSearchQueries handles fallback internally — check if queries
		// look like multi-word LLM output vs paired keywords)
		for (const FString& Query : Result.SearchQueries)
		{
			if (Query.Contains(TEXT(" ")) && Query.Len() > 6)
			{
				Result.bUsedLLM = true;
				break;
			}
		}
	}

	if (Result.SearchQueries.Num() == 0)
	{
		Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
		return Result;
	}

	// Merge results across all queries, tracking hit counts
	TMap<FString, FOliveDiscoveryEntry> MergedEntries;
	TMap<FString, int32> QueryHitCounts;

	for (const FString& Query : Result.SearchQueries)
	{
		// --- Search library/factory/reference templates ---
		TArray<TSharedPtr<FJsonObject>> SearchResults =
			FOliveTemplateSystem::Get().SearchTemplates(Query, 10);

		for (const TSharedPtr<FJsonObject>& ResultObj : SearchResults)
		{
			if (!ResultObj.IsValid())
			{
				continue;
			}

			const FString TemplateId = ResultObj->GetStringField(TEXT("template_id"));
			if (TemplateId.IsEmpty())
			{
				continue;
			}

			// Track how many queries hit this entry
			QueryHitCounts.FindOrAdd(TemplateId)++;

			// Only create the entry once
			if (MergedEntries.Contains(TemplateId))
			{
				// Merge matched functions from additional queries
				FOliveDiscoveryEntry& Existing = MergedEntries[TemplateId];
				const TArray<TSharedPtr<FJsonValue>>* MatchedFuncsArray = nullptr;
				if (ResultObj->TryGetArrayField(TEXT("matched_functions"), MatchedFuncsArray))
				{
					for (const TSharedPtr<FJsonValue>& FuncVal : *MatchedFuncsArray)
					{
						const TSharedPtr<FJsonObject>* FuncObj = nullptr;
						if (FuncVal->TryGetObject(FuncObj) && (*FuncObj).IsValid())
						{
							FString FuncName = (*FuncObj)->GetStringField(TEXT("name"));
							if (!FuncName.IsEmpty())
							{
								Existing.MatchedFunctions.AddUnique(FuncName);
							}
						}
					}
				}
				continue;
			}

			// Build new entry
			FOliveDiscoveryEntry Entry;
			Entry.TemplateId = TemplateId;
			Entry.DisplayName = ResultObj->GetStringField(TEXT("display_name"));
			Entry.SourceType = ResultObj->GetStringField(TEXT("type"));
			Entry.Description = ResultObj->GetStringField(TEXT("catalog_description"));
			Entry.ParentClass = ResultObj->GetStringField(TEXT("parent_class"));
			Entry.TotalFunctions = static_cast<int32>(
				ResultObj->GetNumberField(TEXT("function_count")));

			// Extract matched function names
			const TArray<TSharedPtr<FJsonValue>>* MatchedFuncsArray = nullptr;
			if (ResultObj->TryGetArrayField(TEXT("matched_functions"), MatchedFuncsArray))
			{
				for (const TSharedPtr<FJsonValue>& FuncVal : *MatchedFuncsArray)
				{
					const TSharedPtr<FJsonObject>* FuncObj = nullptr;
					if (FuncVal->TryGetObject(FuncObj) && (*FuncObj).IsValid())
					{
						FString FuncName = (*FuncObj)->GetStringField(TEXT("name"));
						if (!FuncName.IsEmpty())
						{
							Entry.MatchedFunctions.AddUnique(FuncName);
						}
					}
				}
			}

			if (Entry.TotalFunctions == 0 && Entry.MatchedFunctions.Num() > 0)
			{
				Entry.TotalFunctions = Entry.MatchedFunctions.Num();
			}

			MergedEntries.Add(TemplateId, MoveTemp(Entry));
		}

		// --- Search community blueprints (graceful skip if tool not registered) ---
		{
			TSharedPtr<FJsonObject> CommunityParams = MakeShared<FJsonObject>();
			CommunityParams->SetStringField(TEXT("query"), Query);
			CommunityParams->SetNumberField(TEXT("max_results"), 5);

			FOliveToolResult ToolResult =
				FOliveToolRegistry::Get().ExecuteTool(
					TEXT("olive.search_community_blueprints"), CommunityParams);

			if (ToolResult.bSuccess && ToolResult.Data.IsValid())
			{
				// The tool result Data should contain a "results" array
				const TArray<TSharedPtr<FJsonValue>>* ResultsArray = nullptr;
				if (ToolResult.Data->TryGetArrayField(TEXT("results"), ResultsArray))
				{
					for (const TSharedPtr<FJsonValue>& Val : *ResultsArray)
					{
						const TSharedPtr<FJsonObject>* ItemObj = nullptr;
						if (!Val->TryGetObject(ItemObj) || !(*ItemObj).IsValid())
						{
							continue;
						}

						FString Slug;
						(*ItemObj)->TryGetStringField(TEXT("id"), Slug);
						if (Slug.IsEmpty())
						{
							// Fallback: derive slug from title
							FString Title;
							(*ItemObj)->TryGetStringField(TEXT("title"), Title);
							Slug = Title.ToLower().Replace(TEXT(" "), TEXT("-"));
						}

						if (Slug.IsEmpty())
						{
							continue;
						}

						QueryHitCounts.FindOrAdd(Slug)++;

						if (MergedEntries.Contains(Slug))
						{
							continue;
						}

						FOliveDiscoveryEntry Entry;
						Entry.TemplateId = Slug;
						(*ItemObj)->TryGetStringField(TEXT("title"), Entry.DisplayName);
						Entry.SourceType = TEXT("community");

						FString Desc;
						(*ItemObj)->TryGetStringField(TEXT("description"), Desc);
						if (Desc.Len() > 100)
						{
							Desc = Desc.Left(97) + TEXT("...");
						}
						Entry.Description = Desc;

						Entry.TotalFunctions = 0; // Not applicable for community

						MergedEntries.Add(Slug, MoveTemp(Entry));
					}
				}
			}
			// If tool not registered or failed, silently skip
		}
	}

	// Score all entries
	for (auto& Pair : MergedEntries)
	{
		const int32* HitCount = QueryHitCounts.Find(Pair.Key);
		ScoreEntry(Pair.Value, HitCount ? *HitCount : 1);
	}

	// Collect and sort by score descending
	TArray<FOliveDiscoveryEntry> AllEntries;
	AllEntries.Reserve(MergedEntries.Num());
	for (auto& Pair : MergedEntries)
	{
		AllEntries.Add(MoveTemp(Pair.Value));
	}

	AllEntries.Sort([](const FOliveDiscoveryEntry& A, const FOliveDiscoveryEntry& B)
	{
		return A.InternalScore > B.InternalScore;
	});

	// Take top results
	if (AllEntries.Num() > MaxResults)
	{
		AllEntries.SetNum(MaxResults);
	}

	Result.Entries = MoveTemp(AllEntries);
	Result.ElapsedSeconds = FPlatformTime::Seconds() - StartTime;

	UE_LOG(LogOliveUtilityModel, Log,
		TEXT("Discovery pass: %d entries found, %d queries, %.2fs elapsed"),
		Result.Entries.Num(), Result.SearchQueries.Num(), Result.ElapsedSeconds);

	return Result;
}

// =============================================================================
// FormatDiscoveryForPrompt — Markdown output for agent prompt injection
// =============================================================================

FString FOliveUtilityModel::FormatDiscoveryForPrompt(const FOliveDiscoveryResult& Result)
{
	if (Result.Entries.Num() < 2)
	{
		return FString();
	}

	static constexpr int32 MaxOutputChars = 2000;

	// Group entries by source type
	TArray<const FOliveDiscoveryEntry*> LibraryEntries;
	TArray<const FOliveDiscoveryEntry*> FactoryEntries;
	TArray<const FOliveDiscoveryEntry*> CommunityEntries;

	for (const FOliveDiscoveryEntry& Entry : Result.Entries)
	{
		if (Entry.SourceType == TEXT("library"))
		{
			LibraryEntries.Add(&Entry);
		}
		else if (Entry.SourceType == TEXT("factory") || Entry.SourceType == TEXT("reference"))
		{
			FactoryEntries.Add(&Entry);
		}
		else
		{
			CommunityEntries.Add(&Entry);
		}
	}

	FString Output;
	Output.Reserve(MaxOutputChars);
	Output += TEXT("## Reference Templates Found\n\n");
	Output += TEXT("These templates may help with your task. Use `blueprint.get_template(id, pattern=\"FuncName\")` to study implementations.\n");

	// Library section
	if (LibraryEntries.Num() > 0)
	{
		Output += TEXT("\nLibrary (from CombatFS):\n");
		for (const FOliveDiscoveryEntry* Entry : LibraryEntries)
		{
			Output += TEXT("- ") + Entry->TemplateId + TEXT(": ");

			if (!Entry->ParentClass.IsEmpty())
			{
				// Label component templates clearly so the agent understands the architecture
				const bool bIsComponent = Entry->ParentClass.Contains(TEXT("Component"));
				if (bIsComponent)
				{
					Output += TEXT("[") + Entry->ParentClass + TEXT(" — attach to actor] ");
				}
				else
				{
					Output += Entry->ParentClass + TEXT(". ");
				}
			}

			if (Entry->MatchedFunctions.Num() > 0)
			{
				Output += TEXT("Functions: ");
				const int32 ShowCount = FMath::Min(Entry->MatchedFunctions.Num(), 4);
				for (int32 i = 0; i < ShowCount; i++)
				{
					if (i > 0) Output += TEXT(", ");
					Output += Entry->MatchedFunctions[i];
				}
				if (Entry->TotalFunctions > 0)
				{
					Output += FString::Printf(TEXT(" (%d total)"), Entry->TotalFunctions);
				}
			}
			else if (!Entry->Description.IsEmpty())
			{
				FString Desc = Entry->Description;
				if (Desc.Len() > 80)
				{
					Desc = Desc.Left(77) + TEXT("...");
				}
				Output += Desc;
			}

			Output += TEXT("\n");
		}
	}

	// Factory/Reference section
	if (FactoryEntries.Num() > 0)
	{
		Output += TEXT("\nFactory:\n");
		for (const FOliveDiscoveryEntry* Entry : FactoryEntries)
		{
			Output += TEXT("- ") + Entry->TemplateId + TEXT(": ");
			FString Desc = Entry->Description;
			if (Desc.Len() > 80)
			{
				Desc = Desc.Left(77) + TEXT("...");
			}
			Output += Desc + TEXT("\n");
		}
	}

	// Community section
	if (CommunityEntries.Num() > 0)
	{
		Output += TEXT("\nCommunity:\n");
		for (const FOliveDiscoveryEntry* Entry : CommunityEntries)
		{
			Output += TEXT("- ") + Entry->TemplateId + TEXT(": ");
			FString Desc = Entry->Description;
			if (Desc.Len() > 80)
			{
				Desc = Desc.Left(77) + TEXT("...");
			}
			Output += Desc + TEXT("\n");
		}
	}

	// Hard cap: trim community first, then factory if still over limit
	if (Output.Len() > MaxOutputChars)
	{
		// Rebuild without community
		CommunityEntries.Reset();
		FString Trimmed;
		Trimmed.Reserve(MaxOutputChars);
		Trimmed += TEXT("## Reference Templates Found\n\n");
		Trimmed += TEXT("These templates may help with your task. Use `blueprint.get_template(id, pattern=\"FuncName\")` to study implementations.\n");

		if (LibraryEntries.Num() > 0)
		{
			Trimmed += TEXT("\nLibrary (from CombatFS):\n");
			for (const FOliveDiscoveryEntry* Entry : LibraryEntries)
			{
				Trimmed += TEXT("- ") + Entry->TemplateId;
				if (!Entry->ParentClass.IsEmpty())
				{
					Trimmed += TEXT(": ") + Entry->ParentClass;
				}
				if (Entry->TotalFunctions > 0)
				{
					Trimmed += FString::Printf(TEXT(" (%d functions)"), Entry->TotalFunctions);
				}
				Trimmed += TEXT("\n");
			}
		}

		if (FactoryEntries.Num() > 0 && Trimmed.Len() < MaxOutputChars - 200)
		{
			Trimmed += TEXT("\nFactory:\n");
			for (const FOliveDiscoveryEntry* Entry : FactoryEntries)
			{
				Trimmed += TEXT("- ") + Entry->TemplateId + TEXT("\n");
			}
		}

		Output = MoveTemp(Trimmed);
	}

	return Output;
}
