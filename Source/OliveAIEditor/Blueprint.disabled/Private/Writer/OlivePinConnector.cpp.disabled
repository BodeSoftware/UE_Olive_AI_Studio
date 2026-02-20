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

	// Check if connection is possible
	FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);

	if (Response.CanSafeConnect())
	{
		// Get the Blueprint for transaction
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

		// Make the connection
		bool bSuccess = K2Schema->TryCreateConnection(SourcePin, TargetPin);

		if (bSuccess)
		{
			// Mark Blueprint as modified
			if (Blueprint)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			}

			UE_LOG(LogOlivePinConnector, Log, TEXT("Connected pins: %s -> %s"),
				*SourcePin->GetName(), *TargetPin->GetName());

			return FOliveBlueprintWriteResult::Success(AssetPath);
		}
		else
		{
			return FOliveBlueprintWriteResult::Error(TEXT("TryCreateConnection returned false"), AssetPath);
		}
	}
	else if (bAllowConversion)
	{
		// Try to insert a conversion node
		TArray<FString> ConversionOptions = GetConversionOptions(SourcePin->PinType, TargetPin->PinType);

		if (ConversionOptions.Num() > 0)
		{
			// Use the first available conversion
			UEdGraph* Graph = SourcePin->GetOwningNode()->GetGraph();
			return InsertConversionNode(Graph, SourcePin, TargetPin, ConversionOptions[0]);
		}
		else
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("Cannot connect pins and no conversion available: %s"),
					*Response.Message.ToString()),
				AssetPath);
		}
	}
	else
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Cannot connect pins: %s"), *Response.Message.ToString()),
			AssetPath);
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

	// Check connection validity
	FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);

	if (Response.CanSafeConnect())
	{
		return true;
	}
	else
	{
		OutReason = Response.Message.ToString();
		return false;
	}
}

