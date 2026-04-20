// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Services/OliveValidationEngine.h"
#include "Services/OliveTransactionManager.h"
#include "Settings/OliveAISettings.h"
#include "Brain/OliveBrainState.h"
#include "IR/OliveCompileIR.h"
#include "MCP/OliveToolRegistry.h"
#include "Dom/JsonObject.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveWritePipeline, Log, All);

/**
 * Stage of the write pipeline
 */
enum class EOliveWriteStage : uint8
{
	Validate,       // Stage 1: Input validation and precondition checks
	ModeGate,       // Stage 2: Chat mode gate (Ask blocks all, Plan blocks writes, Code passes through)
	Transact,       // Stage 3: Open transaction and mark objects dirty
	Execute,        // Stage 4: Perform the mutation
	Verify,         // Stage 5: Structural checks and optional compilation
	Report          // Stage 6: Generate structured result
};

// Forward declarations
class UBlueprint;
class UEdGraph;
struct FOliveWriteRequest;
struct FOliveWriteResult;

/**
 * Write operation request - input to the pipeline
 */
struct OLIVEAIEDITOR_API FOliveWriteRequest
{
	/** Tool name for logging and mode gate checks */
	FString ToolName;

	/** Tool parameters as JSON */
	TSharedPtr<FJsonObject> Params;

	/** Target asset path (resolved before pipeline) */
	FString AssetPath;

	/** Target asset (loaded before pipeline, may be null for create operations) */
	UObject* TargetAsset = nullptr;

	/** Operation description for transaction naming */
	FText OperationDescription;

	/** Operation category for logging */
	FString OperationCategory;

	/** Whether this request came from MCP or built-in chat.
	 *  NOTE: This no longer bypasses the mode gate. ChatMode is the single source
	 *  of truth for mode gating. External MCP agents get ChatMode=Code by default;
	 *  in-engine autonomous agents inherit the user's mode via MCP server propagation. */
	bool bFromMCP = false;

	/** Whether to auto-compile after write (from settings) */
	bool bAutoCompile = true;

	/** Whether to skip verification (for batch operations) */
	bool bSkipVerification = false;

	/** Current chat mode -- controls Stage 2 (Mode Gate) behavior.
	 *  For built-in chat: set directly by ConversationManager.
	 *  For MCP calls: propagated from FOliveToolCallContext by ExecuteWithOptionalConfirmation.
	 *  External MCP agents default to Code; in-engine autonomous agents inherit the user's mode. */
	EOliveChatMode ChatMode = EOliveChatMode::Code;
};

/**
 * Write operation result - output from the pipeline
 */
struct OLIVEAIEDITOR_API FOliveWriteResult
{
	/** Overall success status */
	bool bSuccess = false;

	/** Which stage completed last (for debugging) */
	EOliveWriteStage CompletedStage = EOliveWriteStage::Validate;

	/** Validation messages */
	TArray<FOliveIRMessage> ValidationMessages;

	/** Operation result data */
	TSharedPtr<FJsonObject> ResultData;

	/** Compile result (if verification included compilation) */
	TOptional<FOliveIRCompileResult> CompileResult;

	/** Execution time in milliseconds */
	double ExecutionTimeMs = 0.0;

	/** Created item name/ID (for creation operations) */
	FString CreatedItem;

	/** Created node IDs (for graph operations) */
	TArray<FString> CreatedNodeIds;

	/** Optional next-step guidance for the AI. Flows through to FOliveToolResult. */
	FString NextStepGuidance;

	/**
	 * Convert to tool result format
	 * @return Tool result suitable for MCP response
	 */
	FOliveToolResult ToToolResult() const;

	/**
	 * Create validation error result
	 * @param Result Validation result containing errors
	 * @return Write result with validation errors
	 */
	static FOliveWriteResult ValidationError(const FOliveValidationResult& Result);

	/**
	 * Create execution error result
	 * @param Code Error code
	 * @param Message Error message
	 * @param Suggestion Optional suggestion for fixing
	 * @return Write result with execution error
	 */
	static FOliveWriteResult ExecutionError(const FString& Code, const FString& Message, const FString& Suggestion = TEXT(""));

	/**
	 * Create success result
	 * @param Data Optional result data
	 * @return Successful write result
	 */
	static FOliveWriteResult Success(const TSharedPtr<FJsonObject>& Data = nullptr);
};

