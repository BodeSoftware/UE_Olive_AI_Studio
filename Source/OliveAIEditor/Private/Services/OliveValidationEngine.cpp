// Copyright Bode Software. All Rights Reserved.

#include "Services/OliveValidationEngine.h"
#include "Services/OliveAssetResolver.h"
#include "OliveAIEditorModule.h"
#include "Editor.h"
#include "Serialization/JsonSerializer.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "OliveBTNodeSerializer.h"
#include "OliveBTNodeFactory.h"
#include "Reader/OliveCppSourceReader.h"
#include "Reader/OliveCppReflectionReader.h"
#include "UObject/UObjectIterator.h"
#include "OliveSnapshotManager.h"
#include "Internationalization/Regex.h"

#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

// FOliveValidationResult implementation

void FOliveValidationResult::AddError(const FString& Code, const FString& Message, const FString& Suggestion)
{
	bValid = false;

	FOliveIRMessage Msg;
	Msg.Severity = EOliveIRSeverity::Error;
	Msg.Code = Code;
	Msg.Message = Message;
	Msg.Suggestion = Suggestion;
	Messages.Add(Msg);
}

void FOliveValidationResult::AddWarning(const FString& Code, const FString& Message, const FString& Suggestion)
{
	FOliveIRMessage Msg;
	Msg.Severity = EOliveIRSeverity::Warning;
	Msg.Code = Code;
	Msg.Message = Message;
	Msg.Suggestion = Suggestion;
	Messages.Add(Msg);
}

void FOliveValidationResult::AddInfo(const FString& Code, const FString& Message)
{
	FOliveIRMessage Msg;
	Msg.Severity = EOliveIRSeverity::Info;
	Msg.Code = Code;
	Msg.Message = Message;
	Messages.Add(Msg);
}

bool FOliveValidationResult::HasErrors() const
{
	return !bValid || Messages.ContainsByPredicate([](const FOliveIRMessage& Msg)
	{
		return Msg.Severity == EOliveIRSeverity::Error;
	});
}

bool FOliveValidationResult::HasWarnings() const
{
	return Messages.ContainsByPredicate([](const FOliveIRMessage& Msg)
	{
		return Msg.Severity == EOliveIRSeverity::Warning;
	});
}

TArray<FOliveIRMessage> FOliveValidationResult::GetErrors() const
{
	return Messages.FilterByPredicate([](const FOliveIRMessage& Msg)
	{
		return Msg.Severity == EOliveIRSeverity::Error;
	});
}

TArray<FOliveIRMessage> FOliveValidationResult::GetWarnings() const
{
	return Messages.FilterByPredicate([](const FOliveIRMessage& Msg)
	{
		return Msg.Severity == EOliveIRSeverity::Warning;
	});
}

TSharedPtr<FJsonObject> FOliveValidationResult::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("valid"), bValid);

	if (Messages.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> MessagesArray;
		for (const FOliveIRMessage& Msg : Messages)
		{
			MessagesArray.Add(MakeShared<FJsonValueObject>(Msg.ToJson()));
		}
		Json->SetArrayField(TEXT("messages"), MessagesArray);
	}

	return Json;
}

void FOliveValidationResult::Merge(const FOliveValidationResult& Other)
{
	if (!Other.bValid)
	{
		bValid = false;
	}
	Messages.Append(Other.Messages);
}

// FOliveValidationEngine implementation

FOliveValidationEngine::FOliveValidationEngine()
{
}

FOliveValidationEngine& FOliveValidationEngine::Get()
{
	static FOliveValidationEngine Instance;
	return Instance;
}

void FOliveValidationEngine::RegisterRule(TSharedPtr<IOliveValidationRule> Rule)
{
	if (!Rule.IsValid())
	{
		return;
	}

	FRWScopeLock WriteLock(RulesLock, SLT_Write);

	// Remove existing rule with same name
	Rules.RemoveAll([&Rule](const TSharedPtr<IOliveValidationRule>& Existing)
	{
		return Existing->GetRuleName() == Rule->GetRuleName();
	});

	Rules.Add(Rule);

	UE_LOG(LogOliveAI, Verbose, TEXT("Registered validation rule: %s"), *Rule->GetRuleName().ToString());
}

void FOliveValidationEngine::UnregisterRule(FName RuleName)
{
	FRWScopeLock WriteLock(RulesLock, SLT_Write);

	Rules.RemoveAll([&RuleName](const TSharedPtr<IOliveValidationRule>& Rule)
	{
		return Rule->GetRuleName() == RuleName;
	});
}

