/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class ProgramEditor;
struct OptimizationInputs;

void splitFunctionsAtCalls(ProgramEditor&, const OptimizationInputs&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
