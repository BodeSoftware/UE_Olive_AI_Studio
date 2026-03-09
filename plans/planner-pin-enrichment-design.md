# Design: Planner Pin Enrichment

## Problem Statement

The Builder (Claude Code CLI) writes `blueprint.apply_plan_json` based on function descriptions from the Build Plan. Pin names are hallucinated because the Planner writes vague descriptions like "call ApplyPointDamage to deal damage" instead of listing exact pin names. The Builder then guesses pin names from LLM training memory, which is wrong for many UE APIs.

**Recurring failures across runs 08d-08h (same root cause each time):**
- `AttachToComponent`: Builder uses `AttachType` -- real pins are `AttachmentRule`, `LocationRule`, `RotationRule`, `ScaleRule`
- `ApplyPointDamage`: Builder uses `HitInfo` -- real pin is `HitResult`
- `OnComponentBeginOverlap`: Builder uses `~Normal` -- real pins are `OtherActor`, `OtherComp`, `OtherBodyIndex`, `bFromSweep`, `SweepResult`
- `GetForwardVector`: Builder uses `InRot` input -- function has no rotator input
- `SetSpeed`, `SetInitialSpeed`, `SetMaxSpeed`: Builder calls functions that don't exist -- these are UPROPERTYs, need `set_var`
- `Set bOrientRotationToMovement`: C++ property, not BlueprintCallable

The plan_json first-attempt success rate has been stuck at ~55% across 4 runs despite many other improvements. Pin hallucination is the dominant remaining failure class.

## What the Code Currently Does

### Planner System Prompt (`BuildPlannerSystemPrompt()`, line 2833)

The Planner is told to produce function descriptions with "node-level detail":

```
"Function descriptions must include node-level detail.
 Example: 'Get ProjectileMovementComp via GetComponentByClass -> set InitialSpeed,
 MaxSpeed via property access -> store Instigator ref as variable'
 Include: component access patterns, function call targets, branch conditions,
 variable get/set operations, and any by-ref pins that need wires."
```

There is **no instruction to include pin names or parameter types**. The word "pin" does not appear in the Planner prompt at all.

### What the Planner Has Access To (MCP tools, line 2290)

The Planner (in `RunPlannerWithTools` mode) has exactly 4 tools:
1. `blueprint.get_template` -- reads library template JSON with **full pin-level node data**
2. `blueprint.list_templates` -- searches templates by keyword
3. `blueprint.describe` -- reads an existing Blueprint's structure
4. `olive.get_recipe` -- gets tested wiring patterns

It does NOT have `blueprint.describe_node_type` (which creates a scratch node and returns its exact pins via `BuildPinManifest`).

### What Templates Contain (pin data is rich)

Library template JSON includes full node graphs with exact pin names, types, and connections:
```json
{
  "name": "InSkeletalMeshComponent",
  "type": { "category": "object", "class": "SkeletalMeshComponent" }
},
{
  "name": "MontageToPlay",
  "type": { "category": "object", "class": "AnimMontage" }
}
```

The Planner reads these via `get_template` calls (13 calls in run 08g, studying 7 specific functions). **The pin data is right there in the Planner's context.** It just doesn't extract it into the Build Plan.

### How the Build Plan Is Injected (line 478)

The Build Plan text is injected **verbatim** into the Builder's prompt at Section 3 of `FormatForPromptInjection()`. No post-processing, no enrichment. Whatever the Planner wrote is exactly what the Builder sees.

### Existing C++ Infrastructure

- `FindFunctionEx()` resolves any function name to `UFunction*` via the 7+1 step search (alias map, Blueprint, parent classes, SCS, interfaces, libraries, universal scan). Returns full `UFunction*` with all parameter metadata.
- `FOliveClassAPIHelper::GetCallableFunctions()` returns function **names only** (no parameters).
- `BuildComponentAPIMap()` already injects a compact API reference for component classes into the Builder prompt (Section 3.5), but lists function names only, not signatures.
- `BuildPinManifest()` creates a full pin listing from any `UEdGraphNode*` -- used by `describe_node_type`.

## Options Evaluated

### Option A: Instruct Planner to Include Pin Names

**Description:** Change `BuildPlannerSystemPrompt()` to explicitly tell the Planner to include key input pin names and types for each function call it describes. The Planner already reads template node graphs and sees exact pin data -- it just needs to be told to extract and include it.

**Feasibility:** High. The Planner already makes 13 tool calls and reads function graphs. Adding "include exact pin names for non-obvious parameters" to the system prompt is a text change. The Planner LLM is capable of extracting pin names from the JSON it reads.

