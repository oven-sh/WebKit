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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
