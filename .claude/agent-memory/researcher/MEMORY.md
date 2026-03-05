# Researcher Agent Memory

## Engine Location
UE 5.5 installed at: `C:/Program Files/Epic Games/UE_5.5/`
PCG plugin headers: `C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/PCG/Source/PCG/Public/`

## Key Research Reports
- `plans/research/pcg-api-ue55.md` — Full PCG plugin public API (UPCGGraph, UPCGNode, UPCGPin, UPCGSettings, EPCGDataType, UPCGSubgraphSettings, UPCGComponent, UPCGSubsystem)
- `plans/research/uanimstatetransitionnode-api-ue55.md` — (exists, topic unknown)
- `plans/research/log-failure-analysis.md` — Session log failure analysis; 5 Olive false negatives documented; interface call gap is primary issue
- `plans/research/interface-event-resolution.md` — Interface event resolution gap: ResolveEventOp and CreateEventNode never check ImplementedInterfaces

## PCG API Key Facts (UE 5.5)
- Module name for PCG plugin is `"PCG"` (add to Build.cs PrivateDependencyModuleNames)
- UPCGGraph::GetNodes() does NOT include InputNode/OutputNode — use GetInputNode()/GetOutputNode()
- Use SetSubgraph(UPCGGraphInterface*) not direct property assignment on UPCGSubgraphSettings
- EPCGDataType is uint32 bitmask — not uint8, never cast to uint8
- Default pin label constants in PCGPinConstants namespace: "In", "Out", "Overrides", "Dependency Only"
- Several UPCGSubsystem methods deprecated in 5.5 — use FPCGGridDescriptor overloads
- All UObject mutations (graph edits) must be on the game thread + FScopedTransaction

## C++ Reflection API Key Facts (UE 5.5)
- Research report: `plans/research/cpp-reflection-api-ue55.md`
- Core headers: `"UObject/Class.h"`, `"UObject/UnrealType.h"`, `"UObject/ObjectMacros.h"`, `"UObject/UObjectIterator.h"`
- Enumerate classes: `TObjectIterator<UClass>` (game thread only)
- Enumerate struct members/function params: `TFieldIterator<FProperty>(Struct)`
- Enumerate functions: `TFieldIterator<UFunction>(Class)`
- Property type casting: `CastField<FObjectProperty>(Prop)` NOT `Cast<>` (FProperty is FField not UObject)
- Blueprintable/BlueprintType are metadata keys, not class flags: `Class->GetBoolMetaData(TEXT("Blueprintable"))`
- BlueprintImplementableEvent = FUNC_BlueprintEvent AND NOT FUNC_Native; NativeEvent = both set
- No STRUCT_BlueprintType flag — check metadata same as class
- Source file discovery: `FSourceCodeNavigation::FindClassHeaderPath(UField*, FString&)` in module `UnrealEd`
- New class generation: `GameProjectUtils::AddCodeToProject(...)` in module `GameProjectGeneration`
- Hot reload: do NOT trigger automatically — AddCodeToProject handles it; prompt user otherwise
- All reflection iteration must be on the game thread

## NeoStack / AIK Architecture (plans/research/neostack-architecture.md)
- Product: Agent Integration Kit by Betide Studio — docs at aik.betide.studio
- MCP server: always-on HTTP server, port 9315 default, auto-starts with editor
- DUAL transport on same port: SSE (`/sse`, MCP 2024-11-05) for Claude Code; Streamable HTTP (`/mcp`, MCP 2025-03-26) for Gemini CLI/Cursor
- Claude Code STILL uses SSE transport (not Streamable HTTP) as of Feb 2026 — `.mcp.json` URL should be `/sse`
- Process lifecycle: ONE process per user request (not per turn); Claude Code exits when task complete
- Domain knowledge: Profiles inject system prompt additions; no AGENTS.md — Claude Code reads CLAUDE.md natively
- Tool consolidation: v0.5.0 reduced from 27+ to 15 tools — fewer broader tools is the AIK philosophy
- .mcp.json placed at project root, written dynamically after server binds with actual port
- ACP (stdio subprocess) = in-editor chat; MCP HTTP = external agents (Cursor); both used simultaneously
- No public source code — AIK is proprietary (GitHub has only EOSIntegrationKit, SteamIntegrationKit, etc.)

