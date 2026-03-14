// Copyright Bode Software. All Rights Reserved.

/**
 * OliveTemplateSystem.cpp
 *
 * Implements template loading, catalog generation, and reference-reading
 * infrastructure for the Olive template system. Templates are JSON files
 * loaded from Content/Templates/ at startup and served as reference material.
 *
 * See plans/template-reference-only-design.md for the migration rationale.
 */

#include "Template/OliveTemplateSystem.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY(LogOliveTemplates);

// =============================================================================
// Singleton
// =============================================================================

FOliveTemplateSystem& FOliveTemplateSystem::Get()
{
	static FOliveTemplateSystem Instance;
	return Instance;
}

// =============================================================================
// Directory Resolution
// =============================================================================

FString FOliveTemplateSystem::GetTemplatesDirectory() const
{
	// Same pattern as OlivePromptAssembler.cpp and OliveCrossSystemToolHandlers.cpp
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio/Content/Templates"));
}

// =============================================================================
// Lifecycle
// =============================================================================

void FOliveTemplateSystem::Initialize()
{
	if (bInitialized)
	{
		UE_LOG(LogOliveTemplates, Warning, TEXT("FOliveTemplateSystem::Initialize() called more than once. Use Reload() instead."));
		return;
	}

	const FString TemplatesDir = GetTemplatesDirectory();

	UE_LOG(LogOliveTemplates, Log, TEXT("Scanning templates directory: %s"), *TemplatesDir);

	ScanDirectory(TemplatesDir);
	LibraryIndex.Initialize(TemplatesDir);
	RebuildCatalog();

	bInitialized = true;
}

void FOliveTemplateSystem::Reload()
{
	UE_LOG(LogOliveTemplates, Log, TEXT("Reloading template system..."));

	Templates.Empty();
	CachedCatalog.Empty();
	LibraryIndex.Shutdown();
	bInitialized = false;

	Initialize();
}

void FOliveTemplateSystem::Shutdown()
{
	Templates.Empty();
	CachedCatalog.Empty();
	LibraryIndex.Shutdown();
	bInitialized = false;

	UE_LOG(LogOliveTemplates, Log, TEXT("Template system shut down."));
}

// =============================================================================
// Directory Scanning
// =============================================================================

void FOliveTemplateSystem::ScanDirectory(const FString& Directory)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*Directory))
	{
		UE_LOG(LogOliveTemplates, Log, TEXT("Templates directory does not exist (this is OK if no templates have been added): %s"), *Directory);
		return;
	}

	// Recursively scan for .json files -- picks up factory/, reference/, and any future subdirectories
	PlatformFile.IterateDirectoryRecursively(*Directory, [this](const TCHAR* FilenameOrDirectory, bool bIsDirectory) -> bool
	{
		if (bIsDirectory)
		{
			return true; // Continue iterating
		}

		const FString FilePath(FilenameOrDirectory);
		if (FilePath.EndsWith(TEXT(".json"), ESearchCase::IgnoreCase))
		{
			LoadTemplateFile(FilePath);
		}

		return true; // Continue iterating
	});

	UE_LOG(LogOliveTemplates, Log, TEXT("Scanned templates directory: found %d template(s)"), Templates.Num());
}

// =============================================================================
// File Loading
// =============================================================================

bool FOliveTemplateSystem::LoadTemplateFile(const FString& FilePath)
{
	// Read file contents
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		UE_LOG(LogOliveTemplates, Warning, TEXT("Failed to read template file: %s"), *FilePath);
		return false;
	}

	// Parse JSON
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		UE_LOG(LogOliveTemplates, Warning, TEXT("Failed to parse JSON in template file: %s"), *FilePath);
		return false;
	}

	// Extract required fields
	FString TemplateId;
	if (!JsonObj->TryGetStringField(TEXT("template_id"), TemplateId) || TemplateId.IsEmpty())
	{
		UE_LOG(LogOliveTemplates, Warning, TEXT("Template file missing required 'template_id' field: %s"), *FilePath);
		return false;
	}

	FString TemplateType;
	if (!JsonObj->TryGetStringField(TEXT("template_type"), TemplateType) || TemplateType.IsEmpty())
	{
		// No template_type means this is a library template -- handled by LibraryIndex, skip silently
		return false;
	}

	FString CatalogDescription;
	if (!JsonObj->TryGetStringField(TEXT("catalog_description"), CatalogDescription) || CatalogDescription.IsEmpty())
	{
		UE_LOG(LogOliveTemplates, Warning, TEXT("Template '%s' missing required 'catalog_description' field: %s"), *TemplateId, *FilePath);
		return false;
	}

	// Check for duplicate template IDs
	if (Templates.Contains(TemplateId))
	{
		UE_LOG(LogOliveTemplates, Warning, TEXT("Duplicate template_id '%s' found in '%s' (already loaded from '%s'). Skipping."),
			*TemplateId, *FilePath, *Templates[TemplateId].FilePath);
		return false;
	}

	// Populate template info
	FOliveTemplateInfo Info;
	Info.TemplateId = TemplateId;
	Info.TemplateType = TemplateType;
	Info.FilePath = FilePath;
	Info.CatalogDescription = CatalogDescription;
	Info.FullJson = JsonObj;

	// Optional fields
	JsonObj->TryGetStringField(TEXT("display_name"), Info.DisplayName);
	JsonObj->TryGetStringField(TEXT("catalog_examples"), Info.CatalogExamples);
	JsonObj->TryGetStringField(TEXT("tags"), Info.Tags);

	// Default display_name to template_id if not specified
	if (Info.DisplayName.IsEmpty())
	{
		Info.DisplayName = TemplateId;
	}

	Templates.Add(TemplateId, MoveTemp(Info));

	UE_LOG(LogOliveTemplates, Verbose, TEXT("Loaded template '%s' (%s) from %s"),
		*TemplateId, *TemplateType, *FilePath);

	return true;
}

// =============================================================================
// Query
// =============================================================================

const FOliveTemplateInfo* FOliveTemplateSystem::FindTemplate(const FString& TemplateId) const
{
	// Direct lookup (fast path -- exact case match)
	const FOliveTemplateInfo* Found = Templates.Find(TemplateId);
	if (Found)
	{
		return Found;
	}

	// Case-insensitive fallback: try lowercase key first
	FString LowerId = TemplateId.ToLower();
	Found = Templates.Find(LowerId);
	if (Found)
	{
		return Found;
	}

	// Full scan for mixed-case IDs (e.g., "Gun" vs "gun")
	for (const auto& Pair : Templates)
	{
		if (Pair.Key.Equals(TemplateId, ESearchCase::IgnoreCase))
		{
			return &Pair.Value;
		}
	}

	return nullptr;
}

TArray<const FOliveTemplateInfo*> FOliveTemplateSystem::GetTemplatesByType(const FString& Type) const
{
	TArray<const FOliveTemplateInfo*> Result;
	for (const auto& Pair : Templates)
	{
		if (Pair.Value.TemplateType == Type)
		{
			Result.Add(&Pair.Value);
		}
	}
	return Result;
}

// =============================================================================
// Catalog Generation
// =============================================================================

void FOliveTemplateSystem::RebuildCatalog()
{
	CachedCatalog = TEXT("[AVAILABLE BLUEPRINT TEMPLATES]\n");
	CachedCatalog += TEXT("Templates are reference material. Study architecture, then build your own with plan_json / granular tools.\n");
	CachedCatalog += TEXT("Search with blueprint.list_templates(query=\"...\"). Read functions with blueprint.get_template(id, pattern=\"FuncName\").\n\n");

	// Group by type
	TArray<const FOliveTemplateInfo*> Factories;
	TArray<const FOliveTemplateInfo*> References;

	for (const auto& Pair : Templates)
	{
		if (Pair.Value.TemplateType == TEXT("factory"))
		{
			Factories.Add(&Pair.Value);
		}
		else
		{
			References.Add(&Pair.Value);
		}
	}

	if (Factories.Num() > 0)
	{
		CachedCatalog += TEXT("Factory templates (architecture + plan_json patterns -- read with get_template):\n");
		for (const FOliveTemplateInfo* T : Factories)
		{
			CachedCatalog += FString::Printf(TEXT("- %s: %s\n"),
				*T->TemplateId, *T->CatalogDescription);
			if (!T->CatalogExamples.IsEmpty())
			{
				CachedCatalog += FString::Printf(TEXT("  Examples: %s.\n"),
					*T->CatalogExamples);
			}

			// List extractable functions
			const TSharedPtr<FJsonObject>* BPObj = nullptr;
			if (T->FullJson->TryGetObjectField(TEXT("blueprint"), BPObj) && BPObj)
			{
				const TArray<TSharedPtr<FJsonValue>>* FuncsArr = nullptr;
				if ((*BPObj)->TryGetArrayField(TEXT("functions"), FuncsArr) && FuncsArr && FuncsArr->Num() > 0)
				{
					FString FuncList;
					for (const TSharedPtr<FJsonValue>& FVal : *FuncsArr)
					{
						const TSharedPtr<FJsonObject>* FObj = nullptr;
						if (!FVal->TryGetObject(FObj) || !FObj) continue;

						FString FName;
						(*FObj)->TryGetStringField(TEXT("name"), FName);

						// Build signature: Name(inputs) -> outputs
						FString Sig = FName + TEXT("(");
						const TArray<TSharedPtr<FJsonValue>>* InsArr = nullptr;
						if ((*FObj)->TryGetArrayField(TEXT("inputs"), InsArr) && InsArr)
						{
							for (int32 i = 0; i < InsArr->Num(); i++)
							{
								const TSharedPtr<FJsonObject>* InObj = nullptr;
								if ((*InsArr)[i]->TryGetObject(InObj) && InObj)
								{
									FString PName;
									(*InObj)->TryGetStringField(TEXT("name"), PName);
									if (i > 0) Sig += TEXT(", ");
									Sig += PName;
								}
							}
						}
						Sig += TEXT(")");

						const TArray<TSharedPtr<FJsonValue>>* OutsArr = nullptr;
						if ((*FObj)->TryGetArrayField(TEXT("outputs"), OutsArr) && OutsArr && OutsArr->Num() > 0)
						{
							Sig += TEXT(" -> ");
							for (int32 i = 0; i < OutsArr->Num(); i++)
							{
								const TSharedPtr<FJsonObject>* OutObj = nullptr;
								if ((*OutsArr)[i]->TryGetObject(OutObj) && OutObj)
								{
									FString PName;
									(*OutObj)->TryGetStringField(TEXT("name"), PName);
									if (i > 0) Sig += TEXT(", ");
									Sig += PName;
								}
							}
						}

						if (!FuncList.IsEmpty()) FuncList += TEXT(", ");
						FuncList += Sig;
					}

					if (!FuncList.IsEmpty())
					{
						CachedCatalog += FString::Printf(TEXT("  Functions: %s\n"), *FuncList);
					}
				}
			}
		}
	}

	if (References.Num() > 0)
	{
		CachedCatalog += TEXT("\nReference templates (patterns -- view with blueprint.get_template):\n");
		for (const FOliveTemplateInfo* T : References)
		{
			CachedCatalog += FString::Printf(TEXT("- %s: %s\n"),
				*T->TemplateId, *T->CatalogDescription);
		}
	}

	// Library catalog (community/extracted templates indexed by FOliveLibraryIndex)
	FString LibCatalog = LibraryIndex.BuildCatalog();
	if (!LibCatalog.IsEmpty())
	{
		CachedCatalog += TEXT("\n");
		CachedCatalog += LibCatalog;
	}

	CachedCatalog += TEXT("[/AVAILABLE BLUEPRINT TEMPLATES]\n");

	UE_LOG(LogOliveTemplates, Log,
		TEXT("Template catalog rebuilt: %d factory, %d reference, %d library, %d chars"),
		Factories.Num(), References.Num(), LibraryIndex.Num(), CachedCatalog.Len());
}


