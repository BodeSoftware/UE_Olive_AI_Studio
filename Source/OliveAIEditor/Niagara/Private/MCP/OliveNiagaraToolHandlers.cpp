// Copyright Bode Software. All Rights Reserved.

#include "OliveNiagaraToolHandlers.h"
#include "OliveNiagaraSchemas.h"
#include "OliveNiagaraReader.h"
#include "OliveNiagaraWriter.h"
#include "OliveNiagaraModuleCatalog.h"
#include "OliveNiagaraAvailability.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOliveNiagaraTools);

FOliveNiagaraToolHandlers& FOliveNiagaraToolHandlers::Get()
{
	static FOliveNiagaraToolHandlers Instance;
	return Instance;
}

// ============================================================================
// Registration
// ============================================================================

void FOliveNiagaraToolHandlers::RegisterAllTools()
{
	if (!FOliveNiagaraAvailability::IsNiagaraAvailable())
	{
		UE_LOG(LogOliveNiagaraTools, Log, TEXT("Niagara plugin not available, skipping Niagara tool registration"));
		return;
	}

	UE_LOG(LogOliveNiagaraTools, Log, TEXT("Registering Niagara MCP tools..."));

	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// niagara.create_system
	Registry.RegisterTool(
		TEXT("niagara.create_system"),
		TEXT("Create a new Niagara particle system asset"),
		OliveNiagaraSchemas::NiagaraCreateSystem(),
		FOliveToolHandler::CreateRaw(this, &FOliveNiagaraToolHandlers::HandleCreateSystem),
		{TEXT("niagara"), TEXT("write")},
		TEXT("niagara")
	);
	RegisteredToolNames.Add(TEXT("niagara.create_system"));

	// niagara.read_system
	Registry.RegisterTool(
		TEXT("niagara.read_system"),
		TEXT("Read the structure of a Niagara system (emitters, modules, renderers)"),
		OliveNiagaraSchemas::NiagaraReadSystem(),
		FOliveToolHandler::CreateRaw(this, &FOliveNiagaraToolHandlers::HandleReadSystem),
		{TEXT("niagara"), TEXT("read")},
		TEXT("niagara")
	);
	RegisteredToolNames.Add(TEXT("niagara.read_system"));

	// niagara.add_emitter
	Registry.RegisterTool(
		TEXT("niagara.add_emitter"),
		TEXT("Add an emitter to a Niagara system"),
		OliveNiagaraSchemas::NiagaraAddEmitter(),
		FOliveToolHandler::CreateRaw(this, &FOliveNiagaraToolHandlers::HandleAddEmitter),
		{TEXT("niagara"), TEXT("write")},
		TEXT("niagara")
	);
	RegisteredToolNames.Add(TEXT("niagara.add_emitter"));

	// niagara.add_module
	Registry.RegisterTool(
		TEXT("niagara.add_module"),
		TEXT("Add a module to a Niagara emitter's stage stack"),
		OliveNiagaraSchemas::NiagaraAddModule(),
		FOliveToolHandler::CreateRaw(this, &FOliveNiagaraToolHandlers::HandleAddModule),
		{TEXT("niagara"), TEXT("write")},
		TEXT("niagara")
	);
	RegisteredToolNames.Add(TEXT("niagara.add_module"));

	// niagara.remove_module
	Registry.RegisterTool(
		TEXT("niagara.remove_module"),
		TEXT("Remove a module from a Niagara emitter's stage stack"),
		OliveNiagaraSchemas::NiagaraRemoveModule(),
		FOliveToolHandler::CreateRaw(this, &FOliveNiagaraToolHandlers::HandleRemoveModule),
		{TEXT("niagara"), TEXT("write")},
		TEXT("niagara")
	);
	RegisteredToolNames.Add(TEXT("niagara.remove_module"));

	// niagara.list_modules
	Registry.RegisterTool(
		TEXT("niagara.list_modules"),
		TEXT("Search available Niagara modules by name, category, or stage"),
		OliveNiagaraSchemas::NiagaraListModules(),
		FOliveToolHandler::CreateRaw(this, &FOliveNiagaraToolHandlers::HandleListModules),
		{TEXT("niagara"), TEXT("read")},
		TEXT("niagara")
	);
	RegisteredToolNames.Add(TEXT("niagara.list_modules"));

	// niagara.compile
	Registry.RegisterTool(
		TEXT("niagara.compile"),
		TEXT("Compile a Niagara system (async)"),
		OliveNiagaraSchemas::NiagaraCompile(),
		FOliveToolHandler::CreateRaw(this, &FOliveNiagaraToolHandlers::HandleCompile),
		{TEXT("niagara"), TEXT("write")},
		TEXT("niagara")
	);
	RegisteredToolNames.Add(TEXT("niagara.compile"));

	// niagara.set_parameter
	Registry.RegisterTool(
		TEXT("niagara.set_parameter"),
		TEXT("Set a module parameter value (supports float, int, vector, color)"),
		OliveNiagaraSchemas::NiagaraSetParameter(),
		FOliveToolHandler::CreateRaw(this, &FOliveNiagaraToolHandlers::HandleSetParameter),
		{TEXT("niagara"), TEXT("write")},
		TEXT("niagara")
	);
	RegisteredToolNames.Add(TEXT("niagara.set_parameter"));

	// niagara.describe_module
	Registry.RegisterTool(
		TEXT("niagara.describe_module"),
		TEXT("Describe a module's parameters (names, types, defaults, current values)"),
		OliveNiagaraSchemas::NiagaraDescribeModule(),
		FOliveToolHandler::CreateRaw(this, &FOliveNiagaraToolHandlers::HandleDescribeModule),
		{TEXT("niagara"), TEXT("read")},
		TEXT("niagara")
	);
	RegisteredToolNames.Add(TEXT("niagara.describe_module"));

	// niagara.set_emitter_property
	Registry.RegisterTool(
		TEXT("niagara.set_emitter_property"),
		TEXT("Set an emitter-level property (SimTarget, bounds, determinism, etc.)"),
		OliveNiagaraSchemas::NiagaraSetEmitterProperty(),
		FOliveToolHandler::CreateRaw(this, &FOliveNiagaraToolHandlers::HandleSetEmitterProperty),
		{TEXT("niagara"), TEXT("write")},
		TEXT("niagara")
	);
	RegisteredToolNames.Add(TEXT("niagara.set_emitter_property"));

	UE_LOG(LogOliveNiagaraTools, Log, TEXT("Registered %d Niagara MCP tools"), RegisteredToolNames.Num());
}

