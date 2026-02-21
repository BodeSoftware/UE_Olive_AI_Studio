// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveGoogleProvider.h"
#include "OliveAIEditorModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const FString FOliveGoogleProvider::GoogleApiBaseUrl = TEXT("https://generativelanguage.googleapis.com/v1beta/models/");
const FString FOliveGoogleProvider::DefaultModel = TEXT("gemini-2.0-flash");

FOliveGoogleProvider::FOliveGoogleProvider()
{
	Config.ProviderName = TEXT("google");
	Config.BaseUrl = GoogleApiBaseUrl;
	Config.ModelId = DefaultModel;
}

FOliveGoogleProvider::~FOliveGoogleProvider()
{
	CancelRequest();
}

// ==========================================
// Configuration
// ==========================================

TArray<FString> FOliveGoogleProvider::GetAvailableModels() const
{
	return {
		TEXT("gemini-2.0-flash"),
		TEXT("gemini-2.0-flash-lite"),
		TEXT("gemini-1.5-pro"),
		TEXT("gemini-1.5-flash")
	};
}

void FOliveGoogleProvider::Configure(const FOliveProviderConfig& InConfig)
{
	Config = InConfig;

	if (Config.BaseUrl.IsEmpty())
	{
		Config.BaseUrl = GoogleApiBaseUrl;
	}

	if (Config.ModelId.IsEmpty())
	{
		Config.ModelId = DefaultModel;
	}
}

bool FOliveGoogleProvider::ValidateConfig(FString& OutError) const
{
	if (Config.ApiKey.IsEmpty())
	{
		OutError = TEXT("Google AI API key is required. Get one at https://aistudio.google.com/apikey");
		return false;
	}

	if (Config.ModelId.IsEmpty())
	{
		OutError = TEXT("Model ID is required.");
		return false;
	}

	return true;
}

// ==========================================
// URL Building
// ==========================================

FString FOliveGoogleProvider::BuildRequestUrl() const
{
	// Format: https://generativelanguage.googleapis.com/v1beta/models/{model}:streamGenerateContent?key={key}&alt=sse
	FString BaseUrl = Config.BaseUrl;

	// Ensure base URL includes the models/ path segment
	if (!BaseUrl.Contains(TEXT("models")))
	{
		if (!BaseUrl.EndsWith(TEXT("/")))
		{
			BaseUrl += TEXT("/");
		}
		BaseUrl += TEXT("models/");
	}
	else if (!BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl += TEXT("/");
	}

	return FString::Printf(
		TEXT("%s%s:streamGenerateContent?key=%s&alt=sse"),
		*BaseUrl,
		*Config.ModelId,
		*Config.ApiKey
	);
}

// ==========================================
// Request
// ==========================================

void FOliveGoogleProvider::SendMessage(
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
	CurrentUsage = FOliveProviderUsage();
	LastFinishReason.Empty();
	LastError.Empty();
	bIsBusy = true;

	// Build request body
	TSharedPtr<FJsonObject> RequestBody = BuildRequestBody(Messages, Tools);
	FString RequestBodyString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBodyString);
	FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

	// Build URL with API key and model
	FString RequestUrl = BuildRequestUrl();

	// Create HTTP request
	CurrentRequest = FHttpModule::Get().CreateRequest();
	CurrentRequest->SetURL(RequestUrl);
	CurrentRequest->SetVerb(TEXT("POST"));
	CurrentRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	// Note: Google uses API key in URL, not in Authorization header
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

	CurrentRequest->OnProcessRequestComplete().BindRaw(this, &FOliveGoogleProvider::OnResponseReceived);

	// Send request
	UE_LOG(LogOliveAI, Log, TEXT("Sending request to Google Gemini: %s"), *Config.ModelId);
	CurrentRequest->ProcessRequest();
}

void FOliveGoogleProvider::CancelRequest()
{
	if (CurrentRequest.IsValid() && bIsBusy)
	{
		CurrentRequest->CancelRequest();
		CurrentRequest.Reset();
		bIsBusy = false;
		UE_LOG(LogOliveAI, Log, TEXT("Google request cancelled"));
	}
}

// ==========================================
// Request Building
// ==========================================

