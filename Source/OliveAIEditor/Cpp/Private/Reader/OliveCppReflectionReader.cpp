// Copyright Bode Software. All Rights Reserved.

#include "Reader/OliveCppReflectionReader.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "UObject/EnumProperty.h"
#include "SourceCodeNavigation.h"

DEFINE_LOG_CATEGORY(LogOliveCppReader);

// ---------------------------------------------------------------------------
// FindClassByName
// ---------------------------------------------------------------------------

UClass* FOliveCppReflectionReader::FindClassByName(const FString& ClassName)
{
	if (ClassName.IsEmpty())
	{
		return nullptr;
	}

	// Try /Script/ path format first (e.g., "/Script/Engine.Actor")
	if (ClassName.Contains(TEXT(".")))
	{
		UClass* Found = FindObject<UClass>(nullptr, *ClassName);
		if (Found)
		{
			return Found;
		}
	}

	// Try exact match via StaticFindFirstObject
	UClass* Found = Cast<UClass>(StaticFindFirstObject(UClass::StaticClass(), *ClassName, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging, TEXT("OliveCppReflectionReader")));
	if (Found)
	{
		return Found;
	}

	// Try with A prefix (Actor-derived)
	Found = Cast<UClass>(StaticFindFirstObject(UClass::StaticClass(), *(TEXT("A") + ClassName), EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging, TEXT("OliveCppReflectionReader")));
	if (Found)
	{
		return Found;
	}

	// Try with U prefix (UObject-derived)
	Found = Cast<UClass>(StaticFindFirstObject(UClass::StaticClass(), *(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging, TEXT("OliveCppReflectionReader")));
	if (Found)
	{
		return Found;
	}

	// Brute force search through all loaded classes
	for (TObjectIterator<UClass> It; It; ++It)
	{
		const FString Name = It->GetName();
		if (Name == ClassName || Name == (TEXT("A") + ClassName) || Name == (TEXT("U") + ClassName))
		{
			return *It;
		}
	}

	UE_LOG(LogOliveCppReader, Warning, TEXT("FindClassByName: Could not find class '%s'"), *ClassName);
	return nullptr;
}

// ---------------------------------------------------------------------------
// FindEnumByName
// ---------------------------------------------------------------------------

UEnum* FOliveCppReflectionReader::FindEnumByName(const FString& EnumName)
{
	if (EnumName.IsEmpty())
	{
		return nullptr;
	}

	UEnum* Found = Cast<UEnum>(StaticFindFirstObject(UEnum::StaticClass(), *EnumName, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging, TEXT("OliveCppReflectionReader")));
	if (Found)
	{
		return Found;
	}

	// Try with E prefix
	Found = Cast<UEnum>(StaticFindFirstObject(UEnum::StaticClass(), *(TEXT("E") + EnumName), EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging, TEXT("OliveCppReflectionReader")));
	if (Found)
	{
		return Found;
	}

	// Brute force
	for (TObjectIterator<UEnum> It; It; ++It)
	{
		const FString Name = It->GetName();
		if (Name == EnumName || Name == (TEXT("E") + EnumName))
		{
			return *It;
		}
	}

	return nullptr;
}

// ---------------------------------------------------------------------------
// FindStructByName
// ---------------------------------------------------------------------------

UScriptStruct* FOliveCppReflectionReader::FindStructByName(const FString& StructName)
{
	if (StructName.IsEmpty())
	{
		return nullptr;
	}

	UScriptStruct* Found = Cast<UScriptStruct>(StaticFindFirstObject(UScriptStruct::StaticClass(), *StructName, EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging, TEXT("OliveCppReflectionReader")));
	if (Found)
	{
		return Found;
	}

	// Try with F prefix
	Found = Cast<UScriptStruct>(StaticFindFirstObject(UScriptStruct::StaticClass(), *(TEXT("F") + StructName), EFindFirstObjectOptions::NativeFirst, ELogVerbosity::NoLogging, TEXT("OliveCppReflectionReader")));
	if (Found)
	{
		return Found;
	}

	// Brute force
	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		const FString Name = It->GetName();
		if (Name == StructName || Name == (TEXT("F") + StructName))
		{
			return *It;
		}
	}

	return nullptr;
}