FOliveValidationResult FOliveValidationEngine::ValidateOperation(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult CombinedResult;

	FRWScopeLock ReadLock(RulesLock, SLT_ReadOnly);

	for (const TSharedPtr<IOliveValidationRule>& Rule : Rules)
	{
		// Check if rule applies to this tool
		TArray<FString> ApplicableTools = Rule->GetApplicableTools();
		if (ApplicableTools.Num() > 0 && !ApplicableTools.Contains(ToolName))
		{
			continue;
		}

		// Run validation
		FOliveValidationResult RuleResult = Rule->Validate(ToolName, Params, TargetAsset);
		CombinedResult.Merge(RuleResult);

		// Stop on first error if needed
		if (!RuleResult.bValid)
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Validation failed for %s: rule %s"),
				*ToolName, *Rule->GetRuleName().ToString());
		}
	}

	return CombinedResult;
}

void FOliveValidationEngine::RegisterCoreRules()
{
	RegisterRule(MakeShared<FOlivePIEProtectionRule>());
	RegisterRule(MakeShared<FOliveSchemaValidationRule>());
	RegisterRule(MakeShared<FOliveAssetExistsRule>());
}

void FOliveValidationEngine::RegisterBlueprintRules()
{
	// Blueprint-specific rules will be added in Phase 1
}

void FOliveValidationEngine::RegisterBehaviorTreeRules()
{
	RegisterRule(MakeShared<FOliveBTAssetTypeRule>());
	RegisterRule(MakeShared<FOliveBTNodeExistsRule>());
	RegisterRule(MakeShared<FOliveBBKeyUniqueRule>());
}

void FOliveValidationEngine::RegisterPCGRules()
{
	// PCG-specific rules will be added in Phase 3
}

TArray<FName> FOliveValidationEngine::GetRegisteredRules() const
{
	TArray<FName> Names;

	FRWScopeLock ReadLock(RulesLock, SLT_ReadOnly);
	for (const TSharedPtr<IOliveValidationRule>& Rule : Rules)
	{
		Names.Add(Rule->GetRuleName());
	}

	return Names;
}

// FOlivePIEProtectionRule implementation

FOliveValidationResult FOlivePIEProtectionRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	// Check if we're in PIE
	bool bInPIE = false;
	if (GEditor)
	{
		bInPIE = GEditor->bIsSimulatingInEditor || GEditor->PlayWorld != nullptr;
	}

	if (bInPIE)
	{
		// Only block write operations
		static TArray<FString> ReadOnlyTools = {
			TEXT("blueprint.read"),
			TEXT("blueprint.read_function"),
			TEXT("project.search"),
			TEXT("project.get_info"),
			TEXT("behaviortree.read"),
			TEXT("blackboard.read")
		};

		if (!ReadOnlyTools.Contains(ToolName))
		{
			Result.AddError(
				TEXT("PIE_ACTIVE"),
				TEXT("Cannot modify assets while Play in Editor is active."),
				TEXT("Stop PIE and try again.")
			);
		}
	}

	return Result;
}

// FOliveSchemaValidationRule implementation

void FOliveSchemaValidationRule::RegisterSchema(const FString& ToolName, const TSharedPtr<FJsonObject>& Schema)
{
	ToolSchemas.Add(ToolName, Schema);
}

FOliveValidationResult FOliveSchemaValidationRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	// Get schema for this tool
	TSharedPtr<FJsonObject>* SchemaPtr = ToolSchemas.Find(ToolName);
	if (!SchemaPtr || !SchemaPtr->IsValid())
	{
		// No schema registered - skip validation
		return Result;
	}

	// TODO: Implement full JSON Schema validation
	// For now, just check required fields

	const TSharedPtr<FJsonObject>& Schema = *SchemaPtr;
	const TArray<TSharedPtr<FJsonValue>>* RequiredFields;

	if (Schema->TryGetArrayField(TEXT("required"), RequiredFields))
	{
		for (const TSharedPtr<FJsonValue>& RequiredField : *RequiredFields)
		{
			FString FieldName = RequiredField->AsString();
			if (!Params.IsValid() || !Params->HasField(FieldName))
			{
				Result.AddError(
					TEXT("MISSING_REQUIRED_FIELD"),
					FString::Printf(TEXT("Missing required field: %s"), *FieldName),
					FString::Printf(TEXT("Add the '%s' field to the parameters."), *FieldName)
				);
			}
		}
	}

	return Result;
}

// FOliveAssetExistsRule implementation

