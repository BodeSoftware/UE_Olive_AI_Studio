# MCP Blueprint Tool Test Results

**Date:** 2026-04-03
**Scope:** 24 tests across 10 groups covering plan_json pipeline, exec flow, ops, validators, structure tools, widgets, templates, and edge cases.

## Summary

- **23/24 tests PASS**
- **1 test FAIL** (Sequence 3+ outputs — fixed)
- **4 issues found**, 2 fixed in this session

## Issues Found & Status

### ISSUE #1: Error message parenthetical in ambiguous component case (FIXED)
- **File:** `OlivePlanValidator.cpp:279-294`
- **Severity:** Cosmetic
- **Problem:** When >1 matching components exist, parenthetical said "has no matching component" — contradicts "2 components found" in the suggestion.
- **Fix:** Made parenthetical dynamic: 0 matches → "no matching component found", >1 matches → "N matching components, ambiguous", non-Actor → "not an Actor — has no components".

### ISSUE #2: Sequence node with 3+ exec_outputs fails (FIXED)
- **File:** `OliveBlueprintPlanResolver.cpp:1814-1824`
- **Severity:** Functional
- **Problem:** UE Sequence node starts with 2 pins. Resolver didn't pass `num_outputs` to the factory when `exec_outputs` specified 3+. Phase 3 wiring couldn't find `then_2`.
- **Fix:** Resolver now sets `num_outputs` property from `Step.ExecOutputs.Num()` when > 2. The factory's `CreateSequenceNode` already supports this via `AddInputPin()` loop.

### ISSUE #3: Interface/FunctionLibrary require explicit parent_class (NOT FIXED)
- **File:** `OliveBlueprintToolHandlers.cpp` (blueprint.create handler)
- **Severity:** UX
- **Problem:** `WidgetBlueprint` auto-defaults parent to `UserWidget`, but `Interface` and `FunctionLibrary` types don't auto-default despite having obvious parents (`Interface`, `BlueprintFunctionLibrary`).
- **Suggested Fix:** Add auto-defaults in the create handler validation.

### ISSUE #4: Preview doesn't validate @ref step existence (NOT FIXED)
- **File:** `OlivePlanValidator.cpp` (Phase 0)
- **Severity:** Minor UX
- **Problem:** `@nonexistent_step.auto` passes preview validation. Only caught at apply time (Phase 4 data wiring) with rollback.
- **Suggested Fix:** Add Phase 0 check that validates all `@ref` step IDs in inputs exist in the plan's step list.

## Detailed Test Results

### Group 1: Plan JSON Core Pipeline
| # | Test | Result |
|---|------|--------|
| 1.1 | event + print_string | PASS |
| 1.2 | `self` input key on component fn | PASS |
| 1.3 | `Target` input key (original path) | PASS |
| 1.4 | Component fn no target (auto-wire) | PASS (ambiguous correctly caught) |
| 1.5 | Ambiguous component targets | PASS (2 matches → error) |

### Group 2: Exec Flow & Wiring
| # | Test | Result |
|---|------|--------|
| 2.1 | Linear 4-step exec chain | PASS |
| 2.2 | Branch with exec_outputs | PASS |
| 2.3 | Sequence 3 outputs | FAIL → FIXED |
| 2.3b | Sequence 2 outputs | PASS |
| 2.5 | Delay in EventGraph | PASS |

### Group 3: Plan JSON Op Coverage
| # | Test | Result |
|---|------|--------|
| 3.1 | spawn_actor + auto MakeTransform | PASS |
| 3.2 | return op in function graph | PASS |
| 3.3 | call_delegate on dispatcher | PASS |

### Group 4: Validator Error Paths
| # | Test | Result |
|---|------|--------|
| 4.1 | LATENT_IN_FUNCTION | PASS (clear error) |
| 4.2 | VARIABLE_NOT_FOUND | PASS (lists alternatives) |
| 4.3 | EXEC_SOURCE_IS_RETURN | PASS (clear error) |
| 4.4 | COMPONENT_FUNCTION_ON_ACTOR (UObject) | PASS (updated message) |

### Groups 5-10: Structure, Widget, Templates, Edge Cases
| # | Test | Result |
|---|------|--------|
| 5.1 | Create BP types (Normal, Widget) | PASS |
| 5.1b | Create Interface, FuncLib | PASS (with explicit parent) |
| 8.1 | Widget hierarchy (Canvas → Text + ProgressBar) | PASS |
| 8.2 | widget.set_property | PASS |
| 8.3 | Plan JSON on WidgetBlueprint | PASS |
| 9.1 | list_templates (factory) | PASS |
| 9.2 | create_from_template (stat_component Health) | PASS |
| 10.1 | Empty plan | PASS (PLAN_EMPTY_STEPS) |
| 10.2 | @ref to nonexistent step | PASS (caught at apply) |
| 10.3 | Stale event chain cleanup | PASS |

## Files Modified

1. `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp`
   - Added `self` input key check (lines 177-181)
   - Added `GameFramework/Actor.h` include
   - Fixed error message for non-Actor BPs
   - Fixed ambiguous component parenthetical (ISSUE #1)

2. `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
   - Added `num_outputs` property for Sequence nodes with 3+ exec_outputs (ISSUE #2)
