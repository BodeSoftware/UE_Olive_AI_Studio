// Copyright Bode Software. All Rights Reserved.

/**
 * OliveNiagaraWriter.cpp
 *
 * Implementation of FOliveNiagaraWriter -- handles creation and mutation of
 * Niagara system assets. All write operations are wrapped in FScopedTransaction.
 *
 * Niagara's stack model: System -> Emitter -> Stage -> Module stack.
 * We address emitters as "emitter_N" and modules as "emitter_N.Stage.module_M"
 * (or "system.Stage.module_M" for system-level modules), matching the reader's
 * output IDs for round-trip consistency.
 *
 * Key Niagara API surfaces used:
 *   - UNiagaraSystem: GetEmitterHandles(), AddEmitterHandle(), RequestCompile(),
 *     GetSystemSpawnScript(), GetSystemUpdateScript()
 *   - FNiagaraEmitterHandle: GetEmitterData() -> FVersionedNiagaraEmitterData
 *   - FVersionedNiagaraEmitterData: direct stage script access via
 *     EmitterSpawnScriptProps.Script, EmitterUpdateScriptProps.Script,
 *     SpawnScriptProps.Script, UpdateScriptProps.Script
 *   - FNiagaraStackGraphUtilities: AddScriptModuleToStack() (the ONLY exported
 *     function from this class — all others cause linker errors)
 *   - UNiagaraSystemFactoryNew: InitializeSystem()
 *   - Manual graph walks: GetNodesOfClass<UNiagaraNodeFunctionCall>() sorted
 *     by Y position replaces non-exported GetOrderedModuleNodes()
 *   - UEdGraphNode::DestroyNode() replaces non-exported RemoveModuleFromStack()
 *
 * Phase 2 (parameter manipulation) is STUBBED because FNiagaraParameterHandle
 * and FNiagaraStackGraphUtilities::CreateRapidIterationParameter() are not
 * exported in UE 5.5. SetParameter/GetModuleParameters return descriptive
 * errors directing users to editor.run_python as an alternative.
 */

#include "Writer/OliveNiagaraWriter.h"
#include "Catalog/OliveNiagaraModuleCatalog.h"

// Niagara engine headers
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraSystemFactoryNew.h"

// NOTE: FNiagaraEditorUtilities and FNiagaraStackGraphUtilities headers are
// intentionally NOT included. Most functions in those classes are not exported
// (not NIAGARAEDITOR_API) in UE 5.5, causing linker errors. We use direct
// data member access and manual graph walks instead.
// The one exception is FNiagaraStackGraphUtilities::AddScriptModuleToStack()
// which IS exported and is accessed via the header below.
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

// Phase 2: RIP parameter support -- stubbed due to non-exported APIs
// FNiagaraParameterHandle and CreateRapidIterationParameter are not exported.
// #include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "NiagaraTypes.h"

// Engine / Editor
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "ScopedTransaction.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveNiagaraWriter, Log, All);

// =============================================================================
// Anonymous namespace: value parsing helpers for RIP parameters
// =============================================================================

namespace
{

/**
 * Parse a vector-style string into an array of float components.
 * Accepts both "X,Y,Z" and "(X=1,Y=2,Z=3)" formats.
 * @param Value Input string
 * @param OutComponents Output float array
 * @param ExpectedCount Expected number of components (2, 3, or 4)
 * @return True if parsing succeeded with the expected component count
 */
bool ParseVectorComponents(const FString& Value, TArray<float>& OutComponents, int32 ExpectedCount)
{
	OutComponents.Reset();

	FString Trimmed = Value.TrimStartAndEnd();

	// Try named format: "(X=1.0,Y=2.0,Z=3.0)" or "(R=1,G=0,B=0,A=1)"
	if (Trimmed.StartsWith(TEXT("(")))
	{
		Trimmed.RemoveFromStart(TEXT("("));
		Trimmed.RemoveFromEnd(TEXT(")"));

		TArray<FString> Parts;
		Trimmed.ParseIntoArray(Parts, TEXT(","));

		for (const FString& Part : Parts)
		{
			FString TrimmedPart = Part.TrimStartAndEnd();
			int32 EqualsIdx;
			if (TrimmedPart.FindChar('=', EqualsIdx))
			{
				FString NumStr = TrimmedPart.Mid(EqualsIdx + 1).TrimStartAndEnd();
				OutComponents.Add(FCString::Atof(*NumStr));
			}
			else
			{
				// No '=' sign, try parsing as plain number
				OutComponents.Add(FCString::Atof(*TrimmedPart));
			}
		}
	}
	else
	{
		// CSV format: "1.0,2.0,3.0"
		TArray<FString> Parts;
		Trimmed.ParseIntoArray(Parts, TEXT(","));

		for (const FString& Part : Parts)
		{
			OutComponents.Add(FCString::Atof(*Part.TrimStartAndEnd()));
		}
	}

	if (OutComponents.Num() != ExpectedCount)
	{
		UE_LOG(LogOliveNiagaraWriter, Warning,
			TEXT("ParseVectorComponents: Expected %d components but got %d from '%s'"),
			ExpectedCount, OutComponents.Num(), *Value);
		return false;
	}

	return true;
}

/**
 * Map ENiagaraScriptUsage from our IR stage enum.
 * Centralizes the stage->usage mapping used by multiple methods.
 */
ENiagaraScriptUsage StageToScriptUsage(EOliveIRNiagaraStage Stage)
{
	switch (Stage)
	{
		case EOliveIRNiagaraStage::SystemSpawn:    return ENiagaraScriptUsage::SystemSpawnScript;
		case EOliveIRNiagaraStage::SystemUpdate:   return ENiagaraScriptUsage::SystemUpdateScript;
		case EOliveIRNiagaraStage::EmitterSpawn:   return ENiagaraScriptUsage::EmitterSpawnScript;
		case EOliveIRNiagaraStage::EmitterUpdate:  return ENiagaraScriptUsage::EmitterUpdateScript;
		case EOliveIRNiagaraStage::ParticleSpawn:  return ENiagaraScriptUsage::ParticleSpawnScript;
		case EOliveIRNiagaraStage::ParticleUpdate: return ENiagaraScriptUsage::ParticleUpdateScript;
		default:                                    return ENiagaraScriptUsage::ParticleUpdateScript;
	}
}

/**
 * Get a human-readable type name from a Niagara pin type.
 * Attempts to resolve structured types (vectors, colors) from the
 * PinSubCategoryObject, falling back to PinCategory.
 */
FString GetTypeNameFromPin(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return TEXT("unknown");
	}

