# Olive AI Studio — Structural Analysis: Why It's Struggling

**Date:** 2026-02-23  
**Scope:** Full architecture review covering tool dispatch, prompt design, validation, self-correction, and the unified provider path

---

## TL;DR Verdict

**The architecture is sound. The problems are all at the seams — where Claude's output meets your validation, where your schemas meet your prompts, and where your loop detector decides what counts as "the same error."** Nothing needs to be redesigned. You need ~8 targeted fixes, most under 20 lines each.

---

## 1. What's Working Well

Before diving into problems, here's what's genuinely solid:

**The unified provider architecture is correctly implemented.** `--max-turns 1`, `FOliveCLIToolCallParser`, `FOliveCLIToolSchemaSerializer`, tool calls parsed from `<tool_call>` XML blocks, `ConversationManager` owns the loop. This was the right call and it's wired up correctly.

**The 6-stage write pipeline.** Validate → Confirm → Transact → Execute → Verify → Report. Every mutation goes through it. This is production-grade infrastructure.

**Tool pack filtering.** `SendToProvider()` at line 65794 correctly sends only the filtered tool set: ReadPack on first turn, +WritePackBasic+WritePackGraph when write intent is detected. The AI sees ~32 tools instead of 95. This is working.

**The ConversationManager agentic loop.** `ProcessPendingToolCalls()` → `ExecuteToolCall()` → `HandleToolResult()` → `SelfCorrectionPolicy.Evaluate()` → `ContinueAfterToolResults()` → `SendToProvider()`. The flow is correct and all brain layer systems fire.

**IR-first design.** Every asset modification goes through typed intermediate representation. This gives you validation, serialization, and future portability for free.

---

## 2. The Problems (Ranked by Impact)

### Problem 1: `add_variable` Schema vs Prompt Mismatch (Critical)

This is probably your #1 source of failures for any Blueprint that has variables.

**The schema** requires a nested `variable` object:
```json
{"path": "/Game/BP_Gun", "variable": {"name": "FireRate", "type": {"category": "...", ...}}}
```

**The system prompt example** (line 13107) shows flat params:
```json
{"path": "/Game/Blueprints/BP_Gun", "name": "FireRate", "type": "Float", "default_value": "0.5"}
```

**The `BuildSystemPrompt` in the unified path** (line 73247) doesn't include variable examples at all — it only documents Plan JSON syntax. The `FOliveCLIToolSchemaSerializer` serializes the schema correctly, but the schema says `variable: VariableSpec (object, required)` while the model has been trained by the prompt example to send flat params.

**Result:** The handler at line 24296 calls `Params->TryGetObjectField("variable", ...)` and gets `nullptr` because the AI sent flat keys. Every `add_variable` call fails with `VALIDATION_MISSING_PARAM`.

**Fix:** Two options (do both):

**Option A — Accept flat format (backward compatible, ~15 lines):**
```cpp
// In HandleBlueprintAddVariable, before the existing TryGetObjectField:
const TSharedPtr<FJsonObject>* VariableJsonPtr;
if (!Params->TryGetObjectField(TEXT("variable"), VariableJsonPtr) || !VariableJsonPtr->IsValid())
{
    // Fallback: check if flat format was used (name/type at root level)
    FString FlatName;
    if (Params->TryGetStringField(TEXT("name"), FlatName) && !FlatName.IsEmpty())
    {
        // Wrap flat params into the expected nested format
        TSharedPtr<FJsonObject> WrappedVariable = MakeShareable(new FJsonObject());
        WrappedVariable->SetStringField(TEXT("name"), FlatName);
        // Copy type, default_value, category if present
        FString FlatType;
        if (Params->TryGetStringField(TEXT("type"), FlatType))
        {
            TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject());
            TypeObj->SetStringField(TEXT("category"), FlatType);
            WrappedVariable->SetObjectField(TEXT("type"), TypeObj);
        }
        FString DefaultValue;
        if (Params->TryGetStringField(TEXT("default_value"), DefaultValue))
            WrappedVariable->SetStringField(TEXT("default_value"), DefaultValue);
        FString Category;
        if (Params->TryGetStringField(TEXT("category"), Category))
            WrappedVariable->SetStringField(TEXT("category"), Category);
        
        UE_LOG(LogOliveBPTools, Log, TEXT("add_variable: Accepted flat format, wrapped into variable object"));
        // Use WrappedVariable as VariableJsonPtr
        // ... continue with ParseVariableFromParams(WrappedVariable) ...
    }
    else
    {
        return FOliveToolResult::Error(...); // existing error
    }
}
```

