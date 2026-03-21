# Research: MCP Session Log Analysis — 2026-03-20

## Question
Analyze the most recent MCP session in `docs/logs/UE_Olive_AI_Toolkit.log` for Claude Code performance issues.

---

## Findings

### 1. Client Info

- **Client:** claude-code **2.1.74** (identified at line 1775)
- **Session started:** 2026-03-20 23:44:14 (initialize request)
- **Tool count returned by tools/list:** **87 tools** (line 1778 — old code, think tool NOT present)
  - Confirmed: 87 = no `olive.think` tool. The "88 = think tool present" threshold was not reached.
- **`instructions` in initialize:** Not surfaced in the log. The server logs the client name/version but does not echo the instructions field content.
- **`think` tool used:** NO. Zero occurrences of `olive.think` tool calls in the entire session.

---

### 2. Total Task Duration

| Marker | Timestamp |
|--------|-----------|
| First tool call (`project.search`) | 23:44:46 |
| Last tool call (`blueprint.read`) | 23:56:47 |
| **Total agent active time** | **12 minutes 1 second** |

UE editor closed at 23:59:23, but the last Olive tool call was at 23:56:47. The "Claude Code" Slate window was destroyed at 23:58:25 (line 2510).

---

### 3. What Was the Task?

**Task: Create a BP_Pistol weapon Blueprint at `/Game/Weapons/BP_Pistol`.**

Inferred from tool calls and params:
- Discovery phase: searched for "weapon", "gun", "pistol", "firearm"; read existing `BP_ThirdPersonCharacter`; read C++ character class, game mode header, and toolkit headers — standard reconnaissance
- Creation: `blueprint.scaffold` created `BP_Pistol` with `GunMesh` (StaticMesh) + `MuzzlePoint` (SceneComponent) and 6 variables (bCanFire, CurrentAmmo, MaxAmmo, FireRate, Damage, bFireResult)
- Added 3 event dispatchers: `OnFired`, `OnAmmoEmpty`, `OnReloaded`
- Added 3 functions: `Fire`, `ResetCanFire`, `Reload`
- Wired `Fire` (19-step plan: branch on bCanFire + ammo check + K2_SetTimer + 3 delegates), `ResetCanFire` (1 step), `Reload` (5 steps)
- Post-verify: wired K2_SetTimer's Object pin to a K2Node_Self node

**Single Blueprint built. No BP_Bullet, no BP_ThirdPersonCharacter modifications.**

---

### 4. Tool Call Count and Breakdown

Total tool calls: **42** (count of `MCP tools/call result` lines)
Total failures: **5**
**Tool success rate: 37/42 = 88.1%**

| Tool | Count | Success | Notes |
|------|-------|---------|-------|
| `project.search` | 5 | 5 | 4 in burst at start (weapon/gun/pistol/firearm), 1 later ("interface") |
| `project.get_relevant_context` | 2 | 2 | weapon+pistol; character+player |
| `blueprint.read` | 3 | 3 | BP_ThirdPersonCharacter full, Fire graph, BP_Pistol summary |
| `cpp.read_class` | 1 | 1 | UE_Olive_AI_ToolkitCharacter with inherited |
| `cpp.list_project_classes` | 1 | 1 | parent ACharacter filter |
| `cpp.read_header` | 3 | 3 | Character.h, GameMode.h, Toolkit.h |
| `olive.get_recipe` | 1 | 1 | "create pistol blueprint weapon" |
| `blueprint.list_templates` | 1 | 1 | "weapon pistol gun" |
| `olive.search_community_blueprints` | 1 | 1 | NEW TOOL — first time observed in production |
| `blueprint.get_template` | 3 | 3 | gun (full), gun+Fire, gun+Reload |
| `blueprint.scaffold` | 1 | 1 | Created BP_Pistol |
| `blueprint.add_function` | 6 | 6 | 3 dispatchers + Fire + ResetCanFire + Reload |
| `blueprint.apply_plan_json` | 6 | 3 | Fire ×3 (fail, fail, succeed); ResetCanFire ×1; Reload ×1; Self-wire attempt ×1 (fail) |
| `blueprint.verify_completion` | 1 | 1 | BP_Pistol final check |
| `blueprint.set_pin_default` | 1 | 1 | node_31.FunctionName = "ResetCanFire" |
| `blueprint.connect_pins` | 3 | 1 | node_21→node_32 (fail), node_38→node_33 (fail), node_41→node_31 (succeed) |
| `blueprint.get_node_pins` | 1 | 1 | Inspected node_31 to confirm it was K2_SetTimer |
| `blueprint.add_node` | 1 | 1 | K2Node_Self |
| `blueprint.compile` | 1 | 1 | Final explicit compile |
| **TOTAL** | **42** | **37** | |