// ---------------------------------------------------------------------------
// GetPropertyTypeName
// ---------------------------------------------------------------------------

FString FOliveCppReflectionReader::GetPropertyTypeName(const FProperty* Property)
{
	if (!Property)
	{
		return TEXT("void");
	}

	if (CastField<FBoolProperty>(Property))
	{
		return TEXT("bool");
	}
	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
	{
		if (ByteProp->Enum)
		{
			return FString::Printf(TEXT("TEnumAsByte<%s>"), *ByteProp->Enum->GetName());
		}
		return TEXT("uint8");
	}
	if (CastField<FIntProperty>(Property))
	{
		return TEXT("int32");
	}
	if (CastField<FInt64Property>(Property))
	{
		return TEXT("int64");
	}
	if (CastField<FFloatProperty>(Property))
	{
		return TEXT("float");
	}
	if (CastField<FDoubleProperty>(Property))
	{
		return TEXT("double");
	}
	if (CastField<FStrProperty>(Property))
	{
		return TEXT("FString");
	}
	if (CastField<FNameProperty>(Property))
	{
		return TEXT("FName");
	}
	if (CastField<FTextProperty>(Property))
	{
		return TEXT("FText");
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		if (UEnum* Enum = EnumProp->GetEnum())
		{
			return Enum->GetName();
		}
		return TEXT("uint8");
	}
	if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		if (ObjProp->PropertyClass)
		{
			return FString::Printf(TEXT("%s*"), *ObjProp->PropertyClass->GetName());
		}
		return TEXT("UObject*");
	}
	if (const FClassProperty* ClassProp = CastField<FClassProperty>(Property))
	{
		if (ClassProp->MetaClass)
		{
			return FString::Printf(TEXT("TSubclassOf<%s>"), *ClassProp->MetaClass->GetName());
		}
		return TEXT("TSubclassOf<UObject>");
	}
	if (const FWeakObjectProperty* WeakProp = CastField<FWeakObjectProperty>(Property))
	{
		if (WeakProp->PropertyClass)
		{
			return FString::Printf(TEXT("TWeakObjectPtr<%s>"), *WeakProp->PropertyClass->GetName());
		}
		return TEXT("TWeakObjectPtr<UObject>");
	}
	if (const FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Property))
	{
		if (SoftObjProp->PropertyClass)
		{
			return FString::Printf(TEXT("TSoftObjectPtr<%s>"), *SoftObjProp->PropertyClass->GetName());
		}
		return TEXT("TSoftObjectPtr<UObject>");
	}
	if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		if (StructProp->Struct)
		{
			return StructProp->Struct->GetName();
		}
		return TEXT("FStruct");
	}
	if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		return FString::Printf(TEXT("TArray<%s>"), *GetPropertyTypeName(ArrayProp->Inner));
	}
	if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
	{
		return FString::Printf(TEXT("TSet<%s>"), *GetPropertyTypeName(SetProp->ElementProp));
	}
	if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
	{
		return FString::Printf(TEXT("TMap<%s, %s>"), *GetPropertyTypeName(MapProp->KeyProp), *GetPropertyTypeName(MapProp->ValueProp));
	}
	if (const FInterfaceProperty* IntProp = CastField<FInterfaceProperty>(Property))
	{
		if (IntProp->InterfaceClass)
		{
			return FString::Printf(TEXT("TScriptInterface<%s>"), *IntProp->InterfaceClass->GetName());
		}
		return TEXT("TScriptInterface<IInterface>");
	}
	if (CastField<FDelegateProperty>(Property))
	{
		return TEXT("FScriptDelegate");
	}
	if (CastField<FMulticastDelegateProperty>(Property))
	{
		return TEXT("FMulticastScriptDelegate");
	}

	// Fallback to engine's built-in type string
	return Property->GetCPPType();
}

// ---------------------------------------------------------------------------
// ExtractPropertyFlags
// ---------------------------------------------------------------------------

