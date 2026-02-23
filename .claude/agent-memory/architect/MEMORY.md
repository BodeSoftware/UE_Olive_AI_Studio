# Architect Agent Memory

## Key Patterns

- **Tool handler pattern**: validate params -> load Blueprint -> build FOliveWriteRequest -> bind executor lambda -> `ExecuteWithOptionalConfirmation`
- **Pre-pipeline validation**: Cheap checks (type validation, duplicate detection) belong BEFORE the write pipeline, not inside the executor lambda. This avoids unnecessary transaction overhead.
- **FOliveWriteResult factory methods**: Use `::ExecutionError()`, `::Success()`, `::ValidationError()`, `::ConfirmationNeeded()` -- do not construct manually.
- **FOliveToolResult vs FOliveWriteResult**: Tool handlers return `FOliveToolResult`; pipeline returns `FOliveWriteResult` which converts via `.ToToolResult()`.
- **Singleton pattern**: All service classes (NodeCatalog, NodeFactory, GraphWriter, WritePipeline, BlueprintReader) are singletons via `static Foo& Get()`.

## Architecture Decisions

### Phase 2 (Graph Edit Integrity) - Feb 2026
- Node type validation added to NodeFactory as `ValidateNodeType()` method, called before creation attempt.
- Fuzzy suggestions use `FOliveNodeCatalog::FuzzyMatch()` returning simple struct (not USTRUCT) with TypeId/DisplayName/Score.
- Duplicate native event check happens in HandleBlueprintAddNode before pipeline entry, with defense-in-depth in CreateEventNode.
- Node removal broken-link capture done via new `CaptureNodeConnections()` on GraphWriter, called before `RemoveNode()`.
- Orphaned exec flow detection added to `VerifyBlueprintStructure` in Stage 5, scoped to the affected graph only.
- Large graph threshold: 500 nodes. Page size: 100 (max 200). Full NodeIdMap built even for paged reads to preserve cross-page connection references.
- `FOliveIRMessage` needs `TSharedPtr<FJsonObject> Context` field for structured context on warnings (additive, non-breaking IR change).

## Error Code Convention
- Use `SCREAMING_SNAKE_CASE` for error codes
- Existing: `VALIDATION_MISSING_PARAM`, `ASSET_NOT_FOUND`, `BP_ADD_NODE_FAILED`, `BP_REMOVE_NODE_FAILED`
- Phase 2 added: `NODE_TYPE_UNKNOWN`, `DUPLICATE_NATIVE_EVENT`, `ORPHANED_EXEC_FLOW` (warning)

### Phase 3 (Chat UX Resilience) - Feb 2026
- ConversationManager ownership moves from SOliveAIChatPanel to FOliveEditorChatSession singleton.
- Panel holds weak ref, rebinds delegates on open/close. Closing panel does NOT cancel operations.
- FOliveMessageQueue: FIFO queue (max 5) for user messages during processing. Drains one at a time on completion.
- FOliveProviderRetryManager: wraps provider with exponential backoff (1s/2s/4s, max 3 retries). Rate-limit Retry-After honored up to 120s.
- Provider error classification uses parseable prefix format `[HTTP:{code}:RetryAfter={s}]` to avoid breaking IOliveAIProvider interface.
- FOlivePromptDistiller::Distill() return type changed from void to FOliveDistillationResult for truncation metadata.
- Truncation note injected into model-visible context when distillation summarizes messages.
- Response truncation detected via FinishReason=="length", warning appended to message.
- Focus profile switch deferred when processing; applied on completion.
- FNotificationInfo toast for background completion when panel is closed.
- Design doc: `plans/phase3-chat-ux-resilience-design.md`

## File Structure
- Tool handlers: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (very large file, 3000+ lines)
- Schemas: `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`
- Pipeline: `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
- Node catalog: `Source/OliveAIEditor/Blueprint/Private/Catalog/OliveNodeCatalog.cpp`
- IR structs: `Source/OliveAIRuntime/Public/IR/CommonIR.h` (FOliveIRGraph, FOliveIRNode, FOliveIRMessage, etc.)