**Tradeoffs:**
- Pro: Systemic fix -- every function the Planner researches via templates gets pin names automatically
- Pro: No new code, no new dependencies, no new tool calls
- Pro: The data source (templates) is ground truth from real Blueprints
- Con: Only covers functions that appear in templates the Planner reads. Novel functions not in any template still get guessed pin names.
- Con: Relies on LLM compliance -- the Planner might forget to include pin names for some functions
- Con: Build Plan size increases. Currently ~7765 chars (08g). Adding `(DamagedActor: Actor, BaseDamage: Float, HitFromDirection: Vector, HitResult: HitResult, DamageTypeClass: Class, DamageCauser: Actor)` per function adds ~80-120 chars per function. With ~15-20 functions across 3 assets, that is +1200-2400 chars. Manageable.

**Verdict:** Necessary but not sufficient. Covers template-backed functions well but leaves a gap for functions not in any template (e.g., `GetForwardVector`, `AttachToComponent`, `SetActorRotation`).

### Option B: Builder Pre-Flight Describe Calls

**Description:** Add a workflow directive telling the Builder to call `blueprint.describe_node_type` for every function in its plan_json before executing it.

**Feasibility:** Low. Each function requires a separate tool call. A plan_json with 8 call ops would need 8 `describe_node_type` calls first. In run 08g, there were 22 plan_json attempts -- at ~4 call ops each, that is ~88 extra tool calls. Current Builder run was 88 total calls. This would double execution time.

**Tradeoffs:**
- Pro: Gets ground-truth pin data for every function
- Con: Massively increases tool call count and run time
- Con: `describe_node_type` resolves K2Node classes, not UE functions. Calling it with "ApplyPointDamage" resolves to `K2Node_CallFunction` which has generic pins (self, execute, then) -- not the function-specific pins. The function-specific pins only appear after `SetFromFunction()` is called, which `describe_node_type` does not do for arbitrary function names.
- Con: Adds a new workflow dependency -- if the Builder skips the pre-flight, it is back to hallucinating

**Verdict:** Rejected. Wrong tool for the job (`describe_node_type` describes node classes, not function signatures), and the cost is prohibitive.

### Option C: Static Pin Reference for Top N Functions

**Description:** Create a knowledge pack with exact pin names for commonly hallucinated functions. Inject into Builder's system prompt.

**Feasibility:** Easy to implement. A 50-line file with the 20 most-problematic functions.

**Tradeoffs:**
- Pro: Zero additional latency, zero tool calls
- Pro: 100% reliable for covered functions
- Con: **This is exactly the whack-a-mole pattern the user rejected.** The list is finite and will never cover all functions. Every new test run will discover more functions that need to be added.
- Con: Does not scale -- UE has ~5000 BlueprintCallable functions

**Verdict:** Rejected. Explicitly against the stated design goals.

### Option D: Give Planner `blueprint.describe_node_type`

**Description:** Add `blueprint.describe_node_type` to the Planner's MCP tool filter so it can look up pin data for functions it's uncertain about.

**Feasibility:** Low for the same reason as Option B. `describe_node_type` resolves **K2Node classes**, not function signatures. Calling it with "ApplyPointDamage" or "AttachToComponent" gives you generic `K2Node_CallFunction` pins, not the function-specific ones.

**Verdict:** Rejected. The tool does not answer the question being asked.

### Option E (New): C++ Post-Processing -- Enrich Build Plan with Function Signatures

**Description:** After the Planner produces the Build Plan text and `ParseBuildPlan()` extracts structural data, add a new C++ post-processing pass that:
1. Scans the Build Plan text for function names mentioned in `call` patterns
2. Resolves each to `UFunction*` via `FindFunctionEx()`
3. Extracts parameter names and types from the `UFunction*`
4. Appends a compact "Pin Reference" section to the Build Plan before injection into the Builder prompt

This is pure C++ -- no LLM calls, no tool calls, deterministic, fast.

**Feasibility:** High. `FindFunctionEx()` already does the hard work. `UFunction*` provides full parameter metadata via `TFieldIterator<FProperty>`. The function name extraction from Build Plan text is straightforward regex/parsing.

**Tradeoffs:**
- Pro: Ground truth -- C++ reflection gives exact pin names for any UE function
- Pro: Covers ALL functions, not just template-backed ones
- Pro: Zero additional LLM cost, zero additional tool calls
- Pro: Deterministic -- no LLM compliance variability
- Pro: Fast -- `FindFunctionEx()` is sub-millisecond per function; 20 lookups < 20ms total
- Con: Function name extraction from natural language is imperfect. "call ApplyPointDamage" is easy; "get the forward vector then normalize it" is harder. But the Planner format is structured enough (Build Plan uses specific patterns like "Call X", "Get Y via Z") that extraction is reliable.
- Con: Does not help with functions that `FindFunctionEx` can't resolve (but if it can't resolve them, plan_json can't execute them either, so the failure is deferred anyway)

