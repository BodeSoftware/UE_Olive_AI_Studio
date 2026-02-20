// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/CommonIR.h"
#include "IR/BlueprintIR.h"

// Forward declarations
class UBlueprint;
class UEdGraph;
class UK2Node_FunctionEntry;
class UK2Node_FunctionResult;
class FOliveComponentReader;
class FOlivePinSerializer;
class FOliveNodeSerializer;
class FOliveGraphReader;
struct FBPVariableDescription;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveBPReader, Log, All);

/**
 * FOliveBlueprintReader
 *
 * Main entry point for reading Blueprint assets into IR format.
 * Coordinates reading of all Blueprint subsystems (metadata, variables,
 * components, functions, graphs) and assembles them into FOliveIRBlueprint.
 *
 * This is a singleton class that provides both summary and full reads:
 * - Summary: Metadata, variable list, component list, function signatures
 * - Full: Everything in summary plus complete graph data with all nodes
 *
 * Key Responsibilities:
 * - Load Blueprints by asset path
 * - Read Blueprint metadata (type, parent class, interfaces)
 * - Read variables with types and default values
 * - Read component hierarchy via FOliveComponentReader
 * - Read function signatures
 * - Read full graph data with all nodes and connections
 * - Read individual graphs (event graphs, functions, macros)
 * - Read overridable and overridden functions
 *
 * Usage:
 *   FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
 *   TOptional<FOliveIRBlueprint> Summary = Reader.ReadBlueprintSummary("/Game/BP_Player");
 *   TOptional<FOliveIRBlueprint> Full = Reader.ReadBlueprintFull("/Game/BP_Player");
 *   TOptional<FOliveIRGraph> Graph = Reader.ReadFunctionGraph(Blueprint, "MyFunction");
 */
class OLIVEAIEDITOR_API FOliveBlueprintReader
{
public:
	/**
	 * Get the singleton instance
	 * @return Reference to the Blueprint reader singleton
	 */
	static FOliveBlueprintReader& Get();

	// ============================================================================
	// Summary Reading (Batch 3 - Fully Implemented)
	// ============================================================================

	/**
	 * Read a Blueprint summary by asset path
	 * Includes metadata, variables, components, function signatures (no graph nodes)
	 * @param AssetPath The asset path (e.g., "/Game/Blueprints/BP_Player")
	 * @return The Blueprint IR if successful, empty optional if not
	 */
	TOptional<FOliveIRBlueprint> ReadBlueprintSummary(const FString& AssetPath);

	/**
	 * Read a Blueprint summary from an already-loaded Blueprint
	 * @param Blueprint The Blueprint to read
	 * @return The Blueprint IR if successful, empty optional if not
	 */
	TOptional<FOliveIRBlueprint> ReadBlueprintSummary(const UBlueprint* Blueprint);

	/**
	 * Read all variables from a Blueprint
	 * @param Blueprint The Blueprint to read from
	 * @return Array of variables in IR format
	 */
	TArray<FOliveIRVariable> ReadVariables(const UBlueprint* Blueprint);

	/**
	 * Read all variables from a Blueprint by asset path
	 * @param AssetPath The asset path
	 * @return Array of variables in IR format
	 */
	TArray<FOliveIRVariable> ReadVariables(const FString& AssetPath);

	/**
	 * Read all components from a Blueprint
	 * @param Blueprint The Blueprint to read from
	 * @return Array of components in IR format
	 */
	TArray<FOliveIRComponent> ReadComponents(const UBlueprint* Blueprint);

	/**
	 * Read all components from a Blueprint by asset path
	 * @param AssetPath The asset path
	 * @return Array of components in IR format
	 */
	TArray<FOliveIRComponent> ReadComponents(const FString& AssetPath);

	/**
	 * Read function signatures from a Blueprint (no graph data)
	 * @param Blueprint The Blueprint to read from
	 * @return Array of function signatures
	 */
	TArray<FOliveIRFunctionSignature> ReadFunctionSignatures(const UBlueprint* Blueprint);

