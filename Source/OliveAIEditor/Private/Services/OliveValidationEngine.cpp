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
#include "UObject/UObjectHash.h"
#include "Internationalization/Regex.h"
#include "Settings/OliveAISettings.h"
#include "Engine/Blueprint.h"
#include "PCGGraph.h"

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
	// Removed in AI Freedom update — redundant with handler-level checks
	// RegisterRule(MakeShared<FOliveAssetExistsRule>());
	RegisterRule(MakeShared<FOliveWriteRateLimitRule>());
}

void FOliveValidationEngine::RegisterBlueprintRules()
{
	RegisterRule(MakeShared<FOliveBPAssetTypeRule>());
	// Removed in AI Freedom update — handler errors are better and more contextual
	// RegisterRule(MakeShared<FOliveBPNodeIdFormatRule>());
	RegisterRule(MakeShared<FOliveBPNamingRule>());

	UE_LOG(LogOliveAI, Log, TEXT("Registered Blueprint validation rules"));
}

void FOliveValidationEngine::RegisterBehaviorTreeRules()
{
	RegisterRule(MakeShared<FOliveBTAssetTypeRule>());
	RegisterRule(MakeShared<FOliveBTNodeExistsRule>());
	RegisterRule(MakeShared<FOliveBBKeyUniqueRule>());
}

void FOliveValidationEngine::RegisterPCGRules()
{
	RegisterRule(MakeShared<FOlivePCGAssetTypeRule>());
	RegisterRule(MakeShared<FOlivePCGNodeClassRule>());

	UE_LOG(LogOliveAI, Log, TEXT("Registered PCG validation rules"));
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
			TEXT("blueprint.get_node_pins"),
			TEXT("blueprint.describe_node_type"),
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

	const TSharedPtr<FJsonObject>& Schema = *SchemaPtr;

	// Check required fields
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

	// If params are null, no further validation possible
	if (!Params.IsValid())
	{
		return Result;
	}

	// Get properties sub-schema
	const TSharedPtr<FJsonObject>* PropertiesPtr;
	if (!Schema->TryGetObjectField(TEXT("properties"), PropertiesPtr))
	{
		return Result;
	}

	const TSharedPtr<FJsonObject>& Properties = *PropertiesPtr;

	// Type-check each present parameter against schema
	TArray<FString> ParamKeys;
	Params->Values.GetKeys(ParamKeys);

	for (const FString& Key : ParamKeys)
	{
		const TSharedPtr<FJsonObject>* PropertySchema;
		if (!Properties->TryGetObjectField(Key, PropertySchema))
		{
			// Check additionalProperties
			bool bAdditionalProperties = true;
			Schema->TryGetBoolField(TEXT("additionalProperties"), bAdditionalProperties);

			if (!bAdditionalProperties)
			{
				TArray<FString> AllowedFields;
				Properties->Values.GetKeys(AllowedFields);

				Result.AddError(
					TEXT("UNEXPECTED_FIELD"),
					FString::Printf(TEXT("Unexpected field: '%s'"), *Key),
					FString::Printf(TEXT("Remove the '%s' field. Allowed fields: %s"),
						*Key, *FString::Join(AllowedFields, TEXT(", ")))
				);
			}
			continue;
		}

		const TSharedPtr<FJsonValue>& Value = Params->Values[Key];

		// Type validation
		ValidateType(Key, Value, *PropertySchema, Result);

		// Enum validation
		const TArray<TSharedPtr<FJsonValue>>* EnumValues;
		if ((*PropertySchema)->TryGetArrayField(TEXT("enum"), EnumValues))
		{
			ValidateEnum(Key, Value, *EnumValues, Result);
		}

		// Nested object validation (one level)
		FString TypeStr;
		if ((*PropertySchema)->TryGetStringField(TEXT("type"), TypeStr) && TypeStr == TEXT("object"))
		{
			const TSharedPtr<FJsonObject>* NestedProperties;
			if ((*PropertySchema)->TryGetObjectField(TEXT("properties"), NestedProperties) && Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject>& NestedObj = Value->AsObject();
				ValidateObject(Key, NestedObj, *PropertySchema, Result);
			}
		}
	}

	return Result;
}

