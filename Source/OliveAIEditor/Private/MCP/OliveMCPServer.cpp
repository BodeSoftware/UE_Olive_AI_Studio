// Copyright Bode Software. All Rights Reserved.

#include "MCP/OliveMCPServer.h"
#include "MCP/OliveJsonRpc.h"
#include "MCP/OliveToolRegistry.h"
#include "OliveMCPPromptTemplates.h"
#include "Index/OliveProjectIndex.h"
#include "Catalog/OliveNodeCatalog.h"
#include "Catalog/OliveBTNodeCatalog.h"
#include "OliveAIEditorModule.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Misc/Guid.h"

// ==========================================
// Singleton
// ==========================================

FOliveMCPServer& FOliveMCPServer::Get()
{
	static FOliveMCPServer Instance;
	return Instance;
}

FOliveMCPServer::FOliveMCPServer()
{
}

FOliveMCPServer::~FOliveMCPServer()
{
	if (State == EOliveMCPServerState::Running)
	{
		Stop();
	}
}

// ==========================================
// Lifecycle
// ==========================================

bool FOliveMCPServer::Start(int32 Port)
{
	if (State == EOliveMCPServerState::Running)
	{
		UE_LOG(LogOliveAI, Warning, TEXT("MCP Server already running on port %d"), ActualPort);
		return true;
	}

	State = EOliveMCPServerState::Starting;

	// Get HTTP server module
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();

	// Try the requested port and a few alternatives
	TArray<int32> PortsToTry;
	for (int32 i = 0; i < 10; ++i)
	{
		PortsToTry.Add(Port + i);
	}

	bool bSuccess = false;
	for (int32 PortToTry : PortsToTry)
	{
		HttpRouter = HttpServerModule.GetHttpRouter(PortToTry);
		if (HttpRouter.IsValid())
		{
			// Bind the route
			RouteHandle = HttpRouter->BindRoute(
				FHttpPath(TEXT("/mcp")),
				EHttpServerRequestVerbs::VERB_POST,
				FHttpRequestHandler::CreateRaw(this, &FOliveMCPServer::HandleRequest)
			);

			if (RouteHandle.IsValid())
			{
				ActualPort = PortToTry;

				// Bind events polling route
				EventRouteHandle = HttpRouter->BindRoute(
					FHttpPath(TEXT("/mcp/events")),
					EHttpServerRequestVerbs::VERB_GET,
					FHttpRequestHandler::CreateRaw(this, &FOliveMCPServer::HandleEventsPoll)
				);
				bSuccess = true;

				// Start the listener
				HttpServerModule.StartAllListeners();

				break;
			}
		}
	}

	if (!bSuccess)
	{
		UE_LOG(LogOliveAI, Error, TEXT("Failed to start MCP Server - could not bind to any port"));
		State = EOliveMCPServerState::Error;
		return false;
	}

	State = EOliveMCPServerState::Running;

	// Start cleanup timer
	if (GEditor && GEditor->GetEditorWorldContext().World())
	{
		GEditor->GetEditorWorldContext().World()->GetTimerManager().SetTimer(
			CleanupTimerHandle,
			FTimerDelegate::CreateRaw(this, &FOliveMCPServer::CleanupInactiveClients),
			60.0f, // Check every minute
			true
		);
	}

	UE_LOG(LogOliveAI, Log, TEXT("MCP Server started on port %d"), ActualPort);

	return true;
}

void FOliveMCPServer::Stop()
{
	if (State != EOliveMCPServerState::Running)
	{
		return;
	}

	State = EOliveMCPServerState::Stopping;

	// Clear cleanup timer
	if (CleanupTimerHandle.IsValid() && GEditor && GEditor->GetEditorWorldContext().World())
	{
		GEditor->GetEditorWorldContext().World()->GetTimerManager().ClearTimer(CleanupTimerHandle);
	}

	// Unbind route
	if (HttpRouter.IsValid() && RouteHandle.IsValid())
	{
		HttpRouter->UnbindRoute(RouteHandle);
		if (EventRouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(EventRouteHandle);
		}
	}

	// Clear client states
	{
		FScopeLock Lock(&ClientsLock);
		ClientStates.Empty();
	}


	// Clear event buffer
	{
		FScopeLock Lock(&EventLock);
		EventBuffer.Empty();
		NextEventId = 1;
	}
	HttpRouter.Reset();
	RouteHandle = FHttpRouteHandle();
	EventRouteHandle = FHttpRouteHandle();
	ActualPort = 0;

	State = EOliveMCPServerState::Stopped;

	UE_LOG(LogOliveAI, Log, TEXT("MCP Server stopped"));
}

