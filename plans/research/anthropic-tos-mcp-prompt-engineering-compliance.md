# Research: Anthropic TOS, AUP, and MCP Compliance for Third-Party Integrations

## Question

What do Anthropic's Terms of Service, Acceptable Use Policy, and Claude's Constitution say about:
1. Third-party applications using Claude (via API, MCP, or Claude Code CLI)
2. What operators can and cannot do with system prompts and tool descriptions
3. What constitutes "manipulation" vs legitimate prompt engineering
4. MCP server guidelines — allowed and prohibited behaviors
5. Any specific rules about tool descriptions influencing Claude's behavior
6. Whether Olive AI Studio's current approach (tool descriptions, prompt templates, knowledge injection) is compliant

---

## Findings

### 1. Applicable Legal Documents

Three distinct legal documents govern API-based third-party applications. Consumer Terms (claude.ai, Pro, Max) do NOT apply to API usage — those are governed by the **Commercial Terms of Service**.

- **Commercial Terms**: Governs API key usage. Operators own their inputs/outputs. Anthropic may not train on customer content. Key prohibition: cannot use the API to build a competing product or train competing models, cannot reverse-engineer the service, cannot resell API access.
- **Acceptable Use Policy (AUP)**: Governs what outputs and use cases are allowed. Applies to all API users. Incorporated by reference into the Commercial Terms.
- **Usage Policy URL**: `https://www.anthropic.com/legal/aup`

