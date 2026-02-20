// Copyright Bode Software. All Rights Reserved.

#include "OliveWidgetTreeSerializer.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/GridPanel.h"
#include "Components/GridSlot.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOliveWidgetSerializer);

FOliveWidgetTreeSerializer::FOliveWidgetTreeSerializer()
	: CurrentWidgetBlueprint(nullptr)
{
}

// ============================================================================
// High-Level Read Methods
// ============================================================================

TOptional<FOliveIRWidgetNode> FOliveWidgetTreeSerializer::ReadWidgetTree(const UWidgetBlueprint* WidgetBlueprint)
{
	if (!WidgetBlueprint)
	{
		UE_LOG(LogOliveWidgetSerializer, Warning, TEXT("ReadWidgetTree called with null WidgetBlueprint"));
		return TOptional<FOliveIRWidgetNode>();
	}

	UWidgetTree* WidgetTree = WidgetBlueprint->WidgetTree;
	if (!WidgetTree)
	{
		UE_LOG(LogOliveWidgetSerializer, Warning, TEXT("WidgetBlueprint '%s' has no WidgetTree"), *WidgetBlueprint->GetName());
		return TOptional<FOliveIRWidgetNode>();
	}

	UWidget* RootWidget = WidgetTree->RootWidget;
	if (!RootWidget)
	{
		UE_LOG(LogOliveWidgetSerializer, Verbose, TEXT("WidgetTree in '%s' has no root widget"), *WidgetBlueprint->GetName());
		return TOptional<FOliveIRWidgetNode>();
	}

	// Store current blueprint for binding lookups
	CurrentWidgetBlueprint = WidgetBlueprint;

	FOliveIRWidgetNode RootNode = SerializeWidget(RootWidget);

	CurrentWidgetBlueprint = nullptr;

	UE_LOG(LogOliveWidgetSerializer, Log, TEXT("Read widget tree from '%s' with root '%s'"),
		*WidgetBlueprint->GetName(), *RootNode.Name);

	return RootNode;
}

TOptional<FOliveIRWidgetNode> FOliveWidgetTreeSerializer::ReadWidget(
	const UWidgetBlueprint* WidgetBlueprint,
	const FString& WidgetName)
{
	if (!WidgetBlueprint)
	{
		UE_LOG(LogOliveWidgetSerializer, Warning, TEXT("ReadWidget called with null WidgetBlueprint"));
		return TOptional<FOliveIRWidgetNode>();
	}

	UWidgetTree* WidgetTree = WidgetBlueprint->WidgetTree;
	if (!WidgetTree)
	{
		UE_LOG(LogOliveWidgetSerializer, Warning, TEXT("WidgetBlueprint '%s' has no WidgetTree"), *WidgetBlueprint->GetName());
		return TOptional<FOliveIRWidgetNode>();
	}

	// Find the widget by name
	UWidget* FoundWidget = FindWidgetByName(WidgetTree, WidgetName);
	if (!FoundWidget)
	{
		UE_LOG(LogOliveWidgetSerializer, Warning, TEXT("Widget '%s' not found in WidgetBlueprint '%s'"),
			*WidgetName, *WidgetBlueprint->GetName());
		return TOptional<FOliveIRWidgetNode>();
	}

	// Store current blueprint for binding lookups
	CurrentWidgetBlueprint = WidgetBlueprint;

	FOliveIRWidgetNode WidgetNode = SerializeWidget(FoundWidget);

	CurrentWidgetBlueprint = nullptr;

	UE_LOG(LogOliveWidgetSerializer, Log, TEXT("Read widget '%s' from '%s'"),
		*WidgetName, *WidgetBlueprint->GetName());

	return WidgetNode;
}

TArray<FOliveIRWidgetNode> FOliveWidgetTreeSerializer::ReadWidgetTreeFlat(const UWidgetBlueprint* WidgetBlueprint)
{
	TArray<FOliveIRWidgetNode> FlatList;

	if (!WidgetBlueprint)
	{
		UE_LOG(LogOliveWidgetSerializer, Warning, TEXT("ReadWidgetTreeFlat called with null WidgetBlueprint"));
		return FlatList;
	}

	UWidgetTree* WidgetTree = WidgetBlueprint->WidgetTree;
	if (!WidgetTree || !WidgetTree->RootWidget)
	{
		UE_LOG(LogOliveWidgetSerializer, Verbose, TEXT("WidgetBlueprint '%s' has no widget tree"), *WidgetBlueprint->GetName());
		return FlatList;
	}

	// Store current blueprint for binding lookups
	CurrentWidgetBlueprint = WidgetBlueprint;

	TraverseWidgetTree(WidgetTree->RootWidget, FlatList);

	CurrentWidgetBlueprint = nullptr;

	UE_LOG(LogOliveWidgetSerializer, Log, TEXT("Read %d widgets from '%s' (flat)"),
		FlatList.Num(), *WidgetBlueprint->GetName());

	return FlatList;
}

