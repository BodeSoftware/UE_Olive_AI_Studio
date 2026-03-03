# Phase 5.5 Warning Escalation + Events Knowledge + Community Browse

## Overview

Three features that collectively address the AI's tendency to design interface functions incorrectly and to commit to the first community example it finds.

**Feature 1 -- Phase 5.5 Warning Escalation**: When `apply_plan_json` detects unwired return pins on an interface function graph, escalate the warning from buried-in-success to a prominent `"design_warnings"` top-level field in the tool result. The plan DID succeed; this is not a failure. But the AI must see it.

**Feature 2 -- Events vs Functions Knowledge**: A dedicated knowledge file (`events_vs_functions.txt`) loaded into Auto/Blueprint capability packs. Provides decision framework for when to use events vs functions, with interface-specific guidance and the hybrid pattern.

**Feature 3 -- Community Browse Mode**: Add `mode` and `ids` parameters to `olive.search_community_blueprints`. Browse mode returns compact summaries (title, type, description snippet, node count). The AI browses many, then fetches detail on the best candidates.

These three features are independent and can be implemented in parallel.

---

## Feature 1: Phase 5.5 Warning Escalation

### Problem Statement

Phase 5.5 currently adds `INTERFACE_FUNCTION_HINT` and `UNWIRED_RETURN_PIN` strings to `Context.PreCompileIssues`, which flow into `Context.Warnings`, which flow into `PlanResult.Warnings`, which get serialized as a `"warnings"` JSON array in the tool result. The tool result is `bSuccess=true`.

The self-correction policy (`FOliveSelfCorrectionPolicy::Evaluate()`) only triggers on `HasToolFailure()` (top-level `bSuccess=false`) or `HasCompileFailure()` (nested `compile_result.success=false`). Warnings inside a success result are invisible to the correction loop.

The AI has ignored these warnings across 3 test sessions. Prompt changes have not helped.

### Design: Design Warnings as Prominent Top-Level Field

The approach does NOT change `bSuccess`. The plan genuinely succeeded. Instead, we:

1. Separate `INTERFACE_FUNCTION_HINT` issues from generic warnings into a new top-level `"design_warnings"` array in the tool result JSON
2. Make the design warning message actionable and prominent with clear guidance
3. Add a `"has_design_warnings": true` boolean to the top-level result so it is impossible to miss

#### Why NOT a new result status

Adding `SUCCESS_WITH_WARNINGS` to `FOliveWriteResult` or `FOliveToolResult` would require changes to `ToToolResult()`, the write pipeline, the MCP JSON serialization, and the self-correction policy. It is high-touch for a narrow concern. The plan succeeded; the status should say success. The issue is visibility, not status.

#### Why NOT trigger self-correction

The plan worked. Nodes are committed. Triggering `FeedBackErrors` would tell the AI to resubmit, but there is nothing to resubmit -- the graph is already built. What we want is for the AI to **consider redesigning the interface** as a follow-up action, not to undo what it just did.

### Code Changes

#### 1. PlanExecutor: Tag interface hints distinctly (no change needed)

The existing code in `PhasePreCompileValidation()` already produces two distinct strings for unwired return pins:
- `UNWIRED_RETURN_PIN: ...` (generic)
- `INTERFACE_FUNCTION_HINT: ...` (interface-specific)

Both are added to `Context.PreCompileIssues`. The `INTERFACE_FUNCTION_HINT` prefix is sufficient to identify these in downstream code. No changes to `OlivePlanExecutor.cpp`.

#### 2. PlanExecutor: Improve the INTERFACE_FUNCTION_HINT message

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** Lines ~3180-3184 (the `INTERFACE_FUNCTION_HINT` string)

Change the message from:

```
INTERFACE_FUNCTION_HINT: This is an interface function with an unwired return value '%s'.
If you don't need a return value, consider redesigning the interface function without outputs --
this makes it an implementable event that supports Timelines, Delays, and latent actions.
```

To:

