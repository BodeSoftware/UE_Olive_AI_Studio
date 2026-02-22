// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveAnthropicProvider.h"
#include "OliveAIEditorModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const FString FOliveAnthropicProvider::AnthropicApiUrl = TEXT("https://api.anthropic.com/v1/messages");
const FString FOliveAnthropicProvider::DefaultModel = TEXT("claude-sonnet-4-6");

FOliveAnthropicProvider::FOliveAnthropicProvider()
{
	Config.ProviderName = TEXT("anthropic");
	Config.BaseUrl = AnthropicApiUrl;
	Config.ModelId = DefaultModel;
}

FOliveAnthropicProvider::~FOliveAnthropicProvider()
{
	*AliveFlag = false;
	CancelRequest();
}

TArray<FString> FOliveAnthropicProvider::GetAvailableModels() const
{
	return {
		TEXT("claude-opus-4-6"),
		TEXT("claude-sonnet-4-6"),
		TEXT("claude-sonnet-4-5"),
		TEXT("claude-opus-4-1"),
		TEXT("claude-3-5-sonnet-latest"),
		TEXT("claude-3-5-haiku-latest")
	};
}

void FOliveAnthropicProvider::Configure(const FOliveProviderConfig& InConfig)
{
	Config = InConfig;
	if (Config.BaseUrl.IsEmpty())
	{
		Config.BaseUrl = AnthropicApiUrl;
	}
	if (Config.ModelId.IsEmpty())
	{
		Config.ModelId = DefaultModel;
	}
}

bool FOliveAnthropicProvider::ValidateConfig(FString& OutError) const
{
	if (Config.ApiKey.IsEmpty())
	{
		OutError = TEXT("Anthropic API key is required. Get one at https://console.anthropic.com/");
		return false;
	}

	if (Config.ModelId.IsEmpty())
	{
		OutError = TEXT("Anthropic model ID is required.");
		return false;
	}

	return true;
}

// ==========================================
// Request
// ==========================================

void FOliveAnthropicProvider::SendMessage(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools,
	FOnOliveStreamChunk OnChunk,
	FOnOliveToolCall OnToolCall,
	FOnOliveComplete OnComplete,
	FOnOliveError OnError,
	const FOliveRequestOptions& Options)
{
	FString ValidationError;
	if (!ValidateConfig(ValidationError))
	{
		OnError.ExecuteIfBound(ValidationError);
		return;
	}

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

	// Reset streaming state
	SSEBuffer.Empty();
	CurrentEventType.Empty();
	AccumulatedResponse.Empty();
	PendingToolCalls.Empty();
	PendingToolArgsJson.Empty();
	CurrentBlockIndex = -1;
	CurrentUsage = FOliveProviderUsage();
	LastError.Empty();
	bIsBusy = true;

	// Build request body
	TSharedPtr<FJsonObject> RequestBody = BuildRequestBody(Messages, Tools, Options);
	FString BodyString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
	FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

	// Resolve effective timeout
	const int32 EffectiveTimeout = Options.TimeoutSeconds > 0 ? Options.TimeoutSeconds : Config.TimeoutSeconds;

	// Create HTTP request
	CurrentRequest = FHttpModule::Get().CreateRequest();
	CurrentRequest->SetURL(Config.BaseUrl);
	CurrentRequest->SetVerb(TEXT("POST"));
	CurrentRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	CurrentRequest->SetHeader(TEXT("x-api-key"), Config.ApiKey);
	CurrentRequest->SetHeader(TEXT("anthropic-version"), TEXT("2023-06-01"));
	CurrentRequest->SetContentAsString(BodyString);
	CurrentRequest->SetTimeout(EffectiveTimeout);

	// Capture weak alive flag for safe async callbacks
	TWeakPtr<bool> WeakAlive = AliveFlag;
	auto* Self = this;

	// Set up streaming response handling via OnRequestProgress64
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

	UE_LOG(LogOliveAI, Log, TEXT("Sending streaming request to Anthropic: %s"), *Config.ModelId);
	CurrentRequest->ProcessRequest();
}

void FOliveAnthropicProvider::CancelRequest()
{
	if (CurrentRequest.IsValid() && bIsBusy)
	{
		CurrentRequest->CancelRequest();
		CurrentRequest.Reset();
		bIsBusy = false;
		UE_LOG(LogOliveAI, Log, TEXT("Anthropic request cancelled"));
	}
}

// ==========================================
// Request Building
// ==========================================

