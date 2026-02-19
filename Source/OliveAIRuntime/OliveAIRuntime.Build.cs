// Copyright Bode Software. All Rights Reserved.

using UnrealBuildTool;

public class OliveAIRuntime : ModuleRules
{
	public OliveAIRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Json",
			"JsonUtilities"
		});
	}
}
