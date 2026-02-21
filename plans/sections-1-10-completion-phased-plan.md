# Olive AI Studio - Sections 1-10 Completion Plan (Post Phase 0-5)

This plan targets remaining work across:
1. Vision & Goals
2. Key Design Decisions
3. Architecture Overview
4. Chat UI & User Experience
5. Agent System & Focus Profiles
6. Confirmation & Autonomy System
7. Context System
8. AI Provider Integration
9. AI Planning Engine (Brain Layer)
10. Policy & Rule System (Control Layer)

The order is optimized to reduce rework:
- Build reliability and enforcement first
- Then complete UX/context hooks
- Then expand providers
- Then add higher-level Brain intelligence

---

## Phase A - Control and Safety Hardening (Foundation for everything else)

### Goals
- Close control-layer gaps before adding more autonomy.
- Ensure write pipeline behavior is deterministic and auditable.

### Deliverables
- Implement full Tier 3 preview generation in write pipeline.
- Complete schema validation beyond required fields (type checks, enum checks, object shape checks).
- Add rate limiting for write operations (`MaxWriteOpsPerMinute`) in validation/control flow.
- Add missing Blueprint and PCG validation registration/coverage.
- Improve compile verification output with structured compile error extraction (not generic failure only).
- Add MCP progress/operation notifications transport (currently log-only).

### Primary files
- `Source/OliveAIEditor/Blueprint/Private/Pipeline/OliveWritePipeline.cpp`
- `Source/OliveAIEditor/Private/Services/OliveValidationEngine.cpp`
- `Source/OliveAIEditor/Private/MCP/OliveMCPServer.cpp`
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`

### Exit criteria
- Tier 3 operations return actionable preview payloads before execution.
- Invalid tool inputs are rejected consistently with structured errors.
- Write burst tests show throttling behavior works.
- MCP clients can subscribe to and receive operation progress events.

---

## Phase B - Chat UX and Context Completion

### Goals
- Finish the in-editor experience promised by sections 4 and 7.
- Align actual behavior with settings and plan expectations.

### Deliverables
- Wire auto-context from active editor/selection (`OnAssetOpened`, selection watcher).
- Implement right-click "Ask AI about this" targeted context handoff.
- Implement drag-drop assets into chat context bar.
- Add mention prefix filters (`@BP_`, `@BT_`, `@BB_`, `@PCG_`) in mention search.
- Honor UI settings toggles in panel:
  - operation feed visibility
  - quick actions visibility
  - completion notification/sound
- Add optional viewport context attachment path (manual trigger in chat UI).

### Primary files
- `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp`
- `Source/OliveAIEditor/Private/UI/SOliveAIInputField.cpp`
- `Source/OliveAIEditor/Private/UI/SOliveAIMessageList.cpp`
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`

### Exit criteria
- Context bar updates correctly from mentions, editor state, and drag-drop.
- Quick actions and operation feed can be toggled from settings.
- Mention filtering returns expected subsets by prefix.

---

## Phase C - Provider Matrix Completion

### Goals
- Complete BYOK provider strategy in section 8.
- Remove "not implemented" provider paths in chat settings.

### Deliverables
- Implement provider clients and factory wiring for:
  - OpenAI Direct
  - Google Direct
  - OpenAI Compatible
  - Z.AI
  - Ollama
- Add provider-specific model lists and request/response adapters.
- Add connection validation UX per provider.
- Replace plain config API key storage with secure storage path (or documented secure bridge) and migration for existing config keys.

### Primary files
- `Source/OliveAIEditor/Private/Providers/IOliveAIProvider.cpp`
- `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp`
- `Source/OliveAIEditor/Public/Providers/` (new provider classes)
- `Source/OliveAIEditor/Private/Settings/OliveAISettings.cpp`

### Exit criteria
- All provider enum options can be selected and used end-to-end.
- Provider error surfaces are specific and actionable.
- No runtime path returns "provider not implemented yet."

---

## Phase D - Focus Profiles and Prompt Governance

### Goals
- Make profile system consistent and configurable.
- Remove UI/manager drift and persist user profile customization.

### Deliverables
- Source profile options in UI from `FOliveFocusProfileManager` only (no hardcoded list).
- Persist custom profiles (save/load) with schema versioning.
- Align naming between "Everything" and current "Full Stack" choice.
- Add profile validation (allowed categories/tools, duplicate names).
- Add profile prompt regression tests (ensuring correct profile prompt additions are assembled).

### Primary files
- `Source/OliveAIEditor/Private/Profiles/OliveFocusProfileManager.cpp`
- `Source/OliveAIEditor/Private/UI/SOliveAIChatPanel.cpp`
- `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp`

### Exit criteria
- Profile list is single-source and consistent across UI/tool filtering/prompting.
- Custom profiles survive editor restart.
- Profile switch changes tool availability and prompt context deterministically.

---

## Phase E - Brain Layer Implementation and Autonomy Upgrade

### Goals
- Introduce explicit Brain-layer services from section 9.
- Move from implicit loop behavior to planned, stateful orchestration.

### Deliverables
- Add explicit Brain layer module/class boundary:
  - context assembly service
  - operation planner/sequencer
  - operation history store (session-local, structured)
  - retry and loop detection policy
- Add intent detection helpers (conversion insertion suggestions, dependency warnings, repeated-failure stop).
- Add context cache with invalidation hooks on write/external asset changes.
- Integrate planner with confirmation and control layers (not bypassing write pipeline).

### Primary files
- `Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp`
- `Source/OliveAIEditor/Public/Chat/` + `Private/Chat/` (new Brain classes)
- `Source/OliveAIEditor/Private/Index/OliveProjectIndex.cpp`
- `Source/OliveAIEditor/Private/MCP/OliveToolRegistry.cpp`

### Exit criteria
- Multi-step operations execute via explicit plan graph/state object.
- Operation history is queryable for current session and used in follow-up prompts.
- Repeated identical failures are detected and halted with actionable feedback.

---

## Phase F - Documentation and Alignment Pass

### Goals
- Bring docs and manifest in line with actual shipped capabilities.

### Deliverables
- Update plugin description and docs that still imply major features are future-only.
- Update long/short plan docs to reflect post-implementation architecture and phase closure.
- Add a "capability matrix" doc (tool/domain/profile/provider coverage).

### Primary files
- `UE_Olive_AI_Studio.uplugin`
- `README.md`
- `codex/ue-ai-agent-plugin-plan-long.md`
- `plans/ue-ai-agent-plugin-plan-v2.md`

### Exit criteria
- No major mismatch between code, settings UI, and documentation claims.

---

## Suggested implementation cadence

1. Phase A  
2. Phase B  
3. Phase C  
4. Phase D  
5. Phase E  
6. Phase F

Rationale:
- A and B stabilize behavior and user trust.
- C broadens compatibility only after stable control+UX.
- D ensures profile consistency before advanced Brain routing.
- E builds autonomy on top of a hardened base.
- F locks documentation to actual state.
