# Pack P2 — Prompt + Knowledge Pack Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Shrink system prompts 40-60%, remove mode language, demote templates from "primary" to "optional reference," delete six knowledge packs, and add a setting to disable template catalog injection.

**Architecture:** Rewrite 10 `Content/SystemPrompts/*.txt` files to shorter, less prescriptive versions. Delete all 6 `Content/SystemPrompts/Knowledge/*.txt` files. Preserve the critical WRONG/RIGHT/WHY anti-patterns block from `cli_blueprint.txt` in the rewritten `Worker_Blueprint.txt`. Add `bInjectTemplateCatalog` bool on `UOliveAISettings` (default `true`); gate the catalog call in `FOlivePromptAssembler` on it. Update catalog-block wording.

**Tech Stack:** Plain text prompt files, C++ settings class, prompt assembler.

**Related spec:** `docs/superpowers/specs/2026-04-18-plugin-makeover-design.md` §4.

**Blocker:** This plan **must start after P5 (tool consolidation) lands**, because the prompts reference tool names and P5 changes them.

---

## File Structure

**Rewrite (whole-file replacement):**
- `Content/SystemPrompts/BaseSystemPrompt.txt` — target ≤40% of current size.
- `Content/SystemPrompts/Base.txt` — target ≤40%.
- `Content/SystemPrompts/Worker_Blueprint.txt` — target ≤30%.
- `Content/SystemPrompts/Worker_BehaviorTree.txt` — target ≤60%.
- `Content/SystemPrompts/Worker_PCG.txt` — target ≤60%.
- `Content/SystemPrompts/Worker_Niagara.txt` — target ≤60%.
- `Content/SystemPrompts/Worker_Cpp.txt` — target ≤60%.
- `Content/SystemPrompts/Worker_Integration.txt` — target ≤60%.
- `Content/SystemPrompts/Orchestrator.txt` — target ≤60%.

**Minor edits only:**
- `Content/SystemPrompts/ToolCallFormat.txt` — remove any mode-related lines.

**Delete:**
- `Content/SystemPrompts/Knowledge/blueprint_authoring.txt`
- `Content/SystemPrompts/Knowledge/blueprint_design_patterns.txt`
- `Content/SystemPrompts/Knowledge/cli_blueprint.txt`
- `Content/SystemPrompts/Knowledge/events_vs_functions.txt`
- `Content/SystemPrompts/Knowledge/node_routing.txt`
- `Content/SystemPrompts/Knowledge/recipe_routing.txt`
- `Content/SystemPrompts/Knowledge/` directory itself (empty after deletions).

**Modify C++:**
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` — add `bInjectTemplateCatalog` UPROPERTY.
- `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h` / `.cpp` — gate template catalog injection on the new setting; remove knowledge-pack loading code.

---

## Universal rewrite rules (apply to every prompt file)

1. Delete every reference to Ask/Plan/Code mode, `/ask`, `/plan`, `ASK_MODE`, `PLAN_MODE`.
2. Delete "always check templates first," "library templates have highest quality," "curated production-quality reference" language. Templates become "one optional reference among plan_json and granular tools."
3. Delete duplication of tool schemas — never re-document a tool's params or behavior that the schema already carries.
4. Delete all "recipes" language (`olive.get_recipe` is removed).
5. Approach choice presented flat: "plan_json, granular tools, or `editor.run_python` — whichever fits."
6. Keep safety rails: "read before write," "compile after changes," "fix the first error first."
7. Keep NeoStack transparency notes: "resolver may auto-translate; check `resolver_notes` in results."

---

## Tasks

### Task 1: Take snapshots before rewrite

**Files:** none (archive step).

- [ ] **Step 1: Record current sizes for the success-criteria check**

Run:

```bash
wc -l "B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Content/SystemPrompts/"*.txt > /tmp/prompt-sizes-before.txt
cat /tmp/prompt-sizes-before.txt
```

Paste the output into `docs/superpowers/plans/p2-size-before.md` for the post-rewrite comparison.

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/plans/p2-size-before.md
git commit -m "P2: snapshot prompt sizes before rewrite"
```

---

### Task 2: Add `bInjectTemplateCatalog` setting

**Files:**
- Modify: `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`

- [ ] **Step 1: Add UPROPERTY in the "Blueprint Plan" category section**

