# Research: Context Management Approaches for LLM Planning

## Question

How do major AI coding assistants (Roo Code, Cursor, Windsurf, Aider, Continue.dev, GitHub Copilot)
handle context management when feeding reference/template data to an LLM planner? We have library
templates with 20-40 functions each, producing 27-31K chars for 3 templates in the Architect's
context window. We need the Planner to make good architectural decisions without drowning in
function-level detail.

---

## Findings

### 1. Aider: Repo Map as a Ranked Signature Summary

Source: https://aider.chat/docs/repomap.html, https://deepwiki.com/Aider-AI/aider/4.1-repository-mapping

Aider's repo map is the closest direct analogue to our problem. It needs to summarize a whole
codebase into a token budget for a planning LLM. Key algorithm:

**What the map contains:**
- File names, class/function signatures, parameter types, and return types
- NEVER full function bodies — only the declaration line (max 100 chars, truncated)
- For files not in the active chat: structural skeleton only

**Ranking via PageRank on a dependency graph:**
- Each file is a graph node; edges are cross-file references
- PageRank with a personalization vector biased toward user-mentioned files/identifiers
- Edge weight multipliers: `mentioned_idents` x10, `chat_rel_fnames` x50, private identifiers x0.1,
  identifiers defined in >5 files (too generic) x0.1
- After ranking, distributes score to specific `(file, identifier)` pairs

