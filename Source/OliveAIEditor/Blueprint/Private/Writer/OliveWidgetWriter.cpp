// Copyright Bode Software. All Rights Reserved.

#include "OliveWidgetWriter.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Editor.h"
#include "UObject/UObjectGlobals.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOliveWidgetWriter);

// ============================================================================
// Singleton Access
// ============================================================================

FOliveWidgetWriter& FOliveWidgetWriter::Get()
{
	static FOliveWidgetWriter Instance;
	return Instance;
}

// ============================================================================
// Widget Operations
// ============================================================================

FOliveBlueprintWriteResult FOliveWidgetWriter::AddWidget(
	const FString& AssetPath,
	const FString& WidgetClass,
	const FString& WidgetName,
	const FString& ParentWidgetName,
	const FString& SlotType)
{
	FString ErrorMsg;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForEditing(AssetPath, ErrorMsg);
	if (!WidgetBlueprint)
	{
		return FOliveBlueprintWriteResult::Error(ErrorMsg, AssetPath);
	}

	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Widget Blueprint while Play-In-Editor is active"),
			AssetPath);
	}

	// Find widget class
	UClass* WidgetClassObj = FindWidgetClass(WidgetClass);
	if (!WidgetClassObj)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Widget class '%s' not found. Use UMG class names like 'ProgressBar', 'TextBlock', 'Image', 'Overlay', 'CanvasPanel', etc."), *WidgetClass),
			AssetPath);
	}

	// Get widget tree
	UWidgetTree* WidgetTree = WidgetBlueprint->WidgetTree;
	if (!WidgetTree)
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Widget Blueprint has no WidgetTree"),
			AssetPath);
	}

	// Generate unique name if not provided
	FString FinalWidgetName = WidgetName.IsEmpty()
		? GenerateUniqueWidgetName(WidgetTree, WidgetClassObj->GetName())
		: WidgetName;

	// Check if name already exists
	if (FindWidget(WidgetTree, FinalWidgetName))
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Widget with name '%s' already exists"), *FinalWidgetName),
			AssetPath);
	}

	// Find parent widget if specified
	UPanelWidget* ParentWidget = nullptr;
	if (!ParentWidgetName.IsEmpty())
	{
		UWidget* ParentCandidate = FindWidget(WidgetTree, ParentWidgetName);
		if (!ParentCandidate)
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("Parent widget '%s' not found"), *ParentWidgetName),
				AssetPath);
		}

		ParentWidget = Cast<UPanelWidget>(ParentCandidate);
		if (!ParentWidget)
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("Widget '%s' is not a panel widget and cannot have children"), *ParentWidgetName),
				AssetPath);
		}
	}
	else if (WidgetTree->RootWidget)
	{
		// If no parent specified, try to add to root if it's a panel
		ParentWidget = Cast<UPanelWidget>(WidgetTree->RootWidget);
		if (!ParentWidget)
		{
			return FOliveBlueprintWriteResult::Error(
				TEXT("Root widget is not a panel and cannot have children. Specify a parent widget that is a panel."),
				AssetPath);
		}
	}

	// Begin transaction
	const FScopedTransaction Transaction(
		FText::Format(
			NSLOCTEXT("OliveAI", "AddWidget", "Add Widget '{0}'"),
			FText::FromString(FinalWidgetName)));

	WidgetBlueprint->Modify();

	// Create the new widget
	UWidget* NewWidget = WidgetTree->ConstructWidget<UWidget>(WidgetClassObj, *FinalWidgetName);
	if (!NewWidget)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Failed to construct widget of class '%s'"), *WidgetClass),
			AssetPath);
	}

	// Add to parent or set as root
	if (ParentWidget)
	{
		UPanelSlot* NewSlot = ParentWidget->AddChild(NewWidget);
		if (!NewSlot)
		{
			return FOliveBlueprintWriteResult::Error(
				FString::Printf(TEXT("Failed to add widget to parent '%s'"), *ParentWidgetName),
				AssetPath);
		}
	}
	else
	{
		// Set as root widget
		WidgetTree->RootWidget = NewWidget;
	}

	// Mark as modified
	MarkDirty(WidgetBlueprint);
	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);

	UE_LOG(LogOliveWidgetWriter, Log, TEXT("Added widget '%s' of class '%s' to '%s'"),
		*FinalWidgetName, *WidgetClass, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath, FinalWidgetName);
}

