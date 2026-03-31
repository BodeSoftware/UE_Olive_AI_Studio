// Copyright Bode Software. All Rights Reserved.

/**
 * NiagaraIR.cpp
 *
 * ToJson() / FromJson() serialization for Niagara IR structs.
 * Follows the same pattern as PCGIR.cpp — each struct produces a
 * TSharedPtr<FJsonObject> and reconstructs from one.
 */

#include "IR/NiagaraIR.h"
#include "Serialization/JsonSerializer.h"

// =============================================================================
// Stage enum <-> string helpers (local to this TU)
// =============================================================================

namespace
{

FString StageToString(EOliveIRNiagaraStage Stage)
{
	switch (Stage)
	{
		case EOliveIRNiagaraStage::SystemSpawn:    return TEXT("SystemSpawn");
		case EOliveIRNiagaraStage::SystemUpdate:   return TEXT("SystemUpdate");
		case EOliveIRNiagaraStage::EmitterSpawn:   return TEXT("EmitterSpawn");
		case EOliveIRNiagaraStage::EmitterUpdate:  return TEXT("EmitterUpdate");
		case EOliveIRNiagaraStage::ParticleSpawn:  return TEXT("ParticleSpawn");
		case EOliveIRNiagaraStage::ParticleUpdate: return TEXT("ParticleUpdate");
		default:                                    return TEXT("Unknown");
	}
}

EOliveIRNiagaraStage StringToStage(const FString& Str)
{
	if (Str == TEXT("SystemSpawn"))    return EOliveIRNiagaraStage::SystemSpawn;
	if (Str == TEXT("SystemUpdate"))   return EOliveIRNiagaraStage::SystemUpdate;
	if (Str == TEXT("EmitterSpawn"))   return EOliveIRNiagaraStage::EmitterSpawn;
	if (Str == TEXT("EmitterUpdate"))  return EOliveIRNiagaraStage::EmitterUpdate;
	if (Str == TEXT("ParticleSpawn"))  return EOliveIRNiagaraStage::ParticleSpawn;
	if (Str == TEXT("ParticleUpdate")) return EOliveIRNiagaraStage::ParticleUpdate;
	return EOliveIRNiagaraStage::Unknown;
}

} // anonymous namespace

// =============================================================================
// FOliveIRNiagaraParameter
// =============================================================================

TSharedPtr<FJsonObject> FOliveIRNiagaraParameter::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("type"), TypeName);

	if (!DefaultValue.IsEmpty())
	{
		Json->SetStringField(TEXT("default_value"), DefaultValue);
	}

	if (bIsOverridden)
	{
		Json->SetBoolField(TEXT("is_overridden"), true);
		if (!OverrideValue.IsEmpty())
		{
			Json->SetStringField(TEXT("override_value"), OverrideValue);
		}
	}

	return Json;
}

FOliveIRNiagaraParameter FOliveIRNiagaraParameter::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRNiagaraParameter Param;
	if (!JsonObject.IsValid())
	{
		return Param;
	}

	Param.Name = JsonObject->GetStringField(TEXT("name"));
	Param.TypeName = JsonObject->GetStringField(TEXT("type"));
	JsonObject->TryGetStringField(TEXT("default_value"), Param.DefaultValue);
	JsonObject->TryGetStringField(TEXT("override_value"), Param.OverrideValue);

	if (JsonObject->HasField(TEXT("is_overridden")))
	{
		Param.bIsOverridden = JsonObject->GetBoolField(TEXT("is_overridden"));
	}

	return Param;
}

// =============================================================================
// FOliveIRNiagaraModule
// =============================================================================

