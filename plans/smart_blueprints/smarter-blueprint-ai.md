# Making Olive AI Smarter at Blueprint Creation

## Problem Summary

The AI (Claude Code via MCP) struggles with blueprint creation due to four compounding issues:

1. **Path format errors** — Creates `/Game/Blueprints` instead of `/Game/Blueprints/BP_Gun`
2. **Never reaches Plan JSON** — Gets stuck on basic setup steps, never builds graph logic
3. **Unhelpful error messages** — Errors like "Invalid asset name" don't tell the AI what to fix
4. **High latency** — ~2 min per Claude Code CLI round-trip, compounding across multiple failed attempts
5. **Plan JSON is disabled by default** — `bEnableBlueprintPlanJsonTools = false`, so the prompt tells the AI to use tools it literally cannot access

---

## Fix 1: Enable Plan JSON by Default

**File:** `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` (line ~91569 in bundled_code)

```cpp
// BEFORE:
bool bEnableBlueprintPlanJsonTools = false;

// AFTER:
bool bEnableBlueprintPlanJsonTools = true;
```

**Why:** The routing wrapper prompt (line 72908) tells the AI: *"For ALL graph logic: use blueprint.preview_plan_json then blueprint.apply_plan_json."* But if Plan JSON tools aren't registered, the AI can never follow this instruction. It then falls back to individual `add_node`/`connect_pins` calls (which are harder to get right) or gives up entirely.

---

## Fix 2: Defensive Path Validation with Actionable Error Messages

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveBlueprintWriter.cpp` (around line 40703)

The current path validation has two problems:
- `FPackageName::IsValidLongPackageName` accepts `/Game/Blueprints` as valid (it's a valid package path, just not a valid *asset* path)
- The `AssetName.IsEmpty()` catch returns `"Invalid asset name"` — useless to the AI

### Replace the validation block:

```cpp
// Validate long package path format up front for actionable errors.
if (!FPackageName::IsValidLongPackageName(AssetPath))
{
    return FOliveBlueprintWriteResult::Error(
        FString::Printf(TEXT("Invalid Blueprint asset path '%s'. Expected '/Game/.../BP_Name'"), *AssetPath));
}

// Parse the asset path
FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
FString AssetName = FPackageName::GetShortName(AssetPath);

// Validate asset name — catch the "/Game/Blueprints" (no asset name) case
if (AssetName.IsEmpty())
{
    return FOliveBlueprintWriteResult::Error(
        FString::Printf(
            TEXT("Path '%s' is a folder, not an asset path. "
                 "Append the Blueprint name, e.g. '%s/BP_MyBlueprint'. "
                 "The path must end with the asset name like '/Game/Blueprints/BP_Gun'."),
            *AssetPath, *AssetPath));
}
```

### Also add path validation in HandleBlueprintCreate (line ~23563):

After the path extraction but before building the write request, add a fast-fail check:

```cpp
// Extract path
FString AssetPath;
if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
{
    return FOliveToolResult::Error(
        TEXT("VALIDATION_MISSING_PARAM"),
        TEXT("Required parameter 'path' is missing or empty"),
        TEXT("Provide the Blueprint asset path (e.g., '/Game/Blueprints/BP_NewActor')")
    );
}

