// Copyright Bode Software. All Rights Reserved.

#include "MCP/OliveToolRegistry.h"
#include "Index/OliveProjectIndex.h"
#include "Services/OliveValidationEngine.h"
#include "Services/OliveErrorBuilder.h"
#include "Settings/OliveAISettings.h"
#include "Brain/OliveToolExecutionContext.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"

namespace
{
	TSharedPtr<FJsonObject> CloneParams(const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Cloned = MakeShared<FJsonObject>();
		if (Params.IsValid())
		{
			for (const auto& Pair : Params->Values)
			{
				Cloned->Values.Add(Pair.Key, Pair.Value);
			}
		}
		return Cloned;
	}

	/**
	 * Try to copy the value of AliasName to CanonicalName on Effective if the
	 * canonical field is missing/empty and the alias field has a non-empty value.
	 * Returns true if an alias was applied.
	 */
	bool TryApplyAlias(
		TSharedPtr<FJsonObject>& Effective,
		const FString& CanonicalName,
		const FString& AliasName,
		TArray<FString>& OutNormalizedFields)
	{
		FString ExistingValue;
		if (Effective->TryGetStringField(CanonicalName, ExistingValue) && !ExistingValue.IsEmpty())
		{
			return false; // canonical already present, nothing to do
		}

		FString AliasValue;
		if (Effective->TryGetStringField(AliasName, AliasValue) && !AliasValue.IsEmpty())
		{
			Effective->SetStringField(CanonicalName, AliasValue);
			OutNormalizedFields.Add(FString::Printf(TEXT("%s->%s"), *AliasName, *CanonicalName));
			return true;
		}
		return false;
	}

	/**
	 * Try to copy a JSON object or array field from AliasName to CanonicalName
	 * on Effective if the canonical field is missing. Works for plan_json<-plan etc.
	 */
	bool TryApplyFieldAlias(
		TSharedPtr<FJsonObject>& Effective,
		const FString& CanonicalName,
		const FString& AliasName,
		TArray<FString>& OutNormalizedFields)
	{
		if (Effective->HasField(CanonicalName))
		{
			return false;
		}

		if (Effective->HasField(AliasName))
		{
			Effective->SetField(CanonicalName, Effective->TryGetField(AliasName));
			OutNormalizedFields.Add(FString::Printf(TEXT("%s->%s"), *AliasName, *CanonicalName));
			return true;
		}
		return false;
	}

	/** Extract the tool family prefix (everything before the first dot). */
	FString GetToolFamily(const FString& ToolName)
	{
		int32 DotIndex;
		if (ToolName.FindChar(TEXT('.'), DotIndex))
		{
			return ToolName.Left(DotIndex);
		}
		return ToolName;
	}

	/** Normalize parameters for Blueprint tools. */
	void NormalizeBlueprintParams(
		const FString& ToolName,
		TSharedPtr<FJsonObject>& Effective,
		TArray<FString>& OutNormalizedFields)
	{
		// Path aliasing — canonicalize to "path"
		const TCHAR* CanonicalField = TEXT("path");

		FString CanonicalValue;
		const bool bHasCanonical = Effective->TryGetStringField(CanonicalField, CanonicalValue) && !CanonicalValue.IsEmpty();

		if (!bHasCanonical)
		{
			TryApplyAlias(Effective, CanonicalField, TEXT("asset_path"), OutNormalizedFields)
				|| TryApplyAlias(Effective, CanonicalField, TEXT("path"), OutNormalizedFields)
				|| TryApplyAlias(Effective, CanonicalField, TEXT("asset"), OutNormalizedFields)
				|| TryApplyAlias(Effective, CanonicalField, TEXT("blueprint"), OutNormalizedFields);
		}

		// Keep both common path keys in sync to avoid schema/handler drift between tools.
		FString PathValue;
		const bool bHasPath = Effective->TryGetStringField(TEXT("path"), PathValue) && !PathValue.IsEmpty();
		FString AssetPathValue;
		const bool bHasAssetPath = Effective->TryGetStringField(TEXT("asset_path"), AssetPathValue) && !AssetPathValue.IsEmpty();

		if (bHasPath && !bHasAssetPath)
		{
			Effective->SetStringField(TEXT("asset_path"), PathValue);
			OutNormalizedFields.Add(TEXT("path->asset_path"));
		}
		else if (!bHasPath && bHasAssetPath)
		{
			Effective->SetStringField(TEXT("path"), AssetPathValue);
			OutNormalizedFields.Add(TEXT("asset_path->path"));
		}

		// plan_json <- plan / steps (supports both object and array fields)
		TryApplyFieldAlias(Effective, TEXT("plan_json"), TEXT("plan"), OutNormalizedFields)
			|| TryApplyFieldAlias(Effective, TEXT("plan_json"), TEXT("steps"), OutNormalizedFields);

		// function_name <- name / function
		TryApplyAlias(Effective, TEXT("function_name"), TEXT("name"), OutNormalizedFields)
			|| TryApplyAlias(Effective, TEXT("function_name"), TEXT("function"), OutNormalizedFields);

		// parent_class <- parent / base_class
		TryApplyAlias(Effective, TEXT("parent_class"), TEXT("parent"), OutNormalizedFields)
			|| TryApplyAlias(Effective, TEXT("parent_class"), TEXT("base_class"), OutNormalizedFields);

		// template_id <- id
		TryApplyAlias(Effective, TEXT("template_id"), TEXT("id"), OutNormalizedFields);
	}

	/** Normalize parameters for BT / Blackboard tools. */
	void NormalizeBTParams(
		TSharedPtr<FJsonObject>& Effective,
		TArray<FString>& OutNormalizedFields)
	{
		// path <- asset_path / asset
		TryApplyAlias(Effective, TEXT("path"), TEXT("asset_path"), OutNormalizedFields)
			|| TryApplyAlias(Effective, TEXT("path"), TEXT("asset"), OutNormalizedFields);

		// key_type <- type
		TryApplyAlias(Effective, TEXT("key_type"), TEXT("type"), OutNormalizedFields);
	}

	/** Normalize parameters for PCG tools. */
	void NormalizePCGParams(
		TSharedPtr<FJsonObject>& Effective,
		TArray<FString>& OutNormalizedFields)
	{
		// path <- asset_path / asset
		TryApplyAlias(Effective, TEXT("path"), TEXT("asset_path"), OutNormalizedFields)
			|| TryApplyAlias(Effective, TEXT("path"), TEXT("asset"), OutNormalizedFields);

		// settings_class <- type / node_type / class
		TryApplyAlias(Effective, TEXT("settings_class"), TEXT("type"), OutNormalizedFields)
			|| TryApplyAlias(Effective, TEXT("settings_class"), TEXT("node_type"), OutNormalizedFields)
			|| TryApplyAlias(Effective, TEXT("settings_class"), TEXT("class"), OutNormalizedFields);
	}

