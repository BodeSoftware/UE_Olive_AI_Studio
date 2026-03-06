// Copyright Bode Software. All Rights Reserved.

/**
 * OliveLibraryCloner.cpp
 *
 * Implements the library template cloner.
 * Phase 1: Structure creation (variables, components, interfaces, dispatchers, function signatures).
 * Phase 2: Graph cloning (classify nodes, create, wire exec/data, set defaults, layout).
 *
 * See plans/library-clone-design.md for full architecture.
 */

#include "Template/OliveLibraryCloner.h"
#include "Template/OliveTemplateSystem.h"

#include "Writer/OliveBlueprintWriter.h"
#include "Writer/OliveComponentWriter.h"
#include "Writer/OliveNodeFactory.h"
#include "Writer/OlivePinConnector.h"
#include "OliveClassResolver.h"
#include "OliveBlueprintTypes.h"
#include "IR/OliveIRTypes.h"
#include "IR/CommonIR.h"
#include "IR/BlueprintIR.h"

#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY(LogOliveLibraryCloner);

// =============================================================================
// Anonymous Namespace Helpers
// =============================================================================

namespace
{
	/** Map library template "type" field to EOliveBlueprintType for CreateBlueprint. */
	EOliveBlueprintType ParseLibraryBlueprintType(const FString& TypeStr)
	{
		const FString Upper = TypeStr.ToUpper();
		if (Upper == TEXT("ACTORCOMPONENT") || Upper == TEXT("ACTOR_COMPONENT") || Upper == TEXT("UNKNOWN"))
		{
			// "Unknown" in library templates often means it was detected as an ActorComponent
			// because the parent is ActorComponent. We let CreateBlueprint infer from parent class.
			// But if the parent is ActorComponent, we want ActorComponent type.
			// This is handled by the caller checking the parent class.
		}
		if (Upper == TEXT("INTERFACE")) return EOliveBlueprintType::Interface;
		if (Upper == TEXT("FUNCTIONLIBRARY") || Upper == TEXT("FUNCTION_LIBRARY"))
			return EOliveBlueprintType::FunctionLibrary;
		if (Upper == TEXT("ACTORCOMPONENT") || Upper == TEXT("ACTOR_COMPONENT"))
			return EOliveBlueprintType::ActorComponent;
		return EOliveBlueprintType::Normal;
	}

	/** Map an IR type category string to EOliveIRTypeCategory. */
	EOliveIRTypeCategory ParseCategoryString(const FString& Category)
	{
		const FString Lower = Category.ToLower();
		if (Lower == TEXT("bool") || Lower == TEXT("boolean")) return EOliveIRTypeCategory::Bool;
		if (Lower == TEXT("byte"))      return EOliveIRTypeCategory::Byte;
		if (Lower == TEXT("int") || Lower == TEXT("integer")) return EOliveIRTypeCategory::Int;
		if (Lower == TEXT("int64"))     return EOliveIRTypeCategory::Int64;
		if (Lower == TEXT("float"))     return EOliveIRTypeCategory::Float;
		if (Lower == TEXT("double"))    return EOliveIRTypeCategory::Double;
		if (Lower == TEXT("string"))    return EOliveIRTypeCategory::String;
		if (Lower == TEXT("name"))      return EOliveIRTypeCategory::Name;
		if (Lower == TEXT("text"))      return EOliveIRTypeCategory::Text;
		if (Lower == TEXT("vector"))    return EOliveIRTypeCategory::Vector;
		if (Lower == TEXT("vector2d"))  return EOliveIRTypeCategory::Vector2D;
		if (Lower == TEXT("rotator"))   return EOliveIRTypeCategory::Rotator;
		if (Lower == TEXT("transform")) return EOliveIRTypeCategory::Transform;
		if (Lower == TEXT("color"))     return EOliveIRTypeCategory::Color;
		if (Lower == TEXT("linearcolor")) return EOliveIRTypeCategory::LinearColor;
		if (Lower == TEXT("object"))    return EOliveIRTypeCategory::Object;
		if (Lower == TEXT("class"))     return EOliveIRTypeCategory::Class;
		if (Lower == TEXT("interface")) return EOliveIRTypeCategory::Interface;
		if (Lower == TEXT("struct"))    return EOliveIRTypeCategory::Struct;
		if (Lower == TEXT("enum"))      return EOliveIRTypeCategory::Enum;
		if (Lower == TEXT("delegate"))  return EOliveIRTypeCategory::Delegate;
		if (Lower == TEXT("mcdelegate") || Lower == TEXT("multicastdelegate"))
			return EOliveIRTypeCategory::MulticastDelegate;
		if (Lower == TEXT("array"))     return EOliveIRTypeCategory::Array;
		if (Lower == TEXT("set"))       return EOliveIRTypeCategory::Set;
		if (Lower == TEXT("map"))       return EOliveIRTypeCategory::Map;
		if (Lower == TEXT("exec"))      return EOliveIRTypeCategory::Exec;
		return EOliveIRTypeCategory::Unknown;
	}

	/** Convert ELibraryCloneMode to a string for logging/result. */
	FString CloneModeToString(ELibraryCloneMode Mode)
	{
		switch (Mode)
		{
		case ELibraryCloneMode::Structure: return TEXT("structure");
		case ELibraryCloneMode::Portable:  return TEXT("portable");
		case ELibraryCloneMode::Full:      return TEXT("full");
		default:                           return TEXT("unknown");
		}
	}

	/** Check if a string looks like a source-project asset path. */
	bool LooksLikeAssetPath(const FString& Value)
	{
		return Value.StartsWith(TEXT("/Game/")) || Value.StartsWith(TEXT("/Script/")) ||
			   Value.Contains(TEXT(".")); // e.g., "/Game/FlexibleCombatSystem/Meshes/Arrow.Arrow"
	}

	/** Strip common asset reference property names that we should clear. */
	static const TSet<FString>& GetAssetRefPropertyNames()
	{
		static TSet<FString> Names = {
			TEXT("StaticMesh"),
			TEXT("SkeletalMesh"),
			TEXT("AnimClass"),
			TEXT("Material"),
			TEXT("Texture"),
			TEXT("Sound"),
			TEXT("SoundCue"),
			TEXT("NiagaraSystem"),
			TEXT("ParticleSystem"),
		};
		return Names;
	}
}

// =============================================================================
// Clone() -- Main Entry Point
// =============================================================================

