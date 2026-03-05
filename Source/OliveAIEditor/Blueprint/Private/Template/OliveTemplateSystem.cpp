// Copyright Bode Software. All Rights Reserved.

/**
 * OliveTemplateSystem.cpp
 *
 * Implements the template loading, catalog generation, parameter substitution,
 * and stub execution for the Olive template system. Templates are JSON files
 * loaded from Content/Templates/ at startup.
 *
 * See plans/phase4_template_implementation.md Task 1 for full specification.
 */

#include "Template/OliveTemplateSystem.h"

#include "Writer/OliveBlueprintWriter.h"
#include "Writer/OliveComponentWriter.h"
#include "Plan/OliveBlueprintPlanResolver.h"
#include "Plan/OlivePlanExecutor.h"
#include "Compile/OliveCompileManager.h"
#include "OliveBlueprintTypes.h"
#include "IR/BlueprintPlanIR.h"
#include "IR/BlueprintIR.h"
#include "IR/CommonIR.h"
#include "IR/OliveIRTypes.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

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
	CachedCatalog += TEXT("Search library templates with blueprint.list_templates(query=\"...\") for proven patterns from real projects.\n");
	CachedCatalog += TEXT("Use blueprint.get_template(id, pattern=\"FuncName\") to read a specific function's full node graph.\n");
	CachedCatalog += TEXT("Factory templates support blueprint.create with template_id for quick scaffolding.\n\n");

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
		CachedCatalog += TEXT("Factory templates (create complete Blueprints, or extract individual functions with get_template):\n");
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
// Parameter Substitution
// =============================================================================

FString FOliveTemplateSystem::SubstituteParameters(
	const FString& Input,
	const TMap<FString, FString>& MergedParams) const
{
	FString Result = Input;

	for (const auto& Pair : MergedParams)
	{
		const FString Token = FString::Printf(TEXT("${%s}"), *Pair.Key);
		Result = Result.Replace(*Token, *Pair.Value);
	}

	// Warn about unsubstituted tokens
	int32 Idx = 0;
	while ((Idx = Result.Find(TEXT("${"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Idx)) != INDEX_NONE)
	{
		int32 End = Result.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Idx);
		if (End != INDEX_NONE)
		{
			FString Unresolved = Result.Mid(Idx, End - Idx + 1);
			UE_LOG(LogOliveTemplates, Warning,
				TEXT("Unsubstituted parameter in template: %s"), *Unresolved);
			Idx = End + 1;
		}
		else
		{
			break;
		}
	}

	return Result;
}

FString FOliveTemplateSystem::EvaluateConditionals(
	const FString& Input,
	const TMap<FString, FString>& MergedParams) const
{
	FString Result = Input;

	// Find patterns like: "true ? value_true : value_false" or "false ? value_true : value_false"
	// These appear in template defaults after parameter substitution, e.g.:
	//   "true ? 100.0 : 0" (when start_full=true, max_value=100.0)
	//
	// We scan for the pattern: BOOL_LITERAL ? VALUE : VALUE
	// where BOOL_LITERAL is "true" or "false" (case-insensitive).
	// The pattern is expected to appear as a complete value in a JSON string,
	// so we look for it between quotes.

	// Handle the pattern: "EXPR ? VAL_TRUE : VAL_FALSE"
	// After SubstituteParameters, conditionals look like:
	//   "true ? 100.0 : 0" or "false ? 100.0 : 0"

	// We use a simple iterative approach since FRegexMatcher is not available in all UE configs
	auto EvaluateSingleConditional = [](const FString& Expr) -> FString
	{
		// Expected format: "BOOL ? VAL_TRUE : VAL_FALSE"
		// Trim whitespace
		FString Trimmed = Expr.TrimStartAndEnd();

		int32 QuestionIdx = Trimmed.Find(TEXT("?"), ESearchCase::CaseSensitive);
		if (QuestionIdx == INDEX_NONE)
		{
			return Expr; // Not a conditional
		}

		int32 ColonIdx = Trimmed.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, QuestionIdx + 1);
		if (ColonIdx == INDEX_NONE)
		{
			return Expr; // Malformed conditional
		}

		FString Condition = Trimmed.Left(QuestionIdx).TrimStartAndEnd();
		FString TrueVal = Trimmed.Mid(QuestionIdx + 1, ColonIdx - QuestionIdx - 1).TrimStartAndEnd();
		FString FalseVal = Trimmed.Mid(ColonIdx + 1).TrimStartAndEnd();

		// Evaluate the condition: only "true"/"false" bool literals are supported
		if (Condition.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			return TrueVal;
		}
		else if (Condition.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			return FalseVal;
		}

		// Not a recognized boolean -- return as-is
		UE_LOG(LogOliveTemplates, Warning,
			TEXT("Conditional expression has non-boolean condition '%s', leaving as-is: %s"),
			*Condition, *Expr);
		return Expr;
	};

	// Scan for conditional patterns within JSON string values.
	// After parameter substitution, conditionals appear as JSON string values like:
	//   "default": "true ? 100.0 : 0"
	// We need to find these and evaluate them in-place.
	//
	// Strategy: find "X ? Y : Z" patterns where X is true/false.
	// We look for " ? " as the delimiter (with spaces for safety).

	int32 SearchStart = 0;
	while (SearchStart < Result.Len())
	{
		// Find " ? " pattern
		int32 QIdx = Result.Find(TEXT(" ? "), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
		if (QIdx == INDEX_NONE)
		{
			break;
		}

		// Find " : " after the "?"
		int32 CIdx = Result.Find(TEXT(" : "), ESearchCase::CaseSensitive, ESearchDir::FromStart, QIdx + 3);
		if (CIdx == INDEX_NONE)
		{
			SearchStart = QIdx + 3;
			continue;
		}

		// Extract the condition part: scan backwards from " ? " to find the start of the expression.
		// The condition is a bool literal, so look for "true" or "false" before the " ? ".
		// In JSON context, the expression is bounded by a quote on the left.
		int32 ExprStart = QIdx;

		// Scan backward to find the start of the condition (skip alphanumeric chars)
		while (ExprStart > 0 && !FChar::IsWhitespace(Result[ExprStart - 1])
			&& Result[ExprStart - 1] != TEXT('"')
			&& Result[ExprStart - 1] != TEXT(','))
		{
			ExprStart--;
		}

		// Extract the true/false value: scan forward from " : " to find the end
		int32 ExprEnd = CIdx + 3;
		while (ExprEnd < Result.Len()
			&& Result[ExprEnd] != TEXT('"')
			&& Result[ExprEnd] != TEXT(',')
			&& Result[ExprEnd] != TEXT('}'))
		{
			ExprEnd++;
		}

		FString FullExpr = Result.Mid(ExprStart, ExprEnd - ExprStart);
		FString Evaluated = EvaluateSingleConditional(FullExpr);

		if (Evaluated != FullExpr)
		{
			Result = Result.Left(ExprStart) + Evaluated + Result.Mid(ExprEnd);
			// Don't advance SearchStart past the replacement -- it's shorter
			SearchStart = ExprStart + Evaluated.Len();
		}
		else
		{
			SearchStart = ExprEnd;
		}
	}

	return Result;
}

