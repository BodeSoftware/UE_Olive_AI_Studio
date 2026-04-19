// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Brain/OliveSelfCorrectionPolicy.h"
#include "MCP/OliveToolRegistry.h"
#include "IR/OliveIRTypes.h"

static FOliveToolResult MakeErr(const FString& Code)
{
	FOliveToolResult R;
	R.bSuccess = false;
	FOliveIRMessage M;
	M.Severity = EOliveIRSeverity::Error;
	M.Code = Code;
	M.Message = TEXT("test");
	R.Messages.Add(M);
	return R;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveSelfCorrectionRetryOnceTest,
	"OliveAI.Brain.SelfCorrection.RetryOnceOnTransient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveSelfCorrectionRetryOnceTest::RunTest(const FString& Parameters)
{
	FOliveSelfCorrectionPolicy Policy;
	FOliveToolResult Transient = MakeErr(TEXT("TIMEOUT"));

	TestTrue(TEXT("First transient error should trigger retry"),
		Policy.ShouldRetry(Transient, 1));
	TestFalse(TEXT("Second transient error should NOT retry"),
		Policy.ShouldRetry(Transient, 2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveSelfCorrectionNoRetryOnUserErrorTest,
	"OliveAI.Brain.SelfCorrection.NoRetryOnUserError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveSelfCorrectionNoRetryOnUserErrorTest::RunTest(const FString& Parameters)
{
	FOliveSelfCorrectionPolicy Policy;
	FOliveToolResult V = MakeErr(TEXT("VALIDATION_FAILED"));
	TestFalse(TEXT("Validation errors should NOT trigger retry"),
		Policy.ShouldRetry(V, 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveSelfCorrectionRetryOnRateLimitTest,
	"OliveAI.Brain.SelfCorrection.RetryOnRateLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveSelfCorrectionRetryOnRateLimitTest::RunTest(const FString& Parameters)
{
	FOliveSelfCorrectionPolicy Policy;
	FOliveToolResult R = MakeErr(TEXT("RATE_LIMIT"));
	TestTrue(TEXT("Rate limits should trigger one retry"),
		Policy.ShouldRetry(R, 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveSelfCorrectionRetryOnHttp5xxTest,
	"OliveAI.Brain.SelfCorrection.RetryOnHttp5xx",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOliveSelfCorrectionRetryOnHttp5xxTest::RunTest(const FString& Parameters)
{
	FOliveSelfCorrectionPolicy Policy;
	FOliveToolResult R = MakeErr(TEXT("HTTP_503"));
	TestTrue(TEXT("HTTP 5xx should trigger one retry"),
		Policy.ShouldRetry(R, 1));
	return true;
}
