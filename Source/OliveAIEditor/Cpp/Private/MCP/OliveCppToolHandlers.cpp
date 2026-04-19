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

	// P5 consolidation: C++ exposes 6 real tools. Legacy names
	// (cpp.read_class, cpp.read_enum, cpp.read_struct, cpp.read_header,
	// cpp.read_source, cpp.list_project_classes, cpp.list_blueprint_callable,
	// cpp.list_overridable, cpp.add_function, cpp.add_property) continue to
	// work as aliases registered in OliveToolRegistry::GetToolAliases().

	// 1. cpp.read (P5: replaces read_class/read_enum/read_struct/read_header/read_source)
	Registry.RegisterTool(
		TEXT("cpp.read"),
		TEXT("Read C++ entities via reflection or source files. Dispatches on 'entity' "
			"(class|enum|struct|header|source). Legacy read_class/read_enum/read_struct/"
			"read_header/read_source are aliases."),
		OliveCppSchemas::CppRead(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleCppRead),
		{TEXT("cpp"), TEXT("read")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.read"));
}

void FOliveCppToolHandlers::RegisterSourceTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// 2. cpp.list (P5: replaces list_project_classes/list_blueprint_callable/list_overridable)
	Registry.RegisterTool(
		TEXT("cpp.list"),
		TEXT("List C++ entities. Dispatches on 'kind' (project|blueprint_callable|overridable). "
			"Legacy list_project_classes/list_blueprint_callable/list_overridable are aliases."),
		OliveCppSchemas::CppList(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleCppList),
		{TEXT("cpp"), TEXT("read")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.list"));
}

void FOliveCppToolHandlers::RegisterWriteTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// 3. cpp.create_class (unchanged)
	Registry.RegisterTool(
		TEXT("cpp.create_class"),
		TEXT("Create a new UE C++ class with header and source boilerplate"),
		OliveCppSchemas::CppCreateClass(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleCreateClass),
		{TEXT("cpp"), TEXT("write")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.create_class"));

	// 4. cpp.add (P5: replaces add_function + add_property)
	Registry.RegisterTool(
		TEXT("cpp.add"),
		TEXT("Add a UFUNCTION or UPROPERTY to a header. Dispatches on 'entity' (function|property). "
			"Legacy cpp.add_function and cpp.add_property are aliases that pre-fill 'entity'."),
		OliveCppSchemas::CppAdd(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleCppAdd),
		{TEXT("cpp"), TEXT("write"), TEXT("add")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.add"));

	// 5. cpp.modify_source (unchanged)
	Registry.RegisterTool(
		TEXT("cpp.modify_source"),
		TEXT("Apply a bounded anchor-based source patch"),
		OliveCppSchemas::CppModifySource(),
		FOliveToolHandler::CreateRaw(this, &FOliveCppToolHandlers::HandleModifySource),
		{TEXT("cpp"), TEXT("write")},
		TEXT("cpp")
	);
	RegisteredToolNames.Add(TEXT("cpp.modify_source"));

	// 6. cpp.compile (unchanged)
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
	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'class_name' is missing"),
			TEXT("Provide the UE class name. Example: \"ACharacter\", \"UActorComponent\""));
	}

	// Determine read mode: "all" (default), "callable", or "overridable"
	FString IncludeMode = TEXT("all");
	Params->TryGetStringField(TEXT("include"), IncludeMode);

	bool bIncludeInherited = false;
	Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

	// --- Filtered mode: callable ---
	if (IncludeMode.Equals(TEXT("callable"), ESearchCase::IgnoreCase))
	{
		// Default include_inherited to true for callable mode (matches old list_blueprint_callable behavior)
		if (!Params->HasField(TEXT("include_inherited")))
		{
			bIncludeInherited = true;
		}

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

	// --- Filtered mode: overridable ---
	if (IncludeMode.Equals(TEXT("overridable"), ESearchCase::IgnoreCase))
	{
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

	// --- Default mode: all (full reflection dump) ---
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
	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'class_name' is missing"),
			TEXT("Provide the UE class name. Example: \"ACharacter\", \"APawn\""));
	}
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
	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'class_name' is missing"),
			TEXT("Provide the UE class name. Example: \"ACharacter\", \"AActor\""));
	}

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
	FString EnumName;
	if (!Params->TryGetStringField(TEXT("enum_name"), EnumName) || EnumName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'enum_name' is missing"),
			TEXT("Provide the enum name. Example: \"ECollisionChannel\", \"EMovementMode\""));
	}

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
	FString StructName;
	if (!Params->TryGetStringField(TEXT("struct_name"), StructName) || StructName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'struct_name' is missing"),
			TEXT("Provide the struct name. Example: \"FVector\", \"FHitResult\""));
	}
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
	FString FilePath;
	if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'file_path' is missing"),
			TEXT("Provide the header file path relative to the project Source/ directory. Example: \"MyProject/Public/MyActor.h\""));
	}
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
	FString FilePath;
	if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'file_path' is missing"),
			TEXT("Provide the source file path relative to the project Source/ directory. Example: \"MyProject/Private/MyActor.cpp\""));
	}
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
	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'class_name' is missing"),
			TEXT("Provide the new class name without prefix. Example: \"MyCharacter\""));
	}
	FString ParentClass;
	if (!Params->TryGetStringField(TEXT("parent_class"), ParentClass) || ParentClass.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'parent_class' is missing"),
			TEXT("Provide the parent class name. Example: \"ACharacter\", \"AActor\", \"UActorComponent\""));
	}
	FString ModuleName;
	if (!Params->TryGetStringField(TEXT("module_name"), ModuleName) || ModuleName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'module_name' is missing"),
			TEXT("Provide the target module name. Use cpp.list_project_classes to see available modules."));
	}

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
	FString FilePath;
	if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'file_path' is missing"),
			TEXT("Provide the header file path. Example: \"MyProject/Public/MyActor.h\""));
	}
	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'property_name' is missing"),
			TEXT("Provide the property name. Example: \"MaxHealth\", \"MoveSpeed\""));
	}
	FString PropertyType;
	if (!Params->TryGetStringField(TEXT("property_type"), PropertyType) || PropertyType.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'property_type' is missing"),
			TEXT("Provide the C++ type. Example: \"float\", \"int32\", \"FVector\", \"TArray<FString>\""));
	}

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
	FString FilePath;
	if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'file_path' is missing"),
			TEXT("Provide the header file path. Example: \"MyProject/Public/MyActor.h\""));
	}
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'function_name' is missing"),
			TEXT("Provide the function name. Example: \"TakeDamage\", \"GetHealth\""));
	}

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
	FString FilePath;
	if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'file_path' is missing"),
			TEXT("Provide the source or header file path. Example: \"MyProject/Private/MyActor.cpp\""));
	}
	FString AnchorText;
	if (!Params->TryGetStringField(TEXT("anchor_text"), AnchorText) || AnchorText.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'anchor_text' is missing"),
			TEXT("Provide the text to search for as the anchor point for the edit."));
	}
	FString Operation;
	if (!Params->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'operation' is missing"),
			TEXT("Provide the operation: \"replace\", \"insert_before\", \"insert_after\", or \"delete\""));
	}

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

	bool bAllowLargeEdit = false;
	Params->TryGetBoolField(TEXT("allow_large_edit"), bAllowLargeEdit);

	// Check replacement size if not explicitly allowed
	if (!bAllowLargeEdit && !ReplacementText.IsEmpty())
	{
		int32 LineCount = 1;
		for (const TCHAR& Ch : ReplacementText)
		{
			if (Ch == TEXT('\n'))
			{
				LineCount++;
			}
		}
		if (LineCount > 200)
		{
			return FOliveToolResult::Error(TEXT("EDIT_TOO_LARGE"),
				FString::Printf(TEXT("Replacement text is %d lines — maximum is 200 without allow_large_edit=true"), LineCount),
				TEXT("Break the edit into smaller chunks, or pass allow_large_edit=true if intentional"));
		}
	}

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