FLibraryCloneResult FOliveLibraryCloner::Clone(
	const FString& TemplateId,
	const FString& AssetPath,
	ELibraryCloneMode InMode,
	const TMap<FString, FString>& InRemapMap,
	const TArray<FString>& GraphWhitelist,
	const FString& ParentClassOverride)
{
	FLibraryCloneResult Result;
	Result.TemplateId = TemplateId;
	Result.AssetPath = AssetPath;
	Result.Mode = InMode;

	// Reset per-clone state
	CurrentMode = InMode;
	RemapMap = InRemapMap;
	NodeMap.Empty();
	NodeIdMap.Empty();
	UnresolvedTypes.Empty();
	TargetBlueprint = nullptr;
	FlattenedVariableNames.Empty();
	TemplateClassName.Empty();

	// ----------------------------------------------------------------
	// 1. Load template via FOliveLibraryIndex
	// ----------------------------------------------------------------
	const FOliveLibraryIndex& LibIndex = FOliveTemplateSystem::Get().GetLibraryIndex();
	const FOliveLibraryTemplateInfo* TemplateInfo = LibIndex.FindTemplate(TemplateId);
	if (!TemplateInfo)
	{
		UE_LOG(LogOliveLibraryCloner, Error,
			TEXT("Template '%s' not found in library index"), *TemplateId);
		Result.Warnings.Add(FString::Printf(
			TEXT("LIBRARY_TEMPLATE_NOT_FOUND: Template '%s' not found"), *TemplateId));
		return Result;
	}

	TSharedPtr<FJsonObject> FullJson = LibIndex.LoadFullJson(TemplateId);
	if (!FullJson.IsValid())
	{
		UE_LOG(LogOliveLibraryCloner, Error,
			TEXT("Failed to load full JSON for template '%s'"), *TemplateId);
		Result.Warnings.Add(FString::Printf(
			TEXT("LIBRARY_TEMPLATE_LOAD_FAILED: Could not load JSON for '%s'"), *TemplateId));
		return Result;
	}

	// Store the template's original class name for self-call detection during graph cloning
	TemplateClassName = TemplateInfo->DisplayName;

	// ----------------------------------------------------------------
	// 2. Resolve inheritance chain and flatten ancestor data
	// ----------------------------------------------------------------
	TArray<const FOliveLibraryTemplateInfo*> InheritanceChain =
		LibIndex.ResolveInheritanceChain(TemplateId);

	// Collect variables/components from unresolvable ancestors for flattening.
	// The chain is root->leaf order. We accumulate data from ancestors that
	// cannot be resolved as parent classes, merging into the template JSON.
	TArray<TSharedPtr<FJsonObject>> AncestorJsons;
	for (const FOliveLibraryTemplateInfo* Ancestor : InheritanceChain)
	{
		if (Ancestor->TemplateId == TemplateId)
		{
			continue; // Skip self
		}

		// Check if this ancestor's parent class is resolvable
		FString AncestorParent = Ancestor->ParentClassName;
		if (AncestorParent.IsEmpty()) continue;

		UClass* AncestorClass = ResolveClass(AncestorParent);
		if (!AncestorClass)
		{
			// This ancestor is unresolvable -- we need to flatten its data
			TSharedPtr<FJsonObject> AncestorJson = LibIndex.LoadFullJson(Ancestor->TemplateId);
			if (AncestorJson.IsValid())
			{
				AncestorJsons.Add(AncestorJson);
				UE_LOG(LogOliveLibraryCloner, Log,
					TEXT("Flattening data from unresolvable ancestor '%s'"),
					*Ancestor->TemplateId);
			}
		}
	}

	// ----------------------------------------------------------------
	// 3. Resolve parent class (root native ancestor strategy)
	// ----------------------------------------------------------------
	FString ParentClassNote;
	FString ResolvedParentClass;

	if (!ParentClassOverride.IsEmpty())
	{
		// User-provided override takes priority
		ResolvedParentClass = ParentClassOverride;
		ParentClassNote = FString::Printf(
			TEXT("Using parent_class_override: %s"), *ParentClassOverride);
	}
	else
	{
		ResolvedParentClass = ResolveParentClass(*TemplateInfo, ParentClassNote);
	}

	if (ResolvedParentClass.IsEmpty())
	{
		UE_LOG(LogOliveLibraryCloner, Error,
			TEXT("Cannot resolve parent class for template '%s'"), *TemplateId);
		Result.Warnings.Add(FString::Printf(
			TEXT("LIBRARY_CLONE_PARENT_UNRESOLVABLE: Cannot resolve parent class for '%s'. ")
			TEXT("Use parent_class_override to specify a parent class."), *TemplateId));
		return Result;
	}

	Result.ParentClass = ResolvedParentClass;
	Result.ParentClassNote = ParentClassNote;

	UE_LOG(LogOliveLibraryCloner, Log,
		TEXT("Resolved parent class: '%s' (%s)"), *ResolvedParentClass, *ParentClassNote);

	// ----------------------------------------------------------------
	// 4. Determine Blueprint type
	// ----------------------------------------------------------------
	FString TypeStr;
	FullJson->TryGetStringField(TEXT("type"), TypeStr);
	EOliveBlueprintType BPType = ParseLibraryBlueprintType(TypeStr);

	// If the type is Unknown/Normal but the parent class is ActorComponent-derived,
	// force ActorComponent type so CreateBlueprint produces the right BP kind.
	if (BPType == EOliveBlueprintType::Normal)
	{
		UClass* ParentClass = ResolveClass(ResolvedParentClass);
		if (ParentClass && ParentClass->IsChildOf(UActorComponent::StaticClass()))
		{
			BPType = EOliveBlueprintType::ActorComponent;
		}
	}

	// ----------------------------------------------------------------
	// 5. Create the Blueprint asset
	// ----------------------------------------------------------------
	FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
	FOliveBlueprintWriteResult CreateResult = Writer.CreateBlueprint(
		AssetPath, ResolvedParentClass, BPType);

	if (!CreateResult.bSuccess)
	{
		FString ErrDetail = CreateResult.GetFirstError(TEXT("Unknown error"));
		UE_LOG(LogOliveLibraryCloner, Error,
			TEXT("Failed to create Blueprint at '%s': %s"), *AssetPath, *ErrDetail);
		Result.Warnings.Add(FString::Printf(
			TEXT("LIBRARY_CLONE_CREATE_FAILED: %s"), *ErrDetail));
		return Result;
	}

	// ----------------------------------------------------------------
	// 6. Load the created Blueprint
	// ----------------------------------------------------------------
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveLibraryCloner, Error,
			TEXT("Blueprint created but failed to load at '%s'"), *AssetPath);
		Result.Warnings.Add(FString::Printf(
			TEXT("LIBRARY_CLONE_LOAD_FAILED: Blueprint created but failed to load at '%s'"),
			*AssetPath));
		return Result;
	}

	TargetBlueprint = Blueprint;

	// ----------------------------------------------------------------
	// 7. Structure creation (within a single transaction)
	// ----------------------------------------------------------------
	{
		const FScopedTransaction Transaction(
			FText::Format(NSLOCTEXT("OliveLibraryCloner", "CloneLibraryTemplate",
				"Olive: Clone Library Template '{0}'"), FText::FromString(TemplateId)));

		Blueprint->Modify();

		// Create structure from flattened ancestors first (bottom of chain),
		// then from the template itself (child overrides parent).
		for (const TSharedPtr<FJsonObject>& AncestorJson : AncestorJsons)
		{
			UE_LOG(LogOliveLibraryCloner, Verbose,
				TEXT("Creating structure from flattened ancestor"));

			CreateVariables(Blueprint, AssetPath, AncestorJson, Result);
			CreateComponents(Blueprint, AssetPath, AncestorJson, Result);
			// Interfaces and dispatchers from ancestors are also flattened
			AddInterfaces(Blueprint, AssetPath, AncestorJson, Result);
			CreateDispatchers(Blueprint, AssetPath, AncestorJson, Result);
		}

		// Create structure from the template itself
		CreateVariables(Blueprint, AssetPath, FullJson, Result);
		CreateComponents(Blueprint, AssetPath, FullJson, Result);
		AddInterfaces(Blueprint, AssetPath, FullJson, Result);
		CreateDispatchers(Blueprint, AssetPath, FullJson, Result);
		CreateFunctionSignatures(Blueprint, AssetPath, FullJson, Result);

		// Also create function signatures from flattened ancestors
		// (after the template's own, so they appear in the right order)
		for (const TSharedPtr<FJsonObject>& AncestorJson : AncestorJsons)
		{
			CreateFunctionSignatures(Blueprint, AssetPath, AncestorJson, Result);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	// ----------------------------------------------------------------
	// 8. Intermediate compile (CRITICAL -- see design Section 13)
	// ----------------------------------------------------------------
	UE_LOG(LogOliveLibraryCloner, Log,
		TEXT("Performing intermediate compile for '%s'"), *AssetPath);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);

	// ----------------------------------------------------------------
	// 9. Graph cloning (skipped for mode == Structure)
	// ----------------------------------------------------------------
	if (CurrentMode != ELibraryCloneMode::Structure)
	{
		const FScopedTransaction GraphTransaction(
			FText::Format(NSLOCTEXT("OliveLibraryCloner", "CloneGraphs",
				"Olive: Clone Graphs for '{0}'"), FText::FromString(TemplateId)));

		Blueprint->Modify();

		// Retrieve the "graphs" object from template JSON
		const TSharedPtr<FJsonObject>* GraphsObj = nullptr;
		if (FullJson->TryGetObjectField(TEXT("graphs"), GraphsObj) && GraphsObj)
		{
			// Clone event graphs
			const TArray<TSharedPtr<FJsonValue>>* EventGraphsArray = nullptr;
			if ((*GraphsObj)->TryGetArrayField(TEXT("event_graphs"), EventGraphsArray) && EventGraphsArray)
			{
				for (const TSharedPtr<FJsonValue>& GraphVal : *EventGraphsArray)
				{
					const TSharedPtr<FJsonObject>* GraphObj = nullptr;
					if (!GraphVal->TryGetObject(GraphObj) || !GraphObj)
					{
						continue;
					}

					FString GraphName;
					(*GraphObj)->TryGetStringField(TEXT("name"), GraphName);

					// Check whitelist
					if (GraphWhitelist.Num() > 0 && !GraphWhitelist.Contains(GraphName))
					{
						continue;
					}

					// Find the matching UEdGraph on the Blueprint
					UEdGraph* TargetGraph = nullptr;
					for (UEdGraph* Graph : Blueprint->UbergraphPages)
					{
						if (Graph && Graph->GetName() == GraphName)
						{
							TargetGraph = Graph;
							break;
						}
					}

					if (!TargetGraph && Blueprint->UbergraphPages.Num() > 0)
					{
						// Default to the first event graph (usually "EventGraph")
						TargetGraph = Blueprint->UbergraphPages[0];
					}

					if (TargetGraph)
					{
						// Clear per-graph node map (IDs like "node_0" repeat across graphs)
						NodeMap.Empty();
						NodeIdMap.Empty();

						FCloneGraphResult GraphResult = CloneGraph(
							Blueprint, TargetGraph, AssetPath, *GraphObj);
						Result.GraphResults.Add(MoveTemp(GraphResult));
					}
					else
					{
						UE_LOG(LogOliveLibraryCloner, Warning,
							TEXT("No event graph found for '%s', skipping"), *GraphName);
					}
				}
			}

			// Clone function graphs
			const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
			if ((*GraphsObj)->TryGetArrayField(TEXT("functions"), FunctionsArray) && FunctionsArray)
			{
				for (const TSharedPtr<FJsonValue>& FuncVal : *FunctionsArray)
				{
					const TSharedPtr<FJsonObject>* FuncObj = nullptr;
					if (!FuncVal->TryGetObject(FuncObj) || !FuncObj)
					{
						continue;
					}

					FString FuncName;
					(*FuncObj)->TryGetStringField(TEXT("name"), FuncName);
					if (FuncName.IsEmpty())
					{
						continue;
					}

					// Check whitelist
					if (GraphWhitelist.Num() > 0 && !GraphWhitelist.Contains(FuncName))
					{
						continue;
					}

					// Find the matching function graph
					UEdGraph* TargetGraph = nullptr;
					for (UEdGraph* Graph : Blueprint->FunctionGraphs)
					{
						if (Graph && Graph->GetName() == FuncName)
						{
							TargetGraph = Graph;
							break;
						}
					}

					if (TargetGraph)
					{
						// Clear per-graph node map (IDs repeat across graphs)
						NodeMap.Empty();
						NodeIdMap.Empty();

						FCloneGraphResult GraphResult = CloneGraph(
							Blueprint, TargetGraph, AssetPath, *FuncObj);
						Result.GraphResults.Add(MoveTemp(GraphResult));
					}
					else
					{
						UE_LOG(LogOliveLibraryCloner, Warning,
							TEXT("Function graph '%s' not found on Blueprint, skipping"),
							*FuncName);
					}
				}
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	// ----------------------------------------------------------------
	// 10. Final compile
	// ----------------------------------------------------------------
	UE_LOG(LogOliveLibraryCloner, Log,
		TEXT("Performing final compile for '%s'"), *AssetPath);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);

	Result.bCompiled = true;
	Result.bCompileSuccess = (Blueprint->Status != BS_Error);
	if (!Result.bCompileSuccess)
	{
		Result.CompileErrors.Add(TEXT("Blueprint has compile errors after clone"));
	}

	// ----------------------------------------------------------------
	// 11. Build remap suggestions from unresolved types
	// ----------------------------------------------------------------
	BuildRemapSuggestions(Result);

	// ----------------------------------------------------------------
	// 12. Success
	// ----------------------------------------------------------------
	Result.bSuccess = true;

	UE_LOG(LogOliveLibraryCloner, Log,
		TEXT("Clone complete: '%s' -> '%s' (mode=%s, vars=%d/%d demoted, comps=%d, "
			 "interfaces=%d, dispatchers=%d, functions=%d)"),
		*TemplateId, *AssetPath, *CloneModeToString(CurrentMode),
		Result.VariablesCreated, Result.VariablesDemoted,
		Result.ComponentsCreated, Result.InterfacesAdded,
		Result.DispatchersCreated, Result.FunctionsCreated);

	return Result;
}

// =============================================================================
// ResolveParentClass
// =============================================================================

FString FOliveLibraryCloner::ResolveParentClass(
	const FOliveLibraryTemplateInfo& TemplateInfo,
	FString& OutNote)
{
	const FOliveLibraryIndex& LibIndex = FOliveTemplateSystem::Get().GetLibraryIndex();

	// Walk the inheritance chain root->leaf
	TArray<const FOliveLibraryTemplateInfo*> Chain =
		LibIndex.ResolveInheritanceChain(TemplateInfo.TemplateId);

	// Try the template's own parent class first (may be in the remap map)
	{
		FString ParentName = TemplateInfo.ParentClassName;
		if (!ParentName.IsEmpty())
		{
			FString RemappedName = ApplyRemap(ParentName);
			UClass* Resolved = ResolveClass(RemappedName);
			if (Resolved)
			{
				if (RemappedName != ParentName)
				{
					OutNote = FString::Printf(
						TEXT("Parent '%s' remapped to '%s' and resolved"),
						*ParentName, *RemappedName);
				}
				else
				{
					OutNote = FString::Printf(
						TEXT("Parent '%s' resolved directly"), *ParentName);
				}
				return Resolved->GetName();
			}
		}
	}

	// Walk the chain from root to leaf, trying each ancestor's parent class.
	// The deepest one that resolves is the best parent.
	FString BestResolvedName;
	FString BestOriginalName;
	FString BestTemplateId;

	for (const FOliveLibraryTemplateInfo* Ancestor : Chain)
	{
		FString AncestorParent = Ancestor->ParentClassName;
		if (AncestorParent.IsEmpty()) continue;

		FString RemappedName = ApplyRemap(AncestorParent);
		UClass* Resolved = ResolveClass(RemappedName);
		if (Resolved)
		{
			// This is a valid parent. Since chain is root->leaf,
			// keep overwriting -- the last (deepest) valid one wins.
			BestResolvedName = Resolved->GetName();
			BestOriginalName = AncestorParent;
			BestTemplateId = Ancestor->TemplateId;
		}
	}

	if (!BestResolvedName.IsEmpty())
	{
		if (BestOriginalName != TemplateInfo.ParentClassName)
		{
			OutNote = FString::Printf(
				TEXT("Original parent '%s' unresolvable. Using '%s' from ancestor '%s' (root native strategy)"),
				*TemplateInfo.ParentClassName, *BestResolvedName, *BestTemplateId);
		}
		else
		{
			OutNote = FString::Printf(TEXT("Parent '%s' resolved"), *BestResolvedName);
		}

		// Track the unresolvable original parent for remap suggestions
		if (BestOriginalName != TemplateInfo.ParentClassName && !TemplateInfo.ParentClassName.IsEmpty())
		{
			TrackUnresolved(TemplateInfo.ParentClassName, TEXT("parent_class"));
		}

		return BestResolvedName;
	}

	// Nothing resolved at all -- should be impossible since the root is always a native class
	OutNote = TEXT("No resolvable parent class found in inheritance chain");
	return FString();
}

// =============================================================================
// Resolution Pipeline
// =============================================================================

UClass* FOliveLibraryCloner::ResolveClass(const FString& SourceName)
{
	if (SourceName.IsEmpty())
	{
		return nullptr;
	}

	// Stage 1: Apply remap
	FString MappedName = ApplyRemap(SourceName);

	// Stage 2: FOliveClassResolver::Resolve()
	FOliveClassResolveResult ResolveResult = FOliveClassResolver::Resolve(MappedName);
	if (ResolveResult.IsValid())
	{
		return ResolveResult.Class;
	}

	// If remap produced a different name but still failed, try the original
	if (MappedName != SourceName)
	{
		ResolveResult = FOliveClassResolver::Resolve(SourceName);
		if (ResolveResult.IsValid())
		{
			return ResolveResult.Class;
		}
	}

	return nullptr;
}

UScriptStruct* FOliveLibraryCloner::ResolveStruct(const FString& SourceName)
{
	if (SourceName.IsEmpty())
	{
		return nullptr;
	}

	FString MappedName = ApplyRemap(SourceName);

	// Common engine module path prefixes to search
	static const FString SearchPrefixes[] = {
		TEXT("/Script/CoreUObject."),
		TEXT("/Script/Engine."),
		TEXT("/Script/PhysicsCore."),
		TEXT("/Script/NavigationSystem."),
		TEXT("/Script/Niagara."),
		TEXT("/Script/InputCore."),
		TEXT("/Script/GameplayTags."),
		TEXT("/Script/GameplayAbilities."),
		TEXT("/Script/AIModule."),
	};

	// Try with and without F prefix
	TArray<FString> Candidates;
	Candidates.Add(MappedName);
	if (!MappedName.StartsWith(TEXT("F")))
	{
		Candidates.Add(TEXT("F") + MappedName);
	}
	else
	{
		// Also try without F prefix
		Candidates.Add(MappedName.Mid(1));
	}

	for (const FString& Name : Candidates)
	{
		for (const FString& Prefix : SearchPrefixes)
		{
			FString FullPath = Prefix + Name;
			UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *FullPath);
			if (Found)
			{
				return Found;
			}
		}
	}

	// If remap produced a different name but failed, also try the original
	if (MappedName != SourceName)
	{
		TArray<FString> OrigCandidates;
		OrigCandidates.Add(SourceName);
		if (!SourceName.StartsWith(TEXT("F")))
		{
			OrigCandidates.Add(TEXT("F") + SourceName);
		}

		for (const FString& Name : OrigCandidates)
		{
			for (const FString& Prefix : SearchPrefixes)
			{
				FString FullPath = Prefix + Name;
				UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *FullPath);
				if (Found)
				{
					return Found;
				}
			}
		}
	}

	return nullptr;
}

FString FOliveLibraryCloner::ApplyRemap(const FString& SourceName) const
{
	if (RemapMap.Num() == 0)
	{
		return SourceName;
	}

	// Try exact match first
	const FString* Mapped = RemapMap.Find(SourceName);
	if (Mapped)
	{
		return *Mapped;
	}

	// Strip _C suffix and try again (design Section 5.2)
	FString StrippedName = SourceName;
	if (StrippedName.EndsWith(TEXT("_C")))
	{
		StrippedName = StrippedName.LeftChop(2);
		Mapped = RemapMap.Find(StrippedName);
		if (Mapped)
		{
			return *Mapped;
		}
	}

	// Try with _C appended (in case user provided without it)
	FString WithSuffix = SourceName + TEXT("_C");
	Mapped = RemapMap.Find(WithSuffix);
	if (Mapped)
	{
		return *Mapped;
	}

	// Case-insensitive fallback
	for (const auto& Pair : RemapMap)
	{
		if (Pair.Key.Equals(SourceName, ESearchCase::IgnoreCase) ||
			Pair.Key.Equals(StrippedName, ESearchCase::IgnoreCase))
		{
			return Pair.Value;
		}
	}

	return SourceName;
}

