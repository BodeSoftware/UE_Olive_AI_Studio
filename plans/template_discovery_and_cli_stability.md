# Plan: Template Discovery + CLI Stability Fixes

## Problem Summary

Three issues identified from testing:

1. **Templates never used** — The CLI provider doesn't inject the template catalog into its system prompt, `cli_blueprint.txt` doesn't mention templates, and first-turn routing explicitly tells the AI to skip discovery and go straight to `blueprint.create`. Result: AI manually builds projectiles in 5-10 iterations instead of using `blueprint.create_from_template("projectile", preset="Bullet")` in one call.

2. **CLI hang** — When the CLI process never responds (e.g., network issues, model overloaded), there's no timeout. The read loop in `SendMessage()` blocks the background thread forever at the `while (IsProcRunning && !bStopReading)` loop (line 313).

3. **Crash on cancel+re-run** — Cancelling a stuck run then immediately starting a new one (375ms gap) causes a null pointer crash at line 381. `CancelRequest()` sets `bIsBusy = false` and kills the process, but the background thread's `AsyncTask` to `HandleResponseComplete` can still race and fire after the new `SendMessage()` has already started.

4. **Missing UE event vocabulary** — The AI doesn't know which Actor events exist, their internal names, or their output pin signatures. This causes self-correction loops when it guesses wrong event names or doesn't know what pins are available. The resolver has a hardcoded `EventNameMap` (10 entries) but this knowledge isn't surfaced to the AI.

---

## Task 1: Inject Template Catalog into CLI System Prompt

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**What:** After the `cli_blueprint` knowledge pack injection (line 557), add a new section that fetches and injects the template catalog from `FOliveTemplateSystem`.

**Where (line ~557):** After the `CLIBlueprint` block, before the tool schemas block:

```cpp
// After cli_blueprint block (around line 557):

// ==========================================
// Template catalog (factory + reference templates)
// ==========================================
if (FOliveTemplateSystem::Get().HasTemplates())
{
    const FString& Catalog = FOliveTemplateSystem::Get().GetCatalogBlock();
    if (!Catalog.IsEmpty())
    {
        SystemPrompt += Catalog;
        SystemPrompt += TEXT("\n\n");
    }
}
```

**Include needed:** Add `#include "Template/OliveTemplateSystem.h"` at top of file.

---

## Task 2: Update `cli_blueprint.txt` — Add Template-First Routing

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**What:** Add a template check step to the CREATE workflow and add a TEMPLATE section.

**Change the CREATE workflow (line 5-6)** from:
```
CREATE new Blueprint:
1. blueprint.create → 2. add_component/add_variable → 3. blueprint.apply_plan_json (ALL graph logic in one call)
```
to:
```
CREATE new Blueprint:
1. Check if a template fits (see Templates below) → blueprint.create_from_template
2. If no template fits: blueprint.create → add_component/add_variable → blueprint.apply_plan_json (ALL graph logic in one call)
```

**Add a new section before "## Rules"** (before line 57):
```
## Templates
Before creating a Blueprint from scratch, check if an available template matches.
- blueprint.create_from_template(template_id, preset, asset_path) creates a complete Blueprint in one call.
- Templates include components, variables, event dispatchers, functions, and event graphs.
- Use a preset name if one matches (e.g., "Bullet", "Rocket" for projectile template).
- If no template matches, proceed with the normal CREATE workflow above.
```

---

## Task 3: Update First-Turn Routing in `BuildConversationPrompt()`

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**What:** Change the first-turn routing directive (line 498) to check templates first.

**Change line 498** from:
```cpp
Prompt += TEXT("- If the task is creating NEW Blueprints, start with blueprint.create (do NOT search first).\n");
```
to:
```cpp
Prompt += TEXT("- If the task is creating NEW Blueprints, check if a template fits first (blueprint.create_from_template). Otherwise use blueprint.create.\n");
```

---

## Task 4: Update `recipe_routing.txt` — Add Template Mention

**File:** `Content/SystemPrompts/Knowledge/recipe_routing.txt`

**What:** Add template awareness to the routing guidance.

**Change line 3** from:
```
- NEW blueprint: create + components/variables + apply_plan_json (ALL graph nodes in one call)
```
to:
```
- NEW blueprint: check templates first (blueprint.create_from_template). If none fit: create + components/variables + apply_plan_json
```