void FOliveSchemaValidationRule::ValidateType(
	const FString& FieldName,
	const TSharedPtr<FJsonValue>& Value,
	const TSharedPtr<FJsonObject>& PropertySchema,
	FOliveValidationResult& Result) const
{
	FString ExpectedType;
	if (!PropertySchema->TryGetStringField(TEXT("type"), ExpectedType))
	{
		return; // No type constraint
	}

	if (!IsTypeMatch(Value, ExpectedType))
	{
		FString ActualType;
		switch (Value->Type)
		{
		case EJson::String: ActualType = TEXT("string"); break;
		case EJson::Number: ActualType = TEXT("number"); break;
		case EJson::Boolean: ActualType = TEXT("boolean"); break;
		case EJson::Object: ActualType = TEXT("object"); break;
		case EJson::Array: ActualType = TEXT("array"); break;
		case EJson::Null: ActualType = TEXT("null"); break;
		default: ActualType = TEXT("unknown"); break;
		}

		Result.AddError(
			TEXT("INVALID_TYPE"),
			FString::Printf(TEXT("Field '%s': expected type '%s' but got '%s'"), *FieldName, *ExpectedType, *ActualType),
			FString::Printf(TEXT("Provide a %s value for '%s'"), *ExpectedType, *FieldName)
		);
	}
}

void FOliveSchemaValidationRule::ValidateEnum(
	const FString& FieldName,
	const TSharedPtr<FJsonValue>& Value,
	const TArray<TSharedPtr<FJsonValue>>& EnumValues,
	FOliveValidationResult& Result) const
{
	FString ValueStr;
	if (!Value->TryGetString(ValueStr))
	{
		return; // Enum validation only applies to string values
	}

	bool bFound = false;
	TArray<FString> AllowedValues;
	for (const TSharedPtr<FJsonValue>& EnumVal : EnumValues)
	{
		FString EnumStr;
		if (EnumVal->TryGetString(EnumStr))
		{
			AllowedValues.Add(EnumStr);
			if (EnumStr == ValueStr)
			{
				bFound = true;
			}
		}
	}

	if (!bFound)
	{
		Result.AddError(
			TEXT("INVALID_ENUM_VALUE"),
			FString::Printf(TEXT("Field '%s': value '%s' is not in allowed values"), *FieldName, *ValueStr),
			FString::Printf(TEXT("Allowed values for '%s': %s"), *FieldName, *FString::Join(AllowedValues, TEXT(", ")))
		);
	}
}

void FOliveSchemaValidationRule::ValidateObject(
	const FString& Prefix,
	const TSharedPtr<FJsonObject>& Value,
	const TSharedPtr<FJsonObject>& Schema,
	FOliveValidationResult& Result) const
{
	if (!Value.IsValid() || !Schema.IsValid())
	{
		return;
	}

	const TSharedPtr<FJsonObject>* NestedProperties;
	if (!Schema->TryGetObjectField(TEXT("properties"), NestedProperties))
	{
		return;
	}

	// Check required fields in nested object
	const TArray<TSharedPtr<FJsonValue>>* RequiredFields;
	if (Schema->TryGetArrayField(TEXT("required"), RequiredFields))
	{
		for (const TSharedPtr<FJsonValue>& RequiredField : *RequiredFields)
		{
			FString FieldName = RequiredField->AsString();
			if (!Value->HasField(FieldName))
			{
				Result.AddError(
					TEXT("MISSING_REQUIRED_FIELD"),
					FString::Printf(TEXT("Missing required field: %s.%s"), *Prefix, *FieldName),
					FString::Printf(TEXT("Add the '%s' field inside '%s'"), *FieldName, *Prefix)
				);
			}
		}
	}

	// Type-check nested fields
	TArray<FString> NestedKeys;
	Value->Values.GetKeys(NestedKeys);

	for (const FString& Key : NestedKeys)
	{
		const TSharedPtr<FJsonObject>* PropSchema;
		if (!(*NestedProperties)->TryGetObjectField(Key, PropSchema))
		{
			bool bAdditionalProperties = true;
			Schema->TryGetBoolField(TEXT("additionalProperties"), bAdditionalProperties);
			if (!bAdditionalProperties)
			{
				Result.AddError(
					TEXT("UNEXPECTED_FIELD"),
					FString::Printf(TEXT("Unexpected field: '%s.%s'"), *Prefix, *Key),
					FString::Printf(TEXT("Remove '%s' from '%s'"), *Key, *Prefix)
				);
			}
			continue;
		}

		ValidateType(FString::Printf(TEXT("%s.%s"), *Prefix, *Key), Value->Values[Key], *PropSchema, Result);
	}
}

