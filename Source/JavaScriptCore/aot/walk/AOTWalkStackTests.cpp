/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A unit whose stack test its callers cover says where it makes a direct call that the plan has not counted below it, and then tests in its own frame: plan/AOTStackTestPlan.h.
// At a suspension: nothing. The body of a coroutine that may suspend tests for itself, and nothing is counted below its ramp.
// At a throw: nothing.
// Withheld, with DirectCalls: the plan covers no body, so nothing is asked.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"

namespace JSC { namespace AOT { namespace Typed {

static bool unitCountsOnCallersForStackTests(const Walker& walker)
{
    return walksInputBody(walker.unit().role) && !walker.code().holdsCallThatPlanHasNotCounted() && walker.plan().conventions().stackCheckOf(walker.unit()) == StackCheck::CoveredByCallers;
}

// The plan counts the frames below a body by what Link lists at its calls and by the steps at its bare sites. A listed call that names the body of a module leads to a start or to the loader.
static bool planHasCountedCallForStackTests(const Walker& walker, FunctionID caller, ValueID site, FunctionID callee)
{
    if (walker.plan().conventions().stackTests().hasCountedAtBareSite(caller, callee))
        return true;
    if (!site || walker.program().at(callee).isModuleBody())
        return false;
    return walker.facts().linked().calleeAt(caller, site) == callee || walker.facts().held().calleeAt(caller, site) == callee;
}

void Walker::noteDirectCallForStackTests(const Draft& draft)
{
    if (!unitCountsOnCallersForStackTests(*this))
        return;
    const CallingConventions& conventions = plan().conventions();

    // CallEmitter::generateConstruct() enters the unit itself, past the stub that finds it and tests the stack in front of it, only from a unit that tests in its own frame.
    if (draft.opcode == Opcode::Construct) {
        TypeID typeOfCallee = ownTypeOf(draft.calleeOperand());
        FunctionID constructor = typeOfCallee && m_types.at(typeOfCallee).isObject() && !draft.newTargetOperand() ? singleFunctionOf(m_types, m_types.at(typeOfCallee)) : FunctionID();
        if (constructor && conventions.exists({ constructor, CodeRole::AllocatingConstructEntry }))
            m_code.noteCallThatPlanHasNotCounted();
        return;
    }

    CodeUnit entered = conventions.unitEnteredBy(draft);
    // The body that the ramp of a coroutine goes on to is the plan's to count, together with the ramp, and stands in no list of Link's.
    if (isBodyOfCoroutine(entered.role, program().at(entered.function).isCoroutine()) && unit() == CodeUnit(entered.function, CodeRole::Body))
        return;
    // A fast version that starts over: its own slow version, entered in tail position, takes the place of this frame, and the plan has counted the one frame for either version.
    if (entered == CodeUnit(unit().function, CodeRole::SlowBody) && unit().role == CodeRole::Body && draft.isTailCall && m_frames.size() == 1)
        return;
    // A guard goes on to one of the two versions in the place of its own frame.
    CodeUnit unitThatSetsUpFrame = entered.role == CodeRole::ParameterGuard ? CodeUnit(entered.function, CodeRole::Body) : entered;
    if (conventions.stackCheckOf(unitThatSetsUpFrame) == StackCheck::Required)
        return;

    // A bound function, like any other entrance, may wait above what it goes on to, in a frame that is counted for no caller.
    bool entersBodyOfFunction = returnsResultOfFunction(entered.role) || entered.role == CodeRole::CoroutineBody;
    bool bodyBeingReadIsCounted = m_frames.size() == 1 || m_frames.last().callsAreCountedByPlan;
    if (!entersBodyOfFunction || !bodyBeingReadIsCounted || !planHasCountedCallForStackTests(*this, currentFunction(), draft.inputValueGiven, entered.function))
        m_code.noteCallThatPlanHasNotCounted();
}

// A callee that counts on its callers was counted in front of each of them, on a cycle of calls too: the member that a caller meets while it is being counted is the one that tests for itself.
// In front of such a member a caller may have been counted, so none of its calls are.
bool Walker::callsOfCopyAreCountedForStackTests(const BodyValue& call, FunctionID callee) const
{
    if (!unitCountsOnCallersForStackTests(*this) || (m_frames.size() > 1 && !m_frames.last().callsAreCountedByPlan))
        return false;
    return plan().conventions().stackTests().countsCallsOfBodyBelowCallers(callee) && planHasCountedCallForStackTests(*this, currentFunction(), call.id(), callee);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
