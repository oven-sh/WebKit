/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBigIntPlace.h"
#include "AOTObjectWords.h"
#include "AllocatorInlines.h"
#include "CompleteSubspaceInlines.h"
#include "FreeListInlines.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include <array>
#include <atomic>
#include <bit>
#include <utility>
#include <wtf/Int128.h>
#if CPU(X86_64) && !OS(WINDOWS)
#include <cpuid.h>
#define AOT_HAS_FUNCTIONS_WITH_MULX 1
#else
#define AOT_HAS_FUNCTIONS_WITH_MULX 0
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class BigIntArithmetic {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    using Digit = JSBigInt::Digit;

    static_assert(bigIntSlotDigitsOffset == JSBigInt::offsetOfData());
    static ALWAYS_INLINE bool isInFrame(JSBigInt* bigInt) { return !*std::bit_cast<uint32_t*>(std::bit_cast<char*>(bigInt) + JSCell::structureIDOffset()); }

    class HeapPlace {
        WTF_FORBID_HEAP_ALLOCATION;
    public:
        explicit HeapPlace(VM& vm)
            : m_vm(vm)
        {
        }

        ALWAYS_INLINE JSBigInt* zero() const { return m_vm.heapBigIntConstantZero.get(); }

        ALWAYS_INLINE JSBigInt* tryMake(unsigned length, bool sign) const
        {
            Allocator allocator = m_vm.cellSpace().allocatorFor(WTF::roundUpToMultipleOf<JSBigInt::atomSize>(JSBigInt::allocationSize(length)), AllocatorForMode::AllocatorIfExists);
            if (!allocator) [[unlikely]]
                return nullptr;
            auto* freeList = std::bit_cast<FreeList*>(std::bit_cast<char*>(allocator.localAllocator()) + LocalAllocator::offsetOfFreeList());
            auto needsCollector = [] -> HeapCell* {
                return nullptr;
            };
            void* cell = freeList->allocateWithCellSize(needsCollector, allocator.cellSize());
            if (!cell) [[unlikely]]
                return nullptr;
            auto* result = static_cast<JSBigInt*>(cell);
            *std::bit_cast<uint64_t*>(cell) = cellHeaderFor(m_vm.bigIntStructure.get()) | (sign ? signInHeader : 0);
            setLengthAndHash(result, length);
            return result;
        }

        ALWAYS_INLINE JSBigInt* tryCopy(JSBigInt* operand) const { return isInFrame(operand) ? tryCopyDigits(*this, operand, operand->sign()) : operand; }

        ALWAYS_INLINE JSBigInt* finish(JSBigInt* result) const
        {
            result->finishCreation(m_vm);
            return result;
        }

        ALWAYS_INLINE JSBigInt* finishTrimmed(JSBigInt* result, unsigned length) const
        {
            length = trimmedLength(result, length);
            result->setLength(length);
            result->finishCreation(m_vm);
            return length ? result : zero();
        }

    private:
        VM& m_vm;
    };

    template<unsigned lengthAtMost>
    class FramePlace {
        WTF_FORBID_HEAP_ALLOCATION;
    public:
        explicit FramePlace(void* place)
            : m_place(static_cast<JSBigInt*>(place))
        {
        }

        ALWAYS_INLINE JSBigInt* zero() const { return tryMake(0, false); }

        ALWAYS_INLINE JSBigInt* tryMake(unsigned length, bool sign) const
        {
            if (length > lengthAtMost) [[unlikely]]
                return nullptr;
            constexpr uint64_t header = static_cast<uint64_t>(HeapBigIntType) << (JSCell::typeInfoTypeOffset() * 8);
            *std::bit_cast<uint64_t*>(m_place) = header | (sign ? signInHeader : 0);
            setLengthAndHash(m_place, length);
            return m_place;
        }

        ALWAYS_INLINE JSBigInt* tryCopy(JSBigInt* operand) const { return tryCopyDigits(*this, operand, operand->sign()); }
        ALWAYS_INLINE JSBigInt* finish(JSBigInt* result) const { return result; }

        ALWAYS_INLINE JSBigInt* finishTrimmed(JSBigInt* result, unsigned length) const
        {
            length = trimmedLength(result, length);
            if (!length)
                return zero();
            result->setLength(length);
            return result;
        }

    private:
        JSBigInt* m_place;
    };

    static ALWAYS_INLINE JSValue valueInHeap(VM& vm, JSValue value)
    {
        if (value.isHeapBigInt() && isInFrame(value.asHeapBigInt())) [[unlikely]]
            return copyToHeap(vm, value.asHeapBigInt());
        return value;
    }

    static NEVER_INLINE JSBigInt* copyToHeap(VM& vm, JSBigInt* bigInt)
    {
        if (JSBigInt* result = tryCopyDigits(HeapPlace(vm), bigInt, bigInt->sign()))
            return result;
        JSBigInt* result = JSBigInt::tryCreateWithLength(vm, bigInt->length());
        RELEASE_ASSERT(result);
        for (unsigned index = 0; index < bigInt->length(); ++index)
            result->setDigit(index, bigInt->digit(index));
        result->setSign(bigInt->sign());
        return result;
    }

#if AOT_HAS_FUNCTIONS_WITH_MULX
#define AOT_WITH_MULX __attribute__((target("bmi2,lzcnt")))
    static ALWAYS_INLINE bool hasMULX()
    {
        uint8_t state = s_stateOfMULX.load(std::memory_order_relaxed);
        if (!state) [[unlikely]]
            return findMULX();
        return state == mulxIsPresent;
    }
#endif

    static constexpr unsigned usualLength = 4;

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryAddOfUsualLength(Place place, JSBigInt* left, JSBigInt* right)
    {
        return trySumOfUsualLength(place, left, left->sign(), right, right->sign());
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* trySubOfUsualLength(Place place, JSBigInt* left, JSBigInt* right)
    {
        return trySumOfUsualLength(place, left, left->sign(), right, !right->sign());
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryMultiplyOfUsualLength(Place place, JSBigInt* left, JSBigInt* right)
    {
        JSBigInt* result = place.tryMake(2 * usualLength, left->sign() != right->sign());
        if (!result) [[unlikely]]
            return nullptr;
        const Digit* x = left->dataStorage();
        const Digit* y = right->dataStorage();
        Digit* digits = result->dataStorage();
        if (x == y)
            squareDigits<usualLength>(x, digits);
        else
            multiplyDigits<usualLength>(x, y, digits);
        if (!digits[2 * usualLength - 1]) [[unlikely]]
            result->m_length = 2 * usualLength - 1;
        return place.finish(result);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryMultiplyByDigit(Place place, JSBigInt* left, JSBigInt* right)
    {
        JSBigInt* longer = right->length() == 1 ? left : right;
        JSBigInt* digit = right->length() == 1 ? right : left;
        unsigned length = longer->length();
        Digit factor = digit->digit(0);
        JSBigInt* result = place.tryMake(length + 1, left->sign() != right->sign());
        if (!result) [[unlikely]]
            return nullptr;
        Digit* digits = result->dataStorage();
        Digit carry = multiplyByDigit(longer->dataStorage(), length, factor, digits);
        if (carry)
            digits[length] = carry;
        else
            result->setLength(length);
        return place.finish(result);
    }

    template<unsigned dividendLength, typename Place>
    static ALWAYS_INLINE JSBigInt* tryRemainderOfUsualLength(Place place, JSBigInt* dividend, JSBigInt* divisor, bool mayBeSum)
    {
        return tryFold<dividendLength>(place, dividend->dataStorage(), dividend->sign(), divisor, mayBeSum);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryRemainderOfDigitProduct(Place place, JSBigInt* left, JSBigInt* right, JSBigInt* divisor)
    {
        UInt128 product = static_cast<UInt128>(left->digit(0)) * right->digit(0);
        Digit low = static_cast<Digit>(product);
        bool isLong = static_cast<Digit>(product >> JSBigInt::digitBits);
        Digit remainder = isLong ? static_cast<Digit>(product % divisor->digit(0)) : low % divisor->digit(0);
        if (!remainder)
            return place.zero();
        JSBigInt* result = place.tryMake(1, left->sign() != right->sign());
        if (!result) [[unlikely]]
            return nullptr;
        result->setDigit(0, remainder);
        return place.finish(result);
    }

    enum class Factors : bool { HaveUsualLength, MayBeShorter };
    template<Factors factors, typename Place>
    static ALWAYS_INLINE JSBigInt* tryRemainderOfProduct(Place place, JSBigInt* left, JSBigInt* right, JSBigInt* divisor, bool mayBeSum)
    {
        if (!isNearPowerOfTwo(divisor->dataStorage())) [[unlikely]]
            return nullptr;
        const Digit* x = left->dataStorage();
        const Digit* y = right->dataStorage();
        std::array<Digit, 2 * usualLength> product;
        if constexpr (factors == Factors::MayBeShorter) {
            unsigned leftLength = left->length();
            unsigned rightLength = right->length();
            if (!leftLength || !rightLength)
                return place.zero();
            product.fill(0);
            if (leftLength < rightLength)
                multiplyDigitsOfAnyLength(y, rightLength, x, leftLength, product.data());
            else
                multiplyDigitsOfAnyLength(x, leftLength, y, rightLength, product.data());
        } else if (x == y)
            squareDigits<usualLength>(x, product.data());
        else
            multiplyDigits<usualLength>(x, y, product.data());
        return tryFold<2 * usualLength>(place, product.data(), left->sign() != right->sign(), divisor, mayBeSum);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryAdd(Place place, JSBigInt* left, JSBigInt* right)
    {
        return trySum(place, left, left->sign(), right, right->sign());
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* trySub(Place place, JSBigInt* left, JSBigInt* right)
    {
        return trySum(place, left, left->sign(), right, !right->sign());
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryMultiply(Place place, JSBigInt* left, JSBigInt* right)
    {
        unsigned leftLength = left->length();
        unsigned rightLength = right->length();
        if (leftLength < rightLength) {
            std::swap(left, right);
            std::swap(leftLength, rightLength);
        }
        if (leftLength > maxFactorLength) [[unlikely]]
            return nullptr;
        if (!rightLength)
            return place.zero();
        bool sign = left->sign() != right->sign();
        if (rightLength == 1 && right->digit(0) == 1)
            return sign == left->sign() ? place.tryCopy(left) : tryCopyDigits(place, left, sign);
        JSBigInt* result = place.tryMake(leftLength + rightLength, sign);
        if (!result) [[unlikely]]
            return nullptr;
        const Digit* x = left->dataStorage();
        const Digit* y = right->dataStorage();
        Digit* digits = result->dataStorage();
        multiplyDigitsOfAnyLength(x, leftLength, y, rightLength, digits);
        result->setLength(leftLength + rightLength - !digits[leftLength + rightLength - 1]);
        return place.finish(result);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryRemainder(VM& vm, Place place, JSBigInt* dividend, JSBigInt* divisor)
    {
        unsigned length = divisor->length();
        unsigned dividendLength = dividend->length();
        if (length == usualLength && isNearPowerOfTwo(divisor->dataStorage())) {
            if (dividendLength < length || (dividendLength == length && dividend->digit(length - 1) < divisor->digit(length - 1)))
                return place.tryCopy(dividend);
            if (dividendLength > maxTermLength) [[unlikely]]
                return nullptr;
            JSBigInt* result = place.tryMake(length, dividend->sign());
            if (!result) [[unlikely]]
                return nullptr;
            foldDigitsOfAnyLength(dividend->dataStorage(), dividendLength, divisor->dataStorage(), result->dataStorage());
            return place.finishTrimmed(result, length);
        }
        if (vm.m_cachedBigIntDivisor.get() != divisor) {
            if (length == 1)
                return tryRemainderByDigit(place, dividend, divisor->digit(0));
            return dividendLength < length ? place.tryCopy(dividend) : tryRemainderOfSameLength(vm, place, dividend, divisor);
        }
        if (dividendLength <= length) {
            if (dividendLength < length || dividend->digit(length - 1) < divisor->digit(length - 1))
                return place.tryCopy(dividend);
        } else if (dividendLength > 2 * length) [[unlikely]]
            return nullptr;
        JSBigInt* result = place.tryMake(length, dividend->sign());
        if (!result) [[unlikely]]
            return nullptr;
        JSBigInt::cachedMod(vm, digitsOf(result), digitsOf(dividend), digitsOf(divisor));
        return place.finishTrimmed(result, length);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryRemainderByQuotient(Place place, JSBigInt* dividend, JSBigInt* divisor, JSBigInt* quotient)
    {
        unsigned length = divisor->length();
        unsigned dividendLength = dividend->length();
        if (!quotient->length())
            return place.tryCopy(dividend);
        if (quotient->length() > 1 || dividendLength < length || dividendLength > length + 1 || !length)
            return nullptr;
        JSBigInt* result = place.tryMake(length, dividend->sign());
        if (!result) [[unlikely]]
            return nullptr;
        Digit factor = quotient->digit(0);
        const Digit* x = dividend->dataStorage();
        const Digit* y = divisor->dataStorage();
        Digit* digits = result->dataStorage();
        Digit carry = 0;
        Digit borrow = 0;
        for (unsigned index = 0; index < length; ++index) {
            UInt128 product = static_cast<UInt128>(y[index]) * factor + carry;
            carry = static_cast<Digit>(product >> JSBigInt::digitBits);
            digits[index] = subtractWithBorrow(x[index], static_cast<Digit>(product), borrow);
        }
        return place.finishTrimmed(result, length);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryDivide(Place place, JSBigInt* dividend, JSBigInt* divisor)
    {
        unsigned length = divisor->length();
        unsigned dividendLength = dividend->length();
        if (dividendLength < length)
            return place.zero();
        if (dividendLength > length + 1 || !length)
            return nullptr;
        bool sign = dividend->sign() != divisor->sign();
        Digit quotient;
        if (dividendLength > length) {
            quotient = length > 1 ? quotientOfOneDigitMore(dividend->dataStorage(), divisor->dataStorage(), length) : 0;
            if (!quotient)
                return nullptr;
        } else {
            Digit high = dividend->digit(length - 1);
            Digit highOfDivisor = divisor->digit(length - 1);
            if (high < highOfDivisor)
                return place.zero();
            if (high == highOfDivisor && length > 1)
                return nullptr;
            quotient = length == 1 ? high / highOfDivisor : JSBigInt::divideSameSize(digitsOf(dividend), digitsOf(divisor));
        }
        JSBigInt* result = place.tryMake(1, sign);
        if (!result) [[unlikely]]
            return nullptr;
        result->setDigit(0, quotient);
        return place.finish(result);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryNegate(Place place, JSBigInt* operand)
    {
        unsigned length = operand->length();
        return length ? tryCopyDigits(place, operand, !operand->sign()) : place.zero();
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryIncrement(Place place, JSBigInt* operand) { return tryStep<false>(place, operand); }
    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryDecrement(Place place, JSBigInt* operand) { return tryStep<true>(place, operand); }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryBitwiseAnd(Place place, JSBigInt* left, JSBigInt* right)
    {
        if (left->sign() || right->sign())
            return nullptr;
        unsigned length = std::min(left->length(), right->length());
        while (length && !(left->digit(length - 1) & right->digit(length - 1)))
            --length;
        if (!length)
            return place.zero();
        if (length > maxTermLength) [[unlikely]]
            return nullptr;
        JSBigInt* result = place.tryMake(length, false);
        if (!result) [[unlikely]]
            return nullptr;
        for (unsigned index = 0; index < length; ++index)
            result->setDigit(index, left->digit(index) & right->digit(index));
        return place.finish(result);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* trySignedRightShift(Place place, JSBigInt* operand, JSBigInt* amount)
    {
        if (operand->sign() || amount->sign())
            return nullptr;
        unsigned amountLength = amount->length();
        unsigned length = operand->length();
        if (!amountLength || !length)
            return place.tryCopy(operand);
        if (amountLength > 1 || length > maxTermLength || amount->digit(0) >= length * JSBigInt::digitBits)
            return nullptr;
        unsigned digitShift = amount->digit(0) / JSBigInt::digitBits;
        unsigned bitShift = amount->digit(0) % JSBigInt::digitBits;
        const Digit* digits = operand->dataStorage() + digitShift;
        unsigned resultLength = length - digitShift;
        bool losesHighDigit = !(digits[resultLength - 1] >> bitShift);
        if (resultLength == losesHighDigit)
            return place.zero();
        JSBigInt* result = place.tryMake(resultLength - losesHighDigit, false);
        if (!result) [[unlikely]]
            return nullptr;
        Digit* resultDigits = result->dataStorage();
        for (unsigned index = 0; index < resultLength - losesHighDigit; ++index) {
            Digit above = index + 1 < resultLength ? digits[index + 1] : 0;
            resultDigits[index] = static_cast<Digit>((static_cast<UInt128>(above) << JSBigInt::digitBits | digits[index]) >> bitShift);
        }
        return place.finish(result);
    }

private:
    static constexpr unsigned maxFactorLength = 16;
    static constexpr unsigned maxTermLength = 32;

    static ALWAYS_INLINE std::span<Digit> digitsOf(JSBigInt* bigInt) { return { bigInt->dataStorage(), bigInt->length() }; }

    static constexpr uint64_t signInHeader = static_cast<uint64_t>(TypeInfoPerCellBit) << (JSCell::typeInfoFlagsOffset() * 8);

    static ALWAYS_INLINE void setLengthAndHash(JSBigInt* result, unsigned length)
    {
        static_assert(OBJECT_OFFSETOF(JSBigInt, m_hash) == OBJECT_OFFSETOF(JSBigInt, m_length) + sizeof(unsigned) && std::endian::native == std::endian::little);
        *std::bit_cast<uint64_t*>(&result->m_length) = length;
    }

    static ALWAYS_INLINE unsigned trimmedLength(JSBigInt* result, unsigned length)
    {
        const Digit* digits = result->dataStorage();
        while (length && !digits[length - 1])
            --length;
        return length;
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryCopyDigits(Place place, JSBigInt* operand, bool sign)
    {
        unsigned length = operand->length();
        if (length > maxTermLength) [[unlikely]]
            return nullptr;
        JSBigInt* result = place.tryMake(length, sign);
        if (!result) [[unlikely]]
            return nullptr;
        const Digit* digits = operand->dataStorage();
        Digit* resultDigits = result->dataStorage();
        for (unsigned index = 0; index < length; ++index)
            resultDigits[index] = digits[index];
        return place.finish(result);
    }

    static ALWAYS_INLINE Digit addWithCarry(Digit x, Digit y, Digit& carry)
    {
        unsigned long long carryOut;
        Digit sum = __builtin_addcll(x, y, carry, &carryOut);
        carry = carryOut;
        return sum;
    }

    static ALWAYS_INLINE Digit subtractWithBorrow(Digit x, Digit y, Digit& borrow)
    {
        unsigned long long borrowOut;
        Digit difference = __builtin_subcll(x, y, borrow, &borrowOut);
        borrow = borrowOut;
        return difference;
    }

    template<unsigned length>
    static ALWAYS_INLINE Digit addDigits(const Digit* x, const Digit* y, Digit* result)
    {
        Digit carry = 0;
        _Pragma("unroll") for (unsigned index = 0; index < length; ++index)
            result[index] = addWithCarry(x[index], y[index], carry);
        return carry;
    }

    template<unsigned length>
    static ALWAYS_INLINE Digit subtractDigits(const Digit* x, const Digit* y, Digit* result)
    {
        Digit borrow = 0;
        _Pragma("unroll") for (unsigned index = 0; index < length; ++index)
            result[index] = subtractWithBorrow(x[index], y[index], borrow);
        return borrow;
    }

#if AOT_HAS_FUNCTIONS_WITH_MULX
    static constexpr uint8_t mulxIsAbsent = 1;
    static constexpr uint8_t mulxIsPresent = 2;
    static NEVER_INLINE bool findMULX()
    {
        constexpr unsigned leafOfExtendedFeatures = 7;
        constexpr unsigned leafOfLZCNT = 0x80000001;
        constexpr unsigned bitOfBMI2 = 1 << 8;
        constexpr unsigned bitOfLZCNT = 1 << 5;
        unsigned eax, ebx, ecx, edx;
        bool isPresent = __get_cpuid_count(leafOfExtendedFeatures, 0, &eax, &ebx, &ecx, &edx) && ebx & bitOfBMI2;
        isPresent = isPresent && __get_cpuid(leafOfLZCNT, &eax, &ebx, &ecx, &edx) && ecx & bitOfLZCNT;
        s_stateOfMULX.store(isPresent ? mulxIsPresent : mulxIsAbsent, std::memory_order_relaxed);
        return isPresent;
    }
    static inline std::atomic<uint8_t> s_stateOfMULX;
#endif

    template<unsigned length>
    static ALWAYS_INLINE void endRow(Digit (&sum)[length])
    {
        static_assert(length == usualLength);
        asm volatile("" : "+r"(sum[0]), "+r"(sum[1]), "+r"(sum[2]), "+r"(sum[3]) : : "memory");
    }

    template<unsigned length>
    static ALWAYS_INLINE void multiplyDigits(const Digit* x, const Digit* y, Digit* result)
    {
        Digit sum[length];
        _Pragma("unroll") for (unsigned row = 0; row < length; ++row) {
            Digit low[length];
            Digit high[length];
            _Pragma("unroll") for (unsigned index = 0; index < length; ++index) {
                UInt128 product = static_cast<UInt128>(x[index]) * y[row];
                low[index] = static_cast<Digit>(product);
                high[index] = static_cast<Digit>(product >> JSBigInt::digitBits);
            }
            Digit carry = 0;
            _Pragma("unroll") for (unsigned index = 1; index < length; ++index)
                low[index] = addWithCarry(low[index], high[index - 1], carry);
            Digit top = high[length - 1] + carry;
            carry = 0;
            if (row) {
                _Pragma("unroll") for (unsigned index = 0; index < length; ++index)
                    low[index] = addWithCarry(sum[index], low[index], carry);
            }
            result[row] = low[0];
            _Pragma("unroll") for (unsigned index = 1; index < length; ++index)
                sum[index - 1] = low[index];
            sum[length - 1] = top + carry;
            if (row + 1 < length)
                endRow(sum);
        }
        _Pragma("unroll") for (unsigned index = 0; index < length; ++index)
            result[length + index] = sum[index];
    }

    template<unsigned length>
    static ALWAYS_INLINE void squareDigits(const Digit* x, Digit* result)
    {
        Digit mixed[2 * length] = { };
        _Pragma("unroll") for (unsigned row = 0; row + 1 < length; ++row) {
            Digit low[length];
            Digit high[length];
            _Pragma("unroll") for (unsigned index = row + 1; index < length; ++index) {
                UInt128 product = static_cast<UInt128>(x[index]) * x[row];
                low[index] = static_cast<Digit>(product);
                high[index] = static_cast<Digit>(product >> JSBigInt::digitBits);
            }
            Digit carry = 0;
            _Pragma("unroll") for (unsigned index = row + 2; index < length; ++index)
                low[index] = addWithCarry(low[index], high[index - 1], carry);
            Digit top = high[length - 1] + carry;
            carry = 0;
            _Pragma("unroll") for (unsigned index = row + 1; index < length; ++index)
                mixed[row + index] = row ? addWithCarry(mixed[row + index], low[index], carry) : low[index];
            mixed[row + length] = top + carry;
        }
        Digit carry = 0;
        _Pragma("unroll") for (unsigned index = 1; index + 1 < 2 * length; ++index)
            mixed[index] = addWithCarry(mixed[index], mixed[index], carry);
        mixed[2 * length - 1] = carry;
        carry = 0;
        _Pragma("unroll") for (unsigned index = 0; index < length; ++index) {
            UInt128 product = static_cast<UInt128>(x[index]) * x[index];
            result[2 * index] = addWithCarry(mixed[2 * index], static_cast<Digit>(product), carry);
            result[2 * index + 1] = addWithCarry(mixed[2 * index + 1], static_cast<Digit>(product >> JSBigInt::digitBits), carry);
        }
    }

    static ALWAYS_INLINE Digit quotientOfOneDigitMore(const Digit* x, const Digit* y, unsigned length)
    {
        unsigned shift = clz(y[length - 1]);
        auto shifted = [&](const Digit* digits, unsigned index) ALWAYS_INLINE_LAMBDA {
            return digits[index] << shift | (index ? digits[index - 1] >> 1 >> (JSBigInt::digitBits - 1 - shift) : 0);
        };
        Digit highOfDivisor = shifted(y, length - 1);
        Digit high = x[length] << shift | x[length - 1] >> 1 >> (JSBigInt::digitBits - 1 - shift);
        if (high >= highOfDivisor || x[length] >> 1 >> (JSBigInt::digitBits - 1 - shift))
            return 0;
        UInt128 dividend = static_cast<UInt128>(high) << JSBigInt::digitBits | shifted(x, length - 1);
        UInt128 quotient = dividend / highOfDivisor;
        UInt128 remainder = dividend - quotient * highOfDivisor;
        Digit nextOfDivisor = shifted(y, length - 2);
        Digit next = shifted(x, length - 2);
        while (!(remainder >> JSBigInt::digitBits) && quotient * nextOfDivisor > (remainder << JSBigInt::digitBits | next)) {
            --quotient;
            remainder += highOfDivisor;
        }
        Digit factor = static_cast<Digit>(quotient);
        Digit carry = 0;
        Digit borrow = 0;
        for (unsigned index = 0; index < length; ++index) {
            UInt128 product = static_cast<UInt128>(y[index]) * factor + carry;
            carry = static_cast<Digit>(product >> JSBigInt::digitBits);
            subtractWithBorrow(x[index], static_cast<Digit>(product), borrow);
        }
        subtractWithBorrow(x[length], carry, borrow);
        return factor - borrow;
    }

    static ALWAYS_INLINE Digit multiplyByDigit(const Digit* x, unsigned length, Digit factor, Digit* result)
    {
        Digit carry = 0;
        for (unsigned index = 0; index < length; ++index) {
            UInt128 product = static_cast<UInt128>(x[index]) * factor + carry;
            result[index] = static_cast<Digit>(product);
            carry = static_cast<Digit>(product >> JSBigInt::digitBits);
        }
        return carry;
    }

    static ALWAYS_INLINE void multiplyDigitsOfAnyLength(const Digit* x, unsigned length, const Digit* y, unsigned rows, Digit* result)
    {
        result[length] = multiplyByDigit(x, length, y[0], result);
        for (unsigned row = 1; row < rows; ++row) {
            Digit carry = 0;
            for (unsigned index = 0; index < length; ++index) {
                UInt128 product = static_cast<UInt128>(x[index]) * y[row] + result[row + index] + carry;
                result[row + index] = static_cast<Digit>(product);
                carry = static_cast<Digit>(product >> JSBigInt::digitBits);
            }
            result[row + length] = carry;
        }
    }

    static ALWAYS_INLINE bool isNearPowerOfTwo(const Digit* divisor)
    {
        static_assert(usualLength == 4);
        constexpr unsigned maxBits = 56;
        Digit high = divisor[3];
        return !~(divisor[1] & divisor[2]) && !(high & (high + 1)) && high >> (JSBigInt::digitBits - 3) && !((-divisor[0] - 1) >> maxBits);
    }

    static ALWAYS_INLINE void foldDigitsOfAnyLength(const Digit* dividend, unsigned dividendLength, const Digit* divisor, Digit* result)
    {
        std::array<Digit, 2 * usualLength> part { };
        Digit* high = part.data() + usualLength;
        unsigned rest = (dividendLength - 1) / usualLength * usualLength;
        for (unsigned index = rest; index < dividendLength; ++index)
            high[index - rest] = dividend[index];
        if (!rest) {
            foldDigits<usualLength>(high, divisor, result);
            return;
        }
        for (;;) {
            rest -= usualLength;
            for (unsigned index = 0; index < usualLength; ++index)
                part[index] = dividend[rest + index];
            foldDigits<2 * usualLength>(part.data(), divisor, high);
            if (!rest)
                break;
        }
        for (unsigned index = 0; index < usualLength; ++index)
            result[index] = high[index];
    }

    template<unsigned dividendLength>
    static ALWAYS_INLINE void foldDigits(const Digit* dividend, const Digit* divisor, Digit* result)
    {
        constexpr unsigned length = usualLength;
        unsigned shift = std::countl_zero(divisor[length - 1]);
        Digit factor = -divisor[0] << shift;
        Digit folded[length];
        Digit carry = 0;
        _Pragma("unroll") for (unsigned index = 0; index < length; ++index) {
            UInt128 sum = static_cast<UInt128>(dividend[index]) + carry;
            if (length + index < dividendLength)
                sum += static_cast<UInt128>(dividend[length + index]) * factor;
            folded[index] = static_cast<Digit>(sum);
            carry = static_cast<Digit>(sum >> JSBigInt::digitBits);
        }
        Digit top = carry << shift | folded[length - 1] >> 1 >> (JSBigInt::digitBits - 1 - shift);
        folded[length - 1] &= divisor[length - 1];
        UInt128 sum = static_cast<UInt128>(top) * -divisor[0] + folded[0];
        folded[0] = static_cast<Digit>(sum);
        Digit high = 0;
        folded[1] = addWithCarry(folded[1], static_cast<Digit>(sum >> JSBigInt::digitBits), high);
        _Pragma("unroll") for (unsigned index = 2; index < length; ++index)
            folded[index] = addWithCarry(folded[index], 0, high);
        while (high || folded[length - 1] >= divisor[length - 1]) [[unlikely]] {
            Digit difference[length];
            Digit borrow = subtractDigits<length>(folded, divisor, difference);
            if (!high && borrow)
                break;
            _Pragma("unroll") for (unsigned index = 0; index < length; ++index)
                folded[index] = difference[index];
            high -= borrow;
        }
        _Pragma("unroll") for (unsigned index = 0; index < length; ++index)
            result[index] = folded[index];
    }

    template<unsigned dividendLength, typename Place>
    static ALWAYS_INLINE JSBigInt* tryFold(Place place, const Digit* dividend, bool sign, JSBigInt* divisor, bool mayBeSum)
    {
        Digit remainder[usualLength];
        const Digit* digitsOfDivisor = divisor->dataStorage();
        if (!isNearPowerOfTwo(digitsOfDivisor)) [[unlikely]]
            return nullptr;
        foldDigits<dividendLength>(dividend, digitsOfDivisor, remainder);
        if (mayBeSum) {
            bool isSum = sign & !divisor->sign();
            Digit sum[usualLength];
            subtractDigits<usualLength>(digitsOfDivisor, remainder, sum);
            Digit anyDigit = 0;
            _Pragma("unroll") for (unsigned index = 0; index < usualLength; ++index) {
                anyDigit |= remainder[index];
                remainder[index] ^= (remainder[index] ^ sum[index]) & -static_cast<Digit>(isSum);
            }
            if (!anyDigit) [[unlikely]]
                return place.zero();
            sign &= !isSum;
        }
        JSBigInt* result = place.tryMake(usualLength, sign);
        if (!result) [[unlikely]]
            return nullptr;
        Digit* digits = result->dataStorage();
        _Pragma("unroll") for (unsigned index = 0; index < usualLength; ++index)
            digits[index] = remainder[index];
        if (!digits[usualLength - 1]) [[unlikely]]
            return place.finishTrimmed(result, usualLength);
        return place.finish(result);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* trySum(Place place, JSBigInt* left, bool leftIsNegative, JSBigInt* right, bool rightIsNegative)
    {
        unsigned leftLength = left->length();
        unsigned rightLength = right->length();
        if (!rightLength)
            return place.tryCopy(left);
        if (!leftLength)
            return rightIsNegative == right->sign() ? place.tryCopy(right) : tryCopyDigits(place, right, rightIsNegative);
        if (leftIsNegative == rightIsNegative) {
            if (leftLength < rightLength) {
                std::swap(left, right);
                std::swap(leftLength, rightLength);
            }
            return tryAddMagnitudes(place, left, leftLength, right, rightLength, leftIsNegative);
        }
        if (leftLength == rightLength) {
            unsigned index = leftLength - 1;
            while (left->digit(index) == right->digit(index)) {
                if (!index)
                    return place.zero();
                --index;
            }
            if (left->digit(index) > right->digit(index))
                return trySubtractMagnitudes(place, left, leftLength, right, rightLength, leftIsNegative);
            return trySubtractMagnitudes(place, right, rightLength, left, leftLength, rightIsNegative);
        }
        if (leftLength > rightLength)
            return trySubtractMagnitudes(place, left, leftLength, right, rightLength, leftIsNegative);
        return trySubtractMagnitudes(place, right, rightLength, left, leftLength, rightIsNegative);
    }

    template<bool isDecrement, typename Place>
    static ALWAYS_INLINE JSBigInt* tryStep(Place place, JSBigInt* operand)
    {
        unsigned length = operand->length();
        if (!length || length > maxTermLength) [[unlikely]]
            return nullptr;
        bool sign = operand->sign();
        bool growsMagnitude = sign == isDecrement;
        Digit low = operand->digit(0);
        if (!growsMagnitude && length == 1 && low == 1)
            return place.zero();
        if (growsMagnitude ? !~low : !low) [[unlikely]]
            return nullptr;
        JSBigInt* result = place.tryMake(length, sign);
        if (!result) [[unlikely]]
            return nullptr;
        result->setDigit(0, growsMagnitude ? low + 1 : low - 1);
        for (unsigned index = 1; index < length; ++index)
            result->setDigit(index, operand->digit(index));
        return place.finish(result);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* trySumOfUsualLength(Place place, JSBigInt* left, bool leftIsNegative, JSBigInt* right, bool rightIsNegative)
    {
        const Digit* x = left->dataStorage();
        const Digit* y = right->dataStorage();
        constexpr unsigned high = usualLength - 1;
        if (leftIsNegative == rightIsNegative) {
            if ((x[high] | y[high]) >> (JSBigInt::digitBits - 1)) [[unlikely]]
                return nullptr;
            JSBigInt* result = place.tryMake(usualLength, leftIsNegative);
            if (!result) [[unlikely]]
                return nullptr;
            addDigits<usualLength>(x, y, result->dataStorage());
            return place.finish(result);
        }
        if (x[high] == y[high]) [[unlikely]]
            return nullptr;
        if (x[high] < y[high]) {
            std::swap(x, y);
            leftIsNegative = rightIsNegative;
        }
        JSBigInt* result = place.tryMake(usualLength, leftIsNegative);
        if (!result) [[unlikely]]
            return nullptr;
        Digit* digits = result->dataStorage();
        subtractDigits<usualLength>(x, y, digits);
        if (!digits[high]) [[unlikely]]
            return place.finishTrimmed(result, usualLength);
        return place.finish(result);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryAddMagnitudes(Place place, JSBigInt* longer, unsigned length, JSBigInt* shorter, unsigned shorterLength, bool sign)
    {
        if (length > maxTermLength) [[unlikely]]
            return nullptr;
        const Digit* x = longer->dataStorage();
        const Digit* y = shorter->dataStorage();
        Digit highDigits = length == shorterLength ? x[length - 1] | y[length - 1] : x[length - 1];
        bool mayCarry = length == shorterLength ? highDigits >> (JSBigInt::digitBits - 1) : !~highDigits;
        JSBigInt* result = place.tryMake(length + mayCarry, sign);
        if (!result) [[unlikely]]
            return nullptr;
        Digit* digits = result->dataStorage();
        Digit carry;
        if (length == 4 && shorterLength == 4)
            carry = addDigits<4>(x, y, digits);
        else {
            carry = 0;
            for (unsigned index = 0; index < shorterLength; ++index)
                digits[index] = addWithCarry(x[index], y[index], carry);
            for (unsigned index = shorterLength; index < length; ++index)
                digits[index] = addWithCarry(x[index], 0, carry);
        }
        if (mayCarry) {
            digits[length] = carry;
            result->setLength(length + carry);
        }
        return place.finish(result);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* trySubtractMagnitudes(Place place, JSBigInt* larger, unsigned length, JSBigInt* smaller, unsigned smallerLength, bool sign)
    {
        if (length > maxTermLength) [[unlikely]]
            return nullptr;
        JSBigInt* result = place.tryMake(length, sign);
        if (!result) [[unlikely]]
            return nullptr;
        const Digit* x = larger->dataStorage();
        const Digit* y = smaller->dataStorage();
        Digit* digits = result->dataStorage();
        if (length == 4 && smallerLength == 4)
            subtractDigits<4>(x, y, digits);
        else {
            Digit borrow = 0;
            for (unsigned index = 0; index < smallerLength; ++index)
                digits[index] = subtractWithBorrow(x[index], y[index], borrow);
            for (unsigned index = smallerLength; index < length; ++index)
                digits[index] = subtractWithBorrow(x[index], 0, borrow);
        }
        while (!digits[length - 1])
            --length;
        result->setLength(length);
        return place.finish(result);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryRemainderByDigit(Place place, JSBigInt* dividend, Digit divisor)
    {
        unsigned length = dividend->length();
        if (!length)
            return place.zero();
        if (length > 2)
            return nullptr;
        Digit remainder = dividend->digit(0) % divisor;
        if (length == 2)
            remainder = static_cast<Digit>((static_cast<UInt128>(dividend->digit(1) % divisor) << JSBigInt::digitBits | dividend->digit(0)) % divisor);
        if (!remainder)
            return place.zero();
        JSBigInt* result = place.tryMake(1, dividend->sign());
        if (!result) [[unlikely]]
            return nullptr;
        result->setDigit(0, remainder);
        return place.finish(result);
    }

    template<typename Place>
    static ALWAYS_INLINE JSBigInt* tryRemainderOfSameLength(VM& vm, Place place, JSBigInt* dividend, JSBigInt* divisor)
    {
        unsigned length = divisor->length();
        if (dividend->length() != length || length < 2 || length > maxFactorLength || dividend->digit(length - 1) <= divisor->digit(length - 1))
            return nullptr;
        if (vm.m_nextCachedBigIntDivisor.get() == divisor)
            return nullptr;
        JSBigInt* result = place.tryMake(length, dividend->sign());
        if (!result) [[unlikely]]
            return nullptr;
        if (!isInFrame(divisor)) {
            vm.m_nextCachedBigIntDivisor.setWithoutWriteBarrier(divisor);
            vm.m_bigIntDivisorCount = 1;
        }
        JSBigInt::remainderSameSize(digitsOf(result), digitsOf(dividend), digitsOf(divisor));
        return place.finishTrimmed(result, length);
    }
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