---

## Task 5: Add CLI Process Timeout (Watchdog)

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**What:** Add a timeout to the read loop so a hung CLI process doesn't block forever.

**Where (line 313):** The inline read loop currently has no time limit:
```cpp
while (FPlatformProcess::IsProcRunning(ProcessHandle) && !bStopReading)
```

**Add a timer before the loop (around line 310):**
```cpp
const double ProcessStartTime = FPlatformTime::Seconds();
const double ProcessTimeoutSeconds = 180.0; // 3 minutes max per CLI invocation
```

**Add a timeout check inside the loop (after line 313):**
```cpp
while (FPlatformProcess::IsProcRunning(ProcessHandle) && !bStopReading)
{
    // Check for timeout
    if (FPlatformTime::Seconds() - ProcessStartTime > ProcessTimeoutSeconds)
    {
        UE_LOG(LogOliveCLIProvider, Warning, TEXT("%s process timed out after %.0f seconds"), *CLIName, ProcessTimeoutSeconds);
        bStopReading = true;
        FPlatformProcess::TerminateProc(ProcessHandle, true);
        break;
    }

    // ... existing read logic ...
```

After the loop exits, the existing cleanup code handles pipe closing and return code. If we broke out via timeout, `ReturnCode` from `GetProcReturnCode` will be non-zero (or the process is terminated), so `HandleResponseComplete` will fire the error callback with "process exited with code X".

**Also add a named constant** at the top of the file (anonymous namespace or static):
```cpp
namespace
{
    /** Maximum seconds to wait for a CLI process to produce output before timing out */
    constexpr double CLI_PROCESS_TIMEOUT_SECONDS = 180.0;
}
```

**Refinement — idle timeout vs. total timeout:** We actually want an *idle* timeout (no output for N seconds), not a total timeout. A long-running process that IS producing output is fine. Change to:

```cpp
const double IdleTimeoutSeconds = 120.0; // 2 minutes with no output = hung
double LastOutputTime = FPlatformTime::Seconds();

while (FPlatformProcess::IsProcRunning(ProcessHandle) && !bStopReading)
{
    FString Chunk = FPlatformProcess::ReadPipe(StdoutRead);
    if (!Chunk.IsEmpty())
    {
        LastOutputTime = FPlatformTime::Seconds(); // Reset idle timer
        OutputBuffer += Chunk;
        // ... existing line processing ...
    }
    else
    {
        // Check idle timeout
        if (FPlatformTime::Seconds() - LastOutputTime > IdleTimeoutSeconds)
        {
            UE_LOG(LogOliveCLIProvider, Warning, TEXT("%s process idle for %.0f seconds — terminating"), *CLIName, IdleTimeoutSeconds);
            bStopReading = true;
            FPlatformProcess::TerminateProc(ProcessHandle, true);
            break;
        }
        FPlatformProcess::Sleep(0.01f);
    }
}
```

This keeps the `else { Sleep(0.01f) }` branch but adds the idle check before sleeping.

---

## Task 6: Fix Cancel-Then-Rerun Race Condition

**File:** `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp`

**What:** After `CancelRequest()` kills the process, the background thread's `AsyncTask` to game thread can still fire and call `HandleResponseComplete`. The `bIsBusy` guard catches most cases, but there's a window where a new `SendMessage()` sets `bIsBusy = true` BEFORE the old `AsyncTask` runs.

**Fix approach — add a generation counter:**

In the header (`OliveCLIProviderBase.h`), add a new member:
```cpp
/** Generation counter to distinguish stale async completions from current request */
std::atomic<uint32> RequestGeneration{0};
```

In `SendMessage()`, increment it when starting a new request (after setting `bIsBusy = true`, around line 159):
```cpp
bIsBusy = true;
const uint32 ThisGeneration = ++RequestGeneration;
AccumulatedResponse.Empty();
```

Capture `ThisGeneration` in the background lambda, and check it before dispatching completion:

**Change the completion AsyncTask (line 379):**
```cpp
// Capture ThisGeneration in the background lambda
AsyncTask(ENamedThreads::GameThread, [this, ReturnCode, ThisGeneration]()
{
    FScopeLock Lock(&CallbackLock);
    if (!bIsBusy) return;
    if (RequestGeneration != ThisGeneration) return; // Stale completion from cancelled request
    HandleResponseComplete(ReturnCode);
});
```

