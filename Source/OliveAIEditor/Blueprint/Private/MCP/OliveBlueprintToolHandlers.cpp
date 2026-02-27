// Copyright Bode Software. All Rights Reserved.

#include "OliveBlueprintToolHandlers.h"
#include "OliveBlueprintSchemas.h"
#include "OliveBlueprintReader.h"
#include "OliveBlueprintWriter.h"
#include "OliveComponentWriter.h"
#include "OliveGraphWriter.h"
#include "OliveAnimGraphWriter.h"
#include "OliveWidgetWriter.h"
#include "OliveCompileManager.h"
#include "OliveWritePipeline.h"
#include "Brain/OliveToolExecutionContext.h"
#include "OliveBlueprintTypes.h"
#include "IR/BlueprintIR.h"
#include "IR/CommonIR.h"
#include "Engine/Blueprint.h"
#include "Animation/AnimBlueprint.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Services/OliveAssetResolver.h"
#include "OliveNodeFactory.h"
#include "OliveNodeCatalog.h"
#include "OliveGraphReader.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Plan/OliveBlueprintPlanResolver.h"
#include "Plan/OliveBlueprintPlanLowerer.h"
#include "Plan/OlivePlanExecutor.h"
#include "Plan/OlivePlanValidator.h"
#include "Plan/OlivePinManifest.h"
#include "Plan/OliveFunctionResolver.h"
#include "Services/OliveGraphBatchExecutor.h"
#include "Services/OliveBatchExecutionScope.h"
#include "IR/OliveIRSchema.h"
#include "IR/BlueprintPlanIR.h"
#include "Template/OliveTemplateSystem.h"

DEFINE_LOG_CATEGORY(LogOliveBPTools);

namespace
{

/**
 * Build large-graph summary metadata and attach it to a graph IR JSON object.
 * Adds is_large_graph, page_size, total_pages, event_nodes list, and node_type_breakdown.
 *
 * @param ResultData The graph JSON object to augment with summary fields
 * @param Graph The raw UEdGraph to extract event nodes and type breakdown from
 * @param NodeIdMap The node ID map built by the graph reader (for resolving event node IDs)
 * @param PageSize The page size to report in the summary
 */
void AttachLargeGraphSummaryMetadata(
	TSharedPtr<FJsonObject>& ResultData,
	const UEdGraph* Graph,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap,
	int32 PageSize)
{
	if (!ResultData.IsValid() || !Graph)
	{
		return;
	}

	const int32 TotalNodes = NodeIdMap.Num();

	ResultData->SetBoolField(TEXT("is_large_graph"), true);
	ResultData->SetNumberField(TEXT("page_size"), PageSize);
	ResultData->SetNumberField(TEXT("total_pages"),
		FMath::CeilToInt(static_cast<float>(TotalNodes) / static_cast<float>(PageSize)));

	// Build event node list (entry points the agent can navigate from)
	TArray<TSharedPtr<FJsonValue>> EventNodes;
	// Build node type breakdown (class name -> count)
	TMap<FString, int32> TypeBreakdown;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		// Count node types
		FString ClassName = Node->GetClass()->GetName();
		TypeBreakdown.FindOrAdd(ClassName, 0)++;

		// Collect event nodes (entry points)
		if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
		{
			const FString* NodeId = NodeIdMap.Find(Node);
			if (NodeId)
			{
				TSharedPtr<FJsonObject> EventObj = MakeShareable(new FJsonObject());
				EventObj->SetStringField(TEXT("node_id"), *NodeId);
				EventObj->SetStringField(TEXT("event_name"),
					EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
				EventObj->SetBoolField(TEXT("is_custom_event"),
					EventNode->IsA<UK2Node_CustomEvent>());
				EventObj->SetNumberField(TEXT("pos_x"), EventNode->NodePosX);
				EventObj->SetNumberField(TEXT("pos_y"), EventNode->NodePosY);
				EventNodes.Add(MakeShareable(new FJsonValueObject(EventObj)));
			}
		}
	}

	ResultData->SetArrayField(TEXT("event_nodes"), EventNodes);

	// Build node type breakdown JSON
	TSharedPtr<FJsonObject> BreakdownObj = MakeShareable(new FJsonObject());
	for (const auto& Pair : TypeBreakdown)
	{
		BreakdownObj->SetNumberField(Pair.Key, Pair.Value);
	}
	ResultData->SetObjectField(TEXT("node_type_breakdown"), BreakdownObj);
}

/**
 * Extract large-graph paging parameters from tool call params.
 * @param Params The tool call parameters
 * @param OutPage Set to page number if 'page' field is present (-1 if absent)
 * @param OutPageSize Set to page size (clamped to [10, 200])
 * @param OutMode Set to the mode string ("auto" or "full")
 */
void ExtractPagingParams(
	const TSharedPtr<FJsonObject>& Params,
	int32& OutPage,
	int32& OutPageSize,
	FString& OutMode)
{
	OutPage = -1;
	OutPageSize = OLIVE_GRAPH_PAGE_SIZE;
	OutMode = TEXT("auto");

	if (!Params.IsValid())
	{
		return;
	}

	if (Params->HasField(TEXT("page")))
	{
		OutPage = static_cast<int32>(Params->GetNumberField(TEXT("page")));
		if (OutPage < 0) OutPage = 0;
	}

	if (Params->HasField(TEXT("page_size")))
	{
		OutPageSize = static_cast<int32>(Params->GetNumberField(TEXT("page_size")));
		OutPageSize = FMath::Clamp(OutPageSize, 10, 200);
	}

	Params->TryGetStringField(TEXT("mode"), OutMode);
}

/**
 * Handle a graph read with large-graph detection.
 * If the graph exceeds OLIVE_LARGE_GRAPH_THRESHOLD nodes and no page/full-mode is requested,
 * returns a summary. If page is specified, returns that page. Otherwise returns full graph.
 *
 * @param Graph The raw UEdGraph
 * @param Blueprint The owning Blueprint
 * @param GraphReader The graph reader to use
 * @param Params The original tool call parameters
 * @param ResolveInfo Asset resolution info for redirect injection
 * @return Tool result with appropriate graph data
 */
FOliveToolResult HandleGraphReadWithPaging(
	UEdGraph* Graph,
	const UBlueprint* Blueprint,
	TSharedPtr<FOliveGraphReader>& GraphReader,
	const TSharedPtr<FJsonObject>& Params,
	const FOliveAssetResolveInfo& ResolveInfo)
{
	if (!Graph || !GraphReader.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("INTERNAL_ERROR"),
			TEXT("Invalid graph or graph reader"),
			TEXT("This is an internal error. Please report it.")
		);
	}

	// Extract paging parameters
	int32 Page = -1;
	int32 PageSize = OLIVE_GRAPH_PAGE_SIZE;
	FString Mode;
	ExtractPagingParams(Params, Page, PageSize, Mode);

	// Count nodes (fast -- just count the array, no serialization)
	const int32 RawNodeCount = Graph->Nodes.Num();

	// If page is explicitly requested, return that page
	if (Page >= 0)
	{
		const int32 Offset = Page * PageSize;
		FOliveIRGraph PageIR = GraphReader->ReadGraphPage(Graph, Blueprint, Offset, PageSize);
		TSharedPtr<FJsonObject> ResultData = PageIR.ToJson();

		ResultData->SetNumberField(TEXT("page"), Page);
		ResultData->SetNumberField(TEXT("page_size"), PageSize);
		ResultData->SetNumberField(TEXT("total_pages"),
			FMath::CeilToInt(static_cast<float>(PageIR.NodeCount) / static_cast<float>(PageSize)));
		ResultData->SetBoolField(TEXT("is_large_graph"), PageIR.NodeCount >= OLIVE_LARGE_GRAPH_THRESHOLD);

		// Inject redirector info
		if (!ResolveInfo.RedirectedFrom.IsEmpty() && ResultData.IsValid())
		{
			ResultData->SetStringField(TEXT("redirected_from"), ResolveInfo.RedirectedFrom);
		}

		return FOliveToolResult::Success(ResultData);
	}

	// If graph is large and mode is not "full", return summary
	if (RawNodeCount >= OLIVE_LARGE_GRAPH_THRESHOLD && Mode != TEXT("full"))
	{
		FOliveIRGraph SummaryIR = GraphReader->ReadGraphSummary(Graph, Blueprint);
		TSharedPtr<FJsonObject> ResultData = SummaryIR.ToJson();

		// Attach large-graph metadata (event nodes, type breakdown, paging info)
		AttachLargeGraphSummaryMetadata(ResultData, Graph, GraphReader->GetNodeIdMap(), PageSize);

		// Inject redirector info
		if (!ResolveInfo.RedirectedFrom.IsEmpty() && ResultData.IsValid())
		{
			ResultData->SetStringField(TEXT("redirected_from"), ResolveInfo.RedirectedFrom);
		}

		return FOliveToolResult::Success(ResultData);
	}

	// Normal full read
	FOliveIRGraph GraphIR = GraphReader->ReadGraph(Graph, Blueprint);
	TSharedPtr<FJsonObject> ResultData = GraphIR.ToJson();

	// Inject redirector info
	if (!ResolveInfo.RedirectedFrom.IsEmpty() && ResultData.IsValid())
	{
		ResultData->SetStringField(TEXT("redirected_from"), ResolveInfo.RedirectedFrom);
	}

	return FOliveToolResult::Success(ResultData);
}

FOliveWriteResult ExecuteWithOptionalConfirmation(
	FOliveWritePipeline& Pipeline,
	const FOliveWriteRequest& Request,
	FOliveWriteExecutor Executor)
{
	FString ConfirmationToken;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("confirmation_token"), ConfirmationToken);
	}

	if (!ConfirmationToken.IsEmpty())
	{
		return Pipeline.ExecuteConfirmed(Request, ConfirmationToken, Executor);
	}

	return Pipeline.Execute(Request, Executor);
}

FString NormalizeSemanticToken(const FString& InValue)
{
	FString Out = InValue;
	Out.ReplaceInline(TEXT(" "), TEXT(""));
	Out.ReplaceInline(TEXT("_"), TEXT(""));
	Out.ReplaceInline(TEXT("-"), TEXT(""));
	Out = Out.ToLower();
	return Out;
}

TSharedPtr<FJsonObject> BuildPinDescriptor(const UEdGraphPin* Pin)
{
	TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
	PinObj->SetStringField(TEXT("name"), Pin->GetName());
	PinObj->SetStringField(TEXT("display_name"), Pin->GetDisplayName().ToString());
	PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
	PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
	PinObj->SetBoolField(TEXT("is_exec"), Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
	return PinObj;
}

TSharedPtr<FJsonObject> BuildPinManifest(const UEdGraphNode* Node)
{
	TSharedPtr<FJsonObject> PinsObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Inputs;
	TArray<TSharedPtr<FJsonValue>> Outputs;

	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}

		TSharedPtr<FJsonValueObject> PinValue = MakeShared<FJsonValueObject>(BuildPinDescriptor(Pin));
		if (Pin->Direction == EGPD_Input)
		{
			Inputs.Add(PinValue);
		}
		else
		{
			Outputs.Add(PinValue);
		}
	}

	PinsObj->SetArrayField(TEXT("inputs"), Inputs);
	PinsObj->SetArrayField(TEXT("outputs"), Outputs);
	return PinsObj;
}

bool ResolveSemanticPinOnNode(
	const UEdGraphNode* Node,
	const FString& Semantic,
	EEdGraphPinDirection Direction,
	FString& OutPinName,
	TArray<TSharedPtr<FJsonValue>>& OutCandidates)
{
	if (!Node)
	{
		return false;
	}

	const FString SemanticNorm = NormalizeSemanticToken(Semantic);
	TArray<const UEdGraphPin*> DirectionPins;
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != Direction)
		{
			continue;
		}

		DirectionPins.Add(Pin);
		OutCandidates.Add(MakeShared<FJsonValueObject>(BuildPinDescriptor(Pin)));

		const FString PinNorm = NormalizeSemanticToken(Pin->GetName());
		const FString DisplayNorm = NormalizeSemanticToken(Pin->GetDisplayName().ToString());
		if (SemanticNorm == PinNorm || SemanticNorm == DisplayNorm)
		{
			OutPinName = Pin->GetName();
			return true;
		}
	}

	auto PickFirstPin = [&](bool bExec) -> bool
	{
		for (const UEdGraphPin* Pin : DirectionPins)
		{
			const bool bPinExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
			if (bPinExec == bExec)
			{
				OutPinName = Pin->GetName();
				return true;
			}
		}
		return false;
	};

	auto PickByContains = [&](const FString& Token) -> bool
	{
		for (const UEdGraphPin* Pin : DirectionPins)
		{
			const FString PinNorm = NormalizeSemanticToken(Pin->GetName());
			const FString DisplayNorm = NormalizeSemanticToken(Pin->GetDisplayName().ToString());
			if (PinNorm.Contains(Token) || DisplayNorm.Contains(Token))
			{
				OutPinName = Pin->GetName();
				return true;
			}
		}
		return false;
	};

	if (SemanticNorm == TEXT("execout") || SemanticNorm == TEXT("then"))
	{
		return PickFirstPin(true);
	}
	if (SemanticNorm == TEXT("execin") || SemanticNorm == TEXT("execute"))
	{
		return PickFirstPin(true);
	}
	if (SemanticNorm == TEXT("datain"))
	{
		return PickFirstPin(false);
	}
	if (SemanticNorm == TEXT("dataout"))
	{
		return PickFirstPin(false);
	}
	if (SemanticNorm == TEXT("true"))
	{
		return PickByContains(TEXT("true"));
	}
	if (SemanticNorm == TEXT("false"))
	{
		return PickByContains(TEXT("false"));
	}
	if (SemanticNorm == TEXT("condition"))
	{
		return PickByContains(TEXT("condition"));
	}

	return false;
}
} // namespace

// ============================================================================
// Singleton Access
// ============================================================================

FOliveBlueprintToolHandlers& FOliveBlueprintToolHandlers::Get()
{
	static FOliveBlueprintToolHandlers Instance;
	return Instance;
}

// ============================================================================
// Registration
// ============================================================================

void FOliveBlueprintToolHandlers::RegisterAllTools()
{
	UE_LOG(LogOliveBPTools, Log, TEXT("Registering Blueprint MCP tools..."));

	RegisterReaderTools();
	RegisterAssetWriterTools();
	RegisterVariableWriterTools();
	RegisterComponentWriterTools();
	RegisterFunctionWriterTools();
	RegisterGraphWriterTools();
	RegisterAnimBPWriterTools();
	RegisterWidgetWriterTools();

	// Plan JSON tools (gated by settings)
	if (const UOliveAISettings* Settings = UOliveAISettings::Get())
	{
		if (Settings->bEnableBlueprintPlanJsonTools)
		{
			RegisterPlanTools();
		}
	}

	// Template tools (always registered; handlers check template availability)
	RegisterTemplateTools();

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered %d Blueprint MCP tools"), RegisteredToolNames.Num());
}

void FOliveBlueprintToolHandlers::UnregisterAllTools()
{
	UE_LOG(LogOliveBPTools, Log, TEXT("Unregistering Blueprint MCP tools..."));

	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
	for (const FString& ToolName : RegisteredToolNames)
	{
		Registry.UnregisterTool(ToolName);
	}

	RegisteredToolNames.Empty();

	UE_LOG(LogOliveBPTools, Log, TEXT("Blueprint MCP tools unregistered"));
}

// ============================================================================
// Reader Tool Registration
// ============================================================================

