# Olive AI — Recipe System + CLI Wrapper Rewrite

**Master Implementation Document**
**Date:** 2026-02-23
**Scope:** Single task — CLI wrapper rewrite + recipe system, ships together

---

# Part 1: Problem & Log Analysis

## 1.1 The Problem

Olive AI drives Unreal Engine Blueprint creation/modification through LLM tool-calling. The LLM receives 30+ MCP tools and must figure out the correct sequence. Three failure categories: wrong tool sequence (2,000–5,000 tokens wasted per mistake), wrong parameters (2–3 correction turns), and no pattern for unfamiliar situations.

## 1.2 Real Failures From the CLI Path (Log: 2026-02-23)

### Session 1: Create BP_Bullet + BP_Gun (mostly succeeded)

**What worked:** AI correctly batched 20 tool calls — `blueprint.create` × 2, `add_component` × 6, `add_variable` × 4, `modify_component` × 3, `add_function` × 1, `preview_plan_json` × 2, `apply_plan_json` × 2.

**Failure 1 — PLAN_INVALID_EXEC_AFTER in function graph:**
AI tried `exec_after: "entry"` in the Fire function graph. The validator rejected it — "entry" is not a declared step_id. Function graphs auto-create entry nodes; the first impure step should omit exec_after.

*Root cause:* Wrapper prompt has no guidance about function graphs vs event graphs.

