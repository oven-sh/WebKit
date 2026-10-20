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
        { "valueflow-body"_s, IRForm::AsRead, runValueFlowBodyTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "valueflow-bodies"_s, IRForm::AsRead, runValueFlowBodiesTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "valueflow-solve"_s, IRForm::AsRead, runValueFlowSolveTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "valueflow-compare"_s, IRForm::AsRead, runValueFlowCompareTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "valueflow-families"_s, IRForm::AsRead, runValueFlowFamiliesTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "valueflow-why"_s, IRForm::AsRead, runValueFlowWhyTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "valueflow-results"_s, IRForm::AsRead, runValueFlowResultsTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "valueflow-apply"_s, IRForm::AsRead, runValueFlowApplyTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