void FOliveBlueprintToolHandlers::RegisterReaderTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// blueprint.read
	Registry.RegisterTool(
		TEXT("blueprint.read"),
		TEXT("Read Blueprint structure with optional mode (summary or full with all graph data)"),
		OliveBlueprintSchemas::BlueprintRead(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintRead),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.read"));

	// blueprint.read_function
	Registry.RegisterTool(
		TEXT("blueprint.read_function"),
		TEXT("Read a single function graph from a Blueprint"),
		OliveBlueprintSchemas::BlueprintReadFunction(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintReadFunction),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.read_function"));

	// blueprint.read_event_graph
	Registry.RegisterTool(
		TEXT("blueprint.read_event_graph"),
		TEXT("Read an event graph from a Blueprint"),
		OliveBlueprintSchemas::BlueprintReadEventGraph(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintReadEventGraph),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.read_event_graph"));

	// blueprint.read_variables
	Registry.RegisterTool(
		TEXT("blueprint.read_variables"),
		TEXT("Read all variables from a Blueprint"),
		OliveBlueprintSchemas::BlueprintReadVariables(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintReadVariables),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.read_variables"));

	// blueprint.read_components
	Registry.RegisterTool(
		TEXT("blueprint.read_components"),
		TEXT("Read component hierarchy from a Blueprint"),
		OliveBlueprintSchemas::BlueprintReadComponents(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintReadComponents),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.read_components"));

	// blueprint.read_hierarchy
	Registry.RegisterTool(
		TEXT("blueprint.read_hierarchy"),
		TEXT("Read class hierarchy from a Blueprint (parent chain)"),
		OliveBlueprintSchemas::BlueprintReadHierarchy(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintReadHierarchy),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.read_hierarchy"));

	// blueprint.list_overridable_functions
	Registry.RegisterTool(
		TEXT("blueprint.list_overridable_functions"),
		TEXT("List all functions from parent classes that can be overridden"),
		OliveBlueprintSchemas::BlueprintListOverridableFunctions(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintListOverridableFunctions),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.list_overridable_functions"));

	// blueprint.get_node_pins
	Registry.RegisterTool(
		TEXT("blueprint.get_node_pins"),
		TEXT("Get pin manifest for a specific node in a Blueprint graph"),
		OliveBlueprintSchemas::BlueprintGetNodePins(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintGetNodePins),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.get_node_pins"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered %d reader tools"), 8);
}

// ============================================================================
// Asset Writer Tool Registration
// ============================================================================

void FOliveBlueprintToolHandlers::RegisterAssetWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// blueprint.create
	Registry.RegisterTool(
		TEXT("blueprint.create"),
		TEXT("Create a new Blueprint asset"),
		OliveBlueprintSchemas::BlueprintCreate(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCreate),
		{TEXT("blueprint"), TEXT("write"), TEXT("create")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.create"));

	// blueprint.set_parent_class
	Registry.RegisterTool(
		TEXT("blueprint.set_parent_class"),
		TEXT("Change the parent class of a Blueprint (Tier 3 - potentially destructive)"),
		OliveBlueprintSchemas::BlueprintSetParentClass(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintSetParentClass),
		{TEXT("blueprint"), TEXT("write"), TEXT("refactor")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.set_parent_class"));

	// blueprint.add_interface
	Registry.RegisterTool(
		TEXT("blueprint.add_interface"),
		TEXT("Add an interface to a Blueprint"),
		OliveBlueprintSchemas::BlueprintAddInterface(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintAddInterface),
		{TEXT("blueprint"), TEXT("write"), TEXT("interface")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.add_interface"));

	// blueprint.remove_interface
	Registry.RegisterTool(
		TEXT("blueprint.remove_interface"),
		TEXT("Remove an interface from a Blueprint"),
		OliveBlueprintSchemas::BlueprintRemoveInterface(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintRemoveInterface),
		{TEXT("blueprint"), TEXT("write"), TEXT("interface")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.remove_interface"));

	// blueprint.compile
	Registry.RegisterTool(
		TEXT("blueprint.compile"),
		TEXT("Force compile a Blueprint and return compilation results"),
		OliveBlueprintSchemas::BlueprintCompile(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCompile),
		{TEXT("blueprint"), TEXT("compile")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.compile"));

	// blueprint.delete
	Registry.RegisterTool(
		TEXT("blueprint.delete"),
		TEXT("Delete a Blueprint asset (Tier 3 - requires confirmation)"),
		OliveBlueprintSchemas::BlueprintDelete(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintDelete),
		{TEXT("blueprint"), TEXT("write"), TEXT("delete")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.delete"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 6 asset writer tools"));
}

void FOliveBlueprintToolHandlers::RegisterVariableWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// blueprint.add_variable
	Registry.RegisterTool(
		TEXT("blueprint.add_variable"),
		TEXT("Add a variable to a Blueprint"),
		OliveBlueprintSchemas::BlueprintAddVariable(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintAddVariable),
		{TEXT("blueprint"), TEXT("write"), TEXT("variable")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.add_variable"));

	// blueprint.remove_variable
	Registry.RegisterTool(
		TEXT("blueprint.remove_variable"),
		TEXT("Remove a variable from a Blueprint"),
		OliveBlueprintSchemas::BlueprintRemoveVariable(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintRemoveVariable),
		{TEXT("blueprint"), TEXT("write"), TEXT("variable")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.remove_variable"));

	// blueprint.modify_variable
	Registry.RegisterTool(
		TEXT("blueprint.modify_variable"),
		TEXT("Modify an existing variable's properties"),
		OliveBlueprintSchemas::BlueprintModifyVariable(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintModifyVariable),
		{TEXT("blueprint"), TEXT("write"), TEXT("variable")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.modify_variable"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 3 variable writer tools"));
}

void FOliveBlueprintToolHandlers::RegisterComponentWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// blueprint.add_component
	Registry.RegisterTool(
		TEXT("blueprint.add_component"),
		TEXT("Add a component to a Blueprint's component hierarchy"),
		OliveBlueprintSchemas::BlueprintAddComponent(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintAddComponent),
		{TEXT("blueprint"), TEXT("write"), TEXT("component")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.add_component"));

	// blueprint.remove_component
	Registry.RegisterTool(
		TEXT("blueprint.remove_component"),
		TEXT("Remove a component from a Blueprint's component hierarchy"),
		OliveBlueprintSchemas::BlueprintRemoveComponent(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintRemoveComponent),
		{TEXT("blueprint"), TEXT("write"), TEXT("component")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.remove_component"));

	// blueprint.modify_component
	Registry.RegisterTool(
		TEXT("blueprint.modify_component"),
		TEXT("Modify properties of an existing component in a Blueprint"),
		OliveBlueprintSchemas::BlueprintModifyComponent(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintModifyComponent),
		{TEXT("blueprint"), TEXT("write"), TEXT("component")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.modify_component"));

	// blueprint.reparent_component
	Registry.RegisterTool(
		TEXT("blueprint.reparent_component"),
		TEXT("Change the parent of a component in the hierarchy"),
		OliveBlueprintSchemas::BlueprintReparentComponent(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintReparentComponent),
		{TEXT("blueprint"), TEXT("write"), TEXT("component")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.reparent_component"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 4 component writer tools"));
}

void FOliveBlueprintToolHandlers::RegisterFunctionWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// blueprint.add_function
	Registry.RegisterTool(
		TEXT("blueprint.add_function"),
		TEXT("Add a user-defined function to a Blueprint with specified signature"),
		OliveBlueprintSchemas::BlueprintAddFunction(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintAddFunction),
		{TEXT("blueprint"), TEXT("write"), TEXT("function")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.add_function"));

	// blueprint.remove_function
	Registry.RegisterTool(
		TEXT("blueprint.remove_function"),
		TEXT("Remove a function from a Blueprint"),
		OliveBlueprintSchemas::BlueprintRemoveFunction(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintRemoveFunction),
		{TEXT("blueprint"), TEXT("write"), TEXT("function")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.remove_function"));

	// blueprint.modify_function_signature
	Registry.RegisterTool(
		TEXT("blueprint.modify_function_signature"),
		TEXT("Modify the signature of an existing function (parameters, return values, flags)"),
		OliveBlueprintSchemas::BlueprintModifyFunctionSignature(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintModifyFunctionSignature),
		{TEXT("blueprint"), TEXT("write"), TEXT("function")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.modify_function_signature"));

	// blueprint.add_event_dispatcher
	Registry.RegisterTool(
		TEXT("blueprint.add_event_dispatcher"),
		TEXT("Add an event dispatcher (multicast delegate) to a Blueprint"),
		OliveBlueprintSchemas::BlueprintAddEventDispatcher(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintAddEventDispatcher),
		{TEXT("blueprint"), TEXT("write"), TEXT("function")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.add_event_dispatcher"));

	// blueprint.override_function
	Registry.RegisterTool(
		TEXT("blueprint.override_function"),
		TEXT("Override a parent class function in the Blueprint"),
		OliveBlueprintSchemas::BlueprintOverrideFunction(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintOverrideFunction),
		{TEXT("blueprint"), TEXT("write"), TEXT("function")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.override_function"));

	// blueprint.add_custom_event
	Registry.RegisterTool(
		TEXT("blueprint.add_custom_event"),
		TEXT("Add a custom event to the Blueprint's event graph"),
		OliveBlueprintSchemas::BlueprintAddCustomEvent(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintAddCustomEvent),
		{TEXT("blueprint"), TEXT("write"), TEXT("function")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.add_custom_event"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 6 function writer tools"));
}

void FOliveBlueprintToolHandlers::RegisterGraphWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// blueprint.add_node
	Registry.RegisterTool(
		TEXT("blueprint.add_node"),
		TEXT("Add a node to a Blueprint graph"),
		OliveBlueprintSchemas::BlueprintAddNode(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintAddNode),
		{TEXT("blueprint"), TEXT("write"), TEXT("graph")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.add_node"));

	// blueprint.remove_node
	Registry.RegisterTool(
		TEXT("blueprint.remove_node"),
		TEXT("Remove a node from a Blueprint graph"),
		OliveBlueprintSchemas::BlueprintRemoveNode(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintRemoveNode),
		{TEXT("blueprint"), TEXT("write"), TEXT("graph")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.remove_node"));

	// blueprint.connect_pins
	Registry.RegisterTool(
		TEXT("blueprint.connect_pins"),
		TEXT("Connect two pins in a Blueprint graph"),
		OliveBlueprintSchemas::BlueprintConnectPins(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintConnectPins),
		{TEXT("blueprint"), TEXT("write"), TEXT("graph")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.connect_pins"));

	// blueprint.disconnect_pins
	Registry.RegisterTool(
		TEXT("blueprint.disconnect_pins"),
		TEXT("Disconnect two pins in a Blueprint graph"),
		OliveBlueprintSchemas::BlueprintDisconnectPins(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintDisconnectPins),
		{TEXT("blueprint"), TEXT("write"), TEXT("graph")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.disconnect_pins"));

	// blueprint.set_pin_default
	Registry.RegisterTool(
		TEXT("blueprint.set_pin_default"),
		TEXT("Set the default value of an input pin"),
		OliveBlueprintSchemas::BlueprintSetPinDefault(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintSetPinDefault),
		{TEXT("blueprint"), TEXT("write"), TEXT("graph")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.set_pin_default"));

	// blueprint.set_node_property
	Registry.RegisterTool(
		TEXT("blueprint.set_node_property"),
		TEXT("Set a property on a Blueprint node"),
		OliveBlueprintSchemas::BlueprintSetNodeProperty(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintSetNodeProperty),
		{TEXT("blueprint"), TEXT("write"), TEXT("graph")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.set_node_property"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 6 graph writer tools"));
}

void FOliveBlueprintToolHandlers::RegisterAnimBPWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// animbp.add_state_machine
	Registry.RegisterTool(
		TEXT("animbp.add_state_machine"),
		TEXT("Add a state machine to an Animation Blueprint's AnimGraph"),
		OliveBlueprintSchemas::AnimBPAddStateMachine(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleAnimBPAddStateMachine),
		{TEXT("animbp"), TEXT("write"), TEXT("state_machine")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("animbp.add_state_machine"));

	// animbp.add_state
	Registry.RegisterTool(
		TEXT("animbp.add_state"),
		TEXT("Add a state to an existing state machine in an Animation Blueprint"),
		OliveBlueprintSchemas::AnimBPAddState(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleAnimBPAddState),
		{TEXT("animbp"), TEXT("write"), TEXT("state")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("animbp.add_state"));

	// animbp.add_transition
	Registry.RegisterTool(
		TEXT("animbp.add_transition"),
		TEXT("Add a transition between two states in a state machine"),
		OliveBlueprintSchemas::AnimBPAddTransition(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleAnimBPAddTransition),
		{TEXT("animbp"), TEXT("write"), TEXT("transition")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("animbp.add_transition"));

	// animbp.set_transition_rule
	Registry.RegisterTool(
		TEXT("animbp.set_transition_rule"),
		TEXT("Set the rule/condition for a transition between states"),
		OliveBlueprintSchemas::AnimBPSetTransitionRule(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleAnimBPSetTransitionRule),
		{TEXT("animbp"), TEXT("write"), TEXT("transition")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("animbp.set_transition_rule"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 4 AnimBP writer tools"));
}

void FOliveBlueprintToolHandlers::RegisterWidgetWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// widget.add_widget
	Registry.RegisterTool(
		TEXT("widget.add_widget"),
		TEXT("Add a widget to a Widget Blueprint"),
		OliveBlueprintSchemas::WidgetAddWidget(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleWidgetAddWidget),
		{TEXT("widget"), TEXT("write"), TEXT("umg")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("widget.add_widget"));

	// widget.remove_widget
	Registry.RegisterTool(
		TEXT("widget.remove_widget"),
		TEXT("Remove a widget from a Widget Blueprint"),
		OliveBlueprintSchemas::WidgetRemoveWidget(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleWidgetRemoveWidget),
		{TEXT("widget"), TEXT("write"), TEXT("umg")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("widget.remove_widget"));

	// widget.set_property
	Registry.RegisterTool(
		TEXT("widget.set_property"),
		TEXT("Set a property on a widget in a Widget Blueprint"),
		OliveBlueprintSchemas::WidgetSetProperty(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleWidgetSetProperty),
		{TEXT("widget"), TEXT("write"), TEXT("umg")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("widget.set_property"));

	// widget.bind_property
	Registry.RegisterTool(
		TEXT("widget.bind_property"),
		TEXT("Bind a widget property to a Blueprint function"),
		OliveBlueprintSchemas::WidgetBindProperty(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleWidgetBindProperty),
		{TEXT("widget"), TEXT("write"), TEXT("umg")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("widget.bind_property"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 4 Widget writer tools"));
}

// ============================================================================
// Reader Tool Handlers
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintRead(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRead: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' field")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRead: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path (e.g., '/Game/Blueprints/BP_Player')")
		);
	}

	// Extract mode (default to summary)
	FString Mode = TEXT("summary");
	Params->TryGetStringField(TEXT("mode"), Mode);

	// Validate mode
	if (Mode != TEXT("summary") && Mode != TEXT("full"))
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRead: Invalid mode '%s' for path='%s'"), *Mode, *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_VALUE"),
			FString::Printf(TEXT("Invalid mode '%s'. Must be 'summary' or 'full'"), *Mode),
			TEXT("Use 'summary' for structure only, or 'full' for complete graph data")
		);
	}

	// Resolve asset path (follows redirectors, checks existence)
	FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
	if (!ResolveInfo.IsSuccess())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRead: Asset not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to resolve asset at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}
	FString ResolvedPath = ResolveInfo.ResolvedPath;

	// Read Blueprint
	FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
	TOptional<FOliveIRBlueprint> Result;
	bool bAutoFull = false;

	if (Mode == TEXT("full"))
	{
		Result = Reader.ReadBlueprintFull(ResolvedPath);
	}
	else
	{
		// Auto-full-read for small Blueprints: count total nodes first.
		// If small enough, return full data to save follow-up read calls.
		static constexpr int32 AUTO_FULL_READ_NODE_THRESHOLD = 50;

		UBlueprint* Blueprint = Reader.LoadBlueprint(ResolvedPath);
		if (Blueprint)
		{
			int32 TotalNodeCount = 0;
			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				if (Graph) { TotalNodeCount += Graph->Nodes.Num(); }
			}
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph) { TotalNodeCount += Graph->Nodes.Num(); }
			}
			for (UEdGraph* Graph : Blueprint->MacroGraphs)
			{
				if (Graph) { TotalNodeCount += Graph->Nodes.Num(); }
			}

			if (TotalNodeCount <= AUTO_FULL_READ_NODE_THRESHOLD)
			{
				bAutoFull = true;
				UE_LOG(LogOliveBPTools, Log,
					TEXT("HandleBlueprintRead: auto-upgrade to full read (%d nodes <= %d threshold)"),
					TotalNodeCount, AUTO_FULL_READ_NODE_THRESHOLD);
			}
		}

		if (bAutoFull)
		{
			Result = Reader.ReadBlueprintFull(ResolvedPath);
		}
		else
		{
			Result = Reader.ReadBlueprintSummary(ResolvedPath);
		}
	}

	if (!Result.IsSet())
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to load Blueprint at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Serialize to JSON
	TSharedPtr<FJsonObject> ResultData = Result->ToJson();

	// Inject redirector info so callers know the canonical path was different
	if (!ResolveInfo.RedirectedFrom.IsEmpty() && ResultData.IsValid())
	{
		ResultData->SetStringField(TEXT("redirected_from"), ResolveInfo.RedirectedFrom);
	}

	// Annotate auto-full-read so the caller knows full data was included automatically
	if (bAutoFull && ResultData.IsValid())
	{
		ResultData->SetStringField(TEXT("read_mode"), TEXT("auto_full"));
		ResultData->SetStringField(TEXT("read_mode_note"),
			TEXT("Blueprint is small enough that full graph data was included automatically. "
				 "No need to call read_function or read_event_graph."));
	}

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintReadFunction(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadFunction: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadFunction: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract function_name
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadFunction: Missing required param 'function_name' for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'function_name' is missing or empty"),
			TEXT("Provide the name of the function to read")
		);
	}

	// Resolve asset path (follows redirectors, checks existence)
	FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
	if (!ResolveInfo.IsSuccess())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadFunction: Asset not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to resolve asset at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}
	FString ResolvedPath = ResolveInfo.ResolvedPath;

	// Load the Blueprint to access raw graph for large-graph detection
	FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
	UBlueprint* Blueprint = Reader.LoadBlueprint(ResolvedPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadFunction: Failed to load Blueprint at path='%s'"), *ResolvedPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to load Blueprint at path '%s'"), *ResolvedPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Find the function graph by name
	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == FunctionName)
		{
			TargetGraph = Graph;
			break;
		}
	}

	if (!TargetGraph)
	{
		return FOliveToolResult::Error(
			TEXT("GRAPH_NOT_FOUND"),
			FString::Printf(TEXT("Function '%s' not found in Blueprint '%s'"), *FunctionName, *AssetPath),
			TEXT("Verify the function name is correct and exists in the Blueprint")
		);
	}

	// Delegate to the paging-aware graph read helper
	TSharedPtr<FOliveGraphReader> GraphReader = Reader.GetGraphReader();
	return HandleGraphReadWithPaging(TargetGraph, Blueprint, GraphReader, Params, ResolveInfo);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintReadEventGraph(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadEventGraph: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadEventGraph: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract graph_name (optional, defaults to "EventGraph")
	FString GraphName = TEXT("EventGraph");
	Params->TryGetStringField(TEXT("graph_name"), GraphName);

	// Resolve asset path (follows redirectors, checks existence)
	FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
	if (!ResolveInfo.IsSuccess())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadEventGraph: Asset not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to resolve asset at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}
	FString ResolvedPath = ResolveInfo.ResolvedPath;

	// Load the Blueprint to access raw graph for large-graph detection
	FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
	UBlueprint* Blueprint = Reader.LoadBlueprint(ResolvedPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadEventGraph: Failed to load Blueprint at path='%s'"), *ResolvedPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to load Blueprint at path '%s'"), *ResolvedPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Find the event graph by name
	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			TargetGraph = Graph;
			break;
		}
	}

	// Fallback: if "EventGraph" was requested and not found by name, use first Ubergraph
	if (!TargetGraph && GraphName == TEXT("EventGraph") && Blueprint->UbergraphPages.Num() > 0)
	{
		TargetGraph = Blueprint->UbergraphPages[0];
	}

	if (!TargetGraph)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadEventGraph: Graph '%s' not found in Blueprint '%s'"), *GraphName, *AssetPath);
		return FOliveToolResult::Error(
			TEXT("GRAPH_NOT_FOUND"),
			FString::Printf(TEXT("Event graph '%s' not found in Blueprint '%s'"), *GraphName, *AssetPath),
			TEXT("Verify the graph name is correct. Most Blueprints have an 'EventGraph'")
		);
	}

	// Delegate to the paging-aware graph read helper
	TSharedPtr<FOliveGraphReader> GraphReader = Reader.GetGraphReader();
	return HandleGraphReadWithPaging(TargetGraph, Blueprint, GraphReader, Params, ResolveInfo);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintReadVariables(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadVariables: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintReadVariables: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Resolve asset path (follows redirectors, checks existence)
	FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
	if (!ResolveInfo.IsSuccess())
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to resolve asset at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}
	FString ResolvedPath = ResolveInfo.ResolvedPath;

	// Read variables
	FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
	TArray<FOliveIRVariable> Variables = Reader.ReadVariables(ResolvedPath);

	// Build result JSON
	TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());

	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	for (const FOliveIRVariable& Variable : Variables)
	{
		VariablesArray.Add(MakeShareable(new FJsonValueObject(Variable.ToJson())));
	}

	ResultData->SetArrayField(TEXT("variables"), VariablesArray);
	ResultData->SetNumberField(TEXT("count"), Variables.Num());

	// Inject redirector info so callers know the canonical path was different
	if (!ResolveInfo.RedirectedFrom.IsEmpty())
	{
		ResultData->SetStringField(TEXT("redirected_from"), ResolveInfo.RedirectedFrom);
	}

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintReadComponents(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Resolve asset path (follows redirectors, checks existence)
	FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
	if (!ResolveInfo.IsSuccess())
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to resolve asset at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}
	FString ResolvedPath = ResolveInfo.ResolvedPath;

	// Read components
	FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
	TArray<FOliveIRComponent> Components = Reader.ReadComponents(ResolvedPath);

	// Build result JSON
	TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());

	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	FString RootComponentName;

	for (const FOliveIRComponent& Component : Components)
	{
		ComponentsArray.Add(MakeShareable(new FJsonValueObject(Component.ToJson())));

		// Find root component
		if (Component.bIsRoot)
		{
			RootComponentName = Component.Name;
		}
	}

	ResultData->SetArrayField(TEXT("components"), ComponentsArray);
	ResultData->SetStringField(TEXT("root"), RootComponentName);
	ResultData->SetNumberField(TEXT("count"), Components.Num());

	// Inject redirector info so callers know the canonical path was different
	if (!ResolveInfo.RedirectedFrom.IsEmpty())
	{
		ResultData->SetStringField(TEXT("redirected_from"), ResolveInfo.RedirectedFrom);
	}

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintReadHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Resolve asset path (follows redirectors, checks existence)
	FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
	if (!ResolveInfo.IsSuccess())
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to resolve asset at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}
	FString ResolvedPath = ResolveInfo.ResolvedPath;

	// Read hierarchy
	FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
	TArray<FString> Hierarchy = Reader.ReadHierarchy(ResolvedPath);

	// Build result JSON
	TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());

	TArray<TSharedPtr<FJsonValue>> HierarchyArray;
	for (const FString& ClassName : Hierarchy)
	{
		HierarchyArray.Add(MakeShareable(new FJsonValueString(ClassName)));
	}

	ResultData->SetArrayField(TEXT("hierarchy"), HierarchyArray);
	ResultData->SetNumberField(TEXT("depth"), Hierarchy.Num());

	// Inject redirector info so callers know the canonical path was different
	if (!ResolveInfo.RedirectedFrom.IsEmpty())
	{
		ResultData->SetStringField(TEXT("redirected_from"), ResolveInfo.RedirectedFrom);
	}

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintListOverridableFunctions(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
	if (!ResolveInfo.IsSuccess())
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to resolve asset at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}
	const FString ResolvedPath = ResolveInfo.ResolvedPath;

	// Read overridable functions
	FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
	TArray<FOliveIRFunctionSignature> Functions = Reader.ReadOverridableFunctions(ResolvedPath);

	// Build result JSON
	TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());

	TArray<TSharedPtr<FJsonValue>> FunctionsArray;
	for (const FOliveIRFunctionSignature& Function : Functions)
	{
		FunctionsArray.Add(MakeShareable(new FJsonValueObject(Function.ToJson())));
	}

	ResultData->SetArrayField(TEXT("functions"), FunctionsArray);
	ResultData->SetNumberField(TEXT("count"), Functions.Num());
	if (!ResolveInfo.RedirectedFrom.IsEmpty())
	{
		ResultData->SetStringField(TEXT("redirected_from"), ResolveInfo.RedirectedFrom);
	}

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintGetNodePins(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object")
		);
	}

	// Parse required parameters
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph"), GraphName) || GraphName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'graph'"),
			TEXT("Provide the graph name (e.g., 'EventGraph' or function name)")
		);
	}

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'node_id'"),
			TEXT("Provide the node ID (e.g., 'node_0')")
		);
	}

	// Resolve asset path
	FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
	if (!ResolveInfo.IsSuccess())
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to resolve asset at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}
	const FString ResolvedPath = ResolveInfo.ResolvedPath;

	// Load Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ResolvedPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *ResolvedPath),
			TEXT("Verify the asset path is correct")
		);
	}

	// Find the node via GraphWriter cache
	FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();
	UEdGraphNode* Node = GraphWriter.GetCachedNode(ResolvedPath, NodeId);
	if (!Node)
	{
		return FOliveToolResult::Error(
			TEXT("NODE_NOT_FOUND"),
			FString::Printf(TEXT("Node '%s' not found in graph '%s'. "
				"Node IDs are assigned when nodes are created via add_node or apply_plan_json "
				"and are scoped to the graph."), *NodeId, *GraphName),
			TEXT("Use blueprint.read with mode:'full' to see all nodes in the graph")
		);
	}

	// Build pin manifest using existing anonymous namespace helper
	TSharedPtr<FJsonObject> PinsData = BuildPinManifest(Node);

	// Build result
	TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
	ResultData->SetStringField(TEXT("asset_path"), ResolvedPath);
	ResultData->SetStringField(TEXT("graph"), GraphName);
	ResultData->SetStringField(TEXT("node_id"), NodeId);
	ResultData->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	ResultData->SetObjectField(TEXT("pins"), PinsData);
	ResultData->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

	if (!ResolveInfo.RedirectedFrom.IsEmpty())
	{
		ResultData->SetStringField(TEXT("redirected_from"), ResolveInfo.RedirectedFrom);
	}

	return FOliveToolResult::Success(ResultData);
}

// ============================================================================
// Asset Writer Tool Handlers
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintCreate(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintCreate: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'parent_class' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintCreate: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path (e.g., '/Game/Blueprints/BP_NewActor')")
		);
	}

	// Defensive: detect folder-only paths before they reach the writer
	{
		FString ShortName = FPackageName::GetShortName(AssetPath);
		if (ShortName.IsEmpty() || AssetPath.EndsWith(TEXT("/")))
		{
			FOliveToolResult Result = FOliveToolResult::Error(
				TEXT("VALIDATION_PATH_IS_FOLDER"),
				FString::Printf(TEXT("'%s' is a folder path, not an asset path. The last segment must be the Blueprint name."), *AssetPath),
				FString::Printf(TEXT("Append a Blueprint name like '%s/BP_MyBlueprint'. Example: /Game/Blueprints/BP_Gun"), *AssetPath)
			);
			if (Result.Data.IsValid())
			{
				Result.Data->SetStringField(TEXT("self_correction_hint"),
					FString::Printf(TEXT("You used '%s' which is a folder. Add the BP name: '%s/BP_MyBlueprint'. Then retry blueprint.create."),
						*AssetPath, *AssetPath));
			}
			return Result;
		}

		// Also catch paths missing /Game/ prefix
		if (!AssetPath.StartsWith(TEXT("/Game/")))
		{
			FOliveToolResult Result = FOliveToolResult::Error(
				TEXT("VALIDATION_INVALID_PATH_PREFIX"),
				FString::Printf(TEXT("Path '%s' must start with '/Game/'."), *AssetPath),
				FString::Printf(TEXT("Use '/Game/Blueprints/%s' or another path under /Game/"), *ShortName)
			);
			if (Result.Data.IsValid())
			{
				Result.Data->SetStringField(TEXT("self_correction_hint"),
					FString::Printf(TEXT("Paths must start with /Game/. Try: /Game/Blueprints/%s"), *ShortName));
			}
			return Result;
		}
	}

	// Extract parent_class
	FString ParentClass;
	if (!Params->TryGetStringField(TEXT("parent_class"), ParentClass) || ParentClass.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'parent_class' is missing or empty"),
			TEXT("Provide the parent class name (e.g., 'Actor', 'Character', '/Game/BP_Base')")
		);
	}

	// Extract type (optional, defaults to Normal)
	FString TypeString = TEXT("Normal");
	Params->TryGetStringField(TEXT("type"), TypeString);

	// Parse Blueprint type (case-insensitive, lenient)
	EOliveBlueprintType BlueprintType = EOliveBlueprintType::Normal;
	FString TypeUpper = TypeString.ToUpper();
	if (TypeUpper == TEXT("NORMAL") || TypeUpper == TEXT("BLUEPRINT") || TypeUpper.IsEmpty())
	{
		BlueprintType = EOliveBlueprintType::Normal;
	}
	else if (TypeUpper == TEXT("INTERFACE"))
	{
		BlueprintType = EOliveBlueprintType::Interface;
	}
	else if (TypeUpper == TEXT("FUNCTIONLIBRARY") || TypeUpper == TEXT("FUNCTION_LIBRARY"))
	{
		BlueprintType = EOliveBlueprintType::FunctionLibrary;
	}
	else if (TypeUpper == TEXT("MACROLIBRARY") || TypeUpper == TEXT("MACRO_LIBRARY"))
	{
		BlueprintType = EOliveBlueprintType::MacroLibrary;
	}
	else if (TypeUpper == TEXT("ANIMATIONBLUEPRINT") || TypeUpper == TEXT("ANIMATION_BLUEPRINT") || TypeUpper == TEXT("ANIM_BLUEPRINT"))
	{
		BlueprintType = EOliveBlueprintType::AnimationBlueprint;
	}
	else if (TypeUpper == TEXT("WIDGETBLUEPRINT") || TypeUpper == TEXT("WIDGET_BLUEPRINT") || TypeUpper == TEXT("UMG") || TypeUpper == TEXT("UI"))
	{
		BlueprintType = EOliveBlueprintType::WidgetBlueprint;
	}
	else
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("blueprint.create: Unknown type '%s', defaulting to Normal. "
			"The 'type' field is the Blueprint subtype (Normal, Interface, FunctionLibrary), "
			"not the parent class. Use 'parent_class' for Actor/Pawn/Character."), *TypeString);
		BlueprintType = EOliveBlueprintType::Normal;
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.create");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = nullptr; // Create operation has no target
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Create Blueprint '%s'"), *AssetPath)
	);
	Request.OperationCategory = TEXT("create");
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP(); // MCP calls skip confirmation
	Request.bAutoCompile = false; // No need to compile new empty Blueprint
	Request.bSkipVerification = false;

	// Create executor that calls FOliveBlueprintWriter
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, ParentClass, BlueprintType](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.CreateBlueprint(AssetPath, ParentClass, BlueprintType);

		if (!WriteResult.bSuccess)
		{
			// Convert errors to FOliveWriteResult format
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			UE_LOG(LogOliveBPTools, Error, TEXT("blueprint.create failed: path='%s' parent='%s' type=%d error='%s'"),
				*AssetPath, *ParentClass, static_cast<int32>(BlueprintType), *ErrorMsg);
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_CREATE_FAILED"),
				ErrorMsg,
				TEXT("Verify the parent class exists and the asset path is valid")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("parent_class"), ParentClass);
		ResultData->SetStringField(TEXT("type"), FOliveBlueprintTypeDetector::TypeToString(BlueprintType));

		FOliveWriteResult Result = FOliveWriteResult::Success(ResultData);
		Result.CreatedItem = AssetPath;
		return Result;
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintSetParentClass(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintSetParentClass: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'new_parent' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintSetParentClass: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract new_parent
	FString NewParent;
	if (!Params->TryGetStringField(TEXT("new_parent"), NewParent) || NewParent.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintSetParentClass: Missing required param 'new_parent' for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'new_parent' is missing or empty"),
			TEXT("Provide the new parent class name or path")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintSetParentClass: Blueprint not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.set_parent_class");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Change parent class of '%s' to '%s'"), *AssetPath, *NewParent)
	);
	Request.OperationCategory = TEXT("refactoring"); // Tier 3 - destructive
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true; // Need to compile after reparenting
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, NewParent](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.SetParentClass(AssetPath, NewParent);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_REPARENT_FAILED"),
				ErrorMsg,
				TEXT("Verify the new parent class exists and is compatible")
			);
		}

		// Success
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("new_parent"), NewParent);

		// Include compile warnings if any
		if (WriteResult.CompileErrors.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ErrorsArray;
			for (const FString& CompileError : WriteResult.CompileErrors)
			{
				ErrorsArray.Add(MakeShareable(new FJsonValueString(CompileError)));
			}
			ResultData->SetArrayField(TEXT("compile_errors"), ErrorsArray);
		}

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintAddInterface(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'interface' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract interface
	FString InterfacePath;
	if (!Params->TryGetStringField(TEXT("interface"), InterfacePath) || InterfacePath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'interface' is missing or empty"),
			TEXT("Provide the interface name or path (e.g., 'BPI_Interactable' or '/Game/Interfaces/BPI_Interactable')")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_interface");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add interface '%s' to '%s'"), *InterfacePath, *AssetPath)
	);
	Request.OperationCategory = TEXT("interface"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, InterfacePath](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddInterface(AssetPath, InterfacePath);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_ADD_INTERFACE_FAILED"),
				ErrorMsg,
				TEXT("Verify the interface exists and is not already implemented")
			);
		}

		// Success
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("interface"), InterfacePath);

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintRemoveInterface(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'interface' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract interface
	FString InterfacePath;
	if (!Params->TryGetStringField(TEXT("interface"), InterfacePath) || InterfacePath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'interface' is missing or empty"),
			TEXT("Provide the interface name or path to remove")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.remove_interface");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Remove interface '%s' from '%s'"), *InterfacePath, *AssetPath)
	);
	Request.OperationCategory = TEXT("interface"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, InterfacePath](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.RemoveInterface(AssetPath, InterfacePath);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_REMOVE_INTERFACE_FAILED"),
				ErrorMsg,
				TEXT("Verify the interface is currently implemented by this Blueprint")
			);
		}

		// Success
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("interface"), InterfacePath);

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintCompile(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintCompile: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' field")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintCompile: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintCompile: Blueprint not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Compile directly using FOliveCompileManager (no write pipeline needed)
	FOliveCompileManager& CompileManager = FOliveCompileManager::Get();
	FOliveIRCompileResult CompileResult = CompileManager.Compile(Blueprint);

	// Build result data
	TSharedPtr<FJsonObject> ResultData = CompileResult.ToJson();

	// Return success even if compilation had errors (the errors are in the result)
	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintDelete(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintDelete: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' field")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintDelete: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path to delete")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintDelete: Blueprint not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.delete");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Delete Blueprint '%s'"), *AssetPath)
	);
	Request.OperationCategory = TEXT("delete"); // Tier 3 - destructive
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false; // No compilation needed for delete
	Request.bSkipVerification = true; // No verification possible after delete

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.DeleteBlueprint(AssetPath);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_DELETE_FAILED"),
				ErrorMsg,
				TEXT("Ensure the Blueprint is not currently open in an editor and has no references")
			);
		}

		// Success
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("deleted_path"), AssetPath);

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

// ============================================================================
// Variable Writer Tool Handler Stubs
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintAddVariable(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddVariable: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'variable' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddVariable: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract variable object — accept both nested and flat formats
	const TSharedPtr<FJsonObject>* VariableJsonPtr;
	TSharedPtr<FJsonObject> WrappedVariable; // Prevent dangling — must outlive VariableJsonPtr
	if (!Params->TryGetObjectField(TEXT("variable"), VariableJsonPtr) || !(*VariableJsonPtr).IsValid())
	{
		// Fallback: check if flat format was used (name/type at root level)
		FString FlatName;
		if (Params->TryGetStringField(TEXT("name"), FlatName) && !FlatName.IsEmpty())
		{
			WrappedVariable = MakeShareable(new FJsonObject());
			WrappedVariable->SetStringField(TEXT("name"), FlatName);

			// Handle type: could be string ("Float") or object ({"category":"Float"})
			const TSharedPtr<FJsonObject>* TypeObjPtr;
			FString FlatType;
			if (Params->TryGetObjectField(TEXT("type"), TypeObjPtr))
			{
				WrappedVariable->SetObjectField(TEXT("type"), *TypeObjPtr);
			}
			else if (Params->TryGetStringField(TEXT("type"), FlatType))
			{
				TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject());
				TypeObj->SetStringField(TEXT("category"), FlatType);
				WrappedVariable->SetObjectField(TEXT("type"), TypeObj);
			}

			// Copy optional fields
			FString Val;
			if (Params->TryGetStringField(TEXT("default_value"), Val))
				WrappedVariable->SetStringField(TEXT("default_value"), Val);
			if (Params->TryGetStringField(TEXT("category"), Val))
				WrappedVariable->SetStringField(TEXT("category"), Val);

			UE_LOG(LogOliveBPTools, Log, TEXT("add_variable: Accepted flat format, wrapped into variable object"));
			VariableJsonPtr = &WrappedVariable;
		}
		else
		{
			UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddVariable: Missing required param 'variable' for path='%s'"), *AssetPath);
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				TEXT("Required parameter 'variable' is missing or invalid"),
				TEXT("Provide a variable specification with name and type")
			);
		}
	}

	// Parse variable from JSON to IR
	FOliveIRVariable Variable = ParseVariableFromParams(*VariableJsonPtr);

	// Validate variable has required fields
	if (Variable.Name.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddVariable: Variable 'name' field is empty for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_FIELD"),
			TEXT("Variable 'name' field is required"),
			TEXT("Provide a name for the variable")
		);
	}

	if (Variable.Type.Category == EOliveIRTypeCategory::Unknown)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddVariable: Variable 'type' is Unknown for variable='%s' path='%s'"), *Variable.Name, *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_FIELD"),
			TEXT("Variable 'type' field is required and must be valid"),
			TEXT("Provide a valid type specification with a category")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddVariable: Blueprint not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_variable");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add variable '%s' to '%s'"), *Variable.Name, *AssetPath)
	);
	Request.OperationCategory = TEXT("variable"); // Tier 1
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false; // No need to compile for variable addition
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, Variable](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddVariable(AssetPath, Variable);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_ADD_VARIABLE_FAILED"),
				ErrorMsg,
				TEXT("Verify the variable name is unique and the type is valid")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("variable_name"), Variable.Name);
		ResultData->SetStringField(TEXT("variable_type"), Variable.Type.GetDisplayName());

		FOliveWriteResult Result = FOliveWriteResult::Success(ResultData);
		Result.CreatedItem = Variable.Name;
		return Result;
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintRemoveVariable(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRemoveVariable: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'name' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRemoveVariable: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract variable name
	FString VariableName;
	if (!Params->TryGetStringField(TEXT("name"), VariableName) || VariableName.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRemoveVariable: Missing required param 'name' for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide the name of the variable to remove")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRemoveVariable: Blueprint not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.remove_variable");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Remove variable '%s' from '%s'"), *VariableName, *AssetPath)
	);
	Request.OperationCategory = TEXT("variable"); // Tier 2 for removal
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, VariableName](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.RemoveVariable(AssetPath, VariableName);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_REMOVE_VARIABLE_FAILED"),
				ErrorMsg,
				TEXT("Verify the variable exists in the Blueprint")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("removed_variable"), VariableName);

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintModifyVariable(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path', 'name', and 'changes' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract variable name
	FString VariableName;
	if (!Params->TryGetStringField(TEXT("name"), VariableName) || VariableName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide the name of the variable to modify")
		);
	}

	// Extract changes object
	const TSharedPtr<FJsonObject>* ChangesJsonPtr;
	if (!Params->TryGetObjectField(TEXT("changes"), ChangesJsonPtr) || !ChangesJsonPtr->IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'changes' is missing or invalid"),
			TEXT("Provide a changes object with properties to modify")
		);
	}

	TSharedPtr<FJsonObject> ChangesJson = *ChangesJsonPtr;

	// Parse changes into a map
	TMap<FString, FString> Modifications;

	// Extract all possible change fields
	FString TempValue;
	bool TempBool;

	if (ChangesJson->TryGetStringField(TEXT("default_value"), TempValue))
	{
		Modifications.Add(TEXT("DefaultValue"), TempValue);
	}

	if (ChangesJson->TryGetStringField(TEXT("category"), TempValue))
	{
		Modifications.Add(TEXT("Category"), TempValue);
	}

	if (ChangesJson->TryGetStringField(TEXT("description"), TempValue))
	{
		Modifications.Add(TEXT("Description"), TempValue);
	}

	if (ChangesJson->TryGetBoolField(TEXT("blueprint_read_write"), TempBool))
	{
		Modifications.Add(TEXT("bBlueprintReadWrite"), TempBool ? TEXT("true") : TEXT("false"));
	}

	if (ChangesJson->TryGetBoolField(TEXT("expose_on_spawn"), TempBool))
	{
		Modifications.Add(TEXT("bExposeOnSpawn"), TempBool ? TEXT("true") : TEXT("false"));
	}

	if (ChangesJson->TryGetBoolField(TEXT("replicated"), TempBool))
	{
		Modifications.Add(TEXT("bReplicated"), TempBool ? TEXT("true") : TEXT("false"));
	}

	if (ChangesJson->TryGetBoolField(TEXT("save_game"), TempBool))
	{
		Modifications.Add(TEXT("bSaveGame"), TempBool ? TEXT("true") : TEXT("false"));
	}

	if (ChangesJson->TryGetBoolField(TEXT("edit_anywhere"), TempBool))
	{
		Modifications.Add(TEXT("bEditAnywhere"), TempBool ? TEXT("true") : TEXT("false"));
	}

	if (ChangesJson->TryGetBoolField(TEXT("blueprint_visible"), TempBool))
	{
		Modifications.Add(TEXT("bBlueprintVisible"), TempBool ? TEXT("true") : TEXT("false"));
	}

	if (Modifications.Num() == 0)
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_NO_CHANGES"),
			TEXT("No valid modifications found in 'changes' object"),
			TEXT("Provide at least one property to modify (e.g., default_value, category, description, flags)")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.modify_variable");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Modify variable '%s' in '%s'"), *VariableName, *AssetPath)
	);
	Request.OperationCategory = TEXT("variable"); // Tier 1
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, VariableName, Modifications](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.ModifyVariable(AssetPath, VariableName, Modifications);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_MODIFY_VARIABLE_FAILED"),
				ErrorMsg,
				TEXT("Verify the variable exists and the modifications are valid")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("variable_name"), VariableName);
		ResultData->SetNumberField(TEXT("modified_properties_count"), Modifications.Num());

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