**Failure 2 — SpawnActor wiring failed (Location/Rotation pins don't exist):**
AI used `@break_xform.~Location` and `@break_xform.~Rotation`. SpawnActor only has `SpawnTransform` (Transform type). 2 of 3 data connections failed. Pin manifest showed the correct pin names.

*Root cause:* The wrapper's plan JSON example used `"Location":"@get_loc.auto"` — the AI learned the wrong pin name from the system prompt.

**Failure 3 — Fingerprint mismatches on every apply_plan_json:**
Every apply logged "fingerprint mismatch." AI called preview then apply in the same batch, but other tools modified the blueprint between them.

*Root cause:* Wrapper said "Always call blueprint.preview_plan_json first." Harmful for batched execution.

### Session 2: Modify BP_ThirdPersonCharacter (failed)

**Failure 4 — Wrong asset path:**
AI tried `/Game/Blueprints/BP_ThirdPersonCharacter`. Real path: `/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter`. Wasted a turn.

*Root cause:* No guidance to search for paths before modifying existing assets.

**Failure 5 — Variable type format wrong for object references:**
AI used `{"category":"object","sub_category":"/Game/Blueprints/BP_Gun"}`. The field is `class_name` not `sub_category`, and the value should be `"BP_Gun_C"` not an asset path.

*Root cause:* Wrapper only documented primitive types. No object/class/array/enum examples.

**Failure 6 — Cascading failure:**
Variables failed (wrong type) → AI called `SetNearbyGun` in a plan → function didn't exist → PLAN_EXECUTION_FAILED → self-correction loop detected.

*Root cause:* No rule about not referencing failed resources in subsequent calls.

### What Fixes What

| Fix | CLI Wrapper | Recipe System |
|-----|:-----------:|:------------:|
| Function graph exec flow | ✓ new section | ✓ edit_existing_graph |
| SpawnActor pin names | ✓ fixed example | ✓ create, fix_wiring |
| Fingerprint batch conflict | ✓ removed mandate | ✓ routing: "prefer direct apply" |
| Search before modify | ✓ new workflow | ✓ modify recipe |
| Variable type format | ✓ expanded types | ✓ variables_components |
| Cascading failure guard | ✓ new rule | — |

---

# Part 2: Architecture

## 2.1 Three Layers

```
┌──────────────────────────────────────────────────────────┐
│  LAYER 1: Routing Table (knowledge pack — system prompt) │
│  Always present. ~130 tokens.                            │
│  Decision tree + recipe directory + critical rules.      │
│  Injected via BuildSharedSystemPreamble() so ALL         │
│  provider paths get it identically.                      │
├──────────────────────────────────────────────────────────┤
│  LAYER 2: Recipe Tool (on-demand retrieval)              │
│  olive.get_recipe(category, name)                        │
│  Returns ~100–250 tokens of focused content.             │
│  Tool description carries mini routing bullets for       │
│  MCP/external clients that lack a system prompt.         │
├──────────────────────────────────────────────────────────┤
│  LAYER 3: Recipe Library (disk)                          │
│  Text files in Content/SystemPrompts/Knowledge/recipes/  │
│  Organized by category subdirectory.                     │
│  Manifest provides metadata + profile mapping.           │
│  **New recipes = drop file + update manifest + restart editor.** No C++ rebuild needed, but loader runs at startup so new files require restart.  │
└──────────────────────────────────────────────────────────┘
```

## 2.2 Provider Path Coverage

| Provider | Gets Routing Table Via |
|----------|----------------------|
| Anthropic, OpenAI, Google, Ollama, OpenRouter, ZAI, OpenAI Compatible | Knowledge pack (existing flow) |
| **Claude Code CLI** | **NEW: calls BuildSharedSystemPreamble()** |
| External MCP client | Tool description routing bullets + no-arg discovery |

7 of 8 providers already work via existing knowledge pack mechanism. Only Claude Code needs a code change.

## 2.3 Token Budget

| Component | Tokens | When |
|-----------|--------|------|
| Routing table (Layer 1) | ~130 | Every turn |
| Recipe fetch (Layer 2) | ~100–250 | Turn it's retrieved |
| Tool description bullets | ~50 | Every turn (in tool defs) |
| **Steady state** | **~180** | |

**vs. cost of failure:** Wrong workflow = 2–3 correction turns × ~3,000 tokens = 6,000–9,000 wasted.

## 2.4 Disk Structure

```
Content/SystemPrompts/Knowledge/
├── blueprint_authoring.txt          ← existing, unchanged
├── recipe_routing.txt               ← NEW: Layer 1 routing table
└── recipes/                         ← NEW: Layer 3 recipe library
    ├── _manifest.json
    └── blueprint/
        ├── create.txt
        ├── modify.txt
        ├── fix_wiring.txt
        ├── variables_components.txt
        └── edit_existing_graph.txt
```

## 2.5 Recipe File Format Convention

```
WORKFLOW:
1. tool.name → {key_params}
2. tool.name → {key_params}

EXAMPLE:
{single JSON blob}

GOTCHAS:
- Most expensive mistake
- Non-obvious requirement
```

No markdown headers, no prose. Target 100–250 tokens per recipe.

---

# Part 3: CLI Wrapper Rewrite

## 3.1 Current vs New — What Changed

**Added:** Workflows section (CREATE/MODIFY/SMALL EDIT decision tree)
**Added:** Function Graphs vs EventGraph section
**Added:** Full variable type reference (object, class, array, enum)
**Added:** `BuildSharedSystemPreamble()` call for recipe routing + knowledge packs
**Changed:** Plan JSON example — SpawnTransform instead of Location
**Changed:** Preview rule — "optional, prefer direct apply" instead of "always call first"
**Changed:** Plan size rule — "3+ nodes → plan_json" instead of "NEVER use add_node"
**Added:** "Never guess asset paths" rule
**Added:** Cascading failure guard
**Added:** SpawnActor pin reality note
**Kept:** CLI-specific tool schema serialization and tool call format instructions

## 3.2 Replacement Code: BuildSystemPrompt()

**File:** `OliveClaudeCodeProvider.cpp`
**Action:** Replace the entire `BuildSystemPrompt()` method

```cpp
FString FOliveClaudeCodeProvider::BuildSystemPrompt(const FString& UserTask, const TArray<FOliveToolDefinition>& Tools) const
{
	FString SystemPrompt;

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

	// ==========================================
	// Identity
	// ==========================================
	SystemPrompt += TEXT("You are an Unreal Engine 5.5 Blueprint specialist.\n");
	SystemPrompt += TEXT("Think through the complete design before calling tools.\n\n");

	// ==========================================
	// Workflows — the AI needs to know WHAT to do first
	// ==========================================
	SystemPrompt += TEXT("## Workflows\n");
	SystemPrompt += TEXT("CREATE new Blueprint:\n");
	SystemPrompt += TEXT("1. blueprint.create → 2. add_component/add_variable → 3. blueprint.apply_plan_json (ALL graph logic in one call)\n\n");
	SystemPrompt += TEXT("MODIFY existing Blueprint:\n");
	SystemPrompt += TEXT("1. project.search (find exact path) → 2. blueprint.read (understand current state) → 3. add_variable/add_component → 4. blueprint.apply_plan_json\n");
	SystemPrompt += TEXT("IMPORTANT: Always search for the asset path first. Paths vary by project (e.g. /Game/ThirdPerson/Blueprints/ not /Game/Blueprints/).\n\n");
	SystemPrompt += TEXT("SMALL EDIT (1-2 nodes on existing graph):\n");
	SystemPrompt += TEXT("1. blueprint.read_event_graph → 2. blueprint.add_node + blueprint.connect_pins\n\n");

	// ==========================================
	// Plan JSON reference
	// ==========================================
	SystemPrompt += TEXT("## Plan JSON (v2.0)\n");
	SystemPrompt += TEXT("```json\n");
	SystemPrompt += TEXT("{\"schema_version\":\"2.0\",\"steps\":[\n");
	SystemPrompt += TEXT("  {\"step_id\":\"evt\",\"op\":\"event\",\"target\":\"BeginPlay\"},\n");
	SystemPrompt += TEXT("  {\"step_id\":\"get_xform\",\"op\":\"call\",\"target\":\"GetActorTransform\"},\n");
	SystemPrompt += TEXT("  {\"step_id\":\"spawn\",\"op\":\"spawn_actor\",\"target\":\"Actor\",");
	SystemPrompt += TEXT("\"inputs\":{\"SpawnTransform\":\"@get_xform.auto\"},\"exec_after\":\"evt\"},\n");
	SystemPrompt += TEXT("  {\"step_id\":\"print\",\"op\":\"call\",\"target\":\"PrintString\",");
	SystemPrompt += TEXT("\"inputs\":{\"InString\":\"Done\"},\"exec_after\":\"spawn\"}\n");
	SystemPrompt += TEXT("]}\n");
	SystemPrompt += TEXT("```\n\n");

	// ==========================================
	// Ops, wire syntax, exec flow (compact)
	// ==========================================
	SystemPrompt += TEXT("## Ops\n");
	SystemPrompt += TEXT("event, custom_event, call, get_var, set_var, branch, sequence, cast, ");
	SystemPrompt += TEXT("for_loop, for_each_loop, delay, is_valid, print_string, spawn_actor, ");
	SystemPrompt += TEXT("make_struct, break_struct, return, comment\n\n");

	SystemPrompt += TEXT("## Wires\n");
	SystemPrompt += TEXT("Data: @step.auto (type-match, use ~80%), @step.~hint (fuzzy), @step.PinName (exact)\n");
	SystemPrompt += TEXT("Literals: \"InString\":\"Hello\" (no @ = pin default)\n");
	SystemPrompt += TEXT("Exec: exec_after:\"step_id\" (chains then→execute), exec_outputs:{\"True\":\"a\",\"False\":\"b\"} for Branch\n\n");

	// ==========================================
	// Function graph exec flow — was missing, caused PLAN_INVALID_EXEC_AFTER
	// ==========================================
	SystemPrompt += TEXT("## Function Graphs vs EventGraph\n");
	SystemPrompt += TEXT("- EventGraph: use op:\"event\" with target:\"BeginPlay\"/\"Tick\"/etc. as entry points\n");
	SystemPrompt += TEXT("- Function graphs (blueprint.add_function): the entry node is auto-created.\n");
	SystemPrompt += TEXT("  First impure step should NOT use exec_after. It auto-chains from the entry node.\n");
	SystemPrompt += TEXT("  Pure steps (get_var, pure function calls) need no exec wiring.\n\n");

	// ==========================================
	// Function resolution
	// ==========================================
	SystemPrompt += TEXT("## Function Resolution\n");
	SystemPrompt += TEXT("Use natural names for op:call. Resolves K2_ prefixes and aliases automatically.\n");
	SystemPrompt += TEXT("Examples: Destroy→K2_DestroyActor, Print→PrintString, GetWorldTransform→K2_GetComponentToWorld\n\n");

	// ==========================================
	// Variable type format — expanded from log failures
	// ==========================================
	SystemPrompt += TEXT("## Variable Types\n");
	SystemPrompt += TEXT("Basic: {\"category\":\"float\"}, {\"category\":\"bool\"}, {\"category\":\"int\"}, {\"category\":\"string\"}\n");
	SystemPrompt += TEXT("Shorthand also works: \"Float\", \"Boolean\", \"Integer\", \"String\", \"Vector\", \"Rotator\", \"Transform\"\n");
	SystemPrompt += TEXT("Object ref: {\"category\":\"object\",\"class_name\":\"Actor\"} (use UE class name, not asset path)\n");
	SystemPrompt += TEXT("Blueprint ref: {\"category\":\"object\",\"class_name\":\"BP_Gun_C\"} (append _C for Blueprint classes)\n");
	SystemPrompt += TEXT("Class ref: {\"category\":\"class\",\"class_name\":\"Actor\"} (for TSubclassOf)\n");
	SystemPrompt += TEXT("Array: {\"category\":\"array\",\"value_type\":{\"category\":\"float\"}}\n");
	SystemPrompt += TEXT("Enum: {\"category\":\"byte\",\"enum_name\":\"ECollisionChannel\"}\n\n");

	// ==========================================
	// Key rules — cleaned up, no preview mandate
	// ==========================================
	SystemPrompt += TEXT("## Rules\n");
	SystemPrompt += TEXT("- Asset paths MUST end with asset name: /Game/Blueprints/BP_Gun (NOT /Game/Blueprints/)\n");
	SystemPrompt += TEXT("- Use project.search to find asset paths — never guess paths\n");
	SystemPrompt += TEXT("- Use apply_plan_json for 3+ nodes. For 1-2 nodes, add_node + connect_pins is fine.\n");
	SystemPrompt += TEXT("- preview_plan_json is optional. Prefer calling apply_plan_json directly. If you preview, do it in a separate turn before apply.\n");
	SystemPrompt += TEXT("- STEP ORDER: data-provider steps (get_var, pure calls) BEFORE steps that reference them via @ref.\n");
	SystemPrompt += TEXT("- If add_variable fails, do NOT reference that variable in subsequent plan_json calls.\n");
	SystemPrompt += TEXT("- SpawnActor uses SpawnTransform (Transform type), NOT separate Location/Rotation pins.\n");
	SystemPrompt += TEXT("- Component classes: StaticMeshComponent, SphereComponent, BoxComponent, ");
	SystemPrompt += TEXT("CapsuleComponent, ArrowComponent, ProjectileMovementComponent, SceneComponent, AudioComponent\n\n");

	// ==========================================
	// Tool schemas (CLI-specific: inline since no native tool calling)
	// ==========================================
	if (Tools.Num() > 0)
	{
		SystemPrompt += FOliveCLIToolSchemaSerializer::Serialize(Tools, /*bCompact=*/true);
		SystemPrompt += TEXT("\n");
	}

	// ==========================================
	// Tool call format instructions (CLI-specific)
	// ==========================================
	SystemPrompt += FOliveCLIToolCallParser::GetFormatInstructions();

	return SystemPrompt;
}
```

**Also add to top of OliveClaudeCodeProvider.cpp:**
```cpp
#include "Chat/OlivePromptAssembler.h"
```

## 3.3 BuildSharedSystemPreamble()

**File:** `OlivePromptAssembler.h` — add to public section:

```cpp
/**
 * Returns shared preamble text that ALL provider paths should include.
 * Contains recipe routing, knowledge packs, and other cross-cutting context.
 * Claude Code provider, future CLI providers, etc. call this to stay
 * in sync with the knowledge packs that API providers get automatically.
 */
