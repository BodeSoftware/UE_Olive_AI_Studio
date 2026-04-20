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

/** Completion stage marker (kept as an enum for back-compat with existing callers). */
enum class EOliveWriteStage : uint8
{
	Validate,
	Transact,
	Execute,
	Verify,
	Report
};

class UBlueprint;
class UEdGraph;
struct FOliveWriteRequest;
struct FOliveWriteResult;

/** Write operation request — input to the minimal pipeline. */
struct OLIVEAIEDITOR_API FOliveWriteRequest
{
	/** Tool name for logging */
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

	/** Whether this request came from MCP or built-in chat. Retained for logging only. */
	bool bFromMCP = false;

	/** Whether to auto-compile after write */
	bool bAutoCompile = true;

	/** Whether to skip verification (for batch operations) */
	bool bSkipVerification = false;
};

/** Write operation result — output from the pipeline. */
struct OLIVEAIEDITOR_API FOliveWriteResult
{
	bool bSuccess = false;
	EOliveWriteStage CompletedStage = EOliveWriteStage::Validate;
	TArray<FOliveIRMessage> ValidationMessages;
	TSharedPtr<FJsonObject> ResultData;
	TOptional<FOliveIRCompileResult> CompileResult;
	double ExecutionTimeMs = 0.0;
	FString CreatedItem;
	TArray<FString> CreatedNodeIds;
	FString NextStepGuidance;

	FOliveToolResult ToToolResult() const;

	static FOliveWriteResult ValidationError(const FOliveValidationResult& Result);
	static FOliveWriteResult ExecutionError(const FString& Code, const FString& Message, const FString& Suggestion = TEXT(""));
	static FOliveWriteResult Success(const TSharedPtr<FJsonObject>& Data = nullptr);
};

/**
 * Delegate for the Execute step — provided by each tool handler.
 * Takes the request + loaded target asset and returns the write result.
 */
DECLARE_DELEGATE_RetVal_TwoParams(FOliveWriteResult, FOliveWriteExecutor, const FOliveWriteRequest&, UObject*);

/**
 * Minimal Write Pipeline.
 *
 * Flow: load (if needed) → FScopedTransaction → Executor → optional compile → result.
 *
 * No multi-stage pipeline, no mode gate, no orphan detection, no snapshot, no
 * self-correction — each handler's Executor lambda is the only thing that runs.
 * Compatible with the existing FOliveWriteRequest / FOliveWriteResult / FOliveWriteExecutor
 * handler API so call sites do not need to change.
 *
 * Thread Safety: game thread only (UE API requirement).
 */
class OLIVEAIEDITOR_API FOliveWritePipeline
{
public:
	static FOliveWritePipeline& Get();

	/** Execute a write operation. */
	FOliveWriteResult Execute(const FOliveWriteRequest& Request, FOliveWriteExecutor Executor);

	/** Kept for back-compat; no-op now that snapshot-based orphan baselines are gone. */
	void ClearOrphanBaselines() {}

	/** Kept for back-compat; callers may set it but nothing reads it. */
	bool bRunActive = false;

private:
	FOliveWritePipeline() = default;
	FOliveWritePipeline(const FOliveWritePipeline&) = delete;
	FOliveWritePipeline& operator=(const FOliveWritePipeline&) = delete;
};
