# Phase 2 -- Graph Edit Integrity

**Goal:** Make graph mutations robust against invalid authoring intents.

**Exit criteria:**
- Unknown node type errors include top suggestions.
- Duplicate native events are blocked pre-mutation.
- Large graphs do not force full-detail payloads by default.

---

## Overview of Changes

Six discrete features, ordered by implementation dependency:

| # | Feature | Primary files touched |
|---|---------|----------------------|
| 1 | Node type validation against catalog | `OliveNodeFactory`, `OliveNodeCatalog` |
| 2 | Fuzzy suggestions for unknown types | `OliveNodeCatalog`, `HandleBlueprintAddNode` |
| 3 | Duplicate native-event prevention | `OliveNodeFactory`, `HandleBlueprintAddNode` |
| 4 | Expanded node-removal report payloads | `OliveGraphWriter::RemoveNode`, `HandleBlueprintRemoveNode` |
| 5 | Post-op orphaned exec-flow detection | `OliveWritePipeline::StageVerify` |
| 6 | Large-graph read mode (summary + paging) | `OliveGraphReader`, `OliveBlueprintReader`, `HandleBlueprintRead` |

---

## Task 1 -- Node Type Validation Against Catalog

### What

Before `OliveNodeFactory::CreateNode` attempts creation, validate the requested `NodeType` against the node catalog's known types. Today, an unknown type silently falls through to a nullptr return with a generic "Failed to create node" error. This task makes the failure explicit and structured.

### Files to modify

- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` -- `CreateNode` method
- `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h` -- add `ValidateNodeType` method

### Design

Add a public method to `FOliveNodeFactory`:

```cpp
/**
 * Validate whether a node type string is recognized.
 * Checks both the factory's built-in node creator map and the
 * node catalog (for CallFunction types specified by function name).
 *
 * @param NodeType  The type string to validate
 * @param Properties  The node properties (needed for CallFunction resolution)
 * @return True if the node type can be created
 */
bool ValidateNodeType(const FString& NodeType, const TMap<FString, FString>& Properties) const;
```

Implementation logic:
1. Check `NodeCreators.Contains(NodeType)` -- covers all built-in types (Branch, Sequence, etc.)
2. For `CallFunction` type, check that the `function_name` property resolves via `FindFunction()` or the catalog has a matching entry.
3. For `Event` type, check that `event_name` resolves via parent class lookup (this is already done inside `CreateEventNode` but should also be surfaced pre-creation).
4. Return false for unrecognized types.

Call `ValidateNodeType` at the top of `CreateNode`. If it returns false, set `LastError` to a structured message and return nullptr immediately. The caller (HandleBlueprintAddNode) then uses the catalog for suggestions (Task 2).

### Integration

No new files. The factory already has `IsNodeTypeSupported()` but it only checks the `NodeCreators` map -- it does not check function-level resolution or catalog membership. `ValidateNodeType` is the richer version that replaces it for pre-creation checks.

---

## Task 2 -- Fuzzy Suggestions for Unknown Node Types

### What

When `HandleBlueprintAddNode` receives an unknown node type, return an error payload that includes the top 5 fuzzy-matched suggestions from the node catalog.

### Files to modify

- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` -- `HandleBlueprintAddNode`
- `Source/OliveAIEditor/Blueprint/Public/Catalog/OliveNodeCatalog.h` -- add `FuzzyMatch` method
- `Source/OliveAIEditor/Blueprint/Private/Catalog/OliveNodeCatalog.cpp` -- implement `FuzzyMatch`

### Design

Add to `FOliveNodeCatalog`:

```cpp
/**
 * Return the top N closest matches for a query string.
 * Uses the existing MatchScore/CalculateMatchScore infrastructure
 * but also matches against built-in node type names (OliveNodeTypes namespace).
 *
 * @param Query  The unrecognized node type string
 * @param MaxResults  Maximum suggestions to return (default 5)
 * @return Array of {type_id, display_name, score} sorted by score descending
 */
struct FOliveNodeSuggestion
{
    FString TypeId;
    FString DisplayName;
    int32 Score = 0;
};

TArray<FOliveNodeSuggestion> FuzzyMatch(const FString& Query, int32 MaxResults = 5) const;
```