// ============================================================================
// Component Writer Tool Handler Stubs
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintAddComponent(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddComponent: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'class' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddComponent: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract component class
	FString ComponentClass;
	if (!Params->TryGetStringField(TEXT("class"), ComponentClass) || ComponentClass.IsEmpty())
	{
		// Fallback: accept "component_class" alias
		Params->TryGetStringField(TEXT("component_class"), ComponentClass);
	}
	if (ComponentClass.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'class' is missing or empty"),
			TEXT("Provide the component class name (e.g., 'UStaticMeshComponent', 'USkeletalMeshComponent')")
		);
	}

	// Extract optional parameters
	FString ComponentName;
	Params->TryGetStringField(TEXT("name"), ComponentName);
	if (ComponentName.IsEmpty())
	{
		Params->TryGetStringField(TEXT("component_name"), ComponentName);
	}

	FString ParentName;
	Params->TryGetStringField(TEXT("parent"), ParentName);
	if (ParentName.IsEmpty())
	{
		Params->TryGetStringField(TEXT("parent_component"), ParentName);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_component");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add component '%s' to '%s'"), *ComponentClass, *AssetPath)
	);
	Request.OperationCategory = TEXT("component"); // Tier 1
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, ComponentClass, ComponentName, ParentName](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveComponentWriter& Writer = FOliveComponentWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddComponent(AssetPath, ComponentClass, ComponentName, ParentName);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_ADD_COMPONENT_FAILED"),
				ErrorMsg,
				TEXT("Verify the component class exists and the parent component (if specified) supports children")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("component_name"), WriteResult.CreatedItemName);
		ResultData->SetStringField(TEXT("component_class"), ComponentClass);
		if (!ParentName.IsEmpty())
		{
			ResultData->SetStringField(TEXT("parent_component"), ParentName);
		}

		FOliveWriteResult Result = FOliveWriteResult::Success(ResultData);
		Result.CreatedItem = WriteResult.CreatedItemName;
		return Result;
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintRemoveComponent(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'name' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract component name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("name"), ComponentName) || ComponentName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide the name of the component to remove")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.remove_component");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Remove component '%s' from '%s'"), *ComponentName, *AssetPath)
	);
	Request.OperationCategory = TEXT("component"); // Tier 2 for removal
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, ComponentName](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveComponentWriter& Writer = FOliveComponentWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.RemoveComponent(AssetPath, ComponentName);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_REMOVE_COMPONENT_FAILED"),
				ErrorMsg,
				TEXT("Verify the component exists in the Blueprint")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("removed_component"), ComponentName);

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintModifyComponent(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path', 'name', and 'properties' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract component name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("name"), ComponentName) || ComponentName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide the name of the component to modify")
		);
	}

	// Extract properties object
	const TSharedPtr<FJsonObject>* PropertiesJsonPtr;
	if (!Params->TryGetObjectField(TEXT("properties"), PropertiesJsonPtr) || !PropertiesJsonPtr->IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'properties' is missing or invalid"),
			TEXT("Provide a properties object with component properties to modify")
		);
	}

	TSharedPtr<FJsonObject> PropertiesJson = *PropertiesJsonPtr;

	// Convert properties object to TMap<FString, FString>
	TMap<FString, FString> Properties;
	for (const auto& Property : PropertiesJson->Values)
	{
		FString Value;
		if (Property.Value->Type == EJson::String)
		{
			Value = Property.Value->AsString();
		}
		else if (Property.Value->Type == EJson::Number)
		{
			Value = FString::Printf(TEXT("%g"), Property.Value->AsNumber());
		}
		else if (Property.Value->Type == EJson::Boolean)
		{
			Value = Property.Value->AsBool() ? TEXT("true") : TEXT("false");
		}
		else if (Property.Value->Type == EJson::Object)
		{
			// For complex types like vectors, serialize to string
			FString JsonString;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
			FJsonSerializer::Serialize(Property.Value->AsObject().ToSharedRef(), Writer);
			Value = JsonString;
		}
		else
		{
			continue; // Skip unsupported types
		}

		Properties.Add(Property.Key, Value);
	}

	if (Properties.Num() == 0)
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_NO_PROPERTIES"),
			TEXT("No valid properties found in 'properties' object"),
			TEXT("Provide at least one property to modify (e.g., RelativeLocation, RelativeRotation, bVisible)")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.modify_component");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Modify component '%s' in '%s'"), *ComponentName, *AssetPath)
	);
	Request.OperationCategory = TEXT("component"); // Tier 1 for property modification
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, ComponentName, Properties](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveComponentWriter& Writer = FOliveComponentWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.ModifyComponent(AssetPath, ComponentName, Properties);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_MODIFY_COMPONENT_FAILED"),
				ErrorMsg,
				TEXT("Verify the component exists and the property names are valid for this component type")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("component_name"), ComponentName);
		ResultData->SetNumberField(TEXT("modified_properties_count"), Properties.Num());

		// Include warnings if any properties failed
		if (WriteResult.Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningsArray;
			for (const FString& Warning : WriteResult.Warnings)
			{
				WarningsArray.Add(MakeShareable(new FJsonValueString(Warning)));
			}
			ResultData->SetArrayField(TEXT("warnings"), WarningsArray);
		}

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintReparentComponent(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path', 'name', and 'new_parent' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract component name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("name"), ComponentName) || ComponentName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide the name of the component to reparent")
		);
	}

	// Extract new parent name
	FString NewParentName;
	if (!Params->TryGetStringField(TEXT("new_parent"), NewParentName))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'new_parent' is missing"),
			TEXT("Provide the new parent component name (use empty string for root)")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.reparent_component");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Reparent component '%s' to '%s' in '%s'"),
			*ComponentName,
			NewParentName.IsEmpty() ? TEXT("(root)") : *NewParentName,
			*AssetPath)
	);
	Request.OperationCategory = TEXT("component"); // Tier 2 for reparenting
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, ComponentName, NewParentName](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveComponentWriter& Writer = FOliveComponentWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.ReparentComponent(AssetPath, ComponentName, NewParentName);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_REPARENT_COMPONENT_FAILED"),
				ErrorMsg,
				TEXT("Verify both components exist and the new parent is a SceneComponent that can have children")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("component_name"), ComponentName);
		ResultData->SetStringField(TEXT("new_parent"), NewParentName.IsEmpty() ? TEXT("(root)") : NewParentName);

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

