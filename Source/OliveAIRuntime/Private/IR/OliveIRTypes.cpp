// Copyright Bode Software. All Rights Reserved.

#include "IR/OliveIRTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

TSharedPtr<FJsonObject> FOliveIRType::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	// Convert category enum to string
	FString CategoryStr;
	switch (Category)
	{
		case EOliveIRTypeCategory::Bool: CategoryStr = TEXT("bool"); break;
		case EOliveIRTypeCategory::Byte: CategoryStr = TEXT("byte"); break;
		case EOliveIRTypeCategory::Int: CategoryStr = TEXT("int"); break;
		case EOliveIRTypeCategory::Int64: CategoryStr = TEXT("int64"); break;
		case EOliveIRTypeCategory::Float: CategoryStr = TEXT("float"); break;
		case EOliveIRTypeCategory::Double: CategoryStr = TEXT("double"); break;
		case EOliveIRTypeCategory::String: CategoryStr = TEXT("string"); break;
		case EOliveIRTypeCategory::Name: CategoryStr = TEXT("name"); break;
		case EOliveIRTypeCategory::Text: CategoryStr = TEXT("text"); break;
		case EOliveIRTypeCategory::Vector: CategoryStr = TEXT("vector"); break;
		case EOliveIRTypeCategory::Vector2D: CategoryStr = TEXT("vector2d"); break;
		case EOliveIRTypeCategory::Rotator: CategoryStr = TEXT("rotator"); break;
		case EOliveIRTypeCategory::Transform: CategoryStr = TEXT("transform"); break;
		case EOliveIRTypeCategory::Color: CategoryStr = TEXT("color"); break;
		case EOliveIRTypeCategory::LinearColor: CategoryStr = TEXT("linearcolor"); break;
		case EOliveIRTypeCategory::Object: CategoryStr = TEXT("object"); break;
		case EOliveIRTypeCategory::Class: CategoryStr = TEXT("class"); break;
		case EOliveIRTypeCategory::Interface: CategoryStr = TEXT("interface"); break;
		case EOliveIRTypeCategory::Struct: CategoryStr = TEXT("struct"); break;
		case EOliveIRTypeCategory::Enum: CategoryStr = TEXT("enum"); break;
		case EOliveIRTypeCategory::Delegate: CategoryStr = TEXT("delegate"); break;
		case EOliveIRTypeCategory::MulticastDelegate: CategoryStr = TEXT("multicastdelegate"); break;
		case EOliveIRTypeCategory::Array: CategoryStr = TEXT("array"); break;
		case EOliveIRTypeCategory::Set: CategoryStr = TEXT("set"); break;
		case EOliveIRTypeCategory::Map: CategoryStr = TEXT("map"); break;
		case EOliveIRTypeCategory::Exec: CategoryStr = TEXT("exec"); break;
		case EOliveIRTypeCategory::Wildcard: CategoryStr = TEXT("wildcard"); break;
		default: CategoryStr = TEXT("unknown"); break;
	}

	Json->SetStringField(TEXT("category"), CategoryStr);

	if (!ClassName.IsEmpty())
	{
		Json->SetStringField(TEXT("class"), ClassName);
	}
	if (!StructName.IsEmpty())
	{
		Json->SetStringField(TEXT("struct_name"), StructName);
	}
	if (!EnumName.IsEmpty())
	{
		Json->SetStringField(TEXT("enum_name"), EnumName);
	}
	if (!ElementTypeJson.IsEmpty())
	{
		Json->SetStringField(TEXT("element_type"), ElementTypeJson);
	}
	if (!KeyTypeJson.IsEmpty())
	{
		Json->SetStringField(TEXT("key_type"), KeyTypeJson);
	}
	if (!ValueTypeJson.IsEmpty())
	{
		Json->SetStringField(TEXT("value_type"), ValueTypeJson);
	}
	if (bIsReference)
	{
		Json->SetBoolField(TEXT("is_reference"), bIsReference);
	}
	if (bIsConst)
	{
		Json->SetBoolField(TEXT("is_const"), bIsConst);
	}

	return Json;
}