// =============================================================================
// Helpers
// =============================================================================

namespace
{
	/** Serialize a JSON object to a compact string */
	FString JsonToString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Output;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Output;
	}

	/** Serialize a JSON object to a pretty string */
	FString JsonToPrettyString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Output;
	}
}

// =============================================================================
// GetTemplateContent
// =============================================================================

TArray<TSharedPtr<FJsonObject>> FOliveTemplateSystem::SearchTemplates(
	const FString& Query, int32 MaxResults) const
{
	// Reserve slots for factory/reference results so they aren't truncated by library results.
	// Request fewer library results to leave room for up to 5 factory/reference matches.
	const int32 LibraryMaxResults = FMath::Max(MaxResults - 5, 1);
	TArray<TSharedPtr<FJsonObject>> Results = LibraryIndex.Search(Query, LibraryMaxResults);

	// Also search factory/reference templates (they are excluded from library index)
	if (!Query.IsEmpty())
	{
		TArray<FString> QueryTokens = FOliveLibraryIndex::Tokenize(Query);
		if (QueryTokens.Num() > 0)
		{
			// Collect factory/reference matches with scores for sorting
			struct FScoredResult
			{
				TSharedPtr<FJsonObject> Json;
				int32 Score;
			};
			TArray<FScoredResult> FactoryRefResults;

			for (const auto& Pair : Templates)
			{
				const FOliveTemplateInfo& Info = Pair.Value;

				// Score by matching query tokens against template metadata
				int32 Score = 0;
				for (const FString& Token : QueryTokens)
				{
					if (Info.TemplateId.Contains(Token, ESearchCase::IgnoreCase)) Score += 2;
					if (Info.DisplayName.Contains(Token, ESearchCase::IgnoreCase)) Score += 2;
					if (Info.CatalogDescription.Contains(Token, ESearchCase::IgnoreCase)) Score += 1;
					if (Info.Tags.Contains(Token, ESearchCase::IgnoreCase)) Score += 1;
					if (Info.CatalogExamples.Contains(Token, ESearchCase::IgnoreCase)) Score += 1;
				}

				if (Score > 0)
				{
					TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
					ResultObj->SetStringField(TEXT("template_id"), Info.TemplateId);
					ResultObj->SetStringField(TEXT("display_name"), Info.DisplayName);
					ResultObj->SetStringField(TEXT("type"), Info.TemplateType);
					ResultObj->SetStringField(TEXT("catalog_description"), Info.CatalogDescription);
					if (!Info.CatalogExamples.IsEmpty())
					{
						ResultObj->SetStringField(TEXT("examples"), Info.CatalogExamples);
					}
					FactoryRefResults.Add({ ResultObj, Score });
				}
			}

			// Sort factory/reference results by score descending so best matches appear first
			FactoryRefResults.Sort([](const FScoredResult& A, const FScoredResult& B)
			{
				return A.Score > B.Score;
			});

			for (const FScoredResult& Scored : FactoryRefResults)
			{
				Results.Add(Scored.Json);
			}
		}
	}

	// Trim merged results to MaxResults
	if (Results.Num() > MaxResults)
	{
		Results.SetNum(MaxResults);
	}

	return Results;
}