/**
 * Delegate for the Execute stage - provided by each tool handler
 * @param Request The write request
 * @param TargetAsset The target asset (may be modified during execution)
 * @return Write result from the execution
 */
DECLARE_DELEGATE_RetVal_TwoParams(FOliveWriteResult, FOliveWriteExecutor, const FOliveWriteRequest&, UObject*);

/**
 * Write Pipeline Service
 *
 * Orchestrates the 6-stage write safety pipeline:
 * 1. Validate  - Input validation, schema checking, precondition verification
 * 2. Mode Gate - Chat mode check (Ask blocks all writes, Plan blocks writes except preview, Code passes through)
 * 3. Transact  - Open FScopedTransaction, call Modify() on target objects
 * 4. Execute   - Run the actual mutation via the provided executor delegate
 * 5. Verify    - Structural checks, optional compilation, consistency validation
 * 6. Report    - Assemble structured result with timing and diagnostics
 *
 * MCP clients (bFromMCP=true) skip Stage 2 entirely -- external agents are always Code mode.
 * Built-in chat clients have their mode set on FOliveWriteRequest.ChatMode by the ConversationManager.
 *
 * Thread Safety: All methods are game thread only (UE API requirement)
 */
class OLIVEAIEDITOR_API FOliveWritePipeline
{
public:
	/** Get singleton instance */
	static FOliveWritePipeline& Get();

	/**
	 * Execute a write operation through the full pipeline
	 * @param Request The write request with all parameters (includes ChatMode for Stage 2)
	 * @param Executor Delegate that performs the actual mutation
	 * @return Write result indicating success or failure
	 */
	FOliveWriteResult Execute(const FOliveWriteRequest& Request, FOliveWriteExecutor Executor);

	// ============================================================================
	// Shared Verification Helpers
	// ============================================================================

	/**
	 * Detect orphaned execution flow in a graph.
	 * An orphaned exec flow is an exec output pin that has no connections,
	 * where the node itself has at least one connected exec input
	 * (meaning execution reaches this node but cannot continue).
	 *
	 * Skips pure nodes, FunctionResult/return nodes, reroute nodes, comment nodes,
	 * Sequence nodes with at least one connected output, and custom events.
	 *
	 * @param Graph The graph to analyze
	 * @param OutMessages Warning messages for each orphaned flow detected
	 * @return Number of orphaned exec flows found
	 */
	int32 DetectOrphanedExecFlows(const UEdGraph* Graph, TArray<FOliveIRMessage>& OutMessages) const;

	/**
	 * Detect data input pins that are required but have no connection and no default.
	 * Skips: exec pins, hidden pins, self/WorldContextObject pins, pins with non-empty
	 * DefaultValue or AutogeneratedDefaultValue, pins on comment/reroute nodes.
	 *
	 * @param Graph Graph to check (all nodes unless NodesToCheck is non-empty)
	 * @param NodesToCheck If non-empty, only check these nodes (for scoped checks)
	 * @param OutMessages Warning messages with pin details
	 * @return Count of unwired required pins
	 */
	static int32 DetectUnwiredRequiredDataPins(
		const UEdGraph* Graph,
		const TSet<UEdGraphNode*>& NodesToCheck,
		TArray<FString>& OutMessages);

private:
	FOliveWritePipeline() = default;

	// Non-copyable
	FOliveWritePipeline(const FOliveWritePipeline&) = delete;
	FOliveWritePipeline& operator=(const FOliveWritePipeline&) = delete;

	// ============================================================================
	// Pipeline Stages
	// ============================================================================

	/**
	 * Stage 1: Validate inputs and preconditions
	 * @param Request The write request
	 * @return Validation result (success or errors)
	 */
	FOliveWriteResult StageValidate(const FOliveWriteRequest& Request);

	/**
	 * Stage 2: Mode gate -- checks Request.ChatMode to allow or block the write.
	 *
	 * Logic:
	 *   - Ask mode: blocks all writes with ASK_MODE error
	 *   - Plan mode: allows blueprint.preview_plan_json, blocks all other writes with PLAN_MODE error
	 *   - Code mode: passes through (destructive ops like delete/reparent noted for future prompt UX)
	 *
	 * Only called for non-MCP requests (bFromMCP=false). MCP always bypasses.
	 *
	 * @param Request The write request (carries ChatMode)
	 * @return Set result if mode blocks the operation; empty optional if pass-through
	 */
	TOptional<FOliveWriteResult> StageModeGate(const FOliveWriteRequest& Request);

