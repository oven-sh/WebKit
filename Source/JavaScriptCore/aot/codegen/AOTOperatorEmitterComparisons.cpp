/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RelationTraits {
    BinaryOperator stub;
    RuntimeOperation operation;
    Relation mirrored;
};

} // anonymous namespace

static RelationTraits traitsOfOperator(Relation relation)
{
    switch (relation) {
    case Relation::Less:
        return { BinaryOperator::Less, RuntimeOperation::CompareLess, Relation::Greater };
    case Relation::LessOrEqual:
        return { BinaryOperator::LessEq, RuntimeOperation::CompareLessEq, Relation::GreaterOrEqual };
    case Relation::Greater:
        return { BinaryOperator::Greater, RuntimeOperation::CompareGreater, Relation::Less };
    case Relation::GreaterOrEqual:
        return { BinaryOperator::GreaterEq, RuntimeOperation::CompareGreaterEq, Relation::LessOrEqual };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
static Condition withIntegerOperands(Operand left, Operand right, const Functor& functor)
{
    switch (left.representation) {
    case Representation::Int32:
        return functor(left.as<Word32>(), right.as<Word32>());
    case Representation::Int64:
        return functor(left.as<Integer>(), right.as<Integer>());
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

// An element access tests Below(index, length), so B3 merges its test with a loop condition of this form.
static Condition compareNonNegative(Output& out, Relation relation, LValue left, LValue right)
{
    switch (relation) {
    case Relation::Less:
        return Condition(out.below(left, right));
    case Relation::LessOrEqual:
        return Condition(out.belowOrEqual(left, right));
    case Relation::Greater:
        return Condition(out.below(right, left));
    case Relation::GreaterOrEqual:
        return Condition(out.belowOrEqual(right, left));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename T>
Condition OperatorEmitter::compare(std::optional<Relation> relation, T left, T right)
{
    if (!relation)
        return equal(left, right);
    switch (*relation) {
    case Relation::Less:
        return lessThan(left, right);
    case Relation::LessOrEqual:
        return lessThanOrEqual(left, right);
    case Relation::Greater:
        return greaterThan(left, right);
    case Relation::GreaterOrEqual:
        return greaterThanOrEqual(left, right);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void OperatorEmitter::generateComparisonOfNumbers(const ValueContext& value, std::optional<Relation> relation)
{
    if (!relation && (tryGenerateTestOfRemainder(value, value->leftOperand()) || tryGenerateTestOfRemainder(value, value->rightOperand())))
        return;
    if (tryGenerateComparisonOfWholeRemainder(value, relation))
        return;

    Operand left = get(value->leftOperand());
    Operand right = get(value->rightOperand());
    const Type& leftType = typeOf(value->leftOperand());
    const Type& rightType = typeOf(value->rightOperand());
    auto mayBeInt32 = [](Operand operand, const Type& type) {
        return operand.is<Word32>() || (operand.is<Boxed>() && type.primitives & Primitive::Integer);
    };
    auto int32Of = [&](Operand operand) {
        return operand.is<Boxed>() ? low32(operand.as<Boxed>()) : operand.as<Word32>();
    };

    if (left.is<Word32>() && right.is<Word32>()) {
        defineTest(value, compare(relation, left.as<Word32>(), right.as<Word32>()));
        return;
    }
    if (!mayBeInt32(left, leftType) || !mayBeInt32(right, rightType)) {
        Number leftNumber = asNumber(left, leftType);
        defineTest(value, compare(relation, leftNumber, asNumber(right, rightType)));
        return;
    }

    defineTestByBranching(value, [&](BranchTargets targets) {
        Boxed tested = left.is<Boxed>() ? left.as<Boxed>() : right.as<Boxed>();
        if (left.is<Boxed>() && right.is<Boxed>())
            tested = Boxed(m_out.bitAnd(left.value, right.value));
        LBasicBlock int32Case = newBlock();
        LBasicBlock numberCase = newBlock();
        m_out.branch(isInt32(tested).value(), normalEdge(int32Case), unlikelyEdge(numberCase));
        m_out.appendTo(int32Case);
        Word32 leftInt32 = int32Of(left);
        branch(compare(relation, leftInt32, int32Of(right)), targets);
        m_out.appendTo(numberCase);
        Number leftNumber = asNumber(left, leftType);
        branch(compare(relation, leftNumber, asNumber(right, rightType)), targets);
    });
}

void OperatorEmitter::generateRelation(const ValueContext& value, Relation relation)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    Condition result;
    switch (value->form()) {
    case OperatorForm::Generic:
    case OperatorForm::NumberAndValue:
        result = relationOnValues(value, relation);
        break;
    case OperatorForm::Integers: {
        bool areBothNonNegative = typeOf(left).integers.minimum >= 0 && typeOf(right).integers.minimum >= 0;
        result = withIntegerOperands(get(left), get(right), [&](auto x, auto y) {
            return areBothNonNegative ? compareNonNegative(m_out, relation, x.value(), y.value()) : compare(relation, x, y);
        });
        break;
    }
    case OperatorForm::Numbers:
        generateComparisonOfNumbers(value, relation);
        return;
    case OperatorForm::BigInts:
        result = callOperation<Condition>(value, traitsOfOperator(relation).operation, { boxCell(in<CellPointer>(left)), boxCell(in<CellPointer>(right)) });
        break;
    case OperatorForm::Strings: {
        CellPointer leftString = in<CellPointer>(left);
        Word32 order = emitter<StringEmitter>().order(value, leftString, in<CellPointer>(right));
        result = compare(relation, order, word32(0));
        break;
    }
    case OperatorForm::NullableStrings:
        result = relationOnNullableStrings(value, relation);
        break;
    case OperatorForm::Unselected:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
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

bool OperatorEmitter::tryGenerateComparisonOfWholeRemainder(const ValueContext& value, std::optional<Relation> relation)
{
    const WholeRemainder* left = findWholeRemainder(value->leftOperand());
    const WholeRemainder* right = findWholeRemainder(value->rightOperand());
    if (!left && !right)
        return false;
    Operand leftOperand = left ? left->whole : get(value->leftOperand());
    Operand rightOperand = right ? right->whole : get(value->rightOperand());
    auto isInteger = [](Operand operand) {
        return operand.is<Word32>() || operand.is<Integer>();
    };
    if (!isInteger(leftOperand) || !isInteger(rightOperand))
        return false;

    auto compareBitsOfDividend = [&](const WholeRemainder* remainder, Operand& operand, Operand& other) {
        if (relation || !remainder || !other.value->hasInt())
            return;
        int64_t constant = other.value->asInt();
        if (!constant) {
            operand = remainder->zeroWhereWholeIsZero;
            return;
        }
        uint64_t divisor = remainder->powerOfTwoDivisor;
        if (!divisor || static_cast<uint64_t>(constant < 0 ? -constant : constant) >= divisor)
            return;
        if (remainder->dividend.is<Word32>()) {
            uint32_t sign = 1u << 31;
            uint32_t lowBits = static_cast<uint32_t>(divisor - 1);
            operand = bitAnd(remainder->dividend.as<Word32>(), word32(static_cast<int32_t>(sign | lowBits)));
            other = word32(static_cast<int32_t>((constant < 0 ? sign : 0) | (static_cast<uint32_t>(constant) & lowBits)));
            return;
        }
        uint64_t sign = 1ull << 63;
        uint64_t lowBits = divisor - 1;
        operand = bitAnd(remainder->dividend.as<Integer>(), integer(static_cast<int64_t>(sign | lowBits)));
        other = integer(static_cast<int64_t>((constant < 0 ? sign : 0) | (static_cast<uint64_t>(constant) & lowBits)));
    };
    compareBitsOfDividend(left, leftOperand, rightOperand);
    compareBitsOfDividend(right, rightOperand, leftOperand);

    auto widened = [&](Operand operand) {
        return operand.is<Word32>() ? signExtended(operand.as<Word32>()) : operand.as<Integer>();
    };
    Vector<Condition, 3> conditions;
    if (leftOperand.is<Word32>() && rightOperand.is<Word32>())
        conditions.append(compare(relation, leftOperand.as<Word32>(), rightOperand.as<Word32>()));
    else {
        Integer wideLeft = widened(leftOperand);
        conditions.append(compare(relation, wideLeft, widened(rightOperand)));
    }
    for (const WholeRemainder* remainder : { left, right }) {
        if (remainder && remainder->isNumber)
            conditions.append(remainder->isNumber);
    }
    if (conditions.size() == 1) {
        defineTest(value, conditions[0]);
        return true;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        for (Condition condition : conditions.span().first(conditions.size() - 1)) {
            LBasicBlock next = newBlock();
            branch(condition, BranchTargets { normalEdge(next), targets.notTaken });
            m_out.appendTo(next);
        }
        branch(conditions.last(), targets);
    });
    return true;
}

// The order of a text and a literal of one character is read off the first character and the length.
Condition OperatorEmitter::relationOnStringCells(const ValueContext& value, Relation relation, CellPointer left, CellPointer right)
{
    StringEmitter& strings = emitter<StringEmitter>();
    auto characterOf = [&](ValueID operand) -> std::optional<char16_t> {
        const Value& definition = m_function.at(definitionThroughUpcasts(m_function, operand));
        if (definition.opcode != Opcode::String)
            return std::nullopt;
        String text = strings.textOf(definition.string());
        return text.length() == 1 ? std::optional(text[0]) : std::nullopt;
    };
    if (std::optional<char16_t> character = characterOf(value->rightOperand())) {
        Word32 order = strings.orderToCharacter(strings.viewOfString(value, left), *character);
        return compare(relation, order, word32(0));
    }
    if (std::optional<char16_t> character = characterOf(value->leftOperand())) {
        Word32 order = strings.orderToCharacter(strings.viewOfString(value, right), *character);
        return compare(traitsOfOperator(relation).mirrored, order, word32(0));
    }
    Word32 order = strings.order(value, left, right);
    return compare(relation, order, word32(0));
}

Condition OperatorEmitter::relationOnNullableStrings(const ValueContext& value, Relation relation)
{
    Boxed left = in<Boxed>(value->leftOperand());
    Boxed right = in<Boxed>(value->rightOperand());
    LBasicBlock areStrings = newBlock();
    LBasicBlock oneIsUndefined = newBlock();
    Join<Condition> join(m_out);
    branch(isCell(Boxed(m_out.bitOr(left.value(), right.value()))), { normalEdge(areStrings), unlikelyEdge(oneIsUndefined) });
    m_out.appendTo(areStrings);
    join.add(relationOnStringCells(value, relation, unboxCell(left), unboxCell(right)));
    m_out.appendTo(oneIsUndefined);
    join.add(boolean(false));
    return join.finish();
}

Condition OperatorEmitter::relationOnValues(const ValueContext& value, Relation relation)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    if (value->form() == OperatorForm::Generic) {
        Boxed x = in<Boxed>(left);
        Boxed y = in<Boxed>(right);
        return Condition(m_out.castToInt32(callBinaryStub(value, traitsOfOperator(relation).stub, x, y)));
    }

    bool numberIsOnRight = representationOf(left) != Representation::Double;
    ValueID otherID = numberIsOnRight ? left : right;
    Number numberOperand = in<Number>(numberIsOnRight ? right : left);
    Boxed otherOperand = in<Boxed>(otherID);
    auto compareByOperation = [&](const CallOptions& options) {
        Boxed boxedNumber = emitter<ConversionEmitter>().boxDouble(numberOperand);
        Boxed x = numberIsOnRight ? otherOperand : boxedNumber;
        Boxed y = numberIsOnRight ? boxedNumber : otherOperand;
        return callOperation<Condition>(value, traitsOfOperator(relation).operation, { x, y }, options);
    };
    if (!(typeOf(otherID).primitives & Primitive::Number))
        return compareByOperation(CallOptions { });
    return either<Condition>(isNumber(otherOperand), [&] {
        Number otherNumber = numberToDouble(otherOperand);
        return numberIsOnRight ? compare(relation, otherNumber, numberOperand) : compare(relation, numberOperand, otherNumber);
    }, [&] {
        return compareByOperation(CallOptions { .isOnRarePath = true });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
