# Gun Fix: Implementation Tasks

## Pre-Read Summary

This document provides task-by-task instructions for fixing three issues discovered during a gun+bullet creation test, plus two supplementary improvements. Tasks are ordered by impact: root cause first, then safety nets, then cosmetics, then supplements.

---

## Current State of Relevant Files

### OliveSelfCorrectionPolicy.cpp (311 lines)

`Evaluate()` (line 13) calls `HasCompileFailure()` first (line 24), then `HasToolFailure()` (line 54). Both parse the ResultJson from `FOliveToolResult::ToJsonString()`.

**Bug found during design review:** `HasCompileFailure()` at line 102 looks for `compile_result` at the TOP LEVEL of the JSON object. But `FOliveToolResult::ToJson()` (OliveToolRegistry.cpp line 203) nests the pipeline's ResultData inside a `"data"` field. The actual JSON structure is:

```json
{
  "success": true,
  "data": {
    "compile_result": { "success": false, "errors": [...] },
    "compile_status": "error",
    "asset_path": "/Game/..."
  }
}
```

So `HasCompileFailure()` never finds compile failures for ANY tool result, not just plan results. It looks at the wrong nesting level. Similarly, `asset_path` extraction (line 133) looks at the top level but the path is inside `data`. This means compile error self-correction has been BROKEN for every tool, not just `apply_plan_json`.

### OliveBlueprintPlanResolver.cpp (989 lines)

`ResolveGetVarOp()` at line 551 checks if the variable exists on the Blueprint via `BlueprintHasVariable()` (line 576). If not found, it warns but still resolves successfully (returns true, line 613). It never checks whether the target name matches a component in the SCS.

### OlivePlanExecutor.cpp (1625 lines)

`PhaseCreateNodes()` at line 169 iterates resolved steps, creating nodes one by one. On failure at line 273 (`!WriteResult.bSuccess`), it adds a wiring error and returns false (line 292). But between lines 265 and 292, any nodes already created (stored in `Context.StepToNodeMap` and `Context.StepToNodePtr`) are NOT cleaned up. The `Execute()` method at line 91 checks `!bNodesCreated` and calls `AssembleResult()` directly -- no cleanup of partial nodes.

### Content/SystemPrompts/Knowledge/recipes/blueprint/ (9 .txt files)

Recipe files with TAGS headers. Manifest at `_manifest.json` has 9 entries. No component reference recipe exists.

---

## Task 1: Fix HasCompileFailure JSON Nesting Bug (CRITICAL)

**Priority:** Highest -- this is the root cause of Fix 3 AND it affects ALL tools, not just plan JSON.

**Objective:** Make `HasCompileFailure()` look for `compile_result` inside the `data` object, not at the top level. Also fix `asset_path` extraction.

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

**Changes:**

Replace lines 92-141 (the entire `HasCompileFailure` method body) with logic that:

1. Deserializes the JSON (same as now)
2. First tries `JsonObj->TryGetObjectField(TEXT("compile_result"), CompileResult)` (top-level, for backward compat)
3. If not found, tries `JsonObj->TryGetObjectField(TEXT("data"), DataObj)` then `(*DataObj)->TryGetObjectField(TEXT("compile_result"), CompileResult)`
4. Proceeds with the existing success/error extraction logic
5. For `asset_path` extraction: also look inside `data` if not found at top level

**Exact old code to replace** (lines 100-137):

```cpp
	const TSharedPtr<FJsonObject>* CompileResult;
	if (!JsonObj->TryGetObjectField(TEXT("compile_result"), CompileResult))
	{
		return false;
	}

	bool bSuccess = true;
	(*CompileResult)->TryGetBoolField(TEXT("success"), bSuccess);
	if (bSuccess)
	{
		return false;
	}

	// Extract errors
	OutErrors.Empty();
	const TArray<TSharedPtr<FJsonValue>>* ErrorsArray;
	if ((*CompileResult)->TryGetArrayField(TEXT("errors"), ErrorsArray))
	{
		for (const auto& ErrVal : *ErrorsArray)
		{
			if (ErrVal->Type == EJson::String)
			{
				OutErrors += ErrVal->AsString() + TEXT("\n");
			}
			else if (ErrVal->AsObject().IsValid())
			{
				OutErrors += ErrVal->AsObject()->GetStringField(TEXT("message")) + TEXT("\n");
			}
		}
	}

	// Extract asset path
	JsonObj->TryGetStringField(TEXT("asset_path"), OutAssetPath);
	if (OutAssetPath.IsEmpty())
	{
		JsonObj->TryGetStringField(TEXT("blueprint_path"), OutAssetPath);
	}
```

