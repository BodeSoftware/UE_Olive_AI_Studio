// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/CppIR.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveCppSource, Log, All);

/**
 * FOliveCppSourceReader
 *
 * Reads C++ source files (.h/.cpp) from the project Source/ directory.
 * Enforces path safety to prevent reading files outside the project.
 * All methods are static.
 */
class OLIVEAIEDITOR_API FOliveCppSourceReader
{
public:
	/** Maximum file size to read (2MB) */
	static constexpr int64 MaxFileSize = 2 * 1024 * 1024;

	/**
	 * Read a header file from the project Source/ directory
	 * @param FilePath Relative path within project (e.g., "MyGame/Public/MyActor.h")
	 * @param StartLine Start line (1-based, 0 = beginning)
	 * @param EndLine End line (0 = end of file)
	 * @return Source file IR or empty optional if not found/not allowed
	 */
	static TOptional<FOliveIRCppSourceFile> ReadHeader(
		const FString& FilePath,
		int32 StartLine = 0,
		int32 EndLine = 0
	);

	/**
	 * Read a source file from the project Source/ directory
	 * Same parameters as ReadHeader
	 */
	static TOptional<FOliveIRCppSourceFile> ReadSource(
		const FString& FilePath,
		int32 StartLine = 0,
		int32 EndLine = 0
	);

	/**
	 * List C++ classes found in the project Source/ directory
	 * Scans .h files for UCLASS() macros
	 * @param ModuleFilter Only classes in this module (empty = all)
	 * @param ParentClassFilter Only classes inheriting from this (empty = all, checked via UCLASS macro regex)
	 * @return Array of class info (name, header path, parent if detectable)
	 */
	static TArray<FOliveIRCppSourceFile> ListProjectClasses(
		const FString& ModuleFilter = TEXT(""),
		const FString& ParentClassFilter = TEXT("")
	);

	/**
	 * Check if a path is safe to read (within project Source/ directory)
	 */
	static bool IsPathSafe(const FString& FilePath);

	/**
	 * Get the absolute project Source/ directory path
	 */
	static FString GetProjectSourceDir();

private:
	/**
	 * Read a file with line range support
	 * @param AbsolutePath Full path to the file
	 * @param RelativePath Relative path for the IR result
	 * @param StartLine Start line (1-based, 0 = beginning)
	 * @param EndLine End line (0 = end of file)
	 * @return Source file IR
	 */
	static TOptional<FOliveIRCppSourceFile> ReadFileWithRange(
		const FString& AbsolutePath,
		const FString& RelativePath,
		int32 StartLine,
		int32 EndLine
	);

	/**
	 * Resolve a relative path to an absolute path within the project
	 * Handles both "Source/ModuleName/..." and "ModuleName/..." formats
	 */
	static FString ResolveFilePath(const FString& RelativePath);

	/**
	 * Normalize and validate a path (no "..", no absolute paths)
	 */
	static bool ValidatePath(const FString& Path, FString& OutError);
};
