# Olive AI Studio

AI development tooling for Unreal Engine 5.5+.

This plugin ships two main surfaces:

- an in-editor chat panel for asking questions, planning work, and running edits
- an MCP server so external agents like Claude Code can operate on Unreal assets through structured tools instead of touching `.uasset` files directly

The current codebase already includes real tool coverage for Blueprints, Animation Blueprints, Widget Blueprints, Behavior Trees, Blackboards, PCG graphs, C++ source edits, snapshots/rollback, template-driven generation, and editor Python execution.

## What Is In The Repo

- `Source/OliveAIEditor/` - editor module, UI, providers, MCP server, validation, write pipeline, Blueprint/BT/PCG/C++/Python tool handlers
- `Source/OliveAIRuntime/` - runtime IR types used by plan and compile results
- `Content/Templates/` - factory, reference, and library templates used by template search and Blueprint generation
- `Config/DefaultOliveAI.ini` - default plugin settings
- `mcp-bridge.js` - stdio-to-HTTP bridge used by Claude Code MCP connections

## Current Feature Set

- In-editor chat tab with `Code`, `Plan`, and `Ask` chat modes
- External MCP server with `tools/list`, `tools/call`, `resources/*`, `prompts/*`, and poll-based events
- Project tools like `project.search`, `project.get_asset_info`, `project.get_class_hierarchy`, `project.bulk_read`, `project.snapshot`, `project.rollback`, and `project.get_relevant_context`
- Blueprint tools for read/write, variables, components, functions, graph nodes, timelines, plan JSON preview/apply, and template-based creation
- Animation Blueprint, Widget Blueprint, Behavior Tree, Blackboard, PCG, and C++ tool families
- `olive.build` for batching many tool calls into one ordered operation
- `editor.run_python` for editor scripting when higher-level tools are not enough
- Multiple providers for the built-in chat: Claude Code, Codex, OpenRouter, Z.ai, Anthropic, OpenAI, Google AI, Ollama, and OpenAI-compatible endpoints

## Requirements

- Unreal Engine `5.5+`
- A C++ Unreal project
- Windows is the primary development path reflected by this repo and build setup
- Node.js if you want Claude Code MCP bridging through `mcp-bridge.js`
- One of:
  - an API-backed provider key for in-editor chat
  - Claude Code CLI for external/legacy Claude workflows
  - Codex CLI for external/legacy Codex workflows

## Install In A Project

1. Put the repo in your project as `Plugins/UE_Olive_AI_Studio`.
2. Open the Unreal project.
3. Enable `Olive AI Studio` if Unreal prompts you.
4. Let Unreal rebuild the modules if needed.
5. Restart the editor.

The plugin descriptor enables these plugin dependencies:

- `EditorScriptingUtilities`
- `PCG`
- `EnhancedInput`
- `PythonScriptPlugin`
- `SQLiteCore`

## First-Time Editor Setup

Open `Project Settings -> Plugins -> Olive AI Studio` and configure:

- `Provider` - defaults from config currently point at `ClaudeCode`
- provider credentials or local endpoint
- `Model`
- `Auto-Start Server` if you want MCP available as soon as the editor opens
- optional policy settings like auto-compile, rate limits, checkpoints, and plan JSON requirements

Important behavior from the code:

- the legacy in-editor `Claude Code` provider is disabled unless `Enable Legacy Claude Code Provider` is turned on
- the recommended Claude workflow in the codebase is external MCP, not the legacy embedded provider
- Blueprint plan JSON tools are enabled by default
- the MCP server auto-starts on port `3000` by default

## Open The UI

- `Tools -> Olive AI -> Olive AI Chat`
- `Tools -> Olive AI -> Olive AI -- Claude Code`
- shortcut: `Ctrl+Shift+O` opens the chat tab

The chat panel is backed by an editor-lifetime session, so closing the tab does not tear down in-flight work.

## In-Editor Chat Setup

If you want Olive to run directly inside Unreal without an external coding agent:

1. Choose a provider in `Project Settings -> Plugins -> Olive AI Studio`.
2. Fill in the matching API key, URL, or local model endpoint.
3. Pick a model.
4. Open `Tools -> Olive AI -> Olive AI Chat`.

