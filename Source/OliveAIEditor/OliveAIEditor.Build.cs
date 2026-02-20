// Copyright Bode Software. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class OliveAIEditor : ModuleRules
{
	public OliveAIEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		string BlueprintRoot = Path.Combine(ModuleDirectory, "Blueprint");
		string BlueprintPublic = Path.Combine(BlueprintRoot, "Public");
		string BlueprintPrivate = Path.Combine(BlueprintRoot, "Private");

		// This module keeps headers under Blueprint/Public instead of Module/Public.
		// Add recursive include paths so legacy short includes (e.g. "OliveBlueprintTypes.h") resolve.
		if (Directory.Exists(BlueprintPublic))
		{
			PublicIncludePaths.Add(BlueprintPublic);
			foreach (string Dir in Directory.GetDirectories(BlueprintPublic, "*", SearchOption.AllDirectories))
			{
				PublicIncludePaths.Add(Dir);
			}
		}

		if (Directory.Exists(BlueprintPrivate))
		{
			PrivateIncludePaths.Add(BlueprintPrivate);
			foreach (string Dir in Directory.GetDirectories(BlueprintPrivate, "*", SearchOption.AllDirectories))
			{
				PrivateIncludePaths.Add(Dir);
			}
		}

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"OliveAIRuntime"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Editor
			"UnrealEd",
			"EditorFramework",
			"EditorStyle",
			"EditorSubsystem",
			"ToolMenus",
			"LevelEditor",
			"MainFrame",
			"WorkspaceMenuStructure",

			// UI
			"Slate",
			"SlateCore",
			"InputCore",
			"PropertyEditor",

			// Networking
			"HTTP",
			"HTTPServer",
			"Sockets",
			"Networking",

			// Data
			"Json",
			"JsonUtilities",

			// Asset Management
			"AssetRegistry",
			"AssetTools",
			"ContentBrowser",

			// Blueprint
			"BlueprintGraph",
			"Kismet",
			"KismetWidgets",
			"GraphEditor",
			"Blutility",

			// Animation Blueprint
			"AnimGraph",
			"AnimGraphRuntime",

			// Widget Blueprint
			"UMG",
			"UMGEditor",

			// Configuration
			"Projects",
			"DeveloperSettings",
			"Settings",
			"SettingsEditor"
		});
	}
}
