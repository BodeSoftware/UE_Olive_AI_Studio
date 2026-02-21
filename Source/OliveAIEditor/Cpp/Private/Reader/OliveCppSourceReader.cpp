// Copyright Bode Software. All Rights Reserved.

#include "Reader/OliveCppSourceReader.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Internationalization/Regex.h"

DEFINE_LOG_CATEGORY(LogOliveCppSource);

// ----------------------------------------------------------------------------
// Path Safety
// ----------------------------------------------------------------------------

FString FOliveCppSourceReader::GetProjectSourceDir()
{
	return FPaths::ProjectDir() / TEXT("Source");
}

bool FOliveCppSourceReader::IsPathSafe(const FString& FilePath)
{
	// Reject absolute paths
	if (!FPaths::IsRelative(FilePath))
	{
		return false;
	}

	// Reject path traversal
	if (FilePath.Contains(TEXT("..")) || FilePath.Contains(TEXT("~")))
	{
		return false;
	}

	// Must be .h, .cpp, or .inl
	FString Extension = FPaths::GetExtension(FilePath, false).ToLower();
	if (Extension != TEXT("h") && Extension != TEXT("cpp") && Extension != TEXT("inl"))
	{
		return false;
	}

	return true;
}

bool FOliveCppSourceReader::ValidatePath(const FString& Path, FString& OutError)
{
	if (Path.IsEmpty())
	{
		OutError = TEXT("File path cannot be empty");
		return false;
	}

	if (!IsPathSafe(Path))
	{
		OutError = FString::Printf(TEXT("Path '%s' is not safe. Must be relative, no '..', and must be .h/.cpp/.inl"), *Path);
		return false;
	}

	return true;
}

FString FOliveCppSourceReader::ResolveFilePath(const FString& RelativePath)
{
	FString SourceDir = GetProjectSourceDir();

	// If path already starts with "Source/", strip it to avoid double Source/Source/
	FString CleanPath = RelativePath;
	if (CleanPath.StartsWith(TEXT("Source/")) || CleanPath.StartsWith(TEXT("Source\\")))
	{
		CleanPath = CleanPath.RightChop(7);
	}

	FString AbsolutePath = SourceDir / CleanPath;
	FPaths::NormalizeFilename(AbsolutePath);

	// Final safety check: resolved path must still be under Source/
	FString NormalizedSourceDir = SourceDir;
	FPaths::NormalizeFilename(NormalizedSourceDir);
	if (!AbsolutePath.StartsWith(NormalizedSourceDir))
	{
		UE_LOG(LogOliveCppSource, Warning, TEXT("ResolveFilePath: Path escaped the sandbox: %s"), *AbsolutePath);
		return FString();
	}

	return AbsolutePath;
}

// ----------------------------------------------------------------------------
// File Reading
// ----------------------------------------------------------------------------

TOptional<FOliveIRCppSourceFile> FOliveCppSourceReader::ReadFileWithRange(
	const FString& AbsolutePath,
	const FString& RelativePath,
	int32 StartLine,
	int32 EndLine)
{
	// Check file exists
	if (!FPaths::FileExists(AbsolutePath))
	{
		UE_LOG(LogOliveCppSource, Warning, TEXT("File not found: %s"), *AbsolutePath);
		return TOptional<FOliveIRCppSourceFile>();
	}

	// Check file size
	int64 FileSize = IFileManager::Get().FileSize(*AbsolutePath);
	if (FileSize > MaxFileSize)
	{
		UE_LOG(LogOliveCppSource, Warning, TEXT("File too large (%lld bytes, max %lld): %s"), FileSize, MaxFileSize, *AbsolutePath);
		return TOptional<FOliveIRCppSourceFile>();
	}

	// Read entire file
	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *AbsolutePath))
	{
		UE_LOG(LogOliveCppSource, Error, TEXT("Failed to read file: %s"), *AbsolutePath);
		return TOptional<FOliveIRCppSourceFile>();
	}

	// Split into lines
	TArray<FString> Lines;
	FileContent.ParseIntoArrayLines(Lines);

	FOliveIRCppSourceFile Result;
	Result.FilePath = RelativePath;
	Result.TotalLines = Lines.Num();

	// Apply line range (StartLine is 1-based, 0 means beginning)
	int32 ActualStart = (StartLine > 0) ? FMath::Clamp(StartLine - 1, 0, Lines.Num()) : 0;
	int32 ActualEnd = (EndLine > 0) ? FMath::Clamp(EndLine, 0, Lines.Num()) : Lines.Num();

	if (ActualStart >= ActualEnd)
	{
		Result.Content = TEXT("");
		Result.StartLine = StartLine;
		Result.EndLine = EndLine;
		return Result;
	}

	// Build content from selected lines
	FString SelectedContent;
	for (int32 i = ActualStart; i < ActualEnd; ++i)
	{
		if (i > ActualStart)
		{
			SelectedContent += TEXT("\n");
		}
		SelectedContent += Lines[i];
	}

	Result.Content = SelectedContent;
	Result.StartLine = ActualStart + 1; // Convert back to 1-based
	Result.EndLine = ActualEnd;
	Result.bIsTruncated = (ActualStart > 0 || ActualEnd < Lines.Num());

	return Result;
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

