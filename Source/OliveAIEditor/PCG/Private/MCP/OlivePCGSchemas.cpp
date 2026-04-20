// Copyright Bode Software. All Rights Reserved.

#include "OlivePCGSchemas.h"
#include "OliveBlueprintSchemas.h"

namespace OlivePCGSchemas
{
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

	TSharedPtr<FJsonObject> PCGCreate()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path for the new PCG graph asset (e.g., /Game/PCG/MyGraph)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> PCGRead()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph to read")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> PCGAddNode()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph")));
		Props->SetObjectField(TEXT("settings_class"),
			OliveBlueprintSchemas::StringProp(TEXT("Settings class name (e.g., PCGSurfaceSamplerSettings, SurfaceSampler)")));
		Props->SetObjectField(TEXT("pos_x"),
			OliveBlueprintSchemas::IntProp(TEXT("Editor X position for the node")));
		Props->SetObjectField(TEXT("pos_y"),
			OliveBlueprintSchemas::IntProp(TEXT("Editor Y position for the node")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("settings_class") });
		return Schema;
	}

	TSharedPtr<FJsonObject> PCGRemoveNode()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph")));
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID to remove (cannot remove input/output)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("node_id") });
		return Schema;
	}

	TSharedPtr<FJsonObject> PCGConnect()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph")));
		Props->SetObjectField(TEXT("source_node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Source node ID (e.g., input, node_0)")));
		Props->SetObjectField(TEXT("source_pin"),
			OliveBlueprintSchemas::StringProp(TEXT("Source output pin name (e.g., Out)")));
		Props->SetObjectField(TEXT("target_node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Target node ID")));
		Props->SetObjectField(TEXT("target_pin"),
			OliveBlueprintSchemas::StringProp(TEXT("Target input pin name (e.g., In)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("source_node_id"), TEXT("source_pin"),
			TEXT("target_node_id"), TEXT("target_pin") });
		return Schema;
	}

	TSharedPtr<FJsonObject> PCGDisconnect()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph")));
		Props->SetObjectField(TEXT("source_node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Source node ID")));
		Props->SetObjectField(TEXT("source_pin"),
			OliveBlueprintSchemas::StringProp(TEXT("Source output pin name")));
		Props->SetObjectField(TEXT("target_node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Target node ID")));
		Props->SetObjectField(TEXT("target_pin"),
			OliveBlueprintSchemas::StringProp(TEXT("Target input pin name")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("source_node_id"), TEXT("source_pin"),
			TEXT("target_node_id"), TEXT("target_pin") });
		return Schema;
	}

	TSharedPtr<FJsonObject> PCGSetSettings()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph")));
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID of the target node")));
		Props->SetObjectField(TEXT("properties"),
			OliveBlueprintSchemas::ObjectProp(TEXT("Property values as key-value pairs (uses UE ImportText format)"), MakeProperties()));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("node_id"), TEXT("properties") });
		return Schema;
	}

	TSharedPtr<FJsonObject> PCGAddSubgraph()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph")));
		Props->SetObjectField(TEXT("subgraph_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph to reference as subgraph")));
		Props->SetObjectField(TEXT("pos_x"),
			OliveBlueprintSchemas::IntProp(TEXT("Editor X position")));
		Props->SetObjectField(TEXT("pos_y"),
			OliveBlueprintSchemas::IntProp(TEXT("Editor Y position")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("subgraph_path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> PCGExecute()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph to execute")));
		Props->SetObjectField(TEXT("timeout"),
			OliveBlueprintSchemas::IntProp(TEXT("Maximum execution time in seconds (default 30)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}
}
