# Researcher Agent Memory

## Engine Location
UE 5.5 installed at: `C:/Program Files/Epic Games/UE_5.5/`
PCG plugin headers: `C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/PCG/Source/PCG/Public/`

## Key Research Reports
- `plans/research/pcg-api-ue55.md` — Full PCG plugin public API (UPCGGraph, UPCGNode, UPCGPin, UPCGSettings, EPCGDataType, UPCGSubgraphSettings, UPCGComponent, UPCGSubsystem)
- `plans/research/uanimstatetransitionnode-api-ue55.md` — (exists, topic unknown)
- `plans/research/log-failure-analysis.md` — Session log failure analysis; 5 Olive false negatives documented; interface call gap is primary issue

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

## Search Patterns
- Use `find "C:/Program Files/Epic Games/UE_5.5/Engine/Plugins/..." -name "*.h"` to locate headers
- Engine also accessible at same path with UE_5.1, UE_5.6, UE_5.7 installed on C: drive
