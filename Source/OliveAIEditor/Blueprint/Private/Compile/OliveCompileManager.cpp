// Copyright Bode Software. All Rights Reserved.

#include "OliveCompileManager.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Internationalization/Regex.h"
#include "Editor.h"
#include "HAL/PlatformTime.h"
#include "Misc/MessageDialog.h"

DEFINE_LOG_CATEGORY(LogOliveCompile);

// ============================================================================
// Singleton
// ============================================================================

FOliveCompileManager& FOliveCompileManager::Get()
{
	static FOliveCompileManager Instance;
	return Instance;
}

// ============================================================================
// Compilation Methods
// ============================================================================

FOliveIRCompileResult FOliveCompileManager::Compile(UBlueprint* Blueprint)
{
	FOliveIRCompileResult Result;

	if (!Blueprint)
	{
		Result.Errors.Add(FOliveIRCompileError::MakeError(
			TEXT("Blueprint is null"),
			TEXT("Provide a valid Blueprint pointer or asset path.")));
		return Result;
	}

	if (IsPIEActive())
	{
		Result.Errors.Add(FOliveIRCompileError::MakeError(
			TEXT("Cannot compile while Play-In-Editor is active"),
			TEXT("Stop PIE before attempting compilation.")));
		return Result;
	}

	UE_LOG(LogOliveCompile, Log, TEXT("Compiling Blueprint: %s"), *Blueprint->GetPathName());

	// Time the compilation
	double StartTime = FPlatformTime::Seconds();

	// Create a compiler results log to capture ALL compiler messages, including
	// graph-level errors that don't attach to any specific node (e.g., "Graph named
	// 'Interact' already exists"). Without this, those errors are silently dropped
	// because ExtractNodeErrors only finds per-node ErrorMsg/ErrorType.
	FCompilerResultsLog CompilerLog;
	CompilerLog.bSilentMode = true; // Don't spam the editor's message log tab
	CompilerLog.bAnnotateMentionedNodes = false; // We handle node annotation ourselves
	CompilerLog.BeginEvent(TEXT("OliveCompile"));

	// Perform compilation WITH results capture
	EBlueprintCompileOptions CompileOptions = EBlueprintCompileOptions::None;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, CompileOptions, &CompilerLog);

	CompilerLog.EndEvent();

	// Calculate compile time
	Result.CompileTimeMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	// Parse results from the compiled Blueprint
	ParseCompileLog(Blueprint, Result);

	// Extract node-level errors (from ErrorMsg/ErrorType on each UEdGraphNode)
	ExtractNodeErrors(Blueprint, Result);

	// Extract compiler-log-level errors that were NOT captured via per-node extraction.
	// This catches graph-level errors like duplicate graph names, interface conflicts,
	// and other structural issues that the compiler logs but doesn't attach to nodes.
	ExtractCompilerLogErrors(CompilerLog, Result);

	// Set success flag based on error count AND Blueprint status.
	// Blueprint->Status is set by UE's compiler after FKismetEditorUtilities::CompileBlueprint returns.
	// BS_Error is authoritative -- some errors (e.g., duplicate graph names, interface conflicts)
	// are graph-level and do NOT attach to any specific node, so per-node extraction misses them.
	Result.bSuccess = (Result.Errors.Num() == 0) && (Blueprint->Status != BS_Error);

	// Defense-in-depth: If the Blueprint reports an error state but NEITHER per-node
	// extraction NOR compiler log capture found any errors, add a synthetic error.
	// This should be rare now that we capture from FCompilerResultsLog, but serves
	// as a safety net for edge cases where the compiler sets BS_Error without logging.
	if (Blueprint->Status == BS_Error && Result.Errors.Num() == 0)
	{
		Result.Errors.Add(FOliveIRCompileError::MakeError(
			TEXT("Blueprint has compile errors that are not attached to specific nodes. "
				 "This usually means a structural problem like duplicate graph names, "
				 "interface conflicts, or circular dependencies."),
			TEXT("Use blueprint.read to examine the Blueprint structure. "
				 "Check for duplicate function/graph names and interface conflicts. "
				 "Consider using mode:'replace' in plan_json to rebuild the graph.")));

		UE_LOG(LogOliveCompile, Warning,
			TEXT("Blueprint '%s' has BS_Error status but no errors were found from nodes or compiler log. "
				 "Added synthetic error for structural problem detection."),
			*Blueprint->GetPathName());
	}

	UE_LOG(LogOliveCompile, Log, TEXT("Compilation complete: %s - Errors: %d, Warnings: %d, Time: %.2fms"),
		Result.bSuccess ? TEXT("SUCCESS") : TEXT("FAILED"),
		Result.Errors.Num(),
		Result.Warnings.Num(),
		Result.CompileTimeMs);

	return Result;
}