TMap<FString, FString> FOliveTemplateSystem::MergeParameters(
	const FOliveTemplateInfo& Info,
	const TMap<FString, FString>& UserParams,
	const FString& PresetName) const
{
	TMap<FString, FString> Merged;

	// Step 1: Start with defaults from the template's parameter schema
	if (Info.FullJson.IsValid())
	{
		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		if (Info.FullJson->TryGetObjectField(TEXT("parameters"), ParamsObj) && ParamsObj)
		{
			for (const auto& Pair : (*ParamsObj)->Values)
			{
				const TSharedPtr<FJsonObject>* ParamDef = nullptr;
				if (Pair.Value->TryGetObject(ParamDef) && ParamDef)
				{
					FString Default;
					(*ParamDef)->TryGetStringField(TEXT("default"), Default);
					Merged.Add(Pair.Key, Default);
				}
			}
		}

		// Step 2: Overlay preset values if a preset name is specified
		// Presets are stored as a JSON object: { "Bullet": { "speed": "5000", ... }, "Rocket": { ... } }
		// The key is the preset name, the value is the parameter overrides directly.
		if (!PresetName.IsEmpty())
		{
			bool bPresetFound = false;
			const TSharedPtr<FJsonObject>* PresetsObj = nullptr;
			if (Info.FullJson->TryGetObjectField(TEXT("presets"), PresetsObj) && PresetsObj)
			{
				// Case-insensitive preset lookup
				for (const auto& PresetPair : (*PresetsObj)->Values)
				{
					if (!PresetPair.Key.Equals(PresetName, ESearchCase::IgnoreCase))
					{
						continue;
					}

					const TSharedPtr<FJsonObject>* PresetValues = nullptr;
					if (PresetPair.Value->TryGetObject(PresetValues) && PresetValues)
					{
						for (const auto& PPair : (*PresetValues)->Values)
						{
							FString Value;
							if (PPair.Value->TryGetString(Value))
							{
								Merged.Add(PPair.Key, Value);
							}
						}
					}

					bPresetFound = true;
					break;
				}
			}

			if (!bPresetFound)
			{
				UE_LOG(LogOliveTemplates, Warning,
					TEXT("Preset '%s' not found in template '%s'. Using defaults only."),
					*PresetName, *Info.TemplateId);
			}
		}
	}

	// Step 3: Overlay user-provided overrides (highest priority)
	for (const auto& Pair : UserParams)
	{
		Merged.Add(Pair.Key, Pair.Value);
	}

	return Merged;
}

// =============================================================================
// Helpers
// =============================================================================

namespace
{
	/** Map a simple type string ("Float", "Int", etc.) to EOliveIRTypeCategory */
	EOliveIRTypeCategory ParseSimpleTypeCategory(const FString& TypeStr)
	{
		const FString Lower = TypeStr.ToLower();
		if (Lower == TEXT("float"))     return EOliveIRTypeCategory::Float;
		if (Lower == TEXT("double"))    return EOliveIRTypeCategory::Double;
		if (Lower == TEXT("int") || Lower == TEXT("integer")) return EOliveIRTypeCategory::Int;
		if (Lower == TEXT("int64"))     return EOliveIRTypeCategory::Int64;
		if (Lower == TEXT("bool") || Lower == TEXT("boolean")) return EOliveIRTypeCategory::Bool;
		if (Lower == TEXT("byte"))      return EOliveIRTypeCategory::Byte;
		if (Lower == TEXT("string"))    return EOliveIRTypeCategory::String;
		if (Lower == TEXT("name"))      return EOliveIRTypeCategory::Name;
		if (Lower == TEXT("text"))      return EOliveIRTypeCategory::Text;
		if (Lower == TEXT("vector"))    return EOliveIRTypeCategory::Vector;
		if (Lower == TEXT("rotator"))   return EOliveIRTypeCategory::Rotator;
		if (Lower == TEXT("transform")) return EOliveIRTypeCategory::Transform;
		if (Lower == TEXT("object"))    return EOliveIRTypeCategory::Object;
		if (Lower == TEXT("class"))     return EOliveIRTypeCategory::Class;
		if (Lower == TEXT("struct"))    return EOliveIRTypeCategory::Struct;
		if (Lower == TEXT("enum"))      return EOliveIRTypeCategory::Enum;
		return EOliveIRTypeCategory::Unknown;
	}

