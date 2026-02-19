# MCP Bridge for Claude Code CLI

This bridge connects Claude Code CLI to the Olive AI Studio MCP server running inside Unreal Editor.

## Architecture

```
Claude Code CLI <--stdio--> mcp-bridge.js <--HTTP--> Unreal Editor MCP Server
```

The bridge translates between:
- **stdio transport** (used by Claude Code CLI)
- **HTTP/JSON-RPC transport** (used by Unreal Editor MCP server)

## Quick Start

1. **Start Unreal Editor** with Olive AI Studio plugin enabled
2. **Open terminal** in the plugin folder: `cd YourProject/Plugins/UE_Olive_AI_Studio`
3. **Run Claude Code**: `claude`
4. **Verify connection**: Type `/mcp` - you should see `olive-ai-studio` listed

That's it! The bridge auto-discovers the MCP server on ports 3000-3009.

## Prerequisites

- **Node.js 14+** - Download from https://nodejs.org/
- **Unreal Editor** running with Olive AI Studio plugin

## How It Works

When Claude Code starts, it reads `.mcp.json` and spawns `node mcp-bridge.js`. The bridge:

1. **Auto-discovers** the MCP server by checking ports 3000-3009
2. **Connects** to the first responding port
3. **Forwards** all MCP requests from Claude Code to Unreal Editor
4. **Returns** responses back to Claude Code

## Manual Testing

```bash
# Test if the bridge can find the server
node mcp-bridge.js

# If Unreal is running, you'll see:
# [Olive MCP Bridge] Discovering Olive AI MCP server on localhost...
# [Olive MCP Bridge] Found MCP server on port 3000
# [Olive MCP Bridge] Connected to Olive AI MCP server at localhost:3000
```

## Configuration

The bridge auto-discovers the port, but you can override it:

```bash
node mcp-bridge.js --port 3001 --host localhost
```

Options:
- `--port` - Skip auto-discovery, use specific port
- `--host` - MCP server host (default: localhost)

## Available MCP Tools

Once connected, these tools are available:

| Tool | Description |
|------|-------------|
| `project.search` | Search for assets by name (fuzzy matching) |
| `project.get_asset_info` | Get detailed asset information |
| `project.get_class_hierarchy` | Get class inheritance tree |
| `project.get_dependencies` | Get asset dependencies |
| `project.get_referencers` | Get assets that reference this asset |
| `project.get_config` | Get project configuration |

## Troubleshooting

### "Could not find Olive AI MCP server"

The bridge couldn't find the server on ports 3000-3009.

1. Verify Unreal Editor is running
2. Check Output Log for: `[LogOliveAI] MCP Server started on port XXXX`
3. Ensure the plugin is enabled in Project Settings
4. Check if a firewall is blocking localhost connections

### Bridge starts but no tools available

1. Wait a few seconds after editor starts
2. Try `/mcp` in Claude Code to see connection status
3. Check Output Log for any MCP server errors

### "ECONNREFUSED" error

The server was found but then disconnected.

1. Unreal Editor may have closed or crashed
2. Restart Unreal Editor
3. Wait for "MCP Server started" message before using Claude Code

## Development

The bridge is a simple stdio-to-HTTP forwarder:
- Reads JSON-RPC requests from stdin (line-delimited)
- Forwards to `http://localhost:PORT/mcp` via POST
- Returns responses to stdout

Logs go to stderr (won't interfere with MCP protocol).
