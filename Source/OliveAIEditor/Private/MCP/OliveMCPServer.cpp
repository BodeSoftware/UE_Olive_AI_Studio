// Copyright Bode Software. All Rights Reserved.

#include "MCP/OliveMCPServer.h"
#include "MCP/OliveJsonRpc.h"
#include "MCP/OliveToolRegistry.h"
#include "OliveMCPPromptTemplates.h"
#include "Index/OliveProjectIndex.h"
#include "Catalog/OliveNodeCatalog.h"
#include "Catalog/OliveBTNodeCatalog.h"
#include "OliveAIEditorModule.h"
#include "Brain/OliveToolExecutionContext.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Async/Async.h"

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

	// Write .mcp.json so Claude Code CLI can discover the bridge
	WriteMcpConfigFile();

	return true;
}

void FOliveMCPServer::Stop()
{
	if (State != EOliveMCPServerState::Running)
	{
		return;
	}

	State = EOliveMCPServerState::Stopping;

	// Remove .mcp.json so stale config doesn't persist
	CleanupMcpConfigFile();

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
// .mcp.json Management
// ==========================================

void FOliveMCPServer::WriteMcpConfigFile()
{
	const FString PluginDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
	const FString ConfigPath = FPaths::Combine(PluginDir, TEXT(".mcp.json"));

	// Match the existing bridge format: command + args.
	// mcp-bridge.js auto-discovers the server on ports 3000-3009.
	const FString ConfigContent = FString::Printf(
		TEXT("{\n")
		TEXT("  \"mcpServers\": {\n")
		TEXT("    \"olive-ai-studio\": {\n")
		TEXT("      \"command\": \"node\",\n")
		TEXT("      \"args\": [\"mcp-bridge.js\"]\n")
		TEXT("    }\n")
		TEXT("  }\n")
		TEXT("}\n")
	);

	if (FFileHelper::SaveStringToFile(ConfigContent, *ConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogOliveAI, Log, TEXT("Wrote .mcp.json to %s"), *ConfigPath);
	}
	else
	{
		UE_LOG(LogOliveAI, Warning, TEXT("Failed to write .mcp.json to %s"), *ConfigPath);
	}
}

void FOliveMCPServer::CleanupMcpConfigFile()
{
	const FString PluginDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio")));
	const FString ConfigPath = FPaths::Combine(PluginDir, TEXT(".mcp.json"));

	if (IFileManager::Get().FileExists(*ConfigPath))
	{
		IFileManager::Get().Delete(*ConfigPath);
		UE_LOG(LogOliveAI, Log, TEXT("Cleaned up .mcp.json at %s"), *ConfigPath);
	}
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

	// Route tools/call to async handler (needs game thread for UE API calls)
	FString Method = OliveJsonRpc::GetMethod(JsonRequest);
	if (Method == TEXT("tools/call") && !OliveJsonRpc::IsNotification(JsonRequest))
	{
		TSharedPtr<FJsonObject> Params = OliveJsonRpc::GetParams(JsonRequest);
		TSharedPtr<FJsonValue> RequestId = OliveJsonRpc::GetRequestId(JsonRequest);
		UpdateClientActivity(ClientId);
		HandleToolsCallAsync(Params, ClientId, RequestId, OnComplete);
		return true;
	}

	// Process the request
	TSharedPtr<FJsonObject> Response = ProcessJsonRpcRequest(JsonRequest, ClientId);

	// Notifications don't get responses — return 202 Accepted per Streamable HTTP MCP spec
	if (OliveJsonRpc::IsNotification(JsonRequest))
	{
		TUniquePtr<FHttpServerResponse> HttpResponse = MakeUnique<FHttpServerResponse>();
		HttpResponse->Code = EHttpServerResponseCodes::Accepted;
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

	UE_LOG(LogOliveAI, Log, TEXT("MCP Request: %s from %s"), *Method, *ClientId);

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
	TSharedPtr<FJsonObject> FullResult = FOliveToolRegistry::Get().GetToolsListMCP();

	// Apply tool filter if active
	TSet<FString> ActiveFilter;
	{
		FScopeLock Lock(&ToolFilterLock);
		ActiveFilter = ToolFilterPrefixes;
	}

	if (ActiveFilter.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FilteredTools;
		const TArray<TSharedPtr<FJsonValue>>* AllTools = nullptr;
		if (FullResult.IsValid() && FullResult->TryGetArrayField(TEXT("tools"), AllTools))
		{
			for (const TSharedPtr<FJsonValue>& ToolVal : *AllTools)
			{
				const TSharedPtr<FJsonObject>* ToolObj;
				if (ToolVal->TryGetObject(ToolObj) && ToolObj)
				{
					FString ToolName;
					(*ToolObj)->TryGetStringField(TEXT("name"), ToolName);

					bool bAllowed = false;
					for (const FString& Prefix : ActiveFilter)
					{
						if (ToolName.StartsWith(Prefix))
						{
							bAllowed = true;
							break;
						}
					}

					if (bAllowed)
					{
						FilteredTools.Add(ToolVal);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> FilteredResult = MakeShared<FJsonObject>();
		FilteredResult->SetArrayField(TEXT("tools"), FilteredTools);

		UE_LOG(LogOliveAI, Log, TEXT("MCP tools/list: returning %d/%d tools (filtered by %d prefixes)"),
			FilteredTools.Num(),
			AllTools ? AllTools->Num() : 0,
			ActiveFilter.Num());

		return FilteredResult;
	}

	// No filter -- return all tools
	int32 ToolCount = 0;
	const TArray<TSharedPtr<FJsonValue>>* ToolsArray;
	if (FullResult.IsValid() && FullResult->TryGetArrayField(TEXT("tools"), ToolsArray))
	{
		ToolCount = ToolsArray->Num();
	}
	UE_LOG(LogOliveAI, Log, TEXT("MCP tools/list: returning %d tools"), ToolCount);

	return FullResult;
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

	// Fire event (pass Arguments so subscribers can inspect tool call parameters)
	OnToolCalled.Broadcast(ToolName, ClientId, Arguments);

	// Emit progress notification
	{
		TSharedPtr<FJsonObject> ProgressParams = MakeShared<FJsonObject>();
		ProgressParams->SetStringField(TEXT("tool"), ToolName);
		ProgressParams->SetStringField(TEXT("status"), TEXT("started"));
		SendNotification(TEXT("tools/progress"), ProgressParams, ClientId);
	}

	// Capture start time before execution for duration tracking
	const double ToolStartTime = FPlatformTime::Seconds();

	// Execute tool with MCP origin context.
	// If an in-engine autonomous agent is active, propagate its chat mode
	// so the write pipeline's mode gate can enforce Plan/Ask restrictions.
	// External MCP agents default to Code mode (unrestricted).
	FOliveToolCallContext ToolContext;
	ToolContext.Origin = EOliveToolCallOrigin::MCP;
	ToolContext.SessionId = ClientId;
	if (bHasInternalAgent)
	{
		ToolContext.ChatMode = InternalAgentChatMode;
	}
	FOliveToolExecutionContextScope ContextScope(ToolContext);

	FOliveToolResult ToolResult = FOliveToolRegistry::Get().ExecuteTool(ToolName, Arguments);

	// Build MCP response
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	const FString ToolResultText = ToolResult.ToJsonString();

	TArray<TSharedPtr<FJsonValue>> Content;
	TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
	TextContent->SetStringField(TEXT("type"), TEXT("text"));
	TextContent->SetStringField(TEXT("text"), ToolResultText);
	Content.Add(MakeShared<FJsonValueObject>(TextContent));

	Result->SetArrayField(TEXT("content"), Content);

	if (!ToolResult.bSuccess)
	{
		Result->SetBoolField(TEXT("isError"), true);
	}

	// Emit completion notification with timing and summary
	const double DurationMs = (FPlatformTime::Seconds() - ToolStartTime) * 1000.0;
	{
		const FString Summary = ToolResultText.Left(200);

		TSharedPtr<FJsonObject> CompleteParams = MakeShared<FJsonObject>();
		CompleteParams->SetStringField(TEXT("tool"), ToolName);
		CompleteParams->SetStringField(TEXT("status"), ToolResult.bSuccess ? TEXT("completed") : TEXT("failed"));
		CompleteParams->SetNumberField(TEXT("duration_ms"), static_cast<int32>(DurationMs));
		CompleteParams->SetStringField(TEXT("summary"), Summary);
		SendNotification(TEXT("tools/progress"), CompleteParams, ClientId);
	}

	// Record in activity ring buffer and fire OnToolCompleted delegate
	RecordToolCompletion(ToolName, ClientId, ToolResult.bSuccess, DurationMs);

	return Result;
}

void FOliveMCPServer::HandleToolsCallAsync(
	const TSharedPtr<FJsonObject>& Params,
	const FString& ClientId,
	const TSharedPtr<FJsonValue>& RequestId,
	const FHttpResultCallback& OnComplete)
{
	// Validate params
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
		SendJsonResponse(OliveJsonRpc::CreateResponse(RequestId, ErrorResult), OnComplete);
		return;
	}

	// Extract tool name
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
		SendJsonResponse(OliveJsonRpc::CreateResponse(RequestId, ErrorResult), OnComplete);
		return;
	}

	UE_LOG(LogOliveAI, Log, TEXT("MCP tools/call: %s (client: %s)"), *ToolName, *ClientId);

	// Log tool call parameters for diagnostics
	if (Arguments.IsValid())
	{
		FString ParamStr;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ParamStr);
		FJsonSerializer::Serialize(Arguments.ToSharedRef(), Writer);
		// Truncate very long params (e.g. plan_json bodies) to keep logs readable
		if (ParamStr.Len() > 500)
		{
			ParamStr = ParamStr.Left(500) + TEXT("...(truncated)");
		}
		UE_LOG(LogOliveAI, Log, TEXT("MCP tools/call params: %s"), *ParamStr);
	}

	// Fire event (pass Arguments so subscribers can inspect tool call parameters)
	OnToolCalled.Broadcast(ToolName, ClientId, Arguments);

	// Emit progress notification
	{
		TSharedPtr<FJsonObject> ProgressParams = MakeShared<FJsonObject>();
		ProgressParams->SetStringField(TEXT("tool"), ToolName);
		ProgressParams->SetStringField(TEXT("status"), TEXT("started"));
		SendNotification(TEXT("tools/progress"), ProgressParams, ClientId);
	}

	// Capture start time before dispatching to game thread for accurate wall-clock duration
	const double ToolStartTime = FPlatformTime::Seconds();

	// Capture internal agent state before dispatching -- these are read on the HTTP
	// thread but only written on the game thread, so the snapshot is safe.
	const bool bCapturedHasInternalAgent = bHasInternalAgent;
	const EOliveChatMode CapturedChatMode = InternalAgentChatMode;

	// Dispatch tool execution to the game thread
	AsyncTask(ENamedThreads::GameThread, [this, ToolName, Arguments, ClientId, RequestId, OnComplete, ToolStartTime, bCapturedHasInternalAgent, CapturedChatMode]()
	{
		// Set up execution context on the game thread.
		// Propagate internal agent chat mode so the write pipeline's mode gate
		// can enforce Plan/Ask restrictions for in-engine autonomous runs.
		FOliveToolCallContext ToolContext;
		ToolContext.Origin = EOliveToolCallOrigin::MCP;
		ToolContext.SessionId = ClientId;
		if (bCapturedHasInternalAgent)
		{
			ToolContext.ChatMode = CapturedChatMode;
		}
		FOliveToolExecutionContextScope ContextScope(ToolContext);

		FOliveToolResult ToolResult = FOliveToolRegistry::Get().ExecuteTool(ToolName, Arguments);

		UE_LOG(LogOliveAI, Log, TEXT("MCP tools/call result: %s -> %s"), *ToolName, ToolResult.bSuccess ? TEXT("SUCCESS") : TEXT("FAILED"));

		// Build MCP response
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		const FString ToolResultText = ToolResult.ToJsonString();

		TArray<TSharedPtr<FJsonValue>> Content;
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), ToolResultText);
		Content.Add(MakeShared<FJsonValueObject>(TextContent));

		Result->SetArrayField(TEXT("content"), Content);

		if (!ToolResult.bSuccess)
		{
			Result->SetBoolField(TEXT("isError"), true);
		}

		// Emit completion notification with timing and summary
		const double DurationMs = (FPlatformTime::Seconds() - ToolStartTime) * 1000.0;
		{
			const FString Summary = ToolResultText.Left(200);

			TSharedPtr<FJsonObject> CompleteParams = MakeShared<FJsonObject>();
			CompleteParams->SetStringField(TEXT("tool"), ToolName);
			CompleteParams->SetStringField(TEXT("status"), ToolResult.bSuccess ? TEXT("completed") : TEXT("failed"));
			CompleteParams->SetNumberField(TEXT("duration_ms"), static_cast<int32>(DurationMs));
			CompleteParams->SetStringField(TEXT("summary"), Summary);
			SendNotification(TEXT("tools/progress"), CompleteParams, ClientId);
		}

		// Record in activity ring buffer and fire OnToolCompleted delegate
		RecordToolCompletion(ToolName, ClientId, ToolResult.bSuccess, DurationMs);

		SendJsonResponse(OliveJsonRpc::CreateResponse(RequestId, Result), OnComplete);
	});
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

	// ==========================================
	// Domain Knowledge Resources
	// ==========================================

	// Knowledge: Events vs Functions
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/events-vs-functions"));
		Resource->SetStringField(TEXT("name"), TEXT("Events vs Functions"));
		Resource->SetStringField(TEXT("description"),
			TEXT("When to implement logic as Event Graphs (BeginPlay, Tick, custom events) vs Function Graphs in Blueprints. Decision criteria and common mistakes."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Knowledge: Blueprint Design Patterns
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/blueprint-patterns"));
		Resource->SetStringField(TEXT("name"), TEXT("Blueprint Design Patterns"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Reusable UE5 Blueprint architecture patterns: component composition, interface communication, event dispatchers, inheritance strategies."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Knowledge: Blueprint Authoring
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/blueprint-authoring"));
		Resource->SetStringField(TEXT("name"), TEXT("Blueprint Authoring Rules"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Blueprint authoring rules: naming conventions, variable organization, function decomposition, compile-error-first debugging."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Knowledge: Node Routing
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/node-routing"));
		Resource->SetStringField(TEXT("name"), TEXT("Node Routing Guide"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Guide for choosing the right Blueprint node type: K2Node subclasses, macro vs function, pure vs impure, latent actions."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// ==========================================
	// Recipe Resources
	// ==========================================

	// Recipe: Create
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/create"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Create Blueprint"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Step-by-step recipe for creating a new Blueprint from scratch."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Modify
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/modify"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Modify Blueprint"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Step-by-step recipe for modifying an existing Blueprint's logic."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Fix Wiring
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/fix_wiring"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Fix Wiring"));
		Resource->SetStringField(TEXT("description"),
			TEXT("How to diagnose and fix pin wiring issues in Blueprint graphs using pin_manifests from tool results."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Spawn Actor
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/spawn_actor"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Spawn Actor"));
		Resource->SetStringField(TEXT("description"),
			TEXT("SpawnActor node usage with SpawnTransform pin and MakeTransform pattern."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Interface Pattern
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/interface_pattern"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Interface Pattern"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Blueprint Interface (BPI) creation, implementation, and interface-based calling pattern."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Multi Asset
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/multi_asset"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Multi-Asset Workflow"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Multi-asset workflow: creating multiple related Blueprints with cross-asset references and build order."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Variables and Components
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/variables_components"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Variables & Components"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Add variables and components with correct type format and configuration."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Edit Existing Graph
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/edit_existing_graph"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Edit Existing Graph"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Insert, remove, or rewire nodes in an existing event graph or function graph."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Function Graph
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/function_graph"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Function Graph"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Function graph entry node, auto-chain, graph_target, and return values."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Object Variable Type
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/object_variable_type"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Object Variable Types"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Variable type format for object refs, class refs, arrays, and enums."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Component Reference
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/component_reference"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Component Reference"));
		Resource->SetStringField(TEXT("description"),
			TEXT("How to access component properties and transforms using GetComponentByClass."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Events and Functions
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/events_and_functions"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Events and Functions"));
		Resource->SetStringField(TEXT("description"),
			TEXT("When to use events vs functions, interface output traps, hybrid pattern, smooth movement with Tick+FInterpTo."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		Resources.Add(MakeShared<FJsonValueObject>(Resource));
	}

	// Recipe: Input Handling
	{
		TSharedPtr<FJsonObject> Resource = MakeShared<FJsonObject>();
		Resource->SetStringField(TEXT("uri"), TEXT("olive://knowledge/recipe/input_handling"));
		Resource->SetStringField(TEXT("name"), TEXT("Recipe: Input Handling"));
		Resource->SetStringField(TEXT("description"),
			TEXT("Enhanced Input Actions vs InputKey decision tree, tool split for asset creation vs wiring."));
		Resource->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
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
	else if (Uri.StartsWith(TEXT("olive://knowledge/")))
	{
		// Map knowledge URI to file path under Content/SystemPrompts/Knowledge/
		FString RelPath = Uri.RightChop(18); // strip "olive://knowledge/"

		const FString KnowledgeDir = FPaths::Combine(
			FPaths::ConvertRelativePathToFull(
				FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio"))),
			TEXT("Content"), TEXT("SystemPrompts"), TEXT("Knowledge"));

		FString FilePath;
		if (RelPath.StartsWith(TEXT("recipe/")))
		{
			// olive://knowledge/recipe/create -> Knowledge/recipes/blueprint/create.txt
			FString RecipeName = RelPath.RightChop(7); // strip "recipe/"
			FilePath = FPaths::Combine(KnowledgeDir, TEXT("recipes"), TEXT("blueprint"), RecipeName + TEXT(".txt"));
		}
		else
		{
			// olive://knowledge/events-vs-functions -> Knowledge/events_vs_functions.txt
			FString FileName = RelPath.Replace(TEXT("-"), TEXT("_"));
			FilePath = FPaths::Combine(KnowledgeDir, FileName + TEXT(".txt"));
		}

		if (FPaths::FileExists(FilePath))
		{
			FFileHelper::LoadFileToString(ContentText, *FilePath);
			MimeType = TEXT("text/plain");
		}
		else
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Knowledge resource file not found: %s (URI: %s)"), *FilePath, *Uri);
			return OliveJsonRpc::CreateErrorResponse(
				nullptr,
				OliveJsonRpc::RESOURCE_NOT_FOUND,
				FString::Printf(TEXT("Knowledge resource not found: %s"), *Uri)
			)->GetObjectField(TEXT("error"));
		}
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

			// Skip events targeted at a different client
			// Broadcast events (empty TargetClientId) are always included
			// Targeted events are only included if the client ID matches
			bool bIsBroadcast = Event.TargetClientId.IsEmpty();
			bool bIsTargeted = !Event.TargetClientId.IsEmpty() && Event.TargetClientId == ClientId;
			if (!bIsBroadcast && !bIsTargeted)
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

// ==========================================
// Tool Filtering
// ==========================================

void FOliveMCPServer::SetToolFilter(const TSet<FString>& AllowedPrefixes)
{
	FScopeLock Lock(&ToolFilterLock);
	ToolFilterPrefixes = AllowedPrefixes;
	UE_LOG(LogOliveAI, Log, TEXT("MCP tool filter set: %d prefixes"), AllowedPrefixes.Num());
}

void FOliveMCPServer::ClearToolFilter()
{
	FScopeLock Lock(&ToolFilterLock);
	ToolFilterPrefixes.Empty();
	UE_LOG(LogOliveAI, Log, TEXT("MCP tool filter cleared"));
}

void FOliveMCPServer::SetChatModeForInternalAgent(EOliveChatMode Mode)
{
	InternalAgentChatMode = Mode;
	bHasInternalAgent = true;
	UE_LOG(LogOliveAI, Log, TEXT("MCP internal agent chat mode set to %s"), LexToString(Mode));
}

void FOliveMCPServer::ClearChatModeForInternalAgent()
{
	InternalAgentChatMode = EOliveChatMode::Code;
	bHasInternalAgent = false;
	UE_LOG(LogOliveAI, Log, TEXT("MCP internal agent chat mode cleared"));
}

// ==========================================
// Recent Tool Calls
// ==========================================

TArray<FOliveMCPServer::FRecentToolCall> FOliveMCPServer::GetRecentToolCalls(int32 MaxCount) const
{
	FScopeLock Lock(&RecentToolCallsLock);

	const int32 Count = FMath::Min(MaxCount, RecentToolCalls.Num());
	TArray<FRecentToolCall> Result;
	Result.Reserve(Count);

	// Return newest-first (ring buffer stores oldest-first, so iterate in reverse)
	for (int32 i = RecentToolCalls.Num() - 1; i >= 0 && Result.Num() < Count; --i)
	{
		Result.Add(RecentToolCalls[i]);
	}

	return Result;
}

void FOliveMCPServer::RecordToolCompletion(const FString& ToolName, const FString& ClientId, bool bSuccess, double DurationMs)
{
	// Add to ring buffer
	{
		FScopeLock Lock(&RecentToolCallsLock);

		FRecentToolCall Entry;
		Entry.Timestamp = FDateTime::UtcNow();
		Entry.ToolName = ToolName;
		Entry.ClientId = ClientId;
		Entry.bSuccess = bSuccess;
		Entry.DurationMs = DurationMs;

		RecentToolCalls.Add(MoveTemp(Entry));

		// Trim if over capacity
		while (RecentToolCalls.Num() > MaxRecentToolCalls)
		{
			RecentToolCalls.RemoveAt(0);
		}
	}

	// Fire delegate (we are already on game thread for async path,
	// and on game thread for sync path)
	OnToolCompleted.Broadcast(ToolName, ClientId, bSuccess, DurationMs);
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
