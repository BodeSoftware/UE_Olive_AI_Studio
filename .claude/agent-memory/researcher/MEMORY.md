# Researcher Agent Memory

## Engine Location
UE 5.5 installed at: `C:/Program Files/Epic Games/UE_5.5/`
PCG plugin headers: `C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/PCG/Source/PCG/Public/`

## Key Research Reports
- `plans/research/bow-arrow-session-log-analysis-2026-03-08d.md` — Run 08d: QUALITY RECOVERED. 86% tool success, 57% plan_json (lower but all recovered), ~20min total. No gutted functions. All 3 BPs compile SUCCESS. Reviewer SATISFIED. Fix B triggered x2, worked. 1800s runtime used ~17.8min, no kill. Zero auto-continues. 10 recommendations filed.
- `plans/research/bow-arrow-session-log-analysis-2026-03-08c.md` — Post-fix run analysis (2026-03-08c): REGRESSION. 77% tool success, 67% plan_json, ~36min total. Fix B (rollback) confirmed x2, Fix A unobservable. describe_node_type failed for SetFieldOfView+K2_AttachToComponent (catalog gaps) → StartAim gutted, NockArrow retried. 900s wall-clock kill mid-work. 8 recommendations filed.
- `plans/research/bow-arrow-session-log-analysis-2026-03-08b.md` — Previous run: 88% tool success, 67% plan_json, ~19min total
- `plans/research/pcg-api-ue55.md` — Full PCG plugin public API
- `plans/research/uanimstatetransitionnode-api-ue55.md` — (exists, topic unknown)
- `plans/research/log-failure-analysis.md` — Session log failure analysis
- `plans/research/interface-event-resolution.md` — Interface event resolution gap
- `plans/research/competitive-tool-analysis.md` — Full competitor landscape (AIK, flopperam, chongdashu, gimmeDG, etc.), tool count analysis, MCP spec status
- `plans/research/competitor-deep-dive-2026-03.md` — DEEP dive: NeoStack AIK (full changelog, ACP/MCP dual transport, Profiles, 500+ checks), Aura 12.0 (Telos 2.0, Dragon Agent, pricing tiers, 43 tools), Ultimate Co-Pilot (Python MCP bridge, scene population), Ludus AI (broken BP repair, coverage gaps), plus full comparison matrix and adoption recommendations
- `plans/research/multi-agent-architectures.md` — Roo Code boomerang/orchestrator, Aider architect/editor split, Claude Code subagents, Devin 2.0, token optimization patterns, reflection agent research
- `plans/research/context-management-approaches.md` — Aider/Cursor/Roo/Windsurf/Copilot/Continue context strategies; tiered template overview proposal for Olive Planner

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

## NeoStack / AIK Architecture (plans/research/competitor-deep-dive-2026-03.md)
- Product: Agent Integration Kit by Betide Studio — docs at aik.betide.studio; $109.99 one-time on FAB
- MCP server: port 9315, auto-starts with editor; SSE endpoint GET /sse
- DUAL transport: ACP (bundled adapter binary) for Claude Code/Codex/Copilot/Cursor; MCP SSE for Gemini CLI; native OpenRouter client (API key only, no external process)
- Profiles system: 5 built-in profiles (Full, Animation, Blueprint & Gameplay, Cinematics, VFX & Materials); whitelist tool access + custom instructions injected per profile; NO AGENTS.md — AIK does not generate CLAUDE.md
- Tool consolidation (v0.5.0): 27+ → ~15 unified tools; ~15 publicly known names: edit_rigging, edit_animation_asset, edit_character_asset, edit_ai_tree, edit_graph, generate_asset, read_asset (partial list)
- 36+ asset type readers for @-mention context injection
- Validation: 500+ checks are crash-prevention guards added reactively in v0.3.0 after launch crashes — scattered null/type/state guards, NOT semantic plan validation. Plus v0.5.0 try/catch wrapper around all tool execution.
- No plan-preview-before-execute; no structured self-correction; error recovery is the agent's responsibility
- Asset coverage: Blueprint + ALL animation + Materials + Niagara + Sequencer + IK + Widget + BT/ST + PCG + MetaSounds + EQS + viewport screenshots + Python execution
- @-mention UX: user types @AssetName in chat → asset reader auto-generates structured context for LLM
- Multi-session: up to 8 concurrent agent sessions
- Conversation handoff: switch connected agents mid-session with context preserved (v0.5.0)

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

## Aura (RamenVR) Key Facts
- Architecture: UE plugin (per-engine-version) + local server process + hosted cloud backend (credit-based)
- Three modes: Ask (analysis, no changes), Plan (stored as .md files in /Saved/.Aura/plans/), Agent (live changes with Keep/Reject/Review diff UI)
- Telos 2.0: proprietary reasoning framework by MIT-trained researchers; claimed >99% accuracy on existing BP graphs, 25x error reduction, 10x speed at 1/10 cost vs competitors. Mechanism unknown — likely fine-tuned or RLHF on top of Claude Sonnet/Opus.
- Dragon Agent (Aura 12.0, March 2 2026): fully autonomous via Unreal Python; runs without UE open; compiles C++, closes/reopens projects; orchestrates Claude Code. This is Olive's editor.run_python taken to full autonomy.
- Agents: Blueprint (Telos), C++ (self-correcting claims), Python/Dragon, Art Tooling — each specialized, each consumes credits
- 43 Unreal-native tools total (Blueprint, Level Design, 3D Modeling, Asset Generation, Code Generation)
- MCP: exposes tools via MCP for Claude Code, VS Code, Cursor (external agent use). AccelByte vertical MCP integration published.
- Pricing: Free 2-week trial ($40 credit), Pro $40/month ($40 credit), Ultra $200/month ($280 credit), Enterprise custom
- Models: Claude Sonnet 4.6 (base), Haiku (cheap tasks), Opus 4.6 ("Super Mode"); credits consumed by subagent complexity
- Known bug (Jan 2026 launch): "AI refuses to work on existing blueprints" — Telos 2.0 claims resolution; unconfirmed
- Windows only (Mac planned); UE 5.3–5.7; does NOT train on asset/game data (conv data may be used; enterprise training off by default)

