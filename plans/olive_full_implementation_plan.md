# Olive AI — Full Implementation Plan

## Overview

This plan combines the 7 reliability fixes from the log analysis with a recipe/prompt architecture overhaul that ensures shared knowledge stays in sync between the CLI and API paths without duplicating content.

---

## Part A: Prompt Architecture Overhaul (Recipe System)

### Problem

The CLI's `BuildSystemPrompt()` has ~3KB of domain knowledge hardcoded in C++ string literals (workflows, plan JSON syntax, variable types, rules). This duplicates content that also exists in recipe files and `blueprint_authoring.txt`. When you add a recipe or update instructions, you have to manually update the C++ too — and they drift out of sync.

Meanwhile, the recipe files exist on disk and are loadable via `olive.get_recipe`, but the AI almost never calls that tool because it costs an entire iteration and the hardcoded instructions are already in the system prompt.

### Solution

1. Move the shared domain knowledge from C++ into a new disk file: `cli_blueprint.txt`
2. Have `BuildSystemPrompt()` load it from disk instead of hardcoded strings
3. Add condensed recipe summaries to `recipe_routing.txt` so the most important patterns are always in the system prompt — no tool call needed
4. Add multi-asset planning to both `recipe_routing.txt` and `blueprint_authoring.txt`
5. Add a `multi_asset` recipe for detailed reference via `olive.get_recipe`

### A1: Create `cli_blueprint.txt` knowledge pack

**New file**: `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

This file contains everything currently hardcoded in `BuildSystemPrompt()` lines 74247-74337 — workflows, plan JSON reference, ops, wires, function graphs, function resolution, variable types, and rules. Moved verbatim from C++, then enhanced with multi-asset planning.

```
You are an Unreal Engine 5.5 Blueprint specialist.
Think through the complete design before calling tools.

## Workflows
CREATE new Blueprint:
1. blueprint.create → 2. add_component/add_variable → 3. blueprint.apply_plan_json (ALL graph logic in one call)

MODIFY existing Blueprint:
1. project.search (find exact path) → 2. blueprint.read (understand current state) → 3. add_variable/add_component → 4. blueprint.apply_plan_json
For MODIFY: always search for the asset path first — never guess paths.

SMALL EDIT (1-2 nodes on existing graph):
1. blueprint.read_event_graph → 2. blueprint.add_node + blueprint.connect_pins

MULTI-ASSET (2+ Blueprints needed, e.g. "gun and bullet"):
1. Create ALL asset structures first (blueprint.create + add_component + add_variable for EVERY asset)
2. Wire graph logic for each asset AFTER all structures exist
3. Budget: with 10 iterations and 2 assets, spend max 5 per asset
4. NEVER spend all iterations on one asset when multiple are needed

## Plan JSON (v2.0)
```json
{"schema_version":"2.0","steps":[
  {"step_id":"evt","op":"event","target":"BeginPlay"},
  {"step_id":"get_xform","op":"call","target":"GetActorTransform"},
  {"step_id":"spawn","op":"spawn_actor","target":"Actor","inputs":{"SpawnTransform":"@get_xform.auto"},"exec_after":"evt"},
  {"step_id":"print","op":"call","target":"PrintString","inputs":{"InString":"Done"},"exec_after":"spawn"}
]}
```

## Ops
event, custom_event, call, get_var, set_var, branch, sequence, cast, for_loop, for_each_loop, delay, is_valid, print_string, spawn_actor, make_struct, break_struct, return, comment

## Wires
Data: @step.auto (type-match, use ~80%), @step.~hint (fuzzy), @step.PinName (exact)
Literals: "InString":"Hello" (no @ = pin default)
Exec: exec_after:"step_id" (chains then→execute), exec_outputs:{"True":"a","False":"b"} for Branch

