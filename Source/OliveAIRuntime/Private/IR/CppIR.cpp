// Copyright Bode Software. All Rights Reserved.

#include "IR/CppIR.h"
#include "Serialization/JsonSerializer.h"

// Helper: Serialize a TMap<FString, FString> to a JSON object (only if non-empty)
static void SerializeStringMap(const TMap<FString, FString>& Map, const FString& FieldName, TSharedPtr<FJsonObject>& Json)
{
	if (Map.Num() > 0)
	{
		TSharedPtr<FJsonObject> MapJson = MakeShared<FJsonObject>();
		for (const auto& Pair : Map)
		{
			MapJson->SetStringField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(FieldName, MapJson);
	}
}

// Helper: Deserialize a JSON object to a TMap<FString, FString>
static void DeserializeStringMap(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TMap<FString, FString>& OutMap)
{
	const TSharedPtr<FJsonObject>* MapJson;
	if (JsonObject->TryGetObjectField(FieldName, MapJson))
	{
		for (const auto& Pair : (*MapJson)->Values)
		{
			OutMap.Add(Pair.Key, Pair.Value->AsString());
		}
	}
}

// FOliveIRCppPropertyFlags

TSharedPtr<FJsonObject> FOliveIRCppPropertyFlags::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	if (bBlueprintReadOnly) { Json->SetBoolField(TEXT("blueprint_read_only"), true); }
	if (bBlueprintReadWrite) { Json->SetBoolField(TEXT("blueprint_read_write"), true); }
	if (bEditAnywhere) { Json->SetBoolField(TEXT("edit_anywhere"), true); }
	if (bEditDefaultsOnly) { Json->SetBoolField(TEXT("edit_defaults_only"), true); }
	if (bEditInstanceOnly) { Json->SetBoolField(TEXT("edit_instance_only"), true); }
	if (bVisibleAnywhere) { Json->SetBoolField(TEXT("visible_anywhere"), true); }
	if (bConfig) { Json->SetBoolField(TEXT("config"), true); }
	if (bTransient) { Json->SetBoolField(TEXT("transient"), true); }
	if (bReplicated) { Json->SetBoolField(TEXT("replicated"), true); }
	if (bExposeOnSpawn) { Json->SetBoolField(TEXT("expose_on_spawn"), true); }
	if (bSaveGame) { Json->SetBoolField(TEXT("save_game"), true); }

	return Json;
}

FOliveIRCppPropertyFlags FOliveIRCppPropertyFlags::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRCppPropertyFlags Flags;
	if (!JsonObject.IsValid())
	{
		return Flags;
	}

	if (JsonObject->HasField(TEXT("blueprint_read_only"))) { Flags.bBlueprintReadOnly = JsonObject->GetBoolField(TEXT("blueprint_read_only")); }
	if (JsonObject->HasField(TEXT("blueprint_read_write"))) { Flags.bBlueprintReadWrite = JsonObject->GetBoolField(TEXT("blueprint_read_write")); }
	if (JsonObject->HasField(TEXT("edit_anywhere"))) { Flags.bEditAnywhere = JsonObject->GetBoolField(TEXT("edit_anywhere")); }
	if (JsonObject->HasField(TEXT("edit_defaults_only"))) { Flags.bEditDefaultsOnly = JsonObject->GetBoolField(TEXT("edit_defaults_only")); }
	if (JsonObject->HasField(TEXT("edit_instance_only"))) { Flags.bEditInstanceOnly = JsonObject->GetBoolField(TEXT("edit_instance_only")); }
	if (JsonObject->HasField(TEXT("visible_anywhere"))) { Flags.bVisibleAnywhere = JsonObject->GetBoolField(TEXT("visible_anywhere")); }
	if (JsonObject->HasField(TEXT("config"))) { Flags.bConfig = JsonObject->GetBoolField(TEXT("config")); }
	if (JsonObject->HasField(TEXT("transient"))) { Flags.bTransient = JsonObject->GetBoolField(TEXT("transient")); }
	if (JsonObject->HasField(TEXT("replicated"))) { Flags.bReplicated = JsonObject->GetBoolField(TEXT("replicated")); }
	if (JsonObject->HasField(TEXT("expose_on_spawn"))) { Flags.bExposeOnSpawn = JsonObject->GetBoolField(TEXT("expose_on_spawn")); }
	if (JsonObject->HasField(TEXT("save_game"))) { Flags.bSaveGame = JsonObject->GetBoolField(TEXT("save_game")); }

	return Flags;
}

