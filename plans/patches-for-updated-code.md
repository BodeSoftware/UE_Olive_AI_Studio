# Patches for Updated Code (bundled_code.txt - 100009 lines)

All line numbers reference the new bundled_code.txt uploaded Feb 23.

---

## PATCH 1: Fix wrapper prompt add_variable format (P1)

**File:** `OliveClaudeCodeProvider.cpp` (starts at line 72569 in bundled_code.txt)
**Location:** Line 73103 in bundled_code.txt

### Find this line:
```cpp
TEXT("3. blueprint.add_variable → {\"path\": \"/Game/Blueprints/BP_Gun\", \"name\": \"FireRate\", \"type\": \"Float\", \"default_value\": \"0.5\"}\n")
```

### Replace with:
```cpp
TEXT("3. blueprint.add_variable → {\"path\": \"/Game/Blueprints/BP_Gun\", \"variable\": {\"name\": \"FireRate\", \"type\": {\"category\": \"float\"}, \"default_value\": \"0.5\"}}\n")
TEXT("   For class refs: {\"type\": {\"category\": \"class\", \"class_name\": \"Actor\"}} (NOT \"AActor\")\n")
TEXT("   Simple types: {\"category\": \"float\"}, {\"category\": \"bool\"}, {\"category\": \"int\"}, {\"category\": \"vector\"}, {\"category\": \"string\"}\n")
```

### Why:
The handler at line 24278 does `Params->TryGetObjectField(TEXT("variable"), ...)`.
It requires a nested "variable" object. The old prompt showed flat params which
always fail with "Required parameter 'variable' is missing". This is why the AI
failed 8 times in a row (0.06ms = instant validation rejection).

Also, the wrapper prompt should show the EXACT format for common types and 
explicitly note "Actor" not "AActor" since ConvertIRType can't strip prefixes yet.

---

## PATCH 2: Add UE prefix stripping in ConvertIRType (P1)

**File:** `OliveBlueprintWriter.cpp`
**Location:** Lines 42300-42350 in bundled_code.txt (Object, Class, and Interface cases)

### Current code (the Class case, line ~42315):
```cpp
case EOliveIRTypeCategory::Class:
    PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
    if (!IRType.ClassName.IsEmpty())
    {
        UClass* Class = FindObject<UClass>(nullptr, *IRType.ClassName);
        if (!Class)
        {
            Class = FindFirstObject<UClass>( *IRType.ClassName);
        }
        if (Class)
        {
            PinType.PinSubCategoryObject = Class;
        }
    }
    break;
```

### Replace the Class case with:
```cpp
case EOliveIRTypeCategory::Class:
    PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
    if (!IRType.ClassName.IsEmpty())
    {
        UClass* Class = ResolveClassByName(IRType.ClassName);
        if (Class)
        {
            PinType.PinSubCategoryObject = Class;
        }
    }
    break;
```

### Apply the same change to the Object and Interface cases (lines ~42300 and ~42330).

### Add this helper method to FOliveBlueprintWriter (in the .cpp, before ConvertIRType):
```cpp
/**
 * Resolve a UClass by name with UE prefix fallback.
 * Tries: exact -> FindFirstObject -> strip A/U prefix -> add A prefix -> add U prefix
 * Matches the pattern used by FOliveBTNodeFactory::ResolveNodeClass.
 */
UClass* FOliveBlueprintWriter::ResolveClassByName(const FString& ClassName)
{
    // 1. Exact match
    UClass* Class = FindObject<UClass>(nullptr, *ClassName);
    if (Class) return Class;

    // 2. FindFirstObject (searches all packages)
    Class = FindFirstObject<UClass>(*ClassName);
    if (Class) return Class;

    // 3. Strip A/U prefix (AActor -> Actor, UObject -> Object)
    if (ClassName.Len() > 1 && (ClassName[0] == TEXT('A') || ClassName[0] == TEXT('U')))
    {
        FString Stripped = ClassName.Mid(1);
        Class = FindFirstObject<UClass>(*Stripped);
        if (Class) return Class;
    }

    // 4. Try adding A prefix (Actor -> AActor)
    Class = FindFirstObject<UClass>(*(TEXT("A") + ClassName));
    if (Class) return Class;

    // 5. Try adding U prefix (Object -> UObject)
    Class = FindFirstObject<UClass>(*(TEXT("U") + ClassName));
    if (Class) return Class;

    return nullptr;
}
```

### Add the declaration to OliveBlueprintWriter.h:
```cpp
/** Resolve a UClass by name with UE prefix fallback (A/U prefix strip/add) */
static UClass* ResolveClassByName(const FString& ClassName);
```

### Why:
The AI writes "AActor" because that's the C++ class name. The BT node factory
already handles this (line 16067-16074) but ConvertIRType doesn't. This caused
`TSubclassOf<AActor>` to fail while `TSubclassOf<Actor>` worked. With this fix,
both work.

---

## PATCH 3: Fix crash-on-cancel race condition (P0)

**File:** `OliveClaudeCodeProvider.cpp`
**Location:** Lines 73279-73284 (CancelRequest) and 73050-73067 (completion lambda)

### Current CancelRequest (line 73279):
```cpp
void FOliveClaudeCodeProvider::CancelRequest()
{
    bStopReading = true;
    KillClaudeProcess();
    bIsBusy = false;
}
```

