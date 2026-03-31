// Copyright Bode Software. All Rights Reserved.

/**
 * OliveNiagaraReader.cpp
 *
 * Implementation of the Niagara system reader. Walks UNiagaraSystem assets
 * to produce IR representations of system structure.
 *
 * Key API path for reading a stage's modules:
 *   Direct script access via FVersionedNiagaraEmitterData properties
 *   UNiagaraScriptSource::NodeGraph -> GetNodesOfClass<UNiagaraNodeOutput>
 *   UNiagaraScriptSource::NodeGraph -> GetNodesOfClass<UNiagaraNodeFunctionCall>
 *
 * NOTE: FNiagaraEditorUtilities::GetScriptFromSystem(),
 * FNiagaraEditorUtilities::GetScriptOutputNode(), and
 * FNiagaraStackGraphUtilities::GetOrderedModuleNodes() are NOT exported
 * (not NIAGARAEDITOR_API) in UE 5.5, so we use direct data member access
 * and manual graph walks instead.
 */

#include "OliveNiagaraReader.h"

#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraCommon.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveNiagaraReader, Log, All);

// ============================================================================
// Anonymous namespace: Replacements for non-exported Niagara editor utilities
// ============================================================================

namespace
{

/**
 * Find the output node for a given script usage in a Niagara graph.
 * Replaces FNiagaraEditorUtilities::GetScriptOutputNode() which is not
 * exported from the NiagaraEditor module.
 *
 * @param Script The Niagara script to find the output node for
 * @return The output node matching the script's usage, or nullptr
 */
UNiagaraNodeOutput* FindScriptOutputNode(UNiagaraScript* Script)
{
	if (!Script)
	{
		return nullptr;
	}

	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Source || !Source->NodeGraph)
	{
		return nullptr;
	}

	const ENiagaraScriptUsage ScriptUsage = Script->GetUsage();

	TArray<UNiagaraNodeOutput*> OutputNodes;
	Source->NodeGraph->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);

	for (UNiagaraNodeOutput* Node : OutputNodes)
	{
		if (Node && Node->GetUsage() == ScriptUsage)
		{
			return Node;
		}
	}

	return nullptr;
}

/**
 * Get ordered module (function call) nodes from a Niagara graph's output node.
 * Replaces FNiagaraStackGraphUtilities::GetOrderedModuleNodes() which is not
 * exported from the NiagaraEditor module.
 *
 * Enumerates all UNiagaraNodeFunctionCall nodes in the graph and sorts them
 * by Y position to approximate the visual stack order.
 *
 * @param Script The Niagara script whose graph to walk
 * @param OutModuleNodes Output array of ordered module function call nodes
 */
void GetOrderedModuleNodesFromScript(UNiagaraScript* Script,
	TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes)
{
	OutModuleNodes.Reset();

	if (!Script)
	{
		return;
	}

	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Source || !Source->NodeGraph)
	{
		return;
	}

	TArray<UNiagaraNodeFunctionCall*> AllFunctionNodes;
	Source->NodeGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(AllFunctionNodes);

	// Filter to only module nodes (nodes that have a FunctionScript that is a
	// Module usage script). Dynamic inputs and other function calls are excluded.
	for (UNiagaraNodeFunctionCall* FuncNode : AllFunctionNodes)
	{
		if (!FuncNode)
		{
			continue;
		}

		// A module node has a FunctionScript with Module usage, or it may be a
		// script-less function call used as a module placeholder. The simplest
		// heuristic: include nodes that have a FunctionScript (module scripts),
		// and exclude nodes that appear to be dynamic inputs (which typically
		// have no script or are nested inside other function calls).
		if (FuncNode->FunctionScript)
		{
			OutModuleNodes.Add(FuncNode);
		}
	}

	// Sort by Y position to get visual stack order (top-to-bottom = first-to-last)
	OutModuleNodes.Sort([](const UNiagaraNodeFunctionCall& A, const UNiagaraNodeFunctionCall& B)
	{
		return A.NodePosY < B.NodePosY;
	});
}

