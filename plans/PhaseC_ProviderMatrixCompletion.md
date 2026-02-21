# Phase C — Provider Matrix Completion

## Detailed Architectural Plan

Phase C turns provider support from "some paths work" into a complete, production-ready BYOK matrix. Every provider option shown in settings must work end-to-end. No dead UI options, no "not implemented" runtime failures.

---

## Current State Analysis

### What exists today

| Provider | Header | Impl | Streaming | Tool Calls | Factory Entry |
|----------|--------|------|-----------|------------|---------------|
| OpenRouter | ✅ | ✅ Full SSE streaming + tool calls | ✅ | ✅ | ✅ |
| Anthropic | ✅ | ✅ Non-streaming Messages API + tool_use blocks | ❌ | ✅ | ✅ |
| Claude Code CLI | ✅ | ✅ Process-based, pipes | ✅ | ✅ | ✅ |
| OpenAI | ❌ | ❌ | — | — | ❌ `"not implemented yet"` |
| Google | ❌ | ❌ | — | — | ❌ `"not implemented yet"` |
| Ollama | ❌ | ❌ | — | — | ❌ `"not implemented yet"` |

### Key files

| File | Role |
|------|------|
| `Public/Providers/IOliveAIProvider.h` | Interface + factory + shared structs |
| `Private/Providers/IOliveAIProvider.cpp` | Factory routing, `FOliveChatMessage::ToJson` |
| `Public/Settings/OliveAISettings.h` | `EOliveAIProvider` enum, per-provider keys |
| `Private/Settings/OliveAISettings.cpp` | `GetCurrentApiKey()`, `GetCurrentBaseUrl()`, `IsProviderConfigured()` |
| `Private/UI/SOliveAIChatPanel.cpp` | `ConfigureProviderFromSettings()` — the switch that returns "not implemented" |
| `Public/Providers/OliveOpenRouterProvider.h/.cpp` | Reference implementation — full SSE streaming + tool parsing |

### Architectural invariants (do not change)

- `IOliveAIProvider` interface is stable — all new providers implement it identically
- `FOliveProviderFactory::CreateProvider(FString)` is the single creation point
- `FOliveProviderConfig` carries all config for any provider
- `FOliveStreamChunk` is the unified stream output
- `FOliveChatMessage::ToJson()` emits OpenAI-format messages (used by OpenRouter)
- Provider is hot-swappable via `ConversationManager->SetProvider()`
- `SOliveAIChatPanel::ConfigureProviderFromSettings()` maps enum → provider name → factory

---

## What Gets Built

### Task 1 — OpenAI Direct Provider

**Files:**
- `Public/Providers/OliveOpenAIProvider.h`
- `Private/Providers/OliveOpenAIProvider.cpp`

**API:** OpenAI Chat Completions (`https://api.openai.com/v1/chat/completions`)

**Key decisions:**
- OpenAI's API is nearly identical to OpenRouter's (same SSE format, same tool_calls delta format)
- **Reuse pattern:** Copy the OpenRouter streaming/SSE/tool-call-delta parsing logic — it's the same wire format
- Headers: `Authorization: Bearer <key>`, `Content-Type: application/json`
- Tool format: OpenAI function calling (same as OpenRouter — `type: "function"`, `function: {name, parameters}`)
- Models list: `gpt-4o`, `gpt-4o-mini`, `gpt-4-turbo`, `o1`, `o1-mini`
- Default model: `gpt-4o`
- `ValidateConfig`: key required, key must start with `sk-` (basic sanity)

**Streaming:** Full SSE like OpenRouter. Same `data: {...}` / `data: [DONE]` format. Same `choices[0].delta.content` and `choices[0].delta.tool_calls` structure.

**Error handling:**
- 401: "Invalid OpenAI API key. Check your key at platform.openai.com."
- 429: Rate limit with Retry-After
- 403: "Organization/project access denied"
- 400 with `model_not_found`: "Model not available. Check model name."

---

### Task 2 — Google Gemini Provider

**Files:**
- `Public/Providers/OliveGoogleProvider.h`
- `Private/Providers/OliveGoogleProvider.cpp`

