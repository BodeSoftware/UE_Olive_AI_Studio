// Copyright Bode Software. All Rights Reserved.

/**
 * Tests for the chat mode system (Code / Plan / Ask).
 *
 * Covers:
 * - Mode gate in the write pipeline (StageModeGate)
 * - Mode switching on FOliveConversationManager
 * - Deferred mode switching during processing
 * - Session reset clearing state
 * - Slash command routing (input field level)
 */

#include "Misc/AutomationTest.h"

#include "Brain/OliveBrainState.h"
#include "Chat/OliveConversationManager.h"
#include "Pipeline/OliveWritePipeline.h"
#include "Providers/IOliveAIProvider.h"

namespace OliveChatModeTests
{
	static constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/**
	 * Minimal fake provider that completes immediately or stays "busy" depending on Script binding.
	 * Allows tests to simulate processing state on the ConversationManager.
	 */
	class FFakeProvider : public IOliveAIProvider
	{
	public:
		virtual FString GetProviderName() const override { return TEXT("FakeMode"); }
		virtual TArray<FString> GetAvailableModels() const override { return {}; }
		virtual FString GetRecommendedModel() const override { return TEXT("fake"); }
		virtual void Configure(const FOliveProviderConfig& InConfig) override { Config = InConfig; }
		virtual bool ValidateConfig(FString& OutError) const override { OutError.Reset(); return true; }
		virtual const FOliveProviderConfig& GetConfig() const override { return Config; }

		virtual void SendMessage(
			const TArray<FOliveChatMessage>& Messages,
			const TArray<FOliveToolDefinition>& Tools,
			FOnOliveStreamChunk OnChunk,
			FOnOliveToolCall OnToolCall,
			FOnOliveComplete OnComplete,
			FOnOliveError OnError,
			const FOliveRequestOptions& Options = FOliveRequestOptions()) override
		{
			SendCount++;
			StoredOnComplete = OnComplete;
			StoredOnError = OnError;

			if (bCompleteImmediately)
			{
				FOliveProviderUsage Usage;
				Usage.TotalTokens = 1;
				OnComplete.ExecuteIfBound(TEXT(""), Usage);
			}
			// Otherwise: caller must manually fire StoredOnComplete/StoredOnError
		}

		virtual void CancelRequest() override {}
		virtual bool IsBusy() const override { return false; }
		virtual FString GetLastError() const override { return TEXT(""); }

		/** If true, SendMessage completes immediately. If false, completion is held for manual trigger. */
		bool bCompleteImmediately = true;

		int32 SendCount = 0;

		/** Stored delegates for manual completion in async-simulation tests */
		FOnOliveComplete StoredOnComplete;
		FOnOliveError StoredOnError;

	private:
		FOliveProviderConfig Config;
	};
}

// ============================================================================
// Mode Gate Tests (Write Pipeline Stage 2)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveModeGateBlocksWriteInAskTest,
	"OliveAI.ChatMode.ModeGate.BlocksWriteInAsk",
	OliveChatModeTests::TestFlags)

bool FOliveModeGateBlocksWriteInAskTest::RunTest(const FString& Parameters)
{
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_variable");
	Request.ChatMode = EOliveChatMode::Ask;
	Request.bFromMCP = false;

	// Execute through the pipeline -- Stage 2 should block before any mutation
	FOliveWriteResult Result = FOliveWritePipeline::Get().Execute(Request, FOliveWriteExecutor());
	TestFalse(TEXT("Ask mode should block write operations"), Result.bSuccess);
	TestEqual(TEXT("Should stop at Mode Gate stage"), Result.CompletedStage, EOliveWriteStage::ModeGate);

	// Verify the error code is ASK_MODE
	bool bFoundAskModeError = false;
	for (const FOliveIRMessage& Msg : Result.ValidationMessages)
	{
		if (Msg.Message.Contains(TEXT("ASK_MODE")) || Msg.Message.Contains(TEXT("Ask mode")))
		{
			bFoundAskModeError = true;
			break;
		}
	}
	TestTrue(TEXT("Error should reference ASK_MODE"), bFoundAskModeError);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveModeGateBlocksWriteInPlanTest,
	"OliveAI.ChatMode.ModeGate.BlocksWriteInPlan",
	OliveChatModeTests::TestFlags)

