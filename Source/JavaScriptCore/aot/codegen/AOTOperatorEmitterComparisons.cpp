/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTThrowEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RelationTraits {
    BinaryOperator stub;
    RuntimeOperation operation;
};

} // anonymous namespace

static RelationTraits traitsOfRelation(Relation relation)
{
    switch (relation) {
    case Relation::Less:
        return { BinaryOperator::Less, RuntimeOperation::CompareLess };
    case Relation::LessOrEqual:
        return { BinaryOperator::LessEq, RuntimeOperation::CompareLessEq };
    case Relation::Greater:
        return { BinaryOperator::Greater, RuntimeOperation::CompareGreater };
    case Relation::GreaterOrEqual:
        return { BinaryOperator::GreaterEq, RuntimeOperation::CompareGreaterEq };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Representation widthOfComparedIntegers(const CodeGenerator& generator, const Operation& comparison)
{
    bool fitInt32 = true;
    for (ValueID operand : comparison.operands()) {
        if (!constantsInFormOfUses.contains(generator.opcodeOf(operand)))
            return generator.representationOf(operand);
        fitInt32 = fitInt32 && generator.typeOf(operand).isInt32();
    }
    return fitInt32 ? Representation::Int32 : Representation::Int64;
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

Condition OperatorEmitter::relationOnBigInts(const ValueContext& value, Relation relation, CellPointer left, CellPointer right)
{
    auto callRelation = [&] {
        return asBoolean([&](BranchTargets targets) {
            branchOnRelationByStub(value, relation, boxCell(left), boxCell(right), targets);
        });
    };
    Word32 leftLength = loadField<Word32>(left, m_heaps.JSBigInt_length);
    Word32 rightLength = loadField<Word32>(right, m_heaps.JSBigInt_length);
    Word32 signs = bitAnd(bitOr(loadByte(left, m_heaps.JSBigInt_sign), loadByte(right, m_heaps.JSBigInt_sign)), TypeInfoPerCellBit);
    return either<Condition>(belowOrEqual(bitOr(bitOr(leftLength, rightLength), signs), 1), [&] {
        return selectByBranch<Condition>(equal(leftLength, rightLength), [&] {
            return selectByBranch<Condition>(isZero(leftLength), [&] {
                return boolean(relation == Relation::LessOrEqual || relation == Relation::GreaterOrEqual);
            }, [&] {
                return compareNonNegative(m_out, relation, loadField<Integer>(left, m_heaps.JSBigInt_data).value(), loadField<Integer>(right, m_heaps.JSBigInt_data).value());
            });
        }, [&] {
            return compareNonNegative(m_out, relation, leftLength.value(), rightLength.value());
        });
    }, callRelation);
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
    Operand left = generator().getWithAnyNaN(value->leftOperand());
    Operand right = generator().getWithAnyNaN(value->rightOperand());
    const Type& leftType = typeOf(value->leftOperand());
    const Type& rightType = typeOf(value->rightOperand());
    auto mayBeInt32 = [](Operand operand, const Type& type) {
        return operand.is<Word32>() || (operand.is<Boxed>() && type.primitives & Primitive::Integer);
    };
    auto int32Of = [&](Operand operand) {
        return operand.is<Boxed>() ? int32InBox(operand.as<Boxed>()) : operand.as<Word32>();
    };

    if (left.is<Word32>() && right.is<Word32>()) {
        defineTest(value, compare(relation, left.as<Word32>(), right.as<Word32>()));
        return;
    }
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    std::optional<ConversionEmitter::ComparedInteger> leftInteger = relation ? conversions.findComparedInteger(left) : std::nullopt;
    std::optional<ConversionEmitter::ComparedInteger> rightInteger = leftInteger ? conversions.findComparedInteger(right) : std::nullopt;
    if (rightInteger && (leftInteger->mayBeMarker || rightInteger->mayBeMarker) && ConversionEmitter::areComparedAsIntegers(*leftInteger, *rightInteger)) {
        bool leftIsGreater = *relation == Relation::Greater || *relation == Relation::GreaterOrEqual;
        const ConversionEmitter::ComparedInteger& greater = leftIsGreater ? *leftInteger : *rightInteger;
        Condition holds = compare(relation, leftInteger->value, rightInteger->value);
        if (emittersCheckProofs()) {
            Condition holdsOfIntegers = greater.mayBeMarker ? logicalAnd(holds, logicalNot(conversions.isMarker(Operand(Representation::OptionalInt64, greater.value.value())))) : holds;
            emitter<ThrowEmitter>().requireOrCrash(value, equal(holdsOfIntegers, compare(relation, Number(left.value), Number(right.value))), std::nullopt, ProofSource::OtherOperation);
        }
        if (!greater.mayBeMarker) {
            defineTest(value, holds);
            return;
        }
        defineTestByBranching(value, [&](BranchTargets targets) {
            LBasicBlock integersHold = newBlock();
            branch(holds, { normalEdge(integersHold), targets.notTaken });
            m_out.appendTo(integersHold);
            branch(conversions.isMarker(Operand(Representation::OptionalInt64, greater.value.value())), { targets.notTaken, targets.taken });
        });
        return;
    }
    if (left.is<Integer>() || right.is<Integer>()) {
        bool integerIsOnRight = right.is<Integer>();
        Integer integer = (integerIsOnRight ? right : left).as<Integer>();
        Boxed boxed = (integerIsOnRight ? left : right).as<Boxed>();
        build().statistics().count(StatisticID::Int64ComparisonsWithBoxedNumbers);
        defineTestByBranching(value, [&](BranchTargets targets) {
            LBasicBlock int32Case = newBlock();
            LBasicBlock numberCase = newBlock();
            m_out.branch(isInt32(boxed).value(), normalEdge(int32Case), unlikelyEdge(numberCase));
            m_out.appendTo(int32Case);
            Integer wide = signExtended(int32InBox(boxed));
            branch(compare(relation, integerIsOnRight ? wide : integer, integerIsOnRight ? integer : wide), targets);
            m_out.appendTo(numberCase);
            Number unboxed = asNumber(boxed, integerIsOnRight ? leftType : rightType, PayloadOfNaN::Stays);
            Number converted = toNumber(integer);
            branch(compare(relation, integerIsOnRight ? unboxed : converted, integerIsOnRight ? converted : unboxed), targets);
        });
        return;
    }
    if (!mayBeInt32(left, leftType) || !mayBeInt32(right, rightType)) {
        Number leftNumber = asNumber(left, leftType, PayloadOfNaN::Stays);
        defineTest(value, compare(relation, leftNumber, asNumber(right, rightType, PayloadOfNaN::Stays)));
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
        Number leftNumber = asNumber(left, leftType, PayloadOfNaN::Stays);
        branch(compare(relation, leftNumber, asNumber(right, rightType, PayloadOfNaN::Stays)), targets);
    });
}

void OperatorEmitter::generateRelation(const ValueContext& value, Relation relation)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    Condition result;
    switch (value->form()) {
    case OperatorForm::NumberAndValue:
        if (generateRelationOfIntegerAndValue(value, relation) || generateRelationOfBoxedNumberAndValue(value, relation) || generateMixedDoubleRelation(value, relation))
            return;
        [[fallthrough]];
    case OperatorForm::Generic:
        if (value->form() == OperatorForm::Generic && (generateRelationWithLikelyZero(value, relation) || generateRelationOfValuesThatAreLikelyInt32s(value, relation)))
            return;
        result = relationOnValues(value, relation);
        break;
    case OperatorForm::Integers: {
        m_out.noteForm(Form::Open, StateOfKnowledge::Integers);
        bool areBothNonNegative = typeOf(left).integers.minimum >= 0 && typeOf(right).integers.minimum >= 0;
        if (relation == Relation::Less && AOT::hasFlag(build().options().testing, Wire::TestingSwitch::TreatEqualIntegersAsLess)) [[unlikely]]
            relation = Relation::LessOrEqual;
        Representation width = widthOfComparedIntegers(m_generator, value.value());
        Operand leftInteger = m_generator.getIn(left, width);
        Operand rightInteger = m_generator.getIn(right, width);
        result = withIntegerOperands(leftInteger, rightInteger, [&](auto x, auto y) {
            if (areBothNonNegative && emittersCheckProofs())
                emitter<ThrowEmitter>().requireOrCrash(value, greaterThanOrEqual(bitOr(x, y), 0), std::nullopt, ProofSource::OtherOperation);
            return areBothNonNegative ? compareNonNegative(m_out, relation, x.value(), y.value()) : compare(relation, x, y);
        });
        break;
    }
    case OperatorForm::Numbers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        generateComparisonOfNumbers(value, relation);
        return;
    case OperatorForm::BigInts: {
        m_out.noteForm(Form::Closed, StateOfKnowledge::BigInts);
        CellPointer leftBigInt = in<CellPointer>(left);
        CellPointer rightBigInt = in<CellPointer>(right);
        if (!generator().runsRepeatedly(value.block())) {
            defineTestByBranching(value, [&](BranchTargets targets) {
                branchOnRelationByStub(value, relation, boxCell(leftBigInt), boxCell(rightBigInt), targets);
            });
            return;
        }
        result = relationOnBigInts(value, relation, leftBigInt, rightBigInt);
        break;
    }
    case OperatorForm::Strings: {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Strings);
        CellPointer leftString = in<CellPointer>(left);
        result = relationOnStringCells(value, relation, leftString, in<CellPointer>(right));
        break;
    }
    case OperatorForm::NullableStrings:
        m_out.noteForm(Form::Closed, StateOfKnowledge::NullableStrings);
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

void OperatorEmitter::generateIntegerBound(const ValueContext& value)
{
    Opcode comparison = value->comparison();
    bool admitsEquality = comparison == Opcode::LessEq || comparison == Opcode::GreaterEq;
    bool asksForGreaterOnLeft = comparison == Opcode::Greater || comparison == Opcode::GreaterEq;
    bool isRelation = admitsEquality || asksForGreaterOnLeft || comparison == Opcode::Less;
    if (!isRelation || !integerRepresentations.contains(value.representation())) {
        define(value, failWithZero(value));
        return;
    }

    bool asksForGreaterInteger = asksForGreaterOnLeft != value->numberIsOnLeft();
    bool roundsUp = asksForGreaterInteger == admitsEquality;
    static_assert(static_cast<double>(maxTrackedInteger + 1) == 0x1p62);

    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Number number = in<Number>(value->numberOperand());
    Integer rounded = conversions.toInteger(roundsUp ? ceil(number) : floor(number));
    if (value.representation() == Representation::Int32) {
        define(value, conversions.narrow(rounded));
        return;
    }
    Condition isInt32 = equal(signExtended(low32(rounded)), rounded);
    if (!isX86_64())
        isInt32 = Condition(m_out.bitAnd(isInt32.value(), equal(number, number).value()));
    define(value, either<Integer>(isInt32, [&] {
        return rounded;
    }, [&] {
        return callLeaf<Integer>(RuntimeOperation::RoundToIntegerBound, { number, word32(roundsUp | asksForGreaterInteger << 1) });
    }));
}

Condition OperatorEmitter::relationOnStringCells(const ValueContext& value, Relation relation, CellPointer left, CellPointer right)
{
    Word32 order = emitter<StringEmitter>().order(value, left, right);
    return compare(relation, order, word32(0));
}

Condition OperatorEmitter::relationOnNullableStrings(const ValueContext& value, Relation relation)
{
    Boxed left = in<Boxed>(value->leftOperand());
    Boxed right = in<Boxed>(value->rightOperand());
    LBasicBlock areStrings = newBlock();
    LBasicBlock oneIsUndefined = newBlock();
    Join<Condition> join(m_out);
    Condition areCells = isCell(Boxed(m_out.bitOr(left.value(), right.value())));
    ScopeFork fork = m_out.forkScope();
    branch(areCells, { normalEdge(areStrings), unlikelyEdge(oneIsUndefined) });
    m_out.appendToArm(areStrings, fork);
    join.add(relationOnStringCells(value, relation, unboxCell(left), unboxCell(right)));
    m_out.appendToArm(oneIsUndefined, fork);
    join.add(boolean(false));
    return join.finishBehind(fork);
}

bool OperatorEmitter::generateRelationOfIntegerAndValue(const ValueContext& value, Relation relation)
{
    bool numberIsOnRight = !typeOf(value->leftOperand()).isNumber();
    ValueID number = numberIsOnRight ? value->rightOperand() : value->leftOperand();
    ValueID otherID = numberIsOnRight ? value->leftOperand() : value->rightOperand();
    Representation width = representationOf(number);
    if (!integerRepresentations.contains(width))
        return false;

    m_out.noteForm(Form::Open, StateOfKnowledge::NumberAndValue);
    Operand integer = m_generator.getIn(number, width);
    Boxed other = in<Boxed>(otherID);
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock int32Case = newBlock();
        LBasicBlock otherCase = newBlock();
        bool asksStub = width == Representation::Int32;
        m_out.branch(isInt32(other).value(), normalEdge(int32Case), asksStub ? rareEdge(otherCase) : unlikelyEdge(otherCase));
        m_out.appendTo(int32Case);
        Word32 otherInt32 = int32InBox(other);
        Operand otherInteger = width == Representation::Int32 ? Operand(otherInt32) : Operand(signExtended(otherInt32));
        branch(withIntegerOperands(numberIsOnRight ? otherInteger : integer, numberIsOnRight ? integer : otherInteger, [&](auto x, auto y) {
            return compare(relation, x, y);
        }), targets);
        m_out.appendTo(otherCase);
        if (asksStub) {
            Boxed boxedInteger = boxInt32(integer.as<Word32>());
            Boxed x = numberIsOnRight ? other : boxedInteger;
            Boxed y = numberIsOnRight ? boxedInteger : other;
            branchOnRelationByStub(value, relation, x, y, targets);
            return;
        }
        branch(relationOnValues(value, relation, ValueInRelation::IsNoInt32), targets);
    });
    return true;
}

