// Copyright Bode Software. All Rights Reserved.

/**
 * OliveNiagaraWriter.h
 *
 * Writer for Niagara system assets. Handles creation, emitter/module manipulation,
 * parameter setting (via Rapid Iteration Parameters), and compilation.
 *
 * Phase 1: System/emitter/module creation and removal.
 * Phase 2: Parameter manipulation via RIP path, module parameter enumeration,
 *          and emitter property setting via UObject reflection.
 */

#pragma once

#include "CoreMinimal.h"
#include "IR/NiagaraIR.h"

class UNiagaraSystem;
class UNiagaraEmitter;
class UNiagaraScript;
class UNiagaraNodeFunctionCall;
struct FNiagaraTypeDefinition;

/**
 * Result from a Niagara compile operation.
 */
struct OLIVEAIEDITOR_API FNiagaraCompileResult
{
	bool bSuccess = false;

	/** True if compile was kicked off asynchronously and is not yet complete */
	bool bAsync = true;

	/** Human-readable summary (e.g., "Compilation started (async)" or error details) */
	FString Summary;
};

/**
 * Result from setting a Niagara module parameter.
 * Contains the parameter identity, the value that was set, and whether the
 * system needs recompilation (RIP parameters do not, override pins do).
 */
struct OLIVEAIEDITOR_API FNiagaraSetParameterResult
{
	/** Whether the operation succeeded */
	bool bSuccess = false;

	/** Name of the parameter that was set */
	FString ParameterName;

	/** String representation of the value that was applied */
	FString ValueSet;

	/** Niagara type name (e.g., "float", "FVector3f", "FLinearColor") */
	FString TypeName;

	/** True if the system must be recompiled after this change.
	 *  RIP parameters: false. Override pin parameters: true. */
	bool bRequiresRecompile = false;

	/** Error details if bSuccess is false */
	FString ErrorMessage;
};

/**
 * Describes a single parameter (input) on a Niagara module, including
 * its current value and whether it has been overridden from the default.
 * Used by GetModuleParameters() to enumerate module inputs.
 */
struct OLIVEAIEDITOR_API FNiagaraModuleParameterInfo
{
	/** Parameter name as displayed in the module stack */
	FString Name;

	/** Type name (e.g., "float", "FVector3f", "FLinearColor", "int32", "bool") */
	FString TypeName;

	/** Default value in ExportText format */
	FString DefaultValue;

	/** Current override value (empty if using default) */
	FString CurrentValue;

	/** True if the parameter has been overridden from its default */
	bool bIsOverridden = false;

	/** True if this parameter uses the Rapid Iteration Parameter path,
	 *  false if it uses the override pin path */
	bool bIsRapidIteration = true;
};

/**
 * FOliveNiagaraWriter
 *
 * Creates and mutates Niagara system assets. Handles system creation, emitter
 * addition, module stack manipulation, parameter setting, and compilation.
 * Uses emitter/module ID-based addressing matching the reader output
 * (e.g., "emitter_0", "emitter_0.ParticleUpdate.module_2").
 *
 * Phase 2 adds parameter manipulation via the Rapid Iteration Parameter (RIP)
 * path, which is Epic's recommended approach for setting module parameters
 * without going through the ViewModel layer. RIP parameters are stored on the
 * stage script and do not require system recompilation.
 *
 * All write operations are wrapped in FScopedTransaction for undo support.
 *
 * Usage:
 *   FOliveNiagaraWriter& Writer = FOliveNiagaraWriter::Get();
 *   UNiagaraSystem* System = Writer.CreateSystem("/Game/VFX/NS_MyEffect");
 *   FString EmitterId = Writer.AddEmitter(System, "/Niagara/DefaultAssets/...", "Sparks");
 *   FString ModuleId = Writer.AddModule(System, EmitterId, EOliveIRNiagaraStage::ParticleUpdate,
 *       "Add Velocity");
 *   FNiagaraSetParameterResult Result = Writer.SetParameter(System, ModuleId, "Velocity", "0,0,500");
 */