```
INTERFACE_FUNCTION_HINT: Interface function '%s' has unwired return pin '%s' (%s).
Functions WITH outputs become synchronous function graphs -- no Timelines, Delays, or latent nodes allowed.
If this function's implementations need smooth movement, animations, or multi-frame behavior,
remove the outputs from the interface to make it an implementable event instead.
If you genuinely need BOTH a return value AND async behavior, use the hybrid pattern:
function returns immediately, then calls a Custom Event for the async work.
```

The `%s` args are: `Context.GraphName` (function name), `Pin->GetName()` (pin name), `TypeName` (type).

This requires changing the `FString::Printf` to include `Context.GraphName` as the first arg.

#### 3. BlueprintToolHandlers: Separate design_warnings from warnings

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
**Location:** In the `HandleBlueprintApplyPlanJson` handler, around lines 7561-7571 where warnings are serialized into `ResultData`.

Currently:
```cpp
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
```

Change to split out design warnings:
```cpp
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
```

#### 4. BlueprintToolHandlers: Forward design_warnings through pipeline result

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
**Location:** Around lines 7888-7892 where warnings are forwarded from `PipelineResult.ResultData` to `ToolResult.Data`.

After the existing warnings forwarding block, add:
```cpp
const TArray<TSharedPtr<FJsonValue>>* DesignWarnings = nullptr;
if (PipelineResult.ResultData->TryGetArrayField(TEXT("design_warnings"), DesignWarnings))
{
    ToolResult.Data->SetArrayField(TEXT("design_warnings"), *DesignWarnings);
    ToolResult.Data->SetBoolField(TEXT("has_design_warnings"), true);
}
```

#### What the AI Sees

Before (buried):
```json
{
  "success": true,
  "warnings": [
    "Phase 5.5: Auto-fixed orphaned exec...",
    "UNWIRED_RETURN_PIN: Function output 'bSuccess' (bool) on FunctionResult is not wired...",
    "INTERFACE_FUNCTION_HINT: This is an interface function with an unwired return value 'bSuccess'..."
  ]
}
```

After (prominent):
```json
{
  "success": true,
  "has_design_warnings": true,
  "warnings": [
    "Phase 5.5: Auto-fixed orphaned exec...",
    "UNWIRED_RETURN_PIN: Function output 'bSuccess' (bool) on FunctionResult is not wired..."
  ],
  "design_warnings": [
    "INTERFACE_FUNCTION_HINT: Interface function 'Interact' has unwired return pin 'bSuccess' (bool). Functions WITH outputs become synchronous function graphs -- no Timelines, Delays, or latent nodes allowed. If this function's implementations need smooth movement, animations, or multi-frame behavior, remove the outputs from the interface to make it an implementable event instead. If you genuinely need BOTH a return value AND async behavior, use the hybrid pattern: function returns immediately, then calls a Custom Event for the async work."
  ]
}
```

The `"has_design_warnings": true` boolean at the top level is the key forcing function. LLMs notice top-level booleans far more reliably than items buried in arrays.

#### Self-Correction Policy: No Changes

The self-correction policy does not need modification. The plan succeeded; `Evaluate()` returns `Continue`. The design warning is informational guidance for the AI's next decision, not a retry trigger. The AI should read `design_warnings`, consider whether the interface design is appropriate, and potentially modify the interface in a follow-up call if needed -- but this is the AI's judgment call, not an automated correction loop.

---

## Feature 2: Events vs Functions Knowledge File

### File Content

**Path:** `Content/SystemPrompts/Knowledge/events_vs_functions.txt`

