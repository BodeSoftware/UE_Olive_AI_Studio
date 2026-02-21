// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "IR/OliveCompileIR.h"

/** Type of result card to display */
enum class EOliveResultCardType : uint8
{
	BlueprintReadSummary,
	BlueprintWriteResult,
	CompileErrors,
	SnapshotInfo,
	NavigationAction,
	RawJson
};

/** A navigation action that can be triggered from a result card */
struct FOliveNavigationAction
{
	FString Label;
	FString AssetPath;
	FString GraphName;
	TArray<FString> NodeIds;
	FString SnapshotId;
	bool bIsCompileError = false;
	FOliveIRCompileError CompileError;
};

/**
 * FOliveResultCardData
 *
 * Typed result card data parsed from tool results. Contains all information
 * needed to render a rich result card in the chat UI, replacing raw JSON.
 */
struct OLIVEAIEDITOR_API FOliveResultCardData
{
	EOliveResultCardType CardType = EOliveResultCardType::RawJson;
	FString Title;
	FString Subtitle;
	bool bSuccess = true;
	TArray<FOliveNavigationAction> Actions;
	TSharedPtr<FJsonObject> RawJson;

	// BlueprintReadSummary
	FString AssetPath;
	int32 VariableCount = 0;
	int32 FunctionCount = 0;
	int32 GraphCount = 0;
	int32 ComponentCount = 0;
	FString ParentClass;

	// BlueprintWriteResult
	FString OperationDescription;
	TArray<FString> CreatedNodeIds;
	FString CreatedItemName;
	double ExecutionTimeMs = 0.0;

	// CompileErrors
	TArray<FOliveIRCompileError> Errors;
	TArray<FOliveIRCompileError> Warnings;

	// SnapshotInfo
	FString SnapshotId;
	FString SnapshotName;
	int32 AssetCount = 0;
	TArray<FString> ChangedAssets;

	/** Parse tool result JSON into a typed card */
	static FOliveResultCardData FromToolResult(const FString& ToolName, const TSharedPtr<FJsonObject>& ResultJson);
};
