# Plan: Community Blueprint Browsing — Multi-Example Selection

## Problem

The AI searches community blueprints 2-3 times, takes whatever comes back, and commits to the first pattern it sees. It doesn't compare multiple examples and copies whatever pattern the first result uses — even if better examples exist deeper in the results.

## Current Behavior

1. AI calls `olive.search_community_blueprints` 1-3 times
2. Gets back full blueprint details (high token cost per result)
3. Picks the first relevant-looking result
4. Copies that pattern verbatim
5. Never paginates or tries different search terms

## Desired Behavior

1. AI does a lightweight scan across many results
2. Reviews the summaries and decides which look most useful
3. Pulls full details on up to 5 of them
4. Uses those as reference material when designing

## Implementation: Two-Phase Search

### Phase 1: Browse Mode

Add a `mode` parameter to the existing tool:
EXAMPLE:

```json
{
  "tool": "olive.search_community_blueprints",
  "params": {
    "query": "door interactable",
    "mode": "browse",
    "max_results": 20
  }
}
```

Browse mode returns a compact summary per result — just the facts, no quality scoring or recommendations:
- Title/name
- Description (first ~100 chars)
- Tags
- Node count
- Blueprint type

Token cost: ~50-80 tokens per result vs ~500-1000+ for full details.

### Phase 2: Detail Fetch

AI reviews the browse results, picks up to 5 it wants to study, then fetches full details:
EXAMPLE:

```json
{
  "tool": "olive.search_community_blueprints",
  "params": {
    "ids": ["bp_001", "bp_003", "bp_007", "bp_012", "bp_015"],
    "mode": "detail"
  }
}
```

Returns full blueprint JSON for the selected entries.

### Browse Response Format

```json
{
  "results": [
    {
      "id": "bp_door_001",
      "title": "Smooth Door with Timeline",
      "description": "Door that opens smoothly using Timeline and Lerp...",
      "tags": ["door", "interactable", "timeline", "smooth"],
      "node_count": 24,
      "blueprint_type": "Actor"
    }
  ],
  "total_matches": 47,
  "showing": 20
}
```

The AI decides what's relevant and what's worth pulling full details on. No quality indicators, no recommendations, no scoring.

## Implementation Location

**File:** `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`

The existing `olive.search_community_blueprints` handler around lines 73698-73924.

Changes:
1. Add `mode` parameter to schema (default: "detail" for backwards compatibility)
2. Add browse mode query that returns compact summaries
3. Add `ids` parameter for fetching specific entries in detail mode

## Priority

Medium-high. Directly impacts design quality by giving the AI a real selection of examples to learn from instead of copying the first result.

## Success Criteria

- AI browses many results before committing to a pattern
- AI fetches full details on up to 5 results it chose
- AI references multiple examples when designing