	/** Normalize parameters for C++ tools. Tool-specific disambiguation for 'name'. */
	void NormalizeCppParams(
		const FString& ToolName,
		TSharedPtr<FJsonObject>& Effective,
		TArray<FString>& OutNormalizedFields)
	{
		// file_path <- path / file
		TryApplyAlias(Effective, TEXT("file_path"), TEXT("path"), OutNormalizedFields)
			|| TryApplyAlias(Effective, TEXT("file_path"), TEXT("file"), OutNormalizedFields);

		// module_name <- module
		TryApplyAlias(Effective, TEXT("module_name"), TEXT("module"), OutNormalizedFields);

		// anchor_text <- anchor / search_text
		TryApplyAlias(Effective, TEXT("anchor_text"), TEXT("anchor"), OutNormalizedFields)
			|| TryApplyAlias(Effective, TEXT("anchor_text"), TEXT("search_text"), OutNormalizedFields);

		// Tool-specific: 'name' can map to class_name, property_name, or function_name
		// depending on the specific tool.
		if (ToolName == TEXT("cpp.read_class") || ToolName == TEXT("cpp.list_blueprint_callable")
			|| ToolName == TEXT("cpp.list_overridable") || ToolName == TEXT("cpp.create_class"))
		{
			TryApplyAlias(Effective, TEXT("class_name"), TEXT("name"), OutNormalizedFields)
				|| TryApplyAlias(Effective, TEXT("class_name"), TEXT("class"), OutNormalizedFields);
		}
		else if (ToolName == TEXT("cpp.add_property"))
		{
			TryApplyAlias(Effective, TEXT("property_name"), TEXT("name"), OutNormalizedFields);
			TryApplyAlias(Effective, TEXT("property_type"), TEXT("type"), OutNormalizedFields);
		}
		else if (ToolName == TEXT("cpp.add_function"))
		{
			TryApplyAlias(Effective, TEXT("function_name"), TEXT("name"), OutNormalizedFields)
				|| TryApplyAlias(Effective, TEXT("function_name"), TEXT("function"), OutNormalizedFields);
		}
		else if (ToolName == TEXT("cpp.read_enum"))
		{
			TryApplyAlias(Effective, TEXT("enum_name"), TEXT("name"), OutNormalizedFields);
		}
		else if (ToolName == TEXT("cpp.read_struct"))
		{
			TryApplyAlias(Effective, TEXT("struct_name"), TEXT("name"), OutNormalizedFields);
		}
	}

	/** Normalize parameters for project.* tools. */
	void NormalizeProjectParams(
		TSharedPtr<FJsonObject>& Effective,
		TArray<FString>& OutNormalizedFields)
	{
		// asset_path <- path / asset
		TryApplyAlias(Effective, TEXT("asset_path"), TEXT("path"), OutNormalizedFields)
			|| TryApplyAlias(Effective, TEXT("asset_path"), TEXT("asset"), OutNormalizedFields);

		// paths <- assets / asset_paths (array fields)
		TryApplyFieldAlias(Effective, TEXT("paths"), TEXT("assets"), OutNormalizedFields)
			|| TryApplyFieldAlias(Effective, TEXT("paths"), TEXT("asset_paths"), OutNormalizedFields);
	}

	/**
	 * Universal parameter normalization. Runs before every ExecuteTool() call.
	 * Detects the tool family from the tool name and applies per-family alias maps.
	 */
	TSharedPtr<FJsonObject> NormalizeToolParams(
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutNormalizedFields)
	{
		if (!Params.IsValid())
		{
			return Params;
		}

		TSharedPtr<FJsonObject> Effective = CloneParams(Params);
		const FString Family = GetToolFamily(ToolName);

		if (Family == TEXT("blueprint"))
		{
			NormalizeBlueprintParams(ToolName, Effective, OutNormalizedFields);
		}
		else if (Family == TEXT("behaviortree") || Family == TEXT("blackboard") || Family == TEXT("bt"))
		{
			NormalizeBTParams(Effective, OutNormalizedFields);
		}
		else if (Family == TEXT("pcg"))
		{
			NormalizePCGParams(Effective, OutNormalizedFields);
		}
		else if (Family == TEXT("cpp"))
		{
			NormalizeCppParams(ToolName, Effective, OutNormalizedFields);
		}
		else if (Family == TEXT("project"))
		{
			NormalizeProjectParams(Effective, OutNormalizedFields);
		}

		return Effective;
	}

	// ==========================================
	// Tool Alias Map (Backward Compatibility)
	// ==========================================

	/**
	 * Returns the static tool alias map. Maps deprecated tool names to their
	 * new consolidated names with parameter transformations.
	 *
	 * Aliases are resolved in ExecuteTool() BEFORE tool lookup, so deprecated
	 * names transparently redirect to the new handler. This map is lazily
	 * initialized once and never modified after startup.
	 *
	 * See plans/ai-freedom-design.md section 2.4 for the full alias list.
	 */
	const TMap<FString, FOliveToolAlias>& GetToolAliases()
	{
		static TMap<FString, FOliveToolAlias> Aliases = []()
		{
			TMap<FString, FOliveToolAlias> Map;

			// ------------------------------------------------------------------
			// Blueprint read tools -> blueprint.read with section param
			// ------------------------------------------------------------------

			Map.Add(TEXT("blueprint.read_function"), {
				TEXT("blueprint.read"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("section"), TEXT("graph"));
					// Copy function_name -> graph_name if not already set
					FString FuncName;
					if (P->TryGetStringField(TEXT("function_name"), FuncName) && !FuncName.IsEmpty())
					{
						if (!P->HasField(TEXT("graph_name")) || P->GetStringField(TEXT("graph_name")).IsEmpty())
						{
							P->SetStringField(TEXT("graph_name"), FuncName);
						}
					}
				}
			});

			Map.Add(TEXT("blueprint.read_event_graph"), {
				TEXT("blueprint.read"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("section"), TEXT("graph"));
					// graph_name passes through if present
				}
			});

			Map.Add(TEXT("blueprint.read_variables"), {
				TEXT("blueprint.read"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("section"), TEXT("variables"));
				}
			});

			Map.Add(TEXT("blueprint.read_components"), {
				TEXT("blueprint.read"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("section"), TEXT("components"));
				}
			});

			Map.Add(TEXT("blueprint.read_hierarchy"), {
				TEXT("blueprint.read"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("section"), TEXT("hierarchy"));
				}
			});

			Map.Add(TEXT("blueprint.list_overridable_functions"), {
				TEXT("blueprint.read"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("section"), TEXT("overridable_functions"));
				}
			});

			// ------------------------------------------------------------------
			// Blueprint variable tools -> blueprint.add_variable (upsert)
			// ------------------------------------------------------------------

			Map.Add(TEXT("blueprint.modify_variable"), {
				TEXT("blueprint.add_variable"),
				nullptr // all params pass through (add_variable becomes upsert)
			});

			// ------------------------------------------------------------------
			// Blueprint function tools -> blueprint.add_function with function_type
			// ------------------------------------------------------------------

			Map.Add(TEXT("blueprint.override_function"), {
				TEXT("blueprint.add_function"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("function_type"), TEXT("override"));
				}
			});

			Map.Add(TEXT("blueprint.add_custom_event"), {
				TEXT("blueprint.add_function"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("function_type"), TEXT("custom_event"));
				}
			});

			Map.Add(TEXT("blueprint.add_event_dispatcher"), {
				TEXT("blueprint.add_function"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("function_type"), TEXT("event_dispatcher"));
				}
			});

			// ------------------------------------------------------------------
			// Behavior Tree tools -> behaviortree.add_node with node_kind
			// ------------------------------------------------------------------

			Map.Add(TEXT("behaviortree.add_composite"), {
				TEXT("behaviortree.add_node"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("node_kind"), TEXT("composite"));
				}
			});

			Map.Add(TEXT("behaviortree.add_task"), {
				TEXT("behaviortree.add_node"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("node_kind"), TEXT("task"));
				}
			});

			Map.Add(TEXT("behaviortree.add_decorator"), {
				TEXT("behaviortree.add_node"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("node_kind"), TEXT("decorator"));
				}
			});

			Map.Add(TEXT("behaviortree.add_service"), {
				TEXT("behaviortree.add_node"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("node_kind"), TEXT("service"));
				}
			});

			// ------------------------------------------------------------------
			// Blackboard tools -> blackboard.add_key (upsert)
			// ------------------------------------------------------------------

			Map.Add(TEXT("blackboard.modify_key"), {
				TEXT("blackboard.add_key"),
				nullptr // all params pass through
			});

			// ------------------------------------------------------------------
			// C++ tools -> cpp.read_class with include param
			// ------------------------------------------------------------------

			Map.Add(TEXT("cpp.list_blueprint_callable"), {
				TEXT("cpp.read_class"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("include"), TEXT("callable"));
				}
			});

			Map.Add(TEXT("cpp.list_overridable"), {
				TEXT("cpp.read_class"),
				[](TSharedPtr<FJsonObject>& P)
				{
					P->SetStringField(TEXT("include"), TEXT("overridable"));
				}
			});

			// ------------------------------------------------------------------
			// Project tools -> project.get_asset_info
			// ------------------------------------------------------------------

			Map.Add(TEXT("project.get_dependencies"), {
				TEXT("project.get_asset_info"),
				nullptr // all params pass through
			});

			Map.Add(TEXT("project.get_referencers"), {
				TEXT("project.get_asset_info"),
				nullptr // all params pass through
			});

			return Map;
		}();

		return Aliases;
	}
}