bool FOliveSchemaValidationRule::IsTypeMatch(const TSharedPtr<FJsonValue>& Value, const FString& ExpectedType)
{
	if (!Value.IsValid())
	{
		return false;
	}

	if (ExpectedType == TEXT("string"))
	{
		return Value->Type == EJson::String;
	}
	else if (ExpectedType == TEXT("number") || ExpectedType == TEXT("integer"))
	{
		return Value->Type == EJson::Number;
	}
	else if (ExpectedType == TEXT("boolean"))
	{
		return Value->Type == EJson::Boolean;
	}
	else if (ExpectedType == TEXT("object"))
	{
		return Value->Type == EJson::Object;
	}
	else if (ExpectedType == TEXT("array"))
	{
		return Value->Type == EJson::Array;
	}

	return true; // Unknown type - don't reject
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
	// Relaxed in AI Freedom update — class resolution downgraded to warning.
	// Blueprint-only BT node classes may not be loaded when this rule runs.
	// Let the handler attempt creation; if UE5 rejects, it returns UE5's error.
	if (ToolName == TEXT("behaviortree.add_task"))
	{
		FString TaskClass;
		Params->TryGetStringField(TEXT("task_class"), TaskClass);
		if (!TaskClass.IsEmpty() &&
			!FOliveBTNodeFactory::Get().ResolveNodeClass(TaskClass, UBTTaskNode::StaticClass()))
		{
			Result.AddWarning(
				TEXT("TASK_CLASS_NOT_FOUND"),
				FString::Printf(TEXT("Task class '%s' was not found. It may not be loaded yet."), *TaskClass),
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
			Result.AddWarning(
				TEXT("DECORATOR_CLASS_NOT_FOUND"),
				FString::Printf(TEXT("Decorator class '%s' was not found. It may not be loaded yet."), *DecoratorClass),
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
			Result.AddWarning(
				TEXT("SERVICE_CLASS_NOT_FOUND"),
				FString::Printf(TEXT("Service class '%s' was not found. It may not be loaded yet."), *ServiceClass),
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
	UEnum* FoundEnum = FindObject<UEnum>(nullptr, *EnumName);
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
	UScriptStruct* FoundStruct = FindObject<UScriptStruct>(nullptr, *StructName);
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
	RegisterRule(MakeShared<FOliveRefactorSafetyRule>());
	// Removed in AI Freedom update — dead code, preferred_layer is never set
	// RegisterRule(MakeShared<FOliveCppOnlyModeRule>());
	RegisterRule(MakeShared<FOliveDuplicateLayerRule>());

	UE_LOG(LogOliveAI, Log, TEXT("Registered Cross-System validation rules"));
}

void FOliveValidationEngine::RegisterNiagaraRules()
{
	RegisterRule(MakeShared<FOliveNiagaraAssetTypeRule>());
	RegisterRule(MakeShared<FOliveNiagaraStageValidRule>());
	RegisterRule(MakeShared<FOliveNiagaraModuleExistsRule>());
	UE_LOG(LogOliveAI, Log, TEXT("Registered Niagara validation rules"));
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

// FOliveWriteRateLimitRule - rate limits write operations

FOliveValidationResult FOliveWriteRateLimitRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	const UOliveAISettings* Settings = UOliveAISettings::Get();
	if (!Settings || Settings->MaxWriteOpsPerMinute <= 0)
	{
		return Result; // Unlimited
	}

	const double Now = FPlatformTime::Seconds();
	const double WindowSeconds = 60.0;

	FScopeLock Lock(&TimestampLock);

	// Prune timestamps older than 60 seconds
	WriteTimestamps.RemoveAll([Now, WindowSeconds](double Timestamp)
	{
		return (Now - Timestamp) > WindowSeconds;
	});

	if (WriteTimestamps.Num() >= Settings->MaxWriteOpsPerMinute)
	{
		// Calculate when the oldest timestamp will expire
		const double OldestInWindow = WriteTimestamps[0];
		const double RetryAfter = WindowSeconds - (Now - OldestInWindow);

		Result.AddError(
			TEXT("RATE_LIMITED"),
			FString::Printf(TEXT("Write rate limit exceeded: %d operations in the last 60 seconds (max: %d)"),
				WriteTimestamps.Num(), Settings->MaxWriteOpsPerMinute),
			FString::Printf(TEXT("Retry after %.0f seconds, or increase 'Max Write Ops Per Minute' in Project Settings > Plugins > Olive AI Studio"), FMath::CeilToDouble(RetryAfter))
		);
		return Result;
	}

	// Record this write timestamp
	WriteTimestamps.Add(Now);

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

// FOliveCppOnlyModeRule - blocks BP creation when preferred_layer=cpp

FOliveValidationResult FOliveCppOnlyModeRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	FString PreferredLayer;
	if (!Params->TryGetStringField(TEXT("preferred_layer"), PreferredLayer))
	{
		return Result; // No preference set, allow everything
	}

	if (PreferredLayer != TEXT("cpp"))
	{
		return Result; // Not in C++-only mode
	}

	// In C++-only mode, block Blueprint/BT/PCG asset creation tools
	FString Suggestion;
	if (ToolName.StartsWith(TEXT("blueprint.")))
	{
		Suggestion = TEXT("Use cpp.create_class, cpp.add_property, or cpp.add_function instead");
	}
	else if (ToolName.StartsWith(TEXT("behaviortree.")))
	{
		Suggestion = TEXT("Behavior Trees are Blueprint-based assets. In C++-only mode, implement AI logic in C++ using UBTTask_BlueprintBase subclasses");
	}
	else if (ToolName.StartsWith(TEXT("pcg.")))
	{
		Suggestion = TEXT("PCG graphs are Blueprint-based assets. In C++-only mode, implement PCG logic in C++ using UPCGSettings subclasses");
	}
	else
	{
		Suggestion = TEXT("Use the equivalent C++ tool instead");
	}

	Result.AddError(
		TEXT("CPP_ONLY_MODE"),
		FString::Printf(TEXT("Tool '%s' is blocked because preferred_layer=cpp is set"), *ToolName),
		Suggestion
	);

	return Result;
}

// ============================================================================
// FOliveDuplicateLayerRule - warns about cross-layer duplication
// ============================================================================

FOliveValidationResult FOliveDuplicateLayerRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	bool bAllowDuplicate = false;
	Params->TryGetBoolField(TEXT("allow_duplicate"), bAllowDuplicate);

	// Blueprint tools: check if C++ parent already has this
	if (ToolName.StartsWith(TEXT("blueprint.")))
	{
		if (!TargetAsset)
		{
			return Result;
		}

		FString Name;
		if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return Result;
		}

		UBlueprint* Blueprint = Cast<UBlueprint>(TargetAsset);
		if (!Blueprint || !Blueprint->ParentClass)
		{
			return Result;
		}

		UClass* ParentClass = Blueprint->ParentClass;

		const bool bIsAddFunction = (ToolName == TEXT("blueprint.add_function"));
		const bool bIsAddVariable = (ToolName == TEXT("blueprint.add_variable"));

		// Determine function_type for unified blueprint.add_function tool
		FString FunctionType;
		if (bIsAddFunction)
		{
			Params->TryGetStringField(TEXT("function_type"), FunctionType);
			if (FunctionType.IsEmpty()) { FunctionType = TEXT("function"); }
		}

		const bool bIsOverrideFunction = (bIsAddFunction && FunctionType == TEXT("override"));
		const bool bIsAddDispatcher = (bIsAddFunction && FunctionType == TEXT("event_dispatcher"));
		const bool bIsAddRegularFunction = (bIsAddFunction && !bIsOverrideFunction && !bIsAddDispatcher);

		if (bIsAddRegularFunction)
		{
			// Check if parent C++ class has a UFUNCTION with this name
			UFunction* ExistingFunc = ParentClass->FindFunctionByName(FName(*Name));
			if (ExistingFunc && ExistingFunc->HasAnyFunctionFlags(FUNC_Native))
			{
				// Relaxed in AI Freedom update — always warn, never block.
				// bAllowDuplicate kept for backward compat but is a no-op for error/warning decision.
				Result.AddWarning(
					TEXT("DUPLICATE_LAYER"),
					FString::Printf(TEXT("C++ parent class '%s' already has a native function '%s'. Creating a Blueprint version may cause confusion or shadowing."),
						*ParentClass->GetName(), *Name),
					FString::Printf(TEXT("Prefer blueprint.add_function with function_type='override' for '%s' if overriding is intended."), *Name)
				);
			}
		}
		else if (bIsAddVariable || bIsAddDispatcher)
		{
			// Check if parent C++ class has a UPROPERTY with this name
			FProperty* ExistingProp = ParentClass->FindPropertyByName(FName(*Name));
			if (ExistingProp)
			{
				// Relaxed in AI Freedom update — always warn, never block.
				// bAllowDuplicate kept for backward compat but is a no-op for error/warning decision.
				Result.AddWarning(
					TEXT("DUPLICATE_LAYER"),
					FString::Printf(TEXT("C++ parent class '%s' already has a property '%s'. Creating a Blueprint member with the same name may shadow it."),
						*ParentClass->GetName(), *Name),
					TEXT("Choose a different name to avoid shadowing, or use the existing C++ property.")
				);
			}
		}
	}
	// C++ tools: check if any Blueprint extends this class and already has matching functionality
	else if (ToolName.StartsWith(TEXT("cpp.")))
	{
		const bool bIsAddFunction = (ToolName == TEXT("cpp.add_function"));
		const bool bIsAddProperty = (ToolName == TEXT("cpp.add_property"));
		if (!bIsAddFunction && !bIsAddProperty)
		{
			return Result;
		}

		FString MemberName;
		if (bIsAddFunction)
		{
			Params->TryGetStringField(TEXT("function_name"), MemberName);
		}
		else
		{
			Params->TryGetStringField(TEXT("property_name"), MemberName);
		}
		if (MemberName.IsEmpty())
		{
			return Result;
		}

		// Resolve class from file_path by basename (best-effort) or from class_name if present.
		FString ClassName;
		Params->TryGetStringField(TEXT("class_name"), ClassName);
		if (ClassName.IsEmpty())
		{
			FString FilePath;
			if (Params->TryGetStringField(TEXT("file_path"), FilePath) && !FilePath.IsEmpty())
			{
				ClassName = FPaths::GetBaseFilename(FilePath);
			}
		}
		if (ClassName.IsEmpty())
		{
			return Result;
		}

		UClass* TargetClass = FindObject<UClass>(nullptr, *ClassName);
		if (!TargetClass)
		{
			return Result; // Class may not be loaded; leave to other rules/tools.
		}

		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(TargetClass, DerivedClasses, true);

		for (UClass* Derived : DerivedClasses)
		{
			if (!Derived || !Derived->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
			{
				continue;
			}

			if (bIsAddFunction)
			{
				UFunction* BPFunc = Derived->FindFunctionByName(FName(*MemberName), EIncludeSuperFlag::ExcludeSuper);
				if (BPFunc && !BPFunc->HasAnyFunctionFlags(FUNC_Native))
				{
					// Relaxed in AI Freedom update — always warn, never block.
					// bAllowDuplicate kept for backward compat but is a no-op for error/warning decision.
					Result.AddWarning(
						TEXT("DUPLICATE_LAYER"),
						FString::Printf(TEXT("Blueprint '%s' (derived from '%s') already defines function '%s'. Adding it to C++ may create a conflict."),
							*Derived->GetName(), *ClassName, *MemberName),
						TEXT("Prefer renaming or migrating the Blueprint implementation to an override.")
					);
					break;
				}
			}
			else
			{
				FProperty* BPProp = Derived->FindPropertyByName(FName(*MemberName));
				if (BPProp)
				{
					// Relaxed in AI Freedom update — always warn, never block.
					// bAllowDuplicate kept for backward compat but is a no-op for error/warning decision.
					Result.AddWarning(
						TEXT("DUPLICATE_LAYER"),
						FString::Printf(TEXT("Blueprint '%s' (derived from '%s') already defines property '%s'. Adding it to C++ may create shadowing/confusion."),
							*Derived->GetName(), *ClassName, *MemberName),
						TEXT("Prefer renaming or migrating the Blueprint variable.")
					);
					break;
				}
			}
		}
	}

	return Result;
}

// ============================================================================
// Blueprint Validation Rules
// ============================================================================

// FOliveBPAssetTypeRule - validates target is a Blueprint

FOliveValidationResult FOliveBPAssetTypeRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!TargetAsset)
	{
		// Asset resolution happens elsewhere; skip if not resolved yet
		return Result;
	}

	UBlueprint* Blueprint = Cast<UBlueprint>(TargetAsset);
	if (!Blueprint)
	{
		// Relaxed in AI Freedom update — read tools get a warning, write tools get an error.
		// Reading a non-Blueprint asset should return "not a Blueprint" data, not block entirely.
		const bool bIsReadTool = ToolName.StartsWith(TEXT("blueprint.read"))
			|| ToolName.StartsWith(TEXT("blueprint.get_"))
			|| ToolName.StartsWith(TEXT("blueprint.list_"));

		if (bIsReadTool)
		{
			Result.AddWarning(
				TEXT("WRONG_ASSET_TYPE"),
				FString::Printf(TEXT("Tool '%s' expects a Blueprint asset, but got '%s'"),
					*ToolName, *TargetAsset->GetClass()->GetName()),
				TEXT("Check the asset path points to a Blueprint (.uasset created from a Blueprint class)")
			);
		}
		else
		{
			Result.AddError(
				TEXT("WRONG_ASSET_TYPE"),
				FString::Printf(TEXT("Tool '%s' requires a Blueprint asset, but got '%s'"),
					*ToolName, *TargetAsset->GetClass()->GetName()),
				TEXT("Check the asset path points to a Blueprint (.uasset created from a Blueprint class)")
			);
		}
	}

	return Result;
}

// FOliveBPNodeIdFormatRule - validates node_id format

FOliveValidationResult FOliveBPNodeIdFormatRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	// Check node_id format for tools that need it
	if (ToolName == TEXT("blueprint.remove_node") || ToolName == TEXT("blueprint.set_node_property"))
	{
		FString NodeId;
		if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_NODE_ID"),
				TEXT("node_id parameter is required"),
				TEXT("Use blueprint.read_function or blueprint.read_event_graph to discover node IDs"));
		}
	}

	// Check source/target node IDs for pin operations
	if (ToolName == TEXT("blueprint.connect_pins") || ToolName == TEXT("blueprint.disconnect_pins"))
	{
		FString SourceNode, TargetNode;
		if (!Params->TryGetStringField(TEXT("source_node"), SourceNode) || SourceNode.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_SOURCE_NODE"),
				TEXT("source_node parameter is required for pin operations"),
				TEXT("Use blueprint.read_function to discover node IDs"));
		}
		if (!Params->TryGetStringField(TEXT("target_node"), TargetNode) || TargetNode.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_TARGET_NODE"),
				TEXT("target_node parameter is required for pin operations"),
				TEXT("Use blueprint.read_function to discover node IDs"));
		}

		FString SourcePin, TargetPin;
		if (!Params->TryGetStringField(TEXT("source_pin"), SourcePin) || SourcePin.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_SOURCE_PIN"),
				TEXT("source_pin parameter is required for pin operations"));
		}
		if (!Params->TryGetStringField(TEXT("target_pin"), TargetPin) || TargetPin.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_TARGET_PIN"),
				TEXT("target_pin parameter is required for pin operations"));
		}
	}

	// Check node_id and pin_name for set_pin_default
	if (ToolName == TEXT("blueprint.set_pin_default"))
	{
		FString NodeId;
		if (!Params->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_NODE_ID"),
				TEXT("node_id parameter is required for set_pin_default"));
		}
		FString PinName;
		if (!Params->TryGetStringField(TEXT("pin_name"), PinName) || PinName.IsEmpty())
		{
			Result.AddError(TEXT("MISSING_PIN_NAME"),
				TEXT("pin_name parameter is required for set_pin_default"));
		}
	}

	return Result;
}

