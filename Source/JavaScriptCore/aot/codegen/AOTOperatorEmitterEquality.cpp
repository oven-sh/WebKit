/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTStringEmitter.h"
#include "AOTStubsValues.h"
#include "AOTTypeTable.h"
#include "AOTValueIdentity.h"
#include "JSCJSValueInlines.h"
#include "JSType.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t bitsOfQuietNaN = 0x7ffull << 52 | 1ull << 51;
static_assert((undefinedDouble & bitsOfQuietNaN) == bitsOfQuietNaN && (nullDouble & bitsOfQuietNaN) == bitsOfQuietNaN);

static bool mayBeStringCell(LiteralStringWord word, const Type& type)
{
    if (word.value()->hasInt64())
        return static_cast<uint64_t>(word.value()->asInt64()) >= minCell32;
    return type.primitives & Primitive::AnyString;
}

namespace {

// A function that has no code for the representation of its operands answers with no condition.
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

    Condition looseEqualityOnBoxed(const ValueContext& value)
    {
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        Boxed x = conversions.box(value, get(left), typeOf(left));
        Boxed y = conversions.box(value, get(right), typeOf(right));
        return callEqualStub(value, false, x, y);
    }

    Condition equalityOnIntegers(const ValueContext& value)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        switch (representationOf(left)) {
        case Representation::Int32:
            return equal(in<Word32>(left), in<Word32>(right));
        case Representation::Int64:
            return equal(in<Integer>(left), in<Integer>(right));
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Double:
        case Representation::DoubleWithPayload:
        case Representation::ImpureDouble:
        case Representation::OptionalInt64:
        case Representation::OptionalDouble:
        case Representation::Cell:
        case Representation::Cell32:
        case Representation::JSValue:
        case Representation::LiteralString:
        case Representation::RawWord:
            break;
        }
        return { };
    }

    Condition equalityOnNumbers(const ValueContext& value, bool isStrict)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        uint16_t leftPrimitives = typeOf(left).primitives;
        uint16_t rightPrimitives = typeOf(right).primitives;
        bool mayBothBeNumbers = (leftPrimitives & Primitive::Number) && (rightPrimitives & Primitive::Number);
        bool mayBothBeNullish = (leftPrimitives & Primitive::UndefinedOrNull) && (rightPrimitives & Primitive::UndefinedOrNull);
        bool mayBeSameNullish = leftPrimitives & rightPrimitives & Primitive::UndefinedOrNull;
        bool mayBeEqualNullish = isStrict ? mayBeSameNullish : mayBothBeNullish;

        Condition areEqualNumbers;
        if (mayBothBeNumbers)
            areEqualNumbers = equal(numberOrNaN(value, left), numberOrNaN(value, right));
        if (!mayBeEqualNullish)
            return areEqualNumbers ? areEqualNumbers : boolean(false);

        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Condition areEqualNullish;
        if (isStrict) {
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
        switch (representationOf(left)) {
        case Representation::Cell:
            return equal(in<CellPointer>(left), in<CellPointer>(right));
        case Representation::JSValue:
            return equalityOnBoxedBits(value, in<Boxed>(left), in<Boxed>(right), isStrict);
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Int32:
        case Representation::Int64:
        case Representation::Double:
        case Representation::DoubleWithPayload:
        case Representation::ImpureDouble:
        case Representation::OptionalInt64:
        case Representation::OptionalDouble:
        case Representation::Cell32:
        case Representation::LiteralString:
        case Representation::RawWord:
            break;
        }
        return { };
    }

    Condition equalityOnStrings(const ValueContext& value, bool isStrict)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        switch (representationOf(left)) {
        case Representation::Cell:
            // Against a written text the other operand is read in place, as StringEqualsLiteral reads it.
            for (auto [written, other] : { std::pair { right, left }, std::pair { left, right } }) {
                const Value& definition = m_function.at(definitionThroughUpcasts(m_function, written));
                if (definition.opcode == Opcode::String)
                    return emitter<StringEmitter>().equalsLiteral(value, ComparedOperand::String, in<CellPointer>(other).value(), definition.string(), false, true);
            }
            return emitter<StringEmitter>().areEqual(value, in<CellPointer>(left), in<CellPointer>(right));
        case Representation::JSValue:
            return equalityOnNullableStrings(value, isStrict);
        case Representation::LiteralString:
            return equalityOnLiteralStrings(value, isStrict);
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Int32:
        case Representation::Int64:
        case Representation::Double:
        case Representation::DoubleWithPayload:
        case Representation::ImpureDouble:
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
        auto [typed, boxed] = mixedOperandsOf(value);
        Number number = in<Number>(typed);
        Boxed other = in<Boxed>(boxed);
        return logicalAnd(isNumber(other), equal(number, numberToDouble(other)));
    }

    Condition equalityOnInt32AndValue(const ValueContext& value)
    {
        auto [typed, boxed] = mixedOperandsOf(value);
        Word32 expected = in<Word32>(typed);
        Boxed other = in<Boxed>(boxed);
        LBasicBlock hasSameBits = newBlock();
        LBasicBlock hasOtherBits = newBlock();
        LBasicBlock isOtherInt32 = newBlock();
        LBasicBlock isNotInt32 = newBlock();
        Join<Condition> join(m_out);
        branch(equal(other, boxInt32(expected)), { unlikelyEdge(hasSameBits), normalEdge(hasOtherBits) });
        m_out.appendTo(hasSameBits);
        join.add(boolean(true));
        m_out.appendTo(hasOtherBits);
        branch(isInt32(other), { normalEdge(isOtherInt32), rareEdge(isNotInt32) });
        m_out.appendTo(isOtherInt32);
        join.add(boolean(false));
        m_out.appendTo(isNotInt32);
        // Zero equals two doubles, 0 and -0. Every other int32 equals one.
        if (expected.value()->hasInt32() && expected.value()->asInt32())
            join.add(equal(other, boxedConstant(JSValue(JSValue::EncodeAsDouble, static_cast<double>(expected.value()->asInt32())))));
        else
            join.add(logicalAnd(isNumber(other), equal(unboxDouble(other), toNumber(expected))));
        return join.finish();
    }

    Condition equalityOnStringAndValue(const ValueContext& value)
    {
        auto [typed, boxed] = mixedOperandsOf(value);
        // Against a written text the value is read in place, as ValueEqualsLiteral reads it.
        if (const Value& definition = m_function.at(definitionThroughUpcasts(m_function, typed)); definition.opcode == Opcode::String) {
            const Type& type = typeOf(boxed);
            bool everyCellIsString = !type.hasObjects() && !(type.primitives & cellPrimitives & ~Primitive::AnyString);
            return emitter<StringEmitter>().equalsLiteral(value, ComparedOperand::Value, in<Boxed>(boxed).value(), definition.string(), false, everyCellIsString);
        }
        CellPointer string = in<CellPointer>(typed);
        Boxed other = in<Boxed>(boxed);
        LBasicBlock isString = newBlock();
        LBasicBlock isNotString = newBlock();
        Join<Condition> join(m_out);
        branchOnType(other, typeTestPlanFor(m_function.at(boxed).type, Representation::JSValue, TypeTable::string), { normalEdge(isString), rareEdge(isNotString) });
        m_out.appendTo(isString);
        join.add(emitter<StringEmitter>().areEqual(value, string, unboxCell(other)));
        m_out.appendTo(isNotString);
        join.add(boolean(false));
        return join.finish();
    }

    Condition equalityOnValues(const ValueContext& value, bool isStrict)
    {
        Boxed x = in<Boxed>(value->leftOperand());
        Boxed y = in<Boxed>(value->rightOperand());
        if (!isStrict || value.shape() != CodeShape::Fast)
            return callEqualStub(value, isStrict, x, y);
        return value->leftOperand() == value->rightOperand() ? isEqualToItself(x) : areValuesEqual(value, x, y);
    }

