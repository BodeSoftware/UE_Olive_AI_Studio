// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Providers/IOliveAIProvider.h"

/**
 * CLI Tool Call Parser
 *
 * Stateless utility that parses <tool_call> XML blocks from CLI text responses
 * into FOliveStreamChunk instances. This enables CLI-based providers (like Claude Code)
 * to emit tool calls that the ConversationManager can process through the Brain Layer,
 * just like native API providers.
 *
 * Expected format:
 *   <tool_call id="tc_1">{"name":"tool.name","arguments":{"param":"value"}}</tool_call>
 *
 * The parser extracts tool calls and returns the remaining "clean" text separately,
 * allowing interstitial reasoning text between tool calls to be preserved.
 */
class OLIVEAIEDITOR_API FOliveCLIToolCallParser
{
public:
	/**
	 * Parse tool calls from a CLI response text.
	 *
	 * Scans ResponseText for <tool_call> XML blocks, extracts each as an
	 * FOliveStreamChunk with bIsToolCall=true, and collects the remaining
	 * non-tool-call text into OutCleanText.
	 *
	 * @param ResponseText     The full text response from the CLI provider
	 * @param OutToolCalls     Populated with parsed tool call chunks (in document order)
	 * @param OutCleanText     Populated with the response text minus any <tool_call> blocks
	 * @return True if at least one valid tool call was parsed, false otherwise
	 */
	static bool Parse(const FString& ResponseText, TArray<FOliveStreamChunk>& OutToolCalls, FString& OutCleanText);

	/**
	 * Get the format instruction text that teaches models how to emit <tool_call> blocks.
	 *
	 * This text should be included in system prompts for CLI providers so the model
	 * knows the expected format for tool invocations.
	 *
	 * @return A static FString containing format instructions and examples
	 */
	static FString GetFormatInstructions();

private:
	/**
	 * Core XML-delimited parser implementation.
	 *
	 * Scans Text for <tool_call ...>...</tool_call> blocks, extracts the id attribute
	 * and JSON body from each, deserializes the JSON to get name and arguments,
	 * and builds FOliveStreamChunk instances. Text outside of tool_call blocks is
	 * appended to OutClean.
	 *
	 * @param Text      Input text to scan
	 * @param OutCalls  Populated with parsed tool call chunks
	 * @param OutClean  Populated with non-tool-call text
	 * @return True if at least one tool call was successfully parsed
	 */
	static bool TryParseXMLDelimited(const FString& Text, TArray<FOliveStreamChunk>& OutCalls, FString& OutClean);

	/**
	 * Generate a unique tool call ID.
	 *
	 * Uses a monotonically increasing counter to produce IDs of the form "tc_1", "tc_2", etc.
	 * Thread-safe via atomic increment.
	 *
	 * @return A unique tool call ID string
	 */
	static FString GenerateToolCallId();
};
