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
#include "K2Node_CallFunction.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Plan/OliveBlueprintPlanResolver.h"
#include "Plan/OliveBlueprintPlanLowerer.h"
#include "Plan/OlivePlanExecutor.h"
#include "Plan/OlivePlanValidator.h"
#include "Plan/OlivePinManifest.h"
#include "Services/OliveGraphBatchExecutor.h"
#include "Services/OliveBatchExecutionScope.h"
#include "IR/OliveIRSchema.h"
#include "IR/BlueprintPlanIR.h"
#include "Template/OliveTemplateSystem.h"
#include "K2Node_Timeline.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "Components/TimelineComponent.h"
#include "UObject/UnrealType.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

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
	FString& OutMode,
	int32& OutMaxNodes)
{
	OutPage = -1;
	OutPageSize = OLIVE_GRAPH_PAGE_SIZE;
	OutMode = TEXT("auto");
	OutMaxNodes = OLIVE_LARGE_GRAPH_THRESHOLD;

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

	if (Params->HasField(TEXT("max_nodes")))
	{
		OutMaxNodes = static_cast<int32>(Params->GetNumberField(TEXT("max_nodes")));
		OutMaxNodes = FMath::Clamp(OutMaxNodes, 10, 5000);
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
	int32 MaxNodes = OLIVE_LARGE_GRAPH_THRESHOLD;
	ExtractPagingParams(Params, Page, PageSize, Mode, MaxNodes);

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
		ResultData->SetBoolField(TEXT("is_large_graph"), PageIR.NodeCount >= MaxNodes);

		// Inject redirector info
		if (!ResolveInfo.RedirectedFrom.IsEmpty() && ResultData.IsValid())
		{
			ResultData->SetStringField(TEXT("redirected_from"), ResolveInfo.RedirectedFrom);
		}

		return FOliveToolResult::Success(ResultData);
	}

	// If graph is large and mode is not "full", return summary
	if (RawNodeCount >= MaxNodes && Mode != TEXT("full"))
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
	FOliveWriteRequest& Request,
	FOliveWriteExecutor Executor)
{
	// Propagate ChatMode from the tool execution context so the mode gate
	// works correctly for both MCP and built-in chat paths.
	// External MCP agents default to Code; in-engine autonomous agents inherit
	// the user's mode via MCP server -> FOliveToolCallContext propagation.
	if (const FOliveToolCallContext* Ctx = FOliveToolExecutionContext::Get())
	{
		Request.ChatMode = Ctx->ChatMode;
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

/**
 * Build a compact JSON snapshot of a Blueprint's current structural state.
 * Used to give the AI agent immediate feedback about the Blueprint after write operations,
 * so it can make informed decisions about next steps without a separate read call.
 *
 * @param Blueprint The Blueprint to snapshot
 * @return JSON object with parent_class, components, variables, event_dispatchers, functions,
 *         event_graph_nodes, and compile_status fields. Returns nullptr if Blueprint is null.
 */
static TSharedPtr<FJsonObject> BuildCompactStateJson(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> StateJson = MakeShareable(new FJsonObject());

	// Parent class
	StateJson->SetStringField(TEXT("parent_class"),
		Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("unknown"));

	// Components
	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	if (Blueprint->SimpleConstructionScript)
	{
		const TArray<USCS_Node*>& SCSNodes = Blueprint->SimpleConstructionScript->GetAllNodes();
		for (const USCS_Node* Node : SCSNodes)
		{
			if (Node && Node->ComponentClass)
			{
				FString Entry = FString::Printf(TEXT("%s (%s)"),
					*Node->GetVariableName().ToString(),
					*Node->ComponentClass->GetName());
				ComponentsArray.Add(MakeShareable(new FJsonValueString(Entry)));
			}
		}
	}
	StateJson->SetArrayField(TEXT("components"), ComponentsArray);

	// Variables
	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		FString Entry = FString::Printf(TEXT("%s (%s)"),
			*Var.VarName.ToString(),
			*Var.VarType.PinCategory.ToString());
		VariablesArray.Add(MakeShareable(new FJsonValueString(Entry)));
	}
	StateJson->SetArrayField(TEXT("variables"), VariablesArray);

	// Event dispatchers
	TArray<TSharedPtr<FJsonValue>> DispatchersArray;
	for (const UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (Graph)
		{
			DispatchersArray.Add(MakeShareable(new FJsonValueString(Graph->GetName())));
		}
	}
	StateJson->SetArrayField(TEXT("event_dispatchers"), DispatchersArray);

	// Functions with node counts
	TArray<TSharedPtr<FJsonValue>> FunctionsArray;
	for (const UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
	{
		if (!FuncGraph) continue;
		const int32 NodeCount = FuncGraph->Nodes.Num();
		FString Entry = (NodeCount <= 1)
			? FString::Printf(TEXT("%s (EMPTY)"), *FuncGraph->GetName())
			: FString::Printf(TEXT("%s (%d nodes)"), *FuncGraph->GetName(), NodeCount);
		FunctionsArray.Add(MakeShareable(new FJsonValueString(Entry)));
	}
	StateJson->SetArrayField(TEXT("functions"), FunctionsArray);

	// Event graph node count
	int32 TotalEventGraphNodes = 0;
	for (const UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			TotalEventGraphNodes += Graph->Nodes.Num();
		}
	}
	StateJson->SetNumberField(TEXT("event_graph_nodes"), TotalEventGraphNodes);

	// Compile status
	FString CompileStatus;
	if (Blueprint->Status == BS_UpToDate)
	{
		CompileStatus = TEXT("ok");
	}
	else if (Blueprint->Status == BS_Error)
	{
		CompileStatus = TEXT("error");
	}
	else
	{
		CompileStatus = TEXT("dirty");
	}
	StateJson->SetStringField(TEXT("compile_status"), CompileStatus);

	return StateJson;
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

	// blueprint.read (unified reader — routes via 'section' param)
	// Sections: all, summary, graph, variables, components, hierarchy,
	// overridable_functions, pins, function_detail. Legacy read_* and
	// describe_function / get_node_pins tool names are aliases that pre-fill
	// the section field.
	Registry.RegisterTool(
		TEXT("blueprint.read"),
		TEXT("Read any aspect of a Blueprint: overview, a specific graph, variables, components, hierarchy, "
			"overridable functions, a single node's pins, or a function's detailed signature"),
		OliveBlueprintSchemas::BlueprintRead(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintRead),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.read"));

	// blueprint.describe_node_type (kept as a first-class tool -- discovery for
	// unfamiliar K2Node types is unrelated to reading an existing Blueprint).
	{
		FOliveToolDefinition Def;
		Def.Name = TEXT("blueprint.describe_node_type");
		Def.Description = TEXT("Describe a Blueprint node type: its pins, properties, and behavior. Use to plan before creating nodes.");
		Def.InputSchema = OliveBlueprintSchemas::BlueprintDescribeNodeType();
		Def.Tags = {TEXT("blueprint"), TEXT("read"), TEXT("discovery")};
		Def.Category = TEXT("blueprint");
		Def.WhenToUse = TEXT("NOT needed for plan_json workflows — plan_json auto-resolves function names. Only use when you need to discover pins on an unfamiliar K2Node subclass before using add_node. Do NOT call this before apply_plan_json.");
		Registry.RegisterTool(Def, FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleDescribeNodeType));
	}
	RegisteredToolNames.Add(TEXT("blueprint.describe_node_type"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 2 reader tools (blueprint.read + describe_node_type; describe_function/get_node_pins/verify_completion consolidated)"));
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
		TEXT("Create a new empty Blueprint asset with the specified parent_class."),
		OliveBlueprintSchemas::BlueprintCreate(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCreate),
		{TEXT("blueprint"), TEXT("write"), TEXT("create")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.create"));

	// blueprint.compile (verify:true folds in the old blueprint.verify_completion behavior)
	Registry.RegisterTool(
		TEXT("blueprint.compile"),
		TEXT("Force compile a Blueprint and return compilation results. Pass verify:true to run the full "
			"verification suite (compile + expected structure checks + orphaned exec flow detection)."),
		OliveBlueprintSchemas::BlueprintCompile(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCompile),
		{TEXT("blueprint"), TEXT("compile")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.compile"));

	// blueprint.delete (consolidated: entity dispatch routes to specialized removers)
	Registry.RegisterTool(
		TEXT("blueprint.delete"),
		TEXT("Delete an entity from a Blueprint. Without 'entity' (or entity='blueprint') deletes the whole asset. "
			"Legacy remove_* tool names are aliases that pre-fill the entity field."),
		OliveBlueprintSchemas::BlueprintDelete(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintDelete),
		{TEXT("blueprint"), TEXT("write"), TEXT("delete")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.delete"));

	// blueprint.modify (consolidated: entity + action dispatch)
	Registry.RegisterTool(
		TEXT("blueprint.modify"),
		TEXT("Modify an entity inside a Blueprint. Dispatches on entity+action to the matching specialized "
			"handler. Legacy modify_* / set_* / reparent_* tool names are aliases that pre-fill the entity."),
		OliveBlueprintSchemas::BlueprintModify(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintModify),
		{TEXT("blueprint"), TEXT("write"), TEXT("modify")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.modify"));

	// blueprint.add (consolidated: entity dispatch)
	Registry.RegisterTool(
		TEXT("blueprint.add"),
		TEXT("Add an entity to a Blueprint. Dispatches on entity (node, variable, function, component, "
			"custom_event, event_dispatcher, interface, timeline) to the matching specialized handler. "
			"Legacy add_* tool names are aliases that pre-fill the entity."),
		OliveBlueprintSchemas::BlueprintAdd(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintAdd),
		{TEXT("blueprint"), TEXT("write"), TEXT("add")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.add"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 5 asset/consolidated writer tools"));
}

void FOliveBlueprintToolHandlers::RegisterVariableWriterTools()
{
	// Variable writers are now reached via blueprint.add (entity='variable') and
	// blueprint.delete (entity='variable'). Legacy tool names remain available
	// as aliases -- see GetToolAliases() in OliveToolRegistry.cpp.
	UE_LOG(LogOliveBPTools, Log, TEXT("Variable writer tools are consolidated into blueprint.add/delete (aliases in registry)"));
}

void FOliveBlueprintToolHandlers::RegisterComponentWriterTools()
{
	// Component writers are now reached via blueprint.add (entity='component'),
	// blueprint.delete (entity='component'), and blueprint.modify (entity='component').
	// Legacy tool names remain available as aliases.
	UE_LOG(LogOliveBPTools, Log, TEXT("Component writer tools are consolidated into blueprint.add/modify/delete (aliases in registry)"));
}

void FOliveBlueprintToolHandlers::RegisterFunctionWriterTools()
{
	// Function writers are now reached via blueprint.add (entity='function' or
	// 'custom_event' or 'event_dispatcher'), blueprint.modify (entity='function'),
	// and blueprint.delete (entity='function'). Legacy names remain as aliases.
	UE_LOG(LogOliveBPTools, Log, TEXT("Function writer tools are consolidated into blueprint.add/modify/delete (aliases in registry)"));
}

void FOliveBlueprintToolHandlers::RegisterGraphWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// connect_pins and disconnect_pins remain first-class because they are the
	// primary wiring verbs used by agents. All other graph writers are reached
	// through blueprint.add/modify/delete with entity dispatch.

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

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 2 graph wiring tools (other graph writers via blueprint.add/modify/delete)"));
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

	// Extract path (required by all sections)
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

	// Extract section (default to "all" for backward compatibility)
	FString Section = TEXT("all");
	Params->TryGetStringField(TEXT("section"), Section);

	// Route to section-specific handler
	if (Section == TEXT("all") || Section == TEXT("summary"))
	{
		return HandleReadSectionAll(Params);
	}
	else if (Section == TEXT("graph"))
	{
		return HandleReadSectionGraph(Params);
	}
	else if (Section == TEXT("variables"))
	{
		return HandleReadSectionVariables(Params);
	}
	else if (Section == TEXT("components"))
	{
		return HandleReadSectionComponents(Params);
	}
	else if (Section == TEXT("hierarchy"))
	{
		return HandleReadSectionHierarchy(Params);
	}
	else if (Section == TEXT("overridable_functions"))
	{
		return HandleReadSectionOverridableFunctions(Params);
	}
	else if (Section == TEXT("pins"))
	{
		// blueprint.get_node_pins folded in. Normalize 'graph_name' -> 'graph'
		// because HandleBlueprintGetNodePins reads 'graph'.
		TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
		for (const auto& Pair : Params->Values) { SubParams->Values.Add(Pair.Key, Pair.Value); }
		FString GraphName;
		if (!SubParams->TryGetStringField(TEXT("graph"), GraphName) || GraphName.IsEmpty())
		{
			if (SubParams->TryGetStringField(TEXT("graph_name"), GraphName) && !GraphName.IsEmpty())
			{
				SubParams->SetStringField(TEXT("graph"), GraphName);
			}
			else if (SubParams->TryGetStringField(TEXT("function_name"), GraphName) && !GraphName.IsEmpty())
			{
				SubParams->SetStringField(TEXT("graph"), GraphName);
			}
		}
		return HandleBlueprintGetNodePins(SubParams);
	}
	else if (Section == TEXT("function_detail"))
	{
		// blueprint.describe_function folded in. The legacy handler accepts
		// 'function_name', 'target_class', 'path' directly, so just pass through.
		return HandleDescribeFunction(Params);
	}
	else
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRead: Invalid section '%s' for path='%s'"), *Section, *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_VALUE"),
			FString::Printf(TEXT("Invalid section '%s'. Must be one of: all, summary, graph, variables, components, hierarchy, overridable_functions, pins, function_detail"), *Section),
			TEXT("Use section='all' for a full overview, section='graph' with graph_name to read a specific graph, 'pins' with graph_name+node_id for a single node's pins, or 'function_detail' with function_name for signature details")
		);
	}
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleReadSectionAll(const TSharedPtr<FJsonObject>& Params)
{
	// Params already validated by HandleBlueprintRead (path presence checked there)
	FString AssetPath;
	Params->TryGetStringField(TEXT("path"), AssetPath);

	// Extract section to differentiate "summary" from "all"
	FString Section = TEXT("all");
	Params->TryGetStringField(TEXT("section"), Section);

	// Extract mode (default to summary)
	FString Mode = TEXT("summary");
	Params->TryGetStringField(TEXT("mode"), Mode);

	// For section="summary", force summary mode regardless of mode param
	if (Section == TEXT("summary"))
	{
		Mode = TEXT("summary");
	}

	// Validate mode
	if (Mode != TEXT("summary") && Mode != TEXT("full") && Mode != TEXT("auto"))
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleReadSectionAll: Invalid mode '%s' for path='%s'"), *Mode, *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_VALUE"),
			FString::Printf(TEXT("Invalid mode '%s'. Must be 'summary', 'full', or 'auto'"), *Mode),
			TEXT("Use 'summary' for structure only, or 'full' for complete graph data")
		);
	}

	// Resolve asset path (follows redirectors, checks existence)
	FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
	if (!ResolveInfo.IsSuccess())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleReadSectionAll: Asset not found at path='%s'"), *AssetPath);
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

			if (TotalNodeCount <= AUTO_FULL_READ_NODE_THRESHOLD && Section != TEXT("summary"))
			{
				bAutoFull = true;
				UE_LOG(LogOliveBPTools, Log,
					TEXT("HandleReadSectionAll: auto-upgrade to full read (%d nodes <= %d threshold)"),
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
				 "No need to use section='graph' for individual graphs."));
	}

	// One-time hint: nudge the agent to research community patterns before building.
	// Fires on the first blueprint.read per editor session so it appears at the
	// decision point (agent just read the BP and is about to plan).
	static bool bCommunitySearchHintShown = false;
	if (!bCommunitySearchHintShown && ResultData.IsValid())
	{
		bCommunitySearchHintShown = true;
		ResultData->SetStringField(TEXT("tip"),
			TEXT("olive.search_community_blueprints('relevant query') can show how other developers built similar patterns."));
	}

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleReadSectionGraph(const TSharedPtr<FJsonObject>& Params)
{
	// Params already validated by HandleBlueprintRead (path presence checked there)
	FString AssetPath;
	Params->TryGetStringField(TEXT("path"), AssetPath);

	// Extract graph_name (required for section="graph")
	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
	{
		// Also check function_name for backward compat (alias map copies function_name -> graph_name,
		// but direct callers might still use function_name)
		if (!Params->TryGetStringField(TEXT("function_name"), GraphName) || GraphName.IsEmpty())
		{
			UE_LOG(LogOliveBPTools, Warning, TEXT("HandleReadSectionGraph: Missing required param 'graph_name' for path='%s'"), *AssetPath);
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				TEXT("Required parameter 'graph_name' is missing or empty when section='graph'"),
				TEXT("Provide the graph name (e.g., 'EventGraph' or a function name like 'MyFunction')")
			);
		}
	}

	// Resolve asset path (follows redirectors, checks existence)
	FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
	if (!ResolveInfo.IsSuccess())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleReadSectionGraph: Asset not found at path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to resolve asset at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}
	FString ResolvedPath = ResolveInfo.ResolvedPath;

	// Load the Blueprint to access raw graphs
	FOliveBlueprintReader& Reader = FOliveBlueprintReader::Get();
	UBlueprint* Blueprint = Reader.LoadBlueprint(ResolvedPath);
	if (!Blueprint)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleReadSectionGraph: Failed to load Blueprint at path='%s'"), *ResolvedPath);
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to load Blueprint at path '%s'"), *ResolvedPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Search all graph collections for the named graph
	UEdGraph* TargetGraph = nullptr;

	// 1. Search UbergraphPages (event graphs)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			TargetGraph = Graph;
			break;
		}
	}

	// 2. Search FunctionGraphs
	if (!TargetGraph)
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				TargetGraph = Graph;
				break;
			}
		}
	}

	// 3. Search interface implementation graphs
	if (!TargetGraph)
	{
		for (FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			for (UEdGraph* Graph : InterfaceDesc.Graphs)
			{
				if (Graph && Graph->GetName() == GraphName)
				{
					TargetGraph = Graph;
					break;
				}
			}
			if (TargetGraph) { break; }
		}
	}

	// 4. Search MacroGraphs
	if (!TargetGraph)
	{
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				TargetGraph = Graph;
				break;
			}
		}
	}

	// 5. Fallback: if "EventGraph" was requested and not found by name, use first Ubergraph
	if (!TargetGraph && GraphName == TEXT("EventGraph") && Blueprint->UbergraphPages.Num() > 0)
	{
		TargetGraph = Blueprint->UbergraphPages[0];
	}

	if (!TargetGraph)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleReadSectionGraph: Graph '%s' not found in Blueprint '%s'"), *GraphName, *AssetPath);

		// Build a helpful list of available graphs
		TArray<FString> AvailableGraphs;
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph) { AvailableGraphs.Add(Graph->GetName()); }
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph) { AvailableGraphs.Add(Graph->GetName()); }
		}
		for (FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			for (UEdGraph* Graph : InterfaceDesc.Graphs)
			{
				if (Graph) { AvailableGraphs.Add(Graph->GetName()); }
			}
		}
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph) { AvailableGraphs.Add(Graph->GetName()); }
		}

		return FOliveToolResult::Error(
			TEXT("GRAPH_NOT_FOUND"),
			FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'. Available graphs: %s"),
				*GraphName, *AssetPath, *FString::Join(AvailableGraphs, TEXT(", "))),
			TEXT("Verify the graph name. Use section='all' to see all graphs in the Blueprint")
		);
	}

	// Delegate to the paging-aware graph read helper
	TSharedPtr<FOliveGraphReader> GraphReader = Reader.GetGraphReader();
	return HandleGraphReadWithPaging(TargetGraph, Blueprint, GraphReader, Params, ResolveInfo);
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleReadSectionVariables(const TSharedPtr<FJsonObject>& Params)
{
	// Params already validated by HandleBlueprintRead (path presence checked there)
	FString AssetPath;
	Params->TryGetStringField(TEXT("path"), AssetPath);

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

FOliveToolResult FOliveBlueprintToolHandlers::HandleReadSectionComponents(const TSharedPtr<FJsonObject>& Params)
{
	// Params already validated by HandleBlueprintRead (path presence checked there)
	FString AssetPath;
	Params->TryGetStringField(TEXT("path"), AssetPath);

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

FOliveToolResult FOliveBlueprintToolHandlers::HandleReadSectionHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	// Params already validated by HandleBlueprintRead (path presence checked there)
	FString AssetPath;
	Params->TryGetStringField(TEXT("path"), AssetPath);

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

FOliveToolResult FOliveBlueprintToolHandlers::HandleReadSectionOverridableFunctions(const TSharedPtr<FJsonObject>& Params)
{
	// Params already validated by HandleBlueprintRead (path presence checked there)
	FString AssetPath;
	Params->TryGetStringField(TEXT("path"), AssetPath);

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

FOliveToolResult FOliveBlueprintToolHandlers::HandleDescribeNodeType(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'type' field")
		);
	}

	FString TypeName;
	if (!Params->TryGetStringField(TEXT("type"), TypeName) || TypeName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'type' is missing or empty"),
			TEXT("Provide a node type name like 'Branch', 'ForEachLoop', or 'K2Node_CallFunction'")
		);
	}

	// --- 3a. Input normalization ---
	// Strip parenthetical suffixes: "Event (Override)" -> "Event"
	FString NormalizedInput = TypeName;
	{
		int32 ParenIndex = INDEX_NONE;
		if (NormalizedInput.FindChar(TEXT('('), ParenIndex))
		{
			NormalizedInput = NormalizedInput.Left(ParenIndex).TrimEnd();
		}
	}
	// Strip "K2Node " prefix (with space) and "K2Node_" prefix
	if (NormalizedInput.StartsWith(TEXT("K2Node "), ESearchCase::IgnoreCase))
	{
		NormalizedInput = NormalizedInput.Mid(7);
	}
	else if (NormalizedInput.StartsWith(TEXT("K2Node_"), ESearchCase::IgnoreCase))
	{
		NormalizedInput = NormalizedInput.Mid(7);
	}

	// Short-name to K2Node class name mapping for common types
	static const TMap<FString, FString> ShortNameMap = {
		// Control flow
		{ TEXT("branch"),              TEXT("K2Node_IfThenElse") },
		{ TEXT("sequence"),            TEXT("K2Node_ExecutionSequence") },
		{ TEXT("delay"),               TEXT("K2Node_CallFunction") },
		{ TEXT("select"),              TEXT("K2Node_Select") },
		{ TEXT("switchonint"),         TEXT("K2Node_SwitchInteger") },
		{ TEXT("switchonstring"),      TEXT("K2Node_SwitchString") },
		{ TEXT("switchonenum"),        TEXT("K2Node_SwitchEnum") },
		// Loops (MacroInstance types -- these require property context; we can describe the K2Node classes instead)
		{ TEXT("forloop"),             TEXT("K2Node_ForEachElementInEnum") },
		{ TEXT("foreachloop"),         TEXT("K2Node_ForEachElementInEnum") },
		// Function/Event
		{ TEXT("callfunction"),        TEXT("K2Node_CallFunction") },
		{ TEXT("event"),               TEXT("K2Node_Event") },
		{ TEXT("customevent"),         TEXT("K2Node_CustomEvent") },
		{ TEXT("componentboundevent"), TEXT("K2Node_ComponentBoundEvent") },
		{ TEXT("functionentry"),       TEXT("K2Node_FunctionEntry") },
		{ TEXT("functionresult"),      TEXT("K2Node_FunctionResult") },
		{ TEXT("return"),              TEXT("K2Node_FunctionResult") },
		// Variables
		{ TEXT("getvariable"),         TEXT("K2Node_VariableGet") },
		{ TEXT("setvariable"),         TEXT("K2Node_VariableSet") },
		{ TEXT("getvar"),              TEXT("K2Node_VariableGet") },
		{ TEXT("setvar"),              TEXT("K2Node_VariableSet") },
		{ TEXT("variableget"),         TEXT("K2Node_VariableGet") },
		{ TEXT("variableset"),         TEXT("K2Node_VariableSet") },
		// Casting
		{ TEXT("cast"),                TEXT("K2Node_DynamicCast") },
		{ TEXT("castto"),              TEXT("K2Node_DynamicCast") },
		// Object creation
		{ TEXT("spawnactor"),          TEXT("K2Node_SpawnActorFromClass") },
		// Struct operations
		{ TEXT("makestruct"),          TEXT("K2Node_MakeStruct") },
		{ TEXT("breakstruct"),         TEXT("K2Node_BreakStruct") },
		{ TEXT("makearray"),           TEXT("K2Node_MakeArray") },
		{ TEXT("makemap"),             TEXT("K2Node_MakeMap") },
		{ TEXT("makeset"),             TEXT("K2Node_MakeSet") },
		// Input (key binding)
		{ TEXT("inputkeyevent"),       TEXT("K2Node_InputKeyEvent") },
		{ TEXT("inputkey"),            TEXT("K2Node_InputKey") },
		// Enhanced Input
		{ TEXT("enhancedinputaction"), TEXT("K2Node_EnhancedInputAction") },
		{ TEXT("inputaction"),         TEXT("K2Node_EnhancedInputAction") },
		{ TEXT("enhancedinputactionevent"), TEXT("K2Node_EnhancedInputAction") },
		// Timeline
		{ TEXT("timeline"),            TEXT("K2Node_Timeline") },
		// Self reference
		{ TEXT("self"),                TEXT("K2Node_Self") },
		{ TEXT("selfref"),             TEXT("K2Node_Self") },
		{ TEXT("selfreference"),       TEXT("K2Node_Self") },
		{ TEXT("SelfReference"),       TEXT("K2Node_Self") },
	};

	// Resolve the type name: check short name map (case-insensitive)
	// Try normalized input with spaces stripped first, then original TypeName
	FString ResolvedClassName = TypeName;
	{
		// Build space-stripped key from normalized input for map lookup
		FString MapKey = NormalizedInput.ToLower();
		MapKey.ReplaceInline(TEXT(" "), TEXT(""));

		if (const FString* Mapped = ShortNameMap.Find(MapKey))
		{
			ResolvedClassName = *Mapped;
		}
		else
		{
			// Fallback: try the original TypeName lowered (for cases where normalization
			// didn't apply, e.g., "getvar" -> already in map)
			FString LowerOriginal = TypeName.ToLower();
			LowerOriginal.ReplaceInline(TEXT(" "), TEXT(""));
			if (const FString* Mapped2 = ShortNameMap.Find(LowerOriginal))
			{
				ResolvedClassName = *Mapped2;
			}
			else if (!NormalizedInput.Equals(TypeName, ESearchCase::IgnoreCase))
			{
				// Use the normalized input as the resolved class name (preserves
				// prefix stripping for StaticFindFirstObject strategies below)
				ResolvedClassName = NormalizedInput;
			}
		}
	}

	// Try to find the UClass for the node
	UClass* NodeClass = nullptr;

	// Strategy 1: StaticFindFirstObject with exact resolved name (with U prefix)
	FString ClassNameWithU = ResolvedClassName;
	if (!ClassNameWithU.StartsWith(TEXT("U")))
	{
		ClassNameWithU = TEXT("U") + ClassNameWithU;
	}
	NodeClass = Cast<UClass>(StaticFindFirstObject(
		UClass::StaticClass(), *ClassNameWithU,
		EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging,
		TEXT("HandleDescribeNodeType")));

	// Strategy 2: Try without U prefix (bare name)
	if (!NodeClass)
	{
		NodeClass = Cast<UClass>(StaticFindFirstObject(
			UClass::StaticClass(), *ResolvedClassName,
			EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging,
			TEXT("HandleDescribeNodeType")));
	}

	// Strategy 3: Try with UK2Node_ prefix
	if (!NodeClass && !ResolvedClassName.StartsWith(TEXT("K2Node_")) && !ResolvedClassName.StartsWith(TEXT("UK2Node_")))
	{
		NodeClass = Cast<UClass>(StaticFindFirstObject(
			UClass::StaticClass(), *(TEXT("UK2Node_") + ResolvedClassName),
			EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging,
			TEXT("HandleDescribeNodeType")));
	}

	// Strategy 4: If user passed K2Node_X, try UK2Node_X
	if (!NodeClass && ResolvedClassName.StartsWith(TEXT("K2Node_")))
	{
		NodeClass = Cast<UClass>(StaticFindFirstObject(
			UClass::StaticClass(), *(TEXT("U") + ResolvedClassName),
			EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging,
			TEXT("HandleDescribeNodeType")));
	}

	// Validate it is a UK2Node subclass
	if (!NodeClass || !NodeClass->IsChildOf(UK2Node::StaticClass()))
	{
		// --- 3c. High-confidence catalog resolution ---
		// Before returning an error, try the node catalog fuzzy match.
		// If the top result has score >= 500 (prefix match or exact ID match),
		// attempt to resolve it as an actual class instead of just suggesting it.
		//
		// Generic camelCase fallback: catalog display names use spaces
		// ("Create Widget"), but the AI often sends squashed camelCase tokens
		// ("CreateWidget"). Build a space-split variant of the input and run
		// fuzzy match against both forms, then pick the better score. This is
		// generic — it applies to every camelCase node name the AI might use
		// (CreateWidget, SpawnActorFromClass, GetActorLocation, etc.) without
		// hardcoding any specific alias.
		auto SplitCamelCase = [](const FString& In) -> FString
		{
			FString Out;
			Out.Reserve(In.Len() * 2);
			for (int32 Idx = 0; Idx < In.Len(); ++Idx)
			{
				const TCHAR Ch = In[Idx];
				if (Idx > 0 && FChar::IsUpper(Ch)
					&& (FChar::IsLower(In[Idx - 1]) || FChar::IsDigit(In[Idx - 1])))
				{
					Out.AppendChar(TEXT(' '));
				}
				Out.AppendChar(Ch);
			}
			return Out;
		};

		if (FOliveNodeCatalog::Get().IsInitialized())
		{
			TArray<FOliveNodeSuggestion> Suggestions = FOliveNodeCatalog::Get().FuzzyMatch(TypeName, 5);

			// Also try the camelCase-split form if it differs from the original.
			// Keep whichever variant produced the highest top score.
			const FString SplitVariant = SplitCamelCase(NormalizedInput);
			if (!SplitVariant.Equals(NormalizedInput, ESearchCase::CaseSensitive)
				&& !SplitVariant.Equals(TypeName, ESearchCase::CaseSensitive))
			{
				TArray<FOliveNodeSuggestion> SplitSuggestions =
					FOliveNodeCatalog::Get().FuzzyMatch(SplitVariant, 5);
				const int32 OriginalTop = Suggestions.Num() > 0 ? Suggestions[0].Score : 0;
				const int32 SplitTop = SplitSuggestions.Num() > 0 ? SplitSuggestions[0].Score : 0;
				if (SplitTop > OriginalTop)
				{
					Suggestions = MoveTemp(SplitSuggestions);
					UE_LOG(LogOliveBPTools, Verbose,
						TEXT("HandleDescribeNodeType: camelCase split '%s' -> '%s' improved fuzzy score %d -> %d"),
						*NormalizedInput, *SplitVariant, OriginalTop, SplitTop);
				}
			}

			if (Suggestions.Num() > 0 && Suggestions[0].Score >= 500)
			{
				const FString& TopTypeId = Suggestions[0].TypeId;

				// Try to find the class using the TypeId from the catalog.
				// Catalog TypeIds for K2Node entries are class names like "K2Node_IfThenElse".
				// Try with UK2Node_ prefix first, then U prefix.
				UClass* CatalogClass = nullptr;

				// Strategy A: TypeId might be a bare K2Node class name (e.g., "K2Node_Timeline")
				FString CatalogClassNameU = TopTypeId;
				if (!CatalogClassNameU.StartsWith(TEXT("U")))
				{
					CatalogClassNameU = TEXT("U") + CatalogClassNameU;
				}
				CatalogClass = Cast<UClass>(StaticFindFirstObject(
					UClass::StaticClass(), *CatalogClassNameU,
					EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging,
					TEXT("HandleDescribeNodeType")));

				// Strategy B: TypeId without U prefix (bare name)
				if (!CatalogClass)
				{
					CatalogClass = Cast<UClass>(StaticFindFirstObject(
						UClass::StaticClass(), *TopTypeId,
						EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging,
						TEXT("HandleDescribeNodeType")));
				}

				// Strategy C: TypeId might be a short name like "Branch" -- try UK2Node_ prefix
				if (!CatalogClass && !TopTypeId.StartsWith(TEXT("K2Node_")) && !TopTypeId.StartsWith(TEXT("UK2Node_")))
				{
					CatalogClass = Cast<UClass>(StaticFindFirstObject(
						UClass::StaticClass(), *(TEXT("UK2Node_") + TopTypeId),
						EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging,
						TEXT("HandleDescribeNodeType")));
				}

				if (CatalogClass && CatalogClass->IsChildOf(UK2Node::StaticClass()))
				{
					NodeClass = CatalogClass;
					ResolvedClassName = TopTypeId;
					UE_LOG(LogOliveBPTools, Log,
						TEXT("HandleDescribeNodeType: Resolved '%s' via catalog fuzzy match (score=%d, type_id='%s')"),
						*TypeName, Suggestions[0].Score, *TopTypeId);
				}
			}
		}
	}

	// Final check: if still no valid class, return error with suggestions
	if (!NodeClass || !NodeClass->IsChildOf(UK2Node::StaticClass()))
	{
		FString SuggestionStr = TEXT("Use short names like 'Branch', 'Sequence', 'Delay', 'CustomEvent', or full class names like 'K2Node_IfThenElse'.");

		if (FOliveNodeCatalog::Get().IsInitialized())
		{
			TArray<FOliveNodeSuggestion> Suggestions = FOliveNodeCatalog::Get().FuzzyMatch(TypeName, 3);
			if (Suggestions.Num() > 0)
			{
				TArray<FString> SuggestionNames;
				for (const auto& S : Suggestions)
				{
					SuggestionNames.Add(S.DisplayName);
				}
				SuggestionStr = FString::Printf(TEXT("Did you mean: %s?"), *FString::Join(SuggestionNames, TEXT(", ")));
			}
		}

		return FOliveToolResult::Error(
			TEXT("NODE_TYPE_NOT_FOUND"),
			FString::Printf(TEXT("Node type '%s' not found (resolved to '%s')"), *TypeName, *ResolvedClassName),
			SuggestionStr
		);
	}

	// Create a scratch Blueprint so the temp graph has a valid outer for
	// FindBlueprintForNodeChecked (called internally by many K2Node subclasses
	// during AllocateDefaultPins). Without this, nodes like UK2Node_CallFunction
	// hit a fatal assertion on the transient package.
	UBlueprint* ScratchBP = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	ScratchBP->ParentClass = AActor::StaticClass();
	ScratchBP->GeneratedClass = AActor::StaticClass();
	ScratchBP->SkeletonGeneratedClass = AActor::StaticClass();

	UEdGraph* TempGraph = NewObject<UEdGraph>(ScratchBP, NAME_None, RF_Transient);
	TempGraph->Schema = UEdGraphSchema_K2::StaticClass();

	UK2Node* TempNode = NewObject<UK2Node>(TempGraph, NodeClass, NAME_None, RF_Transient);
	if (!TempNode)
	{
		return FOliveToolResult::Error(
			TEXT("NODE_CREATION_FAILED"),
			FString::Printf(TEXT("Failed to create temporary node of type '%s'"), *ResolvedClassName),
			TEXT("This node type may require special initialization or a specific graph context")
		);
	}

	TempGraph->AddNode(TempNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	TempNode->AllocateDefaultPins();

	// Build response using the existing BuildPinManifest helper for consistency
	TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
	ResultData->SetStringField(TEXT("type"), NodeClass->GetName());

	// If user's short name differs from the resolved class, note it
	if (!TypeName.Equals(NodeClass->GetName(), ESearchCase::IgnoreCase)
		&& !TypeName.Equals(ResolvedClassName, ESearchCase::IgnoreCase))
	{
		ResultData->SetStringField(TEXT("resolved_from"), TypeName);
	}

	ResultData->SetStringField(TEXT("display_name"),
		TempNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	ResultData->SetStringField(TEXT("description"),
		TempNode->GetTooltipText().ToString());

	// Serialize pins using the same format as get_node_pins
	TSharedPtr<FJsonObject> PinsData = BuildPinManifest(TempNode);
	ResultData->SetObjectField(TEXT("pins"), PinsData);

	// Behavior flags
	ResultData->SetBoolField(TEXT("is_pure"), TempNode->IsNodePure());

	// Check latent flag: for CallFunction nodes, inspect the underlying UFunction metadata
	bool bIsLatent = false;
	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(TempNode))
	{
		if (UFunction* Func = CallNode->GetTargetFunction())
		{
			bIsLatent = Func->HasMetaData(FBlueprintMetadata::MD_Latent);
		}
	}
	ResultData->SetBoolField(TEXT("is_latent"), bIsLatent);

	// Notes about pin accuracy
	ResultData->SetStringField(TEXT("notes"),
		TEXT("Pin layout shown is for default configuration. Actual pins may vary based on "
			 "node properties (e.g., function_name for CallFunction). Use blueprint.get_node_pins "
			 "after adding the node to see exact pins in context."));

	// Cleanup -- remove from graph to avoid dangling references
	TempGraph->RemoveNode(TempNode);

	UE_LOG(LogOliveBPTools, Verbose, TEXT("HandleDescribeNodeType: Described node type '%s' (class: %s)"),
		*TypeName, *NodeClass->GetName());

	return FOliveToolResult::Success(ResultData);
}

// ---------------------------------------------------------------------------
// HandleDescribeFunction — look up a UFunction and return its pin signature
// ---------------------------------------------------------------------------

namespace
{
	/**
	 * Convert EOliveFunctionMatchMethod to a human-readable string for the tool response.
	 * @param Method The match method enum value
	 * @return String representation (e.g., "alias_map", "parent_class")
	 */
	FString MatchMethodToString(EOliveFunctionMatchMethod Method)
	{
		switch (Method)
		{
		case EOliveFunctionMatchMethod::ExactName:              return TEXT("exact_name");
		case EOliveFunctionMatchMethod::AliasMap:               return TEXT("alias_map");
		case EOliveFunctionMatchMethod::GeneratedClass:         return TEXT("generated_class");
		case EOliveFunctionMatchMethod::FunctionGraph:          return TEXT("function_graph");
		case EOliveFunctionMatchMethod::ParentClassSearch:      return TEXT("parent_class");
		case EOliveFunctionMatchMethod::ComponentClassSearch:   return TEXT("component_class");
		case EOliveFunctionMatchMethod::InterfaceSearch:        return TEXT("interface");
		case EOliveFunctionMatchMethod::LibrarySearch:          return TEXT("library");
		case EOliveFunctionMatchMethod::UniversalLibrarySearch: return TEXT("universal_library");
		case EOliveFunctionMatchMethod::FuzzyK2Match:           return TEXT("fuzzy_k2");
		default:                                                return TEXT("unknown");
		}
	}
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleDescribeFunction(const TSharedPtr<FJsonObject>& Params)
{
	// 1. Validate params — function_name required
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'function_name' field")
		);
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'function_name' is missing or empty"),
			TEXT("Provide a function name like 'SetActorLocation', 'ApplyDamage', or 'GetVelocity'")
		);
	}

	// 2. Optional: target_class, path (for Blueprint context)
	FString TargetClass;
	Params->TryGetStringField(TEXT("target_class"), TargetClass);

	FString AssetPath;
	Params->TryGetStringField(TEXT("path"), AssetPath);

	// 3. Load Blueprint if path provided (for context-aware search)
	UBlueprint* Blueprint = nullptr;
	if (!AssetPath.IsEmpty())
	{
		FOliveAssetResolveInfo ResolveInfo = FOliveAssetResolver::Get().ResolveByPath(AssetPath);
		if (ResolveInfo.IsSuccess())
		{
			Blueprint = LoadObject<UBlueprint>(nullptr, *ResolveInfo.ResolvedPath);
		}
		// If load fails, proceed without Blueprint context — FindFunctionEx handles null gracefully
	}

	// 4. Call FindFunctionEx
	FOliveFunctionSearchResult SearchResult =
		FOliveNodeFactory::Get().FindFunctionEx(FunctionName, TargetClass, Blueprint);

	// 5a. SUCCESS PATH — format signature
	if (SearchResult.IsValid())
	{
		UFunction* Func = SearchResult.Function;
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());

		ResultData->SetStringField(TEXT("function_name"), Func->GetName());
		ResultData->SetStringField(TEXT("owning_class"), SearchResult.MatchedClassName);
		ResultData->SetStringField(TEXT("match_method"), MatchMethodToString(SearchResult.MatchMethod));

		// Note alias resolution if the resolved name differs from the input
		if (!FunctionName.Equals(Func->GetName(), ESearchCase::IgnoreCase))
		{
			ResultData->SetStringField(TEXT("resolved_from"), FunctionName);
		}

		// Iterate UFunction parameters via TFieldIterator<FProperty>
		TArray<TSharedPtr<FJsonValue>> ParamsArray;
		FProperty* ReturnProp = nullptr;

		// Also build a compact one-line signature string
		FString Sig = Func->GetName() + TEXT("(");
		bool bFirstSig = true;

		for (TFieldIterator<FProperty> It(Func); It; ++It)
		{
			FProperty* Param = *It;

			if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnProp = Param;
				continue;
			}
			if (!Param->HasAnyPropertyFlags(CPF_Parm))
			{
				continue;
			}

			// Skip hidden WorldContextObject pin (invisible in Blueprint graphs)
			if (Param->GetName() == TEXT("WorldContextObject"))
			{
				continue;
			}

			// Build structured parameter entry
			TSharedPtr<FJsonObject> ParamObj = MakeShareable(new FJsonObject());
			ParamObj->SetStringField(TEXT("name"), Param->GetName());
			ParamObj->SetStringField(TEXT("type"), Param->GetCPPType());

			const bool bIsOutParam = Param->HasAnyPropertyFlags(CPF_OutParm);
			const bool bIsByRef = Param->HasAllPropertyFlags(CPF_OutParm | CPF_ReferenceParm);

			if (bIsByRef)
			{
				ParamObj->SetStringField(TEXT("direction"), TEXT("in_out"));
				ParamObj->SetBoolField(TEXT("by_ref"), true);
			}
			else if (bIsOutParam)
			{
				ParamObj->SetStringField(TEXT("direction"), TEXT("out"));
			}
			else
			{
				ParamObj->SetStringField(TEXT("direction"), TEXT("in"));
			}

			ParamsArray.Add(MakeShareable(new FJsonValueObject(ParamObj)));

			// Append to one-line signature
			if (!bFirstSig)
			{
				Sig += TEXT(", ");
			}
			bFirstSig = false;
			Sig += Param->GetName() + TEXT(": ") + Param->GetCPPType();
			if (bIsByRef)
			{
				Sig += TEXT(" [by-ref]");
			}
		}

		Sig += TEXT(")");

		ResultData->SetArrayField(TEXT("parameters"), ParamsArray);

		if (ReturnProp)
		{
			ResultData->SetStringField(TEXT("return_type"), ReturnProp->GetCPPType());
			Sig += TEXT(" -> ") + ReturnProp->GetCPPType();
		}

		const bool bIsPure = Func->HasAnyFunctionFlags(FUNC_BlueprintPure);
		const bool bIsLatent = Func->HasMetaData(FBlueprintMetadata::MD_Latent);

		ResultData->SetBoolField(TEXT("is_pure"), bIsPure);
		ResultData->SetBoolField(TEXT("is_latent"), bIsLatent);

		if (bIsPure)
		{
			Sig += TEXT(" [pure]");
		}
		if (bIsLatent)
		{
			Sig += TEXT(" [latent]");
		}

		ResultData->SetStringField(TEXT("signature"), Sig);

		// Note for interface functions — requires UK2Node_Message
		if (SearchResult.MatchMethod == EOliveFunctionMatchMethod::InterfaceSearch)
		{
			ResultData->SetStringField(TEXT("note"),
				TEXT("This is an interface function. In plan_json, use target_class with the interface name to create a UK2Node_Message."));
		}

		UE_LOG(LogOliveBPTools, Verbose, TEXT("HandleDescribeFunction: Found '%s' -> '%s' on %s via %s"),
			*FunctionName, *Func->GetName(), *SearchResult.MatchedClassName,
			*MatchMethodToString(SearchResult.MatchMethod));

		return FOliveToolResult::Success(ResultData);
	}

	// 5b. FAILURE PATH — fuzzy suggestions + UPROPERTY detection + search trail
	TSharedPtr<FJsonObject> ErrorData = MakeShareable(new FJsonObject());
	ErrorData->SetStringField(TEXT("searched_function"), FunctionName);

	// Separate PROPERTY MATCH entries from regular search trail
	TArray<TSharedPtr<FJsonValue>> TrailArray;
	TArray<FString> PropertyMatches;

	for (const FString& Location : SearchResult.SearchedLocations)
	{
		if (Location.StartsWith(TEXT("PROPERTY MATCH:")))
		{
			PropertyMatches.Add(Location);
		}
		else
		{
			TrailArray.Add(MakeShareable(new FJsonValueString(Location)));
		}
	}
	ErrorData->SetArrayField(TEXT("search_trail"), TrailArray);

	// Fuzzy suggestions from the last FindFunction call
	const TArray<FString>& FuzzySuggestions = FOliveNodeFactory::Get().GetLastFuzzySuggestions();
	if (FuzzySuggestions.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SuggestArray;
		for (const FString& S : FuzzySuggestions)
		{
			SuggestArray.Add(MakeShareable(new FJsonValueString(S)));
		}
		ErrorData->SetArrayField(TEXT("suggestions"), SuggestArray);
	}

	// Surface UPROPERTY matches prominently
	if (PropertyMatches.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PropArray;
		for (const FString& PM : PropertyMatches)
		{
			PropArray.Add(MakeShareable(new FJsonValueString(PM)));
		}
		ErrorData->SetArrayField(TEXT("property_matches"), PropArray);
	}

	// Build actionable suggestion text
	FString Suggestion = TEXT("Check the search trail and suggestions. ");
	if (PropertyMatches.Num() > 0)
	{
		Suggestion += TEXT("The name matches a UPROPERTY — use get_var/set_var instead of call.");
	}
	else if (FuzzySuggestions.Num() > 0)
	{
		// Show up to 5 suggestions in the hint
		TArray<FString> TopSuggestions;
		const int32 MaxSuggestions = FMath::Min(FuzzySuggestions.Num(), 5);
		for (int32 i = 0; i < MaxSuggestions; ++i)
		{
			TopSuggestions.Add(FuzzySuggestions[i]);
		}
		Suggestion += TEXT("Did you mean: ") + FString::Join(TopSuggestions, TEXT(", ")) + TEXT("?");
	}

	UE_LOG(LogOliveBPTools, Verbose, TEXT("HandleDescribeFunction: Function '%s' not found. Trail: %s"),
		*FunctionName, *SearchResult.BuildSearchedLocationsString());

	FOliveToolResult Result = FOliveToolResult::Error(
		TEXT("FUNCTION_NOT_FOUND"),
		FString::Printf(TEXT("Function '%s' not found"), *FunctionName),
		Suggestion
	);
	// Attach structured error data for programmatic consumption
	Result.Data = ErrorData;
	return Result;
}

