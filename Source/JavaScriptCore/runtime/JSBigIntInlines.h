/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSBigInt.h"
#include "JSCJSValueBigInt.h"

namespace JSC {

inline JSValue JSBigInt::toNumber(JSValue bigInt)
{
    ASSERT(bigInt.isBigInt());
#if USE(BIGINT32)
    if (bigInt.isBigInt32())
        return jsNumber(bigInt.bigInt32AsInt32());
#endif
    return toNumberHeap(uncheckedDowncast<JSBigInt>(bigInt));
}

uint64_t JSBigInt::toBigUInt64(JSValue bigInt)
{
    ASSERT(bigInt.isBigInt());
#if USE(BIGINT32)
    if (bigInt.isBigInt32())
        return static_cast<uint64_t>(static_cast<int64_t>(bigInt.bigInt32AsInt32()));
#endif
    return toBigUInt64Heap(bigInt.asHeapBigInt());
}

inline int64_t JSBigInt::toBigInt64(JSValue bigInt)
{
    ASSERT(bigInt.isBigInt());
#if USE(BIGINT32)
    if (bigInt.isBigInt32())
        return static_cast<int64_t>(bigInt.bigInt32AsInt32());
#endif
    return static_cast<int64_t>(toBigUInt64Heap(bigInt.asHeapBigInt()));
}

// |bigInt| when it fits in 64 bits.
ALWAYS_INLINE std::optional<uint64_t> JSBigInt::absoluteAsUInt64(JSBigInt* bigInt)
{
    unsigned length = bigInt->length();
    if (!length)
        return 0;
    if constexpr (sizeof(Digit) == 8) {
        if (length != 1)
            return std::nullopt;
        return bigInt->digit(0);
    } else {
        ASSERT(sizeof(Digit) == 4);
        if (length > 2)
            return std::nullopt;
        uint64_t absolute = bigInt->digit(0);
        if (length == 2)
            absolute |= static_cast<uint64_t>(bigInt->digit(1)) << 32;
        return absolute;
    }
}

ALWAYS_INLINE std::optional<double> JSBigInt::tryExtractDouble(JSValue value)
{
    if (value.isNumber())
        return value.asNumber();

    if (!value.isBigInt())
        return std::nullopt;

#if USE(BIGINT32)
    if (value.isBigInt32())
        return value.bigInt32AsInt32();
#endif

    ASSERT(value.isHeapBigInt());
    JSBigInt* bigInt = value.asHeapBigInt();
    std::optional<uint64_t> absolute = absoluteAsUInt64(bigInt);
    if (!absolute || absolute.value() > maxSafeIntegerAsUInt64())
        return std::nullopt;
    return (bigInt->sign()) ? -static_cast<double>(absolute.value()) : static_cast<double>(absolute.value());
}

ALWAYS_INLINE std::optional<int64_t> JSBigInt::tryExtractInt64(JSValue value)
{
    ASSERT(value.isBigInt());
#if USE(BIGINT32)
    if (value.isBigInt32())
        return value.bigInt32AsInt32();
#endif

    JSBigInt* bigInt = value.asHeapBigInt();
    std::optional<uint64_t> absolute = absoluteAsUInt64(bigInt);
    if (!absolute)
        return std::nullopt;
    constexpr uint64_t int64MinAbsolute = static_cast<uint64_t>(1) << 63;
    if (bigInt->sign()) {
        if (absolute.value() > int64MinAbsolute)
            return std::nullopt;
        return static_cast<int64_t>(~(absolute.value() - 1)); // Two's complement by hand, as in toBigUInt64Heap.
    }
    if (absolute.value() >= int64MinAbsolute)
        return std::nullopt;
    return static_cast<int64_t>(absolute.value());
}

ALWAYS_INLINE std::optional<uint64_t> JSBigInt::tryExtractUInt64(JSValue value)
{
    ASSERT(value.isBigInt());
#if USE(BIGINT32)
    if (value.isBigInt32()) {
        int32_t int32 = value.bigInt32AsInt32();
        if (int32 < 0)
            return std::nullopt;
        return static_cast<uint64_t>(int32);
    }
#endif

    JSBigInt* bigInt = value.asHeapBigInt();
    if (bigInt->sign())
        return std::nullopt;
    return absoluteAsUInt64(bigInt);
}

ALWAYS_INLINE JSValue JSBigInt::makeHeapBigIntOrBigInt32(JSGlobalObject* globalObject, double value)
{
    ASSERT(isInteger(value));
    if (std::abs(value) <= maxSafeInteger())
        return makeHeapBigIntOrBigInt32(globalObject, static_cast<int64_t>(value));
    return JSBigInt::createFrom(globalObject, value);
}

inline JSBigInt::ComparisonResult JSBigInt::compareToDouble(double x, int32_t y) { return flip(compareToDouble(y, x)); }
inline JSBigInt::ComparisonResult JSBigInt::compareToDouble(double x, int64_t y) { return flip(compareToDouble(y, x)); }
inline JSBigInt::ComparisonResult JSBigInt::compareToDouble(double x, uint64_t y) { return flip(compareToDouble(y, x)); }
inline JSBigInt::ComparisonResult JSBigInt::compareToDouble(double x, JSValue y) { return flip(compareToDouble(y, x)); }

}