## Delegate Node API Key Facts (UE 5.5)
- Research report: `plans/research/delegate-nodes-ue55.md`
- All delegate node types inherit from `UK2Node_BaseMCDelegate` (abstract)
- Headers in `Engine/Source/Editor/BlueprintGraph/Classes/K2Node_*.h`; all implementations in `K2Node_MCDelegate.cpp`
- Initialization sequence (strict order): `NewObject<TNode>(Graph)` → `SetFromProperty(prop, bSelfContext, ownerClass)` → `AllocateDefaultPins()` → `Graph->AddNode(...)`
- `SetFromProperty` MUST be before `AllocateDefaultPins` — pins depend on signature resolved from DelegateReference
- `bSelfContext=true` for Blueprint-owned dispatchers; `false` for C++ parent class dispatchers
- Find `FMulticastDelegateProperty` on `Blueprint->SkeletonGeneratedClass` (preferred) or `GeneratedClass` (fallback) via `FindFProperty<FMulticastDelegateProperty>(class, FName(name))`
- Dispatchers stored as `UEdGraph` in `Blueprint->DelegateSignatureGraphs`; exposed as `FMulticastDelegateProperty` on skeleton class after compile
- `call_delegate` op is FULLY IMPLEMENTED (OlivePlanOps, OliveNodeFactory, resolver)
- `bind_dispatcher` (UK2Node_AddDelegate) is NOT yet implemented — need new op, node type, factory method, and resolver handler
- UK2Node_AddDelegate pins: exec-in, then, Target (object), Delegate/Event (PC_Delegate input, const ref)
- UK2Node_CallDelegate pins: exec-in, then, Target (object), plus one input pin per dispatcher parameter
- `GetDelegatePin()` on base class returns the "Delegate" input pin (only on AddDelegate/RemoveDelegate)

## Blueprint Interface (BPI) API Key Facts (UE 5.5)
- Research report: `plans/research/blueprint-interface-apis-ue55.md`
- Create BPI: `FKismetEditorUtilities::CreateBlueprint(UInterface::StaticClass(), ..., BPTYPE_Interface, ...)` directly; don't use `UBlueprintInterfaceFactory` programmatically (opens dialog)
- Add function to BPI: `AddFunctionGraph<UClass>(BPI, Graph, true, nullptr)` then `GetEntryAndResultNodes` + `CreateUserDefinedPin`
- Implement interface on BP: `FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath(path))`
- `FBPInterfaceDescription` struct: `Interface` (UClass*) + `Graphs` (array of UEdGraph*) — lives in `Blueprint->ImplementedInterfaces`
- PC_Class pins (DoesImplementInterface Interface param): use `TrySetDefaultObject` with `LoadObject<UClass>()`, or `TrySetDefaultValue` with full `_C` path (e.g., `/Game/BPI_X.BPI_X_C`)
- UK2Node_Message: already implemented in OliveNodeFactory; `FunctionReference.SetFromField<UFunction>(Func, false)` is correct
- K2Node_EnhancedInputAction: `InputAction` IS a UPROPERTY; exec pin names = ETriggerEvent values: Triggered/Started/Ongoing/Canceled/Completed; only Triggered visible by default
- UK2Node_Variable ghost node fix needed in CreateNodeByClass: detect `IsA<UK2Node_Variable>()`, extract `variable_name` from properties, call `VarNode->VariableReference.SetSelfMember(FName(*VarName))` BEFORE AllocateDefaultPins
- Interface implementation graphs live in `Blueprint->ImplementedInterfaces[i].Graphs` ONLY — NOT in FunctionGraphs. AddInterfaceGraph() only calls CreateFunctionGraphTerminators, never touches FunctionGraphs.
- Interface graph markers: `Graph->bAllowDeletion = false` AND `Graph->InterfaceGuid` is non-zero (FGuid)
- Interface entry node difference: FunctionReference uses SetExternalMember(name, InterfaceClass, guid) — NOT SetSelfMember. `!EntryNode->FunctionReference.IsSelfContext()` identifies interface graphs.
- GetAllGraphs() includes ImplementedInterfaces[i].Graphs — safe to use. Direct FunctionGraphs iteration MISSES interface graphs.
- Kismet compiler processes interface graphs in a separate loop (line 4739 in KismetCompiler.cpp) but calls same ProcessOneFunctionGraph.

