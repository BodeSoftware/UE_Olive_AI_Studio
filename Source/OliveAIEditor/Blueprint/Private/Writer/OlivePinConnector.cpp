// Copyright Bode Software. All Rights Reserved.

#include "OlivePinConnector.h"
#include "Services/OliveTransactionManager.h"

// Blueprint/Graph includes
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

// Utility includes
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_Knot.h"

DEFINE_LOG_CATEGORY_STATIC(LogOlivePinConnector, Log, All);

namespace
{
	static void RefreshBlueprintEditorState(UBlueprint* Blueprint, const UEdGraphPin* Pin)
	{
		if (const UEdGraphNode* Node = Pin ? Pin->GetOwningNode() : nullptr)
		{
			if (UEdGraph* Graph = Node->GetGraph())
			{
				Graph->NotifyGraphChanged();
			}
		}

		if (Blueprint)
		{
			Blueprint->BroadcastChanged();
		}
	}

	static void NotifyGraphChangedForPin(const UEdGraphPin* Pin)
	{
		RefreshBlueprintEditorState(
			Pin ? FBlueprintEditorUtils::FindBlueprintForNode(Pin->GetOwningNode()) : nullptr,
			Pin);
	}
}

// ============================================================================
// FOlivePinConnector Singleton
// ============================================================================

FOlivePinConnector& FOlivePinConnector::Get()
{
	static FOlivePinConnector Instance;
	return Instance;
}

// ============================================================================
// Connection Operations
// ============================================================================

FOliveBlueprintWriteResult FOlivePinConnector::Connect(
	UEdGraphPin* SourcePin,
	UEdGraphPin* TargetPin,
	bool bAllowConversion)
{
	// Validate pins
	FString ValidationError;
	if (!ValidatePin(SourcePin, ValidationError))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid source pin: %s"), *ValidationError));
	}

	if (!ValidatePin(TargetPin, ValidationError))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid target pin: %s"), *ValidationError));
	}

	// Get asset path for result reporting
	FString AssetPath = GetAssetPathForPin(SourcePin);

	// Get the K2 schema
	const UEdGraphSchema_K2* K2Schema = GetK2Schema();
	if (!K2Schema)
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Failed to get K2 schema"));
	}

	// Check if connection is possible and classify the response
	FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);

	const bool bNeedsConversion =
		(Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE);

	// Detect BREAK_OTHERS responses (pin already has a connection that would be replaced)
	const bool bBreakOthers =
		(Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A
		 || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B
		 || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB);

	// Allow break-others for exec pins only (matches Blueprint editor drag-and-drop behavior).
	// Exec pins are single-output by design, so replacing is always intentional.
	// Do NOT auto-break for data pins — that could silently disconnect important wires.
	const bool bExecAutoBreak = bBreakOthers
		&& SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
		&& TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;

	const bool bCanConnect =
		Response.CanSafeConnect() || bNeedsConversion || bExecAutoBreak;

	if (bExecAutoBreak)
	{
		UE_LOG(LogOlivePinConnector, Log,
			TEXT("Exec auto-break: breaking existing connection on %s.%s to make new connection to %s.%s"),
			*SourcePin->GetOwningNode()->GetName(), *SourcePin->PinName.ToString(),
			*TargetPin->GetOwningNode()->GetName(), *TargetPin->PinName.ToString());
	}

	if (!bCanConnect
		&& SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
		&& TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		UE_LOG(LogOlivePinConnector, Warning,
			TEXT("EXEC WIRE REJECTED: %s.%s -> %s.%s | Response: %d '%s' | "
				 "Src(orphan=%d hidden=%d links=%d dir=%d) "
				 "Tgt(orphan=%d hidden=%d links=%d dir=%d)"),
			*SourcePin->GetOwningNode()->GetName(), *SourcePin->PinName.ToString(),
			*TargetPin->GetOwningNode()->GetName(), *TargetPin->PinName.ToString(),
			(int)Response.Response, *Response.Message.ToString(),
			SourcePin->bOrphanedPin, SourcePin->bHidden, SourcePin->LinkedTo.Num(), (int)SourcePin->Direction,
			TargetPin->bOrphanedPin, TargetPin->bHidden, TargetPin->LinkedTo.Num(), (int)TargetPin->Direction);
	}

	if (!bCanConnect)
	{
		// Build structured diagnostic for type-incompatible connections
		FOliveWiringDiagnostic Diagnostic = BuildWiringDiagnostic(SourcePin, TargetPin, Response);
		FOliveBlueprintWriteResult ErrorResult = FOliveBlueprintWriteResult::Error(
			Diagnostic.ToHumanReadable(), AssetPath);
		ErrorResult.WiringDiagnostic = MoveTemp(Diagnostic);
		return ErrorResult;
	}

	if (bNeedsConversion && !bAllowConversion)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Type conversion needed (%s -> %s) but not allowed. "
				"Pass bAllowConversion=true or use compatible types."),
				*GetPinTypeDescription(SourcePin->PinType),
				*GetPinTypeDescription(TargetPin->PinType)),
			AssetPath);
	}

	UBlueprint* Blueprint = GetOwningBlueprint(SourcePin);

	// Create transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OlivePinConnector", "ConnectPins", "Connect Pins: {0} -> {1}"),
		FText::FromString(SourcePin->GetName()),
		FText::FromString(TargetPin->GetName())));

	if (Blueprint)
	{
		Blueprint->Modify();
	}

	// TryCreateConnection handles everything: direct wires, promotions,
	// AND conversion node insertion (for MAKE_WITH_CONVERSION_NODE).
	bool bSuccess = K2Schema->TryCreateConnection(SourcePin, TargetPin);

	if (bSuccess)
	{
		if (Blueprint)
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		RefreshBlueprintEditorState(Blueprint, SourcePin);

		UE_LOG(LogOlivePinConnector, Log, TEXT("Connected pins: %s -> %s%s"),
			*SourcePin->GetName(), *TargetPin->GetName(),
			bNeedsConversion ? TEXT(" (with conversion)") : TEXT(""));

		FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath);

		// Flag that conversion was inserted (caller can detect the intermediate node)
		if (bNeedsConversion)
		{
			Result.AddWarning(FString::Printf(
				TEXT("Auto-conversion inserted: %s -> %s"),
				*GetPinTypeDescription(SourcePin->PinType),
				*GetPinTypeDescription(TargetPin->PinType)));
		}

		return Result;
	}
	else
	{
		// TryCreateConnection failed even though CanCreateConnection said it was OK.
		// Re-probe to get a fresh response for the diagnostic.
		FPinConnectionResponse FailResponse = K2Schema->CanCreateConnection(SourcePin, TargetPin);
		FOliveWiringDiagnostic Diagnostic = BuildWiringDiagnostic(SourcePin, TargetPin, FailResponse);
		Diagnostic.WhyAutoFixFailed = TEXT("TryCreateConnection returned false unexpectedly despite CanCreateConnection passing");
		FOliveBlueprintWriteResult ErrorResult = FOliveBlueprintWriteResult::Error(
			Diagnostic.ToHumanReadable(), AssetPath);
		ErrorResult.WiringDiagnostic = MoveTemp(Diagnostic);
		return ErrorResult;
	}
}