TOptional<FOliveIRCppSourceFile> FOliveCppSourceReader::ReadHeader(
	const FString& FilePath, int32 StartLine, int32 EndLine)
{
	FString Error;
	if (!ValidatePath(FilePath, Error))
	{
		UE_LOG(LogOliveCppSource, Warning, TEXT("ReadHeader: %s"), *Error);
		return TOptional<FOliveIRCppSourceFile>();
	}

	// Ensure it's a header file
	FString Ext = FPaths::GetExtension(FilePath, false).ToLower();
	if (Ext != TEXT("h") && Ext != TEXT("inl"))
	{
		UE_LOG(LogOliveCppSource, Warning, TEXT("ReadHeader: Expected .h or .inl file, got .%s"), *Ext);
		return TOptional<FOliveIRCppSourceFile>();
	}

	FString AbsPath = ResolveFilePath(FilePath);
	if (AbsPath.IsEmpty())
	{
		UE_LOG(LogOliveCppSource, Warning, TEXT("ReadHeader: Path resolved outside project Source/"));
		return TOptional<FOliveIRCppSourceFile>();
	}

	return ReadFileWithRange(AbsPath, FilePath, StartLine, EndLine);
}

TOptional<FOliveIRCppSourceFile> FOliveCppSourceReader::ReadSource(
	const FString& FilePath, int32 StartLine, int32 EndLine)
{
	FString Error;
	if (!ValidatePath(FilePath, Error))
	{
		UE_LOG(LogOliveCppSource, Warning, TEXT("ReadSource: %s"), *Error);
		return TOptional<FOliveIRCppSourceFile>();
	}

	FString Ext = FPaths::GetExtension(FilePath, false).ToLower();
	if (Ext != TEXT("cpp"))
	{
		UE_LOG(LogOliveCppSource, Warning, TEXT("ReadSource: Expected .cpp file, got .%s"), *Ext);
		return TOptional<FOliveIRCppSourceFile>();
	}

	FString AbsPath = ResolveFilePath(FilePath);
	if (AbsPath.IsEmpty())
	{
		UE_LOG(LogOliveCppSource, Warning, TEXT("ReadSource: Path resolved outside project Source/"));
		return TOptional<FOliveIRCppSourceFile>();
	}

	return ReadFileWithRange(AbsPath, FilePath, StartLine, EndLine);
}

// ----------------------------------------------------------------------------
// Class Listing
// ----------------------------------------------------------------------------

TArray<FOliveIRCppSourceFile> FOliveCppSourceReader::ListProjectClasses(
	const FString& ModuleFilter, const FString& ParentClassFilter)
{
	TArray<FOliveIRCppSourceFile> Results;
	FString SourceDir = GetProjectSourceDir();

	if (!FPaths::DirectoryExists(SourceDir))
	{
		UE_LOG(LogOliveCppSource, Warning, TEXT("Project Source/ directory not found: %s"), *SourceDir);
		return Results;
	}

	// Find all .h files recursively
	TArray<FString> HeaderFiles;
	IFileManager::Get().FindFilesRecursive(HeaderFiles, *SourceDir, TEXT("*.h"), true, false);

	FRegexPattern UClassPattern(TEXT("UCLASS\\s*\\("));
	// Pattern to extract class name: class MODULENAME_API AClassName : public AParentClass
	FRegexPattern ClassDeclPattern(TEXT("class\\s+(?:\\w+_API\\s+)?([AU]\\w+)\\s*(?::\\s*public\\s+(\\w+))?"));

	FString NormalizedSourceDir = SourceDir;
	FPaths::NormalizeFilename(NormalizedSourceDir);
	// Ensure trailing slash for MakePathRelativeTo
	if (!NormalizedSourceDir.EndsWith(TEXT("/")))
	{
		NormalizedSourceDir += TEXT("/");
	}

	for (const FString& HeaderFile : HeaderFiles)
	{
		// Compute relative path from project root
		FString RelPath = HeaderFile;
		FPaths::NormalizeFilename(RelPath);
		FString ProjectDir = FPaths::ProjectDir();
		FPaths::NormalizeFilename(ProjectDir);
		if (!ProjectDir.EndsWith(TEXT("/")))
		{
			ProjectDir += TEXT("/");
		}
		FPaths::MakePathRelativeTo(RelPath, *ProjectDir);

		// Apply module filter (check relative path from Source/)
		if (!ModuleFilter.IsEmpty())
		{
			FString RelFromSource = HeaderFile;
			FPaths::NormalizeFilename(RelFromSource);
			FPaths::MakePathRelativeTo(RelFromSource, *NormalizedSourceDir);
			if (!RelFromSource.StartsWith(ModuleFilter))
			{
				continue;
			}
		}

		// Read file content
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *HeaderFile))
		{
			continue;
		}

		// Check for UCLASS macro
		FRegexMatcher UClassMatcher(UClassPattern, Content);
		if (!UClassMatcher.FindNext())
		{
			continue;
		}

		// Extract class name and parent
		FRegexMatcher ClassMatcher(ClassDeclPattern, Content);
		while (ClassMatcher.FindNext())
		{
			FString ClassName = ClassMatcher.GetCaptureGroup(1);
			FString ParentClass = ClassMatcher.GetCaptureGroup(2);

			// Apply parent class filter
			if (!ParentClassFilter.IsEmpty() && ParentClass != ParentClassFilter)
			{
				continue;
			}

			// Count lines
			TArray<FString> Lines;
			Content.ParseIntoArrayLines(Lines);

			FOliveIRCppSourceFile Entry;
			Entry.FilePath = RelPath;
			Entry.TotalLines = Lines.Num();
			// Store class name in Content field for listing purposes
			Entry.Content = ClassName;

			Results.Add(Entry);
		}
	}

	UE_LOG(LogOliveCppSource, Log, TEXT("Found %d project classes"), Results.Num());
	return Results;
}
