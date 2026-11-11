/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <utility>
#include <wtf/Compiler.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename T>
class Expected {
public:
    constexpr Expected(T value)
        : m_value(std::move(value))
    {
    }

    constexpr const T& predicted() const& LIFETIME_BOUND { return m_value; }
    constexpr T predicted() && { return std::move(m_value); }

private:
    T m_value { };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
