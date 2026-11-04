/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `function walk(n, acc) { if (!n) return acc; return walk(n.next, acc + n.size) }`: a call of the function by itself in tail position is a jump to a head behind the entry of its Body. A turn is an entry behind the guard.
// At a suspension: a function that suspends has no head.
// At a throw: a call under a handler does not come round. The poll at the head throws where the entry block throws.
// Withheld, with DirectCalls: no head is opened, and every ReturnCall says Required.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningRules.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTailCallPlan.h"
#include "AOTThinSummary.h"

namespace JSC { namespace AOT { namespace Typed {

// ---- Who may keep his frame

static bool calleeComesBackInTailCalls(const Walker& walker, FunctionID callee)
{
    return !walker.program().at(callee).hasBody() || walker.plan().endsItsTailCalls(callee);
}

// A callee that ends its own tail calls costs one frame where the caller keeps its own. Whatever is entered by value may go on without end.
TailCallRequirement requirementOfTailCall(const Walker& walker, Reference call)
{
    if (!call || !call.isValue())
        return TailCallRequirement::Required;
    const Operation& record = walker.code().at(call.id());
    if (record.opcode != Opcode::CallDirect)
        return TailCallRequirement::Required;
    return calleeComesBackInTailCalls(walker, record.function()) ? TailCallRequirement::Allowed : TailCallRequirement::Required;
}

// ---- The head

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
