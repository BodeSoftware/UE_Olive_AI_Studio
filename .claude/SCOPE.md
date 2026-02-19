# Development Scope - DO NOT CROSS

## Working Directory
```
b:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio\
```

## Allowed Directories (ALL paths relative to working directory)
- `Source/OliveAIRuntime/` - Runtime module
- `Source/OliveAIEditor/` - Editor module
- `Content/` - Plugin content (if any)
- `Config/` - Plugin configuration
- `plans/` - Implementation plans
- `.claude/` - Claude configuration
- Root files (`*.uplugin`, `*.md`, etc.)

## FORBIDDEN - Never Access These
- `b:\Unreal Projects\UE_Olive_AI_Toolkit\Source\` - Parent project source
- `b:\Unreal Projects\UE_Olive_AI_Toolkit\Content\` - Parent project content
- `b:\Unreal Projects\UE_Olive_AI_Toolkit\Config\` - Parent project config
- `b:\Unreal Projects\UE_Olive_AI_Toolkit\*.uproject` - Parent project file
- ANY path outside the plugin directory

## Why?
The parent project is just a test harness. Plugin development happens entirely within the plugin directory. Accessing parent paths wastes tokens and violates development boundaries.

## Enforcement
Permissions are configured in `.claude/settings.json` with explicit deny rules for parent directories.

## When to Ask
If you think you need to access the parent project, ASK THE USER FIRST. 99.9% of the time, you don't need to.
