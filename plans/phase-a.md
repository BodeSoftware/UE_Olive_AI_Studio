
## Phase A: Control + Safety Hardening

### A1. Tier 3 Preview Generation (Non-Destructive)

Problem:
- Tier 3 routes to `PreviewOnly` but preview generation is stubbed.

Deliverables:
- Implement a real preview payload for Tier 3 operations in the write pipeline:
  - `plan`: human-readable, concise
  - `preview`: structured summary of intended changes
  - `impact`: dependencies/referencers summary (warnings)
  - `confirmation_token`: required to execute
- Execution path must require explicit confirmation token for Tier 3 (not just `confirmed=true`).

Implementation notes:
- Preferred approach: “simulate by inspecting intent + environment”, not full dry-run mutation.
- For Blueprint graph edits, preview should list:
  - target asset + graph name
  - nodes to create/remove (type, display name)
  - pin connections to add/remove
  - property/default changes
  - compile action (if enabled)

Primary code touchpoints:
- `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
- `Source/OliveAIEditor/Blueprint/Public/Pipeline/OliveWritePipeline.h`
- Tool handlers should pass through tokens for confirmed execution:
  - `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
  - `Source/OliveAIEditor/CrossSystem/Private/MCP/OliveCrossSystemToolHandlers.cpp`

Acceptance criteria:
- Tier 3 tool call returns `requires_confirmation=true` with a non-empty `preview` object.
- Re-submitting with `confirmation_token` performs the operation; missing/expired token errors out.
- Preview includes dependency warnings when available.

---

### A2. Structured Compile Errors (Not Generic Failure)

Problem:
- Compile verification returns generic failure; hard for non-code users and for self-correct loop.

Deliverables:
- Extract structured compile errors from Blueprint compile logs:
  - error message + severity
  - (if possible) graph/function name
  - (if possible) node title / node guid / node id
  - actionable suggestion where deterministic

Primary code touchpoints:
- `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
- `Source/OliveAIEditor/Blueprint/Private/Compile/OliveCompileManager.cpp` (if already parsing patterns)

Acceptance criteria:
- A failing compile produces error objects, not just a single generic message.
- UI can render compile errors as clickable items later (Phase B).

---

### A3. Schema Validation: Upgrade From “Required Fields Only”

Problem:
- Schema validation currently checks only required fields; invalid shapes slip through.

Deliverables:
- Implement a pragmatic schema validator sufficient for tool safety:
  - type checking for `string/int/number/bool/object/array`
  - enum validation where schema provides `enum`
  - required properties
  - `additionalProperties` false support where used
  - shallow recursion for objects/arrays (avoid full JSON Schema spec)

Primary code touchpoints:
- `Source/OliveAIEditor/Private/Services/OliveValidationEngine.cpp`

Acceptance criteria:
- Passing wrong types produces deterministic `INVALID_PARAMS` style errors.
- Core tools and Blueprint/BT/PCG tools reject malformed arguments reliably.

---

### A4. Policy Controls: Rate Limiting + Hard Safety Rails

Deliverables:
- Add `MaxWriteOpsPerMinute` policy:
  - apply to write categories (Blueprint/BT/PCG/C++ writers, cross-system writers)
  - include retry-after hint in error
- Confirm hard rails always remain on, even in YOLO:
  - PIE protection
  - transactions/undo
  - asset existence and path safety rules

Primary code touchpoints:
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`
- `Source/OliveAIEditor/Private/Services/OliveValidationEngine.cpp`

Acceptance criteria:
- A burst of writes triggers throttling with a clear error and suggestion.
- YOLO never bypasses PIE locks or path safety rules.

---

### A5. MCP Notifications Transport (Progress for Big Tasks)

Problem:
- MCP notifications are log-only; big runs are opaque to external clients.

Deliverables:
- Add at least one push-capable transport (choose one):
  1. SSE endpoint (HTTP server supports server push via a long-lived response)
  2. WebSocket endpoint (preferred if UE HTTP server support is workable)
  3. Poll-based “events since cursor” endpoint (lowest risk, still useful)
- Emit operation progress events from tool execution and compile verification.

Primary code touchpoints:
- `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp`

Acceptance criteria:
- External client can observe step/progress events for a multi-tool operation.

---

### A6. Hybrid (C++ vs Blueprint) Decision Policy (Control-Layer Enforced)

Goal:
- Avoid duplicate implementations and “thrash” when both BP and C++ can satisfy a request.