FOliveIRCppPropertyFlags FOliveCppReflectionReader::ExtractPropertyFlags(const FProperty* Property)
{
	FOliveIRCppPropertyFlags Flags;
	if (!Property)
	{
		return Flags;
	}

	Flags.bBlueprintReadWrite = Property->HasAllPropertyFlags(CPF_BlueprintVisible) && !Property->HasAllPropertyFlags(CPF_BlueprintReadOnly);
	Flags.bBlueprintReadOnly = Property->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly);
	Flags.bEditAnywhere = Property->HasAllPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance | CPF_DisableEditOnTemplate);
	Flags.bEditDefaultsOnly = Property->HasAllPropertyFlags(CPF_Edit | CPF_DisableEditOnInstance);
	Flags.bEditInstanceOnly = Property->HasAllPropertyFlags(CPF_Edit | CPF_DisableEditOnTemplate);
	Flags.bVisibleAnywhere = Property->HasAllPropertyFlags(CPF_Edit | CPF_EditConst) && !Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance | CPF_DisableEditOnTemplate);
	Flags.bConfig = Property->HasAllPropertyFlags(CPF_Config);
	Flags.bTransient = Property->HasAllPropertyFlags(CPF_Transient);
	Flags.bReplicated = Property->HasAllPropertyFlags(CPF_Net);
	Flags.bExposeOnSpawn = Property->HasAllPropertyFlags(CPF_ExposeOnSpawn);
	Flags.bSaveGame = Property->HasAllPropertyFlags(CPF_SaveGame);

	return Flags;
}

// ---------------------------------------------------------------------------
// ExtractFunctionFlags
// ---------------------------------------------------------------------------

FOliveIRCppFunctionFlags FOliveCppReflectionReader::ExtractFunctionFlags(const UFunction* Function)
{
	FOliveIRCppFunctionFlags Flags;
	if (!Function)
	{
		return Flags;
	}

	Flags.bBlueprintCallable = Function->HasAnyFunctionFlags(FUNC_BlueprintCallable);
	Flags.bBlueprintPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
	Flags.bBlueprintImplementableEvent = Function->HasAnyFunctionFlags(FUNC_BlueprintEvent) && !Function->HasAnyFunctionFlags(FUNC_Native);
	Flags.bBlueprintNativeEvent = Function->HasAnyFunctionFlags(FUNC_BlueprintEvent) && Function->HasAnyFunctionFlags(FUNC_Native);
	Flags.bCallInEditor = Function->GetBoolMetaData(TEXT("CallInEditor"));
	Flags.bServer = Function->HasAnyFunctionFlags(FUNC_NetServer);
	Flags.bClient = Function->HasAnyFunctionFlags(FUNC_NetClient);
	Flags.bNetMulticast = Function->HasAnyFunctionFlags(FUNC_NetMulticast);
	Flags.bReliable = Function->HasAnyFunctionFlags(FUNC_NetReliable);
	Flags.bExec = Function->HasAnyFunctionFlags(FUNC_Exec);
	Flags.bConst = Function->HasAnyFunctionFlags(FUNC_Const);
	Flags.bStatic = Function->HasAnyFunctionFlags(FUNC_Static);
	// Virtual cannot be reliably determined from reflection alone
	Flags.bVirtual = false;

	return Flags;
}

// ---------------------------------------------------------------------------
// ExtractMetadata (UField - for UClass, UFunction, UEnum, UScriptStruct)
// ---------------------------------------------------------------------------

TMap<FString, FString> FOliveCppReflectionReader::ExtractMetadata(const UField* Field)
{
	TMap<FString, FString> Result;
	if (!Field)
	{
		return Result;
	}

#if WITH_METADATA
	static const TArray<FString> UsefulKeys = {
		TEXT("Category"),
		TEXT("ToolTip"),
		TEXT("ShortToolTip"),
		TEXT("DisplayName"),
		TEXT("BlueprintType"),
		TEXT("Blueprintable"),
		TEXT("NotBlueprintable"),
		TEXT("IsBlueprintBase"),
		TEXT("DocumentationPolicy"),
		TEXT("DeprecatedFunction"),
		TEXT("DeprecationMessage"),
		TEXT("ScriptName"),
		TEXT("ModuleRelativePath"),
	};

	for (const FString& Key : UsefulKeys)
	{
		if (Field->HasMetaData(*Key))
		{
			Result.Add(Key, Field->GetMetaData(*Key));
		}
	}
#endif

	return Result;
}