**New code:**

```cpp
	// Look for compile_result -- it may be at top level OR nested inside "data"
	// (FOliveToolResult::ToJson nests pipeline ResultData inside "data")
	const TSharedPtr<FJsonObject>* CompileResult = nullptr;
	TSharedPtr<FJsonObject> DataObj;

	if (JsonObj->TryGetObjectField(TEXT("compile_result"), CompileResult))
	{
		// Found at top level (direct compile tool result or legacy format)
	}
	else
	{
		// Try inside "data" (standard write pipeline result format)
		const TSharedPtr<FJsonObject>* DataPtr = nullptr;
		if (JsonObj->TryGetObjectField(TEXT("data"), DataPtr) && DataPtr && (*DataPtr).IsValid())
		{
			DataObj = *DataPtr;
			if (!DataObj->TryGetObjectField(TEXT("compile_result"), CompileResult))
			{
				return false;
			}
		}
		else
		{
			return false;
		}
	}

	bool bSuccess = true;
	(*CompileResult)->TryGetBoolField(TEXT("success"), bSuccess);
	if (bSuccess)
	{
		return false;
	}

	// Extract errors
	OutErrors.Empty();
	const TArray<TSharedPtr<FJsonValue>>* ErrorsArray;
	if ((*CompileResult)->TryGetArrayField(TEXT("errors"), ErrorsArray))
	{
		for (const auto& ErrVal : *ErrorsArray)
		{
			if (ErrVal->Type == EJson::String)
			{
				OutErrors += ErrVal->AsString() + TEXT("\n");
			}
			else if (ErrVal->AsObject().IsValid())
			{
				OutErrors += ErrVal->AsObject()->GetStringField(TEXT("message")) + TEXT("\n");
			}
		}
	}

	// Extract asset path -- check top level first, then inside "data"
	JsonObj->TryGetStringField(TEXT("asset_path"), OutAssetPath);
	if (OutAssetPath.IsEmpty())
	{
		JsonObj->TryGetStringField(TEXT("blueprint_path"), OutAssetPath);
	}
	if (OutAssetPath.IsEmpty() && DataObj.IsValid())
	{
		DataObj->TryGetStringField(TEXT("asset_path"), OutAssetPath);
		if (OutAssetPath.IsEmpty())
		{
			DataObj->TryGetStringField(TEXT("blueprint_path"), OutAssetPath);
		}
	}
```

**Also in `BuildCompileErrorMessage`:** Enhance the message to include actionable guidance. Replace lines 181-186:

```cpp
	return FString::Printf(
		TEXT("[COMPILE FAILED - Attempt %d/%d] The Blueprint failed to compile after executing '%s'. Errors:\n%s\n"
			 "Please analyze the errors and fix the issue. You may re-call the write tool with corrections. "
			 "Focus on the FIRST error — later errors are often caused by the first one."),
		AttemptNum, MaxAttempts, *ToolName, *Errors);
```

With:

```cpp
	return FString::Printf(
		TEXT("[COMPILE FAILED - Attempt %d/%d] The Blueprint failed to compile after executing '%s'. Errors:\n%s\n"
			 "REQUIRED ACTION: Do NOT declare success. Fix the compile error before finishing.\n"
			 "1. Call blueprint.read on the affected graph with include_pins:true to see the current node/pin state.\n"
			 "2. Focus on the FIRST error — later errors are often caused by the first one.\n"
			 "3. Use connect_pins or set_pin_default to fix the issue, then compile again."),
		AttemptNum, MaxAttempts, *ToolName, *Errors);
```