// ---- ADD THIS BLOCK ----
// Early detection: path looks like a folder (no asset name after last /)
{
    FString ShortName = FPackageName::GetShortName(AssetPath);
    if (ShortName.IsEmpty() || AssetPath.EndsWith(TEXT("/")))
    {
        return FOliveToolResult::Error(
            TEXT("VALIDATION_PATH_IS_FOLDER"),
            FString::Printf(TEXT("'%s' is a folder path, not an asset path. "
                "The path must end with a Blueprint name."), *AssetPath),
            FString::Printf(TEXT("Use a path like '%s/BP_MyBlueprint' — "
                "the last segment must be the Blueprint asset name (e.g., BP_Gun, BP_Projectile)."),
                *AssetPath)
        );
    }
}
// ---- END BLOCK ----
```

---

## Fix 3: Restructure the Routing Wrapper Prompt

**File:** `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp` (around line 72904)

The current wrapper prompt is a wall of numbered steps. The AI clearly isn't absorbing it. Replace it with a tighter, example-driven format.

### Current prompt (replace this):
```cpp
const FString Wrapper =
    TEXT("You have Olive AI Studio MCP tools. Use them to complete this task.\n\n")
    TEXT("REQUIRED WORKFLOW:\n")
    TEXT("1. blueprint.create — create any needed Blueprints (use /Game/Blueprints/BP_ prefix)\n")
    TEXT("2. blueprint.add_component — add components to each Blueprint\n")
    // ... etc
```

### New prompt:

```cpp
const FString Wrapper =
    TEXT("You have Olive AI Studio MCP tools for Unreal Engine.\n\n")

    TEXT("## CRITICAL RULES\n")
    TEXT("- Asset paths MUST end with the asset name: /Game/Blueprints/BP_Gun (NOT /Game/Blueprints/)\n")
    TEXT("- For graph logic, ALWAYS use blueprint.apply_plan_json (schema_version \"2.0\"). NEVER use individual add_node/connect_pins.\n")
    TEXT("- Do NOT read before creating. Create first, then read to verify.\n\n")

    TEXT("## WORKFLOW (follow this exact order)\n")
    TEXT("Step 1: blueprint.create  →  path: \"/Game/Blueprints/BP_Gun\", parent_class: \"Actor\"\n")
    TEXT("Step 2: blueprint.add_component  →  add StaticMesh, Collision, etc.\n")
    TEXT("Step 3: blueprint.add_variable  →  type names: Float, Boolean, Vector, \"TSubclassOf<Actor>\"\n")
    TEXT("Step 4: blueprint.apply_plan_json  →  one atomic call for ALL graph logic\n")
    TEXT("Step 5: blueprint.read  →  verify result\n\n")

    TEXT("## PLAN JSON EXAMPLE (schema_version 2.0)\n")
    TEXT("{\n")
    TEXT("  \"schema_version\": \"2.0\",\n")
    TEXT("  \"target_graph\": \"EventGraph\",\n")
    TEXT("  \"steps\": [\n")
    TEXT("    {\"id\": \"s1\", \"op\": \"event\", \"target\": \"BeginPlay\"},\n")
    TEXT("    {\"id\": \"s2\", \"op\": \"call\", \"target\": \"PrintString\", ")
    TEXT("\"inputs\": {\"InString\": \"Hello World\"}, \"exec_after\": \"s1\"}\n")
    TEXT("  ]\n")
    TEXT("}\n\n")

    TEXT("Data wires: @step_id.auto (type-match), @step_id.~hint (fuzzy), @step_id.PinName (exact)\n")
    TEXT("Exec flow: \"exec_after\": \"s1\" chains execution. Multiple: \"exec_after\": [\"s1\", \"s2\"]\n")
    TEXT("Available ops: call, get_var, set_var, branch, sequence, cast, event, custom_event, ")
    TEXT("for_loop, for_each_loop, delay, is_valid, print_string, spawn_actor, ")
    TEXT("make_struct, break_struct, return\n\n")

    TEXT("USER REQUEST: ");
```

### Key changes:
- **Critical rules FIRST** — the path format and Plan JSON requirement are called out before workflow
- **Concrete parameter examples** — instead of "use /Game/Blueprints/BP_ prefix", shows the exact parameter format
- **Inline Plan JSON example** — a working example right in the prompt, not just a reference to "your system prompt"
- **Shorter** — less text means better absorption by the LLM

---

## Fix 4: Add a Complete Tool-Call Sequence Example to Worker_Blueprint.txt

**File:** `Content/SystemPrompts/Worker_Blueprint.txt`

This is the `--append-system-prompt` content. Add a concrete "golden path" example showing the exact sequence of MCP tool calls for a common task. This is the right kind of few-shot example — not a finished blueprint, but the *process* of building one.

### Append this section:

```
## REFERENCE: Complete Tool-Call Sequence

