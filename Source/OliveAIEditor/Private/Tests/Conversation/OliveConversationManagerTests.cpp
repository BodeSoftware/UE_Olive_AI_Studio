// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Chat/OliveConversationManager.h"
#include "MCP/OliveToolRegistry.h"
#include "Profiles/OliveFocusProfileManager.h"
#include "Brain/OliveToolPackManager.h"

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
	using namespace OliveConversationManagerTests;

	// Ensure profiles/tools/packs initialized.
	FOliveFocusProfileManager::Get().Initialize();
	FOliveToolPackManager::Get().Initialize();

	// Register a temporary tool that requires confirmation unless confirmation_token is present.
	static const FString ToolName = TEXT("test.requires_confirm");
	FOliveToolRegistry& Registry = FOliveToolRegistry::Get();

	int32 ToolExecCount = 0;
	FString LastSeenToken;
	Registry.RegisterTool(
		ToolName,
		TEXT("Test tool for confirmation replay"),
		MakeShared<FJsonObject>(),
		FOliveToolHandler::CreateLambda([&ToolExecCount, &LastSeenToken](const TSharedPtr<FJsonObject>& Params) -> FOliveToolResult
		{
			ToolExecCount++;

			FString Token;
			if (Params.IsValid())
			{
				Params->TryGetStringField(TEXT("confirmation_token"), Token);
			}
			LastSeenToken = Token;

			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			if (Token.IsEmpty())
			{
				Data->SetBoolField(TEXT("requires_confirmation"), true);
				Data->SetStringField(TEXT("plan"), TEXT("Test plan"));
				Data->SetStringField(TEXT("confirmation_token"), TEXT("tok_test_1"));
				return FOliveToolResult::Success(Data);
			}

			Data->SetBoolField(TEXT("success"), true);
			Data->SetStringField(TEXT("seen_token"), Token);
			return FOliveToolResult::Success(Data);
		}),
		{ TEXT("test") },
		TEXT("test"));

	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();
	TSharedPtr<FFakeProvider> Provider = MakeShared<FFakeProvider>();
	Manager->SetProvider(Provider);

	bool bConfirmEventFired = false;
	Manager->OnConfirmationRequired.AddLambda([&bConfirmEventFired](const FString&, const FString&, const FString&)
	{
		bConfirmEventFired = true;
	});

	// Provider script: ask model to call our tool only on the first SendMessage.
	Provider->Script.BindLambda([&](FOnOliveStreamChunk OnChunk, FOnOliveToolCall OnToolCall, FOnOliveComplete OnComplete, FOnOliveError OnError)
	{
		if (Provider->SendCount > 1)
		{
			FOliveProviderUsage Usage;
			Usage.TotalTokens = 1;
			OnComplete.ExecuteIfBound(TEXT(""), Usage);
			return;
		}

		FOliveStreamChunk ToolCall;
		ToolCall.bIsToolCall = true;
		ToolCall.ToolCallId = TEXT("call_1");
		ToolCall.ToolName = ToolName;
		ToolCall.ToolArguments = MakeArgs({ {TEXT("x"), TEXT("y")} });
		OnToolCall.ExecuteIfBound(ToolCall);

		FOliveProviderUsage Usage;
		Usage.TotalTokens = 1;
		OnComplete.ExecuteIfBound(TEXT(""), Usage);
	});

	Manager->SendUserMessage(TEXT("add something"));

	TestTrue(TEXT("Tool should have been executed at least once (initial)"), ToolExecCount >= 1);
	TestTrue(TEXT("Confirmation event should fire"), bConfirmEventFired);
	TestTrue(TEXT("Manager should be waiting for confirmation"), Manager->IsWaitingForConfirmation());

	// Confirm should replay with token (not confirmed=true).
	Manager->ConfirmPendingOperation();

	TestTrue(TEXT("Tool should have been executed at least twice (replay)"), ToolExecCount >= 2);
	TestTrue(TEXT("Replay should include confirmation_token"), LastSeenToken == TEXT("tok_test_1"));

	Registry.UnregisterTool(ToolName);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveConversationToolPackPolicyTest,
	"OliveAI.Conversation.ToolPackPolicy",
	OliveConversationManagerTests::TestFlags)

bool FOliveConversationToolPackPolicyTest::RunTest(const FString& Parameters)
{
	using namespace OliveConversationManagerTests;

	FOliveFocusProfileManager::Get().Initialize();
	FOliveToolPackManager::Get().Initialize();

	TSharedPtr<FOliveConversationManager> Manager = MakeShared<FOliveConversationManager>();
	TSharedPtr<FFakeProvider> Provider = MakeShared<FFakeProvider>();
	Manager->SetProvider(Provider);

	Provider->Script.BindLambda([&](FOnOliveStreamChunk, FOnOliveToolCall, FOnOliveComplete OnComplete, FOnOliveError)
	{
		FOliveProviderUsage Usage;
		Usage.TotalTokens = 1;
		OnComplete.ExecuteIfBound(TEXT(""), Usage);
	});

	// Read-only intent: should not include write packs (best-effort, based on heuristic).
	Manager->StartNewSession();
	Manager->SendUserMessage(TEXT("read the blueprint structure"));
	const int32 ReadToolCount = Provider->ToolsSeen.Num();
	TestTrue(TEXT("Should send some tools"), ReadToolCount > 0);

	// Write intent: should generally include more tools than read-only (write packs added).
	Manager->StartNewSession();
	Manager->SendUserMessage(TEXT("add a variable to the blueprint"));
	const int32 WriteToolCount = Provider->ToolsSeen.Num();
	TestTrue(TEXT("Write intent should not reduce tools vs read intent"), WriteToolCount >= ReadToolCount);

	return true;
}
