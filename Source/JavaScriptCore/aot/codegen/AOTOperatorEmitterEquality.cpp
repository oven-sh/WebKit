/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTControlEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsStrings.h"
#include "AOTStubsValues.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "JSCJSValueInlines.h"
#include "JSType.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t bitsOfQuietNaN = 0x7ffull << 52 | 1ull << 51;
static_assert((undefinedDouble & bitsOfQuietNaN) == bitsOfQuietNaN && (nullDouble & bitsOfQuietNaN) == bitsOfQuietNaN);

namespace {

class EqualityEmitter final : public OperationEmitter {
public:
    explicit EqualityEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    Condition callEqualStub(const OperationContext& context, bool isStrict, Boxed left, Boxed right, const CallOptions& options = { })
    {
        return Condition(m_out.castToInt32(callStub(context, AOT::equal(isStrict), { left.value(), right.value() }, options).first));
    }

    Condition equalityOnBigInts(const ValueContext& value, bool isStrict, CellPointer left, CellPointer right)
    {
        auto callEqual = [&] {
            return callEqualStub(value, isStrict, boxCell(left), boxCell(right));
        };
        if (!generator().runsRepeatedly(value.block()))
            return callEqual();
        Word32 length = loadField<Word32>(left, m_heaps.JSBigInt_length);
        return selectByBranch<Condition>(equal(length, loadField<Word32>(right, m_heaps.JSBigInt_length)), [&] {
            return either<Condition>(belowOrEqual(length, 1), [&] {
                return selectByBranch<Condition>(isZero(length), [&] {
                    return boolean(true);
                }, [&] {
                    Condition haveSameDigit = equal(loadField<Integer>(left, m_heaps.JSBigInt_data), loadField<Integer>(right, m_heaps.JSBigInt_data));
                    Condition haveSameSign = equal(loadByte(left, m_heaps.JSBigInt_sign), loadByte(right, m_heaps.JSBigInt_sign));
                    return Condition(m_out.bitAnd(haveSameDigit.value(), haveSameSign.value()));
                });
            }, callEqual);
        }, [&] {
            return boolean(false);
        });
    }

    bool comparesOnlyNumbers(const ValueContext& value)
    {
        Vector<TypeID, 2> provenTypes;
        Vector<Representation, 2> representations;
        for (ValueID operand : value->operands()) {
            provenTypes.append(typeIDOf(operand));
            representations.append(representationOf(operand));
        }
        RepresentationContext context { m_program, m_plan, types(), m_unit };
        return isEqualityOfNumbers(context, value.value(), OperandForms { provenTypes.span(), representations.span() });
    }

