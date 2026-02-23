// Copyright Bode Software. All Rights Reserved.

/**
 * OliveCLIToolSchemaSerializer.h
 *
 * Stateless utility that converts an array of FOliveToolDefinition into compact
 * human-readable text suitable for embedding in CLI prompts. Groups tools by
 * category and renders each tool as a function-call-style signature with typed
 * parameters and optional descriptions.
 *
 * Output format:
 *   ## Available Tools
 *
 *   ### blueprint
 *   - blueprint.create(path: string [required], parent_class: string)
 *     Create a new Blueprint asset at the specified path.
 *
 * Used by FOliveClaudeCodeProvider::BuildSystemPrompt() to teach CLI-based
 * models about available tools without requiring native tool-call support.
 */

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FOliveToolDefinition;

/**
 * CLI Tool Schema Serializer
 *
 * Converts tool definitions into text-based schema descriptions for
 * injection into system prompts of CLI providers that lack native tool support.
 * All methods are static and thread-safe (no mutable state).
 */
class OLIVEAIEDITOR_API FOliveCLIToolSchemaSerializer
{
public:
	/**
	 * Serialize an array of tool definitions into formatted text grouped by category.
	 *
	 * @param Tools The tool definitions to serialize
	 * @param bCompact If true, omit tool descriptions (signatures only)
	 * @return Formatted text block suitable for embedding in a system prompt
	 */
	static FString Serialize(const TArray<FOliveToolDefinition>& Tools, bool bCompact = false);

	/**
	 * Estimate the token count for serializing the given tools.
	 * Uses the rough heuristic of 1 token per 4 characters.
	 *
	 * @param Tools The tool definitions to estimate for
	 * @return Approximate token count
	 */
	static int32 EstimateTokens(const TArray<FOliveToolDefinition>& Tools);

private:
	/**
	 * Serialize a single tool definition into a line item.
	 *
	 * @param Tool The tool definition to serialize
	 * @param bCompact If true, omit the description line
	 * @return Formatted string for this tool (e.g., "- name(params)\n  description\n")
	 */
	static FString SerializeSingleTool(const FOliveToolDefinition& Tool, bool bCompact);

	/**
	 * Extract and format parameter information from a JSON Schema object.
	 * Reads the "properties" and "required" fields to produce a comma-separated
	 * parameter list like "path: string [required], detail: string".
	 *
	 * @param Schema The JSON Schema object (InputSchema from FOliveToolDefinition)
	 * @return Comma-separated parameter string, or empty if no properties
	 */
	static FString SerializeParams(const TSharedPtr<FJsonObject>& Schema);
};