**Option B — Fix the prompt example (1 line):**
Update line 13107 and the `BuildSystemPrompt` to show the correct nested format:
```
3. blueprint.add_variable → {"path": "/Game/BP_Gun", "variable": {"name": "FireRate", "type": {"category": "Float"}, "default_value": "0.5"}}
```

**Do both.** Option A makes the system robust against any format the AI sends. Option B reduces wasted tokens on retries.

---

### Problem 2: `blueprint.create` Type Validation Is Too Strict (Critical)

The type parsing at line 23778 is case-sensitive exact-match with a hard fail on unknown values:

```cpp
else if (TypeString != TEXT("Normal"))
{
    return FOliveToolResult::Error(
        TEXT("VALIDATION_INVALID_VALUE"),
        FString::Printf(TEXT("Invalid Blueprint type '%s'"), *TypeString), ...);
}
```

Claude frequently sends `"type": "Actor"` (confusing it with `parent_class`) or `"type": "Blueprint"` or `"type": "actor"`. All of these kill the entire create call on what is an **optional parameter that defaults to "Normal"**.

**Fix (~20 lines):** Case-insensitive matching with warn-and-default for unknown values:
```cpp
FString TypeUpper = TypeString.ToUpper();
if (TypeUpper == TEXT("NORMAL") || TypeUpper == TEXT("BLUEPRINT") || TypeUpper.IsEmpty()) {
    BlueprintType = EOliveBlueprintType::Normal;
} else if (TypeUpper == TEXT("INTERFACE")) {
    BlueprintType = EOliveBlueprintType::Interface;
} else if (TypeUpper == TEXT("FUNCTIONLIBRARY") || TypeUpper == TEXT("FUNCTION_LIBRARY")) {
    BlueprintType = EOliveBlueprintType::FunctionLibrary;
// ... other types ...
} else {
    UE_LOG(LogOliveBPTools, Warning, TEXT("blueprint.create: Unknown type '%s', defaulting to Normal"), *TypeString);
    BlueprintType = EOliveBlueprintType::Normal; // DON'T FAIL
}
```

The `parent_class` field already carries the actual class info (Actor, Pawn, Character). The `type` field is about Blueprint subtype. Failing on this is like rejecting a valid HTML form because someone put "submit" in the wrong field.

---

### Problem 3: Loop Detector False Positives on Batch Tool Calls (High)

The `SelfCorrectionPolicy::Evaluate` at line 64945 builds signatures with an **empty asset path**:

```cpp
const FString Signature = FOliveLoopDetector::BuildToolErrorSignature(ToolName, ErrorCode, TEXT(""));
```

`BuildToolErrorSignature` at line 64785 produces: `"blueprint.add_component:ASSET_NOT_FOUND:"` — the same signature regardless of which asset failed.

When the AI sends 3 `add_component` calls to 3 different blueprints and they all fail (because `blueprint.create` failed first), the loop detector sees 3 "retries" of the same signature and triggers `StopWorker`. But these aren't retries — they're **independent failures on different assets** that share a root cause.

**Fix (~8 lines in SelfCorrectionPolicy::Evaluate):**
```cpp
// Extract asset_path from the result JSON to build a more specific signature
FString AssetContext;
{
    TSharedPtr<FJsonObject> ParsedResult;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
    if (FJsonSerializer::Deserialize(Reader, ParsedResult) && ParsedResult.IsValid())
    {
        if (const TSharedPtr<FJsonObject>* ErrorObj; ParsedResult->TryGetObjectField(TEXT("error"), ErrorObj))
        {
            (*ErrorObj)->TryGetStringField(TEXT("asset_path"), AssetContext);
        }
    }
}
const FString Signature = FOliveLoopDetector::BuildToolErrorSignature(ToolName, ErrorCode, AssetContext);
```

Now `add_component` failing on `/Game/BP_Gun` and `/Game/BP_Bullet` produce **different** signatures: `"blueprint.add_component:ASSET_NOT_FOUND:/Game/BP_Gun"` vs `"blueprint.add_component:ASSET_NOT_FOUND:/Game/BP_Bullet"`. Each gets 1 attempt, not 3.

---

### Problem 4: Competing System Prompts (High)

There are **two** system prompts being sent to Claude Code:

1. **The old wrapper prompt** (line ~13090) — the `const FString Wrapper = ...` block that says "follow this exact order, one call at a time" with the flat `add_variable` example
2. **The new `BuildSystemPrompt`** (line 73190) — the unified architecture's design-focused prompt with Plan JSON reference and `FOliveCLIToolSchemaSerializer` output