FOliveBlueprintWriteResult FOliveWidgetWriter::RemoveWidget(
	const FString& AssetPath,
	const FString& WidgetName)
{
	FString ErrorMsg;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForEditing(AssetPath, ErrorMsg);
	if (!WidgetBlueprint)
	{
		return FOliveBlueprintWriteResult::Error(ErrorMsg, AssetPath);
	}

	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Widget Blueprint while Play-In-Editor is active"),
			AssetPath);
	}

	// Get widget tree
	UWidgetTree* WidgetTree = WidgetBlueprint->WidgetTree;
	if (!WidgetTree)
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Widget Blueprint has no WidgetTree"),
			AssetPath);
	}

	// Find widget
	UWidget* Widget = FindWidget(WidgetTree, WidgetName);
	if (!Widget)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Widget '%s' not found"), *WidgetName),
			AssetPath);
	}

	// Begin transaction
	const FScopedTransaction Transaction(
		FText::Format(
			NSLOCTEXT("OliveAI", "RemoveWidget", "Remove Widget '{0}'"),
			FText::FromString(WidgetName)));

	WidgetBlueprint->Modify();

	// Remove from parent or root
	if (Widget == WidgetTree->RootWidget)
	{
		WidgetTree->RootWidget = nullptr;
	}
	else if (UPanelWidget* Parent = Widget->GetParent())
	{
		Parent->RemoveChild(Widget);
	}

	// Mark as modified
	MarkDirty(WidgetBlueprint);
	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);

	UE_LOG(LogOliveWidgetWriter, Log, TEXT("Removed widget '%s' from '%s'"),
		*WidgetName, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath);
}

FOliveBlueprintWriteResult FOliveWidgetWriter::SetProperty(
	const FString& AssetPath,
	const FString& WidgetName,
	const FString& PropertyName,
	const FString& PropertyValue)
{
	FString ErrorMsg;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForEditing(AssetPath, ErrorMsg);
	if (!WidgetBlueprint)
	{
		return FOliveBlueprintWriteResult::Error(ErrorMsg, AssetPath);
	}

	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Widget Blueprint while Play-In-Editor is active"),
			AssetPath);
	}

	// Get widget tree
	UWidgetTree* WidgetTree = WidgetBlueprint->WidgetTree;
	if (!WidgetTree)
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Widget Blueprint has no WidgetTree"),
			AssetPath);
	}

	// Find widget
	UWidget* Widget = FindWidget(WidgetTree, WidgetName);
	if (!Widget)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Widget '%s' not found"), *WidgetName),
			AssetPath);
	}

	// Begin transaction
	const FScopedTransaction Transaction(
		FText::Format(
			NSLOCTEXT("OliveAI", "SetWidgetProperty", "Set Widget Property '{0}.{1}'"),
			FText::FromString(WidgetName),
			FText::FromString(PropertyName)));

	WidgetBlueprint->Modify();
	Widget->Modify();

	// Set property
	if (!SetWidgetProperty(Widget, PropertyName, PropertyValue, ErrorMsg))
	{
		return FOliveBlueprintWriteResult::Error(ErrorMsg, AssetPath);
	}

	// Mark as modified
	MarkDirty(WidgetBlueprint);
	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);

	UE_LOG(LogOliveWidgetWriter, Log, TEXT("Set property '%s' = '%s' on widget '%s' in '%s'"),
		*PropertyName, *PropertyValue, *WidgetName, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath);
}

FOliveBlueprintWriteResult FOliveWidgetWriter::BindProperty(
	const FString& AssetPath,
	const FString& WidgetName,
	const FString& PropertyName,
	const FString& FunctionName)
{
	FString ErrorMsg;
	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintForEditing(AssetPath, ErrorMsg);
	if (!WidgetBlueprint)
	{
		return FOliveBlueprintWriteResult::Error(ErrorMsg, AssetPath);
	}

	if (IsPIEActive())
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Cannot modify Widget Blueprint while Play-In-Editor is active"),
			AssetPath);
	}

	// Get widget tree
	UWidgetTree* WidgetTree = WidgetBlueprint->WidgetTree;
	if (!WidgetTree)
	{
		return FOliveBlueprintWriteResult::Error(
			TEXT("Widget Blueprint has no WidgetTree"),
			AssetPath);
	}

	// Find widget
	UWidget* Widget = FindWidget(WidgetTree, WidgetName);
	if (!Widget)
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(TEXT("Widget '%s' not found"), *WidgetName),
			AssetPath);
	}

	// Begin transaction
	const FScopedTransaction Transaction(
		FText::Format(
			NSLOCTEXT("OliveAI", "BindWidgetProperty", "Bind Widget Property '{0}.{1}' to '{2}'"),
			FText::FromString(WidgetName),
			FText::FromString(PropertyName),
			FText::FromString(FunctionName)));

	WidgetBlueprint->Modify();

	// Create property binding
	if (!CreatePropertyBinding(WidgetBlueprint, Widget, PropertyName, FunctionName, ErrorMsg))
	{
		return FOliveBlueprintWriteResult::Error(ErrorMsg, AssetPath);
	}

	// Mark as modified
	MarkDirty(WidgetBlueprint);
	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);

	UE_LOG(LogOliveWidgetWriter, Log, TEXT("Bound property '%s' on widget '%s' to function '%s' in '%s'"),
		*PropertyName, *WidgetName, *FunctionName, *AssetPath);

	return FOliveBlueprintWriteResult::Success(AssetPath);
}

