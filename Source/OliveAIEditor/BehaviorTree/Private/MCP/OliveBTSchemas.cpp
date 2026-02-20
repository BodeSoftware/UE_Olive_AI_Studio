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
			OliveBlueprintSchemas::StringProp(TEXT("Name for the new key")));
		Props->SetObjectField(TEXT("key_type"),
			OliveBlueprintSchemas::EnumProp(TEXT("Type of the Blackboard key"),
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

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("name"), TEXT("key_type") });
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

	TSharedPtr<FJsonObject> BlackboardModifyKey()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Blackboard")));
		Props->SetObjectField(TEXT("name"),
			OliveBlueprintSchemas::StringProp(TEXT("Current name of the key to modify")));
		Props->SetObjectField(TEXT("new_name"),
			OliveBlueprintSchemas::StringProp(TEXT("New name for the key (omit to keep current)")));
		Props->SetObjectField(TEXT("instance_synced"),
			OliveBlueprintSchemas::BoolProp(TEXT("New instance-synced value"), false));
		Props->SetObjectField(TEXT("description"),
			OliveBlueprintSchemas::StringProp(TEXT("New description (omit to keep current)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("name") });
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

	TSharedPtr<FJsonObject> BehaviorTreeAddComposite()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("parent_node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID of the parent composite (e.g., node_0)")));
		Props->SetObjectField(TEXT("composite_type"),
			OliveBlueprintSchemas::EnumProp(TEXT("Type of composite node"),
				{ TEXT("Selector"), TEXT("Sequence"), TEXT("SimpleParallel") }));
		Props->SetObjectField(TEXT("child_index"),
			OliveBlueprintSchemas::IntProp(TEXT("Insert position among siblings (0-based, -1 for end)")));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("parent_node_id"), TEXT("composite_type") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BehaviorTreeAddTask()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("parent_node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID of the parent composite")));
		Props->SetObjectField(TEXT("task_class"),
			OliveBlueprintSchemas::StringProp(TEXT("Task class name (e.g., BTTask_MoveTo, BTTask_Wait)")));
		Props->SetObjectField(TEXT("child_index"),
			OliveBlueprintSchemas::IntProp(TEXT("Insert position among siblings (-1 for end)")));
		Props->SetObjectField(TEXT("properties"),
			OliveBlueprintSchemas::ObjectProp(TEXT("Initial property values as key-value pairs"), MakeProperties()));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("parent_node_id"), TEXT("task_class") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BehaviorTreeAddDecorator()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID to attach the decorator to")));
		Props->SetObjectField(TEXT("decorator_class"),
			OliveBlueprintSchemas::StringProp(TEXT("Decorator class name (e.g., BTDecorator_Blackboard)")));
		Props->SetObjectField(TEXT("properties"),
			OliveBlueprintSchemas::ObjectProp(TEXT("Initial property values"), MakeProperties()));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("node_id"), TEXT("decorator_class") });
		return Schema;
	}

	TSharedPtr<FJsonObject> BehaviorTreeAddService()
	{
		TSharedPtr<FJsonObject> Schema = MakeSchema(TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeProperties();

		Props->SetObjectField(TEXT("path"),
			OliveBlueprintSchemas::StringProp(TEXT("Asset path of the Behavior Tree")));
		Props->SetObjectField(TEXT("node_id"),
			OliveBlueprintSchemas::StringProp(TEXT("Node ID to attach the service to")));
		Props->SetObjectField(TEXT("service_class"),
			OliveBlueprintSchemas::StringProp(TEXT("Service class name (e.g., BTService_DefaultFocus)")));
		Props->SetObjectField(TEXT("properties"),
			OliveBlueprintSchemas::ObjectProp(TEXT("Initial property values"), MakeProperties()));

		Schema->SetObjectField(TEXT("properties"), Props);
		AddRequired(Schema, { TEXT("path"), TEXT("node_id"), TEXT("service_class") });
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
