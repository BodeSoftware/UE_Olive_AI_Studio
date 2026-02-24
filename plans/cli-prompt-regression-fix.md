# CLI Prompt Regression — Analysis & Fix

## What Happened

System prompt grew from **7,869 → 13,409 chars (+70%)**. The AI now searches 3 times then stops — never creates, modifies, or calls `apply_plan_json`. Before the changes, the same prompt produced 20 tool calls in a single batch.

## Why: Three Sources of Conflicting Instructions

`BuildSharedSystemPreamble()` injects the `blueprint_authoring` knowledge pack (written for the API path) into the CLI path, where it conflicts with the CLI wrapper.

### CONFLICT 1: Which tool for graph editing?

| Source | Instruction |
|--------|------------|
| `blueprint_authoring` rule 7 | "For graph work involving 3+ operations, use **project.batch_write**" |
| CLI wrapper ## Rules | "Use **apply_plan_json** for 3+ nodes" |
| recipe routing QUICK RULES | "NEVER use add_node one-at-a-time for 3+ nodes — use **plan_json**" |

### CONFLICT 2: Read before write vs. never read before create

| Source | Instruction |
|--------|------------|
| `blueprint_authoring` rule 1 | "**Read before write:** Use blueprint.read before creating or modifying" |
| recipe routing QUICK RULES | "**NEVER** call blueprint.read before blueprint.create" |

### CONFLICT 3: Variable type format

| Source | Instruction |
|--------|------------|
| `blueprint_authoring` Quick Ref | "Float, Boolean, Int, String, Vector, Rotator, Transform, TSubclassOf&lt;Actor&gt;" |
| CLI wrapper ## Variable Types | "{category:float}, {category:bool}... Object ref: {category:object, class_name:Actor}" |

### CONFLICT 4: Graph editing philosophy

| Source | Instruction |
|--------|------------|
| `blueprint_authoring` rule 4 | "**Prefer surgical edits** (add/connect/remove specific nodes)" |
| CLI wrapper ## Workflows | "**ALL graph logic in one call** via apply_plan_json" |

### Why the AI froze

With 13K chars of contradictory instructions, the safest response is to do nothing. The old 7,869-char prompt had ONE consistent set of instructions.

---

## The Fix

**Skip `blueprint_authoring` in the CLI path.** The CLI wrapper already covers everything the AI needs. Cherry-pick only project context, policies, and recipe routing.

### Change 1: OliveClaudeCodeProvider.cpp — BuildSystemPrompt()

Find this (around line 74155):

```cpp
	// ==========================================
	// Shared preamble — recipe routing, knowledge packs, cross-cutting context
	// Ensures CLI path stays in sync with API providers
	// ==========================================
	FString Preamble = FOlivePromptAssembler::Get().BuildSharedSystemPreamble(TEXT("Blueprint"));
	if (!Preamble.IsEmpty())
	{
		SystemPrompt += Preamble;
		SystemPrompt += TEXT("\n\n");
	}
```

Replace with:

```cpp
	// ==========================================
	// Cherry-picked preamble — project context + policies + recipe routing ONLY.
	// We intentionally skip blueprint_authoring because it was written for the
	// API path (uses project.batch_write, "read before write" for creates, etc.)
	// and directly conflicts with the CLI wrapper's instructions.
	// ==========================================
	const FOlivePromptAssembler& Assembler = FOlivePromptAssembler::Get();

	const FString ProjectContext = Assembler.GetProjectContext();
	if (!ProjectContext.IsEmpty())
	{
		SystemPrompt += TEXT("## Project\n");
		SystemPrompt += ProjectContext;
		SystemPrompt += TEXT("\n\n");
	}

	const FString PolicyContext = Assembler.GetPolicyContext();
	if (!PolicyContext.IsEmpty())
	{
		SystemPrompt += TEXT("## Policies\n");
		SystemPrompt += PolicyContext;
		SystemPrompt += TEXT("\n\n");
	}

	// Fetch recipe_routing pack directly — skip blueprint_authoring
	const FString RecipeRouting = Assembler.GetKnowledgePackById(TEXT("recipe_routing"));
	if (!RecipeRouting.IsEmpty())
	{
		SystemPrompt += RecipeRouting;
		SystemPrompt += TEXT("\n\n");
	}
```

### Change 2: OlivePromptAssembler.h — Add declaration

```cpp
	/** Get a single knowledge pack by ID (e.g. "recipe_routing", "blueprint_authoring") */
	FString GetKnowledgePackById(const FString& PackId) const;
```

### Change 3: OlivePromptAssembler.cpp — Add implementation

```cpp
FString FOlivePromptAssembler::GetKnowledgePackById(const FString& PackId) const
{
	const FString* PackText = CapabilityKnowledgePacks.Find(PackId);
	return PackText ? *PackText : TEXT("");
}
```

---

## Result

- System prompt drops from ~13,400 back to ~10,000 chars
- Zero conflicting instructions
- CLI still gets: project context, policies, recipe routing table, `olive.get_recipe` tool
- CLI no longer gets: `blueprint_authoring` rules that contradict the wrapper
- API path is completely unaffected
