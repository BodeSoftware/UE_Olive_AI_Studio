# Olive AI — Blueprint Issues & Efficiency Analysis

> **Date:** 2026-02-23  
> **Scope:** Blueprint creation succeeds but modifications fail; overall process is slow and token-heavy

---

## Summary

After analyzing the full codebase I've identified **3 critical bugs** causing modification failures, and **4 architectural bottlenecks** causing the slow/inefficient workflow. Below is a prioritized breakdown with concrete code fixes.

---

## Part 1: Modification Failures (Why Changes Break)

### Issue 1: Plan-First Routing Blocks Modification Attempts Too Early

**Root Cause:** `PlanFirstGraphRoutingThreshold` defaults to **3**, meaning after just 3 granular graph tool calls (`add_node`, `connect_pins`, `set_pin_default`), the system returns `ROUTE_PLAN_REQUIRED` and blocks further edits — even for simple one-node modifications.

**Why This Kills Modifications:** When the AI tries to *modify* an existing blueprint, it typically needs to:
1. `blueprint.read` (to discover existing nodes/pins)  
2. `blueprint.add_node` (the new node)  
3. `blueprint.connect_pins` (wire it in)  

That's already 2 granular graph calls. One more `set_pin_default` and it gets blocked mid-operation. The routing counter also **doesn't reset between user turns** because `GBlueprintRoutingStatsByContext` is keyed per-session, not per-turn.

**Where in Code:** `OliveToolRegistry.cpp` — `ExecuteTool()` routing logic:
```cpp
const int32 NextCount = Stats.GranularGraphCalls + 1;
if (Stats.PlanCalls == 0 && NextCount >= Threshold)
{
    // Returns ROUTE_PLAN_REQUIRED error
}
```

**Fix:**
```cpp
// In OliveToolRegistry.cpp — ExecuteTool(), inside the routing block:

// FIX 1: Don't count read-only tools toward the granular threshold
if (bIsGranularGraphTool && !IsBlueprintReadTool(Name))
{
    // ... existing threshold logic
}

// FIX 2: Reset routing stats at the start of each new user turn.
// Add to ConversationManager::SendUserMessage():
void FOliveConversationManager::SendUserMessage(const FString& Message)
{
    // Reset per-turn routing stats
    {
        FScopeLock Lock(&GBlueprintRoutingStatsLock);
        FString ContextKey = GetRoutingContextKey();
        GBlueprintRoutingStatsByContext.Remove(ContextKey);
    }
    // ... existing code
}
```

Also raise the default threshold from 3 to **8** in `OliveAISettings.h`:
```cpp
int32 PlanFirstGraphRoutingThreshold = 8; // was 3
```

---

### Issue 2: Plan Executor Has No "Delta/Modify" Mode — Only "Create From Scratch"

**Root Cause:** `blueprint.apply_plan_json` / `FOlivePlanExecutor::Execute()` assumes a **clean graph**. Every step is a `PhaseCreateNodes` operation. There is no concept of:
- Referencing **existing** nodes by their node_id  
- Removing/replacing a subset of nodes  
- Inserting new nodes into an existing exec chain  

When the AI tries to use `apply_plan_json` to modify a blueprint, it either recreates duplicate nodes or fails because it can't reference existing wiring.

**Where in Code:** `OlivePlanExecutor.cpp` — all 5 phases assume `StepToNodeMap` starts empty and only maps newly-created step IDs → node IDs. There's no phase that ingests existing node IDs.

**Fix — Add `existing_nodes` support to the plan schema:**

In `BlueprintPlanIR.h`, add to `FOliveIRBlueprintPlan`:
```cpp
/** Optional map of existing node IDs that steps can reference via @ref.
 *  Allows plans to wire into pre-existing graph nodes without recreating them.
 *  Key = a virtual step_id, Value = the real UEdGraphNode GUID string. */
UPROPERTY()
TMap<FString, FString> ExistingNodes;
```

In `OlivePlanExecutor.cpp`, seed the context map before Phase 1:
```cpp
FOliveIRBlueprintPlanResult FOlivePlanExecutor::Execute(...)
{
    FOlivePlanExecutionContext Context;
    
    // Seed context with existing nodes so @ref wiring can target them
    for (const auto& Pair : Plan.ExistingNodes)
    {
        Context.StepToNodeMap.Add(Pair.Key, Pair.Value);
        // Also build pin manifests for existing nodes so wiring works
        if (UEdGraphNode* Node = FindNodeByGuid(ExecutionGraph, Pair.Value))
        {
            Context.StepManifests.Add(Pair.Key, BuildManifestForNode(Node));
        }
    }
    
    // Phase 1 only creates steps NOT in ExistingNodes
    PhaseCreateNodes(Plan, Context, ...);
    // ... rest unchanged
}
```

In `OliveBlueprintPlanLowerer.cpp`, parse the `existing_nodes` field from the JSON the AI sends.

---

### Issue 3: Self-Correction Loop Gets Stuck on Partial Wiring Failures