// ============================================================================
// Function Writer Tool Handler Stubs
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintAddFunction(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddFunction: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'signature' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddFunction: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract signature object
	const TSharedPtr<FJsonObject>* SignatureJsonPtr;
	if (!Params->TryGetObjectField(TEXT("signature"), SignatureJsonPtr) || !SignatureJsonPtr->IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddFunction: Missing required param 'signature' for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'signature' is missing or invalid"),
			TEXT("Provide a function signature with name, inputs, and outputs")
		);
	}

	// Parse function signature from JSON to IR
	FOliveIRFunctionSignature Signature = ParseFunctionSignatureFromParams(*SignatureJsonPtr);

	// Validate signature has required fields
	if (Signature.Name.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddFunction: Function 'name' field is empty for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_FIELD"),
			TEXT("Function 'name' field is required"),
			TEXT("Provide a name for the function")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddFunction: Blueprint not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_function");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add function '%s' to '%s'"), *Signature.Name, *AssetPath)
	);
	Request.OperationCategory = TEXT("function_creation"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, Signature](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddFunction(AssetPath, Signature);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_ADD_FUNCTION_FAILED"),
				ErrorMsg,
				TEXT("Verify the function name is unique and the signature is valid")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("function_name"), WriteResult.CreatedItemName);
		ResultData->SetStringField(TEXT("message"), FString::Printf(
			TEXT("Successfully added function '%s'. "
				 "NEXT STEP: Call olive.get_recipe for this function, then "
				 "blueprint.preview_plan_json + blueprint.apply_plan_json to implement it. "
				 "Do NOT add another function until this one has graph logic."),
			*WriteResult.CreatedItemName));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintRemoveFunction(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRemoveFunction: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'name' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRemoveFunction: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract function name
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("name"), FunctionName) || FunctionName.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRemoveFunction: Missing required param 'name' for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide the name of the function to remove")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRemoveFunction: Blueprint not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Guard: block removal if the function graph has significant logic unless 'force': true is passed
	{
		bool bForce = false;
		Params->TryGetBoolField(TEXT("force"), bForce);

		if (!bForce)
		{
			for (UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
			{
				if (FuncGraph && FuncGraph->GetFName() == FName(*FunctionName))
				{
					const int32 NodeCount = FuncGraph->Nodes.Num();
					if (NodeCount > 2) // Entry node + return node = 2; anything beyond that is real logic
					{
						UE_LOG(LogOliveBPTools, Warning,
							TEXT("HandleBlueprintRemoveFunction: Blocked removal of '%s' — has %d nodes of graph logic"),
							*FunctionName, NodeCount);
						return FOliveToolResult::Error(
							TEXT("GUARD_FUNCTION_HAS_LOGIC"),
							FString::Printf(
								TEXT("Function '%s' has %d nodes of working graph logic. "
									"Removing it will destroy existing work. To modify an existing function, "
									"use blueprint.preview_plan_json + blueprint.apply_plan_json on the existing "
									"function graph instead of removing and recreating it."),
								*FunctionName, NodeCount),
							TEXT("Use blueprint.preview_plan_json to modify the existing function, "
								"or pass 'force': true to override this safety check")
						);
					}
					break;
				}
			}
		}
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.remove_function");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Remove function '%s' from '%s'"), *FunctionName, *AssetPath)
	);
	Request.OperationCategory = TEXT("function_creation"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, FunctionName](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.RemoveFunction(AssetPath, FunctionName);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_REMOVE_FUNCTION_FAILED"),
				ErrorMsg,
				TEXT("Verify the function exists and is not an overridden parent function")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("function_name"), FunctionName);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully removed function '%s'"), *FunctionName));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintModifyFunctionSignature(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path', 'name', and 'changes' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract function name
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("name"), FunctionName) || FunctionName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide the name of the function to modify")
		);
	}

	// Extract changes object
	const TSharedPtr<FJsonObject>* ChangesJsonPtr;
	if (!Params->TryGetObjectField(TEXT("changes"), ChangesJsonPtr) || !ChangesJsonPtr->IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'changes' is missing or invalid"),
			TEXT("Provide a changes object with signature modifications")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.modify_function_signature");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Modify function signature for '%s' in '%s'"), *FunctionName, *AssetPath)
	);
	Request.OperationCategory = TEXT("function_creation"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, FunctionName, ChangesJsonPtr](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		// DESIGN NOTE: Modifying function signature is complex and requires:
		// 1. Removing the existing function
		// 2. Creating a new function with the modified signature
		// 3. Attempting to preserve graph nodes where possible
		// For Phase 1B, we implement a simplified approach: remove and recreate
		// A more sophisticated implementation would be added in Phase 2

		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();

		// First, read the existing function to get base signature
		FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
		TOptional<FOliveIRGraph> ExistingGraph = Reader.ReadFunctionGraph(AssetPath, FunctionName);

		if (!ExistingGraph.IsSet())
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_FUNCTION_NOT_FOUND"),
				FString::Printf(TEXT("Function '%s' not found in Blueprint"), *FunctionName),
				TEXT("Verify the function name is correct")
			);
		}

		// For now, return a not-fully-implemented warning
		// Full implementation would parse changes and rebuild signature
		return FOliveWriteResult::ExecutionError(
			TEXT("BP_MODIFY_SIGNATURE_LIMITED"),
			TEXT("Modifying function signatures requires recreating the function"),
			TEXT("Use blueprint.remove_function followed by blueprint.add_function with new signature for now")
		);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintAddEventDispatcher(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'name' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract dispatcher name
	FString DispatcherName;
	if (!Params->TryGetStringField(TEXT("name"), DispatcherName) || DispatcherName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide the name of the event dispatcher to create")
		);
	}

	// Extract optional params array
	TArray<FOliveIRFunctionParam> DispatcherParams;
	const TArray<TSharedPtr<FJsonValue>>* ParamsArray;
	if (Params->TryGetArrayField(TEXT("params"), ParamsArray))
	{
		for (const TSharedPtr<FJsonValue>& ParamValue : *ParamsArray)
		{
			if (ParamValue->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> ParamObj = ParamValue->AsObject();
				FOliveIRFunctionParam Param;

				ParamObj->TryGetStringField(TEXT("name"), Param.Name);

				// Parse type
				const TSharedPtr<FJsonObject>* TypeJsonPtr;
				if (ParamObj->TryGetObjectField(TEXT("type"), TypeJsonPtr))
				{
					Param.Type = ParseTypeFromParams(*TypeJsonPtr);
				}

				DispatcherParams.Add(Param);
			}
		}
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_event_dispatcher");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add event dispatcher '%s' to '%s'"), *DispatcherName, *AssetPath)
	);
	Request.OperationCategory = TEXT("variable"); // Tier 1 - dispatchers are like variables
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, DispatcherName, DispatcherParams](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddEventDispatcher(AssetPath, DispatcherName, DispatcherParams);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_ADD_DISPATCHER_FAILED"),
				ErrorMsg,
				TEXT("Verify the dispatcher name is unique")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("dispatcher_name"), WriteResult.CreatedItemName);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully added event dispatcher '%s'"), *WriteResult.CreatedItemName));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintOverrideFunction(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintOverrideFunction: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'function_name' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintOverrideFunction: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract function name
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintOverrideFunction: Missing required param 'function_name' for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'function_name' is missing or empty"),
			TEXT("Provide the name of the parent function to override")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintOverrideFunction: Blueprint not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.override_function");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Override function '%s' in '%s'"), *FunctionName, *AssetPath)
	);
	Request.OperationCategory = TEXT("function_creation"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, FunctionName](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.OverrideFunction(AssetPath, FunctionName);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_OVERRIDE_FUNCTION_FAILED"),
				ErrorMsg,
				TEXT("Verify the function exists in a parent class and is overridable")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("function_name"), WriteResult.CreatedItemName);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully overridden function '%s'"), *WriteResult.CreatedItemName));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintAddCustomEvent(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'name' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// Extract event name
	FString EventName;
	if (!Params->TryGetStringField(TEXT("name"), EventName) || EventName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide the name of the custom event to create")
		);
	}

	// Extract optional params array
	TArray<FOliveIRFunctionParam> EventParams;
	const TArray<TSharedPtr<FJsonValue>>* ParamsArray;
	if (Params->TryGetArrayField(TEXT("params"), ParamsArray))
	{
		for (const TSharedPtr<FJsonValue>& ParamValue : *ParamsArray)
		{
			if (ParamValue->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> ParamObj = ParamValue->AsObject();
				FOliveIRFunctionParam Param;

				ParamObj->TryGetStringField(TEXT("name"), Param.Name);

				// Parse type
				const TSharedPtr<FJsonObject>* TypeJsonPtr;
				if (ParamObj->TryGetObjectField(TEXT("type"), TypeJsonPtr))
				{
					Param.Type = ParseTypeFromParams(*TypeJsonPtr);
				}

				EventParams.Add(Param);
			}
		}
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_custom_event");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add custom event '%s' to '%s'"), *EventName, *AssetPath)
	);
	Request.OperationCategory = TEXT("variable"); // Tier 1 - custom events are low risk
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, EventName, EventParams](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddCustomEvent(AssetPath, EventName, EventParams);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_ADD_CUSTOM_EVENT_FAILED"),
				ErrorMsg,
				TEXT("Verify the event name is unique")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("event_name"), WriteResult.CreatedItemName);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully added custom event '%s'"), *WriteResult.CreatedItemName));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

// ============================================================================
// Graph Writer Tool Handlers
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintAddNode(const TSharedPtr<FJsonObject>& Params)
{
	// Parse required parameters
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath))
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddNode: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph"), GraphName))
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddNode: Missing required param 'graph' for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'graph'"),
			TEXT("Provide the graph name (e.g., 'EventGraph' or function name)")
		);
	}

	FString NodeType;
	if (!Params->TryGetStringField(TEXT("type"), NodeType))
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddNode: Missing required param 'type' for path='%s' graph='%s'"), *AssetPath, *GraphName);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'type'"),
			TEXT("Provide the node type (e.g., 'CallFunction', 'Branch', 'VariableGet')")
		);
	}

	// Parse optional properties
	TMap<FString, FString> NodeProperties;
	const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropertiesPtr) && PropertiesPtr->IsValid())
	{
		for (const auto& Pair : (*PropertiesPtr)->Values)
		{
			FString ValueStr;
			if (Pair.Value->TryGetString(ValueStr))
			{
				NodeProperties.Add(Pair.Key, ValueStr);
			}
		}
	}

	// Parse optional position
	int32 PosX = 0;
	int32 PosY = 0;
	Params->TryGetNumberField(TEXT("pos_x"), PosX);
	Params->TryGetNumberField(TEXT("pos_y"), PosY);

	// --- Pre-pipeline validation: check node type before loading Blueprint ---
	// This is cheap (no asset load) and gives the agent actionable suggestions.
	FOliveNodeFactory& Factory = FOliveNodeFactory::Get();
	if (!Factory.ValidateNodeType(NodeType, NodeProperties))
	{
		// Query catalog for fuzzy suggestions
		FOliveNodeCatalog& Catalog = FOliveNodeCatalog::Get();
		TArray<FOliveNodeSuggestion> Suggestions = Catalog.FuzzyMatch(NodeType, 5);

		TSharedPtr<FJsonObject> ErrorData = MakeShareable(new FJsonObject());
		ErrorData->SetStringField(TEXT("requested_type"), NodeType);

		TArray<TSharedPtr<FJsonValue>> SuggestionArray;
		for (const FOliveNodeSuggestion& S : Suggestions)
		{
			TSharedPtr<FJsonObject> SugObj = MakeShareable(new FJsonObject());
			SugObj->SetStringField(TEXT("type_id"), S.TypeId);
			SugObj->SetStringField(TEXT("display_name"), S.DisplayName);
			SuggestionArray.Add(MakeShareable(new FJsonValueObject(SugObj)));
		}
		ErrorData->SetArrayField(TEXT("suggestions"), SuggestionArray);

		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("NODE_TYPE_UNKNOWN"),
			FString::Printf(TEXT("Node type '%s' is not recognized"), *NodeType),
			Suggestions.Num() > 0
				? FString::Printf(TEXT("Did you mean '%s'?"), *Suggestions[0].DisplayName)
				: TEXT("Use blueprint.node_catalog_search to find available node types")
		);
		Result.Data = ErrorData;
		return Result;
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Pre-pipeline check: prevent duplicate native event overrides.
	// Native events like ReceiveBeginPlay, ReceiveTick can only exist once per Blueprint.
	// Catching this before the write pipeline avoids opening a transaction just to reject.
	if (NodeType == OliveNodeTypes::Event && Blueprint->ParentClass)
	{
		FString EventNameStr;
		const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("properties"), PropsObjPtr) && PropsObjPtr->IsValid())
		{
			(*PropsObjPtr)->TryGetStringField(TEXT("event_name"), EventNameStr);
		}

		if (!EventNameStr.IsEmpty())
		{
			FName EventFName(*EventNameStr);
			UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(
				Blueprint, Blueprint->ParentClass, EventFName);

			if (ExistingEvent)
			{
				return FOliveToolResult::Error(
					TEXT("DUPLICATE_NATIVE_EVENT"),
					FString::Printf(TEXT("Native event '%s' already exists at position (%d, %d). "
						"Each native event can only appear once per Blueprint."),
						*EventNameStr, ExistingEvent->NodePosX, ExistingEvent->NodePosY),
					TEXT("Use blueprint.read_event_graph to see existing event nodes, "
						 "or use 'CustomEvent' type to create user-defined events")
				);
			}
		}
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_node");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add node '%s' to graph '%s' in '%s'"), *NodeType, *GraphName, *AssetPath)
	);
	Request.OperationCategory = TEXT("graph_editing"); // Tier 2 - graph editing requires planning
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, GraphName, NodeType, NodeProperties, PosX, PosY](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();
		FOliveBlueprintWriteResult WriteResult = GraphWriter.AddNode(AssetPath, GraphName, NodeType, NodeProperties, PosX, PosY);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");

			// Defense-in-depth: detect duplicate native event from NodeFactory
			FString ErrorCode = TEXT("BP_ADD_NODE_FAILED");
			FString Suggestion = TEXT("Verify the node type is valid and the graph exists");
			if (ErrorMsg.Contains(TEXT("already exists")))
			{
				ErrorCode = TEXT("DUPLICATE_NATIVE_EVENT");
				Suggestion = TEXT("Use blueprint.read_event_graph to see existing event nodes, "
					"or use 'CustomEvent' type to create user-defined events");
			}

			return FOliveWriteResult::ExecutionError(ErrorCode, ErrorMsg, Suggestion);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("graph"), GraphName);
		ResultData->SetStringField(TEXT("node_id"), WriteResult.CreatedNodeId);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully added node '%s' with ID '%s'"), *NodeType, *WriteResult.CreatedNodeId));

		// Deterministic pin manifest to avoid pin-name guessing retries.
		if (UEdGraphNode* CreatedNode = GraphWriter.GetCachedNode(AssetPath, WriteResult.CreatedNodeId))
		{
			ResultData->SetObjectField(TEXT("pins"), BuildPinManifest(CreatedNode));
		}

		FOliveWriteResult Result = FOliveWriteResult::Success(ResultData);
		Result.CreatedNodeIds.Add(WriteResult.CreatedNodeId);
		return Result;
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintRemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	// Parse required parameters
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph"), GraphName))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'graph'"),
			TEXT("Provide the graph name")
		);
	}

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'node_id'"),
			TEXT("Provide the node ID to remove")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.remove_node");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Remove node '%s' from graph '%s' in '%s'"), *NodeId, *GraphName, *AssetPath)
	);
	Request.OperationCategory = TEXT("graph_editing"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, GraphName, NodeId](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();

		// Capture connections before removal so we can report broken links
		TArray<TSharedPtr<FJsonValue>> BrokenLinks;
		UEdGraphNode* NodeToRemove = GraphWriter.GetCachedNode(AssetPath, NodeId);
		if (NodeToRemove)
		{
			// Find the graph to pass to CaptureNodeConnections
			UBlueprint* BP = Cast<UBlueprint>(Target);
			UEdGraph* Graph = nullptr;
			if (BP)
			{
				for (UEdGraph* G : BP->UbergraphPages)
				{
					if (G && G->GetName() == GraphName)
					{
						Graph = G;
						break;
					}
				}
				if (!Graph)
				{
					for (UEdGraph* G : BP->FunctionGraphs)
					{
						if (G && G->GetName() == GraphName)
						{
							Graph = G;
							break;
						}
					}
				}
				if (!Graph)
				{
					for (UEdGraph* G : BP->MacroGraphs)
					{
						if (G && G->GetName() == GraphName)
						{
							Graph = G;
							break;
						}
					}
				}
			}

			BrokenLinks = GraphWriter.CaptureNodeConnections(AssetPath, Graph, NodeToRemove);
		}

		FOliveBlueprintWriteResult WriteResult = GraphWriter.RemoveNode(AssetPath, GraphName, NodeId);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_REMOVE_NODE_FAILED"),
				ErrorMsg,
				TEXT("Verify the node ID is valid and exists in the graph")
			);
		}

		// Success - build result data with broken link report
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("graph"), GraphName);
		ResultData->SetStringField(TEXT("node_id"), NodeId);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully removed node '%s'"), *NodeId));
		ResultData->SetArrayField(TEXT("broken_links"), BrokenLinks);
		ResultData->SetNumberField(TEXT("broken_link_count"), static_cast<double>(BrokenLinks.Num()));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintConnectPins(const TSharedPtr<FJsonObject>& Params)
{
	// Parse required parameters
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath))
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintConnectPins: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph"), GraphName))
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintConnectPins: Missing required param 'graph' for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'graph'"),
			TEXT("Provide the graph name")
		);
	}

	FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();

	auto BuildResolveError = [](const FString& Message, const TArray<TSharedPtr<FJsonValue>>& Candidates) -> FOliveToolResult
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("reason_code"), TEXT("CONNECT_PIN_RESOLUTION_FAILED"));
		ErrorData->SetArrayField(TEXT("candidates"), Candidates);

		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("CONNECT_PIN_RESOLUTION_FAILED"),
			Message,
			TEXT("Use exact pin names from blueprint.add_node pin manifest or pass valid semantic endpoints."));
		Result.Data = ErrorData;
		return Result;
	};

	auto ResolveEndpoint = [&](const FString& StringField, const FString& RefField, EEdGraphPinDirection Direction, FString& OutPinRef) -> TOptional<FOliveToolResult>
	{
		FString DirectRef;
		if (Params->TryGetStringField(StringField, DirectRef) && !DirectRef.IsEmpty())
		{
			OutPinRef = DirectRef;
			return TOptional<FOliveToolResult>();
		}

		const TSharedPtr<FJsonObject>* RefPtr = nullptr;
		if (!Params->TryGetObjectField(RefField, RefPtr) || !RefPtr || !(*RefPtr).IsValid())
		{
			return TOptional<FOliveToolResult>(FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				FString::Printf(TEXT("Missing required parameter '%s'"), *StringField),
				FString::Printf(TEXT("Provide '%s' as 'node_id.pin_name' or '%s' object with node_id + semantic/pin"), *StringField, *RefField)));
		}

		FString NodeId;
		(*RefPtr)->TryGetStringField(TEXT("node_id"), NodeId);
		if (NodeId.IsEmpty())
		{
			return TOptional<FOliveToolResult>(FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				FString::Printf(TEXT("Field '%s.node_id' is required"), *RefField),
				TEXT("Provide node_id in semantic endpoint object")));
		}

		FString ExactPin;
		if ((*RefPtr)->TryGetStringField(TEXT("pin"), ExactPin) && !ExactPin.IsEmpty())
		{
			OutPinRef = FString::Printf(TEXT("%s.%s"), *NodeId, *ExactPin);
			return TOptional<FOliveToolResult>();
		}

		FString Semantic;
		(*RefPtr)->TryGetStringField(TEXT("semantic"), Semantic);
		if (Semantic.IsEmpty())
		{
			return TOptional<FOliveToolResult>(FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				FString::Printf(TEXT("Field '%s.semantic' is required when '%s.pin' is omitted"), *RefField, *RefField),
				TEXT("Provide a semantic such as 'exec_out', 'exec_in', 'data_out', 'data_in', 'True', or 'False'")));
		}

		UEdGraphNode* Node = GraphWriter.GetCachedNode(AssetPath, NodeId);
		if (!Node)
		{
			return TOptional<FOliveToolResult>(FOliveToolResult::Error(
				TEXT("NODE_NOT_FOUND"),
				FString::Printf(TEXT("Node '%s' not found in cache for semantic pin resolution"), *NodeId),
				TEXT("Create/read nodes first so IDs are available, or use exact pin references.")));
		}

		FString ResolvedPinName;
		TArray<TSharedPtr<FJsonValue>> Candidates;
		if (!ResolveSemanticPinOnNode(Node, Semantic, Direction, ResolvedPinName, Candidates))
		{
			return TOptional<FOliveToolResult>(BuildResolveError(
				FString::Printf(TEXT("Could not resolve semantic pin '%s' on node '%s'"), *Semantic, *NodeId),
				Candidates));
		}

		OutPinRef = FString::Printf(TEXT("%s.%s"), *NodeId, *ResolvedPinName);
		return TOptional<FOliveToolResult>();
	};

	FString SourcePin;
	if (TOptional<FOliveToolResult> Err = ResolveEndpoint(TEXT("source"), TEXT("source_ref"), EGPD_Output, SourcePin))
	{
		return Err.GetValue();
	}

	FString TargetPin;
	if (TOptional<FOliveToolResult> Err = ResolveEndpoint(TEXT("target"), TEXT("target_ref"), EGPD_Input, TargetPin))
	{
		return Err.GetValue();
	}

	UE_LOG(LogOliveBPTools, Log, TEXT("connect_pins: resolved source='%s', target='%s', graph='%s', asset='%s'"),
		*SourcePin, *TargetPin, *GraphName, *AssetPath);

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintConnectPins: Blueprint not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.connect_pins");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Connect pins '%s' -> '%s' in graph '%s' of '%s'"), *SourcePin, *TargetPin, *GraphName, *AssetPath)
	);
	Request.OperationCategory = TEXT("graph_editing"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, GraphName, SourcePin, TargetPin](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();
		FOliveBlueprintWriteResult WriteResult = GraphWriter.ConnectPins(AssetPath, GraphName, SourcePin, TargetPin);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_CONNECT_PINS_FAILED"),
				ErrorMsg,
				TEXT("Verify the pin references are valid and compatible")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("graph"), GraphName);
		ResultData->SetStringField(TEXT("source"), SourcePin);
		ResultData->SetStringField(TEXT("target"), TargetPin);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully connected '%s' to '%s'"), *SourcePin, *TargetPin));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintDisconnectPins(const TSharedPtr<FJsonObject>& Params)
{
	// Parse required parameters
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph"), GraphName))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'graph'"),
			TEXT("Provide the graph name")
		);
	}

	FString SourcePin;
	if (!Params->TryGetStringField(TEXT("source"), SourcePin))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'source'"),
			TEXT("Provide the source pin reference in 'node_id.pin_name' format")
		);
	}

	FString TargetPin;
	if (!Params->TryGetStringField(TEXT("target"), TargetPin))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'target'"),
			TEXT("Provide the target pin reference in 'node_id.pin_name' format")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.disconnect_pins");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Disconnect pins '%s' -> '%s' in graph '%s' of '%s'"), *SourcePin, *TargetPin, *GraphName, *AssetPath)
	);
	Request.OperationCategory = TEXT("graph_editing"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, GraphName, SourcePin, TargetPin](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();
		FOliveBlueprintWriteResult WriteResult = GraphWriter.DisconnectPins(AssetPath, GraphName, SourcePin, TargetPin);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_DISCONNECT_PINS_FAILED"),
				ErrorMsg,
				TEXT("Verify the pin references are valid and currently connected")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("graph"), GraphName);
		ResultData->SetStringField(TEXT("source"), SourcePin);
		ResultData->SetStringField(TEXT("target"), TargetPin);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully disconnected '%s' from '%s'"), *SourcePin, *TargetPin));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintSetPinDefault(const TSharedPtr<FJsonObject>& Params)
{
	// Parse required parameters
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph"), GraphName))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'graph'"),
			TEXT("Provide the graph name")
		);
	}

	FString Pin;
	if (!Params->TryGetStringField(TEXT("pin"), Pin))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'pin'"),
			TEXT("Provide the pin reference in 'node_id.pin_name' format")
		);
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'value'"),
			TEXT("Provide the default value to set")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.set_pin_default");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Set default value of pin '%s' to '%s' in graph '%s' of '%s'"), *Pin, *Value, *GraphName, *AssetPath)
	);
	Request.OperationCategory = TEXT("graph_editing"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, GraphName, Pin, Value](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();
		FOliveBlueprintWriteResult WriteResult = GraphWriter.SetPinDefault(AssetPath, GraphName, Pin, Value);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_SET_PIN_DEFAULT_FAILED"),
				ErrorMsg,
				TEXT("Verify the pin reference is valid and is an input pin")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("graph"), GraphName);
		ResultData->SetStringField(TEXT("pin"), Pin);
		ResultData->SetStringField(TEXT("value"), Value);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully set default value of pin '%s' to '%s'"), *Pin, *Value));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintSetNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	// Parse required parameters
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph"), GraphName))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'graph'"),
			TEXT("Provide the graph name")
		);
	}

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'node_id'"),
			TEXT("Provide the node ID")
		);
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property"), PropertyName))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'property'"),
			TEXT("Provide the property name to set")
		);
	}

	FString PropertyValue;
	if (!Params->TryGetStringField(TEXT("value"), PropertyValue))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'value'"),
			TEXT("Provide the property value")
		);
	}

	// Load Blueprint for target asset
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.set_node_property");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Set property '%s' of node '%s' to '%s' in graph '%s' of '%s'"), *PropertyName, *NodeId, *PropertyValue, *GraphName, *AssetPath)
	);
	Request.OperationCategory = TEXT("graph_editing"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, GraphName, NodeId, PropertyName, PropertyValue](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();
		FOliveBlueprintWriteResult WriteResult = GraphWriter.SetNodeProperty(AssetPath, GraphName, NodeId, PropertyName, PropertyValue);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_SET_NODE_PROPERTY_FAILED"),
				ErrorMsg,
				TEXT("Verify the node ID and property name are valid")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("graph"), GraphName);
		ResultData->SetStringField(TEXT("node_id"), NodeId);
		ResultData->SetStringField(TEXT("property"), PropertyName);
		ResultData->SetStringField(TEXT("value"), PropertyValue);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully set property '%s' of node '%s' to '%s'"), *PropertyName, *NodeId, *PropertyValue));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

