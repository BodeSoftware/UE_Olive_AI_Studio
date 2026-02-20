# Implementation Plan: Complete Remaining Tasks

## Context

The Olive AI Studio plugin has its core infrastructure built (chat UI, MCP server, tool registry, Blueprint read/write pipeline, IR serialization, node catalog) but several integration gaps prevent the system from being fully functional. Blueprint tools exist as dead code (never registered at startup), IR output lacks version info, the node catalog isn't exposed via MCP, and the chat UX is missing confirmation flow, compile self-correction, and live operation status. This plan closes all gaps from `plans/remaining_tasks_verified.md`.

---

## Task 1: Wire Real Blueprint MCP Tools into Startup (P0)

**Dependencies:** None
**Files:**
- `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`
- `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp`

**Steps:**
1. Add `#include "MCP/OliveBlueprintToolHandlers.h"` to module file
2. In `OnPostEngineInit()`, after `RegisterBuiltInTools()`, call `FOliveBlueprintToolHandlers::Get().RegisterAllTools()`
3. In `ShutdownModule()`, before `FOliveMCPServer::Get().Stop()`, call `FOliveBlueprintToolHandlers::Get().UnregisterAllTools()`
4. In `OliveToolRegistry.cpp::RegisterBuiltInTools()`, remove the call to `RegisterBlueprintToolStubs()`

**Exit criteria:** `tools/list` returns 40+ real Blueprint tools, not 2 stubs.

---

## Task 2: UE 5.5 Deprecated API Fixes (P0)

**Dependencies:** None
**Files:**
- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveComponentWriter.cpp`
- `Source/OliveAIEditor/Blueprint/Private/Reader/OliveComponentReader.cpp`

**Steps:**
1. In `OliveComponentWriter.cpp::FindSCSNode()` (~line 719), replace `SCS->GetAllNodes()` with recursive traversal using `GetRootNodes()` + `GetChildNodes()` (pattern already used elsewhere in same file)
2. In `OliveComponentReader.cpp::HasComponents()` (~line 152), replace `SCS->GetAllNodes().Num() > 0` with `SCS->GetRootNodes().Num() > 0`
3. Verify `OliveNodeFactory.cpp` has no deprecated API usage

**Exit criteria:** No `GetAllNodes()` deprecation warnings on UE 5.5 compile.

---

## Task 3: Emit and Parse schema_version in IR Serialization (P0)

**Dependencies:** None
**Files:**
- `Source/OliveAIRuntime/Private/IR/BlueprintIR.cpp`

**Steps:**
1. Add `#include "IR/OliveIRSchema.h"`
2. In `FOliveIRBlueprint::ToJson()`, add `Json->SetStringField(TEXT("schema_version"), OliveIR::SchemaVersion)` as first field
3. In `FOliveIRBlueprint::FromJson()`, parse `schema_version` if present: `BP.SchemaVersion = JsonObject->GetStringField(TEXT("schema_version"))`

**Exit criteria:** All IR JSON output includes `"schema_version": "1.0"`. Validator (`OliveIRSchema.cpp:66-78`) finds the field.

---

## Task 4: Node Catalog Lifecycle + MCP Resource Exposure (P0)

**Dependencies:** None
**Files:**
- `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`
- `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp`
- `Source/OliveAIEditor/Blueprint/Private/Catalog/OliveNodeCatalog.cpp` (verify API)

**Steps:**
1. In module startup, call `FOliveNodeCatalog::Get().Initialize()` (or equivalent build method)
2. In module shutdown, call shutdown/cleanup on the catalog
3. In `OliveMCPServer.cpp::HandleResourcesList()`, add `olive://blueprint/node-catalog` and `olive://blueprint/node-catalog/search` entries
4. In `HandleResourcesRead()`, handle the new URIs — serialize catalog listing and search results to JSON
5. Add helper methods to `FOliveNodeCatalog` if needed (`GetCatalogJson()`, `SearchJson(Query)`)

**Exit criteria:** `resources/list` includes catalog resources; `resources/read` returns catalog data and search results.

---

## Task 5: Confirmation Flow End-to-End in Chat UX (P1)

**Dependencies:** Task 1
**Files:**
- `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` + `.h`
- `Source/OliveAIEditor/Private/UI/SOliveAIMessageList.cpp` + `.h`
- `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp` + `.h`

**Steps:**
1. **ConversationManager:** In `HandleToolResult()`, detect `requires_confirmation` in result JSON. Fire new `OnConfirmationRequired` delegate with token + plan. Pause agentic loop (store pending state).
2. **ConversationManager:** Add `ConfirmPendingOperation()` — executes confirmed tool call via write pipeline's `ExecuteConfirmed(token)`, adds result to history, continues loop. Add `DenyPendingOperation()` — adds denial result, continues.
3. **MessageList:** Add `AddConfirmationWidget(ToolCallId, Plan, OnAction)` — shows plan text + Confirm (green) / Deny (red) buttons. On click, fires delegate and replaces with status text.
4. **ChatPanel:** Bind `OnConfirmationRequired` → call `MessageList->AddConfirmationWidget(...)` with delegates to `ConfirmPendingOperation()` / `DenyPendingOperation()`.