Below is the exact sequence of MCP tool calls to create a simple projectile Blueprint.
Follow this pattern for all Blueprint creation tasks.

### Task: "Create a BP_Bullet that moves forward and destroys itself after 3 seconds"

CALL 1: blueprint.create
  {"path": "/Game/Blueprints/BP_Bullet", "parent_class": "Actor"}
  → RESULT: success, asset_path: "/Game/Blueprints/BP_Bullet"

CALL 2: blueprint.add_component
  {"path": "/Game/Blueprints/BP_Bullet", "component_class": "SphereComponent", "name": "CollisionSphere"}
  → RESULT: success

CALL 3: blueprint.add_component
  {"path": "/Game/Blueprints/BP_Bullet", "component_class": "ProjectileMovementComponent", "name": "ProjectileMovement"}
  → RESULT: success

CALL 4: blueprint.add_variable
  {"path": "/Game/Blueprints/BP_Bullet", "name": "BulletSpeed", "type": "Float", "default_value": "3000.0"}
  → RESULT: success

CALL 5: blueprint.apply_plan_json
  {
    "path": "/Game/Blueprints/BP_Bullet",
    "plan": {
      "schema_version": "2.0",
      "target_graph": "EventGraph",
      "steps": [
        {"id": "s1", "op": "event", "target": "BeginPlay"},
        {"id": "s2", "op": "call", "target": "SetLifeSpan", "inputs": {"InLifespan": "3.0"}, "exec_after": "s1"}
      ]
    }
  }
  → RESULT: success, 2 nodes created, 1 exec connection

CALL 6: blueprint.read
  {"path": "/Game/Blueprints/BP_Bullet", "detail": "summary"}
  → RESULT: verify components, variables, and graph nodes are correct

DONE. Report results to user.