## Function Graphs vs EventGraph
- EventGraph: use op:"event" with target:"BeginPlay"/"Tick"/etc. as entry points
- Function graphs (blueprint.add_function): the entry node is auto-created.
  First impure step should NOT use exec_after. It auto-chains from the entry node.
  Pure steps (get_var, pure function calls) need no exec wiring.

## Function Resolution
Use natural names for op:call. Resolves K2_ prefixes and aliases automatically.
Examples: Destroy→K2_DestroyActor, Print→PrintString, GetWorldTransform→K2_GetComponentToWorld

## Variable Types
Basic: {"category":"float"}, {"category":"bool"}, {"category":"int"}, {"category":"string"}
Shorthand also works: "Float", "Boolean", "Integer", "String", "Vector", "Rotator", "Transform"
Object ref: {"category":"object","class_name":"Actor"} (use UE class name, not asset path)
Blueprint ref: {"category":"object","class_name":"BP_Gun_C"} (append _C for Blueprint classes)
Class ref: {"category":"class","class_name":"Actor"} (for TSubclassOf)
Array: {"category":"array","value_type":{"category":"float"}}
Enum: {"category":"byte","enum_name":"ECollisionChannel"}

## Rules
- Asset paths MUST end with asset name: /Game/Blueprints/BP_Gun (NOT /Game/Blueprints/)
- For EXISTING assets, use project.search to find exact paths. For NEW assets, choose the path directly.
- Use apply_plan_json for 3+ nodes. For 1-2 nodes, add_node + connect_pins is fine.
- preview_plan_json is optional. Prefer calling apply_plan_json directly.
- STEP ORDER: data-provider steps (get_var, pure calls) BEFORE steps that reference them via @ref.
- If add_variable fails, do NOT reference that variable in subsequent plan_json calls.
- SpawnActor uses SpawnTransform (Transform type), NOT separate Location/Rotation pins.
- Pin references use DOT separator: node_id.pin_name (NOT colon).
- Node IDs are scoped per graph — a node from "Fire" function does NOT exist in "EventGraph".
- Component classes: StaticMeshComponent, SphereComponent, BoxComponent, CapsuleComponent, ArrowComponent, ProjectileMovementComponent, SceneComponent, AudioComponent
```

### A2: Modify `BuildSystemPrompt()` to load from disk

**File**: `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp`
**Function**: `FOliveClaudeCodeProvider::BuildSystemPrompt`

Replace the entire hardcoded domain knowledge section (lines 74247-74337) with a disk load:

```cpp
// REPLACE everything from "// Identity" through "// Rules" section with:

    // ==========================================
    // Shared domain knowledge — loaded from disk so recipes/prompts stay in sync
    // ==========================================
    const FString CLIBlueprint = Assembler.GetKnowledgePackById(TEXT("cli_blueprint"));
    if (!CLIBlueprint.IsEmpty())
    {
        SystemPrompt += CLIBlueprint;
        SystemPrompt += TEXT("\n\n");
    }
    else
    {
        // Fallback: minimal inline instructions if file missing
        UE_LOG(LogOliveClaudeCode, Warning,
            TEXT("cli_blueprint knowledge pack not found. Using minimal fallback."));
        SystemPrompt += TEXT("You are an Unreal Engine 5.5 Blueprint specialist.\n");
        SystemPrompt += TEXT("Use blueprint.create, add_component, add_variable, apply_plan_json.\n\n");
    }

// KEEP these two blocks unchanged (they are CLI-mechanical, not domain knowledge):
    // Tool schemas (CLI-specific: inline since no native tool calling)
    if (Tools.Num() > 0)
    {
        SystemPrompt += FOliveCLIToolSchemaSerializer::Serialize(Tools, /*bCompact=*/true);
        SystemPrompt += TEXT("\n");
    }

    // Tool call format instructions (CLI-specific)
    SystemPrompt += FOliveCLIToolCallParser::GetFormatInstructions();