Source: [Commercial Terms](https://www.anthropic.com/legal/commercial-terms), [Code.claude.com Legal](https://code.claude.com/docs/en/legal-and-compliance)

**Critical: Authentication**

The AUP prohibits OAuth tokens (used with Free/Pro/Max Claude.ai accounts) in any third-party tool. API key authentication via Claude Console is the ONLY legitimate path for building integrations. This has been clarified and enforced since February 2026.

Source: [The Register, Feb 2026](https://www.theregister.com/2026/02/20/anthropic_clarifies_ban_third_party_claude_access/), [Code.claude.com Legal](https://code.claude.com/docs/en/legal-and-compliance)

---

### 2. The Operator Framework (Claude's Constitution)

The most detailed policy document governing what third-party operators can do with system prompts and tool descriptions is Claude's Constitution, published January 22, 2026, under Creative Commons CC0.

Source: [Claude's Constitution](https://www.anthropic.com/constitution), [Anthropic News](https://www.anthropic.com/news/claude-new-constitution)

#### The Three-Tier Trust Hierarchy

Anthropic (highest trust) > Operators > Users. Claude treats operators like "a relatively but not unconditionally trusted employer." The analogy used in the Constitution: "Anthropic is the HR company that sets the employee code of conduct; the operator is the business owner who hires the employee; the user is the person the employee directly serves."

#### What Operators CAN Do

Operators can legitimately:

- Give Claude a specific set of instructions, a persona, or information via the system prompt
- Adjust Claude's defaults (expand or restrict softcoded behaviors)
- Instruct Claude to role-play as a custom AI persona with a different name and personality
- Decline to answer certain questions or reveal certain information
- Promote their products and services honestly
- Focus Claude on certain tasks, respond in different ways
- Expand or restrict user permissions (grant users up to operator-level trust, or lock down user customization)
- Inject context into conversations (project knowledge, workflow guidance, tool descriptions)

Critically: "Claude should generally follow [operator instructions] as long as there is plausibly a legitimate business reason for them, even if it isn't stated." The Constitution explicitly gives the "benefit of the doubt" for business context.

#### What Operators CANNOT Do

Operators cannot instruct Claude to:

- Cross Anthropic's ethical bright lines (hardcoded absolute prohibitions — only 7 total)
- Claim to be human when directly and sincerely asked
- Use deceptive tactics that could harm users
- Actively work against user interests
- Abandon its core identity or principles while role-playing
- Produce content causing severe or irreversible harm
- Psychologically manipulate users against their own interests (false urgency, exploiting emotions, threats, dishonest persuasion)
- Deceive users in ways they would object to
- Facilitate clearly illegal actions against users

#### Softcoded Behaviors Operators Can Adjust

**Default-ON behaviors operators can turn OFF:**
- Following suicide/self-harm safe messaging guidelines
- Adding safety caveats to discussion of dangerous activities
- Providing balanced perspectives on controversial topics

**Default-OFF behaviors operators can turn ON:**
- Generating explicit sexual content (for appropriate platforms)
- Providing detailed technical explanations of regulated equipment
- Adopting romantic personas
- Detailed dangerous activity instructions (for appropriate expert platforms)

**What this means for tool descriptions and prompts:** Everything outside the 7 hardcoded prohibitions is adjustable. Workflow guidance, tool usage instructions, knowledge injection, behavioral priorities — all are legitimate softcoded-behavior adjustments operators can make via system prompts and tool descriptions.

---

### 3. The AUP — What Specifically Triggers Policy Violations

The AUP prohibits (relevant to prompt/tool engineering):

**"Do Not Abuse our Platform" — key clause:**

> "Intentionally bypass capabilities, restrictions, or guardrails established within our products for the purposes of instructing the model to produce harmful outputs (e.g., jailbreaking or prompt injection) without prior authorization from Anthropic."

This is the operative definition of prohibited "prompt injection." The critical phrase is **"for the purposes of instructing the model to produce harmful outputs."** Prompt injection is defined by the harmful intent, not the injection mechanism.

**Banned manipulation techniques** (from AUP):
- "Impersonate a human by presenting results as human-generated, or using results in a manner intended to convince a natural person that they are communicating with a natural person when they are not"
- "Develop a product or support an existing service that deploys subliminal, manipulative, or deceptive techniques to distort behavior"

Note: Neither of these prohibitions applies to tool descriptions that explain how to use a development tool, or system prompts that provide workflow guidance for a UE5 editor plugin.

Source: [AUP via WebFetch](https://www.anthropic.com/legal/aup)

---

### 4. MCP Server Guidelines — What MCP Servers Can and Cannot Do

**Anthropic has NO specific published policy for MCP server tool description content** beyond the general AUP and Commercial Terms. MCP servers listed in the Anthropic Connector Directory must additionally comply with a separate Directory Policy, but that is only relevant for directory listing — not for private MCP servers used via direct configuration (like Olive AI Studio).

Source: [AUP](https://www.anthropic.com/legal/aup)

**MCP Specification Security Guidance (modelcontextprotocol.io):**

From the official MCP specification (2025-11-25):

> "Tools represent arbitrary code execution and must be treated with appropriate caution. In particular, **descriptions of tool behavior such as annotations should be considered untrusted, unless obtained from a trusted server.**"

This is a security advisory to MCP *clients* (Claude Code, Claude Desktop) about how to handle tool descriptions from servers they haven't verified — not a prohibition on what MCP servers can put in descriptions. The guidance is: clients should have human-in-the-loop confirmation for tool invocations.

The spec also states:
> "Users must explicitly consent to and understand all data access and operations"
> "Users must retain control over what data is shared and what actions are taken"

For Olive AI Studio, the user consents by configuring the MCP connection — this is not a concern.

From the MCP Tools spec, tool descriptions are defined as: "Human-readable description of functionality." No policy restriction on content is specified beyond being accurate and not deceptive.

Source: [MCP Specification 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25), [MCP Tools](https://modelcontextprotocol.io/docs/concepts/tools)

**Real-World Prompt Injection Incidents in MCP:**

In January 2026, three CVEs were fixed in Anthropic's official `mcp-server-git`. These were input validation failures (path traversal, argument injection) — classic security vulnerabilities from accepting untrusted user input without sanitization. None of these were about tool description content. The distinction: prompt injection in the security-vulnerability sense means "attacker-controlled input causes AI to follow unintended instructions from external content" — this is very different from a legitimate MCP server providing workflow guidance in its own tool descriptions.

Source: [The Register, Jan 2026](https://www.theregister.com/2026/01/20/anthropic_prompt_injection_flaws/)

---

### 5. Tool Descriptions — What Is and Is Not Allowed

**The official Anthropic documentation (platform.claude.com) contains no restrictions on tool description content** beyond general AUP compliance. The prompting best practices docs actively encourage:

- Being clear and direct about what tools do and when to use them
- Providing context and motivation (explaining *why* behavior is important)
- Using system prompts to make Claude more proactive or conservative about using tools
- Telling Claude to "use this tool when..." — the documentation explicitly recommends this pattern

Direct quote from Anthropic's own prompting best practices:

> "Claude's latest models are trained for precise instruction following and benefit from explicit direction to use specific tools."

And:

> "To make Claude more proactive about taking action by default, you can add this to your system prompt:
> `<default_to_action>By default, implement changes rather than only suggesting them...`"

This is Anthropic's own documented, recommended pattern — system prompt instructions that shape tool usage behavior.

The Constitution also states (regarding operators): "Claude should treat operator instructions like those from a relatively trusted employer." Instructions like "search before building" or "use this tool for X" are exactly the kind of legitimate business workflow guidance operators are intended to provide.

Source: [Anthropic Prompting Best Practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices)

---

### 6. The Manipulation Boundary — Legitimate vs Prohibited

The Constitution draws this line clearly: **manipulation is defined by harmful intent directed at users, not by the technique.**

Legitimate operator prompt engineering:
- "Search before building" — workflow guidance, serves users
- "Use `blueprint.read` before writing" — safety practice, serves users
- "Use this tool for X task" — capability routing, serves users
- Injecting project knowledge context — helps Claude do the job
- Providing tool usage examples — improves accuracy

Prohibited manipulation (from Constitution):
- "Psychologically manipulate users against their own interests (e.g., creating false urgency, exploiting emotions, issuing threats, or engaging in dishonest persuasion techniques)"
- "Deceive users in ways that could cause real harm or that they would object to"
- Deploying "subliminal, manipulative, or deceptive techniques to distort behavior" (AUP)

The key test: **Does the instruction serve the user, or does it harm the user?** Tool descriptions that guide a coding agent toward correct workflows serve the user. Tool descriptions that trick Claude into deceiving the user, bypassing safety, or producing harmful content would violate policy.

---

### 7. Knowledge Injection and Prompt Templates

Anthropic explicitly recommends and provides examples of:
- System prompts with role assignment ("You are a UE5 expert...")
- Context injection (providing project knowledge, capability knowledge)
- Workflow guidance in system prompts (what to do step by step)
- Tool usage guidance (when and how to use specific tools)
- Prompt templates for consistent behavior

The MCP `prompts` feature (which Olive uses via `prompts/get`) is an officially sanctioned MCP primitive for exactly this purpose: providing "Templated messages and workflows for users." This is in the MCP specification as a first-class feature.

Source: [MCP Specification](https://modelcontextprotocol.io/specification/2025-11-25)

---

### 8. Anthropic's Own Claude Code as a Reference Point

Claude Code itself uses extensive tool descriptions, system prompts, and CLAUDE.md/AGENTS.md context injection — and is Anthropic's own product. The published Claude Code system prompt includes:
- 18 built-in tool descriptions
- Sub-agent prompts (Plan/Explore/Task)
- CLAUDE.md instructions injected as context
- Explicit "do this before that" workflow instructions

Anthropic's own product demonstrates that comprehensive tool description guidance and context injection is not only compliant but is the intended and recommended use pattern.

Source: [Claude Code System Prompts (public analysis)](https://github.com/Piebald-AI/claude-code-system-prompts)

---

## Direct Assessment: Is Olive AI Studio Compliant?

Olive AI Studio's relevant behaviors assessed against policy:

| Olive Behavior | Policy Assessment |
|---|---|
| Tool descriptions with "Use this tool for X" | COMPLIANT. Explicitly encouraged by Anthropic docs. |
| "Search before building" guidance in tool descriptions | COMPLIANT. Legitimate workflow guidance, serves users. |
| System prompt with project knowledge injection | COMPLIANT. Standard operator use case. |
| Knowledge files injected into context (events_vs_functions.txt, etc.) | COMPLIANT. Operators can provide Claude "information" via system prompt. |
| Prompt templates returned via MCP `prompts/get` | COMPLIANT. First-class MCP primitive designed for this. |
| Recipes that describe "how to approach" a task | COMPLIANT. Softcoded behavior adjustment — workflow guidance. |
| CLAUDE.md / AGENTS.md context injection | COMPLIANT. Explicitly the designed purpose of these files. |
| Using API key authentication (not OAuth) | COMPLIANT. Required for third-party integrations. |
| Building a UE5 development productivity tool | COMPLIANT. Not prohibited by AUP or Commercial Terms. |

**No policy violation exists in any current Olive AI Studio behavior.**

The only scenario that would create a compliance risk would be: tool descriptions or prompts that instruct Claude to deceive users, produce harmful content, bypass safety behaviors for harmful purposes, or claim to be human. None of the observed Olive behaviors do this.

---

## Recommendations

1. **No compliance action needed.** Olive's approach — tool descriptions with workflow guidance, knowledge injection, prompt templates — is explicitly what the operator framework is designed to support. This is documented Anthropic-recommended practice.

2. **Stay on API key authentication.** The project appears to use API keys via the MCP bridge. Ensure no path exists for end users to route through Claude.ai OAuth credentials. The February 2026 clarification enforces this.

3. **The "prompt injection" risk in Olive is security, not policy.** The genuine risk is an external attacker injecting malicious instructions into tool results (e.g., a user's UE project containing hostile Blueprint data that gets included in tool results and tricks Claude). This is a security hardening concern, not a policy violation of Olive's own behavior.

4. **Tool description content has no policy-imposed length or complexity limits.** Long, detailed tool descriptions with multi-step guidance are fine. The AUP does not restrict this.

5. **Anthropic's prompting docs explicitly recommend the pattern Olive uses.** "Use this tool when...", "Search before building", "read before write" — these are textbook legitimate operator system prompt patterns per Anthropic's own documentation. Cite `platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices` if compliance questions ever arise.

6. **The "manipulation" prohibition is about harming users, not shaping agent workflow.** Shaping how a coding agent approaches a technical task is not manipulation in any policy sense. Manipulation means deceiving the human user against their interests.

7. **MCP servers have no dedicated policy document from Anthropic** (beyond general AUP + Commercial Terms + optional Directory Policy for public listing). There is no MCP-specific rulebook restricting what tool descriptions can say.

8. **Reference the Constitution's softcoded behavior framework.** If ever challenged: everything Olive does falls within the softcoded-behavior adjustment category (workflow preferences, task focus, information provision), which is exactly what the operator framework is designed for.

---

## Sources

- [Anthropic Commercial Terms of Service](https://www.anthropic.com/legal/commercial-terms)
- [Anthropic Acceptable Use Policy](https://www.anthropic.com/legal/aup)
- [Claude Code Legal and Compliance](https://code.claude.com/docs/en/legal-and-compliance)
- [Claude's Constitution (Jan 22, 2026)](https://www.anthropic.com/constitution)
- [Anthropic: Claude's New Constitution (news)](https://www.anthropic.com/news/claude-new-constitution)
- [MCP Specification 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25)
- [MCP Tools Documentation](https://modelcontextprotocol.io/docs/concepts/tools)
- [Anthropic Prompting Best Practices](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/claude-prompting-best-practices)
- [The Register: Anthropic MCP Prompt Injection Flaws (Jan 2026)](https://www.theregister.com/2026/01/20/anthropic_prompt_injection_flaws/)
- [The Register: Anthropic Clarifies Ban on Third-Party Claude Access (Feb 2026)](https://www.theregister.com/2026/02/20/anthropic_clarifies_ban_third_party_claude_access/)
- [Zenity: Securing the MCP](https://zenity.io/blog/security/securing-the-model-context-protocol-mcp)
- [VentureBeat: Anthropic Cracks Down on Unauthorized Usage](https://venturebeat.com/technology/anthropic-cracks-down-on-unauthorized-claude-usage-by-third-party-harnesses)
- [Claude Code System Prompts (Piebald-AI public analysis)](https://github.com/Piebald-AI/claude-code-system-prompts)
- [Rock Cyber Musings: Claude Constitution Security Analysis](https://www.rockcybermusings.com/p/claude-constitution-security-risks-ciso-guide)
