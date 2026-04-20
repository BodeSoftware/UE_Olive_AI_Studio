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
	 * Register built-in C++ rules
	 */
	void RegisterCppRules();

	/**
	 * Register Cross-System validation rules
	 */
	void RegisterCrossSystemRules();

	/**
	 * Register built-in Niagara rules
	 */
	void RegisterNiagaraRules();

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

	/** Validate a single field's type against schema */
	void ValidateType(const FString& FieldName, const TSharedPtr<FJsonValue>& Value,
		const TSharedPtr<FJsonObject>& PropertySchema, FOliveValidationResult& Result) const;

	/** Validate a value is in an enum array */
	void ValidateEnum(const FString& FieldName, const TSharedPtr<FJsonValue>& Value,
		const TArray<TSharedPtr<FJsonValue>>& EnumValues, FOliveValidationResult& Result) const;

	/** Validate a nested object against a sub-schema (one level deep) */
	void ValidateObject(const FString& Prefix, const TSharedPtr<FJsonObject>& Value,
		const TSharedPtr<FJsonObject>& Schema, FOliveValidationResult& Result) const;

	/** Check if a JSON value matches the expected type string */
	static bool IsTypeMatch(const TSharedPtr<FJsonValue>& Value, const FString& ExpectedType);
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

// ============================================================================
// Blueprint Validation Rules
// ============================================================================

/**
 * Rule that validates Blueprint asset type for BP write tools
 */
class OLIVEAIEDITOR_API FOliveBPAssetTypeRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("BPAssetType")); }
	virtual FString GetDescription() const override { return TEXT("Validates target is a Blueprint for BP write tools"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("blueprint.add_variable"), TEXT("blueprint.remove_variable"),
			TEXT("blueprint.add_component"), TEXT("blueprint.remove_component"), TEXT("blueprint.modify_component"),
			TEXT("blueprint.reparent_component"),
			TEXT("blueprint.add_function"), TEXT("blueprint.remove_function"), TEXT("blueprint.modify_function_signature"),
			TEXT("blueprint.add_node"), TEXT("blueprint.remove_node"),
			TEXT("blueprint.connect_pins"), TEXT("blueprint.disconnect_pins"),
			TEXT("blueprint.set_pin_default"), TEXT("blueprint.set_node_property"),
			TEXT("blueprint.set_parent_class"), TEXT("blueprint.add_interface"), TEXT("blueprint.remove_interface"),
			TEXT("blueprint.compile"), TEXT("blueprint.delete")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that validates node_id format for Blueprint graph operations
 */
class OLIVEAIEDITOR_API FOliveBPNodeIdFormatRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("BPNodeIdFormat")); }
	virtual FString GetDescription() const override { return TEXT("Validates node_id parameters for Blueprint graph operations"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("blueprint.remove_node"),
			TEXT("blueprint.connect_pins"), TEXT("blueprint.disconnect_pins"),
			TEXT("blueprint.set_pin_default"), TEXT("blueprint.set_node_property")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that validates variable/function name format for Blueprint operations
 */
class OLIVEAIEDITOR_API FOliveBPNamingRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("BPNaming")); }
	virtual FString GetDescription() const override { return TEXT("Validates naming conventions for Blueprint members"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("blueprint.add_variable"),
			TEXT("blueprint.add_function"), TEXT("blueprint.modify_function_signature")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

// ============================================================================
// PCG Validation Rules
// ============================================================================

/**
 * Rule that validates PCG asset type for PCG write tools
 */
class OLIVEAIEDITOR_API FOlivePCGAssetTypeRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("PCGAssetType")); }
	virtual FString GetDescription() const override { return TEXT("Validates target is a PCG graph for PCG write tools"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("pcg.add_node"), TEXT("pcg.remove_node"),
			TEXT("pcg.connect"), TEXT("pcg.disconnect"),
			TEXT("pcg.set_settings"), TEXT("pcg.add_subgraph"),
			TEXT("pcg.execute")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that validates PCG node settings class exists
 */
class OLIVEAIEDITOR_API FOlivePCGNodeClassRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("PCGNodeClass")); }
	virtual FString GetDescription() const override { return TEXT("Validates PCG settings class names are valid"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return { TEXT("pcg.add_node") };
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

// ============================================================================
// Niagara Validation Rules
// ============================================================================

/** Validates target asset is a UNiagaraSystem for Niagara write tools */
class OLIVEAIEDITOR_API FOliveNiagaraAssetTypeRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("NiagaraAssetType")); }
	virtual FString GetDescription() const override { return TEXT("Validates target is a Niagara system"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("niagara.add_emitter"), TEXT("niagara.add_module"),
			TEXT("niagara.remove_module"), TEXT("niagara.compile"),
			TEXT("niagara.set_parameter"), TEXT("niagara.set_emitter_property"),
		};
	}
	virtual FOliveValidationResult Validate(
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& Params,
		UObject* TargetAsset = nullptr) override;
};