FOliveIRType FOliveIRType::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRType Type;
	if (!JsonObject.IsValid())
	{
		return Type;
	}

	FString CategoryStr = JsonObject->GetStringField(TEXT("category"));
	if (CategoryStr == TEXT("bool")) Type.Category = EOliveIRTypeCategory::Bool;
	else if (CategoryStr == TEXT("byte")) Type.Category = EOliveIRTypeCategory::Byte;
	else if (CategoryStr == TEXT("int")) Type.Category = EOliveIRTypeCategory::Int;
	else if (CategoryStr == TEXT("int64")) Type.Category = EOliveIRTypeCategory::Int64;
	else if (CategoryStr == TEXT("float")) Type.Category = EOliveIRTypeCategory::Float;
	else if (CategoryStr == TEXT("double")) Type.Category = EOliveIRTypeCategory::Double;
	else if (CategoryStr == TEXT("string")) Type.Category = EOliveIRTypeCategory::String;
	else if (CategoryStr == TEXT("name")) Type.Category = EOliveIRTypeCategory::Name;
	else if (CategoryStr == TEXT("text")) Type.Category = EOliveIRTypeCategory::Text;
	else if (CategoryStr == TEXT("vector")) Type.Category = EOliveIRTypeCategory::Vector;
	else if (CategoryStr == TEXT("vector2d")) Type.Category = EOliveIRTypeCategory::Vector2D;
	else if (CategoryStr == TEXT("rotator")) Type.Category = EOliveIRTypeCategory::Rotator;
	else if (CategoryStr == TEXT("transform")) Type.Category = EOliveIRTypeCategory::Transform;
	else if (CategoryStr == TEXT("color")) Type.Category = EOliveIRTypeCategory::Color;
	else if (CategoryStr == TEXT("linearcolor")) Type.Category = EOliveIRTypeCategory::LinearColor;
	else if (CategoryStr == TEXT("object")) Type.Category = EOliveIRTypeCategory::Object;
	else if (CategoryStr == TEXT("class")) Type.Category = EOliveIRTypeCategory::Class;
	else if (CategoryStr == TEXT("interface")) Type.Category = EOliveIRTypeCategory::Interface;
	else if (CategoryStr == TEXT("struct")) Type.Category = EOliveIRTypeCategory::Struct;
	else if (CategoryStr == TEXT("enum")) Type.Category = EOliveIRTypeCategory::Enum;
	else if (CategoryStr == TEXT("delegate")) Type.Category = EOliveIRTypeCategory::Delegate;
	else if (CategoryStr == TEXT("multicastdelegate")) Type.Category = EOliveIRTypeCategory::MulticastDelegate;
	else if (CategoryStr == TEXT("array")) Type.Category = EOliveIRTypeCategory::Array;
	else if (CategoryStr == TEXT("set")) Type.Category = EOliveIRTypeCategory::Set;
	else if (CategoryStr == TEXT("map")) Type.Category = EOliveIRTypeCategory::Map;
	else if (CategoryStr == TEXT("exec")) Type.Category = EOliveIRTypeCategory::Exec;
	else if (CategoryStr == TEXT("wildcard")) Type.Category = EOliveIRTypeCategory::Wildcard;
	else Type.Category = EOliveIRTypeCategory::Unknown;

	Type.ClassName = JsonObject->GetStringField(TEXT("class"));
	Type.StructName = JsonObject->GetStringField(TEXT("struct_name"));
	Type.EnumName = JsonObject->GetStringField(TEXT("enum_name"));
	Type.ElementTypeJson = JsonObject->GetStringField(TEXT("element_type"));
	Type.KeyTypeJson = JsonObject->GetStringField(TEXT("key_type"));
	Type.ValueTypeJson = JsonObject->GetStringField(TEXT("value_type"));
	Type.bIsReference = JsonObject->GetBoolField(TEXT("is_reference"));
	Type.bIsConst = JsonObject->GetBoolField(TEXT("is_const"));

	return Type;
}

