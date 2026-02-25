# Phase 4: Template System -- Implementation Plan

**Architect:** Olive AI Architect Agent
**Date:** 2026-02-25 (updated 2026-02-25 post-P0/P1/P2)
**Depends on:** Phase 1 (component fix), Phase 2 (auto-wiring), Phase 3 (error recovery) — **all completed**
**Design source:** `plans/olive_phase4_template_system.md`

> **Post-P0/P1/P2 Note:** Priority 0 (universal add_node), Priority 1 (component variables),
> and Priority 2 (auto-wiring) are fully implemented. Key implications for this plan:
> - **Line numbers in this document are approximate.** Files were modified during P0-P2.
>   The coder should search by function/symbol name, not line number.
> - **Plan JSON is now more robust:** Resolver warns on unknown variables (not silent),
>   Phase 1.5 auto-wires component Targets from string literals, validator accepts
>   string-literal component names. Templates using plan JSON benefit automatically.
> - **Error classification exists:** `EOliveErrorCategory` in `OliveSelfCorrectionPolicy.h`.
>   New template error codes must be registered there (see Task 6b addendum).

---

## Overview

This document is the task-by-task implementation plan for the Phase 4 Template System. Each task specifies exact file paths, interface-level code, dependencies, integration points, and verification steps. The coder agent follows this document directly.

**Key architectural decision:** Templates live as plain JSON files under `Content/Templates/`. They are loaded at startup via `IPlatformFile` (same pattern as recipe loading in `OliveCrossSystemToolHandlers.cpp`). No UAsset packaging, no Content Browser integration, no cooked data. This is the same disk-file pattern used by `Content/SystemPrompts/` and `Content/SystemPrompts/Knowledge/recipes/`.

**CLI-first:** All template discovery happens via MCP tools (`blueprint.create_from_template`, `blueprint.get_template`, `blueprint.list_templates`) and system prompt catalog injection. No Slate UI work.

---

## File Map

### New Files

| File | Purpose |
|------|---------|
| `Source/OliveAIEditor/Blueprint/Public/Template/OliveTemplateSystem.h` | `FOliveTemplateSystem` singleton + `FOliveTemplateInfo` struct |
| `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp` | Loading, catalog, parameter substitution, `ApplyTemplate` |
| `Content/Templates/factory/stat_component.json` | First factory template (priority) |
| `Content/Templates/reference/component_patterns.json` | First reference template |

### Modified Files

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Add `RegisterTemplateTools()`, 3 handler methods |
| `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h` | Declare `RegisterTemplateTools()` + handler signatures |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` | Add 3 schema functions |
| `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h` | Declare 3 schema functions |
| `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp` | Inject catalog block into `GetCapabilityKnowledge` flow |
| `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp` | Add `FOliveTemplateSystem::Get().Initialize()` in startup |

---

## Task 1: FOliveTemplateSystem Core Class (Header + Stub Source)

**Priority:** Must be first -- everything depends on it.
**Files to create:**
- `Source/OliveAIEditor/Blueprint/Public/Template/OliveTemplateSystem.h`
- `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp`

**Note on include paths:** The Build.cs at `Source/OliveAIEditor/OliveAIEditor.Build.cs` already adds recursive include paths for the `Blueprint` sub-module (lines 14-37). Since the new files are under `Blueprint/Public/Template/` and `Blueprint/Private/Template/`, they will be auto-discovered by the existing build rules. **No Build.cs changes needed.**

### Header: `OliveTemplateSystem.h`

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MCP/OliveToolRegistry.h"  // FOliveToolResult

DECLARE_LOG_CATEGORY_EXTERN(LogOliveTemplates, Log, All);

/**
 * Metadata for a loaded template (both factory and reference).
 * Populated during Initialize() from JSON files on disk.
 */
struct OLIVEAIEDITOR_API FOliveTemplateInfo
{
    /** Unique template identifier (filename without extension) */
    FString TemplateId;

    /** "factory" or "reference" */
    FString TemplateType;

    /** Human-readable display name */
    FString DisplayName;

    /** Structural description for AI reasoning (mandatory) */
    FString CatalogDescription;

    /** Comma-separated domain examples for fast matching (optional) */
    FString CatalogExamples;

    /** Space-separated tags for reference templates */
    FString Tags;

    /** Absolute path to the JSON file on disk */
    FString FilePath;

    /** Full parsed JSON content (cached for ApplyTemplate and GetTemplateContent) */
    TSharedPtr<FJsonObject> FullJson;
};

/**
 * FOliveTemplateSystem
 *
 * Loads template JSON files from Content/Templates/, builds an auto-generated
 * catalog block for prompt injection, and provides ApplyTemplate() for factory
 * template execution.
 *
 * Singleton. Game thread only.
 *
 * File discovery pattern matches OlivePromptAssembler::LoadPromptTemplates()
 * and FOliveCrossSystemToolHandlers::LoadRecipeLibrary() -- uses IPlatformFile
 * to scan disk directories under the plugin's Content/ folder.
 */
class OLIVEAIEDITOR_API FOliveTemplateSystem
{
public:
    static FOliveTemplateSystem& Get();

    /**
     * Scan Content/Templates/ recursively and load all template metadata.
     * Call after FOliveNodeCatalog::Initialize() and before prompt assembler
     * needs the catalog block.
     */
    void Initialize();

    /** Hot-reload: re-scan and rebuild everything. */
    void Reload();

    /** Tear down cached data. Called during module shutdown. */
    void Shutdown();

    // ============================================================
    // Query
    // ============================================================

    /** Get the cached catalog block string for prompt injection. */
    const FString& GetCatalogBlock() const { return CachedCatalog; }

    /** Find a template by ID. Returns nullptr if not found. */
    const FOliveTemplateInfo* FindTemplate(const FString& TemplateId) const;

    /** Get all loaded template infos. */
    const TMap<FString, FOliveTemplateInfo>& GetAllTemplates() const { return Templates; }

    /** Get templates filtered by type ("factory" or "reference"). */
    TArray<const FOliveTemplateInfo*> GetTemplatesByType(const FString& Type) const;

    /** Return true if at least one template is loaded. */
    bool HasTemplates() const { return Templates.Num() > 0; }

    // ============================================================
    // Execution
    // ============================================================

    /**
     * Apply a factory template: create Blueprint, add variables, add dispatchers,
     * create functions, execute plan JSON for each function, compile.
     *
     * @param TemplateId   Factory template ID
     * @param UserParams   Parameter overrides from the AI
     * @param PresetName   Optional preset name (applied before UserParams)
     * @param AssetPath    Where to create the Blueprint (/Game/...)
     * @return Tool result with created asset info or error
     */
    FOliveToolResult ApplyTemplate(
        const FString& TemplateId,
        const TMap<FString, FString>& UserParams,
        const FString& PresetName,
        const FString& AssetPath);

    /**
     * Get a template's content as a formatted string for AI reference reading.
     * For factory templates: parameter schema + presets + function plan outlines.
     * For reference templates: all patterns (or a specific one).
     *
     * @param TemplateId   Template ID
     * @param PatternName  For reference templates, optional specific pattern
     * @return Formatted content string, or empty + error via out param
     */
    FString GetTemplateContent(
        const FString& TemplateId,
        const FString& PatternName = TEXT("")) const;

private:
    FOliveTemplateSystem() = default;

    // Non-copyable
    FOliveTemplateSystem(const FOliveTemplateSystem&) = delete;
    FOliveTemplateSystem& operator=(const FOliveTemplateSystem&) = delete;

    // ============================================================
    // Loading
    // ============================================================

    /** Get the plugin's Content/Templates/ directory path. */
    FString GetTemplatesDirectory() const;

    /** Recursively scan a directory for .json files and load them. */
    void ScanDirectory(const FString& Directory);

    /** Parse a single JSON file and register it as a template. */
    bool LoadTemplateFile(const FString& FilePath);

    // ============================================================
    // Catalog
    // ============================================================

    /** Rebuild CachedCatalog from all loaded Templates. */
    void RebuildCatalog();

    // ============================================================
    // Parameter Substitution
    // ============================================================

    /**
     * Merge default parameters from the template schema with user overrides.
     * If PresetName is non-empty, preset values are applied between defaults
     * and user overrides.
     */
    TMap<FString, FString> MergeParameters(
        const FOliveTemplateInfo& Info,
        const TMap<FString, FString>& UserParams,
        const FString& PresetName) const;

    /**
     * Replace ${param} tokens in a JSON string with values from MergedParams.
     * Logs warnings for any unsubstituted tokens.
     */
    FString SubstituteParameters(
        const FString& Input,
        const TMap<FString, FString>& MergedParams) const;

    /**
     * Evaluate simple conditional expressions like "${start_full} ? ${max_value} : 0".
     * Only supports bool ternary for defaults. Returns the substituted string
     * with conditionals resolved.
     */
    FString EvaluateConditionals(
        const FString& Input,
        const TMap<FString, FString>& MergedParams) const;

    // ============================================================
    // State
    // ============================================================

    /** All loaded templates keyed by TemplateId */
    TMap<FString, FOliveTemplateInfo> Templates;

    /** Cached catalog block text (rebuilt on Initialize/Reload) */
    FString CachedCatalog;

    /** Whether Initialize() has been called */
    bool bInitialized = false;
};
```

