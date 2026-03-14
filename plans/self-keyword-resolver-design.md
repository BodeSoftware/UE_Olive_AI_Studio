# @self Keyword Recognition in Plan Resolver

**Date:** 2026-03-13
**Priority:** P1 (data wiring silently dropped)
**Complexity:** Junior (~30 lines changed, 0 new files)
**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

## Problem

When the AI writes `"NewOwner": "@self.auto"` in a plan_json step, the resolver's `ExpandComponentRefs` strips the input entirely. The resolver treats ALL `@self` references as redundant "Target pin auto-wire" references and removes them. But `@self` on a non-Target pin (like `NewOwner`, `NewInstigator`, `Actor`) means "wire a Self reference node to this pin" -- a real data connection, not the hidden self pin.

The executor already has a working `@self` handler at `OlivePlanExecutor.cpp:3471` that creates a `UK2Node_Self` node and wires its output to the target pin. But it never fires because `ExpandComponentRefs` deletes the `@self` input before the executor sees it.

**Observed symptom:** `SetOwner` on a spawned bullet has no NewOwner wired. The gun actor (self) should be the owner, but the input is silently dropped. IRSchema logs the warning: `input 'NewOwner' ref '@self.auto' references unknown step 'self'` -- then the resolver strips it.

## Root Cause

Two locations in `ExpandComponentRefs` strip `@self` unconditionally:

1. **Line ~932 (dotted path, `@self.X`):** When `RefStepId == "self"`, marks the input for removal with reason "Target pins auto-wire to self by default."
2. **Line ~1180 (bare path, `@self`):** Same logic for the no-dot case.

Both assume `@self` is always about the hidden Target/self pin. This is wrong -- `@self` on any other pin key means "pass a reference to the current actor."

## Fix

**Change the condition:** Only strip `@self` when the pin name is `Target` or `self`. For all other pin names, preserve the `@self.auto` (or `@self`) reference so the executor's Phase 4 handler can create a `UK2Node_Self` and wire it.

### Location 1: Dotted `@self.X` (line ~932)

**Current code:**
```cpp
if (RefStepId.Equals(TEXT("self"), ESearchCase::IgnoreCase))
{
    RewrittenInputs.Add(PinName, FString()); // empty = mark for removal
    // ... note + log ...
    continue;
}
```

**New code:**
```cpp
if (RefStepId.Equals(TEXT("self"), ESearchCase::IgnoreCase))
{
    // Only strip @self on Target/self pins (hidden self pin auto-wires).
    // On any other pin (e.g., NewOwner, Actor), @self means "wire a
    // K2Node_Self reference node" -- let the executor handle it in Phase 4.
    if (PinName.Equals(TEXT("Target"), ESearchCase::IgnoreCase)
        || PinName.Equals(TEXT("self"), ESearchCase::IgnoreCase))
    {
        RewrittenInputs.Add(PinName, FString()); // empty = mark for removal
        // ... existing note + log (keep as-is) ...
        continue;
    }
    // Non-Target pin: leave @self.X in place for executor Phase 4 handling.
    // No synthesis needed -- executor creates UK2Node_Self inline.
    continue;
}
```

### Location 2: Bare `@self` (line ~1180)

**Current code:**
```cpp
if (RefBody.Equals(TEXT("self"), ESearchCase::IgnoreCase))
{
    RewrittenInputs.Add(PinName, FString()); // empty = mark for removal
    // ... note + log ...
    continue;
}
```

