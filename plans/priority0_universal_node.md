# Priority 0: Universal Node Manipulation

## Overview

Five changes that make `blueprint.add_node` work for ANY UK2Node subclass, add a pin discovery tool, improve error classification in self-correction, add capability routing to prompts, and fix the plan validator's string-literal Target check.

**Implementation Order:** Task 1 (validator fix) -> Task 2 (error classification) -> Task 3 (capability routing) -> Task 4 (universal add_node) + Task 5 (get_node_pins) together.

---

## File Map

### Modified Files

| File | Changes |
|------|---------|
| `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp` | Task 1: Accept string-literal component names as valid Targets |
| `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp` | Task 2: Error classification (A/B/C categories) |
| `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h` | Task 2: New enum `EOliveErrorCategory`, new method `ClassifyErrorCode()` |
| `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp` | Task 3: Load `node_routing` knowledge pack |
| `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h` | Task 4: New public method `CreateNodeByClass()`, modify `CreateNode()` and `ValidateNodeType()` |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | Task 4: Universal fallback implementation |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Task 4: Update `HandleBlueprintAddNode` for fallback result; Task 5: New `HandleBlueprintGetNodePins` |
| `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h` | Task 5: Declare `HandleBlueprintGetNodePins` |
| `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h` | Task 5: Declare `BlueprintGetNodePins()` |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` | Task 5: Implement `BlueprintGetNodePins()` |

### New Files

| File | Purpose |
|------|---------|
| `Content/SystemPrompts/Knowledge/node_routing.txt` | Task 3: Knowledge pack for graph-node capability routing |

---

## Task 1: Validator Fix -- Recognize String-Literal Targets (Change 5)

**Estimated Time:** 30 minutes
**Risk:** Lowest
**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanValidator.cpp`

### Problem

In `CheckComponentFunctionTargets()` at line 100, the check for whether the AI wired a Target pin is:

```cpp
const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
const bool bHasTargetWired = TargetValue && TargetValue->StartsWith(TEXT("@"));
```

This only accepts `@ref` syntax (e.g., `@get_comp.auto`). But the AI can also provide a string literal like `"MyMeshComp"` as the Target value. When it does, the value does NOT start with `@`, so the validator rejects it as unwired, even though the executor's Phase 4 data wiring will try to resolve it as a component variable name or a default value. If the string matches a known SCS component, the plan is valid.

### Fix

Expand the `bHasTargetWired` check to also accept string literals that match an SCS component name.

**Exact change location:** `OlivePlanValidator.cpp`, `CheckComponentFunctionTargets()`, around line 98-105.

Replace:

```cpp
// Check for Target input with a @ref (meaning AI provided a component reference)
const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
const bool bHasTargetWired = TargetValue && TargetValue->StartsWith(TEXT("@"));

if (bHasTargetWired)
{
    continue; // AI correctly wired a component target
}
```

With:

```cpp
// Check for Target input -- either a @ref or a string-literal component name
const FString* TargetValue = PlanStep.Inputs.Find(TEXT("Target"));
bool bHasTargetWired = false;

if (TargetValue && !TargetValue->IsEmpty())
{
    if (TargetValue->StartsWith(TEXT("@")))
    {
        // @ref syntax -- AI provided an explicit component reference
        bHasTargetWired = true;
    }
    else if (Context.Blueprint->SimpleConstructionScript)
    {
        // String literal -- check if it matches an SCS component variable name.
        // This handles the case where the AI passes the component name directly
        // (e.g., "StaticMeshComp") instead of a @ref. Phase 4 data wiring treats
        // non-@ref strings as pin defaults, which won't wire a component reference,
        // BUT this is still the AI's expressed intent to target a component.
        // Phase 1.5 auto-wire will handle the actual wiring if exactly one match exists.
        const FString TargetStr = *TargetValue;
        for (USCS_Node* SCSNode : Context.Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (SCSNode && SCSNode->GetVariableName().ToString() == TargetStr)
            {
                bHasTargetWired = true;
                break;
            }
        }
    }
}

if (bHasTargetWired)
{
    continue; // AI correctly targeted a component
}
```

**Key design notes:**
- Case-sensitive match against `GetVariableName()`. Component variable names are case-sensitive in Blueprints.
- Does NOT do fuzzy matching. An exact match means the AI intentionally chose this component.
- If the string literal does NOT match any SCS component, fall through to the existing error/warning path. This is correct -- Phase 1.5 auto-wire only works when there is exactly one matching component of the right type.

### Test

1. Create a plan with a `call` step targeting a component function (e.g., `SetRelativeLocation` on `StaticMeshComponent`) where the `Target` input is a literal string matching an SCS component name (e.g., `"Mesh"`).
2. Run through `preview_plan_json`. It should NOT emit `COMPONENT_FUNCTION_ON_ACTOR` for that step.
3. Verify that `@ref` syntax still works as before.
4. Verify that a mismatched string literal (e.g., `"NonexistentComp"`) still triggers the error path.

---

## Task 2: Error Classification in Self-Correction (Change 3)

**Estimated Time:** 2 hours
**Risk:** Low-Medium
**Files:**
- `Source/OliveAIEditor/Public/Brain/OliveSelfCorrectionPolicy.h`
- `Source/OliveAIEditor/Private/Brain/OliveSelfCorrectionPolicy.cpp`

### Concept

Currently, `BuildToolErrorMessage()` treats all errors the same way -- it always returns `FeedBackErrors` (retry with guidance). Some errors are fundamentally unsupported features where retrying is wasteful. Others are ambiguous and deserve one retry before escalating.

Introduce a three-category classification:

| Category | Meaning | Self-Correction Behavior |
|----------|---------|--------------------------|
| **A: Fixable Mistake** | Wrong name, missing param, asset not found | Standard retry with guidance (same as today) |
| **B: Unsupported Feature** | Feature not available through current tools | Do NOT retry. Return error + alternative path suggestion. |
| **C: Ambiguous** | Could be fixable or could be unsupported | Retry once with extra diagnostics, then escalate to B behavior |

### Header Changes

Add to `OliveSelfCorrectionPolicy.h`, before the class definition:

```cpp
/**
 * Error classification for self-correction routing.
 * Determines whether an error is worth retrying.
 */
enum class EOliveErrorCategory : uint8
{
    FixableMistake,       // Category A: retry with guidance
    UnsupportedFeature,   // Category B: do NOT retry, suggest alternative
    Ambiguous             // Category C: retry once, then escalate
};
```

Add a new private method declaration to `FOliveSelfCorrectionPolicy`:

```cpp
/**
 * Classify an error code into a correction category.
 * @param ErrorCode The tool error code (e.g., "NODE_TYPE_UNKNOWN", "BP_ADD_NODE_FAILED")
 * @param ErrorMessage The full error message (for secondary pattern matching)
 * @return Category determining retry behavior
 */
static EOliveErrorCategory ClassifyErrorCode(
    const FString& ErrorCode,
    const FString& ErrorMessage);
```