**Root Cause:** When `apply_plan_json` returns `bPartial = true` (all nodes created, some wiring failed), the self-correction hint tells the AI to *"use blueprint.read then granular connect_pins to fix"*. But:

1. The AI calls `blueprint.read` → this returns a large JSON  
2. The AI calls `connect_pins` → may hit `ROUTE_PLAN_REQUIRED` (Issue 1)  
3. Even if allowed, the AI often gets wrong pin names because `blueprint.read` returns display names while `connect_pins` needs internal names  
4. The self-correction policy (`FOliveSelfCorrectionPolicy::Evaluate`) counts these failures against `MaxCorrectionReprompts = 2`, and after 2 failures, stops with an incomplete blueprint

**Where in Code:** `OliveSelfCorrectionPolicy.cpp` and `OliveConversationManager.cpp`:
```cpp
static constexpr int32 MaxCorrectionReprompts = 2;
```

**Fix — Multi-pronged:**

**A)** In the `apply_plan_json` result handler, when `bPartial` is true, automatically retry failed wiring internally (up to 1 retry with fuzzy pin matching) before returning to the AI:
```cpp
// In OlivePlanExecutor.cpp, after Phase 4 (wire data):
if (Context.FailedConnectionCount > 0)
{
    // Phase 4b: Retry failed connections with fuzzy matching
    RetryFailedConnectionsWithFuzzyMatch(Plan, Context);
}
```

**B)** The pin manifest returned in the result should include **both** display name and internal name for each pin. Currently `OlivePinManifest::ToJson()` only outputs the internal name. Add:
```cpp
PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
PinObj->SetStringField(TEXT("display_name"), Pin->PinFriendlyName.IsEmpty() 
    ? Pin->PinName.ToString() 
    : Pin->PinFriendlyName.ToString());
```

**C)** Raise `MaxCorrectionReprompts` to 3, and exempt wiring-fix attempts from the routing threshold:
```cpp
static constexpr int32 MaxCorrectionReprompts = 3;
```

---

## Part 2: Slow & Inefficient Process

### Bottleneck 1: Chatty Round-Trip Pattern (Read → Plan → Preview → Apply)

**Problem:** The current blueprint creation flow requires **4 sequential LLM round-trips minimum:**

1. **Turn 1:** AI calls `blueprint.create` → returns asset path  
2. **Turn 2:** AI calls `blueprint.preview_plan_json` → returns preview fingerprint  
3. **Turn 3:** AI calls `blueprint.apply_plan_json` → returns step_to_node_map  
4. **Turn 4:** AI calls `blueprint.compile` → returns compile result  
5. *(Turn 5+ if errors:* AI calls `blueprint.read` → then fix tools → recompile)  

Each round-trip means: streaming response from the API, parsing tool calls, executing, serializing results, re-assembling the full conversation history, and sending it all back. For Anthropic at ~$3/$15 per million tokens, and a system prompt + history that easily reaches 15-20K tokens, you're looking at **$0.05-0.15+ per blueprint** and 30-60 seconds of wall time.

**Fix — Introduce a `blueprint.create_with_plan` Composite Tool:**

Register a single tool that accepts `path`, `parent_class`, AND `plan_json` in one call. Internally it chains: create → resolve → lower → execute → compile.

In `OliveBlueprintToolHandlers.cpp`, add:
```cpp
void FOliveBlueprintToolHandlers::RegisterAssetWriterTools()
{
    // ... existing tools ...
    
    // Composite: create + plan in one shot
    Registry.RegisterTool(
        TEXT("blueprint.create_with_plan"),
        TEXT("Create a new Blueprint and apply a plan JSON in a single operation"),
        OliveBlueprintSchemas::BlueprintCreateWithPlan(),
        FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCreateWithPlan),
        {TEXT("blueprint"), TEXT("write"), TEXT("graph")},
        TEXT("blueprint")
    );
}

FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintCreateWithPlan(
    const TSharedPtr<FJsonObject>& Params)
{
    // 1. Extract create params and create the BP
    FOliveToolResult CreateResult = HandleBlueprintCreate(Params);
    if (!CreateResult.bSuccess) return CreateResult;
    
    // 2. Extract plan_json and auto-set the path
    // 3. Run preview internally (skip fingerprint requirement)  
    // 4. Apply plan
    // 5. Auto-compile
    // 6. Return combined result
}
```

This collapses 4 turns into **1 turn**, saving ~75% of token costs and wall-clock time.

---

### Bottleneck 2: Oversized Tool Results Bloat Context Window

**Problem:** `blueprint.read` in `full` mode returns the entire graph serialized as JSON — every node, every pin, every connection. For a moderately complex blueprint this is easily **10,000-30,000 characters**. This data goes into the conversation history and gets sent back to the API on every subsequent turn.