// ==========================================
// FOliveToolDefinition
// ==========================================

TSharedPtr<FJsonObject> FOliveToolDefinition::ToMCPJson() const
{
	// MCP protocol format (2024-11-05):
	// { "name": "...", "description": "...", "inputSchema": { "type": "object", "properties": {...} } }
	TSharedPtr<FJsonObject> ToolJson = MakeShared<FJsonObject>();

	ToolJson->SetStringField(TEXT("name"), Name);
	ToolJson->SetStringField(TEXT("description"), Description);

	if (InputSchema.IsValid())
	{
		ToolJson->SetObjectField(TEXT("inputSchema"), InputSchema);
	}
	else
	{
		// Empty schema
		TSharedPtr<FJsonObject> EmptySchema = MakeShared<FJsonObject>();
		EmptySchema->SetStringField(TEXT("type"), TEXT("object"));
		EmptySchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		ToolJson->SetObjectField(TEXT("inputSchema"), EmptySchema);
	}

	// Add annotations object if any guidance fields are set
	if (!UsageGuidance.IsEmpty() || !WhenToUse.IsEmpty())
	{
		TSharedPtr<FJsonObject> Annotations = MakeShared<FJsonObject>();
		if (!UsageGuidance.IsEmpty())
		{
			Annotations->SetStringField(TEXT("usage_guidance"), UsageGuidance);
		}
		if (!WhenToUse.IsEmpty())
		{
			Annotations->SetStringField(TEXT("when_to_use"), WhenToUse);
		}
		ToolJson->SetObjectField(TEXT("annotations"), Annotations);
	}

	return ToolJson;
}

// ==========================================
// FOliveToolResult
// ==========================================

FOliveToolResult FOliveToolResult::Success(const TSharedPtr<FJsonObject>& ResultData)
{
	FOliveToolResult Result;
	Result.bSuccess = true;
	Result.Data = ResultData;
	return Result;
}

FOliveToolResult FOliveToolResult::Error(const FString& Code, const FString& Message, const FString& Suggestion)
{
	FOliveToolResult Result;
	Result.bSuccess = false;

	FOliveIRMessage ErrorMsg;
	ErrorMsg.Severity = EOliveIRSeverity::Error;
	ErrorMsg.Code = Code;
	ErrorMsg.Message = Message;
	ErrorMsg.Suggestion = Suggestion;
	Result.Messages.Add(ErrorMsg);

	return Result;
}

FString FOliveToolResult::ToJsonString() const
{
	TSharedPtr<FJsonObject> Json = ToJson();
	if (!Json.IsValid())
	{
		return TEXT("{}");
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
	return OutputString;
}

TSharedPtr<FJsonObject> FOliveToolResult::ToJson() const
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), bSuccess);

	// Always include structured data when provided, even for failures.
	// Many tools (e.g. batch operations) return rich failure reports in Data.
	if (Data.IsValid())
	{
		Response->SetObjectField(TEXT("data"), Data);
	}

	if (!bSuccess)
	{
		// Include error information
		if (Messages.Num() > 0)
		{
			const FOliveIRMessage& FirstError = Messages[0];
			TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
			ErrorObj->SetStringField(TEXT("code"), FirstError.Code);
			ErrorObj->SetStringField(TEXT("message"), FirstError.Message);
			if (!FirstError.Suggestion.IsEmpty())
			{
				ErrorObj->SetStringField(TEXT("suggestion"), FirstError.Suggestion);
			}
			Response->SetObjectField(TEXT("error"), ErrorObj);
		}
	}

	if (ExecutionTimeMs > 0.0)
	{
		Response->SetNumberField(TEXT("execution_time_ms"), ExecutionTimeMs);
	}

	if (!NextStepGuidance.IsEmpty())
	{
		Response->SetStringField(TEXT("next_step"), NextStepGuidance);
	}

	return Response;
}

// ==========================================
// FOliveToolRegistry - Singleton
// ==========================================

FOliveToolRegistry& FOliveToolRegistry::Get()
{
	static FOliveToolRegistry Instance;
	return Instance;
}

// ==========================================
// Registration
// ==========================================

void FOliveToolRegistry::RegisterTool(
	const FString& Name,
	const FString& Description,
	const TSharedPtr<FJsonObject>& InputSchema,
	FOliveToolHandler Handler,
	const TArray<FString>& Tags,
	const FString& Category)
{
	FOliveToolDefinition Definition;
	Definition.Name = Name;
	Definition.Description = Description;
	Definition.InputSchema = InputSchema;
	Definition.Tags = Tags;
	Definition.Category = Category;

	// Extract category from name if not provided (e.g., "project.search" -> "project")
	if (Category.IsEmpty() && Name.Contains(TEXT(".")))
	{
		int32 DotIndex;
		if (Name.FindChar(TEXT('.'), DotIndex))
		{
			Definition.Category = Name.Left(DotIndex);
		}
	}

	RegisterTool(Definition, Handler);
}

void FOliveToolRegistry::RegisterTool(const FOliveToolDefinition& Definition, FOliveToolHandler Handler)
{
	FRWScopeLock WriteLock(ToolsLock, SLT_Write);

	// Remove existing if present (for hot reload)
	if (const FToolEntry* Existing = Tools.Find(Definition.Name))
	{
		const FString ExistingSanitized = SanitizeToolNameForMCP(Definition.Name);
		if (ExistingSanitized != Definition.Name)
		{
			SanitizedNameToCanonical.Remove(ExistingSanitized);
		}
		Tools.Remove(Definition.Name);
	}

	FToolEntry Entry;
	Entry.Definition = Definition;
	Entry.Handler = Handler;
	Tools.Add(Definition.Name, Entry);

	// Maintain the sanitized→canonical index so MCP callers (which can only send
	// ^[a-zA-Z0-9_-]{1,64}$ names) can reach dot-named tools by their underscore form.
	const FString Sanitized = SanitizeToolNameForMCP(Definition.Name);
	if (Sanitized != Definition.Name)
	{
		SanitizedNameToCanonical.Add(Sanitized, Definition.Name);
	}

	UE_LOG(LogOliveAI, Verbose, TEXT("Registered tool: %s (%s)"), *Definition.Name, *Definition.Category);
}

