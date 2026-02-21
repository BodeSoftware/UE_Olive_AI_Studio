# Phase E — Brain Layer Design

> **Status:** Design Complete — Ready for Implementation  
> **Depends on:** Phase A (Control/Safety), Phase B (Chat UX/Context), Phase C (Provider Matrix), Phase D (Focus Profiles)  
> **Module:** `Source/OliveAIEditor/` — Brain/ subdirectory (NOT a separate UE module)  
> **Estimated effort:** 2–3 weeks (Phase E core) + optional Phase E2

---

## 1. Purpose

The Brain Layer sits between the conversation/MCP interfaces and the tool execution layer. It owns the agentic loop — the plan→delegate→execute→check→fix→repeat cycle that makes the plugin feel like Claude Code rather than a one-shot chat assistant.

Before Phase E, the Conversation Manager directly dispatches tool calls to the tool layer and echoes raw tool JSON back into the model context. After Phase E, the Brain intercepts that flow and adds: origin tracking, state management, confirmation coordination, operation history + prompt distillation, tool packs (schema trimming), retry/loop detection, and token budget management.

**Phase E core does not require orchestrator/workers.** Orchestrator/workers are a Phase E2 upgrade (only build if metrics justify it).

### Design Philosophy

**Single-loop execution, bounded prompts.** The default flow is one agentic loop where the Brain:
- Keeps the model context bounded via the Prompt Distillation Contract (Section 4.4)
- Reduces per-call schema tokens via Tool Packs (Section 7.0)
- Enforces safe writes through confirmation tokens owned by the write pipeline (Section 4.3)
- Prioritizes reliability: write → compile → verify → self-correct (Section 10)

**Phase E2 (optional): Orchestrator + Workers.** For cross-domain or very large tasks, we can add a planning call (no tools) and domain-scoped worker calls with clean contexts. This is an optimization, not a Phase E core dependency.

This gives us three wins in Phase E core:
1. **Token savings (history)** — Tool results stop "snowballing" by being summarized instead of re-fed as unbounded JSON.
2. **Token savings (schemas)** — Per call, the model only sees the tool pack it needs, not every tool in the profile.
3. **Quality** — Verification and correction loops converge more often and fail more clearly when they can't.

### What the Brain Does NOT Do

- Does NOT maintain a DAG of operation dependencies
- Does NOT estimate costs before execution
- Does NOT replace Focus Profiles (those are user-facing, Phase D)
- Does NOT require models to have native subagent support

---

## 2. Architecture

### 2.1 Where the Brain Fits

```
User Message / MCP Request
        │
        ▼
┌─────────────────────────┐
│  Conversation Manager   │  ← still owns session state, message history, streaming
│  (becomes a façade)     │
└──────────┬──────────────┘
           │
           ▼
┌──────────────────────────────────────────────────────┐
│                    Brain Layer                         │
│                                                        │
│  ┌──────────────────┐    ┌─────────────────────────┐  │
│  │ Heuristic Planner│    │ Operation History Store  │  │
│  │ (intent → policy)│    │ (logs + summarization)   │  │
│  └────────┬─────────┘    └─────────────────────────┘  │
│           │                                            │
│           ▼                                            │
│  ┌──────────────────┐                                  │
│  ┌──────────────────────────────────────────┐        │
│  │         Retry / Loop Detection           │        │
│  ├──────────────────────────────────────────┤        │
│  │         Context Cache                    │        │
│  ├──────────────────────────────────────────┤        │
│  │         State Machine                    │        │
│  └──────────────────────────────────────────┘        │
└──────────────────┬───────────────────────────────────┘
                   │
                   ▼
┌─────────────────────────┐
│  Confirmation Manager   │  ← routes through tiers (Phase A)
└──────────┬──────────────┘
           │
           ▼
┌─────────────────────────┐
│  Tool Router            │  ← dispatches to BP/BT/PCG/C++ handlers
└─────────────────────────┘
```

**Note:** Orchestrator/Workers are Phase E2 (optional). Phase E core uses the single-loop Brain plus tool packs and distilled history.

### 2.2 (Phase E2) Orchestrator vs Worker — What Each Does

**Orchestrator Agent:**
- Receives the user's message + project context + operation history summary
- Has NO tools — it cannot execute anything
- Returns a structured plan: a list of steps, each tagged with a domain
- Example output:
```json
{
    "steps": [
        {
            "description": "Create BP_Enemy Blueprint with Health variable, TakeDamage function, and death logic",
            "domain": "blueprint",
            "context_needed": []
        },
        {
            "description": "Create BT_EnemyAI Behavior Tree with patrol sequence and chase behavior",
            "domain": "behaviortree",
            "context_needed": ["BP_Enemy class interface"]
        },
        {
            "description": "Wire BP_Enemy to use BT_EnemyAI via AIController and Blackboard setup",
            "domain": "blueprint+behaviortree",
            "context_needed": ["BP_Enemy", "BT_EnemyAI"]
        }
    ]
}
```

**Worker Agent:**
- Receives a focused task description + domain-specific system prompt + domain-specific tools only
- Also receives any context from previous steps (summaries, not raw JSON)
- Runs its own agentic loop: call tools → check results → self-correct → compile
- Returns a structured result summary to the Brain
- Has NO knowledge of other workers or the overall plan

**The Brain (C++ code):**
- Calls the orchestrator to get the plan
- For each step, picks the right worker configuration and makes a fresh API call
- Passes context between steps via operation history summaries
- Manages state, retries, cancellation, confirmations
- Reports progress to the UI via the operation feed

### 2.3 How This Works With Any Model

The orchestrator and workers are just API calls. Here's what each call looks like:

**Orchestrator call:**
```
POST /v1/chat/completions (or /v1/messages)
{
    system: "You are a planning agent for an Unreal Engine development assistant...",
    messages: [{ role: "user", content: "Make an enemy that patrols and chases the player" }],
    tools: []  ← NO TOOLS
}
```

**Worker call:**
```
POST /v1/chat/completions (or /v1/messages)
{
    system: "You are a Blueprint specialist for Unreal Engine 5.5...",
    messages: [{ role: "user", content: "Create BP_Enemy with Health variable and TakeDamage function..." }],
    tools: [ blueprint.create, blueprint.add_variable, blueprint.add_function, ... ]  ← ONLY BP TOOLS
}
```

