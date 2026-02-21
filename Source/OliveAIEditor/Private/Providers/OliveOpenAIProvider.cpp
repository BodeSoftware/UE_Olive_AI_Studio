// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveOpenAIProvider.h"
#include "OliveAIEditorModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const FString FOliveOpenAIProvider::OpenAIApiUrl = TEXT("https://api.openai.com/v1/chat/completions");
const FString FOliveOpenAIProvider::DefaultModel = TEXT("gpt-4o");

FOliveOpenAIProvider::FOliveOpenAIProvider()
{
	Config.ProviderName = TEXT("openai");
	Config.BaseUrl = OpenAIApiUrl;
	Config.ModelId = DefaultModel;
}

FOliveOpenAIProvider::~FOliveOpenAIProvider()
{
	*AliveFlag = false;
	CancelRequest();
}

// ==========================================
// Configuration
// ==========================================

TArray<FString> FOliveOpenAIProvider::GetAvailableModels() const
{
	return {
		TEXT("gpt-4o"),
		TEXT("gpt-4o-mini"),
		TEXT("gpt-4-turbo"),
		TEXT("o1"),
		TEXT("o1-mini")
	};
}

void FOliveOpenAIProvider::Configure(const FOliveProviderConfig& InConfig)
{
	Config = InConfig;

	// Use default URL if not specified
	if (Config.BaseUrl.IsEmpty())
	{
		Config.BaseUrl = OpenAIApiUrl;
	}

	// Use default model if not specified
	if (Config.ModelId.IsEmpty())
	{
		Config.ModelId = DefaultModel;
	}
}

bool FOliveOpenAIProvider::ValidateConfig(FString& OutError) const
{
	if (Config.ApiKey.IsEmpty())
	{
		OutError = TEXT("API key is required. Get one at https://platform.openai.com/api-keys");
		return false;
	}

	if (!Config.ApiKey.StartsWith(TEXT("sk-")))
	{
		OutError = TEXT("OpenAI API key should start with 'sk-'. Check your key at https://platform.openai.com/api-keys");
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

void FOliveOpenAIProvider::SendMessage(
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
	UE_LOG(LogOliveAI, Log, TEXT("Sending request to OpenAI: %s"), *Config.ModelId);
	CurrentRequest->ProcessRequest();
}

void FOliveOpenAIProvider::CancelRequest()
{
	if (CurrentRequest.IsValid() && bIsBusy)
	{
		CurrentRequest->CancelRequest();
		CurrentRequest.Reset();
		bIsBusy = false;
		UE_LOG(LogOliveAI, Log, TEXT("OpenAI request cancelled"));
	}
}

// ==========================================
// Request Building
// ==========================================

TSharedPtr<FJsonObject> FOliveOpenAIProvider::BuildRequestBody(
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

TArray<TSharedPtr<FJsonValue>> FOliveOpenAIProvider::ConvertMessagesToJson(const TArray<FOliveChatMessage>& Messages)
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

TArray<TSharedPtr<FJsonValue>> FOliveOpenAIProvider::ConvertToolsToJson(const TArray<FOliveToolDefinition>& Tools)
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

void FOliveOpenAIProvider::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	if (!bIsBusy)
	{
		return; // Request was cancelled
	}

	if (!bSuccess || !Response.IsValid())
	{
		HandleError(TEXT("Network error: Failed to connect to OpenAI. Check your internet connection."));
		return;
	}

	int32 StatusCode = Response->GetResponseCode();

	if (StatusCode == 401)
	{
		HandleError(TEXT("Invalid OpenAI API key. Check your key at platform.openai.com."));
		return;
	}

	if (StatusCode == 403)
	{
		HandleError(TEXT("Organization/project access denied. Check your OpenAI organization and project permissions at platform.openai.com."));
		return;
	}

	if (StatusCode == 429)
	{
		FString RetryAfter = Response->GetHeader(TEXT("Retry-After"));
		if (!RetryAfter.IsEmpty())
		{
			HandleError(FString::Printf(TEXT("Rate limited. Try again in %s seconds."), *RetryAfter));
		}
		else
		{
			HandleError(TEXT("Rate limited. Please wait before trying again."));
		}
		return;
	}

	if (StatusCode >= 400)
	{
		FString ErrorBody = Response->GetContentAsString();

		// Try to parse error message from OpenAI error format
		TSharedPtr<FJsonObject> ErrorJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ErrorBody);
		if (FJsonSerializer::Deserialize(Reader, ErrorJson) && ErrorJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrorObj;
			if (ErrorJson->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				FString ErrorMessage = (*ErrorObj)->GetStringField(TEXT("message"));
				FString ErrorCode = (*ErrorObj)->GetStringField(TEXT("code"));

				// Handle specific OpenAI error codes
				if (ErrorCode == TEXT("model_not_found"))
				{
					HandleError(FString::Printf(TEXT("Model '%s' not available. Check model name at platform.openai.com/docs/models."), *Config.ModelId));
					return;
				}

				HandleError(FString::Printf(TEXT("OpenAI API error: %s"), *ErrorMessage));
				return;
			}
		}

		HandleError(FString::Printf(TEXT("OpenAI HTTP error %d"), StatusCode));
		return;
	}

	// Finalize any pending tool calls
	FinalizePendingToolCalls();

	// Complete streaming
	CompleteStreaming();
}

void FOliveOpenAIProvider::ProcessSSEData(const FString& Data)
{
	// Split by newlines and process each line
	TArray<FString> Lines;
	Data.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		ProcessSSELine(Line);
	}
}