/**
 * Get the UNiagaraScript for a given emitter stage using direct data member access.
 * Replaces FNiagaraEditorUtilities::GetScriptFromSystem() which is not exported.
 *
 * @param EmitterData The versioned emitter data for the target emitter
 * @param Stage The IR stage to get the script for
 * @return The stage's UNiagaraScript, or nullptr if not found
 */
UNiagaraScript* GetEmitterStageScript(FVersionedNiagaraEmitterData* EmitterData,
	EOliveIRNiagaraStage Stage)
{
	if (!EmitterData)
	{
		return nullptr;
	}

	switch (Stage)
	{
		case EOliveIRNiagaraStage::EmitterSpawn:  return EmitterData->EmitterSpawnScriptProps.Script;
		case EOliveIRNiagaraStage::EmitterUpdate: return EmitterData->EmitterUpdateScriptProps.Script;
		case EOliveIRNiagaraStage::ParticleSpawn: return EmitterData->SpawnScriptProps.Script;
		case EOliveIRNiagaraStage::ParticleUpdate: return EmitterData->UpdateScriptProps.Script;
		default: return nullptr;
	}
}

} // anonymous namespace

// ============================================================================
// Singleton
// ============================================================================

FOliveNiagaraReader& FOliveNiagaraReader::Get()
{
	static FOliveNiagaraReader Instance;
	return Instance;
}

// ============================================================================
// Public: ReadSystem (asset path)
// ============================================================================

TOptional<FOliveIRNiagaraSystem> FOliveNiagaraReader::ReadSystem(const FString& AssetPath)
{
	UNiagaraSystem* System = LoadSystem(AssetPath);
	if (!System)
	{
		UE_LOG(LogOliveNiagaraReader, Warning, TEXT("Failed to load Niagara system: %s"), *AssetPath);
		return {};
	}

	return ReadSystem(System);
}

// ============================================================================
// Public: ReadSystem (loaded pointer)
// ============================================================================

TOptional<FOliveIRNiagaraSystem> FOliveNiagaraReader::ReadSystem(UNiagaraSystem* System)
{
	if (!System)
	{
		UE_LOG(LogOliveNiagaraReader, Warning, TEXT("ReadSystem called with null system"));
		return {};
	}

	FOliveIRNiagaraSystem Result;
	Result.Name = System->GetName();
	Result.Path = System->GetPathName();

	// Compile status: true if no outstanding compilation requests
	Result.bCompileStatus = !System->HasOutstandingCompilationRequests();

	// System-level modules (SystemSpawn, SystemUpdate)
	Result.SystemModules = SerializeSystemModules(System);

	// Walk emitter handles
	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System->GetEmitterHandles();
	for (int32 i = 0; i < EmitterHandles.Num(); ++i)
	{
		Result.Emitters.Add(SerializeEmitter(System, i));
	}

	UE_LOG(LogOliveNiagaraReader, Log,
		TEXT("Read Niagara system '%s': %d emitters, %d system modules"),
		*Result.Name, Result.Emitters.Num(), Result.SystemModules.Num());

	return Result;
}

// ============================================================================
// Private: LoadSystem
// ============================================================================

UNiagaraSystem* FOliveNiagaraReader::LoadSystem(const FString& AssetPath) const
{
	return Cast<UNiagaraSystem>(
		StaticLoadObject(UNiagaraSystem::StaticClass(), nullptr, *AssetPath));
}

// ============================================================================
// Private: SerializeEmitter
// ============================================================================

