// Copyright Bode Software. All Rights Reserved.

#include "Writer/OliveCppSourceWriter.h"
#include "Reader/OliveCppSourceReader.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Internationalization/Regex.h"
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

DEFINE_LOG_CATEGORY(LogOliveCppWriter);

// ----------------------------------------------------------------------------
// Path Resolution
// ----------------------------------------------------------------------------

FString FOliveCppSourceWriter::ResolveFilePath(const FString& RelativePath)
{
	FString SourceDir = FPaths::ProjectDir() / TEXT("Source");

	// If path already starts with "Source/", strip it to avoid double Source/Source/
	FString CleanPath = RelativePath;
	if (CleanPath.StartsWith(TEXT("Source/")) || CleanPath.StartsWith(TEXT("Source\\")))
	{
		CleanPath = CleanPath.RightChop(7);
	}

	FString AbsolutePath = SourceDir / CleanPath;
	FPaths::NormalizeFilename(AbsolutePath);

	// Safety check: resolved path must still be under Source/
	FString NormalizedSourceDir = SourceDir;
	FPaths::NormalizeFilename(NormalizedSourceDir);
	if (!AbsolutePath.StartsWith(NormalizedSourceDir))
	{
		UE_LOG(LogOliveCppWriter, Warning, TEXT("ResolveFilePath: Path escaped the sandbox: %s"), *AbsolutePath);
		return FString();
	}

	return AbsolutePath;
}

// ----------------------------------------------------------------------------
// Backup
// ----------------------------------------------------------------------------

FString FOliveCppSourceWriter::GetBackupDir()
{
	return FPaths::ProjectSavedDir() / TEXT("OliveAI") / TEXT("Backups");
}

bool FOliveCppSourceWriter::BackupFile(const FString& AbsolutePath, FString* OutBackupPath)
{
	if (!FPaths::FileExists(AbsolutePath))
	{
		return false;
	}

	FString BackupDir = GetBackupDir();
	IFileManager::Get().MakeDirectory(*BackupDir, true);

	FString FileName = FPaths::GetCleanFilename(AbsolutePath);
	FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	FString BackupPath = BackupDir / FString::Printf(TEXT("%s_%s"), *Timestamp, *FileName);

	if (IFileManager::Get().Copy(*BackupPath, *AbsolutePath) == COPY_OK)
	{
		if (OutBackupPath)
		{
			*OutBackupPath = BackupPath;
		}
		UE_LOG(LogOliveCppWriter, Log, TEXT("Backed up %s -> %s"), *AbsolutePath, *BackupPath);
		return true;
	}

	UE_LOG(LogOliveCppWriter, Warning, TEXT("Failed to backup %s"), *AbsolutePath);
	return false;
}

// ----------------------------------------------------------------------------
// Utilities
// ----------------------------------------------------------------------------

FString FOliveCppSourceWriter::DetermineClassPrefix(const FString& ParentClass)
{
	if (ParentClass.StartsWith(TEXT("A")))
	{
		// Check known Actor-derived prefixes
		if (ParentClass == TEXT("AActor") || ParentClass == TEXT("APawn") ||
			ParentClass == TEXT("ACharacter") || ParentClass == TEXT("APlayerController") ||
			ParentClass == TEXT("AGameModeBase") || ParentClass == TEXT("AGameMode") ||
			ParentClass == TEXT("AGameStateBase") || ParentClass == TEXT("APlayerState") ||
			ParentClass == TEXT("AHUD") || ParentClass == TEXT("AInfo") ||
			ParentClass == TEXT("AVolume") || ParentClass == TEXT("ALevelScriptActor") ||
			ParentClass == TEXT("AWorldSettings") || ParentClass == TEXT("ADecalActor") ||
			ParentClass == TEXT("ALight") || ParentClass == TEXT("AStaticMeshActor") ||
			ParentClass == TEXT("ASkeletalMeshActor"))
		{
			return TEXT("A");
		}
		// If starts with A and second char is uppercase, likely Actor-derived
		if (ParentClass.Len() > 1 && FChar::IsUpper(ParentClass[1]))
		{
			return TEXT("A");
		}
	}
	if (ParentClass.StartsWith(TEXT("F")) && ParentClass.Len() > 1 && FChar::IsUpper(ParentClass[1]))
	{
		return TEXT("F");
	}
	return TEXT("U"); // Default to UObject-derived
}