TSharedPtr<FJsonObject> FOliveAnthropicProvider::BuildRequestBody(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools,
	const FOliveRequestOptions& Options) const
{
	// Resolve effective values from per-request options or config defaults
	const int32 EffectiveMaxTokens = Options.MaxTokens > 0 ? Options.MaxTokens : Config.MaxTokens;
	const float EffectiveTemperature = Options.Temperature >= 0.0f ? Options.Temperature : Config.Temperature;

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("model"), NormalizeModelName(Config.ModelId));
	Body->SetNumberField(TEXT("max_tokens"), EffectiveMaxTokens);
	Body->SetNumberField(TEXT("temperature"), EffectiveTemperature);
	Body->SetBoolField(TEXT("stream"), true);

	// Anthropic uses a top-level "system" field, not a system message in the array
	FString SystemText;
	TArray<TSharedPtr<FJsonValue>> AnthropicMessages;

	for (const FOliveChatMessage& Message : Messages)
	{
		if (Message.Role == EOliveChatRole::System)
		{
			if (!SystemText.IsEmpty())
			{
				SystemText += TEXT("\n\n");
			}
			SystemText += Message.Content;
			continue;
		}

		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();

		if (Message.Role == EOliveChatRole::Tool)
		{
			// Anthropic tool results are sent as user messages with tool_result content blocks
			MsgObj->SetStringField(TEXT("role"), TEXT("user"));
			TArray<TSharedPtr<FJsonValue>> ContentBlocks;
			TSharedPtr<FJsonObject> ToolResultBlock = MakeShared<FJsonObject>();
			ToolResultBlock->SetStringField(TEXT("type"), TEXT("tool_result"));
			ToolResultBlock->SetStringField(TEXT("tool_use_id"), Message.ToolCallId);
			ToolResultBlock->SetStringField(TEXT("content"), Message.Content);
			ContentBlocks.Add(MakeShared<FJsonValueObject>(ToolResultBlock));
			MsgObj->SetArrayField(TEXT("content"), ContentBlocks);
		}
		else
		{
			const FString Role = (Message.Role == EOliveChatRole::Assistant) ? TEXT("assistant") : TEXT("user");
			MsgObj->SetStringField(TEXT("role"), Role);
			MsgObj->SetStringField(TEXT("content"), Message.Content);
		}

		AnthropicMessages.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	if (!SystemText.IsEmpty())
	{
		Body->SetStringField(TEXT("system"), SystemText);
	}
	Body->SetArrayField(TEXT("messages"), AnthropicMessages);

	// Anthropic tool format: name, description, input_schema
	if (Tools.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> AnthropicTools;
		for (const FOliveToolDefinition& Tool : Tools)
		{
			TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
			ToolObj->SetStringField(TEXT("name"), Tool.Name);
			ToolObj->SetStringField(TEXT("description"), Tool.Description);
			ToolObj->SetObjectField(TEXT("input_schema"), Tool.InputSchema.IsValid() ? Tool.InputSchema : MakeShared<FJsonObject>());
			AnthropicTools.Add(MakeShared<FJsonValueObject>(ToolObj));
		}
		Body->SetArrayField(TEXT("tools"), AnthropicTools);
	}

	return Body;
}

FString FOliveAnthropicProvider::NormalizeModelName(const FString& InModel) const
{
	FString Model = InModel;
	Model.TrimStartAndEndInline();
	Model.RemoveFromStart(TEXT("anthropic/"));
	return Model;
}

// ==========================================
// Response Handling
// ==========================================

void FOliveAnthropicProvider::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	if (!bIsBusy)
	{
		return; // Request was cancelled
	}

	if (!bSuccess || !Response.IsValid())
	{
		HandleError(TEXT("Network error: Failed to connect to Anthropic"));
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();

	if (StatusCode == 401)
	{
		HandleError(TEXT("Invalid API key. Check your Anthropic API key at https://console.anthropic.com/"));
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

	if (StatusCode == 400)
	{
		FString ErrorBody = Response->GetContentAsString();
		TSharedPtr<FJsonObject> ErrorJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ErrorBody);
		if (FJsonSerializer::Deserialize(Reader, ErrorJson) && ErrorJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrorObj;
			if (ErrorJson->TryGetObjectField(TEXT("error"), ErrorObj))
			{
				FString ErrorMessage = (*ErrorObj)->GetStringField(TEXT("message"));
				HandleError(FString::Printf(TEXT("Anthropic API Error: %s"), *ErrorMessage));
				return;
			}
		}
		HandleError(TEXT("Anthropic API Error: Bad request (400)"));
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
				FString ErrorMessage = (*ErrorObj)->GetStringField(TEXT("message"));
				HandleError(FString::Printf(TEXT("Anthropic API Error: %s"), *ErrorMessage));
				return;
			}
		}
		HandleError(FString::Printf(TEXT("Anthropic API Error: HTTP %d"), StatusCode));
		return;
	}

	// For a successful streaming response, data was already processed via progress callbacks.
	// Finalize any remaining pending tool calls and complete.
	FinalizePendingToolCalls();
	CompleteStreaming();
}

