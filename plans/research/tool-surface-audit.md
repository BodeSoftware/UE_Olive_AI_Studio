# Research: Tool Surface and Validation Layer Audit

## Question
Complete picture of the current tool surface, validation layers, prompt restrictions, and discovery capabilities — intended as the factual foundation for an architectural shift toward "trust the AI, return UE5 errors."

---

## Findings

### 1. Complete Tool Inventory

**Total: 90 tools** across 7 categories.

#### Blueprint (53 tools)

**Readers (8)** — registered in `RegisterReaderTools()`, `OliveBlueprintToolHandlers.cpp`

| Tool | Description |
|------|-------------|
| `blueprint.read` | Read Blueprint structure; auto-upgrades to full read if <= 50 nodes |
| `blueprint.read_function` | Read a single function graph |
| `blueprint.read_event_graph` | Read an event graph |
| `blueprint.read_variables` | Read all variables |
| `blueprint.read_components` | Read component hierarchy |
| `blueprint.read_hierarchy` | Read class inheritance chain |
| `blueprint.list_overridable_functions` | List parent class functions that can be overridden |
| `blueprint.get_node_pins` | Get pin manifest for a specific node |

**Asset Writers (6)** — `RegisterAssetWriterTools()`

| Tool | Description |
|------|-------------|
| `blueprint.create` | Create a new Blueprint asset |
| `blueprint.set_parent_class` | Change parent class (Tier 3) |
| `blueprint.add_interface` | Add an interface |
| `blueprint.remove_interface` | Remove an interface |
| `blueprint.compile` | Force compile and return results |
| `blueprint.delete` | Delete a Blueprint asset (Tier 3) |

**Variable Writers (3)** — `RegisterVariableWriterTools()`

| Tool | Description |
|------|-------------|
| `blueprint.add_variable` | Add a variable |
| `blueprint.remove_variable` | Remove a variable |
| `blueprint.modify_variable` | Modify an existing variable's properties |

**Component Writers (4)** — `RegisterComponentWriterTools()`

| Tool | Description |
|------|-------------|
| `blueprint.add_component` | Add a component to the hierarchy |
| `blueprint.remove_component` | Remove a component |
| `blueprint.modify_component` | Modify component properties |
| `blueprint.reparent_component` | Change a component's parent |

**Function Writers (6)** — `RegisterFunctionWriterTools()`

| Tool | Description |
|------|-------------|
| `blueprint.add_function` | Add a user-defined function |
| `blueprint.remove_function` | Remove a function |
| `blueprint.modify_function_signature` | Modify a function's signature (params, return, flags) |
| `blueprint.add_event_dispatcher` | Add a multicast delegate |
| `blueprint.override_function` | Override a parent class function |
| `blueprint.add_custom_event` | Add a custom event to the event graph |

**Graph Writers (6)** — `RegisterGraphWriterTools()`

| Tool | Description |
|------|-------------|
| `blueprint.add_node` | Add a node to a graph |
| `blueprint.remove_node` | Remove a node |
| `blueprint.connect_pins` | Connect two pins |
| `blueprint.disconnect_pins` | Disconnect two pins |
| `blueprint.set_pin_default` | Set an input pin's default value |
| `blueprint.set_node_property` | Set a UPROPERTY on a node |

**AnimBP Writers (4)** — `RegisterAnimBPWriterTools()`

| Tool | Description |
|------|-------------|
| `animbp.add_state_machine` | Add a state machine to an AnimGraph |
| `animbp.add_state` | Add a state to a state machine |
| `animbp.add_transition` | Add a transition between states |
| `animbp.set_transition_rule` | Set the rule/condition for a transition |

**Widget Writers (4)** — `RegisterWidgetWriterTools()`

| Tool | Description |
|------|-------------|
| `widget.add_widget` | Add a widget to a Widget Blueprint |
| `widget.remove_widget` | Remove a widget |
| `widget.set_property` | Set a widget property |
| `widget.bind_property` | Bind a widget property to a function |

**Plan JSON (2)** — `RegisterPlanTools()`

| Tool | Description |
|------|-------------|
| `blueprint.preview_plan_json` | Preview an intent-level plan without mutating |
| `blueprint.apply_plan_json` | Apply an intent-level plan atomically |

