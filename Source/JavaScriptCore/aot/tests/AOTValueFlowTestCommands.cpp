/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

std::span<const TestCommand> valueFlowTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "valueflow-body"_s, IRForm::AsRead, runValueFlowBodyTestCommand, ProgramAsRead::TypedAndVerified },
        { "valueflow-bodies"_s, IRForm::AsRead, runValueFlowBodiesTestCommand, ProgramAsRead::TypedAndVerified },
        { "valueflow-solve"_s, IRForm::AsRead, runValueFlowSolveTestCommand, ProgramAsRead::TypedAndVerified },
        { "valueflow-families"_s, IRForm::AsRead, runValueFlowFamiliesTestCommand, ProgramAsRead::TypedAndVerified },
        { "valueflow-why"_s, IRForm::AsRead, runValueFlowWhyTestCommand, ProgramAsRead::TypedAndVerified },
        { "valueflow-results"_s, IRForm::AsRead, runValueFlowResultsTestCommand, ProgramAsRead::TypedAndVerified },
        { "valueflow-apply"_s, IRForm::AsRead, runValueFlowApplyTestCommand, ProgramAsRead::TypedAndVerified },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
