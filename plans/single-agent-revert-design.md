# Single-Agent Revert Design

**Date:** 2026-03-09
**Goal:** Remove the multi-agent pipeline, restore single-agent flow with lightweight context gathering. Target: ~30s overhead instead of 60-180s.

---

## 1. The New Flow (Step by Step)

For non-continuation messages with write intent:

1. **@-mention injection** (existing, unchanged) -- `BuildAssetStateSummary()` reads referenced BPs, injects structure
2. **C++ keyword search** (existing, unchanged) -- `ExtractKeywordsFromMessage()` + `FOliveProjectIndex::SearchAssets()` injects "Existing Assets That May Be Relevant"
3. **Template discovery** (existing, restored) -- `FOliveUtilityModel::RunDiscoveryPass()` searches templates using LLM keyword expansion or basic fallback. ~5-15s.
4. **Decomposition directive** (restored + updated) -- structured prompt telling the agent to list planned assets, reference templates, and build each one
5. **Tool execution guardrail** (existing, unchanged) -- requires at least one tool call before final text
6. **Launch CLI agent** (existing, unchanged) -- full tool access, plans and builds autonomously

Total pre-launch overhead: ~10-30s (one utility model LLM call for keyword expansion + C++ searches). Down from 60-180s.

---

## 2. Design Answers

### Q1: Where does GenerateSearchQueries live?

It lives in `FOliveUtilityModel` (`Source/OliveAIEditor/Private/Services/OliveUtilityModel.cpp` line 632). It is NOT part of the pipeline. It was called BY the pipeline's Scout via `RunDiscoveryPass()`, but is a standalone static method. No extraction needed.

The pre-pipeline code called `FOliveUtilityModel::RunDiscoveryPass()` directly from `SendMessageAutonomous()`. We restore that exact pattern. The setting `bEnableTemplateDiscoveryPass` already exists and gates it.

### Q2: What happens to template discovery?

Restore the pre-pipeline `RunDiscoveryPass()` call in `SendMessageAutonomous()`. This searches library/factory/reference templates and produces a formatted discovery block. The agent can also call `blueprint.list_templates` on demand for more targeted search.

The Scout's `TemplateReferences` and `TemplateOverviews` are dropped -- they were pipeline-specific pre-computation that the agent can replicate by calling tools.

### Q3: What happens to Section 3.25 (Function Pin Reference)?

**Drop it.** The agent has `blueprint.describe_function` and `blueprint.describe_node_type` tools for on-demand function signature lookup. The Pin Reference was only valuable because the Build Plan mentioned functions by name without pin details -- without a Build Plan, there is no text to extract from.

### Q4: What about the Reviewer?

**Drop it entirely.** The agent self-corrects via compile results and error messages. The Reviewer added 15-60s of overhead and one extra correction pass. Without a Build Plan, there is nothing meaningful to compare against.

### Q5: Should the decomposition directive be updated?

Yes. The updated directive should:
- Keep the asset decomposition structure (list entities before building)
- Mention template discovery results (if any found above)
- Reference `blueprint.list_templates` and `blueprint.get_template` as on-demand research tools
- Reference `blueprint.describe_function` for function signature lookup
- Keep the "compile to 0 errors before next asset" rule
- Keep the `apply_plan_json` preference for graph logic

### Q6: Settings cleanup

**Remove entirely:**
- `bCustomizeAgentModels` and all per-agent Provider/Model properties (Router, Scout, Researcher, Architect, Reviewer -- 10 UPROPERTYs)
- `AgentTimeoutSeconds`
- `bEnablePostBuildReview`

**Keep:**
- `bEnableTemplateDiscoveryPass` (gates the restored discovery pass)
- `bEnableLLMKeywordExpansion` (gates LLM vs basic keyword extraction within `GenerateSearchQueries`)

### Q7: Can the pipeline fallback become the primary path?

