# Research: C++ Reflection API for UE 5.5 C++ Integration Tool

## Question
What UE 5.5 reflection APIs are needed for a C++ integration tool that reads class/struct/enum metadata, discovers source files, generates boilerplate, and triggers hot reload?

---

## Findings

### 1. UClass Reflection API

**Headers required:**
- `"UObject/Class.h"` — UClass, UStruct, UFunction, UEnum, UScriptStruct, UField, FImplementedInterface
- `"UObject/UnrealType.h"` — FProperty and all property subclasses, TFieldIterator
- `"UObject/ObjectMacros.h"` — EClassFlags, EFunctionFlags, EPropertyFlags, EStructFlags
- `"UObject/UObjectIterator.h"` — TObjectIterator
- `"UObject/FieldIterator.h"` — TFieldIterator (also pulled through UnrealType.h)
- `"UObject/Script.h"` — EFunctionFlags enum

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/CoreUObject/Public/UObject/Class.h`

---

#### 1a. Enumerating All Loaded UClass Objects

Use `TObjectIterator<UClass>`. This iterates all UClass objects in the GUObjectArray (game thread only):

```cpp
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"

for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
{
    UClass* Class = *ClassIt;
    if (!Class->IsNative()) continue; // Skip Blueprint-generated classes
    // ... process Class
}
```

To filter to project classes only (exclude engine classes), check the package path:

```cpp
UPackage* Pkg = Class->GetOutermost();
FString PkgPath = Pkg->GetPathName();
// Engine classes live in /Script/Engine, /Script/CoreUObject, etc.
// Project classes live in /Script/<YourGameModuleName>
if (!PkgPath.StartsWith(TEXT("/Script/Engine")) && !PkgPath.StartsWith(TEXT("/Script/CoreUObject")))
{
    // Likely a project/plugin class
}
```

The better check is `CLASS_CompiledFromBlueprint` — native C++ classes do NOT have this flag:

```cpp
bool bIsNativeCppClass = !Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
```

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/CoreUObject/Public/UObject/ObjectMacros.h` line 232, `UObjectIterator.h`

---

#### 1b. Class Name, Parent, Interfaces, Class Flags

```cpp
UClass* Class = ...; // from TObjectIterator

// Name
FString ClassName = Class->GetName(); // "AMyActor" (with prefix)
FString AuthoredName = Class->GetAuthoredName(); // Same in most cases

// Parent class
UClass* SuperClass = Class->GetSuperClass(); // null for UObject root

// Class flags (EClassFlags)
EClassFlags Flags = Class->GetClassFlags();
bool bIsAbstract      = Class->HasAnyClassFlags(CLASS_Abstract);
bool bIsNative        = Class->HasAnyClassFlags(CLASS_Native);
bool bIsBlueprint     = Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
bool bIsInterface     = Class->HasAnyClassFlags(CLASS_Interface);
bool bIsDeprecated    = Class->HasAnyClassFlags(CLASS_Deprecated);

// Blueprintable / BlueprintType via metadata (NOT class flags -- these are metadata keys)
bool bIsBlueprintable = Class->GetBoolMetaData(TEXT("Blueprintable"));
bool bIsBlueprintType = Class->GetBoolMetaData(TEXT("BlueprintType"));

// Implemented interfaces
const TArray<FImplementedInterface>& Interfaces = Class->Interfaces;
for (const FImplementedInterface& Iface : Interfaces)
{
    UClass* IfaceClass = Iface.Class;       // e.g. UMyInterface
    bool bK2Implemented = Iface.bImplementedByK2; // true if implemented in Blueprint
}

// Check if class implements a specific interface
bool bImplements = Class->ImplementsInterface(UMyInterface::StaticClass());
```

Source: `Class.h` lines 2928, 3091, 3386, 3508-3566