// FOliveIRCppFunctionFlags

TSharedPtr<FJsonObject> FOliveIRCppFunctionFlags::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	if (bBlueprintCallable) { Json->SetBoolField(TEXT("blueprint_callable"), true); }
	if (bBlueprintPure) { Json->SetBoolField(TEXT("blueprint_pure"), true); }
	if (bBlueprintImplementableEvent) { Json->SetBoolField(TEXT("blueprint_implementable_event"), true); }
	if (bBlueprintNativeEvent) { Json->SetBoolField(TEXT("blueprint_native_event"), true); }
	if (bCallInEditor) { Json->SetBoolField(TEXT("call_in_editor"), true); }
	if (bServer) { Json->SetBoolField(TEXT("server"), true); }
	if (bClient) { Json->SetBoolField(TEXT("client"), true); }
	if (bNetMulticast) { Json->SetBoolField(TEXT("net_multicast"), true); }
	if (bReliable) { Json->SetBoolField(TEXT("reliable"), true); }
	if (bExec) { Json->SetBoolField(TEXT("exec"), true); }
	if (bConst) { Json->SetBoolField(TEXT("const"), true); }
	if (bStatic) { Json->SetBoolField(TEXT("static"), true); }
	if (bVirtual) { Json->SetBoolField(TEXT("virtual"), true); }

	return Json;
}

FOliveIRCppFunctionFlags FOliveIRCppFunctionFlags::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRCppFunctionFlags Flags;
	if (!JsonObject.IsValid())
	{
		return Flags;
	}

	if (JsonObject->HasField(TEXT("blueprint_callable"))) { Flags.bBlueprintCallable = JsonObject->GetBoolField(TEXT("blueprint_callable")); }
	if (JsonObject->HasField(TEXT("blueprint_pure"))) { Flags.bBlueprintPure = JsonObject->GetBoolField(TEXT("blueprint_pure")); }
	if (JsonObject->HasField(TEXT("blueprint_implementable_event"))) { Flags.bBlueprintImplementableEvent = JsonObject->GetBoolField(TEXT("blueprint_implementable_event")); }
	if (JsonObject->HasField(TEXT("blueprint_native_event"))) { Flags.bBlueprintNativeEvent = JsonObject->GetBoolField(TEXT("blueprint_native_event")); }
	if (JsonObject->HasField(TEXT("call_in_editor"))) { Flags.bCallInEditor = JsonObject->GetBoolField(TEXT("call_in_editor")); }
	if (JsonObject->HasField(TEXT("server"))) { Flags.bServer = JsonObject->GetBoolField(TEXT("server")); }
	if (JsonObject->HasField(TEXT("client"))) { Flags.bClient = JsonObject->GetBoolField(TEXT("client")); }
	if (JsonObject->HasField(TEXT("net_multicast"))) { Flags.bNetMulticast = JsonObject->GetBoolField(TEXT("net_multicast")); }
	if (JsonObject->HasField(TEXT("reliable"))) { Flags.bReliable = JsonObject->GetBoolField(TEXT("reliable")); }
	if (JsonObject->HasField(TEXT("exec"))) { Flags.bExec = JsonObject->GetBoolField(TEXT("exec")); }
	if (JsonObject->HasField(TEXT("const"))) { Flags.bConst = JsonObject->GetBoolField(TEXT("const")); }
	if (JsonObject->HasField(TEXT("static"))) { Flags.bStatic = JsonObject->GetBoolField(TEXT("static")); }
	if (JsonObject->HasField(TEXT("virtual"))) { Flags.bVirtual = JsonObject->GetBoolField(TEXT("virtual")); }

	return Flags;
}

// FOliveIRCppProperty

TSharedPtr<FJsonObject> FOliveIRCppProperty::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("type_name"), TypeName);

	if (!Category.IsEmpty())
	{
		Json->SetStringField(TEXT("category"), Category);
	}
	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}
	if (!DefaultValue.IsEmpty())
	{
		Json->SetStringField(TEXT("default_value"), DefaultValue);
	}

	TSharedPtr<FJsonObject> FlagsJson = Flags.ToJson();
	if (FlagsJson->Values.Num() > 0)
	{
		Json->SetObjectField(TEXT("flags"), FlagsJson);
	}

	SerializeStringMap(Metadata, TEXT("metadata"), Json);

	return Json;
}

