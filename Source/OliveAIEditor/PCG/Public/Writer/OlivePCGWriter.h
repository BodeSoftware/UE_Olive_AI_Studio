// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UPCGGraph;
class UPCGNode;

/**
 * Result of PCG graph execution
 */
struct OLIVEAIEDITOR_API FPCGExecuteResult
{
	bool bSuccess = false;
	FString Summary;
	float DurationSeconds = 0.0f;
};

/**
 * FOlivePCGWriter
 *
 * Creates and modifies UPCGGraph assets. Handles node creation, connection,
 * property setting, and graph execution. Uses node ID-based addressing
 * matching the reader output.
 *
 * All write operations use FScopedTransaction for undo support.
 *
 * Usage:
 *   FOlivePCGWriter& Writer = FOlivePCGWriter::Get();
 *   UPCGGraph* Graph = Writer.CreatePCGGraph("/Game/PCG/MyGraph");
 *   FString NodeId = Writer.AddNode(Graph, "PCGSurfaceSamplerSettings");
 *   Writer.Connect(Graph, "input", "Out", NodeId, "In");
 */
class OLIVEAIEDITOR_API FOlivePCGWriter
{
public:
	/** Get the singleton instance */
	static FOlivePCGWriter& Get();

	/**
	 * Create a new PCG graph asset
	 * @param AssetPath Content path for the new graph
	 * @return Created graph, or nullptr on failure
	 */
	UPCGGraph* CreatePCGGraph(const FString& AssetPath);

	/**
	 * Add a node by settings class name
	 * @param Graph Target PCG graph
	 * @param SettingsClassName Settings class name (flexible matching via catalog)
	 * @param PosX Editor X position
	 * @param PosY Editor Y position
	 * @return Node ID of the new node, or empty on failure
	 */
	FString AddNode(UPCGGraph* Graph, const FString& SettingsClassName,
		int32 PosX = 0, int32 PosY = 0);

	/**
	 * Remove a node from the graph
	 * @param Graph Target PCG graph
	 * @param NodeId Node ID to remove (cannot remove input/output)
	 * @return True if removed successfully
	 */
	bool RemoveNode(UPCGGraph* Graph, const FString& NodeId);

	/**
	 * Connect two pins
	 * @param Graph Target PCG graph
	 * @param SourceNodeId Source node ID
	 * @param SourcePinName Source output pin name
	 * @param TargetNodeId Target node ID
	 * @param TargetPinName Target input pin name
	 * @return True if connected successfully
	 */
	bool Connect(UPCGGraph* Graph,
		const FString& SourceNodeId, const FString& SourcePinName,
		const FString& TargetNodeId, const FString& TargetPinName);

	/**
	 * Disconnect two pins
	 * @param Graph Target PCG graph
	 * @param SourceNodeId Source node ID
	 * @param SourcePinName Source output pin name
	 * @param TargetNodeId Target node ID
	 * @param TargetPinName Target input pin name
	 * @return True if disconnected successfully
	 */
	bool Disconnect(UPCGGraph* Graph,
		const FString& SourceNodeId, const FString& SourcePinName,
		const FString& TargetNodeId, const FString& TargetPinName);

	/**
	 * Set properties on a node's settings via reflection
	 * @param Graph Target PCG graph
	 * @param NodeId Target node ID
	 * @param Properties Property name -> value pairs
	 * @return True if properties were set successfully
	 */
	bool SetSettings(UPCGGraph* Graph, const FString& NodeId,
		const TMap<FString, FString>& Properties);

	/**
	 * Add a subgraph node referencing another PCG graph
	 * @param Graph Target PCG graph
	 * @param SubgraphPath Asset path of the subgraph
	 * @param PosX Editor X position
	 * @param PosY Editor Y position
	 * @return Node ID of the new subgraph node, or empty on failure
	 */
	FString AddSubgraph(UPCGGraph* Graph, const FString& SubgraphPath,
		int32 PosX = 0, int32 PosY = 0);

	/**
	 * Execute a PCG graph and return a summary
	 * @param Graph Target PCG graph
	 * @param TimeoutSeconds Maximum execution time
	 * @return Execution result
	 */
	FPCGExecuteResult Execute(UPCGGraph* Graph, float TimeoutSeconds = 30.0f);

	/**
	 * Load a PCG graph by asset path
	 */
	UPCGGraph* LoadPCGGraph(const FString& AssetPath) const;

private:
	FOlivePCGWriter() = default;
	~FOlivePCGWriter() = default;
	FOlivePCGWriter(const FOlivePCGWriter&) = delete;
	FOlivePCGWriter& operator=(const FOlivePCGWriter&) = delete;

	/** Build node cache mapping IDs to UPCGNode pointers */
	void BuildNodeCache(const UPCGGraph* Graph);

	/** Find a node by its ID in the cache */
	UPCGNode* FindNodeById(const FString& NodeId) const;

	/** Cached node ID -> UPCGNode mapping */
	TMap<FString, UPCGNode*> NodeCache;

	/** Counter for regular node IDs */
	int32 CacheCounter = 0;
};