bool OperatorEmitter::generateRelationOfBoxedNumberAndValue(const ValueContext& value, Relation relation)
{
    bool numberIsOnRight = !typeOf(value->leftOperand()).isNumber();
    if (representationOf(numberIsOnRight ? value->rightOperand() : value->leftOperand()) != Representation::JSValue)
        return false;

    m_out.noteForm(Form::Open, StateOfKnowledge::NumberAndValue);
    Boxed left = in<Boxed>(value->leftOperand());
    Boxed right = in<Boxed>(value->rightOperand());
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock int32Case = newBlock();
        LBasicBlock otherCase = newBlock();
        m_out.branch(isInt32(Boxed(m_out.bitAnd(left.value(), right.value()))).value(), normalEdge(int32Case), rareEdge(otherCase));
        m_out.appendTo(int32Case);
        Word32 leftInt32 = int32InBox(left);
        branch(compare(relation, leftInt32, int32InBox(right)), targets);
        m_out.appendTo(otherCase);
        branchOnRelationByStub(value, relation, left, right, targets);
    });
    return true;
}

bool OperatorEmitter::generateMixedDoubleRelation(const ValueContext& value, Relation relation)
{
    bool numberIsOnRight = !typeOf(value->leftOperand()).isNumber();
    ValueID numberID = numberIsOnRight ? value->rightOperand() : value->leftOperand();
    ValueID otherID = numberIsOnRight ? value->leftOperand() : value->rightOperand();
    if (representationOf(numberID) != Representation::Double || !(typeOf(otherID).primitives & Primitive::Number) || generator().code().at(value.block()).frequency == BlockFrequency::Rare)
        return false;
    m_out.noteForm(Form::Open, StateOfKnowledge::NumberAndValue);
    Number number = in<Number>(numberID);
    Boxed other = in<Boxed>(otherID);
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock int32Case = newBlock();
        LBasicBlock otherCase = newBlock();
        bool isComparedWithFraction = typeOf(numberID).primitives & Primitive::FractionalNumber;
        m_out.branch(isInt32(other).value(), normalEdge(int32Case), isComparedWithFraction ? normalEdge(otherCase) : unlikelyEdge(otherCase));
        m_out.appendTo(int32Case);
        Number otherNumber = toNumber(int32InBox(other));
        branch(numberIsOnRight ? compare(relation, otherNumber, number) : compare(relation, number, otherNumber), targets);
        m_out.appendTo(otherCase);
        branch(relationOnValues(value, relation, ValueInRelation::IsNoInt32), targets);
    });
    return true;
}