	if (Pin->PinType.PinSubCategoryObject.IsValid())
	{
		return Pin->PinType.PinSubCategoryObject->GetName();
	}

	return Pin->PinType.PinCategory.ToString();
}

/**
 * Find the output node for a given script usage in a Niagara graph.
 * Replaces FNiagaraEditorUtilities::GetScriptOutputNode() which is not
 * exported from the NiagaraEditor module in UE 5.5.
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
 * Get ordered module (function call) nodes from a Niagara script's graph.
 * Replaces FNiagaraStackGraphUtilities::GetOrderedModuleNodes() which is not
 * exported from the NiagaraEditor module in UE 5.5.
 *
 * Enumerates all UNiagaraNodeFunctionCall nodes with a FunctionScript and
 * sorts by Y position to approximate the visual stack order.
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

	for (UNiagaraNodeFunctionCall* FuncNode : AllFunctionNodes)
	{
		if (FuncNode && FuncNode->FunctionScript)
		{
			OutModuleNodes.Add(FuncNode);
		}
	}

	// Sort by Y position to get visual stack order
	OutModuleNodes.Sort([](const UNiagaraNodeFunctionCall& A, const UNiagaraNodeFunctionCall& B)
	{
		return A.NodePosY < B.NodePosY;
	});
}

/**
 * Get the UNiagaraScript for a given emitter stage using direct data member access.
 * Replaces FNiagaraEditorUtilities::GetScriptFromSystem() which is not exported.
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

// =============================================================================
// Singleton
// =============================================================================

FOliveNiagaraWriter& FOliveNiagaraWriter::Get()
{
	static FOliveNiagaraWriter Instance;
	return Instance;
}

// =============================================================================
// CreateSystem
// =============================================================================

UNiagaraSystem* FOliveNiagaraWriter::CreateSystem(const FString& AssetPath)
{
	// --- Parse package and asset name from the content path ---
	FString PackageName = AssetPath;
	FString AssetName;

	int32 LastSlash;
	if (PackageName.FindLastChar('/', LastSlash))
	{
		AssetName = PackageName.Mid(LastSlash + 1);
	}
	else
	{
		AssetName = PackageName;
	}

	if (AssetName.IsEmpty())
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("CreateSystem: Empty asset name from path '%s'"), *AssetPath);
		return nullptr;
	}

	// --- Create the package ---
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("CreateSystem: Failed to create package '%s'"), *PackageName);
		return nullptr;
	}

	Package->FullyLoad();

	// --- Transaction ---
	const FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Olive AI: Create Niagara System '%s'"), *AssetName)));

	// --- Create the Niagara system object ---
	UNiagaraSystem* NewSystem = NewObject<UNiagaraSystem>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);

	if (!NewSystem)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("CreateSystem: Failed to create UNiagaraSystem object"));
		return nullptr;
	}

	// --- Initialize with defaults (adds a default emitter) ---
	// NOTE: Verify exact API in UE 5.5. UNiagaraSystemFactoryNew::InitializeSystem()
	// is the standard factory initialization path. The second parameter controls whether
	// a default emitter is added.
	UNiagaraSystemFactoryNew::InitializeSystem(NewSystem, /*bAddDefaultEmitter=*/true);

	// --- Register with asset registry ---
	FAssetRegistryModule::AssetCreated(NewSystem);

	// --- Save ---
	Package->MarkPackageDirty();
	SaveSystemPackage(NewSystem);

	UE_LOG(LogOliveNiagaraWriter, Log,
		TEXT("CreateSystem: Created Niagara system '%s' at '%s'"),
		*AssetName, *AssetPath);

	return NewSystem;
}

// =============================================================================
// AddEmitter
// =============================================================================

FString FOliveNiagaraWriter::AddEmitter(UNiagaraSystem* System,
	const FString& SourceEmitterPath, const FString& EmitterName)
{
	if (!System)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("AddEmitter: System is null"));
		return FString();
	}

	// --- Determine the emitter name ---
	FString FinalEmitterName = EmitterName;
	if (FinalEmitterName.IsEmpty())
	{
		FinalEmitterName = FString::Printf(TEXT("Emitter_%d"),
			System->GetEmitterHandles().Num());
	}

	// --- Transaction ---
	const FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Olive AI: Add Emitter '%s'"), *FinalEmitterName)));

	System->Modify();

	// --- Load source emitter (if provided) ---
	if (!SourceEmitterPath.IsEmpty())
	{
		// NOTE: Verify exact API in UE 5.5. AddEmitterHandle may take a UNiagaraEmitter
		// directly or require loading via asset data.
		UNiagaraEmitter* SourceEmitter = Cast<UNiagaraEmitter>(
			StaticLoadObject(UNiagaraEmitter::StaticClass(), nullptr, *SourceEmitterPath));

		if (!SourceEmitter)
		{
			UE_LOG(LogOliveNiagaraWriter, Warning,
				TEXT("AddEmitter: Source emitter not found at '%s', creating empty emitter instead"),
				*SourceEmitterPath);
		}
		else
		{
			// NOTE: Verify exact API in UE 5.5. The signature for AddEmitterHandle may
			// differ (e.g., it may take FName instead of FString, or UNiagaraEmitter& vs *).
			FNiagaraEmitterHandle NewHandle = System->AddEmitterHandle(
				*SourceEmitter, FName(*FinalEmitterName), FGuid::NewGuid());

			int32 NewIndex = System->GetEmitterHandles().Num() - 1;
			FString EmitterId = FString::Printf(TEXT("emitter_%d"), NewIndex);

			UE_LOG(LogOliveNiagaraWriter, Log,
				TEXT("AddEmitter: Added emitter '%s' from source '%s' as %s"),
				*FinalEmitterName, *SourceEmitterPath, *EmitterId);

			System->GetPackage()->MarkPackageDirty();
			return EmitterId;
		}
	}

	// --- Empty emitter (no source or source load failed) ---
	// A bare NewObject<UNiagaraEmitter> has no scripts/graphs and will crash
	// AddEmitterHandle. Use Niagara's default template emitter instead.
	static const TCHAR* DefaultEmitterPaths[] = {
		TEXT("/Niagara/DefaultAssets/Templates/SimpleSpriteBurst"),
		TEXT("/Niagara/DefaultAssets/Templates/SimpleSpriteEmitter"),
		TEXT("/Niagara/DefaultEmitters/SimpleSpriteBurst"),
	};

	UNiagaraEmitter* DefaultEmitter = nullptr;
	for (const TCHAR* Path : DefaultEmitterPaths)
	{
		DefaultEmitter = Cast<UNiagaraEmitter>(
			StaticLoadObject(UNiagaraEmitter::StaticClass(), nullptr, Path));
		if (DefaultEmitter)
		{
			break;
		}
	}

	if (!DefaultEmitter)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("AddEmitter: Could not find any default Niagara emitter template. "
			     "Provide a source_emitter path instead."));
		return FString();
	}

	FNiagaraEmitterHandle NewHandle = System->AddEmitterHandle(
		*DefaultEmitter, FName(*FinalEmitterName), FGuid::NewGuid());

	int32 NewIndex = System->GetEmitterHandles().Num() - 1;
	FString EmitterId = FString::Printf(TEXT("emitter_%d"), NewIndex);

	System->GetPackage()->MarkPackageDirty();

	UE_LOG(LogOliveNiagaraWriter, Log,
		TEXT("AddEmitter: Added emitter '%s' from default template as %s"),
		*FinalEmitterName, *EmitterId);

	return EmitterId;
}

