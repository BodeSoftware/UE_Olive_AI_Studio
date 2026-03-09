# Run 09p Fixes Design

Three targeted fixes from the 09p regression analysis. Ordered by priority.

---

## Fix 1: Modify() Reused Nodes (P0)

### Problem

When the plan executor reuses an existing event node (BeginPlay, custom events, ComponentBoundEvent, EnhancedInputAction, FunctionEntry, FunctionResult), it wires from that node's exec pins. If the plan later fails and the `FScopedTransaction` rolls back, NEW nodes are undone but **wiring changes on the reused node survive** because the reused node was never `Modify()`'d within the transaction scope.

On the next plan attempt, the reused node's exec pin has `links=1` from the ghost wiring. `CanCreateConnection` returns `CONNECT_RESPONSE_BREAK_OTHERS_A` (enum value 2, message "Replace existing output connections"). Our `Connect()` method calls `CanSafeConnect()` which only accepts `CONNECT_RESPONSE_MAKE` and `CONNECT_RESPONSE_MAKE_WITH_PROMOTION` -- so it rejects the connection.

### Evidence

```
EXEC WIRE REJECTED: K2Node_CustomEvent_0.then -> K2Node_IfThenElse_0.execute
| Response: 2 'Replace existing output connections'
| Src(orphan=0 hidden=0 links=1 dir=1) Tgt(orphan=0 hidden=0 links=0 dir=0)
```

### Root Cause

Six reuse sites in `PhaseCreateNodes()` never call `Modify()` on the existing node:

1. **Event / CustomEvent reuse** (line ~663) -- `FindExistingEventNode`
2. **EnhancedInputAction reuse** (line ~706) -- `FindExistingEnhancedInputNode`
3. **ComponentBoundEvent reuse** (line ~746) -- `FindExistingComponentBoundEventNode`
4. **FunctionInput reuse** (line ~790) -- FunctionEntry node
5. **FunctionOutput reuse** (line ~860) -- FunctionResult node
6. (No others found)

### Fix

At each of the 5 reuse sites (FunctionInput and FunctionOutput share the same pattern), immediately after finding the existing node and before building the manifest, call:

```cpp
ExistingNode->Modify();
```