void FOliveNiagaraToolHandlers::UnregisterAllTools()
{
	UE_LOG(LogOliveNiagaraTools, Log, TEXT("Unregistering Niagara MCP tools..."));

	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
	for (const FString& ToolName : RegisteredToolNames)
	{
		Registry.UnregisterTool(ToolName);
	}

	RegisteredToolNames.Empty();
	UE_LOG(LogOliveNiagaraTools, Log, TEXT("Niagara MCP tools unregistered"));
}

// ============================================================================
// Helpers
// ============================================================================

bool FOliveNiagaraToolHandlers::LoadSystemFromParams(
	const TSharedPtr<FJsonObject>& Params,
	UNiagaraSystem*& OutSystem,
	FOliveToolResult& OutError)
{
	FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		OutError = FOliveToolResult::Error(TEXT("MISSING_PATH"),
			TEXT("Missing required 'path' parameter"),
			TEXT("Provide the asset path of the Niagara system (e.g., /Game/VFX/NS_MyEffect)"));
		return false;
	}

	OutSystem = FOliveNiagaraWriter::Get().LoadSystem(Path);
	if (!OutSystem)
	{
		OutError = FOliveToolResult::Error(TEXT("NIAGARA_SYSTEM_NOT_FOUND"),
			FString::Printf(TEXT("Niagara system not found: %s"), *Path),
			TEXT("Use project.search to find the correct asset path, or niagara.create_system to create a new one"));
		return false;
	}

	return true;
}