// ==========================================
// Client Management
// ==========================================

TArray<FString> FOliveMCPServer::GetConnectedClients() const
{
	TArray<FString> Clients;

	FScopeLock Lock(&ClientsLock);
	ClientStates.GetKeys(Clients);

	return Clients;
}

TOptional<FMCPClientState> FOliveMCPServer::GetClientState(const FString& ClientId) const
{
	FScopeLock Lock(&ClientsLock);

	const FMCPClientState* FoundState = ClientStates.Find(ClientId);
	if (FoundState)
	{
		return *FoundState;
	}

	return TOptional<FMCPClientState>();
}

int32 FOliveMCPServer::GetConnectedClientCount() const
{
	FScopeLock Lock(&ClientsLock);
	return ClientStates.Num();
}

// ==========================================
// HTTP Handling
// ==========================================

bool FOliveMCPServer::HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Extract client ID from headers or generate new one
	FString ClientId = ExtractClientId(Request);

	// Get request body
	FString RequestBody;
	const TArray<uint8>& Body = Request.Body;
	if (Body.Num() > 0)
	{
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
		RequestBody = FString(Converter.Length(), Converter.Get());
	}

	if (RequestBody.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorResponse = OliveJsonRpc::CreateErrorResponse(
			nullptr,
			OliveJsonRpc::INVALID_REQUEST,
			TEXT("Empty request body")
		);
		SendJsonResponse(ErrorResponse, OnComplete, 400);
		return true;
	}

	// Parse JSON-RPC request
	FString ParseError;
	TSharedPtr<FJsonObject> JsonRequest = OliveJsonRpc::ParseRequest(RequestBody, ParseError);

	if (!JsonRequest.IsValid())
	{
		TSharedPtr<FJsonObject> ErrorResponse = OliveJsonRpc::CreateErrorResponse(
			nullptr,
			OliveJsonRpc::PARSE_ERROR,
			ParseError
		);
		SendJsonResponse(ErrorResponse, OnComplete, 400);
		return true;
	}

	// Validate request
	FString ValidationError;
	if (!OliveJsonRpc::ValidateRequest(JsonRequest, ValidationError))
	{
		TSharedPtr<FJsonObject> ErrorResponse = OliveJsonRpc::CreateErrorResponse(
			OliveJsonRpc::GetRequestId(JsonRequest),
			OliveJsonRpc::INVALID_REQUEST,
			ValidationError
		);
		SendJsonResponse(ErrorResponse, OnComplete, 400);
		return true;
	}

	// Process the request
	TSharedPtr<FJsonObject> Response = ProcessJsonRpcRequest(JsonRequest, ClientId);

	// Notifications don't get responses
	if (OliveJsonRpc::IsNotification(JsonRequest))
	{
		// Send empty 204 response
		TUniquePtr<FHttpServerResponse> HttpResponse = FHttpServerResponse::Create(TEXT(""), TEXT("application/json"));
		OnComplete(MoveTemp(HttpResponse));
		return true;
	}

	SendJsonResponse(Response, OnComplete);
	return true;
}

