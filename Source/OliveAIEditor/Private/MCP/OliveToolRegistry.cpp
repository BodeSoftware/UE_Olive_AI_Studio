// Copyright Bode Software. All Rights Reserved.

#include "MCP/OliveToolRegistry.h"
#include "Index/OliveProjectIndex.h"
#include "Services/OliveValidationEngine.h"
#include "Services/OliveErrorBuilder.h"
#include "OliveAIEditorModule.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"

// ==========================================
// FOliveToolDefinition
// ==========================================

TSharedPtr<FJsonObject> FOliveToolDefinition::ToMCPJson() const
{
	TSharedPtr<FJsonObject> ToolJson = MakeShared<FJsonObject>();

	// MCP format uses "function" wrapper
	TSharedPtr<FJsonObject> FunctionJson = MakeShared<FJsonObject>();
	FunctionJson->SetStringField(TEXT("name"), Name);
	FunctionJson->SetStringField(TEXT("description"), Description);

	if (InputSchema.IsValid())
	{
		FunctionJson->SetObjectField(TEXT("parameters"), InputSchema);
	}
	else
	{
		// Empty schema
		TSharedPtr<FJsonObject> EmptySchema = MakeShared<FJsonObject>();
		EmptySchema->SetStringField(TEXT("type"), TEXT("object"));
		EmptySchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		FunctionJson->SetObjectField(TEXT("parameters"), EmptySchema);
	}

	ToolJson->SetStringField(TEXT("type"), TEXT("function"));
	ToolJson->SetObjectField(TEXT("function"), FunctionJson);

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

	if (bSuccess)
	{
		if (Data.IsValid())
		{
			Response->SetObjectField(TEXT("data"), Data);
		}
	}
	else
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
	// For Phase 0, return all tools. Focus profile filtering will be implemented with Focus Profile Manager.
	// TODO: Integrate with FOliveFocusProfileManager when implemented
	return GetAllTools();
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
	FOliveValidationResult ValidationResult = FOliveValidationEngine::Get().ValidateOperation(Name, Params, nullptr);
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
		Result = Handler.Execute(Params);
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
	RegisterBlueprintToolStubs();

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

void FOliveToolRegistry::RegisterBlueprintToolStubs()
{
	// blueprint.read (stub)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
		PathProp->SetStringField(TEXT("type"), TEXT("string"));
		PathProp->SetStringField(TEXT("description"), TEXT("Blueprint asset path"));
		Properties->SetObjectField(TEXT("path"), PathProp);

		Schema->SetObjectField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("path")));
		Schema->SetArrayField(TEXT("required"), Required);

		RegisterTool(
			TEXT("blueprint.read"),
			TEXT("Read a Blueprint's structure including components, variables, functions, and graphs. [Phase 1 - Not Yet Implemented]"),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleBlueprintReadStub),
			{ TEXT("blueprint"), TEXT("read") },
			TEXT("blueprint")
		);
	}

	// blueprint.create (stub)
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
		NameProp->SetStringField(TEXT("type"), TEXT("string"));
		NameProp->SetStringField(TEXT("description"), TEXT("Name for the new Blueprint"));
		Properties->SetObjectField(TEXT("name"), NameProp);

		TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
		PathProp->SetStringField(TEXT("type"), TEXT("string"));
		PathProp->SetStringField(TEXT("description"), TEXT("Directory path to create in (e.g., /Game/Blueprints)"));
		Properties->SetObjectField(TEXT("path"), PathProp);

		TSharedPtr<FJsonObject> ParentProp = MakeShared<FJsonObject>();
		ParentProp->SetStringField(TEXT("type"), TEXT("string"));
		ParentProp->SetStringField(TEXT("description"), TEXT("Parent class (e.g., Actor, Pawn, Character)"));
		Properties->SetObjectField(TEXT("parent_class"), ParentProp);

		Schema->SetObjectField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("name")));
		Required.Add(MakeShared<FJsonValueString>(TEXT("path")));
		Schema->SetArrayField(TEXT("required"), Required);

		RegisterTool(
			TEXT("blueprint.create"),
			TEXT("Create a new Blueprint asset. [Phase 1 - Not Yet Implemented]"),
			Schema,
			FOliveToolHandler::CreateRaw(this, &FOliveToolRegistry::HandleBlueprintCreateStub),
			{ TEXT("blueprint"), TEXT("create") },
			TEXT("blueprint")
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

// ==========================================
// Tool Handlers - Blueprint Stubs
// ==========================================

FOliveToolResult FOliveToolRegistry::HandleBlueprintReadStub(const TSharedPtr<FJsonObject>& Params)
{
	return FOliveToolResult::Error(
		FOliveErrorBuilder::ERR_NOT_IMPLEMENTED,
		TEXT("blueprint.read is not yet implemented"),
		TEXT("This tool will be available in Phase 1. Use project.search to find Blueprints by name.")
	);
}

FOliveToolResult FOliveToolRegistry::HandleBlueprintCreateStub(const TSharedPtr<FJsonObject>& Params)
{
	return FOliveToolResult::Error(
		FOliveErrorBuilder::ERR_NOT_IMPLEMENTED,
		TEXT("blueprint.create is not yet implemented"),
		TEXT("This tool will be available in Phase 1. You can manually create Blueprints in the Content Browser.")
	);
}