```

### A3: Update `recipe_routing.txt`

**File**: `Content/SystemPrompts/Knowledge/recipe_routing.txt`

Add multi-asset awareness and fix-wiring quick reference to the routing prompt that's always in the system prompt. This way the most critical patterns are visible without a tool call.

```
## Recipes
Call olive.get_recipe(category, name) before starting an unfamiliar task type.

BLUEPRINT: create, modify, fix_wiring, variables_components, edit_existing_graph, multi_asset

QUICK RULES:
- NEW blueprint → create + add_component/add_variable + apply_plan_json (ALL graph nodes in one call)
- MODIFY existing → project.search (find path) → blueprint.read → then write tools
- SMALL EDIT (1-2 nodes) → read_event_graph → add_node + connect_pins
- MULTI-ASSET (2+ blueprints) → create ALL structures first → then wire each graph → budget iterations per asset
- FIX wiring → use pin_manifests from the apply result, NOT blueprint.read
- NEVER call blueprint.read before blueprint.create
- NEVER use add_node one-at-a-time for 3+ nodes — use plan_json
- preview_plan_json is optional — prefer calling apply_plan_json directly
- SpawnActor uses SpawnTransform (Transform), NOT separate Location/Rotation pins
- Object variable refs need class_name: {"category":"object","class_name":"BP_Gun_C"} (append _C)
- Pin refs use DOT: node_id.pin_name (NOT colon)
- Node IDs are scoped per graph — don't use node_ids across different graphs
```

### A4: Update `_manifest.json`

**File**: `Content/SystemPrompts/Knowledge/recipes/_manifest.json`

Add the `multi_asset` recipe entry:

```json
{
  "format_version": "1.0",
  "schema_version": "1.0",
  "categories": {
    "blueprint": {
      "display_name": "Blueprint Workflows",
      "profiles": ["Blueprint", "Auto"],
      "recipes": {
        "create": {
          "description": "Create new Blueprint with components, variables, and graph logic",
          "tags": ["create", "new", "plan_json", "spawn"],
          "max_tokens": 250
        },
        "modify": {
          "description": "Modify an existing Blueprint — search, read, then write",
          "tags": ["modify", "edit", "existing", "change"],
          "max_tokens": 220
        },
        "fix_wiring": {
          "description": "Fix wiring after partial plan failure using pin_manifests from the result",
          "tags": ["fix", "wiring", "error", "pin_manifests", "partial", "connect"],
          "max_tokens": 180
        },
        "variables_components": {
          "description": "Add variables and components with correct type format and configuration",
          "tags": ["variable", "component", "type", "object", "array", "enum"],
          "max_tokens": 250
        },
        "edit_existing_graph": {
          "description": "Insert, remove, or rewire nodes in an existing event graph or function graph",
          "tags": ["edit", "insert", "remove", "disconnect", "rewire", "function", "graph"],
          "max_tokens": 250
        },
        "multi_asset": {
          "description": "Create multiple related Blueprints (e.g. gun + bullet, spawner + enemy) with iteration budgeting",
          "tags": ["multi", "multiple", "system", "gun", "bullet", "spawner", "enemy", "and"],
          "max_tokens": 300
        }
      }
    }
  }
}
```

### A5: Create `multi_asset` recipe

**New file**: `Content/SystemPrompts/Knowledge/recipes/blueprint/multi_asset.txt`

```
WORKFLOW:
1. Create ALL asset structures FIRST (blueprint.create + add_component + add_variable for EVERY asset)
2. Wire graph logic for Asset A (blueprint.apply_plan_json)
3. Wire graph logic for Asset B (blueprint.apply_plan_json)
4. Fix wiring errors for both (if needed)

CRITICAL: Create ALL structures in one iteration before any graph logic.
This ensures cross-references work (BP_Gun can reference BP_Bullet_C as a type).

EXAMPLE (Gun + Bullet):

