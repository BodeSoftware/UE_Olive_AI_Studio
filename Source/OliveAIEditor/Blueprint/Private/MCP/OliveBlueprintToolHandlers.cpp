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
#include "IR/OliveIRSchema.h"
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

/**
 * Resolve a node reference to a UEdGraphNode* within a specific graph.
 *
 * Priority order:
 *   1. GraphWriter cached id (e.g., "node_0") — exact match against per-session cache.
 *   2. Semantic aliases scoped to the graph:
 *        "entry", "function_entry", "functionentry"  -> UK2Node_FunctionEntry
 *        "return", "result", "function_result"       -> UK2Node_FunctionResult
 *        "event:BeginPlay", "event:Tick"              -> UK2Node_Event with that event name
 *   3. Exact GUID string match against Node->NodeGuid.
 *   4. Case-insensitive class-name match (e.g., "K2Node_IfThenElse" in a graph with one branch).
 *
 * Returns nullptr if no resolution path finds a node. The caller should return
 * a NODE_NOT_FOUND error with this function's error string appended.
 */
UEdGraphNode* ResolveNodeRef(const FString& AssetPath, UEdGraph* Graph, const FString& NodeRef)
{
	if (!Graph || NodeRef.IsEmpty())
	{
		return nullptr;
	}

	// 1. Normal cached lookup (node_N) via GraphWriter.
	if (UEdGraphNode* Cached = FOliveGraphWriter::Get().GetCachedNode(AssetPath, NodeRef))
	{
		return Cached;
	}

	const FString Normalized = NormalizeSemanticToken(NodeRef);

	// 2. Semantic aliases scoped to this graph.
	if (Normalized == TEXT("entry") || Normalized == TEXT("functionentry") || Normalized == TEXT("entrypoint"))
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->IsA<UK2Node_FunctionEntry>())
			{
				return Node;
			}
		}
	}
	if (Normalized == TEXT("return") || Normalized == TEXT("result") || Normalized == TEXT("functionresult"))
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->IsA<UK2Node_FunctionResult>())
			{
				return Node;
			}
		}
	}
	if (NodeRef.StartsWith(TEXT("event:")))
	{
		const FString EventName = NodeRef.RightChop(6);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
			{
				if (EventNode->EventReference.GetMemberName().ToString().Equals(EventName, ESearchCase::IgnoreCase))
				{
					return EventNode;
				}
			}
		}
	}

	// 3. GUID match.
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid.ToString().Equals(NodeRef, ESearchCase::IgnoreCase))
		{
			return Node;
		}
	}

	// 4. Class-name match if unique in this graph.
	{
		UEdGraphNode* UniqueMatch = nullptr;
		int32 MatchCount = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->GetClass()->GetName().Equals(NodeRef, ESearchCase::IgnoreCase))
			{
				UniqueMatch = Node;
				MatchCount++;
			}
		}
		if (MatchCount == 1)
		{
			return UniqueMatch;
		}
	}

	return nullptr;
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
	// Old tools (blueprint.read_function, read_event_graph, read_variables,
	// read_components, read_hierarchy, list_overridable_functions) are now
	// aliases in the tool alias map that redirect here with appropriate section.
	Registry.RegisterTool(
		TEXT("blueprint.read"),
		TEXT("Read any aspect of a Blueprint: overview, specific graph, variables, components, hierarchy, or overridable functions"),
		OliveBlueprintSchemas::BlueprintRead(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintRead),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.read"));

	// blueprint.get_node_pins
	Registry.RegisterTool(
		TEXT("blueprint.get_node_pins"),
		TEXT("Get pin manifest for ONE node (after set_node_property). For multiple nodes use blueprint.read(section:'graph', mode:'full')"),
		OliveBlueprintSchemas::BlueprintGetNodePins(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintGetNodePins),
		{TEXT("blueprint"), TEXT("read")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.get_node_pins"));

	// blueprint.describe_node_type
	{
		FOliveToolDefinition Def;
		Def.Name = TEXT("blueprint.describe_node_type");
		Def.Description = TEXT("Describe a Blueprint node type: its pins, properties, and behavior. Use to plan before creating nodes.");
		Def.InputSchema = OliveBlueprintSchemas::BlueprintDescribeNodeType();
		Def.Tags = {TEXT("blueprint"), TEXT("read"), TEXT("discovery")};
		Def.Category = TEXT("blueprint");
		Def.WhenToUse = TEXT("Use to discover pins on an unfamiliar K2Node subclass before calling add_node, so you can wire the right pins in connect_pins afterwards.");
		Registry.RegisterTool(Def, FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleDescribeNodeType));
	}
	RegisteredToolNames.Add(TEXT("blueprint.describe_node_type"));

	// blueprint.describe_function
	Registry.RegisterTool(
		TEXT("blueprint.describe_function"),
		TEXT("Look up a UFunction by name and return its exact pin signature, or fuzzy suggestions on failure"),
		OliveBlueprintSchemas::BlueprintDescribeFunction(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleDescribeFunction),
		{TEXT("blueprint"), TEXT("read"), TEXT("discovery")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.describe_function"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered %d reader tools"), 4);
}

// ============================================================================
// Asset Writer Tool Registration
// ============================================================================

void FOliveBlueprintToolHandlers::RegisterAssetWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// blueprint.create (also handles template creation when template_id is set)
	Registry.RegisterTool(
		TEXT("blueprint.create"),
		TEXT("Create a new empty Blueprint asset at the given path."),
		OliveBlueprintSchemas::BlueprintCreate(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCreate),
		{TEXT("blueprint"), TEXT("write"), TEXT("create"), TEXT("template")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.create"));

	// blueprint.scaffold (composite: create + interfaces + components + variables in one call)
	{
		FOliveToolDefinition Def;
		Def.Name = TEXT("blueprint.scaffold");
		Def.Description = TEXT("Create a Blueprint with components, variables, and interfaces in one call. "
			"Preferred over separate create + add_component + add_variable calls. "
			"Sub-operation failures are collected as warnings, not hard failures.");
		Def.InputSchema = OliveBlueprintSchemas::BlueprintScaffold();
		Def.Tags = {TEXT("blueprint"), TEXT("write"), TEXT("create"), TEXT("component"), TEXT("variable"), TEXT("interface")};
		Def.Category = TEXT("blueprint");
		Def.WhenToUse = TEXT("Preferred over separate blueprint.create + blueprint.add_component + blueprint.add_variable calls. Use this whenever creating a new Blueprint that needs components, variables, or interfaces.");
		Registry.RegisterTool(Def, FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintScaffold));
	}
	RegisteredToolNames.Add(TEXT("blueprint.scaffold"));

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

	// blueprint.create_interface
	Registry.RegisterTool(
		TEXT("blueprint.create_interface"),
		TEXT("Create a new Blueprint Interface (BPI) asset with function signatures. "
			 "Functions without outputs become implementable events. "
			 "Functions with outputs become implementable functions. "
			 "After creation, use blueprint.add_interface to implement it on target BPs."),
		OliveBlueprintSchemas::BlueprintCreateInterface(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCreateInterface),
		{TEXT("blueprint"), TEXT("write"), TEXT("create"), TEXT("interface")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.create_interface"));

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

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 8 asset writer tools"));
}

void FOliveBlueprintToolHandlers::RegisterVariableWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// blueprint.add_variable (upsert: creates if missing, updates if present)
	Registry.RegisterTool(
		TEXT("blueprint.add_variable"),
		TEXT("Add or update a variable (upsert). If the variable already exists, modifies it in-place. "
			"Set modify_only=true to require the variable to already exist."),
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

	// NOTE: blueprint.modify_variable is now an alias that redirects to blueprint.add_variable (upsert).
	// It is no longer registered as a separate tool.

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 2 variable writer tools"));
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

	// blueprint.add_function (unified — routes by function_type param)
	// Old tools (blueprint.add_custom_event, blueprint.add_event_dispatcher,
	// blueprint.override_function) are now aliases in the tool alias map
	// that redirect here with appropriate function_type.
	Registry.RegisterTool(
		TEXT("blueprint.add_function"),
		TEXT("Add a function, custom event, event dispatcher, or override to a Blueprint"),
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

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 3 function writer tools"));
}

void FOliveBlueprintToolHandlers::RegisterGraphWriterTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// blueprint.add_node
	{
		FOliveToolDefinition Def;
		Def.Name = TEXT("blueprint.add_node");
		Def.Description = TEXT("Add a node to a Blueprint graph");
		Def.InputSchema = OliveBlueprintSchemas::BlueprintAddNode();
		Def.Tags = {TEXT("blueprint"), TEXT("write"), TEXT("graph")};
		Def.Category = TEXT("blueprint");
		Def.WhenToUse = TEXT("Primary tool for building graph logic. Add one node at a time (event, CallFunction, Branch, VariableGet/Set, Cast, MakeArray, Timeline, etc.), then wire with connect_pins and set literals with set_pin_default.");
		Registry.RegisterTool(Def, FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintAddNode));
	}
	RegisteredToolNames.Add(TEXT("blueprint.add_node"));

	// blueprint.remove_node
	{
		FOliveToolDefinition Def;
		Def.Name = TEXT("blueprint.remove_node");
		Def.Description = TEXT("Remove a node from a Blueprint graph");
		Def.InputSchema = OliveBlueprintSchemas::BlueprintRemoveNode();
		Def.Tags = {TEXT("blueprint"), TEXT("write"), TEXT("graph")};
		Def.Category = TEXT("blueprint");
		Def.WhenToUse = TEXT("Remove a node by ID. For large rewrites, it is usually faster to recreate the function's graph than to delete nodes one-by-one.");
		Registry.RegisterTool(Def, FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintRemoveNode));
	}
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

	// blueprint.create_timeline
	Registry.RegisterTool(
		TEXT("blueprint.create_timeline"),
		TEXT("Create a Timeline node with tracks and curve data in a Blueprint event graph. Returns node_id and all pin names. Wire outputs with connect_pins (e.g., source: 'node_id.Play', target: 'other.execute'). Exec inputs: Play, PlayFromStart, Stop, Reverse, ReverseFromEnd. Track outputs use the track name as pin name."),
		OliveBlueprintSchemas::BlueprintCreateTimeline(),
		FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCreateTimeline),
		{TEXT("blueprint"), TEXT("write"), TEXT("graph"), TEXT("timeline")},
		TEXT("blueprint")
	);
	RegisteredToolNames.Add(TEXT("blueprint.create_timeline"));

	UE_LOG(LogOliveBPTools, Log, TEXT("Registered 7 graph writer tools"));
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
	else
	{
		UE_LOG(LogOliveBPTools, Warning, TEXT("HandleBlueprintRead: Invalid section '%s' for path='%s'"), *Section, *AssetPath);
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_VALUE"),
			FString::Printf(TEXT("Invalid section '%s'. Must be one of: all, summary, graph, variables, components, hierarchy, overridable_functions"), *Section),
			TEXT("Use section='all' for a full overview, or section='graph' with graph_name to read a specific graph")
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
			TEXT("Try olive.get_recipe('relevant query') for tested wiring patterns."));
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

	// Find the graph first so we can also resolve semantic node refs.
	UEdGraph* TargetGraph = nullptr;
	for (UEdGraph* UG : Blueprint->UbergraphPages)
	{
		if (UG && UG->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			TargetGraph = UG;
			break;
		}
	}
	if (!TargetGraph)
	{
		for (UEdGraph* FG : Blueprint->FunctionGraphs)
		{
			if (FG && FG->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				TargetGraph = FG;
				break;
			}
		}
	}

	// Resolve node_id via cache first, then semantic aliases (entry/return/event:Name/GUID/class).
	UEdGraphNode* Node = ResolveNodeRef(ResolvedPath, TargetGraph, NodeId);
	if (!Node)
	{
		return FOliveToolResult::Error(
			TEXT("NODE_NOT_FOUND"),
			FString::Printf(TEXT("Node '%s' not found in graph '%s'. "
				"Accepted node references: cached 'node_N' ID, 'entry' / 'return' for function entry/result nodes, "
				"'event:EventName' (e.g. 'event:BeginPlay'), a raw NodeGuid, or a unique class name."),
				*NodeId, *GraphName),
			TEXT("Call blueprint.read with section:'graph' to list all nodes with their node_id and class.")
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
				TEXT("This is an interface function. Call blueprint.add_node with type='CallFunction' and target_class=<interface name> to get a UK2Node_Message."));
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
			TEXT("Try olive.get_recipe('relevant query') for tested wiring patterns."));
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
					 "4) Call through the interface with blueprint.add_node type='CallFunction', "
					 "function_name='FunctionName', target_class='%s' — this produces a UK2Node_Message."),
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
					"To set this variable's value at runtime, add a SetVariable node on BeginPlay in the child."),
					*Variable.Name));
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
				 "NEXT STEP: Build its graph with blueprint.add_node + connect_pins + set_pin_default, "
				 "then blueprint.compile. Do NOT add another function until this one has graph logic."),
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
						TEXT(" Note: %s has no graph logic yet (%d empty function(s) total). Wire its graph with blueprint.add_node + connect_pins before adding more functions."),
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
									"Removing it will destroy existing work. Edit the function's existing graph "
									"with blueprint.add_node / remove_node / connect_pins instead of removing and recreating it."),
								*FunctionName, NodeCount),
							TEXT("Edit the function's graph in-place, or pass 'force': true to override this safety check")
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

		// Cache node in GraphWriter for subsequent connect_pins calls
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

