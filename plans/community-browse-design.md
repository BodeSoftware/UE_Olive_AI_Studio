# Community Blueprint Browse Improvement -- Design Document

**Goal:** When the AI calls `olive.search_community_blueprints`, make it browse more results instead of committing to the first decent-looking match. Three changes: total match count in response, dynamic note text, prompt guidance for varied search terms.

**Scope:** 1 C++ file, 1 schema file (no change needed), 2 prompt files. No header changes. No new files. No new error codes. No Build.cs changes.

---

## Change 1: Add total match count to search response

### File
`Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`

### Current code (lines 1549-1594)
The handler builds the main SELECT query (lines 1552-1572), prepares a statement (line 1574), binds parameters (lines 1587-1594), and executes (line 1599). There is no COUNT query.

### Design

Insert a COUNT query **before** the main SELECT query. This runs a separate prepared statement that uses the same FTS MATCH + optional type filter, but returns `SELECT COUNT(*)` instead of full rows. The count query does not need LIMIT/OFFSET.

#### Insertion point: After line 1550 (`const bool bHasTypeFilter = !TypeFilter.IsEmpty();`), before line 1552 (`const TCHAR* SqlWithoutType = TEXT(`)

Insert a block that:
1. Defines two COUNT SQL strings (with and without type filter)
2. Prepares a count statement
3. Binds the FTS query (and type filter if present)
4. Executes it, reading a single `int64` from column 0
5. Stores the result in a local `int64 TotalMatches = 0`
6. If the count statement fails to prepare or execute, sets `TotalMatches = -1` (unknown) rather than failing the entire tool call -- the count is supplementary information

#### Count SQL strings

```cpp
// --- Count total matches (before applying LIMIT/OFFSET) ---
int64 TotalMatches = 0;
{
    const TCHAR* CountSqlWithoutType = TEXT(
        "SELECT COUNT(*) "
        "FROM blueprints_fts fts "
        "JOIN blueprints b ON b.slug = fts.slug "
        "WHERE blueprints_fts MATCH ?");

    const TCHAR* CountSqlWithType = TEXT(
        "SELECT COUNT(*) "
        "FROM blueprints_fts fts "
        "JOIN blueprints b ON b.slug = fts.slug "
        "WHERE blueprints_fts MATCH ? AND b.type = ?");

    FSQLitePreparedStatement CountStmt = CommunityDb->PrepareStatement(
        bHasTypeFilter ? CountSqlWithType : CountSqlWithoutType,
        ESQLitePreparedStatementFlags::None);

    if (CountStmt.IsValid())
    {
        int32 CountBindIdx = 1;
        CountStmt.SetBindingValueByIndex(CountBindIdx++, FtsQuery);
        if (bHasTypeFilter)
        {
            CountStmt.SetBindingValueByIndex(CountBindIdx++, TypeFilter);
        }

        CountStmt.Execute([&TotalMatches](const FSQLitePreparedStatement& Row) -> ESQLitePreparedStatementExecuteRowResult
        {
            Row.GetColumnValueByIndex(0, TotalMatches);
            return ESQLitePreparedStatementExecuteRowResult::Stop;
        });
    }
    // If count fails, TotalMatches stays 0 -- we'll omit it from the response
}
```

**Key decisions:**
- Uses a scoped block `{}` so the `CountStmt` is destroyed before the main statement is prepared. Only one prepared statement should be active at a time to avoid any potential resource contention.
- `GetColumnValueByIndex(0, int64&)` is a confirmed overload in UE 5.5's `FSQLitePreparedStatement` (line 241 of `SQLitePreparedStatement.h`).
- Returns `Stop` after the first row since COUNT(*) always returns exactly one row.
- If the count statement fails to prepare, `TotalMatches` stays 0. The response builder (Change 1b below) will handle this gracefully.

#### Response JSON additions (modifying lines 1643-1650)

After the existing `Response->SetNumberField(TEXT("count"), Results.Num());` on line 1646, add two new fields:

```cpp
Response->SetNumberField(TEXT("count"), Results.Num());

// Total matches and pagination hint
if (TotalMatches > 0)
{
    Response->SetNumberField(TEXT("total_matches"), static_cast<double>(TotalMatches));
    Response->SetBoolField(TEXT("has_more"), (Offset + Results.Num()) < TotalMatches);
}
```

**Why `static_cast<double>`:** `SetNumberField` takes `double`. `int64` to `double` is a narrowing conversion that some compilers warn about. For counts under ~9 quadrillion the precision is exact, and community BP databases will never approach that.

**Why guard with `TotalMatches > 0`:** If the count query failed (TotalMatches stayed 0) or genuinely returned 0, we omit `total_matches` and `has_more` entirely rather than showing misleading data. When TotalMatches is 0 and results are also 0, the existing empty-result path handles it naturally.

---

## Change 2: Replace static response note with dynamic one

### File
`Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`

### Current code (lines 1647-1648)
```cpp
Response->SetStringField(TEXT("note"),
    TEXT("Community examples from blueprintue.com. Quality varies — use your judgment on which patterns to follow."));
```

### Replacement

Replace the two-line `SetStringField` call with:

```cpp
// Dynamic note with pagination hint when more results exist
const bool bHasMore = TotalMatches > 0 && (Offset + Results.Num()) < TotalMatches;
if (bHasMore)
{
    Response->SetStringField(TEXT("note"),
        FString::Printf(TEXT("Showing %d of %lld matches. Quality varies — browse 2-3 pages before committing to a pattern. Use offset=%d for next page."),
            Results.Num(), TotalMatches, Offset + Results.Num()));
}
else
{
    Response->SetStringField(TEXT("note"),
        TEXT("Community examples from blueprintue.com. Quality varies — use your judgment on which patterns to follow."));
}
```

