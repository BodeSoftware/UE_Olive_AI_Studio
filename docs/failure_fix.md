# Olive AI: Blueprint Plan Pipeline — Root Cause Analysis & Fixes

## The Task That Failed

The AI was asked to create a BP_Gun and BP_Bullet blueprint. It successfully:
- Created both blueprints (iteration 1, tc_1 + tc_2) ✅
- Added 6 components, 4 variables, modified 2 components (iteration 2, tc_3–tc_14) ✅
- Previewed BP_Bullet plan successfully (tc_15) ✅

But then **every `apply_plan_json` call failed** across 2 retries, totaling 4 GRAPH_DRIFT failures, killing the entire run. None of the graph logic was ever written.

---

## Root Cause: Architectural Mismatch Between Batching and the Preview→Apply Contract

The preview/apply workflow requires a **two-phase contract**:
1. Call `preview_plan_json` → receive fingerprint
2. Pass that fingerprint to `apply_plan_json`

But Claude Code runs with `--max-turns 1`, meaning it emits **all tool calls in a single response**. The AI must compose the `apply_plan_json` call *at the same time* as the `preview_plan_json` call — before it ever sees the preview result. 

**The AI cannot possibly know the correct fingerprint because it hasn't received the preview result yet.**

This is visible in the log. In iteration 2, all 16 tool calls (tc_3 through tc_18) are parsed at the exact same timestamp `21.19.00:792`:

```
tc_15: preview_plan_json (BP_Bullet)  → succeeds, fingerprint = E086D8D7F95A435B...
tc_16: preview_plan_json (BP_Gun)     → fails (forward ref in plan)
tc_17: apply_plan_json (BP_Bullet)    → fingerprint sent: "eb39b2e1db5a4f4f" ← FABRICATED
tc_18: apply_plan_json (BP_Gun)       → fingerprint sent: "5ceaa9b2dcfb9b08" ← FABRICATED
```

The AI *hallucinated* the fingerprints because it had to. The real fingerprint is a 40-char uppercase SHA1; the AI sent 16-char lowercase gibberish.

Even in iteration 3, where the AI correctly re-previewed first, it *again* batched preview + apply together, so the same problem repeats:

```
tc_19: preview (BP_Bullet) → fingerprint = E086D8D7F95A435B090FC8D8E1A3F14252101771
tc_20: preview (BP_Gun)    → fingerprint = EEA22EE6C4A531812954F7B12040D9624B550DDC
tc_21: apply (BP_Bullet)   → fingerprint sent: "e086d8d7f95a435b" ← lowercase + truncated
tc_22: apply (BP_Gun)      → fingerprint sent: "5ceaa9b2dcfb9b08" ← STALE from iteration 2!
```

**This will fail 100% of the time with the current design.** It's not a flaky bug; it's architecturally impossible to succeed.

---

## Contributing Problems (5 total)

### Problem 1: The Fundamental Batching Problem (CRITICAL)
- Preview and apply **must not** be in the same batch when fingerprints are required
- With `--max-turns 1`, the AI has no way to see preview output before composing apply
- **This alone explains every failure in this log**

### Problem 2: Case-Sensitive Fingerprint Comparison
- `FSHAHash::ToString()` returns UPPERCASE (`E086D8D7...`)
- Claude Code / LLMs naturally output lowercase hex (`e086d8d7...`)
- Line 28461: `if (CurrentFingerprint != ProvidedFingerprint)` — raw comparison, case-sensitive
- Even if the AI somehow got the right fingerprint, case mismatch kills it

### Problem 3: LLM Truncates Long Hex Strings
- SHA1 produces 40-character hex strings
- LLMs frequently truncate to 16 chars (cutting after 8 bytes)
- A 40-char opaque token is hostile to LLM reproduction