Provider notes taken from the code:

- `OpenRouter`, `Anthropic`, `OpenAI`, `Google`, and `Z.ai` expect API keys in settings
- `Ollama` uses `http://localhost:11434` by default
- `OpenAI Compatible` needs a custom base URL; API key is optional
- `Codex` relies on Codex CLI auth or `OPENAI_API_KEY`
- `Claude Code` relies on the local Claude CLI install and its own auth flow

## Claude Code MCP Setup

This is the main external-agent workflow implemented by the repo.

1. Install Node.js.
2. Install Claude Code and confirm `claude` is on your `PATH`.
3. Start Unreal with the plugin enabled.
4. Make sure `Auto-Start Server` is enabled, or start the MCP server from the plugin UI.
5. Open Claude Code in `Plugins/UE_Olive_AI_Studio`.

What the code does for you:

- the MCP server exposes HTTP JSON-RPC at `/mcp`
- on startup it writes `Plugins/UE_Olive_AI_Studio/.mcp.json`
- that config points Claude Code at `mcp-bridge.js`
- `mcp-bridge.js` auto-discovers the server on ports `3000` through `3009`
- the generated `.mcp.json` is cleaned up again when the server stops

If you launch Claude Code from somewhere other than `Plugins/UE_Olive_AI_Studio`, you will need equivalent MCP config in the working directory or user-level Claude settings.

## Codex MCP Setup

The repo also contains a Codex CLI provider.

1. Install Codex CLI: `npm i -g @openai/codex`
2. Authenticate with `codex login` or provide `OPENAI_API_KEY`
3. Start Unreal with the plugin enabled

In the provider implementation, Codex connects directly to `http://localhost:<port>/mcp`, so it does not use `mcp-bridge.js`.

## How Olive Works

- The editor module loads at `PostEngineInit`
- It registers validation rules, project indexing, built-in project tools, Blueprint/BT/PCG/C++/Python tools, templates, prompts, and then starts the MCP server
- Writes go through a validation and execution pipeline with optional confirmation, transactions, verification, and structured results
- Multi-step tool batches can be executed through `olive.build`
- Snapshot tools provide rollback points before risky edits

## Tool Families

Representative tool groups currently registered in code:

- `project.*`
- `blueprint.*`
- `animbp.*`
- `widget.*`
- `blackboard.*`
- `behaviortree.*`
- `pcg.*`
- `cpp.*`
- `editor.run_python`
- `olive.build`
- `olive.get_recipe`
- `olive.search_community_blueprints`

## Development Notes

- `OliveAIRuntime` is the lightweight runtime module
- `OliveAIEditor` contains the editor-only implementation and most of the system
- tests live under `Source/OliveAIEditor/Private/Tests/`
- automation coverage exists for brain/chat behavior, conversation management, provider parsing, and PCG writer behavior

## Troubleshooting

### Plugin Does Not Load

- confirm the project is using Unreal Engine `5.5+`
- regenerate project files and rebuild the editor target
- check the Unreal Output Log and UnrealBuildTool log for the first real compile error

### Chat Opens But Cannot Respond

- verify your selected provider is actually configured in `Project Settings -> Plugins -> Olive AI Studio`
- if `Provider=ClaudeCode`, remember the legacy Claude provider must be explicitly enabled for in-editor use
- try switching to `OpenRouter`, `Anthropic`, `OpenAI`, `Google`, `Ollama`, or another configured provider

### Claude Code Cannot See Olive Tools

- make sure the Unreal editor is running with the plugin enabled
- confirm the MCP server started on port `3000` or another port in the `3000-3009` discovery range
- launch Claude Code from `Plugins/UE_Olive_AI_Studio` so it can pick up the generated `.mcp.json`
- make sure Node.js is installed so `mcp-bridge.js` can run

### Codex Cannot Connect

- confirm Codex CLI is installed and authenticated
- confirm the Unreal MCP server is running
- check that localhost access to `/mcp` is not blocked

## License

MIT

## Contributions

Contributions are accepted under the Contributor License Agreement in `CLA.md`.