FString FOliveTemplateSystem::GetTemplateContent(
	const FString& TemplateId,
	const FString& PatternName) const
{
	// Check library index, but prefer factory/reference formatting if template exists in both.
	// Library index scans the same directory and may index factory/reference templates,
	// but factory templates need the richer format with parameters/presets.
	const FOliveLibraryTemplateInfo* LibInfo = LibraryIndex.FindTemplate(TemplateId);
	if (LibInfo && !Templates.Contains(TemplateId))
	{
		if (PatternName.IsEmpty())
		{
			return LibraryIndex.GetTemplateOverview(TemplateId);
		}
		else
		{
			return LibraryIndex.GetFunctionContent(TemplateId, PatternName);
		}
	}

	// Fall through to original factory/reference template logic
	const FOliveTemplateInfo* Info = FindTemplate(TemplateId);
	if (!Info || !Info->FullJson.IsValid())
	{
		return FString();
	}

	FString Result;

	if (Info->TemplateType == TEXT("factory"))
	{
		// If a specific function was requested, return its full plan JSON
		if (!PatternName.IsEmpty())
		{
			const TSharedPtr<FJsonObject>* BlueprintObj = nullptr;
			if (!Info->FullJson->TryGetObjectField(TEXT("blueprint"), BlueprintObj) || !BlueprintObj)
			{
				return FString();
			}

			const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
			if (!(*BlueprintObj)->TryGetArrayField(TEXT("functions"), FunctionsArray) || !FunctionsArray)
			{
				return FString();
			}

			// Find the matching function
			const TSharedPtr<FJsonObject>* MatchedFunc = nullptr;
			for (const TSharedPtr<FJsonValue>& FuncVal : *FunctionsArray)
			{
				const TSharedPtr<FJsonObject>* FuncObj = nullptr;
				if (!FuncVal->TryGetObject(FuncObj) || !FuncObj) continue;

				FString FuncName;
				(*FuncObj)->TryGetStringField(TEXT("name"), FuncName);
				if (FuncName.Equals(PatternName, ESearchCase::IgnoreCase))
				{
					MatchedFunc = FuncObj;
					break;
				}
			}

			if (!MatchedFunc)
			{
				// Function not found -- prefix with sentinel so callers can detect error vs content
				Result += FOliveLibraryIndex::GetFuncNotFoundSentinel();
				Result += FString::Printf(TEXT("Function '%s' not found in template '%s'.\n"),
					*PatternName, *Info->TemplateId);
				Result += TEXT("Available functions: ");
				bool bFirst = true;
				for (const TSharedPtr<FJsonValue>& FuncVal : *FunctionsArray)
				{
					const TSharedPtr<FJsonObject>* FuncObj = nullptr;
					if (!FuncVal->TryGetObject(FuncObj) || !FuncObj) continue;
					FString FuncName;
					(*FuncObj)->TryGetStringField(TEXT("name"), FuncName);
					if (!bFirst) Result += TEXT(", ");
					Result += FuncName;
					bFirst = false;
				}
				Result += TEXT("\n");
				return Result;
			}

			// Build the function extraction result
			FString FuncName;
			(*MatchedFunc)->TryGetStringField(TEXT("name"), FuncName);

			Result += FString::Printf(TEXT("=== Function: %s (from template '%s') ===\n"),
				*FuncName, *Info->TemplateId);

			// Signature
			FString Desc;
			if ((*MatchedFunc)->TryGetStringField(TEXT("description"), Desc))
			{
				Result += FString::Printf(TEXT("Description: %s\n"), *Desc);
			}

			// Inputs
			const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
			if ((*MatchedFunc)->TryGetArrayField(TEXT("inputs"), InputsArray) && InputsArray && InputsArray->Num() > 0)
			{
				Result += TEXT("Inputs: ");
				for (int32 i = 0; i < InputsArray->Num(); i++)
				{
					const TSharedPtr<FJsonObject>* InObj = nullptr;
					if ((*InputsArray)[i]->TryGetObject(InObj) && InObj)
					{
						FString PName, PType;
						(*InObj)->TryGetStringField(TEXT("name"), PName);
						(*InObj)->TryGetStringField(TEXT("type"), PType);
						if (i > 0) Result += TEXT(", ");
						Result += FString::Printf(TEXT("%s:%s"), *PName, *PType);
					}
				}
				Result += TEXT("\n");
			}
			else
			{
				Result += TEXT("Inputs: none\n");
			}

			// Outputs
			const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
			if ((*MatchedFunc)->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray && OutputsArray->Num() > 0)
			{
				Result += TEXT("Outputs: ");
				for (int32 i = 0; i < OutputsArray->Num(); i++)
				{
					const TSharedPtr<FJsonObject>* OutObj = nullptr;
					if ((*OutputsArray)[i]->TryGetObject(OutObj) && OutObj)
					{
						FString PName, PType;
						(*OutObj)->TryGetStringField(TEXT("name"), PName);
						(*OutObj)->TryGetStringField(TEXT("type"), PType);
						if (i > 0) Result += TEXT(", ");
						Result += FString::Printf(TEXT("%s:%s"), *PName, *PType);
					}
				}
				Result += TEXT("\n");
			}
			else
			{
				Result += TEXT("Outputs: none\n");
			}

			// Variable dependencies -- list variables this function references
			const TSharedPtr<FJsonObject>* PlanObj = nullptr;
			if ((*MatchedFunc)->TryGetObjectField(TEXT("plan"), PlanObj) && PlanObj)
			{
				// Collect variable names from get_var/set_var steps
				TSet<FString> VarDeps;
				const TArray<TSharedPtr<FJsonValue>>* StepsArray = nullptr;
				if ((*PlanObj)->TryGetArrayField(TEXT("steps"), StepsArray) && StepsArray)
				{
					for (const TSharedPtr<FJsonValue>& StepVal : *StepsArray)
					{
						const TSharedPtr<FJsonObject>* StepObj = nullptr;
						if (!StepVal->TryGetObject(StepObj) || !StepObj) continue;

						FString Op, Target;
						(*StepObj)->TryGetStringField(TEXT("op"), Op);
						(*StepObj)->TryGetStringField(TEXT("target"), Target);
						if ((Op == TEXT("get_var") || Op == TEXT("set_var")) && !Target.IsEmpty())
						{
							VarDeps.Add(Target);
						}
					}
				}

				if (VarDeps.Num() > 0)
				{
					// Look up variable types from the blueprint.variables array
					const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
					TMap<FString, FString> VarTypes;
					if ((*BlueprintObj)->TryGetArrayField(TEXT("variables"), VarsArray) && VarsArray)
					{
						for (const TSharedPtr<FJsonValue>& VarVal : *VarsArray)
						{
							const TSharedPtr<FJsonObject>* VarObj = nullptr;
							if (!VarVal->TryGetObject(VarObj) || !VarObj) continue;
							FString VName, VType;
							(*VarObj)->TryGetStringField(TEXT("name"), VName);
							(*VarObj)->TryGetStringField(TEXT("type"), VType);
							VarTypes.Add(VName, VType);
						}
					}

					Result += TEXT("Required variables: ");
					bool bFirst = true;
					for (const FString& Var : VarDeps)
					{
						if (!bFirst) Result += TEXT(", ");
						const FString* VType = VarTypes.Find(Var);
						if (VType)
						{
							Result += FString::Printf(TEXT("%s (%s)"), *Var, **VType);
						}
						else
						{
							Result += Var;
						}
						bFirst = false;
					}
					Result += TEXT("\n");
				}

				// Dispatcher dependencies -- find call_delegate/call_dispatcher steps
				TSet<FString> DispDeps;
				if (StepsArray)
				{
					for (const TSharedPtr<FJsonValue>& StepVal : *StepsArray)
					{
						const TSharedPtr<FJsonObject>* StepObj = nullptr;
						if (!StepVal->TryGetObject(StepObj) || !StepObj) continue;

						FString Op, Target;
						(*StepObj)->TryGetStringField(TEXT("op"), Op);
						(*StepObj)->TryGetStringField(TEXT("target"), Target);
						if ((Op == TEXT("call_delegate") || Op == TEXT("call_dispatcher")) && !Target.IsEmpty())
						{
							DispDeps.Add(Target);
						}
					}
				}

				if (DispDeps.Num() > 0)
				{
					Result += TEXT("Required dispatchers: ");
					bool bFirst = true;
					for (const FString& Disp : DispDeps)
					{
						if (!bFirst) Result += TEXT(", ");
						Result += Disp;
						bFirst = false;
					}
					Result += TEXT("\n");
				}

				// The raw plan JSON
				Result += TEXT("\nplan_json:\n");
				Result += JsonToPrettyString(*PlanObj);
				Result += TEXT("\n");
			}
			else
			{
				Result += TEXT("\nThis function has no plan (empty stub).\n");
			}

			return Result;
		}

		// === Factory template: show parameters, presets, function outlines ===
		Result += FString::Printf(TEXT("=== Factory Template: %s ===\n"), *Info->DisplayName);
		Result += FString::Printf(TEXT("Description: %s\n\n"), *Info->CatalogDescription);

		// Parameters
		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		if (Info->FullJson->TryGetObjectField(TEXT("parameters"), ParamsObj) && ParamsObj)
		{
			Result += TEXT("Parameters:\n");
			for (const auto& Pair : (*ParamsObj)->Values)
			{
				const TSharedPtr<FJsonObject>* ParamDef = nullptr;
				if (Pair.Value->TryGetObject(ParamDef) && ParamDef)
				{
					FString Type, Default, Desc;
					(*ParamDef)->TryGetStringField(TEXT("type"), Type);
					(*ParamDef)->TryGetStringField(TEXT("default"), Default);
					(*ParamDef)->TryGetStringField(TEXT("description"), Desc);
					Result += FString::Printf(TEXT("  %s (%s, default=%s): %s\n"),
						*Pair.Key, *Type, *Default, *Desc);
				}
			}
			Result += TEXT("\n");
		}

		// Presets (stored as JSON object: key = preset name, value = param overrides)
		const TSharedPtr<FJsonObject>* PresetsObj = nullptr;
		if (Info->FullJson->TryGetObjectField(TEXT("presets"), PresetsObj) && PresetsObj)
		{
			Result += TEXT("Presets:\n");
			for (const auto& PresetPair : (*PresetsObj)->Values)
			{
				Result += FString::Printf(TEXT("  %s:"), *PresetPair.Key);

				const TSharedPtr<FJsonObject>* PresetValues = nullptr;
				if (PresetPair.Value->TryGetObject(PresetValues) && PresetValues)
				{
					for (const auto& PP : (*PresetValues)->Values)
					{
						FString Val;
						PP.Value->TryGetString(Val);
						Result += FString::Printf(TEXT(" %s=%s"), *PP.Key, *Val);
					}
				}
				Result += TEXT("\n");
			}
			Result += TEXT("\n");
		}

		// Functions (condensed outlines)
		const TSharedPtr<FJsonObject>* BlueprintObj = nullptr;
		if (Info->FullJson->TryGetObjectField(TEXT("blueprint"), BlueprintObj) && BlueprintObj)
		{
			const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
			if ((*BlueprintObj)->TryGetArrayField(TEXT("functions"), FunctionsArray) && FunctionsArray)
			{
				Result += TEXT("Functions:\n");
				for (const TSharedPtr<FJsonValue>& FuncVal : *FunctionsArray)
				{
					const TSharedPtr<FJsonObject>* FuncObj = nullptr;
					if (!FuncVal->TryGetObject(FuncObj) || !FuncObj) continue;

					FString FuncName;
					(*FuncObj)->TryGetStringField(TEXT("name"), FuncName);
					Result += FString::Printf(TEXT("  %s:\n"), *FuncName);

					const TSharedPtr<FJsonObject>* PlanObj = nullptr;
					if ((*FuncObj)->TryGetObjectField(TEXT("plan"), PlanObj) && PlanObj)
					{
						const TArray<TSharedPtr<FJsonValue>>* StepsArray = nullptr;
						if ((*PlanObj)->TryGetArrayField(TEXT("steps"), StepsArray) && StepsArray)
						{
							for (const TSharedPtr<FJsonValue>& StepVal : *StepsArray)
							{
								const TSharedPtr<FJsonObject>* StepObj = nullptr;
								if (StepVal->TryGetObject(StepObj) && StepObj)
								{
									FString StepId, Op;
									(*StepObj)->TryGetStringField(TEXT("step_id"), StepId);
									(*StepObj)->TryGetStringField(TEXT("op"), Op);
									Result += FString::Printf(TEXT("    %s: %s\n"), *StepId, *Op);
								}
							}
						}
					}
				}
			}
		}
	}
	else if (Info->TemplateType == TEXT("reference"))
	{
		// === Reference template: show patterns ===
		Result += FString::Printf(TEXT("=== Reference Template: %s ===\n"), *Info->DisplayName);
		Result += FString::Printf(TEXT("Description: %s\n\n"), *Info->CatalogDescription);

		const TArray<TSharedPtr<FJsonValue>>* PatternsArray = nullptr;
		if (Info->FullJson->TryGetArrayField(TEXT("patterns"), PatternsArray) && PatternsArray)
		{
			for (const TSharedPtr<FJsonValue>& PatternVal : *PatternsArray)
			{
				const TSharedPtr<FJsonObject>* PatObj = nullptr;
				if (!PatternVal->TryGetObject(PatObj) || !PatObj) continue;

				FString PName, PDesc;
				(*PatObj)->TryGetStringField(TEXT("name"), PName);
				(*PatObj)->TryGetStringField(TEXT("description"), PDesc);

				// If a specific pattern was requested, skip non-matches
				if (!PatternName.IsEmpty() && PName != PatternName)
				{
					continue;
				}

				Result += FString::Printf(TEXT("--- Pattern: %s ---\n"), *PName);
				Result += FString::Printf(TEXT("Description: %s\n"), *PDesc);

				FString Notes;
				if ((*PatObj)->TryGetStringField(TEXT("notes"), Notes))
				{
					Result += FString::Printf(TEXT("Notes: %s\n"), *Notes);
				}

				const TArray<TSharedPtr<FJsonValue>>* StepsArray = nullptr;
				if ((*PatObj)->TryGetArrayField(TEXT("steps"), StepsArray) && StepsArray)
				{
					Result += TEXT("Steps:\n");
					for (const TSharedPtr<FJsonValue>& StepVal : *StepsArray)
					{
						const TSharedPtr<FJsonObject>* StepObj = nullptr;
						if (StepVal->TryGetObject(StepObj) && StepObj)
						{
							Result += TEXT("  ") + JsonToString(*StepObj) + TEXT("\n");
						}
					}
				}
				Result += TEXT("\n");
			}
		}
	}

	return Result;
}

