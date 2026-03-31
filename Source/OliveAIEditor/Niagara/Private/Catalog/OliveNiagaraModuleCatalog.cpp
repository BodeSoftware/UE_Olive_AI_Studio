// Copyright Bode Software. All Rights Reserved.

#include "OliveNiagaraModuleCatalog.h"
#include "IR/NiagaraIR.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "NiagaraScript.h"           // UNiagaraScript, ENiagaraScriptUsage
#include "NiagaraEditorUtilities.h"  // FNiagaraEditorUtilities::GetFilteredScriptAssets

DEFINE_LOG_CATEGORY_STATIC(LogOliveNiagaraCatalog, Log, All);

// -----------------------------------------------------------------------------
// Helper: Clean up an asset name into a human-readable display name.
// Removes common prefixes, inserts spaces before capitals.
// E.g., "SolveForcesAndVelocity" -> "Solve Forces And Velocity"
// -----------------------------------------------------------------------------
namespace
{
	FString CleanDisplayName(const FString& RawName)
	{
		if (RawName.IsEmpty())
		{
			return RawName;
		}

		FString Cleaned = RawName;

		// Insert spaces before uppercase letters that follow lowercase letters
		FString Result;
		Result.Reserve(Cleaned.Len() + 16);

		for (int32 i = 0; i < Cleaned.Len(); ++i)
		{
			TCHAR Ch = Cleaned[i];

			if (i > 0 && FChar::IsUpper(Ch))
			{
				TCHAR Prev = Cleaned[i - 1];
				// Insert space before upper that follows lower, or before upper
				// that starts a new word (upper followed by lower, preceded by upper)
				if (FChar::IsLower(Prev))
				{
					Result.AppendChar(TEXT(' '));
				}
				else if (FChar::IsUpper(Prev) && (i + 1 < Cleaned.Len()) && FChar::IsLower(Cleaned[i + 1]))
				{
					Result.AppendChar(TEXT(' '));
				}
			}

			// Replace underscores with spaces
			if (Ch == TEXT('_'))
			{
				Result.AppendChar(TEXT(' '));
			}
			else
			{
				Result.AppendChar(Ch);
			}
		}

		return Result.TrimStartAndEnd();
	}

	FString ExtractCategoryFromPath(const FString& AssetPath)
	{
		// Asset paths look like: /Niagara/Modules/Forces/DragModule
		// or: /Niagara/Modules/UpdateAge
		// We want the folder after "Modules/" if it exists

		int32 ModulesIdx = INDEX_NONE;
		if (AssetPath.FindLastChar(TEXT('/'), ModulesIdx))
		{
			// Get the parent path up to the last slash
			FString ParentPath = AssetPath.Left(ModulesIdx);

			// Find the last folder name in the parent
			int32 LastSlash = INDEX_NONE;
			if (ParentPath.FindLastChar(TEXT('/'), LastSlash))
			{
				FString FolderName = ParentPath.Mid(LastSlash + 1);

				// If the folder is "Modules" itself, the module is at the root level
				if (!FolderName.Equals(TEXT("Modules"), ESearchCase::IgnoreCase))
				{
					return FolderName;
				}
			}
		}

		return TEXT("General");
	}

	/**
	 * Build keywords from a display name by splitting CamelCase and underscores.
	 */
	void BuildKeywordsFromName(const FString& DisplayName, TArray<FString>& OutKeywords)
	{
		TArray<FString> Words;
		DisplayName.ParseIntoArray(Words, TEXT(" "), true);
		for (const FString& Word : Words)
		{
			FString Lower = Word.ToLower();
			if (Lower.Len() > 2 && !OutKeywords.Contains(Lower))
			{
				OutKeywords.Add(MoveTemp(Lower));
			}
		}
	}
}

// -----------------------------------------------------------------------------
// FOliveNiagaraModuleInfo
// -----------------------------------------------------------------------------

TSharedPtr<FJsonObject> FOliveNiagaraModuleInfo::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("script_asset_path"), ScriptAssetPath);
	Json->SetStringField(TEXT("display_name"), DisplayName);
	Json->SetStringField(TEXT("category"), Category);

	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}

	if (Keywords.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> KeywordsArray;
		for (const FString& Keyword : Keywords)
		{
			KeywordsArray.Add(MakeShared<FJsonValueString>(Keyword));
		}
		Json->SetArrayField(TEXT("keywords"), KeywordsArray);
	}

	if (ValidStages.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> StagesArray;
		for (EOliveIRNiagaraStage Stage : ValidStages)
		{
			FString StageName;
			switch (Stage)
			{
			case EOliveIRNiagaraStage::SystemSpawn:   StageName = TEXT("SystemSpawn"); break;
			case EOliveIRNiagaraStage::SystemUpdate:   StageName = TEXT("SystemUpdate"); break;
			case EOliveIRNiagaraStage::EmitterSpawn:   StageName = TEXT("EmitterSpawn"); break;
			case EOliveIRNiagaraStage::EmitterUpdate:  StageName = TEXT("EmitterUpdate"); break;
			case EOliveIRNiagaraStage::ParticleSpawn:  StageName = TEXT("ParticleSpawn"); break;
			case EOliveIRNiagaraStage::ParticleUpdate: StageName = TEXT("ParticleUpdate"); break;
			default: continue;
			}
			StagesArray.Add(MakeShared<FJsonValueString>(StageName));
		}
		Json->SetArrayField(TEXT("valid_stages"), StagesArray);
	}

	return Json;
}