```
TAGS: event function interface async timeline delay latent synchronous return output implementable
---
## Events vs Functions

### Decision Framework
USE A FUNCTION when you need to return a value to the caller AND the logic is synchronous (completes in one frame).
Examples: GetHealth(), CalculateDamage(), IsAlive(), CanInteract():Bool

USE AN EVENT when the logic might span multiple frames OR the caller doesn't need a return value.
Events support Timelines, Delays, and latent actions. Functions do NOT.
Examples: Interact(), OnDeath(), OpenDoor(), StartAbility()

KEY CONSTRAINT: Function graphs are synchronous-only. They CANNOT contain Timeline nodes, Delay nodes, or any latent action. If your implementation needs any of these, it must be an event.

### Interface Functions vs Interface Events
When defining functions in a Blueprint Interface:
- NO outputs = implementable EVENT (lives in EventGraph, supports async)
- HAS outputs = implementable FUNCTION (lives in synchronous function graph)

Adding outputs to an interface function forces ALL implementations into synchronous function graphs.
Before adding a return value, ask: will any implementation need Timelines, Delays, or multi-frame behavior?
If yes, omit the outputs to get an implementable event.

COMMON MISTAKE: Adding Bool Success output to Interact(). This forces synchronous function graphs on every implementing Blueprint, preventing smooth door rotations, animation playback, or timed effects.

### Hybrid Pattern: Function + Event
When you genuinely need BOTH a return value AND async behavior:
1. Interface function returns a synchronous result immediately (e.g., Bool for accepted/rejected)
2. Inside the function, call a Custom Event on Self
3. The Custom Event runs in EventGraph with full Timeline/Delay support

Example: Interact():Bool that also opens a door smoothly
- Function checks CanToggle → if no, return false
- Function calls Custom Event "DoOpenClose", returns true
- DoOpenClose (EventGraph): Timeline → Lerp → SetRelativeRotation

Use this pattern ONLY when the caller needs the return value. If not, just use an event (simpler).
```

### Integration: How It Gets Loaded

Knowledge files in `Content/SystemPrompts/Knowledge/` are loaded automatically by `FOlivePromptAssembler::LoadPromptTemplates()` (lines 512-541 in `OlivePromptAssembler.cpp`). It scans the directory non-recursively for `.txt` files. The file name (lowercased, without extension) becomes the pack ID.

So `events_vs_functions.txt` becomes pack ID `events_vs_functions`.

To make it visible in Auto and Blueprint profiles, add it to the `ProfileCapabilityPackIds` map.

**File:** `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp`
**Location:** Lines 544-545 (the profile -> pack mapping)

Change from:
```cpp
ProfileCapabilityPackIds.Add(TEXT("Auto"), { TEXT("blueprint_authoring"), TEXT("recipe_routing"), TEXT("node_routing"), TEXT("blueprint_design_patterns") });
ProfileCapabilityPackIds.Add(TEXT("Blueprint"), { TEXT("blueprint_authoring"), TEXT("recipe_routing"), TEXT("node_routing"), TEXT("blueprint_design_patterns") });
```

To:
```cpp
ProfileCapabilityPackIds.Add(TEXT("Auto"), { TEXT("blueprint_authoring"), TEXT("recipe_routing"), TEXT("node_routing"), TEXT("blueprint_design_patterns"), TEXT("events_vs_functions") });
ProfileCapabilityPackIds.Add(TEXT("Blueprint"), { TEXT("blueprint_authoring"), TEXT("recipe_routing"), TEXT("node_routing"), TEXT("blueprint_design_patterns"), TEXT("events_vs_functions") });
```

### Token Budget

The knowledge file is ~45 lines, roughly 350-400 tokens. This is added to every Auto/Blueprint system prompt. The information is high-value (directly prevents a recurring 3-session-long failure pattern) and low-cost (under 400 tokens), so this is an easy trade-off.

### TAGS for Recipe Routing

The TAGS line includes: `event function interface async timeline delay latent synchronous return output implementable`

This ensures `olive.get_recipe` with queries like "interface event", "function return", "timeline delay", or "async behavior" will surface this file alongside relevant recipes.

---

## Feature 3: Community Blueprint Browse Mode

### Schema Changes

