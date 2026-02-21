// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveOpenAICompatibleProvider.h"
#include "OliveAIEditorModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FOliveOpenAICompatibleProvider::FOliveOpenAICompatibleProvider()
{
	Config.ProviderName = TEXT("openai_compatible");
}

FOliveOpenAICompatibleProvider::~FOliveOpenAICompatibleProvider()
{
	*AliveFlag = false;
	CancelRequest();
}

// ==========================================
// Configuration
// ==========================================

TArray<FString> FOliveOpenAICompatibleProvider::GetAvailableModels() const
{
	// No hardcoded models — user types the model name for their endpoint
	return {};
}

void FOliveOpenAICompatibleProvider::Configure(const FOliveProviderConfig& InConfig)
{
	Config = InConfig;
}

bool FOliveOpenAICompatibleProvider::ValidateConfig(FString& OutError) const
{
	if (Config.BaseUrl.IsEmpty())
	{
		OutError = TEXT("Base URL is required. Enter the URL of your OpenAI-compatible endpoint (e.g., http://localhost:1234/v1).");
		return false;
	}

	if (Config.ModelId.IsEmpty())
	{
		OutError = TEXT("Model name is required. Enter the model name your endpoint serves.");
		return false;
	}

	// API key is optional — many local endpoints don't require one

	return true;
}

// ==========================================
// URL Helpers
// ==========================================

FString FOliveOpenAICompatibleProvider::GetCompletionsUrl() const
{
	FString Url = Config.BaseUrl;

	// Remove trailing slash for consistent handling
	while (Url.EndsWith(TEXT("/")))
	{
		Url.LeftChopInline(1);
	}

	// Append /chat/completions if the URL doesn't already end with it
	if (!Url.EndsWith(TEXT("/chat/completions")))
	{
		// Check if URL ends with /v1 or similar path — just append /chat/completions
		Url += TEXT("/chat/completions");
	}

	return Url;
}

// ==========================================
// Request
// ==========================================

void FOliveOpenAICompatibleProvider::SendMessage(
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

	// Resolve the endpoint URL
	FString CompletionsUrl = GetCompletionsUrl();

	// Create HTTP request
	CurrentRequest = FHttpModule::Get().CreateRequest();
	CurrentRequest->SetURL(CompletionsUrl);
	CurrentRequest->SetVerb(TEXT("POST"));
	CurrentRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	// Only set Authorization header if API key is provided
	if (!Config.ApiKey.IsEmpty())
	{
		CurrentRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Config.ApiKey));
	}

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
	UE_LOG(LogOliveAI, Log, TEXT("Sending request to OpenAI-compatible endpoint: %s (model: %s)"), *CompletionsUrl, *Config.ModelId);
	CurrentRequest->ProcessRequest();
}

void FOliveOpenAICompatibleProvider::CancelRequest()
{
	if (CurrentRequest.IsValid() && bIsBusy)
	{
		CurrentRequest->CancelRequest();
		CurrentRequest.Reset();
		bIsBusy = false;
		UE_LOG(LogOliveAI, Log, TEXT("OpenAI-compatible request cancelled"));
	}
}

// ==========================================
// Request Building
// ==========================================

TSharedPtr<FJsonObject> FOliveOpenAICompatibleProvider::BuildRequestBody(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools,
	const FOliveRequestOptions& Options)
{
	// Resolve effective values from per-request options or config defaults
	const int32 EffectiveMaxTokens = Options.MaxTokens > 0 ? Options.MaxTokens : Config.MaxTokens;
	const float EffectiveTemperature = Options.Temperature >= 0.0f ? Options.Temperature : Config.Temperature;

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();

	Body->SetStringField(TEXT("model"), Config.ModelId);
	Body->SetArrayField(TEXT("messages"), ConvertMessagesToJson(Messages));

	if (Tools.Num() > 0)
	{
		Body->SetArrayField(TEXT("tools"), ConvertToolsToJson(Tools));
		// Let the model choose when to use tools
		Body->SetStringField(TEXT("tool_choice"), TEXT("auto"));
	}

	Body->SetBoolField(TEXT("stream"), true);
	Body->SetNumberField(TEXT("temperature"), EffectiveTemperature);
	Body->SetNumberField(TEXT("max_tokens"), EffectiveMaxTokens);

	return Body;
}

TArray<TSharedPtr<FJsonValue>> FOliveOpenAICompatibleProvider::ConvertMessagesToJson(const TArray<FOliveChatMessage>& Messages)
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

TArray<TSharedPtr<FJsonValue>> FOliveOpenAICompatibleProvider::ConvertToolsToJson(const TArray<FOliveToolDefinition>& Tools)
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