FOliveIRCppProperty FOliveIRCppProperty::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRCppProperty Property;
	if (!JsonObject.IsValid())
	{
		return Property;
	}

	Property.Name = JsonObject->GetStringField(TEXT("name"));
	Property.TypeName = JsonObject->GetStringField(TEXT("type_name"));
	JsonObject->TryGetStringField(TEXT("category"), Property.Category);
	JsonObject->TryGetStringField(TEXT("description"), Property.Description);
	JsonObject->TryGetStringField(TEXT("default_value"), Property.DefaultValue);

	const TSharedPtr<FJsonObject>* FlagsJson;
	if (JsonObject->TryGetObjectField(TEXT("flags"), FlagsJson))
	{
		Property.Flags = FOliveIRCppPropertyFlags::FromJson(*FlagsJson);
	}

	DeserializeStringMap(JsonObject, TEXT("metadata"), Property.Metadata);

	return Property;
}

// FOliveIRCppFunction

TSharedPtr<FJsonObject> FOliveIRCppFunction::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);

	if (!ReturnType.IsEmpty())
	{
		Json->SetStringField(TEXT("return_type"), ReturnType);
	}

	if (Parameters.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ParamsArray;
		for (const FOliveIRCppProperty& Param : Parameters)
		{
			ParamsArray.Add(MakeShared<FJsonValueObject>(Param.ToJson()));
		}
		Json->SetArrayField(TEXT("parameters"), ParamsArray);
	}

	if (!Category.IsEmpty())
	{
		Json->SetStringField(TEXT("category"), Category);
	}
	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}

	TSharedPtr<FJsonObject> FlagsJson = Flags.ToJson();
	if (FlagsJson->Values.Num() > 0)
	{
		Json->SetObjectField(TEXT("flags"), FlagsJson);
	}

	SerializeStringMap(Metadata, TEXT("metadata"), Json);

	return Json;
}

FOliveIRCppFunction FOliveIRCppFunction::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRCppFunction Function;
	if (!JsonObject.IsValid())
	{
		return Function;
	}

	Function.Name = JsonObject->GetStringField(TEXT("name"));
	JsonObject->TryGetStringField(TEXT("return_type"), Function.ReturnType);
	JsonObject->TryGetStringField(TEXT("category"), Function.Category);
	JsonObject->TryGetStringField(TEXT("description"), Function.Description);

	const TArray<TSharedPtr<FJsonValue>>* ParamsArray;
	if (JsonObject->TryGetArrayField(TEXT("parameters"), ParamsArray))
	{
		for (const auto& Value : *ParamsArray)
		{
			Function.Parameters.Add(FOliveIRCppProperty::FromJson(Value->AsObject()));
		}
	}

	const TSharedPtr<FJsonObject>* FlagsJson;
	if (JsonObject->TryGetObjectField(TEXT("flags"), FlagsJson))
	{
		Function.Flags = FOliveIRCppFunctionFlags::FromJson(*FlagsJson);
	}

	DeserializeStringMap(JsonObject, TEXT("metadata"), Function.Metadata);

	return Function;
}

// FOliveIRCppClass

TSharedPtr<FJsonObject> FOliveIRCppClass::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("class_name"), ClassName);

	if (!ParentClassName.IsEmpty())
	{
		Json->SetStringField(TEXT("parent_class"), ParentClassName);
	}
	if (!ModuleName.IsEmpty())
	{
		Json->SetStringField(TEXT("module"), ModuleName);
	}
	if (!HeaderPath.IsEmpty())
	{
		Json->SetStringField(TEXT("header_path"), HeaderPath);
	}

	if (Interfaces.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> InterfacesArray;
		for (const FString& Interface : Interfaces)
		{
			InterfacesArray.Add(MakeShared<FJsonValueString>(Interface));
		}
		Json->SetArrayField(TEXT("interfaces"), InterfacesArray);
	}

	if (Properties.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PropsArray;
		for (const FOliveIRCppProperty& Prop : Properties)
		{
			PropsArray.Add(MakeShared<FJsonValueObject>(Prop.ToJson()));
		}
		Json->SetArrayField(TEXT("properties"), PropsArray);
	}

	if (Functions.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FuncsArray;
		for (const FOliveIRCppFunction& Func : Functions)
		{
			FuncsArray.Add(MakeShared<FJsonValueObject>(Func.ToJson()));
		}
		Json->SetArrayField(TEXT("functions"), FuncsArray);
	}

	if (bIsAbstract) { Json->SetBoolField(TEXT("is_abstract"), true); }
	if (bIsBlueprintable) { Json->SetBoolField(TEXT("is_blueprintable"), true); }
	if (bIsBlueprintType) { Json->SetBoolField(TEXT("is_blueprint_type"), true); }
	if (bIsDeprecated) { Json->SetBoolField(TEXT("is_deprecated"), true); }

	SerializeStringMap(ClassMetadata, TEXT("class_metadata"), Json);

	return Json;
}