// REMOVED: ApplyTemplate and supporting helpers (SubstituteParameters, EvaluateConditionals,
// MergeParameters, ParseSimpleTypeCategory, ResolveUnknownIRType, ParseTemplateVariable,
// ParseTemplateFuncParam, ParseBlueprintType) deleted as part of reference-only migration.
// See plans/template-reference-only-design.md for rationale.

// =============================================================================
// FOliveLibraryIndex
// =============================================================================

namespace
{
	/** Stop words excluded from search tokenization. */
	static const TSet<FString> StopWords = {
		TEXT("the"), TEXT("and"), TEXT("for"), TEXT("with"),
		TEXT("from"), TEXT("this"), TEXT("that"), TEXT("into")
	};

	/** Extract an array of strings from a JSON array of objects, pulling the "name" field. */
	TArray<FString> ExtractStringArray(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, int32 MaxCount = 0)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(FieldName, Arr) || !Arr)
		{
			return Result;
		}

		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* ItemObj = nullptr;
			if (Val->TryGetObject(ItemObj) && ItemObj)
			{
				FString Name;
				if ((*ItemObj)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
				{
					Result.Add(Name);
					if (MaxCount > 0 && Result.Num() >= MaxCount)
					{
						break;
					}
				}
			}
		}
		return Result;
	}

	/** Extract component names from the blueprint.components.tree array. */
	TArray<FString> ExtractComponentNames(const TSharedPtr<FJsonObject>& BlueprintObj)
	{
		TArray<FString> Result;
		const TSharedPtr<FJsonObject>* CompObj = nullptr;
		if (!BlueprintObj->TryGetObjectField(TEXT("components"), CompObj) || !CompObj)
		{
			return Result;
		}

		const TArray<TSharedPtr<FJsonValue>>* TreeArr = nullptr;
		if (!(*CompObj)->TryGetArrayField(TEXT("tree"), TreeArr) || !TreeArr)
		{
			return Result;
		}

		for (const TSharedPtr<FJsonValue>& Val : *TreeArr)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			if (Val->TryGetObject(NodeObj) && NodeObj)
			{
				FString Name;
				if ((*NodeObj)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
				{
					Result.Add(Name);
				}
			}
		}
		return Result;
	}

	/** Parse a graphs array (functions or event_graphs) into function summaries. */
	TArray<FOliveLibraryFunctionSummary> ParseGraphsArray(
		const TSharedPtr<FJsonObject>& GraphsObj,
		const FString& ArrayFieldName,
		const FString& GraphType)
	{
		TArray<FOliveLibraryFunctionSummary> Result;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!GraphsObj->TryGetArrayField(ArrayFieldName, Arr) || !Arr)
		{
			return Result;
		}

		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* FuncObj = nullptr;
			if (!Val->TryGetObject(FuncObj) || !FuncObj)
			{
				continue;
			}

			FOliveLibraryFunctionSummary Summary;
			(*FuncObj)->TryGetStringField(TEXT("name"), Summary.Name);
			if (Summary.Name.IsEmpty())
			{
				continue;
			}

			Summary.GraphType = GraphType;
			(*FuncObj)->TryGetNumberField(TEXT("node_count"), Summary.NodeCount);
			(*FuncObj)->TryGetStringField(TEXT("description"), Summary.Description);
			(*FuncObj)->TryGetStringField(TEXT("tags"), Summary.Tags);

			// For event graphs: also gather entry point tags
			if (GraphType == TEXT("EventGraph"))
			{
				const TArray<TSharedPtr<FJsonValue>>* EntryArr = nullptr;
				if ((*FuncObj)->TryGetArrayField(TEXT("entry_points"), EntryArr) && EntryArr)
				{
					for (const TSharedPtr<FJsonValue>& EPVal : *EntryArr)
					{
						const TSharedPtr<FJsonObject>* EPObj = nullptr;
						if (EPVal->TryGetObject(EPObj) && EPObj)
						{
							FString EPTags;
							if ((*EPObj)->TryGetStringField(TEXT("tags"), EPTags) && !EPTags.IsEmpty())
							{
								if (!Summary.Tags.IsEmpty())
								{
									Summary.Tags += TEXT(" ");
								}
								Summary.Tags += EPTags;
							}
						}
					}
				}
			}

			Result.Add(MoveTemp(Summary));
		}
		return Result;
	}
}

// Sentinel prefix moved to FOliveLibraryIndex::GetFuncNotFoundSentinel() in header.
// Use the public accessor instead of a local copy.

// -----------------------------------------------------------------------------
// Initialize
// -----------------------------------------------------------------------------

void FOliveLibraryIndex::Initialize(const FString& TemplatesRootDir)
{
	if (bInitialized)
	{
		return;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*TemplatesRootDir))
	{
		UE_LOG(LogOliveTemplates, Log,
			TEXT("Library index: templates directory does not exist: %s"), *TemplatesRootDir);
		bInitialized = true;
		return;
	}

	int32 FilesScanned = 0;
	PlatformFile.IterateDirectoryRecursively(*TemplatesRootDir,
		[this, &FilesScanned](const TCHAR* FilenameOrDirectory, bool bIsDirectory) -> bool
		{
			if (bIsDirectory)
			{
				return true;
			}

			const FString FilePath(FilenameOrDirectory);
			if (FilePath.EndsWith(TEXT(".json"), ESearchCase::IgnoreCase))
			{
				if (IndexTemplateFile(FilePath))
				{
					FilesScanned++;
				}
			}

			return true;
		});

	// Count total functions across all templates
	int32 TotalFunctions = 0;
	for (const auto& Pair : Templates)
	{
		TotalFunctions += Pair.Value.Functions.Num();
	}

	UE_LOG(LogOliveTemplates, Log,
		TEXT("Library index: %d templates, %d functions indexed (%d library files accepted)"),
		Templates.Num(), TotalFunctions, FilesScanned);

	bInitialized = true;
}

// -----------------------------------------------------------------------------
// Shutdown
// -----------------------------------------------------------------------------

void FOliveLibraryIndex::Shutdown()
{
	Templates.Empty();
	SearchIndex.Empty();
	JsonCache.Empty();
	CacheOrder.Empty();
	bInitialized = false;
}

// -----------------------------------------------------------------------------
// IndexTemplateFile
// -----------------------------------------------------------------------------