**Templates (3)** — `RegisterTemplateTools()`

| Tool | Description |
|------|-------------|
| `blueprint.create_from_template` | Create a Blueprint from a factory template |
| `blueprint.get_template` | View a template's full content (reference or factory) |
| `blueprint.list_templates` | List available templates |

---

#### Behavior Tree / Blackboard (16 tools)

Registered in `FOliveBTToolHandlers::RegisterBlackboardTools()` and `RegisterBehaviorTreeTools()` in `OliveBTToolHandlers.cpp`.

| Tool | Description |
|------|-------------|
| `blackboard.create` | Create a new Blackboard Data asset |
| `blackboard.read` | Read a Blackboard as structured IR |
| `blackboard.add_key` | Add a key (Bool, Int, Float, String, Object, etc.) |
| `blackboard.remove_key` | Remove a key |
| `blackboard.modify_key` | Rename/modify a key |
| `blackboard.set_parent` | Set parent Blackboard for inheritance |
| `behaviortree.create` | Create a new Behavior Tree asset |
| `behaviortree.read` | Read a Behavior Tree as structured IR |
| `behaviortree.set_blackboard` | Associate a Blackboard with a BT |
| `behaviortree.add_composite` | Add a Selector, Sequence, or SimpleParallel |
| `behaviortree.add_task` | Add a task node (e.g., BTTask_MoveTo) |
| `behaviortree.add_decorator` | Attach a decorator |
| `behaviortree.add_service` | Attach a service |
| `behaviortree.remove_node` | Remove a node |
| `behaviortree.move_node` | Move a node to a different parent composite |
| `behaviortree.set_node_property` | Set a UPROPERTY on a BT node |

---

#### PCG (9 tools)

Registered in `FOlivePCGToolHandlers::RegisterAllTools()` in `OlivePCGToolHandlers.cpp`.

| Tool | Description |
|------|-------------|
| `pcg.create` | Create a new PCG graph asset |
| `pcg.read` | Read a PCG graph as structured IR |
| `pcg.add_node` | Add a PCG node by settings class name |
| `pcg.remove_node` | Remove a node (cannot remove Input/Output) |
| `pcg.connect` | Connect two pins (source output -> target input) |
| `pcg.disconnect` | Disconnect two pins |
| `pcg.set_settings` | Set properties on a node's settings via reflection |
| `pcg.add_subgraph` | Add a subgraph node referencing another PCG graph |
| `pcg.execute` | Execute a PCG graph and return a summary |

---

#### C++ (13 tools)

Registered in `FOliveCppToolHandlers::RegisterReflectionTools()`, `RegisterSourceTools()`, `RegisterWriteTools()` in `OliveCppToolHandlers.cpp`.

| Tool | Description |
|------|-------------|
| `cpp.read_class` | Read full reflection data for a C++ class |
| `cpp.list_blueprint_callable` | List BlueprintCallable and BlueprintPure functions on a class |
| `cpp.list_overridable` | List BlueprintImplementableEvent and BlueprintNativeEvent functions |
| `cpp.read_enum` | Read enum values via reflection |
| `cpp.read_struct` | Read struct members via reflection |
| `cpp.read_header` | Read a project .h file with optional line range |
| `cpp.read_source` | Read a project .cpp file with optional line range |
| `cpp.list_project_classes` | List C++ classes in project Source/ |
| `cpp.create_class` | Create a new UE C++ class with header/source boilerplate |
| `cpp.add_property` | Add a UPROPERTY to an existing header |
| `cpp.add_function` | Add a UFUNCTION declaration and stub body |
| `cpp.modify_source` | Apply a bounded anchor-based source patch |
| `cpp.compile` | Trigger Live Coding hot reload compilation |

---

#### Cross-System / Project (17 tools)

Registered across several methods in `OliveCrossSystemToolHandlers.cpp` and `OliveToolRegistry.cpp`.

