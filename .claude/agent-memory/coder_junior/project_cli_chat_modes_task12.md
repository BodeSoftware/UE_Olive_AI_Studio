---
name: CLI Chat Modes Task 12 — Doc Updates
description: Documentation changes made to reflect the new Code/Plan/Ask mode system, removing old tier/profile/preset concepts
type: project
---

Task 12 completed (2026-03-15). Documentation updated to reflect the new chat mode system.

**Why:** CLI Chat Modes architecture replaces 3-tier confirmation + focus profiles + safety presets with a single `EOliveChatMode` (Code/Plan/Ask) per session.

**How to apply:** Any future doc or comment that references the old system should use the new terminology below.

## Changes Made

### CLAUDE.md
- `Confirmation Tiers` section → `Chat Modes` section with Code/Plan/Ask mode table
- `Focus Profiles + Tool Packs` section removed entirely
- `Safety Presets` section → `Settings` section (references `DefaultChatMode`)
- Brain Layer: updated from 7-state to 3-state (`Idle`, `Active`, `Cancelling`)
- Write Pipeline Stage 2: `Confirm — tier routing` → `Mode Gate — mode-based access control`
- Key Singletons table: removed `FOliveFocusProfileManager` and `FOliveToolPackManager` entries
- Startup order: removed steps 13-14 (`FOliveFocusProfileManager`, `FOliveToolPackManager`)
- Module layout: removed `Profiles/` directory entry
- Tests subdir: removed `FocusProfiles/` from list
- Key File Locations: removed `Tool packs config` row
- Blueprint Plan JSON settings: removed `bEnforcePlanFirstGraphRouting`, `PlanFirstGraphRoutingThreshold`
- `FOliveWriteResult` factory methods: removed `ConfirmationNeeded()`

### .claude/agents/architect.md
- `Confirmation Manager — 3-tier system` → `Chat Modes — /code, /plan, /ask`
- `Focus Profiles — Tool-set filters` removed
- `7-stage validation pipeline` → `6-stage write pipeline`

### Content/SystemPrompts/
- Deleted: `ProfileBlueprint.txt`, `ProfileAIBehavior.txt.bak`, `ProfileCppBlueprint.txt.bak`, `ProfileLevelPCG.txt.bak`
- Remaining: Base.txt, BaseSystemPrompt.txt, Knowledge/, Orchestrator.txt, ToolCallFormat.txt, Worker_*.txt