**New code:**
```cpp
if (RefBody.Equals(TEXT("self"), ESearchCase::IgnoreCase))
{
    // Only strip bare @self on Target/self pins.
    // On other pins, rewrite bare @self to @self.auto so the executor's
    // Phase 4 ParseDataRef can parse it (requires a dot).
    if (PinName.Equals(TEXT("Target"), ESearchCase::IgnoreCase)
        || PinName.Equals(TEXT("self"), ESearchCase::IgnoreCase))
    {
        RewrittenInputs.Add(PinName, FString()); // empty = mark for removal
        // ... existing note + log (keep as-is) ...
        continue;
    }
    // Bare @self on a non-Target pin: rewrite to @self.auto so ParseDataRef
    // can parse it. The executor's @self handler (line 3472) will create
    // a UK2Node_Self and wire it.
    RewrittenInputs.Add(PinName, TEXT("@self.auto"));
    bExpanded = true;

    FOliveResolverNote Note;
    Note.Field = FString::Printf(TEXT("step '%s' inputs.%s"), *Step.StepId, *PinName);
    Note.OriginalValue = Value;
    Note.ResolvedValue = TEXT("@self.auto");
    Note.Reason = TEXT("Bare @self on non-Target pin rewritten to @self.auto for Self reference node wiring.");
    OutNotes.Add(MoveTemp(Note));

    UE_LOG(LogOlivePlanResolver, Log,
        TEXT("ExpandComponentRefs: Rewrote bare @self to @self.auto for step '%s' input '%s'"),
        *Step.StepId, *PinName);
    continue;
}
```

## Why This Works

1. `@self` on `Target` pin: still stripped (correct -- Target auto-wires to self by default, the pin is hidden)
2. `@self.auto` on any other pin: passes through to executor's `PhaseWireData` -> `WireDataConnection` -> `ParseDataRef` extracts `SourceStepId = "self"` -> line 3472 creates `UK2Node_Self`, finds the target pin, wires it
3. Bare `@self` on any other pin: rewritten to `@self.auto` so `ParseDataRef` can parse it (requires a dot separator)
4. No synthesized steps needed -- the executor already creates `UK2Node_Self` inline in Phase 4
5. Deduplication is handled naturally by the executor -- each `@self` reference gets its own `K2Node_Self` node, which is fine (Blueprint editor does the same when you drag "Get a reference to self" multiple times)

## IRSchema Warning

The IRSchema at line 1000 already soft-handles this case -- it logs a warning but does NOT hard-reject unknown step refs. The comment says "may be component/param name (resolver will handle)". After this fix, the `@self` reference survives the resolver and the executor handles it. The warning is benign and can be left as-is. If desired, a follow-up could add `"self"` to a known-keyword set in IRSchema to suppress the log line, but that is cosmetic and not required.

## Edge Cases

| Scenario | Before Fix | After Fix |
|----------|-----------|-----------|
| `"Target": "@self.auto"` | Stripped (correct) | Stripped (correct) |
| `"NewOwner": "@self.auto"` | Stripped (BUG) | Preserved, executor wires UK2Node_Self |
| `"NewOwner": "@self"` (bare) | Stripped (BUG) | Rewritten to `@self.auto`, executor wires |
| `"self": "@self.auto"` | Stripped (correct) | Stripped (correct, pin name is "self") |
| `"Actor": "@self.auto"` | Stripped (BUG) | Preserved, executor wires |

## Testing

Verify with a plan that calls `SetOwner` on a spawned actor with `"NewOwner": "@self.auto"`:

```json
{
  "steps": [
    {"step_id": "begin", "op": "event", "target": "BeginPlay"},
    {"step_id": "spawn", "op": "spawn_actor", "target": "BP_Projectile",
     "exec_after": "begin"},
    {"step_id": "set_owner", "op": "call", "target": "SetOwner",
     "exec_after": "spawn",
     "inputs": {"NewOwner": "@self.auto"}}
  ]
}
```

After the fix, the SetOwner node should have a `UK2Node_Self` wired to its NewOwner input pin.

## Implementation Order

1. Modify Location 1 (dotted `@self.X`, ~line 932) -- add pin name guard
2. Modify Location 2 (bare `@self`, ~line 1180) -- add pin name guard + rewrite to `@self.auto`
3. Test with SetOwner plan above
