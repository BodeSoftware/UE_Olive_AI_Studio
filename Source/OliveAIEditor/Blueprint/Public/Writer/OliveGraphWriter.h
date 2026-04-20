// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OliveBlueprintWriter.h"
#include "Dom/JsonValue.h"

// Forward declarations
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class FOliveNodeFactory;
class FOlivePinConnector;
class FJsonObject;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveGraphWriter, Log, All);

/**
 * FOliveGraphWriter
 *
 * Orchestrates graph-level modifications including node creation, removal,
 * property modification, and pin connections. This is the primary interface
 * for AI agents to manipulate Blueprint graphs.
 *
 * Key Features:
 * - Node creation with automatic ID generation and caching
 * - Batch node operations for efficient multi-node creation
 * - Pin connection using "node_id.pin_name" reference format
 * - Session-based node caching for efficient lookups
 * - Transaction support for undo/redo
 *
 * Node ID Format:
 * Node IDs are generated in the format "node_0", "node_1", etc.
 * These IDs are session-specific and used for referencing nodes
 * within a modification session.
 *
 * Pin Reference Format:
 * Pins are referenced using "node_id.pin_name" format, e.g.:
 * - "node_0.exec" - The exec pin on node_0
 * - "node_1.ReturnValue" - The return value pin on node_1
 *
 * Usage:
 *   FOliveGraphWriter& Writer = FOliveGraphWriter::Get();
 *
 *   // Add a single node
 *   TMap<FString, FString> Props;
 *   Props.Add(TEXT("function_name"), TEXT("PrintString"));
 *   auto Result = Writer.AddNode("/Game/BP_Test", "EventGraph", "CallFunction", Props, 200, 100);
 *   FString NodeId = Result.CreatedNodeId; // e.g., "node_0"
 *
 *   // Connect nodes
 *   Writer.ConnectPins("/Game/BP_Test", "EventGraph", "node_0.then", "node_1.execute");
 */
class OLIVEAIEDITOR_API FOliveGraphWriter
{
public:
	/**
	 * Get the singleton instance
	 * @return Reference to the graph writer singleton
	 */
	static FOliveGraphWriter& Get();

	// ============================================================================
	// Node Operations
	// ============================================================================

	/**
	 * Add a single node to a graph
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param GraphName Name of the graph (e.g., "EventGraph", function name)
	 * @param NodeType Type of node to create (use OliveNodeTypes constants)
	 * @param NodeProperties Type-specific properties for node configuration
	 * @param PosX X position in the graph
	 * @param PosY Y position in the graph
	 * @return Result with CreatedNodeId on success
	 */
	FOliveBlueprintWriteResult AddNode(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& NodeType,
		const TMap<FString, FString>& NodeProperties,
		int32 PosX,
		int32 PosY);

	/**
	 * Remove a node from a graph by ID
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param GraphName Name of the graph
	 * @param NodeId ID of the node to remove (e.g., "node_0")
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult RemoveNode(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& NodeId);

	/**
	 * Set a property on an existing node
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param GraphName Name of the graph
	 * @param NodeId ID of the node to modify
	 * @param PropertyName Name of the property to set
	 * @param PropertyValue New value for the property
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult SetNodeProperty(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& NodeId,
		const FString& PropertyName,
		const FString& PropertyValue);

	// ============================================================================
	// Pin Operations
	// ============================================================================

	/**
	 * Connect two pins
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param GraphName Name of the graph
	 * @param SourcePin Source pin reference in "node_id.pin_name" format
	 * @param TargetPin Target pin reference in "node_id.pin_name" format
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult ConnectPins(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& SourcePin,
		const FString& TargetPin);

	/**
	 * Disconnect two specific pins
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param GraphName Name of the graph
	 * @param SourcePin Source pin reference in "node_id.pin_name" format
	 * @param TargetPin Target pin reference in "node_id.pin_name" format
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult DisconnectPins(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& SourcePin,
		const FString& TargetPin);

	/**
	 * Disconnect all connections from a pin
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param GraphName Name of the graph
	 * @param Pin Pin reference in "node_id.pin_name" format
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult DisconnectAllFromPin(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& Pin);

	/**
	 * Set the default value of an input pin
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param GraphName Name of the graph
	 * @param Pin Pin reference in "node_id.pin_name" format
	 * @param DefaultValue New default value as string
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult SetPinDefault(
		const FString& BlueprintPath,
		const FString& GraphName,
		const FString& Pin,
		const FString& DefaultValue);

	// ============================================================================
	// Connection Inspection
	// ============================================================================

	/**
	 * Capture all connections on a node before removal.
	 * Call this BEFORE RemoveNode to get a snapshot of what will break.
	 *
	 * Each element in the returned array is a JSON object with the shape:
	 *   { "pin": "<pin_name>", "direction": "input"|"output",
	 *     "was_connected_to": { "node_id": "<id>", "pin": "<pin_name>" } }
	 *
	 * @param BlueprintPath Asset path for node ID resolution via cache
	 * @param Graph The graph containing the node
	 * @param Node The node about to be removed
	 * @return JSON array of broken link descriptors
	 */
	TArray<TSharedPtr<FJsonValue>> CaptureNodeConnections(
		const FString& BlueprintPath,
		UEdGraph* Graph,
		UEdGraphNode* Node);