Turn 1 — Create BOTH structures (batch all in one response):
  blueprint.create → {path:"/Game/Blueprints/BP_Bullet", parent_class:"Actor"}
  blueprint.add_component → {path:"/Game/Blueprints/BP_Bullet", class:"SphereComponent", name:"Collision", root:true}
  blueprint.add_component → {path:"/Game/Blueprints/BP_Bullet", class:"ProjectileMovementComponent", name:"Movement"}
  blueprint.add_variable → {path:"/Game/Blueprints/BP_Bullet", name:"BulletSpeed", type:"Float", default:"3000.0"}
  blueprint.create → {path:"/Game/Blueprints/BP_Gun", parent_class:"Actor"}
  blueprint.add_component → {path:"/Game/Blueprints/BP_Gun", class:"StaticMeshComponent", name:"GunMesh", root:true}
  blueprint.add_component → {path:"/Game/Blueprints/BP_Gun", class:"SceneComponent", name:"MuzzlePoint"}
  blueprint.add_variable → {path:"/Game/Blueprints/BP_Gun", name:"FireRate", type:"Float", default:"0.2"}

Turn 2 — BP_Bullet EventGraph:
  blueprint.apply_plan_json → {path:"/Game/Blueprints/BP_Bullet", graph:"EventGraph",
    plan:{schema_version:"2.0", steps:[
      {step_id:"bp", op:"event", target:"BeginPlay"},
      {step_id:"life", op:"call", target:"SetLifeSpan", inputs:{InLifespan:"3.0"}, exec_after:"bp"}
    ]}}

Turn 3 — BP_Gun Fire function:
  blueprint.add_function → {path:"/Game/Blueprints/BP_Gun", function_name:"Fire"}
  blueprint.apply_plan_json → {path:"/Game/Blueprints/BP_Gun", graph:"Fire",
    plan:{schema_version:"2.0", steps:[
      {step_id:"muzzle", op:"call", target:"GetWorldTransform", target_class:"SceneComponent"},
      {step_id:"spawn", op:"spawn_actor", target:"Actor",
       inputs:{SpawnTransform:"@muzzle.auto"}}
    ]}}

ITERATION BUDGET:
  With MaxIterations=10 and 2 assets:
  - Turns 1-2: Both structures + first asset graph
  - Turns 3-4: Second asset graph
  - Turns 5-6: Fix wiring errors
  - NEVER spend more than 5 turns on one asset when 2+ are needed

GOTCHAS:
- Blueprint class refs append _C: "BP_Bullet_C" not "BP_Bullet"
- TSubclassOf: {category:"class", class_name:"Actor"} for class variable type
- Function graphs: first impure step has NO exec_after (auto-chains from entry)
- Node IDs are scoped per graph — don't mix between EventGraph and function graphs
```

### A6: Update `blueprint_authoring.txt`

**File**: `Content/SystemPrompts/Knowledge/blueprint_authoring.txt`

Append this section at the end of the existing file:

```
## Multi-Asset Task Planning

When the user's request requires creating MULTIPLE Blueprints (e.g., "create a gun and bullet system"):

1. BEFORE calling any tools, list ALL assets you need to create
2. Create each asset's STRUCTURE first (blueprint.create + add_component + add_variable) before wiring ANY graph logic
3. Create graph logic for each asset AFTER all structures exist
4. Allocate your iteration budget: if you have 10 iterations and need 2 assets, spend at most 5 on each
5. PRIORITIZE creating all assets over perfecting wiring on the first one

