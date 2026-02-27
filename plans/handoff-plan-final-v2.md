# Handoff: Task 2 Reliability Fixes (Final)

Four changes. Two code, one text, one config.

---

## Fix 1: Guard Threshold False Positive

**Problem:** `HandleBlueprintAddFunction` blocks adding new functions if ANY existing function has `Nodes.Num() <= 2`. A function with Entry + one SetVariable node = 2 nodes and IS correctly implemented. The guard treats it as empty → false positive.

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

**Find this block** (inside `HandleBlueprintAddFunction`):
```cpp
// Guard: block adding functions when the same BP has empty function stubs.
{
    TArray<FString> EmptyFunctions;
    for (UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
    {
        if (FuncGraph)
        {
            // Entry + Return = 2 nodes. Anything <= 2 means empty stub.
            if (FuncGraph->Nodes.Num() <= 2)
            {
                // Skip system-generated graphs (EventGraph, ConstructionScript)
                FString GraphName = FuncGraph->GetName();
                if (GraphName != TEXT("EventGraph") && GraphName != TEXT("ConstructionScript"))
                {
                    EmptyFunctions.Add(GraphName);
                }
            }
        }
    }
```

**Replace with:**
```cpp
// Guard: block adding functions when the same BP has empty function stubs.
{
    TArray<FString> EmptyFunctions;
    for (UEdGraph* FuncGraph : Blueprint->FunctionGraphs)
    {
        if (FuncGraph)
        {
            // Skip system-generated graphs
            FString GraphName = FuncGraph->GetName();
            if (GraphName == TEXT("EventGraph") || GraphName == TEXT("ConstructionScript"))
            {
                continue;
            }

            // Count user-created nodes only.
            // UK2Node_FunctionTerminator is the base class for both
            // FunctionEntry and FunctionResult, so this one check skips all system nodes.
            int32 UserNodeCount = 0;
            for (UEdGraphNode* Node : FuncGraph->Nodes)
            {
                if (Node && !Node->IsA<UK2Node_FunctionTerminator>())
                {
                    UserNodeCount++;
                }
            }

            if (UserNodeCount == 0)
            {
                EmptyFunctions.Add(GraphName);
            }
        }
    }
```

**Include** (add if not already present):
```cpp
#include "K2Node_FunctionTerminator.h"
```

**Result:** Entry + SetVariable (ResetCanFire) → UserNodeCount = 1 → not empty → allowed.

---

## Fix 2: Pin Space Normalization

**Problem:** Cast nodes produce pins like `"AsPlayer Controller"` (with space). AI sends `"AsPlayerController"` (PascalCase). Our resolvers fail to match. 8 wasted calls in the log.

### Fix 2a: `FOliveGraphWriter::FindPin`

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveGraphWriter.cpp`

After the case-insensitive block, before the trimmed block, add:

```cpp
// Space-stripped match (AI sends PascalCase, UE pins may have spaces)
{
    FString StrippedPinName = PinName.Replace(TEXT(" "), TEXT("")).ToLower();
    for (UEdGraphPin* TestPin : Node->Pins)
    {
        if (TestPin)
        {
            FString StrippedName = TestPin->GetName().Replace(TEXT(" "), TEXT("")).ToLower();
            if (StrippedName == StrippedPinName)
            {
                return TestPin;
            }
            FString StrippedDisplay = TestPin->GetDisplayName().ToString().Replace(TEXT(" "), TEXT("")).ToLower();
            if (StrippedDisplay == StrippedPinName)
            {
                return TestPin;
            }
        }
    }
}
```

### Fix 2b: `FOlivePinManifest::FindPinSmart`

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePinManifest.cpp`

After Stage 3 (CASE-INSENSITIVE MATCH), before Stage 4 (FUZZY MATCH), add:

```cpp
// ================================================================
// Stage 3.5: SPACE-STRIPPED MATCH
// AI sends PascalCase ("AsPlayerController"), UE pins have spaces
// ("AsPlayer Controller"). Strip spaces from both and compare.
// ================================================================
{
    FString StrippedHint = Hint.Replace(TEXT(" "), TEXT("")).ToLower();
    for (const FOlivePinManifestEntry* Entry : Candidates)
    {
        FString StrippedPin = Entry->PinName.Replace(TEXT(" "), TEXT("")).ToLower();
        FString StrippedDisplay = Entry->DisplayName.Replace(TEXT(" "), TEXT("")).ToLower();
        if (StrippedPin == StrippedHint || StrippedDisplay == StrippedHint)
        {
            if (OutMatchMethod) *OutMatchMethod = TEXT("space_stripped");
            return Entry;
        }
    }
}
```