// =============================================================================
// AddModule
// =============================================================================

FString FOliveNiagaraWriter::AddModule(UNiagaraSystem* System,
	const FString& EmitterId, EOliveIRNiagaraStage Stage,
	const FString& ModuleScriptPath, int32 InsertIndex)
{
	if (!System)
	{
		UE_LOG(LogOliveNiagaraWriter, Error, TEXT("AddModule: System is null"));
		return FString();
	}

	if (Stage == EOliveIRNiagaraStage::Unknown)
	{
		UE_LOG(LogOliveNiagaraWriter, Error, TEXT("AddModule: Stage is Unknown"));
		return FString();
	}

	// --- Validate emitter ID ---
	const bool bIsSystemLevel = EmitterId.Equals(TEXT("system"), ESearchCase::IgnoreCase);
	int32 EmitterIndex = INDEX_NONE;

	if (!bIsSystemLevel)
	{
		EmitterIndex = ResolveEmitterIndex(System, EmitterId);
		if (EmitterIndex == INDEX_NONE)
		{
			UE_LOG(LogOliveNiagaraWriter, Error,
				TEXT("AddModule: Invalid emitter ID '%s'"), *EmitterId);
			return FString();
		}

		// Validate stage is emitter-level
		if (Stage == EOliveIRNiagaraStage::SystemSpawn ||
			Stage == EOliveIRNiagaraStage::SystemUpdate)
		{
			UE_LOG(LogOliveNiagaraWriter, Error,
				TEXT("AddModule: Cannot add system-level stage '%s' to emitter '%s'. Use emitter_id=\"system\" for system stages."),
				*StageToString(Stage), *EmitterId);
			return FString();
		}
	}
	else
	{
		// System-level: validate stage is SystemSpawn or SystemUpdate
		if (Stage != EOliveIRNiagaraStage::SystemSpawn &&
			Stage != EOliveIRNiagaraStage::SystemUpdate)
		{
			UE_LOG(LogOliveNiagaraWriter, Error,
				TEXT("AddModule: System-level target only accepts SystemSpawn/SystemUpdate, got '%s'"),
				*StageToString(Stage));
			return FString();
		}
	}

	// --- Find the module script ---
	UNiagaraScript* ModuleScript = FOliveNiagaraModuleCatalog::Get().FindModuleScript(ModuleScriptPath);
	if (!ModuleScript)
	{
		// Try direct asset load as fallback
		ModuleScript = Cast<UNiagaraScript>(
			StaticLoadObject(UNiagaraScript::StaticClass(), nullptr, *ModuleScriptPath));
	}

	if (!ModuleScript)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("AddModule: Module script not found: '%s'. "
				"Use niagara.list_modules to discover available modules."),
			*ModuleScriptPath);
		return FString();
	}

	// --- Map our stage enum to ENiagaraScriptUsage ---
	// NOTE: Verify exact API in UE 5.5. ENiagaraScriptUsage values:
	//   ParticleSpawnScript = 1, ParticleUpdateScript = 2,
	//   EmitterSpawnScript = 5, EmitterUpdateScript = 6,
	//   SystemSpawnScript = 9, SystemUpdateScript = 10
	// We need the output node for the target stage to add the module into its graph.

	// --- Get the stage's script source and output node ---
	// NOTE: Verify exact API in UE 5.5. The approach to getting the correct
	// UNiagaraGraph and its output node varies by engine version. The stack-based
	// approach via FNiagaraStackGraphUtilities is preferred.

	// We need the emitter handle ID (FGuid) for stack graph utilities
	FGuid EmitterHandleId;
	if (!bIsSystemLevel)
	{
		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		if (Handles.IsValidIndex(EmitterIndex))
		{
			EmitterHandleId = Handles[EmitterIndex].GetId();
		}
		else
		{
			UE_LOG(LogOliveNiagaraWriter, Error,
				TEXT("AddModule: Emitter index %d out of range"), EmitterIndex);
			return FString();
		}
	}

	// Determine ENiagaraScriptUsage for the stage
	// NOTE: Verify exact API in UE 5.5 -- enum values and the approach to
	// locating the output node for a given stage script usage.
	ENiagaraScriptUsage ScriptUsage;
	switch (Stage)
	{
		case EOliveIRNiagaraStage::SystemSpawn:   ScriptUsage = ENiagaraScriptUsage::SystemSpawnScript;   break;
		case EOliveIRNiagaraStage::SystemUpdate:  ScriptUsage = ENiagaraScriptUsage::SystemUpdateScript;  break;
		case EOliveIRNiagaraStage::EmitterSpawn:  ScriptUsage = ENiagaraScriptUsage::EmitterSpawnScript;  break;
		case EOliveIRNiagaraStage::EmitterUpdate: ScriptUsage = ENiagaraScriptUsage::EmitterUpdateScript; break;
		case EOliveIRNiagaraStage::ParticleSpawn: ScriptUsage = ENiagaraScriptUsage::ParticleSpawnScript; break;
		case EOliveIRNiagaraStage::ParticleUpdate:ScriptUsage = ENiagaraScriptUsage::ParticleUpdateScript;break;
		default:
			UE_LOG(LogOliveNiagaraWriter, Error, TEXT("AddModule: Unhandled stage"));
			return FString();
	}

	// --- Locate the script and output node for the target stage ---
	// Use direct data member access (replaces non-exported
	// FNiagaraEditorUtilities::GetScriptFromSystem and GetScriptOutputNode).
	UNiagaraScript* StageScript = nullptr;

	if (bIsSystemLevel)
	{
		if (Stage == EOliveIRNiagaraStage::SystemSpawn)
		{
			StageScript = System->GetSystemSpawnScript();
		}
		else if (Stage == EOliveIRNiagaraStage::SystemUpdate)
		{
			StageScript = System->GetSystemUpdateScript();
		}
	}
	else
	{
		const TArray<FNiagaraEmitterHandle>& AllHandles = System->GetEmitterHandles();
		if (AllHandles.IsValidIndex(EmitterIndex))
		{
			FVersionedNiagaraEmitterData* EmitterData = AllHandles[EmitterIndex].GetEmitterData();
			StageScript = GetEmitterStageScript(EmitterData, Stage);
		}
	}

	UNiagaraNodeOutput* OutputNode = FindScriptOutputNode(StageScript);

	if (!OutputNode)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("AddModule: Could not find output node for stage '%s' on '%s'"),
			*StageToString(Stage), *EmitterId);
		return FString();
	}

	// --- Transaction ---
	const FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Olive AI: Add Module '%s' to %s.%s"),
			*ModuleScriptPath, *EmitterId, *StageToString(Stage))));

	System->Modify();

	// --- Add the module to the stack ---
	// NOTE: Verify exact API in UE 5.5. AddScriptModuleToStack signature may be:
	//   (UNiagaraScript* ModuleScript, UNiagaraNodeOutput& TargetOutputNode, int32 TargetIndex)
	// Returns the newly created UNiagaraNodeFunctionCall node.
	UNiagaraNodeFunctionCall* NewModuleNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript, *OutputNode, InsertIndex);

	if (!NewModuleNode)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("AddModule: FNiagaraStackGraphUtilities::AddScriptModuleToStack failed for '%s'"),
			*ModuleScriptPath);
		return FString();
	}

	// --- Build the module ID ---
	// Count existing function call nodes in the stage graph to determine the index.
	// The module's position in the stack determines its index.
	int32 ModuleIndex = 0;
	UNiagaraGraph* Graph = OutputNode->GetNiagaraGraph();
	if (Graph)
	{
		TArray<UNiagaraNodeFunctionCall*> FunctionCallNodes;
		Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(FunctionCallNodes);

		// Count function calls that are module calls (not dynamic inputs, etc.)
		// and find our new node's position
		int32 ModuleCount = 0;
		for (UNiagaraNodeFunctionCall* FuncNode : FunctionCallNodes)
		{
			if (FuncNode == NewModuleNode)
			{
				ModuleIndex = ModuleCount;
			}
			// NOTE: Verify exact API in UE 5.5. There may be a way to filter
			// only "module" function calls vs. dynamic inputs. For now, count all.
			ModuleCount++;
		}
	}

	FString ModuleId = FString::Printf(TEXT("%s.%s.module_%d"),
		*EmitterId, *StageToString(Stage), ModuleIndex);

	System->GetPackage()->MarkPackageDirty();

	UE_LOG(LogOliveNiagaraWriter, Log,
		TEXT("AddModule: Added '%s' as %s (insert index=%d)"),
		*ModuleScriptPath, *ModuleId, InsertIndex);

	return ModuleId;
}