TSharedPtr<FJsonObject> FOliveGoogleProvider::BuildRequestBody(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();

	// Convert messages to Google contents format, extracting system instruction
	FString SystemText;
	TArray<TSharedPtr<FJsonValue>> Contents = ConvertMessagesToContents(Messages, SystemText);

	Body->SetArrayField(TEXT("contents"), Contents);

	// System instruction goes as a top-level field, not in contents
	if (!SystemText.IsEmpty())
	{
		TSharedPtr<FJsonObject> SystemInstruction = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> SystemParts;

		TSharedPtr<FJsonObject> TextPart = MakeShared<FJsonObject>();
		TextPart->SetStringField(TEXT("text"), SystemText);
		SystemParts.Add(MakeShared<FJsonValueObject>(TextPart));

		SystemInstruction->SetArrayField(TEXT("parts"), SystemParts);
		Body->SetObjectField(TEXT("systemInstruction"), SystemInstruction);
	}

	// Tools in Google format: tools[{ functionDeclarations: [...] }]
	if (Tools.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FunctionDeclarations = ConvertToolsToJson(Tools);

		TSharedPtr<FJsonObject> ToolsObj = MakeShared<FJsonObject>();
		ToolsObj->SetArrayField(TEXT("functionDeclarations"), FunctionDeclarations);

		TArray<TSharedPtr<FJsonValue>> ToolsArray;
		ToolsArray.Add(MakeShared<FJsonValueObject>(ToolsObj));
		Body->SetArrayField(TEXT("tools"), ToolsArray);
	}

	// Generation config
	TSharedPtr<FJsonObject> GenerationConfig = MakeShared<FJsonObject>();
	GenerationConfig->SetNumberField(TEXT("temperature"), Config.Temperature);
	GenerationConfig->SetNumberField(TEXT("maxOutputTokens"), Config.MaxTokens);
	Body->SetObjectField(TEXT("generationConfig"), GenerationConfig);

	return Body;
}

TArray<TSharedPtr<FJsonValue>> FOliveGoogleProvider::ConvertMessagesToContents(
	const TArray<FOliveChatMessage>& Messages,
	FString& OutSystemText)
{
	TArray<TSharedPtr<FJsonValue>> Contents;
	OutSystemText.Empty();

	for (const FOliveChatMessage& Message : Messages)
	{
		// System messages → accumulate into systemInstruction (not in contents)
		if (Message.Role == EOliveChatRole::System)
		{
			if (!OutSystemText.IsEmpty())
			{
				OutSystemText += TEXT("\n\n");
			}
			OutSystemText += Message.Content;
			continue;
		}

		TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Parts;

		if (Message.Role == EOliveChatRole::User)
		{
			// User message → { role: "user", parts: [{ text: "..." }] }
			ContentObj->SetStringField(TEXT("role"), TEXT("user"));

			TSharedPtr<FJsonObject> TextPart = MakeShared<FJsonObject>();
			TextPart->SetStringField(TEXT("text"), Message.Content);
			Parts.Add(MakeShared<FJsonValueObject>(TextPart));
		}
		else if (Message.Role == EOliveChatRole::Assistant)
		{
			// Assistant message → { role: "model", parts: [...] }
			ContentObj->SetStringField(TEXT("role"), TEXT("model"));

			if (Message.ToolCalls.Num() > 0)
			{
				// Assistant with tool calls → functionCall parts
				// Include text content if present
				if (!Message.Content.IsEmpty())
				{
					TSharedPtr<FJsonObject> TextPart = MakeShared<FJsonObject>();
					TextPart->SetStringField(TEXT("text"), Message.Content);
					Parts.Add(MakeShared<FJsonValueObject>(TextPart));
				}

				for (const FOliveStreamChunk& ToolCall : Message.ToolCalls)
				{
					TSharedPtr<FJsonObject> FunctionCallPart = MakeShared<FJsonObject>();
					TSharedPtr<FJsonObject> FunctionCallObj = MakeShared<FJsonObject>();
					FunctionCallObj->SetStringField(TEXT("name"), ToolCall.ToolName);

					if (ToolCall.ToolArguments.IsValid())
					{
						FunctionCallObj->SetObjectField(TEXT("args"), ToolCall.ToolArguments);
					}
					else
					{
						FunctionCallObj->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());
					}

					FunctionCallPart->SetObjectField(TEXT("functionCall"), FunctionCallObj);
					Parts.Add(MakeShared<FJsonValueObject>(FunctionCallPart));
				}
			}
			else
			{
				// Regular assistant text → { text: "..." }
				TSharedPtr<FJsonObject> TextPart = MakeShared<FJsonObject>();
				TextPart->SetStringField(TEXT("text"), Message.Content);
				Parts.Add(MakeShared<FJsonValueObject>(TextPart));
			}
		}
		else if (Message.Role == EOliveChatRole::Tool)
		{
			// Tool result → { role: "user", parts: [{ functionResponse: { name, response: { content } } }] }
			// Google treats tool results as user-role messages with functionResponse parts
			ContentObj->SetStringField(TEXT("role"), TEXT("user"));

			TSharedPtr<FJsonObject> FunctionResponsePart = MakeShared<FJsonObject>();
			TSharedPtr<FJsonObject> FunctionResponseObj = MakeShared<FJsonObject>();
			FunctionResponseObj->SetStringField(TEXT("name"), Message.ToolName);

			TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
			ResponseObj->SetStringField(TEXT("content"), Message.Content);
			FunctionResponseObj->SetObjectField(TEXT("response"), ResponseObj);

			FunctionResponsePart->SetObjectField(TEXT("functionResponse"), FunctionResponseObj);
			Parts.Add(MakeShared<FJsonValueObject>(FunctionResponsePart));
		}

		ContentObj->SetArrayField(TEXT("parts"), Parts);
		Contents.Add(MakeShared<FJsonValueObject>(ContentObj));
	}

	return Contents;
}