// ---------------------------------------------------------------------------
// ExtractMetadata (FField - for FProperty)
// ---------------------------------------------------------------------------

TMap<FString, FString> FOliveCppReflectionReader::ExtractMetadata(const FField* Field)
{
	TMap<FString, FString> Result;
	if (!Field)
	{
		return Result;
	}

#if WITH_METADATA
	const TMap<FName, FString>* MetaDataMap = Field->GetMetaDataMap();
	if (MetaDataMap)
	{
		for (const auto& Pair : *MetaDataMap)
		{
			Result.Add(Pair.Key.ToString(), Pair.Value);
		}
	}
#endif

	return Result;
}

// ---------------------------------------------------------------------------
// ConvertProperty
// ---------------------------------------------------------------------------

FOliveIRCppProperty FOliveCppReflectionReader::ConvertProperty(const FProperty* Property)
{
	FOliveIRCppProperty IR;
	if (!Property)
	{
		return IR;
	}

	IR.Name = Property->GetName();
	IR.TypeName = GetPropertyTypeName(Property);

#if WITH_METADATA
	if (Property->HasMetaData(TEXT("Category")))
	{
		IR.Category = Property->GetMetaData(TEXT("Category"));
	}
	if (Property->HasMetaData(TEXT("ToolTip")))
	{
		IR.Description = Property->GetMetaData(TEXT("ToolTip"));
	}
#endif

	// Attempt to get the default value as string via CPPTypeForwardDeclaration or metadata
	// Default values are stored on the CDO, not on the property itself, so we leave DefaultValue empty here.
	// The caller can populate it from the CDO if needed.

	IR.Flags = ExtractPropertyFlags(Property);
	IR.Metadata = ExtractMetadata(Property);

	return IR;
}

// ---------------------------------------------------------------------------
// ConvertFunction
// ---------------------------------------------------------------------------

FOliveIRCppFunction FOliveCppReflectionReader::ConvertFunction(const UFunction* Function)
{
	FOliveIRCppFunction IR;
	if (!Function)
	{
		return IR;
	}

	IR.Name = Function->GetName();
	IR.Flags = ExtractFunctionFlags(Function);

#if WITH_METADATA
	if (Function->HasMetaData(TEXT("Category")))
	{
		IR.Category = Function->GetMetaData(TEXT("Category"));
	}
	if (Function->HasMetaData(TEXT("ToolTip")))
	{
		IR.Description = Function->GetMetaData(TEXT("ToolTip"));
	}
#endif

	IR.Metadata = ExtractMetadata(Function);

	// Process parameters and return value
	for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
	{
		FProperty* Param = *ParamIt;
		if (!Param->HasAnyPropertyFlags(CPF_Parm))
		{
			continue;
		}

		if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			IR.ReturnType = GetPropertyTypeName(Param);
		}
		else
		{
			FOliveIRCppProperty ParamIR = ConvertProperty(Param);
			IR.Parameters.Add(MoveTemp(ParamIR));
		}
	}

	return IR;
}

// ---------------------------------------------------------------------------
// ReadClass
// ---------------------------------------------------------------------------

