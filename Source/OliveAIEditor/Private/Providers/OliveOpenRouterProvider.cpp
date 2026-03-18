// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveOpenRouterProvider.h"
#include "OliveAIEditorModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const FString FOliveOpenRouterProvider::OpenRouterApiUrl = TEXT("https://openrouter.ai/api/v1/chat/completions");
const FString FOliveOpenRouterProvider::DefaultModel = TEXT("anthropic/claude-sonnet-4");

FOliveOpenRouterProvider::FOliveOpenRouterProvider()
{
	Config.ProviderName = TEXT("openrouter");
	Config.BaseUrl = OpenRouterApiUrl;
	Config.ModelId = DefaultModel;
}

FOliveOpenRouterProvider::~FOliveOpenRouterProvider()
{
	*AliveFlag = false;
	CancelRequest();
}

// ==========================================
// Configuration
// ==========================================

TArray<FString> FOliveOpenRouterProvider::GetAvailableModels() const
{
	// Popular models on OpenRouter
	return {
		TEXT("anthropic/claude-sonnet-4"),
		TEXT("anthropic/claude-opus-4"),
		TEXT("anthropic/claude-3.5-sonnet"),
		TEXT("anthropic/claude-3-opus"),
		TEXT("openai/gpt-4o"),
		TEXT("openai/gpt-4o-mini"),
		TEXT("openai/gpt-4.1"),
		TEXT("openai/gpt-4.1-mini"),
		TEXT("openai/o3"),
		TEXT("openai/o3-mini"),
		TEXT("openai/o4-mini"),
		TEXT("google/gemini-pro-1.5"),
		TEXT("meta-llama/llama-3.1-70b-instruct"),
		TEXT("meta-llama/llama-3.1-405b-instruct")
	};
}

void FOliveOpenRouterProvider::Configure(const FOliveProviderConfig& InConfig)
{
	Config = InConfig;

	// Use default URL if not specified
	if (Config.BaseUrl.IsEmpty())
	{
		Config.BaseUrl = OpenRouterApiUrl;
	}

	// Use default model if not specified
	if (Config.ModelId.IsEmpty())
	{
		Config.ModelId = DefaultModel;
	}
}

bool FOliveOpenRouterProvider::ValidateConfig(FString& OutError) const
{
	if (Config.ApiKey.IsEmpty())
	{
		OutError = TEXT("API key is required. Get one at https://openrouter.ai/keys");
		return false;
	}

	if (Config.ModelId.IsEmpty())
	{
		OutError = TEXT("Model ID is required");
		return false;
	}

	return true;
}

// ==========================================
// Request
// ==========================================