**Constraints:**
- Do NOT change the method signature
- Do NOT change `HasToolFailure()` -- it correctly checks the top-level `success` field
- The `compile_result` is set by `StageReport` in the write pipeline (OliveWritePipeline.cpp line 582), which puts it directly on `FinalResult.ResultData`. When `ToToolResult()` converts to `FOliveToolResult`, this becomes `Data`, which `ToJson()` nests under `"data"`.
- Keep backward compatibility: try top-level first for any tools that might return compile_result directly

**Acceptance Criteria:**
- When `apply_plan_json` returns `{ "success": true, "data": { "compile_result": { "success": false, "errors": [...] } } }`, `HasCompileFailure()` returns true
- When any write tool returns a successful result with compile errors, self-correction triggers
- When a tool returns a failed result (success=false), the existing `HasToolFailure()` path still works unchanged
- Build succeeds

---

## Task 2: Component Guard in Plan Resolver (ROOT CAUSE)

**Priority:** High -- this is why the AI can't reference components.

**Objective:** When `op:"get_var"` targets a name that matches a component in the Blueprint's SCS but NOT a variable, reject with an actionable error message that tells the AI exactly what to do.

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

**Additional includes needed at top of file:**

```cpp
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
```

**Changes to `ResolveGetVarOp` (starting at line 551):**

After the variable-not-found check (the `if (!bFoundInParent)` block at line 591), add a component name check. Insert the following INSIDE the `if (!bFoundInParent)` block, BEFORE the existing UE_LOG statements at lines 595-601.

The logic is:
1. If the variable was not found on the Blueprint or parents
2. Check the Blueprint's SCS (SimpleConstructionScript) for a component with that name
3. If found, return false with an error that includes the EXACT correct pattern

**Exact insertion point:** Inside `ResolveGetVarOp`, replace the block from line 591 to line 601:

OLD (lines 591-601):
```cpp
		if (!bFoundInParent)
		{
			// Not an error — variable might be created by an earlier step in the plan,
			// or might be a native property not in NewVariables
			UE_LOG(LogOlivePlanResolver, Verbose,
				TEXT("Step '%s': Variable '%s' not found on Blueprint '%s' (may be inherited or created by another step)"),
				*Step.StepId, *Step.Target, *BP->GetName());

			UE_LOG(LogOlivePlanResolver, Warning,
				TEXT("    Variable '%s' not found on Blueprint '%s' or parents"),
				*Step.Target, *BP->GetName());
		}
```

NEW:
```cpp
		if (!bFoundInParent)
		{
			// Check if the name matches a component in the SCS
			// This catches a common AI mistake: using get_var for a component name
			if (BP->SimpleConstructionScript)
			{
				FString MatchedComponentClass;
				TArray<USCS_Node*> NodesToSearch;
				for (USCS_Node* RootNode : BP->SimpleConstructionScript->GetRootNodes())
				{
					NodesToSearch.Add(RootNode);
				}
				while (NodesToSearch.Num() > 0)
				{
					USCS_Node* Current = NodesToSearch.Pop();
					if (!Current) continue;
					if (Current->GetVariableName().ToString() == Step.Target)
					{
						// Found a component with this name
						if (Current->ComponentClass)
						{
							FString ClassName = Current->ComponentClass->GetName();
							// Strip the U prefix for display (UArrowComponent -> ArrowComponent)
							if (ClassName.StartsWith(TEXT("U")))
							{
								ClassName = ClassName.Mid(1);
							}
							MatchedComponentClass = ClassName;
						}
						break;
					}
					for (USCS_Node* Child : Current->GetChildNodes())
					{
						NodesToSearch.Add(Child);
					}
				}

				if (!MatchedComponentClass.IsEmpty())
				{
					// This is a component, not a variable -- reject with actionable guidance
					Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
						TEXT("COMPONENT_NOT_VARIABLE"),
						Step.StepId,
						FString::Printf(TEXT("/steps/%d/target"), Idx),
						FString::Printf(
							TEXT("'%s' is a component (class: %s), not a variable. "
								 "Use get_var only for Blueprint variables. "
								 "To access a component, use: "
								 "{\"op\":\"call\", \"target\":\"GetComponentByClass\", "
								 "\"inputs\":{\"ComponentClass\":\"%s\"}}"),
							*Step.Target, *MatchedComponentClass, *MatchedComponentClass),
						FString::Printf(
							TEXT("Replace this get_var step with a call to GetComponentByClass "
								 "with ComponentClass:\"%s\""),
							*MatchedComponentClass)));

					UE_LOG(LogOlivePlanResolver, Warning,
						TEXT("Step '%s': '%s' is a component (%s), not a variable — rejected with guidance"),
						*Step.StepId, *Step.Target, *MatchedComponentClass);

					return false;
				}
			}

			// Not a component either — warn but allow (may be created by another step or inherited from native)
			UE_LOG(LogOlivePlanResolver, Verbose,
				TEXT("Step '%s': Variable '%s' not found on Blueprint '%s' (may be inherited or created by another step)"),
				*Step.StepId, *Step.Target, *BP->GetName());

			UE_LOG(LogOlivePlanResolver, Warning,
				TEXT("    Variable '%s' not found on Blueprint '%s' or parents"),
				*Step.Target, *BP->GetName());
		}
```

