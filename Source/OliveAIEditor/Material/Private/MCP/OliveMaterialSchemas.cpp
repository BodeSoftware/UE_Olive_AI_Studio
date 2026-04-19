// Copyright Bode Software. All Rights Reserved.

#include "MCP/OliveMaterialSchemas.h"

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

    TSharedPtr<FJsonObject> Vec4(const FString& Desc)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), TEXT("array"));
        O->SetStringField(TEXT("description"), Desc);
        TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
        Items->SetStringField(TEXT("type"), TEXT("number"));
        O->SetObjectField(TEXT("items"), Items);
        O->SetNumberField(TEXT("minItems"), 4);
        O->SetNumberField(TEXT("maxItems"), 4);
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

TSharedPtr<FJsonObject> OliveMaterialSchemas::List()
{
    return MakeObjectSchema(
        {
            {TEXT("search_path"),              Str(TEXT("Optional /Game-relative package path to search within (e.g. /Game/Materials). Empty = /Game root."))},
            {TEXT("include_engine_materials"), Bool(TEXT("If true, also include materials under /Engine. Ignored when search_path is set."))}
        },
        {}
    );
}

TSharedPtr<FJsonObject> OliveMaterialSchemas::Read()
{
    return MakeObjectSchema(
        {
            {TEXT("path"), Str(TEXT("Full asset path of the material or material instance (e.g. /Game/Materials/M_Brick.M_Brick)."))}
        },
        {TEXT("path")}
    );
}

TSharedPtr<FJsonObject> OliveMaterialSchemas::ApplyToComponent()
{
    return MakeObjectSchema(
        {
            {TEXT("blueprint_path"), Str(TEXT("Full asset path of the Blueprint containing the component (e.g. /Game/BP_Wall.BP_Wall)."))},
            {TEXT("component_name"), Str(TEXT("Name of the mesh component inside the Blueprint's SimpleConstructionScript."))},
            {TEXT("slot"),           Int(TEXT("Material slot index (0-based)."))},
            {TEXT("material_path"),  Str(TEXT("Full asset path of the material to assign (e.g. /Game/Materials/M_Brick.M_Brick)."))}
        },
        {TEXT("blueprint_path"), TEXT("component_name"), TEXT("slot"), TEXT("material_path")}
    );
}

TSharedPtr<FJsonObject> OliveMaterialSchemas::SetParameterColor()
{
    return MakeObjectSchema(
        {
            {TEXT("material_instance_path"), Str(TEXT("Full asset path of the UMaterialInstanceConstant to modify."))},
            {TEXT("parameter_name"),         Str(TEXT("Name of the vector parameter to set (must exist on the instance's parent material)."))},
            {TEXT("color"),                  Vec4(TEXT("Linear color as [R, G, B, A]; components are typically in the 0..1 range."))}
        },
        {TEXT("material_instance_path"), TEXT("parameter_name"), TEXT("color")}
    );
}

TSharedPtr<FJsonObject> OliveMaterialSchemas::CreateInstance()
{
    return MakeObjectSchema(
        {
            {TEXT("new_instance_path"),     Str(TEXT("Target asset path for the new material instance (e.g. /Game/Materials/MI_Brick_Red.MI_Brick_Red)."))},
            {TEXT("parent_material_path"),  Str(TEXT("Full asset path of the parent material or material instance to base the new instance on."))}
        },
        {TEXT("new_instance_path"), TEXT("parent_material_path")}
    );
}