TSharedPtr<FJsonObject> FOliveWidgetTreeSerializer::ReadWidgetTreeSummary(const UWidgetBlueprint* WidgetBlueprint)
{
	TSharedPtr<FJsonObject> Summary = MakeShareable(new FJsonObject());

	if (!WidgetBlueprint)
	{
		UE_LOG(LogOliveWidgetSerializer, Warning, TEXT("ReadWidgetTreeSummary called with null WidgetBlueprint"));
		return Summary;
	}

	UWidgetTree* WidgetTree = WidgetBlueprint->WidgetTree;
	if (!WidgetTree || !WidgetTree->RootWidget)
	{
		Summary->SetStringField(TEXT("error"), TEXT("No widget tree"));
		return Summary;
	}

	// Get all widgets
	TArray<UWidget*> AllWidgets;
	WidgetTree->GetAllWidgets(AllWidgets);

	// Build summary array
	TArray<TSharedPtr<FJsonValue>> WidgetSummaries;
	for (UWidget* Widget : AllWidgets)
	{
		if (Widget)
		{
			TSharedPtr<FJsonObject> WidgetSummary = MakeShareable(new FJsonObject());
			WidgetSummary->SetStringField(TEXT("name"), Widget->GetName());
			WidgetSummary->SetStringField(TEXT("class"), GetCleanClassName(Widget->GetClass()));

			// Get parent name if any
			UPanelWidget* Parent = Widget->GetParent();
			if (Parent)
			{
				WidgetSummary->SetStringField(TEXT("parent"), Parent->GetName());
			}

			WidgetSummaries.Add(MakeShareable(new FJsonValueObject(WidgetSummary)));
		}
	}

	Summary->SetNumberField(TEXT("widget_count"), AllWidgets.Num());
	Summary->SetArrayField(TEXT("widgets"), WidgetSummaries);

	if (WidgetTree->RootWidget)
	{
		Summary->SetStringField(TEXT("root_widget"), WidgetTree->RootWidget->GetName());
	}

	UE_LOG(LogOliveWidgetSerializer, Log, TEXT("Generated widget tree summary for '%s' with %d widgets"),
		*WidgetBlueprint->GetName(), AllWidgets.Num());

	return Summary;
}

// ============================================================================
// Widget Tree Traversal
// ============================================================================

FOliveIRWidgetNode FOliveWidgetTreeSerializer::SerializeWidget(const UWidget* Widget)
{
	FOliveIRWidgetNode Node;

	if (!Widget)
	{
		UE_LOG(LogOliveWidgetSerializer, Warning, TEXT("SerializeWidget called with null Widget"));
		return Node;
	}

	// Basic information
	Node.Name = Widget->GetName();
	Node.WidgetClass = GetCleanClassName(Widget->GetClass());

	// Extract properties
	Node.Properties = ExtractWidgetProperties(Widget);

	// Determine slot type
	Node.SlotType = DetermineSlotType(Widget);

	// Get children if this is a panel widget
	if (IsPanel(Widget))
	{
		TArray<UWidget*> Children = GetChildWidgets(Widget);
		for (UWidget* Child : Children)
		{
			if (Child)
			{
				FOliveIRWidgetNode ChildNode = SerializeWidget(Child);
				Node.Children.Add(MoveTemp(ChildNode));
			}
		}
	}

	return Node;
}

TArray<UWidget*> FOliveWidgetTreeSerializer::GetChildWidgets(const UWidget* Widget)
{
	TArray<UWidget*> Children;

	if (!Widget)
	{
		return Children;
	}

	const UPanelWidget* Panel = Cast<UPanelWidget>(Widget);
	if (Panel)
	{
		const int32 ChildCount = Panel->GetChildrenCount();
		for (int32 i = 0; i < ChildCount; ++i)
		{
			UWidget* Child = Panel->GetChildAt(i);
			if (Child)
			{
				Children.Add(Child);
			}
		}
	}

	return Children;
}

bool FOliveWidgetTreeSerializer::IsPanel(const UWidget* Widget) const
{
	return Widget && Widget->IsA<UPanelWidget>();
}

