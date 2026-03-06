// Copyright Bode Software. All Rights Reserved.

/**
 * OliveCLIProviderBase.h
 *
 * Abstract base class for CLI-based AI provider implementations.
 * Encapsulates all universal process management, stdin/stdout pipe communication,
 * prompt formatting, and response parsing that is shared across CLI providers
 * (Claude Code, Codex CLI, Gemini CLI, etc.).
 *
 * Subclasses override a small set of virtual hooks to customize:
 *   - GetExecutablePath()   : which CLI binary to run
 *   - GetCLIArguments()     : provider-specific flags
 *   - ParseOutputLine()     : provider-specific stdout format parsing
 *
 * Communication flow (common to all CLI providers):
 * 1. Build conversation prompt + system prompt on game thread
 * 2. Spawn CLI process on background thread with --print-like non-interactive mode
 * 3. Deliver prompt via stdin pipe, close stdin to signal EOF
 * 4. Read stdout line-by-line, dispatch ParseOutputLine() on game thread
 * 5. On process exit, parse tool calls from accumulated text via FOliveCLIToolCallParser
 * 6. Emit tool calls and completion to ConversationManager callbacks
 */

#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include "IOliveAIProvider.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "Providers/OliveCLIToolCallParser.h"

/**
 * Reader thread for CLI process stdout.
 *
 * Reads from the stdout pipe in a loop, splitting on newlines and
 * dispatching complete lines via the OnLine callback. Used by
 * FOliveCLIProviderBase for asynchronous stdout reading.
 *
 * Note: In the current implementation, SendMessage uses an inline read loop
 * rather than this runnable. The runnable is retained for threaded reading
 * and future providers that may prefer that approach.
 */
class OLIVEAIEDITOR_API FOliveCLIReaderRunnable : public FRunnable
{
public:
	FOliveCLIReaderRunnable(
		void* InReadPipe,
		FThreadSafeBool& InStopFlag,
		TFunction<void(const FString&)> InOnLine
	);

	virtual bool Init() override { return true; }
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	void* ReadPipe;
	FThreadSafeBool& bStop;
	TFunction<void(const FString&)> OnLine;
};

/**
 * Context from the most recent autonomous run, used to enrich "continue" messages.
 *
 * Tracks the original user message, which assets were modified, and what tools
 * were called during the run. When the user sends a continuation message
 * (e.g., "continue", "keep going"), this context is injected into the prompt
 * so the new CLI process knows what was already done.
 */
struct FAutonomousRunContext
{
	/** The original user message that started the run */
	FString OriginalMessage;

	/** Asset paths that were modified during the run (extracted from MCP tool calls) */
	TArray<FString> ModifiedAssetPaths;

	/** Tool call log entry: tool name + asset path (if any). */
	struct FToolCallEntry
	{
		FString ToolName;
		FString AssetPath;
	};

	/** Ordered log of tool calls made during the run. Capped at 50 entries. */
	TArray<FToolCallEntry> ToolCallLog;

	/** Recipe names fetched via olive.get_recipe during the run, for continuation prompt */
	TArray<FString> FetchedRecipeNames;

	/** Template IDs fetched via blueprint.get_template during the run, for continuation prompt */
	TArray<FString> FetchedTemplateIds;

	/** Run outcome */
	enum class EOutcome : uint8
	{
		Completed,    // Process exited normally
		IdleTimeout,  // Killed by idle timeout (no stdout for N seconds)
		RuntimeLimit, // Killed by total runtime limit
		OutputStall   // AI started a text response then froze (no chars + no tool calls for 90s)
	};
	EOutcome Outcome = EOutcome::Completed;

	/** Whether this context is valid (a run has completed) */
	bool bValid = false;

	/** Reset to empty state */
	void Reset()
	{
		OriginalMessage.Empty();
		ModifiedAssetPaths.Empty();
		ToolCallLog.Empty();
		FetchedRecipeNames.Empty();
		FetchedTemplateIds.Empty();
		Outcome = EOutcome::Completed;
		bValid = false;
	}
};

