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
	TestEqual(TEXT("BeginRun should enter WorkerActive"), Brain.GetState(), EOliveBrainState::WorkerActive);
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
	TestEqual(TEXT("Completed outcome should enter Completed state"), Brain.GetState(), EOliveBrainState::Completed);

	Brain.ResetToIdle();
	TestEqual(TEXT("Reset should return to Idle"), Brain.GetState(), EOliveBrainState::Idle);

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

	TestEqual(TEXT("Should be in Completed state"), Brain.GetState(), EOliveBrainState::Completed);

	const bool bTransitionAllowed = Brain.TransitionTo(EOliveBrainState::WorkerActive);
	TestFalse(TEXT("Completed -> WorkerActive should be invalid"), bTransitionAllowed);
	TestEqual(TEXT("State should remain Completed"), Brain.GetState(), EOliveBrainState::Completed);

	// Must go through Idle
	Brain.ResetToIdle();
	const bool bNowAllowed = Brain.TransitionTo(EOliveBrainState::WorkerActive);
	TestTrue(TEXT("Idle -> WorkerActive should be valid"), bNowAllowed);
	TestEqual(TEXT("State should be WorkerActive"), Brain.GetState(), EOliveBrainState::WorkerActive);

	return true;
}

