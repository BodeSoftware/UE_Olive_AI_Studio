// Copyright Bode Software. All Rights Reserved.

#include "OliveNiagaraSchemas.h"
#include "OliveBlueprintSchemas.h"

namespace OliveNiagaraSchemas
{
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

	TSharedPtr<FJsonObject> NiagaraCreateSystem()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path for the new Niagara system (e.g., /Game/VFX/NS_MyEffect)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> NiagaraReadSystem()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path of the Niagara system to read")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> NiagaraAddEmitter()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path of the Niagara system")));
		Props->SetObjectField(TEXT("source_emitter"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of a source emitter to copy from (e.g., /Niagara/DefaultAssets/...). Omit for empty emitter")));
		Props->SetObjectField(TEXT("name"),
			OliveBlueprintSchemas::StringProp(TEXT("Display name for the new emitter")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> NiagaraAddModule()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path of the Niagara system")));
		Props->SetObjectField(TEXT("emitter_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Target emitter ID (e.g., emitter_0)")));
		Props->SetObjectField(TEXT("stage"),
			OliveBlueprintSchemas::EnumProp(TEXT("Niagara stack stage"), {
				TEXT("EmitterSpawn"),
				TEXT("EmitterUpdate"),
				TEXT("ParticleSpawn"),
				TEXT("ParticleUpdate"),
				TEXT("SystemSpawn"),
				TEXT("SystemUpdate")
			}));
		Props->SetObjectField(TEXT("module"),
			OliveBlueprintSchemas::StringProp(TEXT("Module name or script asset path (e.g., 'Spawn Rate' or '/Niagara/Modules/...')")));
		Props->SetObjectField(TEXT("index"),
			OliveBlueprintSchemas::IntProp(TEXT("Insert position in the stage stack (-1 or omit for end)"), -1));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("emitter_id"), TEXT("stage"), TEXT("module") });
		return Schema;
	}

	TSharedPtr<FJsonObject> NiagaraRemoveModule()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path of the Niagara system")));
		Props->SetObjectField(TEXT("emitter_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Target emitter ID")));
		Props->SetObjectField(TEXT("module_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Module ID to remove (e.g., emitter_0.ParticleUpdate.module_2)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("emitter_id"), TEXT("module_id") });
		return Schema;
	}

	TSharedPtr<FJsonObject> NiagaraListModules()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("query"),
			OliveBlueprintSchemas::StringProp(TEXT("Search query to filter modules (e.g., 'velocity', 'spawn')")));
		Props->SetObjectField(TEXT("stage"),
			OliveBlueprintSchemas::EnumProp(TEXT("Filter by compatible stage"), {
				TEXT("EmitterSpawn"),
				TEXT("EmitterUpdate"),
				TEXT("ParticleSpawn"),
				TEXT("ParticleUpdate"),
				TEXT("SystemSpawn"),
				TEXT("SystemUpdate")
			}));

		Schema->SetObjectField(TEXT("properties"), Props);
		// No required fields — both query and stage are optional
		return Schema;
	}

	TSharedPtr<FJsonObject> NiagaraCompile()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path of the Niagara system to compile")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> NiagaraSetParameter()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path of the Niagara system")));
		Props->SetObjectField(TEXT("module_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Module ID (e.g., emitter_0.ParticleUpdate.module_2)")));
		Props->SetObjectField(TEXT("parameter"),
			OliveBlueprintSchemas::StringProp(TEXT("Parameter name (e.g., SpawnRate, Velocity)")));
		Props->SetObjectField(TEXT("value"),
			OliveBlueprintSchemas::StringProp(TEXT("Value as string (e.g., '42.0', '100,0,200' for vectors, '1,0.5,0,1' for colors)")));
		Props->SetObjectField(TEXT("type"),
			OliveBlueprintSchemas::StringProp(TEXT("Value type hint: float, int, vec2, vec3, vec4, color. Auto-detected if omitted")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("module_id"), TEXT("parameter"), TEXT("value") });
		return Schema;
	}

	TSharedPtr<FJsonObject> NiagaraDescribeModule()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path of the Niagara system")));
		Props->SetObjectField(TEXT("module_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Module ID to describe (e.g., emitter_0.ParticleUpdate.module_2)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("module_id") });
		return Schema;
	}

	TSharedPtr<FJsonObject> NiagaraSetEmitterProperty()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path of the Niagara system")));
		Props->SetObjectField(TEXT("emitter_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Emitter ID (e.g., emitter_0)")));
		Props->SetObjectField(TEXT("property"),
			OliveBlueprintSchemas::StringProp(TEXT("Property name (e.g., SimTarget, CalculateBoundsMode, bDeterminism)")));
		Props->SetObjectField(TEXT("value"),
			OliveBlueprintSchemas::StringProp(TEXT("Property value as string")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("emitter_id"), TEXT("property"), TEXT("value") });
		return Schema;
	}
}
