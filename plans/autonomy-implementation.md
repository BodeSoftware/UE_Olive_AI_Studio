# Autonomy & Improvisation Implementation Design

## Overview

This design implements the 6 priorities from `plans/better-autonomy.md`. The core philosophy: remove invisible constraints that prevent the AI from improvising, add one escape-hatch tool (Python), fix one blocking bug (interface function resolution), and reframe prompt language from prescriptive to descriptive.

---

## Priority 1: Prompt Philosophy Rewrite

**Goal:** Remove the invisible constraint that makes the AI treat the tool vocabulary as the boundary of what is possible.

### Files to modify

| File | Change |
|------|--------|
| `Content/SystemPrompts/Knowledge/blueprint_authoring.txt` | Rewrite |
| `Content/SystemPrompts/Knowledge/node_routing.txt` | Rewrite |
| `Content/SystemPrompts/Knowledge/cli_blueprint.txt` | Rewrite |
| `Content/SystemPrompts/Knowledge/recipe_routing.txt` | Minor edit |
| `Content/SystemPrompts/Base.txt` | Minor edit |

### Files NOT to modify

- `Content/SystemPrompts/BaseSystemPrompt.txt` -- This is the base template with `{CONTEXT_PLACEHOLDER}` and `{TOOLS_PLACEHOLDER}`. It is loaded into `BasePromptTemplate` in the C++ `LoadPromptTemplates()` function and has variable substitution applied. Do NOT touch it.
- `Content/SystemPrompts/Worker_Blueprint.txt` -- This is the worker prompt for the Brain Layer's state machine (used by `AssembleWorkerPrompt()`). It has a different audience (the orchestrated worker, not the autonomous agent). The Brain Layer path is NOT the primary path anymore (autonomous MCP mode is default). Do NOT rewrite this.
- `Content/SystemPrompts/ProfileBlueprint.txt` -- Focus profile prompt addition. Leave as-is.
- Any `.cpp` or `.h` files -- No code changes for Priority 1.

### Change 1.1: `Content/SystemPrompts/Knowledge/blueprint_authoring.txt`

Replace the ENTIRE file with this content:

```
## Blueprint Authoring

You have three ways to build Blueprint graphs. Use whichever fits.

### plan_json (blueprint.apply_plan_json, schema_version "2.0")
Batch operation. Describe intent, get automatic pin resolution and wiring.
Ops: event, custom_event, call, call_delegate, call_dispatcher, bind_dispatcher,
  get_var, set_var, branch, sequence, cast, for_loop, for_each_loop,
  while_loop, do_once, flip_flop, gate, delay, is_valid, print_string,
  spawn_actor, make_struct, break_struct, return, comment

Best for: standard logic (3+ nodes), function bodies, event graph flows.
Use this ~80% of the time for graph edits.

### Granular tools (blueprint.add_node, connect_pins, set_pin_default)
Place any UK2Node by class name, wire manually. Returns pin manifest.
Best for: node types not in plan_json ops, 1-2 node edits, fixing specific wires.
Examples: K2Node_Timeline, K2Node_Select, K2Node_SwitchEnum, K2Node_EnhancedInputAction.

### editor.run_python
Execute Python directly in UE's editor scripting context. Full access to the `unreal` module.
Best for: anything the other tools cannot express. Properties that reflection cannot set.
Node types that need special configuration. Bulk operations across assets. Querying project state.

### Choosing your approach
- plan_json for speed when the ops cover what you need.
- Granular tools for specific node types or small edits.
- Python for anything else.
- You can mix approaches within a single task. Use plan_json for the bulk, then add_node or Python for the parts plan_json cannot express.
- Never simplify your design to fit a tool's limitations. Use a different tool instead.

### Reliability rules
1. Read before write: use blueprint.read before modifying an existing Blueprint.
2. Variable types: use concrete UE types. Object refs need class_name.
3. Dispatchers: use blueprint.add_function with function_type="event_dispatcher".
4. After structural edits, compile and inspect errors. Fix the FIRST error.
5. Self-correct: if an operation fails, analyze the error and try a different approach.
6. No fake success: report failures truthfully. If something cannot be done, say so.

### Quick reference
PATH FORMAT: /Game/Blueprints/BP_Name (must start with /Game/, must end with asset name)
VARIABLE TYPES: Float, Boolean, Int, String, Vector, Rotator, Transform, Name, Text, Color
  Object ref: {"category":"object","class_name":"Actor"}
  Class ref: {"category":"class","class_name":"Actor"}
  Array: {"category":"array","value_type":{"category":"float"}}

### Multi-asset tasks
Complete one asset at a time (structure + graph logic + compile) before starting the next.
```

### Change 1.2: `Content/SystemPrompts/Knowledge/node_routing.txt`

Replace the ENTIRE file with this content:

```
## Graph Editing Tools

Three approaches for adding logic to Blueprints. Pick whichever fits the task.

### plan_json (blueprint.apply_plan_json, schema_version "2.0")
Declarative: describe intent, automatic pin resolution and wiring.
Ops: event, custom_event, call, call_delegate, call_dispatcher, bind_dispatcher,
  get_var, set_var, branch, sequence, cast, for_loop, for_each_loop,
  while_loop, do_once, flip_flop, gate, delay, is_valid, print_string,
  spawn_actor, make_struct, break_struct, return, comment

Best for: standard logic (3+ nodes), function bodies, event graph flows.

### add_node (blueprint.add_node)
Imperative: place a specific UK2Node by class name, wire manually.
Works with ANY UK2Node subclass -- not limited to the plan_json ops list.
Examples: K2Node_Timeline, K2Node_Select, K2Node_SwitchEnum, K2Node_SwitchString,
  K2Node_EnhancedInputAction, K2Node_Message (interface calls), animation nodes.

Pass `type` (curated name or exact class) + `properties` (UPROPERTY key-value pairs).
Returns pin manifest. Follow up with connect_pins / set_pin_default.

### editor.run_python
Execute Python in UE's editor scripting context. The `unreal` module gives full access
to editor APIs. Use when neither plan_json nor add_node can express what you need.

### get_node_pins (blueprint.get_node_pins)
Re-inspect a node's pins after property changes that reconstruct it.
Use when: set_node_property changes pin layout (e.g., adding array inputs).

### Your UE5 knowledge is valid
You know Unreal Engine. If you know a node type, function, or pattern exists, try it.
Don't limit yourself to what's documented in recipes or alias maps.
If you know K2Node_Message is the right node for an interface call, use add_node to create it.
```

### Change 1.3: `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

Replace the ENTIRE file with this content:

