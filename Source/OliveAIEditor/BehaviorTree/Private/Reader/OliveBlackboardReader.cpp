// Copyright Bode Software. All Rights Reserved.

#include "OliveBlackboardReader.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"

DEFINE_LOG_CATEGORY(LogOliveBTReader);

FOliveBlackboardReader& FOliveBlackboardReader::Get()
{
	static FOliveBlackboardReader Instance;
	return Instance;
}

UBlackboardData* FOliveBlackboardReader::LoadBlackboard(const FString& AssetPath) const
{
	UBlackboardData* BB = Cast<UBlackboardData>(
		StaticLoadObject(UBlackboardData::StaticClass(), nullptr, *AssetPath));

	if (!BB)
	{
		UE_LOG(LogOliveBTReader, Warning, TEXT("Failed to load blackboard: %s"), *AssetPath);
	}

	return BB;
}

TOptional<FOliveIRBlackboard> FOliveBlackboardReader::ReadBlackboard(const FString& AssetPath)
{
	UBlackboardData* BB = LoadBlackboard(AssetPath);
	if (!BB)
	{
		return {};
	}

	return ReadBlackboard(BB);
}

TOptional<FOliveIRBlackboard> FOliveBlackboardReader::ReadBlackboard(const UBlackboardData* BlackboardData)
{
	if (!BlackboardData)
	{
		return {};
	}

	FOliveIRBlackboard IR;
	IR.Name = BlackboardData->GetName();
	IR.Path = BlackboardData->GetPathName();

	if (BlackboardData->Parent)
	{
		IR.ParentPath = BlackboardData->Parent->GetPathName();
	}

	IR.Keys = ReadAllKeys(BlackboardData, false);

	UE_LOG(LogOliveBTReader, Verbose, TEXT("Read blackboard '%s' with %d keys"), *IR.Name, IR.Keys.Num());

	return IR;
}

TArray<FOliveIRBlackboardKey> FOliveBlackboardReader::ReadAllKeys(
	const UBlackboardData* BlackboardData,
	bool bIncludeInherited)
{
	TArray<FOliveIRBlackboardKey> AllKeys;

	if (!BlackboardData)
	{
		return AllKeys;
	}

	// If including inherited, walk parent chain first (so parent keys come first)
	if (bIncludeInherited && BlackboardData->Parent)
	{
		AllKeys = ReadAllKeys(BlackboardData->Parent, true);
	}

	// Add this blackboard's keys
	for (const FBlackboardEntry& Entry : BlackboardData->Keys)
	{
		AllKeys.Add(ConvertEntryToIR(Entry));
	}

	return AllKeys;
}

FOliveIRBlackboardKey FOliveBlackboardReader::ConvertEntryToIR(const FBlackboardEntry& Entry)
{
	FOliveIRBlackboardKey Key;
	Key.Name = Entry.EntryName.ToString();
	Key.bInstanceSynced = Entry.bInstanceSynced;
	Key.Description = Entry.EntryDescription;

	if (Entry.KeyType)
	{
		Key.KeyType = MapKeyType(Entry.KeyType);

		// Extract base class for Object/Class key types
		if (const UBlackboardKeyType_Object* ObjKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
		{
			if (ObjKey->BaseClass)
			{
				Key.BaseClass = ObjKey->BaseClass->GetName();
			}
		}
		else if (const UBlackboardKeyType_Class* ClassKey = Cast<UBlackboardKeyType_Class>(Entry.KeyType))
		{
			if (ClassKey->BaseClass)
			{
				Key.BaseClass = ClassKey->BaseClass->GetName();
			}
		}
		else if (const UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(Entry.KeyType))
		{
			if (EnumKey->EnumType)
			{
				Key.EnumType = EnumKey->EnumType->GetName();
			}
		}
	}
	else
	{
		Key.KeyType = EOliveIRBlackboardKeyType::Unknown;
	}

	return Key;
}

EOliveIRBlackboardKeyType FOliveBlackboardReader::MapKeyType(const UBlackboardKeyType* KeyType)
{
	if (!KeyType)
	{
		return EOliveIRBlackboardKeyType::Unknown;
	}

	if (KeyType->IsA<UBlackboardKeyType_Bool>()) return EOliveIRBlackboardKeyType::Bool;
	if (KeyType->IsA<UBlackboardKeyType_Int>()) return EOliveIRBlackboardKeyType::Int;
	if (KeyType->IsA<UBlackboardKeyType_Float>()) return EOliveIRBlackboardKeyType::Float;
	if (KeyType->IsA<UBlackboardKeyType_String>()) return EOliveIRBlackboardKeyType::String;
	if (KeyType->IsA<UBlackboardKeyType_Name>()) return EOliveIRBlackboardKeyType::Name;
	if (KeyType->IsA<UBlackboardKeyType_Vector>()) return EOliveIRBlackboardKeyType::Vector;
	if (KeyType->IsA<UBlackboardKeyType_Rotator>()) return EOliveIRBlackboardKeyType::Rotator;
	if (KeyType->IsA<UBlackboardKeyType_Enum>()) return EOliveIRBlackboardKeyType::Enum;
	if (KeyType->IsA<UBlackboardKeyType_Object>()) return EOliveIRBlackboardKeyType::Object;
	if (KeyType->IsA<UBlackboardKeyType_Class>()) return EOliveIRBlackboardKeyType::Class;

	UE_LOG(LogOliveBTReader, Warning, TEXT("Unknown blackboard key type: %s"), *KeyType->GetClass()->GetName());
	return EOliveIRBlackboardKeyType::Unknown;
}
