// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/NiagaraIR.h"

class UNiagaraSystem;
class UNiagaraNodeOutput;
class UNiagaraNodeFunctionCall;
struct FNiagaraEmitterHandle;
struct FVersionedNiagaraEmitterData;

/**
 * FOliveNiagaraReader
 *
 * Reads UNiagaraSystem assets into IR structs for AI consumption.
 * Walks the system's emitter handles, per-stage module stacks (via
 * UNiagaraNodeOutput -> ordered UNiagaraNodeFunctionCall chain), and
 * renderer lists to produce a structured representation.
 *
 * Singleton pattern matching FOlivePCGReader / FOliveBlueprintReader.
 *
 * Usage:
 *   TOptional<FOliveIRNiagaraSystem> IR = FOliveNiagaraReader::Get().ReadSystem("/Game/VFX/NS_Fire");
 */
class OLIVEAIEDITOR_API FOliveNiagaraReader
{
public:
	/** Get the singleton instance */
	static FOliveNiagaraReader& Get();

	/**
	 * Read a Niagara system by asset path.
	 * @param AssetPath Content path to the Niagara system (e.g., "/Game/VFX/NS_Fire")
	 * @return IR representation, or unset TOptional on failure
	 */
	TOptional<FOliveIRNiagaraSystem> ReadSystem(const FString& AssetPath);

	/**
	 * Read a Niagara system from an already-loaded pointer.
	 * @param System The loaded UNiagaraSystem to read
	 * @return IR representation, or unset TOptional if System is null
	 */
	TOptional<FOliveIRNiagaraSystem> ReadSystem(UNiagaraSystem* System);

private:
	FOliveNiagaraReader() = default;
	~FOliveNiagaraReader() = default;
	FOliveNiagaraReader(const FOliveNiagaraReader&) = delete;
	FOliveNiagaraReader& operator=(const FOliveNiagaraReader&) = delete;

	/**
	 * Load a Niagara system by content path via StaticLoadObject.
	 * @param AssetPath Content path to the asset
	 * @return Loaded UNiagaraSystem, or nullptr on failure
	 */
	UNiagaraSystem* LoadSystem(const FString& AssetPath) const;

	/**
	 * Serialize an emitter at the given index to IR.
	 * Iterates over emitter-level stages (EmitterSpawn, EmitterUpdate,
	 * ParticleSpawn, ParticleUpdate) and serializes all modules and renderers.
	 * @param System The owning system
	 * @param EmitterIndex Index into System->GetEmitterHandles()
	 * @return IR emitter struct
	 */
	FOliveIRNiagaraEmitter SerializeEmitter(UNiagaraSystem* System, int32 EmitterIndex) const;

	/**
	 * Serialize the ordered module chain for a given stage within an emitter.
	 * Uses FNiagaraEditorUtilities::GetScriptFromSystem to get the stage script,
	 * then FNiagaraStackGraphUtilities::GetOrderedModuleNodes for the linear chain.
	 * @param System The owning system
	 * @param EmitterHandleId The emitter's FGuid identifier
	 * @param EmitterId The IR-level emitter ID string (e.g., "emitter_0")
	 * @param Stage Which stage to read modules from
	 * @return Array of IR modules for this stage
	 */
	TArray<FOliveIRNiagaraModule> SerializeStageModules(
		UNiagaraSystem* System,
		FGuid EmitterHandleId,
		const FString& EmitterId,
		EOliveIRNiagaraStage Stage) const;

	/**
	 * Serialize system-level modules (SystemSpawn, SystemUpdate stages).
	 * These exist on the system's own scripts, not on any emitter.
	 * @param System The system to read
	 * @return Array of IR modules across both system stages
	 */
	TArray<FOliveIRNiagaraModule> SerializeSystemModules(UNiagaraSystem* System) const;

	/**
	 * Serialize all renderers attached to an emitter.
	 * Reads from FVersionedNiagaraEmitterData::GetRenderers().
	 * @param EmitterData The versioned emitter data to read renderers from
	 * @return Array of IR renderer structs
	 */
	TArray<FOliveIRNiagaraRenderer> SerializeRenderers(FVersionedNiagaraEmitterData* EmitterData) const;

	/**
	 * Serialize renderer-specific editable properties via UObject reflection.
	 * Skips properties owned by the UNiagaraRendererProperties base class and UObject.
	 * @param RendererProps The renderer properties object
	 * @param OutProperties Output map of property name -> exported value string
	 */
	void SerializeRendererProperties(
		const class UNiagaraRendererProperties* RendererProps,
		TMap<FString, FString>& OutProperties) const;

	/**
	 * Serialize parameters (inputs) for a single module node.
	 * Reads the module's UNiagaraGraph script variables to discover all inputs.
	 * @param ModuleNode The function call node representing this module
	 * @return Array of IR parameter structs
	 */
	TArray<FOliveIRNiagaraParameter> SerializeModuleParameters(
		const UNiagaraNodeFunctionCall* ModuleNode) const;

	/**
	 * Map ENiagaraScriptUsage (int-cast) to our IR stage enum.
	 * @param Usage The engine script usage value
	 * @return Corresponding IR stage, or Unknown for unrecognized values
	 */
	static EOliveIRNiagaraStage MapScriptUsage(int32 Usage);

	/**
	 * Map our IR stage enum to ENiagaraScriptUsage integer for API calls.
	 * @param Stage The IR stage enum
	 * @return The corresponding ENiagaraScriptUsage cast to int32
	 */
	static int32 MapStageToScriptUsage(EOliveIRNiagaraStage Stage);

	/**
	 * Get a human-readable stage name for module ID construction.
	 * @param Stage The IR stage enum
	 * @return Stage name string (e.g., "ParticleUpdate", "SystemSpawn")
	 */
	static FString GetStageName(EOliveIRNiagaraStage Stage);
};
