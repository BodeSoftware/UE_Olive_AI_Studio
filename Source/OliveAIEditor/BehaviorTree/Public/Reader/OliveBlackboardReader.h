// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BehaviorTreeIR.h"

class UBlackboardData;
class UBlackboardKeyType;
struct FBlackboardEntry;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveBTReader, Log, All);

/**
 * FOliveBlackboardReader
 *
 * Reads UBlackboardData assets into FOliveIRBlackboard IR format.
 * Handles parent chain traversal for inherited keys.
 *
 * Usage:
 *   FOliveBlackboardReader& Reader = FOliveBlackboardReader::Get();
 *   TOptional<FOliveIRBlackboard> BB = Reader.ReadBlackboard("/Game/AI/BB_Enemy");
 */
class OLIVEAIEDITOR_API FOliveBlackboardReader
{
public:
	/** Get the singleton instance */
	static FOliveBlackboardReader& Get();

	/**
	 * Read a blackboard by asset path
	 * @param AssetPath Content path (e.g., "/Game/AI/BB_Enemy")
	 * @return The blackboard IR, or empty if load failed
	 */
	TOptional<FOliveIRBlackboard> ReadBlackboard(const FString& AssetPath);

	/**
	 * Read a blackboard from an already-loaded asset
	 * @param BlackboardData The blackboard asset
	 * @return The blackboard IR, or empty if null
	 */
	TOptional<FOliveIRBlackboard> ReadBlackboard(const UBlackboardData* BlackboardData);

	/**
	 * Read all keys, optionally including inherited keys from parent chain
	 * @param BlackboardData The blackboard asset
	 * @param bIncludeInherited Whether to include keys from parent blackboards
	 * @return Array of blackboard keys in IR format
	 */
	TArray<FOliveIRBlackboardKey> ReadAllKeys(const UBlackboardData* BlackboardData, bool bIncludeInherited = true);

private:
	FOliveBlackboardReader() = default;
	~FOliveBlackboardReader() = default;

	FOliveBlackboardReader(const FOliveBlackboardReader&) = delete;
	FOliveBlackboardReader& operator=(const FOliveBlackboardReader&) = delete;

	/** Convert a single FBlackboardEntry to IR key */
	FOliveIRBlackboardKey ConvertEntryToIR(const FBlackboardEntry& Entry);

	/** Map UBlackboardKeyType subclass to IR key type enum */
	EOliveIRBlackboardKeyType MapKeyType(const UBlackboardKeyType* KeyType);

	/** Load a UBlackboardData by asset path */
	UBlackboardData* LoadBlackboard(const FString& AssetPath) const;
};
