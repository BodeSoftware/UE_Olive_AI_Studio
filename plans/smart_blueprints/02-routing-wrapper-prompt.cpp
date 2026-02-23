// ============================================================================
// PATCH: OliveClaudeCodeProvider.cpp — Restructured Routing Wrapper
// Replace the Wrapper string (around line 72904-72920)
// ============================================================================
//
// FIND the entire Wrapper block:
//
//   const FString Wrapper =
//       TEXT("You have Olive AI Studio MCP tools. Use them to complete this task.\n\n")
//       TEXT("REQUIRED WORKFLOW:\n")
//       TEXT("1. blueprint.create — ...")
//       ...
//       TEXT("USER REQUEST: ");
//
// REPLACE WITH:

	const FString Wrapper =
		TEXT("You have Olive AI Studio MCP tools for Unreal Engine 5.5.\n\n")

		TEXT("## CRITICAL RULES (violating these causes failures)\n")
		TEXT("- Asset paths MUST end with the asset name: /Game/Blueprints/BP_Gun (NOT /Game/Blueprints/)\n")
		TEXT("- For graph logic, ALWAYS use blueprint.apply_plan_json with schema_version \"2.0\"\n")
		TEXT("- NEVER use individual add_node/connect_pins — plan JSON handles it atomically\n")
		TEXT("- Do NOT call blueprint.read_event_graph or blueprint.read BEFORE blueprint.create\n\n")

		TEXT("## WORKFLOW — follow this exact order, one call at a time\n")
		TEXT("1. blueprint.create → {\"path\": \"/Game/Blueprints/BP_Gun\", \"parent_class\": \"Actor\"}\n")
		TEXT("2. blueprint.add_component → {\"path\": \"/Game/Blueprints/BP_Gun\", \"component_class\": \"StaticMeshComponent\", \"name\": \"GunMesh\"}\n")
		TEXT("3. blueprint.add_variable → {\"path\": \"/Game/Blueprints/BP_Gun\", \"name\": \"FireRate\", \"type\": \"Float\", \"default_value\": \"0.5\"}\n")
		TEXT("4. blueprint.apply_plan_json → ALL graph logic in one call (see example below)\n")
		TEXT("5. blueprint.read → verify the result\n\n")

		TEXT("## PLAN JSON EXAMPLE (schema_version 2.0)\n")
		TEXT("{\"path\":\"/Game/Blueprints/BP_Gun\",\"plan\":{\n")
		TEXT("  \"schema_version\":\"2.0\", \"target_graph\":\"EventGraph\",\n")
		TEXT("  \"steps\":[\n")
		TEXT("    {\"id\":\"s1\",\"op\":\"event\",\"target\":\"BeginPlay\"},\n")
		TEXT("    {\"id\":\"s2\",\"op\":\"call\",\"target\":\"SetLifeSpan\",\"inputs\":{\"InLifespan\":\"3.0\"},\"exec_after\":\"s1\"}\n")
		TEXT("  ]\n")
		TEXT("}}\n\n")

		TEXT("Data wires: @step_id.auto (type-match), @step_id.~hint (fuzzy), @step_id.PinName (exact)\n")
		TEXT("Available ops: call, get_var, set_var, branch, sequence, cast, event, custom_event, for_loop, ")
		TEXT("for_each_loop, delay, is_valid, print_string, spawn_actor, make_struct, break_struct, return\n\n")

		TEXT("USER REQUEST: ");