Almost. The current fallback is:
```
## Build Guidance
Break the task into individual Blueprint assets. For each:
1. Create the asset with appropriate parent class
2. Add components, variables, and interfaces
3. Write graph logic with apply_plan_json
4. Compile to 0 errors before moving to the next asset
```

This is too sparse. The restored decomposition directive is richer (includes template references, research tool mentions, entity decomposition heuristic). Use the updated decomposition directive, not the fallback.

---

## 3. Exact Changes Per File

### 3.1 `OliveCLIProviderBase.h` (header)

**Remove:**
- `#include "Brain/OliveAgentConfig.h"`
- `FOliveAgentPipelineResult CachedPipelineResult;` member
- `bool bIsReviewerCorrectionPass = false;` member

### 3.2 `OliveCLIProviderBase.cpp` (implementation)

**Remove:**
- `#include "Brain/OliveAgentPipeline.h"`
- The `EmitStatus` lambda definition (lines ~582-591)
- The entire pipeline block (lines ~593-648): `FOliveAgentPipeline Pipeline; ... CachedPipelineResult = ...`
- The entire Reviewer block in `HandleResponseCompleteAutonomous()` (lines ~1221-1302): `const UOliveAISettings* ReviewSettings = ...` through `bIsReviewerCorrectionPass = false;`
- The `CachedPipelineResult = FOliveAgentPipelineResult();` reset (line ~647)

**Restore (in place of the pipeline block):**
```cpp
// Template discovery pass -- pre-search library/factory/community templates
// using utility model for smart keyword generation.
if (!bIsContinuation)
{
    const UOliveAISettings* DiscoverySettings = UOliveAISettings::Get();
    if (DiscoverySettings && DiscoverySettings->bEnableTemplateDiscoveryPass)
    {
        const FString& DiscoveryInput =
            (LastRunContext.bValid && !LastRunContext.OriginalMessage.IsEmpty())
            ? LastRunContext.OriginalMessage
            : UserMessage;
        FOliveDiscoveryResult Discovery = FOliveUtilityModel::RunDiscoveryPass(DiscoveryInput);
        FString DiscoveryBlock = FOliveUtilityModel::FormatDiscoveryForPrompt(Discovery);

        if (!DiscoveryBlock.IsEmpty())
        {
            EffectiveMessage += TEXT("\n\n");
            EffectiveMessage += DiscoveryBlock;

            UE_LOG(LogOliveCLIProvider, Log,
                TEXT("Discovery pass: %d results in %.1fs (LLM=%s, queries: %s)"),
                Discovery.Entries.Num(),
                Discovery.ElapsedSeconds,
                Discovery.bUsedLLM ? TEXT("yes") : TEXT("no"),
                *FString::Join(Discovery.SearchQueries, TEXT("; ")));
        }
    }
}
```

