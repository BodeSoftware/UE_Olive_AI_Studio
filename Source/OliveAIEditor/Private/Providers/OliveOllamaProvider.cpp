// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveOllamaProvider.h"
#include "OliveAIEditorModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const FString FOliveOllamaProvider::DefaultBaseUrl = TEXT("http://localhost:11434");
const FString FOliveOllamaProvider::DefaultModel = TEXT("llama3.1");

FOliveOllamaProvider::FOliveOllamaProvider()
{
	Config.ProviderName = TEXT("ollama");
	Config.BaseUrl = DefaultBaseUrl;
	Config.ModelId = DefaultModel;
}

FOliveOllamaProvider::~FOliveOllamaProvider()
{
	CancelRequest();
}

// ==========================================
// URL Helpers
// ==========================================

FString FOliveOllamaProvider::GetCompletionsUrl() const
{
	FString BaseUrl = Config.BaseUrl;

	// Remove trailing slash
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1);
	}

	// If the URL already contains the completions path, use as-is
	if (BaseUrl.Contains(TEXT("/v1/chat/completions")))
	{
		return BaseUrl;
	}

	// Append the OpenAI-compatible completions endpoint
	return BaseUrl + TEXT("/v1/chat/completions");
}

// ==========================================
// Configuration
// ==========================================

TArray<FString> FOliveOllamaProvider::GetAvailableModels() const
{
	return {
		TEXT("llama3.1"),
		TEXT("codellama"),
		TEXT("mistral"),
		TEXT("deepseek-coder")
	};
}

void FOliveOllamaProvider::Configure(const FOliveProviderConfig& InConfig)
{
	Config = InConfig;

	// Use default URL if not specified
	if (Config.BaseUrl.IsEmpty())
	{
		Config.BaseUrl = DefaultBaseUrl;
	}

	// Use default model if not specified
	if (Config.ModelId.IsEmpty())
	{
		Config.ModelId = DefaultModel;
	}
}

bool FOliveOllamaProvider::ValidateConfig(FString& OutError) const
{
	if (Config.BaseUrl.IsEmpty())
	{
		OutError = TEXT("Ollama URL is required. Default is http://localhost:11434");
		return false;
	}

	if (Config.ModelId.IsEmpty())
	{
		OutError = TEXT("Model ID is required. Try 'llama3.1' or run 'ollama list' to see available models.");
		return false;
	}

	// No API key required for Ollama (local only)
	return true;
}

// ==========================================
// Request
// ==========================================

void FOliveOllamaProvider::SendMessage(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools,
	FOnOliveStreamChunk OnChunk,
	FOnOliveToolCall OnToolCall,
	FOnOliveComplete OnComplete,
	FOnOliveError OnError)
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
	CurrentUsage = FOliveProviderUsage();
	LastError.Empty();
	bIsBusy = true;

	// Build request body
	TSharedPtr<FJsonObject> RequestBody = BuildRequestBody(Messages, Tools);
	FString RequestBodyString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyString);
	FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

	// Create HTTP request
	FString CompletionsUrl = GetCompletionsUrl();
	CurrentRequest = FHttpModule::Get().CreateRequest();
	CurrentRequest->SetURL(CompletionsUrl);
	CurrentRequest->SetVerb(TEXT("POST"));
	CurrentRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	// No Authorization header — Ollama is local and requires no API key
	CurrentRequest->SetContentAsString(RequestBodyString);
	CurrentRequest->SetTimeout(Config.TimeoutSeconds);

	// Set up streaming response handling using OnRequestProgress64
	CurrentRequest->OnRequestProgress64().BindLambda(
		[this](FHttpRequestPtr Request, uint64 BytesSent, uint64 BytesReceived)
		{
			if (Request.IsValid() && Request->GetResponse().IsValid())
			{
				FString Content = Request->GetResponse()->GetContentAsString();
				if (Content.Len() > SSEBuffer.Len())
				{
					FString NewData = Content.RightChop(SSEBuffer.Len());
					SSEBuffer = Content;
					ProcessSSEData(NewData);
				}
			}
		}
	);

	CurrentRequest->OnProcessRequestComplete().BindRaw(this, &FOliveOllamaProvider::OnResponseReceived);

	// Send request
	UE_LOG(LogOliveAI, Log, TEXT("Sending request to Ollama: %s (URL: %s)"), *Config.ModelId, *CompletionsUrl);
	CurrentRequest->ProcessRequest();
}

void FOliveOllamaProvider::CancelRequest()
{
	if (CurrentRequest.IsValid() && bIsBusy)
	{
		CurrentRequest->CancelRequest();
		CurrentRequest.Reset();
		bIsBusy = false;
		UE_LOG(LogOliveAI, Log, TEXT("Ollama request cancelled"));
	}
}

// ==========================================
// Request Building
// ==========================================