**File:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemSchemas.cpp`
**Location:** The `CommunitySearch()` function starting at line 318

Add two new properties to the schema:

```cpp
TSharedPtr<FJsonObject> CommunitySearch()
{
    TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
    TSharedPtr<FJsonObject> Props = MakeProperties();

    Props->SetObjectField(TEXT("query"),
        OliveBlueprintSchemas::StringProp(TEXT("Search terms (e.g. 'gun fire reload', 'pickup overlap interact', 'health damage')")));
    Props->SetObjectField(TEXT("type"),
        OliveBlueprintSchemas::StringProp(TEXT("Optional: filter by asset type -- 'blueprint', 'material', 'pcg'. Omit to search all types.")));
    Props->SetObjectField(TEXT("max_results"),
        OliveBlueprintSchemas::IntProp(TEXT("Maximum results to return (1-20 for browse, 1-10 for detail, default 5)"), 1, 20));
    Props->SetObjectField(TEXT("offset"),
        OliveBlueprintSchemas::IntProp(TEXT("Skip this many results for pagination (default 0)"), 0, 10000));
    Props->SetObjectField(TEXT("mode"),
        OliveBlueprintSchemas::StringProp(TEXT("'browse' returns compact summaries (title, type, description, node_count) for scanning many results. 'detail' (default) returns full graph data. Use browse first to scan, then detail with ids to fetch specific entries.")));
    Props->SetObjectField(TEXT("ids"),
        OliveBlueprintSchemas::ArrayProp(TEXT("Fetch specific entries by slug. Use with mode:'detail' after browsing. Overrides query when provided."),
            OliveBlueprintSchemas::StringProp(TEXT("Blueprint slug/ID from browse results"))));

    Schema->SetObjectField(TEXT("properties"), Props);
    // query is no longer strictly required when ids is provided
    // But we keep it in required for backwards compatibility --
    // the handler will check: if ids is provided, query is optional
    AddRequired(Schema, { TEXT("query") });
    return Schema;
}
```

**Design decision on `required`:** Keep `query` in the required array for backward compatibility. The handler will accept an empty string for `query` when `ids` is provided. This avoids breaking existing tool schemas cached by MCP clients.

### Handler Changes

**File:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`
**Location:** `HandleSearchCommunityBlueprints()` starting at line 1495

The handler needs to support three code paths:

1. **Browse mode with query** -- compact summaries, higher max_results
2. **Detail mode with query** -- current behavior (backward compatible default)
3. **Detail mode with ids** -- fetch specific entries by slug

#### Parameter Extraction (replace current block at lines 1497-1519)

```cpp
FOliveToolResult FOliveCrossSystemToolHandlers::HandleSearchCommunityBlueprints(
    const TSharedPtr<FJsonObject>& Params)
{
    // --- Mode extraction ---
    FString Mode = TEXT("detail");
    Params->TryGetStringField(TEXT("mode"), Mode);
    Mode = Mode.ToLower();
    const bool bIsBrowseMode = (Mode == TEXT("browse"));

    // --- IDs extraction (for targeted detail fetch) ---
    TArray<FString> RequestedIds;
    const TArray<TSharedPtr<FJsonValue>>* IdsArray = nullptr;
    if (Params->TryGetArrayField(TEXT("ids"), IdsArray) && IdsArray)
    {
        for (const TSharedPtr<FJsonValue>& Val : *IdsArray)
        {
            FString Id;
            if (Val->TryGetString(Id) && !Id.IsEmpty())
            {
                RequestedIds.Add(Id);
            }
        }
    }
    const bool bHasIds = RequestedIds.Num() > 0;

    // --- Query extraction ---
    FString Query;
    Params->TryGetStringField(TEXT("query"), Query);

    // query is required unless ids are provided
    if (Query.IsEmpty() && !bHasIds)
    {
        return FOliveToolResult::Error(TEXT("MISSING_PARAM"),
            TEXT("'query' is required (unless 'ids' is provided)"),
            TEXT("Provide search terms like 'gun fire reload' or use 'ids' to fetch specific entries"));
    }

    // ... rest of extraction (type, max_results, offset) unchanged ...
```

#### Max Results Limits

```cpp
    // Browse mode allows more results (compact, low token cost)
    const int32 MaxResultsCap = bIsBrowseMode ? 20 : 10;
    int32 MaxResults = bIsBrowseMode ? 10 : 5; // default differs by mode
    if (Params->HasField(TEXT("max_results")))
    {
        MaxResults = FMath::Clamp(
            static_cast<int32>(Params->GetNumberField(TEXT("max_results"))),
            1, MaxResultsCap);
    }
```

