# Design: Verify-Before-Write Nudge + Scout Parallelization

**Author:** Architect
**Date:** 2026-03-09
**Status:** Proposed
**Risk level:** Low (prompt-only change + perf optimization in existing code)

---

## Improvement 1: Verify-Before-Write Prompt Nudge

### Root Cause

The Builder (Claude Code CLI running autonomously) writes `plan_json` with hallucinated pin names. From the 08g session log, 10 out of 22 plan_json calls failed (45% failure rate). The most damaging category is **wrong pin names on nodes the Builder has never inspected**:

| Failure | Hallucinated Pin | Actual Pins |
|---------|-----------------|-------------|
| `AttachToComponent` | `AttachType` | `AttachmentRule`, `LocationRule`, `RotationRule`, `ScaleRule` |
| `GetForwardVector` | `InRot` | Only `self` (it's a library function, not rotator-based) |
| `OnComponentBeginOverlap` | `~Normal` | `OtherActor`, `OtherComp`, `OtherBodyIndex`, `bFromSweep`, `SweepResult` |
| `ApplyPointDamage` | `HitInfo` | `HitResult`, `HitBoneName`, etc. |
| `SetSpeed`, `SetMaxSpeed` | N/A (functions don't exist) | Use `set_var` on `InitialSpeed`, `MaxSpeed` properties |

The LLM's training data contains stale or imprecise UE API knowledge. The tool `blueprint.describe_node_type` exists and returns exact pin manifests, but the Builder almost never calls it before writing plan_json.

### Why `describe_node_type` Is Not Sufficient Alone

`describe_node_type` instantiates a UK2Node subclass on a scratch graph and lists its default pins. This works for structural node types (Branch, Sequence, SpawnActor, ComponentBoundEvent) but is **inadequate for function calls** because:

- `UK2Node_CallFunction` with no `function_name` set only shows `self` + exec pins.
- The actual pin layout depends on the resolved UFunction's parameters.
- The Builder would need to call `describe_node_type` AND then check what function-specific pins exist.

However, the Builder already has MCP access to `blueprint.read` (which shows pins on existing nodes) and the error recovery path already includes pin listings (from error-messages-08g Change 2). The real gap is **pre-write verification for known-tricky node categories**.

### Proposed Solution

Add a targeted instruction to Section 5 (Execution directive) of `FormatForPromptInjection()` that nudges the Builder to verify pin names before writing plan_json for specific high-risk node categories. This is a **prompt-only change** -- no new tools, no new infrastructure.

#### Exact Location

File: `Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp`
Function: `FOliveAgentPipelineResult::FormatForPromptInjection()`
Insert point: Between step 4a (write graph logic) and step 4b (template fallback), at line 556.

#### Proposed Text

Insert a new sub-step 4a-prime after step 4a:

```
4. For each function/event:
   a. Write graph logic with apply_plan_json based on the function description in the Build Plan.
      Use `@step.auto` for pin wiring. Do not simplify your design.
   a'. Pin verification: Before writing plan_json that uses overlap events, physics functions
      (ApplyDamage, ApplyPointDamage), attachment functions (AttachToComponent, AttachActorToComponent),
      or movement component setters -- call `blueprint.describe_node_type` or `blueprint.read` to
      confirm exact pin names. Do NOT guess pin names from memory for these categories.
   b. If plan_json fails, check the Reference Templates section ...
   c. Compile after each function ...
```

#### Design Decisions

**Targeted, not universal.** The nudge lists specific node categories rather than saying "always verify." Universal "always describe before writing" would add 1 tool call per plan_json step (potentially 5-10 extra calls per function), slowing the Builder by 30-60 seconds. The targeted approach affects only the 3-4 categories that historically fail.

**Categories selected based on failure data:**

1. **Overlap/collision events** -- `OnComponentBeginOverlap`, `OnComponentHit`, `OnComponentEndOverlap`. Pin names include `SweepResult` (not `HitResult`), `OtherBodyIndex` (not `BodyIndex`), and NO `~Normal` pin.
2. **Damage functions** -- `ApplyDamage`, `ApplyPointDamage`, `ApplyRadialDamage`. Complex signatures with by-ref `FHitResult` parameter, damage type class, instigator controller vs causer actor.
3. **Attachment functions** -- `AttachToComponent`, `AttachActorToActor`, `DetachFromComponent`. UE5 uses `EAttachmentRule` enum pins, not the old `AttachType`.
4. **Movement component property access** -- `ProjectileMovementComponent`, `CharacterMovementComponent`. These have UPROPERTY fields that are NOT BlueprintCallable functions (e.g., `InitialSpeed` is a property, not a `SetSpeed()` function).

**Suggestion, not rule.** The nudge says "call describe_node_type or blueprint.read to confirm" -- it does not mandate a specific tool or block execution. The Builder retains agency to skip verification when confident.

**No `bWantsSimpleLogic` gate.** Even simple/stub logic can fail on wrong pin names. The nudge applies regardless of complexity level.

#### File Changes

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp` | Insert 4 lines in `FormatForPromptInjection()` after line 555 (step 4a) |

#### Exact Diff (Conceptual)

In `FormatForPromptInjection()`, the non-simple-logic else branch (line 552-558) currently reads:

```cpp
Output += TEXT("4. For each function/event:\n");
Output += TEXT("   a. Write graph logic with apply_plan_json based on the function description in the Build Plan. ");
Output += TEXT("Use `@step.auto` for pin wiring. Do not simplify your design.\n");
Output += TEXT("   b. If plan_json fails, check the Reference Templates section -- call ");
Output += TEXT("`blueprint.get_template(template_id, pattern=\"FunctionName\")` for the specific failed function only.\n");
Output += TEXT("   c. Compile after each function. Fix the first error before moving on.\n");
```

Change to:

```cpp
Output += TEXT("4. For each function/event:\n");
Output += TEXT("   a. Write graph logic with apply_plan_json based on the function description in the Build Plan. ");
Output += TEXT("Use `@step.auto` for pin wiring. Do not simplify your design.\n");
Output += TEXT("   VERIFY FIRST: Before writing plan_json that calls overlap events (OnComponentBeginOverlap, ");
Output += TEXT("OnComponentHit), damage functions (ApplyDamage, ApplyPointDamage), attachment functions ");
Output += TEXT("(AttachToComponent), or movement component properties -- call `blueprint.describe_node_type` ");
Output += TEXT("to confirm exact pin names. These functions have non-obvious pin names that differ from common assumptions.\n");
Output += TEXT("   b. If plan_json fails, check the Reference Templates section -- call ");
Output += TEXT("`blueprint.get_template(template_id, pattern=\"FunctionName\")` for the specific failed function only.\n");
Output += TEXT("   c. Compile after each function. Fix the first error before moving on.\n");
```

### Tradeoffs

**Pro:** Zero runtime cost. No new tools, no new files, no new dependencies. Directly addresses the #1 failure category (45% of plan_json failures in 08g were pin name hallucinations).

**Con:** Prompt-based nudging is probabilistic. The Builder may still skip verification. The list of categories is hand-curated and may miss future failure categories.

**Risk:** Very low. Worst case, the nudge is ignored and we're no worse off. It adds ~200 chars to the prompt (~50 tokens), negligible.

### Expected Impact

If the Builder heeds the nudge for even 50% of the target categories, the 08g pin-name failures would drop from 6 (categories A, B, C, F) to approximately 3. Plan_json success rate would improve from 55% to approximately 68%.

---

## Improvement 2: Scout Phase Search Parallelization

### Root Cause

The Scout phase takes 15.2 seconds in the 08g run. The dominant cost is `RunDiscoveryPass()` which:

1. Calls `GenerateSearchQueries(UserMessage)` -- 1 LLM call for keyword expansion (or basic fallback). This takes ~3-5 seconds when LLM is enabled.
2. Iterates over up to 5 search queries **sequentially**, running TWO searches per query:
   - `FOliveTemplateSystem::Get().SearchTemplates(Query, 10)` -- in-memory inverted index, ~1ms per query
   - `FOliveToolRegistry::Get().ExecuteTool("olive.search_community_blueprints", ...)` -- SQLite FTS query, ~1-3 seconds per query (disk I/O + FTS ranking)

The template search is negligible. The community blueprint SQLite queries dominate: 5 queries x ~2s each = ~10s. These queries are **completely independent** -- query 2 does not depend on query 1's results. The merge happens after all queries complete.

### Architecture Analysis

**Current call chain:**
```
RunScout()
  -> RunDiscoveryPass()                          [OliveUtilityModel.cpp:768]
       -> GenerateSearchQueries()                [sequential, LLM call]
       -> for each Query in SearchQueries:       [sequential loop, line 803]
            -> SearchTemplates(Query, 10)         [in-memory, fast]
            -> ExecuteTool("olive.search_community_blueprints")  [SQLite, slow]
       -> Score + sort + trim                    [CPU, fast]
```

**Thread safety constraints:**

1. **`SearchTemplates()`** -- reads from `FOliveTemplateSystem` and `FOliveLibraryIndex`. Both are initialized at startup and read-only after that. The inverted index (`InvertedIndex` TMap) is populated in `Initialize()` and never modified. **Thread-safe for concurrent reads.**

2. **`ExecuteTool("olive.search_community_blueprints")`** -- routes through `FOliveToolRegistry::ExecuteTool()`, which acquires `FRWLock` for tool lookup (read lock), then calls the handler. The handler accesses `CommunityDb` (a `TSharedPtr<FSQLiteDatabase>`) which is a member of `FOliveCrossSystemToolHandlers`. **SQLite is NOT safe for concurrent queries on a single connection from multiple threads.** However, `ExecuteTool` is designed to run on the game thread, and `RunDiscoveryPass` already runs on the game thread.

3. **`MergedEntries` TMap** -- accumulates results across queries. Not thread-safe for concurrent writes.

### Proposed Solution

Refactor the search loop in `RunDiscoveryPass()` to separate the per-query work into two phases:

**Phase A (parallelizable):** Run all `SearchTemplates()` calls concurrently. These are pure in-memory reads with no shared mutable state.

**Phase B (sequential but batched):** Run all `olive.search_community_blueprints` queries. These use a shared SQLite connection and cannot be parallelized without opening separate connections. However, the queries CAN be **batched into a single SQL call** using OR in the FTS MATCH clause.

#### Option A: Batch Community Queries (Recommended)

Instead of 5 separate `ExecuteTool("olive.search_community_blueprints")` calls (each opening a prepared statement, executing, collecting results), execute a single FTS query with all search terms OR'd together.

**Current:** 5 calls x `MATCH 'bow'`, `MATCH 'arrow projectile'`, `MATCH 'ranged weapon'`, etc.
**Proposed:** 1 call x `MATCH 'bow OR "arrow projectile" OR "ranged weapon" OR ...'`

This requires a new internal method on `FOliveCrossSystemToolHandlers` (or a new parameter on the existing tool) that accepts multiple queries and returns merged results.

**Implementation approach -- new private method, not a new tool:**

Add a private method to `FOliveUtilityModel`:

```cpp
// In OliveUtilityModel.cpp, new helper alongside RunDiscoveryPass()
static TArray<TSharedPtr<FJsonValue>> BatchCommunitySearch(
    const TArray<FString>& Queries, int32 MaxResultsPerQuery);
```

This method:
1. Builds a single FTS MATCH string: `query1 OR query2 OR query3 ...`
2. Calls `ExecuteTool("olive.search_community_blueprints")` once with the combined query
3. Returns merged results

**Why not open parallel SQLite connections?** Opening multiple read-only connections to the same database IS safe in SQLite (WAL mode or read-only). But it adds complexity (connection pool, lifetime management) for diminishing returns. A single batched FTS query is faster than 5 sequential queries and simpler to implement.

**Why not use `ParallelFor` / `Async()`?** The searches happen on the game thread during the pipeline. UE's task graph requires a sync point, and `RunDiscoveryPass` is called synchronously from `RunScout`. Using async tasks + `FEvent` wait would work but adds complexity. The batch approach achieves the same speedup with less code.

#### Expected Timing

| Phase | Current | Proposed |
|-------|---------|----------|
| GenerateSearchQueries (LLM) | ~3-5s | ~3-5s (unchanged) |
| Template searches (5x in-memory) | ~5ms | ~5ms (unchanged) |
| Community searches (5x SQLite FTS) | ~10s | ~2-3s (1 batched query) |
| Merge + score | ~1ms | ~1ms (unchanged) |
| **Total RunDiscoveryPass** | **~13-15s** | **~5-8s** |

**Realistic Scout time reduction: 15s -> 8-10s** (saving 5-7 seconds).

### File Changes

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Private/Services/OliveUtilityModel.cpp` | Refactor the `for (Query : SearchQueries)` loop (line 803-952) to batch community searches |

### Detailed Implementation

**Step 1:** Extract the template search into its own loop (runs first, fast):

```cpp
// Phase 1: Template searches (in-memory, all queries)
for (const FString& Query : Result.SearchQueries)
{
    TArray<TSharedPtr<FJsonObject>> SearchResults =
        FOliveTemplateSystem::Get().SearchTemplates(Query, 10);
    // ... merge into MergedEntries as before ...
}
```

**Step 2:** Batch the community search. Build a combined FTS query string from all search queries, then make ONE `ExecuteTool` call:

```cpp
// Phase 2: Community blueprint search (single batched FTS query)
{
    // Build OR'd FTS query: "bow" OR "arrow projectile" OR "ranged weapon"
    FString CombinedFtsQuery;
    for (int32 i = 0; i < Result.SearchQueries.Num(); i++)
    {
        if (i > 0) CombinedFtsQuery += TEXT(" OR ");
        // Quote multi-word queries for FTS phrase matching
        const FString& Q = Result.SearchQueries[i];
        if (Q.Contains(TEXT(" ")))
        {
            CombinedFtsQuery += TEXT("\"") + Q + TEXT("\"");
        }
        else
        {
            CombinedFtsQuery += Q;
        }
    }

    TSharedPtr<FJsonObject> CommunityParams = MakeShared<FJsonObject>();
    CommunityParams->SetStringField(TEXT("query"), CombinedFtsQuery);
    CommunityParams->SetNumberField(TEXT("max_results"), 10);

    FOliveToolResult ToolResult =
        FOliveToolRegistry::Get().ExecuteTool(
            TEXT("olive.search_community_blueprints"), CommunityParams);

    if (ToolResult.bSuccess && ToolResult.Data.IsValid())
    {
        // ... merge community results into MergedEntries (same as current per-query merge) ...
    }
}
```

**Step 3:** The `QueryHitCounts` tracking changes slightly. Currently, a community result that matches 3 different queries gets 3 hits (boosting its score). With a single batched query, each community result gets 1 hit. This is acceptable -- community results already score lower than library results (base score 3 vs 10), and the multi-hit boost was already unreliable since FTS `OR` returns each result once regardless of how many terms matched.

If preserving per-query hit counting matters, the batched approach can still approximate it by checking which original queries each result's title/description contains (post-hoc token matching). But this adds complexity for marginal scoring benefit.

### Edge Cases

1. **FTS syntax edge case:** If a query contains FTS special characters (`"`, `*`, `OR`, `AND`, `NOT`, `NEAR`), they need escaping. Current code doesn't escape either, so this is pre-existing. The risk is low since `GenerateSearchQueries` produces natural language queries.

2. **Empty queries array:** If `GenerateSearchQueries` returns 0 queries, the loop is skipped entirely (line 793-797). No change needed.

3. **Community DB not available:** The existing `ExecuteTool` call gracefully returns empty results when the DB is not found. This behavior is preserved with the batched approach.

4. **Combined query too long:** FTS has no practical length limit for OR'd queries. 5 short phrases is well within bounds.

### Tradeoffs

**Pro:** 5-7 second reduction in Scout time. Simpler code (1 loop + 1 call instead of nested loop with 2 calls). No threading complexity.

**Con:** Loses per-query hit counting for community results (minor scoring impact). The combined FTS query may return slightly different results than 5 individual queries (FTS `OR` ranking differs from running 5 separate ranked queries and merging).

**Risk:** Low. The community search results are used for discovery/reference only -- they don't directly affect the build plan. Slightly different ranking order has minimal downstream impact.

---

## Implementation Order

1. **Improvement 1 (verify-before-write nudge) -- implement first.**
   - Prompt-only change, 4 lines of C++ string literals
   - Can be tested immediately on next build run
   - No risk of regression
   - **Assignment: Junior coder (coder_sonnet)** -- straightforward string insertion at a known location

2. **Improvement 2 (Scout batch search) -- implement second.**
   - Requires restructuring the `RunDiscoveryPass` loop
   - Needs testing to verify FTS `OR` syntax works correctly with the community DB
   - Should verify scoring behavior is acceptable after batching
   - **Assignment: Senior coder (coder/Opus)** -- requires understanding FTS semantics, loop restructuring, and edge case handling

---

## Summary for Coders

### Junior Task (Improvement 1)

**File:** `Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp`
**Function:** `FOliveAgentPipelineResult::FormatForPromptInjection()`
**Location:** After line 555 (`Use @step.auto for pin wiring. Do not simplify your design.\n"`), before step 4b.
**Action:** Insert 3 `Output += TEXT(...)` lines adding a "VERIFY FIRST" sub-instruction about overlap events, damage functions, attachment functions, and movement component properties. See "Exact Diff (Conceptual)" section above for the text.
**Test:** Build and run a bow-and-arrow test. Check that the Builder calls `blueprint.describe_node_type` before writing plan_json for overlap/damage nodes. Compare plan_json success rate to 08g baseline (55%).

### Senior Task (Improvement 2)

**File:** `Source/OliveAIEditor/Private/Services/OliveUtilityModel.cpp`
**Function:** `FOliveUtilityModel::RunDiscoveryPass()`
**Location:** Lines 803-952 (the `for (Query : SearchQueries)` loop).
**Action:** Split into two phases: (1) template search loop (keep sequential, fast), (2) batched community FTS query with OR'd terms. See "Detailed Implementation" section above.
**Test:** Add timing logs before/after the community search. Run a discovery pass and compare elapsed time vs baseline (~15s). Target: under 10s.