NEVER spend all iterations on one asset when multiple are needed.
```

### A7: Update existing recipes with pin/graph scoping reminders

**File**: `Content/SystemPrompts/Knowledge/recipes/blueprint/fix_wiring.txt`

Add to the GOTCHAS section:

```
- Pin references use DOT separator: node_id.pin_name (NOT node_id:pin_name)
- Node IDs are scoped per graph — a node_id from "Fire" function does NOT exist in "EventGraph"
- If fixing wiring in a function graph, pass the function name as the graph parameter, not "EventGraph"
```

---

## Part B: C++ Reliability Fixes

### B1: Auto-Correct Colon Pin References (P0)

**File**: `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp`
**Function**: `FOliveGraphWriter::ParsePinReference`

```cpp
// REPLACE the existing function with:
bool FOliveGraphWriter::ParsePinReference(const FString& PinRef, FString& OutNodeId, FString& OutPinName)
{
    // Format: "node_id.pin_name"
    // Also accept "node_id:pin_name" (common AI mistake) with auto-correction
    int32 SepIndex;
    if (PinRef.FindChar(TEXT('.'), SepIndex))
    {
        OutNodeId = PinRef.Left(SepIndex);
        OutPinName = PinRef.Mid(SepIndex + 1);
        return !OutNodeId.IsEmpty() && !OutPinName.IsEmpty();
    }

    // Fallback: accept colon as separator (AI frequently uses this format)
    if (PinRef.FindChar(TEXT(':'), SepIndex))
    {
        OutNodeId = PinRef.Left(SepIndex);
        OutPinName = PinRef.Mid(SepIndex + 1);
        if (!OutNodeId.IsEmpty() && !OutPinName.IsEmpty())
        {
            UE_LOG(LogOliveGraphWriter, Warning,
                TEXT("Auto-corrected pin reference '%s' (colon->dot). Use 'node_id.pin_name' format."),
                *PinRef);
            return true;
        }
    }

    return false;
}
```

### B2: Zero-Tool-Call Detection and Re-prompt (P0)

**File**: `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`
**Function**: `FOliveConversationManager::HandleComplete`

In the `else` branch where `PendingToolCalls.Num() == 0`, add BEFORE the existing `bHasPendingCorrections` check:

```cpp
        // === NEW: Detect zero-tool-call first response for write-intent tasks ===
        if (CurrentToolIteration == 0
            && bTurnHasExplicitWriteIntent
            && ZeroToolRepromptCount < MaxZeroToolReprompts)
        {
            ZeroToolRepromptCount++;

            FString ForceToolPrompt = FString::Printf(
                TEXT("[SYSTEM: You responded with text but the user's request requires "
                     "action. You MUST call tools to complete the task. Do not explain "
                     "what you would do — actually do it by calling the appropriate tools. "
                     "Re-prompt %d/%d.]"),
                ZeroToolRepromptCount, MaxZeroToolReprompts);

            FOliveChatMessage RepromptMessage;
            RepromptMessage.Role = EOliveChatRole::User;
            RepromptMessage.Content = ForceToolPrompt;
            RepromptMessage.Timestamp = FDateTime::UtcNow();
            AddMessage(RepromptMessage);

            UE_LOG(LogOliveAI, Warning,
                TEXT("AI responded text-only on first iteration with write intent. "
                     "Re-prompting to force tool use (%d/%d)"),
                ZeroToolRepromptCount, MaxZeroToolReprompts);

            SendToProvider();
            return;
        }
```

**File**: `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h`

Add members:

```cpp
    int32 ZeroToolRepromptCount = 0;
    static constexpr int32 MaxZeroToolReprompts = 2;
```

**File**: `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`
**Function**: `SendUserMessage` — add reset:

```cpp
    ZeroToolRepromptCount = 0;