// =============================================================================
// RemoveModule
// =============================================================================

bool FOliveNiagaraWriter::RemoveModule(UNiagaraSystem* System,
	const FString& EmitterId, const FString& ModuleId)
{
	if (!System)
	{
		UE_LOG(LogOliveNiagaraWriter, Error, TEXT("RemoveModule: System is null"));
		return false;
	}

	// --- Parse the module ID ---
	// Expected format: "emitter_N.StageName.module_M" or "system.StageName.module_M"
	TArray<FString> Parts;
	ModuleId.ParseIntoArray(Parts, TEXT("."));

	if (Parts.Num() < 3)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("RemoveModule: Invalid module ID format '%s'. "
				"Expected 'emitter_N.StageName.module_M' or 'system.StageName.module_M'"),
			*ModuleId);
		return false;
	}

	const FString& OwnerPart = Parts[0]; // "emitter_N" or "system"
	const FString& StagePart = Parts[1]; // "ParticleUpdate", etc.
	const FString& ModulePart = Parts[2]; // "module_M"

	// Parse module index from "module_M"
	int32 ModuleIndex = INDEX_NONE;
	if (ModulePart.StartsWith(TEXT("module_")))
	{
		FString IndexStr = ModulePart.Mid(7); // len("module_") = 7
		if (IndexStr.IsNumeric())
		{
			ModuleIndex = FCString::Atoi(*IndexStr);
		}
	}

	if (ModuleIndex == INDEX_NONE || ModuleIndex < 0)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("RemoveModule: Could not parse module index from '%s'"), *ModulePart);
		return false;
	}

	// --- Determine stage ---
	EOliveIRNiagaraStage Stage = StringToStage(StagePart);

	if (Stage == EOliveIRNiagaraStage::Unknown)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("RemoveModule: Unknown stage '%s' in module ID '%s'"),
			*StagePart, *ModuleId);
		return false;
	}

	// --- Resolve emitter ---
	const bool bIsSystemLevel = OwnerPart.Equals(TEXT("system"), ESearchCase::IgnoreCase);

	if (!bIsSystemLevel)
	{
		int32 EmitterIndex = ResolveEmitterIndex(System, OwnerPart);
		if (EmitterIndex == INDEX_NONE)
		{
			UE_LOG(LogOliveNiagaraWriter, Error,
				TEXT("RemoveModule: Invalid emitter ID '%s'"), *OwnerPart);
			return false;
		}
	}

	// --- Get the stage script using direct data member access ---
	// Replaces non-exported FNiagaraEditorUtilities::GetScriptFromSystem
	UNiagaraScript* StageScript = nullptr;

	if (bIsSystemLevel)
	{
		if (Stage == EOliveIRNiagaraStage::SystemSpawn)
		{
			StageScript = System->GetSystemSpawnScript();
		}
		else if (Stage == EOliveIRNiagaraStage::SystemUpdate)
		{
			StageScript = System->GetSystemUpdateScript();
		}
	}
	else
	{
		int32 EmitterIndex = ResolveEmitterIndex(System, OwnerPart);
		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		if (Handles.IsValidIndex(EmitterIndex))
		{
			FVersionedNiagaraEmitterData* EmitterData = Handles[EmitterIndex].GetEmitterData();
			StageScript = GetEmitterStageScript(EmitterData, Stage);
		}
	}

	if (!StageScript)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("RemoveModule: Could not find stage script for stage '%s'"),
			*StagePart);
		return false;
	}

	// --- Find the target module node by index using ordered list ---
	// Replaces non-exported FNiagaraStackGraphUtilities::GetOrderedModuleNodes
	TArray<UNiagaraNodeFunctionCall*> OrderedModules;
	GetOrderedModuleNodesFromScript(StageScript, OrderedModules);

	if (!OrderedModules.IsValidIndex(ModuleIndex))
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("RemoveModule: Module index %d out of range (found %d modules in %s)"),
			ModuleIndex, OrderedModules.Num(), *StagePart);
		return false;
	}

	UNiagaraNodeFunctionCall* TargetModuleNode = OrderedModules[ModuleIndex];

	// --- Transaction ---
	const FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Olive AI: Remove Module '%s'"), *ModuleId)));

	System->Modify();

	// --- Remove the module from the graph ---
	// Replaces non-exported FNiagaraStackGraphUtilities::RemoveModuleFromStack.
	// UEdGraphNode::DestroyNode() removes the node from its graph and cleans up
	// all pin connections.
	TargetModuleNode->Modify();
	TargetModuleNode->DestroyNode();

	System->GetPackage()->MarkPackageDirty();

	UE_LOG(LogOliveNiagaraWriter, Log,
		TEXT("RemoveModule: Removed module '%s'"), *ModuleId);

	return true;
}

