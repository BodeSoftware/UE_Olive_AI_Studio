// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOlivePythonTools, Log, All);

/**
 * FOlivePythonToolHandlers
 *
 * Registers and handles the editor.run_python MCP tool.
 * Bridges UE's PythonScriptPlugin API (IPythonScriptPlugin::ExecPythonCommandEx)
 * with the Olive tool registry.
 *
 * Safety layers:
 * 1. Automatic snapshot via FOliveSnapshotManager before every execution
 * 2. Persistent script logging to Saved/OliveAI/PythonScripts.log
 * 3. try/except wrapper around every script
 * 4. Timeout (configurable, default 30s) -- Note: FPythonCommandEx does not
 *    natively support cancellation, so timeout is advisory via log warning.
 */
class OLIVEAIEDITOR_API FOlivePythonToolHandlers
{
public:
	/** Get singleton instance */
	static FOlivePythonToolHandlers& Get();

	/** Register all Python tools with the tool registry */
	void RegisterAllTools();

	/** Unregister all Python tools */
	void UnregisterAllTools();

private:
	FOlivePythonToolHandlers() = default;

	FOlivePythonToolHandlers(const FOlivePythonToolHandlers&) = delete;
	FOlivePythonToolHandlers& operator=(const FOlivePythonToolHandlers&) = delete;

	/** Handle the editor.run_python tool call */
	FOliveToolResult HandleRunPython(const TSharedPtr<FJsonObject>& Params);

	/** Log a script execution to the persistent log file */
	void LogScriptExecution(const FString& Script, bool bSuccess, const FString& Output);

	/** Get the log file path */
	FString GetScriptLogPath() const;

	/** Wrap a script in try/except for safe execution */
	FString WrapScript(const FString& RawScript) const;

	TArray<FString> RegisteredToolNames;
};
