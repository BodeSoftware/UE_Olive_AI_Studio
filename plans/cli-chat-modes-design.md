# Design: CLI-Style Chat Modes (Code / Plan / Ask)

**Author:** Creative & Design Lead
**Date:** 2026-03-15
**Status:** DRAFT -- awaiting user approval

---

## Developer Intent

The UE developer wants to talk to an AI assistant inside the editor the same way they talk to Claude Code in a terminal. They want autonomy by default -- the AI picks its approach and executes -- with the option to slow down (Plan mode) or go read-only (Ask mode) when the situation calls for it.

The current system over-engineers the decision about "should this action happen." Confirmation tiers, safety presets, focus profiles, tool pack gating, plan-first routing thresholds, brain layer state machines -- all of these add friction that the autonomous MCP mode already bypasses. The in-engine chat should match the autonomous experience, not fight it.

**Skill level assumption:** Intermediate UE user who understands Blueprints conceptually but may not know internal node names or pin wiring. They want the AI to be a capable colleague who builds things, not a rigid tool that asks permission at every step.

**Target experience:** Type a request, watch it happen, interrupt if needed. The AI should feel like a fast-moving pair programmer who explains what they're doing as they do it -- not a bureaucracy that requires pre-approval for every write operation.

---

## Current Behavior (What Exists Today)

### Components in scope for change

| Component | What it does | Status after redesign |
|-----------|-------------|----------------------|
| `EOliveConfirmationTier` (3 tiers) | Per-category write approval (auto/plan+confirm/preview) | **REMOVE** |
| `EOliveSafetyPreset` (Careful/Fast/YOLO) | Global tier modifier | **REMOVE** |
| `FOliveWritePipeline::StageConfirm()` | Stage 2 confirmation routing | **SIMPLIFY** (always auto-execute, mode check only) |
| `FOliveFocusProfileManager` (3 profiles) | Tool visibility filtering | **REMOVE** |
| `OliveToolPacks.json` (4 packs) | Dynamic tool gating per turn intent | **ALREADY DEPRECATED** (comment in ConversationManager.h) |
| `EOliveBrainState` (7 states) | Brain layer state machine | **SIMPLIFY** to 3 states |
| `EOliveWorkerPhase` (5 phases) | Worker sub-states | **KEEP** (internal telemetry) |
| `FOliveRunManager` | Multi-step run tracking | **KEEP** (checkpoint/rollback) |
| `FOliveSelfCorrectionPolicy` | Retry evaluation, error classification | **KEEP** |
| `FOliveLoopDetector` | Infinite loop detection | **KEEP** |
| `FOliveSnapshotManager` | Pre-run snapshots, rollback | **KEEP** |
| `FOliveWritePipeline` (6 stages) | Write safety pipeline | **KEEP** stages 1,3,4,5,6. Stage 2 becomes mode gate. |
| `SOliveAIChatPanel` | Chat UI | **MODIFY** (add mode switcher, remove focus/safety dropdowns) |

### What the autonomous mode already does right

The Claude Code autonomous path (`SendMessageAutonomous`) skips all of the above. It launches a CLI process with MCP tool discovery, the MCP server sets `bFromMCP = true` on all tool calls, and `StageConfirm` is bypassed entirely. The AI runs freely with snapshots as the safety net.

This design extends that philosophy to the in-engine chat path (`SendMessage`), which currently still goes through the orchestrated loop with tier routing, focus profile filtering, and tool pack gating.

---

## Competitor Reference

### Claude Code CLI
- **Three modes:** Code (default, autonomous), Plan (think then approve), Ask (read-only). Toggled via `/code`, `/plan`, `/ask` slash commands or `--mode` flag.
- **Permission model:** In Code mode, tool calls execute freely. Dangerous operations (file delete, git force push) prompt once. The user can pre-approve patterns via allowlists.
- **What works:** The mode concept is simple, memorable, and covers real use cases. Users naturally shift between "build this" and "explain this" without changing settings.
- **What Olive should adopt:** The three-mode model maps directly to our use case. Slash commands for mode switching. Minimal prompt difference between modes -- the same AI identity, just different tool access.

### Cursor (Agent Mode)
- **Single agent mode with interrupt:** The AI executes autonomously. The user sees each file edit streaming in and can accept/reject per-file. No pre-planning phase -- the agent just goes.
- **Tool visibility:** All tools always available. No profile filtering. The AI decides what to use.
- **What works:** Zero-config autonomy. The user trusts the agent and has undo as the escape hatch.
- **What Olive should adopt:** The "all tools visible, undo as safety net" model. Olive already has snapshots -- they're the equivalent of git undo.

### NeoStack / Betide ACP
- **Agent-driven model:** The AI agent has full access to material, Niagara, sequencer, and Blueprint tools. No pre-filtering by domain.
- **Multi-agent:** Supports parallel subagents for different concerns (material generation while Blueprint logic runs).
- **What works:** Domain-agnostic tool access. The AI picks the right tool for the job without being told which domain it's in.
- **What Olive should adopt:** The all-tools-visible approach (which we already do in Auto profile). The subagent concept for parallel research while the main agent builds.

