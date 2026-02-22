# Brain Layer Improvements

> **Status:** Design Complete — Ready for Implementation (Updated to match current codebase)
> **Builds on:** Existing Brain Layer, Write Pipeline, Confirmation Manager, Focus Profiles, Tool Registry, Project Index
> **Estimated effort:** 5–6 weeks
> **Scope:** Improvements to the shipped Brain Layer. Not a rewrite.

---

## 1. What This Is

The Brain Layer shipped. It works. These are targeted improvements that make it significantly better at three things:

1. **Token efficiency** — Prompt distillation + tool packs cut token waste 50–80%
2. **Task completion** — Batch writes collapse 60+ tool calls into 1, preventing half-wired graphs
3. **Context quality** — Project Map indexing gives the model high-signal project awareness without scanning

Each improvement is independently shippable. They compound when combined.

### Current Baseline (Already in the Plugin)

Some of this plan is already partially implemented in the current codebase; treat the sections below as **delta** on top of what exists:

- **Tool packs exist and are used** for schema reduction during provider calls (`FOliveToolPackManager`, selected in `FOliveConversationManager::SendToProvider()`).
- **Tool result prompt distillation exists** (`FOlivePromptDistiller`) and already summarizes older/oversized tool *result* messages.
- **Operation history store exists** (`FOliveOperationHistoryStore`) and can generate summaries, but it is not yet the single “source of truth” for model-facing distilled tool history.

### What This Is NOT