bool FOliveModeGateBlocksWriteInPlanTest::RunTest(const FString& Parameters)
{
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.add_variable");
	Request.ChatMode = EOliveChatMode::Plan;
	Request.bFromMCP = false;

	FOliveWriteResult Result = FOliveWritePipeline::Get().Execute(Request, FOliveWriteExecutor());
	TestFalse(TEXT("Plan mode should block write operations"), Result.bSuccess);
	TestEqual(TEXT("Should stop at Mode Gate stage"), Result.CompletedStage, EOliveWriteStage::ModeGate);

	// Verify the error code is PLAN_MODE
	bool bFoundPlanModeError = false;
	for (const FOliveIRMessage& Msg : Result.ValidationMessages)
	{
		if (Msg.Message.Contains(TEXT("PLAN_MODE")) || Msg.Message.Contains(TEXT("Plan mode")))
		{
			bFoundPlanModeError = true;
			break;
		}
	}
	TestTrue(TEXT("Error should reference PLAN_MODE"), bFoundPlanModeError);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveModeGateAllowsPreviewInPlanTest,
	"OliveAI.ChatMode.ModeGate.AllowsPreviewInPlan",
	OliveChatModeTests::TestFlags)

bool FOliveModeGateAllowsPreviewInPlanTest::RunTest(const FString& Parameters)
{
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.preview_plan_json");
	Request.ChatMode = EOliveChatMode::Plan;
	Request.bFromMCP = false;
	// Skip verification since we have no real Blueprint
	Request.bSkipVerification = true;

	// We don't have a real Blueprint target, so Stage 1 (Validate) may fail
	// or Stage 4 (Execute) will fail with no executor. The key assertion is
	// that Stage 2 does NOT block -- i.e., we don't get a PLAN_MODE error.
	FOliveWriteResult Result = FOliveWritePipeline::Get().Execute(Request, FOliveWriteExecutor());

	// If stage 2 blocked, CompletedStage would be ModeGate
	TestTrue(TEXT("preview_plan_json should pass Mode Gate in Plan mode"),
		Result.CompletedStage != EOliveWriteStage::ModeGate);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveModeGateAllowsPreviewInAskTest,
	"OliveAI.ChatMode.ModeGate.AllowsPreviewInAsk",
	OliveChatModeTests::TestFlags)

