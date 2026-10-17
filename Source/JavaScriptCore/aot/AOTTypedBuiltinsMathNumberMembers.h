/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

    std::optional<Number> powerWithConstantExponent(Number base, ValueID exponent);
    std::optional<Number> powerWithConstantBase(ValueID base, ValueID exponent);
    std::optional<bool> isMathMaxOrMin(ValueID) const;
    Number minOrMaxOfElements(bool isMax, CellPointer array, Pointer butterfly, Integer length, LBasicBlock otherwise);
    Integer divide(Integer, Range dividends, Integer, Range divisors);
    std::optional<Integer> roundedQuotient(InlineBuiltin, ValueID quotient, bool yieldsInteger);
    std::optional<Word32> quotientToInt32(ValueID);
    std::optional<int64_t> minIfIntegerOrNaN(ValueID) const;
    Integer integerOrMarker(ValueID);
    std::optional<Word32> compareWithIntegerConstant(const Value&);
    void findNumbersJoinedWithIntegers(const Vector<BlockID>& order);
    UncheckedKeyHashMap<uint32_t, int64_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_minOfNumbersJoinedWithIntegers;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_integerPhis;
    void findNumbersJoinedWithInt32(const Vector<BlockID>& order);
    BitVector m_hasInt32Phi;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_int32Phis;
    void findConstantJoinsUsedAsNumbers();
    BitVector m_isConstantJoinUsedAsNumber;
    UncheckedKeyHashMap<uint32_t, LValue, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_numberPhis;