	/**
	 * When ParseSimpleTypeCategory returns Unknown, try to resolve the raw type string
	 * as a UClass or UScriptStruct. This handles template authors who write "type": "Actor"
	 * instead of the canonical "type": "object", "class_name": "Actor".
	 */
	void ResolveUnknownIRType(FOliveIRType& Type, const FString& RawTypeStr)
	{
		if (Type.Category != EOliveIRTypeCategory::Unknown || RawTypeStr.IsEmpty())
		{
			return;
		}

		// Try with and without common UE prefixes
		TArray<FString> Candidates;
		Candidates.Add(RawTypeStr);
		if (!RawTypeStr.StartsWith(TEXT("A")) && !RawTypeStr.StartsWith(TEXT("U")))
		{
			Candidates.Add(TEXT("A") + RawTypeStr);
			Candidates.Add(TEXT("U") + RawTypeStr);
		}
		if (!RawTypeStr.StartsWith(TEXT("F")))
		{
			Candidates.Add(TEXT("F") + RawTypeStr);
		}

		// Try UClass first
		for (const FString& Name : Candidates)
		{
			UClass* Class = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Name));
			if (!Class)
			{
				Class = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/CoreUObject.%s"), *Name));
			}
			if (Class)
			{
				Type.Category = EOliveIRTypeCategory::Object;
				Type.ClassName = Class->GetName();
				UE_LOG(LogOliveTemplates, Log,
					TEXT("ResolveUnknownIRType: '%s' resolved as UClass '%s'"),
					*RawTypeStr, *Type.ClassName);
				return;
			}
		}

		// Try UScriptStruct
		for (const FString& Name : Candidates)
		{
			UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Name));
			if (!Struct)
			{
				Struct = FindObject<UScriptStruct>(nullptr, *FString::Printf(TEXT("/Script/CoreUObject.%s"), *Name));
			}
			if (Struct)
			{
				Type.Category = EOliveIRTypeCategory::Struct;
				Type.StructName = Struct->GetName();
				UE_LOG(LogOliveTemplates, Log,
					TEXT("ResolveUnknownIRType: '%s' resolved as UScriptStruct '%s'"),
					*RawTypeStr, *Type.StructName);
				return;
			}
		}

		UE_LOG(LogOliveTemplates, Warning,
			TEXT("ResolveUnknownIRType: Could not resolve '%s' as class or struct"),
			*RawTypeStr);
	}

	/** Parse a template variable JSON object into FOliveIRVariable */
	FOliveIRVariable ParseTemplateVariable(const TSharedPtr<FJsonObject>& VarJson)
	{
		FOliveIRVariable Var;
		if (!VarJson.IsValid()) return Var;

		VarJson->TryGetStringField(TEXT("name"), Var.Name);
		VarJson->TryGetStringField(TEXT("default"), Var.DefaultValue);
		VarJson->TryGetStringField(TEXT("category"), Var.Category);

		FString TypeStr;
		if (VarJson->TryGetStringField(TEXT("type"), TypeStr))
		{
			Var.Type.Category = ParseSimpleTypeCategory(TypeStr);
		}

		// Read type metadata fields
		VarJson->TryGetStringField(TEXT("class_name"), Var.Type.ClassName);
		VarJson->TryGetStringField(TEXT("struct_name"), Var.Type.StructName);
		VarJson->TryGetStringField(TEXT("enum_name"), Var.Type.EnumName);

		// Fallback: if category is Unknown, try to resolve the raw type string
		ResolveUnknownIRType(Var.Type, TypeStr);

		return Var;
	}

	/** Parse a template function param JSON object into FOliveIRFunctionParam */
	FOliveIRFunctionParam ParseTemplateFuncParam(const TSharedPtr<FJsonObject>& ParamJson)
	{
		FOliveIRFunctionParam Param;
		if (!ParamJson.IsValid()) return Param;

		ParamJson->TryGetStringField(TEXT("name"), Param.Name);

		FString TypeStr;
		if (ParamJson->TryGetStringField(TEXT("type"), TypeStr))
		{
			Param.Type.Category = ParseSimpleTypeCategory(TypeStr);
		}

		// Read type metadata fields
		ParamJson->TryGetStringField(TEXT("class_name"), Param.Type.ClassName);
		ParamJson->TryGetStringField(TEXT("struct_name"), Param.Type.StructName);
		ParamJson->TryGetStringField(TEXT("enum_name"), Param.Type.EnumName);
		ParamJson->TryGetBoolField(TEXT("is_reference"), Param.bIsReference);

		// Fallback: if category is Unknown, try to resolve the raw type string
		ResolveUnknownIRType(Param.Type, TypeStr);

		return Param;
	}

	/** Parse blueprint type string to enum */
	EOliveBlueprintType ParseBlueprintType(const FString& TypeStr)
	{
		const FString Upper = TypeStr.ToUpper();
		if (Upper == TEXT("ACTORCOMPONENT") || Upper == TEXT("ACTOR_COMPONENT"))
			return EOliveBlueprintType::ActorComponent;
		if (Upper == TEXT("INTERFACE"))
			return EOliveBlueprintType::Interface;
		if (Upper == TEXT("FUNCTIONLIBRARY") || Upper == TEXT("FUNCTION_LIBRARY"))
			return EOliveBlueprintType::FunctionLibrary;
		return EOliveBlueprintType::Normal;
	}

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

// =============================================================================
// ApplyTemplate
// =============================================================================

