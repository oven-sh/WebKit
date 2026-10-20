/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class ProgramEditor;
struct OptimizationInputs;

enum class OutliningDecision : uint8_t { Unknown, NotProfitable, Outlined };

void outlineColdOperations(ProgramEditor&, const OptimizationInputs&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