FOliveIRNiagaraEmitter FOliveNiagaraReader::SerializeEmitter(
	UNiagaraSystem* System, int32 EmitterIndex) const
{
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (!ensure(EmitterIndex >= 0 && EmitterIndex < Handles.Num()))
	{
		UE_LOG(LogOliveNiagaraReader, Error,
			TEXT("EmitterIndex %d out of range (system has %d emitters)"),
			EmitterIndex, Handles.Num());
		return FOliveIRNiagaraEmitter();
	}

	const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];

	FOliveIRNiagaraEmitter Result;
	Result.Id = FString::Printf(TEXT("emitter_%d"), EmitterIndex);
	Result.Name = Handle.GetName().ToString();
	Result.bEnabled = Handle.GetIsEnabled();

	// Get versioned emitter data (UE 5.4+ versioning)
	FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
	if (!EmitterData)
	{
		UE_LOG(LogOliveNiagaraReader, Warning,
			TEXT("Emitter '%s' (index %d) has no emitter data"), *Result.Name, EmitterIndex);
		return Result;
	}

	// Source emitter path (if added from emitter library)
	// NOTE: Verify exact API in UE 5.5 — GetParent() or GetParentEmitter() may differ
	const UNiagaraEmitter* SourceEmitter = Handle.GetInstance().Emitter;
	if (SourceEmitter)
	{
		Result.SourceEmitterPath = SourceEmitter->GetPathName();
	}

	// Serialize modules for each emitter-level stage
	const FGuid EmitterHandleId = Handle.GetId();

	static const EOliveIRNiagaraStage EmitterStages[] = {
		EOliveIRNiagaraStage::EmitterSpawn,
		EOliveIRNiagaraStage::EmitterUpdate,
		EOliveIRNiagaraStage::ParticleSpawn,
		EOliveIRNiagaraStage::ParticleUpdate
	};

	for (EOliveIRNiagaraStage Stage : EmitterStages)
	{
		TArray<FOliveIRNiagaraModule> StageModules = SerializeStageModules(
			System, EmitterHandleId, Result.Id, Stage);
		Result.Modules.Append(StageModules);
	}

	// Serialize renderers
	Result.Renderers = SerializeRenderers(EmitterData);

	UE_LOG(LogOliveNiagaraReader, Verbose,
		TEXT("  Emitter '%s': %d modules, %d renderers"),
		*Result.Name, Result.Modules.Num(), Result.Renderers.Num());

	return Result;
}

// ============================================================================
// Private: SerializeStageModules
// ============================================================================

TArray<FOliveIRNiagaraModule> FOliveNiagaraReader::SerializeStageModules(
	UNiagaraSystem* System,
	FGuid EmitterHandleId,
	const FString& EmitterId,
	EOliveIRNiagaraStage Stage) const
{
	TArray<FOliveIRNiagaraModule> Result;

	// Resolve emitter data from the handle ID to access stage scripts directly.
	// This replaces FNiagaraEditorUtilities::GetScriptFromSystem() which is not
	// exported in UE 5.5.
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	for (const FNiagaraEmitterHandle& Handle : Handles)
	{
		if (Handle.GetId() == EmitterHandleId)
		{
			EmitterData = Handle.GetEmitterData();
			break;
		}
	}

	if (!EmitterData)
	{
		UE_LOG(LogOliveNiagaraReader, Verbose,
			TEXT("No emitter data found for handle on emitter %s"), *EmitterId);
		return Result;
	}

	// Get the script for this stage via direct data member access
	UNiagaraScript* Script = GetEmitterStageScript(EmitterData, Stage);
	if (!Script)
	{
		// Not all stages may have scripts (e.g., an emitter may not have EmitterSpawn)
		return Result;
	}

	// Get ordered module nodes via manual graph walk
	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	GetOrderedModuleNodesFromScript(Script, ModuleNodes);

	const FString StageName = GetStageName(Stage);

	for (int32 ModuleIdx = 0; ModuleIdx < ModuleNodes.Num(); ++ModuleIdx)
	{
		UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIdx];
		if (!ModuleNode)
		{
			continue;
		}

		FOliveIRNiagaraModule IRModule;
		IRModule.Id = FString::Printf(TEXT("%s.%s.module_%d"),
			*EmitterId, *StageName, ModuleIdx);
		IRModule.Stage = Stage;
		IRModule.StackIndex = ModuleIdx;
		IRModule.bEnabled = ModuleNode->IsNodeEnabled();

		// Module display name
		IRModule.ModuleName = ModuleNode->GetNodeTitle(ENodeTitleType::ListView).ToString();

		// Script asset path (the underlying module script)
		if (ModuleNode->FunctionScript)
		{
			IRModule.ScriptAssetPath = ModuleNode->FunctionScript->GetPathName();
		}

		// Serialize module parameters/inputs
		IRModule.Parameters = SerializeModuleParameters(ModuleNode);

		Result.Add(MoveTemp(IRModule));
	}

	return Result;
}