Insert after the `bPlanJsonRequirePreviewForApply` property (around line 323):

```cpp
/** When true, the template catalog block is injected into the system prompt on every turn.
 *  Turn off to reduce prompt token usage if you do not use templates. */
UPROPERTY(Config, EditAnywhere, Category="Blueprint Plan",
    meta=(DisplayName="Inject Template Catalog in Prompt"))
bool bInjectTemplateCatalog = true;
```

- [ ] **Step 2: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add Source/OliveAIEditor/Public/Settings/OliveAISettings.h
git commit -m "P2: add bInjectTemplateCatalog setting"
```

---

### Task 3: Gate catalog injection in prompt assembler; remove knowledge-pack loading

**Files:**
- Modify: `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp`
- Modify: `Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h` (only if the header exposes a loader API that needs removing).

- [ ] **Step 1: Locate the catalog injection call**

Search in `OlivePromptAssembler.cpp` for `GetCatalogBlock` or `TemplateSystem`. Find the call site that appends the template catalog to the system prompt.

- [ ] **Step 2: Gate on the new setting**

Wrap the catalog block inclusion:

```cpp
const UOliveAISettings* Settings = GetDefault<UOliveAISettings>();
if (Settings && Settings->bInjectTemplateCatalog)
{
    const FString Catalog = FOliveTemplateSystem::Get().GetCatalogBlock();
    if (!Catalog.IsEmpty())
    {
        SystemPrompt += TEXT("\n\n") + Catalog;
    }
}
```

Replace the existing unconditional call with the above.

- [ ] **Step 3: Remove knowledge-pack loading**

Search for any of: `LoadKnowledgePack`, `blueprint_authoring`, `cli_blueprint`, `recipe_routing`, `events_vs_functions`, `node_routing`, `blueprint_design_patterns`, `Knowledge/`. Delete the loading loop/function entirely. If the header exposes a public loader API, delete its declaration too.

- [ ] **Step 4: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add Source/OliveAIEditor/Public/Chat/OlivePromptAssembler.h Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp
git commit -m "P2: gate template catalog injection on setting; remove knowledge-pack loader"
```

---

### Task 4: Delete knowledge pack files and directory

**Files:**
- Delete: all 6 `Content/SystemPrompts/Knowledge/*.txt` files.
- Delete: `Content/SystemPrompts/Knowledge/` directory.

- [ ] **Step 1: Preserve the WRONG/RIGHT/WHY anti-patterns block**

Open `Content/SystemPrompts/Knowledge/cli_blueprint.txt`. Find the "Common Mistakes" section. Copy its content (the WRONG/RIGHT/WHY block for `exec-into-custom-event`, `tilde-on-struct-returns`, `@-prefix-on-exec_after`) to a scratch file `docs/superpowers/plans/p2-preserved-antipatterns.md`. You will paste this into the rewritten `Worker_Blueprint.txt` in Task 6.

- [ ] **Step 2: Delete the files**

```bash
git rm "Content/SystemPrompts/Knowledge/blueprint_authoring.txt"
git rm "Content/SystemPrompts/Knowledge/blueprint_design_patterns.txt"
git rm "Content/SystemPrompts/Knowledge/cli_blueprint.txt"
git rm "Content/SystemPrompts/Knowledge/events_vs_functions.txt"
git rm "Content/SystemPrompts/Knowledge/node_routing.txt"
git rm "Content/SystemPrompts/Knowledge/recipe_routing.txt"
```

- [ ] **Step 3: Delete the directory if now empty**

```bash
rmdir "Content/SystemPrompts/Knowledge"
```

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "P2: delete six knowledge pack files and Knowledge/ directory"
```

---

### Task 5: Rewrite Base.txt and BaseSystemPrompt.txt

**Files:**
- Rewrite: `Content/SystemPrompts/Base.txt`
- Rewrite: `Content/SystemPrompts/BaseSystemPrompt.txt`

- [ ] **Step 1: Read both files and identify load-bearing content**

Identify sections that state: who the agent is, what Olive is, what tools generally exist, the tool-call format, and safety rails. Everything else is candidate for deletion.

- [ ] **Step 2: Rewrite Base.txt**

Target: ≤40% of current size. Structure:

```
# Olive AI Studio

