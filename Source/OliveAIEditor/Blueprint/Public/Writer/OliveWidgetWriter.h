// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OliveBlueprintWriter.h"

// Forward declarations
class UWidgetBlueprint;
class UWidget;
class UPanelWidget;
class UWidgetTree;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveWidgetWriter, Log, All);

/**
 * FOliveWidgetWriter
 *
 * Handles widget-level write operations for Widget Blueprints.
 * All operations use FScopedTransaction for undo support.
 *
 * This class provides methods to:
 * - Add new widgets to a Widget Blueprint's widget hierarchy
 * - Remove existing widgets
 * - Modify widget properties
 * - Bind widget properties to Blueprint functions
 *
 * Key Responsibilities:
 * - Validate widget operations against Widget Blueprint constraints
 * - Manage the Widget Tree for widget changes
 * - Handle parent-child relationships in the widget hierarchy
 * - Support all common widget types (Button, TextBlock, Image, etc.)
 * - Create and manage widget property bindings
 *
 * Usage:
 *   FOliveWidgetWriter& Writer = FOliveWidgetWriter::Get();
 *   auto Result = Writer.AddWidget("/Game/UI/WBP_MainMenu", "Button", "MyButton", "CanvasPanel_0");
 *   if (Result.bSuccess) { ... }
 */
class OLIVEAIEDITOR_API FOliveWidgetWriter
{
public:
	/**
	 * Get the singleton instance
	 * @return Reference to the widget writer singleton
	 */
	static FOliveWidgetWriter& Get();

	// ============================================================================
	// Widget Operations
	// ============================================================================

	/**
	 * Add a new widget to a Widget Blueprint
	 * @param AssetPath Path to the Widget Blueprint to modify
	 * @param WidgetClass Class name of the widget to add (e.g., "Button", "TextBlock", "Image")
	 * @param WidgetName Variable name for the new widget (auto-generated if empty)
	 * @param ParentWidgetName Name of the parent widget (empty for root)
	 * @param SlotType Optional slot type hint (e.g., "Canvas", "HorizontalBox")
	 * @param bIsVariable If true (default), the widget is exposed as a Blueprint variable so the
	 *        graph can reference it via get_var. Set false for purely decorative widgets.
	 * @return Result with widget name on success
	 */
	FOliveBlueprintWriteResult AddWidget(
		const FString& AssetPath,
		const FString& WidgetClass,
		const FString& WidgetName = TEXT(""),
		const FString& ParentWidgetName = TEXT(""),
		const FString& SlotType = TEXT(""),
		bool bIsVariable = true);

	/**
	 * Remove a widget from a Widget Blueprint
	 * @param AssetPath Path to the Widget Blueprint to modify
	 * @param WidgetName Name of the widget to remove
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult RemoveWidget(
		const FString& AssetPath,
		const FString& WidgetName);

	/**
	 * Set a property on a widget
	 * @param AssetPath Path to the Widget Blueprint to modify
	 * @param WidgetName Name of the widget to modify
	 * @param PropertyName Name of the property to set (e.g., "Text", "ColorAndOpacity", "Visibility")
	 * @param PropertyValue String representation of the property value
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult SetProperty(
		const FString& AssetPath,
		const FString& WidgetName,
		const FString& PropertyName,
		const FString& PropertyValue);

	/**
	 * Bind a widget property to a Blueprint function
	 * @param AssetPath Path to the Widget Blueprint to modify
	 * @param WidgetName Name of the widget with the property to bind
	 * @param PropertyName Name of the property to bind
	 * @param FunctionName Name of the function to bind to
	 * @return Result indicating success or failure
	 */
	FOliveBlueprintWriteResult BindProperty(
		const FString& AssetPath,
		const FString& WidgetName,
		const FString& PropertyName,
		const FString& FunctionName);

private:
	FOliveWidgetWriter() = default;
	~FOliveWidgetWriter() = default;

	// Non-copyable
	FOliveWidgetWriter(const FOliveWidgetWriter&) = delete;
	FOliveWidgetWriter& operator=(const FOliveWidgetWriter&) = delete;

	// ============================================================================
	// Private Helper Methods
	// ============================================================================

	/**
	 * Load a Widget Blueprint for editing with PIE check
	 * @param AssetPath Path to the Widget Blueprint to load
	 * @param OutError Error message if loading fails
	 * @return The Widget Blueprint if successful, nullptr otherwise
	 */
	UWidgetBlueprint* LoadWidgetBlueprintForEditing(const FString& AssetPath, FString& OutError);

	/**
	 * Find a widget in the widget tree by name
	 * @param WidgetTree The widget tree to search
	 * @param WidgetName Name of the widget to find
	 * @return The widget if found, nullptr otherwise
	 */
	UWidget* FindWidget(const UWidgetTree* WidgetTree, const FString& WidgetName);

	/**
	 * Find a widget class by name
	 * @param ClassName Class name (e.g., "Button", "TextBlock", "UButton", "UTextBlock")
	 * @return The widget class if found, nullptr otherwise
	 */
	UClass* FindWidgetClass(const FString& ClassName);

	/**
	 * Generate a unique widget name
	 * @param WidgetTree The widget tree to check for duplicates
	 * @param BaseClassName Base class name for the widget
	 * @return A unique widget name
	 */
	FString GenerateUniqueWidgetName(const UWidgetTree* WidgetTree, const FString& BaseClassName);

	/**
	 * Check if a widget class can have children
	 * @param WidgetClass The widget class to check
	 * @return True if the widget is a panel (can have children)
	 */
	bool IsPanelWidget(const UClass* WidgetClass) const;

	/**
	 * Mark a Widget Blueprint as modified for undo and save tracking
	 * @param WidgetBlueprint The Widget Blueprint to mark dirty
	 */
	void MarkDirty(UWidgetBlueprint* WidgetBlueprint);

	/**
	 * Check if Play-In-Editor is currently active
	 * @return True if PIE is active
	 */
	bool IsPIEActive() const;

	/**
	 * Set a property value on a widget using reflection
	 * @param Widget The widget to modify
	 * @param PropertyName Name of the property
	 * @param PropertyValue String representation of the value
	 * @param OutError Error message if setting fails
	 * @return True if property was set successfully
	 */
	bool SetWidgetProperty(
		UWidget* Widget,
		const FString& PropertyName,
		const FString& PropertyValue,
		FString& OutError);

	/**
	 * Create a property binding for a widget
	 * @param WidgetBlueprint The Widget Blueprint containing the widget
	 * @param Widget The widget to bind
	 * @param PropertyName Name of the property to bind
	 * @param FunctionName Name of the function to bind to
	 * @param OutError Error message if binding fails
	 * @return True if binding was created successfully
	 */
	bool CreatePropertyBinding(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* Widget,
		const FString& PropertyName,
		const FString& FunctionName,
		FString& OutError);
};
