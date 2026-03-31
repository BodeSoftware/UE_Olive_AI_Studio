// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CommonIR.h"
#include "NiagaraIR.generated.h"

/**
 * Niagara module stage — identifies where a module lives in the stack hierarchy.
 */
UENUM(BlueprintType)
enum class EOliveIRNiagaraStage : uint8
{
	SystemSpawn,
	SystemUpdate,
	EmitterSpawn,
	EmitterUpdate,
	ParticleSpawn,
	ParticleUpdate,
	Unknown
};

/**
 * IR representation of a single parameter (input) on a Niagara module.
 * Captures both the default value and any user-applied override.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRNiagaraParameter
{
	GENERATED_BODY()

	/** Parameter name as displayed in the stack */
	UPROPERTY()
	FString Name;

	/** Type name (e.g., "float", "FVector", "FLinearColor", "ENiagaraExecutionState") */
	UPROPERTY()
	FString TypeName;

	/** Default value in ExportText format */
	UPROPERTY()
	FString DefaultValue;

	/** Current override value, empty if using default */
	UPROPERTY()
	FString OverrideValue;

	/** Whether this parameter has been overridden from its default */
	UPROPERTY()
	bool bIsOverridden = false;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRNiagaraParameter FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * IR representation of a Niagara module instance within a stage stack.
 * A module is a script placed at a specific position in a stage's module stack.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRNiagaraModule
{
	GENERATED_BODY()

	/** Unique ID within the system (e.g., "emitter_0.ParticleUpdate.module_2" or "system.SystemUpdate.module_0") */
	UPROPERTY()
	FString Id;

	/** Display name (e.g., "Add Velocity", "Solve Forces and Velocity") */
	UPROPERTY()
	FString ModuleName;

	/** Asset path of the module script (e.g., /Niagara/Modules/AddVelocity) */
	UPROPERTY()
	FString ScriptAssetPath;

	/** Which stage this module belongs to */
	UPROPERTY()
	EOliveIRNiagaraStage Stage = EOliveIRNiagaraStage::Unknown;

	/** Position in the stage's module stack (0-based) */
	UPROPERTY()
	int32 StackIndex = 0;

	/** Whether this module is enabled */
	UPROPERTY()
	bool bEnabled = true;

	/** All exposed parameters on this module */
	UPROPERTY()
	TArray<FOliveIRNiagaraParameter> Parameters;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRNiagaraModule FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * IR representation of a Niagara renderer (e.g., Sprite, Mesh, Ribbon, Light).
 * Properties are stored as a flat key-value map since renderer types vary widely.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRNiagaraRenderer
{
	GENERATED_BODY()

	/** Renderer type name (e.g., "SpriteRenderer", "MeshRenderer", "RibbonRenderer", "LightRenderer") */
	UPROPERTY()
	FString RendererType;

	/** Renderer properties as key-value pairs */
	UPROPERTY()
	TMap<FString, FString> Properties;

	/** Whether this renderer is enabled */
	UPROPERTY()
	bool bEnabled = true;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRNiagaraRenderer FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * IR representation of a Niagara emitter within a system.
 * Contains all modules across all stages and all renderers attached to this emitter.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRNiagaraEmitter
{
	GENERATED_BODY()

	/** Unique ID within the system (e.g., "emitter_0") */
	UPROPERTY()
	FString Id;

	/** Display name of the emitter */
	UPROPERTY()
	FString Name;

	/** Asset path of the source emitter if added from the emitter library, empty if created inline */
	UPROPERTY()
	FString SourceEmitterPath;

	/** Whether this emitter is enabled in the system */
	UPROPERTY()
	bool bEnabled = true;

	/** All modules across all stages (EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate) */
	UPROPERTY()
	TArray<FOliveIRNiagaraModule> Modules;

	/** All renderers attached to this emitter */
	UPROPERTY()
	TArray<FOliveIRNiagaraRenderer> Renderers;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRNiagaraEmitter FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * IR representation of an entire Niagara System asset.
 * Top-level container holding system-level modules and all emitters.
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRNiagaraSystem
{
	GENERATED_BODY()

	/** System asset name */
	UPROPERTY()
	FString Name;

	/** Asset path (e.g., /Game/VFX/NS_Explosion) */
	UPROPERTY()
	FString Path;

	/** Modules on system-level stages (SystemSpawn, SystemUpdate) */
	UPROPERTY()
	TArray<FOliveIRNiagaraModule> SystemModules;

	/** All emitters in this system */
	UPROPERTY()
	TArray<FOliveIRNiagaraEmitter> Emitters;

	/** Last known compile status (true = compiled successfully) */
	UPROPERTY()
	bool bCompileStatus = false;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRNiagaraSystem FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};
