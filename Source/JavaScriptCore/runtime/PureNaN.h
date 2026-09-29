/*
 * Copyright (C) 2014 Apple Inc. All rights reserved.
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

#include <bit>
#include <limits>
#include <wtf/Assertions.h>
#include <wtf/StdLibExtras.h>

namespace JSC {

// NaN (not-a-number) double values are central to how JavaScriptCore encodes JavaScript
// values (JSValues).  All values, including integers and non-numeric values, are always
// encoded using the IEEE 754 binary double format.  Non-double values are encoded using
// a NaN with the sign bit set.  The 51-bit payload is then used for encoding the actual
// value - be it an integer or a pointer to an object, or something else. But we only
// make use of the low 49 bits and the top 15 bits being all set to 1 is the indicator
// that a value is not a double. Top 15 bits being set to 1 also indicate a signed
// signaling NaN with some additional NaN payload bits.
//
// Our use of NaN encoding means that we have to be careful with how we use NaNs for
// ordinary doubles. For example, it would be wrong to ever use a NaN that has the top
// 15 bits set, as that would look like a non-double value to JSC.
//
// We can trust that on all of the hardware/OS combinations that we care about,
// NaN-producing math operations never produce a NaN that looks like a tagged value. But
// if we're ever in a situation where we worry about it, we can use purifyNaN() to get a
// NaN that doesn't look like a tagged non-double value. The JavaScript language doesn't
// distinguish between different flavors of NaN and there is no way to detect what kind
// of NaN you have - hence so long as all double NaNs are purified then our tagging
// scheme remains sound.
//
// It's worth noting that there are cases, like sin(), that will almost produce a NaN
// that breaks us. sin(-inf) returns 0xfff8000000000000. This doesn't break us because
// not all of the top 15 bits are set. But it's very close. Hence our assumptions about
// NaN are just about the most aggressive assumptions we could possibly make without
// having to call purifyNaN() in surprising places.
//
// For naming purposes, we say that a NaN is "pure" if it is safe to tag, in the sense
// that doing so would result in a tagged value that would pass the "are you a double"
// test. We say that a NaN is "impure" if attempting to tag it would result in a value
// that would look like something other than a double.

// Some kind of pure NaN.
// Be sure that we define exactly the kind of NaN that is safe. We engineer the bits
// ourselves to ensure that it's !isImpureNaN(). FWIW, this is what
// numeric_limits<double>::quiet_NaN() returns on Mac/X86_64. But AFAICT there is
// no guarantee that quiet_NaN would return a pureNaN on all platforms. For example,
// the docs appear to imply that quiet_NaN could even return a double with the
// signaling bit set on hardware that doesn't do signaling. That would probably
// never happen, but it's healthy to be paranoid.
static constexpr uint64_t PNaNAsBits { 0x7ff8000000000000ULL };
static constexpr double PNaN { std::bit_cast<double>(PNaNAsBits) };
static constexpr uint64_t ImpureNaNAsBits { 0xffff000000000000ULL };
static constexpr double ImpureNaN { std::bit_cast<double>(ImpureNaNAsBits) };
static constexpr size_t JSValueDoubleEncodeOffsetBit { 49 };
static constexpr uint64_t JSValueDoubleEncodeOffset { 1ULL << JSValueDoubleEncodeOffsetBit };
static constexpr uint64_t JSValueNumberTag { 0xfffe000000000000ULL };

inline constexpr bool isImpureNaN(double value)
{
    // Impure when encoded double is not recognized as double.
    uint64_t bits = std::bit_cast<uint64_t>(value);
    bits += JSValueDoubleEncodeOffset;
    uint64_t mask = bits & JSValueNumberTag;
    return !mask || mask == JSValueNumberTag;
}

static_assert(!isImpureNaN(std::numeric_limits<double>::quiet_NaN()));
static_assert(!isImpureNaN(-std::numeric_limits<double>::quiet_NaN()));
static_assert(!isImpureNaN(PNaN));
static_assert(!isImpureNaN(-PNaN));

// If the given value is NaN then return a NaN that is known to be pure.
inline constexpr double purifyNaN(double value)
{
    if (value != value)
        return PNaN;
    return value;
}

// The same, for a language in which a program can tell one NaN from another. It keeps the sign, and as much as it can of the rest.
//
// It is not enough for such a NaN to be pure. It has to stay so when its sign is changed, which the interpreter does to the encoded value, without looking: 0x7ffe000000000000 is pure, and is encoded as
// 0x8000000000000000, which with its top bit turned off is a pointer. What is pure whichever sign it has is what has bit 50 clear, and that is still so when the hardware sets bit 51, as it does to a signalling NaN that it is
// given to work on. So there are 50 bits to tell NaNs apart by, where the hardware has 51.
inline constexpr double purifyNaNKeepingPayload(double value)
{
    if (value == value) [[likely]]
        return value;
    constexpr uint64_t unsafeBit = 1ULL << 50;
    constexpr uint64_t quietBit = 1ULL << 51;
    constexpr uint64_t mantissaMask = (1ULL << 52) - 1;
    uint64_t bits = std::bit_cast<uint64_t>(value) & ~unsafeBit;
    // With nothing left it would be an infinity.
    if (!(bits & mantissaMask))
        bits |= quietBit;
    return std::bit_cast<double>(bits);
}

namespace PureNaNInternal {
constexpr bool staysPure(uint64_t bits)
{
    uint64_t purified = std::bit_cast<uint64_t>(purifyNaNKeepingPayload(std::bit_cast<double>(bits)));
    constexpr uint64_t signBit = 1ULL << 63;
    constexpr uint64_t quietBit = 1ULL << 51;
    for (uint64_t candidate : { purified, purified ^ signBit, purified | quietBit, (purified ^ signBit) | quietBit }) {
        double value = std::bit_cast<double>(candidate);
        if (value == value || isImpureNaN(value))
            return false;
        // As the interpreter changes the sign of one
        uint64_t encoded = (candidate + JSValueDoubleEncodeOffset) ^ signBit;
        uint64_t mask = encoded & JSValueNumberTag;
        if (!mask || mask == JSValueNumberTag)
            return false;
    }
    return (purified & signBit) == (bits & signBit);
}
}
static_assert(PureNaNInternal::staysPure(0x7ff0000000000001ULL));
static_assert(PureNaNInternal::staysPure(0x7ff4000000000000ULL));
static_assert(PureNaNInternal::staysPure(0x7ff8000000000000ULL));
static_assert(PureNaNInternal::staysPure(0x7ffbffffffffffffULL));
static_assert(PureNaNInternal::staysPure(0x7ffc000000000000ULL));
static_assert(PureNaNInternal::staysPure(0x7ffe000000000000ULL));
static_assert(PureNaNInternal::staysPure(0x7fffffffffffffffULL));
static_assert(PureNaNInternal::staysPure(0xfff0000000000001ULL));
static_assert(PureNaNInternal::staysPure(0xfff4000000000000ULL));
static_assert(PureNaNInternal::staysPure(0xfff8000000000000ULL));
static_assert(PureNaNInternal::staysPure(0xfffbffffffffffffULL));
static_assert(PureNaNInternal::staysPure(0xfffc000000000000ULL));
static_assert(PureNaNInternal::staysPure(0xfffe000000000000ULL));
static_assert(PureNaNInternal::staysPure(0xffffffffffffffffULL));
static_assert(std::bit_cast<uint64_t>(purifyNaNKeepingPayload(std::bit_cast<double>(0x7ff8000000012345ULL))) == 0x7ff8000000012345ULL);
static_assert(std::bit_cast<uint64_t>(purifyNaNKeepingPayload(std::bit_cast<double>(0xfff0000000000001ULL))) == 0xfff0000000000001ULL);

} // namespace JSC
