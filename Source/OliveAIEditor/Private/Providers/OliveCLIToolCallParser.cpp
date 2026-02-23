// Copyright Bode Software. All Rights Reserved.

#include "Providers/OliveCLIToolCallParser.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogOliveCLIParser, Log, All);

// ============================================================================
// Public API
// ============================================================================

bool FOliveCLIToolCallParser::Parse(const FString& ResponseText, TArray<FOliveStreamChunk>& OutToolCalls, FString& OutCleanText)
{
	OutToolCalls.Empty();
	OutCleanText.Empty();

	if (ResponseText.IsEmpty())
	{
		return false;
	}

	const bool bFoundCalls = TryParseXMLDelimited(ResponseText, OutToolCalls, OutCleanText);

	if (!bFoundCalls || OutToolCalls.Num() == 0)
	{
		OutCleanText = ResponseText;
		OutToolCalls.Empty();
		return false;
	}

	return true;
}

FString FOliveCLIToolCallParser::GetFormatInstructions()
{
	return FString(
		TEXT("## Tool Call Format\n\n")
		TEXT("When you want to call a tool, output a <tool_call> block:\n")
		TEXT("<tool_call id=\"tc_1\">\n")
		TEXT("{\"name\": \"tool.name\", \"arguments\": {\"param\": \"value\"}}\n")
		TEXT("</tool_call>\n\n")
		TEXT("Rules:\n")
		TEXT("- You can output multiple <tool_call> blocks in one response.\n")
		TEXT("- The id must be unique per call (tc_1, tc_2, tc_3, etc.).\n")
		TEXT("- After tools execute, you'll receive results and can continue.\n")
		TEXT("- When you're done (no more tools needed), respond with text only, no <tool_call> blocks.\n")
		TEXT("- Always include both 'name' and 'arguments' in the JSON body.\n")
	);
}

// ============================================================================
// Private Implementation
// ============================================================================

bool FOliveCLIToolCallParser::TryParseXMLDelimited(const FString& Text, TArray<FOliveStreamChunk>& OutCalls, FString& OutClean)
{
	OutClean.Empty();
	OutCalls.Empty();

	static const FString OpenTag = TEXT("<tool_call");
	static const FString CloseTag = TEXT("</tool_call>");
	static const FString IdPrefix = TEXT("id=\"");

	int32 SearchPos = 0;
	const int32 TextLen = Text.Len();

	while (SearchPos < TextLen)
	{
		// Find the next <tool_call tag
		const int32 TagStart = Text.Find(OpenTag, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchPos);

		if (TagStart == INDEX_NONE)
		{
			// No more tool_call tags - append remaining text to clean output
			OutClean += Text.Mid(SearchPos);
			break;
		}

		// Append text between the last position and this tag start to clean output
		if (TagStart > SearchPos)
		{
			OutClean += Text.Mid(SearchPos, TagStart - SearchPos);
		}

		// Find the closing > of the opening tag
		const int32 OpenTagClose = Text.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TagStart + OpenTag.Len());
		if (OpenTagClose == INDEX_NONE)
		{
			// Malformed: no closing > for the opening tag - include raw text and stop
			UE_LOG(LogOliveCLIParser, Warning, TEXT("Malformed <tool_call> tag: no closing '>' found at position %d"), TagStart);
			OutClean += Text.Mid(TagStart);
			break;
		}

		// Extract the opening tag attributes substring (between <tool_call and >)
		const FString TagAttributes = Text.Mid(TagStart + OpenTag.Len(), OpenTagClose - (TagStart + OpenTag.Len()));

		// Extract id attribute if present
		FString ToolCallId;
		const int32 IdStart = TagAttributes.Find(IdPrefix, ESearchCase::CaseSensitive);
		if (IdStart != INDEX_NONE)
		{
			const int32 IdValueStart = IdStart + IdPrefix.Len();
			const int32 IdEnd = TagAttributes.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, IdValueStart);
			if (IdEnd != INDEX_NONE)
			{
				ToolCallId = TagAttributes.Mid(IdValueStart, IdEnd - IdValueStart);
			}
		}

		if (ToolCallId.IsEmpty())
		{
			ToolCallId = GenerateToolCallId();
		}

		// Find the closing </tool_call> tag
		const int32 CloseTagStart = Text.Find(CloseTag, ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenTagClose + 1);
		if (CloseTagStart == INDEX_NONE)
		{
			// Malformed: no closing tag - include raw text in clean output and stop
			UE_LOG(LogOliveCLIParser, Warning, TEXT("Malformed <tool_call>: no </tool_call> found for block starting at position %d"), TagStart);
			OutClean += Text.Mid(TagStart);
			break;
		}

		// Extract the JSON body between > and </tool_call>
		const int32 BodyStart = OpenTagClose + 1;
		const int32 BodyLen = CloseTagStart - BodyStart;
		FString JsonBody = Text.Mid(BodyStart, BodyLen).TrimStartAndEnd();

		// Deserialize the JSON body
		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonBody);

		if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
		{
			UE_LOG(LogOliveCLIParser, Warning, TEXT("Failed to parse JSON in <tool_call id=\"%s\">: %s"), *ToolCallId, *JsonBody.Left(200));
			// Include the raw block text in clean output so it is not silently lost
			OutClean += Text.Mid(TagStart, (CloseTagStart + CloseTag.Len()) - TagStart);
			SearchPos = CloseTagStart + CloseTag.Len();
			continue;
		}

		// Extract the tool name
		FString ToolName;
		if (!JsonObj->TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty())
		{
			UE_LOG(LogOliveCLIParser, Warning, TEXT("Missing or empty 'name' field in <tool_call id=\"%s\">"), *ToolCallId);
			// Include the raw block text in clean output
			OutClean += Text.Mid(TagStart, (CloseTagStart + CloseTag.Len()) - TagStart);
			SearchPos = CloseTagStart + CloseTag.Len();
			continue;
		}

		// Extract arguments (default to empty object if missing)
		TSharedPtr<FJsonObject> Arguments;
		const TSharedPtr<FJsonObject>* ArgumentsPtr = nullptr;
		if (JsonObj->TryGetObjectField(TEXT("arguments"), ArgumentsPtr) && ArgumentsPtr && ArgumentsPtr->IsValid())
		{
			Arguments = *ArgumentsPtr;
		}
		else
		{
			Arguments = MakeShareable(new FJsonObject());
		}

		// Build the tool call chunk
		FOliveStreamChunk Chunk;
		Chunk.bIsToolCall = true;
		Chunk.ToolCallId = ToolCallId;
		Chunk.ToolName = ToolName;
		Chunk.ToolArguments = Arguments;
		Chunk.bIsComplete = true;

		OutCalls.Add(MoveTemp(Chunk));

		// Advance past the closing tag
		SearchPos = CloseTagStart + CloseTag.Len();
	}

	return OutCalls.Num() > 0;
}

FString FOliveCLIToolCallParser::GenerateToolCallId()
{
	static TAtomic<int32> Counter(0);
	const int32 Id = ++Counter;
	return FString::Printf(TEXT("tc_%d"), Id);
}