void FOliveToolRegistry::UnregisterTool(const FString& Name)
{
	FRWScopeLock WriteLock(ToolsLock, SLT_Write);
	Tools.Remove(Name);
	const FString Sanitized = SanitizeToolNameForMCP(Name);
	if (Sanitized != Name)
	{
		SanitizedNameToCanonical.Remove(Sanitized);
	}
	UE_LOG(LogOliveAI, Verbose, TEXT("Unregistered tool: %s"), *Name);
}

FString FOliveToolRegistry::SanitizeToolNameForMCP(const FString& ToolName)
{
	FString Out = ToolName;
	Out.ReplaceInline(TEXT("."), TEXT("_"));
	return Out;
}

bool FOliveToolRegistry::HasTool(const FString& Name) const
{
	FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);
	if (Tools.Contains(Name))
	{
		return true;
	}

	// Also check if the name is a known alias pointing to a registered tool
	const TMap<FString, FOliveToolAlias>& Aliases = GetToolAliases();
	const FOliveToolAlias* Alias = Aliases.Find(Name);
	if (Alias)
	{
		return Tools.Contains(Alias->NewToolName);
	}

	return false;
}

bool FOliveToolRegistry::IsToolAlias(const FString& Name) const
{
	const TMap<FString, FOliveToolAlias>& Aliases = GetToolAliases();
	return Aliases.Contains(Name);
}

bool FOliveToolRegistry::ResolveAlias(FString& InOutName, TSharedPtr<FJsonObject>& InOutParams) const
{
	const TMap<FString, FOliveToolAlias>& Aliases = GetToolAliases();
	const FOliveToolAlias* Alias = Aliases.Find(InOutName);
	if (!Alias)
	{
		return false;
	}

	const FString OldName = InOutName;
	InOutName = Alias->NewToolName;

	// Clone params before transforming to avoid mutating the caller's original
	if (InOutParams.IsValid())
	{
		InOutParams = CloneParams(InOutParams);
	}
	else
	{
		InOutParams = MakeShared<FJsonObject>();
	}

	// Apply parameter transformation if one is defined for this alias
	if (Alias->TransformParams)
	{
		Alias->TransformParams(InOutParams);
	}

	UE_LOG(LogOliveAI, Log, TEXT("Tool alias: '%s' -> '%s' (deprecated, use new name)"),
		*OldName, *InOutName);

	return true;
}

// ==========================================
// Query
// ==========================================

TArray<FOliveToolDefinition> FOliveToolRegistry::GetAllTools() const
{
	TArray<FOliveToolDefinition> Result;

	FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);
	for (const auto& Pair : Tools)
	{
		Result.Add(Pair.Value.Definition);
	}

	return Result;
}

TOptional<FOliveToolDefinition> FOliveToolRegistry::GetTool(const FString& Name) const
{
	FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);

	const FToolEntry* Entry = Tools.Find(Name);
	if (Entry)
	{
		return Entry->Definition;
	}

	return TOptional<FOliveToolDefinition>();
}

TArray<FOliveToolDefinition> FOliveToolRegistry::GetToolsForMode(EOliveChatMode Mode) const
{
	// Code and Plan modes return all tools.
	// Plan mode blocks writes at the pipeline level (Stage 2 mode gate), not here.
	// This lets the AI see write tool schemas for planning purposes.
	if (Mode != EOliveChatMode::Ask)
	{
		return GetAllTools();
	}

	// Ask mode: return only read/discovery tools.
	// Exclude tools with any write-family tag. Include everything else (safe default).
	// Write-family tags that cause exclusion:
	static const TSet<FString> ExcludeTags = {
		TEXT("write"), TEXT("danger"), TEXT("refactor"),
		TEXT("create"), TEXT("delete")
	};

	TArray<FOliveToolDefinition> Result;

	FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);
	for (const auto& Pair : Tools)
	{
		const FOliveToolDefinition& Definition = Pair.Value.Definition;

		bool bHasExcludeTag = false;
		for (const FString& Tag : Definition.Tags)
		{
			if (ExcludeTags.Contains(Tag))
			{
				bHasExcludeTag = true;
				break;
			}
		}

		if (!bHasExcludeTag)
		{
			Result.Add(Definition);
		}
	}

	return Result;
}

TArray<FOliveToolDefinition> FOliveToolRegistry::GetToolsByCategory(const FString& Category) const
{
	TArray<FOliveToolDefinition> Result;

	FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);
	for (const auto& Pair : Tools)
	{
		if (Pair.Value.Definition.Category.Equals(Category, ESearchCase::IgnoreCase))
		{
			Result.Add(Pair.Value.Definition);
		}
	}

	return Result;
}

TArray<FOliveToolDefinition> FOliveToolRegistry::GetToolsByTag(const FString& Tag) const
{
	TArray<FOliveToolDefinition> Result;

	FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);
	for (const auto& Pair : Tools)
	{
		if (Pair.Value.Definition.Tags.Contains(Tag))
		{
			Result.Add(Pair.Value.Definition);
		}
	}

	return Result;
}

int32 FOliveToolRegistry::GetToolCount() const
{
	FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);
	return Tools.Num();
}

// ==========================================
// Execution
// ==========================================

FOliveToolResult FOliveToolRegistry::ExecuteTool(const FString& Name, const TSharedPtr<FJsonObject>& Params)
{
	double StartTime = FPlatformTime::Seconds();

	// If the caller used the MCP-sanitized name (underscores replaced dots),
	// translate back to the canonical dotted name before anything else.
	// This lets MCP clients like Claude CLI — which can't handle dots in
	// tool names — invoke our dotted tools via their underscored alias.
	FString EffectiveName = Name;
	{
		FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);
		if (!Tools.Contains(EffectiveName))
		{
			if (const FString* Canonical = SanitizedNameToCanonical.Find(EffectiveName))
			{
				EffectiveName = *Canonical;
			}
		}
	}

	// Resolve tool aliases before anything else. This transforms both the
	// tool name and parameters so that downstream normalization, routing,
	// and dispatch all operate on the canonical tool name.
	TSharedPtr<FJsonObject> AliasedParams = Params;
	const bool bWasAliased = ResolveAlias(EffectiveName, AliasedParams);

	TArray<FString> NormalizedFields;
	TSharedPtr<FJsonObject> EffectiveParams = NormalizeToolParams(EffectiveName, AliasedParams, NormalizedFields);

	// Find tool
	FOliveToolHandler Handler;
	{
		FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);

		const FToolEntry* Entry = Tools.Find(EffectiveName);
		if (!Entry)
		{
			return FOliveToolResult::Error(
				FOliveErrorBuilder::ERR_TOOL_NOT_FOUND,
				FString::Printf(TEXT("Tool '%s' not found"), *EffectiveName),
				TEXT("Use tools/list to see available tools.")
			);
		}

		Handler = Entry->Handler;
	}

	// Validate with validation engine
	FOliveValidationResult ValidationResult = FOliveValidationEngine::Get().ValidateOperation(EffectiveName, EffectiveParams, nullptr);
	if (ValidationResult.HasErrors())
	{
		TArray<FOliveIRMessage> Errors = ValidationResult.GetErrors();
		if (Errors.Num() > 0)
		{
			return FOliveToolResult::Error(
				Errors[0].Code,
				Errors[0].Message,
				Errors[0].Suggestion
			);
		}
		return FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_INVALID_PARAMS,
			TEXT("Validation failed"),
			TEXT("Check the tool parameters.")
		);
	}

	// Execute handler
	FOliveToolResult Result;
	if (Handler.IsBound())
	{
		Result = Handler.Execute(EffectiveParams);
	}
	else
	{
		Result = FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_INTERNAL,
			TEXT("Tool handler not bound"),
			TEXT("This is a bug. Please report it.")
		);
	}

	// Add execution time
	double EndTime = FPlatformTime::Seconds();
	Result.ExecutionTimeMs = (EndTime - StartTime) * 1000.0;

	UE_LOG(LogOliveAI, Log, TEXT("Tool '%s' executed in %.2fms - %s"),
		*EffectiveName, Result.ExecutionTimeMs, Result.bSuccess ? TEXT("success") : TEXT("failed"));

	if (NormalizedFields.Num() > 0 || bWasAliased)
	{
		if (!Result.Data.IsValid())
		{
			Result.Data = MakeShared<FJsonObject>();
		}

		if (NormalizedFields.Num() > 0)
		{
			Result.Data->SetBoolField(TEXT("normalized_params"), true);
			TArray<TSharedPtr<FJsonValue>> Fields;
			for (const FString& Field : NormalizedFields)
			{
				Fields.Add(MakeShared<FJsonValueString>(Field));
			}
			Result.Data->SetArrayField(TEXT("normalized_fields"), Fields);
		}

		if (bWasAliased)
		{
			Result.Data->SetStringField(TEXT("aliased_from"), Name);
			Result.Data->SetStringField(TEXT("aliased_to"), EffectiveName);
		}
	}

	return Result;
}