FString BuildSharedSystemPreamble(const FString& ProfileName) const;
```

**File:** `OlivePromptAssembler.cpp` — add implementation:

```cpp
FString FOlivePromptAssembler::BuildSharedSystemPreamble(const FString& ProfileName) const
{
	if (CapabilityKnowledgePacks.Num() == 0)
	{
		UE_LOG(LogOliveAI, Warning, TEXT("BuildSharedSystemPreamble called before Initialize()"));
		return TEXT("");
	}

	FString Preamble;

	// Project context
	const FString ProjectContext = GetProjectContext();
	if (!ProjectContext.IsEmpty())
	{
		Preamble += TEXT("## Project\n");
		Preamble += ProjectContext;
		Preamble += TEXT("\n");
	}

	// Policy context
	const FString PolicyContext = GetPolicyContext();
	if (!PolicyContext.IsEmpty())
	{
		Preamble += TEXT("## Policies\n");
		Preamble += PolicyContext;
		Preamble += TEXT("\n");
	}

	// Capability knowledge — recipes, authoring rules
	const FString CapabilityKnowledge = GetCapabilityKnowledge(ProfileName);
	if (!CapabilityKnowledge.IsEmpty())
	{
		Preamble += CapabilityKnowledge;
		Preamble += TEXT("\n");
	}

	return Preamble;
}
```

**File:** `OlivePromptAssembler.cpp` — update profile mappings in `LoadPromptTemplates()`:

```cpp
ProfileCapabilityPackIds.Add(TEXT("Auto"), { TEXT("blueprint_authoring"), TEXT("recipe_routing") });
ProfileCapabilityPackIds.Add(TEXT("Blueprint"), { TEXT("blueprint_authoring"), TEXT("recipe_routing") });
// NOTE: C++ profile intentionally omits recipe_routing — current recipes are Blueprint-only.
// Add it when C++ recipes exist.
```

**File:** `IOliveAIProvider.h` — add documentation comment:

```cpp
/**
 * IMPORTANT: If your provider builds its own system prompt instead of using
 * ConversationManager::BuildSystemMessage(), you MUST call
 * FOlivePromptAssembler::Get().BuildSharedSystemPreamble(ProfileName)
 * and include its output in your system prompt. This ensures your provider
 * gets recipe routing, knowledge packs, and other cross-cutting context.
 */