// FOliveBPNamingRule - validates naming conventions

FOliveValidationResult FOliveBPNamingRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		Result.AddError(TEXT("MISSING_NAME"),
			FString::Printf(TEXT("name parameter is required for %s"), *ToolName));
		return Result;
	}

	// Check for invalid characters (allow letters, numbers, underscores)
	FRegexPattern InvalidChars(TEXT("[^a-zA-Z0-9_]"));
	FRegexMatcher Matcher(InvalidChars, Name);
	if (Matcher.FindNext())
	{
		Result.AddError(TEXT("INVALID_NAME"),
			FString::Printf(TEXT("Name '%s' contains invalid characters"), *Name),
			TEXT("Use only letters, numbers, and underscores"));
		return Result;
	}

	// Check name doesn't start with a number
	if (Name.Len() > 0 && FChar::IsDigit(Name[0]))
	{
		Result.AddError(TEXT("INVALID_NAME"),
			FString::Printf(TEXT("Name '%s' cannot start with a digit"), *Name),
			TEXT("Start names with a letter or underscore"));
	}

	// Removed in AI Freedom update — single-char names like X, Y, Z are valid
	// in math-heavy Blueprints. No short-name warning.

	return Result;
}

// ============================================================================
// PCG Validation Rules
// ============================================================================

