# Blueprint Module UE 5.5 API Fix Plan

> **Date:** February 19, 2026
> **Author:** Architect
> **Status:** Ready for Implementation
> **Scope:** Fix all UE 5.5 API incompatibilities in the Blueprint.disabled folder

---

## 1. Executive Summary

The Blueprint module (currently disabled at `Source/OliveAIEditor/Blueprint.disabled/`) contains code written for an older UE version. This plan identifies all UE 5.5 API incompatibilities and provides a task-by-task fix guide for the coder.

### Files Affected

| File | Issue Count | Primary Issues |
|------|-------------|----------------|
| `OliveAnimGraphWriter.cpp` | 8 | `GetAllNodes()`, `BoundGraph`, `GetPreviousStatePin/GetNextStatePin`, `CreatePinsForTransition` |
| `OliveAnimGraphSerializer.cpp` | 3 | `BoundGraph` direct access |
| `OliveBlueprintWriter.cpp` | 6 | `ReparentBlueprint`, `ImplementNewInterface`, `RemoveInterface`, `ANY_PACKAGE` |
| `OliveCompileManager.cpp` | 1 | `HasCompilerMessage()` |
| `OliveBlueprintReader.cpp` | 2 | `FindEditorForAsset` const, `GetImplementableInterfaceFunctions` |
| `OliveComponentWriter.cpp` | 5 | `ANY_PACKAGE` |
| `OliveNodeFactory.cpp` | 4 | `ANY_PACKAGE` |
| `OliveBlueprintTypes.cpp` | 1 | `#include "Blueprint/WidgetBlueprint.h"` |
| `OliveWidgetWriter.cpp` | 2 | `#include "Blueprint/WidgetBlueprint.h"` |
| `OliveWidgetTreeSerializer.cpp` | 2 | `#include "Blueprint/WidgetBlueprint.h"` |
| `OliveBlueprintToolHandlers.h` | 1 | `#include "OliveToolRegistry.h"` |
| `OliveWritePipeline.h` | 1 | `#include "OliveToolRegistry.h"` |

---

## 2. UE 5.5 API Changes Reference

### 2.1 AnimGraph API Changes

| Old API (Pre-5.5) | New API (UE 5.5) | Files Affected |
|-------------------|------------------|----------------|
| `UAnimStateNodeBase::BoundGraph` | `UAnimStateNodeBase::GetBoundGraph()` | OliveAnimGraphWriter.cpp, OliveAnimGraphSerializer.cpp |
| `UEdGraph::GetAllNodes()` | `UEdGraph::Nodes` (direct access) | OliveAnimGraphWriter.cpp |
| `UAnimStateTransitionNode::GetPreviousStatePin()` | `UAnimStateTransitionNode::GetInputPin()` | OliveAnimGraphWriter.cpp |
| `UAnimStateTransitionNode::GetNextStatePin()` | `UAnimStateTransitionNode::GetOutputPin()` | OliveAnimGraphWriter.cpp |
| `UAnimStateTransitionNode::CreatePinsForTransition(From, To)` | `UAnimStateTransitionNode::CreateConnections(From, To)` | OliveAnimGraphWriter.cpp |

### 2.2 Blueprint Utility API Changes

| Old API (Pre-5.5) | New API (UE 5.5) | Files Affected |
|-------------------|------------------|----------------|
| `FBlueprintEditorUtils::ReparentBlueprint(BP, NewClass)` | Direct `ParentClass` assignment + `RefreshAllNodes()` | OliveBlueprintWriter.cpp |
| `FBlueprintEditorUtils::ImplementNewInterface(BP, ClassName)` | `FBlueprintEditorUtils::ImplementNewInterface(BP, FTopLevelAssetPath)` | OliveBlueprintWriter.cpp |
| `FBlueprintEditorUtils::RemoveInterface(BP, ClassName)` | `FBlueprintEditorUtils::RemoveInterface(BP, FTopLevelAssetPath)` | OliveBlueprintWriter.cpp |
| `FBlueprintEditorUtils::GetImplementableInterfaceFunctions()` | Iterate `Blueprint->ImplementedInterfaces` manually | OliveBlueprintReader.cpp |
| `UK2Node::HasCompilerMessage()` | `UK2Node::bHasCompilerMessage` | OliveCompileManager.cpp |

