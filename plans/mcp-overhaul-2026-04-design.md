# MCP Architecture Overhaul — 2026-04

**Status:** Ready for implementation
**Owner:** Architect (design) → Coder (execution)
**Target completion:** 4 phases, each phase ships a working plugin
**Scope root:** `Plugins/UE_Olive_AI_Studio/` only (no parent project changes)

---

## 1. Executive Summary

Olive's MCP server is the outlier in the UE-editor MCP ecosystem on three dimensions: HTTP instead of stdio, ~100 exposed tools instead of 8–25, and a preview→apply→fingerprint cycle on plan JSON that no peer implements. Our own session logs confirm the cost: slow round-trips through the `mcp-bridge.js` HTTP shim, token bloat in `tools/list` responses that consume 55K–100K+ context on agents like Claude Code, and 50–58% plan_json success rates driven partly by the two-call preview/apply dance. This design replaces HTTP with native stdio JSON-RPC, collapses `preview_plan_json`+`apply_plan_json` into a single `blueprint.build` (with `blueprint.patch` for granular ops and `blueprint.rollback` for one-shot recovery), consolidates ~100 tools down to ~25 via `detail_level`/`scope` params on readers, and adds a BM25 RAG index over the UE Python API that attaches top-3 doc chunks to every `editor.run_python` failure. The resolver/validator/executor/library-index moat stays untouched. Work ships in four phases: stdio transport (Phase 1), `build`/`patch`/`rollback` collapse (Phase 2), tool consolidation (Phase 3), Python RAG (Phase 4). Each phase ends with a compilable plugin and passing tests.

---

## 2. Architecture Changes

### 2.1 Transport: HTTP → stdio

**Current state.** `FOliveMCPServer` (`Source/OliveAIEditor/Public/MCP/OliveMCPServer.h`) binds an HTTP listener on ports 3000–3009 via `IHttpRouter`. External agents (Claude Code CLI) talk to `mcp-bridge.js` over stdio; the bridge translates stdio JSON-RPC to HTTP POST `/mcp` and auto-discovers the port by pinging each in range. Startup writes `.mcp.json` pointing at the bridge.

**Target state.** A new `FOliveMCPStdioTransport` spawns a long-lived Node process (`mcp-stdio.js`, a 20-line passthrough — see 2.1.b) and speaks NDJSON JSON-RPC 2.0 on its stdin/stdout. The plugin reads lines via `FRunnable`-owned platform pipes, dispatches each request to the existing `ProcessJsonRpcRequest` logic (reused from the current HTTP path, refactored into `FOliveJsonRpcHandler`), and writes responses back. Because MCP stdio servers are spawned *by the client* (Claude Code CLI, Cursor, etc.), the transport's "run" path is inverted: the plugin-side component is actually a passive request handler that just has to be registered. The flow becomes:
```
Claude Code CLI → spawns → mcp-stdio.js → IPC (named pipe or HTTP loopback) → UE Editor (FOliveMCPStdioTransport)
```
Stdio MCP is fundamentally a process-per-client model; since UE is a long-lived editor process, the Node shim is unavoidable, but it collapses to a trivial passthrough.

**Files affected.**
- New: `Source/OliveAIEditor/Public/MCP/OliveMCPStdioTransport.h`, `Private/MCP/OliveMCPStdioTransport.cpp`
- New: `Source/OliveAIEditor/Public/MCP/OliveJsonRpcHandler.h`, `Private/MCP/OliveJsonRpcHandler.cpp` (extracted from `FOliveMCPServer`)
- Modified: `Source/OliveAIEditor/Public/MCP/OliveMCPServer.h`, `Private/MCP/OliveMCPServer.cpp` (HTTP becomes deprecated/optional; method handlers delegate to `FOliveJsonRpcHandler`)
- Modified: `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` (new `EOliveMCPTransportMode` enum)
- Replaced: `mcp-bridge.js` → `mcp-stdio.js` (~60 lines, ~80% smaller)
- Modified: `.mcp.json` template written by `WriteMcpConfigFile()`

**Rationale.** Eliminates the HTTP-discovery latency (up to 10 × 500ms = 5s cold start), removes the port-hunting loop, removes the `HttpServerModule` dependency, matches the transport every other UE MCP server uses. The Node shim cannot be fully eliminated because Claude Code CLI's stdio MCP protocol requires the client to spawn the server process — and UE Editor is not spawnable-on-demand. But shrinking the shim to a passthrough removes all the logic that has been a source of bugs.

**Decision: eliminate mcp-bridge.js, ship mcp-stdio.js as trivial passthrough.** The old bridge had port discovery, multi-host fallback, and timeout handling — none of which are needed when the transport is a named-pipe IPC to a known-running UE editor. If UE isn't running, the new shim errors out immediately with a clear message; no port scanning.

### 2.2 Plan JSON: preview/apply/fingerprint → `blueprint.build`

**Current state.** Two tools:
- `blueprint.preview_plan_json` — runs resolver + validator, returns `preview_fingerprint` + resolved plan summary, no mutation.
- `blueprint.apply_plan_json` — takes optional `preview_fingerprint`, re-runs resolver (tolerant drift check against fingerprint), runs validator, runs executor, returns result. Has an "auto-preview" path when fingerprint is absent and `bPlanJsonRequirePreviewForApply=true`.
The 2-call model is both slow (round-trip × 2) and error-prone (agents occasionally batch them despite the prompt saying not to, or forget the fingerprint entirely).

**Target state.** Single tool `blueprint.build` with the same plan JSON input. Runs resolve → validate → execute in one pipeline call, returns the full result (including compile errors). No fingerprint. Also:
- `blueprint.patch` — batches the granular ops (`add_node`, `connect_pins`, `set_pin_default`, `remove_node`, `disconnect_pins`, `set_node_property`) inside a single transaction. Takes `{asset_path, graph, ops: [{op: "add_node", ...}, ...]}`. Internally dispatches to existing handlers within one `FScopedTransaction`.
- `blueprint.rollback` — one-shot, no confirmation. Takes `{snapshot_id?: string}` — defaults to the most recent snapshot for the active asset(s). Wraps `FOliveSnapshotManager::Rollback` directly, bypassing the current two-step `preview_only→confirmation_token` flow that `project.rollback` uses.

Deprecated aliases registered via `FOliveToolRegistry::ResolveAlias`:
- `blueprint.apply_plan_json` → `blueprint.build` (passthrough, no param transform; `preview_fingerprint` is ignored)
- `blueprint.preview_plan_json` → `blueprint.build` with param transform `{..., dry_run: true}`
- `project.rollback` stays as-is for explicit multi-asset rollback; `blueprint.rollback` is the fast path.

**Files affected.**
- Modified: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` — new `HandleBlueprintBuild`, new `HandleBlueprintPatch`, new `HandleBlueprintRollback`. Old `HandleBlueprintPreviewPlanJson`, `HandleBlueprintApplyPlanJson` are deleted; their aliases are registered by `RegisterPlanTools()`.
- Modified: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` — new `BlueprintBuild`, `BlueprintPatch`, `BlueprintRollback` schema factories.
- Modified: `Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h/.cpp` — add `RegisterAlias(From, To, TransformFn)` public method.
- Modified: `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` — delete `bPlanJsonRequirePreviewForApply` (no longer used).
- Modified: Recipe and knowledge files in `Content/SystemPrompts/` — replace `preview_plan_json`/`apply_plan_json` references with `blueprint.build`.

**Rationale.** The fingerprint drift check was solving a problem that doesn't really exist: in real usage, the graph doesn't mutate between the preview call and the apply call (the agent is the only mutator). When it does — concurrent user edits — the resolver catches the issue because it reads live graph state during resolve. The fingerprint added a second round-trip, an extra token spend on returning the fingerprint, and a failure mode (mismatch warning) that didn't actually stop anything (the pipeline proceeds anyway). Drop it. `dry_run: true` on `build` preserves the preview capability for callers who want it.

