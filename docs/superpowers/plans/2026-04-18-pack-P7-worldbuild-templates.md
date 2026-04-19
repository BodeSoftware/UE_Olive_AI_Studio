# Pack P7 — `worldbuild/` Factory Templates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 5 parametric worldbuild factory templates (`staircase`, `wall`, `pyramid`, `tower`, `arch`) that `blueprint.create_from_template` can instantiate to produce Actor Blueprints with construction scripts that procedurally arrange `StaticMeshComponent`s.

**Architecture:** These are pure JSON files under `Content/Templates/factory/worldbuild/`. No new C++. Each template declares a Blueprint whose ConstructionScript runs a plan_json containing nested `for_loop` ops that call `AddStaticMeshComponent` and `SetWorldLocation` at construction time. Sizes, mesh, and block_size are exposed as user parameters with sensible defaults.

**Tech Stack:** JSON templates, plan_json ops (`for_loop`, `call`, `make_struct`, `get_var`, `set_var`), existing `FOliveTemplateSystem::ApplyTemplate`.

**Related spec:** `docs/superpowers/specs/2026-04-18-plugin-makeover-design.md` §3.12.

**Independence:** Fully isolated. No C++ changes, no dependency on P1/P2/P3/P4/P5/P6. Can run in parallel with all others.

---

## File Structure

**Create:**
- `Content/Templates/factory/worldbuild/staircase.json`
- `Content/Templates/factory/worldbuild/wall.json`
- `Content/Templates/factory/worldbuild/pyramid.json`
- `Content/Templates/factory/worldbuild/tower.json`
- `Content/Templates/factory/worldbuild/arch.json`

**Tests:**
- `Source/OliveAIEditor/Private/Tests/Template/OliveWorldbuildTemplatesTests.cpp` — one test per template: apply it, confirm the produced Blueprint compiles without errors.

---

## Template JSON skeleton

Every worldbuild template has this shape. Use it as the reference.

```json
{
  "template_id": "worldbuild.staircase",
  "template_type": "factory",
  "display_name": "Staircase",
  "tags": "worldbuild,staircase,parametric,architecture",
  "catalog_description": "Parametric staircase of cube meshes. Params: steps, step_size (x,y,z), mesh_path.",
  "parameters": {
    "steps": { "type": "int", "default": "10" },
    "step_x": { "type": "float", "default": "100.0" },
    "step_y": { "type": "float", "default": "200.0" },
    "step_z": { "type": "float", "default": "20.0" },
    "mesh_path": { "type": "string", "default": "/Engine/BasicShapes/Cube.Cube" }
  },
  "blueprint": {
    "type": "Actor",
    "parent_class": "Actor",
    "variables": [
      { "name": "Steps",    "type": "int",    "default": "${steps}",     "editable": true, "tooltip": "Number of steps" },
      { "name": "StepX",    "type": "float",  "default": "${step_x}",    "editable": true },
      { "name": "StepY",    "type": "float",  "default": "${step_y}",    "editable": true },
      { "name": "StepZ",    "type": "float",  "default": "${step_z}",    "editable": true },
      { "name": "MeshRef",  "type": "StaticMesh*", "default": "${mesh_path}", "editable": true }
    ],
    "functions": [
      {
        "name": "ConstructionScript",
        "is_construction_script": true,
        "plan_json": {
          "entry_point": "ConstructionScript",
          "steps": [
            { "id": "fl",    "op": "for_loop", "inputs": { "FirstIndex": 0, "LastIndex": "@var.Steps" } },
            { "id": "add",   "op": "call",     "target": "AddStaticMeshComponent",
              "exec_after": "fl",
              "inputs": { "Target": "@step.auto" },
              "outputs_as_vars": { "ReturnValue": "NewComp" } },
            { "id": "setm",  "op": "call",     "target": "SetStaticMesh",
              "exec_after": "add",
              "inputs": { "Target": "@var.NewComp", "NewMesh": "@var.MeshRef" } },
            { "id": "loc",   "op": "make_struct", "target": "Vector",
              "inputs": {
                "X": "@step.mul_i_sx", "Y": 0, "Z": "@step.mul_i_sz"
              }
            },
            { "id": "mul_i_sx", "op": "call", "target": "Multiply_FloatFloat",
              "inputs": { "A": "@step.fl.Index", "B": "@var.StepX" } },
            { "id": "mul_i_sz", "op": "call", "target": "Multiply_FloatFloat",
              "inputs": { "A": "@step.fl.Index", "B": "@var.StepZ" } },
            { "id": "setloc", "op": "call", "target": "K2_SetRelativeLocation",
              "exec_after": "setm",
              "inputs": { "Target": "@var.NewComp", "NewLocation": "@step.loc" } }
          ]
        }
      }
    ]
  }
}
```

