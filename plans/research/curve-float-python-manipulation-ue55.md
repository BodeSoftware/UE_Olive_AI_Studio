# Research: UCurveFloat / FRichCurve Key Manipulation — Python and C++ APIs (UE 5.5)

## Question

Can UE5's Python API manipulate `CurveFloat` keys (add/remove/modify)? What alternatives exist? Should we build a C++ MCP tool instead?

---

## Findings

### 1. What Python CAN Do with UCurveFloat

**Python-accessible methods on `unreal.CurveFloat`:**
- `get_float_value(time: float) -> float` — evaluate the curve at a time (BlueprintCallable, UFUNCTION)

**Python-accessible methods on `unreal.CurveBase` (parent):**
- `get_time_range() -> (min_time, max_time)` — BlueprintCallable
- `get_value_range() -> (min_value, max_value)` — BlueprintCallable

**Python-readable struct properties on `unreal.RichCurve`:**
- `default_value` (float) — read/write
- `keys` (Array[RichCurveKey]) — accessible but NOT appendable (see below)

**Python-accessible struct `unreal.RichCurveKey`:**

`FRichCurveKey` is `USTRUCT(BlueprintType)` — fully Python-accessible. Properties:
- `time` (float) — read/write
- `value` (float) — read/write
- `interp_mode` (RichCurveInterpMode) — read/write
- `tangent_mode` (RichCurveTangentMode) — read/write
- `tangent_weight_mode` (RichCurveTangentWeightMode) — read/write

Source: `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/RichCurve.h`, line 78

---

### 2. What Python CANNOT Do — and Why

**`FRichCurve::AddKey()` is NOT callable from Python.**

`FRichCurve` is declared `USTRUCT()` (no `BlueprintType`), and `AddKey()` / `DeleteKey()` / `UpdateOrAddKey()` / `SetKeyValue()` are `ENGINE_API` virtual C++ methods — NOT decorated with `UFUNCTION`. Python only binds `UFUNCTION`-decorated methods. There is no Python `add_key()`, `delete_key()`, or `update_or_add_key()` on `unreal.RichCurve`.

Source: `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/RichCurve.h`, lines 196–353

**`FRichCurve.Keys` array cannot be appended from Python.**

The `Keys` property is declared:
```cpp
UPROPERTY(EditAnywhere, EditFixedSize, Category="Curve", meta=(EditFixedOrder))
TArray<FRichCurveKey> Keys;
```
`EditFixedSize` means the array size is locked — existing slots can have their values modified, but Python cannot add or remove elements. `set_editor_property("keys", [key1, key2])` with a different-length array will be silently ignored or fail.

Source: `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/RichCurve.h`, line 351

**`get_editor_property("float_curve")` returns a view, not a mutable object.**

`UCurveFloat.FloatCurve` is `UPROPERTY()` with no specifiers (no EditAnywhere, no BlueprintReadWrite). Python can read it via `get_editor_property("float_curve")`, but setting a replaced `FRichCurve` struct back via `set_editor_property` is not reliably supported because `FRichCurve` is not `USTRUCT(BlueprintType)`.

**`CreateCurveFromCSVString()` and `ImportFromJSONString()` are NOT callable from Python.**

Both are `ENGINE_API` methods on `UCurveBase` with no `UFUNCTION` decorator. Python does not bind them.

Source: `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/CurveBase.h`, lines 38–56

---

### 3. Confirmed Working Python Workarounds

#### Approach A: CSV Import via `AssetImportTask` + `CSVImportFactory` (CONFIRMED WORKING)

A community forum user confirmed this works for `UCurveLinearColor` and the pattern applies equally to `UCurveFloat`:

```python
import unreal, csv, os, tempfile

def set_curve_float_keys(asset_path, keys):
    # keys = list of (time, value) tuples
    # Write temp CSV
    with tempfile.NamedTemporaryFile(mode='w', suffix='.csv', delete=False, newline='') as f:
        writer = csv.writer(f)
        for (t, v) in keys:
            writer.writerow([t, v])
        temp_path = f.name

    task = unreal.AssetImportTask()
    task.filename = temp_path
    task.destination_path = os.path.dirname(asset_path)
    task.destination_name = os.path.basename(asset_path)
    task.replace_existing = True
    task.automated = True
    task.save = True

    factory = unreal.CSVImportFactory()
    settings = unreal.CSVImportSettings()
    settings.import_type = unreal.ECSVImportType.ECSV_CURVE_FLOAT
    factory.automated_import_settings = settings
    task.factory = factory

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    os.unlink(temp_path)
```

