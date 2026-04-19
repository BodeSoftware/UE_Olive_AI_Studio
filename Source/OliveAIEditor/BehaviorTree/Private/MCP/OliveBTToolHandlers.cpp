// Copyright Bode Software. All Rights Reserved.

#include "OliveBTToolHandlers.h"
#include "OliveBTSchemas.h"
#include "OliveBlackboardReader.h"
#include "OliveBlackboardWriter.h"
#include "OliveBehaviorTreeReader.h"
#include "OliveBehaviorTreeWriter.h"
#include "OliveBTNodeCatalog.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOliveBTTools);

FOliveBTToolHandlers& FOliveBTToolHandlers::Get()
{
	static FOliveBTToolHandlers Instance;
	return Instance;
}

void FOliveBTToolHandlers::RegisterAllTools()
{
	UE_LOG(LogOliveBTTools, Log, TEXT("Registering BT/BB MCP tools..."));

	RegisterBlackboardTools();
	RegisterBehaviorTreeTools();

	UE_LOG(LogOliveBTTools, Log, TEXT("Registered %d BT/BB MCP tools"), RegisteredToolNames.Num());
}

void FOliveBTToolHandlers::UnregisterAllTools()
{
	UE_LOG(LogOliveBTTools, Log, TEXT("Unregistering BT/BB MCP tools..."));

	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
	for (const FString& ToolName : RegisteredToolNames)
	{
		Registry.UnregisterTool(ToolName);
	}

	RegisteredToolNames.Empty();
	UE_LOG(LogOliveBTTools, Log, TEXT("BT/BB MCP tools unregistered"));
}

void FOliveBTToolHandlers::RegisterBlackboardTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	// P5 consolidation: all blackboard operations flow through blackboard.modify,
	// which dispatches on 'action'. Legacy tool names (blackboard.create,
	// blackboard.read, blackboard.add_key, blackboard.modify_key,
	// blackboard.remove_key, blackboard.set_parent) continue to work as aliases
	// that pre-fill the 'action' parameter. See OliveToolRegistry::GetToolAliases().
	Registry.RegisterTool(
		TEXT("blackboard.modify"),
		TEXT("Modify a Blackboard asset. Dispatches on 'action' (create|read|add_key|modify_key|"
			"remove_key|set_parent) to the matching internal handler. Legacy blackboard.* tool names "
			"are aliases that pre-fill 'action'."),
		OliveBTSchemas::BlackboardModify(),
		FOliveToolHandler::CreateRaw(this, &FOliveBTToolHandlers::HandleBlackboardModify),
		{TEXT("blackboard"), TEXT("write"), TEXT("read")},
		TEXT("blackboard")
	);
	RegisteredToolNames.Add(TEXT("blackboard.modify"));
}

void FOliveBTToolHandlers::RegisterBehaviorTreeTools()
{
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("behaviortree.create"),
		TEXT("Create a new Behavior Tree asset with optional Blackboard association"),
		OliveBTSchemas::BehaviorTreeCreate(),
		FOliveToolHandler::CreateRaw(this, &FOliveBTToolHandlers::HandleBehaviorTreeCreate),
		{TEXT("behaviortree"), TEXT("write")},
		TEXT("behaviortree")
	);
	RegisteredToolNames.Add(TEXT("behaviortree.create"));

	Registry.RegisterTool(
		TEXT("behaviortree.read"),
		TEXT("Read a Behavior Tree as structured IR data with optional Blackboard"),
		OliveBTSchemas::BehaviorTreeRead(),
		FOliveToolHandler::CreateRaw(this, &FOliveBTToolHandlers::HandleBehaviorTreeRead),
		{TEXT("behaviortree"), TEXT("read")},
		TEXT("behaviortree")
	);
	RegisteredToolNames.Add(TEXT("behaviortree.read"));

	// P5: consolidated add dispatcher. Replaces behaviortree.add_node as the
	// canonical entry point. Legacy behaviortree.{add_composite,add_task,
	// add_decorator,add_service,add_node} are aliases that pre-fill 'node_type'.
	Registry.RegisterTool(
		TEXT("behaviortree.add"),
		TEXT("Add a node to a Behavior Tree. Dispatches on 'node_type' (composite|task|decorator|"
			"service|node) to the matching internal handler."),
		OliveBTSchemas::BehaviorTreeAdd(),
		FOliveToolHandler::CreateRaw(this, &FOliveBTToolHandlers::HandleBehaviorTreeAdd),
		{TEXT("behaviortree"), TEXT("write"), TEXT("add")},
		TEXT("behaviortree")
	);
	RegisteredToolNames.Add(TEXT("behaviortree.add"));

	// P5: consolidated modify dispatcher. Replaces set_blackboard, set_node_property,
	// modify_node, set_decorator. Legacy names remain as aliases.
	Registry.RegisterTool(
		TEXT("behaviortree.modify"),
		TEXT("Modify a Behavior Tree. Dispatches on 'entity' (node|decorator|blackboard_ref)."),
		OliveBTSchemas::BehaviorTreeModify(),
		FOliveToolHandler::CreateRaw(this, &FOliveBTToolHandlers::HandleBehaviorTreeModify),
		{TEXT("behaviortree"), TEXT("write"), TEXT("modify")},
		TEXT("behaviortree")
	);
	RegisteredToolNames.Add(TEXT("behaviortree.modify"));

	// P5: rename behaviortree.remove_node -> behaviortree.remove (legacy alias in registry).
	Registry.RegisterTool(
		TEXT("behaviortree.remove"),
		TEXT("Remove a node from the Behavior Tree"),
		OliveBTSchemas::BehaviorTreeRemoveNode(),
		FOliveToolHandler::CreateRaw(this, &FOliveBTToolHandlers::HandleBehaviorTreeRemoveNode),
		{TEXT("behaviortree"), TEXT("write"), TEXT("delete")},
		TEXT("behaviortree")
	);
	RegisteredToolNames.Add(TEXT("behaviortree.remove"));

	// P5: rename behaviortree.move_node -> behaviortree.move (legacy alias in registry).
	Registry.RegisterTool(
		TEXT("behaviortree.move"),
		TEXT("Move a node to a different parent composite"),
		OliveBTSchemas::BehaviorTreeMoveNode(),
		FOliveToolHandler::CreateRaw(this, &FOliveBTToolHandlers::HandleBehaviorTreeMoveNode),
		{TEXT("behaviortree"), TEXT("write")},
		TEXT("behaviortree")
	);
	RegisteredToolNames.Add(TEXT("behaviortree.move"));
}