### Source: `OliveTemplateSystem.cpp` (Stub + Loading + Catalog)

Implement in this order within the .cpp:

1. **Singleton Get()** -- standard static-local pattern.
2. **GetTemplatesDirectory()** -- `FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio/Content/Templates"))`. Same pattern as `OlivePromptAssembler.cpp` line 483 and `OliveCrossSystemToolHandlers.cpp` line 1002.
3. **Initialize()** -- Call `ScanDirectory(GetTemplatesDirectory())`, then `RebuildCatalog()`. Log template count. Set `bInitialized = true`.
4. **Reload()** -- `Templates.Empty()`, `CachedCatalog.Empty()`, then re-run Initialize.
5. **Shutdown()** -- `Templates.Empty()`, `CachedCatalog.Empty()`, `bInitialized = false`.
6. **ScanDirectory()** -- Use `IPlatformFile::Get().IterateDirectoryRecursively()`. For each file ending in `.json`, call `LoadTemplateFile()`. This is the recursive version (unlike recipe loading which is flat). Pattern reference: `OlivePromptAssembler.cpp` lines 498-525 but using `IterateDirectoryRecursively` instead of `IterateDirectory`.
7. **LoadTemplateFile()** -- `FFileHelper::LoadFileToString`, `FJsonSerializer::Deserialize`, extract required fields (`template_id`, `template_type`, `catalog_description`), populate `FOliveTemplateInfo`, store in `Templates` map keyed by `template_id`. Return false and log warning if JSON is malformed or missing required fields.
8. **FindTemplate()** -- Simple `Templates.Find()`.
9. **GetTemplatesByType()** -- Iterate Templates, filter by TemplateType.
10. **RebuildCatalog()** -- Build the catalog string exactly as shown in `plans/olive_phase4_template_system.md` Section 7. Group by type (factory first, reference second). Use the `CatalogDescription` and `CatalogExamples` fields.
11. **SubstituteParameters()** -- String replace `${key}` with value. Warn on unsubstituted tokens (find `${` without a match). Exactly as shown in plan Section 8.
12. **EvaluateConditionals()** -- Regex find `"EXPR ? VAL_TRUE : VAL_FALSE"` where EXPR is `true`/`false`. Replace with appropriate value. This handles the stat_component default: `"${start_full} ? ${max_value} : 0"`. Keep it simple -- only bool ternary.
13. **MergeParameters()** -- Start with defaults from `parameters` object in FullJson, overlay preset values if preset found, overlay UserParams last.
14. **ApplyTemplate()** -- Stubbed for Task 4.
15. **GetTemplateContent()** -- Stubbed for Task 4.

### Verification

After Task 1, the plugin should compile. Run the build command:
```
"C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" UE_Olive_AI_ToolkitEditor Win64 Development "-Project=B:/Unreal Projects/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject" -WaitMutex
```

No templates exist yet so Initialize() will log "0 factory, 0 reference" -- that is correct.

---

## Task 2: Template JSON Files (Starter Library)

