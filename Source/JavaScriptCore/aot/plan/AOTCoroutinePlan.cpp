/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutinePlan.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTMechanisms.h"
#include "AOTThinSummary.h"

namespace JSC { namespace AOT { namespace Typed {

bool isAsyncFunctionInWhichNothingSuspends(const Program& program, const Facts& facts, FunctionID id)
{
    const Function& function = program.at(id);
    if (!facts.mechanisms().contains(Mechanism::TypedOperations))
        return false;
    if (!function.has(FunctionFlags::IsAsync) || function.has(FunctionFlags::IsGenerator) || function.isModuleBody())
        return false;
    return !facts.summaries().at(id).has(SummaryTrait::Suspends);
}

bool isNonYieldingGenerator(const Program& program, const Facts& facts, FunctionID id)
{
    const Function& function = program.at(id);
    if (!facts.mechanisms().contains(Mechanism::TypedOperations) || !facts.mechanisms().contains(Mechanism::Guesses))
        return false;
    if (!function.has(FunctionFlags::IsGenerator) || function.has(FunctionFlags::IsAsync) || function.isModuleBody() || function.results.size() != 1 || !function.calleeParameterIndex() || isLibraryCode(program, function))
        return false;
    const ThinSummary& summary = facts.summaries().at(id);
    return !summary.has(SummaryTrait::SuspendsAfterStart) && !summary.has(SummaryTrait::HasDirectEval) && !summary.has(SummaryTrait::UsesArguments);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