// FOlivePCGAssetTypeRule - validates target is a PCG graph

FOliveValidationResult FOlivePCGAssetTypeRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!TargetAsset)
	{
		return Result;
	}

	if (!TargetAsset->IsA<UPCGGraphInterface>())
	{
		Result.AddError(
			TEXT("WRONG_ASSET_TYPE"),
			FString::Printf(TEXT("Tool '%s' requires a PCG graph asset, but got '%s'"),
				*ToolName, *TargetAsset->GetClass()->GetName()),
			TEXT("Check the asset path points to a PCG graph")
		);
	}

	return Result;
}

// FOlivePCGNodeClassRule - validates PCG settings class

FOliveValidationResult FOlivePCGNodeClassRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	FString SettingsClass;
	if (!Params->TryGetStringField(TEXT("settings_class"), SettingsClass) || SettingsClass.IsEmpty())
	{
		Result.AddError(TEXT("MISSING_SETTINGS_CLASS"),
			TEXT("settings_class parameter is required for pcg.add_node"),
			TEXT("Specify the PCG settings class name (e.g., 'SurfaceSampler', 'StaticMeshSpawner', 'PointFilter')"));
		return Result;
	}

	// Try to find the class - check common naming patterns
	FString FullClassName = FString::Printf(TEXT("PCGSettings_%s"), *SettingsClass);
	UClass* FoundClass = FindObject<UClass>(nullptr, *FullClassName);
	if (!FoundClass)
	{
		// Try with UPCGSettings prefix
		FullClassName = FString::Printf(TEXT("UPCGSettings_%s"), *SettingsClass);
		FoundClass = FindObject<UClass>(nullptr, *FullClassName);
	}
	if (!FoundClass)
	{
		// Try the exact name provided
		FoundClass = FindObject<UClass>(nullptr, *SettingsClass);
	}

	if (!FoundClass)
	{
		Result.AddWarning(TEXT("UNKNOWN_SETTINGS_CLASS"),
			FString::Printf(TEXT("PCG settings class '%s' was not found. It may not be loaded or uses a different name."), *SettingsClass),
			TEXT("Common PCG settings: SurfaceSampler, StaticMeshSpawner, PointFilter, PointDensityFilter, AttributeNoise, Difference, Intersection, Union"));
	}

	return Result;
}

