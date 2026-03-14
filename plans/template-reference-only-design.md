# Template System Migration: Reference-Only

**Author:** Architect
**Date:** 2026-03-13
**Status:** Awaiting Approval
**Scope:** Remove all template copy/clone tools; all templates become reference material

---

## 1. Motivation

The template system currently has three template types and two execution paths:

- **Factory templates** can be instantiated via `blueprint.create` with `template_id`, which calls `ApplyTemplate()` -- a 760-line function that creates a Blueprint, adds components/variables/dispatchers, substitutes `${param}` tokens, executes plan_json for each function, and compiles.
- **Library templates** can be cloned via `blueprint.create_from_library`, which calls `FOliveLibraryCloner::Clone()` -- a 3200-line class with a 6-phase per-graph pipeline (classify, create, wire exec, wire data, set defaults, auto-layout) plus type resolution, demotion, and remap logic.

Both paths duplicate capabilities the AI already has through its standard tools:
- `blueprint.create` (empty) + `add_component` + `add_variable` + `add_dispatcher` + `apply_plan_json` covers everything `ApplyTemplate()` does.
- The AI reads library templates as reference and builds its own logic. The clone path was a shortcut that frequently produced broken Blueprints due to unresolvable dependencies, requiring remap parameters the AI often got wrong.

**The decision:** All templates become reference-only. The AI reads them, understands the architecture, and builds Blueprints itself. This removes ~4000 lines of execution code and two tools, while keeping the entire discovery and reference-reading infrastructure intact.

**Competitive context:** NeoStack and Aura have no template system at all. Our reference template approach is a differentiator -- the AI gets architectural knowledge that competitors' models need to just "know."

---

## 2. What Gets Removed

### 2.1 Tools

| Tool | Handler | Action |
|------|---------|--------|
| `blueprint.create_from_library` | `HandleBlueprintCreateFromLibrary` | DELETE registration + handler |
| `blueprint.create` `template_id` path | `HandleBlueprintCreateFromTemplate` | DELETE delegation code + handler |

The `blueprint.create` tool itself stays -- it just loses the `template_id`/`template_params`/`preset` parameters.

### 2.2 Classes and Methods to Delete

**Entire file deletions (2 files, ~3700 lines):**

| File | Lines | What |
|------|-------|------|
| `Blueprint/Public/Template/OliveLibraryCloner.h` | 532 | Header for FOliveLibraryCloner + enums + result structs |
| `Blueprint/Private/Template/OliveLibraryCloner.cpp` | 3204 | Full cloner implementation |

**Methods to delete from FOliveTemplateSystem (in OliveTemplateSystem.h + .cpp):**

| Method | Lines (approx) | Purpose |
|--------|----------------|---------|
| `ApplyTemplate()` | ~760 | Factory template execution |
| `MergeParameters()` | ~80 | Param default + preset + user merge |
| `SubstituteParameters()` | ~30 | `${token}` replacement |
| `EvaluateConditionals()` | ~100 | Bool ternary evaluation |

**Methods to delete from FOliveBlueprintToolHandlers (in .h and .cpp):**

| Method | Lines (approx) | Purpose |
|--------|----------------|---------|
| `HandleBlueprintCreateFromTemplate()` | ~35 | Factory template delegation |
| `HandleBlueprintCreateFromLibrary()` | ~130 | Library clone handler |

### 2.3 Schemas to Delete

In `OliveBlueprintSchemas.cpp`:
- `BlueprintCreateFromLibrary()` -- entire function (~40 lines)
- The `template_id`, `template_params`, `preset` properties from `BlueprintCreate()` schema

### 2.4 Tool Alias to Remove

In `OliveToolRegistry.cpp` (line ~406):
```cpp
Map.Add(TEXT("blueprint.create_from_template"), {
    TEXT("blueprint.create"),
    nullptr
});
```

### 2.5 Include to Remove

In `OliveBlueprintToolHandlers.cpp` (line 44):
```cpp
#include "Template/OliveLibraryCloner.h"
```

