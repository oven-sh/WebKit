/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <bit>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class TypeArena;

class ValueKinds {
public:
    static ValueKinds of(const Type&);

    constexpr ValueKinds() = default;

    constexpr bool isEmpty() const { return !m_bits; }
    constexpr bool containsAll(ValueKinds other) const { return !(other.m_bits & ~m_bits); }
    constexpr uint32_t size() const { return std::popcount(m_bits); }
    constexpr void add(ValueKinds other) { m_bits |= other.m_bits; }

    Type type() const;

    friend constexpr bool operator==(ValueKinds, ValueKinds) = default;

private:
    enum Bit : uint8_t { Boolean = 1, Number = 2, String = 4, Symbol = 8, BigInt = 16, Object = 32 };

    constexpr explicit ValueKinds(uint8_t bits)
        : m_bits(bits)
    {
    }

    uint8_t m_bits { 0 };
};

bool includesData(const Program&, const Type&, const DataNode&);
// The run time stores the members of constant data without a test.
void expectFieldsFilledWithOtherData(Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