FOliveValidationResult FOliveAssetExistsRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	// Only check for tools that operate on existing assets
	static TArray<FString> AssetRequiredTools = {
		TEXT("blueprint.read"),
		TEXT("blueprint.add_variable"),
		TEXT("blueprint.add_component"),
		TEXT("blueprint.add_function"),
		TEXT("behaviortree.read"),
		TEXT("behaviortree.set_blackboard"),
		TEXT("behaviortree.add_composite"),
		TEXT("behaviortree.add_task"),
		TEXT("behaviortree.add_decorator"),
		TEXT("behaviortree.add_service"),
		TEXT("behaviortree.remove_node"),
		TEXT("behaviortree.move_node"),
		TEXT("behaviortree.set_node_property"),
		TEXT("blackboard.read"),
		TEXT("blackboard.add_key"),
		TEXT("blackboard.remove_key"),
		TEXT("blackboard.modify_key"),
		TEXT("blackboard.set_parent")
	};

	if (!AssetRequiredTools.Contains(ToolName))
	{
		return Result;
	}

	// Get the path parameter
	if (!Params.IsValid())
	{
		Result.AddError(
			TEXT("MISSING_PARAMS"),
			TEXT("No parameters provided."),
			TEXT("Provide the required parameters.")
		);
		return Result;
	}

	FString AssetPath = Params->GetStringField(TEXT("path"));
	if (AssetPath.IsEmpty())
	{
		AssetPath = Params->GetStringField(TEXT("blueprint"));
	}
	if (AssetPath.IsEmpty())
	{
		AssetPath = Params->GetStringField(TEXT("asset"));
	}

	if (AssetPath.IsEmpty())
	{
		// No path field - might be okay for some tools
		return Result;
	}

	// Check if asset exists
	if (!FOliveAssetResolver::Get().DoesAssetExist(AssetPath))
	{
		Result.AddError(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath),
			TEXT("Use project.search to find the correct asset path.")
		);
	}

	return Result;
}

// ============================================================================
// BT/BB Validation Rules (Phase 2)
// ============================================================================

// FOliveBTAssetTypeRule - validates BT tools target BT assets, BB tools target BB assets

FOliveValidationResult FOliveBTAssetTypeRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid() || !Params->HasField(TEXT("path")))
	{
		return Result;
	}

	FString Path = Params->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		return Result;
	}

	// For create tools, we don't validate the target type (it doesn't exist yet)
	if (ToolName.EndsWith(TEXT(".create")))
	{
		return Result;
	}

	// Load the asset to check type
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
	if (!Asset)
	{
		return Result; // Let AssetExistsRule handle this
	}

	// BT tools must target BT assets
	static TArray<FString> BTTools = {
		TEXT("behaviortree.read"), TEXT("behaviortree.set_blackboard"),
		TEXT("behaviortree.add_composite"), TEXT("behaviortree.add_task"),
		TEXT("behaviortree.add_decorator"), TEXT("behaviortree.add_service"),
		TEXT("behaviortree.remove_node"), TEXT("behaviortree.move_node"),
		TEXT("behaviortree.set_node_property")
	};

	if (BTTools.Contains(ToolName))
	{
		if (!Asset->IsA(UBehaviorTree::StaticClass()))
		{
			Result.AddError(
				TEXT("WRONG_ASSET_TYPE"),
				FString::Printf(TEXT("'%s' is not a Behavior Tree asset"), *Path),
				TEXT("Provide the path to a Behavior Tree asset")
			);
		}
	}

	// BB tools must target BB assets
	static TArray<FString> BBTools = {
		TEXT("blackboard.read"), TEXT("blackboard.add_key"),
		TEXT("blackboard.remove_key"), TEXT("blackboard.modify_key"),
		TEXT("blackboard.set_parent")
	};

	if (BBTools.Contains(ToolName))
	{
		if (!Asset->IsA(UBlackboardData::StaticClass()))
		{
			Result.AddError(
				TEXT("WRONG_ASSET_TYPE"),
				FString::Printf(TEXT("'%s' is not a Blackboard asset"), *Path),
				TEXT("Provide the path to a Blackboard Data asset")
			);
		}
	}

	return Result;
}

// FOliveBTNodeExistsRule - validates node_id parameters refer to existing nodes