**Also guard the line-parsing AsyncTasks (line 330):**
```cpp
AsyncTask(ENamedThreads::GameThread, [this, Line, ThisGeneration]()
{
    FScopeLock Lock(&CallbackLock);
    if (!bIsBusy) return;
    if (RequestGeneration != ThisGeneration) return; // Stale
    ParseOutputLine(Line);
});
```

**And the final buffer flush AsyncTask (line 362):**
```cpp
AsyncTask(ENamedThreads::GameThread, [this, OutputBuffer, ThisGeneration]()
{
    FScopeLock Lock(&CallbackLock);
    if (!bIsBusy) return;
    if (RequestGeneration != ThisGeneration) return; // Stale
    ParseOutputLine(OutputBuffer);
});
```

This way, if a cancel fires `++RequestGeneration` and then a new `SendMessage` fires `++RequestGeneration` again, the old background thread's completions are silently dropped because their captured generation doesn't match.

**Also increment in `CancelRequest()`** (before killing the process):
```cpp
void FOliveCLIProviderBase::CancelRequest()
{
    bStopReading = true;
    ++RequestGeneration; // Invalidate any in-flight async tasks from old request

    {
        FScopeLock Lock(&CallbackLock);
        // ... existing cleanup ...
    }
    KillProcess();
}
```

---

## Task 7: Create UE Event Mapping Reference Template

**File:** `Content/Templates/reference/ue_events.json` (NEW FILE)

**What:** A reference template documenting all Blueprint-overridable Actor events — their user-friendly names, internal names, and output pin signatures. This is the vocabulary the AI needs to wire event graphs correctly without guessing.

The resolver already maps these 10 events (in `OliveBlueprintPlanResolver.cpp` line 867):
- BeginPlay → ReceiveBeginPlay
- EndPlay → ReceiveEndPlay
- Tick → ReceiveTick
- ActorBeginOverlap → ReceiveActorBeginOverlap
- ActorEndOverlap → ReceiveActorEndOverlap
- AnyDamage → ReceiveAnyDamage
- Hit → ReceiveHit
- PointDamage → ReceivePointDamage
- RadialDamage → ReceiveRadialDamage
- Destroyed → ReceiveDestroyed

**Template structure** (follows `component_patterns.json` pattern):