// -----------------------------------------------------------------------------
// FOliveNiagaraModuleCatalog
// -----------------------------------------------------------------------------

FOliveNiagaraModuleCatalog& FOliveNiagaraModuleCatalog::Get()
{
	static FOliveNiagaraModuleCatalog Instance;
	return Instance;
}

void FOliveNiagaraModuleCatalog::Initialize()
{
	if (bInitialized)
	{
		return;
	}

	UE_LOG(LogOliveNiagaraCatalog, Log, TEXT("Initializing Niagara Module Catalog..."));

	// Discover Module scripts via the canonical Niagara utility, which queries asset
	// registry tags (LibraryVisibility, ScriptUsage) without loading every script object.
	// This avoids accessing UNiagaraScript member fields that may not be publicly exposed.
	FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions FilterOptions;
	FilterOptions.ScriptUsageToInclude = ENiagaraScriptUsage::Module;
	FilterOptions.bIncludeNonLibraryScripts = false;   // Library-visible only
	FilterOptions.bIncludeDeprecatedScripts = false;

	TArray<FAssetData> ScriptAssets;
	FNiagaraEditorUtilities::GetFilteredScriptAssets(FilterOptions, ScriptAssets);

	for (const FAssetData& Asset : ScriptAssets)
	{
		FOliveNiagaraModuleInfo Info;
		Info.ScriptAssetPath = Asset.GetSoftObjectPath().ToString();

		// Build display name from asset name, cleaned up
		FString RawName = Asset.AssetName.ToString();
		Info.DisplayName = CleanDisplayName(RawName);

		// Extract category from the asset's folder path
		FString PackagePath = Asset.PackagePath.ToString();
		Info.Category = ExtractCategoryFromPath(PackagePath);

		// Get description from asset registry tags.
		// UNiagaraScript exposes "ScriptDescription" as an AssetRegistrySearchable tag,
		// which is accessible without loading the script and avoids member access issues.
		{
			FString ScriptDescription;
			if (Asset.GetTagValue(TEXT("ScriptDescription"), ScriptDescription) && !ScriptDescription.IsEmpty())
			{
				Info.Description = MoveTemp(ScriptDescription);
			}
		}
		// Also try tooltip from the asset metadata as a fallback
		if (Info.Description.IsEmpty())
		{
			FString ToolTip;
			if (Asset.GetTagValue(TEXT("Tooltip"), ToolTip) && !ToolTip.IsEmpty())
			{
				Info.Description = MoveTemp(ToolTip);
			}
		}

		// Build keywords from display name words + category
		BuildKeywordsFromName(Info.DisplayName, Info.Keywords);
		if (!Info.Category.Equals(TEXT("General"), ESearchCase::IgnoreCase))
		{
			FString LowerCategory = Info.Category.ToLower();
			if (!Info.Keywords.Contains(LowerCategory))
			{
				Info.Keywords.Add(MoveTemp(LowerCategory));
			}
		}

		// Determine valid stages.
		// Most Niagara modules are usable on multiple stages. We check the script's
		// supported usage contexts. If the script has no explicit restrictions, we
		// default to the common particle-level stages.
		// DESIGN NOTE: UNiagaraScript doesn't directly expose "valid stages" — the
		// module usage context is controlled by ENiagaraScriptUsage plus the
		// "Suggested" category metadata. We infer stages from category/path heuristics
		// and mark most modules as available on all particle/emitter stages.
		// Architect should review if a more precise API is available.
		{
			bool bIsSystemLevel = Info.Category.Contains(TEXT("System"))
				|| Info.DisplayName.Contains(TEXT("System"))
				|| PackagePath.Contains(TEXT("/System/"));

			bool bIsEmitterLevel = Info.Category.Contains(TEXT("Emitter"))
				|| Info.DisplayName.Contains(TEXT("Emitter"));

			bool bIsSpawnOnly = Info.Category.Contains(TEXT("Spawn"))
				|| Info.DisplayName.Contains(TEXT("Spawn"))
				|| Info.DisplayName.Contains(TEXT("Initialize"));

			if (bIsSystemLevel)
			{
				Info.ValidStages.Add(EOliveIRNiagaraStage::SystemSpawn);
				Info.ValidStages.Add(EOliveIRNiagaraStage::SystemUpdate);
			}
			else if (bIsEmitterLevel)
			{
				Info.ValidStages.Add(EOliveIRNiagaraStage::EmitterSpawn);
				Info.ValidStages.Add(EOliveIRNiagaraStage::EmitterUpdate);
			}
			else if (bIsSpawnOnly)
			{
				// Spawn-specific modules go on spawn stages
				Info.ValidStages.Add(EOliveIRNiagaraStage::ParticleSpawn);
				Info.ValidStages.Add(EOliveIRNiagaraStage::EmitterSpawn);
			}
			else
			{
				// Default: available on all emitter and particle stages
				Info.ValidStages.Add(EOliveIRNiagaraStage::EmitterSpawn);
				Info.ValidStages.Add(EOliveIRNiagaraStage::EmitterUpdate);
				Info.ValidStages.Add(EOliveIRNiagaraStage::ParticleSpawn);
				Info.ValidStages.Add(EOliveIRNiagaraStage::ParticleUpdate);
			}
		}

		// Index this module
		int32 Index = ModuleInfos.Num();
		PathIndex.Add(Info.ScriptAssetPath, Index);

		FString LowerDisplayName = Info.DisplayName.ToLower();
		// Only add to DisplayNameIndex if not already present (first-come wins for duplicates)
		if (!DisplayNameIndex.Contains(LowerDisplayName))
		{
			DisplayNameIndex.Add(LowerDisplayName, Index);
		}

		ModuleInfos.Add(MoveTemp(Info));
	}

	bInitialized = true;
	UE_LOG(LogOliveNiagaraCatalog, Log, TEXT("Niagara Module Catalog initialized: %d modules"), ModuleInfos.Num());
}