**Key plan_json conventions used:**
- `@var.Name` — reference to a Blueprint variable.
- `@step.Id` — reference to another step's output.
- `@step.Id.OutputPin` — reference to a specific pin of another step (e.g., `@step.fl.Index` reads the `Index` pin of the for_loop).
- `outputs_as_vars` — binds a step's output pin to a newly-created local variable that later steps can read via `@var.Name`.
- `exec_after` — serial wiring; omitting it makes the step pure.
- `is_construction_script: true` — signals the executor to place the plan inside the UCS graph.

If `outputs_as_vars` on call steps is not currently supported by the plan executor, fall back to using the step id as the value reference (e.g., `@step.add`) and declare the local in an explicit `set_var` step. Test Task 2 will confirm which path works; adjust the other templates accordingly.

---

## Tasks

### Task 1: Verify plan_json supports `for_loop` + construction script entry

**Files:** none changed; read-only verification.

- [ ] **Step 1: Check existing plan_json examples**

Read `Source/OliveAIEditor/Blueprint/Public/Plan/` headers and one existing factory template in `Content/Templates/factory/` (if the directory has any — if empty, skip). Confirm:
- `for_loop` op exists in `OlivePlanOps`.
- `@var.Name` resolution works on user-defined variables.
- `@step.Id.Pin` notation is supported for referring to output pins.
- Factory templates can define a `ConstructionScript` function with `is_construction_script: true`.

If any of these are missing, document the gap in `docs/superpowers/plans/p7-plan-json-gaps.md` and decide whether to:
- Simplify the templates (e.g., skip templates that need unsupported features).
- Extend the plan executor (out of scope — defer to a follow-up pack).

- [ ] **Step 2: Commit (if any findings)**

```bash
git add docs/superpowers/plans/p7-plan-json-gaps.md
git commit -m "P7: plan_json capability audit for worldbuild templates"
```

If no findings, skip this commit.

---

### Task 2: Implement `staircase.json` and verify end-to-end

**Files:**
- Create: `Content/Templates/factory/worldbuild/staircase.json`
- Create: `Source/OliveAIEditor/Private/Tests/Template/OliveWorldbuildTemplatesTests.cpp`

- [ ] **Step 1: Create the directory**

```bash
mkdir -p "Content/Templates/factory/worldbuild"
mkdir -p "Source/OliveAIEditor/Private/Tests/Template"
```

- [ ] **Step 2: Write the staircase template**

Paste the skeleton above into `staircase.json`. Keep the content as-is; do not parameterize the template_id.

