// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOliveCppTools, Log, All);

/**
 * FOliveCppToolHandlers
 *
 * Registers and handles all C++ integration MCP tools.
 * Bridges between the MCP tool registry and the C++ reader/writer infrastructure.
 *
 * P5 consolidated tools (6 real):
 *   cpp.read (entity: class|enum|struct|header|source)
 *   cpp.list (kind: project|blueprint_callable|overridable)
 *   cpp.create_class
 *   cpp.add (entity: function|property)
 *   cpp.modify_source
 *   cpp.compile
 *
 * Legacy tool names (cpp.read_class, cpp.read_enum, cpp.read_struct,
 * cpp.read_header, cpp.read_source, cpp.list_project_classes,
 * cpp.list_blueprint_callable, cpp.list_overridable, cpp.add_function,
 * cpp.add_property) continue to work as aliases registered in
 * OliveToolRegistry::GetToolAliases().
 */
class OLIVEAIEDITOR_API FOliveCppToolHandlers
{
public:
	/** Get singleton instance */
	static FOliveCppToolHandlers& Get();

	/** Register all C++ tools with the tool registry */
	void RegisterAllTools();

	/** Unregister all C++ tools */
	void UnregisterAllTools();

private:
	FOliveCppToolHandlers() = default;

	FOliveCppToolHandlers(const FOliveCppToolHandlers&) = delete;
	FOliveCppToolHandlers& operator=(const FOliveCppToolHandlers&) = delete;

	// Registration helpers
	void RegisterReflectionTools();
	void RegisterSourceTools();
	void RegisterWriteTools();

	// P5 consolidated dispatchers
	FOliveToolResult HandleCppRead(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleCppList(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleCppAdd(const TSharedPtr<FJsonObject>& Params);

	// Internal handlers (invoked by dispatchers; legacy names reach them via aliases)
	FOliveToolResult HandleReadClass(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleListBlueprintCallable(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleListOverridable(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleReadEnum(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleReadStruct(const TSharedPtr<FJsonObject>& Params);

	// Source handlers
	FOliveToolResult HandleReadHeader(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleReadSource(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleListProjectClasses(const TSharedPtr<FJsonObject>& Params);

	// Write handlers
	FOliveToolResult HandleCreateClass(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleAddProperty(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleAddFunction(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleModifySource(const TSharedPtr<FJsonObject>& Params);
	FOliveToolResult HandleCompile(const TSharedPtr<FJsonObject>& Params);

	TArray<FString> RegisteredToolNames;
};