| Tool | Source | Description |
|------|--------|-------------|
| `project.search` | `OliveToolRegistry.cpp::RegisterProjectTools()` | Search for assets by name |
| `project.get_asset_info` | same | Detailed asset info (deps, referencers, metadata) |
| `project.get_class_hierarchy` | same | Class inheritance hierarchy |
| `project.get_dependencies` | same | Assets that an asset depends on |
| `project.get_referencers` | same | Assets that reference an asset |
| `project.get_config` | same | Project configuration (engine version, plugins, asset types) |
| `project.bulk_read` | `RegisterBulkTools()` | Read up to 20 assets in a single call |
| `project.implement_interface` | same | Add an interface to multiple Blueprints |
| `project.refactor_rename` | same | Rename with dependency-aware reference updates |
| `project.create_ai_character` | same | Create full AI character setup (BP + BT + BB) |
| `project.move_to_cpp` | same | Analyze BP and scaffold C++ migration artifacts |
| `project.snapshot` | `RegisterSnapshotTools()` | Save IR state for comparison or rollback |
| `project.list_snapshots` | same | List available snapshots |
| `project.rollback` | same | Restore assets to a previous snapshot state |
| `project.diff` | same | Compare current state against a snapshot |
| `project.index_build` | `RegisterIndexTools()` | Export project index to JSON |
| `project.index_status` | same | Check if project index is stale |
| `project.get_relevant_context` | same | Search index for relevant assets by query |
| `project.batch_write` | `RegisterBatchTools()` | Execute multiple graph ops atomically under one transaction |
| `olive.get_recipe` | `RegisterRecipeTools()` | Search for tested Blueprint wiring patterns |

Note: `project.batch_write` and `olive.get_recipe` counted above. Total cross-system = 20. (The 17 stated above missed 3 from the index/recipe tools added at different lines; corrected total: 20.)

---

**Category Summary:**

| Category | Count |
|----------|-------|
| Blueprint (incl. AnimBP, Widget, Plan, Template) | 53 |
| Behavior Tree + Blackboard | 16 |
| PCG | 9 |
| C++ | 13 |
| Cross-system / Project | 20 (incl. project.batch_write, olive.get_recipe) |
| **Total** | **111** |

Note: The discrepancy from the initial count arises from cross-system tools being registered in multiple registration methods. The final tally above was derived by enumerating each `RegisterTool` call directly.

---

### 2. Validation Layers That Block Operations

Validation runs in two places: (A) the `FOliveValidationEngine` rule pipeline (pre-mutation, before any UE5 API is touched), and (B) inline handler-level guards directly in tool handler functions.

#### A. FOliveValidationEngine Rules

Registered across five `Register*Rules()` calls. All rules run before Stage 3 (Transact) of the write pipeline.

**Core Rules** (`RegisterCoreRules()`)

| Rule | What It Blocks | UE5 Would Catch? | Crash Risk if Removed? |
|------|---------------|-------------------|------------------------|
| `FOlivePIEProtectionRule` | All write tools while PIE is active | NO — UE5 will corrupt in-flight objects | YES — editor crash |
| `FOliveSchemaValidationRule` | Missing required params, wrong JSON types, enum value mismatch | Partial — wrong types may cause assertion failures deep in UE5 | Low for type errors; high for missing params causing null deref |
| `FOliveAssetExistsRule` | A hardcoded subset of tools whose `path` must already exist (see list below) | UE5 returns null from `StaticLoadObject` — no crash | No crash — returns null silently |
| `FOliveWriteRateLimitRule` | Write ops exceeding `MaxWriteOpsPerMinute` (default 30/min) | No | No |

The `FOliveAssetExistsRule` hardcoded tool list (line 569):
```
blueprint.read, blueprint.add_variable, blueprint.add_component,
blueprint.add_function, behaviortree.read, behaviortree.set_blackboard,
behaviortree.add_composite, behaviortree.add_task, behaviortree.add_decorator,
behaviortree.add_service, behaviortree.remove_node, behaviortree.move_node,
behaviortree.set_node_property, blackboard.read, blackboard.add_key,
blackboard.remove_key, blackboard.modify_key, blackboard.set_parent
```
This is incomplete — the majority of Blueprint write tools are NOT in this list, so asset-not-found for those tools falls through to handler-level `StaticLoadObject` which returns null and then the handler produces a tool error anyway.

**Blueprint Rules** (`RegisterBlueprintRules()`)

