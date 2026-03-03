// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"
#include "Pipeline/OliveWritePipeline.h"
#include "IR/CommonIR.h"
#include "IR/BlueprintIR.h"

// Forward declarations
class FOliveWritePipeline;
class FOliveBlueprintReader;
class FOliveBlueprintWriter;
class FOliveGraphWriter;
class FOliveCompileManager;
class UBlueprint;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveBPTools, Log, All);

/**
 * Blueprint Tool Handlers
 *
 * Registers and handles all Blueprint-related MCP tools.
 * Acts as a bridge between the MCP tool registry and the
 * Blueprint reader/writer infrastructure.
 *
 * Tool Categories:
 * - Readers: blueprint.read (unified, section-based), blueprint.get_node_pins
 * - Asset Writers: blueprint.create, set_parent_class, add_interface, etc.
 * - Variable Writers: blueprint.add_variable (upsert), remove_variable
 * - Component Writers: blueprint.add_component, remove_component, etc.
 * - Function Writers: blueprint.add_function (unified: function, custom_event, event_dispatcher, override), etc.
 * - Graph Writers: blueprint.add_node, connect_pins, set_pin_default, etc.
 * - AnimBP Writers: animbp.add_state_machine, add_state, add_transition, etc.
 * - Widget Writers: widget.add_widget, remove_widget, bind_property, etc.
 */
class OLIVEAIEDITOR_API FOliveBlueprintToolHandlers
{
public:
	/** Get singleton instance */
	static FOliveBlueprintToolHandlers& Get();

	/**
	 * Register all Blueprint tools with the tool registry
	 * Called during module startup after core services are initialized
	 */
	void RegisterAllTools();

	/**
	 * Unregister all Blueprint tools
	 * Called during module shutdown
	 */
	void UnregisterAllTools();

private:
	FOliveBlueprintToolHandlers() = default;

	// Non-copyable
	FOliveBlueprintToolHandlers(const FOliveBlueprintToolHandlers&) = delete;
	FOliveBlueprintToolHandlers& operator=(const FOliveBlueprintToolHandlers&) = delete;

	// ============================================================================
	// Tool Registration Helpers
	// ============================================================================

	void RegisterReaderTools();
	void RegisterAssetWriterTools();
	void RegisterVariableWriterTools();
	void RegisterComponentWriterTools();
	void RegisterFunctionWriterTools();
	void RegisterGraphWriterTools();
	void RegisterAnimBPWriterTools();
	void RegisterWidgetWriterTools();

	// ============================================================================
	// Reader Tool Handlers
	// ============================================================================

	/** Unified blueprint.read handler -- routes to section-specific helpers */
	FOliveToolResult HandleBlueprintRead(const TSharedPtr<FJsonObject>& Params);

	/** Describe a Blueprint node type: its pins, properties, and behavior */
	FOliveToolResult HandleDescribeNodeType(const TSharedPtr<FJsonObject>& Params);