void FOliveNiagaraModuleCatalog::Shutdown()
{
	ModuleInfos.Empty();
	PathIndex.Empty();
	DisplayNameIndex.Empty();
	bInitialized = false;
}

TArray<FOliveNiagaraModuleInfo> FOliveNiagaraModuleCatalog::Search(const FString& Query, EOliveIRNiagaraStage FilterStage) const
{
	// Build the candidate set, filtering by stage if requested
	TArray<int32> CandidateIndices;
	CandidateIndices.Reserve(ModuleInfos.Num());

	for (int32 i = 0; i < ModuleInfos.Num(); ++i)
	{
		if (FilterStage != EOliveIRNiagaraStage::Unknown)
		{
			if (!ModuleInfos[i].ValidStages.Contains(FilterStage))
			{
				continue;
			}
		}
		CandidateIndices.Add(i);
	}

	// If no query, return all candidates (filtered by stage)
	if (Query.IsEmpty())
	{
		TArray<FOliveNiagaraModuleInfo> Output;
		Output.Reserve(CandidateIndices.Num());
		for (int32 Idx : CandidateIndices)
		{
			Output.Add(ModuleInfos[Idx]);
		}
		return Output;
	}

	// Score and rank
	struct FSearchResult
	{
		int32 Index;
		int32 Score;
	};

	TArray<FSearchResult> Results;
	FString LowerQuery = Query.ToLower();

	for (int32 Idx : CandidateIndices)
	{
		int32 Score = ComputeSearchScore(ModuleInfos[Idx], LowerQuery);
		if (Score > 0)
		{
			Results.Add({ Idx, Score });
		}
	}

	// Sort by score descending
	Results.Sort([](const FSearchResult& A, const FSearchResult& B)
	{
		return A.Score > B.Score;
	});

	TArray<FOliveNiagaraModuleInfo> Output;
	Output.Reserve(Results.Num());
	for (const FSearchResult& Result : Results)
	{
		Output.Add(ModuleInfos[Result.Index]);
	}

	return Output;
}

const FOliveNiagaraModuleInfo* FOliveNiagaraModuleCatalog::GetByPath(const FString& ScriptPath) const
{
	const int32* Index = PathIndex.Find(ScriptPath);
	if (Index)
	{
		return &ModuleInfos[*Index];
	}
	return nullptr;
}

