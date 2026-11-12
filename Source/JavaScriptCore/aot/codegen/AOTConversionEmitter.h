/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTOperationEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubsArithmetic.h"
#include <array>
#include <limits>
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class ConversionEmitter final : public OperationEmitter {
public:
    explicit ConversionEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    Boxed box(const OperationContext&, Operand, const Type&);
    Operand inFormOfType(Operand operand, const Type& type) { return operand.is<Boxed>() && type.isInt32() ? Operand(boxInt32(unbox(operand.as<Boxed>(), type, Representation::Int32).as<Word32>())) : operand; }
    Operand unbox(Boxed, const Type&, Representation);
    Operand toOptional(Operand, const Type&, Representation);
    Operand inRepresentation(const OperationContext&, Operand produced, const Type& producedType, Representation wanted);

    Boxed boxNumber(Operand);
    Boxed boxDouble(Number value) { return Boxed(m_out.sub(m_out.bitCast(value.value(), Machine::Int64), m_numberTag)); }
    Boxed boxNumberByStub(Number value) { return boxByStub(BoxedNumberForm::Double, value.value()); }
    Boxed boxNumberByStub(OptionalInteger value) { return boxByStub(BoxedNumberForm::OptionalInteger, value.value()); }
    Boxed boxNumberByStub(OptionalNumber value) { return boxByStub(BoxedNumberForm::OptionalDouble, value.value()); }
    Boxed boxInteger(Integer, Range);
    Number toNumber(Integer, Range);
    Integer toInteger(Number);
    Integer toInteger(Number, Range, bool standsInLoop);
    Word32 int32OrElse(Number, LBasicBlock isNoInt32);
    Word32 int32OrElse(Integer, Range, LBasicBlock isNoInt32);
    Integer truncateUnchecked(Number);
    Word32 truncateToWord(Number);
    std::optional<Integer> exactIntegerBehind(Number);
    void noteIntegerBehind(Number number, LValue integer) { m_integersNotedBehindNumbers.add(number.value(), integer); }
    void noteIndexOfNumber(Number number, Integer index) { m_indexesOfNumbers.add(number.value(), index.value()); }
    void noteIndexOfKey(Number);
    void noteInt32OfNumber(Number number, Integer integer) { m_int32sOfNumbers.add(number.value(), integer.value()); }
    void noteTruncationOfNumber(Number number, Word32 word) { m_truncationsOfNumbers.add(number.value(), word.value()); }
    std::optional<Integer> int32Behind(Number number) const
    {
        LValue integer = m_int32sOfNumbers.get(number.value());
        return integer ? std::optional(Integer(integer)) : std::nullopt;
    }
    void noteHeldAsIndex(Number number) { m_numbersHeldAsIndexes.add(number.value()); }
    bool isHeldAsIndex(LValue number) const { return m_numbersHeldAsIndexes.contains(number); }
    Number numberAsIndex(Number);
    std::optional<Integer> indexBehind(Number number) const
    {
        LValue index = m_indexesOfNumbers.get(number.value());
        return index ? std::optional(Integer(index)) : std::nullopt;
    }
    void noteInt32ElementBehindNumberOrNaN(Number number, Boxed element)
    {
        if (number.value()->opcode() == Machine::Select)
            m_int32ElementsBehindNumbersOrNaN.add(number.value(), element.value());
    }
    bool isOptionalInteger(LValue number) const { return m_optionalIntegersOfNumbers.contains(number); }
    std::optional<Number> convertOptionalInteger(LValue optional);
    std::optional<Number> convertIntegerOrNull(LValue optional);
    void noteNumberOfInt32Element(Number number) { m_numbersOfInt32Elements.add(number.value()); }
    void noteDoubleElement(LValue number) { m_doubleElements.add(number); }
    Integer widen(Word32, Range);
    void noteSignExtended(Word32 word, Integer extended) { m_signExtendedWords.add(word.value(), extended.value()); }
    void forgetSignExtended(Word32 word) { m_signExtendedWords.remove(word.value()); }
    std::optional<Integer> signExtendedIntegerOf(Word32 word) const
    {
        LValue extended = m_signExtendedWords.get(word.value());
        return extended ? std::optional(Integer(extended)) : std::nullopt;
    }
    Word32 lowHalfOfIntegerToldInsideInt32(Integer);
    Word32 lowHalfOfOptionalInteger(OptionalInteger);
    Integer integerKnownNotMarker(OptionalInteger);
    Word32 narrow(Integer value) { return low32(value); }
    enum class BitsOfInt32 : uint8_t { All, Lowest };
    static constexpr unsigned numberOfLowestBits = numberOfLowestBitsOfInt32;
    Word32 toInt32(Number, BitsOfInt32 = BitsOfInt32::All);
    Word32 truncateNullableNumber(Boxed);
    Word32 toInt32OfBoxedNumber(Boxed);
    Number purified(Number);
    Number withoutPayload(Number);
    Number quietNaN(Number);
    void noteNumberWithoutPayload(Operand optional, Number number) { m_numbersWithoutPayload.add(optional.value, number.value()); }

    Operand decode(NarrowReference, const Type&, Representation);
    NarrowReference compress(Operand, const Type&);

    Integer bitsOf(Operand);
    Operand fromBits(Integer, Representation);
    static uint64_t markerBits(uint16_t primitive, Representation);
    Condition isMarker(Operand);
    Condition isNullOrAbsent(OptionalNumber);
    struct ComparedInteger {
        Integer value;
        bool mayBeMarker;
        bool markerIsNaN { false };
        Range range { Range::full() };
        bool isRounded { false };
    };
    static bool areComparedAsIntegers(const ComparedInteger& left, const ComparedInteger& right)
    {
        static constexpr Range unrounded { -maxExactInteger + 1, maxExactInteger - 1 };
        auto mayBeRoundedTo = [](const ComparedInteger& other) {
            return other.isRounded || !other.range.isSubsetOf(unrounded);
        };
        return !(left.isRounded && mayBeRoundedTo(right)) && !(right.isRounded && mayBeRoundedTo(left));
    }
    std::optional<ComparedInteger> findComparedInteger(Operand);
    Condition isMarker(Operand operand, uint64_t marker)
    {
        if (std::optional<Word32> narrow = operand.is<OptionalInteger>() ? narrowWordBehind(operand.as<OptionalInteger>()) : std::nullopt)
            return equal(*narrow, marker == integerMarkers.absent ? absentInt32 : marker == integerMarkers.undefined ? undefinedInt32 : nullInt32);
        return equal(bitsOf(operand), static_cast<int64_t>(marker));
    }
    Condition isAbsent(Operand);
    Operand withoutMarkers(Operand);

private:
    enum class ExactIntegerUse : bool { Whole, Truncated };
    struct ExactInteger {
        LValue value { nullptr };
        bool mayBeRounded { false };
        Condition holdsWhile { };
    };
    struct IntegerBehindNumber {
        Range range;
        bool mayBeRounded { false };
        bool isTested { false };
        bool holdsElementOfList { false };
        bool asksWhetherWideIntegerIsInt32 { false };
        int64_t largestSumOfOtherTerms { 0 };
    };
    using IntegersBehindNumbers = UncheckedKeyHashMap<LValue, std::optional<IntegerBehindNumber>>;
    using ExactIntegers = UncheckedKeyHashMap<LValue, std::optional<ExactInteger>>;
    std::optional<ExactInteger> findExactIntegerBehind(LValue number, ExactIntegerUse);
    ValueID parameterOfLoopHeadFedBy(ValueID);
    bool comesRoundToIntegerBehind(LValue number);
    std::optional<IntegerBehindNumber> describeIntegerBehind(LValue number, bool mayTestWhatWasRead, const IntegersBehindNumbers& operands);
    ExactInteger computeIntegerBehind(LValue number, const IntegerBehindNumber&, bool inWords, const ExactIntegers& operands);
    std::optional<Integer> findTruncation(Number);
#if CPU(ARM64)
    bool convertsWithJavaScriptSemantics();
    Integer exactInt32OrAllOnes(Number);
#endif
    Word32 toInt32OfAnyNumber(Number, BitsOfInt32);
    Operand convertMarkers(Operand source, const Type&, Representation to, std::optional<Operand> unmarked = std::nullopt);
    Integer integerPartOf(Operand, const Type&);
    Number numberPartOf(Operand, const Type&);
    Boxed boxOptional(Operand, const Type&);
    Boxed boxByStub(BoxedNumberForm, LValue);
    Integer quietNaNWithSignOf(Integer bits) { return Integer(m_out.shl(m_out.bitOr(m_out.lShr(bits.value(), 51), 1), 51)); }
    Operand unboxOptional(Boxed, const Type&, Representation);
    Number unboxDoubleThatIsAsked(Boxed);
    void stopUnlessBoxHolds(Boxed, TypeID kinds);
    void stopUnlessToldRangeHolds(Condition);

    bool m_boxesNumbersByStub { false };
    Place m_placeThatBoxesByStub { Place::Anywhere };
    StateOfKnowledge m_stateThatBoxesByStub { StateOfKnowledge::Always };
    UncheckedKeyHashMap<LValue, LValue> m_integersNotedBehindNumbers;
    UncheckedKeyHashMap<LValue, LValue> m_indexesOfNumbers;
    UncheckedKeyHashMap<LValue, LValue> m_int32sOfNumbers;
    UncheckedKeyHashMap<LValue, LValue> m_truncationsOfNumbers;
    UncheckedKeyHashSet<LValue> m_numbersHeldAsIndexes;
    UncheckedKeyHashMap<LValue, LValue> m_int32ElementsBehindNumbersOrNaN;
    struct OptionalIntegerOfNumber {
        LValue integer;
        Range range;
        bool markerIsNaN { false };
    };
    UncheckedKeyHashMap<LValue, OptionalIntegerOfNumber> m_optionalIntegersOfNumbers;
    UncheckedKeyHashMap<LValue, OptionalIntegerOfNumber> m_integersOrNullOfNumbers;
    UncheckedKeyHashMap<LValue, LValue> m_signExtendedWords;
    struct TruncatedNumber {
        LValue number { nullptr };
        LValue word { nullptr };
        LBasicBlock block { nullptr };
    };
    TruncatedNumber m_lastTruncation;
    UncheckedKeyHashSet<LValue> m_numbersOfInt32Elements;
    UncheckedKeyHashSet<LValue> m_doubleElements;
    UncheckedKeyHashMap<LValue, LValue> m_numbersWithoutPayload;
    std::array<IntegersBehindNumbers, 2> m_integersBehindNumbers;
    UncheckedKeyHashMap<LValue, bool> m_holdsIntegerThatComesRound;
    LValue m_integerThatComesRound { nullptr };
    Vector<ValueID> m_parametersOfLoopHeadsFed;
    BitVector m_valuesFollowed;

    using PrimitiveEmitter::compress;
    using PrimitiveEmitter::decode;
    using PrimitiveEmitter::toNumber;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