	/**
	 * Read function signatures from a Blueprint by asset path
	 * @param AssetPath The asset path
	 * @return Array of function signatures
	 */
	TArray<FOliveIRFunctionSignature> ReadFunctionSignatures(const FString& AssetPath);

	/**
	 * Read class hierarchy for a Blueprint (inheritance chain)
	 * @param Blueprint The Blueprint to read from
	 * @return Array of class names from most derived to UObject
	 */
	TArray<FString> ReadHierarchy(const UBlueprint* Blueprint);

	/**
	 * Read class hierarchy from a Blueprint by asset path
	 * @param AssetPath The asset path
	 * @return Array of class names from most derived to UObject
	 */
	TArray<FString> ReadHierarchy(const FString& AssetPath);

	// ============================================================================
	// Full Reading (Batch 4 - Implemented)
	// ============================================================================

	/**
	 * Read a complete Blueprint with all graph data
	 * @param AssetPath The asset path
	 * @return The Blueprint IR if successful, empty optional if not
	 */
	TOptional<FOliveIRBlueprint> ReadBlueprintFull(const FString& AssetPath);

	/**
	 * Read a complete Blueprint with all graph data from an already-loaded Blueprint
	 * @param Blueprint The Blueprint to read
	 * @return The Blueprint IR if successful, empty optional if not
	 */
	TOptional<FOliveIRBlueprint> ReadBlueprintFull(const UBlueprint* Blueprint);

	/**
	 * Read a function graph with all nodes
	 * @param Blueprint The Blueprint containing the function
	 * @param FunctionName Name of the function to read
	 * @return The function graph in IR format, or empty optional if not found
	 */
	TOptional<FOliveIRGraph> ReadFunctionGraph(const UBlueprint* Blueprint, const FString& FunctionName);

	/**
	 * Read a function graph with all nodes by asset path
	 * @param BlueprintPath The asset path to the Blueprint
	 * @param FunctionName Name of the function to read
	 * @return The function graph in IR format, or empty optional if not found
	 */
	TOptional<FOliveIRGraph> ReadFunctionGraph(const FString& BlueprintPath, const FString& FunctionName);

	/**
	 * Read the event graph with all nodes
	 * @param Blueprint The Blueprint to read from
	 * @param GraphName Name of the event graph (default "EventGraph")
	 * @return The event graph in IR format, or empty optional if not found
	 */
	TOptional<FOliveIRGraph> ReadEventGraph(const UBlueprint* Blueprint, const FString& GraphName = TEXT("EventGraph"));

	/**
	 * Read the event graph with all nodes by asset path
	 * @param BlueprintPath The asset path to the Blueprint
	 * @param EventGraphName Name of the event graph (default "EventGraph")
	 * @return The event graph in IR format, or empty optional if not found
	 */
	TOptional<FOliveIRGraph> ReadEventGraph(const FString& BlueprintPath, const FString& EventGraphName = TEXT("EventGraph"));

	/**
	 * Read a macro graph with all nodes
	 * @param Blueprint The Blueprint containing the macro
	 * @param MacroName Name of the macro to read
	 * @return The macro graph in IR format, or empty optional if not found
	 */
	TOptional<FOliveIRGraph> ReadMacroGraph(const UBlueprint* Blueprint, const FString& MacroName);

	/**
	 * Read a macro graph with all nodes by asset path
	 * @param BlueprintPath The asset path to the Blueprint
	 * @param MacroName Name of the macro to read
	 * @return The macro graph in IR format, or empty optional if not found
	 */
	TOptional<FOliveIRGraph> ReadMacroGraph(const FString& BlueprintPath, const FString& MacroName);

	/**
	 * Read all overridable functions from parent classes
	 * @param Blueprint The Blueprint to read from
	 * @return Array of overridable function signatures
	 */
	TArray<FOliveIRFunctionSignature> ReadOverridableFunctions(const UBlueprint* Blueprint);