The struct can be a simple nested struct or declared in the header before the class. It does not need USTRUCT since it is not serialized to UE reflection -- it is converted to JSON inline.

In `HandleBlueprintAddNode`, insert validation before building the write request:

```cpp
// After parsing NodeType, before loading Blueprint:
FOliveNodeFactory& Factory = FOliveNodeFactory::Get();
if (!Factory.IsNodeTypeSupported(NodeType))
{
    // Check catalog for fuzzy matches
    FOliveNodeCatalog& Catalog = FOliveNodeCatalog::Get();
    TArray<FOliveNodeSuggestion> Suggestions = Catalog.FuzzyMatch(NodeType, 5);

    TSharedPtr<FJsonObject> ErrorData = MakeShareable(new FJsonObject());
    ErrorData->SetStringField(TEXT("requested_type"), NodeType);

    TArray<TSharedPtr<FJsonValue>> SuggestionArray;
    for (const auto& S : Suggestions)
    {
        TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
        Obj->SetStringField(TEXT("type_id"), S.TypeId);
        Obj->SetStringField(TEXT("display_name"), S.DisplayName);
        SuggestionArray.Add(MakeShareable(new FJsonValueObject(Obj)));
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
```

This check happens before Blueprint loading, so it is cheap and fast.

### JSON payload shape

```json
{
  "success": false,
  "error_code": "NODE_TYPE_UNKNOWN",
  "error_message": "Node type 'BranchNode' is not recognized",
  "suggestion": "Did you mean 'Branch'?",
  "requested_type": "BranchNode",
  "suggestions": [
    { "type_id": "Branch", "display_name": "Branch" },
    { "type_id": "K2Node_IfThenElse", "display_name": "Branch (If Then Else)" },
    { "type_id": "Func_KismetSystemLibrary_DoesImplementInterface", "display_name": "Does Implement Interface" }
  ]
}
```

---

## Task 3 -- Duplicate Native-Event Prevention

### What

Prevent creation of a second instance of a native event override (e.g., a second `BeginPlay` node) in the same Blueprint. Native events like `ReceiveBeginPlay`, `ReceiveTick`, `ReceiveDestroyed` can only exist once per Blueprint. A duplicate silently breaks the graph in UE.

### Files to modify

- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` -- `CreateEventNode`
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` -- `HandleBlueprintAddNode`

### Design

The existing `CreateEventNode` already calls `FBlueprintEditorUtils::FindOverrideForFunction` and returns the existing node if found (line 265-276 of `OliveNodeFactory.cpp`). However, this is silent -- the caller does not know whether the node was created fresh or if an existing one was returned. The agent may think it created a new node when it did not.

**Change 1: NodeFactory signals "already exists"**

Modify `CreateEventNode` to differentiate between "found existing" and "created new":

```cpp
// If override already exists, set error and return nullptr
// (do NOT silently return the existing node -- the caller needs to know)
if (EventNode)
{
    LastError = FString::Printf(
        TEXT("Native event '%s' already exists in this Blueprint (node at %d, %d). "
             "Each native event can only appear once."),
        *EventNameStr, EventNode->NodePosX, EventNode->NodePosY);
    return nullptr;
}
```

**Change 2: Structured error in HandleBlueprintAddNode**

In the executor lambda, when the factory returns nullptr and the error contains "already exists", produce a structured error:

```cpp
if (!WriteResult.bSuccess)
{
    FString ErrorMsg = WriteResult.Errors.Num() > 0 ? WriteResult.Errors[0] : TEXT("Unknown error");

    // Check for duplicate event pattern
    FString ErrorCode = TEXT("BP_ADD_NODE_FAILED");
    if (ErrorMsg.Contains(TEXT("already exists")))
    {
        ErrorCode = TEXT("DUPLICATE_NATIVE_EVENT");
    }

    return FOliveWriteResult::ExecutionError(ErrorCode, ErrorMsg,
        TEXT("Use blueprint.read_event_graph to find existing event nodes"));
}
```

