// Copyright Bode Software. All Rights Reserved.

#include "IR/BehaviorTreeIR.h"
#include "Serialization/JsonSerializer.h"

// FOliveIRBlackboardKey

TSharedPtr<FJsonObject> FOliveIRBlackboardKey::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);

	FString TypeStr;
	switch (KeyType)
	{
		case EOliveIRBlackboardKeyType::Bool: TypeStr = TEXT("bool"); break;
		case EOliveIRBlackboardKeyType::Int: TypeStr = TEXT("int"); break;
		case EOliveIRBlackboardKeyType::Float: TypeStr = TEXT("float"); break;
		case EOliveIRBlackboardKeyType::String: TypeStr = TEXT("string"); break;
		case EOliveIRBlackboardKeyType::Name: TypeStr = TEXT("name"); break;
		case EOliveIRBlackboardKeyType::Vector: TypeStr = TEXT("vector"); break;
		case EOliveIRBlackboardKeyType::Rotator: TypeStr = TEXT("rotator"); break;
		case EOliveIRBlackboardKeyType::Enum: TypeStr = TEXT("enum"); break;
		case EOliveIRBlackboardKeyType::Object: TypeStr = TEXT("object"); break;
		case EOliveIRBlackboardKeyType::Class: TypeStr = TEXT("class"); break;
		default: TypeStr = TEXT("unknown"); break;
	}
	Json->SetStringField(TEXT("key_type"), TypeStr);

	if (!BaseClass.IsEmpty())
	{
		Json->SetStringField(TEXT("base_class"), BaseClass);
	}
	if (!EnumType.IsEmpty())
	{
		Json->SetStringField(TEXT("enum_type"), EnumType);
	}
	if (bInstanceSynced)
	{
		Json->SetBoolField(TEXT("instance_synced"), true);
	}
	if (!Description.IsEmpty())
	{
		Json->SetStringField(TEXT("description"), Description);
	}

	return Json;
}

FOliveIRBlackboardKey FOliveIRBlackboardKey::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRBlackboardKey Key;
	if (!JsonObject.IsValid())
	{
		return Key;
	}

	Key.Name = JsonObject->GetStringField(TEXT("name"));

	FString TypeStr = JsonObject->GetStringField(TEXT("key_type"));
	if (TypeStr == TEXT("bool")) Key.KeyType = EOliveIRBlackboardKeyType::Bool;
	else if (TypeStr == TEXT("int")) Key.KeyType = EOliveIRBlackboardKeyType::Int;
	else if (TypeStr == TEXT("float")) Key.KeyType = EOliveIRBlackboardKeyType::Float;
	else if (TypeStr == TEXT("string")) Key.KeyType = EOliveIRBlackboardKeyType::String;
	else if (TypeStr == TEXT("name")) Key.KeyType = EOliveIRBlackboardKeyType::Name;
	else if (TypeStr == TEXT("vector")) Key.KeyType = EOliveIRBlackboardKeyType::Vector;
	else if (TypeStr == TEXT("rotator")) Key.KeyType = EOliveIRBlackboardKeyType::Rotator;
	else if (TypeStr == TEXT("enum")) Key.KeyType = EOliveIRBlackboardKeyType::Enum;
	else if (TypeStr == TEXT("object")) Key.KeyType = EOliveIRBlackboardKeyType::Object;
	else if (TypeStr == TEXT("class")) Key.KeyType = EOliveIRBlackboardKeyType::Class;
	else Key.KeyType = EOliveIRBlackboardKeyType::Unknown;

	Key.BaseClass = JsonObject->GetStringField(TEXT("base_class"));
	Key.EnumType = JsonObject->GetStringField(TEXT("enum_type"));
	Key.bInstanceSynced = JsonObject->GetBoolField(TEXT("instance_synced"));
	Key.Description = JsonObject->GetStringField(TEXT("description"));

	return Key;
}

// FOliveIRBlackboard

TSharedPtr<FJsonObject> FOliveIRBlackboard::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("path"), Path);

	if (!ParentPath.IsEmpty())
	{
		Json->SetStringField(TEXT("parent"), ParentPath);
	}

	if (Keys.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> KeysArray;
		for (const FOliveIRBlackboardKey& Key : Keys)
		{
			KeysArray.Add(MakeShared<FJsonValueObject>(Key.ToJson()));
		}
		Json->SetArrayField(TEXT("keys"), KeysArray);
	}

	return Json;
}

