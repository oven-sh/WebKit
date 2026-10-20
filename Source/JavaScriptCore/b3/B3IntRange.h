/*
 * Copyright (C) 2015-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2025-2026 the V8 project authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Limits.h"
#include "B3Opcode.h"
#include "B3Type.h"
#include <algorithm>
#include <wtf/CheckedArithmetic.h>
#include <wtf/Int128.h>
#include <wtf/MathExtras.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TriState.h>

namespace JSC { namespace B3 {

class Procedure;
class Value;

class IntRange {
public:
    IntRange()
    {
    }

    IntRange(int64_t min, int64_t max)
        : m_min(min)
        , m_max(max)
    {
        ASSERT(m_min <= m_max);
    }

    template<typename T>
    static IntRange LEGACY_NODELETE top()
    {
        return IntRange(std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
    }

    static IntRange LEGACY_NODELETE top(Type type)
    {
        switch (type.kind()) {
        case Int32:
            return top<int32_t>();
        case Int64:
            return top<int64_t>();
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return IntRange();
        }
    }

    template<typename T>
    static IntRange LEGACY_NODELETE rangeForMask(T mask)
    {
        if (mask == static_cast<T>(-1))
            return top<T>();
        if constexpr (std::is_signed_v<T>) {
            if (mask < 0)
                return IntRange(std::numeric_limits<T>::min() & mask, mask & std::numeric_limits<T>::max());
        }
        return IntRange(0, mask);
    }

    static IntRange LEGACY_NODELETE rangeForMask(int64_t mask, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return rangeForMask<int32_t>(static_cast<int32_t>(mask));
        case Int64:
            return rangeForMask<int64_t>(mask);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return IntRange();
        }
    }

    template<typename T>
    static IntRange LEGACY_NODELETE rangeForZShr(int32_t shiftAmount)
    {
        std::make_unsigned_t<T> mask = 0;
        mask--;
        mask >>= shiftAmount;
        return rangeForMask<T>(static_cast<T>(mask));
    }

    static IntRange LEGACY_NODELETE rangeForZShr(int32_t shiftAmount, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return rangeForZShr<int32_t>(shiftAmount);
        case Int64:
            return rangeForZShr<int64_t>(shiftAmount);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return IntRange();
        }
    }

    int64_t LEGACY_NODELETE min() const { return m_min; }
    int64_t LEGACY_NODELETE max() const { return m_max; }

    void dump(PrintStream& out) const
    {
        out.print("[", m_min, ",", m_max, "]");
    }

    template<typename T>
    bool LEGACY_NODELETE couldOverflowAdd(const IntRange& other)
    {
        return sumOverflows<T>(m_min, other.m_min)
            || sumOverflows<T>(m_min, other.m_max)
            || sumOverflows<T>(m_max, other.m_min)
            || sumOverflows<T>(m_max, other.m_max);
    }

    bool LEGACY_NODELETE couldOverflowAdd(const IntRange& other, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return couldOverflowAdd<int32_t>(other);
        case Int64:
            return couldOverflowAdd<int64_t>(other);
        default:
            return true;
        }
    }

    template<typename T>
    bool LEGACY_NODELETE couldOverflowSub(const IntRange& other)
    {
        return differenceOverflows<T>(m_min, other.m_min)
            || differenceOverflows<T>(m_min, other.m_max)
            || differenceOverflows<T>(m_max, other.m_min)
            || differenceOverflows<T>(m_max, other.m_max);
    }

    bool LEGACY_NODELETE couldOverflowSub(const IntRange& other, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return couldOverflowSub<int32_t>(other);
        case Int64:
            return couldOverflowSub<int64_t>(other);
        default:
            return true;
        }
    }

    template<typename T>
    bool LEGACY_NODELETE couldOverflowMul(const IntRange& other)
    {
        return productOverflows<T>(m_min, other.m_min)
            || productOverflows<T>(m_min, other.m_max)
            || productOverflows<T>(m_max, other.m_min)
            || productOverflows<T>(m_max, other.m_max);
    }

    bool LEGACY_NODELETE couldOverflowMul(const IntRange& other, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return couldOverflowMul<int32_t>(other);
        case Int64:
            return couldOverflowMul<int64_t>(other);
        default:
            return true;
        }
    }

    template<typename T>
    IntRange LEGACY_NODELETE shl(int32_t shiftAmount)
    {
        T newMin = static_cast<T>(m_min) << static_cast<T>(shiftAmount);
        T newMax = static_cast<T>(m_max) << static_cast<T>(shiftAmount);

        if (((newMin >> shiftAmount) != static_cast<T>(m_min))
            || ((newMax >> shiftAmount) != static_cast<T>(m_max))) {
            newMin = std::numeric_limits<T>::min();
            newMax = std::numeric_limits<T>::max();
        }

        return IntRange(newMin, newMax);
    }

    IntRange LEGACY_NODELETE shl(int32_t shiftAmount, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return shl<int32_t>(shiftAmount);
        case Int64:
            return shl<int64_t>(shiftAmount);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return IntRange();
        }
    }

    template<typename T>
    IntRange LEGACY_NODELETE sShr(int32_t shiftAmount)
    {
        T newMin = static_cast<T>(m_min) >> static_cast<T>(shiftAmount);
        T newMax = static_cast<T>(m_max) >> static_cast<T>(shiftAmount);

        return IntRange(newMin, newMax);
    }

    IntRange LEGACY_NODELETE sShr(int32_t shiftAmount, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return sShr<int32_t>(shiftAmount);
        case Int64:
            return sShr<int64_t>(shiftAmount);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return IntRange();
        }
    }

    template<typename T>
    IntRange LEGACY_NODELETE zShr(int32_t shiftAmount)
    {
        // This is an awkward corner case for all of the other logic.
        if (!shiftAmount)
            return *this;

        // If the input range may be negative, then all we can say about the output range is that it
        // will be masked. That's because -1 right shifted just produces that mask.
        if (m_min < 0)
            return rangeForZShr<T>(shiftAmount);

        // If the input range is non-negative, then this just brings the range closer to zero.
        using UnsignedT = std::make_unsigned_t<T>;
        UnsignedT newMin = static_cast<UnsignedT>(m_min) >> static_cast<UnsignedT>(shiftAmount);
        UnsignedT newMax = static_cast<UnsignedT>(m_max) >> static_cast<UnsignedT>(shiftAmount);
        
        return IntRange(newMin, newMax);
    }

    IntRange LEGACY_NODELETE zShr(int32_t shiftAmount, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return zShr<int32_t>(shiftAmount);
        case Int64:
            return zShr<int64_t>(shiftAmount);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return IntRange();
        }
    }

    template<typename T>
    IntRange LEGACY_NODELETE add(const IntRange& other)
    {
        if (couldOverflowAdd<T>(other))
            return top<T>();
        return IntRange(m_min + other.m_min, m_max + other.m_max);
    }

    IntRange LEGACY_NODELETE add(const IntRange& other, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return add<int32_t>(other);
        case Int64:
            return add<int64_t>(other);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return IntRange();
        }
    }

    template<typename T>
    IntRange LEGACY_NODELETE sub(const IntRange& other)
    {
        if (couldOverflowSub<T>(other))
            return top<T>();
        return IntRange(m_min - other.m_max, m_max - other.m_min);
    }

    IntRange LEGACY_NODELETE sub(const IntRange& other, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return sub<int32_t>(other);
        case Int64:
            return sub<int64_t>(other);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return IntRange();
        }
    }

    template<typename T>
    IntRange mul(const IntRange& other)
    {
        if (couldOverflowMul<T>(other))
            return top<T>();
        auto [min, max] = std::minmax({
            m_min * other.m_min,
            m_min * other.m_max,
            m_max * other.m_min,
            m_max * other.m_max
        });
        return IntRange(min, max);
    }

    IntRange mul(const IntRange& other, Type type)
    {
        switch (type.kind()) {
        case Int32:
            return mul<int32_t>(other);
        case Int64:
            return mul<int64_t>(other);
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return IntRange();
        }
    }

    template<typename T>
    IntRange LEGACY_NODELETE sExt()
    {
        ASSERT(m_min >= INT32_MIN);
        ASSERT(m_max <= INT32_MAX);
        int64_t typeMin = std::numeric_limits<T>::min();
        int64_t typeMax = std::numeric_limits<T>::max();
        auto min = m_min;
        auto max = m_max;

        if (typeMin <= min && min <= typeMax
            && typeMin <= max && max <= typeMax)
            return IntRange(min, max);

        // Given type T with N bits, signed extension will turn bit N-1 as
        // a sign bit. If bits N-1 upwards are identical for both min and max,
        // then we're guaranteed that even after the sign extension, min and
        // max will still be in increasing order.
        //
        // For example, when T is int8_t, the space of numbers from highest to
        // lowest are as follows (in binary bits):
        //
        //      highest     0 111 1111  ^
        //                    ...       |
        //            1     0 000 0001  |   top segment
        //            0     0 000 0000  v
        //
        //           -1     1 111 1111  ^
        //           -2     1 111 1110  |   bottom segment
        //                    ...       |
        //       lowest     1 000 0000  v
        //
        // Note that if we exclude the sign bit, the range is made up of 2 segments
        // of contiguous increasing numbers. If min and max are both in the same
        // segment before the sign extension, then min and max will continue to be
        // in a contiguous segment after the sign extension. Only when min and max
        // spans across more than 1 of these segments, will min and max no longer
        // be guaranteed to be in a contiguous range after the sign extension.
        //
        // Hence, we can check if bits N-1 and up are identical for the range min
        // and max. If so, then the new min and max can be be computed by simply
        // applying sign extension to their original values.

        constexpr unsigned numberOfBits = countOfBits<T>;
        constexpr int64_t segmentMask = (1ll << (numberOfBits - 1)) - 1;
        constexpr int64_t topBitsMask = ~segmentMask;
        int64_t minTopBits = topBitsMask & min;
        int64_t maxTopBits = topBitsMask & max;

        if (minTopBits == maxTopBits)
            return IntRange(static_cast<int64_t>(static_cast<T>(min)), static_cast<int64_t>(static_cast<T>(max)));

        return top<T>();
    }

    IntRange LEGACY_NODELETE zExt32()
    {
        ASSERT(m_min >= INT32_MIN);
        ASSERT(m_max <= INT32_MAX);
        uint64_t min = static_cast<uint64_t>(static_cast<uint32_t>(m_min));
        uint64_t max = static_cast<uint64_t>(static_cast<uint32_t>(m_max));
        if (m_max < 0 || m_min >= 0) {
            // m_min = -2, m_max = -1 then should return [0xFFFF_FFFE, 0xFFFF_FFFF]
            // m_min =  1, m_max =  2 then should return [1, 2]
            return IntRange(min, max);
        }
        // m_min = a negative integer, m_max >= 0 then should return [0, 0xFFFF_FFFF]
        return IntRange(0, std::numeric_limits<uint32_t>::max());
    }

    friend bool operator==(const IntRange&, const IntRange&) = default;
    bool isSubsetOf(const IntRange& other) const { return other.m_min <= m_min && m_max <= other.m_max; }
    bool overlaps(const IntRange& other) const { return m_min <= other.m_max && other.m_min <= m_max; }
    IntRange intersectWith(const IntRange& other) const { ASSERT(overlaps(other)); return IntRange(std::max(m_min, other.m_min), std::min(m_max, other.m_max)); }

private:
    int64_t m_min { 0 };
    int64_t m_max { 0 };
};

enum class Signedness : uint8_t { Signed, Unsigned };

struct IntegerInterval {
    Int128 min { 0 };
    Int128 max { 0 };

    bool contains(const IntegerInterval& other) const { return min <= other.min && other.max <= max; }
    bool overlaps(const IntegerInterval& other) const { return min <= other.max && other.min <= max; }
    IntegerInterval join(const IntegerInterval& other) const { return { std::min(min, other.min), std::max(max, other.max) }; }

    friend bool operator==(const IntegerInterval&, const IntegerInterval&) = default;
};

JS_EXPORT_PRIVATE IntRange rangeFor(const Procedure&, Value*);
JS_EXPORT_PRIVATE bool isKnownRangePlausible(const Procedure&, Value*);

JS_EXPORT_PRIVATE IntegerInterval fullInterval(Type, Signedness);
JS_EXPORT_PRIVATE IntegerInterval intervalFor(const Procedure&, Value*, Signedness);

JS_EXPORT_PRIVATE TriState evaluateComparison(Opcode, const IntRange& left, const IntRange& right);
inline TriState evaluateComparison(Opcode opcode, const IntRange& left, int64_t right) { return evaluateComparison(opcode, left, IntRange(right, right)); }
JS_EXPORT_PRIVATE TriState evaluateComparison(Opcode, const IntegerInterval& left, const IntegerInterval& right);
JS_EXPORT_PRIVATE TriState evaluateComparison(const Procedure&, Value* comparison);

inline bool isIntegerComparison(Opcode opcode)
{
    switch (opcode) {
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Below:
    case BelowEqual:
    case Above:
    case AboveEqual:
        return true;
    default:
        return false;
    }
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