However, it is cleaner to do the duplicate check **before** entering the write pipeline, alongside the Task 2 validation. This avoids the overhead of opening a transaction just to reject.

**Preferred approach -- pre-pipeline check in HandleBlueprintAddNode:**

After loading the Blueprint and before building the write request, when `NodeType == "Event"`:

```cpp
if (NodeType == OliveNodeTypes::Event)
{
    const FString* EventNamePtr = nullptr;
    // ... extract from properties
    if (EventNamePtr)
    {
        FName EventFName(**EventNamePtr);
        UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(
            Blueprint, Blueprint->ParentClass, EventFName);
        if (Existing)
        {
            return FOliveToolResult::Error(
                TEXT("DUPLICATE_NATIVE_EVENT"),
                FString::Printf(TEXT("Native event '%s' already exists at position (%d, %d). "
                    "Each native event can only appear once per Blueprint."),
                    **EventNamePtr, Existing->NodePosX, Existing->NodePosY),
                TEXT("Use blueprint.read_event_graph to see existing event nodes, "
                     "or use 'CustomEvent' type to create user-defined events")
            );
        }
    }
}
```

This is the safer pattern: validation before mutation. The NodeFactory change still applies as a defense-in-depth measure, but the tool handler catches it first with a better error.

---

## Task 4 -- Expanded Node-Removal Report Payloads

### What

When a node is removed, enumerate all connections that were broken by the removal and include them in the response payload. This lets the agent know which pins are now dangling and may need reconnection.

### Files to modify

- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp` -- `RemoveNode`
- `Source/OliveAIEditor/Blueprint/Public/Writer/OliveGraphWriter.h` -- update `RemoveNode` result type or add new struct
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` -- `HandleBlueprintRemoveNode` executor lambda

### Design

**Step 1: Capture connections before removal**

In `FOliveGraphWriter::RemoveNode`, before calling `FBlueprintEditorUtils::RemoveNode`, iterate the node's pins and record all connections:

```cpp
struct FOliveBrokenLink
{
    FString PinName;          // Pin on the removed node
    FString PinDirection;     // "input" or "output"
    FString ConnectedNodeId;  // Node ID of the connected node (resolved from cache or graph)
    FString ConnectedPinName; // Pin name on the connected node
};
```

Build a `TArray<FOliveBrokenLink>` by iterating `Node->Pins`:

```cpp
TArray<FOliveBrokenLink> BrokenLinks;
for (UEdGraphPin* Pin : Node->Pins)
{
    for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
    {
        if (LinkedPin && LinkedPin->GetOwningNode())
        {
            FOliveBrokenLink Link;
            Link.PinName = Pin->GetName();
            Link.PinDirection = (Pin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output");
            Link.ConnectedPinName = LinkedPin->GetName();
            // Resolve node ID from cache (reverse lookup)
            Link.ConnectedNodeId = ResolveNodeId(BlueprintPath, LinkedPin->GetOwningNode());
            BrokenLinks.Add(Link);
        }
    }
}
```

**Step 2: Add a `ResolveNodeId` helper to FOliveGraphWriter**

```cpp
// Private helper: reverse-lookup a UEdGraphNode* to its cached node ID
FString ResolveNodeId(const FString& BlueprintPath, UEdGraphNode* Node) const;
```

Implementation: iterate `NodeIdCache[BlueprintPath]` looking for a matching weak pointer. If not found, return the node's `GetName()` as fallback.

**Step 3: Return broken links in FOliveBlueprintWriteResult**

Add a new field to `FOliveBlueprintWriteResult`:

```cpp
/** Links broken by a removal operation */
TArray<FOliveBrokenLink> BrokenLinks;
```

Or, since `FOliveBlueprintWriteResult` is a USTRUCT, and `FOliveBrokenLink` would also need to be one, consider instead storing this as a `TSharedPtr<FJsonObject>` on the result and building the JSON directly in `RemoveNode`. This avoids adding a new USTRUCT just for removal metadata. Use a simple `TSharedPtr<FJsonObject> ExtraData` field or build it in the tool handler.