```

---

# Part 4: Recipe Tool Implementation

## 4.1 OliveCrossSystemToolHandlers.h — Add to private section:

```cpp
// Recipe system
void RegisterRecipeTools();
void LoadRecipeLibrary();
FOliveToolResult HandleGetRecipe(const TSharedPtr<FJsonObject>& Params);

/** Loaded recipes: Key = "category/name", Value = file content */
TMap<FString, FString> RecipeLibrary;

/** Manifest data: Key = "category/name", Value = description */
TMap<FString, FString> RecipeDescriptions;

/** Categories discovered from manifest */
TArray<FString> RecipeCategories;
```

## 4.2 OliveCrossSystemToolHandlers.cpp — Add to RegisterAllTools():

```cpp
void FOliveCrossSystemToolHandlers::RegisterAllTools()
{
	// ... existing calls ...
	RegisterBulkTools();
	RegisterBatchTools();
	RegisterSnapshotTools();
	RegisterIndexTools();

	// NEW: Recipe system
	LoadRecipeLibrary();
	RegisterRecipeTools();
}
```

## 4.3 LoadRecipeLibrary()

```cpp
void FOliveCrossSystemToolHandlers::LoadRecipeLibrary()
{
	const FString RecipesDir = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("UE_Olive_AI_Studio/Content/SystemPrompts/Knowledge/recipes"));

	if (!IFileManager::Get().DirectoryExists(*RecipesDir))
	{
		UE_LOG(LogOliveCrossSystemTools, Warning, TEXT("Recipe directory not found: %s"), *RecipesDir);
		return;
	}

	// Load manifest
	const FString ManifestPath = FPaths::Combine(RecipesDir, TEXT("_manifest.json"));
	FString ManifestContent;
	if (FFileHelper::LoadFileToString(ManifestContent, *ManifestPath))
	{
		TSharedPtr<FJsonObject> ManifestJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestContent);
		if (FJsonSerializer::Deserialize(Reader, ManifestJson) && ManifestJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* CategoriesObj;
			if (ManifestJson->TryGetObjectField(TEXT("categories"), CategoriesObj))
			{
				for (const auto& CategoryPair : (*CategoriesObj)->Values)
				{
					const FString& CategoryName = CategoryPair.Key;
					RecipeCategories.Add(CategoryName);

					const TSharedPtr<FJsonObject>* CategoryObj;
					if (CategoryPair.Value->TryGetObject(CategoryObj))
					{
						const TSharedPtr<FJsonObject>* RecipesObj;
						if ((*CategoryObj)->TryGetObjectField(TEXT("recipes"), RecipesObj))
						{
							for (const auto& RecipePair : (*RecipesObj)->Values)
							{
								const FString& RecipeName = RecipePair.Key;
								const FString Key = FString::Printf(TEXT("%s/%s"), *CategoryName, *RecipeName);

								const TSharedPtr<FJsonObject>* RecipeMetaObj;
								if (RecipePair.Value->TryGetObject(RecipeMetaObj))
								{
									FString Description;
									(*RecipeMetaObj)->TryGetStringField(TEXT("description"), Description);
									RecipeDescriptions.Add(Key, Description);
								}

								const FString FilePath = FPaths::Combine(RecipesDir, CategoryName, RecipeName + TEXT(".txt"));
								FString Content;
								if (FFileHelper::LoadFileToString(Content, *FilePath))
								{
									RecipeLibrary.Add(Key, Content);
									UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Loaded recipe: %s (%d chars)"), *Key, Content.Len());
								}
								else
								{
									UE_LOG(LogOliveCrossSystemTools, Warning, TEXT("Recipe file not found: %s"), *FilePath);
								}
							}
						}
					}
				}
			}
		}
	}

	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Recipe library loaded: %d recipes in %d categories"),
		RecipeLibrary.Num(), RecipeCategories.Num());
}
```

## 4.4 RegisterRecipeTools()

Uses the actual `RegisterTool(name, description, InputSchema, handler, tags, category)` API and `OliveCrossSystemSchemas` pattern — **NOT** the nonexistent FOliveToolParam builder.

```cpp
// In OliveCrossSystemSchemas.h, add declaration:
TSharedPtr<FJsonObject> RecipeGetRecipe();
```

```cpp
// In OliveCrossSystemSchemas.cpp, add implementation:
TSharedPtr<FJsonObject> RecipeGetRecipe()
{
	TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
	TSharedPtr<FJsonObject> Props = MakeProperties();

	Props->SetObjectField(TEXT("category"),
		OliveBlueprintSchemas::StringProp(TEXT("Recipe category (e.g. 'blueprint'). Omit to list categories.")));
	Props->SetObjectField(TEXT("name"),
		OliveBlueprintSchemas::StringProp(TEXT("Recipe name (e.g. 'create', 'modify'). Omit to list recipes in category. Comma-separated for batch.")));

	Schema->SetObjectField(TEXT("properties"), Props);
	// No required fields — both params are optional
	return Schema;
}
```

```cpp
// In OliveCrossSystemToolHandlers.cpp:
void FOliveCrossSystemToolHandlers::RegisterRecipeTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("olive.get_recipe"),
		TEXT("Retrieve a worked example showing the exact tool-call sequence for a workflow. "
			"Available categories: blueprint. "
			"Call with category only to list recipes. Call with category + name for the full example. "
			"Patterns: NEW blueprint -> get_recipe(blueprint, create) | "
			"MODIFY existing -> get_recipe(blueprint, modify) | "
			"FIX wiring -> get_recipe(blueprint, fix_wiring)"),
		OliveCrossSystemSchemas::RecipeGetRecipe(),
		FOliveToolHandler::CreateRaw(this, &FOliveCrossSystemToolHandlers::HandleGetRecipe),
		{TEXT("crosssystem"), TEXT("read")},  // tags: crosssystem (matches profile filter) + read (for read_pack)
		TEXT("crosssystem")                    // category: must be crosssystem, not olive — profiles filter by this
	);
	RegisteredToolNames.Add(TEXT("olive.get_recipe"));

	UE_LOG(LogOliveCrossSystemTools, Log, TEXT("Registered recipe tool with %d recipes available"), RecipeLibrary.Num());
}
```

## 4.5 HandleGetRecipe()

Uses the actual `FOliveToolResult` API: `Success(TSharedPtr<FJsonObject>)` and `Error(code, message, suggestion)`.

```cpp
FOliveToolResult FOliveCrossSystemToolHandlers::HandleGetRecipe(const TSharedPtr<FJsonObject>& Params)
{
	// Guard: some callers may pass null/empty params
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("RECIPE_INVALID_PARAMS"),
			TEXT("Parameters required"),
			TEXT("Call with {\"category\":\"blueprint\"} to list recipes, or {\"category\":\"blueprint\",\"name\":\"create\"} for content."));
	}

	FString Category, Name;
	Params->TryGetStringField(TEXT("category"), Category);
	Params->TryGetStringField(TEXT("name"), Name);

	// No category — list all categories
	if (Category.IsEmpty())
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> CatArray;
		for (const FString& Cat : RecipeCategories)
		{
			int32 Count = 0;
			for (const auto& Pair : RecipeLibrary)
			{
				if (Pair.Key.StartsWith(Cat + TEXT("/"))) Count++;
			}
			TSharedPtr<FJsonObject> CatObj = MakeShared<FJsonObject>();
			CatObj->SetStringField(TEXT("name"), Cat);
			CatObj->SetNumberField(TEXT("recipe_count"), Count);
			CatArray.Add(MakeShared<FJsonValueObject>(CatObj));
		}
		Data->SetArrayField(TEXT("categories"), CatArray);
		return FOliveToolResult::Success(Data);
	}

	// Validate category
	if (!RecipeCategories.Contains(Category))
	{
		FString ValidCats;
		for (const FString& Cat : RecipeCategories)
		{
			if (!ValidCats.IsEmpty()) ValidCats += TEXT(", ");
			ValidCats += Cat;
		}
		return FOliveToolResult::Error(
			TEXT("RECIPE_UNKNOWN_CATEGORY"),
			FString::Printf(TEXT("Unknown category '%s'"), *Category),
			FString::Printf(TEXT("Valid categories: %s"), *ValidCats));
	}

	// Category only — list recipes in category
	if (Name.IsEmpty())
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("category"), Category);
		TArray<TSharedPtr<FJsonValue>> RecipeArray;
		for (const auto& Pair : RecipeDescriptions)
		{
			if (Pair.Key.StartsWith(Category + TEXT("/")))
			{
				FString RecipeName = Pair.Key.RightChop(Category.Len() + 1);
				TSharedPtr<FJsonObject> RecipeObj = MakeShared<FJsonObject>();
				RecipeObj->SetStringField(TEXT("name"), RecipeName);
				RecipeObj->SetStringField(TEXT("description"), Pair.Value);
				RecipeArray.Add(MakeShared<FJsonValueObject>(RecipeObj));
			}
		}
		Data->SetArrayField(TEXT("recipes"), RecipeArray);
		return FOliveToolResult::Success(Data);
	}

	// Handle comma-separated names for batch fetch
	TArray<FString> Names;
	Name.ParseIntoArray(Names, TEXT(","), true);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ResultArray;

	for (const FString& SingleName : Names)
	{
		FString TrimmedName = SingleName.TrimStartAndEnd();
		FString Key = FString::Printf(TEXT("%s/%s"), *Category, *TrimmedName);

		const FString* Content = RecipeLibrary.Find(Key);
		if (!Content)
		{
			FString ValidRecipes;
			for (const auto& Pair : RecipeLibrary)
			{
				if (Pair.Key.StartsWith(Category + TEXT("/")))
				{
					FString RName = Pair.Key.RightChop(Category.Len() + 1);
					if (!ValidRecipes.IsEmpty()) ValidRecipes += TEXT(", ");
					ValidRecipes += RName;
				}
			}
			return FOliveToolResult::Error(
				TEXT("RECIPE_NOT_FOUND"),
				FString::Printf(TEXT("Recipe '%s' not found in category '%s'"), *TrimmedName, *Category),
				FString::Printf(TEXT("Available recipes: %s"), *ValidRecipes));
		}

		TSharedPtr<FJsonObject> RecipeObj = MakeShared<FJsonObject>();
		RecipeObj->SetStringField(TEXT("name"), TrimmedName);
		RecipeObj->SetStringField(TEXT("content"), *Content);
		ResultArray.Add(MakeShared<FJsonValueObject>(RecipeObj));
	}

	Data->SetStringField(TEXT("category"), Category);
	Data->SetArrayField(TEXT("recipes"), ResultArray);
	return FOliveToolResult::Success(Data);
}
```

## 4.6 Tool Pack Gating — REQUIRED

`olive.get_recipe` will be invisible to the AI unless added to tool packs. Add it to **read_pack** in both locations:

**Config/OliveToolPacks.json** — add to `read_pack` array:
```json
"read_pack": [
	"project.search",
	"project.get_asset_info",
	...existing entries...
	"olive.get_recipe"
]
```

**OliveToolPackManager.cpp → RegisterDefaultPacks()** — add fallback entry:
```cpp
PackDefinitions.FindOrAdd(EOliveToolPack::ReadPack) = {
	TEXT("project.search"), TEXT("project.get_asset_info"),
	TEXT("project.get_class_hierarchy"), TEXT("project.get_dependencies"),
	TEXT("blueprint.read"), TEXT("blueprint.read_function"),
	TEXT("blueprint.read_event_graph"), TEXT("blueprint.read_variables"),
	TEXT("blueprint.read_components"),
	TEXT("behaviortree.read"), TEXT("blackboard.read"),
	TEXT("pcg.read_graph"),
	TEXT("cpp.read_class"), TEXT("cpp.read_header"),
	TEXT("olive.get_recipe")  // NEW
};
```

## 4.7 Self-Correction Alignment — REQUIRED

The GRAPH_DRIFT correction directive still pushes preview-first behavior. Update in `OliveToolPackManager.cpp` (or wherever `BuildCorrectionDirective` lives, around line 52388):

**Before:**
```cpp
else if (ErrorCode == TEXT("GRAPH_DRIFT"))
{
	Guidance = TEXT(
		"The graph fingerprint did not match. Do NOT batch preview_plan_json and apply_plan_json "
		"in the same response. Call blueprint.preview_plan_json FIRST, wait for the result, then "
		"call blueprint.apply_plan_json with the exact fingerprint. Alternatively, omit the "
		"preview_fingerprint parameter entirely — apply will proceed with inline validation.");
}
```

**After:**
```cpp
else if (ErrorCode == TEXT("GRAPH_DRIFT"))
{
	Guidance = TEXT(
		"The graph fingerprint did not match because other operations modified the Blueprint "
		"between preview and apply. BEST PRACTICE: prefer calling apply_plan_json directly — "
		"it validates inline. If you do use preview, call it in a separate turn BEFORE apply, "
		"never in the same batch.");
}
```

## 4.8 Content Availability Timing

The recipe loader runs at editor startup. Adding/editing recipe `.txt` files requires an editor restart — not a rebuild, but not instant either. The doc has been corrected to say "no rebuild" not "instant."

Future enhancement: add a `olive.reload_recipes` tool or tie into hot-reload, but this is out of scope for V1.

---

# Part 5: Content Files

## 5.1 recipe_routing.txt

```
## Recipes
Call olive.get_recipe(category, name) before starting an unfamiliar task type.

