# Research: Niagara C++ API Changes — UE 5.5 → 5.6 → 5.7

## Question
Which Niagara C++ APIs used by `OliveNiagaraWriter.cpp` changed between UE 5.5, 5.6, and 5.7? Are there
breaking changes that would require code edits for a future port?

## Method
Direct header comparison across all three installed engine versions:
- 5.5: `/mnt/epic-games/UE_5.5/`
- 5.6: `/mnt/epic-games/UE_5.6/`
- 5.7: `/mnt/epic-games/UE_5.7/`

---

## Findings

### 1. FVersionedNiagaraEmitterData — Script Props

**Question:** Do `EmitterSpawnScriptProps`, `EmitterUpdateScriptProps`, `SpawnScriptProps`, `UpdateScriptProps` still exist?

**5.5 state (confirmed from header, lines 356–459):**
```cpp
// Runtime (non-editor) — always present:
UPROPERTY() FNiagaraEmitterScriptProperties UpdateScriptProps;
UPROPERTY() FNiagaraEmitterScriptProperties SpawnScriptProps;

// Editor-only (#if WITH_EDITORONLY_DATA):
UPROPERTY() FNiagaraEmitterScriptProperties EmitterSpawnScriptProps;
UPROPERTY() FNiagaraEmitterScriptProperties EmitterUpdateScriptProps;
```

**5.6 verdict: NO CHANGE.** All four fields have identical declarations at the same positions in the struct.
Source: `/mnt/epic-games/UE_5.6/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitter.h` lines 359–462.

**5.7 verdict: NO CHANGE.** All four fields remain identical.
Source: `/mnt/epic-games/UE_5.7/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitter.h` lines 360–466.

**Impact on our code:** None. `EmitterSpawnScriptProps.Script`, `EmitterUpdateScriptProps.Script`, `SpawnScriptProps.Script`, and `UpdateScriptProps.Script` are safe across all three versions.

---

### 2. FNiagaraEmitterHandle::GetEmitterData()

**5.5 signature:**
```cpp
NIAGARA_API FVersionedNiagaraEmitterData* GetEmitterData() const;
```

**5.6 verdict: IDENTICAL.** Same signature, same location in `NiagaraEmitterHandle.h` line 84.

**5.7 verdict: IDENTICAL.** Same signature at line 86.
Source: All three `/mnt/epic-games/UE_5.x/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitterHandle.h`

**Impact on our code:** None. `Handles[i].GetEmitterData()` is safe across all three versions.

---

### 3. UNiagaraSystem::AddEmitterHandle()

**5.5 signature:**
```cpp
NIAGARA_API FNiagaraEmitterHandle AddEmitterHandle(UNiagaraEmitter& SourceEmitter, FName EmitterName, FGuid EmitterVersion);
```
Source: `NiagaraSystem.h` line 308.

**5.6 verdict: IDENTICAL.** Same signature at line 328.

**5.7 verdict: IDENTICAL.** Same signature at line 328.

**Impact on our code:** None. Our `System->AddEmitterHandle(*EmptyEmitter, FName(*FinalEmitterName), FGuid::NewGuid())` call (OliveNiagaraWriter.cpp line 437) is correct across all three versions. Note: this method is `#if WITH_EDITORONLY_DATA` in all versions — correct for an editor plugin.

---

### 4. UNiagaraSystem::GetSystemSpawnScript() / GetSystemUpdateScript()

**5.5 signatures:**
```cpp
NIAGARA_API UNiagaraScript* GetSystemSpawnScript();
NIAGARA_API UNiagaraScript* GetSystemUpdateScript();
NIAGARA_API const UNiagaraScript* GetSystemSpawnScript() const;
NIAGARA_API const UNiagaraScript* GetSystemUpdateScript() const;
```

**5.6 verdict: IDENTICAL.** Lines 368–371.

**5.7 verdict: IDENTICAL.** Lines 368–371.

**Impact on our code:** None. All call sites in OliveNiagaraWriter.cpp (lines 581, 585, 749, 753, 1301, 1303) are safe.

---

### 5. UNiagaraScriptSource::NodeGraph

**5.5 declaration:**
```cpp
UPROPERTY()
TObjectPtr<class UNiagaraGraph> NodeGraph = nullptr;
```
Source: `NiagaraScriptSource.h` line 25.

**5.6 verdict: IDENTICAL.** Same declaration at line 25.

**5.7 verdict: IDENTICAL.** Same declaration at line 25.