void FOliveToolRegistry::ExecuteToolAsync(
	const FString& Name,
	const TSharedPtr<FJsonObject>& Params,
	TFunction<void(FOliveToolResult)> Callback)
{
	// Dispatch to game thread for UE API safety
	AsyncTask(ENamedThreads::GameThread, [this, Name, Params, Callback]()
	{
		FOliveToolResult Result = ExecuteTool(Name, Params);
		if (Callback)
		{
			Callback(Result);
		}
	});
}

// ==========================================
// MCP Format
// ==========================================

TSharedPtr<FJsonObject> FOliveToolRegistry::GetToolsListMCP() const
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> ToolsArray;

	// MCP always returns all tools -- external agents are always Code mode.
	TArray<FOliveToolDefinition> ToolsList = GetAllTools();

	for (const FOliveToolDefinition& Tool : ToolsList)
	{
		TSharedPtr<FJsonObject> ToolJson = Tool.ToMCPJson();

		// Sanitize the name for the MCP wire format. MCP spec requires tool names
		// to match ^[a-zA-Z0-9_-]{1,64}$ (dots not allowed), and Claude CLI silently
		// drops tools whose names violate this. We keep dotted names internally
		// (better logs / error messages / knowledge-pack references) but expose
		// underscored names on the wire. ExecuteTool translates back via
		// SanitizedNameToCanonical.
		if (ToolJson.IsValid())
		{
			FString WireName;
			if (ToolJson->TryGetStringField(TEXT("name"), WireName))
			{
				const FString Sanitized = SanitizeToolNameForMCP(WireName);
				if (Sanitized != WireName)
				{
					ToolJson->SetStringField(TEXT("name"), Sanitized);
				}
			}
		}

		ToolsArray.Add(MakeShared<FJsonValueObject>(ToolJson));
	}

	Response->SetArrayField(TEXT("tools"), ToolsArray);

	return Response;
}

// ==========================================
// Lifecycle
// ==========================================

void FOliveToolRegistry::RegisterBuiltInTools()
{
	RegisterProjectTools();
	RegisterBatchTool();

	UE_LOG(LogOliveAI, Log, TEXT("Registered %d built-in tools"), GetToolCount());
}

void FOliveToolRegistry::ClearAllTools()
{
	FRWScopeLock WriteLock(ToolsLock, SLT_Write);
	Tools.Empty();
}

// ==========================================
// Built-in Tool Registration
// ==========================================

void FOliveToolRegistry::RegisterProjectTools()
{
	// project.search
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> QueryProp = MakeShared<FJsonObject>();
		QueryProp->SetStringField(TEXT("type"), TEXT("string"));
		QueryProp->SetStringField(TEXT("description"), TEXT("Search query (asset name or partial match)"));
		Properties->SetObjectField(TEXT("query"), QueryProp);

		TSharedPtr<FJsonObject> MaxResultsProp = MakeShared<FJsonObject>();
		MaxResultsProp->SetStringField(TEXT("type"), TEXT("integer"));
		MaxResultsProp->SetStringField(TEXT("description"), TEXT("Maximum results to return (default: 50)"));
		Properties->SetObjectField(TEXT("max_results"), MaxResultsProp);

		Schema->SetObjectField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("query")));
		Schema->SetArrayField(TEXT("required"), Required);

		RegisterTool(
			TEXT("project.search"),
			TEXT("Search for assets in the project by name. Returns matching assets with their paths and types."),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleProjectSearch),
			{ TEXT("project"), TEXT("search") },
			TEXT("project")
		);
	}

	// project.get_asset_info
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
		PathProp->SetStringField(TEXT("type"), TEXT("string"));
		PathProp->SetStringField(TEXT("description"), TEXT("Asset path (e.g., /Game/Blueprints/BP_Player)"));
		Properties->SetObjectField(TEXT("path"), PathProp);

		Schema->SetObjectField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("path")));
		Schema->SetArrayField(TEXT("required"), Required);

		RegisterTool(
			TEXT("project.get_asset_info"),
			TEXT("Get detailed information about an asset including dependencies, referencers, and metadata."),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleProjectGetAssetInfo),
			{ TEXT("project"), TEXT("info") },
			TEXT("project")
		);
	}

	// project.get_class_hierarchy
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> RootClassProp = MakeShared<FJsonObject>();
		RootClassProp->SetStringField(TEXT("type"), TEXT("string"));
		RootClassProp->SetStringField(TEXT("description"), TEXT("Root class name (optional, defaults to Actor)"));
		Properties->SetObjectField(TEXT("root_class"), RootClassProp);

		Schema->SetObjectField(TEXT("properties"), Properties);

		RegisterTool(
			TEXT("project.get_class_hierarchy"),
			TEXT("Get the class inheritance hierarchy starting from a root class."),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleProjectGetClassHierarchy),
			{ TEXT("project"), TEXT("hierarchy") },
			TEXT("project")
		);
	}

	// Removed in AI Freedom Phase 2 — already returned by project.get_asset_info
	// project.get_dependencies
	// {
	// 	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	// 	Schema->SetStringField(TEXT("type"), TEXT("object"));
	//
	// 	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	//
	// 	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	// 	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	// 	PathProp->SetStringField(TEXT("description"), TEXT("Asset path to get dependencies for"));
	// 	Properties->SetObjectField(TEXT("path"), PathProp);
	//
	// 	Schema->SetObjectField(TEXT("properties"), Properties);
	//
	// 	TArray<TSharedPtr<FJsonValue>> Required;
	// 	Required.Add(MakeShared<FJsonValueString>(TEXT("path")));
	// 	Schema->SetArrayField(TEXT("required"), Required);
	//
	// 	RegisterTool(
	// 		TEXT("project.get_dependencies"),
	// 		TEXT("Get all assets that the specified asset depends on."),
	// 		Schema,
	// 		FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleProjectGetDependencies),
	// 		{ TEXT("project"), TEXT("dependencies") },
	// 		TEXT("project")
	// 	);
	// }

	// Removed in AI Freedom Phase 2 — already returned by project.get_asset_info
	// project.get_referencers
	// {
	// 	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	// 	Schema->SetStringField(TEXT("type"), TEXT("object"));
	//
	// 	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	//
	// 	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	// 	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	// 	PathProp->SetStringField(TEXT("description"), TEXT("Asset path to get referencers for"));
	// 	Properties->SetObjectField(TEXT("path"), PathProp);
	//
	// 	Schema->SetObjectField(TEXT("properties"), Properties);
	//
	// 	TArray<TSharedPtr<FJsonValue>> Required;
	// 	Required.Add(MakeShared<FJsonValueString>(TEXT("path")));
	// 	Schema->SetArrayField(TEXT("required"), Required);
	//
	// 	RegisterTool(
	// 		TEXT("project.get_referencers"),
	// 		TEXT("Get all assets that reference the specified asset."),
	// 		Schema,
	// 		FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleProjectGetReferencers),
	// 		{ TEXT("project"), TEXT("referencers") },
	// 		TEXT("project")
	// 	);
	// }

	// Removed in AI Freedom Phase 2 — in system prompt; rarely used by AI
	// project.get_config
	// {
	// 	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	// 	Schema->SetStringField(TEXT("type"), TEXT("object"));
	// 	Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	//
	// 	RegisterTool(
	// 		TEXT("project.get_config"),
	// 		TEXT("Get project configuration including engine version, enabled plugins, and primary asset types."),
	// 		Schema,
	// 		FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleProjectGetConfig),
	// 		{ TEXT("project"), TEXT("config") },
	// 		TEXT("project")
	// 	);
	// }
}