void FOliveAnthropicProvider::ProcessSSEData(const FString& Data)
{
	TArray<FString> Lines;
	Data.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		ProcessSSELine(Line);
	}
}

void FOliveAnthropicProvider::ProcessSSELine(const FString& Line)
{
	// Anthropic SSE format has two-line events:
	//   event: <type>
	//   data: <json>
	//
	// We track the event type, then process the data line with it.

	if (Line.StartsWith(TEXT("event: ")))
	{
		CurrentEventType = Line.RightChop(7).TrimEnd();
		return;
	}

	if (Line.StartsWith(TEXT("data: ")))
	{
		FString JsonStr = Line.RightChop(6);

		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Anthropic: Failed to parse SSE data: %s"), *JsonStr);
			return;
		}

		ParseStreamEvent(CurrentEventType, JsonObj);
		CurrentEventType.Empty();
		return;
	}

	// Empty lines or comments are ignored (standard SSE behavior)
}

void FOliveAnthropicProvider::ParseStreamEvent(const FString& EventType, const TSharedPtr<FJsonObject>& Data)
{
	if (EventType == TEXT("message_start"))
	{
		// Extract input token usage from message.usage
		const TSharedPtr<FJsonObject>* MessageObj;
		if (Data->TryGetObjectField(TEXT("message"), MessageObj))
		{
			const TSharedPtr<FJsonObject>* UsageObj;
			if ((*MessageObj)->TryGetObjectField(TEXT("usage"), UsageObj))
			{
				if ((*UsageObj)->HasField(TEXT("input_tokens")))
				{
					CurrentUsage.PromptTokens = (*UsageObj)->GetIntegerField(TEXT("input_tokens"));
				}
			}
		}
	}
	else if (EventType == TEXT("content_block_start"))
	{
		// A new content block is starting
		int32 Index = Data->GetIntegerField(TEXT("index"));
		CurrentBlockIndex = Index;

		const TSharedPtr<FJsonObject>* ContentBlockObj;
		if (Data->TryGetObjectField(TEXT("content_block"), ContentBlockObj))
		{
			FString BlockType;
			(*ContentBlockObj)->TryGetStringField(TEXT("type"), BlockType);

			if (BlockType == TEXT("tool_use"))
			{
				// Start a new pending tool call
				FOliveStreamChunk NewCall;
				NewCall.bIsToolCall = true;
				(*ContentBlockObj)->TryGetStringField(TEXT("id"), NewCall.ToolCallId);
				(*ContentBlockObj)->TryGetStringField(TEXT("name"), NewCall.ToolName);
				PendingToolCalls.Add(Index, NewCall);
				PendingToolArgsJson.Add(Index, FString());

				UE_LOG(LogOliveAI, Log, TEXT("Anthropic: Tool use block started: %s (id: %s) at index %d"),
					*NewCall.ToolName, *NewCall.ToolCallId, Index);
			}
			// "text" blocks don't need special start handling
		}
	}
	else if (EventType == TEXT("content_block_delta"))
	{
		int32 Index = Data->GetIntegerField(TEXT("index"));

		const TSharedPtr<FJsonObject>* DeltaObj;
		if (Data->TryGetObjectField(TEXT("delta"), DeltaObj))
		{
			FString DeltaType;
			(*DeltaObj)->TryGetStringField(TEXT("type"), DeltaType);

			if (DeltaType == TEXT("text_delta"))
			{
				// Text content streaming
				FString Text;
				(*DeltaObj)->TryGetStringField(TEXT("text"), Text);
				if (!Text.IsEmpty())
				{
					AccumulatedResponse += Text;

					FOliveStreamChunk Chunk;
					Chunk.Text = Text;
					Chunk.bIsComplete = false;
					OnChunkCallback.ExecuteIfBound(Chunk);
				}
			}
			else if (DeltaType == TEXT("input_json_delta"))
			{
				// Tool argument JSON arriving in chunks
				FString PartialJson;
				(*DeltaObj)->TryGetStringField(TEXT("partial_json"), PartialJson);
				if (!PartialJson.IsEmpty())
				{
					FString* ExistingArgs = PendingToolArgsJson.Find(Index);
					if (ExistingArgs)
					{
						*ExistingArgs += PartialJson;
					}
					else
					{
						PendingToolArgsJson.Add(Index, PartialJson);
					}
				}
			}
		}
	}
	else if (EventType == TEXT("content_block_stop"))
	{
		int32 Index = Data->GetIntegerField(TEXT("index"));

		// If this was a tool_use block, finalize the individual tool call now
		FOliveStreamChunk* PendingCall = PendingToolCalls.Find(Index);
		if (PendingCall)
		{
			FString* ArgsStr = PendingToolArgsJson.Find(Index);
			if (ArgsStr && !ArgsStr->IsEmpty())
			{
				TSharedPtr<FJsonObject> ArgsJson;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ArgsStr);
				if (FJsonSerializer::Deserialize(Reader, ArgsJson) && ArgsJson.IsValid())
				{
					PendingCall->ToolArguments = ArgsJson;
				}
				else
				{
					PendingCall->ToolArguments = MakeShared<FJsonObject>();
					UE_LOG(LogOliveAI, Warning, TEXT("Anthropic: Failed to parse tool arguments: %s"), **ArgsStr);
				}
			}
			else
			{
				PendingCall->ToolArguments = MakeShared<FJsonObject>();
			}

			UE_LOG(LogOliveAI, Log, TEXT("Anthropic: Tool call complete: %s (id: %s)"),
				*PendingCall->ToolName, *PendingCall->ToolCallId);
			OnToolCallCallback.ExecuteIfBound(*PendingCall);

			// Clean up this tool call — it's been dispatched
			PendingToolCalls.Remove(Index);
			PendingToolArgsJson.Remove(Index);
		}

		CurrentBlockIndex = -1;
	}
	else if (EventType == TEXT("message_delta"))
	{
		// Extract stop_reason and output token usage
		const TSharedPtr<FJsonObject>* DeltaObj;
		if (Data->TryGetObjectField(TEXT("delta"), DeltaObj))
		{
			FString StopReason;
			(*DeltaObj)->TryGetStringField(TEXT("stop_reason"), StopReason);
			if (!StopReason.IsEmpty())
			{
				UE_LOG(LogOliveAI, Log, TEXT("Anthropic: Stop reason: %s"), *StopReason);
			}
		}

		const TSharedPtr<FJsonObject>* UsageObj;
		if (Data->TryGetObjectField(TEXT("usage"), UsageObj))
		{
			if ((*UsageObj)->HasField(TEXT("output_tokens")))
			{
				CurrentUsage.CompletionTokens = (*UsageObj)->GetIntegerField(TEXT("output_tokens"));
			}
		}
	}
	else if (EventType == TEXT("message_stop"))
	{
		// Message is complete — streaming will finalize in OnResponseReceived
		UE_LOG(LogOliveAI, Log, TEXT("Anthropic: message_stop received"));
	}
	else if (EventType == TEXT("ping"))
	{
		// Keepalive, ignore
	}
	else if (EventType == TEXT("error"))
	{
		// Anthropic can send error events mid-stream
		const TSharedPtr<FJsonObject>* ErrorObj;
		if (Data->TryGetObjectField(TEXT("error"), ErrorObj))
		{
			FString ErrorMessage;
			(*ErrorObj)->TryGetStringField(TEXT("message"), ErrorMessage);
			UE_LOG(LogOliveAI, Error, TEXT("Anthropic stream error: %s"), *ErrorMessage);
			HandleError(FString::Printf(TEXT("Anthropic stream error: %s"), *ErrorMessage));
		}
	}
}

