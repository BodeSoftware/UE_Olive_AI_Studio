# Pack P6 — `material.*` Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new `Material/` submodule with 5 MCP tools for material enumeration, application, and instance management.

**Architecture:** Mirror `Blueprint/`, `BehaviorTree/`, etc. submodule layout. `FOliveMaterialReader` for enumeration, `FOliveMaterialWriter` for application/instance creation. Both singletons. Writes use `FScopedTransaction` and `Modify()`. For Blueprint component writes, reuse `FOliveAssetResolver` to load the Blueprint.

**Tech Stack:** UE 5.5 C++, `UAssetRegistry`, `UMaterialInterface`, `UMaterialInstanceDynamic`, `FScopedTransaction`.

**Related spec:** `docs/superpowers/specs/2026-04-18-plugin-makeover-design.md` §3.11.

**Independence:** Fully isolated — zero dependencies on P1/P2/P3/P4/P5/P7. Can run in parallel with all others.

---

## Tools

| Tool | Type | Purpose |
|---|---|---|
| `material.list` | read | Enumerate materials in project with optional search_path / include_engine_materials filter |
| `material.read` | read | Parameters, texture refs, parent material for a given asset |
| `material.apply_to_component` | write | Apply material to a Blueprint component |
| `material.set_parameter_color` | write | Set a color/vector parameter on a material instance |
| `material.create_instance` | write | Create a new `UMaterialInstanceConstant` asset at a target path |

---

## File Structure

**Create:**

```
Source/OliveAIEditor/Material/
├── Public/
│   ├── OliveMaterialReader.h
│   ├── OliveMaterialWriter.h
│   └── MCP/
│       ├── OliveMaterialToolHandlers.h
│       └── OliveMaterialSchemas.h
└── Private/
    ├── OliveMaterialReader.cpp
    ├── OliveMaterialWriter.cpp
    └── MCP/
        ├── OliveMaterialToolHandlers.cpp
        └── OliveMaterialSchemas.cpp
```

**Modify:**
- `Source/OliveAIEditor/OliveAIEditor.Build.cs` — add Material/ include paths and required deps (`UnrealEd`, `MaterialEditor` where needed).
- `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp` — register `FOliveMaterialToolHandlers` in `OnPostEngineInit()`.

**Tests:**

```
Source/OliveAIEditor/Private/Tests/Material/
├── OliveMaterialReaderTests.cpp
└── OliveMaterialToolsTests.cpp
```

---

## Tasks

### Task 1: Scaffold directories + Build.cs

**Files:**
- Create: directories as above.
- Modify: `OliveAIEditor.Build.cs`.

- [ ] **Step 1: mkdir**

```bash
mkdir -p "Source/OliveAIEditor/Material/Public/MCP" \
         "Source/OliveAIEditor/Material/Private/MCP" \
         "Source/OliveAIEditor/Private/Tests/Material"
```

- [ ] **Step 2: Update Build.cs** — add include paths for `Material/Public` and `Material/Private` (follow the pattern used for other submodules).

- [ ] **Step 3: Build** — clean build expected.

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Material/ Source/OliveAIEditor/Private/Tests/Material/ Source/OliveAIEditor/OliveAIEditor.Build.cs
git commit -m "P6: scaffold Material/ submodule"
```

---

### Task 2: Implement `FOliveMaterialReader` — list + read

**Files:**
- Create: `Source/OliveAIEditor/Material/Public/OliveMaterialReader.h`
- Create: `Source/OliveAIEditor/Material/Private/OliveMaterialReader.cpp`

- [ ] **Step 1: Header**

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FOliveMaterialSummary
{
    FString Path;
    FString Name;
    FString ParentPath;    // for instances
    bool bIsInstance = false;
};

struct FOliveMaterialDetails
{
    FString Path;
    FString ParentPath;
    bool bIsInstance = false;
    TMap<FString, FLinearColor> VectorParameters;
    TMap<FString, float> ScalarParameters;
    TMap<FString, FString> TextureParameters;   // param name -> texture path
};

class OLIVEAIEDITOR_API FOliveMaterialReader
{
public:
    static FOliveMaterialReader& Get();

    TArray<FOliveMaterialSummary> ListMaterials(const FString& SearchPath, bool bIncludeEngine) const;
    FOliveMaterialDetails ReadMaterial(const FString& MaterialPath) const;

private:
    FOliveMaterialReader() = default;
};
```