**Are both being sent?** The wrapper prompt is in `plans/smart_blueprints/02-routing-wrapper-prompt.cpp` — a patch file. If it was applied to `OliveClaudeCodeProvider.cpp`, it would be in the `SendMessage` path. But `BuildSystemPrompt` is also in `SendMessage`. So either:
- The old wrapper was replaced by `BuildSystemPrompt` (good, no conflict)
- Both are being concatenated (bad, ~6KB of competing instructions)

**Check and fix:** Ensure only `BuildSystemPrompt` is used. Remove any residual `Wrapper` string construction. If both are present, the AI gets contradictory instructions ("follow this exact order, one call at a time" vs "think through the complete design before calling tools").

---

### Problem 5: No Dependency Ordering in Batch Tool Calls (Medium)

When the AI outputs multiple `<tool_call>` blocks in one response, `ProcessPendingToolCalls()` at line 66040 executes them **all sequentially from a flat array** with no dependency analysis:

```cpp
for (const FOliveStreamChunk& ToolCall : CallsToProcess)
{
    ExecuteToolCall(ToolCall);
}
```

If the AI outputs:
1. `blueprint.create` → BP_Gun  
2. `blueprint.add_component` → BP_Gun + StaticMesh  
3. `blueprint.add_variable` → BP_Gun + FireRate  

And #1 fails, #2 and #3 will also fail (ASSET_NOT_FOUND), bloating the error history and confusing the AI's next attempt.

The `bStopAfterToolResults` flag only stops on `StopWorker` from the loop detector — it doesn't stop on a single tool failure.

**Fix (two approaches):**

**Quick fix — stop batch on first hard failure (~10 lines):**
Add a flag check in the loop body:
```cpp
// After ExecuteToolCall returns, check if a create/foundational tool failed
if (!Result.bSuccess && IsFoundationalTool(ToolCall.ToolName))
{
    // Skip remaining tools that likely depend on this one
    bStopAfterToolResults = false; // Don't stop the whole run
    // Just SKIP remaining tools with a clear message
    for remaining tools: add SKIPPED result with "Skipped: prerequisite tool failed"
    break;
}
```

Where `IsFoundationalTool` returns true for `blueprint.create`, `behaviortree.create`, `pcg.create_graph`, `cpp.create_class`.

**Better fix — dependency graph:** Parse tool arguments to detect asset_path dependencies. If tool B's `blueprint_path` matches tool A's create `path`, B depends on A. Skip B if A fails. This is more work (~50 lines) but eliminates all cascade failures.

---

### Problem 6: Prompt Token Budget on Complex Tasks (Medium)

`BuildSystemPrompt` at line 73190 includes:
- Identity text (~50 tokens)
- Plan JSON reference + example (~200 tokens)  
- Available ops list (~50 tokens)
- Data wire syntax (~60 tokens)
- Exec flow rules (~40 tokens)
- Function resolution rules (~40 tokens)
- Key rules (~80 tokens)
- **Tool schema serialization** (varies: ~800-2000 tokens for 32 tools)
- Tool call format instructions (~80 tokens)

Total: ~1,400-2,600 tokens of system prompt.

Then Claude Code's **own built-in system prompt** adds another ~2,000-4,000 tokens (file editing, bash, permissions — none relevant to UE5).

And the conversation history (including previous tool results) adds more.

**On a complex task like "create a gun that shoots bullets"**, the AI needs context budget for designing TWO blueprints with components, variables, and graph logic. The system prompt overhead leaves less room for the actual design thinking.

**Fix:** The system prompt is already good — but make the `FOliveCLIToolSchemaSerializer::Serialize()` compact mode more aggressive. Currently it includes full parameter descriptions. For tools the AI already knows well (blueprint.create, add_component), a one-liner signature is enough:

```
blueprint.create(path*, parent_class="Actor", type="Normal")
blueprint.add_component(path*, class*, name?, parent?)
blueprint.add_variable(path*, variable: {name*, type*: {category*}, default_value?, category?})
```

This cuts tool schema tokens by ~60%.

---

### Problem 7: `add_component` Uses Different Param Names Than Schema (Low-Medium)

The schema at line 21588 uses `class` and `name`:
```
blueprint.add_component(path*, class*, name?, parent?)
```

But the old wrapper prompt at line 13107 uses `component_class` and `name`:
```json
{"path": "...", "component_class": "StaticMeshComponent", "name": "GunMesh"}
```

**Check the handler** — does it look for `class` or `component_class`? If there's a mismatch, the AI could send the wrong key depending on which prompt it's following.