/**
 * Abstract base class for CLI-based AI providers.
 *
 * Provides the complete IOliveAIProvider implementation for providers that
 * communicate by spawning a CLI process, writing to stdin, and reading from stdout.
 * Subclasses only need to implement a handful of virtual hooks to customize
 * executable path, CLI arguments, and output format parsing.
 *
 * Thread safety:
 * - SendMessage() stores callbacks under CallbackLock, then spawns a background task.
 * - The background task reads stdout and dispatches ParseOutputLine() to the game thread.
 * - HandleResponseComplete() runs on the game thread under CallbackLock.
 * - CancelRequest() and KillProcess() can be called from any thread.
 */
class OLIVEAIEDITOR_API FOliveCLIProviderBase : public IOliveAIProvider
{
public:
	virtual ~FOliveCLIProviderBase();

	// ==========================================
	// IOliveAIProvider Interface (implemented in base)
	// ==========================================

	virtual void SendMessage(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools,
		FOnOliveStreamChunk OnChunk,
		FOnOliveToolCall OnToolCall,
		FOnOliveComplete OnComplete,
		FOnOliveError OnError,
		const FOliveRequestOptions& Options = FOliveRequestOptions()
	) override;

	/**
	 * Send a message in autonomous MCP mode. Launches the CLI process with
	 * autonomous arguments (no system prompt injection, no tool schema serialization).
	 * The CLI discovers tools via MCP server and manages its own agentic loop.
	 *
	 * @param UserMessage  The user's task description sent to stdin
	 * @param OnChunk      Called for each streamed progress chunk (text, tool activity, etc.)
	 * @param OnComplete   Called when the autonomous process finishes naturally
	 * @param OnError      Called on process spawn failure, idle timeout, or non-zero exit with no output
	 */
	virtual void SendMessageAutonomous(
		const FString& UserMessage,
		FOnOliveStreamChunk OnChunk,
		FOnOliveComplete OnComplete,
		FOnOliveError OnError
	) override;

	virtual bool SupportsAutonomousMode() const override { return true; }
	virtual void CancelRequest() override;
	virtual bool IsBusy() const override { return bIsBusy; }
	virtual FString GetLastError() const override { return LastError; }
	virtual const FOliveProviderConfig& GetConfig() const override { return CurrentConfig; }
	virtual void Configure(const FOliveProviderConfig& Config) override;

protected:
	// ==========================================
	// Virtual hooks for subclasses
	// ==========================================

	/**
	 * Get the path to the CLI executable.
	 * Called on a background thread during process spawn.
	 * @return Full path to the executable, or empty string if not found
	 */
	virtual FString GetExecutablePath() const = 0;

	/**
	 * Build CLI arguments (flags) for the process.
	 * Called on a background thread during process spawn.
	 * @param SystemPromptArg The already-escaped --append-system-prompt argument fragment,
	 *        or empty string if no system prompt. Subclass should incorporate this into flags.
	 * @return The full argument string for the CLI process
	 */
	virtual FString GetCLIArguments(const FString& SystemPromptArg) const = 0;

	/**
	 * Get CLI arguments for autonomous MCP mode.
	 * In autonomous mode the CLI discovers tools via MCP and manages its own agentic loop,
	 * so there is no system prompt injection, no --max-turns 1, and no --strict-mcp-config.
	 * Override in subclasses for provider-specific autonomous arguments.
	 *
	 * Default implementation: delegates to GetCLIArguments with an empty system prompt arg.
	 * @return The full argument string for autonomous CLI execution
	 */
	virtual FString GetCLIArgumentsAutonomous() const;

	/**
	 * Parse a single line of stdout output.
	 * Called on the game thread under CallbackLock.
	 * Default implementation: no-op. Override for provider-specific format parsing
	 * (e.g., Claude's stream-json format).
	 * @param Line A single trimmed, non-empty line from stdout
	 */
	virtual void ParseOutputLine(const FString& Line);

	/**
	 * Get the working directory for the CLI process.
	 * Default: returns the stored WorkingDirectory member.
	 * @return Absolute path to use as the process working directory
	 */
	virtual FString GetWorkingDirectory() const;

	/**
	 * Whether the executable requires Node.js to run (i.e., ends in .js).
	 * Default: checks if GetExecutablePath() ends with ".js".
	 * @return True if the executable should be launched via "node <path>"
	 */
	virtual bool RequiresNodeRunner() const;