### Implementation Changes

**New method: `ClassifyErrorCode()`**

Add to `OliveSelfCorrectionPolicy.cpp`:

```cpp
EOliveErrorCategory FOliveSelfCorrectionPolicy::ClassifyErrorCode(
    const FString& ErrorCode,
    const FString& ErrorMessage)
{
    // ============================================================
    // Category B: Unsupported Feature -- do NOT retry
    // ============================================================

    // These represent features that genuinely cannot be done with current tools.
    // Retrying will never help; the AI needs to choose a different approach.
    if (ErrorCode == TEXT("USER_DENIED"))
    {
        return EOliveErrorCategory::UnsupportedFeature;
    }

    // ============================================================
    // Category C: Ambiguous -- retry once, then escalate
    // ============================================================

    // These might be fixable with a different approach, but might also
    // indicate a fundamental limitation. One retry with diagnostics.
    if (ErrorCode == TEXT("BP_ADD_NODE_FAILED"))
    {
        // Could be a typo in class name (fixable) or a genuinely unsupported
        // node type (unsupported). One retry with class lookup guidance.
        return EOliveErrorCategory::Ambiguous;
    }
    if (ErrorCode == TEXT("PLAN_EXECUTION_FAILED"))
    {
        return EOliveErrorCategory::Ambiguous;
    }

    // ============================================================
    // Category A: Fixable Mistake -- standard retry (default)
    // ============================================================

    // Everything else is treated as a fixable mistake:
    // VALIDATION_MISSING_PARAM, ASSET_NOT_FOUND, NODE_TYPE_UNKNOWN,
    // FUNCTION_NOT_FOUND, DUPLICATE_NATIVE_EVENT, DATA_PIN_NOT_FOUND,
    // EXEC_PIN_NOT_FOUND, BP_CONNECT_PINS_FAILED, PLAN_RESOLVE_FAILED,
    // COMPILE_FAILED, COMPONENT_FUNCTION_ON_ACTOR, etc.
    return EOliveErrorCategory::FixableMistake;
}
```

**CRITICAL NOTE:** The B category is intentionally very small right now. Only `USER_DENIED` truly belongs in B. As universal add_node lands (Task 4), errors that previously had no workaround become fixable (moving from potential-B to A). The classification should be conservative -- it is better to retry once more than to give up too early.

**Modify `Evaluate()` method:**

At the point where it calls `BuildToolErrorMessage()` and sets `Decision.Action = EOliveCorrectionAction::FeedBackErrors` (around line 124), add category-based routing.

Replace the tool failure block (lines 102-128) with:

```cpp
// Check for tool failure
FString ErrorCode, ErrorMessage;
if (HasToolFailure(ResultJson, ErrorCode, ErrorMessage))
{
    // Classify error before deciding retry behavior
    const EOliveErrorCategory Category = ClassifyErrorCode(ErrorCode, ErrorMessage);

    // Category B: Unsupported Feature -- do NOT retry, suggest alternative
    if (Category == EOliveErrorCategory::UnsupportedFeature)
    {
        Decision.Action = EOliveCorrectionAction::FeedBackErrors;
        Decision.EnrichedMessage = BuildToolErrorMessage(
            ToolName, ErrorCode, ErrorMessage, 1, 1);
        Decision.EnrichedMessage += TEXT("\n\n[UNSUPPORTED] This error indicates a feature "
            "limitation, not a fixable mistake. Do NOT retry the same operation. "
            "Choose a fundamentally different approach or ask the user for guidance.");

        UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Category B (Unsupported) for '%s' error '%s'. "
            "Returning guidance without retry encouragement."), *ToolName, *ErrorCode);
        return Decision;
    }

    // Build error signature and record attempt (per-signature tracking)
    const FString Signature = FOliveLoopDetector::BuildToolErrorSignature(ToolName, ErrorCode, AssetContext);
    LoopDetector.RecordAttempt(Signature, FString::Printf(TEXT("tool_retry_%d"), LoopDetector.GetAttemptCount(Signature)));

    const int32 SignatureAttempts = LoopDetector.GetAttemptCount(Signature);
    Decision.AttemptNumber = SignatureAttempts;
    Decision.MaxAttempts = Policy.MaxRetriesPerError;

    // Check for loops
    if (LoopDetector.IsLooping(Signature, Policy) || LoopDetector.IsOscillating() || LoopDetector.IsBudgetExhausted(Policy))
    {
        Decision.Action = EOliveCorrectionAction::StopWorker;
        Decision.LoopReport = LoopDetector.BuildLoopReport();
        UE_LOG(LogOliveAI, Warning, TEXT("SelfCorrection: Loop detected for tool '%s' error '%s'. Stopping."), *ToolName, *ErrorCode);
        return Decision;
    }

    // Category C: Ambiguous -- allow 1 retry, then escalate
    if (Category == EOliveErrorCategory::Ambiguous && SignatureAttempts > 1)
    {
        Decision.Action = EOliveCorrectionAction::FeedBackErrors;
        Decision.EnrichedMessage = BuildToolErrorMessage(
            ToolName, ErrorCode, ErrorMessage, SignatureAttempts, Policy.MaxRetriesPerError);
        Decision.EnrichedMessage += TEXT("\n\n[ESCALATION] This error may indicate a fundamental "
            "limitation rather than a fixable mistake. If the same approach keeps failing, "
            "try add_node with the exact UK2Node class name, or ask the user.");

        UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Category C escalation for '%s' error '%s', "
            "attempt %d."), *ToolName, *ErrorCode, SignatureAttempts);
        return Decision;
    }

    // Category A (and first attempt of Category C): standard retry
    Decision.Action = EOliveCorrectionAction::FeedBackErrors;
    Decision.EnrichedMessage = BuildToolErrorMessage(ToolName, ErrorCode, ErrorMessage, SignatureAttempts, Policy.MaxRetriesPerError);
    UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Tool failure '%s' error '%s', attempt %d/%d"),
        *ToolName, *ErrorCode, SignatureAttempts, Policy.MaxRetriesPerError);
    UE_LOG(LogOliveAI, Log, TEXT("SelfCorrection: Injecting tool correction:\n%s"), *Decision.EnrichedMessage);
    return Decision;
}
```

### Test

1. Trigger a `USER_DENIED` error. Verify the self-correction returns guidance with `[UNSUPPORTED]` tag and does NOT increment retry counters or record loop detector attempts.
2. Trigger a `BP_ADD_NODE_FAILED` error twice in a row. On the first attempt, it should retry normally (Category C -> first attempt allowed). On the second attempt, it should append the `[ESCALATION]` text.
3. Trigger a `FUNCTION_NOT_FOUND` error. It should retry normally as Category A, with no escalation behavior (standard progressive disclosure).

---