// ============================================================================
// Verify Completion Handler
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleVerifyCompletion(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide at least 'asset_path'."));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'asset_path' is missing or empty"),
			TEXT("Provide the Blueprint asset path to verify."));
	}

	// Load Blueprint
	FOliveBlueprintReader& BPReader = FOliveBlueprintReader::Get();
	UBlueprint* Blueprint = BPReader.LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Failed to load Blueprint at path '%s'"), *AssetPath),
			TEXT("Use project.search to find the correct asset path."));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> IssuesArray;
	bool bComplete = true;

	// ------------------------------------------------------------------
	// 1. Compile the Blueprint
	// ------------------------------------------------------------------
	FOliveIRCompileResult CompileResult = FOliveCompileManager::Get().Compile(Blueprint);

	int32 CompileErrorCount = CompileResult.Errors.Num();
	for (const FOliveIRCompileError& Err : CompileResult.Errors)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("type"), TEXT("compile_error"));
		Issue->SetStringField(TEXT("message"), Err.Message);
		if (!Err.GraphName.IsEmpty())
		{
			Issue->SetStringField(TEXT("graph"), Err.GraphName);
		}
		if (!Err.Suggestion.IsEmpty())
		{
			Issue->SetStringField(TEXT("suggestion"), Err.Suggestion);
		}
		IssuesArray.Add(MakeShared<FJsonValueObject>(Issue));
	}
	if (CompileErrorCount > 0)
	{
		bComplete = false;
	}
	ResultData->SetNumberField(TEXT("compile_errors"), CompileErrorCount);

	// ------------------------------------------------------------------
	// 2. Check orphaned exec flows across all graphs
	// ------------------------------------------------------------------
	int32 TotalOrphanedExec = 0;
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();

	auto CheckOrphanedExec = [&](UEdGraph* Graph)
	{
		if (!Graph) return;
		TArray<FOliveIRMessage> OrphanMessages;
		int32 Count = Pipeline.DetectOrphanedExecFlows(Graph, OrphanMessages);
		TotalOrphanedExec += Count;
		for (const FOliveIRMessage& Msg : OrphanMessages)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("orphaned_exec"));
			Issue->SetStringField(TEXT("message"), Msg.Message);
			Issue->SetStringField(TEXT("graph"), Graph->GetName());
			IssuesArray.Add(MakeShared<FJsonValueObject>(Issue));
		}
	};

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		CheckOrphanedExec(Graph);
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		CheckOrphanedExec(Graph);
	}
	if (TotalOrphanedExec > 0)
	{
		bComplete = false;
	}
	ResultData->SetNumberField(TEXT("orphaned_exec_flows"), TotalOrphanedExec);

	// ------------------------------------------------------------------
	// 3. Check unwired required data pins across all graphs
	// ------------------------------------------------------------------
	int32 TotalUnwiredPins = 0;

	auto CheckUnwiredPins = [&](UEdGraph* Graph)
	{
		if (!Graph) return;
		TSet<UEdGraphNode*> EmptySet; // Check all nodes
		TArray<FString> UnwiredMessages;
		int32 Count = FOliveWritePipeline::DetectUnwiredRequiredDataPins(Graph, EmptySet, UnwiredMessages);
		TotalUnwiredPins += Count;
		for (const FString& Msg : UnwiredMessages)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("unwired_pin"));
			Issue->SetStringField(TEXT("message"), Msg);
			Issue->SetStringField(TEXT("graph"), Graph->GetName());
			IssuesArray.Add(MakeShared<FJsonValueObject>(Issue));
		}
	};

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		CheckUnwiredPins(Graph);
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		CheckUnwiredPins(Graph);
	}
	if (TotalUnwiredPins > 0)
	{
		// Unwired pins are warnings, not errors -- don't set bComplete to false
		// They may be intentional in some patterns (e.g., optional pins)
	}
	ResultData->SetNumberField(TEXT("unwired_required_pins"), TotalUnwiredPins);

	// ------------------------------------------------------------------
	// 4. Check expected functions
	// ------------------------------------------------------------------
	const TArray<TSharedPtr<FJsonValue>>* ExpectedFunctions = nullptr;
	if (Params->TryGetArrayField(TEXT("expected_functions"), ExpectedFunctions) && ExpectedFunctions)
	{
		TArray<TSharedPtr<FJsonValue>> MissingFunctionsArr;
		for (const TSharedPtr<FJsonValue>& Val : *ExpectedFunctions)
		{
			if (!Val.IsValid()) continue;
			FString ExpectedName = Val->AsString();
			if (ExpectedName.IsEmpty()) continue;

			bool bFound = false;

			// Check FunctionGraphs
			for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetFName() == FName(*ExpectedName))
				{
					bFound = true;
					break;
				}
			}

			// Also check ubergraph pages for custom events
			if (!bFound)
			{
				for (const UEdGraph* Graph : Blueprint->UbergraphPages)
				{
					if (!Graph) continue;
					for (const UEdGraphNode* Node : Graph->Nodes)
					{
						if (const UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
						{
							if (CE->CustomFunctionName == FName(*ExpectedName))
							{
								bFound = true;
								break;
							}
						}
					}
					if (bFound) break;
				}
			}

			if (!bFound)
			{
				bComplete = false;
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("type"), TEXT("missing_function"));
				Issue->SetStringField(TEXT("message"),
					FString::Printf(TEXT("Expected function '%s' not found"), *ExpectedName));
				IssuesArray.Add(MakeShared<FJsonValueObject>(Issue));
				MissingFunctionsArr.Add(MakeShared<FJsonValueString>(ExpectedName));
			}
		}
		if (MissingFunctionsArr.Num() > 0)
		{
			ResultData->SetArrayField(TEXT("missing_functions"), MissingFunctionsArr);
		}
	}

	// ------------------------------------------------------------------
	// 5. Check expected variables
	// ------------------------------------------------------------------
	const TArray<TSharedPtr<FJsonValue>>* ExpectedVariables = nullptr;
	if (Params->TryGetArrayField(TEXT("expected_variables"), ExpectedVariables) && ExpectedVariables)
	{
		TArray<TSharedPtr<FJsonValue>> MissingVariablesArr;
		for (const TSharedPtr<FJsonValue>& Val : *ExpectedVariables)
		{
			if (!Val.IsValid()) continue;
			FString ExpectedName = Val->AsString();
			if (ExpectedName.IsEmpty()) continue;

			bool bFound = false;
			for (const FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				if (Var.VarName == FName(*ExpectedName))
				{
					bFound = true;
					break;
				}
			}

			if (!bFound)
			{
				bComplete = false;
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("type"), TEXT("missing_variable"));
				Issue->SetStringField(TEXT("message"),
					FString::Printf(TEXT("Expected variable '%s' not found"), *ExpectedName));
				IssuesArray.Add(MakeShared<FJsonValueObject>(Issue));
				MissingVariablesArr.Add(MakeShared<FJsonValueString>(ExpectedName));
			}
		}
		if (MissingVariablesArr.Num() > 0)
		{
			ResultData->SetArrayField(TEXT("missing_variables"), MissingVariablesArr);
		}
	}

	// ------------------------------------------------------------------
	// 6. Build result
	// ------------------------------------------------------------------
	ResultData->SetBoolField(TEXT("complete"), bComplete);
	ResultData->SetArrayField(TEXT("issues"), IssuesArray);
	ResultData->SetNumberField(TEXT("issue_count"), IssuesArray.Num());

	FString Summary;
	if (bComplete && IssuesArray.Num() == 0)
	{
		Summary = TEXT("Blueprint verification passed with no issues.");
	}
	else if (bComplete)
	{
		Summary = FString::Printf(TEXT("Blueprint verification passed with %d warning(s)."), IssuesArray.Num());
	}
	else
	{
		Summary = FString::Printf(TEXT("Blueprint verification FAILED with %d issue(s)."), IssuesArray.Num());
	}
	ResultData->SetStringField(TEXT("summary"), Summary);

	FOliveToolResult Result = FOliveToolResult::Success(ResultData);
	if (!bComplete)
	{
		Result.NextStepGuidance = TEXT("Fix the reported issues, then re-run blueprint.verify_completion to confirm.");
	}
	return Result;
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

	// Extract type first (needed for parent_class defaults)
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

	// Extract parent_class — auto-default for types that imply a specific parent
	FString ParentClass;
	Params->TryGetStringField(TEXT("parent_class"), ParentClass);

	if (ParentClass.IsEmpty())
	{
		if (BlueprintType == EOliveBlueprintType::WidgetBlueprint)
		{
			ParentClass = TEXT("UserWidget");
		}
		else if (BlueprintType == EOliveBlueprintType::AnimationBlueprint)
		{
			ParentClass = TEXT("AnimInstance");
		}
		else
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				TEXT("Required parameter 'parent_class' is missing or empty."),
				TEXT("Provide the parent class name (e.g., 'Actor', 'Character', 'Pawn', '/Game/Blueprints/BP_Base'). "
					"WidgetBlueprint and AnimationBlueprint types auto-default to UserWidget and AnimInstance respectively.")
			);
		}
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
			FString ErrorMsg = WriteResult.GetFirstError();
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

	FOliveToolResult ToolResult = Result.ToToolResult();

	// Enrich success response with inherited members so the AI knows what the
	// parent class provides and doesn't try to add duplicate variables/dispatchers.
	if (ToolResult.bSuccess && ToolResult.Data.IsValid())
	{
		UBlueprint* CreatedBP = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (CreatedBP && CreatedBP->ParentClass)
		{
			TArray<TSharedPtr<FJsonValue>> InheritedVars;
			TArray<TSharedPtr<FJsonValue>> InheritedDispatchers;

			for (TFieldIterator<FProperty> It(CreatedBP->ParentClass); It; ++It)
			{
				// Skip private/internal properties
				if (!It->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly))
				{
					continue;
				}

				const FString PropName = It->GetName();

				// Skip known internal properties
				if (PropName.StartsWith(TEXT("UberGraphFrame")) ||
					PropName == TEXT("bCanEverTick"))
				{
					continue;
				}

				if (CastField<FMulticastDelegateProperty>(*It))
				{
					InheritedDispatchers.Add(MakeShared<FJsonValueString>(PropName));
				}
				else
				{
					// Build "Name (Type)" string for concise listing
					FString TypeName = It->GetCPPType();
					InheritedVars.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("%s (%s)"), *PropName, *TypeName)));
				}
			}

			if (InheritedVars.Num() > 0)
			{
				// Cap at 30 to avoid bloating the response
				if (InheritedVars.Num() > 30)
				{
					int32 Total = InheritedVars.Num();
					InheritedVars.SetNum(30);
					InheritedVars.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("... and %d more"), Total - 30)));
				}
				ToolResult.Data->SetArrayField(TEXT("inherited_variables"), InheritedVars);
			}

			if (InheritedDispatchers.Num() > 0)
			{
				ToolResult.Data->SetArrayField(TEXT("inherited_dispatchers"), InheritedDispatchers);
			}

			// Also list inherited SCS components from parent Blueprints
			TArray<TSharedPtr<FJsonValue>> InheritedComponents;
			UBlueprint* ParentBP = Cast<UBlueprint>(CreatedBP->ParentClass->ClassGeneratedBy);
			if (ParentBP && ParentBP->SimpleConstructionScript)
			{
				TArray<USCS_Node*> AllNodes = ParentBP->SimpleConstructionScript->GetAllNodes();
				for (USCS_Node* Node : AllNodes)
				{
					if (Node && Node->ComponentTemplate)
					{
						InheritedComponents.Add(MakeShared<FJsonValueString>(
							FString::Printf(TEXT("%s (%s)"),
								*Node->GetVariableName().ToString(),
								*Node->ComponentClass->GetName())));
					}
				}
			}

			// Also enumerate native components from C++ parent class CDO
			// (covers ACharacter, APawn, etc. whose components aren't in SCS)
			if (!ParentBP && CreatedBP->ParentClass->IsChildOf(AActor::StaticClass()))
			{
				AActor* CDO = Cast<AActor>(CreatedBP->ParentClass->GetDefaultObject());
				if (CDO)
				{
					TInlineComponentArray<UActorComponent*> NativeComps;
					CDO->GetComponents(NativeComps);
					for (UActorComponent* Comp : NativeComps)
					{
						if (Comp)
						{
							InheritedComponents.Add(MakeShared<FJsonValueString>(
								FString::Printf(TEXT("%s (%s)"),
									*Comp->GetName(), *Comp->GetClass()->GetName())));
						}
					}
				}
			}

			if (InheritedComponents.Num() > 0)
			{
				ToolResult.Data->SetArrayField(TEXT("inherited_components"), InheritedComponents);
			}
		}
	}

	// One-time hint: nudge the agent to research community patterns after creating
	static bool bCommunitySearchHintShownCreate = false;
	if (!bCommunitySearchHintShownCreate && ToolResult.bSuccess && ToolResult.Data.IsValid())
	{
		bCommunitySearchHintShownCreate = true;
		ToolResult.Data->SetStringField(TEXT("tip"),
			TEXT("olive.search_community_blueprints('relevant query') can show how other developers built similar patterns."));
	}

	return ToolResult;
}