bool OperatorEmitter::generateRelationWithLikelyZero(const ValueContext& value, Relation relation)
{
    ValueID leftID = value->leftOperand();
    if (!isBigInt(value->rightOperand()) || isBigInt(leftID) || !expectsBigInts(value.block(), leftID))
        return false;
    m_out.noteForm(Form::Open, StateOfKnowledge::BigInts);
    Boxed left = in<Boxed>(leftID);
    Boxed right = boxedOperand(value->rightOperand());
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock isLeftCell = newBlock();
        LBasicBlock areBigInts = newBlock();
        LBasicBlock isRightZero = newBlock();
        LBasicBlock otherCase = newBlock();
        branch(isCell(left), { normalEdge(isLeftCell), rareEdge(otherCase) });
        m_out.appendTo(isLeftCell);
        CellPointer leftCell = unboxCell(left);
        branch(equal(loadByte(leftCell, m_heaps.JSCell_typeInfoType), static_cast<int32_t>(HeapBigIntType)), { normalEdge(areBigInts), rareEdge(otherCase) });
        m_out.appendTo(areBigInts);
        branch(isZero(loadField<Word32>(unboxCell(right), m_heaps.JSBigInt_length)), { normalEdge(isRightZero), rareEdge(otherCase) });
        m_out.appendTo(isRightZero);
        Condition isNegative = notEqual(bitAnd(loadByte(leftCell, m_heaps.JSBigInt_sign), TypeInfoPerCellBit), 0);
        switch (relation) {
        case Relation::Less:
            branch(isNegative, targets);
            break;
        case Relation::GreaterOrEqual:
            branch(isNegative, targets.swapped());
            break;
        case Relation::LessOrEqual:
        case Relation::Greater: {
            bool isGreater = relation == Relation::Greater;
            LBasicBlock isNotNegative = newBlock();
            branch(isNegative, { isGreater ? targets.notTaken : targets.taken, normalEdge(isNotNegative) });
            m_out.appendTo(isNotNegative);
            branch(isZero(loadField<Word32>(leftCell, m_heaps.JSBigInt_length)), isGreater ? targets.swapped() : targets);
            break;
        }
        }
        m_out.appendTo(otherCase);
        branchOnRelationByStub(value, relation, left, right, targets);
    });
    return true;
}