	// ============================================================================
	// Session Management
	// ============================================================================

	/**
	 * Clear the node cache for a specific Blueprint
	 * Call this when starting a new editing session or when the Blueprint
	 * has been modified externally.
	 * @param BlueprintPath Full asset path to the Blueprint
	 */
	void ClearNodeCache(const FString& BlueprintPath);

	/**
	 * Clear all node caches
	 * Call this when starting a completely new session.
	 */
	void ClearAllCaches();

	/**
	 * Get a cached node by ID
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param NodeId ID of the node to retrieve
	 * @return The node if found and still valid, nullptr otherwise
	 */
	UEdGraphNode* GetCachedNode(const FString& BlueprintPath, const FString& NodeId);

	/**
	 * Get all cached node IDs for a Blueprint
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @return Array of cached node IDs
	 */
	TArray<FString> GetCachedNodeIds(const FString& BlueprintPath) const;

	/**
	 * Check if a node ID exists in the cache
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param NodeId ID of the node to check
	 * @return True if the node ID exists in cache (may be invalid)
	 */
	bool HasCachedNode(const FString& BlueprintPath, const FString& NodeId) const;

	/**
	 * Cache an externally-created node and return a generated node ID.
	 * Used by tool handlers that create nodes directly (e.g., create_timeline)
	 * rather than through GraphWriter.AddNode().
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param Node The node to cache
	 * @return Generated node ID for subsequent tool calls (e.g., "node_5")
	 */
	FString CacheExternalNode(const FString& BlueprintPath, UEdGraphNode* Node);

private:
	FOliveGraphWriter();
	~FOliveGraphWriter() = default;

	// Non-copyable
	FOliveGraphWriter(const FOliveGraphWriter&) = delete;
	FOliveGraphWriter& operator=(const FOliveGraphWriter&) = delete;

	// ============================================================================
	// Private Helper Methods
	// ============================================================================

	/**
	 * Find a graph in a Blueprint by name
	 * Searches UbergraphPages (event graphs), FunctionGraphs, and MacroGraphs.
	 * @param Blueprint The Blueprint to search
	 * @param GraphName Name of the graph to find
	 * @return The graph if found, nullptr otherwise
	 */
	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName);

	/**
	 * Find a node by ID, first checking cache then searching the graph
	 * @param Blueprint The Blueprint containing the graph
	 * @param Graph The graph to search
	 * @param BlueprintPath Path for cache lookup
	 * @param NodeId ID of the node to find
	 * @return The node if found, nullptr otherwise
	 */
	UEdGraphNode* FindNodeById(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FString& BlueprintPath,
		const FString& NodeId);

	/**
	 * Find a pin on a node by name
	 * Tries exact name match first, then display name match.
	 * @param Node The node containing the pin
	 * @param PinName Name of the pin to find
	 * @return The pin if found, nullptr otherwise
	 */
	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName);

	/**
	 * Parse a pin reference string into node ID and pin name
	 * @param PinRef Pin reference in "node_id.pin_name" format
	 * @param OutNodeId Output parameter for the node ID
	 * @param OutPinName Output parameter for the pin name
	 * @return True if parsing succeeded
	 */
	bool ParsePinReference(const FString& PinRef, FString& OutNodeId, FString& OutPinName);

	/**
	 * Generate a unique node ID for a Blueprint
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @return Generated node ID (e.g., "node_0", "node_1")
	 */
	FString GenerateNodeId(const FString& BlueprintPath);

	/**
	 * Cache a node with its ID
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param NodeId ID to associate with the node
	 * @param Node The node to cache
	 */
	void CacheNode(const FString& BlueprintPath, const FString& NodeId, UEdGraphNode* Node);

	/**
	 * Remove a node from the cache
	 * @param BlueprintPath Full asset path to the Blueprint
	 * @param NodeId ID of the node to remove from cache
	 */
	void RemoveFromCache(const FString& BlueprintPath, const FString& NodeId);

	/**
	 * Reverse-lookup a UEdGraphNode* in the NodeIdCache to find its string ID.
	 * Falls back to the node's GetName() if the node is not present in the cache.
	 * @param BlueprintPath Asset path used as cache key
	 * @param Node The node pointer to look up
	 * @return Cached node ID or the node's UObject name as fallback
	 */
	FString ResolveNodeId(const FString& BlueprintPath, UEdGraphNode* Node) const;

	/**
	 * Load a Blueprint for editing with validation
	 * @param AssetPath Full asset path to the Blueprint
	 * @param OutError Output parameter for error message if loading fails
	 * @return The Blueprint if successful, nullptr otherwise
	 */
	UBlueprint* LoadBlueprintForEditing(const FString& AssetPath, FString& OutError);

	/**
	 * Check if Play-In-Editor is currently active
	 * @return True if PIE is active
	 */
	bool IsPIEActive() const;

	// ============================================================================
	// Private Members
	// ============================================================================

	/** Cache of node IDs to nodes: BlueprintPath -> (NodeId -> Node) */
	TMap<FString, TMap<FString, TWeakObjectPtr<UEdGraphNode>>> NodeIdCache;

	/** Next node index for ID generation: BlueprintPath -> NextIndex */
	TMap<FString, int32> NextNodeIndex;

	/** Critical section for thread safety when accessing caches */
	mutable FCriticalSection CacheLock;
};