	/**
	 * Stage 3: Open transaction (returns transaction wrapper)
	 * @param Request The write request
	 * @param TargetAsset Target asset to modify
	 * @return Scoped transaction wrapper
	 */
	TUniquePtr<FOliveTransactionManager::FScopedOliveTransaction> StageTransact(
		const FOliveWriteRequest& Request,
		UObject* TargetAsset);

	/**
	 * Stage 4: Execute the mutation
	 * @param Request The write request
	 * @param TargetAsset Target asset to modify
	 * @param Executor Delegate to execute the mutation
	 * @return Execution result
	 */
	FOliveWriteResult StageExecute(
		const FOliveWriteRequest& Request,
		UObject* TargetAsset,
		FOliveWriteExecutor& Executor,
		UObject*& OutEffectiveTargetAsset);

	/**
	 * Stage 5: Verify result (structural checks + optional compile)
	 * @param Request The write request
	 * @param TargetAsset Target asset that was modified
	 * @param ExecuteResult Result from execution stage
	 * @return Verification result
	 */
	FOliveWriteResult StageVerify(
		const FOliveWriteRequest& Request,
		UObject* TargetAsset,
		const FOliveWriteResult& ExecuteResult);

	/**
	 * Stage 6: Assemble final report
	 * @param Request The write request
	 * @param VerifyResult Result from verification stage
	 * @param TotalTimeMs Total execution time
	 * @return Final report result
	 */
	FOliveWriteResult StageReport(
		const FOliveWriteRequest& Request,
		const FOliveWriteResult& VerifyResult,
		double TotalTimeMs);

	// ============================================================================
	// Verification
	// ============================================================================

	/**
	 * Run structural verification on a Blueprint
	 * @param Blueprint Blueprint to verify
	 * @param OutMessages Validation messages (output)
	 * @return True if structure is valid
	 */
	bool VerifyBlueprintStructure(UBlueprint* Blueprint, TArray<FOliveIRMessage>& OutMessages) const;

	/**
	 * Compile and gather errors
	 * @param Blueprint Blueprint to compile
	 * @return Compile result with errors/warnings
	 */
	FOliveIRCompileResult CompileAndGatherErrors(UBlueprint* Blueprint) const;

	/**
	 * Check whether a tool name represents a graph-editing operation.
	 * Returns true for tool names starting with blueprint.add_node,
	 * blueprint.remove_node, blueprint.connect_pins, or blueprint.disconnect_pins.
	 *
	 * @param ToolName The tool name to check
	 * @return True if this is a graph-editing tool
	 */
	bool IsGraphEditOperation(const FString& ToolName) const;

	// ============================================================================
	// Orphan Detection Baseline (per-run delta tracking)
	// ============================================================================
public:

	/**
	 * Capture the current orphan count for a graph as the baseline.
	 * Called lazily on first graph-edit check per run, NOT eagerly on run start.
	 * Idempotent: does nothing if a baseline already exists for this graph.
	 *
	 * @param GraphPath Unique graph identifier (Blueprint path + graph name)
	 * @param CurrentOrphanCount The current absolute orphan count
	 */
	void SetOrphanBaseline(const FString& GraphPath, int32 CurrentOrphanCount);

	/**
	 * Get the number of NEW orphans since the baseline was captured.
	 * Returns the absolute count if no baseline exists (first check).
	 *
	 * @param GraphPath Unique graph identifier
	 * @param CurrentOrphanCount The current absolute orphan count
	 * @return Delta (new orphans since baseline), minimum 0
	 */
	int32 GetOrphanDelta(const FString& GraphPath, int32 CurrentOrphanCount) const;

	/** Clear all orphan baselines (called at run start to reset previous run data) */
	void ClearOrphanBaselines();

	/** Per-run orphan baselines: graph path -> orphan count at start of run */
	TMap<FString, int32> OrphanBaselines;

	/** Whether a brain run is currently active (baselines are valid) */
	bool bRunActive = false;
};
