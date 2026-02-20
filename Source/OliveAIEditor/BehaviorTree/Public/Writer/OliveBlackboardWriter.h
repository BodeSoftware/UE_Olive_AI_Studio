// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BehaviorTreeIR.h"

class UBlackboardData;
class UBlackboardKeyType;

/**
 * FOliveBlackboardWriter
 *
 * Creates and modifies UBlackboardData assets.
 * All write operations use FScopedTransaction for undo support.
 *
 * Usage:
 *   FOliveBlackboardWriter& Writer = FOliveBlackboardWriter::Get();
 *   UBlackboardData* BB = Writer.CreateBlackboard("/Game/AI/BB_Enemy");
 *   Writer.AddKey(BB, "TargetActor", EOliveIRBlackboardKeyType::Object, "Actor");
 */
class OLIVEAIEDITOR_API FOliveBlackboardWriter
{
public:
	/** Get the singleton instance */
	static FOliveBlackboardWriter& Get();

	/**
	 * Create a new Blackboard asset
	 * @param AssetPath Content path (e.g., "/Game/AI/BB_Enemy")
	 * @param ParentPath Optional parent blackboard path for inheritance
	 * @return Created blackboard, or nullptr on failure
	 */
	UBlackboardData* CreateBlackboard(const FString& AssetPath, const FString& ParentPath = TEXT(""));

	/**
	 * Add a key to a blackboard
	 * @param BlackboardData Target blackboard
	 * @param KeyName Name for the new key
	 * @param KeyType Type of the key
	 * @param BaseClass For Object/Class types, the base class name
	 * @param EnumType For Enum type, the enum name
	 * @param bInstanceSynced Whether this key is instance-synced
	 * @param Description Optional description
	 * @return True if key was added successfully
	 */
	bool AddKey(UBlackboardData* BlackboardData, const FString& KeyName,
		EOliveIRBlackboardKeyType KeyType, const FString& BaseClass = TEXT(""),
		const FString& EnumType = TEXT(""), bool bInstanceSynced = false,
		const FString& Description = TEXT(""));

	/**
	 * Remove a key from a blackboard
	 * @param BlackboardData Target blackboard
	 * @param KeyName Name of the key to remove
	 * @return True if key was found and removed
	 */
	bool RemoveKey(UBlackboardData* BlackboardData, const FString& KeyName);

	/**
	 * Modify key properties
	 * @param BlackboardData Target blackboard
	 * @param KeyName Current name of the key
	 * @param NewName New name (empty = no rename)
	 * @param bSetInstanceSynced Whether to set instance synced flag
	 * @param bInstanceSynced New instance synced value
	 * @param NewDescription New description (empty = no change)
	 * @return True if key was found and modified
	 */
	bool ModifyKey(UBlackboardData* BlackboardData, const FString& KeyName,
		const FString& NewName = TEXT(""), bool bSetInstanceSynced = false,
		bool bInstanceSynced = false, const FString& NewDescription = TEXT(""));

	/**
	 * Set parent blackboard (with circular inheritance check)
	 * @param BlackboardData Target blackboard
	 * @param ParentPath Path to parent blackboard
	 * @return True if parent was set successfully
	 */
	bool SetParent(UBlackboardData* BlackboardData, const FString& ParentPath);

private:
	FOliveBlackboardWriter() = default;
	~FOliveBlackboardWriter() = default;

	FOliveBlackboardWriter(const FOliveBlackboardWriter&) = delete;
	FOliveBlackboardWriter& operator=(const FOliveBlackboardWriter&) = delete;

	/**
	 * Create a UBlackboardKeyType instance for the given IR key type
	 * @param Outer The outer object for the key type
	 * @param KeyType IR key type enum
	 * @param BaseClass For Object/Class types
	 * @param EnumType For Enum type
	 * @return Created key type instance, or nullptr if unknown type
	 */
	UBlackboardKeyType* CreateKeyTypeInstance(UObject* Outer, EOliveIRBlackboardKeyType KeyType,
		const FString& BaseClass = TEXT(""), const FString& EnumType = TEXT(""));

	/**
	 * Check if setting a parent would create circular inheritance
	 * @param BlackboardData The blackboard that would get the new parent
	 * @param ProposedParent The proposed parent
	 * @return True if circular inheritance would result
	 */
	bool WouldCreateCircularInheritance(const UBlackboardData* BlackboardData, const UBlackboardData* ProposedParent) const;

	/** Load a blackboard by path */
	UBlackboardData* LoadBlackboard(const FString& AssetPath) const;
};