BLUEPRINT: create, modify, fix_wiring, variables_components, edit_existing_graph

QUICK RULES:
- NEW blueprint → create + add_component/add_variable + apply_plan_json (ALL graph nodes in one call)
- MODIFY existing → project.search (find path) → blueprint.read → then write tools
- SMALL EDIT (1-2 nodes) → read_event_graph → add_node + connect_pins
- FIX wiring → use pin_manifests from the apply result, NOT blueprint.read
- NEVER call blueprint.read before blueprint.create
- NEVER use add_node one-at-a-time for 3+ nodes — use plan_json
- preview_plan_json is optional — prefer calling apply_plan_json directly. If you preview, do it in a separate turn.
- SpawnActor uses SpawnTransform (Transform), NOT separate Location/Rotation pins
- Object variable refs need class_name: {"category":"object","class_name":"BP_Gun_C"} (append _C)
```

## 5.2 _manifest.json

**Location:** `Content/SystemPrompts/Knowledge/recipes/_manifest.json`

```json
{
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
        }
      }
    }
  }
}
```

## 5.3 recipes/blueprint/create.txt

```
WORKFLOW:
1. blueprint.create → {path:"/Game/Blueprints/BP_Name", parent_class:"Actor"}
2. blueprint.add_component → {path, class:"StaticMeshComponent", name:"MyMesh"}
   blueprint.add_variable → {path, variable:{name:"Speed", type:{category:"float"}, default_value:"600.0"}}
   (batch all components + variables in one turn)