#### IDs Fetch Path (new code, before the FTS query path)

When `ids` is provided, skip the FTS query entirely and do a direct slug lookup:

```cpp
    if (bHasIds)
    {
        // Clamp to max 10 IDs per request
        if (RequestedIds.Num() > 10)
        {
            RequestedIds.SetNum(10);
        }

        // Build SQL with IN clause using placeholders
        FString Placeholders;
        for (int32 i = 0; i < RequestedIds.Num(); ++i)
        {
            if (i > 0) Placeholders += TEXT(",");
            Placeholders += TEXT("?");
        }

        const FString IdSql = FString::Printf(TEXT(
            "SELECT b.slug, b.title, b.type, b.ue_version, b.node_count, "
            "b.functions, b.variables, b.components, b.compact, b.url "
            "FROM blueprints b "
            "WHERE b.slug IN (%s)"), *Placeholders);

        FSQLitePreparedStatement IdStmt = CommunityDb->PrepareStatement(
            *IdSql, ESQLitePreparedStatementFlags::None);

        if (!IdStmt.IsValid())
        {
            return FOliveToolResult::Error(TEXT("DB_ERROR"),
                FString::Printf(TEXT("Failed to prepare ID lookup: %s"),
                    *CommunityDb->GetLastError()),
                TEXT("Check database integrity"));
        }

        for (int32 i = 0; i < RequestedIds.Num(); ++i)
        {
            IdStmt.SetBindingValueByIndex(i + 1, RequestedIds[i]);
        }

        TArray<TSharedPtr<FJsonValue>> Results;
        IdStmt.Execute([&Results](const FSQLitePreparedStatement& Row)
            -> ESQLitePreparedStatementExecuteRowResult
        {
            // Same column extraction as the full detail path
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            FString Slug, Title, Type, UeVersion, Functions, Variables, Components, Compact, Url;
            int32 NodeCount = 0;

            Row.GetColumnValueByIndex(0, Slug);
            Row.GetColumnValueByIndex(1, Title);
            Row.GetColumnValueByIndex(2, Type);
            Row.GetColumnValueByIndex(3, UeVersion);
            Row.GetColumnValueByIndex(4, NodeCount);
            Row.GetColumnValueByIndex(5, Functions);
            Row.GetColumnValueByIndex(6, Variables);
            Row.GetColumnValueByIndex(7, Components);
            Row.GetColumnValueByIndex(8, Compact);
            Row.GetColumnValueByIndex(9, Url);

            Entry->SetStringField(TEXT("id"), Slug);
            Entry->SetStringField(TEXT("title"), Title);
            Entry->SetStringField(TEXT("type"), Type);
            Entry->SetStringField(TEXT("ue_version"), UeVersion);
            Entry->SetNumberField(TEXT("node_count"), NodeCount);
            Entry->SetStringField(TEXT("functions"), Functions);
            Entry->SetStringField(TEXT("variables"), Variables);
            if (!Components.IsEmpty())
            {
                Entry->SetStringField(TEXT("components"), Components);
            }
            Entry->SetStringField(TEXT("compact"), Compact);
            Entry->SetStringField(TEXT("url"), Url);

            Results.Add(MakeShared<FJsonValueObject>(Entry));
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        });

        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetArrayField(TEXT("results"), Results);
        Response->SetNumberField(TEXT("count"), Results.Num());
        Response->SetStringField(TEXT("mode"), TEXT("detail"));
        return FOliveToolResult::Success(Response);
    }
```

#### Browse Mode Query (new SQL, returns compact data)

When `bIsBrowseMode` is true, use a SQL query that selects only lightweight columns. The `compact` column (which contains the full graph summary) is the expensive one -- browse mode omits it along with `functions`, `variables`, and `components`.

