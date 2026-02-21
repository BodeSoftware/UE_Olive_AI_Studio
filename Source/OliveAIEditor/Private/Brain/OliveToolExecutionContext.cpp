// Copyright Bode Software. All Rights Reserved.

#include "Brain/OliveToolExecutionContext.h"
#include "OliveAIEditorModule.h"

// File-scoped thread_local avoids MSVC C2492 (thread_local + dllexport)
static thread_local const FOliveToolCallContext* GCurrentToolContext = nullptr;

const FOliveToolCallContext*& FOliveToolExecutionContext::GetCurrentContextRef()
{
	return GCurrentToolContext;
}

const FOliveToolCallContext* FOliveToolExecutionContext::Get()
{
	return GetCurrentContextRef();
}

bool FOliveToolExecutionContext::IsFromMCP()
{
	const FOliveToolCallContext* Ctx = Get();
	return Ctx && Ctx->Origin == EOliveToolCallOrigin::MCP;
}

FOliveToolExecutionContextScope::FOliveToolExecutionContextScope(const FOliveToolCallContext& InContext)
	: PreviousContext(FOliveToolExecutionContext::GetCurrentContextRef())
	, StoredContext(InContext)
{
	FOliveToolExecutionContext::GetCurrentContextRef() = &StoredContext;
}

FOliveToolExecutionContextScope::~FOliveToolExecutionContextScope()
{
	FOliveToolExecutionContext::GetCurrentContextRef() = PreviousContext;
}