```
You are an Unreal Engine 5.5 Blueprint specialist.
Think through the complete design before calling tools.

## Approach
You have three ways to build Blueprint graphs:
1. plan_json — Batch. Creates entire function graphs in one call. Efficient for common patterns.
2. Granular tools — add_node, connect_pins, set_pin_default. Works with any UK2Node subclass.
3. editor.run_python — Python in UE's editor context. Full access to the unreal module.

Use whichever fits the task. Mix approaches freely within a single task.
Never simplify your design to fit a tool's limitations — use a different tool instead.

## Workflows
CREATE new Blueprint:
1. Check if a template fits → blueprint.create with template_id
2. If no template: blueprint.create → add_component/add_variable → apply_plan_json

MODIFY existing Blueprint:
1. project.search (find path) → blueprint.read → add_variable/add_component → apply_plan_json

SMALL EDIT (1-2 nodes): blueprint.read → add_node + connect_pins
MULTI-ASSET: Complete one asset fully (structure + logic + compile) before starting the next.

## Plan JSON (v2.0)
```json
{"schema_version":"2.0","steps":[
  {"step_id":"evt","op":"event","target":"BeginPlay"},
  {"step_id":"tf","op":"make_struct","target":"Transform","inputs":{"Location":"0,0,100"}},
  {"step_id":"spawn","op":"spawn_actor","target":"Actor","inputs":{"SpawnTransform":"@tf.auto"},"exec_after":"evt"},
  {"step_id":"print","op":"call","target":"PrintString","inputs":{"InString":"Done"},"exec_after":"spawn"}
]}
```

## Ops
event, custom_event, call, call_delegate, call_dispatcher, bind_dispatcher, get_var, set_var, branch, sequence, cast, for_loop, for_each_loop, while_loop, do_once, flip_flop, gate, delay, is_valid, print_string, spawn_actor, make_struct, break_struct, return, comment

## Wires
Data: @step.auto (type-match, use ~80%), @step.~hint (fuzzy), @step.PinName (exact)
Literals: "InString":"Hello" (no @ = pin default)
Component refs: @ComponentName auto-expands to get_var
Exec: exec_after:"step_id" chains execution. exec_outputs:{"True":"a","False":"b"} for Branch/Cast.
exec_after and exec_outputs are MUTUALLY EXCLUSIVE on the source step.

## Function Graphs vs EventGraph
- EventGraph: op:"event" with target:"BeginPlay"/"Tick"/etc.
- Component events: {"op":"event","target":"OnComponentBeginOverlap","properties":{"component_name":"CollisionComp"}}
- Function graphs: entry node is auto-created. Impure steps MUST have exec_after.

## Function Resolution
Use natural names for op:call. K2_ prefixes and aliases resolve automatically.
Examples: Destroy→K2_DestroyActor, Print→PrintString, GetWorldTransform→K2_GetComponentToWorld
Interface functions: use target_class with the interface name (e.g., "BPI_Interactable").

## Variable Types
Shorthand: "Float", "Boolean", "Integer", "String", "Vector", "Rotator", "Transform"
Object ref: {"category":"object","class_name":"Actor"}
Blueprint ref: {"category":"object","class_name":"BP_Gun_C"} (append _C)
Class ref: {"category":"class","class_name":"Actor"}
Array: {"category":"array","value_type":{"category":"float"}}

## Templates
Templates are reference material, not scripts. They describe common architectures.
Read a template to understand the pattern, then decide your own approach.
- blueprint.create with template_id creates structure (components, variables, function stubs).
- After template creation, write your own plan_json for each function.

## Rules
- Complete the FULL task. Create structures, wire graphs, compile, verify.
- Fix compile errors before declaring done.
- Self-correct on errors. If one approach fails, try a different tool or technique.
- If something genuinely cannot be done with available tools, tell the user what you built, what you could not do, and why.
- Prefer apply_plan_json for 3+ nodes. For 1-2 nodes, add_node + connect_pins.
- If apply_plan_json returns wiring errors, fix with granular connect_pins rather than re-planning.
- Component classes: StaticMeshComponent, SphereComponent, BoxComponent, CapsuleComponent, ArrowComponent, ProjectileMovementComponent, SceneComponent, AudioComponent
```

### Change 1.4: `Content/SystemPrompts/Knowledge/recipe_routing.txt`

Replace the ENTIRE file with this content:

```
## Routing
- olive.get_recipe(query) has tested wiring patterns. Use it when unsure about a pattern, but skip it for simple/well-known operations.
- NEW blueprint: check templates first (blueprint.create with template_id). If none fit: create + components/variables + apply_plan_json
- MODIFY existing: project.search (find path) → blueprint.read → write tools
- SMALL EDIT (1-2 nodes): blueprint.read → add_node + connect_pins
- MULTI-ASSET: complete one asset at a time before starting the next.
- FIX wiring: use wiring_errors and pin_manifests from the apply result
- Prefer plan_json for 3+ nodes. Use add_node/connect_pins for small edits or when the needed operation is not in the plan vocabulary. Use editor.run_python when neither covers what you need.
- Split complex logic into multiple functions when it makes architectural sense.
```

### Change 1.5: `Content/SystemPrompts/Base.txt`

Replace the ENTIRE file with this content:

```
## Common Rules

These rules apply to all operations regardless of domain:

1. **Read before write.** Always examine existing state before modifying an asset.
2. **Chain operations.** Complete the full task. Execute all necessary steps.
3. **Compile after changes.** Trigger compilation when finished writing to a Blueprint. Check compile results.
4. **Report results.** End with a clear summary of assets created/modified and any issues encountered.
5. **Self-correct on errors.** If an operation fails, analyze the error and try a different approach. Don't repeat the same failing operation.
6. **One asset at a time.** Finish all operations on one asset before moving to the next.
7. **Use exact names.** Variable names, function names, and asset paths are case-sensitive.
8. **Respect existing structure.** Don't delete or overwrite things the user did not ask to change.
9. **No fake success.** Never claim a node was created or a pin was connected unless tool results confirm it.
10. **Wire the logic.** When creating Blueprints, implement the actual logic. The user should not need to open the Blueprint editor to finish your work.
11. **Communicate limitations.** If something genuinely cannot be done with available tools, say what you built, what you could not do, and why — instead of silently producing an incomplete result.
```

---

## Priority 2: `editor.run_python` Tool

**Goal:** Add a single tool that gives the AI access to UE's Python editor scripting API, covering any gap in the tool vocabulary.

### Files to create

| File | Purpose |
|------|---------|
| `Source/OliveAIEditor/Python/Public/MCP/OlivePythonToolHandlers.h` | Tool handler class declaration |
| `Source/OliveAIEditor/Python/Private/MCP/OlivePythonToolHandlers.cpp` | Tool handler implementation |
| `Source/OliveAIEditor/Python/Public/MCP/OlivePythonSchemas.h` | Schema declaration |
| `Source/OliveAIEditor/Python/Private/MCP/OlivePythonSchemas.cpp` | Schema implementation |

### Files to modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/OliveAIEditor.Build.cs` | Add "Python" sub-module + PythonScriptPlugin dependency |
| `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp` | Register Python tools during startup |

### Change 2.1: Build.cs — Add Python sub-module

In `OliveAIEditor.Build.cs`, add `"Python"` to the `SubModules` array (line 14):

```csharp
string[] SubModules = { "Blueprint", "BehaviorTree", "PCG", "Cpp", "CrossSystem", "Brain", "Python" };
```

Add `"PythonScriptPlugin"` to `PrivateDependencyModuleNames`. Place it after `"InputBlueprintNodes"` (the last entry, around line 117):

```csharp
// Python scripting (editor.run_python tool)
"PythonScriptPlugin"
```

**IMPORTANT:** `PythonScriptPlugin` is added as a hard compile-time dependency. The module ships with UE 5.5 as a built-in engine plugin and is always available for compilation, even when the plugin is not enabled at runtime. The runtime availability check is `IPythonScriptPlugin::Get() != nullptr` (module loaded) followed by `->IsPythonAvailable()` (Python support compiled in). This is the standard UE pattern for optional engine plugin dependencies.

**CRITICAL API NOTE (verified from UE 5.5 source):**
- There is NO `IPythonScriptPlugin::IsAvailable()` static method. Use `IPythonScriptPlugin::Get()` which returns `nullptr` if the module is not loaded.
- `IsPythonAvailable()` is an instance method that checks if Python support is compiled in.
- `FPythonLogOutputEntry::Type` is `EPythonLogOutputType` (Info/Warning/Error), NOT `SeverityType`/`EMessageSeverity`.
- `ExecutionMode` should be `EPythonCommandExecutionMode::ExecuteFile` for multi-line scripts (not `ExecuteStatement` which is for single statements).
- `FPythonCommandEx` default `ExecutionMode` is already `ExecuteFile`, so we do not need to set it explicitly.