**Priority:** Second -- needed for testing Tasks 3-5.
**Depends on:** Nothing (these are data files, but Task 4 needs them for testing).

**Files to create:**
- `Content/Templates/factory/stat_component.json`
- `Content/Templates/reference/component_patterns.json`

### 2a: stat_component.json

Use the complete JSON from `plans/olive_phase4_template_system.md` Section 6, "Factory template format" example. This is the priority template. Copy it verbatim.

**CRITICAL: The plan JSON inside each function must use `schema_version: "2.0"`** so the apply_plan_json handler routes to FOlivePlanExecutor (v2 path). Verify the plan shown in the design doc already has this.

The stat_component.json template should have:
- `template_id`: `"stat_component"`
- `template_type`: `"factory"`
- `blueprint.type`: `"ActorComponent"` (maps to parent_class `ActorComponent`)
- `blueprint.parent_class`: `"ActorComponent"`
- Variables: `Current${stat_name}` (Float), `Max${stat_name}` (Float)
- Event dispatchers: `On${stat_name}Changed` (NewValue:Float, Delta:Float), `${depletion_event}` (no params)
- Functions: `Apply${stat_name}Change` with the plan from the design doc
- Parameters: stat_name, max_value, start_full, enable_regen, regen_rate, depletion_event
- Presets: Health, Stamina, Mana

**One thing to verify:** The plan steps in the template use ops like `get_var`, `call`, `set_var`, `branch` -- all present in `OlivePlanOps` namespace (`BlueprintPlanIR.h`). The function targets like `Add_FloatFloat`, `FClamp`, `Subtract_FloatFloat`, `LessEqual_FloatFloat` must be resolvable by `FOliveFunctionResolver`. These are standard K2 math library functions. The function resolver's alias map (verified in Phase A) includes these. **No issues expected.**

**One issue to watch:** The `Apply${stat_name}Change` function has `inputs` and `outputs` defined in the template, but the `blueprint.add_function` tool creates the function graph. The template executor (Task 4) needs to call `blueprint.add_function` with the correct signature first, then `blueprint.apply_plan_json` to fill in the graph logic.

### 2b: component_patterns.json

Use the complete JSON from `plans/olive_phase4_template_system.md` Section 6, "Reference template format" example. Copy it verbatim.

### Additional templates (lower priority, create after core system works)

These can be created later. For initial implementation, stat_component + component_patterns are sufficient to prove the system works end-to-end.

### Verification

After Task 2, restart the editor (or call `FOliveTemplateSystem::Get().Reload()` from console). The log should show:
```
Template catalog rebuilt: 1 factory, 1 reference, N chars
```

---

## Task 3: Tool Registration (Schemas + Registration + Handler Stubs)

**Priority:** Third -- tools need to be visible before handlers work.
**Depends on:** Task 1 (FOliveTemplateSystem class must exist for handler stubs to reference it).

### 3a: Schema Definitions

**File to modify:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`
**Header to modify:** `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h`
(The header is: check what the actual file path is)

First, check the schema header location:

The schemas are in a namespace `OliveBlueprintSchemas` declared in the header. Add 3 new functions:

```cpp
// In OliveBlueprintSchemas.h (or wherever the namespace declaration lives):
namespace OliveBlueprintSchemas
{
    // ... existing schemas ...

    /** Schema for blueprint.create_from_template */
    TSharedPtr<FJsonObject> BlueprintCreateFromTemplate();

    /** Schema for blueprint.get_template */
    TSharedPtr<FJsonObject> BlueprintGetTemplate();

    /** Schema for blueprint.list_templates */
    TSharedPtr<FJsonObject> BlueprintListTemplates();
}
```

**Schema: BlueprintCreateFromTemplate()**
```
Properties:
  template_id:  string (required)  -- "ID of the factory template to instantiate"
  asset_path:   string (required)  -- "Where to create the Blueprint (e.g., /Game/Blueprints/BP_Health)"
  parameters:   object (optional)  -- "Parameter overrides as key-value pairs"
                                      additionalProperties: { type: "string" }
  preset:       string (optional)  -- "Named preset to use as base (e.g., 'Health', 'Stamina')"
Required: ["template_id", "asset_path"]
```

**Schema: BlueprintGetTemplate()**
```
Properties:
  template_id:  string (required)  -- "ID of the template to view (factory or reference)"
  pattern:      string (optional)  -- "Specific pattern name within a reference template"
Required: ["template_id"]
```

**Schema: BlueprintListTemplates()**
```
Properties:
  type:         string (optional)  -- "Filter by template type"
                enum: ["factory", "reference"]
Required: []  (no required params)
```

Use the existing helper functions (`StringProp`, `ObjectProp`, `EnumProp`, `AddRequired`, `MakeSchema`, `MakeProperties`) from `OliveBlueprintSchemas.cpp`.

### 3b: Registration

**File to modify:** `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h`

Add to private section (after `RegisterPlanTools`):
```cpp
    void RegisterTemplateTools();

    FOliveToolResult HandleBlueprintCreateFromTemplate(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintGetTemplate(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleBlueprintListTemplates(const TSharedPtr<FJsonObject>& Params);
```

**File to modify:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

Add `RegisterTemplateTools()` method (similar to `RegisterPlanTools` at line 5720):

```cpp
void FOliveBlueprintToolHandlers::RegisterTemplateTools()
{
    FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

    Registry.RegisterTool(
        TEXT("blueprint.create_from_template"),
        TEXT("Create a complete Blueprint from a factory template. "
             "Templates provide parameterized, pre-wired Blueprints for common patterns "
             "(health, projectile, trigger, door, spawner). "
             "Use blueprint.list_templates or the catalog in context to discover available templates."),
        OliveBlueprintSchemas::BlueprintCreateFromTemplate(),
        FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCreateFromTemplate),
        {TEXT("blueprint"), TEXT("write"), TEXT("template")},
        TEXT("blueprint")
    );
    RegisteredToolNames.Add(TEXT("blueprint.create_from_template"));

    Registry.RegisterTool(
        TEXT("blueprint.get_template"),
        TEXT("View a template's full content (parameter schema, presets, plan patterns). "
             "Use this to read patterns as reference before writing your own plan."),
        OliveBlueprintSchemas::BlueprintGetTemplate(),
        FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintGetTemplate),
        {TEXT("blueprint"), TEXT("read"), TEXT("template")},
        TEXT("blueprint")
    );
    RegisteredToolNames.Add(TEXT("blueprint.get_template"));

    Registry.RegisterTool(
        TEXT("blueprint.list_templates"),
        TEXT("List available templates with descriptions and examples."),
        OliveBlueprintSchemas::BlueprintListTemplates(),
        FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintListTemplates),
        {TEXT("blueprint"), TEXT("read"), TEXT("template")},
        TEXT("blueprint")
    );
    RegisteredToolNames.Add(TEXT("blueprint.list_templates"));

    UE_LOG(LogOliveBPTools, Log, TEXT("Registered template tools (create_from_template, get_template, list_templates)"));
}
```

**Call site:** In `RegisterAllTools()` (line 425-448 of OliveBlueprintToolHandlers.cpp), add after the plan tools block:

```cpp
    // Template tools (gated by template system availability)
    if (FOliveTemplateSystem::Get().HasTemplates())
    {
        RegisterTemplateTools();
    }
