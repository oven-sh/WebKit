/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

Wire::CheckCause computeUnknownObjectCause(const Function&, ValueID operand);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