	/** Section-specific reader helpers (called by HandleBlueprintRead based on section param) */
	FOliveToolResult HandleReadSectionAll(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleReadSectionGraph(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleReadSectionVariables(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleReadSectionComponents(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleReadSectionHierarchy(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleReadSectionOverridableFunctions(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintGetNodePins(const TSharedPtr<FJsonObject>& Params);

	// ============================================================================
	// Asset Writer Tool Handlers
	// ============================================================================

	FOliveToolResult HandleBlueprintCreate(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintSetParentClass(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintAddInterface(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintRemoveInterface(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintCompile(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintDelete(const TSharedPtr<FJsonObject>& Params);

	/** Create a new Blueprint Interface (BPI) asset with function signatures */
	FOliveToolResult HandleBlueprintCreateInterface(const TSharedPtr<FJsonObject>& Params);

	// ============================================================================
	// Variable Writer Tool Handlers
	// ============================================================================

	FOliveToolResult HandleBlueprintAddVariable(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintRemoveVariable(const TSharedPtr<FJsonObject>& Params);

	// ============================================================================
	// Component Writer Tool Handlers
	// ============================================================================

	FOliveToolResult HandleBlueprintAddComponent(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintRemoveComponent(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintModifyComponent(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintReparentComponent(const TSharedPtr<FJsonObject>& Params);

	// ============================================================================
	// Function Writer Tool Handlers
	// ============================================================================

	/** Unified handler for blueprint.add_function — routes by function_type param */
	FOliveToolResult HandleBlueprintAddFunction(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintRemoveFunction(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintModifyFunctionSignature(const TSharedPtr<FJsonObject>& Params);

	/** Internal helpers for function_type routing (not registered as separate tools) */
	FOliveToolResult HandleAddFunctionType_Function(const TSharedPtr<FJsonObject>& Params, const FString& AssetPath, UBlueprint* Blueprint);
	FOliveToolResult HandleAddFunctionType_CustomEvent(const TSharedPtr<FJsonObject>& Params, const FString& AssetPath, UBlueprint* Blueprint);
	FOliveToolResult HandleAddFunctionType_EventDispatcher(const TSharedPtr<FJsonObject>& Params, const FString& AssetPath, UBlueprint* Blueprint);
	FOliveToolResult HandleAddFunctionType_Override(const TSharedPtr<FJsonObject>& Params, const FString& AssetPath, UBlueprint* Blueprint);

	// ============================================================================
	// Plan JSON Tool Handlers
	// ============================================================================

	void RegisterPlanTools();
	FOliveToolResult HandleBlueprintPreviewPlanJson(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintApplyPlanJson(const TSharedPtr<FJsonObject>& Params);

	// ============================================================================
	// Template Tool Handlers
	// ============================================================================

	void RegisterTemplateTools();
	/** Internal helper: create Blueprint from template (called by HandleBlueprintCreate when template_id is set) */
	FOliveToolResult HandleBlueprintCreateFromTemplate(const FString& TemplateId, const FString& AssetPath,
		const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintGetTemplate(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintListTemplates(const TSharedPtr<FJsonObject>& Params);

	// ============================================================================
	// Graph Writer Tool Handlers
	// ============================================================================

	FOliveToolResult HandleBlueprintAddNode(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintRemoveNode(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintConnectPins(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintDisconnectPins(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintSetPinDefault(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleBlueprintSetNodeProperty(const TSharedPtr<FJsonObject>& Params);

	/** Create a Timeline node with tracks and curve data in an event graph */
	FOliveToolResult HandleBlueprintCreateTimeline(const TSharedPtr<FJsonObject>& Params);

	// ============================================================================
	// AnimBP Writer Tool Handlers
	// ============================================================================

	FOliveToolResult HandleAnimBPAddStateMachine(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleAnimBPAddState(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleAnimBPAddTransition(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleAnimBPSetTransitionRule(const TSharedPtr<FJsonObject>& Params);

	// ============================================================================
	// Widget Writer Tool Handlers
	// ============================================================================

	FOliveToolResult HandleWidgetAddWidget(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleWidgetRemoveWidget(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleWidgetSetProperty(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleWidgetBindProperty(const TSharedPtr<FJsonObject>& Params);

	// ============================================================================
	// Common Helpers
	// ============================================================================

	/**
	 * Parse required path parameter and load Blueprint
	 * @param Params Tool parameters
	 * @param OutBlueprint Loaded Blueprint (output)
	 * @param OutError Error result if failed (output)
	 * @return True if Blueprint loaded successfully
	 */
	bool LoadBlueprintFromParams(
		const TSharedPtr<FJsonObject>& Params,
		UBlueprint*& OutBlueprint,
		FOliveToolResult& OutError);

	/**
	 * Build a write request from tool parameters
	 */
	FOliveWriteRequest BuildWriteRequest(
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& Params,
		const FString& OperationCategory,
		const FText& Description);

	/**
	 * Parse type specification from JSON to FOliveIRType
	 */
	FOliveIRType ParseTypeFromParams(const TSharedPtr<FJsonObject>& TypeJson);

	/**
	 * Parse function signature from JSON to FOliveIRFunctionSignature
	 */
	FOliveIRFunctionSignature ParseFunctionSignatureFromParams(const TSharedPtr<FJsonObject>& SigJson);

	/**
	 * Parse variable definition from JSON to FOliveIRVariable
	 */
	FOliveIRVariable ParseVariableFromParams(const TSharedPtr<FJsonObject>& VarJson);

	// ============================================================================
	// Registered Tool Names (for cleanup)
	// ============================================================================

	TArray<FString> RegisteredToolNames;
};