// ==========================================
// Tool Call Finalization
// ==========================================

void FOliveAnthropicProvider::FinalizePendingToolCalls()
{
	// Dispatch any tool calls that weren't finalized via content_block_stop
	// (safety net — normally content_block_stop handles each one)
	for (auto& Pair : PendingToolCalls)
	{
		FOliveStreamChunk& Call = Pair.Value;
		int32 Index = Pair.Key;

		FString* ArgsStr = PendingToolArgsJson.Find(Index);
		if (ArgsStr && !ArgsStr->IsEmpty())
		{
			TSharedPtr<FJsonObject> ArgsJson;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ArgsStr);
			if (FJsonSerializer::Deserialize(Reader, ArgsJson) && ArgsJson.IsValid())
			{
				Call.ToolArguments = ArgsJson;
			}
			else
			{
				Call.ToolArguments = MakeShared<FJsonObject>();
				UE_LOG(LogOliveAI, Warning, TEXT("Anthropic: Failed to parse tool arguments in finalize: %s"), **ArgsStr);
			}
		}
		else if (!Call.ToolArguments.IsValid())
		{
			Call.ToolArguments = MakeShared<FJsonObject>();
		}

		if (Call.ToolName.IsEmpty())
		{
			UE_LOG(LogOliveAI, Warning, TEXT("[Anthropic] Skipping tool call with empty name (id: %s)"), *Call.ToolCallId);
			continue;
		}

		UE_LOG(LogOliveAI, Log, TEXT("Anthropic: Finalizing tool call: %s (id: %s)"), *Call.ToolName, *Call.ToolCallId);
		OnToolCallCallback.ExecuteIfBound(Call);
	}

	PendingToolCalls.Empty();
	PendingToolArgsJson.Empty();
}

