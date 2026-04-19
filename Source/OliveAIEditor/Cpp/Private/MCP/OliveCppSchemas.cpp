// Copyright Bode Software. All Rights Reserved.

#include "OliveCppSchemas.h"
#include "OliveBlueprintSchemas.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace OliveCppSchemas
{
	// Local helpers matching OliveBlueprintSchemas pattern
	static TSharedPtr<FJsonObject> MakeSchema(const FString& Type)
	{
		TSharedPtr<FJsonObject> Schema = MakeShareable(new FJsonObject());
		Schema->SetStringField(TEXT("type"), Type);
		return Schema;
	}

	static TSharedPtr<FJsonObject> MakeProperties()
	{
		return MakeShareable(new FJsonObject());
	}

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
	// Reflection Reader Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> CppReadClass()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("class_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Class name to inspect (e.g., ACharacter, UActorComponent). Prefix (A/U/F) is optional.")));
		Props->SetObjectField(TEXT("include"),
			OliveBlueprintSchemas::EnumProp(
				TEXT("What to include: 'all' = full reflection dump (default), 'callable' = BlueprintCallable/BlueprintPure functions only, 'overridable' = BlueprintImplementableEvent/BlueprintNativeEvent functions only"),
				{ TEXT("all"), TEXT("callable"), TEXT("overridable") }));
		Props->SetObjectField(TEXT("include_inherited"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include properties/functions from parent classes"), false));
		Props->SetObjectField(TEXT("include_functions"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include function list (only used when include='all')"), true));
		Props->SetObjectField(TEXT("include_properties"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include property list (only used when include='all')"), true));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("class_name") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppListBlueprintCallable()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("class_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Class name to list callable functions for")));
		Props->SetObjectField(TEXT("include_inherited"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include callables from parent classes"), true));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("class_name") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppListOverridable()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("class_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Class name to list overridable events/functions for")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("class_name") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppReadEnum()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("enum_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Enum type name (e.g., ECollisionChannel, EMovementMode)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("enum_name") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppReadStruct()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("struct_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Struct name to inspect (e.g., FVector, FHitResult)")));
		Props->SetObjectField(TEXT("include_inherited"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include fields from parent struct"), false));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("struct_name") });
		return Schema;
	}

	// ============================================================================
	// Source Reader Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> CppReadHeader()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("file_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Relative path to .h file within project Source/ directory")));
		Props->SetObjectField(TEXT("start_line"),
			OliveBlueprintSchemas::IntProp(TEXT("Start line number (1-based, 0 for beginning)")));
		Props->SetObjectField(TEXT("end_line"),
			OliveBlueprintSchemas::IntProp(TEXT("End line number (0 for end of file)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("file_path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppReadSource()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("file_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Relative path to .cpp file within project Source/ directory")));
		Props->SetObjectField(TEXT("start_line"),
			OliveBlueprintSchemas::IntProp(TEXT("Start line number (1-based, 0 for beginning)")));
		Props->SetObjectField(TEXT("end_line"),
			OliveBlueprintSchemas::IntProp(TEXT("End line number (0 for end of file)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("file_path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppListProjectClasses()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("module_filter"),
			OliveBlueprintSchemas::StringProp(TEXT("Filter to classes in a specific module (e.g., 'MyGame')")));
		Props->SetObjectField(TEXT("parent_class"),
			OliveBlueprintSchemas::StringProp(TEXT("Only list classes inheriting from this class")));

		Schema->SetObjectField(TEXT("properties"), Props);
		// No required fields - both parameters are optional
		return Schema;
	}

	// ============================================================================
	// Source Writer Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> CppCreateClass()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("class_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Name for the new class (without prefix, e.g., 'MyActor')")));
		Props->SetObjectField(TEXT("parent_class"),
			OliveBlueprintSchemas::StringProp(TEXT("Parent class (e.g., AActor, UActorComponent)")));
		Props->SetObjectField(TEXT("module_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Module to create the class in")));
		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Subdirectory within module Source/ (e.g., 'Characters')")));
		Props->SetObjectField(TEXT("interfaces"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Interfaces to implement"),
				OliveBlueprintSchemas::StringProp(TEXT("Interface class name"))));

		// properties array: each item is an object with property definition fields
		auto PropItemProps = MakeProperties();
		PropItemProps->SetObjectField(TEXT("name"), OliveBlueprintSchemas::StringProp(TEXT("Property name")));
		PropItemProps->SetObjectField(TEXT("type"), OliveBlueprintSchemas::StringProp(TEXT("C++ type")));
		PropItemProps->SetObjectField(TEXT("category"), OliveBlueprintSchemas::StringProp(TEXT("UPROPERTY Category")));
		PropItemProps->SetObjectField(TEXT("specifiers"),
			OliveBlueprintSchemas::ArrayProp(TEXT("UPROPERTY specifiers"),
				OliveBlueprintSchemas::StringProp(TEXT("Specifier"))));
		PropItemProps->SetObjectField(TEXT("default_value"), OliveBlueprintSchemas::StringProp(TEXT("Default value expression")));
		auto PropItemSchema = MakeSchema(TEXT("object"));
		PropItemSchema->SetObjectField(TEXT("properties"), PropItemProps);
		Props->SetObjectField(TEXT("properties"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Initial UPROPERTY definitions"), PropItemSchema));

		// functions array: each item is an object with function definition fields
		auto FuncItemProps = MakeProperties();
		FuncItemProps->SetObjectField(TEXT("name"), OliveBlueprintSchemas::StringProp(TEXT("Function name")));
		FuncItemProps->SetObjectField(TEXT("return_type"), OliveBlueprintSchemas::StringProp(TEXT("Return type")));
		FuncItemProps->SetObjectField(TEXT("specifiers"),
			OliveBlueprintSchemas::ArrayProp(TEXT("UFUNCTION specifiers"),
				OliveBlueprintSchemas::StringProp(TEXT("Specifier"))));
		FuncItemProps->SetObjectField(TEXT("is_virtual"), OliveBlueprintSchemas::BoolProp(TEXT("Whether function is virtual"), false));
		auto FuncItemSchema = MakeSchema(TEXT("object"));
		FuncItemSchema->SetObjectField(TEXT("properties"), FuncItemProps);
		Props->SetObjectField(TEXT("functions"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Initial UFUNCTION declarations"), FuncItemSchema));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("class_name"), TEXT("parent_class"), TEXT("module_name") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppAddProperty()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("file_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Path to .h file")));
		Props->SetObjectField(TEXT("property_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Name for the new property")));
		Props->SetObjectField(TEXT("property_type"),
			OliveBlueprintSchemas::StringProp(TEXT("C++ type (e.g., float, FVector, UStaticMesh*)")));
		Props->SetObjectField(TEXT("category"),
			OliveBlueprintSchemas::StringProp(TEXT("UPROPERTY Category")));
		Props->SetObjectField(TEXT("specifiers"),
			OliveBlueprintSchemas::ArrayProp(TEXT("UPROPERTY specifiers (e.g., EditAnywhere, BlueprintReadWrite)"),
				OliveBlueprintSchemas::StringProp(TEXT("Specifier"))));
		Props->SetObjectField(TEXT("default_value"),
			OliveBlueprintSchemas::StringProp(TEXT("Default value expression")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("file_path"), TEXT("property_name"), TEXT("property_type") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppAddFunction()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("file_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Path to .h file")));
		Props->SetObjectField(TEXT("function_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Name for the new function")));
		Props->SetObjectField(TEXT("return_type"),
			OliveBlueprintSchemas::StringProp(TEXT("Return type (empty/void for void)")));

		// parameters array: each item is {name, type}
		auto ParamItemProps = MakeProperties();
		ParamItemProps->SetObjectField(TEXT("name"), OliveBlueprintSchemas::StringProp(TEXT("Parameter name")));
		ParamItemProps->SetObjectField(TEXT("type"), OliveBlueprintSchemas::StringProp(TEXT("C++ type")));
		auto ParamItemSchema = MakeSchema(TEXT("object"));
		ParamItemSchema->SetObjectField(TEXT("properties"), ParamItemProps);
		Props->SetObjectField(TEXT("parameters"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Function parameters"), ParamItemSchema));

		Props->SetObjectField(TEXT("specifiers"),
			OliveBlueprintSchemas::ArrayProp(TEXT("UFUNCTION specifiers (e.g., BlueprintCallable, Category=AI)"),
				OliveBlueprintSchemas::StringProp(TEXT("Specifier"))));
		Props->SetObjectField(TEXT("is_virtual"),
			OliveBlueprintSchemas::BoolProp(TEXT("Whether function is virtual"), false));
		Props->SetObjectField(TEXT("body"),
			OliveBlueprintSchemas::StringProp(TEXT("Optional function body for .cpp")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("file_path"), TEXT("function_name") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppModifySource()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("file_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Path to .h/.cpp/.inl file under Source/")));
		Props->SetObjectField(TEXT("anchor_text"),
			OliveBlueprintSchemas::StringProp(TEXT("Exact text anchor to locate")));
		Props->SetObjectField(TEXT("operation"),
			OliveBlueprintSchemas::EnumProp(TEXT("Patch operation"),
				{ TEXT("replace"), TEXT("insert_before"), TEXT("insert_after") }));
		Props->SetObjectField(TEXT("replacement_text"),
			OliveBlueprintSchemas::StringProp(TEXT("Replacement or insertion text")));
		Props->SetObjectField(TEXT("occurrence"),
			OliveBlueprintSchemas::IntProp(TEXT("1-based occurrence index when multiple matches exist")));
		Props->SetObjectField(TEXT("start_line"),
			OliveBlueprintSchemas::IntProp(TEXT("Optional 1-based lower line guard (0 = no guard)")));
		Props->SetObjectField(TEXT("end_line"),
			OliveBlueprintSchemas::IntProp(TEXT("Optional 1-based upper line guard (0 = no guard)")));
		Props->SetObjectField(TEXT("require_unique_match"),
			OliveBlueprintSchemas::BoolProp(TEXT("If true, fail when anchor appears more than once"), true));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("file_path"), TEXT("anchor_text"), TEXT("operation") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppCompile()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Schema->SetObjectField(TEXT("properties"), Props);
		// No required fields - triggers Live Coding with no parameters
		return Schema;
	}

	// ============================================================================
	// P5 Consolidated Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> CppRead()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("entity"),
			OliveBlueprintSchemas::EnumProp(
				TEXT("What to read. 'class' = reflection dump for a UE class; 'enum' = enum values/metadata; 'struct' = struct members; 'header' = read a .h file; 'source' = read a .cpp file."),
				{ TEXT("class"), TEXT("enum"), TEXT("struct"), TEXT("header"), TEXT("source") }));

		// entity = class
		Props->SetObjectField(TEXT("class_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Class name (entity='class'; e.g., ACharacter, UActorComponent). Prefix (A/U/F) is optional.")));
		Props->SetObjectField(TEXT("include"),
			OliveBlueprintSchemas::EnumProp(
				TEXT("Class read mode (entity='class'): 'all' = full reflection dump (default), 'callable' = BlueprintCallable/BlueprintPure only, 'overridable' = BlueprintImplementableEvent/BlueprintNativeEvent only."),
				{ TEXT("all"), TEXT("callable"), TEXT("overridable") }));
		Props->SetObjectField(TEXT("include_inherited"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include inherited members (entity='class'|'struct')"), false));
		Props->SetObjectField(TEXT("include_functions"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include function list (entity='class', include='all')"), true));
		Props->SetObjectField(TEXT("include_properties"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include property list (entity='class', include='all')"), true));

		// entity = enum
		Props->SetObjectField(TEXT("enum_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Enum type name (entity='enum'; e.g., ECollisionChannel, EMovementMode)")));

		// entity = struct
		Props->SetObjectField(TEXT("struct_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Struct name (entity='struct'; e.g., FVector, FHitResult)")));

		// entity = header | source
		Props->SetObjectField(TEXT("file_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Relative path to .h / .cpp file under project Source/ (entity='header'|'source')")));
		Props->SetObjectField(TEXT("start_line"),
			OliveBlueprintSchemas::IntProp(TEXT("Start line 1-based (entity='header'|'source'; 0 for beginning)")));
		Props->SetObjectField(TEXT("end_line"),
			OliveBlueprintSchemas::IntProp(TEXT("End line 1-based (entity='header'|'source'; 0 for end of file)")));

		Schema->SetStringField(TEXT("description"),
			TEXT("Read C++ entities via reflection or source files. Dispatches on 'entity' (class|enum|struct|header|source). "
				"Legacy cpp.read_class / cpp.read_enum / cpp.read_struct / cpp.read_header / cpp.read_source are aliases that pre-fill 'entity'."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("entity") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppList()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("kind"),
			OliveBlueprintSchemas::EnumProp(
				TEXT("What to list. 'project' = C++ classes in project Source/; 'blueprint_callable' = BlueprintCallable/BlueprintPure on a class; 'overridable' = BlueprintImplementableEvent/BlueprintNativeEvent on a class."),
				{ TEXT("project"), TEXT("blueprint_callable"), TEXT("overridable") }));

		// kind = project
		Props->SetObjectField(TEXT("module_filter"),
			OliveBlueprintSchemas::StringProp(TEXT("Filter to classes in a specific module (kind='project'; e.g., 'MyGame')")));
		Props->SetObjectField(TEXT("parent_class"),
			OliveBlueprintSchemas::StringProp(TEXT("Only list classes inheriting from this class (kind='project')")));

		// kind = blueprint_callable | overridable
		Props->SetObjectField(TEXT("class_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Class to list functions for (kind='blueprint_callable'|'overridable')")));
		Props->SetObjectField(TEXT("include_inherited"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include functions from parent classes (kind='blueprint_callable'; default true)"), true));

		Schema->SetStringField(TEXT("description"),
			TEXT("List C++ entities. Dispatches on 'kind' (project|blueprint_callable|overridable). "
				"Legacy cpp.list_project_classes / cpp.list_blueprint_callable / cpp.list_overridable are aliases that pre-fill 'kind'."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("kind") });
		return Schema;
	}

	TSharedPtr<FJsonObject> CppAdd()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("entity"),
			OliveBlueprintSchemas::EnumProp(
				TEXT("What to add. 'function' adds a UFUNCTION declaration and stub body; 'property' adds a UPROPERTY to a header."),
				{ TEXT("function"), TEXT("property") }));

		// Shared
		Props->SetObjectField(TEXT("file_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Path to .h file")));

		// entity = property
		Props->SetObjectField(TEXT("property_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Property name (entity='property')")));
		Props->SetObjectField(TEXT("property_type"),
			OliveBlueprintSchemas::StringProp(TEXT("C++ type (entity='property'; e.g., float, FVector, UStaticMesh*)")));
		Props->SetObjectField(TEXT("category"),
			OliveBlueprintSchemas::StringProp(TEXT("UPROPERTY Category (entity='property')")));
		Props->SetObjectField(TEXT("default_value"),
			OliveBlueprintSchemas::StringProp(TEXT("Default value expression (entity='property')")));

		// entity = function
		Props->SetObjectField(TEXT("function_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Function name (entity='function')")));
		Props->SetObjectField(TEXT("return_type"),
			OliveBlueprintSchemas::StringProp(TEXT("Return type (entity='function'; empty/void for void)")));

		auto ParamItemProps = MakeProperties();
		ParamItemProps->SetObjectField(TEXT("name"), OliveBlueprintSchemas::StringProp(TEXT("Parameter name")));
		ParamItemProps->SetObjectField(TEXT("type"), OliveBlueprintSchemas::StringProp(TEXT("C++ type")));
		auto ParamItemSchema = MakeSchema(TEXT("object"));
		ParamItemSchema->SetObjectField(TEXT("properties"), ParamItemProps);
		Props->SetObjectField(TEXT("parameters"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Function parameters (entity='function')"), ParamItemSchema));

		Props->SetObjectField(TEXT("is_virtual"),
			OliveBlueprintSchemas::BoolProp(TEXT("Whether function is virtual (entity='function')"), false));
		Props->SetObjectField(TEXT("body"),
			OliveBlueprintSchemas::StringProp(TEXT("Optional function body for .cpp (entity='function')")));

		// Shared
		Props->SetObjectField(TEXT("specifiers"),
			OliveBlueprintSchemas::ArrayProp(
				TEXT("UPROPERTY/UFUNCTION specifiers (e.g., EditAnywhere, BlueprintCallable, Category=AI)"),
				OliveBlueprintSchemas::StringProp(TEXT("Specifier"))));

		Schema->SetStringField(TEXT("description"),
			TEXT("Add a UFUNCTION or UPROPERTY to a header. Dispatches on 'entity' (function|property). "
				"Legacy cpp.add_function and cpp.add_property are aliases that pre-fill 'entity'."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("entity"), TEXT("file_path") });
		return Schema;
	}
}