EOliveIRNiagaraStage FOliveNiagaraToolHandlers::ParseStageParam(const FString& StageStr) const
{
	// Normalize to lowercase for comparison
	FString Lower = StageStr.ToLower();

	if (Lower == TEXT("emitterspawn") || Lower == TEXT("emitter_spawn"))
	{
		return EOliveIRNiagaraStage::EmitterSpawn;
	}
	if (Lower == TEXT("emitterupdate") || Lower == TEXT("emitter_update"))
	{
		return EOliveIRNiagaraStage::EmitterUpdate;
	}
	if (Lower == TEXT("particlespawn") || Lower == TEXT("particle_spawn"))
	{
		return EOliveIRNiagaraStage::ParticleSpawn;
	}
	if (Lower == TEXT("particleupdate") || Lower == TEXT("particle_update"))
	{
		return EOliveIRNiagaraStage::ParticleUpdate;
	}
	if (Lower == TEXT("systemspawn") || Lower == TEXT("system_spawn"))
	{
		return EOliveIRNiagaraStage::SystemSpawn;
	}
	if (Lower == TEXT("systemupdate") || Lower == TEXT("system_update"))
	{
		return EOliveIRNiagaraStage::SystemUpdate;
	}

	return EOliveIRNiagaraStage::Unknown;
}

// ============================================================================
// Tool Handlers
// ============================================================================