### Change 2.2: `Source/OliveAIEditor/Python/Public/MCP/OlivePythonSchemas.h`

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace OlivePythonSchemas
{
    /** Schema for editor.run_python tool */
    TSharedPtr<FJsonObject> EditorRunPython();
}
```

### Change 2.3: `Source/OliveAIEditor/Python/Private/MCP/OlivePythonSchemas.cpp`

Follow the exact schema helper pattern from `OliveBlueprintSchemas.cpp` (the `MakeSchema`, `StringProp`, `BoolProp`, `AddRequired` helper functions). Since these are in an anonymous namespace in BlueprintSchemas, the Python schemas file needs its own copies. OR -- since they are simple enough -- just inline the JSON construction.

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "OlivePythonSchemas.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace OlivePythonSchemas
{
    static TSharedPtr<FJsonObject> MakeSchema(const FString& Type)
    {
        TSharedPtr<FJsonObject> Schema = MakeShareable(new FJsonObject());
        Schema->SetStringField(TEXT("type"), Type);
        return Schema;
    }

    static TSharedPtr<FJsonObject> StringProp(const FString& Description)
    {
        TSharedPtr<FJsonObject> Prop = MakeSchema(TEXT("string"));
        Prop->SetStringField(TEXT("description"), Description);
        return Prop;
    }

    static void AddRequired(TSharedPtr<FJsonObject> Schema, const TArray<FString>& RequiredFields)
    {
        TArray<TSharedPtr<FJsonValue>> RequiredArray;
        for (const FString& Field : RequiredFields)
        {
            RequiredArray.Add(MakeShareable(new FJsonValueString(Field)));
        }
        Schema->SetArrayField(TEXT("required"), RequiredArray);
    }

    TSharedPtr<FJsonObject> EditorRunPython()
    {
        TSharedPtr<FJsonObject> Properties = MakeShareable(new FJsonObject());

        Properties->SetObjectField(TEXT("script"),
            StringProp(TEXT("Python script to execute in UE's editor scripting context. "
                "The 'unreal' module is available with full access to editor APIs "
                "(e.g., unreal.EditorAssetLibrary, unreal.BlueprintEditorLibrary). "
                "Scripts are wrapped in try/except automatically. "
                "Use print() for output — stdout is captured and returned.")));

        TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
        Schema->SetStringField(TEXT("description"),
            TEXT("Execute Python in the Unreal Editor's scripting context. "
                "Full access to the 'unreal' module and all editor APIs. "
                "A snapshot is taken automatically before execution for rollback safety. "
                "Use when standard Blueprint/BT/PCG/C++ tools cannot express what you need."));
        Schema->SetObjectField(TEXT("properties"), Properties);
        AddRequired(Schema, {TEXT("script")});

        return Schema;
    }
}
```

### Change 2.4: `Source/OliveAIEditor/Python/Public/MCP/OlivePythonToolHandlers.h`

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePythonTools, Log, All);

/**
 * FOlivePythonToolHandlers
 *
 * Registers and handles the editor.run_python MCP tool.
 * Bridges UE's PythonScriptPlugin API (IPythonScriptPlugin::ExecPythonCommandEx)
 * with the Olive tool registry.
 *
 * Safety layers:
 * 1. Automatic snapshot via FOliveSnapshotManager before every execution
 * 2. Persistent script logging to Saved/OliveAI/PythonScripts.log
 * 3. try/except wrapper around every script
 * 4. Timeout (configurable, default 30s) -- Note: FPythonCommandEx does not
 *    natively support cancellation, so timeout is advisory via log warning.
 */
class OLIVEAIEDITOR_API FOlivePythonToolHandlers
{
public:
    /** Get singleton instance */
    static FOlivePythonToolHandlers& Get();

    /** Register all Python tools with the tool registry */
    void RegisterAllTools();

    /** Unregister all Python tools */
    void UnregisterAllTools();

private:
    FOlivePythonToolHandlers() = default;

    FOlivePythonToolHandlers(const FOlivePythonToolHandlers&) = delete;
    FOlivePythonToolHandlers& operator=(const FOlivePythonToolHandlers&) = delete;

    /** Handle the editor.run_python tool call */
    FOliveToolResult HandleRunPython(const TSharedPtr<FJsonObject>& Params);

    /** Log a script execution to the persistent log file */
    void LogScriptExecution(const FString& Script, bool bSuccess, const FString& Output);

    /** Get the log file path */
    FString GetScriptLogPath() const;

    /** Wrap a script in try/except for safe execution */
    FString WrapScript(const FString& RawScript) const;

    TArray<FString> RegisteredToolNames;
};
```

### Change 2.5: `Source/OliveAIEditor/Python/Private/MCP/OlivePythonToolHandlers.cpp`

This is the core implementation. Key details:

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "OlivePythonToolHandlers.h"
#include "OlivePythonSchemas.h"
#include "CrossSystem/OliveSnapshotManager.h"
#include "MCP/OliveToolRegistry.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"

// PythonScriptPlugin includes
#include "IPythonScriptPlugin.h"

DEFINE_LOG_CATEGORY(LogOlivePythonTools);
```

**Singleton:**
```cpp
FOlivePythonToolHandlers& FOlivePythonToolHandlers::Get()
{
    static FOlivePythonToolHandlers Instance;
    return Instance;
}
```

**RegisterAllTools:**
```cpp
void FOlivePythonToolHandlers::RegisterAllTools()
{
    UE_LOG(LogOlivePythonTools, Log, TEXT("Registering Python MCP tools..."));

    FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

    Registry.RegisterTool(
        TEXT("editor.run_python"),
        TEXT("Execute Python in UE's editor scripting context. Full access to the unreal module. "
             "Use when standard tools cannot express what you need."),
        OlivePythonSchemas::EditorRunPython(),
        FOliveToolHandler::CreateRaw(this, &FOlivePythonToolHandlers::HandleRunPython),
        {TEXT("python"), TEXT("editor"), TEXT("write")},
        TEXT("editor")
    );
    RegisteredToolNames.Add(TEXT("editor.run_python"));

    UE_LOG(LogOlivePythonTools, Log, TEXT("Registered %d Python MCP tools"), RegisteredToolNames.Num());
}
```

**UnregisterAllTools** -- follow the CppToolHandlers pattern exactly.

**HandleRunPython** -- this is the critical implementation:

