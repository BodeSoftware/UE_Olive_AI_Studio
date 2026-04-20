// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/OliveValidationEngine.h"
#include "Brain/OliveBrainState.h"
#include "OliveToolRegistry.generated.h"

/**
 * Tool Definition
 *
 * Metadata for a registered tool including schema and tags for filtering.
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveToolDefinition
{
	GENERATED_BODY()

	/** Unique tool name (e.g., "blueprint.read", "project.search") */
	UPROPERTY()
	FString Name;

	/** Human-readable description for AI understanding */
	UPROPERTY()
	FString Description;

	/** JSON Schema for input parameters */
	TSharedPtr<FJsonObject> InputSchema;

	/** Tags for filtering by chat mode (e.g., "read", "write", "danger") */
	UPROPERTY()
	TArray<FString> Tags;

	/** Category (e.g., "blueprint", "project", "behaviortree") */
	UPROPERTY()
	FString Category;

	/** Detailed usage guidance for AI agents. Multi-line.
	 *  Appears in tools/list under annotations.usage_guidance (MCP spec extension). */
	FString UsageGuidance;

	/** When to use this tool vs alternatives. Helps AI pick the right tool.
	 *  Appears in tools/list under annotations.when_to_use. */
	FString WhenToUse;

	/** Convert to JSON for MCP tools/list */
	TSharedPtr<FJsonObject> ToMCPJson() const;
};

/**
 * Tool Execution Result
 *
 * Returned by tool handlers with success status, data, and any messages.
 */
USTRUCT()
struct OLIVEAIEDITOR_API FOliveToolResult
{
	GENERATED_BODY()

	/** Whether execution succeeded */
	UPROPERTY()
	bool bSuccess = false;

	/** Result data (tool-specific) */
	TSharedPtr<FJsonObject> Data;

	/** Validation/execution messages */
	TArray<FOliveIRMessage> Messages;

	/** Execution time in milliseconds */
	UPROPERTY()
	double ExecutionTimeMs = 0.0;

	/** Optional next-step guidance for the AI. Contextual suggestion
	 *  based on what just happened. Serialized as "next_step" in JSON output. */
	FString NextStepGuidance;

	/** Create a success result */
	static FOliveToolResult Success(const TSharedPtr<FJsonObject>& ResultData = nullptr);

	/** Create an error result */
	static FOliveToolResult Error(const FString& Code, const FString& Message, const FString& Suggestion = TEXT(""));

	/** Convert to JSON string for responses */
	FString ToJsonString() const;

	/** Convert to JSON object */
	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * Tool handler delegate signature
 *
 * Takes parameters JSON, returns result
 */
DECLARE_DELEGATE_RetVal_OneParam(FOliveToolResult, FOliveToolHandler, const TSharedPtr<FJsonObject>&);

/**
 * Tool Alias
 *
 * Maps a deprecated tool name to a new consolidated tool name with
 * an optional parameter transformation. Used for backward compatibility
 * during tool consolidation (Phase 2). Aliases do not appear in tools/list.
 */
struct FOliveToolAlias
{
	/** The new consolidated tool name to redirect to */
	FString NewToolName;

	/**
	 * Parameter transformation function.
	 * Called with a mutable clone of the original parameters to inject/rename
	 * fields required by the new consolidated tool.
	 * May be unset if only the tool name changes (pass-through).
	 */
	TFunction<void(TSharedPtr<FJsonObject>&)> TransformParams;
};

/**
 * Tool Registry
 *
 * Central registry for all available tools. Handles registration,
 * lookup, execution, and mode-based filtering (Code/Plan/Ask).
 *
 * Supports tool aliases for backward compatibility: deprecated tool names
 * are transparently redirected to their new consolidated names with
 * parameter transformation. Aliases are resolved before tool lookup
 * and do not appear in tools/list discovery.
 */
class OLIVEAIEDITOR_API FOliveToolRegistry
{
public:
	/** Get singleton instance */
	static FOliveToolRegistry& Get();

	// ==========================================
	// Registration
	// ==========================================

	/**
	 * Register a tool
	 * @param Name Unique tool name
	 * @param Description Human-readable description
	 * @param InputSchema JSON Schema for parameters
	 * @param Handler Function to execute
	 * @param Tags Tags for mode-based filtering (e.g., "read", "write", "danger")
	 * @param Category Tool category
	 */
	void RegisterTool(
		const FString& Name,
		const FString& Description,
		const TSharedPtr<FJsonObject>& InputSchema,
		FOliveToolHandler Handler,
		const TArray<FString>& Tags = {},
		const FString& Category = TEXT("")
	);

