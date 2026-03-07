# Agent Pipeline Integration (Phase 8)

## Files Modified
- `Public/Providers/OliveCLIProviderBase.h` - Added `FOliveAgentPipelineResult CachedPipelineResult` + `bool bIsReviewerCorrectionPass`
- `Private/Providers/OliveCLIProviderBase.cpp` - Replaced discovery pass + decomposition directive with pipeline; added Reviewer in HandleResponseCompleteAutonomous
- `Public/Chat/OliveConversationManager.h` - Added same two members
- `Private/Chat/OliveConversationManager.cpp` - Three integration points: SendUserMessage, BuildSystemMessage, HandleComplete

## Key Design Decisions
- Pipeline runs in `SendMessageAutonomous()` BEFORE the CLI process launches (blocking, tick-pumped)
- Pipeline runs in `SendUserMessage()` only when `bTurnHasExplicitWriteIntent` is true
- `CachedPipelineResult.FormatForPromptInjection()` produces the markdown block
- Reviewer runs after Builder completes in both autonomous and orchestrated paths
- `bIsReviewerCorrectionPass` flag prevents infinite review->correct->review loop
- Autonomous Reviewer: dispatches correction via `SendMessageAutonomous()` with AliveGuard + saved callbacks
- Orchestrated Reviewer: uses `ActiveContextPaths` for modified asset collection (HistoryStore.Records is private)
- CLIProviderBase reviewer must set `bIsBusy = false` before re-dispatching (SendMessageAutonomous sets it back to true)

## FOliveOperationHistoryStore API Notes
- `GetTotalRecordCount()` returns count; `Records` array is **private**
- No `GetRecord(int32 Index)` method; use `GetRunHistory(RunId)` or `GetStepHistory(RunId, Step)` for access
- `FOliveOperationRecord.AffectedAssets` (TArray<FString>), NOT `AssetPath`

## Pipeline Classes
- `FOliveAgentPipeline` at `Public/Brain/OliveAgentPipeline.h` / `Private/Brain/OliveAgentPipeline.cpp`
- NOT a singleton -- instantiate per run on the stack
- `Execute()` returns `FOliveAgentPipelineResult`; `RunReviewer()` returns `FOliveReviewerResult`
- Result types in `Public/Brain/OliveAgentConfig.h`