Any model that supports tool calling works. Claude, GPT-4, Gemini, Llama, Mistral, Ollama local models — all of them. No native "subagent protocol" needed. Our Brain C++ code is the orchestrator runtime, not the model.

### 2.4 When to Use Orchestrator vs Direct Execution

Not every request needs the full orchestrator flow. The heuristic planner decides:

| Request Type | Flow | Why |
|-------------|------|-----|
| "Explain this Blueprint" | Direct single call | No tools needed, just text response |
| "Add a Health variable to BP_Player" | Direct worker call | Single domain, simple task |
| "Create BP_HealthPickup with overlap healing" | Single BP worker with agentic loop | Single domain, multi-step |
| "Make an enemy that patrols" | Orchestrator → BP worker → BT worker → integration | Cross-domain task |
| "Refactor the damage system across all enemies" | Orchestrator → multiple BP workers | Multi-asset, needs planning |

The rule: **if the heuristic planner detects a single domain, skip the orchestrator and go straight to a worker. Use the orchestrator only for cross-domain or multi-asset tasks.**

This means simple requests (the majority) have zero overhead from the orchestrator layer.

### 2.5 Conversation Manager Refactoring

The Conversation Manager currently does too much. After Phase E:

**Conversation Manager keeps:**
- Session state (message history, conversation ID)
- API call mechanics (streaming, SSE parsing)
- Provider abstraction (OpenRouter client)
- UI event forwarding (stream chunks → chat panel)

**Brain Layer takes over:**
- Deciding whether to use orchestrator vs direct worker
- Tool call orchestration within workers
- Prompt assembly (system prompt + context + history + messages)
- Worker lifecycle management
- Operation sequencing and state tracking
- Confirmation flow coordination

The Conversation Manager calls `Brain.ProcessUserMessage()` instead of directly dispatching to the API. The Brain calls back to the Conversation Manager when it needs to make API calls (orchestrator call, worker calls).

---

## 3. Agent Definitions

### 3.1 Orchestrator Agent

The orchestrator has no tools. It only plans.

**System prompt core (abbreviated — full prompt is a Phase E deliverable):**
```
You are the planning agent for an Unreal Engine AI development assistant.

Your job is to break the user's request into concrete steps, each tagged with a domain.

Available domains:
- "blueprint" — Creating/modifying Blueprints, variables, functions, components, event graphs
- "behaviortree" — Creating/modifying Behavior Trees, tasks, decorators, services, blackboards
- "pcg" — Creating/modifying PCG graphs, density rules, scatter volumes
- "cpp" — Creating/modifying C++ classes, headers, UCLASS/UPROPERTY reflection
- "blueprint+behaviortree" — Cross-domain wiring (e.g., connecting a BP to a BT)

For each step, specify:
- description: What the worker should accomplish (be specific, include names and types)
- domain: Which worker handles this step
- context_needed: What information from previous steps this step needs

Respond ONLY with the JSON plan. Do not include explanations.
```

**Token cost per call:** ~1,500 tokens (system prompt) + user message + history summary. No tool schemas. Cheap.

**Model selection:** Uses the same model the user has configured. Could optionally use a cheaper model (e.g., Haiku-class) since planning doesn't need the strongest reasoning, but this is a settings-level decision, not architecture.

### 3.2 Worker Agent Configurations

Each worker gets a domain-specific system prompt and only the tools for its domain.

```cpp
USTRUCT()
struct FOliveWorkerConfig
{
    FName Domain;                          // "blueprint", "behaviortree", "pcg", "cpp"
    FString SystemPromptPath;              // Content/SystemPrompts/Worker_{Domain}.txt
    TArray<FString> AllowedToolPrefixes;   // e.g., {"blueprint."} or {"blueprint.", "behaviortree."}
    int32 MaxToolCallsPerTask = 30;        // safety cap per worker invocation
    bool bAutoCompileAfterWrites = true;
};
```

**Predefined worker configs:**

| Worker | Tool Prefixes | Approx Schema Tokens | Notes |
|--------|--------------|---------------------|-------|
| Blueprint | `blueprint.*` | ~3,000 | Most common, handles BP creation/modification |
| BehaviorTree | `behaviortree.*` | ~2,500 | BT creation, task/decorator/service setup |
| PCG | `pcg.*` | ~2,000 | PCG graph creation, density/scatter rules |
| C++ | `cpp.*` | ~2,500 | Header/source generation, UCLASS scaffolding |
| Blueprint+BT | `blueprint.*` + `behaviortree.*` | ~5,500 | Cross-domain wiring steps |
| All | `*` | ~12,000+ | Fallback when domain is unclear |

**Compare:** Sending all tools every call = ~12,000+ schema tokens × 10 calls = 120,000 tokens. With domain-scoped workers = ~3,000 × 10 calls = 30,000 tokens. **75% reduction in schema token cost.**

### 3.3 Worker System Prompts

Each worker gets a focused system prompt. These are critical deliverables — prompt quality directly determines how well the AI uses tools.

**Common structure for all worker prompts:**

```
You are a {domain} specialist for Unreal Engine 5.5.

## Your Task
{task_description from orchestrator or user}

## Context From Previous Steps
{operation summaries from earlier workers, if any}

## Rules
- Read before write: always examine existing state before modifying
- Chain operations: complete the full task, don't stop after one tool call
- Compile after changes: trigger compilation when you've finished writing
- Report what you did: end with a clear summary of assets created/modified
- If something fails, try to fix it. If you can't fix it after 3 attempts, report the error.

## {Domain}-Specific Patterns
{domain-specific best practices, naming conventions, common patterns}
```

**Worker prompts are stored as text files** in `Content/SystemPrompts/`:
```
Content/SystemPrompts/
├── Orchestrator.txt
├── Worker_Blueprint.txt
├── Worker_BehaviorTree.txt
├── Worker_PCG.txt
├── Worker_Cpp.txt
└── Worker_Integration.txt    ← for cross-domain steps
```

This makes them editable without recompiling the plugin, and version-controllable.

### 3.4 Focus Profiles Refactor (Phase D Migration)

Phase D shipped with multiple Focus Profiles (Blueprint, AI & Behavior, Level & PCG, C++ Only, C++ & Blueprint, Everything, plus custom). For product UX, Phase E deliberately simplifies this to **exactly three user-selectable options**:

| Profile | What It Includes | When to Use |
|---------|-----------------|-------------|
| **Auto (default)** | Everything; Brain decides | Most users |
| **Blueprint** | "Editor work": Blueprint + Behavior Trees + Blackboards + PCG + Project/Cross-system tools | Anything you do in the editor outside of C++ code |
| **C++** | C++ tools only | Native code work |

This is a breaking change for users who relied on advanced profiles. The upside is a clearer product with fewer wrong-mode mistakes and simpler onboarding.

**Important rule:** Tool Packs (Section 7) layer on top of whichever profile is selected. Profiles remain the **upper bound** (permission boundary); packs are the **per-call subset** to reduce schema tokens.

**Migration mapping (required):**
- `Blueprint`, `AI & Behavior`, `Level & PCG` → `Blueprint`
- `C++ Only` → `C++`
- `C++ & Blueprint`, `Everything`, `Full Stack` (legacy) → `Auto`
- Any custom profile → `Auto`

**Implementation notes:**
1. Update `FOliveFocusProfileManager` to expose only these 3 profiles in `GetAllProfiles()` / UI list.
2. Keep name normalization (e.g., `Full Stack` → `Everything`) only if it is still needed for settings migration; user-facing list remains 3 options.
3. The `Blueprint` profile must include tool categories: `blueprint`, `behaviortree`, `blackboard`, `pcg`, `project`, `crosssystem`.
4. The `C++` profile maps to the existing "C++ Only" behavior (cpp tools only) even if the display name is just "C++".

### 3.5 Focus Profiles + Workers: How They Interact

**How Focus Profiles interact with Workers:**

- **Blueprint profile selected:**
  - BP/BT/BB/PCG tasks → direct tool use within the single-loop Brain (no orchestrator required for Phase E).
  - Cross-domain editor tasks (BP + BT/BB + PCG) → still allowed and expected; Brain stays within editor domains.
  - C++ task requested → Brain warns: "You're in Blueprint mode. Switch to Auto or C++ to work with code." Hard boundary — Blueprint mode never uses `cpp.*` tools.

- **C++ profile selected:**
  - C++ tasks → direct C++ tools
  - Editor graph task requested → Brain warns: switch to Auto or Blueprint
  - Hard boundary — C++ mode never uses Blueprint/BT/BB/PCG tools

- **Auto profile selected:**
  - Brain uses heuristic planner to detect domains
  - Uses the best domains/tools as needed (still constrained by tool packs per call)
  - No restrictions

**Why hard boundaries between Blueprints and C++:** These are genuinely different workflows with different tool sets and different system prompts. A user in C++ mode doesn't want the AI suddenly creating Blueprint assets, and vice versa. The boundary prevents surprising behavior. Auto mode exists for users who want the AI to handle everything.

**Worker domains within each profile:**

| Focus Profile | Available Worker Domains |
|--------------|------------------------|
| Blueprint | `blueprint`, `behaviortree`, `blackboard`, `pcg`, `project`, `crosssystem` |
| C++ | `cpp` |
| Auto | All domains |

---

## 4. Core Types

### 4.1 Tool Call Origin Tracking

**Problem:** Tool handlers currently set `Request.bFromMCP = true` unconditionally in Blueprint handlers, which prevents confirmation tier routing when the same handlers are called from Editor Chat.

**Solution:** Thread-local execution context with RAII scope.

```cpp
// ── OliveToolExecutionContext.h ──

UENUM()
enum class EOliveToolCallOrigin : uint8
{
    EditorChat,    // Built-in chat panel
    MCP            // External agent via MCP server
};

USTRUCT()
struct FOliveToolCallContext
{
    EOliveToolCallOrigin Origin;
    FString SessionId;       // Conversation or MCP session
    FString RunId;           // Unique per agentic run (user message → completion)
    FName ActiveFocusProfile;
    FName ActiveWorkerDomain; // Which worker is currently executing
    bool bRunModeActive;     // Heuristic planner set this
};

// Thread-local singleton
class FOliveToolExecutionContext
{
public:
    static const FOliveToolCallContext* Get();

private:
    friend class FOliveToolExecutionContextScope;
    static thread_local const FOliveToolCallContext* CurrentContext;
};

// RAII scope — set context for the duration of tool execution
class FOliveToolExecutionContextScope
{
public:
    FOliveToolExecutionContextScope(const FOliveToolCallContext& InContext);
    ~FOliveToolExecutionContextScope();

private:
    const FOliveToolCallContext* PreviousContext;
};
```

**Usage in MCP server:**
```cpp
FOliveToolCallContext Ctx;
Ctx.Origin = EOliveToolCallOrigin::MCP;
Ctx.SessionId = MCPSessionId;
FOliveToolExecutionContextScope Scope(Ctx);
ToolRouter->ExecuteTool(ToolName, Params);
```

**Usage in Brain (worker execution):**
```cpp
FOliveToolCallContext Ctx;
Ctx.Origin = EOliveToolCallOrigin::EditorChat;
Ctx.SessionId = ConversationId;
Ctx.RunId = CurrentRunId;
Ctx.ActiveFocusProfile = ActiveProfile;
Ctx.ActiveWorkerDomain = WorkerConfig.Domain;
Ctx.bRunModeActive = true;
FOliveToolExecutionContextScope Scope(Ctx);
ToolRouter->ExecuteTool(ToolName, Params);
```

**Fix in tool handlers:**
```cpp
// BEFORE (broken):
Request.bFromMCP = true;

// AFTER (fixed):
const FOliveToolCallContext* Ctx = FOliveToolExecutionContext::Get();
Request.bFromMCP = (Ctx && Ctx->Origin == EOliveToolCallOrigin::MCP);
```

### 4.2 Brain State Machine

```cpp
UENUM()
enum class EOliveBrainState : uint8
{
    Idle,                  // No active operation
    Planning,              // Orchestrator is generating a plan
    WorkerActive,          // A worker agent is executing (tool calls in progress)
    AwaitingConfirmation,  // Tier 2/3 — waiting for user approval
    Cancelling,            // User hit Stop — draining in-flight ops
    Completed,             // Run finished successfully
    Error                  // Run failed (after exhausting retries)
};
```

**State transitions:**