**plan_json success rate: 3/6 = 50%**

`blueprint.add_function` triggers `bAutoCompile` — 6 auto-compiles (one per function/dispatcher). Plus 3 via `apply_plan_json` success, 1 via `verify_completion`, 1 explicit. Total compile events: **11 — all SUCCESS**.

---

### 5. Top Thinking Gaps (>15s between consecutive tool calls)

Ordered by duration:

| # | Gap Duration | Before | After | Assessment |
|---|-------------|--------|-------|------------|
| 1 | **~163s (2:43)** | 23:46:29 `olive.search_community_blueprints` | 23:49:12 `blueprint.get_template` | LARGEST: reading community blueprint results |
| 2 | **~106s (1:46)** | 23:54:30 `connect_pins` (success) | 23:56:16 `blueprint.compile` | Second largest: post-Self-wire deliberation before compiling |
| 3 | **~50s** | 23:53:10 `blueprint.read` Fire graph | 23:54:00 `connect_pins` (fail — wrong node) | Reading 39-node graph, misidentified target node |
| 4 | **~40s** | 23:44:49 `get_relevant_context` | 23:45:29 `blueprint.read` | Initial recon decision-making |
| 5 | **~32s** | 23:51:54 `verify_completion` | 23:52:26 `set_pin_default` | Post-verify analysis: noticed FunctionName pin unset |
| 6 | **~28s** | 23:45:59 `cpp.read_header` | 23:46:28 `olive.get_recipe` | Switching from research to recipe/template phase |
| 7 | **~27s** | 23:49:41 `blueprint.scaffold` | 23:50:08 `blueprint.add_function` | Planning function structure |
| 8 | **~21s** | 23:49:19 `blueprint.get_template` Reload | 23:49:40 `blueprint.scaffold` | Reviewing templates, planning scaffold params |
| 9 | **~19s** | 23:52:36 `apply_plan_json` Self (fail) | 23:52:55 `blueprint.add_node` K2Node_Self | Correct fallback decision |
| 10 | **~18s** | 23:50:18 `blueprint.add_function` Reload | 23:50:36 `apply_plan_json` Fire (fail) | Planning Fire plan JSON |

---

### 6. Did Claude Ask the User Any Questions?

**No.** The session was fully autonomous. The user only interacted at the end: opened BP_Pistol in the editor (line 2508), then opened BP_ThirdPersonCharacter (line 2513), then saved both and closed the editor (lines 2516–2526). No in-session user interaction between tool calls.

---

### 7. Redundant / Wasted Calls

**7a. Four synonymous project.search calls (lines 1781–1796)**
"weapon", "gun", "pistol", "firearm" — four separate calls in 3 seconds. All returned near-identical results (no weapon assets exist yet). A single `project.get_relevant_context` with a combined query covers this. Wasted: ~3s + 4 round trips.

**7b. Two irrelevant cpp.read_header calls (lines 1840–1847)**
`UE_Olive_AI_ToolkitGameMode.h` and `UE_Olive_AI_Toolkit.h` are irrelevant to creating BP_Pistol. The character header (line 1832) was legitimate. Wasted: ~15-20s including thinking time.

**7c. Community blueprints 2:43 gap**
The community blueprint search itself was fast (83ms). The 2:43 delay was Claude reading/processing results. If the results are verbose (full node graphs), this is a significant token overhead. `mode:"browse"` should limit output — worth verifying what this actually returns in volume.

**7d. Missing schema_version on first apply_plan_json (line 1976)**
Classic recurring error. First Fire attempt rejected immediately for missing `schema_version`. Cost: ~18s.