---

## The Three Modes

### Code Mode (Default)

**What it does:** Full autonomous execution. The AI reads, plans, builds, compiles, and self-corrects without asking permission. The user sees streaming output and can interrupt at any time.

**Tool access:** All tools. No filtering. No gating. No confirmation tiers.

**When the AI pauses for input:**
- Never for standard operations (create, add variable, add component, wire graph, compile).
- Asset deletion prompts once: "I'm about to delete BP_OldGun. Proceed?" This is the only mandatory confirmation.
- Reparenting to a fundamentally different class prompts once: "Changing BP_Enemy parent from Character to Pawn will remove all Character-specific components. Proceed?"
- These are not "tiers" -- they're specific dangerous operations hardcoded in the write pipeline, not configurable per-category.

**System prompt delta from Ask/Plan:** One sentence at the end of the system prompt: "You are in Code mode. Execute the user's request using whatever tools and approach you judge best. Take snapshots before destructive changes."

**Safety net:** Auto-snapshot before every run (already implemented). Compile-after-write (already implemented). Self-correction on errors (already implemented). Loop detection (already implemented).

### Plan Mode

**What it does:** The AI researches, reads assets, searches templates, thinks through the approach, and presents a structured plan. No write tools execute until the user says "go" (or a variant: "do it", "execute", "approved", etc.).