bool FOliveLibraryIndex::IndexTemplateFile(const FString& FilePath)
{
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *FilePath))
	{
		UE_LOG(LogOliveTemplates, Warning, TEXT("Library index: failed to read file: %s"), *FilePath);
		return false;
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		UE_LOG(LogOliveTemplates, Warning, TEXT("Library index: failed to parse JSON: %s"), *FilePath);
		return false;
	}

	// Skip factory/reference templates -- they are handled by FOliveTemplateSystem::LoadTemplateFile()
	// and would cause double-indexing (inflated counts, duplicate search results).
	FString TemplateType;
	if (JsonObj->TryGetStringField(TEXT("template_type"), TemplateType) && !TemplateType.IsEmpty())
	{
		if (TemplateType.Equals(TEXT("factory"), ESearchCase::IgnoreCase) ||
			TemplateType.Equals(TEXT("reference"), ESearchCase::IgnoreCase))
		{
			return false;
		}
	}

	// Extract template_id (required). Fall back to "name" for legacy format.
	FString TemplateId;
	if (!JsonObj->TryGetStringField(TEXT("template_id"), TemplateId) || TemplateId.IsEmpty())
	{
		FString NameField;
		if (JsonObj->TryGetStringField(TEXT("name"), NameField) && !NameField.IsEmpty())
		{
			// Derive template_id from name: lowercase, spaces to underscores
			TemplateId = NameField.ToLower().Replace(TEXT(" "), TEXT("_"));
		}
		else
		{
			// No usable identifier -- skip
			return false;
		}
	}

	// Check for duplicates
	if (Templates.Contains(TemplateId))
	{
		// Try folder-qualified ID to disambiguate
		FString ProjectFolder = FPaths::GetBaseFilename(FPaths::GetPath(FilePath));
		FString QualifiedId = ProjectFolder.ToLower() + TEXT("_") + TemplateId;
		if (Templates.Contains(QualifiedId))
		{
			UE_LOG(LogOliveTemplates, Verbose,
				TEXT("Library index: duplicate template_id '%s' (also tried '%s') from %s -- skipping"),
				*TemplateId, *QualifiedId, *FilePath);
			return false;
		}
		TemplateId = QualifiedId;
	}

	FOliveLibraryTemplateInfo Info;
	Info.TemplateId = TemplateId;
	Info.FilePath = FilePath;

	// Display name: prefer display_name, fall back to name
	if (!JsonObj->TryGetStringField(TEXT("display_name"), Info.DisplayName) || Info.DisplayName.IsEmpty())
	{
		JsonObj->TryGetStringField(TEXT("name"), Info.DisplayName);
		if (Info.DisplayName.IsEmpty())
		{
			Info.DisplayName = TemplateId;
		}
	}

	// Optional metadata fields (handle gracefully if missing)
	JsonObj->TryGetStringField(TEXT("catalog_description"), Info.CatalogDescription);
	JsonObj->TryGetStringField(TEXT("tags"), Info.Tags);
	JsonObj->TryGetStringField(TEXT("inherited_tags"), Info.InheritedTags);
	JsonObj->TryGetStringField(TEXT("source_project"), Info.SourceProject);
	JsonObj->TryGetStringField(TEXT("depends_on"), Info.DependsOn);

	// Check both array format (library) and object format (factory) for parameters
	const TArray<TSharedPtr<FJsonValue>>* ParamsArr = nullptr;
	if (JsonObj->TryGetArrayField(TEXT("parameters"), ParamsArr) && ParamsArr && ParamsArr->Num() > 0)
	{
		Info.bHasParameters = true;
	}
	else
	{
		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		if (JsonObj->TryGetObjectField(TEXT("parameters"), ParamsObj) && ParamsObj && (*ParamsObj)->Values.Num() > 0)
		{
			Info.bHasParameters = true;
		}
	}

	// Extract blueprint-level metadata
	const TSharedPtr<FJsonObject>* BlueprintObj = nullptr;
	if (JsonObj->TryGetObjectField(TEXT("blueprint"), BlueprintObj) && BlueprintObj)
	{
		(*BlueprintObj)->TryGetStringField(TEXT("type"), Info.BlueprintType);

		const TSharedPtr<FJsonObject>* ParentClassObj = nullptr;
		if ((*BlueprintObj)->TryGetObjectField(TEXT("parent_class"), ParentClassObj) && ParentClassObj)
		{
			(*ParentClassObj)->TryGetStringField(TEXT("name"), Info.ParentClassName);
			(*ParentClassObj)->TryGetStringField(TEXT("source"), Info.ParentClassSource);
		}

		// If parent_class is a plain string instead of an object, handle that too
		if (Info.ParentClassName.IsEmpty())
		{
			(*BlueprintObj)->TryGetStringField(TEXT("parent_class"), Info.ParentClassName);
		}

		Info.InterfaceNames = ExtractStringArray(*BlueprintObj, TEXT("interfaces"));
		Info.VariableNames = ExtractStringArray(*BlueprintObj, TEXT("variables"), 100);
		Info.ComponentNames = ExtractComponentNames(*BlueprintObj);
		Info.DispatcherNames = ExtractStringArray(*BlueprintObj, TEXT("event_dispatchers"));
	}

	// Fallback: check top-level fields for extracted format (no "blueprint" wrapper)
	if (Info.ParentClassName.IsEmpty())
	{
		const TSharedPtr<FJsonObject>* TopParentClassObj = nullptr;
		if (JsonObj->TryGetObjectField(TEXT("parent_class"), TopParentClassObj) && TopParentClassObj)
		{
			(*TopParentClassObj)->TryGetStringField(TEXT("name"), Info.ParentClassName);
			(*TopParentClassObj)->TryGetStringField(TEXT("source"), Info.ParentClassSource);
		}
		if (Info.ParentClassName.IsEmpty())
		{
			JsonObj->TryGetStringField(TEXT("parent_class"), Info.ParentClassName);
		}
	}
	if (Info.BlueprintType.IsEmpty())
	{
		JsonObj->TryGetStringField(TEXT("type"), Info.BlueprintType);
	}
	if (Info.InterfaceNames.Num() == 0)
	{
		Info.InterfaceNames = ExtractStringArray(JsonObj, TEXT("interfaces"));
	}
	if (Info.VariableNames.Num() == 0)
	{
		Info.VariableNames = ExtractStringArray(JsonObj, TEXT("variables"), 100);
	}
	if (Info.ComponentNames.Num() == 0)
	{
		Info.ComponentNames = ExtractComponentNames(JsonObj);
	}
	if (Info.DispatcherNames.Num() == 0)
	{
		Info.DispatcherNames = ExtractStringArray(JsonObj, TEXT("event_dispatchers"));
	}

	// Extract graph/function metadata from "graphs" section
	const TSharedPtr<FJsonObject>* GraphsObj = nullptr;
	if (JsonObj->TryGetObjectField(TEXT("graphs"), GraphsObj) && GraphsObj)
	{
		TArray<FOliveLibraryFunctionSummary> Functions =
			ParseGraphsArray(*GraphsObj, TEXT("functions"), TEXT("Function"));
		TArray<FOliveLibraryFunctionSummary> EventGraphs =
			ParseGraphsArray(*GraphsObj, TEXT("event_graphs"), TEXT("EventGraph"));

		Info.Functions.Append(Functions);
		Info.Functions.Append(EventGraphs);
	}

	// Also check blueprint.functions for factory-style templates
	if (Info.Functions.Num() == 0 && BlueprintObj)
	{
		const TArray<TSharedPtr<FJsonValue>>* FuncsArr = nullptr;
		if ((*BlueprintObj)->TryGetArrayField(TEXT("functions"), FuncsArr) && FuncsArr)
		{
			for (const TSharedPtr<FJsonValue>& FVal : *FuncsArr)
			{
				const TSharedPtr<FJsonObject>* FObj = nullptr;
				if (!FVal->TryGetObject(FObj) || !FObj) continue;

				FOliveLibraryFunctionSummary Summary;
				(*FObj)->TryGetStringField(TEXT("name"), Summary.Name);
				if (Summary.Name.IsEmpty()) continue;

				Summary.GraphType = TEXT("Function");
				(*FObj)->TryGetStringField(TEXT("description"), Summary.Description);

				// Count steps in plan as proxy for node count
				const TSharedPtr<FJsonObject>* PlanObj = nullptr;
				if ((*FObj)->TryGetObjectField(TEXT("plan"), PlanObj) && PlanObj)
				{
					const TArray<TSharedPtr<FJsonValue>>* StepsArr = nullptr;
					if ((*PlanObj)->TryGetArrayField(TEXT("steps"), StepsArr) && StepsArr)
					{
						Summary.NodeCount = StepsArr->Num();
					}
				}

				Info.Functions.Add(MoveTemp(Summary));
			}
		}
	}

	// Auto-generate catalog_description if missing
	if (Info.CatalogDescription.IsEmpty())
	{
		Info.CatalogDescription = FString::Printf(
			TEXT("%s blueprint (%s) with %d functions"),
			Info.BlueprintType.IsEmpty() ? TEXT("Blueprint") : *Info.BlueprintType,
			Info.ParentClassName.IsEmpty() ? TEXT("Actor") : *Info.ParentClassName,
			Info.Functions.Num());
	}

	// Store and build search tokens -- discard the full JSON
	BuildSearchTokens(Info);
	Templates.Add(TemplateId, MoveTemp(Info));

	return true;
}

// -----------------------------------------------------------------------------
// Tokenize
// -----------------------------------------------------------------------------

TArray<FString> FOliveLibraryIndex::Tokenize(const FString& Input)
{
	TArray<FString> Tokens;
	if (Input.IsEmpty())
	{
		return Tokens;
	}

	// Split on spaces, underscores, hyphens
	FString Normalized = Input;
	Normalized = Normalized.Replace(TEXT("_"), TEXT(" "));
	Normalized = Normalized.Replace(TEXT("-"), TEXT(" "));

	TArray<FString> RawParts;
	Normalized.ParseIntoArray(RawParts, TEXT(" "), true);

	for (FString& Part : RawParts)
	{
		Part = Part.ToLower().TrimStartAndEnd();
		if (Part.Len() < 2)
		{
			continue;
		}
		if (StopWords.Contains(Part))
		{
			continue;
		}
		Tokens.AddUnique(Part);
	}

	return Tokens;
}

// -----------------------------------------------------------------------------
// BuildSearchTokens
// -----------------------------------------------------------------------------

void FOliveLibraryIndex::BuildSearchTokens(const FOliveLibraryTemplateInfo& Info)
{
	auto AddTokens = [this, &Info](const FString& Source)
	{
		TArray<FString> Tokens = Tokenize(Source);
		for (const FString& Token : Tokens)
		{
			SearchIndex.FindOrAdd(Token).Add(Info.TemplateId);
		}
	};

	AddTokens(Info.TemplateId);
	AddTokens(Info.DisplayName);
	AddTokens(Info.Tags);
	AddTokens(Info.InheritedTags);
	AddTokens(Info.CatalogDescription);
	AddTokens(Info.SourceProject);

	// Index structural metadata for richer search (parent class, interfaces, components, dispatchers).
	// VariableNames are intentionally excluded — too numerous (up to 100/template) and would dilute results.
	AddTokens(Info.ParentClassName);
	for (const FString& Name : Info.InterfaceNames) { AddTokens(Name); }
	for (const FString& Name : Info.ComponentNames) { AddTokens(Name); }
	for (const FString& Name : Info.DispatcherNames) { AddTokens(Name); }

	for (const FOliveLibraryFunctionSummary& Func : Info.Functions)
	{
		AddTokens(Func.Name);
		AddTokens(Func.Tags);
		AddTokens(Func.Description);
	}
}

// -----------------------------------------------------------------------------
// Search
// -----------------------------------------------------------------------------