void FOliveOpenRouterProvider::SendMessage(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools,
	FOnOliveStreamChunk OnChunk,
	FOnOliveToolCall OnToolCall,
	FOnOliveComplete OnComplete,
	FOnOliveError OnError,
	const FOliveRequestOptions& Options)
{
	// Validate config
	FString ValidationError;
	if (!ValidateConfig(ValidationError))
	{
		OnError.ExecuteIfBound(ValidationError);
		return;
	}

	// Check if already busy
	if (bIsBusy)
	{
		OnError.ExecuteIfBound(TEXT("A request is already in progress"));
		return;
	}

	// Store callbacks
	OnChunkCallback = OnChunk;
	OnToolCallCallback = OnToolCall;
	OnCompleteCallback = OnComplete;
	OnErrorCallback = OnError;

	// Reset state
	SSEBuffer.Empty();
	AccumulatedResponse.Empty();
	PendingToolCalls.Empty();
	PendingToolArgsBuffer.Empty();
	CurrentUsage = FOliveProviderUsage();
	LastError.Empty();
	bIsBusy = true;

	// Build request body
	TSharedPtr<FJsonObject> RequestBody = BuildRequestBody(Messages, Tools, Options);
	FString RequestBodyString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyString);
	FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

	// Resolve effective timeout
	const int32 EffectiveTimeout = Options.TimeoutSeconds > 0 ? Options.TimeoutSeconds : Config.TimeoutSeconds;

	// Create HTTP request
	CurrentRequest = FHttpModule::Get().CreateRequest();
	CurrentRequest->SetURL(Config.BaseUrl);
	CurrentRequest->SetVerb(TEXT("POST"));
	CurrentRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	CurrentRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Config.ApiKey));
	CurrentRequest->SetHeader(TEXT("HTTP-Referer"), TEXT("https://olivestudio.ai"));
	CurrentRequest->SetHeader(TEXT("X-Title"), TEXT("Olive AI Studio"));
	CurrentRequest->SetContentAsString(RequestBodyString);
	CurrentRequest->SetTimeout(EffectiveTimeout);

	// Capture weak alive flag for safe async callbacks
	TWeakPtr<bool> WeakAlive = AliveFlag;
	auto* Self = this;

	// Set up streaming response handling using OnRequestProgress64
	CurrentRequest->OnRequestProgress64().BindLambda(
		[WeakAlive, Self](FHttpRequestPtr Request, uint64 BytesSent, uint64 BytesReceived)
		{
			if (!WeakAlive.IsValid()) return;
			if (Request.IsValid() && Request->GetResponse().IsValid())
			{
				FString Content = Request->GetResponse()->GetContentAsString();
				if (Content.Len() > Self->SSEBuffer.Len())
				{
					FString NewData = Content.RightChop(Self->SSEBuffer.Len());
					Self->SSEBuffer = Content;
					Self->ProcessSSEData(NewData);
				}
			}
		}
	);

	CurrentRequest->OnProcessRequestComplete().BindLambda(
		[WeakAlive, Self](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedSuccessfully)
		{
			if (WeakAlive.IsValid())
			{
				Self->OnResponseReceived(Req, Resp, bConnectedSuccessfully);
			}
		}
	);

	// Send request
	UE_LOG(LogOliveAI, Log, TEXT("Sending request to OpenRouter: %s"), *Config.ModelId);
	CurrentRequest->ProcessRequest();
}

void FOliveOpenRouterProvider::CancelRequest()
{
	if (CurrentRequest.IsValid() && bIsBusy)
	{
		CurrentRequest->CancelRequest();
		CurrentRequest.Reset();
		bIsBusy = false;
		UE_LOG(LogOliveAI, Log, TEXT("OpenRouter request cancelled"));
	}
}

// ==========================================
// Request Building
// ==========================================

bool FOliveOpenRouterProvider::IsReasoningModel(const FString& ModelId)
{
	// OpenRouter model IDs have provider prefix (e.g., "openai/o3")
	// Extract the model part after the last slash
	FString ModelPart = ModelId;
	int32 SlashIdx;
	if (ModelId.FindLastChar(TEXT('/'), SlashIdx))
	{
		ModelPart = ModelId.Mid(SlashIdx + 1);
	}

	return ModelPart.StartsWith(TEXT("o1"))
		|| ModelPart.StartsWith(TEXT("o3"))
		|| ModelPart.StartsWith(TEXT("o4"))
		|| ModelPart.Contains(TEXT("gpt-4.1"));
}

TSharedPtr<FJsonObject> FOliveOpenRouterProvider::BuildRequestBody(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools,
	const FOliveRequestOptions& Options)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();

	// Resolve effective values from per-request options or config defaults
	const int32 EffectiveMaxTokens = Options.MaxTokens > 0 ? Options.MaxTokens : Config.MaxTokens;
	const float EffectiveTemperature = Options.Temperature >= 0.0f ? Options.Temperature : Config.Temperature;

	Body->SetStringField(TEXT("model"), Config.ModelId);
	Body->SetArrayField(TEXT("messages"), ConvertMessagesToJson(Messages));

	if (Tools.Num() > 0)
	{
		Body->SetArrayField(TEXT("tools"), ConvertToolsToJson(Tools));
		// Let the model choose when to use tools
		Body->SetStringField(TEXT("tool_choice"), TEXT("auto"));
	}

	Body->SetBoolField(TEXT("stream"), true);

	if (IsReasoningModel(Config.ModelId))
	{
		Body->SetNumberField(TEXT("max_completion_tokens"), EffectiveMaxTokens);
		// Reasoning models reject temperature and top_p
	}
	else
	{
		Body->SetNumberField(TEXT("temperature"), EffectiveTemperature);
		Body->SetNumberField(TEXT("max_tokens"), EffectiveMaxTokens);
	}

	return Body;
}