## Task 3: Capability Routing in Prompts (Change 4)

**Estimated Time:** 1 hour
**Risk:** Low
**Files:**
- `Content/SystemPrompts/Knowledge/node_routing.txt` (NEW)
- `Source/OliveAIEditor/Private/Chat/OlivePromptAssembler.cpp`

### New Knowledge Pack File

Create `Content/SystemPrompts/Knowledge/node_routing.txt`:

```
## Graph Node Paths

When adding logic to Blueprints, choose the right tool based on what you need:

### Path 1: Plan JSON (preferred for 3+ nodes)
Use `blueprint.apply_plan_json` with schema_version "2.0" for standard logic:
- Supported ops: event, custom_event, call, get_var, set_var, branch, sequence,
  cast, for_loop, for_each_loop, while_loop, do_once, flip_flop, gate, delay,
  is_valid, print_string, spawn_actor, make_struct, break_struct, return, comment
- Automatic pin resolution, exec wiring, and data wiring
- Use this for 90% of Blueprint graph work

### Path 2: add_node (for specialized/exotic nodes)
Use `blueprint.add_node` when you need a node type NOT covered by plan JSON ops:
- Component Bound Events (K2Node_ComponentBoundEvent)
- Timeline nodes (K2Node_Timeline)
- Select nodes (K2Node_Select)
- Switch on Enum/String/Int (K2Node_SwitchEnum, K2Node_SwitchString, etc.)
- Enhanced Input Action nodes
- Animation nodes (in AnimBP graphs)
- Any UK2Node subclass by exact class name

How add_node works:
1. Pass `type` as either a curated name (CallFunction, Branch, etc.) OR the exact
   UK2Node class name (e.g., "K2Node_ComponentBoundEvent", "K2Node_Timeline")
2. Pass `properties` as key-value pairs matching the node's UPROPERTY fields
3. The tool returns the node's pin manifest showing all actual pin names
4. Follow up with `blueprint.connect_pins` and `blueprint.set_pin_default` for wiring
5. Use `blueprint.get_node_pins` to re-inspect pins after property changes

### Path 3: add_node + get_node_pins (reconstruct workflow)
For nodes whose pins change based on properties:
1. `blueprint.add_node` with initial properties -> get pin manifest
2. `blueprint.set_node_property` to change a key property -> pins may change
3. `blueprint.get_node_pins` to re-discover pins after reconstruction
4. Wire the updated pins with `blueprint.connect_pins`

### Error Recovery: When plan JSON fails
If `apply_plan_json` fails for a specific node type (e.g., component bound events),
fall back to `add_node` with the exact UK2Node class name. Example:
- Plan JSON has no `component_bound_event` op
- Use: add_node type:"K2Node_ComponentBoundEvent" properties:{"DelegatePropertyName":"OnComponentHit","ComponentPropertyName":"CollisionComp"}
- Then wire with connect_pins
```

### Register the Knowledge Pack

In `OlivePromptAssembler.cpp`, at line 529 (the `ProfileCapabilityPackIds` registration), add `node_routing` to the Auto and Blueprint profiles:

Replace:
```cpp
ProfileCapabilityPackIds.Add(TEXT("Auto"), { TEXT("blueprint_authoring"), TEXT("recipe_routing") });
ProfileCapabilityPackIds.Add(TEXT("Blueprint"), { TEXT("blueprint_authoring"), TEXT("recipe_routing") });
```

With:
```cpp
ProfileCapabilityPackIds.Add(TEXT("Auto"), { TEXT("blueprint_authoring"), TEXT("recipe_routing"), TEXT("node_routing") });
ProfileCapabilityPackIds.Add(TEXT("Blueprint"), { TEXT("blueprint_authoring"), TEXT("recipe_routing"), TEXT("node_routing") });
```

### Update Self-Correction Guidance

In `OliveSelfCorrectionPolicy.cpp`, in the `BuildToolErrorMessage()` method, update the guidance for error codes that now have an alternative path via universal add_node.

Locate the `NODE_TYPE_UNKNOWN` / `BP_ADD_NODE_FAILED` guidance block (around line 322-335). Replace the else-branch guidance:

```cpp
else if (ErrorCode == TEXT("NODE_TYPE_UNKNOWN") || ErrorCode == TEXT("BP_ADD_NODE_FAILED"))
{
    if (ErrorMessage.Contains(TEXT("class")) && ErrorMessage.Contains(TEXT("not found")))
    {
        Guidance = TEXT("The specified class was not found. "
            "If this is a Blueprint class, provide the full asset path (e.g., '/Game/Blueprints/BP_Bullet'). "
            "Use project.search_assets to find the correct path. "
            "If this is a native C++ class, use the full prefixed name (e.g., 'ACharacter', 'APawn'). "
            "Do NOT guess class names -- search first.");
    }
    else
    {
        Guidance = TEXT("The node type was not recognized as a curated type. "
            "You can also pass the exact UK2Node class name as the type "
            "(e.g., 'K2Node_ComponentBoundEvent', 'K2Node_Timeline', 'K2Node_Select'). "
            "Use blueprint.search_nodes to find available node types. "
            "After creation, use blueprint.get_node_pins to discover the actual pin names.");
    }
}
```

Also add a new block for event-related failures that should suggest add_node as a fallback:

After the `DUPLICATE_NATIVE_EVENT` block, add:

```cpp
else if (ErrorCode == TEXT("PLAN_RESOLVE_FAILED") && ErrorMessage.Contains(TEXT("event")))
{
    Guidance = TEXT("The plan failed to resolve an event operation. "
        "If you need a component delegate event (e.g., OnComponentHit, OnComponentBeginOverlap), "
        "plan JSON does not support component_bound_event. Use add_node instead:\n"
        "  blueprint.add_node type:\"K2Node_ComponentBoundEvent\" "
        "properties:{\"DelegatePropertyName\":\"OnComponentHit\",\"ComponentPropertyName\":\"CollisionComp\"}\n"
        "Then wire with blueprint.connect_pins.");
}
```

**IMPORTANT:** Insert this new block BEFORE the generic `PLAN_RESOLVE_FAILED` block (lines 382-389) so the more specific pattern match takes priority. The existing generic block remains as the fallback.

### Test

1. Open the editor, activate the Blueprint or Auto profile.
2. Confirm that `GetCapabilityKnowledge("Blueprint")` includes the `node_routing` content.
3. Trigger a `NODE_TYPE_UNKNOWN` error with self-correction. Verify the guidance now mentions UK2Node class names.
4. Trigger a `PLAN_RESOLVE_FAILED` error with "event" in the message. Verify it suggests `K2Node_ComponentBoundEvent`.

---

## Task 4: Universal add_node -- Drop the Type Whitelist (Change 1)