// ============================================================================
// Consolidated Dispatchers (P5)
//
// These dispatchers route on entity / kind to the existing specialized
// handlers. Legacy tool names (cpp.read_class, cpp.read_enum, cpp.read_struct,
// cpp.read_header, cpp.read_source, cpp.list_project_classes,
// cpp.list_blueprint_callable, cpp.list_overridable, cpp.add_function,
// cpp.add_property) are preserved as aliases that pre-fill the dispatch field
// in OliveToolRegistry::GetToolAliases().
// ============================================================================

namespace
{
	/** Clone params so we can normalize fields without mutating the caller. */
	static TSharedPtr<FJsonObject> CloneCppParams(const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Params.IsValid())
		{
			for (const auto& Pair : Params->Values) { Out->Values.Add(Pair.Key, Pair.Value); }
		}
		return Out;
	}
} // anonymous namespace

FOliveToolResult FOliveCppToolHandlers::HandleCppRead(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a params object with an 'entity' field."));
	}

	FString Entity;
	Params->TryGetStringField(TEXT("entity"), Entity);
	Entity = Entity.ToLower();
	if (Entity.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'entity'"),
			TEXT("entity must be one of: class, enum, struct, header, source"));
	}

	TSharedPtr<FJsonObject> SubParams = CloneCppParams(Params);

	if (Entity == TEXT("class"))
	{
		return HandleReadClass(SubParams);
	}
	if (Entity == TEXT("enum"))
	{
		return HandleReadEnum(SubParams);
	}
	if (Entity == TEXT("struct"))
	{
		return HandleReadStruct(SubParams);
	}
	if (Entity == TEXT("header"))
	{
		return HandleReadHeader(SubParams);
	}
	if (Entity == TEXT("source"))
	{
		return HandleReadSource(SubParams);
	}

	return FOliveToolResult::Error(
		TEXT("VALIDATION_INVALID_VALUE"),
		FString::Printf(TEXT("Unknown entity '%s'"), *Entity),
		TEXT("entity must be one of: class, enum, struct, header, source"));
}

