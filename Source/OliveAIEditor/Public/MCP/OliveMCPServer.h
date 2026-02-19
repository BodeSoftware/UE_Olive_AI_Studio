// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "OliveMCPServer.generated.h"

/**
 * MCP Server State
 */
UENUM()
enum class EOliveMCPServerState : uint8
{
	Stopped,
	Starting,
	Running,
	Stopping,
	Error
};

/**
 * MCP Protocol Version
 */
#define OLIVE_MCP_PROTOCOL_VERSION TEXT("2024-11-05")

/**
 * MCP Server Info
 */
#define OLIVE_MCP_SERVER_NAME TEXT("olive-ai-studio")
#define OLIVE_MCP_SERVER_VERSION TEXT("0.1.0")

/**
 * Client Connected Event
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnMCPClientConnected, const FString& /* ClientId */);

/**
 * Client Disconnected Event
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnMCPClientDisconnected, const FString& /* ClientId */);

/**
 * Tool Called Event
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMCPToolCalled, const FString& /* ToolName */, const FString& /* ClientId */);

/**
 * MCP Client State
 *
 * Tracks per-client protocol state.
 */
USTRUCT()
struct FMCPClientState
{
	GENERATED_BODY()

	/** Whether the client has completed initialization */
	UPROPERTY()
	bool bInitialized = false;

	/** Client name from initialization */
	UPROPERTY()
	FString ClientName;

	/** Client version from initialization */
	UPROPERTY()
	FString ClientVersion;

	/** Last activity timestamp */
	FDateTime LastActivity;

	/** Client capabilities */
	TSharedPtr<FJsonObject> Capabilities;
};

/**
 * MCP Server
 *
 * HTTP server implementing the Model Context Protocol (MCP) for
 * external AI agent integration (Claude Code, Cursor, etc.).
 *
 * Supports:
 * - JSON-RPC 2.0 over HTTP POST
 * - MCP protocol handshake (initialize/initialized)
 * - Tools listing and execution
 * - Resources listing and reading
 */
class OLIVEAIEDITOR_API FOliveMCPServer
{
public:
	/** Get singleton instance */
	static FOliveMCPServer& Get();

	// ==========================================
	// Lifecycle
	// ==========================================

	/**
	 * Start the MCP server
	 * @param Port Port to listen on (tries alternatives if unavailable)
	 * @return true if started successfully
	 */
	bool Start(int32 Port = 3000);

	/**
	 * Stop the server
	 */
	void Stop();

	/**
	 * Get current server state
	 */
	EOliveMCPServerState GetState() const { return State; }

	/**
	 * Get the actual port the server is listening on
	 * May differ from requested port if that was in use
	 */
	int32 GetActualPort() const { return ActualPort; }

	/**
	 * Check if server is running
	 */
	bool IsRunning() const { return State == EOliveMCPServerState::Running; }

	// ==========================================
	// Events
	// ==========================================

	/** Fired when a client connects and initializes */
	FOnMCPClientConnected OnClientConnected;

	/** Fired when a client disconnects */
	FOnMCPClientDisconnected OnClientDisconnected;

	/** Fired when a tool is called */
	FOnMCPToolCalled OnToolCalled;

	// ==========================================
	// Client Management
	// ==========================================

	/**
	 * Get list of connected client IDs
	 */
	TArray<FString> GetConnectedClients() const;

	/**
	 * Get client state by ID
	 */
	TOptional<FMCPClientState> GetClientState(const FString& ClientId) const;

	/**
	 * Get connected client count
	 */
	int32 GetConnectedClientCount() const;

	// ==========================================
	// Notifications
	// ==========================================

	/**
	 * Send a notification to a specific client or broadcast
	 * @param Method Notification method name
	 * @param Params Notification parameters
	 * @param ClientId Target client (empty = broadcast)
	 */
	void SendNotification(
		const FString& Method,
		const TSharedPtr<FJsonObject>& Params,
		const FString& ClientId = TEXT("")
	);

private:
	FOliveMCPServer();
	~FOliveMCPServer();

	// Prevent copying
	FOliveMCPServer(const FOliveMCPServer&) = delete;
	FOliveMCPServer& operator=(const FOliveMCPServer&) = delete;

	// ==========================================
	// HTTP Handling
	// ==========================================

	/** Handle incoming HTTP request */
	bool HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** Process JSON-RPC request */
	TSharedPtr<FJsonObject> ProcessJsonRpcRequest(
		const TSharedPtr<FJsonObject>& Request,
		const FString& ClientId
	);

	/** Send JSON response */
	void SendJsonResponse(
		const TSharedPtr<FJsonObject>& Response,
		const FHttpResultCallback& OnComplete,
		int32 HttpStatusCode = 200
	);

	// ==========================================
	// MCP Method Handlers
	// ==========================================

	/** Handle initialize request */
	TSharedPtr<FJsonObject> HandleInitialize(
		const TSharedPtr<FJsonObject>& Params,
		const FString& ClientId
	);

	/** Handle initialized notification */
	void HandleInitialized(const FString& ClientId);

	/** Handle tools/list request */
	TSharedPtr<FJsonObject> HandleToolsList(const TSharedPtr<FJsonObject>& Params);

	/** Handle tools/call request */
	TSharedPtr<FJsonObject> HandleToolsCall(
		const TSharedPtr<FJsonObject>& Params,
		const FString& ClientId
	);

	/** Handle resources/list request */
	TSharedPtr<FJsonObject> HandleResourcesList(const TSharedPtr<FJsonObject>& Params);

	/** Handle resources/read request */
	TSharedPtr<FJsonObject> HandleResourcesRead(const TSharedPtr<FJsonObject>& Params);

	/** Handle ping request */
	TSharedPtr<FJsonObject> HandlePing();

	// ==========================================
	// Client Management Internals
	// ==========================================

	/** Generate unique client ID */
	FString GenerateClientId() const;

	/** Extract client ID from request headers */
	FString ExtractClientId(const FHttpServerRequest& Request) const;

	/** Update client last activity time */
	void UpdateClientActivity(const FString& ClientId);

	/** Cleanup inactive clients */
	void CleanupInactiveClients();

	// ==========================================
	// State
	// ==========================================

	/** Current server state */
	EOliveMCPServerState State = EOliveMCPServerState::Stopped;

	/** Port the server is listening on */
	int32 ActualPort = 0;

	/** HTTP router handle */
	TSharedPtr<IHttpRouter> HttpRouter;

	/** Route handle for cleanup */
	FHttpRouteHandle RouteHandle;

	/** Connected clients */
	TMap<FString, FMCPClientState> ClientStates;

	/** Lock for client state access */
	mutable FCriticalSection ClientsLock;

	/** Cleanup timer handle */
	FTimerHandle CleanupTimerHandle;

	/** Client timeout in seconds */
	static constexpr float ClientTimeoutSeconds = 300.0f;

	/** Maximum concurrent clients */
	static constexpr int32 MaxClients = 10;
};