### 2.3 Object Finding API Changes

| Old API (Pre-5.5) | New API (UE 5.5) | Files Affected |
|-------------------|------------------|----------------|
| `FindObject<UClass>(ANY_PACKAGE, *ClassName)` | `FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None, ELogVerbosity::Warning)` | OliveBlueprintWriter.cpp, OliveComponentWriter.cpp, OliveNodeFactory.cpp |

### 2.4 Include Path Changes

| Old Include | New Include | Module Dependency | Files Affected |
|-------------|-------------|-------------------|----------------|
| `#include "Blueprint/WidgetBlueprint.h"` | `#include "WidgetBlueprint.h"` | UMGEditor | OliveBlueprintTypes.cpp, OliveWidgetWriter.cpp, OliveWidgetTreeSerializer.cpp |
| `#include "OliveToolRegistry.h"` | `#include "MCP/OliveToolRegistry.h"` | N/A (local path) | OliveBlueprintToolHandlers.h, OliveWritePipeline.h |

### 2.5 Editor Subsystem API Changes

| Old API (Pre-5.5) | New API (UE 5.5) | Files Affected |
|-------------------|------------------|----------------|
| `FindEditorForAsset(Blueprint, false)` | `FindEditorForAsset(const_cast<UBlueprint*>(Blueprint), false)` | OliveBlueprintReader.cpp |

---

## 3. Implementation Tasks

### Task 1: Fix Include Paths (Prerequisites)

**Priority:** Highest (blocks compilation of all other files)
**Estimated Time:** 5 minutes
**Files to Modify:**

1. **`Public/MCP/OliveBlueprintToolHandlers.h.disabled`**
   - Line 6: Change `#include "OliveToolRegistry.h"` to `#include "MCP/OliveToolRegistry.h"`

2. **`Public/Pipeline/OliveWritePipeline.h.disabled`**
   - Line 10: Change `#include "OliveToolRegistry.h"` to `#include "MCP/OliveToolRegistry.h"`

3. **`Private/OliveBlueprintTypes.cpp.disabled`**
   - Line 7: Change `#include "Blueprint/WidgetBlueprint.h"` to `#include "WidgetBlueprint.h"`

4. **`Private/Writer/OliveWidgetWriter.cpp.disabled`**
   - Line 4: Change `#include "Blueprint/WidgetBlueprint.h"` to `#include "WidgetBlueprint.h"`
   - Line 17: Keep `#include "WidgetBlueprint.h"` (already correct)

5. **`Private/Reader/OliveWidgetTreeSerializer.cpp.disabled`**
   - Line 4: Change `#include "Blueprint/WidgetBlueprint.h"` to `#include "WidgetBlueprint.h"`
   - Line 26: Keep `#include "WidgetBlueprint.h"` (already correct)

---

### Task 2: Fix AnimGraph Writer APIs

**Priority:** High
**Estimated Time:** 20 minutes
**File:** `Private/Writer/OliveAnimGraphWriter.cpp.disabled`

**Changes Required:**

1. **Line 119** - Replace `GetAllNodes()` with direct `Nodes` access:
   ```cpp
   // OLD:
   StateMachineNode->NodePosY = AnimGraph->GetAllNodes().Num() * 150;
   // NEW:
   StateMachineNode->NodePosY = AnimGraph->Nodes.Num() * 150;
   ```

2. **Line 228** - Replace `GetAllNodes()` with direct `Nodes` access:
   ```cpp
   // OLD:
   StateNode->NodePosX = StateMachineGraph->GetAllNodes().Num() * 250;
   // NEW:
   StateNode->NodePosX = StateMachineGraph->Nodes.Num() * 250;
   ```

