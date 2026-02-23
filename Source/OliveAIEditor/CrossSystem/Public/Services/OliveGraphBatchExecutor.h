// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Forward declarations
struct FOliveBlueprintWriteResult;
class FJsonObject;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveGraphBatchExecutor, Log, All);

/**
 * FOliveGraphBatchExecutor
 *
 * Shared utility for batch graph write operations. Provides the core dispatch,
 * template resolution, and allowlist logic used by project.batch_write and
 * blueprint.apply_plan_json.
 *
 * Extracted from OliveCrossSystemToolHandlers.cpp to allow reuse without
 * duplicating logic.
 */
class OLIVEAIEDITOR_API FOliveGraphBatchExecutor
{
public:
	/**
	 * Dispatch a single write operation to the appropriate FOliveGraphWriter method.
	 *
	 * @param ToolName    The tool name (e.g., "blueprint.add_node", "blueprint.connect_pins")
	 * @param BlueprintPath  Full asset path to the target Blueprint
	 * @param OpParams    Parameters for the operation
	 * @return Write result from the underlying writer method
	 */
	static FOliveBlueprintWriteResult DispatchWriterOp(
		const FString& ToolName,
		const FString& BlueprintPath,
		const TSharedPtr<FJsonObject>& OpParams);

	/**
	 * Walk all string values in OpParams and resolve ${opId.field} references
	 * from previously stored op results.
	 *
	 * @param OpParams      The op params JSON to mutate in-place
	 * @param OpResultsById Map of opId -> result data JSON from prior ops
	 * @param OutError      Set on failure
	 * @return true if all templates resolved successfully
	 */
	static bool ResolveTemplateReferences(
		TSharedPtr<FJsonObject>& OpParams,
		const TMap<FString, TSharedPtr<FJsonObject>>& OpResultsById,
		FString& OutError);

	/**
	 * Get the set of tool names allowed in batch write operations.
	 *
	 * @return Const reference to the static allowlist set
	 */
	static const TSet<FString>& GetBatchWriteAllowlist();
};