TArray<TSharedPtr<FJsonValue>> FOliveGoogleProvider::ConvertToolsToJson(const TArray<FOliveToolDefinition>& Tools)
{
	TArray<TSharedPtr<FJsonValue>> FunctionDeclarations;

	for (const FOliveToolDefinition& Tool : Tools)
	{
		TSharedPtr<FJsonObject> FuncDecl = MakeShared<FJsonObject>();
		FuncDecl->SetStringField(TEXT("name"), Tool.Name);
		FuncDecl->SetStringField(TEXT("description"), Tool.Description);

		if (Tool.InputSchema.IsValid())
		{
			FuncDecl->SetObjectField(TEXT("parameters"), Tool.InputSchema);
		}
		else
		{
			// Empty parameters object
			TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
			EmptyParams->SetStringField(TEXT("type"), TEXT("object"));
			EmptyParams->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
			FuncDecl->SetObjectField(TEXT("parameters"), EmptyParams);
		}

		FunctionDeclarations.Add(MakeShared<FJsonValueObject>(FuncDecl));
	}

	return FunctionDeclarations;
}

// ==========================================
// Response Handling
// ==========================================

void FOliveGoogleProvider::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	if (!bIsBusy)
	{
		return; // Request was cancelled
	}

	if (!bSuccess || !Response.IsValid())
	{
		HandleError(TEXT("Network error: Failed to connect to Google AI API"));
		return;
	}

	int32 StatusCode = Response->GetResponseCode();

	if (StatusCode == 400)
	{
		// Try to extract error details
		FString ErrorBody = Response->GetContentAsString();
		TSharedPtr<FJsonObject> ErrorJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ErrorBody);
		if (FJsonSerializer::Deserialize(Reader, ErrorJson) && ErrorJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrorObj;
			if (ErrorJson->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				FString ErrorMessage;
				(*ErrorObj)->TryGetStringField(TEXT("message"), ErrorMessage);
				if (!ErrorMessage.IsEmpty())
				{
					HandleError(FString::Printf(TEXT("Invalid request: %s"), *ErrorMessage));
					return;
				}
			}
		}
		HandleError(TEXT("Invalid request. Check model name and request format."));
		return;
	}

	if (StatusCode == 403)
	{
		HandleError(TEXT("API key invalid or quota exceeded. Check your key at https://aistudio.google.com/apikey"));
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

	if (StatusCode == 404)
	{
		HandleError(FString::Printf(TEXT("Model '%s' not found. Check the model name."), *Config.ModelId));
		return;
	}

	if (StatusCode >= 400)
	{
		FString ErrorBody = Response->GetContentAsString();
		TSharedPtr<FJsonObject> ErrorJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ErrorBody);
		if (FJsonSerializer::Deserialize(Reader, ErrorJson) && ErrorJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrorObj;
			if (ErrorJson->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				FString ErrorMessage;
				(*ErrorObj)->TryGetStringField(TEXT("message"), ErrorMessage);
				if (!ErrorMessage.IsEmpty())
				{
					HandleError(FString::Printf(TEXT("Google AI API Error: %s"), *ErrorMessage));
					return;
				}
			}
		}
		HandleError(FString::Printf(TEXT("Google AI API Error: HTTP %d"), StatusCode));
		return;
	}

	// Check for safety-blocked responses
	if (LastFinishReason == TEXT("SAFETY"))
	{
		HandleError(TEXT("Response blocked by Google safety filter. Try rephrasing your request."));
		return;
	}

	if (LastFinishReason == TEXT("RECITATION"))
	{
		HandleError(TEXT("Response blocked due to Google recitation policy."));
		return;
	}

	// Complete streaming
	CompleteStreaming();
}