FOliveIRCompileResult FOliveCompileManager::Compile(const FString& AssetPath)
{
	FOliveIRCompileResult Result;

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		Result.Errors.Add(FOliveIRCompileError::MakeError(
			FString::Printf(TEXT("Blueprint not found at path: %s"), *AssetPath),
			TEXT("Verify the asset path is correct. Use project.search to find the correct path.")));
		return Result;
	}

	return Compile(Blueprint);
}

// ============================================================================
// Error Query Methods
// ============================================================================

bool FOliveCompileManager::HasErrors(const UBlueprint* Blueprint) const
{
	if (!Blueprint)
	{
		return false;
	}

	return Blueprint->Status == BS_Error;
}

TArray<FOliveIRCompileError> FOliveCompileManager::GetExistingErrors(const UBlueprint* Blueprint) const
{
	TArray<FOliveIRCompileError> Errors;

	if (!Blueprint)
	{
		return Errors;
	}

	// Extract errors from nodes without recompiling
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Check if node has error
			if (Node->ErrorType == EMessageSeverity::Error && !Node->ErrorMsg.IsEmpty())
			{
				FOliveIRCompileError Error;
				Error.Message = Node->ErrorMsg;
				Error.Severity = EOliveIRCompileErrorSeverity::Error;
				Error.GraphName = Graph->GetName();
				Error.NodeId = Node->GetFName().ToString();
				Error.NodeName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				Error.Suggestion = GenerateSuggestion(Error);
				Errors.Add(Error);
			}
			else if (Node->ErrorType == EMessageSeverity::Warning && !Node->ErrorMsg.IsEmpty())
			{
				// Also capture warnings that exist on nodes
				FOliveIRCompileError Warning;
				Warning.Message = Node->ErrorMsg;
				Warning.Severity = EOliveIRCompileErrorSeverity::Warning;
				Warning.GraphName = Graph->GetName();
				Warning.NodeId = Node->GetFName().ToString();
				Warning.NodeName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				Warning.Suggestion = GenerateSuggestion(Warning);
				Errors.Add(Warning);
			}
		}
	}

	return Errors;
}

// ============================================================================
// Suggestion Generation
// ============================================================================