// ==========================================
// Tool Handlers - Project
// ==========================================

FOliveToolResult FOliveToolRegistry::HandleProjectSearch(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_INVALID_PARAMS,
			TEXT("Parameters required"),
			TEXT("Provide a 'query' parameter.")
		);
	}

	FString Query = Params->GetStringField(TEXT("query"));
	if (Query.IsEmpty())
	{
		return FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_INVALID_PARAMS,
			TEXT("Query cannot be empty"),
			TEXT("Provide a non-empty search query.")
		);
	}

	int32 MaxResults = 50;
	if (Params->HasField(TEXT("max_results")))
	{
		MaxResults = FMath::Clamp(Params->GetIntegerField(TEXT("max_results")), 1, 200);
	}

	// Use Project Index
	TArray<FOliveAssetInfo> Results = FOliveProjectIndex::Get().SearchAssets(Query, MaxResults);

	// Build response
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> AssetsArray;
	for (const FOliveAssetInfo& Asset : Results)
	{
		TSharedPtr<FJsonObject> AssetJson = Asset.ToJson();
		AssetsArray.Add(MakeShared<FJsonValueObject>(AssetJson));
	}

	Data->SetArrayField(TEXT("results"), AssetsArray);
	Data->SetNumberField(TEXT("count"), Results.Num());
	Data->SetStringField(TEXT("query"), Query);

	return FOliveToolResult::Success(Data);
}

FOliveToolResult FOliveToolRegistry::HandleProjectGetAssetInfo(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_INVALID_PARAMS,
			TEXT("Parameters required"),
			TEXT("Provide a 'path' parameter.")
		);
	}

	FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		return FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_INVALID_PARAMS,
			TEXT("Path cannot be empty"),
			TEXT("Provide an asset path.")
		);
	}

	TOptional<FOliveAssetInfo> AssetInfo = FOliveProjectIndex::Get().GetAssetByPath(Path);
	if (!AssetInfo.IsSet())
	{
		return FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_NOT_FOUND,
			FString::Printf(TEXT("Asset not found: %s"), *Path),
			TEXT("Use project.search to find the correct asset path.")
		);
	}

	return FOliveToolResult::Success(AssetInfo->ToJson());
}

FOliveToolResult FOliveToolRegistry::HandleProjectGetClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	FName RootClass = NAME_None;

	if (Params.IsValid() && Params->HasField(TEXT("root_class")))
	{
		RootClass = FName(*Params->GetStringField(TEXT("root_class")));
	}

	FString HierarchyJson = FOliveProjectIndex::Get().GetClassHierarchyJson(RootClass);

	// Parse the JSON string back to object
	TSharedPtr<FJsonObject> Data;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HierarchyJson);
	if (FJsonSerializer::Deserialize(Reader, Data))
	{
		return FOliveToolResult::Success(Data);
	}

	// Fallback
	TSharedPtr<FJsonObject> EmptyData = MakeShared<FJsonObject>();
	EmptyData->SetArrayField(TEXT("hierarchy"), TArray<TSharedPtr<FJsonValue>>());
	return FOliveToolResult::Success(EmptyData);
}

FOliveToolResult FOliveToolRegistry::HandleProjectGetDependencies(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_INVALID_PARAMS,
			TEXT("Parameters required"),
			TEXT("Provide a 'path' parameter.")
		);
	}

	FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		return FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_INVALID_PARAMS,
			TEXT("Path cannot be empty"),
			TEXT("Provide an asset path.")
		);
	}

	TArray<FString> Dependencies = FOliveProjectIndex::Get().GetDependencies(Path);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> DepsArray;
	for (const FString& Dep : Dependencies)
	{
		DepsArray.Add(MakeShared<FJsonValueString>(Dep));
	}

	Data->SetArrayField(TEXT("dependencies"), DepsArray);
	Data->SetStringField(TEXT("asset"), Path);
	Data->SetNumberField(TEXT("count"), Dependencies.Num());

	return FOliveToolResult::Success(Data);
}

FOliveToolResult FOliveToolRegistry::HandleProjectGetReferencers(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_INVALID_PARAMS,
			TEXT("Parameters required"),
			TEXT("Provide a 'path' parameter.")
		);
	}

	FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		return FOliveToolResult::Error(
			FOliveErrorBuilder::ERR_INVALID_PARAMS,
			TEXT("Path cannot be empty"),
			TEXT("Provide an asset path.")
		);
	}

	TArray<FString> Referencers = FOliveProjectIndex::Get().GetReferencers(Path);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> RefsArray;
	for (const FString& Ref : Referencers)
	{
		RefsArray.Add(MakeShared<FJsonValueString>(Ref));
	}

	Data->SetArrayField(TEXT("referencers"), RefsArray);
	Data->SetStringField(TEXT("asset"), Path);
	Data->SetNumberField(TEXT("count"), Referencers.Num());

	return FOliveToolResult::Success(Data);
}

FOliveToolResult FOliveToolRegistry::HandleProjectGetConfig(const TSharedPtr<FJsonObject>& Params)
{
	FString ConfigJson = FOliveProjectIndex::Get().GetProjectConfigJson();

	// Parse the JSON string back to object
	TSharedPtr<FJsonObject> Data;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ConfigJson);
	if (FJsonSerializer::Deserialize(Reader, Data))
	{
		return FOliveToolResult::Success(Data);
	}

	// Fallback
	TSharedPtr<FJsonObject> EmptyData = MakeShared<FJsonObject>();
	return FOliveToolResult::Success(EmptyData);
}

// ==========================================
// olive.batch — multi-step tool dispatcher
// ==========================================

namespace OliveBatchInternal
{
	/**
	 * Follow a dotted path ("node_id", "data.node_id", "content.0.text") through
	 * a JSON value and return the final value as a string, or empty string if
	 * the path cannot be resolved.
	 */
	static FString ResolveDottedPath(const TSharedPtr<FJsonObject>& Root, const FString& Path)
	{
		if (!Root.IsValid() || Path.IsEmpty())
		{
			return FString();
		}

		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."), true);
		if (Parts.Num() == 0)
		{
			return FString();
		}

		TSharedPtr<FJsonValue> Current = MakeShared<FJsonValueObject>(Root);
		for (const FString& Part : Parts)
		{
			if (!Current.IsValid())
			{
				return FString();
			}

			if (Current->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject>& Obj = Current->AsObject();
				if (!Obj.IsValid() || !Obj->HasField(Part))
				{
					return FString();
				}
				Current = Obj->TryGetField(Part);
			}
			else if (Current->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Arr = Current->AsArray();
				int32 Index = FCString::Atoi(*Part);
				if (Index < 0 || Index >= Arr.Num())
				{
					return FString();
				}
				Current = Arr[Index];
			}
			else
			{
				return FString();
			}
		}

		if (!Current.IsValid())
		{
			return FString();
		}

		// Coerce the final value to string
		switch (Current->Type)
		{
		case EJson::String: return Current->AsString();
		case EJson::Number: return FString::SanitizeFloat(Current->AsNumber());
		case EJson::Boolean: return Current->AsBool() ? TEXT("true") : TEXT("false");
		default: return FString();
		}
	}