FString FOliveCppSourceWriter::GetAPIMacro(const FString& ModuleName)
{
	return ModuleName.ToUpper() + TEXT("_API");
}

FString FOliveCppSourceWriter::ExtractClassNameFromHeader(const TArray<FString>& Lines)
{
	FRegexPattern ClassPattern(TEXT("class\\s+(?:\\w+_API\\s+)?(\\w+)\\s*(?::|\\{)"));
	FString AllContent = FString::Join(Lines, TEXT("\n"));

	FRegexMatcher Matcher(ClassPattern, AllContent);
	if (Matcher.FindNext())
	{
		return Matcher.GetCaptureGroup(1);
	}

	return FString();
}

// ----------------------------------------------------------------------------
// Insertion Points
// ----------------------------------------------------------------------------

int32 FOliveCppSourceWriter::FindPropertyInsertionPoint(const TArray<FString>& Lines)
{
	int32 LastUPropertyEnd = -1;
	int32 PublicSection = -1;
	int32 GeneratedBodyLine = -1;

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		FString Trimmed = Lines[i].TrimStartAndEnd();
		if (Trimmed.Contains(TEXT("UPROPERTY(")))
		{
			// The property declaration is on the next line after UPROPERTY()
			// Find the next non-empty line that looks like a property declaration
			for (int32 j = i + 1; j < Lines.Num() && j <= i + 3; ++j)
			{
				FString NextTrimmed = Lines[j].TrimStartAndEnd();
				if (!NextTrimmed.IsEmpty() && !NextTrimmed.StartsWith(TEXT("UPROPERTY")) && NextTrimmed.Contains(TEXT(";")))
				{
					LastUPropertyEnd = j + 1;
					break;
				}
			}
		}
		if (Trimmed == TEXT("public:"))
		{
			PublicSection = i;
		}
		if (Trimmed.Contains(TEXT("GENERATED_BODY()")))
		{
			GeneratedBodyLine = i;
		}
	}

	// Prefer inserting after the last UPROPERTY block
	if (LastUPropertyEnd >= 0)
	{
		return LastUPropertyEnd;
	}

	// Otherwise insert after GENERATED_BODY() + constructor area in public section
	if (GeneratedBodyLine >= 0)
	{
		// Skip past GENERATED_BODY(), any blank lines, and constructor
		for (int32 i = GeneratedBodyLine + 1; i < Lines.Num(); ++i)
		{
			FString Trimmed = Lines[i].TrimStartAndEnd();
			// Stop at protected:/private: sections
			if (Trimmed == TEXT("protected:") || Trimmed == TEXT("private:"))
			{
				return i;
			}
			// After the constructor declaration, find the next blank line
			if (Trimmed.IsEmpty() && i > GeneratedBodyLine + 2)
			{
				return i;
			}
		}
		return GeneratedBodyLine + 2;
	}

	if (PublicSection >= 0)
	{
		return PublicSection + 2;
	}

	// Fallback: before the closing brace
	return FMath::Max(0, Lines.Num() - 1);
}

int32 FOliveCppSourceWriter::FindFunctionInsertionPoint(const TArray<FString>& Lines)
{
	int32 LastUFunctionEnd = -1;
	int32 ProtectedSection = -1;
	int32 PrivateSection = -1;
	int32 PublicSection = -1;

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		FString Trimmed = Lines[i].TrimStartAndEnd();
		if (Trimmed.Contains(TEXT("UFUNCTION(")))
		{
			// Find the function declaration after this macro
			for (int32 j = i + 1; j < Lines.Num() && j <= i + 5; ++j)
			{
				FString NextTrimmed = Lines[j].TrimStartAndEnd();
				if (!NextTrimmed.IsEmpty() && !NextTrimmed.StartsWith(TEXT("UFUNCTION")) && NextTrimmed.Contains(TEXT(";")))
				{
					LastUFunctionEnd = j + 1;
					break;
				}
			}
		}
		if (Trimmed == TEXT("public:"))
		{
			PublicSection = i;
		}
		if (Trimmed == TEXT("protected:"))
		{
			ProtectedSection = i;
		}
		if (Trimmed == TEXT("private:"))
		{
			PrivateSection = i;
		}
	}

	// Prefer inserting after the last UFUNCTION block
	if (LastUFunctionEnd >= 0)
	{
		return LastUFunctionEnd;
	}

	// Insert before protected: section if it exists
	if (ProtectedSection >= 0)
	{
		return ProtectedSection;
	}

	// Insert before private: section if it exists
	if (PrivateSection >= 0)
	{
		return PrivateSection;
	}

	// Fallback: after public: section
	if (PublicSection >= 0)
	{
		// Skip past GENERATED_BODY and constructor
		for (int32 i = PublicSection + 1; i < Lines.Num(); ++i)
		{
			FString Trimmed = Lines[i].TrimStartAndEnd();
			if (Trimmed.IsEmpty() && i > PublicSection + 3)
			{
				return i + 1;
			}
		}
		return PublicSection + 3;
	}

	// Fallback: before the closing brace
	return FMath::Max(0, Lines.Num() - 1);
}