// ============================================================================
// Private: SerializeSystemModules
// ============================================================================

TArray<FOliveIRNiagaraModule> FOliveNiagaraReader::SerializeSystemModules(
	UNiagaraSystem* System) const
{
	TArray<FOliveIRNiagaraModule> Result;

	static const EOliveIRNiagaraStage SystemStages[] = {
		EOliveIRNiagaraStage::SystemSpawn,
		EOliveIRNiagaraStage::SystemUpdate
	};

	for (EOliveIRNiagaraStage Stage : SystemStages)
	{
		const FString StageName = GetStageName(Stage);

		// Use direct script accessors for system-level stages
		UNiagaraScript* Script = nullptr;

		if (Stage == EOliveIRNiagaraStage::SystemSpawn)
		{
			Script = System->GetSystemSpawnScript();
		}
		else if (Stage == EOliveIRNiagaraStage::SystemUpdate)
		{
			Script = System->GetSystemUpdateScript();
		}

		if (!Script)
		{
			continue;
		}

		// Get ordered module nodes via manual graph walk (replaces the
		// non-exported FNiagaraStackGraphUtilities::GetOrderedModuleNodes
		// and FNiagaraEditorUtilities::GetScriptOutputNode)
		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		GetOrderedModuleNodesFromScript(Script, ModuleNodes);

		for (int32 ModuleIdx = 0; ModuleIdx < ModuleNodes.Num(); ++ModuleIdx)
		{
			UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIdx];
			if (!ModuleNode)
			{
				continue;
			}

			FOliveIRNiagaraModule IRModule;
			IRModule.Id = FString::Printf(TEXT("system.%s.module_%d"),
				*StageName, ModuleIdx);
			IRModule.Stage = Stage;
			IRModule.StackIndex = ModuleIdx;
			IRModule.bEnabled = ModuleNode->IsNodeEnabled();
			IRModule.ModuleName = ModuleNode->GetNodeTitle(ENodeTitleType::ListView).ToString();

			if (ModuleNode->FunctionScript)
			{
				IRModule.ScriptAssetPath = ModuleNode->FunctionScript->GetPathName();
			}

			IRModule.Parameters = SerializeModuleParameters(ModuleNode);

			Result.Add(MoveTemp(IRModule));
		}
	}

	return Result;
}

// ============================================================================
// Private: SerializeRenderers
// ============================================================================

TArray<FOliveIRNiagaraRenderer> FOliveNiagaraReader::SerializeRenderers(
	FVersionedNiagaraEmitterData* EmitterData) const
{
	TArray<FOliveIRNiagaraRenderer> Result;

	if (!EmitterData)
	{
		return Result;
	}

	const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();

	for (const UNiagaraRendererProperties* Renderer : Renderers)
	{
		if (!Renderer)
		{
			continue;
		}

		FOliveIRNiagaraRenderer IRRenderer;

		// Get a clean renderer type name
		// e.g., "NiagaraSpriteRendererProperties" -> "SpriteRenderer"
		FString ClassName = Renderer->GetClass()->GetName();

		// Strip "Niagara" prefix and "Properties" suffix for cleaner display
		static const FString NiagaraPrefix = TEXT("Niagara");
		static const FString PropertiesSuffix = TEXT("Properties");

		if (ClassName.StartsWith(NiagaraPrefix))
		{
			ClassName.RightChopInline(NiagaraPrefix.Len());
		}
		if (ClassName.EndsWith(PropertiesSuffix))
		{
			ClassName.LeftChopInline(PropertiesSuffix.Len());
		}

		IRRenderer.RendererType = ClassName;
		IRRenderer.bEnabled = Renderer->GetIsEnabled();

		// Serialize renderer-specific properties via reflection
		SerializeRendererProperties(Renderer, IRRenderer.Properties);

		Result.Add(MoveTemp(IRRenderer));
	}

	return Result;
}