```

**WAIT**: This creates a startup ordering problem. `RegisterAllTools()` is called at step 6 in the startup sequence (`OliveAIEditorModule.cpp` line 202). `FOliveTemplateSystem::Get().Initialize()` hasn't been called yet at that point. We need to either:

(a) Always register template tools (they'll just return errors if no templates loaded), OR
(b) Call `FOliveTemplateSystem::Get().Initialize()` before `RegisterAllTools()`.

**Decision: Option (a).** Always register template tools. If no templates are loaded, `HandleBlueprintCreateFromTemplate` returns an error saying "No templates available." This is simpler and avoids startup ordering issues. The tools being visible in the tool list is itself useful for the AI to know the feature exists.

So the registration block becomes:
```cpp
    // Template tools (always registered; handlers check template availability)
    RegisterTemplateTools();
```

**Include needed in OliveBlueprintToolHandlers.cpp:** Add `#include "Template/OliveTemplateSystem.h"` at the top.

### 3c: Handler Stubs

Add stub implementations for the 3 handlers:

**HandleBlueprintListTemplates** -- Fully implementable now (just queries FOliveTemplateSystem):
```cpp
FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintListTemplates(const TSharedPtr<FJsonObject>& Params)
{
    FString TypeFilter;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("type"), TypeFilter);
    }

    const auto& AllTemplates = FOliveTemplateSystem::Get().GetAllTemplates();

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> TemplatesArray;

    for (const auto& Pair : AllTemplates)
    {
        const FOliveTemplateInfo& Info = Pair.Value;

        if (!TypeFilter.IsEmpty() && Info.TemplateType != TypeFilter)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("template_id"), Info.TemplateId);
        Entry->SetStringField(TEXT("type"), Info.TemplateType);
        Entry->SetStringField(TEXT("display_name"), Info.DisplayName);
        Entry->SetStringField(TEXT("description"), Info.CatalogDescription);
        if (!Info.CatalogExamples.IsEmpty())
        {
            Entry->SetStringField(TEXT("examples"), Info.CatalogExamples);
        }
        TemplatesArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    ResultData->SetArrayField(TEXT("templates"), TemplatesArray);
    ResultData->SetNumberField(TEXT("count"), TemplatesArray.Num());

    return FOliveToolResult::Success(ResultData);
}
```

**HandleBlueprintGetTemplate** -- Fully implementable now (delegates to `GetTemplateContent`):
```cpp
FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintGetTemplate(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FOliveToolResult::Error(TEXT("VALIDATION_INVALID_PARAMS"),
            TEXT("Parameters object is null"),
            TEXT("Provide 'template_id'"));
    }

    FString TemplateId;
    if (!Params->TryGetStringField(TEXT("template_id"), TemplateId) || TemplateId.IsEmpty())
    {
        return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
            TEXT("Required parameter 'template_id' is missing"),
            TEXT("Provide template_id"));
    }

    FString PatternName;
    Params->TryGetStringField(TEXT("pattern"), PatternName);

    FString Content = FOliveTemplateSystem::Get().GetTemplateContent(TemplateId, PatternName);
    if (Content.IsEmpty())
    {
        return FOliveToolResult::Error(TEXT("TEMPLATE_NOT_FOUND"),
            FString::Printf(TEXT("Template '%s' not found"), *TemplateId),
            TEXT("Use blueprint.list_templates to see available templates"));
    }

    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("template_id"), TemplateId);
    ResultData->SetStringField(TEXT("content"), Content);

    return FOliveToolResult::Success(ResultData);
}
```

**HandleBlueprintCreateFromTemplate** -- Stub that delegates to `FOliveTemplateSystem::Get().ApplyTemplate()`:
```cpp
FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintCreateFromTemplate(const TSharedPtr<FJsonObject>& Params)
{
    // Validation
    if (!Params.IsValid()) { /* standard error */ }

    FString TemplateId; /* extract, required */
    FString AssetPath;  /* extract, required */
    FString PresetName; /* extract, optional */

    // Extract parameters object
    TMap<FString, FString> UserParams;
    const TSharedPtr<FJsonObject>* ParamsObj;
    if (Params->TryGetObjectField(TEXT("parameters"), ParamsObj))
    {
        for (const auto& Pair : (*ParamsObj)->Values)
        {
            FString Value;
            if (Pair.Value->TryGetString(Value))
            {
                UserParams.Add(Pair.Key, Value);
            }
        }
    }

    return FOliveTemplateSystem::Get().ApplyTemplate(TemplateId, UserParams, PresetName, AssetPath);
}
```

### Verification

Build should succeed. Launch editor. In MCP tools/list, the 3 template tools should appear. `blueprint.list_templates` should return empty array (or 2 entries after Task 2 templates are present).

---

## Task 4: Template Executor (ApplyTemplate + GetTemplateContent)

**Priority:** Fourth -- the core execution logic.
**Depends on:** Tasks 1, 2, 3.
**File to modify:** `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp`

### 4a: GetTemplateContent Implementation

This is the simpler one. It formats a template's content as a human-readable string for the AI.

**For factory templates:** Output the parameter schema (name, type, default, description for each param), the presets, and for each function, a condensed plan outline (step_id + op for each step).

