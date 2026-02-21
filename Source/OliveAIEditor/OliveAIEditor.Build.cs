// Copyright Bode Software. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class OliveAIEditor : ModuleRules
{
	public OliveAIEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Add recursive include paths for sub-module directories (Blueprint, BehaviorTree, etc.)
		// so short includes (e.g. "OliveBlueprintTypes.h", "OliveBlackboardReader.h") resolve.
		string[] SubModules = { "Blueprint", "BehaviorTree", "PCG", "Cpp", "CrossSystem" };
		foreach (string SubModule in SubModules)
		{
			string SubRoot = Path.Combine(ModuleDirectory, SubModule);
			string SubPublic = Path.Combine(SubRoot, "Public");
			string SubPrivate = Path.Combine(SubRoot, "Private");

			if (Directory.Exists(SubPublic))
			{
				PublicIncludePaths.Add(SubPublic);
				foreach (string Dir in Directory.GetDirectories(SubPublic, "*", SearchOption.AllDirectories))
				{
					PublicIncludePaths.Add(Dir);
				}
			}

			if (Directory.Exists(SubPrivate))
			{
				PrivateIncludePaths.Add(SubPrivate);
				foreach (string Dir in Directory.GetDirectories(SubPrivate, "*", SearchOption.AllDirectories))
				{
					PrivateIncludePaths.Add(Dir);
				}
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

			// AI
			"AIModule",
			"GameplayTasks",

			// PCG
			"PCG",

			// Configuration
			"Projects",
			"DeveloperSettings",
			"Settings",
			"SettingsEditor",

			// C++ Integration
			"GameProjectGeneration",
			"LiveCoding",
			"SourceCodeAccess"
		});
	}
}