## Timeline Node API Key Facts (UE 5.5)
- Research reports: `plans/research/timeline-node-api.md` (overview), `plans/research/timeline-node-api-deep.md` (exhaustive)
- `UK2Node_Timeline` does NOT store timeline data — only `TimelineName` (FName). Data is in `UTimelineTemplate` on `Blueprint->Timelines`.
- `bAutoPlay`, `bLoop`, `bReplicated`, `bIgnoreTimeDilation` on UK2Node_Timeline are ALL `Transient` caches — source of truth is on UTimelineTemplate.
- `TimelineLength` is a field on `UTimelineTemplate`, NOT on `UK2Node_Timeline` — `set_node_property` cannot set it.
- UTimelineTemplate constructor defaults: `TimelineLength = 5.0f`, `bReplicated = false`, `LengthMode` = TL_TimelineLength (zero-init)
- PostInitProperties auto-sets `TimelineGuid = FGuid::NewGuid()` and calls `UpdateCachedNames()` — do NOT set these manually
- Creation order MATTERS: set TimelineName → AddNewTimeline → populate template (tracks, flags, length) → AddDisplayTrack → AllocateDefaultPins
- Track name set via `Track.SetTrackName(FName, UTimelineTemplate*)` — `TrackName` is private on FTTTrackBase
- FTTPropertyTrack::SetTrackName auto-generates PropertyName: `"{TimelineName}_{TrackName}_{Guid}"` (sanitized to valid C++ identifier)
- FTTEventTrack::SetTrackName auto-generates FunctionName: `"{TimelineName}__{TrackName}__EventFunc"`
- Track types: FTTFloatTrack (UCurveFloat), FTTVectorTrack (UCurveVector), FTTLinearColorTrack (UCurveLinearColor), FTTEventTrack (UCurveFloat with bIsEventCurve=true)
- Curve outer MUST be `Blueprint->GeneratedClass` (not Blueprint, not template): `NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public)`
- Add keys: `CurveFloat->FloatCurve.AddKey(time, value)` — FRichCurve::AddKey(), default interp RCIM_Linear
- After adding tracks to existing node: call `TimelineNode->ReconstructNode()` to rebuild pins
- bAutoPlay/bLoop must sync to BOTH template AND node when modifying existing node
- Only Actor-based BPTYPE_Normal/LevelScript blueprints support timelines — `FBlueprintEditorUtils::DoesSupportTimelines(Blueprint)` checks `IsActorBased && DoesSupportEventGraphs`
- Timelines ONLY valid in ubergraphs (event graphs) and composite graphs nested inside ubergraphs — NOT function graphs
- AddNewTimeline returns nullptr silently on: incompatible BP type, duplicate name
- AddNewTimeline calls `MarkBlueprintAsStructurallyModified` internally — no need to call it again
- AddNewTimeline does NOT create FScopedTransaction — caller must wrap in transaction
- Template UObject name: `"{VarName}_Template"` via `TimelineVariableNameToTemplateName()`
- Template outer: `Blueprint->GeneratedClass`, with `RF_Transactional` flag
- `GetTrackPin(FName)` is DECLARED but NOT IMPLEMENTED in K2Node_Timeline.cpp — use `FindPin(TrackName)` instead
- Fixed pin FNames: Play, PlayFromStart, Stop, Reverse, ReverseFromEnd, SetNewTime, NewTime, Update, Finished, Direction
- `AllocateDefaultPins` calls `GetBlueprint()` which calls `FindBlueprintForNodeChecked()` — node must be on a graph with UBlueprint outer chain
- DestroyNode automatically removes associated template from Blueprint->Timelines
- Current CreateNodeByClass does NOT handle timelines — would create broken node (no template)
- No `timeline` plan op exists yet in OlivePlanOps vocabulary
- Module: UK2Node_Timeline in BlueprintGraph; UTimelineTemplate/UCurveFloat in Engine; AddNewTimeline in UnrealEd
- ETimelineLengthMode in Components/TimelineComponent.h: TL_TimelineLength (0), TL_LastKeyFrame (1)
- ERichCurveInterpMode in Curves/RealCurve.h: RCIM_Linear, RCIM_Constant, RCIM_Cubic, RCIM_None

