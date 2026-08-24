/*
 * Copyright (C) 2017 Caio Lima <ticaiolima@gmail.com>
 * Copyright (C) 2017-2024 Apple Inc. All rights reserved.
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
 *
 * Parts of the implementation below:
 *
 * Copyright 2017-2021 the V8 project authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 *
 * Copyright (c) 2014 the Dart project authors.  Please see the AUTHORS file [1]
 * for details. All rights reserved. Use of this source code is governed by a
 * BSD-style license that can be found in the LICENSE file [2].
 *
 * [1] https://github.com/dart-lang/sdk/blob/master/AUTHORS
 * [2] https://github.com/dart-lang/sdk/blob/master/LICENSE
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file [3].
 *
 * [3] https://golang.org/LICENSE
 */

#include "config.h"
#include "JSBigInt.h"
#include "JSCJSValueBigInt.h"

#include "BigIntObject.h"
#include "JSCJSValueInlines.h"
#include "JSObjectInlines.h"
#include "MathCommon.h"
#include "ParseInt.h"
#include "StructureCreateInlines.h"
#include <algorithm>
#include <wtf/Hasher.h>
#include <wtf/Int128.h>
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

const ClassInfo JSBigInt::s_info = { "BigInt"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(JSBigInt) };

JSBigInt::JSBigInt(VM& vm, Structure* structure, unsigned length)
    : Base(vm, structure)
    , m_length(length)
{ }

void JSBigInt::initialize(InitializationType initType)
{
    if (initType == InitializationType::WithZero)
        zeroSpan(digits());
}

Structure* JSBigInt::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(HeapBigIntType, StructureFlags), info());
}

inline JSBigInt* JSBigInt::createZero(VM& vm)
{
    JSBigInt* cached = vm.heapBigIntConstantZero.get();
    ASSERT(cached);
    return cached;
}

JSBigInt* JSBigInt::tryCreateZero(VM& vm)
{
    JSBigInt* cached = vm.heapBigIntConstantZero.get();
    ASSERT(cached);
    return cached;
}

inline JSBigInt* JSBigInt::createWithLength(JSGlobalObject* nullOrGlobalObjectForOOM, VM& vm, unsigned length)
{
    if (length > maxLength) [[unlikely]] {
        if (nullOrGlobalObjectForOOM) {
            auto scope = DECLARE_THROW_SCOPE(vm);
            throwOutOfMemoryError(nullOrGlobalObjectForOOM, scope, "BigInt generated from this operation is too big"_s);
        }
        return nullptr;
    }

    ASSERT(length <= maxLength);
    auto* cell = tryAllocateCell<JSBigInt>(vm, JSBigInt::allocationSize(length));
    if (!cell) [[unlikely]] {
        if (nullOrGlobalObjectForOOM) {
            auto scope = DECLARE_THROW_SCOPE(vm);
            throwOutOfMemoryError(nullOrGlobalObjectForOOM, scope);
        }
        return nullptr;
    }

    JSBigInt* bigInt = new (NotNull, cell) JSBigInt(vm, vm.bigIntStructure.get(), length);
    bigInt->finishCreation(vm);
    return bigInt;
}

JSBigInt* JSBigInt::tryCreateWithLength(VM& vm, unsigned length)
{
    return createWithLength(nullptr, vm, length);
}

JSBigInt* JSBigInt::createWithLength(JSGlobalObject* globalObject, unsigned length)
{
    return createWithLength(globalObject, globalObject->vm(), length);
}

inline JSBigInt* JSBigInt::createFrom(JSGlobalObject* nullOrGlobalObjectForOOM, VM& vm, int32_t value)
{
    if (!value)
        return createZero(vm);

    JSBigInt* bigInt = createWithLength(nullOrGlobalObjectForOOM, vm, 1);
    if (!bigInt) [[unlikely]]
        return nullptr;

    if (value < 0) {
        bigInt->setDigit(0, static_cast<Digit>(-1 * static_cast<int64_t>(value)));
        bigInt->setSign(true);
    } else
        bigInt->setDigit(0, static_cast<Digit>(value));

    return bigInt;
}

JSBigInt* JSBigInt::createFrom(JSGlobalObject* globalObject, int32_t value)
{
    return createFrom(globalObject, globalObject->vm(), value);
}

JSBigInt* JSBigInt::tryCreateFrom(VM& vm, int32_t value)
{
    return createFrom(nullptr, vm, value);
}

JSBigInt* JSBigInt::createFrom(JSGlobalObject* globalObject, uint32_t value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!value)
        RELEASE_AND_RETURN(scope, createZero(vm));
    
    JSBigInt* bigInt = createWithLength(globalObject, 1);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bigInt->setDigit(0, static_cast<Digit>(value));
    return bigInt;
}

inline JSBigInt* JSBigInt::tryCreateFromImpl(JSGlobalObject* globalObject, uint64_t value, bool sign)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!value)
        RELEASE_AND_RETURN(scope, createZero(vm));

    if (sizeof(Digit) == 8 || value <= UINT32_MAX) {
        JSBigInt* bigInt = createWithLength(globalObject, 1);
        RETURN_IF_EXCEPTION(scope, nullptr);
        bigInt->setDigit(0, static_cast<Digit>(value));
        bigInt->setSign(sign);
        return bigInt;
    }

    ASSERT(sizeof(Digit) == 4);
    JSBigInt* bigInt = createWithLength(globalObject, 2);
    RETURN_IF_EXCEPTION(scope, nullptr);
    Digit lowBits  = static_cast<Digit>(value & 0xffffffff);
    Digit highBits = static_cast<Digit>((value >> 32) & 0xffffffff);

    ASSERT(highBits);

    bigInt->setDigit(0, lowBits);
    bigInt->setDigit(1, highBits);
    bigInt->setSign(sign);

    return bigInt;
}

JSBigInt* JSBigInt::createFrom(JSGlobalObject* globalObject, uint64_t value)
{
    return tryCreateFromImpl(globalObject, value, false);
}

JSBigInt* JSBigInt::createFrom(JSGlobalObject* globalObject, int64_t value)
{
    uint64_t unsignedValue;
    bool sign = false;
    if (value < 0) {
        unsignedValue = static_cast<uint64_t>(-(value + 1)) + 1;
        sign = true;
    } else
        unsignedValue = value;
    return tryCreateFromImpl(globalObject, unsignedValue, sign);
}

JSBigInt* JSBigInt::createFrom(JSGlobalObject* globalObject, Int128 value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!value)
        RELEASE_AND_RETURN(scope, createZero(vm));

    UInt128 unsignedValue;
    bool sign = false;
    if (value < 0) {
        unsignedValue = static_cast<UInt128>(-(value + 1)) + 1;
        sign = true;
    } else
        unsignedValue = value;

    if (unsignedValue <= UINT64_MAX)
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, static_cast<uint64_t>(unsignedValue), sign));

    if constexpr (sizeof(Digit) == 8) {
        JSBigInt* bigInt = createWithLength(globalObject, 2);
        RETURN_IF_EXCEPTION(scope, nullptr);

        Digit lowBits = static_cast<Digit>(static_cast<uint64_t>(unsignedValue));
        Digit highBits = static_cast<Digit>(static_cast<uint64_t>(unsignedValue >> 64));

        ASSERT(highBits);

        bigInt->setDigit(0, lowBits);
        bigInt->setDigit(1, highBits);
        bigInt->setSign(sign);
        return bigInt;
    }

    ASSERT(sizeof(Digit) == 4);

    Digit digit0 = static_cast<Digit>(static_cast<uint64_t>(unsignedValue));
    Digit digit1 = static_cast<Digit>(static_cast<uint64_t>(unsignedValue >> 32));
    Digit digit2 = static_cast<Digit>(static_cast<uint64_t>(unsignedValue >> 64));
    Digit digit3 = static_cast<Digit>(static_cast<uint64_t>(unsignedValue >> 96));

    ASSERT(digit2 || digit3);

    int length = digit3 ? 4 : 3;
    JSBigInt* bigInt = createWithLength(globalObject, length);
    RETURN_IF_EXCEPTION(scope, nullptr);

    bigInt->setDigit(0, digit0);
    bigInt->setDigit(1, digit1);
    bigInt->setDigit(2, digit2);
    if (digit3)
        bigInt->setDigit(3, digit3);
    bigInt->setSign(sign);
    return bigInt;
}

JSBigInt* JSBigInt::createFrom(JSGlobalObject* globalObject, bool value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!value)
        RELEASE_AND_RETURN(scope, createZero(vm));

    JSBigInt* bigInt = createWithLength(globalObject, 1);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bigInt->setDigit(0, static_cast<Digit>(value));
    return bigInt;
}

JSBigInt* JSBigInt::createFrom(JSGlobalObject* globalObject, double value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(isInteger(value));
    if (!value)
        RELEASE_AND_RETURN(scope, createZero(vm));

    bool sign = value < 0; // -0 was already handled above.
    uint64_t doubleBits = std::bit_cast<uint64_t>(value);
    int32_t rawExponent = static_cast<int32_t>(doubleBits >> doublePhysicalMantissaSize) & 0x7ff;
    ASSERT(rawExponent != 0x7ff); // Since value is integer, exponent should not be 0x7ff (full bits, used for infinity etc.).
    ASSERT(rawExponent >= 0x3ff); // Since value is integer, exponent should be >= 0 + bias (0x3ff).
    int32_t exponent = rawExponent - 0x3ff;
    int32_t digits = exponent / digitBits + 1;
    Vector<Digit, 64> resultVector(FillWith { }, digits, 0);
    auto result = resultVector.mutableSpan();

    // We construct a BigInt from the double value by shifting its mantissa
    // according to its exponent and mapping the bit pattern onto digits.
    //
    //               <----------- bitlength = exponent + 1 ----------->
    //                <----- 52 ------> <------ trailing zeroes ------>
    // mantissa:     1yyyyyyyyyyyyyyyyy 0000000000000000000000000000000
    // digits:    0001xxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
    //                <-->          <------>
    //           msdTopBit         digitBits
    //

    uint64_t mantissa = (doubleBits & doublePhysicalMantissaMask) | doubleMantissaHiddenBit;

    int32_t mantissaTopBit = doubleMantissaSize - 1; // 0-indexed.
    // 0-indexed position of most significant bit in the most significant digit.
    int32_t msdTopBit = exponent % digitBits;
    // Number of unused bits in mantissa. We'll keep them shifted to the
    // left (i.e. most significant part) of the underlying uint64_t.
    int32_t remainingMantissaBits = 0;
    // Next digit under construction.
    Digit digit = 0;

    // First, build the MSD by shifting the mantissa appropriately.
    if (msdTopBit < mantissaTopBit) {
        remainingMantissaBits = mantissaTopBit - msdTopBit;
        digit = static_cast<Digit>(mantissa >> remainingMantissaBits);
        mantissa = mantissa << (64 - remainingMantissaBits);
    } else {
        ASSERT(msdTopBit >= mantissaTopBit);
        digit = static_cast<Digit>(mantissa << (msdTopBit - mantissaTopBit));
        mantissa = 0;
    }
    result[digits - 1] = digit;
    // Then fill in the rest of the digits.
    for (int32_t digitIndex = digits - 2; digitIndex >= 0; digitIndex--) {
        if (remainingMantissaBits > 0) {
            remainingMantissaBits -= digitBits;
            if constexpr (sizeof(Digit) == 4) {
                digit = mantissa >> 32;
                mantissa = mantissa << 32;
            } else {
                ASSERT(sizeof(Digit) == 8);
                digit = mantissa;
                mantissa = 0;
            }
        } else
            digit = 0;
        result[digitIndex] = digit;
    }
    RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, sign, result));
}


JSBigInt* JSBigInt::tryCreateFromWords(VM& vm, std::span<const uint64_t> words, bool sign)
{
    // Trim leading zeroes
    size_t wordCount = words.size();
    while (wordCount > 0 && words[wordCount - 1] == 0) {
        wordCount--;
    }

    if (wordCount == 0) {
        return tryCreateZero(vm);
    }

    // Check size limit
    if (wordCount > maxLength) [[unlikely]] {
        return nullptr;
    }

    JSBigInt* bigInt = tryCreateWithLength(vm, wordCount);
    if (!bigInt) [[unlikely]]
        return nullptr;

    bigInt->setSign(sign);

    // Fast path: use memcpy to copy all words at once
    // No need to initialize to zero first since we're copying all the data
    memcpy(bigInt->dataStorage(), words.data(), wordCount * sizeof(uint64_t));

    return bigInt;
}

JSBigInt* JSBigInt::createFromWords(JSGlobalObject* globalObject, std::span<const uint64_t> words, bool sign)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSBigInt* result = tryCreateFromWords(vm, words, sign);
    if (!result) {
        throwOutOfMemoryError(globalObject, scope, "BigInt generated from this operation is too big"_s);
        return nullptr;
    }

    RELEASE_AND_RETURN(scope, result);
}

size_t JSBigInt::toWordsArray(std::span<uint64_t> words)
{
    size_t copyCount = std::min(words.size(), static_cast<size_t>(length()));
    if (copyCount > 0) {
        // Fast path: use memcpy to copy all words at once
        memcpy(words.data(), dataStorage(), copyCount * sizeof(uint64_t));
    }
    return copyCount;
}

JSValue JSBigInt::toPrimitive(JSGlobalObject*, PreferredPrimitiveType) const
{
    return const_cast<JSBigInt*>(this);
}

unsigned JSBigInt::bitLength() const
{
    if (isZero())
        return 1;
    return m_length * digitBits - clz(digit(m_length - 1));
}

JSValue JSBigInt::parseInt(JSGlobalObject* globalObject, StringView s, ErrorParseMode parserMode)
{
    if (s.is8Bit())
        return parseInt(globalObject, s.span8(), parserMode);
    return parseInt(globalObject, s.span16(), parserMode);
}

JSValue JSBigInt::parseInt(JSGlobalObject* nullOrGlobalObjectForOOM, VM& vm, StringView s, uint8_t radix, ErrorParseMode parserMode, ParseIntSign sign)
{
    if (s.is8Bit())
        return parseInt(nullOrGlobalObjectForOOM, vm, s.span8(), 0, radix, parserMode, sign, ParseIntMode::DisallowEmptyString);
    return parseInt(nullOrGlobalObjectForOOM, vm, s.span16(), 0, radix, parserMode, sign, ParseIntMode::DisallowEmptyString);
}

JSValue JSBigInt::stringToBigInt(JSGlobalObject* globalObject, StringView s)
{
    return parseInt(globalObject, s, ErrorParseMode::IgnoreExceptions);
}

String JSBigInt::toString(JSGlobalObject* globalObject, unsigned radix)
{
    if (this->isZero())
        return globalObject->vm().smallStrings.singleCharacterStringRep('0');

    if (hasOneBitSet(radix))
        return toStringBasePowerOfTwo(globalObject->vm(), globalObject, this, radix);

    return toStringGeneric(globalObject->vm(), globalObject, this, radix);
}

String JSBigInt::tryGetString(VM& vm, JSBigInt* bigInt, unsigned radix)
{
    if (bigInt->isZero())
        return vm.smallStrings.singleCharacterStringRep('0');

    if (hasOneBitSet(radix))
        return toStringBasePowerOfTwo(vm, nullptr, bigInt, radix);

    return toStringGeneric(vm, nullptr, bigInt, radix);
}

class HeapBigIntImpl {
public:
    explicit HeapBigIntImpl(JSBigInt* bigInt)
        : m_bigInt(bigInt)
    { }

    ALWAYS_INLINE bool isZero() { return m_bigInt->isZero(); }
    ALWAYS_INLINE bool sign() { return m_bigInt->sign(); }
    ALWAYS_INLINE unsigned length() { return m_bigInt->length(); }
    ALWAYS_INLINE JSBigInt::Digit digit(unsigned i) { return m_bigInt->digit(i); }
    ALWAYS_INLINE std::span<const JSBigInt::Digit> digits() { return { m_bigInt->dataStorage(), m_bigInt->length() }; }
    ALWAYS_INLINE JSBigInt* toHeapBigInt(JSGlobalObject*, VM&) { return m_bigInt; }
    ALWAYS_INLINE JSBigInt* toHeapBigInt(JSGlobalObject*) { return m_bigInt; }

private:
    friend struct JSBigInt::ImplResult;
    JSBigInt* m_bigInt;
};

template<typename D>
static std::span<D> NODELETE normalize(std::span<D> x)
{
    while (!x.empty() && !x.back())
        x = x.first(x.size() - 1);
    return x;
}

class Int32BigIntImpl {
public:
    explicit Int32BigIntImpl(int32_t value)
        : m_value(value)
    {
        if (!isZero())
            m_digit = digit(0);
    }

    ALWAYS_INLINE bool isZero() { return !m_value; }
    ALWAYS_INLINE bool sign() { return m_value < 0; }
    ALWAYS_INLINE unsigned length() { return isZero() ? 0 : 1; }
    ALWAYS_INLINE JSBigInt::Digit digit(unsigned i)
    {
        ASSERT(length());
        ASSERT_UNUSED(i, i == 0);
        if (sign())
            return static_cast<JSBigInt::Digit>(WTF::negate(static_cast<int64_t>(m_value)));
        return m_value;
    }

    ALWAYS_INLINE std::span<const JSBigInt::Digit> digits() { return { &m_digit, length() }; }

    ALWAYS_INLINE JSBigInt* toHeapBigInt(JSGlobalObject* nullOrGlobalObjectForOOM, VM& vm)
    {
        return JSBigInt::createFrom(nullOrGlobalObjectForOOM, vm, m_value);
    }

    ALWAYS_INLINE JSBigInt* toHeapBigInt(JSGlobalObject* globalObject)
    {
        return JSBigInt::createFrom(globalObject, m_value);
    }

private:
    friend struct JSBigInt::ImplResult;
    int32_t m_value;
    JSBigInt::Digit m_digit { };
};

class Int64BigIntImpl {
public:
    static constexpr unsigned numDigits = isRegister64Bit() ? 1 : 2;

    explicit Int64BigIntImpl(int64_t value)
        : m_value(value)
        , m_sign(value < 0)
    {
#if CPU(REGISTER64)
        if (!isZero())
            m_digits[0] = digit(0);
#else
        for (unsigned i = 0; i < length(); ++i)
            m_digits[i] = digit(i);
#endif
    }

    explicit Int64BigIntImpl(uint64_t value)
        : m_value(value)
        , m_sign(false)
    {
#if CPU(REGISTER64)
        if (!isZero())
            m_digits[0] = digit(0);
#else
        for (unsigned i = 0; i < length(); ++i)
            m_digits[i] = digit(i);
#endif
    }

    ALWAYS_INLINE bool isZero() { return !m_value; }
    ALWAYS_INLINE bool sign() { return m_sign; }
    ALWAYS_INLINE unsigned length() { return isZero() ? 0 : numDigits; }
    ALWAYS_INLINE JSBigInt::Digit digit(unsigned i)
    {
        ASSERT_UNUSED(i, i < length());
#if CPU(REGISTER64)
        if (sign())
            return static_cast<JSBigInt::Digit>(WTF::negate(static_cast<int64_t>(m_value)));
        return m_value;
#else
        static_assert(sizeof(JSBigInt::Digit) == 4);
        if (sign())
            return static_cast<JSBigInt::Digit>(WTF::negate(static_cast<int64_t>(m_value)) >> (32 * i));
        return static_cast<JSBigInt::Digit>(m_value >> (32 * i));
#endif
    }

    ALWAYS_INLINE std::span<const JSBigInt::Digit> digits() { return { m_digits, length() }; }

private:
    friend struct JSBigInt::ImplResult;
    uint64_t m_value;
    JSBigInt::Digit m_digits[numDigits] { };
    bool m_sign;
};

ALWAYS_INLINE JSBigInt::ImplResult::ImplResult(HeapBigIntImpl& heapImpl)
    : payload(heapImpl.m_bigInt)
{ }

ALWAYS_INLINE JSBigInt::ImplResult::ImplResult(JSBigInt* heapBigInt)
    : payload(heapBigInt)
{ }

#if USE(BIGINT32)
ALWAYS_INLINE JSBigInt::ImplResult::ImplResult(Int32BigIntImpl& int32Impl)
    : payload(jsBigInt32(int32Impl.m_value))
{ }
#endif

ALWAYS_INLINE JSBigInt::ImplResult::ImplResult(JSValue value)
    : payload(value)
{ }

static ALWAYS_INLINE JSValue NODELETE tryConvertToBigInt32(JSBigInt::ImplResult implResult)
{
    if (!implResult.payload)
        return JSValue();
    if (implResult.payload.isBigInt32())
        return implResult.payload;
    return tryConvertToBigInt32(implResult.payload.asHeapBigInt());
}

static ALWAYS_INLINE JSBigInt::ImplResult zeroImpl(VM& vm)
{
#if USE(BIGINT32)
    UNUSED_PARAM(vm);
    return jsBigInt32(0);
#else
    return vm.heapBigIntConstantZero.get();
#endif
}

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::exponentiateImpl(JSGlobalObject* globalObject, BigIntImpl1 base, BigIntImpl2 exponent)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (exponent.sign()) {
        throwRangeError(globalObject, scope, "Negative exponent is not allowed"_s);
        return nullptr;
    }

    // 2. If base is 0n and exponent is 0n, return 1n.
    if (exponent.isZero())
        RELEASE_AND_RETURN(scope, JSBigInt::createFrom(globalObject, 1));
    
    // 3. Return a BigInt representing the mathematical value of base raised
    //    to the power exponent.
    if (base.isZero())
        return { base };

    if (base.length() == 1 && base.digit(0) == 1) {
        // (-1) ** even_number == 1.
        if (base.sign() && !(exponent.digit(0) & 1))
            RELEASE_AND_RETURN(scope, JSBigInt::unaryMinusImpl(globalObject, base));

        // (-1) ** odd_number == -1; 1 ** anything == 1.
        return { base };
    }

    // For all bases >= 2, very large exponents would lead to unrepresentable
    // results.
    static_assert(maxLengthBits < std::numeric_limits<Digit>::max(), "maxLengthBits needs to be less than digit::max()");
    if (exponent.length() > 1) {
        throwOutOfMemoryError(globalObject, scope, "BigInt generated from this operation is too big"_s);
        return nullptr;
    }

    Digit expValue = exponent.digit(0);
    if (expValue == 1)
        return { base };
    if (expValue >= maxLengthBits) {
        throwOutOfMemoryError(globalObject, scope, "BigInt generated from this operation is too big"_s);
        return nullptr;
    }

    static_assert(maxLengthBits <= maxInt, "maxLengthBits needs to be <= maxInt");
    int n = static_cast<int>(expValue);
    if (base.length() == 1 && base.digit(0) == 2) {
        // Fast path for 2^n.
        int neededDigits = 1 + (n / digitBits);

        Vector<Digit, 16> resultVector(FillWith { }, neededDigits, 0);
        auto result = resultVector.mutableSpan();

        // All bits are zero. Now set the n-th bit.
        Digit msd = static_cast<Digit>(1) << (n % digitBits);
        result[neededDigits - 1] = msd;

        // Result is negative for odd powers of -2n.
        bool sign = false;
        if (base.sign()) 
            sign = static_cast<bool>(n & 1);
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, sign, result));
    }

    JSBigInt* result = nullptr;
    JSBigInt* runningSquare = base.toHeapBigInt(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);

    // This implicitly sets the result's sign correctly.
    if (n & 1) {
        result = base.toHeapBigInt(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    n >>= 1;
    for (; n; n >>= 1) {
        ImplResult temp = JSBigInt::multiplyImpl(globalObject, HeapBigIntImpl { runningSquare }, HeapBigIntImpl { runningSquare });
        RETURN_IF_EXCEPTION(scope, nullptr);
        ASSERT(temp.payload);
        ASSERT(temp.payload.isHeapBigInt());
        JSBigInt* maybeResult = temp.payload.asHeapBigInt();
        runningSquare = maybeResult;
        if (n & 1) {
            if (!result)
                result = runningSquare;
            else {
                temp = JSBigInt::multiplyImpl(globalObject, HeapBigIntImpl { result }, HeapBigIntImpl { runningSquare });
                RETURN_IF_EXCEPTION(scope, nullptr);
                ASSERT(temp.payload);
                ASSERT(temp.payload.isHeapBigInt());
                maybeResult = temp.payload.asHeapBigInt();
                result = maybeResult;
            }
        }
    }

    return { result };
}

JSValue JSBigInt::exponentiate(JSGlobalObject* globalObject, JSBigInt* base, JSBigInt* exponent)
{
    return tryConvertToBigInt32(exponentiateImpl(globalObject, HeapBigIntImpl { base }, HeapBigIntImpl { exponent }));
}

#if USE(BIGINT32)
JSValue JSBigInt::exponentiate(JSGlobalObject* globalObject, JSBigInt* base, int32_t exponent)
{
    return tryConvertToBigInt32(exponentiateImpl(globalObject, HeapBigIntImpl { base }, Int32BigIntImpl { exponent }));
}

JSValue JSBigInt::exponentiate(JSGlobalObject* globalObject, int32_t base, JSBigInt* exponent)
{
    return tryConvertToBigInt32(exponentiateImpl(globalObject, Int32BigIntImpl { base }, HeapBigIntImpl { exponent }));
}

JSValue JSBigInt::exponentiate(JSGlobalObject* globalObject, int32_t base, int32_t exponent)
{
    return tryConvertToBigInt32(exponentiateImpl(globalObject, Int32BigIntImpl { base }, Int32BigIntImpl { exponent }));
}
#endif

#if CPU(REGISTER64)
using TwoDigit = UInt128;
#else
using TwoDigit = uint64_t;
#endif

// Where a column's carry lives between the three accumulator digits. Keeping it in the condition
// flags is the shortest instruction sequence, but there is only one flag register, so a scheduler
// interleaving several accumulations has to save and restore it. Materializing the carry as a value
// costs an instruction per term and lets those accumulations overlap freely.
enum class CarryForm : uint8_t { Flags, Value };

template<CarryForm carryForm>
class DigitColumnAccumulator {
    using Digit = JSBigInt::Digit;
public:
    ALWAYS_INLINE void mac(Digit a, Digit b)
    {
        TwoDigit prod = static_cast<TwoDigit>(a) * b;
        if constexpr (carryForm == CarryForm::Value) {
            Digit carry = 0;
            t0 = addCarrying(t0, static_cast<Digit>(prod), 0, carry);
            t1 = addCarrying(t1, static_cast<Digit>(prod >> JSBigInt::digitBits), carry, carry);
            t2 += carry;
            return;
        }
        TwoDigit sum0 = static_cast<TwoDigit>(t0) + static_cast<Digit>(prod);
        t0 = static_cast<Digit>(sum0);
        TwoDigit sum1 = static_cast<TwoDigit>(t1) + static_cast<Digit>(prod >> JSBigInt::digitBits) + static_cast<Digit>(sum0 >> JSBigInt::digitBits);
        t1 = static_cast<Digit>(sum1);
        t2 += static_cast<Digit>(sum1 >> JSBigInt::digitBits);
    }

    // Accumulates 2 * a * b. The doubled product needs one bit more than the two digits prod
    // occupies, and that bit lands in t2, the digit above the pair.
    ALWAYS_INLINE void macDoubled(Digit a, Digit b)
    {
        TwoDigit prod = static_cast<TwoDigit>(a) * b;
        Digit high = static_cast<Digit>(prod >> (JSBigInt::digitBits * 2 - 1));
        TwoDigit doubled = prod << 1;
        if constexpr (carryForm == CarryForm::Value) {
            Digit carry = 0;
            t0 = addCarrying(t0, static_cast<Digit>(doubled), 0, carry);
            t1 = addCarrying(t1, static_cast<Digit>(doubled >> JSBigInt::digitBits), carry, carry);
            t2 += carry + high;
            return;
        }
        TwoDigit sum0 = static_cast<TwoDigit>(t0) + static_cast<Digit>(doubled);
        t0 = static_cast<Digit>(sum0);
        TwoDigit sum1 = static_cast<TwoDigit>(t1) + static_cast<Digit>(doubled >> JSBigInt::digitBits) + static_cast<Digit>(sum0 >> JSBigInt::digitBits);
        t1 = static_cast<Digit>(sum1);
        t2 += static_cast<Digit>(sum1 >> JSBigInt::digitBits) + high;
    }

    ALWAYS_INLINE Digit storeAndShift()
    {
        Digit result = t0;
        t0 = t1;
        t1 = t2;
        t2 = 0;
        return result;
    }

    ALWAYS_INLINE Digit low() const { return t0; }

    // True once the running sum fits in the single digit low() returns, which is what callers rely
    // on at the final column.
    ALWAYS_INLINE bool fitsInLow() const { return !t1 && !t2; }

private:
    ALWAYS_INLINE static Digit addCarrying(Digit a, Digit b, Digit carryIn, Digit& carryOut)
    {
#if COMPILER(GCC) && GCC_VERSION < 150000
        Digit sum = 0;
        bool carry0 = __builtin_add_overflow(a, b, &sum);
        Digit result = 0;
        bool carry1 = __builtin_add_overflow(sum, carryIn, &result);
        carryOut = static_cast<Digit>(carry0 | carry1);
        return result;
#else
        if constexpr (sizeof(Digit) == sizeof(unsigned long long)) {
            unsigned long long out = 0;
            Digit result = __builtin_addcll(a, b, carryIn, &out);
            carryOut = static_cast<Digit>(out);
            return result;
        } else {
            unsigned out = 0;
            Digit result = __builtin_addc(a, b, carryIn, &out);
            carryOut = static_cast<Digit>(out);
            return result;
        }
#endif
    }

    Digit t0 { 0 };
    Digit t1 { 0 };
    Digit t2 { 0 };
};

template<size_t N>
class CombaAccumulator {
    using Digit = JSBigInt::Digit;
public:
    template<size_t K, size_t I = 0>
    ALWAYS_INLINE void computeColumn(std::span<const Digit, N> a, std::span<const Digit, N> b)
    {
        if constexpr (I < N) {
            constexpr int J = static_cast<int>(K) - static_cast<int>(I);
            if constexpr (J >= 0 && J < static_cast<int>(N))
                m_accumulator.mac(a[I], b[J]);
            computeColumn<K, I + 1>(a, b);
        }
    }

    template<size_t K = 0>
    ALWAYS_INLINE void pass(std::span<Digit, N * 2> r, std::span<const Digit, N> a, std::span<const Digit, N> b)
    {
        if constexpr (K < 2 * N - 1) {
            computeColumn<K>(a, b);
            r[K] = m_accumulator.storeAndShift();
            pass<K + 1>(r, a, b);
        } else {
            ASSERT(m_accumulator.fitsInLow());
            r[N * 2 - 1] = m_accumulator.low();
        }
    }

    // Column K of a * a. The pairs (I, J) and (J, I) contribute the same product, so it is
    // accumulated once at twice the weight, halving the digit multiplies to N * (N + 1) / 2.
    template<size_t K, size_t I = 0>
    ALWAYS_INLINE void computeSquareColumn(std::span<const Digit, N> a)
    {
        if constexpr (I < N) {
            constexpr int J = static_cast<int>(K) - static_cast<int>(I);
            if constexpr (J >= static_cast<int>(I) && J < static_cast<int>(N)) {
                if constexpr (J == static_cast<int>(I))
                    m_accumulator.mac(a[I], a[I]);
                else
                    m_accumulator.macDoubled(a[I], a[J]);
            }
            computeSquareColumn<K, I + 1>(a);
        }
    }

    template<size_t K = 0>
    ALWAYS_INLINE void squarePass(std::span<Digit, N * 2> r, std::span<const Digit, N> a)
    {
        if constexpr (K < 2 * N - 1) {
            computeSquareColumn<K>(a);
            r[K] = m_accumulator.storeAndShift();
            squarePass<K + 1>(r, a);
        } else {
            ASSERT(m_accumulator.fitsInLow());
            r[N * 2 - 1] = m_accumulator.low();
        }
    }

private:
    DigitColumnAccumulator<CarryForm::Value> m_accumulator;
};

template<size_t N>
std::span<JSBigInt::Digit, N * 2> JSBigInt::multiplyCombaFixed(std::span<const Digit, N> x, std::span<const Digit, N> y, std::span<Digit, N * 2> result)
{
    static_assert(N == 1 || N == 2 || N == 4 || N == 8 || N == maxCombaFixedSize);
    std::array<Digit, N> a;
    std::array<Digit, N> b;

    // Ensure that all loads are done before entering to computation. This allows compiler to use registers for all elements.
    for (size_t i = 0; i < N; ++i)
        a[i] = x[i];
    for (size_t i = 0; i < N; ++i)
        b[i] = y[i];

    CombaAccumulator<N> acc;
    acc.template pass<>(result, a, b);
    return result;
}

template<size_t N>
std::span<JSBigInt::Digit, N * 2> JSBigInt::squareCombaFixed(std::span<const Digit, N> x, std::span<Digit, N * 2> result)
{
    static_assert(N == 1 || N == 2 || N == 4 || N == 8 || N == maxCombaFixedSize);
    std::array<Digit, N> a;

    // Ensure that all loads are done before entering to computation. This allows compiler to use registers for all elements.
    for (size_t i = 0; i < N; ++i)
        a[i] = x[i];

    CombaAccumulator<N> acc;
    acc.template squarePass<>(result, a);
    return result;
}

std::span<JSBigInt::Digit> JSBigInt::multiplySingle(std::span<const Digit> multiplicand, Digit multiplier, std::span<Digit> result)
{
    RELEASE_ASSERT(result.size() > multiplicand.size());
    Digit carry = 0;
    Digit high = 0;
    size_t i = 0;
    for (; i < multiplicand.size(); ++i) {
        auto [low, newHigh] = digitMul(multiplicand[i], multiplier);
        Digit newCarry = 0;
        result[i] = digitAdd3(low, high, carry, newCarry);
        high = newHigh;
        carry = newCarry;
    }
    result[i++] = carry + high;
    return result.first(i);
}

// Z := X * Y.
// O(n²) "schoolbook" multiplication algorithm. Optimized to minimize
// bounds and overflow checks: rather than looping over X for every digit
// of Y (or vice versa), we loop over Z. The {BODY} macro above is what
// computes one of Z's digits as a sum of the products of relevant digits
// of X and Y. This yields a nearly 2x improvement compared to more obvious
// implementations.
// This method is *highly* performance sensitive even for the advanced
// algorithms, which use this as the base case of their recursive calls.
#define MULTIPLY_BODY(min, max) \
    do { \
        for (uint32_t j = min; j <= max; j++) { \
            auto [low, high] = digitMul(x[j], y[i - j]); \
            zi = digitAdd(zi, low, carry); \
            next = digitAdd(next, high, nextCarry); \
        } \
        result[i] = zi; \
    } while (0)

std::span<JSBigInt::Digit> JSBigInt::multiplySchoolbook(std::span<const Digit> xSpan, std::span<const Digit> ySpan, std::span<Digit> resultSpan)
{
    RELEASE_ASSERT(xSpan.size() >= ySpan.size());
    RELEASE_ASSERT(resultSpan.size() >= xSpan.size() + ySpan.size());
    RELEASE_ASSERT(xSpan.size());
    RELEASE_ASSERT(ySpan.size());

    const auto* x = xSpan.data();
    const auto* y = ySpan.data();
    auto* result = resultSpan.data();

    Digit next = 0, nextCarry = 0, carry = 0;
    // Unrolled first iteration: it's trivial.
    {
        auto [low, high] = digitMul(x[0], y[0]);
        result[0] = low;
        next = high;
    }
    size_t i = 1;
    // Unrolled second iteration: a little less setup.
    if (i < ySpan.size()) {
        Digit zi = next;
        next = 0;
        MULTIPLY_BODY(0, 1);
        i++;
    }

    // Main part: since xSpan.size() >= ySpan.size() > i, no bounds checks are needed.
    for (; i < ySpan.size(); i++) {
        Digit temp = 0;
        Digit zi = digitAdd(next, carry, temp);
        next = nextCarry + temp;
        carry = 0;
        nextCarry = 0;
        MULTIPLY_BODY(0, i);
    }

    // Last part: i exceeds y now, we have to be careful about bounds.
    size_t loopEnd = xSpan.size() + ySpan.size() - 2;
    for (; i <= loopEnd; i++) {
        size_t maxXIndex = std::min<size_t>(i, xSpan.size() - 1);
        size_t maxYIndex = ySpan.size() - 1;
        size_t minXIndex = i - maxYIndex;
        Digit temp = 0;
        Digit zi = digitAdd(next, carry, temp);
        next = nextCarry + temp;
        carry = 0;
        nextCarry = 0;
        MULTIPLY_BODY(minXIndex, maxXIndex);
    }

    // Write the last digit.
    Digit temp = 0;
    result[i++] = digitAdd(next, carry, temp);
    ASSERT(!temp);
    return resultSpan.first(i);
}

#undef MULTIPLY_BODY

// For the needs of cachedMod, computes only the low result.size() digits of X * Y.
void JSBigInt::multiplySpecialLow(std::span<const Digit> xSpan, std::span<const Digit> ySpan, std::span<Digit> resultSpan)
{
    RELEASE_ASSERT(ySpan.size() >= 1);
    RELEASE_ASSERT(xSpan.size() >= 2);
    RELEASE_ASSERT(xSpan.size() >= ySpan.size() - 1);
    RELEASE_ASSERT(resultSpan.size());

    const auto* x = xSpan.data();
    const auto* y = ySpan.data();
    auto* result = resultSpan.data();

    DigitColumnAccumulator<CarryForm::Flags> accumulator;
    size_t lastColumn = resultSpan.size() - 1;
    size_t mainEnd = std::min({ xSpan.size(), ySpan.size(), lastColumn });
    size_t column = 0;

    // Expanding phase: both operands still cover the whole column, so the term range is exactly
    // [0, column] and needs no clamping.
    for (; column < mainEnd; ++column) {
        for (size_t j = 0; j <= column; ++j)
            accumulator.mac(x[j], y[column - j]);
        result[column] = accumulator.storeAndShift();
    }

    // Shrinking phase: the term range is clipped at both ends.
    for (; column <= lastColumn; ++column) {
        size_t maxYIndex = std::min<size_t>(column, ySpan.size() - 1);
        size_t minXIndex = column - maxYIndex;
        size_t maxXIndex = std::min<size_t>(column, xSpan.size() - 1);
        for (size_t j = minXIndex; j <= maxXIndex; ++j)
            accumulator.mac(x[j], y[column - j]);
        result[column] = accumulator.storeAndShift();
    }
}

// For the needs of cachedMod, computes only product digits from startPosition and onward.
// result[startPosition] corresponds to product digit startPosition.
// The accumulator state from positions below startPosition is lost, so the computed digits are an
// *approximate* value.
void JSBigInt::multiplySpecialHigh(std::span<const Digit> xSpan, std::span<const Digit> ySpan, std::span<Digit> resultSpan, size_t startPosition)
{
    RELEASE_ASSERT(xSpan.size() >= ySpan.size());
    RELEASE_ASSERT(ySpan.size() >= 1);
    size_t fullSize = xSpan.size() + ySpan.size();
    RELEASE_ASSERT(startPosition < fullSize);
    RELEASE_ASSERT(resultSpan.size() >= fullSize);

    const auto* x = xSpan.data();
    const auto* y = ySpan.data();
    auto* result = resultSpan.data();

    DigitColumnAccumulator<CarryForm::Flags> accumulator;
    size_t column = startPosition;

    // Expanding phase: column < ySpan.size(), so the term range starts at 0.
    for (; column < ySpan.size(); ++column) {
        for (size_t j = 0; j <= column; ++j)
            accumulator.mac(x[j], y[column - j]);
        result[column] = accumulator.storeAndShift();
    }

    // Shrinking phase: the term range is clipped at both ends.
    size_t lastColumn = fullSize - 2;
    for (; column <= lastColumn; ++column) {
        size_t minXIndex = column - (ySpan.size() - 1);
        size_t maxXIndex = std::min<size_t>(column, xSpan.size() - 1);
        for (size_t j = minXIndex; j <= maxXIndex; ++j)
            accumulator.mac(x[j], y[column - j]);
        result[column] = accumulator.storeAndShift();
    }

    ASSERT(accumulator.fitsInLow());
    result[column] = accumulator.low();
}

// Product scanning costs less per single-digit product than multiplySchoolbook (one
// add-with-carry chain instead of two counter accumulations) but more per result column (it shifts
// its three-digit running sum). A shape has O(x * y) products and only x + y - 1 columns, so the
// balance is set by the smaller operand: product scanning wins once the columns are long, and again
// on very thin shapes, where multiplySchoolbook's expanding phase all but vanishes and it clamps the
// term range on nearly every column instead.
//
// These bounds are deliberately coarse. Between them the two are within a few percent, and that
// margin is smaller than the swing caused by where multiplyComba's short inner loops happen to land
// relative to a cache line: measuring the same shapes across four builds whose only difference was
// padding ahead of multiplyComba moved individual shapes by up to 6 points and flipped signs. Only
// bounds whose sign held across all four are encoded here, so do not narrow them without
// re-measuring the same way.
static constexpr size_t minCombaSmallerSize = 8;
static constexpr size_t maxCombaThinSmallerSize = 2;
static constexpr size_t minCombaThinLargerSize = 16;
static constexpr bool shouldUseComba(size_t largerSize, size_t smallerSize)
{
    return smallerSize >= minCombaSmallerSize
        || (smallerSize <= maxCombaThinSmallerSize && largerSize >= minCombaThinLargerSize);
}

// Z := X * Y by product scanning. Each column's running sum lives in three digits, so every
// product feeds a single add-with-carry chain instead of materializing a carry bit per addend.
// Splitting the column walk into ramp-up, steady and ramp-down phases makes every loop bound exact,
// so no column pays for clamping its term range.
//
// The shift is more per-column work than multiplySchoolbook does, so this is not a win at every
// shape; see shouldUseComba.
std::span<JSBigInt::Digit> JSBigInt::multiplyComba(std::span<const Digit> xSpan, std::span<const Digit> ySpan, std::span<Digit> resultSpan)
{
    RELEASE_ASSERT(xSpan.size() >= ySpan.size());
    RELEASE_ASSERT(resultSpan.size() >= xSpan.size() + ySpan.size());
    RELEASE_ASSERT(ySpan.size());

    const auto* x = xSpan.data();
    const auto* y = ySpan.data();
    auto* result = resultSpan.data();
    const size_t xSize = xSpan.size();
    const size_t ySize = ySpan.size();

    DigitColumnAccumulator<CarryForm::Flags> accumulator;
    for (size_t i = 0; i < ySize; ++i) {
        for (size_t j = 0; j <= i; ++j)
            accumulator.mac(x[j], y[i - j]);
        result[i] = accumulator.storeAndShift();
    }
    for (size_t i = ySize; i < xSize; ++i) {
        for (size_t j = i - ySize + 1; j <= i; ++j)
            accumulator.mac(x[j], y[i - j]);
        result[i] = accumulator.storeAndShift();
    }
    for (size_t i = xSize; i < xSize + ySize - 1; ++i) {
        for (size_t j = i - ySize + 1; j <= xSize - 1; ++j)
            accumulator.mac(x[j], y[i - j]);
        result[i] = accumulator.storeAndShift();
    }
    ASSERT(accumulator.fitsInLow());
    result[xSize + ySize - 1] = accumulator.low();
    return resultSpan.first(xSize + ySize);
}

// Compile-time-specialized forms of multiplySpecialLow / multiplySpecialHigh for cachedMod. The
// loops below are ordinary loops, but every bound is a compile-time constant, so the index
// arithmetic folds away and the compiler is free to unroll as far as it pays off.
//
// These must accumulate in exactly the same order as the generic versions: multiplySpecialHigh is
// deliberately approximate (it drops the carry coming from columns below StartPosition), and the
// error bound that cachedMod's corrective loop relies on depends on that accumulation order.

// Accumulates x[j] * y[i - j] for j in [min, max] into acc.
ALWAYS_INLINE static void multiplySpecialColumn(const JSBigInt::Digit* x, const JSBigInt::Digit* y, size_t i, size_t min, size_t max, DigitColumnAccumulator<CarryForm::Flags>& acc)
{
    for (size_t j = min; j <= max; ++j)
        acc.mac(x[j], y[i - j]);
}

template<size_t XSize, size_t YSize, size_t StartPosition>
ALWAYS_INLINE void JSBigInt::multiplySpecialHighFixed(std::span<const Digit, XSize> xSpan, std::span<const Digit, YSize> ySpan, std::span<Digit, XSize + YSize> resultSpan)
{
    static_assert(XSize >= YSize && YSize >= 1);
    static_assert(StartPosition < XSize + YSize);
    constexpr size_t loopEnd = XSize + YSize - 2;

    const auto* x = xSpan.data();
    const auto* y = ySpan.data();
    auto* result = resultSpan.data();

    DigitColumnAccumulator<CarryForm::Flags> acc;
    for (size_t i = StartPosition; i <= loopEnd; ++i) {
        size_t minXIndex = i < YSize ? 0 : i - (YSize - 1);
        multiplySpecialColumn(x, y, i, minXIndex, std::min(i, XSize - 1), acc);
        result[i] = acc.storeAndShift();
    }

    ASSERT(acc.fitsInLow());
    result[loopEnd + 1] = acc.low();
}

template<size_t XSize, size_t YSize, size_t RSize>
ALWAYS_INLINE void JSBigInt::multiplySpecialLowFixed(std::span<const Digit, XSize> xSpan, std::span<const Digit, YSize> ySpan, std::span<Digit, RSize> resultSpan)
{
    static_assert(XSize >= 2 && YSize >= 1 && RSize >= 2);
    static_assert(XSize + 1 >= YSize);
    constexpr size_t loopEnd = RSize - 1;
    constexpr size_t mainEnd = std::min({ XSize, YSize, loopEnd });

    const auto* x = xSpan.data();
    const auto* y = ySpan.data();
    auto* result = resultSpan.data();

    DigitColumnAccumulator<CarryForm::Flags> acc;
    size_t i = 0;

    // Expanding phase: both operands still cover the whole column, so the term range is exactly
    // [0, i] and needs no clamping.
    for (; i < mainEnd; ++i) {
        multiplySpecialColumn(x, y, i, 0, i, acc);
        result[i] = acc.storeAndShift();
    }

    // Shrinking phase: the term range is clipped at both ends.
    for (; i <= loopEnd; ++i) {
        size_t maxYIndex = std::min(i, YSize - 1);
        multiplySpecialColumn(x, y, i, i - maxYIndex, std::min(i, XSize - 1), acc);
        result[i] = acc.storeAndShift();
    }
}

// The sub-quadratic algorithms below can run for seconds on the largest inputs, so they service a
// termination request every few million digit multiplications, the way a JIT'ed loop does at its
// back edge. Each unit of work is roughly one CPU multiply; the count only has to be accurate
// enough to check often enough not to appear stuck and rarely enough not to cost anything. Once a
// check has thrown, the algorithms unwind without finishing their buffers, and the caller returns
// the exception. Callers without a global object, which cannot throw, never interrupt.
class JSBigInt::InterruptCheck {
    WTF_MAKE_NONCOPYABLE(InterruptCheck);
public:
    explicit InterruptCheck(VM* vm)
        : m_vm(vm)
    {
    }

    ALWAYS_INLINE void addWork(size_t units)
    {
        m_work += units;
        if (m_work >= workThreshold) [[unlikely]]
            checkSlow();
    }

    ALWAYS_INLINE bool interrupted() const { return m_interrupted; }

private:
    void checkSlow();

    static constexpr size_t workThreshold = 5000000;

    VM* m_vm;
    size_t m_work { 0 };
    bool m_interrupted { false };
};

void JSBigInt::InterruptCheck::checkSlow()
{
    m_work = 0;
    if (!m_vm)
        return;
    // This handles every asynchronous trap the way a RETURN_IF_EXCEPTION does, so a termination
    // request leaves the TerminationException pending.
    if (m_vm->hasExceptionsAfterHandlingTraps()) [[unlikely]]
        m_interrupted = true;
}

// Karatsuba multiplication, ported from V8 [1], which is in turn based on Go's math/big [2].
//
// [1]: https://source.chromium.org/chromium/chromium/src/+/main:v8/src/bigint/mul-karatsuba.cc
// [2]: https://go.dev/src/math/big/nat.go
//
// The threshold is the smaller operand's size, measured against the Comba base case (V8's
// schoolbook crosses over at 34). Balanced shapes win from 40 digits, but for a long x
// karatsubaLength() rounds an odd smaller operand up by a digit, and at 41 or 43 digits that
// padding costs the 2-3% Karatsuba would gain. At 44 no measured shape regresses.
static constexpr size_t karatsubaThreshold = 44;

static size_t karatsubaRoundUpLength(size_t length)
{
    if (length <= 36)
        return roundUpToMultipleOf<2>(length);
    unsigned shift = std::bit_width(length) - 5;
    if ((length >> shift) >= 0x18)
        shift++;
    size_t additive = (static_cast<size_t>(1) << shift) - 1;
    if (shift >= 2 && (length & additive) < (static_cast<size_t>(1) << (shift - 2)))
        return length;
    return ((length + additive) >> shift) << shift;
}

static size_t karatsubaLength(size_t n)
{
    n = karatsubaRoundUpLength(n);
    unsigned i = 0;
    while (n > karatsubaThreshold) {
        n >>= 1;
        i++;
    }
    return n << i;
}

template<typename DigitType>
static std::span<DigitType> clampedSubspan(std::span<DigitType> x, size_t offset, size_t length)
{
    if (offset >= x.size())
        return { };
    return x.subspan(offset, std::min(length, x.size() - offset));
}

JSBigInt::Digit JSBigInt::inplaceAddAndPropagate(std::span<Digit> z, std::span<const Digit> x)
{
    x = normalize(x);
    RELEASE_ASSERT(z.size() >= x.size());
    Digit carry = inplaceAdd(z, x);
    for (size_t i = x.size(); i < z.size() && carry; i++) {
        Digit newCarry = 0;
        z[i] = digitAdd(z[i], carry, newCarry);
        carry = newCarry;
    }
    return carry;
}

JSBigInt::Digit JSBigInt::inplaceSubAndPropagate(std::span<Digit> z, std::span<const Digit> x)
{
    x = normalize(x);
    RELEASE_ASSERT(z.size() >= x.size());
    Digit borrow = inplaceSub(z, x);
    for (size_t i = x.size(); i < z.size() && borrow; i++) {
        Digit newBorrow = 0;
        z[i] = digitSub(z[i], borrow, newBorrow);
        borrow = newBorrow;
    }
    return borrow;
}

void JSBigInt::karatsubaAbsoluteDifference(std::span<Digit> result, std::span<const Digit> x, std::span<const Digit> y, bool& negative)
{
    x = normalize(x);
    y = normalize(y);
    if (compareDigits(x, y) == ComparisonResult::LessThan) {
        negative = !negative;
        std::swap(x, y);
    }
    auto difference = subSchoolbook(x, y, result);
    std::ranges::fill(result.subspan(difference.size()), 0);
}

void JSBigInt::multiplyZeroPadded(InterruptCheck& interrupt, std::span<Digit> result, std::span<const Digit> x, std::span<const Digit> y)
{
    x = normalize(x);
    y = normalize(y);
    if (x.size() < y.size())
        std::swap(x, y);
    if (y.empty()) {
        std::ranges::fill(result, 0);
        return;
    }
    auto product = multiplyDigitsInto(interrupt, x, y, result);
    std::ranges::fill(result.subspan(product.size()), 0);
    // The sub-quadratic algorithms count their work in the base cases they dispatch here, since
    // their own passes over the digits are linear in comparison.
    if (y.size() < karatsubaThreshold)
        interrupt.addWork(x.size() * y.size());
}

void JSBigInt::karatsubaMain(InterruptCheck& interrupt, std::span<Digit> z, std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> scratch, size_t n)
{
    if (n < karatsubaThreshold) {
        multiplyZeroPadded(interrupt, z.first(std::min(z.size(), 2 * n)), x, y);
        return;
    }
    ASSERT(scratch.size() >= 4 * n);
    ASSERT(!(n & 1));
    size_t n2 = n >> 1;
    auto x0 = clampedSubspan(x, 0, n2);
    auto x1 = clampedSubspan(x, n2, n2);
    auto y0 = clampedSubspan(y, 0, n2);
    auto y1 = clampedSubspan(y, n2, n2);
    auto scratchForRecursion = scratch.subspan(2 * n, 2 * n);

    auto p0 = scratch.first(n);
    karatsubaMain(interrupt, p0, x0, y0, scratchForRecursion, n2);
    if (interrupt.interrupted())
        return;
    std::ranges::copy(p0, z.begin());

    auto p2 = scratch.subspan(n, n);
    karatsubaMain(interrupt, p2, x1, y1, scratchForRecursion, n2);
    if (interrupt.interrupted())
        return;
    auto z2 = z.subspan(n);
    size_t end = std::min(z2.size(), p2.size());
    std::ranges::copy(p2.first(end), z2.begin());
    ASSERT(normalize(p2).size() <= end);

    Digit overflow = inplaceAddAndPropagate(z.subspan(n2), p0);
    overflow += inplaceAddAndPropagate(z.subspan(n2), p2);

    auto xDifference = scratch.first(n2);
    auto yDifference = scratch.subspan(n2, n2);
    bool negative = false;
    karatsubaAbsoluteDifference(xDifference, x1, x0, negative);
    karatsubaAbsoluteDifference(yDifference, y0, y1, negative);
    auto p1 = scratch.subspan(n, n);
    karatsubaMain(interrupt, p1, xDifference, yDifference, scratchForRecursion, n2);
    if (interrupt.interrupted())
        return;
    if (negative)
        overflow -= inplaceSubAndPropagate(z.subspan(n2), p1);
    else
        overflow += inplaceAddAndPropagate(z.subspan(n2), p1);
    ASSERT_UNUSED(overflow, !overflow);
}

void JSBigInt::karatsubaChunk(InterruptCheck& interrupt, std::span<Digit> z, std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> scratch)
{
    x = normalize(x);
    y = normalize(y);
    if (x.size() < y.size())
        std::swap(x, y);
    if (y.size() < karatsubaThreshold) {
        multiplyZeroPadded(interrupt, z, x, y);
        return;
    }
    size_t k = karatsubaLength(y.size());
    ASSERT(scratch.size() >= 4 * k);
    karatsubaStart(interrupt, z, x, y, scratch, k);
}

void JSBigInt::karatsubaStart(InterruptCheck& interrupt, std::span<Digit> z, std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> scratch, size_t k)
{
    karatsubaMain(interrupt, z, x, y, scratch, k);
    if (interrupt.interrupted())
        return;
    if (z.size() > 2 * k)
        std::ranges::fill(z.subspan(2 * k), 0);
    if (k >= y.size() && x.size() == y.size())
        return;

    Vector<Digit> chunkProduct(2 * k);
    auto product = chunkProduct.mutableSpan();
    auto x0 = clampedSubspan(x, 0, k);
    auto y0 = clampedSubspan(y, 0, k);
    auto y1 = clampedSubspan(y, k, y.size());
    if (!y1.empty()) {
        karatsubaChunk(interrupt, product, x0, y1, scratch);
        if (interrupt.interrupted())
            return;
        inplaceAddAndPropagate(z.subspan(k), product);
    }
    for (size_t i = k; i < x.size(); i += k) {
        auto xi = clampedSubspan(x, i, k);
        karatsubaChunk(interrupt, product, xi, y0, scratch);
        if (interrupt.interrupted())
            return;
        inplaceAddAndPropagate(z.subspan(i), product);
        if (!y1.empty()) {
            karatsubaChunk(interrupt, product, xi, y1, scratch);
            if (interrupt.interrupted())
                return;
            inplaceAddAndPropagate(z.subspan(i + k), product);
        }
    }
}

std::span<JSBigInt::Digit> JSBigInt::multiplyKaratsuba(InterruptCheck& interrupt, std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    ASSERT(x.size() >= y.size());
    ASSERT(y.size() >= karatsubaThreshold);
    RELEASE_ASSERT(result.size() >= x.size() + y.size());
    size_t k = karatsubaLength(y.size());
    Vector<Digit> scratch(4 * k);
    auto z = result.first(x.size() + y.size());
    karatsubaStart(interrupt, z, x, y, scratch.mutableSpan(), k);
    return z;
}

// Toom-Cook (Toom-3) multiplication, ported from V8 [1].
// Reference: https://en.wikipedia.org/wiki/Toom%E2%80%93Cook_multiplication
//
// [1]: https://source.chromium.org/chromium/chromium/src/+/main:v8/src/bigint/mul-toom.cc
//
// Measured against the Karatsuba above (V8's crossover from its schoolbook-based Karatsuba is
// 210): Toom-3 wins from 480 digits of the smaller operand, and below that Karatsuba's
// size-rounding heuristics keep it ahead or level on every shape.
static constexpr size_t toomThreshold = 480;

// Z := X + Y, zero-padding Z. Z may alias either operand.
static void addZeroPadded(std::span<JSBigInt::Digit> z, std::span<const JSBigInt::Digit> x, std::span<const JSBigInt::Digit> y)
{
    using Digit = JSBigInt::Digit;
    if (x.size() < y.size())
        std::swap(x, y);
    ASSERT(z.size() >= x.size());
    Digit carry = 0;
    size_t i = 0;
    for (; i < y.size(); i++) {
        Digit newCarry = 0;
        z[i] = JSBigInt::digitAdd3(x[i], y[i], carry, newCarry);
        carry = newCarry;
    }
    for (; i < x.size(); i++) {
        Digit newCarry = 0;
        z[i] = JSBigInt::digitAdd(x[i], carry, newCarry);
        carry = newCarry;
    }
    for (; i < z.size(); i++) {
        z[i] = carry;
        carry = 0;
    }
}

// Z := X - Y for normalized X >= Y, zero-padding Z. Z may alias either operand.
static void subZeroPadded(std::span<JSBigInt::Digit> z, std::span<const JSBigInt::Digit> x, std::span<const JSBigInt::Digit> y)
{
    using Digit = JSBigInt::Digit;
    ASSERT(z.size() >= x.size() && x.size() >= y.size());
    Digit borrow = 0;
    size_t i = 0;
    for (; i < y.size(); i++) {
        Digit newBorrow = 0;
        z[i] = JSBigInt::digitSub2(x[i], y[i], borrow, newBorrow);
        borrow = newBorrow;
    }
    for (; i < x.size(); i++) {
        Digit newBorrow = 0;
        z[i] = JSBigInt::digitSub(x[i], borrow, newBorrow);
        borrow = newBorrow;
    }
    ASSERT(!borrow);
    for (; i < z.size(); i++)
        z[i] = 0;
}

static bool lessThanNormalized(std::span<const JSBigInt::Digit> x, std::span<const JSBigInt::Digit> y)
{
    if (x.size() != y.size())
        return x.size() < y.size();
    for (size_t i = x.size(); i-- > 0;) {
        if (x[i] != y[i])
            return x[i] < y[i];
    }
    return false;
}

// Z := X + Y on sign-magnitude values, returning the sign of Z. Z may alias either operand.
static bool addSigned(std::span<JSBigInt::Digit> z, std::span<const JSBigInt::Digit> x, bool xNegative, std::span<const JSBigInt::Digit> y, bool yNegative)
{
    if (xNegative == yNegative) {
        addZeroPadded(z, x, y);
        return xNegative;
    }
    x = normalize(x);
    y = normalize(y);
    if (!lessThanNormalized(x, y)) {
        subZeroPadded(z, x, y);
        return xNegative;
    }
    subZeroPadded(z, y, x);
    return !xNegative;
}

// Z := X - Y on sign-magnitude values, returning the sign of Z. Z may alias either operand.
static bool subtractSigned(std::span<JSBigInt::Digit> z, std::span<const JSBigInt::Digit> x, bool xNegative, std::span<const JSBigInt::Digit> y, bool yNegative)
{
    if (xNegative != yNegative) {
        addZeroPadded(z, x, y);
        return xNegative;
    }
    x = normalize(x);
    y = normalize(y);
    if (!lessThanNormalized(x, y)) {
        subZeroPadded(z, x, y);
        return xNegative;
    }
    subZeroPadded(z, y, x);
    return !xNegative;
}

static void timesTwo(std::span<JSBigInt::Digit> x)
{
    JSBigInt::Digit carry = 0;
    for (auto& digit : x) {
        JSBigInt::Digit d = digit;
        digit = (d << 1) | carry;
        carry = d >> (JSBigInt::digitBits - 1);
    }
}

static void divideByTwo(std::span<JSBigInt::Digit> x)
{
    JSBigInt::Digit carry = 0;
    for (size_t i = x.size(); i-- > 0;) {
        JSBigInt::Digit d = x[i];
        x[i] = (d >> 1) | carry;
        carry = d << (JSBigInt::digitBits - 1);
    }
}

static void divideByThree(std::span<JSBigInt::Digit> x)
{
    using Digit = JSBigInt::Digit;
    constexpr unsigned halfDigitBits = JSBigInt::halfDigitBits;
    constexpr Digit halfDigitMask = JSBigInt::halfDigitMask;
    Digit remainder = 0;
    for (size_t i = x.size(); i-- > 0;) {
        Digit d = x[i];
        Digit upper = (remainder << halfDigitBits) | (d >> halfDigitBits);
        Digit upperResult = upper / 3;
        remainder = upper - 3 * upperResult;
        Digit lower = (remainder << halfDigitBits) | (d & halfDigitMask);
        Digit lowerResult = lower / 3;
        remainder = lower - 3 * lowerResult;
        x[i] = (upperResult << halfDigitBits) | lowerResult;
    }
}

void JSBigInt::toom3Main(InterruptCheck& interrupt, std::span<Digit> z, std::span<const Digit> x, std::span<const Digit> y)
{
    ASSERT(z.size() >= x.size() + y.size());
    // Phase 1: Splitting.
    size_t i = (std::max(x.size(), y.size()) + 2) / 3;
    auto x0 = clampedSubspan(x, 0, i);
    auto x1 = clampedSubspan(x, i, i);
    auto x2 = clampedSubspan(x, 2 * i, i);
    auto y0 = clampedSubspan(y, 0, i);
    auto y1 = clampedSubspan(y, i, i);
    auto y2 = clampedSubspan(y, 2 * i, i);

    // Temporary storage.
    size_t pLength = i + 1; // For all px, qx below.
    size_t rLength = 2 * pLength; // For all r_x, Rx below.
    Vector<Digit> tempStorage(4 * rLength);
    // We will use the same variable names as the Wikipedia article, as much as C++ lets us: our
    // "pm1" is their "p(-1)" etc. For consistency with other algorithms, we use X and Y where
    // Wikipedia uses m and n.
    // We will use and reuse the temporary storage as follows:
    //
    //   chunk                  | -------- time ----------->
    //   [0 .. i]               |( po )( pm1 ) ( rm2  )
    //   [i+1 .. rLength-1]     |( qo )( qm1 ) ( rm2  )
    //   [rLength .. rLength+i] | (p1 ) ( pm2 ) (rinf)
    //   [rLength+i+1 .. 2*rLength-1] | (q1 ) ( qm2 ) (rinf)
    //   [2*rLength .. 3*rLength-1]   |      (   r1          )
    //   [3*rLength .. 4*rLength-1]   |             (  rm1   )
    //
    // This requires interleaving phases 2 and 3 a bit: after computing r1 = p1 * q1, we can reuse
    // p1's storage for pm2, and so on.
    auto t = tempStorage.mutableSpan();
    auto po = t.subspan(0, pLength);
    auto qo = t.subspan(pLength, pLength);
    auto p1 = t.subspan(rLength, pLength);
    auto q1 = t.subspan(rLength + pLength, pLength);
    auto r1 = t.subspan(2 * rLength, rLength);
    auto rm1 = t.subspan(3 * rLength, rLength);

    // We can also share the backing stores of Z, r0, R0.
    auto r0 = z.first(rLength);

    // Phase 2a: Evaluation, steps 0, 1, m1.
    // po = X0 + X2
    addZeroPadded(po, x0, x2);
    // p0 = X0
    // p1 = po + X1
    addZeroPadded(p1, po, x1);
    // pm1 = po - X1
    auto pm1 = po;
    bool pm1Sign = subtractSigned(pm1, po, false, x1, false);

    // qo = Y0 + Y2
    addZeroPadded(qo, y0, y2);
    // q0 = Y0
    // q1 = qo + Y1
    addZeroPadded(q1, qo, y1);
    // qm1 = qo - Y1
    auto qm1 = qo;
    bool qm1Sign = subtractSigned(qm1, qo, false, y1, false);

    // Phase 3a: Pointwise multiplication, steps 0, 1, m1.
    multiplyZeroPadded(interrupt, r0, x0, y0);
    multiplyZeroPadded(interrupt, r1, p1, q1);
    multiplyZeroPadded(interrupt, rm1, pm1, qm1);
    bool rm1Sign = pm1Sign != qm1Sign;

    // Phase 2b: Evaluation, steps m2 and inf.
    // pm2 = (pm1 + X2) * 2 - X0
    auto pm2 = p1;
    bool pm2Sign = addSigned(pm2, pm1, pm1Sign, x2, false);
    timesTwo(pm2);
    pm2Sign = subtractSigned(pm2, pm2, pm2Sign, x0, false);
    // pinf = X2

    // qm2 = (qm1 + Y2) * 2 - Y0
    auto qm2 = q1;
    bool qm2Sign = addSigned(qm2, qm1, qm1Sign, y2, false);
    timesTwo(qm2);
    qm2Sign = subtractSigned(qm2, qm2, qm2Sign, y0, false);
    // qinf = Y2

    // Phase 3b: Pointwise multiplication, steps m2 and inf.
    auto rm2 = t.first(rLength);
    multiplyZeroPadded(interrupt, rm2, pm2, qm2);
    bool rm2Sign = pm2Sign != qm2Sign;

    auto rinf = t.subspan(rLength, rLength);
    multiplyZeroPadded(interrupt, rinf, x2, y2);
    if (interrupt.interrupted())
        return;

    // Phase 4: Interpolation.
    auto R0 = r0;
    auto R4 = rinf;
    // R3 <- (rm2 - r1) / 3
    auto R3 = rm2;
    bool R3Sign = subtractSigned(R3, rm2, rm2Sign, r1, false);
    divideByThree(R3);
    // R1 <- (r1 - rm1) / 2
    auto R1 = r1;
    bool R1Sign = subtractSigned(R1, r1, false, rm1, rm1Sign);
    divideByTwo(R1);
    // R2 <- rm1 - r0
    auto R2 = rm1;
    bool R2Sign = subtractSigned(R2, rm1, rm1Sign, R0, false);
    // R3 <- (R2 - R3) / 2 + 2 * rinf
    R3Sign = subtractSigned(R3, R2, R2Sign, R3, R3Sign);
    divideByTwo(R3);
    R3Sign = addSigned(R3, R3, R3Sign, rinf, false);
    R3Sign = addSigned(R3, R3, R3Sign, rinf, false);
    // R2 <- R2 + R1 - R4
    R2Sign = addSigned(R2, R2, R2Sign, R1, R1Sign);
    R2Sign = subtractSigned(R2, R2, R2Sign, R4, false);
    // R1 <- R1 - R3
    R1Sign = subtractSigned(R1, R1, R1Sign, R3, R3Sign);

    ASSERT(!R1Sign || normalize(R1).empty());
    ASSERT(!R2Sign || normalize(R2).empty());
    ASSERT(!R3Sign || normalize(R3).empty());

    // Phase 5: Recomposition. R0 is already in place. Overflow can't happen.
    std::ranges::fill(z.subspan(R0.size()), 0);
    inplaceAddAndPropagate(z.subspan(i), R1);
    inplaceAddAndPropagate(z.subspan(2 * i), R2);
    inplaceAddAndPropagate(z.subspan(3 * i), R3);
    inplaceAddAndPropagate(z.subspan(4 * i), R4);
}

std::span<JSBigInt::Digit> JSBigInt::multiplyToomCook(InterruptCheck& interrupt, std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    ASSERT(x.size() >= y.size());
    ASSERT(y.size() >= toomThreshold);
    RELEASE_ASSERT(result.size() >= x.size() + y.size());
    auto z = result.first(x.size() + y.size());
    // toom3Main splits both operands into thirds of the larger one, so a moderately longer x costs
    // the same five products as a balanced pair and beats chunking x into y-sized pieces. Beyond
    // that ratio the padding wastes more than the chunking does.
    if (x.size() * 3 <= y.size() * 5) {
        toom3Main(interrupt, z, x, y);
        return z;
    }
    size_t k = y.size();
    toom3Main(interrupt, z, x.first(k), y);
    Vector<Digit> chunkProduct(2 * k);
    auto product = chunkProduct.mutableSpan();
    for (size_t i = k; i < x.size() && !interrupt.interrupted(); i += k) {
        auto xi = clampedSubspan(x, i, k);
        if (xi.size() < k) {
            // The last chunk is shorter, so let the size dispatch pick its algorithm.
            multiplyZeroPadded(interrupt, product, xi, y);
        } else
            toom3Main(interrupt, product, xi, y);
        inplaceAddAndPropagate(z.subspan(i), product);
    }
    return z;
}

// FFT-based multiplication, due to Schönhage and Strassen, ported from V8 [1]. The implementation
// mostly follows the description given in Christoph Lüders: Fast Multiplication of Large Integers,
// http://arxiv.org/abs/1503.04955
//
// [1]: https://source.chromium.org/chromium/chromium/src/+/main:v8/src/bigint/mul-fft.cc
namespace FFT {

using Digit = JSBigInt::Digit;
using SignedDigit = std::make_signed_t<Digit>;
static constexpr unsigned digitBits = JSBigInt::digitBits;
static constexpr unsigned log2DigitBits = std::countr_zero(digitBits);
static_assert((1u << log2DigitBits) == digitBits);

// See shouldUseFFT for what the thresholds mean. The 64-bit values are measured against the Toom-3
// above; the 32-bit ones reduce to V8's single threshold on the smaller operand.
#if CPU(REGISTER64)
static constexpr size_t fftThreshold = 2300;
static constexpr size_t fftMinSmallerSize = 600;
static constexpr size_t fftChunkThreshold = 1150;
static constexpr size_t fftInnerThreshold = 200;
#else
static constexpr size_t fftThreshold = 2200;
static constexpr size_t fftMinSmallerSize = 1100;
static constexpr size_t fftChunkThreshold = 1100;
static constexpr size_t fftInnerThreshold = 200;
#endif
// Above this ratio of the operand sizes, one transform sized for both is mostly padding, so x is
// multiplied in y-sized chunks instead.
static constexpr size_t asymmetricChunkingThreshold = 100;

// Part 1: Functions for "mod F_n" arithmetic.
// F_n is of the shape 2^K + 1, and for convenience we use K to count the number of digits rather
// than the number of bits, so F_n (or K) are implicit and deduced from the length {length} of the
// digits array.

// Helper function for {modFn} below.
static void modFnHelper(Digit* x, size_t length, SignedDigit high)
{
    if (high > 0) {
        Digit borrow = high;
        x[length - 1] = 0;
        for (size_t i = 0; i < length; i++) {
            Digit newBorrow = 0;
            x[i] = JSBigInt::digitSub(x[i], borrow, newBorrow);
            borrow = newBorrow;
            if (!borrow)
                break;
        }
    } else {
        Digit carry = -high;
        x[length - 1] = 0;
        for (size_t i = 0; i < length; i++) {
            Digit newCarry = 0;
            x[i] = JSBigInt::digitAdd(x[i], carry, newCarry);
            carry = newCarry;
            if (!carry)
                break;
        }
    }
}

// {x} := {x} mod F_n, assuming that {x} is "slightly" larger than F_n (e.g. after addition of two
// numbers that were mod-F_n-normalized before).
static void modFn(Digit* x, size_t length)
{
    size_t K = length - 1;
    SignedDigit high = x[K];
    if (!high)
        return;
    modFnHelper(x, length, high);
    high = x[K];
    if (!high)
        return;
    ASSERT(high == 1 || high == -1);
    modFnHelper(x, length, high);
    high = x[K];
    if (high == -1)
        modFnHelper(x, length, high);
}

// {dest} := {src} mod F_n, assuming that {src} is about twice as long as F_n (e.g. after
// multiplication of two numbers that were mod-F_n-normalized before).
// {length} is length of {dest}; {src} is twice as long.
static void modFnDoubleWidth(Digit* dest, const Digit* src, size_t length)
{
    size_t K = length - 1;
    Digit borrow = 0;
    for (size_t i = 0; i < K; i++) {
        Digit newBorrow = 0;
        dest[i] = JSBigInt::digitSub2(src[i], src[i + K], borrow, newBorrow);
        borrow = newBorrow;
    }
    Digit newBorrow = 0;
    dest[K] = JSBigInt::digitSub2(0, src[2 * K], borrow, newBorrow);
    // {borrow} may be non-zero here, that's OK as {modFn} will take care of it.
    modFn(dest, length);
}

// Sets {sum} := {a} + {b} and {diff} := {a} - {b}, which is more efficient than computing sum and
// difference separately. Applies "mod F_n" normalization to both results.
static void sumDiff(Digit* sum, Digit* diff, const Digit* a, const Digit* b, size_t length)
{
    Digit carry = 0;
    Digit borrow = 0;
    for (size_t i = 0; i < length; i++) {
        // Read both values first, because inputs and outputs can overlap.
        Digit ai = a[i];
        Digit bi = b[i];
        Digit newCarry = 0;
        sum[i] = JSBigInt::digitAdd3(ai, bi, carry, newCarry);
        carry = newCarry;
        Digit newBorrow = 0;
        diff[i] = JSBigInt::digitSub2(ai, bi, borrow, newBorrow);
        borrow = newBorrow;
    }
    modFn(sum, length);
    modFn(diff, length);
}

// {result} := ({input} << shift) mod F_n, where shift >= K.
static void shiftModFnLarge(Digit* result, const Digit* input, size_t digitShift, unsigned bitsShift, size_t K)
{
    // If {digitShift} is greater than K, we use the following transformation (where, since
    // everything is mod 2^K + 1, we are allowed to add or subtract any multiple of 2^K + 1 at any
    // time):
    //      x * 2^{K+m}   mod 2^K + 1
    //   == x * 2^K * 2^m - (2^K + 1)*(x * 2^m)   mod 2^K + 1
    //   == x * 2^K * 2^m - x * 2^K * 2^m - x * 2^m   mod 2^K + 1
    //   == -x * 2^m   mod 2^K + 1
    // So the flow is the same as for m < K, but we invert the subtraction's operands. In order to
    // avoid underflow, we virtually initialize the result to 2^K + 1:
    //   input  =  [ iK ][iK-1] ....  .... [ i1 ][ i0 ]
    //   result =  [   1][0000] ....  .... [0000][0001]
    //            +                  [ iK ] .... [ iX ]
    //            -      [iX-1] .... [ i0 ]
    ASSERT(digitShift >= K);
    digitShift -= K;
    Digit borrow = 0;
    if (!bitsShift) {
        Digit carry = 1;
        for (size_t i = 0; i < digitShift; i++) {
            Digit newCarry = 0;
            result[i] = JSBigInt::digitAdd(input[i + K - digitShift], carry, newCarry);
            carry = newCarry;
        }
        result[digitShift] = JSBigInt::digitSub(input[K] + carry, input[0], borrow);
        for (size_t i = digitShift + 1; i < K; i++) {
            Digit d = input[i - digitShift];
            Digit newBorrow = 0;
            result[i] = JSBigInt::digitSub2(0, d, borrow, newBorrow);
            borrow = newBorrow;
        }
    } else {
        Digit addCarry = 1;
        Digit inputCarry = input[K - digitShift - 1] >> (digitBits - bitsShift);
        for (size_t i = 0; i < digitShift; i++) {
            Digit d = input[i + K - digitShift];
            Digit summand = (d << bitsShift) | inputCarry;
            Digit newCarry = 0;
            result[i] = JSBigInt::digitAdd(summand, addCarry, newCarry);
            addCarry = newCarry;
            inputCarry = d >> (digitBits - bitsShift);
        }
        {
            // result[digitShift] = (addCarry + iKPart) - i0Part
            Digit d = input[K];
            Digit iKPart = (d << bitsShift) | inputCarry;
            Digit iKCarry = d >> (digitBits - bitsShift);
            Digit newCarry = 0;
            Digit sum = JSBigInt::digitAdd(addCarry, iKPart, newCarry);
            addCarry = newCarry;
            // {iKCarry} is less than a full digit, so we can merge {addCarry} into it without
            // overflow.
            iKCarry += addCarry;
            d = input[0];
            Digit i0Part = d << bitsShift;
            result[digitShift] = JSBigInt::digitSub(sum, i0Part, borrow);
            inputCarry = d >> (digitBits - bitsShift);
            if (digitShift + 1 < K) {
                d = input[1];
                Digit subtrahend = (d << bitsShift) | inputCarry;
                Digit newBorrow = 0;
                result[digitShift + 1] = JSBigInt::digitSub2(iKCarry, subtrahend, borrow, newBorrow);
                borrow = newBorrow;
                inputCarry = d >> (digitBits - bitsShift);
            }
        }
        for (size_t i = digitShift + 2; i < K; i++) {
            Digit d = input[i - digitShift];
            Digit subtrahend = (d << bitsShift) | inputCarry;
            Digit newBorrow = 0;
            result[i] = JSBigInt::digitSub2(0, subtrahend, borrow, newBorrow);
            borrow = newBorrow;
            inputCarry = d >> (digitBits - bitsShift);
        }
    }
    // The virtual 1 in result[K] should be eliminated by {borrow}. If there is no borrow, then
    // the virtual initialization was too much. Subtract 2^K + 1.
    result[K] = 0;
    if (borrow != 1) {
        borrow = 1;
        for (size_t i = 0; i < K; i++) {
            Digit newBorrow = 0;
            result[i] = JSBigInt::digitSub(result[i], borrow, newBorrow);
            borrow = newBorrow;
            if (!borrow)
                break;
        }
        if (borrow) {
            // The result must be 2^K.
            for (size_t i = 0; i < K; i++)
                result[i] = 0;
            result[K] = 1;
        }
    }
}

// Sets {result} := {input} * 2^{powerOfTwo} mod 2^{K} + 1.
// This function is highly relevant for overall performance.
static void shiftModFn(Digit* result, const Digit* input, size_t powerOfTwo, size_t K, size_t zeroAbove = std::numeric_limits<size_t>::max())
{
    // The modulo-reduction amounts to a subtraction, which we combine with the shift as follows:
    //   input  =  [ iK ][iK-1] ....  .... [ i1 ][ i0 ]
    //   result =        [iX-1] .... [ i0 ] <---------- shift by {powerOfTwo}
    //            -                  [ iK ] .... [ iX ]
    // where "X" is the index "K - digitShift".
    size_t digitShift = powerOfTwo / digitBits;
    unsigned bitsShift = powerOfTwo % digitBits;
    // By an analogous construction to the "digitShift >= K" case, it turns out that:
    //    x * 2^{2K+m} == x * 2^m   mod 2^K + 1.
    while (digitShift >= 2 * K)
        digitShift -= 2 * K; // Faster than '%'!
    if (digitShift >= K)
        return shiftModFnLarge(result, input, digitShift, bitsShift, K);
    Digit borrow = 0;
    if (!bitsShift) {
        // We do a single pass over {input}, starting by copying digits [i1] to [iX-1] to result
        // indices digitShift+1 to K-1.
        size_t i = 1;
        // Read input digits unless we know they are zero.
        size_t cap = std::min(K - digitShift, zeroAbove);
        for (; i < cap; i++)
            result[i + digitShift] = input[i];
        // Any remaining work can hard-code the knowledge that input[i] == 0.
        for (; i < K - digitShift; i++) {
            ASSERT(!input[i]);
            result[i + digitShift] = 0;
        }
        // Second phase: subtract input digits [iX] to [iK] from (virtually) zero-initialized
        // result indices 0 to digitShift-1.
        cap = std::min(K, zeroAbove);
        for (; i < cap; i++) {
            Digit d = input[i];
            Digit newBorrow = 0;
            result[i - K + digitShift] = JSBigInt::digitSub2(0, d, borrow, newBorrow);
            borrow = newBorrow;
        }
        // Any remaining work can hard-code the knowledge that input[i] == 0.
        for (; i < K; i++) {
            ASSERT(!input[i]);
            Digit newBorrow = 0;
            result[i - K + digitShift] = JSBigInt::digitSub(0, borrow, newBorrow);
            borrow = newBorrow;
        }
        // Last step: subtract [iK] from [i0] and store at result index digitShift.
        Digit newBorrow = 0;
        result[digitShift] = JSBigInt::digitSub2(input[0], input[K], borrow, newBorrow);
        borrow = newBorrow;
    } else {
        // Same flow as before, but taking bitsShift != 0 into account.
        // First phase: result indices digitShift+1 to K.
        Digit carry = 0;
        size_t i = 0;
        // Read input digits unless we know they are zero.
        size_t cap = std::min(K - digitShift, zeroAbove);
        for (; i < cap; i++) {
            Digit d = input[i];
            result[i + digitShift] = (d << bitsShift) | carry;
            carry = d >> (digitBits - bitsShift);
        }
        // Any remaining work can hard-code the knowledge that input[i] == 0.
        for (; i < K - digitShift; i++) {
            ASSERT(!input[i]);
            result[i + digitShift] = carry;
            carry = 0;
        }
        // Second phase: result indices 0 to digitShift - 1.
        cap = std::min(K, zeroAbove);
        for (; i < cap; i++) {
            Digit d = input[i];
            Digit newBorrow = 0;
            result[i - K + digitShift] = JSBigInt::digitSub2(0, (d << bitsShift) | carry, borrow, newBorrow);
            borrow = newBorrow;
            carry = d >> (digitBits - bitsShift);
        }
        // Any remaining work can hard-code the knowledge that input[i] == 0.
        if (i < K) {
            ASSERT(!input[i]);
            Digit newBorrow = 0;
            result[i - K + digitShift] = JSBigInt::digitSub2(0, carry, borrow, newBorrow);
            borrow = newBorrow;
            carry = 0;
            i++;
        }
        for (; i < K; i++) {
            ASSERT(!input[i]);
            Digit newBorrow = 0;
            result[i - K + digitShift] = JSBigInt::digitSub(0, borrow, newBorrow);
            borrow = newBorrow;
        }
        // Last step: compute result[digitShift].
        Digit d = input[K];
        Digit newBorrow = 0;
        result[digitShift] = JSBigInt::digitSub2(result[digitShift], (d << bitsShift) | carry, borrow, newBorrow);
        borrow = newBorrow;
        // No carry left.
        ASSERT(!(d >> (digitBits - bitsShift)));
    }
    result[K] = 0;
    for (size_t i = digitShift + 1; i <= K && borrow; i++) {
        Digit newBorrow = 0;
        result[i] = JSBigInt::digitSub(result[i], borrow, newBorrow);
        borrow = newBorrow;
    }
    if (borrow) {
        // Underflow means we subtracted too much. Add 2^K + 1.
        Digit carry = 1;
        for (size_t i = 0; i <= K; i++) {
            Digit newCarry = 0;
            result[i] = JSBigInt::digitAdd(result[i], carry, newCarry);
            carry = newCarry;
            if (!carry)
                break;
        }
        Digit newCarry = 0;
        result[K] = JSBigInt::digitAdd(result[K], 1, newCarry);
    }
}

// Part 2: FFT-based multiplication is very sensitive to appropriate choice of parameters. The
// following functions choose the parameters that the subsequent actual computation will use. This
// is partially based on formal constraints and partially on experimentally-determined heuristics.

struct Parameters {
    unsigned m { 0 };
    size_t K { 0 };
    size_t n { 0 };
    size_t s { 0 };
    size_t r { 0 };
};

// Computes parameters for the main calculation, given a bit length {N} and an {m}. See the paper
// for details.
static void computeParameters(size_t N, unsigned m, Parameters& params)
{
    N *= digitBits;
    size_t n = static_cast<size_t>(1) << m; // 2^m
    size_t nhalf = n >> 1;
    size_t s = (N + n - 1) >> m; // ceil(N/n)
    s = roundUpToMultipleOf(digitBits, s);
    size_t K = m + 2 * s + 1; // K must be at least this big...
    K = roundUpToMultipleOf(nhalf, K); // ...and a multiple of n/2.
    size_t r = K >> (m - 1); // Which multiple?

    // We want recursive calls to make progress, so force K to be a multiple of 8 if it's above
    // the recursion threshold. Otherwise, K must be a multiple of digitBits.
    const unsigned threshold = (K + 1 >= fftInnerThreshold * digitBits) ? 3 + log2DigitBits : log2DigitBits;
    unsigned trailingZeros = std::countr_zero(K);
    while (trailingZeros < threshold) {
        K += (static_cast<size_t>(1) << trailingZeros);
        r = K >> (m - 1);
        trailingZeros = std::countr_zero(K);
    }

    ASSERT(!(K % digitBits));
    ASSERT(!(s % digitBits));
    params.K = K / digitBits;
    params.s = s / digitBits;
    params.n = n;
    params.r = r;
}

// Computes parameters for recursive invocations ("inner layer").
static void computeParametersInner(size_t N, Parameters& params)
{
    unsigned maxM = std::countr_zero(N);
    unsigned NBits = std::bit_width(N);
    unsigned m = NBits - 4; // Don't let s get too small.
    m = std::min(maxM, m);
    N *= digitBits;
    size_t n = static_cast<size_t>(1) << m; // 2^m
    // We can't round up s in the inner layer, because N = n*s is fixed.
    size_t s = N >> m;
    ASSERT(N == s * n);
    size_t K = m + 2 * s + 1; // K must be at least this big...
    K = roundUpToMultipleOf(n, K); // ...and a multiple of n and digitBits.
    K = roundUpToMultipleOf(digitBits, K);
    params.r = K >> m; // Which multiple?
    ASSERT(!(K % digitBits));
    ASSERT(!(s % digitBits));
    params.K = K / digitBits;
    params.s = s / digitBits;
    params.n = n;
    params.m = m;
}

static size_t predictInnerK(size_t N)
{
    Parameters params;
    computeParametersInner(N, params);
    return params.K;
}

// Applies heuristics to decide whether {m} should be decremented, by looking at what would happen
// to {K} and {s} if {m} was decremented.
static bool shouldDecrementM(const Parameters& current, const Parameters& next, const Parameters& afterNext)
{
    // K == 64 seems to work particularly well.
    if (current.K == 64 && next.K >= 112)
        return false;
    // Small values for s are never efficient.
    if (current.s < 6)
        return true;
    // The time is roughly determined by K * n. When we decrement m, then n always halves, and K
    // usually gets bigger, by up to 2x.
    // For not-quite-so-small s, look at how much bigger K would get: if the K increase is small
    // enough, making n smaller is worth it.
    // Empirically, it's most meaningful to look at the K *after* next.
    // The specific threshold values have been chosen by running many benchmarks on inputs of many
    // sizes, and manually selecting thresholds that seemed to produce good results.
    double factor = static_cast<double>(afterNext.K) / current.K;
    if ((current.s == 6 && factor < 3.85)
        || (current.s == 7 && factor < 3.73)
        || (current.s == 8 && factor < 3.55)
        || (current.s == 9 && factor < 3.50)
        || factor < 3.4)
        return true;
    // If K is just below the recursion threshold, make sure we do recurse, unless doing so would
    // be particularly inefficient (large inner K).
    // If K is just above the recursion threshold, doubling it often makes the inner call more
    // efficient.
    if (current.K >= 160 && current.K < 250 && predictInnerK(next.K) < 28)
        return true;
    // If we found no reason to decrement, keep m as large as possible.
    return false;
}

// Decides what parameters to use for a given input bit length {N}.
// Returns the chosen m.
static unsigned getParameters(size_t N, Parameters& params)
{
    unsigned NBits = std::bit_width(N);
    unsigned maxM = NBits - 3; // Larger m make s too small.
    maxM = std::max(log2DigitBits, maxM); // Smaller m break the logic below.
    unsigned m = maxM;
    Parameters current;
    computeParameters(N, m, current);
    Parameters next;
    computeParameters(N, m - 1, next);
    while (m > 2) {
        Parameters afterNext;
        computeParameters(N, m - 2, afterNext);
        if (shouldDecrementM(current, next, afterNext)) {
            m--;
            current = next;
            next = afterNext;
        } else
            break;
    }
    params = current;
    return m;
}

} // namespace FFT

// Part 3: Fast Fourier Transformation.

class JSBigInt::FFTContainer {
    WTF_MAKE_NONCOPYABLE(FFTContainer);
    using Digit = JSBigInt::Digit;
public:
    // {n} is the number of chunks, whose length is {K}+1.
    // {K} determines F_n = 2^(K * digitBits) + 1.
    FFTContainer(InterruptCheck& interrupt, size_t n, size_t K)
        : m_interrupt(interrupt)
        , m_n(n)
        , m_K(K)
        , m_length(K + 1)
        , m_storage(m_length * n)
        , m_parts(n)
        , m_temp(m_length * 2)
    {
        Digit* pointer = m_storage.mutableSpan().data();
        for (size_t i = 0; i < n; i++, pointer += m_length)
            m_parts[i] = pointer;
    }

    void startDefault(std::span<const Digit> x, size_t chunkSize, size_t theta, size_t omega);
    void start(std::span<const Digit> x, size_t chunkSize, size_t theta, size_t omega);

    void normalizeAndRecombine(size_t omega, unsigned m, std::span<Digit> z, size_t chunkSize);
    void counterWeightAndRecombine(size_t theta, unsigned m, std::span<Digit> z, size_t chunkSize);

    void fftReturnShuffled(size_t start, size_t length, size_t omega, Digit* temp);
    void fftRecurse(size_t start, size_t half, size_t omega, Digit* temp);

    void backwardFFT(size_t start, size_t length, size_t omega);

    void pointwiseMultiply(const FFTContainer& other);

    static void multiplyInner(InterruptCheck&, std::span<Digit> z, std::span<const Digit> x, std::span<const Digit> y, const FFT::Parameters&);

private:
    Digit* temp() { return m_temp.mutableSpan().data(); }
    std::span<Digit> tempSpan() { return m_temp.mutableSpan().first(m_length); }
    std::span<Digit> part(size_t i) { return { m_parts[i], m_length }; }

    InterruptCheck& m_interrupt;
    const size_t m_n; // Number of parts.
    const size_t m_K; // Always m_length - 1.
    const size_t m_length; // Length of each part, in digits.
    Vector<Digit> m_storage; // Combined storage of all parts.
    Vector<Digit*> m_parts; // Pointers to each part.
    Vector<Digit> m_temp; // Temporary storage with size 2 * m_length.
};

static inline void copyAndZeroExtend(std::span<JSBigInt::Digit> destination, const JSBigInt::Digit* source, size_t digitsToCopy)
{
    memcpySpan(destination.first(digitsToCopy), std::span { source, digitsToCopy });
    zeroSpan(destination.subspan(digitsToCopy));
}

// Reads {x} into the FFTContainer's internal storage, dividing it into chunks while doing so;
// then performs the forward FFT.
void JSBigInt::FFTContainer::startDefault(std::span<const Digit> x, size_t chunkSize, size_t theta, size_t omega)
{
    size_t length = x.size();
    const Digit* pointer = x.data();
    size_t currentTheta = 0;
    size_t i = 0;
    for (; i < m_n && length > 0; i++, currentTheta += theta) {
        m_interrupt.addWork(m_length);
        chunkSize = std::min(chunkSize, length);
        // For invocations via multiplyInner, x.size() == m_n * chunkSize + 1, because the outer
        // layer's "K" is passed as the inner layer's "N". Since x is (mod Fn)-normalized on the
        // outer layer, there is the rare corner case where x[m_n * chunkSize] == 1. Detect that
        // case, and handle the extra bit as part of the last chunk; we always have the space.
        if (i == m_n - 1 && length == chunkSize + 1) {
            ASSERT(x[m_n * chunkSize] <= 1);
            ASSERT(m_length >= chunkSize + 1);
            chunkSize++;
        }
        if (currentTheta) {
            // Multiply with theta^i, and reduce modulo 2^K + 1.
            // We pass theta as a shift amount; it really means 2^theta.
            copyAndZeroExtend(tempSpan(), pointer, chunkSize);
            FFT::shiftModFn(m_parts[i], temp(), currentTheta, m_K, chunkSize);
        } else
            copyAndZeroExtend(part(i), pointer, chunkSize);
        pointer += chunkSize;
        length -= chunkSize;
    }
    ASSERT(!length);
    for (; i < m_n; i++)
        zeroSpan(part(i));
    fftReturnShuffled(0, m_n, omega, temp());
}

// This version of start is optimized for the case where ~half of the container will be filled
// with padding zeros.
void JSBigInt::FFTContainer::start(std::span<const Digit> x, size_t chunkSize, size_t theta, size_t omega)
{
    size_t length = x.size();
    if (length > m_n * chunkSize / 2)
        return startDefault(x, chunkSize, theta, omega);
    ASSERT_UNUSED(theta, !theta);
    const Digit* pointer = x.data();
    size_t nhalf = m_n / 2;
    // Unrolled first iteration.
    chunkSize = std::min(chunkSize, length);
    copyAndZeroExtend(part(0), pointer, chunkSize);
    copyAndZeroExtend(part(nhalf), pointer, chunkSize);
    pointer += chunkSize;
    length -= chunkSize;
    size_t i = 1;
    for (; i < nhalf && length > 0; i++) {
        m_interrupt.addWork(m_length);
        chunkSize = std::min(chunkSize, length);
        copyAndZeroExtend(part(i), pointer, chunkSize);
        size_t w = omega * i;
        FFT::shiftModFn(m_parts[i + nhalf], m_parts[i], w, m_K, chunkSize);
        pointer += chunkSize;
        length -= chunkSize;
    }
    for (; i < nhalf; i++) {
        zeroSpan(part(i));
        zeroSpan(part(i + nhalf));
    }
    fftRecurse(0, nhalf, omega, temp());
}

// Forward transformation.
// We use the "DIF" aka "decimation in frequency" transform, because it leaves the result in "bit
// reversed" order, which is precisely what we need as input for the "DIT" aka "decimation in
// time" backwards transform.
void JSBigInt::FFTContainer::fftReturnShuffled(size_t start, size_t length, size_t omega, Digit* temp)
{
    ASSERT(!(length & 1)); // {length} must be even.
    if (m_interrupt.interrupted())
        return;
    size_t half = length / 2;
    FFT::sumDiff(m_parts[start], m_parts[start + half], m_parts[start], m_parts[start + half], m_length);
    for (size_t k = 1; k < half; k++) {
        m_interrupt.addWork(m_length);
        FFT::sumDiff(m_parts[start + k], temp, m_parts[start + k], m_parts[start + half + k], m_length);
        size_t w = omega * k;
        FFT::shiftModFn(m_parts[start + half + k], temp, w, m_K);
    }
    fftRecurse(start, half, omega, temp);
}

// Recursive step of the above, factored out for additional callers.
void JSBigInt::FFTContainer::fftRecurse(size_t start, size_t half, size_t omega, Digit* temp)
{
    if (half > 1) {
        fftReturnShuffled(start, half, 2 * omega, temp);
        fftReturnShuffled(start + half, half, 2 * omega, temp);
    }
}

// Backward transformation.
// We use the "DIT" aka "decimation in time" transform here, because it turns bit-reversed input
// into normally sorted output.
void JSBigInt::FFTContainer::backwardFFT(size_t start, size_t length, size_t omega)
{
    ASSERT(!(length & 1)); // {length} must be even.
    size_t half = length / 2;
    // Don't recurse for half == 2, as pointwiseMultiply already performed the first level of the
    // backwards FFT.
    if (half > 2) {
        backwardFFT(start, half, 2 * omega);
        backwardFFT(start + half, half, 2 * omega);
    }
    if (m_interrupt.interrupted())
        return;
    FFT::sumDiff(m_parts[start], m_parts[start + half], m_parts[start], m_parts[start + half], m_length);
    for (size_t k = 1; k < half; k++) {
        m_interrupt.addWork(m_length);
        size_t w = omega * (length - k);
        FFT::shiftModFn(temp(), m_parts[start + half + k], w, m_K);
        FFT::sumDiff(m_parts[start + k], m_parts[start + half + k], m_parts[start + k], temp(), m_length);
    }
}

// Recombines the result's parts into {z}, after backwards FFT.
void JSBigInt::FFTContainer::normalizeAndRecombine(size_t omega, unsigned m, std::span<Digit> z, size_t chunkSize)
{
    std::ranges::fill(z, 0);
    size_t zIndex = 0;
    const size_t shift = m_n * omega - m;
    for (size_t i = 0; i < m_n && !m_interrupt.interrupted(); i++, zIndex += chunkSize) {
        m_interrupt.addWork(m_length);
        Digit* part = m_parts[i];
        FFT::shiftModFn(temp(), part, shift, m_K);
        Digit carry = 0;
        size_t zi = zIndex;
        size_t j = 0;
        for (; j < m_length && zi < z.size(); j++, zi++) {
            Digit newCarry = 0;
            z[zi] = digitAdd3(z[zi], m_temp[j], carry, newCarry);
            carry = newCarry;
        }
        for (; j < m_length; j++)
            ASSERT(!m_temp[j]);
        if (carry)
            z[zi] = carry;
    }
}

// Helper function for {counterWeightAndRecombine} below.
static bool fftShouldBeNegative(const JSBigInt::Digit* x, size_t xLength, JSBigInt::Digit threshold, size_t s)
{
    if (x[2 * s] >= threshold)
        return true;
    for (size_t i = 2 * s + 1; i < xLength; i++) {
        if (x[i] > 0)
            return true;
    }
    return false;
}

// Same as {normalizeAndRecombine} above, but for the needs of the recursive invocation ("inner
// layer") of FFT multiplication, where an additional counter-weighting step is required.
void JSBigInt::FFTContainer::counterWeightAndRecombine(size_t theta, unsigned m, std::span<Digit> z, size_t s)
{
    std::ranges::fill(z, 0);
    size_t zIndex = 0;
    for (size_t k = 0; k < m_n && !m_interrupt.interrupted(); k++, zIndex += s) {
        m_interrupt.addWork(m_length);
        // shift = -theta * k - m, taken modulo 2 * m_n * theta (the order of 2^theta).
        size_t shift = theta * k + m;
        ASSERT(shift <= 2 * m_n * theta);
        if (shift)
            shift = 2 * m_n * theta - shift;
        Digit* input = m_parts[k];
        FFT::shiftModFn(temp(), input, shift, m_K);
        size_t remainingZ = z.size() - zIndex;
        if (fftShouldBeNegative(temp(), m_length, k + 1, s)) {
            // Subtract F_n from input before adding to result. We use the following transformation
            // (knowing that X < F_n):
            // Z + (X - F_n) == Z - (F_n - X)
            Digit borrowZ = 0;
            Digit borrowFn = 0;
            {
                // i == 0:
                Digit d = digitSub(1, m_temp[0], borrowFn);
                z[zIndex] = digitSub(z[zIndex], d, borrowZ);
            }
            size_t i = 1;
            for (; i < m_K && i < remainingZ; i++) {
                Digit newBorrowFn = 0;
                Digit d = digitSub2(0, m_temp[i], borrowFn, newBorrowFn);
                borrowFn = newBorrowFn;
                Digit newBorrowZ = 0;
                z[zIndex + i] = digitSub2(z[zIndex + i], d, borrowZ, newBorrowZ);
                borrowZ = newBorrowZ;
            }
            ASSERT(i == m_K && m_K == m_length - 1);
            for (; i < m_length && i < remainingZ; i++) {
                Digit newBorrowFn = 0;
                Digit d = digitSub2(1, m_temp[i], borrowFn, newBorrowFn);
                borrowFn = newBorrowFn;
                Digit newBorrowZ = 0;
                z[zIndex + i] = digitSub2(z[zIndex + i], d, borrowZ, newBorrowZ);
                borrowZ = newBorrowZ;
            }
            ASSERT(!borrowFn);
            for (; borrowZ > 0 && i < remainingZ; i++) {
                Digit newBorrowZ = 0;
                z[zIndex + i] = digitSub(z[zIndex + i], borrowZ, newBorrowZ);
                borrowZ = newBorrowZ;
            }
        } else {
            Digit carry = 0;
            size_t i = 0;
            for (; i < m_length && i < remainingZ; i++) {
                Digit newCarry = 0;
                z[zIndex + i] = digitAdd3(z[zIndex + i], m_temp[i], carry, newCarry);
                carry = newCarry;
            }
            for (; i < m_length; i++)
                ASSERT(!m_temp[i]);
            for (; carry > 0 && i < remainingZ; i++) {
                Digit newCarry = 0;
                z[zIndex + i] = digitAdd(z[zIndex + i], carry, newCarry);
                carry = newCarry;
            }
            // {carry} might be != 0 here if z was negative before. That's fine.
        }
    }
}

// Main FFT function for recursive invocations ("inner layer").
void JSBigInt::FFTContainer::multiplyInner(InterruptCheck& interrupt, std::span<Digit> z, std::span<const Digit> x, std::span<const Digit> y, const FFT::Parameters& params)
{
    size_t omega = 2 * params.r; // really: 2^(2r)
    size_t theta = params.r; // really: 2^r

    FFTContainer a(interrupt, params.n, params.K);
    a.startDefault(x, params.s, theta, omega);
    FFTContainer b(interrupt, params.n, params.K);
    b.startDefault(y, params.s, theta, omega);

    a.pointwiseMultiply(b);

    FFTContainer& c = a;
    c.backwardFFT(0, params.n, omega);

    c.counterWeightAndRecombine(theta, params.m, z, params.s);
}

// Pointwise multiplication of the parts.
void JSBigInt::FFTContainer::pointwiseMultiply(const FFTContainer& other)
{
    ASSERT(m_n == other.m_n);
    // The (m_K & 3) != 0 condition makes sure that the inner FFT gets to split the work into at
    // least 4 chunks.
    bool useFFT = m_length >= FFT::fftInnerThreshold && !(m_K & 3);
    FFT::Parameters params;
    if (useFFT)
        FFT::computeParametersInner(m_K, params);
    std::span<Digit> result { temp(), 2 * m_length };
    for (size_t i = 0; i < m_n && !m_interrupt.interrupted(); i++) {
        std::span<const Digit> a { m_parts[i], m_length };
        std::span<const Digit> b { other.m_parts[i], m_length };
        if (useFFT)
            multiplyInner(m_interrupt, result, a, b, params);
        else
            multiplyZeroPadded(m_interrupt, result, a, b);
        if (m_interrupt.interrupted())
            return;
        FFT::modFnDoubleWidth(m_parts[i], result.data(), m_length);
        // To improve cache friendliness, we perform the first level of the backwards FFT here.
        if (i & 1)
            FFT::sumDiff(m_parts[i - 1], m_parts[i], m_parts[i - 1], m_parts[i], m_length);
    }
}

// One transform covers x.size() + y.size() digits, so that sum sets the crossover against Toom-3,
// whose cost for a longer x grows with x / y chunks of y-sized products. The smaller operand still
// has to be wide enough for the transform's overhead to pay off, and once x is so long that it is
// chunked, each chunk's transform only covers 2 * y.size(), so that needs the larger minimum.
static bool shouldUseFFT(size_t largerSize, size_t smallerSize)
{
    using namespace FFT;
    if (smallerSize < fftMinSmallerSize || largerSize + smallerSize < fftThreshold)
        return false;
    if (largerSize > asymmetricChunkingThreshold * smallerSize)
        return smallerSize >= fftChunkThreshold;
    return true;
}

// Part 4: Tying everything together into a multiplication algorithm.
std::span<JSBigInt::Digit> JSBigInt::multiplyFFT(InterruptCheck& interrupt, std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    using namespace FFT;
    ASSERT(x.size() >= y.size());
    ASSERT(shouldUseFFT(x.size(), y.size()));
    RELEASE_ASSERT(result.size() >= x.size() + y.size());
    auto z = result.first(x.size() + y.size());

    Parameters params;
    if (x.data() == y.data() && x.size() == y.size()) {
        // Squaring.
        unsigned m = getParameters(x.size() * 2, params);
        size_t omega = params.r; // really: 2^r
        FFTContainer a(interrupt, params.n, params.K);
        a.start(x, params.s, 0, omega);
        a.pointwiseMultiply(a);
        a.backwardFFT(0, params.n, omega);
        a.normalizeAndRecombine(omega, m, z, params.s);
    } else if (x.size() > y.size() * asymmetricChunkingThreshold) {
        // Asymmetric input sizes. Proceed in chunks. See multiplyToomCook.
        size_t k = y.size();
        unsigned m = getParameters(k * 2, params);
        size_t omega = params.r; // really: 2^r
        // The container {b} only needs to be initialized once, whereas {a} will be reused for
        // each chunk.
        FFTContainer b(interrupt, params.n, params.K);
        b.start(y, params.s, 0, omega);
        FFTContainer a(interrupt, params.n, params.K);
        // Unroll the first iteration to initialize {z}.
        auto x0 = clampedSubspan(x, 0, k);
        a.start(x0, params.s, 0, omega);
        a.pointwiseMultiply(b);
        a.backwardFFT(0, params.n, omega);
        a.normalizeAndRecombine(omega, m, z, params.s);
        // Then loop for the remaining chunks.
        Vector<Digit> chunkProduct(2 * k);
        auto product = chunkProduct.mutableSpan();
        for (size_t i = k; i < x.size() && !interrupt.interrupted(); i += k) {
            auto xi = clampedSubspan(x, i, k);
            a.start(xi, params.s, 0, omega);
            a.pointwiseMultiply(b);
            a.backwardFFT(0, params.n, omega);
            a.normalizeAndRecombine(omega, m, product, params.s);
            inplaceAddAndPropagate(z.subspan(i), product);
        }
    } else {
        // Similar-ish sized inputs. Handle them in one go.
        unsigned m = getParameters(x.size() + y.size(), params);
        size_t omega = params.r; // really: 2^r

        FFTContainer a(interrupt, params.n, params.K);
        a.start(x, params.s, 0, omega);
        FFTContainer b(interrupt, params.n, params.K);
        b.start(y, params.s, 0, omega);
        a.pointwiseMultiply(b);
        a.backwardFFT(0, params.n, omega);
        a.normalizeAndRecombine(omega, m, z, params.s);
    }
    return z;
}

ALWAYS_INLINE std::span<JSBigInt::Digit> JSBigInt::multiplyDigitsInto(InterruptCheck& interrupt, std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    ASSERT(!y.empty());
    ASSERT(x.size() >= y.size());
    ASSERT(result.size() >= x.size() + y.size());

    if (x.size() == y.size()) {
        // Aliased operands mean squaring, which needs only half of the digit multiplies.
        bool isSquare = x.data() == y.data();
        switch (y.size()) {
        case 1:
            if (isSquare)
                return squareCombaFixed<1>(x.first<1>(), result.first<2>());
            return multiplyCombaFixed<1>(x.first<1>(), y.first<1>(), result.first<2>());
        case 2:
            if (isSquare)
                return squareCombaFixed<2>(x.first<2>(), result.first<4>());
            return multiplyCombaFixed<2>(x.first<2>(), y.first<2>(), result.first<4>());
        case 4:
            if (isSquare)
                return squareCombaFixed<4>(x.first<4>(), result.first<8>());
            return multiplyCombaFixed<4>(x.first<4>(), y.first<4>(), result.first<8>());
        case 8:
            if (isSquare)
                return squareCombaFixed<8>(x.first<8>(), result.first<16>());
            return multiplyCombaFixed<8>(x.first<8>(), y.first<8>(), result.first<16>());
        case 16:
            if (isSquare)
                return squareCombaFixed<16>(x.first<16>(), result.first<32>());
            return multiplyCombaFixed<16>(x.first<16>(), y.first<16>(), result.first<32>());
        }
    }
    if (y.size() == 1)
        return multiplySingle(x, y[0], result);
    if (y.size() >= karatsubaThreshold) {
        if (y.size() < toomThreshold)
            return multiplyKaratsuba(interrupt, x, y, result);
        if (shouldUseFFT(x.size(), y.size()))
            return multiplyFFT(interrupt, x, y, result);
        return multiplyToomCook(interrupt, x, y, result);
    }
    if (shouldUseComba(x.size(), y.size()))
        return multiplyComba(x, y, result);
    return multiplySchoolbook(x, y, result);
}

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::multiplyImpl(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (x.length() < y.length())
        RELEASE_AND_RETURN(scope, multiplyImpl(globalObject, y, x));

    ASSERT(x.length() >= y.length());

    if (y.isZero())
        return { y };

    unsigned resultLength = x.length() + y.length();
    bool resultSign = x.sign() != y.sign();

    if (y.length() == 1) {
        if (y.digit(0) == 1) {
            if (resultSign == x.sign())
                return { x };
            RELEASE_AND_RETURN(scope, JSBigInt::unaryMinusImpl(globalObject, x));
        }
    }

    // N * M result with non-zero digits N / M is guaranteed to be (N + M - 1) or (N + M) length.
    // It is not so wasteful if we just allocate JSBigInt with N + M here.
    // It is possible that we will hit the JSBigInt size limit, so let's validate it after all the computation.
    if (resultLength - 1 > maxLength) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope, "BigInt generated from this operation is too big"_s);
        return nullptr;
    }

    auto xSpan = x.digits();
    auto ySpan = y.digits();
    ASSERT(xSpan.size() >= 1);
    ASSERT(ySpan.size() >= 1);

    // Note that resultLength can be one-larger than maxLength.
    // We still accept. And if the adjusted result is still larger, we will throw an OOM error.
    auto* cell = tryAllocateCell<JSBigInt>(vm, JSBigInt::allocationSize(resultLength));
    if (!cell) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope, "BigInt generated from this operation is too big"_s);
        return nullptr;
    }

    JSBigInt* bigInt = new (NotNull, cell) JSBigInt(vm, vm.bigIntStructure.get(), resultLength);
    bigInt->finishCreation(vm);
    bigInt->setSign(resultSign);

    InterruptCheck interrupt(&vm);
    std::span<Digit> result = multiplyDigitsInto(interrupt, xSpan, ySpan, bigInt->digits());
    if (interrupt.interrupted()) [[unlikely]] {
        // The digits were never finished, so leave the cell as a zero rather than a value.
        bigInt->setLength(0);
        bigInt->setSign(false);
        RELEASE_ASSERT(scope.exception());
        return nullptr;
    }
    ASSERT(!result.empty());
    if (!result.back())
        result = result.first(result.size() - 1);

    if (result.size() > maxLength) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope, "BigInt generated from this operation is too big"_s);
        return nullptr;
    }
    bigInt->setLength(result.size());

    return bigInt;
}

JSValue JSBigInt::multiply(JSGlobalObject* globalObject, JSBigInt* x, JSBigInt* y)
{
    return tryConvertToBigInt32(multiplyImpl(globalObject, HeapBigIntImpl { x }, HeapBigIntImpl { y }));
}
#if USE(BIGINT32)
JSValue JSBigInt::multiply(JSGlobalObject* globalObject, int32_t x, JSBigInt* y)
{
    return tryConvertToBigInt32(multiplyImpl(globalObject, Int32BigIntImpl { x }, HeapBigIntImpl { y }));
}
JSValue JSBigInt::multiply(JSGlobalObject* globalObject, JSBigInt* x, int32_t y)
{
    return tryConvertToBigInt32(multiplyImpl(globalObject, HeapBigIntImpl { x }, Int32BigIntImpl { y }));
}
#endif

// The algorithm is from "Division by Invariant Integers using Multiplication", Granlund and Montgomery, PLDI'94.
// https://gmplib.org/~tege/divcnst-pldi94.pdf
// This is summarized in "Improved division by invariant integers", Moller and Granlund, as "Previous Methods".
// https://gmplib.org/~tege/division-paper.pdf
//
// We implemented both previous and new methods and it turned out that the previous method is faster than the new method.
// The reason is ARM64 etc. has umulhi, which performs high umul in a extremely fast manner than the older hardware.
// So making the following adjustment branch more predictable is profitable than avoiding umulhi.
class DigitDiv {
    using Digit = JSBigInt::Digit;
    static constexpr auto digitBits = JSBigInt::digitBits;
public:
    DigitDiv(Digit d)
        : m_divisor(d)
        , m_inverse(calculateInverse(d))
    {
    }

    ALWAYS_INLINE Digit div(Digit high, Digit low, Digit& remainder)
    {
        ASSERT(high < m_divisor); // This means that quotient is within Digit. This is an invariant used in digitDiv too.
        Digit u1 = high;
        Digit u0 = low;
        Digit v = m_inverse;
        Digit d = m_divisor;

        // 1. q = ((v * u1) / beta) + u1
        Digit q = u1 + static_cast<Digit>((static_cast<TwoDigit>(u1) * static_cast<TwoDigit>(v)) >> digitBits);

        // 2. <p1, p0> = q * d
        TwoDigit p = static_cast<TwoDigit>(q) * static_cast<TwoDigit>(d);

        // 3. <r1, r0> = <u1, u0> - <p1, p0>
        TwoDigit u = (static_cast<TwoDigit>(u1) << digitBits) | u0;
        TwoDigit rem = u - p;

        // 4. while (r1 > 0 || r0 >= d) { q++; <r1, r0> = <r1, r0> - d; }
        while (rem >= d) {
            q++;
            rem -= d;
        }

        Digit r = static_cast<Digit>(rem);

#if ASSERT_ENABLED
        Digit refR = 0;
        Digit refQ = JSBigInt::digitDiv(u1, u0, d, refR);
        if (!(refR == r && refQ == q)) [[unlikely]] {
            dataLogLn(u1, " ", u0, " ", d, " ", refR, " ", r, " ", refR == r, " ", refQ, " ", q, " ", refQ == q);
            ASSERT(refR == r);
            ASSERT(refQ == q);
        }
#endif

        remainder = r;
        return q;
    }

private:
    static ALWAYS_INLINE Digit NODELETE calculateInverse(Digit d)
    {
        ASSERT(d & (1ULL << (digitBits - 1))); // d is already normalized.
        TwoDigit limit = ~static_cast<TwoDigit>(0);
        return static_cast<Digit>(limit / d);
    }

    const Digit m_divisor { };
    const Digit m_inverse { };
};

// Computes Q(uotient) and remainder for A/b, such that
// Q = (A - remainder) / b, with 0 <= remainder < b.
// If Q.len == 0, only the remainder will be returned.
// Q may be the same as A for an in-place division.
std::span<JSBigInt::Digit> JSBigInt::divideSingle(std::span<Digit> q, Digit& remainder, std::span<const Digit> a, Digit b)
{
    RELEASE_ASSERT(b != 0);
    RELEASE_ASSERT(a.size() > 0);
    remainder = 0;
    size_t length = a.size();
    if (!q.empty()) {
        if (a[length - 1] >= b) {
            RELEASE_ASSERT(q.size() >= a.size());
            for (size_t i = length; i-- > 0;)
                q[i] = digitDiv(remainder, a[i], b, remainder);
            return q.first(length);
        }

        RELEASE_ASSERT(q.size() >= a.size() - 1);
        remainder = a[length - 1];
        for (size_t i = length - 1; i-- > 0;)
            q[i] = digitDiv(remainder, a[i], b, remainder);
        return q.first(length - 1);
    }

    for (size_t i = length; i-- > 0;)
        digitDiv(remainder, a[i], b, remainder);
    return { };
}

JSBigInt::Digit JSBigInt::addAndReturnCarry(std::span<Digit> z, std::span<const Digit> x, std::span<const Digit> y)
{
    RELEASE_ASSERT(z.size() >= y.size() && x.size() >= y.size());
    Digit carry = 0;
    for (size_t i = 0; i < y.size(); i++) {
        Digit newCarry = 0;
        z[i] = digitAdd3(x[i], y[i], carry, newCarry);
        carry = newCarry;
    }
    return carry;
}

JSBigInt::Digit JSBigInt::subtractAndReturnBorrow(std::span<Digit> z, std::span<const Digit> x, std::span<const Digit> y)
{
    RELEASE_ASSERT(z.size() >= y.size() && x.size() >= y.size());
    Digit borrow = 0;
    for (size_t i = 0; i < y.size(); i++) {
        Digit borrowOut = 0;
        z[i] = digitSub2(x[i], y[i], borrow, borrowOut);
        borrow = borrowOut;
    }
    return borrow;
}

// Z += X. Returns the "carry" (0 or 1) after adding all of X's digits.
JSBigInt::Digit JSBigInt::inplaceAdd(std::span<Digit> z, std::span<const Digit> x)
{
    return addAndReturnCarry(z, z, x);
}

// Z -= X. Returns the "borrow" (0 or 1) after subtracting all of X's digits.
JSBigInt::Digit JSBigInt::inplaceSub(std::span<Digit> z, std::span<const Digit> x)
{
  return subtractAndReturnBorrow(z, z, x);
}

bool JSBigInt::greaterThanOrEqual(std::span<const Digit> a, std::span<const Digit> b)
{
    ASSERT(a.size() == b.size());
    for (size_t i = a.size(); i-- > 0;) {
        if (a[i] != b[i])
            return a[i] > b[i];
    }
    return true;
}

static std::span<JSBigInt::Digit> spanCopy(std::span<JSBigInt::Digit> z, std::span<const JSBigInt::Digit> x)
{
    if (z.data() == x.data())
        return z;
    memmoveSpan(z, x);
    return z.first(x.size());
}

// Z := X << shift
// Z and X may alias for an in-place shift.
std::span<JSBigInt::Digit> JSBigInt::leftShift(std::span<Digit> z, std::span<const Digit> x, unsigned shift)
{
    ASSERT(shift < digitBits);
    ASSERT(z.size() >= x.size());
    if (shift == 0)
        return spanCopy(z, x);

    Digit carry = 0;
    size_t i = 0;
    for (; i < x.size(); i++) {
        Digit d = x[i];
        z[i] = (d << shift) | carry;
        carry = d >> (digitBits - shift);
    }

    if (i < z.size())
        z[i++] = carry;
    else {
        ASSERT(carry == 0);
    }
    return z.first(i);
}

// Z := X >> shift
// Z and X may alias for an in-place shift.
std::span<JSBigInt::Digit> JSBigInt::rightShift(std::span<Digit> z, std::span<const Digit> x, unsigned shift)
{
    ASSERT(shift < digitBits);
    x = normalize(x);
    if (shift == 0)
        return spanCopy(z, x);

    if (x.empty())
        return { };

    RELEASE_ASSERT(z.size() >= x.size());
    Digit carry = x[0] >> shift;
    size_t last = x.size() - 1;
    size_t i = 0;
    for (; i < last; i++) {
        Digit d = x[i + 1];
        z[i] = (d << (digitBits - shift)) | carry;
        carry = d >> shift;
    }
    z[i++] = carry;
    return z.first(x.size());
}

// Computes Q(uotient) and R(emainder) for A/B, such that
// Q = (A - R) / B, with 0 <= R < B.
// Both Q and R are optional: callers that are only interested in one of them
// can pass the other with len == 0.
// If Q is present, its length must be at least A.len - B.len + 1.
// If R is present, its length must be at least B.len.
// See Knuth, Volume 2, section 4.3.1, Algorithm D.
std::tuple<std::span<JSBigInt::Digit>, std::span<JSBigInt::Digit>> JSBigInt::divideSchoolbook(std::span<Digit> q, std::span<Digit> r, std::span<const Digit> a, std::span<const Digit> b, InterruptCheck* interrupt)
{
    RELEASE_ASSERT(b.size() >= 2); // Use divideSingle otherwise.
    RELEASE_ASSERT(a.size() >= b.size()); // No-op otherwise.
    // The quotient has a.size() - b.size() + 1 digits unless a's top b.size() digits are below b,
    // in which case the top one is zero and q may omit it; the loop below asserts that.
    RELEASE_ASSERT(q.empty() || q.size() >= a.size() - b.size());
    RELEASE_ASSERT(r.empty() || r.size() >= b.size());

    // The unusual variable names inside this function are consistent with
    // Knuth's book, as well as with Go's implementation of this algorithm.
    // Maintaining this consistency is probably more useful than trying to
    // come up with more descriptive names for them.
    const size_t n = b.size();
    const size_t m = a.size() - n;

    // D1.
    // Left-shift inputs so that the divisor's MSB is set. This is necessary
    // to prevent the digit-wise divisions (see digitDiv call below) from
    // overflowing (they take a two digits wide input, and return a one digit
    // result).
    Digit lastDigit = b[n - 1];
    unsigned shift = clz(lastDigit);

    // Allocate divisor storage and normalize if needed.
    Vector<Digit, 16> normalizedDivisorStorage;
    std::span<const Digit> normalizedDivisor;
    if (shift > 0) {
        normalizedDivisorStorage.resize(n);
        auto divisorSpan = normalizedDivisorStorage.mutableSpan();
        auto filled = leftShift(divisorSpan, b, shift);
        ASSERT_UNUSED(filled, filled.size() == divisorSpan.size());
        normalizedDivisor = divisorSpan;
    } else
        normalizedDivisor = b;
    RELEASE_ASSERT(normalizedDivisor.size() == b.size());

    // U holds the (continuously updated) remaining part of the dividend, which
    // eventually becomes the remainder.
    Vector<Digit, 16> u(a.size() + 1);
    auto uSpan = u.mutableSpan();
    {
        auto filled = leftShift(uSpan, a, shift);
        if (uSpan.size() != filled.size())
            zeroSpan(uSpan.subspan(filled.size()));
    }
    RELEASE_ASSERT(uSpan.size() == a.size() + 1);

    // In each iteration, {qhatv} holds {divisor} * {current quotient digit}.
    // "v" is the book's name for {divisor}, "qhat" the current quotient digit.
    Vector<Digit, 16> qhatv(n + 1);
    auto qhatvSpan = qhatv.mutableSpan();
    RELEASE_ASSERT(qhatvSpan.size() == n + 1);

    // D2.
    // Iterate over the dividend's digits (like the "grad school" algorithm).
    // {vn1} is the divisor's most significant digit.
    // Since {n} is >= 2, {vn1} and {vn2} are always accessible.
    Digit vn1 = normalizedDivisor[n - 1];
    Digit vn2 = normalizedDivisor[n - 2];
    DigitDiv digitDiv(vn1);
    for (size_t j = m + 1; j-- > 0;) {
        // A long divisor makes each quotient digit a long row, so the termination check is per row.
        if (interrupt) {
            interrupt->addWork(n);
            if (interrupt->interrupted())
                break;
        }
        // D3.
        // Estimate the current iteration's quotient digit (see Knuth for details).
        // {qhat} is the current quotient digit.
        Digit qhat = std::numeric_limits<Digit>::max();

        // {ujn} is the dividend's most significant remaining digit.
        Digit ujn = uSpan[j + n];
        if (ujn != vn1) {
            // {rhat} is the current iteration's remainder.
            Digit rhat = 0;
            // Estimate the current quotient digit by dividing the most significant
            // digits of dividend and divisor. The result will not be too small,
            // but could be a bit too large.
            qhat = digitDiv.div(ujn, uSpan[j + n - 1], rhat);

            // Decrement the quotient estimate as needed by looking at the next
            // digit, i.e. by testing whether
            // qhat * v_{n-2} > (rhat << digitBits) + u_{j+n-2}.
            Digit ujn2 = uSpan[j + n - 2];
            while (productGreaterThan(qhat, vn2, rhat, ujn2)) {
                qhat--;
                Digit prevRhat = rhat;
                rhat += vn1;
                // v[n-1] >= 0, so this tests for overflow.
                if (rhat < prevRhat)
                    break;
            }
        }

        // D4.
        // Multiply the divisor with the current quotient digit, and subtract
        // it from the dividend. If there was "borrow", then the quotient digit
        // was one too high, so we must correct it and undo one subtraction of
        // the (shifted) divisor.
        if (qhat != 0) {
            auto filled = multiplySingle(normalizedDivisor, qhat, qhatvSpan);
            if (qhatvSpan.size() != filled.size())
                zeroSpan(qhatvSpan.subspan(filled.size()));

            Digit c = inplaceSub(uSpan.subspan(j), qhatvSpan);
            if (c) {
                c = inplaceAdd(uSpan.subspan(j), normalizedDivisor);
                uSpan[j + n] = uSpan[j + n] + c;
                qhat--;
            }
        }

        if (!q.empty()) {
            if (j >= q.size())
                RELEASE_ASSERT(qhat == 0);
            else
                q[j] = qhat;
        }
    }

    // Determine the actual quotient length: it's m+1 if q[m] is non-zero, otherwise m.
    auto qResult = q;
    if (!q.empty())
        qResult = q.first(std::min(m + 1, q.size()));
    auto rResult = r;
    if (!r.empty())
        rResult = rightShift(r, uSpan, shift);

    return { qResult, rResult };
}

// Z := X, zero-padding Z. Only the digits of X that fit are read.
static void copyZeroPadded(std::span<JSBigInt::Digit> z, std::span<const JSBigInt::Digit> x)
{
    size_t count = std::min(x.size(), z.size());
    memcpySpan(z.first(count), x.first(count));
    std::ranges::fill(z.subspan(count), 0);
}

// Z := X >> shift, zero-padding Z. Z and X may alias.
void JSBigInt::rightShiftZeroPadded(std::span<Digit> z, std::span<const Digit> x, unsigned shift)
{
    auto shifted = rightShift(z, x, shift);
    std::ranges::fill(z.subspan(shifted.size()), 0);
}

// X += y for a single digit y. X must have room for the carry.
static void addDigit(std::span<JSBigInt::Digit> x, JSBigInt::Digit y)
{
    JSBigInt::Digit carry = y;
    for (size_t i = 0; carry; i++) {
        JSBigInt::Digit newCarry = 0;
        x[i] = JSBigInt::digitAdd(x[i], carry, newCarry);
        carry = newCarry;
    }
}

// X -= y for a single digit y <= X.
static void subtractDigit(std::span<JSBigInt::Digit> x, JSBigInt::Digit y)
{
    JSBigInt::Digit borrow = y;
    for (size_t i = 0; borrow; i++) {
        JSBigInt::Digit newBorrow = 0;
        x[i] = JSBigInt::digitSub(x[i], borrow, newBorrow);
        borrow = newBorrow;
    }
}

// Burnikel-Ziegler division, ported from V8 [1].
// Reference: "Fast Recursive Division" by Christoph Burnikel and Joachim Ziegler, found at
// http://cr.yp.to/bib/1998/burnikel.ps
//
// [1]: https://source.chromium.org/chromium/chromium/src/+/main:v8/src/bigint/div-burnikel.cc
static constexpr size_t burnikelThreshold = 57;

// Compares [aHigh, A] with B, returning the sign of the difference.
static int compareWithHighDigit(JSBigInt::Digit aHigh, std::span<const JSBigInt::Digit> a, std::span<const JSBigInt::Digit> b)
{
    b = normalize(b);
    size_t aLength;
    if (!aHigh) {
        a = normalize(a);
        aLength = a.size();
    } else
        aLength = a.size() + 1;
    if (aLength != b.size())
        return aLength < b.size() ? -1 : 1;
    size_t i = aLength;
    if (aHigh) {
        i--;
        if (aHigh != b[i])
            return aHigh < b[i] ? -1 : 1;
    }
    while (i-- > 0) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

// Since the Burnikel-Ziegler method is inherently recursive, we put non-changing data into a
// container object.
class JSBigInt::BurnikelZiegler {
    WTF_MAKE_NONCOPYABLE(BurnikelZiegler);
public:
    BurnikelZiegler(InterruptCheck& interrupt, size_t scratchSpace)
        : m_interrupt(interrupt)
        , m_scratch(scratchSpace >= burnikelThreshold ? scratchSpace : 0)
    {
    }

    void divideBasecase(std::span<Digit> q, std::span<Digit> r, std::span<const Digit>, std::span<const Digit>);
    void d3n2n(std::span<Digit> q, std::span<Digit> r, std::span<const Digit> a1a2, std::span<const Digit> a3, std::span<const Digit>);
    void d2n1n(std::span<Digit> q, std::span<Digit> r, std::span<const Digit>, std::span<const Digit>);

private:
    InterruptCheck& m_interrupt;
    Vector<Digit> m_scratch;
};

void JSBigInt::BurnikelZiegler::divideBasecase(std::span<Digit> q, std::span<Digit> r, std::span<const Digit> a, std::span<const Digit> b)
{
    a = normalize(a);
    b = normalize(b);
    ASSERT(!b.empty());
    auto comparison = compareDigits(a, b);
    if (comparison != ComparisonResult::GreaterThan) {
        std::ranges::fill(q, 0);
        if (comparison == ComparisonResult::Equal) {
            // If A == B, then Q=1, R=0.
            std::ranges::fill(r, 0);
            q[0] = 1;
        } else {
            // If A < B, then Q=0, R=A.
            copyZeroPadded(r, a);
        }
        return;
    }
    if (b.size() == 1) {
        Digit remainder = 0;
        auto quotient = divideSingle(q, remainder, a, b[0]);
        std::ranges::fill(q.subspan(quotient.size()), 0);
        r[0] = remainder;
        std::ranges::fill(r.subspan(1), 0);
        m_interrupt.addWork(a.size());
        return;
    }
    auto [quotient, remainder] = divideSchoolbook(q, r, a, b, &m_interrupt);
    std::ranges::fill(q.subspan(quotient.size()), 0);
    std::ranges::fill(r.subspan(remainder.size()), 0);
}

// Algorithm 2 from the paper. Variable names same as there.
// Returns Q(uotient) and R(emainder) for A/B, with B having two thirds the size of A = [A1, A2, A3].
void JSBigInt::BurnikelZiegler::d3n2n(std::span<Digit> q, std::span<Digit> r, std::span<const Digit> a1a2, std::span<const Digit> a3, std::span<const Digit> b)
{
    ASSERT(!(b.size() & 1));
    size_t n = b.size() / 2;
    ASSERT(a1a2.size() == 2 * n);
    // Actual condition is stricter than length: A < B * 2^(digitBits * n)
    ASSERT(compareDigits(a1a2, b) == ComparisonResult::LessThan);
    ASSERT(a3.size() == n);
    ASSERT(q.size() == n);
    ASSERT(r.size() == 2 * n);
    // 1. Split A into three parts A = [A1, A2, A3] with Ai < 2^(digitBits * n).
    auto a1 = a1a2.subspan(n, n);
    // 2. Split B into two parts B = [B1, B2] with Bi < 2^(digitBits * n).
    auto b1 = b.subspan(n, n);
    auto b2 = b.first(n);
    // 3. Distinguish the cases A1 < B1 or A1 >= B1.
    auto qhat = q;
    auto r1 = r.subspan(n, n);
    Digit r1High = 0;
    if (compareDigits(a1, b1) == ComparisonResult::LessThan) {
        // 3a. If A1 < B1, compute Qhat = floor([A1, A2] / B1) with remainder R1 using algorithm
        //     D2n1n.
        d2n1n(qhat, r1, a1a2, b1);
        if (m_interrupt.interrupted())
            return;
    } else {
        // 3b. If A1 >= B1, set Qhat = 2^(digitBits * n) - 1 and set R1 = [A1, A2] - [B1, 0] + [0, B1]
        std::ranges::fill(qhat, ~static_cast<Digit>(0));
        // Step 1: compute A1 - B1, which can't underflow because of the comparison guarding this
        // else-branch, and always has a one-digit result because of this function's
        // preconditions.
        auto temp = r1;
        subZeroPadded(temp, normalize(a1), normalize(b1));
        auto difference = normalize(std::span<const Digit>(temp));
        ASSERT(difference.size() <= 1);
        if (!difference.empty())
            r1High = difference[0];
        // Step 2: compute A2 + B1.
        auto a2 = a1a2.first(n);
        r1High += addAndReturnCarry(r1, a2, b1);
    }
    // 4. Compute D = Qhat * B2 using (Karatsuba) multiplication.
    auto d = m_scratch.mutableSpan().first(2 * n);
    multiplyZeroPadded(m_interrupt, d, qhat, b2);
    if (m_interrupt.interrupted())
        return;

    // 5. Compute Rhat = R1*2^(digitBits * n) + A3 - D = [R1, A3] - D.
    copyZeroPadded(r.first(n), a3);
    // 6. As long as Rhat < 0, repeat:
    while (compareWithHighDigit(r1High, r, d) < 0) {
        // 6a. Rhat = Rhat + B
        r1High += inplaceAdd(r, b);
        // 6b. Qhat = Qhat - 1
        subtractDigit(qhat, 1);
    }
    // 5. Compute Rhat = R1*2^(digitBits * n) + A3 - D = [R1, A3] - D.
    Digit borrow = inplaceSub(r, d);
    ASSERT_UNUSED(borrow, borrow == r1High);
    ASSERT(compareDigits(r, b) == ComparisonResult::LessThan);
    // 7. Return R = Rhat, Q = Qhat.
}

// Algorithm 1 from the paper. Variable names same as there.
// Returns Q(uotient) and (R)emainder for A/B, with A twice the size of B.
void JSBigInt::BurnikelZiegler::d2n1n(std::span<Digit> q, std::span<Digit> r, std::span<const Digit> a, std::span<const Digit> b)
{
    size_t n = b.size();
    ASSERT(a.size() <= 2 * n);
    // A < B * 2^(digitBits * n)
    ASSERT(compareDigits(clampedSubspan(a, n, n), b) == ComparisonResult::LessThan);
    ASSERT(q.size() == n);
    ASSERT(r.size() == n);
    // 1. If n is odd or smaller than some convenient constant, compute Q and R by school division
    //    and return.
    if ((n & 1) || n < burnikelThreshold)
        return divideBasecase(q, r, a, b);
    // 2. Split A into four parts A = [A1, ..., A4] with Ai < 2^(digitBits * n / 2). Split B into
    //    two parts [B2, B1] with Bi < 2^(digitBits * n / 2).
    auto a1a2 = clampedSubspan(a, n, n);
    auto a3 = clampedSubspan(a, n / 2, n / 2);
    auto a4 = clampedSubspan(a, 0, n / 2);
    // 3. Compute the high part Q1 of floor(A/B) as Q1 = floor([A1, A2, A3] / [B1, B2]) with
    //    remainder R1 = [R11, R12], using algorithm D3n2n.
    auto q1 = q.subspan(n / 2, n / 2);
    Vector<Digit> r1(n);
    d3n2n(q1, r1.mutableSpan(), a1a2, a3, b);
    if (m_interrupt.interrupted())
        return;
    // 4. Compute the low part Q2 of floor(A/B) as Q2 = floor([R11, R12, A4] / [B1, B2]) with
    //    remainder R, using algorithm D3n2n.
    auto q2 = q.first(n / 2);
    d3n2n(q2, r, r1.span(), a4, b);
    // 5. Return Q = [Q1, Q2] and R.
}

// Algorithm 3 from the paper. Variables names same as there.
// Returns Q(uotient) and R(emainder) for A/B (no size restrictions). R is optional, Q is not. Every
// digit of Q and of R is written.
std::tuple<std::span<JSBigInt::Digit>, std::span<JSBigInt::Digit>> JSBigInt::divideBurnikelZiegler(InterruptCheck& interrupt, std::span<Digit> q, std::span<Digit> r, std::span<const Digit> a, std::span<const Digit> b)
{
    RELEASE_ASSERT(a.size() >= b.size());
    RELEASE_ASSERT(r.empty() || r.size() >= b.size());
    RELEASE_ASSERT(q.size() > a.size() - b.size());
    size_t quotientLength = a.size() - b.size() + 1;
    size_t aLength = a.size();
    size_t s = b.size();
    // The requirements are:
    // - n >= s, n as small as possible.
    // - m must be a power of two.
    // 1. Set m = min {2^k | 2^k * burnikelThreshold > s}.
    size_t m = static_cast<size_t>(1) << std::bit_width(s / burnikelThreshold);
    // 2. Set j = roundup(s/m) and n = j * m.
    size_t j = (s + m - 1) / m;
    size_t n = j * m;
    // 3. Set sigma = max{tao | 2^tao * B < 2^(digitBits * n)}.
    unsigned sigma = clz(b[s - 1]);
    size_t digitShift = n - s;
    // 4. Set B = B * 2^sigma to normalize B. Shift A by the same amount.
    // Usage of temp: B[n], Z[2n], Ri[n], Qi[n].
    Vector<Digit> temp(n * 5);
    auto bShifted = temp.mutableSpan().first(n);
    std::ranges::fill(bShifted.first(digitShift), 0);
    auto shiftedDivisor = leftShift(bShifted.subspan(digitShift), b, sigma);
    ASSERT_UNUSED(shiftedDivisor, shiftedDivisor.size() == s);
    b = bShifted;
    // We need an extra digit if A's top digit does not have enough space for the left-shift by
    // {sigma}. Additionally, the top bit of A must be 0 (see "-1" in step 5 below), which combined
    // with B being normalized (i.e. B's top bit is 1) ensures the preconditions of the helper
    // functions.
    size_t extraDigit = clz(a[aLength - 1]) < sigma + 1 ? 1 : 0;
    aLength = a.size() + digitShift + extraDigit;
    Vector<Digit> aShiftedStorage(aLength);
    auto aShifted = aShiftedStorage.mutableSpan();
    std::ranges::fill(aShifted.first(digitShift), 0);
    auto shiftedDividend = leftShift(aShifted.subspan(digitShift), a, sigma);
    // A shift of zero copies a's digits without the carry digit.
    std::ranges::fill(aShifted.subspan(digitShift + shiftedDividend.size()), 0);
    a = aShifted;
    // 5. Set t = min{t >= 2 | A < 2^(digitBits * t * n - 1)}.
    size_t t = std::max<size_t>((aLength + n - 1) / n, 2);
    // 6. Split A conceptually into t blocks.
    // 7. Set Z_(t-2) = [A_(t-1), A_(t-2)].
    size_t zLength = n * 2;
    auto z = temp.mutableSpan().subspan(n, zLength);
    copyZeroPadded(z, clampedSubspan(a, n * (t - 2), zLength));
    // 8. For i from t-2 downto 0 do:
    BurnikelZiegler bz(interrupt, n);
    auto ri = temp.mutableSpan().subspan(3 * n, n);
    {
        // First iteration unrolled and specialized.
        // We might not have n digits at the top of Q, so use temporary storage for Qi...
        auto qi = temp.mutableSpan().subspan(4 * n, n);
        bz.d2n1n(qi, ri, z, b);
        if (interrupt.interrupted())
            return { q, r };
        // ...but there *will* be enough space for any non-zero result digits!
        auto quotientChunk = normalize(std::span<const Digit>(qi));
        auto target = q.subspan(n * (t - 2));
        ASSERT(quotientChunk.size() <= target.size());
        copyZeroPadded(target, quotientChunk);
    }
    // Now loop over any remaining iterations.
    for (size_t i = t - 2; i-- > 0;) {
        // 8b. If i > 0, set Z_(i-1) = [Ri, A_(i-1)].
        // (De-duped with unrolled first iteration, hence reading A_(i).)
        copyZeroPadded(z.subspan(n), ri);
        copyZeroPadded(z.first(n), clampedSubspan(a, n * i, n));
        // 8a. Using algorithm D2n1n compute Qi, Ri such that Zi = B*Qi + Ri.
        auto qi = q.subspan(i * n, n);
        bz.d2n1n(qi, ri, z, b);
        if (interrupt.interrupted())
            return { q, r };
    }
    // 9. Return Q = [Q_(t-2), ..., Q_0] and R = R_0 * 2^(-sigma).
#if ASSERT_ENABLED
    for (size_t i = 0; i < digitShift; i++)
        ASSERT(!ri[i]);
#endif
    if (!r.empty()) {
        auto remainder = normalize(std::span<const Digit>(ri.subspan(digitShift)));
        ASSERT(remainder.size() <= r.size());
        rightShiftZeroPadded(r, remainder, sigma);
        r = r.first(s);
    }
    return { q.first(quotientLength), r };
}

// Barrett division, finding the inverse with Newton's method, ported from V8 [1].
// Reference: "Fast Division of Large Integers" by Karl Hasselström, found at
// https://treskal.com/s/masters-thesis.pdf
//
// Many thanks to Karl Wiberg, k@w5.se, for both writing up an understandable theoretical
// description of the algorithm and privately providing a demo implementation, on which the
// implementation in this file is based.
//
// [1]: https://source.chromium.org/chromium/chromium/src/+/main:v8/src/bigint/div-barrett.cc
#if CPU(REGISTER64)
static constexpr size_t barrettThreshold = 13000;
#else
static constexpr size_t barrettThreshold = 22000;
#endif
static constexpr size_t newtonInversionThreshold = 25;

static constexpr size_t divideBarrettScratchSpace(size_t n) { return n + 2; }
// Local values S and W need "n plus a few" digits; U needs 2*n "plus a few". In all tested cases
// the "few" were either 2 or 3, so give 5 to be safe. S and W are not live at the same time.
static constexpr size_t invertNewtonExtraSpace = 5;
static constexpr size_t invertNewtonScratchSpace(size_t n) { return 3 * n + 2 * invertNewtonExtraSpace; }
static constexpr size_t invertScratchSpace(size_t n) { return n < newtonInversionThreshold ? 2 * n : invertNewtonScratchSpace(n); }

#if ASSERT_ENABLED
static void assertIntegerPartRange(std::span<const JSBigInt::Digit> x, JSBigInt::Digit min, JSBigInt::Digit max)
{
    JSBigInt::Digit integerPart = x.back();
    ASSERT(integerPart >= min);
    ASSERT(integerPart <= max);
}
#else
static void assertIntegerPartRange(std::span<const JSBigInt::Digit>, JSBigInt::Digit, JSBigInt::Digit) { }
#endif

// Z := (the fractional part of) 1/V, via naive division.
// See comments at {invert} and {invertNewton} below for details.
void JSBigInt::invertBasecase(InterruptCheck& interrupt, std::span<Digit> z, std::span<const Digit> v, std::span<Digit> scratch)
{
    ASSERT(z.size() > v.size());
    ASSERT(!v.empty());
    ASSERT(scratch.size() >= 2 * v.size());
    size_t n = v.size();
    auto x = scratch.first(2 * n);
    Digit borrow = 0;
    size_t i = 0;
    for (; i < n; i++)
        x[i] = 0;
    for (; i < 2 * n; i++) {
        Digit newBorrow = 0;
        x[i] = digitSub2(0, v[i - n], borrow, newBorrow);
        borrow = newBorrow;
    }
    ASSERT(borrow == 1);
    // We don't need the remainder.
    std::span<Digit> quotient;
    if (n < burnikelThreshold)
        quotient = std::get<0>(divideSchoolbook(z, { }, x, v, &interrupt));
    else
        quotient = std::get<0>(divideBurnikelZiegler(interrupt, z, { }, x, v));
    std::ranges::fill(z.subspan(quotient.size()), 0);
}

// This is Algorithm 4.2 from the paper.
// Computes the inverse of V, shifted by digitBits * 2 * V.size(), accurate to V.size()+1 digits.
// The V.size() low digits of the result digits will be written to Z, plus there is an implicit
// top digit with value 1.
// Needs invertNewtonScratchSpace(V.size()) of scratch space.
// The result is either correct or off by one (about half the time it is correct, half the time it
// is one too much, and in the corner case where V is minimal and the implicit top digit would
// have to be 2 it is one too little). Barrett's division algorithm can handle that, so we don't
// care.
void JSBigInt::invertNewton(InterruptCheck& interrupt, std::span<Digit> z, std::span<const Digit> v, std::span<Digit> scratch)
{
    const size_t vn = v.size();
    ASSERT(z.size() >= vn);
    ASSERT(scratch.size() >= invertNewtonScratchSpace(vn));
    const size_t sOffset = 0;
    const size_t wOffset = 0; // S and W can share their scratch space.
    const size_t uOffset = vn + invertNewtonExtraSpace;

    // The base case won't work otherwise.
    ASSERT(v.size() >= 3);

    size_t basecasePrecision = std::min<size_t>(newtonInversionThreshold - 1, (vn + 1) / 2);
    // V must have more digits than the basecase.
    ASSERT(v.size() > basecasePrecision);
    ASSERT(v.back() >> (digitBits - 1));

    // Step (1): Setup.
    // Calculate precision required at each step.
    // {k} is the number of fraction bits for the current iteration.
    size_t k = vn * digitBits;
    std::array<size_t, 8 * sizeof(size_t)> targetFractionBits; // "k_i" in the paper.
    unsigned iterations = 0; // "i" in the paper, except inverted to run downwards.
    while (k > basecasePrecision * digitBits) {
        targetFractionBits[iterations++] = k;
        k = (k + 1) / 2;
    }
    // At this point, k <= basecasePrecision * digitBits is the number of fraction bits to use in
    // the base case. {iterations} is one past the highest index in use for targetFractionBits.

    // Step (2): Initial approximation.
    size_t initialDigits = (k + 1 + digitBits - 1) / digitBits;
    auto topPartOfV = v.subspan(vn - initialDigits, initialDigits);
    invertBasecase(interrupt, z, topPartOfV, scratch);
    z[initialDigits] = z[initialDigits] + 1; // Implicit top digit.
    // From now on, we'll keep zLength updated to the part that's already computed.
    size_t zLength = initialDigits + 1;

    // Step (3): Precision doubling loop.
    while (true) {
        assertIntegerPartRange(z.first(zLength), 1, 2);

        // (3b): S = Z^2
        auto s = scratch.subspan(sOffset, 2 * zLength);
        multiplyZeroPadded(interrupt, s, z.first(zLength), z.first(zLength));
        if (interrupt.interrupted())
            return;
        ASSERT(!s.back());
        s = s.first(s.size() - 1); // Top digit of S is unused.
        assertIntegerPartRange(s, 1, 4);

        // (3c): T = V, truncated so that at least 2k+3 fraction bits remain.
        size_t fractionDigits = (2 * k + 3 + digitBits - 1) / digitBits;
        size_t tLength = std::min(v.size(), fractionDigits);
        auto t = v.subspan(v.size() - tLength, tLength);

        // (3d): U = T * S, truncated so that at least 2k+1 fraction bits remain (U has one
        // integer digit, which might be zero).
        fractionDigits = (2 * k + 1 + digitBits - 1) / digitBits;
        auto u = scratch.subspan(uOffset, s.size() + t.size());
        ASSERT(u.size() > fractionDigits);
        multiplyZeroPadded(interrupt, u, s, t);
        if (interrupt.interrupted())
            return;
        u = u.subspan(u.size() - (1 + fractionDigits));
        assertIntegerPartRange(u, 0, 3);

        // (3e): W = 2 * Z, padded with "0" fraction bits so that it has the same number of
        // fraction bits as U.
        ASSERT(u.size() >= zLength);
        auto w = scratch.subspan(wOffset, u.size());
        size_t paddingDigits = u.size() - zLength;
        std::ranges::fill(w.first(paddingDigits), 0);
        auto doubled = leftShift(w.subspan(paddingDigits), z.first(zLength), 1);
        ASSERT_UNUSED(doubled, doubled.size() == zLength);
        assertIntegerPartRange(w, 2, 4);

        // (3f): Z = W - U.
        // This check is '<=' instead of '<' because U's top digit is its integer part, and we want
        // vn fraction digits.
        if (u.size() <= vn) {
            // Normal subtraction.
            // This is not the last iteration.
            ASSERT(iterations > 1);
            zLength = u.size();
            Digit borrow = subtractAndReturnBorrow(z.first(zLength), w, u);
            ASSERT_UNUSED(borrow, !borrow);
            assertIntegerPartRange(z.first(zLength), 1, 2);
        } else {
            // Truncate some least significant digits so that we get vn fraction digits, and
            // compute the integer digit separately.
            // This is the last iteration.
            ASSERT(iterations == 1);
            zLength = vn;
            auto wPart = w.subspan(w.size() - vn - 1, vn);
            auto uPart = u.subspan(u.size() - vn - 1, vn);
            Digit borrow = subtractAndReturnBorrow(z.first(vn), wPart, uPart);
            Digit integerPart = w.back() - u.back() - borrow;
            ASSERT(integerPart == 1 || integerPart == 2);
            if (integerPart == 2) {
                // This is the rare case where the correct result would be 2.0, but since we can't
                // express that by returning only the fractional part with an implicit 1-digit, we
                // have to return [1.]9999... instead.
                std::ranges::fill(z.first(vn), ~static_cast<Digit>(0));
            }
            break;
        }
        // (3g, 3h): Update local variables and loop.
        k = targetFractionBits[--iterations];
    }
}

// Computes the inverse of V, shifted by digitBits * 2 * V.size(), accurate to V.size()+1 digits.
// The V.size() low digits of the result digits will be written to Z, plus there is an implicit
// top digit with value 1.
// (Corner case: if V is minimal, the implicit digit should be 2; in that case we return one less
// than the correct answer. divideBarrett can handle that.)
// Needs invertScratchSpace(V.size()) digits of scratch space.
void JSBigInt::invert(InterruptCheck& interrupt, std::span<Digit> z, std::span<const Digit> v, std::span<Digit> scratch)
{
    ASSERT(z.size() > v.size());
    ASSERT(!v.empty());
    ASSERT(v.back() >> (digitBits - 1));
    ASSERT(scratch.size() >= invertScratchSpace(v.size()));

    size_t vn = v.size();
    if (vn >= newtonInversionThreshold)
        return invertNewton(interrupt, z, v, scratch);
    if (vn == 1) {
        Digit d = v[0];
        Digit dummyRemainder = 0;
        z[0] = digitDiv(~d, ~static_cast<Digit>(0), d, dummyRemainder);
        z[1] = 0;
    } else {
        invertBasecase(interrupt, z, v, scratch);
        if (z[vn] == 1) {
            std::ranges::fill(z.first(vn), ~static_cast<Digit>(0));
            z[vn] = 0;
        }
    }
}

// This is algorithm 3.5 from the paper.
// Computes Q(uotient) and R(emainder) for A/B using I, which is a precomputed approximation of
// 1/B (e.g. with invert() above).
// Needs divideBarrettScratchSpace(A.size()) scratch space.
void JSBigInt::divideBarrett(InterruptCheck& interrupt, std::span<Digit> q, std::span<Digit> r, std::span<const Digit> a, std::span<const Digit> b, std::span<const Digit> inverse, std::span<Digit> scratch)
{
    ASSERT(q.size() > a.size() - b.size());
    ASSERT(r.size() >= b.size());
    ASSERT(a.size() > b.size()); // Careful: This is *not* '>=' !
    ASSERT(a.size() <= 2 * b.size());
    ASSERT(!b.empty());
    ASSERT(b.back() >> (digitBits - 1));
    ASSERT(inverse.size() == a.size() - b.size());
    ASSERT(scratch.size() >= divideBarrettScratchSpace(a.size()));

    auto fullQuotient = q;
    size_t n = b.size();

    // (1): A1 = A with B.size() fewer digits.
    auto a1 = a.subspan(n);
    ASSERT(a1.size() == inverse.size());

    // (2): Q = A1*I with I.size() fewer digits.
    // {inverse} has an implicit high digit with value 1, so we add {A1} to the high part of the
    // multiplication result.
    auto k = scratch.first(2 * inverse.size());
    multiplyZeroPadded(interrupt, k, a1, inverse);
    if (interrupt.interrupted())
        return;
    q = q.first(inverse.size() + 1);
    addZeroPadded(q, k.subspan(inverse.size()), a1);
    // K is no longer used, can reuse {scratch} for P.

    // (3): R = A - B*Q (approximate remainder).
    auto p = scratch.first(a.size() + 1);
    multiplyZeroPadded(interrupt, p, b, q);
    if (interrupt.interrupted())
        return;
    auto remainder = r.first(n);
    Digit borrow = subtractAndReturnBorrow(remainder, a, p.first(n));
    // R may be allocated wider than B, zero out any extra digits if so.
    std::ranges::fill(r.subspan(n), 0);
    Digit rHigh = a[n] - p[n] - borrow;

    // Adjust R and Q so that they become the correct remainder and quotient.
    // The number of iterations is guaranteed to be at most some very small constant, unless the
    // caller gave us a bad approximate quotient.
    if (rHigh >> (digitBits - 1)) {
        // (5b): R < 0, so R += B
        Digit qSub = 0;
        do {
            rHigh += inplaceAdd(remainder, b);
            qSub++;
            ASSERT(qSub <= 5);
        } while (rHigh);
        subtractDigit(q, qSub);
    } else {
        Digit qAdd = 0;
        while (rHigh || greaterThanOrEqual(remainder, b)) {
            // (5c): R >= B, so R -= B
            rHigh -= inplaceSub(remainder, b);
            qAdd++;
            ASSERT(qAdd <= 5);
        }
        addDigit(q, qAdd);
    }
    // (5a): Return.
    std::ranges::fill(fullQuotient.subspan(q.size()), 0);
}

// Computes Q(uotient) and R(emainder) for A/B, using Barrett division. Every digit of Q and of R
// is written.
std::tuple<std::span<JSBigInt::Digit>, std::span<JSBigInt::Digit>> JSBigInt::divideBarrett(InterruptCheck& interrupt, std::span<Digit> q, std::span<Digit> r, std::span<const Digit> a, std::span<const Digit> b)
{
    RELEASE_ASSERT(q.size() > a.size() - b.size() + 1);
    RELEASE_ASSERT(r.size() >= b.size());
    RELEASE_ASSERT(a.size() > b.size()); // Careful: This is *not* '>=' !
    RELEASE_ASSERT(!b.empty());
    size_t quotientLength = a.size() - b.size() + 1;
    size_t remainderLength = b.size();

    // Normalize B, and shift A by the same amount.
    unsigned shift = clz(b.back());
    Vector<Digit> bNormalizedStorage;
    Vector<Digit> aNormalizedStorage;
    if (shift) {
        bNormalizedStorage.resize(b.size());
        auto shifted = leftShift(bNormalizedStorage.mutableSpan(), b, shift);
        ASSERT_UNUSED(shifted, shifted.size() == b.size());
        b = bNormalizedStorage.span();
        // A gains a digit if its top digit has no room for the shift.
        aNormalizedStorage.resize(a.size() + (clz(a.back()) < shift ? 1 : 0));
        auto shiftedDividend = leftShift(aNormalizedStorage.mutableSpan(), a, shift);
        ASSERT_UNUSED(shiftedDividend, shiftedDividend.size() == aNormalizedStorage.size());
        a = aNormalizedStorage.span();
    }

    // The core divideBarrett function above only supports A having at most twice as many digits
    // as B. We generalize this to arbitrary inputs similar to Burnikel-Ziegler division by
    // performing a t-by-1 division of B-sized chunks. It's easy to special-case the situation
    // where we don't need to bother.
    size_t barrettDividendLength = a.size() <= 2 * b.size() ? a.size() : 2 * b.size();
    size_t inverseLength = barrettDividendLength - b.size();
    // +1 is for temporary use by invert().
    Vector<Digit> inverseStorage(inverseLength + 1);
    size_t scratchLength = std::max(invertScratchSpace(inverseLength), divideBarrettScratchSpace(barrettDividendLength));
    Vector<Digit> scratchStorage(scratchLength);
    auto scratch = scratchStorage.mutableSpan();
    invert(interrupt, inverseStorage.mutableSpan(), b.subspan(b.size() - inverseLength, inverseLength), scratch);
    if (interrupt.interrupted())
        return { q, r };
    ASSERT(!inverseStorage[inverseLength]);
    auto inverse = inverseStorage.span().first(inverseLength);
    if (a.size() > 2 * b.size()) {
        // This follows the variable names and and algorithmic steps of divideBurnikelZiegler().
        size_t n = b.size(); // Chunk length.
        // (5): {t} is the number of B-sized chunks of A.
        size_t t = (a.size() + n - 1) / n;
        ASSERT(t >= 3);
        // (6)/(7): Z is used for the current 2-chunk block to be divided by B, initialized to the
        // two topmost chunks of A.
        size_t zLength = n * 2;
        Vector<Digit> zStorage(zLength);
        auto z = zStorage.mutableSpan();
        copyZeroPadded(z, clampedSubspan(a, n * (t - 2), zLength));
        // (8): For i from t-2 downto 0 do
        size_t qiLength = n + 1;
        Vector<Digit> qiStorage(qiLength);
        auto qi = qiStorage.mutableSpan();
        Vector<Digit> riStorage(n);
        auto ri = riStorage.mutableSpan();
        // First iteration unrolled and specialized.
        {
            size_t i = t - 2;
            divideBarrett(interrupt, qi, ri, z, b, inverse, scratch);
            if (interrupt.interrupted())
                return { q, r };
            auto target = q.subspan(n * i);
            // In the first iteration, all qiLength = n + 1 digits may be used.
            copyZeroPadded(target, qi);
#if ASSERT_ENABLED
            for (size_t j = target.size(); j < qiLength; j++)
                ASSERT(!qi[j]);
#endif
        }
        // Now loop over any remaining iterations.
        for (size_t i = t - 2; i-- > 0;) {
            // (8b): If i > 0, set Z_(i-1) = [Ri, A_(i-1)].
            // (De-duped with unrolled first iteration, hence reading A_(i).)
            copyZeroPadded(z.subspan(n), ri);
            copyZeroPadded(z.first(n), clampedSubspan(a, n * i, n));
            // (8a): Compute Qi, Ri such that Zi = B*Qi + Ri.
            divideBarrett(interrupt, qi, ri, z, b, inverse, scratch);
            if (interrupt.interrupted())
                return { q, r };
            ASSERT(!qi[qiLength - 1]);
            // (9): Return Q = [Q_(t-2), ..., Q_0]...
            copyZeroPadded(q.subspan(n * i, n), qi);
        }
        auto remainder = normalize(std::span<const Digit>(ri));
        ASSERT(remainder.size() <= r.size());
        // (9): ...and R = R_0 * 2^(-leading_zeros).
        rightShiftZeroPadded(r, remainder, shift);
    } else {
        divideBarrett(interrupt, q, r, a, b, inverse, scratch);
        if (interrupt.interrupted())
            return { q, r };
        rightShiftZeroPadded(r, r, shift);
    }
    return { q.first(quotientLength), r.first(remainderLength) };
}

// The number of quotient digits a caller of divideDigitsInto has to provide.
size_t JSBigInt::quotientLength(std::span<const Digit> a, std::span<const Digit> b)
{
    ASSERT(a.size() >= b.size());
    size_t length = a.size() - b.size() + 1;
    // Barrett division normalizes the dividend itself, which can grow it by a digit.
    if (b.size() >= barrettThreshold)
        length++;
    return length;
}

// Computes Q(uotient) and R(emainder) for A/B with the algorithm suited to the operands' sizes.
// Either Q or R may be empty; Q, when present, must have quotientLength(a, b) digits. The returned
// spans are not normalized.
//
// The recursive algorithms pay for their block structure in full even when the quotient is short,
// whereas the schoolbook cost is the quotient length times the divisor length, so both the
// divisor and the quotient have to be long enough. V8 only gates on the divisor; measured here, a
// quotient under 57 digits loses on every divisor size, by a factor of 4 at one digit.
static bool shouldUseSchoolbookDivision(size_t dividendSize, size_t divisorSize)
{
    return divisorSize < burnikelThreshold || dividendSize - divisorSize < burnikelThreshold;
}

std::tuple<std::span<JSBigInt::Digit>, std::span<JSBigInt::Digit>> JSBigInt::divideDigitsInto(InterruptCheck& interrupt, std::span<Digit> q, std::span<Digit> r, std::span<const Digit> a, std::span<const Digit> b)
{
    ASSERT(b.size() >= 2);
    ASSERT(a.size() >= b.size());
    ASSERT(q.empty() || q.size() >= quotientLength(a, b));
    ASSERT(r.empty() || r.size() >= b.size());
    if (shouldUseSchoolbookDivision(a.size(), b.size()))
        return divideSchoolbook(q, r, a, b, &interrupt);
    if (b.size() < barrettThreshold) {
        if (!q.empty())
            return divideBurnikelZiegler(interrupt, q, r, a, b);
        Vector<Digit> quotientStorage(quotientLength(a, b));
        auto [quotient, remainder] = divideBurnikelZiegler(interrupt, quotientStorage.mutableSpan(), r, a, b);
        return { { }, remainder };
    }
    if (q.empty()) {
        Vector<Digit> quotientStorage(quotientLength(a, b));
        auto [quotient, remainder] = divideBarrett(interrupt, quotientStorage.mutableSpan(), r, a, b);
        return { { }, remainder };
    }
    if (r.empty()) {
        Vector<Digit> remainderStorage(b.size());
        auto [quotient, remainder] = divideBarrett(interrupt, q, remainderStorage.mutableSpan(), a, b);
        return { quotient, { } };
    }
    return divideBarrett(interrupt, q, r, a, b);
}

static ALWAYS_INLINE JSBigInt::Digit estimateQhat(std::span<const JSBigInt::Digit> a, std::span<const JSBigInt::Digit> b)
{
    ASSERT(a.size() == b.size());
    const size_t n = a.size();
    ASSERT(n > 1); // one digit case is already handled via divideSingle.
    ASSERT(b.back() != 0); // b should be normalized
    ASSERT(a.back() > b.back());

    constexpr auto digitBits = JSBigInt::digitBits;

    // a.back() > b.back(), so a > b and quotient is at least 1.
    // Since a and b have the same number of digits, the quotient fits in one digit.
    //
    // We estimate qhat from Algorithm D by normalizing only the top 2-3 digits (no vector allocation),
    // then verify by computing qhat * b and comparing with a.

    unsigned shift = clz(b.back());

    // Compute normalized top digits inline.
    // vn1 = normalized b[n-1]
    // vn2 = normalized b[n-2]
    // un = overflow from normalizing a (high digit after left shift)
    // un1 = normalized a[n-1]
    // un2 = normalized a[n-2]
    JSBigInt::Digit vn1, vn2, un, un1, un2;

    if (shift == 0) {
        vn1 = b[n - 1];
        vn2 = b[n - 2];
        un = 0;
        un1 = a[n - 1];
        un2 = a[n - 2];
    } else {
        // Left-shift by 'shift' bits to normalize
        vn1 = (b[n - 1] << shift) | (b[n - 2] >> (digitBits - shift));
        vn2 = (b[n - 2] << shift) | (n >= 3 ? (b[n - 3] >> (digitBits - shift)) : 0);
        un = a[n - 1] >> (digitBits - shift);
        un1 = (a[n - 1] << shift) | (a[n - 2] >> (digitBits - shift));
        un2 = (a[n - 2] << shift) | (n >= 3 ? (a[n - 3] >> (digitBits - shift)) : 0);
    }

    // Estimate quotient using the normalized top digits.
    // Since a and b have the same number of digits with a.back() > b.back(),
    // after normalization un < vn1 is guaranteed:
    // - vn1 has its MSB set, so vn1 >= 2^(digitBits-1)
    // - un = a[n-1] >> (digitBits - shift), which is at most 2^(digitBits-1) - 1
    ASSERT(un < vn1);

    JSBigInt::Digit rhat = 0;
    JSBigInt::Digit qhat = JSBigInt::digitDiv(un, un1, vn1, rhat);

    // Refine qhat using the second most significant digit of divisor.
    while (JSBigInt::productGreaterThan(qhat, vn2, rhat, un2)) {
        qhat--;
        JSBigInt::Digit prevRhat = rhat;
        rhat += vn1;
        if (rhat < prevRhat)
            break;
    }

    return qhat;
}

JSBigInt::Digit JSBigInt::divideSameSize(std::span<const Digit> a, std::span<const Digit> b)
{
    RELEASE_ASSERT(a.size() == b.size());
    const size_t n = a.size();
    RELEASE_ASSERT(n >= 2); // Use divideSingle otherwise.
    ASSERT(b.back() != 0); // b should be normalized

    // absoluteCompare ensures that a > b.

    // MSB is the same. Thus result is 1 or 0.
    if (a.back() == b.back())
        return 1; // a >= b

    Digit qhat = estimateQhat(a, b);

    // Verify qhat by computing qhat * b and comparing with a inline.
    // If qhat * b > a, decrement qhat.
    Digit mulCarry = 0;
    Digit subBorrow = 0;
    for (size_t i = 0; i < n; i++) {
        // Compute qhat * b[i] + mulCarry
        auto [low, high] = digitMul(qhat, b[i]);
        Digit product = low + mulCarry;
        mulCarry = high + (product < low ? 1 : 0);

        // Subtract product from a[i] to check if qhat * b > a
        Digit borrowOut = 0;
        digitSub2(a[i], product, subBorrow, borrowOut);
        subBorrow = borrowOut;
    }

    // If there's overflow from multiplication or borrow from subtraction,
    // qhat * b > a, so decrement qhat.
    if (mulCarry || subBorrow)
        qhat--;

    return qhat;
}

std::span<JSBigInt::Digit> JSBigInt::remainderSameSize(std::span<Digit> r, std::span<const Digit> a, std::span<const Digit> b)
{
    RELEASE_ASSERT(a.size() == b.size());
    const size_t n = a.size();
    RELEASE_ASSERT(n >= 2); // Use divideSingle otherwise.
    ASSERT(b.back() != 0); // b should be normalized
    ASSERT(r.size() >= n);

    // absoluteCompare ensures that a > b.

    // a.back() == b.back(): quotient is 0 or 1
    if (a.back() == b.back())
        return subSchoolbook(a, b, r);

    Digit qhat = estimateQhat(a, b);

    // Compute remainder = a - qhat * b inline without allocating a vector.
    // We compute qhat * b and subtract from a in a single pass.
    Digit mulCarry = 0;
    Digit subBorrow = 0;
    for (size_t i = 0; i < n; i++) {
        // Compute qhat * b[i] + mulCarry
        auto [low, high] = digitMul(qhat, b[i]);
        Digit product = low + mulCarry;
        mulCarry = high + (product < low ? 1 : 0);

        // Compute r[i] = a[i] - product - subBorrow
        Digit borrowOut = 0;
        r[i] = digitSub2(a[i], product, subBorrow, borrowOut);
        subBorrow = borrowOut;
    }

    // If there's overflow from multiplication or borrow from subtraction,
    // qhat was too large, add back b.
    if (mulCarry || subBorrow)
        inplaceAdd(r.first(n), b);

    return r.first(n);
}

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::divideImpl(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    // 1. If y is 0n, throw a RangeError exception.
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (y.isZero()) {
        throwRangeError(globalObject, scope, "0 is an invalid divisor value."_s);
        return nullptr;
    }

    // 2. Let quotient be the mathematical value of x divided by y.
    // 3. Return a BigInt representing quotient rounded towards 0 to the next
    //    integral value.
    bool resultSign = x.sign() != y.sign();
    switch (absoluteCompare(x, y)) {
    case ComparisonResult::LessThan: {
        RELEASE_AND_RETURN(scope, zeroImpl(vm));
    }
    case ComparisonResult::Equal: {
        RELEASE_AND_RETURN(scope, createFrom(globalObject, vm, resultSign ? -1 : 1));
    }
    case ComparisonResult::GreaterThan:
    case ComparisonResult::Undefined:
        break;
    }

    auto xSpan = x.digits();
    auto ySpan = y.digits();
    size_t qLength = xSpan.size() - ySpan.size() + 1;
    if (ySpan.size() == 1) {
        Digit divisor = ySpan[0];
        if (divisor == 1) {
            if (resultSign == x.sign())
                return JSBigInt::ImplResult { x };
            RELEASE_AND_RETURN(scope, JSBigInt::unaryMinusImpl(globalObject, x));
        }

        Vector<Digit, 16> q(qLength);
        Digit remainder;
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, resultSign, divideSingle(q.mutableSpan(), remainder, xSpan, divisor)));
    }

    if (xSpan.size() == ySpan.size()) {
        auto quotientDigit = divideSameSize(xSpan, ySpan);
        if (!quotientDigit)
            RELEASE_AND_RETURN(scope, zeroImpl(vm));

        auto* quotient = createWithLength(globalObject, 1);
        RETURN_IF_EXCEPTION(scope, nullptr);

        quotient->setDigit(0, quotientDigit);
        quotient->setSign(resultSign);
        return quotient;
    }

    Vector<Digit, 16> q(quotientLength(xSpan, ySpan));
    InterruptCheck interrupt(&vm);
    auto [qSpan, rSpan] = divideDigitsInto(interrupt, q.mutableSpan(), { }, xSpan, ySpan);
    if (interrupt.interrupted()) [[unlikely]] {
        RELEASE_ASSERT(scope.exception());
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, resultSign, qSpan));
}

JSValue JSBigInt::divide(JSGlobalObject* globalObject, JSBigInt* x, JSBigInt* y)
{
    return tryConvertToBigInt32(divideImpl(globalObject, HeapBigIntImpl { x }, HeapBigIntImpl { y }));
}
#if USE(BIGINT32)
JSValue JSBigInt::divide(JSGlobalObject* globalObject, JSBigInt* x, int32_t y)
{
    return tryConvertToBigInt32(divideImpl(globalObject, HeapBigIntImpl { x }, Int32BigIntImpl { y }));
}
JSValue JSBigInt::divide(JSGlobalObject* globalObject, int32_t x, JSBigInt* y)
{
    return tryConvertToBigInt32(divideImpl(globalObject, Int32BigIntImpl { x }, HeapBigIntImpl { y }));
}
#endif

template <typename BigIntImpl>
JSBigInt* JSBigInt::copy(JSGlobalObject* globalObject, BigIntImpl x)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(!x.isZero());

    JSBigInt* result = createWithLength(globalObject, x.length());
    RETURN_IF_EXCEPTION(scope, nullptr);
    memcpySpan(result->digits(), x.digits());
    result->setSign(x.sign());
    return result;
}

template <typename BigIntImpl>
JSBigInt::ImplResult JSBigInt::unaryMinusImpl(JSGlobalObject* globalObject, BigIntImpl x)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (x.isZero())
        RELEASE_AND_RETURN(scope, zeroImpl(vm));

    JSBigInt* result = copy(globalObject, x);
    RETURN_IF_EXCEPTION(scope, nullptr);

    result->setSign(!x.sign());
    return result;
}

JSValue JSBigInt::unaryMinus(JSGlobalObject* globalObject, JSBigInt* x)
{
    return tryConvertToBigInt32(unaryMinusImpl(globalObject, HeapBigIntImpl { x }));
}

JSBigInt::ComparisonResult JSBigInt::compareDigits(std::span<const Digit> x, std::span<const Digit> y)
{
    x = normalize(x);
    y = normalize(y);
    if (x.size() != y.size())
        return x.size() < y.size() ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;
    for (size_t i = x.size(); i-- > 0;) {
        if (x[i] != y[i])
            return x[i] < y[i] ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;
    }
    return ComparisonResult::Equal;
}

std::span<JSBigInt::Digit> JSBigInt::addDigits(std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    x = normalize(x);
    y = normalize(y);
    if (x.size() < y.size())
        std::swap(x, y);
    RELEASE_ASSERT(result.size() >= x.size() + 1);
    return normalize(addDigitsInto(x, y, result));
}

std::span<JSBigInt::Digit> JSBigInt::multiplyDigits(InterruptCheck& interrupt, std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    x = normalize(x);
    y = normalize(y);
    if (x.empty() || y.empty())
        return { };
    if (x.size() < y.size())
        std::swap(x, y);
    RELEASE_ASSERT(result.size() >= x.size() + y.size());
    return normalize(multiplyDigitsInto(interrupt, x, y, result));
}

std::span<JSBigInt::Digit> JSBigInt::divideDigits(InterruptCheck& interrupt, std::span<Digit> quotient, std::span<const Digit> x, std::span<const Digit> y)
{
    x = normalize(x);
    y = normalize(y);
    RELEASE_ASSERT(!y.empty());

    auto comparisonResult = compareDigits(x, y);
    if (comparisonResult == ComparisonResult::LessThan)
        return { };

    RELEASE_ASSERT(quotient.size() >= x.size());
    if (comparisonResult == ComparisonResult::Equal) {
        quotient[0] = 1;
        return quotient.first(1);
    }

    // x > y, thus x.size() >= y.size().
    if (y.size() == 1) {
        Digit remainder;
        return normalize(divideSingle(quotient, remainder, x, y[0]));
    }

    if (x.size() == y.size()) {
        auto quotientDigit = divideSameSize(x, y);
        if (!quotientDigit)
            return { };
        quotient[0] = quotientDigit;
        return quotient.first(1);
    }

    auto [quotientSpan, remainderSpan] = divideDigitsInto(interrupt, quotient, { }, x, y);
    return normalize(quotientSpan);
}

std::span<JSBigInt::Digit> JSBigInt::oneShiftedLeft(std::span<Digit> result, unsigned bitIndex)
{
    unsigned digitIndex = bitIndex / digitBits;
    RELEASE_ASSERT(result.size() >= digitIndex + 1);
    result = result.first(digitIndex + 1);
    zeroSpan(result);
    result[digitIndex] = static_cast<Digit>(1) << (bitIndex % digitBits);
    return result;
}

// https://tc39.es/proposal-bigint-math/#sec-bigint.sqrt
JSValue JSBigInt::sqrt(JSGlobalObject* globalObject, JSBigInt* bigInt)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(!bigInt->sign());

    if (bigInt->isZero())
        RELEASE_AND_RETURN(scope, bigInt);

    auto value = bigInt->digits();
    Vector<Digit, 16> resultStorage(value.size() + 2);
    Vector<Digit, 16> quotientStorage(value.size() + 2);
    Vector<Digit, 16> sumStorage(value.size() + 2);
    Vector<Digit, 16> nextStorage(value.size() + 2);

    // 2^floor(floor(log2(value)) / 2)
    auto result = oneShiftedLeft(resultStorage.mutableSpan(), (bigInt->bitLength() - 1) >> 1);
    InterruptCheck interrupt(&vm);
    for (size_t iteration = 0; ; ++iteration) {
        // result = ((value / result) + result) >> 1
        auto quotient = divideDigits(interrupt, quotientStorage.mutableSpan(), value, result);
        RETURN_IF_EXCEPTION(scope, { });
        auto sum = addDigits(quotient, result, sumStorage.mutableSpan());
        auto next = normalize(rightShift(nextStorage.mutableSpan(), sum, 1));
        if (iteration) {
            auto comparisonResult = compareDigits(next, result);
            if (comparisonResult == ComparisonResult::Equal || comparisonResult == ComparisonResult::GreaterThan)
                break;
        }

        result = spanCopy(resultStorage.mutableSpan(), next);
    }

    RELEASE_AND_RETURN(scope, tryConvertToBigInt32(tryCreateFromImpl(globalObject, vm, false, result)));
}

// https://tc39.es/proposal-bigint-math/#sec-bigint.cbrt
JSValue JSBigInt::cbrt(JSGlobalObject* globalObject, JSBigInt* bigInt)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (bigInt->isZero())
        RELEASE_AND_RETURN(scope, bigInt);

    constexpr std::array<Digit, 1> three = { 3 };

    bool sign = bigInt->sign();
    auto value = bigInt->digits();
    Vector<Digit, 16> resultStorage(value.size() + 2);
    Vector<Digit, 16> squaredStorage(value.size() + 2);
    Vector<Digit, 16> quotientStorage(value.size() + 2);
    Vector<Digit, 16> doubledStorage(value.size() + 2);
    Vector<Digit, 16> sumStorage(value.size() + 2);
    Vector<Digit, 16> nextStorage(value.size() + 2);

    // 2^floor(floor(log2(value)) / 3)
    auto result = oneShiftedLeft(resultStorage.mutableSpan(), (bigInt->bitLength() - 1) / 3);
    InterruptCheck interrupt(&vm);
    for (size_t iteration = 0; ; ++iteration) {
        // result = ((2 * result) + (value / (result * result))) / 3
        auto resultSquared = multiplyDigits(interrupt, result, result, squaredStorage.mutableSpan());
        RETURN_IF_EXCEPTION(scope, { });
        auto quotient = divideDigits(interrupt, quotientStorage.mutableSpan(), value, resultSquared);
        RETURN_IF_EXCEPTION(scope, { });
        auto doubledResult = normalize(leftShift(doubledStorage.mutableSpan(), result, 1));
        auto sum = addDigits(doubledResult, quotient, sumStorage.mutableSpan());
        auto next = divideDigits(interrupt, nextStorage.mutableSpan(), sum, three);
        if (iteration) {
            auto comparisonResult = compareDigits(next, result);
            if (comparisonResult == ComparisonResult::Equal || comparisonResult == ComparisonResult::GreaterThan)
                break;
        }

        result = spanCopy(resultStorage.mutableSpan(), next);
    }

    RELEASE_AND_RETURN(scope, tryConvertToBigInt32(tryCreateFromImpl(globalObject, vm, sign, result)));
}

// Compute the multiplicative inverse Inv ≈ floor(2^(2n*digitBits) / B) for cached modulo.
// Given divisor B with n digits, the inverse has n+1 digits.
// Uses V8's bit-negation trick to avoid a (2n+1)-digit dividend:
//   A = ~(B << n) ≈ 2^(2n) - B*2^n - 1, then Inv = A/B + 2^n (undo the subtraction).
//
// This is computing I in Algorithm 2.5 in the following reference.
// R. P. Brent and P. Zimmermann, Modern Computer Arithmetic. Cambridge, U.K.: Cambridge University Press, 2010.
void JSBigInt::cachedModMakeInverse(VM& vm, std::span<const Digit> b)
{
    size_t n = b.size();
    ASSERT(n >= 2 && n <= maxCachedModDivisorSize);

    size_t invLen = n + 1;
    vm.m_bigIntCachedInverse.resize(invLen);

    // Construct A (2n digits) using bit-negation trick:
    // A[0..n-1] = ~0 (all 1-bits), A[n..2n-1] = ~B[i-n]
    Vector<Digit, 2 * maxCachedModDivisorSize> a(2 * n);
    size_t i = 0;
    for (; i < n; i++)
        a[i] = ~static_cast<Digit>(0);
    for (; i < 2 * n; i++)
        a[i] = ~b[i - n];

    // Inv = A / B. Since A has 2n digits and B has n digits,
    // quotient has at most n+1 digits (which is invLen).
    auto inv = vm.m_bigIntCachedInverse.mutableSpan();
    divideSchoolbook(inv, { }, a.span(), b);

    // Undo the bit-negation: add 1 to the upper part (starting at digit n).
    // This corresponds to adding back 2^n that was subtracted by the trick.
    RELEASE_ASSERT(inv.size() == invLen);
    {
        Digit carry = 0;
        inv[n] = digitAdd(inv[n], 1, carry);
        ASSERT(!carry);
    }

    // Optionally add 1 to the whole inverse to improve convergence of the
    // corrective loop in cachedMod. But don't do it when there's a risk of overflow.
    if (inv[0] != ~static_cast<Digit>(0) || inv[invLen - 1] != ~static_cast<Digit>(0)) {
        Digit carry = 0;
        inv[0] = digitAdd(inv[0], 1, carry);
        for (size_t j = 1; j < invLen && carry; j++) {
            Digit c = 0;
            inv[j] = digitAdd(inv[j], carry, c);
            carry = c;
        }
    }
}

// Reduction factor for divisors close to a power of the digit base. With n = b.size() and
// T = 2^(n * digitBits), returns C = T mod B when T = q * B + C has a single-digit C and
// q <= maxFoldQuotient, and 0 otherwise.
JSBigInt::Digit JSBigInt::cachedModFoldFactor(std::span<const Digit> b)
{
    // We would like to prepare for Crandall reduction, which is efficient when the divisor `b` is a
    // pseudo-Mersenne number 2^k - c with a small c. A Mersenne number is 2^k - 1, like 0xff.
    // Note the fold factor returned here is c << (n * digitBits - k) rather than c itself, since T
    // sits at the next whole digit boundary at or above b.
    // Efficiency needs the factor to be a single digit, which keeps each fold one multiply, and q
    // to be small, which is what bounds the corrective loop.
    size_t n = b.size();
    RELEASE_ASSERT(n >= 2 && n <= maxCachedModDivisorSize);

    // T is 1 followed by n zero digits, so it needs n + 1 digits. The quotient of an (n + 1)-digit
    // value by an n-digit one needs at most two digits, and the remainder at most n.
    std::array<Digit, maxCachedModDivisorSize + 1> t;
    for (size_t i = 0; i < n; ++i)
        t[i] = 0;
    t[n] = 1;

    std::array<Digit, 2> q;
    std::array<Digit, maxCachedModDivisorSize> r;
    // Only the returned spans are meaningful, and neither is guaranteed to have its leading zero
    // digits trimmed, so normalize before judging their widths.
    auto [qSpanRaw, rSpanRaw] = divideSchoolbook(std::span { q }, std::span { r }.first(n), std::span { t }.first(n + 1), b);
    auto qSpan = normalize(qSpanRaw);
    auto rSpan = normalize(rSpanRaw);

    // A divisor B of n digits qualifies for the folding reduction when T = 2^(n * digitBits)
    // satisfies T = q * B + C with C a single digit and q no larger than this bound.
    //
    // The two conditions do different jobs. C fitting one digit is what keeps each fold a
    // single-digit multiply and caps the carry out of a fold at one digit, which is what makes two
    // folds always sufficient. q is independent of that and sets how far the twice-folded value can
    // still exceed B, so it alone decides how many times the corrective loop subtracts.
    //
    // Cost is therefore flat in C but linear in q, and it crosses the multiplicative inverse path
    // it replaces at roughly q = 12 for a 4-digit divisor. Divisors with a single-digit C and a
    // large q do exist (2^(n * digitBits - k) - 1 has C = 2^k and q = 2^k), so the bound is real
    // rather than defensive. It sits well below the crossover because the moduli this path exists
    // for cluster at the bottom of the range: the ed25519 and secp256k1 field primes give q of 2
    // and 1, and nothing observed needs more.
    constexpr Digit maxFoldQuotient = 4;
    if (qSpan.size() != 1 || qSpan[0] > maxFoldQuotient)
        return 0;

    // The fold stays a single-digit multiply only when the remainder is one digit. An empty
    // remainder would mean B divides T exactly, so B is a power of two, which the shift paths
    // handle and which would collide with the not-qualifying sentinel anyway.
    if (rSpan.size() != 1)
        return 0;

    return rSpan[0];
}

// Subtract b from the value high * 2^(n * digitBits) + r once, if that value is at least b, and
// return the new high digit. The difference is always formed and then selected in or discarded, so
// no branch depends on the comparison; the borrow out of the low n digits answers it, since a borrow
// is covered exactly when high is non-zero.
template<typename RSpan, typename BSpan>
ALWAYS_INLINE JSBigInt::Digit JSBigInt::reduceOnce(RSpan r, BSpan b, Digit high)
{
    constexpr size_t capacity = BSpan::extent == std::dynamic_extent ? maxCachedModDivisorSize : BSpan::extent;
    size_t n = b.size();
    ASSERT(n <= capacity && r.size() >= n);

    std::array<Digit, capacity> difference;
    Digit borrow = 0;
    for (size_t i = 0; i < n; ++i) {
        Digit borrowOut = 0;
        difference[i] = digitSub2(r[i], b[i], borrow, borrowOut);
        borrow = borrowOut;
    }

    bool subtract = high || !borrow;
    for (size_t i = 0; i < n; ++i)
        r[i] = subtract ? difference[i] : r[i];
    return subtract ? high - borrow : high;
}

// This is Crandall reduction implementation. When factor is not appropriate for efficiency, we use
// Barrett reduction instead.
//
// R = A mod B for a divisor whose reduction factor C = 2^(n * digitBits) mod B is a single digit.
//
// Splitting A into low and high halves of n digits gives A = lo + hi * 2^(n * digitBits), and
// since 2^(n * digitBits) is congruent to C, that is congruent to lo + hi * C. Each such fold is a
// row of single-digit multiplies.
//
// Two folds always suffice. Write D for the digit base. The first fold's running carry never
// exceeds C: a column computes a[i] + a[n + i] * C + carry, so if the incoming carry is at most C
// the total is at most (D - 1) + (D - 1) * C + C = C * D + D - 1, whose high half is again at most
// C. The carry starts at zero, so one digit above the low n always holds it. Folding that digit
// back multiplies it by C once more, adding less than D^2 to a value below D^n, so with n >= 2 the
// final carry out is at most 1 and the residue stays below 2 * D^n. Since B = (D^n - C) / q, that
// is roughly 2 * q * B, so the corrective loop runs a bounded number of times given the q cap the
// factor check enforces.
//
// This wants a single-digit C, which is the Pseudo-Mersenne shape 2^k - c for a small c.
//
// The span extents are template parameters so that one body serves both the size-specialized and
// the size-agnostic callers. When they are static the sizes are compile-time constants, so the fold
// loop unrolls and the per-column bound arithmetic folds away; when they are dynamic the same source
// keeps its loops.
template<typename RSpan, typename ASpan, typename BSpan>
ALWAYS_INLINE void JSBigInt::cachedModFoldImpl(RSpan r, ASpan a, BSpan b, Digit c)
{
    size_t n = b.size();

    // First fold: r = a[0..n-1] + a[n..a.size()-1] * c, keeping the carry out separately.
    Digit carry = 0;
    for (size_t i = 0; i < n; ++i) {
        Digit high = 0;
        Digit low = a[i];
        if (n + i < a.size()) {
            auto [productLow, productHigh] = digitMul(a[n + i], c);
            high = productHigh;
            Digit addCarry = 0;
            low = digitAdd(low, productLow, addCarry);
            high += addCarry;
        }
        Digit sumCarry = 0;
        r[i] = digitAdd(low, carry, sumCarry);
        carry = high + sumCarry;
    }

    // Second fold: the single digit above the low n is worth c times its value.
    if (carry) {
        auto [productLow, productHigh] = digitMul(carry, c);
        Digit addCarry = 0;
        r[0] = digitAdd(r[0], productLow, addCarry);
        Digit propagate = productHigh + addCarry;
        for (size_t i = 1; i < n && propagate; ++i) {
            Digit nextCarry = 0;
            r[i] = digitAdd(r[i], propagate, nextCarry);
            propagate = nextCarry;
        }
        carry = propagate;
    }

    // Peel the first corrective subtraction off as a branch-free one. With q at least two the loop
    // below runs zero or one time in a near-even split, so its branch mispredicts about half the
    // time; with q one no subtraction is ever needed, the branch always predicts, and the select
    // would be pure overhead. q is one exactly when b is more than half of 2^(n * digitBits), that
    // is when b's top bit is set. Static extents only: the select unrolls and costs a fixed amount
    // there, whereas for a large dynamic n it costs more than the branch it replaces.
    if constexpr (BSpan::extent != std::dynamic_extent) {
        if (!(b.back() >> (digitBits - 1)))
            carry = reduceOnce(r, b, carry);
    }
    while (carry || greaterThanOrEqual(r, b))
        carry -= inplaceSub(r, b);
}

template<size_t N, size_t ASize>
ALWAYS_INLINE void JSBigInt::cachedModFoldFixed(std::span<Digit, N> r, std::span<const Digit, ASize> a, std::span<const Digit, N> b, Digit c)
{
    static_assert(N >= 2);
    static_assert(ASize >= N && ASize <= 2 * N);

    cachedModFoldImpl(r, a, b, c);
}

void JSBigInt::cachedModFold(std::span<Digit> r, std::span<const Digit> a, std::span<const Digit> b, Digit c)
{
    size_t n = b.size();
    // A one-digit divisor would leave the second fold's carry unpropagated and turn the corrective
    // loop into a walk over the whole digit range, so fail here rather than spin.
    RELEASE_ASSERT(n >= 2);
    RELEASE_ASSERT(r.size() == n);
    RELEASE_ASSERT(a.size() >= n && a.size() <= 2 * n);

    cachedModFoldImpl(r, a, b, c);
}

// Compile-time-specialized form of cachedMod for a divisor of exactly N digits and a dividend of
// exactly ASize digits. Every span size is static, so the special multiplies unroll and the
// scratch buffer becomes a stack array with no zeroing. The arithmetic is identical to cachedMod;
// see the commentary there for the algorithm and the error bound the corrective loop relies on.
template<size_t N, size_t ASize>
ALWAYS_INLINE void JSBigInt::cachedModFixed(VM& vm, std::span<Digit, N> r, std::span<const Digit, ASize> a, std::span<const Digit, N> b)
{
    static_assert(N >= 2);
    static_assert(ASize >= N && ASize <= 2 * N);

    constexpr size_t invSize = N + 1;
    constexpr size_t startPos = 2 * N - 2;
    constexpr size_t scratchSize = ASize + invSize;

    auto inv = vm.m_bigIntCachedInverse.span().first<invSize>();

    // Step 1: high digits of A * Inv. Only digits from startPos up are computed, so the low part
    // of scratch is left untouched here and is overwritten by step 3 before it is read.
    std::array<Digit, scratchSize> scratch;
    if constexpr (ASize >= invSize)
        multiplySpecialHighFixed<ASize, invSize, startPos>(a, inv, std::span<Digit, scratchSize> { scratch });
    else
        multiplySpecialHighFixed<invSize, ASize, startPos>(inv, a, std::span<Digit, scratchSize> { scratch });

    // Step 2: estimated quotient Q sits at digit 2n of the product.
    // Copy it out before step 3 overwrites the low part of scratch, so the compiler can keep the
    // digits in registers instead of reloading them around the aliasing stores.
    constexpr size_t qSize = scratchSize - 2 * N;
    std::array<Digit, qSize> q;
    for (size_t i = 0; i < qSize; ++i)
        q[i] = scratch[2 * N + i];

    // Step 3: product_low = B * Q, low n+1 digits only.
    auto productLow = std::span<Digit, scratchSize> { scratch }.template first<invSize>();
    multiplySpecialLowFixed(b, std::span<const Digit, qSize> { q }, productLow);

    // Step 4: R = A[0..n-1] - product_low[0..n-1].
    Digit borrow = 0;
    for (size_t i = 0; i < N; ++i) {
        Digit borrowOut = 0;
        r[i] = digitSub2(a[i], productLow[i], borrow, borrowOut);
        borrow = borrowOut;
    }

    // Track the extra digit: r_high = A[n] - product_low[n] - borrow.
    Digit an = 0;
    if constexpr (ASize > N)
        an = a[N];
    Digit rHigh = an - productLow[N] - borrow;

    // Step 5: corrective loop using the sign bit of r_high.
    constexpr Digit signBit = static_cast<Digit>(1) << (digitBits - 1);
    if (rHigh & signBit) {
        do {
            rHigh += inplaceAdd(r, b);
        } while (rHigh);
    } else {
        while (rHigh || greaterThanOrEqual(r, b))
            rHigh -= inplaceSub(r, b);
    }
}

// Cached modulo: R = A mod B, using precomputed inverse Inv.
// A must have between n and 2n digits (where n = B.size()).
// Returns the normalized result span within r.
// R. P. Brent and P. Zimmermann, Modern Computer Arithmetic. Cambridge, U.K.: Cambridge University Press, 2010.
std::span<const JSBigInt::Digit> JSBigInt::cachedMod(VM& vm, std::span<Digit> r, std::span<const Digit> a, std::span<const Digit> b)
{
    size_t n = b.size();
    ASSERT(n >= 2 && n <= maxCachedModDivisorSize);
    ASSERT(a.size() >= n && a.size() <= 2 * n);
    ASSERT(r.size() >= n);

    r = r.first(n);

    // Divisors close to a power of the digit base reduce by folding the high half down with a single-digit multiply.
    if (Digit foldFactor = vm.m_bigIntFoldFactor) {
        if (n <= maxFixedCachedModDivisorSize) {
            auto dispatchDividend = [&]<size_t N, size_t ASize>(auto&& self) ALWAYS_INLINE_LAMBDA -> bool {
                if constexpr (ASize <= 2 * N) {
                    if (a.size() == ASize) {
                        cachedModFoldFixed<N, ASize>(r.first<N>(), a.first<ASize>(), b.first<N>(), foldFactor);
                        return true;
                    }
                    return self.template operator()<N, ASize + 1>(self);
                } else
                    return false;
            };
            auto dispatchDivisor = [&]<size_t N>(auto&& self) ALWAYS_INLINE_LAMBDA -> bool {
                if constexpr (N >= 2) {
                    if (b.size() == N)
                        return dispatchDividend.template operator()<N, N>(dispatchDividend);
                    return self.template operator()<N - 1>(self);
                } else
                    return false;
            };
            if (dispatchDivisor.template operator()<maxFixedCachedModDivisorSize>(dispatchDivisor))
                return r;
        }
        cachedModFold(r, a, b, foldFactor);
        return r;
    }

    // cachedModFixed reads the inverse through a static-extent span, which has no bounds check of
    // its own, and the size-agnostic path below indexes it up to n. Both rely on the inverse having
    // been rebuilt for this divisor, which happens in cachedModMakeInverse when the divisor is
    // armed.
    ASSERT(vm.m_bigIntCachedInverse.size() == n + 1);
    auto inv = vm.m_bigIntCachedInverse.span().first(n + 1);

    // Divisors up to maxFixedCachedModDivisorSize digits get a size-specialized path. With every
    // span extent static the special multiplies unroll and drop their per-column bound
    // arithmetic. Larger divisors keep the size-agnostic code below, where the loops are long
    // enough that the per-column overhead no longer dominates.
    //
    // Both lambdas walk their size down/up at compile time and test one size per step. The
    // dividend walk covers all of n to 2n, the range cachedMod accepts: which widths actually
    // occur depends on the modulus, not just on the caller. A product of two reduced operands
    // fills 2n digits only when the modulus nearly fills its top digit, and reducing a sum rather
    // than a product yields n or n + 1 digits, so restricting this to any single dividend width
    // would leave whole classes of modulus silently on the slow path.
    if (n <= maxFixedCachedModDivisorSize) {
        auto dispatchDividend = [&]<size_t N, size_t ASize>(auto&& self) ALWAYS_INLINE_LAMBDA -> bool {
            if constexpr (ASize <= 2 * N) {
                if (a.size() == ASize) {
                    cachedModFixed<N, ASize>(vm, r.first<N>(), a.first<ASize>(), b.first<N>());
                    return true;
                }
                return self.template operator()<N, ASize + 1>(self);
            } else
                return false;
        };
        auto dispatchDivisor = [&]<size_t N>(auto&& self) ALWAYS_INLINE_LAMBDA -> bool {
            if constexpr (N >= 2) {
                if (b.size() == N)
                    return dispatchDividend.template operator()<N, N>(dispatchDividend);
                return self.template operator()<N - 1>(self);
            } else
                return false;
        };
        if (dispatchDivisor.template operator()<maxFixedCachedModDivisorSize>(dispatchDivisor))
            return r;
    }

    // Step 1: Compute only the high digits of A * Inv via multiplySpecialHigh.
    //
    // Longhand multiplication of A (m digits) x Inv (k = n+1 digits):
    // Each a_j * i_r produces a two-digit result (H:L). The low part L
    // goes to column j+r, the high part H carries into column j+r+1.
    //
    // Example: n = 2, A = (a3 a2 a1 a0), Inv = (i2 i1 i0).
    //
    //        +------+------+------+------+------+------+------+
    //        | col6 | col5 | col4 | col3 | col2 | col1 | col0 |
    //        +------+------+------+------+------+------+------+
    //  A*i0  |      |      |      | a3i0 | a2i0 | a1i0 | a0i0 |
    //  A*i1  |      |      | a3i1 | a2i1 | a1i1 | a0i1 |      |
    //  A*i2  |      | a3i2 | a2i2 | a1i2 | a0i2 |      |      |
    //        +------+------+------+------+------+------+------+
    //  Sum   |  P6  |  P5  |  P4  |  P3  |  P2  |  P1  |  P0  |
    //        +------+------+------+------+------+------+------+
    //        |<-- Q = floor(P/B^(2n)) -->|      |             |
    //        |<--- multiplySpecialHigh -------->|<-- skip --->|
    //                                           ^
    //                                      startPos = 2
    //
    // The code accumulates columns left-to-right, holding each column's running sum in
    // DigitColumnAccumulator's three digits (t0, t1, t2). Storing a column shifts that sum down by
    // one digit, so the state carried into column i+1 is what did not fit in digit i.
    //
    // multiplySpecialHigh starts at column startPos with the accumulator zeroed, losing the carry
    // out of columns [0..startPos-1]. All pair-sums within columns >= startPos are exact; only the
    // incoming carry is lost.
    //
    // Error bound (general n, s = startPos = 2n-2):
    //
    //   Let S be the exact value of the columns below s:
    //     S = sum over j+r < s of a_j * i_r * B^(j+r)
    //   The lost state is exactly floor(S / B^s), so the lost value is
    //     E = floor(S / B^s) * B^s <= S
    //   Column c holds at most c+1 terms, each at most (B-1)^2, so
    //     S <= (B-1)^2 * sum over c < s of (c+1) * B^c < s * (B-1) * B^s < s * B^(s+1)
    //   With s = 2n-2: E < (2n-2) * B^(2n-1)
    //
    //   True quotient:  Q_true   = floor(P     / B^(2n))
    //   Our quotient:   Q_approx = floor((P-E) / B^(2n))
    //
    //   Write P = Q_true * B^(2n) + R,  0 <= R < B^(2n).
    //
    //     If R >= E: P-E = Q_true * B^(2n) + (R-E)
    //                => Q_approx = Q_true               (error = 0)
    //
    //     If R < E: P-E = (Q_true-1) * B^(2n) + (B^(2n) + R - E)
    //               This needs E < B^(2n), i.e. 2n-2 < B. Divisors are capped at
    //               maxCachedModDivisorSize digits, so 2n-2 is at most 62 while B is 2^digitBits;
    //               raising that cap anywhere near B would invalidate this step.
    //               => Q_approx = Q_true - 1           (error = 1)
    //
    //   Therefore: 0 <= Q_true - Q_approx <= 1.
    //   Step 5's corrective loop handles Q being off by 1.
    size_t startPos = 2 * n - 2;
    size_t scratchSpace = a.size() + inv.size();
    // cachedMod's scratch holds the dividend (at most 2n digits) plus the inverse (n + 1 digits).
    constexpr unsigned maxCachedModScratchSize = 3 * maxCachedModDivisorSize + 1;
    ASSERT(scratchSpace <= maxCachedModScratchSize);
    Vector<Digit, maxCachedModScratchSize> scratch(scratchSpace);
    if (a.size() >= inv.size())
        multiplySpecialHigh(a, inv, scratch.mutableSpan(), startPos);
    else
        multiplySpecialHigh(inv, a, scratch.mutableSpan(), startPos);

    // Step 2: Extract estimated quotient Q from position 2n in the product.
    // This means right-shifting 2n digits.
    auto qSpan = scratch.span().subspan(2 * n);

    // Step 3: Compute product_low = B * Q (only low n+1 digits needed).
    // Reuse the low part of scratch for product_low (no overlap with qSpan).
    auto productLow = scratch.mutableSpan().first(n + 1);
    multiplySpecialLow(b, qSpan, productLow);

    // Step 4: R = A[0..n-1] - product_low[0..n-1].
    Digit borrow = subtractAndReturnBorrow(r, a, productLow.first(n));

    // Track the extra digit: r_high = A[n] - product_low[n] - borrow.
    Digit an = a.size() > n ? a[n] : 0;
    Digit rHigh = an - productLow[n] - borrow;

    // Step 5: Corrective loop using sign bit of r_high.
    constexpr Digit signBit = static_cast<Digit>(1) << (digitBits - 1);
    if (rHigh & signBit) {
        // Result is negative — add B back until r_high == 0.
        do {
            rHigh += inplaceAdd(r, b);
        } while (rHigh);
    } else {
        // Result is non-negative but may be >= B — subtract B.
        while (rHigh || greaterThanOrEqual(r, b))
            rHigh -= inplaceSub(r, b);
    }

    return r.first(n);
}

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::remainderImpl(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    // 1. If y is 0n, throw a RangeError exception.
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (y.isZero()) {
        throwRangeError(globalObject, scope, "0 is an invalid divisor value."_s);
        return nullptr;
    }

    // 2. Return the JSBigInt representing x modulo y.
    // See https://github.com/tc39/proposal-bigint/issues/84 though.
    switch (absoluteCompare(x, y)) {
    case ComparisonResult::LessThan: {
        return { x };
    }
    case ComparisonResult::Equal: {
        RELEASE_AND_RETURN(scope, zeroImpl(vm));
    }
    case ComparisonResult::GreaterThan:
    case ComparisonResult::Undefined:
        break;
    }

    auto xSpan = x.digits();
    auto ySpan = y.digits();
    if (ySpan.size() == 1) {
        Digit divisor = ySpan[0];
        if (divisor == 1)
            RELEASE_AND_RETURN(scope, zeroImpl(vm));

        Digit remainderDigit;
        divideSingle({ }, remainderDigit, xSpan, divisor);
        if (!remainderDigit)
            RELEASE_AND_RETURN(scope, zeroImpl(vm));

        auto* remainder = createWithLength(globalObject, 1);
        RETURN_IF_EXCEPTION(scope, nullptr);

        remainder->setDigit(0, remainderDigit);
        remainder->setSign(x.sign());
        return remainder;
    }

    // Cached multiplicative inverse optimization for repeated modulo with the same divisor.
    if constexpr (std::is_same_v<BigIntImpl2, HeapBigIntImpl>) {
        if (vm.m_cachedBigIntDivisor.get() == y.toHeapBigInt(globalObject)) {
            if (xSpan.size() <= 2 * ySpan.size()) {
                unsigned resultLength = ySpan.size();
                if (resultLength <= maxInPlaceCachedModSize) {
                    auto* cell = tryAllocateCell<JSBigInt>(vm, JSBigInt::allocationSize(resultLength));
                    if (!cell) [[unlikely]] {
                        throwOutOfMemoryError(globalObject, scope);
                        return nullptr;
                    }
                    JSBigInt* bigInt = new (NotNull, cell) JSBigInt(vm, vm.bigIntStructure.get(), resultLength);
                    bigInt->finishCreation(vm);
                    bigInt->setSign(x.sign());
                    auto rSpan = normalize(cachedMod(vm, bigInt->digits(), xSpan, ySpan));
                    if (rSpan.empty())
                        RELEASE_AND_RETURN(scope, zeroImpl(vm));
                    bigInt->setLength(rSpan.size());
                    return bigInt;
                }
                Vector<Digit, maxCachedModDivisorSize> r(resultLength);
                auto rSpan = cachedMod(vm, r.mutableSpan(), xSpan, ySpan);
                RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, x.sign(), rSpan));
            }
        } else if (vm.m_nextCachedBigIntDivisor.get() == y.toHeapBigInt(globalObject)) {
            if (++vm.m_bigIntDivisorCount >= 100) {
                vm.m_cachedBigIntDivisor.setWithoutWriteBarrier(y.toHeapBigInt(globalObject));
                vm.m_bigIntFoldFactor = cachedModFoldFactor(ySpan);
                if (!vm.m_bigIntFoldFactor)
                    cachedModMakeInverse(vm, ySpan); // Compute inverse when appropriate fold-factor is not found.
            }
        } else if (ySpan.size() >= 2 && ySpan.size() <= maxCachedModDivisorSize) {
            vm.m_nextCachedBigIntDivisor.setWithoutWriteBarrier(y.toHeapBigInt(globalObject));
            vm.m_bigIntDivisorCount = 1;
        }
    }

    Vector<Digit, 16> r(ySpan.size());
    std::span<const Digit> rSpan;
    if (xSpan.size() == ySpan.size())
        rSpan = remainderSameSize(r.mutableSpan(), xSpan, ySpan);
    else {
        InterruptCheck interrupt(&vm);
        rSpan = std::get<1>(divideDigitsInto(interrupt, { }, r.mutableSpan(), xSpan, ySpan));
        if (interrupt.interrupted()) [[unlikely]] {
            RELEASE_ASSERT(scope.exception());
            return nullptr;
        }
    }
    RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, x.sign(), rSpan));
}

JSValue JSBigInt::remainder(JSGlobalObject* globalObject, JSBigInt* x, JSBigInt* y)
{
    return tryConvertToBigInt32(remainderImpl(globalObject, HeapBigIntImpl { x }, HeapBigIntImpl { y }));
}
#if USE(BIGINT32)
JSValue JSBigInt::remainder(JSGlobalObject* globalObject, JSBigInt* x, int32_t y)
{
    return tryConvertToBigInt32(remainderImpl(globalObject, HeapBigIntImpl { x }, Int32BigIntImpl { y }));
}
JSValue JSBigInt::remainder(JSGlobalObject* globalObject, int32_t x, JSBigInt* y)
{
    return tryConvertToBigInt32(remainderImpl(globalObject, Int32BigIntImpl { x }, HeapBigIntImpl { y }));
}
#endif

JSBigInt::ImplResult JSBigInt::absoluteAddOne(JSGlobalObject* globalObject, std::span<const Digit> x, bool resultSign)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    unsigned resultLength = addOneLength(x);
    if (resultLength > maxLength) [[unlikely]] {
        Vector<Digit> scratch(resultLength);
        auto result = absoluteAddOne(x, scratch.mutableSpan());
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, resultSign, result));
    }

    auto* cell = tryAllocateCell<JSBigInt>(vm, JSBigInt::allocationSize(resultLength));
    if (!cell) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }

    JSBigInt* bigInt = new (NotNull, cell) JSBigInt(vm, vm.bigIntStructure.get(), resultLength);
    bigInt->finishCreation(vm);
    bigInt->setSign(resultSign);

    auto span = absoluteAddOne(x, bigInt->digits());
    ASSERT(!span.empty());
    ASSERT(span.back());
    if (span.size() < resultLength)
        bigInt->setLength(span.size());
    return bigInt;
}

JSBigInt::ImplResult JSBigInt::absoluteSubOne(JSGlobalObject* globalObject, std::span<const Digit> x, bool resultSign)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(!x.empty());
    unsigned resultLength = subOneLength(x);
    auto* cell = tryAllocateCell<JSBigInt>(vm, JSBigInt::allocationSize(resultLength));
    if (!cell) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }

    JSBigInt* bigInt = new (NotNull, cell) JSBigInt(vm, vm.bigIntStructure.get(), resultLength);
    bigInt->finishCreation(vm);
    bigInt->setSign(resultSign);

    auto span = normalize(absoluteSubOne(x, bigInt->digits()));
    if (span.empty())
        RELEASE_AND_RETURN(scope, zeroImpl(vm));
    if (span.size() < resultLength)
        bigInt->setLength(span.size());
    return bigInt;
}

template <typename BigIntImpl>
JSBigInt::ImplResult JSBigInt::incImpl(JSGlobalObject* globalObject, BigIntImpl x)
{
    auto xSpan = x.digits();
    if (!x.sign())
        return absoluteAddOne(globalObject, xSpan, false);
    return absoluteSubOne(globalObject, xSpan, true);
}

JSValue JSBigInt::inc(JSGlobalObject* globalObject, JSBigInt* x)
{
    return tryConvertToBigInt32(incImpl(globalObject, HeapBigIntImpl { x }));
}

template <typename BigIntImpl>
JSBigInt::ImplResult JSBigInt::decImpl(JSGlobalObject* globalObject, BigIntImpl x)
{
    if (x.isZero()) {
#if USE(BIGINT32)
        return jsBigInt32(-1);
#else
        return createFrom(globalObject, -1);
#endif
    }

    auto xSpan = x.digits();
    if (!x.sign())
        return absoluteSubOne(globalObject, xSpan, false);
    return absoluteAddOne(globalObject, xSpan, true);
}

JSValue JSBigInt::dec(JSGlobalObject* globalObject, JSBigInt* x)
{
    return tryConvertToBigInt32(decImpl(globalObject, HeapBigIntImpl { x }));
}

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::addImpl(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    bool xSign = x.sign();

    // x + y == x + y
    // -x + -y == -(x + y)
    if (xSign == y.sign())
        return absoluteAdd(globalObject, x, y, xSign);

    // x + -y == x - y == -(y - x)
    // -x + y == y - x == -(x - y)
    ComparisonResult comparisonResult = absoluteCompare(x, y);
    if (comparisonResult == ComparisonResult::Equal)
        return zeroImpl(globalObject->vm());
    if (comparisonResult == ComparisonResult::GreaterThan)
        return absoluteSub(globalObject, x, y, xSign);

    return absoluteSub(globalObject, y, x, !xSign);
}
JSValue JSBigInt::add(JSGlobalObject* globalObject, JSBigInt* x, JSBigInt* y)
{
    return tryConvertToBigInt32(addImpl(globalObject, HeapBigIntImpl { x }, HeapBigIntImpl { y }));
}
#if USE(BIGINT32)
JSValue JSBigInt::add(JSGlobalObject* globalObject, JSBigInt* x, int32_t y)
{
    return tryConvertToBigInt32(addImpl(globalObject, HeapBigIntImpl { x }, Int32BigIntImpl { y }));
}
JSValue JSBigInt::add(JSGlobalObject* globalObject, int32_t x, JSBigInt* y)
{
    return tryConvertToBigInt32(addImpl(globalObject, Int32BigIntImpl { x }, HeapBigIntImpl { y }));
}
#endif

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::subImpl(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    bool xSign = x.sign();
    if (xSign != y.sign()) {
        // x - (-y) == x + y
        // (-x) - y == -(x + y)
        return absoluteAdd(globalObject, x, y, xSign);
    }
    // x - y == -(y - x)
    // (-x) - (-y) == y - x == -(x - y)
    ComparisonResult comparisonResult = absoluteCompare(x, y);
    if (comparisonResult == ComparisonResult::Equal)
        return zeroImpl(globalObject->vm());
    if (comparisonResult == ComparisonResult::GreaterThan)
        return absoluteSub(globalObject, x, y, xSign);

    return absoluteSub(globalObject, y, x, !xSign);
}

JSValue JSBigInt::sub(JSGlobalObject* globalObject, JSBigInt* x, JSBigInt* y)
{
    return tryConvertToBigInt32(subImpl(globalObject, HeapBigIntImpl { x }, HeapBigIntImpl { y }));
}
#if USE(BIGINT32)
JSValue JSBigInt::sub(JSGlobalObject* globalObject, JSBigInt* x, int32_t y)
{
    return tryConvertToBigInt32(subImpl(globalObject, HeapBigIntImpl { x }, Int32BigIntImpl { y }));
}
JSValue JSBigInt::sub(JSGlobalObject* globalObject, int32_t x, JSBigInt* y)
{
    return tryConvertToBigInt32(subImpl(globalObject, Int32BigIntImpl { x }, HeapBigIntImpl { y }));
}
#endif

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::bitwiseAndImpl(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto xSpan = x.digits();
    auto ySpan = y.digits();
    if (!x.sign() && !y.sign()) {
        Vector<Digit, 16> resultVector(andLength(xSpan, ySpan));
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, false, absoluteAnd(xSpan, ySpan, resultVector.mutableSpan())));
    }

    if (x.sign() && y.sign()) {
        // (-x) & (-y) == ~(x-1) & ~(y-1) == ~((x-1) | (y-1))
        // == -(((x-1) | (y-1)) + 1)
        Vector<Digit, 16> resultXVector(subOneLength(xSpan));
        auto resultX = normalize(absoluteSubOne(xSpan, resultXVector.mutableSpan()));

        Vector<Digit, 16> resultYVector(subOneLength(ySpan));
        auto resultY = normalize(absoluteSubOne(ySpan, resultYVector.mutableSpan()));

        Vector<Digit, 16> resultVector(orLength(resultX, resultY));
        auto result = normalize(absoluteOr(resultX, resultY, resultVector.mutableSpan()));

        Vector<Digit, 16> finalResultVector(addOneLength(result));
        auto finalResult = absoluteAddOne(result, finalResultVector.mutableSpan());

        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, true, finalResult));
    }

    ASSERT(x.sign() != y.sign());
    // x & (-y) == x & ~(y-1)
    auto computeResult = [&] (auto x, auto y) -> JSBigInt* {
        ASSERT(!x.sign()); 
        ASSERT(y.sign()); 
        auto xSpan = x.digits();
        auto ySpan = y.digits();
        Vector<Digit, 16> resultYVector(subOneLength(ySpan));
        auto resultY = normalize(absoluteSubOne(ySpan, resultYVector.mutableSpan()));

        Vector<Digit, 16> resultVector(andNotLength(xSpan, resultY));
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, false, absoluteAndNot(xSpan, resultY, resultVector.mutableSpan())));
    };
    if (x.sign())
        return computeResult(y, x);
    return computeResult(x, y);
}

JSValue JSBigInt::bitwiseAnd(JSGlobalObject* globalObject, JSBigInt* x, JSBigInt* y)
{
    return tryConvertToBigInt32(bitwiseAndImpl(globalObject, HeapBigIntImpl { x }, HeapBigIntImpl { y }));
}
#if USE(BIGINT32)
JSValue JSBigInt::bitwiseAnd(JSGlobalObject* globalObject, JSBigInt* x, int32_t y)
{
    return tryConvertToBigInt32(bitwiseAndImpl(globalObject, HeapBigIntImpl { x }, Int32BigIntImpl { y }));
}
JSValue JSBigInt::bitwiseAnd(JSGlobalObject* globalObject, int32_t x, JSBigInt* y)
{
    return tryConvertToBigInt32(bitwiseAndImpl(globalObject, Int32BigIntImpl { x }, HeapBigIntImpl { y }));
}
#endif

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::bitwiseOrImpl(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto xSpan = x.digits();
    auto ySpan = y.digits();
    if (!x.sign() && !y.sign()) {
        Vector<Digit, 16> resultVector(orLength(xSpan, ySpan));
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, false, absoluteOr(xSpan, ySpan, resultVector.mutableSpan())));
    }
    
    if (x.sign() && y.sign()) {
        // (-x) | (-y) == ~(x-1) | ~(y-1) == ~((x-1) & (y-1))
        // == -(((x-1) & (y-1)) + 1)
        Vector<Digit, 16> resultXVector(subOneLength(xSpan));
        auto resultX = normalize(absoluteSubOne(xSpan, resultXVector.mutableSpan()));

        Vector<Digit, 16> resultYVector(subOneLength(ySpan));
        auto resultY = normalize(absoluteSubOne(ySpan, resultYVector.mutableSpan()));

        Vector<Digit, 16> resultVector(andLength(resultX, resultY));
        auto result = normalize(absoluteAnd(resultX, resultY, resultVector.mutableSpan()));

        Vector<Digit, 16> finalResultVector(addOneLength(result));
        auto finalResult = absoluteAddOne(result, finalResultVector.mutableSpan());

        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, true, finalResult));
    }

    ASSERT(x.sign() != y.sign());

    // x | (-y) == x | ~(y-1) == ~((y-1) &~ x) == -(((y-1) &~ x) + 1)
    auto computeResult = [&] (auto x, auto y) -> JSBigInt* {
        ASSERT(!x.sign());
        ASSERT(y.sign());

        auto xSpan = x.digits();
        auto ySpan = y.digits();
        Vector<Digit, 16> resultYVector(subOneLength(ySpan));
        auto resultY = normalize(absoluteSubOne(ySpan, resultYVector.mutableSpan()));

        Vector<Digit, 16> resultVector(andNotLength(resultY, xSpan));
        auto result = normalize(absoluteAndNot(resultY, xSpan, resultVector.mutableSpan()));

        Vector<Digit, 16> finalResultVector(addOneLength(result));
        auto finalResult = absoluteAddOne(result, finalResultVector.mutableSpan());

        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, true, finalResult));
    };

    if (x.sign())
        return computeResult(y, x);
    return computeResult(x, y);
}

JSValue JSBigInt::bitwiseOr(JSGlobalObject* globalObject, JSBigInt* x, JSBigInt* y)
{
    return tryConvertToBigInt32(bitwiseOrImpl(globalObject, HeapBigIntImpl { x }, HeapBigIntImpl { y }));
}
#if USE(BIGINT32)
JSValue JSBigInt::bitwiseOr(JSGlobalObject* globalObject, JSBigInt* x, int32_t y)
{
    return tryConvertToBigInt32(bitwiseOrImpl(globalObject, HeapBigIntImpl { x }, Int32BigIntImpl { y }));
}
JSValue JSBigInt::bitwiseOr(JSGlobalObject* globalObject, int32_t x, JSBigInt* y)
{
    return tryConvertToBigInt32(bitwiseOrImpl(globalObject, Int32BigIntImpl { x }, HeapBigIntImpl { y }));
}
#endif

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::bitwiseXorImpl(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto xSpan = x.digits();
    auto ySpan = y.digits();
    if (!x.sign() && !y.sign()) {
        Vector<Digit, 16> resultVector(xorLength(xSpan, ySpan));
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, false, absoluteXor(xSpan, ySpan, resultVector.mutableSpan())));
    }

    if (x.sign() && y.sign()) {
        // (-x) ^ (-y) == ~(x-1) ^ ~(y-1) == (x-1) ^ (y-1)
        Vector<Digit, 16> resultXVector(subOneLength(xSpan));
        auto resultX = normalize(absoluteSubOne(xSpan, resultXVector.mutableSpan()));

        Vector<Digit, 16> resultYVector(subOneLength(ySpan));
        auto resultY = normalize(absoluteSubOne(ySpan, resultYVector.mutableSpan()));

        Vector<Digit, 16> resultVector(xorLength(resultX, resultY));
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, false, absoluteXor(resultX, resultY, resultVector.mutableSpan())));
    }
    ASSERT(x.sign() != y.sign());

    // x ^ (-y) == x ^ ~(y-1) == ~(x ^ (y-1)) == -((x ^ (y-1)) + 1)
    auto computeResult = [&] (auto x, auto y) -> JSBigInt* {
        ASSERT(!x.sign());
        ASSERT(y.sign());

        auto xSpan = x.digits();
        auto ySpan = y.digits();
        Vector<Digit, 16> resultYVector(subOneLength(ySpan));
        auto resultY = normalize(absoluteSubOne(ySpan, resultYVector.mutableSpan()));

        Vector<Digit, 16> resultVector(xorLength(resultY, xSpan));
        auto result = normalize(absoluteXor(resultY, xSpan, resultVector.mutableSpan()));

        Vector<Digit, 16> finalResultVector(addOneLength(result));
        auto finalResult = absoluteAddOne(result, finalResultVector.mutableSpan());

        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, true, finalResult));
    };

    // Assume that x is the positive BigInt.
    if (x.sign())
        return computeResult(y, x);
    return computeResult(x, y);
}

JSValue JSBigInt::bitwiseXor(JSGlobalObject* globalObject, JSBigInt* x, JSBigInt* y)
{
    return tryConvertToBigInt32(bitwiseXorImpl(globalObject, HeapBigIntImpl { x }, HeapBigIntImpl { y }));
}
#if USE(BIGINT32)
JSValue JSBigInt::bitwiseXor(JSGlobalObject* globalObject, JSBigInt* x, int32_t y)
{
    return tryConvertToBigInt32(bitwiseXorImpl(globalObject, HeapBigIntImpl { x }, Int32BigIntImpl { y }));
}
JSValue JSBigInt::bitwiseXor(JSGlobalObject* globalObject, int32_t x, JSBigInt* y)
{
    return tryConvertToBigInt32(bitwiseXorImpl(globalObject, Int32BigIntImpl { x }, HeapBigIntImpl { y }));
}
#endif

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::leftShiftImpl(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    if (x.isZero() || y.isZero())
        return { x };

    if (y.sign())
        return rightShiftByAbsolute(globalObject, x, y);

    return leftShiftByAbsolute(globalObject, x, y);
}

JSValue JSBigInt::leftShift(JSGlobalObject* globalObject, JSBigInt* x, JSBigInt* y)
{
    return tryConvertToBigInt32(leftShiftImpl(globalObject, HeapBigIntImpl { x }, HeapBigIntImpl { y }));
}
#if USE(BIGINT32)
JSValue JSBigInt::leftShift(JSGlobalObject* globalObject, JSBigInt* x, int32_t y)
{
    return tryConvertToBigInt32(leftShiftImpl(globalObject, HeapBigIntImpl { x }, Int32BigIntImpl { y }));
}
JSValue JSBigInt::leftShift(JSGlobalObject* globalObject, int32_t x, JSBigInt* y)
{
    return tryConvertToBigInt32(leftShiftImpl(globalObject, Int32BigIntImpl { x }, HeapBigIntImpl { y }));
}
JSValue JSBigInt::leftShiftSlow(JSGlobalObject* globalObject, int32_t x, int32_t y)
{
    return tryConvertToBigInt32(leftShiftImpl(globalObject, Int32BigIntImpl { x }, Int32BigIntImpl { y }));
}
#endif

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::signedRightShiftImpl(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    if (x.isZero() || y.isZero())
        return { x };

    if (y.sign())
        return leftShiftByAbsolute(globalObject, x, y);

    return rightShiftByAbsolute(globalObject, x, y);
}

JSValue JSBigInt::signedRightShift(JSGlobalObject* globalObject, JSBigInt* x, JSBigInt* y)
{
    return tryConvertToBigInt32(signedRightShiftImpl(globalObject, HeapBigIntImpl { x }, HeapBigIntImpl { y }));
}
#if USE(BIGINT32)
JSValue JSBigInt::signedRightShift(JSGlobalObject* globalObject, JSBigInt* x, int32_t y)
{
    return tryConvertToBigInt32(signedRightShiftImpl(globalObject, HeapBigIntImpl { x }, Int32BigIntImpl { y }));
}
JSValue JSBigInt::signedRightShift(JSGlobalObject* globalObject, int32_t x, JSBigInt* y)
{
    return tryConvertToBigInt32(signedRightShiftImpl(globalObject, Int32BigIntImpl { x }, HeapBigIntImpl { y }));
}
#endif

template <typename BigIntImpl>
JSBigInt::ImplResult JSBigInt::bitwiseNotImpl(JSGlobalObject* globalObject, BigIntImpl x)
{
    VM& vm = globalObject->vm();
    auto xSpan = x.digits();
    if (x.sign()) {
        // ~(-x) == ~(~(x-1)) == x-1
        Vector<Digit, 16> resultVector(subOneLength(xSpan));
        return tryCreateFromImpl(globalObject, vm, false, absoluteSubOne(xSpan, resultVector.mutableSpan()));
    } 
    // ~x == -x-1 == -(x+1)
    Vector<Digit, 16> resultVector(addOneLength(xSpan));
    auto result = absoluteAddOne(xSpan, resultVector.mutableSpan());
    return tryCreateFromImpl(globalObject, vm, true, result);
}

JSValue JSBigInt::bitwiseNot(JSGlobalObject* globalObject, JSBigInt* x)
{
    return tryConvertToBigInt32(bitwiseNotImpl(globalObject, HeapBigIntImpl { x }));
}

// {carry} must point to an initialized Digit and will either be incremented
// by one or left alone.
inline JSBigInt::Digit JSBigInt::digitAdd(Digit a, Digit b, Digit& carry)
{
    auto result = static_cast<TwoDigit>(a) + b;
    carry += static_cast<Digit>(result >> static_cast<int>(digitBits));
    return static_cast<Digit>(result);
}

// {carry} is set to 0 or 1. {c} must be 0 or 1.
inline JSBigInt::Digit JSBigInt::digitAdd3(Digit a, Digit b, Digit c, Digit& carry)
{
    ASSERT(c <= 1);
    Digit partial;
    bool carryFromPartial = __builtin_add_overflow(a, b, &partial);
    Digit result;
    bool carryFromC = __builtin_add_overflow(partial, c, &result);
    // a + b <= 2^digitBits * 2 - 2, so at most one of the two additions can carry.
    carry = static_cast<Digit>(carryFromPartial) | static_cast<Digit>(carryFromC);
    return result;
}

// {borrow} must point to an initialized Digit and will either be incremented
// by one or left alone.
inline JSBigInt::Digit JSBigInt::digitSub(Digit a, Digit b, Digit& borrow)
{
    auto result = static_cast<TwoDigit>(a) - b;
    borrow += static_cast<Digit>(result >> static_cast<int>(digitBits)) & 1;
    return static_cast<Digit>(result);
}

// {borrowOut} is set to 0 or 1. {borrowIn} must be 0 or 1.
inline JSBigInt::Digit JSBigInt::digitSub2(Digit a, Digit b, Digit borrowIn, Digit& borrowOut)
{
    ASSERT(borrowIn <= 1);
    Digit partial;
    bool borrowFromPartial = __builtin_sub_overflow(a, b, &partial);
    Digit result;
    bool borrowFromBorrowIn = __builtin_sub_overflow(partial, borrowIn, &result);
    // b + borrowIn <= 2^digitBits, so at most one of the two subtractions can borrow.
    borrowOut = static_cast<Digit>(borrowFromPartial) | static_cast<Digit>(borrowFromBorrowIn);
    return result;
}

ALWAYS_INLINE std::tuple<JSBigInt::Digit, JSBigInt::Digit> JSBigInt::digitMul(Digit a, Digit b)
{
    TwoDigit result = static_cast<TwoDigit>(a) * static_cast<TwoDigit>(b);
    Digit high = static_cast<Digit>(result >> static_cast<int>(digitBits));
    Digit low = static_cast<Digit>(result);
    return { low, high };
}

// Raises {base} to the power of {exponent}. Does not check for overflow.
inline JSBigInt::Digit JSBigInt::digitPow(Digit base, Digit exponent)
{
    Digit result = 1ull;
    while (exponent > 0) {
        if (exponent & 1)
            result *= base;

        exponent >>= 1;
        base *= base;
    }

    return result;
}

// Returns the quotient.
// quotient = (high << digitBits + low - remainder) / divisor
inline JSBigInt::Digit JSBigInt::digitDiv(Digit high, Digit low, Digit divisor, Digit& remainder)
{
    ASSERT(high < divisor);
#if CPU(X86_64)
    Digit quotient;
    Digit rem;
    __asm__("divq  %[divisor]"
        // Outputs: {quotient} will be in rax, {rem} in rdx.
        : "=a"(quotient), "=d"(rem)
        // Inputs: put {high} into rdx, {low} into rax, and {divisor} into
        // any register or stack slot.
        : "d"(high), "a"(low), [divisor] "rm"(divisor));
    remainder = rem;
    return quotient;
#elif CPU(X86)
    Digit quotient;
    Digit rem;
    __asm__("divl  %[divisor]"
        // Outputs: {quotient} will be in eax, {rem} in edx.
        : "=a"(quotient), "=d"(rem)
        // Inputs: put {high} into edx, {low} into eax, and {divisor} into
        // any register or stack slot.
        : "d"(high), "a"(low), [divisor] "rm"(divisor));
    remainder = rem;
    return quotient;
#else
    // Fast path: if |high| is zero, the computation can be done within Digit range.
    // We do not need to have complicated path.
    if (!high) {
        ASSERT(divisor);
        remainder = low % divisor;
        return low / divisor;
    }

    static constexpr Digit halfDigitBase = 1ull << halfDigitBits;

    // Adapted from Warren, Hacker's Delight, p. 152.
    unsigned s = clz(divisor);
    // If {s} is digitBits here, it causes an undefined behavior.
    // But {s} is never digitBits since {divisor} is never zero here.
    ASSERT(s != digitBits);
    divisor <<= s;

    Digit vn1 = divisor >> halfDigitBits;
    Digit vn0 = divisor & halfDigitMask;

    // {sZeroMask} which is 0 if s == 0 and all 1-bits otherwise.
    // {s} can be 0. If {s} is 0, performing "low >> (digitBits - s)" must not be done since it causes an undefined behavior
    // since `>> digitBits` is undefied in C++. Quoted from C++ spec, "The type of the result is that of the promoted left operand.
    // The behavior is undefined if the right operand is negative, or greater than or equal to the length in bits of the promoted
    // left operand". We mask the right operand of the shift by {shiftMask} (`digitBits - 1`), which makes `digitBits - 0` zero.
    // This shifting produces a value which covers 0 < {s} <= (digitBits - 1) cases. {s} == digitBits never happen as we asserted.
    // Since {sZeroMask} clears the value in the case of {s} == 0, {s} == 0 case is also covered.
    static_assert(sizeof(CPURegister) == sizeof(Digit));
    Digit sZeroMask = static_cast<Digit>((-static_cast<CPURegister>(s)) >> (digitBits - 1));
    static constexpr unsigned shiftMask = digitBits - 1;
    Digit un32 = (high << s) | ((low >> ((digitBits - s) & shiftMask)) & sZeroMask);

    Digit un10 = low << s;
    Digit un1 = un10 >> halfDigitBits;
    Digit un0 = un10 & halfDigitMask;
    Digit q1 = un32 / vn1;
    Digit rhat = un32 - q1 * vn1;

    while (q1 >= halfDigitBase || q1 * vn0 > rhat * halfDigitBase + un1) {
        q1--;
        rhat += vn1;
        if (rhat >= halfDigitBase)
            break;
    }

    Digit un21 = un32 * halfDigitBase + un1 - q1 * divisor;
    Digit q0 = un21 / vn1;
    rhat = un21 - q0 * vn1;

    while (q0 >= halfDigitBase || q0 * vn0 > rhat * halfDigitBase + un0) {
        q0--;
        rhat += vn1;
        if (rhat >= halfDigitBase)
            break;
    }

    remainder = (un21 * halfDigitBase + un0 - q0 * divisor) >> s;
    return q1 * halfDigitBase + q0;
#endif
}

// Multiplies {source} with {factor} and adds {summand} to the result.
// {result} and {source} may be the same BigInt for inplace modification.
// Multiplies {this} with {factor} and adds {summand} to the result.
void JSBigInt::multiplyAdd(std::span<const Digit> source, Digit factor, Digit summand, std::span<Digit> result)
{
    RELEASE_ASSERT(result.size() >= source.size());

    Digit carry = summand;
    Digit high = 0;
    size_t i = 0;
    for (; i < source.size(); i++) {
        // Compute this round's multiplication.
        auto [current, newHigh] = digitMul(source[i], factor);

        // Add last round's carryovers.
        Digit newCarry = 0;
        current = digitAdd(current, high, newCarry);
        current = digitAdd(current, carry, newCarry);

        // Store result and prepare for next round.
        result[i] = current;
        carry = newCarry;
        high = newHigh;
    }

    if (result.size() > i) {
        result[i++] = carry + high;

        // Current callers don't pass in such large results, but let's be robust.
        while (i < result.size())
            result[i++] = 0;
    } else
        ASSERT(!(carry + high));
}

bool JSBigInt::equals(JSBigInt* x, JSBigInt* y)
{
    if (x->sign() != y->sign())
        return false;

    if (x->length() != y->length())
        return false;

    for (unsigned i = 0; i < x->length(); i++) {
        if (x->digit(i) != y->digit(i))
            return false;
    }

    return true;
}

template <typename BigIntImpl1, typename BigIntImpl2>
inline JSBigInt::ComparisonResult JSBigInt::absoluteCompare(BigIntImpl1 x, BigIntImpl2 y)
{
    ASSERT(!x.length() || x.digit(x.length() - 1));
    ASSERT(!y.length() || y.digit(y.length() - 1));

    int diff = x.length() - y.length();
    if (diff)
        return diff < 0 ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;

    int i = x.length() - 1;
    while (i >= 0 && x.digit(i) == y.digit(i))
        i--;

    if (i < 0)
        return ComparisonResult::Equal;

    return x.digit(i) > y.digit(i) ? ComparisonResult::GreaterThan : ComparisonResult::LessThan;
}

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ComparisonResult JSBigInt::compareImpl(BigIntImpl1 x, BigIntImpl2 y)
{
    bool xSign = x.sign();

    if (xSign != y.sign())
        return xSign ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;

    ComparisonResult result = absoluteCompare(x, y);
    if (result == ComparisonResult::GreaterThan)
        return xSign ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;
    if (result == ComparisonResult::LessThan)
        return xSign ? ComparisonResult::GreaterThan : ComparisonResult::LessThan;

    return ComparisonResult::Equal; 
}

JSBigInt::ComparisonResult JSBigInt::compare(JSBigInt* x, JSBigInt* y)
{
    return compareImpl(HeapBigIntImpl { x }, HeapBigIntImpl { y });
}

JSBigInt::ComparisonResult JSBigInt::compare(int32_t x, JSBigInt* y)
{
    return compareImpl(Int32BigIntImpl { x }, HeapBigIntImpl { y });
}

JSBigInt::ComparisonResult JSBigInt::compare(JSBigInt* x, int32_t y)
{
    return compareImpl(HeapBigIntImpl { x }, Int32BigIntImpl { y });
}

JSBigInt::ComparisonResult JSBigInt::compare(JSBigInt* x, int64_t y)
{
    return compareImpl(HeapBigIntImpl { x }, Int64BigIntImpl { y });
}

JSBigInt::ComparisonResult JSBigInt::compare(JSValue x, int64_t y)
{
    ASSERT(x.isBigInt());
#if USE(BIGINT32)
    if (x.isBigInt32())
        return compareImpl(Int32BigIntImpl { x.bigInt32AsInt32() }, Int64BigIntImpl { y });
#endif
    return compare(x.asHeapBigInt(), y);
}

JSBigInt::ComparisonResult JSBigInt::compare(JSBigInt* x, uint64_t y)
{
    return compareImpl(HeapBigIntImpl { x }, Int64BigIntImpl { y });
}

JSBigInt::ComparisonResult JSBigInt::compare(JSValue x, uint64_t y)
{
    ASSERT(x.isBigInt());
#if USE(BIGINT32)
    if (x.isBigInt32())
        return compareImpl(Int32BigIntImpl { x.bigInt32AsInt32() }, Int64BigIntImpl { y });
#endif
    return compare(x.asHeapBigInt(), y);
}

JSBigInt::ComparisonResult JSBigInt::compare(JSValue x, JSValue y)
{
    ASSERT(x.isBigInt() && y.isBigInt());
#if USE(BIGINT32)
    if (x.isBigInt32() && y.isBigInt32()) {
        int32_t x1 = x.asBigInt32();
        int32_t y1 = y.asBigInt32();
        if (x1 == y1)
            return JSBigInt::ComparisonResult::Equal;
        if (x1 < y1)
            return JSBigInt::ComparisonResult::LessThan;
        return JSBigInt::ComparisonResult::GreaterThan;
    }
    if (x.isBigInt32())
        return compare(x.bigInt32AsInt32(), y.asHeapBigInt());
    if (y.isBigInt32())
        return compare(x.asHeapBigInt(), y.bigInt32AsInt32());
#endif
    return compare(x.asHeapBigInt(), y.asHeapBigInt());
}

std::span<JSBigInt::Digit> JSBigInt::addSchoolbook(std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    RELEASE_ASSERT(x.size() >= y.size());
    RELEASE_ASSERT(result.size() >= (x.size() + 1));
    Digit carry = 0;
    size_t i = 0;
    for (; i < y.size(); i++) {
        Digit newCarry = 0;
        result[i] = digitAdd3(x[i], y[i], carry, newCarry);
        carry = newCarry;
    }

    for (; i < x.size(); i++) {
        Digit newCarry = 0;
        result[i] = digitAdd(x[i], carry, newCarry);
        carry = newCarry;
    }

    result[i++] = carry;
    return result.first(i);
}

template<size_t N>
ALWAYS_INLINE std::span<JSBigInt::Digit, N + 1> JSBigInt::addSchoolbookFixed(std::span<const Digit, N> x, std::span<const Digit, N> y, std::span<Digit, N + 1> result)
{
    Digit carry = 0;
    for (size_t i = 0; i < N; ++i) {
        Digit newCarry = 0;
        result[i] = digitAdd3(x[i], y[i], carry, newCarry);
        carry = newCarry;
    }
    result[N] = carry;
    return result;
}

ALWAYS_INLINE std::span<JSBigInt::Digit> JSBigInt::addDigitsInto(std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    ASSERT(x.size() >= y.size());
    ASSERT(result.size() >= x.size() + 1);

    if (x.size() == y.size()) {
        switch (x.size()) {
        case 1:
            return addSchoolbookFixed<1>(x.first<1>(), y.first<1>(), result.first<2>());
        case 2:
            return addSchoolbookFixed<2>(x.first<2>(), y.first<2>(), result.first<3>());
        case 3:
            return addSchoolbookFixed<3>(x.first<3>(), y.first<3>(), result.first<4>());
        case 4:
            return addSchoolbookFixed<4>(x.first<4>(), y.first<4>(), result.first<5>());
        }
    }
    return addSchoolbook(x, y, result);
}

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::absoluteAdd(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y, bool resultSign)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (x.length() < y.length())
        RELEASE_AND_RETURN(scope, absoluteAdd(globalObject, y, x, resultSign));

    if (x.isZero()) {
        ASSERT(y.isZero());
        return { x };
    }

    if (y.isZero()) {
        if (resultSign == x.sign())
            return { x };
        RELEASE_AND_RETURN(scope, unaryMinusImpl(globalObject, x));
    }

    unsigned resultLength = x.length() + 1;
    if (resultLength > maxLength) [[unlikely]] {
        Vector<Digit> scratch(resultLength);
        auto span = addSchoolbook(x.digits(), y.digits(), scratch.mutableSpan());
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, resultSign, span));
    }

    auto* cell = tryAllocateCell<JSBigInt>(vm, JSBigInt::allocationSize(resultLength));
    if (!cell) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }

    JSBigInt* bigInt = new (NotNull, cell) JSBigInt(vm, vm.bigIntStructure.get(), resultLength);
    bigInt->finishCreation(vm);
    bigInt->setSign(resultSign);

    auto span = addDigitsInto(x.digits(), y.digits(), bigInt->digits());
    ASSERT(!span.empty());
    if (!span.back())
        bigInt->setLength(span.size() - 1);

    return bigInt;
}

std::span<JSBigInt::Digit> JSBigInt::subSchoolbook(std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    RELEASE_ASSERT(x.size() >= y.size());
    RELEASE_ASSERT(result.size() >= x.size());
    Digit borrow = 0;
    size_t i = 0;
    for (; i < y.size(); i++) {
        Digit newBorrow = 0;
        result[i] = digitSub2(x[i], y[i], borrow, newBorrow);
        borrow = newBorrow;
    }

    for (; i < x.size(); i++) {
        Digit newBorrow = 0;
        result[i] = digitSub(x[i], borrow, newBorrow);
        borrow = newBorrow;
    }

    ASSERT(!borrow);
    return result.first(x.size());
}

template<size_t N>
ALWAYS_INLINE std::span<JSBigInt::Digit, N> JSBigInt::subSchoolbookFixed(std::span<const Digit, N> x, std::span<const Digit, N> y, std::span<Digit, N> result)
{
    Digit borrow = 0;
    for (size_t i = 0; i < N; ++i) {
        Digit newBorrow = 0;
        result[i] = digitSub2(x[i], y[i], borrow, newBorrow);
        borrow = newBorrow;
    }
    ASSERT(!borrow);
    return result;
}

ALWAYS_INLINE std::span<JSBigInt::Digit> JSBigInt::subDigitsInto(std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    ASSERT(x.size() >= y.size());
    ASSERT(compareDigits(x, y) != ComparisonResult::LessThan);
    ASSERT(result.size() >= x.size());

    if (x.size() == y.size()) {
        switch (x.size()) {
        case 1:
            return subSchoolbookFixed<1>(x.first<1>(), y.first<1>(), result.first<1>());
        case 2:
            return subSchoolbookFixed<2>(x.first<2>(), y.first<2>(), result.first<2>());
        case 3:
            return subSchoolbookFixed<3>(x.first<3>(), y.first<3>(), result.first<3>());
        case 4:
            return subSchoolbookFixed<4>(x.first<4>(), y.first<4>(), result.first<4>());
        }
    }
    return subSchoolbook(x, y, result);
}

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::absoluteSub(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y, bool resultSign)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Callers fold the equal case into zero, so |x| > |y| here and neither x nor the difference is
    // zero.
    ASSERT(absoluteCompare(x, y) == ComparisonResult::GreaterThan);
    ASSERT(!x.isZero());

    if (y.isZero()) {
        if (resultSign == x.sign())
            return ImplResult { x };
        RELEASE_AND_RETURN(scope, JSBigInt::unaryMinusImpl(globalObject, x));
    }

    unsigned resultLength = x.length();
    if (resultLength > maxInPlaceSubSize) [[unlikely]] {
        Vector<Digit> scratch(resultLength);
        auto span = subSchoolbook(x.digits(), y.digits(), scratch.mutableSpan());
        RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, resultSign, span));
    }

    auto* cell = tryAllocateCell<JSBigInt>(vm, JSBigInt::allocationSize(resultLength));
    if (!cell) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }

    JSBigInt* bigInt = new (NotNull, cell) JSBigInt(vm, vm.bigIntStructure.get(), resultLength);
    bigInt->finishCreation(vm);
    bigInt->setSign(resultSign);

    auto span = normalize(subDigitsInto(x.digits(), y.digits(), bigInt->digits()));
    ASSERT(!span.empty());
    bigInt->setLength(span.size());
    return bigInt;
}

// Returns whether (factor1 * factor2) > (high << digitBits) + low.
inline bool JSBigInt::productGreaterThan(Digit factor1, Digit factor2, Digit high, Digit low)
{
    auto [resultLow, resultHigh] = digitMul(factor1, factor2);
    return resultHigh > high || (resultHigh == high && resultLow > low);
}

// Helper for Absolute{And,AndNot,Or,Xor}.
// Performs the given binary {op} on digit pairs of {x} and {y}; when the
// end of the shorter of the two is reached, {extraDigits} configures how
// remaining digits in the longer input are handled: copied to the result
// or ignored.
// Example:
//       y:             [ y2 ][ y1 ][ y0 ]
//       x:       [ x3 ][ x2 ][ x1 ][ x0 ]
//                   |     |     |     |
//                (Copy)  (op)  (op)  (op)
//                   |     |     |     |
//                   v     v     v     v
// result: [  0 ][ x3 ][ r2 ][ r1 ][ r0 ]
template<typename BitwiseOp>
inline std::span<JSBigInt::Digit> JSBigInt::absoluteBitwiseOp(std::span<const Digit> x, std::span<const Digit> y, ExtraDigitsHandling extraDigits, BitwiseOp&& op, std::span<Digit> result)
{
    if (x.size() < y.size())
        std::swap(x, y);

    ASSERT(x.size() >= y.size());

    size_t numPairs = y.size();
    size_t maxLength = x.size();

    size_t resultLength = extraDigits == ExtraDigitsHandling::Copy ? maxLength : numPairs;
    RELEASE_ASSERT(result.size() >= resultLength);

    size_t i = 0;
    for (; i < numPairs; i++)
        result[i] = op(x[i], y[i]);

    if (extraDigits == ExtraDigitsHandling::Copy) {
        if (numPairs != maxLength)
            memcpySpan(result.subspan(numPairs), x.subspan(numPairs));
    }

    return result.first(resultLength);
}

std::span<JSBigInt::Digit> JSBigInt::absoluteAnd(std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    ASSERT(result.size() >= andLength(x, y));
    auto digitOperation = [](Digit a, Digit b) {
        return a & b;
    };
    return absoluteBitwiseOp(x, y, ExtraDigitsHandling::Skip, digitOperation, result);
}

std::span<JSBigInt::Digit> JSBigInt::absoluteOr(std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    ASSERT(result.size() >= orLength(x, y));
    auto digitOperation = [](Digit a, Digit b) {
        return a | b;
    };
    return absoluteBitwiseOp(x, y, ExtraDigitsHandling::Copy, digitOperation, result);
}

std::span<JSBigInt::Digit> JSBigInt::absoluteAndNot(std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    // x & ~y
    RELEASE_ASSERT(result.size() >= andNotLength(x, y));

    size_t i = 0;
    for (; i < std::min(x.size(), y.size()); i++)
        result[i] = x[i] & ~y[i];

    for (; i < x.size(); ++i)
        result[i] = x[i];

    return result.first(x.size());
}

std::span<JSBigInt::Digit> JSBigInt::absoluteXor(std::span<const Digit> x, std::span<const Digit> y, std::span<Digit> result)
{
    ASSERT(result.size() >= xorLength(x, y));
    auto digitOperation = [](Digit a, Digit b) {
        return a ^ b;
    };
    return absoluteBitwiseOp(x, y, ExtraDigitsHandling::Copy, digitOperation, result);
}

std::span<JSBigInt::Digit> JSBigInt::absoluteAddOne(std::span<const Digit> x, std::span<Digit> result)
{
    ASSERT(result.size() >= addOneLength(x));
    Digit carry = 1;
    size_t i = 0;
    for (; i < x.size(); i++) {
        Digit newCarry = 0;
        result[i] = digitAdd(x[i], carry, newCarry);
        carry = newCarry;
    }
    if (carry)
        result[i++] = carry;
    return result.first(i);
}

std::span<JSBigInt::Digit> JSBigInt::absoluteSubOne(std::span<const Digit> x, std::span<Digit> result)
{
    ASSERT(!x.empty());
    ASSERT(result.size() >= subOneLength(x));
    Digit borrow = 1;
    for (size_t i = 0; i < x.size(); i++) {
        Digit newBorrow = 0;
        result[i] = digitSub(x[i], borrow, newBorrow);
        borrow = newBorrow;
    }
    ASSERT(!borrow);
    return result.first(x.size());
}

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::leftShiftByAbsolute(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto optionalShift = toShiftAmount(y);
    if (!optionalShift) {
        throwOutOfMemoryError(globalObject, scope, "BigInt generated from this operation is too big"_s);
        return nullptr;
    }

    Digit shift = *optionalShift;
    size_t digitShift = static_cast<size_t>(shift / digitBits);
    size_t bitsShift = static_cast<size_t>(shift % digitBits);
    auto xSpan = x.digits();
    size_t length = xSpan.size();
    bool grow = bitsShift && (xSpan[length - 1] >> (digitBits - bitsShift));
    size_t resultLength = length + digitShift + grow;
    if (resultLength > maxLength) {
        throwOutOfMemoryError(globalObject, scope, "BigInt generated from this operation is too big"_s);
        return nullptr;
    }

    Vector<Digit, 16> resultVector(resultLength);
    auto result = resultVector.mutableSpan();
    if (!bitsShift) {
        size_t i = 0;
        for (; i < digitShift; i++)
            result[i] = 0ul;

        for (; i < resultLength; i++)
            result[i] = xSpan[i - digitShift];
    } else {
        Digit carry = 0;
        for (size_t i = 0; i < digitShift; i++)
            result[i] = 0ul;

        for (size_t i = 0; i < length; i++) {
            Digit d = xSpan[i];
            result[i + digitShift] = (d << bitsShift) | carry;
            carry = d >> (digitBits - bitsShift);
        }

        if (grow)
            result[length + digitShift] = carry;
        else
            ASSERT(!carry);
    }

    RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, x.sign(), result));
}

template <typename BigIntImpl1, typename BigIntImpl2>
JSBigInt::ImplResult JSBigInt::rightShiftByAbsolute(JSGlobalObject* globalObject, BigIntImpl1 x, BigIntImpl2 y)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto xSpan = x.digits();
    size_t length = xSpan.size();
    bool sign = x.sign();
    auto optionalShift = toShiftAmount(y);
    if (!optionalShift)
        RELEASE_AND_RETURN(scope, rightShiftByMaximum(globalObject, sign));

    Digit shift = *optionalShift;
    size_t digitalShift = static_cast<size_t>(shift / digitBits);
    size_t bitsShift = static_cast<size_t>(shift % digitBits);
    if (length <= digitalShift)
        RELEASE_AND_RETURN(scope, rightShiftByMaximum(globalObject, sign));

    size_t resultLength = length - digitalShift;

    // For negative numbers, round down if any bit was shifted out (so that e.g.
    // -5n >> 1n == -3n and not -2n). Check now whether this will happen and
    // whether it can cause overflow into a new digit. If we allocate the result
    // large enough up front, it avoids having to do a second allocation later.
    bool mustRoundDown = false;
    if (sign) {
        const Digit mask = (static_cast<Digit>(1) << bitsShift) - 1;
        if (xSpan[digitalShift] & mask)
            mustRoundDown = true;
        else {
            for (size_t i = 0; i < digitalShift; i++) {
                if (xSpan[i]) {
                    mustRoundDown = true;
                    break;
                }
            }
        }
    }

    // If bitsShift is non-zero, it frees up bits, preventing overflow.
    if (mustRoundDown && !bitsShift) {
        // Overflow cannot happen if the most significant digit has unset bits.
        Digit msd = xSpan[length - 1];
        bool roundingCanOverflow = !static_cast<Digit>(~msd);
        if (roundingCanOverflow)
            resultLength++;
    }

    ASSERT(resultLength <= length);
    Vector<Digit, 16> resultVector(resultLength);
    auto result = resultVector.mutableSpan();

    if (!bitsShift) {
        result[resultLength - 1] = 0;
        for (size_t i = digitalShift; i < length; i++)
            result[i - digitalShift] = xSpan[i];
    } else {
        Digit carry = xSpan[digitalShift] >> bitsShift;
        size_t last = length - digitalShift - 1;
        for (size_t i = 0; i < last; i++) {
            Digit d = xSpan[i + digitalShift + 1];
            result[i] = (d << (digitBits - bitsShift)) | carry;
            carry = d >> bitsShift;
        }
        result[last] = carry;
    }

    if (sign) {
        if (mustRoundDown) {
            // Since the result is negative, rounding down means adding one to
            // its absolute value. This cannot overflow.
            result = normalize(result);
            Vector<Digit, 16> finalResultVector(addOneLength(result));
            auto finalResult = absoluteAddOne(result, finalResultVector.mutableSpan());
            RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, sign, finalResult));
        }
    }

    RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, sign, result));
}

JSBigInt::ImplResult JSBigInt::rightShiftByMaximum(JSGlobalObject* globalObject, bool sign)
{
    if (sign)
        return createFrom(globalObject, -1);

    return createZero(globalObject->vm());
}

// Lookup table for the maximum number of bits required per character of a
// base-N string representation of a number. To increase accuracy, the array
// value is the actual value multiplied by 32. To generate this table:
// for (var i = 0; i <= 36; i++) { print(Math.ceil(Math.log2(i) * 32) + ","); }
constexpr auto maxBitsPerCharTable = WTF::toArray<uint8_t>({
    0,   0,   32,  51,  64,  75,  83,  90,  96, // 0..8
    102, 107, 111, 115, 119, 122, 126, 128,     // 9..16
    131, 134, 136, 139, 141, 143, 145, 147,     // 17..24
    149, 151, 153, 154, 156, 158, 159, 160,     // 25..32
    162, 163, 165, 166,                         // 33..36
});

static constexpr unsigned bitsPerCharTableShift = 5;
static constexpr size_t bitsPerCharTableMultiplier = 1u << bitsPerCharTableShift;

// Compute (an overapproximation of) the length of the resulting string:
// Divide bit length of the BigInt by bits representable per character.
uint64_t JSBigInt::calculateMaximumCharactersRequired(unsigned length, unsigned radix, Digit lastDigit, bool sign)
{
    unsigned leadingZeros = clz(lastDigit);

    size_t bitLength = length * digitBits - leadingZeros;

    // Maximum number of bits we can represent with one character. We'll use this
    // to find an appropriate chunk size below.
    uint8_t maxBitsPerChar = maxBitsPerCharTable[radix];

    // For estimating result length, we have to be pessimistic and work with
    // the minimum number of bits one character can represent.
    uint8_t minBitsPerChar = maxBitsPerChar - 1;

    // Perform the following computation with uint64_t to avoid overflows.
    uint64_t maximumCharactersRequired = bitLength;
    maximumCharactersRequired *= bitsPerCharTableMultiplier;

    // Round up.
    maximumCharactersRequired += minBitsPerChar - 1;
    maximumCharactersRequired /= minBitsPerChar;
    maximumCharactersRequired += sign;
    
    return maximumCharactersRequired;
}

String JSBigInt::toStringBasePowerOfTwo(VM& vm, JSGlobalObject* nullOrGlobalObjectForOOM, JSBigInt* x, unsigned radix)
{
    ASSERT(hasOneBitSet(radix));
    ASSERT(radix >= 2 && radix <= 32);
    ASSERT(!x->isZero());

    const unsigned length = x->length();
    const bool sign = x->sign();
    const unsigned bitsPerChar = ctz(radix);
    const unsigned charMask = radix - 1;
    // Compute the length of the resulting string: divide the bit length of the
    // BigInt by the number of bits representable per character (rounding up).
    const Digit msd = x->digit(length - 1);

    const unsigned msdLeadingZeros = clz(msd);

    const size_t bitLength = length * digitBits - msdLeadingZeros;
    const size_t charsRequired = (bitLength + bitsPerChar - 1) / bitsPerChar + sign;

    if (charsRequired > JSString::MaxLength) {
        if (nullOrGlobalObjectForOOM) {
            auto scope = DECLARE_THROW_SCOPE(vm);
            throwOutOfMemoryError(nullOrGlobalObjectForOOM, scope);
        }
        return String();
    }

    Vector<Latin1Character> resultString(charsRequired);
    Digit digit = 0;
    // Keeps track of how many unprocessed bits there are in {digit}.
    unsigned availableBits = 0;
    int pos = static_cast<int>(charsRequired - 1);
    for (unsigned i = 0; i < length - 1; i++) {
        Digit newDigit = x->digit(i);
        // Take any leftover bits from the last iteration into account.
        int current = (digit | (newDigit << availableBits)) & charMask;
        resultString[pos--] = radixDigits[current];
        int consumedBits = bitsPerChar - availableBits;
        digit = newDigit >> consumedBits;
        availableBits = digitBits - consumedBits;
        while (availableBits >= bitsPerChar) {
            resultString[pos--] = radixDigits[digit & charMask];
            digit >>= bitsPerChar;
            availableBits -= bitsPerChar;
        }
    }
    // Take any leftover bits from the last iteration into account.
    int current = (digit | (msd << availableBits)) & charMask;
    resultString[pos--] = radixDigits[current];
    digit = msd >> (bitsPerChar - availableBits);
    while (digit) {
        resultString[pos--] = radixDigits[digit & charMask];
        digit >>= bitsPerChar;
    }

    if (sign)
        resultString[pos--] = '-';

    ASSERT(pos == -1);
    return StringImpl::adopt(WTF::move(resultString));
}

// Divide-and-conquer conversion to string, ported from V8 [1]. The basic idea is to recursively
// cut the BigInt in half (using a division with remainder, the divisor being ~half as large (in
// bits) as the current dividend).
//
// As preparation, we build up a list of metadata for each recursion level. We do this bottom-up,
// i.e. start with the level that will produce two halves that are register-sized and bail out to
// the base case. Each higher level (executed earlier, prepared later) uses a divisor that is the
// square of the previously-created "next" level's divisor. Preparation terminates when the current
// divisor is at least half as large as the bigint. We also precompute each level's divisor's
// inverse, so we can use Barrett division later.
//
// Example: say we want to format 1234567890123, and we can fit two decimal digits into a register
// for the base case.
//
//              1234567890123
//                    ↓
//               %100000000 (a)              // Level 2,
//             /            \                // is the top level.
//         12345            67890123
//           ↓                  ↓
//    (e) %10000             %10000 (b)      // Level 1
//        /    \            /      \
//       1     2345      6789      0123
//       ↓   (f) ↓         ↓ (d)     ↓
// (g) %100    %100      %100      %100 (c)  // Level 0
//     / \     /   \     /   \     /   \
//    00 01   23   45   67   89   01   23
//        ↓    ↓    ↓    ↓    ↓    ↓    ↓    // Base case.
//       "1" "23" "45" "67" "89" "01" "23"
//
// We start building levels in order 0 -> 1 -> 2, performing the squarings 100² = 10000 and
// 10000² = 100000000 each only once. Execution then happens in order (a) through (g); lower-level
// divisors are used repeatedly. We build the string from right to left.
// Note that we can skip the division at (g) and fall through directly.
// Also, note that there are two chunks with value 1: one of them must produce a leading "0" in
// its string representation, the other must not.
//
// In this example, {chunkDivisor} is 100 and {chunkChars} is 2.
//
// [1]: https://source.chromium.org/chromium/chromium/src/+/main:v8/src/bigint/tostring.cc
// The number of digits from which the divide-and-conquer conversion beats the schoolbook loop in
// toStringGeneric. Measured on 64-bit for every radix (V8's crossover is 23); the 32-bit value is
// V8's.
#if CPU(REGISTER64)
static constexpr size_t toStringFastThreshold = 14;
#else
static constexpr size_t toStringFastThreshold = 38;
#endif

class JSBigInt::ToStringFormatter {
    WTF_MAKE_NONCOPYABLE(ToStringFormatter);
public:
    ToStringFormatter(InterruptCheck& interrupt, std::span<const Digit> digits, unsigned radix, std::span<Latin1Character> out)
        : m_interrupt(interrupt)
        , m_digits(digits)
        , m_radix(radix)
        , m_outStart(out.data())
        , m_outEnd(out.data() + out.size())
        , m_out(m_outEnd)
    {
        ASSERT(!m_digits.empty() && m_digits.back());
        uint8_t maxBitsPerChar = maxBitsPerCharTable[radix];
        m_chunkChars = digitBits * bitsPerCharTableMultiplier / maxBitsPerChar;
        m_chunkDivisor = digitPow(radix, m_chunkChars);
        // By construction of chunkChars, there can't have been overflow.
        ASSERT(m_chunkDivisor);
    }

    void run();

    // Returns the formatted characters, which end at the end of the output buffer.
    std::span<Latin1Character> result() const { return { m_out, m_outEnd }; }

private:
    struct Level {
        unsigned leadingZeroShift { 0 };
        // The number of characters generated by *each half* of this level.
        size_t charCount { 0 };
        Vector<Digit> divisor;
        Vector<Digit> inverse;
    };

    void createLevels(size_t targetBitLength);
    void computeInverse(Level&, size_t dividendLength);
    std::span<const Digit> inverse(const Level&, size_t dividendLength) const;
    Latin1Character* processLevel(size_t levelIndex, std::span<Digit> chunk, Latin1Character* out, bool isLastOnLevel, bool allowInPlaceShift);
    Latin1Character* fillWithZeros(size_t levelIndex, Latin1Character* rightBoundary, Latin1Character* out, bool isLastOnLevel);
    Latin1Character* basecaseLast(Digit, Latin1Character* out);

    InterruptCheck& m_interrupt;
    std::span<const Digit> m_digits;
    unsigned m_radix;
    unsigned m_chunkChars { 0 };
    Digit m_chunkDivisor { 0 };
    Latin1Character* m_outStart;
    Latin1Character* m_outEnd;
    Latin1Character* m_out;
    // Level i's divisor is the square of level i - 1's; the last one is the top level.
    Vector<Level> m_levels;
};

void JSBigInt::ToStringFormatter::createLevels(size_t targetBitLength)
{
    Level base;
    base.charCount = m_chunkChars;
    base.divisor.append(m_chunkDivisor);
    m_levels.append(WTF::move(base));
    // We can stop creating levels when the next level's divisor, which is the square of the
    // current level's divisor, would be strictly bigger (in terms of its numeric value) than the
    // input we're formatting. Since computing that next divisor is expensive, we want to predict
    // the necessity based on bit lengths. Bit lengths are an imperfect predictor of numeric value,
    // so we have to be careful:
    // - since we can't estimate which one of two numbers of equal bit length is bigger, we have to
    //   aim for a strictly bigger bit length.
    // - when squaring, the bit length sometimes doubles (e.g. 0b11² == 0b1001), but usually we
    //   "lose" a bit (e.g. 0b10² == 0b100).
    auto bitLength = [](std::span<const Digit> x) {
        return x.size() * digitBits - clz(x.back());
    };
    while (bitLength(m_levels.last().divisor.span()) * 2 - 1 <= targetBitLength) {
        Level next;
        auto& previous = m_levels.last();
        next.charCount = previous.charCount * 2;
        RELEASE_ASSERT(next.charCount < std::numeric_limits<size_t>::max() / 2);
        next.divisor = Vector<Digit>(previous.divisor.size() * 2);
        multiplyZeroPadded(m_interrupt, next.divisor.mutableSpan(), previous.divisor.span(), previous.divisor.span());
        if (m_interrupt.interrupted())
            return;
        next.divisor.shrink(normalize(next.divisor.span()).size());
        // Left-shifting the divisor must only happen after it's been used to compute the next
        // divisor.
        previous.leadingZeroShift = clz(previous.divisor.last());
        leftShift(previous.divisor.mutableSpan(), previous.divisor.span(), previous.leadingZeroShift);
        computeInverse(previous, 0);
        if (m_interrupt.interrupted())
            return;
        m_levels.append(WTF::move(next));
    }
    auto& top = m_levels.last();
    top.leadingZeroShift = clz(top.divisor.last());
    leftShift(top.divisor.mutableSpan(), top.divisor.span(), top.leadingZeroShift);
    // Not computing the top level's inverse here so that it can take the input's length into
    // account to save some effort on inverse generation.
}

// The top level might get by with a smaller inverse than we could maximally compute, so the caller
// should provide the dividend length.
void JSBigInt::ToStringFormatter::computeInverse(Level& level, size_t dividendLength)
{
    size_t inverseLength = level.divisor.size();
    if (dividendLength) {
        inverseLength = dividendLength - level.divisor.size();
        ASSERT(inverseLength <= level.divisor.size());
    }
    Vector<Digit> scratch(invertScratchSpace(inverseLength));
    level.inverse = Vector<Digit>(inverseLength + 1);
    auto input = level.divisor.span().subspan(level.divisor.size() - inverseLength, inverseLength);
    invert(m_interrupt, level.inverse.mutableSpan(), input, scratch.mutableSpan());
    if (m_interrupt.interrupted())
        return;
    ASSERT(!level.inverse.last());
    level.inverse.shrink(inverseLength);
}

std::span<const JSBigInt::Digit> JSBigInt::ToStringFormatter::inverse(const Level& level, size_t dividendLength) const
{
    ASSERT(!level.inverse.isEmpty());
    size_t inverseLength = dividendLength - level.divisor.size();
    ASSERT(inverseLength <= level.inverse.size());
    return level.inverse.span().subspan(level.inverse.size() - inverseLength);
}

void JSBigInt::ToStringFormatter::run()
{
    createLevels(m_digits.size() * digitBits - clz(m_digits.back()));
    if (m_interrupt.interrupted())
        return;
    // The input is read-only, so the top level works on a copy it may shift in place.
    Vector<Digit> input(m_digits.size());
    memcpySpan(input.mutableSpan(), m_digits);
    m_out = processLevel(m_levels.size() - 1, input.mutableSpan(), m_out, true, true);
}

// When processing the last (most significant) digit, don't write leading zeros.
Latin1Character* JSBigInt::ToStringFormatter::basecaseLast(Digit digit, Latin1Character* out)
{
    do {
        ASSERT(out > m_outStart);
        *(--out) = radixDigits[digit % m_radix];
        digit /= m_radix;
    } while (digit > 0);
    return out;
}

// Writes '0' characters right-to-left, starting at {out}-1, until the distance from
// {rightBoundary} to {out} equals the number of characters that the level is supposed to produce.
Latin1Character* JSBigInt::ToStringFormatter::fillWithZeros(size_t levelIndex, Latin1Character* rightBoundary, Latin1Character* out, bool isLastOnLevel)
{
    // Fill up with zeros up to the character count expected to be generated on this level; unless
    // this is the left edge of the result.
    if (isLastOnLevel)
        return out;
    size_t chunkChars = levelIndex == notFound ? m_chunkChars : m_levels[levelIndex].charCount * 2;
    Latin1Character* end = rightBoundary - chunkChars;
    ASSERT(out >= end);
    while (out > end)
        *(--out) = '0';
    return out;
}

// Formats {chunk} into the characters below {out}. {chunk} may be overwritten when
// {allowInPlaceShift} is set.
Latin1Character* JSBigInt::ToStringFormatter::processLevel(size_t levelIndex, std::span<Digit> chunk, Latin1Character* out, bool isLastOnLevel, bool allowInPlaceShift)
{
    // Step 0: if only one digit is left, bail out to the base case.
    auto normalized = normalize(std::span<const Digit>(chunk));
    if (normalized.size() <= 1) {
        Latin1Character* rightBoundary = out;
        if (normalized.size() == 1)
            out = basecaseLast(normalized[0], out);
        return fillWithZeros(levelIndex, rightBoundary, out, isLastOnLevel);
    }

    ASSERT(levelIndex != notFound);
    Level& level = m_levels[levelIndex];
    size_t nextLevelIndex = levelIndex ? levelIndex - 1 : notFound;
    auto divisor = level.divisor.span();

    // Step 1: If the chunk is guaranteed to remain smaller than the divisor even after
    // left-shifting, fall through to the next level immediately.
    if (normalized.size() < divisor.size()) {
        Latin1Character* rightBoundary = out;
        out = processLevel(nextLevelIndex, chunk, out, isLastOnLevel, allowInPlaceShift);
        return fillWithZeros(levelIndex, rightBoundary, out, isLastOnLevel);
    }

    // Step 2: Prepare the chunk: shift it left to match the normalized divisor. This is done in
    // place when the caller allows it and the top digit has room, since both halves below are
    // allocated one digit longer than they need for that purpose.
    unsigned shift = level.leadingZeroShift;
    Vector<Digit> shiftedStorage;
    std::span<Digit> shifted;
    bool shiftedInPlace = false;
    if (!shift)
        shifted = chunk.first(normalized.size());
    else if (allowInPlaceShift && clz(normalized.back()) >= shift) {
        shifted = chunk.first(normalized.size());
        leftShift(shifted, normalized, shift);
        shiftedInPlace = true;
    } else {
        shiftedStorage = Vector<Digit>(normalized.size() + 1);
        shifted = leftShift(shiftedStorage.mutableSpan(), normalized, shift);
    }
    auto dividend = normalize(std::span<const Digit>(shifted));
    // Check (now precisely) if the chunk is smaller than the divisor.
    auto comparison = compareDigits(dividend, divisor);
    if (comparison != ComparisonResult::GreaterThan) {
        Latin1Character* rightBoundary = out;
        if (comparison == ComparisonResult::LessThan) {
            // If the chunk is strictly smaller than the divisor, we can process it directly on the
            // next level as the right half, and know that the left half is all '0'.
            // In case we shifted {chunk} in-place, we must undo that before the call.
            if (shiftedInPlace)
                rightShift(shifted, shifted, shift);
            out = processLevel(nextLevelIndex, chunk, out, isLastOnLevel, allowInPlaceShift);
        } else {
            // If the chunk is equal to the divisor, we know that the right half is all '0', and the
            // left half is '...0001'.
            // Handling this case specially is an optimization; we could also fall through to the
            // generic "chunk > divisor" path below.
            out = fillWithZeros(nextLevelIndex, rightBoundary, out, false);
            ASSERT(out > m_outStart);
            *(--out) = '1';
        }
        // In both cases, make sure the left half is fully written.
        return fillWithZeros(levelIndex, rightBoundary, out, isLastOnLevel);
    }

    // Step 3: Allocate space for the results.
    // Allocate one extra digit so the next level can left-shift in-place.
    Vector<Digit> right(divisor.size() + 1);
    // Allocate one extra digit because divideBarrett requires it.
    Vector<Digit> left(dividend.size() - divisor.size() + 1);

    // Step 4: Divide to split {dividend} into {left} and {right}.
    size_t inverseLength = dividend.size() - divisor.size();
    if (!inverseLength) {
        auto [quotient, remainder] = divideSchoolbook(left.mutableSpan(), right.mutableSpan(), dividend, divisor, &m_interrupt);
        std::ranges::fill(left.mutableSpan().subspan(quotient.size()), 0);
        std::ranges::fill(right.mutableSpan().subspan(remainder.size()), 0);
    } else if (divisor.size() == 1) {
        Digit remainder = 0;
        auto quotient = divideSingle(left.mutableSpan(), remainder, dividend, divisor[0]);
        std::ranges::fill(left.mutableSpan().subspan(quotient.size()), 0);
        right[0] = remainder;
        std::ranges::fill(right.mutableSpan().subspan(1), 0);
        m_interrupt.addWork(dividend.size());
    } else {
        Vector<Digit> scratch(divideBarrettScratchSpace(dividend.size()));
        // The top level only computes its inverse when the dividend's length is known. Other
        // levels have precomputed theirs.
        if (level.inverse.isEmpty())
            computeInverse(level, dividend.size());
        if (m_interrupt.interrupted())
            return out;
        divideBarrett(m_interrupt, left.mutableSpan(), right.mutableSpan(), dividend, divisor, inverse(level, dividend.size()), scratch.mutableSpan());
    }
    if (m_interrupt.interrupted())
        return out;
    rightShiftZeroPadded(right.mutableSpan(), right.span(), shift);
    ASSERT(normalize(left.span()).size() <= divisor.size());

    // Step 5: Recurse.
    Latin1Character* endOfRightPart = processLevel(nextLevelIndex, right.mutableSpan(), out, false, true);
    if (m_interrupt.interrupted())
        return out;
    // The recursive calls are required and hence designed to write exactly as many characters as
    // their level is responsible for.
    ASSERT_UNUSED(endOfRightPart, endOfRightPart == out - level.charCount);
    return processLevel(nextLevelIndex, left.mutableSpan(), out - level.charCount, isLastOnLevel, true);
}

String JSBigInt::toStringGeneric(VM& vm, JSGlobalObject* nullOrGlobalObjectForOOM, JSBigInt* x, unsigned radix)
{
    // FIXME: [JSC] Revisit usage of Vector into JSBigInt::toString
    // https://bugs.webkit.org/show_bug.cgi?id=180671
    Vector<Latin1Character> resultString;

    ASSERT(radix >= 2 && radix <= 36);
    ASSERT(!x->isZero());

    unsigned length = x->length();
    bool sign = x->sign();

    uint8_t maxBitsPerChar = maxBitsPerCharTable[radix];
    uint64_t maximumCharactersRequired = calculateMaximumCharactersRequired(length, radix, x->digit(length - 1), sign);

    if (maximumCharactersRequired > JSString::MaxLength) {
        if (nullOrGlobalObjectForOOM) {
            auto scope = DECLARE_THROW_SCOPE(vm);
            throwOutOfMemoryError(nullOrGlobalObjectForOOM, scope);
        }
        return String();
    }

    if (length >= toStringFastThreshold) {
        Vector<Latin1Character> buffer(maximumCharactersRequired);
        InterruptCheck interrupt(nullOrGlobalObjectForOOM ? &vm : nullptr);
        ToStringFormatter formatter(interrupt, x->digits(), radix, buffer.mutableSpan());
        formatter.run();
        if (interrupt.interrupted()) [[unlikely]]
            return String();
        auto characters = formatter.result();
        ASSERT(!characters.empty());
        // Remove leading zeroes.
        size_t leadingZeroes = 0;
        while (leadingZeroes + 1 < characters.size() && characters[leadingZeroes] == '0')
            leadingZeroes++;
        characters = characters.subspan(leadingZeroes);
        size_t resultLength = characters.size() + sign;
        auto result = buffer.mutableSpan().first(resultLength);
        memmoveSpan(result.subspan(sign), characters);
        if (sign)
            result[0] = '-';
        buffer.shrink(resultLength);
        return StringImpl::adopt(WTF::move(buffer));
    }

    Digit lastDigit;
    if (length == 1)
        lastDigit = x->digit(0);
    else {
        unsigned chunkChars = digitBits * bitsPerCharTableMultiplier / maxBitsPerChar;
        Digit chunkDivisor = digitPow(radix, chunkChars);

        // By construction of chunkChars, there can't have been overflow.
        ASSERT(chunkDivisor);

        // {rest} holds the part of the BigInt that we haven't looked at yet.
        // Not to be confused with "remainder"!
        // In the first round, divide the input, allocating a new BigInt for
        // the result == rest; from then on divide the rest in-place.
        Vector<Digit, 16> rest(length);
        std::span<const Digit> dividend = x->digits();
        do {
            Digit chunk;
            std::span<Digit> quotient = divideSingle(rest.mutableSpan(), chunk, dividend, chunkDivisor);

            for (unsigned i = 0; i < chunkChars; i++) {
                resultString.append(radixDigits[chunk % radix]);
                chunk /= radix;
            }
            ASSERT(!chunk);

            // Update dividend to point to the quotient for next iteration.
            // The quotient.size() tells us how many digits are non-zero.
            dividend = normalize(quotient);
        } while (dividend.size() > 1);

        lastDigit = dividend.empty() ? 0 : dividend[0];
    }

    do {
        resultString.append(radixDigits[lastDigit % radix]);
        lastDigit /= radix;
    } while (lastDigit > 0);
    ASSERT(resultString.size());
    ASSERT(resultString.size() <= static_cast<size_t>(maximumCharactersRequired));

    // Remove leading zeroes.
    unsigned newSizeNoLeadingZeroes = resultString.size();
    while (newSizeNoLeadingZeroes  > 1 && resultString[newSizeNoLeadingZeroes - 1] == '0')
        newSizeNoLeadingZeroes--;

    resultString.shrink(newSizeNoLeadingZeroes);

    if (sign)
        resultString.append('-');

    std::ranges::reverse(resultString);

    return StringImpl::adopt(WTF::move(resultString));
}

double JSBigInt::toNumber(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwTypeError(globalObject, scope, "Conversion from 'BigInt' to 'number' is not allowed."_s);
    return 0.0;
}

// Numerical value of the first 128 ASCII characters, using 255 as sentinel for "invalid".
static constexpr auto charValueTable = WTF::toArray<uint8_t>({
    255, 255, 255, 255, 255, 255, 255, 255, // 0..7
    255, 255, 255, 255, 255, 255, 255, 255, // 8..15
    255, 255, 255, 255, 255, 255, 255, 255, // 16..23
    255, 255, 255, 255, 255, 255, 255, 255, // 24..31
    255, 255, 255, 255, 255, 255, 255, 255, // 32..39
    255, 255, 255, 255, 255, 255, 255, 255, // 40..47
    0, 1, 2, 3, 4, 5, 6, 7, // 48..55    '0' == 48
    8, 9, 255, 255, 255, 255, 255, 255, // 56..63    '9' == 57
    255, 10, 11, 12, 13, 14, 15, 16, // 64..71    'A' == 65
    17, 18, 19, 20, 21, 22, 23, 24, // 72..79
    25, 26, 27, 28, 29, 30, 31, 32, // 80..87
    33, 34, 35, 255, 255, 255, 255, 255, // 88..95    'Z' == 90
    255, 10, 11, 12, 13, 14, 15, 16, // 96..103   'a' == 97
    17, 18, 19, 20, 21, 22, 23, 24, // 104..111
    25, 26, 27, 28, 29, 30, 31, 32, // 112..119
    33, 34, 35, 255, 255, 255, 255, 255, // 120..127  'z' == 122
});

template<typename CharType>
static ALWAYS_INLINE unsigned digitCharValue(CharType character)
{
    if (static_cast<unsigned>(character) >= charValueTable.size())
        return 255;
    return charValueTable[character];
}

// The number of parts, each holding as many characters as fit one digit, from which the balanced
// combination below beats the multiplyAdd loop in parseInt. That loop advances by the characters
// that fit an int32, about half a digit, so the crossover is far below V8's 25 parts: measured on
// 64-bit, 3 parts break even and 4 win for every radix. The 32-bit value is V8's.
#if CPU(REGISTER64)
static constexpr size_t fromStringLargeThreshold = 4;
#else
static constexpr size_t fromStringLargeThreshold = 165;
#endif
static_assert(fromStringLargeThreshold >= 3);

// The number of characters of the given radix whose value fits one digit, and that radix power.
struct CharactersPerDigit {
    unsigned count;
    JSBigInt::Digit multiplier;
};

static constexpr CharactersPerDigit computeCharactersPerDigit(unsigned radix)
{
    JSBigInt::Digit multiplier = radix;
    unsigned count = 1;
    while (multiplier <= std::numeric_limits<JSBigInt::Digit>::max() / radix) {
        multiplier *= radix;
        count++;
    }
    return { count, multiplier };
}

static constexpr auto charactersPerDigitTable = [] {
    std::array<CharactersPerDigit, 37> table { };
    for (unsigned radix = 2; radix <= 36; radix++)
        table[radix] = computeCharactersPerDigit(radix);
    return table;
}();

// Combines the parts in a balanced-binary-tree like order, ported from V8 [1]: multiply-and-add
// neighboring pairs of parts, then loop, until only one part is left. The benefit is that the
// multiplications will have inputs of similar sizes, which makes them amenable to fast
// multiplication algorithms. We have to do more multiplications than the classic algorithm though,
// because we also have to multiply the multipliers.
// Optimizations:
// - We can skip the multiplier for the first part, because we never need it.
// - Most multipliers are the same; we can avoid repeated multiplications and just copy the
//   previous result. (In theory we could even de-dupe them, but as the parts/multipliers grow,
//   we'll need most of the memory anyway.) Copied results are marked with a * below.
// - We can reuse memory using a system of three buffers whose usage rotates:
//   - one is considered empty, and is overwritten with the new parts,
//   - one holds the multipliers (and will be "empty" in the next round), and
//   - one initially holds the parts and is overwritten with the new multipliers
//   Parts and multipliers both grow in each iteration, and get fewer, so we use the space of two
//   adjacent old chunks for one new chunk.
//   {z} is also big enough, but it's convenient to let only the last round write into it, so the
//   result always ends up in the right place without needing another copy. So we need to
//   allocate two scratch vectors.
// - We don't have to keep track of the positions and sizes of the chunks, because we can deduce
//   their precise placement from the iteration index. Chunks at the end of a buffer are shorter,
//   which the clamped subspans express.
//
// Example, assuming a digit is 4 bits, fitting one decimal digit. Initial state:
//
//     parts          1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
//     multipliers   10 10 10 10 10 10 10 10 10 10 10 10 10 10 10
//
// After the first iteration of the outer loop:
//
//     parts          12    34    56    78    90    12    34    5
//     multipliers         100  *100  *100  *100  *100  *100   10
//
// After the second iteration:
//
//     parts          1234        5678        9012        345
//     multipliers               10000      *10000       1000
//
// After the third iteration:
//
//     parts          12345678                9012345
//     multipliers                           10000000
//
// And then there's an obvious last iteration.
//
// [1]: https://source.chromium.org/chromium/chromium/src/+/main:v8/src/bigint/fromstring.cc
void JSBigInt::fromStringLarge(InterruptCheck& interrupt, std::span<Digit> z, std::span<Digit> parts, Digit maxMultiplier, Digit lastMultiplier)
{
    size_t numParts = parts.size();
    // The first round below never writes to z, and the loop after it only runs once there are at
    // least two parts left, so two parts would leave z untouched.
    ASSERT(numParts >= 3);
    ASSERT(z.size() >= numParts);
    Vector<Digit> tempStorage(numParts * 2);
    auto multipliers = tempStorage.mutableSpan().first(numParts);
    auto temp = tempStorage.mutableSpan().subspan(numParts, numParts);
    // Unrolled and specialized first iteration: partLength == 1, so instead of digit sub-vectors
    // we have individual digit values, and the multipliers are known up front.
    {
        auto newParts = temp;
        auto newMultipliers = parts;
        size_t i = 0;
        for (; i + 1 < numParts; i += 2) {
            Digit pIn = parts[i];
            Digit pIn2 = parts[i + 1];
            Digit mIn = maxMultiplier;
            Digit mIn2 = i == numParts - 2 ? lastMultiplier : maxMultiplier;
            // p[j] = p[i] * m[i+1] + p[i+1]
            auto [pLow, pHigh] = digitMul(pIn, mIn2);
            Digit carry = 0;
            newParts[i] = digitAdd(pLow, pIn2, carry);
            newParts[i + 1] = pHigh + carry;
            // m[j] = m[i] * m[i+1]
            if (i > 0) {
                if (i > 2 && mIn2 != lastMultiplier) {
                    newMultipliers[i] = newMultipliers[i - 2];
                    newMultipliers[i + 1] = newMultipliers[i - 1];
                } else {
                    auto [mLow, mHigh] = digitMul(mIn, mIn2);
                    newMultipliers[i] = mLow;
                    newMultipliers[i + 1] = mHigh;
                }
            }
        }
        // Trailing last part (if {numParts} was odd).
        if (i < numParts) {
            newParts[i] = parts[i];
            newMultipliers[i] = lastMultiplier;
            i += 2;
        }
        numParts = i >> 1;
        auto newTemp = multipliers;
        parts = newParts;
        multipliers = newMultipliers;
        temp = newTemp;
    }
    size_t partLength = 2;

    // Remaining iterations.
    while (numParts > 1) {
        // In the very last iteration, write into {z}.
        auto newParts = numParts == 2 ? z : temp;
        auto newMultipliers = parts;
        size_t newPartLength = partLength * 2;
        size_t i = 0;
        for (; i + 1 < numParts; i += 2) {
            size_t start = i * partLength;
            auto pIn = clampedSubspan(parts, start, partLength);
            auto pIn2 = clampedSubspan(parts, start + partLength, partLength);
            auto mIn = clampedSubspan(multipliers, start, partLength);
            auto mIn2 = clampedSubspan(multipliers, start + partLength, partLength);
            auto pOut = clampedSubspan(newParts, start, newPartLength);
            auto mOut = clampedSubspan(newMultipliers, start, newPartLength);
            // p[j] = p[i] * m[i+1] + p[i+1]
            multiplyZeroPadded(interrupt, pOut, pIn, mIn2);
            if (interrupt.interrupted())
                return;
            Digit overflow = inplaceAddAndPropagate(pOut, pIn2);
            ASSERT_UNUSED(overflow, !overflow);
            // m[j] = m[i] * m[i+1]
            if (i > 0) {
                bool copied = false;
                if (i > 2) {
                    size_t previousStart = (i - 2) * partLength;
                    auto mInPrevious = clampedSubspan(multipliers, previousStart, partLength);
                    auto mIn2Previous = clampedSubspan(multipliers, previousStart + partLength, partLength);
                    if (compareDigits(mIn, mInPrevious) == ComparisonResult::Equal && compareDigits(mIn2, mIn2Previous) == ComparisonResult::Equal) {
                        copied = true;
                        auto mOutPrevious = clampedSubspan(newMultipliers, previousStart, newPartLength);
                        memcpySpan(mOut, mOutPrevious.first(mOut.size()));
                    }
                }
                if (!copied) {
                    multiplyZeroPadded(interrupt, mOut, mIn, mIn2);
                    if (interrupt.interrupted())
                        return;
                }
            }
        }
        // Trailing last part (if {numParts} was odd).
        if (i < numParts) {
            auto pIn = clampedSubspan(parts, i * partLength, partLength);
            auto mIn = clampedSubspan(multipliers, i * partLength, partLength);
            auto pOut = clampedSubspan(newParts, i * partLength, newPartLength);
            auto mOut = clampedSubspan(newMultipliers, i * partLength, newPartLength);
            copyZeroPadded(pOut, pIn);
            copyZeroPadded(mOut, mIn);
            i += 2;
        }
        numParts = i >> 1;
        partLength = newPartLength;
        auto newTemp = multipliers;
        parts = newParts;
        multipliers = newMultipliers;
        temp = newTemp;
    }
    // z might be bigger than we requested; be robust towards that.
    std::ranges::fill(z.subspan(std::min(partLength, z.size())), 0);
}

// The digits of a string of characters in a non-power-of-two radix, given the number of
// characters that fit one digit. Combines them with fromStringLarge above, and returns false on
// an invalid character.
template<typename CharType>
bool JSBigInt::parseDigitsLarge(InterruptCheck& interrupt, std::span<Digit> result, std::span<const CharType> characters, unsigned radix, unsigned charsPerPart, Digit maxMultiplier)
{
    size_t numParts = (characters.size() + charsPerPart - 1) / charsPerPart;
    ASSERT(result.size() >= numParts);
    ASSERT(numParts >= 3);
    Vector<Digit> partsStorage(numParts);
    auto parts = partsStorage.mutableSpan();
    size_t position = 0;
    Digit lastMultiplier = maxMultiplier;
    for (size_t i = 0; i < numParts; i++) {
        size_t count = std::min<size_t>(charsPerPart, characters.size() - position);
        Digit part = 0;
        Digit multiplier = 1;
        for (size_t j = 0; j < count; j++) {
            unsigned value = digitCharValue(characters[position + j]);
            if (value >= radix)
                return false;
            part = part * radix + value;
            multiplier *= radix;
        }
        parts[i] = part;
        lastMultiplier = multiplier;
        position += count;
    }
    fromStringLarge(interrupt, result, parts, maxMultiplier, lastMultiplier);
    return true;
}

// The digits of a string of characters in a power-of-two radix: each character contributes
// exactly ctz(radix) bits, so they are packed from the least significant character up. Returns
// false on an invalid character.
template<typename CharType>
bool JSBigInt::parseDigitsPowerOfTwo(std::span<Digit> result, std::span<const CharType> characters, unsigned radix)
{
    ASSERT(hasOneBitSet(radix));
    unsigned bitsPerChar = ctz(radix);
    ASSERT(result.size() * digitBits >= characters.size() * bitsPerChar);
    size_t digitIndex = 0;
    Digit digit = 0;
    unsigned bitsInDigit = 0;
    for (size_t i = characters.size(); i-- > 0;) {
        unsigned value = digitCharValue(characters[i]);
        if (value >= radix)
            return false;
        digit |= static_cast<Digit>(value) << bitsInDigit;
        bitsInDigit += bitsPerChar;
        if (bitsInDigit >= digitBits) {
            result[digitIndex++] = digit;
            bitsInDigit -= digitBits;
            // The bits of this character that did not fit, if any.
            digit = bitsInDigit ? static_cast<Digit>(value) >> (bitsPerChar - bitsInDigit) : 0;
        }
    }
    if (bitsInDigit)
        result[digitIndex++] = digit;
    std::ranges::fill(result.subspan(digitIndex), 0);
    return true;
}

template <typename CharType>
JSValue JSBigInt::parseInt(JSGlobalObject* globalObject, std::span<const CharType> data, ErrorParseMode errorParseMode)
{
    VM& vm = globalObject->vm();

    size_t p = 0;
    while (p < data.size() && isStrWhiteSpace(data[p]))
        ++p;

    // Check Radix from first characters
    if (p + 1 < data.size() && data[p] == '0') {
        if (isASCIIAlphaCaselessEqual(data[p + 1], 'b'))
            return parseInt(globalObject, vm, data, p + 2, 2, errorParseMode, ParseIntSign::Unsigned, ParseIntMode::DisallowEmptyString);
        
        if (isASCIIAlphaCaselessEqual(data[p + 1], 'x'))
            return parseInt(globalObject, vm, data, p + 2, 16, errorParseMode, ParseIntSign::Unsigned, ParseIntMode::DisallowEmptyString);
        
        if (isASCIIAlphaCaselessEqual(data[p + 1], 'o'))
            return parseInt(globalObject, vm, data, p + 2, 8, errorParseMode, ParseIntSign::Unsigned, ParseIntMode::DisallowEmptyString);
    }

    ParseIntSign sign = ParseIntSign::Unsigned;
    if (p < data.size()) {
        if (data[p] == '-') {
            sign = ParseIntSign::Signed;
            ++p;
        } else if (data[p] == '+')
            ++p;
    }

    return parseInt(globalObject, vm, data, p, 10, errorParseMode, sign);
}

template <typename CharType>
JSValue JSBigInt::parseInt(JSGlobalObject* nullOrGlobalObjectForOOM, VM& vm, std::span<const CharType> data, unsigned startIndex, unsigned radix, ErrorParseMode errorParseMode, ParseIntSign sign, ParseIntMode parseMode)
{
    size_t p = startIndex;

    if (parseMode != ParseIntMode::AllowEmptyString && startIndex == data.size()) {
        ASSERT(nullOrGlobalObjectForOOM);
        if (errorParseMode == ErrorParseMode::ThrowExceptions) {
            auto scope = DECLARE_THROW_SCOPE(vm);
            throwVMError(nullOrGlobalObjectForOOM, scope, createSyntaxError(nullOrGlobalObjectForOOM, "Failed to parse String to BigInt"_s));
        }
        return JSValue();
    }

    // Skipping leading zeros
    while (p < data.size() && data[p] == '0')
        ++p;

    int endIndex = data.size() - 1;
    // Removing trailing spaces
    while (endIndex >= static_cast<int>(p) && isStrWhiteSpace(data[endIndex]))
        --endIndex;

    size_t length = endIndex + 1;

    if (p == length) {
#if USE(BIGINT32)
        return jsBigInt32(0);
#else
        return createZero(vm);
#endif
    }

    unsigned lengthLimitForBigInt32;
#if USE(BIGINT32)
    static_assert(sizeof(Digit) >= sizeof(uint64_t));
    // The idea is to pick the limit such that:
    // radix ** lengthLimitForBigInt32 >= INT32_MAX
    // radix ** (lengthLimitForBigInt32 - 1) <= INT32_MAX
#if ASSERT_ENABLED
    auto limitWorks = [&] {
        double lengthLimit = lengthLimitForBigInt32;
        double lowerLimit = pow(static_cast<double>(radix), lengthLimit - 1);
        double upperLimit = pow(static_cast<double>(radix), lengthLimit);
        double target = std::numeric_limits<int32_t>::max();
        return lowerLimit <= target && target <= upperLimit && upperLimit <= std::numeric_limits<int64_t>::max();
    };
#endif
    switch (radix) {
    case 2:
        lengthLimitForBigInt32 = 31;
        ASSERT(limitWorks());
        break;
    case 8:
        lengthLimitForBigInt32 = 11;
        ASSERT(limitWorks());
        break;
    case 10:
        lengthLimitForBigInt32 = 10;
        ASSERT(limitWorks());
        break;
    case 16:
        lengthLimitForBigInt32 = 8;
        ASSERT(limitWorks());
        break;
    default:
        lengthLimitForBigInt32 = 1;
        break;
    }
#else
    // The idea is to pick the largest limit such that:
    // radix ** lengthLimitForBigInt32 <= INT32_MAX
#if ASSERT_ENABLED
    auto limitWorks = [&] {
        double lengthLimit = lengthLimitForBigInt32;
        double valueLimit = pow(static_cast<double>(radix), lengthLimit);
        double overValueLimit = pow(static_cast<double>(radix), lengthLimit + 1);
        double target = std::numeric_limits<int32_t>::max();
        return valueLimit <= target && target < overValueLimit;
    };
#endif
    switch (radix) {
    case 2:
        lengthLimitForBigInt32 = 30;
        ASSERT(limitWorks());
        break;
    case 8:
        lengthLimitForBigInt32 = 10;
        ASSERT(limitWorks());
        break;
    case 10:
        lengthLimitForBigInt32 = 9;
        ASSERT(limitWorks());
        break;
    case 16:
        lengthLimitForBigInt32 = 7;
        ASSERT(limitWorks());
        break;
    default:
        lengthLimitForBigInt32 = 1;
        break;
    }
#endif // USE(BIGINT32)

    auto computeLength = [](unsigned radix, unsigned charcount) -> std::optional<unsigned> {
        ASSERT(2 <= radix && radix <= 36);

        size_t bitsPerChar = maxBitsPerCharTable[radix];
        size_t chars = charcount;
        const unsigned roundup = bitsPerCharTableMultiplier - 1;
        if (chars <= (std::numeric_limits<size_t>::max() - roundup) / bitsPerChar) {
            size_t bitsMin = bitsPerChar * chars;

            // Divide by 32 (see table), rounding up.
            bitsMin = (bitsMin + roundup) >> bitsPerCharTableShift;
            if (bitsMin <= static_cast<size_t>(maxInt)) {
                // Divide by kDigitsBits, rounding up.
                unsigned length = (bitsMin + digitBits - 1) / digitBits;
                if (length <= maxLength)
                    return length;
            }
        }

        return std::nullopt;
    };

    unsigned initialLength = length - p;

    // Inputs too long for the multiplyAdd loop below to stay cheap are parsed in one of the
    // linear-time ways: packing bits for a power-of-two radix, or combining digit-sized parts
    // in a balanced tree otherwise. Inputs that may fit a BigInt32 keep the loop. The comparisons
    // spell out ceil(initialLength / charsPerPart) >= fromStringLargeThreshold without the
    // division, since they run on every parse; the length check in front of the table load is
    // implied by the part count (over 30 characters for every radix) and only short-circuits it.
    {
        bool isPowerOfTwoRadix = hasOneBitSet(radix);
        bool useLinearParse = isPowerOfTwoRadix
            ? initialLength > lengthLimitForBigInt32
            : initialLength > 30 && initialLength > (fromStringLargeThreshold - 1) * charactersPerDigitTable[radix].count;
        if (useLinearParse) {
            auto [charsPerPart, maxMultiplier] = charactersPerDigitTable[radix];
            size_t numParts = (initialLength + charsPerPart - 1) / charsPerPart;
            auto characters = data.subspan(p, initialLength);
            auto resultLength = computeLength(radix, initialLength);
            if (!resultLength) [[unlikely]] {
                if (nullOrGlobalObjectForOOM) {
                    auto scope = DECLARE_THROW_SCOPE(vm);
                    throwOutOfMemoryError(nullOrGlobalObjectForOOM, scope, "BigInt generated from this operation is too big"_s);
                }
                return JSValue();
            }
            // The parts can outnumber the digits of the result by one: the last part is short,
            // and the bit estimate above is tighter than a digit per part.
            Vector<Digit> resultVector(std::max<size_t>(resultLength.value(), numParts));
            InterruptCheck interrupt(nullOrGlobalObjectForOOM ? &vm : nullptr);
            bool valid = isPowerOfTwoRadix
                ? parseDigitsPowerOfTwo(resultVector.mutableSpan(), characters, radix)
                : parseDigitsLarge(interrupt, resultVector.mutableSpan(), characters, radix, charsPerPart, maxMultiplier);
            if (interrupt.interrupted()) [[unlikely]]
                return JSValue();
            if (!valid) {
                if (errorParseMode == ErrorParseMode::ThrowExceptions) {
                    auto scope = DECLARE_THROW_SCOPE(vm);
                    ASSERT(nullOrGlobalObjectForOOM);
                    throwVMError(nullOrGlobalObjectForOOM, scope, createSyntaxError(nullOrGlobalObjectForOOM, "Failed to parse String to BigInt"_s));
                }
                return JSValue();
            }
            return tryCreateFromImpl(nullOrGlobalObjectForOOM, vm, sign == ParseIntSign::Signed, resultVector.span());
        }
    }

    unsigned limit0 = '0' + (radix < 10 ? radix : 10);
    unsigned limita = 'a' + (static_cast<int32_t>(radix) - 10);
    unsigned limitA = 'A' + (static_cast<int32_t>(radix) - 10);
    Vector<Digit, 16> resultVector;
    while (p < length) {
        Checked<uint64_t, CrashOnOverflow> digit = 0;
        Checked<uint64_t, CrashOnOverflow> multiplier = 1;
        for (unsigned i = 0; i < lengthLimitForBigInt32 && p < length; ++i, ++p) {
            digit *= radix;
            multiplier *= radix;
            if (data[p] >= '0' && data[p] < limit0)
                digit += static_cast<uint64_t>(data[p] - '0');
            else if (data[p] >= 'a' && data[p] < limita)
                digit += static_cast<uint64_t>(data[p] - 'a' + 10);
            else if (data[p] >= 'A' && data[p] < limitA)
                digit += static_cast<uint64_t>(data[p] - 'A' + 10);
            else {
                if (errorParseMode == ErrorParseMode::ThrowExceptions) {
                    auto scope = DECLARE_THROW_SCOPE(vm);
                    ASSERT(nullOrGlobalObjectForOOM);
                    throwVMError(nullOrGlobalObjectForOOM, scope, createSyntaxError(nullOrGlobalObjectForOOM, "Failed to parse String to BigInt"_s));
                }
                return JSValue();
            }
        }

        if (resultVector.isEmpty()) {
            if (p == length) {
                ASSERT(digit <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
                int64_t maybeResult = digit;
                ASSERT(maybeResult >= 0);
                if (sign == ParseIntSign::Signed)
                    maybeResult *= -1;

                if (static_cast<int64_t>(static_cast<int32_t>(maybeResult)) == maybeResult) {
#if USE(BIGINT32)
                    return jsBigInt32(static_cast<int32_t>(maybeResult));
#else
                    return JSBigInt::createFrom(nullOrGlobalObjectForOOM, vm, static_cast<int32_t>(maybeResult));
#endif
                }
            }

            auto length = computeLength(radix, initialLength);
            if (!length) [[unlikely]] {
                if (nullOrGlobalObjectForOOM) {
                    auto scope = DECLARE_THROW_SCOPE(vm);
                    throwOutOfMemoryError(nullOrGlobalObjectForOOM, scope, "BigInt generated from this operation is too big"_s);
                }
                return JSValue();
            }

            resultVector.fill(0, length.value());
        }

        ASSERT(static_cast<uint64_t>(static_cast<Digit>(multiplier)) == multiplier);
        ASSERT(static_cast<uint64_t>(static_cast<Digit>(digit)) == digit);
        multiplyAdd(resultVector.span(), static_cast<Digit>(multiplier), static_cast<Digit>(digit), resultVector.mutableSpan());
    }

    return tryCreateFromImpl(nullOrGlobalObjectForOOM, vm, sign == ParseIntSign::Signed, resultVector.span());
}

JSObject* JSBigInt::toObject(JSGlobalObject* globalObject) const
{
    return BigIntObject::create(globalObject->vm(), globalObject, const_cast<JSBigInt*>(this));
}

bool JSBigInt::equalsToNumber(JSValue numValue)
{
    ASSERT(numValue.isNumber());
    
    if (numValue.isInt32())
        return equalsToInt32(numValue.asInt32());
    
    double value = numValue.asDouble();
    return compareToDouble(this, value) == ComparisonResult::Equal;
}

bool JSBigInt::equalsToInt32(int32_t value)
{
    if (!value)
        return this->isZero();
    return (this->length() == 1) && (this->sign() == (value < 0)) && (this->digit(0) == static_cast<Digit>(std::abs(static_cast<int64_t>(value))));
}

JSBigInt::ComparisonResult JSBigInt::compareToDouble(JSBigInt* x, double y)
{
    return compareToDouble(HeapBigIntImpl { x }, y);
}

JSBigInt::ComparisonResult JSBigInt::compareToDouble(double x, JSBigInt* y)
{
    return compareToDouble(x, HeapBigIntImpl { y });
}

template <typename BigIntImpl>
JSBigInt::ComparisonResult JSBigInt::compareToDouble(BigIntImpl x, double y)
{
    // This algorithm expect that the double format is IEEE 754

    uint64_t doubleBits = std::bit_cast<uint64_t>(y);
    int rawExponent = static_cast<int>(doubleBits >> 52) & 0x7FF;

    // Handle finite doubles for {y}.
    if (rawExponent == 0x7FF) {
        if (std::isnan(y))
            return ComparisonResult::Undefined;

        return (y == std::numeric_limits<double>::infinity()) ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;
    }

    bool xSign = x.sign();
    
    // Note that this is different from the double's sign bit for -0. That's
    // intentional because -0 must be treated like 0.
    bool ySign = y < 0;
    if (xSign != ySign)
        return xSign ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;

    if (!y) {
        // If {y} is zero, then ySign is false and xSign must be false.
        ASSERT(!xSign);
        return x.isZero() ? ComparisonResult::Equal : ComparisonResult::GreaterThan;
    }

    if (x.isZero()) {
        // If {x} is zero, then xSign is false and ySign must be false which indicates that {y} is greater than zero.
        ASSERT(!ySign && y > 0);
        return ComparisonResult::LessThan;
    }

    // Right now, only two cases left:
    //     {x} >= 1 and {y} > 0
    //     {x} <= -1 and {y} < 0

    // Non-finite doubles are handled above.
    ASSERT(rawExponent != 0x7FF);
    int exponent = rawExponent - 0x3FF;
    if (exponent < 0) {
        // The absolute value of the double is less than 1. Only 0n has an
        // absolute value smaller than that, but we've already covered that case.
        // Note that this also handles denormal doubles for {y}.
        return xSign ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;
    }

    int xLength = x.length();
    Digit xMSD = x.digit(xLength - 1);
    int msdLeadingZeros = clz(xMSD);

    int xBitLength = xLength * digitBits - msdLeadingZeros;
    int yBitLength = exponent + 1;
    if (xBitLength < yBitLength)
        return xSign? ComparisonResult::GreaterThan : ComparisonResult::LessThan;

    if (xBitLength > yBitLength)
        return xSign ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;
    
    // At this point, we know that signs and bit lengths (i.e. position of
    // the most significant bit in exponent-free representation) are identical.
    // {x} is not zero, {y} is finite and not denormal.
    // Now we virtually convert the double to an integer by shifting its
    // mantissa according to its exponent, so it will align with the BigInt {x},
    // and then we compare them bit for bit until we find a difference or the
    // least significant bit.
    //                    <----- 52 ------> <-- virtual trailing zeroes -->
    // y / mantissa:     1yyyyyyyyyyyyyyyyy 0000000000000000000000000000000
    // x / digits:    0001xxxx xxxxxxxx xxxxxxxx ...
    //                    <-->          <------>
    //              msdTopBit         digitBits
    //
    uint64_t mantissa = doubleBits & 0x000FFFFFFFFFFFFF;
    mantissa |= 0x0010000000000000;
    const int mantissaTopBit = 52; // 0-indexed.

    // 0-indexed position of {x}'s most significant bit within the {msd}.
    int msdTopBit = digitBits - 1 - msdLeadingZeros;
    ASSERT(msdTopBit == static_cast<int>((xBitLength - 1) % digitBits));
    
    // Shifted chunk of {mantissa} for comparing with {digit}.
    Digit compareMantissa;

    // Number of unprocessed bits in {mantissa}. We'll keep them shifted to
    // the left (i.e. most significant part) of the underlying uint64_t.
    int remainingMantissaBits = 0;
    
    // First, compare the most significant digit against the beginning of
    // the mantissa and then we align them.
    if (msdTopBit < mantissaTopBit) {
        remainingMantissaBits = (mantissaTopBit - msdTopBit);
        compareMantissa = static_cast<Digit>(mantissa >> remainingMantissaBits);
        mantissa = mantissa << (64 - remainingMantissaBits);
    } else {
        compareMantissa = static_cast<Digit>(mantissa << (msdTopBit - mantissaTopBit));
        mantissa = 0;
    }

    if (xMSD > compareMantissa)
        return xSign ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;

    if (xMSD < compareMantissa)
        return xSign ? ComparisonResult::GreaterThan : ComparisonResult::LessThan;
    
    // Then, compare additional digits against any remaining mantissa bits.
    for (int digitIndex = xLength - 2; digitIndex >= 0; digitIndex--) {
        if (remainingMantissaBits > 0) {
            remainingMantissaBits -= digitBits;
            if (sizeof(mantissa) != sizeof(xMSD)) {
                compareMantissa = static_cast<Digit>(mantissa >> (64 - digitBits));
                // "& 63" to appease compilers. digitBits is 32 here anyway.
                mantissa = mantissa << (digitBits & 63);
            } else {
                compareMantissa = static_cast<Digit>(mantissa);
                mantissa = 0;
            }
        } else
            compareMantissa = 0;

        Digit digit = x.digit(digitIndex);
        if (digit > compareMantissa)
            return xSign ? ComparisonResult::LessThan : ComparisonResult::GreaterThan;
        if (digit < compareMantissa)
            return xSign ? ComparisonResult::GreaterThan : ComparisonResult::LessThan;
    }

    // Integer parts are equal; check whether {y} has a fractional part.
    if (mantissa) {
        ASSERT(remainingMantissaBits > 0);
        return xSign ? ComparisonResult::GreaterThan : ComparisonResult::LessThan;
    }

    return ComparisonResult::Equal;
}

JSBigInt::ComparisonResult JSBigInt::compareToDouble(int32_t x, double y)
{
    return compareToDouble(Int32BigIntImpl { x }, y);
}

JSBigInt::ComparisonResult JSBigInt::compareToDouble(int64_t x, double y)
{
    return compareToDouble(Int64BigIntImpl { x }, y);
}

JSBigInt::ComparisonResult JSBigInt::compareToDouble(uint64_t x, double y)
{
    return compareToDouble(Int64BigIntImpl { x }, y);
}

JSBigInt::ComparisonResult JSBigInt::compareToDouble(JSValue x, double y)
{
    ASSERT(x.isBigInt());
#if USE(BIGINT32)
    if (x.isBigInt32())
        return compareToDouble(x.bigInt32AsInt32(), y);
#endif
    return compareToDouble(x.asHeapBigInt(), y);
}

template <typename BigIntImpl>
std::optional<JSBigInt::Digit> JSBigInt::toShiftAmount(BigIntImpl x)
{
    if (x.length() > 1)
        return std::nullopt;
    
    Digit value = x.digit(0);
    static_assert(maxLengthBits < std::numeric_limits<Digit>::max(), "maxLengthBits needs to be less than digit");
    
    if (value > maxLengthBits)
        return std::nullopt;

    return value;
}

JSBigInt::RoundingResult JSBigInt::decideRounding(JSBigInt* bigInt, int32_t mantissaBitsUnset, int32_t digitIndex, uint64_t currentDigit)
{
    if (mantissaBitsUnset > 0)
        return RoundingResult::RoundDown;
    int32_t topUnconsumedBit = 0;
    if (mantissaBitsUnset < 0) {
        // There are unconsumed bits in currentDigit.
        topUnconsumedBit = -mantissaBitsUnset - 1;
    } else {
        ASSERT(mantissaBitsUnset == 0);
        // currentDigit fit the mantissa exactly; look at the next digit.
        if (digitIndex == 0)
            return RoundingResult::RoundDown;
        digitIndex--;
        currentDigit = static_cast<uint64_t>(bigInt->digit(digitIndex));
        topUnconsumedBit = digitBits - 1;
    }
    // If the most significant remaining bit is 0, round down.
    uint64_t bitmask = static_cast<uint64_t>(1) << topUnconsumedBit;
    if ((currentDigit & bitmask) == 0)
        return RoundingResult::RoundDown;
    // If any other remaining bit is set, round up.
    bitmask -= 1;
    if ((currentDigit & bitmask) != 0)
        return RoundingResult::RoundUp;
    while (digitIndex > 0) {
        digitIndex--;
        if (bigInt->digit(digitIndex) != 0)
            return RoundingResult::RoundUp;
    }
    return RoundingResult::Tie;
}

JSValue JSBigInt::toNumberHeap(JSBigInt* bigInt)
{
    if (bigInt->isZero())
        return jsNumber(0);
    ASSERT(bigInt->length());

    // Conversion mechanism is the following.
    //
    // 1. Get exponent bits.
    // 2. Collect mantissa 52 bits.
    // 3. Add rounding result of unused bits to mantissa and adjust mantissa & exponent bits.
    // 4. Generate double by combining (1) and (3).

    const unsigned length = bigInt->length();
    const bool sign = bigInt->sign();
    const Digit msd = bigInt->digit(length - 1);
    const unsigned msdLeadingZeros = clz(msd);
    const size_t bitLength = length * digitBits - msdLeadingZeros;
    // Double's exponent bits overflow.
    if (bitLength > 1024)
        return jsDoubleNumber(sign ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
    uint64_t exponent = bitLength - 1;
    uint64_t currentDigit = msd;
    int32_t digitIndex = length - 1;
    int32_t shiftAmount = msdLeadingZeros + 1 + (64 - digitBits);
    ASSERT(1 <= shiftAmount);
    ASSERT(shiftAmount <= 64);
    uint64_t mantissa = (shiftAmount == 64) ? 0 : currentDigit << shiftAmount;

    // unsetBits = 64 - setBits - 12 // 12 for non-mantissa bits
    //     setBits = 64 - (msdLeadingZeros + 1 + bitsNotAvailableDueToDigitSize);  // 1 for hidden mantissa bit.
    //                 = 64 - (msdLeadingZeros + 1 + (64 - digitBits))
    //                 = 64 - shiftAmount
    // Hence, unsetBits = 64 - (64 - shiftAmount) - 12 = shiftAmount - 12

    mantissa >>= 12; // (12 = 64 - 52), we shift 12 bits to put 12 zeros in uint64_t mantissa.
    int32_t mantissaBitsUnset = shiftAmount - 12;

    // If not all mantissa bits are defined yet, get more digits as needed.
    // Collect mantissa 52bits from several digits.

    if constexpr (digitBits < 64) {
        if (mantissaBitsUnset >= static_cast<int32_t>(digitBits) && digitIndex > 0) {
            digitIndex--;
            currentDigit = static_cast<uint64_t>(bigInt->digit(digitIndex));
            mantissa |= (currentDigit << (mantissaBitsUnset - digitBits));
            mantissaBitsUnset -= digitBits;
        }
    }

    if (mantissaBitsUnset > 0 && digitIndex > 0) {
        ASSERT(mantissaBitsUnset < static_cast<int32_t>(digitBits));
        digitIndex--;
        currentDigit = static_cast<uint64_t>(bigInt->digit(digitIndex));
        mantissa |= (currentDigit >> (digitBits - mantissaBitsUnset));
        mantissaBitsUnset -= digitBits;
    }

    // If there are unconsumed digits left, we may have to round.
    RoundingResult rounding = decideRounding(bigInt, mantissaBitsUnset, digitIndex, currentDigit);
    if (rounding == RoundingResult::RoundUp || (rounding == RoundingResult::Tie && (mantissa & 1) == 1)) {
        ++mantissa;
        // Incrementing the mantissa can overflow the mantissa bits. In that case the new mantissa will be all zero (plus hidden bit).
        if ((mantissa >> doublePhysicalMantissaSize) != 0) {
            mantissa = 0;
            exponent++;
            // Incrementing the exponent can overflow too.
            if (exponent > 1023)
                return jsDoubleNumber(sign ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
        }
    }

    uint64_t signBit = sign ? (static_cast<uint64_t>(1) << 63) : 0;
    exponent = (exponent + 0x3ff) << doublePhysicalMantissaSize; // 0x3ff is double exponent bias.
    uint64_t doubleBits = signBit | exponent | mantissa;
    ASSERT((doubleBits & (static_cast<uint64_t>(1) << 63)) == signBit);
    ASSERT((doubleBits & (static_cast<uint64_t>(0x7ff) << 52)) == exponent);
    ASSERT((doubleBits & ((static_cast<uint64_t>(1) << 52) - 1)) == mantissa);
    return jsNumber(std::bit_cast<double>(doubleBits));
}

template <typename BigIntImpl>
JSBigInt::ImplResult JSBigInt::asIntNImpl(JSGlobalObject* globalObject, uint64_t n, BigIntImpl bigInt)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (bigInt.isZero())
        return { bigInt };
    if (n == 0)
        RELEASE_AND_RETURN(scope, zeroImpl(vm));

    uint64_t neededLength = (n + digitBits - 1) / digitBits;
    uint64_t length = static_cast<uint64_t>(bigInt.length());
    // If bigInt has less than n bits, return it directly.
    if (length < neededLength)
        return { bigInt };
    ASSERT(neededLength <= INT32_MAX);
    Digit topDigit = bigInt.digit(static_cast<int32_t>(neededLength) - 1);
    Digit compareDigit = static_cast<Digit>(1) << ((n - 1) % digitBits);
    if (length == neededLength && topDigit < compareDigit)
        return { bigInt };

    // Otherwise we have to truncate (which is a no-op in the special case
    // of bigInt == -2^(n-1)), and determine the right sign. We also might have
    // to subtract from 2^n to simulate having two's complement representation.
    // In most cases, the result's sign is bigInt.sign() xor "(n-1)th bit present".
    // The only exception is when bigInt is negative, has the (n-1)th bit, and all
    // its bits below (n-1) are zero. In that case, the result is the minimum
    // n-bit integer (example: asIntN(3, -12n) => -4n).
    bool hasBit = (topDigit & compareDigit) == compareDigit;
    ASSERT(n <= INT32_MAX);
    int32_t N = static_cast<int32_t>(n);
    if (!hasBit)
        RELEASE_AND_RETURN(scope, truncateToNBits(globalObject, N, bigInt));
    if (!bigInt.sign())
        RELEASE_AND_RETURN(scope, truncateAndSubFromPowerOfTwo(globalObject, N, bigInt, true));

    // Negative numbers must subtract from 2^n, except for the special case
    // described above.
    if ((topDigit & (compareDigit - 1)) == 0) {
        for (int32_t i = static_cast<int32_t>(neededLength) - 2; i >= 0; i--) {
            if (bigInt.digit(i) != 0)
                RELEASE_AND_RETURN(scope, truncateAndSubFromPowerOfTwo(globalObject, N, bigInt, false));
        }
        // Truncation is no-op if bigInt == -2^(n-1).
        if (length == neededLength && topDigit == compareDigit)
            return { bigInt };
        RELEASE_AND_RETURN(scope, truncateToNBits(globalObject, N, bigInt));
    }
    RELEASE_AND_RETURN(scope, truncateAndSubFromPowerOfTwo(globalObject, N, bigInt, false));
}

template <typename BigIntImpl>
JSBigInt::ImplResult JSBigInt::asUintNImpl(JSGlobalObject* globalObject, uint64_t n, BigIntImpl bigInt)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (bigInt.isZero())
        return { bigInt };
    if (n == 0)
        RELEASE_AND_RETURN(scope, zeroImpl(vm));

    // If bigInt is negative, simulate two's complement representation.
    if (bigInt.sign()) {
        if (n > maxLengthBits) {
            throwOutOfMemoryError(globalObject, scope, "BigInt generated from this operation is too big"_s);
            return nullptr;
        }
        RELEASE_AND_RETURN(scope, truncateAndSubFromPowerOfTwo(globalObject, static_cast<int32_t>(n), bigInt, false));
    }

    // If bigInt is positive and has up to n bits, return it directly.
    if (n >= maxLengthBits)
        return { bigInt };
    static_assert(maxLengthBits < INT32_MAX - digitBits);
    int32_t neededLength = static_cast<int32_t>((n + digitBits - 1) / digitBits);
    if (static_cast<int32_t>(bigInt.length()) < neededLength)
        return { bigInt };

    int32_t bitsInTopDigit = n % digitBits;
    if (static_cast<int32_t>(bigInt.length()) == neededLength) {
        if (bitsInTopDigit == 0)
            return { bigInt };
        Digit topDigit = bigInt.digit(neededLength - 1);
        if ((topDigit >> bitsInTopDigit) == 0)
            return { bigInt };
    }

    // Otherwise, truncate.
    ASSERT(n <= INT32_MAX);
    RELEASE_AND_RETURN(scope, truncateToNBits(globalObject, static_cast<int32_t>(n), bigInt));
}

template <typename BigIntImpl>
JSBigInt::ImplResult JSBigInt::truncateToNBits(JSGlobalObject* globalObject, int32_t n, BigIntImpl bigInt)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto span = bigInt.digits();

    ASSERT(n != 0);
    ASSERT(span.size() > n / digitBits);

    int32_t neededDigits = (n + (digitBits - 1)) / digitBits;
    ASSERT(neededDigits <= static_cast<int32_t>(span.size()));

    Vector<Digit, 16> resultVector(neededDigits);
    auto result = resultVector.mutableSpan();

    // Copy all digits except the MSD.
    int32_t last = neededDigits - 1;
    for (int32_t i = 0; i < last; i++)
        result[i] = span[i];

    // The MSD might contain extra bits that we don't want.
    Digit msd = span[last];
    if (n % digitBits != 0) {
        int32_t drop = digitBits - (n % digitBits);
        msd = (msd << drop) >> drop;
    }
    result[last] = msd;
    RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, bigInt.sign(), result));
}

// Subtracts the least significant n bits of abs(bigInt) from 2^n.
template <typename BigIntImpl>
JSBigInt::ImplResult JSBigInt::truncateAndSubFromPowerOfTwo(JSGlobalObject* globalObject, int32_t n, BigIntImpl bigInt, bool resultSign)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(n != 0);
    ASSERT(n <= static_cast<int32_t>(maxLengthBits));

    auto span = bigInt.digits();

    int32_t neededDigits = (n + (digitBits - 1)) / digitBits;
    ASSERT(neededDigits <= static_cast<int32_t>(maxLength)); // Follows from n <= maxLengthBits.

    Vector<Digit, 16> resultVector(neededDigits);
    auto result = resultVector.mutableSpan();

    // Process all digits except the MSD.
    int32_t i = 0;
    int32_t last = neededDigits - 1;
    int32_t length = span.size();
    Digit borrow = 0;
    // Take digits from bigInt unless its length is exhausted.
    int32_t limit = std::min(last, length);
    for (; i < limit; i++) {
        Digit newBorrow = 0;
        Digit difference = digitSub(0, span[i], newBorrow);
        difference = digitSub(difference, borrow, newBorrow);
        result[i] = difference;
        borrow = newBorrow;
    }
    // Then simulate leading zeroes in {bigInt} as needed.
    for (; i < last; i++) {
        Digit newBorrow = 0;
        Digit difference = digitSub(0, borrow, newBorrow);
        result[i] = difference;
        borrow = newBorrow;
    }

    // The MSD might contain extra bits that we don't want.
    Digit msd = last < length ? span[last] : 0;
    int32_t msdBitsConsumed = n % digitBits;
    Digit resultMSD;
    if (msdBitsConsumed == 0) {
        Digit newBorrow = 0;
        resultMSD = digitSub(0, msd, newBorrow);
        resultMSD = digitSub(resultMSD, borrow, newBorrow);
    } else {
        int32_t drop = digitBits - msdBitsConsumed;
        msd = (msd << drop) >> drop;
        Digit minuendMSD = static_cast<Digit>(1) << (digitBits - drop);
        Digit newBorrow = 0;
        resultMSD = digitSub(minuendMSD, msd, newBorrow);
        resultMSD = digitSub(resultMSD, borrow, newBorrow);
        ASSERT(newBorrow == 0); // result < 2^n.
        // If all subtracted bits were zero, we have to get rid of the
        // materialized minuendMSD again.
        resultMSD &= (minuendMSD - 1);
    }
    result[last] = resultMSD;
    RELEASE_AND_RETURN(scope, tryCreateFromImpl(globalObject, vm, resultSign, result));
}

JSValue JSBigInt::asIntN(JSGlobalObject* globalObject, uint64_t n, JSBigInt* bigInt)
{
    return tryConvertToBigInt32(asIntNImpl(globalObject, n, HeapBigIntImpl { bigInt }));
}

JSValue JSBigInt::asUintN(JSGlobalObject* globalObject, uint64_t n, JSBigInt* bigInt)
{
    return tryConvertToBigInt32(asUintNImpl(globalObject, n, HeapBigIntImpl { bigInt }));
}

#if USE(BIGINT32)
JSValue JSBigInt::asIntN(JSGlobalObject* globalObject, uint64_t n, int32_t bigInt)
{
    return tryConvertToBigInt32(asIntNImpl(globalObject, n, Int32BigIntImpl { bigInt }));
}

JSValue JSBigInt::asUintN(JSGlobalObject* globalObject, uint64_t n, int32_t bigInt)
{
    return tryConvertToBigInt32(asUintNImpl(globalObject, n, Int32BigIntImpl { bigInt }));
}
#endif

uint64_t JSBigInt::toBigUInt64Heap(JSBigInt* bigInt)
{
    auto length = bigInt->length();
    if (!length)
        return 0;
    uint64_t value = 0;
    if constexpr (sizeof(Digit) == 4) {
        value = static_cast<uint64_t>(bigInt->digit(0));
        if (length > 1)
            value |= static_cast<uint64_t>(bigInt->digit(1)) << 32;
    } else {
        ASSERT(sizeof(Digit) == 8);
        value = bigInt->digit(0);
    }
    if (!bigInt->sign())
        return value;
    return ~(value - 1); // To avoid undefined behavior, we compute two's compliment by hand in C while this is simply `-value`.
}

static ALWAYS_INLINE unsigned NODELETE computeHash(JSBigInt::Digit* digits, unsigned length, bool sign)
{
    Hasher hasher;
    WTF::add(hasher, sign);
    for (unsigned index = 0; index < length; ++index)
        WTF::add(hasher, digits[index]);
    return hasher.hash();
}

std::optional<unsigned> JSBigInt::concurrentHash()
{
    // FIXME: Implement JSBigInt::concurrentHash by inserting right store barriers.
    // https://bugs.webkit.org/show_bug.cgi?id=216801
    return std::nullopt;
}

unsigned JSBigInt::hashSlow()
{
    ASSERT(!m_hash);
    m_hash = computeHash(dataStorage(), length(), sign());
    return m_hash;
}

JSBigInt* JSBigInt::tryCreateFromImpl(JSGlobalObject* nullOrGlobalObjectForOOM, VM& vm, bool sign, std::span<const Digit> digits)
{
    digits = normalize(digits);
    if (digits.empty())
        return createZero(vm);

    JSBigInt* result = createWithLength(nullOrGlobalObjectForOOM, vm, digits.size());
    if (!result) [[unlikely]]
        return nullptr;
    memcpySpan(result->digits(), digits);
    result->setSign(sign);
    return result;
}

JSBigInt* JSBigInt::tryCreateFrom(JSGlobalObject* nullOrGlobalObjectForOOM, VM& vm, bool sign, std::span<const Digit> digits)
{
    return tryCreateFromImpl(nullOrGlobalObjectForOOM, vm, sign, digits);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