TSharedPtr<FJsonObject> FOliveIRNiagaraModule::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("id"), Id);
	Json->SetStringField(TEXT("module_name"), ModuleName);

	if (!ScriptAssetPath.IsEmpty())
	{
		Json->SetStringField(TEXT("script_asset_path"), ScriptAssetPath);
	}

	Json->SetStringField(TEXT("stage"), StageToString(Stage));
	Json->SetNumberField(TEXT("stack_index"), StackIndex);

	if (!bEnabled)
	{
		Json->SetBoolField(TEXT("enabled"), false);
	}

	if (Parameters.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ParamsArray;
		for (const FOliveIRNiagaraParameter& Param : Parameters)
		{
			ParamsArray.Add(MakeShared<FJsonValueObject>(Param.ToJson()));
		}
		Json->SetArrayField(TEXT("parameters"), ParamsArray);
	}

	return Json;
}

FOliveIRNiagaraModule FOliveIRNiagaraModule::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRNiagaraModule Module;
	if (!JsonObject.IsValid())
	{
		return Module;
	}

	Module.Id = JsonObject->GetStringField(TEXT("id"));
	Module.ModuleName = JsonObject->GetStringField(TEXT("module_name"));
	JsonObject->TryGetStringField(TEXT("script_asset_path"), Module.ScriptAssetPath);

	FString StageStr;
	if (JsonObject->TryGetStringField(TEXT("stage"), StageStr))
	{
		Module.Stage = StringToStage(StageStr);
	}

	if (JsonObject->HasField(TEXT("stack_index")))
	{
		Module.StackIndex = static_cast<int32>(JsonObject->GetNumberField(TEXT("stack_index")));
	}

	// Default enabled to true if not present
	if (JsonObject->HasField(TEXT("enabled")))
	{
		Module.bEnabled = JsonObject->GetBoolField(TEXT("enabled"));
	}
	else
	{
		Module.bEnabled = true;
	}

	const TArray<TSharedPtr<FJsonValue>>* ParamsArray;
	if (JsonObject->TryGetArrayField(TEXT("parameters"), ParamsArray))
	{
		for (const auto& Value : *ParamsArray)
		{
			Module.Parameters.Add(
				FOliveIRNiagaraParameter::FromJson(Value->AsObject()));
		}
	}

	return Module;
}

// =============================================================================
// FOliveIRNiagaraRenderer
// =============================================================================

TSharedPtr<FJsonObject> FOliveIRNiagaraRenderer::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("renderer_type"), RendererType);

	if (!bEnabled)
	{
		Json->SetBoolField(TEXT("enabled"), false);
	}

	if (Properties.Num() > 0)
	{
		TSharedPtr<FJsonObject> PropsJson = MakeShared<FJsonObject>();
		for (const auto& Pair : Properties)
		{
			PropsJson->SetStringField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(TEXT("properties"), PropsJson);
	}

	return Json;
}

FOliveIRNiagaraRenderer FOliveIRNiagaraRenderer::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRNiagaraRenderer Renderer;
	if (!JsonObject.IsValid())
	{
		return Renderer;
	}

	Renderer.RendererType = JsonObject->GetStringField(TEXT("renderer_type"));

	// Default enabled to true if not present
	if (JsonObject->HasField(TEXT("enabled")))
	{
		Renderer.bEnabled = JsonObject->GetBoolField(TEXT("enabled"));
	}
	else
	{
		Renderer.bEnabled = true;
	}

	const TSharedPtr<FJsonObject>* PropsJson;
	if (JsonObject->TryGetObjectField(TEXT("properties"), PropsJson))
	{
		for (const auto& Pair : (*PropsJson)->Values)
		{
			Renderer.Properties.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	return Renderer;
}

// =============================================================================
// FOliveIRNiagaraEmitter
// =============================================================================

TSharedPtr<FJsonObject> FOliveIRNiagaraEmitter::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("id"), Id);
	Json->SetStringField(TEXT("name"), Name);

	if (!SourceEmitterPath.IsEmpty())
	{
		Json->SetStringField(TEXT("source_emitter_path"), SourceEmitterPath);
	}

	if (!bEnabled)
	{
		Json->SetBoolField(TEXT("enabled"), false);
	}

	if (Modules.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ModulesArray;
		for (const FOliveIRNiagaraModule& Module : Modules)
		{
			ModulesArray.Add(MakeShared<FJsonValueObject>(Module.ToJson()));
		}
		Json->SetArrayField(TEXT("modules"), ModulesArray);
	}

	if (Renderers.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> RenderersArray;
		for (const FOliveIRNiagaraRenderer& Renderer : Renderers)
		{
			RenderersArray.Add(MakeShared<FJsonValueObject>(Renderer.ToJson()));
		}
		Json->SetArrayField(TEXT("renderers"), RenderersArray);
	}

	return Json;
}