### Common Mistakes to Avoid
- ❌ blueprint.create with path "/Game/Blueprints" (FOLDER, not asset — append BP name)
- ❌ blueprint.read_event_graph BEFORE blueprint.create (asset doesn't exist yet)
- ❌ Using blueprint.add_node + blueprint.connect_pins instead of blueprint.apply_plan_json
- ❌ Spending time searching for assets that don't exist yet
```

---

## Fix 5: Add Path-Format Self-Correction Hint

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

When the AI gets a path error, the self-correction system feeds the error back. But the current error messages don't contain enough information for the AI to self-correct. Add a `self_correction_hint` to path-related errors.

In the `HandleBlueprintCreate` function, after the new path validation block from Fix 2, ensure the error result includes the hint field:

```cpp
// In the validation block for folder paths:
FOliveToolResult Result = FOliveToolResult::Error(
    TEXT("VALIDATION_PATH_IS_FOLDER"),
    FString::Printf(TEXT("'%s' is a folder path, not an asset path."), *AssetPath),
    FString::Printf(TEXT("Append a Blueprint name: '%s/BP_MyBlueprint'"), *AssetPath)
);
Result.Data->SetStringField(TEXT("self_correction_hint"),
    FString::Printf(TEXT("You used '%s' as the path. This is a folder. "
        "Add the Blueprint name to the end, like '%s/BP_Gun'. "
        "Then retry blueprint.create with the corrected path."),
        *AssetPath, *AssetPath));
return Result;
```

---

## Fix 6: Consider Switching Provider from Claude Code CLI to Anthropic API

**Why it matters:**

The Claude Code CLI path goes:

```
Olive → spawn claude CLI → CLI thinks → CLI calls MCP → mcp-bridge.js → HTTP → MCP server → tool → result → HTTP → bridge → CLI → CLI thinks again → next tool call
```

Each round-trip adds ~10-30s of latency. For a task requiring 6 tool calls, that's 1-3 minutes of *pure overhead*.

The Anthropic provider path goes:

```
Olive → HTTP API call → streaming response → tool_use block parsed → tool dispatched in-process → tool_result sent back → next turn
```

Tool dispatch happens directly in the game thread via `FOliveToolRegistry::ExecuteTool()` — no subprocess, no MCP bridge, no HTTP round-trip for each tool.

### How to switch:

The `FOliveAnthropicProvider` already exists and handles the full tool-use loop (streaming SSE parsing of `tool_use` blocks at line 72089, tool result assembly at line 71877). The user just needs to:

1. Set `Provider` to `Anthropic` in Project Settings > Plugins > Olive AI Studio
2. Set their Anthropic API key
3. Set `SelectedModel` to `claude-sonnet-4-20250514` (or whichever model)

### What needs attention:

The Anthropic provider needs the system prompt to include tool definitions. Check that `FOlivePromptAssembler` is correctly injecting available tools into the system prompt when using the Anthropic provider (versus relying on Claude Code's MCP tool discovery). Look at how tool schemas are serialized for the API's `tools` parameter in the Anthropic provider's `SendRequest` method.

---

## Fix 7: Add a "Blueprint Quick Start" to blueprint_authoring.txt Knowledge Pack

**File:** `Content/SystemPrompts/Knowledge/blueprint_authoring.txt`

This knowledge pack is injected into the system prompt for both "Auto" and "Blueprint" profiles. Currently it has "8 reliability rules" but no concrete guidance on the creation workflow. Add a condensed quick-reference:

```
## Blueprint Creation Quick Reference

ALWAYS use this workflow order:
1. CREATE the Blueprint (path must end with asset name: /Game/Blueprints/BP_Name)
2. ADD components (StaticMeshComponent, CapsuleComponent, etc.)
3. ADD variables (types: Float, Boolean, Vector, Int, String, Object references)
4. BUILD graph logic with blueprint.apply_plan_json (schema 2.0) — one call for all logic
5. VERIFY with blueprint.read

PATH FORMAT:
  ✓ /Game/Blueprints/BP_Gun
  ✓ /Game/Characters/BP_Enemy
  ✗ /Game/Blueprints          ← This is a FOLDER, not an asset
  ✗ /Game/Blueprints/         ← Trailing slash = folder
  ✗ Blueprints/BP_Gun         ← Must start with /Game/

VARIABLE TYPES (use these exact strings):
  Float, Boolean, Int, String, Vector, Rotator, Transform,
  "TSubclassOf<Actor>", "TArray<Float>", Name, Text, Color

PLAN JSON OPS:
  event, custom_event, call, get_var, set_var, branch, sequence,
  for_loop, for_each_loop, delay, spawn_actor, print_string,
  is_valid, cast, make_struct, break_struct, return
```

---

## Priority Order

| Priority | Fix | Effort | Impact |
|----------|-----|--------|--------|
| **P0** | Fix 1: Enable Plan JSON by default | 1 line | AI can actually use the tools the prompt tells it to use |
| **P0** | Fix 2: Path validation errors | ~20 lines | Eliminates the #1 failure mode |
| **P1** | Fix 3: Restructure wrapper prompt | ~30 lines | AI absorbs the workflow and follows it |
| **P1** | Fix 4: Tool-call sequence example | ~50 lines txt | AI has a concrete pattern to follow |
| **P2** | Fix 5: Self-correction hints | ~10 lines | AI recovers faster from errors |
| **P2** | Fix 7: Knowledge pack update | ~25 lines txt | Reinforces rules from a second location in the prompt |
| **P3** | Fix 6: Switch to Anthropic provider | Config change | 5-10x faster tool loop (but requires API key) |

Fixes 1-4 together should dramatically improve success rate. Fix 6 addresses speed.