FString FOliveCompileManager::GenerateSuggestion(const FOliveIRCompileError& Error) const
{
	FString SourcePin, TargetPin, Reason;
	FString VarName, FuncName, ClassName;
	FString ExpectedType, ActualType;
	FString PinName, NodeType;

	// Try each pattern matcher and generate appropriate suggestion
	if (MatchPinConnectionError(Error.Message, SourcePin, TargetPin, Reason))
	{
		return FString::Printf(
			TEXT("Check pin types: ensure '%s' is compatible with '%s'. Reason: %s. Consider using a Cast or conversion node."),
			*SourcePin, *TargetPin, *Reason);
	}

	if (MatchMissingVariableError(Error.Message, VarName))
	{
		return FString::Printf(
			TEXT("Variable '%s' not found. Use blueprint.add_variable to create it first, or check spelling."),
			*VarName);
	}

	if (MatchMissingFunctionError(Error.Message, FuncName, ClassName))
	{
		if (ClassName.IsEmpty())
		{
			return FString::Printf(
				TEXT("Function '%s' not found. Check spelling or use node catalog to find the correct function name."),
				*FuncName);
		}
		else
		{
			return FString::Printf(
				TEXT("Function '%s' not found in class '%s'. Verify the target class has this function or check parent classes."),
				*FuncName, *ClassName);
		}
	}

	if (MatchTypeError(Error.Message, ExpectedType, ActualType))
	{
		return FString::Printf(
			TEXT("Type mismatch: expected '%s' but got '%s'. Use a Cast node or appropriate conversion function."),
			*ExpectedType, *ActualType);
	}

	if (MatchUnconnectedPinError(Error.Message, PinName))
	{
		return FString::Printf(
			TEXT("Pin '%s' requires a connection. Connect an appropriate node or provide a default value."),
			*PinName);
	}

	if (MatchCircularDependencyError(Error.Message))
	{
		return TEXT("Graph contains a circular reference. Break the loop by restructuring the logic, using events, delays, or storing intermediate results in variables.");
	}

	if (MatchDeprecatedNodeError(Error.Message, NodeType))
	{
		if (NodeType.IsEmpty())
		{
			return TEXT("This node is deprecated. Replace it with the recommended alternative shown in the node tooltip or documentation.");
		}
		else
		{
			return FString::Printf(
				TEXT("Node '%s' is deprecated. Replace it with the recommended alternative."),
				*NodeType);
		}
	}

	// Generic suggestions based on common keywords
	const FString LowerMessage = Error.Message.ToLower();

	if (LowerMessage.Contains(TEXT("is not connected")))
	{
		return TEXT("One or more required pins are not connected. Connect all required input pins to appropriate sources.");
	}

	if (LowerMessage.Contains(TEXT("invalid")) || LowerMessage.Contains(TEXT("illegal")))
	{
		return TEXT("The operation or value is invalid in this context. Review the node configuration and input values.");
	}

	if (LowerMessage.Contains(TEXT("access none")) || LowerMessage.Contains(TEXT("accessed none")))
	{
		return TEXT("Attempting to access a null reference. Add an IsValid check before accessing the object or ensure the reference is properly set.");
	}

	if (LowerMessage.Contains(TEXT("self")) && LowerMessage.Contains(TEXT("target")))
	{
		return TEXT("The target for this call is invalid. Ensure the node is being called on the correct object context.");
	}

	if (LowerMessage.Contains(TEXT("array")) && LowerMessage.Contains(TEXT("out of bounds")))
	{
		return TEXT("Array access is out of bounds. Add a length check before accessing array elements.");
	}

	if (LowerMessage.Contains(TEXT("replication")) || LowerMessage.Contains(TEXT("replicate")))
	{
		return TEXT("Replication error. Ensure proper server/client context and that replicated variables are properly configured.");
	}

	if (LowerMessage.Contains(TEXT("interface")))
	{
		return TEXT("Interface-related error. Ensure the target object implements the interface and the interface function signature matches.");
	}

	if (LowerMessage.Contains(TEXT("signature")) || LowerMessage.Contains(TEXT("override")))
	{
		return TEXT("Function signature mismatch. Ensure all parameter types and return types match the parent or interface definition.");
	}

	if (LowerMessage.Contains(TEXT("latent")) || LowerMessage.Contains(TEXT("delay")))
	{
		return TEXT("Latent action error. Ensure latent actions (Delay, etc.) are used in appropriate contexts and not in pure functions.");
	}

	if (LowerMessage.Contains(TEXT("const")) || LowerMessage.Contains(TEXT("read only")))
	{
		return TEXT("Attempting to modify a const or read-only value. Create a local copy to modify or use an appropriate setter function.");
	}

	// Default generic suggestion
	return TEXT("Review the graph for issues. Check the Output Log for additional details and ensure all pins are properly connected with compatible types.");
}

// ============================================================================
// Parsing Methods
// ============================================================================

void FOliveCompileManager::ParseCompileLog(const UBlueprint* Blueprint, FOliveIRCompileResult& OutResult) const
{
	if (!Blueprint)
	{
		return;
	}

	// Check Blueprint compile status for overall state
	switch (Blueprint->Status)
	{
	case BS_Error:
		// Errors will be extracted from node messages
		break;
	case BS_UpToDateWithWarnings:
		// Warnings will be extracted from node messages
		break;
	case BS_UpToDate:
		// Successfully compiled
		break;
	case BS_Dirty:
	case BS_Unknown:
		OutResult.Warnings.Add(FOliveIRCompileError::MakeWarning(
			TEXT("Blueprint may require recompilation"),
			TEXT("Blueprint status indicates it may not be up to date.")));
		break;
	default:
		break;
	}
}