// ============================================================================
// Helpers
// ============================================================================

bool FOliveBTToolHandlers::LoadBlackboardFromParams(
	const TSharedPtr<FJsonObject>& Params,
	UBlackboardData*& OutBB,
	FOliveToolResult& OutError)
{
	FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		OutError = FOliveToolResult::Error(TEXT("MISSING_PATH"), TEXT("Missing required 'path' parameter"),
			TEXT("Provide the asset path of the Blackboard"));
		return false;
	}

	OutBB = Cast<UBlackboardData>(StaticLoadObject(UBlackboardData::StaticClass(), nullptr, *Path));
	if (!OutBB)
	{
		OutError = FOliveToolResult::Error(TEXT("BLACKBOARD_NOT_FOUND"),
			FString::Printf(TEXT("Blackboard not found: %s"), *Path),
			TEXT("Use project.search to find the correct asset path"));
		return false;
	}

	return true;
}

bool FOliveBTToolHandlers::LoadBehaviorTreeFromParams(
	const TSharedPtr<FJsonObject>& Params,
	UBehaviorTree*& OutBT,
	FOliveToolResult& OutError)
{
	FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		OutError = FOliveToolResult::Error(TEXT("MISSING_PATH"), TEXT("Missing required 'path' parameter"),
			TEXT("Provide the asset path of the Behavior Tree"));
		return false;
	}

	OutBT = Cast<UBehaviorTree>(StaticLoadObject(UBehaviorTree::StaticClass(), nullptr, *Path));
	if (!OutBT)
	{
		OutError = FOliveToolResult::Error(TEXT("BEHAVIORTREE_NOT_FOUND"),
			FString::Printf(TEXT("Behavior Tree not found: %s"), *Path),
			TEXT("Use project.search to find the correct asset path"));
		return false;
	}

	return true;
}

EOliveIRBlackboardKeyType FOliveBTToolHandlers::ParseKeyType(const FString& InTypeStr)
{
	const FString TypeStr = InTypeStr.ToLower();
	if (TypeStr == TEXT("bool") || TypeStr == TEXT("boolean")) return EOliveIRBlackboardKeyType::Bool;
	if (TypeStr == TEXT("int")) return EOliveIRBlackboardKeyType::Int;
	if (TypeStr == TEXT("float")) return EOliveIRBlackboardKeyType::Float;
	if (TypeStr == TEXT("string")) return EOliveIRBlackboardKeyType::String;
	if (TypeStr == TEXT("name")) return EOliveIRBlackboardKeyType::Name;
	if (TypeStr == TEXT("vector")) return EOliveIRBlackboardKeyType::Vector;
	if (TypeStr == TEXT("rotator")) return EOliveIRBlackboardKeyType::Rotator;
	if (TypeStr == TEXT("enum")) return EOliveIRBlackboardKeyType::Enum;
	if (TypeStr == TEXT("object")) return EOliveIRBlackboardKeyType::Object;
	if (TypeStr == TEXT("class")) return EOliveIRBlackboardKeyType::Class;
	return EOliveIRBlackboardKeyType::Unknown;
}

// ============================================================================
// Blackboard Handlers
// ============================================================================

