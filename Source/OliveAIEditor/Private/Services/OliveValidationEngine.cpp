// Copyright Bode Software. All Rights Reserved.

#include "Services/OliveValidationEngine.h"
#include "Services/OliveAssetResolver.h"
#include "OliveAIEditorModule.h"
#include "Editor.h"
#include "Serialization/JsonSerializer.h"

// FOliveValidationResult implementation

void FOliveValidationResult::AddError(const FString& Code, const FString& Message, const FString& Suggestion)
{
	bValid = false;

	FOliveIRMessage Msg;
	Msg.Severity = EOliveIRSeverity::Error;
	Msg.Code = Code;
	Msg.Message = Message;
	Msg.Suggestion = Suggestion;
	Messages.Add(Msg);
}

void FOliveValidationResult::AddWarning(const FString& Code, const FString& Message, const FString& Suggestion)
{
	FOliveIRMessage Msg;
	Msg.Severity = EOliveIRSeverity::Warning;
	Msg.Code = Code;
	Msg.Message = Message;
	Msg.Suggestion = Suggestion;
	Messages.Add(Msg);
}

void FOliveValidationResult::AddInfo(const FString& Code, const FString& Message)
{
	FOliveIRMessage Msg;
	Msg.Severity = EOliveIRSeverity::Info;
	Msg.Code = Code;
	Msg.Message = Message;
	Messages.Add(Msg);
}

bool FOliveValidationResult::HasErrors() const
{
	return !bValid || Messages.ContainsByPredicate([](const FOliveIRMessage& Msg)
	{
		return Msg.Severity == EOliveIRSeverity::Error;
	});
}

bool FOliveValidationResult::HasWarnings() const
{
	return Messages.ContainsByPredicate([](const FOliveIRMessage& Msg)
	{
		return Msg.Severity == EOliveIRSeverity::Warning;
	});
}

TArray<FOliveIRMessage> FOliveValidationResult::GetErrors() const
{
	return Messages.FilterByPredicate([](const FOliveIRMessage& Msg)
	{
		return Msg.Severity == EOliveIRSeverity::Error;
	});
}

TArray<FOliveIRMessage> FOliveValidationResult::GetWarnings() const
{
	return Messages.FilterByPredicate([](const FOliveIRMessage& Msg)
	{
		return Msg.Severity == EOliveIRSeverity::Warning;
	});
}

TSharedPtr<FJsonObject> FOliveValidationResult::ToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("valid"), bValid);

	if (Messages.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> MessagesArray;
		for (const FOliveIRMessage& Msg : Messages)
		{
			MessagesArray.Add(MakeShared<FJsonValueObject>(Msg.ToJson()));
		}
		Json->SetArrayField(TEXT("messages"), MessagesArray);
	}

	return Json;
}

void FOliveValidationResult::Merge(const FOliveValidationResult& Other)
{
	if (!Other.bValid)
	{
		bValid = false;
	}
	Messages.Append(Other.Messages);
}

// FOliveValidationEngine implementation

FOliveValidationEngine::FOliveValidationEngine()
{
}

FOliveValidationEngine& FOliveValidationEngine::Get()
{
	static FOliveValidationEngine Instance;
	return Instance;
}

void FOliveValidationEngine::RegisterRule(TSharedPtr<IOliveValidationRule> Rule)
{
	if (!Rule.IsValid())
	{
		return;
	}

	FRWScopeLock WriteLock(RulesLock, SLT_Write);

	// Remove existing rule with same name
	Rules.RemoveAll([&Rule](const TSharedPtr<IOliveValidationRule>& Existing)
	{
		return Existing->GetRuleName() == Rule->GetRuleName();
	});

	Rules.Add(Rule);

	UE_LOG(LogOliveAI, Verbose, TEXT("Registered validation rule: %s"), *Rule->GetRuleName().ToString());
}

void FOliveValidationEngine::UnregisterRule(FName RuleName)
{
	FRWScopeLock WriteLock(RulesLock, SLT_Write);

	Rules.RemoveAll([&RuleName](const TSharedPtr<IOliveValidationRule>& Rule)
	{
		return Rule->GetRuleName() == RuleName;
	});
}

FOliveValidationResult FOliveValidationEngine::ValidateOperation(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult CombinedResult;

	FRWScopeLock ReadLock(RulesLock, SLT_ReadOnly);

	for (const TSharedPtr<IOliveValidationRule>& Rule : Rules)
	{
		// Check if rule applies to this tool
		TArray<FString> ApplicableTools = Rule->GetApplicableTools();
		if (ApplicableTools.Num() > 0 && !ApplicableTools.Contains(ToolName))
		{
			continue;
		}

		// Run validation
		FOliveValidationResult RuleResult = Rule->Validate(ToolName, Params, TargetAsset);
		CombinedResult.Merge(RuleResult);

		// Stop on first error if needed
		if (!RuleResult.bValid)
		{
			UE_LOG(LogOliveAI, Warning, TEXT("Validation failed for %s: rule %s"),
				*ToolName, *Rule->GetRuleName().ToString());
		}
	}

	return CombinedResult;
}