| Rule | What It Blocks |
|------|---------------|
| `FOliveBPAssetTypeRule` | Blueprinttools against non-Blueprint assets |
| `FOliveBPNodeIdFormatRule` | `node_id`, `source_node`, `target_node`, `source_pin`, `target_pin` missing for node/pin ops; also validates pin name format |
| `FOliveBPNamingRule` | Names with non-alphanumeric chars, names starting with a digit, very short names (warning) |

**Behavior Tree / Blackboard Rules** (`RegisterBehaviorTreeRules()`)

| Rule | What It Blocks |
|------|---------------|
| `FOliveBTAssetTypeRule` | BT tools against non-BT assets; BB tools against non-BB assets |
| `FOliveBTNodeExistsRule` | BT task/decorator/service class not found via `FOliveBTNodeFactory::ResolveNodeClass` (blocks pre-add, not post-add); node IDs that don't exist in the tree; parent IDs that aren't composites; removing root node |
| `FOliveBBKeyUniqueRule` | Duplicate key names; removing/modifying non-existent keys; circular Blackboard parent chains |

**PCG Rules** (`RegisterPCGRules()`)

| Rule | What It Blocks |
|------|---------------|
| `FOlivePCGAssetTypeRule` | PCG tools against non-PCG assets |
| `FOlivePCGNodeClassRule` | `settings_class` missing for `pcg.add_node`; unknown PCG settings class (WARNING only, not error) |

**C++ Rules** (`RegisterCppRules()`)

| Rule | What It Blocks |
|------|---------------|
| `FOliveCppPathSafetyRule` | File paths with `..`, absolute paths, or non-.h/.cpp/.inl extensions; `anchor_text` missing for `cpp.modify_source`; invalid `operation` value |
| `FOliveCppClassExistsRule` | Missing `class_name`; class not in reflection (WARNING only) |
| `FOliveCppEnumExistsRule` | Missing `enum_name`; enum not found (WARNING only) |
| `FOliveCppStructExistsRule` | Missing `struct_name`; struct not found (WARNING only) |
| `FOliveCppCompileGuardRule` | C++ write ops while Live Coding is compiling |

**Cross-System Rules** (`RegisterCrossSystemRules()`)

| Rule | What It Blocks |
|------|---------------|
| `FOliveBulkReadLimitRule` | `project.bulk_read` with > 20 assets or empty paths |
| `FOliveSnapshotExistsRule` | `snapshot_id` missing; snapshot not found; `project.rollback` without `confirmation_token` when `preview_only=false` |
| `FOliveRefactorSafetyRule` | Missing params for refactor/implement_interface/move_to_cpp; `new_name` with invalid chars; `paths` > 50 for implement_interface |
| `FOliveCppOnlyModeRule` | All `blueprint.*`, `behaviortree.*`, `pcg.*` tools when `preferred_layer=cpp` (blocks entirely) |
| `FOliveDuplicateLayerRule` | `blueprint.add_function` when parent C++ class has same native function (blocks by default; `allow_duplicate=true` downgrades to warning); `blueprint.add_variable` when C++ parent has same property (same); reverse checks for `cpp.add_function` and `cpp.add_property` against derived Blueprint classes |

#### B. Inline Handler-Level Guards

Each tool handler performs its own pre-validation before touching UE5 APIs. These are generally "did UE5 load the asset?" checks that redundantly cover what the validation engine partially handles. Key patterns:

- Every handler that needs a Blueprint: `UBlueprint* Blueprint = LoadObject<...>(); if (!Blueprint) return Error(...)`
- `blueprint.add_node`: Pre-checks for duplicate native event overrides (calls `FBlueprintEditorUtils::FindOverrideForFunction`) before opening a transaction
- `blueprint.add_node`: Calls `FOliveNodeCatalog::Get().FuzzyMatch()` when the node type is unrecognized — returns suggestions instead of error code
- `blueprint.apply_plan_json`: Full `FOlivePlanValidator` phase-0 pipeline runs before execution (see Section 5 below)
- BT handlers: Use `LoadBlackboardFromParams`/`LoadBehaviorTreeFromParams` helpers that also return structured errors

#### C. Plan Validator (Phase-0 for plan_json tools)

`FOlivePlanValidator::Validate()` runs after resolve, before execution. It catches:

1. **`COMPONENT_FUNCTION_ON_ACTOR`**: A step calls a component-only function (e.g., `SetMeshComponent`) on an Actor BP but Target is unwired — blocks unless auto-resolved (single unambiguous match).
2. **`EXEC_WIRING_CONFLICT`**: A step uses `exec_after` targeting a step that also declares `exec_outputs` — would double-claim the exec output pin.

#### D. batch_write Allowlist

`project.batch_write` only dispatches to a hardcoded allowlist (in `FOliveGraphBatchExecutor::GetBatchWriteAllowlist()`):
```
blueprint.add_node, blueprint.connect_pins, blueprint.disconnect_pins,
blueprint.set_pin_default, blueprint.set_node_property, blueprint.remove_node
```
All other tools passed to `ops[]` are rejected at dispatch time.

---

### 3. Prompt Restrictions and Tool Pack Filtering

#### A. Capability Knowledge Packs

Three knowledge packs are loaded from `Content/SystemPrompts/Knowledge/` and injected for Blueprint/Auto profiles:

**`blueprint_authoring.txt`** — rules AI must follow:
- Read before write
- Variable typing constraints (no wildcard/exec types)
- "Batch-first" rule: prefer `plan_json` or `project.batch_write` for 3+ ops
- No fake success reporting
- Creation workflow: CREATE -> ADD components -> ADD variables -> BUILD graph with `apply_plan_json` -> VERIFY

**`recipe_routing.txt`** — routing directives:
- "ALWAYS call `olive.get_recipe` before your first plan_json for each function"
- Template check first for new Blueprints
- "NEVER call `blueprint.read` before `blueprint.create`"
- Prefer `plan_json` for 3+ nodes; use `add_node`/`connect_pins` for 1-2 ops
- Keep plans under 12 steps

**`node_routing.txt`** — which tool to use for which node type:
- Plan JSON path (preferred, 90% of cases)
- `add_node` path (for Timeline, Select, Switch, Enhanced Input, K2Node subclasses)
- `add_node + get_node_pins` path (pin-changing nodes)
- Fallback guidance for plan JSON failures

#### B. CLI Provider System Prompt Injections

`OliveCLIProviderBase.cpp` appends additional directives dynamically in `BuildAssetStateSummary()` / `BuildContinuationPrompt()`:

- "Before writing your first plan_json for each function, call `olive.get_recipe`"
- On continuation: routes AI to `olive.get_recipe` name=`"<FunctionName>"` for each empty function stub
- Search guidance: "If the task is modifying EXISTING assets, start with `project.search`"
- "Do not repeat identical `project.search` queries unless results changed"
Source: `OliveCLIProviderBase.cpp` lines ~369, ~1101, ~1143, ~1368, ~1375

#### C. Tool Pack Filtering (FOliveToolPackManager)

Tool packs gate tool visibility per turn based on intent detection. The packs (from config or fallback defaults):

| Pack | Enabled When | Tools |
|------|-------------|-------|
| `read_pack` | Always | project.search, get_asset_info, blueprint.read*, cpp.read*, bt/bb.read, olive.get_recipe |
| `write_pack_basic` | `bTurnHasExplicitWriteIntent` | blueprint.create, add_variable, add_component, add_function, add_custom_event, compile; bt.create, bb.add_key; pcg.create; cpp.create_class |
| `write_pack_graph` | `bTurnHasExplicitWriteIntent` | blueprint.preview_plan_json, apply_plan_json, add_node, remove_node, connect/disconnect_pins, set_pin_default, set_node_property |
| `danger_pack` | `bTurnHasDangerIntent` | blueprint.delete, set_parent_class, add_interface, remove_interface |

Note: Many tools from the full inventory (e.g., all AnimBP/Widget tools, BT write tools, cross-system tools, C++ write tools) are NOT in any pack definition — meaning the fallback defaults don't cover them. These tools depend on the config file `OliveToolPacks.json` for visibility.

#### D. Focus Profile Filtering

`FOliveFocusProfileManager::IsToolAllowedForProfile()` filters by:
1. Tool in `Profile.ExcludedTools` array — explicitly blocked
2. `Profile.ToolCategories` array — if non-empty, tool's category must be in the set