void FOliveLibraryCloner::TrackUnresolved(const FString& SourceName, const FString& UsageContext)
{
	TArray<FString>& Contexts = UnresolvedTypes.FindOrAdd(SourceName);
	// Avoid duplicate contexts
	if (!Contexts.Contains(UsageContext))
	{
		Contexts.Add(UsageContext);
	}
}

// =============================================================================
// ParseLibraryType
// =============================================================================

FOliveLibraryCloner::FParsedLibraryType FOliveLibraryCloner::ParseLibraryType(
	const TSharedPtr<FJsonObject>& TypeJson) const
{
	FParsedLibraryType Parsed;
	if (!TypeJson.IsValid())
	{
		return Parsed;
	}

	TypeJson->TryGetStringField(TEXT("category"), Parsed.Category);
	TypeJson->TryGetStringField(TEXT("class"), Parsed.ClassName);
	TypeJson->TryGetStringField(TEXT("struct_name"), Parsed.StructName);
	TypeJson->TryGetStringField(TEXT("enum_name"), Parsed.EnumName);

	// element_type for arrays (may be a JSON string containing a nested object)
	FString ElementTypeStr;
	if (TypeJson->TryGetStringField(TEXT("element_type"), ElementTypeStr))
	{
		Parsed.ElementType = ElementTypeStr;
	}

	return Parsed;
}

// =============================================================================
// CreateVariables
// =============================================================================

void FOliveLibraryCloner::CreateVariables(
	UBlueprint* Blueprint,
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& TemplateJson,
	FLibraryCloneResult& Result)
{
	const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
	if (!TemplateJson->TryGetArrayField(TEXT("variables"), VarsArray) || !VarsArray)
	{
		return;
	}

	FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();

	for (const TSharedPtr<FJsonValue>& VarVal : *VarsArray)
	{
		const TSharedPtr<FJsonObject>* VarObj = nullptr;
		if (!VarVal->TryGetObject(VarObj) || !VarObj)
		{
			continue;
		}

		FString VarName;
		(*VarObj)->TryGetStringField(TEXT("name"), VarName);
		if (VarName.IsEmpty())
		{
			Result.Warnings.Add(TEXT("Skipped variable with empty name"));
			continue;
		}

		// Skip component-defined variables (they are created by AddComponent)
		FString DefinedIn;
		(*VarObj)->TryGetStringField(TEXT("defined_in"), DefinedIn);
		if (DefinedIn.Equals(TEXT("component"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		// De-duplicate: if this variable was already created from an ancestor, skip
		// (child template's definition wins because ancestors are processed first)
		bool bAlreadyExists = false;
		for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
		{
			if (ExistingVar.VarName.IsEqual(FName(*VarName), ENameCase::IgnoreCase))
			{
				bAlreadyExists = true;
				break;
			}
		}

		if (bAlreadyExists)
		{
			// If this is from an ancestor, note that we're skipping a duplicate
			if (FlattenedVariableNames.Contains(VarName))
			{
				UE_LOG(LogOliveLibraryCloner, Verbose,
					TEXT("Variable '%s' already exists from ancestor, child overrides skipped"),
					*VarName);
			}
			continue;
		}

		// Parse type
		const TSharedPtr<FJsonObject>* TypeObj = nullptr;
		FParsedLibraryType LibType;
		if ((*VarObj)->TryGetObjectField(TEXT("type"), TypeObj) && TypeObj)
		{
			LibType = ParseLibraryType(*TypeObj);
		}

		// Build FOliveIRVariable
		FOliveIRVariable VarIR;
		VarIR.Name = VarName;

		// Category
		FString Category;
		(*VarObj)->TryGetStringField(TEXT("category"), Category);
		VarIR.Category = Category;

		// Flags
		const TSharedPtr<FJsonObject>* FlagsObj = nullptr;
		if ((*VarObj)->TryGetObjectField(TEXT("flags"), FlagsObj) && FlagsObj)
		{
			(*FlagsObj)->TryGetBoolField(TEXT("blueprint_read_write"), VarIR.bBlueprintReadWrite);
			(*FlagsObj)->TryGetBoolField(TEXT("expose_on_spawn"), VarIR.bExposeOnSpawn);
			(*FlagsObj)->TryGetBoolField(TEXT("replicated"), VarIR.bReplicated);
			(*FlagsObj)->TryGetBoolField(TEXT("save_game"), VarIR.bSaveGame);
			(*FlagsObj)->TryGetBoolField(TEXT("edit_anywhere"), VarIR.bEditAnywhere);
			(*FlagsObj)->TryGetBoolField(TEXT("blueprint_visible"), VarIR.bBlueprintVisible);
		}

		// Resolve type through the pipeline
		EOliveIRTypeCategory TypeCategory = ParseCategoryString(LibType.Category);
		VarIR.Type.Category = TypeCategory;
		bool bDemoted = false;

		switch (TypeCategory)
		{
		case EOliveIRTypeCategory::Object:
		case EOliveIRTypeCategory::Class:
		case EOliveIRTypeCategory::Interface:
		{
			FString ClassName = LibType.ClassName;
			if (!ClassName.IsEmpty())
			{
				UClass* Resolved = ResolveClass(ClassName);
				if (Resolved)
				{
					VarIR.Type.ClassName = Resolved->GetName();
				}
				else
				{
					// Demote to base UObject*
					VarIR.Type.ClassName = TEXT("Object");
					bDemoted = true;
					FString Warning = FString::Printf(
						TEXT("Variable '%s' type demoted: %s -> Object (class not found in project)"),
						*VarName, *ClassName);
					Result.Warnings.Add(Warning);
					TrackUnresolved(ClassName, FString::Printf(TEXT("variable: %s (type)"), *VarName));
					UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *Warning);
				}
			}
			break;
		}

		case EOliveIRTypeCategory::Struct:
		{
			FString StructName = LibType.StructName;
			if (!StructName.IsEmpty())
			{
				UScriptStruct* Resolved = ResolveStruct(StructName);
				if (Resolved)
				{
					VarIR.Type.StructName = Resolved->GetName();
				}
				else
				{
					// Demote struct to String
					VarIR.Type.Category = EOliveIRTypeCategory::String;
					VarIR.Type.StructName.Empty();
					bDemoted = true;
					FString Warning = FString::Printf(
						TEXT("Variable '%s' type demoted: struct %s -> String (struct not found)"),
						*VarName, *StructName);
					Result.Warnings.Add(Warning);
					TrackUnresolved(StructName, FString::Printf(TEXT("variable: %s (struct type)"), *VarName));
					UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *Warning);
				}
			}
			break;
		}

		case EOliveIRTypeCategory::Enum:
		{
			FString EnumName = LibType.EnumName;
			if (!EnumName.IsEmpty())
			{
				// Try to find the enum
				UEnum* FoundEnum = FindObject<UEnum>(nullptr,
					*FString::Printf(TEXT("/Script/Engine.%s"), *EnumName));
				if (!FoundEnum)
				{
					FoundEnum = FindObject<UEnum>(nullptr,
						*FString::Printf(TEXT("/Script/CoreUObject.%s"), *EnumName));
				}

				if (FoundEnum)
				{
					VarIR.Type.EnumName = FoundEnum->GetName();
				}
				else
				{
					// Demote enum to Byte
					VarIR.Type.Category = EOliveIRTypeCategory::Byte;
					VarIR.Type.EnumName.Empty();
					bDemoted = true;
					FString Warning = FString::Printf(
						TEXT("Variable '%s' type demoted: enum %s -> Byte (enum not found)"),
						*VarName, *EnumName);
					Result.Warnings.Add(Warning);
					TrackUnresolved(EnumName, FString::Printf(TEXT("variable: %s (enum type)"), *VarName));
					UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *Warning);
				}
			}
			break;
		}

		case EOliveIRTypeCategory::Array:
		{
			// Parse element type from the raw JSON string
			if (!LibType.ElementType.IsEmpty())
			{
				VarIR.Type.ElementTypeJson = LibType.ElementType;

				// Try to resolve the element type's class/struct if it's an object/struct
				TSharedPtr<FJsonObject> ElemTypeObj;
				TSharedRef<TJsonReader<>> ElemReader = TJsonReaderFactory<>::Create(LibType.ElementType);
				if (FJsonSerializer::Deserialize(ElemReader, ElemTypeObj) && ElemTypeObj.IsValid())
				{
					FParsedLibraryType ElemType = ParseLibraryType(ElemTypeObj);
					EOliveIRTypeCategory ElemCategory = ParseCategoryString(ElemType.Category);

					if (ElemCategory == EOliveIRTypeCategory::Object && !ElemType.ClassName.IsEmpty())
					{
						UClass* ElemClass = ResolveClass(ElemType.ClassName);
						if (!ElemClass)
						{
							// Demote element type to UObject*
							TSharedPtr<FJsonObject> DemotedElem = MakeShared<FJsonObject>();
							DemotedElem->SetStringField(TEXT("category"), TEXT("object"));
							DemotedElem->SetStringField(TEXT("class"), TEXT("Object"));
							FString DemotedStr;
							TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> DemotedWriter =
								TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&DemotedStr);
							FJsonSerializer::Serialize(DemotedElem.ToSharedRef(), DemotedWriter);
							VarIR.Type.ElementTypeJson = DemotedStr;
							bDemoted = true;
							FString Warning = FString::Printf(
								TEXT("Variable '%s' array element type demoted: %s -> Object"),
								*VarName, *ElemType.ClassName);
							Result.Warnings.Add(Warning);
							TrackUnresolved(ElemType.ClassName,
								FString::Printf(TEXT("variable: %s (array element type)"), *VarName));
						}
					}
					else if (ElemCategory == EOliveIRTypeCategory::Struct && !ElemType.StructName.IsEmpty())
					{
						UScriptStruct* ElemStruct = ResolveStruct(ElemType.StructName);
						if (!ElemStruct)
						{
							// Demote element struct to String
							TSharedPtr<FJsonObject> DemotedElem = MakeShared<FJsonObject>();
							DemotedElem->SetStringField(TEXT("category"), TEXT("string"));
							FString DemotedStr;
							TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> DemotedWriter =
								TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&DemotedStr);
							FJsonSerializer::Serialize(DemotedElem.ToSharedRef(), DemotedWriter);
							VarIR.Type.ElementTypeJson = DemotedStr;
							bDemoted = true;
							FString Warning = FString::Printf(
								TEXT("Variable '%s' array element type demoted: struct %s -> String"),
								*VarName, *ElemType.StructName);
							Result.Warnings.Add(Warning);
							TrackUnresolved(ElemType.StructName,
								FString::Printf(TEXT("variable: %s (array element struct)"), *VarName));
						}
					}
				}
			}
			break;
		}

		default:
			// Primitive types (bool, int, float, string, etc.) need no resolution
			break;
		}

		// Create the variable
		FOliveBlueprintWriteResult VarResult = Writer.AddVariable(AssetPath, VarIR);
		if (VarResult.bSuccess)
		{
			Result.VariablesCreated++;
			if (bDemoted)
			{
				Result.VariablesDemoted++;
			}
			FlattenedVariableNames.Add(VarName);
		}
		else
		{
			FString ErrMsg = FString::Printf(TEXT("Failed to add variable '%s'"), *VarName);
			if (!VarResult.GetFirstError().IsEmpty())
			{
				ErrMsg += TEXT(": ") + VarResult.GetFirstError();
			}
			Result.Warnings.Add(ErrMsg);
			Result.VariablesSkipped++;
			UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *ErrMsg);
		}
	}
}

// =============================================================================
// CreateComponents
// =============================================================================

void FOliveLibraryCloner::CreateComponents(
	UBlueprint* Blueprint,
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& TemplateJson,
	FLibraryCloneResult& Result)
{
	// Library templates store components in: components.tree (array of hierarchical nodes)
	const TSharedPtr<FJsonObject>* ComponentsObj = nullptr;
	if (!TemplateJson->TryGetObjectField(TEXT("components"), ComponentsObj) || !ComponentsObj)
	{
		return;
	}

	// Get the root component name
	FString RootName;
	(*ComponentsObj)->TryGetStringField(TEXT("root"), RootName);

	// Get the tree array
	const TArray<TSharedPtr<FJsonValue>>* TreeArray = nullptr;
	if (!(*ComponentsObj)->TryGetArrayField(TEXT("tree"), TreeArray) || !TreeArray)
	{
		return;
	}

	// Recursively create components from the tree
	CreateComponentsFromTree(AssetPath, *TreeArray, TEXT(""), Result);

	// Set root component if specified and different from DefaultSceneRoot
	if (!RootName.IsEmpty() && RootName != TEXT("DefaultSceneRoot"))
	{
		FOliveComponentWriter& CompWriter = FOliveComponentWriter::Get();
		FOliveBlueprintWriteResult RootResult = CompWriter.SetRootComponent(AssetPath, RootName);
		if (!RootResult.bSuccess)
		{
			Result.Warnings.Add(FString::Printf(
				TEXT("Failed to set root component '%s'"), *RootName));
		}
	}
}

