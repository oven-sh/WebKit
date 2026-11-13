/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCallingConventions.h"

namespace JSC { namespace AOT { namespace Typed {

class Plan;

ScalarizedResults planScalarizedResults(const Program&, const Facts&, const Plan&);
ScalarizedParameters planScalarizedParameters(const Program&, const Facts&, const Plan&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
