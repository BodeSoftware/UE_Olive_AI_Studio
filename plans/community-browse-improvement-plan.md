# Plan: Improve Community Blueprint Browsing

## Goal
When the AI calls `olive.search_community_blueprints`, make it browse more results instead of committing to the first decent-looking match. This is NOT about forcing the AI to use the tool — only improving behavior when it already calls it.

## Context
- The search tool returns up to 5 results per call (max 10) with pagination via `offset`
- The AI currently grabs the first good-looking result and moves on
- It has no idea how many total matches exist, so it doesn't know browsing is worthwhile
- All changes must preserve AI freedom — nudges, not restrictions

## Changes

### 1. Add total match count to search response
**File:** `Source/OliveAIEditor/CrossSystem/Private/OliveCrossSystemToolHandlers.cpp`

In the `CommunitySearch()` function (around line 73698), before the main SELECT query:
- Run a `SELECT COUNT(*)` using the same FTS MATCH and type filter conditions
- Store the total count

In the response JSON (around line 73918), add two new fields:
- `"total_matches"` — set to the result of the COUNT query
- `"has_more"` — set to `true` when `offset + results.Num() < total_matches`, otherwise `false`

### 2. Replace static response note with dynamic one
**File:** `Source/OliveAIEditor/CrossSystem/Private/OliveCrossSystemToolHandlers.cpp`

Replace the hardcoded note (lines 73921-73922):
```cpp
Response->SetStringField(TEXT("note"),
    TEXT("Community examples from blueprintue.com. Quality varies — use your judgment on which patterns to follow."));
```

With a dynamic `FString::Printf` that produces:
```
"Showing {count} of {total_matches} matches. Quality varies widely — don't commit to the first good-looking result. Browse 2-3 pages to compare approaches (if available). Use offset={offset + count} for next page."
```

When `has_more` is false, simplify to just the quality note without the pagination hint.

### 3. Add prompt guidance for trying different search terms
**Files:**
- `Content/SystemPrompts/Worker_Blueprint.txt`
- `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

Add this sentence to the existing community search guidance in both files:
```
Try different search terms if the first results don't match well — the same pattern can have many names in the community.
```

