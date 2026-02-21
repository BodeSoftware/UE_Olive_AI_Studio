// Copyright Bode Software. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IR/CppIR.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveCppReader, Log, All);

/**
 * FOliveCppReflectionReader
 *
 * Reads UE reflection data (UClass, UEnum, UScriptStruct) and
 * converts to CppIR structs. All methods are static and must be
 * called on the game thread (reflection iteration is not thread-safe).
 */
class OLIVEAIEDITOR_API FOliveCppReflectionReader
{
public:
	/**
	 * Find a UClass by name, handling prefix stripping and /Script/ paths.
	 * Tries: exact match, with 'A' prefix, with 'U' prefix, /Script/ paths,
	 * then falls back to brute-force TObjectIterator search.
	 */
	static UClass* FindClassByName(const FString& ClassName);

	/**
	 * Read full class reflection data.
	 * @param ClassName Class to read
	 * @param bIncludeInherited Include inherited properties/functions
	 * @param bIncludeFunctions Include functions
	 * @param bIncludeProperties Include properties
	 * @return Class IR or empty optional if not found
	 */
	static TOptional<FOliveIRCppClass> ReadClass(
		const FString& ClassName,
		bool bIncludeInherited = false,
		bool bIncludeFunctions = true,
		bool bIncludeProperties = true
	);

	/**
	 * List all BlueprintCallable/BlueprintPure functions on a class.
	 */
	static TArray<FOliveIRCppFunction> ListBlueprintCallable(
		const FString& ClassName,
		bool bIncludeInherited = true
	);

	/**
	 * List all overridable functions (BlueprintImplementableEvent + BlueprintNativeEvent).
	 */
	static TArray<FOliveIRCppFunction> ListOverridable(const FString& ClassName);

	/**
	 * Read enum reflection data.
	 */
	static TOptional<FOliveIRCppEnum> ReadEnum(const FString& EnumName);

	/**
	 * Read struct reflection data.
	 */
	static TOptional<FOliveIRCppStruct> ReadStruct(
		const FString& StructName,
		bool bIncludeInherited = false
	);

private:
	/** Convert a UFunction to IR */
	static FOliveIRCppFunction ConvertFunction(const UFunction* Function);

	/** Convert an FProperty to IR */
	static FOliveIRCppProperty ConvertProperty(const FProperty* Property);

	/** Get the C++ type name string for an FProperty */
	static FString GetPropertyTypeName(const FProperty* Property);

	/** Extract property flags into IR struct */
	static FOliveIRCppPropertyFlags ExtractPropertyFlags(const FProperty* Property);

	/** Extract function flags into IR struct */
	static FOliveIRCppFunctionFlags ExtractFunctionFlags(const UFunction* Function);

	/** Extract metadata from a UField (UClass, UFunction, UEnum, etc.) */
	static TMap<FString, FString> ExtractMetadata(const UField* Field);

	/** Extract metadata from an FField (FProperty) */
	static TMap<FString, FString> ExtractMetadata(const FField* Field);

	/** Find a UEnum by name */
	static UEnum* FindEnumByName(const FString& EnumName);

	/** Find a UScriptStruct by name */
	static UScriptStruct* FindStructByName(const FString& StructName);
};
