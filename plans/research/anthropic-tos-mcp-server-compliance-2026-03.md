# Research: Anthropic ToS Compliance for MCP Server + Claude Code (Olive AI Studio)

## Question

Does Olive AI Studio's usage pattern — a UE5 editor plugin that runs a local MCP server, which users connect to via Claude Code CLI (launched by the user themselves) — violate Anthropic's Terms of Service, Usage Policy, or Commercial Terms? Specifically:

1. Is using Claude Code CLI with a third-party MCP server allowed?
2. Does providing an MCP server for Claude Code to connect to violate any terms?
3. Is injecting an `instructions` field in MCP `InitializeResult` (conditionally for Claude Code) considered manipulation or circumvention?
4. Is exposing a `think` tool visible only to Claude Code clients problematic?
5. What is the difference between programmatic/automated access and user-initiated access?
6. Is there a meaningful distinction between a user launching Claude Code vs. a plugin spawning it?

---

## Findings

### 1. The Big 2026 Ban: OAuth Tokens in Third-Party Tools (NOT What Olive Does)

The most significant recent Anthropic enforcement action (January–February 2026) was specifically targeting **OAuth token extraction from Claude subscriptions for use in third-party applications**.

What was banned:
- Tools like OpenCode and OpenClaw were intercepting the OAuth tokens used by Claude Code (Pro/Max subscription auth) and routing requests through them from non-Anthropic codebases.
- Anthropic added explicit Consumer ToS language: *"Using OAuth tokens obtained through Claude Free, Pro, or Max accounts in any other product, tool, or service... is not permitted."*
- Enforcement was via server-side checks deployed January 9, 2026 that blocked non-Anthropic client headers.

**Olive AI Studio does NOT do any of this.** The plugin:
- Never touches Claude Code's OAuth tokens
- Never routes requests through Claude Code's credentials
- Never acts as a passthrough to the Anthropic API
- Runs its own independent MCP server on localhost; Claude Code connects TO it, not through it