TArray<TSharedPtr<FJsonValue>> FOliveOpenRouterProvider::ConvertMessagesToJson(const TArray<FOliveChatMessage>& Messages)
{
	TArray<TSharedPtr<FJsonValue>> JsonMessages;

	for (const FOliveChatMessage& Message : Messages)
	{
		TSharedPtr<FJsonObject> MsgJson = Message.ToJson();
		if (MsgJson.IsValid())
		{
			JsonMessages.Add(MakeShared<FJsonValueObject>(MsgJson));
		}
	}

	return JsonMessages;
}

TArray<TSharedPtr<FJsonValue>> FOliveOpenRouterProvider::ConvertToolsToJson(const TArray<FOliveToolDefinition>& Tools)
{
	TArray<TSharedPtr<FJsonValue>> JsonTools;

	for (const FOliveToolDefinition& Tool : Tools)
	{
		TSharedPtr<FJsonObject> ToolJson = Tool.ToMCPJson();
		if (ToolJson.IsValid())
		{
			JsonTools.Add(MakeShared<FJsonValueObject>(ToolJson));
		}
	}

	return JsonTools;
}

// ==========================================
// Response Handling
// ==========================================

void FOliveOpenRouterProvider::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	if (!bIsBusy)
	{
		return; // Request was cancelled
	}

	if (!bSuccess || !Response.IsValid())
	{
		HandleError(TEXT("[HTTP:0] Network error: Failed to connect to OpenRouter"));
		return;
	}

	int32 StatusCode = Response->GetResponseCode();

	if (StatusCode == 401)
	{
		HandleError(TEXT("[HTTP:401] Invalid API key. Please check your OpenRouter API key in settings."));
		return;
	}

	if (StatusCode == 429)
	{
		FString RetryAfter = Response->GetHeader(TEXT("Retry-After"));
		if (!RetryAfter.IsEmpty())
		{
			int32 RetryAfterSeconds = FCString::Atoi(*RetryAfter);
			if (RetryAfterSeconds <= 0) { RetryAfterSeconds = 60; }
			HandleError(FString::Printf(TEXT("[HTTP:429:RetryAfter=%d] Rate limited by OpenRouter. Try again in %s seconds."), RetryAfterSeconds, *RetryAfter));
		}
		else
		{
			HandleError(TEXT("[HTTP:429:RetryAfter=60] Rate limited by OpenRouter. Please wait before trying again."));
		}
		return;
	}

	if (StatusCode >= 400)
	{
		FString ErrorBody = Response->GetContentAsString();

		// Try to parse error message
		TSharedPtr<FJsonObject> ErrorJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ErrorBody);
		if (FJsonSerializer::Deserialize(Reader, ErrorJson) && ErrorJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrorObj;
			if (ErrorJson->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				FString ErrorMessage = (*ErrorObj)->GetStringField(TEXT("message"));
				HandleError(FString::Printf(TEXT("[HTTP:%d] API Error: %s"), StatusCode, *ErrorMessage));
				return;
			}
		}

		HandleError(FString::Printf(TEXT("[HTTP:%d] HTTP Error %d"), StatusCode, StatusCode));
		return;
	}

	// Finalize any pending tool calls
	FinalizePendingToolCalls();

	// Complete streaming
	CompleteStreaming();
}

void FOliveOpenRouterProvider::ProcessSSEData(const FString& Data)
{
	// Split by newlines and process each line
	TArray<FString> Lines;
	Data.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		ProcessSSELine(Line);
	}
}

void FOliveOpenRouterProvider::ProcessSSELine(const FString& Line)
{
	// SSE format: "data: {...json...}"
	if (!Line.StartsWith(TEXT("data: ")))
	{
		return;
	}

	FString JsonData = Line.RightChop(6); // Remove "data: "

	// Check for stream end
	if (JsonData == TEXT("[DONE]"))
	{
		return;
	}

	// Parse JSON
	TSharedPtr<FJsonObject> ChunkJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonData);
	if (!FJsonSerializer::Deserialize(Reader, ChunkJson) || !ChunkJson.IsValid())
	{
		UE_LOG(LogOliveAI, Warning, TEXT("[OpenRouter] Failed to parse SSE chunk: %s"), *JsonData);
		return;
	}

	ParseStreamChunk(ChunkJson);
}

