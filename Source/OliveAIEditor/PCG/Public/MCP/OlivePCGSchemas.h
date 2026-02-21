// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * PCG Tool Schema Builder
 *
 * Provides JSON Schema Draft 7 definitions for all PCG MCP tools.
 * Reuses common helpers from OliveBlueprintSchemas.
 */
namespace OlivePCGSchemas
{
	/** Schema for pcg.create: {path: string} */
	TSharedPtr<FJsonObject> PCGCreate();

	/** Schema for pcg.read: {path: string} */
	TSharedPtr<FJsonObject> PCGRead();

	/** Schema for pcg.add_node: {path, settings_class, pos_x?, pos_y?} */
	TSharedPtr<FJsonObject> PCGAddNode();

	/** Schema for pcg.remove_node: {path, node_id} */
	TSharedPtr<FJsonObject> PCGRemoveNode();

	/** Schema for pcg.connect: {path, source_node_id, source_pin, target_node_id, target_pin} */
	TSharedPtr<FJsonObject> PCGConnect();

	/** Schema for pcg.disconnect: {path, source_node_id, source_pin, target_node_id, target_pin} */
	TSharedPtr<FJsonObject> PCGDisconnect();

	/** Schema for pcg.set_settings: {path, node_id, properties: {key: value}} */
	TSharedPtr<FJsonObject> PCGSetSettings();

	/** Schema for pcg.add_subgraph: {path, subgraph_path, pos_x?, pos_y?} */
	TSharedPtr<FJsonObject> PCGAddSubgraph();

	/** Schema for pcg.execute: {path, timeout?} */
	TSharedPtr<FJsonObject> PCGExecute();
}
