// Copyright Bode Software. All Rights Reserved.

#include "OliveCrossSystemSchemas.h"
#include "OliveBlueprintSchemas.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace OliveCrossSystemSchemas
{
	// Local helpers matching OliveBlueprintSchemas pattern
	static TSharedPtr<FJsonObject> MakeSchema(const FString& Type)
	{
		TSharedPtr<FJsonObject> Schema = MakeShareable(new FJsonObject());
		Schema->SetStringField(TEXT("type"), Type);
		return Schema;
	}

	static TSharedPtr<FJsonObject> MakeProperties()
	{
		return MakeShareable(new FJsonObject());
	}

	static void AddRequired(TSharedPtr<FJsonObject> Schema, const TArray<FString>& RequiredFields)
	{
		TArray<TSharedPtr<FJsonValue>> RequiredArray;
		for (const FString& Field : RequiredFields)
		{
			RequiredArray.Add(MakeShareable(new FJsonValueString(Field)));
		}
		Schema->SetArrayField(TEXT("required"), RequiredArray);
	}

	// ============================================================================
	// Bulk Operations
	// ============================================================================

	TSharedPtr<FJsonObject> ProjectBulkRead()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("paths"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Array of asset paths to read (max 20)"),
				OliveBlueprintSchemas::StringProp(TEXT("Asset path"))));
		Props->SetObjectField(TEXT("read_mode"),
			OliveBlueprintSchemas::EnumProp(TEXT("Level of detail for each asset"),
				{ TEXT("summary"), TEXT("full") }));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("paths") });
		return Schema;
	}

	TSharedPtr<FJsonObject> ProjectImplementInterface()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("paths"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Array of Blueprint asset paths to add the interface to"),
				OliveBlueprintSchemas::StringProp(TEXT("Asset path"))));
		Props->SetObjectField(TEXT("interface"),
			OliveBlueprintSchemas::StringProp(TEXT("Interface class name to implement")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("paths"), TEXT("interface") });
		return Schema;
	}

	TSharedPtr<FJsonObject> ProjectRefactorRename()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("asset_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Path of asset to rename")));
		Props->SetObjectField(TEXT("new_name"),
			OliveBlueprintSchemas::StringProp(TEXT("New name for the asset")));
		Props->SetObjectField(TEXT("update_references"),
			OliveBlueprintSchemas::BoolProp(TEXT("Update all references to this asset"), true));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("asset_path"), TEXT("new_name") });
		return Schema;
	}


	// ============================================================================
	// Index / Context Operations
	// ============================================================================

	TSharedPtr<FJsonObject> ProjectIndexBuild()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("force"),
			OliveBlueprintSchemas::BoolProp(TEXT("Force re-export even if the index is not stale"), false));

		Schema->SetObjectField(TEXT("properties"), Props);
		return Schema;
	}

	TSharedPtr<FJsonObject> ProjectIndexStatus()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();
		Schema->SetObjectField(TEXT("properties"), Props);
		return Schema;
	}

	TSharedPtr<FJsonObject> ProjectGetRelevantContext()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("query"),
			OliveBlueprintSchemas::StringProp(TEXT("Search query to find relevant assets")));
		Props->SetObjectField(TEXT("max_assets"),
			OliveBlueprintSchemas::IntProp(TEXT("Maximum number of assets to return"), 1, 50));
		Props->SetObjectField(TEXT("kinds"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Filter by asset kind (e.g., Blueprint, BehaviorTree, PCG, Material)"),
				OliveBlueprintSchemas::StringProp(TEXT("Asset kind"))));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("query") });
		return Schema;
	}

	// ============================================================================
	// Recipe Operations
	// ============================================================================

	TSharedPtr<FJsonObject> RecipeGetRecipe()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("query"),
			OliveBlueprintSchemas::StringProp(TEXT("Free-text search query (e.g. 'spawn actor transform', 'variable type object class', 'function graph entry')")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("query") });
		return Schema;
	}

}