3. **Line 238** - Replace `GetBoundGraph()` (if needed, verify if this is already the getter method):
   ```cpp
   // If BoundGraph is being accessed directly, change to:
   StateNode->GetBoundGraph()->Rename(*UniqueName, nullptr, REN_DontCreateRedirectors);
   ```

4. **Line 354** - Replace `CreatePinsForTransition`:
   ```cpp
   // OLD:
   TransitionNode->CreatePinsForTransition(FromState, ToState);
   // NEW:
   TransitionNode->CreateConnections(FromState, ToState);
   ```

5. **Lines 357-358** - Replace pin getter methods:
   ```cpp
   // OLD:
   UEdGraphPin* FromPin = TransitionNode->GetPreviousStatePin();
   UEdGraphPin* ToPin = TransitionNode->GetNextStatePin();
   // NEW:
   UEdGraphPin* FromPin = TransitionNode->GetInputPin();
   UEdGraphPin* ToPin = TransitionNode->GetOutputPin();
   ```

6. **Lines 600-601** - Same pin getter fix:
   ```cpp
   // OLD:
   UEdGraphPin* PrevPin = TransitionNode->GetPreviousStatePin();
   UEdGraphPin* NextPin = TransitionNode->GetNextStatePin();
   // NEW:
   UEdGraphPin* PrevPin = TransitionNode->GetInputPin();
   UEdGraphPin* NextPin = TransitionNode->GetOutputPin();
   ```

7. **Lines 462-469, 562-564, 709-711, 735-737** - Verify `GetBoundGraph()` usage is correct (should already be using getter method)

---

### Task 3: Fix AnimGraph Serializer APIs

**Priority:** High
**Estimated Time:** 10 minutes
**File:** `Private/Reader/OliveAnimGraphSerializer.cpp.disabled`

**Changes Required:**

1. **Line 431** - Replace direct `BoundGraph` access:
   ```cpp
   // OLD:
   UEdGraph* TransitionGraph = TransitionNode->BoundGraph;
   // NEW:
   UEdGraph* TransitionGraph = TransitionNode->GetBoundGraph();
   ```

2. **Line 591** - In `GetStateBoundGraph()` method:
   ```cpp
   // OLD:
   return StateNode->BoundGraph;
   // NEW:
   return StateNode->GetBoundGraph();
   ```

---

### Task 4: Fix Blueprint Writer APIs

**Priority:** High
**Estimated Time:** 30 minutes
**File:** `Private/Writer/OliveBlueprintWriter.cpp.disabled`

**Changes Required:**

1. **Line 367** - Replace `ReparentBlueprint`:
   ```cpp
   // OLD:
   const bool bSuccess = FBlueprintEditorUtils::ReparentBlueprint(Blueprint, NewParentUClass);

   // NEW:
   Blueprint->ParentClass = NewParentUClass;
   FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
   FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
   const bool bSuccess = true;
   ```

2. **Line 433** - Replace `ImplementNewInterface`:
   ```cpp
   // OLD:
   FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceClass->GetFName());

   // NEW:
   FTopLevelAssetPath InterfacePath(InterfaceClass->GetPathName());
   FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfacePath);
   ```

3. **Line 492** - Replace `RemoveInterface`:
   ```cpp
   // OLD:
   FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfaceClass->GetFName());

   // NEW:
   FTopLevelAssetPath InterfacePath(InterfaceClass->GetPathName());
   FBlueprintEditorUtils::RemoveInterface(Blueprint, InterfacePath);
   ```

4. **Lines 1441, 1456, 1471, 1486, 1524, 1535, 1568, 1576, 1584** - Replace `ANY_PACKAGE`:
   ```cpp
   // OLD:
   Class = FindObject<UClass>(ANY_PACKAGE, *ClassName);

   // NEW (Option A - Preferred):
   Class = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);

   // NEW (Option B - More explicit):
   Class = FindObject<UClass>(nullptr, *ClassName);
   // If not found, try with full path search
   ```