- Not an orchestrator/worker rewrite (that's deferred to a future phase if needed — see Section 12)
- Not a new confirmation system (the existing write pipeline owns confirmation tokens)
- Not a new tool registry (existing `FOliveToolRegistry` and `FOliveFocusProfileManager` are reused)

---

## 2. Prompt Distillation

### 2.1 Problem

Raw tool JSON accumulating in the model's context is the #1 token killer in agentic loops. After 8 tool calls, the model's context is full of JSON it doesn't need, degrading both quality and cost.

### 2.2 The Rule

**Never echo full tool JSON to the model unless it's one of the last 2 tool call/result pairs.**

| Destination | What it gets |
|-------------|-------------|
| UI / Operation Feed / History Store | Full JSON, always |
| Model context (last 1–2 calls) | Full raw JSON |
| Model context (older calls, current run) | One-line summaries |
| Model context (previous runs) | Compressed session summary |

### 2.3 Distillation Levels

**Raw (last 1–2 calls):**
Full tool call JSON. The model needs recent results to continue its chain of thought.

**Summary (older calls within current run):**
```
[1] blueprint.create "BP_Enemy" (parent: ACharacter) → success, /Game/Characters/BP_Enemy
[2] blueprint.add_variable "Health" (float, default=100) → success
[3] blueprint.add_component "CapsuleComp" (UCapsuleComponent) → success
```

**Compressed (previous runs / session history):**
```
Earlier: Created BP_Enemy with Health variable, TakeDamage function, 3 components. All succeeded.
```

### 2.4 Implementation

**Current state:** `FOlivePromptDistiller` already distills tool *result* messages, keeping only the last N verbatim (and summarizing older/oversized results).

**Delta:** apply the “last 1–2 raw pairs” rule consistently across the whole prompt assembly, including:
- tool result messages (already handled)
- any tool call “echo” messages (if present in provider message history)
- any operation-history / “what changed” context injected outside the chat transcript

Add `BuildModelContext()` to `FOliveOperationHistoryStore` (or equivalent) so prompt assembly can include distilled tool history without dumping raw JSON for the entire session:

```cpp
// Returns distilled history for prompt assembly.
// Last RawResultCount results stay as raw JSON.
// Older results become one-line summaries.
// Previous runs become compressed summaries.
// Total output respects TokenBudget.
FString BuildModelContext(int32 TokenBudget, int32 RawResultCount = 2) const;
```

Pre-compute `OneLinerSummary` (or similar) at record time on each `FOliveOperationRecord` so distillation is fast during prompt assembly.

### 2.5 Integration

Update `FOlivePromptAssembler` to include distilled operation history context (and avoid dumping raw tool JSON for the entire session). The assembler's prompt structure becomes:

```
System prompt + Focus Profile prompt
+ Project context (from Project Map if indexed)
+ Distilled operation history (raw last 2, summaries for older)
+ Tool schemas (filtered by profile + pack)
+ Conversation messages
```

---

## 3. Tool Packs

### 3.1 Problem

Focus Profiles filter by domain (Blueprints vs C++). But even within "Blueprints," there are 40+ tools. Sending all of them when the model only needs read tools wastes schema tokens on every API call.

### 3.2 Solution

Tool Packs filter by **what the current step needs**. Combined with Focus Profiles, they cut schemas from 40+ down to 8–12 per call.

```
All registered tools
    → Focus Profile filter (domain)
    → Tool Pack filter (capability needed)
    → Final tool list sent to model
```

### 3.3 Pack Definitions

**Current state:** tool packs are already config-driven and loaded from `Config/OliveToolPacks.json`.

Delta work here is primarily maintenance (keep pack contents accurate as tools evolve) plus any new packs required by new tools (e.g. `project.batch_write`).

Example (JSON):

```json
{
  "read_pack": [
    "project.search",
    "project.get_asset_info",
    "blueprint.read"
  ],
  "write_pack_basic": [
    "blueprint.create",
    "blueprint.add_variable"
  ],
  "write_pack_graph": [
    "blueprint.add_node",
    "blueprint.connect_pins",
    "blueprint.set_pin_default"
  ],
  "danger_pack": [
    "blueprint.delete",
    "blueprint.set_parent_class"
  ]
}
```

### 3.4 Pack Selection

The Brain selects packs based on classified intent:

| Classified Intent | Packs Sent | Approx Schema Tokens |
|------------------|------------|---------------------|
| Read-only / explanation | `ReadPack` | ~2,000 |
| Simple creation | `ReadPack` + `WritePackBasic` | ~4,000 |
| Graph editing | `ReadPack` + `WritePackBasic` + `WritePackGraph` | ~6,000 |
| Destructive / refactor | All packs | ~8,000 |

Without packs, every call sends ~12,000+ schema tokens. With packs, typical calls are 2,000–6,000. **50–80% reduction.**

### 3.5 FOliveToolPackManager

**Current state:** `FOliveToolPackManager` already exists. The interface below is conceptual; actual signatures should follow the current `FOliveToolDefinition` + `EOliveToolPack` implementation.

```cpp
class FOliveToolPackManager
{
public:
    void Initialize();  // Load pack definitions from config

    // Get tools for a combination of profile + packs
    TArray<FOliveToolSchema> GetToolsForCall(
        FName FocusProfile,
        const TArray<FName>& ActivePacks
    ) const;

    // Check if a tool is in a specific pack (used by batch_write allowlist)
    bool IsToolInPack(const FString& ToolName, FName PackName) const;

private:
    TMap<FName, TArray<FString>> PackDefinitions;
};
```

### 3.6 Heuristic Planner Update

**Current state:** pack selection is already applied in `FOliveConversationManager::SendToProvider()` using turn intent + “in tool loop” heuristics.

**Delta:** move pack selection into a single run-policy/brain decision (for consistency + debuggability), and expose chosen packs in logs / run reports:
- Add selected packs to `FOliveRunPolicy` (or equivalent)
- Keep the ability for explicit user overrides (advanced UI)
- Ensure `project.batch_write` allowlist gating stays consistent with packs (Section 5.6)

---

## 4. Focus Profile UX Simplification

### 4.1 Change

Present **3 primary profiles** in the main dropdown: **Auto**, **Blueprints**, **C++**.

Keep all existing profiles (**AI & Behavior**, **Level & PCG**, **C++ & Blueprint**, **Everything**, custom) accessible under an **Advanced** section in the dropdown.

### 4.2 Rationale

Simpler UX for new users. Power users still have full control. No profiles are deleted. No internal changes to `FOliveFocusProfileManager` or `GetToolsForProfile()`.

### 4.3 How Packs Layer On

Tool packs apply on top of whichever profile is active (primary or advanced). A power user on "AI & Behavior" with a read-only intent gets: AI & Behavior tools ∩ ReadPack = very tight tool list.

---

## 5. Batch Writes (`project.batch_write`)

### 5.1 Problem

A "create a gun Blueprint that fires bullets" requires 60+ individual tool calls: create nodes, connect pins, set defaults. This hits `MaxToolIterations`, leaves half-wired graphs, and burns tokens on 60 separate round trips.

### 5.2 Solution

One tool call that executes many granular write ops atomically under a single outer transaction and compiles once at the end.

**Tool name:** `project.batch_write`
**Category:** `crosssystem`
**Tags:** `crosssystem`, `write`

### 5.3 Input Schema

```json
{
  "type": "object",
  "required": ["path", "ops"],
  "properties": {
    "path": {
      "type": "string",
      "description": "Target asset path. All ops must target this same asset."
    },
    "ops": {
      "type": "array",
      "minItems": 1,
      "description": "Ordered operations to execute.",
      "items": {
        "type": "object",
        "required": ["tool", "params"],
        "properties": {
          "id": { "type": "string", "description": "Optional op identifier for intra-batch references (unique within the batch)." },
          "tool": { "type": "string", "description": "Existing tool name (e.g., blueprint.add_node)." },
          "params": { "type": "object", "description": "Tool params. If missing 'path', batch injects top-level path." }
        }
      }
    },
    "dry_run": { "type": "boolean", "default": false, "description": "Validate and normalize ops, but do not mutate assets." },
    "auto_compile": { "type": "boolean", "default": true, "description": "Compile once at end." },
    "stop_on_error": { "type": "boolean", "default": true, "description": "Fail fast on first error." }
  }
}
```

Max ops enforced at validation time. Default 200, configurable via `UOliveAISettings::BatchWriteMaxOps` (clamp 1–1000).

### 5.3.1 Intra-Batch References (Required for Real Graph Builds)

Batch writes need a way for later ops to reference results produced by earlier ops (e.g. node IDs).

**V1 proposal (minimal):**
- Each op may specify an `id`.
- During execution, the batch tool stores each op result payload under that `id`.
- In later ops, any **string** value in `params` may contain template references like:
  - `${SomeOp.node_id}`
  - `${SomeOp.data.node_id}` (if your result nests)

The batch executor resolves templates before dispatching each op. Unresolved references are validation errors before any mutation (and should be surfaced clearly to the model).

### 5.3.2 Dry Run (Better Confirmation + Safer Debugging)

When `dry_run=true`, the tool:
- validates allowlist/single-asset/max-ops
- normalizes ops (injecting `path`, resolving templates, applying defaults)
- returns the normalized ops + validation report
- **does not** enter a transaction or mutate assets

This enables “Confirm → Execute” flows with a concrete preview and makes debugging failed batches cheaper.

### 5.4 Output Schema

```json
{
  "success": false,
  "data": {
    "asset_path": "/Game/Weapons/BP_Gun",
    "total_ops": 60,
    "completed_ops": 46,
    "stop_on_error": true,
    "failed_op": {
      "index_1based": 47,
      "tool": "blueprint.connect_pins",
      "error_code": "PIN_NOT_FOUND",
      "error_message": "Pin 'BulletClass' not found on node SpawnActor_3"
    },
    "results": [
      { "index_1based": 1, "tool": "blueprint.add_node", "success": true, "data": { "node_id": "..." } },
      { "index_1based": 2, "tool": "blueprint.add_node", "success": true, "data": { "node_id": "..." } }
    ],
    "created_node_ids": ["node_1", "node_2"],
    "compile_result": null
  }
}
```

Top-level `total_ops`, `completed_ops`, and `failed_op` let the model immediately see progress without parsing the results array.

### 5.5 Atomicity

Current write tools use `OLIVE_SCOPED_TRANSACTION(...)` internally. Nested transactions commit independently, making rollback impossible.

**Solution:** TLS batch execution scope flag.

```cpp
class FOliveBatchExecutionScope
{
public:
    static bool IsActive();
    FOliveBatchExecutionScope();   // sets TLS flag
    ~FOliveBatchExecutionScope();  // clears TLS flag
};
```

Writer code gates internal transactions:

```cpp
// In OliveBlueprintWriter.cpp, OliveGraphWriter.cpp, OlivePinConnector.cpp:
if (!FOliveBatchExecutionScope::IsActive())
{
    OLIVE_SCOPED_TRANSACTION(TEXT("Add Variable"));
}
// ... mutation logic runs either way
```

During batch, only the outer pipeline transaction exists. On failure, the outer transaction rolls back everything.

**Minimum files to gate:**
- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveBlueprintWriter.cpp` — asset creation, variables, components, functions, custom events, dispatchers
- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp` — AddNode, AddNodes, ConnectPins, SetPinDefault, SetNodeProperty, RemoveNode, DisconnectPins
- `Source/OliveAIEditor/Blueprint/Private/Writer/OlivePinConnector.cpp` — connect/disconnect ops

### 5.6 Allowlist

Batch only allows tools in `WritePackBasic` and `WritePackGraph` packs (via `FOliveToolPackManager::IsToolInPack()`). Anything in `DangerPack` is rejected at validation time before any mutation.

This leverages the tool pack system (Section 3) instead of maintaining a separate manual allowlist.

### 5.7 Execution Path

`HandleBatchWrite` builds a single `FOliveWriteRequest`:
- `ToolName = "project.batch_write"`
- `AssetPath = path`
- `TargetAsset = loaded UObject` (load once, error if not found)
- `OperationCategory = "graph_editing"` (Tier 2; MCP skips confirmation; editor chat requires confirm)
- `bAutoCompile = auto_compile`

Executor lambda:
1. Enter `FOliveBatchExecutionScope`
2. Iterate ops in order
3. Dispatch by tool name to the corresponding **writer call directly** (not `FOliveToolRegistry::ExecuteTool` — that would re-enter pipelines/transactions)
4. Collect per-op results
5. On first failure: set `failed_op`, return `FOliveWriteResult::ExecutionError(...)` so pipeline rolls back
6. On success: `completed_ops == total_ops`, attach compile result

### 5.8 Shared Parsing Helpers

To avoid duplicating param parsing between individual tool handlers and the batch executor:

```cpp
// Source/OliveAIEditor/Public/Services/OliveToolParamHelpers.h
namespace OliveToolParamHelpers
{
    FString GetRequiredString(const FJsonObject& Params, const FString& Key, FString& OutError);
    FOliveVariableIR ParseVariableParams(const FJsonObject& Params, FString& OutError);
    FOliveFunctionIR ParseFunctionParams(const FJsonObject& Params, FString& OutError);
    FOliveNodeIR ParseNodeParams(const FJsonObject& Params, FString& OutError);
    // ... standard JSON-to-IR conversions already used by handlers
}
```

Refactor existing tool handlers to use these helpers (no behavior change). Batch executor uses the same helpers. Prevents drift between individual and batch code paths.

### 5.9 V1 Scope

If supporting all write tools in batch is too large for v1, start with Blueprint graph ops only:
- `blueprint.add_node` / `blueprint.add_nodes`
- `blueprint.connect_pins` / `blueprint.disconnect_pins`
- `blueprint.set_pin_default` / `blueprint.set_node_property`
- `blueprint.remove_node`

Other tool names return a structured validation error in batch. This still solves the half-wired graph problem.

### 5.10 Additional Chunky Tool (Optional, High ROI)

Add `blueprint.add_nodes` (plural) wrapping `FOliveGraphWriter::AddNodes(...)` which already exists. Useful both inside and outside batch.

---

## 6. Project Map Indexing

### 6.1 Problem

When the user asks "make a gun that fires bullets," the model needs to know what's in the project — existing bullet classes, projectile components, weapon base classes. Without context it creates everything from scratch, ignoring existing assets.

Currently the model would need to `project.bulk_read` many assets to discover this. Expensive, slow, burns tokens.

### 6.2 Solution

A persisted Project Map — a lightweight catalog of all assets and classes with enough metadata to answer "what's relevant to this request?" without reading full asset contents.

### 6.3 Project Map Format

**Persisted to:** `Saved/OliveAI/ProjectMap.json` (Saved, not Content — no source control churn)

**Contents (v1):**
- Version + engine/project identifiers
- Last indexed UTC
- Assets: `name`, `path`, `asset_class`, `package_path`, minimal tags (parent class for BPs, blueprint type)
- Classes: class names + parent chain summary
- Counts and basic stats (asset class breakdown)

### 6.4 Build / Update Logic

Leverage existing `FOliveProjectIndex`:
- Already watches AssetRegistry events and maintains searchable state
- Add methods to export current state to Project Map JSON
- Add "dirty" tracking for incremental updates
- Full rebuild re-scans from AssetRegistry; incremental persists in-memory deltas

### 6.5 New Tools

**`project.index_build`** — Build or rebuild the persisted Project Map.
- Input: `mode` (`"full"` default, or `"incremental"`)
- Output: `status`, `asset_count`, `class_count`, `last_indexed_utc`, `duration_ms`, `file_path`

**`project.index_status`** — Return current index readiness + last persisted map info.

**`project.get_relevant_context`** — Return curated context from the Project Map given a query.
- Input: `query` (string, required), `max_assets` (int, default 10, configurable via `UOliveAISettings::RelevantContextMaxAssets`, clamp 1–50), `kinds` (optional filter list, e.g. `["Blueprint","BehaviorTree"]`), `include_suggestions` (bool, default true)
- Output: `query`, `results` array (`name`, `path`, `asset_class`, `primary_tags`, `score`), `suggested_bulk_read_paths` (optional)

### 6.6 UX

- `/index` chat command triggers `project.index_build` (full)
- Editor UI button "Index Project" / "Update Index" calls the same underlying code
- After indexing, `project.get_relevant_context("weapon")` returns plausible weapon-related assets quickly without bulk reads

---

## 7. Verification-First Loop Updates

### 7.1 Current State

The Brain already has the agentic loop: execute → check → retry. These improvements make verification more structured.

### 7.2 Changes

**Structured compile error feedback:** When auto-compile produces errors, feed them to the model as structured data (error code, asset path, function name, line), not raw text dumps. This lets the model self-correct precisely.

**Post-batch verification:** After a `project.batch_write`, the loop should always compile (handled by `auto_compile=true`) and then read back at least the function/event graph summary to confirm the graph is wired correctly. Prompt updates enforce this.

**"No fake success" rule:** System prompts explicitly instruct the model: never claim nodes or wiring exist unless tool results confirm it. If a batch fails, report what failed, don't fabricate success.

---

## 8. Retry / Loop Detection (Error Signatures)

### 8.1 Error Signatures

A normalized string identifying a specific failure:

- **Tool failure:** `{tool_name}:{error_code}:{asset_path}`
  - Example: `blueprint.connect_pins:PIN_NOT_FOUND:/Game/Characters/BP_Enemy`
- **Compile error:** `{compiler_error_code}:{asset_path}:{function_name}`
  - Example: `Error:BP_Enemy:TakeDamage:Missing return node`

### 8.2 Loop Detection Rules

- Same error signature + same attempted fix twice → **stop**
- Same set of errors repeats after 2 correction cycles → **oscillation detected**, stop
- Total correction attempts exceed `MaxCorrectionCyclesPerRun` (default 5) → stop

### 8.3 Stop Behavior

When the Brain stops due to loop detection, it reports:
- What it was trying to accomplish
- Which error kept recurring (actual error text)
- What fixes it attempted
- Suggestion: "This might require a different approach. You could try [specific manual suggestion] or rephrase the request."

---

## 9. Per-Request Provider Options

### 9.1 Problem

`IOliveAIProvider::SendMessage` is configured globally. No way to enforce a cheap call vs an expensive one.

### 9.2 Solution

```cpp
USTRUCT()
struct FOliveRequestOptions
{
    int32 MaxTokens = 4096;
    float Temperature = 0.7f;
    float TimeoutSeconds = 60.0f;
};
```

Add to provider interface (new overload or optional parameter):

```cpp
virtual void SendMessage(
    const FConversationContext& Context,
    const FOliveRequestOptions& Options,
    const FOnStreamChunk& OnChunk,
    const FOnComplete& OnComplete
) = 0;
```

The Brain uses this to enforce lower `MaxTokens` for simple reads, higher for complex edits, shorter timeouts for verification calls.

---

## 10. Confirmation System Clarification

### 10.1 The Rule

**The write pipeline owns confirmation tokens. The Brain does not generate, validate, or expire tokens.**

`FOliveWritePipeline` and `OliveSnapshotManager` already handle `confirmation_token` lifecycle. The Brain's role:

1. Detect tool call returned `requires_confirmation`
2. Store pending UI state (tool, params, token)
3. Show confirmation UI
4. On approve: replay exact same tool call with the token the pipeline issued
5. On cancel: discard pending state

No changes to the pipeline. The Brain just stores and replays.

---

## 11. Prompt / System Prompt Updates

### 11.1 Deliverables

| File | Change |
|------|--------|
| `Content/SystemPrompts/BaseSystemPrompt.txt` | Add: prefer `project.get_relevant_context` for vague requests; prefer `project.batch_write` for graph edits |
| `Content/SystemPrompts/Knowledge/blueprint_authoring.txt` | Batch-first graph editing; compile + readback before claiming success; "no fake success" |
| `Content/SystemPrompts/Worker_Blueprint.txt` | BP-specific batch patterns |
| Domain-specific worker prompts | BT/PCG/C++ patterns as needed |

### 11.2 Key Rules

- Use `project.get_relevant_context` early to discover existing project assets before creating new ones
- Use `project.batch_write` for any multi-step graph edits (nodes + pins + defaults)
- Always compile after writes
- Always read back at least the graph summary after a batch to verify
- Never claim success without tool result proof
- "Blueprint work" in prompts means any UEdGraph-authored asset (BP, BT, PCG), but tools stay namespaced

---

## 12. Future: Orchestrator + Workers (Not This Phase)

### When to Build It

If real usage data shows:
- Cross-domain tasks (BP + BT) failing because accumulated context degrades quality
- Token cost per cross-domain task exceeding acceptable thresholds
- Users reporting multi-domain requests produce significantly worse results than single-domain

### What It Would Add

- Orchestrator agent (no tools, plans steps by domain)
- Worker agents (domain-scoped tools, clean context per step)
- Inter-worker context passing via Operation History summaries
- Worker configs map to existing Focus Profiles (no parallel system)

This is deferred because: competitors ship without it, prompt distillation + tool packs + batch writes solve 80% of the token problem, and verification-first quality matters more than architectural sophistication.

---

## 13. Configurable Settings

All new settings in `UOliveAISettings`, with defaults in `Config/DefaultOliveAI.ini`:

| Setting | Default | Range | Purpose |
|---------|---------|-------|---------|
| `BatchWriteMaxOps` | 200 | 1–1000 | Max ops per `project.batch_write` call |
| `RelevantContextMaxAssets` | 10 | 1–50 | Max results from `project.get_relevant_context` |
| `PromptDistillationRawResults` | 2 | 1–5 | How many recent tool results stay as raw JSON in model context |
| `MaxCorrectionCyclesPerRun` | 5 | 1–20 | Loop detection: max self-correction attempts |

---

## 14. Files

### 14.1 New Files

```
Source/OliveAIEditor/
├── Public/Brain/
│   ├── OliveToolPackManager.h         — pack definitions + filtering
│   └── OliveRequestOptions.h          — per-request provider options
│
├── Private/Brain/
│   ├── OliveToolPackManager.cpp
│   └── OliveRequestOptions.cpp
│
├── Public/Services/
│   └── OliveToolParamHelpers.h        — shared param parsing
│
├── Private/Services/
│   └── OliveToolParamHelpers.cpp
│
├── CrossSystem/Private/MCP/
│   └── OliveBatchExecutionScope.h/cpp — TLS batch flag
│
├── CrossSystem/Public/MCP/
│   └── (schema additions to OliveCrossSystemSchemas.h/cpp)
│
Content/SystemPrompts/
├── (updates to existing files)
│
Config/
├── DefaultOliveAI.ini                 — batch/index limits (and other defaults)
└── OliveToolPacks.json                — tool pack definitions
```

### 14.2 Modified Files

| File | Change |
|------|--------|
| `Public/Providers/IOliveAIProvider.h` | Add `FOliveRequestOptions` to `SendMessage` |
| All provider implementations | Support per-request options |
| `Private/Brain/OlivePromptDistiller.cpp` | Extend distillation (tool calls + run-compressed summaries) |
| `Private/Brain/OliveOperationHistory.cpp` | (Optional) Add model-facing context builder for distilled op history |
| `Private/Brain/OliveHeuristicPlanner.cpp` | Move pack selection into run policy (debuggable) |
| `Private/Brain/OlivePromptAssembler.cpp` | Use distilled op context + pack-filtered schemas |
| `Blueprint/Private/Writer/OliveBlueprintWriter.cpp` | Gate `OLIVE_SCOPED_TRANSACTION` on batch scope |
| `Blueprint/Private/Writer/OliveGraphWriter.cpp` | Same transaction gating |
| `Blueprint/Private/Writer/OlivePinConnector.cpp` | Same transaction gating |
| `Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Refactor param parsing to shared helpers |
| `CrossSystem/Private/MCP/OliveCrossSystemSchemas.cpp` | Add `project.batch_write` + index tool schemas |
| `CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp` | Add `HandleBatchWrite` + index tool handlers |
| `Private/Index/OliveProjectIndex.cpp` | Add Project Map export, dirty tracking, incremental |
| Focus Profile UI widget | 3 primary + advanced dropdown |
| `Config/OliveToolPacks.json` | Pack definitions (maintenance / additions) |
| `Config/DefaultOliveAI.ini` | Batch/index limits |

---

## 15. Implementation Order

### Step 1: Configurable Settings (1 day)
- Add `BatchWriteMaxOps`, `RelevantContextMaxAssets`, `PromptDistillationRawResults`, `MaxCorrectionCyclesPerRun` to `UOliveAISettings`
- Defaults in `Config/DefaultOliveAI.ini`
- **Test:** Changing config changes runtime behavior.

### Step 2: Per-Request Provider Options (2 days)
- Add `FOliveRequestOptions` struct
- Update `IOliveAIProvider::SendMessage`
- Implement across providers
- **Test:** Different max_tokens per call works.

### Step 3: Tool Pack Manager (3 days)
**Already present in plugin.** Delta work:
- Audit `Config/OliveToolPacks.json` for completeness as tools evolve
- Add any missing packs (if needed) and ensure naming consistency
- Move pack selection into a run policy / brain decision (Section 3.6)
- **Test:** Profile + pack filtering produces correct tool lists; chosen packs are visible in logs/run report.

### Step 4: Prompt Distillation (3 days)
**Already partially present in plugin** (`FOlivePromptDistiller` distills tool results). Delta work:
- Ensure the “last 1–2 raw pairs” rule applies consistently (tool calls + tool results + injected op-history context)
- Add run-compressed summaries for older runs/session history
- Update `FOlivePromptAssembler` to prefer distilled op-history context over raw JSON dumps
- **Test:** After 8 tool calls, model context contains raw JSON for only the last configured window; token count drops measurably vs baseline.

### Step 5: Shared Parsing Helpers (3 days)
- Create `OliveToolParamHelpers` with extracted parsing logic
- Refactor existing Blueprint tool handlers to use helpers
- **Test:** All existing tool handler behavior unchanged. Helpers are reusable.

### Step 6: Batch Execution Scope (2 days)
- Implement `FOliveBatchExecutionScope` TLS flag
- Gate `OLIVE_SCOPED_TRANSACTION` in OliveBlueprintWriter, OliveGraphWriter, OlivePinConnector
- **Test:** Writers still create transactions normally. With scope active, they skip.

### Step 7: `project.batch_write` Tool (5 days) -- COMPLETED
- [x] Add schema + registration in CrossSystem
- [x] Implement `HandleBatchWrite`: validation, allowlist (via pack manager), execution, output
- [x] Implement intra-batch reference resolution (Section 5.3.1)
- [x] Implement `dry_run` mode (Section 5.3.2)
- [x] V1: Blueprint graph ops only (`add_node`, `connect_pins`, `set_pin_default`, `set_node_property`, `disconnect_pins`, `remove_node`)
- **Test:** 60-op batch compiles successfully. Failed op 47 → rollback, `completed_ops=46`. Allowlist rejects deletes. Single-asset enforced.

### Step 8: `blueprint.add_nodes` (2 days)
- Schema + handler wrapping `FOliveGraphWriter::AddNodes(...)`
- **Test:** Multiple nodes created in one call, both standalone and inside batch.

### Step 9: Project Map Indexing (5 days)
- Define Project Map JSON format
- Add export/dirty-tracking/incremental to `FOliveProjectIndex`
- Implement `project.index_build`, `project.index_status`, `project.get_relevant_context`
- Add `/index` chat command + Editor UI button
- **Test:** After indexing, `get_relevant_context("weapon")` returns plausible results. Incremental update reflects renamed assets.

### Step 10: Error Signature Loop Detection (2 days)
- Implement error signature generation (tool name + error code + asset path)
- Add oscillation detection
- Implement stop behavior with actionable user guidance
- **Test:** Same error twice → stops. Oscillating errors → stops. Guidance message is specific.

### Step 11: Focus Profile UX (1 day)
- Update dropdown: 3 primary + advanced section
- No internal changes to `FOliveFocusProfileManager`
- **Test:** Auto/Blueprints/C++ visible by default. Advanced profiles accessible.

### Step 12: Prompt Updates (3 days)
- Update `BaseSystemPrompt.txt`, `blueprint_authoring.txt`, `Worker_Blueprint.txt`
- Batch-first graph editing instructions
- `get_relevant_context` usage for vague requests
- "No fake success" rule
- **Test:** Model behavior shifts to batch + verify pattern.

### Step 13: Integration Testing (3 days)
- "Make a gun that fires bullets" end-to-end via batch write
- Batch rollback on failure
- Project map → relevant context → informed creation
- Prompt distillation token savings measurement
- Tool pack filtering verification
- Loop detection with compile errors
- Per-request budget enforcement

**Total estimated effort: 5–6 weeks**

---

## 16. Acceptance Tests

### Batch Write
1. **Happy path:** 60-op graph build → `completed_ops=60`, compile success, readback confirms wiring
2. **Rollback:** Op 3 of 3 fails (bad pin) → no nodes from ops 1–2 exist
3. **Allowlist:** Include `project.refactor_rename` in batch → validation error before any mutation
4. **Single-asset:** Op with different `params.path` → validation error
5. **Config limit:** `BatchWriteMaxOps=20`, submit 21 → validation error
6. **Iteration cap:** Editor chat "build Fire() graph" via single batch → completes without hitting `MaxToolIterations`
7. **References:** Op 2 can reference node ID returned by op 1 (e.g. `${Spawn.node_id}` resolves and connects correctly)
8. **Dry run:** `dry_run=true` returns normalized ops + validation report and makes no asset changes

### Project Map
1. **Fresh project:** `index_status` says not indexed
2. **After indexing:** `get_relevant_context("weapon")` returns plausible results quickly
3. **Incremental:** Rename asset → "Update Index" → retrieval reflects new path

### Prompt Distillation
1. After 8 tool calls, model context contains raw JSON for only last 2
2. Token count measurably lower than pre-distillation baseline

### Tool Packs
1. Read-only intent → only ReadPack tools in API call
2. Graph editing intent → ReadPack + WritePackBasic + WritePackGraph
3. Pack membership query correctly gates batch allowlist

### Loop Detection
1. Same compile error 3 times with same fix → Brain stops, reports error + suggestions
2. Oscillating errors (fix A causes B, fix B causes A) → detected and stopped