### Replace with:
```cpp
void FOliveClaudeCodeProvider::CancelRequest()
{
    bStopReading = true;

    // Clear callbacks BEFORE killing process so the completion
    // lambda (if it races to fire) won't invoke stale delegates
    {
        FScopeLock Lock(&CallbackLock);
        CurrentOnComplete.Unbind();
        CurrentOnError.Unbind();
        CurrentOnChunk.Unbind();
        CurrentOnToolCall.Unbind();
        bIsBusy = false;
    }

    KillClaudeProcess();
}
```

### Current completion lambda (line ~73050):
```cpp
// Signal completion
AsyncTask(ENamedThreads::GameThread, [this, ReturnCode]()
{
    FScopeLock Lock(&CallbackLock);
    bIsBusy = false;

    if (ReturnCode != 0 && AccumulatedResponse.IsEmpty())
    {
        CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("Claude process exited with code %d"), ReturnCode));
    }
    else
    {
        FOliveProviderUsage Usage;
        Usage.Model = TEXT("claude-code-cli");
        CurrentOnComplete.ExecuteIfBound(AccumulatedResponse, Usage);
    }
});
```

### Replace with:
```cpp
// Signal completion
AsyncTask(ENamedThreads::GameThread, [this, ReturnCode]()
{
    FScopeLock Lock(&CallbackLock);

    // Guard: if already cancelled or a new request started, don't fire stale callbacks
    if (!bIsBusy)
    {
        return;
    }

    bIsBusy = false;

    if (ReturnCode != 0 && AccumulatedResponse.IsEmpty())
    {
        CurrentOnError.ExecuteIfBound(FString::Printf(TEXT("Claude process exited with code %d"), ReturnCode));
    }
    else
    {
        FOliveProviderUsage Usage;
        Usage.Model = TEXT("claude-code-cli");
        CurrentOnComplete.ExecuteIfBound(AccumulatedResponse, Usage);
    }
});
```

### Also guard the ParseOutputLine lambdas (~line 73036 and 73038):

Find both places where `ParseOutputLine` is called inside an AsyncTask lambda:
```cpp
AsyncTask(ENamedThreads::GameThread, [this, Line]()
{
    ParseOutputLine(Line);
});
```
and
```cpp
AsyncTask(ENamedThreads::GameThread, [this, OutputBuffer]()
{
    ParseOutputLine(OutputBuffer);
});
```

### Add a guard to each:
```cpp
AsyncTask(ENamedThreads::GameThread, [this, Line]()
{
    FScopeLock Lock(&CallbackLock);
    if (!bIsBusy) return;
    ParseOutputLine(Line);
});
```

### Why:
The crash at line 483 (address 0x24 = null pointer dereference) happens because:
1. Background thread finishes reading, schedules AsyncTask on game thread
2. User closes chat window, CancelRequest() runs on game thread
3. CancelRequest kills process, clears pipes, sets bIsBusy=false
4. Something triggers a new SendMessage which overwrites callbacks
5. The old AsyncTask fires, calls ExecuteIfBound on garbage/null delegates → CRASH

The fix: CancelRequest clears callbacks under lock first. The completion lambda
checks bIsBusy before firing — if cancel already ran, it no-ops.

---

## PATCH 4 (bonus): Better error message for type resolution failure

**File:** `OliveBlueprintWriter.cpp`  
**Location:** Line 42157 (ValidateVariableTypeForCreation)

### Find:
```cpp
if (bNeedsSubType && PinType.PinSubCategoryObject == nullptr)
{
    OutError = FString::Printf(
        TEXT("Type resolution failed for variable '%s' (%s). Provide a valid class/struct/enum name."),
        *Variable.Name,
        *Variable.Type.GetDisplayName());
    return false;
}
```

### Replace with:
```cpp
if (bNeedsSubType && PinType.PinSubCategoryObject == nullptr)
{
    FString NameHint;
    if (!Variable.Type.ClassName.IsEmpty()) NameHint = Variable.Type.ClassName;
    else if (!Variable.Type.StructName.IsEmpty()) NameHint = Variable.Type.StructName;
    else if (!Variable.Type.EnumName.IsEmpty()) NameHint = Variable.Type.EnumName;
    else NameHint = TEXT("(empty)");

    OutError = FString::Printf(
        TEXT("Type resolution failed for variable '%s': could not find '%s'. "
             "Use UE class names without prefix: 'Actor' not 'AActor', "
             "'StaticMeshComponent' not 'UStaticMeshComponent'. "
             "For TSubclassOf<Actor>, use category:'class' + class_name:'Actor'."),
        *Variable.Name,
        *NameHint);
    return false;
}
```

### Why:
The current error message says "provide a valid class/struct/enum name" but doesn't
say what was tried or how to fix it. The new message tells the AI exactly what
went wrong and what format to use. This cuts self-correction retries from 8+ to 1-2.

---

## Summary

| Patch | Priority | Impact | Lines Changed |
|-------|----------|--------|---------------|
| 1. Fix wrapper prompt format | P1 | Eliminates 8+ add_variable failures per run | ~3 lines |
| 2. UE prefix stripping | P1 | AActor/Actor/UActor all work | ~25 lines + helper |
| 3. Crash-on-cancel fix | P0 | Prevents editor crash on chat close | ~15 lines |
| 4. Better error message | P1 | 1-2 retries instead of 8+ on type errors | ~10 lines |
