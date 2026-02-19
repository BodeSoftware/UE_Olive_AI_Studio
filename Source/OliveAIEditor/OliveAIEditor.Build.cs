// Copyright Bode Software. All Rights Reserved.

using UnrealBuildTool;

public class OliveAIEditor : ModuleRules
{
	public OliveAIEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

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

			// Blueprint (for future phases, register dependencies now)
			"BlueprintGraph",
			"Kismet",
			"KismetWidgets",
			"GraphEditor",

			// Configuration
			"Projects",
			"DeveloperSettings",
			"Settings",
			"SettingsEditor"
		});
	}
}