This registers the node with the active `FScopedTransaction` (owned by the write pipeline's Stage 3). If the transaction rolls back, UE's undo system will restore the node's pin state (including `LinkedTo` arrays), so stale `links=1` won't persist.

### Exact Locations in `OlivePlanExecutor.cpp`

All in `PhaseCreateNodes()`:

| Reuse Site | After Line | Insert After |
|---|---|---|
| Event/CustomEvent | ~661 (`if (ExistingNode)`) | Before `const FString ReuseNodeId = ...` at line ~667 |
| EnhancedInputAction | ~706 (`if (ExistingNode)`) | Before `const FString ReuseNodeId = ...` at line ~708 |
| ComponentBoundEvent | ~746 (`if (ExistingNode)`) | Before `const FString ReuseNodeId = ...` at line ~748 |
| FunctionEntry | ~790 (after finding EntryNode) | Before manifest build |
| FunctionResult | ~860 (after finding ResultNode) | Before manifest build |

Each insertion is a single line:

```cpp
ExistingNode->Modify();  // Register with FScopedTransaction for undo safety
```

For FunctionEntry/FunctionResult, the variable name is `EntryNode` / `ResultNode` respectively.

### Edge Cases

- **Modify on a node with no transaction active**: `Modify()` is a no-op when there's no active `FScopedTransaction`. Since we're always inside the write pipeline's Stage 3, this is safe. But even if called outside a transaction, it won't crash.
- **Double Modify**: If the node was already `Modify()`'d (e.g., by the graph or Blueprint-level `Modify()` in the pipeline), calling it again is harmless -- UE deduplicates.
- **Performance**: `Modify()` is cheap -- it just snapshots the object for undo. 5 extra calls per plan execution is negligible.

### Assignment

**Junior coder.** 5 lines of code across 5 sites. Pattern is identical at each site.

### Estimated Lines

~5 new lines (one `Modify()` call per reuse site), plus a brief comment at the first site explaining why.

---

## Fix 2: Handle BREAK_OTHERS for Exec Pins (P0)

### Problem

When `CanCreateConnection` returns `CONNECT_RESPONSE_BREAK_OTHERS_A` (response 2) for exec pins, our `Connect()` method rejects it because `CanSafeConnect()` only allows `CONNECT_RESPONSE_MAKE` and `CONNECT_RESPONSE_MAKE_WITH_PROMOTION`. The diagnostic then falls through to `EOliveWiringFailureReason::TypesIncompatible` because `BuildWiringDiagnostic` has no case for "already connected exec pin". The AI sees "TypesIncompatible" and chases false type-error leads.

**Two sub-problems:**
1. `Connect()` should handle `CONNECT_RESPONSE_BREAK_OTHERS_*` for exec pins (same behavior as the Blueprint editor's drag-and-drop)
2. `BuildWiringDiagnostic` should detect already-connected pins and report `AlreadyConnected` instead of `TypesIncompatible`

### Analysis: Is Auto-Breaking Safe?

The Blueprint editor auto-breaks exec connections when you drag a new wire. For plan_json execution, this is **desirable** behavior:

- Exec pins are single-output by design (one `then` pin goes to one target)
- When the plan says "step A exec_after step B", it means B's exec output should go to A -- any existing connection was either from a previous failed plan (rollback artifact) or from an earlier step in the same plan that the AI is intentionally overriding
- Phase 1.5 already uses `TryCreateConnection` directly (bypassing `Connect`), which does handle BREAK_OTHERS

However, we should NOT auto-break for data pins -- BREAK_OTHERS on data pins could silently disconnect important wires.

### Fix -- Two Parts

**Part A: Auto-break exec connections in `Connect()` (~10 lines)**

In `OlivePinConnector.cpp`, in the `Connect()` method, after the `CanSafeConnect()` check fails, add a case for exec pins where the response is `CONNECT_RESPONSE_BREAK_OTHERS_A`, `_B`, or `_AB`:

```cpp
// Current code (line ~67):
const bool bCanConnect = Response.CanSafeConnect() || bNeedsConversion;

// Replace with:
const bool bBreakOthers =
    (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A
     || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B
     || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB);

// Allow break-others for exec pins (matches Blueprint editor behavior)
const bool bExecAutoBreak = bBreakOthers
    && SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
    && TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;

const bool bCanConnect = Response.CanSafeConnect() || bNeedsConversion || bExecAutoBreak;
```

Since `TryCreateConnection` (called at line ~120) already handles BREAK_OTHERS responses internally (it breaks existing connections then makes the new one), no other changes are needed in the success path. Add a log message when `bExecAutoBreak` is true:

```cpp
if (bExecAutoBreak)
{
    UE_LOG(LogOlivePinConnector, Log,
        TEXT("Exec auto-break: breaking existing connection on %s.%s to make new connection to %s.%s"),
        *SourcePin->GetOwningNode()->GetName(), *SourcePin->PinName.ToString(),
        *TargetPin->GetOwningNode()->GetName(), *TargetPin->PinName.ToString());
}
```

**Part B: Detect AlreadyConnected in `BuildWiringDiagnostic()` (~15 lines)**

In `OlivePinConnector.cpp`, in `BuildWiringDiagnostic()`, add a check BEFORE the generic `TypesIncompatible` fallback (before the `else` at line ~808):

```cpp
// After the ObjectCastRequired case, before the generic else:
// Already-connected pin (BREAK_OTHERS response or source has links)
else if (SourcePin->LinkedTo.Num() > 0 || TargetPin->LinkedTo.Num() > 0)
{
    Diag.Reason = EOliveWiringFailureReason::AlreadyConnected;

    if (SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
    {
        Diag.WhyAutoFixFailed = FString::Printf(
            TEXT("Exec output '%s' is already connected to '%s'. "
                 "Disconnect first or use a different exec output pin (e.g., exec_outputs)."),
            *SourcePin->PinName.ToString(),
            SourcePin->LinkedTo.Num() > 0
                ? *SourcePin->LinkedTo[0]->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString()
                : TEXT("unknown"));
    }
    else
    {
        Diag.WhyAutoFixFailed = FString::Printf(
            TEXT("Pin '%s' already has %d connection(s). Disconnect existing wires first."),
            (SourcePin->LinkedTo.Num() > 0) ? *SourcePin->PinName.ToString() : *TargetPin->PinName.ToString(),
            FMath::Max(SourcePin->LinkedTo.Num(), TargetPin->LinkedTo.Num()));
    }
}
```

### Why Both Parts

Part A is the behavioral fix -- exec auto-break makes plan_json robust against stale wiring from rollback artifacts (the root cause from Fix 1). Part B is the diagnostic fix -- even with Fix 1 and Fix 2A, there are legitimate cases where an exec pin is already connected (e.g., the AI's plan wires two exec_after to the same source step's `then` pin, which is a plan error). Part B ensures the error message accurately describes the problem.

### Edge Cases

- **Data pin BREAK_OTHERS**: Intentionally NOT auto-breaking. Data pins can have multiple connections (fan-out from outputs). If `CanCreateConnection` returns BREAK_OTHERS for data pins, something unusual is happening and we should report it.
- **Multiple exec outputs**: Nodes like `Branch` have `True`/`False` exec outputs. BREAK_OTHERS only applies to the specific pin that already has a connection, not other pins on the same node.
- **Fix 1 interaction**: Fix 1 (Modify) prevents the root cause. Fix 2A (auto-break) is defense-in-depth for cases where stale connections survive for other reasons. They are complementary.

### Assignment

**Senior coder.** Part A modifies the connection logic's safety gate; Part B adds diagnostic categorization. Both require understanding the UE pin connection response model.

### Estimated Lines

~25 new lines across both parts.

### Files Modified

- `Source/OliveAIEditor/Blueprint/Private/Writer/OlivePinConnector.cpp` -- both parts

---

## Fix 3: Target-Aware Plan JSON (P1)

### Problem

When calling a function on a non-self actor (e.g., a spawned actor calls `K2_AttachToComponent`), plan_json has no mechanism to wire the `self` pin. The self pin is hidden on most nodes, and `FindPinSmart` excludes hidden pins. Phase 1.5 only auto-wires self pins for component-class functions with matching SCS components -- it doesn't handle arbitrary actor targets.

The AI must fall back to granular tools (`add_node` + `connect_pins`) to wire the self pin, costing ~2 minutes of agent time.

### Evidence

Plan created `K2_AttachToComponent` targeting self (character), but the AI wanted the spawned bow actor as Target. Had to recreate with granular tools.

### Fix -- Two Parts

**Part A: Support explicit "Target"/"self" input key in Phase 4 (~25 lines)**

In `PhaseWireData()` in `OlivePlanExecutor.cpp`, add a pin-key alias and hidden-pin override:

The existing `ResolvedPinKey` remapping (line ~2526) handles `set_var` "value" -> variable name. Add a similar remap for "Target" -> "self":

```cpp
// After the set_var ResolvedPinKey remap (line ~2532), add:
// "Target" is the AI-friendly name for the hidden "self" pin.
// When the AI explicitly provides Target=@some_step.auto, they want
// to wire to the node's self pin (which is hidden by default).
if (ResolvedPinKey.Equals(TEXT("Target"), ESearchCase::IgnoreCase)
    || ResolvedPinKey.Equals(TEXT("self"), ESearchCase::IgnoreCase))
{
    ResolvedPinKey = TEXT("self");
}
```

This alone won't work because `FindPinSmart` excludes hidden pins. We need to handle the "self" pin as a special case in `WireDataConnection()`.

In `WireDataConnection()`, after the `@self` reference handling block (line ~2812) and before the normal manifest-based resolution, add a special case: when the `TargetPinHint` (after remap) is "self", bypass `FindPinSmart` and directly find the hidden self pin on the node:

```cpp
// After the @self block, before "// 2. Get manifests":
// Special case: wiring to a node's hidden "self" pin (explicit Target input)
if (TargetPinHint.Equals(TEXT("self"), ESearchCase::IgnoreCase))
{
    UEdGraphNode* TargetNode = Context.GetNodePtr(TargetStepId);
    if (TargetNode)
    {
        UEdGraphPin* SelfPin = TargetNode->FindPin(UEdGraphSchema_K2::PN_Self);
        if (SelfPin)
        {
            // Parse source ref and wire to the self pin directly
            FString SourceStepId2, SourcePinHint2;
            if (ParseDataRef(SourceRef, SourceStepId2, SourcePinHint2))
            {
                // Resolve source pin via normal manifest path
                const FOlivePinManifest* SrcManifest = Context.GetManifest(SourceStepId2);
                UEdGraphNode* SrcNode = Context.GetNodePtr(SourceStepId2);

                if (SrcManifest && SrcNode)
                {
                    // Find source output pin (auto or named)
                    UEdGraphPin* SrcOutputPin = nullptr;
                    if (SourcePinHint2 == TEXT("auto"))
                    {
                        // Find first object-type output
                        for (UEdGraphPin* Pin : SrcNode->Pins)
                        {
                            if (Pin && Pin->Direction == EGPD_Output
                                && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                                && !Pin->bHidden)
                            {
                                SrcOutputPin = Pin;
                                break;
                            }
                        }
                    }
                    else
                    {
                        SrcOutputPin = SrcNode->FindPin(FName(*SourcePinHint2));
                    }

                    if (SrcOutputPin)
                    {
                        FOlivePinConnector& Connector = FOlivePinConnector::Get();
                        FOliveBlueprintWriteResult ConnectResult = Connector.Connect(
                            SrcOutputPin, SelfPin, /*bAllowConversion=*/true);
                        if (ConnectResult.bSuccess)
                        {
                            Result.bSuccess = true;
                            Result.SourceMatchMethod = TEXT("explicit_target");
                            Result.TargetMatchMethod = TEXT("self_pin_direct");
                            Result.ResolvedSourcePin = SrcOutputPin->GetName();
                            Result.ResolvedTargetPin = TEXT("self");
                            UE_LOG(LogOlivePlanExecutor, Log,
                                TEXT("  Data wire OK: @%s.%s -> step '%s'.self (explicit Target)"),
                                *SourceStepId2, *SourcePinHint2, *TargetStepId);
                            return Result;
                        }
                    }
                }
            }

            // If we found the self pin but couldn't wire, provide a clear error
            Result.ErrorMessage = FString::Printf(
                TEXT("Found self pin on step '%s' but could not wire from '%s'. "
                     "Ensure the source produces an actor/object reference."),
                *TargetStepId, *SourceRef);
            return Result;
        }
    }

    // No self pin found -- fall through to normal resolution
    // (might be a node without a self pin, e.g., a pure math node)
}
```

### Part B: Auto-Infer Target From Typed References -- DEFERRED

After analysis, Part B (auto-inferring Target in the resolver) is complex and error-prone:

1. The resolver runs BEFORE node creation, so we don't have real `UEdGraphPin*` objects to check type compatibility
2. The resolver would need to resolve the source step's return type, which requires `UFunction*` lookups that may not yet be available for all steps
3. Ambiguity detection is hard -- a plan with `spawn_actor` + `get_var(SomeActor)` + `call(AttachToComponent)` has two potential targets
4. The payoff is marginal: the AI can explicitly write `"Target": "@spawn_bow.auto"` now that Part A exists

**Recommendation**: Ship Part A first. If we see the AI consistently failing to write explicit Target inputs, revisit Part B as a separate design.

### What Part A Enables

```json
{"step_id": "attach", "op": "call", "target": "K2_AttachToComponent",
 "inputs": {
   "Target": "@spawn_bow.auto",
   "Parent": "@get_mesh.auto",
   "SocketName": "hand_r_socket"
 }}
```

The AI writes `"Target": "@spawn_bow.auto"`, Phase 4 resolves `Target` -> `self` pin, finds the hidden self pin directly on the node, and wires the spawn_bow output to it.

### System Prompt Update

The planner/builder system prompt (or knowledge pack) should document this capability:

```
When calling a function on a non-self actor, add "Target": "@step_id.auto" to inputs.
Example: AttachToComponent on a spawned actor:
  {"op": "call", "target": "K2_AttachToComponent",
   "inputs": {"Target": "@spawn_step.auto", "Parent": "@get_mesh.auto"}}
```

This should be added to `events_vs_functions.txt` or `blueprint_design_patterns.txt` in the knowledge pack directory.

### Edge Cases

- **No self pin**: Some nodes (static functions, pure math) have no self pin. The code falls through to normal resolution, which is correct.
- **Self pin already connected**: Phase 1.5 may have auto-wired the self pin to a component. The new explicit Target wire would need to break that connection. Since Part A routes through `FOlivePinConnector::Connect()`, and with Fix 2's exec auto-break... wait, self pins are data pins not exec. `TryCreateConnection` handles BREAK_OTHERS for data pins too. Let me verify: the self pin is an object reference input. If it already has a connection from Phase 1.5, `CanCreateConnection` will return BREAK_OTHERS_B. Our `Connect()` currently rejects this. **Fix**: Part A should use `TryCreateConnection` directly for the self pin wire, bypassing the `CanSafeConnect` gate. This is safe because the AI explicitly requested this Target wiring.

**Updated code for the connection section in Part A:**

```cpp
// Use TryCreateConnection directly for self pin (same as Phase 1.5).
// This handles BREAK_OTHERS responses by auto-breaking the existing
// connection, which is correct when the AI explicitly specifies Target.
const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
bool bConnected = Schema->TryCreateConnection(SrcOutputPin, SelfPin);
if (bConnected)
{
    Result.bSuccess = true;
    // ... rest of success handling
}
```

This is simpler and more robust than going through `FOlivePinConnector::Connect()`.

- **Multiple self pins**: UE nodes have at most one self pin (named "self"). Safe.
- **Phase 1.5 conflict**: If Phase 1.5 auto-wired a component to the self pin AND the AI also specified Target, Phase 4 runs after 1.5 and will break the 1.5 connection. This is correct -- explicit AI intent should override auto-wiring. Add a log warning when breaking an existing self pin connection.

### Assignment

**Senior coder.** Requires understanding hidden pin resolution, interaction with Phase 1.5 auto-wiring, and careful handling of the `ResolvedPinKey` alias chain.

### Estimated Lines

~50-60 new lines in `OlivePlanExecutor.cpp` (Phase 4 + WireDataConnection).

### Files Modified

- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp` -- PhaseWireData + WireDataConnection
- Knowledge pack text file (system prompt update, not C++ -- can be a separate task)

---

## Implementation Order

1. **Fix 1** (Junior) -- Modify() reused nodes. ~5 lines. Eliminates the root cause of stale exec pin connections surviving rollback.

2. **Fix 2** (Senior) -- Auto-break exec + AlreadyConnected diagnostic. ~25 lines. Defense-in-depth for Fix 1, plus improved error messages for legitimate already-connected cases.

3. **Fix 3 Part A** (Senior) -- Target/self pin wiring in Phase 4. ~55 lines. Enables a new plan_json capability.

4. **Fix 3 Part B** -- DEFERRED. Re-evaluate after observing agent behavior with Part A.

---

## Summary for Coder

### Fix 1 (Junior)
- **File**: `OlivePlanExecutor.cpp`, function `PhaseCreateNodes()`
- **What**: Add `ExistingNode->Modify()` (or `EntryNode->Modify()` / `ResultNode->Modify()`) at each of the 5 reuse sites, immediately after finding the existing node and before building the manifest
- **Search for**: `"Reused existing"` log messages and `ReusedStepIds.Add` to find all sites
- **Test**: Create a plan that reuses BeginPlay, let it fail (e.g., bad function name), then retry. The retry should succeed without "Response: 2" errors.

### Fix 2 (Senior)
- **File**: `OlivePinConnector.cpp`
- **Part A**: In `Connect()`, expand `bCanConnect` to include `CONNECT_RESPONSE_BREAK_OTHERS_*` when both pins are exec. Add log line.
- **Part B**: In `BuildWiringDiagnostic()`, add `AlreadyConnected` detection before the generic `TypesIncompatible` fallback. Check `SourcePin->LinkedTo.Num() > 0 || TargetPin->LinkedTo.Num() > 0`.

### Fix 3 Part A (Senior)
- **File**: `OlivePlanExecutor.cpp`
- **In `PhaseWireData()`**: Add `ResolvedPinKey` alias: "Target" -> "self" (case-insensitive)
- **In `WireDataConnection()`**: After the `@self` reference block, add special case for `TargetPinHint == "self"`. Bypass manifest, find hidden self pin directly via `FindPin(UEdGraphSchema_K2::PN_Self)`. Use `TryCreateConnection` directly (not `FOlivePinConnector::Connect`) to handle existing connections from Phase 1.5.
- **Log warning** if breaking an existing self pin connection.