FString FOliveCppSourceWriter::FindCorrespondingSourceFile(const FString& HeaderPath)
{
	// Replace Public/ with Private/ and .h with .cpp
	FString SourcePath = HeaderPath;
	SourcePath = SourcePath.Replace(TEXT("/Public/"), TEXT("/Private/"));
	SourcePath = SourcePath.Replace(TEXT("\\Public\\"), TEXT("\\Private\\"));
	SourcePath = FPaths::ChangeExtension(SourcePath, TEXT("cpp"));

	if (FPaths::FileExists(SourcePath))
	{
		return SourcePath;
	}

	// Try same directory as header
	SourcePath = FPaths::ChangeExtension(HeaderPath, TEXT("cpp"));
	if (FPaths::FileExists(SourcePath))
	{
		return SourcePath;
	}

	return FString();
}

// ----------------------------------------------------------------------------
// Content Generation
// ----------------------------------------------------------------------------

FString FOliveCppSourceWriter::GenerateHeaderContent(
	const FString& FullClassName,
	const FString& ParentClass,
	const FString& ModuleName,
	const TArray<FString>& Interfaces)
{
	FString APIMacro = GetAPIMacro(ModuleName);
	FString GeneratedHeaderName = FullClassName + TEXT(".generated.h");

	// Build interface list for class declaration
	FString InterfaceDecl;
	for (const FString& Interface : Interfaces)
	{
		InterfaceDecl += FString::Printf(TEXT(", public %s"), *Interface);
	}

	FString Content;
	Content += TEXT("// Copyright Bode Software. All Rights Reserved.\n\n");
	Content += TEXT("#pragma once\n\n");
	Content += TEXT("#include \"CoreMinimal.h\"\n");
	Content += FString::Printf(TEXT("#include \"%s\"\n\n"), *GeneratedHeaderName);
	Content += TEXT("UCLASS()\n");
	Content += FString::Printf(TEXT("class %s %s : public %s%s\n"), *APIMacro, *FullClassName, *ParentClass, *InterfaceDecl);
	Content += TEXT("{\n");
	Content += TEXT("\tGENERATED_BODY()\n\n");
	Content += TEXT("public:\n");
	Content += FString::Printf(TEXT("\t%s();\n\n"), *FullClassName);
	Content += TEXT("protected:\n\n");
	Content += TEXT("private:\n\n");
	Content += TEXT("};\n");

	return Content;
}

FString FOliveCppSourceWriter::GenerateSourceContent(
	const FString& FullClassName,
	const FString& HeaderRelativePath)
{
	FString Content;
	Content += TEXT("// Copyright Bode Software. All Rights Reserved.\n\n");
	Content += FString::Printf(TEXT("#include \"%s\"\n\n"), *HeaderRelativePath);
	Content += FString::Printf(TEXT("%s::%s()\n"), *FullClassName, *FullClassName);
	Content += TEXT("{\n");
	Content += TEXT("}\n");

	return Content;
}

// ----------------------------------------------------------------------------
// CreateClass
// ----------------------------------------------------------------------------

