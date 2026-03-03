// Copyright Bode Software. All Rights Reserved.
//
// DEPRECATED: Tool pack filtering removed in AI Freedom update. This class is
// retained for one release cycle. See header for details.

#include "Brain/OliveToolPackManager.h"
#include "Profiles/OliveFocusProfileManager.h"
#include "OliveAIEditorModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

FOliveToolPackManager& FOliveToolPackManager::Get()
{
	static FOliveToolPackManager Instance;
	return Instance;
}

void FOliveToolPackManager::Initialize()
{
	PackDefinitions.Empty();

	if (!LoadPacksFromConfig())
	{
		UE_LOG(LogOliveAI, Warning, TEXT("ToolPackManager: Failed to load config, using default packs"));
		RegisterDefaultPacks();
	}

	bInitialized = true;

	// Log pack sizes
	for (const auto& Pair : PackDefinitions)
	{
		UE_LOG(LogOliveAI, Log, TEXT("ToolPackManager: Pack '%s' has %d tools"),
			*PackToConfigKey(Pair.Key), Pair.Value.Num());
	}
}

TArray<FOliveToolDefinition> FOliveToolPackManager::GetPackTools(
	EOliveToolPack Pack,
	const FString& FocusProfileName) const
{
	TArray<FOliveToolDefinition> Result;

	const TSet<FString>* PackTools = PackDefinitions.Find(Pack);
	if (!PackTools)
	{
		return Result;
	}

	// Get all tools allowed by the profile
	TArray<FOliveToolDefinition> ProfileTools = FOliveToolRegistry::Get().GetToolsForProfile(FocusProfileName);

	// Intersect: only include tools that are both in the pack AND allowed by profile
	for (const FOliveToolDefinition& Tool : ProfileTools)
	{
		if (PackTools->Contains(Tool.Name))
		{
			Result.Add(Tool);
		}
	}

	return Result;
}

TArray<FOliveToolDefinition> FOliveToolPackManager::GetCombinedPackTools(
	const TArray<EOliveToolPack>& Packs,
	const FString& FocusProfileName) const
{
	// Collect all tool names from the requested packs
	TSet<FString> CombinedToolNames;
	for (EOliveToolPack Pack : Packs)
	{
		const TSet<FString>* PackTools = PackDefinitions.Find(Pack);
		if (PackTools)
		{
			CombinedToolNames.Append(*PackTools);
		}
	}

	// Get profile-filtered tools and intersect
	TArray<FOliveToolDefinition> ProfileTools = FOliveToolRegistry::Get().GetToolsForProfile(FocusProfileName);
	TArray<FOliveToolDefinition> Result;

	for (const FOliveToolDefinition& Tool : ProfileTools)
	{
		if (CombinedToolNames.Contains(Tool.Name))
		{
			Result.Add(Tool);
		}
	}

	return Result;
}

TArray<FString> FOliveToolPackManager::GetPackToolNames(EOliveToolPack Pack) const
{
	const TSet<FString>* PackTools = PackDefinitions.Find(Pack);
	if (!PackTools)
	{
		return TArray<FString>();
	}
	return PackTools->Array();
}

bool FOliveToolPackManager::IsToolInPack(const FString& ToolName, EOliveToolPack Pack) const
{
	const TSet<FString>* PackTools = PackDefinitions.Find(Pack);
	return PackTools && PackTools->Contains(ToolName);
}

int32 FOliveToolPackManager::GetPackSize(EOliveToolPack Pack) const
{
	const TSet<FString>* PackTools = PackDefinitions.Find(Pack);
	return PackTools ? PackTools->Num() : 0;
}

