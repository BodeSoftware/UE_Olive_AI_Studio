// Copyright Bode Software. All Rights Reserved.

#include "OliveCppToolHandlers.h"
#include "OliveCppSchemas.h"
#include "OliveCppReflectionReader.h"
#include "OliveCppSourceReader.h"
#include "OliveCppSourceWriter.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOliveCppTools);

FOliveCppToolHandlers& FOliveCppToolHandlers::Get()
{
	static FOliveCppToolHandlers Instance;
	return Instance;
}

void FOliveCppToolHandlers::RegisterAllTools()
{
	UE_LOG(LogOliveCppTools, Log, TEXT("Registering C++ MCP tools..."));

	RegisterReflectionTools();
	RegisterSourceTools();
	RegisterWriteTools();

	UE_LOG(LogOliveCppTools, Log, TEXT("Registered %d C++ MCP tools"), RegisteredToolNames.Num());
}

void FOliveCppToolHandlers::UnregisterAllTools()
{
	UE_LOG(LogOliveCppTools, Log, TEXT("Unregistering C++ MCP tools..."));

	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
	for (const FString& ToolName : RegisteredToolNames)
	{
		Registry.UnregisterTool(ToolName);
	}

	RegisteredToolNames.Empty();
	UE_LOG(LogOliveCppTools, Log, TEXT("C++ MCP tools unregistered"));
}

// ============================================================================
// Registration
// ============================================================================

