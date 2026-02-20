// Copyright Bode Software. All Rights Reserved.

#include "OliveBlackboardWriter.h"
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
#include "AssetToolsModule.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveBBWriter, Log, All);
DEFINE_LOG_CATEGORY(LogOliveBBWriter);

FOliveBlackboardWriter& FOliveBlackboardWriter::Get()
{
	static FOliveBlackboardWriter Instance;
	return Instance;
}

UBlackboardData* FOliveBlackboardWriter::LoadBlackboard(const FString& AssetPath) const
{
	return Cast<UBlackboardData>(
		StaticLoadObject(UBlackboardData::StaticClass(), nullptr, *AssetPath));
}

UBlackboardData* FOliveBlackboardWriter::CreateBlackboard(const FString& AssetPath, const FString& ParentPath)
{
	// Parse path into package path and asset name
	FString PackagePath;
	FString AssetName;

	int32 LastSlash;
	if (AssetPath.FindLastChar('/', LastSlash))
	{
		PackagePath = AssetPath.Left(LastSlash);
		AssetName = AssetPath.RightChop(LastSlash + 1);
	}
	else
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("CreateBlackboard: Invalid path '%s'"), *AssetPath);
		return nullptr;
	}

	// Remove asset name prefix if it matches (e.g., "/Game/AI/BB_Enemy.BB_Enemy")
	if (AssetName.Contains(TEXT(".")))
	{
		AssetName = FPaths::GetBaseFilename(AssetName);
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "CreateBlackboard", "Olive AI: Create Blackboard '{0}'"),
		FText::FromString(AssetName)));

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UBlackboardData::StaticClass(), nullptr);

	UBlackboardData* NewBB = Cast<UBlackboardData>(NewAsset);
	if (!NewBB)
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("CreateBlackboard: Failed to create asset at '%s'"), *AssetPath);
		return nullptr;
	}

	// Set parent if specified
	if (!ParentPath.IsEmpty())
	{
		UBlackboardData* ParentBB = LoadBlackboard(ParentPath);
		if (ParentBB)
		{
			NewBB->Parent = ParentBB;
		}
		else
		{
			UE_LOG(LogOliveBBWriter, Warning, TEXT("CreateBlackboard: Parent blackboard not found: %s"), *ParentPath);
		}
	}

	UE_LOG(LogOliveBBWriter, Log, TEXT("Created blackboard: %s"), *AssetPath);
	return NewBB;
}

bool FOliveBlackboardWriter::AddKey(UBlackboardData* BlackboardData, const FString& KeyName,
	EOliveIRBlackboardKeyType KeyType, const FString& BaseClass,
	const FString& EnumType, bool bInstanceSynced, const FString& Description)
{
	if (!BlackboardData)
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("AddKey: BlackboardData is null"));
		return false;
	}

	// Check for duplicate key name
	for (const FBlackboardEntry& Existing : BlackboardData->Keys)
	{
		if (Existing.EntryName == FName(*KeyName))
		{
			UE_LOG(LogOliveBBWriter, Error, TEXT("AddKey: Key '%s' already exists"), *KeyName);
			return false;
		}
	}

	// Also check parent chain
	if (BlackboardData->Parent)
	{
		TArray<FOliveIRBlackboardKey> InheritedKeys = FOliveBlackboardReader::Get().ReadAllKeys(BlackboardData->Parent, true);
		for (const FOliveIRBlackboardKey& InheritedKey : InheritedKeys)
		{
			if (InheritedKey.Name == KeyName)
			{
				UE_LOG(LogOliveBBWriter, Error, TEXT("AddKey: Key '%s' already exists in parent blackboard"), *KeyName);
				return false;
			}
		}
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "AddBBKey", "Olive AI: Add Blackboard Key '{0}'"),
		FText::FromString(KeyName)));

	BlackboardData->Modify();

	// Create the key type instance
	UBlackboardKeyType* KeyTypeInstance = CreateKeyTypeInstance(BlackboardData, KeyType, BaseClass, EnumType);
	if (!KeyTypeInstance)
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("AddKey: Failed to create key type for '%s'"), *KeyName);
		return false;
	}

	// Create and add the entry
	FBlackboardEntry NewEntry;
	NewEntry.EntryName = FName(*KeyName);
	NewEntry.KeyType = KeyTypeInstance;
	NewEntry.bInstanceSynced = bInstanceSynced;
	NewEntry.EntryDescription = Description;

	BlackboardData->Keys.Add(NewEntry);

	// Update key IDs
	BlackboardData->UpdateKeyIDs();
	BlackboardData->PostEditChange();

	UE_LOG(LogOliveBBWriter, Log, TEXT("Added key '%s' to blackboard '%s'"), *KeyName, *BlackboardData->GetName());
	return true;
}