// ============================================================================
// AnimBP Writer Tool Handlers
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleAnimBPAddStateMachine(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'name' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Animation Blueprint asset path")
		);
	}

	// Extract state machine name
	FString StateMachineName;
	if (!Params->TryGetStringField(TEXT("name"), StateMachineName) || StateMachineName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide a name for the state machine")
		);
	}

	// Load Animation Blueprint
	UAnimBlueprint* AnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
	if (!AnimBlueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Animation Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and points to an Animation Blueprint")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("animbp.add_state_machine");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = AnimBlueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add state machine '%s' to '%s'"), *StateMachineName, *AssetPath)
	);
	Request.OperationCategory = TEXT("state_machine"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([StateMachineName](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Target);
		if (!AnimBP)
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("INVALID_TARGET"),
				TEXT("Target is not an Animation Blueprint"),
				TEXT("Ensure the asset path points to an Animation Blueprint")
			);
		}

		FOliveAnimGraphWriter& Writer = FOliveAnimGraphWriter::Get();
		FOliveAnimGraphWriteResult WriteResult = Writer.AddStateMachine(AnimBP, StateMachineName);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("ANIMBP_ADD_STATE_MACHINE_FAILED"),
				ErrorMsg,
				TEXT("Verify the state machine name is valid")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), Req.AssetPath);
		ResultData->SetStringField(TEXT("state_machine_name"), WriteResult.CreatedStateMachineName);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully created state machine '%s'"), *WriteResult.CreatedStateMachineName));

		FOliveWriteResult Result = FOliveWriteResult::Success(ResultData);
		Result.CreatedItem = WriteResult.CreatedStateMachineName;
		return Result;
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleAnimBPAddState(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path', 'machine', and 'name' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Animation Blueprint asset path")
		);
	}

	// Extract state machine name
	FString StateMachineName;
	if (!Params->TryGetStringField(TEXT("machine"), StateMachineName) || StateMachineName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'machine' is missing or empty"),
			TEXT("Provide the state machine name")
		);
	}

	// Extract state name
	FString StateName;
	if (!Params->TryGetStringField(TEXT("name"), StateName) || StateName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide a name for the state")
		);
	}

	// Extract optional animation asset
	FString AnimationAsset;
	Params->TryGetStringField(TEXT("animation"), AnimationAsset);

	// Load Animation Blueprint
	UAnimBlueprint* AnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
	if (!AnimBlueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Animation Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and points to an Animation Blueprint")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("animbp.add_state");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = AnimBlueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add state '%s' to state machine '%s' in '%s'"), *StateName, *StateMachineName, *AssetPath)
	);
	Request.OperationCategory = TEXT("state"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([StateMachineName, StateName, AnimationAsset](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Target);
		if (!AnimBP)
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("INVALID_TARGET"),
				TEXT("Target is not an Animation Blueprint"),
				TEXT("Ensure the asset path points to an Animation Blueprint")
			);
		}

		FOliveAnimGraphWriter& Writer = FOliveAnimGraphWriter::Get();
		FOliveAnimGraphWriteResult WriteResult = Writer.AddState(AnimBP, StateMachineName, StateName, AnimationAsset);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("ANIMBP_ADD_STATE_FAILED"),
				ErrorMsg,
				TEXT("Verify the state machine exists and the state name is valid")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), Req.AssetPath);
		ResultData->SetStringField(TEXT("state_machine"), StateMachineName);
		ResultData->SetStringField(TEXT("state_name"), WriteResult.CreatedStateName);
		if (!AnimationAsset.IsEmpty())
		{
			ResultData->SetStringField(TEXT("animation_asset"), AnimationAsset);
		}
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully created state '%s' in state machine '%s'"), *WriteResult.CreatedStateName, *StateMachineName));

		FOliveWriteResult Result = FOliveWriteResult::Success(ResultData);
		Result.CreatedItem = WriteResult.CreatedStateName;
		return Result;
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleAnimBPAddTransition(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path', 'machine', 'from', and 'to' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Animation Blueprint asset path")
		);
	}

	// Extract state machine name
	FString StateMachineName;
	if (!Params->TryGetStringField(TEXT("machine"), StateMachineName) || StateMachineName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'machine' is missing or empty"),
			TEXT("Provide the state machine name")
		);
	}

	// Extract from state
	FString FromState;
	if (!Params->TryGetStringField(TEXT("from"), FromState) || FromState.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'from' is missing or empty"),
			TEXT("Provide the source state name")
		);
	}

	// Extract to state
	FString ToState;
	if (!Params->TryGetStringField(TEXT("to"), ToState) || ToState.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'to' is missing or empty"),
			TEXT("Provide the destination state name")
		);
	}

	// Load Animation Blueprint
	UAnimBlueprint* AnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
	if (!AnimBlueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Animation Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and points to an Animation Blueprint")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("animbp.add_transition");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = AnimBlueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add transition from '%s' to '%s' in state machine '%s' of '%s'"), *FromState, *ToState, *StateMachineName, *AssetPath)
	);
	Request.OperationCategory = TEXT("transition"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([StateMachineName, FromState, ToState](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Target);
		if (!AnimBP)
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("INVALID_TARGET"),
				TEXT("Target is not an Animation Blueprint"),
				TEXT("Ensure the asset path points to an Animation Blueprint")
			);
		}

		FOliveAnimGraphWriter& Writer = FOliveAnimGraphWriter::Get();
		FOliveAnimGraphWriteResult WriteResult = Writer.AddTransition(AnimBP, StateMachineName, FromState, ToState);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("ANIMBP_ADD_TRANSITION_FAILED"),
				ErrorMsg,
				TEXT("Verify the state machine and both states exist")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), Req.AssetPath);
		ResultData->SetStringField(TEXT("state_machine"), StateMachineName);
		ResultData->SetStringField(TEXT("from_state"), FromState);
		ResultData->SetStringField(TEXT("to_state"), ToState);
		ResultData->SetStringField(TEXT("transition_id"), WriteResult.CreatedTransitionId);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully created transition from '%s' to '%s'"), *FromState, *ToState));

		FOliveWriteResult Result = FOliveWriteResult::Success(ResultData);
		Result.CreatedItem = WriteResult.CreatedTransitionId;
		return Result;
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleAnimBPSetTransitionRule(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path', 'machine', 'from', 'to', and 'rule' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Animation Blueprint asset path")
		);
	}

	// Extract state machine name
	FString StateMachineName;
	if (!Params->TryGetStringField(TEXT("machine"), StateMachineName) || StateMachineName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'machine' is missing or empty"),
			TEXT("Provide the state machine name")
		);
	}

	// Extract from state
	FString FromState;
	if (!Params->TryGetStringField(TEXT("from"), FromState) || FromState.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'from' is missing or empty"),
			TEXT("Provide the source state name")
		);
	}

	// Extract to state
	FString ToState;
	if (!Params->TryGetStringField(TEXT("to"), ToState) || ToState.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'to' is missing or empty"),
			TEXT("Provide the destination state name")
		);
	}

	// Extract rule
	const TSharedPtr<FJsonObject>* RulePtr;
	if (!Params->TryGetObjectField(TEXT("rule"), RulePtr) || !RulePtr->IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'rule' is missing or invalid"),
			TEXT("Provide a rule expression object")
		);
	}

	// Load Animation Blueprint
	UAnimBlueprint* AnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *AssetPath);
	if (!AnimBlueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Animation Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and points to an Animation Blueprint")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("animbp.set_transition_rule");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = AnimBlueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Set transition rule from '%s' to '%s' in state machine '%s' of '%s'"), *FromState, *ToState, *StateMachineName, *AssetPath)
	);
	Request.OperationCategory = TEXT("transition"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;
	Request.bSkipVerification = false;

	// Capture rule expression
	TSharedPtr<FJsonObject> RuleExpression = *RulePtr;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([StateMachineName, FromState, ToState, RuleExpression](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Target);
		if (!AnimBP)
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("INVALID_TARGET"),
				TEXT("Target is not an Animation Blueprint"),
				TEXT("Ensure the asset path points to an Animation Blueprint")
			);
		}

		FOliveAnimGraphWriter& Writer = FOliveAnimGraphWriter::Get();
		FOliveAnimGraphWriteResult WriteResult = Writer.SetTransitionRule(AnimBP, StateMachineName, FromState, ToState, RuleExpression);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("ANIMBP_SET_TRANSITION_RULE_FAILED"),
				ErrorMsg,
				TEXT("Verify the transition exists and the rule expression is valid")
			);
		}

		// Success - build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), Req.AssetPath);
		ResultData->SetStringField(TEXT("state_machine"), StateMachineName);
		ResultData->SetStringField(TEXT("from_state"), FromState);
		ResultData->SetStringField(TEXT("to_state"), ToState);
		ResultData->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully set transition rule from '%s' to '%s' (rule graph creation not fully implemented)"), *FromState, *ToState));

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

// ============================================================================
// Widget Writer Tool Handler Stubs
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleWidgetAddWidget(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'class' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Widget Blueprint asset path")
		);
	}

	// Extract class
	FString WidgetClass;
	if (!Params->TryGetStringField(TEXT("class"), WidgetClass) || WidgetClass.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'class' is missing or empty"),
			TEXT("Provide the widget class name (e.g., 'Button', 'TextBlock', 'Image')")
		);
	}

	// Extract optional parameters
	FString WidgetName = TEXT("");
	Params->TryGetStringField(TEXT("name"), WidgetName);

	FString ParentWidget = TEXT("");
	Params->TryGetStringField(TEXT("parent"), ParentWidget);

	FString SlotType = TEXT("");
	Params->TryGetStringField(TEXT("slot"), SlotType);

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("widget.add_widget");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.OperationDescription = FText::Format(
		NSLOCTEXT("OliveAI", "AddWidgetDesc", "Add Widget '{0}' of class '{1}'"),
		FText::FromString(WidgetName.IsEmpty() ? TEXT("(auto)") : WidgetName),
		FText::FromString(WidgetClass));
	Request.OperationCategory = TEXT("widget");
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false; // Widget Blueprints don't need compilation for widget tree changes

	// Define executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([WidgetClass, WidgetName, ParentWidget, SlotType](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveWidgetWriter& Writer = FOliveWidgetWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddWidget(
			Req.AssetPath,
			WidgetClass,
			WidgetName,
			ParentWidget,
			SlotType);

		if (!WriteResult.bSuccess)
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("WIDGET_ADD_FAILED"),
				WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error"),
				TEXT("Check widget class name and parent widget"));
		}

		// Build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("widget_name"), WriteResult.CreatedItemName);
		ResultData->SetStringField(TEXT("widget_class"), WidgetClass);
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);

		FOliveWriteResult Result = FOliveWriteResult::Success(ResultData);
		Result.CreatedItem = WriteResult.CreatedItemName;
		return Result;
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleWidgetRemoveWidget(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'name' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Widget Blueprint asset path")
		);
	}

	// Extract widget name
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("name"), WidgetName) || WidgetName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing or empty"),
			TEXT("Provide the widget name to remove")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("widget.remove_widget");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.OperationDescription = FText::Format(
		NSLOCTEXT("OliveAI", "RemoveWidgetDesc", "Remove Widget '{0}'"),
		FText::FromString(WidgetName));
	Request.OperationCategory = TEXT("widget");
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;

	// Define executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([WidgetName](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveWidgetWriter& Writer = FOliveWidgetWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.RemoveWidget(
			Req.AssetPath,
			WidgetName);

		if (!WriteResult.bSuccess)
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("WIDGET_REMOVE_FAILED"),
				WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error"),
				TEXT("Check that the widget exists in the Widget Blueprint"));
		}

		// Build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("removed_widget"), WidgetName);
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleWidgetSetProperty(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path', 'widget', 'property', and 'value' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Widget Blueprint asset path")
		);
	}

	// Extract widget name
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget"), WidgetName) || WidgetName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'widget' is missing or empty"),
			TEXT("Provide the widget name")
		);
	}

	// Extract property name
	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property"), PropertyName) || PropertyName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'property' is missing or empty"),
			TEXT("Provide the property name to set")
		);
	}

	// Extract property value
	FString PropertyValue;
	if (!Params->TryGetStringField(TEXT("value"), PropertyValue))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'value' is missing"),
			TEXT("Provide the property value")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("widget.set_property");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.OperationDescription = FText::Format(
		NSLOCTEXT("OliveAI", "SetWidgetPropertyDesc", "Set Widget Property '{0}.{1}'"),
		FText::FromString(WidgetName),
		FText::FromString(PropertyName));
	Request.OperationCategory = TEXT("widget");
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;

	// Define executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([WidgetName, PropertyName, PropertyValue](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveWidgetWriter& Writer = FOliveWidgetWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.SetProperty(
			Req.AssetPath,
			WidgetName,
			PropertyName,
			PropertyValue);

		if (!WriteResult.bSuccess)
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("WIDGET_PROPERTY_SET_FAILED"),
				WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error"),
				TEXT("Check that the widget and property exist and the value is valid for the property type"));
		}

		// Build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("widget_name"), WidgetName);
		ResultData->SetStringField(TEXT("property_name"), PropertyName);
		ResultData->SetStringField(TEXT("property_value"), PropertyValue);
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleWidgetBindProperty(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path', 'widget', 'property', and 'function' fields")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Widget Blueprint asset path")
		);
	}

	// Extract widget name
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget"), WidgetName) || WidgetName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'widget' is missing or empty"),
			TEXT("Provide the widget name")
		);
	}

	// Extract property name
	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property"), PropertyName) || PropertyName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'property' is missing or empty"),
			TEXT("Provide the property name to bind")
		);
	}

	// Extract function name
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function"), FunctionName) || FunctionName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'function' is missing or empty"),
			TEXT("Provide the function name to bind to")
		);
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("widget.bind_property");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.OperationDescription = FText::Format(
		NSLOCTEXT("OliveAI", "BindWidgetPropertyDesc", "Bind Widget Property '{0}.{1}' to '{2}'"),
		FText::FromString(WidgetName),
		FText::FromString(PropertyName),
		FText::FromString(FunctionName));
	Request.OperationCategory = TEXT("widget");
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = false;

	// Define executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([WidgetName, PropertyName, FunctionName](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveWidgetWriter& Writer = FOliveWidgetWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.BindProperty(
			Req.AssetPath,
			WidgetName,
			PropertyName,
			FunctionName);

		if (!WriteResult.bSuccess)
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("WIDGET_PROPERTY_BIND_FAILED"),
				WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error"),
				TEXT("Check that the widget, property, and function exist and are compatible"));
		}

		// Build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("widget_name"), WidgetName);
		ResultData->SetStringField(TEXT("property_name"), PropertyName);
		ResultData->SetStringField(TEXT("function_name"), FunctionName);
		ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);

		// Note any warnings from the writer
		if (WriteResult.Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningsArray;
			for (const FString& Warning : WriteResult.Warnings)
			{
				WarningsArray.Add(MakeShareable(new FJsonValueString(Warning)));
			}
			ResultData->SetArrayField(TEXT("warnings"), WarningsArray);
		}

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	return Result.ToToolResult();
}

// ============================================================================
// Common Helpers
// ============================================================================

bool FOliveBlueprintToolHandlers::LoadBlueprintFromParams(
	const TSharedPtr<FJsonObject>& Params,
	UBlueprint*& OutBlueprint,
	FOliveToolResult& OutError)
{
	// This helper is for future use by writer tools
	// For now, reader tools call FOliveBlueprintReader directly which handles loading

	if (!Params.IsValid())
	{
		OutError = FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object")
		);
		return false;
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
		return false;
	}

	// Load Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to load Blueprint at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
		return false;
	}

	OutBlueprint = Blueprint;
	return true;
}

FOliveIRType FOliveBlueprintToolHandlers::ParseTypeFromParams(const TSharedPtr<FJsonObject>& TypeJson)
{
	FOliveIRType Type;
	Type.Category = EOliveIRTypeCategory::Unknown;

	if (!TypeJson.IsValid())
	{
		return Type;
	}

	// Parse category (required)
	FString CategoryString;
	if (!TypeJson->TryGetStringField(TEXT("category"), CategoryString))
	{
		return Type;
	}
	CategoryString = CategoryString.ToLower();

	// Alias normalization: map common variations to canonical names
	if (CategoryString == TEXT("boolean"))
	{
		CategoryString = TEXT("bool");
	}
	else if (CategoryString == TEXT("integer") || CategoryString == TEXT("int32"))
	{
		CategoryString = TEXT("int");
	}
	else if (CategoryString == TEXT("linearcolor") || CategoryString == TEXT("flinearcolor"))
	{
		CategoryString = TEXT("linear_color");
	}
	else if (CategoryString == TEXT("str") || CategoryString == TEXT("fstring"))
	{
		CategoryString = TEXT("string");
	}
	else if (CategoryString == TEXT("fname"))
	{
		CategoryString = TEXT("name");
	}
	else if (CategoryString == TEXT("ftext"))
	{
		CategoryString = TEXT("text");
	}
	else if (CategoryString == TEXT("fvector") || CategoryString == TEXT("vec") || CategoryString == TEXT("vec3"))
	{
		CategoryString = TEXT("vector");
	}
	else if (CategoryString == TEXT("frotator") || CategoryString == TEXT("rot"))
	{
		CategoryString = TEXT("rotator");
	}
	else if (CategoryString == TEXT("ftransform"))
	{
		CategoryString = TEXT("transform");
	}
	else if (CategoryString == TEXT("fcolor"))
	{
		CategoryString = TEXT("color");
	}
	else if (CategoryString == TEXT("fvector2d") || CategoryString == TEXT("vec2") || CategoryString == TEXT("vec2d"))
	{
		CategoryString = TEXT("vector2d");
	}
	else if (CategoryString == TEXT("float32"))
	{
		CategoryString = TEXT("float");
	}
	else if (CategoryString == TEXT("float64"))
	{
		CategoryString = TEXT("double");
	}
	else if (CategoryString == TEXT("uint8"))
	{
		CategoryString = TEXT("byte");
	}

	// Map category string to enum
	if (CategoryString == TEXT("bool"))
	{
		Type.Category = EOliveIRTypeCategory::Bool;
	}
	else if (CategoryString == TEXT("byte"))
	{
		Type.Category = EOliveIRTypeCategory::Byte;
	}
	else if (CategoryString == TEXT("int"))
	{
		Type.Category = EOliveIRTypeCategory::Int;
	}
	else if (CategoryString == TEXT("int64"))
	{
		Type.Category = EOliveIRTypeCategory::Int64;
	}
	else if (CategoryString == TEXT("float"))
	{
		Type.Category = EOliveIRTypeCategory::Float;
	}
	else if (CategoryString == TEXT("double"))
	{
		Type.Category = EOliveIRTypeCategory::Double;
	}
	else if (CategoryString == TEXT("string"))
	{
		Type.Category = EOliveIRTypeCategory::String;
	}
	else if (CategoryString == TEXT("name"))
	{
		Type.Category = EOliveIRTypeCategory::Name;
	}
	else if (CategoryString == TEXT("text"))
	{
		Type.Category = EOliveIRTypeCategory::Text;
	}
	else if (CategoryString == TEXT("vector"))
	{
		Type.Category = EOliveIRTypeCategory::Vector;
	}
	else if (CategoryString == TEXT("vector2d"))
	{
		Type.Category = EOliveIRTypeCategory::Vector2D;
	}
	else if (CategoryString == TEXT("rotator"))
	{
		Type.Category = EOliveIRTypeCategory::Rotator;
	}
	else if (CategoryString == TEXT("transform"))
	{
		Type.Category = EOliveIRTypeCategory::Transform;
	}
	else if (CategoryString == TEXT("color"))
	{
		Type.Category = EOliveIRTypeCategory::Color;
	}
	else if (CategoryString == TEXT("linear_color"))
	{
		Type.Category = EOliveIRTypeCategory::LinearColor;
	}
	else if (CategoryString == TEXT("object"))
	{
		Type.Category = EOliveIRTypeCategory::Object;
		TypeJson->TryGetStringField(TEXT("class_name"), Type.ClassName);
	}
	else if (CategoryString == TEXT("class"))
	{
		Type.Category = EOliveIRTypeCategory::Class;
		TypeJson->TryGetStringField(TEXT("class_name"), Type.ClassName);
	}
	else if (CategoryString == TEXT("interface"))
	{
		Type.Category = EOliveIRTypeCategory::Interface;
		TypeJson->TryGetStringField(TEXT("class_name"), Type.ClassName);
	}
	else if (CategoryString == TEXT("struct"))
	{
		Type.Category = EOliveIRTypeCategory::Struct;
		TypeJson->TryGetStringField(TEXT("struct_name"), Type.StructName);
	}
	else if (CategoryString == TEXT("enum"))
	{
		Type.Category = EOliveIRTypeCategory::Enum;
		TypeJson->TryGetStringField(TEXT("enum_name"), Type.EnumName);
	}
	else if (CategoryString == TEXT("delegate"))
	{
		Type.Category = EOliveIRTypeCategory::Delegate;
	}
	else if (CategoryString == TEXT("multicast_delegate"))
	{
		Type.Category = EOliveIRTypeCategory::MulticastDelegate;
	}
	else if (CategoryString == TEXT("array"))
	{
		Type.Category = EOliveIRTypeCategory::Array;
		// Element type stored as JSON string for recursive parsing
		FString ElementTypeStr;
		if (TypeJson->TryGetStringField(TEXT("element_type"), ElementTypeStr))
		{
			Type.ElementTypeJson = ElementTypeStr;
		}
	}
	else if (CategoryString == TEXT("set"))
	{
		Type.Category = EOliveIRTypeCategory::Set;
		FString ElementTypeStr;
		if (TypeJson->TryGetStringField(TEXT("element_type"), ElementTypeStr))
		{
			Type.ElementTypeJson = ElementTypeStr;
		}
	}
	else if (CategoryString == TEXT("map"))
	{
		Type.Category = EOliveIRTypeCategory::Map;
		FString KeyTypeStr;
		if (TypeJson->TryGetStringField(TEXT("key_type"), KeyTypeStr))
		{
			Type.KeyTypeJson = KeyTypeStr;
		}
		FString ValueTypeStr;
		if (TypeJson->TryGetStringField(TEXT("value_type"), ValueTypeStr))
		{
			Type.ValueTypeJson = ValueTypeStr;
		}
	}

	// Parse flags
	TypeJson->TryGetBoolField(TEXT("is_reference"), Type.bIsReference);
	TypeJson->TryGetBoolField(TEXT("is_const"), Type.bIsConst);

	return Type;
}