---

## Fix 3: Text Updates

### 3a: MODIFY workflow — mention reference templates

**File:** `Content/SystemPrompts/Knowledge/cli_blueprint.txt`

**Find:**
```
MODIFY existing Blueprint:
1. project.search (find exact path) → 2. blueprint.read (understand current state) → 3. add_variable/add_component → 4. blueprint.apply_plan_json
```

**Replace with:**
```
MODIFY existing Blueprint:
1. project.search (find exact path)
2. If the task matches a reference template (e.g. pickup/interaction), call blueprint.get_template for the architectural pattern
3. blueprint.read (understand current state)
4. add_variable/add_component → blueprint.apply_plan_json
```

### 3b: Same in recipe_routing.txt

**File:** `Content/SystemPrompts/Knowledge/recipe_routing.txt`

**Find:**
```
- MODIFY existing: project.search (find path) -> blueprint.read -> then write tools
```

**Replace with:**
```
- MODIFY existing: project.search (find path) -> check reference templates if relevant -> blueprint.read -> then write tools
```

### 3c: Fix stale multi_asset.txt

**File:** `Content/SystemPrompts/Knowledge/recipes/blueprint/multi_asset.txt`

**Replace entire content with:**
```
TAGS: multi asset multiple blueprint cross dependency
---
MULTI-ASSET workflow (e.g. gun + bullet, spawner + enemy):
Complete ONE asset fully before starting the next.
Cross-references (e.g., SpawnActor for BP_Bullet) resolve as long as the referenced asset exists.
If Asset B needs to reference Asset A's class, create Asset A's structure first (just blueprint.create is enough).
Blueprint class refs append _C: "BP_Bullet_C" not "BP_Bullet".
```

### 3d: Remove stale section from blueprint_authoring.txt

**File:** `Content/SystemPrompts/Knowledge/blueprint_authoring.txt`

**Delete this entire section:**
```
## Multi-Asset Task Planning

When the user's request requires creating MULTIPLE Blueprints (e.g., "create a gun and bullet system"):

1. BEFORE calling any tools, list ALL assets you need to create
2. Create each asset's STRUCTURE first (blueprint.create + add_component + add_variable) before wiring ANY graph logic
3. Create graph logic for each asset AFTER all structures exist
4. Allocate your iteration budget: if you have 10 iterations and need 2 assets, spend at most 5 on each
5. PRIORITIZE creating all assets over perfecting wiring on the first one

NEVER spend all iterations on one asset when multiple are needed.
```

---

## Fix 4: Raise max-turns to 500

**Problem:** The current 50-turn limit artificially kills healthy runs on complex tasks. Our plan_json system is more efficient than granular tools (multiple nodes per call), so call counts are naturally low. Competitors like NeoStack don't impose turn limits at all — the user's agent subscription is the limit.

Loop prevention doesn't need a hard turn count. The self-correction system already handles it:
- Identical plan detection (stops after 3 identical submissions)
- Loop/oscillation detection (catches repeated failure patterns)
- Granular fallback escalation (plan_json → add_node/connect_pins)
- Budget exhaustion backstop (20 cycles per error signature)
- Idle timeout (kills processes with no tool calls for 180s)

Setting max-turns to 500 makes it a crash-only backstop, not an operational limit. Real loop breaking comes from the behavioral systems above.

**File:** `Source/OliveAIEditor/Private/Providers/OliveClaudeCodeProvider.cpp` (or wherever `--max-turns` is set)

**Find:** where the CLI args are built, look for `--max-turns 50`

**Change to:** `--max-turns 500`

**Also update** the settings tooltip if it references the old value:
```
"Maximum turns for autonomous CLI mode. Crash-only safety ceiling — loop detection handles stuck runs.
Each MCP tools/call counts as one turn. Set high (500) and let self-correction manage behavior."
```

---

## Test

Run: `"make @BP_Gun a pickup item the player can pickup and equip by pressing E"`

Watch for:
- Guard allows adding functions when ResetCanFire exists (UserNodeCount = 1, not blocked)
- `connect_pins` with `"AsPlayerController"` resolves to `"AsPlayer Controller"`
- AI reads `pickup_interaction` template early and modifies both BP_Gun and the player character
- Run completes naturally without hitting 50-turn wall