**Impact on our code:** None. `Source->NodeGraph` accesses (OliveNiagaraWriter.cpp lines 190, 198, 230, 236) are safe.

---

### 6. UNiagaraNodeFunctionCall

**5.5, 5.6, 5.7:** The class is identical in all three versions. Key public members remain unchanged:
- `TObjectPtr<UNiagaraScript> FunctionScript`
- `FNiagaraFunctionSignature Signature`
- `TMap<FName, FName> FunctionSpecifiers`
- `GetCalledGraph()`, `GetCalledUsage()`, `GetFunctionScriptSource()`, `GetScriptData()`

**Impact on our code:** None.

---

### 7. FNiagaraStackGraphUtilities::AddScriptModuleToStack()

**5.5 public signature:**
```cpp
NIAGARAEDITOR_API UNiagaraNodeFunctionCall* AddScriptModuleToStack(
    UNiagaraScript* ModuleScript,
    UNiagaraNodeOutput& TargetOutputNode,
    int32 TargetIndex = INDEX_NONE,
    FString SuggestedName = FString(),
    const FGuid& VersionGuid = FGuid());
```

**5.6 verdict: IDENTICAL.** Line 278 in NiagaraStackGraphUtilities.h — same signature.

**5.7 verdict: IDENTICAL.** Line 288 — same signature.

There is also a new `FAddScriptModuleToStackArgs` struct overload introduced in 5.5 and carried forward unchanged. The struct-based overload is non-`_API` (internal); the `UNiagaraScript*` overload with `NIAGARAEDITOR_API` is the stable public one we use.

`RemoveModuleFromStack` overloads are also unchanged across all three versions.

**Impact on our code:** None.

---

### 8. UNiagaraNodeOutput::GetUsage()

**5.5 declaration:**
```cpp
ENiagaraScriptUsage GetUsage() const { return ScriptType; }
```

**5.6 verdict: IDENTICAL.**

**5.7 verdict: IDENTICAL.**

`ScriptType` (UPROPERTY), `ScriptTypeId` (UPROPERTY), `GetUsageId()`, and `SetUsageId()` are unchanged in all versions.

**Impact on our code:** None.

---

### 9. UNiagaraEmitter — GetUniqueEmitterName() and class hierarchy

This is the **most significant architectural change** found.

**5.5 and 5.6:**
```cpp
class UNiagaraEmitter : public UObject, public INiagaraParameterDefinitionsSubscriber, ...
// GetUniqueEmitterName() is declared on UNiagaraEmitter directly:
NIAGARA_API const FString& GetUniqueEmitterName() const;  // line 930 (5.5), line 921 (5.6)
```

**5.7: NEW BASE CLASS `UNiagaraEmitterBase`**
```cpp
// New file: NiagaraEmitterBase.h
UCLASS(MinimalAPI)
class UNiagaraEmitterBase : public UObject
{
public:
    NIAGARA_API const FString& GetUniqueEmitterName() const;  // moved here
    NIAGARA_API bool SetUniqueEmitterName(const FString& InName);
    // ...
protected:
    UPROPERTY() FString UniqueEmitterName;  // storage also moved here
};

// UNiagaraEmitter now derives from UNiagaraEmitterBase:
class UNiagaraEmitter : public UNiagaraEmitterBase, public INiagaraParameterDefinitionsSubscriber, ...
```

**Source:** `/mnt/epic-games/UE_5.7/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitterBase.h`

The method signature is **unchanged** — `GetUniqueEmitterName()` still returns `const FString&`. But:

1. The method is now on `UNiagaraEmitterBase`, not directly on `UNiagaraEmitter`.
2. `UNiagaraEmitter` inherits it, so calls via `UNiagaraEmitter*` still compile with no change.
3. `UNiagaraStatelessEmitter` also inherits from `UNiagaraEmitterBase` in 5.7.

**Impact on our code:** Our usage at `OliveNiagaraWriter.cpp:1359` calls `Emitter->GetUniqueEmitterName()` where `Emitter` is `UNiagaraEmitter*`. This compiles correctly in 5.7 because `UNiagaraEmitter` inherits the method from `UNiagaraEmitterBase`. **No source change required.**

However, if we ever add `#include "NiagaraEmitterBase.h"` for a new feature, note that it resides in a new header that doesn't exist in 5.5/5.6.

---

### 10. FNiagaraEmitterHandle::GetInstance() — Signature Change in 5.7

This is a **breaking change** for non-const usage.

**5.5 and 5.6:**
```cpp
NIAGARA_API FVersionedNiagaraEmitter GetInstance() const;   // returns by value
NIAGARA_API FVersionedNiagaraEmitter& GetInstance();         // non-const ref overload
```

