// Copyright Bode Software. All Rights Reserved.

#include "UI/SOliveAIContextBar.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Styling/AppStyle.h"
#include "AssetRegistry/AssetRegistryModule.h"

#define LOCTEXT_NAMESPACE "OliveAIContextBar"

void SOliveAIContextBar::Construct(const FArguments& InArgs)
{
	OnAssetRemoved = InArgs._OnAssetRemoved;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		.Visibility_Lambda([this]()
		{
			return (ManualContextAssets.Num() > 0 || !AutoContextPath.IsEmpty())
				? EVisibility::Visible
				: EVisibility::Collapsed;
		})
		[
			SNew(SHorizontalBox)

			// Label
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ContextLabel", "Context:"))
				.TextStyle(FAppStyle::Get(), "SmallText")
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			// Tags container
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(TagContainer, SWrapBox)
				.UseAllottedSize(true)
			]
		]
	];
}

// ==========================================
// Context Management
// ==========================================

void SOliveAIContextBar::AddContextAsset(const FString& AssetPath, const FString& DisplayName, bool bIsAutoContext)
{
	if (bIsAutoContext)
	{
		SetAutoContext(AssetPath, DisplayName);
		return;
	}

	// Don't add duplicates
	if (ManualContextAssets.Contains(AssetPath))
	{
		return;
	}

	ManualContextAssets.Add(AssetPath, DisplayName);
	RefreshDisplay();
}

void SOliveAIContextBar::RemoveContextAsset(const FString& AssetPath)
{
	if (AutoContextPath == AssetPath)
	{
		ClearAutoContext();
	}
	else if (ManualContextAssets.Remove(AssetPath) > 0)
	{
		AssetTagWidgets.Remove(AssetPath);
		RefreshDisplay();
	}

	OnAssetRemoved.ExecuteIfBound(AssetPath);
}

void SOliveAIContextBar::SetAutoContext(const FString& AssetPath, const FString& DisplayName)
{
	// Remove old auto context if different
	if (!AutoContextPath.IsEmpty() && AutoContextPath != AssetPath)
	{
		AssetTagWidgets.Remove(AutoContextPath);
	}

	AutoContextPath = AssetPath;
	AutoContextDisplayName = DisplayName;
	RefreshDisplay();
}

void SOliveAIContextBar::ClearAutoContext()
{
	if (!AutoContextPath.IsEmpty())
	{
		AssetTagWidgets.Remove(AutoContextPath);
		AutoContextPath.Empty();
		AutoContextDisplayName.Empty();
		RefreshDisplay();
	}
}

void SOliveAIContextBar::ClearManualContext()
{
	for (const auto& Pair : ManualContextAssets)
	{
		AssetTagWidgets.Remove(Pair.Key);
	}
	ManualContextAssets.Empty();
	RefreshDisplay();
}

void SOliveAIContextBar::ClearAllContext()
{
	ClearAutoContext();
	ClearManualContext();
}

TArray<FString> SOliveAIContextBar::GetContextAssetPaths() const
{
	TArray<FString> Paths;

	if (!AutoContextPath.IsEmpty())
	{
		Paths.Add(AutoContextPath);
	}

	for (const auto& Pair : ManualContextAssets)
	{
		Paths.Add(Pair.Key);
	}

	return Paths;
}

bool SOliveAIContextBar::HasContextAsset(const FString& AssetPath) const
{
	return AutoContextPath == AssetPath || ManualContextAssets.Contains(AssetPath);
}

// ==========================================
// Private Methods
// ==========================================