**Format specifiers:** `%d` for `int32` (`Results.Num()`), `%lld` for `int64` (`TotalMatches`), `%d` for `int32` (`Offset + Results.Num()`).

**Behavior matrix:**

| TotalMatches | Results | has_more | Note text |
|---|---|---|---|
| 47 | 5 | true | "Showing 5 of 47 matches. Quality varies -- browse 2-3 pages before committing to a pattern. Use offset=5 for next page." |
| 47 | 5 (offset=45) | false | Static quality note (no pagination hint) |
| 3 | 3 | false | Static quality note |
| 0 | 0 | false | Static quality note |
| -1 (query failed) | 0 | false | Static quality note (TotalMatches guard prevents bHasMore) |
| 0 (count failed) | 5 | false | Static quality note (TotalMatches=0 guard) |

**Note on the "count failed but results exist" edge case:** If the count query fails but the main query succeeds, `TotalMatches` will be 0 while `Results.Num()` could be >0. The `TotalMatches > 0` guard in `bHasMore` prevents showing "Showing 5 of 0 matches." The response will simply have no `total_matches`/`has_more` fields and show the static note. This is acceptable degradation.

---

## Change 3: Prompt guidance for trying different search terms

### File 1: `Content/SystemPrompts/Worker_Blueprint.txt`

**Current text (lines 79-80):**
```
### Community Reference
Before building complex gameplay systems (weapons, inventory, interaction, AI, movement, etc.), search community examples with olive.search_community_blueprints to see how other developers approached similar patterns. These are community-submitted from blueprintue.com — quality varies, use your judgment. Results prefer UE 5.0+ examples. Search multiple times with different terms if needed, or use offset to paginate through more results.
```

**New text (replace lines 79-80):**
```
### Community Reference
Before building complex gameplay systems (weapons, inventory, interaction, AI, movement, etc.), search community examples with olive.search_community_blueprints to see how other developers approached similar patterns. These are community-submitted from blueprintue.com — quality varies, use your judgment. Results prefer UE 5.0+ examples. Search multiple times with different terms if needed, or use offset to paginate through more results. Try different search terms if the first results don't match well — the same pattern can have many names in the community.
```

The only change is appending the sentence `Try different search terms if the first results don't match well — the same pattern can have many names in the community.` to the end of the existing paragraph on line 80.

### File 2: `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**Current text (lines 15-16):**
```
1. Check if a template fits → blueprint.create with template_id
2. Search community examples → olive.search_community_blueprints(query). Quality varies — use your judgment and improvise as needed.
```

**And lines 20-21:**
```
1. project.search (find path) → blueprint.read
2. Search community examples → olive.search_community_blueprints(query). Quality varies — use your judgment and improvise as needed.
```

**New text for line 16:**
```
2. Search community examples → olive.search_community_blueprints(query). Quality varies — use your judgment and improvise as needed. Try different search terms if the first results don't match well.
```

**New text for line 21:**
```
2. Search community examples → olive.search_community_blueprints(query). Quality varies — use your judgment and improvise as needed. Try different search terms if the first results don't match well.
```

Append `Try different search terms if the first results don't match well.` to both occurrences. Shorter than the Worker_Blueprint version because cli_blueprint is a compact reference document.

---

## Schema: No changes needed

The `CommunitySearch()` schema in `OliveCrossSystemSchemas.cpp` (lines 318-335) defines the *input* schema. The new `total_matches` and `has_more` fields are *output* fields in the response JSON. MCP tool schemas only define input parameters, so no schema changes are required.

---

## Files changed summary

| File | Change |
|---|---|
| `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp` | Add COUNT query + dynamic note (Changes 1 & 2) |
| `Content/SystemPrompts/Worker_Blueprint.txt` | Append search-term guidance sentence (Change 3) |
| `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | Append search-term guidance to both workflow steps (Change 3) |

No header changes. No new files. No Build.cs changes. No new error codes.

---

## Implementation order

1. **Change 1+2 together** (C++ file) -- The COUNT query and dynamic note are in the same function and depend on the same `TotalMatches` variable. Implement as a single edit session.
2. **Change 3** (prompt files) -- Two simple text appends. Can be done in parallel with Change 1+2 or after.

---

## Edge cases and error handling

| Scenario | Behavior |
|---|---|
| COUNT query fails to prepare | `TotalMatches` stays 0. No `total_matches`/`has_more` in response. Static note shown. |
| COUNT query returns 0 | No `total_matches`/`has_more` in response (guarded by `TotalMatches > 0`). Static note shown. |
| COUNT returns 0 but main query returns rows | Should not happen (same WHERE clause), but `TotalMatches > 0` guard prevents showing "0 of 0." Static note shown. |
| Database not available | Handled by existing early return on line 1523-1533. No change needed. |
| Main query fails | Handled by existing error on line 1636-1641. No change needed. |
| `Results.Num()` exceeds `TotalMatches` | Should not happen, but `has_more` would be `false` (negative comparison). Correct behavior. |
| Very large `TotalMatches` (>2^53) | `int64` to `double` precision loss. Irrelevant for real data (database has ~150K entries). |

---

## Testing

Manual verification:
1. Search with a broad term (e.g., "gun") -- should show `total_matches` > `count` and `has_more: true` with pagination hint in note
2. Search with a narrow term that returns fewer than `max_results` -- should show `has_more: false` with static note
3. Search with offset that exhausts results -- should show `has_more: false` with static note
4. Search with no database file present -- existing behavior unchanged (empty results, "index not found" note)