    void noteFormAgainstWrittenText(const ValueContext& value, StringID literal)
    {
        constexpr StateOfKnowledge inPlace = StateOfKnowledge::LiteralComparedInPlace;
        static_assert(formByLoopAlone(Opcode::StrictEq, inPlace, true).form == formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, true).form);
        static_assert(formByLoopAlone(Opcode::StrictEq, inPlace, false).form == formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, false).form);
        static_assert(formByLoopAlone(Opcode::Eq, inPlace, true).form == formByLoopAlone(Opcode::StrictEq, inPlace, true).form && formByLoopAlone(Opcode::Eq, inPlace, false).form == formByLoopAlone(Opcode::StrictEq, inPlace, false).form);
        if (!m_records.formsSaid.areNoted())
            return;
        StringEmitter& strings = emitter<StringEmitter>();
        String text = strings.textOf(literal);
        if (text.isEmpty()) {
            m_out.noteForm(Form::Open, StateOfKnowledge::EmptyLiteral);
            return;
        }
        if (!text.is8Bit() || !StringEmitter::canCompareInline(text)) {
            m_out.noteForm(Form::Closed, StateOfKnowledge::LiteralComparedByStub);
            return;
        }
        FormAtPlace named = formByLoopAlone(value.opcode(), inPlace, generator().usesLoopForms(value.block()));
        m_out.noteForm(named.form, inPlace, named.place);
    }

    Condition looseEqualityOnBoxed(const ValueContext& value)
    {
        m_out.noteForm(Form::Closed, stateOf(value->form()));
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        Boxed x = conversions.box(value, get(left), typeOf(left));
        Boxed y = conversions.box(value, get(right), typeOf(right));
        return callEqualStub(value, false, x, y);
    }

    Condition isMultipleOfConstant(ValueID remainder, ValueID comparedWith)
    {
        if (m_code.useCountOf(remainder) != 1)
            return { };
        const Operation& divided = operationAt(definitionOf(remainder));
        if (divided.opcode != Opcode::RemainderOfIntegers)
            return { };
        const Operation& zero = operationAt(definitionOf(comparedWith));
        if (zero.opcode != Opcode::Integer || zero.integer())
            return { };
        ValueID dividend = divided.dividendOperand();
        Representation representation = representationOf(dividend);
        Range dividends = typeOf(dividend).integers;
        Range divisors = typeOf(divided.divisorOperand()).integers;
        bool isSigned = dividends.minimum < 0;
        if ((representation != Representation::Int32 && representation != Representation::Int64) || !divisors.isSingleValue() || !dividends.isSubsetOf(isSigned ? Range::int32() : Range::uint32()))
            return { };
        int64_t divisor = std::abs(divisors.minimum);
        if (divisor < 3 || divisor > std::numeric_limits<int32_t>::max() || hasOneBitSet(static_cast<uint64_t>(divisor)))
            return { };

        unsigned shift = std::countr_zero(static_cast<uint32_t>(divisor));
        uint32_t oddPart = static_cast<uint32_t>(divisor) >> shift;
        uint32_t inverse = oddPart;
        while (inverse * oddPart != 1)
            inverse *= 2 - inverse * oddPart;
        uint32_t offset = isSigned ? (std::numeric_limits<int32_t>::max() / oddPart) >> shift << shift : 0;
        uint32_t greatestQuotient = isSigned ? 2 * offset >> shift : std::numeric_limits<uint32_t>::max() / static_cast<uint32_t>(divisor);
        Word32 quotient = mul(representation == Representation::Int32 ? in<Word32>(dividend) : low32(in<Integer>(dividend)), word32(static_cast<int32_t>(inverse)));
        if (offset)
            quotient = add(quotient, word32(static_cast<int32_t>(offset)));
        if (shift)
            quotient = Word32(m_out.bitOr(m_out.lShr(quotient.value(), shift), m_out.shl(quotient.value(), 32 - shift)));
        build().statistics().count(StatisticID::MultiplesTestedWithoutRemainder);
        return belowOrEqual(quotient, word32(static_cast<int32_t>(greatestQuotient)));
    }

    Condition equalityOnIntegers(const ValueContext& value)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        if (Condition isMultiple = isMultipleOfConstant(left, right))
            return isMultiple;
        if (Condition isMultiple = isMultipleOfConstant(right, left))
            return isMultiple;
        switch (formOfOperands(value)) {
        case Representation::Int32: {
            Word32 x = in<Word32>(left);
            return equal(x, in<Word32>(right));
        }
        case Representation::Int64: {
            Integer x = in<Integer>(left);
            return equal(x, in<Integer>(right));
        }
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Double:
        case Representation::OptionalInt64:
        case Representation::OptionalDouble:
        case Representation::Cell:
        case Representation::Cell32:
        case Representation::JSValue:
        case Representation::RawWord:
            break;
        }
        return { };
    }

    Condition strictEqualityOnBoxesThatHoldNumbersOrNothing(const ValueContext& value)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        Boxed x = in<Boxed>(left);
        Boxed y = in<Boxed>(right);
        return selectByBranch<Condition>(isInt32(Boxed(m_out.bitAnd(x.value(), y.value()))), [&] {
            return equal(int32InBox(x), int32InBox(y));
        }, [&] {
            ConversionEmitter& conversions = emitter<ConversionEmitter>();
            Operand first = conversions.unbox(x, typeOf(left), Representation::OptionalDouble);
            Operand second = conversions.unbox(y, typeOf(right), Representation::OptionalDouble);
            Condition areEqualNumbers = equal(conversions.withoutMarkers(first).as<Number>(), conversions.withoutMarkers(second).as<Number>());
            Condition areSameNothing = logicalAnd(equal(conversions.bitsOf(first), conversions.bitsOf(second)), conversions.isMarker(first));
            return logicalOr(areEqualNumbers, areSameNothing);
        });
    }

    Condition strictEqualityOnOptionalDoubles(const ValueContext& value, Number x, Number y)
    {
        auto holdSameMarker = [&] {
            ConversionEmitter& conversions = emitter<ConversionEmitter>();
            Operand left = get(value->leftOperand());
            return logicalAnd(equal(conversions.bitsOf(left), conversions.bitsOf(get(value->rightOperand()))), conversions.isMarker(left));
        };
        return selectByBranch<Condition>(equalOrUnordered(x, y), [&] {
            if (!generator().paysForCodeInLine(value.block()))
                return logicalOr(equal(x, y), holdSameMarker());
            return selectByBranch<Condition>(equal(x, y), [&] { return boolean(true); }, holdSameMarker);
        }, [&] {
            return boolean(false);
        });
    }

    Condition equalityOnNumbers(const ValueContext& value, bool isStrict)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        if (isStrict && get(left).is<Boxed>() && get(right).is<Boxed>())
            return strictEqualityOnBoxesThatHoldNumbersOrNothing(value);
        if (isStrict && get(left).representation == Representation::OptionalInt64 && get(right).representation == Representation::OptionalInt64) {
            ConversionEmitter& conversions = emitter<ConversionEmitter>();
            Integer leftBits = conversions.bitsOf(get(left));
            return equal(leftBits, conversions.bitsOf(get(right)));
        }
        ConversionEmitter& integers = emitter<ConversionEmitter>();
        std::optional<ConversionEmitter::ComparedInteger> leftInteger = isStrict ? integers.findComparedInteger(get(left)) : std::nullopt;
        std::optional<ConversionEmitter::ComparedInteger> rightInteger = leftInteger ? integers.findComparedInteger(get(right)) : std::nullopt;
        if (rightInteger && (leftInteger->mayBeMarker || rightInteger->mayBeMarker) && ConversionEmitter::areComparedAsIntegers(*leftInteger, *rightInteger)) {
            Condition areSame = equal(leftInteger->value, rightInteger->value);
            if (leftInteger->mayBeMarker && rightInteger->mayBeMarker && (leftInteger->markerIsNaN || rightInteger->markerIsNaN))
                areSame = logicalAnd(areSame, logicalNot(integers.isMarker(Operand(Representation::OptionalInt64, leftInteger->value.value()))));
            if (emittersCheckProofs()) {
                Condition areEqual = equal(numberOrNaN(value, left), numberOrNaN(value, right));
                if (get(left).is<OptionalNumber>() && get(right).is<OptionalNumber>())
                    areEqual = logicalOr(areEqual, logicalAnd(equal(integers.bitsOf(get(left)), integers.bitsOf(get(right))), integers.isMarker(get(left))));
                emitter<ThrowEmitter>().requireOrCrash(value, equal(areSame, areEqual), std::nullopt, ProofSource::OtherOperation);
            }
            return areSame;
        }
        uint16_t leftPrimitives = typeOf(left).primitives;
        uint16_t rightPrimitives = typeOf(right).primitives;
        bool mayBothBeNumbers = (leftPrimitives & Primitive::Number) && (rightPrimitives & Primitive::Number);
        bool mayBothBeNullish = (leftPrimitives & Primitive::UndefinedOrNull) && (rightPrimitives & Primitive::UndefinedOrNull);
        bool mayBeSameNullish = leftPrimitives & rightPrimitives & Primitive::UndefinedOrNull;
        bool mayBeEqualNullish = isStrict ? mayBeSameNullish : mayBothBeNullish;

        if (isStrict && mayBothBeNumbers && mayBeEqualNullish && get(left).representation == Representation::OptionalDouble && get(right).representation == Representation::OptionalDouble)
            return strictEqualityOnOptionalDoubles(value, numberOrNaN(value, left), numberOrNaN(value, right));

        Condition areEqualNumbers;
        if (mayBothBeNumbers)
            areEqualNumbers = equal(numberOrNaN(value, left), numberOrNaN(value, right));
        if (!mayBeEqualNullish)
            return areEqualNumbers ? areEqualNumbers : boolean(false);

        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Condition areEqualNullish;
        if (isStrict) {
            if (get(left).representation != get(right).representation)
                return { };
            Integer leftBits = conversions.bitsOf(get(left));
            Integer rightBits = conversions.bitsOf(get(right));
            areEqualNullish = equal(leftBits, rightBits);
            if (mayBothBeNumbers)
                areEqualNullish = logicalAnd(areEqualNullish, conversions.isMarker(get(left)));
            return logicalOr(areEqualNumbers, areEqualNullish);
        }

        for (ValueID id : { left, right }) {
            if (!(typeOf(id).primitives & Primitive::Number))
                continue;
            Condition isNullish = conversions.isMarker(get(id));
            areEqualNullish = areEqualNullish ? logicalAnd(areEqualNullish, isNullish) : isNullish;
        }
        if (!areEqualNullish)
            return boolean(true);
        return logicalOr(areEqualNumbers, areEqualNullish);
    }

    Condition equalityOnBits(const ValueContext& value, bool isStrict)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        switch (formOfOperands(value)) {
        case Representation::Cell: {
            CellPointer x = in<CellPointer>(left);
            return equal(x, in<CellPointer>(right));
        }
        case Representation::Cell32: {
            NarrowReference x = in<NarrowReference>(left);
            build().statistics().count(StatisticID::NarrowReferencesCompared);
            return equal(x, in<NarrowReference>(right));
        }
        case Representation::JSValue: {
            Boxed x = in<Boxed>(left);
            return equalityOnBoxedBits(value, x, in<Boxed>(right), isStrict);
        }
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Int32:
        case Representation::Int64:
        case Representation::Double:
        case Representation::OptionalInt64:
        case Representation::OptionalDouble:
        case Representation::RawWord:
            break;
        }
        return { };
    }

    bool hasBranched() const { return m_hasBranched; }

    Condition equalsWrittenText(const ValueContext& value, ComparedOperand operand, LValue subject, StringID text, bool everyCellIsString)
    {
        if (!value.isBranchCondition())
            return emitter<StringEmitter>().equalsLiteral(value, operand, subject, text, std::nullopt, everyCellIsString);
        emitter<StringEmitter>().branchOnEqualsLiteral(value, operand, subject, text, value.branchTargets(), std::nullopt, everyCellIsString);
        m_hasBranched = true;
        return { };
    }

    Condition equalityOnStrings(const ValueContext& value, bool isStrict)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        switch (formOfOperands(value)) {
        case Representation::Cell:
        case Representation::JSValue: {
            if (!typeOf(left).isAnyString() || !typeOf(right).isAnyString())
                return equalityOnNullableStrings(value, isStrict);
            for (auto [written, other] : { std::pair { right, left }, std::pair { left, right } }) {
                const Operation& definition = operationAt(definitionOf(written));
                if (definition.opcode == Opcode::String) {
                    noteFormAgainstWrittenText(value, definition.string());
                    return equalsWrittenText(value, ComparedOperand::String, in<CellPointer>(other).value(), definition.string(), true);
                }
            }
            m_out.noteForm(Form::Closed, stateOf(value->form()));
            CellPointer x = in<CellPointer>(left);
            return emitter<StringEmitter>().areEqual(value, x, in<CellPointer>(right));
        }
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Int32:
        case Representation::Int64:
        case Representation::Double:
        case Representation::OptionalInt64:
        case Representation::OptionalDouble:
        case Representation::Cell32:
        case Representation::RawWord:
            break;
        }
        return { };
    }

    Condition equalityOnNumberAndValue(const ValueContext& value)
    {
        std::optional<MixedOperands> operands = mixedOperandsOf(value, &Type::isNumber);
        if (!operands)
            return { };
        m_out.noteForm(Form::Open, StateOfKnowledge::NumberAndValue);
        Number number = in<Number>(operands->typed);
        Boxed other = in<Boxed>(operands->boxed);
        Condition otherIsNumber = isNumber(other);
        return logicalAnd(otherIsNumber, equal(number, numberToDouble(other)));
    }

    bool branchOnEqualityOfInt32AndValue(const ValueContext& value)
    {
        std::optional<MixedOperands> operands = value.isBranchCondition() ? mixedOperandsOf(value, &Type::isInt32) : std::nullopt;
        if (!operands)
            return false;
        m_out.noteForm(Form::Open, StateOfKnowledge::Int32AndValue);
        BranchTargets targets = value.branchTargets();
        Word32 expected = in<Word32>(operands->typed);
        Boxed other = in<Boxed>(operands->boxed);
        LBasicBlock isInt32OfSomeValue = newBlock();
        LBasicBlock isNotInt32 = newBlock();
        branch(isInt32(other), { normalEdge(isInt32OfSomeValue), rareEdge(isNotInt32) });
        m_out.appendTo(isInt32OfSomeValue);
        branch(equal(unboxInt32(other), expected), targets);
        m_out.appendTo(isNotInt32);
        if (expected.value()->hasInt32() && expected.value()->asInt32())
            branch(equal(other, boxedConstant(JSValue(JSValue::EncodeAsDouble, static_cast<double>(expected.value()->asInt32())))), targets);
        else {
            Condition otherIsNumber = isNumber(other);
            Number otherNumber = unboxDouble(other);
            branch(logicalAnd(otherIsNumber, equal(otherNumber, toNumber(expected))), targets);
        }
        return true;
    }

    Condition equalityOnInt32AndValue(const ValueContext& value)
    {
        std::optional<MixedOperands> operands = mixedOperandsOf(value, &Type::isInt32);
        if (!operands)
            return { };
        m_out.noteForm(Form::Open, StateOfKnowledge::Int32AndValue);
        Word32 expected = in<Word32>(operands->typed);
        Boxed other = in<Boxed>(operands->boxed);
        LBasicBlock isSameInt32 = newBlock();
        LBasicBlock isOtherInt32 = newBlock();
        LBasicBlock isInt32OfSomeValue = newBlock();
        LBasicBlock isNotInt32 = newBlock();
        Join<Condition> join(m_out);
        branch(isInt32(other), { normalEdge(isInt32OfSomeValue), rareEdge(isNotInt32) });
        m_out.appendTo(isInt32OfSomeValue);
        branch(equal(unboxInt32(other), expected), { unlikelyEdge(isSameInt32), normalEdge(isOtherInt32) });
        m_out.appendTo(isSameInt32);
        join.add(boolean(true));
        m_out.appendTo(isOtherInt32);
        join.add(boolean(false));
        m_out.appendTo(isNotInt32);
        if (expected.value()->hasInt32() && expected.value()->asInt32())
            join.add(equal(other, boxedConstant(JSValue(JSValue::EncodeAsDouble, static_cast<double>(expected.value()->asInt32())))));
        else {
            Condition otherIsNumber = isNumber(other);
            Number otherNumber = unboxDouble(other);
            join.add(logicalAnd(otherIsNumber, equal(otherNumber, toNumber(expected))));
        }
        return join.finish();
    }

    Condition equalityOnStringAndValue(const ValueContext& value)
    {
        std::optional<MixedOperands> operands = mixedOperandsOf(value, &Type::isAnyString);
        if (!operands)
            return { };
        auto [typed, boxed] = *operands;
        if (const Operation& definition = operationAt(definitionOf(typed)); definition.opcode == Opcode::String) {
            const Type& type = typeOf(boxed);
            bool everyCellIsString = !type.hasObjects() && !(type.primitives & cellPrimitives & ~Primitive::AnyString);
            noteFormAgainstWrittenText(value, definition.string());
            return equalsWrittenText(value, ComparedOperand::Value, in<Boxed>(boxed).value(), definition.string(), everyCellIsString);
        }
        m_out.noteForm(Form::Closed, StateOfKnowledge::StringAndValue);
        CellPointer string = in<CellPointer>(typed);
        Boxed other = in<Boxed>(boxed);
        if (mayBeEmpty(boxed))
            return callEqualStub(value, true, boxCell(string), other);
        return Condition(m_out.castToInt32(callStub(value, stringEqualsValue(), { string.value(), other.value() }).first));
    }

    void generateLooseEqualityOnValues(const ValueContext& value)
    {
        Boxed x = in<Boxed>(value->leftOperand());
        Boxed y = in<Boxed>(value->rightOperand());
        ComparedKinds kinds = kindsComparedBy(value);
        bool mayBeAbsent = mayBeEmpty(value->leftOperand()) || mayBeEmpty(value->rightOperand());
        if (mayBeAbsent || !kinds.mayBothBeStrings || !generator().takesOpenForm(value, StateOfKnowledge::Generic)) {
            m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
            defineTest(value, callEqualStub(value, false, x, y));
            return;
        }
        build().statistics().count(StatisticID::LooseEqualitiesOpened);
        defineTestByBranching(value, [&](BranchTargets targets) {
            LBasicBlock areCells = newBlock();
            LBasicBlock areNotBothCells = newBlock();
            LBasicBlock areDifferentCells = newBlock();
            LBasicBlock firstIsString = newBlock();
            LBasicBlock areStrings = newBlock();
            LBasicBlock callsStub = newBlock();
            m_out.branch(areBothCells(x, y).value(), normalEdge(areCells), normalEdge(areNotBothCells));
            m_out.appendTo(areCells);
            m_out.branch(equal(x, y).value(), targets.taken, normalEdge(areDifferentCells));
            m_out.appendTo(areDifferentCells);
            m_out.branch(hasCellType(unboxCell(x), StringType).value(), normalEdge(firstIsString), rareEdge(callsStub));
            m_out.appendTo(firstIsString);
            m_out.branch(hasCellType(unboxCell(y), StringType).value(), normalEdge(areStrings), rareEdge(callsStub));
            m_out.appendTo(areStrings);
            emitter<StringEmitter>().branchOnEqualityOfDistinctStrings(value, unboxCell(x), unboxCell(y), targets);
            m_out.appendTo(areNotBothCells);
            LBasicBlock areInt32s = newBlock();
            m_out.branch(isInt32(Boxed(m_out.bitAnd(x.value(), y.value()))).value(), normalEdge(areInt32s), rareEdge(callsStub));
            m_out.appendTo(areInt32s);
            branch(equal(x, y), targets);
            m_out.appendTo(callsStub);
            branch(callEqualStub(value, false, x, y), targets);
        });
    }

    void generateStrictEqualityOnValues(const ValueContext& value)
    {
        Boxed x = in<Boxed>(value->leftOperand());
        Boxed y = in<Boxed>(value->rightOperand());
        if (value->leftOperand() == value->rightOperand()) {
            m_out.noteForm(Form::Open, StateOfKnowledge::ValueAndItself);
            defineTestByBranching(value, [&](BranchTargets targets) {
                LBasicBlock isNoInt32 = newBlock();
                LBasicBlock isDouble = newBlock();
                m_out.branch(isInt32(x).value(), targets.taken, normalEdge(isNoInt32));
                m_out.appendTo(isNoInt32);
                m_out.branch(isNumber(x).value(), normalEdge(isDouble), targets.taken);
                m_out.appendTo(isDouble);
                Number number = unboxDouble(x);
                branch(equal(number, number), targets);
            });
            return;
        }
        if (bitsDecide(kindsComparedBy(value))) {
            m_out.noteForm(Form::Open, StateOfKnowledge::Bits);
            defineTest(value, equal(x, y));
            return;
        }
        bool isOpen = generator().takesOpenForm(value, StateOfKnowledge::Generic) || generator().isInRecursion(value.block());
        if (asksForInt32sFirst(value) && !isOpen) {
            m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
            defineTestByBranching(value, [&](BranchTargets targets) {
                LBasicBlock areInt32s = newBlock();
                LBasicBlock callsStub = newBlock();
                m_out.branch(isInt32(Boxed(m_out.bitAnd(x.value(), y.value()))).value(), normalEdge(areInt32s), rareEdge(callsStub));
                m_out.appendTo(areInt32s);
                branch(equal(x, y), targets);
                m_out.appendTo(callsStub);
                LValue operands[] = { x.value(), y.value() };
                emitter<RuntimeCallEmitter>().callStubAndBranch(value, strictEqualInFlags(), operands, { }, targets);
            });
            return;
        }
        ValueID expectedObject = value->expectsObjectOnLeft() ? value->leftOperand() : value->expectsObjectOnRight() ? value->rightOperand() : ValueID();
        bool asksKindInLine = expectedObject && !mayBeEmpty(expectedObject) && generator().code().at(value.block()).frequency != BlockFrequency::Rare;
        if (asksKindInLine && isOpen) {
            Boxed tested = expectedObject == value->leftOperand() ? x : y;
            defineTestByBranching(value, [&](BranchTargets targets) {
                LBasicBlock isCellOfSomeKind = newBlock();
                LBasicBlock bitsTell = newBlock();
                LBasicBlock callsStub = newBlock();
                m_out.branch(isCell(tested).value(), normalEdge(isCellOfSomeKind), rareEdge(callsStub));
                m_out.appendTo(isCellOfSomeKind);
                m_out.branch(hasCellTypeAbove(unboxCell(tested), static_cast<JSType>(LastValueCompareCellType)).value(), normalEdge(bitsTell), rareEdge(callsStub));
                m_out.appendTo(bitsTell);
                branch(equal(x, y), targets);
                m_out.appendTo(callsStub);
                LValue operands[] = { x.value(), y.value() };
                emitter<RuntimeCallEmitter>().callStubAndBranch(value, strictEqualInFlags(), operands, { }, targets);
            });
            return;
        }
        if (!isOpen) {
            if (value.isBranchCondition()) {
                LValue operands[] = { x.value(), y.value() };
                emitter<RuntimeCallEmitter>().callStubAndBranch(value, strictEqualInFlags(), operands, { }, value.branchTargets());
                return;
            }
            defineTest(value, callEqualStub(value, true, x, y));
            return;
        }
        if (value.isBranchCondition()) {
            branchOnEqualityOfValues(value, x, y, value.branchTargets());
            return;
        }
        defineTest(value, areValuesEqual(value, x, y));
    }