- [ ] **Step 3: Write the first failing test**

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "MCP/OliveToolRegistry.h"
#include "Template/OliveTemplateSystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveWorldbuildStaircaseTest,
    "OliveAI.Template.Worldbuild.Staircase",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveWorldbuildStaircaseTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("template_id"), TEXT("worldbuild.staircase"));
    P->SetStringField(TEXT("path"), TEXT("/Game/Tests/BP_Olive_Staircase_Test"));
    TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
    ParamsObj->SetStringField(TEXT("steps"), TEXT("3"));
    P->SetObjectField(TEXT("parameters"), ParamsObj);

    FOliveToolResult R = FOliveToolRegistry::Get().InvokeTool(TEXT("blueprint.create_from_template"), P);

    TestTrue(TEXT("create_from_template should succeed"), R.bSuccess);
    if (!R.bSuccess)
    {
        AddError(FString::Printf(TEXT("Error: %s / %s"), *R.ErrorCode, *R.ErrorMessage));
    }

    // The template-application result should report that the plan executed and compile succeeded.
    if (R.Data.IsValid())
    {
        bool bCompiled = false;
        R.Data->TryGetBoolField(TEXT("compiled"), bCompiled);
        TestTrue(TEXT("Created Blueprint should compile"), bCompiled);
    }

    return true;
}
```

- [ ] **Step 4: Build + run the test**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Session Frontend > `OliveAI.Template.Worldbuild.Staircase`. Run.

If the test fails, inspect the error and adjust the staircase template. Likely issues:
- Function name mismatch (`AddStaticMeshComponent` may need a different signature in UE 5.5 — try `K2_AddStaticMeshComponentFromClass` or check `UActorComponent::CreateComponent`).
- Pin name mismatch on `for_loop.Index`.
- Variable type mismatch — try `StaticMesh` instead of `StaticMesh*`.

Iterate until the test passes.

- [ ] **Step 5: Commit**

```bash
git add Content/Templates/factory/worldbuild/staircase.json Source/OliveAIEditor/Private/Tests/Template/OliveWorldbuildTemplatesTests.cpp
git commit -m "P7: worldbuild staircase template + passing end-to-end test"
```

---

### Task 3: Implement `wall.json`

**Files:**
- Create: `Content/Templates/factory/worldbuild/wall.json`
- Modify: `Source/OliveAIEditor/Private/Tests/Template/OliveWorldbuildTemplatesTests.cpp` — add wall test.

- [ ] **Step 1: Author the template**

Pattern: two nested `for_loop`s — outer over columns (`X_Count`), inner over rows (`Y_Count`). Each iteration calls `AddStaticMeshComponent` + `K2_SetRelativeLocation`.

Parameters: `x_count` (default 10), `y_count` (default 3), `block_x` (default 100), `block_y` (default 100), `mesh_path` (default cube).

The plan_json uses the same pattern as `staircase.json` but with nested loops:

```json
{
  "op": "for_loop",
  "id": "outer",
  "inputs": { "FirstIndex": 0, "LastIndex": "@var.XCount" }
}
```

Inside the outer loop, wire a second `for_loop` with `exec_after: "outer"` and a nested LoopBody pin.

If nested for_loops are not fully supported by the plan executor, fall back to one loop that computes `x = index % x_count` and `y = index / x_count` using `Modulo_Int64Int64` and `Divide_IntInt`. Document the fallback in the template's `catalog_description`.

- [ ] **Step 2: Add test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveWorldbuildWallTest,
    "OliveAI.Template.Worldbuild.Wall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveWorldbuildWallTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("template_id"), TEXT("worldbuild.wall"));
    P->SetStringField(TEXT("path"), TEXT("/Game/Tests/BP_Olive_Wall_Test"));
    FOliveToolResult R = FOliveToolRegistry::Get().InvokeTool(TEXT("blueprint.create_from_template"), P);
    TestTrue(TEXT("wall template applies"), R.bSuccess);
    if (R.Data.IsValid())
    {
        bool bCompiled = false;
        R.Data->TryGetBoolField(TEXT("compiled"), bCompiled);
        TestTrue(TEXT("wall Blueprint compiles"), bCompiled);
    }
    return true;
}
```

- [ ] **Step 3: Build + run** until green.

- [ ] **Step 4: Commit**

```bash
git add Content/Templates/factory/worldbuild/wall.json Source/OliveAIEditor/Private/Tests/Template/OliveWorldbuildTemplatesTests.cpp
git commit -m "P7: worldbuild wall template + test"
```

---

### Task 4: Implement `tower.json`

**Files:**
- Create: `Content/Templates/factory/worldbuild/tower.json`
- Modify: test file — add tower test.

- [ ] **Step 1: Author the template**

Pattern: columns of blocks arranged in a circle, stacked N high.

Parameters: `sides` (default 8), `height` (default 10), `radius` (default 200), `block_size` (default 100), `mesh_path`.

Uses `Sin`/`Cos` calls and trig to compute each column's X/Y offset: `angle = (side_index / sides) * 2π`, `x = cos(angle) * radius`, `y = sin(angle) * radius`. Then for each column, an inner loop stacks `height` blocks along Z.

If trig calls are complex in plan_json, the fallback is a simpler tower: a single column of blocks at origin, `height` tall. Ship the simpler version and mark the parametric circle version as a stretch goal in the template's `catalog_description`.

- [ ] **Step 2: Add test** — same pattern as wall.

