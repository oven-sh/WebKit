/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

// The value an operation hands on unchanged: the operand of a cast, of a request, of the operations that finish a literal or a class.
ValueID sameValueOperandOf(const Value&);
ValueID definitionThroughSameValues(const Function&, ValueID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
