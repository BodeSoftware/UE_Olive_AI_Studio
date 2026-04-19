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

	// =======================================================================
	// P5 consolidated schemas
	// =======================================================================

	TSharedPtr<FJsonObject> PCGAdd()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph")));
		Props->SetObjectField(TEXT("node_kind"),
			OliveBlueprintSchemas::EnumProp(TEXT("What to add. 'node' (default) creates a settings-backed node, 'subgraph' references another PCG graph."),
				{ TEXT("node"), TEXT("subgraph") }));

		// Fields for node_kind='node'
		Props->SetObjectField(TEXT("settings_class"),
			OliveBlueprintSchemas::StringProp(TEXT("Settings class name for node_kind='node' (e.g., PCGSurfaceSamplerSettings, SurfaceSampler)")));

		// Fields for node_kind='subgraph'
		Props->SetObjectField(TEXT("subgraph_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph to reference (node_kind='subgraph')")));

		// Position (shared)
		Props->SetObjectField(TEXT("pos_x"),
			OliveBlueprintSchemas::IntProp(TEXT("Editor X position for the node")));
		Props->SetObjectField(TEXT("pos_y"),
			OliveBlueprintSchemas::IntProp(TEXT("Editor Y position for the node")));

		Schema->SetStringField(TEXT("description"),
			TEXT("Add a node to a PCG graph. Dispatches on 'node_kind' (node|subgraph). Legacy "
				"pcg.add_node and pcg.add_subgraph are aliases that pre-fill 'node_kind'."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> PCGModify()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the PCG graph")));
		Props->SetObjectField(TEXT("entity"),
			OliveBlueprintSchemas::EnumProp(TEXT("What to modify. 'node' changes node-level attributes; 'settings' sets settings class properties via reflection."),
				{ TEXT("node"), TEXT("settings") }));

		// Shared
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID of the target node. Use pcg.read to see node IDs.")));

		// Fields for entity='settings' (reuses HandleSetSettings shape)
		Props->SetObjectField(TEXT("properties"),
			OliveBlueprintSchemas::ObjectProp(TEXT("Property values as key-value pairs (entity='settings'). Uses UE ImportText format."), MakeProperties()));

		Schema->SetStringField(TEXT("description"),
			TEXT("Modify a PCG node. Dispatches on 'entity' (node|settings). Legacy pcg.modify_node "
				"and pcg.set_settings are aliases that pre-fill 'entity'."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("entity") });
		return Schema;
	}

	TSharedPtr<FJsonObject> PCGConnectUnified()
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
		Props->SetObjectField(TEXT("break"),
			OliveBlueprintSchemas::BoolProp(TEXT("If true, disconnect (break) the edge instead of creating one. Default false."), false));

		Schema->SetStringField(TEXT("description"),
			TEXT("Connect or disconnect two pins in a PCG graph. Set 'break':true to disconnect. "
				"Legacy pcg.connect_pins is a pass-through alias; pcg.disconnect is an alias that "
				"pre-fills 'break':true."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("source_node_id"), TEXT("source_pin"),
			TEXT("target_node_id"), TEXT("target_pin") });
		return Schema;
	}
}
