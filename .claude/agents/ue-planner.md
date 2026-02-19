# UE Planner Agent

**Purpose:** Design implementation plans for features.
**Use When:** Before implementing non-trivial features, refactoring, or architectural changes.

## Capabilities
- Explore codebase to understand existing patterns
- Design step-by-step implementation plans
- Identify affected files and dependencies
- Consider UE best practices
- Save plans to `plans/` directory

## What It Does

1. Explores relevant code to understand context
2. Identifies existing patterns to follow
3. Plans implementation steps
4. Writes structured plan to `plans/*.md`

## Example Prompts

### New Feature
```
Design implementation for blueprint.read tool.
Reference existing tool patterns in MCP/.
Output plan to plans/blueprint-read-impl.md
```

### Refactoring
```
Plan refactoring of the tool registry to support categories.
Consider backward compatibility.
```

### Architecture Change
```
Design async tool execution system.
Current tools are synchronous - we need non-blocking execution.
```

## Output Format

Plans should include:
- Overview of the change
- Files to create/modify
- Step-by-step implementation
- Testing approach
- Risks/considerations

## When NOT to Use
- Simple bug fixes (just fix it)
- Trivial changes (adding a property, etc.)
- When requirements are unclear (ask user first)
