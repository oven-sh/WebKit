/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpectedRowsEmitter.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

void ExpectedRowsEmitter::generateExpectRows(const ValueContext& value)
{
    // The host makes the rows, at no allocation of the program: they have the general shape of their layout, which promises least.
    uint32_t shape = m_plan.shapes().generalShapeOf(value->layout());
    if (!shape) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    callVoidOperation(value, RuntimeOperation::ExpectRows, { boxed(value->targetOperand()), word32(static_cast<int32_t>(shape)) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