FOliveToolResult FOliveCppSourceWriter::CreateClass(
	const FString& ClassName,
	const FString& ParentClass,
	const FString& ModuleName,
	const FString& SubPath,
	const TArray<FString>& Interfaces)
{
	// Validate inputs
	if (ClassName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("INVALID_INPUT"), TEXT("ClassName cannot be empty"));
	}
	if (ParentClass.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("INVALID_INPUT"), TEXT("ParentClass cannot be empty"));
	}
	if (ModuleName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("INVALID_INPUT"), TEXT("ModuleName cannot be empty"));
	}

	// Reject path traversal in SubPath
	if (SubPath.Contains(TEXT("..")) || SubPath.Contains(TEXT("~")))
	{
		return FOliveToolResult::Error(TEXT("INVALID_PATH"), TEXT("SubPath must not contain '..' or '~'"));
	}

	// Determine full class name with prefix
	FString Prefix = DetermineClassPrefix(ParentClass);
	FString FullClassName = ClassName;
	if (!ClassName.StartsWith(Prefix))
	{
		FullClassName = Prefix + ClassName;
	}

	// Build file paths
	FString SourceDir = FPaths::ProjectDir() / TEXT("Source") / ModuleName;

	FString HeaderDir = SourceDir / TEXT("Public");
	FString SourceFileDir = SourceDir / TEXT("Private");
	if (!SubPath.IsEmpty())
	{
		HeaderDir = HeaderDir / SubPath;
		SourceFileDir = SourceFileDir / SubPath;
	}

	FString HeaderPath = HeaderDir / (FullClassName + TEXT(".h"));
	FString SourcePath = SourceFileDir / (FullClassName + TEXT(".cpp"));

	FPaths::NormalizeFilename(HeaderPath);
	FPaths::NormalizeFilename(SourcePath);

	// Check files don't already exist
	if (FPaths::FileExists(HeaderPath))
	{
		return FOliveToolResult::Error(TEXT("FILE_EXISTS"),
			FString::Printf(TEXT("Header already exists: %s"), *HeaderPath));
	}
	if (FPaths::FileExists(SourcePath))
	{
		return FOliveToolResult::Error(TEXT("FILE_EXISTS"),
			FString::Printf(TEXT("Source file already exists: %s"), *SourcePath));
	}

	// Create directories
	IFileManager::Get().MakeDirectory(*HeaderDir, true);
	IFileManager::Get().MakeDirectory(*SourceFileDir, true);

	// Generate content
	FString HeaderContent = GenerateHeaderContent(FullClassName, ParentClass, ModuleName, Interfaces);

	// Build header include path relative to module Public/
	FString HeaderIncludePath = FullClassName + TEXT(".h");
	if (!SubPath.IsEmpty())
	{
		HeaderIncludePath = SubPath / HeaderIncludePath;
	}
	FString SourceContent = GenerateSourceContent(FullClassName, HeaderIncludePath);

	// Write files
	if (!FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath))
	{
		return FOliveToolResult::Error(TEXT("WRITE_FAILED"),
			FString::Printf(TEXT("Failed to write header: %s"), *HeaderPath));
	}

	if (!FFileHelper::SaveStringToFile(SourceContent, *SourcePath))
	{
		// Clean up the header we just wrote if source fails
		IFileManager::Get().Delete(*HeaderPath);
		return FOliveToolResult::Error(TEXT("WRITE_FAILED"),
			FString::Printf(TEXT("Failed to write source: %s"), *SourcePath));
	}

	UE_LOG(LogOliveCppWriter, Log, TEXT("Created class %s: %s, %s"), *FullClassName, *HeaderPath, *SourcePath);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("class_name"), FullClassName);
	ResultData->SetStringField(TEXT("header_path"), HeaderPath);
	ResultData->SetStringField(TEXT("source_path"), SourcePath);
	ResultData->SetStringField(TEXT("module"), ModuleName);

	return FOliveToolResult::Success(ResultData);
}

// ----------------------------------------------------------------------------
// AddProperty
// ----------------------------------------------------------------------------

