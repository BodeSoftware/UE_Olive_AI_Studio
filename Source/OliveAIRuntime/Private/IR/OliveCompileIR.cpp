// Copyright Bode Software. All Rights Reserved.

#include "IR/OliveCompileIR.h"
#include "Serialization/JsonSerializer.h"

// ============================================================================
// FOliveIRCompileError
// ============================================================================

TSharedPtr<FJsonObject> FOliveIRCompileError::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	Json->SetStringField(TEXT("message"), Message);
	Json->SetStringField(TEXT("severity"), GetSeverityString());

	if (!NodeId.IsEmpty())
	{
		Json->SetStringField(TEXT("node_id"), NodeId);
	}
	if (!NodeName.IsEmpty())
	{
		Json->SetStringField(TEXT("node_name"), NodeName);
	}
	if (!GraphName.IsEmpty())
	{
		Json->SetStringField(TEXT("graph_name"), GraphName);
	}
	if (Line > 0)
	{
		Json->SetNumberField(TEXT("line"), Line);
	}
	if (!Suggestion.IsEmpty())
	{
		Json->SetStringField(TEXT("suggestion"), Suggestion);
	}

	return Json;
}

FOliveIRCompileError FOliveIRCompileError::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRCompileError Error;

	if (!JsonObject.IsValid())
	{
		return Error;
	}

	Error.Message = JsonObject->GetStringField(TEXT("message"));

	FString SeverityStr = JsonObject->GetStringField(TEXT("severity"));
	if (SeverityStr == TEXT("Warning"))
	{
		Error.Severity = EOliveIRCompileErrorSeverity::Warning;
	}
	else if (SeverityStr == TEXT("Note"))
	{
		Error.Severity = EOliveIRCompileErrorSeverity::Note;
	}
	else
	{
		Error.Severity = EOliveIRCompileErrorSeverity::Error;
	}

	Error.NodeId = JsonObject->GetStringField(TEXT("node_id"));
	Error.NodeName = JsonObject->GetStringField(TEXT("node_name"));
	Error.GraphName = JsonObject->GetStringField(TEXT("graph_name"));
	Error.Line = static_cast<int32>(JsonObject->GetNumberField(TEXT("line")));
	Error.Suggestion = JsonObject->GetStringField(TEXT("suggestion"));

	return Error;
}

FString FOliveIRCompileError::GetSeverityString() const
{
	switch (Severity)
	{
	case EOliveIRCompileErrorSeverity::Error:
		return TEXT("Error");
	case EOliveIRCompileErrorSeverity::Warning:
		return TEXT("Warning");
	case EOliveIRCompileErrorSeverity::Note:
		return TEXT("Note");
	default:
		return TEXT("Unknown");
	}
}

FOliveIRCompileError FOliveIRCompileError::MakeError(const FString& InMessage, const FString& InSuggestion)
{
	FOliveIRCompileError Error;
	Error.Message = InMessage;
	Error.Severity = EOliveIRCompileErrorSeverity::Error;
	Error.Suggestion = InSuggestion;
	return Error;
}

FOliveIRCompileError FOliveIRCompileError::MakeWarning(const FString& InMessage, const FString& InSuggestion)
{
	FOliveIRCompileError Warning;
	Warning.Message = InMessage;
	Warning.Severity = EOliveIRCompileErrorSeverity::Warning;
	Warning.Suggestion = InSuggestion;
	return Warning;
}

FOliveIRCompileError FOliveIRCompileError::MakeNote(const FString& InMessage)
{
	FOliveIRCompileError Note;
	Note.Message = InMessage;
	Note.Severity = EOliveIRCompileErrorSeverity::Note;
	return Note;
}

// ============================================================================
// FOliveIRCompileResult
// ============================================================================

TSharedPtr<FJsonObject> FOliveIRCompileResult::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	Json->SetBoolField(TEXT("success"), bSuccess);
	Json->SetNumberField(TEXT("compile_time_ms"), CompileTimeMs);
	Json->SetNumberField(TEXT("error_count"), Errors.Num());
	Json->SetNumberField(TEXT("warning_count"), Warnings.Num());

	if (Errors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrorsArray;
		for (const FOliveIRCompileError& Error : Errors)
		{
			ErrorsArray.Add(MakeShared<FJsonValueObject>(Error.ToJson()));
		}
		Json->SetArrayField(TEXT("errors"), ErrorsArray);
	}

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FOliveIRCompileError& Warning : Warnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueObject>(Warning.ToJson()));
		}
		Json->SetArrayField(TEXT("warnings"), WarningsArray);
	}

	return Json;
}

FString FOliveIRCompileResult::ToJsonString() const
{
	TSharedPtr<FJsonObject> JsonObject = ToJson();
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return OutputString;
}

FOliveIRCompileResult FOliveIRCompileResult::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FOliveIRCompileResult Result;

	if (!JsonObject.IsValid())
	{
		return Result;
	}

	Result.bSuccess = JsonObject->GetBoolField(TEXT("success"));
	Result.CompileTimeMs = JsonObject->GetNumberField(TEXT("compile_time_ms"));

	const TArray<TSharedPtr<FJsonValue>>* ErrorsArray;
	if (JsonObject->TryGetArrayField(TEXT("errors"), ErrorsArray))
	{
		for (const auto& ErrorValue : *ErrorsArray)
		{
			Result.Errors.Add(FOliveIRCompileError::FromJson(ErrorValue->AsObject()));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* WarningsArray;
	if (JsonObject->TryGetArrayField(TEXT("warnings"), WarningsArray))
	{
		for (const auto& WarningValue : *WarningsArray)
		{
			Result.Warnings.Add(FOliveIRCompileError::FromJson(WarningValue->AsObject()));
		}
	}

	return Result;
}

FOliveIRCompileResult FOliveIRCompileResult::Success(double InCompileTimeMs)
{
	FOliveIRCompileResult Result;
	Result.bSuccess = true;
	Result.CompileTimeMs = InCompileTimeMs;
	return Result;
}

FOliveIRCompileResult FOliveIRCompileResult::Failure(const FString& ErrorMessage, const FString& Suggestion)
{
	FOliveIRCompileResult Result;
	Result.bSuccess = false;
	Result.AddError(FOliveIRCompileError::MakeError(ErrorMessage, Suggestion));
	return Result;
}

void FOliveIRCompileResult::AddError(const FOliveIRCompileError& Error)
{
	Errors.Add(Error);
	bSuccess = false;
}

void FOliveIRCompileResult::AddWarning(const FOliveIRCompileError& Warning)
{
	FOliveIRCompileError WarningCopy = Warning;
	WarningCopy.Severity = EOliveIRCompileErrorSeverity::Warning;
	Warnings.Add(WarningCopy);
}