```
Idle → Planning                    (user sends complex/cross-domain message)
Idle → WorkerActive                (user sends single-domain message, skip orchestrator)
Planning → WorkerActive            (orchestrator returned plan, Brain dispatches first step)
WorkerActive → WorkerActive        (current worker done, next step begins)
WorkerActive → AwaitingConfirmation (worker's tool needs Tier 2/3 confirmation)
AwaitingConfirmation → WorkerActive (user approved)
AwaitingConfirmation → Idle        (user cancelled)
WorkerActive → Completed           (all steps done)
WorkerActive → Error               (retries exhausted, loop detected)
Any → Cancelling                   (user hits Stop)
Cancelling → Idle                  (in-flight ops drained, summary generated)
Completed → Idle                   (reset for next message)
Error → Idle                       (reset after error reported)
```

**Sub-states within WorkerActive (tracked but not top-level):**
```cpp
UENUM()
enum class EOliveWorkerPhase : uint8
{
    Streaming,         // Receiving model response
    ExecutingTools,    // Running tool calls
    Compiling,         // Auto-compile in progress
    SelfCorrecting,    // Feeding errors back to model
    Complete           // Worker finished its task
};
```

### 4.3 Confirmation Token Re-entry

When a worker's tool call needs confirmation:

1. Tool handler returns `requires_confirmation` + token + plan
2. Brain pauses the worker (transitions to `AwaitingConfirmation`)
3. User reviews and approves/cancels
4. Brain resumes the worker with the confirmation token
5. Worker continues its agentic loop

**Single source of truth (required):**
- Confirmation tokens are issued and validated by the existing systems that already do this:
  - `FOliveWritePipeline` (Blueprint writes)
  - `FOliveSnapshotManager` (project.rollback)
- The Brain does **not** generate, expire, or validate tokens. The Brain only stores pending UI state (tool name, args, plan text) and replays the tool call with the `confirmation_token` that the pipeline returned.

**Schema addition for writer tools:**
```json
{
    "confirmation_token": {
        "type": "string",
        "description": "Optional. If provided, skips confirmation and executes the previously approved plan."
    }
}
```

### 4.4 Operation History Store

Per-session log of all tool calls across all workers. Critical for passing context between workers and for prompt summarization.

```cpp
USTRUCT()
struct FOliveOperationRecord
{
    int32 Sequence;              // monotonic counter
    FString RunId;               // groups ops within a single user request
    FString WorkerDomain;        // which worker executed this
    int32 StepIndex;             // which step in the orchestrator's plan (0 if no orchestrator)
    FString ToolName;            // e.g. "blueprint.add_node"
    TSharedPtr<FJsonObject> Params;
    TSharedPtr<FJsonObject> Result;
    EOliveOperationStatus Status;
    FString ErrorMessage;
    int32 ConfirmationTier;
    FDateTime Timestamp;
    TArray<FString> AffectedAssets;
};

UENUM()
enum class EOliveOperationStatus : uint8
{
    Success,
    Failed,
    Skipped,
    Cancelled,
    RequiresConfirmation
};

class FOliveOperationHistoryStore
{
public:
    void RecordOperation(const FOliveOperationRecord& Record);

    // Build summary for orchestrator (what happened so far in this run)
    FString BuildOrchestratorSummary(const FString& RunId) const;

    // Build summary for a worker (what previous workers accomplished)
    // This is what gets passed as context to the next worker
    FString BuildWorkerContext(const FString& RunId, int32 UpToStep) const;

    // Build compressed summary for the user's conversation history
    FString BuildPromptSummary(int32 TokenBudget) const;

    // Query operations
    TArray<FOliveOperationRecord> GetRunHistory(const FString& RunId) const;
    TArray<FOliveOperationRecord> GetStepHistory(const FString& RunId, int32 StepIndex) const;

    // Partial success stats
    void GetRunStats(const FString& RunId,
        int32& OutSucceeded, int32& OutFailed, int32& OutSkipped) const;

    void Clear();

private:
    TArray<FOliveOperationRecord> Records;
    FString SessionId;
};
```

#### Prompt Distillation Contract (Token Efficiency Core)

This is the primary token-saving mechanism in Phase E.

Rules:
1. The plugin always stores **full** tool params/results for UI/history/debug (local only).
2. When sending conversation context back to the model, the Brain must **distill** older tool results:
   - Keep the **last 2 tool call/result pairs** in "raw" form *unless they exceed a size cap*.
   - Distill everything older into compact summaries (one to a few lines each).
3. Size cap (hard rule):
   - If a single tool result exceeds the cap (by chars or estimated tokens), it must be summarized even if it is recent.
   - The summary must include the minimal fields needed to continue (asset path, created/modified item IDs, compile status, top errors).
4. The model must never receive "unbounded JSON" history. If more detail is needed, the model should re-call read tools.

This is concrete and testable: given a long run, the prompt should stay bounded and older operations should collapse to summaries.

**Inter-worker context example:**

Step 1 (BP worker) created `BP_Enemy`. Step 2 (BT worker) needs to know what's in BP_Enemy but doesn't need the raw tool JSON. `BuildWorkerContext(RunId, 1)` produces:

```
Previous steps completed:
- Created BP_Enemy (parent: ACharacter) at /Game/Characters/BP_Enemy
  - Variables: Health (float, default=100), bIsDead (bool, default=false)
  - Functions: TakeDamage (float DamageAmount → void), Die (void → void)
  - Components: CapsuleComponent, SkeletalMesh, AIPerceptionStimuliSource
```

~100 tokens instead of ~2,000+ tokens of raw tool call JSON.

**Prompt summarization levels** (for conversation history):

- **Large budget (>2000 tokens):** Per-operation summary with params and results
- **Medium budget (500–2000 tokens):** Grouped by asset with outcome summary
- **Small budget (<500 tokens):** One-line session summary

### 4.5 Retry Policy & Loop Detection

```cpp
USTRUCT()
struct FOliveRetryPolicy
{
    int32 MaxRetriesPerError = 3;          // same error within one worker
    int32 MaxCorrectionCyclesPerWorker = 5; // total retry budget per worker
    int32 MaxWorkerFailures = 2;           // how many workers can fail before stopping the run
    float RetryDelaySeconds = 0.0f;
};

class FOliveLoopDetector
{
public:
    bool IsLooping(const FString& ErrorSignature, const FString& AttemptedFix) const;
    bool IsOscillating() const;
    void RecordAttempt(const FString& ErrorSignature, const FString& AttemptedFix);
    void Reset();  // called at start of each worker

private:
    TMap<FString, TArray<FString>> AttemptHistory;
    TArray<FString> RecentErrors;
};
```

