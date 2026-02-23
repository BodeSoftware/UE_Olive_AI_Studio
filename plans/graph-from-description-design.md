# Graph-From-Description: Post-Creation Pin Introspection & Smart Wiring

## Revision
- **Version**: 1.0
- **Date**: 2026-02-22
- **Author**: Architect Agent

---

## Table of Contents

1. [Problem Analysis](#1-problem-analysis)
2. [Root Cause Decomposition](#2-root-cause-decomposition)
3. [Architecture Overview](#3-architecture-overview)
4. [New Data Structures](#4-new-data-structures)
5. [Enhanced Function Resolution](#5-enhanced-function-resolution)
6. [Pin Introspection Manifest](#6-pin-introspection-manifest)
7. [Smart Exec Wiring](#7-smart-exec-wiring)
8. [Smart Data Wiring](#8-smart-data-wiring)
9. [Enhanced @ref Syntax](#9-enhanced-ref-syntax)
10. [Auto-Layout Algorithm](#10-auto-layout-algorithm)
11. [Plan Path Enforcement](#11-plan-path-enforcement)
12. [Error Recovery & Partial Success](#12-error-recovery--partial-success)
13. [File Structure & Module Boundaries](#13-file-structure--module-boundaries)
14. [Integration Points](#14-integration-points)
15. [Implementation Phases](#15-implementation-phases)
16. [Testing Strategy](#16-testing-strategy)
17. [Migration Path](#17-migration-path)
18. [Edge Cases & Error Handling](#18-edge-cases--error-handling)

---

## 1. Problem Analysis

### The Gun Blueprint Failure (22/45 = 49% failure rate)

When an AI agent attempted to create a "gun that shoots a bullet" Blueprint, it generated 45 tool calls. Here is the breakdown of every failure category with exact counts and root causes:

#### 1.1 Component Addition Failures (5 failures)

The AI guessed wrong parameter formats for `add_component`. This is outside the scope of this design (component tools have their own schema), but it illustrates the general "guess and fail" pattern.

#### 1.2 Variable Addition Failure (1 failure)

The AI could not resolve `TSubclassOf<AActor>` as a variable type. The variable type system uses IR types (`EOliveIRTypeCategory`), which map differently from how the AI naturally describes UE types.

#### 1.3 Batch Write Function Name Failures (3 failures)

The AI guessed `K2_GetComponentToWorld` -- a function that does not exist. The actual UE 5.5 functions are:
- `K2_GetComponentLocation` (returns FVector)
- `K2_GetComponentRotation` (returns FRotator)
- `GetComponentTransform` (returns FTransform)

The current `FindFunction()` does exact `FindFunctionByName` with a hardcoded list of 3 fallback classes (`UKismetSystemLibrary`, `UObject`, `AActor`). It has zero fuzzy matching and zero alias resolution.

#### 1.4 Pin Connection Failures (9 failures -- ALL wrong pin names)

This is the catastrophic failure mode. Every single `connect_pins` call failed because the AI guessed pin names:

| AI Guessed | Actual UE Pin Name | Node Type |
|------------|-------------------|-----------|
| `then` | `Then` (note case) | K2Node_CallFunction |
| `execute` | `execute` (correct but node may use different name) | K2Node_CallFunction |
| `self` | `self` (hidden pin, not connectable on static functions) | Static library call |
| `ReturnValue` | `ReturnValue` (exists but on wrong node) | Mismatch |
| `ProjectileClass` | Not a real pin | SpawnActor (actual: `Class`) |
| `FireRate` | Not a real pin name | Variable access (actual: auto-generated name) |
| `bCanFire` | Not a real pin name | Variable (actual: BP-generated pin name) |
| `FunctionName` | Not a real pin name | Delegate bind (actual varies) |

The pattern is clear: **the AI does not know UE's internal pin naming conventions and never will**. Pin names in UE are:
- Case-sensitive (`Then` vs `then`)
- Often prefixed by the engine (`self`, `WorldContextObject`)
- Generated from C++ parameter names (`InString` not `String`, `Duration` not `Time`)
- Inconsistent across node types (Branch uses `Condition`, other nodes use different names)

#### 1.5 Set Pin Default Failures (2 failures)

Same root cause as pin connection failures -- the AI guessed wrong pin names for setting default values.

### Why the Plan Path Was Not Used

The worker prompt tells the AI to use the Plan JSON path for 3+ node operations. But the AI fell back to granular tools (`project.batch_write` with individual `connect_pins` calls) because:

1. The Plan JSON system's `@stepId.pinName` syntax still requires knowing pin names
2. The lowerer hardcodes `.then` and `.execute` for exec pins -- if the node uses different names, the connection fails silently
3. There is no feedback loop: when the plan path is available but the AI uses granular tools, nothing warns it

### The Fundamental Insight

After `FOliveNodeFactory::CreateNode()` calls `AllocateDefaultPins()`, the returned `UEdGraphNode*` has a fully populated `Pins` array. Every pin's exact name, direction, type, and display name are available via the UE reflection system. **The system already has all the information it needs -- it just throws it away and asks the AI to guess instead.**

---

## 2. Root Cause Decomposition

Each failure maps to a specific system deficiency:

| # | Root Cause | Current Behavior | Required Behavior |
|---|-----------|-----------------|-------------------|
| RC1 | Function name resolution is exact-match only | `FindFunction("GetComponentTransform")` fails because actual name is `K2_GetComponentLocation` | Fuzzy search across all loaded classes, alias map for common names, fall back to catalog |
| RC2 | Pin names must be known before node creation | AI guesses `then`, `execute`, `ReturnValue` | System introspects real pins after node creation |
| RC3 | Exec pin names are hardcoded in lowerer | `${s1.node_id}.then` -> `${s2.node_id}.execute` always | Introspect actual exec output/input pin names from `UEdGraphNode::Pins` |
| RC4 | Data pin references require exact names | `@s1.ReturnValue` requires knowing the pin is called "ReturnValue" | Support `@s1.~Location` (fuzzy), `@s1.result` (semantic), `@s1.auto` (type-match) |
| RC5 | No pin fallback chain | `FindPin` tries exact -> display name -> case-insensitive -> trimmed, then gives up | Add fuzzy match and type-based matching with suggestions |
| RC6 | Layout is trivial | `StepIndex * 300, 0` -- all nodes on one row | Exec-flow-aware layout with branching |
| RC7 | No plan path enforcement | AI can use granular tools even when plan path is better | Emit warnings when granular tools are used for 3+ ops |
| RC8 | Failures are fatal | First connection failure aborts entire batch | Continue with remaining connections, report all failures |

---

## 3. Architecture Overview

### Current Pipeline (v1.0)

```
Plan JSON --> [Resolve] --> [Lower] --> [Execute Batch]
                |               |              |
           Map ops to       Emit flat      Dispatch
           node types       op list        to writers
                                |
                           HARDCODED pin names
                           NO pin introspection
                           ABORT on first failure
```

### Enhanced Pipeline (v2.0)

```
Plan JSON --> [Resolve+] --> [Create Nodes] --> [Introspect] --> [Wire] --> [Defaults]
                 |                |                  |              |           |
            Smart fuzzy     Node creation      Build pin       Smart pin   Smart pin
            function       via factory +      manifest per    matching:    matching:
            resolution     cache in writer    created node    exec + data  name + type
                                                  |
                                          FOlivePinManifest
                                          (exact pin names,
                                           types, directions)
```

The key architectural change: **split the lowerer's single-pass "emit all ops" into a multi-phase executor that creates nodes first, introspects their pins, then wires them using the introspection data.**

### System Boundary

```
+------------------------------------------------------------------+
|  HandleBlueprintApplyPlanJson (tool handler)                      |
|                                                                    |
|  1. Parse + validate plan (existing)                               |
|  2. Load Blueprint (existing)                                      |
|  3. Resolve plan steps (ENHANCED: smart function resolution)       |
|  4. Execute via NEW FOlivePlanExecutor:                            |
|     a. Phase 1: Create all nodes -> get UEdGraphNode* pointers     |
|     b. Phase 2: Introspect all pins -> build FOlivePinManifest     |
|     c. Phase 3: Wire exec connections (using manifest)             |
|     d. Phase 4: Wire data connections (using manifest)             |
|     e. Phase 5: Set pin defaults (using manifest)                  |
|     f. Phase 6: Auto-layout (using exec flow topology)             |
|  5. Compile + verify (existing)                                    |
|  6. Return result with manifest summary                            |
+------------------------------------------------------------------+
```

### What Changes vs What Stays

| Component | Change Level | Details |
|-----------|-------------|---------|
| `BlueprintPlanIR.h` | **Additive** | New `@ref` syntax variants parsed by existing `FromJson` |
| `OliveBlueprintPlanResolver.cpp` | **Enhanced** | `ResolveCallOp` gets smart fuzzy resolution |
| `OliveBlueprintPlanLowerer.h/cpp` | **Bypassed** | New executor replaces the lowerer for v2.0 plans; lowerer remains for backward compat |
| `OliveNodeFactory.h/cpp` | **Enhanced** | `FindFunction` gets fuzzy search + alias map |
| `OliveGraphWriter.h/cpp` | **Enhanced** | New `FindPinSmart()` method with fallback chain |
| `OliveNodeCatalog.h/cpp` | **Enhanced** | New `SearchFunctions()` method for function-specific search |
| `OliveBlueprintToolHandlers.cpp` | **Enhanced** | `HandleBlueprintApplyPlanJson` uses new executor |
| NEW: `OlivePlanExecutor.h/cpp` | **New** | Multi-phase plan execution with pin introspection |
| NEW: `OlivePinManifest.h/cpp` | **New** | Pin introspection data structures and builder |
| NEW: `OliveFunctionResolver.h/cpp` | **New** | Smart function resolution with fuzzy matching |
| NEW: `OliveGraphLayoutEngine.h/cpp` | **New** | Exec-flow-aware auto-layout |

---

## 4. New Data Structures

### 4.1 FOlivePinManifestEntry

Represents a single pin's introspected metadata from a real `UEdGraphNode*`.

```cpp
// File: Source/OliveAIEditor/Blueprint/Public/Plan/OlivePinManifest.h

/**
 * Introspected pin data from a real UEdGraphNode after creation.
 * This is ground truth -- these are the ACTUAL pin names, not guesses.
 */
struct OLIVEAIEDITOR_API FOlivePinManifestEntry
{
    /** Exact internal pin name as returned by UEdGraphPin::GetName() */
    FString PinName;

    /** Display name as returned by UEdGraphPin::GetDisplayName().ToString() */
    FString DisplayName;

    /** Pin direction: true = input, false = output */
    bool bIsInput = true;

    /** Pin category from UEdGraphPin::PinType */
    FString PinCategory;         // e.g., "exec", "bool", "float", "object"

    /** Pin subcategory (class name for objects, struct name for structs) */
    FString PinSubCategory;      // e.g., "Actor", "Vector"

    /** Full type string for AI-readable description */
    FString TypeDisplayString;   // e.g., "bool", "float", "Actor Object Reference"

    /** Whether this is an execution (flow control) pin */
    bool bIsExec = false;

    /** Whether this pin is hidden (e.g., self pin on static functions) */
    bool bIsHidden = false;

    /** Whether this pin has a default value */
    bool bHasDefaultValue = false;

    /** Current default value (for input pins) */
    FString DefaultValue;

    /** Whether this pin is currently connected */
    bool bIsConnected = false;

    /** IR type category for type-compatibility matching */
    EOliveIRTypeCategory IRTypeCategory = EOliveIRTypeCategory::Unknown;
};
```

### 4.2 FOlivePinManifest

Complete pin manifest for a single created node.

```cpp
/**
 * Complete pin manifest for a single created node.
 * Built by introspecting the real UEdGraphNode* after CreateNode().
 *
 * This is the contract between node creation (Phase 1) and
 * wiring (Phases 3-5). All pin references in wiring phases
 * resolve against this manifest, never against AI-guessed names.
 */
struct OLIVEAIEDITOR_API FOlivePinManifest
{
    /** The step ID from the plan that created this node */
    FString StepId;

    /** The node ID assigned by GraphWriter (e.g., "node_0") */
    FString NodeId;

    /** The node type (e.g., "CallFunction", "Branch") */
    FString NodeType;

    /** For CallFunction: the resolved function name */
    FString ResolvedFunctionName;

    /** Whether this node has exec flow (has at least one exec pin) */
    bool bHasExecPins = false;

    /** Whether this is a pure node (no exec pins) */
    bool bIsPure = false;

    /** All pins on this node */
    TArray<FOlivePinManifestEntry> Pins;

    // ====================================================================
    // Query Methods
    // ====================================================================

    /**
     * Find the primary exec input pin (the "execute" pin).
     * For most nodes, there is exactly one. Returns nullptr if pure node.
     */
    const FOlivePinManifestEntry* FindExecInput() const;

    /**
     * Find the primary exec output pin (the "then" pin).
     * For most nodes, there is exactly one. Returns nullptr if pure node.
     */
    const FOlivePinManifestEntry* FindExecOutput() const;

    /**
     * Find all exec output pins (e.g., Branch has "True" and "False").
     * @return Array of pointers to exec output pin entries
     */
    TArray<const FOlivePinManifestEntry*> FindAllExecOutputs() const;

    /**
     * Find a data input pin by exact name.
     * @param Name Pin name to search for
     * @return Pointer to the pin entry, or nullptr
     */
    const FOlivePinManifestEntry* FindDataInputByName(const FString& Name) const;

    /**
     * Find a data output pin by exact name.
     * @param Name Pin name to search for
     * @return Pointer to the pin entry, or nullptr
     */
    const FOlivePinManifestEntry* FindDataOutputByName(const FString& Name) const;

    /**
     * Find a pin using the smart fallback chain:
     * exact name -> display name -> case-insensitive -> fuzzy -> type-match.
     *
     * @param Hint The name hint from the plan (may be approximate)
     * @param bIsInput Whether to search input or output pins
     * @param TypeHint Optional: expected type category for type-based fallback
     * @param OutMatchMethod Set to the method that matched (for diagnostics)
     * @return Pointer to the best matching pin entry, or nullptr
     */
    const FOlivePinManifestEntry* FindPinSmart(
        const FString& Hint,
        bool bIsInput,
        EOliveIRTypeCategory TypeHint = EOliveIRTypeCategory::Unknown,
        FString* OutMatchMethod = nullptr) const;

    /**
     * Get all non-hidden, non-exec data pins in a given direction.
     * @param bInput True for input pins, false for output pins
     * @return Array of pointers to data pin entries
     */
    TArray<const FOlivePinManifestEntry*> GetDataPins(bool bInput) const;

    /**
     * Serialize manifest to JSON for inclusion in apply result.
     * Useful for debugging and for the AI to understand what was created.
     */
    TSharedPtr<FJsonObject> ToJson() const;

    // ====================================================================
    // Static Builder
    // ====================================================================

    /**
     * Build a manifest by introspecting a real UEdGraphNode.
     * This is the core factory method -- it reads every pin on the node
     * and populates the manifest with ground-truth data.
     *
     * @param Node The created node to introspect
     * @param StepId The plan step ID that created this node
     * @param NodeId The GraphWriter node ID assigned to this node
     * @param NodeType The OliveNodeTypes constant
     * @return Populated manifest
     */
    static FOlivePinManifest Build(
        UEdGraphNode* Node,
        const FString& StepId,
        const FString& NodeId,
        const FString& NodeType);
};
```

### 4.3 FOlivePlanExecutionContext

Shared state across all phases of plan execution.

```cpp
/**
 * Execution context for multi-phase plan application.
 * Holds the manifests built during node creation and provides
 * lookup methods for wiring phases.
 */
struct OLIVEAIEDITOR_API FOlivePlanExecutionContext
{
    /** The target Blueprint */
    UBlueprint* Blueprint = nullptr;

    /** The target graph */
    UEdGraph* Graph = nullptr;

    /** Asset path for GraphWriter cache operations */
    FString AssetPath;

    /** Graph name for GraphWriter operations */
    FString GraphName;

    /** Step ID -> Pin Manifest mapping (populated during Phase 1) */
    TMap<FString, FOlivePinManifest> StepManifests;

    /** Step ID -> Node ID mapping (populated during Phase 1) */
    TMap<FString, FString> StepToNodeMap;

    /** Step ID -> UEdGraphNode* mapping (populated during Phase 1) */
    TMap<FString, UEdGraphNode*> StepToNodePtr;

    /** Accumulated warnings (non-fatal) */
    TArray<FString> Warnings;

    /** Accumulated wiring errors (non-fatal, reported at end) */
    TArray<FOliveIRBlueprintPlanError> WiringErrors;

    /** Count of successfully created nodes */
    int32 CreatedNodeCount = 0;

    /** Count of successfully made connections */
    int32 SuccessfulConnectionCount = 0;

    /** Count of failed connections */
    int32 FailedConnectionCount = 0;

    /** Count of successfully set pin defaults */
    int32 SuccessfulDefaultCount = 0;

    /** Count of failed pin default sets */
    int32 FailedDefaultCount = 0;

    // ====================================================================
    // Lookup Methods
    // ====================================================================

    /** Get manifest for a step, or nullptr if step was not created */
    const FOlivePinManifest* GetManifest(const FString& StepId) const;

    /** Get the node pointer for a step, or nullptr */
    UEdGraphNode* GetNodePtr(const FString& StepId) const;

    /** Get the node ID for a step, or empty string */
    FString GetNodeId(const FString& StepId) const;
};
```

### 4.4 FOliveSmartWireResult

Result of a single smart wiring attempt.

```cpp
/**
 * Result of attempting to wire two pins using smart resolution.
 */
struct OLIVEAIEDITOR_API FOliveSmartWireResult
{
    bool bSuccess = false;

    /** How the source pin was matched (for diagnostics) */
    FString SourceMatchMethod;  // "exact", "display_name", "case_insensitive", "fuzzy", "type_match", "exec_primary"

    /** How the target pin was matched */
    FString TargetMatchMethod;

    /** Actual source pin name used */
    FString ResolvedSourcePin;

    /** Actual target pin name used */
    FString ResolvedTargetPin;

    /** Error message if failed */
    FString ErrorMessage;

    /** Suggestions if failed (actual pin names on the node) */
    TArray<FString> Suggestions;
};
```

---

## 5. Enhanced Function Resolution

### 5.1 Problem

The current `FOliveNodeFactory::FindFunction()` searches exactly 3 classes (`UKismetSystemLibrary`, `UObject`, `AActor`) and does exact `FindFunctionByName`. When the AI says `GetComponentTransform`, it fails because:

1. The function might be called `K2_GetComponentLocation` (a K2-prefixed wrapper)
2. The function might be on `USceneComponent`, not in the 3 hardcoded classes
3. The AI doesn't know about UE's `K2_` prefix convention

### 5.2 New Class: FOliveFunctionResolver

```cpp
// File: Source/OliveAIEditor/Blueprint/Public/Plan/OliveFunctionResolver.h

DECLARE_LOG_CATEGORY_EXTERN(LogOliveFunctionResolver, Log, All);

/**
 * Result of function resolution with match quality information.
 */
struct OLIVEAIEDITOR_API FOliveFunctionMatch
{
    /** The resolved UFunction pointer */
    UFunction* Function = nullptr;

    /** The class that owns this function */
    UClass* OwningClass = nullptr;

    /** How the function was found */
    enum class EMatchMethod : uint8
    {
        ExactName,           // Exact FindFunctionByName match
        K2Prefix,            // Added/removed K2_ prefix
        Alias,               // Common alias map (e.g., "Print" -> "PrintString")
        CatalogExact,        // Exact match in node catalog
        CatalogFuzzy,        // Fuzzy match in node catalog (score >= threshold)
        BroadClassSearch,    // Found by iterating all Blueprint Function Libraries
        ParentClassSearch,   // Found on the Blueprint's parent class hierarchy
    };

    EMatchMethod MatchMethod = EMatchMethod::ExactName;

    /** Match confidence score (0-100). 100 = exact, lower = fuzzier */
    int32 Confidence = 0;

    /** Display name for error messages */
    FString DisplayName;
};

/**
 * FOliveFunctionResolver
 *
 * Smart function resolution that goes far beyond exact name matching.
 * Implements a prioritized fallback chain to find the UFunction* that
 * best matches an AI-provided function name.
 *
 * Resolution order:
 *   1. Exact FindFunctionByName on TargetClass (if provided)
 *   2. Exact match in the 3 core library classes + Blueprint parent hierarchy
 *   3. K2_ prefix manipulation (add K2_ prefix, remove K2_ prefix)
 *   4. Common alias map (static table of known aliases)
 *   5. Node catalog exact match on function name or display name
 *   6. Node catalog fuzzy match (score >= MinAutoMatchScore)
 *   7. Broad search: iterate all UBlueprintFunctionLibrary subclasses
 *
 * If multiple candidates are found at the same priority level, the
 * resolver picks the one whose class is closest to the Blueprint's
 * parent class in the inheritance hierarchy. If still ambiguous,
 * it returns an error with all candidates listed.
 *
 * Thread Safety: Stateless, all methods are static. Thread-safe for reads.
 */
class OLIVEAIEDITOR_API FOliveFunctionResolver
{
public:
    /**
     * Resolve a function name to a UFunction*.
     *
     * @param FunctionName   Name as provided by the AI (may be approximate)
     * @param TargetClass    Optional class name for disambiguation
     * @param Blueprint      The target Blueprint (for parent class hierarchy search)
     * @return Match result with function pointer and confidence
     */
    static FOliveFunctionMatch Resolve(
        const FString& FunctionName,
        const FString& TargetClass,
        UBlueprint* Blueprint);

    /**
     * Get all candidates for a function name (for "did you mean?" suggestions).
     *
     * @param FunctionName   Name to search for
     * @param MaxResults     Maximum candidates to return
     * @return Array of matches sorted by confidence descending
     */
    static TArray<FOliveFunctionMatch> GetCandidates(
        const FString& FunctionName,
        int32 MaxResults = 5);

private:
    // ====================================================================
    // Resolution Strategies (tried in order)
    // ====================================================================

    /** Strategy 1: Exact name match on a specific class */
    static UFunction* TryExactMatch(
        const FString& FunctionName,
        UClass* Class);

    /** Strategy 2: Try adding or removing the K2_ prefix */
    static UFunction* TryK2PrefixMatch(
        const FString& FunctionName,
        UClass* Class,
        FString& OutResolvedName);

    /** Strategy 3: Look up in the common alias map */
    static UFunction* TryAliasMatch(
        const FString& FunctionName,
        FString& OutResolvedName);

    /** Strategy 4: Search the node catalog */
    static FOliveFunctionMatch TryCatalogMatch(
        const FString& FunctionName);

    /** Strategy 5: Broad search across all function libraries */
    static TArray<FOliveFunctionMatch> BroadSearch(
        const FString& FunctionName,
        int32 MaxResults);

    // ====================================================================
    // Alias Map
    // ====================================================================

    /**
     * Get the static alias map.
     * Maps common short names to actual UE function names.
     */
    static const TMap<FString, FString>& GetAliasMap();

    /**
     * Get the class search order for a given Blueprint.
     * Returns: TargetClass (if any), parent hierarchy, then common libraries.
     */
    static TArray<UClass*> GetSearchOrder(
        const FString& TargetClass,
        UBlueprint* Blueprint);

    /** Minimum fuzzy match score to accept a catalog result */
    static constexpr int32 MinFuzzyScore = 60;
};
```

### 5.3 Alias Map (Static Data)

The alias map covers the most common AI mistakes observed in logs:

```cpp
// In OliveFunctionResolver.cpp

const TMap<FString, FString>& FOliveFunctionResolver::GetAliasMap()
{
    static const TMap<FString, FString> Aliases = {
        // Transform / Location
        {TEXT("GetComponentTransform"), TEXT("K2_GetComponentLocation")},
        {TEXT("GetWorldTransform"), TEXT("GetComponentTransform")},
        {TEXT("GetLocation"), TEXT("K2_GetActorLocation")},
        {TEXT("SetLocation"), TEXT("K2_SetActorLocation")},
        {TEXT("GetActorLocation"), TEXT("K2_GetActorLocation")},
        {TEXT("SetActorLocation"), TEXT("K2_SetActorLocation")},
        {TEXT("GetRotation"), TEXT("K2_GetActorRotation")},
        {TEXT("SetRotation"), TEXT("K2_SetActorRotation")},
        {TEXT("GetActorRotation"), TEXT("K2_GetActorRotation")},
        {TEXT("SetActorRotation"), TEXT("K2_SetActorRotation")},
        {TEXT("GetWorldLocation"), TEXT("K2_GetComponentLocation")},
        {TEXT("GetWorldRotation"), TEXT("K2_GetComponentRotation")},
        {TEXT("GetForwardVector"), TEXT("GetActorForwardVector")},
        {TEXT("GetRightVector"), TEXT("GetActorRightVector")},
        {TEXT("GetUpVector"), TEXT("GetActorUpVector")},

        // Spawning
        {TEXT("SpawnActor"), TEXT("BeginDeferredActorSpawnFromClass")},
        {TEXT("SpawnActorFromClass"), TEXT("BeginDeferredActorSpawnFromClass")},

        // Math
        {TEXT("Add"), TEXT("Add_VectorVector")},
        {TEXT("Subtract"), TEXT("Subtract_VectorVector")},
        {TEXT("Multiply"), TEXT("Multiply_VectorFloat")},
        {TEXT("Normalize"), TEXT("Normal")},
        {TEXT("VectorLength"), TEXT("VSize")},
        {TEXT("Distance"), TEXT("GetDistanceTo")},
        {TEXT("Lerp"), TEXT("Lerp")},
        {TEXT("Clamp"), TEXT("FClamp")},

        // String
        {TEXT("Print"), TEXT("PrintString")},
        {TEXT("ToString"), TEXT("Conv_IntToString")},
        {TEXT("Concatenate"), TEXT("Concat_StrStr")},
        {TEXT("Format"), TEXT("Format")},

        // Object
        {TEXT("Destroy"), TEXT("K2_DestroyActor")},
        {TEXT("DestroyActor"), TEXT("K2_DestroyActor")},
        {TEXT("GetOwner"), TEXT("GetOwner")},
        {TEXT("IsValid"), TEXT("IsValid")},
        {TEXT("GetClass"), TEXT("GetClass")},

        // Component
        {TEXT("GetComponentByClass"), TEXT("GetComponentByClass")},
        {TEXT("AddComponent"), TEXT("AddComponent")},

        // Timer
        {TEXT("SetTimer"), TEXT("K2_SetTimer")},
        {TEXT("SetTimerByFunctionName"), TEXT("K2_SetTimerByFunctionName")},
        {TEXT("ClearTimer"), TEXT("K2_ClearTimer")},

        // Physics
        {TEXT("AddForce"), TEXT("AddForce")},
        {TEXT("AddImpulse"), TEXT("AddImpulse")},
        {TEXT("SetSimulatePhysics"), TEXT("SetSimulatePhysics")},

        // Input
        {TEXT("GetInputAxisValue"), TEXT("GetInputAxisValue")},
        {TEXT("GetInputAxisKeyValue"), TEXT("GetInputAxisKeyValue")},
    };
    return Aliases;
}
```

### 5.4 K2 Prefix Strategy

```
Algorithm: TryK2PrefixMatch(FunctionName, Class)

1. If FunctionName starts with "K2_":
   a. Strip prefix: TryName = FunctionName.Mid(3)  // "K2_GetActorLocation" -> "GetActorLocation"
   b. Try FindFunctionByName(Class, TryName)
   c. If found, return it

2. If FunctionName does NOT start with "K2_":
   a. Add prefix: TryName = "K2_" + FunctionName  // "GetActorLocation" -> "K2_GetActorLocation"
   b. Try FindFunctionByName(Class, TryName)
   c. If found, return it

3. Return nullptr (no match)
```

### 5.5 Integration with PlanResolver

The `ResolveCallOp` method in `OliveBlueprintPlanResolver.cpp` is enhanced to use `FOliveFunctionResolver` instead of the direct catalog search:

```cpp
// BEFORE (current):
Out.Properties.Add(TEXT("function_name"), Step.Target);
// ... catalog search for disambiguation ...
// Accepts unresolved functions with warning

// AFTER (enhanced):
FOliveFunctionMatch Match = FOliveFunctionResolver::Resolve(
    Step.Target, Step.TargetClass, Blueprint);

if (Match.Function)
{
    Out.Properties.Add(TEXT("function_name"), Match.Function->GetName());
    Out.Properties.Add(TEXT("target_class"), Match.OwningClass->GetName());
    if (Match.Confidence < 90)
    {
        Warnings.Add(FString::Printf(
            TEXT("Step '%s': '%s' resolved to '%s::%s' (confidence: %d, method: %s)"),
            *Step.StepId, *Step.Target,
            *Match.OwningClass->GetName(), *Match.Function->GetName(),
            Match.Confidence, *MatchMethodToString(Match.MatchMethod)));
    }
}
else
{
    TArray<FOliveFunctionMatch> Candidates = FOliveFunctionResolver::GetCandidates(Step.Target);
    TArray<FString> Alternatives;
    for (const FOliveFunctionMatch& C : Candidates)
    {
        Alternatives.Add(FString::Printf(TEXT("%s::%s"),
            *C.OwningClass->GetName(), *C.Function->GetName()));
    }
    Errors.Add(FOliveIRBlueprintPlanError::MakeStepError(
        TEXT("FUNCTION_NOT_FOUND"), Step.StepId, ...
        , TEXT("Did you mean: ") + FString::Join(Alternatives, TEXT(", "))));
    Error.Alternatives = Alternatives;
}
```

---

## 6. Pin Introspection Manifest

### 6.1 How Introspection Works

After `FOliveNodeFactory::CreateNode()` returns a `UEdGraphNode*`, we iterate its `Pins` array. This is the **ground truth** -- no guessing involved.

```
Algorithm: FOlivePinManifest::Build(Node, StepId, NodeId, NodeType)

1. Initialize manifest with StepId, NodeId, NodeType
2. For each UEdGraphPin* Pin in Node->Pins:
   a. Skip if Pin->bHidden AND NOT Pin->bAdvancedView
      (Hidden pins like WorldContextObject should not be wirable)
   b. Create FOlivePinManifestEntry:
      - PinName = Pin->GetName()           // "Then", "ReturnValue", "InString"
      - DisplayName = Pin->GetDisplayName().ToString()  // "Then", "Return Value", "In String"
      - bIsInput = (Pin->Direction == EGPD_Input)
      - PinCategory = Pin->PinType.PinCategory.ToString()  // "exec", "bool", "float", "object"
      - PinSubCategory = Pin->PinType.PinSubCategoryObject ?
          Pin->PinType.PinSubCategoryObject->GetName() : Pin->PinType.PinSubCategory.ToString()
      - TypeDisplayString = UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString()
      - bIsExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
      - bIsHidden = Pin->bHidden
      - bHasDefaultValue = !Pin->DefaultValue.IsEmpty() || !Pin->AutogeneratedDefaultValue.IsEmpty()
      - DefaultValue = Pin->DefaultValue.IsEmpty() ? Pin->AutogeneratedDefaultValue : Pin->DefaultValue
      - bIsConnected = Pin->HasAnyConnections()
      - IRTypeCategory = ConvertPinTypeToIRCategory(Pin->PinType)
   c. Add entry to manifest.Pins
3. Set bHasExecPins = (any pin is exec)
4. Set bIsPure = !bHasExecPins
5. Return manifest
```

### 6.2 The FindPinSmart Fallback Chain

This is the core algorithm that replaces guessing with introspection:

```
Algorithm: FOlivePinManifest::FindPinSmart(Hint, bIsInput, TypeHint, OutMatchMethod)

Input: Hint (the AI-provided pin name, may be wrong)
       bIsInput (direction to search)
       TypeHint (optional: expected EOliveIRTypeCategory)

1. EXACT NAME MATCH
   For each pin in Pins where pin.bIsInput == bIsInput AND NOT pin.bIsExec AND NOT pin.bIsHidden:
     If pin.PinName == Hint:
       Set *OutMatchMethod = "exact"
       Return &pin

2. DISPLAY NAME MATCH
   For each matching pin:
     If pin.DisplayName == Hint:
       Set *OutMatchMethod = "display_name"
       Return &pin

3. CASE-INSENSITIVE MATCH
   For each matching pin:
     If pin.PinName.Equals(Hint, IgnoreCase) OR pin.DisplayName.Equals(Hint, IgnoreCase):
       Set *OutMatchMethod = "case_insensitive"
       Return &pin

4. FUZZY MATCH (Levenshtein distance + substring)
   Best = nullptr, BestScore = 0
   For each matching pin:
     Score = 0
     // Substring bonus
     If pin.PinName.Contains(Hint, IgnoreCase) OR Hint.Contains(pin.PinName, IgnoreCase):
       Score += 60
     If pin.DisplayName.Contains(Hint, IgnoreCase) OR Hint.Contains(pin.DisplayName, IgnoreCase):
       Score += 50
     // Levenshtein penalty (lower distance = higher score)
     LevenDist = LevenshteinDistance(Hint.ToLower(), pin.PinName.ToLower())
     If LevenDist <= 3:
       Score += max(0, 40 - LevenDist * 10)
     If Score > BestScore:
       BestScore = Score
       Best = &pin
   If Best != nullptr AND BestScore >= 40:
     Set *OutMatchMethod = "fuzzy"
     Return Best

5. TYPE-BASED MATCH (last resort)
   If TypeHint != Unknown:
     Candidates = all matching pins where pin.IRTypeCategory == TypeHint
     If Candidates.Num() == 1:
       Set *OutMatchMethod = "type_match"
       Return Candidates[0]
     // If multiple type matches, no unique resolution -- fall through to failure

6. FAILURE
   Return nullptr
```

### 6.3 What the Manifest Enables

With the manifest, the wiring phases no longer need to guess:

| Scenario | Old Behavior | New Behavior |
|---------|-------------|-------------|
| Connect exec flow from BeginPlay to PrintString | Hardcode `.then` -> `.execute` | `Source.FindExecOutput()->PinName` -> `Target.FindExecInput()->PinName` |
| Connect data: Branch condition | AI guesses `@s1.ReturnValue` -> `Condition` | `Source.FindPinSmart("ReturnValue", false)` -> `Target.FindPinSmart("Condition", true)` |
| Set default on PrintString's text | AI guesses pin name `InString` | `Target.FindPinSmart("InString", true)` resolves to actual `InString` pin |
| Connect SpawnActor's Class pin | AI guesses `ProjectileClass` | `Target.FindPinSmart("ProjectileClass", true, Object)` fuzzy matches to `Class` pin |

---

## 7. Smart Exec Wiring

### 7.1 Problem

The current lowerer hardcodes exec pin names:

```cpp
// Current (broken):
Params->SetStringField(TEXT("source"),
    FString::Printf(TEXT("${%s.node_id}.then"), *PlanStep.ExecAfter));
Params->SetStringField(TEXT("target"),
    FString::Printf(TEXT("${%s.node_id}.execute"), *PlanStep.StepId));
```

This fails because:
- Some nodes have no pin literally named `then` (they use `Then` with capital T)
- Event nodes have exec output but no `then` -- it is just the only output exec pin
- Branch nodes have `True` and `False` as exec outputs, not `then`
- Sequence nodes have `Then 0`, `Then 1`, etc.

### 7.2 Solution: Introspect-based Exec Wiring

In Phase 3 (Wire Exec Connections), for each `exec_after` and `exec_outputs` entry:

```
Algorithm: WireExecConnection(SourceStepId, SourcePinHint, TargetStepId, Context)

Inputs:
  SourceStepId: step whose exec OUTPUT we want
  SourcePinHint: optional hint for which exec output (e.g., "True", "False", empty for primary)
  TargetStepId: step whose exec INPUT we want to connect to
  Context: FOlivePlanExecutionContext

1. Get source and target manifests:
   SourceManifest = Context.GetManifest(SourceStepId)
   TargetManifest = Context.GetManifest(TargetStepId)
   If either is null, report error "Step not found", continue to next wire

2. Resolve target exec input pin:
   TargetExecIn = TargetManifest->FindExecInput()
   If TargetExecIn is null:
     If TargetManifest->bIsPure:
       Report warning "Target step '{TargetStepId}' is a pure node, skipping exec wire"
       Return success (pure nodes don't need exec wiring)
     Else:
       Report error "No exec input pin found on target step '{TargetStepId}'"
       Return failure

3. Resolve source exec output pin:
   If SourcePinHint is empty:
     // Default: use the primary (first) exec output
     SourceExecOut = SourceManifest->FindExecOutput()
   Else:
     // Specific pin requested (e.g., "True" for Branch)
     SourceExecOut = SourceManifest->FindPinSmart(SourcePinHint, false /*output*/)
     If SourceExecOut is null OR NOT SourceExecOut->bIsExec:
       // Fallback: try matching against all exec outputs
       AllExecOuts = SourceManifest->FindAllExecOutputs()
       for each ExecOut in AllExecOuts:
         if ExecOut->DisplayName.Equals(SourcePinHint, IgnoreCase)
            OR ExecOut->PinName.Equals(SourcePinHint, IgnoreCase):
           SourceExecOut = ExecOut
           break

   If SourceExecOut is null:
     Report error with suggestions: "No exec output pin matching '{SourcePinHint}' on step '{SourceStepId}'. Available exec outputs: {list}"
     Return failure

4. Make the connection:
   SourcePinRef = Context.GetNodeId(SourceStepId) + "." + SourceExecOut->PinName
   TargetPinRef = Context.GetNodeId(TargetStepId) + "." + TargetExecIn->PinName
   Result = FOliveGraphWriter::Get().ConnectPins(Context.AssetPath, Context.GraphName, SourcePinRef, TargetPinRef)
   Return Result
```

### 7.3 exec_after Resolution

For `exec_after`, the AI specifies which step should precede this one. The current lowerer converts this to:
- Source: `${exec_after_step}.then`
- Target: `${this_step}.execute`

With smart wiring:
- Source: `SourceManifest.FindExecOutput()->PinName` (introspected, ground truth)
- Target: `TargetManifest.FindExecInput()->PinName` (introspected, ground truth)

### 7.4 exec_outputs Resolution

For `exec_outputs`, the AI specifies named exec output pins (e.g., `"True": "step_on_true"`, `"False": "step_on_false"`). The hint is the key in the `exec_outputs` map.

With smart wiring, the hint is resolved via `FindPinSmart` on the source manifest. For a Branch node, `"True"` matches the `True` exec output pin. For a Sequence node, `"Then 0"` or `"0"` or `"Output 0"` would be tried through the fallback chain.

---

## 8. Smart Data Wiring

### 8.1 Problem

The current system requires `@stepId.pinName` where `pinName` is the exact internal UE pin name. The AI does not know these names and guesses wrong.

### 8.2 Solution: Enhanced @ref Resolution

In Phase 4 (Wire Data Connections), for each `@ref` input:

```
Algorithm: WireDataConnection(TargetStepId, TargetPinHint, SourceRef, Context)

Inputs:
  TargetStepId: step with the input pin
  TargetPinHint: the key in the inputs map (what the AI calls this input)
  SourceRef: the value starting with "@" (e.g., "@s1.ReturnValue", "@s1.~Location", "@s1.auto")
  Context: FOlivePlanExecutionContext

1. Parse the @ref:
   Parse "@{SourceStepId}.{SourcePinHint}" from SourceRef
   If parse fails, report error

2. Get manifests:
   SourceManifest = Context.GetManifest(SourceStepId)
   TargetManifest = Context.GetManifest(TargetStepId)

3. Resolve the TARGET input pin (what pin does the AI want to set?):
   TargetPin = TargetManifest->FindPinSmart(TargetPinHint, true /*input*/)
   If TargetPin is null:
     Report error with suggestions: "No input pin matching '{TargetPinHint}' on step '{TargetStepId}'. Available data inputs: {list}"
     Continue to next wire

4. Resolve the SOURCE output pin:
   If SourcePinHint == "auto":
     // TYPE-BASED AUTO-MATCH: find the output pin whose type matches the target's type
     SourcePin = FindTypeCompatibleOutput(SourceManifest, TargetPin->IRTypeCategory, TargetPin->PinSubCategory)
   Else if SourcePinHint starts with "~":
     // FUZZY MATCH: strip the ~ prefix and use fuzzy matching
     FuzzyHint = SourcePinHint.Mid(1)
     SourcePin = SourceManifest->FindPinSmart(FuzzyHint, false /*output*/, TargetPin->IRTypeCategory)
   Else:
     // STANDARD: try smart resolution
     SourcePin = SourceManifest->FindPinSmart(SourcePinHint, false /*output*/, TargetPin->IRTypeCategory)

   If SourcePin is null:
     Report error with suggestions
     Continue to next wire

5. Make the connection:
   SourcePinRef = Context.GetNodeId(SourceStepId) + "." + SourcePin->PinName
   TargetPinRef = Context.GetNodeId(TargetStepId) + "." + TargetPin->PinName
   Result = FOliveGraphWriter::Get().ConnectPins(Context.AssetPath, Context.GraphName, SourcePinRef, TargetPinRef)
   If Result.bSuccess:
     Context.SuccessfulConnectionCount++
   Else:
     Context.FailedConnectionCount++
     Record error but CONTINUE to next wire
```

### 8.3 FindTypeCompatibleOutput Algorithm

```
Algorithm: FindTypeCompatibleOutput(SourceManifest, TargetTypeCategory, TargetSubCategory)

1. Get all non-hidden, non-exec output pins from source
2. Filter to pins whose IRTypeCategory == TargetTypeCategory
3. If exactly 1 match: return it
4. If multiple matches AND TargetSubCategory is not empty:
   Sub-filter to pins whose PinSubCategory == TargetSubCategory
   If exactly 1 match: return it
5. If multiple matches remain:
   Prefer the pin named "ReturnValue" if present (most common output)
   Otherwise, return the first match (and log a warning about ambiguity)
6. If 0 matches: return nullptr
```

---

## 9. Enhanced @ref Syntax

### 9.1 Backward-Compatible Extensions

The existing `@stepId.pinName` syntax remains fully supported. New syntax variants are additive:

| Syntax | Meaning | Example | Behavior |
|--------|---------|---------|----------|
| `@s1.ReturnValue` | Exact pin name | Standard | Smart fallback chain (exact -> display -> fuzzy) |
| `@s1.~Location` | Fuzzy pin name | `~` prefix = fuzzy | Fuzzy match against all output pins |
| `@s1.auto` | Type-auto-match | Reserved keyword | Find output pin whose type matches the target input's type |
| `@s1.result` | Semantic alias | No prefix = try standard | Smart fallback resolves "result" to "ReturnValue" via fuzzy |

### 9.2 Parsing Changes

The existing `ParseDataReference` function in the lowerer splits on `.` to get `StepId` and `PinName`. This continues to work unchanged. The new semantics are handled **at resolution time** (in the executor), not at parse time.

The executor checks:
1. If `PinName == "auto"`: use type-based auto-matching
2. If `PinName` starts with `~`: strip prefix, use fuzzy matching with type hint
3. Otherwise: use standard `FindPinSmart` (which already includes fuzzy fallback)

### 9.3 Examples Applied to the Gun Blueprint

The AI's failed plan for the gun BP would look like this with the new syntax:

```json
{
  "schema_version": "2.0",
  "steps": [
    {
      "step_id": "evt",
      "op": "event",
      "target": "BeginPlay"
    },
    {
      "step_id": "get_fire_rate",
      "op": "get_var",
      "target": "FireRate"
    },
    {
      "step_id": "set_timer",
      "op": "call",
      "target": "SetTimerByFunctionName",
      "inputs": {
        "FunctionName": "Fire",
        "Time": "@get_fire_rate.auto"
      },
      "exec_after": "evt"
    },
    {
      "step_id": "fire_event",
      "op": "custom_event",
      "target": "Fire"
    },
    {
      "step_id": "check_can_fire",
      "op": "get_var",
      "target": "bCanFire"
    },
    {
      "step_id": "branch",
      "op": "branch",
      "inputs": {
        "Condition": "@check_can_fire.auto"
      },
      "exec_after": "fire_event",
      "exec_outputs": {
        "True": "spawn"
      }
    },
    {
      "step_id": "get_muzzle",
      "op": "call",
      "target": "GetWorldTransform",
      "target_class": "SceneComponent"
    },
    {
      "step_id": "spawn",
      "op": "spawn_actor",
      "target": "BP_Projectile",
      "inputs": {
        "SpawnTransform": "@get_muzzle.auto"
      },
      "exec_after": "branch"
    }
  ]
}
```

What happens at resolution:
- `SetTimerByFunctionName` -> alias map -> `K2_SetTimerByFunctionName` (found on UKismetSystemLibrary)
- `GetWorldTransform` on `SceneComponent` -> alias map -> `GetComponentTransform` (or direct find)
- `@get_fire_rate.auto` -> FireRate get_var output is a float, SetTimer's `Time` input is a float -> type match
- `@check_can_fire.auto` -> bCanFire get_var output is a bool, Branch's `Condition` is a bool -> type match
- `@get_muzzle.auto` -> GetWorldTransform's output is a Transform, SpawnActor's `SpawnTransform` is a Transform -> type match

---

## 10. Auto-Layout Algorithm

### 10.1 Problem

The current layout is `(StepIndex * 300, 0)` -- all nodes on a single horizontal line. This is unreadable for any non-trivial graph.

### 10.2 Solution: Exec-Flow-Aware Layout

```
Algorithm: LayoutGraph(Context, Plan)

Constants:
  HORIZONTAL_SPACING = 350   // pixels between columns
  VERTICAL_SPACING   = 200   // pixels between rows
  BRANCH_OFFSET      = 250   // extra vertical offset for branch targets
  PURE_NODE_OFFSET   = -120  // pure nodes placed above their consumer
  INITIAL_X          = 0
  INITIAL_Y          = 0

1. BUILD EXEC CHAINS
   Build a directed graph from exec_after/exec_outputs:
   - Each step is a node in the layout graph
   - exec_after creates edge: exec_after_step -> this_step
   - exec_outputs creates edges: this_step -> target_step (per output)

   Identify root nodes (steps with no exec_after and not referenced in any exec_outputs):
   These are entry points (events, custom events).

2. TOPOLOGICAL SORT WITH COLUMN ASSIGNMENT
   BFS from each root node:
   - Root gets Column = 0
   - Each successor gets Column = max(Column of all predecessors) + 1
   - Track which column each step is assigned to

3. ROW ASSIGNMENT
   For each column, assign rows:
   - Steps in the same column are ordered by their plan index (preserve declaration order)
   - Branch targets: "True" branch goes to Row 0, "False" branch goes to Row 1
   - Multiple outputs from Sequence: Row 0, 1, 2, etc.

4. PURE NODE PLACEMENT
   Pure nodes (no exec pins) are placed adjacent to their consumer:
   - If a pure node's output connects to step S at (X, Y):
     Place the pure node at (X - HORIZONTAL_SPACING/2, Y + PURE_NODE_OFFSET)
   - If a pure node feeds multiple consumers, place it left of the earliest consumer

5. COMPUTE POSITIONS
   For each step:
     PosX = INITIAL_X + Column * HORIZONTAL_SPACING
     PosY = INITIAL_Y + Row * VERTICAL_SPACING

   For branch/sequence offset:
     If a step is the "False" target of a branch:
       PosY += BRANCH_OFFSET

6. APPLY POSITIONS
   For each step in Context.StepToNodePtr:
     Node->NodePosX = PosX
     Node->NodePosY = PosY
```

### 10.3 Layout Data Structure

```cpp
/**
 * Layout metadata for a single node.
 * Computed by the layout engine, applied to UEdGraphNode positions.
 */
struct FOliveLayoutEntry
{
    FString StepId;
    int32 Column = 0;
    int32 Row = 0;
    int32 PosX = 0;
    int32 PosY = 0;
    bool bIsPure = false;
};

/**
 * FOliveGraphLayoutEngine
 *
 * Computes 2D positions for nodes based on their exec flow topology.
 * Produces a more readable layout than simple grid placement.
 */
class OLIVEAIEDITOR_API FOliveGraphLayoutEngine
{
public:
    /**
     * Compute layout positions for all steps in a plan.
     *
     * @param Plan The original plan (for exec_after/exec_outputs topology)
     * @param Context The execution context (for pure node detection via manifests)
     * @return Map of StepId -> layout entry with computed positions
     */
    static TMap<FString, FOliveLayoutEntry> ComputeLayout(
        const FOliveIRBlueprintPlan& Plan,
        const FOlivePlanExecutionContext& Context);

    /**
     * Apply computed layout to actual node positions.
     * Must be called on the game thread.
     *
     * @param Layout The computed layout entries
     * @param Context The execution context (for StepToNodePtr lookup)
     */
    static void ApplyLayout(
        const TMap<FString, FOliveLayoutEntry>& Layout,
        FOlivePlanExecutionContext& Context);

private:
    static constexpr int32 HORIZONTAL_SPACING = 350;
    static constexpr int32 VERTICAL_SPACING = 200;
    static constexpr int32 BRANCH_OFFSET = 250;
    static constexpr int32 PURE_NODE_OFFSET = -120;
};
```

---

## 11. Plan Path Enforcement

### 11.1 Problem

The AI has access to both granular tools (`blueprint.add_node`, `blueprint.connect_pins`, `project.batch_write`) and the plan path (`blueprint.preview_plan_json` / `blueprint.apply_plan_json`). When building a graph with 3+ nodes, the plan path is superior because it provides atomic execution, smart resolution, and pin introspection. But nothing prevents or warns the AI when it uses the inferior path.

### 11.2 Solution: Soft Warning in Granular Tool Handlers

Add a counter to the tool handler context that tracks how many `blueprint.add_node` and `blueprint.connect_pins` calls have been made in the current brain layer turn. When the count exceeds a threshold, append a warning to the tool result.

This does NOT block the operation -- it is informational only.

```cpp
// In HandleBlueprintAddNode, after successful execution:
if (FOliveToolExecutionContext::GetTurnNodeWriteCount() >= 3)
{
    ToolResult.AddWarning(
        TEXT("PLAN_PATH_PREFERRED: You have made ") +
        FString::FromInt(FOliveToolExecutionContext::GetTurnNodeWriteCount()) +
        TEXT(" granular node operations this turn. For 3+ node operations, use ")
        TEXT("blueprint.preview_plan_json + blueprint.apply_plan_json for better ")
        TEXT("reliability. The plan path provides smart pin resolution and atomic execution."));
}
```

### 11.3 Settings Integration

Add a setting to control this behavior:

```cpp
// In UOliveAISettings:
/** Threshold of granular node ops before emitting plan path warning (0 = disabled) */
UPROPERTY(EditAnywhere, Config, Category = "Blueprint|Plan JSON",
    meta = (ClampMin = "0", ClampMax = "20"))
int32 PlanPathWarningThreshold = 3;
```

---

## 12. Error Recovery & Partial Success

### 12.1 Problem

The current executor aborts on the first failure:

```cpp
// Current behavior (line ~6176 in OliveBlueprintToolHandlers.cpp):
if (!WriteResult.bSuccess)
{
    return FOliveWriteResult::ExecutionError(...);  // ABORT entire plan
}
```

This means if 1 of 20 connections fails, the nodes are created but zero connections are made.

### 12.2 Solution: Continue-on-Failure for Wiring Phases

Node creation (Phase 1) remains fail-fast -- if a node cannot be created, it is a hard error because downstream connections depend on it. But wiring phases (3, 4, 5) continue on failure and accumulate errors.

```
Phase 1: Create Nodes       - FAIL FAST (abort if any node fails)
Phase 2: Build Manifests     - ALWAYS SUCCEEDS (introspection never fails)
Phase 3: Wire Exec           - CONTINUE ON FAILURE (accumulate errors)
Phase 4: Wire Data           - CONTINUE ON FAILURE (accumulate errors)
Phase 5: Set Defaults        - CONTINUE ON FAILURE (accumulate errors)
Phase 6: Layout              - ALWAYS SUCCEEDS (best-effort)
```

### 12.3 Result Structure

The result includes a detailed breakdown:

```json
{
  "success": true,
  "partial": true,
  "created_nodes": 8,
  "connections_succeeded": 6,
  "connections_failed": 2,
  "defaults_succeeded": 3,
  "defaults_failed": 1,
  "step_to_node_map": { "evt": "node_0", "print": "node_1", ... },
  "wiring_errors": [
    {
      "error_code": "PIN_NOT_FOUND",
      "step_id": "spawn",
      "message": "No input pin matching 'ProjectileClass' on step 'spawn'",
      "suggestion": "Available data inputs: Class (Actor Class Reference), SpawnTransform (Transform), CollisionHandlingOverride (Enum)",
      "alternatives": ["Class", "SpawnTransform", "CollisionHandlingOverride"]
    }
  ],
  "pin_manifests": {
    "evt": {
      "node_type": "Event",
      "exec_outputs": ["Then"],
      "data_outputs": []
    },
    "print": {
      "node_type": "CallFunction",
      "exec_inputs": ["execute"],
      "exec_outputs": ["then"],
      "data_inputs": ["InString (String)", "bPrintToScreen (bool)", "bPrintToLog (bool)", "TextColor (LinearColor)", "Duration (float)"],
      "data_outputs": []
    }
  },
  "compile_result": { ... }
}
```

### 12.4 Success Determination

```
bSuccess = (CreatedNodeCount == Plan.Steps.Num())  // all nodes created
bPartial = (FailedConnectionCount > 0 || FailedDefaultCount > 0)

// Even with partial failures, the result is still "success" because:
// 1. All nodes exist in the graph
// 2. The AI can see what failed and issue targeted fix-up calls
// 3. The compile result will reveal structural issues
// 4. The pin_manifests tell the AI the exact pin names to use for repairs
```

---

## 13. File Structure & Module Boundaries

### 13.1 New Files

```
Source/OliveAIEditor/Blueprint/
  Public/Plan/
    OlivePlanExecutor.h           # Multi-phase plan executor
    OlivePinManifest.h            # Pin introspection data structures
    OliveFunctionResolver.h       # Smart function resolution
    OliveGraphLayoutEngine.h      # Exec-flow-aware auto-layout
  Private/Plan/
    OlivePlanExecutor.cpp
    OlivePinManifest.cpp
    OliveFunctionResolver.cpp
    OliveGraphLayoutEngine.cpp
```

### 13.2 Modified Files

```
Source/OliveAIEditor/Blueprint/
  Private/Plan/
    OliveBlueprintPlanResolver.cpp    # Use FOliveFunctionResolver in ResolveCallOp
  Private/Writer/
    OliveNodeFactory.cpp              # Enhanced FindFunction -> delegate to FOliveFunctionResolver
    OliveGraphWriter.cpp              # Add FindPinSmart public method
  Public/Writer/
    OliveGraphWriter.h                # Declare FindPinSmart
  Private/MCP/
    OliveBlueprintToolHandlers.cpp    # HandleBlueprintApplyPlanJson uses new executor
                                      # HandleBlueprintAddNode gets plan path warning
  Private/MCP/
    OliveBlueprintSchemas.cpp         # Update apply_plan_json schema for v2.0

Source/OliveAIRuntime/
  Public/IR/
    BlueprintPlanIR.h                 # schema_version "2.0" constant
                                      # No structural changes (backward compatible)

Content/SystemPrompts/
  Worker_Blueprint.txt                # Document new @ref syntax and auto matching

Config/
  DefaultOliveAI.ini                  # PlanPathWarningThreshold setting
```

### 13.3 Module Dependencies

```
OlivePlanExecutor depends on:
  - OlivePinManifest (build manifests)
  - OliveFunctionResolver (via PlanResolver)
  - OliveGraphWriter (create nodes, connect pins, set defaults)
  - OliveNodeFactory (create nodes)
  - OliveGraphLayoutEngine (auto-layout)
  - BlueprintPlanIR (plan data structures)
  - OliveBlueprintPlanResolver (step resolution)

OlivePinManifest depends on:
  - CommonIR.h (EOliveIRTypeCategory for type matching)
  - UEdGraphNode, UEdGraphPin, UEdGraphSchema_K2 (UE types)

OliveFunctionResolver depends on:
  - OliveNodeCatalog (fuzzy search)
  - UE reflection (FindFunctionByName, class iteration)

OliveGraphLayoutEngine depends on:
  - BlueprintPlanIR (plan topology)
  - OlivePinManifest (pure node detection)
  - UEdGraphNode (set positions)
```

### 13.4 What Is Public vs Private

| Class | Public API | Private Implementation |
|-------|-----------|----------------------|
| `FOlivePlanExecutor` | `Execute(Plan, ResolvedSteps, Blueprint, Graph, AssetPath, GraphName) -> FOliveIRBlueprintPlanResult` | Phase methods, context management |
| `FOlivePinManifest` | `Build()`, `FindPinSmart()`, `FindExecInput/Output()`, `ToJson()` | Levenshtein helper, type conversion |
| `FOliveFunctionResolver` | `Resolve()`, `GetCandidates()` | Alias map, K2 prefix strategy, broad search |
| `FOliveGraphLayoutEngine` | `ComputeLayout()`, `ApplyLayout()` | Topological sort, row assignment |

---

## 14. Integration Points

### 14.1 Integration with HandleBlueprintApplyPlanJson

The tool handler changes from:

```
Current:
  Resolve -> Lower -> Execute Batch (flat ops list)
```

To:

```
New:
  Resolve -> Execute via FOlivePlanExecutor (multi-phase with introspection)
```

The lowerer (`OliveBlueprintPlanLowerer`) is **not deleted** -- it remains for backward compatibility if someone has v1.0 plans that relied on exact pin names. The tool handler checks the `schema_version` field:

```cpp
if (Plan.SchemaVersion == TEXT("2.0"))
{
    // Use new multi-phase executor with pin introspection
    FOlivePlanExecutor Executor;
    PlanResult = Executor.Execute(Plan, ResolveResult.ResolvedSteps,
        Blueprint, TargetGraph, AssetPath, GraphTarget);
}
else
{
    // v1.0 backward compatibility: use existing lowerer + batch dispatch
    FOlivePlanLowerResult LowerResult = FOliveBlueprintPlanLowerer::Lower(
        ResolveResult.ResolvedSteps, Plan, GraphTarget, AssetPath);
    // ... existing batch execution code ...
}
```

**Important**: Even for v1.0 plans, the enhanced `FOliveFunctionResolver` is used via the resolver, so function resolution improves for all plans regardless of version.

### 14.2 Integration with the Write Pipeline

The `FOlivePlanExecutor` runs **inside** the write pipeline's executor lambda (same as current behavior). This means:
- The entire plan execution is wrapped in a single `FScopedTransaction`
- If anything goes fatally wrong, the transaction is rolled back
- Compilation happens once at the end (Stage 5 of the pipeline)

### 14.3 Integration with GraphWriter Node Cache

The executor uses `FOliveGraphWriter::Get().AddNode()` for node creation, which automatically caches created nodes. The manifests store the same `NodeId` strings, so wiring phases can use `ConnectPins` which looks up nodes from cache.

### 14.4 Integration with the Prompt System

The worker prompt (`Content/SystemPrompts/Worker_Blueprint.txt`) is updated to:
1. Document the new `@ref` syntax variants (`@s1.auto`, `@s1.~fuzzy`)
2. Recommend `schema_version: "2.0"` for all new plans
3. Explain that exact pin names are not needed -- the system resolves them
4. Show the gun blueprint example using the new syntax

### 14.5 Integration with the Brain Layer

The brain layer's self-correction policy benefits from the enhanced error reporting. When a plan execution returns partial success with `wiring_errors`, the self-correction policy can:
1. Read the `pin_manifests` to see the actual pin names
2. Generate targeted `blueprint.connect_pins` calls using exact pin names from the manifest
3. No longer need to guess -- the manifest provides ground truth

---

## 15. Implementation Phases

### Phase A: Foundation (FOlivePinManifest + FOliveFunctionResolver)

**Estimated effort: 2-3 days**

Files to create:
- `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePinManifest.h`
- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePinManifest.cpp`
- `Source/OliveAIEditor/Blueprint/Public/Plan/OliveFunctionResolver.h`
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveFunctionResolver.cpp`

Implementation order:
1. `FOlivePinManifestEntry` struct (plain data)
2. `FOlivePinManifest` struct with `Build()` static method
3. `FindExecInput()`, `FindExecOutput()`, `FindAllExecOutputs()` query methods
4. `FindPinSmart()` with the full fallback chain (exact -> display -> case -> fuzzy -> type)
5. `ToJson()` for serialization
6. `FOliveFunctionResolver::GetAliasMap()` static data
7. `FOliveFunctionResolver::TryExactMatch()` (mirrors current FindFunction)
8. `FOliveFunctionResolver::TryK2PrefixMatch()`
9. `FOliveFunctionResolver::TryAliasMatch()`
10. `FOliveFunctionResolver::TryCatalogMatch()` (delegates to `FOliveNodeCatalog::Search`)
11. `FOliveFunctionResolver::BroadSearch()` (iterates function libraries)
12. `FOliveFunctionResolver::Resolve()` (orchestrates all strategies)
13. `FOliveFunctionResolver::GetCandidates()` (for suggestions)

**Build and verify**: Run a build after this phase to ensure compilation succeeds. No runtime behavior changes yet.

### Phase B: Plan Executor (FOlivePlanExecutor)

**Estimated effort: 3-4 days**

Files to create:
- `Source/OliveAIEditor/Blueprint/Public/Plan/OlivePlanExecutor.h`
- `Source/OliveAIEditor/Blueprint/Private/Plan/OlivePlanExecutor.cpp`

Implementation order:
1. `FOlivePlanExecutionContext` struct
2. `FOlivePlanExecutor::Execute()` entry point
3. Phase 1: `ExecuteNodeCreation()` -- iterate resolved steps, call `GraphWriter.AddNode()`, build manifests
4. Phase 2: `BuildAllManifests()` -- call `FOlivePinManifest::Build()` for each created node (technically this happens inside Phase 1, right after each AddNode succeeds)
5. Phase 3: `WireExecConnections()` -- iterate plan steps' `exec_after` and `exec_outputs`, use manifest-based resolution
6. Phase 4: `WireDataConnections()` -- iterate plan steps' `inputs` for `@ref` values, use manifest-based resolution with enhanced `@ref` syntax
7. Phase 5: `SetPinDefaults()` -- iterate plan steps' `inputs` for literal values, use manifest-based pin lookup
8. Build result with stats, step_to_node_map, wiring_errors, pin_manifests summary

**Build and verify**: Compile. No integration yet (executor is not called).

### Phase C: Integration (Wire into HandleBlueprintApplyPlanJson)

**Estimated effort: 1-2 days**

Files to modify:
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`

Implementation order:
1. Modify `ResolveCallOp` to use `FOliveFunctionResolver::Resolve()` instead of direct catalog search
2. Modify `HandleBlueprintApplyPlanJson` to detect `schema_version "2.0"` and route to `FOlivePlanExecutor`
3. Keep the v1.0 code path intact for backward compatibility
4. Update result serialization to include `pin_manifests` and `wiring_errors`

**Build and verify**: Build. Test with v1.0 plans (should still work). Test with v2.0 plans.

### Phase D: Auto-Layout (FOliveGraphLayoutEngine)

**Estimated effort: 1-2 days**

Files to create:
- `Source/OliveAIEditor/Blueprint/Public/Plan/OliveGraphLayoutEngine.h`
- `Source/OliveAIEditor/Blueprint/Private/Plan/OliveGraphLayoutEngine.cpp`

Implementation order:
1. Build exec chain adjacency list from plan
2. Identify root nodes (entry points)
3. BFS column assignment
4. Row assignment with branch offset
5. Pure node placement
6. Apply positions to `UEdGraphNode::NodePosX/Y`
7. Integrate into `FOlivePlanExecutor` as Phase 6

**Build and verify**: Build. Test layout with Branch and Sequence nodes.

### Phase E: Polish (Warnings, Prompt Updates, Settings)

**Estimated effort: 1 day**

Files to modify:
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` (plan path warning in add_node)
- `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` (PlanPathWarningThreshold)
- `Config/DefaultOliveAI.ini` (default value)
- `Content/SystemPrompts/Worker_Blueprint.txt` (document new syntax)
- `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` (update schema description)

---

## 16. Testing Strategy

### 16.1 Unit Tests for FOlivePinManifest

```
Test: PinManifest_BuildFromPrintString
  Create a PrintString call function node in a test Blueprint
  Call FOlivePinManifest::Build()
  Assert: manifest has exec input "execute", exec output "then"
  Assert: manifest has data inputs "InString", "bPrintToScreen", "bPrintToLog", "TextColor", "Duration"
  Assert: manifest has NO hidden WorldContextObject pin in the manifest
  Assert: FindExecInput() returns the "execute" pin
  Assert: FindExecOutput() returns the "then" pin

Test: PinManifest_BuildFromBranch
  Create a Branch node
  Call FOlivePinManifest::Build()
  Assert: FindExecInput() returns "execute"
  Assert: FindAllExecOutputs() returns 2 pins: "Then" (for true) and "Else" (for false)
     -- NOTE: actual UE names must be verified at runtime; this test verifies the introspection works
  Assert: FindPinSmart("Condition", true) returns the boolean input pin
  Assert: FindPinSmart("True", false) returns the true exec output
  Assert: FindPinSmart("False", false) returns the false exec output

Test: PinManifest_FindPinSmartFuzzy
  Create a SpawnActorFromClass node
  Call FOlivePinManifest::Build()
  Assert: FindPinSmart("Class", true) matches the class input pin (exact)
  Assert: FindPinSmart("class", true) matches via case-insensitive
  Assert: FindPinSmart("ProjectileClass", true) matches "Class" via fuzzy (substring "Class")
  Assert: FindPinSmart("SpawnTransform", true) matches the transform input

Test: PinManifest_FindPinSmartTypeMatch
  Create a SetActorLocation node
  Call FOlivePinManifest::Build()
  Assert: FindPinSmart("Location", true, EOliveIRTypeCategory::Vector) matches via type match
  Assert: FindPinSmart("xyz", true, EOliveIRTypeCategory::Vector) matches via type match
    (only one Vector input exists)

Test: PinManifest_PureNode
  Create an Add_FloatFloat node (pure, no exec pins)
  Assert: bIsPure == true
  Assert: FindExecInput() returns nullptr
  Assert: FindExecOutput() returns nullptr
```

### 16.2 Unit Tests for FOliveFunctionResolver

```
Test: FunctionResolver_ExactMatch
  Resolve("PrintString", "", nullptr)
  Assert: Function != nullptr
  Assert: MatchMethod == ExactName or via library search
  Assert: Confidence >= 90

Test: FunctionResolver_K2Prefix
  Resolve("GetActorLocation", "", TestBlueprint)
  Assert: Function != nullptr (resolved to K2_GetActorLocation)
  Assert: MatchMethod == K2Prefix

Test: FunctionResolver_AliasMap
  Resolve("GetComponentTransform", "SceneComponent", TestBlueprint)
  Assert: Function != nullptr
  Assert: MatchMethod == Alias

Test: FunctionResolver_NotFound
  Resolve("CompletelyFakeFunction", "", TestBlueprint)
  Assert: Function == nullptr
  GetCandidates("CompletelyFakeFunction") returns empty or low-score results

Test: FunctionResolver_Ambiguous
  Resolve("SetVisibility", "", TestBlueprint)
  -- SetVisibility exists on multiple classes
  Assert: either resolves to most-relevant class or returns error with alternatives
```

### 16.3 Integration Tests

```
Test: PlanExecutor_SimpleBeginPlayPrint
  Plan: event(BeginPlay) -> call(PrintString, inputs: {"InString": "Hello"})
  Execute with v2.0 schema
  Assert: 2 nodes created
  Assert: exec connection made (BeginPlay.then -> PrintString.execute)
  Assert: InString default set to "Hello"
  Assert: compiles successfully

Test: PlanExecutor_BranchWiring
  Plan: event(BeginPlay) -> branch(Condition: "@get_bool.auto")
                            -> print_true(exec_after: branch via "True")
                            -> print_false(exec_after: branch via "False")
  Execute with v2.0 schema
  Assert: 4 nodes created (or 5 with get_bool)
  Assert: Branch True -> print_true exec wired
  Assert: Branch False -> print_false exec wired
  Assert: data wire from get_bool output to Branch Condition input

Test: PlanExecutor_PartialSuccess
  Plan: event(BeginPlay) -> call(PrintString) -> call(FakeFunctionThatDoesNotExist)
  Resolve: third step fails resolution
  Assert: Resolution error for step 3
  Assert: Steps 1-2 would succeed if resolver allowed partial (but resolver is all-or-nothing)

Test: PlanExecutor_WiringPartialSuccess
  Plan: 3 nodes, 2 correct connections, 1 impossible connection
  Assert: 3 nodes created
  Assert: 2 connections made
  Assert: 1 wiring_error with suggestions
  Assert: bPartial == true

Test: PlanExecutor_AutoRefSyntax
  Plan with "@s1.auto" data reference
  Assert: auto-matches by type compatibility
  Assert: correct connection made

Test: PlanExecutor_FuzzyRefSyntax
  Plan with "@s1.~Location" data reference
  Assert: fuzzy-matches to the actual location output pin
  Assert: correct connection made
```

### 16.4 Layout Tests

```
Test: Layout_LinearChain
  3 steps in linear exec chain: A -> B -> C
  Assert: A.Column=0, B.Column=1, C.Column=2
  Assert: all on Row 0

Test: Layout_BranchSplit
  A -> Branch -> (True: B, False: C)
  Assert: A.Column=0, Branch.Column=1, B.Column=2 Row=0, C.Column=2 Row=1

Test: Layout_PureNodePlacement
  A (event) -> B (has input from Pure node P)
  Assert: P placed to the left and above B
```

---

## 17. Migration Path

### 17.1 Backward Compatibility Guarantee

- `schema_version: "1.0"` plans continue to work exactly as before (lowerer path)
- `schema_version: "2.0"` plans use the new executor
- If `schema_version` is omitted, default to `"1.0"` for backward compat
- The existing `@stepId.pinName` syntax works in both versions
- The new `@stepId.auto` and `@stepId.~fuzzy` syntax only works in v2.0 (ignored/error in v1.0)

### 17.2 Prompt Migration

The worker prompt is updated to:
1. Default to `schema_version: "2.0"` for all new plans
2. Document the new `@ref` variants
3. Encourage `@stepId.auto` for data wires where pin names are uncertain
4. Remove the warning about exact pin names being required

### 17.3 Settings Migration

New settings are added with sensible defaults:
- `PlanPathWarningThreshold = 3` (start warning after 3 granular ops)
- No existing settings change

### 17.4 Future Deprecation

Once v2.0 has proven stable (after 2-3 months of usage), the v1.0 lowerer path can be deprecated with a warning in the tool result. It should not be removed -- external tools may depend on it.

---

## 18. Edge Cases & Error Handling

### 18.1 Pure Nodes in Exec Flow

**Problem**: A pure node (e.g., `MakeVector`) has no exec pins. If the plan says `exec_after: "make_vec"`, the exec wiring will fail because there is no exec input.

**Handling**: In Phase 3, if the target of an exec wire is a pure node:
- Skip the exec wire silently (pure nodes do not participate in exec flow)
- Add a warning: "Step 'make_vec' is a pure node; exec_after wire skipped"
- The data wire from the pure node's output to its consumer is handled in Phase 4 as normal

### 18.2 Multiple Exec Outputs Without Explicit Mapping

**Problem**: A Sequence node has `Then 0`, `Then 1`, etc. If `exec_after: "seq"` without specifying which output, which one to use?

**Handling**: Use `FindExecOutput()` which returns the **first** (primary) exec output. For Sequence, this would be `Then 0`. If the AI wants a specific output, it must use `exec_outputs` instead of `exec_after`.

### 18.3 Node Creation Failure Mid-Plan

**Problem**: Step 5 of 10 fails to create (e.g., function not found). Steps 6-10 may depend on step 5.

**Handling**: Node creation is fail-fast. The entire plan stops if any node fails to create. Rationale:
- Wiring depends on all nodes existing
- Creating partial graphs is worse than creating nothing (orphaned nodes confuse the AI)
- The error message includes the exact step that failed and suggestions
- The AI can fix the plan and retry

Exception: if the failed step is a pure data node with no downstream exec dependencies, it could theoretically be skipped. But this complexity is not worth the implementation cost in v1. Keep it simple: fail fast on node creation.

### 18.4 Ambiguous Type Matching

**Problem**: `@s1.auto` is used, but the source node has multiple output pins of the same type.

**Handling**: In `FindTypeCompatibleOutput`:
1. If exactly 1 match by type: use it
2. If multiple matches by type AND one is named "ReturnValue": prefer "ReturnValue"
3. If multiple matches and none is "ReturnValue": use the first, add warning about ambiguity
4. If 0 matches: fail with error listing available output types

### 18.5 Circular Exec References

**Problem**: `exec_after` or `exec_outputs` create a cycle (step A -> B -> A).

**Handling**: The exec wiring phase does NOT check for cycles -- it simply wires what is requested. UE allows circular exec flow (e.g., while loops), so this is valid. The topological sort in the layout engine handles cycles by breaking them at the back-edge.

### 18.6 Self-Referencing Data Wire

**Problem**: `@self.pinName` where a step references its own output as input.

**Handling**: This is a valid UE pattern (e.g., a node's output fed back to its own input via a reroute). The wiring phase allows it. However, `FindPinSmart` will search output pins for the source and input pins for the target, so a self-reference works as long as the pin names are different (which they always are -- a pin cannot be both input and output).

### 18.7 Event Node Already Exists

**Problem**: Plan includes `"op": "event", "target": "BeginPlay"` but BeginPlay already exists in the graph.

**Handling**: The existing `CreateEventNode` in `OliveNodeFactory.cpp` already checks for duplicate native events (line 328-338) and returns nullptr with an error. The executor treats this as a node creation failure.

**Enhanced handling**: Before failing, check if the existing event node is already in the graph and can be reused. Add the existing node to the manifest and skip creation:

```
In Phase 1, before calling AddNode for an Event op:
  If event already exists in graph:
    existing_node = FindExistingEvent(Graph, EventName)
    if existing_node:
      Add to manifest using existing node's pins
      Add to StepToNodeMap using existing node's cached ID (or generate one)
      Add warning: "Event 'BeginPlay' already exists, reusing existing node"
      Skip creation, continue to next step
```

This is critical because almost every plan starts with an event that already exists.

### 18.8 Large Plans (50+ Steps)

**Problem**: Performance concern for plans with many steps.

**Handling**:
- Pin introspection is O(N) where N = number of pins per node (typically < 20). For 50 nodes, this is ~1000 pin reads. Negligible.
- `FindPinSmart` is O(P) per call where P = pins on that node. For fuzzy matching, it includes Levenshtein which is O(L^2) per pin where L = name length (typically < 30 chars). Still negligible.
- The existing `PlanJsonMaxSteps` setting (default 128) caps plan size.
- Layout engine BFS is O(N + E) where E = edges. For 128 nodes with an average of 2 edges each, this is < 400 operations. Negligible.

No performance optimization needed for v1 of this feature.

### 18.9 Non-Blueprint Nodes

**Problem**: Some nodes in UE graphs are not `UK2Node` subclasses (e.g., `UEdGraphNode_Comment`). They may have unusual pin structures.

**Handling**: `FOlivePinManifest::Build()` works on `UEdGraphNode*` (base class), not `UK2Node*`. Comment nodes have no pins, so the manifest is empty. This is correct -- comment nodes do not participate in wiring.

### 18.10 Pin Name Collision

**Problem**: A node might have multiple pins with the same display name but different internal names (rare but possible in generated code).

**Handling**: `FindPinSmart` returns the **first** match at each priority level. If the AI needs to target a specific pin among duplicates, it must use the exact internal name (which it can get from the manifest in a previous turn's result or from `blueprint.read`).

---

## Appendix A: Class Summary for Coder

| Class | File | Singleton? | Responsibility |
|-------|------|-----------|---------------|
| `FOlivePinManifestEntry` | `OlivePinManifest.h` | No | Single pin's introspected data |
| `FOlivePinManifest` | `OlivePinManifest.h` | No | All pins for one node + query methods |
| `FOlivePlanExecutionContext` | `OlivePlanExecutor.h` | No | Shared state across execution phases |
| `FOliveSmartWireResult` | `OlivePlanExecutor.h` | No | Result of one smart wiring attempt |
| `FOlivePlanExecutor` | `OlivePlanExecutor.h` | No | Multi-phase plan execution |
| `FOliveFunctionMatch` | `OliveFunctionResolver.h` | No | Function resolution result |
| `FOliveFunctionResolver` | `OliveFunctionResolver.h` | No (static) | Smart function name resolution |
| `FOliveLayoutEntry` | `OliveGraphLayoutEngine.h` | No | Layout position for one node |
| `FOliveGraphLayoutEngine` | `OliveGraphLayoutEngine.h` | No (static) | Exec-flow-aware layout computation |

## Appendix B: Error Codes Added

| Code | Phase | Severity | Meaning |
|------|-------|----------|---------|
| `FUNCTION_NOT_FOUND` | Resolve | Error | Function name could not be resolved by any strategy |
| `FUNCTION_AMBIGUOUS` | Resolve | Error | Multiple functions match, need target_class |
| `NODE_CREATION_FAILED` | Execute/Phase1 | Fatal | Node factory returned nullptr |
| `EXEC_PIN_NOT_FOUND` | Execute/Phase3 | Error (non-fatal) | No exec pin matching hint on node |
| `DATA_PIN_NOT_FOUND` | Execute/Phase4 | Error (non-fatal) | No data pin matching hint on node |
| `DATA_PIN_AMBIGUOUS` | Execute/Phase4 | Warning | Multiple type-compatible pins, first used |
| `DEFAULT_PIN_NOT_FOUND` | Execute/Phase5 | Error (non-fatal) | Cannot find input pin for default value |
| `EVENT_ALREADY_EXISTS` | Execute/Phase1 | Warning | Reusing existing event node |
| `PURE_NODE_EXEC_SKIP` | Execute/Phase3 | Warning | Skipped exec wire to pure node |

## Appendix C: Levenshtein Distance Helper

The `FindPinSmart` method uses Levenshtein distance for fuzzy matching. Implementation note for the coder: UE does not provide a built-in Levenshtein function. Implement a simple O(n*m) dynamic programming version in `OlivePinManifest.cpp` as a file-local helper:

```cpp
namespace
{
    int32 LevenshteinDistance(const FString& A, const FString& B)
    {
        const int32 M = A.Len();
        const int32 N = B.Len();
        TArray<int32> Prev, Curr;
        Prev.SetNumZeroed(N + 1);
        Curr.SetNumZeroed(N + 1);
        for (int32 j = 0; j <= N; ++j) Prev[j] = j;
        for (int32 i = 1; i <= M; ++i)
        {
            Curr[0] = i;
            for (int32 j = 1; j <= N; ++j)
            {
                int32 Cost = (A[i-1] == B[j-1]) ? 0 : 1;
                Curr[j] = FMath::Min3(Prev[j] + 1, Curr[j-1] + 1, Prev[j-1] + Cost);
            }
            Swap(Prev, Curr);
        }
        return Prev[N];
    }
}
```

## Appendix D: Data Flow Diagram

```
                    AI Agent (LLM)
                         |
                    Plan JSON v2.0
                    (intent-level)
                         |
                         v
              +---------------------+
              | Schema Validation   |  BlueprintPlanIR.h
              +---------------------+
                         |
                         v
              +---------------------+
              | Plan Resolution     |  OliveBlueprintPlanResolver
              |  + Smart Function   |  + FOliveFunctionResolver
              |    Resolution       |    (alias, K2_, fuzzy, catalog)
              +---------------------+
                         |
                    ResolvedSteps[]
                         |
                         v
    +============================================+
    |         FOlivePlanExecutor.Execute()        |
    |                                            |
    |  Phase 1: Create Nodes ----------------->  |
    |    For each resolved step:                 |
    |      NodeFactory.CreateNode()              |
    |      FOlivePinManifest::Build(node)        |
    |      Cache manifest in context             |
    |                                            |
    |  Phase 2: (manifests built in Phase 1)     |
    |                                            |
    |  Phase 3: Wire Exec Connections -------->  |
    |    For each exec_after:                    |
    |      Source.FindExecOutput() (manifest)    |
    |      Target.FindExecInput() (manifest)     |
    |      GraphWriter.ConnectPins(exact names)  |
    |    For each exec_outputs entry:            |
    |      Source.FindPinSmart(hint) (manifest)  |
    |      Target.FindExecInput() (manifest)     |
    |      GraphWriter.ConnectPins(exact names)  |
    |                                            |
    |  Phase 4: Wire Data Connections -------->  |
    |    For each @ref input:                    |
    |      Parse @sourceStep.pinHint             |
    |      Source.FindPinSmart(hint)  (manifest) |
    |      Target.FindPinSmart(key)  (manifest)  |
    |      OR: @source.auto -> type match        |
    |      OR: @source.~fuzzy -> fuzzy match     |
    |      GraphWriter.ConnectPins(exact names)  |
    |                                            |
    |  Phase 5: Set Pin Defaults -------------> |
    |    For each literal input:                 |
    |      Target.FindPinSmart(key)  (manifest)  |
    |      GraphWriter.SetPinDefault(exact name) |
    |                                            |
    |  Phase 6: Auto-Layout ----------------->  |
    |    GraphLayoutEngine.ComputeLayout()       |
    |    GraphLayoutEngine.ApplyLayout()         |
    |                                            |
    +============================================+
                         |
                         v
              +---------------------+
              | Write Pipeline      |  OliveWritePipeline
              | Stage 5: Verify     |  (compile, structural checks)
              +---------------------+
                         |
                         v
              +---------------------+
              | Result with         |  FOliveIRBlueprintPlanResult
              | - step_to_node_map  |  + pin_manifests
              | - pin_manifests     |  + wiring_errors
              | - wiring_errors     |  + compile_result
              | - compile_result    |
              +---------------------+
                         |
                         v
                    AI Agent (LLM)
                    (can self-correct
                     using manifest data)
```