**For reference templates:** If `PatternName` is empty, output all patterns. If `PatternName` is provided, output just that pattern. Each pattern should show its description, notes (if any), and the full steps array as formatted JSON.

### 4b: ApplyTemplate Implementation

This is the critical method. It orchestrates existing tool handlers to create a complete Blueprint from a factory template.

**Execution flow:**

```
1. Validate template exists and is "factory" type
2. Merge parameters (defaults -> preset -> user overrides)
3. Get the "blueprint" section from FullJson
4. Serialize it back to string, substitute parameters
5. Re-parse the substituted JSON
6. Evaluate conditionals in default values
7. Create the Blueprint asset
8. Add each variable
9. Add each event dispatcher
10. For each function: create function + apply plan
11. Compile
12. Return result
```

**CRITICAL DESIGN DECISION: Direct Writer Calls vs Tool Calls**

The ApplyTemplate method should call the `FOliveBlueprintWriter` singleton methods directly (like the tool handlers do inside their executor lambdas), NOT call tool handlers or the tool registry. Reasons:
- Avoids going through the write pipeline 5-10 times per template (one for each variable, dispatcher, function)
- All operations happen in a single logical unit
- The outer `ApplyTemplate` call itself goes through the write pipeline once

**Implementation approach:**

ApplyTemplate itself is wrapped in a write pipeline call. The tool handler (`HandleBlueprintCreateFromTemplate`) builds an `FOliveWriteRequest` with `OperationCategory = "template_apply"` and calls `ExecuteWithOptionalConfirmation`. Inside the executor lambda, the template system does all the work.

Wait -- this is better handled differently. Let me reconsider.

**Revised approach:** `ApplyTemplate` is called FROM the tool handler, but it should NOT go through the write pipeline internally. Instead, the TOOL HANDLER wraps the whole thing in a single pipeline call. This is the established pattern: the handler builds the request and executor, the executor lambda calls writer methods directly.

So `ApplyTemplate` becomes the body of the executor lambda. It receives the `UObject* TargetAsset` (which will be nullptr for create operations, same as `HandleBlueprintCreate`).

**Actually, the cleanest approach:** `ApplyTemplate` returns `FOliveToolResult` directly, and internally it:
1. Creates the Blueprint via `FOliveBlueprintWriter::Get().CreateBlueprint()`
2. Loads it back
3. Wraps subsequent operations in a single `FScopedTransaction`
4. Adds variables, dispatchers, functions
5. For each function with a plan: calls the plan resolver + executor directly
6. Compiles
7. Returns structured result

This avoids the write pipeline for internal operations. The outer `HandleBlueprintCreateFromTemplate` still validates params and returns `FOliveToolResult`.

**But wait -- `CreateBlueprint` creates its own transaction internally.** Looking at how `HandleBlueprintCreate` works (line 1614), it goes through the write pipeline which opens a transaction. We need to call `CreateBlueprint` directly, then do everything else in a separate transaction.

**Final approach (simplest, most reliable):**

```
1. FOliveBlueprintWriter::Get().CreateBlueprint(AssetPath, ParentClass, BPType)
   - This handles asset creation with its own transaction
   - If this fails, return error immediately

2. Load the created Blueprint
3. Open a single FScopedTransaction for all modifications
4. Add variables via FOliveBlueprintWriter::Get().AddVariable(AssetPath, VarIR)
   - These go through the writer which does its own transaction management
   - Wait: AddVariable also opens its own FScopedTransaction internally

Hmm, nested transactions might be fine (UE supports them). But cleaner:
   - Use FOliveBatchExecutionScope to suppress inner transactions
   - Then do everything in one outer transaction
```

**FINAL FINAL approach (battle-tested by the plan executor):**

Use the same pattern as `HandleBlueprintApplyPlanJson` v2 path (line 6533). The tool handler creates a write request, the executor lambda does:

```cpp
Executor.BindLambda([...](const FOliveWriteRequest& InRequest, UObject* TargetAsset) -> FOliveWriteResult
{
    // The pipeline opened a transaction for us.
    // TargetAsset is nullptr for create operations.

    // Step 1: Create Blueprint (inside transaction)
    FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
    FOliveBlueprintWriteResult CreateResult = Writer.CreateBlueprint(AssetPath, ParentClass, BPType);
    if (!CreateResult.bSuccess) { return error; }

    // Step 2: Load the Blueprint we just created
    UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!BP) { return error; }

    BP->Modify();

    // Step 3: Add variables
    for (each variable in template)
    {
        FOliveIRVariable VarIR;
        // ... populate from substituted template JSON ...
        FOliveBlueprintWriteResult VarResult = Writer.AddVariable(AssetPath, VarIR);
        if (!VarResult.bSuccess) { accumulate warning, continue; }
    }

    // Step 4: Add event dispatchers
    for (each dispatcher in template)
    {
        Writer.AddEventDispatcher(AssetPath, Name, Params);
    }

    // Step 5: For each function with a plan
    for (each function in template)
    {
        // 5a: Create function graph
        Writer.AddFunction(AssetPath, Signature);

        // 5b: If function has a plan, execute it
        if (function has "plan" field)
        {
            // Parse plan JSON
            FOliveIRBlueprintPlan Plan = FOliveIRBlueprintPlan::FromJson(PlanJson);

            // Resolve
            FOlivePlanResolveResult ResolveResult =
                FOliveBlueprintPlanResolver::Resolve(Plan, BP);

            // Find the function graph
            UEdGraph* FuncGraph = FindGraphByName(BP, FunctionName);

            // Execute with FOlivePlanExecutor
            FOliveBatchExecutionScope BatchScope;
            FOlivePlanExecutor PlanExecutor;
            FOliveIRBlueprintPlanResult PlanResult = PlanExecutor.Execute(
                Plan, ResolveResult.ResolvedSteps, BP, FuncGraph, AssetPath, FunctionName);

            // Accumulate warnings/errors from plan result
        }
    }

    // Step 6: Build result
    TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
    ResultData->SetStringField(TEXT("asset_path"), AssetPath);
    ResultData->SetStringField(TEXT("template_id"), TemplateId);
    // ... applied_params, created variables, functions, etc.

    return FOliveWriteResult::Success(ResultData);
});
```

