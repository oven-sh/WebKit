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
        { "flow"_s, IRForm::AsRead, runFlowTestCommand, ProgramAsRead::TypedAndVerified },
        { "flow-apply"_s, IRForm::AsRead, runFlowApplyTestCommand, ProgramAsRead::TypedAndVerified },
        { "flow-report"_s, IRForm::AsRead, runFlowReportTestCommand, ProgramAsRead::TypedAndVerified },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