	/**
	 * Get the name of the CLI for error messages and logging.
	 * Default: returns "CLI". Subclasses should return e.g., "Claude".
	 * @return Human-readable CLI name for log messages
	 */
	virtual FString GetCLIName() const;

	// ==========================================
	// Shared infrastructure (protected, usable by subclasses)
	// ==========================================

	/**
	 * Build the conversation prompt from message history.
	 * Formats [User], [Assistant], [Tool Result] blocks and appends
	 * a "## Next Action Required" directive for the CLI's stdin channel.
	 *
	 * @param Messages The conversation history
	 * @param Tools Available tool definitions (currently unused in prompt body)
	 * @return Formatted prompt string for stdin delivery
	 */
	FString BuildConversationPrompt(const TArray<FOliveChatMessage>& Messages, const TArray<FOliveToolDefinition>& Tools) const;

	/**
	 * Build the domain-specific system prompt using the prompt assembler.
	 * Must be called on the game thread (accesses UObject settings via FOlivePromptAssembler).
	 * Includes project context, policies, recipe routing, CLI blueprint knowledge,
	 * tool schemas, and tool call format instructions.
	 *
	 * @param UserTask The user's task description (used for context)
	 * @param Tools Available tool definitions to serialize as schemas
	 * @return Assembled system prompt string
	 */
	FString BuildCLISystemPrompt(const FString& UserTask, const TArray<FOliveToolDefinition>& Tools) const;

	/**
	 * Shared process lifecycle: spawn CLI, pipe stdin, read stdout loop, handle exit.
	 * Used by both SendMessage() (orchestrated) and SendMessageAutonomous() (autonomous).
	 *
	 * Callers must set bIsBusy, store callbacks, increment RequestGeneration, and clear
	 * AccumulatedResponse BEFORE calling this method. LaunchCLIProcess captures the
	 * current RequestGeneration for staleness checks internally.
	 *
	 * Thread model:
	 * - Must be called on the game thread (captures AliveGuard, CLIName, then
	 *   dispatches to a background thread for process I/O).
	 * - OnProcessExit is called on the game thread, inside Guard + Generation + CallbackLock checks.
	 *
	 * @param CLIArgs                  Fully-built CLI argument string (output of GetCLIArguments or GetCLIArgumentsAutonomous)
	 * @param StdinContent             Written to stdin then EOF. If empty, stdin is closed immediately (signal EOF with no input).
	 * @param OnProcessExit            Called on the game thread with the process return code. Invoked inside
	 *                                 Guard/Generation/CallbackLock checks so the callee can safely access members.
	 * @param WorkingDirectoryOverride If non-empty, used as the process working directory instead of GetWorkingDirectory().
	 *                                 Primary use case: autonomous sandbox directory.
	 */
	void LaunchCLIProcess(
		const FString& CLIArgs,
		const FString& StdinContent,
		TFunction<void(int32)> OnProcessExit,
		const FString& WorkingDirectoryOverride = FString()
	);

	/**
	 * Set up the autonomous agent sandbox directory.
	 * Creates {ProjectDir}/Saved/OliveAI/AgentSandbox/ with:
	 * - AGENTS.md with agent role context and knowledge packs (read by all CLIs)
	 * - Provider-specific files via WriteProviderSpecificSandboxFiles()
	 * Stores the path in AutonomousSandboxDir for use as working directory.
	 */
	virtual void SetupAutonomousSandbox();

	/**
	 * Write provider-specific files into the sandbox directory.
	 * Called at the end of SetupAutonomousSandbox().
	 * Default: no-op. Claude overrides to write .mcp.json.
	 *
	 * @param AgentContext The built agent context string (rules + knowledge packs)
	 */
	virtual void WriteProviderSpecificSandboxFiles(const FString& AgentContext);

	/**
	 * Handle process completion for orchestrated mode.
	 * Parses tool calls from accumulated response text via FOliveCLIToolCallParser
	 * and emits them through OnToolCall before signaling completion.
	 * Called on the game thread under CallbackLock.
	 *
	 * @param ReturnCode The process exit code
	 */
	void HandleResponseComplete(int32 ReturnCode);