**7e. apply_plan_json attempt 2 used wrong node type for return (line 1981–2065)**
Attempt 2 (schema_version=1.0) included `op:"call", target:"FunctionOutput"` as a literal node type string instead of using the `op:"return"` op. The executor rejected "FunctionOutput" as unknown. Attempt 3 (schema_version=2.0) correctly used `op:"return"`. Cost: ~35s.

**7f. connect_pins node_21→node_32 failed silently (line 2405–2415)**
The plan_json had already wired `node_21.ReturnValue → node_32.RemainingAmmo` (line 2248). When Claude tried to re-wire it post-verify, the connection failed with an **empty error message** (line 2412: `Error: Execution failed for tool 'blueprint.connect_pins' ()`). The blank error message caused Claude to assume the connection was never made and triggered a 4-minute workaround cascade.

**7g. connect_pins to wrong node after graph read (line 2453–2463)**
After reading the 39-node Fire graph, Claude attempted `node_38.self → node_33.Object`. node_33 is a Branch node (no Object pin). The actual K2_SetTimer is node_31. The dense 39-node graph output caused node ID misidentification. Cost: ~50s thinking + 1 failed call.

---

### 8. Error Cascades

**Cascade 1: Fire function plan (3 attempts, ~52 seconds)**
```
23:50:36  apply_plan_json Fire — FAIL: PLAN_MISSING_FIELD (no schema_version)
23:50:54  apply_plan_json Fire — FAIL: Unknown node type 'FunctionOutput' (used call op instead of return op)
23:51:28  apply_plan_json Fire — SUCCESS (schema_version=2.0, op:return used correctly)
```
Total wasted: ~52s. Both failures were agent errors with known workarounds.

**Cascade 2: K2_SetTimer Object pin wiring (~4 minutes)**
```
23:51:54  verify_completion — SUCCESS (agent notices FunctionName pin not set)
23:52:26  set_pin_default FunctionName=ResetCanFire — SUCCESS
23:52:27  connect_pins node_21.ReturnValue → node_32.RemainingAmmo — FAIL (silent, already wired)
23:52:36  apply_plan_json (call "Self") — FAIL: FUNCTION_NOT_FOUND
23:52:55  add_node K2Node_Self — SUCCESS
23:53:10  blueprint.read Fire graph (39 nodes, 47 connections)
23:54:00  connect_pins node_38.self → node_33.Object — FAIL (wrong node: Branch not SetTimer)
23:54:16  get_node_pins node_31 — SUCCESS (confirmed node_31 = K2_SetTimer)
23:54:30  connect_pins node_41.self → node_31.Object — SUCCESS
```
Total wasted: ~4 minutes. Root causes: (1) silent empty error on already-wired pin confused the agent; (2) wrong node ID extracted from dense 39-node graph read.

---

### 9. Self-Reference Issues

**No `get_var "self"` or `SelfReference` variable lookup issue this session.** However there was a related attempt: at line 2416–2429, Claude used `op:"call", target:"Self"` in plan_json — treating "Self" as a callable function. This failed correctly with FUNCTION_NOT_FOUND (the alias map of 222 entries was searched, no Self function exists). Recovery was fast — immediately pivoted to `blueprint.add_node K2Node_Self` on the next call. This is the correct recovery path.

---

### 10. Comparison to Previous Sessions

| Metric | This Session (2026-03-20) | Gun+Bullet Run B (03-15b) | Bow Run 09t |
|--------|--------------------------|--------------------------|-------------|
| Task | 1 BP (BP_Pistol) | 3 BPs | 3 BPs |
| Duration | **12:01** | 4:12 | 7:38 |
| Total tool calls | 42 | 42 | 59 |
| Tool success rate | 88.1% | ~88% | 86.4% |
| plan_json success rate | 50% (3/6) | 58% (7/12) | 77.8% (7/9) |
| Compile cycles | 11 (all SUCCESS) | ~8 | ~9 |
| Auto-continues | 0 | 0 | 0 |

**12 minutes for a single BP is slower than comparable sessions.** The overhead is explained by:
1. Heavy research phase (~5 minutes for 16 tool calls including community blueprints)
2. K2_SetTimer Self-pin wiring cascade (~4 minutes)
3. Three attempts on Fire plan (~52 seconds)

