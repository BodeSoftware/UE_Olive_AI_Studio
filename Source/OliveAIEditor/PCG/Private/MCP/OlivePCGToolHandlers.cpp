// Copyright Bode Software. All Rights Reserved.

#include "OlivePCGToolHandlers.h"
#include "OlivePCGSchemas.h"
#include "OlivePCGReader.h"
#include "OlivePCGWriter.h"
#include "OlivePCGNodeCatalog.h"
#include "OlivePCGAvailability.h"
#include "PCGGraph.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOlivePCGTools);

FOlivePCGToolHandlers& FOlivePCGToolHandlers::Get()
{
	static FOlivePCGToolHandlers Instance;
	return Instance;
}

void FOlivePCGToolHandlers::RegisterAllTools()
{
	if (!FOlivePCGAvailability::IsPCGAvailable())
	{
		UE_LOG(LogOlivePCGTools, Log, TEXT("PCG plugin not available, skipping PCG tool registration"));
		return;
	}

	UE_LOG(LogOlivePCGTools, Log, TEXT("Registering PCG MCP tools..."));

	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// pcg.create
	Registry.RegisterTool(
		TEXT("pcg.create"),
		TEXT("Create a new PCG graph asset"),
		OlivePCGSchemas::PCGCreate(),
		FOliveToolHandler::CreateRaw(this, &FOlivePCGToolHandlers::HandleCreate),
		{TEXT("pcg"), TEXT("write")},
		TEXT("pcg")
	);
	RegisteredToolNames.Add(TEXT("pcg.create"));

	// pcg.read
	Registry.RegisterTool(
		TEXT("pcg.read"),
		TEXT("Read a PCG graph as structured IR data with nodes, pins, edges, and settings"),
		OlivePCGSchemas::PCGRead(),
		FOliveToolHandler::CreateRaw(this, &FOlivePCGToolHandlers::HandleRead),
		{TEXT("pcg"), TEXT("read")},
		TEXT("pcg")
	);
	RegisteredToolNames.Add(TEXT("pcg.read"));

	// pcg.add_node
	Registry.RegisterTool(
		TEXT("pcg.add_node"),
		TEXT("Add a PCG node by settings class name (e.g., SurfaceSampler, StaticMeshSpawner)"),
		OlivePCGSchemas::PCGAddNode(),
		FOliveToolHandler::CreateRaw(this, &FOlivePCGToolHandlers::HandleAddNode),
		{TEXT("pcg"), TEXT("write")},
		TEXT("pcg")
	);
	RegisteredToolNames.Add(TEXT("pcg.add_node"));

	// pcg.remove_node
	Registry.RegisterTool(
		TEXT("pcg.remove_node"),
		TEXT("Remove a node from a PCG graph (cannot remove Input/Output nodes)"),
		OlivePCGSchemas::PCGRemoveNode(),
		FOliveToolHandler::CreateRaw(this, &FOlivePCGToolHandlers::HandleRemoveNode),
		{TEXT("pcg"), TEXT("write")},
		TEXT("pcg")
	);
	RegisteredToolNames.Add(TEXT("pcg.remove_node"));

	// pcg.connect
	Registry.RegisterTool(
		TEXT("pcg.connect"),
		TEXT("Connect two pins in a PCG graph (source output -> target input)"),
		OlivePCGSchemas::PCGConnect(),
		FOliveToolHandler::CreateRaw(this, &FOlivePCGToolHandlers::HandleConnect),
		{TEXT("pcg"), TEXT("write")},
		TEXT("pcg")
	);
	RegisteredToolNames.Add(TEXT("pcg.connect"));

	// pcg.disconnect
	Registry.RegisterTool(
		TEXT("pcg.disconnect"),
		TEXT("Disconnect two pins in a PCG graph"),
		OlivePCGSchemas::PCGDisconnect(),
		FOliveToolHandler::CreateRaw(this, &FOlivePCGToolHandlers::HandleDisconnect),
		{TEXT("pcg"), TEXT("write")},
		TEXT("pcg")
	);
	RegisteredToolNames.Add(TEXT("pcg.disconnect"));

	// pcg.set_settings
	Registry.RegisterTool(
		TEXT("pcg.set_settings"),
		TEXT("Set properties on a PCG node's settings via reflection"),
		OlivePCGSchemas::PCGSetSettings(),
		FOliveToolHandler::CreateRaw(this, &FOlivePCGToolHandlers::HandleSetSettings),
		{TEXT("pcg"), TEXT("write")},
		TEXT("pcg")
	);
	RegisteredToolNames.Add(TEXT("pcg.set_settings"));

	// pcg.add_subgraph
	Registry.RegisterTool(
		TEXT("pcg.add_subgraph"),
		TEXT("Add a subgraph node referencing another PCG graph"),
		OlivePCGSchemas::PCGAddSubgraph(),
		FOliveToolHandler::CreateRaw(this, &FOlivePCGToolHandlers::HandleAddSubgraph),
		{TEXT("pcg"), TEXT("write")},
		TEXT("pcg")
	);
	RegisteredToolNames.Add(TEXT("pcg.add_subgraph"));

	// pcg.execute
	Registry.RegisterTool(
		TEXT("pcg.execute"),
		TEXT("Execute a PCG graph and return a summary of results"),
		OlivePCGSchemas::PCGExecute(),
		FOliveToolHandler::CreateRaw(this, &FOlivePCGToolHandlers::HandleExecute),
		{TEXT("pcg"), TEXT("write")},
		TEXT("pcg")
	);
	RegisteredToolNames.Add(TEXT("pcg.execute"));

	UE_LOG(LogOlivePCGTools, Log, TEXT("Registered %d PCG MCP tools"), RegisteredToolNames.Num());
}

