// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

class UPCGGraph;

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePCGTools, Log, All);

/**
 * FOlivePCGToolHandlers
 *
 * Registers and handles all PCG MCP tools.
 * Acts as a bridge between the MCP tool registry and the PCG
 * reader/writer/catalog infrastructure.
 *
 * Tools: pcg.create, pcg.read, pcg.add_node, pcg.remove_node,
 *        pcg.connect, pcg.disconnect, pcg.set_settings,
 *        pcg.add_subgraph, pcg.execute
 */
class OLIVEAIEDITOR_API FOlivePCGToolHandlers
{
public:
	/** Get singleton instance */
	static FOlivePCGToolHandlers& Get();

	/** Register all PCG tools with the tool registry (guarded by PCG availability) */
	void RegisterAllTools();

	/** Unregister all PCG tools */
	void UnregisterAllTools();

private:
	FOlivePCGToolHandlers() = default;
	FOlivePCGToolHandlers(const FOlivePCGToolHandlers&) = delete;
	FOlivePCGToolHandlers& operator=(const FOlivePCGToolHandlers&) = delete;

	// Tool handlers
	FOliveToolResult HandleCreate(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleRead(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleAddNode(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleRemoveNode(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleConnect(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleDisconnect(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleSetSettings(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleAddSubgraph(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleExecute(const TSharedPtr<FJsonObject>& Params);

	// Helpers
	bool LoadGraphFromParams(const TSharedPtr<FJsonObject>& Params,
		UPCGGraph*& OutGraph, FOliveToolResult& OutError);

	TArray<FString> RegisteredToolNames;
};
