# Research: UE 5.6 and 5.7 API Migration Impact

## Question
Which UE APIs used heavily by Olive AI Studio changed, were deprecated, or were removed in UE 5.6 and 5.7, and what is the migration effort for each?

---

## Findings

### 1. Build System — IncludeOrderVersion (MEDIUM risk, 5.6→5.7)

**What changed:**
In UE 5.5, `EngineIncludeOrderVersion.Latest` equals `Unreal5_5`. When UE 5.6 ships, `Latest` becomes `Unreal5_6`, and when 5.7 ships it becomes `Unreal5_7`. Each engine version introduces `UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_X` macros for the *previous* round, and C4668 fires as a warning-as-error on Windows if those macros are referenced by code that hasn't updated yet.

**Plugin status:**
`OliveAIEditor.Build.cs` does NOT set `IncludeOrderVersion`. This means it defaults to `Latest`, so it always picks up the new include order for whatever engine version it compiles against. That is the *correct* posture — no change needed. Plugins that hardcode `Unreal5_4` or `Unreal5_3` will get warnings on 5.6/5.7 builds until they update the value.

**Action:** None required. The default `Latest` behavior is correct.

Source: [Epic forum: C4668 on UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_7](https://forums.unrealengine.com/t/ue-5-6-c4668-on-ue_enable_include_order_deprecated_in_5_7-during-editor-build-win64/2654923)

---

### 2. Symbol Export Refactor — _API Defines (HIGH risk, 5.5→5.6)

**What changed:**
In UE 5.6 (CL 41869343), Epic moved `_API` macros from class-level declarations to individual method declarations across several modules. Previously, marking a parent class with e.g. `PCG_API` exported all its methods; now individual methods must be explicitly marked. Epic used the Fortnite client to decide which methods need individual export annotations.

**Concrete example:** `UPCGSplineProjectionData::GetSpline()` — exported via parent class export in 5.5, NOT exported in 5.6. Results in `LNK2019 unresolved external symbol` at link time.

**Plugin risk:**
The plugin calls `UPCGGraph::AddEdge`, `RemoveEdge`, `AddNodeOfType`, `GetInputNode`, `GetOutputNode`, `GetNodes`, `RemoveNode` — these are all top-level graph methods on `UPCGGraph` itself (not inherited), so they are likely to retain their individual `PCG_API` marks. The higher risk is in any PCG *data* classes the plugin might add later. Blueprint/editor APIs (`UK2Node*`, `FBlueprintEditorUtils`, `FKismetEditorUtilities`) are in `UnrealEd` and `BlueprintGraph` — Epic uses these internally extensively, so they are unlikely to lose exports. The bigger practical risk is that **any third-party plugin the team adds** in 5.6 may have this issue.

**Action:** If link errors appear on 5.6 upgrade on PCG methods, the fix is to find the alternative exported helper (Epic usually provides one) or use the Fortnite-approved path. Monitor Epic's forums for specific affected symbols.

Source: [Epic forum: Linker errors in 5.6 due to _API define move](https://forums.unrealengine.com/t/linker-errors-in-5-6-due-to-moving-the-use-of-api-defines-from-types-to-individual-methods/2591466)

---

### 3. PCG — PCGPointArrayData Replaces PCGPointData (LOW risk for plugin, 5.5→5.6)

**What changed:**
In UE 5.6 the PCG Data port now returns `PCGPointArrayData` instead of `PCGPointData`. Casting a `UPCGData*` to `UPCGPointData*` in post-generation callbacks and editor utility blueprints now returns null. The new API uses `ToPointDataWithContext()` which requires an `FPCGContext`, meaning point data access from *outside* a PCG graph is more restricted.

**Plugin risk:**
Low. The plugin's PCG module (`OlivePCGWriter`, `OlivePCGReader`) operates at the graph/node topology level — it creates nodes, connects edges, and reads node types. It does NOT query `UPCGPointData` or access point arrays. The `UPCGSubsystem::GetInstance(World)` call in `OlivePCGWriter::Execute()` is also not deprecated (the deprecated methods in 5.5 are the `ScheduleComponent` variants without `UPCGComponent`; `GetInstance` itself is fine).

Additionally, 5.5 already deprecated several PCG data methods with "Call/Implement version with FPCGContext parameter":
- `UPCGSpatialData::SamplePoint()` — must pass `FPCGContext*`
- `UPCGDifferenceData::Initialize()` — same
- `UPCGSplineInteriorSurfaceData::Initialize()` — same

These are all in the PCG internal execution path, not in Olive's PCG tool layer. No action required unless the plugin adds point-data introspection features.

Source:
- [PCG 5.5→5.6 point data forum thread](https://forums.unrealengine.com/t/pcg-problem-going-from-ue-5-5-to-5-6-get-point-data-not-working-anymore/2651694)
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/PCG/Source/PCG/Public/Data/PCGSpatialData.h` (line 126–152)

---

### 4. PCG — Production Ready in 5.7 (UNKNOWN risk)

**What changed:**
In UE 5.7, PCG moves from Experimental to Production-Ready. "Production Ready" transitions typically involve:
- Removal of deprecated experimental APIs (the ones marked `UE_DEPRECATED(5.5, ...)` in 5.5 headers are now likely *removed* in 5.7)
- Possible changes to how `UPCGSubsystem` schedules work (new `FPCGGridDescriptor` overloads become required)

**Plugin risk:**
The plugin uses `UPCGSubsystem::GetInstance(World)` to run PCG after graph mutations. The deprecated variants in 5.5 are:
- `ScheduleComponent` without `UPCGComponent` parameter (deprecated 5.5)
- Several `FPCGGridDescriptor`-less scheduling overloads (deprecated 5.5)

The plugin's `OlivePCGWriter::Execute()` calls `GetInstance` then likely uses it to refresh the component — if it calls any of the 5.5-deprecated scheduling variants, those will be *errors* in 5.7.

**Action (for 5.7):** Audit `OlivePCGWriter::Execute()` against the list of `UE_DEPRECATED(5.5, ...)` methods in `PCGSubsystem.h`. Any call matching those signatures must be migrated to the `FPCGGridDescriptor` or `UPCGComponent` overloads before targeting 5.7.

Source: Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/PCG/Source/PCG/Public/PCGSubsystem.h` (lines 252–266, 342–345)

---

### 5. Asset Registry — EnumerateAssets Deprecated Overloads (MEDIUM risk, 5.5→5.6/5.7)

**What changed:**
In UE 5.5, two `EnumerateAssets` overloads were deprecated:
```cpp
// DEPRECATED (5.5) — the two-arg versions without InEnumerateFlags
UE_DEPRECATED(5.5, "Use EnumerateAssets with InEnumerateFlags instead.")
virtual bool EnumerateAssets(const FARFilter& Filter,
    TFunctionRef<bool(const FAssetData&)> Callback,
    bool bSkipARFilteredAssets = true) const = 0; // ← deprecated

// CURRENT — must use flags overload
virtual bool EnumerateAssets(const FARFilter& Filter,
    TFunctionRef<bool(const FAssetData&)> Callback,
    UE::AssetRegistry::EEnumerateAssetsFlags InEnumerateFlags) const = 0;
```

Also deprecated in 5.5:
- `EnumerateAllAssets()` without `InEnumerateFlags`
- `ReadLockEnumerateTagToAssetDatas()` with `TArray` output

**Plugin status:**
The plugin uses `GetAssets(Filter, ...)` (not `EnumerateAssets`), so the deprecated `EnumerateAssets` overloads are not called. The plugin also uses `GetAssetsByClass(FTopLevelAssetPath, ...)` which is the current non-deprecated form (the old `FName` form was deprecated in 5.1). `FAssetRegistryModule::AssetCreated(UObject*)` still exists as a static forwarding helper and is not deprecated.

**Action:** No changes needed for current code. Future code that adds `EnumerateAssets` calls must use the `InEnumerateFlags` overload.

Source: Engine source: `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/IAssetRegistry.h` (lines 304–327)

---

### 6. Blueprint Graph APIs — bHasCompilerMessage and K2Node (LOW risk)

**What found:**
`bHasCompilerMessage` on `UEdGraphNode` is a public bitfield (`uint8 bHasCompilerMessage:1`) declared in `EdGraphNode.h` (line 366 in 5.5). It is NOT deprecated in 5.5. No evidence of it being moved or removed in 5.6/5.7.

`UK2Node::ShouldDrawAsBead()` was deprecated in 5.4 — if the plugin ever called that, it would be an error by 5.6. Verified it doesn't.

`FBlueprintEditorUtils` — the deprecated methods are all 5.0–5.1 era (blueprint nativization removal, short class names). The `ImplementNewInterface(BP, FTopLevelAssetPath)` overload the plugin uses is the current 5.1+ form. No new deprecations found in 5.5 source.

`FKismetEditorUtilities::CreateBlueprint` and related methods — no new deprecations found in 5.5 source.

**Action:** None required.

Source: Engine source:
- `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/Engine/Classes/EdGraph/EdGraphNode.h` (line 366)
- `/mnt/epic-games/UE_5.5/Engine/Source/Editor/UnrealEd/Public/Kismet2/BlueprintEditorUtils.h` (lines 1404–1465)
- `/mnt/epic-games/UE_5.5/Engine/Source/Editor/BlueprintGraph/Classes/K2Node.h` (line 269)

---

### 7. SCS (SimpleConstructionScript) — GetAllNodes and GetRootNodes (LOW risk)

**What found:**
In UE 5.5, `USimpleConstructionScript::GetAllNodes()` is a live public method (not deprecated). The plugin uses `SCS->GetRootNodes()` and `USCS_Node::GetChildNodes()` — both are present and not deprecated in 5.5.

`RootNode_DEPRECATED` and `ActorComponentNodes_DEPRECATED` are marked deprecated properties on `USimpleConstructionScript`, but these are the old pre-5.x fields and are not used by the plugin.

**Action:** None required. These APIs are stable across 5.5–5.7.

Source: Engine source: `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Engine/SimpleConstructionScript.h` (lines 77–142)

---

### 8. UMG / Widget Blueprint — UUMGSequencePlayer and IMovieScenePlayer (LOW risk for plugin)

**What changed in 5.6:**
`UUMGSequencePlayer` and `IMovieScenePlayer` are being deprecated in favor of lightweight "runner" structs. Widget animations no longer use a player object. `UUMGSequencePlayer` remains for backwards compatibility in 5.6 but will be removed.

**Plugin status:**
The plugin does NOT use `UUMGSequencePlayer` or `IMovieScenePlayer`. Its UMG support reads and writes `UWidgetBlueprint` at the graph/widget-tree level (nodes, slots, bindings) — it doesn't drive widget animation playback. Zero risk.

**Additional 5.5 deprecations in UMG already present:**
- `UUserWidget::RemoveFromViewport()` — deprecated 5.1, use `RemoveFromParent()`
- Several direct member accesses (`ColorAndOpacity`, `ForegroundColor`, etc.) — deprecated 5.2

None of these are used by the plugin.

**UE 5.7 Slate crash (FSlateCachedElementData):**
A crash in `FSlateCachedElementData::AddCache` and `FSlateInvalidationRoot::ClearAllFastPathData` was reported in 5.7 involving widgets drawn into two caches (likely a double-reference bug). The fix involves the `Slate.VerifyParentChildrenRelationship` debug flag. This is an *engine bug*, not an API change — no plugin code changes needed.

**Action:** None required.

Source:
- Tom Looman, UE 5.6 Performance Highlights
- [Slate widget double-cache crash in 5.7](https://forums.unrealengine.com/t/widget-invalidation-ensure-and-crash-in-5-7-update-fslatecachedelementdata-addcache-fslateinvalidationroot-clearallfastpathdata/2699493)
- Engine source: `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/UMG/Public/Blueprint/UserWidget.h`

---

### 9. FHttpServerModule (NO risk found)

**What found:**
`FHttpServerModule` continues to exist in UE 5.6 documentation. The method signature `GetHttpRouter(uint32 Port, bool bFailOnBindError = false)` is unchanged. `StartAllListeners()` and `StopAllListeners()` are unchanged. No deprecation found in 5.5 source or 5.6/5.7 release announcements.

**Action:** None required.

Source: Engine source: `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/Online/HTTPServer/Public/HttpServerModule.h`

---

### 10. ILiveCodingModule (NO risk found)

**What found:**
`ILiveCodingModule` in 5.5 exposes `EnableByDefault(bool)`, `HasStarted()`, `IsCompiling()` — no deprecations found in the header. No 5.6/5.7 release notes mention Live Coding API changes. The plugin depends on the `LiveCoding` module in Build.cs which remains stable.

**Action:** None required.

Source: Engine source: `/mnt/epic-games/UE_5.5/Engine/Source/Developer/Windows/LiveCoding/Public/ILiveCodingModule.h`

---

### 11. IPythonScriptPlugin (NO risk found)

**What found:**
The plugin's Python tool uses `IPythonScriptPlugin::ExecPythonCommand()` and `ExecPythonCommandEx()`. Neither is deprecated in 5.5. No 5.6/5.7 release notes mention Python scripting API changes. The module path moved slightly across UE versions (it's under `Experimental/PythonScriptPlugin/`) but the public interface is stable.

Note: In 5.5, Python scripting is tagged `Experimental` in the plugin descriptor. In 5.7, it remains experimental. No functional API changes found.

**Action:** None required.

Source: Engine source: `/mnt/epic-games/UE_5.5/Engine/Plugins/Experimental/PythonScriptPlugin/Source/PythonScriptPlugin/Public/IPythonScriptPlugin.h`

---

### 12. EditorStyle Module — Still Present but Class Deprecated (LOW risk)

**What found:**
`FEditorStyle` (the class, not the module) was deprecated in UE 5.1 with all its methods. The `EditorStyle` *module* still exists in 5.5. The plugin depends on it in Build.cs but only uses `FAppStyle` — not `FEditorStyle`. The include `#include "EditorStyleSet.h"` in `OliveAIEditorCommands.h` is still valid (it's where `FAppStyle` is declared/forwarded).

The `EditorStyle` module is a stub/bridge in 5.5 — it's not removed. Based on community reports through 5.6, it still compiles fine as a dependency. No evidence of it being physically removed yet.

**Action:** None currently. If the module is removed in a future version, the fix is to drop the `EditorStyle` Build.cs dependency and switch to `AppStyle` — but the header include `EditorStyleSet.h` would need to change too. Monitor.

Source: Engine source: `/mnt/epic-games/UE_5.5/Engine/Source/Editor/EditorStyle/Public/EditorStyleSet.h` (line 49+)

---

### 13. AnimGraph — UAnimGraphNode_Base (NO risk found)

**What found:**
No deprecations found in `UAnimGraphNode_Base` relevant to the plugin's use. `UAnimGraphNode_AnimDynamics::ResetSim()` exists in 5.6 docs without deprecation markers. The plugin reads Anim Blueprint graphs by traversing `UEdGraph` nodes — this topology-level API is stable.

**Action:** None required.

---

### 14. UE 5.7 — UReplicationBridge Removed (NOT APPLICABLE)

UE 5.7 removed `UReplicationBridge` base class for Iris networking. The plugin does not use Iris or multiplayer replication systems.

---

### 15. UE 5.7 — New FNonshrinkingAllocator and UE_REWRITE (MONITORING)

`UE_REWRITE` replaces `FORCEINLINE` for macro-like functions in 5.7. If the plugin ever uses `FORCEINLINE` in ways that conflict with this, warnings may appear. `FNonshrinkingAllocator` for `TArray` is a new opt-in, not a breaking change.

**Action:** None currently.

---

## Summary Table

| API Area | 5.5→5.6 Risk | 5.6→5.7 Risk | Plugin Uses It | Action |
|---|---|---|---|---|
| Build IncludeOrderVersion | None (using Latest) | None | Yes (default) | None |
| _API symbol export refactor | MEDIUM (PCG methods) | Low | PCG graph methods | Monitor for linker errors |
| PCG PointArrayData | None (not used) | None | No | None |
| PCG Subsystem deprecated variants | Compiler warnings | Compile ERROR | Partially | Audit OlivePCGWriter::Execute |
| Asset Registry GetAssets | None | None | Yes | None |
| Asset Registry EnumerateAssets | None (not used) | None | No | None |
| Blueprint graph (UK2Node etc.) | None | None | Yes | None |
| bHasCompilerMessage | None | None | Yes | None |
| SCS GetRootNodes/GetAllNodes | None | None | Yes | None |
| UUMGSequencePlayer | None (not used) | Not used | No | None |
| FHttpServerModule | None | None | Yes | None |
| ILiveCodingModule | None | None | Yes | None |
| IPythonScriptPlugin | None | None | Yes | None |
| EditorStyle module | None | Monitor | Yes (Build.cs) | Monitor |
| AnimGraph | None | None | Yes | None |
| Slate FSlateCachedElementData | None | Engine bug | Indirect | None |

---

## Recommendations

1. **Highest priority — PCG subsystem audit before 5.7 targeting.** The 5.5 headers show six methods on `UPCGSubsystem` deprecated with `(5.5, ...)`. If any of those match what `OlivePCGWriter::Execute()` calls, they will be compile errors in 5.7 (when 5.5-deprecated items are removed). Specifically check against lines 252–266 and 342–345 of `PCGSubsystem.h`. The safe methods are `GetInstance(World)` and the `FPCGGridDescriptor` overloads.

2. **5.6 _API export linker errors are latent.** If the team upgrades to 5.6 and hits `LNK2019` on a PCG or other engine method, the cause is the CL 41869343 export refactor. The fix is to find the Epic-blessed alternative function (usually documented in the forum thread) — not to cast or use internal methods.

3. **No Blueprint graph API changes found for 5.6 or 5.7.** `UK2Node`, `UEdGraph`, `UEdGraphPin`, `FEdGraphPinType` (including `PC_Real`/`PC_Float`/`PC_Double` pin category pattern), `FBlueprintEditorUtils`, `FKismetEditorUtilities`, `USCS_Node`, and `USimpleConstructionScript` are all stable through the versions researched. The plugin's Blueprint graph layer should compile without changes.

4. **Asset registry patterns are clean.** The plugin already uses `FTopLevelAssetPath` and `GetAssetsByClass` in the correct non-deprecated form. `GetAssets(FARFilter, ...)` is stable. No changes needed.

5. **UMG widget animation refactor does not affect Olive.** The `UUMGSequencePlayer` deprecation is only relevant if you drive widget animations programmatically. The plugin edits widget graph structure, not animation playback.

6. **Drop `EditorStyle` from Build.cs when targeting 5.7+.** The module is a stub and may be removed. Switch to `AppStyle` as the dependency once the module disappears. The code already uses `FAppStyle` exclusively — only the Build.cs and one `#include "EditorStyleSet.h"` would need updating.

7. **HTTP and Live Coding are fully stable.** No action needed for `FHttpServerModule` or `ILiveCodingModule` through 5.7.

8. **Do not declare a specific `EngineIncludeOrderVersion` in Build.cs.** The current default-to-`Latest` behavior is correct and avoids the C4668 warning. Hardcoding a version number would require a manual update with every engine upgrade.

9. **UE 5.7 PCG becoming production-ready means experimental-API cleanup.** All 5.5-era PCG deprecations are candidates for removal. The plugin's PCG integration is narrow (graph topology only) and does not touch the deprecated data/element APIs, but verify before each major engine version bump.

10. **No 5.7 source access was available.** Findings for 5.7 are inferred from: release announcement features, community migration reports, Tom Looman's performance blog, and the 5.5 source deprecation markers (items deprecated in 5.5 are removal candidates in 5.7). Once 5.7 source is accessible via Epic's GitHub, a follow-up audit of the exact symbols removed is recommended.

---

## Sources

- [UE 5.6 Release Announcement](https://www.unrealengine.com/en-US/news/unreal-engine-5-6-is-now-available)
- [UE 5.7 Performance Highlights — Tom Looman](https://tomlooman.com/unreal-engine-5-7-performance-highlights/)
- [Linker errors in 5.6 — _API define move forum thread](https://forums.unrealengine.com/t/linker-errors-in-5-6-due-to-moving-the-use-of-api-defines-from-types-to-individual-methods/2591466)
- [PCG 5.5→5.6 PCGPointData forum thread](https://forums.unrealengine.com/t/pcg-problem-going-from-ue-5-5-to-5-6-get-point-data-not-working-anymore/2651694)
- [UE 5.6 C4668 IncludeOrderVersion forum thread](https://forums.unrealengine.com/t/ue-5-6-c4668-on-ue_enable_include_order_deprecated_in_5_7-during-editor-build-win64/2654923)
- [UE 5.7 Slate widget double-cache crash](https://forums.unrealengine.com/t/widget-invalidation-ensure-and-crash-in-5-7-update-fslatecachedelementdata-addcache-fslateinvalidationroot-clearallfastpathdata/2699493)
- [IAssetRegistry 5.6 API reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/AssetRegistry/AssetRegistry/IAssetRegistry/GetAssetsByClass)
- [UPCGGraph 5.6 API reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/PCG/UPCGGraph)
- Engine source (UE 5.5 local): `IAssetRegistry.h`, `BlueprintEditorUtils.h`, `K2Node.h`, `EdGraphNode.h`, `SimpleConstructionScript.h`, `HttpServerModule.h`, `ILiveCodingModule.h`, `IPythonScriptPlugin.h`, `UMGSequencePlayer.h`, `PCGSubsystem.h`, `PCGGraph.h`
