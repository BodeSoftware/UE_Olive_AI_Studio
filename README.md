# Olive AI Studio

**AI-Powered Development Assistant for Unreal Engine 5.5+**

Olive AI Studio brings intelligent AI assistance directly into Unreal Engine, helping you build games faster with natural language commands.

![Version](https://img.shields.io/badge/version-0.1.0-blue.svg)
![UE Version](https://img.shields.io/badge/Unreal%20Engine-5.5+-orange.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)

## Features

- **Built-in AI Chat** - Dockable chat panel inside the editor
- **Claude Code Integration** - Use Claude Code CLI with your UE project
- **Smart Asset Search** - Natural language asset discovery
- **Project Intelligence** - Class hierarchies, dependencies, and more
- **Coming Soon**: Blueprint editing, Behavior Trees, PCG graphs, C++ integration

## Installation

### Option 1: Quick Install (Recommended)

1. **Download the Plugin**
   - Download the latest release ZIP from GitHub
   - Or clone this repository

2. **Copy to Your Project**
   ```
   YourProject/
   └── Plugins/
       └── UE_Olive_AI_Studio/    <- Copy the entire plugin folder here
   ```

3. **Enable the Plugin**
   - Open your Unreal Engine project
   - Go to **Edit > Plugins**
   - Search for "Olive AI Studio"
   - Check the **Enabled** checkbox
   - Click **Restart Now**

4. **Configure Your API Key**
   - Go to **Edit > Project Settings**
   - Navigate to **Plugins > Olive AI Studio**
   - Enter your OpenRouter API key (or other provider)
   - Click **Save**

That's it! You're ready to use Olive AI Studio.

### Option 2: Engine Plugin Install

To install for all projects:

1. Copy the plugin folder to your engine's plugins directory:
   ```
   UE_5.5/
   └── Engine/
       └── Plugins/
           └── UE_Olive_AI_Studio/
   ```

2. Restart Unreal Engine
3. Enable the plugin in each project's Plugin settings

## Quick Start

### Using the Built-in Chat

1. Open the chat panel: **Window > Olive AI Studio**
2. Type your request in natural language:
   - "Find all blueprints that inherit from PlayerController"
   - "Show me the class hierarchy for UActorComponent"
   - "What assets reference BP_Player?"
3. Get instant results with clickable asset links

### Using with Claude Code CLI

If you have Claude Code installed:

1. **Install Node.js** (required for MCP bridge)
   - Download from: https://nodejs.org/
   - Any recent version (14+) works

2. **Start Unreal Editor**
   - Open your project with Olive AI Studio enabled
   - The MCP server starts automatically

3. **Use Claude Code**
   - Open Claude Code in your project directory
   - Type commands like: "Use the project.search tool to find BP_Player"
   - Claude Code will automatically connect to the MCP server

No additional configuration needed - it just works!

## Features in Detail

### Current Features (Phase 0)

| Feature | Description |
|---------|-------------|
| **Asset Search** | Fuzzy search across all project assets |
| **Asset Info** | Detailed metadata for any asset |
| **Class Hierarchy** | Full inheritance trees |
| **Dependencies** | What assets depend on what |
| **Referencers** | Find what references an asset |
| **Project Config** | Access project settings |

### Coming Soon

| Phase | Features | Status |
|-------|----------|--------|
| **Phase 1** | Blueprint reading & editing | In development |
| **Phase 2** | Behavior Trees & Blackboards | Planned |
| **Phase 3** | PCG graph editing | Planned |
| **Phase 4** | C++ integration | Planned |

## Configuration

Access settings via **Edit > Project Settings > Plugins > Olive AI Studio**

### AI Provider Settings

| Setting | Description | Default |
|---------|-------------|---------|
| **Provider Type** | OpenRouter, Anthropic, OpenAI, etc. | OpenRouter |
| **API Key** | Your provider's API key | (empty) |
| **Model ID** | Model to use (e.g., anthropic/claude-3.5-sonnet) | claude-3.5-sonnet |
| **Max Tokens** | Maximum response length | 4096 |
| **Temperature** | Creativity level (0.0-1.0) | 0.7 |

### MCP Server Settings

| Setting | Description | Default |
|---------|-------------|---------|
| **Auto-Start Server** | Start MCP server on editor launch | Enabled |
| **Server Port** | Port for MCP connections | 3000 |
| **Enable Rate Limiting** | Prevent API abuse | Enabled |

### UI Settings

| Setting | Description | Default |
|---------|-------------|---------|
| **Show Timestamps** | Show message times in chat | Enabled |
| **Auto-Scroll** | Auto-scroll to new messages | Enabled |
| **Max History** | Maximum conversation length | 50 messages |

## Troubleshooting

### Plugin Won't Enable

**Problem:** "Plugin failed to load" error

**Solutions:**
- Ensure you're using Unreal Engine 5.5 or later
- Try rebuilding the plugin: Right-click .uproject > Generate Visual Studio project files
- Check the Output Log for specific errors

### Chat Panel Won't Open

**Problem:** "Window > Olive AI Studio" menu missing

**Solutions:**
- Verify plugin is enabled in Edit > Plugins
- Restart the editor completely
- Check for compilation errors in the Output Log

### API Requests Failing

**Problem:** "Failed to send message" error

**Solutions:**
1. Verify your API key is correct in Project Settings
2. Check your internet connection
3. Ensure the provider (e.g., OpenRouter) is accessible
4. Check Output Log for detailed error messages

### Claude Code Can't Connect

**Problem:** MCP tools not available in Claude Code

**Solutions:**
1. Install Node.js from https://nodejs.org/
2. Ensure Unreal Editor is running with plugin enabled
3. Check that port 3000 isn't blocked by firewall
4. Look for "MCP Server started" message in Output Log

## Getting API Keys

### OpenRouter (Recommended)

1. Visit https://openrouter.ai/
2. Sign up for an account
3. Go to Settings > API Keys
4. Create a new API key
5. Copy and paste into Olive AI Studio settings

**Why OpenRouter?** Access to multiple AI models (Claude, GPT-4, etc.) with simple pricing.

### Direct Anthropic

1. Visit https://console.anthropic.com/
2. Sign up for an account
3. Go to API Keys
4. Create a new key
5. In Olive AI Studio, set Provider Type to "Anthropic"

## Support

- **Documentation**: See `/plans` folder for detailed implementation plans
- **Issues**: Report bugs via GitHub Issues
- **Discussions**: Use GitHub Discussions for questions

## Requirements

- **Unreal Engine**: 5.5 or later
- **Platform**: Windows (Mac/Linux support coming)
- **For Claude Code**: Node.js 14+ installed

## License

MIT License - See LICENSE file for details

## Credits

Built with ❤️ for the Unreal Engine community

Powered by:
- Claude AI (Anthropic)
- OpenRouter
- Model Context Protocol (MCP)