```

### B3: Iteration Budget Awareness (P1)

**File**: `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`
**Function**: `FOliveConversationManager::SendToProvider`

After distillation, before sending to provider:

```cpp
    // === Inject iteration budget awareness ===
    if (CurrentToolIteration > 0)
    {
        const int32 RemainingIterations = MaxToolIterations - CurrentToolIteration;
        FString BudgetNote;

        if (RemainingIterations <= 3)
        {
            BudgetNote = FString::Printf(
                TEXT("[ITERATION BUDGET: %d/%d used, only %d remaining. "
                     "CRITICAL: Focus on completing the most important remaining work. "
                     "If there are multiple assets to create, prioritize creating them "
                     "over perfecting existing ones.]"),
                CurrentToolIteration, MaxToolIterations, RemainingIterations);
        }
        else if (RemainingIterations <= 6)
        {
            BudgetNote = FString::Printf(
                TEXT("[ITERATION BUDGET: %d/%d used, %d remaining. "
                     "Plan remaining tool calls efficiently.]"),
                CurrentToolIteration, MaxToolIterations, RemainingIterations);
        }

        if (!BudgetNote.IsEmpty())
        {
            FOliveChatMessage BudgetMessage;
            BudgetMessage.Role = EOliveChatRole::System;
            BudgetMessage.Content = BudgetNote;
            BudgetMessage.Timestamp = FDateTime::UtcNow();

            int32 InsertIndex = MessagesToSend.Num();
            for (int32 i = MessagesToSend.Num() - 1; i >= 0; --i)
            {
                if (MessagesToSend[i].Role == EOliveChatRole::User)
                {
                    InsertIndex = i;
                    break;
                }
            }
            MessagesToSend.Insert(BudgetMessage, InsertIndex);
        }
    }
```

### B4: Enhanced Self-Correction Guidance (P1)

**File**: `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`
**Function**: `FOliveSelfCorrectionPolicy::BuildToolErrorMessage`

Replace the `BP_REMOVE_NODE_FAILED` case:

```cpp
    else if (ErrorCode == TEXT("BP_REMOVE_NODE_FAILED"))
    {
        Guidance = TEXT("The node was not found in the specified graph. "
            "IMPORTANT: node_ids (node_0, node_1, etc.) are scoped to the graph "
            "where they were created. A node created by apply_plan_json in the 'Fire' "
            "function graph does NOT exist in 'EventGraph'. "
            "Use blueprint.read_event_graph or blueprint.read_function to find the "
            "correct node_id in the target graph.");
    }
```

Add a new case for `BP_CONNECT_PINS_FAILED`:

```cpp
    else if (ErrorCode == TEXT("BP_CONNECT_PINS_FAILED"))
    {
        Guidance = TEXT("Pin connection failed. Common causes: "
            "1) Node not found — node_ids are scoped per graph. "
            "2) Pin format — use 'node_id.pin_name' (dot, NOT colon). "
            "3) Pin name mismatch — use pin_manifests from apply_plan_json result. "
            "4) Type mismatch — ensure compatible pin types.");
    }
```

### B5: Fix Version Check CreateProc (P3)

**File**: `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp`
**Function**: `FOliveClaudeCodeProvider::GetClaudeCodeVersion`

```cpp
    FString VersionOutput;
    int32 ReturnCode;
    if (ClaudePath.EndsWith(TEXT(".js")))
    {
        FString NodeArgs = FString::Printf(TEXT("\"%s\" --version"), *ClaudePath);
        FPlatformProcess::ExecProcess(TEXT("node"), *NodeArgs, &ReturnCode, &VersionOutput, nullptr);
    }
    else
    {
        FPlatformProcess::ExecProcess(*ClaudePath, TEXT("--version"), &ReturnCode, &VersionOutput, nullptr);
    }
```

### B6: Dynamic MaxToolIterations for Multi-Asset Tasks (P2)

**File**: `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`
**Function**: `SendUserMessage`

After `DetectWriteIntent`:

```cpp
    if (bTurnHasExplicitWriteIntent && DetectMultiAssetIntent(Message))
    {
        MaxToolIterations = FMath::Max(MaxToolIterations, 20);
        UE_LOG(LogOliveAI, Log,
            TEXT("Multi-asset task detected. Increased MaxToolIterations to %d"),
            MaxToolIterations);
    }
    else
    {
        MaxToolIterations = 10;
    }