**Restore (in place of the pipeline's decomposition section):**
```cpp
// Structured decomposition directive.
if (!bIsContinuation && MessageImpliesMutation(UserMessage))
{
    EffectiveMessage += TEXT("\n\n## Required: Asset Decomposition\n\n");
    EffectiveMessage += TEXT("Before calling ANY tools, list every game entity that needs its own Blueprint:\n\n");
    EffectiveMessage += TEXT("ASSETS:\n");
    EffectiveMessage += TEXT("1. BP_Name -- Type -- purpose\n");
    EffectiveMessage += TEXT("2. ...\n");
    EffectiveMessage += TEXT("N. Modify @ExistingBP -- changes\n\n");
    EffectiveMessage += TEXT("\"Does this thing exist in the world with its own transform?\" -> separate Blueprint.\n");
    EffectiveMessage += TEXT("\"Is it a value on an existing actor?\" -> variable. \"Is it a capability?\" -> component.\n");
    EffectiveMessage += TEXT("Weapons, projectiles, doors, keys, vehicles = always separate actors.\n\n");
    EffectiveMessage += TEXT("After listing assets, research patterns:\n");
    EffectiveMessage += TEXT("- Check Reference Templates Found above if present\n");
    EffectiveMessage += TEXT("- Search blueprint.list_templates(query=\"...\") for library patterns\n");
    EffectiveMessage += TEXT("- Use blueprint.describe_function(class, function) to check exact pin names before writing plan_json\n\n");
    EffectiveMessage += TEXT("Build the COMPLETE system. For each Blueprint:\n");
    EffectiveMessage += TEXT("1. Create structure (components, variables, interfaces, dispatchers)\n");
    EffectiveMessage += TEXT("2. Add function signatures\n");
    EffectiveMessage += TEXT("3. Compile the structure\n");
    EffectiveMessage += TEXT("4. Write graph logic with apply_plan_json. Use @step.auto for pin wiring.\n");
    EffectiveMessage += TEXT("5. Compile to 0 errors. Fix first error before moving on.\n");
    EffectiveMessage += TEXT("Do not stop until every asset is fully built, wired, and compiled.\n");
}
```

### 3.3 `OliveConversationManager.h` (header)

**Remove:**
- `#include "Brain/OliveAgentConfig.h"`
- `FOliveAgentPipelineResult CachedPipelineResult;` member
- `TSet<FString> ModifiedAssetPaths;` member
- `bool bIsReviewerCorrectionPass = false;` member

### 3.4 `OliveConversationManager.cpp` (implementation)

**Remove:**
- `#include "Brain/OliveAgentPipeline.h"` (line 18)
- Pipeline execution in `SendUserMessage()` (lines ~471-483):
  ```cpp
  // Remove:
  ModifiedAssetPaths.Reset();
  if (bTurnHasExplicitWriteIntent)
  {
      FOliveAgentPipeline Pipeline;
      CachedPipelineResult = Pipeline.Execute(Message, ActiveContextPaths);
  }
  else
  {
      CachedPipelineResult = FOliveAgentPipelineResult(); // Reset
  }
  ```
- Pipeline injection in `BuildSystemMessage()` (lines ~649-657):
  ```cpp
  // Remove:
  if (CachedPipelineResult.bValid)
  {
      FString PipelineContext = CachedPipelineResult.FormatForPromptInjection();
      ...
  }
  ```
- Reviewer block in `HandleComplete()` (lines ~970-1010): entire `bShouldReview` / `RunReviewer` / `bIsReviewerCorrectionPass` section
- `ModifiedAssetPaths.Add(ToolAssetPath)` in `ExecuteToolCall()` (line ~1292)
- Any remaining references to `ModifiedAssetPaths`, `bIsReviewerCorrectionPass`, or `CachedPipelineResult`

### 3.5 `OliveAISettings.h` (settings)

**Remove the entire "Agent Pipeline" category** (~lines 428-536):
- `bEnableTemplateDiscoveryPass` -- KEEP (move to general category or leave in place)
- `bCustomizeAgentModels` -- remove
- `RouterProvider`, `RouterModel` -- remove
- `ScoutProvider`, `ScoutModel` -- remove
- `ResearcherProvider`, `ResearcherModel` -- remove
- `ArchitectProvider`, `ArchitectModel` -- remove
- `ReviewerProvider`, `ReviewerModel` -- remove
- `AgentTimeoutSeconds` -- remove
- `bEnablePostBuildReview` -- remove

**Remove method:**
- `GetAgentModelConfig()` declaration

**Remove:**
- `#include "Brain/OliveAgentConfig.h"` (line 7)

### 3.6 `OliveAISettings.cpp`

**Remove:**
- `#include "Brain/OliveAgentConfig.h"` (line 4)
- `GetAgentModelConfig()` implementation (~lines 341-400+)

### 3.7 Pipeline Files -- Clean Delete (Option B)

**Delete these files entirely:**

1. `Source/OliveAIEditor/Public/Brain/OliveAgentPipeline.h` (~312 lines)
2. `Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp` (~4000+ lines)
3. `Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h` (~267 lines)

**Rationale:** These files are dead code after removal. Leaving deprecated files in the codebase creates confusion, increases compile times, and risks accidental re-inclusion. A clean delete is safe because:
- No Build.cs references to these files (Brain/ is a recursive include path, not file-specific)
- No test files reference pipeline types
- No UI/Slate files reference pipeline types
- No MCP prompt templates reference pipeline types
- No DefaultOliveAI.ini entries for pipeline settings
- No uplugin manifest references

### 3.8 Transitive Dependency Cleanup

All files that include the deleted headers must be updated. Complete list:

| File | Include to Remove | Members/Code to Remove |
|------|-------------------|----------------------|
| `OliveCLIProviderBase.h` | `Brain/OliveAgentConfig.h` | `CachedPipelineResult`, `bIsReviewerCorrectionPass` |
| `OliveCLIProviderBase.cpp` | `Brain/OliveAgentPipeline.h` | Pipeline block, Reviewer block (see 3.2) |
| `OliveConversationManager.h` | `Brain/OliveAgentConfig.h` | `CachedPipelineResult`, `ModifiedAssetPaths`, `bIsReviewerCorrectionPass` |
| `OliveConversationManager.cpp` | `Brain/OliveAgentPipeline.h` | Pipeline execution, injection, Reviewer block (see 3.4) |
| `OliveAISettings.h` | `Brain/OliveAgentConfig.h` | All Agent Pipeline category UPROPERTYs, `GetAgentModelConfig()` |
| `OliveAISettings.cpp` | `Brain/OliveAgentConfig.h` | `GetAgentModelConfig()` implementation |

**Non-breaking reference (comment only):**
- `OliveClassAPIHelper.h` line 12: comment says "OliveAgentPipeline (component API map injection into Builder prompt)". Update comment to remove the pipeline reference. No include, no code dependency.

### 3.9 `DefaultOliveAI.ini`

No pipeline-specific entries exist. No changes needed.

---

## 4. System Prompt After Revert (stdin channel)

The stdin prompt for a write-intent message looks like:

```
<user message>

## Current State of @BP_Character
Components: ...
Variables: ...
Functions: ...
**Do NOT re-read these assets** -- their current state is shown above.

## Existing Assets That May Be Relevant
- /Game/Blueprints/BP_Gun (Blueprint)
- /Game/Blueprints/BP_Projectile (Blueprint)
Use project.search if you need more details on any of these.

## Reference Templates Found
(output from FOliveUtilityModel::FormatDiscoveryForPrompt)
- bp_weapon_component (library, combatfs): 8 functions, parent: UActorComponent
  Matched: Fire, Reload
- projectile (factory): parameterized projectile template
...

## Required: Asset Decomposition
Before calling ANY tools, list every game entity that needs its own Blueprint:
...
(decomposition directive as shown in section 3.2)

## Tool Execution Requirement
Before any final explanation text, execute at least one MCP tool call...
```

---

## 5. Risk Assessment

### Capabilities Lost

| Capability | Pipeline Feature | Mitigation |
|---|---|---|
| Pre-validated parent classes | Validator (C++ reflection) | Agent picks correct classes from UE knowledge + compile feedback |
| Pre-resolved function pin names | Section 3.25 | Agent uses `blueprint.describe_function` on demand |
| Component API map | Section 3.5 | Agent reads component APIs via `blueprint.read` |
| Pre-computed Build Plan | Architect agent | Agent plans inline (decomposition directive) |
| Task complexity routing | Router agent | All tasks get same treatment (simpler) |
| Post-build review | Reviewer agent | Agent self-corrects via compile errors |
| Template overview pre-loading | Scout template content | Agent calls `blueprint.get_template` on demand |

### Risk: Quality regression on complex multi-asset tasks

The pipeline's main value was on 4+ asset tasks where the Architect produced a coherent Build Plan spanning multiple assets with interactions. Without it, the agent must plan inline. Mitigation:
- The decomposition directive forces explicit asset listing before building
- Template discovery provides reference patterns
- `blueprint.describe_function` provides pin-level accuracy on demand

**Estimated impact:** Complex tasks (4+ assets) may need 1-2 more self-correction cycles. Simple/moderate tasks (1-3 assets) should be faster overall due to eliminated overhead.

### Risk: Pin name hallucination

Without Section 3.25, the agent guesses pin names from LLM training memory. This was the original motivation for the Pin Reference feature.

**Mitigation:** `blueprint.describe_function` tool exists and returns exact pin names. The updated decomposition directive explicitly tells the agent to use it. The resolver's UPROPERTY auto-rewrite catches `call` ops that should be `set_var`/`get_var`.

### Risk: No regression -- gains

- 60-180s overhead eliminated (down to 10-30s)
- No more pipeline timeout failures (which triggered the sparse fallback)
- No more per-agent model configuration complexity
- Simpler codebase (4000+ fewer lines of active code)

---

## 6. Task Breakdown

### Senior Task 1: CLIProviderBase Revert (1-2 hours)
- Remove pipeline block in `SendMessageAutonomous()`, restore discovery pass + decomposition directive
- Remove Reviewer block in `HandleResponseCompleteAutonomous()` (lines ~1221-1302)
- Remove `CachedPipelineResult` and `bIsReviewerCorrectionPass` members from header
- Remove `#include "Brain/OliveAgentPipeline.h"` and `#include "Brain/OliveAgentConfig.h"`
- Test: autonomous run completes with discovery + decomposition, no pipeline calls

### Senior Task 2: ConversationManager Revert (30 min)
- Remove pipeline execution in `SendUserMessage()` (lines ~471-483)
- Remove pipeline injection in `BuildSystemMessage()` (lines ~649-657)
- Remove Reviewer block in `HandleComplete()` (lines ~970-1010)
- Remove `CachedPipelineResult`, `ModifiedAssetPaths`, `bIsReviewerCorrectionPass` members
- Remove both includes (`OliveAgentPipeline.h`, `OliveAgentConfig.h`)
- Test: orchestrated provider path works without pipeline

### Junior Task 3: Settings Cleanup (30 min)
- Remove Agent Pipeline category properties from `OliveAISettings.h` (all per-agent provider/model UPROPERTYs, `bCustomizeAgentModels`, `AgentTimeoutSeconds`, `bEnablePostBuildReview`)
- Remove `#include "Brain/OliveAgentConfig.h"` from both `.h` and `.cpp`
- Remove `GetAgentModelConfig()` from `.h` and `.cpp`
- Keep `bEnableTemplateDiscoveryPass` and `bEnableLLMKeywordExpansion`

### Junior Task 4: Delete Pipeline Files + Comment Cleanup (15 min)
- Delete `Source/OliveAIEditor/Public/Brain/OliveAgentPipeline.h`
- Delete `Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp`
- Delete `Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h`
- Update `OliveClassAPIHelper.h` line 12: change comment from "OliveAgentPipeline (component API map injection into Builder prompt)" to "Prompt assembler / future context injection"
- Verify build succeeds

### Senior Task 5: System Prompt Update (1-2 hours)
- Update `SetupAutonomousSandbox()` to write the revised AGENTS.md (see Section 8)
- Update `BuildCLISystemPrompt()` to use the revised `--append-system-prompt` content (see Section 8)
- Test: sandbox AGENTS.md contains planning + research + building guidance
- Test: `--append-system-prompt` contains knowledge packs without pipeline-specific references

### Senior Task 6: Build and Smoke Test (30 min)
- Full rebuild
- Test autonomous mode: write-intent task runs discovery + decomposition + CLI launch
- Test autonomous mode: read-only task skips discovery, launches CLI directly
- Test orchestrated mode: ConversationManager works without pipeline
- Test continuation: "continue" message works without pipeline context

**Total estimated time: 4-6 hours**

---

## 7. Implementation Order

1. Junior Task 4 (delete pipeline files, verify no compile break)
2. Junior Task 3 (settings cleanup)
3. Senior Task 1 (CLIProviderBase -- the main revert)
4. Senior Task 2 (ConversationManager revert)
5. Senior Task 5 (system prompt update)
6. Senior Task 6 (build + smoke test)

Step 1 first because it identifies any unexpected transitive dependencies immediately at compile time. Step 2 removes the types that steps 3-4 reference. Steps 3-4 are the core revert. Step 5 updates the agent's context. Step 6 validates everything.

---

## 8. System Prompt Design

The agent now handles planning, research, AND building. The system prompt must cover all three roles without being prescriptive about how the agent sequences its work.

### 8.1 Two Prompt Channels

**Channel 1: `--append-system-prompt` (reference context)**
This is the persistent context the agent can refer back to. It contains knowledge packs, tool schemas, policies, and domain reference material. The agent reads this when it needs to look something up -- it does NOT drive the agent's immediate actions.

**Channel 2: `stdin` prompt (imperative channel)**
This is the user's message plus injected context (asset state, discovery results, decomposition directive). It tells the agent what to do NOW.

### 8.2 `--append-system-prompt` Content (BuildCLISystemPrompt)

The current `BuildCLISystemPrompt()` is almost right for the single-agent flow. Changes needed:

**Keep as-is:**
- Project context (from `GetProjectContext()`)
- Policy context (from `GetPolicyContext()`)
- recipe_routing knowledge pack
- cli_blueprint knowledge pack
- blueprint_design_patterns knowledge pack
- events_vs_functions knowledge pack
- node_routing knowledge pack
- Template catalog block
- Tool schemas (CLI-specific inline serialization)
- Tool call format instructions

**No changes needed to BuildCLISystemPrompt.** The existing content is already appropriate for a single agent that plans, researches, and builds. The knowledge packs were written for this exact use case before the pipeline was added. The pipeline only changed the stdin channel (injecting Build Plan, Section 3.25, Component API Map), not the system prompt.

### 8.3 Sandbox AGENTS.md Content (SetupAutonomousSandbox)

The sandbox AGENTS.md is the equivalent of `--append-system-prompt` for the autonomous (MCP) path. It needs a planning section added since the agent now plans for itself.

**Current structure:**
1. Identity + critical rules
2. cli_blueprint.txt knowledge pack
3. recipe_routing.txt knowledge pack
4. blueprint_design_patterns.txt knowledge pack
5. events_vs_functions.txt knowledge pack
6. node_routing.txt knowledge pack

**Updated structure -- add a planning section between identity and knowledge packs:**

```
# Olive AI Studio - Agent Context

You are an AI assistant integrated with Unreal Engine 5.5 via Olive AI Studio.
Your job is to help users create and modify game assets (Blueprints, Behavior Trees,
PCG graphs, etc.) using the MCP tools provided.

## Critical Rules
- You are NOT a plugin developer. Do NOT modify plugin source code.
- Use ONLY the MCP tools to create and edit game assets.
- All asset paths should be under `/Game/` (the project's Content directory).
- When creating Blueprints, use `blueprint.create` (with optional template_id for
  templates) -- never try to create .uasset files manually.
- Complete the FULL task: create structures, wire graph logic, compile, and verify.
  Do not stop partway.
- After each compile pass, ask yourself: 'Have I built everything the user asked for?'
  If not, continue building the next part.
- Before finishing, verify you built EVERY part the user asked for -- don't stop after
  the first Blueprint compiles.
- Batch independent tool calls (add_variable, add_component) in a single response
  when possible.
- After creating from a template (blueprint.create with template_id), check the result
  for the list of created functions. Write plan_json for EACH function -- they are
  empty stubs. Do NOT call blueprint.read or read_function after template creation.

## Planning

For multi-asset tasks, plan before building. Ask:
- "Does this thing exist in the world with its own transform?" -> separate Blueprint
- "Is it a value on an existing actor?" -> variable
- "Is it a capability attached to many actors?" -> component

Common decomposition: weapons, projectiles, doors, keys, vehicles = always separate actors.
After listing your assets, identify how they communicate (interfaces, dispatchers, casts,
overlap events). See Blueprint Design Patterns for details.

## Research

Research tools help you verify assumptions before writing graph logic:
- `blueprint.list_templates(query="...")` -- search library/factory templates for patterns
- `blueprint.get_template(id, pattern="FuncName")` -- read specific function implementations
- `blueprint.describe_function(class, function)` -- verify exact pin names
- `blueprint.describe_node_type(type)` -- check K2Node properties and pins
- `project.search(query)` -- find existing assets by name
- `olive.get_recipe(query)` -- tested wiring patterns for common tasks

Research when you are unsure. Skip research when you are confident in your UE5 knowledge.

## Building

Three approaches -- use whichever fits, mix freely:
1. plan_json -- batch declarative, best for standard logic (3+ nodes)
2. Granular tools (add_node, connect_pins) -- any UK2Node, best for edge cases
3. editor.run_python -- full UE editor API, best for anything tools can't express

Build one asset at a time: structure -> function signatures -> compile structure ->
graph logic -> compile to 0 errors -> next asset.

## Self-Correction
- Fix the FIRST compile error before moving on
- After a plan_json failure, all nodes from that plan are rolled back.
  Do NOT reference node IDs from a failed plan.
- If one approach fails twice, try a different tool or technique
- If something genuinely cannot be done, tell the user what and why

---

(knowledge packs follow: cli_blueprint, recipe_routing, design_patterns, events_vs_functions, node_routing)
```

**Key differences from the current AGENTS.md:**
1. Added `## Planning` section -- decomposition heuristics (from blueprint_design_patterns Section 0, condensed)
2. Added `## Research` section -- lists research tools and when to use them
3. Added `## Building` section -- the three approaches (already in cli_blueprint but not prominent enough)
4. Added `## Self-Correction` section -- error recovery guidance
5. Removed "Component API Reference" mention from pin verification (pipeline-specific)
6. All existing knowledge packs remain unchanged

### 8.4 stdin Prompt Content (SendMessageAutonomous)

The stdin prompt is assembled dynamically in `SendMessageAutonomous()`. After the revert, it contains:

1. User message
2. @-mention asset state (if any) + "Do NOT re-read these assets"
3. Existing Assets That May Be Relevant (keyword search results)
4. Reference Templates Found (discovery pass results, if enabled)
5. Required: Asset Decomposition (for write-intent, non-continuation only)
6. Tool Execution Requirement

This matches the pre-pipeline flow exactly, with the decomposition directive updated per Section 3.2.

### 8.5 Knowledge Pack Audit

All existing knowledge packs are appropriate for the single-agent flow:

| Pack | Status | Notes |
|------|--------|-------|
| `cli_blueprint.txt` | **Keep** | Written for single agent. Contains plan_json syntax, tool approaches, pin verification. Only reference to pipeline: "Component API Reference (injected in pipeline prompt)" in pin verification priority list. Update this one line. |
| `recipe_routing.txt` | **Keep** | Generic routing, no pipeline references |
| `blueprint_design_patterns.txt` | **Keep** | Section 0 (decomposition) duplicates the stdin directive but at expanded length. The inline directive is a condensed version. Both are fine -- the system prompt gives the detailed reference, stdin gives the imperative. |
| `events_vs_functions.txt` | **Keep** | Pure reference content, no pipeline references |
| `node_routing.txt` | **Keep** | Pure reference content, no pipeline references |
| `blueprint_authoring.txt` | **Keep (API path only)** | Used by PromptAssembler for API providers, NOT by CLI path. No pipeline references. |

**One update needed:** In `cli_blueprint.txt`, the pin verification priority list (line ~107-111) says:
```
2. Component API Reference (injected in pipeline prompt) -- function signatures for key components
```
This reference to "pipeline prompt" is stale. Change to:
```
2. blueprint.describe_function(class, function) -- exact function signatures
```

### 8.6 Implementation Details for System Prompt Changes

**SetupAutonomousSandbox() changes:**

The `AgentContext` string assembly (lines ~383-431) needs the new Planning/Research/Building/Self-Correction sections inserted between the Critical Rules block and the first knowledge pack separator.

Specifically:
1. After line 396 (the template creation rule), add the four new sections
2. Keep the knowledge pack appending loop unchanged (lines ~398-431)

**BuildCLISystemPrompt() changes:**

None. The function already assembles the correct content for a single agent.

**cli_blueprint.txt change:**

Update one line in the Pin Name Verification section. This is a content file change, not a code change.

---

## 9. Edge Cases and New Risks

### 9.1 EmitStatus Lambda

The `EmitStatus` lambda (lines ~582-591) is currently used to show pipeline progress ("Analyzing task...", "Build plan ready..."). After the revert, it could still be useful for the discovery pass. Decision: **keep it** but update the status messages.

Instead of:
```
*Analyzing task and searching project for relevant assets...*
*Build plan ready. Launching builder...*
```

Use:
```
*Searching for relevant templates and assets...*
*Launching builder...*
```

The EmitStatus lambda is defined before the discovery pass code and used inside it. The pipeline-specific messages are removed with the pipeline block.

### 9.2 AutoContinue After Reviewer Removal

The auto-continue mechanism has three triggers:
1. Timeout-based auto-continue (keep)
2. Zero-tool-call nudge (keep)
3. Reviewer correction pass (remove)

The reviewer correction pass re-enters `SendMessageAutonomous()` with `bIsReviewerCorrectionPass = true` and `bIsAutoContinuation = true`. After removing the Reviewer block, the `bIsReviewerCorrectionPass` flag and its reset at line 1302 are both removed. The remaining auto-continue triggers work independently.

### 9.3 ModifiedAssetPaths in ConversationManager

`OliveConversationManager.cpp` tracks `ModifiedAssetPaths` (line 1292) and uses it for the Reviewer (line 972). After removing the Reviewer, this member and its population are dead code. Remove both.

Note: `LastRunContext.ModifiedAssetPaths` in `OliveCLIProviderBase.cpp` is a DIFFERENT member (part of `FAutonomousRunContext`). It is used by the continuation prompt builder and must be KEPT.

### 9.4 Pipeline PlannerSandbox Directory

`FOliveAgentPipeline::SetupPlannerSandbox()` creates `Saved/OliveAI/PlannerSandbox/`. This directory may exist on developer machines from previous runs. After deleting the pipeline files, the directory becomes orphaned but harmless (it is in Saved/ which is gitignored). No cleanup action needed.

### 9.5 OliveClassAPIHelper Comment

`OliveClassAPIHelper.h` line 12 has a comment: "OliveAgentPipeline (component API map injection into Builder prompt)". This is a comment only -- no code dependency. Update the comment to reflect current usage. The helper is still used by `OliveBlueprintPlanResolver`.

### 9.6 No Stale Config File Entries

`DefaultOliveAI.ini` was checked and contains no pipeline-specific entries. No cleanup needed.

### 9.7 Saved User Settings Migration

Users who had `bCustomizeAgentModels = true` with per-agent settings in their project's `Saved/Config/` will get UE's default behavior when properties are removed: the stale config entries are silently ignored. No migration code needed.