FOliveIRCompileError FOliveCompileManager::ParseErrorMessage(const FString& Message, const UBlueprint* Blueprint) const
{
	FOliveIRCompileError Error;
	Error.Message = Message;
	Error.Severity = EOliveIRCompileErrorSeverity::Error;

	// Try to find the associated node
	UEdGraphNode* Node = FindErrorNode(Message, Blueprint);
	if (Node)
	{
		Error.NodeId = Node->GetFName().ToString();
		Error.NodeName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

		// Find which graph contains the node
		if (UEdGraph* Graph = Node->GetGraph())
		{
			Error.GraphName = Graph->GetName();
		}
	}

	// Generate suggestion based on the error
	Error.Suggestion = GenerateSuggestion(Error);

	return Error;
}

UEdGraphNode* FOliveCompileManager::FindErrorNode(const FString& ErrorMessage, const UBlueprint* Blueprint) const
{
	if (!Blueprint)
	{
		return nullptr;
	}

	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			// Check if this node has a matching error message
			if (Node->ErrorMsg.Contains(ErrorMessage) || ErrorMessage.Contains(Node->ErrorMsg))
			{
				return Node;
			}

			// Also check if the error message references this node's name
			FString NodeName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			if (!NodeName.IsEmpty() && ErrorMessage.Contains(NodeName))
			{
				return Node;
			}
		}
	}

	return nullptr;
}

void FOliveCompileManager::ExtractNodeErrors(const UBlueprint* Blueprint, FOliveIRCompileResult& OutResult) const
{
	if (!Blueprint)
	{
		return;
	}

	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);

	// Track nodes we've already processed to avoid duplicates
	TSet<FString> ProcessedNodeIds;

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			FString NodeId = Node->GetFName().ToString();

			// Skip if already processed
			if (ProcessedNodeIds.Contains(NodeId))
			{
				continue;
			}

			// Check for errors
			if (Node->ErrorType == EMessageSeverity::Error)
			{
				FOliveIRCompileError Error;
				Error.Message = Node->ErrorMsg.IsEmpty() ?
					FString::Printf(TEXT("Error in node: %s"), *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()) :
					Node->ErrorMsg;
				Error.Severity = EOliveIRCompileErrorSeverity::Error;
				Error.GraphName = Graph->GetName();
				Error.NodeId = NodeId;
				Error.NodeName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				Error.Suggestion = GenerateSuggestion(Error);

				OutResult.Errors.Add(Error);
				ProcessedNodeIds.Add(NodeId);
			}
			// Check for warnings
			else if (Node->ErrorType == EMessageSeverity::Warning)
			{
				FOliveIRCompileError Warning;
				Warning.Message = Node->ErrorMsg.IsEmpty() ?
					FString::Printf(TEXT("Warning in node: %s"), *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()) :
					Node->ErrorMsg;
				Warning.Severity = EOliveIRCompileErrorSeverity::Warning;
				Warning.GraphName = Graph->GetName();
				Warning.NodeId = NodeId;
				Warning.NodeName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				Warning.Suggestion = GenerateSuggestion(Warning);

				OutResult.Warnings.Add(Warning);
				ProcessedNodeIds.Add(NodeId);
			}

			// Check K2Node specific error state
			if (UK2Node* K2Node = Cast<UK2Node>(Node))
			{
				// K2Nodes may have additional error tracking (UE 5.5: bHasCompilerMessage is a public member)
				if (K2Node->bHasCompilerMessage && !ProcessedNodeIds.Contains(NodeId))
				{
					FOliveIRCompileError Error;
					Error.Message = Node->ErrorMsg.IsEmpty() ?
						TEXT("Compiler error in K2 node") :
						Node->ErrorMsg;
					Error.Severity = (Node->ErrorType == EMessageSeverity::Error) ?
						EOliveIRCompileErrorSeverity::Error :
						EOliveIRCompileErrorSeverity::Warning;
					Error.GraphName = Graph->GetName();
					Error.NodeId = NodeId;
					Error.NodeName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
					Error.Suggestion = GenerateSuggestion(Error);

					if (Error.Severity == EOliveIRCompileErrorSeverity::Error)
					{
						OutResult.Errors.Add(Error);
					}
					else
					{
						OutResult.Warnings.Add(Error);
					}
					ProcessedNodeIds.Add(NodeId);
				}
			}
		}
	}
}