void FOliveGoogleProvider::ProcessSSEData(const FString& Data)
{
	TArray<FString> Lines;
	Data.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		ProcessSSELine(Line);
	}
}

void FOliveGoogleProvider::ProcessSSELine(const FString& Line)
{
	// Google SSE format: "data: {...json...}"
	if (!Line.StartsWith(TEXT("data: ")))
	{
		return;
	}

	FString JsonData = Line.RightChop(6); // Remove "data: "

	if (JsonData.IsEmpty())
	{
		return;
	}

	// Parse JSON
	TSharedPtr<FJsonObject> ChunkJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonData);
	if (!FJsonSerializer::Deserialize(Reader, ChunkJson) || !ChunkJson.IsValid())
	{
		UE_LOG(LogOliveAI, Warning, TEXT("Google: Failed to parse SSE data: %s"), *JsonData);
		return;
	}

	ParseStreamChunk(ChunkJson);
}

void FOliveGoogleProvider::ParseStreamChunk(const TSharedPtr<FJsonObject>& ChunkJson)
{
	// Process candidates array
	const TArray<TSharedPtr<FJsonValue>>* Candidates;
	if (!ChunkJson->TryGetArrayField(TEXT("candidates"), Candidates) || Candidates->Num() == 0)
	{
		// No candidates — might be a usage-only chunk or error
		// Check for usageMetadata at root level even without candidates
		const TSharedPtr<FJsonObject>* UsagePtr;
		if (ChunkJson->TryGetObjectField(TEXT("usageMetadata"), UsagePtr))
		{
			if ((*UsagePtr)->HasField(TEXT("promptTokenCount")))
			{
				CurrentUsage.PromptTokens = (*UsagePtr)->GetIntegerField(TEXT("promptTokenCount"));
			}
			if ((*UsagePtr)->HasField(TEXT("candidatesTokenCount")))
			{
				CurrentUsage.CompletionTokens = (*UsagePtr)->GetIntegerField(TEXT("candidatesTokenCount"));
			}
			if ((*UsagePtr)->HasField(TEXT("totalTokenCount")))
			{
				CurrentUsage.TotalTokens = (*UsagePtr)->GetIntegerField(TEXT("totalTokenCount"));
			}
		}
		return;
	}

	TSharedPtr<FJsonObject> Candidate = (*Candidates)[0]->AsObject();
	if (!Candidate.IsValid())
	{
		return;
	}

	// Check finish reason
	FString FinishReason;
	if (Candidate->TryGetStringField(TEXT("finishReason"), FinishReason) && !FinishReason.IsEmpty())
	{
		LastFinishReason = FinishReason;

		if (FinishReason == TEXT("SAFETY"))
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Google: Response blocked by safety filter"));
			// Don't return yet — there may still be partial content or usage data
		}
		else if (FinishReason == TEXT("RECITATION"))
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Google: Response blocked by recitation policy"));
		}
		else if (FinishReason == TEXT("MAX_TOKENS"))
		{
			UE_LOG(LogOliveAI, Log, TEXT("Google: Response truncated due to max tokens"));
		}
	}

	// Process content parts
	const TSharedPtr<FJsonObject>* ContentPtr;
	if (Candidate->TryGetObjectField(TEXT("content"), ContentPtr))
	{
		const TArray<TSharedPtr<FJsonValue>>* PartsArray;
		if ((*ContentPtr)->TryGetArrayField(TEXT("parts"), PartsArray))
		{
			for (const TSharedPtr<FJsonValue>& PartValue : *PartsArray)
			{
				TSharedPtr<FJsonObject> Part = PartValue->AsObject();
				if (!Part.IsValid())
				{
					continue;
				}

				// Check for text content
				FString TextContent;
				if (Part->TryGetStringField(TEXT("text"), TextContent))
				{
					if (!TextContent.IsEmpty())
					{
						AccumulatedResponse += TextContent;

						FOliveStreamChunk Chunk;
						Chunk.Text = TextContent;
						OnChunkCallback.ExecuteIfBound(Chunk);
					}
				}

				// Check for function call
				const TSharedPtr<FJsonObject>* FunctionCallPtr;
				if (Part->TryGetObjectField(TEXT("functionCall"), FunctionCallPtr))
				{
					FString FuncName;
					(*FunctionCallPtr)->TryGetStringField(TEXT("name"), FuncName);

					const TSharedPtr<FJsonObject>* ArgsPtr;
					TSharedPtr<FJsonObject> FuncArgs;
					if ((*FunctionCallPtr)->TryGetObjectField(TEXT("args"), ArgsPtr))
					{
						FuncArgs = *ArgsPtr;
					}
					else
					{
						FuncArgs = MakeShared<FJsonObject>();
					}

					// Google doesn't provide tool call IDs, generate one
					FString ToolCallId = FString::Printf(TEXT("google_call_%s_%d"),
						*FuncName,
						FMath::Rand());

					FOliveStreamChunk ToolChunk;
					ToolChunk.bIsToolCall = true;
					ToolChunk.ToolCallId = ToolCallId;
					ToolChunk.ToolName = FuncName;
					ToolChunk.ToolArguments = FuncArgs;

					UE_LOG(LogOliveAI, Log, TEXT("Google: Tool call: %s (id: %s)"), *FuncName, *ToolCallId);
					OnToolCallCallback.ExecuteIfBound(ToolChunk);
				}
			}
		}
	}

	// Extract usage metadata
	const TSharedPtr<FJsonObject>* UsagePtr;
	if (ChunkJson->TryGetObjectField(TEXT("usageMetadata"), UsagePtr))
	{
		if ((*UsagePtr)->HasField(TEXT("promptTokenCount")))
		{
			CurrentUsage.PromptTokens = (*UsagePtr)->GetIntegerField(TEXT("promptTokenCount"));
		}
		if ((*UsagePtr)->HasField(TEXT("candidatesTokenCount")))
		{
			CurrentUsage.CompletionTokens = (*UsagePtr)->GetIntegerField(TEXT("candidatesTokenCount"));
		}
		if ((*UsagePtr)->HasField(TEXT("totalTokenCount")))
		{
			CurrentUsage.TotalTokens = (*UsagePtr)->GetIntegerField(TEXT("totalTokenCount"));
		}
	}
}

