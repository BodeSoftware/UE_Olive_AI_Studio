# UE Builder Agent

**Purpose:** Handle Unreal Engine builds and compilation.
**Use When:** Building the project, checking compile errors, running in background.

## Capabilities
- Run UBT/UHT build commands
- Parse build output for errors
- Identify error locations
- Suggest fixes for common errors
- Run in background for long builds

## Build Commands

### Editor Build (Development)
```bash
# From Engine directory
UnrealEditor.exe "B:\Unreal Projects\UE_Olive_AI_Toolkit\UE_Olive_AI_Toolkit.uproject" -build
```

### Module Rebuild
```bash
# Force rebuild specific module
UnrealBuildTool.exe UE_Olive_AI_Toolkit Win64 Development -module=OliveAIEditor
```

### Hot Reload Check
```bash
# Check if LiveCoding is available
UnrealEditor.exe -LiveCoding
```

## Common Error Patterns

| Error Pattern | Meaning | Typical Fix |
|---------------|---------|-------------|
| `unresolved external symbol` | Missing implementation | Add function body |
| `cannot open include file` | Missing header | Check include paths, add dependency |
| `GENERATED_BODY() missing` | UHT issue | Add macro to UCLASS/USTRUCT |
| `redefinition of` | Duplicate symbol | Check header guards |
| `linking failed` | Missing .lib or dependency | Add module to .Build.cs |

## Background Execution

```
Launch with: run_in_background: true
Check with: Read the output_file path returned
Kill with: KillShell if build is stuck
```

## Example Prompts

### Simple Build
```
Build the Unreal project and report any errors
```

### Background Build
```
Build the project in the background.
I'll continue working while it builds.
```

### Error Focus
```
Build and focus on errors in OliveAIEditor module
```

## When NOT to Use
- Code exploration (use Explorer)
- Planning (use Planner)
- Writing code (use main context)
