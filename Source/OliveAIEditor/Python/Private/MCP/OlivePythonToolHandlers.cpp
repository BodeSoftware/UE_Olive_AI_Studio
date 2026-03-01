// Copyright Bode Software. All Rights Reserved.

#include "OlivePythonToolHandlers.h"
#include "OlivePythonSchemas.h"
#include "OliveSnapshotManager.h"
#include "MCP/OliveToolRegistry.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"

// PythonScriptPlugin includes
#include "IPythonScriptPlugin.h"

DEFINE_LOG_CATEGORY(LogOlivePythonTools);

FOlivePythonToolHandlers& FOlivePythonToolHandlers::Get()
{
	static FOlivePythonToolHandlers Instance;
	return Instance;
}

void FOlivePythonToolHandlers::RegisterAllTools()
{
	UE_LOG(LogOlivePythonTools, Log, TEXT("Registering Python MCP tools..."));

	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	Registry.RegisterTool(
		TEXT("editor.run_python"),
		TEXT("Execute Python in UE's editor scripting context. Full access to the unreal module. "
			 "Use when standard tools cannot express what you need."),
		OlivePythonSchemas::EditorRunPython(),
		FOliveToolHandler::CreateRaw(this, &FOlivePythonToolHandlers::HandleRunPython),
		{TEXT("blueprint"), TEXT("cpp"), TEXT("python"), TEXT("editor"), TEXT("write")},
		TEXT("editor")
	);
	RegisteredToolNames.Add(TEXT("editor.run_python"));

	UE_LOG(LogOlivePythonTools, Log, TEXT("Registered %d Python MCP tools"), RegisteredToolNames.Num());
}

void FOlivePythonToolHandlers::UnregisterAllTools()
{
	UE_LOG(LogOlivePythonTools, Log, TEXT("Unregistering Python MCP tools..."));

	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();
	for (const FString& ToolName : RegisteredToolNames)
	{
		Registry.UnregisterTool(ToolName);
	}

	RegisteredToolNames.Empty();
	UE_LOG(LogOlivePythonTools, Log, TEXT("Python MCP tools unregistered"));
}

