# Plan: Restriction Cleanup & Reliability Fix

## The Problem

The AI spends 76% of its time thinking, goes granular when it shouldn't, and gets blocked by guards that misfire. Task 1 (create gun) went from 5 min → 8 min. Task 2 (pickup item) burns 4 calls hitting a broken guard. Root cause: too many overlapping restrictions that fight each other.

---

## Phase 1: Fix the Bugs (do first, smallest changes)

### 1A. ConstructionScript guard name mismatch
**File:** `OliveBlueprintToolHandlers.cpp` — `HandleBlueprintAddFunction` guard  
**What:** Change `GraphName == TEXT("ConstructionScript")` → `GraphName.Contains(TEXT("ConstructionScript"))`  
**Why:** Every Blueprint has `UserConstructionScript` with only a FunctionEntry node. The guard sees it as "empty" and blocks ALL `add_function` calls. This caused 4 failures in task 2.

### 1B. K2_SetTimerByFunctionName alias
**File:** `OliveFunctionResolver.cpp` — alias table  
**What:** Add alias `"K2_SetTimerByFunctionName" → "SetTimerByFunctionName"` (the actual name on `UKismetSystemLibrary`)  
**Why:** The Fire function's 16-step plan_json fails at step 15 every time, rolling back all 14 created nodes. Confirm the correct internal name first by checking UE source or adding a log.

---

## Phase 2: Remove the Guards (C++ changes)

### 2A. Remove the empty-function guard entirely
**File:** `OliveBlueprintToolHandlers.cpp` — `HandleBlueprintAddFunction`  
**What:** Delete the entire `GUARD_EMPTY_FUNCTIONS_EXIST` block (~35 lines, from `// Guard: block adding functions` to the closing brace before `// Build write request`)  
**Why:** This guard was meant to stop the AI from batching add_function calls before implementing them. But:
- It has the ConstructionScript bug (1A)
- It blocks legitimate MODIFY workflows where you need to add a function to a BP that was created by template (template functions count as "empty" until plan_json fills them)
- The AI already gets "work one at a time" guidance from the template success response
- When it misfires, the AI goes into a death spiral of retries and granular workarounds
- NeoStack has no equivalent guard and works fine

**Keep:** The `GUARD_FUNCTION_HAS_LOGIC` guard on `remove_function` — that one protects against accidentally deleting work, which is a different and valid concern.

### 2B. Remove plan-first routing enforcement
**File:** `OliveBlueprintToolHandlers.cpp` — the `bPlanRoutingEnabled` block in the tool routing section  
**What:** Delete the `ROUTE_PLAN_REQUIRED` enforcement block (~30 lines). Keep the stats tracking if useful for telemetry, just remove the error return.  
**Why:** The enforcement only kicks in if the AI has NEVER used plan_json (`Stats.PlanCalls == 0`). Once it uses plan_json once, it's free to go granular forever. This means it only blocks the case where the AI goes straight to granular from the start — which almost never happens since the prompts already say to use plan_json. When it does block, the error message just confuses the AI.

---

## Phase 3: Trim the Prompts

The goal: cut CLAUDE.md hardcoded rules from 12 → 6, and reduce AGENTS.md directive rules (MUST/NEVER/REQUIRED/ALWAYS) by ~60%.

### 3A. CLAUDE.md hardcoded rules (in `GenerateClaudeMd`)
**Keep (format rules that prevent broken output):**
1. Use ONLY MCP tools, don't modify plugin source
2. Use schema_version "2.0" for all plan_json
3. After create_from_template, functions are empty stubs — write plan_json for each
4. Once all Blueprints compile 0 errors 0 warnings, task is COMPLETE — stop
5. Call olive.get_recipe before first plan_json for each function
6. All asset paths under /Game/

**Remove (workflow micromanagement):**
- "Always preview before applying plan JSON" — preview is optional by design, this contradicts the AGENTS.md text that says "preview is optional"
- "Do not re-preview an unchanged plan" — self-correction already catches identical plans
- "Multi-asset: complete ONE Blueprint fully before starting next" — let the AI decide
- "Do NOT scaffold all assets first" — removing the contradictory guidance
- "Fall back to add_node only after second failure" — no longer enforced in C++, and the AI should just use whatever works
- "After adding a function, write plan_json before creating next function" — same sequencing micromanagement

### 3B. AGENTS.md / cli_blueprint.txt / blueprint_authoring.txt
**Audit pass — change MUST/NEVER/REQUIRED → "prefer" or "recommended" for workflow rules.**  
Keep MUST/NEVER only for format rules:
- `exec_after` and `exec_outputs` mutually exclusive (breaks plans)
- Data-provider steps before @ref steps (breaks plans)
- Class refs append _C (breaks resolution)
- Function graphs don't need event steps (breaks compilation)

**Change to guidance (not directives):**
- "Prefer plan_json for 3+ nodes" instead of "you MUST use plan_json"
- "Preview is recommended but optional" instead of contradictory rules about when to preview
- "For multi-asset tasks, completing one BP before starting the next tends to work better" instead of "Do NOT scaffold"

### 3C. Recipe files — no changes needed
The 10 recipe files (create.txt, modify.txt, etc.) are on-demand knowledge retrieved by `olive.get_recipe`. They're not in the system prompt, so they don't add thinking overhead. Leave them alone.

### 3D. Self-correction responses — trim verbosity
The error messages from `OliveSelfCorrectionPolicy` are very long (5-8 line MUST/NEVER blocks). The AI reads these and adds them to its deliberation burden. Shorten to 1-2 lines: what failed + what to try instead. Example:
- Before: `"[IDENTICAL PLAN - Seen 2 times] Your plan is identical to a previous submission... You MUST change... Specifically: - If function not found... - If pin connection failed... - If component Target... Do NOT resubmit"`
- After: `"Same plan submitted twice. Change the failing step's approach or use olive.get_recipe for the correct pattern."`

---

## Phase 4: Improve Error Messages (optional, low priority)

### 4A. Log guard rejections
The `add_function` guard rejects with 0.12ms execution and no log entry — only the AI sees the error. Add a `UE_LOG(Warning)` before returning `GUARD_EMPTY_FUNCTIONS_EXIST` so future debugging doesn't require guessing.

### 4B. Empty error string in plan_json failure
When `apply_plan_json` fails, the WritePipeline logs `Error: Execution failed for tool 'blueprint.apply_plan_json' (): ` with an empty error string after the colon. The actual error (e.g. "Function not found") is logged separately by PlanExecutor but not propagated to the WritePipeline error message. This makes debugging harder.

---

## Execution Order

1. **Phase 1A** — ConstructionScript fix (1 line, unblocks task 2 immediately)
2. **Phase 2A** — Remove empty-function guard (delete 35 lines)
3. **Phase 2B** — Remove plan-first enforcement (delete 30 lines)
4. **Phase 3A** — Trim CLAUDE.md rules (edit ~15 lines in GenerateClaudeMd)
5. **Phase 1B** — Timer alias (need to verify correct UE name first)
6. **Phase 3B** — AGENTS.md audit (text changes across 3-4 files)
7. **Phase 3D** — Self-correction message trim (edit OliveSelfCorrectionPolicy.cpp)
8. **Phase 4** — Error message improvements (nice to have)

## Expected Impact

- Task 1 should return to ~5 min (less thinking time from fewer rules)
- Task 2 should stop hitting guard wall (0 blocked add_function calls)
- K2_SetTimerByFunctionName plans stop failing
- AI can choose its own workflow order — if it batches and everything compiles, that's fine
- Compile validation remains the real quality gate (unchanged)
