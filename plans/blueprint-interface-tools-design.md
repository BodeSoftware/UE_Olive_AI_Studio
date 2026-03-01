# Blueprint Interface (BPI) Tooling Design

**Author:** Architect Agent
**Date:** 2026-03-01
**Status:** Draft
**Scope:** New `blueprint.create_interface` tool, VariableGet ghost node fix, DoesImplementInterface alias, recipe + template updates

---

## Problem Statement

The AI agent knows UE interface patterns but our tools give it NO way to create Blueprint Interface assets. In a pickup interaction test, the AI cast on every actor instead of using interfaces because:

1. No tool to CREATE a BPI asset
2. No recipe teaching the full BPI workflow
3. `add_node VariableGet` via `CreateNodeByClass` creates ghost nodes (0 pins) because `FMemberReference` is not handled for `UK2Node_Variable` subclasses
4. `DoesImplementInterface` is not in the function alias map, making it harder for the AI to discover

What already works:
- `blueprint.add_interface` tool (adds BPI to an existing BP)
- `K2Node_Message` creation (auto-detected in `CreateCallFunctionNode` when `bIsInterfaceCall`)
- Plan_json `call` op with `target_class: "BPI_Name"` (FindFunction resolves via InterfaceSearch)

---

## Change Summary

| Task | File(s) | Type | Risk |
|------|---------|------|------|
| T1: `blueprint.create_interface` tool | OliveBlueprintSchemas.h/cpp, OliveBlueprintToolHandlers.h/cpp, OliveBlueprintWriter.h/cpp | New tool | Low |
| T2: VariableGet/Set `FMemberReference` fix in `CreateNodeByClass` | OliveNodeFactory.cpp | Bug fix | Low |
| T3: `DoesImplementInterface` alias in GetAliasMap | OliveNodeFactory.cpp | Enhancement | Trivial |
| T4: Interface workflow recipe | Content/SystemPrompts/Knowledge/recipes/blueprint/interface_pattern.txt, _manifest.json | New content | None |
| T5: Update pickup_interaction reference template | Content/Templates/reference/pickup_interaction.json | Content update | None |

---

## Task 1: `blueprint.create_interface` Tool

### 1.1 Writer Method

Add `CreateBlueprintInterface()` to `FOliveBlueprintWriter`.

**File:** `Source/OliveAIEditor/Blueprint/Public/Writer/OliveBlueprintWriter.h`

Add to public Asset-Level Operations section (after `AddInterface`/`RemoveInterface`):