```cpp
FOliveToolResult FOlivePythonToolHandlers::HandleRunPython(const TSharedPtr<FJsonObject>& Params)
{
    // 1. Validate parameter
    FString Script;
    if (!Params->TryGetStringField(TEXT("script"), Script))
    {
        return FOliveToolResult::Error(
            TEXT("VALIDATION_MISSING_PARAM"),
            TEXT("Missing required parameter 'script'"),
            TEXT("Provide the Python script to execute")
        );
    }

    if (Script.TrimStartAndEnd().IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("VALIDATION_EMPTY_SCRIPT"),
            TEXT("Script is empty"),
            TEXT("Provide a non-empty Python script")
        );
    }

    // 2. Check if PythonScriptPlugin is available
    // IPythonScriptPlugin::Get() returns nullptr if the module is not loaded.
    // IsPythonAvailable() checks if Python support is actually compiled in.
    IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
    if (!PythonPlugin)
    {
        return FOliveToolResult::Error(
            TEXT("PYTHON_PLUGIN_NOT_AVAILABLE"),
            TEXT("The Python Editor Script Plugin is not enabled. "
                 "Enable it in Edit > Plugins > Scripting > Python Editor Script Plugin, then restart the editor."),
            TEXT("Enable the Python Editor Script Plugin in Project Settings")
        );
    }

    if (!PythonPlugin->IsPythonAvailable())
    {
        return FOliveToolResult::Error(
            TEXT("PYTHON_NOT_AVAILABLE"),
            TEXT("The Python Editor Script Plugin is loaded but Python support is not available. "
                 "This may indicate a Python installation issue."),
            TEXT("Check that Python 3.x is installed and the PythonScriptPlugin can find it")
        );
    }

    // 3. Take automatic snapshot before execution
    //    Use a simple name to identify Python script snapshots
    {
        TArray<FString> EmptyAssets; // Snapshot with no specific assets = project-wide safety net
        FOliveSnapshotManager::Get().CreateSnapshot(
            TEXT("pre_python_script"),
            EmptyAssets,
            TEXT("Automatic snapshot before editor.run_python execution")
        );
    }

    // 4. Wrap script in try/except
    const FString WrappedScript = WrapScript(Script);

    // 5. Execute via FPythonCommandEx
    FPythonCommandEx PythonCommand;
    PythonCommand.Command = WrappedScript;
    // ExecutionMode defaults to ExecuteFile which handles multi-line scripts.
    // ExecuteStatement is for single statements only.
    // No need to set ExecutionMode explicitly.

    const double StartTime = FPlatformTime::Seconds();
    const bool bSuccess = PythonPlugin->ExecPythonCommandEx(PythonCommand);
    const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

    // 6. Collect output from PythonCommand.LogOutput and CommandResult
    FString OutputText;

    // CommandResult contains the expression result (if any)
    if (!PythonCommand.CommandResult.IsEmpty())
    {
        OutputText += PythonCommand.CommandResult;
    }

    // LogOutput contains print() statements and error messages
    for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
    {
        if (!OutputText.IsEmpty())
        {
            OutputText += TEXT("\n");
        }

        // Prefix errors/warnings for clarity
        // FPythonLogOutputEntry::Type is EPythonLogOutputType (Info/Warning/Error)
        if (Entry.Type == EPythonLogOutputType::Error)
        {
            OutputText += TEXT("[ERROR] ");
        }
        else if (Entry.Type == EPythonLogOutputType::Warning)
        {
            OutputText += TEXT("[WARNING] ");
        }

        OutputText += Entry.Output;
    }

    // 7. Log to persistent file
    LogScriptExecution(Script, bSuccess, OutputText);

    // 8. Build result
    TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
    ResultData->SetBoolField(TEXT("success"), bSuccess);
    ResultData->SetStringField(TEXT("output"), OutputText);
    ResultData->SetNumberField(TEXT("execution_time_ms"), ElapsedMs);

    if (bSuccess)
    {
        return FOliveToolResult::Success(ResultData);
    }
    else
    {
        FOliveToolResult Result = FOliveToolResult::Error(
            TEXT("PYTHON_EXECUTION_FAILED"),
            TEXT("Python script execution failed"),
            TEXT("Check the output for error details. A snapshot was taken before execution for rollback.")
        );
        Result.Data = ResultData;
        return Result;
    }
}
```

**WrapScript:**
```cpp
FString FOlivePythonToolHandlers::WrapScript(const FString& RawScript) const
{
    // Wrap in try/except to prevent editor crashes from Python exceptions.
    // Indent every line of the original script by 4 spaces for the try block.
    FString Indented;
    TArray<FString> Lines;
    RawScript.ParseIntoArrayLines(Lines);
    for (const FString& Line : Lines)
    {
        Indented += TEXT("    ") + Line + TEXT("\n");
    }

    return FString::Printf(
        TEXT("import traceback\ntry:\n%sexcept Exception as e:\n    print('[PYTHON_ERROR] ' + str(e))\n    traceback.print_exc()\n"),
        *Indented
    );
}
```

**LogScriptExecution:**
```cpp
void FOlivePythonToolHandlers::LogScriptExecution(const FString& Script, bool bSuccess, const FString& Output)
{
    const FString LogPath = GetScriptLogPath();

    // Ensure directory exists
    const FString LogDir = FPaths::GetPath(LogPath);
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    PlatformFile.CreateDirectoryTree(*LogDir);

    const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"));
    const FString StatusStr = bSuccess ? TEXT("SUCCESS") : TEXT("FAILED");

    FString LogEntry = FString::Printf(
        TEXT("\n========== [%s] %s ==========\n%s\n---------- Output ----------\n%s\n"),
        *Timestamp, *StatusStr, *Script, *Output
    );

    FFileHelper::SaveStringToFile(
        LogEntry, *LogPath,
        FFileHelper::EEncodingOptions::AutoDetect,
        &IFileManager::Get(),
        EFileWrite::FILEWRITE_Append
    );
}

FString FOlivePythonToolHandlers::GetScriptLogPath() const
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OliveAI"), TEXT("PythonScripts.log"));
}
```

### Change 2.6: Startup Registration

In `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`:

Add include at top (after existing tool handler includes):
```cpp
#include "MCP/OlivePythonToolHandlers.h"
```

Add registration after C++ tools (after line ~234 `UE_LOG(LogOliveAI, Log, TEXT("C++ tools registered"));`):
```cpp
// Register Python tools
FOlivePythonToolHandlers::Get().RegisterAllTools();
UE_LOG(LogOliveAI, Log, TEXT("Python tools registered"));
```

Add to shutdown (if there is a `ShutdownModule()` that unregisters tools -- check existing pattern):
```cpp
FOlivePythonToolHandlers::Get().UnregisterAllTools();
```

### Change 2.7: Focus Profile Visibility

The `editor.run_python` tool is tagged `{python, editor, write}`. It should be visible in **all** focus profiles (Auto, Blueprint, C++). Check how other tags map to profiles:

Looking at the focus profile system, the tool has category `"editor"`. The Auto profile shows all tools. Blueprint and C++ profiles filter by tags. The tag `"editor"` is not currently in any profile's filter set, but the tag `"write"` is commonly used.

**Decision:** The simplest approach is to give it tags that make it visible everywhere: `{TEXT("blueprint"), TEXT("cpp"), TEXT("python"), TEXT("editor"), TEXT("write")}`. This ensures it shows up in Auto (all tools), Blueprint (has "blueprint" tag), and C++ (has "cpp" tag).

Update the tag array in `RegisterAllTools()`:
```cpp
{TEXT("blueprint"), TEXT("cpp"), TEXT("python"), TEXT("editor"), TEXT("write")},
```

### What NOT to do for Priority 2

- Do NOT add size limits on scripts.
- Do NOT add a whitelist/blacklist of Python modules.
- Do NOT add a confirmation dialog -- the snapshot is the safety net.
- Do NOT try to implement timeout/cancellation -- `FPythonCommandEx` does not support it. The try/except wrapper prevents crashes. Log a warning if execution takes >30s.
- Do NOT create a `Content/SystemPrompts/Knowledge/python_scripting.txt` knowledge pack. The AI already knows UE's Python API from training data. The tool description is sufficient.
- Do NOT modify any existing tool handler files.

---

## Priority 3: Interface Function Resolution Fix

**Goal:** Fix the bug where `FindFunction` reports `ExactName` when a function is found on a UInterface class, which prevents the downstream pipeline from creating `UK2Node_Message` nodes.

### Files to modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | Fix Step 1 in `FindFunction` |

### Change 3.1: Fix Step 1 interface class detection

In `OliveNodeFactory.cpp` at line 1907-1922, the Step 1 block finds a function on the specified class and reports `ExactName`. When that class is (or derives from) `UInterface`, it should report `InterfaceSearch` instead so the downstream pipeline creates `UK2Node_Message`.

**Current code** (lines 1907-1922):
```cpp
// --- Step 1: Search specified ClassName ---
if (!ClassName.IsEmpty())
{
    UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s', class='%s'): searching specified class"), *ResolvedName, *ClassName);
    UClass* Class = FindClass(ClassName);
    if (Class)
    {
        UFunction* Func = TryClassWithK2(Class);
        if (Func)
        {
            UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): found in specified class '%s'"), *ResolvedName, *Class->GetName());
            ReportMatch(EOliveFunctionMatchMethod::ExactName);
            return Func;
        }
    }
}
```

