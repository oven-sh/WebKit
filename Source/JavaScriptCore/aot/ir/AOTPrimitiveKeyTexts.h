/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPrimitives.h"
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

struct PrimitiveKeyText {
    uint16_t primitive;
    ASCIILiteral text;
};

// The primitives of which ToPropertyKey makes one text each.
inline constexpr PrimitiveKeyText primitiveKeyTexts[] = {
    { Primitive::Undefined, "undefined"_s },
    { Primitive::Null, "null"_s },
    { Primitive::False, "false"_s },
    { Primitive::True, "true"_s },
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