FOliveValidationResult FOliveBTNodeExistsRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	FString Path;
	Params->TryGetStringField(TEXT("path"), Path);

	// Check node_id format (must be node_N)
	auto ValidateNodeIdFormat = [&Result](const FString& ParamName, const FString& NodeId)
	{
		if (NodeId.IsEmpty())
		{
			return;
		}

		if (!NodeId.StartsWith(TEXT("node_")))
		{
			Result.AddError(
				TEXT("INVALID_NODE_ID"),
				FString::Printf(TEXT("Invalid node ID format: '%s' (parameter '%s')"), *NodeId, *ParamName),
				TEXT("Node IDs must be in the format 'node_N' (e.g., node_0, node_1)")
			);
		}
	};

	if (Params->HasField(TEXT("node_id")))
	{
		ValidateNodeIdFormat(TEXT("node_id"), Params->GetStringField(TEXT("node_id")));
	}

	if (Params->HasField(TEXT("parent_node_id")))
	{
		ValidateNodeIdFormat(TEXT("parent_node_id"), Params->GetStringField(TEXT("parent_node_id")));
	}

	if (Params->HasField(TEXT("new_parent_id")))
	{
		ValidateNodeIdFormat(TEXT("new_parent_id"), Params->GetStringField(TEXT("new_parent_id")));
	}

	// Validate node class parameters for add operations
	if (ToolName == TEXT("behaviortree.add_task"))
	{
		FString TaskClass;
		Params->TryGetStringField(TEXT("task_class"), TaskClass);
		if (!TaskClass.IsEmpty() &&
			!FOliveBTNodeFactory::Get().ResolveNodeClass(TaskClass, UBTTaskNode::StaticClass()))
		{
			Result.AddError(
				TEXT("TASK_CLASS_NOT_FOUND"),
				FString::Printf(TEXT("Task class '%s' was not found"), *TaskClass),
				TEXT("Use resource olive://behaviortree/node-catalog/search?q=<query> to discover available classes")
			);
		}
	}
	else if (ToolName == TEXT("behaviortree.add_decorator"))
	{
		FString DecoratorClass;
		Params->TryGetStringField(TEXT("decorator_class"), DecoratorClass);
		if (!DecoratorClass.IsEmpty() &&
			!FOliveBTNodeFactory::Get().ResolveNodeClass(DecoratorClass, UBTDecorator::StaticClass()))
		{
			Result.AddError(
				TEXT("DECORATOR_CLASS_NOT_FOUND"),
				FString::Printf(TEXT("Decorator class '%s' was not found"), *DecoratorClass),
				TEXT("Use resource olive://behaviortree/node-catalog/search?q=<query> to discover available classes")
			);
		}
	}
	else if (ToolName == TEXT("behaviortree.add_service"))
	{
		FString ServiceClass;
		Params->TryGetStringField(TEXT("service_class"), ServiceClass);
		if (!ServiceClass.IsEmpty() &&
			!FOliveBTNodeFactory::Get().ResolveNodeClass(ServiceClass, UBTService::StaticClass()))
		{
			Result.AddError(
				TEXT("SERVICE_CLASS_NOT_FOUND"),
				FString::Printf(TEXT("Service class '%s' was not found"), *ServiceClass),
				TEXT("Use resource olive://behaviortree/node-catalog/search?q=<query> to discover available classes")
			);
		}
	}

	// Deep node checks require a valid target BT asset.
	if (Path.IsEmpty())
	{
		return Result;
	}

	UBehaviorTree* BehaviorTree = Cast<UBehaviorTree>(
		StaticLoadObject(UBehaviorTree::StaticClass(), nullptr, *Path));
	if (!BehaviorTree || !BehaviorTree->RootNode)
	{
		return Result;
	}

	FOliveIRBTNode Root = FOliveBTNodeSerializer::Get().SerializeTree(BehaviorTree);
	TSet<FString> AllNodeIds;
	TSet<FString> CompositeNodeIds;

	TFunction<void(const FOliveIRBTNode&)> CollectNodeIds = [&](const FOliveIRBTNode& Node)
	{
		AllNodeIds.Add(Node.Id);
		if (Node.NodeType == EOliveIRBTNodeType::Composite)
		{
			CompositeNodeIds.Add(Node.Id);
		}

		for (const FOliveIRBTNode& Decorator : Node.Decorators)
		{
			CollectNodeIds(Decorator);
		}
		for (const FOliveIRBTNode& Service : Node.Services)
		{
			CollectNodeIds(Service);
		}
		for (const FOliveIRBTNode& Child : Node.Children)
		{
			CollectNodeIds(Child);
		}
	};

	CollectNodeIds(Root);

	auto EnsureNodeExists = [&Result, &Params, &AllNodeIds](const FString& FieldName)
	{
		FString NodeId;
		if (Params->TryGetStringField(*FieldName, NodeId) && !NodeId.IsEmpty() && !AllNodeIds.Contains(NodeId))
		{
			Result.AddError(
				TEXT("NODE_NOT_FOUND"),
				FString::Printf(TEXT("Node '%s' was not found in this Behavior Tree"), *NodeId),
				TEXT("Call behaviortree.read to refresh current node IDs")
			);
		}
	};

	EnsureNodeExists(TEXT("node_id"));
	EnsureNodeExists(TEXT("parent_node_id"));
	EnsureNodeExists(TEXT("new_parent_id"));

	auto EnsureComposite = [&Result, &Params, &CompositeNodeIds](const FString& FieldName)
	{
		FString NodeId;
		if (Params->TryGetStringField(*FieldName, NodeId) && !NodeId.IsEmpty() && !CompositeNodeIds.Contains(NodeId))
		{
			Result.AddError(
				TEXT("PARENT_NOT_COMPOSITE"),
				FString::Printf(TEXT("Node '%s' is not a composite and cannot have children"), *NodeId),
				TEXT("Use a Selector, Sequence, or SimpleParallel node as parent")
			);
		}
	};

	if (ToolName == TEXT("behaviortree.add_composite") || ToolName == TEXT("behaviortree.add_task"))
	{
		EnsureComposite(TEXT("parent_node_id"));
	}
	else if (ToolName == TEXT("behaviortree.move_node"))
	{
		EnsureComposite(TEXT("new_parent_id"));
	}

	if (ToolName == TEXT("behaviortree.remove_node"))
	{
		FString NodeId;
		if (Params->TryGetStringField(TEXT("node_id"), NodeId) && NodeId == Root.Id)
		{
			Result.AddError(
				TEXT("CANNOT_REMOVE_ROOT"),
				TEXT("Cannot remove the root node from a Behavior Tree"),
				TEXT("Remove or move child nodes instead")
			);
		}
	}

	return Result;
}

