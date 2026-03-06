# Research: How AI Coding Tools Handle Automatic Context Injection, Template Retrieval, and RAG

## Question

How do Cursor, Windsurf, Aider, GitHub Copilot Workspace, Claude Code, and RAG-focused tools (Cody, Continue.dev) handle automatic context injection and template/example retrieval when an autonomous agent starts a task? What mechanisms ensure the agent actually USES the retrieved context? What are the best patterns to apply to Olive's library template system?

---

## Findings

### 1. Cursor — Embedding-Based Pre-Search with Manual Override

Cursor builds a local vector index of the entire codebase using an embedding model (OpenAI or custom). The indexing pipeline:

1. **Chunking**: Code is split into semantic chunks (function bodies, class definitions, logical blocks) — NOT arbitrary fixed-size windows
2. **Embedding**: Each chunk is embedded using an AI embedding model
3. **Storage**: Vectors stored in Turbopuffer (a cloud vector DB) with obfuscated file paths and line ranges. Raw code stays local.
4. **Query**: When the user types a message, the query is embedded and nearest-neighbor search fires against stored vectors
5. **Retrieval**: Semantically similar chunks are ranked and returned with file paths / line ranges, then actual code is fetched locally

**How context gets injected:** Two modes:
- **Passive**: When using Chat, Cursor automatically pre-searches the codebase and injects the top-ranked relevant files/chunks into the model context BEFORE the model generates a response. The user does not explicitly trigger this.
- **Active**: User types `@Codebase` to explicitly trigger a search with their message as the query. The model then sees a "View references" panel.

**Critical insight:** Cursor's `@Codebase` does NOT just perform a literal keyword search. It embeds the entire user intent and performs semantic matching — finding code that is *conceptually related* even if the words don't literally appear. The system also uses query enhancement via LLM rewriting to bridge the semantic gap between natural language intent and code terminology.

**`.cursor/rules/*.mdc` — Per-task static injection:** Cursor supports dynamic rule files that activate only when the AI is performing relevant tasks. This is a per-task static context injection — not dynamic retrieval, but filtered inclusion. Rule type "Always" = always injected. Rule type "Auto Attached" = injected only for relevant file types or tasks.