// ============================================================================
// blueprint.scaffold — Composite tool: create + interfaces + components + variables
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintScaffold(const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintScaffold: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' and 'parent_class' fields")
		);
	}

	// Extract path (required)
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintScaffold: Missing required param 'path'"));
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
					FString::Printf(TEXT("You used '%s' which is a folder. Add the BP name: '%s/BP_MyBlueprint'. Then retry blueprint.scaffold."),
						*AssetPath, *AssetPath));
			}
			return Result;
		}

		// Catch paths missing /Game/ prefix
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

	// Extract type (optional, default "Normal")
	FString TypeString = TEXT("Normal");
	Params->TryGetStringField(TEXT("type"), TypeString);

	// Parse Blueprint type (case-insensitive, lenient) — same logic as HandleBlueprintCreate
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
		UE_LOG(LogOliveBPTools, Warning, TEXT("blueprint.scaffold: Unknown type '%s', defaulting to Normal."), *TypeString);
		BlueprintType = EOliveBlueprintType::Normal;
	}

	// Extract parent_class (required, with auto-defaults for typed BPs)
	FString ParentClass;
	Params->TryGetStringField(TEXT("parent_class"), ParentClass);

	if (ParentClass.IsEmpty())
	{
		if (BlueprintType == EOliveBlueprintType::WidgetBlueprint)
		{
			ParentClass = TEXT("UserWidget");
		}
		else if (BlueprintType == EOliveBlueprintType::AnimationBlueprint)
		{
			ParentClass = TEXT("AnimInstance");
		}
		else
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				TEXT("Required parameter 'parent_class' is missing or empty."),
				TEXT("Provide the parent class name (e.g., 'Actor', 'Character', 'Pawn').")
			);
		}
	}

	// Parse optional arrays from params (pre-parse so lambda captures clean data)

	// --- Interfaces ---
	TArray<FString> InterfaceNames;
	const TArray<TSharedPtr<FJsonValue>>* InterfacesArray = nullptr;
	if (Params->TryGetArrayField(TEXT("interfaces"), InterfacesArray) && InterfacesArray)
	{
		for (const TSharedPtr<FJsonValue>& Val : *InterfacesArray)
		{
			FString InterfaceName;
			if (Val.IsValid() && Val->TryGetString(InterfaceName) && !InterfaceName.IsEmpty())
			{
				InterfaceNames.Add(InterfaceName);
			}
		}
	}

	// --- Components ---
	struct FComponentSpec
	{
		FString ClassName;
		FString Name;
		FString Parent;
	};
	TArray<FComponentSpec> ComponentSpecs;
	const TArray<TSharedPtr<FJsonValue>>* ComponentsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("components"), ComponentsArray) && ComponentsArray)
	{
		for (const TSharedPtr<FJsonValue>& Val : *ComponentsArray)
		{
			const TSharedPtr<FJsonObject>* CompObj = nullptr;
			if (!Val.IsValid() || !Val->TryGetObject(CompObj) || !CompObj || !(*CompObj).IsValid())
			{
				continue;
			}

			FComponentSpec Spec;
			if (!(*CompObj)->TryGetStringField(TEXT("class"), Spec.ClassName) || Spec.ClassName.IsEmpty())
			{
				continue; // class is required per component
			}
			(*CompObj)->TryGetStringField(TEXT("name"), Spec.Name);
			(*CompObj)->TryGetStringField(TEXT("parent"), Spec.Parent);
			ComponentSpecs.Add(MoveTemp(Spec));
		}
	}

	// --- Variables ---
	TArray<FOliveIRVariable> VariableSpecs;
	const TArray<TSharedPtr<FJsonValue>>* VariablesArray = nullptr;
	if (Params->TryGetArrayField(TEXT("variables"), VariablesArray) && VariablesArray)
	{
		for (const TSharedPtr<FJsonValue>& Val : *VariablesArray)
		{
			const TSharedPtr<FJsonObject>* VarObj = nullptr;
			if (!Val.IsValid() || !Val->TryGetObject(VarObj) || !VarObj || !(*VarObj).IsValid())
			{
				continue;
			}

			FOliveIRVariable Variable = ParseVariableFromParams(*VarObj);
			if (!Variable.Name.IsEmpty())
			{
				VariableSpecs.Add(MoveTemp(Variable));
			}
		}
	}

	UE_LOG(LogOliveBPTools, Log,
		TEXT("blueprint.scaffold: path='%s' parent='%s' type='%s' interfaces=%d components=%d variables=%d"),
		*AssetPath, *ParentClass, *TypeString,
		InterfaceNames.Num(), ComponentSpecs.Num(), VariableSpecs.Num());

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.scaffold");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = nullptr; // Create operation has no pre-existing target
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Scaffold Blueprint '%s' (%d interfaces, %d components, %d variables)"),
			*AssetPath, InterfaceNames.Num(), ComponentSpecs.Num(), VariableSpecs.Num())
	);
	Request.OperationCategory = TEXT("create");
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true; // Compile once at end after all sub-operations
	Request.bSkipVerification = false;

	// Create executor that sequentially: creates BP, adds interfaces, adds components, adds variables
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, ParentClass, BlueprintType, InterfaceNames, ComponentSpecs, VariableSpecs]
		(const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		TArray<FString> Warnings;
		int32 InterfacesAdded = 0;
		int32 ComponentsAdded = 0;
		int32 VariablesAdded = 0;

		// ---- Step 1: Create the Blueprint ----
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult CreateResult = Writer.CreateBlueprint(AssetPath, ParentClass, BlueprintType);

		if (!CreateResult.bSuccess)
		{
			FString ErrorMsg = CreateResult.GetFirstError();
			UE_LOG(LogOliveBPTools, Error, TEXT("blueprint.scaffold: Create failed: path='%s' error='%s'"),
				*AssetPath, *ErrorMsg);
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_SCAFFOLD_CREATE_FAILED"),
				ErrorMsg,
				TEXT("Verify the parent class exists and the asset path is valid")
			);
		}

		// Load the newly created Blueprint for subsequent operations
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			UE_LOG(LogOliveBPTools, Error, TEXT("blueprint.scaffold: Created Blueprint but failed to load it at '%s'"), *AssetPath);
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_SCAFFOLD_LOAD_FAILED"),
				FString::Printf(TEXT("Blueprint was created at '%s' but could not be loaded for subsequent operations"), *AssetPath),
				TEXT("This is an internal error. The Blueprint was created but interfaces/components/variables were not added.")
			);
		}

		// ---- Step 2: Add interfaces (failures are warnings) ----
		for (const FString& InterfaceName : InterfaceNames)
		{
			FOliveBlueprintWriteResult InterfaceResult = Writer.AddInterface(AssetPath, InterfaceName);
			if (InterfaceResult.bSuccess)
			{
				InterfacesAdded++;
			}
			else
			{
				FString ErrorMsg = InterfaceResult.GetFirstError();
				UE_LOG(LogOliveBPTools, Warning, TEXT("blueprint.scaffold: Failed to add interface '%s': %s"),
					*InterfaceName, *ErrorMsg);
				Warnings.Add(FString::Printf(TEXT("Interface '%s': %s"), *InterfaceName, *ErrorMsg));
			}
		}

		// ---- Step 3: Add components (failures are warnings) ----
		FOliveComponentWriter& CompWriter = FOliveComponentWriter::Get();
		for (const auto& Spec : ComponentSpecs)
		{
			FOliveBlueprintWriteResult CompResult = CompWriter.AddComponent(AssetPath, Spec.ClassName, Spec.Name, Spec.Parent);
			if (CompResult.bSuccess)
			{
				ComponentsAdded++;
			}
			else
			{
				FString ErrorMsg = CompResult.GetFirstError();
				UE_LOG(LogOliveBPTools, Warning, TEXT("blueprint.scaffold: Failed to add component '%s' ('%s'): %s"),
					*Spec.ClassName, *Spec.Name, *ErrorMsg);
				Warnings.Add(FString::Printf(TEXT("Component '%s' ('%s'): %s"), *Spec.ClassName, *Spec.Name, *ErrorMsg));
			}
		}

		// ---- Step 4: Add variables (failures are warnings) ----
		for (const FOliveIRVariable& Variable : VariableSpecs)
		{
			// Skip variables with unknown type (likely malformed input)
			if (Variable.Type.Category == EOliveIRTypeCategory::Unknown)
			{
				UE_LOG(LogOliveBPTools, Warning, TEXT("blueprint.scaffold: Skipping variable '%s' — type is Unknown"),
					*Variable.Name);
				Warnings.Add(FString::Printf(TEXT("Variable '%s': type is Unknown (missing or invalid type spec)"), *Variable.Name));
				continue;
			}

			FOliveBlueprintWriteResult VarResult = Writer.AddVariable(AssetPath, Variable);
			if (VarResult.bSuccess)
			{
				VariablesAdded++;
			}
			else
			{
				FString ErrorMsg = VarResult.GetFirstError();
				UE_LOG(LogOliveBPTools, Warning, TEXT("blueprint.scaffold: Failed to add variable '%s': %s"),
					*Variable.Name, *ErrorMsg);
				Warnings.Add(FString::Printf(TEXT("Variable '%s': %s"), *Variable.Name, *ErrorMsg));
			}
		}

		// Build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), AssetPath);
		ResultData->SetStringField(TEXT("parent_class"), ParentClass);
		ResultData->SetStringField(TEXT("type"), FOliveBlueprintTypeDetector::TypeToString(BlueprintType));
		ResultData->SetNumberField(TEXT("interfaces_added"), InterfacesAdded);
		ResultData->SetNumberField(TEXT("components_added"), ComponentsAdded);
		ResultData->SetNumberField(TEXT("variables_added"), VariablesAdded);

		// Attach warnings array
		if (Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningsJsonArray;
			for (const FString& Warning : Warnings)
			{
				WarningsJsonArray.Add(MakeShared<FJsonValueString>(Warning));
			}
			ResultData->SetArrayField(TEXT("warnings"), WarningsJsonArray);
		}

		// Reload the Blueprint to get the final state (after all modifications)
		Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (Blueprint)
		{
			if (TSharedPtr<FJsonObject> StateJson = BuildCompactStateJson(Blueprint))
			{
				ResultData->SetObjectField(TEXT("blueprint_state"), StateJson);
			}
		}

		FOliveWriteResult Result = FOliveWriteResult::Success(ResultData);
		Result.CreatedItem = AssetPath;
		return Result;
	});

	// Execute through pipeline (single transaction wrapping all sub-operations)
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

	FOliveToolResult ToolResult = Result.ToToolResult();

	// Enrich success response with inherited members (same pattern as HandleBlueprintCreate)
	if (ToolResult.bSuccess && ToolResult.Data.IsValid())
	{
		UBlueprint* CreatedBP = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (CreatedBP && CreatedBP->ParentClass)
		{
			TArray<TSharedPtr<FJsonValue>> InheritedVars;
			TArray<TSharedPtr<FJsonValue>> InheritedDispatchers;

			for (TFieldIterator<FProperty> It(CreatedBP->ParentClass); It; ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly))
				{
					continue;
				}

				const FString PropName = It->GetName();
				if (PropName.StartsWith(TEXT("UberGraphFrame")) || PropName == TEXT("bCanEverTick"))
				{
					continue;
				}

				if (CastField<FMulticastDelegateProperty>(*It))
				{
					InheritedDispatchers.Add(MakeShared<FJsonValueString>(PropName));
				}
				else
				{
					FString TypeName = It->GetCPPType();
					InheritedVars.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("%s (%s)"), *PropName, *TypeName)));
				}
			}

			if (InheritedVars.Num() > 0)
			{
				if (InheritedVars.Num() > 30)
				{
					int32 Total = InheritedVars.Num();
					InheritedVars.SetNum(30);
					InheritedVars.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("... and %d more"), Total - 30)));
				}
				ToolResult.Data->SetArrayField(TEXT("inherited_variables"), InheritedVars);
			}

			if (InheritedDispatchers.Num() > 0)
			{
				ToolResult.Data->SetArrayField(TEXT("inherited_dispatchers"), InheritedDispatchers);
			}

			// List inherited SCS components from parent Blueprints
			TArray<TSharedPtr<FJsonValue>> InheritedComponents;
			UBlueprint* ParentBP = Cast<UBlueprint>(CreatedBP->ParentClass->ClassGeneratedBy);
			if (ParentBP && ParentBP->SimpleConstructionScript)
			{
				TArray<USCS_Node*> AllNodes = ParentBP->SimpleConstructionScript->GetAllNodes();
				for (USCS_Node* Node : AllNodes)
				{
					if (Node && Node->ComponentTemplate)
					{
						InheritedComponents.Add(MakeShared<FJsonValueString>(
							FString::Printf(TEXT("%s (%s)"),
								*Node->GetVariableName().ToString(),
								*Node->ComponentClass->GetName())));
					}
				}
			}

			// Also enumerate native components from C++ parent class CDO
			if (!ParentBP && CreatedBP->ParentClass->IsChildOf(AActor::StaticClass()))
			{
				AActor* CDO = Cast<AActor>(CreatedBP->ParentClass->GetDefaultObject());
				if (CDO)
				{
					TInlineComponentArray<UActorComponent*> NativeComps;
					CDO->GetComponents(NativeComps);
					for (UActorComponent* Comp : NativeComps)
					{
						if (Comp)
						{
							InheritedComponents.Add(MakeShared<FJsonValueString>(
								FString::Printf(TEXT("%s (%s)"),
									*Comp->GetName(), *Comp->GetClass()->GetName())));
						}
					}
				}
			}

			if (InheritedComponents.Num() > 0)
			{
				ToolResult.Data->SetArrayField(TEXT("inherited_components"), InheritedComponents);
			}
		}
	}

	return ToolResult;
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
			FString ErrorMsg = WriteResult.GetFirstError();
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

	// Extract path -- accept 'path' or 'asset_path' (the legacy verify tool used asset_path).
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintCompile: Missing required param 'path'"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the Blueprint asset path")
		);
	}

	// verify:true routes to the completion-verification path (folded in from the
	// former blueprint.verify_completion tool). HandleVerifyCompletion expects
	// 'asset_path', so normalize before delegating.
	bool bVerify = false;
	if (Params->TryGetBoolField(TEXT("verify"), bVerify) && bVerify)
	{
		TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
		for (const auto& Pair : Params->Values) { SubParams->Values.Add(Pair.Key, Pair.Value); }
		SubParams->SetStringField(TEXT("asset_path"), AssetPath);
		return HandleVerifyCompletion(SubParams);
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

	// Return failure if compilation had errors, so the Builder knows to fix them
	if (!CompileResult.bSuccess)
	{
		// Build a human-readable error summary for the Builder
		FString ErrorSummary = FString::Printf(
			TEXT("Compilation FAILED with %d error(s)."),
			CompileResult.Errors.Num());

		// Include first 5 error messages for immediate visibility
		const int32 MaxErrors = FMath::Min(CompileResult.Errors.Num(), 5);
		for (int32 i = 0; i < MaxErrors; ++i)
		{
			ErrorSummary += TEXT("\n  - ") + CompileResult.Errors[i].Message;
		}
		if (CompileResult.Errors.Num() > MaxErrors)
		{
			ErrorSummary += FString::Printf(
				TEXT("\n  ... and %d more error(s). See 'errors' array in result data."),
				CompileResult.Errors.Num() - MaxErrors);
		}

		FOliveToolResult FailResult = FOliveToolResult::Error(
			TEXT("COMPILE_FAILED"),
			ErrorSummary,
			TEXT("Fix the reported errors and compile again"));
		FailResult.Data = ResultData;  // Full error details still available
		return FailResult;
	}

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

	// --- Consolidated entity dispatch (P5) ---
	// Legacy callers (no 'entity' field) keep the previous behavior of deleting
	// the whole Blueprint asset. When 'entity' is provided we route to a
	// specialized handler, validating entity-specific required fields first.
	FString Entity;
	Params->TryGetStringField(TEXT("entity"), Entity);
	Entity = Entity.ToLower();
	if (!Entity.IsEmpty() && Entity != TEXT("blueprint"))
	{
		// Clone params so we can normalize field aliases without mutating caller state.
		TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
		for (const auto& Pair : Params->Values) { SubParams->Values.Add(Pair.Key, Pair.Value); }

		auto MissingParam = [](const FString& FieldName, const FString& WhenEntity) -> FOliveToolResult
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				FString::Printf(TEXT("Missing required parameter '%s' for entity='%s'"), *FieldName, *WhenEntity),
				FString::Printf(TEXT("Provide '%s' when entity='%s'."), *FieldName, *WhenEntity));
		};

		if (Entity == TEXT("node"))
		{
			FString NodeId;
			if (!SubParams->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
			{
				return MissingParam(TEXT("node_id"), Entity);
			}
			// HandleBlueprintRemoveNode reads 'graph'; accept graph_name and function_name as aliases.
			FString GraphName;
			if (!SubParams->TryGetStringField(TEXT("graph"), GraphName) || GraphName.IsEmpty())
			{
				if (SubParams->TryGetStringField(TEXT("graph_name"), GraphName) && !GraphName.IsEmpty())
				{
					SubParams->SetStringField(TEXT("graph"), GraphName);
				}
				else if (SubParams->TryGetStringField(TEXT("function_name"), GraphName) && !GraphName.IsEmpty())
				{
					SubParams->SetStringField(TEXT("graph"), GraphName);
				}
				else
				{
					// Default to EventGraph if caller did not specify — matches legacy behavior
					// and keeps the common case (remove an event graph node) ergonomic.
					SubParams->SetStringField(TEXT("graph"), TEXT("EventGraph"));
				}
			}
			return HandleBlueprintRemoveNode(SubParams);
		}
		if (Entity == TEXT("component"))
		{
			FString CompName;
			if (!SubParams->TryGetStringField(TEXT("component_name"), CompName) || CompName.IsEmpty())
			{
				SubParams->TryGetStringField(TEXT("name"), CompName);
			}
			if (CompName.IsEmpty())
			{
				return MissingParam(TEXT("component_name"), Entity);
			}
			SubParams->SetStringField(TEXT("name"), CompName);
			return HandleBlueprintRemoveComponent(SubParams);
		}
		if (Entity == TEXT("variable"))
		{
			FString VarName;
			if (!SubParams->TryGetStringField(TEXT("variable_name"), VarName) || VarName.IsEmpty())
			{
				SubParams->TryGetStringField(TEXT("name"), VarName);
			}
			if (VarName.IsEmpty())
			{
				return MissingParam(TEXT("variable_name"), Entity);
			}
			SubParams->SetStringField(TEXT("name"), VarName);
			return HandleBlueprintRemoveVariable(SubParams);
		}
		if (Entity == TEXT("function"))
		{
			FString FuncName;
			if (!SubParams->TryGetStringField(TEXT("function_name"), FuncName) || FuncName.IsEmpty())
			{
				SubParams->TryGetStringField(TEXT("name"), FuncName);
			}
			if (FuncName.IsEmpty())
			{
				return MissingParam(TEXT("function_name"), Entity);
			}
			SubParams->SetStringField(TEXT("name"), FuncName);
			return HandleBlueprintRemoveFunction(SubParams);
		}
		if (Entity == TEXT("interface"))
		{
			FString InterfacePath;
			if (!SubParams->TryGetStringField(TEXT("interface_path"), InterfacePath) || InterfacePath.IsEmpty())
			{
				SubParams->TryGetStringField(TEXT("interface"), InterfacePath);
			}
			if (InterfacePath.IsEmpty())
			{
				return MissingParam(TEXT("interface_path"), Entity);
			}
			SubParams->SetStringField(TEXT("interface"), InterfacePath);
			return HandleBlueprintRemoveInterface(SubParams);
		}

		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_VALUE"),
			FString::Printf(TEXT("Unknown entity '%s'"), *Entity),
			TEXT("entity must be one of: blueprint, node, component, variable, function, interface"));
	}

	// --- Whole-Blueprint delete (legacy behavior) ---
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
// Consolidated Dispatchers (P5): blueprint.modify and blueprint.add
//
// These dispatchers route on 'entity' (+ optional 'action') to the existing
// specialized handlers. Legacy tool names are preserved as aliases that
// pre-fill entity/action before dispatch. Dispatchers validate the minimum
// fields they need to route correctly; downstream handlers perform their own
// per-operation validation.
// ============================================================================