UWidget* FOliveWidgetTreeSerializer::FindWidgetByName(const UWidgetTree* WidgetTree, const FString& WidgetName) const
{
	if (!WidgetTree)
	{
		return nullptr;
	}

	TArray<UWidget*> AllWidgets;
	WidgetTree->GetAllWidgets(AllWidgets);

	for (UWidget* Widget : AllWidgets)
	{
		if (Widget && Widget->GetName() == WidgetName)
		{
			return Widget;
		}
	}

	return nullptr;
}

void FOliveWidgetTreeSerializer::TraverseWidgetTree(const UWidget* Widget, TArray<FOliveIRWidgetNode>& OutList)
{
	if (!Widget)
	{
		return;
	}

	// Serialize this widget
	FOliveIRWidgetNode Node = SerializeWidget(Widget);

	// Don't add children to this node for flat list, but do traverse them
	TArray<FOliveIRWidgetNode> Children = MoveTemp(Node.Children);
	Node.Children.Empty();

	OutList.Add(MoveTemp(Node));

	// Traverse children
	for (const FOliveIRWidgetNode& Child : Children)
	{
		// Need to look up the actual widget to continue traversal
		if (IsPanel(Widget))
		{
			TArray<UWidget*> ChildWidgets = GetChildWidgets(Widget);
			for (UWidget* ChildWidget : ChildWidgets)
			{
				TraverseWidgetTree(ChildWidget, OutList);
			}
		}
		break; // Already processed children above
	}
}

// ============================================================================
// Widget Property Reading
// ============================================================================

