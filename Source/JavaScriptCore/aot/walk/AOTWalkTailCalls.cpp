/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

static bool calleeComesBackInTailCalls(const Walker& walker, FunctionID callee)
{
    return !walker.program().at(callee).hasBody() || walker.plan().endsItsTailCalls(callee);
}

TailCallRequirement requirementOfTailCall(const Walker& walker, Reference call)
{
    if (!call || !call.isValue())
        return TailCallRequirement::Required;
    const Operation& record = walker.code().at(call.id());
    if (record.opcode != Opcode::CallDirect)
        return TailCallRequirement::Required;
    return calleeComesBackInTailCalls(walker, record.function()) ? TailCallRequirement::Allowed : TailCallRequirement::Required;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