bool FOlivePinConnector::CanConnect(
	const UEdGraphPin* SourcePin,
	const UEdGraphPin* TargetPin,
	FString& OutReason) const
{
	// Validate pins
	FString ValidationError;
	if (!ValidatePin(SourcePin, ValidationError))
	{
		OutReason = FString::Printf(TEXT("Invalid source pin: %s"), *ValidationError);
		return false;
	}

	if (!ValidatePin(TargetPin, ValidationError))
	{
		OutReason = FString::Printf(TEXT("Invalid target pin: %s"), *ValidationError);
		return false;
	}

	// Get the K2 schema
	const UEdGraphSchema_K2* K2Schema = GetK2Schema();
	if (!K2Schema)
	{
		OutReason = TEXT("Failed to get K2 schema");
		return false;
	}

	// Check connection validity (including autocast-compatible pairs)
	FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);

	if (Response.CanSafeConnect() ||
		Response.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE)
	{
		return true;
	}
	else
	{
		OutReason = Response.Message.ToString();
		return false;
	}
}

// ============================================================================
// Batch Operations
// ============================================================================

FOliveBlueprintWriteResult FOlivePinConnector::DisconnectAll(UEdGraphPin* Pin)
{
	FString ValidationError;
	if (!ValidatePin(Pin, ValidationError))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid pin: %s"), *ValidationError));
	}

	FString AssetPath = GetAssetPathForPin(Pin);

	if (Pin->LinkedTo.Num() == 0)
	{
		FOliveBlueprintWriteResult Result = FOliveBlueprintWriteResult::Success(AssetPath);
		Result.AddWarning(TEXT("Pin has no connections to break"));
		return Result;
	}

	UBlueprint* Blueprint = GetOwningBlueprint(Pin);

	// Create transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OlivePinConnector", "DisconnectAll", "Disconnect All: {0}"),
		FText::FromString(Pin->GetName())));

	if (Blueprint)
	{
		Blueprint->Modify();
	}

	const UEdGraphSchema_K2* K2Schema = GetK2Schema();

	// Break all connections
	int32 NumBroken = Pin->LinkedTo.Num();
	Pin->BreakAllPinLinks();

	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	RefreshBlueprintEditorState(Blueprint, Pin);

	UE_LOG(LogOlivePinConnector, Log, TEXT("Disconnected %d connections from pin %s"),
		NumBroken, *Pin->GetName());

	return FOliveBlueprintWriteResult::Success(AssetPath);
}