TOptional<FOliveIRCppClass> FOliveCppReflectionReader::ReadClass(
	const FString& ClassName,
	bool bIncludeInherited,
	bool bIncludeFunctions,
	bool bIncludeProperties)
{
	UClass* Class = FindClassByName(ClassName);
	if (!Class)
	{
		UE_LOG(LogOliveCppReader, Warning, TEXT("ReadClass: Class '%s' not found"), *ClassName);
		return {};
	}

	FOliveIRCppClass IR;
	IR.ClassName = Class->GetName();

	// Parent class
	if (UClass* SuperClass = Class->GetSuperClass())
	{
		IR.ParentClassName = SuperClass->GetName();
	}

	// Module name from package path (strip "/Script/" prefix)
	UPackage* Package = Class->GetOutermost();
	if (Package)
	{
		FString PackagePath = Package->GetName();
		// Package names look like "/Script/ModuleName"
		if (PackagePath.StartsWith(TEXT("/Script/")))
		{
			IR.ModuleName = PackagePath.RightChop(8); // Remove "/Script/"
		}
		else
		{
			IR.ModuleName = PackagePath;
		}
	}

	// Header path via FSourceCodeNavigation
	FString HeaderPath;
	if (FSourceCodeNavigation::FindClassHeaderPath(Class, HeaderPath))
	{
		IR.HeaderPath = HeaderPath;
	}

	// Implemented interfaces
	for (const FImplementedInterface& Iface : Class->Interfaces)
	{
		if (Iface.Class)
		{
			IR.Interfaces.Add(Iface.Class->GetName());
		}
	}

	// Class flags
	IR.bIsAbstract = Class->HasAnyClassFlags(CLASS_Abstract);
	IR.bIsDeprecated = Class->HasAnyClassFlags(CLASS_Deprecated);

#if WITH_METADATA
	IR.bIsBlueprintable = Class->GetBoolMetaData(TEXT("Blueprintable"));
	IR.bIsBlueprintType = Class->GetBoolMetaData(TEXT("BlueprintType"));
#endif

	// Class-level metadata
	IR.ClassMetadata = ExtractMetadata(Class);

	// Properties
	if (bIncludeProperties)
	{
		EFieldIteratorFlags::SuperClassFlags SuperFlags = bIncludeInherited
			? EFieldIteratorFlags::IncludeSuper
			: EFieldIteratorFlags::ExcludeSuper;

		for (TFieldIterator<FProperty> PropIt(Class, SuperFlags); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			IR.Properties.Add(ConvertProperty(Prop));
		}
	}

	// Functions
	if (bIncludeFunctions)
	{
		EFieldIteratorFlags::SuperClassFlags SuperFlags = bIncludeInherited
			? EFieldIteratorFlags::IncludeSuper
			: EFieldIteratorFlags::ExcludeSuper;

		for (TFieldIterator<UFunction> FuncIt(Class, SuperFlags); FuncIt; ++FuncIt)
		{
			UFunction* Func = *FuncIt;

			// Skip delegate signature functions
			if (Func->HasAnyFunctionFlags(FUNC_Delegate))
			{
				continue;
			}

			IR.Functions.Add(ConvertFunction(Func));
		}
	}

	UE_LOG(LogOliveCppReader, Log, TEXT("ReadClass: Read '%s' with %d properties and %d functions"),
		*IR.ClassName, IR.Properties.Num(), IR.Functions.Num());

	return IR;
}

// ---------------------------------------------------------------------------
// ListBlueprintCallable
// ---------------------------------------------------------------------------

TArray<FOliveIRCppFunction> FOliveCppReflectionReader::ListBlueprintCallable(
	const FString& ClassName,
	bool bIncludeInherited)
{
	TArray<FOliveIRCppFunction> Result;

	UClass* Class = FindClassByName(ClassName);
	if (!Class)
	{
		UE_LOG(LogOliveCppReader, Warning, TEXT("ListBlueprintCallable: Class '%s' not found"), *ClassName);
		return Result;
	}

	EFieldIteratorFlags::SuperClassFlags SuperFlags = bIncludeInherited
		? EFieldIteratorFlags::IncludeSuper
		: EFieldIteratorFlags::ExcludeSuper;

	for (TFieldIterator<UFunction> FuncIt(Class, SuperFlags); FuncIt; ++FuncIt)
	{
		UFunction* Func = *FuncIt;

		if (Func->HasAnyFunctionFlags(FUNC_Delegate))
		{
			continue;
		}

		if (Func->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Func->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			Result.Add(ConvertFunction(Func));
		}
	}

	return Result;
}

// ---------------------------------------------------------------------------
// ListOverridable
// ---------------------------------------------------------------------------

TArray<FOliveIRCppFunction> FOliveCppReflectionReader::ListOverridable(const FString& ClassName)
{
	TArray<FOliveIRCppFunction> Result;

	UClass* Class = FindClassByName(ClassName);
	if (!Class)
	{
		UE_LOG(LogOliveCppReader, Warning, TEXT("ListOverridable: Class '%s' not found"), *ClassName);
		return Result;
	}

	// Include inherited to find all overridable functions in the hierarchy
	for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
	{
		UFunction* Func = *FuncIt;

		if (Func->HasAnyFunctionFlags(FUNC_Delegate))
		{
			continue;
		}

		// BlueprintImplementableEvent or BlueprintNativeEvent
		if (Func->HasAnyFunctionFlags(FUNC_BlueprintEvent))
		{
			Result.Add(ConvertFunction(Func));
		}
	}

	return Result;
}

