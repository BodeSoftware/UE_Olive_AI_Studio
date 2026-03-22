---
name: Session Performance Fix Design
description: Design decision to fix 32-minute Claude Code CLI sessions by template-first routing, fixing template exec chain bugs, expanding gun template with pickup/equip, and adding anti-pattern guidance
type: project
---

Designed a comprehensive fix for Claude Code CLI session performance regression. Core issue: "create a gun that shoots bullets" takes 32+ minutes (65 tool calls) when it should take under 5 minutes.

**Why:** The prompt routing in recipe_routing.txt and cli_blueprint.txt explicitly tells the AI to search the project FIRST before considering templates. Combined with template exec chain bugs (pure functions like GetInstigator/GetOwner having exec_after), missing pickup/equip logic in the gun template, and Python debugging spirals, the session balloons from ~3 tool calls to 65.

**How to apply:** Design plan at `plans/session-performance-fix.md`. Changes span 6 files: 2 prompt files, 2 template JSONs, 2 C++ files (string literal updates only). Implementation order: fix template bugs first, then expand gun template, then update prompt routing, then C++ strings, then anti-pattern guidance.

Key technical findings:
- GetInstigator, GetOwner, GetController, GetControlRotation, GetInstigatorController are all PURE functions (no exec pins). Templates must NOT use exec_after on steps that call these.
- OnComponentHit OtherActor pin: fuzzy `~Other` may not match reliably. Use explicit `OtherActor`.
- `@self.auto` is valid syntax handled by the plan resolver for self-reference wiring.