private:
    struct MixedOperands {
        ValueID typed;
        ValueID boxed;
    };

    std::optional<MixedOperands> mixedOperandsOf(const ValueContext& value, bool (Type::*isTyped)() const)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        if ((typeOf(left).*isTyped)())
            return MixedOperands { left, right };
        if ((typeOf(right).*isTyped)())
            return MixedOperands { right, left };
        return std::nullopt;
    }

    Representation formOfOperands(const ValueContext& value)
    {
        ValueID left = value->leftOperand();
        return representationOf(constantsInFormOfUses.contains(opcodeOf(left)) ? value->rightOperand() : left);
    }

    Number numberOrNaN(const ValueContext& value, ValueID id)
    {
        switch (representationOf(id)) {
        case Representation::Double:
            return in<Number>(id);
        case Representation::OptionalDouble:
            return emitter<ConversionEmitter>().withoutMarkers(get(id)).as<Number>();
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Int32:
        case Representation::Int64:
        case Representation::OptionalInt64:
        case Representation::Cell:
        case Representation::Cell32:
        case Representation::JSValue:
        case Representation::RawWord:
            break;
        }
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return number(0);
    }

    Condition equalityOnBoxedBits(const ValueContext& value, Boxed x, Boxed y, bool isStrict)
    {
        Condition haveSameBits = equal(x, y);
        if (isStrict)
            return haveSameBits;
        auto leftPrimitives = typeOf(value->leftOperand()).primitives;
        auto rightPrimitives = typeOf(value->rightOperand()).primitives;
        bool mayBeUndefinedAndNull = (leftPrimitives & Primitive::Undefined && rightPrimitives & Primitive::Null) || (leftPrimitives & Primitive::Null && rightPrimitives & Primitive::Undefined);
        if (!mayBeUndefinedAndNull)
            return haveSameBits;
        Condition isLeftNullish = isUndefinedOrNull(x, leftPrimitives);
        Condition isRightNullish = isUndefinedOrNull(y, rightPrimitives);
        return logicalOr(haveSameBits, logicalAnd(isLeftNullish, isRightNullish));
    }

    Condition equalityOnNullableStrings(const ValueContext& value, bool isStrict)
    {
        for (auto [written, other] : { std::pair { value->rightOperand(), value->leftOperand() }, std::pair { value->leftOperand(), value->rightOperand() } }) {
            const Operation& definition = operationAt(definitionOf(written));
            if (definition.opcode == Opcode::String) {
                noteFormAgainstWrittenText(value, definition.string());
                return equalsWrittenText(value, ComparedOperand::Value, in<Boxed>(other).value(), definition.string(), true);
            }
        }
        m_out.noteForm(Form::Closed, stateOf(value->form()));
        Boxed x = in<Boxed>(value->leftOperand());
        Boxed y = in<Boxed>(value->rightOperand());
        if (!mayBeEmpty(value->leftOperand()) && !mayBeEmpty(value->rightOperand())) {
            if (typeOf(value->leftOperand()).isAnyString())
                return Condition(m_out.castToInt32(callStub(value, stringEqualsNullable(), { x.value(), y.value() }).first));
            if (typeOf(value->rightOperand()).isAnyString())
                return Condition(m_out.castToInt32(callStub(value, stringEqualsNullable(), { y.value(), x.value() }).first));
            return Condition(m_out.castToInt32(callStub(value, nullableStringsAreEqual(isStrict), { x.value(), y.value() }).first));
        }
        return callEqualStub(value, isStrict, x, y);
    }

    bool mayBeEmpty(ValueID id) { return typeOf(id).primitives & Primitive::Absent; }

    struct ComparedKinds {
        bool typesAreExhaustive;
        bool mayBothBeNaN;
        bool mayBothBeNumbers;
        bool mayBothBeStrings;
        bool mayBeEqualByContents;
    };

    ComparedKinds kindsComparedBy(const ValueContext& value)
    {
        Type left = typeOf(value->leftOperand());
        Type right = typeOf(value->rightOperand());
        bool typesAreExhaustive = left.isJavaScriptValue() && right.isJavaScriptValue();
        auto mayBothBe = [&](uint16_t primitives) {
            return !typesAreExhaustive || (left.primitives & primitives && right.primitives & primitives);
        };
        bool mayBothBeStrings = mayBothBe(Primitive::AnyString);
        return { typesAreExhaustive, mayBothBe(Primitive::WholeNumber), mayBothBe(Primitive::Number), mayBothBeStrings, mayBothBeStrings || mayBothBe(Primitive::BigInt) };
    }

    bool asksForInt32sFirst(const ValueContext& value)
    {
        return value->opcode == Opcode::StrictEq && value->expectsInt32s() && generator().code().at(value.block()).frequency != BlockFrequency::Rare;
    }

    static bool bitsDecide(const ComparedKinds& kinds) { return !kinds.mayBothBeNumbers && !kinds.mayBeEqualByContents; }

    void decideWithoutContents(const ValueContext& value, Boxed x, Boxed y, BranchTargets targets, NOESCAPE const Invocable<void()> auto& answerByStub, NOESCAPE const Invocable<void()> auto& answerForStrings)
    {
        static_assert(formDependsOnLoopOf(Opcode::StrictEq));
        Type left = typeOf(value->leftOperand());
        Type right = typeOf(value->rightOperand());
        ComparedKinds kinds = kindsComparedBy(value);
        auto [typesAreExhaustive, mayBothBeNaN, mayBothBeNumbers, mayBothBeStrings, mayBeEqualByContents] = kinds;
        auto mayBeComparedByIdentity = [&, typesAreExhaustive = typesAreExhaustive](const Type& type) {
            return !typesAreExhaustive || type.hasObjects() || type.primitives & Primitive::AnySymbol;
        };
        bool areInt32sDecided = mayBothBeNumbers && asksForInt32sFirst(value);
        if (areInt32sDecided) {
            LBasicBlock areInt32s = newBlock();
            LBasicBlock areNotBothInt32s = newBlock();
            m_out.branch(isInt32(Boxed(m_out.bitAnd(x.value(), y.value()))).value(), normalEdge(areInt32s), rareEdge(areNotBothInt32s));
            m_out.appendTo(areInt32s);
            branch(equal(x, y), targets);
            m_out.appendTo(areNotBothInt32s);
        }

        bool comparesStringsHere = (generator().takesFormOfLoops(value, StateOfKnowledge::Generic) || generator().isInRecursion(value.block())) && mayBothBeStrings;

        LBasicBlock hasOtherBits = newBlock();
        LBasicBlock callsStub = newBlock();
        bool stubIsRare = !mayBeEqualByContents || comparesStringsHere;
        auto edgeToStub = [&] {
            return stubIsRare ? rareEdge(callsStub) : unlikelyEdge(callsStub);
        };
        if (mayBothBeNaN) {
            LBasicBlock hasSameBits = newBlock();
            LBasicBlock isSameNumber = newBlock();
            m_out.branch(equal(x, y).value(), unlikelyEdge(hasSameBits), normalEdge(hasOtherBits));
            m_out.appendTo(hasSameBits);
            m_out.branch(isNumber(x).value(), normalEdge(isSameNumber), targets.taken);
            m_out.appendTo(isSameNumber);
            m_out.branch(isInt32(x).value(), targets.taken, edgeToStub());
        } else
            m_out.branch(equal(x, y).value(), targets.taken, normalEdge(hasOtherBits));

        m_out.appendTo(hasOtherBits);
        LBasicBlock areCells = mayBeEqualByContents ? newBlock() : nullptr;
        LBasicBlock areNotBothCells = mayBothBeNumbers && !areInt32sDecided ? newBlock() : nullptr;
        auto nonCellEdge = [&] {
            return areNotBothCells ? normalEdge(areNotBothCells) : areInt32sDecided ? edgeToStub() : targets.notTaken;
        };
        m_out.branch(areBothCells(x, y).value(), areCells ? normalEdge(areCells) : targets.notTaken, nonCellEdge());
        if (areNotBothCells) {
            m_out.appendTo(areNotBothCells);
            m_out.branch(isInt32(Boxed(m_out.bitAnd(x.value(), y.value()))).value(), targets.notTaken, edgeToStub());
        }
        if (areCells) {
            LBasicBlock comparesContents = comparesStringsHere ? newBlock() : callsStub;
            m_out.appendTo(areCells);
            for (auto [id, word] : { std::pair { value->leftOperand(), x }, std::pair { value->rightOperand(), y } }) {
                if (!mayBeEmpty(id))
                    continue;
                LBasicBlock isPresent = newBlock();
                m_out.branch(isEmpty(word).value(), targets.notTaken, normalEdge(isPresent));
                m_out.appendTo(isPresent);
            }
            if (mayBeComparedByIdentity(left) || mayBeComparedByIdentity(right)) {
                CellPointer tested = unboxCell(mayBeComparedByIdentity(left) ? x : y);
                m_out.branch(hasCellTypeAbove(tested, static_cast<JSType>(LastValueCompareCellType)).value(), targets.notTaken, unlikelyEdge(comparesContents));
            } else
                m_out.jump(comparesContents);
            if (comparesStringsHere) {
                LBasicBlock firstIsString = newBlock();
                LBasicBlock areStrings = newBlock();
                m_out.appendTo(comparesContents);
                m_out.branch(hasCellType(unboxCell(x), StringType).value(), normalEdge(firstIsString), rareEdge(callsStub));
                m_out.appendTo(firstIsString);
                m_out.branch(hasCellType(unboxCell(y), StringType).value(), normalEdge(areStrings), targets.notTaken);
                m_out.appendTo(areStrings);
                answerForStrings();
            }
        }
        m_out.appendTo(callsStub);
        answerByStub();
    }

    void branchOnEqualityOfValues(const ValueContext& value, Boxed x, Boxed y, BranchTargets targets)
    {
        auto branchInStub = [&] {
            LValue operands[] = { x.value(), y.value() };
            emitter<RuntimeCallEmitter>().callStubAndBranch(value, strictEqualInFlags(), operands, { }, targets);
        };
        decideWithoutContents(value, x, y, targets, branchInStub, [&] {
            branch(emitter<StringEmitter>().areEqual(value, unboxCell(x), unboxCell(y), StringEmitter::StringIdentity::Distinct), targets);
        });
    }

    Condition areValuesEqual(const ValueContext& value, Boxed x, Boxed y)
    {
        LBasicBlock isSame = newBlock();
        LBasicBlock isOther = newBlock();
        Join<Condition> join(m_out);
        decideWithoutContents(value, x, y, { normalEdge(isSame), normalEdge(isOther) }, [&] {
            join.add(callEqualStub(value, true, x, y));
        }, [&] {
            join.add(emitter<StringEmitter>().areEqual(value, unboxCell(x), unboxCell(y), StringEmitter::StringIdentity::Distinct));
        });
        m_out.appendTo(isSame);
        join.add(boolean(true));
        m_out.appendTo(isOther);
        join.add(boolean(false));
        return join.finish();
    }

    Condition areBothCells(Boxed x, Boxed y) { return isCell(Boxed(m_out.bitOr(x.value(), y.value()))); }

    bool m_hasBranched { false };
};

} // anonymous namespace