## Interface Event API Key Facts (UE 5.5)
- Research report: `plans/research/interface-event-resolution.md`
- Interface functions with NO outputs: `FunctionCanBePlacedAsEvent()` returns true — implemented as `UK2Node_Event` in EventGraph
- Interface functions WITH outputs: `FunctionCanBePlacedAsEvent()` returns false — get own graph in `ImplementedInterfaces[i].Graphs`
- `ConformImplementedInterfaces` only creates graphs for NON-event interface functions (line 7371 in BlueprintEditorUtils.cpp)
- Create interface event node: `EventNode->EventReference.SetFromField<UFunction>(InterfaceFunc, false)` + `bOverrideFunction = true`
- Use `SetFromField` not `SetExternalMember` — SetFromField also sets MemberGuid for BPI rename tracking
- `UK2Node_Event::IsInterfaceEventNode()` checks if EventReference parent class IsChildOf(UInterface)
- For BPI (not native), search SkeletonGeneratedClass: `CastChecked<UBlueprint>(InterfaceClass->ClassGeneratedBy)->SkeletonGeneratedClass`
- Duplicate detection: `FBlueprintEditorUtils::FindOverrideForFunction(BP, InterfaceClass, EventName)` — pass interface class, NOT parent class
- `FBlueprintEditorUtils::FindImplementedInterfaces(BP, true, OutClasses)` gets all interfaces including inherited
- `UEdGraphSchema_K2::FunctionCanBePlacedAsEvent()`: !outputs AND overridable AND !static AND !const AND !thread-safe AND !ForceAsFunction

## Autocast / Auto-Conversion API Key Facts (UE 5.5)
- Research report: `plans/research/autocast-api.md`
- `SearchForAutocastFunction(OutputType, InputType)` returns `TOptional<FSearchForAutocastFunctionResults>` with `{FName TargetFunction, UClass* FunctionOwner}`
- `FindSpecializedConversionNode(OutputType, InputPin, bCreateNode)` returns `TOptional<FFindSpecializedConversionNodeResults>` with `UK2Node*` template
- `CreateAutomaticConversionNodeAndConnections(PinA, PinB)` is the one-stop shop: finds conversion, spawns node, auto-wires
- `Schema->TryCreateConnection(PinA, PinB)` handles auto-conversion internally via `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE`
- **CRITICAL:** `CanSafeConnect()` does NOT include `CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE` — only MAKE and MAKE_WITH_PROMOTION
- Autocast functions: `BlueprintAutocast` metadata + FUNC_Static|Native|Public|BlueprintPure + one input + return value
- `FAutocastFunctionMap` is file-scope internal to EdGraphSchema_K2.cpp — not directly accessible
- `SplitPin(Pin, bNotify)` creates sub-pins named `{PinName}_{ComponentName}` (e.g., `Location_X`, `Rotation_Pitch`)
- NO autocast from Vector/Rotator to float — must use SplitPin or break_struct
- OlivePinConnector::CreateConversionNode() is NOT IMPLEMENTED (returns nullptr) — use TryCreateConnection instead

## Enhanced Input API Key Facts (UE 5.5)
- Research report: `plans/research/enhanced-input-actions.md`
- `UInputAction` and `UInputMappingContext` are `UDataAsset` subclasses (NOT blueprints) — `blueprint.create` cannot create them
- Module `"EnhancedInput"` provides both classes; module `"InputBlueprintNodes"` provides `UK2Node_EnhancedInputAction`
- Headers: `InputAction.h`, `InputMappingContext.h`, `EnhancedActionKeyMapping.h`, `EnhancedInputSubsystems.h` (all in EnhancedInput/Public)
- Create assets via: `AssetTools.CreateAsset(Name, PackagePath, UInputAction::StaticClass(), nullptr)` — do NOT use factory classes (private to InputEditor module)
- Add key mapping: `IMC->MapKey(InputAction*, FKey)` returns `FEnhancedActionKeyMapping&` for further modifier/trigger config
- `EInputActionValueType`: Boolean (button), Axis1D (float), Axis2D (Vector2D), Axis3D (Vector)
- `UK2Node_EnhancedInputAction::InputAction` MUST be set before `AllocateDefaultPins()` — null InputAction = no data pins, compile error
- Exec pin names (ETriggerEvent values): `Triggered`, `Started`, `Ongoing`, `Canceled`, `Completed` — only `Triggered` visible by default
- Node is ubergraph-only (`IsCompatibleWithGraph` checks GT_Ubergraph) and requires `Blueprint->SupportsInputEvents()` = true (Actor/Pawn/Character)
- `editor.run_python` is the best current approach: `unreal.AssetToolsHelpers.get_asset_tools().create_asset(..., unreal.InputAction, None)`, then `imc.map_key(ia, unreal.Key.space_bar)`
- `UEnhancedInputLocalPlayerSubsystem::AddMappingContext(IMC, Priority)` is a UFUNCTION(BlueprintCallable) — FindFunction will locate it for plan_json `call` ops