namespace
{
	/** Clone a JSON params object so we can inject/rename fields without mutating the caller. */
	TSharedPtr<FJsonObject> CloneModifyParams(const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Params.IsValid())
		{
			for (const auto& Pair : Params->Values) { Out->Values.Add(Pair.Key, Pair.Value); }
		}
		return Out;
	}

	/** Ensure Target has a string field 'Dst' — copy from 'Src' if 'Dst' is missing/empty. */
	void EnsureStringField(TSharedPtr<FJsonObject>& Target, const TCHAR* Dst, const TCHAR* Src)
	{
		FString Existing;
		if (Target->TryGetStringField(Dst, Existing) && !Existing.IsEmpty())
		{
			return;
		}
		FString SrcValue;
		if (Target->TryGetStringField(Src, SrcValue) && !SrcValue.IsEmpty())
		{
			Target->SetStringField(Dst, SrcValue);
		}
	}

	FOliveToolResult MissingEntityParam(const FString& FieldName, const FString& Entity, const FString& Action)
	{
		FString Label = Action.IsEmpty()
			? FString::Printf(TEXT("entity='%s'"), *Entity)
			: FString::Printf(TEXT("entity='%s', action='%s'"), *Entity, *Action);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			FString::Printf(TEXT("Missing required parameter '%s' for %s"), *FieldName, *Label),
			FString::Printf(TEXT("Provide '%s' when %s."), *FieldName, *Label));
	}
} // anonymous namespace

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintModify(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a params object with 'path' and 'entity' fields."));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blueprint asset path."));
	}

	FString Entity;
	Params->TryGetStringField(TEXT("entity"), Entity);
	Entity = Entity.ToLower();
	if (Entity.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'entity'"),
			TEXT("entity must be one of: component, function, variable, node, pin_default, blueprint"));
	}

	FString Action;
	Params->TryGetStringField(TEXT("action"), Action);
	Action = Action.ToLower();

	TSharedPtr<FJsonObject> SubParams = CloneModifyParams(Params);

	if (Entity == TEXT("component"))
	{
		EnsureStringField(SubParams, TEXT("name"), TEXT("component_name"));
		FString Name;
		if (!SubParams->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return MissingEntityParam(TEXT("component_name"), Entity, Action);
		}

		if (Action == TEXT("reparent"))
		{
			if (!SubParams->HasField(TEXT("new_parent")))
			{
				return MissingEntityParam(TEXT("new_parent"), Entity, Action);
			}
			return HandleBlueprintReparentComponent(SubParams);
		}
		// Default / set_properties path
		if (!SubParams->HasField(TEXT("properties")))
		{
			return MissingEntityParam(TEXT("properties"), Entity,
				Action.IsEmpty() ? FString(TEXT("set_properties")) : Action);
		}
		return HandleBlueprintModifyComponent(SubParams);
	}

	if (Entity == TEXT("function"))
	{
		EnsureStringField(SubParams, TEXT("name"), TEXT("function_name"));
		FString Name;
		if (!SubParams->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return MissingEntityParam(TEXT("function_name"), Entity, Action);
		}

		if (Action == TEXT("override_virtual"))
		{
			// Reuse the add_function path with function_type=override.
			SubParams->SetStringField(TEXT("function_type"), TEXT("override"));
			EnsureStringField(SubParams, TEXT("function_name"), TEXT("name"));
			return HandleBlueprintAddFunction(SubParams);
		}

		// Default / set_signature
		if (!SubParams->HasField(TEXT("changes")))
		{
			return MissingEntityParam(TEXT("changes"), Entity,
				Action.IsEmpty() ? FString(TEXT("set_signature")) : Action);
		}
		return HandleBlueprintModifyFunctionSignature(SubParams);
	}

	if (Entity == TEXT("variable"))
	{
		// HandleBlueprintAddVariable accepts either nested {variable: {...}} or
		// flat {name, type, ...}. For modify we want set_properties semantics
		// (upsert with modify_only=true). Ensure a variable_name is present.
		FString VarName;
		if (!SubParams->TryGetStringField(TEXT("variable_name"), VarName) || VarName.IsEmpty())
		{
			SubParams->TryGetStringField(TEXT("name"), VarName);
		}
		if (VarName.IsEmpty())
		{
			// Maybe nested
			const TSharedPtr<FJsonObject>* VarObj = nullptr;
			if (SubParams->TryGetObjectField(TEXT("variable"), VarObj) && VarObj && VarObj->IsValid())
			{
				(*VarObj)->TryGetStringField(TEXT("name"), VarName);
			}
		}
		if (VarName.IsEmpty())
		{
			return MissingEntityParam(TEXT("variable_name"), Entity, Action);
		}

		// Require at least something to set — either a 'properties' map
		// (rich kwargs) or 'changes' or a 'variable' spec. Otherwise this
		// would be a no-op.
		if (!SubParams->HasField(TEXT("properties"))
			&& !SubParams->HasField(TEXT("changes"))
			&& !SubParams->HasField(TEXT("variable")))
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				TEXT("Missing modification payload for entity='variable'"),
				TEXT("Provide 'properties' (rich kwargs), 'changes', or a 'variable' spec."));
		}

		// Ensure HandleBlueprintAddVariable sees the variable name.
		if (!SubParams->HasField(TEXT("name")))
		{
			SubParams->SetStringField(TEXT("name"), VarName);
		}
		// Force upsert-modify semantics so we don't accidentally create a new variable.
		SubParams->SetBoolField(TEXT("modify_only"), true);
		return HandleBlueprintAddVariable(SubParams);
	}

	if (Entity == TEXT("node"))
	{
		// Ensure we have graph + node_id routing info
		if (!SubParams->HasField(TEXT("graph")))
		{
			EnsureStringField(SubParams, TEXT("graph"), TEXT("graph_name"));
		}
		if (Action == TEXT("move"))
		{
			// HandleBlueprintSetNodeProperty can technically be used, but there's no
			// dedicated move handler. Implement inline as a graph-writer call.
			FString GraphName, NodeId;
			if (!SubParams->TryGetStringField(TEXT("graph"), GraphName) || GraphName.IsEmpty())
			{
				return MissingEntityParam(TEXT("graph"), Entity, Action);
			}
			if (!SubParams->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
			{
				return MissingEntityParam(TEXT("node_id"), Entity, Action);
			}
			int32 PosX = 0, PosY = 0;
			const bool bHasX = SubParams->HasField(TEXT("pos_x"));
			const bool bHasY = SubParams->HasField(TEXT("pos_y"));
			if (bHasX) { PosX = static_cast<int32>(SubParams->GetNumberField(TEXT("pos_x"))); }
			if (bHasY) { PosY = static_cast<int32>(SubParams->GetNumberField(TEXT("pos_y"))); }
			if (!bHasX && !bHasY)
			{
				return FOliveToolResult::Error(
					TEXT("VALIDATION_MISSING_PARAM"),
					TEXT("Missing 'pos_x' and/or 'pos_y' for node.move"),
					TEXT("Provide at least one of pos_x / pos_y."));
			}

			// Load Blueprint and move the cached node in a transaction.
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
			if (!Blueprint)
			{
				return FOliveToolResult::Error(
					TEXT("ASSET_NOT_FOUND"),
					FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
					TEXT("Verify the asset path is correct."));
			}

			FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();
			UEdGraphNode* Node = GraphWriter.GetCachedNode(AssetPath, NodeId);
			if (!Node)
			{
				return FOliveToolResult::Error(
					TEXT("NODE_NOT_FOUND"),
					FString::Printf(TEXT("Node '%s' not found (may not have been created in this session)"), *NodeId),
					TEXT("Re-read the graph to refresh node IDs, or use add_node first."));
			}

			// Build a write request / pipeline execution so the move participates in undo.
			FOliveWriteRequest Request;
			Request.ToolName = TEXT("blueprint.modify");
			Request.Params = SubParams;
			Request.AssetPath = AssetPath;
			Request.TargetAsset = Blueprint;
			Request.OperationDescription = FText::FromString(
				FString::Printf(TEXT("Move node '%s' to (%d, %d)"), *NodeId, PosX, PosY));
			Request.OperationCategory = TEXT("graph_editing");
			Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
			Request.bAutoCompile = false;
			Request.bSkipVerification = true;

			FOliveWriteExecutor Executor;
			Executor.BindLambda([Node, PosX, PosY, bHasX, bHasY](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
			{
				Node->Modify();
				if (bHasX) { Node->NodePosX = PosX; }
				if (bHasY) { Node->NodePosY = PosY; }
				TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetNumberField(TEXT("pos_x"), Node->NodePosX);
				Data->SetNumberField(TEXT("pos_y"), Node->NodePosY);
				return FOliveWriteResult::Success(Data);
			});

			FOliveWriteResult R = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);
			return R.ToToolResult();
		}

		// Default / set_property
		return HandleBlueprintSetNodeProperty(SubParams);
	}

	if (Entity == TEXT("pin_default"))
	{
		// HandleBlueprintSetPinDefault uses 'pin' + 'value'. Accept 'pin_name' as alias.
		EnsureStringField(SubParams, TEXT("pin"), TEXT("pin_name"));
		return HandleBlueprintSetPinDefault(SubParams);
	}

	if (Entity == TEXT("blueprint"))
	{
		if (Action == TEXT("set_parent_class"))
		{
			return HandleBlueprintSetParentClass(SubParams);
		}
		if (Action == TEXT("set_defaults"))
		{
			// DESIGN NOTE: No dedicated HandleBlueprintSetDefaults exists yet.
			// Until it is added we surface a structured "not implemented" error
			// so callers can still discover the entity shape from the schema.
			return FOliveToolResult::Error(
				TEXT("NOT_IMPLEMENTED"),
				TEXT("entity='blueprint', action='set_defaults' is not implemented yet"),
				TEXT("Use editor.run_python to set Class Defaults, or modify individual components/variables."));
		}
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_VALUE"),
			FString::Printf(TEXT("Unknown action '%s' for entity='blueprint'"), *Action),
			TEXT("action must be 'set_parent_class' or 'set_defaults'."));
	}

	return FOliveToolResult::Error(
		TEXT("VALIDATION_INVALID_VALUE"),
		FString::Printf(TEXT("Unknown entity '%s'"), *Entity),
		TEXT("entity must be one of: component, function, variable, node, pin_default, blueprint"));
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintAdd(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a params object with 'path' and 'entity' fields."));
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blueprint asset path."));
	}

	FString Entity;
	Params->TryGetStringField(TEXT("entity"), Entity);
	Entity = Entity.ToLower();
	if (Entity.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'entity'"),
			TEXT("entity must be one of: node, variable, function, component, custom_event, event_dispatcher, interface, timeline"));
	}

	TSharedPtr<FJsonObject> SubParams = CloneModifyParams(Params);

	if (Entity == TEXT("node"))
	{
		// HandleBlueprintAddNode expects 'type'. Accept 'node_type' as an alias.
		EnsureStringField(SubParams, TEXT("type"), TEXT("node_type"));
		return HandleBlueprintAddNode(SubParams);
	}
	if (Entity == TEXT("variable"))
	{
		return HandleBlueprintAddVariable(SubParams);
	}
	if (Entity == TEXT("function"))
	{
		// Ensure function_type=function if caller didn't set one.
		if (!SubParams->HasField(TEXT("function_type")))
		{
			SubParams->SetStringField(TEXT("function_type"), TEXT("function"));
		}
		return HandleBlueprintAddFunction(SubParams);
	}
	if (Entity == TEXT("component"))
	{
		return HandleBlueprintAddComponent(SubParams);
	}
	if (Entity == TEXT("custom_event"))
	{
		SubParams->SetStringField(TEXT("function_type"), TEXT("custom_event"));
		return HandleBlueprintAddFunction(SubParams);
	}
	if (Entity == TEXT("event_dispatcher"))
	{
		SubParams->SetStringField(TEXT("function_type"), TEXT("event_dispatcher"));
		return HandleBlueprintAddFunction(SubParams);
	}
	if (Entity == TEXT("interface"))
	{
		// HandleBlueprintAddInterface uses 'interface' string field — alias interface_path.
		EnsureStringField(SubParams, TEXT("interface"), TEXT("interface_path"));
		return HandleBlueprintAddInterface(SubParams);
	}
	if (Entity == TEXT("timeline"))
	{
		// HandleBlueprintCreateTimeline accepts its params directly.
		return HandleBlueprintCreateTimeline(SubParams);
	}

	return FOliveToolResult::Error(
		TEXT("VALIDATION_INVALID_VALUE"),
		FString::Printf(TEXT("Unknown entity '%s'"), *Entity),
		TEXT("entity must be one of: node, variable, function, component, custom_event, event_dispatcher, interface, timeline"));
}