---

## 3. What Gets Kept (Unchanged)

### 3.1 Discovery and Reference Tools

| Tool | Purpose | Changes |
|------|---------|---------|
| `blueprint.list_templates` | Search templates by query | NONE |
| `blueprint.get_template` | Read template content, optionally a specific function | NONE |

Both handlers (`HandleBlueprintListTemplates`, `HandleBlueprintGetTemplate`) remain as-is. Their code paths for reading factory, reference, and library templates are purely read-only.

### 3.2 Template Data Files

All template JSON files stay on disk:
- `Content/Templates/factory/*.json` (5 files: gun, projectile, stat_component, interactable_door, interactable_gate)
- `Content/Templates/reference/*.json`
- `Content/Templates/library/**/*.json`

Factory templates still contain `${param}` tokens in their JSON. This is fine for reference reading -- when the AI reads a factory template via `get_template`, it sees the raw plan_json with `${param}` tokens. The AI understands these as parameterizable values and substitutes its own concrete values when building the Blueprint. The `GetTemplateContent()` method for factory templates already renders a useful human-readable format showing parameters, presets, and function outlines.

### 3.3 Template System Infrastructure

| Component | Keeps |
|-----------|-------|
| `FOliveTemplateSystem` | Singleton, `Initialize()`, `Shutdown()`, `Reload()`, `FindTemplate()`, `GetAllTemplates()`, `GetTemplatesByType()`, `SearchTemplates()`, `GetCatalogBlock()`, `GetTemplateContent()` |
| `FOliveLibraryIndex` | All of it -- `Initialize()`, `Search()`, `FindTemplate()`, `LoadFullJson()`, `GetFunctionContent()`, `GetTemplateOverview()`, `ResolveInheritanceChain()`, `BuildCatalog()`, `Tokenize()` |
| `FOliveTemplateInfo` | Struct stays |
| `FOliveLibraryTemplateInfo` | Struct stays |
| `FOliveLibraryFunctionSummary` | Struct stays |

### 3.4 Extraction Pipeline

All Python tools stay:
- `tools/extract_blueprints.py`
- `tools/resolve_inheritance.py`
- `tools/prepare_library.py`
- `tools/auto_tagger.py`

---

## 4. Changes to `blueprint.create`

### 4.1 Schema Change (OliveBlueprintSchemas.cpp)

Remove from `BlueprintCreate()`:
- `template_id` property
- `template_params` property
- `preset` property
- All template references in the schema description

The `parent_class` property becomes required-with-smart-defaults (currently it says "Not required when template_id is set"). For WidgetBlueprint and AnimationBlueprint types, it auto-defaults to UserWidget/AnimInstance respectively — the schema description and any validation should reflect this rather than saying "unconditionally required."

Before:
```
"Create a new Blueprint asset. When template_id is provided, creates from a factory template..."
```

After:
```
"Create a new empty Blueprint asset with the specified parent_class."
```

### 4.2 Handler Change (OliveBlueprintToolHandlers.cpp)

In `HandleBlueprintCreate()`:
- Remove the `template_id` check block (lines ~2132-2146) that delegates to `HandleBlueprintCreateFromTemplate`
- Remove the `bCommunitySearchHintShownCreate` template branch
- Simplify the `parent_class` missing error message (remove "or provide template_id" suggestion)

### 4.3 Declaration Change (OliveBlueprintToolHandlers.h)

Remove method declarations:
```cpp
FOliveToolResult HandleBlueprintCreateFromTemplate(const FString& TemplateId, const FString& AssetPath,
    const TSharedPtr<FJsonObject>& Params);
FOliveToolResult HandleBlueprintCreateFromLibrary(const TSharedPtr<FJsonObject>& Params);
```

---

## 5. Changes to FOliveTemplateSystem

### 5.1 Header (OliveTemplateSystem.h)