FOliveBlueprintWriteResult FOlivePinConnector::Disconnect(
	UEdGraphPin* PinA,
	UEdGraphPin* PinB)
{
	FString ValidationError;
	if (!ValidatePin(PinA, ValidationError))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid pin A: %s"), *ValidationError));
	}

	if (!ValidatePin(PinB, ValidationError))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Invalid pin B: %s"), *ValidationError));
	}

	FString AssetPath = GetAssetPathForPin(PinA);

	// Check if they are actually connected
	if (!PinA->LinkedTo.Contains(PinB))
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Pins are not connected"), AssetPath);
	}

	UBlueprint* Blueprint = GetOwningBlueprint(PinA);

	// Create transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OlivePinConnector", "Disconnect", "Disconnect: {0} <-> {1}"),
		FText::FromString(PinA->GetName()),
		FText::FromString(PinB->GetName())));

	if (Blueprint)
	{
		Blueprint->Modify();
	}

	// Break the specific connection
	PinA->BreakLinkTo(PinB);

	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}

	RefreshBlueprintEditorState(Blueprint, PinA);

	UE_LOG(LogOlivePinConnector, Log, TEXT("Disconnected pins: %s <-> %s"),
		*PinA->GetName(), *PinB->GetName());

	return FOliveBlueprintWriteResult::Success(AssetPath);
}

// ============================================================================
// Pin Queries
// ============================================================================

TArray<UEdGraphPin*> FOlivePinConnector::GetConnectedPins(const UEdGraphPin* Pin) const
{
	TArray<UEdGraphPin*> Result;

	if (Pin)
	{
		Result = Pin->LinkedTo;
	}

	return Result;
}

bool FOlivePinConnector::HasConnections(const UEdGraphPin* Pin) const
{
	return Pin && Pin->LinkedTo.Num() > 0;
}

int32 FOlivePinConnector::GetConnectionCount(const UEdGraphPin* Pin) const
{
	return Pin ? Pin->LinkedTo.Num() : 0;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

const UEdGraphSchema_K2* FOlivePinConnector::GetK2Schema() const
{
	return GetDefault<UEdGraphSchema_K2>();
}

FString FOlivePinConnector::GetPinTypeDescription(const FEdGraphPinType& PinType) const
{
	FString Description;

	// Get the category name
	Description = PinType.PinCategory.ToString();

	// Add subcategory object name if present
	if (PinType.PinSubCategoryObject.IsValid())
	{
		Description += TEXT("_") + PinType.PinSubCategoryObject->GetName();
	}

	// Add container type
	if (PinType.ContainerType == EPinContainerType::Array)
	{
		Description = TEXT("Array_") + Description;
	}
	else if (PinType.ContainerType == EPinContainerType::Set)
	{
		Description = TEXT("Set_") + Description;
	}
	else if (PinType.ContainerType == EPinContainerType::Map)
	{
		Description = TEXT("Map_") + Description;
	}

	// Note if reference
	if (PinType.bIsReference)
	{
		Description += TEXT("_Ref");
	}

	return Description;
}

bool FOlivePinConnector::ValidatePin(const UEdGraphPin* Pin, FString& OutError) const
{
	if (!Pin)
	{
		OutError = TEXT("Pin is null");
		return false;
	}

	UEdGraphNode* OwningNode = Pin->GetOwningNode();
	if (!OwningNode)
	{
		OutError = TEXT("Pin has no owning node");
		return false;
	}

	UEdGraph* Graph = OwningNode->GetGraph();
	if (!Graph)
	{
		OutError = TEXT("Pin's node has no graph");
		return false;
	}

	return true;
}

UBlueprint* FOlivePinConnector::GetOwningBlueprint(const UEdGraphPin* Pin) const
{
	if (!Pin)
	{
		return nullptr;
	}

	UEdGraphNode* Node = Pin->GetOwningNode();
	if (!Node)
	{
		return nullptr;
	}

	UEdGraph* Graph = Node->GetGraph();
	if (!Graph)
	{
		return nullptr;
	}

	return FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
}

FString FOlivePinConnector::GetAssetPathForPin(const UEdGraphPin* Pin) const
{
	UBlueprint* Blueprint = GetOwningBlueprint(Pin);
	if (Blueprint)
	{
		return Blueprint->GetPathName();
	}
	return TEXT("");
}

// ============================================================================
// FOliveWiringDiagnostic Implementation
// ============================================================================

FString FOliveWiringDiagnostic::ReasonToString(EOliveWiringFailureReason InReason)
{
	switch (InReason)
	{
	case EOliveWiringFailureReason::TypesIncompatible:  return TEXT("TypesIncompatible");
	case EOliveWiringFailureReason::StructToScalar:     return TEXT("StructToScalar");
	case EOliveWiringFailureReason::ScalarToStruct:     return TEXT("ScalarToStruct");
	case EOliveWiringFailureReason::ObjectCastRequired: return TEXT("ObjectCastRequired");
	case EOliveWiringFailureReason::ContainerMismatch:  return TEXT("ContainerMismatch");
	case EOliveWiringFailureReason::DirectionMismatch:  return TEXT("DirectionMismatch");
	case EOliveWiringFailureReason::SameNode:           return TEXT("SameNode");
	case EOliveWiringFailureReason::AlreadyConnected:   return TEXT("AlreadyConnected");
	case EOliveWiringFailureReason::Unknown:
	default:                                            return TEXT("Unknown");
	}
}

TSharedPtr<FJsonObject> FOliveWiringDiagnostic::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("failure_reason"), ReasonToString(Reason));
	Obj->SetStringField(TEXT("source_type"), SourceTypeName);
	Obj->SetStringField(TEXT("target_type"), TargetTypeName);
	Obj->SetStringField(TEXT("source_pin"), SourcePinName);
	Obj->SetStringField(TEXT("target_pin"), TargetPinName);
	Obj->SetStringField(TEXT("schema_message"), SchemaMessage);
	Obj->SetStringField(TEXT("why_autofix_failed"), WhyAutoFixFailed);

	TArray<TSharedPtr<FJsonValue>> AltArray;
	for (const FOliveWiringAlternative& Alt : Alternatives)
	{
		TSharedPtr<FJsonObject> AltObj = MakeShared<FJsonObject>();
		AltObj->SetStringField(TEXT("label"), Alt.Label);
		AltObj->SetStringField(TEXT("action"), Alt.Action);
		AltObj->SetStringField(TEXT("confidence"), Alt.Confidence);
		AltArray.Add(MakeShared<FJsonValueObject>(AltObj));
	}
	Obj->SetArrayField(TEXT("alternatives"), AltArray);

	return Obj;
}

