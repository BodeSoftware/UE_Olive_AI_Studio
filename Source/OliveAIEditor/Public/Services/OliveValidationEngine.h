// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/OliveIRTypes.h"
#include "OliveValidationEngine.generated.h"

/**
 * Validation result containing all messages
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveValidationResult
{
	GENERATED_BODY()

	/** Whether validation passed (no errors) */
	UPROPERTY()
	bool bValid = true;

	/** All validation messages */
	UPROPERTY()
	TArray<FOliveIRMessage> Messages;

	/** Add an error message */
	void AddError(const FString& Code, const FString& Message, const FString& Suggestion = TEXT(""));

	/** Add a warning message */
	void AddWarning(const FString& Code, const FString& Message, const FString& Suggestion = TEXT(""));

	/** Add an info message */
	void AddInfo(const FString& Code, const FString& Message);

	/** Check if there are any errors */
	bool HasErrors() const;

	/** Check if there are any warnings */
	bool HasWarnings() const;

	/** Get all error messages */
	TArray<FOliveIRMessage> GetErrors() const;

	/** Get all warning messages */
	TArray<FOliveIRMessage> GetWarnings() const;

	/** Convert to JSON */
	TSharedPtr<FJsonObject> ToJson() const;

	/** Combine with another result */
	void Merge(const FOliveValidationResult& Other);
};

/**
 * Interface for validation rules
 */
class OLIVEAIEDITOR_API IOliveValidationRule
{
public:
	virtual ~IOliveValidationRule() = default;

	/** Get unique rule name */
	virtual FName GetRuleName() const = 0;

	/** Get rule description */
	virtual FString GetDescription() const = 0;

	/** Get tools this rule applies to (empty = all) */
	virtual TArray<FString> GetApplicableTools() const { return {}; }

	/** Validate parameters before execution */
	virtual FOliveValidationResult Validate(
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& Params,
		UObject* TargetAsset = nullptr
	) = 0;
};

/**
 * Validation Engine
 *
 * Rule-based validation for all operations before execution.
 * Supports registering custom validation rules.
 */
class OLIVEAIEDITOR_API FOliveValidationEngine
{
public:
	/** Get singleton instance */
	static FOliveValidationEngine& Get();

	/**
	 * Register a validation rule
	 * @param Rule The rule to register
	 */
	void RegisterRule(TSharedPtr<IOliveValidationRule> Rule);

	/**
	 * Unregister a validation rule by name
	 * @param RuleName Name of the rule to remove
	 */
	void UnregisterRule(FName RuleName);

	/**
	 * Validate an operation
	 * @param ToolName Name of the tool being called
	 * @param Params Tool parameters
	 * @param TargetAsset Target asset (if applicable)
	 * @return Validation result
	 */
	FOliveValidationResult ValidateOperation(
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& Params,
		UObject* TargetAsset = nullptr
	);

	/**
	 * Register built-in Blueprint rules
	 */
	void RegisterBlueprintRules();

	/**
	 * Register built-in Behavior Tree rules
	 */
	void RegisterBehaviorTreeRules();

	/**
	 * Register built-in PCG rules
	 */
	void RegisterPCGRules();

	/**
	 * Register core rules (PIE protection, schema validation, etc.)
	 */
	void RegisterCoreRules();

	/**
	 * Get all registered rule names
	 */
	TArray<FName> GetRegisteredRules() const;

private:
	FOliveValidationEngine();

	TArray<TSharedPtr<IOliveValidationRule>> Rules;
	mutable FRWLock RulesLock;
};

// ============================================================================
// Built-in Validation Rules
// ============================================================================

/**
 * Rule that blocks operations during Play in Editor
 */
class OLIVEAIEDITOR_API FOlivePIEProtectionRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("PIEProtection")); }
	virtual FString GetDescription() const override { return TEXT("Blocks write operations during Play in Editor"); }
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that validates JSON schema for tool parameters
 */
class OLIVEAIEDITOR_API FOliveSchemaValidationRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("SchemaValidation")); }
	virtual FString GetDescription() const override { return TEXT("Validates parameters match expected schema"); }
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;

	/** Register a schema for a tool */
	void RegisterSchema(const FString& ToolName, const TSharedPtr<FJsonObject>& Schema);

private:
	TMap<FString, TSharedPtr<FJsonObject>> ToolSchemas;
};

/**
 * Rule that checks if target asset exists
 */
class OLIVEAIEDITOR_API FOliveAssetExistsRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("AssetExists")); }
	virtual FString GetDescription() const override { return TEXT("Validates target asset exists"); }
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

// ============================================================================
// Behavior Tree / Blackboard Validation Rules
// ============================================================================

/**
 * Rule that validates BT tools target BT assets and BB tools target BB assets
 */
class OLIVEAIEDITOR_API FOliveBTAssetTypeRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("BTAssetType")); }
	virtual FString GetDescription() const override { return TEXT("Validates correct asset type for BT/BB tools"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("behaviortree.read"), TEXT("behaviortree.set_blackboard"),
			TEXT("behaviortree.add_composite"), TEXT("behaviortree.add_task"),
			TEXT("behaviortree.add_decorator"), TEXT("behaviortree.add_service"),
			TEXT("behaviortree.remove_node"), TEXT("behaviortree.move_node"),
			TEXT("behaviortree.set_node_property"),
			TEXT("blackboard.read"), TEXT("blackboard.add_key"),
			TEXT("blackboard.remove_key"), TEXT("blackboard.modify_key"),
			TEXT("blackboard.set_parent")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that validates node_id parameters have correct format
 */
class OLIVEAIEDITOR_API FOliveBTNodeExistsRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("BTNodeExists")); }
	virtual FString GetDescription() const override { return TEXT("Validates node_id parameters have correct format"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("behaviortree.add_composite"), TEXT("behaviortree.add_task"),
			TEXT("behaviortree.add_decorator"), TEXT("behaviortree.add_service"),
			TEXT("behaviortree.remove_node"), TEXT("behaviortree.move_node"),
			TEXT("behaviortree.set_node_property")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that validates Blackboard key name is not empty for add_key
 */
class OLIVEAIEDITOR_API FOliveBBKeyUniqueRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("BBKeyUnique")); }
	virtual FString GetDescription() const override { return TEXT("Validates Blackboard key operations and parent inheritance safety"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("blackboard.add_key"),
			TEXT("blackboard.remove_key"),
			TEXT("blackboard.modify_key"),
			TEXT("blackboard.set_parent")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};