**Loop detection is per-worker.** Each worker gets a fresh loop detector. If a worker loops, the Brain can retry the worker with a different prompt, skip that step, or stop the run.

#### Error Signature Definition (Must Be Explicit)

For loop detection, a "same failure" is defined by a stable signature string.

- Tool failures:
  - Signature: `{tool_name}:{error_code}:{asset_path}`
- Compile failures:
  - Signature: `{asset_path}:{top_compiler_error_code_or_hash}:{top_message_hash}`

Two identical signatures in a row with the same attempted fix = strong loop signal.

#### Loop Response (Product Quality Requirement)

When the Brain detects a loop, it must not just stop. It must:
1. Report what it tried (high level; no raw JSON dump).
2. Report the stable error signature + key error message.
3. Suggest a different next step:
   - run specific reads (e.g., `blueprint.read_function`, `cpp.read_class`) or
   - ask the user to choose between 2–3 remediation options.

### 4.6 Context Cache

```cpp
class FOliveContextCache
{
public:
    TSharedPtr<FJsonObject> GetCachedIR(const FString& AssetPath) const;
    void CacheIR(const FString& AssetPath, TSharedPtr<FJsonObject> IR);
    void Invalidate(const FString& AssetPath);
    void InvalidateAll();
    FString GetSummarizedIR(const FString& AssetPath, int32 TokenBudget) const;

private:
    struct FCacheEntry
    {
        TSharedPtr<FJsonObject> IR;
        FDateTime CachedAt;
        uint32 ContentHash;
    };

    TMap<FString, FCacheEntry> Cache;
    float TimeoutSeconds = 600.0f;

    void OnAssetChanged(const FAssetData& AssetData);
};
```

**Dual invalidation:** Asset registry events + post-write invalidation.

Cache is shared across all workers within a session. When worker 1 creates an asset, worker 2 can read it from cache.

---

## 5. Heuristic Planner

The heuristic planner classifies intent and sets the run policy. Its most important job is deciding **whether to use the orchestrator or go direct to a worker.**

### 5.1 Run Policy

```cpp
UENUM()
enum class EOliveRunMode : uint8
{
    Direct,         // Single API call, no tools (explanations, simple questions)
    SingleWorker,   // One worker with agentic loop (single-domain tasks)
    Orchestrated    // Orchestrator → multiple workers (cross-domain tasks)
};

USTRUCT()
struct FOliveRunPolicy
{
    EOliveRunMode RunMode = EOliveRunMode::Direct;
    FName PrimaryDomain;                   // for SingleWorker mode
    EOliveRiskLevel RiskLevel = EOliveRiskLevel::Low;
    int32 ExpectedConfirmationTier = 1;
    int32 MaxToolCalls = 50;               // total across all workers
    bool bAutoCompileAfterWrites = true;
};

UENUM()
enum class EOliveRiskLevel : uint8
{
    Low,      // reads, simple creates
    Medium,   // multi-step modifications
    High      // deletions, reparenting, bulk operations
};
```

### 5.2 Run Mode Classification

```cpp
// Step 1: Detect domains mentioned in the user's message
TSet<FName> DetectedDomains = DetectDomains(UserMessage);

// Step 2: Intersect with Focus Profile (user's choice is the upper bound)
TSet<FName> AllowedDomains = GetFocusProfileDomains(ActiveProfile);
TSet<FName> EffectiveDomains = DetectedDomains.Intersect(AllowedDomains);

// Step 3: Decide run mode
if (IsReadOnlyOrExplanation(UserMessage))
{
    Policy.RunMode = EOliveRunMode::Direct;
}
else if (EffectiveDomains.Num() <= 1)
{
    Policy.RunMode = EOliveRunMode::SingleWorker;
    Policy.PrimaryDomain = EffectiveDomains.Num() == 1
        ? EffectiveDomains.Array()[0]
        : FName("blueprint");  // default
}
else
{
    Policy.RunMode = EOliveRunMode::Orchestrated;
}
```

### 5.3 Intent Classification (Risk + Confirmation Tier)

```
// HIGH RISK
"delete|remove|destroy|drop|wipe|clean up"  → RiskLevel::High, Tier 3
"reparent|change parent"                     → RiskLevel::High, Tier 3
"refactor|restructure|reorganize"            → RiskLevel::High, Tier 3
"bulk|all blueprints|every|across project"   → RiskLevel::High, Tier 2

// MEDIUM RISK
"create .* with|build .* that|make .* which" → RiskLevel::Medium, Tier 2
"add .* function|implement|wire up"          → RiskLevel::Medium, Tier 2
"fix|debug|solve|repair"                     → RiskLevel::Medium, Tier 1
"modify|change|update|edit"                  → RiskLevel::Medium, Tier 2

// LOW RISK
"explain|describe|what does|how does|read"   → RiskLevel::Low, Tier 1
"list|show|find|search"                      → RiskLevel::Low, Tier 1
"add variable|add component|rename"          → RiskLevel::Low, Tier 1
```

### 5.4 Domain Detection

```
"blueprint|BP_|graph|node|pin|variable|component|function" → "blueprint"
"behavior tree|BT_|task|decorator|service|blackboard|BB_"  → "behaviortree"
"pcg|procedural|scatter|density|point"                      → "pcg"
"c++|cpp|header|source|UCLASS|UPROPERTY|reflect"           → "cpp"
"enemy|patrol|AI|combat|chase|wander"                       → "behaviortree" + "blueprint"
"pickup|collectable|overlap|trigger"                        → "blueprint"
```

---

## 6. Agentic Loop

### 6.1 Top-Level Flow

```
User sends message
    │
    ▼
[Heuristic Planner]  → classify intent, detect domains, set RunPolicy
    │
    ├── RunMode::Direct ──────────▶ [Single API Call] → response to user. Done.
    │
    ├── RunMode::SingleWorker ────▶ [Spawn Worker] → worker runs agentic loop
    │                                  │               → results to user. Done.
    │
    └── RunMode::Orchestrated ────▶ [Call Orchestrator]
                                       │
                                       ▼
                                   [Parse Plan] → list of steps with domains
                                       │
                                       ▼
                                   [For each step:]
                                       │
                                       ├── Build worker context from previous steps
                                       ├── Select worker config by domain
                                       ├── Spawn worker with:
                                       │     - domain-specific system prompt
                                       │     - domain-scoped tools
                                       │     - task description from orchestrator
                                       │     - context from previous steps
                                       │
                                       ├── Worker runs its agentic loop:
                                       │     tool calls → compile → self-correct → done
                                       │
                                       ├── Record results in Operation History
                                       │
                                       ├── If worker failed:
                                       │     retry worker? skip step? stop run?
                                       │
                                       └── Next step
                                       │
                                       ▼
                                   [All steps done]
                                       │
                                       ▼
                                   [Generate run report] → partial success summary
                                       │
                                       ▼
                                   [Response to user] with summary of what was created/modified
```