**Token budget with binary search:**
- Default budget: 1k tokens for the map (configurable via `--map-tokens`)
- Binary search to find maximum number of ranked tags that fit within budget ±15%
- Sampling trick: texts ≥200 chars estimated via line sampling (N=lines//100), not exact count

**The key insight:** The map never contains the "detail tier" (function bodies). Function bodies
are fetched by the AI on demand via explicit file inclusion (`/add` command or agent tool calls).
The map exists only to let the LLM know "what exists and how it connects."

**Output format example:**
```
aider/coders/base_coder.py:
⋮...
│class Coder:
│    abs_fnames = None
⋮...
│    @classmethod
│    def create(self, main_model, edit_format, io, skip_model_availabily_check=False, **kwargs,
⋮...
│    def run(self, with_message=None):
```

One line per function, no descriptions, no tags — just the signature. The `⋮...` markers indicate
collapsed sections.

**Token cost for a 100-function codebase:** roughly 800–1500 tokens for the map.

---

### 2. Cursor: Hybrid Semantic Search + Hard Token Limits per Section

Source: https://dev.to/tawe/the-anatomy-of-a-cursor-prompt-2hb8, https://cursor.com/docs/context/codebase-indexing

**Context assembly structure (5 layers, in order):**
1. User/project rules (`.cursor/rules`)
2. System instructions + tool schemas
3. Live context (current file, semantic search results, LSP definitions)
4. Conversation history (pruned oldest-first if needed)
5. Current user query

**Semantic search mechanism:**
- Tree-sitter chunks code at logical boundaries (functions, classes) — never raw byte splits
- Each chunk embedded via OpenAI embeddings or custom model
- Query embedding compared against code embeddings in Turbopuffer vector database
- Nearest-neighbor retrieval ranked by semantic similarity
- Hybrid: semantic search PLUS regex/grep for exact symbol matches

**Token budget reality:**
- Standard mode: ~20k–30k practical working budget
- "Priompt" engine: server-side priority scoring that trims low-priority context first
  - Kept: system instructions, tool definitions, current user query
  - Trimmed first: old conversation history, low-similarity search results

**The key insight:** Cursor never pre-loads reference files. It retrieves snippets on demand and
includes only the top-N ranked chunks. Users can force include via `@file` but the default is
always pull-on-demand.

---

### 3. Roo Code: Intelligent Condensing at Threshold + Boomerang Isolation

Source: https://docs.roocode.com/features/intelligent-context-condensing, https://docs.roocode.com/features/boomerang-tasks

**Context condensing:**
- Triggers when conversation reaches a configurable threshold % of context window (default: 100%)
- Uses an LLM to summarize old conversation history
- Preserves: slash commands from first message, recent tool results
- Collapses: intermediate exploration steps, file reads, search results

**Boomerang (Orchestrator) pattern — the most relevant finding:**
- Each sub-agent operates in complete **context isolation** — it does NOT inherit the parent's
  conversation history
- Information passes **down** explicitly via the initial task instruction
- Information passes **up** via `attempt_completion` result summary — ONLY the summary returns,
  not the full sub-agent conversation
- "Prevents the parent (orchestrator) task from becoming cluttered with detailed execution steps"

**The key insight:** Roo Code's boomerang pattern is exactly what our pipeline already does
(Router → Scout → Architect → Builder), but Roo makes the critical distinction:
- Orchestrator receives **summaries** from sub-agents, not their raw output
- Sub-agents receive **targeted instructions** not the full conversation
- Each stage's context window is independent

**Token budget:** Reserves 30% of context (20% for output, 10% safety buffer), leaving 70% for
working content.

---

### 4. Windsurf Cascade: Developer Action Tracking + Automatic Context Retrieval

Source: https://docs.windsurf.com/windsurf/cascade/cascade

**Key mechanism:** Cascade tracks every file edit, terminal command, and clipboard action to build
persistent context across sessions. It does NOT pre-load files — it loads context when it detects
the task requires it.

**Context retrieval:**
- Indexing engine retrieves from the entire codebase, not just recently-opened files
- Context-aware model selection: automatically switches between fast/deep models based on
  context complexity
- For previous conversations: retrieves summaries and specific relevant checkpoints, NOT the
  full conversation history

**The key insight:** Context selection happens at retrieval time, not at load time. The system
decides "what does this task need?" not "include everything that might be relevant."

---

### 5. GitHub Copilot: Multi-Strategy Parallel Search + Tiered Indexing

Source: https://code.visualstudio.com/docs/copilot/reference/workspace-context

**Multiple parallel strategies run simultaneously:**
1. GitHub code search (for fast exact-match retrieval)
2. Local semantic search (meaning-based matching)
3. Text-based search (filename + content keyword)
4. Language intelligence (symbol resolution, cross-file go-to-definition)

**Three indexing tiers based on project size:**
- Small (<750 files): Advanced local semantic index
- Medium (750–2,500 files): Local semantic index (manual build)
- Large (2,500+ files): Basic index with simplified algorithms

**Practical token budget:**
- Small projects: include everything
- Larger projects: "only the most relevant parts are kept"
- The model receiving context never sees the selection mechanism — it just receives the result

**The key insight:** Copilot uses project SIZE to automatically determine which retrieval strategy
to apply. Smaller projects get more context; larger ones get filtered views.

---

### 6. Continue.dev: AST-Based + LSP-Powered Automatic Context

Source: https://docs.continue.dev/ide-extensions/agent/context-selection, https://docs.continue.dev/ide-extensions/autocomplete/context-selection

**Automatic context includes:**
- Code around cursor position (before and after)
- Go-to-definition targets via LSP (if you're calling a function, the function's definition)
- Import-resolved symbols (not all imports — only ones referenced near cursor)
- Recent file history

**For agent mode:**
- Tool call results are automatically included as context items (the agent sees what its tools returned)
- Uses recursive AST traversal to pull type definitions for parameters

**The key insight:** Continue never front-loads a "reference library." It pulls context based on
what the current cursor/task specifically needs.

---

### 7. Context Rot and "Lost in the Middle" — The Scientific Case for Less Context

Sources: https://research.trychroma.com/context-rot, https://aclanthology.org/2024.tacl-1.9/

**Research findings (2023–2025):**
- All 18 tested frontier models (including Claude, GPT-4) exhibit performance degradation as input
  length increases, even when the context window is not full
- The "lost in the middle" effect: LLMs strongly attend to tokens at the **beginning and end** of
  context; content in the **middle** of a long context gets disproportionately less attention
- Stanford study: with 20 retrieved documents (~4,000 tokens), accuracy drops from 70–75% down to
  55–60%
- Performance degradation is non-uniform — varies by task type, but planning tasks are especially
  vulnerable because they require attending to ALL provided context, not just one anchor point

**For Olive's specific problem:**
- Our 27-31K char template overview for 3 templates is roughly 7-9K tokens of LLM-accessible data
- The Architect's total input (template data + discovery block + user message + asset IR) can push
  to 12-18K tokens easily
- At that scale, the Planner's ability to synthesize information from the **middle** of the context
  is meaningfully degraded

**Best mitigation from the research:** Strategic placement matters. Put the most important
information at the START (system prompt) or END (most recent user turn). The discovery block
("here are templates that exist") is higher signal than function-by-function detail.

---

### 8. Anthropic on Context Engineering: Just-In-Time Retrieval

Source: https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents

**Core principle:** "The smallest set of high-signal tokens that maximize the likelihood of your
desired outcome."

**Just-in-time retrieval pattern:**
- Agents maintain lightweight identifiers (file paths, template IDs) rather than preloading content
- Data is dynamically loaded using tools at the moment it's needed
- Claude Code itself uses grep/glob to navigate files rather than loading everything upfront

**For planning specifically:**
- Examples/reference data should be "diverse and canonical" not "exhaustive"
- The planner needs to understand WHAT EXISTS, not internalize every detail
- Details belong in the execution phase (Builder), not the planning phase (Architect)

**Progressive disclosure for sub-agent architectures:**
- "Compaction": summarize and restart with condensed history
- "Structured note-taking": agents maintain NOTES.md for cross-step persistence
- "Sub-agent architectures": each agent gets 1,000–2,000 token summaries from other agents,
  not raw outputs

---

### 9. Progressive Context Enrichment Pattern (Inferable)

Source: https://www.inferable.ai/blog/posts/llm-progressive-context-encrichment

**5-step pattern:**
1. Start with minimal context sufficient to understand the task type
2. Allow the model to identify what additional information it needs
3. Provide tools enabling specific data retrieval
4. Process the current step with focused context
5. Repeat

**This is what the Builder already does** (via `blueprint.get_template`). The research question
is whether the Planner/Architect stage can apply the same principle — and the answer is yes,
with the right data structures.

**The critical distinction** from current implementation: the pattern says "minimal context to
understand task TYPE." For the Architect, this means:
- It needs to know: what templates exist, what they're called, what they do at a high level,
  what functions they expose
- It does NOT need: node counts, tag lists per function, description text per function

---

## Current Implementation Analysis

The Olive pipeline's `GetTemplateOverview()` currently outputs for each template:
- Header (template name, type, description) — ~150-200 chars
- Parent class, source project, tags — ~100 chars
- Inheritance chain — ~50-100 chars
- Variables (up to 20 names) — ~200-500 chars depending on count
- Components — ~100-200 chars
- Interfaces, dispatchers — ~100 chars
- Functions: up to 15 detailed entries, each with name + node count + tags + description

**The token breakdown for a template with 40 functions:**
- Header + metadata: ~500 chars
- 15 detailed function entries at ~100-200 chars each = 1,500-3,000 chars
- 25 remaining function names only = ~500 chars
- Total per template: **~2,500-4,000 chars**
- For 3 templates: **~7,500-12,000 chars** (roughly 2,000-3,200 tokens)

The problem is the **function detail block**. The Architect does NOT need node counts or per-function
description text to make architectural decisions. It needs:
1. "This template exists and handles X"
2. "It exposes these functions by name"
3. "You can fetch details with blueprint.get_template()"

---

## Patterns Worth Adopting

### Pattern A: Signature-Only Function Listing (Aider Repo Map Pattern)
Replace per-function detail lines with name-only listing. Remove node counts and description text
from the Planner's context. Those are implementation details for the Builder.

**Current:**
```
  - AttackHit [23 nodes] {damage, combat, hit} -- Handles damage application on hit
  - PlayAttackAnim [12 nodes] {animation, sequence} -- Plays the attack animation sequence
```

**Proposed (signature-only):**
```
Functions: AttackHit, PlayAttackAnim, StartCharge, OnChargeComplete, ...
```

Token reduction per template: from ~1,500-3,000 chars for functions to ~200-400 chars. A 5-8x
reduction in function listing token cost.

### Pattern B: Relevance-Scored Function Subset (Aider PageRank Adaptation)
Rather than "first 15 detailed, rest name-only", score functions by relevance to the user's query
and show only the top 5-8 in the detail view. Everything else collapses to names.

**Relevance scoring for functions:**
- Function name contains words from user message: high score
- Function tags overlap with user message keywords: high score
- Functions flagged as `entry_point` by the auto-tagger: always shown
- Functions with 0 nodes (stubs, not yet implemented): skip or deprioritize

This mirrors Aider's personalization vector approach.

### Pattern C: Hard Token Budget Cap per Template (Cursor Priompt Pattern)
Implement a character/token budget for the entire `TemplateOverviews` block:
- Global budget: 4,000 chars total for all template overviews (roughly 1,000 tokens)
- Per template budget: 4,000 / N templates, minimum 800 chars
- Within budget: header + metadata always included, functions truncated to fit

This is a blunt instrument but ensures the block never grows proportionally with template complexity.

### Pattern D: Two-Tier Overview Output (Tiered Reference Data)
Add a `GetTemplateHeader()` method that returns only what the Planner/Architect needs, separate
from `GetTemplateOverview()` which the Builder uses:

**Header tier (Planner/Architect use):**
```
=== BP_AbilityMelee (ActorComponent, child of BP_AbilityParent) ===
Handles melee attack cycle: damage, animation, movement, spawning.
Functions (6): AttackHit, PlayAttackAnim, StartCharge, OnChargeComplete, StopLoopSound, MeleeTrace
Use blueprint.get_template("combatfs_bp_ability_melee", pattern="FunctionName") for node graphs.
```
Target: ~300-400 chars per template.

**Detail tier (Builder use):** Current `GetTemplateOverview()` output, unchanged.

### Pattern E: Query-Matched Function Highlights (Relevance Injection)
The Scout already knows `MatchedFunctions` for each template (from `FOliveDiscoveryEntry`). Use
this to show **only matched functions** in the overview, not all 40.

The `FOliveTemplateReference` struct already captures `MatchedFunctions`. The Architect's context
could then be:
```
=== BP_AbilityMelee === [matched: AttackHit, PlayAttackAnim]
These functions are the closest match to your task. Use get_template() for full node graphs.
```

This is the most targeted approach: no wasted context on unrelated functions.

---

## Recommendations

1. **Adopt Pattern D (two-tier output) as the primary change.** Add `GetTemplateHeaderForPlanner()`
   to `FOliveLibraryIndex` — a compact 300-400 char summary with: template name, type, parent,
   one-sentence description, function names only (comma-separated, all of them), and a fetch hint.
   Use this in `RunScout`/`ExecuteCLIPath` instead of the full `GetTemplateOverview()`. Keep
   `GetTemplateOverview()` for `blueprint.get_template` tool responses (Builder use).

2. **Combine with Pattern E (matched functions highlighted).** The `MatchedFunctions` list from
   `FOliveDiscoveryEntry` is already computed by the discovery pass. Format the header as:
   `"Matched functions: AttackHit, PlayAttackAnim (+ 4 others -- fetch for full list)"`.
   This gives the Architect exactly the signal it needs: "these specific functions are relevant
   to your task."

3. **Apply a hard global budget cap (Pattern C) as a safety net.** Even with the compact header
   format, 5+ templates could still overflow. Cap `TemplateOverviews` at 6,000 chars total in the
   Scout/ExecuteCLIPath code. If the cap is reached, drop lowest-ranked templates rather than
   truncating mid-template.

4. **Keep full function detail in the Builder's path, not the Planner's.** The current
   `FormatForPromptInjection()` passes `Scout.DiscoveryBlock` and `Scout.TemplateOverviews` to the
   Builder. `DiscoveryBlock` already contains the compact discovery output. The `TemplateOverviews`
   block in the Builder's context should switch to the compact header format too — the Builder
   fetches full node data via `blueprint.get_template()`, which is exactly the progressive
   disclosure pattern Anthropic recommends.

5. **Don't put function descriptions in the planning context at all.** Research confirms the
   "lost in the middle" effect degrades planning quality when too much mid-context detail is
   present. The Architect needs to know function names to reference them in the Build Plan.
   It does not need to read 40 one-sentence descriptions to form an architectural opinion.
   Descriptions are useful for the human reading the catalog; they are noise for the Planner LLM.

6. **Future: relevance scoring for function ordering.** Aider's PageRank approach and Cursor's
   embedding similarity retrieval both show that ordering by relevance (not by declaration order)
   significantly improves planning quality. For now, the `MatchedFunctions` highlight achieves
   the same goal with zero infrastructure cost.

**Expected outcome:** Template overviews shrink from 27-31K chars (for 3 templates) to ~1,200-1,500
chars, freeing roughly 7,000-8,500 tokens for the user message, asset IR, and system prompt content
that actually drives planning quality.

---

## Sources

- [Aider Repository Map Documentation](https://aider.chat/docs/repomap.html)
- [Building a Better Repository Map with Tree-Sitter (Aider)](https://aider.chat/2023/10/22/repomap.html)
- [Aider Repository Mapping -- DeepWiki Technical Analysis](https://deepwiki.com/Aider-AI/aider/4.1-repository-mapping)
- [Roo Code Intelligent Context Condensing](https://docs.roocode.com/features/intelligent-context-condensing)
- [Roo Code Boomerang Tasks](https://docs.roocode.com/features/boomerang-tasks)
- [Cursor Codebase Indexing](https://cursor.com/docs/context/codebase-indexing)
- [The Anatomy of a Cursor Prompt](https://dev.to/tawe/the-anatomy-of-a-cursor-prompt-2hb8)
- [How Cursor Indexes Codebases Fast](https://read.engineerscodex.com/p/how-cursor-indexes-codebases-fast)
- [Windsurf Cascade Documentation](https://docs.windsurf.com/windsurf/cascade/cascade)
- [GitHub Copilot Workspace Context](https://code.visualstudio.com/docs/copilot/reference/workspace-context)
- [Continue.dev Agent Context Selection](https://docs.continue.dev/ide-extensions/agent/context-selection)
- [Context Rot Research -- Chroma](https://research.trychroma.com/context-rot)
- [Lost in the Middle: How Language Models Use Long Contexts (Liu et al., ACL 2024)](https://aclanthology.org/2024.tacl-1.9/)
- [Anthropic: Effective Context Engineering for AI Agents](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents)
- [Progressive Context Enrichment for LLMs -- Inferable](https://www.inferable.ai/blog/posts/llm-progressive-context-encrichment)
