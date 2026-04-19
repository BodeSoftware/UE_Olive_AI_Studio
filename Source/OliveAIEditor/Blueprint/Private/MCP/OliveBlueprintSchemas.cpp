// Copyright Bode Software. All Rights Reserved.

#include "OliveBlueprintSchemas.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace OliveBlueprintSchemas
{
	// ============================================================================
	// Helper Functions
	// ============================================================================

	/**
	 * Create a basic JSON Schema object with type
	 */
	static TSharedPtr<FJsonObject> MakeSchema(const FString& Type)
	{
		TSharedPtr<FJsonObject> Schema = MakeShareable(new FJsonObject());
		Schema->SetStringField(TEXT("type"), Type);
		return Schema;
	}

	/**
	 * Create a properties object for object schemas
	 */
	static TSharedPtr<FJsonObject> MakeProperties()
	{
		return MakeShareable(new FJsonObject());
	}

	/**
	 * Add a required field array to a schema
	 */
	static void AddRequired(TSharedPtr<FJsonObject> Schema, const TArray<FString>& RequiredFields)
	{
		TArray<TSharedPtr<FJsonValue>> RequiredArray;
		for (const FString& Field : RequiredFields)
		{
			RequiredArray.Add(MakeShareable(new FJsonValueString(Field)));
		}
		Schema->SetArrayField(TEXT("required"), RequiredArray);
	}

	/**
	 * Create a number (floating-point) property schema with a default value
	 */
	static TSharedPtr<FJsonObject> NumberProp(const FString& Description, double DefaultValue)
	{
		TSharedPtr<FJsonObject> Prop = MakeSchema(TEXT("number"));
		Prop->SetStringField(TEXT("description"), Description);
		Prop->SetNumberField(TEXT("default"), DefaultValue);
		return Prop;
	}

	// ============================================================================
	// Common Schema Components
	// ============================================================================

	TSharedPtr<FJsonObject> StringProp(const FString& Description, bool bRequired)
	{
		TSharedPtr<FJsonObject> Prop = MakeSchema(TEXT("string"));
		Prop->SetStringField(TEXT("description"), Description);
		return Prop;
	}

	TSharedPtr<FJsonObject> IntProp(const FString& Description, int32 Min, int32 Max)
	{
		TSharedPtr<FJsonObject> Prop = MakeSchema(TEXT("integer"));
		Prop->SetStringField(TEXT("description"), Description);
		if (Min > INT32_MIN)
		{
			Prop->SetNumberField(TEXT("minimum"), Min);
		}
		if (Max < INT32_MAX)
		{
			Prop->SetNumberField(TEXT("maximum"), Max);
		}
		return Prop;
	}

	TSharedPtr<FJsonObject> BoolProp(const FString& Description, bool DefaultValue)
	{
		TSharedPtr<FJsonObject> Prop = MakeSchema(TEXT("boolean"));
		Prop->SetStringField(TEXT("description"), Description);
		Prop->SetBoolField(TEXT("default"), DefaultValue);
		return Prop;
	}

	TSharedPtr<FJsonObject> ArrayProp(const FString& Description, TSharedPtr<FJsonObject> ItemSchema)
	{
		TSharedPtr<FJsonObject> Prop = MakeSchema(TEXT("array"));
		Prop->SetStringField(TEXT("description"), Description);
		Prop->SetObjectField(TEXT("items"), ItemSchema);
		return Prop;
	}

	TSharedPtr<FJsonObject> ObjectProp(const FString& Description, TSharedPtr<FJsonObject> Properties)
	{
		TSharedPtr<FJsonObject> Prop = MakeSchema(TEXT("object"));
		Prop->SetStringField(TEXT("description"), Description);
		Prop->SetObjectField(TEXT("properties"), Properties);
		return Prop;
	}

	TSharedPtr<FJsonObject> EnumProp(const FString& Description, const TArray<FString>& Values)
	{
		TSharedPtr<FJsonObject> Prop = MakeSchema(TEXT("string"));
		Prop->SetStringField(TEXT("description"), Description);

		TArray<TSharedPtr<FJsonValue>> EnumArray;
		for (const FString& Value : Values)
		{
			EnumArray.Add(MakeShareable(new FJsonValueString(Value)));
		}
		Prop->SetArrayField(TEXT("enum"), EnumArray);

		return Prop;
	}

	TSharedPtr<FJsonObject> TypeSpecSchema()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		// Category (required)
		Properties->SetObjectField(TEXT("category"),
			EnumProp(TEXT("Type category (bool, int, float, string, object, class, struct, enum, array, etc.)"),
			{
				TEXT("bool"), TEXT("byte"), TEXT("int"), TEXT("int64"), TEXT("float"), TEXT("double"),
				TEXT("string"), TEXT("name"), TEXT("text"), TEXT("vector"), TEXT("vector2d"),
				TEXT("rotator"), TEXT("transform"), TEXT("color"), TEXT("linear_color"),
				TEXT("object"), TEXT("class"), TEXT("interface"), TEXT("struct"), TEXT("enum"),
				TEXT("delegate"), TEXT("multicast_delegate"), TEXT("array"), TEXT("set"), TEXT("map")
			}));

		// ClassName (for object/class types)
		Properties->SetObjectField(TEXT("class_name"),
			StringProp(TEXT("Class name (for object/class types, e.g., 'AActor', 'UStaticMeshComponent')")));

		// StructName (for struct types)
		Properties->SetObjectField(TEXT("struct_name"),
			StringProp(TEXT("Struct name (for struct types, e.g., 'Vector', 'Rotator')")));

		// EnumName (for enum types)
		Properties->SetObjectField(TEXT("enum_name"),
			StringProp(TEXT("Enum name (for enum types)")));

		// ElementType (for array/set types) - simplified as string
		Properties->SetObjectField(TEXT("element_type"),
			StringProp(TEXT("Element type for array/set (can be nested type spec)")));

		// KeyType (for map types)
		Properties->SetObjectField(TEXT("key_type"),
			StringProp(TEXT("Key type for map (can be nested type spec)")));

		// ValueType (for map types)
		Properties->SetObjectField(TEXT("value_type"),
			StringProp(TEXT("Value type for map (can be nested type spec)")));

		// bIsReference
		Properties->SetObjectField(TEXT("is_reference"),
			BoolProp(TEXT("Whether this type is passed by reference"), false));

		// bIsConst
		Properties->SetObjectField(TEXT("is_const"),
			BoolProp(TEXT("Whether this type is const"), false));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Type specification for variables and parameters"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("category")});

		return Schema;
	}

	TSharedPtr<FJsonObject> FunctionParamSchema()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Parameter name")));

		Properties->SetObjectField(TEXT("type"),
			TypeSpecSchema());

		Properties->SetObjectField(TEXT("default_value"),
			StringProp(TEXT("Default value as string (optional)")));

		Properties->SetObjectField(TEXT("is_out_param"),
			BoolProp(TEXT("Whether this is an output parameter"), false));

		Properties->SetObjectField(TEXT("is_reference"),
			BoolProp(TEXT("Whether this parameter is passed by reference"), false));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Function parameter definition"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("name"), TEXT("type")});

		return Schema;
	}

	TSharedPtr<FJsonObject> FunctionSignatureSchema()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Function name")));

		Properties->SetObjectField(TEXT("inputs"),
			ArrayProp(TEXT("Input parameters"), FunctionParamSchema()));

		Properties->SetObjectField(TEXT("outputs"),
			ArrayProp(TEXT("Output parameters (return values)"), FunctionParamSchema()));

		Properties->SetObjectField(TEXT("is_static"),
			BoolProp(TEXT("Whether this is a static function"), false));

		Properties->SetObjectField(TEXT("is_pure"),
			BoolProp(TEXT("Whether this is a pure function (no side effects)"), false));

		Properties->SetObjectField(TEXT("is_const"),
			BoolProp(TEXT("Whether this is a const function"), false));

		Properties->SetObjectField(TEXT("is_public"),
			BoolProp(TEXT("Whether this function is publicly accessible"), true));

		Properties->SetObjectField(TEXT("category"),
			StringProp(TEXT("Category for organization in the editor")));

		Properties->SetObjectField(TEXT("description"),
			StringProp(TEXT("Description/tooltip for the function")));

		Properties->SetObjectField(TEXT("keywords"),
			StringProp(TEXT("Keywords for search")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Complete function signature"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("name")});

		return Schema;
	}

	TSharedPtr<FJsonObject> VariableSchema()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Variable name")));

		Properties->SetObjectField(TEXT("type"),
			TypeSpecSchema());

		Properties->SetObjectField(TEXT("default_value"),
			StringProp(TEXT("Default value as string")));

		Properties->SetObjectField(TEXT("category"),
			StringProp(TEXT("Category for organization")));

		Properties->SetObjectField(TEXT("description"),
			StringProp(TEXT("Tooltip description")));

		Properties->SetObjectField(TEXT("blueprint_read_write"),
			BoolProp(TEXT("Whether this variable is read/write accessible from Blueprints"), true));

		Properties->SetObjectField(TEXT("expose_on_spawn"),
			BoolProp(TEXT("Whether this variable is exposed on spawn"), false));

		Properties->SetObjectField(TEXT("replicated"),
			BoolProp(TEXT("Whether this variable is replicated"), false));

		Properties->SetObjectField(TEXT("save_game"),
			BoolProp(TEXT("Whether this variable is saved with save games"), false));

		Properties->SetObjectField(TEXT("edit_anywhere"),
			BoolProp(TEXT("Whether this variable can be edited in the editor"), true));

		Properties->SetObjectField(TEXT("blueprint_visible"),
			BoolProp(TEXT("Whether this variable is visible in Blueprints"), true));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Complete variable definition"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("name"), TEXT("type")});

		return Schema;
	}

	TSharedPtr<FJsonObject> ComponentSpecSchema()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("class"),
			StringProp(TEXT("Component class name (e.g., 'StaticMeshComponent', 'BoxComponent')")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Component variable name (auto-generated if not provided)")));

		Properties->SetObjectField(TEXT("parent"),
			StringProp(TEXT("Parent component name (omit for root attachment)")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Component specification"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("class")});

		return Schema;
	}

	// ============================================================================
	// Reader Tool Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> BlueprintRead()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path (e.g., '/Game/Blueprints/BP_Player')")));

		Properties->SetObjectField(TEXT("section"),
			EnumProp(TEXT("What to read. 'all' (default) returns summary/full overview. "
				"'summary' returns structure only (no graph data). "
				"'graph' returns a single graph (requires graph_name). "
				"'variables' returns variables only. "
				"'components' returns component hierarchy. "
				"'hierarchy' returns class inheritance chain. "
				"'overridable_functions' lists parent functions that can be overridden. "
				"'pins' returns the pin manifest for a single node (requires graph_name + node_id). "
				"'function_detail' returns the detailed signature of one function (requires function_name)."),
			{TEXT("all"), TEXT("summary"), TEXT("graph"), TEXT("variables"),
			 TEXT("components"), TEXT("hierarchy"), TEXT("overridable_functions"),
			 TEXT("pins"), TEXT("function_detail")}));

		Properties->SetObjectField(TEXT("node_id"),
			StringProp(TEXT("Node ID (required when section='pins')")));

		Properties->SetObjectField(TEXT("function_name"),
			StringProp(TEXT("Function name (required when section='function_detail'; also accepted for section='graph' as an alias of graph_name).")));

		Properties->SetObjectField(TEXT("target_class"),
			StringProp(TEXT("Optional class to scope the function lookup (section='function_detail')")));

		Properties->SetObjectField(TEXT("graph_name"),
			StringProp(TEXT("Graph name (required when section='graph'). Use a function name or 'EventGraph'.")));

		Properties->SetObjectField(TEXT("mode"),
			EnumProp(TEXT("Read mode for section='all': 'summary' or 'full'. "
				"For section='graph': 'auto' (default, large graphs return summary) or 'full' (force complete read)."),
			{TEXT("summary"), TEXT("full"), TEXT("auto")}));

		Properties->SetObjectField(TEXT("page"),
			IntProp(TEXT("Page number (0-based) for large graphs (section='graph' or 'all' with mode='full'). "
				"If omitted and graph is large, returns summary with paging info."), 0, INT32_MAX));

		Properties->SetObjectField(TEXT("page_size"),
			IntProp(TEXT("Nodes per page (default 100, max 200). Only used with 'page' parameter."), 10, 200));

		Properties->SetObjectField(TEXT("max_nodes"),
			IntProp(TEXT("Node count threshold for auto-summary (default 100). Graphs with more nodes return summary instead of full data. Set higher to get full data for larger graphs."), 10, 5000));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Unified Blueprint reader. Read any aspect of a Blueprint by specifying 'section'. "
				"Default section 'all' returns summary for large BPs and auto-upgrades to full for small ones (<=50 nodes). "
				"section='graph' with graph_name reads a single graph with large-graph paging support (100+ node threshold, adjustable via max_nodes)."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintGetNodePins()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("graph"),
			StringProp(TEXT("Graph name (e.g., 'EventGraph' or function name)")));

		Properties->SetObjectField(TEXT("node_id"),
			StringProp(TEXT("Node ID to inspect (e.g., 'node_0')")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Get pin manifest for ONE node. For multiple nodes, use blueprint.read(section:'graph', mode:'full') "
				 "instead — returns pins for ALL nodes in one call. Only use get_node_pins for a single node "
				 "after set_node_property reconstructs its pins."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("graph"), TEXT("node_id")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintDescribeNodeType()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("type"), StringProp(
			TEXT("Node type to describe. Accepts short names (Branch, ForEachLoop, Sequence, Delay), "
				 "full K2 class names (K2Node_IfThenElse), or catalog names (ComponentBoundEvent, CallFunction). "
				 "For function calls, use 'CallFunction' and check blueprint.get_node_pins after creation.")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, {TEXT("type")});
		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintDescribeFunction()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("function_name"), StringProp(
			TEXT("Function name to look up (e.g., 'SetActorLocation', 'ApplyDamage', 'GetVelocity'). "
				 "Accepts aliases (e.g., 'SetTimer' resolves to 'K2_SetTimer').")));

		Props->SetObjectField(TEXT("target_class"), StringProp(
			TEXT("Optional class to search first (e.g., 'CharacterMovementComponent', 'ACharacter'). "
				 "If omitted, searches alias map, common libraries, and all UBlueprintFunctionLibrary subclasses.")));

		Props->SetObjectField(TEXT("path"), StringProp(
			TEXT("Optional Blueprint asset path for context-aware search. "
				 "Enables searching the Blueprint's own functions, parent hierarchy, SCS components, and interfaces.")));

		Schema->SetStringField(TEXT("description"),
			TEXT("Look up a UFunction by name and return its exact signature: parameter names, types, "
				 "by-ref flags, return type, pure/latent markers, and owning class. "
				 "On failure, returns fuzzy suggestions and UPROPERTY detection. "
				 "Use this BEFORE writing plan_json to verify function names and pin names."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, {TEXT("function_name")});
		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintVerifyCompletion()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("asset_path"), StringProp(
			TEXT("Blueprint asset path to verify (e.g., '/Game/Blueprints/BP_MyActor')")));

		Props->SetObjectField(TEXT("expected_functions"),
			ArrayProp(
				TEXT("Function names that should exist in the Blueprint (optional)"),
				MakeSchema(TEXT("string"))));

		Props->SetObjectField(TEXT("expected_variables"),
			ArrayProp(
				TEXT("Variable names that should exist in the Blueprint (optional)"),
				MakeSchema(TEXT("string"))));

		Schema->SetStringField(TEXT("description"),
			TEXT("Verify a Blueprint is complete: compiles without errors, expected functions and variables "
				 "exist, no orphaned exec flows, no unwired required data pins. Returns a structured report "
				 "with all issues found. Use after applying plan_json or making multiple edits."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, {TEXT("asset_path")});
		return Schema;
	}

	// ============================================================================
	// Asset Writer Tool Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> BlueprintCreate()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Asset path for new Blueprint (e.g., '/Game/Blueprints/BP_NewActor')")));

		Properties->SetObjectField(TEXT("parent_class"),
			StringProp(TEXT("Parent class name (e.g., 'Actor', 'Character', '/Game/Blueprints/BP_Base'). "
				"Auto-defaults to UserWidget for WidgetBlueprint, AnimInstance for AnimationBlueprint.")));

		Properties->SetObjectField(TEXT("type"),
			EnumProp(TEXT("Blueprint type (defaults to 'Normal')"),
			{TEXT("Normal"), TEXT("Interface"), TEXT("FunctionLibrary"), TEXT("MacroLibrary"), TEXT("AnimationBlueprint"), TEXT("WidgetBlueprint")}));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Create a new empty Blueprint asset with the specified parent_class."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintScaffold()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Asset path for new Blueprint (e.g., '/Game/Blueprints/BP_NewActor')")));

		Properties->SetObjectField(TEXT("parent_class"),
			StringProp(TEXT("Parent class name (e.g., 'Actor', 'Character', 'Pawn', '/Game/Blueprints/BP_Base'). "
				"Auto-defaults to UserWidget for WidgetBlueprint, AnimInstance for AnimationBlueprint.")));

		Properties->SetObjectField(TEXT("type"),
			EnumProp(TEXT("Blueprint type (defaults to 'Normal')"),
			{TEXT("Normal"), TEXT("Interface"), TEXT("FunctionLibrary"), TEXT("MacroLibrary"), TEXT("AnimationBlueprint"), TEXT("WidgetBlueprint")}));

		Properties->SetObjectField(TEXT("components"),
			ArrayProp(TEXT("Components to add to the Blueprint (each: {class: string, name?: string, parent?: string})"),
				ComponentSpecSchema()));

		Properties->SetObjectField(TEXT("variables"),
			ArrayProp(TEXT("Variables to add to the Blueprint (each follows the standard variable schema with name, type, etc.)"),
				VariableSchema()));

		Properties->SetObjectField(TEXT("interfaces"),
			ArrayProp(TEXT("Interfaces to implement (e.g., ['BPI_Interactable', 'BPI_Damageable'])"),
				MakeSchema(TEXT("string"))));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Create a Blueprint with components, variables, and interfaces in one call. "
				"Replaces the pattern of separate create + add_component + add_variable + add_interface calls. "
				"Sub-operation failures (e.g., one component fails) are collected as warnings, not hard failures."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("parent_class")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintSetParentClass()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("new_parent"),
			StringProp(TEXT("New parent class name or path")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Change the parent class of a Blueprint (potentially destructive)"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("new_parent")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintAddInterface()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("interface"),
			StringProp(TEXT("Interface name or path (e.g., 'BPI_Interactable' or '/Game/Interfaces/BPI_Interactable')")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add an interface to a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("interface")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintCreateInterface()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Asset path for the new Blueprint Interface "
				"(e.g., '/Game/Interfaces/BPI_Interactable')")));

		// Function parameter schema (input or output)
		TSharedPtr<FJsonObject> ParamSchema = MakeSchema(TEXT("object"));
		{
			TSharedPtr<FJsonObject> ParamProps = MakeProperties();
			ParamProps->SetObjectField(TEXT("name"),
				StringProp(TEXT("Parameter name")));
			ParamProps->SetObjectField(TEXT("type"),
				StringProp(TEXT("Parameter type (e.g., 'Actor', 'Float', 'Bool', "
					"'Text', 'Vector', 'FString')")));
			ParamSchema->SetObjectField(TEXT("properties"), ParamProps);
			AddRequired(ParamSchema, {TEXT("name"), TEXT("type")});
		}

		// Single function definition schema
		TSharedPtr<FJsonObject> FuncSchema = MakeSchema(TEXT("object"));
		{
			TSharedPtr<FJsonObject> FuncProps = MakeProperties();
			FuncProps->SetObjectField(TEXT("name"),
				StringProp(TEXT("Function name (e.g., 'Interact', 'GetDisplayName')")));
			FuncProps->SetObjectField(TEXT("inputs"),
				ArrayProp(TEXT("Input parameters (optional). "
					"Functions with no outputs become events in implementing BPs."),
					ParamSchema));
			FuncProps->SetObjectField(TEXT("outputs"),
				ArrayProp(TEXT("Output/return parameters (optional). "
					"Functions WITH outputs must be implemented as functions, "
					"not events."),
					ParamSchema));
			FuncSchema->SetObjectField(TEXT("properties"), FuncProps);
			AddRequired(FuncSchema, {TEXT("name")});
		}

		Properties->SetObjectField(TEXT("functions"),
			ArrayProp(TEXT("Array of function signatures to define on the interface. "
				"Each function can have inputs, outputs, or both."),
				FuncSchema));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Create a new Blueprint Interface (BPI) asset with function signatures. "
				 "After creation, use blueprint.add_interface to implement it on target BPs. "
				 "Functions without outputs become events; functions with outputs become "
				 "overridable functions."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("functions")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintRemoveInterface()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("interface"),
			StringProp(TEXT("Interface name or path to remove")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Remove an interface from a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("interface")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintCompile()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("asset_path"),
			StringProp(TEXT("Alias of 'path' used by the verify path.")));

		Properties->SetObjectField(TEXT("verify"),
			BoolProp(TEXT("When true, run the verify-completion checks instead of a plain compile. "
				"Allows optional expected_functions/expected_variables params to assert structure."), false));

		Properties->SetObjectField(TEXT("expected_functions"),
			ArrayProp(TEXT("Function names that must exist (only used when verify=true)"),
				MakeSchema(TEXT("string"))));

		Properties->SetObjectField(TEXT("expected_variables"),
			ArrayProp(TEXT("Variable names that must exist (only used when verify=true)"),
				MakeSchema(TEXT("string"))));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Force compile a Blueprint and return compilation results. When verify=true, runs the full "
				"verification suite (compile + expected structure checks + orphaned exec flow detection) — "
				"the former blueprint.verify_completion tool is folded in."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintDelete()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("entity"),
			EnumProp(TEXT("What to delete. 'blueprint' (default) deletes the whole asset. "
				"Other values target an entity inside the Blueprint."),
				{TEXT("blueprint"), TEXT("node"), TEXT("component"), TEXT("variable"),
				 TEXT("function"), TEXT("interface")}));

		// Optional entity-specific fields. Validation happens in the handler
		// because required fields vary with 'entity'.
		Properties->SetObjectField(TEXT("node_id"),
			StringProp(TEXT("Node ID (required when entity='node')")));
		Properties->SetObjectField(TEXT("graph_name"),
			StringProp(TEXT("Graph name for node deletion (optional, aliased to 'graph')")));
		Properties->SetObjectField(TEXT("component_name"),
			StringProp(TEXT("Component name (required when entity='component')")));
		Properties->SetObjectField(TEXT("variable_name"),
			StringProp(TEXT("Variable name (required when entity='variable')")));
		Properties->SetObjectField(TEXT("function_name"),
			StringProp(TEXT("Function name (required when entity='function')")));
		Properties->SetObjectField(TEXT("interface_path"),
			StringProp(TEXT("Interface name or path (required when entity='interface')")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Delete an entity from a Blueprint. When entity='blueprint' (or omitted), deletes the whole asset. "
				"Other entity values dispatch to a specialized remover. Legacy tool names "
				"(blueprint.remove_node, remove_component, remove_variable, remove_function, remove_interface) "
				"are aliases that pre-fill the entity field."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintModify()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("entity"),
			EnumProp(TEXT("Entity class to modify."),
				{TEXT("component"), TEXT("function"), TEXT("variable"),
				 TEXT("node"), TEXT("pin_default"), TEXT("blueprint")}));

		Properties->SetObjectField(TEXT("action"),
			StringProp(TEXT("Entity-specific action. component: set_properties|reparent. "
				"function: set_signature|override_virtual. variable: set_properties. "
				"node: move|set_property. blueprint: set_parent_class|set_defaults. "
				"pin_default: (omit action).")));

		// Passthrough fields consumed by the dispatched handlers. We declare them as
		// optional; the downstream handler validates presence based on entity+action.
		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Entity name (variable, function, or component) — passed through to the underlying handler.")));
		Properties->SetObjectField(TEXT("variable_name"),
			StringProp(TEXT("Variable name (alias for 'name' when entity='variable')")));
		Properties->SetObjectField(TEXT("component_name"),
			StringProp(TEXT("Component name (alias for 'name' when entity='component')")));
		Properties->SetObjectField(TEXT("function_name"),
			StringProp(TEXT("Function name (alias for 'name' when entity='function')")));
		Properties->SetObjectField(TEXT("new_parent"),
			StringProp(TEXT("New parent class (blueprint.set_parent_class) or new parent component (component.reparent).")));
		Properties->SetObjectField(TEXT("graph"),
			StringProp(TEXT("Graph name (node actions)")));
		Properties->SetObjectField(TEXT("node_id"),
			StringProp(TEXT("Node ID (node actions)")));
		Properties->SetObjectField(TEXT("pin"),
			StringProp(TEXT("Pin reference (entity='pin_default')")));
		Properties->SetObjectField(TEXT("pin_name"),
			StringProp(TEXT("Pin name (alias for 'pin' when entity='pin_default')")));
		Properties->SetObjectField(TEXT("value"),
			StringProp(TEXT("Value for pin_default or set_property.")));
		Properties->SetObjectField(TEXT("property"),
			StringProp(TEXT("Property name (node.set_property)")));

		// Flexible object passthroughs
		TSharedPtr<FJsonObject> PropertiesSchema = MakeSchema(TEXT("object"));
		PropertiesSchema->SetStringField(TEXT("description"),
			TEXT("Generic property map. For component.set_properties: UE property name -> value. "
				"For variable.set_properties: accepts up to 21 fields including tooltip, category, "
				"is_public, is_read_only, is_editable, is_instance_editable, expose_on_spawn, is_private, "
				"is_replicated, replication_condition, is_save_game, is_advanced_display, is_multiline, "
				"ui_min, ui_max, clamp_min, clamp_max, bitmask, bitmask_enum, units, delta_value."));
		Properties->SetObjectField(TEXT("properties"), PropertiesSchema);

		TSharedPtr<FJsonObject> ChangesSchema = MakeSchema(TEXT("object"));
		ChangesSchema->SetStringField(TEXT("description"),
			TEXT("Change set for function.set_signature (inputs, outputs, is_pure, category, description)."));
		Properties->SetObjectField(TEXT("changes"), ChangesSchema);

		Properties->SetObjectField(TEXT("pos_x"),
			IntProp(TEXT("X position (node.move)"), -10000, 10000));
		Properties->SetObjectField(TEXT("pos_y"),
			IntProp(TEXT("Y position (node.move)"), -10000, 10000));

		TSharedPtr<FJsonObject> DefaultsSchema = MakeSchema(TEXT("object"));
		DefaultsSchema->SetStringField(TEXT("description"),
			TEXT("Class-default values to set when entity='blueprint' and action='set_defaults'."));
		Properties->SetObjectField(TEXT("defaults"), DefaultsSchema);

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Modify an entity inside a Blueprint. Dispatches on entity+action to the "
				"appropriate underlying handler. Legacy tool names (blueprint.modify_component, "
				"modify_function_signature, reparent_component, set_parent_class, set_pin_default, "
				"set_node_property, set_defaults) are aliases that pre-fill entity+action."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("entity")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintAdd()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("entity"),
			EnumProp(TEXT("Kind of entity to add."),
				{TEXT("node"), TEXT("variable"), TEXT("function"), TEXT("component"),
				 TEXT("custom_event"), TEXT("event_dispatcher"), TEXT("interface"),
				 TEXT("timeline")}));

		// Passthrough fields consumed by the dispatched handlers.
		Properties->SetObjectField(TEXT("graph"),
			StringProp(TEXT("Graph name (entity='node')")));
		Properties->SetObjectField(TEXT("type"),
			StringProp(TEXT("Node type (entity='node')")));
		Properties->SetObjectField(TEXT("node_type"),
			StringProp(TEXT("Alias for 'type' when entity='node'")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Entity name (variable, event, dispatcher, function, component, or timeline).")));

		Properties->SetObjectField(TEXT("variable"),
			VariableSchema());

		Properties->SetObjectField(TEXT("signature"),
			FunctionSignatureSchema());

		Properties->SetObjectField(TEXT("params"),
			ArrayProp(TEXT("Parameters for custom_event or event_dispatcher"), FunctionParamSchema()));

		Properties->SetObjectField(TEXT("class"),
			StringProp(TEXT("Component class (entity='component')")));
		Properties->SetObjectField(TEXT("parent"),
			StringProp(TEXT("Parent component (entity='component')")));
		Properties->SetObjectField(TEXT("interface"),
			StringProp(TEXT("Interface name or path (entity='interface')")));
		Properties->SetObjectField(TEXT("timeline_name"),
			StringProp(TEXT("Timeline name (entity='timeline')")));
		Properties->SetObjectField(TEXT("tracks"),
			ArrayProp(TEXT("Timeline tracks array (entity='timeline')"), MakeSchema(TEXT("object"))));
		Properties->SetObjectField(TEXT("pos_x"),
			IntProp(TEXT("X position (entity='node')"), -10000, 10000));
		Properties->SetObjectField(TEXT("pos_y"),
			IntProp(TEXT("Y position (entity='node')"), -10000, 10000));

		TSharedPtr<FJsonObject> PropsSchema = MakeSchema(TEXT("object"));
		PropsSchema->SetStringField(TEXT("description"),
			TEXT("Entity-specific properties (e.g., function_name for call nodes)."));
		Properties->SetObjectField(TEXT("properties"), PropsSchema);

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Add an entity to a Blueprint. Dispatches on entity to the matching specialized handler. "
				"Legacy tool names (blueprint.add_node, add_variable, add_function, add_component, "
				"add_interface, create_timeline, add_custom_event, add_event_dispatcher) are aliases "
				"that pre-fill the entity field."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("entity")});

		return Schema;
	}

	// ============================================================================
	// Variable Writer Tool Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> BlueprintAddVariable()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("variable"),
			VariableSchema());

		Properties->SetObjectField(TEXT("modify_only"),
			BoolProp(TEXT("When true, error if the variable does not already exist (prevents accidental creation). "
				"Default false: creates if missing, updates if present."), false));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Add or update a variable (upsert). If the variable already exists, modifies it in-place. "
				"Set modify_only=true to require the variable to already exist (old modify_variable behavior). "
				"Accepts flat format (name, type at top level) or nested {variable: {name, type, ...}}."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("variable")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintRemoveVariable()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Variable name to remove")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Remove a variable from a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name")});

		return Schema;
	}

	// NOTE: BlueprintModifyVariable has been consolidated into BlueprintAddVariable with upsert behavior.
	// Old tool name 'blueprint.modify_variable' is an alias that redirects to 'blueprint.add_variable'.

	// ============================================================================
	// Component Writer Tool Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> BlueprintAddComponent()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("class"),
			StringProp(TEXT("Component class name (e.g., 'StaticMeshComponent', 'BoxComponent')")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Component variable name (auto-generated if not provided)")));

		Properties->SetObjectField(TEXT("parent"),
			StringProp(TEXT("Parent component name (omit for root attachment)")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add a component to a Blueprint's component hierarchy"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("class")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintRemoveComponent()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Component name to remove")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Remove a component from a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintModifyComponent()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Component name to modify")));

		TSharedPtr<FJsonObject> PropsSchema = MakeSchema(TEXT("object"));
		PropsSchema->SetStringField(TEXT("description"), TEXT("Map of property names to values"));

		Properties->SetObjectField(TEXT("properties"), PropsSchema);

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Modify component properties"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name"), TEXT("properties")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintReparentComponent()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Component name to reparent")));

		Properties->SetObjectField(TEXT("new_parent"),
			StringProp(TEXT("New parent component name")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Change the parent of a component in the hierarchy"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name"), TEXT("new_parent")});

		return Schema;
	}

	// ============================================================================
	// Function Writer Tool Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> BlueprintAddFunction()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("function_type"),
			EnumProp(TEXT("What kind of function to add. "
				"'function' (default) creates a user-defined function graph. "
				"'custom_event' creates a custom event in the event graph. "
				"'event_dispatcher' creates a multicast delegate variable. "
				"'override' overrides a parent class or interface function."),
			{TEXT("function"), TEXT("custom_event"), TEXT("event_dispatcher"), TEXT("override")}));

		// 'name' is used by custom_event, event_dispatcher, and override (via function_name alias).
		// For function_type='function', the name comes from signature.name, but we also accept
		// a top-level 'name' for consistency.
		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Name of the function, event, or dispatcher to create. "
				"For function_type='function', can also be specified inside 'signature'.")));

		// signature — used by function_type='function'
		Properties->SetObjectField(TEXT("signature"),
			FunctionSignatureSchema());

		// params — used by custom_event and event_dispatcher
		Properties->SetObjectField(TEXT("params"),
			ArrayProp(TEXT("Parameters for custom_event or event_dispatcher"), FunctionParamSchema()));

		// function_name — used by override (name of parent function to override)
		Properties->SetObjectField(TEXT("function_name"),
			StringProp(TEXT("Parent function name to override (for function_type='override')")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Unified function creation tool. Creates functions (default), custom events, "
				"event dispatchers, or overrides parent/interface functions. "
				"Use function_type to select the operation. Old tool names "
				"(blueprint.add_custom_event, blueprint.add_event_dispatcher, blueprint.override_function) "
				"are aliases that auto-set function_type."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintRemoveFunction()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Function name to remove")));

		Properties->SetObjectField(TEXT("force"),
			BoolProp(TEXT("Force removal even if the function has graph logic. "
				"Default false. When false, removal is blocked if the function "
				"has more than entry+return nodes. Use true when you intend "
				"to recreate the function with different logic."), false));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Remove a function from a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintModifyFunctionSignature()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Function name to modify")));

		// Changes object
		TSharedPtr<FJsonObject> ChangesProps = MakeProperties();
		ChangesProps->SetObjectField(TEXT("inputs"), ArrayProp(TEXT("New input parameters"), FunctionParamSchema()));
		ChangesProps->SetObjectField(TEXT("outputs"), ArrayProp(TEXT("New output parameters"), FunctionParamSchema()));
		ChangesProps->SetObjectField(TEXT("is_pure"), BoolProp(TEXT("Pure function flag"), false));
		ChangesProps->SetObjectField(TEXT("category"), StringProp(TEXT("New category")));
		ChangesProps->SetObjectField(TEXT("description"), StringProp(TEXT("New description")));

		Properties->SetObjectField(TEXT("changes"),
			ObjectProp(TEXT("Signature changes to apply"), ChangesProps));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Modify a function's signature"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name"), TEXT("changes")});

		return Schema;
	}

	// NOTE: BlueprintAddEventDispatcher, BlueprintOverrideFunction, and BlueprintAddCustomEvent
	// have been consolidated into BlueprintAddFunction with function_type parameter.
	// Old tool names are aliases that set function_type automatically.

	// ============================================================================
	// Graph Writer Tool Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> BlueprintAddNode()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("graph"),
			StringProp(TEXT("Graph name (e.g., 'EventGraph' or function name)")));

		Properties->SetObjectField(TEXT("type"),
			StringProp(TEXT("Node type (e.g., 'CallFunction', 'Branch', 'VariableGet')")));

		TSharedPtr<FJsonObject> PropsSchema = MakeSchema(TEXT("object"));
		PropsSchema->SetStringField(TEXT("description"), TEXT("Node-specific properties (e.g., function_name, variable_name)"));

		Properties->SetObjectField(TEXT("properties"), PropsSchema);

		Properties->SetObjectField(TEXT("pos_x"),
			IntProp(TEXT("X position in graph"), -10000, 10000));

		Properties->SetObjectField(TEXT("pos_y"),
			IntProp(TEXT("Y position in graph"), -10000, 10000));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add a node to a Blueprint graph. For 3+ node operations, prefer blueprint.apply_plan_json (schema_version 2.0) which provides automatic pin resolution and atomic execution."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("graph"), TEXT("type")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintRemoveNode()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("graph"),
			StringProp(TEXT("Graph name")));

		Properties->SetObjectField(TEXT("node_id"),
			StringProp(TEXT("Node ID to remove")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Remove a node from a Blueprint graph. Pass routing_reason: 'op_unsupported' if plan path cannot express this operation."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("graph"), TEXT("node_id")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintConnectPins()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("graph"),
			StringProp(TEXT("Graph name")));

		Properties->SetObjectField(TEXT("source"),
			StringProp(TEXT("Source pin reference (format: 'node_id.pin_name')")));

		Properties->SetObjectField(TEXT("target"),
			StringProp(TEXT("Target pin reference (format: 'node_id.pin_name')")));

		// Optional semantic endpoint objects for deterministic server-side pin resolution.
		TSharedPtr<FJsonObject> SourceRefProp = MakeSchema(TEXT("object"));
		SourceRefProp->SetStringField(TEXT("description"),
			TEXT("Optional source endpoint object: {node_id: string, pin?: string, semantic?: string}"));
		Properties->SetObjectField(TEXT("source_ref"), SourceRefProp);

		TSharedPtr<FJsonObject> TargetRefProp = MakeSchema(TEXT("object"));
		TargetRefProp->SetStringField(TEXT("description"),
			TEXT("Optional target endpoint object: {node_id: string, pin?: string, semantic?: string}"));
		Properties->SetObjectField(TEXT("target_ref"), TargetRefProp);

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Connect two pins in a Blueprint graph. Use source/target exact pin refs or source_ref/target_ref semantic endpoint objects. For multi-step graph construction, prefer blueprint.apply_plan_json which handles pin wiring automatically via @step.auto syntax."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("graph")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintDisconnectPins()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("graph"),
			StringProp(TEXT("Graph name")));

		Properties->SetObjectField(TEXT("source"),
			StringProp(TEXT("Source pin reference (format: 'node_id.pin_name')")));

		Properties->SetObjectField(TEXT("target"),
			StringProp(TEXT("Target pin reference (format: 'node_id.pin_name')")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Disconnect two pins in a Blueprint graph. Pass routing_reason: 'op_unsupported' if plan path cannot express this operation."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("graph"), TEXT("source"), TEXT("target")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintSetPinDefault()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("graph"),
			StringProp(TEXT("Graph name")));

		Properties->SetObjectField(TEXT("pin"),
			StringProp(TEXT("Pin reference (format: 'node_id.pin_name')")));

		Properties->SetObjectField(TEXT("value"),
			StringProp(TEXT("Default value to set")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Set the default value of an input pin. For bulk defaults, prefer blueprint.apply_plan_json which sets defaults via the inputs map."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("graph"), TEXT("pin"), TEXT("value")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintSetNodeProperty()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("graph"),
			StringProp(TEXT("Graph name")));

		Properties->SetObjectField(TEXT("node_id"),
			StringProp(TEXT("Node ID")));

		Properties->SetObjectField(TEXT("property"),
			StringProp(TEXT("Property name")));

		Properties->SetObjectField(TEXT("value"),
			StringProp(TEXT("Property value")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Set a property on a node. Pass routing_reason: 'op_unsupported' if plan path cannot express this operation."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("graph"), TEXT("node_id"), TEXT("property"), TEXT("value")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintCreateTimeline()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path (e.g., '/Game/Blueprints/BP_Door')")));

		Properties->SetObjectField(TEXT("graph"),
			StringProp(TEXT("Event graph name (default: 'EventGraph'). Must be a ubergraph page.")));

		Properties->SetObjectField(TEXT("timeline_name"),
			StringProp(TEXT("Timeline variable name (e.g., 'DoorTimeline'). Auto-generated if omitted.")));

		Properties->SetObjectField(TEXT("length"),
			NumberProp(TEXT("Timeline length in seconds"), 5.0));

		Properties->SetObjectField(TEXT("auto_play"),
			BoolProp(TEXT("Start playing automatically on BeginPlay"), false));

		Properties->SetObjectField(TEXT("loop"),
			BoolProp(TEXT("Loop when finished"), false));

		Properties->SetObjectField(TEXT("replicated"),
			BoolProp(TEXT("Replicate timeline to clients"), false));

		Properties->SetObjectField(TEXT("ignore_time_dilation"),
			BoolProp(TEXT("Ignore global time dilation"), false));

		// Track item schema
		TSharedPtr<FJsonObject> TrackSchema = MakeSchema(TEXT("object"));
		{
			TSharedPtr<FJsonObject> TrackProps = MakeProperties();
			TrackProps->SetObjectField(TEXT("name"),
				StringProp(TEXT("Track name. Becomes the output pin name on the timeline node.")));
			TrackProps->SetObjectField(TEXT("type"),
				EnumProp(TEXT("Track type"), {TEXT("float"), TEXT("vector"), TEXT("color"), TEXT("event")}));
			TSharedPtr<FJsonObject> KeyTupleSchema = ArrayProp(
				TEXT("Key tuple values. Float: [time, value]. Vector: [time, x, y, z]. "
					"Color: [time, r, g, b, a]. Event: [time, 0]."),
				MakeSchema(TEXT("number")));
			TrackProps->SetObjectField(TEXT("keys"),
				ArrayProp(TEXT("Keyframe array. Float: [[time, value], ...]. Vector: [[time, x, y, z], ...]. "
					"Color: [[time, r, g, b, a], ...]. Event: [[time, 0], ...]."),
					KeyTupleSchema));
			TrackProps->SetObjectField(TEXT("interp"),
				EnumProp(TEXT("Interpolation mode (default: linear). Ignored for event tracks."),
					{TEXT("linear"), TEXT("cubic"), TEXT("constant")}));
			TrackSchema->SetStringField(TEXT("description"), TEXT("Track definition"));
			TrackSchema->SetObjectField(TEXT("properties"), TrackProps);
			AddRequired(TrackSchema, {TEXT("name"), TEXT("type"), TEXT("keys")});
		}

		Properties->SetObjectField(TEXT("tracks"),
			ArrayProp(TEXT("Array of track definitions (at least one required). Each track produces an output pin on the timeline node."),
				TrackSchema));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Create a Timeline node with tracks and curve data in a Blueprint event graph. "
				"Returns the node ID and complete pin manifest for wiring with connect_pins or plan_json. "
				"Timelines only work in Actor-based Blueprints (not Widget BPs, Component BPs, etc.)."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("tracks")});

		return Schema;
	}

	// ============================================================================
	// Plan JSON Tool Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> BlueprintPreviewPlanJson()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("asset_path"),
			StringProp(TEXT("Content path of the target Blueprint asset (e.g., '/Game/Blueprints/BP_Player')")));

		// graph_target — optional, defaults to EventGraph
		TSharedPtr<FJsonObject> GraphTargetProp = StringProp(TEXT("Name of the graph to target"));
		GraphTargetProp->SetStringField(TEXT("default"), TEXT("EventGraph"));
		Properties->SetObjectField(TEXT("graph_target"), GraphTargetProp);

		// mode — optional, defaults to "merge", enum restricted
		TSharedPtr<FJsonObject> ModeProp = EnumProp(
			TEXT("'merge' (default): preserves existing nodes, adds new ones alongside. "
				 "'replace': clears all non-entry/non-result nodes from the target graph before applying — "
				 "use to rewrite a function atomically instead of manually deleting nodes."),
			{TEXT("merge"), TEXT("replace")});
		ModeProp->SetStringField(TEXT("default"), TEXT("merge"));
		Properties->SetObjectField(TEXT("mode"), ModeProp);

		// plan_json — required object containing the intent-level plan
		TSharedPtr<FJsonObject> PlanJsonProp = MakeSchema(TEXT("object"));
		PlanJsonProp->SetStringField(TEXT("description"),
			TEXT("Intent-level plan. Use schema_version \"2.0\" (recommended): the executor creates nodes first, introspects actual pin names, then wires using ground-truth names — you do not need to know exact pin names. v2.0 inputs support @step.auto (type-based auto-match), @step.~hint (fuzzy prefix match), and @step.pinName (standard). v1.0 (legacy) uses a lowerer that maps ops to concrete nodes. Fields: schema_version (string), steps array. Each step: step_id, op (call, get_var, set_var, branch, sequence, cast, event, custom_event, for_loop, for_each_loop, delay, is_valid, print_string, spawn_actor, make_struct, break_struct, return, comment), target, and optional inputs/exec_after/exec_outputs."));
		Properties->SetObjectField(TEXT("plan_json"), PlanJsonProp);

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Preview an intent-level Blueprint plan without mutating the asset. v2.0 plans (schema_version \"2.0\") return resolved_steps with ground-truth pin names (no lowering needed). v1.0 plans return lowered_ops_count and plan_summary. Both paths validate the plan, compute a diff against the current graph, and return a fingerprint for drift detection. Use before blueprint.apply_plan_json to verify correctness."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("asset_path"), TEXT("plan_json")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintApplyPlanJson()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("asset_path"),
			StringProp(TEXT("Content path of the target Blueprint asset (e.g., '/Game/Blueprints/BP_Player')")));

		// graph_target — optional, defaults to EventGraph
		TSharedPtr<FJsonObject> GraphTargetProp = StringProp(TEXT("Name of the graph to target"));
		GraphTargetProp->SetStringField(TEXT("default"), TEXT("EventGraph"));
		Properties->SetObjectField(TEXT("graph_target"), GraphTargetProp);

		// mode — optional, defaults to "merge", enum restricted
		TSharedPtr<FJsonObject> ModeProp = EnumProp(
			TEXT("'merge' (default): preserves existing nodes, adds new ones alongside. "
				 "'replace': clears all non-entry/non-result nodes from the target graph before applying — "
				 "use to rewrite a function atomically instead of manually deleting nodes."),
			{TEXT("merge"), TEXT("replace")});
		ModeProp->SetStringField(TEXT("default"), TEXT("merge"));
		Properties->SetObjectField(TEXT("mode"), ModeProp);

		// plan_json — required object containing the intent-level plan
		TSharedPtr<FJsonObject> PlanJsonProp = MakeSchema(TEXT("object"));
		PlanJsonProp->SetStringField(TEXT("description"),
			TEXT("Intent-level plan. Use schema_version \"2.0\" (recommended): the executor creates nodes first, introspects actual pin names, then wires using ground-truth names — you do not need to know exact pin names. v2.0 inputs support @step.auto (type-based auto-match), @step.~hint (fuzzy prefix match), and @step.pinName (standard). v1.0 (legacy) uses a lowerer that maps ops to concrete nodes. Fields: schema_version (string), steps array. Each step: step_id, op (call, get_var, set_var, branch, sequence, cast, event, custom_event, for_loop, for_each_loop, delay, is_valid, print_string, spawn_actor, make_struct, break_struct, return, comment), target, and optional inputs/exec_after/exec_outputs."));
		Properties->SetObjectField(TEXT("plan_json"), PlanJsonProp);

		// preview_fingerprint — optional string for drift detection (never required)
		Properties->SetObjectField(TEXT("preview_fingerprint"),
			StringProp(TEXT("Optional 8-char hex fingerprint from a prior blueprint.preview_plan_json call. Used for drift detection — if omitted or mismatched, apply proceeds with inline validation via the resolve+execute pipeline.")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Apply an intent-level Blueprint plan atomically. Supports schema_version \"1.0\" (lowerer path: maps ops to concrete nodes) and \"2.0\" (plan executor: creates nodes first, introspects real pin names via pin manifests, then wires using ground-truth names). v2.0 supports @step.auto (type-based auto-match) and @step.~hint (fuzzy prefix) data wire syntax so you never need to guess pin names. preview_fingerprint is optional — if omitted, apply proceeds with inline validation. Result includes wiring_errors and pin_manifests for self-correction. Compiles once at the end."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("asset_path"), TEXT("plan_json")});

		return Schema;
	}

	// ============================================================================
	// Template Tool Schemas
	// ============================================================================

	// Templates are reference-only. No create/clone tools -- only get_template and list_templates.

	TSharedPtr<FJsonObject> BlueprintCreateFromTemplate()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("template_id"),
			StringProp(TEXT("Factory template ID (e.g., 'gun', 'projectile'). Use blueprint.list_templates to see available templates."), true));

		Props->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path to create (e.g., '/Game/Blueprints/BP_Pistol')"), true));

		Props->SetObjectField(TEXT("preset"),
			StringProp(TEXT("Optional preset name (e.g., 'Pistol', 'Rocket'). Applies preset parameter values from the template."), false));

		// parameters is an object with string values
		{
			TSharedPtr<FJsonObject> ParamsProp = MakeSchema(TEXT("object"));
			ParamsProp->SetStringField(TEXT("description"),
				TEXT("Optional parameter overrides. Keys are parameter names from the template, values are strings."));
			TSharedPtr<FJsonObject> AddlProps = MakeSchema(TEXT("string"));
			ParamsProp->SetObjectField(TEXT("additionalProperties"), AddlProps);
			Props->SetObjectField(TEXT("parameters"), ParamsProp);
		}

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, {TEXT("template_id"), TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintGetTemplate()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("template_id"),
			StringProp(TEXT("ID of the template to view (factory, reference, or library)")));

		Properties->SetObjectField(TEXT("pattern"),
			StringProp(TEXT("For reference templates: pattern name to filter. "
				"For factory templates: function name to extract full plan JSON (e.g., pattern=\"Fire\" returns Fire's complete plan). "
				"For library templates: specify a function name to retrieve its full node graph.")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("View a template's content. Without pattern: shows parameters, presets, function outlines. "
				"With pattern: for factory templates returns a function's full plan_json (ready for apply_plan_json); "
				"for reference templates returns a specific pattern; "
				"for library templates returns a function's full node graph."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("template_id")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintListTemplates()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("type"),
			EnumProp(TEXT("Filter by template type"), {TEXT("factory"), TEXT("reference"), TEXT("library")}));

		Properties->SetObjectField(TEXT("query"),
			StringProp(TEXT("Search query to find templates by name, tag, function name, or keyword. "
				"Searches across all templates including library templates from extracted projects.")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("List available templates. Use query parameter to search by name, tag, function name, "
				"or keyword across all templates including library templates from extracted projects."));
		Schema->SetObjectField(TEXT("properties"), Properties);

		return Schema;
	}

	// ============================================================================
	// AnimBP Writer Tool Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> AnimBPAddStateMachine()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Animation Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("State machine name")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add a state machine to an Animation Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name")});

		return Schema;
	}

	TSharedPtr<FJsonObject> AnimBPAddState()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Animation Blueprint asset path")));

		Properties->SetObjectField(TEXT("machine"),
			StringProp(TEXT("State machine name")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("State name")));

		Properties->SetObjectField(TEXT("animation"),
			StringProp(TEXT("Animation asset path (optional)")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add a state to a state machine"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("machine"), TEXT("name")});

		return Schema;
	}

	TSharedPtr<FJsonObject> AnimBPAddTransition()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Animation Blueprint asset path")));

		Properties->SetObjectField(TEXT("machine"),
			StringProp(TEXT("State machine name")));

		Properties->SetObjectField(TEXT("from"),
			StringProp(TEXT("Source state name")));

		Properties->SetObjectField(TEXT("to"),
			StringProp(TEXT("Target state name")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add a transition between two states"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("machine"), TEXT("from"), TEXT("to")});

		return Schema;
	}

	TSharedPtr<FJsonObject> AnimBPSetTransitionRule()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Animation Blueprint asset path")));

		Properties->SetObjectField(TEXT("machine"),
			StringProp(TEXT("State machine name")));

		Properties->SetObjectField(TEXT("from"),
			StringProp(TEXT("Source state name")));

		Properties->SetObjectField(TEXT("to"),
			StringProp(TEXT("Target state name")));

		TSharedPtr<FJsonObject> RuleSchema = MakeSchema(TEXT("object"));
		RuleSchema->SetStringField(TEXT("description"), TEXT("Transition rule definition (graph nodes)"));

		Properties->SetObjectField(TEXT("rule"), RuleSchema);

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Set the transition rule for a state transition"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("machine"), TEXT("from"), TEXT("to"), TEXT("rule")});

		return Schema;
	}

	// ============================================================================
	// Widget Writer Tool Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> WidgetAddWidget()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Widget Blueprint asset path")));

		Properties->SetObjectField(TEXT("class"),
			StringProp(TEXT("Widget class name (e.g., 'Button', 'TextBlock', 'Image')")));

		Properties->SetObjectField(TEXT("parent"),
			StringProp(TEXT("Parent widget name (omit for root)")));

		Properties->SetObjectField(TEXT("slot"),
			StringProp(TEXT("Slot type (e.g., 'Canvas', 'HorizontalBox', 'VerticalBox')")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Widget variable name (auto-generated if not provided)")));

		Properties->SetObjectField(TEXT("is_variable"),
			BoolProp(TEXT("Mark widget as a Blueprint variable (default: true). "
						  "Set false for decorative widgets that don't need graph access."), true));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add a widget to a Widget Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("class")});

		return Schema;
	}

	TSharedPtr<FJsonObject> WidgetRemoveWidget()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Widget Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Widget name to remove")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Remove a widget from a Widget Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name")});

		return Schema;
	}

	TSharedPtr<FJsonObject> WidgetSetProperty()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Widget Blueprint asset path")));

		Properties->SetObjectField(TEXT("widget"),
			StringProp(TEXT("Widget name")));

		Properties->SetObjectField(TEXT("property"),
			StringProp(TEXT("Property name (e.g., 'Text', 'Color', 'Visibility')")));

		Properties->SetObjectField(TEXT("value"),
			StringProp(TEXT("Property value")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Set a property on a widget"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("widget"), TEXT("property"), TEXT("value")});

		return Schema;
	}

	TSharedPtr<FJsonObject> WidgetBindProperty()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Widget Blueprint asset path")));

		Properties->SetObjectField(TEXT("widget"),
			StringProp(TEXT("Widget name")));

		Properties->SetObjectField(TEXT("property"),
			StringProp(TEXT("Property name to bind")));

		Properties->SetObjectField(TEXT("function"),
			StringProp(TEXT("Function name to bind to")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Bind a widget property to a Blueprint function"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("widget"), TEXT("property"), TEXT("function")});

		return Schema;
	}

} // namespace OliveBlueprintSchemas
