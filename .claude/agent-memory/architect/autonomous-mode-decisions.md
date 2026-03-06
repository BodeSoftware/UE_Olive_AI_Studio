# Autonomous Mode Architecture Decisions

## NeoStack Migration (Autonomous MCP Mode) - Feb 2026
- `plans/neostack-migration-impl.md` -- 10 tasks across 7 phases
- **Core concept**: Claude Code runs as long-lived process, discovers tools via MCP natively, manages its own agentic loop. Plugin is just a tool server.
- **Feature flag**: `bUseAutonomousMCPMode` on UOliveAISettings (default true). Fallback to orchestrated path when false.
- **CLI args change**: Remove `--max-turns 1`, `--strict-mcp-config`, `--append-system-prompt`. Add `--max-turns 25` as safety ceiling (NOT orchestration).
- **New method**: `SendMessageAutonomous()` on CLIProviderBase -- simpler than SendMessage(). No prompt building, no tool schema serialization, no tool call parsing. Just user message to stdin, stream stdout, wait for exit.
- **Interface addition**: `IOliveAIProvider::SendMessageAutonomous()` with default error impl. Only CLIProviderBase overrides.
- **ConversationManager routing**: `IsAutonomousProvider()` checks provider name + settings flag. Autonomous path bypasses: BuildSystemMessage, GetAvailableTools, PromptDistiller, iteration budgets, correction directives, ProcessPendingToolCalls, SelfCorrectionPolicy, LoopDetector, zero-tool re-prompts.
- **Dynamic .mcp.json**: Written by MCP server after successful Start(), cleaned up on Stop(). Uses bridge format for backward compat.
- **AGENTS.md rewrite**: 60-80 lines of focused tool-usage guide. Removes architecture docs, build commands, file locations.
- **Nothing gets deleted**: Autonomous path is purely additive. Orchestrated CLI + API paths preserved behind feature flag.
- **MCP server already works**: HandleToolsCallAsync dispatches to game thread, sets MCP origin context, returns structured results. No tool-side changes needed.
- **Cost controls**: `AutonomousMaxRuntimeSeconds` (default 300), `AutonomousMaxTurns` (default 25). Both configurable via settings.
- **ParseOutputLine**: `tool_use`/`tool_result` events become informational progress chunks instead of warnings.

## Timeout and Plan Reliability Fixes - Feb 2026
- `plans/timeout-and-plan-reliability.md` -- 12 tasks across 4 issues (C, B, A, D)
- **Issue C (HIGHEST): Partial success rollback cascade**: Pipeline Stage 6 (Report) flips `bSuccess=false` when compile has errors (line 596-598). Post-pipeline rollback at tool handler line 7291 checks `!PipelineResult.bSuccess` and triggers `RollbackPlanNodes()` which removes ALL created nodes. Fix: check `ResultData.status == "partial_success"` before triggering rollback. ResultData survives pipeline stages (shared pointer copy-and-extend pattern).
- **Issue A (activity timeout)**: Replace hard 300s runtime limit with activity-based timeout using `FOliveMCPServer::OnToolCalled` delegate. `std::atomic<double> LastToolCallTimestamp` on CLIProviderBase. New setting `AutonomousIdleToolSeconds` (default 120). Hard limit raised to 900s as safety net.
- **Issue D (redundant previews)**: Prompt guidance only.
- **New setting**: `AutonomousIdleToolSeconds` (int32, default 120, 0-600)

## Autonomous Efficiency Round 2 - Feb 2026
- `plans/autonomous-efficiency-round2.md` -- 4 targeted fixes for autonomous mode
- **Task 1 (call_delegate op)**: Event dispatchers are NOT UFunctions. UE uses `UK2Node_CallDelegate` (inherits `UK2Node_BaseMCDelegate`), not `UK2Node_CallFunction`. New `OlivePlanOps::CallDelegate` + `OliveNodeTypes::CallDelegate`.
- **Task 2 (ExpandedPlan propagation) -- CRITICAL BUG**: `ExpandComponentRefs` mutates a LOCAL copy inside `Resolve()`, but the executor receives the ORIGINAL unmutated plan. Fix: add `ExpandedPlan` field to `FOlivePlanResolveResult`, propagate to all downstream consumers.
- **Tasks 3-4 (prompt)**: Recipe lookup made mandatory. Done condition added.

## Autonomous Efficiency Round 3 - Feb 2026
- `plans/autonomous-efficiency-round3.md` -- 3 fixes for stalling/continuation/redundant reads
- **Fix 1 (anti-stall interleave)**: Replaced "scaffold everything then wire" multi-asset guidance with "complete ONE asset fully before starting next". Changes in 4 files: AGENTS.md, recipe_routing.txt, cli_blueprint.txt, OliveCLIProviderBase.cpp sandbox CLAUDE.md.
- **Fix 2 (continue context injection)**: `FAutonomousRunContext` struct on CLIProviderBase tracks original message + modified asset paths + tool call log + run outcome. `IsContinuationMessage()` detects "continue"/"keep going"/"finish". `BuildContinuationPrompt()` enriches with previous run context. `FOnMCPToolCalled` delegate extended to 3 params (adds Arguments JSON) for asset path extraction.
- **Fix 2 key insight**: `OnToolCalled.Broadcast` fires BEFORE execution -- cannot capture per-call success/failure. Acceptable because we only need asset list + tool sequence, not outcomes.
- **Fix 3 (template result enrichment)**: `function_details` and `event_graph_details` arrays added to ApplyTemplate result. Each entry has `has_graph_logic`, `node_count`, `plan_steps` (condensed step summaries), `plan_errors`. Eliminates post-template read flood (7 reads -> 0).
- **Fix 3 key data**: `PlanResult.CreatedNodes.Num()` provides node count. Step summaries built from `Plan.Steps` (step_id + op + target).
- **Implementation order**: Fix 1 (30min) -> Fix 3 (1.5hr) -> Fix 2 (3hr). All independent, can parallelize.