**Fix:** The handler should accept both. Add a fallback:
```cpp
FString ComponentClass;
if (!Params->TryGetStringField(TEXT("class"), ComponentClass))
    Params->TryGetStringField(TEXT("component_class"), ComponentClass);
```

---

### Problem 8: No Tool Call Argument Logging on Failure (Diagnostic)

When a tool call fails, the log shows:
```
Tool 'blueprint.create' failed. Error: VALIDATION_INVALID_VALUE
```

But NOT what arguments were sent. You can't debug what the AI sent wrong without adding logging:

**Fix (~8 lines in ExecuteToolCall or HandleToolResult):**
```cpp
if (!Result.bSuccess && ToolCall.ToolArguments.IsValid())
{
    FString ArgsStr;
    auto Writer = TJsonWriterFactory<>::Create(&ArgsStr);
    FJsonSerializer::Serialize(ToolCall.ToolArguments.ToSharedRef(), Writer);
    Writer->Close();
    UE_LOG(LogOliveAI, Warning, TEXT("Tool '%s' FAILED with args: %s"), *ToolCall.ToolName, *ArgsStr);
}
```

---

## 3. Priority-Ordered Fix List

| # | Fix | Impact | Effort | Lines |
|---|-----|--------|--------|-------|
| 1 | Accept flat `add_variable` format | Critical | 30min | ~25 |
| 2 | Lenient `blueprint.create` type parsing | Critical | 20min | ~20 |
| 3 | Fix loop detector false positives (pass asset_path) | High | 15min | ~8 |
| 4 | Remove competing wrapper prompt if still present | High | 10min | ~5 |
| 5 | Stop batch on foundational tool failure | Medium | 30min | ~15 |
| 6 | Log tool call arguments on failure | Diagnostic | 10min | ~8 |
| 7 | Compact tool schema serialization | Medium | 45min | ~30 |
| 8 | Accept `component_class` alias in add_component | Low | 5min | ~3 |

**Fixes 1-4 alone should eliminate the majority of failures you're seeing.** They address the exact failure chain: AI sends wrong format → validation rejects it → cascade failures → loop detector kills the run.

---

## 4. What Does NOT Need to Change

- **ConversationManager** — The agentic loop is correct. Don't touch it.
- **Write Pipeline** — The 6-stage pipeline is solid. Don't bypass it.
- **Brain Layer state machine** — Idle → WorkerActive → Completed flow is correct.
- **Tool Pack Manager** — Filtering is working correctly with the unified architecture.
- **Prompt Distiller** — Already manages context growth for multi-turn loops.
- **IR type system** — Solid foundation, no issues found.
- **MCP Server** — Not involved in the unified path (which is correct).

---

## 5. The "Gun Blueprint" Failure, Reconstructed

Here's what happens when someone says "create a gun blueprint that shoots bullets" with the current code:

1. **Turn 1:** AI outputs `<tool_call>` blocks for `blueprint.create`, `add_component`, `add_variable`
2. `blueprint.create` → AI might send `"type": "Actor"` → **VALIDATION_INVALID_VALUE** → fails
3. `add_component` → asset doesn't exist → **ASSET_NOT_FOUND** → fails  
4. `add_variable` → either asset doesn't exist (ASSET_NOT_FOUND) or flat format (VALIDATION_MISSING_PARAM) → fails
5. Loop detector sees 3x `ASSET_NOT_FOUND` with empty asset_path → same signature → **StopWorker**
6. Run aborted after 1 turn with 0 successful operations

**After fixes 1-4:**

1. `blueprint.create` → AI sends `"type": "Actor"` → **warn, default to Normal** → succeeds ✅
2. `add_component` → asset exists now → succeeds ✅
3. `add_variable` → AI sends flat format → **auto-wrapped** → succeeds ✅
4. AI continues to `apply_plan_json` for graph logic
5. If plan has wiring errors → SelfCorrectionPolicy feeds back errors → AI retries
6. Loop detector correctly tracks per-asset errors, no false positives

The same user prompt goes from "total failure in 2ms" to "complete Blueprint with graph logic in 3-5 turns."

---

## 6. Summary

Your architecture is well-designed. The unified provider path, the write pipeline, the brain layer, the tool packs — all of it is correct and well-engineered. The plugin is struggling because of **validation fragility at the AI boundary** — places where the system assumes the AI will send exactly the right format, and rejects everything else. LLMs are probabilistic; they'll send `"type": "Actor"` instead of `"parent_class": "Actor"` sometimes. The system needs to be lenient on input (accept reasonable variations) and strict on output (validate the final UE operation).

The 8 fixes above address this philosophy. They make the input layer more forgiving without compromising the write pipeline's safety guarantees.