// =============================================================================
// Compile
// =============================================================================

FNiagaraCompileResult FOliveNiagaraWriter::Compile(UNiagaraSystem* System)
{
	FNiagaraCompileResult Result;

	if (!System)
	{
		Result.bSuccess = false;
		Result.bAsync = false;
		Result.Summary = TEXT("System is null");
		return Result;
	}

	// NOTE: Verify exact API in UE 5.5. RequestCompile() triggers async compilation.
	// The parameter typically controls whether to force a full recompile.
	System->RequestCompile(/*bForceCompile=*/false);

	Result.bSuccess = true;
	Result.bAsync = true;
	Result.Summary = FString::Printf(
		TEXT("Compilation started (async) for system '%s'"),
		*System->GetName());

	UE_LOG(LogOliveNiagaraWriter, Log, TEXT("Compile: %s"), *Result.Summary);

	return Result;
}

// =============================================================================
// LoadSystem
// =============================================================================

UNiagaraSystem* FOliveNiagaraWriter::LoadSystem(const FString& AssetPath) const
{
	return Cast<UNiagaraSystem>(
		StaticLoadObject(UNiagaraSystem::StaticClass(), nullptr, *AssetPath));
}

// =============================================================================
// Phase 2: SetParameter
// =============================================================================

FNiagaraSetParameterResult FOliveNiagaraWriter::SetParameter(UNiagaraSystem* System,
	const FString& ModuleId, const FString& ParameterName, const FString& Value)
{
	FNiagaraSetParameterResult Result;
	Result.ParameterName = ParameterName;

	// Phase 2 parameter setting requires FNiagaraParameterHandle and
	// FNiagaraStackGraphUtilities::CreateRapidIterationParameter, which are
	// NOT exported (not NIAGARAEDITOR_API) in UE 5.5. This prevents us from
	// building the RIP parameter key needed to write to the parameter store.
	//
	// Use editor.run_python as an alternative for setting module parameters.
	Result.ErrorMessage = FString::Printf(
		TEXT("niagara.set_parameter is not yet available: the Niagara Rapid Iteration Parameter "
			 "APIs (FNiagaraParameterHandle, CreateRapidIterationParameter) are not exported in "
			 "UE 5.5. Use editor.run_python to set module parameters via Python scripting instead. "
			 "Module: '%s', Parameter: '%s', Value: '%s'"),
		*ModuleId, *ParameterName, *Value);

	UE_LOG(LogOliveNiagaraWriter, Warning,
		TEXT("SetParameter: Stubbed — RIP APIs not exported in UE 5.5. "
			 "Module='%s', Param='%s', Value='%s'"),
		*ModuleId, *ParameterName, *Value);

	return Result;
}

// =============================================================================
// Phase 2: GetModuleParameters
// =============================================================================