```cpp
/**
 * Create a new Blueprint Interface asset with function signatures.
 * Functions with outputs become implementable functions.
 * Functions without outputs become implementable events.
 * @param AssetPath Full asset path (e.g., "/Game/Interfaces/BPI_Interactable")
 * @param Functions Array of function signatures to add to the interface
 * @return Result with asset path on success, list of created function names
 */
FOliveBlueprintWriteResult CreateBlueprintInterface(
    const FString& AssetPath,
    const TArray<FOliveIRFunctionSignature>& Functions);
```

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveBlueprintWriter.cpp`

Implementation approach -- follows the `CreateBlueprint` pattern precisely:

```cpp
FOliveBlueprintWriteResult FOliveBlueprintWriter::CreateBlueprintInterface(
    const FString& AssetPath,
    const TArray<FOliveIRFunctionSignature>& Functions)
{
    if (IsPIEActive())
    {
        return FOliveBlueprintWriteResult::Error(
            TEXT("Cannot create Blueprints while Play-In-Editor is active"));
    }

    // Validate path format
    if (!FPackageName::IsValidLongPackageName(AssetPath))
    {
        return FOliveBlueprintWriteResult::Error(
            FString::Printf(TEXT("Invalid Blueprint asset path '%s'. "
                "Expected '/Game/.../BPI_Name'"), *AssetPath));
    }

    FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
    FString AssetName = FPackageName::GetShortName(AssetPath);

    if (AssetName.IsEmpty())
    {
        return FOliveBlueprintWriteResult::Error(
            FString::Printf(TEXT("Path '%s' is a folder, not an asset path. "
                "Append the interface name, e.g. '%s/BPI_Interactable'."),
                *AssetPath, *AssetPath));
    }

    // Check if asset already exists
    FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
    if (FindObject<UBlueprint>(nullptr, *FullObjectPath))
    {
        return FOliveBlueprintWriteResult::Error(
            FString::Printf(TEXT("Blueprint already exists at path: %s"),
                *AssetPath));
    }

    // Must have at least one function
    if (Functions.Num() == 0)
    {
        return FOliveBlueprintWriteResult::Error(
            TEXT("Blueprint Interface must have at least one function. "
                 "Provide a 'functions' array with function definitions."));
    }

    // Create the package
    UPackage* Package = CreatePackage(*AssetPath);
    if (!Package)
    {
        return FOliveBlueprintWriteResult::Error(
            FString::Printf(TEXT("Failed to create package for: %s"),
                *AssetPath));
    }

    // Transaction for undo support
    OLIVE_SCOPED_TRANSACTION(FText::Format(
        NSLOCTEXT("OliveBPWriter", "CreateBlueprintInterface",
            "Create Blueprint Interface '{0}'"),
        FText::FromString(AssetName)));

    // Create the Blueprint Interface
    // UInterface::StaticClass() is the parent for all Blueprint Interfaces
    UBlueprint* NewBPI = FKismetEditorUtilities::CreateBlueprint(
        UInterface::StaticClass(),
        Package,
        *AssetName,
        BPTYPE_Interface,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass()
    );

    if (!NewBPI)
    {
        return FOliveBlueprintWriteResult::Error(
            FString::Printf(TEXT("Failed to create Blueprint Interface: %s"),
                *AssetPath));
    }

    // Add each function signature to the interface
    TArray<FString> CreatedFunctions;
    TArray<FString> Warnings;

    for (const FOliveIRFunctionSignature& Sig : Functions)
    {
        if (Sig.Name.IsEmpty())
        {
            Warnings.Add(TEXT("Skipped function with empty name"));
            continue;
        }

        // Create a new function graph
        UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
            NewBPI,
            FName(*Sig.Name),
            UEdGraph::StaticClass(),
            UEdGraphSchema_K2::StaticClass()
        );

        if (!FuncGraph)
        {
            Warnings.Add(FString::Printf(
                TEXT("Failed to create function graph for '%s'"),
                *Sig.Name));
            continue;
        }

        // Add to Blueprint's function graphs
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(
            NewBPI, FuncGraph, /*bIsUserCreated=*/true,
            /*SignatureFromObject=*/nullptr);

        // Find entry node to add input parameters
        UK2Node_FunctionEntry* EntryNode = nullptr;
        for (UEdGraphNode* Node : FuncGraph->Nodes)
        {
            EntryNode = Cast<UK2Node_FunctionEntry>(Node);
            if (EntryNode) break;
        }

        if (EntryNode)
        {
            // Add input parameters to the entry node
            for (const FOliveIRFunctionParam& Param : Sig.Inputs)
            {
                FEdGraphPinType PinType = CreatePinTypeFromParam(Param);

                TSharedPtr<FUserPinInfo> NewPinInfo =
                    MakeShareable(new FUserPinInfo());
                NewPinInfo->PinName = FName(*Param.Name);
                NewPinInfo->PinType = PinType;
                // Entry node outputs are function inputs
                NewPinInfo->DesiredPinDirection = EGPD_Output;

                EntryNode->UserDefinedPins.Add(NewPinInfo);
            }

            EntryNode->ReconstructNode();
        }

        // Add output parameters via result node
        if (Sig.Outputs.Num() > 0)
        {
            UK2Node_FunctionResult* ResultNode = nullptr;
            for (UEdGraphNode* Node : FuncGraph->Nodes)
            {
                ResultNode = Cast<UK2Node_FunctionResult>(Node);
                if (ResultNode) break;
            }

            // Create result node if not found
            if (!ResultNode)
            {
                FGraphNodeCreator<UK2Node_FunctionResult> Creator(*FuncGraph);
                ResultNode = Creator.CreateNode();
                ResultNode->NodePosX = EntryNode ?
                    EntryNode->NodePosX + 400 : 400;
                ResultNode->NodePosY = EntryNode ?
                    EntryNode->NodePosY : 0;
                Creator.Finalize();
            }

            for (const FOliveIRFunctionParam& Param : Sig.Outputs)
            {
                FEdGraphPinType PinType = CreatePinTypeFromParam(Param);

                TSharedPtr<FUserPinInfo> NewPinInfo =
                    MakeShareable(new FUserPinInfo());
                NewPinInfo->PinName = FName(*Param.Name);
                NewPinInfo->PinType = PinType;
                // Result node inputs are function outputs
                NewPinInfo->DesiredPinDirection = EGPD_Input;

                ResultNode->UserDefinedPins.Add(NewPinInfo);
            }

            ResultNode->ReconstructNode();
        }

        CreatedFunctions.Add(Sig.Name);
    }

    // Mark as structurally modified and notify asset registry
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBPI);
    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewBPI);

    UE_LOG(LogOliveBPWriter, Log,
        TEXT("Created Blueprint Interface: %s with %d functions"),
        *AssetPath, CreatedFunctions.Num());

    FOliveBlueprintWriteResult Result =
        FOliveBlueprintWriteResult::Success(AssetPath, AssetName);
    for (const FString& W : Warnings)
    {
        Result.AddWarning(W);
    }
    return Result;
}
```

**Key UE API details (verified by researcher):**
- `FKismetEditorUtilities::CreateBlueprint` with `UInterface::StaticClass()` as parent and `BPTYPE_Interface` is the correct way to create a BPI
- `FBlueprintEditorUtils::AddFunctionGraph<UClass>` with template param `UClass` is the pattern used in the existing codebase (see line 1332 of OliveBlueprintWriter.cpp)
- `FUserPinInfo` + `EntryNode->UserDefinedPins` is how custom pins are added (same pattern as `AddFunction`, lines 969-975)
- `FGraphNodeCreator` is needed for creating result nodes when none exist (the BPI function graph may not auto-create one)

**Includes needed in OliveBlueprintWriter.cpp** (verify these are already present -- most should be):
- `K2Node_FunctionEntry.h` -- already included (line 14)
- `K2Node_FunctionResult.h` -- already included (line 15)
- No new includes required.

### 1.2 Tool Handler

**File:** `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h`

Add to the Asset Writer Tool Handlers section (after `HandleBlueprintDelete`):

```cpp
FOliveToolResult HandleBlueprintCreateInterface(const TSharedPtr<FJsonObject>& Params);
```

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

Implementation -- follows the `HandleBlueprintAddInterface` pattern with write pipeline:

```cpp
FOliveToolResult FOliveBlueprintToolHandlers::HandleBlueprintCreateInterface(
    const TSharedPtr<FJsonObject>& Params)
{
    // Validate parameters
    if (!Params.IsValid())
    {
        return FOliveToolResult::Error(
            TEXT("VALIDATION_INVALID_PARAMS"),
            TEXT("Parameters object is null"),
            TEXT("Provide 'path' and 'functions' parameters")
        );
    }

    // Extract path
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("VALIDATION_MISSING_PARAM"),
            TEXT("Required parameter 'path' is missing or empty"),
            TEXT("Provide the interface asset path (e.g., '/Game/Interfaces/BPI_Interactable')")
        );
    }

    // Path validation (same as HandleBlueprintCreate)
    {
        FString ShortName = FPackageName::GetShortName(AssetPath);
        if (ShortName.IsEmpty() || AssetPath.EndsWith(TEXT("/")))
        {
            return FOliveToolResult::Error(
                TEXT("VALIDATION_PATH_IS_FOLDER"),
                FString::Printf(TEXT("'%s' is a folder path, not an asset path."),
                    *AssetPath),
                FString::Printf(TEXT("Append the interface name: '%s/BPI_Interactable'"),
                    *AssetPath)
            );
        }

        if (!AssetPath.StartsWith(TEXT("/Game/")))
        {
            return FOliveToolResult::Error(
                TEXT("VALIDATION_INVALID_PATH_PREFIX"),
                FString::Printf(TEXT("Path '%s' must start with '/Game/'."),
                    *AssetPath),
                FString::Printf(TEXT("Use '/Game/Interfaces/%s'"), *ShortName)
            );
        }
    }

    // Extract functions array
    const TArray<TSharedPtr<FJsonValue>>* FunctionsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("functions"), FunctionsArray)
        || !FunctionsArray || FunctionsArray->Num() == 0)
    {
        return FOliveToolResult::Error(
            TEXT("VALIDATION_MISSING_PARAM"),
            TEXT("Required parameter 'functions' is missing or empty"),
            TEXT("Provide at least one function definition: "
                 "[{\"name\": \"Interact\", \"inputs\": [{\"name\": \"Caller\", \"type\": \"Actor\"}]}]")
        );
    }

    // Parse function signatures
    TArray<FOliveIRFunctionSignature> Functions;
    for (const TSharedPtr<FJsonValue>& FuncValue : *FunctionsArray)
    {
        const TSharedPtr<FJsonObject>* FuncObjPtr = nullptr;
        if (!FuncValue->TryGetObject(FuncObjPtr) || !FuncObjPtr->IsValid())
        {
            continue;
        }
        const TSharedPtr<FJsonObject>& FuncObj = *FuncObjPtr;

        FOliveIRFunctionSignature Sig;

        // Name (required)
        if (!FuncObj->TryGetStringField(TEXT("name"), Sig.Name)
            || Sig.Name.IsEmpty())
        {
            return FOliveToolResult::Error(
                TEXT("VALIDATION_MISSING_PARAM"),
                TEXT("Each function must have a 'name' field"),
                TEXT("Example: {\"name\": \"Interact\"}")
            );
        }

        // Parse inputs
        const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
        if (FuncObj->TryGetArrayField(TEXT("inputs"), InputsArray) && InputsArray)
        {
            for (const TSharedPtr<FJsonValue>& InputValue : *InputsArray)
            {
                const TSharedPtr<FJsonObject>* InputObjPtr = nullptr;
                if (!InputValue->TryGetObject(InputObjPtr) || !InputObjPtr->IsValid())
                {
                    continue;
                }
                const TSharedPtr<FJsonObject>& InputObj = *InputObjPtr;

                FOliveIRFunctionParam Param;
                InputObj->TryGetStringField(TEXT("name"), Param.Name);

                // Type -- accept string or object
                FString TypeStr;
                const TSharedPtr<FJsonObject>* TypeJsonPtr = nullptr;
                if (InputObj->TryGetObjectField(TEXT("type"), TypeJsonPtr)
                    && TypeJsonPtr->IsValid())
                {
                    Param.Type = ParseTypeFromParams(*TypeJsonPtr);
                }
                else if (InputObj->TryGetStringField(TEXT("type"), TypeStr))
                {
                    // Simple string type (e.g., "Actor", "Float", "Bool")
                    TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject());
                    TypeObj->SetStringField(TEXT("type"), TypeStr);
                    Param.Type = ParseTypeFromParams(TypeObj);
                }

                Sig.Inputs.Add(Param);
            }
        }

        // Parse outputs (same pattern)
        const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
        if (FuncObj->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray)
        {
            for (const TSharedPtr<FJsonValue>& OutputValue : *OutputsArray)
            {
                const TSharedPtr<FJsonObject>* OutputObjPtr = nullptr;
                if (!OutputValue->TryGetObject(OutputObjPtr)
                    || !OutputObjPtr->IsValid())
                {
                    continue;
                }
                const TSharedPtr<FJsonObject>& OutputObj = *OutputObjPtr;

                FOliveIRFunctionParam Param;
                OutputObj->TryGetStringField(TEXT("name"), Param.Name);

                FString TypeStr;
                const TSharedPtr<FJsonObject>* TypeJsonPtr = nullptr;
                if (OutputObj->TryGetObjectField(TEXT("type"), TypeJsonPtr)
                    && TypeJsonPtr->IsValid())
                {
                    Param.Type = ParseTypeFromParams(*TypeJsonPtr);
                }
                else if (OutputObj->TryGetStringField(TEXT("type"), TypeStr))
                {
                    TSharedPtr<FJsonObject> TypeObj = MakeShareable(new FJsonObject());
                    TypeObj->SetStringField(TEXT("type"), TypeStr);
                    Param.Type = ParseTypeFromParams(TypeObj);
                }

                Sig.Outputs.Add(Param);
            }
        }

        Functions.Add(Sig);
    }

    // Build write request
    FOliveWriteRequest Request;
    Request.ToolName = TEXT("blueprint.create_interface");
    Request.Params = Params;
    Request.AssetPath = AssetPath;
    Request.TargetAsset = nullptr; // New asset, does not exist yet
    Request.OperationDescription = FText::FromString(
        FString::Printf(TEXT("Create Blueprint Interface '%s' with %d functions"),
            *AssetPath, Functions.Num()));
    Request.OperationCategory = TEXT("create"); // Tier 1
    Request.bFromMCP = FOliveToolExecutionContext::IsFromMCP();
    Request.bAutoCompile = true;
    Request.bSkipVerification = false;

    // Create executor
    FOliveWriteExecutor Executor;
    Executor.BindLambda([AssetPath, Functions](
        const FOliveWriteRequest& Req, UObject* Target) -> FOliveWriteResult
    {
        FOliveBlueprintWriter& Writer = FOliveBlueprintWriter::Get();
        FOliveBlueprintWriteResult WriteResult =
            Writer.CreateBlueprintInterface(AssetPath, Functions);

        if (!WriteResult.bSuccess)
        {
            FString ErrorMsg = WriteResult.Errors.Num() > 0
                ? WriteResult.Errors[0] : TEXT("Unknown error");
            return FOliveWriteResult::ExecutionError(
                TEXT("BPI_CREATE_FAILED"),
                ErrorMsg,
                TEXT("Check the path and function signatures")
            );
        }

        // Build success result
        TSharedPtr<FJsonObject> ResultData =
            MakeShareable(new FJsonObject());
        ResultData->SetStringField(TEXT("asset_path"),
            WriteResult.AssetPath);
        ResultData->SetStringField(TEXT("created_name"),
            WriteResult.CreatedItemName);

        // List created functions
        TArray<TSharedPtr<FJsonValue>> FuncNames;
        for (const FOliveIRFunctionSignature& Sig : Functions)
        {
            FuncNames.Add(MakeShareable(
                new FJsonValueString(Sig.Name)));
        }
        ResultData->SetArrayField(TEXT("functions"), FuncNames);

        // Include usage guidance in result
        ResultData->SetStringField(TEXT("next_steps"),
            FString::Printf(
                TEXT("Interface created. To use it: "
                     "1) blueprint.add_interface on target BPs with interface='%s'. "
                     "2) Functions without outputs become events the BP must implement. "
                     "3) Functions with outputs become functions the BP must override. "
                     "4) Call through the interface with plan_json: "
                     "{\"op\": \"call\", \"target\": \"FunctionName\", "
                     "\"target_class\": \"%s\"} (creates UK2Node_Message)."),
                *WriteResult.AssetPath,
                *FPackageName::GetShortName(WriteResult.AssetPath)));

        FOliveWriteResult WR = FOliveWriteResult::Success(ResultData);

        // Forward warnings
        for (const FString& W : WriteResult.Warnings)
        {
            WR.Warnings.Add(W);
        }

        return WR;
    });

    // Execute through pipeline
    FOliveWritePipeline& Pipeline = FOliveWritePipeline::Get();
    FOliveWriteResult Result =
        ExecuteWithOptionalConfirmation(Pipeline, Request, Executor);

    return Result.ToToolResult();
}
```

### 1.3 Schema

**File:** `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h`

Add after `BlueprintAddInterface()`:

```cpp
/**
 * Schema for blueprint.create_interface
 * Create a new Blueprint Interface asset with function signatures.
 * Params: {path: string, functions: [{name: string, inputs?: [{name, type}], outputs?: [{name, type}]}]}
 */