**Verdict:** This is the recommended approach.

## Recommended Design: Option A + E Combined

Use Option A (prompt change) as the primary mechanism and Option E (C++ enrichment) as the safety net.

**Why both?** Option A makes the Build Plan self-documenting -- the Planner includes pin names from template data it already has. Option E catches the gap -- functions not in any template get their signatures resolved via C++ reflection and appended. Together they cover ~95%+ of functions.

### Change 1: Planner Prompt Update (Option A)

**File:** `OliveAgentPipeline.cpp`, `BuildPlannerSystemPrompt()` at line 2833

Add pin name instruction to the function description rules. Change the existing example and add a new rule.

**Current text (lines 2879-2883):**
```
"- Function descriptions must include node-level detail.\n"
"  Example: \"Get ProjectileMovementComp via GetComponentByClass -> set InitialSpeed, "
"MaxSpeed via property access -> store Instigator ref as variable\"\n"
"  Include: component access patterns, function call targets, branch conditions, "
"variable get/set operations, and any by-ref pins that need wires.\n"
```

**New text:**
```
"- Function descriptions must include node-level detail WITH EXACT PIN NAMES.\n"
"  For each call op, list the input pins you will wire: FunctionName(PinName: Type, ...)\n"
"  Example: \"Call ApplyPointDamage(DamagedActor: Actor, BaseDamage: Float, "
"HitFromDirection: Vector, HitResult: HitResult, DamageTypeClass: Class, "
"DamageCauser: Actor) -> branch on result\"\n"
"  Example: \"Get ProjectileMovementComp via get_var -> set InitialSpeed, MaxSpeed "
"via set_var (these are properties, NOT callable functions)\"\n"
"  Include: exact pin names from template node graphs, component access patterns, "
"call targets, branch conditions, and which names are properties (set_var) vs "
"functions (call).\n"
"  If you read a template function graph, copy the pin names exactly as they appear.\n"
```

This is ~200 chars longer than the current text. Negligible impact on Planner prompt size.

### Change 2: C++ Function Signature Enrichment (Option E)

**New method:** `BuildFunctionPinReference()` on `FOliveAgentPipeline`

**Location:** `OliveAgentPipeline.h` (declaration), `OliveAgentPipeline.cpp` (implementation)

**Called from:** `FormatForPromptInjection()`, after Section 3 (Build Plan) and before Section 3.5 (Component API Map). New Section 3.25.

#### Algorithm

1. **Extract function names from Build Plan text.** Scan for patterns:
   - `Call FunctionName` (case-insensitive "call" followed by PascalCase identifier)
   - `FunctionName(` (identifier followed by opening paren -- covers the new Planner format)
   - `op: "call", target: "FunctionName"` (if plan_json snippets appear)
   - Skip known non-function words: `get_var`, `set_var`, `branch`, `sequence`, `cast`, `event`, `custom_event`, etc. (the plan_json op vocabulary)

2. **Deduplicate and resolve.** For each extracted name:
   - Call `FOliveNodeFactory::Get().FindFunctionEx(FuncName, "", Blueprint)` where Blueprint is the first matching Blueprint from `FOliveArchitectResult::ParentClasses` (resolved via `TryResolveClass`)
   - If the Build Plan includes a target class hint (e.g., "on ProjectileMovementComponent"), use that as the class parameter
   - Cache results -- same function name may appear multiple times across assets

3. **Format signatures.** For each resolved `UFunction*`:
   - Extract parameters via `TFieldIterator<FProperty>(Func)`, skipping return value property
   - Format: `FunctionName(ParamName: Type, ParamName2: Type2, ...) -> ReturnType`
   - Use `FOliveClassAPIHelper::GetPropertyTypeString()` for type names
   - Flag `CPF_OutParm | CPF_ReferenceParm` params as `by-ref` (these are the ones that cause "must have input wired" compile errors)
   - Mark pure functions with `[pure]`

4. **Budget cap.** Maximum 2500 chars for the pin reference section. At ~80 chars per function signature, this covers ~30 functions. If over budget, prioritize functions that appear in the Build Plan's function descriptions (not component API map functions).