**Preferred approach:** Build the broken-link JSON array in the `HandleBlueprintRemoveNode` executor lambda by calling a new `FOliveGraphWriter` method that captures links before removal.

Add to `FOliveGraphWriter`:

```cpp
/**
 * Capture all connections on a node before removal.
 * Call this BEFORE RemoveNode to get a snapshot of what will break.
 * @param BlueprintPath Asset path for node ID resolution
 * @param Graph The graph containing the node
 * @param Node The node about to be removed
 * @return JSON array of broken link descriptors
 */
TArray<TSharedPtr<FJsonValue>> CaptureNodeConnections(
    const FString& BlueprintPath,
    UEdGraph* Graph,
    UEdGraphNode* Node);
```

In `HandleBlueprintRemoveNode`, restructure the executor to:
1. Resolve the node (via `GetCachedNode` or graph scan)
2. Call `CaptureNodeConnections` on it
3. Call `RemoveNode`
4. Include the captured connections in the result payload

**Result payload shape:**

```json
{
  "success": true,
  "asset_path": "/Game/BP_Player",
  "graph": "EventGraph",
  "node_id": "node_3",
  "message": "Successfully removed node 'node_3'",
  "broken_links": [
    {
      "pin": "then",
      "direction": "output",
      "was_connected_to": {
        "node_id": "node_4",
        "pin": "execute"
      }
    },
    {
      "pin": "Condition",
      "direction": "input",
      "was_connected_to": {
        "node_id": "node_2",
        "pin": "ReturnValue"
      }
    }
  ],
  "broken_link_count": 2
}
```

---

## Task 5 -- Post-Op Orphaned Exec-Flow Detection

### What

After any graph-mutating write operation (add_node, remove_node, connect_pins, disconnect_pins), detect exec-flow pins that are connected on one side but lead to a dead end on the other. Report these as warnings in the pipeline's Stage 5 (Verify) output.

An "orphaned exec flow" is an exec output pin that connects to nothing, breaking the execution chain. This commonly happens after node removal or when building graphs incrementally.

### Files to modify

- `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp` -- `VerifyBlueprintStructure`
- `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h` -- add helper declaration

### Design

Add a private helper to `FOliveWritePipeline`:

```cpp
/**
 * Detect orphaned execution flow in a graph.
 * An orphaned exec flow is an exec output pin that has no connections,
 * where the node itself has at least one connected exec input
 * (meaning execution reaches this node but cannot continue).
 *
 * @param Graph The graph to analyze
 * @param OutMessages Warning messages for each orphaned flow
 * @return Number of orphaned exec flows found
 */
int32 DetectOrphanedExecFlows(const UEdGraph* Graph, TArray<FOliveIRMessage>& OutMessages) const;
```

Implementation:
1. Iterate all nodes in `Graph->Nodes`
2. For each node, check if it has any exec output pins (`EEdGraphPinDirection::EGPD_Output` with `PC_Exec` category)
3. If an exec output pin has zero connections AND the node has at least one connected exec input pin (or is an event node), flag it
4. Skip pure nodes (they have no exec pins by definition)
5. Skip nodes that are intended terminal nodes: `K2Node_FunctionResult`, return nodes
6. Generate a warning message with node class, approximate node name, and the disconnected pin name

Call this from `VerifyBlueprintStructure` for graph-editing operations. The check should only run when `Request.ToolName` starts with `"blueprint.add_node"`, `"blueprint.remove_node"`, `"blueprint.connect_pins"`, or `"blueprint.disconnect_pins"`.

To scope the check to the affected graph (not all graphs in the Blueprint), pass the graph name through the request. Since `FOliveWriteRequest::Params` already contains the `"graph"` field, extract it in `StageVerify`:

```cpp
// In StageVerify, after structural checks:
FString GraphName;
if (Request.Params.IsValid())
{
    Request.Params->TryGetStringField(TEXT("graph"), GraphName);
}

if (!GraphName.IsEmpty() && IsGraphEditOperation(Request.ToolName))
{
    // Find the specific graph
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph && Graph->GetName() == GraphName)
        {
            DetectOrphanedExecFlows(Graph, StructuralMessages);
            break;
        }
    }
    // Also check function graphs
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetName() == GraphName)
        {
            DetectOrphanedExecFlows(Graph, StructuralMessages);
            break;
        }
    }
}
```