**5.7:**
```cpp
[[nodiscard]] NIAGARA_API const FVersionedNiagaraEmitter GetInstance() const;  // const only, by value
NIAGARA_API void SetInstance(const FVersionedNiagaraEmitter& VersionedData);   // separate setter
NIAGARA_API void SetInstanceVersion(const FGuid& InVersion);                   // helper
```

The mutable `GetInstance()` reference overload **is removed in 5.7**. The non-const overload is replaced by `SetInstance()` / `SetInstanceVersion()`.

**Impact on our code:** `OliveNiagaraWriter.cpp:989` and `1349` both call:
```cpp
UNiagaraEmitter* Emitter = Handles[EmitterIndex].GetInstance().Emitter;
```
This uses the `const` overload (calling `.Emitter` on the returned value copy), which compiles correctly in all three versions — the `const` overload returning `FVersionedNiagaraEmitter` by value exists in 5.5, 5.6, and 5.7.

**No immediate breakage**, but any code that tries to mutate via `Handle.GetInstance().Emitter = ...` or similar will fail to compile in 5.7. Our code does not do this. Flag for review during a 5.7 port.

Additionally in 5.7, a new method `GetEmitterBase()` returns `UNiagaraEmitterBase*`:
```cpp
NIAGARA_API UNiagaraEmitterBase* GetEmitterBase() const;
```
This is the 5.7-forward way to get a pointer to the emitter when you don't need the full `UNiagaraEmitter` type.

---

### 11. bInterpolatedSpawning — Property Renamed in 5.6

**5.5 on `FVersionedNiagaraEmitterData`:**
```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Emitter")
uint32 bInterpolatedSpawning : 1;
```

**5.6 and 5.7:**
```cpp
// Primary property renamed to an enum:
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Emitter", meta = (SegmentedDisplay))
ENiagaraInterpolatedSpawnMode InterpolatedSpawnMode;

// Old bool preserved as deprecated:
UPROPERTY(meta = (DeprecatedProperty))
uint32 bInterpolatedSpawning_DEPRECATED : 1;
```

**Impact on our code:** Our phase 2 memory note documents `bInterpolatedSpawning` as a property we set directly. If our writer ever touches this field, it must use `InterpolatedSpawnMode` in 5.6+. Search OliveNiagaraWriter.cpp shows no current usage of this field — not yet implemented, so **no immediate impact**, but the planned implementation needs to use the enum form.

---

### 12. General Architecture Changes — 5.6 / 5.7

**5.6 changes (confirmed from headers):**
- `bInterpolatedSpawning` → `ENiagaraInterpolatedSpawnMode InterpolatedSpawnMode` (breaking if used directly)
- `UNiagaraEmitter::GetScript()` gained `const` qualifier on the signature (was non-const in 5.5, is now `const` in 5.6): `NIAGARA_API UNiagaraScript* GetScript(ENiagaraScriptUsage Usage, FGuid UsageId) const;` — minor ABI change, benign for callers
- `FORCEINLINE` replaced with `inline` on several inlined methods (`GetScalabilitySettings`, `GetEventHandlers`, `GetDefaultFixedBounds`) — no impact on callers
- Asset tag system: Niagara systems/emitters that use asset tags need re-save on first load (asset pipeline concern, not plugin C++ concern)
- Lightweight emitter "Emitter Mode" UI changed (removed right-click menu option) — no C++ API impact

**5.7 changes (confirmed from headers):**
- `UNiagaraEmitterBase` introduced as new base class for `UNiagaraEmitter` and `UNiagaraStatelessEmitter`
- `GetInstance()` mutable ref overload removed — replaced by `SetInstance()` / `SetInstanceVersion()`
- New `GetEmitterBase()` method on `FNiagaraEmitterHandle` returning `UNiagaraEmitterBase*`
- New `FVersionedNiagaraEmitterBase` / `FVersionedNiagaraEmitterBaseWeakPtr` structs in new `NiagaraEmitterBase.h` header
- `ForEachEnabledRendererWithIndex()` added to `FVersionedNiagaraEmitterData` (5.7 only)
- `FOnRenderersChanged` delegate moved to `UNiagaraEmitterBase`; `FOnPropertiesChanged` removed from `UNiagaraEmitter` in 5.7 (still present in 5.6)
- Niagara Data Channels: new Access Contexts feature (no impact on our emitter creation code)
- Asset tag system: new Tagged Asset Browser Configuration assets