### 6.2 Worker Agentic Loop (Internal)

Each worker runs its own loop:

```
Worker receives task + context + tools
    │
    ▼
[Prompt Assembly]  → worker system prompt + task + context + tool schemas
    │
    ▼
[API Call]         → send to model
    │
    ▼
[Parse Response]   → text → stream to UI (via Brain)
    │                tool_use → execute
    │
    ├── No tool calls → Worker complete. Return summary.
    │
    └── Has tool calls ──▶ [Execute Tools]
                              │
                              ├── Set FOliveToolExecutionContextScope
                              ├── Check confirmation tier
                              ├── Execute via Tool Router
                              ├── Record in Operation History
                              ├── Invalidate context cache
                              │
                              ▼
                          [Post-Execution Check]
                              │
                              ├── Auto-compile if writes occurred
                              │
                              ├── No errors → assemble results, loop back to [API Call]
                              │
                              └── Errors → check LoopDetector
                                    ├── OK → feed errors back, loop
                                    └── STOP → worker failed, return error to Brain
```

### 6.3 Cancellation Flow

When the user hits "Stop":

1. Brain transitions to `Cancelling`
2. If orchestrator is running → abort, no further steps
3. If a worker is active → let current tool call finish, stop the worker's loop
4. Skip all remaining steps in the plan
5. Generate partial success report from Operation History
6. Report to user: what completed, what was skipped
7. Transition to `Idle`

### 6.4 Partial Success Tracking

```cpp
struct FOliveRunReport
{
    FString RunId;
    EOliveRunOutcome Outcome;  // Completed, PartialSuccess, Failed, Cancelled
    int32 TotalSteps;          // from orchestrator plan (or 1 for single-worker)
    int32 CompletedSteps;
    int32 FailedSteps;
    int32 SkippedSteps;
    TArray<FOliveStepSummary> StepSummaries;
    FString UserGuidance;
};

struct FOliveStepSummary
{
    int32 StepIndex;
    FName WorkerDomain;
    FString Description;
    EOliveRunOutcome Outcome;
    int32 ToolCallsExecuted;
    TArray<FString> AssetsCreated;
    TArray<FString> AssetsModified;
    FString ErrorSummary;
};
```

---

## 7. Token Budget Management

### 7.0 Tool Packs (Schema Token Reduction Core)

Prompt distillation reduces accumulation. Tool Packs reduce the **flat schema cost per call**.

**Definition:** A Tool Pack is a curated subset of tool definitions that the model sees for a given call.

Rules:
1. Start from the selected Focus Profile tool list (upper bound).
2. Intersect with the current Tool Pack membership list (per-call subset).
3. Escalate packs only when needed (read → basic write → graph write → danger).

Suggested packs:
- `read_pack` (safe reads, high-value context gathering)
- `write_pack_basic` (Tier 1 simple writes)
- `write_pack_graph` (Tier 2 graph editing)
- `danger_pack` (Tier 3 destructive/refactor/delete/bulk)

**Pack membership must be explicit tool names**, not vague descriptions.

**Config-driven (recommended):** define pack membership in a config file so packs can evolve as tools are added without requiring constant code edits.

Example file (proposed): `Config/OliveToolPacks.json`
```json
{
  "read_pack": [
    "project.search", "project.get_info", "project.get_dependencies", "project.get_referencers",
    "blueprint.read", "blueprint.read_function", "blueprint.read_event_graph",
    "behaviortree.read", "blackboard.read",
    "pcg.read",
    "cpp.read_class", "cpp.read_header", "cpp.read_source"
  ],
  "write_pack_basic": [
    "blueprint.create", "blueprint.add_variable", "blueprint.add_component",
    "blueprint.add_custom_event", "blueprint.add_event_dispatcher",
    "behaviortree.set_blackboard",
    "blackboard.add_key", "blackboard.remove_key",
    "pcg.create",
    "cpp.create_class", "cpp.add_property", "cpp.add_function"
  ],
  "write_pack_graph": [
    "blueprint.add_node", "blueprint.remove_node", "blueprint.connect_pins", "blueprint.disconnect_pins",
    "blueprint.set_pin_default", "blueprint.set_node_property"
  ],
  "danger_pack": [
    "blueprint.delete", "blueprint.set_parent_class",
    "project.refactor_rename", "project.rollback", "project.move_to_cpp"
  ]
}
```

Notes:
- The exact membership will be adjusted over time, but it must be written down and versioned.
- Packs apply to both Primary and Advanced Focus Profiles.

### 7.1 Orchestrator Budget

The orchestrator is cheap — no tool schemas:

```
Orchestrator Context:
├── System prompt:          ~1,500 tokens
├── Project context:        ~500 tokens
├── User message:           variable
├── Session history:        up to 2,000 tokens (BuildPromptSummary)
└── Response budget:        ~2,000 tokens
Total per orchestrator call: ~6,000-8,000 tokens
```

### 7.2 Worker Budget

Workers are scoped — domain tools only:

```
Worker Context:
├── System prompt:          ~1,500 tokens (domain-specific)
├── Task description:       ~200-500 tokens (from orchestrator)
├── Context from prev steps: ~200-1,000 tokens (BuildWorkerContext)
├── Tool schemas:           ~2,000-3,500 tokens (domain-scoped)
├── Worker conversation:    remaining budget (tool calls + results within this worker)
└── Response budget:        ~4,096 tokens
Total per worker start:     ~5,000-7,000 tokens
```

### 7.3 Token Savings Example

**Task:** "Make an enemy that patrols and chases the player"

**Old design (single loop, all tools):**
```
Per API call: ~3,000 system + ~12,000 schemas + conversation = ~15,000+ base
10 calls × 15,000 base = 150,000 tokens minimum (just schemas + system prompt)
Plus tool results accumulating in conversation = easily 200,000+ total
```