bool FOliveToolPackManager::LoadPacksFromConfig()
{
	const FString PluginDir = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UE_Olive_AI_Studio"));
	const FString ConfigPath = FPaths::Combine(PluginDir, TEXT("Config/OliveToolPacks.json"));

	if (!FPaths::FileExists(ConfigPath))
	{
		UE_LOG(LogOliveAI, Warning, TEXT("ToolPackManager: Config file not found: %s"), *ConfigPath);
		return false;
	}

	FString JsonContent;
	if (!FFileHelper::LoadFileToString(JsonContent, *ConfigPath))
	{
		UE_LOG(LogOliveAI, Warning, TEXT("ToolPackManager: Failed to read config file"));
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogOliveAI, Warning, TEXT("ToolPackManager: Failed to parse config JSON"));
		return false;
	}

	// Parse each pack
	auto ParsePack = [&](const FString& Key, EOliveToolPack Pack)
	{
		const TArray<TSharedPtr<FJsonValue>>* ToolsArray;
		if (Root->TryGetArrayField(Key, ToolsArray))
		{
			TSet<FString>& Tools = PackDefinitions.FindOrAdd(Pack);
			for (const TSharedPtr<FJsonValue>& Val : *ToolsArray)
			{
				if (Val.IsValid() && Val->Type == EJson::String)
				{
					Tools.Add(Val->AsString());
				}
			}
		}
	};

	ParsePack(TEXT("read_pack"), EOliveToolPack::ReadPack);
	ParsePack(TEXT("write_pack_basic"), EOliveToolPack::WritePackBasic);
	ParsePack(TEXT("write_pack_graph"), EOliveToolPack::WritePackGraph);
	ParsePack(TEXT("danger_pack"), EOliveToolPack::DangerPack);

	UE_LOG(LogOliveAI, Log, TEXT("ToolPackManager: Loaded packs from config"));
	return true;
}

void FOliveToolPackManager::RegisterDefaultPacks()
{
	// Fallback packs if config file is missing
	PackDefinitions.FindOrAdd(EOliveToolPack::ReadPack) = {
		TEXT("project.search"), TEXT("project.get_asset_info"),
		TEXT("project.get_class_hierarchy"), TEXT("project.get_dependencies"),
		TEXT("blueprint.read"), TEXT("blueprint.get_node_pins"), TEXT("blueprint.describe_node_type"),
		TEXT("behaviortree.read"), TEXT("blackboard.read"),
		TEXT("pcg.read_graph"),
		TEXT("cpp.read_class"), TEXT("cpp.read_header"),
		TEXT("olive.get_recipe")
	};

	PackDefinitions.FindOrAdd(EOliveToolPack::WritePackBasic) = {
		TEXT("blueprint.create"), TEXT("blueprint.add_variable"),
		TEXT("blueprint.add_component"), TEXT("blueprint.add_function"),
		TEXT("blueprint.compile"),
		TEXT("behaviortree.create"), TEXT("blackboard.add_key"),
		TEXT("pcg.create_graph"), TEXT("cpp.create_class")
	};

	PackDefinitions.FindOrAdd(EOliveToolPack::WritePackGraph) = {
		TEXT("blueprint.preview_plan_json"), TEXT("blueprint.apply_plan_json"),
		TEXT("blueprint.add_node"), TEXT("blueprint.remove_node"),
		TEXT("blueprint.connect_pins"), TEXT("blueprint.disconnect_pins"),
		TEXT("blueprint.set_pin_default"), TEXT("blueprint.set_node_property"),
		TEXT("blueprint.create_timeline")
	};

	PackDefinitions.FindOrAdd(EOliveToolPack::DangerPack) = {
		TEXT("blueprint.delete"), TEXT("blueprint.set_parent_class"),
		TEXT("blueprint.add_interface"), TEXT("blueprint.remove_interface")
	};
}

FString FOliveToolPackManager::PackToConfigKey(EOliveToolPack Pack)
{
	switch (Pack)
	{
	case EOliveToolPack::ReadPack: return TEXT("read_pack");
	case EOliveToolPack::WritePackBasic: return TEXT("write_pack_basic");
	case EOliveToolPack::WritePackGraph: return TEXT("write_pack_graph");
	case EOliveToolPack::DangerPack: return TEXT("danger_pack");
	default: return TEXT("unknown");
	}
}
