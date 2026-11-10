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
#include <bit>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static FunctionID functionOfClosureInTailCalls(const Code& code, ValueID value)
{
    if (!value)
        return { };
    const Operation& made = code.at(underlyingCellOf(code, value));
    return made.opcode == Opcode::NewClosure ? made.function() : FunctionID();
}

static bool calleeComesBackInTailCalls(const Walker& walker, FunctionID callee, NOESCAPE const Invocable<ValueID(unsigned)> auto& argumentFor)
{
    const Plan& plan = walker.plan();
    if (!walker.program().at(callee).hasBody() || plan.endsItsTailCalls(callee))
        return true;
    uint64_t mustEndTheirs = plan.entryParametersThatMustEndTheirTailCalls(callee);
    if (!mustEndTheirs)
        return false;
    for (; mustEndTheirs; mustEndTheirs &= mustEndTheirs - 1) {
        FunctionID made = functionOfClosureInTailCalls(walker.code(), argumentFor(static_cast<unsigned>(std::countr_zero(mustEndTheirs))));
        if (!made || !plan.endsItsTailCalls(made))
            return false;
    }
    return true;
}

bool calleeOfTailCallComesBack(const Walker& walker, FunctionID callee, std::span<const Reference> arguments)
{
    return calleeComesBackInTailCalls(walker, callee, [&](unsigned entryParameter) {
        size_t position = entryParameter - walker.program().at(callee).firstParameterIndex();
        return position < arguments.size() && arguments[position].isRecord() ? arguments[position].id() : ValueID();
    });
}

TailCallRequirement requirementOfTailCall(const Walker& walker, Reference call)
{
    if (!call || !call.isValue())
        return TailCallRequirement::Required;
    const Operation& record = walker.code().at(call.id());
    if (record.opcode != Opcode::CallDirect)
        return TailCallRequirement::Required;
    std::span<const ValueID> arguments;
    if (returnsResultOfFunction(record.role()) && !record.numberOfBoundArguments())
        arguments = record.argumentsOperands();
    bool comesBack = calleeComesBackInTailCalls(walker, record.function(), [&](unsigned entryParameter) {
        return entryParameter < arguments.size() ? arguments[entryParameter] : ValueID();
    });
    return comesBack ? TailCallRequirement::Allowed : TailCallRequirement::Required;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