This calls `UCurveBase::CreateCurveFromCSVString()` internally, which calls `FRichCurve::AddKey()` and sets `RCIM_Linear` interp mode.

Source: Forum thread — [No way! Procedurally generated curve?!](https://forums.unrealengine.com/t/no-way-procedurally-generated-curve/2339539)

**Limitations of Approach A:**
- Requires writing a temp file to disk
- Always uses `RCIM_Linear` interp mode (hardcoded in `CreateCurveFromCSVString`)
- Replaces the ENTIRE curve (no additive key insertion)
- Only works in editor context

#### Approach B: `AnimationLibrary.add_float_curve_keys()` (AnimSequence ONLY)

```python
unreal.AnimationLibrary.add_float_curve_keys(anim_sequence, curve_name, times_array, values_array)
```

This is `BlueprintCallable` and Python-accessible, but it operates on **animation curves inside an AnimSequence**, NOT on standalone `UCurveFloat` assets. Not useful for Timeline tracks.

Source: [UE Forum thread on anim curve key speed](https://forums.unrealengine.com/t/ue5-0-python-api-add-any-keys-to-float-curves-of-anim-sequence-is-very-slowly/638740)

---

### 4. C++ API — What Exists and Works

**`FRichCurve::AddKey()`** — The primary method. Already used in Olive:

```cpp
// From OliveBlueprintToolHandlers.cpp line 7086:
FKeyHandle H = NewTrack.CurveFloat->FloatCurve.AddKey(
    static_cast<float>(Key[0]),   // time
    static_cast<float>(Key[1])    // value
);
NewTrack.CurveFloat->FloatCurve.SetKeyInterpMode(H, ParsedTrack.InterpMode);
```

Full C++ API surface on `FRichCurve` (all `ENGINE_API`):
- `AddKey(float time, float value, bool bUnwindRotation, FKeyHandle)` → `FKeyHandle`
- `DeleteKey(FKeyHandle)`
- `UpdateOrAddKey(float time, float value)` → `FKeyHandle`
- `SetKeyTime(FKeyHandle, float)`
- `SetKeyValue(FKeyHandle, float, bool bAutoSetTangents)`
- `GetKeyValue(FKeyHandle)` → `float`
- `SetKeyInterpMode(FKeyHandle, ERichCurveInterpMode)`
- `SetKeyTangentMode(FKeyHandle, ERichCurveTangentMode)`
- `GetCopyOfKeys()` → `TArray<FRichCurveKey>`
- `Reset()` — clears all keys
- `SetKeys(const TArray<FRichCurveKey>&)` — bulk set (must be pre-sorted by time)

Source: `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/RichCurve.h`, lines 205–331

**Correct pattern for a standalone `UCurveFloat` asset mutation from C++ MCP tool:**

```cpp
// 1. Load or create asset
UCurveFloat* CurveAsset = LoadObject<UCurveFloat>(nullptr, *AssetPath);
if (!CurveAsset) { /* create via FAssetToolsModule */ }

// 2. Undo support
FScopedTransaction Transaction(NSLOCTEXT("Olive", "SetCurveKeys", "Set Curve Float Keys"));
CurveAsset->Modify();

// 3. Mutate
CurveAsset->FloatCurve.Reset();
for (const FKeySample& K : Keys)
{
    FKeyHandle H = CurveAsset->FloatCurve.AddKey(K.Time, K.Value);
    CurveAsset->FloatCurve.SetKeyInterpMode(H, K.InterpMode);
}

// 4. Notify editor
CurveAsset->PostEditChange();
CurveAsset->MarkPackageDirty();
```

`UCurveBase` has `PostEditChangeProperty()` and `FOnUpdateCurve OnUpdateCurve` delegate — both fire automatically via `PostEditChange()`.

Source: `/mnt/epic-games/UE_5.5/Engine/Source/Runtime/Engine/Classes/Curves/CurveBase.h`

---

### 5. Python via `editor.run_python` — Practical Assessment

The Olive plugin's `editor.run_python` tool can execute the CSV approach (Approach A above). For the AI agent, this means:

```python
# Agent can do this from editor.run_python:
import unreal, tempfile, csv, os

def create_float_curve(asset_path, keys):
    # keys = list of [time, value] pairs
    tmp = tempfile.NamedTemporaryFile(mode='w', suffix='.csv', delete=False)
    writer = csv.writer(tmp)
    [writer.writerow(k) for k in keys]
    tmp.close()

    task = unreal.AssetImportTask()
    task.filename = tmp.name
    task.destination_path = '/'.join(asset_path.split('/')[:-1])
    task.destination_name = asset_path.split('/')[-1]
    task.replace_existing = True
    task.automated = True
    factory = unreal.CSVImportFactory()
    s = unreal.CSVImportSettings()
    s.import_type = unreal.ECSVImportType.ECSV_CURVE_FLOAT
    factory.automated_import_settings = s
    task.factory = factory
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    os.unlink(tmp.name)
```

**Limitation:** Interp mode is always Linear (hardcoded in `CreateCurveFromCSVString()`). No support for cubic/constant without a C++ tool.

---

### 6. C++ MCP Tool Design Sketch

A dedicated `curve.set_keys` tool would be the correct long-term solution:

- **Tool name:** `curve.set_keys`
- **Input:**
  ```json
  {
    "path": "/Game/Curves/MyCurve",
    "keys": [
      {"time": 0.0, "value": 0.0, "interp_mode": "linear"},
      {"time": 1.0, "value": 100.0, "interp_mode": "cubic"}
    ],
    "replace": true
  }
  ```
- **C++ implementation:** Follows the write pipeline pattern. Loads `UCurveFloat`, `Modify()`, `Reset()` (if replace=true), loops `AddKey()` + `SetKeyInterpMode()`, calls `PostEditChange()` and `MarkPackageDirty()`.
- **Module dependencies:** Already included in `OliveAIEditor.Build.cs`: `Engine` (for UCurveFloat/FRichCurve), `UnrealEd` (for FScopedTransaction).

---

## Summary Table

| Approach | Additive Keys | Interp Mode | Works from Python | Works from C++ | Notes |
|---|---|---|---|---|---|
| `add_key()` on `unreal.RichCurve` | N/A | N/A | NO | N/A | No UFUNCTION binding |
| `set_editor_property("keys", [...])` | NO | Any | Blocked by EditFixedSize | N/A | Array size locked |
| `CreateCurveFromCSVString()` | NO (replaces all) | Linear only | NO (no UFUNCTION) | YES | |
| `ImportFromJSONString()` | NO (replaces all) | Full | NO (no UFUNCTION) | YES | |
| `CSVImportFactory` via `AssetImportTask` | NO (replaces all) | Linear only | YES (temp file) | YES | Confirmed working |
| `FRichCurve::AddKey()` direct | YES | Full | NO | YES | The right approach for C++ tool |
| `AnimationLibrary.add_float_curve_keys()` | YES | Partial | YES | YES | AnimSequence ONLY |

---

## Recommendations

1. **Python cannot directly call `AddKey()`** — this is a hard limitation. `FRichCurve::AddKey()` has no UFUNCTION binding and `FRichCurve.Keys` is `EditFixedSize` so array mutation is blocked at the Python layer.

2. **The CSV import workaround works from Python** via `editor.run_python`, but always produces linear interp mode and requires a temp file. This is viable for simple cases where the agent needs to create a curve asset with linear segments.

3. **A custom `curve.set_keys` C++ MCP tool is the correct full solution.** The Olive plugin already has `UCurveFloat.h` included and uses `FRichCurve::AddKey()` for Timeline tracks (lines 7082–7091 of `OliveBlueprintToolHandlers.cpp`). Reusing that pattern for a standalone curve asset tool is low-risk.

4. **`AnimationLibrary.add_float_curve_keys()` is a red herring** for Timeline purposes — it only works on `AnimSequence` assets, not standalone `UCurveFloat`.

5. **For the short term (no new C++ tool):** The agent can use `editor.run_python` with the CSV import pattern. This covers the basic use case. Document the linear-interp-only limitation in the knowledge prompt.

6. **The `curve.set_keys` C++ tool** should support: `replace` flag (reset + repopulate vs additive), per-key `interp_mode` (linear/constant/cubic), per-key `tangent_mode`, and optionally `time_range`/`default_value`. It should also handle asset creation if the asset doesn't exist yet.

7. **No `editor.run_python` path can set cubic tangents** — only C++ gives access to `SetKeyTangentMode()` and `SetKeyTangentWeightMode()`. If Timeline curve tracks need smooth cubic interpolation, the existing C++ Timeline tool path is correct and should be used.