FOliveToolResult FOliveCppSourceWriter::AddProperty(
	const FString& FilePath,
	const FString& PropertyName,
	const FString& PropertyType,
	const FString& Category,
	const TArray<FString>& Specifiers,
	const FString& DefaultValue)
{
	// Validate inputs
	if (FilePath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("INVALID_INPUT"), TEXT("FilePath cannot be empty"));
	}
	if (PropertyName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("INVALID_INPUT"), TEXT("PropertyName cannot be empty"));
	}
	if (PropertyType.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("INVALID_INPUT"), TEXT("PropertyType cannot be empty"));
	}

	// Path safety check
	if (!FOliveCppSourceReader::IsPathSafe(FilePath))
	{
		return FOliveToolResult::Error(TEXT("INVALID_PATH"),
			FString::Printf(TEXT("Path '%s' is not safe. Must be relative, no '..', and must be .h/.cpp/.inl"), *FilePath));
	}

	// Resolve path
	FString AbsPath = ResolveFilePath(FilePath);
	if (AbsPath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("INVALID_PATH"),
			FString::Printf(TEXT("Path resolved outside project Source/: %s"), *FilePath));
	}

	if (!FPaths::FileExists(AbsPath))
	{
		return FOliveToolResult::Error(TEXT("FILE_NOT_FOUND"),
			FString::Printf(TEXT("Header not found: %s"), *AbsPath));
	}

	// Backup before modifying
	BackupFile(AbsPath);

	// Read file
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *AbsPath))
	{
		return FOliveToolResult::Error(TEXT("READ_FAILED"),
			FString::Printf(TEXT("Failed to read file: %s"), *AbsPath));
	}

	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines);

	// Find insertion point
	int32 InsertAt = FindPropertyInsertionPoint(Lines);

	// Build UPROPERTY specifier string
	FString SpecStr;
	TArray<FString> AllSpecs;
	if (!Category.IsEmpty())
	{
		AllSpecs.Add(FString::Printf(TEXT("Category=\"%s\""), *Category));
	}
	AllSpecs.Append(Specifiers);

	if (AllSpecs.Num() > 0)
	{
		SpecStr = FString::Join(AllSpecs, TEXT(", "));
	}

	// Build property declaration line
	FString PropertyLine;
	if (!DefaultValue.IsEmpty())
	{
		PropertyLine = FString::Printf(TEXT("\t%s %s = %s;"), *PropertyType, *PropertyName, *DefaultValue);
	}
	else
	{
		PropertyLine = FString::Printf(TEXT("\t%s %s;"), *PropertyType, *PropertyName);
	}

	// Insert the property block
	Lines.Insert(TEXT(""), InsertAt);
	Lines.Insert(FString::Printf(TEXT("\tUPROPERTY(%s)"), *SpecStr), InsertAt + 1);
	Lines.Insert(PropertyLine, InsertAt + 2);

	// Write back
	FString NewContent = FString::Join(Lines, TEXT("\n"));
	if (!FFileHelper::SaveStringToFile(NewContent, *AbsPath))
	{
		return FOliveToolResult::Error(TEXT("WRITE_FAILED"),
			FString::Printf(TEXT("Failed to write file: %s"), *AbsPath));
	}

	UE_LOG(LogOliveCppWriter, Log, TEXT("Added property %s %s to %s"), *PropertyType, *PropertyName, *FilePath);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("file"), FilePath);
	ResultData->SetStringField(TEXT("property"), PropertyName);
	ResultData->SetStringField(TEXT("type"), PropertyType);
	ResultData->SetNumberField(TEXT("line"), InsertAt + 2);

	return FOliveToolResult::Success(ResultData);
}

// ----------------------------------------------------------------------------
// AddFunction
// ----------------------------------------------------------------------------