**Warning payload shape (in `validation_messages` array):**

```json
{
  "severity": "warning",
  "code": "ORPHANED_EXEC_FLOW",
  "message": "Node 'Branch' has disconnected exec output 'False' - execution flow will stop here",
  "suggestion": "Connect the 'False' pin to continue execution, or remove the node if not needed",
  "context": {
    "node_class": "K2Node_IfThenElse",
    "node_title": "Branch",
    "pin_name": "False",
    "node_position": { "x": 400, "y": 200 }
  }
}
```

Note: The `context` field requires adding a `Context` field to `FOliveIRMessage`. Check if one already exists. If not, add `TSharedPtr<FJsonObject> Context` to `FOliveIRMessage` so that structured context can be attached to any message.

### Edge cases

- **Sequence nodes with unused outputs**: A Sequence node with 3 outputs but only 2 connected is normal workflow -- only flag if zero outputs are connected.
- **Reroute nodes**: Treat as pass-through; do not flag reroute nodes with disconnected outputs as orphaned.
- **Comment nodes**: Skip entirely.
- **Custom events with no body**: These are valid (may be called via dispatcher). Do not flag custom events with disconnected exec output.

---

## Task 6 -- Large-Graph Read Mode

### What

When `blueprint.read` (or `blueprint.read_event_graph` / `blueprint.read_function`) encounters a graph with 500+ nodes, return a summary-first payload instead of the full node list. Provide a paging mechanism for the agent to request detail on demand.

### Files to modify

- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` -- reader tool handlers
- `Source/OliveAIEditor/Blueprint/Public/Reader/OliveGraphReader.h` -- add `ReadGraphSummary` and `ReadGraphPage`
- `Source/OliveAIEditor/Blueprint/Private/Reader/OliveGraphReader.cpp` -- implement them
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` -- update schema for paging params
- `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h` -- new schema declarations if needed

### Design

**Threshold constant:**

```cpp
// In OliveGraphReader.h or a shared constants header
static constexpr int32 OLIVE_LARGE_GRAPH_THRESHOLD = 500;
static constexpr int32 OLIVE_GRAPH_PAGE_SIZE = 100;
```

**New methods on FOliveGraphReader:**

```cpp
/**
 * Read a graph summary for large graphs.
 * Includes graph metadata, statistics, event/entry node list,
 * and a categorized node-type breakdown -- but no individual node details.
 *
 * @param Graph The graph to summarize
 * @param OwningBlueprint The owning Blueprint
 * @return Summary IR with NodeCount populated but Nodes array empty
 */
FOliveIRGraph ReadGraphSummary(const UEdGraph* Graph, const UBlueprint* OwningBlueprint);

/**
 * Read a page of nodes from a graph.
 * Returns nodes [Offset, Offset + Limit) from the graph's node list.
 * Node IDs are stable across pages (same ID map).
 *
 * @param Graph The graph to read
 * @param OwningBlueprint The owning Blueprint
 * @param Offset Zero-based node offset
 * @param Limit Maximum nodes to return
 * @return Partial graph with only the requested page of nodes
 */
FOliveIRGraph ReadGraphPage(
    const UEdGraph* Graph,
    const UBlueprint* OwningBlueprint,
    int32 Offset,
    int32 Limit);
```

**Summary payload:**

The summary includes:
- `node_count`, `connection_count` (already in FOliveIRGraph)
- `event_nodes` -- list of event/entry nodes with IDs and names (these are the "roots" the agent needs to navigate from)
- `node_type_breakdown` -- map of node class name to count (e.g., `{"K2Node_CallFunction": 180, "K2Node_VariableGet": 90, ...}`)
- `is_large_graph: true` flag
- `page_size` -- the recommended page size
- `total_pages` -- ceil(node_count / page_size)
- The `Nodes` array is empty in summary mode