TSharedPtr<FJsonObject> FOliveMCPServer::ProcessJsonRpcRequest(
	const TSharedPtr<FJsonObject>& Request,
	const FString& ClientId)
{
	FString Method = OliveJsonRpc::GetMethod(Request);
	TSharedPtr<FJsonObject> Params = OliveJsonRpc::GetParams(Request);
	TSharedPtr<FJsonValue> RequestId = OliveJsonRpc::GetRequestId(Request);

	UE_LOG(LogOliveAI, Verbose, TEXT("MCP Request: %s from %s"), *Method, *ClientId);

	// Update client activity
	UpdateClientActivity(ClientId);

	// Route to handler
	if (Method == TEXT("initialize"))
	{
		return OliveJsonRpc::CreateResponse(RequestId, HandleInitialize(Params, ClientId));
	}
	else if (Method == TEXT("initialized"))
	{
		// This is a notification, no response needed
		HandleInitialized(ClientId);
		return nullptr;
	}
	else if (Method == TEXT("tools/list"))
	{
		return OliveJsonRpc::CreateResponse(RequestId, HandleToolsList(Params));
	}
	else if (Method == TEXT("tools/call"))
	{
		return OliveJsonRpc::CreateResponse(RequestId, HandleToolsCall(Params, ClientId));
	}
	else if (Method == TEXT("resources/list"))
	{
		return OliveJsonRpc::CreateResponse(RequestId, HandleResourcesList(Params));
	}
	else if (Method == TEXT("resources/read"))
	{
		return OliveJsonRpc::CreateResponse(RequestId, HandleResourcesRead(Params));
	}
	else if (Method == TEXT("prompts/list"))
	{
		return OliveJsonRpc::CreateResponse(RequestId, HandlePromptsList(Params));
	}
	else if (Method == TEXT("prompts/get"))
	{
		return OliveJsonRpc::CreateResponse(RequestId, HandlePromptsGet(Params));
	}
	else if (Method == TEXT("ping"))
	{
		return OliveJsonRpc::CreateResponse(RequestId, HandlePing());
	}
	else
	{
		return OliveJsonRpc::CreateErrorResponse(
			RequestId,
			OliveJsonRpc::METHOD_NOT_FOUND,
			FString::Printf(TEXT("Method '%s' not found"), *Method)
		);
	}
}

void FOliveMCPServer::SendJsonResponse(
	const TSharedPtr<FJsonObject>& Response,
	const FHttpResultCallback& OnComplete,
	int32 HttpStatusCode)
{
	FString ResponseBody = OliveJsonRpc::Serialize(Response);

	TUniquePtr<FHttpServerResponse> HttpResponse = FHttpServerResponse::Create(
		ResponseBody,
		TEXT("application/json")
	);

	OnComplete(MoveTemp(HttpResponse));
}

// ==========================================
// MCP Method Handlers
// ==========================================

TSharedPtr<FJsonObject> FOliveMCPServer::HandleInitialize(
	const TSharedPtr<FJsonObject>& Params,
	const FString& ClientId)
{
	// Check max clients
	if (GetConnectedClientCount() >= MaxClients)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetNumberField(TEXT("max_clients"), MaxClients);
		ErrorData->SetNumberField(TEXT("current_clients"), GetConnectedClientCount());

		return OliveJsonRpc::CreateErrorResponse(
			nullptr,
			OliveJsonRpc::SERVER_BUSY,
			TEXT("Maximum number of clients reached"),
			ErrorData
		)->GetObjectField(TEXT("error"));
	}

	// Create client state
	FMCPClientState ClientState;
	ClientState.bInitialized = false;  // Will be true after 'initialized' notification
	ClientState.LastActivity = FDateTime::UtcNow();

	// Extract client info
	if (Params.IsValid())
	{
		const TSharedPtr<FJsonObject>* ClientInfo;
		if (Params->TryGetObjectField(TEXT("clientInfo"), ClientInfo))
		{
			ClientState.ClientName = (*ClientInfo)->GetStringField(TEXT("name"));
			ClientState.ClientVersion = (*ClientInfo)->GetStringField(TEXT("version"));
		}

		const TSharedPtr<FJsonObject>* Capabilities;
		if (Params->TryGetObjectField(TEXT("capabilities"), Capabilities))
		{
			ClientState.Capabilities = *Capabilities;
		}
	}

	// Store client state
	{
		FScopeLock Lock(&ClientsLock);
		ClientStates.Add(ClientId, ClientState);
	}

	UE_LOG(LogOliveAI, Log, TEXT("MCP Client initializing: %s (%s %s)"),
		*ClientId, *ClientState.ClientName, *ClientState.ClientVersion);

	// Build response
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("protocolVersion"), OLIVE_MCP_PROTOCOL_VERSION);

	// Server capabilities
	TSharedPtr<FJsonObject> ServerCapabilities = MakeShared<FJsonObject>();

	// Tools capability
	TSharedPtr<FJsonObject> ToolsCapability = MakeShared<FJsonObject>();
	ToolsCapability->SetBoolField(TEXT("listChanged"), true);
	ServerCapabilities->SetObjectField(TEXT("tools"), ToolsCapability);

	// Resources capability
	TSharedPtr<FJsonObject> ResourcesCapability = MakeShared<FJsonObject>();
	ResourcesCapability->SetBoolField(TEXT("listChanged"), true);
	ServerCapabilities->SetObjectField(TEXT("resources"), ResourcesCapability);

	// Prompts capability
	TSharedPtr<FJsonObject> PromptsCapability = MakeShared<FJsonObject>();
	ServerCapabilities->SetObjectField(TEXT("prompts"), PromptsCapability);

	Result->SetObjectField(TEXT("capabilities"), ServerCapabilities);

	// Server info
	TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
	ServerInfo->SetStringField(TEXT("name"), OLIVE_MCP_SERVER_NAME);
	ServerInfo->SetStringField(TEXT("version"), OLIVE_MCP_SERVER_VERSION);
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);

	return Result;
}