**Estimated Time:** 4-6 hours
**Risk:** Medium (but curated types are completely untouched)
**Files:**
- `Source/OliveAIEditor/Blueprint/Public/Writer/OliveNodeFactory.h`
- `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

### Design

When the AI passes a type string like `"K2Node_ComponentBoundEvent"`, it is NOT in the `NodeCreators` map. Currently, `ValidateNodeType()` rejects it and `CreateNode()` never runs. The fix:

1. **`ValidateNodeType()`** -- when the type is not in `NodeCreators`, attempt a UClass lookup instead of immediately failing. If a valid UK2Node subclass is found, return true.
2. **`CreateNode()`** -- when the type is not in `NodeCreators`, call a new `CreateNodeByClass()` fallback.
3. **`CreateNodeByClass()`** -- new method that instantiates any UK2Node subclass via `NewObject`, sets UPROPERTY fields via reflection, calls `AllocateDefaultPins()` + `ReconstructNode()`, and adds to the graph.

The curated `NodeCreators` map is ALWAYS checked first. The universal fallback is ONLY reached when no curated creator exists.

### 4.1: Header Changes to OliveNodeFactory.h

Add a new public method and a new private helper:

```cpp
public:
    // ... existing public methods ...

    /**
     * Create a node by resolving the type string as a UK2Node subclass name.
     * This is the universal fallback -- used when the type is not in the curated
     * NodeCreators map.
     *
     * @param Blueprint The Blueprint containing the graph
     * @param Graph The graph to add the node to
     * @param ClassName The UK2Node subclass name (e.g., "K2Node_ComponentBoundEvent")
     * @param Properties UPROPERTY field name -> string value pairs to set via reflection
     * @param PosX X position
     * @param PosY Y position
     * @return The created node, or nullptr (with LastError set)
     */
    UEdGraphNode* CreateNodeByClass(
        UBlueprint* Blueprint,
        UEdGraph* Graph,
        const FString& ClassName,
        const TMap<FString, FString>& Properties,
        int32 PosX,
        int32 PosY);

private:
    // ... existing private methods ...

    /**
     * Resolve a UK2Node subclass by name. Searches across multiple engine module packages.
     * @param ClassName Short class name (e.g., "K2Node_ComponentBoundEvent") or prefixed name
     * @return The UClass if found and is a subclass of UK2Node, nullptr otherwise
     */
    UClass* FindK2NodeClass(const FString& ClassName) const;

    /**
     * Set UPROPERTY fields on a node via reflection.
     * Uses the same type-dispatch pattern as OliveGraphWriter::SetNodeProperty.
     *
     * @param Node The node to set properties on
     * @param Properties Key-value pairs (UPROPERTY name -> string value)
     * @param OutSetProperties Names of properties that were successfully set
     * @param OutSkippedProperties Names of properties that could not be set (with reason)
     * @return Number of properties successfully set
     */
    int32 SetNodePropertiesViaReflection(
        UEdGraphNode* Node,
        const TMap<FString, FString>& Properties,
        TArray<FString>& OutSetProperties,
        TMap<FString, FString>& OutSkippedProperties);