TSharedPtr<FJsonObject> BlueprintCreateInterface();
```

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp`

Add after `BlueprintAddInterface()`:

```cpp
TSharedPtr<FJsonObject> BlueprintCreateInterface()
{
    TSharedPtr<FJsonObject> Properties = MakeProperties();

    Properties->SetObjectField(TEXT("path"),
        StringProp(TEXT("Asset path for the new Blueprint Interface "
            "(e.g., '/Game/Interfaces/BPI_Interactable')")));

    // Function parameter schema (input or output)
    TSharedPtr<FJsonObject> ParamSchema = MakeSchema(TEXT("object"));
    {
        TSharedPtr<FJsonObject> ParamProps = MakeProperties();
        ParamProps->SetObjectField(TEXT("name"),
            StringProp(TEXT("Parameter name")));
        ParamProps->SetObjectField(TEXT("type"),
            StringProp(TEXT("Parameter type (e.g., 'Actor', 'Float', 'Bool', "
                "'Text', 'Vector', 'FString')")));
        ParamSchema->SetObjectField(TEXT("properties"), ParamProps);
        AddRequired(ParamSchema, {TEXT("name"), TEXT("type")});
    }

    // Single function definition schema
    TSharedPtr<FJsonObject> FuncSchema = MakeSchema(TEXT("object"));
    {
        TSharedPtr<FJsonObject> FuncProps = MakeProperties();
        FuncProps->SetObjectField(TEXT("name"),
            StringProp(TEXT("Function name (e.g., 'Interact', 'GetDisplayName')")));
        FuncProps->SetObjectField(TEXT("inputs"),
            ArrayProp(TEXT("Input parameters (optional). "
                "Functions with no outputs become events in implementing BPs."),
                ParamSchema));
        FuncProps->SetObjectField(TEXT("outputs"),
            ArrayProp(TEXT("Output/return parameters (optional). "
                "Functions WITH outputs must be implemented as functions, "
                "not events."),
                ParamSchema));
        FuncSchema->SetObjectField(TEXT("properties"), FuncProps);
        AddRequired(FuncSchema, {TEXT("name")});
    }

    Properties->SetObjectField(TEXT("functions"),
        ArrayProp(TEXT("Array of function signatures to define on the interface. "
            "Each function can have inputs, outputs, or both."),
            FuncSchema));

    TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
    Schema->SetStringField(TEXT("description"),
        TEXT("Create a new Blueprint Interface (BPI) asset with function signatures. "
             "After creation, use blueprint.add_interface to implement it on target BPs. "
             "Functions without outputs become events; functions with outputs become "
             "overridable functions."));
    Schema->SetObjectField(TEXT("properties"), Properties);
    AddRequired(Schema, {TEXT("path"), TEXT("functions")});

    return Schema;
}
```

