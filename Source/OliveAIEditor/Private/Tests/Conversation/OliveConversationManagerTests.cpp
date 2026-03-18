// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Chat/OliveConversationManager.h"
#include "MCP/OliveToolRegistry.h"
#include "Settings/OliveAISettings.h"
// Focus profile system removed -- replaced by chat modes

namespace OliveConversationManagerTests
{
	static constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	class FFakeProvider : public IOliveAIProvider
	{
	public:
		// Identity/config
		virtual FString GetProviderName() const override { return TEXT("Fake"); }
		virtual TArray<FString> GetAvailableModels() const override { return {}; }
		virtual FString GetRecommendedModel() const override { return TEXT("fake"); }
		virtual void Configure(const FOliveProviderConfig& InConfig) override { Config = InConfig; }
		virtual bool ValidateConfig(FString& OutError) const override { OutError.Reset(); return true; }
		virtual const FOliveProviderConfig& GetConfig() const override { return Config; }

		// Requests
		virtual void SendMessage(
			const TArray<FOliveChatMessage>& Messages,
			const TArray<FOliveToolDefinition>& Tools,
			FOnOliveStreamChunk OnChunk,
			FOnOliveToolCall OnToolCall,
			FOnOliveComplete OnComplete,
			FOnOliveError OnError,
			const FOliveRequestOptions& Options = FOliveRequestOptions()) override
		{
			MessagesSeen = Messages;
			ToolsSeen = Tools;
			OptionsSeen = Options;
			SendCount++;

			if (Script.IsBound())
			{
				Script.Execute(MoveTemp(OnChunk), MoveTemp(OnToolCall), MoveTemp(OnComplete), MoveTemp(OnError));
				return;
			}

			// Default: complete with empty assistant response.
			FOliveProviderUsage Usage;
			Usage.TotalTokens = 1;
			OnComplete.ExecuteIfBound(TEXT(""), Usage);
		}

		virtual void CancelRequest() override {}
		virtual void SendMessageAutonomous(
			const FString& UserMessage,
			const FString& ContinuationContext,
			FOnOliveStreamChunk OnChunk,
			FOnOliveComplete OnComplete,
			FOnOliveError OnError) override
		{
			AutonomousUserMessageSeen = UserMessage;
			AutonomousContinuationContextSeen = ContinuationContext;
			AutonomousSendCount++;

			if (AutonomousScript.IsBound())
			{
				AutonomousScript.Execute(MoveTemp(OnChunk), MoveTemp(OnComplete), MoveTemp(OnError));
				return;
			}

			FOliveProviderUsage Usage;
			Usage.TotalTokens = 1;
			OnComplete.ExecuteIfBound(TEXT("autonomous complete"), Usage);
		}
		virtual bool SupportsAutonomousMode() const override { return bSupportsAutonomousMode; }
		virtual bool IsBusy() const override { return false; }
		virtual FString GetLastError() const override { return TEXT(""); }

		// Test surface
		DECLARE_DELEGATE_FourParams(
			FScript,
			FOnOliveStreamChunk /*OnChunk*/,
			FOnOliveToolCall /*OnToolCall*/,
			FOnOliveComplete /*OnComplete*/,
			FOnOliveError /*OnError*/);
		DECLARE_DELEGATE_ThreeParams(
			FAutonomousScript,
			FOnOliveStreamChunk /*OnChunk*/,
			FOnOliveComplete /*OnComplete*/,
			FOnOliveError /*OnError*/);

		FScript Script;
		FAutonomousScript AutonomousScript;
		bool bSupportsAutonomousMode = false;

		int32 SendCount = 0;
		int32 AutonomousSendCount = 0;
		TArray<FOliveChatMessage> MessagesSeen;
		TArray<FOliveToolDefinition> ToolsSeen;
		FOliveRequestOptions OptionsSeen;
		FString AutonomousUserMessageSeen;
		FString AutonomousContinuationContextSeen;

	private:
		FOliveProviderConfig Config;
	};