You are an AI agent embedded in the Unreal Engine 5 editor. You help a developer build Blueprints, Behavior Trees, PCG graphs, Niagara systems, C++, Widgets, and level content by calling MCP tools.

## How to work

- Read before write: inspect the current state of an asset before modifying it.
- Compile after changes and check the result before moving on.
- If a tool returns an error with a code + message + suggestion, follow the suggestion first.
- The resolver may auto-translate your intent — check resolver_notes in tool results to see what happened.

## Building Blueprints

Three equal approaches — use whichever fits:
- **plan_json** (blueprint.apply_plan_json): batch declarative graph edits.
- **granular tools** (blueprint.add, blueprint.modify, blueprint.connect_pins, ...): any UK2Node subclass, one at a time.
- **editor.run_python**: for anything the other tools cannot express.

Templates exist as optional references (list_templates, get_template). Use them only if one matches your task — they are not required.
```

Keep it this short. Delete any residual mode/recipe/knowledge-pack guidance.

- [ ] **Step 3: Rewrite BaseSystemPrompt.txt**

Same target and structure. If `BaseSystemPrompt.txt` is a superset of `Base.txt`, consolidate so there is no duplication. If they serve different roles (check `FOlivePromptAssembler`), keep the role distinction but apply the same brevity rules.

- [ ] **Step 4: Verify size reduction**

```bash
wc -l "B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Content/SystemPrompts/Base.txt" "B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Content/SystemPrompts/BaseSystemPrompt.txt"
```

Compare against `p2-size-before.md`. Both must be ≤40% of their original line counts.

- [ ] **Step 5: Commit**

```bash
git add Content/SystemPrompts/Base.txt Content/SystemPrompts/BaseSystemPrompt.txt
git commit -m "P2: rewrite Base.txt and BaseSystemPrompt.txt (<=40% original size)"
```

---

### Task 6: Rewrite Worker_Blueprint.txt

**Files:**
- Rewrite: `Content/SystemPrompts/Worker_Blueprint.txt`

- [ ] **Step 1: Draft the new file**

Target: ≤30% of current size. Required sections:

1. **Purpose** (1 short paragraph): you are building or editing a Blueprint.
2. **Three approaches** (bullet list, no hierarchy): plan_json / granular / python.
3. **Read before write** (1 line).
4. **Anti-patterns** (from `p2-preserved-antipatterns.md`): the WRONG/RIGHT/WHY block preserved verbatim.
5. **Optional templates** (2 lines): `list_templates` searches, `get_template` reads, `create_from_template` builds. Use only if one matches.

No mode language. No "always check templates first." No recipes. No knowledge-pack references.

- [ ] **Step 2: Paste the preserved anti-patterns block**

Copy from `docs/superpowers/plans/p2-preserved-antipatterns.md` into the new file under the `## Anti-patterns` heading. Verbatim.

- [ ] **Step 3: Verify size reduction**

```bash
wc -l "B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Content/SystemPrompts/Worker_Blueprint.txt"
```

Must be ≤30% of the original.

- [ ] **Step 4: Commit**

```bash
git add Content/SystemPrompts/Worker_Blueprint.txt
git commit -m "P2: rewrite Worker_Blueprint.txt (<=30% original size, anti-patterns preserved)"
```

---

### Task 7: Trim remaining Worker prompts

**Files:**
- Modify: `Content/SystemPrompts/Worker_BehaviorTree.txt`
- Modify: `Content/SystemPrompts/Worker_PCG.txt`
- Modify: `Content/SystemPrompts/Worker_Niagara.txt`
- Modify: `Content/SystemPrompts/Worker_Cpp.txt`
- Modify: `Content/SystemPrompts/Worker_Integration.txt`
- Modify: `Content/SystemPrompts/Orchestrator.txt`

- [ ] **Step 1: Apply the universal rewrite rules to each file**

For each file in order:
- Delete mode language, recipe language, knowledge-pack references.
- Delete any section that restates information the tool schemas already carry.
- Keep the asset-type-specific guidance that is not in any schema (e.g., "BT composites evaluate children in order; prefer Selector for fallback, Sequence for pipeline").
- Target size: ≤60% of current.

- [ ] **Step 2: Verify sizes**