### 1.4 Registration

**File:** `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp`

In `RegisterAssetWriterTools()`, add after the `blueprint.add_interface` registration block (after line 559):

```cpp
// blueprint.create_interface
Registry.RegisterTool(
    TEXT("blueprint.create_interface"),
    TEXT("Create a new Blueprint Interface (BPI) asset with function signatures. "
         "Functions without outputs become implementable events. "
         "Functions with outputs become implementable functions. "
         "After creation, use blueprint.add_interface to implement it on target BPs."),
    OliveBlueprintSchemas::BlueprintCreateInterface(),
    FOliveToolHandler::CreateRaw(this, &FOliveBlueprintToolHandlers::HandleBlueprintCreateInterface),
    {TEXT("blueprint"), TEXT("write"), TEXT("create"), TEXT("interface")},
    TEXT("blueprint")
);
RegisteredToolNames.Add(TEXT("blueprint.create_interface"));
```

Update the log count in `RegisterAssetWriterTools()` from 6 to 7.

### 1.5 Confirmation Tier

This tool creates a new asset (like `blueprint.create`). It should be **Tier 1 (auto-execute)** -- creating a BPI is low risk, easily undone. The `OperationCategory = TEXT("create")` achieves this, matching `blueprint.create`.

### 1.6 New Error Code

