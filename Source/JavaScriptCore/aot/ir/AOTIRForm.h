/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include <wtf/Assertions.h>

namespace JSC { namespace AOT { namespace Typed {

struct IRFormSet {
    uint8_t bits { 0 };

    static constexpr IRFormSet between(IRForm first, IRForm last)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(first <= last);
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(static_cast<unsigned>(last) < 8 * sizeof(bits));
        unsigned formsThroughLast = (2u << static_cast<unsigned>(last)) - 1;
        unsigned formsBeforeFirst = (1u << static_cast<unsigned>(first)) - 1;
        return { static_cast<uint8_t>(formsThroughLast & ~formsBeforeFirst) };
    }

    constexpr bool contains(IRForm form) const { return (bits >> static_cast<unsigned>(form)) & 1; }

    friend constexpr bool operator==(IRFormSet, IRFormSet) = default;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
