I can draft it, but I cannot save the file right now because the session is under a hard read-only Plan Mode restriction. The write would need to go to plans/, but the current system reminder explicitly forbids any file modification.
Here is the full .md content ready to save as plans/plan-mode-persistent-chat-design.md:
# Design: Persistent Plan Session for Plan Mode
**Author:** OpenCode
**Date:** 2026-03-18
**Status:** DRAFT
**Related docs:**
- `plans/cli-chat-modes-design.md`
- `plans/cli-chat-modes-architecture.md`
- `plans/agent-planning-system-design.md`
---
## Problem Statement
Plan mode currently behaves more like a prompt/style switch than a true ongoing planning conversation.
When the user starts in Plan mode and the AI proposes a plan, follow-up messages often lose the specific plan context that was just discussed. This causes failures like:
- User: `create a gun blueprint`
- AI: proposes a plan
- User: `have it shoot bullets too`
- AI no longer remembers the prior gun plan and treats the follow-up too much like a fresh request
This is especially visible with autonomous CLI providers that do not support native session resume. In those cases, the system often sends only the newest message instead of a durable planning context.
The result is that Plan mode does **not** feel like Claude Code / Codex native plan flows, where:
- the plan is a living artifact
- follow-up messages revise the existing plan
- switching from Plan mode to Code mode carries the plan forward into execution
---
## Product Goal
Make Plan mode behave like a persistent planning chat session.
The AI should:
- remember the current plan it just proposed
- treat user follow-ups as revisions to that plan unless the user clearly starts a new task
- preserve task identity when switching between Plan and Code
- carry the active plan into Code mode as the execution starting point
This is a **task continuity** feature, not a “missing requirement inference” feature. The system should remember what the AI and user were planning together, not guess what the AI forgot.
---
## Non-Goals
This design does **not** aim to:
- add full transcript persistence to disk
- make all providers support real CLI resume
- invent requirements the user did not ask for
- auto-switch modes based on message content
- replace the existing chat history model
This is a lightweight session-memory layer specifically for Plan-mode continuity and Plan->Code handoff.
---
## Root Cause
### 1. Plan mode state is too shallow
The current system tracks mode (`Code`, `Plan`, `Ask`), but not enough structured state about the active plan itself.
It knows:
- what mode the user is in
It does **not** reliably preserve:
- the active planning goal
- the latest plan draft
- the current target asset/system
- the user's follow-up revisions to that plan
### 2. Autonomous non-resumable providers are effectively stateless between turns
For providers without native session resume, each new message is treated as largely independent. That means the provider may receive:
- the new user message
- optional asset search/context
But not:
- the active plan draft
- the previous planning decisions
- the fact that the user is continuing the same planning thread
### 3. Plan->Code handoff is mode-only, not plan-aware
When the user switches from Plan mode to Code mode, the system preserves the mode change but does not carry forward enough of the active planning artifact.
This makes execution feel detached from the plan the AI just proposed.
---
## Desired User Experience
### Example 1: Plan refinement
User:
`create a gun blueprint`
AI in Plan mode:
- proposes a plan for a gun Blueprint
User:
`have it shoot bullets too`
Expected behavior:
- AI revises the same gun plan
- AI does not start over from scratch
- AI treats this as an adjustment to the active plan
### Example 2: Plan to Code
User:
`create a gun blueprint`
AI in Plan mode:
- proposes the plan
User:
`switch to code mode and do it`
Expected behavior:
- AI keeps the same task identity
- AI uses the current plan draft as execution context
- AI starts building the planned gun system
### Example 3: New task reset
User:
`create a gun blueprint`
AI in Plan mode:
- creates a plan
User:
`actually make a door blueprint instead`
Expected behavior:
- previous plan session is replaced or archived
- new plan session starts for the door blueprint
---
## Proposed Solution
Introduce a first-class **Plan Session State** owned by `FOliveConversationManager`.
This state stores a compact, structured representation of the ongoing planning conversation. It becomes the canonical “what are we currently planning?” object.
Plan mode then works as a persistent session:
- initial plan creates the plan session
- follow-up messages revise the plan session
- Code mode can consume the plan session
- switching back to Plan mode resumes the same session
---
## New Core Type: FOlivePlanSessionState
**File:** `Source/OliveAIEditor/Public/Chat/OliveConversationManager.h`
Add a struct like:
```cpp
struct FOlivePlanSessionState
{
	bool bHasActivePlan = false;
	FString SessionId;
	FString GoalSummary;
	FString TargetSummary;
	FString ActiveModeContext;
	FString LatestPlanText;
	FString LatestPlanSummary;
	TArray<FString> UserPlanAdjustments;
	TArray<FString> KeyDecisions;
	TArray<FString> OutstandingQuestions;
	TArray<FString> PlannedAssetsOrArtifacts;
	FDateTime LastUpdatedUtc;
	void Reset();
	bool IsEmpty() const;
};
Field meanings
- bHasActivePlan
  - whether a meaningful active plan exists
- SessionId
  - logical ID for telemetry/debugging; not provider session resume
- GoalSummary
  - short task identity
  - example: Create a gun blueprint
- TargetSummary
  - what the plan is centered on
  - example: Gun Blueprint system
- ActiveModeContext
  - textual mode state for continuity
  - example: Plan, later Code (executing approved plan)
- LatestPlanText
  - full most recent plan response text, truncated if needed
- LatestPlanSummary
  - compact structured summary of the current plan
- UserPlanAdjustments
  - follow-up user changes
  - example:
    - Have it shoot bullets too
    - Use hitscan instead
    - Switch to code mode and build it
- KeyDecisions
  - plan decisions the AI already made
  - example:
    - Use separate gun and projectile actors
    - Attach gun to character mesh socket
- OutstandingQuestions
  - open issues the plan identified but has not resolved
- PlannedAssetsOrArtifacts
  - asset list from the active plan
  - example:
    - BP_Gun
    - BP_Bullet
    - Modify BP_ThirdPersonCharacter
---
High-Level Behavior Changes
In Plan mode
Every user message should be interpreted as one of:
1. Start plan
2. Revise plan
3. Approve/transition plan
4. Replace plan with new task
The system should no longer treat Plan mode as “prompt style only.”
It should treat it as “operate on the active plan session.”
In Code mode after Plan mode
If the user switches to Code mode while a plan session exists:
- preserve the same task identity
- provide the latest plan summary to the provider
- frame execution as “implement this active approved plan”
In Ask mode
Ask mode should remain read-only and should not automatically create or revise a plan session unless explicitly desired. Keep Ask mode simple.
---
Conversation Manager Responsibilities
Primary file: Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp
Add plan-session lifecycle management here.
New responsibilities
- own the active FOlivePlanSessionState
- update it from user messages
- update it from assistant planning responses
- decide whether a message continues the active plan or starts a new task
- pass compact plan context into autonomous stateless provider calls
- pass plan summary into Code mode execution after Plan mode
---
New Helper Methods
1. UpdatePlanSessionFromUserMessage
void UpdatePlanSessionFromUserMessage(const FString& Message);
Purpose:
- classify the new user turn relative to the active plan
- update adjustments, mode intent, or reset plan session if task changed
Behavior:
- if no active plan exists and current mode is Plan, initialize one
- if message looks like a continuation, append to UserPlanAdjustments
- if message clearly starts a new task, reset and start a new plan session
- if message is a mode transition command, preserve plan identity
2. UpdatePlanSessionFromAssistantMessage
void UpdatePlanSessionFromAssistantMessage(const FOliveChatMessage& AssistantMessage);
Purpose:
- extract and store the current plan artifact after each plan response
Behavior:
- when in Plan mode, treat assistant responses as candidate plan updates
- store:
  - LatestPlanText
  - LatestPlanSummary
  - PlannedAssetsOrArtifacts
  - KeyDecisions
  - OutstandingQuestions
This does not require perfect parsing initially. Version 1 can use lightweight heuristics.
3. BuildPlanContinuationContext
FString BuildPlanContinuationContext() const;
Purpose:
- generate a compact context block for stateless providers
Output shape:
## Active Planning Context
Current mode: Plan
Current task: Create a gun blueprint
Current target: Gun Blueprint system
## Current Plan Summary
- Planned assets: BP_Gun, BP_Bullet, Modify BP_ThirdPersonCharacter
- Key decisions: separate gun actor, projectile bullets
- User follow-up adjustments:
  - have it shoot bullets too
Treat the next user message as a continuation of this active plan unless it clearly starts a new task.
4. BuildPlanExecutionContext
FString BuildPlanExecutionContext() const;
Purpose:
- generate the handoff context when moving from Plan to Code
Output shape:
## Approved Plan Context
The user is continuing an existing planned task.
Current task: Create a gun blueprint
Execution mode: Code
Plan summary:
- Create BP_Gun
- Create BP_Bullet
- Modify character to equip/fire the gun
Implement this plan, adapting only where necessary based on actual project state.
5. ShouldContinueActivePlan
bool ShouldContinueActivePlan(const FString& Message) const;
Purpose:
- decide whether a user message continues the active plan
Heuristics:
- continuation cues:
  - it
  - that
  - also
  - too
  - instead
  - now
  - switch to code mode
  - do it
  - build it
- explicit new-task cues:
  - new unrelated asset/system request with no continuation language
  - actually make a door blueprint instead
  - new task
  - forget that
Important:
- this is task continuity detection
- not “guess what requirement was missing”
---
Provider Integration
Stateless autonomous providers must receive active plan context
Primary file: Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp
Current issue:
- non-session providers treat each message as independent
New behavior:
- if SupportsSessionResume() is false
- and there is an active plan session
- prepend BuildPlanContinuationContext() before the new message
New provider-side hook
File: Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h
Add either:
- a setter for continuation context, or
- an overload parameter in SendMessageAutonomous
Recommended shape:
void SendMessageAutonomous(
	const FString& UserMessage,
	const FString& ContinuationContext,
	const FOnOliveStreamChunk& OnChunk,
	const FOnOliveComplete& OnComplete,
	const FOnOliveError& OnError);
Alternative:
keep the external signature stable and set a temporary member before dispatch.
Recommended behavior matrix
Provider supports resume
- rely on native session resume
- still optionally inject mode/plan summary on major transitions
Provider does not support resume
- always prepend active plan context if plan session exists
- on Plan->Code transition, prepend execution context
---
Plan Response Capture
The system must remember the plan the AI just suggested.
That means after every assistant response in Plan mode, capture a structured summary.
Version 1 extraction strategy
Do not build a fragile formal parser first.
Use simple extraction:
- keep raw assistant plan text in LatestPlanText
- build LatestPlanSummary using:
  - first paragraph
  - numbered list headings if present
  - bullets that look like assets, steps, or decisions
Detect plan-like lines:
- numbered items
- assets:
- create
- modify
- use
- add
- components
- variables
- functions
- communication
Version 2 extraction strategy
If needed later, add a structured internal planning envelope, for example:
- explicit PLAN_SUMMARY
- ASSETS
- DECISIONS
- QUESTIONS
But do not block V1 on this.
---
Plan Mode Prompt Strategy
Plan mode should explicitly instruct the AI that it is editing a persistent plan, not starting fresh every turn.
File: Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp
Update the Plan mode suffix to emphasize:
- ongoing planning session
- revise the active plan when a follow-up comes in
- preserve task identity across follow-up messages
- if switching to Code mode, treat the plan as the approved execution basis
Recommended suffix language:
You are in Plan mode. Treat this as an ongoing planning session, not a one-turn answer.
When the user sends follow-up messages, revise the current plan unless they clearly start a new task.
Preserve task identity across follow-ups and mode switches.
Do not execute write operations.
---
## Plan -> Code Handoff
This is a core requirement.
When the user switches from Plan to Code:
- do not discard the active plan
- do not rely only on generic chat history
- explicitly pass the active plan summary into the next Code-mode run
### Required behavior
If:
- current mode was `Plan`
- active plan exists
- user switches to `Code`
Then:
- mark plan session as approved/in execution
- build execution context from the active plan
- prepend that context to the next Code-mode provider call
This should make the experience feel like:
- “now execute the plan we just worked out”
instead of:
- “start over in Code mode”
---
UI / UX Notes
Optional system messages
When the plan session is updated, show concise system messages like:
- Plan session started: Create a gun blueprint
- Plan updated with user revision: have it shoot bullets too
- Switched to Code mode: executing active plan
These messages improve transparency without exposing internals.
Optional future enhancement
Add a small plan-session badge or expandable summary in the chat UI:
- current task
- current mode
- number of revisions
Not required for the first implementation.
---
Logging
Add explicit logs for debugging continuity.
Files:
- Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp
- Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp
Recommended logs:
- Plan session started: <GoalSummary>
- Plan session updated: <message summary>
- Plan session reset: new task detected
- Injecting plan continuation context for stateless provider
- Injecting plan execution context for Plan->Code transition
This is important because the current issue is easiest to verify in logs.
---
Test Plan
1. Conversation continuity in Plan mode
Add tests to:
Source/OliveAIEditor/Private/Tests/Conversation/OliveConversationManagerTests.cpp
Cases:
- start plan session from first Plan-mode request
- follow-up with it, also, too continues the active plan
- explicit new task resets the plan session
2. Plan capture
Verify:
- assistant response in Plan mode updates LatestPlanText
- summary fields are populated
- planned assets are retained when detectable
3. Stateless provider continuation injection
Verify:
- non-resumable providers receive continuation context on follow-up turns
- resumable providers do not require the fallback path
4. Plan -> Code transition
Verify:
- user starts in Plan mode
- AI returns plan
- user switches to Code
- next provider message includes plan execution context
5. Negative cases
Verify:
- Ask mode does not accidentally start execution
- unrelated task resets active plan session
- no plan context is attached when there is no active plan
---
Suggested File-by-File Change List
Modify
1. Source/OliveAIEditor/Public/Chat/OliveConversationManager.h
   - add FOlivePlanSessionState
   - add helper method declarations
   - add active plan session member
2. Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp
   - manage plan session lifecycle
   - update session on user/assistant turns
   - build continuation/execution context
   - preserve session across mode changes
3. Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h
   - add continuation-context support for autonomous sends
4. Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp
   - prepend plan context for stateless providers
   - log context injection
5. Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp
   - strengthen Plan mode suffix to describe persistent planning behavior
6. Source/OliveAIEditor/Private/Tests/Conversation/OliveConversationManagerTests.cpp
   - add continuity and mode-transition tests
Optional later modifications
7. Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp
   - show plan-session status in UI
8. Source/OliveAIEditor/Private/Brain/OlivePromptDistiller.cpp
   - preserve task identity / plan summary better in compaction logic
---
Implementation Phases
Phase 1: Minimal continuity fix
Goal:
make Plan mode remember the current plan across follow-up messages
Work:
- add FOlivePlanSessionState
- update it from user and assistant Plan-mode messages
- inject continuation context into stateless providers
Success criteria:
- create a gun blueprint -> have it shoot bullets too stays on the same plan
Phase 2: Plan -> Code handoff
Goal:
make execution feel like continuation of the approved plan
Work:
- build execution-context handoff
- preserve session on mode switch
- inject plan summary into first Code-mode execution turn
Success criteria:
- switch to code mode and do it executes the current plan without starting over
Phase 3: Better summary extraction
Goal:
improve reliability of stored plan summaries
Work:
- improve plan extraction heuristics
- keep more structured asset/decision/question summaries
Success criteria:
- plan revisions stay coherent across longer chats
Phase 4: UI polish (optional)
Goal:
make active plan state visible to the user
Work:
- lightweight plan-session indicator
- small system messages for plan lifecycle events
---
Risks
1. Over-aggressive continuation
Risk:
a new task is incorrectly treated as continuation of the active plan
Mitigation:
- only continue when cues are strong
- reset on explicit new-task language
- log the classification for debugging
2. Poor summary extraction
Risk:
assistant plan summaries are captured badly
Mitigation:
- keep raw LatestPlanText
- use summary only as a compact helper
- improve heuristics incrementally
3. Context bloat
Risk:
continuation blocks get too large
Mitigation:
- keep summary compact
- include only latest plan summary + user revisions
- avoid replaying full transcript
4. Divergence between chat history and plan session
Risk:
plan session state gets out of sync with visible chat
Mitigation:
- update session only at clear lifecycle points
- log every update
- use assistant final response text as the canonical plan artifact
---
Acceptance Criteria
This feature is complete when all of the following are true:
- In Plan mode, the AI remembers the plan it just proposed
- Follow-up user messages revise the same plan unless the user clearly starts a new task
- Stateless autonomous providers receive compact plan continuation context
- Switching from Plan to Code carries the active plan into execution
- Logs clearly show when plan session context is created, updated, reset, and injected
---
Example End-to-End Flow
Turn 1
Mode: Plan
User:
create a gun blueprint
System:
- create FOlivePlanSessionState
- set GoalSummary = "Create a gun blueprint"
- AI responds with a plan
- capture that plan into LatestPlanText and LatestPlanSummary
Turn 2
Mode: Plan
User:
have it shoot bullets too
System:
- classify as continuation
- append to UserPlanAdjustments
- build plan continuation context
- send revised planning request
- AI updates the same plan
Turn 3
Mode: Code
User:
switch to code mode and build it
System:
- preserve active plan session
- build execution context from LatestPlanSummary
- send Code-mode request with approved-plan context
- AI executes the existing plan
---
Recommendation
Implement Phase 1 and Phase 2 together.
Reason:
- Phase 1 alone fixes follow-up memory in Plan mode
- Phase 2 is required for the experience to match native CLI plan flows
- together they address the real user-facing problem: persistent planning plus clean execution handoff
Do not start with a heavy parser or disk persistence. A compact in-memory plan session is the right first implementation.