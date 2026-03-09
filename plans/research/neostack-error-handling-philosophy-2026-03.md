# Research: NeoStack AIK Error Handling Philosophy and AI Guidance Patterns

## Question

How does NeoStack AIK (Betide Studio) handle errors and guide the AI when tool calls fail?
Specifically:
1. When a function/node isn't found — what does the error look like? Does it suggest alternatives?
2. When pin names are wrong — does it list available pins?
3. Does it use knowledge packs / recipes / front-loaded knowledge, or smart error messages?
4. How does it handle component property access vs. function calls?
5. Does it have an alias/resolution system like Olive's?

Also: general best practices from MCP server design and other UE AI tools.

---

## Findings

### 1. NeoStack AIK Error Handling — What Is Publicly Known

**Source:** `plans/research/neostack-deep-dive-2026-03.md`, `plans/research/competitor-deep-dive-2026-03.md`

AIK's error handling is almost entirely undocumented publicly. What is known:

**The "500+ checks" are crash prevention, not semantic guidance.**

Added in v0.3.0 (Feb 8, 2026) in response to 100+ crashes at launch. From changelog: "500+ additional checks added to reduce crashes." These are defensive null/type/state guards in C++ — they prevent the editor from crashing, they do not guide the AI to use different parameters.

v0.5.0 (Feb 16, 2026) added a second layer: "Crash protection wraps all tool execution" — this is a try/catch or structured exception wrapper around each tool body. If the crash-prevention checks miss something, the outer catch prevents total editor death and presumably returns a structured error to the agent.

**No evidence of a rich error message system with suggestions.**

There is no public documentation, changelog mention, or user report describing AIK returning "did you mean X?" suggestions, listing available pins, or enumerating callable functions when a call fails. The tool audit log (`Saved/Logs/AIK_ToolAudit.log`) records timestamp, operation status (OK/FAIL), action type, asset name, and result details — but this is for crash reporting, not real-time AI guidance.

**Error recovery is delegated entirely to the agent.**

From the architecture: AIK spawns a Claude Code (or Gemini CLI, etc.) subprocess via ACP. The agent is Claude itself. When a tool call fails, the error message is returned to the agent, and Claude uses its own chain-of-thought reasoning to decide what to try next. AIK does NOT have a separate self-correction loop, retry policy, or error classification system. This is confirmed by: "Self-correction: Not explicitly documented... AIK relies on the LLM's own retry capability (since Claude Code and Gemini CLI have their own agentic loops) rather than a plugin-level self-correction system."

The v0.3.2 release note "one-shot task capability via improved prompting" confirms the correction strategy is prompt engineering injected at startup, not runtime error guidance.

**Conclusion: AIK's philosophy is prevention (500+ guards + try/catch) + delegation (agent handles recovery).**

---

### 2. AIK Knowledge Injection — Profiles vs. Recipes

**Source:** `aik.betide.studio/profiles`, `plans/research/neostack-deep-dive-2026-03.md` Section 5

AIK's knowledge injection uses three mechanisms:

**Profiles (primary channel for front-loaded knowledge):**
- 5 built-in profiles: Full Toolkit, Animation, Blueprint & Gameplay, Cinematics, VFX & Materials
- Each profile is a whitelist of tool visibility + optional custom instructions text
- The custom instructions are injected into the agent's system prompt at session start
- Individual tool descriptions can be overridden per-profile (this is how domain-specific guidance is delivered)
- No runtime knowledge injection on error — all knowledge is front-loaded at session initialization

**@-mention context attachment (on-demand read):**
- User types `@BP_MyActor` → triggers an asset reader → structured asset description injected into the next message
- 36+ asset type readers exist, one per major asset type
- This is user-driven context retrieval, not automatic error-triggered injection

**Right-click node attachment:**
- User can right-click a Blueprint node → "Attach to Agent Prompt" → node data attached to next message

