// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Niagara Tool Schema Builder
 *
 * Provides JSON Schema Draft 7 definitions for all Niagara MCP tools.
 * Reuses common helpers from OliveBlueprintSchemas.
 */
namespace OliveNiagaraSchemas
{
	/** Schema for niagara.create_system: {path: string} */
	TSharedPtr<FJsonObject> NiagaraCreateSystem();

	/** Schema for niagara.read_system: {path: string} */
	TSharedPtr<FJsonObject> NiagaraReadSystem();

	/** Schema for niagara.add_emitter: {path: string, source_emitter?: string, name?: string} */
	TSharedPtr<FJsonObject> NiagaraAddEmitter();

	/** Schema for niagara.add_module: {path, emitter_id, stage, module, index?} */
	TSharedPtr<FJsonObject> NiagaraAddModule();

	/** Schema for niagara.remove_module: {path, emitter_id, module_id} */
	TSharedPtr<FJsonObject> NiagaraRemoveModule();

	/** Schema for niagara.list_modules: {query?, stage?} */
	TSharedPtr<FJsonObject> NiagaraListModules();

	/** Schema for niagara.compile: {path: string} */
	TSharedPtr<FJsonObject> NiagaraCompile();

	/** Schema for niagara.set_parameter: {path, module_id, parameter, value, type?} */
	TSharedPtr<FJsonObject> NiagaraSetParameter();

	/** Schema for niagara.describe_module: {path, module_id} */
	TSharedPtr<FJsonObject> NiagaraDescribeModule();

	/** Schema for niagara.set_emitter_property: {path, emitter_id, property, value} */
	TSharedPtr<FJsonObject> NiagaraSetEmitterProperty();
}