// ==========================================
// Completion
// ==========================================

void FOliveGoogleProvider::CompleteStreaming()
{
	CurrentUsage.Model = Config.ModelId;

	// Estimate cost (Gemini 2.0 Flash: ~$0.10/1M input, ~$0.40/1M output)
	double InputCost = CurrentUsage.PromptTokens * 0.0000001;
	double OutputCost = CurrentUsage.CompletionTokens * 0.0000004;
	CurrentUsage.EstimatedCostUSD = InputCost + OutputCost;

	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Log, TEXT("Google request complete. Tokens: %d prompt, %d completion"),
		CurrentUsage.PromptTokens, CurrentUsage.CompletionTokens);

	OnCompleteCallback.ExecuteIfBound(AccumulatedResponse, CurrentUsage);
}

void FOliveGoogleProvider::HandleError(const FString& ErrorMessage)
{
	LastError = ErrorMessage;
	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Error, TEXT("Google error: %s"), *ErrorMessage);
	OnErrorCallback.ExecuteIfBound(ErrorMessage);
}

// ==========================================
// Connection Validation
// ==========================================

void FOliveGoogleProvider::ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const
{
	FString Error;
	if (!ValidateConfig(Error))
	{
		Callback(false, Error);
		return;
	}

	FString Url = FString::Printf(TEXT("https://generativelanguage.googleapis.com/v1beta/models?key=%s"), *Config.ApiKey);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));

	Request->OnProcessRequestComplete().BindLambda(
		[Callback](FHttpRequestPtr /*Req*/, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid())
			{
				Callback(false, TEXT("Cannot connect to Google AI. Check your internet connection."));
				return;
			}

			int32 Code = Response->GetResponseCode();
			if (Code == 200)
			{
				Callback(true, TEXT("Connected to Google AI. API key valid."));
			}
			else if (Code == 400 || Code == 403)
			{
				Callback(false, TEXT("Google AI API key invalid or quota exceeded. Check your key at https://aistudio.google.com/app/apikey"));
			}
			else
			{
				Callback(false, FString::Printf(TEXT("Google AI returned HTTP %d: %s"), Code, *Response->GetContentAsString()));
			}
		});

	Request->ProcessRequest();
}