void FOliveCppToolHandlers::RegisterReflectionTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("cpp.read_class"),
		TEXT("Read full reflection data for a C++ class (properties, functions, interfaces, metadata)"),
		OliveCppSchemas::CppReadClass(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleReadClass),
		{TEXT("cpp"), TEXT("read")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.read_class"));

	Registry.RegisterTool(
		TEXT("cpp.list_blueprint_callable"),
		TEXT("List all BlueprintCallable and BlueprintPure functions on a class"),
		OliveCppSchemas::CppListBlueprintCallable(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleListBlueprintCallable),
		{TEXT("cpp"), TEXT("read")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.list_blueprint_callable"));

	Registry.RegisterTool(
		TEXT("cpp.list_overridable"),
		TEXT("List overridable functions (BlueprintImplementableEvent, BlueprintNativeEvent)"),
		OliveCppSchemas::CppListOverridable(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleListOverridable),
		{TEXT("cpp"), TEXT("read")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.list_overridable"));

	Registry.RegisterTool(
		TEXT("cpp.read_enum"),
		TEXT("Read enum values and metadata via reflection"),
		OliveCppSchemas::CppReadEnum(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleReadEnum),
		{TEXT("cpp"), TEXT("read")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.read_enum"));

	Registry.RegisterTool(
		TEXT("cpp.read_struct"),
		TEXT("Read struct members via reflection"),
		OliveCppSchemas::CppReadStruct(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleReadStruct),
		{TEXT("cpp"), TEXT("read")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.read_struct"));
}

void FOliveCppToolHandlers::RegisterSourceTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("cpp.read_header"),
		TEXT("Read a project .h file with optional line range"),
		OliveCppSchemas::CppReadHeader(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleReadHeader),
		{TEXT("cpp"), TEXT("read")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.read_header"));

	Registry.RegisterTool(
		TEXT("cpp.read_source"),
		TEXT("Read a project .cpp file with optional line range"),
		OliveCppSchemas::CppReadSource(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleReadSource),
		{TEXT("cpp"), TEXT("read")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.read_source"));

	Registry.RegisterTool(
		TEXT("cpp.list_project_classes"),
		TEXT("List C++ classes defined in project Source/ directory"),
		OliveCppSchemas::CppListProjectClasses(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleListProjectClasses),
		{TEXT("cpp"), TEXT("read")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.list_project_classes"));
}

void FOliveCppToolHandlers::RegisterWriteTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("cpp.create_class"),
		TEXT("Create a new UE C++ class with header and source boilerplate"),
		OliveCppSchemas::CppCreateClass(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleCreateClass),
		{TEXT("cpp"), TEXT("write")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.create_class"));

	Registry.RegisterTool(
		TEXT("cpp.add_property"),
		TEXT("Add a UPROPERTY to an existing header file"),
		OliveCppSchemas::CppAddProperty(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleAddProperty),
		{TEXT("cpp"), TEXT("write")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.add_property"));

	Registry.RegisterTool(
		TEXT("cpp.add_function"),
		TEXT("Add a UFUNCTION declaration and stub body"),
		OliveCppSchemas::CppAddFunction(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleAddFunction),
		{TEXT("cpp"), TEXT("write")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.add_function"));

	Registry.RegisterTool(
		TEXT("cpp.modify_source"),
		TEXT("Apply a bounded anchor-based source patch"),
		OliveCppSchemas::CppModifySource(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleModifySource),
		{TEXT("cpp"), TEXT("write")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.modify_source"));

	Registry.RegisterTool(
		TEXT("cpp.compile"),
		TEXT("Trigger Live Coding hot reload compilation"),
		OliveCppSchemas::CppCompile(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleCompile),
		{TEXT("cpp"), TEXT("write")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.compile"));
}

// ============================================================================
// Reflection Handlers
// ============================================================================

FOliveToolResult FOliveCppToolHandlers::HandleReadClass(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName = Params->GetStringField(TEXT("class_name"));
	bool bIncludeInherited = false;
	Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
	bool bIncludeFunctions = true;
	Params->TryGetBoolField(TEXT("include_functions"), bIncludeFunctions);
	bool bIncludeProperties = true;
	Params->TryGetBoolField(TEXT("include_properties"), bIncludeProperties);

	TOptional<FOliveIRCppClass> ClassIR = FOliveCppReflectionReader::ReadClass(ClassName, bIncludeInherited, bIncludeFunctions, bIncludeProperties);

	if (!ClassIR.IsSet())
	{
		return FOliveToolResult::Error(TEXT("CLASS_NOT_FOUND"),
			FString::Printf(TEXT("Class '%s' not found"), *ClassName),
			TEXT("Check the class name. Try with or without A/U prefix."));
	}

	return FOliveToolResult::Success(ClassIR.GetValue().ToJson());
}

FOliveToolResult FOliveCppToolHandlers::HandleListBlueprintCallable(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName = Params->GetStringField(TEXT("class_name"));
	bool bIncludeInherited = true;
	Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

	UClass* Class = FOliveCppReflectionReader::FindClassByName(ClassName);
	if (!Class)
	{
		return FOliveToolResult::Error(TEXT("CLASS_NOT_FOUND"),
			FString::Printf(TEXT("Class '%s' not found"), *ClassName),
			TEXT("Check the class name. Try with or without A/U prefix."));
	}

	TArray<FOliveIRCppFunction> Functions = FOliveCppReflectionReader::ListBlueprintCallable(ClassName, bIncludeInherited);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("class_name"), Class->GetName());
	TArray<TSharedPtr<FJsonValue>> FuncsArray;
	for (const FOliveIRCppFunction& Func : Functions)
	{
		FuncsArray.Add(MakeShared<FJsonValueObject>(Func.ToJson()));
	}
	ResultData->SetArrayField(TEXT("functions"), FuncsArray);
	ResultData->SetNumberField(TEXT("count"), Functions.Num());

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveCppToolHandlers::HandleListOverridable(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName = Params->GetStringField(TEXT("class_name"));

	UClass* Class = FOliveCppReflectionReader::FindClassByName(ClassName);
	if (!Class)
	{
		return FOliveToolResult::Error(TEXT("CLASS_NOT_FOUND"),
			FString::Printf(TEXT("Class '%s' not found"), *ClassName),
			TEXT("Check the class name. Try with or without A/U prefix."));
	}

	TArray<FOliveIRCppFunction> Functions = FOliveCppReflectionReader::ListOverridable(ClassName);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("class_name"), Class->GetName());
	TArray<TSharedPtr<FJsonValue>> FuncsArray;
	for (const FOliveIRCppFunction& Func : Functions)
	{
		FuncsArray.Add(MakeShared<FJsonValueObject>(Func.ToJson()));
	}
	ResultData->SetArrayField(TEXT("functions"), FuncsArray);
	ResultData->SetNumberField(TEXT("count"), Functions.Num());

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveCppToolHandlers::HandleReadEnum(const TSharedPtr<FJsonObject>& Params)
{
	FString EnumName = Params->GetStringField(TEXT("enum_name"));

	TOptional<FOliveIRCppEnum> EnumIR = FOliveCppReflectionReader::ReadEnum(EnumName);

	if (!EnumIR.IsSet())
	{
		return FOliveToolResult::Error(TEXT("ENUM_NOT_FOUND"),
			FString::Printf(TEXT("Enum '%s' not found"), *EnumName),
			TEXT("Check the enum name. Use the full name including namespace if applicable."));
	}

	return FOliveToolResult::Success(EnumIR.GetValue().ToJson());
}

FOliveToolResult FOliveCppToolHandlers::HandleReadStruct(const TSharedPtr<FJsonObject>& Params)
{
	FString StructName = Params->GetStringField(TEXT("struct_name"));
	bool bIncludeInherited = false;
	Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

	TOptional<FOliveIRCppStruct> StructIR = FOliveCppReflectionReader::ReadStruct(StructName, bIncludeInherited);

	if (!StructIR.IsSet())
	{
		return FOliveToolResult::Error(TEXT("STRUCT_NOT_FOUND"),
			FString::Printf(TEXT("Struct '%s' not found"), *StructName),
			TEXT("Check the struct name. Use the full name including F prefix if applicable."));
	}

	return FOliveToolResult::Success(StructIR.GetValue().ToJson());
}

// ============================================================================
// Source Handlers
// ============================================================================

FOliveToolResult FOliveCppToolHandlers::HandleReadHeader(const TSharedPtr<FJsonObject>& Params)
{
	FString FilePath = Params->GetStringField(TEXT("file_path"));
	int32 StartLine = 0;
	int32 EndLine = 0;
	if (Params->HasField(TEXT("start_line")))
	{
		StartLine = (int32)Params->GetNumberField(TEXT("start_line"));
	}
	if (Params->HasField(TEXT("end_line")))
	{
		EndLine = (int32)Params->GetNumberField(TEXT("end_line"));
	}

	TOptional<FOliveIRCppSourceFile> Result = FOliveCppSourceReader::ReadHeader(FilePath, StartLine, EndLine);
	if (!Result.IsSet())
	{
		return FOliveToolResult::Error(TEXT("READ_FAILED"),
			FString::Printf(TEXT("Failed to read header: %s"), *FilePath),
			TEXT("Ensure the file exists in the project Source/ directory"));
	}

	return FOliveToolResult::Success(Result.GetValue().ToJson());
}

FOliveToolResult FOliveCppToolHandlers::HandleReadSource(const TSharedPtr<FJsonObject>& Params)
{
	FString FilePath = Params->GetStringField(TEXT("file_path"));
	int32 StartLine = 0;
	int32 EndLine = 0;
	if (Params->HasField(TEXT("start_line")))
	{
		StartLine = (int32)Params->GetNumberField(TEXT("start_line"));
	}
	if (Params->HasField(TEXT("end_line")))
	{
		EndLine = (int32)Params->GetNumberField(TEXT("end_line"));
	}

	TOptional<FOliveIRCppSourceFile> Result = FOliveCppSourceReader::ReadSource(FilePath, StartLine, EndLine);
	if (!Result.IsSet())
	{
		return FOliveToolResult::Error(TEXT("READ_FAILED"),
			FString::Printf(TEXT("Failed to read source: %s"), *FilePath),
			TEXT("Ensure the file exists in the project Source/ directory"));
	}

	return FOliveToolResult::Success(Result.GetValue().ToJson());
}

FOliveToolResult FOliveCppToolHandlers::HandleListProjectClasses(const TSharedPtr<FJsonObject>& Params)
{
	FString ModuleFilter;
	Params->TryGetStringField(TEXT("module_filter"), ModuleFilter);
	FString ParentClass;
	Params->TryGetStringField(TEXT("parent_class"), ParentClass);

	TArray<FOliveIRCppSourceFile> Classes = FOliveCppSourceReader::ListProjectClasses(ModuleFilter, ParentClass);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ClassesArray;
	for (const FOliveIRCppSourceFile& Entry : Classes)
	{
		TSharedPtr<FJsonObject> ClassObj = MakeShared<FJsonObject>();
		ClassObj->SetStringField(TEXT("class_name"), Entry.Content);
		ClassObj->SetStringField(TEXT("file_path"), Entry.FilePath);
		ClassObj->SetNumberField(TEXT("total_lines"), Entry.TotalLines);
		ClassesArray.Add(MakeShared<FJsonValueObject>(ClassObj));
	}
	ResultData->SetArrayField(TEXT("classes"), ClassesArray);
	ResultData->SetNumberField(TEXT("count"), Classes.Num());

	return FOliveToolResult::Success(ResultData);
}

// ============================================================================
// Write Handlers
// ============================================================================

FOliveToolResult FOliveCppToolHandlers::HandleCreateClass(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName = Params->GetStringField(TEXT("class_name"));
	FString ParentClass = Params->GetStringField(TEXT("parent_class"));
	FString ModuleName = Params->GetStringField(TEXT("module_name"));

	FString SubPath;
	Params->TryGetStringField(TEXT("path"), SubPath);

	TArray<FString> Interfaces;
	const TArray<TSharedPtr<FJsonValue>>* InterfacesArray;
	if (Params->TryGetArrayField(TEXT("interfaces"), InterfacesArray))
	{
		for (const auto& Value : *InterfacesArray)
		{
			Interfaces.Add(Value->AsString());
		}
	}

	return FOliveCppSourceWriter::CreateClass(ClassName, ParentClass, ModuleName, SubPath, Interfaces);
}

FOliveToolResult FOliveCppToolHandlers::HandleAddProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString FilePath = Params->GetStringField(TEXT("file_path"));
	FString PropertyName = Params->GetStringField(TEXT("property_name"));
	FString PropertyType = Params->GetStringField(TEXT("property_type"));

	FString Category;
	Params->TryGetStringField(TEXT("category"), Category);

	FString DefaultValue;
	Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	TArray<FString> Specifiers;
	const TArray<TSharedPtr<FJsonValue>>* SpecArray;
	if (Params->TryGetArrayField(TEXT("specifiers"), SpecArray))
	{
		for (const auto& Value : *SpecArray)
		{
			Specifiers.Add(Value->AsString());
		}
	}

	return FOliveCppSourceWriter::AddProperty(FilePath, PropertyName, PropertyType, Category, Specifiers, DefaultValue);
}

FOliveToolResult FOliveCppToolHandlers::HandleAddFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString FilePath = Params->GetStringField(TEXT("file_path"));
	FString FunctionName = Params->GetStringField(TEXT("function_name"));

	FString ReturnType;
	Params->TryGetStringField(TEXT("return_type"), ReturnType);

	TArray<TPair<FString, FString>> Parameters;
	const TArray<TSharedPtr<FJsonValue>>* ParamsArray;
	if (Params->TryGetArrayField(TEXT("parameters"), ParamsArray))
	{
		for (const auto& Value : *ParamsArray)
		{
			TSharedPtr<FJsonObject> ParamObj = Value->AsObject();
			if (ParamObj.IsValid())
			{
				FString Name = ParamObj->GetStringField(TEXT("name"));
				FString Type = ParamObj->GetStringField(TEXT("type"));
				Parameters.Add(TPair<FString, FString>(Name, Type));
			}
		}
	}

	TArray<FString> Specifiers;
	const TArray<TSharedPtr<FJsonValue>>* SpecArray;
	if (Params->TryGetArrayField(TEXT("specifiers"), SpecArray))
	{
		for (const auto& Value : *SpecArray)
		{
			Specifiers.Add(Value->AsString());
		}
	}

	bool bIsVirtual = false;
	Params->TryGetBoolField(TEXT("is_virtual"), bIsVirtual);

	FString Body;
	Params->TryGetStringField(TEXT("body"), Body);

	return FOliveCppSourceWriter::AddFunction(FilePath, FunctionName, ReturnType, Parameters, Specifiers, bIsVirtual, Body);
}

FOliveToolResult FOliveCppToolHandlers::HandleCompile(const TSharedPtr<FJsonObject>& Params)
{
	return FOliveCppSourceWriter::TriggerCompile();
}

FOliveToolResult FOliveCppToolHandlers::HandleModifySource(const TSharedPtr<FJsonObject>& Params)
{
	FString FilePath = Params->GetStringField(TEXT("file_path"));
	FString AnchorText = Params->GetStringField(TEXT("anchor_text"));
	FString Operation = Params->GetStringField(TEXT("operation"));

	FString ReplacementText;
	Params->TryGetStringField(TEXT("replacement_text"), ReplacementText);

	int32 Occurrence = 1;
	if (Params->HasField(TEXT("occurrence")))
	{
		Occurrence = (int32)Params->GetNumberField(TEXT("occurrence"));
	}

	int32 StartLine = 0;
	if (Params->HasField(TEXT("start_line")))
	{
		StartLine = (int32)Params->GetNumberField(TEXT("start_line"));
	}

	int32 EndLine = 0;
	if (Params->HasField(TEXT("end_line")))
	{
		EndLine = (int32)Params->GetNumberField(TEXT("end_line"));
	}

	bool bRequireUniqueMatch = true;
	Params->TryGetBoolField(TEXT("require_unique_match"), bRequireUniqueMatch);

	return FOliveCppSourceWriter::ModifySource(
		FilePath,
		AnchorText,
		Operation,
		ReplacementText,
		Occurrence,
		StartLine,
		EndLine,
		bRequireUniqueMatch);
}