- `BPI_CREATE_FAILED` -- returned when `CreateBlueprintInterface` fails in the executor lambda.

---

## Task 2: VariableGet/Set FMemberReference Fix in CreateNodeByClass

### Problem

When the AI calls `blueprint.add_node` with `type: "VariableGet"` (or `"K2Node_VariableGet"`) and the type falls through to `CreateNodeByClass` (the universal fallback), the node is created via `NewObject` + reflection. But `UK2Node_Variable::VariableReference` is an `FMemberReference` (nested struct) -- `SetNodePropertiesViaReflection` cannot reach its fields. The node ends up with an empty `VariableReference` and 0 pins after `AllocateDefaultPins`.

Note: The curated path through `CreateVariableGetNode` (registered in `NodeCreators` map) works fine because it calls `SetSelfMember` directly. The bug only affects the `CreateNodeByClass` fallback path. However, `CreateNodeByClass` IS reached when the AI passes `type: "K2Node_VariableGet"` (the class name rather than `"GetVariable"` / `"VariableGet"`).

### Fix Location

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`
**Function:** `CreateNodeByClass()` (starts at line 1628)

### Fix Specification

Add a new `UK2Node_Variable` block immediately after the existing `UK2Node_CallFunction` block (which ends at line 1758). The pattern mirrors the FMemberReference special-case for CallFunction:

```cpp
// --- FMemberReference support for UK2Node_Variable (Get/Set) ---
// Same issue as UK2Node_CallFunction: VariableReference is a nested
// FMemberReference that SetNodePropertiesViaReflection cannot populate.
// We must call SetSelfMember() BEFORE AllocateDefaultPins.
if (NewNode->IsA<UK2Node_Variable>())
{
    // Extract variable name from properties (check common aliases)
    FString VarName;
    const FString* VarNamePtr = Properties.Find(TEXT("variable_name"));
    if (!VarNamePtr) VarNamePtr = Properties.Find(TEXT("VariableName"));
    if (!VarNamePtr) VarNamePtr = Properties.Find(TEXT("MemberName"));
    if (!VarNamePtr) VarNamePtr = Properties.Find(TEXT("name"));
    if (VarNamePtr && !VarNamePtr->IsEmpty())
    {
        VarName = *VarNamePtr;
    }

    if (!VarName.IsEmpty())
    {
        UK2Node_Variable* VarNode = CastChecked<UK2Node_Variable>(NewNode);
        VarNode->VariableReference.SetSelfMember(FName(*VarName));

        UE_LOG(LogOliveNodeFactory, Log,
            TEXT("CreateNodeByClass: Set VariableReference.SetSelfMember('%s') "
                 "for %s"),
            *VarName, *NodeClass->GetName());
    }
    else
    {
        UE_LOG(LogOliveNodeFactory, Warning,
            TEXT("CreateNodeByClass: %s created without variable_name "
                 "property. The node will have no VariableReference and "
                 "likely 0 pins. Pass 'variable_name' (or 'VariableName'"
                 "/'MemberName'/'name') in properties."),
            *NodeClass->GetName());
    }
}
```

**Include needed:** `K2Node_Variable.h` -- check if already included. Looking at the includes (lines 1-65), `K2Node_VariableGet.h` and `K2Node_VariableSet.h` are included (lines 18-19), and `UK2Node_Variable` is the base class defined in `K2Node_Variable.h`. This header may be transitively included. The coder should verify and add `#include "K2Node_Variable.h"` if not already present.

