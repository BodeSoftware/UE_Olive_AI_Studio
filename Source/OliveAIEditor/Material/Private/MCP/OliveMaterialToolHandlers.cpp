// Copyright Bode Software. All Rights Reserved.

#include "MCP/OliveMaterialToolHandlers.h"
#include "MCP/OliveMaterialSchemas.h"
#include "OliveMaterialReader.h"
#include "OliveMaterialWriter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveMaterialTools, Log, All);

FOliveMaterialToolHandlers& FOliveMaterialToolHandlers::Get()
{
    static FOliveMaterialToolHandlers Instance;
    return Instance;
}

// ---------------------------------------------------------------------------
// Param helpers
// ---------------------------------------------------------------------------
namespace
{
    bool GetString(const TSharedPtr<FJsonObject>& P, const FString& Key, FString& Out)
    {
        return P.IsValid() && P->TryGetStringField(Key, Out);
    }

    bool GetBool(const TSharedPtr<FJsonObject>& P, const FString& Key, bool Default)
    {
        if (!P.IsValid())
        {
            return Default;
        }
        bool B = Default;
        P->TryGetBoolField(Key, B);
        return B;
    }

    int32 GetInt(const TSharedPtr<FJsonObject>& P, const FString& Key, int32 Default)
    {
        if (!P.IsValid())
        {
            return Default;
        }
        int32 I = Default;
        P->TryGetNumberField(Key, I);
        return I;
    }

    bool GetLinearColor(const TSharedPtr<FJsonObject>& P, const FString& Key, FLinearColor& Out)
    {
        if (!P.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!P->TryGetArrayField(Key, Arr) || !Arr || Arr->Num() < 4)
        {
            return false;
        }
        Out = FLinearColor(
            static_cast<float>((*Arr)[0]->AsNumber()),
            static_cast<float>((*Arr)[1]->AsNumber()),
            static_cast<float>((*Arr)[2]->AsNumber()),
            static_cast<float>((*Arr)[3]->AsNumber()));
        return true;
    }
}

// ---------------------------------------------------------------------------
// Read handlers
// ---------------------------------------------------------------------------

FOliveToolResult FOliveMaterialToolHandlers::HandleList(const TSharedPtr<FJsonObject>& Params)
{
    FString SearchPath;
    GetString(Params, TEXT("search_path"), SearchPath);
    const bool bIncludeEngine = GetBool(Params, TEXT("include_engine_materials"), false);

    TArray<FOliveMaterialSummary> Mats = FOliveMaterialReader::Get().ListMaterials(SearchPath, bIncludeEngine);

    TArray<TSharedPtr<FJsonValue>> JsonMats;
    JsonMats.Reserve(Mats.Num());
    for (const FOliveMaterialSummary& S : Mats)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("path"), S.Path);
        O->SetStringField(TEXT("name"), S.Name);
        O->SetBoolField(TEXT("is_instance"), S.bIsInstance);
        if (S.bIsInstance)
        {
            O->SetStringField(TEXT("parent_path"), S.ParentPath);
        }
        JsonMats.Add(MakeShared<FJsonValueObject>(O));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("materials"), JsonMats);
    Data->SetNumberField(TEXT("count"), Mats.Num());
    return FOliveToolResult::Success(Data);
}

FOliveToolResult FOliveMaterialToolHandlers::HandleRead(const TSharedPtr<FJsonObject>& Params)
{
    FString Path;
    if (!GetString(Params, TEXT("path"), Path) || Path.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'path' is required."),
            TEXT("Example: {\"path\": \"/Game/Materials/M_Brick.M_Brick\"}"));
    }

    FOliveMaterialDetails D = FOliveMaterialReader::Get().ReadMaterial(Path);
    if (D.Path.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MATERIAL_NOT_FOUND"),
            FString::Printf(TEXT("Material '%s' not found."), *Path),
            TEXT("Use a full asset path like /Game/Materials/M_Brick.M_Brick."));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("path"), D.Path);
    Data->SetBoolField(TEXT("is_instance"), D.bIsInstance);
    if (D.bIsInstance)
    {
        Data->SetStringField(TEXT("parent_path"), D.ParentPath);
    }

    // Vector parameters as {R, G, B, A} objects
    TSharedPtr<FJsonObject> VectorParams = MakeShared<FJsonObject>();
    for (const TPair<FString, FLinearColor>& Pair : D.VectorParameters)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(Pair.Value.R));
        Arr.Add(MakeShared<FJsonValueNumber>(Pair.Value.G));
        Arr.Add(MakeShared<FJsonValueNumber>(Pair.Value.B));
        Arr.Add(MakeShared<FJsonValueNumber>(Pair.Value.A));
        VectorParams->SetArrayField(Pair.Key, Arr);
    }
    Data->SetObjectField(TEXT("vector_parameters"), VectorParams);

    // Scalar parameters as {name: value}
    TSharedPtr<FJsonObject> ScalarParams = MakeShared<FJsonObject>();
    for (const TPair<FString, float>& Pair : D.ScalarParameters)
    {
        ScalarParams->SetNumberField(Pair.Key, Pair.Value);
    }
    Data->SetObjectField(TEXT("scalar_parameters"), ScalarParams);

    // Texture parameters as {name: asset_path}
    TSharedPtr<FJsonObject> TextureParams = MakeShared<FJsonObject>();
    for (const TPair<FString, FString>& Pair : D.TextureParameters)
    {
        TextureParams->SetStringField(Pair.Key, Pair.Value);
    }
    Data->SetObjectField(TEXT("texture_parameters"), TextureParams);

    return FOliveToolResult::Success(Data);
}

// ---------------------------------------------------------------------------
// Write handlers
// ---------------------------------------------------------------------------