void FOliveMCPServer::HandleInitialized(const FString& ClientId)
{
	{
		FScopeLock Lock(&ClientsLock);

		FMCPClientState* ClientState = ClientStates.Find(ClientId);
		if (ClientState)
		{
			ClientState->bInitialized = true;
			UE_LOG(LogOliveAI, Log, TEXT("MCP Client initialized: %s"), *ClientId);
		}
	}

	OnClientConnected.Broadcast(ClientId);
}

TSharedPtr<FJsonObject> FOliveMCPServer::HandleToolsList(const TSharedPtr<FJsonObject>& Params)
{
	return FOliveToolRegistry::Get().GetToolsListMCP();
}

TSharedPtr<FJsonObject> FOliveMCPServer::HandleToolsCall(
	const TSharedPtr<FJsonObject>& Params,
	const FString& ClientId)
{
	if (!Params.IsValid())
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetBoolField(TEXT("isError"), true);

		TArray<TSharedPtr<FJsonValue>> Content;
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), TEXT("Missing parameters"));
		Content.Add(MakeShared<FJsonValueObject>(TextContent));

		ErrorResult->SetArrayField(TEXT("content"), Content);
		return ErrorResult;
	}

	// Extract tool name and arguments
	FString ToolName = Params->GetStringField(TEXT("name"));
	TSharedPtr<FJsonObject> Arguments = nullptr;

	const TSharedPtr<FJsonObject>* ArgsPtr;
	if (Params->TryGetObjectField(TEXT("arguments"), ArgsPtr))
	{
		Arguments = *ArgsPtr;
	}

	if (ToolName.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetBoolField(TEXT("isError"), true);

		TArray<TSharedPtr<FJsonValue>> Content;
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), TEXT("Tool name is required"));
		Content.Add(MakeShared<FJsonValueObject>(TextContent));

		ErrorResult->SetArrayField(TEXT("content"), Content);
		return ErrorResult;
	}

	// Fire event
	OnToolCalled.Broadcast(ToolName, ClientId);

	// Emit progress notification
	{
		TSharedPtr<FJsonObject> ProgressParams = MakeShared<FJsonObject>();
		ProgressParams->SetStringField(TEXT("tool"), ToolName);
		ProgressParams->SetStringField(TEXT("status"), TEXT("started"));
		SendNotification(TEXT("tools/progress"), ProgressParams, ClientId);
	}

	// Execute tool
	FOliveToolResult ToolResult = FOliveToolRegistry::Get().ExecuteTool(ToolName, Arguments);

	// Build MCP response
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Content;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), ToolResult.ToJsonString());
	Content.Add(MakeShared<FJsonValueObject>(TextContent));

	Result->SetArrayField(TEXT("content"), Content);

	if (!ToolResult.bSuccess)
	{
		Result->SetBoolField(TEXT("isError"), true);
	}


	// Emit completion notification
	{
		TSharedPtr<FJsonObject> CompleteParams = MakeShared<FJsonObject>();
		CompleteParams->SetStringField(TEXT("tool"), ToolName);
		CompleteParams->SetStringField(TEXT("status"), ToolResult.bSuccess ? TEXT("completed") : TEXT("failed"));
		SendNotification(TEXT("tools/progress"), CompleteParams, ClientId);
	}
	return Result;
}