```

### 4.2: Implementation of FindK2NodeClass

Add to `OliveNodeFactory.cpp`:

```cpp
UClass* FOliveNodeFactory::FindK2NodeClass(const FString& ClassName) const
{
    // Strategy 1: Direct FindFirstObject (handles fully-qualified and short names)
    UClass* Found = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
    if (Found && Found->IsChildOf(UK2Node::StaticClass()))
    {
        return Found;
    }

    // Strategy 2: Try with "UK2Node_" prefix variants
    // The AI might pass "ComponentBoundEvent" without the UK2Node_ prefix
    if (!ClassName.StartsWith(TEXT("K2Node_")) && !ClassName.StartsWith(TEXT("UK2Node_")))
    {
        // Try K2Node_X
        FString Prefixed = TEXT("K2Node_") + ClassName;
        Found = FindFirstObject<UClass>(*Prefixed, EFindFirstObjectOptions::NativeFirst);
        if (Found && Found->IsChildOf(UK2Node::StaticClass()))
        {
            return Found;
        }
        // Try UK2Node_X
        Prefixed = TEXT("UK2Node_") + ClassName;
        Found = FindFirstObject<UClass>(*Prefixed, EFindFirstObjectOptions::NativeFirst);
        if (Found && Found->IsChildOf(UK2Node::StaticClass()))
        {
            return Found;
        }
    }

    // Strategy 3: Strip "U" prefix if present (AI might pass "UK2Node_Timeline")
    if (ClassName.StartsWith(TEXT("U")))
    {
        FString Stripped = ClassName.Mid(1);
        Found = FindFirstObject<UClass>(*Stripped, EFindFirstObjectOptions::NativeFirst);
        if (Found && Found->IsChildOf(UK2Node::StaticClass()))
        {
            return Found;
        }
    }

    // Strategy 4: Try common engine packages explicitly via StaticLoadClass
    // K2Nodes live in multiple modules. FindFirstObject searches loaded modules,
    // but some may not be loaded yet. StaticLoadClass forces loading.
    static const TCHAR* PackagePaths[] = {
        TEXT("/Script/BlueprintGraph"),
        TEXT("/Script/UnrealEd"),
        TEXT("/Script/AnimGraph"),
        TEXT("/Script/GameplayAbilitiesEditor"),
        TEXT("/Script/EnhancedInput"),
        TEXT("/Script/AIModule"),
    };

    // Build the class name to search for (strip U prefix for StaticLoadClass)
    FString SearchName = ClassName;
    if (SearchName.StartsWith(TEXT("U")))
    {
        SearchName = SearchName.Mid(1);
    }

    for (const TCHAR* Package : PackagePaths)
    {
        FString FullPath = FString::Printf(TEXT("%s.%s"), Package, *SearchName);
        Found = StaticLoadClass(UK2Node::StaticClass(), nullptr, *FullPath, nullptr, LOAD_Quiet);
        if (Found)
        {
            return Found;
        }
    }

    return nullptr;
}
```

**Design notes:**
- `FindFirstObject<UClass>` with `NativeFirst` searches all loaded packages. This finds most K2Nodes from commonly-loaded modules (BlueprintGraph, UnrealEd).
- `StaticLoadClass` forces module loading for packages that might not be loaded yet (e.g., GameplayAbilitiesEditor). The `LOAD_Quiet` flag suppresses errors when the class doesn't exist in that package.
- The search is NOT exhaustive of all engine modules -- it covers the most common ones. If a K2Node lives in an obscure module, the AI would need to provide the full `/Script/ModuleName.ClassName` path.
- `IsChildOf(UK2Node::StaticClass())` ensures we only accept actual K2Node subclasses, not arbitrary UClasses.

### 4.3: Implementation of SetNodePropertiesViaReflection

Add to `OliveNodeFactory.cpp`. This mirrors the existing pattern in `OliveGraphWriter.cpp` lines 352-390:

```cpp
int32 FOliveNodeFactory::SetNodePropertiesViaReflection(
    UEdGraphNode* Node,
    const TMap<FString, FString>& Properties,
    TArray<FString>& OutSetProperties,
    TMap<FString, FString>& OutSkippedProperties)
{
    int32 SetCount = 0;

    for (const auto& Pair : Properties)
    {
        const FString& PropName = Pair.Key;
        const FString& PropValue = Pair.Value;

        FProperty* Property = Node->GetClass()->FindPropertyByName(FName(*PropName));
        if (!Property)
        {
            OutSkippedProperties.Add(PropName, TEXT("Property not found on node class"));
            UE_LOG(LogOliveNodeFactory, Warning,
                TEXT("SetNodePropertiesViaReflection: Property '%s' not found on %s"),
                *PropName, *Node->GetClass()->GetName());
            continue;
        }

        // Skip properties that are not editable (private/transient)
        if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_Config))
        {
            // Still allow it -- many K2Node properties are set programmatically
            // and don't have CPF_Edit. ImportText will work regardless.
            UE_LOG(LogOliveNodeFactory, Verbose,
                TEXT("SetNodePropertiesViaReflection: Property '%s' is not CPF_Edit, "
                     "attempting ImportText anyway"), *PropName);
        }

        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);

        // Type-specific fast path (same as OliveGraphWriter pattern)
        bool bSet = false;
        if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
        {
            BoolProp->SetPropertyValue(ValuePtr, PropValue.ToBool());
            bSet = true;
        }
        else if (FIntProperty* IntProp = CastField<FIntProperty>(Property))
        {
            IntProp->SetPropertyValue(ValuePtr, FCString::Atoi(*PropValue));
            bSet = true;
        }
        else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
        {
            FloatProp->SetPropertyValue(ValuePtr, FCString::Atof(*PropValue));
            bSet = true;
        }
        else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
        {
            DoubleProp->SetPropertyValue(ValuePtr, FCString::Atod(*PropValue));
            bSet = true;
        }
        else if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
        {
            StrProp->SetPropertyValue(ValuePtr, PropValue);
            bSet = true;
        }
        else if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
        {
            NameProp->SetPropertyValue(ValuePtr, FName(*PropValue));
            bSet = true;
        }
        else if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
        {
            TextProp->SetPropertyValue(ValuePtr, FText::FromString(PropValue));
            bSet = true;
        }
        else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
        {
            // Try to load the object from a path
            UObject* Obj = StaticLoadObject(ObjProp->PropertyClass, nullptr, *PropValue);
            if (Obj)
            {
                ObjProp->SetObjectPropertyValue(ValuePtr, Obj);
                bSet = true;
            }
            else
            {
                // Try FindFirstObject for already-loaded objects
                Obj = FindFirstObject<UObject>(*PropValue, EFindFirstObjectOptions::NativeFirst);
                if (Obj && Obj->IsA(ObjProp->PropertyClass))
                {
                    ObjProp->SetObjectPropertyValue(ValuePtr, Obj);
                    bSet = true;
                }
                else
                {
                    OutSkippedProperties.Add(PropName,
                        FString::Printf(TEXT("Could not resolve object '%s' for property type '%s'"),
                            *PropValue, *ObjProp->PropertyClass->GetName()));
                    continue;
                }
            }
        }
        else
        {
            // Generic text import -- handles FKey, enums, structs, etc.
            const TCHAR* ImportResult = Property->ImportText_Direct(*PropValue, ValuePtr, Node, PPF_None);
            bSet = (ImportResult != nullptr);
            if (!bSet)
            {
                OutSkippedProperties.Add(PropName,
                    FString::Printf(TEXT("ImportText failed for value '%s'"), *PropValue));
                continue;
            }
        }

        if (bSet)
        {
            OutSetProperties.Add(PropName);
            SetCount++;
            UE_LOG(LogOliveNodeFactory, Verbose,
                TEXT("SetNodePropertiesViaReflection: Set '%s' = '%s'"),
                *PropName, *PropValue);
        }
    }

    return SetCount;
}
```

**Key design notes:**
- `FObjectPropertyBase` handling is critical for K2Nodes that reference UClasses or assets (e.g., `UK2Node_ComponentBoundEvent` has `FMulticastDelegateProperty` and `FObjectProperty` fields). `StaticLoadObject` first, then `FindFirstObject` as fallback.
- The `ImportText_Direct` generic fallback handles complex types like `FKey`, enum values, and struct values. It is the same mechanism UE's own serialization uses.
- **CPF_Edit check is informational, not blocking.** Many K2Node UPROPERTYs don't have `CPF_Edit` because they aren't exposed in the Blueprint editor details panel, but they ARE writable programmatically.
- **OutSkippedProperties** is reported to the AI so it knows which properties didn't take and can self-correct.

### 4.4: Implementation of CreateNodeByClass

Add to `OliveNodeFactory.cpp`:

```cpp
UEdGraphNode* FOliveNodeFactory::CreateNodeByClass(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& ClassName,
    const TMap<FString, FString>& Properties,
    int32 PosX,
    int32 PosY)
{
    LastError.Empty();

    // Validate inputs
    if (!Blueprint || !Graph)
    {
        LastError = TEXT("Blueprint or Graph is null");
        return nullptr;
    }

    // Find the K2Node class
    UClass* NodeClass = FindK2NodeClass(ClassName);
    if (!NodeClass)
    {
        LastError = FString::Printf(
            TEXT("Could not find UK2Node subclass '%s'. "
                 "Ensure the class name is correct (e.g., 'K2Node_ComponentBoundEvent', "
                 "'K2Node_Timeline'). The class must be a subclass of UK2Node."),
            *ClassName);
        return nullptr;
    }

    UE_LOG(LogOliveNodeFactory, Log,
        TEXT("CreateNodeByClass: Resolved '%s' -> %s (module: %s)"),
        *ClassName, *NodeClass->GetName(), *NodeClass->GetOuterUPackage()->GetName());

    // Create the node
    UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
    if (!NewNode)
    {
        LastError = FString::Printf(
            TEXT("NewObject failed for class '%s'"), *NodeClass->GetName());
        return nullptr;
    }

    // Set properties BEFORE AllocateDefaultPins.
    // Many K2Node subclasses generate pins based on property values
    // (e.g., K2Node_ComponentBoundEvent needs DelegatePropertyName set
    // before pin allocation to know which delegate signature to expose).
    TArray<FString> SetProps;
    TMap<FString, FString> SkippedProps;
    const int32 PropsSet = SetNodePropertiesViaReflection(
        NewNode, Properties, SetProps, SkippedProps);

    UE_LOG(LogOliveNodeFactory, Log,
        TEXT("CreateNodeByClass: Set %d/%d properties on %s"),
        PropsSet, Properties.Num(), *NodeClass->GetName());

    // Create GUID for the node
    NewNode->CreateNewGuid();

    // Allocate pins based on current property state
    NewNode->AllocateDefaultPins();

    // Add to graph BEFORE ReconstructNode -- some nodes need graph context
    Graph->AddNode(NewNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);

    // Reconstruct to finalize pin layout.
    // This is mandatory -- many K2Nodes only produce correct pins after
    // reconstruction (which may read properties, resolve references, etc.).
    NewNode->ReconstructNode();

    // Set position
    SetNodePosition(NewNode, PosX, PosY);

    UE_LOG(LogOliveNodeFactory, Log,
        TEXT("CreateNodeByClass: Successfully created %s at (%d, %d) "
             "with %d pins, %d properties set, %d skipped"),
        *NodeClass->GetName(), PosX, PosY,
        NewNode->Pins.Num(), PropsSet, SkippedProps.Num());

    return NewNode;
}
```

**CRITICAL DESIGN DECISION: Properties set BEFORE AllocateDefaultPins.**

Rationale: Many K2Node subclasses use property values during `AllocateDefaultPins()` to determine which pins to create. For example:
- `UK2Node_ComponentBoundEvent` reads `DelegatePropertyName` to know which delegate signature to expose as pins
- `UK2Node_DynamicCast` reads `TargetType` to create the correct output pin type
- `UK2Node_CallFunction` reads the function reference to know which parameter pins to create

If properties are set AFTER pin allocation, the pins would be wrong and `ReconstructNode()` would be needed to regenerate them. By setting properties first, `AllocateDefaultPins()` generates correct pins immediately, and `ReconstructNode()` serves as a safety net.

**ReconstructNode() is still called after AllocateDefaultPins()** as defense-in-depth. Some nodes may need the graph context to fully resolve their pins, and reconstruction handles deferred setup.

### 4.5: Modify ValidateNodeType

In `OliveNodeFactory.cpp`, modify the `ValidateNodeType()` method. Currently at line 144, it immediately returns false when the type is not in `NodeCreators`. Change it to also check for K2Node classes:

Replace lines 143-152:

```cpp
// Step 1: Check if the node type exists in the creator map at all
if (!NodeCreators.Contains(NodeType))
{
    // Cast away const for LastError -- ValidateNodeType is logically a query
    // but LastError is a diagnostic side-channel, consistent with existing pattern
    const_cast<FOliveNodeFactory*>(this)->LastError = FString::Printf(
        TEXT("Unknown node type: '%s'. Use blueprint.node_catalog_search to find available node types."),
        *NodeType);
    return false;
}
```

With:

```cpp
// Step 1: Check if the node type exists in the curated creator map
if (!NodeCreators.Contains(NodeType))
{
    // Step 1b: Universal fallback -- try resolving as a UK2Node subclass name
    UClass* NodeClass = const_cast<FOliveNodeFactory*>(this)->FindK2NodeClass(NodeType);
    if (NodeClass)
    {
        UE_LOG(LogOliveNodeFactory, Log,
            TEXT("ValidateNodeType: '%s' not in curated map, but resolved as UK2Node subclass %s"),
            *NodeType, *NodeClass->GetName());
        return true; // Valid as a universal node class
    }

    const_cast<FOliveNodeFactory*>(this)->LastError = FString::Printf(
        TEXT("Unknown node type: '%s'. Not found as a curated type or UK2Node subclass. "
             "Use blueprint.search_nodes to find available node types, or pass the exact "
             "UK2Node class name (e.g., 'K2Node_ComponentBoundEvent')."),
        *NodeType);
    return false;
}
```

### 4.6: Modify CreateNode to Use Fallback

In `OliveNodeFactory.cpp`, modify the `CreateNode()` method. Currently at line 95, after validation passes, it indexes into `NodeCreators[NodeType]`. When the type is a universal class (not in `NodeCreators`), this would crash. Add a check:

Replace lines 94-96:

```cpp
// Call the appropriate creator
const FNodeCreator& Creator = NodeCreators[NodeType];
UEdGraphNode* NewNode = Creator(Blueprint, Graph, Properties);
```

With:

```cpp
// Call the appropriate creator (curated types) or universal fallback
UEdGraphNode* NewNode = nullptr;
if (const FNodeCreator* Creator = NodeCreators.Find(NodeType))
{
    NewNode = (*Creator)(Blueprint, Graph, Properties);
}
else
{
    // Universal fallback: type passed ValidateNodeType, so it's a valid K2Node class.
    // Position will be set by the caller (SetNodePosition below), but CreateNodeByClass
    // also accepts position for logging. Pass 0,0 here; actual position set below.
    NewNode = CreateNodeByClass(Blueprint, Graph, NodeType, Properties, PosX, PosY);
    // Position already set inside CreateNodeByClass, so skip SetNodePosition below
    if (NewNode)
    {
        UE_LOG(LogOliveNodeFactory, Log,
            TEXT("Created universal node '%s' at (%d, %d)"), *NodeType, PosX, PosY);
        return NewNode; // Early return -- position already set
    }
}
```

**Wait -- there is a subtlety.** The `SetNodePosition()` call at line 110 happens for ALL nodes. For the universal fallback, `CreateNodeByClass` already sets position. We need to avoid double-setting. The simplest fix: make `CreateNodeByClass` NOT set position (remove the `SetNodePosition` call from it), and let `CreateNode` handle it uniformly.

**Revised CreateNodeByClass:** Remove the `SetNodePosition` call from `CreateNodeByClass`. Let the caller (`CreateNode`) handle positioning uniformly. Update the code:

In `CreateNodeByClass`, remove:
```cpp
// Set position
SetNodePosition(NewNode, PosX, PosY);
```

And in `CreateNode`, the revised fallback becomes simpler:

```cpp
// Call the appropriate creator (curated types) or universal fallback
UEdGraphNode* NewNode = nullptr;
if (const FNodeCreator* Creator = NodeCreators.Find(NodeType))
{
    NewNode = (*Creator)(Blueprint, Graph, Properties);
}
else
{
    // Universal fallback: type passed ValidateNodeType, so it's a valid K2Node class
    NewNode = CreateNodeByClass(Blueprint, Graph, NodeType, Properties, PosX, PosY);
}
```

But wait, `CreateNodeByClass` takes PosX/PosY only for logging in this revised version. Actually, let me simplify further. Remove PosX/PosY from `CreateNodeByClass` entirely and let `CreateNode`'s `SetNodePosition` handle it:

**Final revised header for CreateNodeByClass:**

```cpp
UEdGraphNode* CreateNodeByClass(
    UBlueprint* Blueprint,
    UEdGraph* Graph,
    const FString& ClassName,
    const TMap<FString, FString>& Properties);