---

### Task 5: Fix Compile Manager API

**Priority:** Medium
**Estimated Time:** 5 minutes
**File:** `Private/Compile/OliveCompileManager.cpp.disabled`

**Changes Required:**

1. **Line 479** - Replace `HasCompilerMessage()`:
   ```cpp
   // OLD:
   if (K2Node->HasCompilerMessage() && !ProcessedNodeIds.Contains(NodeId))

   // NEW:
   if (K2Node->bHasCompilerMessage && !ProcessedNodeIds.Contains(NodeId))
   ```

---

### Task 6: Fix Blueprint Reader APIs

**Priority:** Medium
**Estimated Time:** 15 minutes
**File:** `Private/Reader/OliveBlueprintReader.cpp.disabled`

**Changes Required:**

1. **Line 111** - Fix `FindEditorForAsset` const issue:
   ```cpp
   // OLD:
   IR.bIsBeingEdited = AssetEditorSubsystem->FindEditorForAsset(Blueprint, false) != nullptr;

   // NEW:
   IR.bIsBeingEdited = AssetEditorSubsystem->FindEditorForAsset(const_cast<UBlueprint*>(Blueprint), false) != nullptr;
   ```

2. **Line 490** - Replace `GetImplementableInterfaceFunctions`:
   ```cpp
   // OLD:
   FBlueprintEditorUtils::GetImplementableInterfaceFunctions(Blueprint, OverridableFunctionNames);

   // NEW: Iterate implemented interfaces manually
   TSet<FName> OverridableFunctionNames;
   for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
   {
       if (UClass* InterfaceClass = InterfaceDesc.Interface)
       {
           for (TFieldIterator<UFunction> FuncIt(InterfaceClass, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
           {
               UFunction* Function = *FuncIt;
               if (Function && !Function->HasAnyFunctionFlags(FUNC_Private))
               {
                   OverridableFunctionNames.Add(Function->GetFName());
               }
           }
       }
   }
   ```

---

### Task 7: Fix Component Writer APIs

**Priority:** Medium
**Estimated Time:** 15 minutes
**File:** `Private/Writer/OliveComponentWriter.cpp.disabled`

**Changes Required:**

1. **Line 719** - Verify `GetAllNodes()` usage (this is `USCS_Node`, may still be valid):
   ```cpp
   // Check if USimpleConstructionScript::GetAllNodes() still exists
   // If deprecated, replace with:
   TArray<USCS_Node*> AllNodes;
   SCS->GetAllNodesRecursive(AllNodes);
   ```

2. **Lines 738, 748, 759, 799** - Replace `ANY_PACKAGE`:
   ```cpp
   // OLD:
   Class = FindObject<UClass>(ANY_PACKAGE, *NormalizedName);

   // NEW:
   Class = FindFirstObject<UClass>(*NormalizedName, EFindFirstObjectOptions::NativeFirst);
   ```

---

### Task 8: Fix Node Factory APIs

**Priority:** Medium
**Estimated Time:** 10 minutes
**File:** `Private/Writer/OliveNodeFactory.cpp.disabled`

**Changes Required:**

1. **Lines 647, 658, 716, 726, 750** - Replace `ANY_PACKAGE`:
   ```cpp
   // OLD:
   UClass* Class = FindObject<UClass>(ANY_PACKAGE, *ClassName);
   UScriptStruct* Struct = FindObject<UScriptStruct>(ANY_PACKAGE, *StructName);

   // NEW:
   UClass* Class = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
   UScriptStruct* Struct = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst);
   ```

---

### Task 9: Fix Component Reader APIs

**Priority:** Low
**Estimated Time:** 5 minutes
**File:** `Private/Reader/OliveComponentReader.cpp.disabled`