Remove from the public interface:
```cpp
FOliveToolResult ApplyTemplate(
    const FString& TemplateId,
    const TMap<FString, FString>& UserParams,
    const FString& PresetName,
    const FString& AssetPath);
```

Remove from private:
```cpp
TMap<FString, FString> MergeParameters(...) const;
FString SubstituteParameters(...) const;
FString EvaluateConditionals(...) const;
```

Everything else stays. `GetTemplateContent()` stays because it is the read path for `get_template`.

### 5.2 Source (OliveTemplateSystem.cpp)

Delete the following sections:
- `MergeParameters()` (~lines 555-640)
- `SubstituteParameters()` (~lines 396-430)
- `EvaluateConditionals()` (~lines 429-555)
- `ApplyTemplate()` (~lines 1293-2100)

Remove includes that are only needed by the execution path:
- `"Writer/OliveBlueprintWriter.h"` -- check if `GetTemplateContent` uses it (it does NOT, only ApplyTemplate uses it)
- `"Writer/OliveComponentWriter.h"` -- only ApplyTemplate
- `"Plan/OliveBlueprintPlanResolver.h"` -- only ApplyTemplate
- `"Plan/OlivePlanExecutor.h"` -- only ApplyTemplate
- `"Compile/OliveCompileManager.h"` -- only ApplyTemplate
- `"OliveBlueprintTypes.h"` -- only ApplyTemplate (for `ParseBlueprintType`)
- `"IR/BlueprintPlanIR.h"` -- only ApplyTemplate
- `"IR/BlueprintIR.h"` -- only ApplyTemplate
- `"IR/CommonIR.h"` -- only ApplyTemplate
- `"IR/OliveIRTypes.h"` -- only ApplyTemplate
- `"Kismet2/BlueprintEditorUtils.h"` -- only ApplyTemplate
- `"Kismet2/KismetEditorUtilities.h"` -- only ApplyTemplate

After removal, the only includes needed are those supporting `Initialize()`, `ScanDirectory()`, `LoadTemplateFile()`, `RebuildCatalog()`, `GetTemplateContent()`, and `SearchTemplates()`. Verify each before removing.

### 5.3 Catalog Text Update (RebuildCatalog in OliveTemplateSystem.cpp)

Current catalog text references "Factory templates support blueprint.create with template_id for quick scaffolding." This must change.

Replace the intro block:
```
[AVAILABLE BLUEPRINT TEMPLATES]
Search library templates with blueprint.list_templates(query="...") for proven patterns from real projects.
Use blueprint.get_template(id, pattern="FuncName") to read a specific function's full node graph.
Factory templates support blueprint.create with template_id for quick scaffolding.
```

With:
```
[AVAILABLE BLUEPRINT TEMPLATES]
Templates are reference material. Study architecture, then build your own with plan_json / granular tools.
Search with blueprint.list_templates(query="..."). Read functions with blueprint.get_template(id, pattern="FuncName").
```

Update factory template catalog label:
```
"Factory templates (create complete Blueprints, or extract individual functions with get_template):"
```
To:
```
"Factory templates (architecture + plan_json patterns -- read with get_template):"
```

---

## 6. Changes to RegisterTemplateTools

In `FOliveBlueprintToolHandlers::RegisterTemplateTools()`:

- Remove the `blueprint.create_from_library` registration block (lines 9309-9319)
- Update the log message from `"get_template, list_templates, create_from_library"` to `"get_template, list_templates"`
- Update the `get_template` tool description to remove "full plan ready for apply_plan_json" language that implies copy semantics. New description:

```
"View a template's content. Without pattern: shows structure overview (components, variables, function signatures). "
"With pattern=FunctionName: returns the function's full graph or plan as reference for building your own."
```

- Update the `list_templates` tool description (minor -- current wording is fine, but the note in search results should change):

In `HandleBlueprintListTemplates`, update the result note from:
```
"Use blueprint.get_template(template_id) to view full content. For library templates, use pattern param to retrieve a specific function's node graph."
```
To:
```
"Use blueprint.get_template(template_id) to view structure. Use pattern param to read a specific function's graph as reference."
```

