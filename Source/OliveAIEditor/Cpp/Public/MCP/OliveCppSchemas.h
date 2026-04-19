// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * C++ Tool Schema Builder
 *
 * Provides JSON Schema Draft 7 definitions for all C++ reflection, source reading,
 * and source writing MCP tools. Reuses common helpers from OliveBlueprintSchemas.
 */
namespace OliveCppSchemas
{
	// ============================================================================
	// Reflection Reader Schemas
	// ============================================================================

	/** Schema for cpp.read_class: {class_name, include_inherited?, include_functions?, include_properties?} */
	TSharedPtr<FJsonObject> CppReadClass();

	/** Schema for cpp.list_blueprint_callable: {class_name, include_inherited?} */
	TSharedPtr<FJsonObject> CppListBlueprintCallable();

	/** Schema for cpp.list_overridable: {class_name} */
	TSharedPtr<FJsonObject> CppListOverridable();

	/** Schema for cpp.read_enum: {enum_name} */
	TSharedPtr<FJsonObject> CppReadEnum();

	/** Schema for cpp.read_struct: {struct_name, include_inherited?} */
	TSharedPtr<FJsonObject> CppReadStruct();

	// ============================================================================
	// Source Reader Schemas
	// ============================================================================

	/** Schema for cpp.read_header: {file_path, start_line?, end_line?} */
	TSharedPtr<FJsonObject> CppReadHeader();

	/** Schema for cpp.read_source: {file_path, start_line?, end_line?} */
	TSharedPtr<FJsonObject> CppReadSource();

	/** Schema for cpp.list_project_classes: {module_filter?, parent_class?} */
	TSharedPtr<FJsonObject> CppListProjectClasses();

	// ============================================================================
	// Source Writer Schemas
	// ============================================================================

	/** Schema for cpp.create_class: {class_name, parent_class, module_name, path?, interfaces?, properties?, functions?} */
	TSharedPtr<FJsonObject> CppCreateClass();

	/** Schema for cpp.add_property: {file_path, property_name, property_type, category?, specifiers?, default_value?} */
	TSharedPtr<FJsonObject> CppAddProperty();

	/** Schema for cpp.add_function: {file_path, function_name, return_type?, parameters?, specifiers?, is_virtual?, body?} */
	TSharedPtr<FJsonObject> CppAddFunction();

	/** Schema for cpp.modify_source: {file_path, anchor_text, operation, replacement_text?, occurrence?, start_line?, end_line?, require_unique_match?} */
	TSharedPtr<FJsonObject> CppModifySource();

	/** Schema for cpp.compile: {} */
	TSharedPtr<FJsonObject> CppCompile();

	// ============================================================================
	// P5 Consolidated Schemas
	// ============================================================================

	/** Schema for cpp.read: dispatches on entity (class|enum|struct|header|source). */
	TSharedPtr<FJsonObject> CppRead();

	/** Schema for cpp.list: dispatches on kind (project|blueprint_callable|overridable). */
	TSharedPtr<FJsonObject> CppList();

	/** Schema for cpp.add: dispatches on entity (function|property). */
	TSharedPtr<FJsonObject> CppAdd();
}