```

**And the CreateNode fallback is simply:**

```cpp
UEdGraphNode* NewNode = nullptr;
if (const FNodeCreator* Creator = NodeCreators.Find(NodeType))
{
    NewNode = (*Creator)(Blueprint, Graph, Properties);
}
else
{
    NewNode = CreateNodeByClass(Blueprint, Graph, NodeType, Properties);
}
```

The existing `SetNodePosition(NewNode, PosX, PosY)` at line 110 handles both paths uniformly.

### 4.7: Update HandleBlueprintAddNode Result

In `OliveBlueprintToolHandlers.cpp`, the `HandleBlueprintAddNode` handler (line 3621) currently calls `Factory.ValidateNodeType()` and, on failure, builds a `NODE_TYPE_UNKNOWN` error with fuzzy suggestions. After the universal fallback is added, `ValidateNodeType` will succeed for valid UK2Node class names, so this error path is only reached for truly unknown types.

However, the executor lambda (line 3770) needs an update for the result. When a universal fallback node is created, we want to include property feedback (which properties were set vs skipped).

**The executor lambda already returns pin manifest** via `BuildPinManifest()` at line 3802. This is sufficient for the universal fallback too, since `GraphWriter.AddNode()` calls `Factory.CreateNode()` which now includes the universal fallback.

**One additional change needed:** After a universal fallback node is created, the tool result should include property setting feedback. This requires threading the skipped-property information from `CreateNodeByClass` through `GraphWriter.AddNode` and back to the handler.

**Decision: Defer property feedback to a follow-up.** The existing result already includes the pin manifest, which is the most important information for the AI. Property feedback (which properties were set vs skipped) would require threading a new output field through `GraphWriter.AddNode` -> `FOliveBlueprintWriteResult`, which is a broader API change. For now, any property that fails to set will cause `ReconstructNode()` to produce incorrect or missing pins, which the pin manifest will reveal -- giving the AI indirect feedback. A dedicated property-feedback field can be added later.

**One change IS needed:** The pre-pipeline validation check. Currently at line 3681:

```cpp
if (!Factory.ValidateNodeType(NodeType, NodeProperties))
```

If `ValidateNodeType` returns true for a universal class, we proceed. But the `NODE_TYPE_UNKNOWN` error path at line 3700 creates error data including fuzzy catalog suggestions. This is only reached if validation fails, so no change needed -- the universal fallback means fewer reaches to this error path. Good.

### 4.8: Additional Include

In `OliveNodeFactory.cpp`, add this include at the top (for `StaticLoadClass`):

```cpp
#include "UObject/UObjectGlobals.h"  // For StaticLoadClass
```

This is likely already available through existing includes, but verify during compilation. If `StaticLoadClass` is not available, it is declared in `UObject/UObjectGlobals.h` which is part of CoreUObject.

### Test Plan for Task 4

1. **Curated type unchanged:** Call `add_node` with `type:"Branch"`. Verify it works exactly as before (curated path, no fallback).

2. **Universal K2Node by exact name:** Call `add_node` with `type:"K2Node_Select"` and no properties. Verify it creates a node and returns a pin manifest.

3. **Universal K2Node with properties:** Call `add_node` with:
   ```json
   {
     "path": "/Game/Blueprints/BP_Test",
     "graph": "EventGraph",
     "type": "K2Node_ComponentBoundEvent",
     "properties": {
       "DelegatePropertyName": "OnComponentHit",
       "ComponentPropertyName": "CollisionComp"
     }
   }
   ```
   Verify the node is created and pins reflect the OnComponentHit delegate signature.

4. **Stripped prefix:** Call `add_node` with `type:"ComponentBoundEvent"` (without `K2Node_` prefix). Verify `FindK2NodeClass` resolves it via the prefix strategy.

5. **Invalid class name:** Call `add_node` with `type:"K2Node_DoesNotExist"`. Verify it returns `NODE_TYPE_UNKNOWN` error with helpful message.

6. **Non-K2Node class rejected:** Call `add_node` with `type:"Actor"`. Even though `FindFirstObject<UClass>` finds `AActor`, the `IsChildOf(UK2Node)` check should reject it.

7. **Property reflection:** Create a `K2Node_InputKey` via the universal fallback (it already has a curated creator, but bypass it temporarily for testing). Set `InputKey` property to `"E"`. Verify the pin manifest includes the Pressed/Released exec pins.

---

## Task 5: Pin Discovery Tool -- blueprint.get_node_pins (Change 2)

**Estimated Time:** 1-2 hours
**Risk:** Low
**Files:**
- `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h`
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`
- `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h`
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