TMap<FString, FString> FOliveWidgetTreeSerializer::ExtractWidgetProperties(const UWidget* Widget)
{
	TMap<FString, FString> Properties;

	if (!Widget)
	{
		return Properties;
	}

	UClass* WidgetClass = Widget->GetClass();
	if (!WidgetClass)
	{
		return Properties;
	}

	// Get the Class Default Object for comparison
	UObject* CDO = WidgetClass->GetDefaultObject();
	if (!CDO)
	{
		return Properties;
	}

	// Iterate through all properties
	for (TFieldIterator<FProperty> PropIt(WidgetClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (!Property)
		{
			continue;
		}

		// Skip properties that shouldn't be exposed
		if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
		{
			continue;
		}

		// Skip deprecated properties
		if (Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			continue;
		}

		// Check if the property value differs from the default
		const void* InstanceValue = Property->ContainerPtrToValuePtr<void>(Widget);
		const void* DefaultValue = Property->ContainerPtrToValuePtr<void>(CDO);

		if (!InstanceValue || !DefaultValue)
		{
			continue;
		}

		// Use Identical for comparison
		if (!Property->Identical(InstanceValue, DefaultValue))
		{
			FString ValueStr = SerializePropertyValue(Property, InstanceValue);

			if (!ValueStr.IsEmpty())
			{
				Properties.Add(Property->GetName(), ValueStr);
			}
		}
	}

	// Also extract slot properties
	TMap<FString, FString> SlotProps = ExtractSlotProperties(Widget);
	for (const auto& Pair : SlotProps)
	{
		Properties.Add(FString::Printf(TEXT("Slot.%s"), *Pair.Key), Pair.Value);
	}

	return Properties;
}

FString FOliveWidgetTreeSerializer::DetermineSlotType(const UWidget* Widget)
{
	if (!Widget)
	{
		return TEXT("None");
	}

	UPanelSlot* Slot = Widget->Slot;
	if (!Slot)
	{
		return TEXT("None");
	}

	// Determine slot type based on class
	if (Slot->IsA<UCanvasPanelSlot>())
	{
		return TEXT("Canvas");
	}
	else if (Slot->IsA<UHorizontalBoxSlot>())
	{
		return TEXT("HorizontalBox");
	}
	else if (Slot->IsA<UVerticalBoxSlot>())
	{
		return TEXT("VerticalBox");
	}
	else if (Slot->IsA<UGridSlot>())
	{
		return TEXT("Grid");
	}
	else if (Slot->IsA<UUniformGridSlot>())
	{
		return TEXT("UniformGrid");
	}
	else if (Slot->IsA<UOverlaySlot>())
	{
		return TEXT("Overlay");
	}
	else if (Slot->IsA<UScrollBoxSlot>())
	{
		return TEXT("ScrollBox");
	}
	else if (Slot->IsA<USizeBoxSlot>())
	{
		return TEXT("SizeBox");
	}
	else if (Slot->IsA<UWrapBoxSlot>())
	{
		return TEXT("WrapBox");
	}

	return GetCleanClassName(Slot->GetClass());
}

TMap<FString, FString> FOliveWidgetTreeSerializer::ExtractSlotProperties(const UWidget* Widget)
{
	TMap<FString, FString> SlotProperties;

	if (!Widget || !Widget->Slot)
	{
		return SlotProperties;
	}

	UPanelSlot* Slot = Widget->Slot;
	UClass* SlotClass = Slot->GetClass();
	if (!SlotClass)
	{
		return SlotProperties;
	}

	// Get the Class Default Object for comparison
	UObject* CDO = SlotClass->GetDefaultObject();
	if (!CDO)
	{
		return SlotProperties;
	}

	// Iterate through all slot properties
	for (TFieldIterator<FProperty> PropIt(SlotClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (!Property)
		{
			continue;
		}

		// Skip properties that shouldn't be exposed
		if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
		{
			continue;
		}

		// Skip deprecated properties
		if (Property->HasAnyPropertyFlags(CPF_Deprecated))
		{
			continue;
		}

		// Check if the property value differs from the default
		const void* InstanceValue = Property->ContainerPtrToValuePtr<void>(Slot);
		const void* DefaultValue = Property->ContainerPtrToValuePtr<void>(CDO);

		if (!InstanceValue || !DefaultValue)
		{
			continue;
		}

		// Use Identical for comparison
		if (!Property->Identical(InstanceValue, DefaultValue))
		{
			FString ValueStr = SerializePropertyValue(Property, InstanceValue);

			if (!ValueStr.IsEmpty())
			{
				SlotProperties.Add(Property->GetName(), ValueStr);
			}
		}
	}

	return SlotProperties;
}

FString FOliveWidgetTreeSerializer::GetCleanClassName(const UClass* Class) const
{
	if (!Class)
	{
		return TEXT("Unknown");
	}

	FString ClassName = Class->GetName();

	// Remove common prefixes for cleaner display
	if (ClassName.StartsWith(TEXT("U"), ESearchCase::CaseSensitive))
	{
		ClassName.RemoveAt(0, 1);
	}

	return ClassName;
}

FString FOliveWidgetTreeSerializer::SerializePropertyValue(const FProperty* Property, const void* ValuePtr) const
{
	if (!Property || !ValuePtr)
	{
		return FString();
	}

	// Use ExportTextItem for conversion to string
	FString ValueStr;
	Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);

	return ValueStr;
}

// ============================================================================
// Widget Binding Reading
// ============================================================================

bool FOliveWidgetTreeSerializer::HasBindings(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
{
	if (!WidgetBlueprint || !Widget)
	{
		return false;
	}

	// Widget bindings are stored in the WidgetBlueprint's Bindings array
	// This is a simplified check - in a full implementation, we would iterate
	// through the bindings and check if any match this widget

	// PHASE2_DEFERRED: Widget binding extraction requires access to the WidgetBlueprint's
	// binding data structures. Deferred to Phase 2 when write support for bindings is added.

	return false;
}

TMap<FString, FString> FOliveWidgetTreeSerializer::GetWidgetBindings(
	const UWidgetBlueprint* WidgetBlueprint,
	const UWidget* Widget)
{
	TMap<FString, FString> Bindings;

	if (!WidgetBlueprint || !Widget)
	{
		return Bindings;
	}

	// PHASE2_DEFERRED: Widget binding extraction requires iterating through the
	// WidgetBlueprint's delegate bindings and matching them to this widget.

	UE_LOG(LogOliveWidgetSerializer, Verbose,
		TEXT("[PHASE2_DEFERRED] Widget binding extraction deferred to Phase 2"));

	return Bindings;
}

// ============================================================================
// Named Slot Reading
// ============================================================================

bool FOliveWidgetTreeSerializer::HasNamedSlots(const UWidget* Widget) const
{
	if (!Widget)
	{
		return false;
	}

	// Named slots are primarily used by UUserWidget and compound widgets
	// Check if the widget has any named slot properties
	UClass* WidgetClass = Widget->GetClass();
	if (!WidgetClass)
	{
		return false;
	}

	for (TFieldIterator<FProperty> PropIt(WidgetClass); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (Property && Property->GetName().Contains(TEXT("NamedSlot")))
		{
			return true;
		}
	}

	return false;
}

TMap<FString, FString> FOliveWidgetTreeSerializer::GetNamedSlots(const UWidget* Widget)
{
	TMap<FString, FString> NamedSlots;

	if (!Widget)
	{
		return NamedSlots;
	}

	// PHASE2_DEFERRED: Named slot extraction requires checking for INamedSlotInterface
	// and extracting slot metadata. Not critical for basic widget tree reading.

	UE_LOG(LogOliveWidgetSerializer, Verbose,
		TEXT("[PHASE2_DEFERRED] Named slot extraction deferred to Phase 2"));

	return NamedSlots;
}