---

## 7. Changes to CLI Provider (OliveCLIProviderBase.cpp)

### 7.1 IsStructureCreatingTool (line ~145)

Remove `create_from_template` from the check:
```cpp
|| ToolName.Contains(TEXT("create_from_template"));
```

### 7.2 Agent Context / BuildSystemPrompt (line ~545-550)

Remove:
```
"When creating Blueprints, use `blueprint.create` (with optional template_id for templates)..."
```
Replace with:
```
"When creating Blueprints, use `blueprint.create` with parent_class..."
```

Remove:
```
"After creating from a template (blueprint.create with template_id), check the result for the list of created functions..."
```
This entire line is dead after the template execution path is removed.

### 7.3 BuildPrompt first-iteration routing (line ~2043)

Remove:
```
"If the task is creating NEW Blueprints, check if a template fits first (blueprint.create with template_id)."
```
Replace with:
```
"If the task is creating NEW Blueprints, search templates for reference patterns (blueprint.list_templates), then use blueprint.create with parent_class."
```

---

## 8. Knowledge File Updates

### 8.1 cli_blueprint.txt

**CREATE new Blueprint section (lines ~14-19):**

Current:
```
1. Reference patterns are available via blueprint.list_templates(query="relevant terms"). Study matching functions with blueprint.get_template(id, pattern="FuncName").
2. If a library template closely matches, clone it directly: blueprint.create_from_library(template_id, path, mode="portable")...
3. If a factory template fits, use blueprint.create with template_id for quick scaffolding.
4. From scratch: blueprint.create -> add_component/add_variable -> apply_plan_json
```

Replace with:
```
1. Reference patterns are available via blueprint.list_templates(query="relevant terms"). Study matching functions with blueprint.get_template(id, pattern="FuncName").
2. Create the Blueprint: blueprint.create with parent_class -> add_component/add_variable -> apply_plan_json
3. Use template patterns as reference to inform your plan_json structure, variable naming, and component layout.
```

**Templates & Pattern Sources section (lines ~84-107):**

Remove all "Clone directly" paragraphs and "blueprint.create with template_id" language. Remove the library clone mode descriptions. Keep the search/reference flow.

Replace the section with:
```
## Templates & Pattern Sources
Templates are reference material, not scripts. Study patterns, then build your own approach.
Often library templates are the best quality (if available), factory templates are still good,
and community blueprints have mixed variety. Compare several templates before building.
- Library templates: extracted from real shipped projects with full node graphs. Search with
  blueprint.list_templates(query="..."), read specific functions with
  blueprint.get_template(id, pattern="FuncName"). The catalog lists domain keywords per
  project so you know what's available. Adapt naming to fit your project.
- Factory templates: hand-crafted parameterized templates for common patterns. Read with
  get_template to study architecture, variable layout, and plan_json patterns.
- Community blueprints: olive.search_community_blueprints(query) for 150K+ community
  examples. Mixed quality -- compare multiple results before committing.
```

Remove the "Component architecture" paragraph about library template parent classes being ActorComponents -- this was clone-specific guidance. Replace with a simpler note:
```
- Component architecture: when a template's parent is ActorComponent or SceneComponent,
  the pattern IS a component. Create a component Blueprint and add it to an actor.
```

**Function verification section (lines ~112-115):**

Keep as-is. Template references are still the first verification source.

### 8.2 recipe_routing.txt

**Routing section (lines ~3-4):**

Current:
```
- NEW blueprint: FIRST search the project... THEN check templates and reference patterns via blueprint.list_templates(query="..."). If a factory template fits, use blueprint.create with template_id. Otherwise: create + components/variables + apply_plan_json
```

Replace with:
```
- NEW blueprint: FIRST search the project (project.search + project.get_relevant_context) for existing implementations. THEN check templates for reference patterns via blueprint.list_templates(query="..."). Study relevant functions with get_template, then: blueprint.create + components/variables + apply_plan_json
```