// FOliveBBKeyUniqueRule - validates key name uniqueness for blackboard.add_key

FOliveValidationResult FOliveBBKeyUniqueRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	FString Path;
	Params->TryGetStringField(TEXT("path"), Path);
	if (Path.IsEmpty())
	{
		return Result;
	}

	UBlackboardData* Blackboard = Cast<UBlackboardData>(
		StaticLoadObject(UBlackboardData::StaticClass(), nullptr, *Path));
	if (!Blackboard)
	{
		return Result;
	}

	auto HasKeyInChain = [](const UBlackboardData* Root, const FString& KeyName, bool bLocalOnly) -> bool
	{
		const UBlackboardData* Current = Root;
		while (Current)
		{
			for (const FBlackboardEntry& Entry : Current->Keys)
			{
				if (Entry.EntryName == FName(*KeyName))
				{
					return true;
				}
			}

			if (bLocalOnly)
			{
				break;
			}

			Current = Current->Parent;
		}

		return false;
	};

	auto ValidateNonEmptyKeyName = [&Result](const FString& KeyName)
	{
		if (KeyName.IsEmpty())
		{
			Result.AddError(
				TEXT("EMPTY_KEY_NAME"),
				TEXT("Key name cannot be empty"),
				TEXT("Provide a non-empty key name")
			);
		}
	};

	if (ToolName == TEXT("blackboard.add_key"))
	{
		FString KeyName;
		Params->TryGetStringField(TEXT("name"), KeyName);
		ValidateNonEmptyKeyName(KeyName);

		if (!KeyName.IsEmpty() && HasKeyInChain(Blackboard, KeyName, false))
		{
			Result.AddError(
				TEXT("DUPLICATE_KEY"),
				FString::Printf(TEXT("Key '%s' already exists in this Blackboard or parent chain"), *KeyName),
				TEXT("Use a unique key name")
			);
		}
	}
	else if (ToolName == TEXT("blackboard.remove_key"))
	{
		FString KeyName;
		Params->TryGetStringField(TEXT("name"), KeyName);
		ValidateNonEmptyKeyName(KeyName);

		if (!KeyName.IsEmpty() && !HasKeyInChain(Blackboard, KeyName, true))
		{
			Result.AddError(
				TEXT("KEY_NOT_FOUND"),
				FString::Printf(TEXT("Key '%s' was not found in this Blackboard"), *KeyName),
				TEXT("Use blackboard.read to inspect existing keys")
			);
		}
	}
	else if (ToolName == TEXT("blackboard.modify_key"))
	{
		FString KeyName;
		Params->TryGetStringField(TEXT("name"), KeyName);
		ValidateNonEmptyKeyName(KeyName);

		if (!KeyName.IsEmpty() && !HasKeyInChain(Blackboard, KeyName, true))
		{
			Result.AddError(
				TEXT("KEY_NOT_FOUND"),
				FString::Printf(TEXT("Key '%s' was not found in this Blackboard"), *KeyName),
				TEXT("Use blackboard.read to inspect existing keys")
			);
		}

		FString NewName;
		if (Params->TryGetStringField(TEXT("new_name"), NewName) && !NewName.IsEmpty() && NewName != KeyName)
		{
			if (HasKeyInChain(Blackboard, NewName, false))
			{
				Result.AddError(
					TEXT("DUPLICATE_KEY"),
					FString::Printf(TEXT("Cannot rename key to '%s' because it already exists"), *NewName),
					TEXT("Choose a unique key name")
				);
			}
		}
	}
	else if (ToolName == TEXT("blackboard.set_parent"))
	{
		FString ParentPath;
		Params->TryGetStringField(TEXT("parent_path"), ParentPath);
		if (ParentPath.IsEmpty())
		{
			return Result;
		}

		UBlackboardData* Parent = Cast<UBlackboardData>(
			StaticLoadObject(UBlackboardData::StaticClass(), nullptr, *ParentPath));
		if (!Parent)
		{
			return Result;
		}

		if (Parent == Blackboard)
		{
			Result.AddError(
				TEXT("CIRCULAR_PARENT"),
				TEXT("A Blackboard cannot inherit from itself"),
				TEXT("Choose a different parent Blackboard")
			);
			return Result;
		}

		TSet<const UBlackboardData*> Visited;
		const UBlackboardData* Current = Parent;
		while (Current)
		{
			if (Current == Blackboard || Visited.Contains(Current))
			{
				Result.AddError(
					TEXT("CIRCULAR_PARENT"),
					TEXT("Setting this parent would create circular Blackboard inheritance"),
					TEXT("Choose a parent that does not reference this Blackboard in its ancestry")
				);
				break;
			}

			Visited.Add(Current);
			Current = Current->Parent;
		}
	}

	return Result;
}