FOliveIRBlackboard FOliveIRBlackboard::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRBlackboard BB;
	if (!JsonObject.IsValid())
	{
		return BB;
	}

	BB.Name = JsonObject->GetStringField(TEXT("name"));
	BB.Path = JsonObject->GetStringField(TEXT("path"));
	BB.ParentPath = JsonObject->GetStringField(TEXT("parent"));

	const TArray<TSharedPtr<FJsonValue>>* KeysArray;
	if (JsonObject->TryGetArrayField(TEXT("keys"), KeysArray))
	{
		for (const auto& KeyValue : *KeysArray)
		{
			BB.Keys.Add(FOliveIRBlackboardKey::FromJson(KeyValue->AsObject()));
		}
	}

	return BB;
}

// FOliveIRBTNode

TSharedPtr<FJsonObject> FOliveIRBTNode::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("id"), Id);

	FString TypeStr;
	switch (NodeType)
	{
		case EOliveIRBTNodeType::Root: TypeStr = TEXT("root"); break;
		case EOliveIRBTNodeType::Composite: TypeStr = TEXT("composite"); break;
		case EOliveIRBTNodeType::Task: TypeStr = TEXT("task"); break;
		case EOliveIRBTNodeType::Decorator: TypeStr = TEXT("decorator"); break;
		case EOliveIRBTNodeType::Service: TypeStr = TEXT("service"); break;
		default: TypeStr = TEXT("unknown"); break;
	}
	Json->SetStringField(TEXT("node_type"), TypeStr);

	Json->SetStringField(TEXT("class"), NodeClass);
	Json->SetStringField(TEXT("title"), Title);

	if (NodeType == EOliveIRBTNodeType::Composite)
	{
		FString CompTypeStr;
		switch (CompositeType)
		{
			case EOliveIRBTCompositeType::Selector: CompTypeStr = TEXT("selector"); break;
			case EOliveIRBTCompositeType::Sequence: CompTypeStr = TEXT("sequence"); break;
			case EOliveIRBTCompositeType::SimpleParallel: CompTypeStr = TEXT("simple_parallel"); break;
			default: CompTypeStr = TEXT("unknown"); break;
		}
		Json->SetStringField(TEXT("composite_type"), CompTypeStr);
	}

	// Children
	if (Children.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ChildrenArray;
		for (const FOliveIRBTNode& Child : Children)
		{
			ChildrenArray.Add(MakeShared<FJsonValueObject>(Child.ToJson()));
		}
		Json->SetArrayField(TEXT("children"), ChildrenArray);
	}

	// Decorators
	if (Decorators.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> DecoratorsArray;
		for (const FOliveIRBTNode& Decorator : Decorators)
		{
			DecoratorsArray.Add(MakeShared<FJsonValueObject>(Decorator.ToJson()));
		}
		Json->SetArrayField(TEXT("decorators"), DecoratorsArray);
	}

	// Services
	if (Services.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ServicesArray;
		for (const FOliveIRBTNode& Service : Services)
		{
			ServicesArray.Add(MakeShared<FJsonValueObject>(Service.ToJson()));
		}
		Json->SetArrayField(TEXT("services"), ServicesArray);
	}

	// Properties
	if (Properties.Num() > 0)
	{
		TSharedPtr<FJsonObject> PropsJson = MakeShared<FJsonObject>();
		for (const auto& Pair : Properties)
		{
			PropsJson->SetStringField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(TEXT("properties"), PropsJson);
	}

	// Referenced blackboard keys
	if (ReferencedBlackboardKeys.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> KeysArray;
		for (const FString& Key : ReferencedBlackboardKeys)
		{
			KeysArray.Add(MakeShared<FJsonValueString>(Key));
		}
		Json->SetArrayField(TEXT("blackboard_keys"), KeysArray);
	}

	return Json;
}