5. **Output format:**
```
## Function Pin Reference

These are the exact UE pin names. Use these in plan_json inputs -- do not guess.

- ApplyPointDamage(DamagedActor: Actor, BaseDamage: Float, HitFromDirection: Vector, HitResult: HitResult [by-ref], DamageTypeClass: Class, DamageCauser: Actor)
- AttachToComponent(Parent: SceneComponent, SocketName: Name, LocationRule: EAttachmentRule, RotationRule: EAttachmentRule, ScaleRule: EAttachmentRule, bWeldSimulatedBodies: Bool)
- GetForwardVector() -> Vector [pure]
- SpawnActor(UClass: Class, SpawnTransform: Transform, CollisionHandlingOverride: ESpawnActorCollisionHandlingMethod) -> Actor
```

#### Key Implementation Details

**Function name extraction regex (pseudocode):**
```
// Pattern 1: "Call FunctionName" or "call FunctionName"
FRegexPattern CallPattern(TEXT("\\bcall\\s+([A-Z][A-Za-z0-9_]+)"), ERegexPatternFlags::CaseInsensitive);

// Pattern 2: "FunctionName(" -- PascalCase identifier before paren
FRegexPattern ParenPattern(TEXT("\\b([A-Z][A-Za-z0-9_]+)\\s*\\("));

// Pattern 3: standalone PascalCase after "->", "via", "using"
FRegexPattern ChainPattern(TEXT("(?:->|via|using)\\s+([A-Z][A-Za-z0-9_]+)"));
```

**Exclusion set (plan_json ops and structural keywords):**
```cpp
static const TSet<FString> ExcludeNames = {
    TEXT("Call"), TEXT("Get"), TEXT("Set"), TEXT("Branch"), TEXT("Sequence"),
    TEXT("Cast"), TEXT("Event"), TEXT("Custom"), TEXT("Delay"), TEXT("Print"),
    TEXT("Spawn"), TEXT("Make"), TEXT("Break"), TEXT("Return"), TEXT("Comment"),
    TEXT("Float"), TEXT("Int"), TEXT("Bool"), TEXT("String"), TEXT("Vector"),
    TEXT("Rotator"), TEXT("Transform"), TEXT("Actor"), TEXT("Component"),
    TEXT("Object"), TEXT("Class"), TEXT("Struct"), TEXT("Array"), TEXT("Map"),
    TEXT("Reference"), TEXT("Template"), TEXT("None"), TEXT("True"), TEXT("False"),
    TEXT("Default"), TEXT("Action"), TEXT("Parent"), TEXT("Functions"),
    TEXT("Variables"), TEXT("Components"), TEXT("Events"), TEXT("Interfaces"),
    TEXT("Dispatchers"), TEXT("Interactions"), TEXT("Order"), TEXT("Create"),
    TEXT("Modify"),
};
```

**UFunction parameter iteration:**
```cpp
FString FormatFunctionSignature(UFunction* Func)
{
    FString Sig = Func->GetName();
    Sig += TEXT("(");

    bool bFirst = true;
    FProperty* ReturnProp = nullptr;

    for (TFieldIterator<FProperty> It(Func); It; ++It)
    {
        FProperty* Param = *It;
        if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            ReturnProp = Param;
            continue;
        }
        if (!Param->HasAnyPropertyFlags(CPF_Parm))
        {
            continue;
        }

        if (!bFirst) Sig += TEXT(", ");
        bFirst = false;

        Sig += Param->GetName();
        Sig += TEXT(": ");
        Sig += FOliveClassAPIHelper::GetPropertyTypeString(Param);

        if (Param->HasAllPropertyFlags(CPF_OutParm | CPF_ReferenceParm))
        {
            Sig += TEXT(" [by-ref]");
        }
    }

    Sig += TEXT(")");

    if (ReturnProp)
    {
        Sig += TEXT(" -> ");
        Sig += FOliveClassAPIHelper::GetPropertyTypeString(ReturnProp);
    }

    if (Func->HasAnyFunctionFlags(FUNC_BlueprintPure))
    {
        Sig += TEXT(" [pure]");
    }

    return Sig;
}
```

### Integration Point

**`FormatForPromptInjection()` at line 504 (after Validator Warnings, before Component API Map):**

```cpp
// Section 3.25: Function Pin Reference (C++ resolved)
FString PinRef = BuildFunctionPinReference(Architect.BuildPlan, Architect.ParentClasses);
if (!PinRef.IsEmpty())
{
    Output += TEXT("\n");
    Output += PinRef;
    Output += TEXT("\n");
}
```

### What NOT to Change