class OLIVEAIEDITOR_API FOliveNiagaraWriter
{
public:
	/** Get the singleton instance */
	static FOliveNiagaraWriter& Get();

	// =========================================================================
	// Phase 1: System / Emitter / Module Creation
	// =========================================================================

	/**
	 * Create a new Niagara system asset at the given content path.
	 * The system is initialized with a default emitter via UNiagaraSystemFactoryNew.
	 * @param AssetPath Content path for the new system (e.g., "/Game/VFX/NS_Fire")
	 * @return Created system, or nullptr on failure
	 */
	UNiagaraSystem* CreateSystem(const FString& AssetPath);

	/**
	 * Add an emitter to a system from the emitter library or as a new empty emitter.
	 * @param System Target Niagara system
	 * @param SourceEmitterPath Asset path of a source emitter to copy from. If empty, adds an empty emitter.
	 * @param EmitterName Display name for the new emitter
	 * @return Emitter ID (e.g., "emitter_2") or empty string on failure
	 */
	FString AddEmitter(UNiagaraSystem* System,
		const FString& SourceEmitterPath, const FString& EmitterName);

	/**
	 * Add a module to an emitter's stage stack. The module is looked up first via
	 * the module catalog (for display name matching), then by direct asset path.
	 * @param System Target Niagara system
	 * @param EmitterId Emitter ID (e.g., "emitter_0") or "system" for system-level stages
	 * @param Stage Which stage stack to add the module to
	 * @param ModuleScriptPath Module name or full script asset path
	 * @param InsertIndex Position in the stack (-1 for end)
	 * @return Module ID (e.g., "emitter_0.ParticleUpdate.module_3") or empty on failure
	 */
	FString AddModule(UNiagaraSystem* System,
		const FString& EmitterId, EOliveIRNiagaraStage Stage,
		const FString& ModuleScriptPath, int32 InsertIndex = -1);

	/**
	 * Remove a module from an emitter's stage stack.
	 * @param System Target Niagara system
	 * @param EmitterId Emitter ID containing the module
	 * @param ModuleId Full module ID (e.g., "emitter_0.ParticleUpdate.module_2")
	 * @return True if removed successfully
	 */
	bool RemoveModule(UNiagaraSystem* System,
		const FString& EmitterId, const FString& ModuleId);

	/**
	 * Trigger async compilation of a Niagara system.
	 * @param System Target Niagara system
	 * @return Compile result with status and summary
	 */
	FNiagaraCompileResult Compile(UNiagaraSystem* System);

	/**
	 * Load a Niagara system by content path.
	 * @param AssetPath Content path (e.g., "/Game/VFX/NS_Fire")
	 * @return Loaded system, or nullptr if not found
	 */
	UNiagaraSystem* LoadSystem(const FString& AssetPath) const;

	// =========================================================================
	// Phase 2: Parameter Manipulation
	// =========================================================================

	/**
	 * Set a module parameter by name using the Rapid Iteration Parameter (RIP) path.
	 *
	 * The RIP path writes parameter overrides directly to the stage script's
	 * RapidIterationParameters store, bypassing the ViewModel layer. This is the
	 * same approach Epic uses in NiagaraEmitterFactoryNew.cpp for setting module
	 * defaults at creation time.
	 *
	 * Supported types for RIP: float, int32, FVector2f, FVector3f, FVector4f, FLinearColor.
	 * Bool and enum types are not yet supported (they use the override pin path).
	 *
	 * @param System Target Niagara system
	 * @param ModuleId Full module ID (e.g., "emitter_0.ParticleUpdate.module_2")
	 * @param ParameterName Name of the parameter to set (e.g., "Velocity", "Lifetime")
	 * @param Value String value to set. Format depends on type:
	 *              float: "1.5", int32: "42", FVector3f: "100,200,300" or "(X=100,Y=200,Z=300)",
	 *              FLinearColor: "1,0,0,1" or "(R=1,G=0,B=0,A=1)"
	 * @return Result with success status, value set, type, and recompile requirement
	 */
	FNiagaraSetParameterResult SetParameter(UNiagaraSystem* System,
		const FString& ModuleId, const FString& ParameterName, const FString& Value);

