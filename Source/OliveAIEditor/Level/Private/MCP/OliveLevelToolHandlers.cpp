// Copyright Bode Software. All Rights Reserved.

#include "MCP/OliveLevelToolHandlers.h"
#include "MCP/OliveLevelSchemas.h"
#include "Reader/OliveLevelReader.h"
#include "Writer/OliveLevelWriter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveLevelTools, Log, All);

FOliveLevelToolHandlers& FOliveLevelToolHandlers::Get()
{
    static FOliveLevelToolHandlers Instance;
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

    double GetNumber(const TSharedPtr<FJsonObject>& P, const FString& Key, double Default)
    {
        if (!P.IsValid())
        {
            return Default;
        }
        double N = Default;
        P->TryGetNumberField(Key, N);
        return N;
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

    FVector GetVec3(const TSharedPtr<FJsonObject>& P, const FString& Key, const FVector& Default)
    {
        if (!P.IsValid())
        {
            return Default;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!P->TryGetArrayField(Key, Arr) || !Arr || Arr->Num() < 3)
        {
            return Default;
        }
        return FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
    }

    FRotator GetRot(const TSharedPtr<FJsonObject>& P, const FString& Key, const FRotator& Default)
    {
        if (!P.IsValid())
        {
            return Default;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!P->TryGetArrayField(Key, Arr) || !Arr || Arr->Num() < 3)
        {
            return Default;
        }
        // Convention: [pitch, yaw, roll]
        return FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
    }
}

// ---------------------------------------------------------------------------
// Read handlers
// ---------------------------------------------------------------------------

FOliveToolResult FOliveLevelToolHandlers::HandleListActors(const TSharedPtr<FJsonObject>& /*Params*/)
{
    TArray<FOliveActorSummary> Actors = FOliveLevelReader::Get().ListActors();

    TArray<TSharedPtr<FJsonValue>> JsonActors;
    JsonActors.Reserve(Actors.Num());
    for (const FOliveActorSummary& S : Actors)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"), S.Name);
        O->SetStringField(TEXT("class_path"), S.ClassPath);
        O->SetStringField(TEXT("folder"), S.FolderPath);

        TArray<TSharedPtr<FJsonValue>> Loc;
        Loc.Add(MakeShared<FJsonValueNumber>(S.Location.X));
        Loc.Add(MakeShared<FJsonValueNumber>(S.Location.Y));
        Loc.Add(MakeShared<FJsonValueNumber>(S.Location.Z));
        O->SetArrayField(TEXT("location"), Loc);

        JsonActors.Add(MakeShared<FJsonValueObject>(O));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("actors"), JsonActors);
    Data->SetNumberField(TEXT("count"), Actors.Num());
    return FOliveToolResult::Success(Data);
}

FOliveToolResult FOliveLevelToolHandlers::HandleFindActors(const TSharedPtr<FJsonObject>& Params)
{
    FString Pattern;
    FString ClassPath;
    GetString(Params, TEXT("pattern"), Pattern);
    GetString(Params, TEXT("class_path"), ClassPath);

    TArray<FOliveActorSummary> Actors = FOliveLevelReader::Get().FindActors(Pattern, ClassPath);

    TArray<TSharedPtr<FJsonValue>> JsonActors;
    JsonActors.Reserve(Actors.Num());
    for (const FOliveActorSummary& S : Actors)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"), S.Name);
        O->SetStringField(TEXT("class_path"), S.ClassPath);
        JsonActors.Add(MakeShared<FJsonValueObject>(O));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("actors"), JsonActors);
    Data->SetNumberField(TEXT("count"), Actors.Num());
    return FOliveToolResult::Success(Data);
}

FOliveToolResult FOliveLevelToolHandlers::HandleGetActorMaterials(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (!GetString(Params, TEXT("name"), Name) || Name.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'name' is required."),
            TEXT("Example: {\"name\": \"MyActor\"}"));
    }

    TArray<FString> Mats = FOliveLevelReader::Get().GetActorMaterials(Name);

    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Reserve(Mats.Num());
    for (const FString& M : Mats)
    {
        Arr.Add(MakeShared<FJsonValueString>(M));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("materials"), Arr);
    return FOliveToolResult::Success(Data);
}