void FOliveCompileManager::ExtractCompilerLogErrors(const FCompilerResultsLog& CompilerLog, FOliveIRCompileResult& OutResult) const
{
	// Iterate all messages captured by the compiler results log.
	// Many of these will duplicate errors already found via per-node extraction
	// (ExtractNodeErrors), so we deduplicate by checking if each message text
	// is already present (substring match) in an existing error.
	for (const TSharedRef<FTokenizedMessage>& Msg : CompilerLog.Messages)
	{
		const EMessageSeverity::Type Severity = Msg->GetSeverity();

		// Only capture errors and warnings
		if (Severity != EMessageSeverity::Error && Severity != EMessageSeverity::Warning
			&& Severity != EMessageSeverity::PerformanceWarning)
		{
			continue;
		}

		const FString MsgText = Msg->ToText().ToString();
		if (MsgText.IsEmpty())
		{
			continue;
		}

		// Deduplicate: check if this message is already captured by per-node extraction.
		// Use bidirectional substring containment to handle cases where the compiler log
		// message is slightly longer/shorter than the node-level ErrorMsg.
		const TArray<FOliveIRCompileError>& ExistingList =
			(Severity == EMessageSeverity::Error) ? OutResult.Errors : OutResult.Warnings;

		bool bAlreadyCaptured = false;
		for (const FOliveIRCompileError& Existing : ExistingList)
		{
			if (Existing.Message.Contains(MsgText) || MsgText.Contains(Existing.Message))
			{
				bAlreadyCaptured = true;
				break;
			}
		}

		if (bAlreadyCaptured)
		{
			continue;
		}

		FOliveIRCompileError NewError;
		NewError.Message = MsgText;
		NewError.Severity = (Severity == EMessageSeverity::Error)
			? EOliveIRCompileErrorSeverity::Error
			: EOliveIRCompileErrorSeverity::Warning;
		NewError.Suggestion = GenerateSuggestion(NewError);

		if (Severity == EMessageSeverity::Error)
		{
			OutResult.Errors.Add(MoveTemp(NewError));

			UE_LOG(LogOliveCompile, Log,
				TEXT("Captured compiler-log error (not on any node): %s"), *MsgText);
		}
		else
		{
			OutResult.Warnings.Add(MoveTemp(NewError));
		}
	}
}

// ============================================================================
// Error Pattern Matchers
// ============================================================================

bool FOliveCompileManager::MatchPinConnectionError(const FString& Message, FString& OutSourcePin, FString& OutTargetPin, FString& OutReason) const
{
	// Pattern: "Cannot connect X to Y: reason" or "Cannot connect 'X' to 'Y': reason"
	// Using simple string parsing as FRegexMatcher may not be available in all engine versions

	int32 CannotConnectIndex = Message.Find(TEXT("Cannot connect"), ESearchCase::IgnoreCase);
	if (CannotConnectIndex == INDEX_NONE)
	{
		// Also try "can't connect" variant
		CannotConnectIndex = Message.Find(TEXT("can't connect"), ESearchCase::IgnoreCase);
	}

	if (CannotConnectIndex == INDEX_NONE)
	{
		return false;
	}

	// Try to extract pin names
	int32 ToIndex = Message.Find(TEXT(" to "), ESearchCase::IgnoreCase, ESearchDir::FromStart, CannotConnectIndex);
	if (ToIndex == INDEX_NONE)
	{
		return false;
	}

	// Extract source pin (between "connect" and "to")
	int32 ConnectEnd = CannotConnectIndex + (Message.Contains(TEXT("can't")) ? 13 : 14); // Length of "cannot connect" or "can't connect"
	FString SourcePart = Message.Mid(ConnectEnd, ToIndex - ConnectEnd).TrimStartAndEnd();

	// Remove quotes if present
	OutSourcePin = SourcePart.Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT(""));

	// Find reason (after colon)
	int32 ColonIndex = Message.Find(TEXT(":"), ESearchCase::IgnoreCase, ESearchDir::FromStart, ToIndex);

	if (ColonIndex != INDEX_NONE)
	{
		// Extract target pin (between "to" and ":")
		FString TargetPart = Message.Mid(ToIndex + 4, ColonIndex - ToIndex - 4).TrimStartAndEnd();
		OutTargetPin = TargetPart.Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT(""));

		// Extract reason
		OutReason = Message.Mid(ColonIndex + 1).TrimStartAndEnd();
	}
	else
	{
		// No colon, just extract target
		FString TargetPart = Message.Mid(ToIndex + 4).TrimStartAndEnd();
		OutTargetPin = TargetPart.Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT(""));
		OutReason = TEXT("Incompatible types");
	}

	return !OutSourcePin.IsEmpty() && !OutTargetPin.IsEmpty();
}