FOliveToolResult FOliveCppToolHandlers::HandleCppList(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a params object with a 'kind' field."));
	}

	FString Kind;
	Params->TryGetStringField(TEXT("kind"), Kind);
	Kind = Kind.ToLower();
	if (Kind.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'kind'"),
			TEXT("kind must be one of: project, blueprint_callable, overridable"));
	}

	TSharedPtr<FJsonObject> SubParams = CloneCppParams(Params);

	if (Kind == TEXT("project"))
	{
		return HandleListProjectClasses(SubParams);
	}
	if (Kind == TEXT("blueprint_callable"))
	{
		return HandleListBlueprintCallable(SubParams);
	}
	if (Kind == TEXT("overridable"))
	{
		return HandleListOverridable(SubParams);
	}

	return FOliveToolResult::Error(
		TEXT("VALIDATION_INVALID_VALUE"),
		FString::Printf(TEXT("Unknown kind '%s'"), *Kind),
		TEXT("kind must be one of: project, blueprint_callable, overridable"));
}

FOliveToolResult FOliveCppToolHandlers::HandleCppAdd(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a params object with 'entity' and 'file_path' fields."));
	}

	FString Entity;
	Params->TryGetStringField(TEXT("entity"), Entity);
	Entity = Entity.ToLower();
	if (Entity.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'entity'"),
			TEXT("entity must be one of: function, property"));
	}

	TSharedPtr<FJsonObject> SubParams = CloneCppParams(Params);

	if (Entity == TEXT("function"))
	{
		return HandleAddFunction(SubParams);
	}
	if (Entity == TEXT("property"))
	{
		return HandleAddProperty(SubParams);
	}

	return FOliveToolResult::Error(
		TEXT("VALIDATION_INVALID_VALUE"),
		FString::Printf(TEXT("Unknown entity '%s'"), *Entity),
		TEXT("entity must be one of: function, property"));
}