FOliveToolResult FOliveCppSourceWriter::AddFunction(
	const FString& FilePath,
	const FString& FunctionName,
	const FString& ReturnType,
	const TArray<TPair<FString, FString>>& Parameters,
	const TArray<FString>& Specifiers,
	bool bIsVirtual,
	const FString& Body)
{
	// Validate inputs
	if (FilePath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("INVALID_INPUT"), TEXT("FilePath cannot be empty"));
	}
	if (FunctionName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("INVALID_INPUT"), TEXT("FunctionName cannot be empty"));
	}

	// Path safety check
	if (!FOliveCppSourceReader::IsPathSafe(FilePath))
	{
		return FOliveToolResult::Error(TEXT("INVALID_PATH"),
			FString::Printf(TEXT("Path '%s' is not safe. Must be relative, no '..', and must be .h/.cpp/.inl"), *FilePath));
	}

	// Resolve header path
	FString AbsHeaderPath = ResolveFilePath(FilePath);
	if (AbsHeaderPath.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("INVALID_PATH"),
			FString::Printf(TEXT("Path resolved outside project Source/: %s"), *FilePath));
	}

	if (!FPaths::FileExists(AbsHeaderPath))
	{
		return FOliveToolResult::Error(TEXT("FILE_NOT_FOUND"),
			FString::Printf(TEXT("Header not found: %s"), *AbsHeaderPath));
	}

	// Backup header before modifying
	BackupFile(AbsHeaderPath);

	// Read header file
	FString HeaderContent;
	if (!FFileHelper::LoadFileToString(HeaderContent, *AbsHeaderPath))
	{
		return FOliveToolResult::Error(TEXT("READ_FAILED"),
			FString::Printf(TEXT("Failed to read header: %s"), *AbsHeaderPath));
	}

	TArray<FString> HeaderLines;
	HeaderContent.ParseIntoArrayLines(HeaderLines);

	// Extract class name from header
	FString ClassName = ExtractClassNameFromHeader(HeaderLines);
	if (ClassName.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("PARSE_ERROR"),
			TEXT("Could not extract class name from header file"));
	}

	// Build parameter string
	FString ParamStr;
	for (int32 i = 0; i < Parameters.Num(); ++i)
	{
		if (i > 0)
		{
			ParamStr += TEXT(", ");
		}
		ParamStr += FString::Printf(TEXT("%s %s"), *Parameters[i].Value, *Parameters[i].Key);
	}

	// Determine effective return type
	FString EffectiveReturnType = ReturnType.IsEmpty() ? TEXT("void") : ReturnType;

	// Build UFUNCTION specifier string
	FString SpecStr;
	if (Specifiers.Num() > 0)
	{
		SpecStr = FString::Join(Specifiers, TEXT(", "));
	}

	// Build function declaration for header
	FString FuncDecl;
	if (bIsVirtual)
	{
		FuncDecl = FString::Printf(TEXT("\tvirtual %s %s(%s);"), *EffectiveReturnType, *FunctionName, *ParamStr);
	}
	else
	{
		FuncDecl = FString::Printf(TEXT("\t%s %s(%s);"), *EffectiveReturnType, *FunctionName, *ParamStr);
	}

	// Find insertion point in header
	int32 InsertAt = FindFunctionInsertionPoint(HeaderLines);

	// Insert UFUNCTION + declaration
	HeaderLines.Insert(TEXT(""), InsertAt);
	HeaderLines.Insert(FString::Printf(TEXT("\tUFUNCTION(%s)"), *SpecStr), InsertAt + 1);
	HeaderLines.Insert(FuncDecl, InsertAt + 2);

	// Write modified header
	FString NewHeaderContent = FString::Join(HeaderLines, TEXT("\n"));
	if (!FFileHelper::SaveStringToFile(NewHeaderContent, *AbsHeaderPath))
	{
		return FOliveToolResult::Error(TEXT("WRITE_FAILED"),
			FString::Printf(TEXT("Failed to write header: %s"), *AbsHeaderPath));
	}

	// Find and modify corresponding .cpp file
	FString AbsSourcePath = FindCorrespondingSourceFile(AbsHeaderPath);
	bool bSourceUpdated = false;

	if (!AbsSourcePath.IsEmpty())
	{
		// Backup source file
		BackupFile(AbsSourcePath);

		FString SourceContent;
		if (FFileHelper::LoadFileToString(SourceContent, *AbsSourcePath))
		{
			// Build function implementation
			FString FuncBody;
			if (!Body.IsEmpty())
			{
				FuncBody = Body;
			}
			else
			{
				// Generate default stub
				if (EffectiveReturnType == TEXT("void"))
				{
					FuncBody = TEXT("// TODO: Implement");
				}
				else if (EffectiveReturnType == TEXT("bool"))
				{
					FuncBody = TEXT("return false;");
				}
				else if (EffectiveReturnType == TEXT("int32") || EffectiveReturnType == TEXT("int") || EffectiveReturnType == TEXT("float") || EffectiveReturnType == TEXT("double"))
				{
					FuncBody = TEXT("return 0;");
				}
				else if (EffectiveReturnType == TEXT("FString"))
				{
					FuncBody = TEXT("return FString();");
				}
				else
				{
					FuncBody = FString::Printf(TEXT("return %s();"), *EffectiveReturnType);
				}
			}

			FString FuncImpl;
			FuncImpl += TEXT("\n");
			FuncImpl += FString::Printf(TEXT("%s %s::%s(%s)\n"), *EffectiveReturnType, *ClassName, *FunctionName, *ParamStr);
			FuncImpl += TEXT("{\n");
			FuncImpl += FString::Printf(TEXT("\t%s\n"), *FuncBody);
			FuncImpl += TEXT("}\n");

			// Append to end of source file
			SourceContent += FuncImpl;

			if (FFileHelper::SaveStringToFile(SourceContent, *AbsSourcePath))
			{
				bSourceUpdated = true;
			}
			else
			{
				UE_LOG(LogOliveCppWriter, Warning, TEXT("Failed to write source file: %s"), *AbsSourcePath);
			}
		}
	}
	else
	{
		UE_LOG(LogOliveCppWriter, Warning, TEXT("No corresponding .cpp file found for %s"), *AbsHeaderPath);
	}

	UE_LOG(LogOliveCppWriter, Log, TEXT("Added function %s::%s to %s"), *ClassName, *FunctionName, *FilePath);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("file"), FilePath);
	ResultData->SetStringField(TEXT("class_name"), ClassName);
	ResultData->SetStringField(TEXT("function"), FunctionName);
	ResultData->SetStringField(TEXT("return_type"), EffectiveReturnType);
	ResultData->SetNumberField(TEXT("header_line"), InsertAt + 2);
	ResultData->SetBoolField(TEXT("source_updated"), bSourceUpdated);
	if (bSourceUpdated)
	{
		ResultData->SetStringField(TEXT("source_file"), AbsSourcePath);
	}

	return FOliveToolResult::Success(ResultData);
}