	static TSharedPtr<FJsonObject> MakeArgs(const TMap<FString, FString>& KVs)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		for (const auto& Pair : KVs)
		{
			Obj->SetStringField(Pair.Key, Pair.Value);
		}
		return Obj;
	}

	static FOliveChatMessage MakeAssistantMessage(const FString& Content)
	{
		FOliveChatMessage Message;
		Message.Role = EOliveChatRole::Assistant;
		Message.Content = Content;
		Message.Timestamp = FDateTime::UtcNow();
		return Message;
	}

	class FScopedAutonomousModeSetting
	{
	public:
		FScopedAutonomousModeSetting(const bool bNewValue)
		{
			Settings = GetMutableDefault<UOliveAISettings>();
			if (Settings)
			{
				bOldValue = Settings->bUseAutonomousMCPMode;
				Settings->bUseAutonomousMCPMode = bNewValue;
			}
		}

		~FScopedAutonomousModeSetting()
		{
			if (Settings)
			{
				Settings->bUseAutonomousMCPMode = bOldValue;
			}
		}

	private:
		UOliveAISettings* Settings = nullptr;
		bool bOldValue = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveConversationConfirmationReplayTest,
	"OliveAI.Conversation.ConfirmationTokenReplay",
	OliveConversationManagerTests::TestFlags)

bool FOliveConversationConfirmationReplayTest::RunTest(const FString& Parameters)
{
	// Confirmation flow removed -- replaced by mode gate in write pipeline.
	// This test now verifies basic chat mode switching instead.

	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();

	// Default mode should be Code (from settings default)
	TestEqual(TEXT("Default mode should be Code"), Manager->GetChatMode(), EOliveChatMode::Code);

	// Switch to Plan
	Manager->SetChatMode(EOliveChatMode::Plan);
	TestEqual(TEXT("Mode should be Plan after SetChatMode"), Manager->GetChatMode(), EOliveChatMode::Plan);

	// Switch to Ask
	Manager->SetChatMode(EOliveChatMode::Ask);
	TestEqual(TEXT("Mode should be Ask after SetChatMode"), Manager->GetChatMode(), EOliveChatMode::Ask);

	// Switch back to Code
	Manager->SetChatMode(EOliveChatMode::Code);
	TestEqual(TEXT("Mode should be Code after switching back"), Manager->GetChatMode(), EOliveChatMode::Code);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveConversationToolPackPolicyTest,
	"OliveAI.Conversation.ToolPackPolicy",
	OliveConversationManagerTests::TestFlags)

bool FOliveConversationToolPackPolicyTest::RunTest(const FString& Parameters)
{
	using namespace OliveConversationManagerTests;

	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();
	TSharedPtr<FFakeProvider> Provider = MakeShared<FFakeProvider>();
	Manager->SetProvider(Provider);

	Provider->Script.BindLambda([&](FOnOliveStreamChunk, FOnOliveToolCall, FOnOliveComplete OnComplete, FOnOliveError)
	{
		FOliveProviderUsage Usage;
		Usage.TotalTokens = 1;
		OnComplete.ExecuteIfBound(TEXT(""), Usage);
	});

	// After AI Freedom update: tool pack filtering is removed. Both read-only and
	// write-intent messages should receive the SAME set of profile-filtered tools.
	// All tools are sent on every iteration so the AI can discover and plan freely.

	Manager->StartNewSession();
	Manager->SendUserMessage(TEXT("read the blueprint structure"));
	const int32 ReadToolCount = Provider->ToolsSeen.Num();
	TestTrue(TEXT("Should send some tools for read-only message"), ReadToolCount > 0);

	Manager->StartNewSession();
	Manager->SendUserMessage(TEXT("add a variable to the blueprint"));
	const int32 WriteToolCount = Provider->ToolsSeen.Num();
	TestTrue(TEXT("Should send some tools for write-intent message"), WriteToolCount > 0);

	// With tool pack filtering removed, both intents should get identical tool counts
	TestEqual(TEXT("Read and write intent should get identical tool counts (no pack filtering)"),
		ReadToolCount, WriteToolCount);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveConversationPlanSessionStartsAndUpdatesTest,
	"OliveAI.Conversation.PlanSessionStartsAndUpdates",
	OliveConversationManagerTests::TestFlags)

