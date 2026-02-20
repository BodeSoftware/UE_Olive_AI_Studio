// Copyright Bode Software. All Rights Reserved.

using UnrealBuildTool;

public class OliveAIEditor : ModuleRules
{
	public OliveAIEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// NOTE: Blueprint folder is temporarily excluded until UE 5.5 API compatibility is fixed.
		// Core MCP server and chat UI work without it.

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

			// Blueprint (minimal - for project index)
			"BlueprintGraph",
			"Kismet",

			// Configuration
			"Projects",
			"DeveloperSettings",
			"Settings",
			"SettingsEditor"
		});
	}
}
