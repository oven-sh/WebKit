/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubsArithmetic.h"
#include <array>
#include <limits>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class ConversionEmitter final : public OperationEmitter {
public:
    explicit ConversionEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    Boxed box(const OperationContext&, Operand, const Type&);
    // The engine takes the storage of a list from the form of the word that it is given, and the compiler from the type. See unbox().
    Operand inFormOfType(Operand operand, const Type& type) { return operand.is<Boxed>() && type.isInt32() ? Operand(boxInt32(unbox(operand.as<Boxed>(), type, Representation::Int32).as<Word32>())) : operand; }
    Operand unbox(Boxed, const Type&, Representation);
    Operand toOptional(Operand, const Type&, Representation);
    // For the code of an operation whose result is produced in one representation and wanted in another.
    Operand inRepresentation(const OperationContext&, Operand produced, const Type& producedType, Representation wanted);

    Boxed boxNumber(Operand);
    Boxed boxDouble(Number value) { return Boxed(m_out.sub(m_out.bitCast(value.value(), Machine::Int64), m_numberTag)); }
    Boxed boxNumberByStub(Number value) { return boxByStub(BoxedNumberForm::Double, value.value()); }
    Boxed boxInteger(Integer, Range);
    Number toNumber(Integer, Range);
    Integer toInteger(Number);
    // The int32 that the number is. -0 is none and NaN is none: the bits are compared.
    Word32 int32OrElse(Number, LBasicBlock isNoInt32);
    Word32 int32OrElse(Integer, Range, LBasicBlock isNoInt32);
#if CPU(ARM64)
    // NaN is 0, and a number beyond int64 is the nearest int64.
    Integer toNearestInteger(Number);
#endif
    // The result, without rounding, of the integer arithmetic that made a number. Where the number was rounded, both lie beyond 2^52.
    std::optional<Integer> exactIntegerBehind(Number);
    // The number is the integer, or -0 where the integer is 0. The integer is computed on every way to the number.
    void noteIntegerBehind(Number number, LValue integer) { m_integersNotedBehindNumbers.add(number.value(), integer); }
    Integer widen(Word32, Range);
    Word32 narrow(Integer value) { return low32(value); }
    // A number beyond int64 is a multiple of 2^11, and what toInteger() makes of it, or of NaN, ends in as many zeros: whoever looks at so many of the lowest bits alone needs no test of the number.
    enum class BitsOfInt32 : uint8_t { All, Lowest };
    static constexpr unsigned numberOfLowestBits = numberOfLowestBitsOfInt32;
    Word32 toInt32(Number, BitsOfInt32 = BitsOfInt32::All);
    Word32 toInt32OfBoxedNumber(Boxed);
    // What a read of bytes as a number answers: every NaN is the one NaN.
    Number purified(Number);
    // A NaN keeps the sign that arithmetic gave it and none of its other bits.
    Number withoutPayload(Number);

    Operand decode(NarrowReference, const Type&, Representation);
    NarrowReference compress(Operand, const Type&);

    Integer bitsOf(Operand);
    Operand fromBits(Integer, Representation);
    static uint64_t markerBits(uint16_t primitive, Representation);
    Condition isMarker(Operand);
    Condition isMarker(Operand operand, uint64_t marker)
    {
        if (std::optional<Word32> narrow = operand.is<OptionalInteger>() ? narrowWordBehind(operand.as<OptionalInteger>()) : std::nullopt)
            return equal(*narrow, marker == integerMarkers.absent ? absentInt32 : marker == integerMarkers.undefined ? undefinedInt32 : nullInt32);
        return equal(bitsOf(operand), static_cast<int64_t>(marker));
    }
    Condition isAbsent(Operand);
    Operand withoutMarkers(Operand);

    LiteralStringWord literalStringConstant(StringID);
    LiteralStringWord toLiteralString(CellPointer string);
    LiteralStringWord toLiteralString(Operand, const Type&);
    Boxed boxLiteralString(const OperationContext&, LiteralStringWord, const Type&);
    Condition isStringCell(LiteralStringWord);

private:
    // Whole: an Int64 that holds without a test. Truncated: whoever asks reads the lowest 32 bits alone, behind whatever test it takes.
    enum class ExactIntegerUse : bool { Whole, Truncated };
    struct ExactInteger {
        // An Int64. Where it is truncated and may not be rounded, an Int32 with its lowest bits: sums, differences and products of words have the lowest bits of the exact ones.
        LValue value { nullptr };
        bool mayBeRounded { false };
        // Set where the integer is tested: it is the number only while this holds.
        Condition holdsWhile { };
    };
    // What is known of the integer behind a number before any code computes it.
    struct IntegerBehindNumber {
        Range range;
        bool mayBeRounded { false };
        // The range holds only behind a test: each integer behind the number whose size nothing bounds is an int32.
        bool isTested { false };
        int64_t largestSumOfOtherTerms { 0 };
    };
    using IntegersBehindNumbers = UncheckedKeyHashMap<LValue, std::optional<IntegerBehindNumber>>;
    using ExactIntegers = UncheckedKeyHashMap<LValue, std::optional<ExactInteger>>;
    std::optional<ExactInteger> findExactIntegerBehind(LValue number, ExactIntegerUse);
    ValueID parameterOfLoopHeadFedBy(ValueID);
    bool comesRoundToIntegerBehind(LValue number);
    std::optional<IntegerBehindNumber> describeIntegerBehind(LValue number, bool mayTestWhatWasRead, const IntegersBehindNumbers& operands);
    ExactInteger computeIntegerBehind(LValue number, const IntegerBehindNumber&, bool inWords, const ExactIntegers& operands);
    Word32 toInt32OfAnyNumber(Number, BitsOfInt32);
    Operand convertMarkers(Operand source, const Type&, Representation to, std::optional<Operand> unmarked = std::nullopt);
    Integer integerPartOf(Operand, const Type&);
    Number numberPartOf(Operand, const Type&);
    Boxed boxOptional(Operand, const Type&);
    Boxed boxByStub(BoxedNumberForm, LValue);
    Integer quietNaNWithSignOf(Integer bits) { return Integer(m_out.shl(m_out.bitOr(m_out.lShr(bits.value(), 51), 1), 51)); }
    Operand unboxOptional(Boxed, const Type&, Representation);

    // Set while box() works for a block on the failing side of a test: a call of the stub there, a test and two arms on the way that is taken.
    bool m_boxesNumbersByStub { false };
    // Which of the places has set it.
    Place m_placeThatBoxesByStub { Place::Anywhere };
    UncheckedKeyHashMap<LValue, LValue> m_integersNotedBehindNumbers;
    // By whether what was read may be tested. An operation never changes, so what is known of it holds for the whole function.
    std::array<IntegersBehindNumbers, 2> m_integersBehindNumbers;
    // comesRoundToIntegerBehind(): of the arithmetic that was asked, whether it holds m_integerThatComesRound. A thousand lines that add to one sum are each made of all before them.
    UncheckedKeyHashMap<LValue, bool> m_holdsIntegerThatComesRound;
    LValue m_integerThatComesRound { nullptr };
    // By the index of the value, for the values of Code that have been followed.
    Vector<ValueID> m_parametersOfLoopHeadsFed;
    BitVector m_valuesFollowed;

    using PrimitiveEmitter::compress;
    using PrimitiveEmitter::decode;
    using PrimitiveEmitter::toNumber;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
