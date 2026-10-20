/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

std::span<const TestCommand> flowTestCommands()
{
    static constexpr TestCommand commands[] = {
        { "flow-object-set"_s, std::nullopt, runFlowObjectSetTestCommand },
        { "flow-value"_s, IRForm::AsRead, runFlowValueTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-key-set"_s, IRForm::AsRead, runFlowKeySetTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-solve"_s, IRForm::AsRead, runFlowSolveTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-query"_s, IRForm::AsRead, runFlowQueryTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-sites"_s, IRForm::AsRead, runFlowSitesTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-walk"_s, IRForm::AsRead, runFlowPropertyWalkTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-construction"_s, IRForm::AsRead, runFlowConstructionTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-summary"_s, IRForm::AsRead, runFlowSummaryTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-links"_s, IRForm::AsRead, runFlowCallLinksTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-order"_s, IRForm::AsRead, runFlowOrderTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow"_s, IRForm::AsRead, runFlowTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-apply"_s, IRForm::AsRead, runFlowApplyTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-validate"_s, IRForm::AsRead, runFlowValidateTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
        { "flow-report"_s, IRForm::AsRead, runFlowReportTestCommand, RawWordCheck::Run, ProgramAsRead::TypedAndVerified },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
