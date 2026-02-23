// Copyright Bode Software. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IOliveAIProvider.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"

/**
 * Claude Code CLI Provider
 *
 * Implements IOliveAIProvider by spawning the Claude Code CLI process.
 * This allows users to use their Claude Max subscription instead of API keys.
 *
 * Communication flow:
 * 1. Spawn 'claude' process with --print flag (non-interactive JSON mode)
 * 2. Send user messages via stdin as JSON
 * 3. Read streamed responses from stdout
 * 4. Parse JSON responses and emit chunks
 *
 * Requirements:
 * - Claude Code CLI installed and in PATH
 * - User authenticated with Claude Max or API key
 */
class OLIVEAIEDITOR_API FOliveClaudeCodeProvider : public IOliveAIProvider
{
public:
	FOliveClaudeCodeProvider();
	virtual ~FOliveClaudeCodeProvider();

	// ==========================================
	// IOliveAIProvider Interface
	// ==========================================

	virtual FString GetProviderName() const override { return TEXT("Claude Code CLI"); }
	virtual TArray<FString> GetAvailableModels() const override;
	virtual FString GetRecommendedModel() const override;

	virtual void Configure(const FOliveProviderConfig& Config) override;
	virtual bool ValidateConfig(FString& OutError) const override;
	virtual const FOliveProviderConfig& GetConfig() const override { return CurrentConfig; }

	virtual void SendMessage(
		const TArray<FOliveChatMessage>& Messages,
		const TArray<FOliveToolDefinition>& Tools,
		FOnOliveStreamChunk OnChunk,
		FOnOliveToolCall OnToolCall,
		FOnOliveComplete OnComplete,
		FOnOliveError OnError,
		const FOliveRequestOptions& Options = FOliveRequestOptions()
	) override;

	virtual void CancelRequest() override;
	virtual bool IsBusy() const override { return bIsBusy; }
	virtual FString GetLastError() const override { return LastError; }
	virtual void ValidateConnection(TFunction<void(bool bSuccess, const FString& Message)> Callback) const override;

	// ==========================================
	// Claude Code Specific
	// ==========================================

	/**
	 * Check if Claude Code CLI is installed and accessible
	 */
	static bool IsClaudeCodeInstalled();

	/**
	 * Get the path to the claude executable
	 */
	static FString GetClaudeExecutablePath();

	/**
	 * Get Claude Code version
	 */
	static FString GetClaudeCodeVersion();

private:
	/**
	 * Spawn the claude process
	 */
	bool SpawnClaudeProcess();

	/**
	 * Kill any running claude process
	 */
	void KillClaudeProcess();

	/**
	 * Send input to the claude process
	 */
	bool SendToProcess(const FString& Input);

	/**
	 * Read output from the claude process
	 */
	void ReadProcessOutput();

	/**
	 * Parse a line of JSON output from claude
	 */
	void ParseOutputLine(const FString& Line);

	/**
	 * Build the prompt/message to send to claude
	 */
	FString BuildPrompt(const TArray<FOliveChatMessage>& Messages, const TArray<FOliveToolDefinition>& Tools) const;

	/**
	 * Build domain-specific system prompt using the prompt assembler.
	 * Must be called on the game thread (accesses UObject settings).
	 * @param UserTask The user's task description (passed to template as TaskDescription)
	 */
	FString BuildSystemPrompt(const FString& UserTask, const TArray<FOliveToolDefinition>& Tools) const;

	// ==========================================
	// Process Management
	// ==========================================

	/** Process handle */
	FProcHandle ProcessHandle;

	/** Stdin pipe for writing to process */
	void* StdinWritePipe = nullptr;

	/** Stdout pipe for reading from process */
	void* StdoutReadPipe = nullptr;

	/** Reader thread handle */
	FRunnableThread* ReaderThread = nullptr;

	/** Flag to stop reader thread */
	FThreadSafeBool bStopReading;

	// ==========================================
	// State
	// ==========================================

	FOliveProviderConfig CurrentConfig;
	FString LastError;
	FThreadSafeBool bIsBusy;

	/** Accumulated response text */
	FString AccumulatedResponse;

	/** Current callbacks */
	FOnOliveStreamChunk CurrentOnChunk;
	FOnOliveToolCall CurrentOnToolCall;
	FOnOliveComplete CurrentOnComplete;
	FOnOliveError CurrentOnError;

	/** Lock for callback access */
	FCriticalSection CallbackLock;

	/** Working directory for claude (plugin directory) */
	FString WorkingDirectory;
};

/**
 * Reader thread for claude process stdout
 */
class FOliveClaudeReaderRunnable : public FRunnable
{
public:
	FOliveClaudeReaderRunnable(
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