void FOliveLibraryCloner::CreateComponentsFromTree(
	const FString& AssetPath,
	const TArray<TSharedPtr<FJsonValue>>& TreeArray,
	const FString& ParentName,
	FLibraryCloneResult& Result)
{
	FOliveComponentWriter& CompWriter = FOliveComponentWriter::Get();

	for (const TSharedPtr<FJsonValue>& NodeVal : TreeArray)
	{
		const TSharedPtr<FJsonObject>* NodeObj = nullptr;
		if (!NodeVal->TryGetObject(NodeObj) || !NodeObj)
		{
			continue;
		}

		FString CompName, CompClass;
		(*NodeObj)->TryGetStringField(TEXT("name"), CompName);
		(*NodeObj)->TryGetStringField(TEXT("class"), CompClass);

		if (CompName.IsEmpty() || CompClass.IsEmpty())
		{
			Result.Warnings.Add(FString::Printf(
				TEXT("Skipped component with empty name='%s' or class='%s'"),
				*CompName, *CompClass));
			continue;
		}

		// Resolve the component class
		UClass* ResolvedClass = ResolveClass(CompClass);
		if (!ResolvedClass)
		{
			FString Warning = FString::Printf(
				TEXT("Component '%s' skipped: class '%s' not found in project"),
				*CompName, *CompClass);
			Result.Warnings.Add(Warning);
			Result.ComponentsSkipped++;
			TrackUnresolved(CompClass, FString::Printf(TEXT("component: %s"), *CompName));
			UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *Warning);

			// Still try to create children (they may be standard components)
			const TArray<TSharedPtr<FJsonValue>>* ChildrenArray = nullptr;
			if ((*NodeObj)->TryGetArrayField(TEXT("children"), ChildrenArray) && ChildrenArray)
			{
				// Children are orphaned since parent was skipped -- attach to parent's parent
				CreateComponentsFromTree(AssetPath, *ChildrenArray, ParentName, Result);
			}
			continue;
		}

		// Create the component
		FOliveBlueprintWriteResult CompResult = CompWriter.AddComponent(
			AssetPath, ResolvedClass->GetName(), CompName, ParentName);

		if (!CompResult.bSuccess)
		{
			FString ErrMsg = FString::Printf(
				TEXT("Failed to add component '%s' (%s)"), *CompName, *CompClass);
			if (!CompResult.GetFirstError().IsEmpty())
			{
				ErrMsg += TEXT(": ") + CompResult.GetFirstError();
			}
			Result.Warnings.Add(ErrMsg);
			Result.ComponentsSkipped++;
			UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *ErrMsg);
			continue;
		}

		Result.ComponentsCreated++;

		// Apply properties, but skip asset references (meshes, materials, etc.)
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if ((*NodeObj)->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
		{
			TMap<FString, FString> PropertiesToSet;
			for (const auto& PropPair : (*PropsObj)->Values)
			{
				FString PropVal;
				if (!PropPair.Value->TryGetString(PropVal))
				{
					continue;
				}

				// Skip asset reference properties
				if (GetAssetRefPropertyNames().Contains(PropPair.Key))
				{
					if (LooksLikeAssetPath(PropVal))
					{
						Result.Warnings.Add(FString::Printf(
							TEXT("Component '%s' property '%s' cleared: "
								 "original referenced '%s'"),
							*CompName, *PropPair.Key, *PropVal));
						continue;
					}
				}

				PropertiesToSet.Add(PropPair.Key, PropVal);
			}

			if (PropertiesToSet.Num() > 0)
			{
				FOliveBlueprintWriteResult ModResult = CompWriter.ModifyComponent(
					AssetPath, CompName, PropertiesToSet);
				if (!ModResult.bSuccess)
				{
					Result.Warnings.Add(FString::Printf(
						TEXT("Component '%s' created but property modification failed"),
						*CompName));
				}
			}
		}

		// Recursively create children
		const TArray<TSharedPtr<FJsonValue>>* ChildrenArray = nullptr;
		if ((*NodeObj)->TryGetArrayField(TEXT("children"), ChildrenArray) && ChildrenArray)
		{
			CreateComponentsFromTree(AssetPath, *ChildrenArray, CompName, Result);
		}
	}
}

// =============================================================================
// AddInterfaces
// =============================================================================

void FOliveLibraryCloner::AddInterfaces(
	UBlueprint* Blueprint,
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& TemplateJson,
	FLibraryCloneResult& Result)
{
	const TArray<TSharedPtr<FJsonValue>>* InterfacesArray = nullptr;
	if (!TemplateJson->TryGetArrayField(TEXT("interfaces"), InterfacesArray) || !InterfacesArray)
	{
		return;
	}

	FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();

	for (const TSharedPtr<FJsonValue>& InterfaceVal : *InterfacesArray)
	{
		const TSharedPtr<FJsonObject>* InterfaceObj = nullptr;
		FString InterfaceName;

		if (InterfaceVal->TryGetObject(InterfaceObj) && InterfaceObj)
		{
			// Object format: {"name": "I_RangedWeaponInterface_C", "path": "..."}
			(*InterfaceObj)->TryGetStringField(TEXT("name"), InterfaceName);
		}
		else
		{
			// String format: just the interface name
			InterfaceVal->TryGetString(InterfaceName);
		}

		if (InterfaceName.IsEmpty())
		{
			continue;
		}

		// Resolve interface class
		FString RemappedName = ApplyRemap(InterfaceName);
		UClass* InterfaceClass = ResolveClass(RemappedName);
		if (!InterfaceClass)
		{
			FString Warning = FString::Printf(
				TEXT("Interface '%s' skipped: not found in project"), *InterfaceName);
			Result.Warnings.Add(Warning);
			Result.InterfacesSkipped++;
			TrackUnresolved(InterfaceName, TEXT("interface"));
			UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *Warning);
			continue;
		}

		// Check if already implemented (from ancestor flattening)
		bool bAlreadyImplemented = false;
		for (const FBPInterfaceDescription& Desc : Blueprint->ImplementedInterfaces)
		{
			if (Desc.Interface == InterfaceClass)
			{
				bAlreadyImplemented = true;
				break;
			}
		}

		if (bAlreadyImplemented)
		{
			UE_LOG(LogOliveLibraryCloner, Verbose,
				TEXT("Interface '%s' already implemented, skipping"), *InterfaceName);
			continue;
		}

		FOliveBlueprintWriteResult InterfaceResult = Writer.AddInterface(
			AssetPath, InterfaceClass->GetPathName());
		if (InterfaceResult.bSuccess)
		{
			Result.InterfacesAdded++;
		}
		else
		{
			FString ErrMsg = FString::Printf(
				TEXT("Failed to add interface '%s'"), *InterfaceName);
			if (!InterfaceResult.GetFirstError().IsEmpty())
			{
				ErrMsg += TEXT(": ") + InterfaceResult.GetFirstError();
			}
			Result.Warnings.Add(ErrMsg);
			Result.InterfacesSkipped++;
			UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *ErrMsg);
		}
	}
}

// =============================================================================
// CreateDispatchers
// =============================================================================

void FOliveLibraryCloner::CreateDispatchers(
	UBlueprint* Blueprint,
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& TemplateJson,
	FLibraryCloneResult& Result)
{
	const TArray<TSharedPtr<FJsonValue>>* DispatchersArray = nullptr;
	if (!TemplateJson->TryGetArrayField(TEXT("event_dispatchers"), DispatchersArray) || !DispatchersArray)
	{
		return;
	}

	FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();

	for (const TSharedPtr<FJsonValue>& DispVal : *DispatchersArray)
	{
		const TSharedPtr<FJsonObject>* DispObj = nullptr;
		FString DispName;

		if (DispVal->TryGetObject(DispObj) && DispObj)
		{
			(*DispObj)->TryGetStringField(TEXT("name"), DispName);
		}
		else
		{
			// Simple string format (just a name, no params)
			DispVal->TryGetString(DispName);
		}

		if (DispName.IsEmpty())
		{
			Result.Warnings.Add(TEXT("Skipped dispatcher with empty name"));
			continue;
		}

		// Check if already exists (from ancestor flattening)
		bool bAlreadyExists = false;
		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			if (Var.VarName == FName(*DispName) &&
				Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				bAlreadyExists = true;
				break;
			}
		}
		if (bAlreadyExists)
		{
			UE_LOG(LogOliveLibraryCloner, Verbose,
				TEXT("Dispatcher '%s' already exists, skipping"), *DispName);
			continue;
		}

		// Parse delegate params
		TArray<FOliveIRFunctionParam> DispParams;
		bool bParamTypeUnresolvable = false;

		if (DispObj)
		{
			const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
			if ((*DispObj)->TryGetArrayField(TEXT("params"), ParamsArray) && ParamsArray)
			{
				for (const TSharedPtr<FJsonValue>& PVal : *ParamsArray)
				{
					const TSharedPtr<FJsonObject>* PObj = nullptr;
					if (!PVal->TryGetObject(PObj) || !PObj)
					{
						continue;
					}

					FOliveIRFunctionParam Param;
					(*PObj)->TryGetStringField(TEXT("name"), Param.Name);

					// Parse param type
					const TSharedPtr<FJsonObject>* TypeObj = nullptr;
					FString TypeStr;
					if ((*PObj)->TryGetObjectField(TEXT("type"), TypeObj) && TypeObj)
					{
						FParsedLibraryType LibType = ParseLibraryType(*TypeObj);
						Param.Type.Category = ParseCategoryString(LibType.Category);

						// Resolve object/struct types for delegate params
						if (Param.Type.Category == EOliveIRTypeCategory::Object && !LibType.ClassName.IsEmpty())
						{
							UClass* ParamClass = ResolveClass(LibType.ClassName);
							if (ParamClass)
							{
								Param.Type.ClassName = ParamClass->GetName();
							}
							else
							{
								// Demote to UObject*
								Param.Type.ClassName = TEXT("Object");
								Result.Warnings.Add(FString::Printf(
									TEXT("Dispatcher '%s' param '%s' type demoted: %s -> Object"),
									*DispName, *Param.Name, *LibType.ClassName));
								TrackUnresolved(LibType.ClassName,
									FString::Printf(TEXT("dispatcher: %s param %s"), *DispName, *Param.Name));
							}
						}
						else if (Param.Type.Category == EOliveIRTypeCategory::Struct && !LibType.StructName.IsEmpty())
						{
							UScriptStruct* ParamStruct = ResolveStruct(LibType.StructName);
							if (ParamStruct)
							{
								Param.Type.StructName = ParamStruct->GetName();
							}
							else
							{
								// Cannot demote struct params in delegates easily; skip whole dispatcher
								bParamTypeUnresolvable = true;
								FString Warning = FString::Printf(
									TEXT("Dispatcher '%s' skipped: param '%s' struct type '%s' not found"),
									*DispName, *Param.Name, *LibType.StructName);
								Result.Warnings.Add(Warning);
								TrackUnresolved(LibType.StructName,
									FString::Printf(TEXT("dispatcher: %s param %s (struct)"), *DispName, *Param.Name));
								UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *Warning);
								break;
							}
						}
					}
					else if ((*PObj)->TryGetStringField(TEXT("type"), TypeStr))
					{
						Param.Type.Category = ParseCategoryString(TypeStr);
					}

					DispParams.Add(Param);
				}
			}
		}

		if (bParamTypeUnresolvable)
		{
			Result.DispatchersSkipped++;
			continue;
		}

		FOliveBlueprintWriteResult DispResult = Writer.AddEventDispatcher(AssetPath, DispName, DispParams);
		if (DispResult.bSuccess)
		{
			Result.DispatchersCreated++;
		}
		else
		{
			FString ErrMsg = FString::Printf(TEXT("Failed to add dispatcher '%s'"), *DispName);
			if (!DispResult.GetFirstError().IsEmpty())
			{
				ErrMsg += TEXT(": ") + DispResult.GetFirstError();
			}
			Result.Warnings.Add(ErrMsg);
			Result.DispatchersSkipped++;
			UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *ErrMsg);
		}
	}
}

// =============================================================================
// CreateFunctionSignatures
// =============================================================================