```json
{
    "template_id": "ue_events",
    "template_type": "reference",
    "display_name": "UE Actor Event Reference",

    "catalog_description": "Actor event reference: BeginPlay, Tick, overlap, damage, hit events with correct names and output pin signatures. Use op:event with the user-friendly name (resolver handles the Receive prefix automatically).",
    "catalog_examples": "",

    "tags": "event beginplay tick overlap damage hit destroyed endplay actor",

    "patterns": [
        {
            "name": "BeginPlay",
            "description": "Fires once when the actor starts playing (after all components initialized). Most common event for setup logic.",
            "notes": "Use op:event, target:BeginPlay. No output data pins — exec only.",
            "plan_example": {
                "step_id": "evt",
                "op": "event",
                "target": "BeginPlay"
            }
        },
        {
            "name": "Tick",
            "description": "Fires every frame. Has DeltaSeconds output pin.",
            "notes": "Use op:event, target:Tick. Output pins: DeltaSeconds (Float). Must enable ticking on the actor first.",
            "plan_example": {
                "step_id": "evt",
                "op": "event",
                "target": "Tick"
            }
        },
        {
            "name": "EndPlay",
            "description": "Fires when the actor is being removed from the world.",
            "notes": "Use op:event, target:EndPlay. Output pins: EndPlayReason (EEndPlayReason).",
            "plan_example": {
                "step_id": "evt",
                "op": "event",
                "target": "EndPlay"
            }
        },
        {
            "name": "ActorBeginOverlap",
            "description": "Fires when another actor overlaps this actor. Requires collision with Generate Overlap Events enabled.",
            "notes": "Use op:event, target:ActorBeginOverlap. Output pins: OtherActor (Actor).",
            "plan_example": {
                "step_id": "evt",
                "op": "event",
                "target": "ActorBeginOverlap"
            }
        },
        {
            "name": "ActorEndOverlap",
            "description": "Fires when another actor stops overlapping this actor.",
            "notes": "Use op:event, target:ActorEndOverlap. Output pins: OtherActor (Actor).",
            "plan_example": {
                "step_id": "evt",
                "op": "event",
                "target": "ActorEndOverlap"
            }
        },
        {
            "name": "Hit",
            "description": "Fires when this actor is hit by something (physics collision).",
            "notes": "Use op:event, target:Hit. Output pins: MyComp (PrimitiveComponent), Other (Actor), OtherComp (PrimitiveComponent), NormalImpulse (Vector), HitResult (HitResult).",
            "plan_example": {
                "step_id": "evt",
                "op": "event",
                "target": "Hit"
            }
        },
        {
            "name": "AnyDamage",
            "description": "Fires when the actor receives any type of damage.",
            "notes": "Use op:event, target:AnyDamage. Output pins: Damage (Float), DamageType (DamageType), InstigatedBy (Controller), DamageCauser (Actor).",
            "plan_example": {
                "step_id": "evt",
                "op": "event",
                "target": "AnyDamage"
            }
        },
        {
            "name": "PointDamage",
            "description": "Fires when the actor receives point damage (e.g., from a projectile hit).",
            "notes": "Use op:event, target:PointDamage. Output pins: Damage (Float), DamageType (DamageType), HitLocation (Vector), HitNormal (Vector), HitComponent (PrimitiveComponent), BoneName (Name), ShotFromDirection (Vector), InstigatedBy (Controller), DamageCauser (Actor), HitInfo (HitResult).",
            "plan_example": {
                "step_id": "evt",
                "op": "event",
                "target": "PointDamage"
            }
        },
        {
            "name": "RadialDamage",
            "description": "Fires when the actor receives radial (area-of-effect) damage.",
            "notes": "Use op:event, target:RadialDamage. Output pins: DamageReceived (Float), DamageType (DamageType), Origin (Vector), HitInfo (HitResult), InstigatedBy (Controller), DamageCauser (Actor).",
            "plan_example": {
                "step_id": "evt",
                "op": "event",
                "target": "RadialDamage"
            }
        },
        {
            "name": "Destroyed",
            "description": "Fires when the actor is being destroyed.",
            "notes": "Use op:event, target:Destroyed. No output data pins — exec only.",
            "plan_example": {
                "step_id": "evt",
                "op": "event",
                "target": "Destroyed"
            }
        }
    ]
}
```

**Key details:**
- Each event lists its **output pin names and types** — this is the critical missing knowledge
- The `plan_example` shows the exact plan JSON syntax
- Notes remind the AI to use user-friendly names (resolver handles `Receive` prefix)
- Template system auto-discovers it from `Content/Templates/reference/` directory
- The catalog block will include it alongside `component_patterns` once Task 1 injects the catalog into CLI prompts

---

## File Summary

| Task | File | Change |
|------|------|--------|
| 1 | `OliveCLIProviderBase.cpp` | Add template catalog injection to `BuildCLISystemPrompt()` |
| 2 | `cli_blueprint.txt` | Add template-first CREATE workflow + Templates section |
| 3 | `OliveCLIProviderBase.cpp` | Update first-turn routing directive |
| 4 | `recipe_routing.txt` | Add template mention to routing |
| 5 | `OliveCLIProviderBase.cpp` | Add idle timeout to read loop |
| 6 | `OliveCLIProviderBase.h` + `OliveCLIProviderBase.cpp` | Add RequestGeneration counter to prevent stale completions |
| 7 | `Content/Templates/reference/ue_events.json` | NEW — UE event mapping reference with pin signatures |

## Dependency Order

- Tasks 1-4, 7 are independent (template discovery — can be done in parallel)
- Tasks 5-6 are independent (CLI stability — can be done in parallel)
- All 7 tasks can technically run in parallel since they touch different files/sections

## Coder Grouping (for parallel execution)

**Coder A** — Tasks 1, 3, 5, 6 (all in `OliveCLIProviderBase.cpp/.h` — same files, best done by one coder)
**Coder B** — Tasks 2, 4 (text file updates — `cli_blueprint.txt` + `recipe_routing.txt`)
**Coder C** — Task 7 (new JSON file — `ue_events.json`)

## Build Verification

After all tasks: run UBT build to verify compilation (only Tasks 1/3/5/6 touch C++ — Tasks 2/4/7 are content files).