**Exit criteria:** Tier 2/3 operations pause with confirm/deny UI. Confirm resumes; Deny cancels. MCP still bypasses (via `bFromMCP` flag).

---

## Task 6: Compile-Fail Self-Correction Loop (P1)

**Dependencies:** Task 1
**Files:**
- `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp` + `.h`

**Steps:**
1. Add `CompileRetryCount` (int32, default 0) and `MaxCompileRetries` (constexpr 3) to header
2. In `HandleToolResult()`, check result data for `compile_result.success == false`. If failed and retries < max, increment counter and append retry hint to tool result content
3. Reset `CompileRetryCount = 0` in `SendUserMessage()`
4. Verify write pipeline's `StageReport()` includes compile errors in result JSON (fix if missing)

**Key insight:** No special retry loop needed — the existing agentic loop already sends tool results back to the provider. Enriching the result with compile error details + retry hint lets the AI naturally self-correct.

**Exit criteria:** Compile failures trigger up to 3 auto-correction attempts. Counter resets per user message.

---

## Task 7: Operation Feed Live Progress (P1)

**Dependencies:** None
**Files:**
- `Source/OliveAIEditor/Private/UI/SOliveAIMessageList.cpp` + `.h`

**Steps:**
1. Add `EOliveToolCallStatus` enum (Running, Completed, Failed)
2. Replace `TMap<FString, TSharedPtr<SWidget>> ToolCallWidgets` with struct-based `TMap<FString, FOliveToolCallWidgetState>` holding named widget refs (StatusIcon, StatusText, SummaryText)
3. Refactor `AddToolCallIndicator()` to use `SAssignNew` for named widgets stored in state struct
4. Implement `UpdateToolCallStatus()` — swap spinner for check/error icon, update status text, show result summary
5. Update `ClearMessages()` to clear new state map

**Exit criteria:** Tool indicators animate from spinner → checkmark/error. Brief summary shown per tool.

---

## Task 8: Close Partial Implementations with Phase 2 Gating (P2)

**Dependencies:** None
**Files:**
- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveAnimGraphWriter.cpp`
- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveWidgetWriter.cpp`
- `Source/OliveAIEditor/Blueprint/Private/Reader/OliveBlueprintReader.cpp`
- `Source/OliveAIEditor/Blueprint/Private/Reader/OliveWidgetTreeSerializer.cpp`

**Steps:**
1. Replace all bare TODOs with structured `FOliveIRMessage` warnings (severity: Warning, code: `PHASE2_DEFERRED`)
2. AnimGraphWriter: Gate animation asset assignment and transition rule parsing with logged warnings
3. WidgetWriter: Gate property binding with warning in tool result
4. BlueprintReader: Implement basic compile message extraction or gate with warning; extract basic delegate signatures
5. WidgetTreeSerializer: Gate binding extraction and named slots with warnings

**Exit criteria:** No silent data loss from TODOs. All deferred features return structured warnings visible in tool results.

---

## Task 9: Acceptance Test Scenarios (P2)

**Dependencies:** Tasks 1–7
**Files to create:**
- `docs/verification/phase-1-acceptance-proof.md`

**Scenarios to test:**
- A: Create BP_HealthPickup via chat (components + event)
- B: Same via MCP (verify parity)
- C: Trigger Tier 2 confirmation flow
- D: Force compile error → verify self-correction
- E: Query node catalog via MCP resource

---

## Execution Order

```
Parallel batch 1 (P0, no dependencies):
  Task 1 — Wire Blueprint tools
  Task 2 — UE 5.5 fixes
  Task 3 — IR schema_version
  Task 4 — Node catalog

Sequential (P1, after Task 1):
  Task 5 — Confirmation flow
  Task 6 — Compile self-correction

Parallel with P1:
  Task 7 — Operation feed (no dependencies)

After all P0+P1:
  Task 8 — Phase 2 gating
  Task 9 — Acceptance tests
```

## Verification

After all tasks:
1. **Compile test:** Full plugin build on UE 5.5 — zero errors, zero deprecation warnings
2. **MCP test:** `tools/list` returns 40+ tools; `resources/list` includes node catalog
3. **Chat test:** Send "Create a Blueprint with a variable" — observe tool calls, operation feed status, and completion
4. **Confirmation test:** Set variable operations to Tier 2 in settings, trigger from chat, verify confirm/deny UI
5. **Self-correction test:** Create intentionally broken wiring, observe retry attempts in chat
