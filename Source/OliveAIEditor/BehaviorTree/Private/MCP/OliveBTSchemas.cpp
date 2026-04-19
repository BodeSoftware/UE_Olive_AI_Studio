// Copyright Bode Software. All Rights Reserved.

#include "OliveBTSchemas.h"
#include "OliveBlueprintSchemas.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace OliveBTSchemas
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
	// Blackboard Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> BlackboardCreate()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path for the new Blackboard asset (e.g., /Game/AI/BB_Enemy)")));
		Props->SetObjectField(TEXT("parent"),
			OliveBlueprintSchemas::StringProp(TEXT("Optional parent Blackboard path for key inheritance")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BlackboardRead()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Blackboard to read")));
		Props->SetObjectField(TEXT("include_inherited"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include keys inherited from parent Blackboards"), false));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BlackboardAddKey()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Blackboard")));
		Props->SetObjectField(TEXT("name"),
			OliveBlueprintSchemas::StringProp(TEXT("Name for the key. If it already exists, the key is modified instead of created (upsert).")));
		Props->SetObjectField(TEXT("key_type"),
			OliveBlueprintSchemas::EnumProp(TEXT("Type of the Blackboard key (required for new keys, ignored on modify)"),
				{ TEXT("bool"), TEXT("int"), TEXT("float"), TEXT("string"), TEXT("name"),
				  TEXT("vector"), TEXT("rotator"), TEXT("enum"), TEXT("object"), TEXT("class") }));
		Props->SetObjectField(TEXT("base_class"),
			OliveBlueprintSchemas::StringProp(TEXT("For Object/Class keys: the base class name (e.g., Actor, Pawn)")));
		Props->SetObjectField(TEXT("enum_type"),
			OliveBlueprintSchemas::StringProp(TEXT("For Enum keys: the enum type name")));
		Props->SetObjectField(TEXT("instance_synced"),
			OliveBlueprintSchemas::BoolProp(TEXT("Whether this key is instance-synced"), false));
		Props->SetObjectField(TEXT("description"),
			OliveBlueprintSchemas::StringProp(TEXT("Optional description/tooltip for the key")));
		Props->SetObjectField(TEXT("new_name"),
			OliveBlueprintSchemas::StringProp(TEXT("If the key exists, rename it to this value (upsert modify only)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("name") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BlackboardRemoveKey()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Blackboard")));
		Props->SetObjectField(TEXT("name"),
			OliveBlueprintSchemas::StringProp(TEXT("Name of the key to remove")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("name") });
		return Schema;
	}

	// Removed in AI Freedom Phase 2 — blackboard.modify_key merged into blackboard.add_key (upsert)

	TSharedPtr<FJsonObject> BlackboardModify()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Blackboard (or the target path when action='create')")));
		Props->SetObjectField(TEXT("action"),
			OliveBlueprintSchemas::EnumProp(TEXT("Operation to perform"),
				{ TEXT("create"), TEXT("read"), TEXT("add_key"),
				  TEXT("modify_key"), TEXT("remove_key"), TEXT("set_parent") }));

		// Fields used by create / set_parent (parent Blackboard reference)
		Props->SetObjectField(TEXT("parent"),
			OliveBlueprintSchemas::StringProp(TEXT("Parent Blackboard path (action='create'). Optional.")));
		Props->SetObjectField(TEXT("parent_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Parent Blackboard path (action='set_parent'). Required for set_parent.")));

		// Fields used by read
		Props->SetObjectField(TEXT("include_inherited"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include keys inherited from parent Blackboards (action='read')"), false));

		// Fields used by add_key / modify_key / remove_key
		Props->SetObjectField(TEXT("name"),
			OliveBlueprintSchemas::StringProp(TEXT("Key name (required for add_key/modify_key/remove_key)")));
		Props->SetObjectField(TEXT("new_name"),
			OliveBlueprintSchemas::StringProp(TEXT("Rename target (action='modify_key' or upsert on add_key)")));
		Props->SetObjectField(TEXT("key_type"),
			OliveBlueprintSchemas::EnumProp(TEXT("Key type (action='add_key', required for new keys)"),
				{ TEXT("bool"), TEXT("int"), TEXT("float"), TEXT("string"), TEXT("name"),
				  TEXT("vector"), TEXT("rotator"), TEXT("enum"), TEXT("object"), TEXT("class") }));
		Props->SetObjectField(TEXT("base_class"),
			OliveBlueprintSchemas::StringProp(TEXT("For Object/Class keys: base class name (action='add_key')")));
		Props->SetObjectField(TEXT("enum_type"),
			OliveBlueprintSchemas::StringProp(TEXT("For Enum keys: enum type name (action='add_key')")));
		Props->SetObjectField(TEXT("instance_synced"),
			OliveBlueprintSchemas::BoolProp(TEXT("Instance-synced flag (action='add_key' or 'modify_key')"), false));
		Props->SetObjectField(TEXT("description"),
			OliveBlueprintSchemas::StringProp(TEXT("Key description/tooltip (action='add_key' or 'modify_key')")));

		Schema->SetStringField(TEXT("description"),
			TEXT("Modify a Blackboard asset. Dispatches on 'action' to create/read/add_key/modify_key/"
				"remove_key/set_parent. Legacy blackboard.{create,read,add_key,modify_key,remove_key,"
				"set_parent} tool names are aliases that pre-fill the 'action' field."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("action") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BlackboardSetParent()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Blackboard")));
		Props->SetObjectField(TEXT("parent_path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the parent Blackboard")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("parent_path") });
		return Schema;
	}

	// ============================================================================
	// Behavior Tree Schemas
	// ============================================================================

	TSharedPtr<FJsonObject> BehaviorTreeCreate()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Content path for the new Behavior Tree asset")));
		Props->SetObjectField(TEXT("blackboard"),
			OliveBlueprintSchemas::StringProp(TEXT("Optional Blackboard asset path to associate")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BehaviorTreeRead()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree to read")));
		Props->SetObjectField(TEXT("include_blackboard"),
			OliveBlueprintSchemas::BoolProp(TEXT("Include associated Blackboard data in response"), true));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BehaviorTreeSetBlackboard()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("blackboard"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Blackboard to associate")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("blackboard") });
		return Schema;
	}

	// Removed in AI Freedom Phase 2 — behaviortree.add_composite, add_task, add_decorator, add_service
	// consolidated into behaviortree.add_node

	TSharedPtr<FJsonObject> BehaviorTreeAddNode()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("node_kind"),
			OliveBlueprintSchemas::EnumProp(TEXT("Kind of node to add"),
				{ TEXT("composite"), TEXT("task"), TEXT("decorator"), TEXT("service") }));

		// composite params
		Props->SetObjectField(TEXT("composite_type"),
			OliveBlueprintSchemas::EnumProp(TEXT("For composite: Selector, Sequence, or SimpleParallel"),
				{ TEXT("Selector"), TEXT("Sequence"), TEXT("SimpleParallel") }));

		// task/decorator/service class param (unified as 'class')
		Props->SetObjectField(TEXT("class"),
			OliveBlueprintSchemas::StringProp(TEXT("Class name for task/decorator/service (e.g., BTTask_MoveTo, BTDecorator_Blackboard)")));

		// parent_node_id: required for composite and task
		Props->SetObjectField(TEXT("parent_node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID of the parent composite. Required for composite and task. Use 'root' for root.")));

		// node_id: required for decorator and service (the node to attach to)
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID to attach decorator/service to. Required for decorator and service.")));

		// child_index: optional for composite and task
		Props->SetObjectField(TEXT("child_index"),
			OliveBlueprintSchemas::IntProp(TEXT("Insert position among siblings (-1 for end). For composite and task.")));

		// properties: optional for task/decorator/service
		Props->SetObjectField(TEXT("properties"),
			OliveBlueprintSchemas::ObjectProp(TEXT("Initial property values as key-value pairs (for task/decorator/service)"), MakeProperties()));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("node_kind") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BehaviorTreeAdd()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("node_type"),
			OliveBlueprintSchemas::EnumProp(TEXT("Kind of node to add"),
				{ TEXT("composite"), TEXT("task"), TEXT("decorator"), TEXT("service"), TEXT("node") }));

		// Passthrough fields (shape matches BehaviorTreeAddNode)
		Props->SetObjectField(TEXT("node_kind"),
			OliveBlueprintSchemas::StringProp(TEXT("Legacy alias for 'node_type'. 'node_type' takes precedence when both are set.")));
		Props->SetObjectField(TEXT("composite_type"),
			OliveBlueprintSchemas::EnumProp(TEXT("For composite: Selector, Sequence, or SimpleParallel"),
				{ TEXT("Selector"), TEXT("Sequence"), TEXT("SimpleParallel") }));
		Props->SetObjectField(TEXT("class"),
			OliveBlueprintSchemas::StringProp(TEXT("Class name for task/decorator/service (e.g., BTTask_MoveTo, BTDecorator_Blackboard)")));
		Props->SetObjectField(TEXT("parent_node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID of parent composite. Required for composite/task. Use 'root' for root.")));
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID to attach decorator/service to.")));
		Props->SetObjectField(TEXT("child_index"),
			OliveBlueprintSchemas::IntProp(TEXT("Insert position among siblings (-1 for end). For composite/task.")));
		Props->SetObjectField(TEXT("properties"),
			OliveBlueprintSchemas::ObjectProp(TEXT("Initial property values as key-value pairs (for task/decorator/service)"), MakeProperties()));

		Schema->SetStringField(TEXT("description"),
			TEXT("Add a node to a Behavior Tree. Dispatches on 'node_type' (composite|task|decorator|"
				"service|node) to the matching internal handler. Legacy behaviortree.add_{composite,"
				"task,decorator,service,node} tool names are aliases that pre-fill 'node_type'."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("node_type") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BehaviorTreeModify()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("entity"),
			OliveBlueprintSchemas::EnumProp(TEXT("What to modify"),
				{ TEXT("node"), TEXT("decorator"), TEXT("blackboard_ref") }));

		// Fields used by entity='node' / entity='decorator' (set_node_property shape)
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID of the target node (entity='node' or 'decorator')")));
		Props->SetObjectField(TEXT("property"),
			OliveBlueprintSchemas::StringProp(TEXT("UPROPERTY name to set (entity='node' or 'decorator')")));
		Props->SetObjectField(TEXT("value"),
			OliveBlueprintSchemas::StringProp(TEXT("Value as string in UE ImportText format")));

		// Fields used by entity='blueprint'/'blackboard_ref' — sets the BT's Blackboard
		Props->SetObjectField(TEXT("blackboard"),
			OliveBlueprintSchemas::StringProp(TEXT("Blackboard asset path (entity='blackboard_ref')")));

		Schema->SetStringField(TEXT("description"),
			TEXT("Modify a Behavior Tree. Dispatches on 'entity' (node|decorator|blackboard_ref) to "
				"the matching internal handler. Legacy behaviortree.{modify_node,set_node_property,"
				"set_decorator,set_blackboard} tool names are aliases that pre-fill 'entity'."));
		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("entity") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BehaviorTreeRemoveNode()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID to remove")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("node_id") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BehaviorTreeMoveNode()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID to move")));
		Props->SetObjectField(TEXT("new_parent_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID of the new parent composite")));
		Props->SetObjectField(TEXT("child_index"),
			OliveBlueprintSchemas::IntProp(TEXT("Insert position in new parent (-1 for end)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("node_id"), TEXT("new_parent_id") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BehaviorTreeSetNodeProperty()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID of the target node")));
		Props->SetObjectField(TEXT("property"),
			OliveBlueprintSchemas::StringProp(TEXT("UPROPERTY name to set")));
		Props->SetObjectField(TEXT("value"),
			OliveBlueprintSchemas::StringProp(TEXT("Value as string (uses UE ImportText format)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("node_id"), TEXT("property"), TEXT("value") });
		return Schema;
	}
}