	/**
	 * Substitute any "@alias.path" tokens in a string with values pulled from
	 * prior step results. Accepts paths with dot-separated fields.
	 * Examples: "@entry.node_id", "@spawn.data.node_id"
	 */
	static FString SubstituteStringRefs(const FString& Input, const TMap<FString, TSharedPtr<FJsonObject>>& Aliases)
	{
		if (Input.IsEmpty() || !Input.Contains(TEXT("@")))
		{
			return Input;
		}

		FString Out;
		Out.Reserve(Input.Len());

		for (int32 i = 0; i < Input.Len(); )
		{
			const TCHAR Ch = Input[i];
			if (Ch != TEXT('@'))
			{
				Out.AppendChar(Ch);
				++i;
				continue;
			}

			// Parse @alias.path token. Alias + path characters: letters, digits, _, .
			int32 TokenEnd = i + 1;
			while (TokenEnd < Input.Len())
			{
				const TCHAR Tc = Input[TokenEnd];
				const bool bIsAlnum = (Tc >= TEXT('A') && Tc <= TEXT('Z')) || (Tc >= TEXT('a') && Tc <= TEXT('z')) || (Tc >= TEXT('0') && Tc <= TEXT('9'));
				if (bIsAlnum || Tc == TEXT('_') || Tc == TEXT('.'))
				{
					++TokenEnd;
					continue;
				}
				break;
			}

			// Token is Input[i+1 .. TokenEnd)
			const FString RawToken = Input.Mid(i + 1, TokenEnd - (i + 1));
			int32 DotIdx = INDEX_NONE;
			RawToken.FindChar(TEXT('.'), DotIdx);

			const FString Alias = (DotIdx == INDEX_NONE) ? RawToken : RawToken.Left(DotIdx);
			const FString Path = (DotIdx == INDEX_NONE) ? FString(TEXT("node_id")) : RawToken.Mid(DotIdx + 1);

			const TSharedPtr<FJsonObject>* AliasData = Aliases.Find(Alias);
			if (AliasData && AliasData->IsValid() && !Alias.IsEmpty())
			{
				const FString Resolved = ResolveDottedPath(*AliasData, Path);
				if (!Resolved.IsEmpty())
				{
					Out += Resolved;
					i = TokenEnd;
					continue;
				}
			}

			// Couldn't resolve — leave the token as-is so the error trail is visible.
			Out += Input.Mid(i, TokenEnd - i);
			i = TokenEnd;
		}

		return Out;
	}

	/** Recursively walk a JSON value, substituting @alias.path tokens in every string. */
	static TSharedPtr<FJsonValue> SubstituteRefsInValue(const TSharedPtr<FJsonValue>& Value, const TMap<FString, TSharedPtr<FJsonObject>>& Aliases);

	static TSharedPtr<FJsonObject> SubstituteRefsInObject(const TSharedPtr<FJsonObject>& In, const TMap<FString, TSharedPtr<FJsonObject>>& Aliases)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!In.IsValid()) return Out;
		for (const auto& Pair : In->Values)
		{
			Out->SetField(Pair.Key, SubstituteRefsInValue(Pair.Value, Aliases));
		}
		return Out;
	}

	static TSharedPtr<FJsonValue> SubstituteRefsInValue(const TSharedPtr<FJsonValue>& Value, const TMap<FString, TSharedPtr<FJsonObject>>& Aliases)
	{
		if (!Value.IsValid()) return nullptr;

		switch (Value->Type)
		{
		case EJson::String:
			return MakeShared<FJsonValueString>(SubstituteStringRefs(Value->AsString(), Aliases));
		case EJson::Object:
			return MakeShared<FJsonValueObject>(SubstituteRefsInObject(Value->AsObject(), Aliases));
		case EJson::Array:
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				Out.Add(SubstituteRefsInValue(Item, Aliases));
			}
			return MakeShared<FJsonValueArray>(Out);
		}
		default:
			return Value;
		}
	}
}

void FOliveToolRegistry::RegisterBatchTool()
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// steps
	{
		TSharedPtr<FJsonObject> StepsProp = MakeShared<FJsonObject>();
		StepsProp->SetStringField(TEXT("type"), TEXT("array"));
		StepsProp->SetStringField(TEXT("description"),
			TEXT("Ordered list of tool calls to execute in one round-trip. Each step: "
				 "{tool: string, params: object, id?: string}. Use id to alias the step's "
				 "result so later steps can reference it via \"@id\" (shorthand for "
				 "@id.node_id) or \"@id.path.to.field\" anywhere in string params. "
				 "All steps share one transaction: undo reverses the whole batch. "
				 "Stops on first failure unless stop_on_error=false."));

		TSharedPtr<FJsonObject> ItemSchema = MakeShared<FJsonObject>();
		ItemSchema->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> ItemProps = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> ToolProp = MakeShared<FJsonObject>();
		ToolProp->SetStringField(TEXT("type"), TEXT("string"));
		ToolProp->SetStringField(TEXT("description"), TEXT("Tool name to invoke (e.g., blueprint.add_node)"));
		ItemProps->SetObjectField(TEXT("tool"), ToolProp);

		TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
		ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
		ParamsProp->SetStringField(TEXT("description"), TEXT("Parameters for the tool call. Strings support @alias.path refs to prior step results."));
		ItemProps->SetObjectField(TEXT("params"), ParamsProp);

		TSharedPtr<FJsonObject> IdProp = MakeShared<FJsonObject>();
		IdProp->SetStringField(TEXT("type"), TEXT("string"));
		IdProp->SetStringField(TEXT("description"), TEXT("Optional alias. Following steps can reference this step's result with @id or @id.field."));
		ItemProps->SetObjectField(TEXT("id"), IdProp);

		ItemSchema->SetObjectField(TEXT("properties"), ItemProps);
		TArray<TSharedPtr<FJsonValue>> ItemRequired;
		ItemRequired.Add(MakeShared<FJsonValueString>(TEXT("tool")));
		ItemRequired.Add(MakeShared<FJsonValueString>(TEXT("params")));
		ItemSchema->SetArrayField(TEXT("required"), ItemRequired);

		StepsProp->SetObjectField(TEXT("items"), ItemSchema);
		Properties->SetObjectField(TEXT("steps"), StepsProp);
	}

	// stop_on_error
	{
		TSharedPtr<FJsonObject> StopProp = MakeShared<FJsonObject>();
		StopProp->SetStringField(TEXT("type"), TEXT("boolean"));
		StopProp->SetStringField(TEXT("description"), TEXT("If true (default), stop at the first failing step. If false, run all steps regardless."));
		Properties->SetObjectField(TEXT("stop_on_error"), StopProp);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("steps")));
	Schema->SetArrayField(TEXT("required"), Required);

	FOliveToolDefinition Def;
	Def.Name = TEXT("olive.batch");
	Def.Description = TEXT("Execute a sequence of primitive tool calls in one round-trip. Reduces HTTP overhead when a task needs many related calls (e.g. build a function: add_node x N + connect_pins x M + set_pin_default x K + compile). Use @alias refs to chain outputs.");
	Def.InputSchema = Schema;
	Def.Tags = { TEXT("meta"), TEXT("batch") };
	Def.Category = TEXT("meta");
	Def.WhenToUse = TEXT("Use whenever you have 3+ related tool calls. Set id on steps whose node_id / asset_path you need later, then reference with @id or @id.field in subsequent params.");

	RegisterTool(Def, FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleOliveBatch));
}