**Pattern Research Sources section (lines ~12-14):**

Remove "Clone directly" and "blueprint.create with template_id for quick creation" language. Update to reference-only framing:
```
- Library templates (blueprint.list_templates query="...") -- extracted from proven, shipped projects. High quality. Full node graphs with real implementation patterns. Study with get_template, adapt naming to fit your project.
- Factory templates (blueprint.list_templates type="factory") -- hand-crafted patterns. Read with get_template to study architecture and plan_json structure.
```

### 8.3 Worker_Blueprint.txt (line ~85)

Current:
```
- Clone directly: blueprint.create_from_library(template_id, path) creates a real Blueprint from a library template...
```

Remove this bullet entirely. Keep the preceding "Library templates (highest quality)" bullet about using them as reference.

---

## 9. File Change Summary

### Files to DELETE (2 files, ~3700 lines removed)

| File | Lines |
|------|-------|
| `Source/OliveAIEditor/Blueprint/Public/Template/OliveLibraryCloner.h` | 532 |
| `Source/OliveAIEditor/Blueprint/Private/Template/OliveLibraryCloner.cpp` | 3204 |

### Files to MODIFY (9 files)

| File | Nature of Change | Estimated Lines Removed |
|------|-----------------|------------------------|
| `Source/OliveAIEditor/Blueprint/Public/Template/OliveTemplateSystem.h` | Remove `ApplyTemplate`, `MergeParameters`, `SubstituteParameters`, `EvaluateConditionals` declarations | ~15 |
| `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp` | Delete 4 method bodies + dead includes, update `RebuildCatalog` text | ~980 |
| `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h` | Remove 2 handler declarations | ~5 |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Delete 2 handlers + template delegation in `HandleBlueprintCreate` + remove registration + remove include | ~185 |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` | Delete `BlueprintCreateFromLibrary` schema, strip template params from `BlueprintCreate` | ~55 |
| `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp` | Remove `create_from_template` alias + `template_id` normalization | ~5 |
| `Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` | Remove template references from prompts | ~8 |
| `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | Rewrite template sections to reference-only | ~30 net |
| `Content/SystemPrompts/Knowledge/recipe_routing.txt` | Remove clone/create-from-template language | ~5 |
| `Content/SystemPrompts/Worker_Blueprint.txt` | Remove clone bullet | ~2 |

**Total lines removed:** ~5000 (including the two deleted files)

---

## 10. Order of Operations

### Task 1: Delete the Cloner (Senior, ~15 min)

1. Delete `OliveLibraryCloner.h` and `OliveLibraryCloner.cpp`
2. Remove `#include "Template/OliveLibraryCloner.h"` from `OliveBlueprintToolHandlers.cpp`
3. Remove `HandleBlueprintCreateFromLibrary` handler + declaration
4. Remove `blueprint.create_from_library` registration from `RegisterTemplateTools()`
5. Remove `BlueprintCreateFromLibrary()` schema
6. Build -- confirm no compilation errors

### Task 2: Remove Factory Template Execution (Senior, ~20 min)

1. Remove `HandleBlueprintCreateFromTemplate` handler + declaration
2. Remove `template_id` delegation block from `HandleBlueprintCreate` (lines ~2132-2146)
3. Simplify `parent_class` missing error message
4. Strip `template_id`, `template_params`, `preset` from `BlueprintCreate()` schema
5. Remove `create_from_template` alias from `OliveToolRegistry.cpp`
6. Remove `template_id` normalization from `NormalizeBlueprintParams`
7. Build -- confirm no compilation errors

### Task 3: Clean Up FOliveTemplateSystem (Senior, ~15 min)

1. Delete `ApplyTemplate()`, `MergeParameters()`, `SubstituteParameters()`, `EvaluateConditionals()` from .cpp
2. Remove corresponding declarations from .h
3. Remove includes that were only used by the execution path (verify each)
4. Update `RebuildCatalog()` text
5. Build -- confirm no compilation errors