Deliverables:
- Add a simple, explicit policy in prompts + enforcement hooks:
  - Default: prefer Blueprint for gameplay wiring and iteration unless user asked for C++.
  - Prefer C++ when:
    - user requests performance/replication/core framework
    - creating reusable systems intended for many BPs
    - modifying an existing C++ base class is required for correctness
  - When both exist:
    - do not re-create functionality in BP if a reflected C++ method already exists
    - do not patch C++ to mirror BP unless user confirms
    - prefer exposing C++ to BP (UPROPERTY/UFUNCTION) over duplicating logic

Implementation notes:
- Phase A goal is making this predictable via:
  - structured “intent” flags in tool parameters where needed (e.g., `preferred_layer: "bp"|"cpp"|"hybrid"`)
  - clear “already exists” detection in C++ reflection reader and Blueprint readers

Primary touchpoints:
- `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp` (policy text injection)
- `Source/OliveAIEditor/Cpp/Private/Reader/OliveCppReflectionReader.cpp`
- `Source/OliveAIEditor/Blueprint/Private/Reader/OliveBlueprintReader.cpp`

Acceptance criteria:
- When a Blueprint request targets a class with existing C++ API, results reference and use that API.
- No duplicate function creation when the capability already exists in C++.

---

### A7. C++-Only Tasks (Rules, Safety, and UX Contracts)

Goal:
- Make pure C++ tasks first-class: clear constraints, safe patching, and predictable compile behavior.

Definition:
- A “C++-only task” is any request that should be satisfied without creating/modifying Blueprint assets, except where explicitly requested (e.g., “also expose to BP”).

Routing rules:
- If user says “C++ only”, set intent to `preferred_layer=cpp` and enforce:
  - do not create/modify any Blueprint assets
  - do not generate Blueprint scaffolding as a side effect
- If user requests both (“C++ + expose to BP”), treat as hybrid:
  - C++ changes are primary
  - Blueprint changes are limited to exposure/usage wiring (e.g., calling the new UFUNCTION)

Safety requirements (must hold even in YOLO):
- Path safety:
  - only allow relative paths inside project `Source/` for source edits
  - block traversal and unsupported extensions
- Compile guard:
  - block C++ writes while Live Coding compile is in progress
- Bounded edits:
  - `cpp.modify_source` requires `anchor_text` and a bounded operation (`replace|insert_before|insert_after`)
  - reject ambiguous anchors (0 matches or >1 matches)
- Transactions:
  - use explicit “patch units” (each tool call is an undoable unit; UE’s `FScopedTransaction` doesn’t cover file edits, so record a rollback plan)
- Rollback:
  - capture pre-edit file snapshots (in-memory or on disk) per run checkpoint
  - allow rollback via `project.rollback` when C++ tasks are part of a run

Verification:
- For read-only tasks: no compile required.
- For write tasks:
  - if Live Coding is enabled, prefer `cpp.compile` via Live Coding (or expose a compile tool result)
  - otherwise, warn that full build is external and provide instructions in the report

UX contracts (Phase B UI must support):
- C++ file cards:
  - show file path, summary of changes, and a small diff snippet around anchor
  - buttons: “Open file”, “Copy path”, “Rollback change”
- Compile result cards:
  - show success/failure, key errors, and next action
- Run Mode integration:
  - C++ steps can be part of a run and share checkpoints
  - rollback restores modified files and invalidates any cached reflection data

Primary touchpoints:
- Tools:
  - `Source/OliveAIEditor/Cpp/Private/MCP/OliveCppToolHandlers.cpp`
  - `Source/OliveAIEditor/Cpp/Private/Writer/OliveCppSourceWriter.cpp`
  - `Source/OliveAIEditor/Cpp/Private/Reader/OliveCppSourceReader.cpp`
  - `Source/OliveAIEditor/Cpp/Private/Reader/OliveCppReflectionReader.cpp`
- Validation:
  - `Source/OliveAIEditor/Private/Services/OliveValidationEngine.cpp`
- Cross-system rollback/checkpoints:
  - `Source/OliveAIEditor/CrossSystem/Private/OliveSnapshotManager.cpp`

Acceptance criteria:
- A “C++ only” request never touches Blueprint assets.
- Source patch operations are bounded and reversible.
- Compile guard prevents edits during live compilation.
- The UI reports exactly what changed and how to rollback.