bool FOliveConversationPlanSessionStartsAndUpdatesTest::RunTest(const FString& Parameters)
{
	using namespace OliveConversationManagerTests;

	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();
	Manager->SetChatMode(EOliveChatMode::Plan);

	Manager->UpdatePlanSessionFromUserMessage(TEXT("Plan a patrol enemy with waypoint movement."));
	const FString BeforeAssistant = Manager->BuildPlanContinuationContext();
	TestTrue(TEXT("User turn should start a plan session"), Manager->BuildPlanExecutionContext().Contains(TEXT("patrol enemy")));
	TestTrue(TEXT("Continuation context should stay empty until an assistant plan exists"), BeforeAssistant.IsEmpty());

	Manager->UpdatePlanSessionFromAssistantMessage(MakeAssistantMessage(
		TEXT("1. Create a patrol enemy Blueprint. 2. Add waypoint references. 3. Build patrol logic and perception hooks.")));
	const FString AfterAssistant = Manager->BuildPlanContinuationContext();

	TestTrue(TEXT("Assistant turn should update active plan summary"), AfterAssistant.Contains(TEXT("Create a patrol enemy Blueprint")));
	TestTrue(TEXT("Execution context should reflect active plan"), Manager->BuildPlanExecutionContext().Contains(TEXT("patrol enemy")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveConversationPlanSessionContinuationCueTest,
	"OliveAI.Conversation.PlanSessionContinuationCues",
	OliveConversationManagerTests::TestFlags)

bool FOliveConversationPlanSessionContinuationCueTest::RunTest(const FString& Parameters)
{
	using namespace OliveConversationManagerTests;

	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();
	Manager->SetChatMode(EOliveChatMode::Plan);
	Manager->UpdatePlanSessionFromUserMessage(TEXT("Plan a patrol enemy with waypoint movement."));
	Manager->UpdatePlanSessionFromAssistantMessage(MakeAssistantMessage(TEXT("Create the enemy, add waypoint refs, then wire patrol movement.")));

	TestTrue(TEXT("'it' should continue the active plan"), Manager->ShouldContinueActivePlan(TEXT("Give it a hearing sense too.")));
	TestTrue(TEXT("'also' should continue the active plan"), Manager->ShouldContinueActivePlan(TEXT("Also add a return-to-patrol recovery step.")));

	Manager->UpdatePlanSessionFromUserMessage(TEXT("Also add a return-to-patrol recovery step."));
	const FString Continuation = Manager->BuildPlanContinuationContext();
	TestTrue(TEXT("Continuation context should retain the adjustment"), Continuation.Contains(TEXT("return-to-patrol")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveConversationPlanSessionExplicitResetTest,
	"OliveAI.Conversation.PlanSessionExplicitReset",
	OliveConversationManagerTests::TestFlags)

bool FOliveConversationPlanSessionExplicitResetTest::RunTest(const FString& Parameters)
{
	using namespace OliveConversationManagerTests;

	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();
	Manager->SetChatMode(EOliveChatMode::Plan);
	Manager->UpdatePlanSessionFromUserMessage(TEXT("Plan a patrol enemy with waypoint movement."));
	Manager->UpdatePlanSessionFromAssistantMessage(MakeAssistantMessage(TEXT("Create the enemy and wire patrol movement.")));

	TestFalse(TEXT("Explicit new-task wording should not continue the old plan"),
		Manager->ShouldContinueActivePlan(TEXT("New task: plan an interactable dialogue terminal instead.")));
	TestFalse(TEXT("Short unrelated task should not continue the active plan"),
		Manager->ShouldContinueActivePlan(TEXT("Create a door blueprint.")));
	TestTrue(TEXT("Plan-to-code handoff cue should continue the active plan"),
		Manager->ShouldContinueActivePlan(TEXT("Switch to code mode and do it.")));

	Manager->UpdatePlanSessionFromUserMessage(TEXT("New task: plan an interactable dialogue terminal instead."));
	const FString Continuation = Manager->BuildPlanContinuationContext();
	TestTrue(TEXT("New task should replace the plan goal"), Continuation.Contains(TEXT("dialogue terminal")));
	TestFalse(TEXT("New task should clear old goal context"), Continuation.Contains(TEXT("patrol enemy")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveConversationPlanToCodeAutonomousHandoffTest,
	"OliveAI.Conversation.PlanToCodeAutonomousHandoff",
	OliveConversationManagerTests::TestFlags)

bool FOliveConversationPlanToCodeAutonomousHandoffTest::RunTest(const FString& Parameters)
{
	using namespace OliveConversationManagerTests;

	FScopedAutonomousModeSetting AutonomousMode(true);
	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();
	TSharedPtr<FFakeProvider> Provider = MakeShared<FFakeProvider>();
	Provider->bSupportsAutonomousMode = true;
	Manager->SetProvider(Provider);

	Manager->SetChatMode(EOliveChatMode::Plan);
	Manager->UpdatePlanSessionFromUserMessage(TEXT("Plan a patrol enemy with waypoint movement."));
	Manager->UpdatePlanSessionFromAssistantMessage(MakeAssistantMessage(
		TEXT("Create the enemy Blueprint, add waypoint refs, then wire patrol movement and perception.")));

	Manager->SetChatMode(EOliveChatMode::Code);
	Manager->SendUserMessage(TEXT("Implement the approved plan."));

	TestEqual(TEXT("Code handoff should use autonomous send once"), Provider->AutonomousSendCount, 1);
	TestTrue(TEXT("Autonomous continuation context should include execution framing"),
		Provider->AutonomousContinuationContextSeen.Contains(TEXT("Approved Plan To Execute")));
	TestTrue(TEXT("Autonomous continuation context should include the planned goal"),
		Provider->AutonomousContinuationContextSeen.Contains(TEXT("patrol enemy")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveConversationAskModeDoesNotCreatePlanSessionTest,
	"OliveAI.Conversation.AskModeDoesNotCreatePlanSession",
	OliveConversationManagerTests::TestFlags)

bool FOliveConversationAskModeDoesNotCreatePlanSessionTest::RunTest(const FString& Parameters)
{
	using namespace OliveConversationManagerTests;

	FScopedAutonomousModeSetting AutonomousMode(true);
	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();
	TSharedPtr<FFakeProvider> Provider = MakeShared<FFakeProvider>();
	Provider->bSupportsAutonomousMode = true;
	Manager->SetProvider(Provider);
	Manager->SetChatMode(EOliveChatMode::Ask);

	bool bSawError = false;
	FString ErrorText;
	Manager->OnError.AddLambda([&](const FString& InError)
	{
		bSawError = true;
		ErrorText = InError;
	});

	Manager->SendUserMessage(TEXT("What components does the patrol enemy need?"));

	TestTrue(TEXT("Ask mode should block autonomous execution"), bSawError);
	TestTrue(TEXT("Ask mode error should explain read-only restriction"), ErrorText.Contains(TEXT("read-only")));
	TestEqual(TEXT("Ask mode should not launch autonomous provider"), Provider->AutonomousSendCount, 0);
	TestTrue(TEXT("Ask mode should not create a plan continuation context"), Manager->BuildPlanContinuationContext().IsEmpty());
	TestTrue(TEXT("Ask mode should not create a plan execution context"), Manager->BuildPlanExecutionContext().IsEmpty());
	return true;
}