## Autonomous Efficiency Round 4 - Feb 2026
- `plans/autonomous-efficiency-round4.md` -- 5 fixes (slim templates, auto-continue, tool filter, resolver notes, auto-read)
- **Fix 1 (slim templates)**: Remove `plan` keys from ALL factory template JSON files (gun.json: 3 functions, stat_component.json: 1 function). Templates create structure only (components, vars, empty function stubs, dispatchers). AI writes its own plan_json. Update 4 prompt locations (AGENTS.md, recipe_routing.txt, cli_blueprint.txt, sandbox CLAUDE.md).
- **Fix 1 key finding**: No-template run was 2x faster (4:53 vs 10:11). AI micro-edited template plans instead of trusting them. AI is demonstrably good at writing plan_json from recipes alone.
- **Fix 2 (auto-continue on stall)**: Reduce `CLI_IDLE_TIMEOUT_SECONDS` from 120 to 50. After idle timeout + real work done + not at max retries, auto-relaunch via `SendMessageAutonomous(BuildContinuationPrompt("continue"))`. `AutoContinueCount` (max 3) on CLIProviderBase. Only fires for `IdleTimeout`, NOT `RuntimeLimit`. Uses `AsyncTask(GameThread)` to avoid re-entering from completion handler.
- **Fix 3 (tool filter)**: `SetToolFilter(TSet<FString>)` / `ClearToolFilter()` on FOliveMCPServer. Filters `HandleToolsList` only -- `HandleToolsCall` accepts ANY tool (defense-in-depth). `DetermineToolPrefixes()` does keyword matching on user message. Default (ambiguous): blueprint + project + olive + cross-system (~35 tools). Multi-domain: no filter (all 104). `ToolFilterLock` for thread safety.
- **Fix 4 (component event notes)**: Component bound events already work end-to-end (ResolveEventOp passes through to CreateEventNode which does SCS scan). Only change: better error suggestion text + resolver note for non-EventNameMap events.
- **Fix 5 (auto-read small BPs)**: `AUTO_FULL_READ_NODE_THRESHOLD = 50`. In HandleBlueprintRead, count nodes via UBlueprint graph arrays before calling Reader. Below threshold: auto-upgrade summary to full read. Adds `read_mode: "auto_full"` hint to result JSON.
- **All fixes independent** -- can fully parallelize. Total ~4.5 hours.

## Idle-After-Read Nudge + Template Rewrite - Feb 2026
- `plans/idle-nudge-template-fix.md` -- 2 fixes for autonomous pickup task idle timeout
- **Root cause**: AI reads pickup_interaction template, sees complex multi-asset plan with CRITICAL RULES, stalls 180s reasoning, gets killed with zero writes.
- **Fix 1 (read-nudge)**: New `IsReadOperation()` helper + `BuildReadNudgePrompt()` method on CLIProviderBase. When idle timeout fires after a read op (get_template, get_recipe, search, etc.), auto-continue with targeted nudge instead of reporting to user. Shares `AutoContinueCount` budget (max 3) with write-stall auto-continue.
- **Fix 1 timeouts lowered**: `CLI_IDLE_TIMEOUT_SECONDS` 120->90, `CLI_EXTENDED_IDLE_TIMEOUT_SECONDS` 180->150, `AutonomousIdleToolSeconds` default 240->120.
- **Fix 1 key insight**: The existing `HandleResponseCompleteAutonomous` had a branch at line 993 that explicitly logged "reporting to user instead of auto-continuing" for read-op stalls. This was the wrong policy -- the AI needed a push, not a death sentence.
- **Fix 2 (template rewrite)**: `pickup_interaction.json` rewritten from 4 patterns + CRITICAL RULES to 1 pattern with 7 sequential steps. 66 lines, no MUST/NEVER language. Preserves metadata fields for catalog compatibility.

## Codex CLI Provider - Mar 2026
- `plans/codex-cli-provider-design.md` -- FOliveCodexProvider subclass of FOliveCLIProviderBase
- **Key Codex CLI facts**: `codex exec --json` for non-interactive JSONL output. Native Rust binary via npm. No `--max-turns`, no `--append-system-prompt`, no `--strict-mcp-config`.
- **Direct HTTP MCP**: Codex connects directly to `http://localhost:PORT/mcp` via `-c 'mcp_servers.olive.url="..."'`. No mcp-bridge.js needed.
- **Sandbox refactor**: `SetupAutonomousSandbox()` made virtual. New `WriteProviderSpecificSandboxFiles()` hook. Claude writes `.mcp.json` + `CLAUDE.md`. Codex writes nothing (MCP via CLI flag).
- **IsAutonomousProvider fix**: New `SupportsAutonomousMode()` virtual on IOliveAIProvider (default false, true in FOliveCLIProviderBase). Replaces hardcoded `== "Claude Code CLI"` check.
- **EOliveAIProvider::Codex**: New enum value. No API key field (auth via `codex login` or `OPENAI_API_KEY` env var).
- **JSONL format MUST be verified empirically** before implementing ParseOutputLine().
- **Phase 1 = autonomous only**. Orchestrated mode deferred (no system prompt injection mechanism).
- **Codex binary discovery**: `where codex` -> npm global `codex.exe` path. Prefer native `.exe` over Node.js launcher.