TSharedPtr<FJsonObject> FOliveOllamaProvider::BuildRequestBody(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();

	Body->SetStringField(TEXT("model"), Config.ModelId);
	Body->SetArrayField(TEXT("messages"), ConvertMessagesToJson(Messages));

	if (Tools.Num() > 0)
	{
		Body->SetArrayField(TEXT("tools"), ConvertToolsToJson(Tools));
		Body->SetStringField(TEXT("tool_choice"), TEXT("auto"));
	}

	Body->SetBoolField(TEXT("stream"), true);
	Body->SetNumberField(TEXT("temperature"), Config.Temperature);

	if (Config.MaxTokens > 0)
	{
		Body->SetNumberField(TEXT("max_tokens"), Config.MaxTokens);
	}

	return Body;
}

TArray<TSharedPtr<FJsonValue>> FOliveOllamaProvider::ConvertMessagesToJson(const TArray<FOliveChatMessage>& Messages)
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

TArray<TSharedPtr<FJsonValue>> FOliveOllamaProvider::ConvertToolsToJson(const TArray<FOliveToolDefinition>& Tools)
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

void FOliveOllamaProvider::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	if (!bIsBusy)
	{
		return; // Request was cancelled
	}

	if (!bSuccess || !Response.IsValid())
	{
		// Ollama-specific: connection refused means the daemon isn't running
		HandleError(TEXT("Ollama is not running. Start it with `ollama serve`."));
		return;
	}

	int32 StatusCode = Response->GetResponseCode();

	// Connection-level failure (no response code)
	if (StatusCode == 0)
	{
		HandleError(TEXT("Ollama is not running. Start it with `ollama serve`."));
		return;
	}

	if (StatusCode == 404)
	{
		// Ollama returns 404 when the model isn't available
		HandleError(FString::Printf(
			TEXT("Model '%s' not found. Pull it with `ollama pull %s`."),
			*Config.ModelId, *Config.ModelId));
		return;
	}

	if (StatusCode == 408 || StatusCode == 504)
	{
		HandleError(TEXT("Request timed out. Check if Ollama is still running."));
		return;
	}

	if (StatusCode == 429)
	{
		HandleError(TEXT("Ollama is overloaded. Wait a moment and try again."));
		return;
	}

	if (StatusCode >= 400)
	{
		FString ErrorBody = Response->GetContentAsString();

		// Try to parse Ollama error response (can be OpenAI-format or Ollama-native)
		TSharedPtr<FJsonObject> ErrorJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ErrorBody);
		if (FJsonSerializer::Deserialize(Reader, ErrorJson) && ErrorJson.IsValid())
		{
			// OpenAI-compatible error format
			const TSharedPtr<FJsonObject>* ErrorObj;
			if (ErrorJson->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				FString ErrorMessage = (*ErrorObj)->GetStringField(TEXT("message"));
				if (!ErrorMessage.IsEmpty())
				{
					// Check for model-not-found in the error message
					if (ErrorMessage.Contains(TEXT("not found")) || ErrorMessage.Contains(TEXT("does not exist")))
					{
						HandleError(FString::Printf(
							TEXT("Model '%s' not found. Pull it with `ollama pull %s`."),
							*Config.ModelId, *Config.ModelId));
						return;
					}

					HandleError(FString::Printf(TEXT("Ollama error: %s"), *ErrorMessage));
					return;
				}
			}

			// Ollama native error format: {"error": "message string"}
			FString NativeError;
			if (ErrorJson->TryGetStringField(TEXT("error"), NativeError) && !NativeError.IsEmpty())
			{
				if (NativeError.Contains(TEXT("not found")) || NativeError.Contains(TEXT("does not exist")))
				{
					HandleError(FString::Printf(
						TEXT("Model '%s' not found. Pull it with `ollama pull %s`."),
						*Config.ModelId, *Config.ModelId));
					return;
				}

				HandleError(FString::Printf(TEXT("Ollama error: %s"), *NativeError));
				return;
			}
		}

		HandleError(FString::Printf(TEXT("Ollama HTTP error %d"), StatusCode));
		return;
	}

	// Finalize any pending tool calls
	FinalizePendingToolCalls();

	// Complete streaming
	CompleteStreaming();
}

void FOliveOllamaProvider::ProcessSSEData(const FString& Data)
{
	// Split by newlines and process each line
	TArray<FString> Lines;
	Data.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		ProcessSSELine(Line);
	}
}

void FOliveOllamaProvider::ProcessSSELine(const FString& Line)
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
		return;
	}

	ParseStreamChunk(ChunkJson);
}

void FOliveOllamaProvider::ParseStreamChunk(const TSharedPtr<FJsonObject>& ChunkJson)
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

	// Check for tool calls (supported in llama3.1+, mistral, etc.)
	// Models that don't support tools simply won't include tool_calls — graceful degradation
	if (Delta->HasField(TEXT("tool_calls")))
	{
		ParseToolCallDelta(Delta);
	}

	// Check for usage in chunk
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