FOliveToolResult FOliveBTToolHandlers::HandleBlackboardCreate(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing"),
			TEXT("Provide 'path' as a /Game/... asset path. Example: \"/Game/AI/BB_MyBlackboard\""));
	}
	FString ParentPath;
	Params->TryGetStringField(TEXT("parent"), ParentPath);

	UBlackboardData* NewBB = FOliveBlackboardWriter::Get().CreateBlackboard(Path, ParentPath);
	if (!NewBB)
	{
		return FOliveToolResult::Error(TEXT("CREATE_FAILED"),
			FString::Printf(TEXT("Failed to create Blackboard at '%s'"), *Path),
			TEXT("Verify the path is a valid /Game/... asset path and the parent directory exists"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), NewBB->GetName());
	Result->SetStringField(TEXT("path"), NewBB->GetPathName());
	Result->SetStringField(TEXT("status"), TEXT("created"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBlackboardRead(const TSharedPtr<FJsonObject>& Params)
{
	UBlackboardData* BB;
	FOliveToolResult Error;
	if (!LoadBlackboardFromParams(Params, BB, Error))
	{
		return Error;
	}

	bool bIncludeInherited = false;
	Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

	TOptional<FOliveIRBlackboard> IR = FOliveBlackboardReader::Get().ReadBlackboard(BB);
	if (!IR.IsSet())
	{
		return FOliveToolResult::Error(TEXT("READ_FAILED"), TEXT("Failed to read Blackboard"),
			TEXT("The asset may be corrupted or not a valid Blackboard. Use project.search to verify."));
	}

	TSharedPtr<FJsonObject> Result = IR.GetValue().ToJson();

	// Optionally include inherited keys
	if (bIncludeInherited && BB->Parent)
	{
		TArray<FOliveIRBlackboardKey> AllKeys =
			FOliveBlackboardReader::Get().ReadAllKeys(BB, true);

		TArray<TSharedPtr<FJsonValue>> AllKeysArray;
		for (const FOliveIRBlackboardKey& Key : AllKeys)
		{
			AllKeysArray.Add(MakeShared<FJsonValueObject>(Key.ToJson()));
		}
		Result->SetArrayField(TEXT("all_keys"), AllKeysArray);
	}

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBlackboardAddKey(const TSharedPtr<FJsonObject>& Params)
{
	UBlackboardData* BB;
	FOliveToolResult Error;
	if (!LoadBlackboardFromParams(Params, BB, Error))
	{
		return Error;
	}

	FString KeyName;
	if (!Params->TryGetStringField(TEXT("name"), KeyName) || KeyName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing"),
			TEXT("Provide the key name. Example: \"name\": \"TargetActor\""));
	}

	// --- Upsert: check if key already exists ---
	bool bKeyExists = false;
	for (const FBlackboardEntry& Existing : BB->Keys)
	{
		if (Existing.EntryName == FName(*KeyName))
		{
			bKeyExists = true;
			break;
		}
	}

	if (bKeyExists)
	{
		// Delegate to modify logic: extract modify-relevant params and forward
		UE_LOG(LogOliveBTTools, Log, TEXT("Key '%s' already exists in blackboard '%s' — upsert: modifying instead"),
			*KeyName, *BB->GetName());
		return HandleBlackboardModifyKey(Params);
	}

	// --- Create path: key does not exist ---
	FString KeyTypeStr;
	if (!Params->TryGetStringField(TEXT("key_type"), KeyTypeStr) || KeyTypeStr.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'key_type' is missing"),
			TEXT("Provide key_type: bool, int, float, string, name, vector, rotator, enum, object, class"));
	}
	FString BaseClass;
	FString EnumType;
	FString Description;
	bool bInstanceSynced = false;
	Params->TryGetStringField(TEXT("base_class"), BaseClass);
	Params->TryGetStringField(TEXT("enum_type"), EnumType);
	Params->TryGetStringField(TEXT("description"), Description);
	Params->TryGetBoolField(TEXT("instance_synced"), bInstanceSynced);

	EOliveIRBlackboardKeyType KeyType = ParseKeyType(KeyTypeStr);
	if (KeyType == EOliveIRBlackboardKeyType::Unknown)
	{
		return FOliveToolResult::Error(TEXT("INVALID_KEY_TYPE"),
			FString::Printf(TEXT("Unknown key type: '%s'"), *KeyTypeStr),
			TEXT("Use: bool, int, float, string, name, vector, rotator, enum, object, class"));
	}

	bool bSuccess = FOliveBlackboardWriter::Get().AddKey(
		BB, KeyName, KeyType, BaseClass, EnumType, bInstanceSynced, Description);

	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("ADD_KEY_FAILED"),
			FString::Printf(TEXT("Failed to add key '%s'"), *KeyName),
			TEXT("Verify the key name is not defined in a parent Blackboard"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("key_name"), KeyName);
	Result->SetStringField(TEXT("key_type"), KeyTypeStr);
	Result->SetStringField(TEXT("status"), TEXT("added"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBlackboardRemoveKey(const TSharedPtr<FJsonObject>& Params)
{
	UBlackboardData* BB;
	FOliveToolResult Error;
	if (!LoadBlackboardFromParams(Params, BB, Error))
	{
		return Error;
	}

	FString KeyName;
	if (!Params->TryGetStringField(TEXT("name"), KeyName) || KeyName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing"),
			TEXT("Provide the name of the key to remove. Example: \"name\": \"TargetActor\""));
	}

	bool bSuccess = FOliveBlackboardWriter::Get().RemoveKey(BB, KeyName);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("REMOVE_KEY_FAILED"),
			FString::Printf(TEXT("Failed to remove key '%s'"), *KeyName),
			TEXT("Verify the key exists in this Blackboard (not inherited)"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("key_name"), KeyName);
	Result->SetStringField(TEXT("status"), TEXT("removed"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBlackboardModifyKey(const TSharedPtr<FJsonObject>& Params)
{
	UBlackboardData* BB;
	FOliveToolResult Error;
	if (!LoadBlackboardFromParams(Params, BB, Error))
	{
		return Error;
	}

	FString KeyName;
	if (!Params->TryGetStringField(TEXT("name"), KeyName) || KeyName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'name' is missing"),
			TEXT("Provide the name of the existing key to modify. Example: \"name\": \"TargetActor\""));
	}
	FString NewName;
	FString Description;
	Params->TryGetStringField(TEXT("new_name"), NewName);
	Params->TryGetStringField(TEXT("description"), Description);

	bool bSetInstanceSynced = Params->HasField(TEXT("instance_synced"));
	bool bInstanceSynced = false;
	Params->TryGetBoolField(TEXT("instance_synced"), bInstanceSynced);

	bool bSuccess = FOliveBlackboardWriter::Get().ModifyKey(
		BB, KeyName, NewName, bSetInstanceSynced, bInstanceSynced, Description);

	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("MODIFY_KEY_FAILED"),
			FString::Printf(TEXT("Failed to modify key '%s'"), *KeyName),
			TEXT("Verify the key exists in this Blackboard (not inherited). Use blackboard.read to check."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("key_name"), NewName.IsEmpty() ? KeyName : NewName);
	Result->SetStringField(TEXT("status"), TEXT("modified"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBlackboardSetParent(const TSharedPtr<FJsonObject>& Params)
{
	UBlackboardData* BB;
	FOliveToolResult Error;
	if (!LoadBlackboardFromParams(Params, BB, Error))
	{
		return Error;
	}

	FString ParentPath;
	if (!Params->TryGetStringField(TEXT("parent_path"), ParentPath) || ParentPath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'parent_path' is missing"),
			TEXT("Provide the asset path of the parent Blackboard. Example: \"/Game/AI/BB_BaseKeys\""));
	}

	bool bSuccess = FOliveBlackboardWriter::Get().SetParent(BB, ParentPath);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("SET_PARENT_FAILED"),
			FString::Printf(TEXT("Failed to set parent to '%s'"), *ParentPath),
			TEXT("Check that the parent exists and wouldn't create circular inheritance"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("parent"), ParentPath);
	Result->SetStringField(TEXT("status"), TEXT("parent_set"));

	return FOliveToolResult::Success(Result);
}

// ============================================================================
// Behavior Tree Handlers
// ============================================================================

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeCreate(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'path' is missing"),
			TEXT("Provide 'path' as a /Game/... asset path. Example: \"/Game/AI/BT_MyTree\""));
	}
	FString BlackboardPath;
	Params->TryGetStringField(TEXT("blackboard"), BlackboardPath);

	UBehaviorTree* NewBT = FOliveBehaviorTreeWriter::Get().CreateBehaviorTree(Path);
	if (!NewBT)
	{
		return FOliveToolResult::Error(TEXT("CREATE_FAILED"),
			FString::Printf(TEXT("Failed to create Behavior Tree at '%s'"), *Path),
			TEXT("Verify the path is a valid /Game/... asset path and the parent directory exists"));
	}

	// Set blackboard if specified
	if (!BlackboardPath.IsEmpty())
	{
		FOliveBehaviorTreeWriter::Get().SetBlackboard(NewBT, BlackboardPath);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), NewBT->GetName());
	Result->SetStringField(TEXT("path"), NewBT->GetPathName());
	Result->SetStringField(TEXT("status"), TEXT("created"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeRead(const TSharedPtr<FJsonObject>& Params)
{
	UBehaviorTree* BT;
	FOliveToolResult Error;
	if (!LoadBehaviorTreeFromParams(Params, BT, Error))
	{
		return Error;
	}

	bool bIncludeBlackboard = true;
	if (Params->HasField(TEXT("include_blackboard")))
	{
		bIncludeBlackboard = Params->GetBoolField(TEXT("include_blackboard"));
	}

	TOptional<FBTReadResult> ReadResult =
		FOliveBehaviorTreeReader::Get().ReadBehaviorTree(BT, bIncludeBlackboard);

	if (!ReadResult.IsSet())
	{
		return FOliveToolResult::Error(TEXT("READ_FAILED"), TEXT("Failed to read Behavior Tree"),
			TEXT("The asset may be corrupted or not a valid Behavior Tree. Use project.search to verify."));
	}

	return FOliveToolResult::Success(ReadResult.GetValue().ToJson());
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeSetBlackboard(const TSharedPtr<FJsonObject>& Params)
{
	UBehaviorTree* BT;
	FOliveToolResult Error;
	if (!LoadBehaviorTreeFromParams(Params, BT, Error))
	{
		return Error;
	}

	FString BlackboardPath;
	if (!Params->TryGetStringField(TEXT("blackboard"), BlackboardPath) || BlackboardPath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'blackboard' is missing"),
			TEXT("Provide the Blackboard asset path. Example: \"/Game/AI/BB_MyBlackboard\""));
	}

	bool bSuccess = FOliveBehaviorTreeWriter::Get().SetBlackboard(BT, BlackboardPath);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("SET_BLACKBOARD_FAILED"),
			FString::Printf(TEXT("Failed to set Blackboard '%s'"), *BlackboardPath),
			TEXT("Verify the Blackboard exists at the given path. Use project.search to find it."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blackboard"), BlackboardPath);
	Result->SetStringField(TEXT("status"), TEXT("blackboard_set"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeAddNode(const TSharedPtr<FJsonObject>& Params)
{
	// Validate node_kind parameter
	FString NodeKind;
	if (!Params->TryGetStringField(TEXT("node_kind"), NodeKind) || NodeKind.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'node_kind' is missing"),
			TEXT("Provide node_kind: \"composite\", \"task\", \"decorator\", or \"service\""));
	}

	const FString NodeKindLower = NodeKind.ToLower();

	if (NodeKindLower == TEXT("composite"))
	{
		// For composites, composite_type is the class selector.
		// Map 'class' -> 'composite_type' if composite_type is missing but class is provided
		if (!Params->HasField(TEXT("composite_type")))
		{
			FString ClassParam;
			if (Params->TryGetStringField(TEXT("class"), ClassParam) && !ClassParam.IsEmpty())
			{
				Params->SetStringField(TEXT("composite_type"), ClassParam);
			}
		}
		return HandleBehaviorTreeAddComposite(Params);
	}
	else if (NodeKindLower == TEXT("task"))
	{
		// Map 'class' -> 'task_class' if task_class is missing
		if (!Params->HasField(TEXT("task_class")))
		{
			FString ClassParam;
			if (Params->TryGetStringField(TEXT("class"), ClassParam) && !ClassParam.IsEmpty())
			{
				Params->SetStringField(TEXT("task_class"), ClassParam);
			}
		}
		return HandleBehaviorTreeAddTask(Params);
	}
	else if (NodeKindLower == TEXT("decorator"))
	{
		// Map 'class' -> 'decorator_class' if decorator_class is missing
		if (!Params->HasField(TEXT("decorator_class")))
		{
			FString ClassParam;
			if (Params->TryGetStringField(TEXT("class"), ClassParam) && !ClassParam.IsEmpty())
			{
				Params->SetStringField(TEXT("decorator_class"), ClassParam);
			}
		}
		return HandleBehaviorTreeAddDecorator(Params);
	}
	else if (NodeKindLower == TEXT("service"))
	{
		// Map 'class' -> 'service_class' if service_class is missing
		if (!Params->HasField(TEXT("service_class")))
		{
			FString ClassParam;
			if (Params->TryGetStringField(TEXT("class"), ClassParam) && !ClassParam.IsEmpty())
			{
				Params->SetStringField(TEXT("service_class"), ClassParam);
			}
		}
		return HandleBehaviorTreeAddService(Params);
	}
	else
	{
		return FOliveToolResult::Error(TEXT("INVALID_NODE_KIND"),
			FString::Printf(TEXT("Unknown node_kind: '%s'"), *NodeKind),
			TEXT("Use: \"composite\", \"task\", \"decorator\", or \"service\""));
	}
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeAddComposite(const TSharedPtr<FJsonObject>& Params)
{
	UBehaviorTree* BT;
	FOliveToolResult Error;
	if (!LoadBehaviorTreeFromParams(Params, BT, Error))
	{
		return Error;
	}

	FString ParentNodeId;
	if (!Params->TryGetStringField(TEXT("parent_node_id"), ParentNodeId) || ParentNodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'parent_node_id' is missing"),
			TEXT("Provide the node_id of the parent composite. Use 'root' for the root. Use behaviortree.read to see node IDs."));
	}
	FString CompositeType;
	if (!Params->TryGetStringField(TEXT("composite_type"), CompositeType) || CompositeType.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'composite_type' is missing"),
			TEXT("Provide composite_type: Selector, Sequence, or SimpleParallel"));
	}
	int32 ChildIndex = Params->HasField(TEXT("child_index")) ?
		(int32)Params->GetNumberField(TEXT("child_index")) : -1;

	FString NewNodeId = FOliveBehaviorTreeWriter::Get().AddComposite(
		BT, ParentNodeId, CompositeType, ChildIndex);

	if (NewNodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("ADD_COMPOSITE_FAILED"),
			FString::Printf(TEXT("Failed to add %s composite under '%s'"), *CompositeType, *ParentNodeId),
			TEXT("Verify parent_node_id exists and composite_type is valid (Selector, Sequence, SimpleParallel)."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNodeId);
	Result->SetStringField(TEXT("composite_type"), CompositeType);
	Result->SetStringField(TEXT("status"), TEXT("added"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeAddTask(const TSharedPtr<FJsonObject>& Params)
{
	UBehaviorTree* BT;
	FOliveToolResult Error;
	if (!LoadBehaviorTreeFromParams(Params, BT, Error))
	{
		return Error;
	}

	FString ParentNodeId;
	if (!Params->TryGetStringField(TEXT("parent_node_id"), ParentNodeId) || ParentNodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'parent_node_id' is missing"),
			TEXT("Provide the node_id of the parent composite. Use behaviortree.read to see node IDs."));
	}
	FString TaskClass;
	if (!Params->TryGetStringField(TEXT("task_class"), TaskClass) || TaskClass.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'task_class' is missing"),
			TEXT("Provide the task class name. Example: \"BTTask_MoveTo\", \"BTTask_Wait\""));
	}
	int32 ChildIndex = Params->HasField(TEXT("child_index")) ?
		(int32)Params->GetNumberField(TEXT("child_index")) : -1;

	TMap<FString, FString> Properties;
	const TSharedPtr<FJsonObject>* PropsJson;
	if (Params->TryGetObjectField(TEXT("properties"), PropsJson))
	{
		for (const auto& Pair : (*PropsJson)->Values)
		{
			Properties.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	FString NewNodeId = FOliveBehaviorTreeWriter::Get().AddTask(
		BT, ParentNodeId, TaskClass, ChildIndex, Properties);

	if (NewNodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("ADD_TASK_FAILED"),
			FString::Printf(TEXT("Failed to add task '%s' under '%s'"), *TaskClass, *ParentNodeId),
			TEXT("Verify the class exists. Use resource olive://behaviortree/node-catalog/search?q=<query>."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNodeId);
	Result->SetStringField(TEXT("task_class"), TaskClass);
	Result->SetStringField(TEXT("status"), TEXT("added"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeAddDecorator(const TSharedPtr<FJsonObject>& Params)
{
	UBehaviorTree* BT;
	FOliveToolResult Error;
	if (!LoadBehaviorTreeFromParams(Params, BT, Error))
	{
		return Error;
	}

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'node_id' is missing"),
			TEXT("Provide the node_id to attach the decorator to. Use behaviortree.read to see node IDs."));
	}
	FString DecoratorClass;
	if (!Params->TryGetStringField(TEXT("decorator_class"), DecoratorClass) || DecoratorClass.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'decorator_class' is missing"),
			TEXT("Provide the decorator class name. Example: \"BTDecorator_Blackboard\", \"BTDecorator_Cooldown\""));
	}

	TMap<FString, FString> Properties;
	const TSharedPtr<FJsonObject>* PropsJson;
	if (Params->TryGetObjectField(TEXT("properties"), PropsJson))
	{
		for (const auto& Pair : (*PropsJson)->Values)
		{
			Properties.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	FString NewNodeId = FOliveBehaviorTreeWriter::Get().AddDecorator(
		BT, NodeId, DecoratorClass, Properties);

	if (NewNodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("ADD_DECORATOR_FAILED"),
			FString::Printf(TEXT("Failed to add decorator '%s' on '%s'"), *DecoratorClass, *NodeId),
			TEXT("Verify the node exists and the decorator class name is valid."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNodeId);
	Result->SetStringField(TEXT("decorator_class"), DecoratorClass);
	Result->SetStringField(TEXT("status"), TEXT("added"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeAddService(const TSharedPtr<FJsonObject>& Params)
{
	UBehaviorTree* BT;
	FOliveToolResult Error;
	if (!LoadBehaviorTreeFromParams(Params, BT, Error))
	{
		return Error;
	}

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'node_id' is missing"),
			TEXT("Provide the node_id to attach the service to. Use behaviortree.read to see node IDs."));
	}
	FString ServiceClass;
	if (!Params->TryGetStringField(TEXT("service_class"), ServiceClass) || ServiceClass.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'service_class' is missing"),
			TEXT("Provide the service class name. Example: \"BTService_DefaultFocus\""));
	}

	TMap<FString, FString> Properties;
	const TSharedPtr<FJsonObject>* PropsJson;
	if (Params->TryGetObjectField(TEXT("properties"), PropsJson))
	{
		for (const auto& Pair : (*PropsJson)->Values)
		{
			Properties.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	FString NewNodeId = FOliveBehaviorTreeWriter::Get().AddService(
		BT, NodeId, ServiceClass, Properties);

	if (NewNodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("ADD_SERVICE_FAILED"),
			FString::Printf(TEXT("Failed to add service '%s' on '%s'"), *ServiceClass, *NodeId),
			TEXT("Verify the node exists and the service class name is valid."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NewNodeId);
	Result->SetStringField(TEXT("service_class"), ServiceClass);
	Result->SetStringField(TEXT("status"), TEXT("added"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeRemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	UBehaviorTree* BT;
	FOliveToolResult Error;
	if (!LoadBehaviorTreeFromParams(Params, BT, Error))
	{
		return Error;
	}

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'node_id' is missing"),
			TEXT("Provide the node_id of the node to remove. Use behaviortree.read to see node IDs."));
	}

	bool bSuccess = FOliveBehaviorTreeWriter::Get().RemoveNode(BT, NodeId);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("REMOVE_NODE_FAILED"),
			FString::Printf(TEXT("Failed to remove node '%s'"), *NodeId),
			TEXT("Verify the node exists. Cannot remove the root node."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("status"), TEXT("removed"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeMoveNode(const TSharedPtr<FJsonObject>& Params)
{
	UBehaviorTree* BT;
	FOliveToolResult Error;
	if (!LoadBehaviorTreeFromParams(Params, BT, Error))
	{
		return Error;
	}

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'node_id' is missing"),
			TEXT("Provide the node_id of the node to move. Use behaviortree.read to see node IDs."));
	}
	FString NewParentId;
	if (!Params->TryGetStringField(TEXT("new_parent_id"), NewParentId) || NewParentId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'new_parent_id' is missing"),
			TEXT("Provide the node_id of the new parent composite."));
	}
	int32 ChildIndex = Params->HasField(TEXT("child_index")) ?
		(int32)Params->GetNumberField(TEXT("child_index")) : -1;

	bool bSuccess = FOliveBehaviorTreeWriter::Get().MoveNode(BT, NodeId, NewParentId, ChildIndex);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("MOVE_NODE_FAILED"),
			FString::Printf(TEXT("Failed to move node '%s' to '%s'"), *NodeId, *NewParentId),
			TEXT("Verify both nodes exist and the new parent is a composite node."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("new_parent_id"), NewParentId);
	Result->SetStringField(TEXT("status"), TEXT("moved"));

	return FOliveToolResult::Success(Result);
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeSetNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	UBehaviorTree* BT;
	FOliveToolResult Error;
	if (!LoadBehaviorTreeFromParams(Params, BT, Error))
	{
		return Error;
	}

	FString NodeId;
	if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'node_id' is missing"),
			TEXT("Provide the node_id of the target node. Use behaviortree.read to see node IDs."));
	}
	FString Property;
	if (!Params->TryGetStringField(TEXT("property"), Property) || Property.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'property' is missing"),
			TEXT("Provide the UPROPERTY name to set. Example: \"WaitTime\", \"AcceptableRadius\""));
	}
	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return FOliveToolResult::Error(TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Required parameter 'value' is missing"),
			TEXT("Provide the property value as a string. Example: \"5.0\", \"true\""));
	}

	bool bSuccess = FOliveBehaviorTreeWriter::Get().SetNodeProperty(BT, NodeId, Property, Value);
	if (!bSuccess)
	{
		return FOliveToolResult::Error(TEXT("SET_PROPERTY_FAILED"),
			FString::Printf(TEXT("Failed to set property '%s' on node '%s'"), *Property, *NodeId),
			TEXT("Verify the node exists and the property name matches a UPROPERTY on that node class."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("property"), Property);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetStringField(TEXT("status"), TEXT("property_set"));

	return FOliveToolResult::Success(Result);
}

// ============================================================================
// Consolidated Dispatchers (P5)
//
// These dispatchers route on action/node_type/entity to the existing specialized
// handlers. Legacy tool names (blackboard.create/read/add_key/modify_key/
// remove_key/set_parent, behaviortree.{add_node,add_composite,add_task,
// add_decorator,add_service,modify_node,set_node_property,set_decorator,
// set_blackboard,remove_node,move_node}) are preserved as aliases that pre-fill
// action/node_type/entity before dispatch. See OliveToolRegistry::GetToolAliases().
// ============================================================================

namespace
{
	/** Clone params so we can normalize fields without mutating the caller. */
	TSharedPtr<FJsonObject> CloneBTParams(const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Params.IsValid())
		{
			for (const auto& Pair : Params->Values) { Out->Values.Add(Pair.Key, Pair.Value); }
		}
		return Out;
	}
} // anonymous namespace

FOliveToolResult FOliveBTToolHandlers::HandleBlackboardModify(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a params object with 'path' and 'action' fields."));
	}

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Blackboard asset path."));
	}

	FString Action;
	Params->TryGetStringField(TEXT("action"), Action);
	Action = Action.ToLower();
	if (Action.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'action'"),
			TEXT("action must be one of: create, read, add_key, modify_key, remove_key, set_parent"));
	}

	TSharedPtr<FJsonObject> SubParams = CloneBTParams(Params);

	if (Action == TEXT("create"))
	{
		return HandleBlackboardCreate(SubParams);
	}
	if (Action == TEXT("read"))
	{
		return HandleBlackboardRead(SubParams);
	}
	if (Action == TEXT("add_key"))
	{
		return HandleBlackboardAddKey(SubParams);
	}
	if (Action == TEXT("modify_key"))
	{
		return HandleBlackboardModifyKey(SubParams);
	}
	if (Action == TEXT("remove_key"))
	{
		return HandleBlackboardRemoveKey(SubParams);
	}
	if (Action == TEXT("set_parent"))
	{
		return HandleBlackboardSetParent(SubParams);
	}

	return FOliveToolResult::Error(
		TEXT("VALIDATION_INVALID_VALUE"),
		FString::Printf(TEXT("Unknown action '%s'"), *Action),
		TEXT("action must be one of: create, read, add_key, modify_key, remove_key, set_parent"));
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeAdd(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a params object with 'path' and 'node_type' fields."));
	}

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Behavior Tree asset path."));
	}

	// Accept 'node_type' (canonical) or 'node_kind' (legacy). 'node_type' wins.
	FString NodeType;
	Params->TryGetStringField(TEXT("node_type"), NodeType);
	if (NodeType.IsEmpty())
	{
		Params->TryGetStringField(TEXT("node_kind"), NodeType);
	}
	NodeType = NodeType.ToLower();

	if (NodeType.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'node_type'"),
			TEXT("node_type must be one of: composite, task, decorator, service, node"));
	}

	TSharedPtr<FJsonObject> SubParams = CloneBTParams(Params);

	if (NodeType == TEXT("node"))
	{
		// Generic pass-through: forward to the legacy add_node dispatcher.
		// Legacy callers supplied node_kind inside Params and the old handler
		// routes on that. Ensure node_kind is set so downstream validation passes.
		FString InnerKind;
		if (!SubParams->TryGetStringField(TEXT("node_kind"), InnerKind) || InnerKind.IsEmpty())
		{
			// Try inferring from 'composite_type' / 'class' but fall back to a
			// helpful error rather than silently guessing.
			FString CompositeType;
			if (SubParams->TryGetStringField(TEXT("composite_type"), CompositeType) && !CompositeType.IsEmpty())
			{
				SubParams->SetStringField(TEXT("node_kind"), TEXT("composite"));
			}
			else
			{
				return FOliveToolResult::Error(
					TEXT("VALIDATION_MISSING_PARAM"),
					TEXT("node_type='node' requires an inner 'node_kind' field"),
					TEXT("Either use node_type=\"composite\"|\"task\"|\"decorator\"|\"service\" directly, "
						"or supply node_kind inside params."));
			}
		}
		return HandleBehaviorTreeAddNode(SubParams);
	}

	if (NodeType == TEXT("composite") || NodeType == TEXT("task")
		|| NodeType == TEXT("decorator") || NodeType == TEXT("service"))
	{
		// Inject node_kind for the legacy dispatcher which still reads that field.
		SubParams->SetStringField(TEXT("node_kind"), NodeType);
		return HandleBehaviorTreeAddNode(SubParams);
	}

	return FOliveToolResult::Error(
		TEXT("VALIDATION_INVALID_VALUE"),
		FString::Printf(TEXT("Unknown node_type '%s'"), *NodeType),
		TEXT("node_type must be one of: composite, task, decorator, service, node"));
}

FOliveToolResult FOliveBTToolHandlers::HandleBehaviorTreeModify(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_INVALID_PARAMS"),
			TEXT("Parameters object is null"),
			TEXT("Provide a params object with 'path' and 'entity' fields."));
	}

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'path'"),
			TEXT("Provide the Behavior Tree asset path."));
	}

	FString Entity;
	Params->TryGetStringField(TEXT("entity"), Entity);
	Entity = Entity.ToLower();
	if (Entity.IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'entity'"),
			TEXT("entity must be one of: node, decorator, blackboard_ref"));
	}

	TSharedPtr<FJsonObject> SubParams = CloneBTParams(Params);

	if (Entity == TEXT("node") || Entity == TEXT("decorator"))
	{
		// DESIGN NOTE: BT decorators are individually-identifiable via node_id in
		// the current reader/writer model, so entity='decorator' routes to the
		// same set_node_property handler as entity='node'. If decorator-specific
		// operations diverge later, split the dispatch here.
		return HandleBehaviorTreeSetNodeProperty(SubParams);
	}
	if (Entity == TEXT("blackboard_ref"))
	{
		return HandleBehaviorTreeSetBlackboard(SubParams);
	}

	return FOliveToolResult::Error(
		TEXT("VALIDATION_INVALID_VALUE"),
		FString::Printf(TEXT("Unknown entity '%s'"), *Entity),
		TEXT("entity must be one of: node, decorator, blackboard_ref"));
}
