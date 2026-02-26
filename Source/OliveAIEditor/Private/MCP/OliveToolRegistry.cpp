// Copyright Bode Software. All Rights Reserved.

#include "MCP/OliveToolRegistry.h"
#include "Index/OliveProjectIndex.h"
#include "Services/OliveValidationEngine.h"
#include "Services/OliveErrorBuilder.h"
#include "Profiles/OliveFocusProfileManager.h"
#include "Settings/OliveAISettings.h"
#include "Brain/OliveToolExecutionContext.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"

namespace
{
	bool IsBlueprintTool(const FString& ToolName)
	{
		return ToolName.StartsWith(TEXT("blueprint."));
	}

	bool IsBlueprintPlanTool(const FString& ToolName)
	{
		return ToolName == TEXT("blueprint.preview_plan_json")
			|| ToolName == TEXT("blueprint.apply_plan_json");
	}

	bool IsBlueprintGranularGraphTool(const FString& ToolName)
	{
		return ToolName == TEXT("blueprint.add_node")
			|| ToolName == TEXT("blueprint.remove_node")
			|| ToolName == TEXT("blueprint.connect_pins")
			|| ToolName == TEXT("blueprint.disconnect_pins")
			|| ToolName == TEXT("blueprint.set_pin_default")
			|| ToolName == TEXT("blueprint.set_node_property");
	}

	FString GetRoutingContextKey()
	{
		if (const FOliveToolCallContext* Ctx = FOliveToolExecutionContext::Get())
		{
			if (!Ctx->RunId.IsEmpty())
			{
				return FString::Printf(TEXT("run:%s"), *Ctx->RunId);
			}
			if (!Ctx->SessionId.IsEmpty())
			{
				return FString::Printf(TEXT("session:%s"), *Ctx->SessionId);
			}
		}
		return TEXT("global");
	}

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
		// Path aliasing — plan tools use asset_path, others use path
		const TCHAR* CanonicalField = IsBlueprintPlanTool(ToolName) ? TEXT("asset_path") : TEXT("path");

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

		// template_id <- template / id
		TryApplyAlias(Effective, TEXT("template_id"), TEXT("template"), OutNormalizedFields)
			|| TryApplyAlias(Effective, TEXT("template_id"), TEXT("id"), OutNormalizedFields);
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

	struct FBlueprintRoutingStats
	{
		int32 GranularGraphCalls = 0;
		int32 PlanCalls = 0;
	};

	FCriticalSection GBlueprintRoutingStatsLock;
	TMap<FString, FBlueprintRoutingStats> GBlueprintRoutingStatsByContext;
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
	Tools.Remove(Definition.Name);

	FToolEntry Entry;
	Entry.Definition = Definition;
	Entry.Handler = Handler;
	Tools.Add(Definition.Name, Entry);

	UE_LOG(LogOliveAI, Verbose, TEXT("Registered tool: %s (%s)"), *Definition.Name, *Definition.Category);
}

void FOliveToolRegistry::UnregisterTool(const FString& Name)
{
	FRWScopeLock WriteLock(ToolsLock, SLT_Write);
	Tools.Remove(Name);
	UE_LOG(LogOliveAI, Verbose, TEXT("Unregistered tool: %s"), *Name);
}

bool FOliveToolRegistry::HasTool(const FString& Name) const
{
	FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);
	return Tools.Contains(Name);
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

