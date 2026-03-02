# Task: Add Community Blueprint Search MCP Tool

## What This Is

We have a SQLite database at `Content/CommunityBlueprints/community_blueprints.db` containing ~150,000 community-submitted examples scraped from blueprintue.com. The AI searches this to see how real developers solved common patterns before building its own version. This is **reference material, not instructions** — quality varies, the AI reads results and uses its judgment.

This is separate from:
- **Templates** (`blueprint.get_template`) — curated, verified Blueprint factories we control
- **Recipes** (`olive.get_recipe`) — tested step-by-step wiring patterns
- **Community examples** (this tool) — crowd-sourced reference material the AI evaluates

## Database Schema

The database is a standard SQLite file with FTS5 full-text search. Schema:

```sql
CREATE TABLE blueprints (
    slug        TEXT PRIMARY KEY,
    title       TEXT NOT NULL,
    type        TEXT,           -- 'blueprint', 'material', 'pcg', etc.
    ue_version  TEXT,
    url         TEXT,
    node_count  INTEGER,
    node_types  TEXT,           -- comma-separated K2Node class names
    functions   TEXT,           -- comma-separated function names used
    variables   TEXT,           -- comma-separated variable names
    components  TEXT,           -- comma-separated component names
    compact     TEXT,           -- human-readable graph summary for AI consumption
    search_text TEXT            -- full-text search content with synonym expansion
);

CREATE VIRTUAL TABLE blueprints_fts USING fts5(
    slug UNINDEXED, title, search_text,
    content='blueprints', content_rowid='rowid',
    tokenize='porter unicode61'
);
```

The `compact` field is what the AI reads. Example:
```
Flow: InputAction.Pressed → Branch → SpawnActorFromClass → SetVelocity
Data: GetBaseAimRotation.ReturnValue → SpawnActor.SpawnTransform
Defaults: SpawnActorFromClass.CollisionHandlingOverride=AdjustIfPossibleButAlwaysSpawn
```

## What To Implement

### 1. New MCP Tool: `olive.search_community_blueprints`

**Parameters:**
- `query` (string, required): Search terms like "gun fire reload" or "pickup overlap interact"  
- `type` (string, optional): Filter by asset type — "blueprint", "material", etc. Default: no filter
- `max_results` (integer, optional): 1-10, default 5
- `offset` (integer, optional): Skip this many results for pagination. Default 0. If first results aren't useful, search again with offset=5 to get the next batch.

**Search behavior:**
- Multi-word queries use OR logic: "gun fire reload" → FTS5 query "gun OR fire OR reload"
- Single words pass through directly
- Results prefer UE 5.0+ examples — sort by UE version (5.x first) then by FTS5 rank within version groups
- If type filter provided, add `AND b.type = ?` to the WHERE clause

**SQL query:**
```sql
SELECT b.slug, b.title, b.type, b.ue_version, b.node_count,
       b.functions, b.variables, b.compact, b.url
FROM blueprints_fts fts
JOIN blueprints b ON b.slug = fts.slug
WHERE blueprints_fts MATCH ?
  AND (? IS NULL OR b.type = ?)
ORDER BY 
  CASE WHEN CAST(b.ue_version AS REAL) >= 5.0 THEN 0 ELSE 1 END,
  rank
LIMIT ?
OFFSET ?
```

**Success response format:**
```json
{
  "results": [
    {
      "title": "Gun System",
      "type": "blueprint",
      "ue_version": "4.27",
      "node_count": 71,
      "functions": "SpawnActorFromClass,GetBaseAimRotation",
      "variables": "Ammo,CurrentMag,IsAiming,Reloading",
      "compact": "Flow: InputAction.Pressed → Branch → SpawnActor...",
      "url": "https://blueprintue.com/blueprint/51st8htj/"
    }
  ],
  "count": 5,
  "note": "Community examples from blueprintue.com. Quality varies — use your judgment on which patterns to follow."
}
```

**If db file doesn't exist**, return success with a message (not an error):
```json
{
  "results": [],
  "count": 0,
  "note": "Community blueprint index not found. This is an optional feature — place community_blueprints.db in Content/CommunityBlueprints/."
}
```

### 2. SQLite Integration

UE5 ships with SQLite. Add `"SQLiteCore"` to `PrivateDependencyModuleNames` in `OliveAIEditor.Build.cs`.

Use UE5's SQLite API:
```cpp
#include "SQLiteDatabase.h"
// FSQLiteDatabase to open/query
```

If UE5's built-in SQLite API is problematic, fall back to bundling sqlite3 directly (single .c/.h pair) which is public domain. The db is read-only so only SELECT queries are needed.

**Database path** — follow the same pattern as `LoadRecipeLibrary()`:
```cpp
const FString DbPath = FPaths::Combine(
    FPaths::ProjectPluginsDir(),
    TEXT("UE_Olive_AI_Studio/Content/CommunityBlueprints/community_blueprints.db"));
```

Keep the database connection open after first query (lazy init). Close on `UnregisterAllTools()`.

### 3. Registration — Follow Exact Existing Patterns

**Schema** — Add to `OliveCrossSystemSchemas.h/.cpp`:

