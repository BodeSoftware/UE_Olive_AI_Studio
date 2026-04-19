// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

/**
 * FOliveLevelToolHandlers
 *
 * Registers and handles level.* MCP tools: listing, finding, spawning,
 * deleting, and modifying actors in the current editor level.
 *
 * Delegates to FOliveLevelReader (read ops) and FOliveLevelWriter (write ops).
 */
class OLIVEAIEDITOR_API FOliveLevelToolHandlers
{
public:
    /** Get singleton instance */
    static FOliveLevelToolHandlers& Get();

    /** Register all level.* tools with the tool registry */
    void RegisterAllTools();

    /** Unregister all level.* tools */
    void UnregisterAllTools();

private:
    FOliveLevelToolHandlers() = default;
    FOliveLevelToolHandlers(const FOliveLevelToolHandlers&) = delete;
    FOliveLevelToolHandlers& operator=(const FOliveLevelToolHandlers&) = delete;

    /** Names of tools registered by this handler (for symmetric unregister) */
    TArray<FString> RegisteredToolNames;

    // Read handlers
    FOliveToolResult HandleListActors(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleFindActors(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleGetActorMaterials(const TSharedPtr<FJsonObject>& Params);

    // Write handlers
    FOliveToolResult HandleSpawnActor(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleDeleteActor(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleSetTransform(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleSetPhysics(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleApplyMaterial(const TSharedPtr<FJsonObject>& Params);
};