void FOliveOpenRouterProvider::ParseStreamChunk(const TSharedPtr<FJsonObject>& ChunkJson)
{
	// Get choices array
	const TArray<TSharedPtr<FJsonValue>>* Choices;
	if (!ChunkJson->TryGetArrayField(TEXT("choices"), Choices) || Choices->Num() == 0)
	{
		return;
	}

	TSharedPtr<FJsonObject> Choice = (*Choices)[0]->AsObject();
	if (!Choice.IsValid())
	{
		return;
	}

	// Check for finish reason
	FString FinishReason = Choice->GetStringField(TEXT("finish_reason"));
	if (!FinishReason.IsEmpty() && FinishReason != TEXT("null"))
	{
		// Store finish reason so HandleComplete can detect truncation (e.g. "length")
		CurrentUsage.FinishReason = FinishReason;

		// Finalize pending tool calls
		FinalizePendingToolCalls();
		return;
	}

	// Get delta
	const TSharedPtr<FJsonObject>* DeltaPtr;
	if (!Choice->TryGetObjectField(TEXT("delta"), DeltaPtr))
	{
		return;
	}

	TSharedPtr<FJsonObject> Delta = *DeltaPtr;

	// Check for text content
	if (Delta->HasField(TEXT("content")))
	{
		FString Content = Delta->GetStringField(TEXT("content"));
		if (!Content.IsEmpty())
		{
			AccumulatedResponse += Content;

			FOliveStreamChunk Chunk;
			Chunk.Text = Content;
			OnChunkCallback.ExecuteIfBound(Chunk);
		}
	}

	// Check for tool calls
	if (Delta->HasField(TEXT("tool_calls")))
	{
		ParseToolCallDelta(Delta);
	}

	// Check for usage in chunk (some providers send this)
	const TSharedPtr<FJsonObject>* UsagePtr;
	if (ChunkJson->TryGetObjectField(TEXT("usage"), UsagePtr))
	{
		CurrentUsage.PromptTokens = (*UsagePtr)->GetIntegerField(TEXT("prompt_tokens"));
		CurrentUsage.CompletionTokens = (*UsagePtr)->GetIntegerField(TEXT("completion_tokens"));
		CurrentUsage.TotalTokens = (*UsagePtr)->GetIntegerField(TEXT("total_tokens"));
	}
}

// ==========================================
// Tool Call Parsing
// ==========================================

void FOliveOpenRouterProvider::ParseToolCallDelta(const TSharedPtr<FJsonObject>& Delta)
{
	const TArray<TSharedPtr<FJsonValue>>* ToolCallsArray;
	if (!Delta->TryGetArrayField(TEXT("tool_calls"), ToolCallsArray))
	{
		return;
	}

	for (const TSharedPtr<FJsonValue>& ToolCallValue : *ToolCallsArray)
	{
		TSharedPtr<FJsonObject> ToolCallObj = ToolCallValue->AsObject();
		if (!ToolCallObj.IsValid())
		{
			continue;
		}

		// Get index for tracking
		int32 Index = ToolCallObj->GetIntegerField(TEXT("index"));

		// Get or create pending tool call
		FOliveStreamChunk* PendingCall = PendingToolCalls.Find(Index);
		if (!PendingCall)
		{
			FOliveStreamChunk NewCall;
			NewCall.bIsToolCall = true;
			PendingToolCalls.Add(Index, NewCall);
			PendingCall = PendingToolCalls.Find(Index);
		}

		// Update tool call ID if present
		if (ToolCallObj->HasField(TEXT("id")))
		{
			PendingCall->ToolCallId = ToolCallObj->GetStringField(TEXT("id"));
		}

		// Update function info if present
		const TSharedPtr<FJsonObject>* FunctionPtr;
		if (ToolCallObj->TryGetObjectField(TEXT("function"), FunctionPtr))
		{
			if ((*FunctionPtr)->HasField(TEXT("name")))
			{
				PendingCall->ToolName = (*FunctionPtr)->GetStringField(TEXT("name"));
			}

			if ((*FunctionPtr)->HasField(TEXT("arguments")))
			{
				FString ArgsChunk = (*FunctionPtr)->GetStringField(TEXT("arguments"));

				// Accumulate arguments in dedicated buffer (not PendingCall->Text)
				PendingToolArgsBuffer.FindOrAdd(Index) += ArgsChunk;
			}
		}
	}
}