	/**
	 * Get all parameters (inputs) for a module, including their types, defaults,
	 * and current override values.
	 *
	 * Enumerates the module's input pins and checks the stage script's
	 * RapidIterationParameters store for any existing overrides.
	 *
	 * @param System Target Niagara system
	 * @param ModuleId Full module ID (e.g., "emitter_0.ParticleUpdate.module_2")
	 * @return Array of parameter info structs, empty on failure
	 */
	TArray<FNiagaraModuleParameterInfo> GetModuleParameters(UNiagaraSystem* System,
		const FString& ModuleId);

	/**
	 * Set an emitter-level property via UObject reflection on UNiagaraEmitter.
	 *
	 * Uses FProperty::ImportText to set the value. The emitter is located by
	 * parsing the emitter ID, and the property is found via reflection on
	 * UNiagaraEmitter::StaticClass(). Common properties include SimTarget,
	 * CalculateBoundsMode, etc.
	 *
	 * @param System Target Niagara system
	 * @param EmitterId Emitter ID (e.g., "emitter_0")
	 * @param PropertyName Name of the property on UNiagaraEmitter
	 * @param Value Value string in UE ImportText format
	 * @return True if the property was set successfully
	 */
	bool SetEmitterProperty(UNiagaraSystem* System,
		const FString& EmitterId, const FString& PropertyName, const FString& Value);

private:
	FOliveNiagaraWriter() = default;
	~FOliveNiagaraWriter() = default;
	FOliveNiagaraWriter(const FOliveNiagaraWriter&) = delete;
	FOliveNiagaraWriter& operator=(const FOliveNiagaraWriter&) = delete;

	// =========================================================================
	// Emitter / Stage Resolution (shared between Phase 1 and Phase 2)
	// =========================================================================

	/**
	 * Resolve an emitter string ID (e.g., "emitter_0") to an index in the
	 * system's EmitterHandles array.
	 * @param System Target system
	 * @param EmitterId String ID to resolve
	 * @return Index into GetEmitterHandles(), or INDEX_NONE on failure
	 */
	int32 ResolveEmitterIndex(UNiagaraSystem* System, const FString& EmitterId) const;

	/**
	 * Convert our IR stage enum to the string name of the stage, used for
	 * building module IDs.
	 * @param Stage IR stage enum value
	 * @return Stage name string (e.g., "ParticleUpdate", "EmitterSpawn")
	 */
	static FString StageToString(EOliveIRNiagaraStage Stage);

	/**
	 * Convert a stage name string back to our IR stage enum.
	 * @param StageName Stage string (e.g., "ParticleUpdate", "SystemSpawn")
	 * @return Corresponding stage enum, or EOliveIRNiagaraStage::Unknown
	 */
	static EOliveIRNiagaraStage StringToStage(const FString& StageName);

	/**
	 * Save the package containing the given system to disk.
	 * @param System System whose package to save
	 * @return True if saved successfully
	 */
	bool SaveSystemPackage(UNiagaraSystem* System) const;

	// =========================================================================
	// Phase 2: Module Location Parsing and Node Resolution
	// =========================================================================

	/**
	 * Parsed representation of a module ID. Encodes the emitter index,
	 * stage, module index within the stage's stack, and whether the module
	 * lives at the system level.
	 */
	struct FModuleLocation
	{
		/** Emitter index in GetEmitterHandles(), or -1 for system-level */
		int32 EmitterIndex = -1;

		/** Which stage the module belongs to */
		EOliveIRNiagaraStage Stage = EOliveIRNiagaraStage::Unknown;

