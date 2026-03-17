// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Brain/OliveBrainLayer.h"

namespace OliveBrainLayerTests
{
	static constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBrainLayerRunLifecycleTest,
	"OliveAI.Brain.RunLifecycle",
	OliveBrainLayerTests::TestFlags)

bool FOliveBrainLayerRunLifecycleTest::RunTest(const FString& Parameters)
{
	FOliveBrainLayer Brain;

	TestEqual(TEXT("Initial state should be Idle"), Brain.GetState(), EOliveBrainState::Idle);
	TestEqual(TEXT("Initial phase should be Streaming"), Brain.GetWorkerPhase(), EOliveWorkerPhase::Streaming);

	const FString RunId = Brain.BeginRun();
	TestTrue(TEXT("RunId should be non-empty"), !RunId.IsEmpty());
	TestEqual(TEXT("BeginRun should enter Active"), Brain.GetState(), EOliveBrainState::Active);
	TestEqual(TEXT("Worker phase should reset to Streaming"), Brain.GetWorkerPhase(), EOliveWorkerPhase::Streaming);

	bool bPhaseChanged = false;
	Brain.OnWorkerPhaseChanged.AddLambda([&bPhaseChanged](EOliveWorkerPhase NewPhase)
	{
		bPhaseChanged = (NewPhase == EOliveWorkerPhase::ExecutingTools);
	});
	Brain.SetWorkerPhase(EOliveWorkerPhase::ExecutingTools);
	TestTrue(TEXT("Worker phase change should fire"), bPhaseChanged);
	TestEqual(TEXT("Worker phase should be ExecutingTools"), Brain.GetWorkerPhase(), EOliveWorkerPhase::ExecutingTools);

	Brain.CompleteRun(EOliveRunOutcome::Completed);
	// 3-state model: CompleteRun transitions Active->Idle directly
	TestEqual(TEXT("Completed outcome should return to Idle"), Brain.GetState(), EOliveBrainState::Idle);
	TestEqual(TEXT("Last outcome should be Completed"), Brain.GetLastOutcome(), EOliveRunOutcome::Completed);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBrainLayerCancelTest,
	"OliveAI.Brain.Cancel",
	OliveBrainLayerTests::TestFlags)

bool FOliveBrainLayerCancelTest::RunTest(const FString& Parameters)
{
	FOliveBrainLayer Brain;
	Brain.BeginRun();

	TestTrue(TEXT("Brain should be active after BeginRun"), Brain.IsActive());

	Brain.RequestCancel();
	TestEqual(TEXT("Cancel should enter Cancelling"), Brain.GetState(), EOliveBrainState::Cancelling);

	Brain.ResetToIdle();
	TestEqual(TEXT("Reset should return to Idle"), Brain.GetState(), EOliveBrainState::Idle);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOliveBrainLayerInvalidTransitionTest,
	"OliveAI.Brain.InvalidTransitions",
	OliveBrainLayerTests::TestFlags)

bool FOliveBrainLayerInvalidTransitionTest::RunTest(const FString& Parameters)
{
	FOliveBrainLayer Brain;
	Brain.BeginRun();
	Brain.CompleteRun(EOliveRunOutcome::Completed);

	// 3-state model: Active->Idle after CompleteRun
	TestEqual(TEXT("Should be in Idle state after CompleteRun"), Brain.GetState(), EOliveBrainState::Idle);

	// Idle -> Active is the only valid transition from Idle
	const bool bTransitionAllowed = Brain.TransitionTo(EOliveBrainState::Cancelling);
	TestFalse(TEXT("Idle -> Cancelling should be invalid"), bTransitionAllowed);
	TestEqual(TEXT("State should remain Idle"), Brain.GetState(), EOliveBrainState::Idle);

	// Idle -> Active should work
	const bool bNowAllowed = Brain.TransitionTo(EOliveBrainState::Active);
	TestTrue(TEXT("Idle -> Active should be valid"), bNowAllowed);
	TestEqual(TEXT("State should be Active"), Brain.GetState(), EOliveBrainState::Active);

	return true;
}
