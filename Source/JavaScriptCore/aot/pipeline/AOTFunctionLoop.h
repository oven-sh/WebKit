/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include <span>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class PassRunner;
class ProgramAnalyses;

// Reads one function as a schedule that runs in place has left it, with the ids that it keeps.
using ReaderOfScheduledFunction = ScopedLambda<void(FunctionID)>;

// What rides along with a schedule that runs in place.
struct StepsAroundSchedule {
    // Right in front of the schedule of a function, by the thread that runs it. It reads no other body, and of this one it changes only words that no analysis reads.
    const ScopedLambda<void(FunctionID, Function&)>* inFront { nullptr };
    // One reader after the other goes through a batch of functions, so that the code of a reader stays in the cache as the functions do.
    std::span<const ReaderOfScheduledFunction* const> behind;
    // Once, beside the loop, inside its interning phase and its caches. It reads no body and interns nothing.
    const ScopedLambda<void()>* beside { nullptr };
};

void runOnFunctions(FunctionSchedule, Program&, const OptimizationInputs&, const PassRunner&, ProgramAnalyses&, const StepsAroundSchedule& = { });

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