TArray<TSharedPtr<FJsonObject>> FOliveLibraryIndex::Search(const FString& Query, int32 MaxResults) const
{
	TArray<TSharedPtr<FJsonObject>> Results;
	if (Query.IsEmpty() || Templates.Num() == 0)
	{
		return Results;
	}

	TArray<FString> QueryTokens = Tokenize(Query);
	if (QueryTokens.Num() == 0)
	{
		return Results;
	}

	// Score templates by matching tokens
	TMap<FString, int32> Scores;

	for (const FString& Token : QueryTokens)
	{
		// Direct token lookup in inverted index
		const TSet<FString>* DirectMatches = SearchIndex.Find(Token);
		if (DirectMatches)
		{
			for (const FString& Id : *DirectMatches)
			{
				Scores.FindOrAdd(Id) += 2;  // Exact token match scores 2
			}
		}

		// Substring matching across all index keys
		for (const auto& IndexPair : SearchIndex)
		{
			if (IndexPair.Key.Contains(Token) && (&IndexPair.Value != DirectMatches))
			{
				for (const FString& Id : IndexPair.Value)
				{
					Scores.FindOrAdd(Id) += 1;  // Substring match scores 1
				}
			}
		}
	}

	// Sort by score descending
	TArray<TPair<FString, int32>> SortedScores;
	for (const auto& Pair : Scores)
	{
		SortedScores.Add(TPair<FString, int32>(Pair.Key, Pair.Value));
	}
	SortedScores.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
	{
		return A.Value > B.Value;
	});

	// Build result JSON for top results
	const int32 Count = FMath::Min(MaxResults, SortedScores.Num());
	for (int32 i = 0; i < Count; i++)
	{
		const FString& TemplateId = SortedScores[i].Key;
		const FOliveLibraryTemplateInfo* Info = FindTemplate(TemplateId);
		if (!Info)
		{
			continue;
		}

		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetStringField(TEXT("template_id"), Info->TemplateId);
		ResultObj->SetStringField(TEXT("display_name"), Info->DisplayName);
		ResultObj->SetStringField(TEXT("type"), TEXT("library"));
		ResultObj->SetStringField(TEXT("blueprint_type"), Info->BlueprintType);
		ResultObj->SetStringField(TEXT("parent_class"), Info->ParentClassName);
		ResultObj->SetBoolField(TEXT("has_parameters"), Info->bHasParameters);
		ResultObj->SetNumberField(TEXT("function_count"), Info->Functions.Num());
		ResultObj->SetStringField(TEXT("catalog_description"), Info->CatalogDescription);

		if (!Info->SourceProject.IsEmpty())
		{
			ResultObj->SetStringField(TEXT("source_project"), Info->SourceProject);
		}

		// Find functions that match the query
		TArray<TSharedPtr<FJsonValue>> MatchedFunctions;
		for (const FOliveLibraryFunctionSummary& Func : Info->Functions)
		{
			bool bFuncMatches = false;
			for (const FString& Token : QueryTokens)
			{
				if (Func.Name.Contains(Token, ESearchCase::IgnoreCase) ||
					Func.Tags.Contains(Token, ESearchCase::IgnoreCase) ||
					Func.Description.Contains(Token, ESearchCase::IgnoreCase))
				{
					bFuncMatches = true;
					break;
				}
			}

			if (bFuncMatches)
			{
				TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
				FuncObj->SetStringField(TEXT("name"), Func.Name);
				FuncObj->SetNumberField(TEXT("node_count"), Func.NodeCount);
				if (!Func.Tags.IsEmpty())
				{
					FuncObj->SetStringField(TEXT("tags"), Func.Tags);
				}
				MatchedFunctions.Add(MakeShared<FJsonValueObject>(FuncObj));
			}
		}

		if (MatchedFunctions.Num() > 0)
		{
			ResultObj->SetArrayField(TEXT("matched_functions"), MatchedFunctions);
		}

		Results.Add(ResultObj);
	}

	return Results;
}

// -----------------------------------------------------------------------------
// FindTemplate
// -----------------------------------------------------------------------------

const FOliveLibraryTemplateInfo* FOliveLibraryIndex::FindTemplate(const FString& TemplateId) const
{
	// Direct lookup first (fast path -- keys are stored lowercase)
	const FOliveLibraryTemplateInfo* Found = Templates.Find(TemplateId);
	if (Found)
	{
		return Found;
	}

	// Case-insensitive fallback for callers that pass mixed-case IDs
	FString LowerId = TemplateId.ToLower();
	Found = Templates.Find(LowerId);
	if (Found)
	{
		return Found;
	}

	// Full scan fallback (handles edge cases like folder-qualified IDs with odd casing)
	for (const auto& Pair : Templates)
	{
		if (Pair.Key.Equals(TemplateId, ESearchCase::IgnoreCase))
		{
			return &Pair.Value;
		}
	}

	return nullptr;
}

// -----------------------------------------------------------------------------
// LoadFullJson
// -----------------------------------------------------------------------------

TSharedPtr<FJsonObject> FOliveLibraryIndex::LoadFullJson(const FString& TemplateId) const
{
	check(IsInGameThread());

	// Look up template first so we always use the canonical key for cache operations.
	// FindTemplate may resolve case-insensitively, so the canonical ID may differ from
	// the caller-provided TemplateId. Using the canonical key prevents duplicate cache entries.
	const FOliveLibraryTemplateInfo* Info = FindTemplate(TemplateId);
	if (!Info)
	{
		return nullptr;
	}

	const FString& CanonicalId = Info->TemplateId;

	// Check cache using canonical key
	const TSharedPtr<FJsonObject>* Cached = JsonCache.Find(CanonicalId);
	if (Cached && Cached->IsValid())
	{
		// Move to front of CacheOrder (LRU touch)
		CacheOrder.Remove(CanonicalId);
		CacheOrder.Insert(CanonicalId, 0);
		return *Cached;
	}

	// Read and parse from disk
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *Info->FilePath))
	{
		UE_LOG(LogOliveTemplates, Warning,
			TEXT("Library index: failed to load JSON for '%s' from %s"),
			*CanonicalId, *Info->FilePath);
		return nullptr;
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		UE_LOG(LogOliveTemplates, Warning,
			TEXT("Library index: failed to parse JSON for '%s' from %s"),
			*CanonicalId, *Info->FilePath);
		return nullptr;
	}

	// Evict oldest if cache is full
	while (CacheOrder.Num() >= MAX_CACHE_SIZE)
	{
		const FString OldestId = CacheOrder.Last();
		JsonCache.Remove(OldestId);
		CacheOrder.RemoveAt(CacheOrder.Num() - 1);
	}

	// Add to cache using canonical key
	JsonCache.Add(CanonicalId, JsonObj);
	CacheOrder.Insert(CanonicalId, 0);

	return JsonObj;
}

// -----------------------------------------------------------------------------
// GetFunctionContent
// -----------------------------------------------------------------------------

