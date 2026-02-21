// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CommonIR.h"
#include "CppIR.generated.h"

/**
 * UPROPERTY flags for a C++ reflected property
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRCppPropertyFlags
{
	GENERATED_BODY()

	UPROPERTY()
	bool bBlueprintReadOnly = false;

	UPROPERTY()
	bool bBlueprintReadWrite = false;

	UPROPERTY()
	bool bEditAnywhere = false;

	UPROPERTY()
	bool bEditDefaultsOnly = false;

	UPROPERTY()
	bool bEditInstanceOnly = false;

	UPROPERTY()
	bool bVisibleAnywhere = false;

	UPROPERTY()
	bool bConfig = false;

	UPROPERTY()
	bool bTransient = false;

	UPROPERTY()
	bool bReplicated = false;

	UPROPERTY()
	bool bExposeOnSpawn = false;

	UPROPERTY()
	bool bSaveGame = false;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRCppPropertyFlags FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * UFUNCTION flags for a C++ reflected function
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRCppFunctionFlags
{
	GENERATED_BODY()

	UPROPERTY()
	bool bBlueprintCallable = false;

	UPROPERTY()
	bool bBlueprintPure = false;

	UPROPERTY()
	bool bBlueprintImplementableEvent = false;

	UPROPERTY()
	bool bBlueprintNativeEvent = false;

	UPROPERTY()
	bool bCallInEditor = false;

	UPROPERTY()
	bool bServer = false;

	UPROPERTY()
	bool bClient = false;

	UPROPERTY()
	bool bNetMulticast = false;

	UPROPERTY()
	bool bReliable = false;

	UPROPERTY()
	bool bExec = false;

	UPROPERTY()
	bool bConst = false;

	UPROPERTY()
	bool bStatic = false;

	UPROPERTY()
	bool bVirtual = false;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRCppFunctionFlags FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * A reflected C++ property (UPROPERTY or function parameter)
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRCppProperty
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	/** Full C++ type string (e.g., "float", "TArray<FVector>", "UStaticMeshComponent*") */
	UPROPERTY()
	FString TypeName;

	/** UPROPERTY Category */
	UPROPERTY()
	FString Category;

	/** Tooltip/comment */
	UPROPERTY()
	FString Description;

	/** Default value as string */
	UPROPERTY()
	FString DefaultValue;

	UPROPERTY()
	FOliveIRCppPropertyFlags Flags;

	/** Raw metadata key-values */
	TMap<FString, FString> Metadata;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRCppProperty FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * A reflected C++ function (UFUNCTION)
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRCppFunction
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	/** Return type string (empty for void) */
	UPROPERTY()
	FString ReturnType;

	/** Function parameters (reuses property struct) */
	UPROPERTY()
	TArray<FOliveIRCppProperty> Parameters;

	/** UFUNCTION Category */
	UPROPERTY()
	FString Category;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FOliveIRCppFunctionFlags Flags;

	/** Raw metadata key-values */
	TMap<FString, FString> Metadata;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRCppFunction FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * A reflected C++ class (UCLASS)
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRCppClass
{
	GENERATED_BODY()

	/** e.g., "AMyCharacter" */
	UPROPERTY()
	FString ClassName;

	/** e.g., "ACharacter" */
	UPROPERTY()
	FString ParentClassName;

	/** e.g., "MyGame" */
	UPROPERTY()
	FString ModuleName;

	/** Relative .h path if known */
	UPROPERTY()
	FString HeaderPath;

	/** Implemented interfaces */
	UPROPERTY()
	TArray<FString> Interfaces;

	UPROPERTY()
	TArray<FOliveIRCppProperty> Properties;

	UPROPERTY()
	TArray<FOliveIRCppFunction> Functions;

	UPROPERTY()
	bool bIsAbstract = false;

	UPROPERTY()
	bool bIsBlueprintable = false;

	UPROPERTY()
	bool bIsBlueprintType = false;

	UPROPERTY()
	bool bIsDeprecated = false;

	/** Class-level metadata key-values */
	TMap<FString, FString> ClassMetadata;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRCppClass FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * A reflected C++ enum (UENUM)
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRCppEnum
{
	GENERATED_BODY()

	UPROPERTY()
	FString EnumName;

	/** "uint8", "int32", etc. */
	UPROPERTY()
	FString UnderlyingType;

	UPROPERTY()
	bool bIsBlueprintType = false;

	/** enum class vs enum */
	UPROPERTY()
	bool bIsScoped = false;

	UPROPERTY()
	TArray<FString> Values;

	/** value -> display name */
	TMap<FString, FString> ValueDisplayNames;

	/** Raw metadata key-values */
	TMap<FString, FString> Metadata;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRCppEnum FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * A reflected C++ struct (USTRUCT)
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRCppStruct
{
	GENERATED_BODY()

	UPROPERTY()
	FString StructName;

	/** If inherits from another struct */
	UPROPERTY()
	FString ParentStructName;

	UPROPERTY()
	bool bIsBlueprintType = false;

	UPROPERTY()
	TArray<FOliveIRCppProperty> Properties;

	/** Raw metadata key-values */
	TMap<FString, FString> Metadata;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRCppStruct FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};

/**
 * A C++ source file reference with optional content
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRCppSourceFile
{
	GENERATED_BODY()

	/** Relative path from project root */
	UPROPERTY()
	FString FilePath;

	/** File content (may be truncated) */
	UPROPERTY()
	FString Content;

	UPROPERTY()
	int32 TotalLines = 0;

	/** If reading a range */
	UPROPERTY()
	int32 StartLine = 0;

	UPROPERTY()
	int32 EndLine = 0;

	UPROPERTY()
	bool bIsTruncated = false;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRCppSourceFile FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};