FOliveIRNiagaraEmitter FOliveIRNiagaraEmitter::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRNiagaraEmitter Emitter;
	if (!JsonObject.IsValid())
	{
		return Emitter;
	}

	Emitter.Id = JsonObject->GetStringField(TEXT("id"));
	Emitter.Name = JsonObject->GetStringField(TEXT("name"));
	JsonObject->TryGetStringField(TEXT("source_emitter_path"), Emitter.SourceEmitterPath);

	// Default enabled to true if not present
	if (JsonObject->HasField(TEXT("enabled")))
	{
		Emitter.bEnabled = JsonObject->GetBoolField(TEXT("enabled"));
	}
	else
	{
		Emitter.bEnabled = true;
	}

	const TArray<TSharedPtr<FJsonValue>>* ModulesArray;
	if (JsonObject->TryGetArrayField(TEXT("modules"), ModulesArray))
	{
		for (const auto& Value : *ModulesArray)
		{
			Emitter.Modules.Add(
				FOliveIRNiagaraModule::FromJson(Value->AsObject()));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RenderersArray;
	if (JsonObject->TryGetArrayField(TEXT("renderers"), RenderersArray))
	{
		for (const auto& Value : *RenderersArray)
		{
			Emitter.Renderers.Add(
				FOliveIRNiagaraRenderer::FromJson(Value->AsObject()));
		}
	}

	return Emitter;
}

// =============================================================================
// FOliveIRNiagaraSystem
// =============================================================================

TSharedPtr<FJsonObject> FOliveIRNiagaraSystem::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("path"), Path);
	Json->SetBoolField(TEXT("compile_status"), bCompileStatus);

	if (SystemModules.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ModulesArray;
		for (const FOliveIRNiagaraModule& Module : SystemModules)
		{
			ModulesArray.Add(MakeShared<FJsonValueObject>(Module.ToJson()));
		}
		Json->SetArrayField(TEXT("system_modules"), ModulesArray);
	}

	if (Emitters.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> EmittersArray;
		for (const FOliveIRNiagaraEmitter& Emitter : Emitters)
		{
			EmittersArray.Add(MakeShared<FJsonValueObject>(Emitter.ToJson()));
		}
		Json->SetArrayField(TEXT("emitters"), EmittersArray);
	}

	return Json;
}

FOliveIRNiagaraSystem FOliveIRNiagaraSystem::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRNiagaraSystem System;
	if (!JsonObject.IsValid())
	{
		return System;
	}

	System.Name = JsonObject->GetStringField(TEXT("name"));
	System.Path = JsonObject->GetStringField(TEXT("path"));

	if (JsonObject->HasField(TEXT("compile_status")))
	{
		System.bCompileStatus = JsonObject->GetBoolField(TEXT("compile_status"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ModulesArray;
	if (JsonObject->TryGetArrayField(TEXT("system_modules"), ModulesArray))
	{
		for (const auto& Value : *ModulesArray)
		{
			System.SystemModules.Add(
				FOliveIRNiagaraModule::FromJson(Value->AsObject()));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* EmittersArray;
	if (JsonObject->TryGetArrayField(TEXT("emitters"), EmittersArray))
	{
		for (const auto& Value : *EmittersArray)
		{
			System.Emitters.Add(
				FOliveIRNiagaraEmitter::FromJson(Value->AsObject()));
		}
	}

	return System;
}