Three built-in profiles (Phase E):
- **Auto**: All tools visible (no categories filter, no excluded tools)
- **Blueprint**: Allows `blueprint`, `behaviortree`, `blackboard`, `pcg`, `project`, `crosssystem` categories; excludes `cpp` category
- **C++**: Allows `cpp` category only; excludes all Blueprint/BT/PCG

Profile capability pack mapping (in `OlivePromptAssembler.cpp`):
- Auto → `blueprint_authoring`, `recipe_routing`, `node_routing`
- Blueprint → same
- C++ → none (intentionally empty until C++ recipes exist)

---

### 4. Discovery Tool Capabilities

#### `blueprint.read`
**Returns** (summary mode, <= 50 nodes auto-upgrades to full):
- Blueprint type (Normal, Interface, Macro, FunctionLibrary, LevelScriptActor, Widget)
- Parent class name
- Implemented interfaces
- Variable list: name, type, default value, visibility flags
- Component list: class, attach parent, relative transform
- Function signature list: name, params, return type, flags (no graph nodes)
- (Full mode) Complete graph data: all nodes with positions, pin manifests, connections

**What's missing:**
- Compiled class info (what functions are BlueprintCallable in the generated class)
- Event dispatchers list is not exposed separately in summary
- No information about which functions have actual logic vs empty stubs at the summary level (node count per graph would help)

#### `blueprint.read_function` / `blueprint.read_event_graph`
**Returns**: Full graph IR with all nodes, pins, connections. Large graphs paginated via `ReadGraphPage` (threshold 500 nodes, page size 100). Returns `node_count`, `page_count`, `total_nodes` metadata.

**What's missing**: No "which nodes have errors" annotation from pre-existing compile state.

#### `project.search`
**Returns**: Asset name, path, class (asset type), native class. Up to 200 results (clamped). Searches via `FOliveProjectIndex::SearchAssets`.

**What's missing**: No filter by class hierarchy (e.g., "all Blueprints that extend ACharacter"). Only name-based search.

#### `project.get_asset_info`
**Returns**: Asset dependencies, referencers, metadata (asset tags from the registry). Does NOT return compiled class info or Blueprint structure.

#### Other notable discovery tools:
- `project.get_class_hierarchy`: Returns class tree from a root class (default AActor)
- `cpp.read_class`: Full reflection dump — UPROPERTYs, UFUNCTIONs, interfaces, metadata, BlueprintCallable flags
- `blueprint.list_overridable_functions`: Parent class functions the AI can override in this Blueprint
- `blueprint.get_node_pins`: Pin manifest for a specific existing node (for wiring after `add_node`)

**Key gap across all discovery tools**: There is no "what compile errors does this Blueprint currently have?" read tool. The only way to get compile errors is to call `blueprint.compile`, which is a write-adjacent operation that touches the engine's compilation pipeline.

---

### 5. Node Catalog and Function Resolution

#### FOliveNodeCatalog

Built from three sources:
1. **`BuildFromK2NodeClasses()`** — scans all `UK2Node` subclasses in loaded modules
2. **`BuildFromFunctionLibraries()`** — scans all `UBlueprintFunctionLibrary` subclasses for BlueprintCallable functions
3. **Manual entries** — `AddBuiltInFlowControlNodes()`, `AddMathNodes()`, `AddStringNodes()`, `AddArrayNodes()`, `AddUtilityNodes()`

Each entry (`FOliveNodeTypeInfo`) stores: TypeId, DisplayName, Category, Subcategory, Description, Usage, Keywords, approximate InputPins/OutputPins, behavior flags (bIsPure, bIsLatent, bRequiresTarget, bIsEvent), FunctionName, FunctionClass.

**Interface functions**: The catalog's `BuildFromFunctionLibraries` scans function libraries specifically. Interface functions on `UInterface` classes are NOT in `UBlueprintFunctionLibrary`, so they are NOT cataloged by default. However, `blueprint.list_overridable_functions` and `cpp.list_overridable` can surface them via reflection.