FOliveIRCppClass FOliveIRCppClass::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRCppClass Class;
	if (!JsonObject.IsValid())
	{
		return Class;
	}

	Class.ClassName = JsonObject->GetStringField(TEXT("class_name"));
	JsonObject->TryGetStringField(TEXT("parent_class"), Class.ParentClassName);
	JsonObject->TryGetStringField(TEXT("module"), Class.ModuleName);
	JsonObject->TryGetStringField(TEXT("header_path"), Class.HeaderPath);

	const TArray<TSharedPtr<FJsonValue>>* InterfacesArray;
	if (JsonObject->TryGetArrayField(TEXT("interfaces"), InterfacesArray))
	{
		for (const auto& Value : *InterfacesArray)
		{
			Class.Interfaces.Add(Value->AsString());
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* PropsArray;
	if (JsonObject->TryGetArrayField(TEXT("properties"), PropsArray))
	{
		for (const auto& Value : *PropsArray)
		{
			Class.Properties.Add(FOliveIRCppProperty::FromJson(Value->AsObject()));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* FuncsArray;
	if (JsonObject->TryGetArrayField(TEXT("functions"), FuncsArray))
	{
		for (const auto& Value : *FuncsArray)
		{
			Class.Functions.Add(FOliveIRCppFunction::FromJson(Value->AsObject()));
		}
	}

	if (JsonObject->HasField(TEXT("is_abstract"))) { Class.bIsAbstract = JsonObject->GetBoolField(TEXT("is_abstract")); }
	if (JsonObject->HasField(TEXT("is_blueprintable"))) { Class.bIsBlueprintable = JsonObject->GetBoolField(TEXT("is_blueprintable")); }
	if (JsonObject->HasField(TEXT("is_blueprint_type"))) { Class.bIsBlueprintType = JsonObject->GetBoolField(TEXT("is_blueprint_type")); }
	if (JsonObject->HasField(TEXT("is_deprecated"))) { Class.bIsDeprecated = JsonObject->GetBoolField(TEXT("is_deprecated")); }

	DeserializeStringMap(JsonObject, TEXT("class_metadata"), Class.ClassMetadata);

	return Class;
}

// FOliveIRCppEnum

TSharedPtr<FJsonObject> FOliveIRCppEnum::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("enum_name"), EnumName);

	if (!UnderlyingType.IsEmpty())
	{
		Json->SetStringField(TEXT("underlying_type"), UnderlyingType);
	}

	if (bIsBlueprintType) { Json->SetBoolField(TEXT("is_blueprint_type"), true); }
	if (bIsScoped) { Json->SetBoolField(TEXT("is_scoped"), true); }

	if (Values.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ValuesArray;
		for (const FString& Value : Values)
		{
			ValuesArray.Add(MakeShared<FJsonValueString>(Value));
		}
		Json->SetArrayField(TEXT("values"), ValuesArray);
	}

	SerializeStringMap(ValueDisplayNames, TEXT("value_display_names"), Json);
	SerializeStringMap(Metadata, TEXT("metadata"), Json);

	return Json;
}

FOliveIRCppEnum FOliveIRCppEnum::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRCppEnum Enum;
	if (!JsonObject.IsValid())
	{
		return Enum;
	}

	Enum.EnumName = JsonObject->GetStringField(TEXT("enum_name"));
	JsonObject->TryGetStringField(TEXT("underlying_type"), Enum.UnderlyingType);

	if (JsonObject->HasField(TEXT("is_blueprint_type"))) { Enum.bIsBlueprintType = JsonObject->GetBoolField(TEXT("is_blueprint_type")); }
	if (JsonObject->HasField(TEXT("is_scoped"))) { Enum.bIsScoped = JsonObject->GetBoolField(TEXT("is_scoped")); }

	const TArray<TSharedPtr<FJsonValue>>* ValuesArray;
	if (JsonObject->TryGetArrayField(TEXT("values"), ValuesArray))
	{
		for (const auto& Value : *ValuesArray)
		{
			Enum.Values.Add(Value->AsString());
		}
	}

	DeserializeStringMap(JsonObject, TEXT("value_display_names"), Enum.ValueDisplayNames);
	DeserializeStringMap(JsonObject, TEXT("metadata"), Enum.Metadata);

	return Enum;
}