```bash
wc -l "B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Content/SystemPrompts/Worker_BehaviorTree.txt" "...Worker_PCG.txt" "...Worker_Niagara.txt" "...Worker_Cpp.txt" "...Worker_Integration.txt" "...Orchestrator.txt"
```

Each ≤60% of original.

- [ ] **Step 3: Commit**

```bash
git add Content/SystemPrompts/Worker_BehaviorTree.txt Content/SystemPrompts/Worker_PCG.txt Content/SystemPrompts/Worker_Niagara.txt Content/SystemPrompts/Worker_Cpp.txt Content/SystemPrompts/Worker_Integration.txt Content/SystemPrompts/Orchestrator.txt
git commit -m "P2: trim worker prompts (<=60% original size)"
```

---

### Task 8: Minor edit on ToolCallFormat.txt

**Files:**
- Modify: `Content/SystemPrompts/ToolCallFormat.txt`

- [ ] **Step 1: Remove mode-related lines only**

Open the file. Delete any line referencing Ask/Plan/Code mode, `ASK_MODE`, `PLAN_MODE`, or mode-dependent tool availability. Leave the rest of the tool-call format instructions untouched.

- [ ] **Step 2: Commit**

```bash
git add Content/SystemPrompts/ToolCallFormat.txt
git commit -m "P2: remove mode lines from ToolCallFormat.txt"
```

---

### Task 9: Rewrite the template catalog block language

**Files:**
- Modify: `Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp` (locate `GetCatalogBlock`).

- [ ] **Step 1: Find the catalog block header text**

In `OliveTemplateSystem.cpp`, find `GetCatalogBlock`. Look at the strings that lead the block (e.g., "Templates available...").

- [ ] **Step 2: Rewrite the lead-in**

Replace whatever pitch language exists with this neutral lead-in:

```cpp
FString Header = TEXT("## Templates (optional references)\n"
    "Search with blueprint.list_templates(query=\"...\"). Read with blueprint.get_template(id). "
    "Use blueprint.create_from_template only if one matches your task exactly. "
    "You are not required to use templates.\n\n");
```

Do not change the enumeration logic that follows — only the lead-in strings.

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Blueprint/Private/Template/OliveTemplateSystem.cpp
git commit -m "P2: rewrite template catalog lead-in to neutral language"
```

---

### Task 10: End-to-end smoke test

**Files:** none changed.

- [ ] **Step 1: Build and open editor**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

Open the UE Editor. Wait for PostEngineInit to complete.

- [ ] **Step 2: Open chat panel, send one test prompt**

Prompt: "Create a new Blueprint called BP_TestActor that prints 'hello' on BeginPlay."

Observe: the AI should complete the task successfully using plan_json or granular tools, without any mention of templates being required or any mode switching.

- [ ] **Step 3: Verify prompt assembly output size**

If `FOlivePromptAssembler` has a debug-dump path, compare current assembled prompt size against a pre-P2 captured sample. Target: ≥40% smaller. If no debug-dump path exists, skip this check — the `wc -l` checks in earlier tasks suffice.

- [ ] **Step 4: Toggle setting and re-test**

In Project Settings > Plugins > Olive AI Studio, set `Inject Template Catalog in Prompt` to `false`. Send the same prompt again. Confirm it still works.

- [ ] **Step 5: Final commit**

No code changes expected. If anything needed a fix during smoke test, commit it separately.

---

## Acceptance criteria

1. `ubt-build-5.5` green.
2. All `Content/SystemPrompts/Knowledge/*.txt` files deleted; directory removed.
3. Every rewritten file meets its size target (`Base` ≤40%, `Worker_Blueprint` ≤30%, others ≤60%).
4. No prompt file contains the strings: "Ask mode", "Plan mode", "ASK_MODE", "PLAN_MODE", "recipes", "always check templates first", "highest quality", "curated".
5. WRONG/RIGHT/WHY anti-patterns block preserved in `Worker_Blueprint.txt`.
6. `bInjectTemplateCatalog` setting toggles catalog injection on and off.
7. End-to-end smoke test: chat panel completes a Blueprint task without errors.

## Out of scope

- Actual tool surface consolidation (that's P5 — this plan runs after).
- Adding new prompts for `level.*` or `material.*` — those are each pack's own scope.
- Modifying the tool schemas themselves.