3. blueprint.apply_plan_json → {asset_path, graph_target:"EventGraph", plan_json:{...}}
   (ALL graph logic in ONE call — never add_node one-at-a-time for 3+ nodes)

EXAMPLE:
{"asset_path":"/Game/Blueprints/BP_Bullet","graph_target":"EventGraph",
 "plan_json":{"schema_version":"2.0","steps":[
   {"step_id":"evt","op":"event","target":"BeginPlay"},
   {"step_id":"get_xform","op":"call","target":"GetActorTransform"},
   {"step_id":"set_speed","op":"call","target":"SetPhysicsLinearVelocity",
    "inputs":{"NewVel":"@get_xform.~Forward","Target":"MeshComp"},
    "exec_after":"evt"}
]}}

GOTCHAS:
- Do NOT call blueprint.read before blueprint.create — the asset doesn't exist yet
- Do NOT call preview_plan_json in the same turn as apply — call apply_plan_json directly (or preview in a prior turn)
- SpawnActor uses SpawnTransform pin (Transform type), NOT Location/Rotation separately
- Variable type for object refs: {category:"object", class_name:"BP_Gun_C"} — append _C
- Data-provider steps (get_var, pure calls) MUST appear BEFORE steps that @ref them
```

## 5.4 recipes/blueprint/modify.txt

```
WORKFLOW:
1. project.search → {query:"BP_ThirdPersonCharacter"} (find EXACT asset path — never guess)
2. blueprint.read → {path:"/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter", mode:"summary"}
3. blueprint.read_event_graph → {path} (understand current graph before changing it)
4. add_variable / add_component as needed (batch in one turn)
5. blueprint.apply_plan_json → {asset_path, mode:"merge", plan_json:{...}}
   mode:"merge" preserves existing nodes. mode:"replace" clears graph first.