	/**
	 * Register a tool with definition struct
	 */
	void RegisterTool(const FOliveToolDefinition& Definition, FOliveToolHandler Handler);

	/**
	 * Unregister a tool (for hot reload)
	 */
	void UnregisterTool(const FString& Name);

	/**
	 * Check if a tool exists
	 */
	bool HasTool(const FString& Name) const;

	// ==========================================
	// Query
	// ==========================================

	/**
	 * Get all registered tool definitions
	 */
	TArray<FOliveToolDefinition> GetAllTools() const;

	/**
	 * Get tool definition by name
	 */
	TOptional<FOliveToolDefinition> GetTool(const FString& Name) const;

	/**
	 * Get tools filtered by chat mode.
	 * Code/Plan: returns all tools (Plan blocks writes at the pipeline, not here).
	 * Ask: returns only read/discovery tools (excludes write/danger tags).
	 * @param Mode The active chat mode
	 * @return Tools available for that mode
	 */
	TArray<FOliveToolDefinition> GetToolsForMode(EOliveChatMode Mode) const;

	/**
	 * Get tools by category
	 */
	TArray<FOliveToolDefinition> GetToolsByCategory(const FString& Category) const;

	/**
	 * Get tools by tag
	 */
	TArray<FOliveToolDefinition> GetToolsByTag(const FString& Tag) const;

	/**
	 * Get tool count
	 */
	int32 GetToolCount() const;

	// ==========================================
	// Alias Resolution
	// ==========================================

	/**
	 * Check if a tool name is a deprecated alias.
	 * @param Name Tool name to check
	 * @return true if the name is a known alias for a consolidated tool
	 */
	bool IsToolAlias(const FString& Name) const;

	/**
	 * Resolve a tool alias, transforming the tool name and parameters.
	 * If the name is not an alias, returns false and leaves parameters unchanged.
	 *
	 * @param InOutName Tool name; replaced with the new name if aliased
	 * @param InOutParams Parameters; transformed in-place if aliased
	 * @return true if an alias was resolved
	 */
	bool ResolveAlias(FString& InOutName, TSharedPtr<FJsonObject>& InOutParams) const;

	// ==========================================
	// Execution
	// ==========================================

	/**
	 * Execute a tool with parameters
	 * @param Name Tool name to execute
	 * @param Params Parameters as JSON object
	 * @return Execution result
	 */
	FOliveToolResult ExecuteTool(const FString& Name, const TSharedPtr<FJsonObject>& Params);

	/**
	 * Execute a tool asynchronously
	 * @param Name Tool name
	 * @param Params Parameters
	 * @param Callback Completion callback
	 */
	void ExecuteToolAsync(
		const FString& Name,
		const TSharedPtr<FJsonObject>& Params,
		TFunction<void(FOliveToolResult)> Callback
	);

	// ==========================================
	// MCP Format
	// ==========================================

	/**
	 * Get tools list in MCP protocol format.
	 * MCP always returns all tools (external agents are always Code mode).
	 * @return JSON object with "tools" array in MCP format
	 */
	TSharedPtr<FJsonObject> GetToolsListMCP() const;

	// ==========================================
	// Lifecycle
	// ==========================================

	/** Register all built-in tools */
	void RegisterBuiltInTools();

	/** Clear all registered tools */
	void ClearAllTools();

private:
	FOliveToolRegistry() = default;
	~FOliveToolRegistry() = default;

	// Prevent copying
	FOliveToolRegistry(const FOliveToolRegistry&) = delete;
	FOliveToolRegistry& operator=(const FOliveToolRegistry&) = delete;

	/** Tool entry with definition and handler */
	struct FToolEntry
	{
		FOliveToolDefinition Definition;
		FOliveToolHandler Handler;
	};

	/** Registered tools by name */
	TMap<FString, FToolEntry> Tools;

	/** Lock for thread-safe access */
	mutable FRWLock ToolsLock;

	// ==========================================
	// Built-in Tool Registration
	// ==========================================

	void RegisterProjectTools();

	// ==========================================
	// Built-in Tool Handlers
	// ==========================================

	// Project tools
	FOliveToolResult HandleProjectSearch(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleProjectGetAssetInfo(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleProjectGetClassHierarchy(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleProjectGetDependencies(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleProjectGetReferencers(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleProjectGetConfig(const TSharedPtr<FJsonObject>& Params);
};