Instead, browse mode returns the `slug` (as `id`), `title`, `type`, `ue_version`, `node_count`, and the first ~150 characters of `compact` as a `description` snippet.

```cpp
    // --- Browse mode vs Detail mode ---
    if (bIsBrowseMode)
    {
        // Browse: compact summaries only
        const TCHAR* BrowseSqlBase = TEXT(
            "SELECT b.slug, b.title, b.type, b.ue_version, b.node_count, "
            "SUBSTR(b.compact, 1, 150) "
            "FROM blueprints_fts fts "
            "JOIN blueprints b ON b.slug = fts.slug "
            "WHERE blueprints_fts MATCH ?%s "
            "ORDER BY "
            "CASE WHEN CAST(b.ue_version AS REAL) >= 5.0 THEN 0 ELSE 1 END, "
            "rank "
            "LIMIT ? OFFSET ?");

        const FString BrowseSql = FString::Printf(BrowseSqlBase,
            bHasTypeFilter ? TEXT(" AND b.type = ?") : TEXT(""));

        FSQLitePreparedStatement BrowseStmt = CommunityDb->PrepareStatement(
            *BrowseSql, ESQLitePreparedStatementFlags::None);

        if (!BrowseStmt.IsValid())
        {
            return FOliveToolResult::Error(TEXT("DB_ERROR"),
                FString::Printf(TEXT("Failed to prepare browse query: %s"),
                    *CommunityDb->GetLastError()),
                TEXT("Check database integrity"));
        }

        int32 BrowseBindIdx = 1;
        BrowseStmt.SetBindingValueByIndex(BrowseBindIdx++, FtsQuery);
        if (bHasTypeFilter)
        {
            BrowseStmt.SetBindingValueByIndex(BrowseBindIdx++, TypeFilter);
        }
        BrowseStmt.SetBindingValueByIndex(BrowseBindIdx++, MaxResults);
        BrowseStmt.SetBindingValueByIndex(BrowseBindIdx++, Offset);

        TArray<TSharedPtr<FJsonValue>> Results;

        BrowseStmt.Execute([&Results](const FSQLitePreparedStatement& Row)
            -> ESQLitePreparedStatementExecuteRowResult
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            FString Slug, Title, Type, UeVersion, DescSnippet;
            int32 NodeCount = 0;

            Row.GetColumnValueByIndex(0, Slug);
            Row.GetColumnValueByIndex(1, Title);
            Row.GetColumnValueByIndex(2, Type);
            Row.GetColumnValueByIndex(3, UeVersion);
            Row.GetColumnValueByIndex(4, NodeCount);
            Row.GetColumnValueByIndex(5, DescSnippet);

            Entry->SetStringField(TEXT("id"), Slug);
            Entry->SetStringField(TEXT("title"), Title);
            Entry->SetStringField(TEXT("type"), Type);
            Entry->SetStringField(TEXT("ue_version"), UeVersion);
            Entry->SetNumberField(TEXT("node_count"), NodeCount);
            if (!DescSnippet.IsEmpty())
            {
                Entry->SetStringField(TEXT("description"), DescSnippet);
            }

            Results.Add(MakeShared<FJsonValueObject>(Entry));
            return ESQLitePreparedStatementExecuteRowResult::Continue;
        });

        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetArrayField(TEXT("results"), Results);
        Response->SetNumberField(TEXT("count"), Results.Num());
        Response->SetStringField(TEXT("mode"), TEXT("browse"));

        if (TotalMatches > 0)
        {
            Response->SetNumberField(TEXT("total_matches"), static_cast<double>(TotalMatches));
            Response->SetBoolField(TEXT("has_more"), (Offset + Results.Num()) < TotalMatches);
        }

        if (TotalMatches > 0 && (Offset + Results.Num()) < TotalMatches)
        {
            Response->SetStringField(TEXT("note"),
                FString::Printf(TEXT("Showing %d of %lld matches (browse mode). "
                    "Use 'ids' with mode:'detail' to fetch full data for specific entries. "
                    "Use offset=%d for more results."),
                    Results.Num(), TotalMatches, Offset + Results.Num()));
        }

        return FOliveToolResult::Success(Response);
    }

    // --- Detail mode with query (existing path, unchanged) ---
    // ... existing SQL and execution code stays as-is ...
```