**Zero-pin guard consideration:** Unlike CallFunction, we do NOT add a zero-pin guard for Variable nodes. The curated path `CreateVariableGetNode` also produces 0-pin nodes when the variable doesn't exist yet (e.g., not compiled). The node can still be valid -- it resolves at compile time. The existing warning log is sufficient.

---

## Task 3: DoesImplementInterface Alias

### Problem

`DoesImplementInterface` is a function on `UKismetSystemLibrary`. When the AI uses plan_json `call` op with `target: "DoesImplementInterface"`, FindFunction will find it via the library search step (KismetSystemLibrary is one of the 11 hardcoded libraries). However, agents sometimes use shortened forms like `ImplementsInterface` or `HasInterface`.

### Fix Location

**File:** `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp`
**Function:** `GetAliasMap()` (starts at line 2464)

### Fix Specification

Add aliases in an appropriate section (after the Actor Operations section, or create a new "Interface" section):

```cpp
// ================================================================
// Interface Checks (UKismetSystemLibrary)
// ================================================================
Map.Add(TEXT("ImplementsInterface"), TEXT("DoesImplementInterface"));
Map.Add(TEXT("HasInterface"), TEXT("DoesImplementInterface"));
Map.Add(TEXT("CheckInterface"), TEXT("DoesImplementInterface"));
```

This ensures that `DoesImplementInterface` is accessible via common shorthand. The canonical `DoesImplementInterface` already works via library search, but aliases make discovery easier for the AI.

---

## Task 4: Interface Workflow Recipe

### New File

**File:** `Content/SystemPrompts/Knowledge/recipes/blueprint/interface_pattern.txt`

```
TAGS: interface blueprint interface bpi interactable decouple cast
---
BLUEPRINT INTERFACE (BPI) workflow:
1. blueprint.create_interface with path and functions array
   Functions with outputs = implementable functions
   Functions without outputs = implementable events

2. blueprint.add_interface on each implementing BP (e.g., item BPs)

3. Implement interface functions in each BP:
   - No-output functions: appear as events in EventGraph (auto-created)
   - With-output functions: appear in FunctionGraphs (auto-created)
   - Wire logic with apply_plan_json targeting the interface function graph

4. Call through interface from the caller BP:
   plan_json call op with target_class set to the BPI name
   Example: {"op": "call", "target": "Interact", "target_class": "BPI_Interactable"}
   This creates a UK2Node_Message (interface call) -- no cast needed.

5. Check if an actor implements the interface:
   plan_json call op: {"op": "call", "target": "DoesImplementInterface"}
   The Interface pin default = BPI asset path + _C suffix

No casting needed. That is the whole point of interfaces.
```