void FOliveLibraryCloner::CreateFunctionSignatures(
	UBlueprint* Blueprint,
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& TemplateJson,
	FLibraryCloneResult& Result)
{
	// Library templates store functions in: graphs.functions (array)
	const TSharedPtr<FJsonObject>* GraphsObj = nullptr;
	if (!TemplateJson->TryGetObjectField(TEXT("graphs"), GraphsObj) || !GraphsObj)
	{
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
	if (!(*GraphsObj)->TryGetArrayField(TEXT("functions"), FunctionsArray) || !FunctionsArray)
	{
		return;
	}

	FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();

	for (const TSharedPtr<FJsonValue>& FuncVal : *FunctionsArray)
	{
		const TSharedPtr<FJsonObject>* FuncObj = nullptr;
		if (!FuncVal->TryGetObject(FuncObj) || !FuncObj)
		{
			continue;
		}

		FString FuncName;
		(*FuncObj)->TryGetStringField(TEXT("name"), FuncName);
		if (FuncName.IsEmpty())
		{
			Result.Warnings.Add(TEXT("Skipped function with empty name"));
			continue;
		}

		// Check if function already exists (from ancestor flattening)
		bool bAlreadyExists = false;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == FName(*FuncName))
			{
				bAlreadyExists = true;
				break;
			}
		}
		if (bAlreadyExists)
		{
			UE_LOG(LogOliveLibraryCloner, Verbose,
				TEXT("Function '%s' already exists, skipping"), *FuncName);
			continue;
		}

		// Build function signature
		FOliveIRFunctionSignature Sig;
		Sig.Name = FuncName;

		// Parse function metadata
		FString Access;
		if ((*FuncObj)->TryGetStringField(TEXT("access"), Access))
		{
			Sig.bIsPublic = !Access.Equals(TEXT("private"), ESearchCase::IgnoreCase);
		}

		FString Category;
		if ((*FuncObj)->TryGetStringField(TEXT("category"), Category))
		{
			Sig.Category = Category;
		}

		bool bIsPure = false;
		if ((*FuncObj)->TryGetBoolField(TEXT("is_pure"), bIsPure))
		{
			Sig.bIsPure = bIsPure;
		}

		bool bIsConst = false;
		if ((*FuncObj)->TryGetBoolField(TEXT("is_const"), bIsConst))
		{
			Sig.bIsConst = bIsConst;
		}

		FString Description;
		if ((*FuncObj)->TryGetStringField(TEXT("description"), Description))
		{
			Sig.Description = Description;
		}

		// Parse inputs
		const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
		if ((*FuncObj)->TryGetArrayField(TEXT("inputs"), InputsArray) && InputsArray)
		{
			for (const TSharedPtr<FJsonValue>& InVal : *InputsArray)
			{
				const TSharedPtr<FJsonObject>* InObj = nullptr;
				if (!InVal->TryGetObject(InObj) || !InObj)
				{
					continue;
				}

				FOliveIRFunctionParam Param;
				(*InObj)->TryGetStringField(TEXT("name"), Param.Name);
				(*InObj)->TryGetBoolField(TEXT("is_reference"), Param.bIsReference);

				// Parse param type
				const TSharedPtr<FJsonObject>* TypeObj = nullptr;
				FString TypeStr;
				if ((*InObj)->TryGetObjectField(TEXT("type"), TypeObj) && TypeObj)
				{
					FParsedLibraryType LibType = ParseLibraryType(*TypeObj);
					Param.Type.Category = ParseCategoryString(LibType.Category);
					Param.Type.ClassName = LibType.ClassName;
					Param.Type.StructName = LibType.StructName;
					Param.Type.EnumName = LibType.EnumName;
					Param.Type.ElementTypeJson = LibType.ElementType;

					// Resolve complex types
					if (Param.Type.Category == EOliveIRTypeCategory::Object && !LibType.ClassName.IsEmpty())
					{
						UClass* ParamClass = ResolveClass(LibType.ClassName);
						if (ParamClass)
						{
							Param.Type.ClassName = ParamClass->GetName();
						}
						else
						{
							Param.Type.ClassName = TEXT("Object");
							Result.Warnings.Add(FString::Printf(
								TEXT("Function '%s' input '%s' type demoted: %s -> Object"),
								*FuncName, *Param.Name, *LibType.ClassName));
							TrackUnresolved(LibType.ClassName,
								FString::Printf(TEXT("function: %s input %s"), *FuncName, *Param.Name));
						}
					}
					else if (Param.Type.Category == EOliveIRTypeCategory::Struct && !LibType.StructName.IsEmpty())
					{
						UScriptStruct* ParamStruct = ResolveStruct(LibType.StructName);
						if (ParamStruct)
						{
							Param.Type.StructName = ParamStruct->GetName();
						}
						else
						{
							// Demote to String for unresolvable struct params
							Param.Type.Category = EOliveIRTypeCategory::String;
							Param.Type.StructName.Empty();
							Result.Warnings.Add(FString::Printf(
								TEXT("Function '%s' input '%s' type demoted: struct %s -> String"),
								*FuncName, *Param.Name, *LibType.StructName));
							TrackUnresolved(LibType.StructName,
								FString::Printf(TEXT("function: %s input %s (struct)"), *FuncName, *Param.Name));
						}
					}
				}
				else if ((*InObj)->TryGetStringField(TEXT("type"), TypeStr))
				{
					Param.Type.Category = ParseCategoryString(TypeStr);
				}

				Sig.Inputs.Add(Param);
			}
		}

		// Parse outputs
		const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
		if ((*FuncObj)->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray)
		{
			for (const TSharedPtr<FJsonValue>& OutVal : *OutputsArray)
			{
				const TSharedPtr<FJsonObject>* OutObj = nullptr;
				if (!OutVal->TryGetObject(OutObj) || !OutObj)
				{
					continue;
				}

				FOliveIRFunctionParam OutParam;
				(*OutObj)->TryGetStringField(TEXT("name"), OutParam.Name);
				OutParam.bIsOutParam = true;

				// Parse output type
				const TSharedPtr<FJsonObject>* TypeObj = nullptr;
				FString TypeStr;
				if ((*OutObj)->TryGetObjectField(TEXT("type"), TypeObj) && TypeObj)
				{
					FParsedLibraryType LibType = ParseLibraryType(*TypeObj);
					OutParam.Type.Category = ParseCategoryString(LibType.Category);
					OutParam.Type.ClassName = LibType.ClassName;
					OutParam.Type.StructName = LibType.StructName;
					OutParam.Type.EnumName = LibType.EnumName;
					OutParam.Type.ElementTypeJson = LibType.ElementType;

					// Resolve complex types
					if (OutParam.Type.Category == EOliveIRTypeCategory::Object && !LibType.ClassName.IsEmpty())
					{
						UClass* OutClass = ResolveClass(LibType.ClassName);
						if (OutClass)
						{
							OutParam.Type.ClassName = OutClass->GetName();
						}
						else
						{
							OutParam.Type.ClassName = TEXT("Object");
							Result.Warnings.Add(FString::Printf(
								TEXT("Function '%s' output '%s' type demoted: %s -> Object"),
								*FuncName, *OutParam.Name, *LibType.ClassName));
							TrackUnresolved(LibType.ClassName,
								FString::Printf(TEXT("function: %s output %s"), *FuncName, *OutParam.Name));
						}
					}
					else if (OutParam.Type.Category == EOliveIRTypeCategory::Struct && !LibType.StructName.IsEmpty())
					{
						UScriptStruct* OutStruct = ResolveStruct(LibType.StructName);
						if (OutStruct)
						{
							OutParam.Type.StructName = OutStruct->GetName();
						}
						else
						{
							OutParam.Type.Category = EOliveIRTypeCategory::String;
							OutParam.Type.StructName.Empty();
							Result.Warnings.Add(FString::Printf(
								TEXT("Function '%s' output '%s' type demoted: struct %s -> String"),
								*FuncName, *OutParam.Name, *LibType.StructName));
							TrackUnresolved(LibType.StructName,
								FString::Printf(TEXT("function: %s output %s (struct)"), *FuncName, *OutParam.Name));
						}
					}
				}
				else if ((*OutObj)->TryGetStringField(TEXT("type"), TypeStr))
				{
					OutParam.Type.Category = ParseCategoryString(TypeStr);
				}

				Sig.Outputs.Add(OutParam);
			}
		}

		// Create the function graph (empty, with signature)
		FOliveBlueprintWriteResult FuncResult = Writer.AddFunction(AssetPath, Sig);
		if (FuncResult.bSuccess)
		{
			Result.FunctionsCreated++;
			UE_LOG(LogOliveLibraryCloner, Log,
				TEXT("Created function signature '%s' (%d inputs, %d outputs)"),
				*FuncName, Sig.Inputs.Num(), Sig.Outputs.Num());
		}
		else
		{
			FString ErrMsg = FString::Printf(TEXT("Failed to create function '%s'"), *FuncName);
			if (!FuncResult.GetFirstError().IsEmpty())
			{
				ErrMsg += TEXT(": ") + FuncResult.GetFirstError();
			}
			Result.Warnings.Add(ErrMsg);
			UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *ErrMsg);
		}
	}
}

// =============================================================================
// BuildRemapSuggestions
// =============================================================================

void FOliveLibraryCloner::BuildRemapSuggestions(FLibraryCloneResult& Result)
{
	for (const auto& Pair : UnresolvedTypes)
	{
		FLibraryCloneResult::FRemapSuggestion Suggestion;
		Suggestion.SourceName = Pair.Key;
		Suggestion.UsedIn = Pair.Value;

		// Generate a human-readable suggestion based on usage context
		if (Pair.Value.Contains(TEXT("parent_class")))
		{
			Suggestion.Suggestion = FString::Printf(
				TEXT("Map '%s' to your project's equivalent class, or use a native class (e.g., Actor, Pawn)"),
				*Pair.Key);
		}
		else if (Pair.Key.EndsWith(TEXT("Component_C")) || Pair.Key.Contains(TEXT("Component")))
		{
			Suggestion.Suggestion = FString::Printf(
				TEXT("Map '%s' to your project's component class if you have one"),
				*Pair.Key);
		}
		else if (Pair.Key.StartsWith(TEXT("S_")) || Pair.Key.StartsWith(TEXT("F")))
		{
			Suggestion.Suggestion = FString::Printf(
				TEXT("Map '%s' to your project's equivalent struct"),
				*Pair.Key);
		}
		else if (Pair.Key.StartsWith(TEXT("I_")))
		{
			Suggestion.Suggestion = FString::Printf(
				TEXT("Map '%s' to your project's interface if you have one"),
				*Pair.Key);
		}
		else
		{
			int32 FuncCount = 0;
			int32 VarCount = 0;
			for (const FString& Usage : Pair.Value)
			{
				if (Usage.Contains(TEXT("function"))) FuncCount++;
				if (Usage.Contains(TEXT("variable"))) VarCount++;
			}

			if (FuncCount > 0 && VarCount > 0)
			{
				Suggestion.Suggestion = FString::Printf(
					TEXT("Map '%s' to your project's equivalent class (used in %d variable(s) and %d function(s))"),
					*Pair.Key, VarCount, FuncCount);
			}
			else
			{
				Suggestion.Suggestion = FString::Printf(
					TEXT("Map '%s' to your project's equivalent class"),
					*Pair.Key);
			}
		}

		Result.RemapSuggestions.Add(MoveTemp(Suggestion));
	}

	// Sort suggestions by number of usages (most impactful first)
	Result.RemapSuggestions.Sort([](const FLibraryCloneResult::FRemapSuggestion& A,
		const FLibraryCloneResult::FRemapSuggestion& B)
	{
		return A.UsedIn.Num() > B.UsedIn.Num();
	});
}

// =============================================================================
// VariableExistsOnBlueprint
// =============================================================================

bool FOliveLibraryCloner::VariableExistsOnBlueprint(
	const UBlueprint* Blueprint, const FString& VarName) const
{
	if (!Blueprint || VarName.IsEmpty())
	{
		return false;
	}

	// Check NewVariables
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName.IsEqual(FName(*VarName), ENameCase::IgnoreCase))
		{
			return true;
		}
	}

	// Check FlattenedVariableNames (from ancestor flattening)
	if (FlattenedVariableNames.Contains(VarName))
	{
		return true;
	}

	// Check SCS components (components ARE variables)
	if (Blueprint->SimpleConstructionScript)
	{
		const TArray<USCS_Node*>& AllNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
		for (const USCS_Node* SCSNode : AllNodes)
		{
			if (SCSNode && SCSNode->GetVariableName().IsEqual(FName(*VarName), ENameCase::IgnoreCase))
			{
				return true;
			}
		}
	}

	return false;
}

// =============================================================================
// Graph Cloning (Phase 2)
// =============================================================================

namespace
{
	/**
	 * Strip the "Receive" prefix from event function names.
	 * ReceiveBeginPlay -> BeginPlay, ReceiveEndPlay -> EndPlay, etc.
	 */
	FString StripEventPrefix(const FString& FunctionName)
	{
		if (FunctionName.StartsWith(TEXT("Receive")))
		{
			return FunctionName.Mid(7); // len("Receive") = 7
		}
		return FunctionName;
	}

	/**
	 * Parse a connection string of the form "node_X.PinName".
	 * Splits on the FIRST dot only, since pin names may contain dots.
	 */
	bool ParseConnectionString(const FString& Connection, FString& OutNodeId, FString& OutPinName)
	{
		int32 DotIndex = INDEX_NONE;
		if (!Connection.FindChar(TEXT('.'), DotIndex) || DotIndex <= 0)
		{
			return false;
		}
		OutNodeId = Connection.Left(DotIndex);
		OutPinName = Connection.Mid(DotIndex + 1);
		return !OutNodeId.IsEmpty() && !OutPinName.IsEmpty();
	}

	/**
	 * Check if a library node type is an exec pin type (for connection scanning).
	 */
	bool IsExecPin(const TSharedPtr<FJsonObject>& PinObj)
	{
		bool bIsExec = false;
		if (PinObj->TryGetBoolField(TEXT("is_exec"), bIsExec) && bIsExec)
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* TypeObj = nullptr;
		if (PinObj->TryGetObjectField(TEXT("type"), TypeObj) && TypeObj)
		{
			FString Category;
			if ((*TypeObj)->TryGetStringField(TEXT("category"), Category))
			{
				return Category.Equals(TEXT("exec"), ESearchCase::IgnoreCase);
			}
		}

		return false;
	}

	/**
	 * Check if a class name looks like it belongs to a source project
	 * (i.e., starts with a project-specific prefix or has _C suffix for BPs).
	 */
	bool IsLikelySourceProjectClass(const FString& ClassName)
	{
		// If it has a _C suffix, it's a Blueprint-generated class
		if (ClassName.EndsWith(TEXT("_C")))
		{
			return true;
		}

		// Check some common project prefixes that aren't engine classes
		if (ClassName.StartsWith(TEXT("BP_")) || ClassName.StartsWith(TEXT("W_")) ||
			ClassName.StartsWith(TEXT("BPI_")))
		{
			return true;
		}

		return false;
	}
}