#### Detail Mode: Add `id` and `mode` Fields

In the existing detail-mode path, add two fields to each result entry and the response:

1. Add `Entry->SetStringField(TEXT("id"), Slug)` by reading slug from column 0 (currently skipped with comment "0=slug (skip)")
2. Add `Response->SetStringField(TEXT("mode"), TEXT("detail"))` to the response

This ensures the detail response includes the `id` field that browse mode returns, so the AI can reference them consistently.

### Browse Response Format

```json
{
  "results": [
    {
      "id": "door-smooth-timeline-12345",
      "title": "Smooth Door with Timeline",
      "type": "Actor",
      "ue_version": "5.3",
      "node_count": 24,
      "description": "BeginPlay -> Timeline(DoorTimeline) -> Lerp(Rotator) -> SetRelativeRotation..."
    },
    {
      "id": "door-instant-toggle-67890",
      "title": "Simple Toggle Door",
      "type": "Actor",
      "ue_version": "5.1",
      "node_count": 8,
      "description": "Event(Interact) -> Branch(bIsOpen) -> SetRelativeRotation(0,90,0)..."
    }
  ],
  "count": 2,
  "mode": "browse",
  "total_matches": 47,
  "has_more": true,
  "note": "Showing 2 of 47 matches (browse mode). Use 'ids' with mode:'detail' to fetch full data for specific entries. Use offset=2 for more results."
}
```

Token cost per browse entry: ~40-60 tokens (vs ~500-1000+ for detail). A 20-result browse scan costs roughly the same as 1-2 detail results.

### Tool Description Update

**File:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`
**Location:** Lines 1477-1484 (tool description in `RegisterCommunityTools()`)

Update the description to mention browse mode:

```cpp
TEXT("Search ~150K real-world community Blueprints for gameplay patterns. "
    "Two modes: 'browse' returns compact summaries (title, type, node_count, description snippet) "
    "for scanning many results quickly. 'detail' (default) returns full graph data. "
    "Recommended workflow: browse first with max_results:15-20, then fetch full detail "
    "on 3-5 promising entries using the 'ids' parameter. "
    "Results prefer UE 5.0+ examples. Quality varies -- adapt rather than copy.")
```

### Backward Compatibility

- Default `mode` is `"detail"` -- existing callers see no change
- `query` remains in `required` -- existing schemas work
- When `ids` is provided, `query` can be empty string (handler checks for this)
- All existing response fields are preserved in detail mode
- New `id` field added to detail mode results (additive, non-breaking)

---

## Implementation Order

These three features are independent. The coder can build them in any order or in parallel. However, for logical grouping:

1. **Feature 2 (Knowledge file)** -- Simplest, zero C++ changes, just create the text file and add the pack ID. Can be tested immediately.
2. **Feature 1 (Warning escalation)** -- Small C++ changes in two files, no new structs or APIs.
3. **Feature 3 (Community browse)** -- Most code, but self-contained in CrossSystem module.

---

## Coder Tasks

### Task 1: Create events_vs_functions.txt knowledge file

**File to create:** `Content/SystemPrompts/Knowledge/events_vs_functions.txt`

Create the file with the exact content specified in Feature 2 above (the block starting with `TAGS:` and ending with `Use this pattern ONLY when the caller needs the return value. If not, just use an event (simpler).`).

### Task 2: Wire events_vs_functions into capability packs

**File to edit:** `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp`
**Location:** Lines 544-545

Add `TEXT("events_vs_functions")` to both the Auto and Blueprint profile pack arrays. See Feature 2 section above for exact before/after.

### Task 3: Improve INTERFACE_FUNCTION_HINT message

**File to edit:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**Location:** Lines ~3180-3184 (the `Context.PreCompileIssues.Add` call for `INTERFACE_FUNCTION_HINT`)

Replace the current message format string with the improved version that includes the graph name and mentions the hybrid pattern. The new format string takes three `%s` args: `*Context.GraphName`, `*Pin->GetName()`, `*TypeName` (in that order).

### Task 4: Separate design_warnings in apply_plan_json result

**File to edit:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
**Two locations:**

**Location A:** Around lines 7561-7571 (inside the executor lambda where `PlanResult.Warnings` are serialized into `ResultData`). Replace the simple warnings loop with the split logic that separates `INTERFACE_FUNCTION_HINT:` entries into a `design_warnings` array. Add `has_design_warnings` boolean.

**Location B:** Around lines 7888-7892 (where `PipelineResult.ResultData` fields are forwarded to `ToolResult.Data`). Add forwarding of `design_warnings` and `has_design_warnings` after the existing warnings forwarding block.

### Task 5: Add mode and ids parameters to community search schema

**File to edit:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemSchemas.cpp`
**Location:** The `CommunitySearch()` function starting at line 318

