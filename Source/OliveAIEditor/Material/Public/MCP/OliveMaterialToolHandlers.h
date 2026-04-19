// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MCP/OliveToolRegistry.h"

/**
 * FOliveMaterialToolHandlers
 *
 * Registers and handles material.* MCP tools: listing, reading, applying
 * materials to Blueprint components, creating material instances, and
 * setting vector parameters on material instances.
 *
 * Delegates to FOliveMaterialReader (read ops) and FOliveMaterialWriter (write ops).
 */
class OLIVEAIEDITOR_API FOliveMaterialToolHandlers
{
public:
    /** Get singleton instance */
    static FOliveMaterialToolHandlers& Get();

    /** Register all material.* tools with the tool registry */
    void RegisterAllTools();

    /** Unregister all material.* tools */
    void UnregisterAllTools();

private:
    FOliveMaterialToolHandlers() = default;
    FOliveMaterialToolHandlers(const FOliveMaterialToolHandlers&) = delete;
    FOliveMaterialToolHandlers& operator=(const FOliveMaterialToolHandlers&) = delete;

    /** Names of tools registered by this handler (for symmetric unregister) */
    TArray<FString> RegisteredToolNames;

    // Read handlers
    FOliveToolResult HandleList(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleRead(const TSharedPtr<FJsonObject>& Params);

    // Write handlers
    FOliveToolResult HandleApplyToComponent(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleSetParameterColor(const TSharedPtr<FJsonObject>& Params);
    FOliveToolResult HandleCreateInstance(const TSharedPtr<FJsonObject>& Params);
};