// ==========================================
// Completion
// ==========================================

void FOliveAnthropicProvider::CompleteStreaming()
{
	CurrentUsage.TotalTokens = CurrentUsage.PromptTokens + CurrentUsage.CompletionTokens;
	CurrentUsage.Model = NormalizeModelName(Config.ModelId);

	// Estimate cost (Claude Sonnet 4.5: ~$3/1M input, ~$15/1M output)
	double InputCost = CurrentUsage.PromptTokens * 0.000003;
	double OutputCost = CurrentUsage.CompletionTokens * 0.000015;
	CurrentUsage.EstimatedCostUSD = InputCost + OutputCost;

	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Log, TEXT("Anthropic request complete. Tokens: %d prompt, %d completion"),
		CurrentUsage.PromptTokens, CurrentUsage.CompletionTokens);

	OnCompleteCallback.ExecuteIfBound(AccumulatedResponse, CurrentUsage);
}

void FOliveAnthropicProvider::HandleError(const FString& ErrorMessage)
{
	LastError = ErrorMessage;
	bIsBusy = false;
	CurrentRequest.Reset();

	UE_LOG(LogOliveAI, Error, TEXT("Anthropic error: %s"), *ErrorMessage);
	OnErrorCallback.ExecuteIfBound(ErrorMessage);
}

// ==========================================
// Connection Validation
// ==========================================

void FOliveAnthropicProvider::ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const
{
	FString Error;
	if (!ValidateConfig(Error))
	{
		Callback(false, Error);
		return;
	}

	// Key format sanity check — warn but proceed (key format may change)
	if (!Config.ApiKey.StartsWith(TEXT("sk-ant-")))
	{
		UE_LOG(LogOliveAI, Warning, TEXT("[Anthropic] API key does not start with 'sk-ant-' prefix. Key format may have changed."));
	}

	// Send a minimal request to verify the key works.
	// Use max_tokens=1 and a tiny prompt to minimize cost.
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(AnthropicApiUrl);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("x-api-key"), Config.ApiKey);
	Request->SetHeader(TEXT("anthropic-version"), TEXT("2023-06-01"));
	Request->SetContentAsString(TEXT("{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"));
	Request->SetTimeout(15);

	Request->OnProcessRequestComplete().BindLambda(
		[Callback](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
		{
			if (!bSuccess || !Resp.IsValid())
			{
				Callback(false, TEXT("Cannot connect to Anthropic API. Check your internet connection."));
				return;
			}

			const int32 StatusCode = Resp->GetResponseCode();
			if (StatusCode == 200)
			{
				Callback(true, TEXT("Connected to Anthropic. API key valid."));
			}
			else if (StatusCode == 401)
			{
				Callback(false, TEXT("Invalid API key. Check your key at https://console.anthropic.com/"));
			}
			else if (StatusCode == 403)
			{
				Callback(false, TEXT("Access denied. Check your Anthropic account permissions."));
			}
			else if (StatusCode == 429)
			{
				// Rate limited means key is valid
				Callback(true, TEXT("Connected to Anthropic. API key valid (rate limited, try again shortly)."));
			}
			else
			{
				Callback(false, FString::Printf(TEXT("Anthropic returned HTTP %d. Check your API key and account."), StatusCode));
			}
		}
	);

	Request->ProcessRequest();
}