Add `mode` (string) and `ids` (array of string) properties to the schema. Keep `query` in required. See Feature 3 schema section for exact code.

### Task 6: Implement browse mode and ids fetch in community search handler

**File to edit:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`
**Location:** `HandleSearchCommunityBlueprints()` starting at line 1495

This is the largest change. Restructure the handler:

1. Extract `mode` parameter (default "detail") and `ids` parameter (optional array)
2. Allow empty `query` when `ids` is provided
3. Adjust `MaxResults` cap: 20 for browse, 10 for detail
4. If `ids` provided: direct slug lookup SQL (no FTS), return full detail for matched slugs
5. If browse mode: lightweight SQL selecting slug, title, type, ue_version, node_count, and SUBSTR(compact, 1, 150) as description. Higher default max_results (10 vs 5).
6. If detail mode with query: existing code path, but add `id` (slug) and `mode` fields to response
7. The count query runs before the mode branch (it is used by both browse and detail paths with query)

See Feature 3 handler section for detailed code.

### Task 7: Update community search tool description

**File to edit:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`
**Location:** Lines 1477-1484 (tool description in `RegisterCommunityTools()`)

Replace the tool description with the updated version that mentions browse mode, ids parameter, and the recommended browse-then-detail workflow.

### Task 8: Add id field to detail mode results

**File to edit:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`
**Location:** Inside the Execute lambda for the detail query (around line 1638-1670)

Currently column 0 (slug) is read but not used. Add:
```cpp
FString Slug;
Row.GetColumnValueByIndex(0, Slug);
Entry->SetStringField(TEXT("id"), Slug);
```

This ensures detail mode results have the same `id` field as browse results.

---

## Edge Cases

### Feature 1
- **No unwired return pins:** `design_warnings` array is simply not present. `has_design_warnings` is not set. Zero impact on normal flow.
- **Multiple unwired return pins:** Multiple `INTERFACE_FUNCTION_HINT` entries in the `design_warnings` array. Each pin gets its own entry.
- **Non-interface function with unwired returns:** Only `UNWIRED_RETURN_PIN` fires, not `INTERFACE_FUNCTION_HINT`. These stay in regular `warnings`. No design warning.

### Feature 2
- **File not found at startup:** `LoadPromptTemplates()` silently skips missing files. The pack ID in the profile mapping just returns empty content. No crash, no error. The knowledge is simply absent.
- **Token budget:** ~400 tokens added per system prompt. Well within budget.

### Feature 3
- **Empty query with ids:** Valid. Handler skips FTS entirely, does direct slug lookup.
- **Invalid slugs in ids:** SQL IN clause returns no rows for non-matching slugs. Result array is shorter than requested. No error.
- **ids with mode:"browse":** Ignored. When `ids` is provided, always returns full detail (browse is for scanning, not fetching known entries).
- **ids exceeds 10:** Clamped to first 10 entries silently.
- **Database not available:** Existing "database not found" path returns empty result with note. Unchanged for all modes.
- **FTS special characters in query:** Existing error handling for `QUERY_ERROR` applies to browse mode too.
