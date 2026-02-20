// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Blueprint Tool Schema Builder
 *
 * Provides JSON Schema definitions for all Blueprint MCP tools.
 * Schemas follow JSON Schema Draft 7 format for MCP compatibility.
 *
 * Each schema function returns a TSharedPtr<FJsonObject> representing
 * a complete JSON Schema Draft 7 object with type, properties, required fields,
 * and descriptions for AI agent understanding.
 */
namespace OliveBlueprintSchemas
{
	// ============================================================================
	// Common Schema Components
	// ============================================================================

	/** Create a string property schema */
	TSharedPtr<FJsonObject> StringProp(const FString& Description, bool bRequired = false);

	/** Create an integer property schema */
	TSharedPtr<FJsonObject> IntProp(const FString& Description, int32 Min = 0, int32 Max = INT32_MAX);

	/** Create a boolean property schema */
	TSharedPtr<FJsonObject> BoolProp(const FString& Description, bool DefaultValue = false);

	/** Create an array property schema */
	TSharedPtr<FJsonObject> ArrayProp(const FString& Description, TSharedPtr<FJsonObject> ItemSchema);

	/** Create an object property schema */
	TSharedPtr<FJsonObject> ObjectProp(const FString& Description, TSharedPtr<FJsonObject> Properties);

	/** Create an enum property schema */
	TSharedPtr<FJsonObject> EnumProp(const FString& Description, const TArray<FString>& Values);

	/** Type specification schema (for variable/parameter types) */
	TSharedPtr<FJsonObject> TypeSpecSchema();

	/** Function parameter schema */
	TSharedPtr<FJsonObject> FunctionParamSchema();

	/** Function signature schema */
	TSharedPtr<FJsonObject> FunctionSignatureSchema();

	/** Variable definition schema */
	TSharedPtr<FJsonObject> VariableSchema();

	/** Component specification schema */
	TSharedPtr<FJsonObject> ComponentSpecSchema();

	// ============================================================================
	// Reader Tool Schemas
	// ============================================================================

	/**
	 * Schema for blueprint.read
	 * Read Blueprint structure
	 * Params: {path: string, mode?: "summary"|"full"}
	 */
	TSharedPtr<FJsonObject> BlueprintRead();

	/**
	 * Schema for blueprint.read_function
	 * Read single function graph
	 * Params: {path: string, function_name: string}
	 */
	TSharedPtr<FJsonObject> BlueprintReadFunction();

	/**
	 * Schema for blueprint.read_event_graph
	 * Read event graph
	 * Params: {path: string, graph_name?: string}
	 */
	TSharedPtr<FJsonObject> BlueprintReadEventGraph();

	/**
	 * Schema for blueprint.read_variables
	 * Read all variables
	 * Params: {path: string}
	 */
	TSharedPtr<FJsonObject> BlueprintReadVariables();

	/**
	 * Schema for blueprint.read_components
	 * Read component tree
	 * Params: {path: string}
	 */
	TSharedPtr<FJsonObject> BlueprintReadComponents();

	/**
	 * Schema for blueprint.read_hierarchy
	 * Read class hierarchy
	 * Params: {path: string}
	 */
	TSharedPtr<FJsonObject> BlueprintReadHierarchy();

	/**
	 * Schema for blueprint.list_overridable_functions
	 * List overridable functions from parent
	 * Params: {path: string}
	 */
	TSharedPtr<FJsonObject> BlueprintListOverridableFunctions();

	// ============================================================================
	// Asset Writer Tool Schemas
	// ============================================================================

	/**
	 * Schema for blueprint.create
	 * Create new Blueprint
	 * Params: {path: string, parent_class: string, type?: string}
	 */
	TSharedPtr<FJsonObject> BlueprintCreate();

	/**
	 * Schema for blueprint.set_parent_class
	 * Change parent class
	 * Params: {path: string, new_parent: string}
	 */
	TSharedPtr<FJsonObject> BlueprintSetParentClass();

	/**
	 * Schema for blueprint.add_interface
	 * Implement interface
	 * Params: {path: string, interface: string}
	 */
	TSharedPtr<FJsonObject> BlueprintAddInterface();

	/**
	 * Schema for blueprint.remove_interface
	 * Remove interface
	 * Params: {path: string, interface: string}
	 */
	TSharedPtr<FJsonObject> BlueprintRemoveInterface();

	/**
	 * Schema for blueprint.compile
	 * Force compile
	 * Params: {path: string}
	 */
	TSharedPtr<FJsonObject> BlueprintCompile();

	/**
	 * Schema for blueprint.delete
	 * Delete Blueprint
	 * Params: {path: string}
	 */
	TSharedPtr<FJsonObject> BlueprintDelete();

	// ============================================================================
	// Variable Writer Tool Schemas
	// ============================================================================

	/**
	 * Schema for blueprint.add_variable
	 * Add variable
	 * Params: {path: string, variable: VariableSpec}
	 */
	TSharedPtr<FJsonObject> BlueprintAddVariable();

	/**
	 * Schema for blueprint.remove_variable
	 * Remove variable
	 * Params: {path: string, name: string}
	 */
	TSharedPtr<FJsonObject> BlueprintRemoveVariable();

	/**
	 * Schema for blueprint.modify_variable
	 * Modify variable
	 * Params: {path: string, name: string, changes: object}
	 */
	TSharedPtr<FJsonObject> BlueprintModifyVariable();

	// ============================================================================
	// Component Writer Tool Schemas
	// ============================================================================

