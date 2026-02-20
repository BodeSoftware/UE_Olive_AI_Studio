// Copyright Bode Software. All Rights Reserved.

#include "OlivePinSerializer.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================================
// Main Serialization Methods
// ============================================================================

FOliveIRPin FOlivePinSerializer::SerializePin(
	const UEdGraphPin* Pin,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	FOliveIRPin IRPin;

	if (!Pin)
	{
		return IRPin;
	}

	// Basic pin info
	IRPin.Name = Pin->GetName();
	IRPin.DisplayName = Pin->GetDisplayName().ToString();

	// Direction
	IRPin.bIsInput = (Pin->Direction == EGPD_Input);

	// Check if exec pin
	IRPin.bIsExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);

	// Serialize type
	IRPin.Type = SerializePinType(Pin->PinType);

	// Default value (only for input pins)
	if (IRPin.bIsInput && !IRPin.bIsExec)
	{
		IRPin.DefaultValue = SerializeDefaultValue(Pin);
	}

	// Connections
	if (Pin->LinkedTo.Num() > 0)
	{
		if (IRPin.bIsInput)
		{
			// Input pins have single connection
			IRPin.Connection = ResolveConnection(Pin, NodeIdMap);
		}
		else
		{
			// Output pins can have multiple connections
			IRPin.Connections = ResolveAllConnections(Pin, NodeIdMap);
			// Also set single Connection for backwards compatibility
			if (IRPin.Connections.Num() > 0)
			{
				IRPin.Connection = IRPin.Connections[0];
			}
		}
	}

	return IRPin;
}