**New design (orchestrator + workers):**
```
1 orchestrator call:    ~6,000 tokens
5 BP worker calls:      ~5,000 base each = 25,000 (BP results stay in BP context)
4 BT worker calls:      ~5,000 base each = 20,000 (BT results stay in BT context)
1 integration call:     ~7,000 base = 7,000
Total: ~58,000 tokens
```

**~70% reduction** in total token usage for cross-domain tasks.

### 7.4 Prompt Assembly

```cpp
class FOlivePromptAssembler
{
public:
    // Assemble prompt for orchestrator call
    FConversationContext AssembleOrchestratorPrompt(
        const FString& UserMessage,
        const FOliveOperationHistoryStore& History,
        const TArray<FChatMessage>& ConversationHistory,
        int32 ModelContextWindow
    ) const;

    // Assemble prompt for worker call
    FConversationContext AssembleWorkerPrompt(
        const FOliveWorkerConfig& Config,
        const FString& TaskDescription,
        const FString& PreviousStepContext,
        const FOliveContextCache& Cache,
        int32 ModelContextWindow
    ) const;

    // Assemble prompt for direct single call (no tools)
    FConversationContext AssembleDirectPrompt(
        const FString& UserMessage,
        const TArray<FChatMessage>& ConversationHistory,
        int32 ModelContextWindow
    ) const;

private:
    static constexpr int32 ResponseBudget = 4096;
};
```

### 7.5 Truncation Priority (Within Workers)

When a worker's conversation exceeds its context window:

1. Compress earlier tool results to summaries (keep most recent 2 turns verbatim)
2. Summarize the task description if very long
3. **NEVER truncate tool schemas**
4. **NEVER truncate system prompt**

---

## 8. System Prompt Authoring

### 8.1 Deliverables

| File | Purpose | Priority |
|------|---------|----------|
| `Orchestrator.txt` | How to break tasks into domain steps | P0 |
| `Worker_Blueprint.txt` | BP-specific patterns, tool usage conventions | P0 |
| `Worker_BehaviorTree.txt` | BT-specific patterns | P1 |
| `Worker_PCG.txt` | PCG-specific patterns | P1 |
| `Worker_Cpp.txt` | C++ generation patterns | P1 |
| `Worker_Integration.txt` | Cross-domain wiring patterns | P1 |
| `Base.txt` | Common rules included in all prompts | P0 |

### 8.2 Iteration Process

1. Write initial prompts based on known UE patterns
2. Test with common requests
3. Observe: did the orchestrator split correctly? Did workers use tools efficiently?
4. Refine prompts based on failures
5. Track metrics: task completion rate, tool calls per task, self-correction frequency

### 8.3 Project Policy Injection

Users can define project-level rules appended to all system prompts:

```ini
# DefaultAIAgent.ini
[/Script/AIAgentEditor.OliveAISettings]
ProjectNamingConvention="All Blueprints must start with BP_, all Behavior Trees with BT_"
ProjectRules="Never create Blueprints in /Game/Root. Always use /Game/{Category}/{AssetName}"
```

---

## 9. Files & Integration Points

### 9.1 New Files

```
Source/OliveAIEditor/
├── Public/Brain/
│   ├── OliveBrainLayer.h              — main brain class, orchestrator + worker lifecycle
│   ├── OliveBrainState.h              — state machine enums + transitions
│   ├── OliveOrchestrator.h            — orchestrator agent logic + plan parsing
│   ├── OliveWorkerAgent.h             — worker agent logic + agentic loop
│   ├── OliveWorkerConfig.h            — worker configurations per domain
│   ├── OliveToolExecutionContext.h    — thread-local context + RAII scope
│   ├── OliveOperationHistory.h        — operation history store + summarization
│   ├── OliveRetryPolicy.h            — retry policy + loop detector
│   ├── OliveContextCache.h           — asset IR cache
│   ├── OliveHeuristicPlanner.h       — intent classification + run mode decision
│   ├── OlivePromptAssembler.h        — prompt assembly for orchestrator + workers
│   └── OliveRunReport.h              — partial success reporting
│
├── Private/Brain/
│   ├── OliveBrainLayer.cpp
│   ├── OliveOrchestrator.cpp
│   ├── OliveWorkerAgent.cpp
│   ├── OliveWorkerConfig.cpp
│   ├── OliveToolExecutionContext.cpp
│   ├── OliveOperationHistory.cpp
│   ├── OliveRetryPolicy.cpp
│   ├── OliveContextCache.cpp
│   ├── OliveHeuristicPlanner.cpp
│   ├── OlivePromptAssembler.cpp
│   └── OliveRunReport.cpp
│
Content/SystemPrompts/
├── Orchestrator.txt
├── Base.txt
├── Worker_Blueprint.txt
├── Worker_BehaviorTree.txt
├── Worker_PCG.txt
├── Worker_Cpp.txt
└── Worker_Integration.txt
```

### 9.2 Modified Files