**KEY: The `CreateBlueprint` call is INSIDE the executor lambda.** This is the same pattern as `HandleBlueprintCreate`. The write pipeline's transaction wraps the entire operation.

**ISSUE: `CreateBlueprint` creates a package and saves it via `FAssetToolsModule`. It may or may not open its own transaction.** Let me check...

Looking at the existing `HandleBlueprintCreate` handler (line 1614), the `CreateBlueprint` call is inside the executor lambda which is inside the write pipeline transaction. This works because `CreateBlueprint` is designed to work within an existing transaction. So the same pattern applies here.

**Includes needed in OliveTemplateSystem.cpp:**
```cpp
#include "Template/OliveTemplateSystem.h"
#include "Writer/OliveBlueprintWriter.h"
#include "Reader/OliveBlueprintReader.h"
#include "Plan/OliveBlueprintPlanResolver.h"
#include "Plan/OlivePlanExecutor.h"
#include "Plan/OlivePlanValidator.h"
#include "Services/OliveBatchExecutionScope.h"
#include "IR/BlueprintPlanIR.h"
#include "IR/BlueprintIR.h"
#include "IR/CommonIR.h"
#include "IR/OliveIRSchema.h"
#include "Pipeline/OliveWritePipeline.h"
#include "Brain/OliveToolExecutionContext.h"
#include "OliveBlueprintTypes.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
```

### 4c: Variable Parsing from Template JSON

Template variables look like:
```json
{ "name": "CurrentHealth", "type": "Float", "default": "100.0", "category": "Health" }
```

This needs to be converted to `FOliveIRVariable`. The type field is a simple string ("Float", "Int", "Bool", etc.) which maps to `EOliveIRTypeCategory`. Use the same `ParseTypeFromParams` approach as the existing handlers, but the template uses a simplified format.

Write a helper `ParseTemplateVariable(TSharedPtr<FJsonObject> VarJson)` that converts the simplified template variable format to `FOliveIRVariable`:
- `name` -> `Variable.Name`
- `type` -> wrap in `{"category": type}` and call the same type parser
- `default` -> `Variable.DefaultValue`
- `category` -> `Variable.Category`

### 4d: Function Signature Parsing

Template functions look like:
```json
{
    "name": "ApplyHealthChange",
    "inputs": [{ "name": "Delta", "type": "Float" }],
    "outputs": [{ "name": "NewValue", "type": "Float" }],
    "plan": { ... }
}
```

Convert to `FOliveIRFunctionSignature`:
- `name` -> `Signature.Name`
- `inputs` -> array of `FOliveIRFunctionParam` (each with Name and Type)
- `outputs` -> array of `FOliveIRFunctionParam`

### 4e: Event Dispatcher Parsing

Template dispatchers look like:
```json
{
    "name": "OnHealthChanged",
    "params": [{ "name": "NewValue", "type": "Float" }, { "name": "Delta", "type": "Float" }]
}
```

Convert to the format expected by `Writer.AddEventDispatcher(AssetPath, Name, Params)` which takes `TArray<FOliveIRFunctionParam>`.

### Verification

After Task 4:
1. Launch editor with stat_component.json in `Content/Templates/factory/`
2. Via MCP or Claude Code, call:
```json
{
    "tool": "blueprint.create_from_template",
    "arguments": {
        "template_id": "stat_component",
        "asset_path": "/Game/Test/BP_TestHealth",
        "preset": "Health"
    }
}
```
3. Verify: Blueprint created at `/Game/Test/BP_TestHealth` with:
   - Parent class: ActorComponent
   - Variables: CurrentHealth (Float), MaxHealth (Float)
   - Event dispatchers: OnHealthChanged, OnDeath
   - Function: ApplyHealthChange with graph logic
   - Blueprint compiles without errors

4. Also test `blueprint.get_template` with `stat_component` and `component_patterns`.

---

## Task 5: Prompt/Catalog Injection

**Priority:** Fifth -- enables AI discovery without explicit tool calls.
**Depends on:** Task 1 (catalog block generation).

### 5a: Inject into Capability Knowledge Packs

**File to modify:** `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp`

The cleanest integration point is in `LoadPromptTemplates()` (around line 528). After building `ProfileCapabilityPackIds`, add the template catalog as a synthetic knowledge pack:

```cpp
// After line 533 (ProfileCapabilityPackIds setup):

// Inject template catalog as a capability knowledge pack.
// This ensures CLI agents (Claude Code, etc.) see available templates
// in their system prompt via BuildSharedSystemPreamble().
if (FOliveTemplateSystem::Get().HasTemplates())
{
    const FString& CatalogBlock = FOliveTemplateSystem::Get().GetCatalogBlock();
    if (!CatalogBlock.IsEmpty())
    {
        CapabilityKnowledgePacks.Add(TEXT("template_catalog"), CatalogBlock);

        // Add to Blueprint and Auto profiles
        TArray<FString>* AutoPacks = ProfileCapabilityPackIds.Find(TEXT("Auto"));
        if (AutoPacks) { AutoPacks->Add(TEXT("template_catalog")); }

        TArray<FString>* BPPacks = ProfileCapabilityPackIds.Find(TEXT("Blueprint"));
        if (BPPacks) { BPPacks->Add(TEXT("template_catalog")); }

        UE_LOG(LogOliveAI, Log, TEXT("Injected template catalog into capability knowledge (%d chars)"),
            CatalogBlock.Len());
    }
}
```

**Include needed:** Add `#include "Template/OliveTemplateSystem.h"` at the top of `OlivePromptAssembler.cpp`.

**STARTUP ORDERING CHECK:** `LoadPromptTemplates()` is called from `Initialize()`, which is called at step 13 in the startup sequence (`OliveAIEditorModule.cpp` line 246). By that time, all tool handlers are registered. But `FOliveTemplateSystem::Get().Initialize()` hasn't been called yet!

This means we need `FOliveTemplateSystem::Get().Initialize()` to be called BEFORE `FOlivePromptAssembler::Get().Initialize()`. See Task 6 for the startup ordering.