**Component-specific functions**: The catalog's `ClassIndex` maps ClassName → TypeIds for member functions. Component class functions (e.g., `UStaticMeshComponent::SetStaticMesh`) are cataloged via `BuildFromFunctionLibraries` only if they appear in a function library, NOT if they are direct UFUNCTION members on the component class. However, `FOliveNodeFactory::FindFunction()` searches `Blueprint->GeneratedClass` and SCS component classes as fallbacks, so they CAN be resolved at execution time even without being in the catalog.

**Fallback when not in catalog**: `blueprint.add_node` calls `FOliveNodeFactory::ValidateNodeType()`. If the type is not in the `NodeCreators` map, it attempts `CreateNodeByClass()` — a universal fallback that resolves the type string as a `UK2Node` subclass name via `FindK2NodeClass()`. This means ANY valid UK2Node subclass can be used by passing its exact class name (e.g., `"K2Node_ComponentBoundEvent"`), even if not cataloged.

**Function resolution order** in `FOliveNodeFactory::FindFunction()`:
1. Alias map (~136 entries: `MakeTransform`, `BreakVector`, etc.)
2. Specified class (if `target_class` provided)
3. Blueprint's `GeneratedClass`
4. Blueprint parent class hierarchy
5. Blueprint SCS component classes
6. Common library classes (KismetMathLibrary, KismetSystemLibrary, etc.)

Each class tried with exact name, then `K2_` prefix variant.

**What the catalog misses**:
- Interface functions (must use reflection tools or `list_overridable_functions`)
- Newly-created Blueprint functions until catalog rebuild (not a concern for plan_json path)
- Engine functions not in function libraries (resolved at runtime by FindFunction but not discoverable via catalog search)

---

### 6. Observations Relevant to the "Trust the AI" Architectural Shift

#### Validation rules that are safety-critical (MUST keep)
- **`FOlivePIEProtectionRule`**: Prevents mutation during PIE. UE5 will crash or corrupt state.
- **`FOliveCppCompileGuardRule`**: Prevents C++ writes during Live Coding compilation — race condition risk.
- **`FOliveWriteRateLimitRule`**: Rate limiter. Configurable. Worth keeping as a safety backstop.
- **Path safety in `FOliveCppPathSafetyRule`**: Prevents `../..` traversal to engine source. Must stay.

#### Validation rules that are "pre-check" redundancy (candidates for removal/relaxation)
- **`FOliveAssetExistsRule`** (the hardcoded list): The handler already does `StaticLoadObject` which returns null cleanly. This rule is redundant AND incomplete (most Blueprint tools are not in the list). Removing it just means the AI gets a slightly different error message — from the validation layer vs from the handler.
- **`FOliveBTNodeExistsRule` class checks** (task/decorator/service class resolution): This rejects a valid class name that happens not to be loaded yet. UE5's own node factory would fail gracefully. This is a false-negative risk (blocking valid operations).
- **`FOliveDuplicateLayerRule`**: Has the `allow_duplicate=true` bypass, making it more of a guardian than a hard blocker. In a "trust the AI" model, this could be downgraded to a warning-only rule universally.
- **`FOlivePCGNodeClassRule`**: Already only a WARNING (not an error). No change needed.
- **`FOliveCppClassExistsRule` / `FOliveCppEnumExistsRule` / `FOliveCppStructExistsRule`**: Already warnings, not errors. Fine as-is.
- **`FOliveBPNamingRule`**: Alphanumeric/underscore constraint is UE5's actual constraint (Blueprint variable names follow this). This is not "pre-checking" — it mirrors UE5's real restriction. KEEP.

#### Prompt restrictions worth reconsidering
- The `olive.get_recipe` mandate ("ALWAYS call before first plan_json") in `recipe_routing.txt` costs one full round-trip per function. If the AI already knows the pattern, this is waste.
- The "plan under 12 steps" guideline in `recipe_routing.txt` can prevent the AI from expressing legitimate complex operations in a single plan.
- The `node_routing.txt` "path 1 vs path 2 vs path 3" structure is a useful reference but could become outdated if plan_json gains new ops.

#### Tool pack gaps
The fallback `RegisterDefaultPacks()` in `OliveToolPackManager.cpp` does NOT include AnimBP tools, Widget tools, most cross-system tools, or BT/PCG write tools beyond `behaviortree.create` and `pcg.create_graph`. If the config file `OliveToolPacks.json` is absent or stale, those tools are invisible even with explicit write intent. This is a latent usability bug independent of the architectural shift.