// ============================================================================
// Private Helper Methods
// ============================================================================

UWidgetBlueprint* FOliveWidgetWriter::LoadWidgetBlueprintForEditing(const FString& AssetPath, FString& OutError)
{
	// Load the asset
	UObject* LoadedObject = LoadObject<UObject>(nullptr, *AssetPath);
	if (!LoadedObject)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path '%s'"), *AssetPath);
		return nullptr;
	}

	// Check if it's a Widget Blueprint
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(LoadedObject);
	if (!WidgetBlueprint)
	{
		OutError = FString::Printf(TEXT("Asset at '%s' is not a Widget Blueprint (type: %s)"),
			*AssetPath, *LoadedObject->GetClass()->GetName());
		return nullptr;
	}

	return WidgetBlueprint;
}

UWidget* FOliveWidgetWriter::FindWidget(const UWidgetTree* WidgetTree, const FString& WidgetName)
{
	if (!WidgetTree)
	{
		return nullptr;
	}

	// Search all widgets in the tree
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

UClass* FOliveWidgetWriter::FindWidgetClass(const FString& ClassName)
{
	// Try with U prefix
	FString ClassNameWithU = ClassName;
	if (!ClassNameWithU.StartsWith(TEXT("U")))
	{
		ClassNameWithU = TEXT("U") + ClassName;
	}

	// Try to find the class
	UClass* FoundClass = FindObject<UClass>(nullptr, *ClassNameWithU);
	if (FoundClass && FoundClass->IsChildOf(UWidget::StaticClass()))
	{
		return FoundClass;
	}

	// Try without U prefix
	FString ClassNameWithoutU = ClassName;
	if (ClassNameWithoutU.StartsWith(TEXT("U")))
	{
		ClassNameWithoutU = ClassName.RightChop(1);
	}

	FoundClass = FindObject<UClass>(nullptr, *ClassNameWithoutU);
	if (FoundClass && FoundClass->IsChildOf(UWidget::StaticClass()))
	{
		return FoundClass;
	}

	// Try common widget classes
	static const TMap<FString, UClass*> CommonWidgets = {
		{TEXT("Button"), UButton::StaticClass()},
		{TEXT("TextBlock"), UTextBlock::StaticClass()},
		{TEXT("Image"), UImage::StaticClass()},
		{TEXT("CanvasPanel"), UCanvasPanel::StaticClass()},
		{TEXT("HorizontalBox"), UHorizontalBox::StaticClass()},
		{TEXT("VerticalBox"), UVerticalBox::StaticClass()}
	};

	if (const UClass* const* CommonClass = CommonWidgets.Find(ClassNameWithoutU))
	{
		return const_cast<UClass*>(*CommonClass);
	}

	// Fallback: try StaticLoadClass with UMG module path
	// This covers ALL UMG widgets (ProgressBar, Overlay, ScrollBox, etc.) without enumeration
	FString UMGPath = FString::Printf(TEXT("/Script/UMG.%s"), *ClassNameWithoutU);
	FoundClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *UMGPath);
	if (FoundClass)
	{
		return FoundClass;
	}

	// Also try CommonUI module path
	FString CommonUIPath = FString::Printf(TEXT("/Script/CommonUI.%s"), *ClassNameWithoutU);
	FoundClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *CommonUIPath);
	if (FoundClass)
	{
		return FoundClass;
	}

	return nullptr;
}

FString FOliveWidgetWriter::GenerateUniqueWidgetName(const UWidgetTree* WidgetTree, const FString& BaseClassName)
{
	// Remove 'U' prefix if present
	FString BaseName = BaseClassName;
	if (BaseName.StartsWith(TEXT("U")))
	{
		BaseName = BaseName.RightChop(1);
	}

	// Try base name first
	if (!FindWidget(WidgetTree, BaseName))
	{
		return BaseName;
	}

	// Add numeric suffix
	int32 Counter = 0;
	FString CandidateName;
	do
	{
		CandidateName = FString::Printf(TEXT("%s_%d"), *BaseName, Counter++);
	} while (FindWidget(WidgetTree, CandidateName));

	return CandidateName;
}