**Alternative approach (simpler, no ordering dependency):** Instead of injecting during LoadPromptTemplates, modify `GetCapabilityKnowledge()` to append the catalog block dynamically:

```cpp
FString FOlivePromptAssembler::GetCapabilityKnowledge(const FString& ProfileName) const
{
    // ... existing code to build Combined from packs ...

    // Append template catalog for Blueprint-relevant profiles
    FString NormalizedProfile = FOliveFocusProfileManager::Get().NormalizeProfileName(ProfileName);
    if (NormalizedProfile == TEXT("Auto") || NormalizedProfile == TEXT("Blueprint"))
    {
        if (FOliveTemplateSystem::Get().HasTemplates())
        {
            const FString& Catalog = FOliveTemplateSystem::Get().GetCatalogBlock();
            if (!Catalog.IsEmpty())
            {
                if (!Combined.IsEmpty()) { Combined += TEXT("\n\n"); }
                Combined += Catalog;
            }
        }
    }

    return Combined;
}
```

**Decision: Use the dynamic approach.** It avoids all startup ordering issues. `GetCapabilityKnowledge` is called at prompt assembly time, which is always after initialization is complete.

**Note (post-P0):** `OlivePromptAssembler.cpp` was already modified during P0 Task 3 to add
`node_routing` to the Auto and Blueprint profile capability pack arrays. The dynamic approach
here modifies `GetCapabilityKnowledge()` (a different function) so there is no conflict, but
the coder should be aware the file has recent changes and line numbers have shifted.

### Verification

After Task 5:
1. Launch editor, verify templates are loaded
2. Send a message via the built-in chat or via MCP using a provider that goes through prompt assembly
3. Check that the system prompt includes `[AVAILABLE BLUEPRINT TEMPLATES]` block
4. For Claude Code CLI path: verify `BuildSharedSystemPreamble` includes the catalog (since it calls `GetCapabilityKnowledge`)

---

## Task 6: Startup Integration

**Priority:** Sixth (but can be done alongside Task 1).
**Depends on:** Task 1.

### 6a: Add Initialize call to startup sequence

**File to modify:** `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`

Add `FOliveTemplateSystem::Get().Initialize()` in `OnPostEngineInit()`. The template system needs:
- File system access (always available)
- JSON parsing (always available)
- Nothing from the node catalog, plan resolver, or other Blueprint subsystems (those are only needed by ApplyTemplate at runtime)

So it can go very early. But for cleanliness, put it after Blueprint tools are registered and before the prompt assembler:

```cpp
// After line 202 (RegisterAllTools) and before line 246 (PromptAssembler Initialize):

// Initialize template system (scans Content/Templates/)
FOliveTemplateSystem::Get().Initialize();
```

Specifically, insert between line 234 (CrossSystem tools) and line 240 (FocusProfileManager):

```cpp
    // ...existing code...
    FOliveCrossSystemToolHandlers::Get().RegisterAllTools();
    UE_LOG(LogOliveAI, Log, TEXT("Cross-System tools registered"));

    // Initialize template system (scans Content/Templates/ for JSON templates)
    FOliveTemplateSystem::Get().Initialize();

    // Initialize focus profiles after tool registration...
    FOliveFocusProfileManager::Get().Initialize();
    // ...
```

**Include needed:** Add `#include "Template/OliveTemplateSystem.h"` at the top of `OliveAIEditorModule.cpp`.

### 6b: Add Shutdown call

In `ShutdownModule()`, add before Blueprint tools unregister:

```cpp
    // Shutdown template system
    FOliveTemplateSystem::Get().Shutdown();
```

### 6c: Template tool registration timing

Given the decision in Task 3 to always register template tools (even if no templates exist), the `RegisterTemplateTools()` call happens inside `RegisterAllTools()` which is called at step 6. This is fine -- the tools are registered, and if someone calls them before templates are loaded, the handlers check `HasTemplates()` and return appropriate errors.

But actually, we decided in Task 3 to always register template tools unconditionally. The handlers themselves check `FindTemplate()` and return "not found" errors. So the registration happens at step 6, templates load later (between steps 8 and 9), and by the time any MCP or chat request arrives, templates are loaded. No issue.

### 6d: Error Classification Registration (Post-P0 Addendum)

**File to modify:** `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

P0 Task 2 added `ClassifyErrorCode()` which maps error code strings to `EOliveErrorCategory` (A=FixableMistake, B=UnsupportedFeature, C=Ambiguous). Template error codes need to be added to this map.

In `ClassifyErrorCode()`, find the existing error code classification block and add:

```cpp
// Template errors
// Category A (FixableMistake) — AI can fix by adjusting params/path:
//   TEMPLATE_INVALID_PARAM, TEMPLATE_APPLY_VARIABLE_FAILED,
//   TEMPLATE_APPLY_DISPATCHER_FAILED, TEMPLATE_APPLY_PLAN_FAILED
// Category B (UnsupportedFeature) — don't retry:
//   TEMPLATE_NOT_FOUND, TEMPLATE_NOT_FACTORY
// Category A:
//   TEMPLATE_APPLY_CREATE_FAILED (usually wrong path, fixable)
//   TEMPLATE_APPLY_FUNCTION_FAILED (usually plan issue, fixable)
```

Specifically add to the Category B set:
- `TEMPLATE_NOT_FOUND`
- `TEMPLATE_NOT_FACTORY`

And to Category A (default, or explicit):
- `TEMPLATE_APPLY_CREATE_FAILED`
- `TEMPLATE_APPLY_PLAN_FAILED`
- `TEMPLATE_APPLY_FUNCTION_FAILED`

The remaining template codes (`TEMPLATE_INVALID_PARAM`, `TEMPLATE_APPLY_VARIABLE_FAILED`, `TEMPLATE_APPLY_DISPATCHER_FAILED`) fall through to default Category A, which is correct.

### Verification

After Task 6:
1. Build and launch editor
2. Check log for:
   ```
   Template catalog rebuilt: 1 factory, 1 reference, N chars
   ```
3. The log line should appear AFTER "Cross-System tools registered" and BEFORE "Focus profiles initialized"

---

## Implementation Order Summary

```
Task 1: FOliveTemplateSystem.h + .cpp (core class, loading, catalog)
         |
         v