// ---------------------------------------------------------------------------
// Write handlers
// ---------------------------------------------------------------------------

FOliveToolResult FOliveLevelToolHandlers::HandleSpawnActor(const TSharedPtr<FJsonObject>& Params)
{
    FOliveSpawnActorArgs Args;
    if (!GetString(Params, TEXT("class_path"), Args.ClassPath) || Args.ClassPath.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'class_path' is required."),
            TEXT("Example: {\"class_path\": \"/Script/Engine.StaticMeshActor\"}"));
    }

    GetString(Params, TEXT("mesh_path"), Args.MeshPath);
    GetString(Params, TEXT("material_path"), Args.MaterialPath);
    GetString(Params, TEXT("label"), Args.Label);
    Args.Location = GetVec3(Params, TEXT("location"), FVector::ZeroVector);
    Args.Rotation = GetRot(Params, TEXT("rotation"), FRotator::ZeroRotator);
    Args.Scale = GetVec3(Params, TEXT("scale"), FVector::OneVector);
    Args.bSimulatePhysics = GetBool(Params, TEXT("simulate_physics"), false);
    Args.bEnableGravity = GetBool(Params, TEXT("enable_gravity"), true);
    Args.Mass = static_cast<float>(GetNumber(Params, TEXT("mass"), 100.0));

    return FOliveLevelWriter::Get().SpawnActor(Args);
}

FOliveToolResult FOliveLevelToolHandlers::HandleDeleteActor(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (!GetString(Params, TEXT("name"), Name) || Name.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'name' is required."),
            TEXT(""));
    }
    return FOliveLevelWriter::Get().DeleteActor(Name);
}

FOliveToolResult FOliveLevelToolHandlers::HandleSetTransform(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (!GetString(Params, TEXT("name"), Name) || Name.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'name' is required."),
            TEXT(""));
    }

    const FVector Loc = GetVec3(Params, TEXT("location"), FVector::ZeroVector);
    const FRotator Rot = GetRot(Params, TEXT("rotation"), FRotator::ZeroRotator);
    const FVector Scale = GetVec3(Params, TEXT("scale"), FVector::OneVector);
    return FOliveLevelWriter::Get().SetTransform(Name, Loc, Rot, Scale);
}

FOliveToolResult FOliveLevelToolHandlers::HandleSetPhysics(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    FString CompName;
    if (!GetString(Params, TEXT("name"), Name) || Name.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'name' is required."),
            TEXT(""));
    }
    GetString(Params, TEXT("component_name"), CompName);

    const bool bSim = GetBool(Params, TEXT("simulate_physics"), false);
    const bool bGrav = GetBool(Params, TEXT("enable_gravity"), true);
    const float Mass = static_cast<float>(GetNumber(Params, TEXT("mass"), 100.0));
    return FOliveLevelWriter::Get().SetPhysics(Name, CompName, bSim, bGrav, Mass);
}

