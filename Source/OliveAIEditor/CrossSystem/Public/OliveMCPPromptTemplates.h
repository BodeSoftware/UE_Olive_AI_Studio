// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePromptTemplates, Log, All);

/**
 * A prompt template parameter definition
 */
struct OLIVEAIEDITOR_API FOlivePromptParam
{
	FString Name;
	FString Description;
	bool bRequired = true;

	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * A prompt template definition
 */
struct OLIVEAIEDITOR_API FOlivePromptTemplate
{
	FString Name;           // Unique identifier (e.g., "explain_blueprint")
	FString Description;    // Human-readable description
	TArray<FOlivePromptParam> Parameters;
	FString TemplateText;   // Template with {{param_name}} placeholders

	TSharedPtr<FJsonObject> ToMCPJson() const;
};

/**
 * FOliveMCPPromptTemplates
 *
 * Manages MCP prompt templates that provide structured prompts
 * for common development tasks. These are exposed via the MCP
 * prompts/list and prompts/get protocol methods.
 */
class OLIVEAIEDITOR_API FOliveMCPPromptTemplates
{
public:
	static FOliveMCPPromptTemplates& Get();

	/** Initialize with default templates */
	void Initialize();

	/** Get all template definitions for prompts/list */
	TSharedPtr<FJsonObject> GetPromptsList() const;

	/** Get a specific prompt with arguments filled in for prompts/get */
	TSharedPtr<FJsonObject> GetPrompt(const FString& Name, const TSharedPtr<FJsonObject>& Arguments) const;

	/** Check if a template exists */
	bool HasTemplate(const FString& Name) const;

	/** Register a custom template */
	void RegisterTemplate(const FOlivePromptTemplate& Template);

private:
	FOliveMCPPromptTemplates() = default;

	void RegisterDefaultTemplates();

	/** Apply arguments to a template, replacing {{param}} placeholders */
	FString ApplyArguments(const FString& TemplateText, const TSharedPtr<FJsonObject>& Arguments) const;

	TMap<FString, FOlivePromptTemplate> Templates;
};