void FOliveOpenAIProvider::ProcessSSELine(const FString& Line)
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
		UE_LOG(LogOliveAI, Warning, TEXT("[OpenAI] Failed to parse SSE chunk: %s"), *JsonData);
		return;
	}

	ParseStreamChunk(ChunkJson);
}

void FOliveOpenAIProvider::ParseStreamChunk(const TSharedPtr<FJsonObject>& ChunkJson)
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

	// Check for usage in chunk (OpenAI sends this with stream_options)
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

void FOliveOpenAIProvider::ParseToolCallDelta(const TSharedPtr<FJsonObject>& Delta)
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

void FOliveOpenAIProvider::FinalizePendingToolCalls()
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

		UE_LOG(LogOliveAI, Log, TEXT("OpenAI tool call: %s (id: %s)"), *Call.ToolName, *Call.ToolCallId);
		OnToolCallCallback.ExecuteIfBound(Call);
	}

	PendingToolCalls.Empty();
	PendingToolArgsBuffer.Empty();
}

// ==========================================
// Completion
// ==========================================

void FOliveOpenAIProvider::CompleteStreaming()
{
	CurrentUsage.Model = Config.ModelId;

	// Estimate cost based on model
	// GPT-4o: ~$2.50/1M input, ~$10/1M output
	// GPT-4o-mini: ~$0.15/1M input, ~$0.60/1M output
	double InputRate = 0.0000025;
	double OutputRate = 0.000010;

	if (Config.ModelId.Contains(TEXT("gpt-4o-mini")))
	{
		InputRate = 0.00000015;
		OutputRate = 0.0000006;
	}
	else if (Config.ModelId.Contains(TEXT("gpt-4-turbo")))
	{
		InputRate = 0.00001;
		OutputRate = 0.00003;
	}
	else if (Config.ModelId == TEXT("o1"))
	{
		InputRate = 0.000015;
		OutputRate = 0.00006;
	}
	else if (Config.ModelId == TEXT("o1-mini"))
	{
		InputRate = 0.000003;
		OutputRate = 0.000012;
	}

	CurrentUsage.EstimatedCostUSD = (CurrentUsage.PromptTokens * InputRate) + (CurrentUsage.CompletionTokens * OutputRate);

	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Log, TEXT("OpenAI request complete. Tokens: %d prompt, %d completion"),
		CurrentUsage.PromptTokens, CurrentUsage.CompletionTokens);

	OnCompleteCallback.ExecuteIfBound(AccumulatedResponse, CurrentUsage);
}

void FOliveOpenAIProvider::HandleError(const FString& ErrorMessage)
{
	LastError = ErrorMessage;
	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Error, TEXT("OpenAI error: %s"), *ErrorMessage);
	OnErrorCallback.ExecuteIfBound(ErrorMessage);
}

// ==========================================
// Connection Validation
// ==========================================

void FOliveOpenAIProvider::ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const
{
	FString Error;
	if (!ValidateConfig(Error))
	{
		Callback(false, Error);
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(TEXT("https://api.openai.com/v1/models"));
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Config.ApiKey));

	Request->OnProcessRequestComplete().BindLambda(
		[Callback](FHttpRequestPtr /*Req*/, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				Callback(false, TEXT("Cannot connect to OpenAI. Check your internet connection."));
				return;
			}

			int32 Code = Response->GetResponseCode();
			if (Code == 200)
			{
				Callback(true, TEXT("Connected to OpenAI. API key valid."));
			}
			else if (Code == 401)
			{
				Callback(false, TEXT("Invalid OpenAI API key. Check your key at https://platform.openai.com/api-keys"));
			}
			else if (Code == 403)
			{
				Callback(false, TEXT("Access denied. Check your OpenAI organization/project settings."));
			}
			else
			{
				Callback(false, FString::Printf(TEXT("OpenAI returned HTTP %d: %s"), Code, *Response->GetContentAsString()));
			}
		});

	Request->ProcessRequest();
}