**API:** Google Generative Language API (`https://generativelanguage.googleapis.com/v1beta/models/{model}:streamGenerateContent?key={key}&alt=sse`)

**Key decisions:**
- Google uses a **different message format** than OpenAI:
  - `contents[]` instead of `messages[]`
  - Roles: `user`, `model` (not `assistant`)
  - No `system` role — use `systemInstruction` top-level field
  - Tool results use `functionResponse` parts, not separate tool messages
- **Tool calling format differs:**
  - Request: `tools: [{ functionDeclarations: [{ name, description, parameters }] }]`
  - Response: `functionCall` parts with `{ name, args }`
  - Tool results: `functionResponse` parts with `{ name, response }`
- API key goes in URL query param `?key=`, not header

**Streaming:** Google SSE format — each event is a JSON object with `candidates[0].content.parts[]`. Parts can be `text` or `functionCall`.

**Message conversion:** Must convert `FOliveChatMessage` array → Google `contents[]` format:
- System messages → `systemInstruction` field
- User messages → `{ role: "user", parts: [{ text: "..." }] }`
- Assistant messages → `{ role: "model", parts: [{ text: "..." }] }`
- Tool call messages → `{ role: "model", parts: [{ functionCall: { name, args } }] }`
- Tool result messages → `{ role: "user", parts: [{ functionResponse: { name, response: { content } } }] }`

**Models:** `gemini-2.0-flash`, `gemini-2.0-flash-lite`, `gemini-1.5-pro`, `gemini-1.5-flash`

**Error handling:**
- 400: "Invalid request — check model name"
- 403: "API key invalid or quota exceeded"
- 429: Rate limit
- Special: Google returns `SAFETY` finish reason for blocked content

---

### Task 3 — Ollama Provider

**Files:**
- `Public/Providers/OliveOllamaProvider.h`
- `Private/Providers/OliveOllamaProvider.cpp`

**API:** Ollama OpenAI-compatible endpoint (`http://localhost:11434/v1/chat/completions`)

**Key decisions:**
- Ollama v0.5+ exposes an **OpenAI-compatible** chat completions endpoint
- Same SSE streaming format as OpenAI
- No API key required (local only) — `ValidateConfig` checks URL reachability
- Tool support: Ollama supports function calling for some models (llama3.1+, mistral, etc.) but not all
- Must handle tool call gracefully when model doesn't support it (no tool_calls in response, just text)

**Special handling:**
- Connection validation: HTTP HEAD/GET to `{baseUrl}/api/tags` to check Ollama is running
- Model validation: Check if model is pulled via `{baseUrl}/api/tags` response
- Error if daemon not running: "Ollama is not running. Start it with `ollama serve`."
- Error if model not pulled: "Model '{model}' not found. Pull it with `ollama pull {model}`."

**Models:** Dynamic from `/api/tags` endpoint, but provide defaults: `llama3.1`, `codellama`, `mistral`, `deepseek-coder`

---

### Task 4 — OpenAI-Compatible Provider (Extensible Base)

**Files:**
- `Public/Providers/OliveOpenAICompatibleProvider.h`
- `Private/Providers/OliveOpenAICompatibleProvider.cpp`

**Purpose:** A generic provider for any OpenAI-compatible API endpoint (LM Studio, vLLM, Together AI, Groq, Fireworks, etc.)

**Key decisions:**
- User provides: base URL + optional API key + model name
- Uses the exact same wire format as OpenAI (SSE streaming, tool_calls)
- `ValidateConfig`: base URL required, model required, key optional
- No hardcoded model list — user types the model name

**Settings additions:**
- Add `EOliveAIProvider::OpenAICompatible` to the enum
- Add `OpenAICompatibleUrl` and `OpenAICompatibleApiKey` fields to settings
- Display name: "OpenAI Compatible (Custom Endpoint)"

---

### Task 5 — Factory & Settings Wiring

**Files modified:**
- `Private/Providers/IOliveAIProvider.cpp` — Add factory entries for new providers
- `Public/Settings/OliveAISettings.h` — Add `OpenAICompatible` enum entry + settings fields
- `Private/Settings/OliveAISettings.cpp` — Update `GetCurrentApiKey()`, `GetCurrentBaseUrl()`, `IsProviderConfigured()`
- `Private/UI/SOliveAIChatPanel.cpp` — Remove all "not implemented" paths from `ConfigureProviderFromSettings()`