- [ ] **Step 2: Cpp**

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "OliveMaterialReader.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"

FOliveMaterialReader& FOliveMaterialReader::Get()
{
    static FOliveMaterialReader I;
    return I;
}

TArray<FOliveMaterialSummary> FOliveMaterialReader::ListMaterials(const FString& SearchPath, bool bIncludeEngine) const
{
    TArray<FOliveMaterialSummary> Out;
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

    TArray<FAssetData> Assets;
    FARFilter Filter;
    Filter.bRecursivePaths = true;
    if (!SearchPath.IsEmpty())
    {
        Filter.PackagePaths.Add(*SearchPath);
    }
    else
    {
        Filter.PackagePaths.Add(TEXT("/Game"));
        if (bIncludeEngine) Filter.PackagePaths.Add(TEXT("/Engine"));
    }
    Filter.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;

    AR.GetAssets(Filter, Assets);

    for (const FAssetData& A : Assets)
    {
        FOliveMaterialSummary S;
        S.Path = A.GetObjectPathString();
        S.Name = A.AssetName.ToString();
        S.bIsInstance = A.AssetClassPath == UMaterialInstanceConstant::StaticClass()->GetClassPathName();
        if (S.bIsInstance)
        {
            if (UMaterialInstance* MI = Cast<UMaterialInstance>(A.GetAsset()))
            {
                S.ParentPath = MI->Parent ? MI->Parent->GetPathName() : FString();
            }
        }
        Out.Add(S);
    }
    return Out;
}

FOliveMaterialDetails FOliveMaterialReader::ReadMaterial(const FString& MaterialPath) const
{
    FOliveMaterialDetails D;
    UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!M) return D;
    D.Path = M->GetPathName();

    if (UMaterialInstance* MI = Cast<UMaterialInstance>(M))
    {
        D.bIsInstance = true;
        D.ParentPath = MI->Parent ? MI->Parent->GetPathName() : FString();

        for (const FVectorParameterValue& V : MI->VectorParameterValues)
        {
            D.VectorParameters.Add(V.ParameterInfo.Name.ToString(), V.ParameterValue);
        }
        for (const FScalarParameterValue& S : MI->ScalarParameterValues)
        {
            D.ScalarParameters.Add(S.ParameterInfo.Name.ToString(), S.ParameterValue);
        }
        for (const FTextureParameterValue& T : MI->TextureParameterValues)
        {
            D.TextureParameters.Add(T.ParameterInfo.Name.ToString(),
                T.ParameterValue ? T.ParameterValue->GetPathName() : FString());
        }
    }

    return D;
}
```

- [ ] **Step 3: Build**

```
ubt-build-5.5 /workspace/UE_Olive_AI_Toolkit/UE_Olive_AI_Toolkit.uproject
```

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Material/Public/OliveMaterialReader.h Source/OliveAIEditor/Material/Private/OliveMaterialReader.cpp
git commit -m "P6: implement FOliveMaterialReader (list + read)"
```

---

### Task 3: Reader tests

**Files:**
- Create: `Source/OliveAIEditor/Private/Tests/Material/OliveMaterialReaderTests.cpp`

