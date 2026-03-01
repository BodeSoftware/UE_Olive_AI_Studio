# Olive AI: Autonomy & Improvisation Plan

## The Problem

Olive's AI agent knows UE5. It knows the correct architectural patterns — Blueprint Interfaces, overlap detection, interface message calls. But when it sits down to build, it only uses what the system explicitly offers: a curated list of plan_json ops, prescribed templates, and 90 MCP tools. When those don't cover what it needs, the AI doesn't improvise — it downgrades its design to fit the available vocabulary, or silently fails and burns time retrying.

This was proven in the gun pickup task. The AI read the template, understood BPI_Interactable was the right pattern, but couldn't express "call interface function on a target" through plan_json. Instead of finding another way, it abandoned the interface entirely and hardcoded a worse design using direct casts. The AI had the right idea and the wrong tools — and no permission or ability to route around the gap.

NeoStack (Betide Studio's competing plugin) takes the opposite approach: 27 simple tools, no batch planning layer, and a Python scripting escape hatch. The AI decides how to build. When the tools don't cover something, Python does. Their philosophy is: the AI is smart enough — just give it reliable primitives and get out of its way.

Olive doesn't need to become NeoStack. Plan_json's batch efficiency is a genuine advantage. But Olive needs to stop being a cage and start being a workshop — tools the AI *chooses* to use, not rails it's forced to follow.

---

## Priority 1: Give the AI Permission to Improvise (Prompting Philosophy)

### Why

This is the highest-priority change because without it, nothing else matters. Even if every tool works perfectly, the AI won't improvise unless the system tells it that improvisation is valid. Right now the prompts say "use these ops" and "follow this template." The AI reads that as "stay inside these lines."

### How

Rewrite the Worker_Blueprint prompt, the system prompt preamble, and template language to establish these principles:

- **Templates are reference material, not scripts.** They describe common architectures. The AI reads them for understanding, then decides its own approach.
- **Plan_json is a fast path, not the only path.** "When plan_json can express what you need, use it — it's faster. When it can't, use granular tools. When those don't work, use Python. Never simplify your design to fit a tool's limitations."
- **Your UE5 knowledge is valid.** "You know Unreal Engine. If you know a node type, a function, or a pattern exists, try it. Don't limit yourself to what's documented in recipes or alias maps."
- **Communicate what you can't do.** "If something genuinely can't be done with available tools, tell the user what you built, what you couldn't do, and why — instead of silently producing an incomplete result or wasting time retrying."
- **A working result with a creative approach beats a broken result that followed the prescribed pattern.**

This isn't adding instructions. It's removing the invisible constraint that makes the AI treat the tool vocabulary as the boundary of what's possible.

---

## Priority 2: Add `run_python` Tool — The Infinite Escape Hatch

### Why

UE5's surface area is enormous. Olive has 90 tools and there will always be a 91st thing the AI needs to do. You can't build tools fast enough to cover every edge case. NeoStack solved this with Python scripting — when their 27 tools don't cover something, the AI writes Python.

One tool eliminates the entire class of "we haven't thought of that yet" problems:

- InputKey property can't be set via reflection? Three lines of Python.
- Need a node type nobody planned for? Python can create any K2Node.
- Need to query something about the project no reader covers? Python.
- Need to set an obscure property on an obscure component? Python.
- Need to do something across multiple assets in a custom way? Python.

This changes the development model from "what tools do we need to build?" to "what tools are worth building for speed?" Plan_json is worth it because batch is faster. Common tools are worth it for convenience. But Python covers everything else, and the AI is never stuck.

### How

- Enable UE5's built-in Python Editor Scripting plugin (comes with 5.5+).
- Create a single MCP tool: `editor.run_python`
  - Input: Python script string
  - Execution: Runs in UE's Python scripting context via `FPythonScriptPlugin::ExecPythonCommand` or `IPythonScriptPlugin::ExecPythonCommandEx`
  - Output: Returns stdout/stderr and success/failure
  - Safety: Runs within UE's editor process, has access to the `unreal` module and all editor APIs
- The AI already knows UE's Python API (`unreal.EditorAssetLibrary`, `unreal.BlueprintEditorLibrary`, `unreal.SubobjectDataSubsystem`, etc.) from training data. No special documentation needed.
- Include in system prompt: "When standard tools can't do what you need, use `editor.run_python` to execute Python directly in the Unreal Editor. The `unreal` module gives you full access to editor APIs."

### Safety Requirements

Python scripts execute in the editor process — a bad script could crash the editor or corrupt assets in memory. Three mandatory safety layers:

1. **Automatic snapshot before execution.** Use `FOliveSnapshotManager` to take a snapshot before every `run_python` call. If the script corrupts state, the user can roll back with one click. This is the primary safety net — it protects against everything, so we don't need to restrict what the AI writes.

2. **Persistent script logging.** Every executed script is logged to a file (e.g., `Saved/OliveAI/PythonScripts.log`) with timestamp, the full script text, and the result (success/failure + output). This makes debugging easy when something goes wrong and provides an audit trail.

3. **Execution timeout.** Kill scripts that run longer than N seconds (e.g., 30s). Prevents infinite loops from hanging the editor. Use UE's `FPythonCommandEx` execution mode which supports cancellation.

4. **Automatic try/except wrapper.** Wrap every script in a try/except block so Python exceptions produce clean error messages instead of editor crashes.

**No size or complexity limits on scripts.** If the AI decides a 200-line script is the right approach, that's its call. The snapshot is the safety net — not script policing.

### Other Considerations

- Not a replacement for dedicated tools — Python is slower than plan_json batch ops. It's the fallback, not the primary path.
- The `PythonScriptPlugin` is marked Experimental in UE 5.5. Graceful degradation: if `IPythonScriptPlugin::Get()` returns nullptr (plugin not enabled), the tool returns a clear error telling the user to enable the plugin in Project Settings.
- UE's Python API: `IPythonScriptPlugin::ExecPythonCommandEx(FPythonCommandEx&)` — captures `CommandResult`, structured `LogOutput` (array of type + message), and success/failure.

---

## Priority 3: Fix Interface Function Resolution (Gap 1)

### Why

This is the specific code bug that blocks the most common pattern the AI fails at. When the AI writes a `call` op with `"class": "BPI_Interactable"`, FindFunction resolves the class, finds the function, but reports `ExactName` match instead of `InterfaceSearch`. The resolver never sets `bIsInterfaceCall = true`, so the executor creates a UK2Node_CallFunction instead of UK2Node_Message, causing a compile error.

This directly caused the gun pickup failure. The AI knew the BPI pattern, tried it, and the tools rejected it. The fix is ~5 lines.

### How

In `FindFunctionEx`, after Step 1 resolves a function on the explicitly specified class: check if that class `IsChildOf<UInterface>()`. If yes, report `InterfaceSearch` match method instead of `ExactName`. Then the existing pipeline flows correctly — resolver marks `bIsInterfaceCall = true`, executor creates UK2Node_Message, Target pin gets wired.

This unblocks:
- Calling interface functions on targets that the editing Blueprint doesn't implement
- The entire BPI_Interactable / DoesImplementInterface / interface message pattern
- Any future interface-based architecture the AI wants to use

---

## Priority 4: Reframe Templates as Context

### Why

Currently templates read like step-by-step instructions: "Step 1: Create BPI_Interactable. Step 2: Add interface to BP_Gun. Step 3: Implement Interact function..." The AI follows these literally, and when one step fails, the whole plan derails — it doesn't know the intent behind the steps, only the steps themselves.

Templates should describe *what* a system looks like, not *how* to build it. The AI figures out the how.

### How

Rewrite existing templates to be architectural descriptions:

**Before (prescriptive):**
```
1. Create BPI_Interactable Blueprint Interface with Interact() function
2. Open BP_Gun, add BPI_Interactable interface
3. Implement Interact function with pickup/drop toggle logic
4. In BP_ThirdPersonCharacter, add E key input
5. On E press, get overlapping actors, DoesImplementInterface check, and Interact call
```

**After (descriptive):**
```
A pickup interaction system typically involves:

- A Blueprint Interface (e.g. BPI_Interactable) defining an Interact() function
- Items implement this interface and handle their own pickup/drop logic in Interact()  
- The player character detects overlapping actors and calls Interact() on any that implement the interface
- This keeps the character decoupled from specific item types — any actor implementing BPI_Interactable is interactive

Key UE patterns: DoesImplementInterface for type checking, UK2Node_Message for interface calls, overlap events for proximity detection, input actions for player triggers.
```

The AI reads this and understands the architecture. How it builds it — plan_json, granular tools, Python, or some combination — is its choice.

---

## Priority 5: Reframe Plan_json as Optional Fast Path

### Why

Plan_json is genuinely powerful — one tool call creates an entire function graph with multiple nodes and connections. That's 10-15x fewer tool calls than granular node-by-node construction. It should stay.

But right now the system prompt presents plan_json ops as "what you can do" rather than "one efficient way to do common things." The AI sees `cast` in the ops list but not `interface_call`, so it uses `cast`. It follows the menu instead of cooking.

### How

Change the system prompt framing around plan_json:

**Before:**
```
Available plan_json operations:
event, custom_event, call, get_var, set_var, branch, sequence, cast, 
for_loop, for_each_loop, while_loop, do_once, flip_flop, gate, delay, 
is_valid, print_string, spawn_actor, make_struct, break_struct, return, comment
```

**After:**
```
You have multiple ways to build Blueprint graphs:

1. plan_json — Batch operation. Creates entire function graphs in one call.
   Efficient for common patterns. Supports these ops: event, custom_event, 
   call, get_var, set_var, branch, sequence, cast, for_loop, for_each_loop, 
   while_loop, do_once, flip_flop, gate, delay, is_valid, print_string, 
   spawn_actor, make_struct, break_struct, return, comment.
   
2. Granular tools — add_node, connect_pins, set_node_property. 
   Works with any UK2Node subclass. Use when plan_json can't express what you need.

3. editor.run_python — Execute Python in UE's editor scripting context.
   Full access to the unreal module. Use when no other tool covers what you need.

Use whichever approach fits the task. Plan_json for speed on common patterns.
Granular tools for specific node types. Python for anything else.
Don't simplify your design to fit plan_json — use a different tool instead.
```

---

## Priority 6: Improve Granular Tool Reliability

### Why

If the AI is going to fall back to add_node and connect_pins when plan_json can't handle something, those tools need to work reliably. Currently add_node with certain K2Node types creates ghost nodes (zero pins, no function bound), and failures are often silent.

NeoStack's approach — 500+ validation checks — is the right model. Every tool call should either succeed and produce a working result, or fail with a clear error that helps the AI try something else.

### How

- **Zero-pin guard (already implemented T1):** If add_node creates a K2Node_CallFunction with 0 pins, fail immediately with guidance.
- **Node inspection after creation:** Return the actual state of the created node — what pins exist, what properties were set, whether it's valid. The AI can verify its work and adjust.
- **Clear error messages with alternatives:** When a tool fails, say what went wrong AND suggest what to try instead. "Could not set InputKey property via reflection — try editor.run_python with unreal.K2Node_InputKeyEvent."
- **No silent failures:** Every tool call returns explicit success/failure. No half-created nodes left in the graph.

This is ongoing work, not a single task. Each time a tool fails silently in logs, that's a validation check to add.

---

## What This Changes

**Before:** AI receives a task → reads template → writes plan_json → hits a wall → retries same approach → downgrades design → produces incomplete result.

**After:** AI receives a task → reads template for context → plans its approach → uses plan_json for the bulk → hits something plan_json can't do → switches to add_node or Python → completes the task as designed → tells the user if anything needs manual attention.

The AI becomes a UE5 developer who happens to have fast batch tools available, rather than a plan_json executor that occasionally gets stuck.

---

## What NOT to Do

- **Don't add more recipes for specific patterns.** The AI already knows BPI interaction, overlap detection, input handling. Each recipe added is another thing the AI treats as mandatory.
- **Don't add more ops to plan_json for edge cases.** The ops list should cover common patterns. Edge cases go through granular tools or Python.
- **Don't add an orchestrator layer to manage tool selection.** The AI should choose its own tools. Adding a system that decides "use plan_json for this, granular for that" is adding more rails, not removing them.
- **Don't restrict Python to specific use cases.** The whole point is that it covers anything. Let the AI decide when to use it.

---

## Success Criteria

The gun pickup task (or similar multi-Blueprint interaction system) should:

1. Complete fully — not 70% done with missing logic
2. Use the correct architecture (BPI_Interactable, not hardcoded casts)
3. Finish in under 15 minutes (currently 27+ min and incomplete)
4. Handle novel patterns without getting stuck on tool limitations
5. Communicate to the user if anything couldn't be automated

The broader measure: when the AI encounters something new that no tool specifically supports, it finds a way instead of failing.