FOliveToolResult FOliveToolRegistry::HandleOliveBatch(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a steps array."));
	}

	const TArray<TSharedPtr<FJsonValue>>* StepsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("steps"), StepsArr) || !StepsArr)
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing or non-array 'steps'"),
			TEXT("Provide steps: [{tool, params, id?}, ...]"));
	}
	if (StepsArr->Num() == 0)
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_EMPTY"),
			TEXT("steps array is empty"),
			TEXT("Provide at least one step."));
	}

	bool bStopOnError = true;
	Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

	TMap<FString, TSharedPtr<FJsonObject>> Aliases;
	TArray<TSharedPtr<FJsonValue>> Results;
	int32 SucceededCount = 0;
	int32 FailedCount = 0;
	int32 FirstFailedIndex = INDEX_NONE;

	for (int32 i = 0; i < StepsArr->Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& StepVal = (*StepsArr)[i];
		const TSharedPtr<FJsonObject> StepObj = StepVal.IsValid() ? StepVal->AsObject() : nullptr;
		if (!StepObj.IsValid())
		{
			TSharedPtr<FJsonObject> ErrRes = MakeShared<FJsonObject>();
			ErrRes->SetNumberField(TEXT("index"), i);
			ErrRes->SetBoolField(TEXT("success"), false);
			ErrRes->SetStringField(TEXT("error_code"), TEXT("VALIDATION_BAD_STEP"));
			ErrRes->SetStringField(TEXT("error_message"), TEXT("Step is not a JSON object"));
			Results.Add(MakeShared<FJsonValueObject>(ErrRes));
			FailedCount++;
			if (FirstFailedIndex == INDEX_NONE) FirstFailedIndex = i;
			if (bStopOnError) break;
			continue;
		}

		FString ToolName;
		if (!StepObj->TryGetStringField(TEXT("tool"), ToolName) || ToolName.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrRes = MakeShared<FJsonObject>();
			ErrRes->SetNumberField(TEXT("index"), i);
			ErrRes->SetBoolField(TEXT("success"), false);
			ErrRes->SetStringField(TEXT("error_code"), TEXT("VALIDATION_MISSING_PARAM"));
			ErrRes->SetStringField(TEXT("error_message"), TEXT("Step is missing 'tool'"));
			Results.Add(MakeShared<FJsonValueObject>(ErrRes));
			FailedCount++;
			if (FirstFailedIndex == INDEX_NONE) FirstFailedIndex = i;
			if (bStopOnError) break;
			continue;
		}

		if (ToolName == TEXT("olive.batch"))
		{
			TSharedPtr<FJsonObject> ErrRes = MakeShared<FJsonObject>();
			ErrRes->SetNumberField(TEXT("index"), i);
			ErrRes->SetBoolField(TEXT("success"), false);
			ErrRes->SetStringField(TEXT("error_code"), TEXT("BATCH_RECURSION"));
			ErrRes->SetStringField(TEXT("error_message"), TEXT("olive.batch cannot invoke olive.batch"));
			ErrRes->SetStringField(TEXT("suggestion"), TEXT("Flatten nested batches into a single steps array."));
			Results.Add(MakeShared<FJsonValueObject>(ErrRes));
			FailedCount++;
			if (FirstFailedIndex == INDEX_NONE) FirstFailedIndex = i;
			if (bStopOnError) break;
			continue;
		}

		const TSharedPtr<FJsonObject>* StepParamsPtr = nullptr;
		TSharedPtr<FJsonObject> StepParams;
		if (StepObj->TryGetObjectField(TEXT("params"), StepParamsPtr) && StepParamsPtr && StepParamsPtr->IsValid())
		{
			StepParams = OliveBatchInternal::SubstituteRefsInObject(*StepParamsPtr, Aliases);
		}
		else
		{
			StepParams = MakeShared<FJsonObject>();
		}

		FString StepId;
		StepObj->TryGetStringField(TEXT("id"), StepId);

		// Dispatch. ExecuteTool routes through the normal registry, which for
		// game-thread-sensitive tools will be entered from the MCP server on the
		// HTTP worker thread and marshalled via the task-graph-wait path. For the
		// built-in batch tool running synchronously here we're already on the
		// caller's thread; each sub-tool's handler does its own dispatch if needed.
		FOliveToolResult StepResult = ExecuteTool(ToolName, StepParams);

		TSharedPtr<FJsonObject> StepEntry = MakeShared<FJsonObject>();
		StepEntry->SetNumberField(TEXT("index"), i);
		if (!StepId.IsEmpty())
		{
			StepEntry->SetStringField(TEXT("id"), StepId);
		}
		StepEntry->SetStringField(TEXT("tool"), ToolName);
		StepEntry->SetBoolField(TEXT("success"), StepResult.bSuccess);
		if (StepResult.Data.IsValid())
		{
			StepEntry->SetObjectField(TEXT("data"), StepResult.Data);
		}
		if (!StepResult.bSuccess && StepResult.Messages.Num() > 0)
		{
			const FOliveIRMessage& FirstErr = StepResult.Messages[0];
			StepEntry->SetStringField(TEXT("error_code"), FirstErr.Code);
			StepEntry->SetStringField(TEXT("error_message"), FirstErr.Message);
			if (!FirstErr.Suggestion.IsEmpty())
			{
				StepEntry->SetStringField(TEXT("suggestion"), FirstErr.Suggestion);
			}
		}

		Results.Add(MakeShared<FJsonValueObject>(StepEntry));

		if (StepResult.bSuccess)
		{
			SucceededCount++;
			if (!StepId.IsEmpty() && StepResult.Data.IsValid())
			{
				Aliases.Add(StepId, StepResult.Data);
			}
		}
		else
		{
			FailedCount++;
			if (FirstFailedIndex == INDEX_NONE) FirstFailedIndex = i;
			if (bStopOnError) break;
		}
	}

	TSharedPtr<FJsonObject> OutData = MakeShared<FJsonObject>();
	OutData->SetArrayField(TEXT("results"), Results);
	OutData->SetNumberField(TEXT("total"), StepsArr->Num());
	OutData->SetNumberField(TEXT("succeeded"), SucceededCount);
	OutData->SetNumberField(TEXT("failed"), FailedCount);
	OutData->SetBoolField(TEXT("stopped_early"), bStopOnError && FirstFailedIndex != INDEX_NONE);
	if (FirstFailedIndex != INDEX_NONE)
	{
		OutData->SetNumberField(TEXT("first_failed_index"), FirstFailedIndex);
	}

	// Overall result: success if no failures (regardless of stop_on_error).
	if (FailedCount == 0)
	{
		return FOliveToolResult::Success(OutData);
	}

	FOliveToolResult OverallError = FOliveToolResult::Error(
		TEXT("BATCH_PARTIAL_FAILURE"),
		FString::Printf(TEXT("%d of %d batch steps failed (first failure at index %d)"), FailedCount, StepsArr->Num(), FirstFailedIndex),
		TEXT("Inspect results[first_failed_index] for the specific failure. Fix and retry only the failing steps."));
	OverallError.Data = OutData;
	return OverallError;
}

