# Olive AI — Phase 4: Template System

**Standalone implementation document**  
**Last updated:** Feb 25, 2026  
**Depends on:** Phase 1 (component variable fix), Phase 2 (auto-wiring)  
**Effort:** ~14-18 hours  

---

## Table of Contents

1. [Design Philosophy](#philosophy)
2. [Architecture Overview](#architecture)
3. [Template Catalog & Discovery](#discovery)
4. [Factory Templates — Complete Blueprints](#factory)
5. [Reference Templates — Patterns & Syntax](#reference)
6. [Template File Format](#format)
7. [Template Loader & Auto-Catalog](#loader)
8. [Template Executor](#executor)
9. [Tool Registration](#tools)
10. [Three Usage Modes](#modes)
11. [Starter Template Library](#starter)
12. [Adding New Templates (Expansion Guide)](#expansion)
13. [File Index](#files)
14. [Verification Checklist](#verification)

---

<a id="philosophy"></a>
## 1. Design Philosophy

### Templates are tools, not railroads

The AI should use templates when they fit, adapt them when they partially fit, reference them when they're structurally similar, and ignore them when nothing matches — all without getting stuck.

### Discovery through reasoning, not keyword matching

The AI can't be expected to match "vehicle fuel system" to a template tagged "health." Instead, templates are described *structurally* — "generic numeric resource with drain/restore" — and the AI reasons that fuel is a numeric resource. Concrete examples (health, stamina) provide fast-path matching for obvious cases.

### Templates describe structure. Examples describe domains.

Every template has:
- A **structural description** that enables reasoning across domains
- **Concrete examples** that enable instant matching for common cases

Together they cover both "make me a health system" (instant match on example) and "make me a radiation tracker" (AI reasons: numeric value that accumulates with a threshold → stat_component).

### Two tiers, one system

- **Factory templates** create complete Blueprints from parameterized plans
- **Reference templates** teach correct syntax patterns without creating anything

Both use the same file format, the same discovery mechanism, and the same folder structure. The only difference is whether the AI calls `create_from_template` or reads the patterns as context.

### Modular by default

Adding a template = adding a JSON file. No code changes, no registration, no recompile. The catalog auto-generates from template metadata at startup. Third-party plugins can drop templates into the folder and they're discovered automatically.

---

<a id="architecture"></a>
## 2. Architecture Overview

```
Content/Templates/
├── factory/
│   ├── stat_component.json          ← Creates complete Blueprint
│   ├── physics_projectile.json
│   ├── trigger_overlap_actor.json
│   ├── door_mechanism.json
│   └── spawner.json
├── reference/
│   ├── component_patterns.json      ← Teaches patterns (no creation)
│   ├── event_patterns.json
│   ├── spawning_patterns.json
│   └── behavior_tree_patterns.json
└── (future categories go here)
    ├── pcg/
    ├── niagara/
    ├── animation/
    └── widget/
```

**Runtime flow:**

```
Plugin Startup
    → Scan Content/Templates/**/*.json
    → Build catalog string from each template's metadata
    → Cache parsed templates in memory

User Request ("make a fuel system for my vehicle")
    → Brain Layer assembles prompt
    → Catalog block injected into prompt (always present, ~80 tokens)
    → AI reads catalog: "stat_component: Generic numeric resource..." 
    → AI reasons: fuel = numeric resource that drains → stat_component fits
    → AI calls blueprint.create_from_template("stat_component", {
          stat_name: "Fuel", max_value: "100", enable_regen: false,
          depletion_event: "OnFuelEmpty"
      })
    → Template executor: load JSON → substitute params → create BP → 
       add variables → execute plans per function → compile → return

User Request ("attach this arrow component to root")
    → Recipe router matches "component attach" tags
    → Reference template component_patterns.json surfaced
    → AI reads the attachment pattern → uses it in its plan
    → No template tool call — just informed plan generation
```

---

<a id="discovery"></a>
## 3. Template Catalog & Discovery

### The Catalog Block

A lightweight text block injected into every Blueprint-domain prompt. Auto-generated at startup from template metadata. Costs ~80 tokens — trivial budget for the discovery it enables.

```
[AVAILABLE BLUEPRINT TEMPLATES]
Use blueprint.create_from_template when a template fits the request.
Use blueprint.get_template to view a template's full plan as reference.
If no template fits, use apply_plan_json as normal.

Factory templates (create complete Blueprints):
- stat_component: Generic numeric resource with current/max values,
  drain/restore functions, optional regen, and threshold events.
  Examples: health, stamina, mana, shield, hunger.
- physics_projectile: Moving actor with collision, configurable 
  speed/lifetime, and on-hit response.
  Examples: bullet, rocket, thrown grenade, debris.
- trigger_overlap_actor: Actor with trigger volume, tracked state, 
  and interaction events.
  Examples: pickup, collectible, pressure plate, checkpoint.
- door_mechanism: Actor with binary state, timeline-driven animation, 
  and optional lock condition.
  Examples: door, gate, drawbridge, elevator.
- spawner: Timed or triggered actor spawning with configurable limits 
  and optional pooling.
  Examples: enemy spawner, loot dropper, wave system.

Reference templates (patterns — use blueprint.get_template to view):
- component_patterns: Attachment, transforms, property access, collision,
  Target wiring for component functions.
- event_patterns: BeginPlay, overlap, hit, input, dispatchers, custom 
  events, interface events.
- spawning_patterns: SpawnActor, deferred spawn, destroy, lifetime,
  owner/instigator, pooling.
- behavior_tree_patterns: Tasks, decorators, services, blackboard,
  patrol, chase, flee sequences.
[/AVAILABLE BLUEPRINT TEMPLATES]
```

### How discovery works

The AI doesn't "select" a template. It reads the catalog as part of its prompt context and reasons about whether any template matches the user's request.

| User says | AI reads | AI reasons | Result |
|-----------|----------|------------|--------|
| "health system" | "stat_component... Examples: health" | Direct match on example | `create_from_template("stat_component", {stat_name:"Health"})` |
| "stamina bar" | "stat_component... Examples: stamina" | Direct match on example | `create_from_template("stat_component", {stat_name:"Stamina", enable_regen:true})` |
| "vehicle fuel" | "Generic numeric resource with drain/restore" | Fuel = numeric resource that drains | `create_from_template("stat_component", {stat_name:"Fuel", depletion_event:"OnFuelEmpty"})` |
| "radiation tracker" | "Generic numeric resource... threshold events" | Radiation = accumulating value with threshold | `create_from_template("stat_component", {stat_name:"Radiation", ...})` |
| "strength attribute" | "Generic numeric resource with current/max" | Strength = numeric attribute | `create_from_template("stat_component", {stat_name:"Strength"})` |
| "custom AI behavior" | No factory match, but BT reference available | Reference useful, no factory fit | `get_template("behavior_tree_patterns")` then `apply_plan_json` |
| "save game system" | Nothing matches | Novel request, no template fit | Normal `apply_plan_json` |

### Why this works without exhaustive tagging

The structural description ("generic numeric resource with drain/restore") is the invariant. It describes what the template *does*, not what domains it applies to. The AI maps from the user's domain to the structure:

- "fuel" → drains, has a max → numeric resource ✓
- "morale" → goes up and down, has a threshold → numeric resource ✓  
- "oxygen" → depletes, regenerates in safe zones → numeric resource with regen ✓

The examples (health, stamina, mana) are just fast-path shortcuts for common cases. They're not the discovery mechanism — the structural description is.

---

<a id="factory"></a>
## 4. Factory Templates — Complete Blueprints

Factory templates create fully-wired, compiled Blueprints from parameterized plan JSON. The AI calls `create_from_template` and gets a working Blueprint back.

### What a factory template contains:

- Blueprint type and parent class
- Variable definitions with parameterized defaults
- Event dispatchers with parameter signatures
- Component hierarchy (for Actor-based templates)
- Function definitions, each with a complete plan JSON
- Parameter schema with types, defaults, and descriptions
- Example presets showing common configurations

### When the AI should use a factory template:

- User request closely matches a template's structural description
- The request can be expressed through the template's parameters
- The user doesn't need deeply custom graph logic

### When the AI should NOT use a factory template:

- The request requires logic the template doesn't cover
- The user explicitly describes custom behavior that doesn't fit parameters
- The request combines patterns from multiple templates (use reference templates + plan generation instead)

---

<a id="reference"></a>
## 5. Reference Templates — Patterns & Syntax

Reference templates don't create anything. They're collections of working plan JSON patterns the AI can read and adapt. They solve the "how do I wire this" problem by providing copy-paste-ready syntax.

### What a reference template contains:

- Named patterns, each with a description and working plan steps
- Tags for recipe router matching
- No Blueprint creation metadata (no parent class, no variables)

### How the AI uses reference templates:

1. AI encounters a sub-problem (e.g., "how do I attach this component")
2. Recipe router matches tags → surfaces component_patterns reference
3. AI reads the "attach_to_root" pattern → gets working plan steps
4. AI adapts the steps for its specific context

Reference templates are essentially **super-recipes with executable examples**.

### Dual delivery:

Reference templates are delivered two ways:
- **Via recipe router** — the template's patterns are registered as recipes with tags. The AI finds them the same way it finds any recipe.
- **Via `blueprint.get_template` tool** — the AI can explicitly request a reference template to read its full pattern library.

---

<a id="format"></a>
## 6. Template File Format

### Factory template format:

```json
{
    "template_id": "stat_component",
    "template_type": "factory",
    "display_name": "Stat Component",
    
    "catalog_description": "Generic numeric resource with current/max values, drain/restore functions, optional regen, and threshold events.",
    "catalog_examples": "health, stamina, mana, shield, hunger",
    
    "parameters": {
        "stat_name": {
            "type": "string",
            "default": "Health",
            "description": "Name of the stat (used for variable and event naming)"
        },
        "max_value": {
            "type": "float",
            "default": "100.0",
            "description": "Maximum stat value"
        },
        "start_full": {
            "type": "bool",
            "default": "true",
            "description": "Start at max value (true) or zero (false)"
        },
        "enable_regen": {
            "type": "bool",
            "default": "false",
            "description": "Auto-regenerate over time"
        },
        "regen_rate": {
            "type": "float",
            "default": "5.0",
            "description": "Points per second when regen is enabled"
        },
        "depletion_event": {
            "type": "string",
            "default": "OnDepleted",
            "description": "Event dispatcher name when stat reaches zero"
        }
    },

    "presets": [
        {
            "name": "Health",
            "params": {
                "stat_name": "Health",
                "max_value": "100",
                "depletion_event": "OnDeath"
            }
        },
        {
            "name": "Stamina",
            "params": {
                "stat_name": "Stamina",
                "max_value": "100",
                "enable_regen": "true",
                "regen_rate": "10",
                "depletion_event": "OnExhausted"
            }
        },
        {
            "name": "Mana",
            "params": {
                "stat_name": "Mana",
                "max_value": "200",
                "enable_regen": "true",
                "regen_rate": "3",
                "depletion_event": "OnManaEmpty"
            }
        }
    ],
    
    "blueprint": {
        "type": "ActorComponent",
        "parent_class": "ActorComponent",

        "variables": [
            {
                "name": "Current${stat_name}",
                "type": "Float",
                "default": "${start_full} ? ${max_value} : 0",
                "category": "${stat_name}"
            },
            {
                "name": "Max${stat_name}",
                "type": "Float",
                "default": "${max_value}",
                "category": "${stat_name}"
            }
        ],

        "event_dispatchers": [
            {
                "name": "On${stat_name}Changed",
                "params": [
                    { "name": "NewValue", "type": "Float" },
                    { "name": "Delta", "type": "Float" }
                ]
            },
            {
                "name": "${depletion_event}",
                "params": []
            }
        ],

        "functions": [
            {
                "name": "Apply${stat_name}Change",
                "description": "Change stat by delta (negative = drain, positive = restore). Clamps to [0, max]. Fires change and depletion events.",
                "inputs": [
                    { "name": "Delta", "type": "Float" }
                ],
                "outputs": [
                    { "name": "NewValue", "type": "Float" }
                ],
                "plan": {
                    "schema_version": "2.0",
                    "steps": [
                        { "step_id": "get_current", "op": "get_var", "target": "Current${stat_name}" },
                        { "step_id": "get_delta", "op": "get_var", "target": "Delta" },
                        { "step_id": "add", "op": "call", "target": "Add_FloatFloat",
                          "inputs": { "A": "@get_current.auto", "B": "@get_delta.auto" } },
                        { "step_id": "clamp", "op": "call", "target": "FClamp",
                          "inputs": { "Value": "@add.auto", "Min": "0.0", "Max": "${max_value}" },
                          "exec_after": "add" },
                        { "step_id": "set_current", "op": "set_var", "target": "Current${stat_name}",
                          "inputs": { "value": "@clamp.auto" },
                          "exec_after": "clamp" },
                        { "step_id": "calc_real_delta", "op": "call", "target": "Subtract_FloatFloat",
                          "inputs": { "A": "@clamp.auto", "B": "@get_current.auto" } },
                        { "step_id": "fire_changed", "op": "call", "target": "On${stat_name}Changed",
                          "inputs": { "NewValue": "@clamp.auto", "Delta": "@calc_real_delta.auto" },
                          "exec_after": "set_current" },
                        { "step_id": "check_depleted", "op": "call", "target": "LessEqual_FloatFloat",
                          "inputs": { "A": "@clamp.auto", "B": "0.0" } },
                        { "step_id": "branch", "op": "branch",
                          "inputs": { "Condition": "@check_depleted.auto" },
                          "exec_after": "fire_changed",
                          "exec_outputs": { "True": "fire_depleted" } },
                        { "step_id": "fire_depleted", "op": "call", "target": "${depletion_event}" },
                        { "step_id": "set_return", "op": "set_var", "target": "NewValue",
                          "inputs": { "value": "@clamp.auto" },
                          "exec_after": "branch" }
                    ]
                }
            }
        ]
    }
}
```

### Reference template format:

```json
{
    "template_id": "component_patterns",
    "template_type": "reference",
    "display_name": "Blueprint Component Patterns",

    "catalog_description": "Attachment, transforms, property access, collision, Target wiring for component functions.",
    "catalog_examples": "",

    "tags": "component attach root transform location rotation scale collision hit overlap visibility property target wire scene static skeletal",

    "patterns": {
        "attach_to_root": {
            "description": "Attach a component to the root component",
            "tags": "attach root parent child hierarchy",
            "steps": [
                { "step_id": "get_comp", "op": "get_var", "target": "MyMeshComponent" },
                { "step_id": "get_root", "op": "call", "target": "K2_GetRootComponent" },
                { "step_id": "attach", "op": "call", "target": "K2_AttachToComponent",
                  "inputs": {
                      "Target": "@get_comp.auto",
                      "Parent": "@get_root.auto",
                      "SocketName": "None",
                      "LocationRule": "KeepRelative",
                      "RotationRule": "KeepRelative",
                      "WeldSimulatedBodies": "true"
                  },
                  "exec_after": "get_root" }
            ]
        },
        "get_component_transform": {
            "description": "Get world transform of a component (position, rotation, scale)",
            "tags": "transform world location rotation position component",
            "steps": [
                { "step_id": "get_comp", "op": "get_var", "target": "MyComponent" },
                { "step_id": "get_tf", "op": "call", "target": "GetWorldTransform",
                  "inputs": { "Target": "@get_comp.auto" } }
            ]
        },
        "set_component_property": {
            "description": "Set a property on a component via setter function",
            "tags": "set property visibility location rotation material",
            "steps": [
                { "step_id": "get_comp", "op": "get_var", "target": "MyComponent" },
                { "step_id": "set_prop", "op": "call", "target": "SetVisibility",
                  "inputs": { "Target": "@get_comp.auto", "bNewVisibility": "true" },
                  "exec_after": "get_comp" }
            ]
        },
        "bind_component_hit": {
            "description": "Bind to a component's OnComponentHit event",
            "tags": "hit collision bind event delegate component",
            "steps": [
                { "step_id": "get_comp", "op": "get_var", "target": "CollisionComponent" },
                { "step_id": "bind_hit", "op": "call", "target": "OnComponentHit.AddDynamic",
                  "inputs": { "Target": "@get_comp.auto" },
                  "exec_after": "get_comp" }
            ]
        },
        "component_function_with_target": {
            "description": "Call a function that belongs to a component class, wiring Target correctly",
            "tags": "target self component function call wire",
            "steps": [
                { "step_id": "get_comp", "op": "get_var", "target": "MySceneComponent" },
                { "step_id": "call_func", "op": "call", "target": "K2_SetRelativeLocation",
                  "inputs": {
                      "Target": "@get_comp.auto",
                      "NewLocation": "@some_vector_step.auto"
                  },
                  "exec_after": "get_comp" }
            ],
            "notes": "Always use get_var to get the component reference, then wire it to Target. The executor auto-wires Target when there's exactly one matching component, but explicit wiring is more reliable."
        }
    }
}
```

### Key format rules:

- `${parameter}` substitution in factory templates — simple string replacement before plan execution
- `template_type` is either `"factory"` or `"reference"`
- `catalog_description` is the structural description used for AI reasoning (mandatory)
- `catalog_examples` is the comma-separated domain examples for fast matching (optional)
- `tags` on reference templates and patterns enable recipe router matching
- `presets` on factory templates show common configurations
- Plans use the same schema as `apply_plan_json` — no new executor code

---

<a id="loader"></a>
## 7. Template Loader & Auto-Catalog

### New class: `FOliveTemplateSystem`

**File:** `Source/OliveAIEditor/Blueprint/Private/OliveTemplateSystem.h`

```cpp
struct FOliveTemplateInfo
{
    FString TemplateId;
    FString TemplateType;       // "factory" or "reference"
    FString DisplayName;
    FString CatalogDescription;
    FString CatalogExamples;
    FString Tags;
    FString FilePath;
    TSharedPtr<FJsonObject> FullJson;
};

class OLIVEAIEDITOR_API FOliveTemplateSystem
{
public:
    static FOliveTemplateSystem& Get();

    /** Scan Content/Templates/ and load all template metadata */
    void Initialize();

    /** Rebuild after hot-reload or new templates added */
    void Reload();

    /** Get the catalog string for prompt injection */
    FString GetCatalogBlock() const;

    /** Get a specific template by ID */
    const FOliveTemplateInfo* FindTemplate(const FString& TemplateId) const;

    /** Get all templates of a given type */
    TArray<const FOliveTemplateInfo*> GetTemplatesByType(const FString& Type) const;

    /** List all available template IDs and descriptions */
    TArray<FOliveTemplateInfo> GetAvailableTemplates() const;

    /** Apply a factory template with parameter overrides */
    FOliveBlueprintWriteResult ApplyTemplate(
        const FString& TemplateId,
        const TMap<FString, FString>& Parameters,
        const FString& AssetPath);

    /** Get a template's full content as JSON string (for reference mode) */
    FString GetTemplateContent(const FString& TemplateId) const;

private:
    /** Scan a directory recursively for .json template files */
    void ScanDirectory(const FString& Directory);

    /** Parse a template file and add to registry */
    bool LoadTemplateFile(const FString& FilePath);

    /** Build the catalog string from loaded templates */
    void RebuildCatalog();

    /** Substitute ${param} tokens in a JSON string */
    FString SubstituteParameters(
        const FString& TemplateJson,
        const TMap<FString, FString>& MergedParams) const;

    /** Registered templates */
    TMap<FString, FOliveTemplateInfo> Templates;

    /** Cached catalog block string */
    FString CachedCatalog;
};
```

### Auto-catalog generation:

```cpp
void FOliveTemplateSystem::RebuildCatalog()
{
    CachedCatalog = TEXT("[AVAILABLE BLUEPRINT TEMPLATES]\n");
    CachedCatalog += TEXT("Use blueprint.create_from_template when a template fits.\n");
    CachedCatalog += TEXT("Use blueprint.get_template to view a template as reference.\n");
    CachedCatalog += TEXT("If no template fits, use apply_plan_json as normal.\n\n");

    // Group by type
    TArray<const FOliveTemplateInfo*> Factories;
    TArray<const FOliveTemplateInfo*> References;

    for (const auto& Pair : Templates)
    {
        if (Pair.Value.TemplateType == TEXT("factory"))
            Factories.Add(&Pair.Value);
        else
            References.Add(&Pair.Value);
    }

    if (Factories.Num() > 0)
    {
        CachedCatalog += TEXT("Factory templates (create complete Blueprints):\n");
        for (const FOliveTemplateInfo* T : Factories)
        {
            CachedCatalog += FString::Printf(TEXT("- %s: %s\n"),
                *T->TemplateId, *T->CatalogDescription);
            if (!T->CatalogExamples.IsEmpty())
            {
                CachedCatalog += FString::Printf(TEXT("  Examples: %s.\n"),
                    *T->CatalogExamples);
            }
        }
    }

    if (References.Num() > 0)
    {
        CachedCatalog += TEXT("\nReference templates (patterns — view with blueprint.get_template):\n");
        for (const FOliveTemplateInfo* T : References)
        {
            CachedCatalog += FString::Printf(TEXT("- %s: %s\n"),
                *T->TemplateId, *T->CatalogDescription);
        }
    }

    CachedCatalog += TEXT("[/AVAILABLE BLUEPRINT TEMPLATES]\n");

    UE_LOG(LogOliveTemplates, Log,
        TEXT("Template catalog rebuilt: %d factory, %d reference, %d chars"),
        Factories.Num(), References.Num(), CachedCatalog.Len());
}
```

### Catalog injection point:

**File:** `Source/OliveAIEditor/Private/OlivePromptAssembler.cpp`

When assembling a prompt for a blueprint-domain task, append the catalog block:

```cpp
// Inject template catalog for blueprint tasks
if (WorkerDomain == TEXT("blueprint") || WorkerDomain == TEXT("behavior_tree"))
{
    const FString Catalog = FOliveTemplateSystem::Get().GetCatalogBlock();
    if (!Catalog.IsEmpty())
    {
        ContextBlocks.Add(Catalog);
    }
}
```

---

<a id="executor"></a>
## 8. Template Executor

### `ApplyTemplate` workflow:

```cpp
FOliveBlueprintWriteResult FOliveTemplateSystem::ApplyTemplate(
    const FString& TemplateId,
    const TMap<FString, FString>& UserParams,
    const FString& AssetPath)
{
    FOliveBlueprintWriteResult Result;

    // 1. Find template
    const FOliveTemplateInfo* Info = FindTemplate(TemplateId);
    if (!Info || Info->TemplateType != TEXT("factory"))
    {
        Result.bSuccess = false;
        Result.ErrorMessage = FString::Printf(
            TEXT("Factory template '%s' not found"), *TemplateId);
        return Result;
    }

    // 2. Merge user params with defaults
    TMap<FString, FString> MergedParams;
    // Start with defaults from template parameter schema
    const TSharedPtr<FJsonObject>* ParamsObj;
    if (Info->FullJson->TryGetObjectField(TEXT("parameters"), ParamsObj))
    {
        for (const auto& Pair : (*ParamsObj)->Values)
        {
            const TSharedPtr<FJsonObject>* ParamDef;
            if (Pair.Value->TryGetObject(ParamDef))
            {
                FString Default;
                (*ParamDef)->TryGetStringField(TEXT("default"), Default);
                MergedParams.Add(Pair.Key, Default);
            }
        }
    }
    // Override with user-provided params
    for (const auto& Pair : UserParams)
    {
        MergedParams.Add(Pair.Key, Pair.Value);
    }

    // 3. Get blueprint definition and substitute parameters
    const TSharedPtr<FJsonObject>* BlueprintObj;
    if (!Info->FullJson->TryGetObjectField(TEXT("blueprint"), BlueprintObj))
    {
        Result.bSuccess = false;
        Result.ErrorMessage = TEXT("Template has no 'blueprint' definition");
        return Result;
    }

    FString BlueprintJson;
    auto Writer = TJsonWriterFactory<>::Create(&BlueprintJson);
    FJsonSerializer::Serialize((*BlueprintObj).ToSharedRef(), Writer);
    Writer->Close();

    BlueprintJson = SubstituteParameters(BlueprintJson, MergedParams);

    // 4. Create Blueprint asset
    // ... (use existing blueprint.create tool internally)

    // 5. Add variables from template
    // ... (use existing blueprint.add_variable tool internally)

    // 6. Add event dispatchers
    // ... (use existing blueprint.add_event_dispatcher tool internally)

    // 7. For each function: create function graph + execute plan
    // ... (use existing blueprint.add_function + apply_plan_json internally)

    // 8. Compile
    // ... (use existing compile manager)

    // 9. Return result with template metadata
    Result.bSuccess = true;
    Result.TemplateId = TemplateId;
    Result.AppliedParams = MergedParams;

    return Result;
}
```

### Parameter substitution:

```cpp
FString FOliveTemplateSystem::SubstituteParameters(
    const FString& Input,
    const TMap<FString, FString>& Params) const
{
    FString Result = Input;

    for (const auto& Pair : Params)
    {
        const FString Token = FString::Printf(TEXT("${%s}"), *Pair.Key);
        Result = Result.Replace(*Token, *Pair.Value);
    }

    // Warn about unsubstituted tokens
    int32 Idx = 0;
    while ((Idx = Result.Find(TEXT("${"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Idx)) != INDEX_NONE)
    {
        int32 End = Result.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Idx);
        if (End != INDEX_NONE)
        {
            FString Unresolved = Result.Mid(Idx, End - Idx + 1);
            UE_LOG(LogOliveTemplates, Warning,
                TEXT("Unsubstituted parameter in template: %s"), *Unresolved);
            Idx = End + 1;
        }
        else
        {
            break;
        }
    }

    return Result;
}
```

---

<a id="tools"></a>
## 9. Tool Registration

### Tool: `blueprint.create_from_template`

```
Parameters:
  template_id:  string (required)  — ID of the factory template
  asset_path:   string (required)  — Where to create the Blueprint
  parameters:   object (optional)  — Parameter overrides
  preset:       string (optional)  — Named preset to use as base
```

**Behavior:**
- If `preset` is specified, load preset params first, then override with `parameters`
- Returns standard `FOliveBlueprintWriteResult` with additional `template_id` and `applied_params` fields
- On failure, returns structured errors the AI can act on (same as apply_plan_json)

### Tool: `blueprint.get_template`

```
Parameters:
  template_id:  string (required)  — ID of any template (factory or reference)
  pattern:      string (optional)  — Specific pattern within a reference template
```

**Behavior:**
- Returns the template content as readable JSON
- For factory templates: returns parameter schema + presets + function plans
- For reference templates: returns all patterns (or a specific pattern if `pattern` specified)
- AI reads this as context — no Blueprint is created
- This is Mode 3 (reference only)

### Tool: `blueprint.list_templates`

```
Parameters:
  type:         string (optional)  — "factory", "reference", or omit for all
```

**Behavior:**
- Returns the same catalog info that's in the prompt, but as structured JSON
- Useful if the AI wants to explore templates interactively

---

<a id="modes"></a>
## 10. Three Usage Modes

### Mode 1: Direct Apply

AI recognizes a strong match and creates a Blueprint directly from a template.

```
User: "Create a health component for my player"

AI calls: blueprint.create_from_template({
    template_id: "stat_component",
    asset_path: "/Game/Components/BP_HealthComponent",
    preset: "Health",
    parameters: { "max_value": "150" }
})
```

Result: Complete Blueprint with variables, dispatchers, and functions. Zero plan generation needed.

### Mode 2: Fork and Customize

AI uses a template as a starting point, then modifies the result with additional plan calls.

```
User: "Create a health component that also has armor that absorbs 
       damage before health is affected"

AI calls: blueprint.create_from_template({
    template_id: "stat_component",
    asset_path: "/Game/Components/BP_HealthComponent",
    preset: "Health"
})

Then: blueprint.add_variable({
    asset_path: "/Game/Components/BP_HealthComponent",
    name: "CurrentArmor", type: "Float", default: "50"
})

Then: blueprint.apply_plan_json({
    asset_path: "/Game/Components/BP_HealthComponent",
    graph_name: "ApplyHealthChange",
    plan: { ... modified damage logic that checks armor first ... }
})
```

Result: Template provides the scaffold, AI provides the custom armor logic.

### Mode 3: Reference Only

AI reads a template to learn patterns, then writes its own plan.

```
User: "Create a custom magic system where spells cost different 
       amounts of mana and some spells have cooldowns"

AI calls: blueprint.get_template({ template_id: "stat_component" })
  → Reads the drain/restore pattern, adapts for mana

AI calls: blueprint.get_template({ 
    template_id: "component_patterns", 
    pattern: "component_function_with_target" 
})
  → Reads how to wire component function targets

Then: blueprint.apply_plan_json({ ... custom magic system plan ... })
```

Result: AI learned correct syntax from templates, then built something novel.

---

<a id="starter"></a>
## 11. Starter Template Library

### Factory Templates (5)

| ID | Structural Description | Examples | Parameters |
|----|----------------------|----------|------------|
| `stat_component` | Generic numeric resource with current/max, drain/restore, optional regen, threshold events | health, stamina, mana, shield, hunger | stat_name, max_value, start_full, enable_regen, regen_rate, depletion_event |
| `physics_projectile` | Moving actor with collision, configurable speed/lifetime, on-hit response | bullet, rocket, grenade, debris | speed, lifetime, damage, collision_radius, destroy_on_hit |
| `trigger_overlap_actor` | Actor with trigger volume, tracked state, interaction events | pickup, collectible, pressure plate, checkpoint | trigger_shape, trigger_size, interaction_event, respawn_time, one_shot |
| `door_mechanism` | Actor with binary state, timeline-driven animation, optional lock | door, gate, drawbridge, elevator | open_rotation, open_time, auto_close, auto_close_delay, starts_locked |
| `spawner` | Timed or triggered actor spawning with limits and optional pooling | enemy spawner, loot dropper, wave system | spawn_class, spawn_interval, max_alive, max_total, spawn_radius |

### Reference Templates (4)

| ID | Description | Pattern Count |
|----|-------------|---------------|
| `component_patterns` | Attachment, transforms, property access, collision, Target wiring | ~6 patterns |
| `event_patterns` | BeginPlay, overlap, hit, input, dispatchers, custom events, interfaces | ~8 patterns |
| `spawning_patterns` | SpawnActor, deferred spawn, destroy, lifetime, owner/instigator | ~5 patterns |
| `behavior_tree_patterns` | Tasks, decorators, services, blackboard, patrol/chase/flee | ~6 patterns |

Total: 9 templates, ~25 reference patterns.

---

<a id="expansion"></a>
## 12. Adding New Templates (Expansion Guide)

### Adding a factory template:

1. Create `Content/Templates/factory/my_template.json`
2. Follow the factory template format (Section 6)
3. Include `catalog_description` (structural — what it does, not what domain)
4. Include `catalog_examples` (domain examples for fast matching)
5. Include `presets` for common configurations
6. Restart editor (or call `FOliveTemplateSystem::Get().Reload()`)
7. Template appears in catalog automatically

**No code changes. No recompile.**

### Adding a reference template:

1. Create `Content/Templates/reference/my_patterns.json`
2. Follow the reference template format (Section 6)
3. Include `tags` for recipe router matching
4. Each pattern gets its own `tags` and `description`
5. Restart editor
6. Patterns are discoverable via recipe router and `get_template` tool

**No code changes. No recompile.**

### Adding a new category:

1. Create `Content/Templates/my_category/` folder
2. Add template JSON files to it
3. The scanner picks them up — categories are just folders

**No code changes. No recompile.**

### Third-party expansion:

A marketplace plugin can add templates by placing files in:
```
Plugins/MyPlugin/Content/Templates/factory/
Plugins/MyPlugin/Content/Templates/reference/
```

The template system scans all `Content/Templates/` directories across all loaded plugins.

### Template quality checklist:

- [ ] `catalog_description` describes structure, not domain
- [ ] `catalog_examples` covers 3-5 common use cases
- [ ] Plans use `@step.auto` for all data wires
- [ ] Plans have been tested through the plan executor
- [ ] Factory templates compile with zero errors using default parameters
- [ ] Each preset compiles with zero errors
- [ ] Reference pattern steps are copy-paste-ready (no placeholder names)

---

<a id="files"></a>
## 13. File Index

| File | Purpose |
|------|---------|
| `Source/OliveAIEditor/Blueprint/Private/OliveTemplateSystem.h` | Template system class declaration |
| `Source/OliveAIEditor/Blueprint/Private/OliveTemplateSystem.cpp` | Loader, catalog builder, executor, parameter substitution |
| `Source/OliveAIEditor/Tools/OliveToolRegistration.cpp` | Register create_from_template, get_template, list_templates |
| `Source/OliveAIEditor/Private/OlivePromptAssembler.cpp` | Catalog injection into blueprint-domain prompts |
| `Content/Templates/factory/stat_component.json` | Stat component factory template |
| `Content/Templates/factory/physics_projectile.json` | Projectile factory template |
| `Content/Templates/factory/trigger_overlap_actor.json` | Trigger/pickup factory template |
| `Content/Templates/factory/door_mechanism.json` | Door/gate factory template |
| `Content/Templates/factory/spawner.json` | Spawner factory template |
| `Content/Templates/reference/component_patterns.json` | Component patterns reference |
| `Content/Templates/reference/event_patterns.json` | Event patterns reference |
| `Content/Templates/reference/spawning_patterns.json` | Spawning patterns reference |
| `Content/Templates/reference/behavior_tree_patterns.json` | BT patterns reference |

---

<a id="verification"></a>
## 14. Verification Checklist

### System Tests

| Test | Expected |
|------|----------|
| Plugin startup with templates in folder | Catalog auto-generated, log shows count |
| Plugin startup with empty Templates folder | No crash, empty catalog |
| Plugin startup with malformed JSON in folder | Skip bad file, log warning, load others |
| `Reload()` after adding new template file | New template appears in catalog |

### Factory Template Tests

| Test | Expected |
|------|----------|
| `create_from_template("stat_component", default params)` | ✅ BP created, compiles, has Current/Max Health vars |
| `create_from_template("stat_component", {stat_name:"Stamina", enable_regen:true})` | ✅ BP has CurrentStamina/MaxStamina, regen function |
| `create_from_template("stat_component", preset:"Mana")` | ✅ BP matches Mana preset config |
| `create_from_template("stat_component", preset:"Health", {max_value:"200"})` | ✅ Preset + override, MaxHealth = 200 |
| `create_from_template("nonexistent")` | ❌ Template not found error |
| `create_from_template("component_patterns")` | ❌ Not a factory template error |
| All 5 factory templates with default params | ✅ All compile with zero errors |
| All presets across all templates | ✅ All compile with zero errors |

### Reference Template Tests

| Test | Expected |
|------|----------|
| `get_template("component_patterns")` | Returns all patterns with steps |
| `get_template("component_patterns", pattern:"attach_to_root")` | Returns just that pattern |
| `get_template("nonexistent")` | ❌ Template not found |
| Recipe router: "attach component root" | Matches component_patterns tags |
| Recipe router: "spawn actor transform" | Matches spawning_patterns tags |

### Discovery Tests

| Test | Expected |
|------|----------|
| "make a health system" | AI matches "health" example → create_from_template |
| "make a stamina bar" | AI matches "stamina" example → create_from_template |
| "make a fuel gauge for vehicle" | AI reasons: fuel = numeric resource → stat_component |
| "make a radiation exposure tracker" | AI reasons: accumulating value with threshold → stat_component |
| "make a save game system" | No template match → normal apply_plan_json |
| "attach an arrow component" | Recipe router → component_patterns reference |

### Expansion Tests

| Test | Expected |
|------|----------|
| Add new JSON file to factory/ folder + reload | New template in catalog |
| Add new JSON file to reference/ folder + reload | New template discoverable |
| Create new category folder + add templates + reload | Templates loaded from new folder |
| Malformed JSON file in templates folder | Skipped with warning, others load fine |
| Template with unsubstituted ${param} | Warning logged, execution continues |