EXAMPLE:
{"asset_path":"/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter",
 "graph_target":"EventGraph","mode":"merge",
 "plan_json":{"schema_version":"2.0","steps":[
   {"step_id":"evt","op":"event","target":"BeginPlay"},
   {"step_id":"get_hp","op":"get_var","target":"Health"},
   {"step_id":"check","op":"branch","inputs":{"Condition":"@get_hp.auto"},
    "exec_after":"evt",
    "exec_outputs":{"True":"heal","False":"die"}}
]}}

GOTCHAS:
- ALWAYS search for the path first — paths vary by project (/Game/ThirdPerson/Blueprints/ not /Game/Blueprints/)
- Read the graph BEFORE writing — you need to know what events/nodes already exist
- If add_variable fails, do NOT reference that variable in subsequent plan_json calls
- Use mode:"merge" unless user explicitly wants to replace all existing logic
- The plan can reference existing event nodes — the executor reuses them instead of creating duplicates
```

## 5.5 recipes/blueprint/fix_wiring.txt

```
WORKFLOW:
1. Read the apply_plan_json result — it contains pin_manifests for failed connections
2. Each failed connection lists ACTUAL available pins: {name, display_name, type, direction}
3. Use blueprint.connect_pins with the ACTUAL pin names from the manifest
   Do NOT guess pin names. Do NOT call blueprint.read to discover pins (manifests already have them).

EXAMPLE (from a typical SpawnActor wiring failure):
apply_plan_json returned:
  "wiring_errors": [{"step_id":"spawn","hint":"Location","available_pins":[
    {"name":"SpawnTransform","type":"Transform"},
    {"name":"Class","type":"Actor Class Reference"}]}]

Fix: Use the node_id from the apply result and the correct pin from the manifest:
blueprint.connect_pins → {
  "path":"/Game/Blueprints/BP_Gun",
  "graph":"Fire",
  "source_ref":{"node_id":"<from apply result>","pin":"ReturnValue"},
  "target_ref":{"node_id":"<from apply result>","pin":"SpawnTransform"}}

GOTCHAS:
- Pin manifests in the apply result are GROUND TRUTH — use those exact names
- Node IDs come from the apply result (e.g. "node_0", "node_2") — never invent them
- Common mismatch: AI uses "Location"/"Rotation" but SpawnActor has "SpawnTransform" (Transform)
- Common mismatch: AI uses "Object" but Cast node has "Input" or the class-specific pin
- If multiple connections failed, batch all connect_pins calls in one turn
```

## 5.6 recipes/blueprint/variables_components.txt

```
WORKFLOW:
1. blueprint.add_variable → {path, variable:{name, type, default_value, flags}}
2. blueprint.add_component → {path, class, name, parent}
3. blueprint.modify_component → {path, name, properties:{...}}
   (batch all in one turn — components first if variables reference them)

VARIABLE TYPES:
Basic:     {"category":"float"} or shorthand "Float"
Bool:      {"category":"bool"} or "Boolean"
Int:       {"category":"int"} or "Integer"
String:    {"category":"string"} or "String"
Vector:    {"category":"struct","struct_name":"Vector"} or "Vector"
Rotator:   {"category":"struct","struct_name":"Rotator"} or "Rotator"
Transform: {"category":"struct","struct_name":"Transform"} or "Transform"
Object:    {"category":"object","class_name":"Actor"}
BP Object: {"category":"object","class_name":"BP_Gun_C"}  ← append _C for Blueprint classes
Class Ref: {"category":"class","class_name":"Actor"}       ← for TSubclassOf<>
Array:     {"category":"array","value_type":{"category":"float"}}
Enum:      {"category":"byte","enum_name":"ECollisionChannel"}

EXAMPLE:
{"path":"/Game/Blueprints/BP_Gun","variable":{
  "name":"EquippedBy","type":{"category":"object","class_name":"Character"},
  "default_value":"","flags":["BlueprintReadWrite","EditAnywhere"]}}

GOTCHAS:
- Object refs use class_name NOT sub_category or asset path
- Blueprint class refs MUST append _C: "BP_Gun_C" not "BP_Gun" or "/Game/Blueprints/BP_Gun"
- Component property values are UE format: Vectors use "(X=1,Y=2,Z=3)" NOT JSON objects
- SceneComponent is valid — use as hierarchy root when grouping child components
```

## 5.7 recipes/blueprint/edit_existing_graph.txt

```
WORKFLOW:
1. blueprint.read_event_graph → {path, graph_name:"EventGraph"} (see existing nodes + connections)
2. For SMALL edits (1-2 nodes): blueprint.add_node + blueprint.connect_pins
3. For LARGER edits: blueprint.apply_plan_json with mode:"merge" (preserves existing nodes)
4. To REMOVE: blueprint.remove_node → {path, graph, node_id} (get node_id from read result)
5. To REWIRE: blueprint.disconnect_pins then blueprint.connect_pins

FUNCTION GRAPHS:
- blueprint.read_function → {path, function_name:"Fire"} to read function graph
- Function entry node is auto-created — do NOT add an event/entry step
- First impure step: omit exec_after (auto-chains from entry node)
- graph_target in apply_plan_json: use the function name, e.g. "Fire"

EXAMPLE (add 2 nodes to existing EventGraph):
{"path":"/Game/Blueprints/BP_Gun","graph":"EventGraph",
 "type":"CallFunction","properties":{"function":"PrintString"}}
Then: connect_pins with source_ref/target_ref using node_ids from the results

EXAMPLE (function graph plan):
{"asset_path":"/Game/Blueprints/BP_Gun","graph_target":"Fire",
 "plan_json":{"schema_version":"2.0","steps":[
   {"step_id":"get_muzzle","op":"call","target":"GetWorldTransform",
    "inputs":{"Target":"MuzzlePoint"}},
   {"step_id":"spawn","op":"spawn_actor","target":"/Game/Blueprints/BP_Bullet",
    "inputs":{"SpawnTransform":"@get_muzzle.auto"}}
]}}
Note: spawn has NO exec_after — it auto-chains from function entry.