**Also add the same guard to `ResolveSetVarOp`:** The same pattern exists in `ResolveSetVarOp` (lines 645-673). Add the identical component check inside the `if (!bFoundInParent)` block there (lines 659-666). Copy the exact same SCS traversal + error logic.

**Constraints:**
- The SCS traversal pattern matches `FOliveComponentWriter::FindSCSNode()` (OliveComponentWriter.cpp line 709). Use the same approach: `GetRootNodes()` + pop-based iteration of `GetChildNodes()`.
- `USCS_Node::GetVariableName()` returns `FName`. Compare with `.ToString()`.
- `USCS_Node::ComponentClass` is a `TObjectPtr<UClass>`.
- This check ONLY runs when the variable is NOT found on the Blueprint. If a variable AND a component share the same name (unlikely but possible), the variable takes precedence.
- New error code: `COMPONENT_NOT_VARIABLE`

**Acceptance Criteria:**
- `op:"get_var" target:"MuzzlePoint"` on a Blueprint with a MuzzlePoint ArrowComponent returns error with `COMPONENT_NOT_VARIABLE` and the message includes `GetComponentByClass` with `ArrowComponent`
- `op:"get_var" target:"bCanFire"` on a Blueprint with a bCanFire boolean variable still resolves successfully
- `op:"set_var" target:"MuzzlePoint"` on the same Blueprint also returns the component error
- Build succeeds

---

## Task 3: Orphan Node Cleanup on Phase 1 Failure

**Priority:** Medium -- cosmetic but confusing to users.

**Objective:** When Phase 1 (node creation) fails partway through, remove all already-created nodes from the graph before returning the error result.

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

**Changes to `PhaseCreateNodes` (at line 169):**

After the `return false;` at line 292 (inside the `!WriteResult.bSuccess` block), and also after the `return false;` at line 313 (the "not in cache" block), the node cleanup needs to happen. But since there are two `return false;` paths, the cleanest approach is to add a cleanup block BEFORE each return, or better: restructure so the cleanup happens in one place.

**Recommended approach:** Add a labeled cleanup block right before each `return false;`. Insert the following cleanup code at TWO locations:

**Location 1:** Right before `return false;` at line 292 (after the wiring error is added):

Insert between lines 290-292:

```cpp
			// Clean up already-created nodes to prevent orphans
			for (const auto& Pair : Context.StepToNodePtr)
			{
				if (Pair.Value && Context.Graph)
				{
					Context.Graph->RemoveNode(Pair.Value);
				}
			}
			Context.StepToNodeMap.Empty();
			Context.StepToNodePtr.Empty();
			Context.StepManifests.Empty();
			Context.CreatedNodeCount = 0;

			return false;
```

**Location 2:** Right before `return false;` at line 313 (after the "not in cache" wiring error):

Insert the same cleanup block between lines 311-313.

**Alternative (DRY approach):** Factor the cleanup into a local lambda or helper. A lambda is cleaner since it only applies within this method:

After the `PlanStepLookup` loop (after line 181), add:

```cpp
	// Cleanup lambda: removes all nodes created so far if Phase 1 aborts mid-way.
	// This prevents orphan nodes from surviving in the graph.
	auto CleanupCreatedNodes = [&Context]()
	{
		UE_LOG(LogOlivePlanExecutor, Log,
			TEXT("Phase 1 cleanup: removing %d orphan nodes from graph"),
			Context.StepToNodePtr.Num());

		for (const auto& Pair : Context.StepToNodePtr)
		{
			if (Pair.Value && Context.Graph)
			{
				Context.Graph->RemoveNode(Pair.Value);
			}
		}
		Context.StepToNodeMap.Empty();
		Context.StepToNodePtr.Empty();
		Context.StepManifests.Empty();
		Context.CreatedNodeCount = 0;
	};
```

Then at both `return false;` locations (lines 292 and 313), insert `CleanupCreatedNodes();` before the return.

**Important: Do NOT cleanup reused event nodes.** If a node was reused (from the event reuse path at line 226), it should NOT be removed -- it existed before the plan started. To handle this, add a `TSet<FString> ReusedStepIds;` to track which steps reused existing nodes, and skip them during cleanup:

```cpp
	TSet<FString> ReusedStepIds;
```

Set it inside the event reuse `continue` block (after line 253):
```cpp
					ReusedStepIds.Add(StepId);
					continue;
```

And modify the cleanup lambda:
```cpp
	auto CleanupCreatedNodes = [&Context, &ReusedStepIds]()
	{
		UE_LOG(LogOlivePlanExecutor, Log,
			TEXT("Phase 1 cleanup: removing %d orphan nodes from graph (skipping %d reused)"),
			Context.StepToNodePtr.Num() - ReusedStepIds.Num(),
			ReusedStepIds.Num());

		for (const auto& Pair : Context.StepToNodePtr)
		{
			if (ReusedStepIds.Contains(Pair.Key))
			{
				continue; // Do not remove pre-existing event nodes
			}
			if (Pair.Value && Context.Graph)
			{
				Context.Graph->RemoveNode(Pair.Value);
			}
		}
		Context.StepToNodeMap.Empty();
		Context.StepToNodePtr.Empty();
		Context.StepManifests.Empty();
		Context.CreatedNodeCount = 0;
	};
```

**Constraints:**
- `UEdGraph::RemoveNode(UEdGraphNode*)` is the correct API. It breaks all pin connections and removes the node from the graph's Nodes array.
- This runs inside the write pipeline's FScopedTransaction. The node removals will be part of the same transaction, so if the entire plan is rolled back (which it will be since we return an error), the removals are also rolled back. This is fine -- the important thing is that the NEXT attempt starts with a clean graph.
- Actually wait -- the write pipeline WILL roll back the entire transaction on error (pipeline line 194 checks `!ExecuteResult.bSuccess`). So the orphan cleanup is actually redundant IF the transaction rollback works perfectly. The issue described in the bug report suggests that UE's undo system captured the node creations individually and the rollback did not fully clean them up. The explicit `RemoveNode()` calls provide defense-in-depth.
- The cleanup must happen BEFORE `AssembleResult()` is called, because `AssembleResult` reads from `Context.StepToNodeMap` to build the result. After cleanup, the maps are empty, and the result will correctly show 0 nodes created.

**Acceptance Criteria:**
- When Phase 1 fails at step 5 of 11, only 0 nodes remain in the graph (not 4)
- Reused event nodes are NOT removed during cleanup
- On retry, the new plan starts fresh without orphans
- Build succeeds

---

## Task 4: Component Reference Recipe Entry

**Priority:** Low -- supplementary to Task 2.

**Objective:** Add a recipe file that teaches the AI how to access components in plan JSON.

**File 1 (new):** `Content/SystemPrompts/Knowledge/recipes/blueprint/component_reference.txt`

**Content:**
```
TAGS: component reference target getcomponentbyclass scene muzzle arrow transform access
---
To access a component's properties or transform in plan JSON, use GetComponentByClass:
  {"step_id":"get_comp", "op":"call", "target":"GetComponentByClass",
   "inputs":{"ComponentClass":"ArrowComponent"}}
  {"step_id":"get_tf", "op":"call", "target":"GetWorldTransform",
   "inputs":{"Target":"@get_comp.auto"}}
Do NOT use get_var for components. Components are NOT variables.
Do NOT invent functions like "GetMuzzlePoint" -- use GetComponentByClass.
Common component classes: StaticMeshComponent, ArrowComponent, BoxComponent, SphereComponent, CapsuleComponent, AudioComponent, ParticleSystemComponent, SkeletalMeshComponent.
```