TArray<FOliveToolDefinition> FOliveToolRegistry::GetToolsForProfile(const FString& ProfileName) const
{
	TArray<FOliveToolDefinition> Result;
	const FOliveFocusProfileManager& FocusManager = FOliveFocusProfileManager::Get();

	FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);
	for (const auto& Pair : Tools)
	{
		const FOliveToolDefinition& Definition = Pair.Value.Definition;
		if (FocusManager.IsToolAllowedForProfile(ProfileName, Definition.Name, Definition.Category))
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
	TArray<FString> NormalizedFields;
	TSharedPtr<FJsonObject> EffectiveParams = NormalizeToolParams(Name, Params, NormalizedFields);

	const bool bIsPlanTool = IsBlueprintPlanTool(Name);
	const bool bIsGranularGraphTool = IsBlueprintGranularGraphTool(Name);
	FString RoutingReasonCode;
	bool bAttachRoutingReason = false;

	if (IsBlueprintTool(Name))
	{
		if (const UOliveAISettings* Settings = UOliveAISettings::Get())
		{
			const bool bPlanRoutingEnabled = Settings->bEnableBlueprintPlanJsonTools && Settings->bEnforcePlanFirstGraphRouting;
			const int32 Threshold = FMath::Max(1, Settings->PlanFirstGraphRoutingThreshold);
			if (bPlanRoutingEnabled && (bIsPlanTool || bIsGranularGraphTool))
			{
				const FString ContextKey = GetRoutingContextKey();
				bool bBypassForUnsupported = false;
				FString RoutingHint;
				if (EffectiveParams.IsValid() && EffectiveParams->TryGetStringField(TEXT("routing_reason"), RoutingHint))
				{
					bBypassForUnsupported = RoutingHint.Equals(TEXT("op_unsupported"), ESearchCase::IgnoreCase);
				}

				{
					FScopeLock Lock(&GBlueprintRoutingStatsLock);
					FBlueprintRoutingStats& Stats = GBlueprintRoutingStatsByContext.FindOrAdd(ContextKey);

					if (bIsPlanTool)
					{
						Stats.PlanCalls++;
					}
					else
					{
						if (bBypassForUnsupported)
						{
							Stats.GranularGraphCalls++;
							RoutingReasonCode = TEXT("ROUTE_OP_UNSUPPORTED");
							bAttachRoutingReason = true;
						}
						else
						{
							const int32 NextCount = Stats.GranularGraphCalls + 1;
							if (Stats.PlanCalls == 0 && NextCount >= Threshold)
							{
								TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
								ErrorData->SetStringField(TEXT("reason_code"), TEXT("ROUTE_PLAN_REQUIRED"));
								ErrorData->SetStringField(TEXT("message"),
									TEXT("Granular graph edits exceeded threshold for this run/session; use plan preview/apply flow."));
								ErrorData->SetNumberField(TEXT("threshold"), Threshold);
								ErrorData->SetNumberField(TEXT("granular_graph_calls"), NextCount);
								TArray<TSharedPtr<FJsonValue>> RecommendedTools;
								RecommendedTools.Add(MakeShared<FJsonValueString>(TEXT("blueprint.apply_plan_json")));
								ErrorData->SetArrayField(TEXT("recommended_tools"), RecommendedTools);

								FOliveToolResult RouteError = FOliveToolResult::Error(
									TEXT("ROUTE_PLAN_REQUIRED"),
									TEXT("Plan-first routing required for multi-step graph edits."),
									TEXT("Call blueprint.apply_plan_json directly (preview is optional — use it in a separate prior turn if needed)."));
								RouteError.Data = ErrorData;
								return RouteError;
							}

							Stats.GranularGraphCalls = NextCount;
							RoutingReasonCode = TEXT("ROUTE_SMALL_EDIT_ALLOWED");
							bAttachRoutingReason = true;
						}
					}
				}
			}
		}
	}

	// Find tool
	FOliveToolHandler Handler;
	{
		FRWScopeLock ReadLock(ToolsLock, SLT_ReadOnly);

		const FToolEntry* Entry = Tools.Find(Name);
		if (!Entry)
		{
			return FOliveToolResult::Error(
				FOliveErrorBuilder::ERR_TOOL_NOT_FOUND,
				FString::Printf(TEXT("Tool '%s' not found"), *Name),
				TEXT("Use tools/list to see available tools.")
			);
		}

		Handler = Entry->Handler;
	}

	// Validate with validation engine
	FOliveValidationResult ValidationResult = FOliveValidationEngine::Get().ValidateOperation(Name, EffectiveParams, nullptr);
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
		*Name, Result.ExecutionTimeMs, Result.bSuccess ? TEXT("success") : TEXT("failed"));

	if (NormalizedFields.Num() > 0 || bAttachRoutingReason)
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

		if (bAttachRoutingReason)
		{
			Result.Data->SetStringField(TEXT("reason_code"), RoutingReasonCode);
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

void FOliveToolRegistry::ClearBlueprintRoutingStats(const FString& ContextKey)
{
	FScopeLock Lock(&GBlueprintRoutingStatsLock);
	GBlueprintRoutingStatsByContext.Remove(ContextKey);
}

// ==========================================
// MCP Format
// ==========================================

TSharedPtr<FJsonObject> FOliveToolRegistry::GetToolsListMCP(const FString& ProfileFilter) const
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> ToolsArray;

	TArray<FOliveToolDefinition> ToolsList = ProfileFilter.IsEmpty()
		? GetAllTools()
		: GetToolsForProfile(ProfileFilter);

	for (const FOliveToolDefinition& Tool : ToolsList)
	{
		TSharedPtr<FJsonObject> ToolJson = Tool.ToMCPJson();
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

	// project.get_dependencies
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
		PathProp->SetStringField(TEXT("type"), TEXT("string"));
		PathProp->SetStringField(TEXT("description"), TEXT("Asset path to get dependencies for"));
		Properties->SetObjectField(TEXT("path"), PathProp);

		Schema->SetObjectField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("path")));
		Schema->SetArrayField(TEXT("required"), Required);

		RegisterTool(
			TEXT("project.get_dependencies"),
			TEXT("Get all assets that the specified asset depends on."),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleProjectGetDependencies),
			{ TEXT("project"), TEXT("dependencies") },
			TEXT("project")
		);
	}

	// project.get_referencers
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
		PathProp->SetStringField(TEXT("type"), TEXT("string"));
		PathProp->SetStringField(TEXT("description"), TEXT("Asset path to get referencers for"));
		Properties->SetObjectField(TEXT("path"), PathProp);

		Schema->SetObjectField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("path")));
		Schema->SetArrayField(TEXT("required"), Required);

		RegisterTool(
			TEXT("project.get_referencers"),
			TEXT("Get all assets that reference the specified asset."),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleProjectGetReferencers),
			{ TEXT("project"), TEXT("referencers") },
			TEXT("project")
		);
	}

	// project.get_config
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());

		RegisterTool(
			TEXT("project.get_config"),
			TEXT("Get project configuration including engine version, enabled plugins, and primary asset types."),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleProjectGetConfig),
			{ TEXT("project"), TEXT("config") },
			TEXT("project")
		);
	}
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

