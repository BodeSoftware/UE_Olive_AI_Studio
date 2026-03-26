// Copyright Bode Software. All Rights Reserved.

#include "OliveBTNodeFactory.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY(LogOliveBTWriter);

FOliveBTNodeFactory& FOliveBTNodeFactory::Get()
{
	static FOliveBTNodeFactory Instance;
	return Instance;
}

UBTCompositeNode* FOliveBTNodeFactory::CreateComposite(UObject* Outer, const FString& CompositeType)
{
	if (!Outer)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("CreateComposite: Outer is null"));
		return nullptr;
	}

	FString TypeLower = CompositeType.ToLower();

	UClass* CompositeClass = nullptr;
	if (TypeLower == TEXT("selector"))
	{
		CompositeClass = UBTComposite_Selector::StaticClass();
	}
	else if (TypeLower == TEXT("sequence"))
	{
		CompositeClass = UBTComposite_Sequence::StaticClass();
	}
	else if (TypeLower == TEXT("simpleparallel") || TypeLower == TEXT("simple_parallel"))
	{
		CompositeClass = UBTComposite_SimpleParallel::StaticClass();
	}
	else
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("CreateComposite: Unknown composite type '%s'. Use Selector, Sequence, or SimpleParallel."), *CompositeType);
		return nullptr;
	}

	UBTCompositeNode* Node = NewObject<UBTCompositeNode>(Outer, CompositeClass, NAME_None, RF_Transactional);
	UE_LOG(LogOliveBTWriter, Verbose, TEXT("Created composite node: %s"), *CompositeType);
	return Node;
}

UBTTaskNode* FOliveBTNodeFactory::CreateTask(UObject* Outer, const FString& ClassName)
{
	if (!Outer)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("CreateTask: Outer is null"));
		return nullptr;
	}

	UClass* TaskClass = ResolveNodeClass(ClassName, UBTTaskNode::StaticClass());
	if (!TaskClass)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("CreateTask: Could not resolve class '%s'"), *ClassName);
		return nullptr;
	}

	UBTTaskNode* Node = NewObject<UBTTaskNode>(Outer, TaskClass, NAME_None, RF_Transactional);
	UE_LOG(LogOliveBTWriter, Verbose, TEXT("Created task node: %s"), *TaskClass->GetName());
	return Node;
}

UBTDecorator* FOliveBTNodeFactory::CreateDecorator(UObject* Outer, const FString& ClassName)
{
	if (!Outer)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("CreateDecorator: Outer is null"));
		return nullptr;
	}

	UClass* DecoratorClass = ResolveNodeClass(ClassName, UBTDecorator::StaticClass());
	if (!DecoratorClass)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("CreateDecorator: Could not resolve class '%s'"), *ClassName);
		return nullptr;
	}

	UBTDecorator* Node = NewObject<UBTDecorator>(Outer, DecoratorClass, NAME_None, RF_Transactional);
	UE_LOG(LogOliveBTWriter, Verbose, TEXT("Created decorator: %s"), *DecoratorClass->GetName());
	return Node;
}

UBTService* FOliveBTNodeFactory::CreateService(UObject* Outer, const FString& ClassName)
{
	if (!Outer)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("CreateService: Outer is null"));
		return nullptr;
	}

	UClass* ServiceClass = ResolveNodeClass(ClassName, UBTService::StaticClass());
	if (!ServiceClass)
	{
		UE_LOG(LogOliveBTWriter, Error, TEXT("CreateService: Could not resolve class '%s'"), *ClassName);
		return nullptr;
	}

	UBTService* Node = NewObject<UBTService>(Outer, ServiceClass, NAME_None, RF_Transactional);
	UE_LOG(LogOliveBTWriter, Verbose, TEXT("Created service: %s"), *ServiceClass->GetName());
	return Node;
}

UClass* FOliveBTNodeFactory::ResolveNodeClass(const FString& ClassName, UClass* BaseClass)
{
	if (ClassName.IsEmpty() || !BaseClass)
	{
		return nullptr;
	}

	// Helper to validate a found class
	auto ValidateClass = [BaseClass](UClass* FoundClass) -> UClass*
	{
		if (FoundClass && FoundClass->IsChildOf(BaseClass) && !FoundClass->HasAnyClassFlags(CLASS_Abstract))
		{
			return FoundClass;
		}
		return nullptr;
	};

	// Strategy 1: Direct lookup
	if (UClass* Found = ValidateClass(FindObject<UClass>(nullptr, *ClassName)))
	{
		return Found;
	}

	// Strategy 2: With "U" prefix
	if (!ClassName.StartsWith(TEXT("U")))
	{
		FString WithU = TEXT("U") + ClassName;
		if (UClass* Found = ValidateClass(FindObject<UClass>(nullptr, *WithU)))
		{
			return Found;
		}
	}

	// Strategy 3: With BT type prefix based on BaseClass
	TArray<FString> Prefixes;
	if (BaseClass == UBTTaskNode::StaticClass() || BaseClass->IsChildOf(UBTTaskNode::StaticClass()))
	{
		Prefixes = { TEXT("BTTask_"), TEXT("UBTTask_") };
	}
	else if (BaseClass == UBTDecorator::StaticClass() || BaseClass->IsChildOf(UBTDecorator::StaticClass()))
	{
		Prefixes = { TEXT("BTDecorator_"), TEXT("UBTDecorator_") };
	}
	else if (BaseClass == UBTService::StaticClass() || BaseClass->IsChildOf(UBTService::StaticClass()))
	{
		Prefixes = { TEXT("BTService_"), TEXT("UBTService_") };
	}

	for (const FString& Prefix : Prefixes)
	{
		if (!ClassName.StartsWith(Prefix))
		{
			FString WithPrefix = Prefix + ClassName;
			if (UClass* Found = ValidateClass(FindObject<UClass>(nullptr, *WithPrefix)))
			{
				return Found;
			}
		}
	}

	// Strategy 4: TObjectIterator fallback - scan all classes
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* TestClass = *It;
		if (TestClass->IsChildOf(BaseClass) && !TestClass->HasAnyClassFlags(CLASS_Abstract))
		{
			FString TestName = TestClass->GetName();
			if (TestName == ClassName || TestName.EndsWith(ClassName))
			{
				UE_LOG(LogOliveBTWriter, Verbose, TEXT("Resolved '%s' to '%s' via TObjectIterator"), *ClassName, *TestName);
				return TestClass;
			}
		}
	}

	UE_LOG(LogOliveBTWriter, Warning, TEXT("Could not resolve node class '%s' with base '%s'"), *ClassName, *BaseClass->GetName());
	return nullptr;
}
