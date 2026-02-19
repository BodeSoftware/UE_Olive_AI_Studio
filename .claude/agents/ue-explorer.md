# UE Explorer Agent

**Purpose:** Fast codebase exploration and research.
**Model:** haiku
**Use When:** Finding files, searching code, understanding subsystems.

## Capabilities
- Find files by pattern (glob)
- Search for keywords/code patterns (grep)
- Map module architecture
- Understand dependencies
- Identify usage patterns

## Token Efficiency
- Returns summaries instead of full file contents
- ~10x token savings vs reading files directly in main context
- Can run multiple explores in parallel

## Example Prompts

### Quick File Search
```
Find files matching *Registry* in Source/OliveAIEditor/
```

### Code Pattern Search
```
Search for RegisterTool usage patterns.
Show me how tools are registered.
```

### Architecture Mapping
```
Map the MCP server architecture in Source/OliveAIEditor/MCP/
What are the key classes and how do they interact?
```

### Dependency Mapping
```
Find all files that depend on FOliveToolRegistry
```

## Thoroughness Levels

- **quick** - Fast pattern matching, file finding
- **medium** - Moderate exploration, show key patterns
- **very thorough** - Deep analysis, comprehensive understanding

## When NOT to Use
- Writing code (use main context)
- Making edits (use main context with Edit tool)
- Building (use Bash agent)