**New code** -- replace the `ReportMatch(EOliveFunctionMatchMethod::ExactName)` line:
```cpp
// --- Step 1: Search specified ClassName ---
if (!ClassName.IsEmpty())
{
    UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s', class='%s'): searching specified class"), *ResolvedName, *ClassName);
    UClass* Class = FindClass(ClassName);
    if (Class)
    {
        UFunction* Func = TryClassWithK2(Class);
        if (Func)
        {
            // If the specified class is a UInterface (or its generated class derives from
            // UInterface), report InterfaceSearch so the downstream pipeline creates
            // UK2Node_Message instead of UK2Node_CallFunction.
            if (Class->IsChildOf(UInterface::StaticClass()) ||
                (Class->ClassGeneratedBy && Cast<UBlueprint>(Class->ClassGeneratedBy) &&
                 Cast<UBlueprint>(Class->ClassGeneratedBy)->BlueprintType == BPTYPE_Interface))
            {
                UE_LOG(LogOliveNodeFactory, Log, TEXT("FindFunction('%s'): found in specified interface class '%s' -> InterfaceSearch"), *ResolvedName, *Class->GetName());
                if (OutMatchMethod)
                {
                    *OutMatchMethod = EOliveFunctionMatchMethod::InterfaceSearch;
                }
            }
            else
            {
                UE_LOG(LogOliveNodeFactory, Verbose, TEXT("FindFunction('%s'): found in specified class '%s'"), *ResolvedName, *Class->GetName());
                ReportMatch(EOliveFunctionMatchMethod::ExactName);
            }
            return Func;
        }
    }
}
```

**Why the two-part check:**
1. `Class->IsChildOf(UInterface::StaticClass())` catches native C++ interfaces (e.g., `UDamageInterface`). These have a native UInterface class.
2. The `ClassGeneratedBy` + `BlueprintType == BPTYPE_Interface` check catches Blueprint Interfaces (e.g., `BPI_Interactable`). When a Blueprint Interface asset is loaded, `FindClass("BPI_Interactable")` resolves to its `UBlueprintGeneratedClass`, which is NOT a child of `UInterface` -- it is a child of `UObject`. But its generating Blueprint has `BlueprintType == BPTYPE_Interface`.

**Include needed:** The file already includes `Engine/Blueprint.h` (for `UBlueprint`). The `BPTYPE_Interface` enum is in `Engine/BlueprintCore.h` which is transitively included. Verify `EBlueprintType::BPTYPE_Interface` compiles. If not, the correct enum value is `EBlueprintType::BPTYPE_Interface` from `Engine/Blueprint.h`.

**NOTE:** The `ReportMatch` lambda checks `bUsedAlias` and overrides with `AliasMap` if an alias was used. For interface calls, we MUST NOT use `ReportMatch` because we need `InterfaceSearch` regardless of whether an alias was used. The existing Step 6 (interface search in `ImplementedInterfaces`) already handles this correctly by setting `*OutMatchMethod` directly instead of using `ReportMatch`. We follow the same pattern.

### What NOT to do for Priority 3

- Do NOT modify `FindFunctionEx` -- it delegates to `FindFunction` and just collects search history.
- Do NOT modify the `ReportMatch` lambda. Just bypass it for the interface case.
- Do NOT add a new `EOliveFunctionMatchMethod` enum value. `InterfaceSearch` already exists and is the correct value.
- Do NOT modify the resolver (`OliveBlueprintPlanResolver.cpp`). The resolver already checks `MatchMethod == InterfaceSearch` and sets `bIsInterfaceCall = true`. The fix is entirely in the factory.

---

## Priority 4: Reframe Templates as Context

**Goal:** Rewrite the `pickup_interaction.json` template to be descriptive (architectural) rather than prescriptive (step-by-step).

### Files to modify

| File | Change |
|------|--------|
| `Content/Templates/reference/pickup_interaction.json` | Rewrite to descriptive style |

### Change 4.1: Rewrite pickup_interaction.json

Replace the ENTIRE file content:

```json
{
    "template_id": "pickup_interaction",
    "template_type": "reference",
    "display_name": "Pickup / Interaction Pattern (Blueprint Interface)",

    "catalog_description": "Decoupled pickup and interaction system using a Blueprint Interface. Items handle their own behavior; the player character interacts without knowing specific item types.",
    "catalog_examples": "",

    "tags": "pickup interact interactable equip item collect grab hold weapon loot drop attach detach interface blueprint interface input overlap",

    "patterns": [
        {
            "name": "PickupSystemArchitecture",
            "description": "A pickup system has three assets: a Blueprint Interface (e.g., BPI_Interactable), one or more item Blueprints that implement it, and a player character that calls it. The interface defines an Interact(InteractingActor:Actor) function. Each item implements this interface and handles its own pickup/drop/use logic inside its Interact implementation — the character never needs to know what kind of item it is.",
            "notes": "The character detects nearby items via overlap (SphereComponent or CapsuleComponent on the item), iterates overlapping actors, checks DoesImplementInterface to filter for interactables, and calls Interact as an interface message (UK2Node_Message, not a direct call or cast). This keeps the character completely decoupled from item types — any actor implementing BPI_Interactable is interactive. No casting is needed. If the project already has a player character, modify it rather than creating a new one."
        }
    ]
}
```

**Key changes from the original:**
- Pure architectural description. No tool names, no numbered steps, no build instructions.
- Describes the three-asset structure and their relationships.
- Explicitly states the correct UE pattern: DoesImplementInterface + interface message call, no casting.
- The AI reads this, understands the architecture, and decides its own build approach.
- Line count: ~24 lines (concise, no bloat).

### What NOT to do for Priority 4

- Do NOT rewrite `component_patterns.json`, `ue_events.json`, or `projectile_patterns.json`. They are already reasonably descriptive. Only rewrite if a test shows the AI following them too literally.
- Do NOT add new templates. The plan explicitly says "don't add more recipes for specific patterns."
- Do NOT modify the template system code. Templates are JSON files loaded at startup; no code changes needed.

---

## Priority 5: Reframe plan_json as Optional Fast Path

**Goal:** The prompt language around plan_json should present it as "one efficient way" rather than "what you can do."

This is already handled by Priority 1's prompt rewrites. The new `blueprint_authoring.txt` and `cli_blueprint.txt` present three approaches (plan_json, granular, Python) as equals. The new `node_routing.txt` explicitly says "pick whichever fits" and mentions that add_node "works with ANY UK2Node subclass -- not limited to the plan_json ops list."

### No additional changes needed for Priority 5

The prompt rewrites in Priority 1 fully address this. The old language ("Available plan_json operations: ...") is replaced with framing that presents multiple approaches. The old prescriptive routing ("MUST use plan JSON path") is replaced with "prefer apply_plan_json for 3+ nodes" (a guideline, not a mandate).

---

## Priority 6: Improve Granular Tool Reliability

**Goal:** Make add_node failures produce clear errors with alternatives, and ensure no silent failures.

### Files to modify

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Enhance error messages in HandleBlueprintAddNode |

### Change 6.1: Enhance add_node error messages with alternatives

The `HandleBlueprintAddNode` function (starting at line 3921) already has good error handling for missing params and unknown node types. The key improvements are:

**6.1a: Ghost node detection already exists.** The zero-pin guard was implemented previously. Verify it is present and produces actionable errors. No change needed if it exists.

**6.1b: Add Python fallback suggestion to node creation failures.**