// ============================================================================
// Blueprint Interface Creation
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintCreateInterface(
	const TSharedPtr<FJsonObject>& Params)
{
	// Validate parameters
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide 'path' and 'functions' parameters")
		);
	}

	// Extract path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing or empty"),
			TEXT("Provide the interface asset path (e.g., '/Game/Interfaces/BPI_Interactable')")
		);
	}

	// Path validation (same as HandleBlueprintCreate)
	{
		FString ShortName = FPackageName::GetShortName(AssetPath);
		if (ShortName.IsEmpty() || AssetPath.EndsWith(TEXT("/")))
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_PATH_IS_FOLDER"),
				FString::Printf(TEXT("'%s' is a folder path, not an asset path."),
					*AssetPath),
				FString::Printf(TEXT("Append the interface name: '%s/BPI_Interactable'"),
					*AssetPath)
			);
		}

		if (!AssetPath.StartsWith(TEXT("/Game/")))
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_INVALID_PATH_PREFIX"),
				FString::Printf(TEXT("Path '%s' must start with '/Game/'."),
					*AssetPath),
				FString::Printf(TEXT("Use '/Game/Interfaces/%s'"), *ShortName)
			);
		}
	}

	// Extract functions array
	const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("functions"), FunctionsArray)
		|| !FunctionsArray || FunctionsArray->Num() == 0)
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'functions' is missing or empty"),
			TEXT("Provide at least one function definition: "
				 "[{\"name\": \"Interact\", \"inputs\": [{\"name\": \"Caller\", \"type\": \"Actor\"}]}]")
		);
	}

	// Parse function signatures
	TArray<FOliveIRFunctionSignature> Functions;
	for (const TSharedPtr<FJsonValue>& FuncValue : *FunctionsArray)
	{
		const TSharedPtr<FJsonObject>* FuncObjPtr = nullptr;
		if (!FuncValue->TryGetObject(FuncObjPtr) || !FuncObjPtr->IsValid())
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& FuncObj = *FuncObjPtr;

		FOliveIRFunctionSignature Sig;

		// Name (required)
		if (!FuncObj->TryGetStringField(TEXT("name"), Sig.Name)
			|| Sig.Name.IsEmpty())
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				TEXT("Each function must have a 'name' field"),
				TEXT("Example: {\"name\": \"Interact\"}")
			);
		}

		// Parse inputs
		const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
		if (FuncObj->TryGetArrayField(TEXT("inputs"), InputsArray) && InputsArray)
		{
			for (const TSharedPtr<FJsonValue>& InputValue : *InputsArray)
			{
				const TSharedPtr<FJsonObject>* InputObjPtr = nullptr;
				if (!InputValue->TryGetObject(InputObjPtr) || !InputObjPtr->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>& InputObj = *InputObjPtr;

				FOliveIRFunctionParam Param;
				InputObj->TryGetStringField(TEXT("name"), Param.Name);

				// Type -- accept string or object with "category" key
				FString TypeStr;
				const TSharedPtr<FJsonObject>* TypeJsonPtr = nullptr;
				if (InputObj->TryGetObjectField(TEXT("type"), TypeJsonPtr)
					&& TypeJsonPtr->IsValid())
				{
					Param.Type = ParseTypeFromParams(*TypeJsonPtr);
				}
				else if (InputObj->TryGetStringField(TEXT("type"), TypeStr))
				{
					// Simple string type (e.g., "Actor", "Float", "Bool")
					// ParseTypeFromParams expects "category" as the field name
					TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject());
					TypeObj->SetStringField(TEXT("category"), TypeStr);
					Param.Type = ParseTypeFromParams(TypeObj);
				}

				Sig.Inputs.Add(Param);
			}
		}

		// Parse outputs (same pattern)
		const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
		if (FuncObj->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray)
		{
			for (const TSharedPtr<FJsonValue>& OutputValue : *OutputsArray)
			{
				const TSharedPtr<FJsonObject>* OutputObjPtr = nullptr;
				if (!OutputValue->TryGetObject(OutputObjPtr)
					|| !OutputObjPtr->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>& OutputObj = *OutputObjPtr;

				FOliveIRFunctionParam Param;
				OutputObj->TryGetStringField(TEXT("name"), Param.Name);

				FString TypeStr;
				const TSharedPtr<FJsonObject>* TypeJsonPtr = nullptr;
				if (OutputObj->TryGetObjectField(TEXT("type"), TypeJsonPtr)
					&& TypeJsonPtr->IsValid())
				{
					Param.Type = ParseTypeFromParams(*TypeJsonPtr);
				}
				else if (OutputObj->TryGetStringField(TEXT("type"), TypeStr))
				{
					TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject());
					TypeObj->SetStringField(TEXT("category"), TypeStr);
					Param.Type = ParseTypeFromParams(TypeObj);
				}

				Sig.Outputs.Add(Param);
			}
		}

		Functions.Add(Sig);
	}

	// Build write request
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.create_interface");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = nullptr; // New asset, does not exist yet
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Create Blueprint Interface '%s' with %d functions"),
			*AssetPath, Functions.Num()));
	Request.OperationCategory = TEXT("create"); // Tier 1
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, Functions](
		const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult =
			Writer.CreateBlueprintInterface(AssetPath, Functions);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.Errors.Num() > 0
				? WriteResult.Errors[0] : TEXT("Unknown error");
			return FOliveWriteResult::ExecutionError(
				TEXT("BPI_CREATE_FAILED"),
				ErrorMsg,
				TEXT("Check the path and function signatures")
			);
		}

		// Build success result
		TSharedPtr<FJsonObject> ResultData =
			MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"),
			WriteResult.AssetPath);
		ResultData->SetStringField(TEXT("created_name"),
			WriteResult.CreatedItemName);

		// List created functions
		TArray<TSharedPtr<FJsonValue>> FuncNames;
		for (const FOliveIRFunctionSignature& Sig : Functions)
		{
			FuncNames.Add(MakeShareable(
				new FJsonValueString(Sig.Name)));
		}
		ResultData->SetArrayField(TEXT("functions"), FuncNames);

		// Include usage guidance in result
		ResultData->SetStringField(TEXT("next_steps"),
			FString::Printf(
				TEXT("Interface created. To use it: "
					 "1) blueprint.add_interface on target BPs with interface='%s'. "
					 "2) Functions without outputs become events the BP must implement. "
					 "3) Functions with outputs become functions the BP must override. "
					 "4) Call through the interface with plan_json: "
					 "{\"op\": \"call\", \"target\": \"FunctionName\", "
					 "\"target_class\": \"%s\"} (creates UK2Node_Message)."),
				*WriteResult.AssetPath,
				*FPackageName::GetShortName(WriteResult.AssetPath)));

		// Embed warnings in result data if any
		if (WriteResult.Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningValues;
			for (const FString& W : WriteResult.Warnings)
			{
				WarningValues.Add(MakeShareable(new FJsonValueString(W)));
			}
			ResultData->SetArrayField(TEXT("warnings"), WarningValues);
		}

		FOliveWriteResult WR = FOliveWriteResult::Success(ResultData);
		WR.CreatedItem = AssetPath;

		return WR;
	});

	// Execute through pipeline
	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult Result =
		ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

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

	// Check for modify_only flag
	bool bModifyOnly = false;
	Params->TryGetBoolField(TEXT("modify_only"), bModifyOnly);

	// Detect modify_variable alias format: {path, name, changes} — no 'variable' object
	const TSharedPtr<FJsonObject>* ChangesJsonPtr = nullptr;
	bool bIsModifyFormat = false;
	FString ModifyVariableName;
	if (Params->TryGetObjectField(TEXT("changes"), ChangesJsonPtr) && ChangesJsonPtr && (*ChangesJsonPtr).IsValid())
	{
		if (Params->TryGetStringField(TEXT("name"), ModifyVariableName) && !ModifyVariableName.IsEmpty())
		{
			bIsModifyFormat = true;
			bModifyOnly = true; // Implicit: modify_variable format always requires existence
			UE_LOG(LogOliveBPTools, Log, TEXT("add_variable: Detected modify_variable alias format (name='%s')"), *ModifyVariableName);
		}
	}

	// If this is a pure modify_variable format, delegate to the modify path directly
	if (bIsModifyFormat)
	{
		// Parse changes into a modifications map
		TSharedPtr<FJsonObject> ChangesJson = *ChangesJsonPtr;
		TMap<FString, FString> Modifications;
		FString TempValue;
		bool TempBool;

		if (ChangesJson->TryGetStringField(TEXT("default_value"), TempValue))
			Modifications.Add(TEXT("DefaultValue"), TempValue);
		if (ChangesJson->TryGetStringField(TEXT("category"), TempValue))
			Modifications.Add(TEXT("Category"), TempValue);
		if (ChangesJson->TryGetStringField(TEXT("description"), TempValue))
			Modifications.Add(TEXT("Description"), TempValue);
		if (ChangesJson->TryGetBoolField(TEXT("blueprint_read_write"), TempBool))
			Modifications.Add(TEXT("bBlueprintReadWrite"), TempBool ? TEXT("true") : TEXT("false"));
		if (ChangesJson->TryGetBoolField(TEXT("expose_on_spawn"), TempBool))
			Modifications.Add(TEXT("bExposeOnSpawn"), TempBool ? TEXT("true") : TEXT("false"));
		if (ChangesJson->TryGetBoolField(TEXT("replicated"), TempBool))
			Modifications.Add(TEXT("bReplicated"), TempBool ? TEXT("true") : TEXT("false"));
		if (ChangesJson->TryGetBoolField(TEXT("save_game"), TempBool))
			Modifications.Add(TEXT("bSaveGame"), TempBool ? TEXT("true") : TEXT("false"));
		if (ChangesJson->TryGetBoolField(TEXT("edit_anywhere"), TempBool))
			Modifications.Add(TEXT("bEditAnywhere"), TempBool ? TEXT("true") : TEXT("false"));
		if (ChangesJson->TryGetBoolField(TEXT("blueprint_visible"), TempBool))
			Modifications.Add(TEXT("bBlueprintVisible"), TempBool ? TEXT("true") : TEXT("false"));

		if (Modifications.Num() == 0)
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_NO_CHANGES"),
				TEXT("No valid modifications found in 'changes' object"),
				TEXT("Provide at least one property to modify (e.g., default_value, category, description, flags)")
			);
		}

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			return FOliveToolResult::Error(
				TEXT("ASSET_NOT_FOUND"),
				FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
				TEXT("Verify the asset path is correct and the asset exists")
			);
		}

		FOliveWriteRequest Request;
		Request.ToolName = TEXT("blueprint.add_variable");
		Request.Params = Params;
		Request.AssetPath = AssetPath;
		Request.TargetAsset = Blueprint;
		Request.OperationDescription = FText::FromString(
			FString::Printf(TEXT("Modify variable '%s' in '%s'"), *ModifyVariableName, *AssetPath));
		Request.OperationCategory = TEXT("variable");
		Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
		Request.bAutoCompile = false;
		Request.bSkipVerification = false;

		FOliveWriteExecutor Executor;
		Executor.BindLambda([AssetPath, ModifyVariableName, Modifications](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
		{
			FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
			FOliveBlueprintWriteResult WriteResult = Writer.ModifyVariable(AssetPath, ModifyVariableName, Modifications);

			if (!WriteResult.bSuccess)
			{
				FString ErrorMsg = WriteResult.GetFirstError();
				return FOliveWriteResult::ExecutionError(
					TEXT("BP_MODIFY_VARIABLE_FAILED"),
					ErrorMsg,
					TEXT("Verify the variable exists and the modifications are valid")
				);
			}

			TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
			ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
			ResultData->SetStringField(TEXT("variable_name"), ModifyVariableName);
			ResultData->SetNumberField(TEXT("modified_properties_count"), Modifications.Num());

			if (TSharedPtr<FJsonObject> StateJson = BuildCompactStateJson(Cast<UBlueprint>(Target)))
			{
				ResultData->SetObjectField(TEXT("blueprint_state"), StateJson);
			}

			return FOliveWriteResult::Success(ResultData);
		});

		FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
		FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);
		return Result.ToToolResult();
	}

	// Standard add_variable path (with upsert support)

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

	// Check if variable already exists — upsert behavior
	bool bVariableExists = false;
	for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
	{
		if (VarDesc.VarName.ToString() == Variable.Name)
		{
			bVariableExists = true;
			break;
		}
	}

	if (bVariableExists)
	{
		// Variable already exists: modify it in-place
		UE_LOG(LogOliveBPTools, Log, TEXT("add_variable: Variable '%s' already exists in '%s', upserting (modifying in-place)"),
			*Variable.Name, *AssetPath);

		// Build modifications map from the variable IR
		TMap<FString, FString> Modifications;
		if (!Variable.DefaultValue.IsEmpty())
			Modifications.Add(TEXT("DefaultValue"), Variable.DefaultValue);
		if (!Variable.Category.IsEmpty())
			Modifications.Add(TEXT("Category"), Variable.Category);
		if (!Variable.Description.IsEmpty())
			Modifications.Add(TEXT("Description"), Variable.Description);
		// Always apply flag values from the variable spec
		Modifications.Add(TEXT("bBlueprintReadWrite"), Variable.bBlueprintReadWrite ? TEXT("true") : TEXT("false"));
		Modifications.Add(TEXT("bExposeOnSpawn"), Variable.bExposeOnSpawn ? TEXT("true") : TEXT("false"));
		Modifications.Add(TEXT("bReplicated"), Variable.bReplicated ? TEXT("true") : TEXT("false"));
		Modifications.Add(TEXT("bSaveGame"), Variable.bSaveGame ? TEXT("true") : TEXT("false"));

		FOliveWriteRequest Request;
		Request.ToolName = TEXT("blueprint.add_variable");
		Request.Params = Params;
		Request.AssetPath = AssetPath;
		Request.TargetAsset = Blueprint;
		Request.OperationDescription = FText::FromString(
			FString::Printf(TEXT("Update variable '%s' in '%s'"), *Variable.Name, *AssetPath));
		Request.OperationCategory = TEXT("variable");
		Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
		Request.bAutoCompile = false;
		Request.bSkipVerification = false;

		FString VarName = Variable.Name;
		FOliveWriteExecutor Executor;
		Executor.BindLambda([AssetPath, VarName, Modifications](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
		{
			FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
			FOliveBlueprintWriteResult WriteResult = Writer.ModifyVariable(AssetPath, VarName, Modifications);

			if (!WriteResult.bSuccess)
			{
				FString ErrorMsg = WriteResult.GetFirstError();
				return FOliveWriteResult::ExecutionError(
					TEXT("BP_MODIFY_VARIABLE_FAILED"),
					ErrorMsg,
					TEXT("Verify the variable exists and the modifications are valid")
				);
			}

			TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
			ResultData->SetStringField(TEXT("asset_path"), WriteResult.AssetPath);
			ResultData->SetStringField(TEXT("variable_name"), VarName);
			ResultData->SetStringField(TEXT("action"), TEXT("updated"));
			ResultData->SetNumberField(TEXT("modified_properties_count"), Modifications.Num());

			return FOliveWriteResult::Success(ResultData);
		});

		FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
		FOliveWriteResult Result = ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);
		return Result.ToToolResult();
	}

	// Variable does not exist

	if (bModifyOnly)
	{
		// Check if the variable is inherited from a parent Blueprint before returning not-found
		bool bFoundInherited = false;
		UClass* CheckClass = Blueprint->ParentClass;
		while (CheckClass)
		{
			UBlueprint* ParentBP = Cast<UBlueprint>(CheckClass->ClassGeneratedBy);
			if (ParentBP)
			{
				for (const FBPVariableDescription& VarDesc : ParentBP->NewVariables)
				{
					if (VarDesc.VarName.ToString() == Variable.Name)
					{
						bFoundInherited = true;
						break;
					}
				}
				if (bFoundInherited) break;
				CheckClass = ParentBP->ParentClass;
			}
			else break;
		}

		if (bFoundInherited)
		{
			return FOliveToolResult::Error(
				TEXT("INHERITED_VARIABLE"),
				FString::Printf(TEXT("Variable '%s' is inherited from a parent Blueprint. "
					"Inherited variable defaults cannot be changed in child Blueprints. "
					"To set this variable's value at runtime, use plan_json with set_var "
					"in the child's BeginPlay event."), *Variable.Name));
		}

		// modify_only=true requires the variable to already exist
		return FOliveToolResult::Error(
			TEXT("VARIABLE_NOT_FOUND"),
			FString::Printf(TEXT("Variable '%s' does not exist in Blueprint '%s' and modify_only=true"),
				*Variable.Name, *AssetPath),
			TEXT("Set modify_only=false or omit it to allow creation, or verify the variable name")
		);
	}

	// Type is required for creation
	if (Variable.Type.Category == EOliveIRTypeCategory::Unknown)
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddVariable: Variable 'type' is Unknown for variable='%s' path='%s'"), *Variable.Name, *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_FIELD"),
			TEXT("Variable 'type' field is required and must be valid for creation"),
			TEXT("Provide a valid type specification with a category")
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
		ResultData->SetStringField(TEXT("action"), TEXT("created"));

		if (TSharedPtr<FJsonObject> StateJson = BuildCompactStateJson(Cast<UBlueprint>(Target)))
		{
			ResultData->SetObjectField(TEXT("blueprint_state"), StateJson);
		}

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
			FString ErrorMsg = WriteResult.GetFirstError();
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

// NOTE: HandleBlueprintModifyVariable has been consolidated into HandleBlueprintAddVariable (upsert).
// The old blueprint.modify_variable tool name is an alias that redirects to blueprint.add_variable.
// HandleBlueprintAddVariable detects the {name, changes} format and routes appropriately.

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
			FString ErrorMsg = WriteResult.GetFirstError();
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

		if (TSharedPtr<FJsonObject> StateJson = BuildCompactStateJson(Cast<UBlueprint>(Target)))
		{
			ResultData->SetObjectField(TEXT("blueprint_state"), StateJson);
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
			FString ErrorMsg = WriteResult.GetFirstError();
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

		const int32 FailedCount = WriteResult.Warnings.Num();
		const int32 ActualSuccessCount = FMath::Max(0, Properties.Num() - FailedCount);
		ResultData->SetNumberField(TEXT("modified_properties_count"), ActualSuccessCount);
		ResultData->SetNumberField(TEXT("requested_properties_count"), Properties.Num());

		if (FailedCount > 0)
		{
			ResultData->SetNumberField(TEXT("failed_properties_count"), FailedCount);
		}

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

		if (TSharedPtr<FJsonObject> StateJson = BuildCompactStateJson(Cast<UBlueprint>(Target)))
		{
			ResultData->SetObjectField(TEXT("blueprint_state"), StateJson);
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
	// Unified handler — routes by function_type param to type-specific helpers.
	// function_type values: "function" (default), "custom_event", "event_dispatcher", "override"

	// Validate parameters
	if (!Params.IsValid())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddFunction: Params object is null"));
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a valid parameters object with 'path' field")
		);
	}

	// Extract path (required for all function_types)
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

	// Load Blueprint (common to all types)
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

	// Determine function_type — defaults to "function"
	FString FunctionType;
	if (!Params->TryGetStringField(TEXT("function_type"), FunctionType) || FunctionType.IsEmpty())
	{
		FunctionType = TEXT("function");
	}

	// Route to type-specific helper
	if (FunctionType == TEXT("custom_event"))
	{
		return HandleAddFunctionType_CustomEvent(Params, AssetPath, Blueprint);
	}
	else if (FunctionType == TEXT("event_dispatcher"))
	{
		return HandleAddFunctionType_EventDispatcher(Params, AssetPath, Blueprint);
	}
	else if (FunctionType == TEXT("override"))
	{
		return HandleAddFunctionType_Override(Params, AssetPath, Blueprint);
	}
	else if (FunctionType == TEXT("function"))
	{
		return HandleAddFunctionType_Function(Params, AssetPath, Blueprint);
	}
	else
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintAddFunction: Unknown function_type='%s'"), *FunctionType);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAM"),
			FString::Printf(TEXT("Unknown function_type '%s'"), *FunctionType),
			TEXT("Use one of: 'function', 'custom_event', 'event_dispatcher', 'override'")
		);
	}
}

