// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CommonIR.h"
#include "BehaviorTreeIR.generated.h"

/**
 * Blackboard key type
 */
UENUM(BlueprintType)
enum class EOliveIRBlackboardKeyType : uint8
{
	Bool,
	Int,
	Float,
	String,
	Name,
	Vector,
	Rotator,
	Enum,
	Object,
	Class,
	Unknown
};

/**
 * Blackboard key definition
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBlackboardKey
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	EOliveIRBlackboardKeyType KeyType = EOliveIRBlackboardKeyType::Unknown;

	/** For Object/Class: the base class */
	UPROPERTY()
	FString BaseClass;

	/** For Enum: the enum type */
	UPROPERTY()
	FString EnumType;

	/** Whether this key is instance-synced */
	UPROPERTY()
	bool bInstanceSynced = false;

	/** Description/tooltip */
	UPROPERTY()
	FString Description;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRBlackboardKey FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Blackboard asset IR
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBlackboard
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Path;

	/** Parent blackboard (for inheritance) */
	UPROPERTY()
	FString ParentPath;

	/** Keys defined in this blackboard */
	UPROPERTY()
	TArray<FOliveIRBlackboardKey> Keys;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRBlackboard FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Behavior tree node type
 */
UENUM(BlueprintType)
enum class EOliveIRBTNodeType : uint8
{
	Root,
	Composite,  // Selector, Sequence, Simple Parallel
	Task,
	Decorator,
	Service,
	Unknown
};

/**
 * Composite type
 */
UENUM(BlueprintType)
enum class EOliveIRBTCompositeType : uint8
{
	Selector,
	Sequence,
	SimpleParallel,
	Unknown
};

/**
 * Behavior tree node
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBTNode
{
	GENERATED_BODY()

	/** Simple node ID */
	UPROPERTY()
	FString Id;

	/** Node type */
	UPROPERTY()
	EOliveIRBTNodeType NodeType = EOliveIRBTNodeType::Unknown;

	/** Node class name (e.g., BTTask_MoveTo) */
	UPROPERTY()
	FString NodeClass;

	/** Display name/title */
	UPROPERTY()
	FString Title;

	/** For composites: the composite type */
	UPROPERTY()
	EOliveIRBTCompositeType CompositeType = EOliveIRBTCompositeType::Unknown;

	/** Child nodes (for composites) - not reflected due to recursion */
	TArray<FOliveIRBTNode> Children;

	/** Decorators attached to this node - not reflected due to recursion */
	TArray<FOliveIRBTNode> Decorators;

	/** Services attached to this node - not reflected due to recursion */
	TArray<FOliveIRBTNode> Services;

	/** Node properties/settings */
	UPROPERTY()
	TMap<FString, FString> Properties;

	/** Blackboard keys this node references */
	UPROPERTY()
	TArray<FString> ReferencedBlackboardKeys;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRBTNode FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * Behavior tree asset IR
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRBehaviorTree
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Path;

	/** Associated blackboard */
	UPROPERTY()
	FString BlackboardPath;

	/** Root node */
	UPROPERTY()
	FOliveIRBTNode Root;

	/** All custom task classes used */
	UPROPERTY()
	TArray<FString> UsedTaskClasses;

	/** All custom decorator classes used */
	UPROPERTY()
	TArray<FString> UsedDecoratorClasses;

	/** All custom service classes used */
	UPROPERTY()
	TArray<FString> UsedServiceClasses;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRBehaviorTree FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};