// ============================================================================
// C++ Validation Rules (Phase 4)
// ============================================================================

void FOliveValidationEngine::RegisterCppRules()
{
	RegisterRule(MakeShared<FOliveCppPathSafetyRule>());
	RegisterRule(MakeShared<FOliveCppClassExistsRule>());
	RegisterRule(MakeShared<FOliveCppEnumExistsRule>());
	RegisterRule(MakeShared<FOliveCppStructExistsRule>());
	RegisterRule(MakeShared<FOliveCppCompileGuardRule>());

	UE_LOG(LogOliveAI, Log, TEXT("Registered C++ validation rules"));
}

// FOliveCppPathSafetyRule - validates file paths are safe for C++ source operations

FOliveValidationResult FOliveCppPathSafetyRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	FString FilePath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("file_path"), FilePath))
	{
		Result.AddError(
			TEXT("MISSING_PATH"),
			TEXT("file_path parameter is required"),
			TEXT("Provide a relative file path within the project Source/ directory")
		);
		return Result;
	}

	if (!FOliveCppSourceReader::IsPathSafe(FilePath))
	{
		Result.AddError(
			TEXT("UNSAFE_PATH"),
			FString::Printf(TEXT("Path '%s' is unsafe. Must be relative, no '..', and have a .h/.cpp/.inl extension"), *FilePath),
			TEXT("Use a relative path within the project Source/ directory")
		);
	}

	if (ToolName == TEXT("cpp.modify_source"))
	{
		FString AnchorText;
		if (!Params->TryGetStringField(TEXT("anchor_text"), AnchorText) || AnchorText.IsEmpty())
		{
			Result.AddError(
				TEXT("MISSING_ANCHOR"),
				TEXT("anchor_text parameter is required for cpp.modify_source"),
				TEXT("Provide exact text anchor to patch")
			);
		}

		FString Operation;
		if (!Params->TryGetStringField(TEXT("operation"), Operation) ||
			(Operation != TEXT("replace") && Operation != TEXT("insert_before") && Operation != TEXT("insert_after")))
		{
			Result.AddError(
				TEXT("INVALID_OPERATION"),
				TEXT("operation must be one of: replace, insert_before, insert_after"),
				TEXT("Use a valid operation value")
			);
		}
	}

	return Result;
}

// FOliveCppClassExistsRule - validates that referenced C++ classes exist in reflection