TSharedPtr<FJsonObject> FOliveMCPServer::HandleResourcesList(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Define available resources
	TArray<TSharedPtr<FJsonValue>> Resources;

	// Project search resource
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://project/search"));
		Resource->SetStringField(TEXT("name"), TEXT("Project Asset Search"));
		Resource->SetStringField(TEXT("description"), TEXT("Search for assets in the project"));
		Resource->SetStringField(TEXT("mimeType"), TEXT("application/json"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Project config resource
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://project/config"));
		Resource->SetStringField(TEXT("name"), TEXT("Project Configuration"));
		Resource->SetStringField(TEXT("description"), TEXT("Project settings and configuration"));
		Resource->SetStringField(TEXT("mimeType"), TEXT("application/json"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Class hierarchy resource
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://project/class-hierarchy"));
		Resource->SetStringField(TEXT("name"), TEXT("Class Hierarchy"));
		Resource->SetStringField(TEXT("description"), TEXT("Blueprint and native class inheritance"));
		Resource->SetStringField(TEXT("mimeType"), TEXT("application/json"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Node catalog resource
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://blueprint/node-catalog"));
		Resource->SetStringField(TEXT("name"), TEXT("Blueprint Node Catalog"));
		Resource->SetStringField(TEXT("description"), TEXT("Available Blueprint node types with categories and metadata"));
		Resource->SetStringField(TEXT("mimeType"), TEXT("application/json"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Node catalog search resource
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://blueprint/node-catalog/search"));
		Resource->SetStringField(TEXT("name"), TEXT("Node Catalog Search"));
		Resource->SetStringField(TEXT("description"), TEXT("Search Blueprint node catalog by query (append ?q=<query>)"));
		Resource->SetStringField(TEXT("mimeType"), TEXT("application/json"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Behavior Tree node catalog resource
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://behaviortree/node-catalog"));
		Resource->SetStringField(TEXT("name"), TEXT("Behavior Tree Node Catalog"));
		Resource->SetStringField(TEXT("description"), TEXT("Available BT task/decorator/service classes with metadata"));
		Resource->SetStringField(TEXT("mimeType"), TEXT("application/json"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Behavior Tree node catalog search resource
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://behaviortree/node-catalog/search"));
		Resource->SetStringField(TEXT("name"), TEXT("Behavior Tree Node Catalog Search"));
		Resource->SetStringField(TEXT("description"), TEXT("Search BT node catalog by query (append ?q=<query>)"));
		Resource->SetStringField(TEXT("mimeType"), TEXT("application/json"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	Result->SetArrayField(TEXT("resources"), Resources);

	return Result;
}

TSharedPtr<FJsonObject> FOliveMCPServer::HandleResourcesRead(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return OliveJsonRpc::CreateErrorResponse(
			nullptr,
			OliveJsonRpc::INVALID_PARAMS,
			TEXT("Missing parameters")
		)->GetObjectField(TEXT("error"));
	}

	FString Uri = Params->GetStringField(TEXT("uri"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Contents;

	FString ContentText;
	FString MimeType = TEXT("application/json");
	auto JsonToString = [](const TSharedPtr<FJsonObject>& Json) -> FString
	{
		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
		return Output;
	};

	if (Uri == TEXT("olive://project/config"))
	{
		ContentText = FOliveProjectIndex::Get().GetProjectConfigJson();
	}
	else if (Uri == TEXT("olive://project/class-hierarchy"))
	{
		ContentText = FOliveProjectIndex::Get().GetClassHierarchyJson(NAME_None);
	}
	else if (Uri.StartsWith(TEXT("olive://project/search")))
	{
		// Parse query from URI
		FString Query;
		int32 QueryStart = Uri.Find(TEXT("?q="));
		if (QueryStart != INDEX_NONE)
		{
			Query = Uri.RightChop(QueryStart + 3);
		}

		ContentText = FOliveProjectIndex::Get().GetSearchResultsJson(Query, 50);
	}
	else if (Uri.StartsWith(TEXT("olive://blueprint/node-catalog/search")))
	{
		// Parse query from URI
		FString Query;
		int32 QueryStart = Uri.Find(TEXT("?q="));
		if (QueryStart != INDEX_NONE)
		{
			Query = Uri.RightChop(QueryStart + 3);
		}

		ContentText = FOliveNodeCatalog::Get().SearchToJson(Query);
	}
	else if (Uri == TEXT("olive://blueprint/node-catalog"))
	{
		ContentText = FOliveNodeCatalog::Get().ToJson();
	}
	else if (Uri.StartsWith(TEXT("olive://behaviortree/node-catalog/search")))
	{
		FString Query;
		int32 QueryStart = Uri.Find(TEXT("?q="));
		if (QueryStart != INDEX_NONE)
		{
			Query = Uri.RightChop(QueryStart + 3);
		}

		TArray<FOliveBTNodeTypeInfo> SearchResults = FOliveBTNodeCatalog::Get().Search(Query);
		TSharedPtr<FJsonObject> SearchJson = MakeShared<FJsonObject>();
		SearchJson->SetStringField(TEXT("query"), Query);
		SearchJson->SetNumberField(TEXT("total_results"), SearchResults.Num());

		TArray<TSharedPtr<FJsonValue>> ResultsArray;
		for (const FOliveBTNodeTypeInfo& Info : SearchResults)
		{
			ResultsArray.Add(MakeShared<FJsonValueObject>(Info.ToJson()));
		}
		SearchJson->SetArrayField(TEXT("results"), ResultsArray);

		ContentText = JsonToString(SearchJson);
	}
	else if (Uri == TEXT("olive://behaviortree/node-catalog"))
	{
		ContentText = JsonToString(FOliveBTNodeCatalog::Get().ToJson());
	}
	else
	{
		return OliveJsonRpc::CreateErrorResponse(
			nullptr,
			OliveJsonRpc::RESOURCE_NOT_FOUND,
			FString::Printf(TEXT("Resource not found: %s"), *Uri)
		)->GetObjectField(TEXT("error"));
	}

	TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
	Content->SetStringField(TEXT("uri"), Uri);
	Content->SetStringField(TEXT("mimeType"), MimeType);
	Content->SetStringField(TEXT("text"), ContentText);
	Contents.Add(MakeShared<FJsonValueObject>(Content));

	Result->SetArrayField(TEXT("contents"), Contents);

	return Result;
}

TSharedPtr<FJsonObject> FOliveMCPServer::HandlePromptsList(const TSharedPtr<FJsonObject>& Params)
{
	return FOliveMCPPromptTemplates::Get().GetPromptsList();
}

TSharedPtr<FJsonObject> FOliveMCPServer::HandlePromptsGet(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return OliveJsonRpc::CreateErrorResponse(
			nullptr,
			OliveJsonRpc::INVALID_PARAMS,
			TEXT("Missing parameters")
		)->GetObjectField(TEXT("error"));
	}

	FString Name = Params->GetStringField(TEXT("name"));

	if (!FOliveMCPPromptTemplates::Get().HasTemplate(Name))
	{
		return OliveJsonRpc::CreateErrorResponse(
			nullptr,
			OliveJsonRpc::INVALID_PARAMS,
			FString::Printf(TEXT("Unknown prompt: %s"), *Name)
		)->GetObjectField(TEXT("error"));
	}

	TSharedPtr<FJsonObject> Arguments = nullptr;
	const TSharedPtr<FJsonObject>* ArgsPtr;
	if (Params->TryGetObjectField(TEXT("arguments"), ArgsPtr))
	{
		Arguments = *ArgsPtr;
	}

	return FOliveMCPPromptTemplates::Get().GetPrompt(Name, Arguments);
}

TSharedPtr<FJsonObject> FOliveMCPServer::HandlePing()
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
	return Result;
}

// ==========================================
// Client Management Internals
// ==========================================

FString FOliveMCPServer::GenerateClientId() const
{
	return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
}

FString FOliveMCPServer::ExtractClientId(const FHttpServerRequest& Request) const
{
	// Try to get from custom header first
	const TArray<FString>* ClientIdHeader = Request.Headers.Find(TEXT("X-MCP-Client-Id"));
	if (ClientIdHeader && ClientIdHeader->Num() > 0)
	{
		return (*ClientIdHeader)[0];
	}

	// Generate new ID
	return GenerateClientId();
}

void FOliveMCPServer::UpdateClientActivity(const FString& ClientId)
{
	FScopeLock Lock(&ClientsLock);

	FMCPClientState* ClientState = ClientStates.Find(ClientId);
	if (ClientState)
	{
		ClientState->LastActivity = FDateTime::UtcNow();
	}
}

void FOliveMCPServer::CleanupInactiveClients()
{
	FDateTime Now = FDateTime::UtcNow();
	TArray<FString> ClientsToRemove;

	{
		FScopeLock Lock(&ClientsLock);

		for (const auto& Pair : ClientStates)
		{
			FTimespan Inactive = Now - Pair.Value.LastActivity;
			if (Inactive.GetTotalSeconds() > ClientTimeoutSeconds)
			{
				ClientsToRemove.Add(Pair.Key);
			}
		}

		for (const FString& ClientId : ClientsToRemove)
		{
			ClientStates.Remove(ClientId);
			UE_LOG(LogOliveAI, Log, TEXT("MCP Client timed out: %s"), *ClientId);
		}
	}

	// Fire disconnect events outside lock
	for (const FString& ClientId : ClientsToRemove)
	{
		OnClientDisconnected.Broadcast(ClientId);
	}
}

// ==========================================
// Notifications
// ==========================================

void FOliveMCPServer::SendNotification(
	const FString& Method,
	const TSharedPtr<FJsonObject>& Params,
	const FString& ClientId)
{
	FScopeLock Lock(&EventLock);

	FMCPNotificationEvent Event;
	Event.EventId = NextEventId++;
	Event.Method = Method;
	Event.Params = Params;
	Event.TargetClientId = ClientId;
	Event.Timestamp = FDateTime::UtcNow();

	EventBuffer.Add(Event);

	// Prune old events if buffer is too large
	if (EventBuffer.Num() > MaxEventBufferSize)
	{
		PruneEventBuffer();
	}

	UE_LOG(LogOliveAI, Verbose, TEXT("MCP Notification buffered: %s (event %lld, target: %s)"),
		*Method, Event.EventId, ClientId.IsEmpty() ? TEXT("broadcast") : *ClientId);
}

// ==========================================
// Event Polling
// ==========================================

bool FOliveMCPServer::HandleEventsPoll(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Parse query parameters from URL
	// Expected: /mcp/events?cursor=N&client_id=X
	int64 Cursor = 0;
	FString ClientId;

	// Extract query params from the request path
	for (const auto& Pair : Request.QueryParams)
	{
		if (Pair.Key == TEXT("cursor"))
		{
			Cursor = FCString::Atoi64(*Pair.Value);
		}
		else if (Pair.Key == TEXT("client_id"))
		{
			ClientId = Pair.Value;
		}
	}

	// Collect matching events
	TArray<TSharedPtr<FJsonValue>> EventsArray;
	int64 MaxEventId = Cursor;

	{
		FScopeLock Lock(&EventLock);

		for (const FMCPNotificationEvent& Event : EventBuffer)
		{
			if (Event.EventId <= Cursor)
			{
				continue;
			}

			// Filter by client: include broadcast events and events targeted at this client
			if (!Event.TargetClientId.IsEmpty() && !ClientId.IsEmpty() && Event.TargetClientId != ClientId)
			{
				continue;
			}

			EventsArray.Add(MakeShared<FJsonValueObject>(Event.ToJson()));

			if (Event.EventId > MaxEventId)
			{
				MaxEventId = Event.EventId;
			}
		}
	}

	// Build response
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetArrayField(TEXT("events"), EventsArray);
	Response->SetNumberField(TEXT("next_cursor"), static_cast<double>(MaxEventId));
	Response->SetNumberField(TEXT("event_count"), EventsArray.Num());

	FString ResponseBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseBody);
	FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);

	TUniquePtr<FHttpServerResponse> HttpResponse = FHttpServerResponse::Create(
		ResponseBody,
		TEXT("application/json")
	);

	OnComplete(MoveTemp(HttpResponse));
	return true;
}

void FOliveMCPServer::PruneEventBuffer()
{
	// Called with EventLock already held
	const FDateTime CutoffTime = FDateTime::UtcNow() - FTimespan::FromSeconds(EventRetentionSeconds);

	EventBuffer.RemoveAll([&CutoffTime](const FMCPNotificationEvent& Event)
	{
		return Event.Timestamp < CutoffTime;
	});

	// If still too large, remove oldest events
	while (EventBuffer.Num() > MaxEventBufferSize)
	{
		EventBuffer.RemoveAt(0);
	}
}