void FOlivePCGToolHandlers::UnregisterAllTools()
{
	UE_LOG(LogOlivePCGTools, Log, TEXT("Unregistering PCG MCP tools..."));

	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
	for (const FString& ToolName : RegisteredToolNames)
	{
		Registry.UnregisterTool(ToolName);
	}

	RegisteredToolNames.Empty();
	UE_LOG(LogOlivePCGTools, Log, TEXT("PCG MCP tools unregistered"));
}

// ============================================================================
// Helpers
// ============================================================================

bool FOlivePCGToolHandlers::LoadGraphFromParams(
	const TSharedPtr<FJsonObject>& Params,
	UPCGGraph*& OutGraph,
	FOliveToolResult& OutError)
{
	FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		OutError = FOliveToolResult::Error(TEXT("MISSING_PATH"),
			TEXT("Missing required 'path' parameter"),
			TEXT("Provide the asset path of the PCG graph"));
		return false;
	}

	OutGraph = FOlivePCGWriter::Get().LoadPCGGraph(Path);
	if (!OutGraph)
	{
		OutError = FOliveToolResult::Error(TEXT("PCG_GRAPH_NOT_FOUND"),
			FString::Printf(TEXT("PCG graph not found: %s"), *Path),
			TEXT("Use project.search to find the correct asset path"));
		return false;
	}

	return true;
}

// ============================================================================
// Tool Handlers
// ============================================================================

