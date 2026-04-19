// Copyright Bode Software. All Rights Reserved.

#include "MCP/OliveLevelSchemas.h"

namespace
{
    TSharedPtr<FJsonObject> Str(const FString& Desc)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), TEXT("string"));
        O->SetStringField(TEXT("description"), Desc);
        return O;
    }

    TSharedPtr<FJsonObject> Num(const FString& Desc)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), TEXT("number"));
        O->SetStringField(TEXT("description"), Desc);
        return O;
    }

    TSharedPtr<FJsonObject> Int(const FString& Desc)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), TEXT("integer"));
        O->SetStringField(TEXT("description"), Desc);
        return O;
    }

    TSharedPtr<FJsonObject> Bool(const FString& Desc)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), TEXT("boolean"));
        O->SetStringField(TEXT("description"), Desc);
        return O;
    }

    TSharedPtr<FJsonObject> Vec3(const FString& Desc)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), TEXT("array"));
        O->SetStringField(TEXT("description"), Desc);
        TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
        Items->SetStringField(TEXT("type"), TEXT("number"));
        O->SetObjectField(TEXT("items"), Items);
        O->SetNumberField(TEXT("minItems"), 3);
        O->SetNumberField(TEXT("maxItems"), 3);
        return O;
    }

    TSharedPtr<FJsonObject> MakeObjectSchema(
        const TMap<FString, TSharedPtr<FJsonObject>>& Properties,
        const TArray<FString>& Required)
    {
        TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("object"));

        TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
        for (const auto& P : Properties)
        {
            Props->SetObjectField(P.Key, P.Value);
        }
        Schema->SetObjectField(TEXT("properties"), Props);

        TArray<TSharedPtr<FJsonValue>> Req;
        for (const FString& R : Required)
        {
            Req.Add(MakeShared<FJsonValueString>(R));
        }
        Schema->SetArrayField(TEXT("required"), Req);
        return Schema;
    }
} // namespace

TSharedPtr<FJsonObject> OliveLevelSchemas::ListActors()
{
    return MakeObjectSchema({}, {});
}

TSharedPtr<FJsonObject> OliveLevelSchemas::FindActors()
{
    return MakeObjectSchema(
        {
            {TEXT("pattern"),    Str(TEXT("Substring to match against actor label/name (case-insensitive). Empty = match all."))},
            {TEXT("class_path"), Str(TEXT("Full UClass path to filter by (e.g. /Script/Engine.StaticMeshActor). Empty = all classes."))}
        },
        {}
    );
}

TSharedPtr<FJsonObject> OliveLevelSchemas::SpawnActor()
{
    return MakeObjectSchema(
        {
            {TEXT("class_path"), Str(TEXT("Full UClass path to spawn (e.g. /Game/BP_MyActor.BP_MyActor_C or /Script/Engine.StaticMeshActor)."))},
            {TEXT("label"),      Str(TEXT("Optional actor label. Defaults to class name if omitted."))},
            {TEXT("location"),   Vec3(TEXT("World-space spawn location [X, Y, Z] in centimetres. Defaults to origin."))},
            {TEXT("rotation"),   Vec3(TEXT("Spawn rotation [Pitch, Yaw, Roll] in degrees. Defaults to zero."))},
            {TEXT("scale"),      Vec3(TEXT("Spawn scale [SX, SY, SZ]. Defaults to (1, 1, 1)."))}
        },
        {TEXT("class_path")}
    );
}

TSharedPtr<FJsonObject> OliveLevelSchemas::DeleteActor()
{
    return MakeObjectSchema(
        {
            {TEXT("name"), Str(TEXT("Actor label or name to delete."))}
        },
        {TEXT("name")}
    );
}

TSharedPtr<FJsonObject> OliveLevelSchemas::SetTransform()
{
    return MakeObjectSchema(
        {
            {TEXT("name"),     Str(TEXT("Actor label or name to transform."))},
            {TEXT("location"), Vec3(TEXT("New world-space location [X, Y, Z] in centimetres. Omit to leave unchanged."))},
            {TEXT("rotation"), Vec3(TEXT("New rotation [Pitch, Yaw, Roll] in degrees. Omit to leave unchanged."))},
            {TEXT("scale"),    Vec3(TEXT("New scale [SX, SY, SZ]. Omit to leave unchanged."))}
        },
        {TEXT("name")}
    );
}

TSharedPtr<FJsonObject> OliveLevelSchemas::SetPhysics()
{
    return MakeObjectSchema(
        {
            {TEXT("name"),             Str(TEXT("Actor label or name."))},
            {TEXT("component_name"),   Str(TEXT("Optional component name. Defaults to the first primitive component found."))},
            {TEXT("simulate_physics"), Bool(TEXT("Enable or disable physics simulation on the component."))},
            {TEXT("enable_gravity"),   Bool(TEXT("Enable or disable gravity on the component."))},
            {TEXT("mass"),             Num(TEXT("Override mass in kilograms. Applied only if simulate_physics is true."))}
        },
        {TEXT("name")}
    );
}

TSharedPtr<FJsonObject> OliveLevelSchemas::ApplyMaterial()
{
    return MakeObjectSchema(
        {
            {TEXT("name"),           Str(TEXT("Actor label or name."))},
            {TEXT("component_name"), Str(TEXT("Optional mesh component name. Defaults to the first static or skeletal mesh component."))},
            {TEXT("slot"),           Int(TEXT("Material slot index (0-based)."))},
            {TEXT("material_path"),  Str(TEXT("Full asset path of the material to apply (e.g. /Game/Materials/M_Brick.M_Brick)."))}
        },
        {TEXT("name"), TEXT("material_path")}
    );
}

TSharedPtr<FJsonObject> OliveLevelSchemas::GetActorMaterials()
{
    return MakeObjectSchema(
        {
            {TEXT("name"), Str(TEXT("Actor label or name whose material slots to query."))}
        },
        {TEXT("name")}
    );
}