FOliveToolResult FOliveLevelToolHandlers::HandleApplyMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    FString CompName;
    FString MaterialPath;

    if (!GetString(Params, TEXT("name"), Name) || Name.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'name' is required."),
            TEXT(""));
    }
    if (!GetString(Params, TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
    {
        return FOliveToolResult::Error(
            TEXT("MISSING_PARAM"),
            TEXT("Parameter 'material_path' is required."),
            TEXT(""));
    }
    GetString(Params, TEXT("component_name"), CompName);
    const int32 Slot = GetInt(Params, TEXT("slot"), 0);

    return FOliveLevelWriter::Get().ApplyMaterial(Name, CompName, Slot, MaterialPath);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FOliveLevelToolHandlers::RegisterAllTools()
{
    UE_LOG(LogOliveLevelTools, Log, TEXT("Registering level.* MCP tools..."));
    FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

    Registry.RegisterTool(
        TEXT("level.list_actors"),
        TEXT("List all actors in the current editor level."),
        OliveLevelSchemas::ListActors(),
        FOliveToolHandler::CreateRaw(this, &FOliveLevelToolHandlers::HandleListActors),
        {TEXT("level"), TEXT("read")},
        TEXT("level"));
    RegisteredToolNames.Add(TEXT("level.list_actors"));

    Registry.RegisterTool(
        TEXT("level.find_actors"),
        TEXT("Find actors by name pattern and/or class path."),
        OliveLevelSchemas::FindActors(),
        FOliveToolHandler::CreateRaw(this, &FOliveLevelToolHandlers::HandleFindActors),
        {TEXT("level"), TEXT("read")},
        TEXT("level"));
    RegisteredToolNames.Add(TEXT("level.find_actors"));

    Registry.RegisterTool(
        TEXT("level.spawn_actor"),
        TEXT("Spawn an actor into the current editor level with optional mesh, material, transform, and physics."),
        OliveLevelSchemas::SpawnActor(),
        FOliveToolHandler::CreateRaw(this, &FOliveLevelToolHandlers::HandleSpawnActor),
        {TEXT("level"), TEXT("write")},
        TEXT("level"));
    RegisteredToolNames.Add(TEXT("level.spawn_actor"));

    Registry.RegisterTool(
        TEXT("level.delete_actor"),
        TEXT("Delete an actor from the current level by name."),
        OliveLevelSchemas::DeleteActor(),
        FOliveToolHandler::CreateRaw(this, &FOliveLevelToolHandlers::HandleDeleteActor),
        {TEXT("level"), TEXT("write"), TEXT("danger")},
        TEXT("level"));
    RegisteredToolNames.Add(TEXT("level.delete_actor"));

    Registry.RegisterTool(
        TEXT("level.set_transform"),
        TEXT("Set an actor's location / rotation / scale."),
        OliveLevelSchemas::SetTransform(),
        FOliveToolHandler::CreateRaw(this, &FOliveLevelToolHandlers::HandleSetTransform),
        {TEXT("level"), TEXT("write")},
        TEXT("level"));
    RegisteredToolNames.Add(TEXT("level.set_transform"));

    Registry.RegisterTool(
        TEXT("level.set_physics"),
        TEXT("Configure physics simulation, gravity, and mass on a primitive component of an actor."),
        OliveLevelSchemas::SetPhysics(),
        FOliveToolHandler::CreateRaw(this, &FOliveLevelToolHandlers::HandleSetPhysics),
        {TEXT("level"), TEXT("write")},
        TEXT("level"));
    RegisteredToolNames.Add(TEXT("level.set_physics"));

    Registry.RegisterTool(
        TEXT("level.apply_material"),
        TEXT("Apply a material asset to a specified slot on an actor's mesh component."),
        OliveLevelSchemas::ApplyMaterial(),
        FOliveToolHandler::CreateRaw(this, &FOliveLevelToolHandlers::HandleApplyMaterial),
        {TEXT("level"), TEXT("write")},
        TEXT("level"));
    RegisteredToolNames.Add(TEXT("level.apply_material"));

    Registry.RegisterTool(
        TEXT("level.get_actor_materials"),
        TEXT("Return the material paths for all mesh components of an actor."),
        OliveLevelSchemas::GetActorMaterials(),
        FOliveToolHandler::CreateRaw(this, &FOliveLevelToolHandlers::HandleGetActorMaterials),
        {TEXT("level"), TEXT("read")},
        TEXT("level"));
    RegisteredToolNames.Add(TEXT("level.get_actor_materials"));

    UE_LOG(LogOliveLevelTools, Log, TEXT("Registered %d level.* MCP tools"), RegisteredToolNames.Num());
}

void FOliveLevelToolHandlers::UnregisterAllTools()
{
    FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
    for (const FString& Name : RegisteredToolNames)
    {
        Registry.UnregisterTool(Name);
    }
    RegisteredToolNames.Reset();
}
