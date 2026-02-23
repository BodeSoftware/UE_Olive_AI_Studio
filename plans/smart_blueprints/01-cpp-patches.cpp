// ============================================================================
// PATCH: OliveBlueprintToolHandlers.cpp — HandleBlueprintCreate
// Apply after the path extraction block (around line 23563)
// ============================================================================
//
// FIND this existing code:
//
//   // Extract path
//   FString AssetPath;
//   if (!Params->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
//   {
//       return FOliveToolResult::Error(
//           TEXT("VALIDATION_MISSING_PARAM"),
//           TEXT("Required parameter 'path' is missing or empty"),
//           TEXT("Provide the Blueprint asset path (e.g., '/Game/Blueprints/BP_NewActor')")
//       );
//   }
//
// INSERT the following block AFTER it (before the parent_class extraction):

	// Defensive: detect folder-only paths before they reach the writer
	{
		FString ShortName = FPackageName::GetShortName(AssetPath);
		if (ShortName.IsEmpty() || AssetPath.EndsWith(TEXT("/")))
		{
			FOliveToolResult Result = FOliveToolResult::Error(
				TEXT("VALIDATION_PATH_IS_FOLDER"),
				FString::Printf(TEXT("'%s' is a folder path, not an asset path. The last segment must be the Blueprint name."), *AssetPath),
				FString::Printf(TEXT("Append a Blueprint name like '%s/BP_MyBlueprint'. Example: /Game/Blueprints/BP_Gun"), *AssetPath)
			);
			if (Result.Data.IsValid())
			{
				Result.Data->SetStringField(TEXT("self_correction_hint"),
					FString::Printf(TEXT("You used '%s' which is a folder. Add the BP name: '%s/BP_MyBlueprint'. Then retry blueprint.create."),
						*AssetPath, *AssetPath));
			}
			return Result;
		}

		// Also catch paths missing /Game/ prefix
		if (!AssetPath.StartsWith(TEXT("/Game/")))
		{
			FOliveToolResult Result = FOliveToolResult::Error(
				TEXT("VALIDATION_INVALID_PATH_PREFIX"),
				FString::Printf(TEXT("Path '%s' must start with '/Game/'."), *AssetPath),
				FString::Printf(TEXT("Use '/Game/Blueprints/%s' or another path under /Game/"), *ShortName)
			);
			if (Result.Data.IsValid())
			{
				Result.Data->SetStringField(TEXT("self_correction_hint"),
					FString::Printf(TEXT("Paths must start with /Game/. Try: /Game/Blueprints/%s"), *ShortName));
			}
			return Result;
		}
	}


// ============================================================================
// PATCH: OliveBlueprintWriter.cpp — CreateBlueprint path validation
// Replace the AssetName.IsEmpty() check (around line 40715)
// ============================================================================
//
// FIND:
//   if (AssetName.IsEmpty())
//   {
//       return FOliveBlueprintWriteResult::Error(TEXT("Invalid asset name"));
//   }
//
// REPLACE WITH:

	if (AssetName.IsEmpty())
	{
		return FOliveBlueprintWriteResult::Error(
			FString::Printf(
				TEXT("Path '%s' is a folder, not an asset path. "
					 "Append the Blueprint name, e.g. '%s/BP_MyBlueprint'. "
					 "The path must end with the asset name like '/Game/Blueprints/BP_Gun'."),
				*AssetPath, *AssetPath));
	}


// ============================================================================
// PATCH: OliveAISettings.h — Enable Plan JSON by default
// ============================================================================
//
// FIND:
//   bool bEnableBlueprintPlanJsonTools = false;
//
// REPLACE WITH:

	bool bEnableBlueprintPlanJsonTools = true;