bool OperatorEmitter::generateRelationOfValuesThatAreLikelyInt32s(const ValueContext& value, Relation relation)
{
    if (generator().code().at(value.block()).frequency == BlockFrequency::Rare)
        return false;
    m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
    Boxed left = in<Boxed>(value->leftOperand());
    Boxed right = in<Boxed>(value->rightOperand());
    if (!mayBothBeInt32(value)) {
        defineTestByBranching(value, [&](BranchTargets targets) {
            branchOnRelationByStub(value, relation, left, right, targets);
        });
        return true;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock int32Case = newBlock();
        LBasicBlock otherCase = newBlock();
        m_out.branch(isInt32(Boxed(m_out.bitAnd(left.value(), right.value()))).value(), normalEdge(int32Case), rareEdge(otherCase));
        m_out.appendTo(int32Case);
        Word32 leftInt32 = int32InBox(left);
        branch(compare(relation, leftInt32, int32InBox(right)), targets);
        m_out.appendTo(otherCase);
        branchOnRelationByStub(value, relation, left, right, targets);
    });
    return true;
}

void OperatorEmitter::branchOnRelationByStub(const ValueContext& value, Relation relation, Boxed left, Boxed right, BranchTargets targets)
{
    LValue operands[] = { left.value(), right.value() };
    emitter<RuntimeCallEmitter>().callStubAndBranch(value, binaryOperation(traitsOfRelation(relation).stub, true), operands, { }, targets);
}

