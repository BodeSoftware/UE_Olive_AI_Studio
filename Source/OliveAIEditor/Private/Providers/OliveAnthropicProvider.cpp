// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveAnthropicProvider.h"
#include "OliveAIEditorModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const FString FOliveAnthropicProvider::AnthropicApiUrl = TEXT("https://api.anthropic.com/v1/messages");
const FString FOliveAnthropicProvider::DefaultModel = TEXT("claude-sonnet-4-5");

FOliveAnthropicProvider::FOliveAnthropicProvider()
{
	Config.ProviderName = TEXT("anthropic");
	Config.BaseUrl = AnthropicApiUrl;
	Config.ModelId = DefaultModel;
}

FOliveAnthropicProvider::~FOliveAnthropicProvider()
{
	CancelRequest();
}

TArray<FString> FOliveAnthropicProvider::GetAvailableModels() const
{
	return {
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
		OutError = TEXT("Anthropic API key is required.");
		return false;
	}

	if (Config.ModelId.IsEmpty())
	{
		OutError = TEXT("Anthropic model ID is required.");
		return false;
	}

	return true;
}

void FOliveAnthropicProvider::SendMessage(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools,
	FOnOliveStreamChunk OnChunk,
	FOnOliveToolCall OnToolCall,
	FOnOliveComplete OnComplete,
	FOnOliveError OnError)
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

	OnChunkCallback = OnChunk;
	OnToolCallCallback = OnToolCall;
	OnCompleteCallback = OnComplete;
	OnErrorCallback = OnError;
	LastError.Empty();
	AccumulatedResponse.Empty();
	bIsBusy = true;

	TSharedPtr<FJsonObject> RequestBody = BuildRequestBody(Messages, Tools);
	FString BodyString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
	FJsonSerializer::Serialize(RequestBody.ToSharedRef(), Writer);

	CurrentRequest = FHttpModule::Get().CreateRequest();
	CurrentRequest->SetURL(Config.BaseUrl);
	CurrentRequest->SetVerb(TEXT("POST"));
	CurrentRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	CurrentRequest->SetHeader(TEXT("x-api-key"), Config.ApiKey);
	CurrentRequest->SetHeader(TEXT("anthropic-version"), TEXT("2023-06-01"));
	CurrentRequest->SetContentAsString(BodyString);
	CurrentRequest->SetTimeout(Config.TimeoutSeconds);
	CurrentRequest->OnProcessRequestComplete().BindRaw(this, &FOliveAnthropicProvider::OnResponseReceived);
	CurrentRequest->ProcessRequest();
}

void FOliveAnthropicProvider::CancelRequest()
{
	if (CurrentRequest.IsValid() && bIsBusy)
	{
		CurrentRequest->CancelRequest();
	}
	CurrentRequest.Reset();
	bIsBusy = false;
}

TSharedPtr<FJsonObject> FOliveAnthropicProvider::BuildRequestBody(
	const TArray<FOliveChatMessage>& Messages,
	const TArray<FOliveToolDefinition>& Tools) const
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("model"), NormalizeModelName(Config.ModelId));
	Body->SetNumberField(TEXT("max_tokens"), Config.MaxTokens);
	Body->SetNumberField(TEXT("temperature"), Config.Temperature);
	Body->SetBoolField(TEXT("stream"), false);

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

void FOliveAnthropicProvider::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess)
{
	if (!bIsBusy)
	{
		return;
	}

	bIsBusy = false;

	if (!bSuccess || !Response.IsValid())
	{
		HandleError(TEXT("Network error: Failed to connect to Anthropic"));
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	if (StatusCode >= 400)
	{
		HandleError(FString::Printf(TEXT("Anthropic API error: HTTP %d"), StatusCode));
		return;
	}

	TSharedPtr<FJsonObject> ResponseJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
	{
		HandleError(TEXT("Failed to parse Anthropic response"));
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
	if (ResponseJson->TryGetArrayField(TEXT("content"), ContentArray) && ContentArray)
	{
		for (const TSharedPtr<FJsonValue>& Item : *ContentArray)
		{
			TSharedPtr<FJsonObject> Block = Item->AsObject();
			if (!Block.IsValid())
			{
				continue;
			}

			FString BlockType;
			Block->TryGetStringField(TEXT("type"), BlockType);

			if (BlockType == TEXT("text"))
			{
				FString Text;
				Block->TryGetStringField(TEXT("text"), Text);
				if (!Text.IsEmpty())
				{
					AccumulatedResponse += Text;

					FOliveStreamChunk Chunk;
					Chunk.Text = Text;
					Chunk.bIsComplete = false;
					OnChunkCallback.ExecuteIfBound(Chunk);
				}
			}
			else if (BlockType == TEXT("tool_use"))
			{
				FOliveStreamChunk ToolCall;
				ToolCall.bIsToolCall = true;
				Block->TryGetStringField(TEXT("id"), ToolCall.ToolCallId);
				Block->TryGetStringField(TEXT("name"), ToolCall.ToolName);

				const TSharedPtr<FJsonObject>* InputObj = nullptr;
				if (Block->TryGetObjectField(TEXT("input"), InputObj) && InputObj)
				{
					ToolCall.ToolArguments = *InputObj;
				}
				else
				{
					ToolCall.ToolArguments = MakeShared<FJsonObject>();
				}

				OnToolCallCallback.ExecuteIfBound(ToolCall);
			}
		}
	}

	FOliveProviderUsage Usage;
	const TSharedPtr<FJsonObject>* UsageObj = nullptr;
	if (ResponseJson->TryGetObjectField(TEXT("usage"), UsageObj) && UsageObj && UsageObj->IsValid())
	{
		if ((*UsageObj)->HasField(TEXT("input_tokens")))
		{
			Usage.PromptTokens = (*UsageObj)->GetIntegerField(TEXT("input_tokens"));
		}
		if ((*UsageObj)->HasField(TEXT("output_tokens")))
		{
			Usage.CompletionTokens = (*UsageObj)->GetIntegerField(TEXT("output_tokens"));
		}
		Usage.TotalTokens = Usage.PromptTokens + Usage.CompletionTokens;
	}
	Usage.Model = NormalizeModelName(Config.ModelId);

	OnCompleteCallback.ExecuteIfBound(AccumulatedResponse, Usage);
}

void FOliveAnthropicProvider::HandleError(const FString& ErrorMessage)
{
	LastError = ErrorMessage;
	bIsBusy = false;
	OnErrorCallback.ExecuteIfBound(ErrorMessage);
}