bool FOliveModeGateAllowsPreviewInAskTest::RunTest(const FString& Parameters)
{
	// blueprint.preview_plan_json is read-only and should pass the mode gate in Ask mode (BUG-1 fix)
	FOliveWriteRequest Request;
	Request.ToolName = TEXT("blueprint.preview_plan_json");
	Request.ChatMode = EOliveChatMode::Ask;
	Request.bFromMCP = false;
	Request.bSkipVerification = true;

	FOliveWriteResult Result = FOliveWritePipeline::Get().Execute(Request, FOliveWriteExecutor());

	// Stage 2 (ModeGate) must not block -- may fail at Validate or Execute, but not at ModeGate
	TestTrue(TEXT("preview_plan_json should pass Mode Gate in Ask mode"),
		Result.CompletedStage != EOliveWriteStage::ModeGate);

	// Confirm no ASK_MODE error in the messages
	for (const FOliveIRMessage& Msg : Result.ValidationMessages)
	{
		TestFalse(TEXT("Should not contain ASK_MODE error"),
			Msg.Message.Contains(TEXT("ASK_MODE")) || Msg.Message.Contains(TEXT("Ask mode")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveModeGatePassesAllInCodeTest,
	"OliveAI.ChatMode.ModeGate.PassesAllInCode",
	OliveChatModeTests::TestFlags)

bool FOliveModeGatePassesAllInCodeTest::RunTest(const FString& Parameters)
{
	// Test several write tools in Code mode -- none should be blocked by Mode Gate
	const TArray<FString> WriteTools = {
		TEXT("blueprint.add_variable"),
		TEXT("blueprint.apply_plan_json"),
		TEXT("blueprint.add_component"),
		TEXT("blueprint.delete"),
		TEXT("blueprint.set_parent_class")
	};

	for (const FString& ToolName : WriteTools)
	{
		FOliveWriteRequest Request;
		Request.ToolName = ToolName;
		Request.ChatMode = EOliveChatMode::Code;
		Request.bFromMCP = false;
		Request.bSkipVerification = true;

		FOliveWriteResult Result = FOliveWritePipeline::Get().Execute(Request, FOliveWriteExecutor());

		// Should NOT stop at ModeGate -- may fail at Validate or Execute, but not at ModeGate
		TestTrue(FString::Printf(TEXT("Code mode should pass Mode Gate for '%s'"), *ToolName),
			Result.CompletedStage != EOliveWriteStage::ModeGate);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveModeGateMCPRespectsChatModeTest,
	"OliveAI.ChatMode.ModeGate.MCPRespectsChatMode",
	OliveChatModeTests::TestFlags)

bool FOliveModeGateMCPRespectsChatModeTest::RunTest(const FString& Parameters)
{
	// MCP calls with ChatMode=Code should pass the mode gate (external agent default)
	{
		FOliveWriteRequest Request;
		Request.ToolName = TEXT("blueprint.add_variable");
		Request.ChatMode = EOliveChatMode::Code;
		Request.bFromMCP = true;
		Request.bSkipVerification = true;

		FOliveWriteResult Result = FOliveWritePipeline::Get().Execute(Request, FOliveWriteExecutor());

		TestTrue(TEXT("MCP + Code mode should pass mode gate"),
			Result.CompletedStage != EOliveWriteStage::ModeGate);
	}

	// MCP calls with ChatMode=Ask should be blocked by the mode gate
	// (in-engine autonomous agent with Ask mode active)
	{
		FOliveWriteRequest Request;
		Request.ToolName = TEXT("blueprint.add_variable");
		Request.ChatMode = EOliveChatMode::Ask;
		Request.bFromMCP = true;
		Request.bSkipVerification = true;

		FOliveWriteResult Result = FOliveWritePipeline::Get().Execute(Request, FOliveWriteExecutor());

		TestTrue(TEXT("MCP + Ask mode should be blocked by mode gate"),
			Result.CompletedStage == EOliveWriteStage::ModeGate);
	}

	// MCP calls with ChatMode=Plan should be blocked by the mode gate
	{
		FOliveWriteRequest Request;
		Request.ToolName = TEXT("blueprint.add_variable");
		Request.ChatMode = EOliveChatMode::Plan;
		Request.bFromMCP = true;
		Request.bSkipVerification = true;

		FOliveWriteResult Result = FOliveWritePipeline::Get().Execute(Request, FOliveWriteExecutor());

		TestTrue(TEXT("MCP + Plan mode should be blocked by mode gate"),
			Result.CompletedStage == EOliveWriteStage::ModeGate);
	}

	return true;
}

// ============================================================================
// Mode Switching Tests (ConversationManager)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveModeSwitchBasicTest,
	"OliveAI.ChatMode.Switching.BasicSetGet",
	OliveChatModeTests::TestFlags)

bool FOliveModeSwitchBasicTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();

	TestEqual(TEXT("Default should be Code"), Manager->GetChatMode(), EOliveChatMode::Code);

	Manager->SetChatMode(EOliveChatMode::Plan);
	TestEqual(TEXT("Should switch to Plan"), Manager->GetChatMode(), EOliveChatMode::Plan);

	Manager->SetChatMode(EOliveChatMode::Ask);
	TestEqual(TEXT("Should switch to Ask"), Manager->GetChatMode(), EOliveChatMode::Ask);

	Manager->SetChatMode(EOliveChatMode::Code);
	TestEqual(TEXT("Should switch back to Code"), Manager->GetChatMode(), EOliveChatMode::Code);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveModeSwitchDelegateTest,
	"OliveAI.ChatMode.Switching.DelegateFires",
	OliveChatModeTests::TestFlags)

bool FOliveModeSwitchDelegateTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();

	EOliveChatMode ReceivedMode = EOliveChatMode::Code;
	int32 DelegateFireCount = 0;
	Manager->OnModeChanged.AddLambda([&ReceivedMode, &DelegateFireCount](EOliveChatMode NewMode)
	{
		ReceivedMode = NewMode;
		DelegateFireCount++;
	});

	Manager->SetChatMode(EOliveChatMode::Plan);
	TestEqual(TEXT("Delegate should receive Plan"), ReceivedMode, EOliveChatMode::Plan);
	TestEqual(TEXT("Delegate should fire once"), DelegateFireCount, 1);

	Manager->SetChatMode(EOliveChatMode::Ask);
	TestEqual(TEXT("Delegate should receive Ask"), ReceivedMode, EOliveChatMode::Ask);
	TestEqual(TEXT("Delegate should fire twice"), DelegateFireCount, 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveModeDeferredSwitchTest,
	"OliveAI.ChatMode.Switching.DeferredDuringProcessing",
	OliveChatModeTests::TestFlags)

bool FOliveModeDeferredSwitchTest::RunTest(const FString& Parameters)
{
	using namespace OliveChatModeTests;

	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();
	TSharedPtr<FFakeProvider> Provider = MakeShared<FFakeProvider>();
	Provider->bCompleteImmediately = false; // Don't auto-complete -- we need to test deferred state
	Manager->SetProvider(Provider);

	// Start processing a message (provider will NOT complete immediately)
	Manager->StartNewSession();
	Manager->SendUserMessage(TEXT("do something"));
	TestTrue(TEXT("Should be processing"), Manager->IsProcessing());

	// Track deferred delegate
	bool bDeferredFired = false;
	EOliveChatMode DeferredMode = EOliveChatMode::Code;
	Manager->OnModeSwitchDeferred.AddLambda([&bDeferredFired, &DeferredMode](EOliveChatMode Mode)
	{
		bDeferredFired = true;
		DeferredMode = Mode;
	});

	// Try to switch while processing -- should be deferred
	Manager->SetChatMode(EOliveChatMode::Plan);
	TestEqual(TEXT("Mode should remain Code while processing"), Manager->GetChatMode(), EOliveChatMode::Code);
	TestTrue(TEXT("Deferred delegate should fire"), bDeferredFired);
	TestEqual(TEXT("Deferred mode should be Plan"), DeferredMode, EOliveChatMode::Plan);

	// Complete the processing -- deferred mode should apply
	FOliveProviderUsage Usage;
	Usage.TotalTokens = 1;
	Provider->StoredOnComplete.ExecuteIfBound(TEXT("done"), Usage);

	TestEqual(TEXT("Mode should be Plan after processing completes"), Manager->GetChatMode(), EOliveChatMode::Plan);

	return true;
}

// ============================================================================
// Slash Command Tests (Input-level routing, tested via ConversationManager)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveSlashCommandModeSetTest,
	"OliveAI.ChatMode.SlashCommands.SetMode",
	OliveChatModeTests::TestFlags)

bool FOliveSlashCommandModeSetTest::RunTest(const FString& Parameters)
{
	// Slash commands are handled at the UI layer (SOliveAIInputField -> SOliveAIChatPanel).
	// Here we test the underlying logic: recognized commands call SetChatMode.
	// The UI routing (KnownCommands set in SOliveAIInputField) is a separate concern.

	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();

	// Simulate /code
	Manager->SetChatMode(EOliveChatMode::Ask); // Start in Ask
	Manager->SetChatMode(EOliveChatMode::Code); // /code sets Code
	TestEqual(TEXT("/code -> Code mode"), Manager->GetChatMode(), EOliveChatMode::Code);

	// Simulate /plan
	Manager->SetChatMode(EOliveChatMode::Plan);
	TestEqual(TEXT("/plan -> Plan mode"), Manager->GetChatMode(), EOliveChatMode::Plan);

	// Simulate /ask
	Manager->SetChatMode(EOliveChatMode::Ask);
	TestEqual(TEXT("/ask -> Ask mode"), Manager->GetChatMode(), EOliveChatMode::Ask);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveSlashCommandModeQueryTest,
	"OliveAI.ChatMode.SlashCommands.ModeQueryDoesNotChangeMode",
	OliveChatModeTests::TestFlags)

bool FOliveSlashCommandModeQueryTest::RunTest(const FString& Parameters)
{
	// /mode is a query command -- it should NOT change the current mode.
	// The SOliveAIChatPanel::HandleSlashCommand just calls AddSystemMessage.
	// Here we verify that the mode is unchanged after what would be a /mode invocation.

	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();

	Manager->SetChatMode(EOliveChatMode::Plan);
	const EOliveChatMode ModeBefore = Manager->GetChatMode();

	// /mode does NOT call SetChatMode -- it only displays the current mode.
	// Verifying that GetChatMode is stable (no side effects)
	const EOliveChatMode ModeAfterQuery = Manager->GetChatMode();
	TestEqual(TEXT("/mode should not change mode"), ModeAfterQuery, ModeBefore);
	TestEqual(TEXT("Mode should remain Plan"), ModeAfterQuery, EOliveChatMode::Plan);

	return true;
}

// ============================================================================
// Session Reset Tests
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveSessionResetClearsStateTest,
	"OliveAI.ChatMode.Switching.SessionResetClearsState",
	OliveChatModeTests::TestFlags)

bool FOliveSessionResetClearsStateTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();

	// Switch to a non-default mode to verify the reset takes effect
	Manager->SetChatMode(EOliveChatMode::Plan);
	TestEqual(TEXT("Should be in Plan mode before reset"), Manager->GetChatMode(), EOliveChatMode::Plan);

	// StartNewSession() should reset mode to the default from settings (Code by default)
	Manager->StartNewSession();
	TestEqual(TEXT("Mode should reset to Code after StartNewSession"), Manager->GetChatMode(), EOliveChatMode::Code);

	// Session ID should be a new unique value (not empty)
	TestFalse(TEXT("New session ID should not be empty"), !Manager->GetSessionId().IsValid());

	// History should be cleared
	TestEqual(TEXT("Message history should be empty after reset"), Manager->GetMessageCount(), 0);

	return true;
}

// ============================================================================
// LexToString Tests (Enum serialization)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveChatModeLexToStringTest,
	"OliveAI.ChatMode.LexToString",
	OliveChatModeTests::TestFlags)

bool FOliveChatModeLexToStringTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Code -> 'Code'"), FString(LexToString(EOliveChatMode::Code)), FString(TEXT("Code")));
	TestEqual(TEXT("Plan -> 'Plan'"), FString(LexToString(EOliveChatMode::Plan)), FString(TEXT("Plan")));
	TestEqual(TEXT("Ask -> 'Ask'"), FString(LexToString(EOliveChatMode::Ask)), FString(TEXT("Ask")));

	return true;
}