**Tool access:**
- **Unrestricted:** All read tools (`blueprint.read`, `project.search`, `blueprint.describe_node_type`, `blueprint.list_templates`, `blueprint.get_template`, `olive.get_recipe`, `project.get_asset_info`, etc.)
- **Blocked:** All write tools return a soft error: `{"error": "PLAN_MODE", "message": "Write tools are blocked in Plan mode. Present your plan and the user will approve execution.", "suggestion": "Describe what you would do, then wait for the user to switch to Code mode or say 'go'."}`
- **Exception:** `blueprint.preview_plan_json` is allowed (it's read-only, generates a preview without mutating).

**How execution starts:**
- User explicitly switches to Code mode via `/code`, then sends the task message.
- Plan mode does not auto-detect approval phrases. The user is in control of mode switching.

**System prompt delta:** "You are in Plan mode. Research the codebase and present a plan. Use read tools freely. Do not execute write operations until the user approves."

**Use cases:**
- Complex refactoring where the user wants to review before changes happen.
- Learning/exploration: "How would you build an inventory system?" -- the AI plans without executing.
- Multi-Blueprint systems where the user wants to approve the asset decomposition.

### Ask Mode

**What it does:** Read-only. The AI answers questions, explains code, searches the project, reads Blueprints. No mutations at all.

**Tool access:**
- **Unrestricted:** All read tools.
- **Blocked:** All write tools return: `{"error": "ASK_MODE", "message": "Write tools are not available in Ask mode. Switch to Code or Plan mode to make changes."}`

**System prompt delta:** "You are in Ask mode. Answer questions about the project and codebase. Do not make any changes."

**Use cases:**
- "What does BP_Enemy's TakeDamage function do?"
- "Show me all Blueprints that implement BPI_Interactable."
- "Explain the component hierarchy of BP_Vehicle."

---

## Mode Switching UX

### Slash Commands (Primary)

Type in the chat input field:
- `/code` -- switch to Code mode
- `/plan` -- switch to Plan mode
- `/ask` -- switch to Ask mode
- `/mode` -- show current mode
- `/status` -- show mode + brain state + last run outcome + active tool count (debug info without cluttering normal UI)

These are handled client-side in `SOliveAIInputField` before being sent to the conversation manager. They don't go to the AI -- they're immediate UI actions.

### Mode Indicator (Visual)

A compact mode badge in the input area, to the left of the text field:

```
[CODE]  Create a bow and arrow system
[PLAN]  How would you build an inventory?
[ASK]   What does BP_Enemy do?
```

The badge is clickable -- clicking cycles through modes (Code -> Plan -> Ask -> Code). Color-coded:
- Code: green (go, active, building)
- Plan: amber (thinking, reviewing)
- Ask: blue (reading, learning)

### Keyboard Shortcut

`Ctrl+Shift+M` cycles modes. Same as clicking the badge.

### Implicit Mode Hints

The AI does NOT auto-switch modes based on message content. If the user is in Code mode and asks "what does this do?", the AI answers the question AND remains in Code mode. Mode is always explicit.

Plan mode stays in Plan mode. The user explicitly switches to Code mode via `/code` when ready to execute. There is no auto-detection of "approval phrases" — that pattern was removed because it caused false positives (e.g. "yes" in a discussion triggered silent Code mode execution).

---

## System Prompt Strategy Per Mode

### Shared Base (All Modes)

The base system prompt stays the same across all modes. It's the AI's identity and capabilities:

```
You are Olive AI, an expert AI assistant for Unreal Engine 5.5 development
integrated directly into the editor. You help developers create and modify
Blueprints, Behavior Trees, PCG graphs, C++ classes, and other game assets.
```

Plus the shared rules from `Base.txt` (read before write, compile after changes, etc.) and the knowledge packs (blueprint authoring, recipe routing, design patterns, etc.).

### Mode-Specific Suffix

Appended as the last paragraph of the system prompt. This is the imperative channel -- last position means highest attention.

**Code mode:**
```
You are in Code mode. Execute the user's request fully -- research, plan, build,
compile, and verify. Use whatever tools and approach you judge best. Take a
snapshot before destructive changes. Do not ask for permission on standard
operations.
```

**Plan mode:**
```
You are in Plan mode. Research the codebase and present a structured plan.
Use read tools freely to understand the current state. Do not execute write
operations. Present your plan as: 1) what assets to create/modify, 2) what
each asset needs (components, variables, functions), 3) how assets communicate.
The user will approve before you build.
```

**Ask mode:**
```
You are in Ask mode. Answer questions about the project using read tools.
Explain what you find clearly. Do not make any changes to assets.
```

### Token Budget

Mode suffix adds ~50 tokens. Removing focus profile prompt additions, safety preset context, and tool pack descriptions saves ~200-400 tokens. Net savings.

---

## What Gets REMOVED

### 1. Confirmation Tiers (entire system)

**Remove:**
- `EOliveConfirmationTier` enum
- `EOliveSafetyPreset` enum
- All 7 per-category tier settings in `UOliveAISettings` (VariableOperationsTier, ComponentOperationsTier, CreateOperationsTier, FunctionCreationTier, GraphEditingTier, RefactoringTier, DeleteOperationsTier)
- `SafetyPreset` setting
- `GetEffectiveTier()`, `SetSafetyPreset()`, `OnSafetyPresetChanged`
- `FOliveWritePipeline::GetOperationTier()`, `TierToRequirement()`, `GeneratePlanDescription()`
- `FOliveWritePipeline::GeneratePreview()` (Tier 3 preview path)
- `PendingConfirmations` map and `GenerateConfirmationToken()` in WritePipeline
- `BuildPreviewPayload()`, `BuildImpactAnalysis()`, `BuildStructuredChanges()`
- All confirmation state in ConversationManager: `bWaitingForConfirmation`, `PendingConfirmationToolCallId`, `PendingConfirmationToolName`, `PendingConfirmationArguments`, `PendingConfirmationToken`, `PendingConfirmationQueue`, `FPendingConfirmationRequest`
- `ConfirmPendingOperation()`, `DenyPendingOperation()`, `ActivateNextPendingConfirmation()`
- `OnConfirmationRequired` delegate
- `HandleConfirmationRequired` in chat panel
- `BuildSafetyPresetToggle()` in chat panel

**Replace with:** A simple mode check in the write pipeline. In Plan/Ask mode, Stage 2 returns a block error. In Code mode, Stage 2 is a no-op (pass-through). Two specific destructive operations (delete asset, reparent to incompatible class) get a one-time prompt regardless of mode -- hardcoded, not configurable.

### 2. Focus Profiles (entire system)

**Remove:**
- `FOliveFocusProfileManager` singleton
- `FOliveFocusProfile` struct
- All profile registration, filtering, migration code
- `SetFocusProfile()`, `GetFocusProfile()`, `DeferredFocusProfile` in ConversationManager
- `OnDeferredProfileApplied` delegate
- `BuildFocusDropdown()` and `BuildFocusProfileMenuContent()` in chat panel
- `DefaultFocusProfile` setting
- `CustomFocusProfilesJson` and `CustomFocusProfilesSchemaVersion` settings
- `GetAvailableTools()` filtering in ConversationManager (replace with "return all tools")
- `ProfileCapabilityPackIds` mapping in PromptAssembler (all modes get all knowledge packs)
- `GetProfilePromptAddition()`, `GetAllowedWorkerDomains()`

**Replace with:** Nothing. All tools are always visible. All knowledge packs are always loaded. The AI decides what to use based on the task.

### 3. Tool Packs (already deprecated)

**Remove:**
- `OliveToolPacks.json`
- `FOliveToolPackManager` singleton (already deprecated per ConversationManager.h comment)
- `bTurnHasExplicitWriteIntent` and `bTurnHasDangerIntent` intent flags in ConversationManager
- All references to tool pack gating

**Replace with:** Nothing. Already deprecated.

### 4. Plan-First Graph Routing

**Remove:**
- `bEnforcePlanFirstGraphRouting` setting
- `PlanFirstGraphRoutingThreshold` setting
- Any code that forces plan_json after N granular tool calls

**Replace with:** Nothing. The AI chooses plan_json or granular tools based on its own judgment.

### 5. Brain Layer State Simplification

**Current states (7):** Idle, Planning, WorkerActive, AwaitingConfirmation, Cancelling, Completed, Error

**New states (3):** `Idle`, `Active`, `Cancelling`

- `Planning` merges into `Active` (planning IS activity).
- `AwaitingConfirmation` is removed entirely (no confirmations in Code mode; Plan mode blocks at the tool layer, not the brain layer).
- `Completed` and `Error` merge into `Idle` with an outcome field on the run.
- Worker phases stay as internal telemetry (Streaming, ExecutingTools, Compiling, SelfCorrecting, Complete).

### 6. Settings Cleanup

**Remove from UOliveAISettings:**
- Entire "Confirmation" category (7 tier settings + safety preset)
- `DefaultFocusProfile`, `CustomFocusProfilesJson`, `CustomFocusProfilesSchemaVersion`
- `bEnforcePlanFirstGraphRouting`, `PlanFirstGraphRoutingThreshold`

**Add to UOliveAISettings:**
- `EOliveChatMode DefaultChatMode = EOliveChatMode::Code` (new enum: Code, Plan, Ask)

### 7. UI Simplification

**Remove from chat panel:**
- Focus profile dropdown (`BuildFocusDropdown`, `BuildFocusProfileMenuContent`)
- Safety preset toggle (`BuildSafetyPresetToggle`)

**Add to chat panel:**
- Mode badge (compact, clickable, left of input field)
- Slash command parsing in input field

---

## What Stays

### Transactions and Undo (`FOliveTransactionManager`)
Every write operation is wrapped in an `FScopedTransaction`. This is non-negotiable UE infrastructure. The user can always Ctrl+Z.

### Snapshots (`FOliveSnapshotManager`)
Auto-snapshot before autonomous runs. Manual snapshot creation. Diff and rollback. This is the primary safety net that replaces confirmation tiers.

### Self-Correction (`FOliveSelfCorrectionPolicy`)
Error classification (A=FixableMistake, B=UnsupportedFeature, C=Ambiguous). Progressive error disclosure. Plan dedup. Granular fallback after repeated plan failures. All of this stays -- it's the AI correcting its own mistakes, not asking the user for help.

### Loop Detection (`FOliveLoopDetector`)
Prevents infinite tool call loops. Essential safety mechanism. Stays unchanged.

### Compile After Write
`bAutoCompileAfterWrite` stays. The write pipeline's Stage 5 (Verify) stays. Compile errors are fed back to the AI for self-correction.

### Write Pipeline (Stages 1, 3, 4, 5, 6)
- Stage 1 (Validate): Input validation, preconditions. Stays.
- Stage 2 (Confirm): **Simplified** -- mode gate only. Code mode = pass-through. Plan/Ask mode = block with mode-specific error. Two hardcoded destructive-op prompts.
- Stage 3 (Transact): FScopedTransaction. Stays.
- Stage 4 (Execute): Mutation. Stays.
- Stage 5 (Verify): Structure checks + compile. Stays.
- Stage 6 (Report): Result assembly. Stays.

### Plan JSON
The preview/apply cycle stays. `bPlanJsonRequirePreviewForApply` stays. The resolver, validator, and executor all stay. These are execution tools, not permission mechanisms.

### Knowledge Packs
All knowledge packs stay and are loaded for all modes. No more per-profile filtering. The AI gets blueprint_authoring, recipe_routing, node_routing, blueprint_design_patterns, and events_vs_functions regardless of what it's doing.

### MCP Server
Stays unchanged. External agents (Claude Code CLI) connect via MCP. The in-engine chat uses the same tool handlers. `bFromMCP` continues to mark tool calls from external agents.

### Run Manager
Multi-step run tracking with checkpoints. Stays. The run concept is orthogonal to modes -- a Code mode run creates checkpoints, a Plan mode "execution" creates checkpoints.

### Templates (Philosophy Change)

Templates stay. The catalog block stays in the prompt. Template tools (`list_templates`, `get_template`, `create_from_template`) stay registered.

**What changes is the relationship between the AI and templates.**

Current system: recipes and routing prompts push the AI toward templates. "Search templates before creating" is a soft mandate in the system prompt. The plan-first routing threshold enforces template lookup after N granular calls.

New system: templates are **context the AI knows about**, not a step the AI is forced through.

- The catalog block remains in the system prompt — it tells the AI what templates exist, what projects are indexed, and how many functions each library has. This is valuable context.
- Quality signals stay on templates. If a factory template has `"quality": "high"` or a library project has better coverage, the AI can prefer it. Quality metadata is informational, not routing logic.
- No routing forces the AI to search templates before creating a Blueprint. If the AI judges that a template fits, it uses one. If the task is novel or simple enough to build from scratch, it builds from scratch.
- Reference templates remain descriptive (architecture patterns, component layouts) — they inform design decisions, they don't prescribe tool sequences.
- Library templates remain searchable — the three-tier discovery (catalog → search → read function) still works. The AI uses it when it wants context on how similar things are built.

**Prompt change:** Remove any "search templates first" directives from the system prompt. Replace with a neutral mention: "Templates are available for common patterns — use `list_templates` to search if you want reference material." One sentence, informational tone.

**Source quality context stays in the prompt.** The AI is currently informed that:
- **Library templates** (extracted from real projects, hand-tagged) are the highest quality — full node graphs, real patterns, reliable.
- **Community Blueprints** (user-submitted, varied authors) have mixed quality — the AI should cross-reference multiple when using them rather than trusting a single one blindly.
- **Factory templates** are parameterized and tested.
- **Reference templates** are hand-written architectural guides.

This is contextual knowledge about the data sources, not behavioral routing. It stays as-is in the prompt. The AI uses this to judge how much to trust what it reads — same way a developer would treat official docs vs. a random forum post. No changes needed here.

---

## Streaming and Visibility

### What the User Sees in Code Mode

The chat panel shows a real-time activity stream, similar to Claude Code's terminal output:

1. **AI thinking text** streams in as it arrives. The user sees the AI's reasoning in real time.

2. **Tool calls** appear as collapsible cards:
   ```
   > blueprint.create  /Game/Blueprints/BP_Bow  [0.3s]
   > blueprint.add_variable  BowDamage (Float)  [0.1s]
   > blueprint.add_component  SkeletalMeshComponent  [0.2s]
   > blueprint.apply_plan_json  EventGraph: BeginPlay + Fire  [1.2s]
   > blueprint.compile  BP_Bow  [0.8s]  SUCCESS
   ```
   Each card is expandable to show full input/output JSON.

3. **Compile results** are highlighted:
   - Green checkmark: compiled successfully
   - Red X with error text: compile failed (AI will self-correct)

4. **Progress indicator:** A small animated indicator showing the AI is active. Not a progress bar (we don't know total steps). Just "working..." with the current tool name.

5. **Interrupt button:** Replaces the send button while processing. One click cancels the current run. The snapshot is preserved for rollback.

### What the User Sees in Plan Mode

1. **AI thinking text** streams normally.
2. **Read tool calls** appear as cards (same as Code mode).
3. **The plan** appears as a structured block:
   ```
   ## Plan

   1. Create BP_Bow (Actor, parent: AActor)
      - SkeletalMeshComponent (BowMesh)
      - ArrowSpawnPoint (SceneComponent)
      - Variables: BowDamage (Float), DrawSpeed (Float), bIsDrawing (Bool)
      - Functions: Fire(), StartDraw(), EndDraw()

   2. Create BP_Arrow (Actor, parent: AActor)
      - ProjectileMovementComponent
      - Variables: ArrowDamage (Float), ArrowSpeed (Float)
      - Functions: Launch(Direction, Speed)

   3. Communication: BP_Bow spawns BP_Arrow, passes damage via SpawnActor

   Ready to execute? Say "go" or switch to Code mode.
   ```
4. **No write tool cards** appear (writes are blocked).

### What the User Sees in Ask Mode

1. **AI thinking text** streams normally.
2. **Read tool calls** appear as cards.
3. **No plan or execution blocks** -- just conversation and information.

### Mode Transition Feedback

When the user switches modes, a system message appears:
```
[System] Switched to Plan mode. Read tools are active. Write tools will be
blocked until you approve a plan.
```

Short, functional, not cute.

---

## The Permission Prompt Model (Code Mode)

### Philosophy

Claude Code's permission model is "allow everything, prompt on danger." Not "prompt on everything, allow some." The current tier system inverts this -- it defaults to blocking and requires configuration to allow.

### Two Hardcoded Prompts

In Code mode, exactly two operations pause for user input:

**1. Asset Deletion**
When the AI calls `blueprint.delete` (or any future asset deletion tool):
```
Olive wants to delete BP_OldGun (/Game/Blueprints/BP_OldGun).
This cannot be undone via Ctrl+Z. A snapshot was taken.
[Allow] [Deny]
```
This is implemented as a special case in the write pipeline, not a tier. The check is: `if (ToolName == "blueprint.delete" && CurrentMode == Code)`.

**2. Destructive Reparenting**
When `blueprint.set_parent_class` would change to an incompatible parent (e.g., Character -> Pawn, Widget -> Actor):
```
Olive wants to change BP_Enemy's parent from ACharacter to APawn.
This will remove CharacterMovementComponent and all Character-specific features.
[Allow] [Deny]
```

### Everything Else: Auto-Execute

All other operations execute without prompting:
- Creating Blueprints
- Adding/removing variables and components
- Creating functions
- Wiring graph logic (plan_json or granular)
- Compiling
- Adding interfaces
- Modifying component properties

The safety net is: transactions (Ctrl+Z), snapshots (rollback to before the run started), and self-correction (the AI fixes its own compile errors).

### User-Configurable Allowlist (Future)

A future enhancement (not in this design) could add a `.olive-permissions` file similar to Claude Code's allowlist:
```json
{
  "allow": ["blueprint.delete"],
  "deny": ["blueprint.set_parent_class"]
}
```
This would let power users skip even the two hardcoded prompts. Not needed for v1.

---

## Subagent Architecture (In-Engine)

### Why Subagents

The autonomous CLI path already spawns a single long-lived process. But there are real scenarios where parallel work helps:

1. **Research while building.** The main agent is wiring BP_Bow's EventGraph. A research subagent is reading BP_Arrow's template to prepare for the next Blueprint.

2. **Validation after execution.** The main agent finishes a plan_json apply. A validation subagent reads back the graph to check for orphaned flows, missing connections, or unexpected node states -- independent of the compile check.

3. **Blueprint analysis for complex reads.** When the user asks about a 500+ node Blueprint, a dedicated reader can page through the graph while the main agent maintains conversation flow.

### Subagent Types

**Research Subagent**
- Access: Read tools only (same as Ask mode tool set)
- Lifetime: Spawned per-task, dies when results are delivered
- Concurrency: One at a time (same as the CLI explorer rule)
- Surfaced to user: Results appear as a collapsible "Research" card in the chat stream
- Implementation: A lightweight `FOliveSubagentRunner` that creates a mini conversation with read-only tool access. Uses the same provider but with a focused system prompt.

**Validator Subagent**
- Access: `blueprint.read`, `blueprint.verify`, `blueprint.get_node_pins` -- pure read tools
- Lifetime: Spawned after each significant write batch (plan_json apply, multi-tool sequence)
- Concurrency: One at a time, runs after the main agent's write tools complete
- Surfaced to user: Results appear as a "Validation" card. Green = clean. Yellow = warnings. Red = issues found (fed back to main agent).
- Implementation: Internal only -- not a user-spawnable agent. The brain layer triggers it after write batches.

**Blueprint Analyzer (Deferred)**
- For large graphs (500+ nodes). Paginates reads and builds a summary.
- This is more of a "tool enhancement" than a subagent. Defer to a later design.

### Subagent Implementation Approach

Subagents are NOT separate CLI processes. They're lightweight in-process workers that:
1. Get a filtered tool list (read-only for research, validation set for validator)
2. Get a focused system prompt (one paragraph describing their role)
3. Use the same AI provider as the main agent
4. Run on the game thread (same as current tool execution)
5. Report results back to the main conversation as system messages

The key constraint: only ONE subagent active at a time, and it cannot run in parallel with write operations. Subagents execute during the "thinking" phase -- after reads, before writes, or after writes before the next user message.

This is intentionally conservative. Parallel execution introduces race conditions with Blueprint modification. The UE editor API is not thread-safe. Parallel subagents that only READ are theoretically safe, but the engineering cost of ensuring no write tool leaks through is not worth it for v1.

### User Interaction with Subagents

Users don't manage subagents directly. They're invisible infrastructure. The user sees:
- "Researching templates..." (research subagent running)
- "Validating Blueprint structure..." (validator subagent running)

If we later want user-spawnable subagents, the slash command would be `/research [query]` to explicitly trigger a research pass. But for v1, the main agent decides when to spawn subagents.

---

## Mode Implementation in the Write Pipeline

### New Stage 2 Logic

```
Stage2_ModeGate(Request, CurrentMode):
  if CurrentMode == Ask:
    return BlockError("ASK_MODE", "Read-only mode. Switch to Code or Plan to make changes.")

  if CurrentMode == Plan:
    if Request.ToolName == "blueprint.preview_plan_json":
      return PassThrough  // preview is read-only
    return BlockError("PLAN_MODE", "Write blocked in Plan mode. Present your plan, then the user will approve.")

  // Code mode: check for the two hardcoded destructive-op prompts
  if Request.ToolName == "blueprint.delete":
    return PromptUser("Delete {AssetName}? This cannot be undone via Ctrl+Z. A snapshot was taken.")

  if Request.ToolName == "blueprint.set_parent_class" && IsDestructiveReparent(Request):
    return PromptUser("Reparent {AssetName} from {OldParent} to {NewParent}? This will remove incompatible components.")

  return PassThrough  // all other writes auto-execute
```

This replaces the entire `GetOperationTier()` -> `TierToRequirement()` -> `GeneratePlanDescription()` -> token/queue flow.

### Mode Storage

`EOliveChatMode` is stored on `FOliveConversationManager` (replaces `ActiveFocusProfile`). It's session-scoped -- resets to default on new session. Persisted as a user setting for the default.

```cpp
enum class EOliveChatMode : uint8
{
    Code,   // Full autonomous execution
    Plan,   // Read + plan, write blocked until approved
    Ask     // Read-only
};
```

### Tool List Filtering by Mode

In `GetAvailableTools()`:
- **Code mode:** Return all registered tools.
- **Plan mode:** Return all tools (the AI needs to see write tools to plan, but they'll return errors when called). Actually -- this is a design choice. Option A: return all tools so the AI can plan using them. Option B: return only read tools so the AI never tries to call writes.

**Decision: Option A (return all tools).** Rationale: When the AI plans, it needs to know what write tools exist and what their schemas are. If we hide write tools, the AI can't make a concrete plan ("I'll call blueprint.apply_plan_json with these steps"). The mode gate in the write pipeline catches actual execution. The tool-level error message tells the AI it's in Plan mode and should present the plan instead of executing.

- **Ask mode:** Return only read tools. The AI shouldn't even see write tools in Ask mode -- there's no reason to reference them. This reduces prompt token cost and prevents the AI from suggesting writes it can't do.

### Integration with Autonomous Mode

The autonomous MCP path (`SendMessageAutonomous`) needs mode awareness:

- **Code mode:** Unchanged. The CLI process runs freely.
- **Plan mode:** The stdin prompt includes "You are in Plan mode..." The MCP server's tool handlers check the mode and return errors for write tools. This means the mode must be accessible from the MCP tool dispatch path, which currently always sets `bFromMCP = true`.
- **Ask mode:** Same as Plan but all write tools blocked.

Implementation: Add `EOliveChatMode CurrentMode` to the MCP server state (set by ConversationManager before launch). Tool handlers check `FOliveMCPServer::Get().GetCurrentMode()` in addition to checking `bFromMCP`.

---

## Brain Layer Simplification

### New State Machine

```
Idle --> Active --> Idle
  |                  ^
  +---> Cancelling --+
```

Three states:
- **Idle:** No active operation. Outcome of last run available via `GetLastOutcome()`.
- **Active:** AI is working (streaming, executing tools, compiling, self-correcting -- all subsumed).
- **Cancelling:** User hit stop. Draining in-flight operations.

Worker phases remain as internal telemetry but are not exposed as state machine transitions. They're metadata on the Active state: `GetWorkerPhase()` still works for UI status display.

### Run Lifecycle

```
User sends message
  -> Brain transitions Idle -> Active
  -> Snapshot taken (if Code mode)
  -> AI processes (streaming, tool calls, self-correction)
  -> Brain transitions Active -> Idle (with outcome: Completed/PartialSuccess/Failed)

User hits Stop
  -> Brain transitions Active -> Cancelling
  -> In-flight ops drain
  -> Brain transitions Cancelling -> Idle (with outcome: Cancelled)
```

---

## Settings Migration

### Removed Settings

When users upgrade, their existing confirmation tier and focus profile settings become orphaned in `DefaultEditor.ini`. UE handles this gracefully -- unknown keys are preserved but ignored.

For clean migration, `UOliveAISettings::PostInitProperties()` can:
1. Check for the old `SafetyPreset` key.
2. If it was `YOLO`, set `DefaultChatMode = Code`.
3. If it was `Careful`, set `DefaultChatMode = Plan`. (Careful users who wanted confirmation probably want Plan mode.)
4. If it was `Fast`, set `DefaultChatMode = Code`.
5. Log the migration.

### New Settings

```cpp
// Replaces: SafetyPreset, DefaultFocusProfile, all tier overrides
UPROPERTY(Config, EditAnywhere, Category="Chat",
    meta=(DisplayName="Default Chat Mode"))
EOliveChatMode DefaultChatMode = EOliveChatMode::Code;
```

That's it. One setting replaces 10.

---

## Edge Cases

### "Continue" in Plan Mode
User is in Plan mode. AI presents a plan. User says "continue" or "keep going." This is ambiguous -- does the user want more planning, or execution?

**Decision:** In Plan mode, "go"/"do it"/"execute"/"approved"/"build it" triggers execution. "Continue"/"keep going" means "continue planning/researching." The distinction is: action verbs = execute, continuation verbs = keep planning.

### Mode Switch Mid-Run
User is in Code mode, AI is actively building. User types `/plan`.

**Decision:** Mode switch is deferred until the current run completes. A system message appears: "Mode will switch to Plan after the current operation finishes." This is the same pattern as the current deferred focus profile switch.

### Plan Mode Execution Returns to Plan
After the user approves a plan and it executes (temporary Code mode), the system returns to Plan mode. But what if the execution fails partway? What if the user cancels?

**Decision:** Mode revert is tied to the run lifecycle, not the outcome. The `Active → Idle` and `Cancelling → Idle` transitions are the ONLY place mode reverts. This means:
- Run completes successfully → revert to Plan
- Run fails or partial success → revert to Plan
- User cancels mid-execution → `Active → Cancelling → Idle` → revert to Plan
- Self-correction loop runs → stays in temporary Code (still Active) → eventually Idle → revert

**Implementation:** `FOliveConversationManager` stores `PreviousMode` when entering a temporary Code execution. The brain layer's `TransitionToIdle()` (called from both completion and cancellation paths) checks `PreviousMode.IsSet()` and restores it. The mode field on `ConversationManager` is the source of truth — the brain layer never stores mode directly.

**What must NOT happen:** The cancel path must not skip the revert. If the user hits Stop during Plan→Code execution, the `Cancelling → Idle` transition must still call `RestorePreviousMode()`. This is a single call site — both `OnRunCompleted()` and `OnRunCancelled()` go through `TransitionToIdle()`.

### Ask Mode with @-mention
User is in Ask mode and @-mentions BP_Gun. The AI reads it and explains.

**Decision:** @-mention context injection works the same in all modes. The AI gets asset state summaries regardless of mode.

### External MCP Agent Mode Interaction
An external Claude Code CLI connects via MCP. What mode are they in?

**Decision:** External MCP agents are always in Code mode. They manage their own permission model (Claude Code has its own allowlist). The in-engine mode setting only affects the in-engine chat path. MCP tool calls continue to set `bFromMCP = true` and bypass Stage 2 entirely, as they do today.

---

## Token Budget Impact

### Tokens Saved
- Focus profile prompt additions: ~100-200 tokens (varies by profile)
- Safety preset context: ~50 tokens
- Confirmation tier descriptions in system prompt: ~100 tokens
- Tool pack visibility descriptions: ~50-100 tokens
- Total saved: ~300-450 tokens

### Tokens Added
- Mode suffix: ~50 tokens
- Net savings: ~250-400 tokens per turn

This is a meaningful saving. At ~4 chars/token, 300 tokens is 1200 characters of system prompt that the AI no longer has to process.

---

## Verification

### Test Prompts

1. **Code mode, simple task:** "Create a Blueprint called BP_HealthPickup that restores 25 health on overlap."
   - Expected: AI creates BP, adds overlap event, wires heal logic, compiles. No pauses for confirmation.

2. **Code mode, ambiguous task:** "Make me a gun."
   - Expected: AI researches templates, decides on asset decomposition, builds BP_Gun with fire logic, possibly BP_Bullet.

3. **Plan mode, complex task:** "Design a complete inventory system with pickup, drop, and UI."
   - Expected: AI reads project, searches templates, presents a multi-Blueprint plan. Does NOT execute any writes.

4. **Plan mode, approval:** User says "go" after seeing the plan.
   - Expected: System switches to temporary Code mode, executes the plan, returns to Plan mode.

5. **Ask mode, read query:** "What components does BP_Enemy have?"
   - Expected: AI calls blueprint.read, explains the component hierarchy. No writes attempted.

6. **Mode switch mid-run:** Start in Code mode, AI is building. Type `/plan`.
   - Expected: System message says mode will switch after current run. Run completes. Mode switches to Plan.

7. **Code mode, deletion:** AI decides to delete an old Blueprint.
   - Expected: Prompt appears asking for confirmation. User can allow or deny.

8. **Plan mode, write attempt:** AI in Plan mode tries to call blueprint.create.
   - Expected: Tool returns PLAN_MODE error. AI adjusts and presents the plan instead.

### What Success Looks Like in Logs

```
[OliveBrain] State: Idle -> Active (mode: Code)
[OliveWritePipeline] Stage 2: Mode gate PASS (Code mode, tool: blueprint.create)
[OliveWritePipeline] Stage 2: Mode gate PASS (Code mode, tool: blueprint.apply_plan_json)
[OliveBrain] State: Active -> Idle (outcome: Completed)
```

vs. Plan mode:
```
[OliveBrain] State: Idle -> Active (mode: Plan)
[OliveWritePipeline] Stage 2: Mode gate BLOCKED (Plan mode, tool: blueprint.create)
[OliveBrain] State: Active -> Idle (outcome: Completed, plan presented)
```

### What Regression Looks Like

- The AI asks "should I create this Blueprint?" in Code mode (confirmation leak from old code).
- Write tools succeed in Ask mode (mode gate not wired).
- The AI can't see write tool schemas in Plan mode and makes vague plans (tool list filtering wrong).
- Focus profile dropdown still appears in UI (incomplete cleanup).
- Settings migration doesn't run and users have stale confirmation tier values.

---

## Implementation Sequence

### Phase 1: Core Mode Infrastructure (1-2 days)
1. Add `EOliveChatMode` enum
2. Add mode storage to `FOliveConversationManager`
3. Modify `FOliveWritePipeline::StageConfirm()` to be mode-based gate
4. Add mode to `UOliveAISettings` (single `DefaultChatMode` property)
5. Wire mode-aware tool list filtering in `GetAvailableTools()`

### Phase 2: UI (1 day)
1. Add mode badge widget to `SOliveAIChatPanel` input area
2. Implement slash command parsing in input field
3. Remove focus profile dropdown and safety preset toggle
4. Add mode transition system messages
5. Add Ctrl+Shift+M cycling

### Phase 3: Removal Cleanup (1-2 days)
1. Remove confirmation tier enums, settings, and pipeline code
2. Remove focus profile manager, all profile-related code
3. Remove tool pack manager (already deprecated)
4. Remove plan-first routing enforcement
5. Simplify brain layer states to Idle/Active/Cancelling
6. Settings migration for existing users

### Phase 4: Autonomous Mode Integration (1 day)
1. Pass mode to MCP server state
2. Wire mode check into MCP tool dispatch
3. Update autonomous sandbox AGENTS.md to include mode context
4. Test Plan mode with autonomous CLI path

### Phase 5: Subagent Foundation (deferred)
1. `FOliveSubagentRunner` class with filtered tool list
2. Research subagent integration point in brain layer
3. Validator subagent trigger after write batches
4. UI cards for subagent results

---

## Open Questions

1. **Should `blueprint.preview_plan_json` be allowed in Ask mode?** It's read-only but implies the user is heading toward execution. Current answer: yes, allow it. Ask mode is "no mutations" and preview doesn't mutate.

2. **Should we show a token/cost estimate before executing a Plan mode approval?** Claude Code doesn't. Cursor doesn't. Probably unnecessary friction. But some users may want to know "executing this plan will take approximately 40 tool calls." Defer to user feedback.

3. **Should mode persist across editor sessions?** Current answer: yes, via `DefaultChatMode` setting. The setting is the default for new conversations. Within a conversation, mode is session-scoped and can change via slash commands.

4. **What about the `bPlanJsonRequirePreviewForApply` setting?** This is an execution safety mechanism, not a permission mechanism. Keep it. The AI still previews before applying plan_json regardless of mode.