**Key EClassFlags values (from ObjectMacros.h):**
| Flag | Value | Meaning |
|------|-------|---------|
| `CLASS_Abstract` | `0x00000001` | Cannot instantiate directly |
| `CLASS_Native` | `0x00000080` | Native C++ class |
| `CLASS_CompiledFromBlueprint` | `0x00040000` | Blueprint-generated |
| `CLASS_Interface` | `0x00004000` | Is a UInterface |
| `CLASS_Deprecated` | `0x02000000` | Deprecated |
| `CLASS_NotPlaceable` | `0x00000200` | Cannot place in level |
| `CLASS_EditInlineNew` | `0x00001000` | Can be created inline in details |
| `CLASS_MinimalAPI` | `0x00080000` | Only exports minimal symbols |

---

#### 1c. Listing UPROPERTY Members

`TFieldIterator<FProperty>` iterates reflected properties on a UStruct (UClass inherits UStruct). The template parameter controls which field type is iterated.

```cpp
#include "UObject/UnrealType.h"  // TFieldIterator, FProperty, EPropertyFlags

// Iterate only properties declared on THIS class (ExcludeSuper):
for (TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
{
    FProperty* Prop = *PropIt;

    FString Name        = Prop->GetName();
    FString CPPType     = Prop->GetCPPType(); // "int32", "FString", "TObjectPtr<AActor>", etc.
    EPropertyFlags Flags = Prop->PropertyFlags;

    // Common flag checks
    bool bBlueprintVisible  = Prop->HasAnyPropertyFlags(CPF_BlueprintVisible);
    bool bBlueprintReadOnly = Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
    bool bEditAnywhere      = Prop->HasAnyPropertyFlags(CPF_Edit);
    bool bEditConst         = Prop->HasAnyPropertyFlags(CPF_EditConst);
    bool bInstancedRef      = Prop->HasAnyPropertyFlags(CPF_InstancedReference);
    bool bSaveGame          = Prop->HasAnyPropertyFlags(CPF_SaveGame);
    bool bReplicated        = Prop->HasAnyPropertyFlags(CPF_Net);
    bool bDeprecated        = Prop->HasAnyPropertyFlags(CPF_Deprecated);
    bool bTransient         = Prop->HasAnyPropertyFlags(CPF_Transient);
    bool bConfig            = Prop->HasAnyPropertyFlags(CPF_Config);
    bool bExposeOnSpawn     = Prop->HasAnyPropertyFlags(CPF_ExposeOnSpawn);

    // Include super class properties with IncludeSuper (default):
    // TFieldIterator<FProperty> PropIt(Class, EFieldIteratorFlags::IncludeSuper);
}

// To include inherited properties:
for (TFieldIterator<FProperty> PropIt(Class); PropIt; ++PropIt) { ... }
// Default EFieldIterationFlags::Default = IncludeSuper | IncludeDeprecated
```

**Key EPropertyFlags values (from ObjectMacros.h):**
| Flag | Value | Meaning |
|------|-------|---------|
| `CPF_Edit` | `0x01` | EditAnywhere |
| `CPF_BlueprintVisible` | `0x04` | BlueprintReadWrite (or BlueprintReadOnly) |
| `CPF_BlueprintReadOnly` | `0x10` | BlueprintReadOnly |
| `CPF_Net` | `0x20` | Replicated |
| `CPF_Parm` | `0x80` | Function parameter |
| `CPF_ReturnParm` | `0x400` | Function return value |
| `CPF_Config` | `0x4000` | Config |
| `CPF_Transient` | `0x2000` | Transient |
| `CPF_SaveGame` | `0x1000000` | SaveGame |
| `CPF_ExposeOnSpawn` | `0x1000000000000` | ExposeOnSpawn |
| `CPF_Deprecated` | `0x20000000` | Deprecated |

Source: `ObjectMacros.h` lines 398-459, `UnrealType.h` lines 1242-1259

---

#### 1d. Property Type Identification (CastField)