FCloneGraphResult FOliveLibraryCloner::CloneGraph(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FString& AssetPath,
	const TSharedPtr<FJsonObject>& GraphJson)
{
	FCloneGraphResult GraphResult;
	GraphResult.GraphName = Graph ? Graph->GetName() : TEXT("Unknown");

	if (!Blueprint || !Graph || !GraphJson.IsValid())
	{
		GraphResult.Warnings.Add(TEXT("Invalid Blueprint, Graph, or GraphJson"));
		return GraphResult;
	}

	// Get the nodes array from the graph JSON
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (!GraphJson->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray || NodesArray->Num() == 0)
	{
		UE_LOG(LogOliveLibraryCloner, Log,
			TEXT("Graph '%s' has no nodes to clone"), *GraphResult.GraphName);
		return GraphResult;
	}

	UE_LOG(LogOliveLibraryCloner, Log,
		TEXT("Cloning graph '%s' (%d nodes)"), *GraphResult.GraphName, NodesArray->Num());

	// Phase 1: Classify all nodes
	TMap<FString, FCloneNodeClassification> Classifications = ClassifyNodes(Blueprint, *NodesArray);

	// Phase 2: Create nodes
	CreateNodes(Blueprint, Graph, *NodesArray, Classifications, GraphResult);

	// Phase 3: Wire exec connections + exec gap repair
	WireExecConnections(*NodesArray, GraphResult);

	// Phase 4: Wire data connections
	WireDataConnections(*NodesArray, GraphResult);

	// Phase 5: Set pin defaults
	SetPinDefaults(*NodesArray, GraphResult);

	// Phase 6: Auto-layout (simple grid-based placement since we don't have plan context)
	// FOliveGraphLayoutEngine requires an FOliveIRBlueprintPlan and FOlivePlanExecutionContext,
	// which we don't have in the cloner. Instead, do a simple column-based layout.
	{
		int32 Col = 0;
		int32 Row = 0;
		constexpr int32 HSpacing = 350;
		constexpr int32 VSpacing = 200;
		constexpr int32 MaxRows = 10;

		for (const auto& Pair : NodeMap)
		{
			if (UEdGraphNode* Node = Pair.Value)
			{
				Node->NodePosX = Col * HSpacing;
				Node->NodePosY = Row * VSpacing;

				Row++;
				if (Row >= MaxRows)
				{
					Row = 0;
					Col++;
				}
			}
		}
	}

	UE_LOG(LogOliveLibraryCloner, Log,
		TEXT("Graph '%s' cloned: %d created, %d skipped, %d connections (%d failed), %d defaults, %d gaps bridged"),
		*GraphResult.GraphName, GraphResult.NodesCreated, GraphResult.NodesSkipped,
		GraphResult.ConnectionsSucceeded, GraphResult.ConnectionsFailed,
		GraphResult.DefaultsSet, GraphResult.ExecGapsBridged);

	return GraphResult;
}

// =============================================================================
// ClassifyNodes
// =============================================================================

