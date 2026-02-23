# Blueprint Create Failure Analysis (2026-02-22)

## Source Evidence

From `docs/logs/UE_Olive_AI_Toolkit.log`:
- Repeated failures at execution stage, not validation stage:
  - `LogOliveWritePipeline: Starting write pipeline for tool 'blueprint.create'`
  - `LogOliveWritePipeline: Error: Execution failed for tool 'blueprint.create'`
  - `LogOliveAI: Tool 'blueprint.create' executed ... - failed`

This pattern indicates:
- `Validate` and `Transact` passed,
- the failure occurred inside `FOliveBlueprintWriter::CreateBlueprint(...)`.

## Is This Covered By Section 17?

No, this is mostly **not** a Section 17 completion issue.

Section 17 phases 1-3 focus on:
- safety metadata (`redirected_from`, `compile_status`, traversal/hot-reload/GC guards),
- graph integrity checks,
- chat UX resilience/retry/truncation behavior.

The observed `blueprint.create` failure is in core Blueprint creation logic (writer/class resolution/path handling), which is outside Section 17's primary scope.

## Likely Root Cause

Most likely failure mode was parent class resolution fragility:
- `FindParentClass` previously relied on narrow lookup patterns.
- Requests using short names, Blueprint class names, or class-object variants could fail unexpectedly.

Possible secondary failure mode:
- invalid/non-long-package asset path format passed into create flow.

## Fixes Applied

1. **Parent class resolution hardened** in `FOliveBlueprintWriter::FindParentClass`:
- Native-first lookup.
- Prefix variants (`A`, `U`) and `_C` candidate.
- `/Script/...` static class load fallbacks.
- Blueprint asset path + generated class fallback.

2. **Path validity check added** before create:
- explicit `FPackageName::IsValidLongPackageName` check with actionable error.

3. **Diagnostics improved**:
- explicit logs for unresolved parent class and create failure context in writer.
- explicit `blueprint.create` failure log in tool handler (path, parent, type, error).
- pipeline execution failure logging now includes `error_code` and `error_message`.

## Expected Outcome

These fixes should resolve common `blueprint.create` failures caused by:
- parent class name/path format mismatch,
- malformed asset path input.

If failures continue after these changes, new logs should now include enough detail to isolate remaining causes quickly.