FOliveValidationResult FOliveCppClassExistsRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		Result.AddError(
			TEXT("MISSING_CLASS_NAME"),
			TEXT("class_name parameter is required"),
			TEXT("Provide the name of a C++ class (e.g., 'AActor', 'UActorComponent')")
		);
		return Result;
	}

	UClass* FoundClass = FOliveCppReflectionReader::FindClassByName(ClassName);
	if (!FoundClass)
	{
		// Use warning instead of error - class may not be loaded yet
		Result.AddWarning(
			TEXT("CLASS_NOT_FOUND"),
			FString::Printf(TEXT("C++ class '%s' was not found in reflection. It may not be loaded yet."), *ClassName),
			TEXT("Verify the class name is correct. Common base classes: AActor, UActorComponent, UObject, APawn, ACharacter")
		);
	}

	return Result;
}

// FOliveCppEnumExistsRule - validates that referenced C++ enums exist

FOliveValidationResult FOliveCppEnumExistsRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	FString EnumName;
	if (!Params->TryGetStringField(TEXT("enum_name"), EnumName) || EnumName.IsEmpty())
	{
		Result.AddError(
			TEXT("MISSING_ENUM_NAME"),
			TEXT("enum_name parameter is required"),
			TEXT("Provide the name of a C++ enum (e.g., 'ECollisionChannel')")
		);
		return Result;
	}

	// Try to find the enum by name
	UEnum* FoundEnum = FindObject<UEnum>(ANY_PACKAGE, *EnumName);
	if (!FoundEnum)
	{
		Result.AddWarning(
			TEXT("ENUM_NOT_FOUND"),
			FString::Printf(TEXT("C++ enum '%s' was not found. It may not be loaded or may use a different name."), *EnumName),
			TEXT("Use cpp.list_enums to discover available enums, or check the full path (e.g., '/Script/Engine.ECollisionChannel')")
		);
	}

	return Result;
}

// FOliveCppStructExistsRule - validates that referenced C++ structs exist

FOliveValidationResult FOliveCppStructExistsRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	FString StructName;
	if (!Params->TryGetStringField(TEXT("struct_name"), StructName) || StructName.IsEmpty())
	{
		Result.AddError(
			TEXT("MISSING_STRUCT_NAME"),
			TEXT("struct_name parameter is required"),
			TEXT("Provide the name of a C++ struct (e.g., 'FVector', 'FHitResult')")
		);
		return Result;
	}

	// Try to find the struct by name
	UScriptStruct* FoundStruct = FindObject<UScriptStruct>(ANY_PACKAGE, *StructName);
	if (!FoundStruct)
	{
		Result.AddWarning(
			TEXT("STRUCT_NOT_FOUND"),
			FString::Printf(TEXT("C++ struct '%s' was not found. It may not be loaded or may use a different name."), *StructName),
			TEXT("Use cpp.list_structs to discover available structs, or check the full path (e.g., '/Script/CoreUObject.Vector')")
		);
	}

	return Result;
}

// FOliveCppCompileGuardRule - blocks C++ write operations during active compilation

FOliveValidationResult FOliveCppCompileGuardRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding && LiveCoding->IsCompiling())
	{
		Result.AddError(
			TEXT("COMPILE_IN_PROGRESS"),
			TEXT("Cannot perform C++ write operations while Live Coding compilation is in progress."),
			TEXT("Wait for the current compilation to finish before making changes.")
		);
	}
#endif

	return Result;
}

// ============================================================================
// Cross-System Validation Rules (Phase 5)
// ============================================================================

void FOliveValidationEngine::RegisterCrossSystemRules()
{
	RegisterRule(MakeShared<FOliveBulkReadLimitRule>());
	RegisterRule(MakeShared<FOliveSnapshotExistsRule>());
	RegisterRule(MakeShared<FOliveRefactorSafetyRule>());

	UE_LOG(LogOliveAI, Log, TEXT("Registered Cross-System validation rules"));
}

// FOliveBulkReadLimitRule - limits bulk read operations to 20 assets maximum

FOliveValidationResult FOliveBulkReadLimitRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	const TArray<TSharedPtr<FJsonValue>>* PathsArray;
	if (Params.IsValid() && Params->TryGetArrayField(TEXT("paths"), PathsArray))
	{
		if (PathsArray->Num() > 20)
		{
			Result.AddError(TEXT("TOO_MANY_ASSETS"),
				FString::Printf(TEXT("Bulk read limited to 20 assets (got %d)"), PathsArray->Num()),
				TEXT("Split into multiple bulk_read calls with fewer assets"));
		}
		if (PathsArray->Num() == 0)
		{
			Result.AddError(TEXT("EMPTY_PATHS"),
				TEXT("paths array cannot be empty"),
				TEXT("Provide at least one asset path"));
		}
	}

	return Result;
}

// FOliveSnapshotExistsRule - validates that referenced snapshot IDs exist