### Manifest Update

**File:** `Content/SystemPrompts/Knowledge/recipes/_manifest.json`

Add to the `"blueprint"` category's `"recipes"` object:

```json
"interface_pattern": {
    "description": "Blueprint Interface (BPI) creation, implementation, and interface-based calling pattern",
    "tags": ["interface", "bpi", "interactable", "decouple", "cast", "message", "overlap"],
    "max_tokens": 200
}
```

---

## Task 5: Update pickup_interaction Reference Template

### Current State

The template at `Content/Templates/reference/pickup_interaction.json` already describes the BPI architecture pattern well. It needs a minor update to mention `blueprint.create_interface` as the tool for creating the BPI (since agents could not previously create one).

### Updated File

**File:** `Content/Templates/reference/pickup_interaction.json`

Replace the existing content:

```json
{
    "template_id": "pickup_interaction",
    "template_type": "reference",
    "display_name": "Pickup / Interaction Pattern (Blueprint Interface)",

    "catalog_description": "Decoupled pickup and interaction system using a Blueprint Interface. Items handle their own behavior; the player character interacts without knowing specific item types.",
    "catalog_examples": "",

    "tags": "pickup interact interactable equip item collect grab hold weapon loot drop attach detach interface blueprint interface input overlap",

    "patterns": [
        {
            "name": "PickupSystemArchitecture",
            "description": "A pickup system has three assets: a Blueprint Interface (BPI) created via blueprint.create_interface, one or more item Blueprints that implement it, and a player character that calls through it. The interface defines an Interact(InteractingActor:Actor) function. Each item implements this interface and handles its own pickup/drop/use logic inside its Interact implementation.",
            "notes": "Create the BPI first with blueprint.create_interface, then add it to each item BP with blueprint.add_interface. The character detects nearby items via overlap, iterates overlapping actors, checks DoesImplementInterface to filter for interactables, and calls Interact as an interface message (plan_json call op with target_class set to the BPI name). No casting needed -- any actor implementing the interface is interactive. If the project already has a player character, modify it rather than creating a new one."
        }
    ]
}
```

Changes from original:
- Added "created via blueprint.create_interface" to the description
- Added "Create the BPI first with blueprint.create_interface, then add it to each item BP with blueprint.add_interface" to notes
- Replaced "UK2Node_Message, not a direct call or cast" with "plan_json call op with target_class set to the BPI name" (tool-level guidance rather than engine-level)
- Kept under 150 lines (this is 18 lines)

---

## Implementation Order

The tasks have minimal dependencies and can be parallelized:

1. **T3 first** (trivial, 3 lines in alias map) -- takes 2 minutes
2. **T2 second** (bug fix, ~30 lines in CreateNodeByClass) -- takes 5 minutes
3. **T1 third** (new tool, largest change) -- takes 20-30 minutes
   - 1a: Writer method in OliveBlueprintWriter.h/cpp
   - 1b: Schema in OliveBlueprintSchemas.h/cpp
   - 1c: Handler in OliveBlueprintToolHandlers.h/cpp
   - 1d: Registration in RegisterAssetWriterTools
4. **T4 and T5 in parallel** (content files, no C++) -- takes 5 minutes

T4 and T5 have zero code dependencies and can be done at any time.

---

## Data Flow

```
AI Agent
  |
  | blueprint.create_interface(path, functions[])
  v
HandleBlueprintCreateInterface()
  | 1. Parse & validate params (path, functions array)
  | 2. Parse FOliveIRFunctionSignature[] from JSON
  | 3. Build FOliveWriteRequest (category="create", Tier 1)
  | 4. Bind executor lambda
  v
FOliveWritePipeline::Execute()
  | Stage 1: Validate (path format, functions non-empty)
  | Stage 2: Confirm (Tier 1 = auto-execute, no prompt)
  | Stage 3: Transact (FScopedTransaction)
  | Stage 4: Execute
  |   v
  |   FOliveBlueprintWriter::CreateBlueprintInterface()
  |     | FKismetEditorUtilities::CreateBlueprint(UInterface, BPTYPE_Interface)
  |     | For each function:
  |     |   CreateNewGraph() + AddFunctionGraph()
  |     |   Find EntryNode -> add UserDefinedPins (inputs)
  |     |   Find/Create ResultNode -> add UserDefinedPins (outputs)
  |     | MarkBlueprintAsStructurallyModified()
  |     | FAssetRegistryModule::AssetCreated()
  |     v
  |   FOliveBlueprintWriteResult
  | Stage 5: Verify (compile if bAutoCompile)
  | Stage 6: Report
  v
FOliveToolResult (asset_path, functions[], next_steps guidance)
```

---

## Edge Cases and Error Handling