In the header, add declaration in the namespace alongside `RecipeGetRecipe()`:
```cpp
// Community Blueprint Search
TSharedPtr<FJsonObject> CommunitySearch();
```

In the .cpp, implement it following the same `MakeSchema`/`MakeProperties`/`AddRequired` helpers already used:
```cpp
TSharedPtr<FJsonObject> CommunitySearch()
{
    TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
    TSharedPtr<FJsonObject> Props = MakeProperties();

    Props->SetObjectField(TEXT("query"),
        OliveBlueprintSchemas::StringProp(TEXT("Search terms (e.g. 'gun fire reload', 'pickup overlap interact', 'health damage')")));
    Props->SetObjectField(TEXT("type"),
        OliveBlueprintSchemas::StringProp(TEXT("Optional: filter by asset type — 'blueprint', 'material', 'pcg'. Omit to search all types.")));
    Props->SetObjectField(TEXT("max_results"),
        OliveBlueprintSchemas::IntProp(TEXT("Maximum results to return (1-10, default 5)"), 5));
    Props->SetObjectField(TEXT("offset"),
        OliveBlueprintSchemas::IntProp(TEXT("Skip this many results for pagination (default 0). Use offset=5 to get the next batch if first results aren't useful."), 0));

    Schema->SetObjectField(TEXT("properties"), Props);
    AddRequired(Schema, { TEXT("query") });
    return Schema;
}
```

**Handler** — Add to `OliveCrossSystemToolHandlers.h/.cpp`:

In the header, add alongside the recipe declarations:
```cpp
// Community Blueprint search
void RegisterCommunityTools();
FOliveToolResult HandleSearchCommunityBlueprints(const TSharedPtr<FJsonObject>& Params);

// SQLite connection for community db (lazy init, read-only)
// Use whatever SQLite handle type is appropriate
```

In `RegisterAllTools()`, add the call:
```cpp
void FOliveCrossSystemToolHandlers::RegisterAllTools()
{
    // ... existing registrations ...
    RegisterCommunityTools();   // <-- ADD THIS
    // ... recipe system ...
}
```

**Tool registration** — follow the exact same pattern as `RegisterRecipeTools()`:
```cpp
void FOliveCrossSystemToolHandlers::RegisterCommunityTools()
{
    FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

    Registry.RegisterTool(
        TEXT("olive.search_community_blueprints"),
        TEXT("Search ~150K community Blueprint examples from blueprintue.com. "
             "Returns compact graph summaries showing node flow, data wires, and defaults. "
             "Results prefer UE 5.0+ examples. Use to see how real developers solved common patterns. "
             "Quality varies — evaluate results and adapt patterns rather than copying directly. "
             "Search multiple times with different terms if initial results aren't relevant, "
             "or use offset to paginate through more results."),
        OliveCrossSystemSchemas::CommunitySearch(),
        FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleSearchCommunityBlueprints),
        {TEXT("crosssystem"), TEXT("read")},
        TEXT("crosssystem")
    );
    RegisteredToolNames.Add(TEXT("olive.search_community_blueprints"));

    UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Registered community blueprint search tool"));
}
```

Note: category is `"crosssystem"` with tags `{"crosssystem", "read"}` — same pattern as `olive.get_recipe`. This ensures it's visible in Blueprint and Auto focus profiles.

### 4. Prompt Update

Add to the end of `Content/SystemPrompts/Worker_Blueprint.txt`, before the file ends, after the Self-Correction section:

```
### Community Reference
Before building complex gameplay systems (weapons, inventory, interaction, AI, movement, etc.), search community examples with olive.search_community_blueprints to see how other developers approached similar patterns. These are community-submitted from blueprintue.com — quality varies, use your judgment. Results prefer UE 5.0+ examples. Search multiple times with different terms if needed, or use offset to paginate through more results.
```

Also add a one-line mention to `Content/SystemPrompts/Knowledge/recipe_routing.txt`:
```
- olive.search_community_blueprints(query) searches ~150K community examples. Use for pattern research before building complex systems.
```

## Files To Modify

1. **`OliveAIEditor.Build.cs`** — Add `"SQLiteCore"` to PrivateDependencyModuleNames
2. **`Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemSchemas.cpp`** — Add `CommunitySearch()` function
3. **`Source/OliveAIEditor/CrossSystem/Public/MCP/OliveCrossSystemSchemas.h`** — Add `CommunitySearch()` declaration  
4. **`Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`** — Add `RegisterCommunityTools()` and `HandleSearchCommunityBlueprints()`
5. **`Source/OliveAIEditor/CrossSystem/Public/MCP/OliveCrossSystemToolHandlers.h`** — Add method declarations and SQLite handle member
6. **`Content/SystemPrompts/Worker_Blueprint.txt`** — Add "Community Reference" section
7. **`Content/SystemPrompts/Knowledge/recipe_routing.txt`** — Add one-line mention

## What NOT To Do

- Don't make the tool required — it's optional reference material, the AI decides when to search
- Don't change how templates or recipes work — this is a separate system
- Don't embed search results into the system prompt automatically
- Don't write to the database — read-only access only
- Don't fail hard if the db file is missing — return an empty result with a helpful message
- Don't add a new tool category — use "crosssystem" to match olive.get_recipe
