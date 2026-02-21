// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveCppWriter, Log, All);

/**
 * FOliveCppSourceWriter
 *
 * Creates and modifies C++ source files for UE classes.
 * Uses text-based insertion (no AST parsing). Backs up files
 * before modification.
 */
class OLIVEAIEDITOR_API FOliveCppSourceWriter
{
public:
	/**
	 * Create a new UE class with header and source files
	 * @param ClassName Class name without prefix (e.g., "MyActor") - prefix is auto-determined from parent
	 * @param ParentClass Parent class (e.g., "AActor", "UActorComponent")
	 * @param ModuleName Module to create in (determines API macro)
	 * @param SubPath Optional subdirectory within module Source/ (e.g., "Characters")
	 * @param Interfaces Optional interfaces to implement
	 * @return Result with created file paths
	 */
	static FOliveToolResult CreateClass(
		const FString& ClassName,
		const FString& ParentClass,
		const FString& ModuleName,
		const FString& SubPath = TEXT(""),
		const TArray<FString>& Interfaces = {}
	);

	/**
	 * Add a UPROPERTY to an existing header file
	 * @param FilePath Relative path to the .h file
	 * @param PropertyName Property name
	 * @param PropertyType C++ type
	 * @param Category UPROPERTY Category (empty for none)
	 * @param Specifiers UPROPERTY specifiers (e.g., {"EditAnywhere", "BlueprintReadWrite"})
	 * @param DefaultValue Optional default value
	 */
	static FOliveToolResult AddProperty(
		const FString& FilePath,
		const FString& PropertyName,
		const FString& PropertyType,
		const FString& Category = TEXT(""),
		const TArray<FString>& Specifiers = {},
		const FString& DefaultValue = TEXT("")
	);

	/**
	 * Add a UFUNCTION declaration to header and stub body to source
	 * @param FilePath Relative path to .h file
	 * @param FunctionName Function name
	 * @param ReturnType Return type (empty for void)
	 * @param Parameters Array of {name, type} pairs
	 * @param Specifiers UFUNCTION specifiers
	 * @param bIsVirtual Whether function is virtual
	 * @param Body Optional function body (empty for default stub)
	 */
	static FOliveToolResult AddFunction(
		const FString& FilePath,
		const FString& FunctionName,
		const FString& ReturnType = TEXT(""),
		const TArray<TPair<FString, FString>>& Parameters = {},
		const TArray<FString>& Specifiers = {},
		bool bIsVirtual = false,
		const FString& Body = TEXT("")
	);

	/**
	 * Trigger Live Coding / hot reload compilation
	 */
	static FOliveToolResult TriggerCompile();

	/**
	 * Modify source text using bounded anchor-based patching.
	 * @param FilePath Relative path to .h/.cpp/.inl file under Source/
	 * @param AnchorText Exact text to match
	 * @param Operation One of: replace, insert_before, insert_after
	 * @param ReplacementText Text to insert or replacement body
	 * @param Occurrence 1-based occurrence index when multiple anchors exist
	 * @param StartLine Optional 1-based start guard (0 = no guard)
	 * @param EndLine Optional 1-based end guard (0 = no guard)
	 * @param bRequireUniqueMatch If true, fail when anchor appears more than once
	 */
	static FOliveToolResult ModifySource(
		const FString& FilePath,
		const FString& AnchorText,
		const FString& Operation,
		const FString& ReplacementText,
		int32 Occurrence = 1,
		int32 StartLine = 0,
		int32 EndLine = 0,
		bool bRequireUniqueMatch = true
	);

private:
	/**
	 * Back up a file before modification
	 * Saves to {Project}/Saved/OliveAI/Backups/{timestamp}_{filename}
	 */
	static bool BackupFile(const FString& AbsolutePath, FString* OutBackupPath = nullptr);

	/**
	 * Get the backup directory path
	 */
	static FString GetBackupDir();

	/**
	 * Determine the class prefix from parent class name
	 * AActor-derived -> A, UObject-derived -> U, FStruct -> F
	 */
	static FString DetermineClassPrefix(const FString& ParentClass);

	/**
	 * Determine API macro from module name (e.g., "MyGame" -> "MYGAME_API")
	 */
	static FString GetAPIMacro(const FString& ModuleName);

	/**
	 * Find the insertion point for a new UPROPERTY in a header file
	 * Looks for the last UPROPERTY() block or the end of the public: section
	 * Returns line index (0-based) where new property should be inserted
	 */
	static int32 FindPropertyInsertionPoint(const TArray<FString>& Lines);

	/**
	 * Find the insertion point for a new UFUNCTION declaration
	 * Returns line index where new function should be inserted
	 */
	static int32 FindFunctionInsertionPoint(const TArray<FString>& Lines);

	/**
	 * Find the corresponding .cpp file for a .h file
	 */
	static FString FindCorrespondingSourceFile(const FString& HeaderPath);

	/**
	 * Generate header file content for a new class
	 */
	static FString GenerateHeaderContent(
		const FString& FullClassName,
		const FString& ParentClass,
		const FString& ModuleName,
		const TArray<FString>& Interfaces
	);

	/**
	 * Generate source file content for a new class
	 */
	static FString GenerateSourceContent(
		const FString& FullClassName,
		const FString& HeaderRelativePath
	);

	/**
	 * Resolve a relative file path to an absolute path within project Source/
	 * Handles both "Source/ModuleName/..." and "ModuleName/..." formats
	 */
	static FString ResolveFilePath(const FString& RelativePath);

	/**
	 * Extract the class name from a header file's content
	 * Uses regex to find: class [MODULE_API] ClassName
	 */
	static FString ExtractClassNameFromHeader(const TArray<FString>& Lines);
};