TArray<FNiagaraModuleParameterInfo> FOliveNiagaraWriter::GetModuleParameters(
	UNiagaraSystem* System, const FString& ModuleId)
{
	TArray<FNiagaraModuleParameterInfo> Result;

	if (!System)
	{
		UE_LOG(LogOliveNiagaraWriter, Error, TEXT("GetModuleParameters: System is null"));
		return Result;
	}

	// --- Parse and locate the module ---
	FModuleLocation Location;
	if (!ParseModuleId(ModuleId, Location))
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("GetModuleParameters: Invalid module ID '%s'"), *ModuleId);
		return Result;
	}

	UNiagaraNodeFunctionCall* ModuleNode = FindModuleNode(System, Location);
	if (!ModuleNode)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("GetModuleParameters: Module not found: '%s'"), *ModuleId);
		return Result;
	}

	// --- Enumerate module input pins ---
	// We can enumerate pin names and types without the non-exported RIP APIs.
	// RIP override checking (FNiagaraParameterHandle, CreateRapidIterationParameter)
	// is NOT available due to non-exported APIs. We report pin defaults only.
	for (const UEdGraphPin* Pin : ModuleNode->GetAllPins())
	{
		if (!Pin || Pin->Direction != EGPD_Input)
		{
			continue;
		}

		// Skip internal parameter map pin
		if (Pin->PinType.PinCategory == TEXT("NiagaraParameterMap"))
		{
			continue;
		}

		FNiagaraModuleParameterInfo Info;
		Info.Name = Pin->GetName();
		Info.TypeName = GetTypeNameFromPin(Pin);
		Info.DefaultValue = Pin->DefaultValue;
		Info.bIsRapidIteration = true; // Assume RIP by default

		// NOTE: RIP override value lookup is not available because
		// FNiagaraParameterHandle and CreateRapidIterationParameter are not
		// exported in UE 5.5. We can only report whether a pin has connections.

		// If pin has connections (linked to dynamic input), note that
		if (Pin->LinkedTo.Num() > 0)
		{
			Info.bIsOverridden = true;
			Info.bIsRapidIteration = false; // Linked inputs are not RIP
			if (Pin->LinkedTo[0])
			{
				Info.CurrentValue = FString::Printf(TEXT("Linked:%s"),
					*Pin->LinkedTo[0]->GetOwningNode()->GetNodeTitle(
						ENodeTitleType::ListView).ToString());
			}
		}

		Result.Add(MoveTemp(Info));
	}

	UE_LOG(LogOliveNiagaraWriter, Log,
		TEXT("GetModuleParameters: Found %d parameters on module '%s' "
			 "(RIP override values not available due to non-exported APIs)"),
		Result.Num(), *ModuleId);

	return Result;
}

// =============================================================================
// Phase 2: SetEmitterProperty
// =============================================================================

bool FOliveNiagaraWriter::SetEmitterProperty(UNiagaraSystem* System,
	const FString& EmitterId, const FString& PropertyName, const FString& Value)
{
	if (!System)
	{
		UE_LOG(LogOliveNiagaraWriter, Error, TEXT("SetEmitterProperty: System is null"));
		return false;
	}

	// --- Resolve emitter ---
	int32 EmitterIndex = ResolveEmitterIndex(System, EmitterId);
	if (EmitterIndex == INDEX_NONE)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("SetEmitterProperty: Invalid emitter ID '%s'"), *EmitterId);
		return false;
	}

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	// Access emitter via const GetInstance() — safe across UE 5.5–5.7
	const FVersionedNiagaraEmitter& EmitterInstance = Handles[EmitterIndex].GetInstance();
	UNiagaraEmitter* Emitter = EmitterInstance.Emitter;
	if (!Emitter)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("SetEmitterProperty: Emitter instance is null for '%s'"), *EmitterId);
		return false;
	}

	// --- Find the property via reflection ---
	FProperty* Prop = FindFProperty<FProperty>(UNiagaraEmitter::StaticClass(), *PropertyName);

	// If not found on emitter, try the versioned emitter data
	FVersionedNiagaraEmitterData* EmitterData = nullptr;
	void* PropertyContainer = Emitter;

	if (!Prop)
	{
		EmitterData = Handles[EmitterIndex].GetEmitterData();
		if (EmitterData)
		{
			// FVersionedNiagaraEmitterData is not a UObject, so we need to search
			// its UScriptStruct or iterate manually. For common properties, provide
			// known aliases.
			// DESIGN NOTE: FVersionedNiagaraEmitterData is a plain struct without
			// USTRUCT reflection. Properties on it must be accessed by name-based
			// lookup on UNiagaraEmitter or via specific accessor methods.
			// For now, only UNiagaraEmitter-reflected properties are supported.
			UE_LOG(LogOliveNiagaraWriter, Warning,
				TEXT("SetEmitterProperty: Property '%s' not found on UNiagaraEmitter. "
					 "FVersionedNiagaraEmitterData properties are not yet supported via reflection."),
				*PropertyName);
		}
		else
		{
			UE_LOG(LogOliveNiagaraWriter, Error,
				TEXT("SetEmitterProperty: Property '%s' not found on UNiagaraEmitter"),
				*PropertyName);
		}
		return false;
	}

	// --- Transaction ---
	const FScopedTransaction Transaction(FText::FromString(
		FString::Printf(TEXT("Olive AI: Set Emitter Property '%s' on '%s'"),
			*PropertyName, *EmitterId)));

	Emitter->Modify();

	// --- Set the property value via ImportText ---
	void* PropAddr = Prop->ContainerPtrToValuePtr<void>(PropertyContainer);
	const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, PropAddr, Emitter, PPF_None);

	if (!ImportResult)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("SetEmitterProperty: Failed to import value '%s' for property '%s' on '%s'"),
			*Value, *PropertyName, *EmitterId);
		return false;
	}

	// --- Request recompile after property change ---
	System->RequestCompile(/*bForceCompile=*/false);
	System->GetPackage()->MarkPackageDirty();

	UE_LOG(LogOliveNiagaraWriter, Log,
		TEXT("SetEmitterProperty: Set '%s' = '%s' on emitter '%s'"),
		*PropertyName, *Value, *EmitterId);

	return true;
}

// =============================================================================
// Private Helpers: Phase 1
// =============================================================================

