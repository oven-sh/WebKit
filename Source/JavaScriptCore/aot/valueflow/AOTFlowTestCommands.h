/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTestCommand.h"

namespace JSC { namespace AOT { namespace Typed {

void runFlowTestCommand(TestCommandContext&);
void runFlowApplyTestCommand(TestCommandContext&);
void runFlowValidateTestCommand(TestCommandContext&);
void runFlowReportTestCommand(TestCommandContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
