# Reimplementation Guide: Utility Model & LLM Keyword Extraction

These two features were added as new files (not modifications to existing committed files), so they can be safely reimplemented from scratch if reverted.

---

## Feature 1: Utility Model (`FOliveUtilityModel`)

### Purpose
Lightweight static helper for quick single-shot LLM completions. Used for keyword expansion and other sub-second tasks. NOT a singleton — stateless class with all static methods.

### Files
- `Source/OliveAIEditor/Public/Services/OliveUtilityModel.h`
- `Source/OliveAIEditor/Private/Services/OliveUtilityModel.cpp`

### Architecture
- **3-tier fallback for `SendSimpleCompletion()`**:
  1. Configured utility model provider (cheap/fast model like Haiku)
  2. Main chat provider (if not ClaudeCode — CLI providers can't do HTTP completions)
  3. Claude Code CLI via `--print` mode (`TrySendCompletionViaCLI()`)
- Creates a transient `IOliveAIProvider` per call via `FOliveProviderFactory::CreateProvider()`
- Blocks game thread with tick-pumping (`FTSTicker::GetCoreTicker().Tick(0.01f)`) until response or timeout
- Config: `Temperature=0.0f`, `MaxTokens=256`

### Key Methods
```cpp
// Public API:
static bool SendSimpleCompletion(SystemPrompt, UserPrompt, OutResponse, OutError);
static bool IsAvailable();
static TArray<FString> ExtractSearchKeywords(UserMessage, MaxKeywords=12);

// Private:
static bool TrySendCompletion(ProviderType, ModelId, ApiKey, BaseUrl, Timeout, System, User, OutResponse, OutError);
static bool TrySendCompletionViaCLI(Timeout, SystemPrompt, UserPrompt, OutResponse, OutError);
static TArray<FString> ExtractKeywordsBasic(UserMessage, MaxKeywords=12);
static FString BuildKeywordExpansionPrompt();
static const TSet<FString>& GetActionVerbStopWords();
static FString ProviderEnumToName(EOliveAIProvider);
```

### CLI Fallback (`TrySendCompletionViaCLI`)
- Uses `FOliveClaudeCodeProvider::GetClaudeExecutablePath()` to find the claude binary
- Runs `claude --print --output-format text --max-turns 1 "prompt"` via `FPlatformProcess::ExecProcess()`
- Handles both direct binary and node.js invocation (`bIsJs` check)
- Escapes double quotes in the prompt

### Dependencies
```cpp
#include "Services/OliveUtilityModel.h"
#include "Settings/OliveAISettings.h"
#include "Providers/IOliveAIProvider.h"
#include "Providers/OliveClaudeCodeProvider.h"  // for GetClaudeExecutablePath()
#include "Template/OliveTemplateSystem.h"       // for FOliveLibraryIndex::Tokenize()
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
```

---

## Feature 2: LLM Keyword Extraction (`ExtractSearchKeywords`)

### Purpose
Extract search keywords from a user message for template pre-search. Uses LLM for synonym expansion (e.g., "gun" → "weapon", "fire", "projectile", "ammo"), falls back to basic tokenizer.

### Keyword Expansion Flow
1. **LLM tier**: Calls `SendSimpleCompletion()` with a keyword expansion system prompt. LLM returns comma-separated lowercase keywords. Needs >= 3 keywords to be accepted.
2. **Basic tokenizer tier**: Uses `FOliveLibraryIndex::Tokenize()` (splits on spaces/underscores/hyphens, lowercases, drops < 2 chars). Then removes action verb stop words.

### System Prompt (for keyword expansion LLM call)
```
You are a search keyword generator for an Unreal Engine Blueprint template library.
Given a user's task description, output 8-12 search keywords that would find relevant
Blueprint templates. Include:
- Direct terms from the task
- UE5 synonyms (e.g., "gun" -> "weapon", "fire", "projectile", "ammo")
- Related Blueprint concepts (e.g., "door" -> "interactable", "overlap", "timeline")
- Component types (e.g., "health" -> "stat", "damage", "combat")
Output ONLY a comma-separated list of lowercase keywords. No explanations.
```

### Action Verb Stop Words (filtered from basic tokenizer output)
```
create, build, make, add, implement, write, modify, change,
update, fix, remove, delete, set, get, wire, connect,
please, want, need, should, using, blueprint, graph, system
```

---

## Feature 3: Settings Properties (in `OliveAISettings.h`)

### Properties to Add (Category: "Utility Model")
```cpp
// In UOliveAISettings class:

UPROPERTY(Config, EditAnywhere, Category="Utility Model",
    meta=(DisplayName="Utility Model Provider"))
EOliveAIProvider UtilityModelProvider = EOliveAIProvider::OpenRouter;

UPROPERTY(Config, EditAnywhere, Category="Utility Model",
    meta=(DisplayName="Utility Model ID"))
FString UtilityModelId = TEXT("anthropic/claude-3-5-haiku-latest");

UPROPERTY(Config, EditAnywhere, Category="Utility Model",
    meta=(DisplayName="Utility Model API Key (Optional)", PasswordField=true))
FString UtilityModelApiKey;

UPROPERTY(Config, EditAnywhere, Category="Utility Model",
    meta=(DisplayName="Utility Model Timeout (seconds)", ClampMin=5, ClampMax=30))
int32 UtilityModelTimeoutSeconds = 10;

UPROPERTY(Config, EditAnywhere, Category="Utility Model",
    meta=(DisplayName="Enable LLM Keyword Expansion"))
bool bEnableLLMKeywordExpansion = true;
```

### DefaultOliveAI.ini Defaults
```ini
[/Script/OliveAIEditor.OliveAISettings]
UtilityModelProvider=OpenRouter
UtilityModelId=anthropic/claude-3-5-haiku-latest
UtilityModelTimeoutSeconds=10
bEnableLLMKeywordExpansion=True
```

### Helper Used by IsAvailable()
`GetApiKeyForProvider()` and `GetBaseUrlForProvider()` on `UOliveAISettings` — these should already exist.

---

## Integration Points (currently NOT connected but infrastructure is ready)

The utility model was built for template pre-search in stdin injection but was disconnected during the anchoring investigation. Future integration points:

1. **Stdin template pre-search**: Call `ExtractSearchKeywords()` → `FOliveLibraryIndex::Search()` → inject results into stdin
2. **Error classification**: Quick LLM call to classify tool errors for self-correction
3. **Community blueprint search enhancement**: Better keyword expansion for `olive.search_community_blueprints`

---

## Build System

No changes needed to `OliveAIEditor.Build.cs` — the `Services/` directory is already in the include path. Just create the `.h` and `.cpp` files.

---

## Known Issue: Settings Visibility

The "Utility Model" category may not appear in Project Settings unless you scroll down in the Olive AI section. This was flagged as unresolved — consider adding a details customization to group it more prominently, or verify it shows up in the editor UI after reimplementation.