int32 FOliveNiagaraWriter::ResolveEmitterIndex(UNiagaraSystem* System,
	const FString& EmitterId) const
{
	if (!System)
	{
		return INDEX_NONE;
	}

	// Expected format: "emitter_N"
	static const FString EmitterPrefix = TEXT("emitter_");

	if (!EmitterId.StartsWith(EmitterPrefix))
	{
		UE_LOG(LogOliveNiagaraWriter, Warning,
			TEXT("ResolveEmitterIndex: ID '%s' does not start with 'emitter_'"), *EmitterId);
		return INDEX_NONE;
	}

	FString IndexStr = EmitterId.Mid(EmitterPrefix.Len());
	if (!IndexStr.IsNumeric())
	{
		UE_LOG(LogOliveNiagaraWriter, Warning,
			TEXT("ResolveEmitterIndex: Non-numeric index in '%s'"), *EmitterId);
		return INDEX_NONE;
	}

	int32 Index = FCString::Atoi(*IndexStr);
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (!Handles.IsValidIndex(Index))
	{
		UE_LOG(LogOliveNiagaraWriter, Warning,
			TEXT("ResolveEmitterIndex: Index %d out of range (system has %d emitters)"),
			Index, Handles.Num());
		return INDEX_NONE;
	}

	return Index;
}

FString FOliveNiagaraWriter::StageToString(EOliveIRNiagaraStage Stage)
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

EOliveIRNiagaraStage FOliveNiagaraWriter::StringToStage(const FString& StageName)
{
	if (StageName == TEXT("SystemSpawn"))        return EOliveIRNiagaraStage::SystemSpawn;
	if (StageName == TEXT("SystemUpdate"))       return EOliveIRNiagaraStage::SystemUpdate;
	if (StageName == TEXT("EmitterSpawn"))       return EOliveIRNiagaraStage::EmitterSpawn;
	if (StageName == TEXT("EmitterUpdate"))      return EOliveIRNiagaraStage::EmitterUpdate;
	if (StageName == TEXT("ParticleSpawn"))      return EOliveIRNiagaraStage::ParticleSpawn;
	if (StageName == TEXT("ParticleUpdate"))     return EOliveIRNiagaraStage::ParticleUpdate;
	return EOliveIRNiagaraStage::Unknown;
}

bool FOliveNiagaraWriter::SaveSystemPackage(UNiagaraSystem* System) const
{
	if (!System)
	{
		return false;
	}

	UPackage* Package = System->GetPackage();
	if (!Package)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("SaveSystemPackage: System '%s' has no package"), *System->GetName());
		return false;
	}

	FString PackageName = Package->GetName();
	FString PackageFileName = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

	bool bSaved = UPackage::SavePackage(Package, System, *PackageFileName, SaveArgs);
	if (!bSaved)
	{
		UE_LOG(LogOliveNiagaraWriter, Warning,
			TEXT("SaveSystemPackage: Failed to save package '%s'"), *PackageName);
	}

	return bSaved;
}

// =============================================================================
// Private Helpers: Phase 2 -- Module Location
// =============================================================================

bool FOliveNiagaraWriter::ParseModuleId(const FString& ModuleId,
	FModuleLocation& OutLocation) const
{
	// Expected format: "emitter_N.StageName.module_M" or "system.StageName.module_M"
	TArray<FString> Parts;
	ModuleId.ParseIntoArray(Parts, TEXT("."));

	if (Parts.Num() < 3)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("ParseModuleId: Invalid format '%s'. "
				 "Expected 'emitter_N.StageName.module_M' or 'system.StageName.module_M'"),
			*ModuleId);
		return false;
	}

	const FString& OwnerPart = Parts[0];
	const FString& StagePart = Parts[1];
	const FString& ModulePart = Parts[2];

	// --- Parse owner (emitter index or system) ---
	OutLocation.bIsSystemLevel = OwnerPart.Equals(TEXT("system"), ESearchCase::IgnoreCase);

	if (!OutLocation.bIsSystemLevel)
	{
		static const FString EmitterPrefix = TEXT("emitter_");
		if (!OwnerPart.StartsWith(EmitterPrefix))
		{
			UE_LOG(LogOliveNiagaraWriter, Error,
				TEXT("ParseModuleId: Owner part '%s' is not 'system' or 'emitter_N'"),
				*OwnerPart);
			return false;
		}

		FString IndexStr = OwnerPart.Mid(EmitterPrefix.Len());
		if (!IndexStr.IsNumeric())
		{
			UE_LOG(LogOliveNiagaraWriter, Error,
				TEXT("ParseModuleId: Non-numeric emitter index in '%s'"), *OwnerPart);
			return false;
		}

		OutLocation.EmitterIndex = FCString::Atoi(*IndexStr);
	}

	// --- Parse stage ---
	OutLocation.Stage = StringToStage(StagePart);
	if (OutLocation.Stage == EOliveIRNiagaraStage::Unknown)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("ParseModuleId: Unknown stage '%s'"), *StagePart);
		return false;
	}

	// --- Parse module index ---
	static const FString ModulePrefix = TEXT("module_");
	if (!ModulePart.StartsWith(ModulePrefix))
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("ParseModuleId: Module part '%s' does not start with 'module_'"),
			*ModulePart);
		return false;
	}

	FString ModuleIndexStr = ModulePart.Mid(ModulePrefix.Len());
	if (!ModuleIndexStr.IsNumeric())
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("ParseModuleId: Non-numeric module index in '%s'"), *ModulePart);
		return false;
	}

	OutLocation.ModuleIndex = FCString::Atoi(*ModuleIndexStr);
	if (OutLocation.ModuleIndex < 0)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("ParseModuleId: Negative module index in '%s'"), *ModulePart);
		return false;
	}

	return true;
}