The `PromptDistiller` does summarize older tool results, but:
- `RecentPairsToKeep = 2` keeps the last 2 tool results verbatim  
- `MaxResultChars = 4000` means anything under 4K chars passes through unsummarized  
- A `blueprint.read` result at 15K chars DOES get summarized... but the one-line summary loses all node/pin context the AI needs for wiring

**Fix:**
1. **Add a `compact` read mode** that returns only node IDs, types, and pin names (no positions, no metadata, no default values). This would be ~500 chars for most blueprints:
```cpp
// In HandleBlueprintRead:
FString Mode;
Params->TryGetStringField(TEXT("mode"), Mode);
if (Mode == TEXT("compact"))
{
    // Serialize only: node_id, node_class, input pin names, output pin names
    // Skip: positions, comments, defaults, metadata
}
```

2. **Reduce `MaxResultChars` to 2000** and increase `RecentPairsToKeep` to 3:
```cpp
// In OliveConversationManager.cpp SendToProvider():
DistillConfig.MaxResultChars = 2000;  // was 4000
DistillConfig.RecentPairsToKeep = 3;   // was 2
```

3. **Have the plan result auto-embed its own pin manifests** in a condensed format so the AI doesn't need a separate `blueprint.read` call after creating nodes.

---

### Bottleneck 3: System Prompt is Rebuilt from Disk on Every Turn

**Problem:** `FOlivePromptAssembler::AssembleSystemPrompt()` reads `.txt` files from `Content/SystemPrompts/` on every call to `SendToProvider()`. It re-loads `Base.txt`, `ProfileBlueprint.txt`, `Worker_Blueprint.txt`, `blueprint_authoring.txt` (knowledge file), and more — every single turn.

**Fix:** Cache the assembled prompt per focus-profile in memory. Invalidate only when the profile changes or the user explicitly reloads:
```cpp
// In OlivePromptAssembler.h:
TMap<FString, FString> CachedPrompts; // ProfileName → assembled prompt
FDateTime LastCacheTime;

FString FOlivePromptAssembler::AssembleSystemPrompt(
    const FString& ProfileName, ...)
{
    if (FString* Cached = CachedPrompts.Find(ProfileName))
    {
        return *Cached; // Skip file I/O
    }
    // ... existing assembly logic ...
    CachedPrompts.Add(ProfileName, Result);
    return Result;
}
```

---

### Bottleneck 4: Preview → Apply Forces Two LLM Turns for Safety

**Problem:** `bPlanJsonRequirePreviewForApply = true` means the AI must call `preview_plan_json` first, get a fingerprint, then call `apply_plan_json` with that fingerprint. This is a safety mechanism but doubles the LLM turns for every plan.

**Fix — Use a single-turn "preview + apply if clean" mode:**

Add an `auto_apply` flag to `preview_plan_json`:
```cpp
// In the preview handler, after validation:
bool bAutoApply = false;
Params->TryGetBoolField(TEXT("auto_apply"), bAutoApply);

if (bAutoApply && PreviewResult.Errors.Num() == 0)
{
    // Automatically apply the plan if preview had zero errors
    return HandleApplyPlanJsonInternal(Plan, ResolvedSteps, ...);
}
// Otherwise return the preview with fingerprint for manual apply
```

The system prompt should instruct the AI to always use `auto_apply: true` unless it's an especially risky operation (like deleting nodes).

---

## Part 3: Quick-Win Priority Order

| Priority | Issue | Impact | Effort |
|----------|-------|--------|--------|
| 🔴 P0 | Routing threshold blocks modifications (Issue 1) | Breaks ALL modifications | 1 hour — config + counter reset |
| 🔴 P0 | Add `existing_nodes` to plan schema (Issue 2) | Blocks modify-existing flow | 4 hours — IR + executor + lowerer |
| 🟡 P1 | `create_with_plan` composite tool (Bottleneck 1) | 75% fewer LLM turns | 3 hours — new handler |
| 🟡 P1 | Compact read mode (Bottleneck 2) | Major token savings | 2 hours — serializer filter |
| 🟡 P1 | Pin manifest includes display names (Issue 3B) | Fixes self-correction wiring | 30 min — serializer change |
| 🟢 P2 | Prompt caching (Bottleneck 3) | Minor latency savings | 1 hour |
| 🟢 P2 | `auto_apply` preview mode (Bottleneck 4) | Saves 1 turn per operation | 2 hours |
| 🟢 P2 | Raise `MaxCorrectionReprompts` to 3 (Issue 3C) | Better error recovery | 5 min — constant change |

---

## Recommended Immediate Actions

1. **Change `PlanFirstGraphRoutingThreshold` from 3 → 8** in `OliveAISettings.h` and **reset routing stats per user turn** in `ConversationManager::SendUserMessage()`. This unblocks modifications immediately.

2. **Build `HandleBlueprintCreateWithPlan`** — this is the single biggest efficiency win, collapsing 4 API round-trips into 1.

3. **Add `existing_nodes` to the plan IR** — this is what makes the plan system work for modifications, not just creation.

Want me to produce the full C++ patch files for any of these?