	/**
	 * Handle process completion for autonomous MCP mode.
	 * Emits accumulated response text via OnComplete without parsing tool calls
	 * (tools were executed via MCP server, not via <tool_call> XML blocks).
	 * Called on the game thread under CallbackLock.
	 *
	 * @param ReturnCode The process exit code
	 */
	void HandleResponseCompleteAutonomous(int32 ReturnCode);

	/**
	 * Check if a user message looks like a continuation request.
	 * Matches common phrases like "continue", "keep going", "finish", "resume".
	 *
	 * @param Message The user's message text
	 * @return True if the message appears to be a continuation of a previous task
	 */
	bool IsContinuationMessage(const FString& Message) const;

	/**
	 * Extract meaningful keywords from a user message for asset search.
	 * Splits on spaces, keeps words 4+ chars, filters out common stop words
	 * and UE jargon that would match too broadly. Caps at 5 keywords.
	 *
	 * @param Message  The user's message text
	 * @return Array of lowercase keyword strings suitable for project index search
	 */
	TArray<FString> ExtractKeywordsFromMessage(const FString& Message) const;

	/**
	 * Build an enriched prompt for a continuation message by injecting context
	 * from the previous autonomous run. Includes the original task, what was done
	 * (grouped by asset with deduped tool names), run outcome, and action directive.
	 *
	 * @param UserMessage The user's continuation message (e.g., "continue")
	 * @return Enriched prompt with previous run context
	 */
	FString BuildContinuationPrompt(const FString& UserMessage) const;

	/**
	 * Build a compact summary of current state for each asset in the given set.
	 * Loads each UBlueprint and formats components, variables, functions (with
	 * node counts), event dispatchers, and compile status. Non-Blueprint assets
	 * get a one-line note.
	 *
	 * MUST be called on the game thread (loads UObject packages).
	 *
	 * @param AssetPaths  Set of asset paths to summarize
	 * @return Formatted summary for injection into prompts
	 */
	FString BuildAssetStateSummary(const TArray<FString>& AssetPaths) const;

	/**
	 * Build a compact summary of current state for each modified asset from the last run.
	 * Convenience overload that reads from LastRunContext.ModifiedAssetPaths.
	 *
	 * MUST be called on the game thread (loads UObject packages).
	 *
	 * @return Formatted summary for injection into continuation prompts
	 */
	FString BuildAssetStateSummary() const { return BuildAssetStateSummary(LastRunContext.ModifiedAssetPaths); }

	/**
	 * Kill the running CLI process and clean up all resources.
	 * Safe to call from any thread. Waits for reader thread completion,
	 * terminates the process, and closes all pipes.
	 */
	void KillProcess();

	/**
	 * Send text input to the running process via stdin pipe.
	 * @param Input Text to write (newline is appended automatically)
	 * @return True if write succeeded
	 */
	bool SendToProcess(const FString& Input);

	// ==========================================
	// Process Management State
	// ==========================================

	/** Process handle for the running CLI */
	FProcHandle ProcessHandle;

	/** Stdin pipe for writing to the process */
	void* StdinWritePipe = nullptr;

	/** Stdout pipe for reading from the process */
	void* StdoutReadPipe = nullptr;

	/** Reader thread handle (for threaded reading mode) */
	FRunnableThread* ReaderThread = nullptr;

	/** Flag to stop the reader thread / inline read loop */
	FThreadSafeBool bStopReading;

	// ==========================================
	// State
	// ==========================================

	/** Current provider configuration */
	FOliveProviderConfig CurrentConfig;

	/** Last error message */
	FString LastError;

	/** Whether a request is currently in progress */
	FThreadSafeBool bIsBusy;

	/** Generation counter to distinguish stale async completions from current request */
	std::atomic<uint32> RequestGeneration{0};

	/** Accumulated response text from stdout */
	FString AccumulatedResponse;

	/** Current stream chunk callback */
	FOnOliveStreamChunk CurrentOnChunk;

	/** Current tool call callback */
	FOnOliveToolCall CurrentOnToolCall;