FString FOliveLibraryIndex::GetFunctionContent(const FString& TemplateId, const FString& FunctionName) const
{
	const FOliveLibraryTemplateInfo* Info = FindTemplate(TemplateId);
	if (!Info)
	{
		// Return empty — handler checks Content.IsEmpty() and returns TEMPLATE_CONTENT_EMPTY
		return FString();
	}

	TSharedPtr<FJsonObject> FullJson = LoadFullJson(TemplateId);
	if (!FullJson.IsValid())
	{
		// Return empty — handler checks Content.IsEmpty() and returns TEMPLATE_CONTENT_EMPTY
		return FString();
	}

	// Lambda to search a JSON array for a matching function by name
	auto SearchGraphArray = [&FunctionName](const TSharedPtr<FJsonObject>& ParentObj, const FString& ArrayField)
		-> const TSharedPtr<FJsonObject>*
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!ParentObj->TryGetArrayField(ArrayField, Arr) || !Arr)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* FuncObj = nullptr;
			if (!Val->TryGetObject(FuncObj) || !FuncObj) continue;

			FString Name;
			(*FuncObj)->TryGetStringField(TEXT("name"), Name);
			if (Name.Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return FuncObj;
			}
		}
		return nullptr;
	};

	const TSharedPtr<FJsonObject>* MatchedFunc = nullptr;

	// Check graphs.functions[] and graphs.event_graphs[]
	const TSharedPtr<FJsonObject>* GraphsObj = nullptr;
	if (FullJson->TryGetObjectField(TEXT("graphs"), GraphsObj) && GraphsObj)
	{
		MatchedFunc = SearchGraphArray(*GraphsObj, TEXT("functions"));
		if (!MatchedFunc)
		{
			MatchedFunc = SearchGraphArray(*GraphsObj, TEXT("event_graphs"));
		}
	}

	// Also check blueprint.functions[] (factory-style templates)
	if (!MatchedFunc)
	{
		const TSharedPtr<FJsonObject>* BlueprintObj = nullptr;
		if (FullJson->TryGetObjectField(TEXT("blueprint"), BlueprintObj) && BlueprintObj)
		{
			MatchedFunc = SearchGraphArray(*BlueprintObj, TEXT("functions"));
		}
	}

	// If not found and DependsOn is set, walk inheritance chain directly
	// (avoid recursive GetFunctionContent to prevent re-resolving chains)
	FString InheritedFromLabel;
	if (!MatchedFunc && !Info->DependsOn.IsEmpty())
	{
		TArray<const FOliveLibraryTemplateInfo*> Chain = ResolveInheritanceChain(TemplateId);
		for (const FOliveLibraryTemplateInfo* Ancestor : Chain)
		{
			if (Ancestor->TemplateId == TemplateId)
			{
				continue;  // Skip self, already searched
			}

			TSharedPtr<FJsonObject> AncestorJson = LoadFullJson(Ancestor->TemplateId);
			if (!AncestorJson.IsValid())
			{
				continue;
			}

			// Search ancestor's graphs for the function using the same lambda
			const TSharedPtr<FJsonObject>* AncestorGraphsObj = nullptr;
			if (AncestorJson->TryGetObjectField(TEXT("graphs"), AncestorGraphsObj) && AncestorGraphsObj)
			{
				MatchedFunc = SearchGraphArray(*AncestorGraphsObj, TEXT("functions"));
				if (!MatchedFunc)
				{
					MatchedFunc = SearchGraphArray(*AncestorGraphsObj, TEXT("event_graphs"));
				}
			}

			// Also check blueprint.functions[] (factory-style)
			if (!MatchedFunc)
			{
				const TSharedPtr<FJsonObject>* AncBlueprintObj = nullptr;
				if (AncestorJson->TryGetObjectField(TEXT("blueprint"), AncBlueprintObj) && AncBlueprintObj)
				{
					MatchedFunc = SearchGraphArray(*AncBlueprintObj, TEXT("functions"));
				}
			}

			if (MatchedFunc)
			{
				// Update FullJson to keep the ancestor's JSON alive (MatchedFunc points into it)
				FullJson = AncestorJson;
				InheritedFromLabel = Ancestor->TemplateId;
				break;
			}
		}
	}

	if (!MatchedFunc)
	{
		// List available function names with sentinel prefix so callers can detect not-found
		FString Result = FOliveLibraryIndex::GetFuncNotFoundSentinel();
		Result += FString::Printf(
			TEXT("Function '%s' not found in template '%s'.\nAvailable functions: "),
			*FunctionName, *TemplateId);
		bool bFirst = true;
		for (const FOliveLibraryFunctionSummary& Func : Info->Functions)
		{
			if (!bFirst) Result += TEXT(", ");
			Result += Func.Name;
			bFirst = false;
		}
		return Result;
	}

	// Build the result string
	FString Result;
	FString FuncName;
	(*MatchedFunc)->TryGetStringField(TEXT("name"), FuncName);

	if (InheritedFromLabel.IsEmpty())
	{
		Result += FString::Printf(TEXT("=== Function: %s (from template '%s') ===\n"),
			*FuncName, *Info->DisplayName);
	}
	else
	{
		Result += FString::Printf(TEXT("=== Function: %s (inherited from '%s', requested via '%s') ===\n"),
			*FuncName, *InheritedFromLabel, *Info->DisplayName);
	}

	// Description and tags
	FString Desc;
	if ((*MatchedFunc)->TryGetStringField(TEXT("description"), Desc) && !Desc.IsEmpty())
	{
		Result += FString::Printf(TEXT("Description: %s\n"), *Desc);
	}
	FString FuncTags;
	if ((*MatchedFunc)->TryGetStringField(TEXT("tags"), FuncTags) && !FuncTags.IsEmpty())
	{
		Result += FString::Printf(TEXT("Tags: %s\n"), *FuncTags);
	}

	// Inputs/outputs (if present, e.g. factory-style functions)
	const TArray<TSharedPtr<FJsonValue>>* InputsArr = nullptr;
	if ((*MatchedFunc)->TryGetArrayField(TEXT("inputs"), InputsArr) && InputsArr && InputsArr->Num() > 0)
	{
		Result += TEXT("Inputs: ");
		for (int32 i = 0; i < InputsArr->Num(); i++)
		{
			const TSharedPtr<FJsonObject>* InObj = nullptr;
			if ((*InputsArr)[i]->TryGetObject(InObj) && InObj)
			{
				FString PName, PType;
				(*InObj)->TryGetStringField(TEXT("name"), PName);
				(*InObj)->TryGetStringField(TEXT("type"), PType);
				if (i > 0) Result += TEXT(", ");
				Result += FString::Printf(TEXT("%s:%s"), *PName, *PType);
			}
		}
		Result += TEXT("\n");
	}

	const TArray<TSharedPtr<FJsonValue>>* OutputsArr = nullptr;
	if ((*MatchedFunc)->TryGetArrayField(TEXT("outputs"), OutputsArr) && OutputsArr && OutputsArr->Num() > 0)
	{
		Result += TEXT("Outputs: ");
		for (int32 i = 0; i < OutputsArr->Num(); i++)
		{
			const TSharedPtr<FJsonObject>* OutObj = nullptr;
			if ((*OutputsArr)[i]->TryGetObject(OutObj) && OutObj)
			{
				FString PName, PType;
				(*OutObj)->TryGetStringField(TEXT("name"), PName);
				(*OutObj)->TryGetStringField(TEXT("type"), PType);
				if (i > 0) Result += TEXT(", ");
				Result += FString::Printf(TEXT("%s:%s"), *PName, *PType);
			}
		}
		Result += TEXT("\n");
	}

	// Scan nodes for variable and dispatcher references
	const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
	if ((*MatchedFunc)->TryGetArrayField(TEXT("nodes"), NodesArr) && NodesArr)
	{
		TSet<FString> ReferencedVars;
		TSet<FString> ReferencedDispatchers;

		for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArr)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			if (!NodeVal->TryGetObject(NodeObj) || !NodeObj) continue;

			FString NodeClass;
			(*NodeObj)->TryGetStringField(TEXT("class"), NodeClass);

			if (NodeClass.Contains(TEXT("VariableGet")) || NodeClass.Contains(TEXT("VariableSet")))
			{
				FString VarName;
				if ((*NodeObj)->TryGetStringField(TEXT("variable_name"), VarName) ||
					(*NodeObj)->TryGetStringField(TEXT("member_name"), VarName))
				{
					ReferencedVars.Add(VarName);
				}
			}

			if (NodeClass.Contains(TEXT("CallDelegate")) || NodeClass.Contains(TEXT("AddDelegate")))
			{
				FString DelegName;
				if ((*NodeObj)->TryGetStringField(TEXT("delegate_name"), DelegName) ||
					(*NodeObj)->TryGetStringField(TEXT("member_name"), DelegName))
				{
					ReferencedDispatchers.Add(DelegName);
				}
			}
		}

		if (ReferencedVars.Num() > 0)
		{
			Result += TEXT("Required variables: ");
			bool bFirst = true;
			for (const FString& Var : ReferencedVars)
			{
				if (!bFirst) Result += TEXT(", ");
				Result += Var;
				bFirst = false;
			}
			Result += TEXT("\n");
		}

		if (ReferencedDispatchers.Num() > 0)
		{
			Result += TEXT("Required dispatchers: ");
			bool bFirst = true;
			for (const FString& Disp : ReferencedDispatchers)
			{
				if (!bFirst) Result += TEXT(", ");
				Result += Disp;
				bFirst = false;
			}
			Result += TEXT("\n");
		}

		// Full nodes as pretty-printed JSON
		Result += FString::Printf(TEXT("\nNodes (%d):\n"), NodesArr->Num());
		TSharedPtr<FJsonObject> NodesWrapper = MakeShared<FJsonObject>();
		// Copy the array to avoid const issues
		TArray<TSharedPtr<FJsonValue>> NodesCopy(*NodesArr);
		NodesWrapper->SetArrayField(TEXT("nodes"), NodesCopy);
		FString NodesPretty;
		TSharedRef<TJsonWriter<>> NodeWriter = TJsonWriterFactory<>::Create(&NodesPretty);
		FJsonSerializer::Serialize(NodesWrapper.ToSharedRef(), NodeWriter);
		Result += NodesPretty;
	}
	else
	{
		// Check for plan (factory-style function)
		const TSharedPtr<FJsonObject>* PlanObj = nullptr;
		if ((*MatchedFunc)->TryGetObjectField(TEXT("plan"), PlanObj) && PlanObj)
		{
			Result += TEXT("\nplan_json:\n");
			FString PlanPretty;
			TSharedRef<TJsonWriter<>> PlanWriter = TJsonWriterFactory<>::Create(&PlanPretty);
			FJsonSerializer::Serialize((*PlanObj).ToSharedRef(), PlanWriter);
			Result += PlanPretty;
		}
		else
		{
			Result += TEXT("\nNo node data or plan found for this function.\n");
		}
	}

	return Result;
}

// -----------------------------------------------------------------------------
// GetTemplateOverview
// -----------------------------------------------------------------------------