- [ ] **Step 1: Write tests**

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "OliveMaterialReader.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialListEngineTest,
    "OliveAI.Material.Reader.ListEngineIncludesBasic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialListEngineTest::RunTest(const FString& Parameters)
{
    TArray<FOliveMaterialSummary> Mats = FOliveMaterialReader::Get().ListMaterials(TEXT("/Engine"), /*bIncludeEngine*/ true);
    TestTrue(TEXT("Engine materials enumeration returns >0"), Mats.Num() > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialReadBasicTest,
    "OliveAI.Material.Reader.ReadEngineBasicMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialReadBasicTest::RunTest(const FString& Parameters)
{
    // /Engine/EngineMaterials/DefaultMaterial is always present in UE 5.5
    FOliveMaterialDetails D = FOliveMaterialReader::Get().ReadMaterial(TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
    TestEqual(TEXT("Path matches"), D.Path, FString(TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
    TestFalse(TEXT("DefaultMaterial is not an instance"), D.bIsInstance);
    return true;
}
```

- [ ] **Step 2: Run tests**

Session Frontend > `OliveAI.Material.Reader.*`. Both pass.

- [ ] **Step 3: Commit**

```bash
git add Source/OliveAIEditor/Private/Tests/Material/OliveMaterialReaderTests.cpp
git commit -m "P6: MaterialReader tests"
```

---

### Task 4: Implement `FOliveMaterialWriter`

**Files:**
- Create: `Source/OliveAIEditor/Material/Public/OliveMaterialWriter.h`
- Create: `Source/OliveAIEditor/Material/Private/OliveMaterialWriter.cpp`

- [ ] **Step 1: Header**

```cpp
// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

class UBlueprint;

class OLIVEAIEDITOR_API FOliveMaterialWriter
{
public:
    static FOliveMaterialWriter& Get();

    FOliveToolResult ApplyToComponent(const FString& BlueprintPath, const FString& ComponentName, int32 Slot, const FString& MaterialPath);
    FOliveToolResult SetParameterColor(const FString& MaterialInstancePath, const FString& ParameterName, const FLinearColor& Color);
    FOliveToolResult CreateInstance(const FString& NewInstancePath, const FString& ParentMaterialPath);

private:
    FOliveMaterialWriter() = default;
};
```

- [ ] **Step 2: Cpp**

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "OliveMaterialWriter.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Components/MeshComponent.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "OliveMaterialWriter"

FOliveMaterialWriter& FOliveMaterialWriter::Get()
{
    static FOliveMaterialWriter I;
    return I;
}

static UBlueprint* LoadBP(const FString& Path)
{
    return LoadObject<UBlueprint>(nullptr, *Path);
}

FOliveToolResult FOliveMaterialWriter::ApplyToComponent(const FString& BlueprintPath, const FString& ComponentName, int32 Slot, const FString& MaterialPath)
{
    UBlueprint* BP = LoadBP(BlueprintPath);
    if (!BP)
        return FOliveToolResult::ValidationError(TEXT("BP_NOT_FOUND"),
            FString::Printf(TEXT("Blueprint '%s' not found."), *BlueprintPath), TEXT(""));

    UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!Mat)
        return FOliveToolResult::ValidationError(TEXT("MATERIAL_NOT_FOUND"),
            FString::Printf(TEXT("Material '%s' not found."), *MaterialPath), TEXT(""));

    if (!BP->SimpleConstructionScript)
        return FOliveToolResult::ValidationError(TEXT("NO_SCS"),
            TEXT("Blueprint has no SimpleConstructionScript."), TEXT(""));

    USCS_Node* Target = nullptr;
    for (USCS_Node* N : BP->SimpleConstructionScript->GetAllNodes())
    {
        if (N->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
        {
            Target = N;
            break;
        }
    }
    if (!Target)
        return FOliveToolResult::ValidationError(TEXT("COMPONENT_NOT_FOUND"),
            FString::Printf(TEXT("Component '%s' not found in Blueprint."), *ComponentName), TEXT(""));

    UMeshComponent* Template = Cast<UMeshComponent>(Target->ComponentTemplate);
    if (!Template)
        return FOliveToolResult::ValidationError(TEXT("COMPONENT_NOT_MESH"),
            TEXT("Component is not a MeshComponent."), TEXT(""));

    FScopedTransaction Tx(LOCTEXT("OliveApplyMaterialBP", "Olive: Apply Material to BP Component"));
    Template->Modify();
    Template->SetMaterial(Slot, Mat);
    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    return FOliveToolResult::Success();
}

FOliveToolResult FOliveMaterialWriter::SetParameterColor(const FString& MIPath, const FString& ParameterName, const FLinearColor& Color)
{
    UMaterialInstanceConstant* MI = LoadObject<UMaterialInstanceConstant>(nullptr, *MIPath);
    if (!MI)
        return FOliveToolResult::ValidationError(TEXT("MI_NOT_FOUND"),
            FString::Printf(TEXT("Material instance '%s' not found."), *MIPath),
            TEXT("The target must be a UMaterialInstanceConstant, not a UMaterial."));

    FScopedTransaction Tx(LOCTEXT("OliveSetParamColor", "Olive: Set Material Instance Vector Parameter"));
    MI->Modify();
    MI->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(*ParameterName), Color);
    MI->PostEditChange();

    return FOliveToolResult::Success();
}

FOliveToolResult FOliveMaterialWriter::CreateInstance(const FString& NewInstancePath, const FString& ParentMaterialPath)
{
    UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentMaterialPath);
    if (!Parent)
        return FOliveToolResult::ValidationError(TEXT("PARENT_NOT_FOUND"),
            FString::Printf(TEXT("Parent material '%s' not found."), *ParentMaterialPath), TEXT(""));

    // Derive package name and asset name from the provided path.
    FString PackageName, AssetName;
    if (!NewInstancePath.Split(TEXT("."), &PackageName, &AssetName))
    {
        PackageName = NewInstancePath;
        AssetName = FPaths::GetBaseFilename(NewInstancePath);
    }

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    IAssetTools& AssetTools = AssetToolsModule.Get();

    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = Parent;

    UObject* Created = AssetTools.CreateAsset(AssetName, FPackageName::GetLongPackagePath(PackageName),
                                              UMaterialInstanceConstant::StaticClass(), Factory);
    if (!Created)
        return FOliveToolResult::ExecutionError(TEXT("CREATE_FAILED"),
            TEXT("CreateAsset returned nullptr."), TEXT("Check that the target package path does not already contain the asset."));

    FOliveToolResult R = FOliveToolResult::Success();
    R.Data = MakeShared<FJsonObject>();
    R.Data->SetStringField(TEXT("path"), Created->GetPathName());
    return R;
}

#undef LOCTEXT_NAMESPACE
```

- [ ] **Step 3: Build** — clean build expected. If `MaterialEditor` headers are missing, add the `MaterialEditor` module to `PrivateDependencyModuleNames` in `Build.cs`.

- [ ] **Step 4: Commit**

```bash
git add Source/OliveAIEditor/Material/Public/OliveMaterialWriter.h Source/OliveAIEditor/Material/Private/OliveMaterialWriter.cpp Source/OliveAIEditor/OliveAIEditor.Build.cs
git commit -m "P6: implement FOliveMaterialWriter (apply/set-param/create-instance)"
```

---

### Task 5: Schemas + handlers + registration

**Files:**
- Create: `Source/OliveAIEditor/Material/Public/MCP/OliveMaterialSchemas.h`
- Create: `Source/OliveAIEditor/Material/Private/MCP/OliveMaterialSchemas.cpp`
- Create: `Source/OliveAIEditor/Material/Public/MCP/OliveMaterialToolHandlers.h`
- Create: `Source/OliveAIEditor/Material/Private/MCP/OliveMaterialToolHandlers.cpp`
- Modify: `Source/OliveAIEditor/Private/OliveAIEditorModule.cpp`

- [ ] **Step 1: Schemas**

Use the same helper-lambda pattern as P4 (see `OliveLevelSchemas.cpp`). Write schemas for:

- `material.list` — properties: `search_path: string`, `include_engine_materials: bool`. No required fields.
- `material.read` — properties: `path: string`. Required: `path`.
- `material.apply_to_component` — `blueprint_path: string`, `component_name: string`, `slot: number`, `material_path: string`. Required: all four.
- `material.set_parameter_color` — `material_instance_path: string`, `parameter_name: string`, `color: [r,g,b,a]`. Required: all three.
- `material.create_instance` — `new_instance_path: string`, `parent_material_path: string`. Required: both.

- [ ] **Step 2: Handlers**

One handler per tool. Parse params, validate required, call Reader/Writer, return `FOliveToolResult`. Same pattern as P4's handlers.

- [ ] **Step 3: Registration**

In `OliveAIEditorModule.cpp`, after level registration (step from P4), add:

```cpp
#include "MCP/OliveMaterialToolHandlers.h"
// ...
FOliveMaterialToolHandlers::Get().RegisterAllTools();
UE_LOG(LogOliveAI, Log, TEXT("Registered material.* MCP tools"));
```

- [ ] **Step 4: Build** — clean build.

- [ ] **Step 5: Commit**

```bash
git add Source/OliveAIEditor/Material/ Source/OliveAIEditor/Private/OliveAIEditorModule.cpp
git commit -m "P6: material.* schemas, handlers, and registration"
```

---

### Task 6: End-to-end tool tests

**Files:**
- Create: `Source/OliveAIEditor/Private/Tests/Material/OliveMaterialToolsTests.cpp`

- [ ] **Step 1: Write tests**

One test per tool. For the write tools, create a temporary target asset in the test (e.g., a minimal Blueprint with a StaticMeshComponent for `apply_to_component`) or use a known engine asset path if safe.

```cpp
// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "MCP/OliveToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialListToolTest,
    "OliveAI.Material.Tool.List",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialListToolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("search_path"), TEXT("/Engine"));
    P->SetBoolField(TEXT("include_engine_materials"), true);
    FOliveToolResult R = FOliveToolRegistry::Get().InvokeTool(TEXT("material.list"), P);
    TestTrue(TEXT("material.list success"), R.bSuccess);
    TestTrue(TEXT("Has materials array"), R.Data.IsValid() && R.Data->HasField(TEXT("materials")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialReadToolTest,
    "OliveAI.Material.Tool.Read",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialReadToolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    P->SetStringField(TEXT("path"), TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
    FOliveToolResult R = FOliveToolRegistry::Get().InvokeTool(TEXT("material.read"), P);
    TestTrue(TEXT("material.read success"), R.bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOliveMaterialCreateInstanceToolTest,
    "OliveAI.Material.Tool.CreateInstance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveMaterialCreateInstanceToolTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
    const FString NewPath = TEXT("/Game/Tests/MI_Olive_Test_Instance.MI_Olive_Test_Instance");
    P->SetStringField(TEXT("new_instance_path"), NewPath);
    P->SetStringField(TEXT("parent_material_path"), TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
    FOliveToolResult R = FOliveToolRegistry::Get().InvokeTool(TEXT("material.create_instance"), P);
    TestTrue(TEXT("Create instance succeeds"), R.bSuccess);

    // Cleanup: delete the created asset via the asset registry or just leave it for manual cleanup
    // (tests are editor-context; leaving artifacts is acceptable if named clearly).
    return true;
}
```

Add tests for `material.apply_to_component` and `material.set_parameter_color` that spin up a temporary Blueprint + MI, assert success, and clean up.

- [ ] **Step 2: Run tests** — all `OliveAI.Material.Tool.*` pass.

- [ ] **Step 3: Commit**

```bash
git add Source/OliveAIEditor/Private/Tests/Material/OliveMaterialToolsTests.cpp
git commit -m "P6: end-to-end material.* tool tests"
```

---

### Task 7: CLAUDE.md update

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Document Material/ submodule, tool list, file locations.**

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "P6: document material.* module in CLAUDE.md"
```

---

## Acceptance criteria

1. `ubt-build-5.5` green.
2. `OliveAI.Material.*` suite green (all 5 tools covered).
3. `FOliveToolRegistry::GetToolNames()` contains all 5 `material.*` tool names.
4. Material changes on Blueprint components are undoable (Ctrl+Z) — manually verified.
5. `CLAUDE.md` updated.

## Out of scope

- `level.*` (P4).
- World builders (P7).
- Parameter scalar/texture setters (only color was requested; follow-up pass).
