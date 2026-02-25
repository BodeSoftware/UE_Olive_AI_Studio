# Olive AI — Let the AI Design, Let the Compiler Handle UE

## Philosophy

The AI is good at design. Ask any model to produce a Blueprint plan JSON in a clean context and it nails it. The problem is our pipeline forces the AI to fight UE implementation details — pin names, K2 function aliases, type conversions, event naming conventions — instead of letting it think at design level.

The fix isn't more rules, more correction directives, or more prompt micromanagement. It's making the tool layer smart enough that the AI's natural design-level thinking "just works." The AI says what it means in human terms, the compiler translates to UE internals.

Think of it like a programming language compiler — the AI writes high-level intent, the resolver lowers it to UE Blueprint IR. Right now the compiler is too thin and the AI is essentially writing assembly.

---

## Part 1: CLI Base Class Extraction

### Why

`FOliveClaudeCodeProvider` mixes universal CLI process management with Claude-specific details. Any future CLI provider (Codex, Gemini CLI, etc.) would rediscover the same bugs. Extract `FOliveCLIProviderBase` so all CLI providers inherit the solved problems.

### What Moves to Base

- Stdin pipe delivery (avoids 32KB cmd-line limit — OS-level, not Claude-specific)
- Process spawn, stdout reader loop, pipe cleanup
- HandleResponseComplete with `FOliveCLIToolCallParser` (any CLI using `<tool_call>` XML)
- Crash → Transient error classification (any CLI can crash)
- Conversation prompt formatting (`[User]`/`[Assistant]`/`[Tool Result]`)
- System prompt assembly via `PromptAssembler` + tool schema serialization
- Action directives (`## Next Action Required`)

### What Stays in FOliveClaudeCodeProvider (~80 lines)

- `GetExecutablePath()` — where is `claude`
- `GetCLIArguments()` — `--print --output-format stream-json --max-turns 1 --strict-mcp-config`
- `ParseOutputLine()` — Claude's stream-json format
- Model list, validation, static helpers

### Adding a Future CLI Provider

```cpp
class FOliveCodexProvider : public FOliveCLIProviderBase
{
    FString GetExecutablePath() const override;      // where is codex
    FString GetCLIArguments(...) const override;      // codex-specific flags
    void ParseOutputLine(const FString& Line) override; // codex output format
    TArray<FString> GetAvailableModels() const override;
    // ... that's it. Everything else inherited.
};
```

### Steps

1. Create `OliveCLIProviderBase.h/.cpp`
2. Move process lifecycle from ClaudeCodeProvider::SendMessage → base SendMessage with virtual hooks
3. Move BuildPrompt → base BuildConversationPrompt
4. Move BuildSystemPrompt → base BuildCLISystemPrompt
5. Move HandleResponseComplete → base
6. Move KillProcess, CancelRequest, pipe members → base
7. Slim ClaudeCodeProvider to inherit from base, remove moved code
8. Update includes
9. Verify error strings match ClassifyError patterns
10. Test: same behavior, crash recovery, large history, cancel

---

## Part 2: Let the AI Design Freely (Resolver Absorbs UE Trivia)

The AI should think like a human designer. Humans say "BeginPlay," "Location," "GetActorTransform," "MakeTransform." UE internally uses `ReceiveBeginPlay`, `SpawnTransform`, `K2_GetActorTransform`, `KismetMathLibrary::MakeTransform`. The resolver should handle this translation silently — the AI never knows or cares.

### R1: Event Name Mapping

**File:** `OliveBlueprintPlanResolver.cpp`

AI says `BeginPlay`. UE wants `ReceiveBeginPlay`. Map it at resolve time, not at factory time where it fails late inside an open transaction.

```
BeginPlay → ReceiveBeginPlay
Tick → ReceiveTick
EndPlay → ReceiveEndPlay
ActorBeginOverlap → ReceiveActorBeginOverlap
ActorEndOverlap → ReceiveActorEndOverlap
AnyDamage → ReceiveAnyDamage
Hit → ReceiveHit
```

~20 lines. Eliminates the first failure on every new Blueprint.

### R2: SpawnActor Input Expansion

**File:** `OliveBlueprintPlanResolver.cpp`

AI thinks in Location/Rotation because that's how humans think. SpawnActor's actual pin is `SpawnTransform` (a Transform). When the resolver sees `Location`/`Rotation` inputs on a `spawn_actor` step, auto-synthesize a MakeTransform step internally and rewire. The AI's design intent was correct — the compiler handles the plumbing.

This is exactly what the UE editor does when you drag a Location wire onto SpawnActor — it offers to create a MakeTransform for you.

~60 lines. Eliminates the entire 8-minute MakeTransform detour.

### R3: Wire-Time Auto-Conversion

**File:** `OlivePlanExecutor.cpp` (PhaseWireData)

`PinConnector` already has `InsertConversionNode`, `GetConversionOptions`, `CanAutoConvert`. PhaseWireData just doesn't use them — it calls `Connect` with `bAllowConversion = false`. Flip to `true`. Vector→Transform, Rotator→Quat, etc. auto-convert just like the Blueprint editor does when you connect incompatible pins.

~1 line change. Eliminates an entire category of wiring failures.

### R4: Function Alias Gaps

**File:** `OliveFunctionResolver.cpp`

The AI uses natural function names. Fill the gaps in the alias table:

