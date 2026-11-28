/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"

namespace JSC { namespace AOT { namespace Typed {

static bool unitCountsOnCallersForStackTests(const Walker& walker)
{
    return walksInputBody(walker.unit().role) && !walker.code().holdsCallThatPlanHasNotCounted() && walker.plan().conventions().stackCheckOf(walker.unit()) == StackCheck::CoveredByCallers;
}

static bool planHasCountedCallForStackTests(const Walker& walker, FunctionID caller, ValueID site, FunctionID callee)
{
    if (walker.plan().conventions().stackTests().hasCountedAtBareSite(caller, callee))
        return true;
    if (!site || walker.program().at(callee).isModuleBody())
        return false;
    return walker.facts().linked().calleeAt(caller, site) == callee || walker.facts().held().calleeAt(caller, site) == callee;
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::noteDirectCallForStackTests(const Draft& draft)
{
    if (!unitCountsOnCallersForStackTests(*this))
        return;
    const CallingConventions& conventions = plan().conventions();

    if (draft.opcode == Opcode::Construct) {
        TypeID typeOfCallee = ownTypeOf(draft.calleeOperand());
        FunctionID constructor = typeOfCallee && m_types.at(typeOfCallee).isObject() && !draft.newTargetOperand() ? singleFunctionOf(m_types, m_types.at(typeOfCallee)) : FunctionID();
        if (constructor && conventions.exists({ constructor, CodeRole::AllocatingConstructEntry }))
            m_code.noteCallThatPlanHasNotCounted();
        return;
    }

    CodeUnit entered = conventions.unitEnteredBy(draft);
    if (isBodyOfCoroutine(entered.role, program().at(entered.function).isCoroutine()) && unit() == CodeUnit(entered.function, CodeRole::Body))
        return;
    if (entered == CodeUnit(unit().function, CodeRole::SlowBody) && unit().role == CodeRole::Body && draft.isTailCall && walksOwnBodyOfUnit())
        return;
    CodeUnit unitThatSetsUpFrame = entered.role == CodeRole::ParameterGuard ? CodeUnit(entered.function, CodeRole::Body) : entered;
    if (conventions.stackCheckOf(unitThatSetsUpFrame) == StackCheck::Required)
        return;

    bool entersBodyOfFunction = returnsResultOfFunction(entered.role) || entered.role == CodeRole::CoroutineBody;
    bool bodyBeingReadIsCounted = walksOwnBodyOfUnit() || m_frames.last().callsAreCountedByPlan;
    if (!entersBodyOfFunction || !bodyBeingReadIsCounted || !planHasCountedCallForStackTests(*this, currentFunction(), draft.inputValueGiven, entered.function))
        m_code.noteCallThatPlanHasNotCounted();
}

bool Walker::callsOfCopyAreCountedForStackTests(const BodyValue& call, FunctionID callee) const
{
    if (!unitCountsOnCallersForStackTests(*this) || (!walksOwnBodyOfUnit() && !m_frames.last().callsAreCountedByPlan))
        return false;
    return plan().conventions().stackTests().countsCallsOfBodyBelowCallers(callee) && planHasCountedCallForStackTests(*this, currentFunction(), call.id(), callee);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