FOliveToolResult FOliveTemplateSystem::ApplyTemplate(
	const FString& TemplateId,
	const TMap<FString, FString>& UserParams,
	const FString& PresetName,
	const FString& AssetPath)
{
	// 1. Validate template
	const FOliveTemplateInfo* Info = FindTemplate(TemplateId);
	if (!Info)
	{
		return FOliveToolResult::Error(
			TEXT("TEMPLATE_NOT_FOUND"),
			FString::Printf(TEXT("Template '%s' not found"), *TemplateId),
			TEXT("Use blueprint.list_templates to see available templates, or olive.search_community_blueprints(query) for community examples"));
	}

	if (Info->TemplateType != TEXT("factory"))
	{
		return FOliveToolResult::Error(
			TEXT("TEMPLATE_NOT_FACTORY"),
			FString::Printf(TEXT("Template '%s' is a reference template, not factory. "
				"Use blueprint.get_template to view reference templates."), *TemplateId),
			TEXT("Only factory templates can be instantiated with blueprint.create + template_id"));
	}

	if (!Info->FullJson.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("TEMPLATE_NOT_FOUND"),
			FString::Printf(TEXT("Template '%s' has no loaded JSON content"), *TemplateId),
			TEXT("Check template file is valid JSON"));
	}

	// 2. Merge parameters
	TMap<FString, FString> MergedParams = MergeParameters(*Info, UserParams, PresetName);

	// 3. Get blueprint section, serialize, substitute, re-parse
	const TSharedPtr<FJsonObject>* BlueprintSection = nullptr;
	if (!Info->FullJson->TryGetObjectField(TEXT("blueprint"), BlueprintSection) || !BlueprintSection)
	{
		return FOliveToolResult::Error(
			TEXT("TEMPLATE_APPLY_CREATE_FAILED"),
			FString::Printf(TEXT("Template '%s' has no 'blueprint' section"), *TemplateId),
			TEXT("Check template JSON structure"));
	}

	FString BlueprintJsonStr = JsonToString(*BlueprintSection);
	BlueprintJsonStr = SubstituteParameters(BlueprintJsonStr, MergedParams);
	BlueprintJsonStr = EvaluateConditionals(BlueprintJsonStr, MergedParams);

	TSharedPtr<FJsonObject> SubstitutedBP;
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BlueprintJsonStr);
		if (!FJsonSerializer::Deserialize(Reader, SubstitutedBP) || !SubstitutedBP.IsValid())
		{
			return FOliveToolResult::Error(
				TEXT("TEMPLATE_APPLY_CREATE_FAILED"),
				TEXT("Failed to re-parse substituted blueprint JSON. Likely an unresolved ${param} broke JSON syntax."),
				TEXT("Check that all required parameters have values"));
		}
	}

	// 4. Extract parent class and type
	FString ParentClass;
	SubstitutedBP->TryGetStringField(TEXT("parent_class"), ParentClass);
	if (ParentClass.IsEmpty())
	{
		ParentClass = TEXT("Actor");
	}

	FString TypeStr;
	SubstitutedBP->TryGetStringField(TEXT("type"), TypeStr);
	EOliveBlueprintType BPType = ParseBlueprintType(TypeStr);

	// Accumulate warnings for non-fatal failures
	TArray<FString> Warnings;
	TArray<FString> CreatedComponents;
	TArray<FString> CreatedVariables;
	TArray<FString> CreatedDispatchers;
	TArray<FString> CreatedFunctions;

	// Per-function and per-event-graph summaries for the result JSON.
	// This lets the AI see what logic was built without needing to call blueprint.read.
	struct FTemplateFunctionSummary
	{
		FString Name;
		int32 NodeCount = 0;
		TArray<FString> StepSummaries;  // e.g., "evt: event BeginPlay", "spawn: spawn_actor Actor"
		bool bPlanExecuted = false;
		bool bPlanSucceeded = false;
		TArray<FString> PlanErrors;
	};
	TArray<FTemplateFunctionSummary> FunctionSummaries;
	TArray<FTemplateFunctionSummary> EventGraphSummaries;

	// 5. Create the Blueprint
	FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
	FOliveBlueprintWriteResult CreateResult = Writer.CreateBlueprint(AssetPath, ParentClass, BPType);
	if (!CreateResult.bSuccess)
	{
		return FOliveToolResult::Error(
			TEXT("TEMPLATE_APPLY_CREATE_FAILED"),
			FString::Printf(TEXT("Failed to create Blueprint at '%s': %s"),
				*AssetPath,
				CreateResult.Warnings.Num() > 0 ? *CreateResult.Warnings[0] : TEXT("Unknown error")),
			TEXT("Check asset path and parent class"));
	}

	// 6. Load the created Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("TEMPLATE_APPLY_CREATE_FAILED"),
			FString::Printf(TEXT("Blueprint created but failed to load at '%s'"), *AssetPath),
			TEXT("This is unexpected — check editor log for details"));
	}

	// Wrap all modifications in a single transaction
	{
		const FScopedTransaction Transaction(
			FText::Format(NSLOCTEXT("OliveTemplates", "ApplyTemplate",
				"Olive: Apply Template '{0}'"), FText::FromString(TemplateId)));

		Blueprint->Modify();

		// 7. Add components
		const TArray<TSharedPtr<FJsonValue>>* ComponentsArray = nullptr;
		if (SubstitutedBP->TryGetArrayField(TEXT("components"), ComponentsArray) && ComponentsArray)
		{
			FOliveComponentWriter& CompWriter = FOliveComponentWriter::Get();

			for (const TSharedPtr<FJsonValue>& CompVal : *ComponentsArray)
			{
				const TSharedPtr<FJsonObject>* CompObj = nullptr;
				if (!CompVal->TryGetObject(CompObj) || !CompObj) continue;

				FString CompClass, CompName, CompParent;
				(*CompObj)->TryGetStringField(TEXT("class"), CompClass);
				(*CompObj)->TryGetStringField(TEXT("name"), CompName);
				(*CompObj)->TryGetStringField(TEXT("parent"), CompParent);

				bool bIsRoot = false;
				(*CompObj)->TryGetBoolField(TEXT("is_root"), bIsRoot);

				if (CompClass.IsEmpty() || CompName.IsEmpty())
				{
					Warnings.Add(FString::Printf(
						TEXT("Skipped component with empty class='%s' or name='%s'"),
						*CompClass, *CompName));
					continue;
				}

				// Create the component
				FOliveBlueprintWriteResult CompResult = CompWriter.AddComponent(
					AssetPath, CompClass, CompName, CompParent);
				if (!CompResult.bSuccess)
				{
					FString Msg = FString::Printf(TEXT("Failed to add component '%s' (%s)"),
						*CompName, *CompClass);
					if (CompResult.Warnings.Num() > 0) Msg += TEXT(": ") + CompResult.Warnings[0];
					Warnings.Add(Msg);
					continue;
				}

				CreatedComponents.Add(CompName);

				// Set as root component if requested
				if (bIsRoot)
				{
					FOliveBlueprintWriteResult RootResult = CompWriter.SetRootComponent(
						AssetPath, CompName);
					if (!RootResult.bSuccess)
					{
						FString Msg = FString::Printf(
							TEXT("Component '%s' created but failed to set as root"), *CompName);
						if (RootResult.Warnings.Num() > 0) Msg += TEXT(": ") + RootResult.Warnings[0];
						Warnings.Add(Msg);
					}
				}

				// Apply properties if present
				const TSharedPtr<FJsonObject>* PropsObj = nullptr;
				if ((*CompObj)->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
				{
					TMap<FString, FString> PropertiesMap;
					for (const auto& PropPair : (*PropsObj)->Values)
					{
						FString PropVal;
						if (PropPair.Value->TryGetString(PropVal))
						{
							PropertiesMap.Add(PropPair.Key, PropVal);
						}
					}

					if (PropertiesMap.Num() > 0)
					{
						FOliveBlueprintWriteResult ModResult = CompWriter.ModifyComponent(
							AssetPath, CompName, PropertiesMap);
						if (!ModResult.bSuccess)
						{
							FString Msg = FString::Printf(
								TEXT("Component '%s' created but property modification failed"),
								*CompName);
							if (ModResult.Warnings.Num() > 0) Msg += TEXT(": ") + ModResult.Warnings[0];
							Warnings.Add(Msg);
						}
					}
				}
			}
		}

		// 8. Add variables
		const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
		if (SubstitutedBP->TryGetArrayField(TEXT("variables"), VarsArray) && VarsArray)
		{
			for (const TSharedPtr<FJsonValue>& VarVal : *VarsArray)
			{
				const TSharedPtr<FJsonObject>* VarObj = nullptr;
				if (!VarVal->TryGetObject(VarObj) || !VarObj) continue;

				FOliveIRVariable VarIR = ParseTemplateVariable(*VarObj);
				if (VarIR.Name.IsEmpty())
				{
					Warnings.Add(TEXT("Skipped variable with empty name"));
					continue;
				}

				FOliveBlueprintWriteResult VarResult = Writer.AddVariable(AssetPath, VarIR);
				if (VarResult.bSuccess)
				{
					CreatedVariables.Add(VarIR.Name);
				}
				else
				{
					FString Msg = FString::Printf(TEXT("Failed to add variable '%s'"), *VarIR.Name);
					if (VarResult.Warnings.Num() > 0) Msg += TEXT(": ") + VarResult.Warnings[0];
					Warnings.Add(Msg);
				}
			}
		}

		// 9. Add event dispatchers
		const TArray<TSharedPtr<FJsonValue>>* DispatchersArray = nullptr;
		if (SubstitutedBP->TryGetArrayField(TEXT("event_dispatchers"), DispatchersArray) && DispatchersArray)
		{
			for (const TSharedPtr<FJsonValue>& DispVal : *DispatchersArray)
			{
				const TSharedPtr<FJsonObject>* DispObj = nullptr;
				if (!DispVal->TryGetObject(DispObj) || !DispObj) continue;

				FString DispName;
				(*DispObj)->TryGetStringField(TEXT("name"), DispName);
				if (DispName.IsEmpty())
				{
					Warnings.Add(TEXT("Skipped dispatcher with empty name"));
					continue;
				}

				// Parse dispatcher params
				TArray<FOliveIRFunctionParam> DispParams;
				const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
				if ((*DispObj)->TryGetArrayField(TEXT("params"), ParamsArray) && ParamsArray)
				{
					for (const TSharedPtr<FJsonValue>& PVal : *ParamsArray)
					{
						const TSharedPtr<FJsonObject>* PObj = nullptr;
						if (PVal->TryGetObject(PObj) && PObj)
						{
							DispParams.Add(ParseTemplateFuncParam(*PObj));
						}
					}
				}

				FOliveBlueprintWriteResult DispResult = Writer.AddEventDispatcher(AssetPath, DispName, DispParams);
				if (DispResult.bSuccess)
				{
					CreatedDispatchers.Add(DispName);
				}
				else
				{
					FString Msg = FString::Printf(TEXT("Failed to add dispatcher '%s'"), *DispName);
					if (DispResult.Warnings.Num() > 0) Msg += TEXT(": ") + DispResult.Warnings[0];
					Warnings.Add(Msg);
				}
			}
		}

		// 10. Create functions and execute plans
		const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
		if (SubstitutedBP->TryGetArrayField(TEXT("functions"), FunctionsArray) && FunctionsArray)
		{
			for (const TSharedPtr<FJsonValue>& FuncVal : *FunctionsArray)
			{
				const TSharedPtr<FJsonObject>* FuncObj = nullptr;
				if (!FuncVal->TryGetObject(FuncObj) || !FuncObj) continue;

				FString FuncName;
				(*FuncObj)->TryGetStringField(TEXT("name"), FuncName);
				if (FuncName.IsEmpty())
				{
					Warnings.Add(TEXT("Skipped function with empty name"));
					continue;
				}

				// Build function signature
				FOliveIRFunctionSignature Sig;
				Sig.Name = FuncName;

				const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
				if ((*FuncObj)->TryGetArrayField(TEXT("inputs"), InputsArray) && InputsArray)
				{
					for (const TSharedPtr<FJsonValue>& InVal : *InputsArray)
					{
						const TSharedPtr<FJsonObject>* InObj = nullptr;
						if (InVal->TryGetObject(InObj) && InObj)
						{
							Sig.Inputs.Add(ParseTemplateFuncParam(*InObj));
						}
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
				if ((*FuncObj)->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray)
				{
					for (const TSharedPtr<FJsonValue>& OutVal : *OutputsArray)
					{
						const TSharedPtr<FJsonObject>* OutObj = nullptr;
						if (OutVal->TryGetObject(OutObj) && OutObj)
						{
							FOliveIRFunctionParam OutParam = ParseTemplateFuncParam(*OutObj);
							OutParam.bIsOutParam = true;
							Sig.Outputs.Add(OutParam);
						}
					}
				}

				// Parse optional function metadata
				FString Category;
				if ((*FuncObj)->TryGetStringField(TEXT("category"), Category))
				{
					Sig.Category = Category;
				}

				FString Access;
				if ((*FuncObj)->TryGetStringField(TEXT("access"), Access))
				{
					if (Access.Equals(TEXT("private"), ESearchCase::IgnoreCase))
					{
						Sig.bIsPublic = false;
					}
				}

				bool bIsPure = false;
				if ((*FuncObj)->TryGetBoolField(TEXT("pure"), bIsPure))
				{
					Sig.bIsPure = bIsPure;
				}

				FString Desc;
				if ((*FuncObj)->TryGetStringField(TEXT("description"), Desc))
				{
					Sig.Description = Desc;
				}

				// Create the function graph
				FOliveBlueprintWriteResult FuncResult = Writer.AddFunction(AssetPath, Sig);
				if (!FuncResult.bSuccess)
				{
					FString Msg = FString::Printf(TEXT("Failed to create function '%s'"), *FuncName);
					if (FuncResult.Warnings.Num() > 0) Msg += TEXT(": ") + FuncResult.Warnings[0];
					Warnings.Add(Msg);
					continue;
				}

				CreatedFunctions.Add(FuncName);

				// Execute plan if present
				const TSharedPtr<FJsonObject>* PlanObj = nullptr;
				if ((*FuncObj)->TryGetObjectField(TEXT("plan"), PlanObj) && PlanObj)
				{
					// Parse the plan
					FOliveIRBlueprintPlan Plan = FOliveIRBlueprintPlan::FromJson(*PlanObj);

					// Resolve (build graph context for the function graph)
					FOliveGraphContext FuncContext = FOliveGraphContext::BuildFromBlueprint(Blueprint, FuncName);
					FOlivePlanResolveResult ResolveResult =
						FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint, FuncContext);

					if (!ResolveResult.bSuccess)
					{
						FString ErrMsg = FString::Printf(
							TEXT("Plan resolve failed for function '%s'"), *FuncName);
						for (const FOliveIRBlueprintPlanError& Err : ResolveResult.Errors)
						{
							ErrMsg += TEXT("\n  ") + Err.Message;
						}
						Warnings.Add(ErrMsg);

						// Track as a failed plan execution in function summaries
						FTemplateFunctionSummary FuncSummary;
						FuncSummary.Name = FuncName;
						FuncSummary.bPlanExecuted = true;
						FuncSummary.bPlanSucceeded = false;
						for (const FOliveIRBlueprintPlanError& Err : ResolveResult.Errors)
						{
							FuncSummary.PlanErrors.Add(Err.Message);
						}
						FunctionSummaries.Add(MoveTemp(FuncSummary));
						continue;
					}

					// Find the function graph
					UEdGraph* FuncGraph = nullptr;
					for (UEdGraph* Graph : Blueprint->FunctionGraphs)
					{
						if (Graph && Graph->GetFName() == FName(*FuncName))
						{
							FuncGraph = Graph;
							break;
						}
					}

					if (!FuncGraph)
					{
						Warnings.Add(FString::Printf(
							TEXT("Function graph '%s' not found after creation — plan skipped"), *FuncName));
						continue;
					}

					// Execute plan using the expanded plan (with all resolver
					// expansions: ExpandComponentRefs, ExpandPlanInputs, etc.)
					FOlivePlanExecutor PlanExecutor;
					FOliveIRBlueprintPlanResult PlanResult = PlanExecutor.Execute(
						ResolveResult.ExpandedPlan, ResolveResult.ResolvedSteps,
						Blueprint, FuncGraph, AssetPath, FuncName);

					if (!PlanResult.bSuccess)
					{
						FString ErrMsg = FString::Printf(
							TEXT("Plan execution partially failed for function '%s'"), *FuncName);
						for (const FOliveIRBlueprintPlanError& Err : PlanResult.Errors)
						{
							ErrMsg += TEXT("\n  ") + Err.Message;
						}
						Warnings.Add(ErrMsg);
					}

					// Collect function plan summary for result enrichment
					{
						FTemplateFunctionSummary FuncSummary;
						FuncSummary.Name = FuncName;
						FuncSummary.bPlanExecuted = true;
						FuncSummary.bPlanSucceeded = PlanResult.bSuccess || PlanResult.bPartial;
						FuncSummary.NodeCount = PlanResult.StepToNodeMap.Num();

						// Build step summaries from the expanded plan
						for (const FOliveIRBlueprintPlanStep& Step : ResolveResult.ExpandedPlan.Steps)
						{
							FString Summary = FString::Printf(TEXT("%s: %s"), *Step.StepId, *Step.Op);
							if (!Step.Target.IsEmpty())
							{
								Summary += TEXT(" ") + Step.Target;
							}
							FuncSummary.StepSummaries.Add(Summary);
						}

						// Capture errors if execution did not fully succeed
						if (!PlanResult.bSuccess)
						{
							for (const FOliveIRBlueprintPlanError& Err : PlanResult.Errors)
							{
								FuncSummary.PlanErrors.Add(Err.Message);
							}
						}

						FunctionSummaries.Add(MoveTemp(FuncSummary));
					}

					// Append resolve warnings
					for (const FString& W : ResolveResult.Warnings)
					{
						Warnings.Add(FString::Printf(TEXT("[%s resolve] %s"), *FuncName, *W));
					}
				}
				else
				{
					// Function created but has no plan -- track as empty stub
					FTemplateFunctionSummary FuncSummary;
					FuncSummary.Name = FuncName;
					FuncSummary.bPlanExecuted = false;
					FunctionSummaries.Add(MoveTemp(FuncSummary));
				}
			}
		}

		// 10b. Force skeleton recompile so newly created functions are discoverable
		// by subsequent event graph plans that may call them (e.g., "call Fire").
		// Without this, FindFunction() fails because the skeleton class hasn't been
		// rebuilt after add_function.
		if (CreatedFunctions.Num() > 0)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
		}

		// 11. Execute event graph plans
		const TArray<TSharedPtr<FJsonValue>>* EventGraphsArray = nullptr;
		if (SubstitutedBP->TryGetArrayField(TEXT("event_graphs"), EventGraphsArray) && EventGraphsArray)
		{
			for (const TSharedPtr<FJsonValue>& EGVal : *EventGraphsArray)
			{
				const TSharedPtr<FJsonObject>* EGObj = nullptr;
				if (!EGVal->TryGetObject(EGObj) || !EGObj) continue;

				FString EGName;
				(*EGObj)->TryGetStringField(TEXT("name"), EGName);
				if (EGName.IsEmpty())
				{
					EGName = TEXT("unnamed_event_graph_plan");
				}

				const TSharedPtr<FJsonObject>* PlanObj = nullptr;
				if (!(*EGObj)->TryGetObjectField(TEXT("plan"), PlanObj) || !PlanObj)
				{
					Warnings.Add(FString::Printf(
						TEXT("Event graph entry '%s' has no 'plan' object — skipped"), *EGName));
					continue;
				}

				// Parse the plan
				FOliveIRBlueprintPlan Plan = FOliveIRBlueprintPlan::FromJson(*PlanObj);

				// Resolve (build graph context for the EventGraph)
				FOliveGraphContext EGContext = FOliveGraphContext::BuildFromBlueprint(Blueprint, TEXT("EventGraph"));
				FOlivePlanResolveResult ResolveResult =
					FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint, EGContext);

				if (!ResolveResult.bSuccess)
				{
					FString ErrMsg = FString::Printf(
						TEXT("Plan resolve failed for event graph entry '%s'"), *EGName);
					for (const FOliveIRBlueprintPlanError& Err : ResolveResult.Errors)
					{
						ErrMsg += TEXT("\n  ") + Err.Message;
					}
					Warnings.Add(ErrMsg);

					// Track as a failed plan execution in event graph summaries
					FTemplateFunctionSummary EGSummary;
					EGSummary.Name = EGName;
					EGSummary.bPlanExecuted = true;
					EGSummary.bPlanSucceeded = false;
					for (const FOliveIRBlueprintPlanError& Err : ResolveResult.Errors)
					{
						EGSummary.PlanErrors.Add(Err.Message);
					}
					EventGraphSummaries.Add(MoveTemp(EGSummary));
					continue;
				}

				// Find the EventGraph (first UbergraphPage, or by name)
				UEdGraph* EventGraph = nullptr;
				for (UEdGraph* Graph : Blueprint->UbergraphPages)
				{
					if (Graph && Graph->GetFName() == FName(TEXT("EventGraph")))
					{
						EventGraph = Graph;
						break;
					}
				}
				// Fallback: use the first ubergraph page
				if (!EventGraph && Blueprint->UbergraphPages.Num() > 0)
				{
					EventGraph = Blueprint->UbergraphPages[0];
				}

				if (!EventGraph)
				{
					Warnings.Add(FString::Printf(
						TEXT("EventGraph not found for event graph entry '%s' — plan skipped"), *EGName));
					continue;
				}

				// Execute plan using the expanded plan (with all resolver
				// expansions: ExpandComponentRefs, ExpandPlanInputs, etc.)
				FOlivePlanExecutor PlanExecutor;
				FOliveIRBlueprintPlanResult PlanResult = PlanExecutor.Execute(
					ResolveResult.ExpandedPlan, ResolveResult.ResolvedSteps,
					Blueprint, EventGraph, AssetPath, TEXT("EventGraph"));

				if (!PlanResult.bSuccess)
				{
					FString ErrMsg = FString::Printf(
						TEXT("Plan execution partially failed for event graph entry '%s'"), *EGName);
					for (const FOliveIRBlueprintPlanError& Err : PlanResult.Errors)
					{
						ErrMsg += TEXT("\n  ") + Err.Message;
					}
					Warnings.Add(ErrMsg);
				}

				// Collect event graph plan summary for result enrichment
				{
					FTemplateFunctionSummary EGSummary;
					EGSummary.Name = EGName;
					EGSummary.bPlanExecuted = true;
					EGSummary.bPlanSucceeded = PlanResult.bSuccess || PlanResult.bPartial;
					EGSummary.NodeCount = PlanResult.StepToNodeMap.Num();

					// Build step summaries from the expanded plan
					for (const FOliveIRBlueprintPlanStep& Step : ResolveResult.ExpandedPlan.Steps)
					{
						FString Summary = FString::Printf(TEXT("%s: %s"), *Step.StepId, *Step.Op);
						if (!Step.Target.IsEmpty())
						{
							Summary += TEXT(" ") + Step.Target;
						}
						EGSummary.StepSummaries.Add(Summary);
					}

					// Capture errors if execution did not fully succeed
					if (!PlanResult.bSuccess)
					{
						for (const FOliveIRBlueprintPlanError& Err : PlanResult.Errors)
						{
							EGSummary.PlanErrors.Add(Err.Message);
						}
					}

					EventGraphSummaries.Add(MoveTemp(EGSummary));
				}

				// Append resolve warnings
				for (const FString& W : ResolveResult.Warnings)
				{
					Warnings.Add(FString::Printf(TEXT("[%s resolve] %s"), *EGName, *W));
				}
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	// 12. Compile
	FOliveIRCompileResult CompileResult = FOliveCompileManager::Get().Compile(Blueprint);
	if (!CompileResult.bSuccess)
	{
		for (const FOliveIRCompileError& Err : CompileResult.Errors)
		{
			Warnings.Add(FString::Printf(TEXT("[compile] %s"), *Err.Message));
		}
	}

	// 13. Build result
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("asset_path"), AssetPath);
	ResultData->SetStringField(TEXT("template_id"), TemplateId);
	if (!PresetName.IsEmpty())
	{
		ResultData->SetStringField(TEXT("preset"), PresetName);
	}
	ResultData->SetBoolField(TEXT("compiled"), CompileResult.bSuccess);

	// compile_result structure for self-correction compatibility
	{
		TSharedPtr<FJsonObject> CompileResultJson = MakeShared<FJsonObject>();
		CompileResultJson->SetBoolField(TEXT("success"), CompileResult.bSuccess);
		if (!CompileResult.bSuccess)
		{
			TArray<TSharedPtr<FJsonValue>> ErrorJsonArray;
			for (const FOliveIRCompileError& Err : CompileResult.Errors)
			{
				ErrorJsonArray.Add(MakeShared<FJsonValueString>(Err.Message));
			}
			CompileResultJson->SetArrayField(TEXT("errors"), ErrorJsonArray);
		}
		ResultData->SetObjectField(TEXT("compile_result"), CompileResultJson);
	}

	// Applied parameters
	TSharedPtr<FJsonObject> ParamsResult = MakeShared<FJsonObject>();
	for (const auto& Pair : MergedParams)
	{
		ParamsResult->SetStringField(Pair.Key, Pair.Value);
	}
	ResultData->SetObjectField(TEXT("applied_params"), ParamsResult);

	// Created items
	auto StringArrayToJson = [](const TArray<FString>& Arr) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : Arr)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	};

	ResultData->SetArrayField(TEXT("components"), StringArrayToJson(CreatedComponents));
	ResultData->SetArrayField(TEXT("variables"), StringArrayToJson(CreatedVariables));
	ResultData->SetArrayField(TEXT("event_dispatchers"), StringArrayToJson(CreatedDispatchers));
	ResultData->SetArrayField(TEXT("functions"), StringArrayToJson(CreatedFunctions));

	// Function details with graph logic summary (eliminates need for post-template reads)
	auto BuildDetailsArray = [&StringArrayToJson](const TArray<FTemplateFunctionSummary>& Summaries)
		-> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> DetailsArray;
		for (const FTemplateFunctionSummary& Summary : Summaries)
		{
			TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("name"), Summary.Name);
			Detail->SetBoolField(TEXT("has_graph_logic"), Summary.bPlanExecuted);

			if (Summary.bPlanExecuted)
			{
				Detail->SetNumberField(TEXT("node_count"), Summary.NodeCount);
				Detail->SetBoolField(TEXT("plan_succeeded"), Summary.bPlanSucceeded);

				// Step summaries (compact, one line per step)
				TArray<TSharedPtr<FJsonValue>> StepJsonArray;
				for (const FString& S : Summary.StepSummaries)
				{
					StepJsonArray.Add(MakeShared<FJsonValueString>(S));
				}
				Detail->SetArrayField(TEXT("plan_steps"), StepJsonArray);

				if (Summary.PlanErrors.Num() > 0)
				{
					Detail->SetArrayField(TEXT("plan_errors"), StringArrayToJson(Summary.PlanErrors));
				}
			}
			else
			{
				// Function was created but has no plan (empty function body -- just entry node)
				Detail->SetStringField(TEXT("note"), TEXT("Empty function body - needs plan_json"));
			}

			DetailsArray.Add(MakeShared<FJsonValueObject>(Detail));
		}
		return DetailsArray;
	};

	if (FunctionSummaries.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("function_details"), BuildDetailsArray(FunctionSummaries));
	}
	if (EventGraphSummaries.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("event_graph_details"), BuildDetailsArray(EventGraphSummaries));
	}

	// Warnings
	if (Warnings.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("warnings"), StringArrayToJson(Warnings));
	}

	UE_LOG(LogOliveTemplates, Log,
		TEXT("ApplyTemplate '%s' (preset=%s) to '%s': %d components, %d vars, %d dispatchers, %d funcs, %d warnings, compiled=%s"),
		*TemplateId, *PresetName, *AssetPath,
		CreatedComponents.Num(), CreatedVariables.Num(), CreatedDispatchers.Num(), CreatedFunctions.Num(),
		Warnings.Num(), CompileResult.bSuccess ? TEXT("true") : TEXT("false"));

	// Build message with explicit empty-function checklist for sequential processing.
	// Listing functions by name prevents batching — "3 funcs" invites batching,
	// "Fire (empty), Reload (empty)" creates a sequential checklist.
	{
		FString Message = FString::Printf(TEXT("Created '%s' from template '%s'"), *AssetPath, *TemplateId);
		if (!PresetName.IsEmpty())
		{
			Message += FString::Printf(TEXT(" (preset=%s)"), *PresetName);
		}

		TArray<FString> EmptyFunctions;
		for (const FTemplateFunctionSummary& Summary : FunctionSummaries)
		{
			if (!Summary.bPlanExecuted)
			{
				EmptyFunctions.Add(Summary.Name);
			}
		}

		if (EmptyFunctions.Num() > 0)
		{
			Message += TEXT(". Functions needing graph logic: ");
			for (int32 i = 0; i < EmptyFunctions.Num(); i++)
			{
				if (i > 0) { Message += TEXT(", "); }
				Message += EmptyFunctions[i] + TEXT(" (empty)");
			}
			Message += TEXT(". REQUIRED: For each empty function, call olive.get_recipe then "
				"blueprint.preview_plan_json + blueprint.apply_plan_json. "
				"Work ONE function at a time — do NOT batch all recipes before writing plans.");
		}

		ResultData->SetStringField(TEXT("message"), Message);
	}

	FOliveToolResult Result = FOliveToolResult::Success(ResultData);
	if (!CompileResult.bSuccess)
	{
		FOliveIRMessage CompileWarning;
		CompileWarning.Severity = EOliveIRSeverity::Warning;
		CompileWarning.Code = TEXT("COMPILE_FAILED");
		CompileWarning.Message = FString::Printf(
			TEXT("Template '%s' applied but Blueprint FAILED TO COMPILE. %d compile error(s). "
				 "Review errors and fix before proceeding."),
			*TemplateId, CompileResult.Errors.Num());
		Result.Messages.Add(MoveTemp(CompileWarning));
	}
	return Result;
}

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

	// Functions
	if (Info->Functions.Num() > 0)
	{
		Result += FString::Printf(TEXT("\nFunctions (%d):\n"), Info->Functions.Num());
		for (const FOliveLibraryFunctionSummary& Func : Info->Functions)
		{
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

	for (const auto& ProjectPair : ByProject)
	{
		// Count functions in this project
		int32 ProjectFunctions = 0;
		for (const FOliveLibraryTemplateInfo* T : ProjectPair.Value)
		{
			ProjectFunctions += T->Functions.Num();
		}

		Catalog += FString::Printf(TEXT("- %s: %d blueprints, %d functions\n"),
			*ProjectPair.Key, ProjectPair.Value.Num(), ProjectFunctions);

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