TSharedRef<SWidget> SOliveAIContextBar::BuildAssetTag(const FString& AssetPath, const FString& DisplayName, bool bIsAutoContext)
{
	// Tag color based on type
	FLinearColor TagColor = bIsAutoContext
		? FLinearColor(0.2f, 0.5f, 0.8f, 0.3f)  // Blue for auto
		: FLinearColor(0.4f, 0.4f, 0.4f, 0.3f); // Gray for manual

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.BorderBackgroundColor(TagColor)
		.Padding(FMargin(4, 2))
		[
			SNew(SHorizontalBox)

			// Auto indicator
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 4, 0)
			[
				SNew(SBox)
				.Visibility(bIsAutoContext ? EVisibility::Visible : EVisibility::Collapsed)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Edit"))
					.DesiredSizeOverride(FVector2D(10, 10))
					.ToolTipText(LOCTEXT("AutoContextTooltip", "Auto-detected from open editor"))
				]
			]

			// Asset name (clickable)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "NoBorder")
				.OnClicked(this, &SOliveAIContextBar::OnAssetTagClicked, AssetPath)
				.ToolTipText(FText::FromString(AssetPath))
				[
					SNew(STextBlock)
					.Text(FText::FromString(DisplayName))
					.TextStyle(FAppStyle::Get(), "SmallText")
				]
			]

			// Remove button
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0, 0, 0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.OnClicked(this, &SOliveAIContextBar::OnRemoveClicked, AssetPath)
				.ToolTipText(LOCTEXT("RemoveTooltip", "Remove from context"))
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.X"))
					.DesiredSizeOverride(FVector2D(10, 10))
				]
			]
		];
}

FReply SOliveAIContextBar::OnRemoveClicked(FString AssetPath)
{
	RemoveContextAsset(AssetPath);
	return FReply::Handled();
}

FReply SOliveAIContextBar::OnAssetTagClicked(FString AssetPath)
{
	// Open the asset in editor
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));

	if (AssetData.IsValid())
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(AssetData.GetAsset());
	}

	return FReply::Handled();
}

void SOliveAIContextBar::RefreshDisplay()
{
	if (!TagContainer.IsValid())
	{
		return;
	}

	TagContainer->ClearChildren();
	AssetTagWidgets.Empty();

	// Add auto context first
	if (!AutoContextPath.IsEmpty())
	{
		TSharedRef<SWidget> AutoTag = BuildAssetTag(AutoContextPath, AutoContextDisplayName, true);
		AssetTagWidgets.Add(AutoContextPath, AutoTag);

		TagContainer->AddSlot()
			.Padding(2)
			[
				AutoTag
			];
	}

	// Add manual context
	for (const auto& Pair : ManualContextAssets)
	{
		TSharedRef<SWidget> ManualTag = BuildAssetTag(Pair.Key, Pair.Value, false);
		AssetTagWidgets.Add(Pair.Key, ManualTag);

		TagContainer->AddSlot()
			.Padding(2)
			[
				ManualTag
			];
	}

	// Show selected nodes sub-pill
	if (SelectedNodeNames.Num() > 0)
	{
		FString NodeText;
		if (SelectedNodeNames.Num() <= 3)
		{
			NodeText = FString::Join(SelectedNodeNames, TEXT(", "));
		}
		else
		{
			NodeText = FString::Printf(TEXT("%d nodes selected"), SelectedNodeNames.Num());
		}

		TagContainer->AddSlot()
			.Padding(2)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.BorderBackgroundColor(FLinearColor(0.3f, 0.3f, 0.3f, 0.2f))
				.Padding(FMargin(4, 2))
				[
					SNew(STextBlock)
					.Text(FText::FromString(NodeText))
					.TextStyle(FAppStyle::Get(), "SmallText")
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Font(FCoreStyle::GetDefaultFontStyle("Italic", 8))
				]
			];
	}
}

void SOliveAIContextBar::SetSelectedNodes(const TArray<FString>& NodeNames)
{
	SelectedNodeNames = NodeNames;
	RefreshDisplay();
}

void SOliveAIContextBar::ClearSelectedNodes()
{
	SelectedNodeNames.Empty();
	RefreshDisplay();
}

#undef LOCTEXT_NAMESPACE