void FOliveOpenAICompatibleProvider::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	if (!bIsBusy)
	{
		return; // Request was cancelled
	}

	if (!bSuccess || !Response.IsValid())
	{
		FString Url = GetCompletionsUrl();
		HandleError(FString::Printf(TEXT("Cannot connect to %s. Verify the server is running and the URL is correct."), *Url));
		return;
	}

	int32 StatusCode = Response->GetResponseCode();

	if (StatusCode == 401)
	{
		HandleError(TEXT("Authentication failed (HTTP 401). Check your API key for this endpoint."));
		return;
	}

	if (StatusCode == 403)
	{
		HandleError(TEXT("Access denied (HTTP 403). Verify your API key and permissions for this endpoint."));
		return;
	}

	if (StatusCode == 429)
	{
		FString RetryAfter = Response->GetHeader(TEXT("Retry-After"));
		if (!RetryAfter.IsEmpty())
		{
			HandleError(FString::Printf(TEXT("Rate limited by the server. Try again in %s seconds."), *RetryAfter));
		}
		else
		{
			HandleError(TEXT("Rate limited by the server. Please wait before trying again."));
		}
		return;
	}

	if (StatusCode >= 400)
	{
		FString ErrorBody = Response->GetContentAsString();

		// Try to parse error message from OpenAI-compatible error format
		TSharedPtr<FJsonObject> ErrorJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ErrorBody);
		if (FJsonSerializer::Deserialize(Reader, ErrorJson) && ErrorJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrorObj;
			if (ErrorJson->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				FString ErrorMessage = (*ErrorObj)->GetStringField(TEXT("message"));
				FString ErrorCode = (*ErrorObj)->GetStringField(TEXT("code"));

				if (ErrorCode == TEXT("model_not_found"))
				{
					HandleError(FString::Printf(TEXT("Model '%s' not found on this endpoint. Check the model name."), *Config.ModelId));
					return;
				}

				HandleError(FString::Printf(TEXT("Server error: %s"), *ErrorMessage));
				return;
			}
		}

		HandleError(FString::Printf(TEXT("Server returned HTTP %d"), StatusCode));
		return;
	}

	// Finalize any pending tool calls
	FinalizePendingToolCalls();

	// Complete streaming
	CompleteStreaming();
}

void FOliveOpenAICompatibleProvider::ProcessSSEData(const FString& Data)
{
	// Split by newlines and process each line
	TArray<FString> Lines;
	Data.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		ProcessSSELine(Line);
	}
}

void FOliveOpenAICompatibleProvider::ProcessSSELine(const FString& Line)
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
		UE_LOG(LogOliveAI, Warning, TEXT("[OpenAICompatible] Failed to parse SSE chunk: %s"), *JsonData);
		return;
	}

	ParseStreamChunk(ChunkJson);
}

void FOliveOpenAICompatibleProvider::ParseStreamChunk(const TSharedPtr<FJsonObject>& ChunkJson)
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

	// Check for usage in chunk (some endpoints send this with stream_options)
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

void FOliveOpenAICompatibleProvider::ParseToolCallDelta(const TSharedPtr<FJsonObject>& Delta)
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

void FOliveOpenAICompatibleProvider::FinalizePendingToolCalls()
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

		UE_LOG(LogOliveAI, Log, TEXT("OpenAI-compatible tool call: %s (id: %s)"), *Call.ToolName, *Call.ToolCallId);
		OnToolCallCallback.ExecuteIfBound(Call);
	}

	PendingToolCalls.Empty();
	PendingToolArgsBuffer.Empty();
}

// ==========================================
// Completion
// ==========================================

void FOliveOpenAICompatibleProvider::CompleteStreaming()
{
	CurrentUsage.Model = Config.ModelId;

	// No cost estimation for generic endpoints — pricing is unknown
	CurrentUsage.EstimatedCostUSD = 0.0;

	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Log, TEXT("OpenAI-compatible request complete. Tokens: %d prompt, %d completion"),
		CurrentUsage.PromptTokens, CurrentUsage.CompletionTokens);

	OnCompleteCallback.ExecuteIfBound(AccumulatedResponse, CurrentUsage);
}

void FOliveOpenAICompatibleProvider::HandleError(const FString& ErrorMessage)
{
	LastError = ErrorMessage;
	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Error, TEXT("OpenAI-compatible endpoint error: %s"), *ErrorMessage);
	OnErrorCallback.ExecuteIfBound(ErrorMessage);
}

// ==========================================
// Connection Validation
// ==========================================

void FOliveOpenAICompatibleProvider::ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const
{
	FString Error;
	if (!ValidateConfig(Error))
	{
		Callback(false, Error);
		return;
	}

	// Build models URL from base URL
	FString BaseUrl = Config.BaseUrl;
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1);
	}

	// Try /v1/models if base URL doesn't already include /v1
	FString ModelsUrl;
	if (BaseUrl.Contains(TEXT("/v1")))
	{
		// Strip to the /v1 part and add /models
		int32 V1Idx = BaseUrl.Find(TEXT("/v1"));
		ModelsUrl = BaseUrl.Left(V1Idx) + TEXT("/v1/models");
	}
	else
	{
		ModelsUrl = BaseUrl + TEXT("/v1/models");
	}

	FString EndpointUrl = Config.BaseUrl;

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ModelsUrl);
	Request->SetVerb(TEXT("GET"));
	if (!Config.ApiKey.IsEmpty())
	{
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Config.ApiKey));
	}

	Request->OnProcessRequestComplete().BindLambda(
		[Callback, EndpointUrl](FHttpRequestPtr /*Req*/, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				Callback(false, FString::Printf(TEXT("Cannot connect to %s. Check the endpoint URL."), *EndpointUrl));
				return;
			}

			int32 Code = Response->GetResponseCode();
			if (Code >= 200 && Code < 300)
			{
				Callback(true, TEXT("Endpoint reachable. Connection valid."));
			}
			else if (Code == 401)
			{
				Callback(false, TEXT("Authentication failed. Check your API key."));
			}
			else
			{
				// Even a non-200 response means the endpoint is reachable
				Callback(true, FString::Printf(TEXT("Endpoint reachable (HTTP %d). Models endpoint may not be supported."), Code));
			}
		});

	Request->ProcessRequest();
}