FString FOliveLibraryIndex::GetTemplateOverview(const FString& TemplateId) const
{
	const FOliveLibraryTemplateInfo* Info = FindTemplate(TemplateId);
	if (!Info)
	{
		// Return empty — handler checks Content.IsEmpty() and returns TEMPLATE_CONTENT_EMPTY
		return FString();
	}

	FString Result;

	Result += FString::Printf(TEXT("=== Template: %s (%s) ===\n"),
		*Info->DisplayName,
		Info->BlueprintType.IsEmpty() ? TEXT("Blueprint") : *Info->BlueprintType);

	if (!Info->CatalogDescription.IsEmpty())
	{
		Result += FString::Printf(TEXT("Description: %s\n"), *Info->CatalogDescription);
	}

	Result += FString::Printf(TEXT("Parent: %s (%s)\n"),
		Info->ParentClassName.IsEmpty() ? TEXT("Actor") : *Info->ParentClassName,
		Info->ParentClassSource.IsEmpty() ? TEXT("native") : *Info->ParentClassSource);

	if (!Info->SourceProject.IsEmpty())
	{
		Result += FString::Printf(TEXT("Source project: %s\n"), *Info->SourceProject);
	}

	if (!Info->Tags.IsEmpty())
	{
		Result += FString::Printf(TEXT("Tags: %s\n"), *Info->Tags);
	}

	if (!Info->DependsOn.IsEmpty())
	{
		Result += FString::Printf(TEXT("Depends on: %s\n"), *Info->DependsOn);

		TArray<const FOliveLibraryTemplateInfo*> Chain = ResolveInheritanceChain(TemplateId);
		if (Chain.Num() > 1)
		{
			Result += TEXT("Inheritance chain: ");
			for (int32 i = 0; i < Chain.Num(); i++)
			{
				if (i > 0) Result += TEXT(" -> ");
				Result += Chain[i]->TemplateId;
			}
			Result += TEXT("\n");
		}
	}

	// Variables
	if (Info->VariableNames.Num() > 0)
	{
		constexpr int32 MaxDisplay = 20;
		Result += FString::Printf(TEXT("\nVariables (%d): "), Info->VariableNames.Num());
		const int32 DisplayCount = FMath::Min(MaxDisplay, Info->VariableNames.Num());
		for (int32 i = 0; i < DisplayCount; i++)
		{
			if (i > 0) Result += TEXT(", ");
			Result += Info->VariableNames[i];
		}
		if (Info->VariableNames.Num() > MaxDisplay)
		{
			Result += FString::Printf(TEXT(" ...and %d more"), Info->VariableNames.Num() - MaxDisplay);
		}
		Result += TEXT("\n");
	}

	// Components
	if (Info->ComponentNames.Num() > 0)
	{
		Result += FString::Printf(TEXT("Components (%d): "), Info->ComponentNames.Num());
		for (int32 i = 0; i < Info->ComponentNames.Num(); i++)
		{
			if (i > 0) Result += TEXT(", ");
			Result += Info->ComponentNames[i];
		}
		Result += TEXT("\n");
	}

	// Interfaces
	if (Info->InterfaceNames.Num() > 0)
	{
		Result += TEXT("Interfaces: ");
		for (int32 i = 0; i < Info->InterfaceNames.Num(); i++)
		{
			if (i > 0) Result += TEXT(", ");
			Result += Info->InterfaceNames[i];
		}
		Result += TEXT("\n");
	}

	// Event dispatchers
	if (Info->DispatcherNames.Num() > 0)
	{
		Result += TEXT("Event Dispatchers: ");
		for (int32 i = 0; i < Info->DispatcherNames.Num(); i++)
		{
			if (i > 0) Result += TEXT(", ");
			Result += Info->DispatcherNames[i];
		}
		Result += TEXT("\n");
	}

	// Functions -- full detail for every function (tags, descriptions, node counts)
	if (Info->Functions.Num() > 0)
	{
		Result += FString::Printf(TEXT("\nFunctions (%d):\n"), Info->Functions.Num());
		for (int32 i = 0; i < Info->Functions.Num(); i++)
		{
			const FOliveLibraryFunctionSummary& Func = Info->Functions[i];
			Result += FString::Printf(TEXT("  - %s"), *Func.Name);
			if (Func.NodeCount > 0)
			{
				Result += FString::Printf(TEXT(" [%d nodes]"), Func.NodeCount);
			}
			if (!Func.Tags.IsEmpty())
			{
				Result += FString::Printf(TEXT(" {%s}"), *Func.Tags);
			}
			if (!Func.Description.IsEmpty())
			{
				Result += FString::Printf(TEXT(" -- %s"), *Func.Description);
			}
			Result += TEXT("\n");
		}
	}

	if (Info->bHasParameters)
	{
		Result += TEXT("\nHas parameters -- use blueprint.create(template_id=\"...\", path=\"...\") to instantiate.\n");
	}

	return Result;
}

// -----------------------------------------------------------------------------
// ResolveInheritanceChain
// -----------------------------------------------------------------------------

TArray<const FOliveLibraryTemplateInfo*> FOliveLibraryIndex::ResolveInheritanceChain(const FString& TemplateId) const
{
	TArray<const FOliveLibraryTemplateInfo*> Chain;
	TSet<FString> Visited;

	FString CurrentId = TemplateId;
	constexpr int32 MaxDepth = 10;

	while (!CurrentId.IsEmpty() && !Visited.Contains(CurrentId) && Chain.Num() < MaxDepth)
	{
		Visited.Add(CurrentId);
		const FOliveLibraryTemplateInfo* ChainInfo = FindTemplate(CurrentId);
		if (!ChainInfo)
		{
			break;
		}
		Chain.Add(ChainInfo);
		CurrentId = ChainInfo->DependsOn;
	}

	// Reverse so root is first, leaf is last
	Algo::Reverse(Chain);
	return Chain;
}

// -----------------------------------------------------------------------------
// BuildCatalog
// -----------------------------------------------------------------------------

FString FOliveLibraryIndex::BuildCatalog() const
{
	if (Templates.Num() == 0)
	{
		return FString();
	}

	FString Catalog;

	// Group by source project
	TMap<FString, TArray<const FOliveLibraryTemplateInfo*>> ByProject;
	int32 TotalFunctions = 0;

	for (const auto& Pair : Templates)
	{
		const FString Project = Pair.Value.SourceProject.IsEmpty()
			? TEXT("(local)")
			: Pair.Value.SourceProject;
		ByProject.FindOrAdd(Project).Add(&Pair.Value);
		TotalFunctions += Pair.Value.Functions.Num();
	}

	Catalog += FString::Printf(
		TEXT("Library templates (%d blueprints, %d functions total):\n"),
		Templates.Num(), TotalFunctions);

	// Common/noise words to exclude from domain keyword extraction
	static const TSet<FString> CatalogStopWords = {
		TEXT("actor"), TEXT("component"), TEXT("blueprint"), TEXT("function"),
		TEXT("event"), TEXT("variable"), TEXT("node"), TEXT("graph"),
		TEXT("parent"), TEXT("base"), TEXT("default"), TEXT("custom"),
		TEXT("comp"), TEXT("get"), TEXT("set"), TEXT("make"), TEXT("break"),
		TEXT("add"), TEXT("remove"), TEXT("update"), TEXT("init"),
		TEXT("begin"), TEXT("end"), TEXT("play"), TEXT("stop"),
		TEXT("notify"), TEXT("anim"), TEXT("animation"), TEXT("modifier"),
		TEXT("apply"), TEXT("revert"), TEXT("data"), TEXT("type"),
		TEXT("name"), TEXT("value"), TEXT("index"), TEXT("array"),
		TEXT("bool"), TEXT("float"), TEXT("int"), TEXT("string"),
		TEXT("vector"), TEXT("rotator"), TEXT("transform"), TEXT("class"),
		TEXT("object"), TEXT("struct"), TEXT("enum"), TEXT("delegate"),
		TEXT("new"), TEXT("old"), TEXT("current"), TEXT("target"),
		TEXT("source"), TEXT("input"), TEXT("output"), TEXT("result"),
		TEXT("state"), TEXT("status"), TEXT("info"), TEXT("config"),
		TEXT("owner"), TEXT("self"), TEXT("other"), TEXT("temp"),
		TEXT("time"), TEXT("delta"), TEXT("alpha"), TEXT("rate"),
		TEXT("length"), TEXT("count"), TEXT("max"), TEXT("min"),
		TEXT("check"), TEXT("is"), TEXT("has"), TEXT("can"),
		TEXT("on"), TEXT("off"), TEXT("enabled"), TEXT("disabled"),
		TEXT("true"), TEXT("false"), TEXT("none"), TEXT("null"),
		TEXT("curve"), TEXT("key"), TEXT("select"), TEXT("clear"),
		TEXT("first"), TEXT("last"), TEXT("next"), TEXT("prev"),
		TEXT("left"), TEXT("right"), TEXT("up"), TEXT("down"),
		TEXT("based"), TEXT("extractor"), TEXT("motion"), TEXT("speed"),
		TEXT("foot"), TEXT("move"), TEXT("sequence")
	};

	for (const auto& ProjectPair : ByProject)
	{
		// Count functions in this project
		int32 ProjectFunctions = 0;
		for (const FOliveLibraryTemplateInfo* T : ProjectPair.Value)
		{
			ProjectFunctions += T->Functions.Num();
		}

		// Extract domain keywords from tags across all templates in this project
		TMap<FString, int32> TagFrequency;
		for (const FOliveLibraryTemplateInfo* T : ProjectPair.Value)
		{
			TArray<FString> TagTokens;
			T->Tags.ParseIntoArray(TagTokens, TEXT(" "));
			for (const FString& Token : TagTokens)
			{
				FString Lower = Token.ToLower();
				if (Lower.Len() >= 3 && !CatalogStopWords.Contains(Lower))
				{
					TagFrequency.FindOrAdd(Lower)++;
				}
			}
		}

		// Sort by frequency and take top 25 domain keywords
		TArray<TPair<FString, int32>> SortedTags;
		for (const auto& TagPair : TagFrequency)
		{
			SortedTags.Add(TagPair);
		}
		SortedTags.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
		{
			return A.Value > B.Value;
		});

		FString KeywordList;
		const int32 MaxKeywords = 25;
		for (int32 i = 0; i < FMath::Min(MaxKeywords, SortedTags.Num()); i++)
		{
			if (!KeywordList.IsEmpty()) KeywordList += TEXT(", ");
			KeywordList += SortedTags[i].Key;
		}

		Catalog += FString::Printf(TEXT("- %s: %d blueprints, %d functions\n"),
			*ProjectPair.Key, ProjectPair.Value.Num(), ProjectFunctions);

		if (!KeywordList.IsEmpty())
		{
			Catalog += FString::Printf(TEXT("  Covers: %s\n"), *KeywordList);
		}

		// List factory templates (those with parameters) individually
		for (const FOliveLibraryTemplateInfo* T : ProjectPair.Value)
		{
			if (T->bHasParameters)
			{
				Catalog += FString::Printf(TEXT("  * %s: %s\n"),
					*T->TemplateId, *T->CatalogDescription);
			}
		}
	}

	Catalog += TEXT("Use blueprint.list_templates(query=\"...\") to find specific functions or patterns.\n");

	return Catalog;
}