To attach the extra summary fields, add them to the graph's JSON output. Since `FOliveIRGraph::ToJson()` is in the Runtime module and should stay lightweight, build the extra fields in the tool handler and merge them into the graph JSON:

```cpp
// In HandleBlueprintReadEventGraph (and similar handlers):
FOliveIRGraph GraphIR = GraphReader.ReadGraph(Graph, Blueprint);

if (GraphIR.NodeCount >= OLIVE_LARGE_GRAPH_THRESHOLD && Mode != TEXT("full"))
{
    // Switch to summary mode
    FOliveIRGraph SummaryIR = GraphReader.ReadGraphSummary(Graph, Blueprint);
    TSharedPtr<FJsonObject> ResultData = SummaryIR.ToJson();

    // Add large-graph metadata
    ResultData->SetBoolField(TEXT("is_large_graph"), true);
    ResultData->SetNumberField(TEXT("page_size"), OLIVE_GRAPH_PAGE_SIZE);
    ResultData->SetNumberField(TEXT("total_pages"),
        FMath::CeilToInt((float)SummaryIR.NodeCount / OLIVE_GRAPH_PAGE_SIZE));

    // Add event node list (the entry points)
    TArray<TSharedPtr<FJsonValue>> EventNodes;
    // ... build from SummaryIR
    ResultData->SetArrayField(TEXT("event_nodes"), EventNodes);

    // Add node type breakdown
    TSharedPtr<FJsonObject> Breakdown = MakeShareable(new FJsonObject());
    // ... build from counting node classes
    ResultData->SetObjectField(TEXT("node_type_breakdown"), Breakdown);

    return FOliveToolResult::Success(ResultData);
}
```

**Paging parameter:**

Add optional `page` and `page_size` parameters to the read tool schemas:

```json
{
  "page": { "type": "integer", "description": "Page number (0-based) for large graphs" },
  "page_size": { "type": "integer", "description": "Nodes per page (default 100, max 200)" }
}
```

When `page` is present, call `ReadGraphPage`:

```cpp
int32 Page = 0;
int32 PageSize = OLIVE_GRAPH_PAGE_SIZE;
Params->TryGetNumberField(TEXT("page"), Page);
Params->TryGetNumberField(TEXT("page_size"), PageSize);
PageSize = FMath::Clamp(PageSize, 10, 200);

if (Page >= 0 && Params->HasField(TEXT("page")))
{
    FOliveIRGraph PageIR = GraphReader.ReadGraphPage(Graph, Blueprint, Page * PageSize, PageSize);
    TSharedPtr<FJsonObject> ResultData = PageIR.ToJson();
    ResultData->SetNumberField(TEXT("page"), Page);
    ResultData->SetNumberField(TEXT("page_size"), PageSize);
    ResultData->SetNumberField(TEXT("total_pages"),
        FMath::CeilToInt((float)Graph->Nodes.Num() / (float)PageSize));
    ResultData->SetBoolField(TEXT("is_large_graph"), true);
    return FOliveToolResult::Success(ResultData);
}
```

**ReadGraphPage implementation:**

```cpp
FOliveIRGraph FOliveGraphReader::ReadGraphPage(
    const UEdGraph* Graph,
    const UBlueprint* OwningBlueprint,
    int32 Offset,
    int32 Limit)
{
    FOliveIRGraph Result;
    Result.Name = Graph->GetName();
    Result.GraphType = DetermineGraphType(Graph, OwningBlueprint);

    // Build full ID map (needed for connection resolution across pages)
    ClearCache();
    BuildNodeIdMap(Graph);

    // Serialize only the requested slice
    int32 Index = 0;
    for (const auto& Pair : NodeIdMap)
    {
        if (Index >= Offset + Limit) break;

        if (Index >= Offset)
        {
            const UEdGraphNode* Node = Pair.Key;
            if (Node)
            {
                FOliveIRNode NodeIR = NodeSerializer->SerializeNode(Node, NodeIdMap);
                NodeIR.Id = Pair.Value;
                Result.Nodes.Add(MoveTemp(NodeIR));
            }
        }
        Index++;
    }

    CalculateStatistics(Result);
    // Override NodeCount with total (not page count)
    Result.NodeCount = NodeIdMap.Num();

    return Result;
}
```

