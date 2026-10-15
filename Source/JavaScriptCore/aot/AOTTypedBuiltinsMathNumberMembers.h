/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Members of class Lowering: included in its body.

    std::optional<Number> powerWithConstantExponent(Number base, ValueID exponent);
    std::optional<Number> powerWithConstantBase(ValueID base, ValueID exponent);
    // Math.max or Math.min, going by how the value is defined: whether it is Math.max.
    std::optional<bool> isMathMaxOrMin(ValueID) const;
    Number minOrMaxOfElements(bool isMax, CellPointer array, Pointer butterfly, Integer length, LBasicBlock otherwise);
    Integer divide(Integer, Range dividends, Integer, Range divisors);
    std::optional<Integer> roundedQuotient(InlineBuiltin, ValueID quotient, bool yieldsInteger);
    std::optional<Word32> toInt32OfQuotient(ValueID);
    // Of a value that is an integer or not a number by what defines it, whatever it is kept as: the least integer it may be.
    std::optional<int64_t> leastIfIntegerOrNaN(ValueID) const;
    // That integer, or INT64_MIN.
    Integer integerOrMarker(ValueID);
    std::optional<Word32> compareWithIntegerConstant(const Value&);
    // Block parameters kept as doubles to which only such values are passed: the integer is joined beside the double. By parameter: the least integer; the phi.
    void findNumbersJoinedWithIntegers(const Vector<BlockID>& order);
    UncheckedKeyHashMap<uint32_t, int64_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_leastOfNumbersJoinedWithIntegers;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_integerPhis;
    // Block parameters kept as doubles of which ToInt32 is taken, at the head of a loop around which an int32 is passed: ToInt32 of it is joined beside the double.
    void findNumbersJoinedWithInt32(const Vector<BlockID>& order);
    BitVector m_hasInt32Phi;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_int32Phis;
    void findJoinsOfConstantsTakenAsNumbers();
    BitVector m_isJoinOfConstantsTakenAsNumber;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_numberPhis;
