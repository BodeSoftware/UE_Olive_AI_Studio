// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Chat/OliveConversationManager.h"
#include "MCP/OliveToolRegistry.h"
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
		virtual bool IsBusy() const override { return false; }
		virtual FString GetLastError() const override { return TEXT(""); }

		// Test surface
		DECLARE_DELEGATE_FourParams(
			FScript,
			FOnOliveStreamChunk /*OnChunk*/,
			FOnOliveToolCall /*OnToolCall*/,
			FOnOliveComplete /*OnComplete*/,
			FOnOliveError /*OnError*/);

		FScript Script;

		int32 SendCount = 0;
		TArray<FOliveChatMessage> MessagesSeen;
		TArray<FOliveToolDefinition> ToolsSeen;
		FOliveRequestOptions OptionsSeen;

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