FOliveToolResult FOliveMaterialToolHandlers::HandleApplyToComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintPath;
    FString ComponentName;
    FString MaterialPath;

    if (!GetString(Params, TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'blueprint_path' is required."),
            TEXT("Example: {\"blueprint_path\": \"/Game/BP_Wall.BP_Wall\"}"));
    }
    if (!GetString(Params, TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'component_name' is required."),
            TEXT("Component must exist in the Blueprint's SimpleConstructionScript."));
    }
    if (!Params.IsValid() || !Params->HasField(TEXT("slot")))
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'slot' is required."),
            TEXT("Slot is a 0-based integer material slot index."));
    }
    if (!GetString(Params, TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'material_path' is required."),
            TEXT("Example: {\"material_path\": \"/Game/Materials/M_Brick.M_Brick\"}"));
    }

    const int32 Slot = GetInt(Params, TEXT("slot"), 0);
    return FOliveMaterialWriter::Get().ApplyToComponent(BlueprintPath, ComponentName, Slot, MaterialPath);
}

FOliveToolResult FOliveMaterialToolHandlers::HandleSetParameterColor(const TSharedPtr<FJsonObject>& Params)
{
    FString InstancePath;
    FString ParameterName;

    if (!GetString(Params, TEXT("material_instance_path"), InstancePath) || InstancePath.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'material_instance_path' is required."),
            TEXT("Target must be a UMaterialInstanceConstant asset path."));
    }
    if (!GetString(Params, TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'parameter_name' is required."),
            TEXT("The parameter must exist on the instance's parent material."));
    }

    FLinearColor Color;
    if (!GetLinearColor(Params, TEXT("color"), Color))
    {
        return FOliveToolResult::Error(
            TEXT("INVALID_PARAM"),
            TEXT("Parameter 'color' must be an array of 4 numbers [R, G, B, A]."),
            TEXT("Example: {\"color\": [1.0, 0.0, 0.0, 1.0]}"));
    }

    return FOliveMaterialWriter::Get().SetParameterColor(InstancePath, ParameterName, Color);
}

FOliveToolResult FOliveMaterialToolHandlers::HandleCreateInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString NewInstancePath;
    FString ParentMaterialPath;

    if (!GetString(Params, TEXT("new_instance_path"), NewInstancePath) || NewInstancePath.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'new_instance_path' is required."),
            TEXT("Example: {\"new_instance_path\": \"/Game/Materials/MI_Brick_Red.MI_Brick_Red\"}"));
    }
    if (!GetString(Params, TEXT("parent_material_path"), ParentMaterialPath) || ParentMaterialPath.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'parent_material_path' is required."),
            TEXT("Example: {\"parent_material_path\": \"/Game/Materials/M_Brick.M_Brick\"}"));
    }

    return FOliveMaterialWriter::Get().CreateInstance(NewInstancePath, ParentMaterialPath);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FOliveMaterialToolHandlers::RegisterAllTools()
{
    UE_LOG(LogOliveMaterialTools, Log, TEXT("Registering material.* MCP tools..."));
    FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

    Registry.RegisterTool(
        TEXT("material.list"),
        TEXT("List material and material-instance assets in the project."),
        OliveMaterialSchemas::List(),
        FOliveToolHandler::CreateRaw(this, &FOliveMaterialToolHandlers::HandleList),
        {TEXT("material"), TEXT("read")},
        TEXT("material"));
    RegisteredToolNames.Add(TEXT("material.list"));

    Registry.RegisterTool(
        TEXT("material.read"),
        TEXT("Read a material or material instance's metadata and exposed parameters."),
        OliveMaterialSchemas::Read(),
        FOliveToolHandler::CreateRaw(this, &FOliveMaterialToolHandlers::HandleRead),
        {TEXT("material"), TEXT("read")},
        TEXT("material"));
    RegisteredToolNames.Add(TEXT("material.read"));

    Registry.RegisterTool(
        TEXT("material.apply_to_component"),
        TEXT("Assign a material to a specific slot on a mesh component inside a Blueprint."),
        OliveMaterialSchemas::ApplyToComponent(),
        FOliveToolHandler::CreateRaw(this, &FOliveMaterialToolHandlers::HandleApplyToComponent),
        {TEXT("material"), TEXT("write"), TEXT("blueprint")},
        TEXT("material"));
    RegisteredToolNames.Add(TEXT("material.apply_to_component"));

    Registry.RegisterTool(
        TEXT("material.set_parameter_color"),
        TEXT("Set a vector (color) parameter on a material instance."),
        OliveMaterialSchemas::SetParameterColor(),
        FOliveToolHandler::CreateRaw(this, &FOliveMaterialToolHandlers::HandleSetParameterColor),
        {TEXT("material"), TEXT("write")},
        TEXT("material"));
    RegisteredToolNames.Add(TEXT("material.set_parameter_color"));

    Registry.RegisterTool(
        TEXT("material.create_instance"),
        TEXT("Create a new UMaterialInstanceConstant asset from a parent material."),
        OliveMaterialSchemas::CreateInstance(),
        FOliveToolHandler::CreateRaw(this, &FOliveMaterialToolHandlers::HandleCreateInstance),
        {TEXT("material"), TEXT("write")},
        TEXT("material"));
    RegisteredToolNames.Add(TEXT("material.create_instance"));

    UE_LOG(LogOliveMaterialTools, Log, TEXT("Registered %d material.* MCP tools"), RegisteredToolNames.Num());
}

void FOliveMaterialToolHandlers::UnregisterAllTools()
{
    FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
    for (const FString& Name : RegisteredToolNames)
    {
        Registry.UnregisterTool(Name);
    }
    RegisteredToolNames.Reset();
}