FString FOliveIRType::GetDisplayName() const
{
	switch (Category)
	{
		case EOliveIRTypeCategory::Bool: return TEXT("Boolean");
		case EOliveIRTypeCategory::Byte: return TEXT("Byte");
		case EOliveIRTypeCategory::Int: return TEXT("Integer");
		case EOliveIRTypeCategory::Int64: return TEXT("Integer64");
		case EOliveIRTypeCategory::Float: return TEXT("Float");
		case EOliveIRTypeCategory::Double: return TEXT("Double");
		case EOliveIRTypeCategory::String: return TEXT("String");
		case EOliveIRTypeCategory::Name: return TEXT("Name");
		case EOliveIRTypeCategory::Text: return TEXT("Text");
		case EOliveIRTypeCategory::Vector: return TEXT("Vector");
		case EOliveIRTypeCategory::Vector2D: return TEXT("Vector2D");
		case EOliveIRTypeCategory::Rotator: return TEXT("Rotator");
		case EOliveIRTypeCategory::Transform: return TEXT("Transform");
		case EOliveIRTypeCategory::Color: return TEXT("Color");
		case EOliveIRTypeCategory::LinearColor: return TEXT("Linear Color");
		case EOliveIRTypeCategory::Object: return FString::Printf(TEXT("%s*"), *ClassName);
		case EOliveIRTypeCategory::Class: return FString::Printf(TEXT("TSubclassOf<%s>"), *ClassName);
		case EOliveIRTypeCategory::Interface: return FString::Printf(TEXT("TScriptInterface<%s>"), *ClassName);
		case EOliveIRTypeCategory::Struct: return StructName;
		case EOliveIRTypeCategory::Enum: return EnumName;
		case EOliveIRTypeCategory::Delegate: return TEXT("Delegate");
		case EOliveIRTypeCategory::MulticastDelegate: return TEXT("Multicast Delegate");
		case EOliveIRTypeCategory::Array: return TEXT("Array");
		case EOliveIRTypeCategory::Set: return TEXT("Set");
		case EOliveIRTypeCategory::Map: return TEXT("Map");
		case EOliveIRTypeCategory::Exec: return TEXT("Exec");
		case EOliveIRTypeCategory::Wildcard: return TEXT("Wildcard");
		default: return TEXT("Unknown");
	}
}

FOliveIRResult FOliveIRResult::Success(const TSharedPtr<FJsonObject>& InData)
{
	FOliveIRResult Result;
	Result.bSuccess = true;
	Result.Data = InData;
	return Result;
}

FOliveIRResult FOliveIRResult::Error(const FString& Code, const FString& Message, const FString& InSuggestion)
{
	FOliveIRResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = Code;
	Result.ErrorMessage = Message;
	Result.Suggestion = InSuggestion;
	return Result;
}

TSharedPtr<FJsonObject> FOliveIRResult::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("success"), bSuccess);

	if (bSuccess)
	{
		if (Data.IsValid())
		{
			Json->SetObjectField(TEXT("data"), Data);
		}
	}
	else
	{
		TSharedPtr<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
		ErrorJson->SetStringField(TEXT("code"), ErrorCode);
		ErrorJson->SetStringField(TEXT("message"), ErrorMessage);
		if (!Suggestion.IsEmpty())
		{
			ErrorJson->SetStringField(TEXT("suggestion"), Suggestion);
		}
		Json->SetObjectField(TEXT("error"), ErrorJson);
	}

	return Json;
}

TSharedPtr<FJsonObject> FOliveIRMessage::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	FString SeverityStr;
	switch (Severity)
	{
		case EOliveIRSeverity::Info: SeverityStr = TEXT("info"); break;
		case EOliveIRSeverity::Warning: SeverityStr = TEXT("warning"); break;
		case EOliveIRSeverity::Error: SeverityStr = TEXT("error"); break;
	}
	Json->SetStringField(TEXT("severity"), SeverityStr);
	Json->SetStringField(TEXT("code"), Code);
	Json->SetStringField(TEXT("message"), Message);

	if (!Suggestion.IsEmpty())
	{
		Json->SetStringField(TEXT("suggestion"), Suggestion);
	}

	if (Details.Num() > 0)
	{
		TSharedPtr<FJsonObject> DetailsJson = MakeShared<FJsonObject>();
		for (const auto& Pair : Details)
		{
			DetailsJson->SetStringField(Pair.Key, Pair.Value);
		}
		Json->SetObjectField(TEXT("details"), DetailsJson);
	}

	return Json;
}

FOliveIRMessage FOliveIRMessage::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRMessage Msg;
	if (!JsonObject.IsValid())
	{
		return Msg;
	}

	FString SeverityStr = JsonObject->GetStringField(TEXT("severity"));
	if (SeverityStr == TEXT("info")) Msg.Severity = EOliveIRSeverity::Info;
	else if (SeverityStr == TEXT("warning")) Msg.Severity = EOliveIRSeverity::Warning;
	else if (SeverityStr == TEXT("error")) Msg.Severity = EOliveIRSeverity::Error;

	Msg.Code = JsonObject->GetStringField(TEXT("code"));
	Msg.Message = JsonObject->GetStringField(TEXT("message"));
	Msg.Suggestion = JsonObject->GetStringField(TEXT("suggestion"));

	const TSharedPtr<FJsonObject>* DetailsJson;
	if (JsonObject->TryGetObjectField(TEXT("details"), DetailsJson))
	{
		for (const auto& Pair : (*DetailsJson)->Values)
		{
			Msg.Details.Add(Pair.Key, Pair.Value->AsString());
		}
	}

	return Msg;
}