Concrete property types are declared in `"UObject/UnrealType.h"` (same header). Use `CastField<T>` (not `Cast<T>` — that's for UObjects):

```cpp
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"  // FEnumProperty

FProperty* Prop = ...;

// Identify the concrete property type
if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
{
    UClass* PropClass = ObjProp->PropertyClass; // e.g. AActor
}
else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
{
    UScriptStruct* Struct = StructProp->Struct; // e.g. FVector
}
else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
{
    FProperty* InnerProp = ArrayProp->Inner; // element type
}
else if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
{
    FProperty* KeyProp   = MapProp->KeyProp;
    FProperty* ValueProp = MapProp->ValueProp;
}
else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
{
    UEnum* Enum = EnumProp->GetEnum();
}
else if (FDelegateProperty* DelProp = CastField<FDelegateProperty>(Prop))
{
    UFunction* Sig = DelProp->SignatureFunction; // delegate signature
}
else if (FMulticastDelegateProperty* MCDelProp = CastField<FMulticastDelegateProperty>(Prop))
{
    UFunction* Sig = MCDelProp->SignatureFunction;
}
```

Concrete FProperty subclasses confirmed in `UnrealType.h`:
- `FNumericProperty` (base for int/float/byte types)
- `FObjectPropertyBase` → `FObjectProperty`, `FWeakObjectProperty`, `FLazyObjectProperty`, `FSoftObjectProperty`
- `FStructProperty` (for USTRUCT members)
- `FArrayProperty` (TArray)
- `FMapProperty` (TMap) — has `KeyProp` and `ValueProp`
- `FSetProperty` (TSet)
- `FBoolProperty`
- `FStrProperty` (FString)
- `FNameProperty` (FName)
- `FTextProperty` (FText)
- `FDelegateProperty`, `FMulticastDelegateProperty`

Source: `UnrealType.h` lines 2545-2991, `EnumProperty.h`

---

#### 1e. Listing UFUNCTION Members

```cpp
#include "UObject/Class.h"  // UFunction, EFunctionFlags
#include "UObject/Script.h" // EFunctionFlags enum

// Iterate functions on this class only (ExcludeSuper):
for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
{
    UFunction* Func = *FuncIt;
    FString FuncName = Func->GetName();
    EFunctionFlags FuncFlags = Func->FunctionFlags;

    bool bBlueprintCallable = Func->HasAnyFunctionFlags(FUNC_BlueprintCallable);
    bool bBlueprintPure     = Func->HasAnyFunctionFlags(FUNC_BlueprintPure);
    bool bBlueprintEvent    = Func->HasAnyFunctionFlags(FUNC_BlueprintEvent); // Implementable or Native
    bool bIsNative          = Func->HasAnyFunctionFlags(FUNC_Native);
    bool bIsStatic          = Func->HasAnyFunctionFlags(FUNC_Static);
    bool bIsDelegate        = Func->HasAnyFunctionFlags(FUNC_Delegate);
    bool bIsMulticast       = Func->HasAnyFunctionFlags(FUNC_MulticastDelegate);
    bool bIsConst           = Func->HasAnyFunctionFlags(FUNC_Const);
    bool bIsExec            = Func->HasAnyFunctionFlags(FUNC_Exec);
    bool bEditorOnly        = Func->HasAnyFunctionFlags(FUNC_EditorOnly);

    // BlueprintImplementableEvent: FUNC_BlueprintEvent set, FUNC_Native NOT set
    bool bIsImplementableEvent = Func->HasAnyFunctionFlags(FUNC_BlueprintEvent)
                              && !Func->HasAnyFunctionFlags(FUNC_Native);

    // BlueprintNativeEvent: FUNC_BlueprintEvent set, FUNC_Native IS set
    bool bIsNativeEvent = Func->HasAnyFunctionFlags(FUNC_BlueprintEvent)
                       && Func->HasAnyFunctionFlags(FUNC_Native);

    // Get return property
    FProperty* ReturnProp = Func->GetReturnProperty(); // nullptr if void

    // Iterate parameters
    for (TFieldIterator<FProperty> ParamIt(Func); ParamIt && ParamIt->HasAnyPropertyFlags(CPF_Parm); ++ParamIt)
    {
        FProperty* Param = *ParamIt;
        bool bIsReturn = Param->HasAnyPropertyFlags(CPF_ReturnParm);
        bool bIsOut    = Param->HasAnyPropertyFlags(CPF_OutParm) && !bIsReturn;
        FString ParamName    = Param->GetName();
        FString ParamCPPType = Param->GetCPPType();
    }
}

// Find a specific function by name (searches hierarchy):
UFunction* Func = Class->FindFunctionByName(TEXT("MyFunction"), EIncludeSuperFlag::IncludeSuper);
```

Source: `Class.h` lines 1938-2063, `Script.h` lines 130-183

**Key EFunctionFlags (from Script.h):**
| Flag | Value | Meaning |
|------|-------|---------|
| `FUNC_BlueprintCallable` | `0x04000000` | UFUNCTION(BlueprintCallable) |
| `FUNC_BlueprintEvent` | `0x08000000` | BlueprintImplementableEvent or BlueprintNativeEvent |
| `FUNC_BlueprintPure` | `0x10000000` | BlueprintPure |
| `FUNC_Native` | `0x00000400` | Has native C++ implementation |
| `FUNC_Static` | `0x00002000` | Static function |
| `FUNC_Exec` | `0x00000200` | Console exec function |
| `FUNC_Const` | `0x40000000` | Const function |
| `FUNC_Delegate` | `0x00100000` | Delegate signature |
| `FUNC_MulticastDelegate` | `0x00010000` | Multicast delegate |
| `FUNC_EditorOnly` | `0x20000000` | Editor-only |

**Detecting overridable functions:**
- `BlueprintImplementableEvent`: `FUNC_BlueprintEvent` AND NOT `FUNC_Native`
- `BlueprintNativeEvent`: `FUNC_BlueprintEvent` AND `FUNC_Native`

---

#### 1f. Class Metadata (UCLASS Specifiers)

UCLASS specifiers like `Blueprintable`, `BlueprintType`, `Abstract` are stored as UField metadata (editor-only, `WITH_METADATA`). They are NOT class flags — they are metadata key/value pairs:

```cpp
// Available in editor builds (WITH_METADATA defined)
bool bBlueprintable = Class->GetBoolMetaData(TEXT("Blueprintable"));
bool bBlueprintType = Class->GetBoolMetaData(TEXT("BlueprintType"));
bool bIsAbstract    = Class->HasAnyClassFlags(CLASS_Abstract); // This one IS a flag
FString Category    = Class->GetMetaData(TEXT("Category"));

// Check if metadata key exists at all
bool bHasMeta = Class->HasMetaData(TEXT("Blueprintable"));
```

Source: `Class.h` lines 218-335 (UField metadata methods, WITH_METADATA block)

Note: Metadata is only available in `WITH_METADATA` builds (editor + debug). It is stripped from shipping builds. Since this is an editor plugin, this is fine.

---

### 2. UEnum Reflection

```cpp
#include "UObject/Class.h"  // UEnum

// Enumerate all UEnums in the project:
for (TObjectIterator<UEnum> EnumIt; EnumIt; ++EnumIt)
{
    UEnum* Enum = *EnumIt;
    FString EnumName = Enum->GetName();

    int32 Count = Enum->NumEnums(); // includes _MAX sentinel
    for (int32 i = 0; i < Count; i++)
    {
        FName  EntryName      = Enum->GetNameByIndex(i);      // "EMyEnum::ValueA"
        FString ShortName     = Enum->GetNameStringByIndex(i); // "ValueA"
        int64  Value          = Enum->GetValueByIndex(i);
        FText  DisplayName    = Enum->GetDisplayNameTextByIndex(i); // localized display name
        FString AuthoredName  = Enum->GetAuthoredNameStringByIndex(i); // from metadata

        // Metadata per-value (editor only)
        bool bHidden = Enum->HasMetaData(TEXT("Hidden"), i);
        FString Tooltip = Enum->GetMetaData(TEXT("ToolTip"), i);
    }

    // Check if it's BlueprintType via metadata
    bool bBlueprintType = Enum->GetBoolMetaData(TEXT("BlueprintType"));
}
```

The `NumEnums()` count includes the auto-generated `EnumName_MAX` entry at the end. Always check `i < Enum->NumEnums() - 1` if you want to exclude the sentinel, or check `Enum->HasMetaData(TEXT("Hidden"), i)`.

Source: `Class.h` lines 2157-2720

---

### 3. UScriptStruct Reflection

```cpp
#include "UObject/Class.h"  // UScriptStruct, EStructFlags

// Enumerate all UScriptStructs:
for (TObjectIterator<UScriptStruct> StructIt; StructIt; ++StructIt)
{
    UScriptStruct* Struct = *StructIt;
    FString StructName = Struct->GetName(); // e.g. "FMyStruct"

    // Parent struct
    UScriptStruct* SuperStruct = Cast<UScriptStruct>(Struct->GetSuperStruct());

    // Struct flags
    EStructFlags StructFlags = Struct->StructFlags;
    bool bIsAtomic   = (StructFlags & STRUCT_Atomic) != 0;
    bool bImmutable  = (StructFlags & STRUCT_Immutable) != 0;

    // BlueprintType is metadata, NOT a struct flag
    bool bBlueprintType = Struct->GetBoolMetaData(TEXT("BlueprintType"));

    // Iterate properties (members) -- same TFieldIterator pattern as UClass:
    for (TFieldIterator<FProperty> PropIt(Struct); PropIt; ++PropIt)
    {
        FProperty* Prop = *PropIt;
        FString MemberName = Prop->GetName();
        FString MemberType = Prop->GetCPPType();
        bool bBlueprintVisible = Prop->HasAnyPropertyFlags(CPF_BlueprintVisible);
    }
}
```

Source: `Class.h` lines 950-1900, `ObjectMacros.h` lines 800-880

**EStructFlags values:**
- `STRUCT_NoFlags` = 0
- `STRUCT_Atomic` = `0x10` — Cannot be partial serialized; treat as single unit
- `STRUCT_Immutable` = `0x20` — Cannot be changed after construction

Note: There is no `STRUCT_BlueprintType` flag. BlueprintType for structs is stored in metadata, just like for classes and enums.

---

### 4. Delegate Signatures

Delegates are exposed as `FProperty` subtypes on UClasses. Their signatures are stored as `UDelegateFunction` / `USparseDelegateFunction` objects (subclasses of `UFunction`).

```cpp
#include "UObject/Class.h"  // UDelegateFunction, USparseDelegateFunction

// When iterating FProperty:
if (FDelegateProperty* DelProp = CastField<FDelegateProperty>(Prop))
{
    // Single-cast delegate
    UFunction* Sig = DelProp->SignatureFunction;
    // Sig is a UFunction with FUNC_Delegate set
    // Iterate Sig's parameters to get delegate signature:
    for (TFieldIterator<FProperty> ParamIt(Sig); ParamIt && ParamIt->HasAnyPropertyFlags(CPF_Parm); ++ParamIt)
    {
        FProperty* Param = *ParamIt;
        FString ParamName = Param->GetName();
        FString ParamType = Param->GetCPPType();
        bool bIsReturn    = Param->HasAnyPropertyFlags(CPF_ReturnParm);
    }
}

if (FMulticastDelegateProperty* MCProp = CastField<FMulticastDelegateProperty>(Prop))
{
    // Multicast delegate - same pattern
    UFunction* Sig = MCProp->SignatureFunction;
}
```

Standalone delegate functions (DECLARE_DYNAMIC_DELEGATE etc.) appear as `UDelegateFunction` objects. They can be found by iterating functions with `FUNC_Delegate` set, or by iterating delegate properties:

```cpp
for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
{
    if ((*FuncIt)->HasAnyFunctionFlags(FUNC_Delegate))
    {
        UDelegateFunction* DelFunc = Cast<UDelegateFunction>(*FuncIt);
        // process delegate signature...
    }
}
```

Source: `Class.h` lines 2105-2125

---

### 5. Source File Discovery

The key API is `FSourceCodeNavigation`, declared in `"SourceCodeNavigation.h"` (module `UnrealEd`):

```cpp
#include "SourceCodeNavigation.h"

// Map a UClass to its header file path:
FString HeaderPath;
bool bFound = FSourceCodeNavigation::FindClassHeaderPath(MyClass, HeaderPath);
// Returns absolute path e.g. "D:/MyProject/Source/MyGame/Public/MyActor.h"

// Map a UClass to its .cpp file path:
FString SourcePath;
bool bFoundCpp = FSourceCodeNavigation::FindClassSourcePath(MyClass, SourcePath);

// Map any UField (including UFunction, UStruct, UEnum) to its header:
FString FieldHeader;
FSourceCodeNavigation::FindClassHeaderPath(MyFunction, FieldHeader);

// Find which module a class belongs to:
FString ModuleName;
bool bFoundModule = FSourceCodeNavigation::FindClassModuleName(MyClass, ModuleName);

// Find a module's source directory:
FString ModulePath;
bool bFoundPath = FSourceCodeNavigation::FindModulePath(TEXT("MyGame"), ModulePath);
// Returns directory like "D:/MyProject/Source/MyGame/"
```

**Important caveats:**
- `FindClassHeaderPath` / `FindClassSourcePath` work by scanning `.Build.cs` files and matching source file names — they do NOT rely on debug symbols. This is reliable without a debug build.
- These are synchronous calls, safe to call from the game thread.
- `FSourceCodeNavigation` lives in the `UnrealEd` module. Add `"UnrealEd"` to `PrivateDependencyModuleNames`.
- For Blueprint-generated classes (CLASS_CompiledFromBlueprint), these methods will return false — no source file exists.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/UnrealEd/Public/SourceCodeNavigation.h` lines 343-374

---

### 6. Source Code Generation (New C++ Class Wizard)

The `GameProjectUtils` API (module `GameProjectGeneration`) provides the class generation pipeline:

**Required module:** Add `"GameProjectGeneration"` to `PrivateDependencyModuleNames`.

**Header:** `"GameProjectUtils.h"`, `"AddToProjectConfig.h"`, `"GameProjectGenerationModule.h"`

```cpp
#include "GameProjectUtils.h"
#include "GameProjectGenerationModule.h"
#include "AddToProjectConfig.h"

// --- Option A: Open the class wizard dialog ---
// This shows the UE "Add C++ Class" dialog and handles everything:
FGameProjectGenerationModule::Get().OpenAddCodeToProjectDialog(
    FAddToProjectConfig()
        .ParentClass(UMyBaseClass::StaticClass())  // Pre-select parent
        .DefaultClassName(TEXT("MyNewClass"))
        .Modal()
);

// --- Option B: Programmatically add code without UI ---
// Get module info for the target module:
const TArray<FModuleContextInfo>& Modules = FGameProjectGenerationModule::Get().GetCurrentProjectModules();
// Find the module you want to add to
const FModuleContextInfo& TargetModule = Modules[0]; // or search by name

// Build class info:
FNewClassInfo ParentInfo(UMyBaseClass::StaticClass());

// Validate and get disallowed names:
FSourceFileDatabase& SourceDB = const_cast<FSourceFileDatabase&>(FSourceCodeNavigation::GetSourceFileDatabase());
const TSet<FString>& DisallowedNames = SourceDB.GetDisallowedHeaderNames();

FString OutHeaderFilePath, OutCppFilePath;
FText FailReason;
GameProjectUtils::EAddCodeToProjectResult Result = GameProjectUtils::AddCodeToProject(
    TEXT("AMyNewActor"),              // Class name (with prefix)
    TEXT(""),                         // Class path (empty = module default)
    TargetModule,
    ParentInfo,
    DisallowedNames,
    OutHeaderFilePath,
    OutCppFilePath,
    FailReason
);

if (Result == GameProjectUtils::EAddCodeToProjectResult::Succeeded)
{
    // Files created at OutHeaderFilePath and OutCppFilePath
    // Hot reload / live coding may have already been triggered
}
```

**FNewClassInfo EClassType options:**
- `EClassType::UObject` — Standard UObject subclass (uses `BaseClass` pointer)
- `EClassType::EmptyCpp` — Plain C++ class (no UObject)
- `EClassType::SlateWidget` — SCompoundWidget subclass
- `EClassType::SlateWidgetStyle` — Slate style class
- `EClassType::UInterface` — UInterface

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Editor/GameProjectGeneration/Public/GameProjectUtils.h` lines 60-166, `AddToProjectConfig.h`

---

### 7. Hot Reload / Live Coding After Changes

**IHotReloadInterface** (deprecated for triggering but still functional for monitoring):

```cpp
#include "Misc/HotReloadInterface.h"

IHotReloadInterface* HotReload = IHotReloadInterface::GetPtr();
if (HotReload)
{
    // Trigger hot reload from editor (blocks until done if WaitForCompletion):
    ECompilationResult::Type Result = HotReload->DoHotReloadFromEditor(
        EHotReloadFlags::WaitForCompletion
    );
}
```

**Preferred modern approach — Live Coding:**

In UE 5.x, Live Coding (`Ctrl+Alt+F11`) is the preferred mechanism. The typical workflow in tools is:
1. Write the source files to disk
2. Call `FSourceCodeNavigation::OpenModuleSolution()` to open the IDE (for user to compile)
3. OR use `GEditor->RequestEndPlayMap()` and then trigger a recompile via the hot reload interface

**Practical recommendation for an AI tool:** Do not attempt fully automated compilation. The `AddCodeToProject` function already attempts a hot reload internally when it returns `Succeeded`. For manual source edits, instruct the user to use Live Coding or the editor's compile button. Do not call `DoHotReloadFromEditor` without user confirmation — it can crash the editor if the module fails to compile.

Source: `C:/Program Files/Epic Games/UE_5.5/Engine/Source/Runtime/CoreUObject/Public/Misc/HotReloadInterface.h`

---

## Build.cs Module Dependencies

For a full C++ integration tool in the `OliveAIEditor` module, add these to `PrivateDependencyModuleNames`:

```csharp
PrivateDependencyModuleNames.AddRange(new string[]
{
    "CoreUObject",         // UClass, UFunction, FProperty, TFieldIterator, TObjectIterator
    "Engine",              // Various engine types referenced by classes
    "UnrealEd",            // FSourceCodeNavigation
    "GameProjectGeneration", // GameProjectUtils, FGameProjectGenerationModule
    "Projects",            // FModuleContextInfo (if needed separately)
});
```

---

## Recommendations

1. **Use TObjectIterator<UClass> for class enumeration.** It is the standard UE pattern. Filter with `HasAnyClassFlags(CLASS_Native)` to exclude Blueprint classes, and filter by package path prefix to identify project vs engine classes.

2. **Use TFieldIterator<FProperty> for property enumeration and TFieldIterator<UFunction> for function enumeration.** Both take a UStruct* (UClass inherits UStruct). The default flags include super class fields — pass `EFieldIteratorFlags::ExcludeSuper` if you only want fields declared on the exact class.

3. **Blueprintable / BlueprintType are metadata keys, not class flags.** To check these, use `Class->GetBoolMetaData(TEXT("Blueprintable"))`. This is WITH_METADATA only (editor builds). Since this is an editor plugin, that is fine.

4. **BlueprintImplementableEvent vs BlueprintNativeEvent:** Check `FUNC_BlueprintEvent` AND (`FUNC_Native` absent = implementable, `FUNC_Native` present = native event with C++ fallback).

5. **UScriptStruct has no STRUCT_BlueprintType flag.** BlueprintType for structs is also metadata: `Struct->GetBoolMetaData(TEXT("BlueprintType"))`.

6. **FSourceCodeNavigation::FindClassHeaderPath(UField*)** is the correct API for header file discovery. It works for UClass, UFunction, UScriptStruct, and UEnum (all are UField subclasses). It does NOT need debug symbols — it works by file-name matching against the module directory.

7. **Do not CastField<T> using Cast<T>.** Property types (FProperty subclasses) are FField-derived, not UObject-derived. Use `CastField<FObjectProperty>(Prop)`, not `Cast<FObjectProperty>(Prop)`.

8. **Avoid triggering hot reload automatically.** `AddCodeToProject` handles this internally on success. For manual source edits, prompt the user to compile rather than calling `DoHotReloadFromEditor` programmatically — a compilation failure mid-session can crash the editor.

9. **UEnum iteration: NumEnums() includes the _MAX sentinel.** Either iterate `i < Enum->NumEnums() - 1` to skip it, or check `Enum->HasMetaData(TEXT("Hidden"), i)` per-entry.

10. **Thread safety: All TObjectIterator and TFieldIterator usage must be on the game thread.** The UObject system is not thread-safe for iteration.
