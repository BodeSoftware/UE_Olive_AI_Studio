# Widget & Rate Limit Fixes Design

**Date:** 2026-03-12
**Status:** Ready for implementation
**Scope:** 3 surgical fixes, no new files, no refactors

---

## Issue 1: ReceiveConstruct event not found on UserWidget (BLOCKING)

### Root Cause

The `EventNameMap` in `OliveBlueprintPlanResolver.cpp` (line 2619-2629) maps Widget Blueprint event names to `Receive*` prefixed names that **do not exist**.

```
{ TEXT("Construct"),        TEXT("ReceiveConstruct") },    // WRONG
{ TEXT("Destruct"),         TEXT("ReceiveDestruct") },     // WRONG
{ TEXT("PreConstruct"),     TEXT("ReceivePreConstruct") }, // WRONG
```

The `Receive*` prefix convention applies ONLY to `AActor` events where the C++ class declares the UFUNCTION with that prefix (e.g., `AActor::ReceiveBeginPlay` is the actual function name declared with `BlueprintImplementableEvent`). On `UUserWidget`, the actual UFUNCTION names declared with `BlueprintImplementableEvent` are:

- `Construct()` (line 523 of `UserWidget.h`)
- `Destruct()` (line 530)
- `PreConstruct(bool IsDesignTime)` (line 515)
- `OnInitialized()` (line 500, has `OnInitialized` as function name)

There is NO `ReceiveConstruct`, `ReceiveDestruct`, or `ReceivePreConstruct` anywhere in the UMG module (confirmed via grep).

The `CreateEventNode` flow at `OliveNodeFactory.cpp` line 445 does `Blueprint->ParentClass->FindFunctionByName(EventName)` which correctly searches the C++ class hierarchy -- but since the resolver already changed `Construct` to `ReceiveConstruct`, the search fails. It then falls through to interface check, SCS delegate check, and finally the Enhanced Input Action check (which tries to find `IA_ReceiveConstruct` and fails).

### Fix

**File:** `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp`
**Lines:** 2619-2629

Change the Widget Blueprint event mappings to use the correct UFunction names:

```cpp
// Widget Blueprint events (UUserWidget overridable events)
// NOTE: Unlike AActor events, UUserWidget uses the BARE function name
// as the UFUNCTION name (Construct, not ReceiveConstruct).
{ TEXT("Construct"),                TEXT("Construct") },
{ TEXT("Destruct"),                 TEXT("Destruct") },
{ TEXT("PreConstruct"),             TEXT("PreConstruct") },
{ TEXT("EventConstruct"),           TEXT("Construct") },
{ TEXT("EventDestruct"),            TEXT("Destruct") },
{ TEXT("EventPreConstruct"),        TEXT("PreConstruct") },
{ TEXT("OnInitialized"),            TEXT("OnInitialized") },
// Pass-throughs (AI sometimes uses internal names directly)
// Remove the ReceiveConstruct/ReceiveDestruct/ReceivePreConstruct
// pass-throughs since those function names don't exist.
// Instead, keep the correct names as pass-throughs:
{ TEXT("Construct"),                TEXT("Construct") },  // already covered above, but harmless
```

Wait -- the map already has `Construct` mapping. The fix is simply to change the target values. Also remove the `Receive*` pass-throughs since those names are invalid, and add a safety fallback: if an AI sends `ReceiveConstruct`, map it to `Construct`.

Concrete changes to the `EventNameMap`:

**Replace lines 2619-2629:**
```cpp
// Widget Blueprint events (UUserWidget overridable events)
// NOTE: Unlike AActor (which uses ReceiveBeginPlay etc.), UUserWidget
// declares its BlueprintImplementableEvent UFunctions with bare names:
// Construct(), Destruct(), PreConstruct(bool).
{ TEXT("Construct"),                TEXT("Construct") },
{ TEXT("Destruct"),                 TEXT("Destruct") },
{ TEXT("PreConstruct"),             TEXT("PreConstruct") },
{ TEXT("OnInitialized"),            TEXT("OnInitialized") },
{ TEXT("EventConstruct"),           TEXT("Construct") },
{ TEXT("EventDestruct"),            TEXT("Destruct") },
{ TEXT("EventPreConstruct"),        TEXT("PreConstruct") },
// Safety: if AI sends the (incorrect) Receive-prefixed names, fix them
{ TEXT("ReceiveConstruct"),         TEXT("Construct") },
{ TEXT("ReceiveDestruct"),          TEXT("Destruct") },
{ TEXT("ReceivePreConstruct"),      TEXT("PreConstruct") },
```

