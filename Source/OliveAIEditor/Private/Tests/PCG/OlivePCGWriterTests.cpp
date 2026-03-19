// Copyright Bode Software. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "PCGGraph.h"
#include "UObject/Package.h"
#include "Writer/OlivePCGWriter.h"

namespace OlivePCGWriterTests
{
	static constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOlivePCGWriterRepeatedExecuteTest,
	"OliveAI.PCG.WriterRepeatedExecute",
	OlivePCGWriterTests::TestFlags)

bool FOlivePCGWriterRepeatedExecuteTest::RunTest(const FString& Parameters)
{
	UPackage* Package = GetTransientPackage();
	TestNotNull(TEXT("Transient package should exist"), Package);

	UPCGGraph* Graph = NewObject<UPCGGraph>(Package, NAME_None, RF_Transient);
	TestNotNull(TEXT("Transient PCG graph should be created"), Graph);
	if (!Graph)
	{
		return false;
	}

	FOlivePCGWriter& Writer = FOlivePCGWriter::Get();
	const FPCGExecuteResult FirstResult = Writer.Execute(Graph, 5.0f);
	TestTrue(TEXT("First execute should succeed"), FirstResult.bSuccess);

	const FPCGExecuteResult SecondResult = Writer.Execute(Graph, 5.0f);
	TestTrue(TEXT("Second execute should also succeed"), SecondResult.bSuccess);

	return FirstResult.bSuccess && SecondResult.bSuccess;
}