FString FOliveWiringDiagnostic::ToHumanReadable() const
{
	FString Result = FString::Printf(
		TEXT("Cannot connect %s to %s: %s. %s"),
		*SourceTypeName, *TargetTypeName,
		*ReasonToString(Reason),
		*WhyAutoFixFailed);

	if (Alternatives.Num() > 0)
	{
		Result += TEXT("\nAlternatives:");
		for (const FOliveWiringAlternative& Alt : Alternatives)
		{
			Result += FString::Printf(TEXT("\n- [%s] %s: %s"),
				*Alt.Confidence, *Alt.Label, *Alt.Action);
		}
	}

	return Result;
}

// ============================================================================
// Wiring Diagnostic Implementation
// ============================================================================

namespace
{
	/**
	 * Get a human-readable type name for a pin, suitable for error messages.
	 * More descriptive than GetPinTypeDescription -- includes "Object Reference" suffixes.
	 */
	FString GetReadablePinTypeName(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return TEXT("(null)");
		}

		const FName& Category = Pin->PinType.PinCategory;

		// Exec pin
		if (Category == UEdGraphSchema_K2::PC_Exec)
		{
			return TEXT("Exec");
		}

		// Struct
		if (Category == UEdGraphSchema_K2::PC_Struct)
		{
			UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
			if (Struct)
			{
				return Struct->GetName();
			}
			return TEXT("Struct");
		}

		// Object / Interface
		if (Category == UEdGraphSchema_K2::PC_Object
			|| Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_SoftObject
			|| Category == UEdGraphSchema_K2::PC_SoftClass)
		{
			UClass* ObjClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
			FString ClassName = ObjClass ? ObjClass->GetName() : TEXT("Object");
			return ClassName + TEXT(" Object Reference");
		}

		if (Category == UEdGraphSchema_K2::PC_Interface)
		{
			UClass* IntClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
			FString IntName = IntClass ? IntClass->GetName() : TEXT("Interface");
			return IntName + TEXT(" Interface");
		}

		// Real (Float/Double in UE 5.5)
		if (Category == UEdGraphSchema_K2::PC_Real)
		{
			if (Pin->PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double)
			{
				return TEXT("Double");
			}
			return TEXT("Float");
		}

		// Wildcard
		if (Category == UEdGraphSchema_K2::PC_Wildcard)
		{
			return TEXT("Wildcard");
		}

		// Boolean, Int, Int64, Name, String, Text, Byte, Enum
		if (Category == UEdGraphSchema_K2::PC_Boolean) return TEXT("Boolean");
		if (Category == UEdGraphSchema_K2::PC_Int) return TEXT("Integer");
		if (Category == UEdGraphSchema_K2::PC_Int64) return TEXT("Integer64");
		if (Category == UEdGraphSchema_K2::PC_Name) return TEXT("Name");
		if (Category == UEdGraphSchema_K2::PC_String) return TEXT("String");
		if (Category == UEdGraphSchema_K2::PC_Text) return TEXT("Text");
		if (Category == UEdGraphSchema_K2::PC_Byte)
		{
			UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
			if (Enum)
			{
				return Enum->GetName();
			}
			return TEXT("Byte");
		}
		if (Category == UEdGraphSchema_K2::PC_Enum)
		{
			UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
			if (Enum)
			{
				return Enum->GetName();
			}
			return TEXT("Enum");
		}