// ============================================================================
// Niagara Validation Rules
// ============================================================================

// FOliveNiagaraAssetTypeRule - validates target is a Niagara system
// DESIGN NOTE: We check the asset path parameter rather than TargetAsset because
// the Niagara tool handlers load the asset themselves. TargetAsset is not pre-loaded
// for Niagara tools (unlike Blueprint). The path non-empty check here is a lightweight
// guard; the tool handler performs the actual UNiagaraSystem load.

FOliveValidationResult FOliveNiagaraAssetTypeRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	// If a TargetAsset was pre-loaded, verify it is a Niagara system via class name
	// (avoids a hard dependency on NiagaraSystem.h in this file)
	if (TargetAsset)
	{
		const FString ClassName = TargetAsset->GetClass()->GetName();
		if (!ClassName.Contains(TEXT("NiagaraSystem")))
		{
			Result.AddError(
				TEXT("WRONG_ASSET_TYPE"),
				FString::Printf(TEXT("Tool '%s' requires a Niagara System asset, but got '%s'"),
					*ToolName, *ClassName),
				TEXT("Check the asset path points to a UNiagaraSystem asset")
			);
		}
		return Result;
	}

	// No pre-loaded asset: validate that the path parameter is present and non-empty
	if (!Params.IsValid())
	{
		return Result;
	}

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		Result.AddError(
			TEXT("MISSING_PATH"),
			TEXT("Missing 'path' parameter"),
			TEXT("Provide the content path to a Niagara System asset (e.g., '/Game/Effects/MySystem')")
		);
	}

	return Result;
}