**Also add** `OnInitialized` as a common alias:
```cpp
{ TEXT("Initialized"),              TEXT("OnInitialized") },
```

### Verification

After this fix, `CreateEventNode` for a Widget Blueprint with target `Construct` will:
1. Resolver maps `Construct` -> `Construct` (or AI sends `ReceiveConstruct` -> `Construct`)
2. `FindFunctionByName(FName("Construct"))` on `UUserWidget` succeeds
3. `SetExternalMember(FName("Construct"), UUserWidget::StaticClass())` creates valid event reference
4. `AllocateDefaultPins()` creates the event pins

### Risk: LOW
Single map change, no logic changes, no new code paths.

---

## Issue 2: WriteRateLimit blocking Tier 1 operations in autonomous mode

### Root Cause

`FOliveWriteRateLimitRule` (in `OliveValidationEngine.h/.cpp`) applies a single rate limit of `MaxWriteOpsPerMinute = 30` to ALL write operations indiscriminately. During autonomous runs, the AI legitimately fires many rapid structural setup calls (add_variable, add_component, add_function) that are all Tier 1 (auto-execute, low risk). These hit the 30/minute ceiling within 40 seconds, causing 9 wasted tool calls in the observed run.

The rate limiter was designed to prevent runaway AI loops, not to throttle legitimate autonomous work. 30 ops/minute is too restrictive for autonomous mode where 15-20 structural calls in 30 seconds is normal behavior.

### Fix

**The simplest correct approach: raise the default from 30 to 120.**

**File:** `Source/OliveAIEditor/Public/Settings/OliveAISettings.h`
**Line:** 329

```cpp
// Before:
int32 MaxWriteOpsPerMinute = 30;

// After:
int32 MaxWriteOpsPerMinute = 120;
```

The `ClampMax` is already 120, so this just changes the default to the maximum allowed value. This is sufficient because:

1. **120/minute (2/second) is still protective** against infinite loops -- a true runaway loop would hit 120 within 60 seconds and get blocked.
2. **No code path changes** -- the existing validation rule, sliding window, and retry-after calculation all work identically.
3. **No mode detection needed** -- we don't need to distinguish autonomous vs. interactive mode. Even interactive mode benefits from a higher limit when the AI is doing rapid setup.
4. **Users can still tighten it** -- the setting is exposed in Project Settings with ClampMin=0, so users who want stricter limits can set it.

### Why not exempt Tier 1?

Considered but rejected:
- The validation rule interface (`IOliveValidationRule::Validate`) receives `(ToolName, Params, TargetAsset)` -- no tier information is available.
- Passing tier info would require changing the `IOliveValidationRule` interface, which touches all 8+ rule implementations.
- The conceptual distinction is weak -- even Tier 2/3 operations shouldn't be rate-limited at 30/minute during normal use.

### Why not a separate autonomous limit?

Considered but rejected:
- Would require detecting autonomous mode in the validation rule (coupling to `UOliveAISettings::bUseAutonomousMCPMode`).
- Adds a new settings field the user must understand.
- The core issue is that 30 is just too low for any workflow, not that autonomous mode is special.