		// Fallback
		return Category.ToString();
	}

	/**
	 * Check if a pin category is a scalar numeric type.
	 */
	bool IsScalarNumeric(const FName& Category, const FName& SubCategory)
	{
		if (Category == UEdGraphSchema_K2::PC_Real
			|| Category == UEdGraphSchema_K2::PC_Int
			|| Category == UEdGraphSchema_K2::PC_Int64
			|| Category == UEdGraphSchema_K2::PC_Byte)
		{
			return true;
		}
		return false;
	}

	/**
	 * Check if a pin type is an object/class reference (including interface).
	 */
	bool IsObjectType(const FName& Category)
	{
		return Category == UEdGraphSchema_K2::PC_Object
			|| Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_SoftObject
			|| Category == UEdGraphSchema_K2::PC_SoftClass
			|| Category == UEdGraphSchema_K2::PC_Interface;
	}

	/**
	 * Get the known sub-pin suffixes for a struct type, if splittable.
	 * Returns empty if not a known splittable struct.
	 */
	TArray<FString> GetKnownStructSubPins(const FString& StructName)
	{
		static const TMap<FString, TArray<FString>> KnownStructs = []()
		{
			TMap<FString, TArray<FString>> Map;
			Map.Add(TEXT("Vector"),      { TEXT("X"), TEXT("Y"), TEXT("Z") });
			Map.Add(TEXT("Vector2D"),    { TEXT("X"), TEXT("Y") });
			Map.Add(TEXT("Rotator"),     { TEXT("Roll"), TEXT("Pitch"), TEXT("Yaw") });
			Map.Add(TEXT("LinearColor"), { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") });
			Map.Add(TEXT("Color"),       { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") });
			return Map;
		}();

		const TArray<FString>* Found = KnownStructs.Find(StructName);
		return Found ? *Found : TArray<FString>();
	}
}

FOliveWiringDiagnostic FOlivePinConnector::BuildWiringDiagnostic(
	const UEdGraphPin* SourcePin,
	const UEdGraphPin* TargetPin,
	const FPinConnectionResponse& Response) const
{
	FOliveWiringDiagnostic Diag;
	Diag.SourcePinName = SourcePin->GetName();
	Diag.TargetPinName = TargetPin->GetName();
	Diag.SourceTypeName = GetReadablePinTypeName(SourcePin);
	Diag.TargetTypeName = GetReadablePinTypeName(TargetPin);
	Diag.SchemaMessage = Response.Message.ToString();

	const FName& SrcCat = SourcePin->PinType.PinCategory;
	const FName& TgtCat = TargetPin->PinType.PinCategory;
	const EPinContainerType SrcContainer = SourcePin->PinType.ContainerType;
	const EPinContainerType TgtContainer = TargetPin->PinType.ContainerType;

	// Add container prefix to type names for clarity
	if (SrcContainer == EPinContainerType::Array)
	{
		Diag.SourceTypeName = TEXT("Array<") + Diag.SourceTypeName + TEXT(">");
	}
	else if (SrcContainer == EPinContainerType::Set)
	{
		Diag.SourceTypeName = TEXT("Set<") + Diag.SourceTypeName + TEXT(">");
	}
	else if (SrcContainer == EPinContainerType::Map)
	{
		Diag.SourceTypeName = TEXT("Map<") + Diag.SourceTypeName + TEXT(">");
	}
	if (TgtContainer == EPinContainerType::Array)
	{
		Diag.TargetTypeName = TEXT("Array<") + Diag.TargetTypeName + TEXT(">");
	}
	else if (TgtContainer == EPinContainerType::Set)
	{
		Diag.TargetTypeName = TEXT("Set<") + Diag.TargetTypeName + TEXT(">");
	}
	else if (TgtContainer == EPinContainerType::Map)
	{
		Diag.TargetTypeName = TEXT("Map<") + Diag.TargetTypeName + TEXT(">");
	}

	// ---- Categorize the failure reason ----

	// Same node check
	if (SourcePin->GetOwningNode() == TargetPin->GetOwningNode())
	{
		Diag.Reason = EOliveWiringFailureReason::SameNode;
		Diag.WhyAutoFixFailed = TEXT("Cannot connect a node to itself.");
	}
	// Direction mismatch
	else if (SourcePin->Direction == TargetPin->Direction)
	{
		Diag.Reason = EOliveWiringFailureReason::DirectionMismatch;
		Diag.WhyAutoFixFailed = FString::Printf(
			TEXT("Both pins have direction %s. Source must be output, target must be input."),
			SourcePin->Direction == EGPD_Output ? TEXT("Output") : TEXT("Input"));
	}
	// Container mismatch (Array<T> vs T, etc.)
	else if (SrcContainer != TgtContainer)
	{
		Diag.Reason = EOliveWiringFailureReason::ContainerMismatch;
		Diag.WhyAutoFixFailed = FString::Printf(
			TEXT("Container types differ: source is %s, target is %s. No automatic container conversion exists."),
			*Diag.SourceTypeName, *Diag.TargetTypeName);
	}
	// Struct -> Scalar
	else if (SrcCat == UEdGraphSchema_K2::PC_Struct
		&& (IsScalarNumeric(TgtCat, TargetPin->PinType.PinSubCategory)
			|| TgtCat == UEdGraphSchema_K2::PC_Boolean
			|| TgtCat == UEdGraphSchema_K2::PC_String))
	{
		Diag.Reason = EOliveWiringFailureReason::StructToScalar;

		// Check if pin is splittable
		const UEdGraphSchema_K2* K2Schema = GetK2Schema();
		UScriptStruct* StructType = Cast<UScriptStruct>(SourcePin->PinType.PinSubCategoryObject.Get());
		FString StructName = StructType ? StructType->GetName() : TEXT("unknown struct");

		if (K2Schema && K2Schema->CanSplitStructPin(*SourcePin))
		{
			TArray<FString> SubPins = GetKnownStructSubPins(StructName);
			if (SubPins.Num() > 0)
			{
				FString SubPinList;
				for (const FString& SP : SubPins)
				{
					if (!SubPinList.IsEmpty()) SubPinList += TEXT(", ");
					SubPinList += FString::Printf(TEXT("%s_%s (%s)"),
						*SourcePin->GetName(), *SP, *Diag.TargetTypeName);
				}
				Diag.WhyAutoFixFailed = FString::Printf(
					TEXT("No autocast for %s -> %s. Pin is splittable into: %s."),
					*StructName, *Diag.TargetTypeName, *SubPinList);
			}
			else
			{
				Diag.WhyAutoFixFailed = FString::Printf(
					TEXT("No autocast for %s -> %s. Pin is splittable (use break_struct to decompose)."),
					*StructName, *Diag.TargetTypeName);
			}
		}
		else
		{
			Diag.WhyAutoFixFailed = FString::Printf(
				TEXT("No autocast for %s -> %s. Struct type %s does not support pin splitting."),
				*StructName, *Diag.TargetTypeName, *StructName);
		}
	}
	// Scalar -> Struct
	else if ((IsScalarNumeric(SrcCat, SourcePin->PinType.PinSubCategory)
			|| SrcCat == UEdGraphSchema_K2::PC_Boolean
			|| SrcCat == UEdGraphSchema_K2::PC_String)
		&& TgtCat == UEdGraphSchema_K2::PC_Struct)
	{
		Diag.Reason = EOliveWiringFailureReason::ScalarToStruct;

		UScriptStruct* StructType = Cast<UScriptStruct>(TargetPin->PinType.PinSubCategoryObject.Get());
		FString StructName = StructType ? StructType->GetName() : TEXT("unknown struct");

		Diag.WhyAutoFixFailed = FString::Printf(
			TEXT("No autocast for %s -> %s. Use make_struct to compose the struct from scalar inputs."),
			*Diag.SourceTypeName, *StructName);
	}
	// Object type mismatch (both are object types but different classes)
	else if (IsObjectType(SrcCat) && IsObjectType(TgtCat))
	{
		Diag.Reason = EOliveWiringFailureReason::ObjectCastRequired;

		UClass* SrcClass = Cast<UClass>(SourcePin->PinType.PinSubCategoryObject.Get());
		UClass* TgtClass = Cast<UClass>(TargetPin->PinType.PinSubCategoryObject.Get());

		if (SrcClass && TgtClass && SrcClass->IsChildOf(TgtClass))
		{
			// Source is more derived than target -- should be safe. Unusual failure.
			Diag.WhyAutoFixFailed = FString::Printf(
				TEXT("%s is a child of %s, but the connection was still rejected. "
				     "This may require an explicit cast or interface conversion."),
				*SrcClass->GetName(), *TgtClass->GetName());
		}
		else if (SrcClass && TgtClass && TgtClass->IsChildOf(SrcClass))
		{
			// Target is more derived -- needs downcast
			Diag.WhyAutoFixFailed = FString::Printf(
				TEXT("Object types are not in a parent-child relationship in the required direction. "
				     "%s is a parent of %s, but the pin expects the more derived type."),
				*SrcClass->GetName(), *TgtClass->GetName());
		}
		else
		{
			Diag.WhyAutoFixFailed = FString::Printf(
				TEXT("Object types %s and %s are not in the same class hierarchy. "
				     "An explicit cast or interface is needed."),
				SrcClass ? *SrcClass->GetName() : TEXT("(unknown)"),
				TgtClass ? *TgtClass->GetName() : TEXT("(unknown)"));
		}
	}
	// Already-connected pin (BREAK_OTHERS response or existing links on a single-connection pin)
	else if (SourcePin->LinkedTo.Num() > 0 || TargetPin->LinkedTo.Num() > 0)
	{
		Diag.Reason = EOliveWiringFailureReason::AlreadyConnected;

		if (SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			Diag.WhyAutoFixFailed = FString::Printf(
				TEXT("Exec output '%s' is already connected to '%s'. "
					 "Disconnect first or use a different exec output pin (e.g., exec_outputs)."),
				*SourcePin->PinName.ToString(),
				SourcePin->LinkedTo.Num() > 0
					? *SourcePin->LinkedTo[0]->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString()
					: TEXT("unknown"));
		}
		else
		{
			Diag.WhyAutoFixFailed = FString::Printf(
				TEXT("Pin '%s' already has %d connection(s) — likely wired by a previous apply_plan_json. "
					 "Use blueprint.read to verify current wiring before manual connect_pins."),
				(SourcePin->LinkedTo.Num() > 0) ? *SourcePin->PinName.ToString() : *TargetPin->PinName.ToString(),
				FMath::Max(SourcePin->LinkedTo.Num(), TargetPin->LinkedTo.Num()));
		}
	}
	// Generic incompatible (Exec->Data, Data->Exec, or truly unrelated types)
	else
	{
		Diag.Reason = EOliveWiringFailureReason::TypesIncompatible;

		if (SrcCat == UEdGraphSchema_K2::PC_Exec || TgtCat == UEdGraphSchema_K2::PC_Exec)
		{
			Diag.WhyAutoFixFailed = TEXT("Exec pins carry execution flow, not data. "
				"You likely meant to connect a data output pin instead.");
		}
		else
		{
			Diag.WhyAutoFixFailed = FString::Printf(
				TEXT("Types %s and %s have no known conversion path. "
				     "This usually means the data flow is wrong at a design level."),
				*Diag.SourceTypeName, *Diag.TargetTypeName);
		}
	}

	// Build alternatives
	Diag.Alternatives = SuggestAlternatives(SourcePin, TargetPin, Diag.Reason);

	return Diag;
}

TArray<FOliveWiringAlternative> FOlivePinConnector::SuggestAlternatives(
	const UEdGraphPin* SourcePin,
	const UEdGraphPin* TargetPin,
	EOliveWiringFailureReason Reason) const
{
	TArray<FOliveWiringAlternative> Alts;

	switch (Reason)
	{
	case EOliveWiringFailureReason::StructToScalar:
	{
		const UEdGraphSchema_K2* K2Schema = GetK2Schema();
		bool bIsSplittable = K2Schema && K2Schema->CanSplitStructPin(*SourcePin);

		if (bIsSplittable)
		{
			UScriptStruct* StructType = Cast<UScriptStruct>(SourcePin->PinType.PinSubCategoryObject.Get());
			FString StructName = StructType ? StructType->GetName() : TEXT("Struct");

			Alts.Add({
				TEXT("Use break_struct op"),
				FString::Printf(TEXT("Add a break_struct step for the %s, then wire the specific "
					"field (e.g., @break_step.X) to the %s input."),
					*StructName, *GetReadablePinTypeName(TargetPin)),
				TEXT("high")
			});

			// Build a ~suffix example from known sub-pins
			TArray<FString> SubPins = GetKnownStructSubPins(StructName);
			FString SuffixExample = SubPins.Num() > 0
				? FString::Printf(TEXT("@source_step.~%s_%s"), *SourcePin->GetName(), *SubPins[0])
				: FString::Printf(TEXT("@source_step.~%s_X"), *SourcePin->GetName());

			Alts.Add({
				TEXT("Use ~PinName suffix"),
				FString::Printf(TEXT("In plan_json inputs, use %s to target a sub-component. "
					"The ~ prefix triggers fuzzy match on split sub-pins."), *SuffixExample),
				TEXT("high")
			});
		}

		Alts.Add({
			TEXT("Use blueprint.read + connect_pins"),
			TEXT("Call blueprint.read(section:'graph', mode:'full') to see all nodes and pins "
				"(including split sub-pins). Then use connect_pins with the exact sub-pin name."),
			TEXT("medium")
		});

		Alts.Add({
			TEXT("Use editor.run_python"),
			TEXT("Schema->SplitPin(Pin) creates sub-pins programmatically. "
				"Use editor.run_python to split the pin and wire the sub-pin."),
			TEXT("low")
		});
		break;
	}

	case EOliveWiringFailureReason::ScalarToStruct:
	{
		UScriptStruct* StructType = Cast<UScriptStruct>(TargetPin->PinType.PinSubCategoryObject.Get());
		FString StructName = StructType ? StructType->GetName() : TEXT("Struct");

		Alts.Add({
			TEXT("Use make_struct op"),
			FString::Printf(TEXT("Add a make_struct step for the target type "
				"(e.g., make_struct target:%s) to compose the struct from scalar inputs."),
				*StructName),
			TEXT("high")
		});

		// Check if a Conv_ function exists in the reverse direction
		// (e.g., Conv_DoubleToVector when trying Float -> Vector)
		// We probe reversed to find composition functions
		Alts.Add({
			TEXT("Use Conv_ function if available"),
			FString::Printf(TEXT("If a Conv_ function exists (e.g., Conv_%sTo%s), add a call step "
				"with the conversion function to transform the scalar to the struct type."),
				*GetReadablePinTypeName(SourcePin), *StructName),
			TEXT("medium")
		});

		Alts.Add({
			TEXT("Use editor.run_python"),
			TEXT("Compose the struct manually in Python."),
			TEXT("low")
		});
		break;
	}

	case EOliveWiringFailureReason::ObjectCastRequired:
	{
		UClass* TgtClass = Cast<UClass>(TargetPin->PinType.PinSubCategoryObject.Get());
		FString TargetClassName = TgtClass ? TgtClass->GetName() : TEXT("TargetClass");

		Alts.Add({
			TEXT("Add a cast step"),
			FString::Printf(TEXT("Add a cast step in plan_json: "
				"{\"op\":\"cast\",\"target\":\"%s\",\"inputs\":{\"Object\":\"@source_step\"}}. "
				"Wire the output cast pin to the target input."), *TargetClassName),
			TEXT("high")
		});

		Alts.Add({
			TEXT("Use cast node via add_node"),
			FString::Printf(TEXT("blueprint.add_node type:\"Cast\" "
				"properties:{\"TargetType\":\"%s\"}, then connect_pins source -> Cast Object input, "
				"Cast output -> target input."), *TargetClassName),
			TEXT("medium")
		});
		break;
	}

	case EOliveWiringFailureReason::ContainerMismatch:
	{
		const EPinContainerType SrcContainer = SourcePin->PinType.ContainerType;

		if (SrcContainer == EPinContainerType::Array)
		{
			Alts.Add({
				TEXT("Add an array operation"),
				TEXT("To get a single element from an Array, add a call step for \"Get\" "
					"(array access by index) or \"GetCopy\" between the source and target."),
				TEXT("high")
			});
		}
		else if (SrcContainer == EPinContainerType::None)
		{
			Alts.Add({
				TEXT("Add a MakeArray operation"),
				TEXT("To convert a single item to an Array, add a call step for \"MakeArray\" "
					"between the source and target."),
				TEXT("high")
			});
		}
		else
		{
			Alts.Add({
				TEXT("Add a container conversion"),
				TEXT("Container types differ. Add an intermediate step to convert between "
					"container types (Array, Set, Map)."),
				TEXT("high")
			});
		}

		Alts.Add({
			TEXT("Use editor.run_python"),
			TEXT("For complex container transformations, use Python."),
			TEXT("low")
		});
		break;
	}

	case EOliveWiringFailureReason::DirectionMismatch:
	{
		Alts.Add({
			TEXT("Fix pin direction"),
			TEXT("Source must be an output pin (EGPD_Output) and target must be an input pin "
				"(EGPD_Input). Swap the source and target parameters."),
			TEXT("high")
		});
		break;
	}

	case EOliveWiringFailureReason::SameNode:
	{
		Alts.Add({
			TEXT("Use different nodes"),
			TEXT("Source and target pins must be on different nodes. "
				"Check your step references to ensure you are connecting between two distinct nodes."),
			TEXT("high")
		});
		break;
	}

	case EOliveWiringFailureReason::AlreadyConnected:
	{
		Alts.Add({
			TEXT("Disconnect existing connection first"),
			TEXT("The target pin already has a connection. Use blueprint.disconnect_pins to remove "
				"the existing connection, then retry."),
			TEXT("high")
		});
		break;
	}

	case EOliveWiringFailureReason::TypesIncompatible:
	case EOliveWiringFailureReason::Unknown:
	default:
	{
		// Check for Exec -> Data mixup specifically
		if (SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			|| TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			Alts.Add({
				TEXT("Check your pin selection"),
				TEXT("You are connecting an execution flow pin to a data pin (or vice versa). "
					"Use blueprint.read(section:'graph', mode:'full') to find the correct data output pin."),
				TEXT("high")
			});
		}
		else
		{
			Alts.Add({
				TEXT("Check your plan logic"),
				FString::Printf(TEXT("Types %s and %s have no known conversion path. "
					"This usually means the data flow is wrong at a design level. "
					"Reconsider which node outputs should feed this input."),
					*GetReadablePinTypeName(SourcePin),
					*GetReadablePinTypeName(TargetPin)),
				TEXT("high")
			});
		}

		Alts.Add({
			TEXT("Use editor.run_python"),
			TEXT("For unconventional type conversions, Python can bypass Blueprint type constraints."),
			TEXT("low")
		});
		break;
	}
	}

	return Alts;
}