// FOliveIRCppStruct

TSharedPtr<FJsonObject> FOliveIRCppStruct::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("struct_name"), StructName);

	if (!ParentStructName.IsEmpty())
	{
		Json->SetStringField(TEXT("parent_struct"), ParentStructName);
	}

	if (bIsBlueprintType) { Json->SetBoolField(TEXT("is_blueprint_type"), true); }

	if (Properties.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PropsArray;
		for (const FOliveIRCppProperty& Prop : Properties)
		{
			PropsArray.Add(MakeShared<FJsonValueObject>(Prop.ToJson()));
		}
		Json->SetArrayField(TEXT("properties"), PropsArray);
	}

	SerializeStringMap(Metadata, TEXT("metadata"), Json);

	return Json;
}

FOliveIRCppStruct FOliveIRCppStruct::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRCppStruct Struct;
	if (!JsonObject.IsValid())
	{
		return Struct;
	}

	Struct.StructName = JsonObject->GetStringField(TEXT("struct_name"));
	JsonObject->TryGetStringField(TEXT("parent_struct"), Struct.ParentStructName);

	if (JsonObject->HasField(TEXT("is_blueprint_type"))) { Struct.bIsBlueprintType = JsonObject->GetBoolField(TEXT("is_blueprint_type")); }

	const TArray<TSharedPtr<FJsonValue>>* PropsArray;
	if (JsonObject->TryGetArrayField(TEXT("properties"), PropsArray))
	{
		for (const auto& Value : *PropsArray)
		{
			Struct.Properties.Add(FOliveIRCppProperty::FromJson(Value->AsObject()));
		}
	}

	DeserializeStringMap(JsonObject, TEXT("metadata"), Struct.Metadata);

	return Struct;
}

// FOliveIRCppSourceFile

TSharedPtr<FJsonObject> FOliveIRCppSourceFile::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("file_path"), FilePath);

	if (!Content.IsEmpty())
	{
		Json->SetStringField(TEXT("content"), Content);
	}

	if (TotalLines > 0)
	{
		Json->SetNumberField(TEXT("total_lines"), TotalLines);
	}
	if (StartLine > 0)
	{
		Json->SetNumberField(TEXT("start_line"), StartLine);
	}
	if (EndLine > 0)
	{
		Json->SetNumberField(TEXT("end_line"), EndLine);
	}
	if (bIsTruncated)
	{
		Json->SetBoolField(TEXT("is_truncated"), true);
	}

	return Json;
}

FOliveIRCppSourceFile FOliveIRCppSourceFile::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRCppSourceFile SourceFile;
	if (!JsonObject.IsValid())
	{
		return SourceFile;
	}

	SourceFile.FilePath = JsonObject->GetStringField(TEXT("file_path"));
	JsonObject->TryGetStringField(TEXT("content"), SourceFile.Content);

	if (JsonObject->HasField(TEXT("total_lines")))
	{
		SourceFile.TotalLines = (int32)JsonObject->GetNumberField(TEXT("total_lines"));
	}
	if (JsonObject->HasField(TEXT("start_line")))
	{
		SourceFile.StartLine = (int32)JsonObject->GetNumberField(TEXT("start_line"));
	}
	if (JsonObject->HasField(TEXT("end_line")))
	{
		SourceFile.EndLine = (int32)JsonObject->GetNumberField(TEXT("end_line"));
	}
	if (JsonObject->HasField(TEXT("is_truncated")))
	{
		SourceFile.bIsTruncated = JsonObject->GetBoolField(TEXT("is_truncated"));
	}

	return SourceFile;
}