FOliveIRFunctionSignature FOliveBlueprintToolHandlers::ParseFunctionSignatureFromParams(const TSharedPtr<FJsonObject>& SigJson)
{
	FOliveIRFunctionSignature Signature;

	if (!SigJson.IsValid())
	{
		return Signature;
	}

	// Parse name (required)
	SigJson->TryGetStringField(TEXT("name"), Signature.Name);

	// Parse inputs array
	const TArray<TSharedPtr<FJsonValue>>* InputsArray;
	if (SigJson->TryGetArrayField(TEXT("inputs"), InputsArray))
	{
		for (const TSharedPtr<FJsonValue>& InputValue : *InputsArray)
		{
			if (InputValue->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> ParamObj = InputValue->AsObject();
				FOliveIRFunctionParam Param;

				ParamObj->TryGetStringField(TEXT("name"), Param.Name);

				// Parse type
				const TSharedPtr<FJsonObject>* TypeJsonPtr;
				if (ParamObj->TryGetObjectField(TEXT("type"), TypeJsonPtr))
				{
					Param.Type = ParseTypeFromParams(*TypeJsonPtr);
				}

				ParamObj->TryGetStringField(TEXT("default_value"), Param.DefaultValue);
				ParamObj->TryGetBoolField(TEXT("is_out_param"), Param.bIsOutParam);
				ParamObj->TryGetBoolField(TEXT("is_reference"), Param.bIsReference);

				Signature.Inputs.Add(Param);
			}
		}
	}

	// Parse outputs array
	const TArray<TSharedPtr<FJsonValue>>* OutputsArray;
	if (SigJson->TryGetArrayField(TEXT("outputs"), OutputsArray))
	{
		for (const TSharedPtr<FJsonValue>& OutputValue : *OutputsArray)
		{
			if (OutputValue->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> ParamObj = OutputValue->AsObject();
				FOliveIRFunctionParam Param;

				ParamObj->TryGetStringField(TEXT("name"), Param.Name);

				// Parse type
				const TSharedPtr<FJsonObject>* TypeJsonPtr;
				if (ParamObj->TryGetObjectField(TEXT("type"), TypeJsonPtr))
				{
					Param.Type = ParseTypeFromParams(*TypeJsonPtr);
				}

				ParamObj->TryGetStringField(TEXT("default_value"), Param.DefaultValue);
				ParamObj->TryGetBoolField(TEXT("is_out_param"), Param.bIsOutParam);
				ParamObj->TryGetBoolField(TEXT("is_reference"), Param.bIsReference);

				Signature.Outputs.Add(Param);
			}
		}
	}

	// Parse flags
	SigJson->TryGetBoolField(TEXT("is_static"), Signature.bIsStatic);
	SigJson->TryGetBoolField(TEXT("is_pure"), Signature.bIsPure);
	SigJson->TryGetBoolField(TEXT("is_const"), Signature.bIsConst);
	SigJson->TryGetBoolField(TEXT("is_public"), Signature.bIsPublic);

	// Parse metadata
	SigJson->TryGetStringField(TEXT("category"), Signature.Category);
	SigJson->TryGetStringField(TEXT("description"), Signature.Description);
	SigJson->TryGetStringField(TEXT("keywords"), Signature.Keywords);

	return Signature;
}

FOliveIRVariable FOliveBlueprintToolHandlers::ParseVariableFromParams(const TSharedPtr<FJsonObject>& VarJson)
{
	FOliveIRVariable Variable;

	if (!VarJson.IsValid())
	{
		return Variable;
	}

	// Parse name (required)
	VarJson->TryGetStringField(TEXT("name"), Variable.Name);

	// Parse type (required)
	const TSharedPtr<FJsonObject>* TypeJsonPtr;
	if (VarJson->TryGetObjectField(TEXT("type"), TypeJsonPtr))
	{
		Variable.Type = ParseTypeFromParams(*TypeJsonPtr);
	}
	else
	{
		FString TypeString;
		if (VarJson->TryGetStringField(TEXT("type"), TypeString) && !TypeString.IsEmpty())
		{
			TypeString = TypeString.ToLower();
			if (TypeString == TEXT("boolean"))
			{
				TypeString = TEXT("bool");
			}
			else if (TypeString == TEXT("integer") || TypeString == TEXT("int32"))
			{
				TypeString = TEXT("int");
			}
			else if (TypeString == TEXT("linearcolor"))
			{
				TypeString = TEXT("linear_color");
			}

			// Accept shorthand string types by normalizing to schema shape.
			TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
			TypeObj->SetStringField(TEXT("category"), TypeString);
			Variable.Type = ParseTypeFromParams(TypeObj);
		}
	}

	// Parse optional fields
	VarJson->TryGetStringField(TEXT("default_value"), Variable.DefaultValue);
	VarJson->TryGetStringField(TEXT("category"), Variable.Category);
	VarJson->TryGetStringField(TEXT("description"), Variable.Description);

	// Parse flags
	VarJson->TryGetBoolField(TEXT("blueprint_read_write"), Variable.bBlueprintReadWrite);
	VarJson->TryGetBoolField(TEXT("expose_on_spawn"), Variable.bExposeOnSpawn);
	VarJson->TryGetBoolField(TEXT("replicated"), Variable.bReplicated);
	VarJson->TryGetBoolField(TEXT("save_game"), Variable.bSaveGame);
	VarJson->TryGetBoolField(TEXT("edit_anywhere"), Variable.bEditAnywhere);
	VarJson->TryGetBoolField(TEXT("blueprint_visible"), Variable.bBlueprintVisible);

	// Set DefinedIn to "self" for newly created variables
	Variable.DefinedIn = TEXT("self");

	return Variable;
}

// ============================================================================
// Plan JSON Tool Registration
// ============================================================================

void FOliveBlueprintToolHandlers::RegisterPlanTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// blueprint.preview_plan_json
	Registry.RegisterTool(
		TEXT("blueprint.preview_plan_json"),
		TEXT("Preview an intent-level Blueprint plan without mutating anything. Returns a diff, fingerprint, and lowered op count."),
		OliveBlueprintSchemas::BlueprintPreviewPlanJson(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintPreviewPlanJson),
		{TEXT("blueprint"), TEXT("read"), TEXT("plan")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.preview_plan_json"));

	// blueprint.apply_plan_json
	Registry.RegisterTool(
		TEXT("blueprint.apply_plan_json"),
		TEXT("Apply an intent-level Blueprint plan atomically. Resolves intents to nodes, executes in a single transaction, compiles once."),
		OliveBlueprintSchemas::BlueprintApplyPlanJson(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintApplyPlanJson),
		{TEXT("blueprint"), TEXT("write"), TEXT("graph"), TEXT("plan")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.apply_plan_json"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered Plan JSON tools (preview + apply)"));
}

// ============================================================================
// Plan JSON Tool Handlers
// ============================================================================

namespace
{

/**
 * Find a graph on the Blueprint by name, searching both UbergraphPages and FunctionGraphs.
 * Falls back to first UbergraphPage if GraphName is "EventGraph" and no exact match is found.
 *
 * @param Blueprint The Blueprint to search
 * @param GraphName The name of the graph to find
 * @return The found graph, or nullptr if not found
 */
UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
	// Search UbergraphPages (event graphs)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Search FunctionGraphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Fallback: if "EventGraph" was requested, use first UbergraphPage
	if (GraphName == TEXT("EventGraph") && Blueprint->UbergraphPages.Num() > 0)
	{
		return Blueprint->UbergraphPages[0];
	}

	return nullptr;
}

/**
 * Find a graph by name, or create a new function graph if it doesn't exist.
 * Only creates for non-EventGraph targets (EventGraph must already exist).
 *
 * @param Blueprint The owning Blueprint
 * @param GraphName The graph name to find or create
 * @param bOutCreated Set to true if a new graph was created
 * @return The found or created graph, or nullptr if creation failed
 */
UEdGraph* FindOrCreateFunctionGraph(UBlueprint* Blueprint, const FString& GraphName, bool& bOutCreated)
{
	bOutCreated = false;

	// Try to find existing graph first
	UEdGraph* Existing = FindGraphByName(Blueprint, GraphName);
	if (Existing)
	{
		return Existing;
	}

	// EventGraph must already exist — we don't create new ubergraph pages
	if (GraphName == TEXT("EventGraph"))
	{
		return nullptr;
	}

	// Create a new function graph
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*GraphName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (!NewGraph)
	{
		return nullptr;
	}

	FBlueprintEditorUtils::AddFunctionGraph(Blueprint, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromObject=*/static_cast<UClass*>(nullptr));
	bOutCreated = true;

	UE_LOG(LogOliveBPTools, Log, TEXT("Created new function graph '%s' on Blueprint '%s'"),
		*GraphName, *Blueprint->GetPathName());

	return NewGraph;
}

/**
 * Build a plan summary JSON object from a resolved and lowered plan.
 * Summarizes step count and operation type breakdown.
 *
 * @param Plan The original plan
 * @param LowerResult The lowered operations result
 * @return JSON object with plan_summary data
 */
TSharedPtr<FJsonObject> BuildPlanSummary(
	const FOliveIRBlueprintPlan& Plan,
	const FOlivePlanLowerResult& LowerResult)
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("step_count"), Plan.Steps.Num());
	Summary->SetNumberField(TEXT("lowered_ops_count"), LowerResult.Ops.Num());

	// Count ops by type
	TMap<FString, int32> OpCounts;
	for (const FOliveIRBlueprintPlanStep& Step : Plan.Steps)
	{
		OpCounts.FindOrAdd(Step.Op, 0)++;
	}

	TSharedPtr<FJsonObject> OpBreakdown = MakeShared<FJsonObject>();
	for (const auto& Pair : OpCounts)
	{
		OpBreakdown->SetNumberField(Pair.Key, Pair.Value);
	}
	Summary->SetObjectField(TEXT("op_count_by_type"), OpBreakdown);

	return Summary;
}

/**
 * Collect all warnings from resolve and lower results into a JSON array.
 *
 * @param ResolveResult The resolve result containing warnings
 * @param LowerResult The lower result containing error warnings
 * @return JSON array of warning strings
 */
TArray<TSharedPtr<FJsonValue>> CollectWarnings(
	const FOlivePlanResolveResult& ResolveResult,
	const FOlivePlanLowerResult& LowerResult)
{
	TArray<TSharedPtr<FJsonValue>> Warnings;
	for (const FString& W : ResolveResult.Warnings)
	{
		Warnings.Add(MakeShared<FJsonValueString>(W));
	}
	// Lower errors that are non-fatal get surfaced as warnings in preview
	for (const FOliveIRBlueprintPlanError& E : LowerResult.Errors)
	{
		if (!E.Message.IsEmpty())
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("[%s] %s"), *E.ErrorCode, *E.Message)));
		}
	}
	return Warnings;
}

/**
 * Serialize an array of FOliveIRBlueprintPlanError to a JSON array.
 *
 * @param Errors The errors to serialize
 * @return JSON array of error objects
 */
TArray<TSharedPtr<FJsonValue>> SerializePlanErrors(const TArray<FOliveIRBlueprintPlanError>& Errors)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FOliveIRBlueprintPlanError& Err : Errors)
	{
		Result.Add(MakeShared<FJsonValueObject>(Err.ToJson()));
	}
	return Result;
}

/**
 * Serialize step-level plan errors into a compact shape optimized for AI repair loops.
 *
 * @param Errors Structured plan errors
 * @return JSON array with step_id/error_code/message/suggestion/location
 */
TArray<TSharedPtr<FJsonValue>> SerializePlanStepErrors(const TArray<FOliveIRBlueprintPlanError>& Errors)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FOliveIRBlueprintPlanError& Err : Errors)
	{
		TSharedPtr<FJsonObject> StepErr = MakeShared<FJsonObject>();
		if (!Err.StepId.IsEmpty())
		{
			StepErr->SetStringField(TEXT("step_id"), Err.StepId);
		}
		if (!Err.ErrorCode.IsEmpty())
		{
			StepErr->SetStringField(TEXT("error_code"), Err.ErrorCode);
		}
		StepErr->SetStringField(TEXT("message"), Err.Message);
		if (!Err.Suggestion.IsEmpty())
		{
			StepErr->SetStringField(TEXT("suggestion"), Err.Suggestion);
		}
		if (!Err.LocationPointer.IsEmpty())
		{
			StepErr->SetStringField(TEXT("location"), Err.LocationPointer);
		}
		Result.Add(MakeShared<FJsonValueObject>(StepErr));
	}
	return Result;
}

/**
 * Build a human-readable summary that includes the first failing step.
 *
 * @param Prefix Failure prefix (e.g. "Plan resolution failed")
 * @param Errors Structured errors
 * @return Summary string
 */
FString BuildPlanFailureMessage(const FString& Prefix, const TArray<FOliveIRBlueprintPlanError>& Errors)
{
	if (Errors.Num() <= 0)
	{
		return Prefix;
	}

	const FOliveIRBlueprintPlanError& First = Errors[0];
	const FString StepLabel = First.StepId.IsEmpty()
		? TEXT("plan")
		: FString::Printf(TEXT("step '%s'"), *First.StepId);

	return FString::Printf(
		TEXT("%s with %d error(s). First failure (%s): [%s] %s"),
		*Prefix, Errors.Num(), *StepLabel, *First.ErrorCode, *First.Message);
}

/**
 * Remove nodes created by a failed plan execution, restoring the graph to
 * its pre-plan state. Reused event nodes (identified by ReusedStepIds) are
 * NOT removed. This runs as a separate FScopedTransaction so it appears as
 * "Olive AI: Rollback failed plan" in the undo history.
 *
 * @param Blueprint      The Blueprint containing the graph
 * @param Graph          The graph from which nodes are removed
 * @param StepToNodeMap  JSON object mapping StepId -> NodeId
 * @param ReusedStepIds  Step IDs that reused pre-existing event nodes (skip these)
 * @param AssetPath      Asset path for GraphWriter cache operations
 * @return Number of nodes removed
 */
