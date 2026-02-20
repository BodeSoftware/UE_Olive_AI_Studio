// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/CommonIR.h"

// Forward declarations
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UBlueprint;
class UK2Node_FunctionEntry;
class UK2Node_FunctionResult;
class FOliveNodeSerializer;
class FOlivePinSerializer;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveGraphReader, Log, All);

/**
 * FOliveGraphReader
 *
 * Reads complete UEdGraph instances into FOliveIRGraph intermediate representation.
 * Handles node ID generation, connection resolution, and graph statistics.
 *
 * This class is responsible for:
 * - Generating stable node IDs for all nodes in a graph
 * - Resolving pin connections to node_id.pin_name format
 * - Determining graph type (EventGraph, Function, Macro)
 * - Extracting function signatures for function graphs
 * - Calculating graph statistics (node count, connection count)
 *
 * Key Design Notes:
 * - Skips tunnel nodes (UK2Node_Tunnel) as they are internal implementation details
 * - Node IDs are generated in format "node_1", "node_2", etc.
 * - The reader maintains internal caches that should be cleared between unrelated reads
 * - Thread safe for single-threaded usage on the game thread
 *
 * Usage:
 *   FOliveGraphReader GraphReader;
 *   FOliveIRGraph GraphIR = GraphReader.ReadGraph(Graph, OwningBlueprint);
 *   GraphReader.ClearCache();  // Call before reading unrelated graphs
 */
class OLIVEAIEDITOR_API FOliveGraphReader
{
public:
	/**
	 * Constructor - Initializes NodeSerializer and PinSerializer
	 */
	FOliveGraphReader();

	/**
	 * Destructor
	 */
	~FOliveGraphReader() = default;

	// ============================================================================
	// Main Reading Methods
	// ============================================================================

	/**
	 * Read an entire graph into IR format
	 * @param Graph The graph to read
	 * @param OwningBlueprint The Blueprint that owns this graph (for context)
	 * @return The graph in IR format
	 */
	FOliveIRGraph ReadGraph(const UEdGraph* Graph, const UBlueprint* OwningBlueprint);

	/**
	 * Read a set of nodes into IR format
	 * Useful for partial graph reads or when nodes are already filtered
	 * @param Nodes The nodes to serialize
	 * @param IdPrefix Prefix for generated node IDs (default: "node_")
	 * @return Array of nodes in IR format
	 */
	TArray<FOliveIRNode> ReadNodes(
		const TArray<UEdGraphNode*>& Nodes,
		const FString& IdPrefix = TEXT("node_"));

	/**
	 * Get the node ID map built during the last ReadGraph call
	 * Useful for resolving connections externally
	 * @return Map of UEdGraphNode pointers to their generated IR IDs
	 */
	const TMap<const UEdGraphNode*, FString>& GetNodeIdMap() const { return NodeIdMap; }

	/**
	 * Clear all internal caches
	 * Should be called between reading unrelated graphs to prevent ID collisions
	 */
	void ClearCache();

private:
	// ============================================================================
	// Internal Helper Methods
	// ============================================================================

	/**
	 * Generate a stable node ID for a node
	 * @param Node The node to generate ID for
	 * @param Index The sequential index of this node in the graph
	 * @return Generated ID in format "node_1", "node_2", etc.
	 */
	FString GenerateNodeId(const UEdGraphNode* Node, int32 Index);

	/**
	 * Resolve a pin connection to node_id.pin_name format
	 * @param Pin The pin to resolve connections for
	 * @return Connection string, or empty if not connected
	 */
	FString ResolveConnection(const UEdGraphPin* Pin);

	/**
	 * Calculate and set statistics on a graph IR
	 * Sets NodeCount and ConnectionCount fields
	 * @param Graph The graph IR to update with statistics
	 */
	void CalculateStatistics(FOliveIRGraph& Graph);

	/**
	 * Determine the type of a graph (EventGraph, Function, Macro, etc.)
	 * @param Graph The graph to analyze
	 * @param OwningBlueprint The Blueprint that owns this graph
	 * @return Graph type as string
	 */
	FString DetermineGraphType(const UEdGraph* Graph, const UBlueprint* OwningBlueprint) const;

	/**
	 * Check if a node should be skipped during serialization
	 * Skips tunnel nodes and other internal implementation nodes
	 * @param Node The node to check
	 * @return True if the node should be skipped
	 */
	bool ShouldSkipNode(const UEdGraphNode* Node) const;

	/**
	 * Read function signature from a function graph
	 * Extracts inputs, outputs, and function flags
	 * @param Graph The function graph
	 * @param OutGraph The IR graph to populate with signature info
	 */
	void ReadFunctionSignature(const UEdGraph* Graph, FOliveIRGraph& OutGraph);

	/**
	 * Find the function entry node in a graph
	 * @param Graph The graph to search
	 * @return The function entry node, or nullptr if not found
	 */
	UK2Node_FunctionEntry* FindFunctionEntryNode(const UEdGraph* Graph) const;

	/**
	 * Find the function result node in a graph
	 * @param Graph The graph to search
	 * @return The function result node, or nullptr if not found
	 */
	UK2Node_FunctionResult* FindFunctionResultNode(const UEdGraph* Graph) const;

	/**
	 * Build the node ID map for a graph
	 * Must be called before serializing nodes to ensure connections resolve correctly
	 * @param Graph The graph to build the map for
	 */
	void BuildNodeIdMap(const UEdGraph* Graph);

	// ============================================================================
	// Member Variables
	// ============================================================================

	/** Maps UEdGraphNode pointers to generated IR IDs */
	TMap<const UEdGraphNode*, FString> NodeIdMap;

	/** Node serializer for converting individual nodes to IR */
	TSharedPtr<FOliveNodeSerializer> NodeSerializer;

	/** Pin serializer for converting pins and resolving types */
	TSharedPtr<FOlivePinSerializer> PinSerializer;
};