| Scenario | Handling |
|----------|----------|
| Path already exists | Pre-check `FindObject<UBlueprint>`, return `BPI_CREATE_FAILED` |
| Empty functions array | Return `VALIDATION_MISSING_PARAM` before pipeline |
| Function with empty name | Skip with warning, continue others |
| Duplicate function names | UE's `CreateNewGraph` will handle this (second graph gets a suffix). Add warning. |
| Invalid type string (e.g., "foo") | `ParseTypeFromParams` falls back to string type. AI gets compile error feedback. |
| Path not starting with /Game/ | Pre-check in handler, return `VALIDATION_INVALID_PATH_PREFIX` |
| PIE active | Writer checks `IsPIEActive()`, returns error |
| BPI already exists but AI tries create_interface again | `FindObject` check catches it, returns error |
| Package creation fails | Writer returns error |
| VariableGet with nonexistent variable name | Node created, variable resolves at compile time (same as curated path) |

---

## Testing Plan

### Manual Tests

1. **Create simple BPI:** `blueprint.create_interface(path="/Game/Interfaces/BPI_Test", functions=[{"name": "TestFunc"}])` -- verify BPI appears in Content Browser, has one function graph
2. **Create BPI with inputs and outputs:** Functions with inputs, outputs, both, and neither. Verify pin types resolve correctly.
3. **Full workflow test:** Create BPI -> add to BP -> wire interface functions -> call through interface -> compile. Verify no compile errors.
4. **VariableGet via CreateNodeByClass:** `blueprint.add_node(type="K2Node_VariableGet", properties={"variable_name": "Health"})` -- verify node has pins
5. **DoesImplementInterface alias:** Plan_json `call` op with `target: "ImplementsInterface"` -- verify it resolves to `DoesImplementInterface`

### Automation Test (Optional)

Add a test in `Source/OliveAIEditor/Private/Tests/` that:
1. Creates a BPI via `FOliveBlueprintWriter::CreateBlueprintInterface`
2. Verifies the Blueprint type is `BPTYPE_Interface`
3. Verifies function graphs exist with correct names
4. Verifies entry/result nodes have correct pin counts
5. Cleans up by deleting the test asset

---

## Files Modified (Summary)

| File | Change |
|------|--------|
| `Source/OliveAIEditor/Blueprint/Public/Writer/OliveBlueprintWriter.h` | Add `CreateBlueprintInterface()` declaration |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveBlueprintWriter.cpp` | Add `CreateBlueprintInterface()` implementation |
| `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintSchemas.h` | Add `BlueprintCreateInterface()` declaration |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintSchemas.cpp` | Add `BlueprintCreateInterface()` implementation |
| `Source/OliveAIEditor/Blueprint/Public/MCP/OliveBlueprintToolHandlers.h` | Add `HandleBlueprintCreateInterface()` declaration |
| `Source/OliveAIEditor/Blueprint/Private/MCP/OliveBlueprintToolHandlers.cpp` | Add handler + registration in `RegisterAssetWriterTools()` |
| `Source/OliveAIEditor/Blueprint/Private/Writer/OliveNodeFactory.cpp` | FMemberReference fix for UK2Node_Variable + DoesImplementInterface aliases |
| `Content/SystemPrompts/Knowledge/recipes/blueprint/interface_pattern.txt` | NEW recipe file |
| `Content/SystemPrompts/Knowledge/recipes/_manifest.json` | Add interface_pattern entry |
| `Content/Templates/reference/pickup_interaction.json` | Update to mention create_interface tool |

---

## Coder Handoff Notes

1. **T2 include check:** Verify `K2Node_Variable.h` is transitively included via `K2Node_VariableGet.h`. If not, add `#include "K2Node_Variable.h"` to OliveNodeFactory.cpp.

2. **T1 FGraphNodeCreator:** The BPI function graph might or might not auto-create a `UK2Node_FunctionResult` when calling `AddFunctionGraph`. The existing `AddFunction` writer (line 980+) has the same pattern -- find or create result node. Follow that pattern exactly. If `FGraphNodeCreator` is not available, fall back to `NewObject<UK2Node_FunctionResult>(FuncGraph)` + manual `CreateNewGuid()` + `AllocateDefaultPins()` + `Graph->AddNode()`.

3. **Type parsing reuse:** The handler reuses `ParseTypeFromParams()` which is a member of `FOliveBlueprintToolHandlers`. Both the simple string-to-type path and the object-to-type path are demonstrated in the handler spec above. Follow the pattern from `HandleAddFunctionType_Function` (~line 3450).

4. **Registration position:** Add the `blueprint.create_interface` registration in `RegisterAssetWriterTools()` after line 559 (after `blueprint.add_interface`). The log count on line 594 must be updated from 6 to 7.

5. **No Build.cs changes needed.** All modified files are in existing modules.

6. **Alias map insertion point:** The `DoesImplementInterface` aliases should go in the GetAliasMap function. Find a natural section -- after the Actor Operations section (~line 2523) is a good spot. Create a new `// Interface Checks` section header.