| File | Change |
|------|--------|
| `Public/FocusProfiles/OliveFocusProfileManager.h` | Simplify to 3 profiles (Blueprints, C++, Auto). Remove custom profile API. Add `GetAllowedWorkerDomains()`. |
| `Private/FocusProfiles/OliveFocusProfileManager.cpp` | Update tool prefix mappings. Remove save/load/validation. Map old profiles in settings migration. |
| Focus Profile UI widget (Slate dropdown) | Change from dynamic list to 3 fixed options. Remove "create custom profile" UI. Default to Auto. |
| `Private/Chat/OliveConversationManager.cpp` | Refactor to façade. Route to `Brain.ProcessUserMessage()`. Keep: session state, streaming, API calls. |
| `Private/MCP/OliveMCPServer.cpp` | Wrap tool execution in `FOliveToolExecutionContextScope` with `Origin = MCP`. MCP bypasses Brain. |
| `Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Fix `bFromMCP` bug. Add `confirmation_token` to writer schemas. |
| `BehaviorTree/Private/MCP/OliveBTToolHandlers.cpp` | Same fix + `confirmation_token`. |
| `PCG/Private/MCP/OlivePCGToolHandlers.cpp` | Same fix + `confirmation_token`. |
| `Cpp/Private/MCP/OliveCppToolHandlers.cpp` | Same fix + `confirmation_token`. |
| `Private/Index/OliveProjectIndex.cpp` | Add `OnAssetChanged` delegate for cache invalidation. |

### 9.3 What's NOT Changed

- **Tool Router** — unchanged
- **Tool handlers** — unchanged except `bFromMCP` fix + `confirmation_token`
- **Confirmation Manager** — unchanged, Brain calls it
- **Focus Profile system** — unchanged, Brain respects it as upper bound
- **UI widgets** — operation feed from Phase B renders step-level progress from Brain events

---

## 10. Implementation Order

### Step 0: Focus Profile Simplification (1–2 days)

- Simplify user-selectable Focus Profiles to exactly 3: Auto (default), Blueprint, C++.
- Blueprint includes: blueprint + behaviortree + blackboard + pcg + project + crosssystem.
- C++ includes: cpp only.
- Add migration mapping for saved profile names (Section 3.4).
- **Test:** Dropdown shows only 3 options; old saved profiles map correctly.

### Step 1: Tool Execution Context (2–3 days)

- Implement `FOliveToolExecutionContext`, RAII scope
- Fix `bFromMCP` bug in all four handler files
- Add `confirmation_token` to writer schemas
- **Test:** Confirmation tiers route correctly by origin.

### Step 2: Brain State Machine + Shell (2–3 days)

- Implement `EOliveBrainState` with transitions
- Create `FOliveBrainLayer` shell class
- Wire state change events to operation feed UI
- **Test:** State transitions correct. Operation feed updates.

### Step 3: Operation History Store (3–4 days)

- Implement `FOliveOperationHistoryStore`
- Implement `BuildPromptSummary()`, `BuildWorkerContext()`, `BuildOrchestratorSummary()`
- Implement `FOliveRunReport` and `FOliveStepSummary`
- **Test:** Operations recorded per-worker/step. Context summaries correct.

### Step 4: Prompt Distillation Integration (2–3 days)

- Make the Brain enforce the Prompt Distillation Contract:
  - last 2 tool call/result pairs raw unless size-capped
  - older tool results summarized
- **Test:** Long runs do not grow prompts unbounded; model can still continue work.

### Step 5: Tool Packs (2–3 days)

- Implement Tool Pack selection and intersection with Focus Profile tool list.
- Load pack membership from config with sane defaults.
- **Test:** Typical calls expose ~8–15 tools, not 40+; escalation works (read → write → danger).

### Step 6: Per-Request Provider Options (1–2 days)

- Add per-request overrides (max tokens, timeout, temperature) to provider calls.
- Use smaller budgets for read/plan-style calls and larger budgets for execution/correction calls.
- **Test:** Budgets are actually applied and observable in provider requests.

### Step 7: Retry & Loop Detection (2–3 days)

- Implement error signature definition and loop response behavior.
- **Test:** repeated identical failure stops with a helpful report and next-step suggestions.

### Step 8: Write → Compile → Verify → Self-Correct Policy (2–3 days)

- Make verify/compile results first-class inputs to the next model turn (with distilled errors).
- **Test:** common Blueprint compile failures trigger a correction attempt and converge or stop cleanly.

### Step 9: System Prompts + Token Budget (3–4 days)

- Write all system prompt files
- Full prompt assembly for all three modes
- Within-worker truncation
- **Test:** Good results with real models. Token budgets respected.

### Step 10: Integration Testing (3–4 days)

- Cross-domain orchestrated task end-to-end
- Single worker end-to-end
- Self-correction, cancellation, loop detection
- Focus Profile interaction
- Multiple model providers
- Token budget verification

**Total estimated effort: 2–3 weeks (Phase E core)**

---

## 11. Validation Criteria

Phase E is complete when:

- [ ] Confirmation tier routing works correctly by origin (not hardcoded `bFromMCP`)
- [ ] Prompt distillation contract holds (last 2 raw pairs unless capped; older summarized)
- [ ] Tool packs reduce per-call schema cost (typical calls expose a small curated tool list)
- [ ] Operation feed shows step-level progress
- [ ] Workers self-correct on compile errors
- [ ] Loop detector stops runaway workers
- [ ] Per-request provider options are enforced (budgets are real)
- [ ] System prompts work with at least 2 different model providers
- [ ] Token usage measurably lower than sending all tools every call
- [ ] Conversation Manager cleanly refactored — Brain owns orchestration, CM owns transport

---

## 12. Decisions Log

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Orchestrator + workers | Phase E2 | Only build if metrics show single-loop degrades on cross-domain tasks |
| Prompt distillation | Yes (Phase E core) | Biggest token savings; prevents JSON snowballing |
| Tool packs | Yes (Phase E core) | Reduces flat schema cost per call without orchestrator complexity |
| Workers are API calls, not model feature | Brain C++ manages coordination | Works with any model. No vendor lock-in. |
| No OperationPlan DAG | Orchestrator outputs simple step list | LLMs produce reliable ordered lists, not reliable dependency graphs. |
| No separate UE modules | Subdirectories within AIAgentEditor | Build complexity. Namespaces sufficient. |
| Focus Profile UI | Exactly 3 options | Product simplicity: Auto (default), Blueprint (editor work), C++ |
| System prompts as text files | Not compiled into plugin | Editable without rebuild. Version-controllable. |
| Thread-local context | RAII scope | No signature changes. Idiomatic UE C++. |

---

## 13. Phase E2 (Optional Upgrade) — Orchestrator/Workers

Phase E2 is only needed if real usage shows the single-loop Brain cannot maintain quality or token efficiency for cross-domain tasks.

**Trigger criteria (examples; tune after telemetry):**
- Cross-domain success rate (BP+BT+BB, BP+PCG) falls below an acceptable threshold.
- Median tokens per successful cross-domain task exceeds budget.
- Frequent context contamination failures (BT work derailed by BP history).

If triggered, Phase E2 adds:
- Orchestrator call (no tools) to decompose multi-domain/multi-asset tasks.
- Worker calls that run with a clean context window and pack-scoped tools.
- Inter-worker context passing via distilled history summaries.

## 14. Future Evolution (NOT Phase E)

- **Parallel worker execution** — Workers without dependencies run concurrently
- **Worker model selection** — Cheaper model for simple workers, stronger for complex
- **Orchestrator re-planning** — Revise remaining plan if a worker fails
- **Plan visualization** — Structured UI showing orchestrator's plan before execution
- **Operation replay** — Re-execute a previous run's plan on different assets
- **Learning from corrections** — Feed user manual fixes back into prompts
- **Dynamic worker configs** — Workers adapt tool set mid-execution