/** Validates the stage parameter is one of the 6 valid Niagara stages */
class OLIVEAIEDITOR_API FOliveNiagaraStageValidRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("NiagaraStageValid")); }
	virtual FString GetDescription() const override { return TEXT("Validates stage is a recognized Niagara stage"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return { TEXT("niagara.add_module") };
	}
	virtual FOliveValidationResult Validate(
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& Params,
		UObject* TargetAsset = nullptr) override;
};

/** Validates the Niagara module script exists */
class OLIVEAIEDITOR_API FOliveNiagaraModuleExistsRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("NiagaraModuleExists")); }
	virtual FString GetDescription() const override { return TEXT("Validates Niagara module script can be found"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return { TEXT("niagara.add_module") };
	}
	virtual FOliveValidationResult Validate(
		const FString& ToolName,
		const TSharedPtr<FJsonObject>& Params,
		UObject* TargetAsset = nullptr) override;
};

// ============================================================================
// C++ Validation Rules
// ============================================================================

/**
 * Rule that validates file paths for C++ source operations are safe (no traversal, within project)
 */
class OLIVEAIEDITOR_API FOliveCppPathSafetyRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("CppPathSafety")); }
	virtual FString GetDescription() const override { return TEXT("Validates file paths for C++ source operations are safe"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("cpp.read_header"), TEXT("cpp.read_source"),
			TEXT("cpp.add_property"), TEXT("cpp.add_function"),
			TEXT("cpp.modify_source")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that validates referenced C++ classes exist in reflection
 */
class OLIVEAIEDITOR_API FOliveCppClassExistsRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("CppClassExists")); }
	virtual FString GetDescription() const override { return TEXT("Validates that referenced C++ classes exist in reflection"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("cpp.read_class"), TEXT("cpp.list_blueprint_callable"),
			TEXT("cpp.list_overridable")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that validates referenced C++ enums exist
 */
class OLIVEAIEDITOR_API FOliveCppEnumExistsRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("CppEnumExists")); }
	virtual FString GetDescription() const override { return TEXT("Validates that referenced C++ enums exist"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return { TEXT("cpp.read_enum") };
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that validates referenced C++ structs exist
 */
class OLIVEAIEDITOR_API FOliveCppStructExistsRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("CppStructExists")); }
	virtual FString GetDescription() const override { return TEXT("Validates that referenced C++ structs exist"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return { TEXT("cpp.read_struct") };
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that blocks C++ write operations during active compilation
 */
class OLIVEAIEDITOR_API FOliveCppCompileGuardRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("CppCompileGuard")); }
	virtual FString GetDescription() const override { return TEXT("Blocks C++ write operations during active compilation"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("cpp.create_class"), TEXT("cpp.add_property"),
			TEXT("cpp.add_function"), TEXT("cpp.modify_source"),
			TEXT("cpp.compile")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

// ============================================================================
// Cross-System Validation Rules
// ============================================================================

/**
 * Rule that limits bulk read operations to 20 assets maximum
 */
class OLIVEAIEDITOR_API FOliveBulkReadLimitRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("BulkReadLimit")); }
	virtual FString GetDescription() const override { return TEXT("Limits bulk read to 20 assets maximum"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return { TEXT("project.bulk_read") };
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that performs safety checks for refactoring operations
 */
class OLIVEAIEDITOR_API FOliveRefactorSafetyRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("RefactorSafety")); }
	virtual FString GetDescription() const override { return TEXT("Safety checks for refactoring operations"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("project.refactor_rename"),
			TEXT("project.implement_interface"),
			TEXT("project.move_to_cpp")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that rate-limits write operations to prevent runaway AI loops.
 * Uses a sliding window of timestamps to enforce MaxWriteOpsPerMinute from settings.
 * Thread-safe via FCriticalSection.
 */
class OLIVEAIEDITOR_API FOliveWriteRateLimitRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("WriteRateLimit")); }
	virtual FString GetDescription() const override { return TEXT("Rate-limits write operations per minute"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("blueprint.add_variable"), TEXT("blueprint.add_component"), TEXT("blueprint.add_function"),
			TEXT("blueprint.add_node"), TEXT("blueprint.remove_variable"), TEXT("blueprint.remove_component"),
			TEXT("blueprint.remove_function"), TEXT("blueprint.remove_node"),
			TEXT("blueprint.create"), TEXT("blueprint.set_parent_class"),
			TEXT("blueprint.add_variable"), TEXT("blueprint.modify_function"),
			TEXT("blueprint.connect_pins"), TEXT("blueprint.disconnect_pins"),
			TEXT("blueprint.set_defaults"), TEXT("blueprint.move_node"),
			TEXT("behaviortree.create"), TEXT("behaviortree.add_node"), TEXT("behaviortree.remove_node"),
			TEXT("behaviortree.modify_node"), TEXT("behaviortree.set_decorator"),
			TEXT("pcg.create"), TEXT("pcg.add_node"), TEXT("pcg.remove_node"),
			TEXT("pcg.modify_node"), TEXT("pcg.connect_pins"),
			TEXT("cpp.create_class"), TEXT("cpp.add_property"), TEXT("cpp.add_function"),
			TEXT("cpp.modify_source"), TEXT("cpp.compile"),
			TEXT("project.refactor_rename"), TEXT("project.implement_interface"),
			TEXT("project.move_to_cpp"), TEXT("project.rollback")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;

private:
	TArray<double> WriteTimestamps;
	mutable FCriticalSection TimestampLock;
};

/**
 * Rule that blocks Blueprint-asset-creation tools when preferred_layer=cpp is set.
 * Enforces C++-only mode by redirecting to C++ tool alternatives.
 */
class OLIVEAIEDITOR_API FOliveCppOnlyModeRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("CppOnlyMode")); }
	virtual FString GetDescription() const override { return TEXT("Blocks BP creation tools when preferred_layer=cpp"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("blueprint.create"),
			TEXT("blueprint.add_variable"), TEXT("blueprint.add_component"),
			TEXT("blueprint.add_function"), TEXT("blueprint.add_node"),
			TEXT("behaviortree.create"), TEXT("behaviortree.add_node"),
			TEXT("pcg.create"), TEXT("pcg.add_node")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};

/**
 * Rule that warns when creating functionality that already exists in another layer (C++ vs Blueprint).
 * Prevents duplicate implementations across layers in hybrid projects.
 */
class OLIVEAIEDITOR_API FOliveDuplicateLayerRule : public IOliveValidationRule
{
public:
	virtual FName GetRuleName() const override { return FName(TEXT("DuplicateLayer")); }
	virtual FString GetDescription() const override { return TEXT("Warns when duplicating functionality across C++ and Blueprint layers"); }
	virtual TArray<FString> GetApplicableTools() const override
	{
		return {
			TEXT("blueprint.add_function"), TEXT("blueprint.add_variable"),
			TEXT("cpp.add_function"), TEXT("cpp.add_property")
		};
	}
	virtual FOliveValidationResult Validate(const FString& ToolName, const TSharedPtr<FJsonObject>& Params, UObject* TargetAsset) override;
};