// ============================================================================
// Private: SerializeRendererProperties
// ============================================================================

void FOliveNiagaraReader::SerializeRendererProperties(
	const UNiagaraRendererProperties* RendererProps,
	TMap<FString, FString>& OutProperties) const
{
	if (!RendererProps)
	{
		return;
	}

	const UClass* RendererClass = RendererProps->GetClass();

	for (TFieldIterator<FProperty> PropIt(RendererClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;

		// Skip properties from base UNiagaraRendererProperties and above
		if (Property->GetOwnerClass() == UNiagaraRendererProperties::StaticClass() ||
			Property->GetOwnerClass() == UObject::StaticClass())
		{
			continue;
		}

		// Only include editable properties
		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		FString ValueStr;
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RendererProps);
		Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);

		if (!ValueStr.IsEmpty())
		{
			OutProperties.Add(Property->GetName(), ValueStr);
		}
	}
}

// ============================================================================
// Private: SerializeModuleParameters
// ============================================================================

TArray<FOliveIRNiagaraParameter> FOliveNiagaraReader::SerializeModuleParameters(
	const UNiagaraNodeFunctionCall* ModuleNode) const
{
	TArray<FOliveIRNiagaraParameter> Result;

	if (!ModuleNode)
	{
		return Result;
	}

	// Module parameters are exposed through the module's input pins.
	// Each input pin on a UNiagaraNodeFunctionCall corresponds to a
	// module parameter that can be overridden.
	for (const UEdGraphPin* Pin : ModuleNode->GetAllPins())
	{
		if (!Pin || Pin->Direction != EGPD_Input)
		{
			continue;
		}

		// Skip the parameter map input pin (internal wiring, not a user parameter)
		// The parameter map pin typically has a specific Niagara type name
		if (Pin->PinType.PinCategory == TEXT("NiagaraParameterMap"))
		{
			continue;
		}

		FOliveIRNiagaraParameter Param;
		Param.Name = Pin->GetName();
		Param.TypeName = Pin->PinType.PinCategory.ToString();

		// If there's a sub-category type (e.g., struct types), capture it
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			Param.TypeName = Pin->PinType.PinSubCategoryObject->GetName();
		}

		// Default value from the pin
		Param.DefaultValue = Pin->DefaultValue;

		// Check if this parameter has an override (a linked input or modified default)
		// A pin with connections has been linked to another parameter
		if (Pin->LinkedTo.Num() > 0)
		{
			Param.bIsOverridden = true;
			// For linked pins, the override value is the linked parameter name
			if (Pin->LinkedTo[0])
			{
				Param.OverrideValue = FString::Printf(TEXT("Linked:%s"),
					*Pin->LinkedTo[0]->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
		}
		else if (!Pin->DefaultValue.IsEmpty() && Pin->DefaultValue != Pin->AutogeneratedDefaultValue)
		{
			// User has set a non-default literal value
			Param.bIsOverridden = true;
			Param.OverrideValue = Pin->DefaultValue;
		}

		Result.Add(MoveTemp(Param));
	}

	// If the pin-based approach yields no parameters, try reading from the
	// module's underlying script graph. The script's variables represent
	// the full parameter interface of the module.
	if (Result.Num() == 0 && ModuleNode->FunctionScript)
	{
		UNiagaraScriptSource* ScriptSource =
			Cast<UNiagaraScriptSource>(ModuleNode->FunctionScript->GetLatestSource());
		if (ScriptSource && ScriptSource->NodeGraph)
		{
			// NOTE: Verify exact API in UE 5.5 -- GetAllMetaData() or
			// ScriptSource->NodeGraph->GetParameters() may be the right accessor
			// for reading module input definitions. The input pins approach
			// above should be the primary path.
			UE_LOG(LogOliveNiagaraReader, Verbose,
				TEXT("Module '%s' has no input pins -- parameters may require "
					 "script variable traversal"),
				*ModuleNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
		}
	}

	return Result;
}

// ============================================================================
// Static: MapScriptUsage
// ============================================================================

EOliveIRNiagaraStage FOliveNiagaraReader::MapScriptUsage(int32 Usage)
{
	// Cast from int32 to ENiagaraScriptUsage for safe comparison
	const ENiagaraScriptUsage ScriptUsage = static_cast<ENiagaraScriptUsage>(Usage);

	switch (ScriptUsage)
	{
	case ENiagaraScriptUsage::SystemSpawnScript:    return EOliveIRNiagaraStage::SystemSpawn;
	case ENiagaraScriptUsage::SystemUpdateScript:   return EOliveIRNiagaraStage::SystemUpdate;
	case ENiagaraScriptUsage::EmitterSpawnScript:   return EOliveIRNiagaraStage::EmitterSpawn;
	case ENiagaraScriptUsage::EmitterUpdateScript:  return EOliveIRNiagaraStage::EmitterUpdate;
	case ENiagaraScriptUsage::ParticleSpawnScript:  return EOliveIRNiagaraStage::ParticleSpawn;
	case ENiagaraScriptUsage::ParticleUpdateScript: return EOliveIRNiagaraStage::ParticleUpdate;
	default: return EOliveIRNiagaraStage::Unknown;
	}
}

// ============================================================================
// Static: MapStageToScriptUsage
// ============================================================================

int32 FOliveNiagaraReader::MapStageToScriptUsage(EOliveIRNiagaraStage Stage)
{
	switch (Stage)
	{
	case EOliveIRNiagaraStage::SystemSpawn:
		return static_cast<int32>(ENiagaraScriptUsage::SystemSpawnScript);
	case EOliveIRNiagaraStage::SystemUpdate:
		return static_cast<int32>(ENiagaraScriptUsage::SystemUpdateScript);
	case EOliveIRNiagaraStage::EmitterSpawn:
		return static_cast<int32>(ENiagaraScriptUsage::EmitterSpawnScript);
	case EOliveIRNiagaraStage::EmitterUpdate:
		return static_cast<int32>(ENiagaraScriptUsage::EmitterUpdateScript);
	case EOliveIRNiagaraStage::ParticleSpawn:
		return static_cast<int32>(ENiagaraScriptUsage::ParticleSpawnScript);
	case EOliveIRNiagaraStage::ParticleUpdate:
		return static_cast<int32>(ENiagaraScriptUsage::ParticleUpdateScript);
	default:
		return static_cast<int32>(ENiagaraScriptUsage::ParticleUpdateScript);
	}
}

// ============================================================================
// Static: GetStageName
// ============================================================================

FString FOliveNiagaraReader::GetStageName(EOliveIRNiagaraStage Stage)
{
	switch (Stage)
	{
	case EOliveIRNiagaraStage::SystemSpawn:    return TEXT("SystemSpawn");
	case EOliveIRNiagaraStage::SystemUpdate:   return TEXT("SystemUpdate");
	case EOliveIRNiagaraStage::EmitterSpawn:   return TEXT("EmitterSpawn");
	case EOliveIRNiagaraStage::EmitterUpdate:  return TEXT("EmitterUpdate");
	case EOliveIRNiagaraStage::ParticleSpawn:  return TEXT("ParticleSpawn");
	case EOliveIRNiagaraStage::ParticleUpdate: return TEXT("ParticleUpdate");
	default:                                   return TEXT("Unknown");
	}
}