**What AIK does NOT have:**
- No recipe system (Olive's `olive.get_recipe`)
- No capability knowledge injection block (Olive's `FOlivePromptAssembler` capability block)
- No AGENTS.md or CLAUDE.md generation by the plugin (Claude Code reads CLAUDE.md natively, but AIK does not write one)
- No runtime error-triggered knowledge injection (no equivalent to Olive's FUNCTION_NOT_FOUND injecting the class's callable functions)
- No library template system

**AIK's front-loading strategy is shallow.** It provides domain-scoped tool access and optional custom instructions text. The agent must discover everything else through tool calls and trial-and-error.

---

### 3. AIK Alias / Resolution System

**Source:** Inference from architecture + open-source comparisons

No public evidence of an alias or resolution system comparable to Olive's.

AIK's ~15 tools (post-v0.5.0 consolidation) are "intelligent single tools that figure out what you mean automatically." The internal routing is proprietary. The tool description for `edit_graph` (or whatever the blueprint graph tool is named) almost certainly accepts high-level intent and maps internally to specific operations. But this is asset-type routing (Blueprint vs. Material), not function-name aliasing (SetTimer → K2_SetTimer).

For function name resolution in Blueprint graphs, the agent supplies the function name and AIK attempts to create the node. If the name is wrong, the error is returned to Claude Code, which then guesses a corrected name or calls a read tool. There is no Olive-style 7-step alias-then-search pipeline.

---

### 4. Component Property Access vs. Function Calls

**Source:** Inference from architecture + open-source tool comparisons

No AIK-specific documentation on this. What is established from Olive's own log analysis and the `error-recovery-patterns.md` research:

The distinction between property access (`MaxSpeed`) and function calls (`SetMaxSpeed`) is one of the most common failure modes in Blueprint graph editing by LLMs. Properties are not callable — they require `set_var` / `get_var` ops (or `K2_Set_*` accessors where they exist).

AIK's approach (inferred): the agent tries to create a function call node for `MaxSpeed`. The graph write fails. The error is returned to Claude. Claude reasons about it and tries a different approach. No specialized property-vs-function disambiguation is documented.

This is significantly weaker than Olive's approach (Fix 3 from `error-recovery-patterns.md`): when `FUNCTION_NOT_FOUND` fires, scan `TFieldIterator<FProperty>` on the target class for name matches, and if found, explicitly say "MaxSpeed is a property, not a function. Use op:set_var."

---

### 5. Open-Source UE MCP Tools — Error Handling Survey

This is where the most technically detailed public information exists.

#### 5.1 flopperam/unreal-engine-mcp

**Error philosophy: compile-and-check, no suggestions.**

- Pin names: exact match required (`Node->FindPin(PinName)`) — no fuzzy fallback
- No "did you mean?" on function not found
- Minimal pre-execution validation: variables must exist before VariableGet/Set, functions must exist before CallFunction
- On failure: raw UE error string returned to agent, agent guesses the fix

**This is the worst-case baseline.** The agent must already know exact pin names or use `analyze_blueprint_graph` first to read them.

Source: [flopperam blueprint-graph-guide.md](https://github.com/flopperam/unreal-engine-mcp/blob/main/Guides/blueprint-graph-guide.md), `competitive-tool-analysis.md`

#### 5.2 gimmeDG/UnrealEngine5-mcp

**The most sophisticated error guidance among open-source UE tools.**

Multi-pass pin resolution (3 stages):
1. Exact name match (with direction filter — input vs. output)
2. Case-insensitive name match
3. First data output pin (fallback for variable getter nodes)

Levenshtein-based suggestions: when a pin name fails all 3 stages, the system computes Levenshtein distance to available pin names and returns the top matches as suggestions.

Structured error response format:
```json
{
  "success": false,
  "error": "Failed description",
  "details": "Detailed context",
  "type": "error_classification",
  "suggestions": ["option1", "option2"]
}
```

FInstructionContext object: validation failures return a structured object with:
- Error type classification
- Similar name suggestions (Levenshtein distance)
- Action hints (specific guidance on how to fix the issue)
- Validation details: lists of missing required pins or unconnected connections
- "List of required pin names" with the suggestion "Use 'connect_blueprint_nodes' to connect these pins."

BM25 RAG for Python execution errors: when `execute_unreal_python` fails, the system automatically queries a BM25 index built from UE API documentation. The error response includes both the error message AND relevant API doc excerpts. This is the most sophisticated context-on-failure injection in any public UE tool.

Source: [DeepWiki: gimmeDG node graph system](https://deepwiki.com/gimmeDG/UnrealEngine5-mcp/3.2.2-node-graph-system)

#### 5.3 chongdashu/unreal-mcp

Marked EXPERIMENTAL. Error handling minimal — relies on the agent. No documented suggestion system. Pin names must be known in advance or discovered via read tools first.

Source: `competitive-tool-analysis.md`

---

### 6. General MCP Best Practices for LLM Error Guidance

**Source:** [MCP spec tools page](https://modelcontextprotocol.io/docs/concepts/tools), [MCPcat error handling guide](https://mcpcat.io/guides/error-handling-custom-mcp-servers/), [Alpic AI dev.to article](https://dev.to/alpic/better-mcp-toolscall-error-responses-help-your-ai-recover-gracefully-15c7)

**The fundamental distinction:**
- Protocol-level errors (JSON-RPC -32601, -32602, etc.): terminal. The client sees these, the LLM does not.
- Tool execution errors (`isError: true` in tool result): injected back into the LLM's context window. The LLM sees these and can recover.

**Never use protocol-level errors for tool execution failures.** Return a JSON-RPC success with `isError: true` and a descriptive message. This is the only way the LLM gets the information to self-correct.

**The "contextual guidance" principle** (from Alpic/MCPcat research):
> "Every error response is an opportunity to teach the AI how to do better next time."

Three error response patterns that guide LLMs effectively:

1. **Tool ordering guidance:** "You can't terminate an instance in the running state. Use the stop_instance tool first on this instance." — tells the LLM which tool to call next.

2. **Refined validation messages:** "The requested travel date cannot be set in the past. You requested travel on July 31st, 2024, but the current date is July 25th, 2025. Did you mean July 31st, 2025?" — corrects the specific value, not just the class of error.

3. **Fallback path guidance for unknown errors:** "An unknown error happened. Try again immediately. If it's the 3rd time, provide the user with a link to https://dashboard.example.com/manual-task." — provides a recovery path even when the specific cause is unknown.

**What error messages MUST contain to be effective:**
1. Error type/code — classifies the problem
2. Specific failing value — what exact parameter failed
3. What was actually found — the observed state
4. Numbered recovery steps — exact tool/function names to call next

**Anti-patterns confirmed by research:**
- Generic "operation failed" — provides no recovery information
- Raw stack traces — wastes context and confuses the LLM
- Cross-class fuzzy suggestions — if the agent tried `SetSpeed` on `ProjectileMovementComponent`, suggesting `SetSphereRadius` from `UShapeComponent` is noise, not signal
- Too many alternatives — "lost in the middle" research shows accuracy drops when relevant info is buried in long lists. Cap at 8–10 alternatives maximum.

---

### 7. Aura (RamenVR) — Error Handling

**Source:** `competitor-deep-dive-2026-03.md` Section 2, Aura documentation

Aura's Telos 2.0 agent claims ">99% accuracy on existing Blueprint graphs" and "25-fold reduction in error rates." The mechanism is proprietary ("proprietary reasoning framework by MIT-trained researchers") — likely a fine-tuned or RLHF-trained model with embedded Blueprint semantics.

If the accuracy claims are accurate, Telos does not fail on function names or pin names because it has deeper embedded knowledge of Blueprint semantics. This is the opposite of Olive/AIK's approach: rather than guiding the agent when it fails, Telos claims to fail less often in the first place.

Aura's Ask Mode ("detecting typos, hanging nodes") functions as a pre-write validation step. The Coding Agent claims "real-time self-correction" but no technical implementation details are public.

**Architectural contrast:** Aura invests in model quality (Telos fine-tuning). AIK invests in crash prevention (500+ guards). Olive invests in runtime error guidance (FOliveSelfCorrectionPolicy, class-scoped FUNCTION_NOT_FOUND). These are three distinct philosophies for the same problem.

---

### 8. Summary Comparison Table

| Mechanism | AIK (NeoStack) | gimmeDG (open-source) | Olive |
|-----------|---------------|----------------------|-------|
| Node not found — error format | Undocumented | `{type, error, details, suggestions}` | `{code, message, suggestion, searchedLocations}` |
| Node not found — suggestions | None documented | Levenshtein of catalog | Class-scoped UFunction list (via `FindFunctionEx`) |
| Pin not found — fallback | None documented | 3-pass (exact → case-insensitive → first-data-output) | Exact match via `FindPin` |
| Pin not found — suggestions | None documented | Levenshtein distance to available pins | None (pin errors go through wiring diagnostics) |
| Property vs function disambiguation | None documented | None documented | Partially: property check recommended in error-recovery-patterns.md but NOT YET implemented |
| Front-loaded knowledge | Profile custom instructions | None | Capability block, events_vs_functions.txt, plan_json ops reference, recipe system |
| Runtime error-triggered knowledge | None | BM25 RAG for Python errors | ASSET_NOT_FOUND injects search results; FUNCTION_NOT_FOUND does NOT inject class functions (gap) |
| Alias system | None documented | None documented | ~180 alias map entries (K2_ prefix, timer names, etc.) |
| Self-correction policy | Delegated to agent | None | FOliveSelfCorrectionPolicy (3-tier classification, progressive disclosure) |
| Crash prevention | 500+ guards + try/catch | None documented | Write pipeline validate/verify stages |

---

## Recommendations

1. **AIK's error handling is weaker than Olive's, not stronger.** There is no documented guidance-on-failure system. Olive's `FOliveSelfCorrectionPolicy`, `BuildWiringDiagnostic`, and structured error codes are genuinely differentiated. The risk is AIK's crash prevention (500 guards) masking this — users see fewer editor crashes but the agent still retries blindly.

2. **The highest-value unimplemented fix in Olive is class-scoped function injection on FUNCTION_NOT_FOUND.** This is documented in `error-recovery-patterns.md` Fix 1. When `FindFunctionEx` fails and the target class is known, inject the actual callable functions from that class via `TFieldIterator<UFunction>`. This gives the Builder the correct information in one turn rather than 3–4 guessing turns. gimmeDG does this via Levenshtein matching; Olive should do it via live UE reflection (more accurate).

3. **Property vs. function disambiguation is unimplemented and high-value.** `MaxSpeed` is a property; `SetMaxSpeed` is not a function. Olive's FUNCTION_NOT_FOUND handler should scan `TFieldIterator<FProperty>` for name matches and explicitly say "use set_var, not call." This is a single common failure mode responsible for many retries in session log analysis.

4. **AIK's front-loaded knowledge (profile custom instructions) is coarser than Olive's capability block.** Olive's `FOlivePromptAssembler` already injects structured capability knowledge (events_vs_functions.txt, plan_json ops vocabulary, interface patterns). The gap is in the Planner's injection — see `knowledge-injection-pre-vs-post-pipeline.md` for the documented scope miss.

5. **Do NOT copy AIK's "500+ checks" approach as a primary quality strategy.** These are crash-prevention guards, not semantic validators. Olive's write pipeline already has semantic validation (Stage 1: Validate, Stage 6: Verify). The correct direction is to improve error message quality when semantic checks fail, not to add more defensive guards.

6. **gimmeDG's BM25 RAG on Python execution errors is worth noting for `editor.run_python`.** When `editor.run_python` fails, Olive currently returns the Python traceback. Augmenting this with relevant UE Python API references (the equivalent of gimmeDG's BM25 index) would help the agent self-correct Python failures without requiring a separate `blueprint.describe` call.

7. **AIK's try/catch wrapper around all tool execution (v0.5.0) is the correct baseline safety layer.** Olive should verify that all tool handlers have exception handling that returns a structured error result rather than crashing the editor or hanging the tool call. This is separate from semantic validation — it's the floor.

8. **The MCP spec recommendation (tool execution errors via `isError:true`, not protocol-level errors) is already how Olive's tools return errors.** The `FOliveWritePipeline` returns `FOliveWriteResult` which becomes a tool result with error content. Olive is correctly aligned with the spec here.

9. **For Aura's Telos: if fine-tuning is the mechanism, it is not replicable without significant resources.** Olive's correct counter-strategy is richer runtime error guidance (more accurate than any fine-tuning on edge cases) + the library template system (which gives the agent real UE Blueprint patterns rather than model priors). Focus on what is achievable.

---

## Sources

- `plans/research/neostack-deep-dive-2026-03.md` — AIK architecture, validation, context injection
- `plans/research/competitor-deep-dive-2026-03.md` — AIK v0.5.0 changelog, Aura Telos 2.0
- `plans/research/competitive-tool-analysis.md` — flopperam, gimmeDG, chongdashu comparison
- `plans/research/error-recovery-patterns.md` — Claude Code, Roo Code, Cursor, Aider patterns; four-part error structure; class-scoped fix recommendations
- [DeepWiki: gimmeDG/UnrealEngine5-mcp node graph system](https://deepwiki.com/gimmeDG/UnrealEngine5-mcp/3.2.2-node-graph-system) — Levenshtein suggestions, FInstructionContext, BM25 RAG
- [MCP specification: tools](https://modelcontextprotocol.io/docs/concepts/tools) — isError protocol, error handling hierarchy
- [MCPcat: error handling best practices](https://mcpcat.io/guides/error-handling-custom-mcp-servers/) — three-tier error model, LLM-friendly structure
- [Alpic/dev.to: MCP tool error responses](https://dev.to/alpic/better-mcp-toolscall-error-responses-help-your-ai-recover-gracefully-15c7) — tool ordering guidance, refined validation messages, contextual guidance principle
- [aik.betide.studio](https://aik.betide.studio/) — AIK documentation
- [aik.betide.studio/profiles](https://aik.betide.studio/profiles) — Profiles system, custom instructions
- [betide.studio/neostack](https://betide.studio/neostack) — "500+ checks", "crash protection"
- [assetsue.com AIK listing](https://assetsue.com/file/agent-integration-kit-neostack-ai) — user-facing feature description