Find the point in `HandleBlueprintAddNode` where node creation fails (where `Factory.CreateNode()` returns nullptr). After the existing error handling, enhance the suggestion to mention Python:

Look for the pattern where `CreateNode` returns nullptr and an error is constructed. The error message should include a suggestion like:

```
TEXT("Node creation failed. If this node type requires special configuration, try editor.run_python with the unreal module.")
```

Specifically, find the code block after `CreateNode` call that handles nullptr return. The existing code likely constructs an error with `Factory.GetLastError()`. Modify the suggestion field to include the Python fallback:

```cpp
// After CreateNode fails:
FString Suggestion = TEXT("Check the type name and properties. ");
if (NodeType.Contains(TEXT("K2Node_")) || NodeType.Contains(TEXT("Input")))
{
    Suggestion += TEXT("For complex node types, try editor.run_python with unreal.BlueprintEditorLibrary or direct node creation via Python.");
}
else
{
    Suggestion += TEXT("Use blueprint.describe_node_type to check available types, or editor.run_python for types not supported by add_node.");
}
```

**6.1c: Add property-set feedback to success result.**

When add_node succeeds, the result already includes pin manifest data. Additionally, if `CreateNodeByClass` was used (the universal fallback), it tracks `OutSetProperties` and `OutSkippedProperties`. These should be included in the result:

Find the success result construction in HandleBlueprintAddNode. Add:
```cpp
// If any properties were skipped during reflection-based setup, include them
// so the AI knows to use set_node_property or editor.run_python for those.
if (SkippedProperties.Num() > 0)
{
    TSharedPtr<FJsonObject> SkippedObj = MakeShareable(new FJsonObject());
    for (const auto& Pair : SkippedProperties)
    {
        SkippedObj->SetStringField(Pair.Key, Pair.Value);
    }
    ResultData->SetObjectField(TEXT("skipped_properties"), SkippedObj);
    ResultData->SetStringField(TEXT("skipped_properties_hint"),
        TEXT("These properties could not be set via reflection. Use blueprint.set_node_property or editor.run_python to set them."));
}
```

**IMPORTANT:** I need to verify whether the current HandleBlueprintAddNode code already tracks skipped properties. Read the full function if needed. If the code calls `Factory.CreateNode()` (not `CreateNodeByClass` directly), the skipped properties may not be available at the handler level. In that case, skip change 6.1c -- it would require plumbing new return data through the factory, which is overengineering for this task.

### What NOT to do for Priority 6

- Do NOT add 500+ validation checks like NeoStack. This is about improving error messages, not adding validation infrastructure.
- Do NOT modify the WriteePipeline or validation engine.
- Do NOT add new error codes. The existing `NODE_TYPE_UNKNOWN`, `BP_ADD_NODE_FAILED`, `GHOST_NODE_PREVENTED` codes are sufficient.
- Do NOT modify `CreateNode`, `CreateNodeByClass`, or any factory internals. Changes are limited to the tool handler's error message construction.

---

## Implementation Order

Tasks are numbered. Dependencies are noted. The coder should work through these in order, but independent tasks can be parallelized.

### Phase A: Code changes (must build together)

| # | Task | Priority | Depends on | Estimated effort |
|---|------|----------|------------|-----------------|
| T1 | Create `Source/OliveAIEditor/Python/Public/MCP/OlivePythonSchemas.h` | P2 | None | 5 min |
| T2 | Create `Source/OliveAIEditor/Python/Private/MCP/OlivePythonSchemas.cpp` | P2 | T1 | 10 min |
| T3 | Create `Source/OliveAIEditor/Python/Public/MCP/OlivePythonToolHandlers.h` | P2 | None | 10 min |
| T4 | Create `Source/OliveAIEditor/Python/Private/MCP/OlivePythonToolHandlers.cpp` | P2 | T1, T2, T3 | 30 min |
| T5 | Modify `OliveAIEditor.Build.cs` (add Python sub-module + dependency) | P2 | None | 5 min |
| T6 | Modify `OliveAIEditorModule.cpp` (register Python tools) | P2 | T3, T4 | 5 min |
| T7 | Fix `FindFunction` Step 1 interface detection in `OliveNodeFactory.cpp` | P3 | None | 10 min |
| T8 | Enhance add_node error messages in `OliveBlueprintToolHandlers.cpp` | P6 | None | 15 min |

**Build after all T1-T8 are complete.** Expected: clean compile.

### Phase B: Content changes (no build needed)

| # | Task | Priority | Depends on | Estimated effort |
|---|------|----------|------------|-----------------|
| T9 | Rewrite `blueprint_authoring.txt` | P1 | None | 5 min |
| T10 | Rewrite `node_routing.txt` | P1 | None | 5 min |
| T11 | Rewrite `cli_blueprint.txt` | P1 | None | 5 min |
| T12 | Rewrite `recipe_routing.txt` | P1 | None | 3 min |
| T13 | Rewrite `Base.txt` | P1 | None | 3 min |
| T14 | Rewrite `pickup_interaction.json` | P4 | None | 5 min |
| T15 | Rewrite remaining prescriptive templates and recipes | P4 | None | 30 min |
| T16 | Rewrite `Worker_Blueprint.txt` | P1 | None | 15 min |

**Phase B tasks are all independent** and can be done in any order. They can run in parallel with Phase A since they are content-only changes.

### Total estimated effort: ~2.5 hours

---

## T15: Rewrite Remaining Prescriptive Templates and Recipes

**Goal:** Apply the same descriptive-not-prescriptive philosophy from T14 to all other reference templates and recipes that currently dictate tool choices.

### Files to modify

#### 15.1: `Content/Templates/reference/component_patterns.json`

**Problem:** Every pattern has `"tool": "blueprint.add_node"` steps with exact class names and properties. Tells the AI "Component bound events cannot be created via plan JSON ops. Use blueprint.add_node directly." This is factually wrong now — plan_json `event` op supports component events via `properties.component_name`.

**Replace entire file with:**

```json
{
    "template_id": "component_patterns",
    "template_type": "reference",
    "display_name": "Component Interaction Patterns",

    "catalog_description": "Common component interaction patterns: collision response, overlap detection, and component variable access.",
    "catalog_examples": "",

    "tags": "component delegate event overlap hit collision target wire variable access bound",

    "patterns": [
        {
            "name": "ComponentEvents",
            "description": "Components like SphereComponent, BoxComponent, and CapsuleComponent have delegate events (OnComponentHit, OnComponentBeginOverlap, OnComponentEndOverlap). These fire when other actors collide with or overlap the component. The component must have the appropriate collision/overlap settings enabled.",
            "notes": "Component events bind to a specific component by name. OnComponentHit provides HitComponent, OtherActor, OtherComp, NormalImpulse, and Hit (FHitResult). OnComponentBeginOverlap provides OverlappedComponent, OtherActor, OtherComp, OtherBodyIndex, bFromSweep, and SweepResult. The component's Generate Overlap Events must be true for overlap events to fire."
        },
        {
            "name": "ComponentVariableAccess",
            "description": "Components added in the Components panel are variables on the Blueprint. Use get_var to retrieve a component reference, then wire it to the Target input of any function call on that component. This is the standard pattern for interacting with components at runtime.",
            "notes": "get_var works for both Blueprint variables and components. set_var does NOT work for components (read-only references). When multiple components of the same type exist, always use get_var with the explicit component name rather than GetComponentByClass."
        }
    ]
}
```

#### 15.2: `Content/Templates/reference/projectile_patterns.json`

**Problem:** Five patterns, each with tool-specific steps (`"tool": "blueprint.add_node"`, inline plan_json code). Tells the AI exactly which tool to use for each pattern.

**Replace entire file with:**

