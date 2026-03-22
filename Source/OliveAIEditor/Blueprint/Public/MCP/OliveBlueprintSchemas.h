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
	 * Schema for blueprint.read (unified reader)
	 * Read Blueprint data filtered by section.
	 * Params: {path: string, section?: "all"|"summary"|"graph"|"variables"|"components"|"hierarchy"|"overridable_functions",
	 *          graph_name?: string (required when section="graph"), mode?: "summary"|"full"|"auto",
	 *          page?: int, page_size?: int}
	 */
	TSharedPtr<FJsonObject> BlueprintRead();

	/**
	 * Schema for blueprint.get_node_pins
	 * Get the pin manifest for a specific node in a Blueprint graph.
	 * Useful for re-inspecting pins after property changes or ReconstructNode.
	 * Params: {path: string, graph: string, node_id: string}
	 */
	TSharedPtr<FJsonObject> BlueprintGetNodePins();

	/**
	 * Schema for blueprint.describe_node_type
	 * Describe a Blueprint node type before creating it.
	 * Returns display name, description, pins, and behavior flags.
	 * Params: {type: string}
	 */
	TSharedPtr<FJsonObject> BlueprintDescribeNodeType();

	/**
	 * Schema for blueprint.describe_function tool.
	 * Look up a UFunction by name and return its exact pin signature.
	 * Params: {function_name: string, target_class?: string, path?: string}
	 */
	TSharedPtr<FJsonObject> BlueprintDescribeFunction();

	/**
	 * Schema for blueprint.verify_completion tool.
	 * Verify a Blueprint is complete: compiles, expected functions/variables exist,
	 * no orphaned exec flows, no unwired required data pins.
	 * Params: {asset_path: string, expected_functions?: string[], expected_variables?: string[]}
	 */
	TSharedPtr<FJsonObject> BlueprintVerifyCompletion();

	// ============================================================================
	// Asset Writer Tool Schemas
	// ============================================================================

	/**
	 * Schema for blueprint.create
	 * Create new Blueprint. Optionally create from a factory template.
	 * Params: {path: string, parent_class: string, type?: string,
	 *          template_id?: string, template_params?: object, preset?: string}
	 */
	TSharedPtr<FJsonObject> BlueprintCreate();

	/**
	 * Schema for blueprint.scaffold
	 * Create a Blueprint with components, variables, and interfaces in one call.
	 * Replaces the pattern of create + N x add_component + N x add_variable + N x add_interface.
	 * Params: {path: string, parent_class: string, type?: string,
	 *          components?: [{class: string, name?: string, parent?: string}],
	 *          variables?: [VariableSpec],
	 *          interfaces?: [string]}
	 */
	TSharedPtr<FJsonObject> BlueprintScaffold();

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
	 * Schema for blueprint.create_interface
	 * Create a new Blueprint Interface asset with function signatures.
	 * Params: {path: string, functions: [{name: string, inputs?: [{name, type}], outputs?: [{name, type}]}]}
	 */
	TSharedPtr<FJsonObject> BlueprintCreateInterface();

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
	 * Schema for blueprint.add_variable (upsert)
	 * Add or update a variable. If the variable already exists, modifies it.
	 * Params: {path: string, variable: VariableSpec, modify_only?: bool}
	 */
	TSharedPtr<FJsonObject> BlueprintAddVariable();

	/**
	 * Schema for blueprint.remove_variable
	 * Remove variable
	 * Params: {path: string, name: string}
	 */
	TSharedPtr<FJsonObject> BlueprintRemoveVariable();

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
	 * Schema for blueprint.add_function (unified)
	 * Add function, custom event, event dispatcher, or override a parent function.
	 * Params: {path: string, function_type?: "function"|"custom_event"|"event_dispatcher"|"override",
	 *          name: string, signature?: FunctionSignature, inputs?: Param[], outputs?: Param[],
	 *          is_pure?: bool, params?: Param[]}
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
	 * Params: {path: string, graph: string, source?: string, target?: string, source_ref?: object, target_ref?: object}
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

	/**
	 * Schema for blueprint.create_timeline
	 * Create a Timeline node with tracks and curve data in an event graph.
	 * Params: {path: string, graph?: string, timeline_name?: string, length?: number,
	 *          auto_play?: bool, loop?: bool, replicated?: bool, ignore_time_dilation?: bool,
	 *          tracks: [{name: string, type: "float"|"vector"|"color"|"event", keys: array, interp?: string}]}
	 */
	TSharedPtr<FJsonObject> BlueprintCreateTimeline();

	// ============================================================================
	// Plan JSON Tool Schemas
	// ============================================================================

	/**
	 * Schema for blueprint.preview_plan_json
	 * Preview an intent-level plan without mutating the Blueprint.
	 * Returns normalized plan, diff summary, and a fingerprint for drift detection.
	 * Params: {asset_path: string, graph_target?: string, mode?: string, plan_json: object}
	 */
	TSharedPtr<FJsonObject> BlueprintPreviewPlanJson();

	/**
	 * Schema for blueprint.apply_plan_json
	 * Apply an intent-level plan atomically to a Blueprint graph.
	 * Resolves intents to concrete nodes, executes as a single transaction, compiles once.
	 * Params: {asset_path: string, graph_target?: string, mode?: string, plan_json: object, preview_fingerprint?: string}
	 */
	TSharedPtr<FJsonObject> BlueprintApplyPlanJson();

	// ============================================================================
	// Template Tool Schemas
	// ============================================================================

	/**
	 * Schema for blueprint.create_from_template
	 * Create a complete Blueprint from a factory template.
	 * Params: {template_id: string, path: string, preset?: string, parameters?: object}
	 */
	TSharedPtr<FJsonObject> BlueprintCreateFromTemplate();

	/**
	 * Schema for blueprint.get_template
	 * View a template's full content
	 * Params: {template_id: string, pattern?: string}
	 */
	TSharedPtr<FJsonObject> BlueprintGetTemplate();

	/**
	 * Schema for blueprint.list_templates
	 * List available templates
	 * Params: {type?: string}
	 */
	TSharedPtr<FJsonObject> BlueprintListTemplates();

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