**Factory updates in `CreateProvider()`:**
```
"openai"              → FOliveOpenAIProvider
"google"              → FOliveGoogleProvider
"ollama"              → FOliveOllamaProvider
"openai_compatible"   → FOliveOpenAICompatibleProvider
```

**Settings enum becomes:**
```cpp
UENUM(BlueprintType)
enum class EOliveAIProvider : uint8
{
    ClaudeCode       UMETA(DisplayName = "Claude Code CLI (No API Key)"),
    OpenRouter       UMETA(DisplayName = "OpenRouter (API Key)"),
    Anthropic        UMETA(DisplayName = "Anthropic (API Key)"),
    OpenAI           UMETA(DisplayName = "OpenAI (API Key)"),
    Google           UMETA(DisplayName = "Google AI (API Key)"),
    Ollama           UMETA(DisplayName = "Ollama (Local)"),
    OpenAICompatible UMETA(DisplayName = "OpenAI Compatible (Custom Endpoint)")
};
```

**`ConfigureProviderFromSettings()` update:** Remove every `OutError = TEXT("... not implemented yet")` path. Each enum value maps to its provider name and goes through the factory.

---

### Task 6 — Anthropic Streaming Upgrade

**Files modified:**
- `Private/Providers/OliveAnthropicProvider.cpp`
- `Public/Providers/OliveAnthropicProvider.h`

**What changes:**
The Anthropic provider currently uses `"stream": false` (non-streaming). Upgrade to SSE streaming for real-time token output.

- Set `"stream": true` in request body
- Add `OnRequestProgress64` handler like OpenRouter
- Parse Anthropic SSE events:
  - `event: message_start` — contains model info
  - `event: content_block_start` — type `text` or `tool_use`
  - `event: content_block_delta` — `text_delta.text` or `input_json_delta.partial_json`
  - `event: content_block_stop` — finalize block
  - `event: message_delta` — finish reason, usage
  - `event: message_stop` — done
- Add streaming state: `SSEBuffer`, `PendingToolCalls`, `CurrentUsage` (like OpenRouter)
- Add `ProcessSSEData`, `ProcessSSELine`, `ParseStreamEvent` methods

**Anthropic SSE format differs from OpenAI:**
- Each line has `event: <type>\n` followed by `data: <json>\n\n`
- Must track current `event` type to interpret `data` correctly
- Tool call arguments arrive as incremental JSON string chunks in `input_json_delta`

---

### Task 7 — Connection Validation & Provider-Specific Error Hints

**Files modified:**
- `IOliveAIProvider.h` — Add `virtual void ValidateConnection(TFunction<void(bool, FString)> Callback) const;`
- Each provider `.cpp` — Implement connection check
- `SOliveAIChatPanel.cpp` — Call validation on provider switch, show results

**What gets added to the interface:**
```cpp
/** Async connection validation. Callback receives (bSuccess, ErrorOrSuccessMessage). */
virtual void ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const;
```

**Per-provider validation:**

| Provider | Validation |
|----------|-----------|
| OpenRouter | GET `https://openrouter.ai/api/v1/auth/key` with Bearer token → check key validity |
| Anthropic | POST to messages endpoint with minimal prompt → check 200 vs error |
| OpenAI | GET `https://api.openai.com/v1/models` → check key works, model exists |
| Google | GET `https://generativelanguage.googleapis.com/v1beta/models?key={key}` → check key, list models |
| Ollama | GET `{baseUrl}/api/tags` → check daemon running, model exists |
| OpenAI Compatible | GET/HEAD `{baseUrl}/v1/models` → check endpoint reachable |
| Claude Code | Check `IsClaudeCodeInstalled()` + run `claude --version` |

**UI integration in chat panel:**
- On provider change: fire async validation
- Show status: spinner → green check / red X with message
- Error messages are provider-specific and actionable (include URLs, commands to run)

---

## Implementation Order

Tasks should be implemented in this exact order:

1. **Task 1 — OpenAI Provider** (simplest, same wire format as OpenRouter)
2. **Task 5 — Factory & Settings Wiring** (wire OpenAI into factory, remove one "not implemented")
3. **Task 3 — Ollama Provider** (OpenAI-compatible, adds local model support)
4. **Task 4 — OpenAI-Compatible Provider** (generic version, enables many endpoints)
5. **Task 2 — Google Provider** (hardest — different message format, different SSE)
6. **Task 6 — Anthropic Streaming Upgrade** (upgrade existing, risk of regression)
7. **Task 7 — Connection Validation** (cross-cutting, touches all providers)

**Rationale:** Start with providers closest to existing patterns (OpenAI = same as OpenRouter). Wire factory early so each new provider is immediately testable. Google is hardest due to format differences. Anthropic streaming is an upgrade (not new code), so do it after new providers are stable. Connection validation is the final polish.

---

## File Structure Summary

```
Source/OliveAIEditor/
├── Public/Providers/
│   ├── IOliveAIProvider.h           (modified: add ValidateConnection)
│   ├── OliveOpenRouterProvider.h    (existing, unchanged)
│   ├── OliveAnthropicProvider.h     (modified: add streaming state)
│   ├── OliveClaudeCodeProvider.h    (existing, unchanged)
│   ├── OliveOpenAIProvider.h        (NEW)
│   ├── OliveGoogleProvider.h        (NEW)
│   ├── OliveOllamaProvider.h        (NEW)
│   └── OliveOpenAICompatibleProvider.h (NEW)
│
├── Private/Providers/
│   ├── IOliveAIProvider.cpp         (modified: factory entries)
│   ├── OliveOpenRouterProvider.cpp  (existing, unchanged)
│   ├── OliveAnthropicProvider.cpp   (modified: streaming upgrade)
│   ├── OliveClaudeCodeProvider.cpp  (existing, unchanged)
│   ├── OliveOpenAIProvider.cpp      (NEW)
│   ├── OliveGoogleProvider.cpp      (NEW)
│   ├── OliveOllamaProvider.cpp      (NEW)
│   └── OliveOpenAICompatibleProvider.cpp (NEW)
│
├── Public/Settings/
│   └── OliveAISettings.h           (modified: add OpenAICompatible enum + fields)
│
├── Private/Settings/
│   └── OliveAISettings.cpp         (modified: update switch statements)
│
└── Private/UI/
    └── SOliveAIChatPanel.cpp        (modified: remove "not implemented", add validation UI)
```

---

## Edge Cases Each Provider Must Handle

| Edge Case | Handling |
|-----------|----------|
| Missing API key | `ValidateConfig` returns false with "API key required. Get one at {url}" |
| Invalid API key (401) | Provider-specific message: "Invalid {provider} API key" |
| Rate limit (429) | Parse Retry-After header, show "Rate limited, retry in X seconds" |
| Model not found | "Model '{model}' not available for {provider}" |
| Network timeout | "Request timed out after {N} seconds" |
| Connection refused | "Cannot connect to {url}. Check your internet / endpoint URL." |
| Streaming disconnect | Finalize any partial response, report error |
| No tool support | Return text-only response, log warning "Model X does not support tool calling" |
| Ollama daemon down | "Ollama not running. Start with `ollama serve`" |
| Ollama model not pulled | "Model not found. Run `ollama pull {model}`" |
| Google safety block | "Response blocked by Google safety filter" |
| Provider changed mid-request | `CancelRequest()` on old provider, switch cleanly |

---

## Definition of Done

- [ ] All 7 `EOliveAIProvider` enum values produce a working provider
- [ ] No code path returns "not implemented yet"
- [ ] Each provider: ValidateConfig catches missing key/URL with actionable message
- [ ] Each provider: HTTP errors produce provider-specific, human-readable messages
- [ ] OpenAI, Ollama, OpenAI-Compatible: SSE streaming works
- [ ] Google: SSE streaming works with Gemini-format conversion
- [ ] Anthropic: Upgraded to SSE streaming
- [ ] Connection validation fires on provider switch with clear pass/fail UI
- [ ] Changing provider in settings immediately takes effect on next request
- [ ] Tool calling works or gracefully degrades for each provider