Condition OperatorEmitter::relationOnValues(const ValueContext& value, Relation relation, ValueInRelation valueInRelation)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    if (value->form() == OperatorForm::Generic) {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
        Boxed x = in<Boxed>(left);
        Boxed y = in<Boxed>(right);
        if (mayBothBeInt32(value))
            return Condition(m_out.castToInt32(callBinaryStub(value, traitsOfRelation(relation).stub, x, y)));
        return asBoolean([&](BranchTargets targets) {
            branchOnRelationByStub(value, relation, x, y, targets);
        });
    }

    bool numberIsOnRight = !typeOf(left).isNumber();
    ValueID otherID = numberIsOnRight ? left : right;
    ValueID numberID = numberIsOnRight ? right : left;
    Number numberOperand;
    if (representationOf(numberID) == Representation::Int32)
        numberOperand = toNumber(in<Word32>(numberID));
    else if (representationOf(numberID) == Representation::Int64)
        numberOperand = emitter<ConversionEmitter>().toNumber(in<Integer>(numberID), typeOf(numberID).integers);
    else
        numberOperand = in<Number>(numberID);
    Boxed otherOperand = in<Boxed>(otherID);
    auto compareByStub = [&] {
        Boxed boxedNumber = emitter<ConversionEmitter>().boxDouble(numberOperand);
        Boxed x = numberIsOnRight ? otherOperand : boxedNumber;
        Boxed y = numberIsOnRight ? boxedNumber : otherOperand;
        return asBoolean([&](BranchTargets targets) {
            branchOnRelationByStub(value, relation, x, y, targets);
        });
    };
    if (!(typeOf(otherID).primitives & Primitive::Number)) {
        m_out.noteForm(Form::Closed, StateOfKnowledge::NumberAndOtherValue);
        return compareByStub();
    }
    m_out.noteForm(Form::Open, StateOfKnowledge::NumberAndValue);
    return either<Condition>(isNumber(otherOperand), [&] {
        Number otherNumber = valueInRelation == ValueInRelation::IsNoInt32 ? unboxDouble(otherOperand) : numberToDouble(otherOperand);
        return numberIsOnRight ? compare(relation, otherNumber, numberOperand) : compare(relation, numberOperand, otherNumber);
    }, [&] {
        return compareByStub();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