### 2.3 Tool count: ~100 → ~25 via consolidation

**Current state.** ~100 registered tools across `blueprint.*`, `bt.*`, `pcg.*`, `niagara.*`, `cpp.*`, `widget.*`, `project.*`, `editor.*`, `olive.*`. Each reader variant (e.g. `blueprint.read`, `blueprint.read_components`, `blueprint.read_variables`, `blueprint.read_event_graph`, `blueprint.read_function`, `blueprint.read_hierarchy`, `blueprint.describe_node_type`, `blueprint.describe_function`, `blueprint.verify_completion`, `blueprint.get_node_pins`, `blueprint.list_overridable_functions`) is a separate tool entry in `tools/list`, each with its own schema. Anthropic's tool-use guidance warns that tool schemas + names + descriptions are prepended to every request and cost 55K–100K context tokens at this scale.

**Target state.** Readers collapse around a `scope` + `detail_level` pattern. Example: `blueprint.read` takes `{asset_path, scope: "summary"|"components"|"variables"|"functions"|"event_graph"|"hierarchy"|"node"|"function_signature"|"node_pins", detail_level: "minimal"|"normal"|"full", target?: string}`. The handler is a single dispatch table keyed on `scope`. Same pattern for `bt.read`, `pcg.read`, `niagara.read`, `cpp.read`.

Target inventory (~25 tools):

| Family | Tools |
|--------|-------|
| `blueprint` | `read`, `build`, `patch`, `rollback`, `create`, `compile`, `delete`, `list_templates`, `get_template`, `create_from_template` |
| `bt` | `read`, `build`, `create` |
| `pcg` | `read`, `build`, `create` |
| `niagara` | `read`, `build`, `create` |
| `cpp` | `read`, `write`, `compile` |
| `widget` | folded into `blueprint.patch` (widget ops become `op: "add_widget"`, etc.) |
| `project` | `search`, `snapshot`, `rollback`, `get_relevant_context` |
| `editor` | `run_python` |
| `olive` | `get_recipe` |