**File 2 (modify):** `Content/SystemPrompts/Knowledge/recipes/_manifest.json`

Add a new entry inside `"categories" > "blueprint" > "recipes"`:

```json
        "component_reference": {
          "description": "How to access component properties and transforms using GetComponentByClass",
          "tags": ["component", "reference", "getcomponentbyclass", "scene", "muzzle", "arrow", "transform"],
          "max_tokens": 200
        }
```

Insert this after the `"spawn_actor"` entry and before `"function_graph"`.

**Constraints:**
- File encoding: UTF-8, LF line endings
- TAGS line must come first, then `---` separator, then content
- Keep under the `max_tokens` estimate

**Acceptance Criteria:**
- `olive.get_recipe` with query "component reference" returns this recipe
- The recipe content matches the plan JSON syntax the AI needs

---

## Task 5: Enhanced Self-Correction Guidance for PLAN_RESOLVE_FAILED

**Priority:** Low -- supplementary.

**Objective:** When `PLAN_RESOLVE_FAILED` fires (which is the error code returned when resolver rejects a step), enhance the guidance to mention `olive.get_recipe` as an option.

**File:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

**Changes:** Replace the guidance for the `PLAN_RESOLVE_FAILED` / `PLAN_LOWER_FAILED` / `PLAN_EXECUTION_FAILED` block at lines 269-272.

OLD (lines 269-272):
```cpp
	else if (ErrorCode == TEXT("PLAN_RESOLVE_FAILED") || ErrorCode == TEXT("PLAN_LOWER_FAILED") || ErrorCode == TEXT("PLAN_EXECUTION_FAILED"))
	{
		Guidance = TEXT("The plan failed during resolution or execution. Check the error details for which step failed, fix that step, and resubmit the corrected plan.");
	}
```

NEW:
```cpp
	else if (ErrorCode == TEXT("PLAN_RESOLVE_FAILED") || ErrorCode == TEXT("PLAN_LOWER_FAILED") || ErrorCode == TEXT("PLAN_EXECUTION_FAILED"))
	{
		Guidance = TEXT("The plan failed during resolution or execution. Check the error details for which step failed. "
			"If a step used the wrong pattern (e.g., get_var for a component, or an invented function name), "
			"call olive.get_recipe with a query describing what you need (e.g., 'component reference' or 'spawn actor') "
			"to get the correct pattern. Fix the failing step and resubmit the corrected plan.");
	}
```

**Also add a new error code case for `COMPONENT_NOT_VARIABLE`:** Insert a new `else if` block before the generic fallback (before line 304):

```cpp
	else if (ErrorCode == TEXT("COMPONENT_NOT_VARIABLE"))
	{
		Guidance = TEXT("You tried to use get_var on a component name. Components are NOT variables. "
			"The error message contains the exact correct pattern using GetComponentByClass. "
			"Replace the get_var step with the call pattern shown in the error message.");
	}
```

**Constraints:**
- Must come before the generic `else` block at line 304
- Error code `COMPONENT_NOT_VARIABLE` matches what Task 2 emits

**Acceptance Criteria:**
- `PLAN_RESOLVE_FAILED` guidance now mentions `olive.get_recipe`
- `COMPONENT_NOT_VARIABLE` has its own specific guidance
- Build succeeds

---

## Execution Order

| Order | Task | Files Modified | Risk |
|-------|------|---------------|------|
| 1 | Task 1: Fix HasCompileFailure nesting | OliveSelfCorrectionPolicy.cpp | Low -- isolated method, no interface changes |
| 2 | Task 2: Component guard in resolver | OliveBlueprintPlanResolver.cpp | Low -- adds early return on new condition, existing paths untouched |
| 3 | Task 3: Orphan node cleanup | OlivePlanExecutor.cpp | Medium -- touches node lifecycle in Phase 1 |
| 4 | Task 5: Self-correction guidance | OliveSelfCorrectionPolicy.cpp | Low -- string changes only |
| 5 | Task 4: Recipe file | New file + manifest edit | None -- content only |