**Changes Required:**

1. **Line 152** - Verify `GetAllNodes()` usage:
   ```cpp
   // Check if USimpleConstructionScript::GetAllNodes() still exists
   // If deprecated, use:
   TArray<USCS_Node*> AllNodes;
   SCS->GetAllNodesRecursive(AllNodes);
   ```

---

### Task 10: Update Build.cs and Rename Folder

**Priority:** Final Step
**Estimated Time:** 5 minutes

1. **Update `OliveAIEditor.Build.cs`** - Add required module dependencies:
   ```csharp
   PrivateDependencyModuleNames.AddRange(new string[]
   {
       // ... existing dependencies ...

       // Required for Widget Blueprint support
       "UMGEditor",

       // Required for Animation Blueprint support
       "AnimGraph",
       "AnimGraphRuntime",
   });
   ```

2. **Rename folder** from `Blueprint.disabled` to `Blueprint`

3. **Remove `.disabled` suffix** from all files

---

## 4. Implementation Order

Execute tasks in this order to minimize compilation errors at each step:

1. **Task 1: Fix Include Paths** - Unblocks header parsing
2. **Task 10 (partial): Update Build.cs** - Add module dependencies
3. **Task 2: Fix AnimGraph Writer** - Core animation system
4. **Task 3: Fix AnimGraph Serializer** - Depends on Task 2 patterns
5. **Task 4: Fix Blueprint Writer** - Core write system
6. **Task 5: Fix Compile Manager** - Simple change
7. **Task 6: Fix Blueprint Reader** - Core read system
8. **Task 7: Fix Component Writer** - Component system
9. **Task 8: Fix Node Factory** - Node creation
10. **Task 9: Fix Component Reader** - Component reading
11. **Task 10 (final): Rename folder and files** - Enable compilation

---

## 5. Testing Checklist

After all fixes are applied, verify:

- [ ] Plugin compiles without errors
- [ ] Blueprint reading works for Normal, Interface, FunctionLibrary types
- [ ] Blueprint creation works
- [ ] Variable add/remove works
- [ ] Component add/remove works
- [ ] Function add/remove works
- [ ] Graph node operations work
- [ ] Animation Blueprint state machine reading works
- [ ] Animation Blueprint state machine writing works (add state, add transition)
- [ ] Widget Blueprint reading works
- [ ] Compilation results are correctly reported

---

## 6. Potential Issues

### 6.1 AnimGraph API Verification

The `CreateConnections` method name needs verification. If UE 5.5 uses a different method name, check:
- `UAnimStateTransitionNode` class definition in engine source
- Search for replacement of `CreatePinsForTransition`

### 6.2 FTopLevelAssetPath Constructor

The `FTopLevelAssetPath` constructor may require different arguments:
```cpp
// Try these variants if the simple constructor fails:
FTopLevelAssetPath InterfacePath(InterfaceClass);
// or
FTopLevelAssetPath InterfacePath(InterfaceClass->GetPackage()->GetFName(), InterfaceClass->GetFName());
```

### 6.3 FindFirstObject Availability

If `FindFirstObject` is not available in UE 5.5, use:
```cpp
UClass* Class = FindObject<UClass>(nullptr, *FullPath);
// Where FullPath is the full object path including package
```

---

## 7. Summary for Coder

The Blueprint module needs 36 individual API changes across 12 files. The changes fall into these categories:

1. **Include path fixes** (5 files) - Simple search/replace
2. **AnimGraph API updates** (2 files) - Use getter methods instead of direct member access
3. **Blueprint utility updates** (2 files) - Use new interface implementation APIs
4. **Object finding updates** (3 files) - Replace deprecated `ANY_PACKAGE`
5. **Build configuration** (1 file) - Add UMGEditor and AnimGraph dependencies

Start with Task 1 (includes) and Task 10 partial (Build.cs), then work through Tasks 2-9 in order. Test compilation after each major task.