- Do not modify plan_json resolver or executor. The pin names in the reference section are for the Builder LLM to read; the resolver already handles alias mapping for common mismatches.
- Do not add `describe_node_type` to the Planner's tool set. It describes K2Node classes, not function signatures.
- Do not create a static list of "commonly hallucinated" functions. The C++ enrichment handles all functions equally.

## File Changes

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp` | Modify `BuildPlannerSystemPrompt()` (Option A prompt text). Add `BuildFunctionPinReference()` method (~120 lines). Add call site in `FormatForPromptInjection()`. |
| `Source/OliveAIEditor/Public/Brain/OliveAgentPipeline.h` | Add `BuildFunctionPinReference()` declaration on `FOliveAgentPipeline`. |

No new files. No new settings. No new dependencies (already includes `OliveNodeFactory.h` and `OliveClassAPIHelper.h`).

## Expected Impact

**Before (run 08g):** 12/22 plan_json success = 55%. 10 failures, of which 7 were pin-name or function-vs-property hallucinations (categories A, B, C, D, F from analysis).

**After:** The 7 pin/property failures would have exact pin data available:
- Category A (SetSpeed/SetInitialSpeed/SetMaxSpeed): Pin reference shows these are NOT functions. The Planner prompt also now explicitly teaches "properties, NOT callable functions" pattern. Builder uses `set_var` on first attempt.
- Category B (~Normal on overlap): Pin reference lists exact output pins: `OtherActor: Actor, OtherComp: PrimitiveComponent, OtherBodyIndex: Int, bFromSweep: Bool, SweepResult: HitResult`.
- Category C (GetForwardVector InRot): Pin reference shows `GetForwardVector() -> Vector [pure]` with zero inputs. Builder knows not to pass a rotator.
- Category D (GetActorTransform): Pin reference resolves via alias to `K2_GetActorTransform() -> Transform [pure]`. Builder knows the exact function name.
- Category F (bOrientRotationToMovement): Pin reference does not list it as a function. If BuildComponentAPIMap lists it as a property, Builder uses `set_var`.

**Projected success rate:** ~80-85% first-attempt (up from 55%). The remaining failures would be structural issues (exec wiring conflicts, category E) which are not pin-name related.

## Edge Cases

1. **Function appears in Build Plan but `FindFunctionEx` can't resolve it.** Skip it silently. If the function can't be resolved at enrichment time, it will also fail at plan_json execution time, and the error recovery system handles that.

2. **Function name is ambiguous** (e.g., "Activate" matches on 5 different classes). Use the Build Plan's asset context -- if the function is described under "### BP_Arrow" with parent class `AActor`, resolve against `AActor`. If still ambiguous, include the first match and note the owning class: `Activate() on UActorComponent`.

3. **Build Plan contains no extractable function names.** Return empty string. Section 3.25 is omitted. No harm done.

4. **Function is a Blueprint-defined function** (exists only in a Blueprint's FunctionGraphs, not in C++). `FindFunctionEx` searches Blueprint `GeneratedClass` + `FunctionGraphs` + `SkeletonGeneratedClass`. If the function is defined in the same Blueprint being created, it won't exist yet. Skip it -- the Builder defined it, so it knows its own function's pins.

5. **`by-ref` marking is wrong or missing.** The `CPF_OutParm | CPF_ReferenceParm` check is the same one used by the Blueprint compiler to determine "must have input wired." If both flags are set, it is genuinely by-ref. If only `CPF_OutParm` is set, it is an output-only parameter (still needs noting but differently). Check: use `CPF_ReferenceParm` alone for by-ref detection, as `CPF_OutParm` without `CPF_ReferenceParm` is a pure output.

## Implementation Order

1. **Prompt change** (Option A) -- modify `BuildPlannerSystemPrompt()`. This is a one-line text edit and can be tested immediately.
2. **Function name extractor** -- write the regex/parsing logic to pull function names from Build Plan text.
3. **Signature formatter** -- `FormatFunctionSignature()` using `UFunction*` + `TFieldIterator<FProperty>`.
4. **`BuildFunctionPinReference()`** -- orchestrator that extracts, resolves, formats, and budgets.
5. **Integration** -- add call site in `FormatForPromptInjection()`.
6. **Test** -- run the bow-and-arrow test to verify plan_json success rate improvement.

## Assignment

**Senior coder.** The prompt change is trivial, but the function name extraction from natural language text requires careful regex work and understanding of the Build Plan format. The `FindFunctionEx` integration and `UFunction` parameter iteration require familiarity with UE reflection. The budget cap logic needs careful ordering. This is ~200 lines of new C++ code in a critical path (every Builder run).