TArray<FString> FOlivePinConnector::GetConversionOptions(
	const FEdGraphPinType& FromType,
	const FEdGraphPinType& ToType) const
{
	TArray<FString> Options;

	const UEdGraphSchema_K2* K2Schema = GetK2Schema();
	if (!K2Schema)
	{
		return Options;
	}

	// Check for auto-conversion support
	if (CanAutoConvert(FromType, ToType))
	{
		// Build conversion name from types
		FString FromTypeName = GetPinTypeDescription(FromType);
		FString ToTypeName = GetPinTypeDescription(ToType);
		Options.Add(FString::Printf(TEXT("Convert_%s_To_%s"), *FromTypeName, *ToTypeName));
	}

	// Check common conversion patterns
	// Integer to Float/Double
	if (FromType.PinCategory == UEdGraphSchema_K2::PC_Int)
	{
		if (ToType.PinCategory == UEdGraphSchema_K2::PC_Float ||
			ToType.PinCategory == UEdGraphSchema_K2::PC_Double)
		{
			Options.AddUnique(TEXT("IntToFloat"));
		}
		if (ToType.PinCategory == UEdGraphSchema_K2::PC_String)
		{
			Options.AddUnique(TEXT("IntToString"));
		}
	}

	// Float/Double to Integer
	if (FromType.PinCategory == UEdGraphSchema_K2::PC_Float ||
		FromType.PinCategory == UEdGraphSchema_K2::PC_Double)
	{
		if (ToType.PinCategory == UEdGraphSchema_K2::PC_Int)
		{
			Options.AddUnique(TEXT("FloatToInt"));
		}
		if (ToType.PinCategory == UEdGraphSchema_K2::PC_String)
		{
			Options.AddUnique(TEXT("FloatToString"));
		}
	}

	// Boolean conversions
	if (FromType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
	{
		if (ToType.PinCategory == UEdGraphSchema_K2::PC_String)
		{
			Options.AddUnique(TEXT("BoolToString"));
		}
	}

	// String conversions
	if (FromType.PinCategory == UEdGraphSchema_K2::PC_String)
	{
		if (ToType.PinCategory == UEdGraphSchema_K2::PC_Name)
		{
			Options.AddUnique(TEXT("StringToName"));
		}
		if (ToType.PinCategory == UEdGraphSchema_K2::PC_Text)
		{
			Options.AddUnique(TEXT("StringToText"));
		}
	}

	// Name conversions
	if (FromType.PinCategory == UEdGraphSchema_K2::PC_Name)
	{
		if (ToType.PinCategory == UEdGraphSchema_K2::PC_String)
		{
			Options.AddUnique(TEXT("NameToString"));
		}
	}

	// Vector conversions
	if (FromType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		// Check for Vector to Rotator, etc.
		if (FromType.PinSubCategoryObject.IsValid() && ToType.PinSubCategoryObject.IsValid())
		{
			FString FromStructName = FromType.PinSubCategoryObject->GetName();
			FString ToStructName = ToType.PinSubCategoryObject->GetName();

			if (FromStructName == TEXT("Vector") && ToStructName == TEXT("Rotator"))
			{
				Options.AddUnique(TEXT("VectorToRotator"));
			}
			if (FromStructName == TEXT("Rotator") && ToStructName == TEXT("Vector"))
			{
				Options.AddUnique(TEXT("RotatorToVector"));
			}
		}
	}

	// Object to interface/class conversions
	if (FromType.PinCategory == UEdGraphSchema_K2::PC_Object)
	{
		if (ToType.PinCategory == UEdGraphSchema_K2::PC_Interface)
		{
			Options.AddUnique(TEXT("ObjectToInterface"));
		}
	}

	return Options;
}

FOliveBlueprintWriteResult FOlivePinConnector::InsertConversionNode(
	UEdGraph* Graph,
	UEdGraphPin* SourcePin,
	UEdGraphPin* TargetPin,
	const FString& ConversionType)
{
	if (!Graph || !SourcePin || !TargetPin)
	{
		return FOliveBlueprintWriteResult::Error(TEXT("Invalid parameters for conversion node insertion"));
	}

	FString AssetPath = GetAssetPathForPin(SourcePin);

	// Calculate position for conversion node (midpoint between source and target)
	UEdGraphNode* SourceNode = SourcePin->GetOwningNode();
	UEdGraphNode* TargetNode = TargetPin->GetOwningNode();

	int32 PosX = (SourceNode->NodePosX + TargetNode->NodePosX) / 2;
	int32 PosY = (SourceNode->NodePosY + TargetNode->NodePosY) / 2;

	// Create the conversion node
	UEdGraphNode* ConversionNode = CreateConversionNode(
		Graph, SourcePin->PinType, TargetPin->PinType, PosX, PosY);

	if (!ConversionNode)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to create conversion node for '%s'"), *ConversionType),
			AssetPath);
	}

	UBlueprint* Blueprint = GetOwningBlueprint(SourcePin);

	// Create transaction for undo support
	OLIVE_SCOPED_TRANSACTION(FText::Format(
		NSLOCTEXT("OlivePinConnector", "InsertConversion", "Insert Conversion Node: {0}"),
		FText::FromString(ConversionType)));

	if (Blueprint)
	{
		Blueprint->Modify();
	}

	const UEdGraphSchema_K2* K2Schema = GetK2Schema();

	// Find input and output pins on conversion node
	UEdGraphPin* ConversionInputPin = nullptr;
	UEdGraphPin* ConversionOutputPin = nullptr;

	for (UEdGraphPin* Pin : ConversionNode->Pins)
	{
		if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			ConversionInputPin = Pin;
		}
		else if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			ConversionOutputPin = Pin;
		}
	}

	if (!ConversionInputPin || !ConversionOutputPin)
	{
		// Remove the conversion node since we can't wire it
		Graph->RemoveNode(ConversionNode);
		return FOliveBlueprintWriteResult::Error(
			TEXT("Conversion node has unexpected pin layout"),
			AssetPath);
	}

	// Connect source -> conversion -> target
	bool bConnected1 = K2Schema->TryCreateConnection(SourcePin, ConversionInputPin);
	bool bConnected2 = K2Schema->TryCreateConnection(ConversionOutputPin, TargetPin);

	if (bConnected1 && bConnected2)
	{
		if (Blueprint)
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		UE_LOG(LogOlivePinConnector, Log, TEXT("Inserted conversion node '%s' between %s and %s"),
			*ConversionType, *SourcePin->GetName(), *TargetPin->GetName());

		return FOliveBlueprintWriteResult::SuccessWithNode(
			AssetPath,
			ConversionNode->GetFName().ToString());
	}
	else
	{
		// Clean up on failure
		Graph->RemoveNode(ConversionNode);
		return FOliveBlueprintWriteResult::Error(
			TEXT("Failed to connect conversion node"),
			AssetPath);
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

bool FOlivePinConnector::CanAutoConvert(
	const FEdGraphPinType& FromType,
	const FEdGraphPinType& ToType) const
{
	const UEdGraphSchema_K2* K2Schema = GetK2Schema();
	if (!K2Schema)
	{
		return false;
	}

	// Check if the schema reports these as compatible with auto-conversion
	// The schema has internal knowledge of available conversion functions

	// For basic types, check common conversions
	FName FromCategory = FromType.PinCategory;
	FName ToCategory = ToType.PinCategory;

	// Numeric conversions
	bool bFromNumeric = (FromCategory == UEdGraphSchema_K2::PC_Int ||
		FromCategory == UEdGraphSchema_K2::PC_Int64 ||
		FromCategory == UEdGraphSchema_K2::PC_Float ||
		FromCategory == UEdGraphSchema_K2::PC_Double ||
		FromCategory == UEdGraphSchema_K2::PC_Byte);

	bool bToNumeric = (ToCategory == UEdGraphSchema_K2::PC_Int ||
		ToCategory == UEdGraphSchema_K2::PC_Int64 ||
		ToCategory == UEdGraphSchema_K2::PC_Float ||
		ToCategory == UEdGraphSchema_K2::PC_Double ||
		ToCategory == UEdGraphSchema_K2::PC_Byte);

	if (bFromNumeric && bToNumeric)
	{
		return true;
	}

	// String conversions
	bool bFromText = (FromCategory == UEdGraphSchema_K2::PC_String ||
		FromCategory == UEdGraphSchema_K2::PC_Name ||
		FromCategory == UEdGraphSchema_K2::PC_Text);

	bool bToText = (ToCategory == UEdGraphSchema_K2::PC_String ||
		ToCategory == UEdGraphSchema_K2::PC_Name ||
		ToCategory == UEdGraphSchema_K2::PC_Text);

	if (bFromText && bToText)
	{
		return true;
	}

	// To string from anything
	if (ToCategory == UEdGraphSchema_K2::PC_String)
	{
		return true; // Most types can convert to string
	}

	return false;
}

UEdGraphNode* FOlivePinConnector::CreateConversionNode(
	UEdGraph* Graph,
	const FEdGraphPinType& FromType,
	const FEdGraphPinType& ToType,
	int32 PosX,
	int32 PosY)
{
	if (!Graph)
	{
		return nullptr;
	}

	const UEdGraphSchema_K2* K2Schema = GetK2Schema();
	if (!K2Schema)
	{
		return nullptr;
	}

	// For now, use a reroute node as a simple passthrough
	// In a full implementation, this would create the appropriate conversion function call
	// based on FromType and ToType

	// Try to find a conversion function
	// This is a simplified implementation - full implementation would use
	// K2Schema->FindSpecializedConversionNode or similar

	// For basic conversions, we can use built-in conversion functions
	// Example: For Int to Float, use Conv_IntToFloat from KismetMathLibrary

	// For now, return nullptr to indicate we couldn't create a conversion
	// The caller should handle this gracefully

	UE_LOG(LogOlivePinConnector, Warning,
		TEXT("Auto-conversion node creation not fully implemented for %s -> %s"),
		*GetPinTypeDescription(FromType),
		*GetPinTypeDescription(ToType));

	return nullptr;
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