```

Add detection function:

```cpp
bool DetectMultiAssetIntent(const FString& UserMessage)
{
    static const TArray<FString> MultiAssetPatterns = {
        TEXT(" and "), TEXT(" with "), TEXT("system"),
        TEXT("both"), TEXT("each"), TEXT("multiple")
    };

    const FString Lower = UserMessage.ToLower();
    if (!MessageContainsAnyKeyword(Lower,
        {TEXT("create"), TEXT("make"), TEXT("build"), TEXT("implement")}))
    {
        return false;
    }

    for (const FString& Pattern : MultiAssetPatterns)
    {
        if (Lower.Contains(Pattern))
            return true;
    }
    return false;
}
```

---

## Implementation Order

| Step | What | Type | Files Changed |
|------|------|------|---------------|
| 1 | A1: Create `cli_blueprint.txt` | New file | `Content/SystemPrompts/Knowledge/cli_blueprint.txt` |
| 2 | A2: Load from disk in `BuildSystemPrompt` | C++ | `OliveClaudeCodeProvider.cpp` |
| 3 | A3: Update `recipe_routing.txt` | Edit file | `Content/SystemPrompts/Knowledge/recipe_routing.txt` |
| 4 | A4: Update `_manifest.json` | Edit file | `Content/SystemPrompts/Knowledge/recipes/_manifest.json` |
| 5 | A5: Create `multi_asset.txt` recipe | New file | `recipes/blueprint/multi_asset.txt` |
| 6 | A6: Update `blueprint_authoring.txt` | Edit file | `Content/SystemPrompts/Knowledge/blueprint_authoring.txt` |
| 7 | A7: Update `fix_wiring.txt` recipe | Edit file | `recipes/blueprint/fix_wiring.txt` |
| 8 | B1: Colon pin auto-correct | C++ | `OliveGraphWriter.cpp` |
| 9 | B2: Zero-tool-call re-prompt | C++ | `OliveConversationManager.cpp/.h` |
| 10 | B3: Iteration budget injection | C++ | `OliveConversationManager.cpp` |
| 11 | B4: Self-correction guidance | C++ | `OliveSelfCorrectionPolicy.cpp` |
| 12 | B5: Version check fix | C++ | `OliveClaudeCodeProvider.cpp` |
| 13 | B6: Dynamic iteration budget | C++ | `OliveConversationManager.cpp` |

Steps 1-7 (Part A) can all be done together as a single commit since they're all prompt/recipe file changes plus one C++ edit. Steps 8-13 (Part B) are independent C++ fixes that can each be their own commit.

---

## File Summary

**New files (2):**
- `Content/SystemPrompts/Knowledge/cli_blueprint.txt`
- `Content/SystemPrompts/Knowledge/recipes/blueprint/multi_asset.txt`

**Modified prompt/recipe files (4):**
- `Content/SystemPrompts/Knowledge/recipe_routing.txt`
- `Content/SystemPrompts/Knowledge/blueprint_authoring.txt`
- `Content/SystemPrompts/Knowledge/recipes/_manifest.json`
- `Content/SystemPrompts/Knowledge/recipes/blueprint/fix_wiring.txt`

**Modified C++ files (4):**
- `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp` (A2 + B5)
- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp` (B1)
- `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` (B2 + B3 + B6)
- `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h` (B2)
- `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` (B4)

---

## Testing Checklist

1. **"Create a gun and bullet blueprint system"** → Should create BOTH BP_Gun and BP_Bullet
2. **Second prompt after task completion** → Should execute tools, not just return text
3. **Pin reference with colon format** → Should auto-correct and succeed
4. **remove_node on wrong graph** → Should get graph-scoping guidance
5. **10+ iteration task** → Budget warnings should appear at iteration 7+
6. **Read-only question after write task** → Should NOT trigger zero-tool-call re-prompt
7. **CLI system prompt** → Should contain content from `cli_blueprint.txt`, NOT hardcoded C++
8. **olive.get_recipe("blueprint","multi_asset")** → Should return the new recipe
9. **Edit a recipe .txt file and restart** → CLI should pick up changes without recompiling
