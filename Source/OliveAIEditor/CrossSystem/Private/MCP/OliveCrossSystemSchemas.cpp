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

	TSharedPtr<FJsonObject> ProjectBatchWrite()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Target asset path — all operations must target this asset")));

		// Build the op item schema: { id?: string, tool: string, params: object }
		auto OpItemProps = MakeProperties();
		OpItemProps->SetObjectField(TEXT("id"),
			OliveBlueprintSchemas::StringProp(TEXT("Optional ID for this operation, used by later ops via ${id.field} template references")));
		OpItemProps->SetObjectField(TEXT("tool"),
			OliveBlueprintSchemas::StringProp(TEXT("Tool name to execute (e.g., blueprint.add_node, blueprint.connect_pins)")));

		auto ParamsObjSchema = MakeSchema(TEXT("object"));
		ParamsObjSchema->SetStringField(TEXT("description"), TEXT("Parameters for the tool call"));
		OpItemProps->SetObjectField(TEXT("params"), ParamsObjSchema);

		auto OpItemSchema = MakeSchema(TEXT("object"));
		OpItemSchema->SetObjectField(TEXT("properties"), OpItemProps);
		AddRequired(OpItemSchema, { TEXT("tool"), TEXT("params") });

		Props->SetObjectField(TEXT("ops"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Ordered array of operations to execute atomically"), OpItemSchema));

		Props->SetObjectField(TEXT("graph"),
			OliveBlueprintSchemas::StringProp(TEXT("Default graph name injected into ops missing a graph param")));
		Props->SetObjectField(TEXT("dry_run"),
			OliveBlueprintSchemas::BoolProp(TEXT("If true, validate and return normalized ops without executing"), false));
		Props->SetObjectField(TEXT("auto_compile"),
			OliveBlueprintSchemas::BoolProp(TEXT("Compile the Blueprint after all operations succeed"), true));
		Props->SetObjectField(TEXT("stop_on_error"),
			OliveBlueprintSchemas::BoolProp(TEXT("Stop execution on first operation failure"), true));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("ops") });
		return Schema;
	}

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

	TSharedPtr<FJsonObject> ProjectCreateAICharacter()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("name"),
			OliveBlueprintSchemas::StringProp(TEXT("Base name for the AI character (e.g., 'Enemy' creates BP_Enemy, BT_Enemy, BB_Enemy)")));
		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content directory for generated assets")));
		Props->SetObjectField(TEXT("parent_class"),
			OliveBlueprintSchemas::StringProp(TEXT("Parent class for the Blueprint (default: ACharacter)")));

		// Blackboard keys: array of {name, key_type} objects
		auto KeyItemProps = MakeProperties();
		KeyItemProps->SetObjectField(TEXT("name"),
			OliveBlueprintSchemas::StringProp(TEXT("Key name")));
		KeyItemProps->SetObjectField(TEXT("key_type"),
			OliveBlueprintSchemas::EnumProp(TEXT("Key type"),
				{ TEXT("bool"), TEXT("int"), TEXT("float"), TEXT("string"), TEXT("name"),
				  TEXT("vector"), TEXT("rotator"), TEXT("object"), TEXT("class") }));
		auto KeyItemSchema = MakeSchema(TEXT("object"));
		KeyItemSchema->SetObjectField(TEXT("properties"), KeyItemProps);
		AddRequired(KeyItemSchema, { TEXT("name"), TEXT("key_type") });

		Props->SetObjectField(TEXT("blackboard_keys"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Initial Blackboard keys"), KeyItemSchema));
		Props->SetObjectField(TEXT("behavior_tree_root"),
			OliveBlueprintSchemas::EnumProp(TEXT("Root composite type"),
				{ TEXT("Selector"), TEXT("Sequence") }));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("name"), TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> ProjectMoveToCpp()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("asset_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Blueprint asset path to analyze/migrate (e.g., /Game/BP_Enemy)")));
		Props->SetObjectField(TEXT("module_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Target C++ module name")));
		Props->SetObjectField(TEXT("target_class_name"),
			OliveBlueprintSchemas::StringProp(TEXT("New C++ class name without prefix")));
		Props->SetObjectField(TEXT("parent_class"),
			OliveBlueprintSchemas::StringProp(TEXT("Optional C++ parent class override")));
		Props->SetObjectField(TEXT("create_wrapper_blueprint"),
			OliveBlueprintSchemas::BoolProp(TEXT("Whether to keep/create a Blueprint wrapper"), true));
		Props->SetObjectField(TEXT("compile_after"),
			OliveBlueprintSchemas::BoolProp(TEXT("Trigger compile after scaffold generation"), false));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("asset_path"), TEXT("module_name"), TEXT("target_class_name") });
		return Schema;
	}

	// ============================================================================
	// Snapshot Operations
	// ============================================================================

	TSharedPtr<FJsonObject> ProjectSnapshot()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("name"),
			OliveBlueprintSchemas::StringProp(TEXT("Name for this snapshot")));
		Props->SetObjectField(TEXT("paths"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Asset paths to snapshot"),
				OliveBlueprintSchemas::StringProp(TEXT("Asset path"))));
		Props->SetObjectField(TEXT("description"),
			OliveBlueprintSchemas::StringProp(TEXT("Optional description of why this snapshot was taken")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("name"), TEXT("paths") });
		return Schema;
	}

	TSharedPtr<FJsonObject> ProjectListSnapshots()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("asset_filter"),
			OliveBlueprintSchemas::StringProp(TEXT("Filter snapshots containing this asset path")));

		Schema->SetObjectField(TEXT("properties"), Props);
		return Schema;
	}

	TSharedPtr<FJsonObject> ProjectRollback()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("snapshot_id"),
			OliveBlueprintSchemas::StringProp(TEXT("ID of the snapshot to restore from")));
		Props->SetObjectField(TEXT("paths"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Specific asset paths to rollback (empty = all in snapshot)"),
				OliveBlueprintSchemas::StringProp(TEXT("Asset path"))));
		Props->SetObjectField(TEXT("preview_only"),
			OliveBlueprintSchemas::BoolProp(TEXT("If true, only return rollback plan and confirmation token"), true));
		Props->SetObjectField(TEXT("confirmation_token"),
			OliveBlueprintSchemas::StringProp(TEXT("Confirmation token returned by preview call")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("snapshot_id") });
		return Schema;
	}

	TSharedPtr<FJsonObject> ProjectDiff()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("snapshot_id"),
			OliveBlueprintSchemas::StringProp(TEXT("ID of the snapshot to compare against current state")));
		Props->SetObjectField(TEXT("paths"),
			OliveBlueprintSchemas::ArrayProp(TEXT("Specific asset paths to diff (empty = all in snapshot)"),
				OliveBlueprintSchemas::StringProp(TEXT("Asset path"))));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("snapshot_id") });
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
}