### Risk: LOW
One-line default change. Existing users who customized the setting keep their value (it's `Config` stored in ini).

---

## Issue 3: SetPercent not found on WidgetBlueprint children

### Root Cause

`FindFunction` in `OliveNodeFactory.cpp` searches a hardcoded list of "library classes" (Step 5, line 2476-2489) that includes commonly-used UE classes. When a plan_json step calls `SetPercent` without specifying `target_class`, the search walks:

1. Alias map -- no match
2. Blueprint class (`UserWidget` GeneratedClass) -- no match
3. Parent hierarchy (`UUserWidget` -> `UWidget` -> ...) -- no match
4. SCS components -- Widget Blueprints don't have SCS
5. Interfaces -- no match
6. Library classes -- `SetPercent` is not on any of the 12 hardcoded classes
7. Universal `UBlueprintFunctionLibrary` scan -- `UProgressBar` is not a function library
8. K2_ fuzzy match -- no match

The library classes list already includes non-library classes like `AActor`, `APawn`, `ACharacter`, `USceneComponent`, `UPrimitiveComponent`, `UUserWidget` -- these are "common classes the AI might call functions on." UMG widget classes are conspicuously absent.

### Fix

**Add key UMG widget classes to the `LibraryClasses` list.**

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`
**Lines:** 2476-2489

Add after `UUserWidget::StaticClass()`:

```cpp
TArray<UClass*> LibraryClasses = {
    UKismetSystemLibrary::StaticClass(),
    UKismetMathLibrary::StaticClass(),
    UKismetStringLibrary::StaticClass(),
    UKismetArrayLibrary::StaticClass(),
    UGameplayStatics::StaticClass(),
    UObject::StaticClass(),
    AActor::StaticClass(),
    USceneComponent::StaticClass(),
    UPrimitiveComponent::StaticClass(),
    APawn::StaticClass(),
    ACharacter::StaticClass(),
    UUserWidget::StaticClass(),
    // UMG widget classes -- common children in Widget Blueprints
    UProgressBar::StaticClass(),
    UTextBlock::StaticClass(),
    UImage::StaticClass(),
    UButton::StaticClass(),
    USlider::StaticClass(),
    UCheckBox::StaticClass(),
    UEditableTextBox::StaticClass(),
    UComboBoxString::StaticClass(),
    UWidgetSwitcher::StaticClass(),
};
```

**New includes needed** (add near the existing `#include "Blueprint/UserWidget.h"` at line 63):

```cpp
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "Components/ComboBoxString.h"
#include "Components/WidgetSwitcher.h"
```

### UMG classes chosen and rationale

| Class | Key functions AI would call |
|-------|---------------------------|
| `UProgressBar` | `SetPercent`, `SetFillColorAndOpacity` |
| `UTextBlock` | `SetText`, `SetColorAndOpacity` |
| `UImage` | `SetBrushFromTexture`, `SetBrushFromMaterial`, `SetColorAndOpacity`, `SetOpacity` |
| `UButton` | `SetIsEnabled`, `SetBackgroundColor` |
| `USlider` | `SetValue`, `SetMinValue`, `SetMaxValue` |
| `UCheckBox` | `SetIsChecked`, `SetCheckedState` |
| `UEditableTextBox` | `SetText`, `SetIsReadOnly` |
| `UComboBoxString` | `AddOption`, `SetSelectedOption`, `ClearOptions` |
| `UWidgetSwitcher` | `SetActiveWidgetIndex`, `SetActiveWidget` |

**NOT included** (too niche or no common Blueprint-callable functions):
- `UScrollBox` -- functions are mostly inherited from `UPanelWidget`
- `UCanvasPanel` -- layout container, rarely called directly
- `UHorizontalBox` / `UVerticalBox` -- children managed via slots
- `UBorder` -- rarely has direct function calls
- `USizeBox` -- `SetWidthOverride` etc. but uncommon

### Why this works

FindFunction's Step 5 loop tries each library class: `UFunction* Func = TryClassWithK2(LibClass)`. When the AI calls `SetPercent` without target_class, the search will now reach `UProgressBar::StaticClass()` and find the function. The match method will be `EOliveFunctionMatchMethod::LibrarySearch`.

This is the same pattern already used for `AActor`, `APawn`, `ACharacter`, etc. -- classes that aren't function libraries but are commonly targeted by AI tool calls.

### Risk: LOW
Adding classes to a search list. If the function name collides (e.g., `SetText` exists on both `UTextBlock` and `UEditableTextBox`), the first match wins. This is the same risk already present with the existing list entries and is acceptable -- the AI should specify `target_class` when ambiguity matters.

---

## Implementation Order

1. **Issue 1 (ReceiveConstruct)** -- FIRST, highest priority, blocking. Single map change in resolver. Junior task, ~10 lines changed.

2. **Issue 2 (Rate limit)** -- SECOND. One-line default change. Junior task, trivial.

3. **Issue 3 (UMG FindFunction)** -- THIRD. Add includes + expand class list. Junior task, ~20 lines added.

All three are independent and could be implemented in parallel, but Issue 1 is the most urgent since it blocks all Widget Blueprint event creation.

---

## Files Modified

| File | Issue | Change |
|------|-------|--------|
| `Source/OliveAIEditor/Blueprint/Private/Plan/OliveBlueprintPlanResolver.cpp` | 1 | Fix EventNameMap widget entries (lines 2619-2629) |
| `Source/OliveAIEditor/Public/Settings/OliveAISettings.h` | 2 | Change default `MaxWriteOpsPerMinute` from 30 to 120 (line 329) |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | 3 | Add UMG widget #includes + expand LibraryClasses list (lines 63, 2476-2489) |