bool FOliveBlackboardWriter::RemoveKey(UBlackboardData* BlackboardData, const FString& KeyName)
{
	if (!BlackboardData)
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("RemoveKey: BlackboardData is null"));
		return false;
	}

	int32 FoundIndex = INDEX_NONE;
	for (int32 i = 0; i < BlackboardData->Keys.Num(); ++i)
	{
		if (BlackboardData->Keys[i].EntryName == FName(*KeyName))
		{
			FoundIndex = i;
			break;
		}
	}

	if (FoundIndex == INDEX_NONE)
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("RemoveKey: Key '%s' not found"), *KeyName);
		return false;
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "RemoveBBKey", "Olive AI: Remove Blackboard Key '{0}'"),
		FText::FromString(KeyName)));

	BlackboardData->Modify();
	BlackboardData->Keys.RemoveAt(FoundIndex);
	BlackboardData->UpdateKeyIDs();
	BlackboardData->PostEditChange();

	UE_LOG(LogOliveBBWriter, Log, TEXT("Removed key '%s' from blackboard '%s'"), *KeyName, *BlackboardData->GetName());
	return true;
}

bool FOliveBlackboardWriter::ModifyKey(UBlackboardData* BlackboardData, const FString& KeyName,
	const FString& NewName, bool bSetInstanceSynced, bool bInstanceSynced,
	const FString& NewDescription)
{
	if (!BlackboardData)
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("ModifyKey: BlackboardData is null"));
		return false;
	}

	FBlackboardEntry* FoundEntry = nullptr;
	for (FBlackboardEntry& Entry : BlackboardData->Keys)
	{
		if (Entry.EntryName == FName(*KeyName))
		{
			FoundEntry = &Entry;
			break;
		}
	}

	if (!FoundEntry)
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("ModifyKey: Key '%s' not found"), *KeyName);
		return false;
	}

	// Check that new name doesn't conflict
	if (!NewName.IsEmpty() && NewName != KeyName)
	{
		for (const FBlackboardEntry& Entry : BlackboardData->Keys)
		{
			if (Entry.EntryName == FName(*NewName))
			{
				UE_LOG(LogOliveBBWriter, Error, TEXT("ModifyKey: Key name '%s' already exists"), *NewName);
				return false;
			}
		}
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "ModifyBBKey", "Olive AI: Modify Blackboard Key '{0}'"),
		FText::FromString(KeyName)));

	BlackboardData->Modify();

	if (!NewName.IsEmpty() && NewName != KeyName)
	{
		FoundEntry->EntryName = FName(*NewName);
	}

	if (bSetInstanceSynced)
	{
		FoundEntry->bInstanceSynced = bInstanceSynced;
	}

	if (!NewDescription.IsEmpty())
	{
		FoundEntry->EntryDescription = NewDescription;
	}

	BlackboardData->UpdateKeyIDs();
	BlackboardData->PostEditChange();

	UE_LOG(LogOliveBBWriter, Log, TEXT("Modified key '%s' in blackboard '%s'"), *KeyName, *BlackboardData->GetName());
	return true;
}

bool FOliveBlackboardWriter::SetParent(UBlackboardData* BlackboardData, const FString& ParentPath)
{
	if (!BlackboardData)
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("SetParent: BlackboardData is null"));
		return false;
	}

	UBlackboardData* ParentBB = LoadBlackboard(ParentPath);
	if (!ParentBB)
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("SetParent: Parent blackboard not found: %s"), *ParentPath);
		return false;
	}

	// Check for circular inheritance
	if (WouldCreateCircularInheritance(BlackboardData, ParentBB))
	{
		UE_LOG(LogOliveBBWriter, Error, TEXT("SetParent: Would create circular inheritance between '%s' and '%s'"),
			*BlackboardData->GetName(), *ParentBB->GetName());
		return false;
	}

	const FScopedTransaction Transaction(FText::Format(
		NSLOCTEXT("OliveAI", "SetBBParent", "Olive AI: Set Blackboard Parent to '{0}'"),
		FText::FromString(ParentBB->GetName())));

	BlackboardData->Modify();
	BlackboardData->Parent = ParentBB;
	BlackboardData->UpdateKeyIDs();
	BlackboardData->PostEditChange();

	UE_LOG(LogOliveBBWriter, Log, TEXT("Set parent of '%s' to '%s'"),
		*BlackboardData->GetName(), *ParentBB->GetName());
	return true;
}

