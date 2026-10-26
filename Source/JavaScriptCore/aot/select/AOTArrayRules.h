/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTIR.h"
#include "AOTLimits.h"

namespace JSC { namespace AOT { namespace Typed {

class SelectionContext;

inline constexpr size_t maxInlineConcatOperands = defaultLimitValue(LimitID::MaxInlineConcatOperands, hostArchitecture());
inline constexpr size_t maxInlinePushOperands = defaultLimitValue(LimitID::MaxInlinePushOperands, hostArchitecture());
inline constexpr size_t maxInlineSpliceInsertions = defaultLimitValue(LimitID::MaxInlineSpliceInsertions, hostArchitecture());
inline constexpr int64_t maxRemovedByInlineSplice = 2;
static_assert(maxRemovedByInlineSplice <= maxArrayElementsInRegisters);

ArrayLiteralStorage chooseArrayLiteralStorage(SelectionContext&, ValueID literal);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