### Task 4: Update Tool Descriptions (Junior, ~10 min)

1. Update `get_template` tool description in `RegisterTemplateTools()`
2. Update result notes in `HandleBlueprintListTemplates`
3. Build -- confirm no compilation errors

### Task 5: Update CLI Provider (Junior, ~10 min)

1. Remove `create_from_template` from `IsStructureCreatingTool` check
2. Update agent context strings in `BuildSystemPrompt`/`BuildPrompt`
3. Build -- confirm no compilation errors

### Task 6: Update Knowledge Files (Junior, ~15 min)

1. Rewrite template sections in `cli_blueprint.txt`
2. Update `recipe_routing.txt`
3. Remove clone bullet from `Worker_Blueprint.txt`
4. No build needed -- text files only

---

## 11. Edge Cases and Concerns

### 11.1 Factory Templates with ${param} Tokens as Reference

When the AI reads `gun.json` via `get_template("gun")`, it sees:
```
Parameters:
  weapon_name (string, default=Gun): Name prefix for variables and events
  max_ammo (int, default=30): Maximum ammo capacity
  ...

Functions:
  Fire:
    s_get_can_fire: get_var
    s_get_ammo: get_var
    ...
```

When it reads `get_template("gun", pattern="Fire")`, it gets the full plan_json with `${weapon_name}` tokens still present. **This is actually good** -- the AI sees that "weapon_name" is parameterized, understands the pattern, and substitutes its own values when writing plan_json. No format changes needed.

The `usage_notes` field in gun.json (e.g., equipping advice, self-collision guidance) is currently not rendered by `GetTemplateContent()`. This is a pre-existing gap and not a regression from this migration. **Follow-up: surface `usage_notes` in `GetTemplateContent()` for factory templates.** Once factory templates are reference-only, usage_notes becomes more valuable — it's architectural context the AI wants when building from scratch rather than copying.

### 11.2 Old AI Models Calling Removed Tools

If an older model tries to call `blueprint.create_from_library` or `blueprint.create` with `template_id`:
- `create_from_library` will hit the `UnknownTool` error path in the tool registry -- standard error handling
- `create` with `template_id` will simply ignore the unknown parameter -- the Blueprint gets created normally without template logic, which is the desired behavior

### 11.3 MCP Client Compatibility

External MCP clients (Claude Code via bridge) discover tools via `tools/list`. After this change, `blueprint.create_from_library` simply disappears from the tool list. `blueprint.create` loses its template parameters. MCP clients discover tools fresh each session, so there is no stale cache concern.

---

## 12. What the Coder Needs to Know

1. **This is a pure deletion task.** No new files, no new classes, no new methods. Every change removes code or simplifies text.

2. **Build after each task.** Tasks 1-5 each independently produce a compilable state. Task 6 is text-only.

3. **The two file deletions (OliveLibraryCloner.h/.cpp) are the biggest win.** 3700 lines of complex graph-cloning code disappears. The only consumer was `HandleBlueprintCreateFromLibrary`.

4. **FOliveTemplateSystem.cpp loses ~980 lines** but the remaining ~2450 lines (scanning, indexing, catalog building, content reading) stay exactly as they are.

5. **Do NOT touch** `FOliveLibraryIndex`, `GetTemplateContent()`, `SearchTemplates()`, or any template data files. The entire reference-reading infrastructure is preserved.

6. **Verify includes before removing — grep, don't eyeball.** The list in Section 5.2 is based on analysis, but `OliveTemplateSystem.cpp` is ~3400 lines with ~2450 surviving. For each of the ~12 includes marked for removal, grep the surviving code for any type/function from that header. "Probably unused" and "compiles clean" are different things. The task breakdown already says "build after each task" which is the safety net, but catching include errors before building saves iteration time.

7. **The knowledge file changes (Task 6) are the most nuanced.** The coder should read the current text, understand the context, and rewrite to reference-only framing. Do not just find-and-replace -- the surrounding sentences need to read naturally.