### Why This Ships with Task 4

The universal add_node creates nodes whose pins depend on property values. After creation, the AI gets the pin manifest in the result. But if the AI later calls `blueprint.set_node_property` to change a property (which calls `ReconstructNode()`), the pins may change. The AI needs a way to re-inspect the node's current pins without re-reading the entire graph.

### 5.1: Schema Declaration

Add to `OliveBlueprintSchemas.h`, in the Reader Tool Schemas section (after `BlueprintListOverridableFunctions`):

```cpp
/**
 * Schema for blueprint.get_node_pins
 * Get the pin manifest for a specific node in a Blueprint graph.
 * Useful for re-inspecting pins after property changes or ReconstructNode.
 * Params: {path: string, graph: string, node_id: string}
 */
TSharedPtr<FJsonObject> BlueprintGetNodePins();
```

### 5.2: Schema Implementation

Add to `OliveBlueprintSchemas.cpp`, in the Reader Tool Schemas section:

```cpp
TSharedPtr<FJsonObject> BlueprintGetNodePins()
{
    TSharedPtr<FJsonObject> Properties = MakeProperties();

    Properties->SetObjectField(TEXT("path"),
        StringProp(TEXT("Blueprint asset path")));

    Properties->SetObjectField(TEXT("graph"),
        StringProp(TEXT("Graph name (e.g., 'EventGraph' or function name)")));

    Properties->SetObjectField(TEXT("node_id"),
        StringProp(TEXT("Node ID to inspect (e.g., 'node_0')")));

    TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
    Schema->SetStringField(TEXT("description"),
        TEXT("Get the pin manifest for a specific node. Returns all pin names, "
             "types, directions, defaults, and connection state. Useful after "
             "blueprint.set_node_property changes pins via ReconstructNode."));
    Schema->SetObjectField(TEXT("properties"), Properties);
    AddRequired(Schema, {TEXT("path"), TEXT("graph"), TEXT("node_id")});

    return Schema;
}
```

### 5.3: Handler Declaration

Add to `OliveBlueprintToolHandlers.h`, in the Reader Tool Handlers section (after `HandleBlueprintListOverridableFunctions`):

```cpp
FOliveToolResult HandleBlueprintGetNodePins(const TSharedPtr<FJsonObject>& Params);
```

### 5.4: Handler Implementation

Add to `OliveBlueprintToolHandlers.cpp`. The handler is a thin wrapper around the existing `BuildPinManifest()` anonymous namespace function:

```cpp
FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintGetNodePins(const TSharedPtr<FJsonObject>& Params)
{
    // Parse required parameters
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("path"), AssetPath))
    {
        return FOliveToolResult::Error(
            TEXT("VALIDATION_MISSING_PARAM"),
            TEXT("Missing required parameter 'path'"),
            TEXT("Provide the Blueprint asset path")
        );
    }

    FString GraphName;
    if (!Params->TryGetStringField(TEXT("graph"), GraphName))
    {
        return FOliveToolResult::Error(
            TEXT("VALIDATION_MISSING_PARAM"),
            TEXT("Missing required parameter 'graph'"),
            TEXT("Provide the graph name (e.g., 'EventGraph' or function name)")
        );
    }

    FString NodeId;
    if (!Params->TryGetStringField(TEXT("node_id"), NodeId))
    {
        return FOliveToolResult::Error(
            TEXT("VALIDATION_MISSING_PARAM"),
            TEXT("Missing required parameter 'node_id'"),
            TEXT("Provide the node ID (e.g., 'node_0')")
        );
    }

    // Load Blueprint
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!Blueprint)
    {
        return FOliveToolResult::Error(
            TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Blueprint not found at path '%s'"), *AssetPath),
            TEXT("Verify the asset path is correct")
        );
    }

    // Find the node via GraphWriter cache
    FOliveGraphWriter& GraphWriter = FOliveGraphWriter::Get();
    UEdGraphNode* Node = GraphWriter.GetCachedNode(AssetPath, NodeId);
    if (!Node)
    {
        return FOliveToolResult::Error(
            TEXT("NODE_NOT_FOUND"),
            FString::Printf(TEXT("Node '%s' not found in graph '%s'. "
                "Node IDs are assigned when nodes are created via add_node or apply_plan_json "
                "and are scoped to the graph."), *NodeId, *GraphName),
            TEXT("Use blueprint.read with include_pins:true to see all nodes in the graph")
        );
    }

    // Build pin manifest using existing helper
    TSharedPtr<FJsonObject> PinsData = BuildPinManifest(Node);

    // Build result
    TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
    ResultData->SetStringField(TEXT("asset_path"), AssetPath);
    ResultData->SetStringField(TEXT("graph"), GraphName);
    ResultData->SetStringField(TEXT("node_id"), NodeId);
    ResultData->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
    ResultData->SetObjectField(TEXT("pins"), PinsData);

    // Also report node title for context
    ResultData->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

    return FOliveToolResult::Success(ResultData);
}
```

**IMPORTANT NOTE on `BuildPinManifest` scope:** The `BuildPinManifest()` function is currently defined in the anonymous namespace at the top of `OliveBlueprintToolHandlers.cpp` (line 287). Since `HandleBlueprintGetNodePins` is a method of `FOliveBlueprintToolHandlers` defined in the same `.cpp` file, it has access to the anonymous namespace function. No header changes needed for this function.

### 5.5: Tool Registration

In `OliveBlueprintToolHandlers.cpp`, in the `RegisterReaderTools()` method (starting at line 469), add the new tool registration. Place it after the existing reader tool registrations:

```cpp
// blueprint.get_node_pins
Registry.RegisterTool(
    TEXT("blueprint.get_node_pins"),
    TEXT("Get pin manifest for a specific node in a Blueprint graph"),
    OliveBlueprintSchemas::BlueprintGetNodePins(),
    FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintGetNodePins),
    {TEXT("blueprint"), TEXT("read")},
    TEXT("blueprint")
);
RegisteredToolNames.Add(TEXT("blueprint.get_node_pins"));
```

**Tag: `read`** (not `write` or `graph`). This is a pure read operation.

### Test Plan for Task 5

1. Create a node via `add_node` (e.g., `type:"CallFunction"` with `properties:{"function_name":"PrintString"}`). Note the returned `node_id`.
2. Call `get_node_pins` with that `node_id`. Verify the returned pin manifest matches what `add_node` returned.
3. Call `set_node_property` to change the function (if applicable) or any property that triggers `ReconstructNode`.
4. Call `get_node_pins` again. Verify the pins reflect the new property state.
5. Call `get_node_pins` with an invalid `node_id`. Verify it returns `NODE_NOT_FOUND` error.

---

## Cross-Cutting Concerns

### Error Codes Added

| Code | Source | Description |
|------|--------|-------------|
| `NODE_NOT_FOUND` | Task 5 (get_node_pins) | Node ID not found in GraphWriter cache |

### Error Codes Modified (Guidance Updated)

| Code | Source | What Changed |
|------|--------|--------------|
| `NODE_TYPE_UNKNOWN` | Task 3 | Now mentions UK2Node class names as alternative |
| `BP_ADD_NODE_FAILED` | Task 3 | Class-not-found variant now mentions asset paths |
| `PLAN_RESOLVE_FAILED` | Task 3 | Event-related failures now suggest K2Node_ComponentBoundEvent |

### Build.cs Changes

None. All referenced K2Node headers (`K2Node.h`, `K2Node_ComponentBoundEvent.h`, etc.) are in modules already listed as dependencies in `OliveAIEditor.Build.cs` (specifically `BlueprintGraph`, `UnrealEd`). The `FindK2NodeClass` function uses `StaticLoadClass` which is in CoreUObject (always available).

Verify during compilation that `StaticLoadClass` compiles. If it does not, add `#include "UObject/UObjectGlobals.h"` in `OliveNodeFactory.cpp`.

### Knowledge Pack Ordering

The `node_routing.txt` pack loads after `blueprint_authoring.txt` and `recipe_routing.txt` (alphabetical order from directory iteration). This is fine -- the packs are concatenated, not ordered by priority.

### PlanExecutor Unchanged

The PlanExecutor (`FOlivePlanExecutor`) is NOT affected by these changes. It uses `GraphWriter.AddNode()` which calls `Factory.CreateNode()`. When the executor passes a resolved `NodeType` from the plan resolver, the type is always a curated OliveNodeTypes constant (the plan resolver maps ops to curated types). The universal fallback only activates for types NOT in the curated map, which only happens via direct `add_node` tool calls from the AI.

If we later want plan JSON to support arbitrary K2Node classes, that would be a separate change to the plan resolver.

---

## Implementation Order Summary

| Step | Task | Duration | Depends On |
|------|------|----------|------------|
| 1 | Task 1: Validator fix | 30 min | Nothing |
| 2 | Task 2: Error classification | 2 hr | Nothing |
| 3 | Task 3: Capability routing | 1 hr | Nothing (but logically comes after Task 2 since it updates same error messages) |
| 4 | Task 4: Universal add_node | 4-6 hr | Nothing (but logically comes after Task 3 since node_routing.txt references the new capability) |
| 5 | Task 5: get_node_pins | 1-2 hr | Task 4 (ships together, tests require universal nodes) |

**Build and test after each task.** Each task is independently compilable.
