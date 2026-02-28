# Task: Remove Function Resolver — Let Node Factory Handle Everything

## Problem

The function resolver (`FOliveFunctionResolver`) sits between the AI's plan and the node factory. When it can't find a function, it "accepts as-is" and reports the plan as successfully resolved ("7/7 steps resolved, 0 errors"). The node factory then fails at execution time, after partial nodes are created, triggering expensive rollbacks.

In the latest pickup task log, this caused 7 plan_json failures and 3 batch rollbacks in a single run. Functions like `GetMesh`, `K2_AttachActorToComponent`, and `WasInputKeyJustPressed` all passed resolution but failed at execution. The AI spent 15 minutes retrying instead of building.

The resolver also provides no value the AI can't provide itself. The AI knows UE5 — when a function fails, it self-corrects. The resolver is a middleman that either:
- Finds the function (the node factory would have found it too)
- Doesn't find it and lies about it ("accepted as-is")

## What To Do

Remove `FOliveFunctionResolver` from the plan resolution pipeline. The node factory (`FOliveNodeFactory::FindFunction`) becomes the single validation point.

### Keep: K2 Prefix Resolution

The one genuinely useful thing the resolver does is K2 prefix handling — `GetRootComponent` → `K2_GetRootComponent`. Move this logic into the node factory's `FindFunction` method:

```cpp
// In FOliveNodeFactory::FindFunction, after all searches fail:
// Try adding K2_ prefix
if (!FunctionName.StartsWith(TEXT("K2_")))
{
    FString K2Name = TEXT("K2_") + FunctionName;
    // repeat the same search with K2Name
}
// Try removing K2_ prefix
else
{
    FString BaseName = FunctionName.Mid(3);
    // repeat the same search with BaseName
}
```

### Keep: Alias Map (Optional)

If there's an alias map (e.g., `SetTimer` → `K2_SetTimerByFunctionName`), move those mappings into the node factory as well. Simple lookup before the search.

### Remove

1. **`FOliveFunctionResolver::Resolve()`** — no longer called
2. **`FOliveBlueprintPlanResolver::ResolveCallOp()`** — simplify to just set `function_name` and `target_class` from the step, no resolution attempt. Or better: pass the step's target straight through to the node factory
3. **The "accepted as-is" path** — gone entirely. If the function doesn't exist, the node factory fails immediately at creation time with a clear error
4. **BroadSearch / confidence thresholds / candidate scoring** — all resolver-specific complexity that goes away

### Files Affected

- `OliveFunctionResolver.cpp` / `.h` — can be deleted entirely or gutted to just K2 prefix + alias helpers
- `OliveBlueprintPlanResolver.cpp` — `ResolveCallOp()` simplifies dramatically. For `call` ops, just pass through `target` as `function_name` and `target_class` as-is
- `OliveNodeFactory.cpp` — `FindFunction()` gains K2 prefix fallback (5-10 lines) and optionally alias lookup

### What Changes For The AI

- Plan resolution stops lying. If a function can't be found, the AI learns at execution time (node factory), not after a fake "success"
- Failures happen before nodes are created — no more partial execution + rollback
- The AI self-corrects faster because errors are immediate and clear
- The AI is free to specify target_class when it knows the right class, or omit it and let the node factory search

### What To Verify

- The gun template task (create gun, shoot bullets) should still work — `K2_SetTimerByFunctionName`, `SetActorLocation`, etc. should all resolve via the node factory's existing search + the new K2 prefix fallback
- Blueprint-defined functions (custom functions created via `add_function`) must still be found via `Blueprint->GeneratedClass->FindFunctionByName`
- Library functions (PrintString, MakeVector, etc.) still found via the common classes list

### Expected Result

Fewer retries per plan. The pickup task had 7 plan failures from unresolved functions — most would become immediate node factory errors instead of "resolved successfully → execute → fail → rollback → retry." The AI gets clearer errors and self-corrects in 1 cycle instead of 3-4.