**Positives:**
- The Fire function plan (19 steps, 2 branches, 3 dispatchers, K2_SetTimer, return op) is the most architecturally complex single-function plan observed in any session. It compiled clean.
- All 11 compile cycles succeeded — zero compile failures across the entire session.
- `call_delegate` for all 3 dispatchers resolved correctly and executed without error.
- The agent used the full research stack: get_recipe + list_templates + search_community_blueprints + get_template (3×) before building — research-first confirmed.
- `olive.search_community_blueprints` used for the first time in production.

---

## Recommendations

1. **Make `schema_version` optional with a default of "2.0".** This removes the most frequent first-attempt failure across all sessions. The field conveys no semantic variation — "2.0" is always the right value. Alternatively, improve the PLAN_MISSING_FIELD error to say exactly what to add: `"Add \"schema_version\": \"2.0\" as the first field in plan_json."` The current message is sufficient for recovery but doesn't prevent the miss.

2. **Fix the empty error message from `blueprint.connect_pins` when the wire already exists.** Line 2412 shows `Error: Execution failed for tool 'blueprint.connect_pins' ()` — empty error body. This is the root cause of the 4-minute cascade. Should return: `"Pin connection skipped: node_21.ReturnValue is already connected to node_32.RemainingAmmo. No action required."` (or even succeed silently — idempotent connect is reasonable behavior).

3. **Document K2_SetTimer Object pin requirement in the knowledge pack.** When calling `KismetSystemLibrary::K2_SetTimer` from within a Blueprint, the Object pin must be explicitly wired to Self. This is not obvious — K2_SetTimer does not auto-bind to self. The gun.json template or the alias map entry for "SetTimer" should include a note: "Requires Object pin wired to self (use K2Node_Self)." This would eliminate this entire post-plan repair cycle.

4. **The four parallel synonymous project.search calls (weapon/gun/pistol/firearm) are a persistent pattern.** Add to system prompt: "Use a single project.search or project.get_relevant_context with space-separated terms instead of separate calls for synonyms. Do not call project.search more than twice during initial discovery."

5. **The 39-node graph read caused node ID misidentification.** When `blueprint.read` returns a graph with >20 nodes, the agent has difficulty reliably matching node IDs. Consider adding a `node_filter` parameter to `blueprint.read graph` that accepts a node type or function name, returning only matching nodes with their IDs and pins. This would allow `blueprint.read(graph="Fire", node_filter="K2_SetTimer")` instead of parsing 39 nodes.

6. **Monitor `olive.search_community_blueprints` result verbosity.** The 2:43 gap after this call (the largest in the session) suggests heavy output processing. The `mode:"browse"` parameter may not be sufficiently limiting result size. Add a token budget check or enforce a max results count that produces results concise enough to process in under 60s.

7. **The agent read GameMode.h and Toolkit.h — irrelevant headers.** This suggests the `cpp.read_class` on `UE_Olive_AI_ToolkitCharacter` (which reads 183 properties + 243 functions) included references to these files and the agent followed them unnecessarily. The `cpp.read_class` response should make the header file path obvious so the agent only reads the primary character header, not every referenced header.

8. **`op:"call", target:"Self"` in plan_json should return a targeted error.** The current FUNCTION_NOT_FOUND trail (222 aliases searched, 23 classes searched) is correct but generic. Add a specific check: if the target is "Self", "self", or "SelfRef", return immediately with: `"'Self' is not a callable function. To get a self-reference, use blueprint.add_node with type K2Node_Self, then connect its output pin."` This short-circuits the expensive function search.

9. **plan_json success rate this session (50%) was caused entirely by two known bug patterns** (missing schema_version, FunctionOutput as node type string). The underlying plan logic was correct — the 19-step Fire function is solid evidence the resolver/validator/executor pipeline is working well. The 50% rate is a misleading performance indicator; the actual plan quality is high.

10. **The 1:46 gap after successfully wiring Self→node_31.Object before calling compile** (lines 2481–2485) is unusual. The agent had just fixed the last known issue but waited nearly 2 minutes before compiling. This may indicate the agent was doing a mental review of the entire session state before committing. No tool calls should have been needed here — just a straight compile. This is a thinking efficiency issue: after a successful repair, the next action should be immediate.