```json
{
    "template_id": "projectile_patterns",
    "template_type": "reference",
    "display_name": "Projectile Behavior Patterns",

    "catalog_description": "Common projectile behaviors: hit response, damage application, bounce counting, homing, and auto-destroy. Use with the projectile factory template.",
    "catalog_examples": "",

    "tags": "projectile hit damage bounce homing lifespan destroy collision overlap apply point damage movement component",

    "patterns": [
        {
            "name": "ProjectileHitResponse",
            "description": "The projectile's collision component (typically CollisionSphere from the factory template) fires OnComponentHit when it strikes something. The hit event provides OtherActor, HitComponent, NormalImpulse, and Hit (FHitResult) for determining what was hit and where.",
            "notes": "A typical hit chain: check OtherActor is valid, apply damage, spawn impact VFX at the hit location, then destroy the projectile. Use ApplyDamage (simple) or ApplyPointDamage (directional, requires HitFromDirection computed from projectile velocity)."
        },
        {
            "name": "ApplyDamageOnHit",
            "description": "UGameplayStatics::ApplyDamage is the simplest damage function. It takes DamagedActor, BaseDamage, EventInstigator (Controller), DamageCauser (the projectile), and an optional DamageTypeClass.",
            "notes": "Get the instigator controller via GetInstigatorController on the projectile. For directional damage use ApplyPointDamage instead, which also requires HitFromDirection (FVector) and HitInfo (FHitResult) from the collision event. Store a damage variable (e.g., BulletDamage) on the projectile and reference it in the damage call."
        },
        {
            "name": "BounceCountingWithDestroy",
            "description": "Track bounces via a counter variable and destroy the projectile after a maximum. The ProjectileMovementComponent fires OnProjectileBounce each time the projectile bounces, providing Hit (FHitResult) and ImpactResult.",
            "notes": "Requires should_bounce=true on the ProjectileMovementComponent. Add BounceCount (Integer, default 0) and MaxBounces (Integer) variables. On each bounce: increment BounceCount, compare to MaxBounces, branch to DestroyActor if exceeded."
        },
        {
            "name": "HomingTargetAssignment",
            "description": "The ProjectileMovementComponent supports homing when bIsHomingProjectile is true. The HomingTargetComponent property must be set to the root component of the target actor at runtime.",
            "notes": "Typically set from the spawning Blueprint immediately after SpawnActor, not inside the projectile. Alternatively, expose a public function on the projectile (e.g., SetTarget) that accepts an Actor, gets its root component, and assigns it to HomingTargetComponent. HomingAccelerationMagnitude controls turn strength."
        },
        {
            "name": "LifespanAutoDestroy",
            "description": "SetLifeSpan in BeginPlay prevents orphaned projectiles from persisting forever. A lifespan of 0 means no auto-destroy.",
            "notes": "Typical values: bullets 2-3s, rockets 5-10s, grenades 3-5s. Can also be called from the spawner after SpawnActor. Simpler than a timer + DestroyActor approach."
        }
    ]
}
```

#### 15.3: `Content/Templates/reference/ue_events.json`

**Problem:** Has `plan_example` blocks with inline plan_json for every event. These are less harmful since they're genuinely reference data (event names + pin signatures), but the examples nudge the AI toward plan_json exclusively.

**Change:** Remove the `plan_example` objects. Keep the event name, description, and pin signature info in `notes`. The AI knows how to create events — it just needs the correct target names and output pin info.

**Replace entire file with:**

```json
{
    "template_id": "ue_events",
    "template_type": "reference",
    "display_name": "UE Actor Event Reference",

    "catalog_description": "Actor lifecycle and interaction events with their output pin signatures.",
    "catalog_examples": "",

    "tags": "event beginplay tick overlap damage hit destroyed endplay actor lifecycle",

    "patterns": [
        {
            "name": "BeginPlay",
            "description": "Fires once when the actor starts playing (after all components are initialized). The most common event for setup logic.",
            "notes": "Target name: ReceiveBeginPlay. No output data pins (exec only)."
        },
        {
            "name": "Tick",
            "description": "Fires every frame while the actor is ticking. The actor must have ticking enabled.",
            "notes": "Target name: ReceiveTick. Output pins: DeltaSeconds (Float)."
        },
        {
            "name": "EndPlay",
            "description": "Fires when the actor is being removed from the world or the game is ending.",
            "notes": "Target name: ReceiveEndPlay. Output pins: EndPlayReason (EEndPlayReason)."
        },
        {
            "name": "ActorBeginOverlap",
            "description": "Fires when another actor begins overlapping this actor. Requires a collision component with Generate Overlap Events enabled.",
            "notes": "Target name: ReceiveActorBeginOverlap. Output pins: OtherActor (Actor)."
        },
        {
            "name": "ActorEndOverlap",
            "description": "Fires when another actor stops overlapping this actor.",
            "notes": "Target name: ReceiveActorEndOverlap. Output pins: OtherActor (Actor)."
        },
        {
            "name": "Hit",
            "description": "Fires when this actor is hit by another actor in a physics collision (blocking hit).",
            "notes": "Target name: ReceiveHit. Output pins: MyComp (PrimitiveComponent), Other (Actor), OtherComp (PrimitiveComponent), NormalImpulse (Vector), Hit (HitResult)."
        },
        {
            "name": "AnyDamage",
            "description": "Fires when the actor receives any type of damage via ApplyDamage.",
            "notes": "Target name: ReceiveAnyDamage. Output pins: Damage (Float), DamageType (DamageType), InstigatedBy (Controller), DamageCauser (Actor)."
        },
        {
            "name": "PointDamage",
            "description": "Fires when the actor receives point damage (e.g., from ApplyPointDamage).",
            "notes": "Target name: ReceivePointDamage. Output pins: Damage (Float), DamageType (DamageType), HitLocation (Vector), HitNormal (Vector), HitComponent (PrimitiveComponent), BoneName (Name), ShotFromDirection (Vector), InstigatedBy (Controller), DamageCauser (Actor), HitInfo (HitResult)."
        },
        {
            "name": "RadialDamage",
            "description": "Fires when the actor receives radial (area-of-effect) damage.",
            "notes": "Target name: ReceiveRadialDamage. Output pins: DamageReceived (Float), DamageType (DamageType), Origin (Vector), HitInfo (HitResult), InstigatedBy (Controller), DamageCauser (Actor)."
        },
        {
            "name": "Destroyed",
            "description": "Fires when DestroyActor is called on this actor, just before actual destruction.",
            "notes": "Target name: ReceiveDestroyed. No output data pins (exec only)."
        }
    ]
}
```

#### 15.4: Recipe files — soften prescriptive language

**`Content/SystemPrompts/Knowledge/recipes/blueprint/create.txt`** — Replace entire file:
```
TAGS: create new blueprint plan_json actor component variable
---
CREATE new Blueprint:
1. blueprint.create with path and parent_class
2. Add structure: components, variables (batch in one turn)
3. Wire graph logic (apply_plan_json for 3+ nodes, add_node for 1-2)

Do NOT call blueprint.read before create — the asset does not exist yet.
Do NOT batch preview_plan_json and apply_plan_json in the same turn.
Data-provider steps (get_var, pure calls) should appear BEFORE steps that @ref them.
```

**`Content/SystemPrompts/Knowledge/recipes/blueprint/modify.txt`** — Replace entire file:
```
TAGS: modify existing blueprint read write search edit
---
MODIFY existing Blueprint:
1. project.search to find exact asset path (never guess paths)
2. blueprint.read to understand current structure
3. Add variables/components as needed
4. Wire graph logic — apply_plan_json mode:"merge" preserves existing nodes

Always search for the path first — paths vary by project.
Read the graph BEFORE writing to know what events/nodes already exist.
The plan executor reuses existing event nodes automatically.
```