**Important:** The full `NodeIdMap` must be built even for paged reads so that cross-page connection references resolve correctly. A node on page 0 may reference a node on page 3 -- the connection string must still be valid.

**ReadGraphSummary implementation:**

Build the ID map, count node types, identify event nodes, but do not serialize individual nodes:

```cpp
FOliveIRGraph FOliveGraphReader::ReadGraphSummary(
    const UEdGraph* Graph,
    const UBlueprint* OwningBlueprint)
{
    FOliveIRGraph Result;
    Result.Name = Graph->GetName();
    Result.GraphType = DetermineGraphType(Graph, OwningBlueprint);

    ClearCache();
    BuildNodeIdMap(Graph);

    // Calculate stats without serializing nodes
    Result.NodeCount = NodeIdMap.Num();

    int32 ConnCount = 0;
    for (const auto& Pair : NodeIdMap)
    {
        if (Pair.Key)
        {
            for (UEdGraphPin* Pin : Pair.Key->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output)
                {
                    ConnCount += Pin->LinkedTo.Num();
                }
            }
        }
    }
    Result.ConnectionCount = ConnCount;

    // Nodes array intentionally left empty for summary
    return Result;
}
```

The event node list and type breakdown are built in the tool handler from the raw `Graph->Nodes`, not from the IR, to avoid serialization overhead.

---

## Implementation Order

The tasks should be implemented in this order:

1. **Task 1** (Node type validation) -- Foundation for Task 2
2. **Task 2** (Fuzzy suggestions) -- Depends on Task 1's validation path
3. **Task 3** (Duplicate event prevention) -- Independent, but same file as Task 2
4. **Task 4** (Removal report payloads) -- Independent
5. **Task 5** (Orphaned exec detection) -- Independent, but test after Tasks 3-4
6. **Task 6** (Large-graph read mode) -- Independent, most complex

Tasks 4 and 5 can be done in parallel. Task 6 can be done in parallel with Tasks 3-5.

---

## Cross-Cutting Concerns

### FOliveIRMessage Context Field

Task 5 needs to attach structured context (node class, position, pin name) to warning messages. Check if `FOliveIRMessage` already has a `Context` or `ExtraData` field. If not, add one:

```cpp
// In CommonIR.h, add to FOliveIRMessage:
/** Optional structured context data */
TSharedPtr<FJsonObject> Context;
```

Update `FOliveIRMessage::ToJson()` to include the context field when present. This is a minor IR change but it is additive (not breaking).

### Thread Safety

All changes are game-thread only. No new threading concerns.

### Error Code Conventions

New error codes introduced:
- `NODE_TYPE_UNKNOWN` -- unknown node type with suggestions
- `DUPLICATE_NATIVE_EVENT` -- duplicate native event blocked
- `ORPHANED_EXEC_FLOW` -- warning code for orphaned exec flows (not an error, a warning)

These follow the existing `SCREAMING_SNAKE_CASE` convention.

### Testing Checklist

For each task, the coder should verify:
1. Task 1-2: `blueprint.add_node` with type `"BranchNode"` (typo) returns suggestions including `"Branch"`
2. Task 1-2: `blueprint.add_node` with type `"CallFunction"` and `function_name: "NonexistentFunction"` returns catalog suggestions
3. Task 3: `blueprint.add_node` with type `"Event"` and `event_name: "ReceiveBeginPlay"` twice -- second call returns `DUPLICATE_NATIVE_EVENT`
4. Task 4: `blueprint.remove_node` on a node with 3 connections returns `broken_links` array with 3 entries
5. Task 5: After `blueprint.remove_node` that leaves orphaned exec pins, the response `validation_messages` contains `ORPHANED_EXEC_FLOW` warnings
6. Task 6: `blueprint.read_event_graph` on a graph with 600+ nodes returns `is_large_graph: true` summary; `page=0` returns first 100 nodes