Consolidation mechanics:
- All removed names stay callable as aliases during the deprecation cycle (one release).
- Aliases do not appear in `tools/list`, keeping the context cost low.
- Every consolidated tool's schema documents `scope` values via `enum` + per-scope `usage_guidance`.
- `blueprint.patch` absorbs: `add_node`, `remove_node`, `connect_pins`, `disconnect_pins`, `set_pin_default`, `set_node_property`, `create_timeline`, widget.*. Each becomes an `op` entry.
- `blueprint.build` absorbs: `preview_plan_json`, `apply_plan_json`.
- `blueprint.create` stays standalone (it's the asset-creation entry point), but absorbs `scaffold`, `set_parent_class`, `add_interface`, `remove_interface`, `create_interface`, `add_variable`, `remove_variable`, `modify_variable`, `add_component`, `remove_component`, `modify_component`, `reparent_component`, `add_function`, `remove_function`, `modify_function_signature`, `add_custom_event`, `add_event_dispatcher`, `override_function` as ops under a unified `structure` param — OR, simpler: these become `blueprint.patch` ops too. We choose: **structure ops (variables/components/functions/interfaces/dispatchers) go in `blueprint.patch`**; `blueprint.create` is only for *creating new assets*.

**Files affected.**
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` — large refactor. New `HandleBlueprintRead` dispatcher; old handlers become private helpers keyed off `scope`.
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` — new unified `BlueprintRead`, `BlueprintPatch` schema factories.
- `Source/OliveAIEditor/BehaviorTree/Private/MCP/OliveBTToolHandlers.cpp` — same pattern for `bt.read`, `bt.build`.
- `Source/OliveAIEditor/PCG/Private/MCP/OlivePCGToolHandlers.cpp` — same for `pcg.read`, `pcg.build`.
- `Source/OliveAIEditor/Niagara/Private/MCP/OliveNiagaraToolHandlers.cpp` — same for `niagara.read`, `niagara.build`.
- `Source/OliveAIEditor/Cpp/Private/MCP/OliveCppToolHandlers.cpp` — `cpp.read`, `cpp.write`.
- `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp` — prune `batch_write`, `bulk_read`, `create_ai_character`, `diff`, `implement_interface`, `move_to_cpp`, `refactor_rename` (these become `olive.build` recipes instead of dedicated tools; the `olive.build` batch executor already exists — see `Source/OliveAIEditor/Private/MCP/OliveBuildTool.h`).
- `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp` — prune `project.batch_write`, `project.bulk_read`, `project.index_build`, `project.index_status` (rolled into `project.search` with a `rebuild_index` flag).

**Rationale.** Fewer tools with richer dispatch = less context, easier model choice, better consistency. The agent makes fewer wrong-tool decisions when the surface is 5 blueprint tools instead of 30. Aliases preserve backward compatibility for external MCP clients and old scripts.

### 2.4 RAG on `editor.run_python` errors

**Current state.** `editor.run_python` failures return stderr + traceback. The agent has no UE Python API documentation in-context beyond what's in the prompt. It hallucinates API names (`unreal.BlueprintFactory().set_parent(...)` when the real API is `ParentClass` attribute assignment, etc.) and loops through failures.

**Target state.** New `FOlivePythonDocIndex` singleton with a BM25 (or TF-IDF — picking BM25 for better recall on short queries) inverted index over UE Python API docs. Index sources:
1. **Generated at first-run.** On `Initialize()`, if the index file doesn't exist or UE build version has changed, run a bundled generator script via `IPythonScriptPlugin::ExecPythonCommand`. The script iterates `unreal.__dict__`, introspects every class/function with `help()` + `inspect`, emits one JSON doc per symbol: `{symbol: "unreal.EditorAssetLibrary.save_asset", signature: "save_asset(asset_to_save: str, only_if_is_dirty: bool = True) -> bool", doc: "...", kind: "method"}`.
2. Output written to `Saved/OliveAI/python_docs.json` (not in plugin Content/ — it's machine-specific).
3. Index loaded lazily, built on demand.

On `editor.run_python` failure, the handler extracts the error line (Python traceback's last `^` or the exception type + message), queries the index for top-3 chunks, appends them to `FOliveToolResult.Data["doc_hints"]` and the error `Suggestion` text.

**Files affected.**
- New: `Source/OliveAIEditor/Python/Public/Docs/OlivePythonDocIndex.h`, `Private/Docs/OlivePythonDocIndex.cpp`
- New: `Source/OliveAIEditor/Python/Private/Docs/GeneratePythonDocs.py` (runs in UE's Python, generates the JSON)
- Modified: `Source/OliveAIEditor/Python/Private/MCP/OlivePythonToolHandlers.cpp` — error path calls `FOlivePythonDocIndex::Search(ExtractQueryFromError(output))`.
- Modified: `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp` — `OnPostEngineInit` calls `FOlivePythonDocIndex::Get().Initialize()` (lazy — just marks "not loaded yet"; actual index build deferred to first search).

**Rationale.** Small investment (BM25 is 200 LOC in C++ without deps; the doc generator is ~50 LOC Python), large payoff in reducing correction cycles when the agent writes Python. Keeps the index machine-local (reflects the user's actual engine build) instead of shipping a stale dump in-plugin.

---

## 3. Phased Task Breakdown

Each phase ships a working plugin. Coder should ensure UBT build + existing tests pass before moving to the next phase.

### Phase 1 — Stdio Transport

**Goal.** External MCP clients connect via stdio (no HTTP bridge). In-editor chat unaffected. HTTP transport remains compiled but off-by-default for one release cycle.

---

#### P1.T1 — Extract JSON-RPC handler from FOliveMCPServer

**Files:** Create `Source/OliveAIEditor/Public/MCP/OliveJsonRpcHandler.h`, `Source/OliveAIEditor/Private/MCP/OliveJsonRpcHandler.cpp`. Modify `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp`.

**Implementation notes.** Move `ProcessJsonRpcRequest`, all `Handle*` methods (`HandleInitialize`, `HandleToolsList`, `HandleToolsCall`, `HandleResourcesList`, `HandleResourcesRead`, `HandlePromptsList`, `HandlePromptsGet`, `HandlePing`, `HandleToolsCallAsync`) from `FOliveMCPServer` into a new `FOliveJsonRpcHandler` singleton. Keep method signatures identical. `FOliveMCPServer::HandleRequest` now calls `FOliveJsonRpcHandler::Get().Process(...)`. Keep client-state tracking (`FMCPClientState`, `ClientStates`, `ClientsLock`) on `FOliveJsonRpcHandler`, not on the transport. Tool filter and internal-agent-mode state also moves to the handler.

**Acceptance criteria.** UBT build passes. In-editor chat and external HTTP MCP calls still work (no behavior change). Code in `OliveMCPServer.cpp` shrinks to pure HTTP plumbing.

**Dependencies.** None.

---

#### P1.T2 — Define stdio transport interface + settings toggle

**Files:** Create `Source/OliveAIEditor/Public/MCP/OliveMCPStdioTransport.h`, `Source/OliveAIEditor/Private/MCP/OliveMCPStdioTransport.cpp`. Modify `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`.

**Implementation notes.**
```cpp
UENUM()
enum class EOliveMCPTransportMode : uint8
{
    Stdio       UMETA(DisplayName = "Stdio (recommended)"),
    Http        UMETA(DisplayName = "HTTP (deprecated)"),
    Both        UMETA(DisplayName = "Both (migration)"),
};
```
Add `UPROPERTY(Config, EditAnywhere) EOliveMCPTransportMode MCPTransportMode = EOliveMCPTransportMode::Stdio;` to `UOliveAISettings`. `FOliveMCPStdioTransport` exposes `Start()`, `Stop()`, `IsRunning()`, `GetSocketPath()`. Pick IPC: **Windows named pipe** (`\\.\pipe\OliveAI_{ProcessId}`) on Windows, unix domain socket on Linux/Mac. Create a `FRunnable`-backed reader thread that: `accept()`s connections, spawns a per-client `FRunnable` that reads NDJSON lines, deserializes to `FJsonObject`, dispatches to `FOliveJsonRpcHandler::Get().Process(Request, ClientId)`, writes response as NDJSON. Use `FPlatformNamedPipe` (UE 5.5 has this).

**Acceptance criteria.** Transport compiles and can `Start`/`Stop`. A unit test can connect to the pipe, send a `ping` request, get a response. No actual Node-side integration yet.

**Dependencies.** P1.T1.

---

#### P1.T3 — Wire transport into module startup

**Files:** Modify `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`.

**Implementation notes.** In `OnPostEngineInit`, after tool registration, inspect `Settings->MCPTransportMode`:
- `Stdio`: `FOliveMCPStdioTransport::Get().Start()`, skip HTTP server.
- `Http`: `FOliveMCPServer::Get().Start(Settings->MCPServerPort)`, skip stdio.
- `Both`: start both.
On `ShutdownModule`, stop whichever is running. `WriteMcpConfigFile` (currently on `FOliveMCPServer`) gets a sibling on `FOliveMCPStdioTransport` that writes a stdio-flavored `.mcp.json` (see P1.T6).

**Acceptance criteria.** Editor boots with `Stdio` mode by default. Log shows `Stdio transport listening on \\.\pipe\OliveAI_1234`. Editor boots with `Http` mode → HTTP server starts as before. `Both` mode starts both without conflict.

**Dependencies.** P1.T1, P1.T2.

---

#### P1.T4 — Replace mcp-bridge.js with mcp-stdio.js passthrough

**Files:** Delete `mcp-bridge.js`. Create `mcp-stdio.js` (~60 lines). Optional: keep `mcp-bridge.js` as a one-line shim that re-exports `mcp-stdio.js` for people with hard-coded paths.

**Implementation notes.** `mcp-stdio.js` does exactly:
1. Read `OLIVE_MCP_PIPE_NAME` env var (fallback: scan `Saved/OliveAI/stdio-pipe.txt` that the plugin writes on startup).
2. Connect to the named pipe / unix socket.
3. Pipe `process.stdin` → pipe, pipe ← `process.stdout`. No parsing, no discovery, no retry.
4. On connect error: emit a single JSON-RPC error response to stdout and exit(1) with a clear `stderr` message ("Olive AI Studio not running — start the UE editor with the plugin enabled").

No port scanning, no multi-host fallback, no timeouts beyond OS defaults.

**Acceptance criteria.** `node mcp-stdio.js` + `echo '{"jsonrpc":"2.0","id":1,"method":"ping"}'` pipe through and return a pong. File is under 80 lines.

**Dependencies.** P1.T3.

---

#### P1.T5 — Write .mcp.json for stdio mode

**Files:** Modify `Source/OliveAIEditor/Private/MCP/OliveMCPStdioTransport.cpp`.

**Implementation notes.** After `Start()` succeeds, write `Plugins/UE_Olive_AI_Studio/.mcp.json` with:
```json
{
  "mcpServers": {
    "olive-ai-studio": {
      "command": "node",
      "args": ["mcp-stdio.js"],
      "env": { "OLIVE_MCP_PIPE_NAME": "\\\\.\\pipe\\OliveAI_1234" }
    }
  }
}
```
Also write the pipe name to `Saved/OliveAI/stdio-pipe.txt` so the shim works even without env inheritance. On `Stop()`, remove both files.

**Acceptance criteria.** `.mcp.json` exists after editor boot, has correct pipe name. Claude Code CLI in the plugin directory can run `claude` and see the `olive-ai-studio` server listed.

**Dependencies.** P1.T4.

---

#### P1.T6 — End-to-end smoke test with Claude Code CLI

**Files:** New `Source/OliveAIEditor/Private/Tests/Integration/OliveMCPStdioTest.cpp`. Manual verification.

**Implementation notes.** Automation test `OliveAI.MCP.Stdio.PingPong` that:
1. Starts `FOliveMCPStdioTransport` on a test pipe name.
2. Spawns `node mcp-stdio.js` as a subprocess with the test pipe env var.
3. Writes `{"jsonrpc":"2.0","id":1,"method":"ping"}\n` to its stdin.
4. Reads one line from stdout.
5. Asserts response matches `{"jsonrpc":"2.0","id":1,"result":{}}`.
Manual verify: run `claude` from plugin dir, ask it to call `project.search` — confirm response returns within 500ms (baseline HTTP-bridge was 1–2s).

**Acceptance criteria.** Automated test passes. Manual Claude Code CLI smoke test: ping round-trip <100ms, `tools/list` returns within 200ms.

**Dependencies.** P1.T5.

---

#### P1.T7 — Default transport flip + migration note

**Files:** Modify `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` (change default). Add `plans/mcp-stdio-migration.md` (brief).

**Implementation notes.** Default `MCPTransportMode = Stdio`. Migration doc: one paragraph telling users who had a custom `.mcp.json` to delete it and let the plugin regenerate. Dedicated setting for backward-compat: `bKeepHttpForOneRelease = true` so power users can opt back in via Project Settings.

**Acceptance criteria.** Fresh project enables plugin → `.mcp.json` uses stdio, CLI connects first try, no HTTP port binding observed.

**Dependencies.** P1.T6.

---

### Phase 2 — `blueprint.build` / `blueprint.patch` / `blueprint.rollback`

**Goal.** Single-call plan execution. Granular ops batch into one transaction. Fingerprint drift check gone.

---

#### P2.T1 — Add RegisterAlias to FOliveToolRegistry

**Files:** Modify `Source/OliveAIEditor/Public/MCP/OliveToolRegistry.h`, `Private/MCP/OliveToolRegistry.cpp`.

**Implementation notes.** The header already defines `FOliveToolAlias` and `ResolveAlias`. Add the registration surface:
```cpp
void RegisterAlias(const FString& FromName, const FString& ToName,
                   TFunction<void(TSharedPtr<FJsonObject>&)> TransformParams = nullptr);
```
Internal: `TMap<FString, FOliveToolAlias> Aliases; mutable FRWLock AliasesLock;`. Wire `ResolveAlias` (if not already wired) to look up and apply. `HasTool(Name)` should return true for aliases as well so the MCP server accepts alias calls. Aliases must NOT appear in `GetAllTools()` / `GetToolsListMCP()`.

**Acceptance criteria.** Unit test: register `"foo.old"` → `"foo.new"`, call `ExecuteTool("foo.old", params)` — arrives at `"foo.new"`'s handler. `tools/list` does not contain `"foo.old"`.

**Dependencies.** None (Phase 2 entry point).

---

#### P2.T2 — Implement HandleBlueprintBuild

**Files:** Modify `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`, `Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`, `Blueprint/Public/MCP/OliveBlueprintSchemas.h`.

**Implementation notes.** New handler `HandleBlueprintBuild` taking `{asset_path, graph?: "EventGraph", plan_json, dry_run?: false, auto_compile?: true}`. Implementation:
1. Reuse the body of the current `HandleBlueprintApplyPlanJson` (`OliveBlueprintToolHandlers.cpp:9354+`) but:
   - Delete fingerprint logic (the entire block at ~9554–9579).
   - Delete the `bPlanJsonRequirePreviewForApply`/`bAutoPreview` branching (~9454–9471).
   - Delete the schema-version drift code that was fingerprint-related.
2. When `dry_run: true`, run resolver + validator + return preview data (lifted from `HandleBlueprintPreviewPlanJson`) without executor. Include `resolved_plan_summary` in Data.
3. Return the same result shape as current `apply_plan_json`, minus `preview_fingerprint`.

Schema: `OliveBlueprintSchemas::BlueprintBuild()` with required `asset_path`, `plan_json`; optional `graph`, `dry_run`, `auto_compile`. `Usage Guidance`: "Primary tool for building Blueprint graph logic. Resolves function names, validates structure, executes, compiles. Use `dry_run: true` to preview without mutation."

Delete `HandleBlueprintPreviewPlanJson` (dead). Keep `HandleBlueprintApplyPlanJson` temporarily — it becomes the alias target registration.

**Acceptance criteria.** Existing plan_json tests pass against `blueprint.build`. `dry_run: true` returns `{dry_run: true, resolved_steps: [...], validator_messages: [...]}` without touching the graph (verify by re-reading graph before/after).

**Dependencies.** P2.T1.

---

#### P2.T3 — Register preview/apply → build aliases

**Files:** Modify `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (`RegisterPlanTools`).

**Implementation notes.** Replace the two old registrations with:
```cpp
Registry.RegisterTool(..., "blueprint.build", ..., &HandleBlueprintBuild);
Registry.RegisterAlias("blueprint.apply_plan_json", "blueprint.build");
Registry.RegisterAlias("blueprint.preview_plan_json", "blueprint.build",
    [](TSharedPtr<FJsonObject>& Params) { Params->SetBoolField("dry_run", true); });
```
Delete the old `HandleBlueprintApplyPlanJson` + `HandleBlueprintPreviewPlanJson` registrations and the dead handler code. Delete `bPlanJsonRequirePreviewForApply` from `OliveAISettings.h`.

**Acceptance criteria.** MCP `tools/list` shows `blueprint.build`, does NOT show the old names. Calling `blueprint.apply_plan_json` still works (alias path). Calling `blueprint.preview_plan_json` returns dry-run output.

**Dependencies.** P2.T2.

---

#### P2.T4 — Implement HandleBlueprintPatch

**Files:** Modify `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`, `Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`.

**Implementation notes.** New handler `HandleBlueprintPatch` taking `{asset_path, graph?, ops: [{op: "add_node"|"remove_node"|"connect_pins"|"disconnect_pins"|"set_pin_default"|"set_node_property"|"add_variable"|"remove_variable"|"modify_variable"|"add_component"|"remove_component"|"modify_component"|"reparent_component"|"add_function"|"remove_function"|"modify_function_signature"|"add_custom_event"|"add_event_dispatcher"|"override_function"|"add_widget"|"remove_widget"|"bind_property"|"set_property", ...op_specific_params}], stop_on_error?: true}`.

Implementation:
1. Load Blueprint via asset resolver.
2. Open ONE `FScopedTransaction`.
3. For each op, dispatch to the existing granular handler (e.g., `HandleBlueprintAddNode`) by calling its static helper directly OR by invoking through the registry (`FOliveToolRegistry::Get().ExecuteTool("blueprint.add_node", ...)`). The registry path is cleaner because handlers already encapsulate validation + pipeline. Note: calling the registry re-opens a nested transaction, which UE handles (nested transactions collapse), but the `bAutoCompile` per-op is wasteful — so patch must pass `skip_compile: true` in op params, then compile once at end.
4. Collect per-op results; if `stop_on_error: true` (default), first failure stops and remaining ops are marked `skipped`.
5. After all ops, trigger a single compile if any op mutated the graph and `auto_compile: true` (default).
6. Return `{success: bool, ops: [{op, success, result}], compile: {...}}`.

Schema `BlueprintPatch` with an `oneOf` in the `ops` array schema — one subschema per op type, enforcing required fields. Model the op-union schema after OpenAPI's discriminator pattern: `{type: "object", properties: {op: {enum: [...]}}, allOf: [{if: {op: "add_node"}, then: {...}}, ...]}`. Keep it manageable by generating with a helper `MakeOpSchema(const FString& Op, TSharedPtr<FJsonObject> ParamSchema)`.

**Acceptance criteria.** Call `blueprint.patch` with 3 ops (add_node, connect_pins, set_pin_default) on a test Blueprint — single transaction, single compile, correct result shape. Undo (editor-side Ctrl+Z) rolls back all 3 as one step.

**Dependencies.** P2.T1.

---

#### P2.T5 — Register granular tool aliases → patch

**Files:** Modify `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`.

**Implementation notes.** For each old granular tool (add_node, connect_pins, etc.), keep its registration BUT register an alias so callers using the old name still route through the same handler. Wait — aliases make sense only for consolidation. Here we keep the granular tools registered AND expose `blueprint.patch` as a sibling. The aliases come in Phase 3 (consolidation). In Phase 2, `patch` is purely additive: it's a new batching tool. Old granular tools continue to work, unchanged, for the Phase 2 scope.

**Acceptance criteria.** No changes to existing tool names in Phase 2. `blueprint.patch` is net-new.

**Dependencies.** P2.T4.

---

#### P2.T6 — Implement HandleBlueprintRollback

**Files:** Modify `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`, `Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`.

**Implementation notes.** Handler `HandleBlueprintRollback` taking `{asset_path?: string, snapshot_id?: string}`:
1. If `snapshot_id` provided → call `FOliveSnapshotManager::Get().Rollback(snapshot_id, paths)` directly with NO confirmation token.
2. If only `asset_path` → find most recent snapshot touching that path via `FOliveSnapshotManager::ListSnapshotsForAsset`, then rollback.
3. If neither → error `MISSING_TARGET`.
4. Return `{reverted_assets: [...], snapshot_id}`.

One-shot semantics (no preview, no token). `project.rollback` keeps the two-step flow for multi-asset destructive operations.

Register with `tags: ["write", "danger", "rollback"]`.

**Acceptance criteria.** Mutate a Blueprint via `blueprint.build`, call `blueprint.rollback` with no args, assert content reverts. Automation test.

**Dependencies.** P2.T1.

---

#### P2.T7 — Update system prompts + recipes

**Files:** Modify files in `Content/SystemPrompts/Knowledge/` and `Content/SystemPrompts/Knowledge/recipes/blueprint/`.

**Implementation notes.** Global search-and-replace:
- `blueprint.preview_plan_json` → `blueprint.build` with note "(use `dry_run: true` for preview)"
- `blueprint.apply_plan_json` → `blueprint.build`
- Kill "preview then apply" instructions. Replace with "call `blueprint.build` once".
- Remove any `preview_fingerprint` mentions.
Keep recipe structure; only replace the tool names and collapse 2-step examples into 1 step.

Files known to contain these tokens (from the open git status + grep results): `cli_blueprint.txt`, `blueprint_authoring.txt`, `recipes/blueprint/edit_existing_graph.txt`, `recipes/blueprint/modify.txt`. Audit the full `Content/SystemPrompts/` tree before committing.

**Acceptance criteria.** `grep -r "preview_plan_json\|apply_plan_json\|preview_fingerprint" Content/SystemPrompts/` returns zero hits outside of migration notes. In-editor chat: ask the AI to modify a graph, verify it calls `blueprint.build` once.

**Dependencies.** P2.T3.

---

#### P2.T8 — Update tests for build/patch/rollback

**Files:** `Source/OliveAIEditor/Private/Tests/Integration/OliveBlueprintPlanTest.cpp` (if exists; otherwise locate via `Glob`). Plan resolver/validator/executor tests should NOT need changes (internals untouched).

**Implementation notes.** Rename/rewrite tests that invoked `blueprint.preview_plan_json` or `blueprint.apply_plan_json` to use `blueprint.build`. Add new tests:
- `OliveAI.Blueprint.Build.DryRunNoMutation` — call with `dry_run: true`, verify graph unchanged.
- `OliveAI.Blueprint.Patch.SingleTransaction` — multi-op patch, verify single undo step.
- `OliveAI.Blueprint.Rollback.MostRecentSnapshot` — mutate, rollback with no id, verify revert.
- `OliveAI.Blueprint.AliasPreviewJson` — call `preview_plan_json`, assert routed to build with `dry_run: true`.

**Acceptance criteria.** All new tests pass. Old plan_json tests still pass under the alias path.

**Dependencies.** P2.T3, P2.T4, P2.T6.

---

### Phase 3 — Tool consolidation

**Goal.** `tools/list` surface drops from ~100 to ~25. Context cost on every inference drops correspondingly. All old names keep working via aliases for one release.

---

#### P3.T1 — Design blueprint.read dispatcher

**Files:** Modify `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`, `Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`.

**Implementation notes.** New `HandleBlueprintRead(Params)`:
1. Read `scope` (required enum). Valid: `"summary"`, `"components"`, `"variables"`, `"functions"`, `"function"`, `"event_graph"`, `"hierarchy"`, `"node_pins"`, `"node_type"`, `"function_signature"`, `"overridable_functions"`, `"verify"`.
2. Read `detail_level` (optional, default `"normal"`): `"minimal"` | `"normal"` | `"full"`.
3. Read `target` (optional): function name for `scope="function"`, node id for `scope="node_pins"`, etc.
4. Dispatch table: map each scope to the existing private helper function that currently implements the corresponding standalone tool. Don't rewrite the internals — just route.
5. Apply `detail_level`: pass through to helpers (most helpers already have verbosity params; add a wrapper where missing).

Schema: single `BlueprintRead` with `scope` as enum + `examples` field showing one example per scope.

**Acceptance criteria.** `blueprint.read {asset_path, scope: "summary"}` returns the same payload as old `blueprint.read`. `scope: "components"` returns same as old `blueprint.read_components`. Test all 12 scopes against their equivalents.

**Dependencies.** P2.T1 (aliases).

---

#### P3.T2 — Alias all old blueprint readers to blueprint.read

**Files:** Modify `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`.

**Implementation notes.** In `RegisterReaderTools`, remove the registrations for `read_components`, `read_variables`, `read_event_graph`, `read_function`, `read_hierarchy`, `describe_node_type`, `describe_function`, `verify_completion`, `get_node_pins`, `list_overridable_functions`. Replace with aliases:
```cpp
Registry.RegisterAlias("blueprint.read_components", "blueprint.read",
    [](auto& P){ P->SetStringField("scope", "components"); });
// ... etc.
```
`blueprint.read` stays registered with the new unified schema.

**Acceptance criteria.** `tools/list` shows 1 reader for blueprint (was 10+). Old names still callable via alias.

**Dependencies.** P3.T1.

---

#### P3.T3 — Consolidate bt.* readers

**Files:** Modify `Source/OliveAIEditor/BehaviorTree/Private/MCP/OliveBTToolHandlers.cpp`, corresponding schemas header.

**Implementation notes.** Same pattern: inventory BT reader tools (search `TEXT("bt.` in the file), create `bt.read` with `scope` param, alias the rest. For writer consolidation, create `bt.build` that takes a BT plan IR (or a simple `{ops: [...]}` similar to `blueprint.patch`). If BT doesn't have a plan_json equivalent yet, defer writer consolidation to a follow-up phase — only do read-side consolidation now.

**Acceptance criteria.** BT read-side tools collapse to `bt.read` + `bt.build` + `bt.create`. Old names aliased.

**Dependencies.** P3.T2 (pattern established).

---

#### P3.T4 — Consolidate pcg.* readers

**Files:** Modify `Source/OliveAIEditor/PCG/Private/MCP/OlivePCGToolHandlers.cpp`.

**Implementation notes.** Same pattern: `pcg.read` dispatcher + aliases. For write-side, PCG has `pcg.add_node`, `pcg.remove_node`, `pcg.connect`, `pcg.disconnect`, `pcg.set_settings`, `pcg.execute`, `pcg.add_subgraph` — collapse into `pcg.build` with `ops: [...]` in the same shape as `blueprint.patch`. Keep `pcg.create` standalone.

**Acceptance criteria.** `pcg.read`, `pcg.build`, `pcg.create` are the only PCG tools in `tools/list`.

**Dependencies.** P3.T3.

---

#### P3.T5 — Consolidate niagara.* readers

**Files:** Modify `Source/OliveAIEditor/Niagara/Private/MCP/OliveNiagaraToolHandlers.cpp`.

**Implementation notes.** `niagara.read` (replaces `read_system`, `list_modules`, `describe_module`), `niagara.build` (replaces `add_emitter`, `add_module`, `remove_module`, `set_emitter_property`, `set_parameter`), `niagara.create` (keep). Keep `niagara.compile` separate.

**Acceptance criteria.** `niagara.read`, `niagara.build`, `niagara.create`, `niagara.compile` remain. Old names aliased.

**Dependencies.** P3.T4.

---

#### P3.T6 — Consolidate cpp.* readers

**Files:** Modify `Source/OliveAIEditor/Cpp/Private/MCP/OliveCppToolHandlers.cpp`.

**Implementation notes.** `cpp.read` (scope: `class`, `header`, `source`, `enum`, `struct`, `project_classes`, `blueprint_callable`, `overridable`). `cpp.write` (ops: `create_class`, `add_function`, `add_property`, `modify_source`). Keep `cpp.compile` separate.

**Acceptance criteria.** `cpp.read`, `cpp.write`, `cpp.compile` remain. Old names aliased.

**Dependencies.** P3.T5.

---

#### P3.T7 — Fold widget.* into blueprint.patch

**Files:** Modify `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`.

**Implementation notes.** `widget.add_widget`, `widget.remove_widget`, `widget.bind_property`, `widget.set_property` become ops under `blueprint.patch` (`op: "add_widget"`, etc.). Alias the old tool names. This is clean because Widget Blueprints are already Blueprints — no conceptual split.

**Acceptance criteria.** `widget.*` no longer in `tools/list`. Old names still callable. Patch op `add_widget` works on a test UserWidget Blueprint.

**Dependencies.** P3.T6.

---

#### P3.T8 — Prune project.* tools

**Files:** Modify `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp`, `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`.

**Implementation notes.**
- Delete: `project.batch_write`, `project.bulk_read`, `project.create_ai_character`, `project.diff`, `project.implement_interface`, `project.move_to_cpp`, `project.refactor_rename`. These are compound workflows the agent can express via `olive.build` recipes. Alias them to `olive.build` invocations with pre-canned step arrays if any existing caller depends on them; otherwise hard-delete with a clear log message `"{name} removed in 2026-04 overhaul — use olive.build"`.
- Merge: `project.index_build`, `project.index_status` → `project.search {rebuild_index: true}` / `{status_only: true}`.
- Keep: `project.search`, `project.snapshot`, `project.rollback`, `project.get_relevant_context`, `project.get_asset_info`, `project.get_dependencies`, `project.get_referencers`, `project.get_class_hierarchy`, `project.get_config`, `project.list_snapshots`.
- Consolidate the kept `get_*` variants into `project.read {scope: "asset_info"|"dependencies"|"referencers"|"class_hierarchy"|"config"|"snapshots"|"relevant_context"}`.

**Acceptance criteria.** Final `project.*` surface: `project.search`, `project.read`, `project.snapshot`, `project.rollback`. Four tools.

**Dependencies.** P3.T7.

---

#### P3.T9 — Verify total tool count and in-editor chat

**Files:** New `Source/OliveAIEditor/Private/Tests/Integration/OliveToolSurfaceTest.cpp`.

**Implementation notes.** Automation test `OliveAI.Tools.Surface.CountUnder30` that asserts `FOliveToolRegistry::Get().GetToolCount() <= 30` (target 25, headroom 5). Dumps the list to the log for manual review. Separately: manual in-editor chat round trip — open chat, issue "read this Blueprint", confirm the assistant calls `blueprint.read` and gets a valid response. Verify all three modes (Code/Plan/Ask) still work — the mode gate is upstream of aliases so alias resolution doesn't bypass gating.

**Acceptance criteria.** Test passes. In-editor chat works in all 3 modes. No orphaned old-tool calls in logs.

**Dependencies.** P3.T8.

---

### Phase 4 — RAG on `editor.run_python` errors

**Goal.** Python failures include top-3 API doc chunks in the error payload.

---

#### P4.T1 — Design FOlivePythonDocIndex

**Files:** Create `Source/OliveAIEditor/Python/Public/Docs/OlivePythonDocIndex.h`, `Private/Docs/OlivePythonDocIndex.cpp`.

**Implementation notes.**
```cpp
class OLIVEAIEDITOR_API FOlivePythonDocIndex
{
public:
    static FOlivePythonDocIndex& Get();

    /** Load index from disk if present; schedule build if missing or stale. Non-blocking. */
    void Initialize();

    /** Returns true once index is loaded and searchable. Safe to call before Initialize(). */
    bool IsReady() const;

    /** Trigger index rebuild (async, runs Python script via IPythonScriptPlugin). */
    void RebuildIndex();

    struct FDocHit
    {
        FString Symbol;       // e.g. "unreal.EditorAssetLibrary.save_asset"
        FString Signature;    // e.g. "save_asset(asset_to_save: str, only_if_is_dirty: bool = True) -> bool"
        FString Snippet;      // first ~200 chars of docstring
        float   Score;        // BM25 score
    };

    /** Top-K search. Returns empty array if index not ready. */
    TArray<FDocHit> Search(const FString& Query, int32 TopK = 3) const;

private:
    // BM25 state
    struct FDoc { FString Symbol; FString Signature; FString FullDoc; TArray<int32> TermIds; };
    TArray<FDoc> Docs;
    TMap<FString, int32> TermToId;
    TMap<int32, TArray<int32>> InvertedIndex;  // term id -> doc ids
    TMap<int32, int32> DocFreq;                // term id -> document frequency
    float AvgDocLength = 0.0f;
    mutable FRWLock IndexLock;
    FThreadSafeBool bReady{false};

    void BuildFromJson(const FString& JsonPath);
    void Tokenize(const FString& Text, TArray<FString>& OutTokens) const;
};
```
BM25 params: k1 = 1.2, b = 0.75 (standard). Tokenizer: lowercase, split on non-alphanumeric, keep dots in symbol names (so `EditorAssetLibrary.save_asset` matches).

**Acceptance criteria.** Header compiles. `Search` on an empty index returns `{}` without crashing.

**Dependencies.** None (parallel to other phase work).

---

#### P4.T2 — Write the Python doc generator

**Files:** Create `Source/OliveAIEditor/Python/Private/Docs/GeneratePythonDocs.py`.

**Implementation notes.**
```python
import unreal, json, inspect, os

output = []
for name in dir(unreal):
    obj = getattr(unreal, name)
    if inspect.isclass(obj):
        for mname, method in inspect.getmembers(obj):
            if mname.startswith('_'): continue
            try:
                sig = str(inspect.signature(method)) if callable(method) else ""
            except (ValueError, TypeError):
                sig = ""
            doc = inspect.getdoc(method) or ""
            output.append({
                "symbol": f"unreal.{name}.{mname}",
                "signature": f"{mname}{sig}",
                "doc": doc,
                "kind": "method" if callable(method) else "property"
            })
    elif callable(obj):
        # free function
        try: sig = str(inspect.signature(obj))
        except (ValueError, TypeError): sig = ""
        output.append({
            "symbol": f"unreal.{name}",
            "signature": f"{name}{sig}",
            "doc": inspect.getdoc(obj) or "",
            "kind": "function"
        })

out_path = os.path.join(unreal.Paths.project_saved_dir(), "OliveAI", "python_docs.json")
os.makedirs(os.path.dirname(out_path), exist_ok=True)
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump({"engine_version": unreal.SystemLibrary.get_engine_version(), "docs": output}, f)

print(f"[OlivePythonDocs] Wrote {len(output)} symbols to {out_path}")
```
Exclude modules starting with `_`. Cap `doc` at 2000 chars to bound index size.

**Acceptance criteria.** Running the script via `editor.run_python` produces `Saved/OliveAI/python_docs.json` with >1000 entries and file size <20MB.

**Dependencies.** P4.T1.

---

#### P4.T3 — Implement index build + persist

**Files:** Modify `Source/OliveAIEditor/Python/Private/Docs/OlivePythonDocIndex.cpp`.

**Implementation notes.**
1. `Initialize()`: check `Saved/OliveAI/python_docs.json`. If missing OR `engine_version` field mismatches current `FEngineVersion`, call `RebuildIndex()`.
2. `RebuildIndex()`: spawn async task that runs `IPythonScriptPlugin::ExecPythonCommand(ReadFile("GeneratePythonDocs.py"))`, then on completion calls `BuildFromJson`.
3. `BuildFromJson`: parse JSON, for each doc: tokenize `symbol + signature + doc`, build term frequency, update inverted index + doc freq, compute avg doc length. All under write lock. Set `bReady = true`.
4. `Search`: under read lock. Tokenize query. For each term, look up inverted index, score each candidate doc with BM25:
```
score(D, Q) = Σ IDF(qi) · (tf(qi, D) · (k1+1)) / (tf(qi, D) + k1 · (1 − b + b · |D|/avgdl))
IDF(qi) = ln((N − df(qi) + 0.5) / (df(qi) + 0.5) + 1)
```
Sort by score desc, return top K with `Snippet = FullDoc.Left(200)`.

**Acceptance criteria.** Index builds in <5s after generator finishes. `Search("save_asset")` returns `unreal.EditorAssetLibrary.save_asset` as top hit. Memory footprint <100MB.

**Dependencies.** P4.T2.

---

#### P4.T4 — Wire RAG into Python error path

**Files:** Modify `Source/OliveAIEditor/Python/Private/MCP/OlivePythonToolHandlers.cpp`.

**Implementation notes.** In `HandleRunPython`, after building error `Result`:
```cpp
if (!bSuccess && FOlivePythonDocIndex::Get().IsReady())
{
    const FString Query = ExtractQueryFromError(OutputText);  // new helper
    auto Hits = FOlivePythonDocIndex::Get().Search(Query, 3);
    if (Hits.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> HintArray;
        FString InlineSuggestion;
        for (const auto& H : Hits)
        {
            TSharedPtr<FJsonObject> HitObj = MakeShared<FJsonObject>();
            HitObj->SetStringField("symbol", H.Symbol);
            HitObj->SetStringField("signature", H.Signature);
            HitObj->SetStringField("snippet", H.Snippet);
            HintArray.Add(MakeShared<FJsonValueObject>(HitObj));
            InlineSuggestion += FString::Printf(TEXT("• %s — %s\n"), *H.Symbol, *H.Signature);
        }
        ResultData->SetArrayField("doc_hints", HintArray);
        Result.NextStepGuidance = FString::Printf(
            TEXT("Possibly relevant APIs:\n%sCheck the signatures and retry."), *InlineSuggestion);
    }
}
```
`ExtractQueryFromError`: extract the Python exception type + symbol name. Priority (regex, first match wins):
1. `AttributeError: '(\w+)' object has no attribute '(\w+)'` → `"\1 \2"`
2. `NameError: name '(\w+)' is not defined` → `"\1"`
3. `TypeError: (\w+)\(\) (got|missing|takes)` → `"\1"`
4. Fallback: the last non-empty line of the traceback.

**Acceptance criteria.** Run `unreal.EditorAssetLbrary.save_asset("/Game/Test")` (misspelled `Library`) → error payload includes `doc_hints[0].symbol == "unreal.EditorAssetLibrary.save_asset"`.

**Dependencies.** P4.T3.

---

#### P4.T5 — Module initialization + feature flag

**Files:** Modify `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`, `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`.

**Implementation notes.** Add `UPROPERTY(Config, EditAnywhere) bool bEnablePythonDocRAG = true;` to settings. In `OnPostEngineInit`, after `FOlivePythonToolHandlers::Get().RegisterAllTools()`, if Python plugin is available AND setting is enabled: `FOlivePythonDocIndex::Get().Initialize()`. Index build is async; first call to `Search` before ready returns empty (handler gracefully skips hints).

**Acceptance criteria.** On fresh editor boot with Python plugin enabled, log shows `[OlivePythonDocIndex] Building index...` then `[OlivePythonDocIndex] Indexed N symbols`. Setting toggle works (disable → no build).

**Dependencies.** P4.T4.

---

#### P4.T6 — Tests

**Files:** New `Source/OliveAIEditor/Private/Tests/OlivePythonDocIndexTest.cpp`.

**Implementation notes.**
- `OliveAI.Python.DocIndex.SearchBasic`: prebuild a synthetic JSON with 10 known entries, `Search("save_asset")` returns expected top hit.
- `OliveAI.Python.DocIndex.EmptyReturnsEmpty`: search before init returns `[]` without crashing.
- `OliveAI.Python.DocIndex.EngineVersionInvalidation`: write JSON with stale version, call `Initialize`, verify rebuild is scheduled.
- Manual: trigger a real Python error in-editor chat, verify `doc_hints` appears in tool result.

**Acceptance criteria.** All automation tests pass.

**Dependencies.** P4.T5.

---

## 4. Migration & Deprecation Strategy

### 4.1 Transport (HTTP → stdio)

**Chosen approach: dual transport for one release, stdio default.**

Rationale: External users may have locked `.mcp.json` configs and CI scripts pinning HTTP ports. A hard cutover would silently break them. We ship `EOliveMCPTransportMode::Stdio` as default, but `Http` and `Both` remain available via Project Settings for one release. The next release (2026-05 or 2026-06) removes HTTP entirely and the code for `FOliveMCPServer` HTTP routing goes away.

Migration steps for users on old config:
1. Delete existing `.mcp.json` in the plugin directory.
2. Restart editor. New `.mcp.json` is written with stdio config.
3. Restart Claude Code CLI — it re-reads `.mcp.json`.

Log a one-time warning at editor boot if `MCPTransportMode == Http`: "HTTP transport is deprecated and will be removed in the next release. Switch to Stdio in Project Settings > Olive AI > MCP Transport Mode."

### 4.2 Tool consolidation (aliases)

**Deprecation window: one release.** All removed tools stay callable via alias. `tools/list` does NOT include alias names (keeps context small for agents). Add a log message when an alias is invoked: `"Tool '{old}' is deprecated; use '{new}'"` — visible in the Output Log but not in the tool response (don't pollute the agent's result stream).

Next release: delete the alias registrations. Callers get `UNKNOWN_TOOL` errors with `Suggestion: "Use 'blueprint.read'"` — the registry already supports suggestion in error responses.

### 4.3 preview_plan_json / apply_plan_json

Same one-release alias window. Alias `apply_plan_json` → `build`, `preview_plan_json` → `build {dry_run: true}`. `bPlanJsonRequirePreviewForApply` setting is deleted immediately (ignored if present in config).

---

## 5. Testing Plan

### 5.1 Automated (UE Automation Framework, `OliveAI.*` filter)

New tests to add:
- `OliveAI.MCP.Stdio.PingPong` — P1.T6
- `OliveAI.MCP.Stdio.ToolsList` — list returns <30 tools, all have valid schemas
- `OliveAI.Tools.Surface.CountUnder30` — P3.T9
- `OliveAI.Blueprint.Build.DryRunNoMutation` — P2.T8
- `OliveAI.Blueprint.Patch.SingleTransaction` — P2.T8
- `OliveAI.Blueprint.Rollback.MostRecentSnapshot` — P2.T8
- `OliveAI.Blueprint.AliasPreviewJson` — P2.T8
- `OliveAI.Blueprint.Read.ScopeDispatch` — call each `scope` value, compare to legacy output
- `OliveAI.Python.DocIndex.SearchBasic` — P4.T6
- `OliveAI.Python.DocIndex.EmptyReturnsEmpty` — P4.T6
- `OliveAI.Python.DocIndex.EngineVersionInvalidation` — P4.T6

Existing tests that need verification (not modification — internals unchanged):
- All `Brain/`, `Conversation/`, `Providers/`, `EdgeCases/`, `PCG/` test suites should pass untouched.
- Plan resolver/validator/executor tests (locate via `Glob "Source/OliveAIEditor/Private/Tests/**/*Plan*.cpp"`) should pass; they should NOT reference MCP tool names directly.

### 5.2 Manual verification

Per-phase manual checks:
- **Phase 1:** Boot editor with stdio default → Claude Code CLI connects → run `claude --print "list blueprints in /Game/Characters"` → responds in <2s. Boot with `Http` mode → HTTP server binds port as before.
- **Phase 2:** In-editor chat: "add a print_string after BeginPlay in BP_TestActor" → AI calls `blueprint.build` once, success. `Ctrl+Z` in the editor rolls back the build.
- **Phase 3:** In-editor chat: "read this Blueprint's components" → AI calls `blueprint.read {scope: "components"}` (not `read_components`). `tools/list` via Claude Code CLI shows 25–30 tools.
- **Phase 4:** In-editor chat: "use Python to save all open assets" → AI writes a script that fails → error response contains `doc_hints` with `unreal.EditorAssetLibrary.save_loaded_assets` or similar.

---

## 6. Risks & Open Questions

1. **Named-pipe IPC on Linux WSL / Docker.** Windows named pipes don't cross the WSL boundary cleanly. If any users run the editor in Windows but Claude Code CLI inside WSL, stdio via named pipe won't work. **Mitigation:** keep `Both` transport mode for one release so Docker/WSL users can stick with HTTP+port while we design a unix-socket or TCP-localhost alternative. **Open question for user:** do you care about the WSL/Docker use case or is Windows-native the only target?

2. **Stdio protocol — MCP clients expect to own the server process.** Per MCP spec, `stdio` transport is client-spawns-server. Our setup inverts this: UE is the persistent host, the Node shim is the spawned child. Claude Code CLI should not care because it just sees stdio — but this is non-standard. If any MCP client (future Cursor version, etc.) does something funny like signal-parenting checks, we may need to revisit. Low risk but non-zero.

3. **BM25 index memory.** ~5000 UE Python symbols × ~500 bytes of tokens/docstring each → ~2.5MB per `FDoc`. With docstring capped at 2KB, index is probably <20MB RAM. Confirm with a measurement in P4.T3. If too large, persist the built index (not just the JSON source) to disk and mmap.

4. **`blueprint.patch` op union schema size.** With ~20 ops, the JSON Schema for the `ops` array becomes large. Agents will see this in `tools/list`. **Mitigation:** use `$ref` + external schema file if schema export exceeds ~10KB. Alternatively, describe op types briefly in `description` and let the agent discover details through trial/error — schema validation at handler time catches errors. **Recommendation:** keep schema compact (list op names, describe common params) and rely on handler validation for op-specific fields.

5. **`dry_run: true` semantics.** Should `dry_run` on `blueprint.build` run only the resolver, or resolver + validator + a simulated executor? Current proposal: resolver + validator (matches old `preview_plan_json`). Running the executor in dry-run would require mocking the graph mutation, which is complex. **Recommendation: resolver + validator only.** Callers wanting stronger preview can look at the resolved plan summary.

6. **`editor.run_python` doc index — what about custom unreal-python modules?** If the user has installed community packages (like `unreal_stylecheck`), they won't be in our index. Acceptable — we index `unreal.*` only. Custom modules can be re-indexed on demand via `RebuildIndex()` if we expose it as a tool; defer to a follow-up.

7. **Alias lifetime commitment.** User wants one-release deprecation — that assumes a defined release cadence. **Open question:** is there a release schedule? If releases are ad hoc, "one release" might mean "6 months". Pick a concrete date in Phase 3 completion for alias removal.

---

## 7. Out of Scope

These are explicitly NOT changed in this overhaul:

- **Plan JSON op vocabulary.** `OlivePlanOps` (`call`, `get_var`, `set_var`, `branch`, `sequence`, `cast`, `event`, `custom_event`, `for_loop`, `for_each_loop`, `while_loop`, `do_once`, `flip_flop`, `gate`, `delay`, `is_valid`, `print_string`, `spawn_actor`, `make_struct`, `break_struct`, `return`, `comment`, `call_delegate`, `call_dispatcher`, `bind_dispatcher`) stays identical.
- **Resolver / Validator / Executor.** `FOliveBlueprintPlanResolver`, `FOlivePlanValidator`, `FOlivePlanExecutor`, all their internals (alias map, FindFunctionEx, autocast, Phase 0 checks, stale event chain cleanup, duplicate node tracking) are untouched.
- **FOliveNodeFactory.** Function search order, alias map, K2_ fuzzy match — all unchanged.
- **FOliveLibraryIndex** and `FOliveTemplateSystem`. Template discovery and clone pipeline unchanged.
- **FOliveSnapshotManager.** Used as-is; `blueprint.rollback` is a thin wrapper.
- **Write pipeline.** 6 stages (validate → mode gate → transact → execute → verify → report) stay intact. `blueprint.build` still routes through it.
- **Brain layer.** State machine (Idle/Active/Cancelling), loop detector, self-correction policy, prompt distiller — all untouched.
- **Providers.** No provider changes. Claude Code, OpenRouter, ZAI, Anthropic, OpenAI, Google, Ollama, OpenAICompatible stay as-is.
- **Chat UI / UX.** `SOliveAIChatPanel`, `FOliveEditorChatSession`, `FOliveConversationManager` — unchanged. Mode gate (Code/Plan/Ask) unchanged.
- **Niagara / PCG / BT core logic.** Only the MCP tool surface is consolidated; readers/writers/catalogs underneath are unchanged.

---

## Appendix A — Current Tool Inventory (~100 tools)

For reference during consolidation work. Scanned 2026-04-16.

```
blueprint.add_component         blueprint.add_custom_event       blueprint.add_event_dispatcher
blueprint.add_function          blueprint.add_interface          blueprint.add_node
blueprint.add_variable          blueprint.apply_plan_json        blueprint.compile
blueprint.connect_pins          blueprint.create                 blueprint.create_from_template
blueprint.create_interface      blueprint.create_timeline        blueprint.delete
blueprint.describe_function     blueprint.describe_node_type     blueprint.disconnect_pins
blueprint.get_node_pins         blueprint.get_template           blueprint.list_overridable_functions
blueprint.list_templates        blueprint.modify_component       blueprint.modify_function_signature
blueprint.modify_variable       blueprint.override_function      blueprint.preview_plan_json
blueprint.read                  blueprint.read_components        blueprint.read_event_graph
blueprint.read_function         blueprint.read_hierarchy         blueprint.read_variables
blueprint.remove_component      blueprint.remove_function        blueprint.remove_interface
blueprint.remove_node           blueprint.remove_variable        blueprint.reparent_component
blueprint.scaffold              blueprint.set_node_property      blueprint.set_parent_class
blueprint.set_pin_default       blueprint.verify_completion
bt.*   (~10 tools)       pcg.*  (9 tools)        niagara.* (10 tools)
cpp.*  (11 tools)        widget.* (4 tools)
project.batch_write             project.bulk_read                project.create_ai_character
project.diff                    project.get_asset_info           project.get_class_hierarchy
project.get_config              project.get_dependencies         project.get_referencers
project.get_relevant_context    project.implement_interface      project.index_build
project.index_status            project.list_snapshots           project.move_to_cpp
project.refactor_rename         project.rollback                 project.search
project.snapshot
editor.run_python               olive.build                      olive.get_recipe
olive.search_community_blueprints
```

## Appendix B — Target Tool Inventory (~25 tools)

```
blueprint.read          blueprint.build         blueprint.patch
blueprint.rollback      blueprint.create        blueprint.compile
blueprint.delete        blueprint.list_templates blueprint.get_template
blueprint.create_from_template
bt.read                 bt.build                bt.create
pcg.read                pcg.build               pcg.create
niagara.read            niagara.build           niagara.create
niagara.compile
cpp.read                cpp.write               cpp.compile
project.read            project.search          project.snapshot
project.rollback
editor.run_python       olive.build             olive.get_recipe
```

~29 tools, within the 25–30 target band.