void FOliveOllamaProvider::ParseToolCallDelta(const TSharedPtr<FJsonObject>& Delta)
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

				// Accumulate arguments (they come in chunks)
				if (!PendingCall->Text.IsEmpty())
				{
					PendingCall->Text += ArgsChunk;
				}
				else
				{
					PendingCall->Text = ArgsChunk;
				}
			}
		}
	}
}

void FOliveOllamaProvider::FinalizePendingToolCalls()
{
	for (auto& Pair : PendingToolCalls)
	{
		FOliveStreamChunk& Call = Pair.Value;

		// Parse accumulated arguments JSON
		if (!Call.Text.IsEmpty())
		{
			TSharedPtr<FJsonObject> ArgsJson;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Call.Text);
			if (FJsonSerializer::Deserialize(Reader, ArgsJson))
			{
				Call.ToolArguments = ArgsJson;
			}
			else
			{
				// Create empty args if parsing failed
				Call.ToolArguments = MakeShared<FJsonObject>();
				UE_LOG(LogOliveAI, Warning, TEXT("Ollama: Failed to parse tool arguments: %s"), *Call.Text);
			}
		}
		else
		{
			Call.ToolArguments = MakeShared<FJsonObject>();
		}

		// Clear text (was used for argument accumulation)
		Call.Text.Empty();

		UE_LOG(LogOliveAI, Log, TEXT("Ollama tool call: %s (id: %s)"), *Call.ToolName, *Call.ToolCallId);
		OnToolCallCallback.ExecuteIfBound(Call);
	}

	PendingToolCalls.Empty();
}

// ==========================================
// Completion
// ==========================================

void FOliveOllamaProvider::CompleteStreaming()
{
	CurrentUsage.Model = Config.ModelId;

	// Ollama is local — no cost
	CurrentUsage.EstimatedCostUSD = 0.0;

	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Log, TEXT("Ollama request complete. Model: %s, Tokens: %d prompt, %d completion"),
		*Config.ModelId, CurrentUsage.PromptTokens, CurrentUsage.CompletionTokens);

	OnCompleteCallback.ExecuteIfBound(AccumulatedResponse, CurrentUsage);
}

void FOliveOllamaProvider::HandleError(const FString& ErrorMessage)
{
	LastError = ErrorMessage;
	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Error, TEXT("Ollama error: %s"), *ErrorMessage);
	OnErrorCallback.ExecuteIfBound(ErrorMessage);
}

// ==========================================
// Connection Validation
// ==========================================

void FOliveOllamaProvider::ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const
{
	FString Error;
	if (!ValidateConfig(Error))
	{
		Callback(false, Error);
		return;
	}

	// Build tags URL from base URL
	FString BaseUrl = Config.BaseUrl;
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1);
	}
	FString TagsUrl = BaseUrl + TEXT("/api/tags");

	FString ModelToCheck = Config.ModelId;

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(TagsUrl);
	Request->SetVerb(TEXT("GET"));

	Request->OnProcessRequestComplete().BindLambda(
		[Callback, ModelToCheck](FHttpRequestPtr /*Req*/, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				Callback(false, TEXT("Ollama is not running. Start it with `ollama serve`."));
				return;
			}

			int32 Code = Response->GetResponseCode();
			if (Code != 200)
			{
				Callback(false, FString::Printf(TEXT("Ollama returned HTTP %d. Is Ollama running correctly?"), Code));
				return;
			}

			// Parse the response to check if the configured model is available
			TSharedPtr<FJsonObject> Json;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				Callback(true, TEXT("Connected to Ollama."));
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* ModelsArray;
			if (!Json->TryGetArrayField(TEXT("models"), ModelsArray))
			{
				Callback(true, TEXT("Connected to Ollama."));
				return;
			}

			// Check if the configured model is in the list
			bool bModelFound = false;
			for (const TSharedPtr<FJsonValue>& ModelValue : *ModelsArray)
			{
				const TSharedPtr<FJsonObject>* ModelObj;
				if (ModelValue->TryGetObject(ModelObj))
				{
					FString ModelName;
					if ((*ModelObj)->TryGetStringField(TEXT("name"), ModelName))
					{
						// Ollama returns names like "llama3.1:latest" — check prefix match
						if (ModelName.StartsWith(ModelToCheck) || ModelToCheck.StartsWith(ModelName.Left(ModelName.Find(TEXT(":")))))
						{
							bModelFound = true;
							break;
						}
					}
				}
			}

			if (bModelFound)
			{
				Callback(true, FString::Printf(TEXT("Connected. Model '%s' available."), *ModelToCheck));
			}
			else
			{
				Callback(false, FString::Printf(TEXT("Connected but model '%s' not found. Run `ollama pull %s`."), *ModelToCheck, *ModelToCheck));
			}
		});

	Request->ProcessRequest();
}
