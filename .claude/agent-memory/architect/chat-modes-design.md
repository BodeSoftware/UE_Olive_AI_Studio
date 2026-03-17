---
name: CLI Chat Modes Architecture
description: Code/Plan/Ask mode system replacing confirmation tiers, focus profiles, and tool packs. 12-task implementation plan.
type: project
---

CLI-style chat modes (Code/Plan/Ask) designed Mar 2026. Design spec: `plans/cli-chat-modes-design.md`. Architecture plan: `plans/cli-chat-modes-architecture.md`.

**Why:** Current confirmation tiers, safety presets, focus profiles, and tool pack gating add friction that autonomous MCP mode already bypasses. Modes unify in-engine chat with the autonomous experience.

**How to apply:**
- `EOliveChatMode` (Code/Plan/Ask) replaces `EOliveConfirmationTier`, `EOliveSafetyPreset`, focus profiles, tool packs
- Mode stored on `FOliveConversationManager.ActiveChatMode`, propagated via `FOliveWriteRequest.ChatMode`
- Stage 2 of write pipeline becomes mode gate: Ask blocks all writes, Plan blocks writes except preview_plan_json, Code passes everything
- MCP path (`bFromMCP=true`) bypasses Stage 2 entirely -- unchanged
- Brain state machine simplified 7->3 states (Idle, Active, Cancelling)
- Plan mode "go"/"do it" triggers temporary Code flip with cancel-safe revert via `FPreviousModeState`
- 12 ordered tasks, root dependency is Task 1 (brain enums)

**Key deletions:** FOliveFocusProfileManager, FOliveToolPackManager, EOliveConfirmationTier, EOliveSafetyPreset, 13 UPROPERTYs from settings, entire confirmation flow in ConversationManager, OliveToolPacks.json, focus profile tests, profile prompt files
