/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"

namespace JSC { namespace AOT { namespace Typed {

class PassRunner;
class ProgramAnalyses;

void runOnFunctions(FunctionSchedule, Program&, const OptimizationInputs&, const PassRunner&, ProgramAnalyses&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