bool FOliveCompileManager::MatchMissingVariableError(const FString& Message, FString& OutVariableName) const
{
	// Pattern: "Variable 'X' not found" or "Unknown variable X" or "variable X does not exist"
	const FString LowerMessage = Message.ToLower();

	// Look for "variable" keyword
	int32 VarIndex = LowerMessage.Find(TEXT("variable"));
	if (VarIndex == INDEX_NONE)
	{
		return false;
	}

	// Check for common error phrases
	if (!LowerMessage.Contains(TEXT("not found")) &&
		!LowerMessage.Contains(TEXT("unknown")) &&
		!LowerMessage.Contains(TEXT("does not exist")) &&
		!LowerMessage.Contains(TEXT("could not find")) &&
		!LowerMessage.Contains(TEXT("undefined")))
	{
		return false;
	}

	// Extract variable name - look for quoted name or word after "variable"
	int32 QuoteStart = Message.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, VarIndex);
	if (QuoteStart != INDEX_NONE)
	{
		int32 QuoteEnd = Message.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);
		if (QuoteEnd != INDEX_NONE)
		{
			OutVariableName = Message.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			return !OutVariableName.IsEmpty();
		}
	}

	// Try double quotes
	QuoteStart = Message.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, VarIndex);
	if (QuoteStart != INDEX_NONE)
	{
		int32 QuoteEnd = Message.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);
		if (QuoteEnd != INDEX_NONE)
		{
			OutVariableName = Message.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			return !OutVariableName.IsEmpty();
		}
	}

	// Extract word after "variable"
	FString AfterVariable = Message.Mid(VarIndex + 8).TrimStartAndEnd();
	TArray<FString> Words;
	AfterVariable.ParseIntoArray(Words, TEXT(" "), true);
	if (Words.Num() > 0)
	{
		OutVariableName = Words[0].TrimStartAndEnd();
		// Clean up any trailing punctuation
		OutVariableName = OutVariableName.Replace(TEXT(","), TEXT("")).Replace(TEXT("."), TEXT(""));
		return !OutVariableName.IsEmpty();
	}

	return false;
}

