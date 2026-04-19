// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Chat/OliveConversationManager.h"
#include "MCP/OliveToolRegistry.h"
#include "Settings/OliveAISettings.h"

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