	/**
	 * Read all overridable functions from parent classes by asset path
	 * @param BlueprintPath The asset path to the Blueprint
	 * @return Array of overridable function signatures
	 */
	TArray<FOliveIRFunctionSignature> ReadOverridableFunctions(const FString& BlueprintPath);

	/**
	 * Read all overridden functions in this Blueprint
	 * @param Blueprint The Blueprint to read from
	 * @return Array of overridden function signatures
	 */
	TArray<FOliveIRFunctionSignature> ReadOverriddenFunctions(const UBlueprint* Blueprint);

private:
	FOliveBlueprintReader();
	~FOliveBlueprintReader() = default;

	// Non-copyable
	FOliveBlueprintReader(const FOliveBlueprintReader&) = delete;
	FOliveBlueprintReader& operator=(const FOliveBlueprintReader&) = delete;

	// ============================================================================
	// Private Helper Methods
	// ============================================================================

	/**
	 * Load a Blueprint by asset path
	 * @param AssetPath The asset path to load
	 * @return The loaded Blueprint, or nullptr if failed
	 */
	UBlueprint* LoadBlueprint(const FString& AssetPath) const;

	/**
	 * Read interface information into the IR
	 * @param Blueprint The Blueprint to read from
	 * @param OutIR The IR to populate
	 */
	void ReadInterfacesInfo(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const;

	/**
	 * Read compilation status into the IR
	 * @param Blueprint The Blueprint to read from
	 * @param OutIR The IR to populate
	 */
	void ReadCompilationStatus(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const;

	/**
	 * Read parent class information into the IR
	 * @param Blueprint The Blueprint to read from
	 * @param OutIR The IR to populate
	 */
	void ReadParentClassInfo(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const;

	/**
	 * Read event dispatchers from a Blueprint
	 * @param Blueprint The Blueprint to read from
	 * @param OutIR The IR to populate
	 */
	void ReadEventDispatchers(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const;

	/**
	 * Read graph names (without full node data)
	 * @param Blueprint The Blueprint to read from
	 * @param OutIR The IR to populate
	 */
	void ReadGraphNames(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const;

	/**
	 * Read Blueprint capabilities based on type
	 * @param Blueprint The Blueprint to read from
	 * @param OutIR The IR to populate
	 */
	void ReadCapabilities(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR) const;

	/**
	 * Read all graphs with full node data
	 * @param Blueprint The Blueprint to read from
	 * @param OutIR The IR to populate
	 */
	void ReadAllGraphs(const UBlueprint* Blueprint, FOliveIRBlueprint& OutIR);

	/**
	 * Read a single variable description to IR format
	 * @param VarDesc The variable description from Blueprint
	 * @param DefinedIn Where the variable is defined ("self" or class name)
	 * @return The variable in IR format
	 */
	FOliveIRVariable ConvertVariableToIR(const FBPVariableDescription& VarDesc, const FString& DefinedIn) const;

	/**
	 * Extract function signature from a function entry node
	 * @param EntryNode The function entry node
	 * @param ResultNode Optional result node for return values
	 * @return The function signature in IR format
	 */
	FOliveIRFunctionSignature ExtractFunctionSignature(
		const UK2Node_FunctionEntry* EntryNode,
		const UK2Node_FunctionResult* ResultNode) const;

	// ============================================================================
	// Sub-Readers
	// ============================================================================

	/** Component reader for SCS traversal */
	TSharedPtr<FOliveComponentReader> ComponentReader;

	/** Pin serializer for type conversion */
	TSharedPtr<FOlivePinSerializer> PinSerializer;

	/** Node serializer for graph node conversion */
	TSharedPtr<FOliveNodeSerializer> NodeSerializer;

	/** Graph reader for full graph serialization */
	TSharedPtr<FOliveGraphReader> GraphReader;
};