Task 2: Template JSON files (stat_component.json, component_patterns.json)
         |
         v
Task 6a-c: Startup integration (Initialize in OliveAIEditorModule.cpp)
         |   <-- BUILD AND VERIFY TEMPLATES LOAD -->
         v
Task 3: Schema definitions + tool registration + handler stubs
         |   <-- BUILD AND VERIFY TOOLS APPEAR IN MCP -->
         v
Task 4: ApplyTemplate + GetTemplateContent (executor logic)
         |   <-- BUILD AND TEST END-TO-END -->
         v
Task 5: Catalog injection into prompt assembler
  +
Task 6d: Error classification for template error codes
         |   <-- VERIFY CATALOG IN SYSTEM PROMPT -->
         v
DONE -- Full system operational
```

Tasks 1 and 6a-c can be done together. Task 2 is just JSON files. The real implementation work is Tasks 3 and 4. Task 5 and 6d are independent and can be parallelized.

**Note for coder:** All line number references in this document are approximate. Files were modified during P0/P1/P2 implementation. Always search by function/symbol name (e.g., `RegisterAllTools`, `GetCapabilityKnowledge`, `ClassifyErrorCode`) rather than relying on line numbers.

---

## Error Codes

| Code | When | Message Pattern |
|------|------|----------------|
| `TEMPLATE_NOT_FOUND` | FindTemplate returns null | "Template 'X' not found" |
| `TEMPLATE_NOT_FACTORY` | create_from_template on a reference template | "Template 'X' is a reference template, not factory" |
| `TEMPLATE_INVALID_PARAM` | User provides unknown parameter | "Unknown parameter 'X' for template 'Y'" (warning, not error) |
| `TEMPLATE_APPLY_CREATE_FAILED` | Blueprint creation step failed | "Failed to create Blueprint: ..." |
| `TEMPLATE_APPLY_VARIABLE_FAILED` | Variable addition failed | "Failed to add variable 'X': ..." (warning, continue) |
| `TEMPLATE_APPLY_DISPATCHER_FAILED` | Event dispatcher addition failed | "Failed to add dispatcher 'X': ..." (warning, continue) |
| `TEMPLATE_APPLY_FUNCTION_FAILED` | Function creation failed | "Failed to create function 'X': ..." |
| `TEMPLATE_APPLY_PLAN_FAILED` | Plan execution for a function failed | "Plan execution failed for function 'X': ..." |

Non-fatal errors (variable/dispatcher failures) are accumulated as warnings in the result. The template application continues even if a variable fails to add. Only Blueprint creation and function creation failures are fatal.

---

## Edge Cases

1. **Template with no functions** -- Perfectly valid. Blueprint is created with variables and dispatchers only.
2. **Template with conditional features** (e.g., `enable_regen`) -- The template JSON currently doesn't support conditional sections (only conditional defaults). The `enable_regen` parameter in stat_component is meant to be handled by the AI: if regen is desired, the AI calls create_from_template with `enable_regen: true`, and a future regen function could be added as a separate plan. For v1, the stat_component template includes the core Apply function only. Regen is a follow-up customization (Mode 2 usage).
3. **Duplicate asset path** -- `CreateBlueprint` returns an error if the asset already exists. The template system propagates this error.
4. **Malformed JSON in template file** -- Logged as warning, template skipped, other templates still load.
5. **Unsubstituted ${param}** -- Logged as warning, execution continues with the literal `${param}` string in place (which will likely cause plan resolution to fail with a helpful error).
6. **Empty Content/Templates/ folder** -- No crash. Catalog is empty. Tools return "no templates available."
7. **Template with functions but no plan** -- Function is created with the signature but no graph logic. This is valid (user can fill in logic later).

---

## Future Expansion (Not in This Phase)

- Additional factory templates (physics_projectile, trigger_overlap_actor, door_mechanism, spawner)
- Additional reference templates (event_patterns, spawning_patterns, behavior_tree_patterns)
- Recipe router integration for reference template patterns
- Template validation tool (verify all templates compile with default params)
- Template creation from existing Blueprints (reverse engineering)
- Conditional template sections (if enable_regen, include regen function)

---

## Appendix: Key Integration Points Reference

| What | Where | Line |
|------|-------|------|
| Plugin dir resolution | `OlivePromptAssembler.cpp` | 483 |
| Recipe file loading pattern | `OliveCrossSystemToolHandlers.cpp` | 1000-1159 |
| Tool registration pattern | `OliveBlueprintToolHandlers.cpp` | 5720-5747 |
| Schema helper functions | `OliveBlueprintSchemas.cpp` | 1-107 |
| Handler + pipeline pattern | `OliveBlueprintToolHandlers.cpp` | 1487-1645 |
| v2 plan executor usage | `OliveBlueprintToolHandlers.cpp` | 6520-6570 |
| AddVariable writer call | `OliveBlueprintToolHandlers.cpp` | 2196-2199 |
| AddEventDispatcher writer call | `OliveBlueprintToolHandlers.cpp` | 3375-3378 |
| AddFunction writer call | `OliveBlueprintToolHandlers.cpp` | 2973+ (search for it) |
| Capability knowledge injection | `OlivePromptAssembler.cpp` | 278-308 |
| Startup sequence | `OliveAIEditorModule.cpp` | 185-267 |
| ExecuteWithOptionalConfirmation | `OliveBlueprintToolHandlers.cpp` | 247-264 |
| ParseVariableFromParams | `OliveBlueprintToolHandlers.cpp` | ~5680 |
| ParseTypeFromParams | `OliveBlueprintToolHandlers.cpp` | (search, earlier in file) |
| FOliveIRBlueprintPlan::FromJson | `BlueprintPlanIR.h` (runtime module) | -- |
| FOliveBlueprintPlanResolver::Resolve | `OliveBlueprintPlanResolver.h` | -- |
| FOlivePlanExecutor::Execute | `OlivePlanExecutor.h` | -- |
| FOliveBatchExecutionScope | `OliveBatchExecutionScope.h` | -- |
| FindGraphByName (anonymous) | `OliveBlueprintToolHandlers.cpp` | 5764-5791 |
| FindOrCreateFunctionGraph | `OliveBlueprintToolHandlers.cpp` | ~5793 |