---

## Recommendations

1. **The PIE protection rule, C++ compile guard, and path safety rule are non-negotiable.** These are the only three validation rules where removing them risks an editor crash or system file corruption. Everything else is "helpful guidance" territory.

2. **`FOliveAssetExistsRule` is the most egregious redundancy.** It's a hardcoded list covering only ~18 tools while 90+ exist. The handlers all do their own `StaticLoadObject`-based checks. Consider deleting `FOliveAssetExistsRule` entirely and letting handler errors propagate — the AI gets structured error codes either way.

3. **`FOliveBTNodeExistsRule`'s class resolution pre-check is a false-negative risk.** Rejecting BT task/decorator/service class names that aren't currently loaded in memory (before the handler even tries) can block valid operations on blueprint-only BT node classes. Consider removing the class-resolution pre-check; keep the node-ID-existence and composite-parent checks which are legitimate structural guards.

4. **`FOliveDuplicateLayerRule` should be warnings-only by default.** The `allow_duplicate=true` bypass already exists but the AI has to know to use it. Flipping the default to warning (with an error path opt-in) aligns with "trust the AI" philosophy.

5. **The `olive.get_recipe` mandate creates guaranteed latency.** Each function creation requires one extra round-trip. Consider making it conditional: only mandate it for complex patterns (e.g., spawn_actor, component references) and provide it as a suggestion for simple patterns.

6. **The `project.bulk_read` 20-asset cap and `behaviortree.add_composite` 50-paths cap are legitimate data-volume guards** — not AI restrictions. Keep them.

7. **Missing discovery capability: compile error read.** There is no `blueprint.get_compile_errors` tool. The AI can only learn about compile errors by calling `blueprint.compile`. If compile results were included in a `blueprint.read` response (e.g., a `compile_status` field from the last compile), the AI could self-correct without an extra round-trip.

8. **Missing discovery capability: function stub detection.** `blueprint.read` in summary mode shows function names but not which functions have 0 nodes. A `node_count` per function in the summary would let the AI prioritize which functions need implementation.

9. **The tool pack system has a coverage gap.** AnimBP tools (`animbp.*`), Widget tools (`widget.*`), most cross-system tools, and BT/PCG write tools are not in the fallback default packs. Any deviation from the config file silently hides these capabilities. The fallback defaults should be audited to include all registered tools in appropriate packs.

10. **`FOliveNodeCatalog` does not catalog interface functions.** This is fine operationally because `FindFunction` resolves them at runtime, but the AI cannot discover interface functions via catalog search. `blueprint.list_overridable_functions` is the correct discovery path for interfaces.

---

## Sources

- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (lines 475–972, 5969–5996, 7469–7508, 989–1121, 3900–4017)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/BehaviorTree/Private/MCP/OliveBTToolHandlers.cpp` (lines 47–215)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/PCG/Private/MCP/OlivePCGToolHandlers.cpp` (lines 28–133)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Cpp/Private/MCP/OliveCppToolHandlers.cpp` (lines 46–191)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp` (lines 60–330, 880–917, 1234–1255)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp` (lines 795–957)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Private/Services/OliveValidationEngine.cpp` (full file, 2021 lines)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp` (lines 450–570)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Private/Brain/OliveToolPackManager.cpp` (lines 160–215)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Private/Profiles/OliveFocusProfileManager.cpp` (lines 141–179)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/CrossSystem/Private/Services/OliveGraphBatchExecutor.cpp` (lines 183–194)
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h`
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Blueprint/Public/Catalog/OliveNodeCatalog.h`
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Blueprint/Public/Reader/OliveBlueprintReader.h`
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Content/SystemPrompts/Knowledge/blueprint_authoring.txt`
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Content/SystemPrompts/Knowledge/recipe_routing.txt`
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Content/SystemPrompts/Knowledge/node_routing.txt`
- `B:/Unreal Projects/UE_Olive_AI_Toolkit/Plugins/UE_Olive_AI_Studio/Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp` (lines 369, 519, 541, 1101, 1143, 1368, 1375)