UNiagaraNodeFunctionCall* FOliveNiagaraWriter::FindModuleNode(
	UNiagaraSystem* System, const FModuleLocation& Location) const
{
	if (!System)
	{
		return nullptr;
	}

	// --- Get the stage script ---
	UNiagaraScript* Script = GetStageScript(
		System, Location.EmitterIndex, Location.Stage, Location.bIsSystemLevel);

	if (!Script)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("FindModuleNode: No script for stage '%s' (emitter idx=%d, system=%d)"),
			*StageToString(Location.Stage), Location.EmitterIndex,
			Location.bIsSystemLevel ? 1 : 0);
		return nullptr;
	}

	// --- Get ordered module nodes via manual graph walk ---
	// Replaces non-exported FNiagaraEditorUtilities::GetScriptOutputNode
	// and FNiagaraStackGraphUtilities::GetOrderedModuleNodes.
	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	GetOrderedModuleNodesFromScript(Script, ModuleNodes);

	if (!ModuleNodes.IsValidIndex(Location.ModuleIndex))
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("FindModuleNode: Module index %d out of range (stage '%s' has %d modules)"),
			Location.ModuleIndex, *StageToString(Location.Stage), ModuleNodes.Num());
		return nullptr;
	}

	return ModuleNodes[Location.ModuleIndex];
}

UNiagaraScript* FOliveNiagaraWriter::GetStageScript(UNiagaraSystem* System,
	int32 EmitterIndex, EOliveIRNiagaraStage Stage, bool bIsSystemLevel) const
{
	if (!System)
	{
		return nullptr;
	}

	if (bIsSystemLevel)
	{
		switch (Stage)
		{
			case EOliveIRNiagaraStage::SystemSpawn:
				return System->GetSystemSpawnScript();
			case EOliveIRNiagaraStage::SystemUpdate:
				return System->GetSystemUpdateScript();
			default:
				UE_LOG(LogOliveNiagaraWriter, Error,
					TEXT("GetStageScript: Stage '%s' is not a system-level stage"),
					*StageToString(Stage));
				return nullptr;
		}
	}

	// Emitter-level: use direct data member access on FVersionedNiagaraEmitterData.
	// Replaces non-exported FNiagaraEditorUtilities::GetScriptFromSystem.
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	if (!Handles.IsValidIndex(EmitterIndex))
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("GetStageScript: Emitter index %d out of range (system has %d emitters)"),
			EmitterIndex, Handles.Num());
		return nullptr;
	}

	FVersionedNiagaraEmitterData* EmitterData = Handles[EmitterIndex].GetEmitterData();
	return GetEmitterStageScript(EmitterData, Stage);
}

FString FOliveNiagaraWriter::GetUniqueEmitterName(UNiagaraSystem* System,
	int32 EmitterIndex) const
{
	if (!System)
	{
		return FString();
	}

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	if (!Handles.IsValidIndex(EmitterIndex))
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("GetUniqueEmitterName: Emitter index %d out of range"),
			EmitterIndex);
		return FString();
	}

	// Access emitter via const GetInstance() — safe across UE 5.5–5.7
	const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
	const FVersionedNiagaraEmitter& EmitterInstance = Handle.GetInstance();
	UNiagaraEmitter* Emitter = EmitterInstance.Emitter;

	if (!Emitter)
	{
		UE_LOG(LogOliveNiagaraWriter, Error,
			TEXT("GetUniqueEmitterName: Emitter instance is null at index %d"),
			EmitterIndex);
		return FString();
	}

	return Emitter->GetUniqueEmitterName();
}

// =============================================================================
// Private Helpers: Phase 2 -- Type Resolution
// =============================================================================

FNiagaraTypeDefinition FOliveNiagaraWriter::ResolveType(const FString& TypeName)
{
	FString Lower = TypeName.ToLower();

	// Scalar types
	if (Lower == TEXT("float") || Lower == TEXT("niagara float"))
	{
		return FNiagaraTypeDefinition::GetFloatDef();
	}
	if (Lower == TEXT("int32") || Lower == TEXT("int") || Lower == TEXT("niagara int32"))
	{
		return FNiagaraTypeDefinition::GetIntDef();
	}
	if (Lower == TEXT("bool") || Lower == TEXT("niagara bool"))
	{
		return FNiagaraTypeDefinition::GetBoolDef();
	}

	// Vector types (Niagara uses float-precision vectors: FVector2f, FVector3f, FVector4f)
	if (Lower == TEXT("fvector2f") || Lower == TEXT("vector2f") || Lower == TEXT("vec2"))
	{
		return FNiagaraTypeDefinition::GetVec2Def();
	}
	if (Lower == TEXT("fvector3f") || Lower == TEXT("vector3f") || Lower == TEXT("vec3") ||
		Lower == TEXT("vector") || Lower == TEXT("fvector"))
	{
		return FNiagaraTypeDefinition::GetVec3Def();
	}
	if (Lower == TEXT("fvector4f") || Lower == TEXT("vector4f") || Lower == TEXT("vec4") ||
		Lower == TEXT("fvector4"))
	{
		return FNiagaraTypeDefinition::GetVec4Def();
	}

	// Color
	if (Lower == TEXT("flinearcolor") || Lower == TEXT("linearcolor") || Lower == TEXT("color"))
	{
		return FNiagaraTypeDefinition::GetColorDef();
	}

	// Fallback: return float definition with a warning
	UE_LOG(LogOliveNiagaraWriter, Warning,
		TEXT("ResolveType: Unknown type '%s', falling back to float"), *TypeName);
	return FNiagaraTypeDefinition::GetFloatDef();
}

// =============================================================================
// Private Helpers: Phase 2 -- RIP Parameter Setting
// =============================================================================

bool FOliveNiagaraWriter::SetRapidIterationParam(const FString& UniqueEmitterName,
	UNiagaraScript* Script, UNiagaraNodeFunctionCall* ModuleNode,
	const FString& ParamName, const FNiagaraTypeDefinition& Type, const FString& Value)
{
	// Stubbed: FNiagaraParameterHandle::CreateModuleParameterHandle(),
	// FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(), and
	// FNiagaraStackGraphUtilities::CreateRapidIterationParameter() are NOT
	// exported (not NIAGARAEDITOR_API) in UE 5.5. Without these, we cannot
	// construct the RIP variable key needed to write to the parameter store.
	//
	// Workaround: Use editor.run_python to set parameters via Python scripting.
	UE_LOG(LogOliveNiagaraWriter, Warning,
		TEXT("SetRapidIterationParam: Stubbed — RIP APIs not exported in UE 5.5. "
			 "Param='%s', Value='%s'"),
		*ParamName, *Value);
	return false;
}