// ---------------------------------------------------------------------------
// ReadEnum
// ---------------------------------------------------------------------------

TOptional<FOliveIRCppEnum> FOliveCppReflectionReader::ReadEnum(const FString& EnumName)
{
	UEnum* Enum = FindEnumByName(EnumName);
	if (!Enum)
	{
		UE_LOG(LogOliveCppReader, Warning, TEXT("ReadEnum: Enum '%s' not found"), *EnumName);
		return {};
	}

	FOliveIRCppEnum IR;
	IR.EnumName = Enum->GetName();

	// Scoped enum detection
	IR.bIsScoped = (Enum->GetCppForm() == UEnum::ECppForm::EnumClass);

	// BlueprintType is metadata
#if WITH_METADATA
	IR.bIsBlueprintType = Enum->GetBoolMetaData(TEXT("BlueprintType"));
#endif

	// Underlying type - try to determine from the enum's underlying type property
	// UEnum doesn't directly expose the underlying C++ type via reflection easily.
	// Most UENUM() use uint8 by default. We check the max value to infer.
	IR.UnderlyingType = TEXT("uint8");

	// Iterate enum values (NumEnums() includes the _MAX sentinel, so stop 1 before)
	const int32 Count = Enum->NumEnums();
	for (int32 i = 0; i < Count; ++i)
	{
		FString ValueName = Enum->GetNameStringByIndex(i);

		// Skip the auto-generated _MAX entry
		if (ValueName.EndsWith(TEXT("_MAX")) || ValueName.Equals(TEXT("MAX")))
		{
#if WITH_METADATA
			if (Enum->HasMetaData(TEXT("Hidden"), i))
			{
				continue;
			}
#endif
			// Even without metadata, skip typical _MAX sentinels
			continue;
		}

		IR.Values.Add(ValueName);

		FText DisplayName = Enum->GetDisplayNameTextByIndex(i);
		if (!DisplayName.IsEmpty())
		{
			IR.ValueDisplayNames.Add(ValueName, DisplayName.ToString());
		}
	}

	// Enum-level metadata
	IR.Metadata = ExtractMetadata(Enum);

	UE_LOG(LogOliveCppReader, Log, TEXT("ReadEnum: Read '%s' with %d values"), *IR.EnumName, IR.Values.Num());

	return IR;
}

// ---------------------------------------------------------------------------
// ReadStruct
// ---------------------------------------------------------------------------

TOptional<FOliveIRCppStruct> FOliveCppReflectionReader::ReadStruct(
	const FString& StructName,
	bool bIncludeInherited)
{
	UScriptStruct* Struct = FindStructByName(StructName);
	if (!Struct)
	{
		UE_LOG(LogOliveCppReader, Warning, TEXT("ReadStruct: Struct '%s' not found"), *StructName);
		return {};
	}

	FOliveIRCppStruct IR;
	IR.StructName = Struct->GetName();

	// Parent struct
	if (UScriptStruct* SuperStruct = Cast<UScriptStruct>(Struct->GetSuperStruct()))
	{
		IR.ParentStructName = SuperStruct->GetName();
	}

	// BlueprintType is metadata
#if WITH_METADATA
	IR.bIsBlueprintType = Struct->GetBoolMetaData(TEXT("BlueprintType"));
#endif

	// Properties
	EFieldIteratorFlags::SuperClassFlags SuperFlags = bIncludeInherited
		? EFieldIteratorFlags::IncludeSuper
		: EFieldIteratorFlags::ExcludeSuper;

	for (TFieldIterator<FProperty> PropIt(Struct, SuperFlags); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		IR.Properties.Add(ConvertProperty(Prop));
	}

	// Struct-level metadata
	IR.Metadata = ExtractMetadata(Struct);

	UE_LOG(LogOliveCppReader, Log, TEXT("ReadStruct: Read '%s' with %d properties"), *IR.StructName, IR.Properties.Num());

	return IR;
}