**`Content/SystemPrompts/Knowledge/recipes/blueprint/edit_existing_graph.txt`** — Replace entire file:
```
TAGS: edit existing graph add_node connect_pins small change
---
Edit an existing graph:
1. Read the graph first (blueprint.read_event_graph or blueprint.read_function)
2. Small edits (1-2 nodes): add_node + connect_pins
3. Larger edits: apply_plan_json with mode:"merge" (preserves existing nodes)
4. Remove: blueprint.remove_node with node_id from read result
5. Rewire: disconnect_pins then connect_pins

For function graphs, use blueprint.read_function to read first.
Node IDs come from read results — never guess them.
mode:"merge" keeps existing nodes; mode:"replace" clears the graph first.
```

**`Content/SystemPrompts/Knowledge/recipes/blueprint/multi_asset.txt`** — Replace entire file:
```
TAGS: multi asset multiple blueprint cross dependency
---
MULTI-ASSET workflow (e.g. gun + bullet, spawner + enemy):
1. Create all asset structures first (blueprint.create + add_component + add_variable for every asset)
2. Wire graph logic for each asset one at a time
3. Fix wiring errors if needed

Create all structures before any graph logic so cross-references work
(BP_Gun can reference BP_Bullet_C as a type).
Blueprint class refs append _C: "BP_Bullet_C" not "BP_Bullet".
Complete one asset fully before moving to the next.
```

### What NOT to do for T15

- Do NOT rewrite factory templates (`gun.json`, `projectile.json`, `stat_component.json`). They are data-driven, not prescriptive.
- Do NOT rewrite `fix_wiring.txt`, `spawn_actor.txt`, `function_graph.txt`, `object_variable_type.txt`, `component_reference.txt`, or `variables_components.txt`. These are genuine technical reference.
- Do NOT create new templates or recipes. This is about fixing existing ones, not adding more.

---

## T16: Rewrite `Worker_Blueprint.txt`

**Goal:** The Worker_Blueprint prompt is the most prescriptive file in the entire system. While it's only used in Brain Layer (non-autonomous) mode, it should still follow the same descriptive philosophy.

### File to modify

`Content/SystemPrompts/Worker_Blueprint.txt`

### Problem

- Lines 57-68: "you MUST use the plan JSON path over granular tools" — mandates plan_json for 3+ nodes
- Lines 186-292: "REFERENCE: Complete Tool-Call Sequence Examples" with "Follow these exact patterns" — step-by-step tool call transcripts the AI copies literally
- Lines 270+: "Common Mistakes — NEVER DO THESE" — forbids approaches that are sometimes valid
- Line 153: Plan ops list is stale (missing while_loop, do_once, flip_flop, gate, call_delegate, call_dispatcher, bind_dispatcher)

### Replace entire file with:

```
You are a Blueprint specialist for Unreal Engine 5.5.

## Your Task
{TASK_DESCRIPTION}

## Context From Previous Steps
{PREVIOUS_STEP_CONTEXT}

## Project Rules
{PROJECT_RULES}

{BASE_RULES}

## Graph Editing

You have three ways to build Blueprint graphs:

1. **plan_json** (blueprint.apply_plan_json) — Batch operation. Creates entire function graphs in one call with automatic pin resolution and wiring. Efficient for 3+ nodes.
   Ops: event, custom_event, call, call_delegate, call_dispatcher, bind_dispatcher, get_var, set_var, branch, sequence, cast, for_loop, for_each_loop, while_loop, do_once, flip_flop, gate, delay, is_valid, print_string, spawn_actor, make_struct, break_struct, return, comment

2. **Granular tools** (add_node, connect_pins, set_pin_default) — Place any UK2Node by class name. Use for node types not in plan_json ops, small edits (1-2 nodes), or fixing specific wires.

3. **editor.run_python** — Execute Python in UE's editor scripting context. Full access to the unreal module. Use when neither plan_json nor granular tools can express what you need.

Use whichever fits. Mix approaches within a task. Never simplify your design to fit a tool's limitations.

## Plan JSON (v2.0)

```json
{
  "schema_version": "2.0",
  "steps": [
    {"step_id": "evt", "op": "event", "target": "BeginPlay"},
    {"step_id": "print", "op": "call", "target": "PrintString", "inputs": {"InString": "Hello"}, "exec_after": "evt"}
  ]
}
```

### Data Wires
- `@step.auto` — auto-match by type (~80% of cases)
- `@step.~hint` — fuzzy match when multiple outputs exist
- `@step.PinName` — smart name match (tries exact, display name, case-insensitive, fuzzy)
- No `@` prefix = literal pin default value

### Exec Flow
- `exec_after`: chains execution from a previous step
- `exec_outputs`: maps named outputs to targets, e.g. `{"True": "step_a", "False": "step_b"}`
- `exec_after` and `exec_outputs` are mutually exclusive on the source step

### Function Resolution
Use natural names for call ops. K2_ prefixes and aliases resolve automatically.
Interface functions: set target_class to the interface name (e.g., "BPI_Interactable").

## Blueprint Patterns

### Variables
- Descriptive names: Health, bIsDead, MovementSpeed
- Object refs: {"category":"object","class_name":"Actor"}
- Blueprint refs append _C: {"category":"object","class_name":"BP_Gun_C"}

### Components
- Components are variables — use get_var to access them in plan_json
- Always wire Target on component functions to the get_var output

### Functions
- Create with blueprint.add_function before applying plan_json to them
- Function graph entry node is auto-created — do not add an event step
- First impure step auto-chains from entry; subsequent steps need exec_after

### Self-Correction
- If apply_plan_json returns wiring_errors: read the graph, fix with connect_pins using actual pin names
- If compilation fails: read the error, fix the FIRST error (usually the root cause)
- If an approach fails: try a different tool or technique instead of repeating the same action
```

### What NOT to do for T16

- Do NOT remove the `{TASK_DESCRIPTION}`, `{PREVIOUS_STEP_CONTEXT}`, `{PROJECT_RULES}`, or `{BASE_RULES}` template variables. The Brain Layer's `AssembleWorkerPrompt()` substitutes these.
- Do NOT add detailed tool-call-sequence examples. The old "Example 1: Projectile Blueprint" section with 7 numbered CALL steps is exactly the prescriptive pattern we're removing.
- Do NOT add a "Common Mistakes" section with ❌ symbols. The AI should decide its own approach, not be told what NOT to do.

---

## Verification Checklist

After implementation, verify:

1. **Build succeeds** with no errors or new warnings.
2. **MCP tools/list** shows `editor.run_python` in the tool list.
3. **editor.run_python** with `{"script": "print('hello')"}` returns success with "hello" in output.
4. **editor.run_python** when PythonScriptPlugin is not enabled returns `PYTHON_PLUGIN_NOT_AVAILABLE` error (i.e., `IPythonScriptPlugin::Get()` returns nullptr).
5. **Snapshot** is created in `Saved/OliveAI/Snapshots/` before each Python execution.
6. **Script log** appears at `Saved/OliveAI/PythonScripts.log` with timestamps.
7. **FindFunction** with `ClassName="BPI_Interactable"` and a function defined on that interface returns `InterfaceSearch` match method (not `ExactName`).
8. **add_node** failure messages mention `editor.run_python` as an alternative.
9. **Knowledge packs** load correctly at startup (check LogOliveAI for "Loaded capability knowledge pack" lines).
10. **editor.run_python** is visible in all focus profiles (Auto, Blueprint, C++).
11. **Reference templates** contain no `"tool":` fields, no `"steps"` with tool names, no inline plan_json code examples.
12. **Recipes** contain no "MUST", "never add_node", or "ITERATION BUDGET" mandates.
13. **Worker_Blueprint.txt** presents three approaches as equals, has no "Follow these exact patterns" examples, no "NEVER DO THESE" section.