// FOliveNiagaraStageValidRule - validates stage is a recognized Niagara simulation stage

FOliveValidationResult FOliveNiagaraStageValidRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	FString Stage;
	if (!Params->TryGetStringField(TEXT("stage"), Stage) || Stage.IsEmpty())
	{
		// Required param check is handled by SchemaValidation rule; skip here
		return Result;
	}

	// Accept both PascalCase and snake_case spellings
	static const TArray<FString> ValidStages = {
		TEXT("EmitterSpawn"),   TEXT("emitter_spawn"),
		TEXT("EmitterUpdate"),  TEXT("emitter_update"),
		TEXT("ParticleSpawn"),  TEXT("particle_spawn"),
		TEXT("ParticleUpdate"), TEXT("particle_update"),
		TEXT("SystemSpawn"),    TEXT("system_spawn"),
		TEXT("SystemUpdate"),   TEXT("system_update")
	};

	if (!ValidStages.Contains(Stage))
	{
		Result.AddError(
			TEXT("INVALID_NIAGARA_STAGE"),
			FString::Printf(TEXT("'%s' is not a valid Niagara stage"), *Stage),
			TEXT("Valid stages: EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, SystemSpawn, SystemUpdate")
		);
	}

	return Result;
}

// FOliveNiagaraModuleExistsRule - validates the module script parameter is present
// Full resolution (asset path → UNiagaraScript*) is deferred to the tool handler
// where the module catalog can perform fuzzy matching.

FOliveValidationResult FOliveNiagaraModuleExistsRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	if (!Params.IsValid())
	{
		return Result;
	}

	FString Module;
	if (!Params->TryGetStringField(TEXT("module"), Module) || Module.IsEmpty())
	{
		Result.AddError(
			TEXT("MISSING_MODULE"),
			TEXT("Missing 'module' parameter"),
			TEXT("Provide the Niagara module script name or content path (e.g., 'Drag' or '/Niagara/Modules/Forces/Drag')")
		);
	}

	return Result;
}
