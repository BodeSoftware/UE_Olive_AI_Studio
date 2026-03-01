// Copyright Bode Software. All Rights Reserved.

#include "OlivePythonSchemas.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace OlivePythonSchemas
{
	static TSharedPtr<FJsonObject> MakeSchema(const FString& Type)
	{
		TSharedPtr<FJsonObject> Schema = MakeShareable(new FJsonObject());
		Schema->SetStringField(TEXT("type"), Type);
		return Schema;
	}

	static TSharedPtr<FJsonObject> StringProp(const FString& Description)
	{
		TSharedPtr<FJsonObject> Prop = MakeSchema(TEXT("string"));
		Prop->SetStringField(TEXT("description"), Description);
		return Prop;
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

	TSharedPtr<FJsonObject> EditorRunPython()
	{
		TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject());

		Properties->SetObjectField(TEXT("script"),
			StringProp(TEXT("Python script to execute in UE's editor scripting context. "
				"The 'unreal' module is available with full access to editor APIs "
				"(e.g., unreal.EditorAssetLibrary, unreal.BlueprintEditorLibrary). "
				"Scripts are wrapped in try/except automatically. "
				"Use print() for output — stdout is captured and returned.")));

		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		Schema->SetStringField(TEXT("description"),
			TEXT("Execute Python in the Unreal Editor's scripting context. "
				"Full access to the 'unreal' module and all editor APIs. "
				"A snapshot is taken automatically before execution for rollback safety. "
				"Use when standard Blueprint/BT/PCG/C++ tools cannot express what you need."));
		Schema->SetObjectField(TEXT("properties"), Properties);
		AddRequired(Schema, {TEXT("script")});

		return Schema;
	}
}