FOliveToolResult FOliveNiagaraToolHandlers::HandleCreateSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing"),
			TEXT("Provide 'path' as a /Game/... asset path. Example: \"/Game/VFX/NS_MyEffect\""));
	}

	UNiagaraSystem* NewSystem = FOliveNiagaraWriter::Get().CreateSystem(Path);
	if (!NewSystem)
	{
		return FOliveToolResult::Error(TEXT("NIAGARA_CREATE_FAILED"),
			FString::Printf(TEXT("Failed to create Niagara system at '%s'"), *Path),
			TEXT("Verify the path is a valid /Game/... asset path and the parent directory exists"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), NewSystem->GetName());
	Result->SetStringField(TEXT("path"), NewSystem->GetPathName());
	Result->SetStringField(TEXT("status"), TEXT("created"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveNiagaraToolHandlers::HandleReadSystem(const TSharedPtr<FJsonObject>& Params)
{
	UNiagaraSystem* System;
	FOliveToolResult Error;
	if (!LoadSystemFromParams(Params, System, Error))
	{
		return Error;
	}

	TOptional<FOliveIRNiagaraSystem> IR = FOliveNiagaraReader::Get().ReadSystem(System);
	if (!IR.IsSet())
	{
		return FOliveToolResult::Error(TEXT("NIAGARA_READ_FAILED"),
			TEXT("Failed to read Niagara system"),
			TEXT("The asset may be corrupted or not a valid Niagara system. Use project.search to verify."));
	}

	return FOliveToolResult::Success(IR.GetValue().ToJson());
}

FOliveToolResult FOliveNiagaraToolHandlers::HandleAddEmitter(const TSharedPtr<FJsonObject>& Params)
{
	UNiagaraSystem* System;
	FOliveToolResult Error;
	if (!LoadSystemFromParams(Params, System, Error))
	{
		return Error;
	}

	FString SourceEmitter;
	Params->TryGetStringField(TEXT("source_emitter"), SourceEmitter);

	FString Name;
	Params->TryGetStringField(TEXT("name"), Name);

	FString EmitterId = FOliveNiagaraWriter::Get().AddEmitter(System, SourceEmitter, Name);
	if (EmitterId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("NIAGARA_ADD_EMITTER_FAILED"),
			TEXT("Failed to add emitter to the Niagara system"),
			SourceEmitter.IsEmpty()
				? TEXT("The system may be in an invalid state. Try niagara.read_system to inspect it.")
				: FString::Printf(TEXT("Verify the source emitter exists: %s. Use niagara.list_modules or project.search to find valid emitters."), *SourceEmitter));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("emitter_id"), EmitterId);
	if (!Name.IsEmpty())
	{
		Result->SetStringField(TEXT("name"), Name);
	}
	Result->SetStringField(TEXT("status"), TEXT("added"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveNiagaraToolHandlers::HandleAddModule(const TSharedPtr<FJsonObject>& Params)
{
	UNiagaraSystem* System;
	FOliveToolResult Error;
	if (!LoadSystemFromParams(Params, System, Error))
	{
		return Error;
	}

	FString EmitterId;
	if (!Params->TryGetStringField(TEXT("emitter_id"), EmitterId) || EmitterId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'emitter_id' is missing"),
			TEXT("Provide the emitter_id (e.g., 'emitter_0'). Use niagara.read_system to see emitter IDs."));
	}

	FString StageStr;
	if (!Params->TryGetStringField(TEXT("stage"), StageStr) || StageStr.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'stage' is missing"),
			TEXT("Provide the stage: EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, SystemSpawn, or SystemUpdate"));
	}

	EOliveIRNiagaraStage Stage = ParseStageParam(StageStr);
	if (Stage == EOliveIRNiagaraStage::Unknown)
	{
		return FOliveToolResult::Error(TEXT("INVALID_STAGE"),
			FString::Printf(TEXT("Invalid stage '%s'"), *StageStr),
			TEXT("Valid stages: EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, SystemSpawn, SystemUpdate"));
	}

	FString Module;
	if (!Params->TryGetStringField(TEXT("module"), Module) || Module.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'module' is missing"),
			TEXT("Provide the module name or script asset path. Use niagara.list_modules to search for available modules."));
	}

	int32 Index = Params->HasField(TEXT("index")) ? (int32)Params->GetNumberField(TEXT("index")) : -1;

	FString ModuleId = FOliveNiagaraWriter::Get().AddModule(System, EmitterId, Stage, Module, Index);
	if (ModuleId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("NIAGARA_ADD_MODULE_FAILED"),
			FString::Printf(TEXT("Failed to add module '%s' to %s.%s"), *Module, *EmitterId, *StageStr),
			TEXT("Verify the module exists using niagara.list_modules. Check that the emitter_id and stage are correct."));
	}

	// Build stage name for response
	FString StageName;
	switch (Stage)
	{
	case EOliveIRNiagaraStage::SystemSpawn:    StageName = TEXT("SystemSpawn"); break;
	case EOliveIRNiagaraStage::SystemUpdate:   StageName = TEXT("SystemUpdate"); break;
	case EOliveIRNiagaraStage::EmitterSpawn:   StageName = TEXT("EmitterSpawn"); break;
	case EOliveIRNiagaraStage::EmitterUpdate:  StageName = TEXT("EmitterUpdate"); break;
	case EOliveIRNiagaraStage::ParticleSpawn:  StageName = TEXT("ParticleSpawn"); break;
	case EOliveIRNiagaraStage::ParticleUpdate: StageName = TEXT("ParticleUpdate"); break;
	default: StageName = StageStr; break;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("module_id"), ModuleId);
	Result->SetStringField(TEXT("emitter_id"), EmitterId);
	Result->SetStringField(TEXT("stage"), StageName);
	Result->SetStringField(TEXT("module_name"), Module);
	Result->SetStringField(TEXT("status"), TEXT("added"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveNiagaraToolHandlers::HandleRemoveModule(const TSharedPtr<FJsonObject>& Params)
{
	UNiagaraSystem* System;
	FOliveToolResult Error;
	if (!LoadSystemFromParams(Params, System, Error))
	{
		return Error;
	}

	FString EmitterId;
	if (!Params->TryGetStringField(TEXT("emitter_id"), EmitterId) || EmitterId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'emitter_id' is missing"),
			TEXT("Provide the emitter_id (e.g., 'emitter_0'). Use niagara.read_system to see emitter IDs."));
	}

	FString ModuleId;
	if (!Params->TryGetStringField(TEXT("module_id"), ModuleId) || ModuleId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'module_id' is missing"),
			TEXT("Provide the module_id (e.g., 'emitter_0.ParticleUpdate.module_2'). Use niagara.read_system to see module IDs."));
	}

	bool bSuccess = FOliveNiagaraWriter::Get().RemoveModule(System, EmitterId, ModuleId);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("NIAGARA_REMOVE_MODULE_FAILED"),
			FString::Printf(TEXT("Failed to remove module '%s' from emitter '%s'"), *ModuleId, *EmitterId),
			TEXT("Verify the module and emitter exist using niagara.read_system"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("module_id"), ModuleId);
	Result->SetStringField(TEXT("status"), TEXT("removed"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveNiagaraToolHandlers::HandleListModules(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	Params->TryGetStringField(TEXT("query"), Query);

	FString StageStr;
	Params->TryGetStringField(TEXT("stage"), StageStr);

	EOliveIRNiagaraStage FilterStage = EOliveIRNiagaraStage::Unknown;
	if (!StageStr.IsEmpty())
	{
		FilterStage = ParseStageParam(StageStr);
		if (FilterStage == EOliveIRNiagaraStage::Unknown)
		{
			return FOliveToolResult::Error(TEXT("INVALID_STAGE"),
				FString::Printf(TEXT("Invalid stage filter '%s'"), *StageStr),
				TEXT("Valid stages: EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, SystemSpawn, SystemUpdate"));
		}
	}

	TArray<FOliveNiagaraModuleInfo> Results = FOliveNiagaraModuleCatalog::Get().Search(Query, FilterStage);

	// Build JSON array of module results
	TArray<TSharedPtr<FJsonValue>> ModulesArray;
	for (const FOliveNiagaraModuleInfo& ModuleInfo : Results)
	{
		ModulesArray.Add(MakeShared<FJsonValueObject>(ModuleInfo.ToJson()));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Results.Num());
	Result->SetArrayField(TEXT("modules"), ModulesArray);

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveNiagaraToolHandlers::HandleCompile(const TSharedPtr<FJsonObject>& Params)
{
	UNiagaraSystem* System;
	FOliveToolResult Error;
	if (!LoadSystemFromParams(Params, System, Error))
	{
		return Error;
	}

	FNiagaraCompileResult CompileResult = FOliveNiagaraWriter::Get().Compile(System);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), CompileResult.Summary);
	Result->SetBoolField(TEXT("async"), CompileResult.bAsync);
	Result->SetBoolField(TEXT("success"), CompileResult.bSuccess);

	if (CompileResult.bSuccess)
	{
		return FOliveToolResult::Success(Result);
	}
	else
	{
		return FOliveToolResult::Error(TEXT("NIAGARA_COMPILE_FAILED"),
			CompileResult.Summary,
			TEXT("Check the Niagara system for invalid modules or missing connections. Use niagara.read_system to inspect the current state."));
	}
}

FOliveToolResult FOliveNiagaraToolHandlers::HandleSetParameter(const TSharedPtr<FJsonObject>& Params)
{
	UNiagaraSystem* System;
	FOliveToolResult Error;
	if (!LoadSystemFromParams(Params, System, Error))
	{
		return Error;
	}

	FString ModuleId;
	if (!Params->TryGetStringField(TEXT("module_id"), ModuleId) || ModuleId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'module_id' is missing"),
			TEXT("Provide the module_id (e.g., 'emitter_0.ParticleUpdate.module_2'). Use niagara.read_system to see module IDs."));
	}

	FString Parameter;
	if (!Params->TryGetStringField(TEXT("parameter"), Parameter) || Parameter.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'parameter' is missing"),
			TEXT("Provide the parameter name (e.g., 'SpawnRate'). Use niagara.describe_module to see available parameters."));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'value' is missing"),
			TEXT("Provide the value as a string (e.g., '42.0' for float, '100,0,200' for vector)"));
	}

	// DESIGN NOTE: Optional 'type' hint is exposed in the schema for the AI to provide,
	// but the writer auto-detects the type from the existing parameter metadata.
	// If type hints become needed in the future, the writer API should be extended.

	FNiagaraSetParameterResult SetResult = FOliveNiagaraWriter::Get().SetParameter(
		System, ModuleId, Parameter, Value);

	if (!SetResult.bSuccess)
	{
		return FOliveToolResult::Error(TEXT("NIAGARA_SET_PARAMETER_FAILED"),
			SetResult.ErrorMessage,
			TEXT("Use niagara.describe_module to see available parameters and their types. Verify the module_id is correct with niagara.read_system."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("module_id"), ModuleId);
	Result->SetStringField(TEXT("parameter"), Parameter);
	Result->SetStringField(TEXT("value"), SetResult.ValueSet);
	Result->SetStringField(TEXT("type"), SetResult.TypeName);
	Result->SetBoolField(TEXT("requires_recompile"), SetResult.bRequiresRecompile);
	Result->SetStringField(TEXT("status"), TEXT("parameter_set"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveNiagaraToolHandlers::HandleDescribeModule(const TSharedPtr<FJsonObject>& Params)
{
	UNiagaraSystem* System;
	FOliveToolResult Error;
	if (!LoadSystemFromParams(Params, System, Error))
	{
		return Error;
	}

	FString ModuleId;
	if (!Params->TryGetStringField(TEXT("module_id"), ModuleId) || ModuleId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'module_id' is missing"),
			TEXT("Provide the module_id (e.g., 'emitter_0.ParticleUpdate.module_2'). Use niagara.read_system to see module IDs."));
	}

	TArray<FNiagaraModuleParameterInfo> Parameters = FOliveNiagaraWriter::Get().GetModuleParameters(
		System, ModuleId);

	// Build JSON array of parameters
	TArray<TSharedPtr<FJsonValue>> ParametersArray;
	for (const FNiagaraModuleParameterInfo& ParamInfo : Parameters)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), ParamInfo.Name);
		ParamObj->SetStringField(TEXT("type"), ParamInfo.TypeName);
		ParamObj->SetStringField(TEXT("default_value"), ParamInfo.DefaultValue);
		ParamObj->SetStringField(TEXT("current_value"), ParamInfo.CurrentValue);
		ParamObj->SetBoolField(TEXT("is_overridden"), ParamInfo.bIsOverridden);
		ParamObj->SetBoolField(TEXT("is_rapid_iteration"), ParamInfo.bIsRapidIteration);
		ParametersArray.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("module_id"), ModuleId);
	Result->SetNumberField(TEXT("parameter_count"), Parameters.Num());
	Result->SetArrayField(TEXT("parameters"), ParametersArray);

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveNiagaraToolHandlers::HandleSetEmitterProperty(const TSharedPtr<FJsonObject>& Params)
{
	UNiagaraSystem* System;
	FOliveToolResult Error;
	if (!LoadSystemFromParams(Params, System, Error))
	{
		return Error;
	}

	FString EmitterId;
	if (!Params->TryGetStringField(TEXT("emitter_id"), EmitterId) || EmitterId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'emitter_id' is missing"),
			TEXT("Provide the emitter_id (e.g., 'emitter_0'). Use niagara.read_system to see emitter IDs."));
	}

	FString Property;
	if (!Params->TryGetStringField(TEXT("property"), Property) || Property.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'property' is missing"),
			TEXT("Provide the property name (e.g., 'SimTarget', 'CalculateBoundsMode', 'bDeterminism')"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'value' is missing"),
			TEXT("Provide the property value as a string"));
	}

	bool bSuccess = FOliveNiagaraWriter::Get().SetEmitterProperty(System, EmitterId, Property, Value);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("NIAGARA_SET_EMITTER_PROPERTY_FAILED"),
			FString::Printf(TEXT("Failed to set property '%s' on emitter '%s'"), *Property, *EmitterId),
			TEXT("Verify the emitter_id and property name. Use niagara.read_system to inspect the system structure."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("emitter_id"), EmitterId);
	Result->SetStringField(TEXT("property"), Property);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetStringField(TEXT("status"), TEXT("property_set"));
	Result->SetBoolField(TEXT("requires_recompile"), true);

	return FOliveToolResult::Success(Result);
}