FOliveIRBTNode FOliveIRBTNode::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRBTNode Node;
	if (!JsonObject.IsValid())
	{
		return Node;
	}

	Node.Id = JsonObject->GetStringField(TEXT("id"));
	Node.NodeClass = JsonObject->GetStringField(TEXT("class"));
	Node.Title = JsonObject->GetStringField(TEXT("title"));

	FString TypeStr = JsonObject->GetStringField(TEXT("node_type"));
	if (TypeStr == TEXT("root")) Node.NodeType = EOliveIRBTNodeType::Root;
	else if (TypeStr == TEXT("composite")) Node.NodeType = EOliveIRBTNodeType::Composite;
	else if (TypeStr == TEXT("task")) Node.NodeType = EOliveIRBTNodeType::Task;
	else if (TypeStr == TEXT("decorator")) Node.NodeType = EOliveIRBTNodeType::Decorator;
	else if (TypeStr == TEXT("service")) Node.NodeType = EOliveIRBTNodeType::Service;
	else Node.NodeType = EOliveIRBTNodeType::Unknown;

	FString CompTypeStr = JsonObject->GetStringField(TEXT("composite_type"));
	if (CompTypeStr == TEXT("selector")) Node.CompositeType = EOliveIRBTCompositeType::Selector;
	else if (CompTypeStr == TEXT("sequence")) Node.CompositeType = EOliveIRBTCompositeType::Sequence;
	else if (CompTypeStr == TEXT("simple_parallel")) Node.CompositeType = EOliveIRBTCompositeType::SimpleParallel;
	else Node.CompositeType = EOliveIRBTCompositeType::Unknown;

	// Parse children, decorators, services, properties...
	const TArray<TSharedPtr<FJsonValue>>* Array;
	if (JsonObject->TryGetArrayField(TEXT("children"), Array))
	{
		for (const auto& Value : *Array)
		{
			Node.Children.Add(FOliveIRBTNode::FromJson(Value->AsObject()));
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("decorators"), Array))
	{
		for (const auto& Value : *Array)
		{
			Node.Decorators.Add(FOliveIRBTNode::FromJson(Value->AsObject()));
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("services"), Array))
	{
		for (const auto& Value : *Array)
		{
			Node.Services.Add(FOliveIRBTNode::FromJson(Value->AsObject()));
		}
	}
	if (JsonObject->TryGetArrayField(TEXT("blackboard_keys"), Array))
	{
		for (const auto& Value : *Array)
		{
			Node.ReferencedBlackboardKeys.Add(Value->AsString());
		}
	}

	const TSharedPtr<FJsonObject>* PropsJson;
	if (JsonObject->TryGetObjectField(TEXT("properties"), PropsJson))
	{
		for (const auto& Pair : (*PropsJson)->Values)
		{
			Node.Properties.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	return Node;
}

// FOliveIRBehaviorTree

TSharedPtr<FJsonObject> FOliveIRBehaviorTree::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Name);
	Json->SetStringField(TEXT("path"), Path);

	if (!BlackboardPath.IsEmpty())
	{
		Json->SetStringField(TEXT("blackboard"), BlackboardPath);
	}

	Json->SetObjectField(TEXT("root"), Root.ToJson());

	// Used classes
	auto AddClassArray = [&Json](const FString& FieldName, const TArray<FString>& Classes)
	{
		if (Classes.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Array;
			for (const FString& Class : Classes)
			{
				Array.Add(MakeShared<FJsonValueString>(Class));
			}
			Json->SetArrayField(FieldName, Array);
		}
	};

	AddClassArray(TEXT("used_tasks"), UsedTaskClasses);
	AddClassArray(TEXT("used_decorators"), UsedDecoratorClasses);
	AddClassArray(TEXT("used_services"), UsedServiceClasses);

	return Json;
}

FOliveIRBehaviorTree FOliveIRBehaviorTree::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRBehaviorTree BT;
	if (!JsonObject.IsValid())
	{
		return BT;
	}

	BT.Name = JsonObject->GetStringField(TEXT("name"));
	BT.Path = JsonObject->GetStringField(TEXT("path"));
	BT.BlackboardPath = JsonObject->GetStringField(TEXT("blackboard"));

	const TSharedPtr<FJsonObject>* RootJson;
	if (JsonObject->TryGetObjectField(TEXT("root"), RootJson))
	{
		BT.Root = FOliveIRBTNode::FromJson(*RootJson);
	}

	auto ParseClassArray = [&JsonObject](const FString& FieldName, TArray<FString>& OutClasses)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array;
		if (JsonObject->TryGetArrayField(FieldName, Array))
		{
			for (const auto& Value : *Array)
			{
				OutClasses.Add(Value->AsString());
			}
		}
	};

	ParseClassArray(TEXT("used_tasks"), BT.UsedTaskClasses);
	ParseClassArray(TEXT("used_decorators"), BT.UsedDecoratorClasses);
	ParseClassArray(TEXT("used_services"), BT.UsedServiceClasses);

	return BT;
}