FOliveIRType FOlivePinSerializer::SerializePinType(const FEdGraphPinType& PinType) const
{
	FOliveIRType IRType;

	// Map the primary category
	IRType.Category = MapPinCategory(PinType.PinCategory);

	// Handle reference and const flags
	IRType.bIsReference = PinType.bIsReference;
	IRType.bIsConst = PinType.bIsConst;

	// Handle container types
	if (PinType.IsContainer())
	{
		if (PinType.IsArray())
		{
			IRType.Category = EOliveIRTypeCategory::Array;
			IRType.ElementTypeJson = SerializeInnerType(PinType);
		}
		else if (PinType.ContainerType == EPinContainerType::Set)
		{
			IRType.Category = EOliveIRTypeCategory::Set;
			IRType.ElementTypeJson = SerializeInnerType(PinType);
		}
		else if (PinType.IsMap())
		{
			IRType.Category = EOliveIRTypeCategory::Map;
			// For maps, we need key and value types
			IRType.KeyTypeJson = SerializeInnerType(PinType);

			// Value type is in PinValueType
			if (PinType.PinValueType.TerminalCategory != NAME_None)
			{
				FEdGraphPinType ValuePinType;
				ValuePinType.PinCategory = PinType.PinValueType.TerminalCategory;
				ValuePinType.PinSubCategory = PinType.PinValueType.TerminalSubCategory;
				ValuePinType.PinSubCategoryObject = PinType.PinValueType.TerminalSubCategoryObject;

				FOliveIRType ValueType = SerializePinType(ValuePinType);
				TSharedPtr<FJsonObject> ValueJson = ValueType.ToJson();
				if (ValueJson.IsValid())
				{
					FString OutputString;
					TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
					FJsonSerializer::Serialize(ValueJson.ToSharedRef(), Writer);
					IRType.ValueTypeJson = OutputString;
				}
			}
		}
	}
	else
	{
		// Non-container types - extract subcategory info
		FString SubcategoryName = GetSubcategoryName(PinType);

		switch (IRType.Category)
		{
		case EOliveIRTypeCategory::Object:
		case EOliveIRTypeCategory::Class:
			if (const UClass* Class = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
			{
				IRType.ClassName = FormatTypeName(Class);
			}
			else
			{
				IRType.ClassName = SubcategoryName;
			}
			break;

		case EOliveIRTypeCategory::Struct:
			if (const UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get()))
			{
				IRType.StructName = FormatTypeName(Struct);
			}
			else
			{
				IRType.StructName = SubcategoryName;
			}
			break;

		case EOliveIRTypeCategory::Enum:
			if (const UEnum* Enum = Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
			{
				IRType.EnumName = FormatTypeName(Enum);
			}
			else
			{
				IRType.EnumName = SubcategoryName;
			}
			break;

		case EOliveIRTypeCategory::Interface:
			if (const UClass* InterfaceClass = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
			{
				IRType.ClassName = FormatTypeName(InterfaceClass);
			}
			else
			{
				IRType.ClassName = SubcategoryName;
			}
			break;

		case EOliveIRTypeCategory::Delegate:
		case EOliveIRTypeCategory::MulticastDelegate:
			// Delegate signature info could be extracted from the function signature
			// For now, store the subcategory object path
			if (PinType.PinSubCategoryObject.IsValid())
			{
				IRType.DelegateSignatureJson = ResolveObjectPath(PinType.PinSubCategoryObject.Get());
			}
			break;

		default:
			// Primitive types don't need additional info
			break;
		}
	}

	return IRType;
}

FString FOlivePinSerializer::SerializeDefaultValue(const UEdGraphPin* Pin) const
{
	if (!Pin)
	{
		return FString();
	}

	// Check for object reference default
	if (Pin->DefaultObject)
	{
		return ResolveObjectPath(Pin->DefaultObject);
	}

	// Check for text default
	if (!Pin->DefaultTextValue.IsEmpty())
	{
		return Pin->DefaultTextValue.ToString();
	}

	// Use string default value
	return Pin->DefaultValue;
}

FString FOlivePinSerializer::ResolveConnection(
	const UEdGraphPin* Pin,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	if (!Pin || Pin->LinkedTo.Num() == 0)
	{
		return FString();
	}

	// Get the first connected pin
	const UEdGraphPin* ConnectedPin = Pin->LinkedTo[0];
	if (!ConnectedPin)
	{
		return FString();
	}

	// Get the owning node
	const UEdGraphNode* ConnectedNode = ConnectedPin->GetOwningNode();
	if (!ConnectedNode)
	{
		return FString();
	}

	// Look up the node ID
	const FString* NodeId = NodeIdMap.Find(ConnectedNode);
	if (!NodeId)
	{
		return FString();
	}

	// Format as "node_id.pin_name"
	return FString::Printf(TEXT("%s.%s"), **NodeId, *ConnectedPin->GetName());
}

TArray<FString> FOlivePinSerializer::ResolveAllConnections(
	const UEdGraphPin* Pin,
	const TMap<const UEdGraphNode*, FString>& NodeIdMap) const
{
	TArray<FString> Connections;

	if (!Pin)
	{
		return Connections;
	}

	for (const UEdGraphPin* ConnectedPin : Pin->LinkedTo)
	{
		if (!ConnectedPin)
		{
			continue;
		}

		const UEdGraphNode* ConnectedNode = ConnectedPin->GetOwningNode();
		if (!ConnectedNode)
		{
			continue;
		}

		const FString* NodeId = NodeIdMap.Find(ConnectedNode);
		if (!NodeId)
		{
			continue;
		}

		// Format as "node_id.pin_name"
		Connections.Add(FString::Printf(TEXT("%s.%s"), **NodeId, *ConnectedPin->GetName()));
	}

	return Connections;
}

// ============================================================================
// Type Mapping Helpers
// ============================================================================

EOliveIRTypeCategory FOlivePinSerializer::MapPinCategory(const FName& CategoryName) const
{
	// Use static map for efficient lookup
	static const TMap<FName, EOliveIRTypeCategory> CategoryMap = []()
	{
		TMap<FName, EOliveIRTypeCategory> Map;

		// Execution
		Map.Add(UEdGraphSchema_K2::PC_Exec, EOliveIRTypeCategory::Exec);

		// Primitives
		Map.Add(UEdGraphSchema_K2::PC_Boolean, EOliveIRTypeCategory::Bool);
		Map.Add(UEdGraphSchema_K2::PC_Byte, EOliveIRTypeCategory::Byte);
		Map.Add(UEdGraphSchema_K2::PC_Int, EOliveIRTypeCategory::Int);
		Map.Add(UEdGraphSchema_K2::PC_Int64, EOliveIRTypeCategory::Int64);
		Map.Add(UEdGraphSchema_K2::PC_Float, EOliveIRTypeCategory::Float);
		Map.Add(UEdGraphSchema_K2::PC_Double, EOliveIRTypeCategory::Double);
		Map.Add(UEdGraphSchema_K2::PC_Real, EOliveIRTypeCategory::Float);  // Real maps to Float

		// Strings
		Map.Add(UEdGraphSchema_K2::PC_String, EOliveIRTypeCategory::String);
		Map.Add(UEdGraphSchema_K2::PC_Name, EOliveIRTypeCategory::Name);
		Map.Add(UEdGraphSchema_K2::PC_Text, EOliveIRTypeCategory::Text);

		// Complex types
		Map.Add(UEdGraphSchema_K2::PC_Struct, EOliveIRTypeCategory::Struct);
		Map.Add(UEdGraphSchema_K2::PC_Object, EOliveIRTypeCategory::Object);
		Map.Add(UEdGraphSchema_K2::PC_Class, EOliveIRTypeCategory::Class);
		Map.Add(UEdGraphSchema_K2::PC_Interface, EOliveIRTypeCategory::Interface);
		Map.Add(UEdGraphSchema_K2::PC_Enum, EOliveIRTypeCategory::Enum);

		// Delegates
		Map.Add(UEdGraphSchema_K2::PC_Delegate, EOliveIRTypeCategory::Delegate);
		Map.Add(UEdGraphSchema_K2::PC_MCDelegate, EOliveIRTypeCategory::MulticastDelegate);

		// Soft references (map to their underlying types)
		Map.Add(UEdGraphSchema_K2::PC_SoftObject, EOliveIRTypeCategory::Object);
		Map.Add(UEdGraphSchema_K2::PC_SoftClass, EOliveIRTypeCategory::Class);

		// Wildcard
		Map.Add(UEdGraphSchema_K2::PC_Wildcard, EOliveIRTypeCategory::Wildcard);

		return Map;
	}();

	const EOliveIRTypeCategory* Found = CategoryMap.Find(CategoryName);
	return Found ? *Found : EOliveIRTypeCategory::Unknown;
}

FString FOlivePinSerializer::ResolveObjectPath(const UObject* Object) const
{
	if (!Object)
	{
		return FString();
	}

	return Object->GetPathName();
}

FString FOlivePinSerializer::FormatTypeName(const UStruct* Struct) const
{
	if (!Struct)
	{
		return FString();
	}

	// Return the struct name with appropriate prefix
	FString StructName = Struct->GetName();

	// UE structs typically have F prefix, but we return the actual name
	// The prefix is part of the name in UE
	return StructName;
}

FString FOlivePinSerializer::FormatTypeName(const UEnum* Enum) const
{
	if (!Enum)
	{
		return FString();
	}

	// Return the enum name with its prefix (E for UE enums)
	return Enum->GetName();
}

FString FOlivePinSerializer::FormatTypeName(const UClass* Class) const
{
	if (!Class)
	{
		return FString();
	}

	// Return the class name with appropriate prefix (U for UObject, A for AActor)
	return Class->GetName();
}

FString FOlivePinSerializer::SerializeInnerType(const FEdGraphPinType& PinType) const
{
	// Create a pin type for the inner type
	FEdGraphPinType InnerPinType;
	InnerPinType.PinCategory = PinType.PinCategory;
	InnerPinType.PinSubCategory = PinType.PinSubCategory;
	InnerPinType.PinSubCategoryObject = PinType.PinSubCategoryObject;
	// Don't carry container flags to inner type
	InnerPinType.ContainerType = EPinContainerType::None;

	// Serialize the inner type
	FOliveIRType InnerType = SerializePinType(InnerPinType);

	// Convert to JSON string
	TSharedPtr<FJsonObject> InnerJson = InnerType.ToJson();
	if (InnerJson.IsValid())
	{
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(InnerJson.ToSharedRef(), Writer);
		return OutputString;
	}

	return FString();
}

FString FOlivePinSerializer::GetSubcategoryName(const FEdGraphPinType& PinType) const
{
	if (PinType.PinSubCategoryObject.IsValid())
	{
		return PinType.PinSubCategoryObject->GetName();
	}

	if (!PinType.PinSubCategory.IsNone())
	{
		return PinType.PinSubCategory.ToString();
	}

	return FString();
}