FOliveToolResult FOlivePythonToolHandlers::HandleRunPython(const TSharedPtr<FJsonObject>& Params)
{
	// 1. Validate parameter
	FString Script;
	if (!Params->TryGetStringField(TEXT("script"), Script))
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_MISSING_PARAM"),
			TEXT("Missing required parameter 'script'"),
			TEXT("Provide the Python script to execute")
		);
	}

	if (Script.TrimStartAndEnd().IsEmpty())
	{
		return FOliveToolResult::Error(
			TEXT("VALIDATION_EMPTY_SCRIPT"),
			TEXT("Script is empty"),
			TEXT("Provide a non-empty Python script")
		);
	}

	// 2. Check if PythonScriptPlugin is available
	// IPythonScriptPlugin::Get() returns nullptr if the module is not loaded.
	// IsPythonAvailable() checks if Python support is actually compiled in.
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin)
	{
		return FOliveToolResult::Error(
			TEXT("PYTHON_PLUGIN_NOT_AVAILABLE"),
			TEXT("The Python Editor Script Plugin is not enabled. "
				 "Enable it in Edit > Plugins > Scripting > Python Editor Script Plugin, then restart the editor."),
			TEXT("Enable the Python Editor Script Plugin in Project Settings")
		);
	}

	if (!PythonPlugin->IsPythonAvailable())
	{
		return FOliveToolResult::Error(
			TEXT("PYTHON_NOT_AVAILABLE"),
			TEXT("The Python Editor Script Plugin is loaded but Python support is not available. "
				 "This may indicate a Python installation issue."),
			TEXT("Check that Python 3.x is installed and the PythonScriptPlugin can find it")
		);
	}

	// 3. Take automatic snapshot before execution
	//    Use a simple name to identify Python script snapshots
	{
		TArray<FString> EmptyAssets; // Snapshot with no specific assets = project-wide safety net
		FOliveSnapshotManager::Get().CreateSnapshot(
			TEXT("pre_python_script"),
			EmptyAssets,
			TEXT("Automatic snapshot before editor.run_python execution")
		);
	}

	// 4. Wrap script in try/except
	const FString WrappedScript = WrapScript(Script);

	// 5. Execute via FPythonCommandEx
	FPythonCommandEx PythonCommand;
	PythonCommand.Command = WrappedScript;
	// ExecutionMode defaults to ExecuteFile which handles multi-line scripts.
	// ExecuteStatement is for single statements only.
	// No need to set ExecutionMode explicitly.

	const double StartTime = FPlatformTime::Seconds();
	const bool bSuccess = PythonPlugin->ExecPythonCommandEx(PythonCommand);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	// 6. Collect output from PythonCommand.LogOutput and CommandResult
	FString OutputText;

	// CommandResult contains the expression result (if any)
	if (!PythonCommand.CommandResult.IsEmpty())
	{
		OutputText += PythonCommand.CommandResult;
	}

	// LogOutput contains print() statements and error messages
	for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
	{
		if (!OutputText.IsEmpty())
		{
			OutputText += TEXT("\n");
		}

		// Prefix errors/warnings for clarity
		// FPythonLogOutputEntry::Type is EPythonLogOutputType (Info/Warning/Error)
		if (Entry.Type == EPythonLogOutputType::Error)
		{
			OutputText += TEXT("[ERROR] ");
		}
		else if (Entry.Type == EPythonLogOutputType::Warning)
		{
			OutputText += TEXT("[WARNING] ");
		}

		OutputText += Entry.Output;
	}

	// 7. Log to persistent file
	LogScriptExecution(Script, bSuccess, OutputText);

	// 8. Build result
	TSharedPtr<FJsonObject> ResultData = MakeShareable(new FJsonObject());
	ResultData->SetBoolField(TEXT("success"), bSuccess);
	ResultData->SetStringField(TEXT("output"), OutputText);
	ResultData->SetNumberField(TEXT("execution_time_ms"), ElapsedMs);

	if (bSuccess)
	{
		return FOliveToolResult::Success(ResultData);
	}
	else
	{
		FOliveToolResult Result = FOliveToolResult::Error(
			TEXT("PYTHON_EXECUTION_FAILED"),
			TEXT("Python script execution failed"),
			TEXT("Check the output for error details. A snapshot was taken before execution for rollback.")
		);
		Result.Data = ResultData;
		return Result;
	}
}

FString FOlivePythonToolHandlers::WrapScript(const FString& RawScript) const
{
	// Wrap in try/except to prevent editor crashes from Python exceptions.
	// Indent every line of the original script by 4 spaces for the try block.
	FString Indented;
	TArray<FString> Lines;
	RawScript.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		Indented += TEXT("    ") + Line + TEXT("\n");
	}

	return FString::Printf(
		TEXT("import traceback\ntry:\n%sexcept Exception as e:\n    print('[PYTHON_ERROR] ' + str(e))\n    traceback.print_exc()\n"),
		*Indented
	);
}

void FOlivePythonToolHandlers::LogScriptExecution(const FString& Script, bool bSuccess, const FString& Output)
{
	const FString ScriptLogFilePath = GetScriptLogPath();

	// Ensure directory exists
	const FString LogDir = FPaths::GetPath(ScriptLogFilePath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*LogDir);

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"));
	const FString StatusStr = bSuccess ? TEXT("SUCCESS") : TEXT("FAILED");

	FString LogEntry = FString::Printf(
		TEXT("\n========== [%s] %s ==========\n%s\n---------- Output ----------\n%s\n"),
		*Timestamp, *StatusStr, *Script, *Output
	);

	FFileHelper::SaveStringToFile(
		LogEntry, *ScriptLogFilePath,
		FFileHelper::EEncodingOptions::AutoDetect,
		&IFileManager::Get(),
		EFileWrite::FILEWRITE_Append
	);
}

FString FOlivePythonToolHandlers::GetScriptLogPath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OliveAI"), TEXT("PythonScripts.log"));
}