## Competitor Pricing Summary (March 2026)
- Aura: $40/month (Pro), $200/month (Ultra)
- AIK (NeoStack): $109.99 one-time
- Ultimate Engine Co-Pilot: one-time (price undisclosed, on FAB)
- Kibibyte Blueprint Generator AI: on FAB (~3,300 generations/$1 with OpenAI key)
- Ludus AI: free tier + paid (price undisclosed)
- All require Windows

## SpawnActor ExposeOnSpawn & Exec Pin Rollback (UE 5.5)
- Research report: `plans/research/spawn-actor-expose-on-spawn-and-exec-rollback.md`
- `UK2Node_SpawnActorFromClass::CreatePinsForClass` called from PostPlacedNewNode/ReconstructNode/OnClassPinChanged ONLY — NOT from AllocateDefaultPins. Olive's NewObject path skips it.
- Fix for missing ExposeOnSpawn pins: call `SpawnActorNode->ReconstructNode()` AFTER setting the variable's CPF_ExposeOnSpawn flag. Must be inside the same FScopedTransaction. Then call `Graph->NotifyGraphChanged()`.
- Alternative: reorder plan steps so variable creation happens BEFORE SpawnActor node allocation.
- "Exec→Exec TypesIncompatible" is a MISLEADING diagnostic from OlivePinConnector. Real cause: `bOrphanedPin == true` on BeginPlay's exec out pin. `CanCreateConnection` returns CONNECT_RESPONSE_DISALLOW ("Cannot make new connections to orphaned pin") and BuildWiringDiagnostic has no bOrphanedPin case — falls through to TypesIncompatible.
- `bOrphanedPin` IS serialized (UEdGraphPin::ExportTextItem line 1097) — NOT transient. Survives/restored by undo.
- Root cause of orphaned exec pin after rollback: BeginPlay node was never Modify()-called before the failed plan wired it. K2Schema::TryCreateConnection calls MarkBlueprintAsModified but NOT Node->Modify(). OlivePinConnector calls Blueprint->Modify() but NOT individual node Modify().
- Fix: In WireExecConnection, check `Pin->bOrphanedPin` before connecting. If true, call `OwningNode->ReconstructNode()` to clear it. Also call `BeginPlayNode->Modify()` before wiring exec in PhaseWireExec.
- Fix for BuildWiringDiagnostic: add bOrphanedPin check before the generic else block → new EOliveWiringFailureReason::OrphanedPin.

## Template Context Budget (2026-03-07 research)
- `GetTemplateOverview()` produces ~2,500-4,000 chars per template with 40 functions (3 templates = 27-31K chars)
- Primary cost is per-function detail: 15 detailed entries at ~100-200 chars each + descriptions/tags
- Planner/Architect needs: template name, type, description, function names only, fetch hint
- Planner does NOT need: node counts, per-function description text, per-function tag lists
- Aider uses signature-only repo map (function declarations, no bodies) — 5-8x smaller than full overview
- "Lost in the middle" research: LLM accuracy drops 30%+ when relevant info is buried mid-context
- Recommendation: add `GetTemplateHeaderForPlanner()` (300-400 chars per template) — name + parent + 1-sentence description + comma-separated function names + fetch hint
- Use `MatchedFunctions` from `FOliveDiscoveryEntry` to highlight relevant functions in the header
- Cap total `TemplateOverviews` block at 6,000 chars in Scout/ExecuteCLIPath
- Keep full `GetTemplateOverview()` for `blueprint.get_template` tool responses (Builder use only)

## Error Recovery Patterns (2026-03-07 research)
- Full report: `plans/research/error-recovery-patterns.md`
- FUNCTION_NOT_FOUND bug: current `FuzzyMatch()` uses catalog-wide search → returns wrong-class results (e.g., `SetSphereRadius` when asked for `SetSpeed` on ProjectileMovementComponent). Fix: scope to target class via `TFieldIterator<UFunction>(ResolvedClass)`
- `FindFunctionEx().SearchedLocations` trail is built but NOT included in the error sent to Builder — only goes to UE_LOG. It IS in ErrorMessage string but Suggestion uses wrong-class catalog matches.
- Property-vs-function confusion: `MaxSpeed` is FProperty not UFunction — Builder should get "use set_var not call" hint when function name matches a property name
- Four-part actionable error format (practitioner: 20%→95% recovery): (1) error type (2) specific details (3) diagnosis with causes (4) numbered recovery steps with exact tool names
- Cursor enforces "read before write" via system prompt only — soft, skippable. No tool-layer enforcement exists anywhere yet.
- Aider repo map = signature-only function list injected upfront — eliminates need for read-before-write entirely; 5-8x smaller than full content
- Structured reflection (arxiv 2509.18847): explicit diagnose+propose format on failure yields large gains on BFCL v3. Must be structured, not just "think harder."
- Intrinsic self-correction WITHOUT external validator makes accuracy WORSE (ACL 2025) — don't tell model to reconsider unless you have ground truth to compare against

## Search Patterns
- Use `find "C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/..." -name "*.h"` to locate headers
- Engine also accessible at same path with UE_5.1, UE_5.6, UE_5.7 installed on C: drive