		/** Zero-based module index within the stage's ordered module list */
		int32 ModuleIndex = -1;

		/** True if the module is at the system level (not emitter-level) */
		bool bIsSystemLevel = false;
	};

	/**
	 * Parse a module ID string into its constituent parts.
	 * Expected formats: "emitter_N.StageName.module_M" or "system.StageName.module_M"
	 * @param ModuleId The module ID string to parse
	 * @param OutLocation Parsed location struct
	 * @return True if parsing succeeded
	 */
	bool ParseModuleId(const FString& ModuleId, FModuleLocation& OutLocation) const;

	/**
	 * Find the UNiagaraNodeFunctionCall representing a module at the given location.
	 * Uses FNiagaraEditorUtilities and FNiagaraStackGraphUtilities to navigate
	 * the system's graph structure and locate the ordered module node.
	 * @param System Target system
	 * @param Location Parsed module location from ParseModuleId()
	 * @return The module's function call node, or nullptr if not found
	 */
	UNiagaraNodeFunctionCall* FindModuleNode(UNiagaraSystem* System,
		const FModuleLocation& Location) const;

	/**
	 * Get the UNiagaraScript for a given stage on a specific emitter (or system).
	 * For emitter-level stages, uses FNiagaraEditorUtilities::GetScriptFromSystem().
	 * For system-level stages, uses System->GetSystemSpawnScript()/GetSystemUpdateScript().
	 * @param System Target system
	 * @param EmitterIndex Emitter index (ignored for system-level stages)
	 * @param Stage Which stage to get the script for
	 * @param bIsSystemLevel True if requesting a system-level stage script
	 * @return The stage's UNiagaraScript, or nullptr if not found
	 */
	UNiagaraScript* GetStageScript(UNiagaraSystem* System,
		int32 EmitterIndex, EOliveIRNiagaraStage Stage, bool bIsSystemLevel) const;

	/**
	 * Get the unique emitter name used as the key prefix for RIP parameters.
	 * The unique name disambiguates emitters that may share the same display name
	 * and is required by FNiagaraStackGraphUtilities::CreateRapidIterationParameter().
	 * @param System Target system
	 * @param EmitterIndex Emitter index in GetEmitterHandles()
	 * @return Unique emitter name string, or empty on failure
	 */
	FString GetUniqueEmitterName(UNiagaraSystem* System, int32 EmitterIndex) const;

	/**
	 * Map a type name string to a Niagara FNiagaraTypeDefinition.
	 * Supports: "float", "int32"/"int", "bool", "FVector2f"/"vec2",
	 * "FVector3f"/"vec3"/"vector", "FVector4f"/"vec4",
	 * "FLinearColor"/"color".
	 * @param TypeName Type name to resolve
	 * @return Corresponding FNiagaraTypeDefinition, or float def as fallback
	 */
	static FNiagaraTypeDefinition ResolveType(const FString& TypeName);

	/**
	 * Set a Rapid Iteration Parameter on a stage script for a given module.
	 * Follows Epic's RIP pattern from NiagaraEmitterFactoryNew.cpp:
	 *   CreateModuleParameterHandle -> CreateAliasedModuleParameterHandle ->
	 *   CreateRapidIterationParameter -> SetValue -> SetParameterData
	 *
	 * @param UniqueEmitterName The unique emitter name for parameter key construction
	 * @param Script The stage script that owns the RIP store
	 * @param ModuleNode The module's function call node (needed for aliased handle)
	 * @param ParamName Name of the parameter (e.g., "Velocity")
	 * @param Type The Niagara type definition for this parameter
	 * @param Value String value to parse and set based on Type
	 * @return True if the parameter was set successfully
	 */
	bool SetRapidIterationParam(const FString& UniqueEmitterName,
		UNiagaraScript* Script, UNiagaraNodeFunctionCall* ModuleNode,
		const FString& ParamName, const FNiagaraTypeDefinition& Type, const FString& Value);
};