## Competitive Context Injection Analysis
- Research report: `plans/research/context-injection-competitive-analysis.md`
- All major tools (Cursor, Aider, Windsurf, Copilot, Cody) do retrieval at SYSTEM level, not AI level
- Aider: graph ranking (PageRank on dependency graph) with 50x multiplier for chat-mentioned files, injected EVERY turn
- Cursor: embedding-based, passive auto-search before every AI response + `@Codebase` explicit trigger
- Windsurf Cascade: full-codebase indexing + real-time action tracking; system selects context automatically
- Copilot Workspace: explicit spec-plan pipeline forces model to describe existing patterns BEFORE writing code
- Agentic RAG finding: "MUST call X first" system prompt instruction reliably produces first-action search behavior
- "Lost in the middle" effect: context at START or END of prompt is better utilized than middle — place injected templates at end
- Token budget: 8 templates ≈ 380 tokens; context quality >> context quantity; >32k-64k tokens degrades most models

## Autonomous Template Pre-Search (Olive-Specific)
- Research report: `plans/research/autonomous-template-presearch.md` — COMPLETE, ready for implementation
- Approach A (correct): pre-search in `SendMessageAutonomous()` before `LaunchCLIProcess()`, inject into stdin
- Implementation location: `OliveCLIProviderBase.cpp`, replacing generic nudge at line ~514
- API: `FOliveTemplateSystem::Get().SearchTemplates(CleanedQuery, 8)` — game thread, in-memory, microseconds
- Strip `@` from user message before tokenizing; pass raw message to `Tokenize()`
- Show matched function names — most actionable signal for agent to follow up with `get_template(id, pattern)`
- Guard with `!IsContinuationMessage(UserMessage)` — skip on continuation runs
- Fallback: if 0 results, use existing generic nudge text

## Autonomous Agent Prompt Flow
- Research report: `plans/research/autonomous-agent-prompt-flow.md`
- Sandbox dir: `{ProjectDir}/Saved/OliveAI/AgentSandbox/`
- Sandbox CLAUDE.md = hardcoded preamble + `cli_blueprint.txt` + `recipe_routing.txt` + `blueprint_design_patterns.txt` (~2,250 tokens)
- Sandbox AGENTS.md = copy of plugin root AGENTS.md (developer docs, ~4,000–5,000 tokens — known issue, can be deleted)
- CLI args autonomous: `--print --output-format stream-json --verbose --dangerously-skip-permissions --max-turns {N}` (no --strict-mcp-config, no --append-system-prompt)
- Stdin message = user message + optional @-mention asset state + always-appended pattern research nudge (~100 tokens)
- Tool filter in autonomous mode: `DetermineToolPrefixes()` keyword-matches the user message to reduce tools/list response
- Template catalog NOT in autonomous sandbox CLAUDE.md — agent must call `blueprint.list_templates` to discover templates
- Self-correction (`FOliveSelfCorrectionPolicy`) only fires on the orchestrated (API) path, NOT autonomous mode
- Continuation prompts (`BuildContinuationPrompt`): fired on timeout, cap 3 auto-continues; includes original task, tool call log, run outcome, asset state

## Search Patterns
- Use `find "C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/..." -name "*.h"` to locate headers
- Engine also accessible at same path with UE_5.1, UE_5.6, UE_5.7 installed on C: drive
