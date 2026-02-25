# Task: Add `components` and `event_graphs` support to `FOliveTemplateSystem::ApplyTemplate`

## Context

Factory templates (JSON files in `Content/Templates/`) define parameterized Blueprints. The `ApplyTemplate` function in `Source/OliveAIEditor/Blueprint/Private/Templates/OliveTemplateSystem.cpp` currently processes these sections from the `"blueprint"` object:

- Step 7: `"variables"` → calls `Writer.AddVariable()`
- Step 8: `"event_dispatchers"` → calls `Writer.AddEventDispatcher()`
- Step 9: `"functions"` → calls `Writer.AddFunction()` + resolves/executes the inline `"plan"` on the function graph

It does **not** handle `"components"` or `"event_graphs"`, which are needed by the new `projectile.json` template (and future Actor-based templates like doors, spawners, triggers).

## What to implement

### 1. Components section (add between step 6 and step 7)

After loading the Blueprint (step 6) and before adding variables (step 7), iterate the `"components"` array and create each component.

**JSON shape:**
```json
"components": [
    {
        "class": "SphereComponent",
        "name": "CollisionSphere",
        "is_root": true,
        "properties": {
            "SphereRadius": "10.0",
            "CollisionProfileName": "BlockAllDynamic"
        }
    },
    {
        "class": "StaticMeshComponent",
        "name": "BulletMesh",
        "parent": "CollisionSphere",
        "properties": {
            "CollisionProfileName": "NoCollision"
        }
    },
    {
        "class": "ProjectileMovementComponent",
        "name": "ProjectileMovement",
        "properties": {
            "InitialSpeed": "3000.0",
            "MaxSpeed": "3000.0"
        }
    }
]
```

**Implementation:**
- Use `FOliveComponentWriter::Get()` (already available in the file, same pattern as `FOliveBlueprintWriter`)
- For each component object:
  1. Read `"class"`, `"name"`, `"parent"` (optional, default empty), `"is_root"` (optional bool)
  2. Call `FOliveComponentWriter::Get().AddComponent(AssetPath, Class, Name, Parent)`
  3. If `"is_root"` is true, call `FOliveComponentWriter::Get().SetRootComponent(AssetPath, Name)`
  4. If `"properties"` object exists, call `FOliveComponentWriter::Get().ModifyComponent(AssetPath, Name, PropertiesMap)` where `PropertiesMap` is `TMap<FString, FString>` built from the JSON key-value pairs
  5. Track created components in a `TArray<FString> CreatedComponents` and include in the result JSON
- On failure, append to `Warnings` and continue (non-fatal, same pattern as variables)

### 2. Event graphs section (add after step 9, before step 10 compile)

After processing functions (step 9) and before compiling (step 10), iterate the `"event_graphs"` array and execute each plan against the EventGraph.

**JSON shape:**
```json
"event_graphs": [
    {
        "name": "SetupLifespan",
        "description": "Set projectile lifespan on BeginPlay",
        "plan": {
            "schema_version": "2.0",
            "steps": [
                {
                    "step_id": "begin_play",
                    "op": "event",
                    "target": "BeginPlay"
                },
                {
                    "step_id": "set_lifespan",
                    "op": "call",
                    "target": "SetLifeSpan",
                    "inputs": { "InLifespan": "3.0" },
                    "exec_after": "begin_play"
                }
            ]
        }
    }
]
```

**Implementation:**
- For each event_graph entry:
  1. Read the `"plan"` object
  2. Parse it: `FOliveIRBlueprintPlan Plan = FOliveIRBlueprintPlan::FromJson(PlanObj)`
  3. Resolve it: `FOliveBlueprintPlanResolver::Resolve(Plan, Blueprint)`
  4. Find the EventGraph: iterate `Blueprint->UbergraphPages` and find the graph named `"EventGraph"` (same approach used in `HandleBlueprintApplyPlanJson`)
  5. Execute: `FOlivePlanExecutor PlanExecutor; PlanExecutor.Execute(Plan, ResolvedSteps, Blueprint, EventGraph, AssetPath, "EventGraph")`
  6. Append errors/warnings same as the function plan pattern
- The `"name"` and `"description"` fields are metadata only (for catalog display), not graph names

### 3. Update result JSON

Add `"components"` to the result data alongside the existing `"variables"`, `"event_dispatchers"`, and `"functions"` arrays:

```cpp
ResultData->SetArrayField(TEXT("components"), StringArrayToJson(CreatedComponents));
```

Also update the log line to include component count.

## Files to modify

- `Source/OliveAIEditor/Blueprint/Private/Templates/OliveTemplateSystem.cpp` — the `ApplyTemplate` function (around line 45440-45710 in bundled_code.txt)

## Reference code patterns

The function plan execution in step 9 (around line 45526-45651) is the exact pattern to follow for event_graphs — same resolve → find graph → execute flow, just targeting `Blueprint->UbergraphPages` instead of `Blueprint->FunctionGraphs`.

The `HandleBlueprintAddComponent` tool handler (around line 26441) shows how component creation params are extracted from JSON — same field names (`class`, `name`, `parent`).

## Testing

After implementation, the `projectile.json` template should work with:
```json
{
    "template_id": "projectile",
    "asset_path": "/Game/Blueprints/BP_TestBullet",
    "preset": "Bullet"
}
```

Expected result: Actor Blueprint with 3 components (CollisionSphere as root, BulletMesh, ProjectileMovement), 3 variables, 2 event dispatchers, 2 functions with wired plans, and a BeginPlay→SetLifeSpan chain in the EventGraph.