bool FOliveWidgetWriter::IsPanelWidget(const UClass* WidgetClass) const
{
	return WidgetClass && WidgetClass->IsChildOf(UPanelWidget::StaticClass());
}

void FOliveWidgetWriter::MarkDirty(UWidgetBlueprint* WidgetBlueprint)
{
	if (WidgetBlueprint)
	{
		WidgetBlueprint->Modify();
		WidgetBlueprint->MarkPackageDirty();
	}
}

bool FOliveWidgetWriter::IsPIEActive() const
{
	return GEditor && GEditor->PlayWorld != nullptr;
}

bool FOliveWidgetWriter::SetWidgetProperty(
	UWidget* Widget,
	const FString& PropertyName,
	const FString& PropertyValue,
	FString& OutError)
{
	if (!Widget)
	{
		OutError = TEXT("Widget is null");
		return false;
	}

	// Find the property on the widget
	FProperty* Property = Widget->GetClass()->FindPropertyByName(*PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on widget class '%s'"),
			*PropertyName, *Widget->GetClass()->GetName());
		return false;
	}

	// Try to import the value
	void* PropertyAddress = Property->ContainerPtrToValuePtr<void>(Widget);
	if (!Property->ImportText_Direct(*PropertyValue, PropertyAddress, Widget, PPF_None))
	{
		OutError = FString::Printf(TEXT("Failed to import value '%s' for property '%s'"),
			*PropertyValue, *PropertyName);
		return false;
	}

	return true;
}

bool FOliveWidgetWriter::CreatePropertyBinding(
	UWidgetBlueprint* WidgetBlueprint,
	UWidget* Widget,
	const FString& PropertyName,
	const FString& FunctionName,
	FString& OutError)
{
	if (!WidgetBlueprint || !Widget)
	{
		OutError = TEXT("Invalid Widget Blueprint or Widget");
		return false;
	}

	// DESIGN NOTE: Widget property bindings in UE use the DelegateBinding system
	// which is complex and requires deep integration with the Widget Blueprint compiler.
	// For Phase 1, we'll provide a basic implementation that documents the approach
	// but may need enhancement in Phase 2 for full binding support.

	// Verify the function exists in the Blueprint
	UEdGraph* FunctionGraph = nullptr;
	for (UEdGraph* Graph : WidgetBlueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == *FunctionName)
		{
			FunctionGraph = Graph;
			break;
		}
	}

	if (!FunctionGraph)
	{
		OutError = FString::Printf(TEXT("Function '%s' not found in Widget Blueprint"), *FunctionName);
		return false;
	}

	// Verify the property exists on the widget
	FProperty* Property = Widget->GetClass()->FindPropertyByName(*PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on widget '%s'"),
			*PropertyName, *Widget->GetName());
		return false;
	}

	// UMG bindings require the widget to be a variable in the Widget Blueprint
	if (!Widget->bIsVariable)
	{
		OutError = FString::Printf(
			TEXT("Widget '%s' must be marked as a variable (bIsVariable=true) for property binding. "
			     "Use widget.set_property to set bIsVariable first."),
			*Widget->GetName());
		return false;
	}

	// Create the editor binding entry that the Widget Blueprint compiler
	// will transform into a runtime delegate binding during compilation
	FDelegateEditorBinding Binding;
	Binding.ObjectName = Widget->GetName();
	Binding.PropertyName = *PropertyName;
	Binding.FunctionName = *FunctionName;
	Binding.Kind = EBindingKind::Function;

	// Resolve function GUID for rename resilience — the compiler uses this
	// to survive function renames between compiles
	UFunction* Func = WidgetBlueprint->SkeletonGeneratedClass
		? WidgetBlueprint->SkeletonGeneratedClass->FindFunctionByName(*FunctionName)
		: nullptr;
	if (!Func)
	{
		Func = WidgetBlueprint->GeneratedClass
			? WidgetBlueprint->GeneratedClass->FindFunctionByName(*FunctionName)
			: nullptr;
	}
	if (Func)
	{
		UBlueprint::GetGuidFromClassByFieldName<UFunction>(
			Func->GetOwnerClass(), Func->GetFName(), Binding.MemberGuid);
	}

	// Remove any existing binding for the same object+property pair
	// (operator== on FDelegateEditorBinding matches on ObjectName+PropertyName only)
	WidgetBlueprint->Bindings.Remove(Binding);
	WidgetBlueprint->Bindings.Add(Binding);

	return true;
}