private:
    struct MixedOperands {
        ValueID typed;
        ValueID boxed;
    };

    MixedOperands mixedOperandsOf(const ValueContext& value)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        if (representationOf(left) == Representation::JSValue)
            return { right, left };
        return { left, right };
    }

    Number numberOrNaN(const ValueContext& value, ValueID id)
    {
        switch (representationOf(id)) {
        case Representation::Double:
        case Representation::DoubleWithPayload:
        case Representation::ImpureDouble:
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
        case Representation::LiteralString:
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
        Condition isLeftNullish = isUndefinedOrNull(x, typeOf(value->leftOperand()).primitives);
        Condition isRightNullish = isUndefinedOrNull(y, typeOf(value->rightOperand()).primitives);
        return logicalOr(haveSameBits, logicalAnd(isLeftNullish, isRightNullish));
    }

    Condition equalityOnLiteralStrings(const ValueContext& value, bool isStrict)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        LiteralStringWord x = in<LiteralStringWord>(left);
        LiteralStringWord y = in<LiteralStringWord>(right);
        auto equalsLiteral = [&](LiteralStringWord loaded, LiteralStringWord literal) -> std::optional<Condition> {
            if (!literal.value()->hasInt64())
                return std::nullopt;
            uint64_t bits = static_cast<uint64_t>(literal.value()->asInt64());
            std::optional<NarrowReference> narrow = bits >= emptyLiteralStringWord && bits < minCell32 ? narrowWordBehind(loaded) : std::nullopt;
            if (!narrow)
                return std::nullopt;
            return equal(*narrow, NarrowReference(m_out.constInt32(static_cast<int32_t>(bits))));
        };
        if (std::optional<Condition> isEqual = equalsLiteral(x, y))
            return *isEqual;
        if (std::optional<Condition> isEqual = equalsLiteral(y, x))
            return *isEqual;
        Condition haveEqualWords = equalityOnBoxedBits(value, Boxed(x.value()), Boxed(y.value()), isStrict);
        if (!mayBeStringCell(x, typeOf(left)) || !mayBeStringCell(y, typeOf(right)))
            return haveEqualWords;

        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Condition areStringCells = logicalAnd(conversions.isStringCell(x), conversions.isStringCell(y));
        return either<Condition>(logicalOr(haveEqualWords, logicalNot(areStringCells)), [&] {
            return haveEqualWords;
        }, [&] {
            return emitter<StringEmitter>().areEqual(value, CellPointer(x.value()), CellPointer(y.value()), StringEmitter::StringIdentity::Distinct);
        });
    }

    Condition equalityOnNullableStrings(const ValueContext& value, bool isStrict)
    {
        // Against a written text the other operand is read in place, as ValueEqualsLiteral reads it. Neither undefined nor null equals a text, strictly or loosely.
        for (auto [written, other] : { std::pair { value->rightOperand(), value->leftOperand() }, std::pair { value->leftOperand(), value->rightOperand() } }) {
            const Value& definition = m_function.at(definitionThroughUpcasts(m_function, written));
            if (definition.opcode == Opcode::String)
                return emitter<StringEmitter>().equalsLiteral(value, ComparedOperand::Value, in<Boxed>(other).value(), definition.string(), false, true);
        }
        Boxed x = in<Boxed>(value->leftOperand());
        Boxed y = in<Boxed>(value->rightOperand());
        LBasicBlock hasSameBits = newBlock();
        LBasicBlock hasOtherBits = newBlock();
        LBasicBlock areStrings = newBlock();
        LBasicBlock oneIsNullish = newBlock();
        Join<Condition> join(m_out);
        branch(equal(x, y), { normalEdge(hasSameBits), normalEdge(hasOtherBits) });
        m_out.appendTo(hasSameBits);
        join.add(boolean(true));
        m_out.appendTo(hasOtherBits);
        bool leftIsString = typeOf(value->leftOperand()).isString();
        bool rightIsString = typeOf(value->rightOperand()).isString();
        Condition areCells = leftIsString ? (rightIsString ? boolean(true) : isCell(y)) : (rightIsString ? isCell(x) : areBothCells(x, y));
        branch(areCells, { normalEdge(areStrings), normalEdge(oneIsNullish) });
        m_out.appendTo(areStrings);
        join.add(emitter<StringEmitter>().areEqual(value, unboxCell(x), unboxCell(y), StringEmitter::StringIdentity::Distinct));
        m_out.appendTo(oneIsNullish);
        join.add(isStrict ? boolean(false) : logicalAnd(isNotCell(x), isNotCell(y)));
        return join.finish();
    }

    Condition isEqualToItself(Boxed x)
    {
        LBasicBlock isSomeNumber = newBlock();
        LBasicBlock isDouble = newBlock();
        LBasicBlock isSelf = newBlock();
        Join<Condition> join(m_out);
        branch(isNumber(x), { normalEdge(isSomeNumber), normalEdge(isSelf) });
        m_out.appendTo(isSomeNumber);
        branch(isInt32(x), { normalEdge(isSelf), unlikelyEdge(isDouble) });
        m_out.appendTo(isDouble);
        Number number = unboxDouble(x);
        join.add(equal(number, number));
        m_out.appendTo(isSelf);
        join.add(boolean(true));
        return join.finish();
    }

    Condition areValuesEqual(const OperationContext& context, Boxed x, Boxed y)
    {
        LBasicBlock hasSameBits = newBlock();
        LBasicBlock isSameNumber = newBlock();
        LBasicBlock hasOtherBits = newBlock();
        LBasicBlock areCells = newBlock();
        LBasicBlock areNotBothCells = newBlock();
        LBasicBlock firstIsNotObject = newBlock();
        LBasicBlock firstIsString = newBlock();
        LBasicBlock areStrings = newBlock();
        LBasicBlock isSame = newBlock();
        LBasicBlock isOther = newBlock();
        LBasicBlock callsStub = newBlock();
        Join<Condition> join(m_out);
        branch(equal(x, y), { unlikelyEdge(hasSameBits), normalEdge(hasOtherBits) });
        m_out.appendTo(hasSameBits);
        branch(isNumber(x), { normalEdge(isSameNumber), normalEdge(isSame) });
        m_out.appendTo(isSameNumber);
        branch(isInt32(x), { normalEdge(isSame), rareEdge(callsStub) });
        m_out.appendTo(hasOtherBits);
        branch(areBothCells(x, y), { normalEdge(areCells), normalEdge(areNotBothCells) });
        m_out.appendTo(areNotBothCells);
        branch(isInt32(Boxed(m_out.bitAnd(x.value(), y.value()))), { normalEdge(isOther), rareEdge(callsStub) });
        m_out.appendTo(areCells);
        branch(isObjectCell(unboxCell(x)), { normalEdge(isOther), unlikelyEdge(firstIsNotObject) });
        m_out.appendTo(firstIsNotObject);
        branch(hasCellType(unboxCell(x), StringType), { normalEdge(firstIsString), rareEdge(callsStub) });
        m_out.appendTo(firstIsString);
        branch(hasCellType(unboxCell(y), StringType), { normalEdge(areStrings), normalEdge(isOther) });
        m_out.appendTo(areStrings);
        join.add(emitter<StringEmitter>().areEqual(context, unboxCell(x), unboxCell(y), StringEmitter::StringIdentity::Distinct));
        m_out.appendTo(isSame);
        join.add(boolean(true));
        m_out.appendTo(isOther);
        join.add(boolean(false));
        m_out.appendTo(callsStub);
        join.add(callEqualStub(context, true, x, y, { .isOnRarePath = true }));
        return join.finish();
    }

    Condition areBothCells(Boxed x, Boxed y) { return isCell(Boxed(m_out.bitOr(x.value(), y.value()))); }
};

} // anonymous namespace

void OperatorEmitter::generateEquality(const ValueContext& value, bool isStrict)
{
    EqualityEmitter equality(generator());
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    Condition result;
    switch (value->form()) {
    case OperatorForm::Generic:
        result = equality.equalityOnValues(value, isStrict);
        break;
    case OperatorForm::Integers:
        result = equality.equalityOnIntegers(value);
        break;
    case OperatorForm::Numbers:
        if (isEqualityOfNumbers(m_program, m_function, value.value())) {
            generateComparisonOfNumbers(value, std::nullopt);
            return;
        }
        result = equality.equalityOnNumbers(value, isStrict);
        break;
    case OperatorForm::Bits:
        result = equality.equalityOnBits(value, isStrict);
        break;
    case OperatorForm::Booleans:
        result = equal(in<Condition>(left), in<Condition>(right));
        break;
    case OperatorForm::BigInts:
        result = equality.callEqualStub(value, isStrict, boxCell(in<CellPointer>(left)), boxCell(in<CellPointer>(right)));
        break;
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
    if (!result) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        result = boolean(false);
    }
    defineTest(value, result);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