static ValueID writtenTextComparedWithValue(const Code& code, const Operation& equality)
{
    for (ValueID operand : { equality.rightOperand(), equality.leftOperand() }) {
        if (code.at(operand).opcode == Opcode::String)
            return operand;
    }
    return { };
}

static_assert(static_cast<uint64_t>(-JSValue::DoubleEncodeOffset) >> 52 == 0xfff && static_cast<uint64_t>(-JSValue::DoubleEncodeOffset) << 12);

void OperatorEmitter::generateNumberOrNaN(const ValueContext& value)
{
    define(value, numberToDouble(in<Boxed>(value->valueOperand())));
}

void OperatorEmitter::generateEquality(const ValueContext& value, bool isStrict)
{
    if (tryGenerateRemainderTest(value))
        return;
    if (emitter<ControlEmitter>().tryGenerateLiteralChain(value))
        return;
    if (isStrict && value->form() == OperatorForm::Bits && emitter<ControlEmitter>().tryGenerateSymbolChain(value))
        return;
    EqualityEmitter equality(generator());
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    Condition result;
    switch (value->form()) {
    case OperatorForm::Generic:
        if (ValueID written = isStrict ? writtenTextComparedWithValue(m_code, value.value()) : ValueID()) {
            ValueID other = written == left ? right : left;
            const Type& type = typeOf(other);
            bool everyCellIsString = !type.hasObjects() && !(type.primitives & cellPrimitives & ~Primitive::AnyString);
            equality.noteFormAgainstWrittenText(value, operationAt(written).string());
            defineTestByBranching(value, [&](BranchTargets targets) {
                emitter<StringEmitter>().branchOnEqualsLiteral(value, ComparedOperand::Value, in<Boxed>(other).value(), operationAt(written).string(), targets, std::nullopt, everyCellIsString);
            });
            return;
        }
        if (bool isRightBigInt = isBigInt(right); isRightBigInt != isBigInt(left) && expectsBigInts(value.block(), left)) {
            m_out.noteForm(Form::Open, StateOfKnowledge::BigInts);
            Boxed known = boxedOperand(isRightBigInt ? right : left);
            Boxed other = in<Boxed>(isRightBigInt ? left : right);
            defineTestByBranching(value, [&](BranchTargets targets) {
                LBasicBlock isCellCase = newBlock();
                LBasicBlock areBigInts = newBlock();
                LBasicBlock isZeroCase = newBlock();
                LBasicBlock otherCase = newBlock();
                branch(isCell(other), { normalEdge(isCellCase), rareEdge(otherCase) });
                m_out.appendTo(isCellCase);
                branch(equal(loadByte(unboxCell(other), m_heaps.JSCell_typeInfoType), static_cast<int32_t>(HeapBigIntType)), { normalEdge(areBigInts), rareEdge(otherCase) });
                m_out.appendTo(areBigInts);
                branch(isZero(loadField<Word32>(unboxCell(known), m_heaps.JSBigInt_length)), { normalEdge(isZeroCase), rareEdge(otherCase) });
                m_out.appendTo(isZeroCase);
                branch(isZero(loadField<Word32>(unboxCell(other), m_heaps.JSBigInt_length)), targets);
                m_out.appendTo(otherCase);
                branch(equality.callEqualStub(value, isStrict, isRightBigInt ? other : known, isRightBigInt ? known : other), targets);
            });
            return;
        }
        if (isStrict)
            equality.generateStrictEqualityOnValues(value);
        else
            equality.generateLooseEqualityOnValues(value);
        return;
    case OperatorForm::Integers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Integers);
        result = equality.equalityOnIntegers(value);
        break;
    case OperatorForm::Numbers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        if (equality.comparesOnlyNumbers(value)) {
            generateComparisonOfNumbers(value, std::nullopt);
            return;
        }
        result = equality.equalityOnNumbers(value, isStrict);
        break;
    case OperatorForm::Bits:
        m_out.noteForm(Form::Open, StateOfKnowledge::Bits);
        result = equality.equalityOnBits(value, isStrict);
        break;
    case OperatorForm::Booleans: {
        m_out.noteForm(Form::Open, StateOfKnowledge::Booleans);
        Condition x = in<Condition>(left);
        result = equal(x, in<Condition>(right));
        break;
    }
    case OperatorForm::BigInts: {
        m_out.noteForm(Form::Closed, StateOfKnowledge::BigInts);
        CellPointer x = in<CellPointer>(left);
        result = equality.equalityOnBigInts(value, isStrict, x, in<CellPointer>(right));
        break;
    }
    case OperatorForm::Strings:
        result = equality.equalityOnStrings(value, true);
        break;
    case OperatorForm::NullableStrings:
        result = equality.equalityOnStrings(value, isStrict);
        break;
    case OperatorForm::NumberAndValue:
        result = isStrict ? equality.equalityOnNumberAndValue(value) : equality.looseEqualityOnBoxed(value);
        break;
    case OperatorForm::StringAndValue:
        result = isStrict ? equality.equalityOnStringAndValue(value) : equality.looseEqualityOnBoxed(value);
        break;
    case OperatorForm::Int32AndValue:
        if (isStrict && equality.branchOnEqualityOfInt32AndValue(value))
            return;
        result = isStrict ? equality.equalityOnInt32AndValue(value) : equality.looseEqualityOnBoxed(value);
        break;
    case OperatorForm::Unselected:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::NullableNumber:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    if (equality.hasBranched())
        return;
    if (!result) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        result = boolean(false);
    }
    defineTest(value, result);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