- [ ] **Step 3: Build + run**.

- [ ] **Step 4: Commit**

```bash
git add Content/Templates/factory/worldbuild/tower.json Source/OliveAIEditor/Private/Tests/Template/OliveWorldbuildTemplatesTests.cpp
git commit -m "P7: worldbuild tower template + test"
```

---

### Task 5: Implement `pyramid.json`

**Files:**
- Create: `Content/Templates/factory/worldbuild/pyramid.json`
- Modify: test file — add pyramid test.

- [ ] **Step 1: Author**

Pattern: for each level `L` from 0 to `levels-1`, place `(base_size - 2*L)^2` blocks centered on the origin, shifted up by `L * block_size`.

Parameters: `base_size` (default 5), `block_size` (default 100), `mesh_path`.

Pyramid math requires nested loops and conditional skip (stop when `base_size - 2*L <= 0`). If the plan executor lacks a mid-loop `break`, loop up to `ceil(base_size / 2)` and compute row count per level = `base_size - 2*L`.

- [ ] **Step 2: Add test.**

- [ ] **Step 3: Build + run.**

- [ ] **Step 4: Commit**

```bash
git add Content/Templates/factory/worldbuild/pyramid.json Source/OliveAIEditor/Private/Tests/Template/OliveWorldbuildTemplatesTests.cpp
git commit -m "P7: worldbuild pyramid template + test"
```

---

### Task 6: Implement `arch.json`

**Files:**
- Create: `Content/Templates/factory/worldbuild/arch.json`
- Modify: test file — add arch test.

- [ ] **Step 1: Author**

Pattern: semicircle of blocks. For each `i` in `0..segments-1`, angle = `π * i / (segments - 1)`, X = `radius * cos(angle)`, Z = `radius * sin(angle)`.

Parameters: `segments` (default 9), `radius` (default 300), `block_size` (default 100), `mesh_path`.

Single `for_loop`, trig calls as in tower. Fallback if trig is complex: a rectangular "arch" (two vertical stacks of N blocks with a horizontal row on top).

- [ ] **Step 2: Add test.**

- [ ] **Step 3: Build + run.**

- [ ] **Step 4: Commit**

```bash
git add Content/Templates/factory/worldbuild/arch.json Source/OliveAIEditor/Private/Tests/Template/OliveWorldbuildTemplatesTests.cpp
git commit -m "P7: worldbuild arch template + test"
```

---

### Task 7: Verify catalog discovery and CLAUDE.md update

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Verify all 5 templates show up in the catalog**

Start editor. In the Output Log, look for lines like `Loaded template 'worldbuild.staircase'`. Confirm all five loaded.

In the chat panel, send: "List available worldbuild templates." The AI should call `blueprint.list_templates(query="worldbuild")` and see all 5.

- [ ] **Step 2: Update CLAUDE.md**

In the "Key File Locations" or "Blueprint Templates" section, add a row noting the worldbuild subdirectory:

```markdown
| Worldbuild templates | `Content/Templates/factory/worldbuild/*.json` |
```

In the "Blueprint Templates" section (near the factory format paragraph), append a sentence:

> Worldbuild templates (`worldbuild.*`) produce parametric Actor Blueprints whose ConstructionScripts procedurally arrange StaticMeshComponents. Drop the generated Blueprint into a level and adjust parameters in the Details panel.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "P7: document worldbuild templates in CLAUDE.md"
```

---

## Acceptance criteria

1. `ubt-build-5.5` green.
2. `OliveAI.Template.Worldbuild.*` suite green (all 5 tests).
3. All 5 JSON files exist at `Content/Templates/factory/worldbuild/`.
4. Each template loads at editor startup (no "Loaded template" warnings).
5. Each template appears in `blueprint.list_templates` output with correct `catalog_description`.
6. Running `blueprint.create_from_template` on each produces a Blueprint that compiles without errors.
7. `CLAUDE.md` updated.

## Out of scope

- Advanced builders: `maze.json`, `bridge.json`, `castle.json`, `town.json`, `house.json`, `mansion.json`, `aqueduct.json`. These defer to a native `FOliveWorldBuilder` C++ pack in a later release.
- Run-time (in-game) spawning — these Blueprints are editor construction only.
- Material/variant selection beyond the single `mesh_path` parameter.