FOliveToolResult FOlivePCGToolHandlers::HandleCreate(const TSharedPtr<FJsonObject>& Params)
{
	FString Path = Params->GetStringField(TEXT("path"));

	UPCGGraph* NewGraph = FOlivePCGWriter::Get().CreatePCGGraph(Path);
	if (!NewGraph)
	{
		return FOliveToolResult::Error(TEXT("CREATE_FAILED"),
			FString::Printf(TEXT("Failed to create PCG graph at '%s'"), *Path));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), NewGraph->GetName());
	Result->SetStringField(TEXT("path"), NewGraph->GetPathName());
	Result->SetStringField(TEXT("status"), TEXT("created"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOlivePCGToolHandlers::HandleRead(const TSharedPtr<FJsonObject>& Params)
{
	UPCGGraph* Graph;
	FOliveToolResult Error;
	if (!LoadGraphFromParams(Params, Graph, Error))
	{
		return Error;
	}

	TOptional<FOliveIRPCGGraph> IR = FOlivePCGReader::Get().ReadPCGGraph(Graph);
	if (!IR.IsSet())
	{
		return FOliveToolResult::Error(TEXT("READ_FAILED"), TEXT("Failed to read PCG graph"));
	}

	return FOliveToolResult::Success(IR.GetValue().ToJson());
}

FOliveToolResult FOlivePCGToolHandlers::HandleAddNode(const TSharedPtr<FJsonObject>& Params)
{
	UPCGGraph* Graph;
	FOliveToolResult Error;
	if (!LoadGraphFromParams(Params, Graph, Error))
	{
		return Error;
	}

	FString SettingsClass = Params->GetStringField(TEXT("settings_class"));
	int32 PosX = Params->HasField(TEXT("pos_x")) ? (int32)Params->GetNumberField(TEXT("pos_x")) : 0;
	int32 PosY = Params->HasField(TEXT("pos_y")) ? (int32)Params->GetNumberField(TEXT("pos_y")) : 0;

	FString NodeId = FOlivePCGWriter::Get().AddNode(Graph, SettingsClass, PosX, PosY);

	if (NodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("ADD_NODE_FAILED"),
			FString::Printf(TEXT("Failed to add node of type '%s'"), *SettingsClass),
			TEXT("Verify the settings class exists. Try variations like: PCGSurfaceSamplerSettings, SurfaceSampler"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("settings_class"), SettingsClass);
	Result->SetStringField(TEXT("status"), TEXT("added"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOlivePCGToolHandlers::HandleRemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	UPCGGraph* Graph;
	FOliveToolResult Error;
	if (!LoadGraphFromParams(Params, Graph, Error))
	{
		return Error;
	}

	FString NodeId = Params->GetStringField(TEXT("node_id"));

	if (NodeId == TEXT("input") || NodeId == TEXT("output"))
	{
		return FOliveToolResult::Error(TEXT("PCG_CANNOT_REMOVE_IO_NODE"),
			TEXT("Cannot remove Input or Output nodes from a PCG graph"),
			TEXT("Only regular nodes (node_0, node_1, etc.) can be removed"));
	}

	bool bSuccess = FOlivePCGWriter::Get().RemoveNode(Graph, NodeId);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("REMOVE_NODE_FAILED"),
			FString::Printf(TEXT("Failed to remove node '%s'"), *NodeId),
			TEXT("Verify the node exists using pcg.read"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("status"), TEXT("removed"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOlivePCGToolHandlers::HandleConnect(const TSharedPtr<FJsonObject>& Params)
{
	UPCGGraph* Graph;
	FOliveToolResult Error;
	if (!LoadGraphFromParams(Params, Graph, Error))
	{
		return Error;
	}

	FString SourceNodeId = Params->GetStringField(TEXT("source_node_id"));
	FString SourcePin = Params->GetStringField(TEXT("source_pin"));
	FString TargetNodeId = Params->GetStringField(TEXT("target_node_id"));
	FString TargetPin = Params->GetStringField(TEXT("target_pin"));

	bool bSuccess = FOlivePCGWriter::Get().Connect(Graph, SourceNodeId, SourcePin, TargetNodeId, TargetPin);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("CONNECT_FAILED"),
			FString::Printf(TEXT("Failed to connect %s.%s -> %s.%s"),
				*SourceNodeId, *SourcePin, *TargetNodeId, *TargetPin),
			TEXT("Verify both nodes exist and pin names are correct. Use pcg.read to see available pins."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_node_id"), SourceNodeId);
	Result->SetStringField(TEXT("source_pin"), SourcePin);
	Result->SetStringField(TEXT("target_node_id"), TargetNodeId);
	Result->SetStringField(TEXT("target_pin"), TargetPin);
	Result->SetStringField(TEXT("status"), TEXT("connected"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOlivePCGToolHandlers::HandleDisconnect(const TSharedPtr<FJsonObject>& Params)
{
	UPCGGraph* Graph;
	FOliveToolResult Error;
	if (!LoadGraphFromParams(Params, Graph, Error))
	{
		return Error;
	}

	FString SourceNodeId = Params->GetStringField(TEXT("source_node_id"));
	FString SourcePin = Params->GetStringField(TEXT("source_pin"));
	FString TargetNodeId = Params->GetStringField(TEXT("target_node_id"));
	FString TargetPin = Params->GetStringField(TEXT("target_pin"));

	bool bSuccess = FOlivePCGWriter::Get().Disconnect(Graph, SourceNodeId, SourcePin, TargetNodeId, TargetPin);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("DISCONNECT_FAILED"),
			FString::Printf(TEXT("Failed to disconnect %s.%s -> %s.%s"),
				*SourceNodeId, *SourcePin, *TargetNodeId, *TargetPin),
			TEXT("Verify the connection exists. Use pcg.read to see current edges."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_node_id"), SourceNodeId);
	Result->SetStringField(TEXT("source_pin"), SourcePin);
	Result->SetStringField(TEXT("target_node_id"), TargetNodeId);
	Result->SetStringField(TEXT("target_pin"), TargetPin);
	Result->SetStringField(TEXT("status"), TEXT("disconnected"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOlivePCGToolHandlers::HandleSetSettings(const TSharedPtr<FJsonObject>& Params)
{
	UPCGGraph* Graph;
	FOliveToolResult Error;
	if (!LoadGraphFromParams(Params, Graph, Error))
	{
		return Error;
	}

	FString NodeId = Params->GetStringField(TEXT("node_id"));

	TMap<FString, FString> Properties;
	const TSharedPtr<FJsonObject>* PropsJson;
	if (Params->TryGetObjectField(TEXT("properties"), PropsJson))
	{
		for (const auto& Pair : (*PropsJson)->Values)
		{
			Properties.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	if (Properties.Num() == 0)
	{
		return FOliveToolResult::Error(TEXT("NO_PROPERTIES"),
			TEXT("No properties provided"),
			TEXT("Provide a 'properties' object with key-value pairs"));
	}

	bool bSuccess = FOlivePCGWriter::Get().SetSettings(Graph, NodeId, Properties);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("SET_SETTINGS_FAILED"),
			FString::Printf(TEXT("Failed to set settings on node '%s'"), *NodeId),
			TEXT("Verify the node exists and property names are correct. Use pcg.read to see current settings."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetNumberField(TEXT("properties_set"), Properties.Num());
	Result->SetStringField(TEXT("status"), TEXT("settings_updated"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOlivePCGToolHandlers::HandleAddSubgraph(const TSharedPtr<FJsonObject>& Params)
{
	UPCGGraph* Graph;
	FOliveToolResult Error;
	if (!LoadGraphFromParams(Params, Graph, Error))
	{
		return Error;
	}

	FString SubgraphPath = Params->GetStringField(TEXT("subgraph_path"));
	int32 PosX = Params->HasField(TEXT("pos_x")) ? (int32)Params->GetNumberField(TEXT("pos_x")) : 0;
	int32 PosY = Params->HasField(TEXT("pos_y")) ? (int32)Params->GetNumberField(TEXT("pos_y")) : 0;

	FString NodeId = FOlivePCGWriter::Get().AddSubgraph(Graph, SubgraphPath, PosX, PosY);
	if (NodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("ADD_SUBGRAPH_FAILED"),
			FString::Printf(TEXT("Failed to add subgraph '%s'"), *SubgraphPath),
			TEXT("Verify the subgraph exists and is not the same graph (cycle detection)"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("subgraph_path"), SubgraphPath);
	Result->SetStringField(TEXT("status"), TEXT("added"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOlivePCGToolHandlers::HandleExecute(const TSharedPtr<FJsonObject>& Params)
{
	UPCGGraph* Graph;
	FOliveToolResult Error;
	if (!LoadGraphFromParams(Params, Graph, Error))
	{
		return Error;
	}

	float Timeout = Params->HasField(TEXT("timeout")) ?
		(float)Params->GetNumberField(TEXT("timeout")) : 30.0f;

	FPCGExecuteResult ExecResult = FOlivePCGWriter::Get().Execute(Graph, Timeout);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), ExecResult.bSuccess);
	Result->SetStringField(TEXT("summary"), ExecResult.Summary);
	Result->SetNumberField(TEXT("duration_seconds"), ExecResult.DurationSeconds);

	if (ExecResult.bSuccess)
	{
		return FOliveToolResult::Success(Result);
	}
	else
	{
		return FOliveToolResult::Error(TEXT("EXECUTE_FAILED"), ExecResult.Summary);
	}
}