	/**
	 * Schema for blueprint.add_component
	 * Add component
	 * Params: {path: string, class: string, name?: string, parent?: string}
	 */
	TSharedPtr<FJsonObject> BlueprintAddComponent();

	/**
	 * Schema for blueprint.remove_component
	 * Remove component
	 * Params: {path: string, name: string}
	 */
	TSharedPtr<FJsonObject> BlueprintRemoveComponent();

	/**
	 * Schema for blueprint.modify_component
	 * Modify component
	 * Params: {path: string, name: string, properties: object}
	 */
	TSharedPtr<FJsonObject> BlueprintModifyComponent();

	/**
	 * Schema for blueprint.reparent_component
	 * Change component parent
	 * Params: {path: string, name: string, new_parent: string}
	 */
	TSharedPtr<FJsonObject> BlueprintReparentComponent();

	// ============================================================================
	// Function Writer Tool Schemas
	// ============================================================================

	/**
	 * Schema for blueprint.add_function
	 * Add function
	 * Params: {path: string, signature: FunctionSignature}
	 */
	TSharedPtr<FJsonObject> BlueprintAddFunction();

	/**
	 * Schema for blueprint.remove_function
	 * Remove function
	 * Params: {path: string, name: string}
	 */
	TSharedPtr<FJsonObject> BlueprintRemoveFunction();

	/**
	 * Schema for blueprint.modify_function_signature
	 * Modify signature
	 * Params: {path: string, name: string, changes: object}
	 */
	TSharedPtr<FJsonObject> BlueprintModifyFunctionSignature();

	/**
	 * Schema for blueprint.add_event_dispatcher
	 * Add dispatcher
	 * Params: {path: string, name: string, params?: Param[]}
	 */
	TSharedPtr<FJsonObject> BlueprintAddEventDispatcher();

	/**
	 * Schema for blueprint.override_function
	 * Override parent function
	 * Params: {path: string, function_name: string}
	 */
	TSharedPtr<FJsonObject> BlueprintOverrideFunction();

	/**
	 * Schema for blueprint.add_custom_event
	 * Add custom event
	 * Params: {path: string, name: string, params?: Param[]}
	 */
	TSharedPtr<FJsonObject> BlueprintAddCustomEvent();

	// ============================================================================
	// Graph Writer Tool Schemas
	// ============================================================================

	/**
	 * Schema for blueprint.add_node
	 * Add node to graph
	 * Params: {path: string, graph: string, type: string, properties?: object, pos_x?: int, pos_y?: int}
	 */
	TSharedPtr<FJsonObject> BlueprintAddNode();

	/**
	 * Schema for blueprint.remove_node
	 * Remove node
	 * Params: {path: string, graph: string, node_id: string}
	 */
	TSharedPtr<FJsonObject> BlueprintRemoveNode();

	/**
	 * Schema for blueprint.connect_pins
	 * Connect two pins
	 * Params: {path: string, graph: string, source: string, target: string}
	 */
	TSharedPtr<FJsonObject> BlueprintConnectPins();

	/**
	 * Schema for blueprint.disconnect_pins
	 * Disconnect pins
	 * Params: {path: string, graph: string, source: string, target: string}
	 */
	TSharedPtr<FJsonObject> BlueprintDisconnectPins();

	/**
	 * Schema for blueprint.set_pin_default
	 * Set pin default
	 * Params: {path: string, graph: string, pin: string, value: string}
	 */
	TSharedPtr<FJsonObject> BlueprintSetPinDefault();

	/**
	 * Schema for blueprint.set_node_property
	 * Set node property
	 * Params: {path: string, graph: string, node_id: string, property: string, value: string}
	 */
	TSharedPtr<FJsonObject> BlueprintSetNodeProperty();

	// ============================================================================
	// AnimBP Writer Tool Schemas
	// ============================================================================

	/**
	 * Schema for animbp.add_state_machine
	 * Add state machine
	 * Params: {path: string, name: string}
	 */
	TSharedPtr<FJsonObject> AnimBPAddStateMachine();

	/**
	 * Schema for animbp.add_state
	 * Add state to machine
	 * Params: {path: string, machine: string, name: string, animation?: string}
	 */
	TSharedPtr<FJsonObject> AnimBPAddState();

	/**
	 * Schema for animbp.add_transition
	 * Add transition
	 * Params: {path: string, machine: string, from: string, to: string}
	 */
	TSharedPtr<FJsonObject> AnimBPAddTransition();

	/**
	 * Schema for animbp.set_transition_rule
	 * Set transition rule
	 * Params: {path: string, machine: string, from: string, to: string, rule: object}
	 */
	TSharedPtr<FJsonObject> AnimBPSetTransitionRule();

	// ============================================================================
	// Widget Writer Tool Schemas
	// ============================================================================

	/**
	 * Schema for widget.add_widget
	 * Add widget
	 * Params: {path: string, class: string, parent?: string, slot?: string, name?: string}
	 */
	TSharedPtr<FJsonObject> WidgetAddWidget();

	/**
	 * Schema for widget.remove_widget
	 * Remove widget
	 * Params: {path: string, name: string}
	 */
	TSharedPtr<FJsonObject> WidgetRemoveWidget();

	/**
	 * Schema for widget.set_property
	 * Set widget property
	 * Params: {path: string, widget: string, property: string, value: string}
	 */
	TSharedPtr<FJsonObject> WidgetSetProperty();

	/**
	 * Schema for widget.bind_property
	 * Bind to function
	 * Params: {path: string, widget: string, property: string, function: string}
	 */
	TSharedPtr<FJsonObject> WidgetBindProperty();
}