int32 RollbackPlanNodes(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FJsonObject& StepToNodeMap,
	const TSet<FString>& ReusedStepIds,
	const FString& AssetPath)
{
	if (!Blueprint || !Graph)
	{
		return 0;
	}

	FScopedTransaction Transaction(
		NSLOCTEXT("OliveBPTools", "RollbackPlan", "Olive AI: Rollback failed plan"));
	Blueprint->Modify();

	FOliveGraphWriter& Writer = FOliveGraphWriter::Get();
	int32 RemovedCount = 0;

	for (const auto& Pair : StepToNodeMap.Values)
	{
		const FString& StepId = Pair.Key;
		if (ReusedStepIds.Contains(StepId))
		{
			continue; // Do not remove pre-existing event nodes
		}

		const FString NodeId = Pair.Value->AsString();
		UEdGraphNode* Node = Writer.GetCachedNode(AssetPath, NodeId);
		if (Node && Graph->Nodes.Contains(Node))
		{
			Node->BreakAllNodeLinks();
			Graph->RemoveNode(Node);
			RemovedCount++;
		}
	}

	// Clear GraphWriter cache for this Blueprint (node IDs are now invalid)
	if (RemovedCount > 0)
	{
		Writer.ClearNodeCache(AssetPath);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		UE_LOG(LogOliveBPTools, Log,
			TEXT("Rolled back %d nodes from failed plan on '%s'"),
			RemovedCount, *Blueprint->GetName());
	}

	return RemovedCount;
}

} // namespace

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintPreviewPlanJson(const TSharedPtr<FJsonObject>& Params)
{
	// ------------------------------------------------------------------
	// 1. Validate parameters
	// ------------------------------------------------------------------
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintPreviewPlanJson: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'asset_path' and 'plan_json' fields"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintPreviewPlanJson: Missing required param 'asset_path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'asset_path' is missing or empty"),
			TEXT("Provide the Blueprint asset path"));
	}

	const TSharedPtr<FJsonObject>* PlanJsonPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("plan_json"), PlanJsonPtr) || !PlanJsonPtr || !(*PlanJsonPtr).IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintPreviewPlanJson: Missing required param 'plan_json' for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'plan_json' is missing or not an object"),
			TEXT("Provide a valid plan_json object with 'steps' array"));
	}
	TSharedPtr<FJsonObject> PlanJson = *PlanJsonPtr;

	FString GraphTarget = TEXT("EventGraph");
	Params->TryGetStringField(TEXT("graph_target"), GraphTarget);

	FString Mode = TEXT("merge");
	Params->TryGetStringField(TEXT("mode"), Mode);

	// ------------------------------------------------------------------
	// 2. Validate plan schema
	// ------------------------------------------------------------------
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	const int32 MaxSteps = Settings ? Settings->PlanJsonMaxSteps : 128;

	FOliveIRResult ValidationResult = FOliveIRValidator::ValidateBlueprintPlanJson(PlanJson, MaxSteps);
	if (!ValidationResult.bSuccess)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("phase"), TEXT("validation"));
		ErrorData->SetStringField(TEXT("error_code"), ValidationResult.ErrorCode);
		ErrorData->SetStringField(TEXT("error_message"), ValidationResult.ErrorMessage);
		if (ValidationResult.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			if (ValidationResult.Data->TryGetArrayField(TEXT("errors"), Errors))
			{
				ErrorData->SetArrayField(TEXT("errors"), *Errors);
			}
		}
		FOliveToolResult Result = FOliveToolResult::Error(
			ValidationResult.ErrorCode,
			ValidationResult.ErrorMessage,
			ValidationResult.Suggestion);
		Result.Data = ErrorData;
		return Result;
	}

	// ------------------------------------------------------------------
	// 3. Parse plan
	// ------------------------------------------------------------------
	FOliveIRBlueprintPlan Plan = FOliveIRBlueprintPlan::FromJson(PlanJson);

	// ------------------------------------------------------------------
	// 4. Load Blueprint
	// ------------------------------------------------------------------
	FOliveBlueprintReader& BPReader = FOliveBlueprintReader::Get();
	UBlueprint* Blueprint = BPReader.LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintPreviewPlanJson: Failed to load Blueprint at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to load Blueprint at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists"));
	}

	// ------------------------------------------------------------------
	// 5. Find target graph (preview is non-mutating — report new graph in diff)
	// ------------------------------------------------------------------
	bool bGraphWillBeCreated = false;
	UEdGraph* TargetGraph = FindGraphByName(Blueprint, GraphTarget);
	if (!TargetGraph)
	{
		// EventGraph must exist; for other targets (function graphs), preview
		// still succeeds but notes the graph will be created on apply.
		if (GraphTarget == TEXT("EventGraph"))
		{
			return FOliveToolResult::Error(
				TEXT("GRAPH_NOT_FOUND"),
				FString::Printf(TEXT("EventGraph not found in Blueprint '%s'"), *AssetPath),
				TEXT("EventGraph should always exist on a Blueprint"));
		}
		bGraphWillBeCreated = true;
	}

	// ------------------------------------------------------------------
	// 6. Read current graph IR (for fingerprint + diff)
	// ------------------------------------------------------------------
	FOliveGraphReader GraphReader;
	FOliveIRGraph CurrentGraphIR;
	if (TargetGraph)
	{
		CurrentGraphIR = GraphReader.ReadGraph(TargetGraph, Blueprint);
	}
	// else: empty IR for a graph that will be created

	// ------------------------------------------------------------------
	// 7. Resolve plan
	// ------------------------------------------------------------------
	FOliveGraphContext GraphContext = FOliveGraphContext::BuildFromBlueprint(Blueprint, GraphTarget);
	FOlivePlanResolveResult ResolveResult = FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint, GraphContext);
	if (!ResolveResult.bSuccess)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("phase"), TEXT("resolve"));
		ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(ResolveResult.Errors));
		ErrorData->SetArrayField(TEXT("step_errors"), SerializePlanStepErrors(ResolveResult.Errors));
		if (ResolveResult.Errors.Num() > 0)
		{
			const FOliveIRBlueprintPlanError& First = ResolveResult.Errors[0];
			if (!First.StepId.IsEmpty())
			{
				ErrorData->SetStringField(TEXT("first_error_step_id"), First.StepId);
			}
			if (!First.ErrorCode.IsEmpty())
			{
				ErrorData->SetStringField(TEXT("first_error_code"), First.ErrorCode);
			}
			ErrorData->SetStringField(TEXT("first_error_message"), First.Message);
		}
		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("PLAN_RESOLVE_FAILED"),
			BuildPlanFailureMessage(TEXT("Plan resolution failed"), ResolveResult.Errors),
			ResolveResult.Errors.Num() > 0 ? ResolveResult.Errors[0].Suggestion : TEXT(""));
		Result.Data = ErrorData;
		return Result;
	}

	// Use the expanded plan (with all resolver expansions applied) for all
	// post-resolve processing. The original Plan must NOT be used after this
	// point because it lacks synthetic steps from ExpandComponentRefs/etc.
	FOliveIRBlueprintPlan& ExpandedPlan = ResolveResult.ExpandedPlan;

	// Collapse exec chains through pure steps (pure nodes have no exec pins)
	FOliveBlueprintPlanResolver::CollapseExecThroughPureSteps(
		ExpandedPlan, ResolveResult.ResolvedSteps, ResolveResult.GlobalNotes);

	// Auto-fix exec_after/exec_outputs conflicts before validation (RC5)
	FOlivePlanValidator::AutoFixExecConflicts(ExpandedPlan, ResolveResult.GlobalNotes);

	// ------------------------------------------------------------------
	// 7b. Phase 0: Structural plan validation
	// ------------------------------------------------------------------
	{
		FOlivePlanValidationResult Phase0Result = FOlivePlanValidator::Validate(
			ExpandedPlan, ResolveResult.ResolvedSteps, Blueprint, GraphContext);

		if (!Phase0Result.bSuccess)
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("phase"), TEXT("phase0_validation"));
			ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(Phase0Result.Errors));
			FOliveToolResult Result = FOliveToolResult::Error(
				TEXT("PLAN_VALIDATION_FAILED"),
				FString::Printf(TEXT("Plan validation failed with %d error(s)"), Phase0Result.Errors.Num()),
				Phase0Result.Errors.Num() > 0 ? Phase0Result.Errors[0].Suggestion : TEXT(""));
			Result.Data = ErrorData;
			return Result;
		}
	}

	// ------------------------------------------------------------------
	// 8. Lower (v1.0 only) or skip (v2.0)
	// ------------------------------------------------------------------
	const bool bIsV2Plan = (ExpandedPlan.SchemaVersion == TEXT("2.0"));

	FOlivePlanLowerResult LowerResult;
	if (!bIsV2Plan)
	{
		LowerResult = FOliveBlueprintPlanLowerer::Lower(
			ResolveResult.ResolvedSteps, ExpandedPlan, GraphTarget, AssetPath);
		if (!LowerResult.bSuccess)
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("phase"), TEXT("lower"));
			ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(LowerResult.Errors));
			FOliveToolResult Result = FOliveToolResult::Error(
				TEXT("PLAN_LOWER_FAILED"),
				FString::Printf(TEXT("Plan lowering failed with %d error(s)"), LowerResult.Errors.Num()),
				LowerResult.Errors.Num() > 0 ? LowerResult.Errors[0].Suggestion : TEXT(""));
			Result.Data = ErrorData;
			return Result;
		}
	}

	// ------------------------------------------------------------------
	// 9. Compute fingerprint and diff
	// ------------------------------------------------------------------
	FString Fingerprint = FOliveBlueprintPlanResolver::ComputePlanFingerprint(CurrentGraphIR, ExpandedPlan);
	TSharedPtr<FJsonObject> Diff = FOliveBlueprintPlanResolver::ComputePlanDiff(
		CurrentGraphIR, ResolveResult.ResolvedSteps, ExpandedPlan);

	// ------------------------------------------------------------------
	// 10. Build result
	// ------------------------------------------------------------------
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("preview_fingerprint"), Fingerprint);
	ResultData->SetStringField(TEXT("schema_version"), ExpandedPlan.SchemaVersion);
	ResultData->SetObjectField(TEXT("diff"), Diff);

	if (bIsV2Plan)
	{
		// v2.0: No lowered ops. Report step count and resolved function names.
		ResultData->SetNumberField(TEXT("resolved_steps_count"), ResolveResult.ResolvedSteps.Num());

		// Include per-step resolution summary so AI can verify resolved function names
		TArray<TSharedPtr<FJsonValue>> StepSummaries;
		StepSummaries.Reserve(ResolveResult.ResolvedSteps.Num());
		for (const FOliveResolvedStep& Resolved : ResolveResult.ResolvedSteps)
		{
			TSharedPtr<FJsonObject> StepObj = MakeShared<FJsonObject>();
			StepObj->SetStringField(TEXT("step_id"), Resolved.StepId);
			StepObj->SetStringField(TEXT("node_type"), Resolved.NodeType);

			// Include resolved function_name and target_class if present
			const FString* FnName = Resolved.Properties.Find(TEXT("function_name"));
			if (FnName && !FnName->IsEmpty())
			{
				StepObj->SetStringField(TEXT("resolved_function"), *FnName);
			}
			const FString* TargetCls = Resolved.Properties.Find(TEXT("target_class"));
			if (TargetCls && !TargetCls->IsEmpty())
			{
				StepObj->SetStringField(TEXT("resolved_class"), *TargetCls);
			}

			StepSummaries.Add(MakeShared<FJsonValueObject>(StepObj));
		}
		ResultData->SetArrayField(TEXT("resolved_steps"), StepSummaries);
		ResultData->SetStringField(TEXT("execution_mode"), TEXT("plan_executor_v2"));
	}
	else
	{
		// v1.0: Include lowered ops summary
		ResultData->SetObjectField(TEXT("plan_summary"), BuildPlanSummary(ExpandedPlan, LowerResult));
		ResultData->SetNumberField(TEXT("lowered_ops_count"), LowerResult.Ops.Num());
		ResultData->SetStringField(TEXT("execution_mode"), TEXT("lowerer_v1"));
	}

	if (bGraphWillBeCreated)
	{
		ResultData->SetBoolField(TEXT("will_create_graph"), true);
		ResultData->SetStringField(TEXT("new_graph_name"), GraphTarget);
	}

	// Collect warnings from resolution (and lowering for v1.0)
	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (!bIsV2Plan)
	{
		Warnings = CollectWarnings(ResolveResult, LowerResult);
	}
	else
	{
		// v2.0: warnings come from resolution only
		for (const FString& Warn : ResolveResult.Warnings)
		{
			Warnings.Add(MakeShared<FJsonValueString>(Warn));
		}
	}

	if (bGraphWillBeCreated)
	{
		Warnings.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("Function graph '%s' does not exist and will be created on apply"), *GraphTarget)));
	}
	if (Warnings.Num() > 0)
	{
		ResultData->SetArrayField(TEXT("warnings"), Warnings);
	}

	// Serialize resolver notes for transparency (e.g., synthetic MakeTransform steps)
	if (ResolveResult.GlobalNotes.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotesArray;
		NotesArray.Reserve(ResolveResult.GlobalNotes.Num());
		for (const FOliveResolverNote& Note : ResolveResult.GlobalNotes)
		{
			NotesArray.Add(MakeShared<FJsonValueObject>(Note.ToJson()));
		}
		ResultData->SetArrayField(TEXT("resolver_notes"), NotesArray);
	}

	UE_LOG(LogOliveBPTools, Log,
		TEXT("Plan preview for '%s' graph '%s': %d steps, schema=%s, fingerprint=%s, new_graph=%s"),
		*AssetPath, *GraphTarget, ExpandedPlan.Steps.Num(), *ExpandedPlan.SchemaVersion, *Fingerprint,
		bGraphWillBeCreated ? TEXT("true") : TEXT("false"));

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintApplyPlanJson(const TSharedPtr<FJsonObject>& Params)
{
	// ------------------------------------------------------------------
	// 1. Validate parameters
	// ------------------------------------------------------------------
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintApplyPlanJson: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'asset_path' and 'plan_json' fields"));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintApplyPlanJson: Missing required param 'asset_path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'asset_path' is missing or empty"),
			TEXT("Provide the Blueprint asset path"));
	}

	const TSharedPtr<FJsonObject>* PlanJsonPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("plan_json"), PlanJsonPtr) || !PlanJsonPtr || !(*PlanJsonPtr).IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintApplyPlanJson: Missing required param 'plan_json' for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'plan_json' is missing or not an object"),
			TEXT("Provide a valid plan_json object with 'steps' array"));
	}
	TSharedPtr<FJsonObject> PlanJson = *PlanJsonPtr;

	FString GraphTarget = TEXT("EventGraph");
	Params->TryGetStringField(TEXT("graph_target"), GraphTarget);

	FString Mode = TEXT("merge");
	Params->TryGetStringField(TEXT("mode"), Mode);

	FString ProvidedFingerprint;
	Params->TryGetStringField(TEXT("preview_fingerprint"), ProvidedFingerprint);

	// ------------------------------------------------------------------
	// 2. Validate plan schema
	// ------------------------------------------------------------------
	const UOliveAISettings* Settings = UOliveAISettings::Get();
	const int32 MaxSteps = Settings ? Settings->PlanJsonMaxSteps : 128;

	FOliveIRResult ValidationResult = FOliveIRValidator::ValidateBlueprintPlanJson(PlanJson, MaxSteps);
	if (!ValidationResult.bSuccess)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("phase"), TEXT("validation"));
		ErrorData->SetStringField(TEXT("error_code"), ValidationResult.ErrorCode);
		ErrorData->SetStringField(TEXT("error_message"), ValidationResult.ErrorMessage);
		if (ValidationResult.Data.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
			if (ValidationResult.Data->TryGetArrayField(TEXT("errors"), Errors))
			{
				ErrorData->SetArrayField(TEXT("errors"), *Errors);
			}
		}
		FOliveToolResult Result = FOliveToolResult::Error(
			ValidationResult.ErrorCode,
			ValidationResult.ErrorMessage,
			ValidationResult.Suggestion);
		Result.Data = ErrorData;
		return Result;
	}

	// ------------------------------------------------------------------
	// 3. Parse plan
	// ------------------------------------------------------------------
	FOliveIRBlueprintPlan Plan = FOliveIRBlueprintPlan::FromJson(PlanJson);

	// ------------------------------------------------------------------
	// 4. Load Blueprint
	// ------------------------------------------------------------------
	FOliveBlueprintReader& BPReader = FOliveBlueprintReader::Get();
	UBlueprint* Blueprint = BPReader.LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintApplyPlanJson: Failed to load Blueprint at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to load Blueprint at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists"));
	}

	// ------------------------------------------------------------------
	// 5. Check preview requirement
	// ------------------------------------------------------------------
	if (ProvidedFingerprint.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Log,
			TEXT("apply_plan_json: no preview_fingerprint provided — skipping drift check, "
				 "proceeding with resolve+execute validation."));
	}

	// ------------------------------------------------------------------
	// 6. Find target graph (create later inside pipeline transaction if needed)
	// ------------------------------------------------------------------
	UEdGraph* TargetGraph = FindGraphByName(Blueprint, GraphTarget);
	const bool bGraphMissing = (TargetGraph == nullptr);
	if (bGraphMissing && GraphTarget == TEXT("EventGraph"))
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintApplyPlanJson: EventGraph not found in Blueprint '%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("GRAPH_NOT_FOUND"),
			FString::Printf(TEXT("EventGraph not found in Blueprint '%s'"), *AssetPath),
			TEXT("EventGraph should always exist on a Blueprint"));
	}

	// ------------------------------------------------------------------
	// 7. Resolve plan (always -- needed before drift detection and everything else)
	// ------------------------------------------------------------------
	FOliveGraphContext GraphContext = FOliveGraphContext::BuildFromBlueprint(Blueprint, GraphTarget);
	FOlivePlanResolveResult ResolveResult = FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint, GraphContext);
	if (!ResolveResult.bSuccess)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("phase"), TEXT("resolve"));
		ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(ResolveResult.Errors));
		ErrorData->SetArrayField(TEXT("step_errors"), SerializePlanStepErrors(ResolveResult.Errors));
		if (ResolveResult.Errors.Num() > 0)
		{
			const FOliveIRBlueprintPlanError& First = ResolveResult.Errors[0];
			if (!First.StepId.IsEmpty())
			{
				ErrorData->SetStringField(TEXT("first_error_step_id"), First.StepId);
			}
			if (!First.ErrorCode.IsEmpty())
			{
				ErrorData->SetStringField(TEXT("first_error_code"), First.ErrorCode);
			}
			ErrorData->SetStringField(TEXT("first_error_message"), First.Message);
		}
		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("PLAN_RESOLVE_FAILED"),
			BuildPlanFailureMessage(TEXT("Plan resolution failed"), ResolveResult.Errors),
			ResolveResult.Errors.Num() > 0 ? ResolveResult.Errors[0].Suggestion : TEXT(""));
		Result.Data = ErrorData;
		return Result;
	}

	// Use the expanded plan (with all resolver expansions applied) for all
	// post-resolve processing. The original Plan must NOT be used after this
	// point because it lacks synthetic steps from ExpandComponentRefs/etc.
	FOliveIRBlueprintPlan& ExpandedPlan = ResolveResult.ExpandedPlan;

	// ------------------------------------------------------------------
	// 7b. Drift detection (if fingerprint provided and graph already existed)
	// Uses ExpandedPlan so fingerprint matches what preview_plan_json computed.
	// ------------------------------------------------------------------
	if (!ProvidedFingerprint.IsEmpty() && !bGraphMissing)
	{
		FOliveGraphReader DriftReader;
		FOliveIRGraph CurrentGraphIR = DriftReader.ReadGraph(TargetGraph, Blueprint);
		FString CurrentFingerprint = FOliveBlueprintPlanResolver::ComputePlanFingerprint(CurrentGraphIR, ExpandedPlan);

		// Tolerant comparison: case-insensitive + prefix match (LLMs truncate/lowercase hex)
		const bool bExactMatch = CurrentFingerprint.Equals(ProvidedFingerprint, ESearchCase::IgnoreCase);
		const bool bPrefixMatch = !bExactMatch
			&& ProvidedFingerprint.Len() >= 6
			&& CurrentFingerprint.StartsWith(ProvidedFingerprint, ESearchCase::IgnoreCase);

		if (!bExactMatch && !bPrefixMatch)
		{
			UE_LOG(LogOliveBPTools, Warning,
				TEXT("apply_plan_json: fingerprint mismatch (provided='%s', current='%s'). "
					 "Proceeding anyway — resolve+execute pipeline will validate."),
				*ProvidedFingerprint, *CurrentFingerprint);
		}
		else if (bPrefixMatch)
		{
			UE_LOG(LogOliveBPTools, Log,
				TEXT("apply_plan_json: fingerprint prefix match accepted (provided='%s' -> current='%s')"),
				*ProvidedFingerprint, *CurrentFingerprint);
		}
	}

	// ------------------------------------------------------------------
	// 8. Post-resolve passes + Lower (v1.0 only)
	// ------------------------------------------------------------------
	const bool bIsV2Plan = (ExpandedPlan.SchemaVersion == TEXT("2.0"));

	// Collapse exec chains through pure steps (pure nodes have no exec pins)
	FOliveBlueprintPlanResolver::CollapseExecThroughPureSteps(
		ExpandedPlan, ResolveResult.ResolvedSteps, ResolveResult.GlobalNotes);

	// Auto-fix exec_after/exec_outputs conflicts before validation (RC5)
	FOlivePlanValidator::AutoFixExecConflicts(ExpandedPlan, ResolveResult.GlobalNotes);

	// ------------------------------------------------------------------
	// 8b. Phase 0: Structural plan validation (before execution)
	// ------------------------------------------------------------------
	{
		FOlivePlanValidationResult Phase0Result = FOlivePlanValidator::Validate(
			ExpandedPlan, ResolveResult.ResolvedSteps, Blueprint, GraphContext);

		if (!Phase0Result.bSuccess)
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("phase"), TEXT("phase0_validation"));
			ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(Phase0Result.Errors));
			FOliveToolResult Result = FOliveToolResult::Error(
				TEXT("PLAN_VALIDATION_FAILED"),
				FString::Printf(TEXT("Plan validation failed with %d error(s)"), Phase0Result.Errors.Num()),
				Phase0Result.Errors.Num() > 0 ? Phase0Result.Errors[0].Suggestion : TEXT(""));
			Result.Data = ErrorData;
			return Result;
		}
	}

	// v1.0 path: lower to batch ops (v2.0 skips this entirely)
	FOlivePlanLowerResult LowerResult;
	if (!bIsV2Plan)
	{
		LowerResult = FOliveBlueprintPlanLowerer::Lower(
			ResolveResult.ResolvedSteps, ExpandedPlan, GraphTarget, AssetPath);
		if (!LowerResult.bSuccess)
		{
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("phase"), TEXT("lower"));
			ErrorData->SetArrayField(TEXT("errors"), SerializePlanErrors(LowerResult.Errors));
			FOliveToolResult Result = FOliveToolResult::Error(
				TEXT("PLAN_LOWER_FAILED"),
				FString::Printf(TEXT("Plan lowering failed with %d error(s)"), LowerResult.Errors.Num()),
				LowerResult.Errors.Num() > 0 ? LowerResult.Errors[0].Suggestion : TEXT(""));
			Result.Data = ErrorData;
			return Result;
		}
	}

	// ------------------------------------------------------------------
	// 9. Build write request
	// ------------------------------------------------------------------
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.apply_plan_json");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::Format(
		NSLOCTEXT("OliveBPTools", "ApplyPlanJson", "AI Agent: Apply Plan JSON ({0} steps, v{1})"),
		FText::AsNumber(ExpandedPlan.Steps.Num()),
		FText::FromString(ExpandedPlan.SchemaVersion));
	Request.OperationCategory = TEXT("plan_apply");
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true;
	Request.bSkipVerification = false;

	// ------------------------------------------------------------------
	// 10. Build executor delegate (version-gated)
	// ------------------------------------------------------------------
	FOliveWriteExecutor Executor;

	if (bIsV2Plan)
	{
		// ============================================================
		// v2.0 PATH: FOlivePlanExecutor with pin introspection
		// ============================================================

		// Capture resolved steps, expanded plan, and resolver notes by value for the lambda.
		// ResolvedSteps is small (one struct per step). ExpandedPlan is also small.
		// GlobalNotes carry transparency data from ExpandPlanInputs/ExpandComponentRefs.
		TArray<FOliveResolvedStep> CapturedResolvedSteps = ResolveResult.ResolvedSteps;
		FOliveIRBlueprintPlan CapturedPlan = ExpandedPlan;
		TArray<FOliveResolverNote> CapturedResolverNotes = ResolveResult.GlobalNotes;

		Executor.BindLambda(
			[CapturedResolvedSteps = MoveTemp(CapturedResolvedSteps),
			 CapturedPlan = MoveTemp(CapturedPlan),
			 CapturedResolverNotes = MoveTemp(CapturedResolverNotes),
			 CapturedMode = Mode,
			 AssetPath, GraphTarget]
			(const FOliveWriteRequest& InRequest, UObject* TargetAsset) -> FOliveWriteResult
			{
				UBlueprint* BP = Cast<UBlueprint>(TargetAsset);
				if (!BP)
				{
					return FOliveWriteResult::ExecutionError(
						TEXT("INVALID_TARGET"),
						TEXT("Target asset is not a valid Blueprint"),
						TEXT("Ensure the asset_path points to a Blueprint"));
				}

				// Suppress inner transactions -- the pipeline owns the outer transaction
				FOliveBatchExecutionScope BatchScope;

				BP->Modify();

				// Ensure target graph exists inside the pipeline transaction
				bool bGraphCreatedInTxn = false;
				UEdGraph* ExecutionGraph = FindOrCreateFunctionGraph(BP, GraphTarget, bGraphCreatedInTxn);
				if (!ExecutionGraph)
				{
					return FOliveWriteResult::ExecutionError(
						TEXT("GRAPH_NOT_FOUND"),
						FString::Printf(TEXT("Graph '%s' not found and could not be created"), *GraphTarget),
						TEXT("EventGraph must already exist; other names are created as function graphs."));
				}

				// Replace mode: clear graph of non-entry nodes before plan execution
				if (CapturedMode == TEXT("replace") && ExecutionGraph)
				{
					TArray<UEdGraphNode*> NodesToRemove;
					for (UEdGraphNode* Node : ExecutionGraph->Nodes)
					{
						if (!Node) continue;
						// Keep event nodes and function entry/result nodes
						if (Node->IsA<UK2Node_Event>()) continue;
						if (Node->IsA<UK2Node_CustomEvent>()) continue;
						if (Node->IsA<UK2Node_FunctionEntry>()) continue;
						if (Node->IsA<UK2Node_FunctionResult>()) continue;
						NodesToRemove.Add(Node);
					}

					for (UEdGraphNode* Node : NodesToRemove)
					{
						Node->BreakAllNodeLinks();
						ExecutionGraph->RemoveNode(Node);
					}

					// Clear cache since we removed nodes
					FOliveGraphWriter::Get().ClearNodeCache(AssetPath);

					UE_LOG(LogOliveBPTools, Log,
						TEXT("Replace mode: cleared %d non-entry nodes from graph '%s'"),
						NodesToRemove.Num(), *GraphTarget);
				}

				// Execute the multi-phase plan
				FOlivePlanExecutor PlanExecutor;
				FOliveIRBlueprintPlanResult PlanResult = PlanExecutor.Execute(
					CapturedPlan, CapturedResolvedSteps,
					BP, ExecutionGraph, AssetPath, GraphTarget);

				// Build the result data JSON
				TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();

				// step_to_node_map
				TSharedPtr<FJsonObject> MapObj = MakeShared<FJsonObject>();
				for (const auto& Pair : PlanResult.StepToNodeMap)
				{
					MapObj->SetStringField(Pair.Key, Pair.Value);
				}
				ResultData->SetObjectField(TEXT("step_to_node_map"), MapObj);
				ResultData->SetNumberField(TEXT("applied_ops_count"), PlanResult.AppliedOpsCount);
				ResultData->SetStringField(TEXT("schema_version"), TEXT("2.0"));

				// Serialize reused step IDs (for rollback: these event nodes survive cleanup)
				if (PlanResult.ReusedStepIds.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> ReusedArr;
					ReusedArr.Reserve(PlanResult.ReusedStepIds.Num());
					for (const FString& Id : PlanResult.ReusedStepIds)
					{
						ReusedArr.Add(MakeShared<FJsonValueString>(Id));
					}
					ResultData->SetArrayField(TEXT("reused_step_ids"), ReusedArr);
				}

				// Serialize plan ownership metadata (for stale error detection)
				if (PlanResult.PlanClassNames.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> ClassArr;
					ClassArr.Reserve(PlanResult.PlanClassNames.Num());
					for (const FString& CN : PlanResult.PlanClassNames)
					{
						ClassArr.Add(MakeShared<FJsonValueString>(CN));
					}
					ResultData->SetArrayField(TEXT("plan_class_names"), ClassArr);
				}
				if (PlanResult.PlanFunctionNames.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> FuncArr;
					FuncArr.Reserve(PlanResult.PlanFunctionNames.Num());
					for (const FString& FN : PlanResult.PlanFunctionNames)
					{
						FuncArr.Add(MakeShared<FJsonValueString>(FN));
					}
					ResultData->SetArrayField(TEXT("plan_function_names"), FuncArr);
				}

				// Serialize wiring errors (for AI self-correction)
				if (PlanResult.Errors.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> ErrorsArr;
					ErrorsArr.Reserve(PlanResult.Errors.Num());
					for (const FOliveIRBlueprintPlanError& Err : PlanResult.Errors)
					{
						ErrorsArr.Add(MakeShared<FJsonValueObject>(Err.ToJson()));
					}
					ResultData->SetArrayField(TEXT("wiring_errors"), ErrorsArr);
				}

				// Serialize warnings
				if (PlanResult.Warnings.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> WarningsArr;
					WarningsArr.Reserve(PlanResult.Warnings.Num());
					for (const FString& Warn : PlanResult.Warnings)
					{
						WarningsArr.Add(MakeShared<FJsonValueString>(Warn));
					}
					ResultData->SetArrayField(TEXT("warnings"), WarningsArr);
				}

				// Forward pin manifests for AI self-correction
				if (PlanResult.PinManifestJsons.Num() > 0)
				{
					TSharedPtr<FJsonObject> ManifestsObj = MakeShared<FJsonObject>();
					for (const auto& Pair : PlanResult.PinManifestJsons)
					{
						ManifestsObj->SetObjectField(Pair.Key, Pair.Value);
					}
					ResultData->SetObjectField(TEXT("pin_manifests"), ManifestsObj);
				}

				// Serialize resolver notes for transparency (e.g., synthetic MakeTransform steps).
				// These tell the AI what the resolver did silently so it can adjust its mental model.
				if (CapturedResolverNotes.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> NotesArray;
					NotesArray.Reserve(CapturedResolverNotes.Num());
					for (const FOliveResolverNote& Note : CapturedResolverNotes)
					{
						NotesArray.Add(MakeShared<FJsonValueObject>(Note.ToJson()));
					}
					ResultData->SetArrayField(TEXT("resolver_notes"), NotesArray);
				}

				// Serialize conversion notes for transparency (e.g., Vector->Transform auto-conversion).
				// These tell the AI what type coercions the wiring phase inserted silently.
				if (PlanResult.ConversionNotesJson.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> ConvNotesArray;
					ConvNotesArray.Reserve(PlanResult.ConversionNotesJson.Num());
					for (const TSharedPtr<FJsonObject>& NoteJson : PlanResult.ConversionNotesJson)
					{
						if (NoteJson.IsValid())
						{
							ConvNotesArray.Add(MakeShared<FJsonValueObject>(NoteJson));
						}
					}
					ResultData->SetArrayField(TEXT("conversion_notes"), ConvNotesArray);
				}

				// Self-correction hint when wiring partially failed
				const bool bHasWiringErrors =
					(PlanResult.Errors.Num() > 0) &&
					PlanResult.bSuccess; // Only include hint on partial success, not total failure
				if (bHasWiringErrors)
				{
					ResultData->SetStringField(TEXT("self_correction_hint"),
						TEXT("Some wiring failed. Use blueprint.read on the target graph to see "
							 "actual pin names on created nodes, then use granular connect_pins/set_pin_default "
							 "to fix failed connections. See wiring_errors for details."));
				}

				if (!PlanResult.bSuccess)
				{
					// Node creation failed entirely
					FOliveWriteResult ErrorResult = FOliveWriteResult::ExecutionError(
						TEXT("PLAN_EXECUTION_FAILED"),
						FString::Printf(TEXT("Plan execution failed: %d of %d nodes created"),
							static_cast<int32>(PlanResult.StepToNodeMap.Num()),
							CapturedPlan.Steps.Num()),
						PlanResult.Errors.Num() > 0 ? PlanResult.Errors[0].Suggestion : TEXT(""));
					ErrorResult.ResultData = ResultData;
					return ErrorResult;
				}

				// Partial success: some wiring failed but all nodes were created.
				// Return Success so the write pipeline commits the transaction
				// (nodes persist in the graph). The AI uses wiring_errors +
				// step_to_node_map to fix failed connections with connect_pins.
				if (PlanResult.bPartial)
				{
					const int32 TotalFailures = PlanResult.ConnectionsFailed + PlanResult.DefaultsFailed;
					FString PartialMessage = FString::Printf(
						TEXT("%d nodes created, %d connections succeeded, %d connections FAILED. "
							 "Nodes are committed. Use wiring_errors and step_to_node_map to fix "
							 "failed connections with connect_pins/set_pin_default."),
						PlanResult.StepToNodeMap.Num(),
						PlanResult.ConnectionsSucceeded,
						TotalFailures);

					ResultData->SetStringField(TEXT("message"), PartialMessage);
					ResultData->SetStringField(TEXT("status"), TEXT("partial_success"));
					ResultData->SetBoolField(TEXT("success"), true);
					// Deliberately NOT setting error_code -- partial success is not an error

					FOliveWriteResult PartialResult = FOliveWriteResult::Success(ResultData);

					// Provide created node IDs for the pipeline's verification stage
					TArray<FString> CreatedNodeIds;
					CreatedNodeIds.Reserve(PlanResult.StepToNodeMap.Num());
					for (const auto& Pair : PlanResult.StepToNodeMap)
					{
						CreatedNodeIds.Add(Pair.Value);
					}
					PartialResult.CreatedNodeIds = MoveTemp(CreatedNodeIds);

					return PartialResult;
				}

				// Full success path (no partial failures)
				ResultData->SetStringField(TEXT("message"),
					FString::Printf(TEXT("Plan applied successfully: %d nodes created, %d connections wired"),
						PlanResult.StepToNodeMap.Num(),
						PlanResult.ConnectionsSucceeded));

				FOliveWriteResult SuccessResult = FOliveWriteResult::Success(ResultData);

				// Collect created node IDs for the pipeline's verification stage
				TArray<FString> CreatedNodeIds;
				CreatedNodeIds.Reserve(PlanResult.StepToNodeMap.Num());
				for (const auto& Pair : PlanResult.StepToNodeMap)
				{
					CreatedNodeIds.Add(Pair.Value);
				}
				SuccessResult.CreatedNodeIds = MoveTemp(CreatedNodeIds);

				return SuccessResult;
			});
	}
	else
	{
		// ============================================================
		// v1.0 PATH: Existing lowerer + batch dispatch (unchanged)
		// ============================================================
		TArray<FOliveLoweredOp> CapturedOps = LowerResult.Ops;
		TMap<FString, int32> CapturedStepMap = LowerResult.StepToFirstOpIndex;

		Executor.BindLambda(
			[CapturedOps = MoveTemp(CapturedOps), CapturedStepMap = MoveTemp(CapturedStepMap), AssetPath, GraphTarget]
			(const FOliveWriteRequest& InRequest, UObject* TargetAsset) -> FOliveWriteResult
			{
				UBlueprint* BP = Cast<UBlueprint>(TargetAsset);
				if (!BP)
				{
					return FOliveWriteResult::ExecutionError(
						TEXT("INVALID_TARGET"),
						TEXT("Target asset is not a valid Blueprint"),
						TEXT("Ensure the asset_path points to a Blueprint"));
				}

				// Suppress inner transactions -- the pipeline owns the outer transaction
				FOliveBatchExecutionScope BatchScope;

				BP->Modify();

				// Ensure target graph exists inside the pipeline transaction so creation
				// participates in rollback if later ops fail.
				bool bGraphCreatedInTxn = false;
				UEdGraph* ExecutionGraph = FindOrCreateFunctionGraph(BP, GraphTarget, bGraphCreatedInTxn);
				if (!ExecutionGraph)
				{
					return FOliveWriteResult::ExecutionError(
						TEXT("GRAPH_NOT_FOUND"),
						FString::Printf(TEXT("Graph '%s' not found and could not be created"), *GraphTarget),
						TEXT("EventGraph must already exist; other names are created as function graphs."));
				}

				TMap<FString, TSharedPtr<FJsonObject>> OpResultsById;
				TMap<FString, FString> StepToNodeMap;
				TArray<FString> CreatedNodeIds;
				int32 AppliedCount = 0;

				for (int32 i = 0; i < CapturedOps.Num(); ++i)
				{
					if (!CapturedOps[i].Params.IsValid())
					{
						return FOliveWriteResult::ExecutionError(
							TEXT("INVALID_OP_PARAMS"),
							FString::Printf(TEXT("Op %d has invalid null params (id='%s', tool='%s')"),
								i, *CapturedOps[i].Id, *CapturedOps[i].ToolName),
							TEXT("Regenerate the plan and retry."));
					}

					// Copy params so template resolution can mutate them
					TSharedPtr<FJsonObject> OpParams = MakeShared<FJsonObject>();
					for (const auto& Field : CapturedOps[i].Params->Values)
					{
						OpParams->Values.Add(Field.Key, Field.Value);
					}

					// Resolve ${opId.field} template references
					FString TemplateError;
					if (!FOliveGraphBatchExecutor::ResolveTemplateReferences(OpParams, OpResultsById, TemplateError))
					{
						return FOliveWriteResult::ExecutionError(
							TEXT("TEMPLATE_RESOLVE_FAILED"),
							FString::Printf(TEXT("Template resolution failed at op %d (id='%s'): %s"),
								i, *CapturedOps[i].Id, *TemplateError),
							TEXT("Check that referenced step IDs exist and produced node_id results"));
					}

					// Dispatch to writer
					FOliveBlueprintWriteResult WriteResult = FOliveGraphBatchExecutor::DispatchWriterOp(
						CapturedOps[i].ToolName, AssetPath, OpParams);

					if (!WriteResult.bSuccess)
					{
						FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");
						return FOliveWriteResult::ExecutionError(
							TEXT("OP_FAILED"),
							FString::Printf(TEXT("Op %d failed (id='%s', tool='%s'): %s"),
								i, *CapturedOps[i].Id, *CapturedOps[i].ToolName, *ErrorMsg),
							TEXT("Check the plan step definition for this operation"));
					}

					AppliedCount++;

					// Build result data for template resolution by later ops
					TSharedPtr<FJsonObject> OpData = MakeShared<FJsonObject>();
					if (!WriteResult.CreatedNodeId.IsEmpty())
					{
						OpData->SetStringField(TEXT("node_id"), WriteResult.CreatedNodeId);
						CreatedNodeIds.Add(WriteResult.CreatedNodeId);
					}
					if (!WriteResult.CreatedItemName.IsEmpty())
					{
						OpData->SetStringField(TEXT("item_name"), WriteResult.CreatedItemName);
					}

					// Store result for template resolution
					if (!CapturedOps[i].Id.IsEmpty())
					{
						OpResultsById.Add(CapturedOps[i].Id, OpData);

						// If this is an add_node op (has a step mapping), record in StepToNodeMap
						if (CapturedStepMap.Contains(CapturedOps[i].Id) && !WriteResult.CreatedNodeId.IsEmpty())
						{
							StepToNodeMap.Add(CapturedOps[i].Id, WriteResult.CreatedNodeId);
						}
					}
				}

				// Build success result
				TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();

				// Serialize step_to_node_map
				TSharedPtr<FJsonObject> MapObj = MakeShared<FJsonObject>();
				for (const auto& Pair : StepToNodeMap)
				{
					MapObj->SetStringField(Pair.Key, Pair.Value);
				}
				ResultData->SetObjectField(TEXT("step_to_node_map"), MapObj);
				ResultData->SetNumberField(TEXT("applied_ops_count"), AppliedCount);

				FOliveWriteResult SuccessResult = FOliveWriteResult::Success(ResultData);
				SuccessResult.CreatedNodeIds = MoveTemp(CreatedNodeIds);
				return SuccessResult;
			});
	}

	// ------------------------------------------------------------------
	// 11. Execute through write pipeline
	// ------------------------------------------------------------------
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult PipelineResult = Pipeline.Execute(Request, Executor);

	// Convert to tool result and inject data from executor result
	FOliveToolResult ToolResult = PipelineResult.ToToolResult();

	if (PipelineResult.bSuccess && PipelineResult.ResultData.IsValid() && ToolResult.Data.IsValid())
	{
		// Forward all fields from the executor's ResultData into the tool result
		const TSharedPtr<FJsonObject>* StepMapObj = nullptr;
		if (PipelineResult.ResultData->TryGetObjectField(TEXT("step_to_node_map"), StepMapObj))
		{
			ToolResult.Data->SetObjectField(TEXT("step_to_node_map"), *StepMapObj);
		}

		double AppliedOps = 0;
		if (PipelineResult.ResultData->TryGetNumberField(TEXT("applied_ops_count"), AppliedOps))
		{
			ToolResult.Data->SetNumberField(TEXT("applied_ops_count"), AppliedOps);
		}

		// v2.0-specific result fields
		if (bIsV2Plan)
		{
			FString SchemaVersion;
			if (PipelineResult.ResultData->TryGetStringField(TEXT("schema_version"), SchemaVersion))
			{
				ToolResult.Data->SetStringField(TEXT("schema_version"), SchemaVersion);
			}

			const TArray<TSharedPtr<FJsonValue>>* WiringErrors = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("wiring_errors"), WiringErrors))
			{
				ToolResult.Data->SetArrayField(TEXT("wiring_errors"), *WiringErrors);
			}

			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("warnings"), Warnings))
			{
				ToolResult.Data->SetArrayField(TEXT("warnings"), *Warnings);
			}

			FString SelfCorrectionHint;
			if (PipelineResult.ResultData->TryGetStringField(TEXT("self_correction_hint"), SelfCorrectionHint))
			{
				ToolResult.Data->SetStringField(TEXT("self_correction_hint"), SelfCorrectionHint);
			}

			const TSharedPtr<FJsonObject>* PinManifests = nullptr;
			if (PipelineResult.ResultData->TryGetObjectField(TEXT("pin_manifests"), PinManifests))
			{
				ToolResult.Data->SetObjectField(TEXT("pin_manifests"), *PinManifests);
			}

			// Forward plan ownership metadata (for stale error detection by self-correction)
			const TArray<TSharedPtr<FJsonValue>>* PlanClassNames = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("plan_class_names"), PlanClassNames))
			{
				ToolResult.Data->SetArrayField(TEXT("plan_class_names"), *PlanClassNames);
			}
			const TArray<TSharedPtr<FJsonValue>>* PlanFuncNames = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("plan_function_names"), PlanFuncNames))
			{
				ToolResult.Data->SetArrayField(TEXT("plan_function_names"), *PlanFuncNames);
			}
		}
	}
	else if (!PipelineResult.bSuccess && PipelineResult.ResultData.IsValid())
	{
		// On failure, still forward key fields if present (v2.0)
		if (bIsV2Plan && ToolResult.Data.IsValid())
		{
			// Forward step_to_node_map so rollback can find the nodes
			const TSharedPtr<FJsonObject>* FailStepMapObj = nullptr;
			if (PipelineResult.ResultData->TryGetObjectField(TEXT("step_to_node_map"), FailStepMapObj))
			{
				ToolResult.Data->SetObjectField(TEXT("step_to_node_map"), *FailStepMapObj);
			}

			// Forward reused_step_ids so rollback knows which nodes to skip
			const TArray<TSharedPtr<FJsonValue>>* FailReusedArr = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("reused_step_ids"), FailReusedArr))
			{
				ToolResult.Data->SetArrayField(TEXT("reused_step_ids"), *FailReusedArr);
			}

			const TArray<TSharedPtr<FJsonValue>>* WiringErrors = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("wiring_errors"), WiringErrors))
			{
				ToolResult.Data->SetArrayField(TEXT("wiring_errors"), *WiringErrors);
			}

			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("warnings"), Warnings))
			{
				ToolResult.Data->SetArrayField(TEXT("warnings"), *Warnings);
			}

			const TSharedPtr<FJsonObject>* PinManifests = nullptr;
			if (PipelineResult.ResultData->TryGetObjectField(TEXT("pin_manifests"), PinManifests))
			{
				ToolResult.Data->SetObjectField(TEXT("pin_manifests"), *PinManifests);
			}

			// Forward plan ownership metadata (for stale error detection by self-correction)
			const TArray<TSharedPtr<FJsonValue>>* FailPlanClassNames = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("plan_class_names"), FailPlanClassNames))
			{
				ToolResult.Data->SetArrayField(TEXT("plan_class_names"), *FailPlanClassNames);
			}
			const TArray<TSharedPtr<FJsonValue>>* FailPlanFuncNames = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("plan_function_names"), FailPlanFuncNames))
			{
				ToolResult.Data->SetArrayField(TEXT("plan_function_names"), *FailPlanFuncNames);
			}
		}
	}

	// ------------------------------------------------------------------
	// 12. Post-pipeline rollback on compile failure
	// ------------------------------------------------------------------
	// If the pipeline reports compile errors, the nodes created by this plan
	// are now zombie nodes (transaction already committed in Stage 4).
	// Remove them so the AI can retry with a clean graph.

	// Check if this was a partial success (all nodes created, some wiring failed).
	// Partial success should NOT be rolled back -- the nodes and successful wires
	// persist, and the AI can fix the remaining failures with connect_pins.
	// Rollback is only for TOTAL failure (Phase 1 node creation aborted).
	bool bIsPartialSuccess = false;
	if (PipelineResult.ResultData.IsValid())
	{
		FString Status;
		if (PipelineResult.ResultData->TryGetStringField(TEXT("status"), Status)
			&& Status == TEXT("partial_success"))
		{
			bIsPartialSuccess = true;
		}
	}

	if (bIsV2Plan && !PipelineResult.bSuccess && !bIsPartialSuccess && PipelineResult.ResultData.IsValid())
	{
		bool bCompileSuccess = true;
		const TSharedPtr<FJsonObject>* CompileResultObj = nullptr;
		if (PipelineResult.ResultData->TryGetObjectField(TEXT("compile_result"), CompileResultObj)
			&& CompileResultObj && (*CompileResultObj).IsValid())
		{
			(*CompileResultObj)->TryGetBoolField(TEXT("success"), bCompileSuccess);
		}

		const TSharedPtr<FJsonObject>* StepMapObj = nullptr;
		const bool bHasStepMap = PipelineResult.ResultData->TryGetObjectField(
			TEXT("step_to_node_map"), StepMapObj) && StepMapObj && (*StepMapObj).IsValid();

		if (!bCompileSuccess && bHasStepMap)
		{
			// Re-find the graph (may have been created inside the pipeline transaction)
			UEdGraph* RollbackGraph = FindGraphByName(Blueprint, GraphTarget);
			if (RollbackGraph)
			{
				// Build the set of reused step IDs from result data
				TSet<FString> ReusedIds;
				const TArray<TSharedPtr<FJsonValue>>* ReusedArr = nullptr;
				if (PipelineResult.ResultData->TryGetArrayField(TEXT("reused_step_ids"), ReusedArr)
					&& ReusedArr)
				{
					for (const TSharedPtr<FJsonValue>& Val : *ReusedArr)
					{
						if (Val.IsValid())
						{
							ReusedIds.Add(Val->AsString());
						}
					}
				}

				const int32 RemovedCount = RollbackPlanNodes(
					Blueprint, RollbackGraph, **StepMapObj, ReusedIds, AssetPath);

				if (RemovedCount > 0 && ToolResult.Data.IsValid())
				{
					ToolResult.Data->SetNumberField(TEXT("rolled_back_nodes"), RemovedCount);
					ToolResult.Data->SetStringField(TEXT("rollback_message"),
						FString::Printf(TEXT("Rolled back %d nodes from failed plan. "
							"The graph has been restored to its pre-plan state. "
							"Fix the plan and resubmit."), RemovedCount));
				}
			}
		}
	}

	UE_LOG(LogOliveBPTools, Log,
		TEXT("Plan apply for '%s' graph '%s': success=%s, schema_version=%s"),
		*AssetPath, *GraphTarget, PipelineResult.bSuccess ? TEXT("true") : TEXT("false"),
		*Plan.SchemaVersion);

	return ToolResult;
}

