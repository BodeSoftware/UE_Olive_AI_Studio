// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "OliveIRTypes.generated.h"

/**
 * Common type categories used in IR pin/variable type definitions
 */
UENUM(BlueprintType)
enum class EOliveIRTypeCategory : uint8
{
	Bool,
	Byte,
	Int,
	Int64,
	Float,
	Double,
	String,		// FString
	Name,		// FName
	Text,		// FText
	Vector,
	Vector2D,
	Rotator,
	Transform,
	Color,
	LinearColor,
	Object,		// UObject*
	Class,		// TSubclassOf<T>
	Interface,	// Interface reference (TScriptInterface)
	Struct,		// USTRUCT
	Enum,		// UENUM
	Delegate,
	MulticastDelegate,
	Array,
	Set,
	Map,
	Exec,		// Execution pin (flow control)
	Wildcard,	// Template/generic pin
	Unknown
};

/**
 * Represents a type in the IR system
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRType
{
	GENERATED_BODY()

	/** Primary type category */
	UPROPERTY()
	EOliveIRTypeCategory Category = EOliveIRTypeCategory::Unknown;

	/** For Object/Class: the class name */
	UPROPERTY()
	FString ClassName;

	/** For Struct: the struct name */
	UPROPERTY()
	FString StructName;

	/** For Enum: the enum name */
	UPROPERTY()
	FString EnumName;

	/** For Array/Set: the element type (serialized) */
	UPROPERTY()
	FString ElementTypeJson;

	/** For Map: the key type (serialized) */
	UPROPERTY()
	FString KeyTypeJson;

	/** For Map: the value type (serialized) */
	UPROPERTY()
	FString ValueTypeJson;

	/** For Delegate: the signature info */
	UPROPERTY()
	FString DelegateSignatureJson;

	/** Whether this type is a reference */
	UPROPERTY()
	bool bIsReference = false;

	/** Whether this type is const */
	UPROPERTY()
	bool bIsConst = false;

	/** Convert to JSON representation */
	TSharedPtr<FJsonObject> ToJson() const;

	/** Parse from JSON representation */
	static FOliveIRType FromJson(const TSharedPtr<FJsonObject>& JsonObject);

	/** Get display name for this type */
	FString GetDisplayName() const;
};

/**
 * Result of an IR operation
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRResult
{
	GENERATED_BODY()

	UPROPERTY()
	bool bSuccess = false;

	UPROPERTY()
	FString ErrorCode;

	UPROPERTY()
	FString ErrorMessage;

	UPROPERTY()
	FString Suggestion;

	/** Additional details as JSON */
	TSharedPtr<FJsonObject> Data;

	static FOliveIRResult Success(const TSharedPtr<FJsonObject>& InData = nullptr);
	static FOliveIRResult Error(const FString& Code, const FString& Message, const FString& InSuggestion = TEXT(""));

	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * Severity levels for validation messages
 */
UENUM(BlueprintType)
enum class EOliveIRSeverity : uint8
{
	Info,
	Warning,
	Error
};

/**
 * A validation or status message
 */
USTRUCT(BlueprintType)
struct OLIVEAIRUNTIME_API FOliveIRMessage
{
	GENERATED_BODY()

	UPROPERTY()
	EOliveIRSeverity Severity = EOliveIRSeverity::Info;

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Message;

	UPROPERTY()
	FString Suggestion;

	UPROPERTY()
	TMap<FString, FString> Details;

	TSharedPtr<FJsonObject> ToJson() const;
	static FOliveIRMessage FromJson(const TSharedPtr<FJsonObject>& JsonObject);
};