FOliveValidationResult FOliveSnapshotExistsRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	FString SnapshotId;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("snapshot_id"), SnapshotId) || SnapshotId.IsEmpty())
	{
		Result.AddError(TEXT("MISSING_SNAPSHOT_ID"),
			TEXT("snapshot_id parameter is required"),
			TEXT("Provide a valid snapshot ID. Use project.list_snapshots to see available snapshots."));
		return Result;
	}

	TOptional<FOliveSnapshotInfo> Info = FOliveSnapshotManager::Get().GetSnapshotInfo(SnapshotId);
	if (!Info.IsSet())
	{
		Result.AddError(TEXT("SNAPSHOT_NOT_FOUND"),
			FString::Printf(TEXT("Snapshot '%s' not found"), *SnapshotId),
			TEXT("Use project.list_snapshots to see available snapshots"));
	}

	if (ToolName == TEXT("project.rollback"))
	{
		bool bPreviewOnly = true;
		Params->TryGetBoolField(TEXT("preview_only"), bPreviewOnly);
		if (!bPreviewOnly)
		{
			FString ConfirmationToken;
			if (!Params->TryGetStringField(TEXT("confirmation_token"), ConfirmationToken) || ConfirmationToken.IsEmpty())
			{
				Result.AddError(TEXT("MISSING_CONFIRMATION_TOKEN"),
					TEXT("confirmation_token is required when preview_only is false"),
					TEXT("Run project.rollback with preview_only=true first to get a token"));
			}
		}
	}

	return Result;
}

// FOliveRefactorSafetyRule - safety checks for refactoring operations

FOliveValidationResult FOliveRefactorSafetyRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		Result.AddError(TEXT("INVALID_PARAMS"), TEXT("Parameters are required"));
		return Result;
	}

	if (ToolName == TEXT("project.refactor_rename"))
	{
		FString AssetPath;
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_PATH"), TEXT("asset_path is required for rename"));
		}

		FString NewName;
		if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_NAME"), TEXT("new_name is required for rename"));
		}
		else
		{
			// Check for invalid characters (only allow letters, numbers, and underscores)
			FRegexPattern InvalidChars(TEXT("[^a-zA-Z0-9_]"));
			FRegexMatcher Matcher(InvalidChars, NewName);
			if (Matcher.FindNext())
			{
				Result.AddError(TEXT("INVALID_NAME"),
					FString::Printf(TEXT("Name '%s' contains invalid characters"), *NewName),
					TEXT("Use only letters, numbers, and underscores"));
			}
		}
	}
	else if (ToolName == TEXT("project.implement_interface"))
	{
		FString Interface;
		if (!Params->TryGetStringField(TEXT("interface"), Interface) || Interface.IsEmpty())
		{
			Result.AddError(TEXT("EMPTY_INTERFACE"), TEXT("interface name is required"));
		}

		const TArray<TSharedPtr<FJsonValue>>* PathsArray;
		if (!Params->TryGetArrayField(TEXT("paths"), PathsArray) || PathsArray->Num() == 0)
		{
			Result.AddError(TEXT("EMPTY_PATHS"), TEXT("At least one Blueprint path is required"));
		}
		else if (PathsArray->Num() > 50)
		{
			Result.AddError(TEXT("TOO_MANY_TARGETS"),
				FString::Printf(TEXT("Cannot implement interface on more than 50 assets at once (got %d)"), PathsArray->Num()),
				TEXT("Split into smaller batches"));
		}
	}
	else if (ToolName == TEXT("project.move_to_cpp"))
	{
		FString AssetPath;
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_PATH"), TEXT("asset_path is required for move_to_cpp"));
		}
		FString ModuleName;
		if (!Params->TryGetStringField(TEXT("module_name"), ModuleName) || ModuleName.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_MODULE"), TEXT("module_name is required for move_to_cpp"));
		}
		FString ClassName;
		if (!Params->TryGetStringField(TEXT("target_class_name"), ClassName) || ClassName.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_CLASS_NAME"), TEXT("target_class_name is required for move_to_cpp"));
		}
		else
		{
			FRegexPattern InvalidChars(TEXT("[^a-zA-Z0-9_]"));
			FRegexMatcher Matcher(InvalidChars, ClassName);
			if (Matcher.FindNext())
			{
				Result.AddError(TEXT("INVALID_CLASS_NAME"),
					FString::Printf(TEXT("target_class_name '%s' contains invalid characters"), *ClassName),
					TEXT("Use only letters, numbers, and underscores"));
			}
		}
	}

	return Result;
}
