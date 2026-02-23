# Phase C: Integration Plan

## Overview
Wire Phase A+B code into the existing codebase so `schema_version: "2.0"` plans use the new pin-introspection pipeline. v1.0 plans continue through the existing lowerer+batch path unchanged.

## Tasks (in execution order)

### C1: Wire FOliveFunctionResolver into ResolveCallOp
**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**What:** Replace the direct catalog search in `ResolveCallOp` with `FOliveFunctionResolver::Resolve()`. One include added, one method body replaced. Preserves fallback behavior for unresolved functions.
**Depends on:** nothing

### C3: Add FOliveBatchExecutionScope to OlivePlanExecutor
**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`
**What:** Wrap phases 3-5 (wiring + defaults) in `{ FOliveBatchExecutionScope BatchScope; ... }` to suppress nested transactions from `FOlivePinConnector::Connect()`. One include added.
**Depends on:** nothing

### C2: Wire FOlivePlanExecutor into HandleBlueprintApplyPlanJson
**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
**What:** Version-gate on `Plan.SchemaVersion == "2.0"`. For v2.0: use FOlivePlanExecutor instead of lowerer+batch. For v1.0: keep existing path byte-for-byte identical. Add pin manifests + wiring errors to JSON result. Three new includes.
**Depends on:** C1, C3

### C5: Update HandleBlueprintPreviewPlanJson for v2.0
**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
**What:** Skip lowering for v2.0 plans, return `resolved_steps` array showing resolved function names/classes so AI can verify before applying.
**Depends on:** C1

### C4: Update Worker_Blueprint.txt prompt
**File:** `Content/SystemPrompts/Worker_Blueprint.txt`
**What:** Document v2.0 as recommended schema. Add `@step.auto`/`@step.~hint` syntax docs. Add examples. Emphasize AI doesn't need to know pin names with v2.0.
**Depends on:** C2

## Detailed spec
See `plans/phase-c-task-integration.md` for exact code changes, current code blocks, and replacement blocks.

## Build verification
Build after all tasks complete to verify compilation.