// ============================================================================
// function_type='function' — Add a user-defined function with signature
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleAddFunctionType_Function(
	const TSharedPtr<FJsonObject>& Params,
	const FString& AssetPath,
	UBlueprint* Blueprint)
{
	// Extract signature object (required for function type)
	const TSharedPtr<FJsonObject>* SignatureJsonPtr = nullptr;
	TSharedPtr<FJsonObject> ParsedFromString; // holds parsed result if signature was a string

	if (!Params->TryGetObjectField(TEXT("signature"), SignatureJsonPtr) || !SignatureJsonPtr || !SignatureJsonPtr->IsValid())
	{
		// Try parsing signature from a JSON string (common LLM mistake: stringified JSON)
		FString SigString;
		if (Params->TryGetStringField(TEXT("signature"), SigString) && !SigString.IsEmpty())
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SigString);
			if (FJsonSerializer::Deserialize(Reader, ParsedFromString) && ParsedFromString.IsValid())
			{
				SignatureJsonPtr = &ParsedFromString;
				UE_LOG(LogOliveBPTools, Log, TEXT("HandleAddFunctionType_Function: Auto-parsed stringified JSON signature for path='%s'"), *AssetPath);
			}
		}

		if (!SignatureJsonPtr || !SignatureJsonPtr->IsValid())
		{
			UE_LOG(LogOliveBPTools, Warning, TEXT("HandleAddFunctionType_Function: Missing required param 'signature' for path='%s'"), *AssetPath);
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				TEXT("Required parameter 'signature' is missing or invalid"),
				TEXT("Provide a function signature as a JSON object with name, inputs, and outputs")
			);
		}
	}

	// Parse function signature from JSON to IR
	FOliveIRFunctionSignature Signature = ParseFunctionSignatureFromParams(*SignatureJsonPtr);

	// If signature.name is empty, try top-level 'name' or 'function_name' as fallback
	if (Signature.Name.IsEmpty())
	{
		FString TopLevelName;
		if (Params->TryGetStringField(TEXT("name"), TopLevelName) && !TopLevelName.IsEmpty())
		{
			Signature.Name = TopLevelName;
		}
		else if (Params->TryGetStringField(TEXT("function_name"), TopLevelName) && !TopLevelName.IsEmpty())
		{
			Signature.Name = TopLevelName;
		}
	}

	// Validate signature has required fields
	if (Signature.Name.IsEmpty())
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleAddFunctionType_Function: Function 'name' field is empty for path='%s'"), *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_FIELD"),
			TEXT("Function 'name' field is required (in signature or top-level 'name')"),
			TEXT("Provide a name for the function")
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
	Request.bAutoCompile = true;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, Signature](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddFunction(AssetPath, Signature);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.GetFirstError();
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
				 "NEXT STEP: Use blueprint.preview_plan_json + blueprint.apply_plan_json to implement it. "
				 "Do NOT add another function until this one has graph logic."),
			*WriteResult.CreatedItemName));

		// Scan for other empty function graphs on this Blueprint
		UBlueprint* BP = Cast<UBlueprint>(Target);
		if (BP)
		{
			int32 EmptyCount = 0;
			FString FirstEmpty;
			for (const UEdGraph* FuncGraph : BP->FunctionGraphs)
			{
				if (!FuncGraph) continue;
				if (FuncGraph->GetName() == Signature.Name) continue;
				if (FuncGraph->Nodes.Num() <= 2)
				{
					EmptyCount++;
					if (FirstEmpty.IsEmpty()) FirstEmpty = FuncGraph->GetName();
				}
			}
			if (EmptyCount > 0)
			{
				FString CurrentMsg;
				ResultData->TryGetStringField(TEXT("message"), CurrentMsg);
				ResultData->SetStringField(TEXT("message"),
					CurrentMsg + FString::Printf(
						TEXT(" Note: %s has no graph logic yet (%d empty function(s) total). Write graph logic with apply_plan_json before adding more functions."),
						*FirstEmpty, EmptyCount));
			}
		}

		if (TSharedPtr<FJsonObject> StateJson = BuildCompactStateJson(Cast<UBlueprint>(Target)))
		{
			ResultData->SetObjectField(TEXT("blueprint_state"), StateJson);
		}

		return FOliveWriteResult::Success(ResultData);
	});

	// Execute through pipeline
	FOliveWriteResult Result = ExecuteWithOptionalConfirmation(FOliveWritePipeline::Get(), Request, Executor);

	return Result.ToToolResult();
}