UNiagaraScript* FOliveNiagaraModuleCatalog::FindModuleScript(const FString& NameOrPath) const
{
	// 1. If the input looks like a path, try direct load
	if (NameOrPath.Contains(TEXT("/")))
	{
		UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *NameOrPath);
		if (Script)
		{
			return Script;
		}

		// Also try via PathIndex in case path format differs slightly
		const int32* Idx = PathIndex.Find(NameOrPath);
		if (Idx)
		{
			return LoadObject<UNiagaraScript>(nullptr, *ModuleInfos[*Idx].ScriptAssetPath);
		}
	}

	// 2. Try exact display name match (case-insensitive via lowered index)
	FString LowerName = NameOrPath.ToLower();
	const int32* NameIdx = DisplayNameIndex.Find(LowerName);
	if (NameIdx)
	{
		return LoadObject<UNiagaraScript>(nullptr, *ModuleInfos[*NameIdx].ScriptAssetPath);
	}

	// 3. Try display name with cleaned input (spaces inserted for CamelCase)
	FString CleanedInput = CleanDisplayName(NameOrPath).ToLower();
	if (CleanedInput != LowerName)
	{
		NameIdx = DisplayNameIndex.Find(CleanedInput);
		if (NameIdx)
		{
			return LoadObject<UNiagaraScript>(nullptr, *ModuleInfos[*NameIdx].ScriptAssetPath);
		}
	}

	// 4. Partial match fallback: find the best-scoring module by name
	int32 BestScore = 0;
	int32 BestIndex = INDEX_NONE;
	for (int32 i = 0; i < ModuleInfos.Num(); ++i)
	{
		// Score only against display name for this lookup to avoid false positives
		const FString& ModuleName = ModuleInfos[i].DisplayName;
		int32 Score = 0;

		if (ModuleName.Equals(NameOrPath, ESearchCase::IgnoreCase))
		{
			Score = 100;
		}
		else if (ModuleName.Contains(NameOrPath, ESearchCase::IgnoreCase))
		{
			Score = 50;
		}
		else if (NameOrPath.Contains(ModuleName, ESearchCase::IgnoreCase))
		{
			Score = 40;
		}

		if (Score > BestScore)
		{
			BestScore = Score;
			BestIndex = i;
		}
	}

	if (BestIndex != INDEX_NONE && BestScore >= 40)
	{
		return LoadObject<UNiagaraScript>(nullptr, *ModuleInfos[BestIndex].ScriptAssetPath);
	}

	return nullptr;
}

TSharedPtr<FJsonObject> FOliveNiagaraModuleCatalog::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> ModulesArray;
	ModulesArray.Reserve(ModuleInfos.Num());
	for (const FOliveNiagaraModuleInfo& Info : ModuleInfos)
	{
		ModulesArray.Add(MakeShared<FJsonValueObject>(Info.ToJson()));
	}
	Json->SetArrayField(TEXT("modules"), ModulesArray);
	Json->SetNumberField(TEXT("count"), ModuleInfos.Num());

	// Provide category summary
	TMap<FString, int32> CategoryCounts;
	for (const FOliveNiagaraModuleInfo& Info : ModuleInfos)
	{
		int32& Count = CategoryCounts.FindOrAdd(Info.Category);
		Count++;
	}

	TSharedPtr<FJsonObject> CategoriesJson = MakeShared<FJsonObject>();
	for (const auto& Pair : CategoryCounts)
	{
		CategoriesJson->SetNumberField(Pair.Key, Pair.Value);
	}
	Json->SetObjectField(TEXT("categories"), CategoriesJson);

	return Json;
}

int32 FOliveNiagaraModuleCatalog::GetModuleCount() const
{
	return ModuleInfos.Num();
}

int32 FOliveNiagaraModuleCatalog::ComputeSearchScore(const FOliveNiagaraModuleInfo& Info, const FString& Query) const
{
	// Query is expected to be already lowercased by the caller
	int32 Score = 0;

	// Exact display name match (highest priority)
	if (Info.DisplayName.Equals(Query, ESearchCase::IgnoreCase))
	{
		Score += 100;
	}
	// Display name contains query
	else if (Info.DisplayName.Contains(Query, ESearchCase::IgnoreCase))
	{
		Score += 50;
	}

	// Category match
	if (Info.Category.Contains(Query, ESearchCase::IgnoreCase))
	{
		Score += 30;
	}

	// Keyword match
	for (const FString& Keyword : Info.Keywords)
	{
		if (Keyword.Contains(Query, ESearchCase::IgnoreCase))
		{
			Score += 20;
			break; // Only count one keyword match
		}
	}

	// Description match (lowest priority)
	if (!Info.Description.IsEmpty() && Info.Description.Contains(Query, ESearchCase::IgnoreCase))
	{
		Score += 10;
	}

	// Asset path match (useful when the user provides a partial path)
	if (Info.ScriptAssetPath.Contains(Query, ESearchCase::IgnoreCase))
	{
		Score += 15;
	}

	return Score;
}