**Build after each task.** Tasks 1-3 are independent and could theoretically be done in parallel, but serial execution is recommended because Task 1 changes the same file as Task 5.

---

## Risks and Edge Cases

### Task 1 (HasCompileFailure)
- **Risk:** Some tools might put `compile_result` at the top level (e.g., a direct compile tool). The code tries top-level first, so this is backward compatible.
- **Edge case:** `data` field exists but is not an object (unlikely but defensive). The `TryGetObjectField` call handles this -- it returns false for non-objects.
- **Edge case:** Tool result has `compile_result` with no `errors` array (just `success: false`). The existing `OutErrors` will be empty, but `HasCompileFailure` still returns true. The `BuildCompileErrorMessage` will show empty errors. This is acceptable -- the AI still gets the correction directive.

### Task 2 (Component Guard)
- **Risk:** A variable and component share the same name. The code only checks SCS when the variable is NOT found, so variables always win. This is correct behavior.
- **Risk:** Parent Blueprint has a component the child doesn't know about. `BP->SimpleConstructionScript` only contains THIS Blueprint's components, not inherited ones. This is fine -- inherited components would need a different access pattern anyway.
- **Edge case:** Blueprint has no SCS (e.g., data-only Blueprints). The null check `if (BP->SimpleConstructionScript)` handles this.
- **Edge case:** Component class is null (corrupted SCS node). The `if (Current->ComponentClass)` check handles this; `MatchedComponentClass` stays empty and the guard does not fire.

### Task 3 (Orphan Cleanup)
- **Risk:** `RemoveNode()` inside a transaction scope. UE's undo system will capture these removals. When the pipeline rolls back the transaction (because the executor returned an error), the removals are also rolled back -- meaning the nodes come back. This seems like a no-op. However, the bug report says nodes survived a transaction rollback, which suggests the transaction boundary may not be working as expected for node creation. The explicit cleanup provides defense-in-depth: if the transaction DOES roll back correctly, the `RemoveNode` calls are undone (harmless). If the transaction does NOT roll back correctly, the explicit cleanup catches the orphans.
- **Alternative consideration:** The real fix might be ensuring the transaction rollback works. But that is a deeper investigation into UE's undo system. The explicit cleanup is the pragmatic fix.
- **Edge case:** `Graph->RemoveNode()` called on a node that is already connected to other nodes created in this same batch. `RemoveNode` breaks all connections first, which is fine since we are cleaning up the entire batch.
- **Edge case:** The reused event node must NOT be removed. The `ReusedStepIds` set handles this.

### Task 4 (Recipe)
- No risks. Pure content addition.

### Task 5 (Guidance Enhancement)
- **Risk:** Longer guidance string increases response size slightly. Negligible.
- The `COMPONENT_NOT_VARIABLE` error code must match exactly between Task 2 and Task 5.

---

## New Error Codes Summary

| Code | Where Emitted | Meaning |
|------|--------------|---------|
| `COMPONENT_NOT_VARIABLE` | OliveBlueprintPlanResolver.cpp | get_var/set_var target matches a component name, not a variable |

---

## Verification Checklist

After all tasks are complete:

1. **Build** the plugin and confirm zero errors
2. **Smoke test the self-correction flow:**
   - Create a Blueprint with a component
   - Submit a plan with `compile_result: { success: false }` in the tool result JSON
   - Verify `HasCompileFailure` returns true (add temporary UE_LOG if needed)
3. **Smoke test the component guard:**
   - Submit a plan with `op:"get_var" target:"MuzzlePoint"` where MuzzlePoint is an ArrowComponent
   - Verify the error code is `COMPONENT_NOT_VARIABLE` and the message includes `GetComponentByClass`
4. **Smoke test orphan cleanup:**
   - Submit a plan where step 5 of 11 will fail (e.g., invented function name)
   - Verify that after the failure, the graph has no orphan nodes from the partial execution