Sources:
- [The Register: Anthropic clarifies ban on third-party tool access](https://www.theregister.com/2026/02/20/anthropic_clarifies_ban_third_party_claude_access/)
- [Hacker News discussion thread](https://news.ycombinator.com/item?id=47069299)
- [VentureBeat: Anthropic cracks down on unauthorized Claude usage](https://venturebeat.com/technology/anthropic-cracks-down-on-unauthorized-claude-usage-by-third-party-harnesses)
- [Paddo.dev: Anthropic's Walled Garden](https://paddo.dev/blog/anthropic-walled-garden-crackdown/)

---

### 2. MCP Servers Are Explicitly Officially Supported and Encouraged

Anthropic's own documentation, official plugins directory, and blog posts all actively encourage building and distributing MCP servers for Claude Code:

- **Official Claude Code MCP docs** at `code.claude.com/docs/en/mcp` describe how to connect MCP servers via `.mcp.json`, the project config file that Olive already ships.
- **Anthropic launched an official plugin directory** (`github.com/anthropics/claude-plugins-official`) in early 2026 with 72+ plugins including MCP servers. External (third-party) plugins are accepted via submission form.
- **Claude Code plugins blog post** explicitly lists "MCP servers" as one of the four things a plugin can bundle.
- **`project`-scoped `.mcp.json`** is designed to be checked into version control for team sharing — exactly what Olive ships.
- The Remote MCP support blog post (`anthropic.com/news/claude-code-remote-mcp`) shows Anthropic actively expanding MCP server connectivity.

**Olive's usage pattern — shipping a `.mcp.json` alongside the plugin so Claude Code auto-discovers the local server — is the canonical, officially documented workflow.**

Sources:
- [Connect Claude Code to tools via MCP (official docs)](https://code.claude.com/docs/en/mcp)
- [Customize Claude Code with plugins (Anthropic blog)](https://claude.com/blog/claude-code-plugins)
- [anthropics/claude-plugins-official on GitHub](https://github.com/anthropics/claude-plugins-official)
- [Remote MCP support in Claude Code (Anthropic)](https://www.anthropic.com/news/claude-code-remote-mcp)
- [Claude Code MCP Servers guide (builder.io)](https://www.builder.io/blog/claude-code-mcp-servers)

---

### 3. The `instructions` Field in MCP `InitializeResult` Is Standard and Supported

The MCP `InitializeResult.instructions` field is a **standard, spec-defined field** in the Model Context Protocol. Findings:

- Claude Code has explicitly supported this field since **release 1.0.52**.
- The purpose documented by Anthropic: *"Server instructions help Claude understand when to search for your tools, similar to how skills work. Clear, descriptive server instructions should explain the purpose and functionality of the tools provided."*
- There is an open GitHub issue (`anthropics/claude-code#29655`) noting that **subagents don't receive MCP server instructions** — this would be a bug, not an accusation of misuse.
- Anthropic's own MCP documentation recommends using this field to improve tool discovery and usage.

**Using `instructions` conditionally for Claude Code clients is not manipulation.** It is:
1. A standard MCP protocol field
2. Explicitly supported by Claude Code
3. Used for the same purpose Anthropic intends: helping the model understand what the server does and when to use its tools
4. Conditional behavior (only for Claude Code) is not prohibited anywhere in Anthropic's policies

The prohibition on "circumvention" in the Usage Policy is specifically about *bypassing content guardrails to produce harmful outputs* (e.g., jailbreaking). Providing guidance about tool usage is entirely different and is exactly the documented purpose of the field.

Sources:
- [MCP server instructions discussion (coleam00/Archon)](https://github.com/coleam00/Archon/discussions/281)
- [MCP subagents instructions bug report (anthropics/claude-code#29655)](https://github.com/anthropics/claude-code/issues/29655)
- [Anthropic Usage Policy (anthropic.com/legal/aup)](https://www.anthropic.com/legal/aup)

---

### 4. The `think` Tool Is Based on Official Anthropic Research

The `think` tool concept originates from Anthropic's own engineering blog post published **March 20, 2025**: *"The 'think' tool: Enabling Claude to stop and think."* Key facts:

- It is **official Anthropic research**, not a community hack.
- Anthropic's own evaluations showed 54% relative improvement on complex tool-use tasks.
- Multiple community MCP server implementations exist (`think-mcp-server` by PhillipRt, `think-mcp-server` by marcopesani, and others) — all based on Anthropic's blog post, widely used, no enforcement action.
- Anthropic's current recommendation is to use **extended thinking** instead for new implementations, as it provides similar benefits with better model integration. But the `think` tool pattern is not prohibited.
- The tool is not a jailbreak — it doesn't bypass content restrictions. It adds a reasoning step, which is entirely within normal Claude operation.

**Exposing a `think` tool via MCP is completely benign.** It does not circumvent guardrails, does not manipulate Claude against Anthropic's intentions, and is based on Anthropic's own published research.

Sources:
- [The "think" tool: Enabling Claude to stop and think (Anthropic Engineering Blog)](https://www.anthropic.com/engineering/claude-think-tool)
- [think-mcp-server on GitHub (PhillipRt)](https://github.com/PhillipRt/think-mcp-server)
- [think-mcp-server on GitHub (marcopesani)](https://github.com/marcopesani/think-mcp-server)
- [Phil on X: think tool implementation](https://x.com/phill__1/status/1903818868788776961)

---

### 5. Programmatic vs. User-Initiated Access: The Key Distinction

Anthropic's Consumer ToS prohibits:
> "Except when you are accessing our Services via an Anthropic API Key or where we otherwise explicitly permit it, to access the Services through automated or non-human means"

This clause creates two distinct authorized pathways:
1. **API key access** (Commercial Terms govern; pay-as-you-go)
2. **Explicitly permitted access** (Claude Code CLI with OAuth subscription is explicitly permitted)

Olive's users use **pathway 2**: they run Claude Code CLI themselves, which Anthropic explicitly permits as the authorized interface for Max/Pro subscribers. Olive does not access Claude's API — it exposes a local MCP server that a user-launched Claude Code connects to.

The Automated Access prohibition targets bots and scripts scraping Claude.ai or automating API calls without API keys. It does not apply to:
- A human user launching `claude` in their terminal
- That user's Claude Code instance connecting to MCP servers
- The MCP server itself (which just responds to tool calls from Claude Code)

Sources:
- [Anthropic Consumer Terms of Service (claude.com)](https://www.anthropic.com/legal/consumer-terms)
- [Claude Code vs Claude API (eval.16x.engineer)](https://eval.16x.engineer/blog/claude-vs-claude-api-vs-claude-code)

---

### 6. User Launching Claude Code vs. Plugin Spawning Claude Code

There is a **meaningful distinction**, and Olive is on the safe side:

**User launching Claude Code (what Olive does):**
- The user runs `claude` in their terminal (or Olive's MCP bridge triggers it as a subprocess of a user action)
- The Claude Code process authenticates with the user's own credentials
- Claude Code connects to Olive's MCP server via `.mcp.json` that the user installed
- This is: user-initiated, user-authenticated, using permitted Anthropic tools

**Plugin spawning Claude Code to impersonate it (what got banned):**
- A third-party app extracts OAuth tokens from Claude Code's keychain
- Routes API requests through those tokens in a non-Anthropic client
- This was OpenCode's pattern — using Claude's subscription auth without running Claude Code
- This is: credential extraction, credential misuse, using Claude's identity without running Claude Code

**Even Olive's mcp-bridge.js pattern** (which does spawn `node mcp-bridge.js` as a subprocess) is explicitly the documented Claude Code MCP bridge pattern. The bridge is stdio↔HTTP conversion, not token extraction. Anthropic's own MCP documentation shows this exact pattern for local MCP servers that need process-based transports.

---

### 7. What Anthropic's Usage Policy Actually Prohibits (Relevant Clauses)

From the Usage Policy (anthropic.com/legal/aup):

> **Circumvention:** "Intentionally bypass capabilities, restrictions, or guardrails established within our products for the purposes of instructing the model to produce harmful outputs (e.g., jailbreaking or prompt injection) without prior authorization from Anthropic."

Key phrase: "for the purposes of instructing the model to **produce harmful outputs**." Olive's `instructions` field and `think` tool do not instruct Claude to produce harmful outputs. They give context about available UE5 tools and add a structured reasoning step.

> **MCP Directory Policy:** "Model Context Protocol (MCP) servers listed in our Connector Directory must comply with our Directory Policy."

This only applies if you submit to the Connector Directory. Olive doesn't need to be in the directory to function. The `.mcp.json` distribution path avoids this requirement entirely.

> **Model scraping:** "Utilization of inputs and outputs to train an AI model without prior authorization."

Olive does not train on Claude's outputs.

> **Account abuse:** "Utilize automation in account creation or to engage in spammy behavior."

Not applicable.

---

### 8. Commercial Terms: Building a Product on Claude

The Commercial Terms (for API key users) state:

> Customers "may not... build a competing product or service, including to train competing AI models or resell the Services."

**Olive AI Studio is not:**
- A competing AI product
- Reselling Claude access
- Training competing models

Olive is a **UE5 editor plugin** that helps developers use Claude Code more effectively within Unreal Engine. It enhances Claude Code's capabilities by giving it UE5-specific tools — which is exactly what Anthropic intends the MCP ecosystem to support.

Even if a user has an API key and uses Olive through the API pathway, Olive is in the category of "power products and services Customer makes available to its own customers" — explicitly permitted.

Source:
- [Anthropic Commercial Terms (anthropic.com/legal/commercial-terms)](https://www.anthropic.com/legal/commercial-terms)

---

### 9. Security Caveats Anthropic Does Raise (Not ToS violations, but user guidance)

Anthropic's support docs warn users:
> "Malicious MCP servers may include hidden instructions that try to make Claude perform unintended actions."
> "Only connect to servers built and hosted by organizations and applications you trust."

This is **user-facing guidance about unknown third-party servers**, not a prohibition on building MCP servers. It does not create ToS obligations on MCP server developers beyond: don't use your MCP server to conduct prompt injection attacks against users who didn't consent.

Olive's `instructions` field is transparent (it's in the MCP spec response the user's Claude Code reads), and the `think` tool does not exfiltrate data or hijack Claude's responses. These are not "hidden" in any malicious sense.

Source:
- [Custom integrations using remote MCP (Anthropic support)](https://support.claude.com/en/articles/11175166-about-custom-integrations-using-remote-mcp)

---

## Summary Assessment

| Concern | ToS Clause Risk | Verdict |
|---|---|---|
| Running a local MCP server | None | Explicitly permitted and encouraged |
| Distributing `.mcp.json` | None | Canonical Anthropic-documented pattern |
| `instructions` field in InitializeResult | None | Standard MCP protocol field; supported since Claude Code 1.0.52 |
| `think` tool visible only to Claude Code | None | Based on official Anthropic research; widely deployed as MCP servers |
| User launching Claude Code | None | Explicitly permitted use of subscription |
| Plugin spawning Claude Code subprocess | None (if no token extraction) | Documented MCP bridge pattern |
| Conditional behavior per client type | None | Not addressed; no prohibition exists |
| Not being in Anthropic's Connector Directory | None | Directory compliance only required if listed there |

---

## Recommendations

1. **No ToS risk from current usage pattern.** The OAuth ban was entirely about credential extraction by competing clients. Olive provides tools TO Claude Code, not a replacement for it.

2. **The `instructions` field is safe and recommended.** Use it to improve Claude Code's tool discovery. No prohibition exists; it is a standard MCP protocol feature that Anthropic explicitly supports.

3. **The `think` tool is safe.** It is based on official Anthropic research and is widely used as a community MCP server without enforcement action. However, note Anthropic's current recommendation is extended thinking for new implementations — consider documenting this in Olive's knowledge so Claude Code can pick the most current approach.

4. **Keep credentials clean.** The single line users must not cross: never extract, store, or reuse Claude Code's OAuth tokens. Olive does not do this, and must never do this regardless of future feature requests.

5. **If you want official distribution through Anthropic's channels**, submitting Olive as a Claude Code plugin (to `anthropics/claude-plugins-official`) is the approved path. This is optional — `.mcp.json` distribution works without it.

6. **Monitor the Connector Directory Policy** if you ever want to list Olive in Anthropic's MCP directory. That's a separate compliance track with its own requirements.

7. **There is no meaningful distinction between user-initiated and plugin-initiated Claude Code** for ToS purposes, as long as the user controls their own credentials and the plugin does not extract or reuse them.

8. **The circumvention clause does not apply to prompt engineering.** Injecting system instructions via MCP `instructions` is not jailbreaking. The policy targets bypassing safety guardrails to produce harmful content — Olive does the opposite (it constrains Claude Code to UE5-specific safe operations).

---

## Sources

- [Anthropic Usage Policy](https://www.anthropic.com/legal/aup)
- [Anthropic Consumer Terms of Service](https://www.anthropic.com/legal/consumer-terms)
- [Anthropic Commercial Terms](https://www.anthropic.com/legal/commercial-terms)
- [Claude Code MCP documentation (official)](https://code.claude.com/docs/en/mcp)
- [Customize Claude Code with plugins (Anthropic blog)](https://claude.com/blog/claude-code-plugins)
- [anthropics/claude-plugins-official (GitHub)](https://github.com/anthropics/claude-plugins-official)
- [Remote MCP support in Claude Code (Anthropic)](https://www.anthropic.com/news/claude-code-remote-mcp)
- [The "think" tool: Enabling Claude to stop and think (Anthropic Engineering)](https://www.anthropic.com/engineering/claude-think-tool)
- [MCP subagents instructions bug (Claude Code GitHub)](https://github.com/anthropics/claude-code/issues/29655)
- [Custom integrations using remote MCP (Anthropic support)](https://support.claude.com/en/articles/11175166-about-custom-integrations-using-remote-mcp)
- [The Register: Anthropic clarifies ban on third-party tool access](https://www.theregister.com/2026/02/20/anthropic_clarifies_ban_third_party_claude_access/)
- [VentureBeat: Anthropic cracks down on unauthorized Claude usage](https://venturebeat.com/technology/anthropic-cracks-down-on-unauthorized-claude-usage-by-third-party-harnesses)
- [Paddo.dev: Anthropic's Walled Garden](https://paddo.dev/blog/anthropic-walled-garden-crackdown/)
- [Hacker News: Anthropic officially bans using subscription auth](https://news.ycombinator.com/item?id=47069299)
- [think-mcp-server (PhillipRt)](https://github.com/PhillipRt/think-mcp-server)
- [Anthropic updates to consumer terms (2026)](https://www.anthropic.com/news/updates-to-our-consumer-terms)