FOliveToolResult FOliveCppSourceWriter::ModifySource(
	const FString& FilePath,
	const FString& AnchorText,
	const FString& Operation,
	const FString& ReplacementText,
	int32 Occurrence,
	int32 StartLine,
	int32 EndLine,
	bool bRequireUniqueMatch)
{
	if (!FOliveCppSourceReader::IsPathSafe(FilePath))
	{
		return FOliveToolResult::Error(TEXT("UNSAFE_PATH"),
			FString::Printf(TEXT("Path is unsafe: %s"), *FilePath),
			TEXT("Use a relative path in Source/ with .h/.cpp/.inl extension"));
	}

	if (AnchorText.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("EMPTY_ANCHOR"), TEXT("anchor_text cannot be empty"));
	}

	if (AnchorText.Len() < 10)
	{
		return FOliveToolResult::Error(TEXT("ANCHOR_TOO_SHORT"),
			FString::Printf(TEXT("anchor_text is only %d characters long — minimum is 10 for reliable matching"), AnchorText.Len()),
			TEXT("Provide a longer, more unique anchor_text to avoid false matches"));
	}

	if (Operation != TEXT("replace") && Operation != TEXT("insert_before") && Operation != TEXT("insert_after"))
	{
		return FOliveToolResult::Error(TEXT("INVALID_OPERATION"),
			FString::Printf(TEXT("Unsupported operation: %s"), *Operation),
			TEXT("Use replace, insert_before, or insert_after"));
	}

	if (Occurrence < 1)
	{
		return FOliveToolResult::Error(TEXT("INVALID_OCCURRENCE"),
			TEXT("occurrence must be >= 1"));
	}

	if (Operation == TEXT("replace") && ReplacementText.IsEmpty())
	{
		return FOliveToolResult::Error(TEXT("EMPTY_REPLACEMENT"),
			TEXT("replacement_text is required for replace operation"));
	}

	const FString AbsPath = ResolveFilePath(FilePath);
	if (AbsPath.IsEmpty() || !FPaths::FileExists(AbsPath))
	{
		return FOliveToolResult::Error(TEXT("FILE_NOT_FOUND"),
			FString::Printf(TEXT("File not found: %s"), *FilePath));
	}

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *AbsPath))
	{
		return FOliveToolResult::Error(TEXT("READ_FAILED"),
			FString::Printf(TEXT("Failed to read file: %s"), *FilePath));
	}

	TArray<int32> MatchStarts;
	int32 SearchFrom = 0;
	while (SearchFrom < Content.Len())
	{
		const int32 FoundAt = Content.Find(AnchorText, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
		if (FoundAt == INDEX_NONE)
		{
			break;
		}
		MatchStarts.Add(FoundAt);
		SearchFrom = FoundAt + AnchorText.Len();
	}

	if (MatchStarts.Num() == 0)
	{
		return FOliveToolResult::Error(TEXT("ANCHOR_NOT_FOUND"),
			TEXT("anchor_text was not found in file"),
			TEXT("Use cpp.read_source/header to verify exact anchor text"));
	}

	if (bRequireUniqueMatch && MatchStarts.Num() > 1)
	{
		return FOliveToolResult::Error(TEXT("AMBIGUOUS_MATCH"),
			FString::Printf(TEXT("anchor_text matched %d locations"), MatchStarts.Num()),
			TEXT("Set require_unique_match=false and choose occurrence"));
	}

	if (Occurrence > MatchStarts.Num())
	{
		return FOliveToolResult::Error(TEXT("OCCURRENCE_OUT_OF_RANGE"),
			FString::Printf(TEXT("Requested occurrence %d but only %d matches found"), Occurrence, MatchStarts.Num()));
	}

	const int32 MatchStart = MatchStarts[Occurrence - 1];
	const int32 MatchEnd = MatchStart + AnchorText.Len();
	const FString Prefix = Content.Left(MatchStart);
	int32 MatchedLine = 1;
	for (int32 CharIndex = 0; CharIndex < Prefix.Len(); ++CharIndex)
	{
		if (Prefix[CharIndex] == TEXT('\n'))
		{
			MatchedLine++;
		}
	}

	if (StartLine > 0 && MatchedLine < StartLine)
	{
		return FOliveToolResult::Error(TEXT("OUT_OF_RANGE"),
			FString::Printf(TEXT("Matched anchor at line %d, before start_line %d"), MatchedLine, StartLine));
	}
	if (EndLine > 0 && MatchedLine > EndLine)
	{
		return FOliveToolResult::Error(TEXT("OUT_OF_RANGE"),
			FString::Printf(TEXT("Matched anchor at line %d, after end_line %d"), MatchedLine, EndLine));
	}

	FString NewContent;
	FString OldExcerpt;
	FString NewExcerpt;
	if (Operation == TEXT("replace"))
	{
		OldExcerpt = AnchorText;
		NewExcerpt = ReplacementText;
		NewContent = Content.Left(MatchStart) + ReplacementText + Content.Mid(MatchEnd);
	}
	else if (Operation == TEXT("insert_before"))
	{
		OldExcerpt = AnchorText;
		NewExcerpt = ReplacementText + AnchorText;
		NewContent = Content.Left(MatchStart) + ReplacementText + Content.Mid(MatchStart);
	}
	else
	{
		OldExcerpt = AnchorText;
		NewExcerpt = AnchorText + ReplacementText;
		NewContent = Content.Left(MatchEnd) + ReplacementText + Content.Mid(MatchEnd);
	}

	FString BackupPath;
	if (!BackupFile(AbsPath, &BackupPath))
	{
		return FOliveToolResult::Error(TEXT("BACKUP_FAILED"),
			FString::Printf(TEXT("Failed to backup file before modifying: %s"), *FilePath));
	}

	if (!FFileHelper::SaveStringToFile(NewContent, *AbsPath))
	{
		return FOliveToolResult::Error(TEXT("WRITE_FAILED"),
			FString::Printf(TEXT("Failed to write file: %s"), *FilePath));
	}

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetBoolField(TEXT("modified"), true);
	ResultData->SetStringField(TEXT("file_path"), FilePath);
	ResultData->SetNumberField(TEXT("matched_line"), MatchedLine);
	ResultData->SetStringField(TEXT("backup_path"), BackupPath);
	ResultData->SetStringField(TEXT("old_excerpt"), OldExcerpt);
	ResultData->SetStringField(TEXT("new_excerpt"), NewExcerpt);
	ResultData->SetBoolField(TEXT("compile_triggered"), false);
	ResultData->SetNumberField(TEXT("match_count"), MatchStarts.Num());
	ResultData->SetNumberField(TEXT("occurrence"), Occurrence);
	ResultData->SetStringField(TEXT("operation"), Operation);

	return FOliveToolResult::Success(ResultData);
}

// ----------------------------------------------------------------------------
// TriggerCompile
// ----------------------------------------------------------------------------

FOliveToolResult FOliveCppSourceWriter::TriggerCompile()
{
#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding && LiveCoding->IsEnabledForSession())
	{
		LiveCoding->Compile();

		UE_LOG(LogOliveCppWriter, Log, TEXT("Live Coding compile triggered"));

		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		ResultData->SetStringField(TEXT("method"), TEXT("LiveCoding"));
		ResultData->SetBoolField(TEXT("triggered"), true);
		return FOliveToolResult::Success(ResultData);
	}
#endif

	UE_LOG(LogOliveCppWriter, Warning, TEXT("Live Coding not available, manual compilation required"));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("method"), TEXT("none"));
	ResultData->SetBoolField(TEXT("triggered"), false);
	ResultData->SetStringField(TEXT("message"), TEXT("Live Coding not available. Please compile manually."));
	return FOliveToolResult::Success(ResultData);
}
