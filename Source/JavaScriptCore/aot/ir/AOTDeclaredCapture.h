/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDeclaredType.h"

namespace JSC { namespace AOT { namespace Typed {

struct DeclaredCapture : DeclaredType {
    constexpr DeclaredCapture() = default;
    DeclaredCapture(const DeclaredType& declared, StringID variableName = { })
        : DeclaredType(declared)
        , variableName(variableName)
    {
    }

    friend bool operator==(const DeclaredCapture&, const DeclaredCapture&) = default;

    StringID variableName;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
