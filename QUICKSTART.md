# Quick Start Guide

Get up and running with Olive AI Studio in 5 minutes!

## Step 1: Install the Plugin

### Copy to Your Project

1. Download or clone this repository
2. Copy the entire `UE_Olive_AI_Studio` folder to your project's `Plugins` folder:

   ```
   YourProject/
   └── Plugins/
       └── UE_Olive_AI_Studio/    <- Put it here
   ```

   If you don't have a `Plugins` folder, create one in your project root.

### Enable in Editor

1. Open your Unreal Engine project
2. A dialog will appear asking to rebuild the plugin - click **Yes**
3. Wait for compilation to complete
4. If no dialog appears:
   - Go to **Edit > Plugins**
   - Search for "Olive AI"
   - Check the box next to **Olive AI Studio**
   - Click **Restart Now**

## Step 2: Get an API Key

You need an API key to use AI features.

### OpenRouter (Easiest)

1. Go to https://openrouter.ai/
2. Click **Sign Up** (or sign in with Google/GitHub)
3. Go to **Settings** > **API Keys**
4. Click **Create Key**
5. Copy the key (starts with `sk-or-v1-...`)

### Alternative: Direct Anthropic

1. Go to https://console.anthropic.com/
2. Sign up for an account
3. Go to **API Keys** section
4. Create a new key
5. Copy the key

## Step 3: Configure the Plugin

1. In Unreal Editor, go to **Edit > Project Settings**
2. Scroll down to **Plugins** section
3. Click **Olive AI Studio**
4. Paste your API key in the **API Key** field
5. Click **Set as Default** (bottom of window)

That's it! Configuration saved.

## Step 4: Start Using It!

### Option A: Built-in Chat (Easiest)

1. Go to **Window > Olive AI Studio** in the menu bar
2. A chat panel will open (you can dock it anywhere)
3. Type a question or command:
   - "Find blueprints that inherit from Character"
   - "Show me what assets reference BP_Player"
   - "What's the class hierarchy for UActorComponent?"
4. Press Enter or click Send

The AI will respond with results and clickable asset links!

### Option B: Claude Code CLI

Use Claude Code directly with your Unreal project:

1. **Install Node.js** (one-time, if not installed)
   - Download from https://nodejs.org/
   - Run installer, accept defaults

2. **Start Your Project**
   - Open Unreal Editor with your project
   - Olive AI Studio will auto-start the MCP server
   - You'll see: `[LogOliveAI] MCP Server started on port 3000`

3. **Connect Claude Code**
   - Open a terminal in your UE project's plugin folder:
     ```
     cd YourProject/Plugins/UE_Olive_AI_Studio
     ```
   - Run Claude Code:
     ```
     claude
     ```
   - Claude Code will auto-discover the MCP server (ports 3000-3009)
   - Type `/mcp` to verify connection - you should see `olive-ai-studio`

4. **Start Using It**
   - "Use project.search to find all player blueprints"
   - "Show me the dependencies of BP_GameMode"
   - "What inherits from APawn?"

The bridge auto-discovers the MCP server - no port configuration needed!

## What Can You Do?

### Current Features

| Command Example | What It Does |
|----------------|--------------|
| "Find all blueprints with 'player' in the name" | Fuzzy asset search |
| "Show me info about BP_PlayerCharacter" | Detailed asset metadata |
| "What inherits from UActorComponent?" | Class hierarchy |
| "What does BP_GameMode depend on?" | Asset dependencies |
| "What references MyMaterial?" | Find all usages |

### Coming Soon

- Create and edit Blueprints with natural language
- Modify Behavior Trees
- Edit PCG graphs
- C++ code integration

## Troubleshooting

### "Plugin failed to load"

- Make sure you're using **Unreal Engine 5.5 or later**
- Right-click your `.uproject` file > **Generate Visual Studio project files**
- Try rebuilding: **Edit > Plugins > Olive AI Studio > Rebuild**

### "Window > Olive AI Studio" menu is missing

- Verify plugin is enabled: **Edit > Plugins**
- Restart Unreal Editor completely
- Check Output Log for errors (Window > Developer Tools > Output Log)

### AI isn't responding

- Check your API key is correct in Project Settings
- Verify internet connection
- Look in Output Log for error messages
- Try a simple query first: "Hello"

### Claude Code can't find tools

1. Install Node.js if you haven't already
2. Make sure Unreal Editor is running
3. Check Output Log for "MCP Server started" message
4. Verify port 3000 isn't blocked by firewall

## Next Steps

- Read the full **README.md** for detailed features
- Check **Project Settings > Plugins > Olive AI Studio** for customization
- Join discussions on GitHub for tips and tricks

## Need Help?

- Check the Output Log: **Window > Developer Tools > Output Log**
- Search for `LogOliveAI` messages for detailed diagnostics
- Report issues on GitHub

---

**Enjoy building with AI assistance!** 🚀