TMap<FString, FCloneNodeClassification> FOliveLibraryCloner::ClassifyNodes(
	UBlueprint* Blueprint,
	const TArray<TSharedPtr<FJsonValue>>& NodesArray)
{
	TMap<FString, FCloneNodeClassification> Results;

	for (const TSharedPtr<FJsonValue>& NodeVal : NodesArray)
	{
		const TSharedPtr<FJsonObject>* NodeObj = nullptr;
		if (!NodeVal->TryGetObject(NodeObj) || !NodeObj)
		{
			continue;
		}

		FString NodeId;
		(*NodeObj)->TryGetStringField(TEXT("id"), NodeId);
		if (NodeId.IsEmpty())
		{
			continue;
		}

		FString NodeType;
		(*NodeObj)->TryGetStringField(TEXT("type"), NodeType);

		FCloneNodeClassification Classification;

		// ------- FunctionEntry / FunctionResult: Always Skip -------
		if (NodeType == TEXT("FunctionEntry") || NodeType == TEXT("FunctionResult"))
		{
			Classification.Disposition = ECloneNodeDisposition::Skip;
			Classification.SkipReason = TEXT("Already created by CreateFunctionSignatures");
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- Knot / Reroute: Always Skip -------
		if (NodeType == TEXT("Knot") || NodeType == TEXT("Reroute"))
		{
			Classification.Disposition = ECloneNodeDisposition::Skip;
			Classification.SkipReason = TEXT("Visual-only routing node");
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- ControlRigGraphNode: Always Skip -------
		if (NodeType == TEXT("ControlRigGraphNode"))
		{
			Classification.Disposition = ECloneNodeDisposition::Skip;
			Classification.SkipReason = TEXT("ControlRig nodes not supported in cloner");
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- Timeline: Skip in structure/portable, create shell in full -------
		if (NodeType == TEXT("Timeline"))
		{
			if (CurrentMode == ELibraryCloneMode::Full)
			{
				// DESIGN NOTE: Timeline nodes require the dedicated create_timeline tool.
				// We skip them in the cloner for now and log a warning.
				Classification.Disposition = ECloneNodeDisposition::Skip;
				Classification.SkipReason = TEXT("Timeline nodes require blueprint.create_timeline tool");
			}
			else
			{
				Classification.Disposition = ECloneNodeDisposition::Skip;
				Classification.SkipReason = TEXT("Timeline nodes skipped in structure/portable mode");
			}
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- Branch: Always Create -------
		if (NodeType == TEXT("Branch"))
		{
			Classification.Disposition = ECloneNodeDisposition::Create;
			Classification.ResolvedNodeType = OliveNodeTypes::Branch;
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- Sequence: Always Create -------
		if (NodeType == TEXT("Sequence"))
		{
			Classification.Disposition = ECloneNodeDisposition::Create;
			Classification.ResolvedNodeType = OliveNodeTypes::Sequence;

			// Count exec output pins to determine num_outputs
			const TArray<TSharedPtr<FJsonValue>>* PinsOut = nullptr;
			if ((*NodeObj)->TryGetArrayField(TEXT("pins_out"), PinsOut) && PinsOut)
			{
				int32 ExecOutputCount = 0;
				for (const TSharedPtr<FJsonValue>& PinVal : *PinsOut)
				{
					const TSharedPtr<FJsonObject>* PinObj = nullptr;
					if (PinVal->TryGetObject(PinObj) && PinObj && IsExecPin(*PinObj))
					{
						ExecOutputCount++;
					}
				}
				if (ExecOutputCount > 2)
				{
					Classification.Properties.Add(TEXT("num_outputs"),
						FString::FromInt(ExecOutputCount));
				}
			}
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- Event: Always Create -------
		if (NodeType == TEXT("Event"))
		{
			Classification.Disposition = ECloneNodeDisposition::Create;
			Classification.ResolvedNodeType = OliveNodeTypes::Event;

			FString FuncName;
			(*NodeObj)->TryGetStringField(TEXT("function"), FuncName);
			Classification.Properties.Add(TEXT("event_name"), StripEventPrefix(FuncName));
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- CustomEvent: Always Create -------
		if (NodeType == TEXT("CustomEvent"))
		{
			Classification.Disposition = ECloneNodeDisposition::Create;
			Classification.ResolvedNodeType = OliveNodeTypes::CustomEvent;

			FString FuncName;
			(*NodeObj)->TryGetStringField(TEXT("function"), FuncName);
			Classification.Properties.Add(TEXT("event_name"), FuncName);
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- Comment: Always Create -------
		if (NodeType == TEXT("Comment"))
		{
			Classification.Disposition = ECloneNodeDisposition::Create;
			Classification.ResolvedNodeType = OliveNodeTypes::Comment;

			FString Title;
			(*NodeObj)->TryGetStringField(TEXT("title"), Title);
			Classification.Properties.Add(TEXT("comment_text"), Title);
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- VariableGet: Create if variable exists -------
		if (NodeType == TEXT("VariableGet"))
		{
			FString VarName;
			(*NodeObj)->TryGetStringField(TEXT("variable"), VarName);

			if (VariableExistsOnBlueprint(Blueprint, VarName))
			{
				Classification.Disposition = ECloneNodeDisposition::Create;
				Classification.ResolvedNodeType = OliveNodeTypes::GetVariable;
				Classification.Properties.Add(TEXT("variable_name"), VarName);
			}
			else
			{
				Classification.Disposition = ECloneNodeDisposition::Skip;
				Classification.SkipReason = FString::Printf(
					TEXT("Variable '%s' not found on Blueprint"), *VarName);
				TrackUnresolved(VarName, TEXT("variable_get node"));
			}
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- VariableSet: Create if variable exists -------
		if (NodeType == TEXT("VariableSet"))
		{
			FString VarName;
			(*NodeObj)->TryGetStringField(TEXT("variable"), VarName);

			if (VariableExistsOnBlueprint(Blueprint, VarName))
			{
				Classification.Disposition = ECloneNodeDisposition::Create;
				Classification.ResolvedNodeType = OliveNodeTypes::SetVariable;
				Classification.Properties.Add(TEXT("variable_name"), VarName);
			}
			else
			{
				Classification.Disposition = ECloneNodeDisposition::Skip;
				Classification.SkipReason = FString::Printf(
					TEXT("Variable '%s' not found on Blueprint"), *VarName);
				TrackUnresolved(VarName, TEXT("variable_set node"));
			}
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- Cast: Resolve target class -------
		if (NodeType == TEXT("Cast"))
		{
			FString TargetClassName;

			// Try properties.TargetClass first, then properties.TargetType
			const TSharedPtr<FJsonObject>* PropsObj = nullptr;
			if ((*NodeObj)->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
			{
				if (!(*PropsObj)->TryGetStringField(TEXT("TargetClass"), TargetClassName))
				{
					(*PropsObj)->TryGetStringField(TEXT("TargetType"), TargetClassName);
				}
			}

			// Fallback: parse from title "Cast To X" -> "X"
			if (TargetClassName.IsEmpty())
			{
				FString Title;
				(*NodeObj)->TryGetStringField(TEXT("title"), Title);
				if (Title.StartsWith(TEXT("Cast To ")))
				{
					TargetClassName = Title.Mid(8); // len("Cast To ") = 8
				}
			}

			// Strip _C suffix before resolving
			FString CleanClassName = TargetClassName;
			if (CleanClassName.EndsWith(TEXT("_C")))
			{
				CleanClassName = CleanClassName.LeftChop(2);
			}

			UClass* ResolvedCastClass = ResolveClass(CleanClassName);
			if (ResolvedCastClass)
			{
				Classification.Disposition = ECloneNodeDisposition::Create;
				Classification.ResolvedNodeType = OliveNodeTypes::Cast;
				Classification.Properties.Add(TEXT("target_class"), ResolvedCastClass->GetName());
			}
			else if (CurrentMode == ELibraryCloneMode::Full)
			{
				// In Full mode, still try to create; it may fail at node creation
				Classification.Disposition = ECloneNodeDisposition::Create;
				Classification.ResolvedNodeType = OliveNodeTypes::Cast;
				Classification.Properties.Add(TEXT("target_class"), CleanClassName);
			}
			else
			{
				Classification.Disposition = ECloneNodeDisposition::Skip;
				Classification.SkipReason = FString::Printf(
					TEXT("Cast target class '%s' not found in project"), *TargetClassName);
				TrackUnresolved(TargetClassName, TEXT("cast node"));
			}
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- CallFunction: Resolve owning class + function -------
		if (NodeType == TEXT("CallFunction"))
		{
			FString FunctionName;
			FString OwningClass;
			(*NodeObj)->TryGetStringField(TEXT("function"), FunctionName);
			(*NodeObj)->TryGetStringField(TEXT("owning_class"), OwningClass);

			bool bIsSelfCall = OwningClass.IsEmpty() ||
				OwningClass.Equals(TEXT("Self"), ESearchCase::IgnoreCase) ||
				OwningClass.Equals(TemplateClassName, ESearchCase::IgnoreCase) ||
				(OwningClass + TEXT("_C")).Equals(TemplateClassName + TEXT("_C"), ESearchCase::IgnoreCase);

			if (bIsSelfCall)
			{
				// Self calls always create; FindFunction will search the Blueprint
				Classification.Disposition = ECloneNodeDisposition::Create;
				Classification.ResolvedNodeType = OliveNodeTypes::CallFunction;
				Classification.Properties.Add(TEXT("function_name"), FunctionName);
				// Omit target_class for self calls
			}
			else
			{
				// Try to resolve the owning class
				FString CleanOwner = OwningClass;
				if (CleanOwner.EndsWith(TEXT("_C")))
				{
					CleanOwner = CleanOwner.LeftChop(2);
				}

				// Try FindFunction to see if the function exists anywhere
				FOliveNodeFactory& Factory = FOliveNodeFactory::Get();
				UFunction* FoundFunc = Factory.FindFunction(FunctionName, CleanOwner, Blueprint);

				if (FoundFunc)
				{
					Classification.Disposition = ECloneNodeDisposition::Create;
					Classification.ResolvedNodeType = OliveNodeTypes::CallFunction;
					Classification.Properties.Add(TEXT("function_name"), FunctionName);

					// Only add target_class if it's not the BP itself
					UClass* FuncOwner = FoundFunc->GetOwnerClass();
					if (FuncOwner && !Blueprint->GeneratedClass->IsChildOf(FuncOwner))
					{
						Classification.Properties.Add(TEXT("target_class"), FuncOwner->GetName());
					}
				}
				else if (IsLikelySourceProjectClass(OwningClass) &&
						 CurrentMode == ELibraryCloneMode::Portable)
				{
					Classification.Disposition = ECloneNodeDisposition::Skip;
					Classification.SkipReason = FString::Printf(
						TEXT("Function '%s' on class '%s' not found (source-project specific)"),
						*FunctionName, *OwningClass);
					TrackUnresolved(OwningClass,
						FString::Printf(TEXT("call: %s"), *FunctionName));
				}
				else if (CurrentMode == ELibraryCloneMode::Full)
				{
					// In Full mode, try anyway
					Classification.Disposition = ECloneNodeDisposition::Create;
					Classification.ResolvedNodeType = OliveNodeTypes::CallFunction;
					Classification.Properties.Add(TEXT("function_name"), FunctionName);
					if (!CleanOwner.IsEmpty())
					{
						Classification.Properties.Add(TEXT("target_class"), CleanOwner);
					}
				}
				else
				{
					Classification.Disposition = ECloneNodeDisposition::Skip;
					Classification.SkipReason = FString::Printf(
						TEXT("Function '%s' on class '%s' not found"),
						*FunctionName, *OwningClass);
					TrackUnresolved(OwningClass,
						FString::Printf(TEXT("call: %s"), *FunctionName));
				}
			}
			Results.Add(NodeId, MoveTemp(Classification));
			continue;
		}

		// ------- Unknown node type: Skip -------
		Classification.Disposition = ECloneNodeDisposition::Skip;
		Classification.SkipReason = FString::Printf(
			TEXT("Unrecognized node type '%s'"), *NodeType);
		Results.Add(NodeId, MoveTemp(Classification));
	}

	return Results;
}

// =============================================================================
// CreateNodes
// =============================================================================

void FOliveLibraryCloner::CreateNodes(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TArray<TSharedPtr<FJsonValue>>& NodesArray,
	const TMap<FString, FCloneNodeClassification>& Classifications,
	FCloneGraphResult& Result)
{
	FOliveNodeFactory& Factory = FOliveNodeFactory::Get();

	for (const TSharedPtr<FJsonValue>& NodeVal : NodesArray)
	{
		const TSharedPtr<FJsonObject>* NodeObj = nullptr;
		if (!NodeVal->TryGetObject(NodeObj) || !NodeObj)
		{
			continue;
		}

		FString NodeId;
		(*NodeObj)->TryGetStringField(TEXT("id"), NodeId);
		if (NodeId.IsEmpty())
		{
			continue;
		}

		const FCloneNodeClassification* Classification = Classifications.Find(NodeId);
		if (!Classification)
		{
			Result.NodesSkipped++;
			continue;
		}

		if (Classification->Disposition != ECloneNodeDisposition::Create)
		{
			if (!Classification->SkipReason.IsEmpty())
			{
				UE_LOG(LogOliveLibraryCloner, Verbose,
					TEXT("Skipping node '%s': %s"), *NodeId, *Classification->SkipReason);
			}
			Result.NodesSkipped++;
			continue;
		}

		// Create the node using the factory
		UEdGraphNode* CreatedNode = Factory.CreateNode(
			Blueprint, Graph,
			Classification->ResolvedNodeType,
			Classification->Properties,
			0, 0);  // Layout is applied in Phase 6

		if (CreatedNode)
		{
			NodeMap.Add(NodeId, CreatedNode);
			Result.NodesCreated++;

			UE_LOG(LogOliveLibraryCloner, Verbose,
				TEXT("Created node '%s' (%s)"), *NodeId, *Classification->ResolvedNodeType);
		}
		else
		{
			FString FactoryError = Factory.GetLastError();
			FString Warning = FString::Printf(
				TEXT("Failed to create node '%s' (%s): %s"),
				*NodeId, *Classification->ResolvedNodeType, *FactoryError);
			Result.Warnings.Add(Warning);
			Result.NodesSkipped++;
			UE_LOG(LogOliveLibraryCloner, Warning, TEXT("%s"), *Warning);
		}
	}
}

// =============================================================================
// WireExecConnections
// =============================================================================

void FOliveLibraryCloner::WireExecConnections(
	const TArray<TSharedPtr<FJsonValue>>& NodesArray,
	FCloneGraphResult& Result)
{
	FOlivePinConnector& Connector = FOlivePinConnector::Get();

	// Track skipped node exec topology for gap repair
	// SkippedNodeId -> { incoming (source nodeId.pinName), outgoing (target nodeId.pinName) }
	struct FSkippedExecInfo
	{
		TArray<FString> IncomingSources;  // "node_X.PinName" that wire INTO the skipped node
		TArray<FString> OutgoingTargets;  // "node_Y.PinName" that the skipped node wires TO
	};
	TMap<FString, FSkippedExecInfo> SkippedExecMap;

	// First pass: collect exec topology for all nodes, wire those that are in NodeMap
	for (const TSharedPtr<FJsonValue>& NodeVal : NodesArray)
	{
		const TSharedPtr<FJsonObject>* NodeObj = nullptr;
		if (!NodeVal->TryGetObject(NodeObj) || !NodeObj)
		{
			continue;
		}

		FString SourceNodeId;
		(*NodeObj)->TryGetStringField(TEXT("id"), SourceNodeId);
		if (SourceNodeId.IsEmpty())
		{
			continue;
		}

		UEdGraphNode** SourceNodePtr = NodeMap.Find(SourceNodeId);

		// Scan pins_out for exec connections
		const TArray<TSharedPtr<FJsonValue>>* PinsOut = nullptr;
		if (!(*NodeObj)->TryGetArrayField(TEXT("pins_out"), PinsOut) || !PinsOut)
		{
			continue;
		}

		for (const TSharedPtr<FJsonValue>& PinVal : *PinsOut)
		{
			const TSharedPtr<FJsonObject>* PinObj = nullptr;
			if (!PinVal->TryGetObject(PinObj) || !PinObj)
			{
				continue;
			}

			if (!IsExecPin(*PinObj))
			{
				continue;
			}

			FString SourcePinName;
			(*PinObj)->TryGetStringField(TEXT("name"), SourcePinName);

			const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
			if (!(*PinObj)->TryGetArrayField(TEXT("connections"), Connections) || !Connections)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& ConnVal : *Connections)
			{
				FString ConnStr;
				if (!ConnVal->TryGetString(ConnStr))
				{
					continue;
				}

				FString TargetNodeId, TargetPinName;
				if (!ParseConnectionString(ConnStr, TargetNodeId, TargetPinName))
				{
					continue;
				}

				// Track incoming connections for skipped target nodes
				if (!NodeMap.Contains(TargetNodeId))
				{
					FSkippedExecInfo& Info = SkippedExecMap.FindOrAdd(TargetNodeId);
					Info.IncomingSources.Add(SourceNodeId + TEXT(".") + SourcePinName);
				}

				// Track outgoing connections from skipped source nodes
				if (!SourceNodePtr)
				{
					FSkippedExecInfo& Info = SkippedExecMap.FindOrAdd(SourceNodeId);
					Info.OutgoingTargets.Add(TargetNodeId + TEXT(".") + TargetPinName);
				}

				// Wire if both nodes exist
				if (!SourceNodePtr)
				{
					continue;
				}

				UEdGraphNode** TargetNodePtr = NodeMap.Find(TargetNodeId);
				if (!TargetNodePtr)
				{
					continue;
				}

				UEdGraphPin* SourcePin = FindPinByName(*SourceNodePtr, SourcePinName, EGPD_Output);
				UEdGraphPin* TargetPin = FindPinByName(*TargetNodePtr, TargetPinName, EGPD_Input);

				if (SourcePin && TargetPin)
				{
					FOliveBlueprintWriteResult WireResult = Connector.Connect(SourcePin, TargetPin);
					if (WireResult.bSuccess)
					{
						Result.ConnectionsSucceeded++;
					}
					else
					{
						Result.ConnectionsFailed++;
						UE_LOG(LogOliveLibraryCloner, Verbose,
							TEXT("Exec wire failed: %s.%s -> %s.%s: %s"),
							*SourceNodeId, *SourcePinName,
							*TargetNodeId, *TargetPinName,
							*WireResult.GetFirstError());
					}
				}
				else
				{
					Result.ConnectionsFailed++;
					UE_LOG(LogOliveLibraryCloner, Verbose,
						TEXT("Exec wire pin not found: %s.%s -> %s.%s"),
						*SourceNodeId, *SourcePinName,
						*TargetNodeId, *TargetPinName);
				}
			}
		}
	}

	// Exec Gap Repair: bridge across skipped nodes
	for (const auto& Pair : SkippedExecMap)
	{
		const FString& SkippedNodeId = Pair.Key;
		const FSkippedExecInfo& Info = Pair.Value;

		// Only bridge if exactly 1 exec-in and 1 exec-out
		if (Info.IncomingSources.Num() == 1 && Info.OutgoingTargets.Num() == 1)
		{
			FString SourceStr = Info.IncomingSources[0];
			FString TargetStr = Info.OutgoingTargets[0];

			FString SrcNodeId, SrcPinName;
			FString TgtNodeId, TgtPinName;

			if (ParseConnectionString(SourceStr, SrcNodeId, SrcPinName) &&
				ParseConnectionString(TargetStr, TgtNodeId, TgtPinName))
			{
				UEdGraphNode** SrcNode = NodeMap.Find(SrcNodeId);
				UEdGraphNode** TgtNode = NodeMap.Find(TgtNodeId);

				if (SrcNode && TgtNode)
				{
					UEdGraphPin* SrcPin = FindPinByName(*SrcNode, SrcPinName, EGPD_Output);
					UEdGraphPin* TgtPin = FindPinByName(*TgtNode, TgtPinName, EGPD_Input);

					if (SrcPin && TgtPin)
					{
						FOliveBlueprintWriteResult BridgeResult = Connector.Connect(SrcPin, TgtPin);
						if (BridgeResult.bSuccess)
						{
							Result.ExecGapsBridged++;
							UE_LOG(LogOliveLibraryCloner, Log,
								TEXT("Bridged exec gap across skipped node '%s': %s -> %s"),
								*SkippedNodeId, *SourceStr, *TargetStr);
						}
						else
						{
							Result.ExecGapsUnbridgeable++;
						}
					}
					else
					{
						Result.ExecGapsUnbridgeable++;
					}
				}
				else
				{
					Result.ExecGapsUnbridgeable++;
				}
			}
		}
		else if (Info.IncomingSources.Num() > 0 && Info.OutgoingTargets.Num() > 1)
		{
			// Multiple exec-outs: cannot bridge safely
			Result.ExecGapsUnbridgeable++;
			UE_LOG(LogOliveLibraryCloner, Warning,
				TEXT("Cannot bridge exec gap for skipped node '%s': %d incoming, %d outgoing exec connections"),
				*SkippedNodeId, Info.IncomingSources.Num(), Info.OutgoingTargets.Num());
		}
	}
}

// =============================================================================
// WireDataConnections
// =============================================================================

void FOliveLibraryCloner::WireDataConnections(
	const TArray<TSharedPtr<FJsonValue>>& NodesArray,
	FCloneGraphResult& Result)
{
	FOlivePinConnector& Connector = FOlivePinConnector::Get();

	for (const TSharedPtr<FJsonValue>& NodeVal : NodesArray)
	{
		const TSharedPtr<FJsonObject>* NodeObj = nullptr;
		if (!NodeVal->TryGetObject(NodeObj) || !NodeObj)
		{
			continue;
		}

		FString SourceNodeId;
		(*NodeObj)->TryGetStringField(TEXT("id"), SourceNodeId);
		if (SourceNodeId.IsEmpty())
		{
			continue;
		}

		UEdGraphNode** SourceNodePtr = NodeMap.Find(SourceNodeId);
		if (!SourceNodePtr)
		{
			continue; // Source node was skipped
		}

		// Scan pins_out for data (non-exec) connections
		const TArray<TSharedPtr<FJsonValue>>* PinsOut = nullptr;
		if (!(*NodeObj)->TryGetArrayField(TEXT("pins_out"), PinsOut) || !PinsOut)
		{
			continue;
		}

		for (const TSharedPtr<FJsonValue>& PinVal : *PinsOut)
		{
			const TSharedPtr<FJsonObject>* PinObj = nullptr;
			if (!PinVal->TryGetObject(PinObj) || !PinObj)
			{
				continue;
			}

			// Skip exec pins (handled in WireExecConnections)
			if (IsExecPin(*PinObj))
			{
				continue;
			}

			FString SourcePinName;
			(*PinObj)->TryGetStringField(TEXT("name"), SourcePinName);

			const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
			if (!(*PinObj)->TryGetArrayField(TEXT("connections"), Connections) || !Connections)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& ConnVal : *Connections)
			{
				FString ConnStr;
				if (!ConnVal->TryGetString(ConnStr))
				{
					continue;
				}

				FString TargetNodeId, TargetPinName;
				if (!ParseConnectionString(ConnStr, TargetNodeId, TargetPinName))
				{
					continue;
				}

				UEdGraphNode** TargetNodePtr = NodeMap.Find(TargetNodeId);
				if (!TargetNodePtr)
				{
					continue; // Target node was skipped
				}

				UEdGraphPin* SourcePin = FindPinByName(*SourceNodePtr, SourcePinName, EGPD_Output);
				UEdGraphPin* TargetPin = FindPinByName(*TargetNodePtr, TargetPinName, EGPD_Input);

				if (SourcePin && TargetPin)
				{
					FOliveBlueprintWriteResult WireResult = Connector.Connect(
						SourcePin, TargetPin, /*bAllowConversion=*/true);
					if (WireResult.bSuccess)
					{
						Result.ConnectionsSucceeded++;
					}
					else
					{
						Result.ConnectionsFailed++;
						UE_LOG(LogOliveLibraryCloner, Verbose,
							TEXT("Data wire failed: %s.%s -> %s.%s: %s"),
							*SourceNodeId, *SourcePinName,
							*TargetNodeId, *TargetPinName,
							*WireResult.GetFirstError());
					}
				}
				else
				{
					Result.ConnectionsFailed++;
					UE_LOG(LogOliveLibraryCloner, Verbose,
						TEXT("Data wire pin not found: %s.%s -> %s.%s"),
						*SourceNodeId, *SourcePinName,
						*TargetNodeId, *TargetPinName);
				}
			}
		}
	}
}

// =============================================================================
// SetPinDefaults
// =============================================================================

void FOliveLibraryCloner::SetPinDefaults(
	const TArray<TSharedPtr<FJsonValue>>& NodesArray,
	FCloneGraphResult& Result)
{
	for (const TSharedPtr<FJsonValue>& NodeVal : NodesArray)
	{
		const TSharedPtr<FJsonObject>* NodeObj = nullptr;
		if (!NodeVal->TryGetObject(NodeObj) || !NodeObj)
		{
			continue;
		}

		FString NodeId;
		(*NodeObj)->TryGetStringField(TEXT("id"), NodeId);
		if (NodeId.IsEmpty())
		{
			continue;
		}

		UEdGraphNode** CreatedNodePtr = NodeMap.Find(NodeId);
		if (!CreatedNodePtr || !(*CreatedNodePtr))
		{
			continue; // Node was skipped
		}

		UEdGraphNode* CreatedNode = *CreatedNodePtr;

		// Scan pins_in for defaults
		const TArray<TSharedPtr<FJsonValue>>* PinsIn = nullptr;
		if (!(*NodeObj)->TryGetArrayField(TEXT("pins_in"), PinsIn) || !PinsIn)
		{
			continue;
		}

		for (const TSharedPtr<FJsonValue>& PinVal : *PinsIn)
		{
			const TSharedPtr<FJsonObject>* PinObj = nullptr;
			if (!PinVal->TryGetObject(PinObj) || !PinObj)
			{
				continue;
			}

			// Skip exec pins
			if (IsExecPin(*PinObj))
			{
				continue;
			}

			FString DefaultStr;
			if (!(*PinObj)->TryGetStringField(TEXT("default"), DefaultStr))
			{
				continue;
			}

			// Skip pins that have connections (connected pins don't use defaults)
			const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
			if ((*PinObj)->TryGetArrayField(TEXT("connections"), Connections) && Connections && Connections->Num() > 0)
			{
				continue;
			}

			FString PinName;
			(*PinObj)->TryGetStringField(TEXT("name"), PinName);
			if (PinName.IsEmpty())
			{
				continue;
			}

			// Check for asset references and clear them with a warning
			if (IsAssetReference(DefaultStr))
			{
				Result.Warnings.Add(FString::Printf(
					TEXT("Pin default cleared for %s.%s: asset reference '%s'"),
					*NodeId, *PinName, *DefaultStr));
				continue;
			}

			UEdGraphPin* Pin = FindPinByName(CreatedNode, PinName, EGPD_Input);
			if (Pin)
			{
				Pin->DefaultValue = DefaultStr;
				Pin->GetOwningNode()->PinDefaultValueChanged(Pin);
				Result.DefaultsSet++;
			}
		}
	}
}

// =============================================================================
// FindPinByName
// =============================================================================

UEdGraphPin* FOliveLibraryCloner::FindPinByName(
	UEdGraphNode* Node,
	const FString& PinName,
	EEdGraphPinDirection Direction)
{
	if (!Node)
	{
		return nullptr;
	}

	// Exact match via engine API
	UEdGraphPin* ExactMatch = Node->FindPin(FName(*PinName), Direction);
	if (ExactMatch)
	{
		return ExactMatch;
	}

	// Case-insensitive fallback: iterate all pins
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction &&
			Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}

	// Try without spaces (library templates sometimes use "My Variable" but pin is "MyVariable")
	FString NoSpaces = PinName.Replace(TEXT(" "), TEXT(""));
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction)
		{
			FString PinNameNoSpaces = Pin->PinName.ToString().Replace(TEXT(" "), TEXT(""));
			if (PinNameNoSpaces.Equals(NoSpaces, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
	}

	return nullptr;
}

// =============================================================================
// IsAssetReference
// =============================================================================

bool FOliveLibraryCloner::IsAssetReference(const FString& DefaultValue) const
{
	if (DefaultValue.IsEmpty())
	{
		return false;
	}

	// Direct path references
	if (DefaultValue.StartsWith(TEXT("/Game/")) || DefaultValue.StartsWith(TEXT("/Script/")))
	{
		return true;
	}

	// UE object reference format: Class'/Game/...'
	if (DefaultValue.Contains(TEXT("'")) && DefaultValue.Contains(TEXT("/")))
	{
		// Check for pattern like: ClassName'/Path/To/Asset.Asset'
		int32 QuoteIdx = INDEX_NONE;
		if (DefaultValue.FindChar(TEXT('\''), QuoteIdx) && QuoteIdx > 0)
		{
			FString AfterQuote = DefaultValue.Mid(QuoteIdx + 1);
			if (AfterQuote.StartsWith(TEXT("/Game/")) || AfterQuote.StartsWith(TEXT("/Script/")))
			{
				return true;
			}
		}
	}

	return false;
}

// =============================================================================
// FLibraryCloneResult::ToJson
// =============================================================================

TSharedPtr<FJsonObject> FLibraryCloneResult::ToJson() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetBoolField(TEXT("success"), bSuccess);
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("template_id"), TemplateId);
	Root->SetStringField(TEXT("mode"), [this]()
	{
		switch (Mode)
		{
		case ELibraryCloneMode::Structure: return TEXT("structure");
		case ELibraryCloneMode::Portable:  return TEXT("portable");
		case ELibraryCloneMode::Full:      return TEXT("full");
		default:                           return TEXT("unknown");
		}
	}());

	Root->SetStringField(TEXT("parent_class"), ParentClass);
	if (!ParentClassNote.IsEmpty())
	{
		Root->SetStringField(TEXT("parent_class_note"), ParentClassNote);
	}

	// Structure counts
	TSharedPtr<FJsonObject> StructureObj = MakeShared<FJsonObject>();
	StructureObj->SetNumberField(TEXT("variables_created"), VariablesCreated);
	StructureObj->SetNumberField(TEXT("variables_demoted"), VariablesDemoted);
	StructureObj->SetNumberField(TEXT("variables_skipped"), VariablesSkipped);
	StructureObj->SetNumberField(TEXT("components_created"), ComponentsCreated);
	StructureObj->SetNumberField(TEXT("components_skipped"), ComponentsSkipped);
	StructureObj->SetNumberField(TEXT("interfaces_added"), InterfacesAdded);
	StructureObj->SetNumberField(TEXT("interfaces_skipped"), InterfacesSkipped);
	StructureObj->SetNumberField(TEXT("dispatchers_created"), DispatchersCreated);
	StructureObj->SetNumberField(TEXT("dispatchers_skipped"), DispatchersSkipped);
	StructureObj->SetNumberField(TEXT("functions_created"), FunctionsCreated);
	Root->SetObjectField(TEXT("structure"), StructureObj);

	// Per-graph results
	if (GraphResults.Num() > 0)
	{
		TSharedPtr<FJsonObject> GraphsObj = MakeShared<FJsonObject>();
		int32 TotalGraphs = GraphResults.Num();
		int32 ClonedGraphs = 0;
		int32 SkippedGraphs = 0;

		TArray<TSharedPtr<FJsonValue>> DetailsArray;
		for (const FCloneGraphResult& GR : GraphResults)
		{
			if (GR.NodesCreated > 0)
			{
				ClonedGraphs++;
			}
			else
			{
				SkippedGraphs++;
			}

			TSharedPtr<FJsonObject> DetailObj = MakeShared<FJsonObject>();
			DetailObj->SetStringField(TEXT("name"), GR.GraphName);
			DetailObj->SetNumberField(TEXT("nodes_created"), GR.NodesCreated);
			DetailObj->SetNumberField(TEXT("nodes_skipped"), GR.NodesSkipped);
			DetailObj->SetNumberField(TEXT("connections_succeeded"), GR.ConnectionsSucceeded);
			DetailObj->SetNumberField(TEXT("connections_failed"), GR.ConnectionsFailed);
			DetailObj->SetNumberField(TEXT("defaults_set"), GR.DefaultsSet);
			if (GR.ExecGapsBridged > 0)
			{
				DetailObj->SetNumberField(TEXT("exec_gaps_bridged"), GR.ExecGapsBridged);
			}
			if (GR.ExecGapsUnbridgeable > 0)
			{
				DetailObj->SetNumberField(TEXT("exec_gaps_unbridgeable"), GR.ExecGapsUnbridgeable);
			}
			DetailsArray.Add(MakeShared<FJsonValueObject>(DetailObj));
		}

		GraphsObj->SetNumberField(TEXT("total"), TotalGraphs);
		GraphsObj->SetNumberField(TEXT("cloned"), ClonedGraphs);
		GraphsObj->SetNumberField(TEXT("skipped"), SkippedGraphs);
		GraphsObj->SetArrayField(TEXT("details"), DetailsArray);
		Root->SetObjectField(TEXT("graphs"), GraphsObj);
	}

	// Compile result
	if (bCompiled)
	{
		TSharedPtr<FJsonObject> CompileObj = MakeShared<FJsonObject>();
		CompileObj->SetBoolField(TEXT("success"), bCompileSuccess);
		if (!bCompileSuccess && CompileErrors.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ErrorArray;
			for (const FString& Err : CompileErrors)
			{
				ErrorArray.Add(MakeShared<FJsonValueString>(Err));
			}
			CompileObj->SetArrayField(TEXT("errors"), ErrorArray);
		}
		Root->SetObjectField(TEXT("compile_result"), CompileObj);
	}

	// Warnings (flat array)
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& W : Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(W));
		}
		Root->SetArrayField(TEXT("warnings"), WarningsArray);

		// Aggregated warnings by category (only when 5+ warnings to reduce noise)
		if (Warnings.Num() >= 5)
		{
			// Category -> {count, sample, unresolvable class names}
			struct FWarningCategory
			{
				int32 Count = 0;
				FString Sample;
				TSet<FString> ClassNames;
			};
			TMap<FString, FWarningCategory> Categories;

			for (const FString& W : Warnings)
			{
				FString Cat;
				if (W.Contains(TEXT("type demoted")))
				{
					Cat = TEXT("variable_type_demoted");
				}
				else if (W.Contains(TEXT("skipped")) && W.Contains(TEXT("component")))
				{
					Cat = TEXT("component_skipped");
				}
				else if (W.Contains(TEXT("skipped")) && (W.Contains(TEXT("not found")) || W.Contains(TEXT("not supported")) || W.Contains(TEXT("source-project"))))
				{
					Cat = TEXT("node_skipped");
				}
				else if (W.Contains(TEXT("interface")) && W.Contains(TEXT("skipped")))
				{
					Cat = TEXT("interface_skipped");
				}
				else if (W.Contains(TEXT("dispatcher")) && W.Contains(TEXT("skipped")))
				{
					Cat = TEXT("dispatcher_skipped");
				}
				else if (W.Contains(TEXT("wire")) && W.Contains(TEXT("failed")))
				{
					Cat = TEXT("wire_failed");
				}
				else if (W.Contains(TEXT("default cleared")) || W.Contains(TEXT("asset reference")))
				{
					Cat = TEXT("default_cleared");
				}
				else
				{
					Cat = TEXT("other");
				}

				FWarningCategory& Entry = Categories.FindOrAdd(Cat);
				Entry.Count++;
				if (Entry.Sample.IsEmpty())
				{
					Entry.Sample = W;
				}

				// Extract class names ending in _C (source-project types)
				if (Cat == TEXT("node_skipped"))
				{
					FString Remainder = W;
					int32 Idx = 0;
					while (Remainder.FindChar(TEXT(' '), Idx))
					{
						FString Token = Remainder.Left(Idx);
						Remainder = Remainder.Mid(Idx + 1);
						if (Token.EndsWith(TEXT("_C")))
						{
							Entry.ClassNames.Add(Token);
						}
					}
					// Check last token
					if (Remainder.EndsWith(TEXT("_C")))
					{
						Entry.ClassNames.Add(Remainder);
					}
				}
			}

			TSharedPtr<FJsonObject> ByCategoryObj = MakeShared<FJsonObject>();
			for (const auto& Pair : Categories)
			{
				TSharedPtr<FJsonObject> CatObj = MakeShared<FJsonObject>();
				CatObj->SetNumberField(TEXT("count"), Pair.Value.Count);
				CatObj->SetStringField(TEXT("sample"), Pair.Value.Sample);
				if (Pair.Value.ClassNames.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> ClassArray;
					for (const FString& ClassName : Pair.Value.ClassNames)
					{
						ClassArray.Add(MakeShared<FJsonValueString>(ClassName));
					}
					CatObj->SetArrayField(TEXT("unresolvable_classes"), ClassArray);
				}
				ByCategoryObj->SetObjectField(Pair.Key, CatObj);
			}
			Root->SetObjectField(TEXT("warnings_by_category"), ByCategoryObj);
		}
	}

	// Remap suggestions
	if (RemapSuggestions.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SuggestionsArray;
		for (const FRemapSuggestion& S : RemapSuggestions)
		{
			TSharedPtr<FJsonObject> SObj = MakeShared<FJsonObject>();
			SObj->SetStringField(TEXT("source_name"), S.SourceName);

			TArray<TSharedPtr<FJsonValue>> UsedInArray;
			for (const FString& U : S.UsedIn)
			{
				UsedInArray.Add(MakeShared<FJsonValueString>(U));
			}
			SObj->SetArrayField(TEXT("used_in"), UsedInArray);
			SObj->SetStringField(TEXT("suggestion"), S.Suggestion);
			SuggestionsArray.Add(MakeShared<FJsonValueObject>(SObj));
		}
		Root->SetArrayField(TEXT("remap_suggestions"), SuggestionsArray);
	}

	// Design warnings flag
	Root->SetBoolField(TEXT("has_design_warnings"),
		VariablesDemoted > 0 || VariablesSkipped > 0 ||
		ComponentsSkipped > 0 || InterfacesSkipped > 0 ||
		DispatchersSkipped > 0 || RemapSuggestions.Num() > 0);

	return Root;
}