// ============================================================
// Template Tools
// ============================================================

void FOliveBlueprintToolHandlers::RegisterTemplateTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("blueprint.create_from_template"),
		TEXT("Create a complete Blueprint from a factory template. "
			"Templates provide parameterized, pre-wired Blueprints for common patterns "
			"(health, projectile, trigger, door, spawner). "
			"Use blueprint.list_templates or the catalog in context to discover available templates."),
		OliveBlueprintSchemas::BlueprintCreateFromTemplate(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCreateFromTemplate),
		{TEXT("blueprint"), TEXT("write"), TEXT("template")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.create_from_template"));

	Registry.RegisterTool(
		TEXT("blueprint.get_template"),
		TEXT("View a template's full content (parameter schema, presets, plan patterns). "
			"Use this to read patterns as reference before writing your own plan."),
		OliveBlueprintSchemas::BlueprintGetTemplate(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintGetTemplate),
		{TEXT("blueprint"), TEXT("read"), TEXT("template")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.get_template"));

	Registry.RegisterTool(
		TEXT("blueprint.list_templates"),
		TEXT("List available templates with descriptions and examples."),
		OliveBlueprintSchemas::BlueprintListTemplates(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintListTemplates),
		{TEXT("blueprint"), TEXT("read"), TEXT("template")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.list_templates"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered template tools (create_from_template, get_template, list_templates)"));
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintListTemplates(const TSharedPtr<FJsonObject>& Params)
{
	FString TypeFilter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("type"), TypeFilter);
	}

	const auto& AllTemplates = FOliveTemplateSystem::Get().GetAllTemplates();

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> TemplatesArray;

	for (const auto& Pair : AllTemplates)
	{
		const FOliveTemplateInfo& Info = Pair.Value;

		if (!TypeFilter.IsEmpty() && Info.TemplateType != TypeFilter)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("template_id"), Info.TemplateId);
		Entry->SetStringField(TEXT("type"), Info.TemplateType);
		Entry->SetStringField(TEXT("display_name"), Info.DisplayName);
		Entry->SetStringField(TEXT("description"), Info.CatalogDescription);
		if (!Info.CatalogExamples.IsEmpty())
		{
			Entry->SetStringField(TEXT("examples"), Info.CatalogExamples);
		}
		TemplatesArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	ResultData->SetArrayField(TEXT("templates"), TemplatesArray);
	ResultData->SetNumberField(TEXT("count"), TemplatesArray.Num());

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintGetTemplate(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide 'template_id'")
		);
	}

	FString TemplateId;
	if (!Params->TryGetStringField(TEXT("template_id"), TemplateId) || TemplateId.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'template_id' is missing"),
			TEXT("Provide template_id")
		);
	}

	FString PatternName;
	Params->TryGetStringField(TEXT("pattern"), PatternName);

	FString Content = FOliveTemplateSystem::Get().GetTemplateContent(TemplateId, PatternName);
	if (Content.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("TEMPLATE_NOT_FOUND"),
			FString::Printf(TEXT("Template '%s' not found"), *TemplateId),
			TEXT("Use blueprint.list_templates to see available templates")
		);
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("template_id"), TemplateId);
	ResultData->SetStringField(TEXT("content"), Content);

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintCreateFromTemplate(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide 'template_id' and 'asset_path'")
		);
	}

	FString TemplateId;
	if (!Params->TryGetStringField(TEXT("template_id"), TemplateId) || TemplateId.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'template_id' is missing"),
			TEXT("Provide template_id")
		);
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'asset_path' is missing"),
			TEXT("Provide asset_path (e.g., /Game/Blueprints/BP_Health)")
		);
	}

	FString PresetName;
	Params->TryGetStringField(TEXT("preset"), PresetName);

	// Extract parameters object
	TMap<FString, FString> UserParams;
	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("parameters"), ParamsObj) && ParamsObj && (*ParamsObj).IsValid())
	{
		for (const auto& KV : (*ParamsObj)->Values)
		{
			FString Value;
			if (KV.Value->TryGetString(Value))
			{
				UserParams.Add(KV.Key, Value);
			}
		}
	}

	return FOliveTemplateSystem::Get().ApplyTemplate(TemplateId, UserParams, PresetName, AssetPath);
}

