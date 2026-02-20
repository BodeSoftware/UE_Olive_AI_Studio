Remaining Tasks (Verified)




## P0 -

1. Wire real Blueprint MCP tools into startup (replace stubs)
- Files:
  - `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp`
  - `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`
  - `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
- Work:
  - Remove/retire `RegisterBlueprintToolStubs()` usage from built-in registration path.
  - Call `FOliveBlueprintToolHandlers::Get().RegisterAllTools()` during startup.
  - Call `FOliveBlueprintToolHandlers::Get().UnregisterAllTools()` on shutdown.
- Exit criteria:
  - `tools/list` shows real Blueprint tools (reader + writer set), not stub descriptions.

2. Complete UE 5.5 compatibility fixes and compile clean
- Files:
  - `Source/OliveAIEditor/Blueprint/Private/Writer/OliveComponentWriter.cpp`
  - `Source/OliveAIEditor/Blueprint/Private/Reader/OliveComponentReader.cpp`
  - `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`
- Work:
  - Replace remaining deprecated SCS usage (`GetAllNodes()` where needed).
  - Run full plugin compile and resolve any remaining UE 5.5 API errors/warnings.
- Exit criteria:
  - Plugin builds successfully on UE 5.5 with no blocker errors.

3. Finalize IR schema lock in runtime serialization
- Files:
  - `Source/OliveAIRuntime/Private/IR/BlueprintIR.cpp`
  - `Source/OliveAIRuntime/Public/IR/BlueprintIR.h`
- Work:
  - Emit `schema_version` from `FOliveIRBlueprint::ToJson()`.
  - Parse/read `schema_version` in `FromJson()` consistently.
  - Verify strict validator + examples remain consistent.
- Exit criteria:
  - All Blueprint IR output includes `schema_version: "1.0"`.

4. Node Catalog runtime integration + MCP resource exposure
- Files:
  - `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`
  - `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp`
  - `Source/OliveAIEditor/Blueprint/Private/Catalog/OliveNodeCatalog.cpp`
- Work:
  - Initialize/shutdown node catalog with module lifecycle.
  - Add MCP resources/endpoints for catalog listing/search/filter data.
- Exit criteria:
  - Catalog is available from MCP `resources/list` and readable via `resources/read`.

## P1
5. Confirmation flow end-to-end in chat UX
- Files:
  - `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
  - `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`
  - `Source/OliveAIEditor/Private/UI/SOliveAIMessageList.cpp`
- Work:
  - Consume `requires_confirmation` responses in chat loop.
  - Add confirm/deny flow with confirmation token routing.
- Exit criteria:
  - Tier 2/3 operations reliably pause and resume only after explicit confirmation.

6. Compile-fail self-correction loop (up to 3 retries)
- Files:
  - `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
  - `Source/OliveAIEditor/Blueprint/Private/Compile/OliveCompileManager.cpp`
  - `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`
- Work:
  - Return structured compile errors from write/compile stages.
  - Feed errors back into retry loop logic with bounded attempts.
- Exit criteria:
  - Multi-step operations retry automatically on compile failure and stop at configured max attempts.

7. Operation feed live progress completeness
- Files:
  - `Source/OliveAIEditor/Private/UI/SOliveAIMessageList.cpp`
  - `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`
- Work:
  - Show step-by-step status for tool chains (started/running/completed/failed).
  - Surface tool result summaries instead of raw-only payloads.
- Exit criteria:
  - User can track each step of a multi-tool Blueprint operation live in chat.

## P2 - Phase 1 Acceptance Closure

8. Close known partial implementations
- Files:
  - `Source/OliveAIEditor/Blueprint/Private/Writer/OliveAnimGraphWriter.cpp`
  - `Source/OliveAIEditor/Blueprint/Private/Writer/OliveWidgetWriter.cpp`
  - `Source/OliveAIEditor/Blueprint/Private/Reader/OliveBlueprintReader.cpp`
  - `Source/OliveAIEditor/Blueprint/Private/Reader/OliveWidgetTreeSerializer.cpp`
- Work:
  - Resolve or explicitly gate documented TODO/deferred behaviors (transition-rule logic, binding depth, compile-message extraction).
- Exit criteria:
  - No hidden TODOs that break declared Phase 1 behavior.

9. Run and record acceptance scenarios
- Suggested artifacts:
  - `docs/verification/phase-1-acceptance-proof.md` (update with real runtime evidence)
  - New test log for "create a health pickup" (chat + MCP parity)
- Work:
  - Validate each Task 16 criterion with runtime evidence.
- Exit criteria:
  - Acceptance checklist complete with reproducible proof.

## Definition of Done for This Remaining List

- No Blueprint stubs are active in runtime tool registration.
- Blueprint MCP tool suite is callable from chat and external MCP clients.
- UE 5.5 compile passes.
- Node catalog is live as MCP resource.
- Confirmation tiers, compile self-correction, and operation feed are visibly functional.
- Task 16 acceptance criteria are evidenced, not inferred.