### Problem 4: No GRAPH_DRIFT-Specific Correction Guidance
- `SelfCorrectionPolicy::BuildToolErrorMessage` (line 65307) has specific guidance for many error codes
- `GRAPH_DRIFT` falls through to the generic else: "Analyze the error and try a different approach"
- The correction message tells the AI to re-run preview, but the AI just does the same batch pattern again

### Problem 5: `preview_plan_json` Is Not a Foundational Tool
- `IsFoundationalTool()` (line 65638) only covers `blueprint.create`, `behaviortree.create`, etc.
- When a `preview_plan_json` fails (like tc_16 with PLAN_INVALID_INPUT_REF), the corresponding `apply_plan_json` still runs
- apply then fails too, doubling the error count and confusing the correction loop

---

## Recommended Fixes (Priority Order)

### Fix A: Auto-Preview Inside Apply (Eliminates the Problem Entirely)

**The cleanest fix.** When `apply_plan_json` receives no fingerprint (or an invalid one), just run the preview internally rather than failing:

```cpp
// In HandleBlueprintApplyPlanJson, replace section 7 (lines 28452-28469):

// 7. Drift detection OR auto-preview
if (!ProvidedFingerprint.IsEmpty() && !bGraphMissing)
{
    FOliveGraphReader DriftReader;
    FOliveIRGraph CurrentGraphIR = DriftReader.ReadGraph(TargetGraph, Blueprint);
    FString CurrentFingerprint = FOliveBlueprintPlanResolver::ComputePlanFingerprint(
        CurrentGraphIR, Plan);

    // Case-insensitive prefix match (LLMs lowercase/truncate hex)
    if (!CurrentFingerprint.Equals(ProvidedFingerprint, ESearchCase::IgnoreCase)
        && !CurrentFingerprint.StartsWith(ProvidedFingerprint, ESearchCase::IgnoreCase))
    {
        // Log drift but don't fail — just re-validate
        UE_LOG(LogOliveAI, Warning,
            TEXT("Fingerprint mismatch (expected '%s', current '%s'). "
                 "Auto-revalidating plan."),
            *ProvidedFingerprint, *CurrentFingerprint);
    }
    // If fingerprint matches, great — proceed
}
// If no fingerprint provided, still proceed (auto-preview mode)
// The resolve + execute steps below will catch any actual issues
```

This means the preview is still *useful* (AI gets resolution feedback before apply), but not a *gate*. The real safety net is the resolve + execute pipeline inside apply, which already validates everything.

**Alternatively**, you could do the full preview internally:

```cpp
// If no valid fingerprint, run preview logic inline
if (ProvidedFingerprint.IsEmpty() || bFingerprintMismatch)
{
    UE_LOG(LogOliveAI, Log, TEXT("Auto-previewing plan (no valid fingerprint provided)"));
    // The resolve step (section 8) already does everything preview does
    // Just skip the drift check and proceed to resolve + execute
}
```

### Fix B: Short, Lowercase, LLM-Friendly Fingerprint

If you want to keep the preview requirement, make the fingerprint something an LLM can actually reproduce:

```cpp
// In ComputePlanFingerprint (line 32213-32218):

// Use first 8 chars of lowercase hex — 32 bits is plenty for drift detection
FSHA1::HashBuffer(TCHAR_TO_UTF8(*Utf8Input), Utf8Input.Len(), Hash.Hash);
FString FullHash = Hash.ToString();
return FullHash.Left(8).ToLower();  // e.g., "e086d8d7"
```

And fix the comparison:
```cpp
// Line 28461:
if (!CurrentFingerprint.Equals(ProvidedFingerprint, ESearchCase::IgnoreCase))
```

### Fix C: Batch-Split Preview and Apply

If you keep fingerprint-gated apply, prevent them from being in the same batch:

```cpp
// In ProcessPendingToolCalls (line 66362), before executing:

// Defer apply_plan_json calls that follow preview_plan_json in the same batch
TArray<FOliveStreamChunk> DeferredApplyCalls;
bool bHasPreviewInBatch = false;

for (const FOliveStreamChunk& ToolCall : CallsToProcess)
{
    if (ToolCall.ToolName == TEXT("blueprint.preview_plan_json"))
    {
        bHasPreviewInBatch = true;
    }
}

if (bHasPreviewInBatch)
{
    // Split: execute everything except apply calls now,
    // apply calls get deferred to next iteration
    TArray<FOliveStreamChunk> ImmediateCalls;
    for (FOliveStreamChunk& TC : CallsToProcess)
    {
        if (TC.ToolName == TEXT("blueprint.apply_plan_json"))
        {
            // Return "deferred" result telling AI to resubmit with fingerprint
            DeferredApplyCalls.Add(MoveTemp(TC));
        }
        else
        {
            ImmediateCalls.Add(MoveTemp(TC));
        }
    }
    CallsToProcess = MoveTemp(ImmediateCalls);
    
    // Queue deferred calls to be suggested in the correction directive
    // OR just return a message telling the AI to re-call apply with the fingerprint
}
```

### Fix D: Add `preview_plan_json` as a Foundational Tool for Apply

Quick win — when preview fails, skip the corresponding apply:

```cpp
// Replace IsFoundationalTool (line 65638):
bool IsFoundationalTool(const FString& ToolName)
{
    return ToolName == TEXT("blueprint.create")
        || ToolName == TEXT("behaviortree.create")
        || ToolName == TEXT("pcg.create_graph")
        || ToolName == TEXT("cpp.create_class")
        || ToolName == TEXT("blueprint.preview_plan_json");  // ADD THIS
}
```

### Fix E: GRAPH_DRIFT Correction Guidance

Add specific guidance in `BuildToolErrorMessage` (after line 65306):

```cpp
else if (ErrorCode == TEXT("GRAPH_DRIFT"))
{
    Guidance = TEXT(
        "The fingerprint did not match. Do NOT batch preview and apply in the same "
        "tool call response. Call blueprint.preview_plan_json FIRST, wait for the "
        "result, then call blueprint.apply_plan_json with the exact fingerprint "
        "string from the preview result. Copy the fingerprint exactly — do not "
        "modify the case or truncate it.");
}
```

---

## My Recommendation

**Do Fix A (auto-preview inside apply).** Here's why:

1. The preview→apply two-phase contract is fundamentally incompatible with `--max-turns 1` batching. Even with perfect correction guidance, the AI will keep batching them together because that's how LLM tool calling works — it emits all calls at once.

2. The preview step doesn't actually provide safety that the resolve+execute pipeline doesn't already provide. Both validate the plan, resolve functions, and check for errors. The fingerprint's only purpose is to detect if the graph changed between preview and apply — but with batched execution happening in milliseconds on the game thread, there's zero real-world risk of graph modification between them.

3. If you still want the preview to be useful (and it is — it gives the AI resolution feedback), keep it as an **optional diagnostic tool** that doesn't gate apply. The AI can call preview to check its plan, see resolved function names, catch issues early. But apply should work without a fingerprint.

If you also do **Fix B** (short fingerprints) and **Fix D** (foundational tool), those are low-effort improvements that help even after Fix A.

---

## Summary of What Happened in This Log

| Iteration | What Happened | Result |
|-----------|--------------|--------|
| 1 | Created BP_Bullet + BP_Gun | ✅ Success |
| 2 | 12 component/variable writes succeeded. 2 previews attempted (1 passed, 1 failed due to forward ref). 2 applies attempted — both GRAPH_DRIFT because fingerprints were fabricated (AI can't know them before preview returns). | ❌ 3 failures |
| 3 (correction) | AI re-previewed both (both pass now — fixed step ordering). Both applies fail with GRAPH_DRIFT again — same batch problem, fingerprints still wrong. Self-correction loop detector fires. | ❌ Run killed |

The BP_Bullet and BP_Gun blueprints exist with correct components and variables, but **have completely empty EventGraphs** because no plan was ever successfully applied.
