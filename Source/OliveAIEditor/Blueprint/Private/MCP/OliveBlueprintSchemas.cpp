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

		Properties->SetObjectField(TEXT("mode"),
			EnumProp(TEXT("Read mode: 'summary' for structure only, 'full' for complete graph data"),
			{TEXT("summary"), TEXT("full")}));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Read Blueprint structure and optionally full graph data"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintReadFunction()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("function_name"),
			StringProp(TEXT("Function name to read")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Read a single function graph from a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("function_name")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintReadEventGraph()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("graph_name"),
			StringProp(TEXT("Event graph name (defaults to 'EventGraph' if not specified)")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Read an event graph from a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintReadVariables()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Read all variables from a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintReadComponents()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Read component hierarchy from a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintReadHierarchy()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Read class hierarchy from a Blueprint (parent chain)"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintListOverridableFunctions()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("List all functions from parent classes that can be overridden"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

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
			StringProp(TEXT("Parent class name (e.g., 'Actor', 'Character', '/Game/Blueprints/BP_Base')")));

		Properties->SetObjectField(TEXT("type"),
			EnumProp(TEXT("Blueprint type (defaults to 'Normal')"),
			{TEXT("Normal"), TEXT("Interface"), TEXT("FunctionLibrary"), TEXT("MacroLibrary"), TEXT("AnimationBlueprint"), TEXT("WidgetBlueprint")}));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Create a new Blueprint asset"));
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

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Force compile a Blueprint and return compilation results"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintDelete()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path to delete")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Delete a Blueprint asset (requires Tier 3 confirmation)"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path")});

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

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add a variable to a Blueprint"));
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

	TSharedPtr<FJsonObject> BlueprintModifyVariable()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Variable name to modify")));

		// Changes object - flexible schema
		TSharedPtr<FJsonObject> ChangesProps = MakeProperties();
		ChangesProps->SetObjectField(TEXT("default_value"), StringProp(TEXT("New default value")));
		ChangesProps->SetObjectField(TEXT("category"), StringProp(TEXT("New category")));
		ChangesProps->SetObjectField(TEXT("description"), StringProp(TEXT("New description")));
		ChangesProps->SetObjectField(TEXT("expose_on_spawn"), BoolProp(TEXT("Expose on spawn flag"), false));
		ChangesProps->SetObjectField(TEXT("replicated"), BoolProp(TEXT("Replication flag"), false));

		Properties->SetObjectField(TEXT("changes"),
			ObjectProp(TEXT("Object containing fields to modify"), ChangesProps));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Modify an existing variable's properties"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name"), TEXT("changes")});

		return Schema;
	}

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

		Properties->SetObjectField(TEXT("signature"),
			FunctionSignatureSchema());

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add a function to a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("signature")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintRemoveFunction()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Function name to remove")));

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

	TSharedPtr<FJsonObject> BlueprintAddEventDispatcher()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Event dispatcher name")));

		Properties->SetObjectField(TEXT("params"),
			ArrayProp(TEXT("Event parameters"), FunctionParamSchema()));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add an event dispatcher to a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintOverrideFunction()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("function_name"),
			StringProp(TEXT("Function name from parent class to override")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Override a parent class function in a Blueprint"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("function_name")});

		return Schema;
	}

	TSharedPtr<FJsonObject> BlueprintAddCustomEvent()
	{
		TSharedPtr<FJsonObject> Properties = MakeProperties();

		Properties->SetObjectField(TEXT("path"),
			StringProp(TEXT("Blueprint asset path")));

		Properties->SetObjectField(TEXT("name"),
			StringProp(TEXT("Custom event name")));

		Properties->SetObjectField(TEXT("params"),
			ArrayProp(TEXT("Event parameters"), FunctionParamSchema()));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Add a custom event to a Blueprint's event graph"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("name")});

		return Schema;
	}

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
		Schema->SetStringField(TEXT("description"), TEXT("Add a node to a Blueprint graph"));
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
		Schema->SetStringField(TEXT("description"), TEXT("Remove a node from a Blueprint graph"));
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

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"), TEXT("Connect two pins in a Blueprint graph"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("graph"), TEXT("source"), TEXT("target")});

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
		Schema->SetStringField(TEXT("description"), TEXT("Disconnect two pins in a Blueprint graph"));
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
		Schema->SetStringField(TEXT("description"), TEXT("Set the default value of an input pin"));
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
		Schema->SetStringField(TEXT("description"), TEXT("Set a property on a node"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("path"), TEXT("graph"), TEXT("node_id"), TEXT("property"), TEXT("value")});

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