	/** Current completion callback */
	FOnOliveComplete CurrentOnComplete;

	/** Current error callback */
	FOnOliveError CurrentOnError;

	/** Lock protecting callback access and AccumulatedResponse */
	FCriticalSection CallbackLock;

	/**
	 * Shared alive-guard for safe async access.
	 *
	 * Background threads and queued game-thread AsyncTasks capture a shared copy
	 * of this bool. The destructor sets it to false BEFORE tearing down members.
	 * Lambdas check *AliveGuard before touching `this`, preventing use-after-free
	 * when the provider is destroyed while async work is still pending.
	 *
	 * This is necessary because:
	 * - AsyncTask(GameThread, ...) lambdas can sit in the task queue after `this` dies
	 * - The background read thread may still reference `this` members during cleanup
	 * - FScopeLock(&CallbackLock) at the top of each lambda would crash on a dead object
	 */
	TSharedPtr<FThreadSafeBool> AliveGuard = MakeShared<FThreadSafeBool>(true);

	/** Working directory for the CLI process */
	FString WorkingDirectory;

	/** Sandbox directory for autonomous mode (written before launch, cleaned up on completion) */
	FString AutonomousSandboxDir;

	/** Timestamp of last successful MCP tool call during autonomous mode.
	 *  Updated by MCP server OnToolCalled delegate. Used for activity-based timeout. */
	std::atomic<double> LastToolCallTimestamp{0.0};

	/** Count of scaffolding operations (add_component, add_variable, modify_component,
	 *  create_from_template) in the current run. Written on game thread via OnToolCalled,
	 *  read on background thread for adaptive idle timeout. */
	std::atomic<int32> ScaffoldingOpCount{0};

	/** Count of olive.get_recipe calls in the current run. Multiple recipe lookups
	 *  signal complex multi-function planning ahead — triggers extended idle timeout. */
	std::atomic<int32> RecipeCallCount{0};

	/** Delegate handle for MCP tool call subscription (cleaned up on process exit) */
	FDelegateHandle ToolCallDelegateHandle;

	/** Context from the most recent autonomous run for "continue" enrichment */
	FAutonomousRunContext LastRunContext;

	/** Whether the last process termination was due to timeout (idle or runtime limit).
	 *  Set on the background thread in LaunchCLIProcess, read on game thread in HandleResponseCompleteAutonomous. */
	bool bLastRunTimedOut = false;

	/** Whether the last timeout was specifically a runtime limit (not idle timeout).
	 *  Set on the background thread alongside bLastRunTimedOut. Used to distinguish
	 *  idle stalls (auto-continuable) from runtime limit hits (not auto-continuable). */
	bool bLastRunWasRuntimeLimit = false;

	/** Number of automatic continuations since last user-initiated message.
	 *  Reset to 0 on each non-continuation SendMessageAutonomous call. */
	int32 AutoContinueCount = 0;

	/** Whether the current SendMessageAutonomous invocation was triggered internally
	 *  by the auto-continue mechanism in HandleResponseCompleteAutonomous.
	 *  Set to true before the auto-continue AsyncTask dispatch, consumed at the top
	 *  of SendMessageAutonomous. This ensures AutoContinueCount is preserved across
	 *  auto-continues but reset on any user-initiated input. */
	bool bIsAutoContinuation = false;

	/** Maximum automatic continuations before giving up and reporting to user */
	static constexpr int32 MaxAutoContinues = 1;

	/**
	 * Asset paths from @-mentions in the chat UI, set before the initial autonomous run.
	 * Consumed (emptied) in SendMessageAutonomous after building the asset state summary.
	 * This allows the AI to see the current state of mentioned assets without re-reading them.
	 */
	TArray<FString> InitialContextAssetPaths;

public:
	/**
	 * Set initial context asset paths for the next autonomous run.
	 * Called by ConversationManager before SendMessageAutonomous with the
	 * @-mentioned asset paths from the chat context bar.
	 *
	 * @param AssetPaths  Array of asset paths (e.g., "/Game/Blueprints/BP_Gun")
	 */
	void SetInitialContextAssets(const TArray<FString>& AssetPaths) { InitialContextAssetPaths = AssetPaths; }
};