---

## Summary Table

| API | 5.5 | 5.6 | 5.7 | Our Code Risk |
|-----|-----|-----|-----|--------------|
| `FVersionedNiagaraEmitterData::EmitterSpawnScriptProps` | Present | Present | Present | None |
| `FVersionedNiagaraEmitterData::EmitterUpdateScriptProps` | Present | Present | Present | None |
| `FVersionedNiagaraEmitterData::SpawnScriptProps` | Present | Present | Present | None |
| `FVersionedNiagaraEmitterData::UpdateScriptProps` | Present | Present | Present | None |
| `FNiagaraEmitterHandle::GetEmitterData()` | Present | Present | Present | None |
| `UNiagaraSystem::AddEmitterHandle(UNiagaraEmitter&, FName, FGuid)` | Present | Present | Present | None |
| `UNiagaraSystem::GetSystemSpawnScript()` | Present | Present | Present | None |
| `UNiagaraSystem::GetSystemUpdateScript()` | Present | Present | Present | None |
| `UNiagaraScriptSource::NodeGraph` | Public UPROPERTY | Same | Same | None |
| `UNiagaraNodeFunctionCall` | Stable | Same | Same | None |
| `FNiagaraStackGraphUtilities::AddScriptModuleToStack(UNiagaraScript*,...)` | Present | Present | Present | None |
| `UNiagaraNodeOutput::GetUsage()` | Present | Present | Present | None |
| `UNiagaraEmitter::GetUniqueEmitterName()` | On UNiagaraEmitter | On UNiagaraEmitter | Moved to UNiagaraEmitterBase (inherited) | None — inherits |
| `FNiagaraEmitterHandle::GetInstance()` const-by-value | Present | Present | Present | None |
| `FNiagaraEmitterHandle::GetInstance()` non-const ref | Present | Present | **REMOVED** | None currently — we don't mutate |
| `FVersionedNiagaraEmitterData::bInterpolatedSpawning` | Present | **DEPRECATED** → replaced by enum | Enum only | Only if we add it |

---

## Recommendations

1. **For a 5.6 port: no changes required to existing OliveNiagaraWriter.cpp.** All APIs we use are stable. The one breaking change in 5.6 (`bInterpolatedSpawning` → `ENiagaraInterpolatedSpawnMode`) affects a property we haven't implemented yet.

2. **For a 5.7 port: one potential compile error.** The non-const `GetInstance()` ref overload is removed. Verify that `Handles[EmitterIndex].GetInstance().Emitter` at lines 989 and 1349 compiles — it should (uses the const overload) but confirm under 5.7 headers. No other write sites found.

3. **bInterpolatedSpawning implementation (future).** When implementing interpolated spawning control, use `InterpolatedSpawnMode` (type: `ENiagaraInterpolatedSpawnMode`) in 5.6+, not the deprecated `bInterpolatedSpawning`. For 5.5 compat: guard with version check or use `GetScript()->bInterpolatedSpawning` directly if targeting 5.5 only.

4. **UNiagaraEmitterBase in 5.7 (new header).** If future code needs `UNiagaraEmitterBase` directly (e.g., to handle stateless emitters), include `NiagaraEmitterBase.h` which only exists in 5.7+. Guard with `#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 7`.

5. **GetInstance() mutation pattern.** In 5.7, any code trying to assign to `Handle.GetInstance().Emitter` won't compile. Use `Handle.SetInstance(FVersionedNiagaraEmitter{...})` instead. Our current code only reads `.Emitter` from the const overload — safe as-is.

6. **UNiagaraEmitter::GetScript() const change.** The 5.5 signature was non-const; 5.6+ is `const`. If we ever store a non-const reference to call `GetScript()`, this is safe — the `const` version works fine for reading. Not a breaking change for callers.

7. **The plugin's Niagara code is effectively 5.6-and-5.7-compatible today** for all implemented API surface, assuming the one open question on `GetInstance()` at lines 989/1349 compiles with 5.7 headers (expected yes, since we use the const overload).

Sources:
- `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitter.h`
- `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitterHandle.h`
- `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraSystem.h`
- `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraScriptSource.h`
- `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraNodeFunctionCall.h`
- `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Public/NiagaraNodeOutput.h`
- `/mnt/epic-games/UE_5.5/Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Public/ViewModels/Stack/NiagaraStackGraphUtilities.h`
- (All above also compared at UE_5.6 and UE_5.7 paths)
- `/mnt/epic-games/UE_5.7/Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitterBase.h` (new in 5.7)