void FOliveValidationEngine::RegisterCoreRules()
{
	RegisterRule(MakeShared<FOlivePIEProtectionRule>());
	RegisterRule(MakeShared<FOliveSchemaValidationRule>());
	RegisterRule(MakeShared<FOliveAssetExistsRule>());
}

void FOliveValidationEngine::RegisterBlueprintRules()
{
	// Blueprint-specific rules will be added in Phase 1
}

void FOliveValidationEngine::RegisterBehaviorTreeRules()
{
	// BT-specific rules will be added in Phase 2
}

void FOliveValidationEngine::RegisterPCGRules()
{
	// PCG-specific rules will be added in Phase 3
}

TArray<FName> FOliveValidationEngine::GetRegisteredRules() const
{
	TArray<FName> Names;

	FRWScopeLock ReadLock(RulesLock, SLT_ReadOnly);
	for (const TSharedPtr<IOliveValidationRule>& Rule : Rules)
	{
		Names.Add(Rule->GetRuleName());
	}

	return Names;
}

// FOlivePIEProtectionRule implementation

FOliveValidationResult FOlivePIEProtectionRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	// Check if we're in PIE
	bool bInPIE = false;
	if (GEditor)
	{
		bInPIE = GEditor->bIsSimulatingInEditor || GEditor->PlayWorld != nullptr;
	}

	if (bInPIE)
	{
		// Only block write operations
		static TArray<FString> ReadOnlyTools = {
			TEXT("blueprint.read"),
			TEXT("blueprint.read_function"),
			TEXT("project.search"),
			TEXT("project.get_info")
		};

		if (!ReadOnlyTools.Contains(ToolName))
		{
			Result.AddError(
				TEXT("PIE_ACTIVE"),
				TEXT("Cannot modify assets while Play in Editor is active."),
				TEXT("Stop PIE and try again.")
			);
		}
	}

	return Result;
}

// FOliveSchemaValidationRule implementation

void FOliveSchemaValidationRule::RegisterSchema(const FString& ToolName, const TSharedPtr<FJsonObject>& Schema)
{
	ToolSchemas.Add(ToolName, Schema);
}

FOliveValidationResult FOliveSchemaValidationRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	// Get schema for this tool
	TSharedPtr<FJsonObject>* SchemaPtr = ToolSchemas.Find(ToolName);
	if (!SchemaPtr || !SchemaPtr->IsValid())
	{
		// No schema registered - skip validation
		return Result;
	}

	// TODO: Implement full JSON Schema validation
	// For now, just check required fields

	const TSharedPtr<FJsonObject>& Schema = *SchemaPtr;
	const TArray<TSharedPtr<FJsonValue>>* RequiredFields;

	if (Schema->TryGetArrayField(TEXT("required"), RequiredFields))
	{
		for (const TSharedPtr<FJsonValue>& RequiredField : *RequiredFields)
		{
			FString FieldName = RequiredField->AsString();
			if (!Params.IsValid() || !Params->HasField(FieldName))
			{
				Result.AddError(
					TEXT("MISSING_REQUIRED_FIELD"),
					FString::Printf(TEXT("Missing required field: %s"), *FieldName),
					FString::Printf(TEXT("Add the '%s' field to the parameters."), *FieldName)
				);
			}
		}
	}

	return Result;
}

// FOliveAssetExistsRule implementation

FOliveValidationResult FOliveAssetExistsRule::Validate(
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& Params,
	UObject* TargetAsset)
{
	FOliveValidationResult Result;

	// Only check for tools that operate on existing assets
	static TArray<FString> AssetRequiredTools = {
		TEXT("blueprint.read"),
		TEXT("blueprint.add_variable"),
		TEXT("blueprint.add_component"),
		TEXT("blueprint.add_function"),
		TEXT("behaviortree.read"),
		TEXT("blackboard.read")
	};

	if (!AssetRequiredTools.Contains(ToolName))
	{
		return Result;
	}

	// Get the path parameter
	if (!Params.IsValid())
	{
		Result.AddError(
			TEXT("MISSING_PARAMS"),
			TEXT("No parameters provided."),
			TEXT("Provide the required parameters.")
		);
		return Result;
	}

	FString AssetPath = Params->GetStringField(TEXT("path"));
	if (AssetPath.IsEmpty())
	{
		AssetPath = Params->GetStringField(TEXT("blueprint"));
	}
	if (AssetPath.IsEmpty())
	{
		AssetPath = Params->GetStringField(TEXT("asset"));
	}

	if (AssetPath.IsEmpty())
	{
		// No path field - might be okay for some tools
		return Result;
	}

	// Check if asset exists
	if (!FOliveAssetResolver::Get().DoesAssetExist(AssetPath))
	{
		Result.AddError(
			TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath),
			TEXT("Use project.search to find the correct asset path.")
		);
	}

	return Result;
}