UBlackboardKeyType* FOliveBlackboardWriter::CreateKeyTypeInstance(UObject* Outer,
	EOliveIRBlackboardKeyType KeyType, const FString& BaseClass, const FString& EnumType)
{
	UClass* KeyTypeClass = nullptr;

	switch (KeyType)
	{
		case EOliveIRBlackboardKeyType::Bool:    KeyTypeClass = UBlackboardKeyType_Bool::StaticClass(); break;
		case EOliveIRBlackboardKeyType::Int:     KeyTypeClass = UBlackboardKeyType_Int::StaticClass(); break;
		case EOliveIRBlackboardKeyType::Float:   KeyTypeClass = UBlackboardKeyType_Float::StaticClass(); break;
		case EOliveIRBlackboardKeyType::String:  KeyTypeClass = UBlackboardKeyType_String::StaticClass(); break;
		case EOliveIRBlackboardKeyType::Name:    KeyTypeClass = UBlackboardKeyType_Name::StaticClass(); break;
		case EOliveIRBlackboardKeyType::Vector:  KeyTypeClass = UBlackboardKeyType_Vector::StaticClass(); break;
		case EOliveIRBlackboardKeyType::Rotator: KeyTypeClass = UBlackboardKeyType_Rotator::StaticClass(); break;
		case EOliveIRBlackboardKeyType::Enum:    KeyTypeClass = UBlackboardKeyType_Enum::StaticClass(); break;
		case EOliveIRBlackboardKeyType::Object:  KeyTypeClass = UBlackboardKeyType_Object::StaticClass(); break;
		case EOliveIRBlackboardKeyType::Class:   KeyTypeClass = UBlackboardKeyType_Class::StaticClass(); break;
		default:
			UE_LOG(LogOliveBBWriter, Error, TEXT("CreateKeyTypeInstance: Unknown key type"));
			return nullptr;
	}

	UBlackboardKeyType* Instance = NewObject<UBlackboardKeyType>(Outer, KeyTypeClass);

	// Set base class for Object/Class types
	if (KeyType == EOliveIRBlackboardKeyType::Object && !BaseClass.IsEmpty())
	{
		UClass* FoundClass = FindObject<UClass>(ANY_PACKAGE, *BaseClass);
		if (!FoundClass)
		{
			FoundClass = FindObject<UClass>(ANY_PACKAGE, *(TEXT("U") + BaseClass));
		}
		if (!FoundClass)
		{
			FoundClass = FindObject<UClass>(ANY_PACKAGE, *(TEXT("A") + BaseClass));
		}
		if (FoundClass)
		{
			Cast<UBlackboardKeyType_Object>(Instance)->BaseClass = FoundClass;
		}
		else
		{
			UE_LOG(LogOliveBBWriter, Warning, TEXT("Could not find base class '%s' for Object key"), *BaseClass);
		}
	}
	else if (KeyType == EOliveIRBlackboardKeyType::Class && !BaseClass.IsEmpty())
	{
		UClass* FoundClass = FindObject<UClass>(ANY_PACKAGE, *BaseClass);
		if (!FoundClass)
		{
			FoundClass = FindObject<UClass>(ANY_PACKAGE, *(TEXT("U") + BaseClass));
		}
		if (!FoundClass)
		{
			FoundClass = FindObject<UClass>(ANY_PACKAGE, *(TEXT("A") + BaseClass));
		}
		if (FoundClass)
		{
			Cast<UBlackboardKeyType_Class>(Instance)->BaseClass = FoundClass;
		}
		else
		{
			UE_LOG(LogOliveBBWriter, Warning, TEXT("Could not find base class '%s' for Class key"), *BaseClass);
		}
	}
	else if (KeyType == EOliveIRBlackboardKeyType::Enum && !EnumType.IsEmpty())
	{
		UEnum* FoundEnum = FindObject<UEnum>(ANY_PACKAGE, *EnumType);
		if (FoundEnum)
		{
			Cast<UBlackboardKeyType_Enum>(Instance)->EnumType = FoundEnum;
		}
		else
		{
			UE_LOG(LogOliveBBWriter, Warning, TEXT("Could not find enum type '%s'"), *EnumType);
		}
	}

	return Instance;
}

bool FOliveBlackboardWriter::WouldCreateCircularInheritance(
	const UBlackboardData* BlackboardData, const UBlackboardData* ProposedParent) const
{
	if (!BlackboardData || !ProposedParent)
	{
		return false;
	}

	// Walk up the proposed parent's chain looking for BlackboardData
	const UBlackboardData* Current = ProposedParent;
	TSet<const UBlackboardData*> Visited;

	while (Current)
	{
		if (Current == BlackboardData)
		{
			return true;
		}

		if (Visited.Contains(Current))
		{
			// Already circular in existing chain
			return true;
		}

		Visited.Add(Current);
		Current = Current->Parent;
	}

	return false;
}
