// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

class UNiagaraSystem;
enum class EOliveIRNiagaraStage : uint8;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveNiagaraTools, Log, All);

/**
 * FOliveNiagaraToolHandlers
 *
 * Registers and handles all Niagara MCP tools.
 * Acts as a bridge between the MCP tool registry and the Niagara
 * reader/writer/catalog infrastructure.
 *
 * Tools: niagara.create_system, niagara.read_system, niagara.add_emitter,
 *        niagara.add_module, niagara.remove_module, niagara.list_modules,
 *        niagara.compile, niagara.set_parameter, niagara.describe_module,
 *        niagara.set_emitter_property
 */
class OLIVEAIEDITOR_API FOliveNiagaraToolHandlers
{
public:
	/** Get singleton instance */
	static FOliveNiagaraToolHandlers& Get();

	/** Register all Niagara tools with the tool registry (guarded by Niagara availability) */
	void RegisterAllTools();

	/** Unregister all Niagara tools */
	void UnregisterAllTools();

private:
	FOliveNiagaraToolHandlers() = default;
	FOliveNiagaraToolHandlers(const FOliveNiagaraToolHandlers&) = delete;
	FOliveNiagaraToolHandlers& operator=(const FOliveNiagaraToolHandlers&) = delete;

	// Tool handlers
	FOliveToolResult HandleCreateSystem(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleReadSystem(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleAddEmitter(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleAddModule(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleRemoveModule(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleListModules(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleCompile(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleSetParameter(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleDescribeModule(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleSetEmitterProperty(const TSharedPtr<FJsonObject>& Params);

	// Helpers

	/**
	 * Load a UNiagaraSystem from the "path" parameter in Params.
	 * @param Params JSON parameters containing a "path" string field
	 * @param OutSystem Receives the loaded system on success
	 * @param OutError Receives the error result on failure
	 * @return True if the system was loaded successfully
	 */
	bool LoadSystemFromParams(const TSharedPtr<FJsonObject>& Params,
		UNiagaraSystem*& OutSystem, FOliveToolResult& OutError);

	/**
	 * Parse a stage parameter string into the IR stage enum.
	 * Accepts both PascalCase ("ParticleUpdate") and snake_case ("particle_update").
	 * @param StageStr The stage string to parse
	 * @return Parsed stage enum value, or Unknown if not recognized
	 */
	EOliveIRNiagaraStage ParseStageParam(const FString& StageStr) const;

	TArray<FString> RegisteredToolNames;
};