GOTCHAS:
- In function graphs, do NOT use exec_after for the first impure step (PLAN_INVALID_EXEC_AFTER)
- In function graphs, do NOT add event/custom_event steps — entry is automatic
- read_event_graph returns node_ids — use these for remove/disconnect, never guess
- mode:"merge" keeps existing nodes; mode:"replace" clears the graph first
```

---

# Part 6: Edge Cases & Mitigations

**AI Doesn't Call Recipe Tool:** Routing table QUICK RULES give enough for common cases. Tool matters for uncommon patterns (fix_wiring, edit_existing_graph). Fallback: self-correction kicks in.

**Recipe Content Drift:** Loader validates tool name patterns against `FOliveToolRegistry` at startup. Logs warnings for unregistered references.

**Multiple Recipes Apply:** Design recipes to be self-contained. Batch fetch: `olive.get_recipe("blueprint", "create,variables_components")` returns both in one call.

**Session Context Loss:** Routing table is always present with essential rules. AI re-fetches recipes each new task type.

**Conflicting Instructions:** Wrapper rules now align with routing table. Known conflict resolved: preview mandate removed from both.

**Token Budget:** ~130 tokens (routing) + ~200 tokens (blueprint_authoring) = ~330 total. With 4000 MaxTokens, leaves ~3,500+ for everything else.

**Initialization Order:** Guard in BuildSharedSystemPreamble checks CapabilityKnowledgePacks.Num() before returning content.

---

# Part 7: Verification Checklist

**CLI Wrapper Rewrite:**
- [ ] BuildSystemPrompt() replaced with new version
- [ ] Plan JSON example uses SpawnTransform (not Location)
- [ ] No mention of "Always call preview_plan_json first"
- [ ] Function graph exec flow section present
- [ ] Variable type reference includes object, class, array, enum
- [ ] Workflows section includes MODIFY with project.search step
- [ ] Cascading failure rule present
- [ ] `#include "Chat/OlivePromptAssembler.h"` added

**Recipe System — API Compliance:**
- [ ] RegisterTool uses `(name, description, InputSchema, handler, tags, category)` — NOT FOliveToolParam
- [ ] InputSchema built via OliveCrossSystemSchemas:: helper (MakeSchema + MakeProperties)
- [ ] FOliveToolResult::Success receives `TSharedPtr<FJsonObject>` — NOT raw FString
- [ ] FOliveToolResult::Error uses `(code, message, suggestion)` three-arg form
- [ ] RecipeGetRecipe() schema added to OliveCrossSystemSchemas namespace

**Tool Pack Gating:**
- [ ] `olive.get_recipe` added to `read_pack` in Config/OliveToolPacks.json
- [ ] `olive.get_recipe` added to RegisterDefaultPacks() fallback in OliveToolPackManager.cpp
- [ ] Tool tags include `"read"` so pack filtering works

**Self-Correction Alignment:**
- [ ] GRAPH_DRIFT guidance updated — no longer pushes preview-first
- [ ] New guidance: "prefer direct apply; if preview, separate turn" matches wrapper + recipes

**Recipe System — Functional:**
- [ ] `recipe_routing.txt` in system prompt for Blueprint and Auto profiles (NOT C++)
- [ ] `olive.get_recipe("blueprint")` returns JSON with `categories` or `recipes` array
- [ ] `olive.get_recipe("blueprint", "create")` returns JSON with `content` field
- [ ] `olive.get_recipe("nonexistent")` returns Error with code + message + suggestion
- [ ] Batch: `olive.get_recipe("blueprint", "create,modify")` returns array of results
- [ ] New .txt + manifest update + editor restart → available (no rebuild, but not instant)

**Integration:**
- [ ] Claude Code provider includes preamble before wrapper content
- [ ] BuildSharedSystemPreamble() returns project + policies + knowledge
- [ ] API providers still get knowledge packs through existing flow
- [ ] Initialization order correct
- [ ] No conflicting instructions across: routing table, recipes, wrapper, AND self-correction hints

---

# Part 8: File Inventory

| File | Action | Lines |
|------|--------|-------|
| `OliveClaudeCodeProvider.cpp` | REPLACE BuildSystemPrompt(), add `#include` | ~90 |
| `OlivePromptAssembler.h` | ADD BuildSharedSystemPreamble() declaration | ~8 |
| `OlivePromptAssembler.cpp` | ADD implementation + profile mappings (no C++ profile) | ~30 |
| `OliveCrossSystemSchemas.h` | ADD RecipeGetRecipe() declaration | ~1 |
| `OliveCrossSystemSchemas.cpp` | ADD RecipeGetRecipe() schema function | ~12 |
| `OliveCrossSystemToolHandlers.h` | ADD declarations + storage members | ~12 |
| `OliveCrossSystemToolHandlers.cpp` | ADD loader, registration, handler | ~170 |
| `OliveToolPackManager.cpp` | ADD `olive.get_recipe` to ReadPack default, update GRAPH_DRIFT guidance | ~4 |
| `Config/OliveToolPacks.json` | ADD `olive.get_recipe` to `read_pack` | ~1 |
| `IOliveAIProvider.h` | ADD documentation comment about preamble contract | ~6 |
| `recipe_routing.txt` | CREATE | ~130 tokens |
| `recipes/_manifest.json` | CREATE | ~1KB |
| `recipes/blueprint/create.txt` | CREATE | ~220 tokens |
| `recipes/blueprint/modify.txt` | CREATE | ~200 tokens |
| `recipes/blueprint/fix_wiring.txt` | CREATE | ~170 tokens |
| `recipes/blueprint/variables_components.txt` | CREATE | ~240 tokens |
| `recipes/blueprint/edit_existing_graph.txt` | CREATE | ~230 tokens |

**Total C++ changes:** ~333 lines across 8 existing files (0 new C++ files)
**Total content files:** 7 new