bool FOliveCompileManager::MatchMissingFunctionError(const FString& Message, FString& OutFunctionName, FString& OutClassName) const
{
	// Patterns:
	// "Function 'X' not found"
	// "No function named X in class Y"
	// "Unable to find function X"
	// "Unknown function X"
	const FString LowerMessage = Message.ToLower();

	// Check for function-related error keywords
	bool bIsFunctionError = LowerMessage.Contains(TEXT("function")) &&
		(LowerMessage.Contains(TEXT("not found")) ||
		 LowerMessage.Contains(TEXT("unknown")) ||
		 LowerMessage.Contains(TEXT("unable to find")) ||
		 LowerMessage.Contains(TEXT("does not exist")) ||
		 LowerMessage.Contains(TEXT("could not find")));

	if (!bIsFunctionError)
	{
		return false;
	}

	// Extract function name from quotes
	int32 QuoteStart = Message.Find(TEXT("'"));
	if (QuoteStart != INDEX_NONE)
	{
		int32 QuoteEnd = Message.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);
		if (QuoteEnd != INDEX_NONE)
		{
			OutFunctionName = Message.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
		}
	}

	// If no quoted name, try to extract from common patterns
	if (OutFunctionName.IsEmpty())
	{
		int32 FuncIndex = LowerMessage.Find(TEXT("function"));
		if (FuncIndex != INDEX_NONE)
		{
			FString AfterFunc = Message.Mid(FuncIndex + 8).TrimStartAndEnd();
			TArray<FString> Words;
			AfterFunc.ParseIntoArray(Words, TEXT(" "), true);
			if (Words.Num() > 0)
			{
				OutFunctionName = Words[0].Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT(""));
			}
		}
	}

	// Try to extract class name if present
	int32 ClassIndex = LowerMessage.Find(TEXT("in class"));
	if (ClassIndex != INDEX_NONE)
	{
		FString AfterClass = Message.Mid(ClassIndex + 8).TrimStartAndEnd();

		// Check for quoted class name
		int32 ClassQuoteStart = AfterClass.Find(TEXT("'"));
		if (ClassQuoteStart != INDEX_NONE)
		{
			int32 ClassQuoteEnd = AfterClass.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, ClassQuoteStart + 1);
			if (ClassQuoteEnd != INDEX_NONE)
			{
				OutClassName = AfterClass.Mid(ClassQuoteStart + 1, ClassQuoteEnd - ClassQuoteStart - 1);
			}
		}
		else
		{
			// Just take the first word
			TArray<FString> Words;
			AfterClass.ParseIntoArray(Words, TEXT(" "), true);
			if (Words.Num() > 0)
			{
				OutClassName = Words[0].Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT(""));
			}
		}
	}

	return !OutFunctionName.IsEmpty();
}

bool FOliveCompileManager::MatchTypeError(const FString& Message, FString& OutExpectedType, FString& OutActualType) const
{
	// Patterns:
	// "Expected type X but got Y"
	// "Type mismatch: expected X, got Y"
	// "Cannot convert from X to Y"
	// "Incompatible types: X and Y"
	const FString LowerMessage = Message.ToLower();

	// Check for type error keywords
	bool bIsTypeError =
		(LowerMessage.Contains(TEXT("type")) &&
		 (LowerMessage.Contains(TEXT("mismatch")) || LowerMessage.Contains(TEXT("expected")))) ||
		(LowerMessage.Contains(TEXT("cannot convert"))) ||
		(LowerMessage.Contains(TEXT("incompatible")));

	if (!bIsTypeError)
	{
		return false;
	}

	// Try "expected X but got Y" pattern
	int32 ExpectedIndex = LowerMessage.Find(TEXT("expected"));
	int32 GotIndex = LowerMessage.Find(TEXT("got"));
	int32 ButIndex = LowerMessage.Find(TEXT("but"));

	if (ExpectedIndex != INDEX_NONE && (GotIndex != INDEX_NONE || ButIndex != INDEX_NONE))
	{
		int32 EndOfExpected = (ButIndex != INDEX_NONE) ? ButIndex : GotIndex;

		// Extract expected type
		FString ExpectedPart = Message.Mid(ExpectedIndex + 8, EndOfExpected - ExpectedIndex - 8).TrimStartAndEnd();
		OutExpectedType = ExpectedPart.Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT("")).Replace(TEXT(","), TEXT(""));

		// Extract actual type
		if (GotIndex != INDEX_NONE)
		{
			FString GotPart = Message.Mid(GotIndex + 3).TrimStartAndEnd();
			TArray<FString> Words;
			GotPart.ParseIntoArray(Words, TEXT(" "), true);
			if (Words.Num() > 0)
			{
				OutActualType = Words[0].Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT(""));
			}
		}

		return !OutExpectedType.IsEmpty();
	}

	// Try "cannot convert from X to Y" pattern
	int32 FromIndex = LowerMessage.Find(TEXT("from"));
	int32 ToIndex = LowerMessage.Find(TEXT(" to "), ESearchCase::IgnoreCase, ESearchDir::FromStart, FromIndex);

	if (FromIndex != INDEX_NONE && ToIndex != INDEX_NONE)
	{
		// Source type is between "from" and "to"
		FString FromPart = Message.Mid(FromIndex + 4, ToIndex - FromIndex - 4).TrimStartAndEnd();
		OutActualType = FromPart.Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT(""));

		// Target type is after "to"
		FString ToPart = Message.Mid(ToIndex + 4).TrimStartAndEnd();
		TArray<FString> Words;
		ToPart.ParseIntoArray(Words, TEXT(" "), true);
		if (Words.Num() > 0)
		{
			OutExpectedType = Words[0].Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT(""));
		}

		return !OutExpectedType.IsEmpty() || !OutActualType.IsEmpty();
	}

	return false;
}

