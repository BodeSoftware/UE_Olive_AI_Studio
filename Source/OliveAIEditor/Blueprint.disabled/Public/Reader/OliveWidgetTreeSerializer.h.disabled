// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IR/BlueprintIR.h"

// Forward declarations
class UWidgetBlueprint;
class UWidget;
class UPanelWidget;
class UWidgetTree;

DECLARE_LOG_CATEGORY_EXTERN(LogOliveWidgetSerializer, Log, All);

/**
 * Widget Tree Serializer
 *
 * Specialized serializer for Widget Blueprint widget hierarchies.
 * Handles:
 * - Widget tree structure with parent/child relationships
 * - Widget properties (visibility, transform, anchors, etc.)
 * - Slot properties (canvas slots, box slots, etc.)
 * - Widget bindings (dynamic properties)
 * - Named slots for compound widgets
 *
 * This serializer produces FOliveIRWidgetNode structures that capture
 * the full widget hierarchy, enabling AI agents to understand and
 * manipulate UMG layouts.
 *
 * Usage:
 *   FOliveWidgetTreeSerializer Serializer;
 *   TOptional<FOliveIRWidgetNode> Root = Serializer.ReadWidgetTree(WidgetBlueprint);
 */
class OLIVEAIEDITOR_API FOliveWidgetTreeSerializer
{
public:
	FOliveWidgetTreeSerializer();
	~FOliveWidgetTreeSerializer() = default;

	// ============================================================================
	// High-Level Read Methods
	// ============================================================================

	/**
	 * Read the entire widget tree from a Widget Blueprint
	 * @param WidgetBlueprint The Widget Blueprint to read from
	 * @return Root widget node with all children recursively populated
	 */
	TOptional<FOliveIRWidgetNode> ReadWidgetTree(const UWidgetBlueprint* WidgetBlueprint);

	/**
	 * Read a specific widget by name
	 * @param WidgetBlueprint The Widget Blueprint
	 * @param WidgetName Name of the widget to read
	 * @return The widget IR, or empty optional if not found
	 */
	TOptional<FOliveIRWidgetNode> ReadWidget(
		const UWidgetBlueprint* WidgetBlueprint,
		const FString& WidgetName);

	/**
	 * Read widget tree as flat list (for simpler processing)
	 * @param WidgetBlueprint The Widget Blueprint
	 * @return Array of all widgets in depth-first order
	 */
	TArray<FOliveIRWidgetNode> ReadWidgetTreeFlat(const UWidgetBlueprint* WidgetBlueprint);

	/**
	 * Get widget tree summary (names and types only, no properties)
	 * @param WidgetBlueprint The Widget Blueprint
	 * @return JSON summary of widget structure
	 */
	TSharedPtr<FJsonObject> ReadWidgetTreeSummary(const UWidgetBlueprint* WidgetBlueprint);

private:
	// ============================================================================
	// Widget Tree Traversal
	// ============================================================================

	/**
	 * Recursively serialize a widget and all its children
	 */
	FOliveIRWidgetNode SerializeWidget(const UWidget* Widget);

	/**
	 * Get children of a panel widget
	 */
	TArray<UWidget*> GetChildWidgets(const UWidget* Widget);

	/**
	 * Check if a widget is a panel (can have children)
	 */
	bool IsPanel(const UWidget* Widget) const;

	/**
	 * Find a widget by name in the widget tree
	 */
	UWidget* FindWidgetByName(const UWidgetTree* WidgetTree, const FString& WidgetName) const;

	/**
	 * Recursively traverse widget tree and add to flat list
	 */
	void TraverseWidgetTree(const UWidget* Widget, TArray<FOliveIRWidgetNode>& OutList);

	// ============================================================================
	// Widget Property Reading
	// ============================================================================

	/**
	 * Extract common widget properties (visibility, render transform, etc.)
	 */
	TMap<FString, FString> ExtractWidgetProperties(const UWidget* Widget);

	/**
	 * Extract slot-specific properties (anchors for canvas, alignment for box, etc.)
	 */
	FString DetermineSlotType(const UWidget* Widget);

	/**
	 * Read slot properties for a widget
	 */
	TMap<FString, FString> ExtractSlotProperties(const UWidget* Widget);

	/**
	 * Get clean class name without prefix (e.g., "Button" instead of "UButton")
	 */
	FString GetCleanClassName(const UClass* Class) const;

	/**
	 * Serialize a property value to string
	 */
	FString SerializePropertyValue(const FProperty* Property, const void* ValuePtr) const;

	// ============================================================================
	// Widget Binding Reading
	// ============================================================================

	/**
	 * Check if a widget has any property bindings
	 */
	bool HasBindings(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget);

	/**
	 * Get all property bindings for a widget
	 * Returns map of property name -> binding expression
	 */
	TMap<FString, FString> GetWidgetBindings(
		const UWidgetBlueprint* WidgetBlueprint,
		const UWidget* Widget);

	// ============================================================================
	// Named Slot Reading
	// ============================================================================

	/**
	 * Check if a widget has named slots (UserWidget, etc.)
	 */
	bool HasNamedSlots(const UWidget* Widget) const;

	/**
	 * Get named slot information
	 */
	TMap<FString, FString> GetNamedSlots(const UWidget* Widget);

	// ============================================================================
	// State
	// ============================================================================

	/** Current WidgetBlueprint being serialized (for binding lookups) */
	const UWidgetBlueprint* CurrentWidgetBlueprint = nullptr;
};
