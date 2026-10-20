/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"

namespace JSC { namespace AOT {

bool isOptionalPass(PassID pass)
{
    return pass != PassID::None && Typed::descriptorOf(pass).necessity == Typed::PassNecessity::Optional;
}

namespace Typed {

FunctionID FunctionPassContext::derivedFunction(const DerivedFunctionKey& key)
{
    RELEASE_ASSERT(inputs.derivedFunctions);
    return inputs.derivedFunctions->ensure(key);
}

double FunctionPassContext::heatOf(BlockID block)
{
    return inputs.heat.activationsOf(program, id) * analyses.blockFrequencies().frequencyOf(block);
}

bool FunctionPassContext::runsAtMostOnce(BlockID block)
{
    return analyses.executionCountOf(block) == ExecutionCount::AtMostOnce;
}

Subject FunctionPassContext::subject() const
{
    return analyses.subject();
}

Subject FunctionPassContext::subject(BlockID block) const
{
    Subject result = subject();
    if (!block)
        return result;
    if (result.function == id)
        result.block = block;
    for (ValueID value : function.at(block).values) {
        SourceLocation location = function.locationOf(value);
        if (!hasModule(location))
            continue;
        result.location = location;
        break;
    }
    return result;
}

Subject FunctionPassContext::subject(ValueID value) const
{
    return analyses.subject(value);
}

uint64_t FunctionPassContext::limit(LimitID limit) const
{
    return build.limit(limit);
}

bool FunctionPassContext::exceeds(LimitID limit, uint64_t amount, ValueID value)
{
    if (amount <= build.limit(limit))
        return false;
    build.gaveUp(limit, subject(value));
    return true;
}

GaveUp FunctionPassContext::gaveUp(LimitID limit, ValueID value)
{
    return build.gaveUp(limit, subject(value));
}

void FunctionPassContext::addDecline(Wire::DeclineKind kind, ValueID value, const DeclineReason& reason, LimitID limit, uint32_t count)
{
    if (!build.options().collectsDeclines())
        return;
    build.addDecline({ .kind = kind, .limit = limit, .count = count, .reason = encodeDeclineReason(kind, reason), .witness = witnessOf(subject(value)) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