```
GetActorTransform → K2_GetActorTransform
MakeTransform → KismetMathLibrary::MakeTransform
GetMesh → GetComponentByClass (SkeletalMeshComponent)
SetActorLocation → K2_SetActorLocation
SetActorRotation → K2_SetActorRotation
```

~10 lines. Each prevents a common resolution failure that triggers retry snowballs.

### R5: Auto-Chain Function Entry (Bug Fix)

**File:** `OlivePlanExecutor.cpp` (PhaseWireExec)

The prompt tells the AI "first impure step doesn't need exec_after, it auto-chains from function entry." The executor never actually does that. The AI follows the instructions correctly, omits `exec_after`, and nothing gets wired. Fix the executor to deliver on the promise.

~30 lines.

### R6: Honest Partial Failure Reporting (Bug Fix)

**File:** `OliveBlueprintToolHandlers.cpp`

`Result.bSuccess` only checks node creation, not wiring. AI gets `success: true` with broken wires buried in nested data. It moves on thinking everything worked. Return `partial_success` or `success: false` when wiring failures exist so the AI gets honest feedback and can decide how to handle it.

~15 lines.

### R7: INVALID_EXEC_REF Guidance

**File:** `OliveSelfCorrectionPolicy.cpp` or `BuildToolErrorMessage`

When the AI uses K2Node IDs from `blueprint.read` as `exec_after` values, it gets `INVALID_EXEC_REF` with no explanation. Add a clear message: "exec_after takes plan step_ids, not K2Node IDs." Let the AI understand the mistake and fix it naturally.

~10 lines.

### R8: Verbose Param Validation Logging

**File:** Various tool handlers in `OliveBlueprintToolHandlers.cpp`

Some tool calls fail silently in 0.06ms — early-return paths with no logging. Add `UE_LOG` so failures are visible. This helps both debugging and the AI (if error messages propagate back).

~30 lines.

---

## Part 3: Fix Prompts That Teach Wrong Things

### P1: Plan Complexity Guidance

Add to recipe_routing.txt QUICK RULES:
- Keep plans under 12 steps, split complex logic into multiple functions
- Never invent node IDs — only use IDs from tool results
- exec_after takes step_ids, not K2Node IDs

### P2: Fix Bad SpawnActor Example

The hardcoded example in `BuildPrompt` teaches `"inputs":{"Location":"@get_loc.auto"}` — wrong pin name. Fix to `"inputs":{"SpawnTransform":"@make_tf.auto"}` or remove once R2 handles the translation.

### P3: make_struct Example

Listed in ops but never shown. AI doesn't know it exists, tries `op:"call"` + `target:"MakeTransform"` instead. Add one-line example.

---

## Part 4: Better Self-Correction (Light Touch)

Not heavy-handed forced retries or blocking completion — just honest feedback so the AI can make informed decisions:

- **Fix 3:** System prompt rules for preview ordering and asset targeting — guidance, not enforcement
- **Fix 4:** Aggressive distillation to reduce context bloat — give the AI more room to think
- **Fix 5:** Error-specific correction hints — tell the AI what went wrong clearly, let it decide how to fix it
- **Fix 6:** Correction directive when errors are pending — surface the errors prominently so the AI sees them, don't force specific actions
- **Fix 7:** Don't silently accept text-only responses when errors are pending — flag it, let the AI decide to retry or explain why it can't

---

## Files Changed

| File | Action | Section |
|------|--------|---------|
| `Providers/OliveCLIProviderBase.h` | **NEW** | Part 1 |
| `Providers/OliveCLIProviderBase.cpp` | **NEW** | Part 1 |
| `Providers/OliveClaudeCodeProvider.h` | **MODIFY** — change parent, remove moved members | Part 1 |
| `Providers/OliveClaudeCodeProvider.cpp` | **MODIFY** — remove moved methods | Part 1 |
| `Brain/OliveBlueprintPlanResolver.cpp` | **MODIFY** — R1 event mapping, R2 spawn expansion | Part 2 |
| `Brain/OlivePlanExecutor.cpp` | **MODIFY** — R3 auto-conversion, R5 auto-chain | Part 2 |
| `Brain/OliveFunctionResolver.cpp` | **MODIFY** — R4 alias gaps | Part 2 |
| `Tools/OliveBlueprintToolHandlers.cpp` | **MODIFY** — R6 honest reporting, R8 logging | Part 2 |
| `Brain/OliveSelfCorrectionPolicy.cpp` | **MODIFY** — R7 INVALID_EXEC_REF | Part 2 |
| `Content/SystemPrompts/Knowledge/recipe_routing.txt` | **MODIFY** — P1 | Part 3 |
| `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | **MODIFY** — P3 | Part 3 |

---

## Priority

| Order | What | Why |
|-------|------|-----|
| **First** | R1, R5, R6 | Broken promises — event names fail every run, auto-chain is a lie, success masking hides failures |
| **Second** | R2, R3, R4 | Biggest AI freedom gains — SpawnActor, auto-conversion, and aliases let the AI think naturally |
| **Third** | Base class extraction (Part 1) | Infrastructure for future CLIs, but Claude Code works today |
| **Fourth** | P1, P2, P3, R7, R8 | Quick wins — small changes, each prevents a specific failure |
| **Fifth** | Part 4 (self-correction) | Light-touch improvements, can be done in parallel |