Source: [Cursor Codebase Indexing Docs](https://cursor.com/docs/context/codebase-indexing), [Engineer's Codex article on Cursor indexing](https://read.engineerscodex.com/p/how-cursor-indexes-codebases-fast)

---

### 2. Aider — Graph-Ranked Repo Map with Personalization

Aider's approach is fundamentally different from embedding-based search. It uses **structural analysis + graph ranking** rather than semantic embeddings.

#### How the repo map is built

1. **Tree-sitter parsing**: Every source file is parsed into an AST. Tree-sitter extracts symbol definitions (functions, classes, variables) and all references to those symbols.
2. **Dependency graph**: A directed multigraph is built where nodes are source files and edges represent "file A references a symbol defined in file B." Shared symbols create edges.
3. **PageRank with personalization**: The NetworkX `pagerank` algorithm runs on this graph with a personalization vector that heavily weights:
   - Files currently being edited: standard PageRank weight
   - Files mentioned in chat: 10x multiplier on edges from those files
   - Long descriptive identifiers (≥8 chars, snake_case / camelCase): 10x multiplier
   - References from chat files: 50x multiplier (most aggressive boost)
   - Private identifiers (starting with `_`): ÷10 penalty
   - Symbols defined in more than 5 files (too generic): ÷10 penalty

4. **Map formatting**: Top-ranked symbols are rendered as a compact "repo map" showing class/function signatures without bodies. Lines truncated to 100 chars. Full bodies of actively-edited files are excluded (model already has them).

5. **Token budget**: Binary search finds the maximum symbols that fit within `--map-tokens` (default: 1,000 tokens). Budget expands when no files are in chat and the model needs broader repo understanding.

**Injection timing:** The repo map is injected into the prompt on **EVERY request**, not just the first. It is the context for every turn. This is pre-populated context, not just-in-time retrieval.

**Critical insight:** Aider does NOT rely on the AI to search for relevant context. The repo map IS the context, built by the system and always included. The AI cannot forget to search because searching is done before the AI sees anything.

Source: [Aider repo map docs](https://aider.chat/docs/repomap.html), [Aider repomap deep dive](https://aider.chat/2023/10/22/repomap.html), [DeepWiki Aider architecture](https://deepwiki.com/Aider-AI/aider/4.1-repository-mapping)

---

### 3. Windsurf/Cascade — Real-Time Action Tracking + Whole-Codebase Indexing

Windsurf's Cascade agent takes a broader behavioral approach:

1. **Whole-codebase indexing**: Unlike some tools that only index recently-opened files, Windsurf's Indexing Engine retrieves context from the **entire codebase** regardless of what files the user has recently opened. This is the key differentiator from simpler context-window approaches.

2. **Real-time action tracking**: Cascade tracks all developer actions in real-time — file edits, terminal commands, clipboard contents, shell output — and uses this to infer intent BEFORE the user even sends a message. This means context selection is informed by what the developer has been doing, not just what they said.

3. **Automatic context gathering**: "Cascade automatically finds and loads the relevant context for your task without you having to tag files manually." — This is the full passive pre-injection model. No `@` mentions needed.

4. **@-mention for conversation context**: When cross-referencing prior conversations, Cascade retrieves summaries and relevant parts, but does NOT inject the full conversation (to avoid context window overload).

**Critical insight:** Windsurf's approach treats context selection as the system's job, not the agent's job. The agent should never need to search for relevant files — the system should have already surfaced them.

Source: [Windsurf Cascade docs](https://docs.windsurf.com/windsurf/cascade/cascade)

---

### 4. GitHub Copilot Workspace (now Copilot Coding Agent) — Explicit Spec-Plan Pipeline

Copilot Workspace's approach separates context gathering from generation through an explicit pipeline:

1. **File relevance scoring**: Uses a combination of LLM techniques AND traditional code search to identify which files are relevant to the user's task. This happens before any code is generated.
2. **Spec generation**: The highest-ranked files are injected as context. Then the model generates a "current state" + "desired state" spec. This forces the model to reason about what exists before deciding what to change.
3. **Plan generation**: Only after the spec is approved does the model generate a concrete plan (files to change, actions per file).
4. **Implementation**: Files are generated one-by-one with the spec and plan in context.

The user can inspect which files were selected for relevance ("View references" button) and override them with natural language.

**Semantic indexing**: GitHub's codebase search uses vector embeddings. "Semantic search focuses on meaning rather than matching exact words — finding authentication-related code across controllers, middleware, models, and configuration files" even when the word 'authentication' doesn't appear.

**Critical insight:** The explicit spec step forces the model to articulate "what currently exists" before acting. This prevents the AI from generating code that ignores existing patterns, because it was required to describe those patterns first.

Source: [GitHub Next Copilot Workspace](https://githubnext.com/projects/copilot-workspace), [GitHub Copilot workspace context docs](https://code.visualstudio.com/docs/copilot/workspace-context)

---

### 5. Claude Code — CLAUDE.md as Static Pre-Population + Tools for JIT Retrieval

Claude Code uses a hybrid model documented by Anthropic:

1. **CLAUDE.md static pre-population**: Files named `CLAUDE.md` at the working directory (and parent directories) are injected into every session at startup as system context. This is static — same content every run. This is the "naive drop into context upfront" approach. It is cheap (no retrieval cost) but cannot adapt to the specific task.

2. **Tool-based JIT retrieval**: `glob`, `grep`, `read`, `web_search`, and `bash` are the AI's search tools. The AI is expected to USE these tools to retrieve task-specific context just-in-time. This is the "progressive disclosure" model.

3. **`--append-system-prompt`**: For orchestrated modes, additional system context can be injected at process startup via CLI argument.

4. **Layer memory model**: Six layers of memory are loaded at session start: CLAUDE.md files (all levels), project-specific instructions, session history, tool results, and external context. The CLAUDE.md layers are always loaded; tool results accumulate as the session progresses.

**Critical insight from Anthropic's documentation:**
> "Claude Code employs this hybrid model: CLAUDE.md files are naively dropped into context up front, while primitives like glob and grep allow it to navigate its environment and retrieve files just-in-time."

The problem for Olive's autonomous mode: the agent's CLAUDE.md contains the general blueprint authoring guide but NOT task-specific context. The agent must then issue a tool call to `blueprint.list_templates` to retrieve template info. If the agent doesn't make that call (which is the bug we're solving), it never gets the templates.

Source: [HumanLayer blog: Writing a good CLAUDE.md](https://www.humanlayer.dev/blog/writing-a-good-claude-md), [Anthropic Effective Context Engineering](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents)

---

### 6. Sourcegraph Cody — Search-First RAG Architecture

Cody's architecture is explicitly "search first":
> "Cody's core philosophy is to first search the entire codebase for relevant context using its powerful code graph and semantic search before generating a response."

Key components:
1. **Repo-level Semantic Graph (RSG)**: Encapsulates the repository's global structure and dependencies. Used as the knowledge source for context retrieval.
2. **Expand and Refine retrieval**: Two-phase. Expansion phase uses graph expansion from the RSG. Refinement phase uses link prediction algorithms on the graph. This is NOT just vector search — it incorporates code structure awareness.
3. **Ranking**: Retrieval phase gathers candidates; ranking phase filters and scores them. Result is a curated set of high-relevance snippets injected into the model context.
4. **Scale**: Can handle 300,000+ repositories, monorepos exceeding 90GB. Operates entirely within enterprise network perimeter (no code sent to third parties).

Source: [Sourcegraph Cody remote repository context blog](https://sourcegraph.com/blog/how-cody-provides-remote-repository-context)

---

### 7. Continue.dev — Explicit @codebase + Automatic Repo Map

Continue.dev uses a combination of approaches:

1. **`@Codebase` provider**: Triggers embeddings-based retrieval when explicitly invoked. User types `@Codebase` to attach semantically-relevant code to the message.
2. **Automatic repo map for supported models**: "Models in the Claude 3, Llama 3.1/3.2, Gemini 1.5, and GPT-4o families will automatically use a repository map during codebase retrieval, which allows the model to understand the structure of your codebase." This is automatic, not user-triggered.
3. **Hybrid retrieval**: Combines embeddings-based search with keyword search. Both signals are merged for ranking.
4. **Custom RAG via MCP**: Continue.dev exposes a custom RAG context provider pattern via MCP servers — the exact integration pattern Olive already uses.

**Critical insight**: Continue.dev's `TextSearchProviderOptions.TextSearchBehavior.BeforeAIInvoke` configuration triggers search BEFORE the AI receives the message. This is the "search-first" pattern implemented at the framework level.

Source: [Continue.dev codebase context docs](https://docs.continue.dev/customize/deep-dives/custom-providers), [DeepWiki Continue.dev codebase indexing](https://deepwiki.com/continuedev/continue/3.4-context-providers)

---

### 8. Agentic RAG Research — What Actually Works

From academic and practitioner research (2024–2025):

#### The "lost in the middle" problem
LLMs exhibit U-shaped attention across long contexts: information at the start and end of context is better utilized than information in the middle. This means large injected context blocks may have the "most relevant" retrieved chunk buried in the middle where the model ignores it.

**Implication for Olive:** Pre-searched template results should appear near the END of the stdin message (not buried in the middle of a large context block), or near the BEGINNING of the message (right after the user's request). Both positions are better than the middle.

#### Mandatory search outperforms optional search
From agentic RAG research: a "TextSearchBehavior.BeforeAIInvoke" approach that forces retrieval before LLM invocation improves task performance by 14% (73.1% → 86.9%) versus standard optional-search RAG.

The pattern implemented in practice:
```
SYSTEM: "You MUST call 'search_child_chunks' AS THE FIRST STEP TO ANSWER THIS QUESTION."
```
This is a **system-prompt enforced forced first action** — the retrieval is not optional.

#### Context quality > context quantity
> "The optimal scenario is to present all the relevant information but nothing that is not needed."

More context is not better. Studies show model performance degrades with excess context ("context rot"), particularly after 32k–64k tokens for most production models. Well-selected small context consistently outperforms large noisy context.

**Implication for Olive:** Injecting 8 well-matched templates (~380 tokens) is better than injecting all 325 templates or a generic catalog.

#### Chunk coherence matters
> "RAG works well with well-structured natural language, but code repositories produce chunks that lack context and meaning necessary for effective indexing."

A single function extracted from a Blueprint template (like `FireArrow`) loses its context (what variables exist, what the class structure is). This is why Olive's library templates include `parent_class`, `variables`, `components`, and `interfaces` in the same JSON — preserving the structural context that makes a function snippet comprehensible.

Source: [RAGFlow 2025 RAG review](https://ragflow.io/blog/rag-review-2025-from-rag-to-context), [Databricks long-context RAG performance](https://www.databricks.com/blog/long-context-rag-performance-llms), [Simplifying RAG context windows (Medium)](https://medium.com/@levi_stringer/simplifying-rag-context-windows-with-conversation-buffers-how-to-stop-your-agent-forgetting-df2149ad7403)

---

### 9. Anthropic's Context Engineering Guidance

From Anthropic's official blog on effective context engineering for agents:

1. **Hybrid strategy is best**: Retrieve some data upfront for speed; allow agents to autonomously explore further. Neither pure pre-population nor pure JIT retrieval alone is optimal.
2. **Tools should have no functional overlap**: Confusing tool sets waste context budget as the model deliberates between similar-seeming options. This validates Olive's approach of having distinct `list_templates` (discovery) vs `get_template` (detail).
3. **Examples are the pictures worth a thousand words**: Few-shot examples injected into prompts are extremely efficient at teaching behavior patterns. Pre-searched template results act as implicit few-shot examples.
4. **Just-in-time retrieval mirrors human cognition**: Agents maintain lightweight identifiers and load full details on demand. The library template system's three-tier design (catalog → list → get) aligns with this.
5. **Progressive disclosure**: Incrementally discovering context through exploration is better than front-loading everything. Each tool call yields context that informs the next decision.

Source: [Anthropic Effective Context Engineering](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents)

---

### 10. What the Prior Olive Research Already Established

The `plans/research/autonomous-template-presearch.md` report (March 2026) already performed deep analysis of the specific implementation approach. Key confirmed findings from that report:

- **Best injection point**: `SendMessageAutonomous()` in `OliveCLIProviderBase.cpp`, replacing the generic nudge at line 514
- **Best approach (Approach A)**: Pre-search in `SendMessageAutonomous()`, inject compact result block into stdin — NOT CLAUDE.md
- **Keyword extraction**: Pass raw user message to `FOliveLibraryIndex::Tokenize()` after stripping `@` prefixes. No additional synonym expansion needed.
- **Result count**: 8 results, ~380 tokens. Negligible cost.
- **Search API**: `FOliveTemplateSystem::Get().SearchTemplates(Query, 8)` — already thread-safe from game thread
- **Show matched function names**: The most actionable signal for the agent to follow up with `get_template(..., pattern="FuncName")`
- **Fallback**: If no results (vague message), fall back to the existing generic nudge
- **Guard**: Wrap in `!IsContinuationMessage(UserMessage)` to skip pre-search on continuation runs (context already established)

That report also confirmed the core diagnosis: **the autonomous path does NOT get the template catalog** (unlike the orchestrated path which injects it via `--append-system-prompt`). The agent must discover templates via tool calls, but it consistently fails to make the right search queries because it doesn't know what's available.

---

## Synthesis: The Approaches Compared Against Olive's Problem

| Approach | Who Does the Retrieval | Pre-populated or JIT | Works Without AI Cooperation |
|---|---|---|---|
| Cursor @Codebase | System (embedding search) | Pre-populated (passive auto) | Yes — system does it |
| Aider repo map | System (graph ranking) | Pre-populated (always) | Yes — always injected |
| Windsurf Cascade | System (indexing engine) | Pre-populated (passive auto) | Yes — system does it |
| Copilot Workspace | System (LLM + code search) | Pre-populated (spec step) | Yes — forced by pipeline |
| Claude Code | AI (glob/grep/tools) | JIT (AI searches itself) | No — AI must decide to search |
| Continue.dev | Hybrid | Pre-populated + JIT | Mostly yes (auto repo map) |
| Cody | System (RSG + semantic) | Pre-populated | Yes — search-first architecture |
| **Olive (current)** | **AI (list_templates tool)** | **JIT (AI must call it)** | **No — AI consistently fails** |
| **Olive (Approach A)** | **System + AI** | **Pre-populated (system) + JIT (AI)** | **Yes for pre-search; AI can refine** |

The pattern is clear: **every successful system does the retrieval at the system level, not the AI level.** Relying on the AI to decide to search, and to use the right search terms, is the weakest design point. Olive's current approach is the outlier.

---

## Key Technical Findings Not in Prior Research

### Keyword extraction via query rewriting (Cursor pattern)
Cursor doesn't just embed the literal user message. It uses LLM-based query rewriting to expand natural language to code-relevant terms before embedding. For example, "make it explode when hit" might be rewritten to "explosion VFX particle system damage component overlap event BeginOverlap" before the embedding search fires.

**Implication for Olive**: The current tokenizer approach (split on whitespace/underscore, drop stopwords) will miss synonyms. A user asking for a "bow system" won't get tokens for "ranged", "projectile", or "arrow" unless those words appear in the message. The Approach A report acknowledges this but notes the existing inverted index makes synonym misses tolerable because domain-specific words in the message (like "bow", "arrow") still score well against the index.

If synonym expansion is needed in future, a small static synonym map (e.g., "bow" → ["ranged", "arrow", "projectile"]; "gun" → ["weapon", "fire", "shoot"]; "AI" → ["behavior", "npc", "enemy"]) could expand the query before tokenization without requiring an embedding model.

### System-prompt enforced mandatory search (from agentic RAG research)
The proven pattern for forcing agent search behavior is:
```
"You MUST call X AS THE FIRST STEP before answering."
```

In Olive's context, a stronger version of the nudge would be:
```
"REQUIRED FIRST STEP: Call blueprint.get_template(id="...", pattern="...")
on at least one of the above templates before beginning construction."
```

Framing it as "required" and naming a specific template ID (surfaced by the pre-search) makes compliance much more likely than a generic "you might want to search for templates."

### The "lost in the middle" placement rule
Research confirms: information injected at the START or END of context is better utilized than information buried in the middle.

For Olive's stdin message structure:
```
{user message}       ← AI reads this first (good)
{asset state}        ← middle (less reliably used)
{template results}   ← if placed here (end), well utilized
{nudge/instruction}  ← if placed at the very end (best for compliance)
```

The current generic nudge is already appended at the end, which is the right position. Pre-searched results should be injected just BEFORE the final nudge/instruction, keeping the imperative ("study these before building") at the very end where it is most reliably read.

### Token budget from competitive data
Competitive tools allocate context as follows:
- Aider repo map: 1,000 tokens (default, configurable)
- Continue.dev codebase retrieval: bounded by model context
- Cursor auto-context: unknown exact budget but typically 5-20 relevant files

For Olive's use case, the prior research calculated 8 template results ≈ 380 tokens. This is conservative and well within budget. The concern is NOT token cost — it is making the injected context actionable enough that the agent follows up with `get_template` calls.

---

## Recommendations

**The prior research (`autonomous-template-presearch.md`) already identified the correct implementation. This report confirms it aligns with the best practices of every major competitor system.**

1. **Implement Approach A immediately.** Pre-search `FOliveTemplateSystem::Get().SearchTemplates()` in `SendMessageAutonomous()` before calling `LaunchCLIProcess()`. The implementation sketch in `autonomous-template-presearch.md` (lines 141-196) is correct and complete.

2. **Use imperative framing in the injected block.** Do not just list templates passively. Include explicit instruction text like "Before building, call `blueprint.get_template(id=..., pattern=...)` on at least one of these." The agentic RAG research shows that "MUST call X first" patterns reliably produce first-action search behavior.

3. **Place injected templates at the END of the stdin message.** The "lost in the middle" research confirms end-position context is better utilized. The current message structure already places the nudge at the end — keep pre-searched results just before the final instruction line.

4. **Do NOT put pre-search results in CLAUDE.md.** Every major competitor distinguishes between reference context (static, in system/background files) and imperative context (dynamic, in the user message). The stdin message is the imperative channel. CLAUDE.md is reference. Template results that the agent should act on NOW belong in stdin.

5. **Synonym expansion is a future improvement, not a blocker.** The current tokenizer is adequate for domain-specific terms ("bow", "arrow", "health", "patrol"). A static synonym map could improve recall for synonyms ("gun" → "weapon", "AI" → "npc enemy behavior") but is not needed for the first implementation.

6. **Consider the Copilot Workspace "spec step" pattern for complex tasks.** For tasks that trigger Tier 2/3 confirmation (plan+confirm / preview), injecting relevant templates into the confirmation-preview step (before the agent writes graph logic) would align with the Copilot Workspace model of forcing context awareness before implementation.

7. **The "search-first" architecture insight applies to the tool description too.** The `blueprint.list_templates` tool description should use imperative language: "Search for relevant patterns BEFORE building any graph logic." If the tool description says "you can optionally search," the AI treats it as optional. If it says "search first," it treats it as a step.

8. **No new infrastructure is required.** Unlike Cursor (cloud vector DB), Aider (tree-sitter + NetworkX), and Cody (RSG), Olive's approach requires only: tokenizing the user message + running an in-memory keyword search + formatting results as markdown. All the infrastructure already exists (`FOliveLibraryIndex::Search()`, `FOliveLibraryIndex::Tokenize()`). The only new code is the call + formatting + injection in `OliveCLIProviderBase.cpp`.