void FOliveOpenRouterProvider::FinalizePendingToolCalls()
{
	for (auto& Pair : PendingToolCalls)
	{
		FOliveStreamChunk& Call = Pair.Value;

		// Parse accumulated arguments JSON from dedicated buffer
		FString* ArgsBuffer = PendingToolArgsBuffer.Find(Pair.Key);
		if (ArgsBuffer && !ArgsBuffer->IsEmpty())
		{
			TSharedPtr<FJsonObject> ArgsJson;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ArgsBuffer);
			if (FJsonSerializer::Deserialize(Reader, ArgsJson))
			{
				Call.ToolArguments = ArgsJson;
			}
			else
			{
				// Create empty args if parsing failed
				Call.ToolArguments = MakeShared<FJsonObject>();
				UE_LOG(LogOliveAI, Warning, TEXT("Failed to parse tool arguments: %s"), **ArgsBuffer);
			}
		}
		else
		{
			Call.ToolArguments = MakeShared<FJsonObject>();
		}

		UE_LOG(LogOliveAI, Log, TEXT("Tool call: %s (id: %s)"), *Call.ToolName, *Call.ToolCallId);
		OnToolCallCallback.ExecuteIfBound(Call);
	}

	PendingToolCalls.Empty();
	PendingToolArgsBuffer.Empty();
}

// ==========================================
// Completion
// ==========================================

void FOliveOpenRouterProvider::CompleteStreaming()
{
	CurrentUsage.Model = Config.ModelId;

	// Estimate cost (rough approximation)
	// Claude Sonnet: ~$3/1M input, ~$15/1M output
	double InputCost = CurrentUsage.PromptTokens * 0.000003;
	double OutputCost = CurrentUsage.CompletionTokens * 0.000015;
	CurrentUsage.EstimatedCostUSD = InputCost + OutputCost;

	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Log, TEXT("OpenRouter request complete. Tokens: %d prompt, %d completion"),
		CurrentUsage.PromptTokens, CurrentUsage.CompletionTokens);

	OnCompleteCallback.ExecuteIfBound(AccumulatedResponse, CurrentUsage);
}

void FOliveOpenRouterProvider::HandleError(const FString& ErrorMessage)
{
	LastError = ErrorMessage;
	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Error, TEXT("OpenRouter error: %s"), *ErrorMessage);
	OnErrorCallback.ExecuteIfBound(ErrorMessage);
}

// ==========================================
// Connection Validation
// ==========================================

void FOliveOpenRouterProvider::ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const
{
	FString Error;
	if (!ValidateConfig(Error))
	{
		Callback(false, Error);
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(TEXT("https://openrouter.ai/api/v1/auth/key"));
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Config.ApiKey));

	Request->OnProcessRequestComplete().BindLambda(
		[Callback](FHttpRequestPtr /*Req*/, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				Callback(false, TEXT("Cannot connect to OpenRouter. Check your internet connection."));
				return;
			}

			int32 Code = Response->GetResponseCode();
			if (Code == 200)
			{
				// Try to parse credits info
				TSharedPtr<FJsonObject> Json;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
				if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
				{
					const TSharedPtr<FJsonObject>* DataObj;
					if (Json->TryGetObjectField(TEXT("data"), DataObj))
					{
						double Credits = 0.0;
						if ((*DataObj)->TryGetNumberField(TEXT("limit_remaining"), Credits))
						{
							Callback(true, FString::Printf(TEXT("Connected to OpenRouter. Remaining credits: $%.2f"), Credits));
							return;
						}
					}
				}
				Callback(true, TEXT("Connected to OpenRouter. API key valid."));
			}
			else if (Code == 401)
			{
				Callback(false, TEXT("Invalid OpenRouter API key. Get one at https://openrouter.ai/keys"));
			}
			else
			{
				Callback(false, FString::Printf(TEXT("OpenRouter returned HTTP %d: %s"), Code, *Response->GetContentAsString()));
			}
		});

	Request->ProcessRequest();
}