bool FOliveCompileManager::MatchUnconnectedPinError(const FString& Message, FString& OutPinName) const
{
	// Patterns:
	// "Pin 'X' is not connected"
	// "Required pin X is not connected"
	// "X is not connected"
	const FString LowerMessage = Message.ToLower();

	if (!LowerMessage.Contains(TEXT("not connected")) && !LowerMessage.Contains(TEXT("unconnected")))
	{
		return false;
	}

	// Try to extract quoted pin name
	int32 QuoteStart = Message.Find(TEXT("'"));
	if (QuoteStart != INDEX_NONE)
	{
		int32 QuoteEnd = Message.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);
		if (QuoteEnd != INDEX_NONE)
		{
			OutPinName = Message.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			return !OutPinName.IsEmpty();
		}
	}

	// Try "Pin X is not connected" pattern
	int32 PinIndex = LowerMessage.Find(TEXT("pin"));
	if (PinIndex != INDEX_NONE)
	{
		FString AfterPin = Message.Mid(PinIndex + 3).TrimStartAndEnd();

		// Find "is not connected" to know where pin name ends
		int32 IsIndex = AfterPin.ToLower().Find(TEXT("is"));
		if (IsIndex != INDEX_NONE)
		{
			OutPinName = AfterPin.Left(IsIndex).TrimStartAndEnd();
			OutPinName = OutPinName.Replace(TEXT("'"), TEXT("")).Replace(TEXT("\""), TEXT(""));
			return !OutPinName.IsEmpty();
		}
	}

	return false;
}

bool FOliveCompileManager::MatchCircularDependencyError(const FString& Message) const
{
	const FString LowerMessage = Message.ToLower();

	return LowerMessage.Contains(TEXT("circular")) ||
		   LowerMessage.Contains(TEXT("cycle detected")) ||
		   LowerMessage.Contains(TEXT("cyclic")) ||
		   (LowerMessage.Contains(TEXT("loop")) && LowerMessage.Contains(TEXT("dependency")));
}

bool FOliveCompileManager::MatchDeprecatedNodeError(const FString& Message, FString& OutNodeType) const
{
	const FString LowerMessage = Message.ToLower();

	if (!LowerMessage.Contains(TEXT("deprecated")))
	{
		return false;
	}

	// Try to extract the deprecated item name from quotes
	int32 QuoteStart = Message.Find(TEXT("'"));
	if (QuoteStart != INDEX_NONE)
	{
		int32 QuoteEnd = Message.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromStart, QuoteStart + 1);
		if (QuoteEnd != INDEX_NONE)
		{
			OutNodeType = Message.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
		}
	}

	return true;
}

// ============================================================================
// Helper Methods
// ============================================================================

UBlueprint* FOliveCompileManager::LoadBlueprint(const FString& AssetPath) const
{
	// Normalize the path
	FString NormalizedPath = AssetPath;
	if (!NormalizedPath.StartsWith(TEXT("/")))
	{
		NormalizedPath = TEXT("/") + NormalizedPath;
	}

	// Try to load the Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *NormalizedPath);
	if (!Blueprint)
	{
		// Try with _C suffix removed if it's a class path
		FString CleanPath = NormalizedPath;
		if (CleanPath.EndsWith(TEXT("_C")))
		{
			CleanPath.LeftChopInline(2);
			Blueprint = LoadObject<UBlueprint>(nullptr, *CleanPath);
		}
	}

	return Blueprint;
}

bool FOliveCompileManager::IsPIEActive() const
{
	return GEditor && GEditor->PlayWorld != nullptr;
}