// ============================================================================
// function_type='custom_event' — Add a custom event to the event graph
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleAddFunctionType_CustomEvent(
	const TSharedPtr<FJsonObject>& Params,
	const FString& AssetPath,
	UBlueprint* Blueprint)
{
	// Extract event name — try 'name' first, then 'function_name' (set by normalizer)
	FString EventName;
	if (!Params->TryGetStringField(TEXT("name"), EventName) || EventName.IsEmpty())
	{
		if (!Params->TryGetStringField(TEXT("function_name"), EventName) || EventName.IsEmpty())
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				TEXT("Required parameter 'name' is missing or empty"),
				TEXT("Provide the name of the custom event to create")
			);
		}
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

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_function");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add custom event '%s' to '%s'"), *EventName, *AssetPath)
	);
	Request.OperationCategory = TEXT("variable"); // Tier 1 - custom events are low risk
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, EventName, EventParams](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddCustomEvent(AssetPath, EventName, EventParams);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.GetFirstError();
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
// function_type='event_dispatcher' — Add an event dispatcher (multicast delegate)
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleAddFunctionType_EventDispatcher(
	const TSharedPtr<FJsonObject>& Params,
	const FString& AssetPath,
	UBlueprint* Blueprint)
{
	// Extract dispatcher name — try 'name' first, then 'function_name' (set by normalizer)
	FString DispatcherName;
	if (!Params->TryGetStringField(TEXT("name"), DispatcherName) || DispatcherName.IsEmpty())
	{
		if (!Params->TryGetStringField(TEXT("function_name"), DispatcherName) || DispatcherName.IsEmpty())
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				TEXT("Required parameter 'name' is missing or empty"),
				TEXT("Provide the name of the event dispatcher to create")
			);
		}
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

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_function");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Add event dispatcher '%s' to '%s'"), *DispatcherName, *AssetPath)
	);
	Request.OperationCategory = TEXT("variable"); // Tier 1 - dispatchers are like variables
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, DispatcherName, DispatcherParams](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddEventDispatcher(AssetPath, DispatcherName, DispatcherParams);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.GetFirstError();
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

// ============================================================================
// function_type='override' — Override a parent class or interface function
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleAddFunctionType_Override(
	const TSharedPtr<FJsonObject>& Params,
	const FString& AssetPath,
	UBlueprint* Blueprint)
{
	// Extract function name — try 'function_name' first, then 'name'
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		if (!Params->TryGetStringField(TEXT("name"), FunctionName) || FunctionName.IsEmpty())
		{
			UE_LOG(LogOliveBPTools, Warning, TEXT("HandleAddFunctionType_Override: Missing required param 'function_name' for path='%s'"), *AssetPath);
			return FOliveToolResult::Error(
				TEXT("VALIDATION_MISSING_PARAM"),
				TEXT("Required parameter 'function_name' (or 'name') is missing or empty"),
				TEXT("Provide the name of the parent function to override")
			);
		}
	}

	// Build write request for pipeline
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_function");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Override function '%s' in '%s'"), *FunctionName, *AssetPath)
	);
	Request.OperationCategory = TEXT("function_creation"); // Tier 2
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true;
	Request.bSkipVerification = false;

	// Create executor
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, FunctionName](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.OverrideFunction(AssetPath, FunctionName);

		if (!WriteResult.bSuccess)
		{
			FString ErrorMsg = WriteResult.GetFirstError();
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_OVERRIDE_FUNCTION_FAILED"),
				ErrorMsg,
				TEXT("Verify the function exists in a parent class or implemented interface and is overridable")
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
	Request.bAutoCompile = true;
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

		FString SuggestionText = Suggestions.Num() > 0
			? FString::Printf(TEXT("Did you mean '%s'? "), *Suggestions[0].DisplayName)
			: TEXT("Use blueprint.describe_node_type to check available types. ");
		SuggestionText += TEXT("Alternatively, use editor.run_python for node types not supported by add_node.");

		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("NODE_TYPE_UNKNOWN"),
			FString::Printf(TEXT("Node type '%s' is not recognized"), *NodeType),
			SuggestionText
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
			FString ErrorMsg = WriteResult.GetFirstError();

			// Defense-in-depth: detect duplicate native event from NodeFactory
			FString ErrorCode = TEXT("BP_ADD_NODE_FAILED");
			FString Suggestion;
			if (ErrorMsg.Contains(TEXT("already exists")))
			{
				ErrorCode = TEXT("DUPLICATE_NATIVE_EVENT");
				Suggestion = TEXT("Use blueprint.read_event_graph to see existing event nodes, "
					"or use 'CustomEvent' type to create user-defined events");
			}
			else
			{
				// Provide actionable suggestions with Python fallback
				Suggestion = TEXT("Check the type name and properties. ");
				if (NodeType.Contains(TEXT("K2Node_")) || NodeType.Contains(TEXT("Input")))
				{
					Suggestion += TEXT("For complex node types, try editor.run_python with unreal.BlueprintEditorLibrary or direct node creation via Python.");
				}
				else
				{
					Suggestion += TEXT("Use blueprint.describe_node_type to check available types, or editor.run_python for types not supported by add_node.");
				}
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
					for (FBPInterfaceDescription& InterfaceDesc : BP->ImplementedInterfaces)
					{
						for (UEdGraph* G : InterfaceDesc.Graphs)
						{
							if (G && G->GetName() == GraphName)
							{
								Graph = G;
								break;
							}
						}
						if (Graph) { break; }
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
		ResultData->SetStringField(TEXT("note"),
			TEXT("Node indices from blueprint.read will have shifted. "
				 "Re-read the graph before referencing other nodes by index."));
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
			FString ErrorMsg = WriteResult.GetFirstError();

			// Check for structured wiring diagnostic (type incompatibility)
			if (WriteResult.WiringDiagnostic.IsSet())
			{
				const FOliveWiringDiagnostic& Diag = WriteResult.WiringDiagnostic.GetValue();

				// Build rich error result with diagnostic JSON
				TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
				ErrorData->SetStringField(TEXT("source_type"), Diag.SourceTypeName);
				ErrorData->SetStringField(TEXT("target_type"), Diag.TargetTypeName);
				ErrorData->SetStringField(TEXT("source_pin"), Diag.SourcePinName);
				ErrorData->SetStringField(TEXT("target_pin"), Diag.TargetPinName);
				ErrorData->SetStringField(TEXT("failure_reason"),
					FOliveWiringDiagnostic::ReasonToString(Diag.Reason));
				ErrorData->SetStringField(TEXT("why_autofix_failed"), Diag.WhyAutoFixFailed);

				TArray<TSharedPtr<FJsonValue>> AltArray;
				for (const FOliveWiringAlternative& Alt : Diag.Alternatives)
				{
					TSharedPtr<FJsonObject> AltObj = MakeShared<FJsonObject>();
					AltObj->SetStringField(TEXT("label"), Alt.Label);
					AltObj->SetStringField(TEXT("action"), Alt.Action);
					AltObj->SetStringField(TEXT("confidence"), Alt.Confidence);
					AltArray.Add(MakeShared<FJsonValueObject>(AltObj));
				}
				ErrorData->SetArrayField(TEXT("alternatives"), AltArray);

				FOliveWriteResult ErrResult = FOliveWriteResult::ExecutionError(
					TEXT("BP_CONNECT_PINS_INCOMPATIBLE"),
					Diag.ToHumanReadable(),
					Diag.Alternatives.Num() > 0
						? Diag.Alternatives[0].Action
						: TEXT("Check pin types and plan logic"));
				ErrResult.ResultData = ErrorData;
				return ErrResult;
			}

			// Fallback for non-type errors (pin not found, etc.)
			return FOliveWriteResult::ExecutionError(
				TEXT("BP_CONNECT_PINS_FAILED"),
				ErrorMsg.IsEmpty()
					? TEXT("Connection failed — pins may already be wired. Use blueprint.read to check current graph state.")
					: ErrorMsg,
				TEXT("Call blueprint.read(section:'graph', mode:'full') to see all node IDs and pin names")
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
// Timeline Tool Handler
// ============================================================================

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintCreateTimeline(const TSharedPtr<FJsonObject>& Params)
{
	// ---- 1. Parse required parameters ----
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blueprint asset path (e.g., '/Game/Blueprints/BP_Door')")
		);
	}

	const TArray<TSharedPtr<FJsonValue>>* TracksArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("tracks"), TracksArray) || !TracksArray || TracksArray->Num() == 0)
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_TRACKS"),
			TEXT("Missing or empty 'tracks' array"),
			TEXT("Provide at least one track: {name, type (float/vector/color/event), keys}")
		);
	}

	// ---- 2. Parse optional parameters ----
	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph"), GraphName) || GraphName.IsEmpty())
	{
		GraphName = TEXT("EventGraph");
	}

	FString UserTimelineName;
	Params->TryGetStringField(TEXT("timeline_name"), UserTimelineName);

	double Length = 5.0;
	Params->TryGetNumberField(TEXT("length"), Length);

	bool bAutoPlay = false;
	Params->TryGetBoolField(TEXT("auto_play"), bAutoPlay);

	bool bLoop = false;
	Params->TryGetBoolField(TEXT("loop"), bLoop);

	bool bReplicated = false;
	Params->TryGetBoolField(TEXT("replicated"), bReplicated);

	bool bIgnoreTimeDilation = false;
	Params->TryGetBoolField(TEXT("ignore_time_dilation"), bIgnoreTimeDilation);

	// ---- 3. Parse and validate tracks ----

	// Reserved pin names that conflict with built-in timeline pins
	static const TSet<FString> ReservedPinNames = {
		TEXT("Play"), TEXT("PlayFromStart"), TEXT("Stop"),
		TEXT("Reverse"), TEXT("ReverseFromEnd"),
		TEXT("Update"), TEXT("Finished"),
		TEXT("SetNewTime"), TEXT("NewTime"), TEXT("Direction")
	};

	struct FParsedTrack
	{
		FString Name;
		FString Type; // "float", "vector", "color", "event"
		ERichCurveInterpMode InterpMode;
		TArray<TArray<double>> Keys; // Each inner array is one keyframe
	};

	TArray<FParsedTrack> ParsedTracks;
	TSet<FString> TrackNames;

	for (int32 TrackIdx = 0; TrackIdx < TracksArray->Num(); ++TrackIdx)
	{
		const TSharedPtr<FJsonObject>* TrackObjPtr = nullptr;
		if (!(*TracksArray)[TrackIdx]->TryGetObject(TrackObjPtr) || !TrackObjPtr || !TrackObjPtr->IsValid())
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_INVALID_TRACKS"),
				FString::Printf(TEXT("Track at index %d is not a valid object"), TrackIdx),
				TEXT("Each track needs: {name: string, type: \"float\"|\"vector\"|\"color\"|\"event\", keys: array}")
			);
		}
		const TSharedPtr<FJsonObject>& TrackObj = *TrackObjPtr;

		FParsedTrack Track;

		// Track name
		if (!TrackObj->TryGetStringField(TEXT("name"), Track.Name) || Track.Name.IsEmpty())
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_INVALID_TRACKS"),
				FString::Printf(TEXT("Track at index %d is missing 'name'"), TrackIdx),
				TEXT("Each track needs a 'name' field (becomes the output pin name)")
			);
		}

		// Check for duplicate track names
		if (TrackNames.Contains(Track.Name))
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_DUPLICATE_TRACK_NAME"),
				FString::Printf(TEXT("Duplicate track name '%s' at index %d"), *Track.Name, TrackIdx),
				TEXT("Track names must be unique within a timeline")
			);
		}

		// Check for reserved pin names
		if (ReservedPinNames.Contains(Track.Name))
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_RESERVED_TRACK_NAME"),
				FString::Printf(TEXT("Track name '%s' conflicts with a built-in timeline pin"), *Track.Name),
				TEXT("Avoid these reserved names: Play, PlayFromStart, Stop, Reverse, ReverseFromEnd, Update, Finished, SetNewTime, NewTime, Direction")
			);
		}
		TrackNames.Add(Track.Name);

		// Track type
		if (!TrackObj->TryGetStringField(TEXT("type"), Track.Type) || Track.Type.IsEmpty())
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_INVALID_TRACKS"),
				FString::Printf(TEXT("Track '%s' is missing 'type'"), *Track.Name),
				TEXT("Track type must be one of: float, vector, color, event")
			);
		}
		Track.Type = Track.Type.ToLower();
		if (Track.Type != TEXT("float") && Track.Type != TEXT("vector") &&
			Track.Type != TEXT("color") && Track.Type != TEXT("event"))
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_INVALID_TRACKS"),
				FString::Printf(TEXT("Track '%s' has invalid type '%s'"), *Track.Name, *Track.Type),
				TEXT("Track type must be one of: float, vector, color, event")
			);
		}

		// Interpolation mode (optional, default linear, ignored for event tracks)
		Track.InterpMode = RCIM_Linear;
		FString InterpStr;
		if (TrackObj->TryGetStringField(TEXT("interp"), InterpStr) && Track.Type != TEXT("event"))
		{
			InterpStr = InterpStr.ToLower();
			if (InterpStr == TEXT("cubic"))
			{
				Track.InterpMode = RCIM_Cubic;
			}
			else if (InterpStr == TEXT("constant"))
			{
				Track.InterpMode = RCIM_Constant;
			}
			// "linear" or anything else = default RCIM_Linear
		}

		// Keys
		const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
		if (!TrackObj->TryGetArrayField(TEXT("keys"), KeysArray) || !KeysArray)
		{
			return FOliveToolResult::Error(
				TEXT("VALIDATION_INVALID_TRACKS"),
				FString::Printf(TEXT("Track '%s' is missing 'keys' array"), *Track.Name),
				TEXT("Each track needs a 'keys' array of keyframes")
			);
		}

		// Determine expected key element count per type
		int32 ExpectedElements = 2; // float and event
		if (Track.Type == TEXT("vector"))
		{
			ExpectedElements = 4; // [time, x, y, z]
		}
		else if (Track.Type == TEXT("color"))
		{
			ExpectedElements = 5; // [time, r, g, b, a]
		}

		// Parse each key
		for (int32 KeyIdx = 0; KeyIdx < KeysArray->Num(); ++KeyIdx)
		{
			const TArray<TSharedPtr<FJsonValue>>* KeyElements = nullptr;
			if (!(*KeysArray)[KeyIdx]->TryGetArray(KeyElements) || !KeyElements)
			{
				return FOliveToolResult::Error(
					TEXT("VALIDATION_INVALID_KEY_FORMAT"),
					FString::Printf(TEXT("Track '%s' key at index %d is not an array"), *Track.Name, KeyIdx),
					FString::Printf(TEXT("Keys must be arrays. Float: [time, value]. Vector: [time, x, y, z]. Color: [time, r, g, b, a]. Event: [time, 0]."))
				);
			}

			if (KeyElements->Num() != ExpectedElements)
			{
				return FOliveToolResult::Error(
					TEXT("VALIDATION_INVALID_KEY_FORMAT"),
					FString::Printf(TEXT("Track '%s' key at index %d has %d elements (expected %d for %s)"),
						*Track.Name, KeyIdx, KeyElements->Num(), ExpectedElements, *Track.Type),
					FString::Printf(TEXT("Float keys: [time, value]. Vector keys: [time, x, y, z]. Color keys: [time, r, g, b, a]. Event keys: [time, 0]."))
				);
			}

			TArray<double> KeyValues;
			for (int32 ElemIdx = 0; ElemIdx < KeyElements->Num(); ++ElemIdx)
			{
				double Val = 0.0;
				if (!(*KeyElements)[ElemIdx]->TryGetNumber(Val))
				{
					return FOliveToolResult::Error(
						TEXT("VALIDATION_INVALID_KEY_FORMAT"),
						FString::Printf(TEXT("Track '%s' key[%d][%d] is not a number"), *Track.Name, KeyIdx, ElemIdx),
						TEXT("All key values must be numbers")
					);
				}
				KeyValues.Add(Val);
			}
			Track.Keys.Add(MoveTemp(KeyValues));
		}

		ParsedTracks.Add(MoveTemp(Track));
	}

	// ---- 4. Load Blueprint and pre-pipeline checks ----
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return FOliveToolResult::Error(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
			TEXT("Verify the asset path is correct and the asset exists")
		);
	}

	// Check DoesSupportTimelines
	if (!FBlueprintEditorUtils::DoesSupportTimelines(Blueprint))
	{
		return FOliveToolResult::Error(
			TEXT("TIMELINE_NOT_SUPPORTED"),
			FString::Printf(TEXT("Blueprint '%s' does not support Timelines"), *AssetPath),
			TEXT("Timelines only work in Actor-based Blueprints (not Widget BPs, Component BPs, Interfaces, etc.)")
		);
	}

	// Determine the timeline name
	FName TimelineVarName;
	if (!UserTimelineName.IsEmpty())
	{
		TimelineVarName = FName(*UserTimelineName);
		// Check for duplicate
		if (Blueprint->FindTimelineTemplateByVariableName(TimelineVarName) != nullptr)
		{
			return FOliveToolResult::Error(
				TEXT("TIMELINE_DUPLICATE_NAME"),
				FString::Printf(TEXT("A timeline named '%s' already exists in '%s'"), *UserTimelineName, *AssetPath),
				TEXT("Choose a different name or omit timeline_name for auto-generation")
			);
		}
	}

	// Find the target graph and validate it is a ubergraph
	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* UG : Blueprint->UbergraphPages)
	{
		if (UG && (UG->GetName() == GraphName || (GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase) && UG == Blueprint->UbergraphPages[0])))
		{
			TargetGraph = UG;
			break;
		}
	}
	if (!TargetGraph)
	{
		return FOliveToolResult::Error(
			TEXT("TIMELINE_GRAPH_NOT_FOUND"),
			FString::Printf(TEXT("Event graph '%s' not found in '%s'"), *GraphName, *AssetPath),
			TEXT("Timelines can only be placed in event graphs (UbergraphPages), not function or macro graphs")
		);
	}

	// Ensure GeneratedClass exists (required by AddNewTimeline)
	if (!Blueprint->GeneratedClass)
	{
		UE_LOG(LogOliveBPTools, Log, TEXT("HandleBlueprintCreateTimeline: Blueprint '%s' has no GeneratedClass, compiling first"), *AssetPath);
		FOliveCompileManager& CompileManager = FOliveCompileManager::Get();
		CompileManager.Compile(Blueprint);
		if (!Blueprint->GeneratedClass)
		{
			return FOliveToolResult::Error(
				TEXT("TIMELINE_CREATE_FAILED"),
				FString::Printf(TEXT("Blueprint '%s' has no GeneratedClass and compilation failed"), *AssetPath),
				TEXT("Compile the Blueprint first and retry")
			);
		}
	}

	// ---- 5. Build write request and executor ----
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.create_timeline");
	Request.Params = Params;
	Request.AssetPath = AssetPath;
	Request.TargetAsset = Blueprint;
	Request.OperationDescription = FText::FromString(
		FString::Printf(TEXT("Create timeline in graph '%s' of '%s'"), *GraphName, *AssetPath)
	);
	Request.OperationCategory = TEXT("graph_editing");
	Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
	Request.bAutoCompile = true;
	Request.bSkipVerification = false;

	// Capture parsed data for the lambda
	FOliveWriteExecutor Executor;
	Executor.BindLambda([AssetPath, GraphName, UserTimelineName, Length, bAutoPlay, bLoop, bReplicated, bIgnoreTimeDilation, ParsedTracks](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		UBlueprint* BP = Cast<UBlueprint>(Target);
		if (!BP)
		{
			return FOliveWriteResult::ExecutionError(TEXT("TIMELINE_CREATE_FAILED"), TEXT("Invalid Blueprint target"), TEXT(""));
		}

		// Find the target graph
		UEdGraph* Graph = nullptr;
		for (UEdGraph* UG : BP->UbergraphPages)
		{
			if (UG && (UG->GetName() == GraphName || (GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase) && UG == BP->UbergraphPages[0])))
			{
				Graph = UG;
				break;
			}
		}
		if (!Graph)
		{
			return FOliveWriteResult::ExecutionError(TEXT("TIMELINE_GRAPH_NOT_FOUND"),
				FString::Printf(TEXT("Event graph '%s' not found"), *GraphName),
				TEXT("Timelines can only be placed in event graphs"));
		}

		// Generate timeline name if not provided
		FName TLName;
		if (UserTimelineName.IsEmpty())
		{
			TLName = FBlueprintEditorUtils::FindUniqueTimelineName(BP);
		}
		else
		{
			TLName = FName(*UserTimelineName);
		}

		// Create the UK2Node_Timeline (outer = Graph so GetBlueprint() works)
		UK2Node_Timeline* TimelineNode = NewObject<UK2Node_Timeline>(Graph);
		TimelineNode->TimelineName = TLName;
		TimelineNode->CreateNewGuid();

		// Create the template via the engine utility
		UTimelineTemplate* Template = FBlueprintEditorUtils::AddNewTimeline(BP, TLName);
		if (!Template)
		{
			// Cleanup the node we created
			TimelineNode->MarkAsGarbage();
			return FOliveWriteResult::ExecutionError(TEXT("TIMELINE_CREATE_FAILED"),
				FString::Printf(TEXT("Failed to create timeline template '%s'"), *TLName.ToString()),
				TEXT("Internal error creating timeline template. Compile the Blueprint and retry."));
		}

		// Configure template properties
		Template->Modify();
		Template->TimelineLength = static_cast<float>(Length);
		Template->bAutoPlay = bAutoPlay;
		Template->bLoop = bLoop;
		Template->bReplicated = bReplicated;
		Template->bIgnoreTimeDilation = bIgnoreTimeDilation;

		// Add tracks
		UClass* OwnerClass = BP->GeneratedClass;
		check(OwnerClass);

		TArray<TSharedPtr<FJsonValue>> TracksCreated;
		TArray<FString> Warnings;
		int32 FloatCount = 0, VectorCount = 0, ColorCount = 0, EventCount = 0;

		for (const auto& ParsedTrack : ParsedTracks)
		{
			TSharedPtr<FJsonObject> TrackInfo = MakeShareable(new FJsonObject());
			TrackInfo->SetStringField(TEXT("name"), ParsedTrack.Name);
			TrackInfo->SetStringField(TEXT("type"), ParsedTrack.Type);
			TrackInfo->SetNumberField(TEXT("keys"), ParsedTrack.Keys.Num());
			TrackInfo->SetStringField(TEXT("pin_name"), ParsedTrack.Name);

			if (ParsedTrack.Keys.Num() == 0)
			{
				Warnings.Add(FString::Printf(TEXT("Track '%s' has no keys -- output will be constant 0."), *ParsedTrack.Name));
			}

			if (ParsedTrack.Type == TEXT("float"))
			{
				FTTFloatTrack NewTrack;
				NewTrack.SetTrackName(FName(*ParsedTrack.Name), Template);
				NewTrack.CurveFloat = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);

				for (const TArray<double>& Key : ParsedTrack.Keys)
				{
					FKeyHandle H = NewTrack.CurveFloat->FloatCurve.AddKey(static_cast<float>(Key[0]), static_cast<float>(Key[1]));
					if (ParsedTrack.InterpMode != RCIM_Linear)
					{
						NewTrack.CurveFloat->FloatCurve.SetKeyInterpMode(H, ParsedTrack.InterpMode);
					}
				}

				int32 Idx = Template->FloatTracks.Num();
				Template->FloatTracks.Add(NewTrack);
				FTTTrackId TrackId;
				TrackId.TrackType = FTTTrackBase::TT_FloatInterp;
				TrackId.TrackIndex = Idx;
				Template->AddDisplayTrack(TrackId);
				FloatCount++;
			}
			else if (ParsedTrack.Type == TEXT("event"))
			{
				FTTEventTrack NewTrack;
				NewTrack.SetTrackName(FName(*ParsedTrack.Name), Template);
				NewTrack.CurveKeys = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
				NewTrack.CurveKeys->bIsEventCurve = true;

				for (const TArray<double>& Key : ParsedTrack.Keys)
				{
					NewTrack.CurveKeys->FloatCurve.AddKey(static_cast<float>(Key[0]), 0.0f);
				}

				int32 Idx = Template->EventTracks.Num();
				Template->EventTracks.Add(NewTrack);
				FTTTrackId TrackId;
				TrackId.TrackType = FTTTrackBase::TT_Event;
				TrackId.TrackIndex = Idx;
				Template->AddDisplayTrack(TrackId);
				EventCount++;
			}
			else if (ParsedTrack.Type == TEXT("vector"))
			{
				FTTVectorTrack NewTrack;
				NewTrack.SetTrackName(FName(*ParsedTrack.Name), Template);
				NewTrack.CurveVector = NewObject<UCurveVector>(OwnerClass, NAME_None, RF_Public);

				for (const TArray<double>& Key : ParsedTrack.Keys)
				{
					FKeyHandle Hx = NewTrack.CurveVector->FloatCurves[0].AddKey(static_cast<float>(Key[0]), static_cast<float>(Key[1]));
					FKeyHandle Hy = NewTrack.CurveVector->FloatCurves[1].AddKey(static_cast<float>(Key[0]), static_cast<float>(Key[2]));
					FKeyHandle Hz = NewTrack.CurveVector->FloatCurves[2].AddKey(static_cast<float>(Key[0]), static_cast<float>(Key[3]));
					if (ParsedTrack.InterpMode != RCIM_Linear)
					{
						NewTrack.CurveVector->FloatCurves[0].SetKeyInterpMode(Hx, ParsedTrack.InterpMode);
						NewTrack.CurveVector->FloatCurves[1].SetKeyInterpMode(Hy, ParsedTrack.InterpMode);
						NewTrack.CurveVector->FloatCurves[2].SetKeyInterpMode(Hz, ParsedTrack.InterpMode);
					}
				}

				int32 Idx = Template->VectorTracks.Num();
				Template->VectorTracks.Add(NewTrack);
				FTTTrackId TrackId;
				TrackId.TrackType = FTTTrackBase::TT_VectorInterp;
				TrackId.TrackIndex = Idx;
				Template->AddDisplayTrack(TrackId);
				VectorCount++;
			}
			else if (ParsedTrack.Type == TEXT("color"))
			{
				FTTLinearColorTrack NewTrack;
				NewTrack.SetTrackName(FName(*ParsedTrack.Name), Template);
				NewTrack.CurveLinearColor = NewObject<UCurveLinearColor>(OwnerClass, NAME_None, RF_Public);

				for (const TArray<double>& Key : ParsedTrack.Keys)
				{
					FKeyHandle Hr = NewTrack.CurveLinearColor->FloatCurves[0].AddKey(static_cast<float>(Key[0]), static_cast<float>(Key[1]));
					FKeyHandle Hg = NewTrack.CurveLinearColor->FloatCurves[1].AddKey(static_cast<float>(Key[0]), static_cast<float>(Key[2]));
					FKeyHandle Hb = NewTrack.CurveLinearColor->FloatCurves[2].AddKey(static_cast<float>(Key[0]), static_cast<float>(Key[3]));
					FKeyHandle Ha = NewTrack.CurveLinearColor->FloatCurves[3].AddKey(static_cast<float>(Key[0]), static_cast<float>(Key[4]));
					if (ParsedTrack.InterpMode != RCIM_Linear)
					{
						NewTrack.CurveLinearColor->FloatCurves[0].SetKeyInterpMode(Hr, ParsedTrack.InterpMode);
						NewTrack.CurveLinearColor->FloatCurves[1].SetKeyInterpMode(Hg, ParsedTrack.InterpMode);
						NewTrack.CurveLinearColor->FloatCurves[2].SetKeyInterpMode(Hb, ParsedTrack.InterpMode);
						NewTrack.CurveLinearColor->FloatCurves[3].SetKeyInterpMode(Ha, ParsedTrack.InterpMode);
					}
				}

				int32 Idx = Template->LinearColorTracks.Num();
				Template->LinearColorTracks.Add(NewTrack);
				FTTTrackId TrackId;
				TrackId.TrackType = FTTTrackBase::TT_LinearColorInterp;
				TrackId.TrackIndex = Idx;
				Template->AddDisplayTrack(TrackId);
				ColorCount++;
			}

			TracksCreated.Add(MakeShareable(new FJsonValueObject(TrackInfo)));
		}

		// Allocate pins (reads template to create track output pins)
		TimelineNode->AllocateDefaultPins();

		// Add node to graph
		Graph->AddNode(TimelineNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

		// Position the node
		TimelineNode->NodePosX = 0;
		TimelineNode->NodePosY = 0;

		// Cache node in GraphWriter for subsequent connect_pins/plan_json calls
		FOliveGraphWriter& GW = FOliveGraphWriter::Get();
		FString NodeId = GW.CacheExternalNode(AssetPath, TimelineNode);

		// Build result data
		TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
		ResultData->SetStringField(TEXT("asset_path"), AssetPath);
		ResultData->SetStringField(TEXT("graph"), GraphName);
		ResultData->SetStringField(TEXT("node_id"), NodeId);
		ResultData->SetStringField(TEXT("timeline_name"), TLName.ToString());

		// Build summary message
		TArray<FString> TypeCounts;
		if (FloatCount > 0) TypeCounts.Add(FString::Printf(TEXT("%d float"), FloatCount));
		if (VectorCount > 0) TypeCounts.Add(FString::Printf(TEXT("%d vector"), VectorCount));
		if (ColorCount > 0) TypeCounts.Add(FString::Printf(TEXT("%d color"), ColorCount));
		if (EventCount > 0) TypeCounts.Add(FString::Printf(TEXT("%d event"), EventCount));
		FString TypeSummary = FString::Join(TypeCounts, TEXT(", "));
		ResultData->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Created timeline '%s' with %d tracks (%s)"),
				*TLName.ToString(), ParsedTracks.Num(), *TypeSummary));

		// Tracks created
		ResultData->SetArrayField(TEXT("tracks_created"), TracksCreated);

		// Template properties
		TSharedPtr<FJsonObject> TemplateProps = MakeShareable(new FJsonObject());
		TemplateProps->SetNumberField(TEXT("length"), Length);
		TemplateProps->SetBoolField(TEXT("auto_play"), bAutoPlay);
		TemplateProps->SetBoolField(TEXT("loop"), bLoop);
		TemplateProps->SetBoolField(TEXT("replicated"), bReplicated);
		TemplateProps->SetBoolField(TEXT("ignore_time_dilation"), bIgnoreTimeDilation);
		ResultData->SetObjectField(TEXT("template_properties"), TemplateProps);

		// Pin manifest (using the existing helper)
		ResultData->SetObjectField(TEXT("pins"), BuildPinManifest(TimelineNode));

		// Add warnings
		for (const FString& W : Warnings)
		{
			if (!ResultData->HasField(TEXT("warnings")))
			{
				ResultData->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
			}
		}
		if (Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarningsArray;
			for (const FString& W : Warnings)
			{
				WarningsArray.Add(MakeShareable(new FJsonValueString(W)));
			}
			ResultData->SetArrayField(TEXT("warnings"), WarningsArray);
		}

		FOliveWriteResult Result = FOliveWriteResult::Success(ResultData);
		Result.CreatedNodeIds.Add(NodeId);
		return Result;
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
			FString ErrorMsg = WriteResult.GetFirstError();
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
			FString ErrorMsg = WriteResult.GetFirstError();
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

	bool bIsVariable = true; // Default true — agent creates widgets to use them in logic
	if (Params->HasField(TEXT("is_variable")))
	{
		bIsVariable = Params->GetBoolField(TEXT("is_variable"));
	}

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
	Executor.BindLambda([WidgetClass, WidgetName, ParentWidget, SlotType, bIsVariable](const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
	{
		FOliveWidgetWriter& Writer = FOliveWidgetWriter::Get();
		FOliveBlueprintWriteResult WriteResult = Writer.AddWidget(
			Req.AssetPath,
			WidgetClass,
			WidgetName,
			ParentWidget,
			SlotType,
			bIsVariable);

		if (!WriteResult.bSuccess)
		{
			return FOliveWriteResult::ExecutionError(
				TEXT("WIDGET_ADD_FAILED"),
				WriteResult.GetFirstError(),
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
				WriteResult.GetFirstError(),
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
				WriteResult.GetFirstError(),
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
	Request.bAutoCompile = true;

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
				WriteResult.GetFirstError(),
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
	{
		FOliveToolDefinition Def;
		Def.Name = TEXT("blueprint.apply_plan_json");
		Def.Description = TEXT("Apply an intent-level Blueprint plan atomically. Resolves intents to nodes, executes in a single transaction, compiles once.");
		Def.InputSchema = OliveBlueprintSchemas::BlueprintApplyPlanJson();
		Def.Tags = {TEXT("blueprint"), TEXT("write"), TEXT("graph"), TEXT("plan")};
		Def.Category = TEXT("blueprint");
		Def.WhenToUse = TEXT("Primary tool for Blueprint graph logic. Handles 3+ node patterns in one call. Auto-resolves function names — no need for describe_node_type or describe_function first. Preview with blueprint.preview_plan_json before applying. Do NOT batch preview and apply in the same response.");
		Registry.RegisterTool(Def, FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintApplyPlanJson));
	}
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

	// Search interface implementation graphs (ImplementedInterfaces[i].Graphs).
	// These are NOT in FunctionGraphs — UE stores them separately.
	for (FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		for (UEdGraph* Graph : InterfaceDesc.Graphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
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

	// Interface function graph race condition guard:
	// When add_interface is called, ImplementNewInterface creates stub graphs in
	// ImplementedInterfaces[i].Graphs, but they may not be materialized yet.
	// If this graph name matches an interface function, force conformance and retry.
	{
		bool bIsInterfaceFunction = false;
		for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			if (InterfaceDesc.Interface)
			{
				for (TFieldIterator<UFunction> FuncIt(InterfaceDesc.Interface); FuncIt; ++FuncIt)
				{
					if ((*FuncIt)->GetFName() == FName(*GraphName))
					{
						bIsInterfaceFunction = true;
						break;
					}
				}
			}
			if (bIsInterfaceFunction) break;
		}

		if (bIsInterfaceFunction)
		{
			UE_LOG(LogOliveBPTools, Log,
				TEXT("'%s' matches interface function — forcing ConformImplementedInterfaces on '%s'"),
				*GraphName, *Blueprint->GetName());
			FBlueprintEditorUtils::ConformImplementedInterfaces(Blueprint);

			UEdGraph* ConformedGraph = FindGraphByName(Blueprint, GraphName);
			if (ConformedGraph)
			{
				UE_LOG(LogOliveBPTools, Log,
					TEXT("ConformImplementedInterfaces materialized graph '%s' successfully"),
					*GraphName);
				return ConformedGraph;
			}
			UE_LOG(LogOliveBPTools, Warning,
				TEXT("ConformImplementedInterfaces did not materialize graph '%s' — falling through to creation"),
				*GraphName);
		}
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

		// Build contextual next-step guidance based on the first error
		if (ResolveResult.Errors.Num() > 0)
		{
			const FOliveIRBlueprintPlanError& FirstErr = ResolveResult.Errors[0];
			if (FirstErr.ErrorCode == TEXT("FUNCTION_NOT_FOUND"))
			{
				Result.NextStepGuidance = FString::Printf(
					TEXT("Function not found for step '%s'. Use blueprint.describe_function to check "
						 "available functions on the target class, or check spelling."),
					*FirstErr.StepId);
			}
			else if (FirstErr.ErrorCode == TEXT("VARIABLE_NOT_FOUND"))
			{
				Result.NextStepGuidance = FString::Printf(
					TEXT("Variable not found for step '%s'. Use blueprint.read with section='variables' "
						 "to see available variables, or add the variable first with blueprint.add_variable."),
					*FirstErr.StepId);
			}
			else
			{
				Result.NextStepGuidance = TEXT("Check the errors array for details. Use blueprint.describe_function "
					"or blueprint.read to verify names and types before retrying.");
			}
		}

		return Result;
	}

	// Use the expanded plan (with all resolver expansions applied) for all
	// post-resolve processing. The original Plan must NOT be used after this
	// point because it lacks synthetic steps from ExpandComponentRefs/etc.
	FOliveIRBlueprintPlan& ExpandedPlan = ResolveResult.ExpandedPlan;

	// Infer missing exec_after from step order (fixes layout for plans without explicit exec flow)
	FOliveBlueprintPlanResolver::InferMissingExecChain(
		ExpandedPlan, ResolveResult.ResolvedSteps, ResolveResult.GlobalNotes);

	// Auto-inject SetCollisionEnabled(NoCollision) after attach calls when mesh collision is unhandled
	FOliveBlueprintPlanResolver::ExpandMissingCollisionDisable(
		ExpandedPlan, ResolveResult.ResolvedSteps, Blueprint, ResolveResult.GlobalNotes);

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

		// Surface Phase 0 warnings (e.g. COLLISION_ON_TRIGGER_COMPONENT) so the AI can self-correct
		for (const FString& W : Phase0Result.Warnings)
		{
			ResolveResult.Warnings.Add(W);
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
	// 5. Check preview requirement / auto-preview
	// ------------------------------------------------------------------
	// When no fingerprint is provided and bPlanJsonRequirePreviewForApply is
	// true, we run an implicit "auto-preview" — the same resolve + validate
	// pipeline that preview_plan_json uses. This saves the AI a round-trip
	// (no need to call preview_plan_json first). The result is tagged with
	// "auto_preview": true so the caller knows preview was inlined.
	const bool bRequirePreview = Settings && Settings->bPlanJsonRequirePreviewForApply;
	const bool bAutoPreview = ProvidedFingerprint.IsEmpty() && bRequirePreview;

	if (ProvidedFingerprint.IsEmpty())
	{
		if (bAutoPreview)
		{
			UE_LOG(LogOliveBPTools, Log,
				TEXT("apply_plan_json: no preview_fingerprint provided — running automatic preview "
					 "(bPlanJsonRequirePreviewForApply=true). Resolve+validate will run inline."));
		}
		else
		{
			UE_LOG(LogOliveBPTools, Log,
				TEXT("apply_plan_json: no preview_fingerprint provided — skipping drift check, "
					 "proceeding with resolve+execute (bPlanJsonRequirePreviewForApply=false)."));
		}
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

		// Build contextual next-step guidance based on the first error
		if (ResolveResult.Errors.Num() > 0)
		{
			const FOliveIRBlueprintPlanError& FirstErr = ResolveResult.Errors[0];
			if (FirstErr.ErrorCode == TEXT("FUNCTION_NOT_FOUND"))
			{
				Result.NextStepGuidance = FString::Printf(
					TEXT("Function not found for step '%s'. Use blueprint.describe_function to check "
						 "available functions on the target class, or check spelling."),
					*FirstErr.StepId);
			}
			else if (FirstErr.ErrorCode == TEXT("VARIABLE_NOT_FOUND"))
			{
				Result.NextStepGuidance = FString::Printf(
					TEXT("Variable not found for step '%s'. Use blueprint.read with section='variables' "
						 "to see available variables, or add the variable first with blueprint.add_variable."),
					*FirstErr.StepId);
			}
			else
			{
				Result.NextStepGuidance = TEXT("Check the errors array for details. Use blueprint.describe_function "
					"or blueprint.read to verify names and types before retrying.");
			}
		}

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

	// Infer missing exec_after from step order (fixes layout for plans without explicit exec flow)
	FOliveBlueprintPlanResolver::InferMissingExecChain(
		ExpandedPlan, ResolveResult.ResolvedSteps, ResolveResult.GlobalNotes);

	// Auto-inject SetCollisionEnabled(NoCollision) after attach calls when mesh collision is unhandled
	FOliveBlueprintPlanResolver::ExpandMissingCollisionDisable(
		ExpandedPlan, ResolveResult.ResolvedSteps, Blueprint, ResolveResult.GlobalNotes);

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

		// Surface Phase 0 warnings (e.g. COLLISION_ON_TRIGGER_COMPONENT) so the AI can self-correct
		for (const FString& W : Phase0Result.Warnings)
		{
			ResolveResult.Warnings.Add(W);
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
					// Check if target is an interface function to provide better guidance
					bool bIsIfaceFunc = false;
					for (const FBPInterfaceDescription& Desc : BP->ImplementedInterfaces)
					{
						if (Desc.Interface)
						{
							for (TFieldIterator<UFunction> It(Desc.Interface); It; ++It)
							{
								if ((*It)->GetFName() == FName(*GraphTarget))
								{
									bIsIfaceFunc = true;
									break;
								}
							}
						}
						if (bIsIfaceFunc) break;
					}

					if (bIsIfaceFunc)
					{
						return FOliveWriteResult::ExecutionError(
							TEXT("INTERFACE_GRAPH_NOT_READY"),
							FString::Printf(TEXT("Interface function graph '%s' exists as an interface function but the graph has not materialized after conformance."), *GraphTarget),
							FString::Printf(TEXT("Try calling blueprint.compile on this Blueprint first, then retry apply_plan_json targeting graph '%s'."), *GraphTarget));
					}

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

					// Enrich with first failed step context for AI self-correction
					const FOliveIRBlueprintPlanError& FirstErr = PlanResult.Errors[0];
					if (!FirstErr.StepId.IsEmpty())
					{
						ResultData->SetStringField(TEXT("failed_step_id"), FirstErr.StepId);
						// Find the step's op type from the plan
						for (const FOliveIRBlueprintPlanStep& PStep : CapturedPlan.Steps)
						{
							if (PStep.StepId == FirstErr.StepId)
							{
								ResultData->SetStringField(TEXT("failed_step_op"), PStep.Op);
								break;
							}
						}
					}
				}

				// Split warnings into regular warnings and design warnings
				TArray<TSharedPtr<FJsonValue>> WarningsArr;
				TArray<TSharedPtr<FJsonValue>> DesignWarningsArr;

				for (const FString& Warn : PlanResult.Warnings)
				{
					if (Warn.StartsWith(TEXT("INTERFACE_FUNCTION_HINT:")))
					{
						DesignWarningsArr.Add(MakeShared<FJsonValueString>(Warn));
					}
					else
					{
						WarningsArr.Add(MakeShared<FJsonValueString>(Warn));
					}
				}

				if (WarningsArr.Num() > 0)
				{
					ResultData->SetArrayField(TEXT("warnings"), WarningsArr);
				}
				if (DesignWarningsArr.Num() > 0)
				{
					ResultData->SetArrayField(TEXT("design_warnings"), DesignWarningsArr);
					ResultData->SetBoolField(TEXT("has_design_warnings"), true);
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

				// Self-correction hint when any wiring errors occurred (partial success OR full rollback).
				// Including the hint on rollback is critical — without it the AI enters a debugging
				// spiral of blueprint.read + get_node_pins calls when the alternatives are already
				// in the wiring_errors array.
				const bool bHasWiringErrors = (PlanResult.Errors.Num() > 0);
				if (bHasWiringErrors)
				{
					ResultData->SetStringField(TEXT("self_correction_hint"),
						TEXT("Some data connections failed. Check the 'wiring_errors' array — each entry "
							 "includes 'alternatives' listing the correct pin names. Fix the pin names in "
							 "your plan_json and retry. Do NOT use blueprint.read or blueprint.get_node_pins "
							 "to debug — the alternatives are already provided."));
				}

				if (!PlanResult.bSuccess)
				{
					// Remove stale step_to_node_map -- these IDs are invalid after rollback
					ResultData->RemoveField(TEXT("step_to_node_map"));
					ResultData->SetStringField(TEXT("rollback_warning"),
						TEXT("All nodes were rolled back. Node IDs are INVALID. Call blueprint.read to get current IDs."));

					// Derive a specific, actionable top-level error code/message from the first
					// structured plan error when available. The generic rollback notification is
					// kept as a suffix so the AI still knows state was rolled back, but the prefix
					// tells it exactly what went wrong so the self-correction loop can act without
					// guessing. Falling back to PLAN_EXECUTION_FAILED only if there are no
					// structured errors preserves the legacy behavior for edge cases.
					FString TopErrorCode = TEXT("PLAN_EXECUTION_FAILED");
					FString TopErrorMessage;
					FString TopSuggestion;
					if (PlanResult.Errors.Num() > 0)
					{
						const FOliveIRBlueprintPlanError& FirstErrForTop = PlanResult.Errors[0];
						if (!FirstErrForTop.ErrorCode.IsEmpty())
						{
							TopErrorCode = FirstErrForTop.ErrorCode;
						}
						TopErrorMessage = FirstErrForTop.Message;
						TopSuggestion = FirstErrForTop.Suggestion;
					}

					const FString RollbackSuffix = FString::Printf(
						TEXT(" (Plan rolled back: %d of %d nodes created. Node IDs from step_to_node_map are INVALID. "
							 "Call blueprint.read for current IDs before referencing any nodes.)"),
						static_cast<int32>(PlanResult.StepToNodeMap.Num()),
						CapturedPlan.Steps.Num());

					FString ErrorMsg;
					if (!TopErrorMessage.IsEmpty())
					{
						ErrorMsg = TopErrorMessage + RollbackSuffix;
					}
					else
					{
						ErrorMsg = FString::Printf(
							TEXT("Plan execution failed: %d of %d nodes created. "
								 "IMPORTANT: All nodes from this plan_json call have been ROLLED BACK and removed from the graph. "
								 "Node IDs from step_to_node_map are NO LONGER VALID. "
								 "Call blueprint.read on the target function/graph to get current node IDs before "
								 "referencing any nodes."),
							static_cast<int32>(PlanResult.StepToNodeMap.Num()),
							CapturedPlan.Steps.Num());
					}

					// Mirror the top-level error fields onto the handler's rich ResultData
					// BEFORE we assign it to ErrorResult, so StageExecute's extraction at
					// OliveWritePipeline.cpp:190 picks them up. Without this mirror, the
					// assignment at the end of this block overwrites the fresh ResultData
					// that ExecutionError() created and loses the top-level error fields.
					ResultData->SetStringField(TEXT("error_code"), TopErrorCode);
					ResultData->SetStringField(TEXT("error_message"), ErrorMsg);
					if (!TopSuggestion.IsEmpty())
					{
						ResultData->SetStringField(TEXT("suggestion"), TopSuggestion);
					}

					FOliveWriteResult ErrorResult = FOliveWriteResult::ExecutionError(
						TopErrorCode,
						ErrorMsg,
						TopSuggestion);
					ErrorResult.ResultData = ResultData;

					// Build contextual next-step guidance from the first error
					if (PlanResult.Errors.Num() > 0)
					{
						const FOliveIRBlueprintPlanError& FirstErr = PlanResult.Errors[0];
						if (FirstErr.ErrorCode == TEXT("NODE_CREATION_FAILED"))
						{
							ErrorResult.NextStepGuidance = FString::Printf(
								TEXT("Node creation failed for step '%s'. Use blueprint.describe_node_type or "
									 "blueprint.describe_function to verify the node type/function exists, then retry with corrected plan_json."),
								*FirstErr.StepId);
						}
						else if (FirstErr.ErrorCode.Contains(TEXT("PIN_NOT_FOUND")))
						{
							ErrorResult.NextStepGuidance = FString::Printf(
								TEXT("Pin not found on step '%s' (see: %s). Use blueprint.describe_function to see available pins, "
									 "or use blueprint.read on the graph to see actual node pins after creation."),
								*FirstErr.StepId, *FirstErr.LocationPointer);
						}
						else
						{
							ErrorResult.NextStepGuidance = TEXT("Check wiring_errors for details. Use blueprint.read on the target graph "
								"to see current state, then retry with corrected plan_json.");
						}
					}

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

					if (TSharedPtr<FJsonObject> StateJson = BuildCompactStateJson(BP))
					{
						ResultData->SetObjectField(TEXT("blueprint_state"), StateJson);
					}

					FOliveWriteResult PartialResult = FOliveWriteResult::Success(ResultData);

					// Provide created node IDs for the pipeline's verification stage
					TArray<FString> CreatedNodeIds;
					CreatedNodeIds.Reserve(PlanResult.StepToNodeMap.Num());
					for (const auto& Pair : PlanResult.StepToNodeMap)
					{
						CreatedNodeIds.Add(Pair.Value);
					}
					PartialResult.CreatedNodeIds = MoveTemp(CreatedNodeIds);

					// Build contextual next-step guidance for partial success
					PartialResult.NextStepGuidance = FString::Printf(
						TEXT("%d connection(s) failed. Read wiring_errors for details, then use "
							 "blueprint.connect_pins or blueprint.set_pin_default to fix. "
							 "Nodes are committed with valid IDs in step_to_node_map."),
						TotalFailures);

					return PartialResult;
				}

				// Full success path (no partial failures)
				ResultData->SetStringField(TEXT("message"),
					FString::Printf(TEXT("Plan applied successfully: %d nodes created, %d connections wired"),
						PlanResult.StepToNodeMap.Num(),
						PlanResult.ConnectionsSucceeded));

				if (TSharedPtr<FJsonObject> StateJson = BuildCompactStateJson(BP))
				{
					ResultData->SetObjectField(TEXT("blueprint_state"), StateJson);
				}

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
					// Check if target is an interface function to provide better guidance
					bool bIsIfaceFunc = false;
					for (const FBPInterfaceDescription& Desc : BP->ImplementedInterfaces)
					{
						if (Desc.Interface)
						{
							for (TFieldIterator<UFunction> It(Desc.Interface); It; ++It)
							{
								if ((*It)->GetFName() == FName(*GraphTarget))
								{
									bIsIfaceFunc = true;
									break;
								}
							}
						}
						if (bIsIfaceFunc) break;
					}

					if (bIsIfaceFunc)
					{
						return FOliveWriteResult::ExecutionError(
							TEXT("INTERFACE_GRAPH_NOT_READY"),
							FString::Printf(TEXT("Interface function graph '%s' exists as an interface function but the graph has not materialized after conformance."), *GraphTarget),
							FString::Printf(TEXT("Try calling blueprint.compile on this Blueprint first, then retry apply_plan_json targeting graph '%s'."), *GraphTarget));
					}

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
						FString ErrorMsg = WriteResult.GetFirstError();
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
	// Propagate ChatMode from execution context (same as ExecuteWithOptionalConfirmation)
	if (const FOliveToolCallContext* Ctx = FOliveToolExecutionContext::Get())
	{
		Request.ChatMode = Ctx->ChatMode;
	}

	FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
	FOliveWriteResult PipelineResult = Pipeline.Execute(Request, Executor);

	// ------------------------------------------------------------------
	// 11b. Post-transaction orphaned pin cleanup on failure (secondary defense)
	// ------------------------------------------------------------------
	// The PRIMARY fix for orphaned pins is EnsurePinNotOrphaned() in
	// OlivePlanExecutor.cpp, which clears bOrphanedPin at the point of use
	// (WireExecConnection, WireDataConnection, auto-chain paths).
	//
	// This block is a SECONDARY defense: after the pipeline cancels the
	// transaction (rollback), reused event nodes may still have stale
	// bOrphanedPin flags.  We clear them here so subsequent plan_json
	// calls start with clean pins.
	if (!PipelineResult.bSuccess && bIsV2Plan)
	{
		UEdGraph* CleanupGraph = FindGraphByName(Blueprint, GraphTarget);
		if (CleanupGraph)
		{
			for (UEdGraphNode* Node : CleanupGraph->Nodes)
			{
				if (!Node) continue;
				// Only clean event-like nodes (the ones that get reused across plan retries)
				if (!Node->IsA<UK2Node_Event>()
					&& !Node->IsA<UK2Node_FunctionEntry>()
					&& !Node->IsA<UK2Node_FunctionResult>())
				{
					continue;
				}
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->bOrphanedPin)
					{
						Pin->bOrphanedPin = false;
						UE_LOG(LogOliveBPTools, Log,
							TEXT("Post-rollback cleanup: cleared bOrphanedPin on '%s' pin '%s'"),
							*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
							*Pin->PinName.ToString());
					}
				}
			}
		}
	}

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

			const TArray<TSharedPtr<FJsonValue>>* DesignWarnings = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("design_warnings"), DesignWarnings))
			{
				ToolResult.Data->SetArrayField(TEXT("design_warnings"), *DesignWarnings);
				ToolResult.Data->SetBoolField(TEXT("has_design_warnings"), true);
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

			const TArray<TSharedPtr<FJsonValue>>* DesignWarnings = nullptr;
			if (PipelineResult.ResultData->TryGetArrayField(TEXT("design_warnings"), DesignWarnings))
			{
				ToolResult.Data->SetArrayField(TEXT("design_warnings"), *DesignWarnings);
				ToolResult.Data->SetBoolField(TEXT("has_design_warnings"), true);
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

	// ------------------------------------------------------------------
	// 13. Tag auto-preview in result (so caller knows preview was inlined)
	// ------------------------------------------------------------------
	if (bAutoPreview && ToolResult.Data.IsValid())
	{
		ToolResult.Data->SetBoolField(TEXT("auto_preview"), true);
	}

	UE_LOG(LogOliveBPTools, Log,
		TEXT("Plan apply for '%s' graph '%s': success=%s, schema_version=%s, auto_preview=%s"),
		*AssetPath, *GraphTarget, PipelineResult.bSuccess ? TEXT("true") : TEXT("false"),
		*Plan.SchemaVersion, bAutoPreview ? TEXT("true") : TEXT("false"));

	return ToolResult;
}

// ============================================================
// Template Tools
// ============================================================

void FOliveBlueprintToolHandlers::RegisterTemplateTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	{
		FOliveToolDefinition Def;
		Def.Name = TEXT("blueprint.get_template");
		Def.Description = TEXT("View a template's content. Without pattern: shows structure overview (components, variables, function signatures). "
			"With pattern=FunctionName: returns the function's full graph or plan as reference for building your own.");
		Def.InputSchema = OliveBlueprintSchemas::BlueprintGetTemplate();
		Def.Tags = {TEXT("blueprint"), TEXT("read"), TEXT("template")};
		Def.Category = TEXT("blueprint");
		Def.WhenToUse = TEXT("Read a template's content. Use the pattern parameter to read a specific function's node graph as reference for building. Omit pattern for structure overview.");
		Registry.RegisterTool(Def, FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintGetTemplate));
	}
	RegisteredToolNames.Add(TEXT("blueprint.get_template"));

	{
		FOliveToolDefinition Def;
		Def.Name = TEXT("blueprint.list_templates");
		Def.Description = TEXT("List available templates. Use query parameter to search by name, tag, function name, "
			"or keyword across all templates including library templates from extracted projects.");
		Def.InputSchema = OliveBlueprintSchemas::BlueprintListTemplates();
		Def.Tags = {TEXT("blueprint"), TEXT("read"), TEXT("template")};
		Def.Category = TEXT("blueprint");
		Def.WhenToUse = TEXT("Returns digests with each result. For library templates with relevant functions, call get_template(id, pattern=\"FuncName\") to read the full node graph as reference before building.");
		Registry.RegisterTool(Def, FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintListTemplates));
	}
	RegisteredToolNames.Add(TEXT("blueprint.list_templates"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered template tools (get_template, list_templates)"));
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintListTemplates(const TSharedPtr<FJsonObject>& Params)
{
	FString TypeFilter;
	FString Query;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("type"), TypeFilter);
		Params->TryGetStringField(TEXT("query"), Query);
	}

	// Trim whitespace-only queries so they don't trigger the search path
	Query.TrimStartAndEndInline();

	// If a search query is provided, delegate to SearchTemplates which covers
	// both reference templates and library templates from extracted projects.
	if (!Query.IsEmpty())
	{
		TArray<TSharedPtr<FJsonObject>> SearchResults = FOliveTemplateSystem::Get().SearchTemplates(Query);

		// Post-filter by type if a type filter was also provided.
		// Search results carry a "type" field ("library" or "reference").
		if (!TypeFilter.IsEmpty())
		{
			SearchResults.RemoveAll([&TypeFilter](const TSharedPtr<FJsonObject>& Entry)
			{
				FString EntryType;
				Entry->TryGetStringField(TEXT("type"), EntryType);
				return !EntryType.Equals(TypeFilter, ESearchCase::IgnoreCase);
			});
		}

		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ResultsArray;
		for (const TSharedPtr<FJsonObject>& Entry : SearchResults)
		{
			ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
		}

		ResultData->SetArrayField(TEXT("results"), ResultsArray);
		ResultData->SetNumberField(TEXT("count"), ResultsArray.Num());
		ResultData->SetStringField(TEXT("query"), Query);
		if (!TypeFilter.IsEmpty())
		{
			ResultData->SetStringField(TEXT("type_filter"), TypeFilter);
		}
		ResultData->SetStringField(TEXT("note"),
			TEXT("Use blueprint.get_template(template_id) to view structure. "
				"Use pattern param to read a specific function's graph as reference."));

		return FOliveToolResult::Success(ResultData);
	}

	// No query — list templates, optionally filtered by type.
	const bool bWantsLibrary = TypeFilter.Equals(TEXT("library"), ESearchCase::IgnoreCase);
	const bool bHasNonLibraryFilter = !TypeFilter.IsEmpty() && !bWantsLibrary;

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> TemplatesArray;

	// Include reference templates (unless filtering to library only)
	if (!bWantsLibrary)
	{
		const auto& AllTemplates = FOliveTemplateSystem::Get().GetAllTemplates();
		for (const auto& Pair : AllTemplates)
		{
			const FOliveTemplateInfo& Info = Pair.Value;

			if (bHasNonLibraryFilter && !Info.TemplateType.Equals(TypeFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("template_id"), Info.TemplateId);
			Entry->SetStringField(TEXT("type"), Info.TemplateType);
			Entry->SetStringField(TEXT("display_name"), Info.DisplayName);
			Entry->SetStringField(TEXT("catalog_description"), Info.CatalogDescription);
			if (!Info.CatalogExamples.IsEmpty())
			{
				Entry->SetStringField(TEXT("examples"), Info.CatalogExamples);
			}
			TemplatesArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	// Include library template summary (unless filtering to non-library type)
	if (!bHasNonLibraryFilter)
	{
		const FOliveLibraryIndex& LibIndex = FOliveTemplateSystem::Get().GetLibraryIndex();
		if (LibIndex.Num() > 0)
		{
			// Group by source project for compact summary
			TMap<FString, int32> ProjectCounts;
			TMap<FString, int32> ProjectFuncCounts;
			for (const auto& Pair : LibIndex.GetAllTemplates())
			{
				const FString& Proj = Pair.Value.SourceProject.IsEmpty()
					? TEXT("unknown") : Pair.Value.SourceProject;
				ProjectCounts.FindOrAdd(Proj)++;
				ProjectFuncCounts.FindOrAdd(Proj) += Pair.Value.Functions.Num();
			}

			for (const auto& Pair : ProjectCounts)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("type"), TEXT("library"));
				Entry->SetStringField(TEXT("source_project"), Pair.Key);
				Entry->SetNumberField(TEXT("template_count"), Pair.Value);
				Entry->SetNumberField(TEXT("function_count"),
					ProjectFuncCounts.FindRef(Pair.Key));
				Entry->SetStringField(TEXT("note"),
					TEXT("Use query parameter to search library templates by name, tag, or function."));
				TemplatesArray.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	ResultData->SetArrayField(TEXT("templates"), TemplatesArray);
	ResultData->SetNumberField(TEXT("count"), TemplatesArray.Num());
	ResultData->SetStringField(TEXT("note"),
		TEXT("Use query parameter to search across all templates including library. "
			"Use blueprint.get_template(template_id) to inspect a template."));

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

	// Sentinel prefix used by GetFunctionContent to signal "function not found"
	const FString& FUNC_NOT_FOUND_SENTINEL = FOliveLibraryIndex::GetFuncNotFoundSentinel();

	// Check library index — but skip if the template exists as a reference template
	// (reference templates need the richer pattern-formatted output from GetTemplateContent).
	const FOliveLibraryIndex& LibIndex = FOliveTemplateSystem::Get().GetLibraryIndex();
	const FOliveLibraryTemplateInfo* LibInfo = LibIndex.FindTemplate(TemplateId);
	const bool bExistsAsReference = (FOliveTemplateSystem::Get().FindTemplate(TemplateId) != nullptr);

	if (LibInfo && !bExistsAsReference)
	{
		FString Content;
		if (PatternName.IsEmpty())
		{
			Content = LibIndex.GetTemplateOverview(TemplateId);
		}
		else
		{
			Content = LibIndex.GetFunctionContent(TemplateId, PatternName);
		}

		if (Content.IsEmpty())
		{
			return FOliveToolResult::Error(
				TEXT("TEMPLATE_CONTENT_EMPTY"),
				FString::Printf(TEXT("Library template '%s' found but content retrieval failed%s"),
					*TemplateId,
					PatternName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" for function '%s'"), *PatternName)),
				PatternName.IsEmpty()
					? TEXT("The template file may be corrupted. Try blueprint.list_templates with a query to find alternatives.")
					: TEXT("Check the function name. Use blueprint.get_template without pattern to see available functions.")
			);
		}

		// Check for function-not-found sentinel from GetFunctionContent
		if (Content.StartsWith(FUNC_NOT_FOUND_SENTINEL))
		{
			FString ErrorMsg = Content.Mid(FUNC_NOT_FOUND_SENTINEL.Len());
			return FOliveToolResult::Error(
				TEXT("FUNCTION_NOT_FOUND"),
				ErrorMsg,
				TEXT("Check the function name. Use blueprint.get_template without pattern to see available functions.")
			);
		}

		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		ResultData->SetStringField(TEXT("template_id"), TemplateId);
		ResultData->SetStringField(TEXT("source"), TEXT("library"));
		ResultData->SetStringField(TEXT("content"), Content);

		return FOliveToolResult::Success(ResultData);
	}

	// Fall through to factory/reference template lookup.
	FString Content = FOliveTemplateSystem::Get().GetTemplateContent(TemplateId, PatternName);
	if (Content.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("TEMPLATE_NOT_FOUND"),
			FString::Printf(TEXT("Template '%s' not found"), *TemplateId),
			TEXT("Use blueprint.list_templates to see available templates, or blueprint.list_templates with query to search library templates")
		);
	}

	// Check for function-not-found sentinel from GetTemplateContent (factory/reference path)
	if (Content.StartsWith(FUNC_NOT_FOUND_SENTINEL))
	{
		FString ErrorMsg = Content.Mid(FUNC_NOT_FOUND_SENTINEL.Len());
		return FOliveToolResult::Error(
			TEXT("FUNCTION_NOT_FOUND"),
			ErrorMsg,
			TEXT("Check the function name. Use blueprint.get_template without pattern to see available functions.")
		);
	}

	// Add source field for factory/reference templates
	const FOliveTemplateInfo* FactoryRefInfo = FOliveTemplateSystem::Get().FindTemplate(TemplateId);
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("template_id"), TemplateId);
	if (FactoryRefInfo)
	{
		ResultData->SetStringField(TEXT("source"), FactoryRefInfo->TemplateType);
	}
	ResultData->SetStringField(TEXT("content"), Content);

	return FOliveToolResult::Success(ResultData);
}

