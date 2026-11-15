/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTBigIntPlace.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTIRProgram.h"
#include "AOTNumbering.h"
#include "AOTStringEmitter.h"
#include "AOTStubsPrimitiveFastPaths.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsValues.h"
#include "AOTTypeTable.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestPlan.h"
#include "PureNaN.h"
#include "TypeofType.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ArithmeticOperatorTraits {
    BinaryOperator stub;
    bool hasStubWithDouble;
    RuntimeOperation onBigInts;
};

struct UnaryOperatorTraits {
    UnaryOperationForm stub;
    RuntimeOperation onBigInt;
};

} // anonymous namespace

static ArithmeticOperatorTraits traitsOfArithmeticOperator(ArithmeticOperator op)
{
    switch (op) {
    case ArithmeticOperator::Add:
        return { BinaryOperator::Add, true, RuntimeOperation::ValueAdd };
    case ArithmeticOperator::Sub:
        return { BinaryOperator::Sub, true, RuntimeOperation::ValueSub };
    case ArithmeticOperator::Mul:
        return { BinaryOperator::Mul, true, RuntimeOperation::ValueMul };
    case ArithmeticOperator::Div:
        return { BinaryOperator::Div, true, RuntimeOperation::ValueDiv };
    case ArithmeticOperator::Mod:
        return { BinaryOperator::Mod, false, RuntimeOperation::ValueMod };
    case ArithmeticOperator::Pow:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static UnaryOperatorTraits traitsOfUnaryOperator(UnaryOperator op)
{
    switch (op) {
    case UnaryOperator::Negate:
        return { UnaryOperationForm::Negate, RuntimeOperation::ValueNegate };
    case UnaryOperator::Increment:
        return { UnaryOperationForm::Increment, RuntimeOperation::Increment };
    case UnaryOperator::Decrement:
        return { UnaryOperationForm::Decrement, RuntimeOperation::Decrement };
    case UnaryOperator::BitNot:
        return { UnaryOperationForm::BitNot, RuntimeOperation::ValueBitNot };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand OperatorEmitter::failWithZero(const ValueContext& value)
{
    fail(value, InternalErrorKind::OpcodeWithoutCode);
    return emitter<ConstantEmitter>().zeroOf(value.representation());
}

void OperatorEmitter::defineHeldToItsType(const ValueContext& value, Operand result)
{
    bool walkHasAsked = (m_code.role() == CodeRole::Body || m_code.role() == CodeRole::CoroutineBody) && AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime);
    if (!walkHasAsked && emittersCheckProofs() && numberRepresentations.contains(result.representation)) {
        const TypeTestPlan& plan = typeTestPlanFor(widestTypeOf(result.representation), result.representation, typeIDOf(value.id()));
        Operand tested = generator().isCheckedSum(value.id()) ? Operand(Number(generator().numberOfCheckedWord(generator().wordOfNumberInCheckedSum(result.value)))) : result;
        if (plan.verdict != TypeTestVerdict::AlwaysPasses)
            emitter<ThrowEmitter>().requireOrCrash(value, hasType(tested, plan), std::nullopt, ProofSource::OtherOperation);
    }
    define(value, result);
}

static bool markersMayMeetInArithmetic(ArithmeticOperator op, const Type& left, const Type& right)
{
    constexpr uint16_t numberOrUndefined = Primitive::Number | Primitive::Undefined;
    bool isOfMachine = op == ArithmeticOperator::Add || op == ArithmeticOperator::Sub || op == ArithmeticOperator::Mul || op == ArithmeticOperator::Div;
    return isOfMachine && left.hasOnlyPrimitives(numberOrUndefined) && right.hasOnlyPrimitives(numberOrUndefined);
}

Operand OperatorEmitter::arithmeticResult(const ValueContext& value, ArithmeticOperator op)
{
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    if (op == ArithmeticOperator::Mod && generator().isProductLeftToRemainder(left) && !generator().isGenerated(left)) [[unlikely]]
        return remainderOfProduct(value);
    if (isArithmeticOnLikelyBigInts(value)) [[unlikely]]
        return arithmeticOnLikelyBigInts(value, op);
    switch (value->form()) {
    case OperatorForm::Generic: {
        m_out.noteForm(op == ArithmeticOperator::Pow ? Form::Runtime : Form::Closed, StateOfKnowledge::Generic);
        Boxed x = in<Boxed>(left);
        Boxed y = in<Boxed>(right);
        if (sumsInt32sInPlace(value, op))
            return sumOfAnyValues(value, op, x, y);
        if (op == ArithmeticOperator::Add && typeOf(left).isAnyString() && types().isSubset(typeIDOf(right), types().builtinClass(BuiltinClasses::Array)) && generator().isInLoop(value.block()) && !standsInRareBlock(value)) {
            build().statistics().count(StatisticID::ArraysAddedToStrings);
            return callOperation<Boxed>(value, RuntimeOperation::ValueAdd, { x, y });
        }
        return arithmeticOnBoxedOperands(value, op, x, y);
    }
    case OperatorForm::Integers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Integers);
        return arithmeticOnIntegers(value, op);
    case OperatorForm::CheckedIntegers:
        m_out.noteForm(Form::Open, StateOfKnowledge::CheckedIntegers);
        return arithmeticOnCheckedIntegers(value, op);
    case OperatorForm::Numbers: {
        if (op != ArithmeticOperator::Mod && op != ArithmeticOperator::Pow)
            m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        if (value.representation() == Representation::JSValue) {
            auto termOfBoxedSum = [&](ValueID term) -> Operand {
                return representationOf(term) == Representation::Int32 ? get(term) : Operand(in<Boxed>(term));
            };
            Operand x = termOfBoxedSum(left);
            Operand y = termOfBoxedSum(right);
            return sumOfBoxedNumbers(op, x, typeOf(left), y, typeOf(right), emitter<CastEmitter>().takeNumberChecksOf(value.id()).span());
        }
        if (generator().isCheckedSum(value.id())) [[unlikely]]
            return checkedSum(value, op);
        if (generator().isCarriedSum(value.id())) [[unlikely]] {
            Integer x = integerAddedToSum(value, left);
            Integer y = integerAddedToSum(value, right);
            Integer sum = op == ArithmeticOperator::Add ? add(x, y) : sub(x, y);
            if (emittersCheckProofs())
                emitter<ThrowEmitter>().requireOrCrash(value, belowOrEqual(add(sum, integer(maxExactInteger)), integer(2 * maxExactInteger)), std::nullopt, ProofSource::RememberedFact);
            return toNumber(sum);
        }
        if (handsOnBitsOfNaN(value.value()) && (generator().holdsAnyNaN(left) || generator().holdsAnyNaN(right) || generator().takesUndefinedForNaN(left) || generator().takesUndefinedForNaN(right))) {
            if (generator().takesUndefinedForNaN(left) && generator().takesUndefinedForNaN(right))
                build().statistics().count(StatisticID::ArithmeticOnMarkers);
            Number x = asNumericOperandWithAnyNaN(left);
            Number y = asNumericOperandWithAnyNaN(right);
            return generator().settleNaNOf(value, arithmeticOnNumbers(op, x, y, &value));
        }
        if (representationOf(left) == Representation::OptionalDouble && representationOf(right) == Representation::OptionalDouble && markersMayMeetInArithmetic(op, typeOf(left), typeOf(right))
            && !(emitter<ConversionEmitter>().isOptionalInteger(get(left).value) && emitter<ConversionEmitter>().isOptionalInteger(get(right).value))) {
            build().statistics().count(StatisticID::ArithmeticOnMarkers);
            return emitter<ConversionEmitter>().withoutPayload(arithmeticOnNumbers(op, Number(get(left).value), Number(get(right).value)));
        }
        if (op == ArithmeticOperator::Mod && generator().isRemainderOfBox(value.id())) [[unlikely]]
            return remainderOfBox(value);
        Number x = asNumericOperand(left, &value);
        Number y = asNumericOperand(right, &value);
        return arithmeticOnNumbers(op, x, y, &value);
    }
    case OperatorForm::NumberAndValue:
        if (op != ArithmeticOperator::Pow && traitsOfArithmeticOperator(op).hasStubWithDouble)
            return arithmeticOnNumberAndValue(value, op);
        break;
    case OperatorForm::BigInts: {
        m_out.noteForm(op == ArithmeticOperator::Pow ? Form::Runtime : Form::Closed, StateOfKnowledge::BigInts);
        Boxed x = boxCell(in<CellPointer>(left));
        Boxed y = boxCell(in<CellPointer>(right));
        if (op != ArithmeticOperator::Pow && generator().runsRepeatedly(value.block()))
            return unboxCell(callOperation<Boxed>(value, traitsOfArithmeticOperator(op).onBigInts, { x, y }));
        return unboxCell(arithmeticOnBoxedOperands(value, op, x, y));
    }
    case OperatorForm::Strings:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::MayBeString:
        if (op == ArithmeticOperator::Add) {
            m_out.noteForm(Form::Closed, stateOf(value->form()));
            return addStrings(value);
        }
        break;
    case OperatorForm::Unselected:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::NullableNumber:
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    return failWithZero(value);
}

bool OperatorEmitter::isBigInt(ValueID value) const
{
    const Type& type = typeOf(value);
    return !type.isNever() && type.hasOnlyPrimitives(Primitive::BigInt);
}

const BigIntValues& OperatorEmitter::bigIntValues()
{
    if (!m_bigIntValues) {
        bool mayUseFrame = !generator().keepsValuesInHeapFrame() && !AOT::hasFlag(build().options().testing, Wire::TestingSwitch::DisableFrameCells);
        m_bigIntValues.emplace(m_code, types(), !mayUseFrame ? BigIntValues::FrameUse::Never : unitRunsInIterations() ? BigIntValues::FrameUse::Everywhere : BigIntValues::FrameUse::InLoops);
        if (m_bigIntValues->hasCopies())
            generator().noteCopiesOfBigInts();
    }
    return *m_bigIntValues;
}

bool OperatorEmitter::unitRunsInIterations() const
{
    return m_generator.isFunctionCalledRepeatedly() || m_facts.linked().functionsCalledInLoopsOfOwnFile.get(m_unit.function.index());
}

bool OperatorEmitter::expectsBigInts(BlockID block, ValueID value)
{
    return (m_code.at(block).isInLoop || unitRunsInIterations()) && !generator().checksEffects() && bigIntValues().isLikely(value) && !bigIntValues().isRare(block);
}

bool OperatorEmitter::isArithmeticOnLikelyBigInts(const ValueContext& value)
{
    return !m_makesProductOfRemainder && BigIntValues::isArithmetic(operationAt(value.id())) && expectsBigInts(value.block(), value.id());
}

Boxed OperatorEmitter::boxedOperand(ValueID operand)
{
    return representationOf(operand) == Representation::Cell ? boxCell(in<CellPointer>(operand)) : in<Boxed>(operand);
}

Pointer OperatorEmitter::placeOfBigInt(const ValueContext& value, unsigned length)
{
    Machine::StackSlot* slot;
    if (bigIntValues().mayShareItsPlace(value.id())) {
        if (m_blockOfBigIntPlaces != value.block()) {
            m_blockOfBigIntPlaces = value.block();
            m_bigIntPlacesUsed = 0;
        }
        if (m_bigIntPlacesUsed == m_bigIntPlaces.size())
            m_bigIntPlaces.append(m_proc.addStackSlot(bigIntSlotSize(length)));
        slot = m_bigIntPlaces[m_bigIntPlacesUsed++];
        slot->ensureSize(bigIntSlotSize(length));
    } else
        slot = m_proc.addStackSlot(bigIntSlotSize(length));
    slot->setIsDeadAtSuspensions(true);
    LValue place = m_out.slotBase(slot);
    Vector<LValue, BigIntValues::maxValuesKept> kept;
    for (ValueID other : bigIntValues().valuesKeptBeside(value.id())) {
        static constexpr OpcodeSet passesOperandOn { Opcode::Box, Opcode::Unbox, Opcode::Narrowed };
        while (!generator().isGenerated(other) && passesOperandOn.contains(opcodeOf(other)))
            other = operationAt(other).operand(0);
        if (generator().isGenerated(other))
            kept.append(boxedOperand(other).value());
    }
    if (kept.isEmpty())
        return Pointer(place);

    static_assert(BigIntValues::maxValuesKept == 2);
    auto addPlace = [&] {
        LValue added = m_out.lockedStackSlot(bigIntSlotSize(length));
        added->slot()->setIsDeadAtSuspensions(true);
        return added;
    };
    auto holdsKeptValue = [&](LValue one) {
        LValue holds = nullptr;
        for (LValue value : kept) {
            LValue isHere = m_out.equal(value, one);
            holds = holds ? m_out.bitOr(holds, isHere) : isHere;
        }
        return holds;
    };
    LValue otherPlace = addPlace();
    if (kept.size() > 1)
        otherPlace = m_out.select(holdsKeptValue(otherPlace), addPlace(), otherPlace);
    return Pointer(m_out.select(holdsKeptValue(place), otherPlace, place));
}

ValueID OperatorEmitter::zeroComparedWithRemainder(BlockID block, ValueID remainder)
{
    const Terminator& terminator = m_code.at(block).terminator;
    const Operation& operation = operationAt(remainder);
    if (operation.opcode != Opcode::Mod || !BigIntValues::isArithmetic(operation) || terminator.kind != TerminatorKind::Branch || m_code.useCountOf(remainder) != 3)
        return { };
    auto isRemainder = [&](ValueID value) {
        return value == remainder || (!m_code.isConstant(value) && opcodeOf(value) == Opcode::Unbox && m_code.useCountOf(value) == 1 && operationAt(value).valueOperand() == remainder);
    };
    ValueID condition = terminator.conditionOperand();
    const Operation& test = operationAt(condition);
    if (test.opcode != Opcode::GreaterEq || test.block() != block || m_code.useCountOf(condition) != 1 || (test.form() != OperatorForm::Generic && test.form() != OperatorForm::BigInts) || !isRemainder(test.leftOperand()))
        return { };
    ValueID zero = test.rightOperand();
    if (!m_code.isConstant(zero) && opcodeOf(zero) == Opcode::Unbox)
        zero = operationAt(zero).valueOperand();
    if (!isBigInt(zero) || bigIntValues().mayBeInFrame(zero))
        return { };
    const Edge& taken = m_code.at(terminator.taken());
    const Edge& notTaken = m_code.at(terminator.notTaken());
    const Block& other = m_code.at(notTaken.to);
    if (!notTaken.arguments().empty() || generator().onlyPredecessorOf(notTaken.to) != block || other.terminator.kind != TerminatorKind::Jump || m_code.valuesOf(notTaken.to).size() != 1)
        return { };
    ValueID sum = m_code.valuesOf(notTaken.to)[0];
    const Operation& addition = operationAt(sum);
    if (addition.opcode != Opcode::Add || !BigIntValues::isArithmetic(addition))
        return { };
    ValueID divisor = operation.rightOperand();
    if (!(addition.leftOperand() == divisor && addition.rightOperand() == remainder) && !(addition.leftOperand() == remainder && addition.rightOperand() == divisor))
        return { };
    const Edge& joined = m_code.at(other.terminator.next());
    if (joined.to != taken.to || joined.arguments().size() != taken.arguments().size())
        return { };
    bool isBrought = false;
    for (size_t index = 0; index < taken.arguments().size(); ++index) {
        if (taken.arguments()[index] == remainder && joined.arguments()[index] == sum && !isBrought)
            isBrought = true;
        else if (taken.arguments()[index] != joined.arguments()[index] || taken.arguments()[index] == remainder)
            return { };
    }
    return isBrought ? zero : ValueID();
}

bool OperatorEmitter::isConstantRead(ValueID value) const
{
    const Operation& read = operationAt(value);
    if (read.opcode == Opcode::GetModuleVariable) {
        const PromotedVariable* promoted = m_plan.promotedEnvironments().find(read.variable());
        return (!promoted || promoted->maker != m_code.function()) && m_plan.isImmutable(m_program, read.variable());
    }
    if (read.opcode != Opcode::GetEnvironmentField)
        return false;
    const Layout& layout = m_program.at(read.layout());
    return read.field() < layout.fields.size() && layout.fields[read.field()].flags.contains(FieldFlags::IsImmutable);
}

std::span<const ValueID> OperatorEmitter::valuesWithZerosAhead(BlockID block, std::span<const ValueID> values, Vector<ValueID>& storage)
{
    for (size_t index = 0; index < values.size(); ++index) {
        ValueID remainder = values[index];
        if (opcodeOf(remainder) != Opcode::Mod || !expectsBigInts(block, remainder))
            continue;
        ValueID zero = zeroComparedWithRemainder(block, remainder);
        if (!zero || m_code.isConstant(zero) || m_code.blockOf(zero) != block || !isConstantRead(zero))
            continue;
        size_t place = index;
        while (place < values.size() && values[place] != zero)
            ++place;
        if (place == values.size() || std::ranges::any_of(operationAt(zero).operands(), [&](ValueID operand) { return std::ranges::contains(values.subspan(index, place - index), operand); }))
            continue;
        storage.shrink(0);
        storage.append(values);
        std::rotate(storage.begin() + index, storage.begin() + place, storage.begin() + place + 1);
        return storage.span();
    }
    return values;
}

Pointer OperatorEmitter::withTagForSum(const ValueContext& value, Pointer address)
{
    ValueID zero = !m_makesProductOfRemainder && expectsBigInts(value.block(), value.id()) ? zeroComparedWithRemainder(value.block(), value.id()) : ValueID();
    if (!zero || (!m_code.isConstant(zero) && !generator().isGenerated(zero)))
        return address;
    Condition isZeroHere = isZero(loadField<Word32>(unboxCell(boxedOperand(zero)), m_heaps.JSBigInt_length));
    return Pointer(m_out.add(address.value(), m_out.zeroExtPtr(isZeroHere.value())));
}

Operand OperatorEmitter::copyToHeap(const ValueContext& context, Operand value)
{
    Boxed copied = callOperation<Boxed>(context, RuntimeOperation::CopyBigIntToHeap, { value.is<CellPointer>() ? boxCell(value.as<CellPointer>()) : value.as<Boxed>() });
    return value.is<CellPointer>() ? Operand(unboxCell(copied)) : Operand(copied);
}

template<typename Functor>
Operand OperatorEmitter::foundByLeaf(const ValueContext& value, Boxed found, const Functor& make)
{
    LBasicBlock isFound = newBlock();
    LBasicBlock isNotFound = newBlock();
    Join<Boxed> join(m_out);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(m_out.isZero64(found.value()), rareEdge(isNotFound), normalEdge(isFound));

    m_out.appendToArm(isFound, fork);
    join.add(found);

    m_out.appendToArm(isNotFound, fork);
    join.add(make());
    Boxed result = join.finishBehind(fork);
    return value.representation() == Representation::Cell ? Operand(unboxCell(result)) : Operand(result);
}

Operand OperatorEmitter::arithmeticOnLikelyBigInts(const ValueContext& value, ArithmeticOperator op)
{
    struct Operations {
        RuntimeOperation inHeap;
        RuntimeOperation inFrame;
        unsigned lengthOfPlace;
    };
    auto operationsOf = [](ArithmeticOperator op) -> Operations {
        switch (op) {
        case ArithmeticOperator::Add:
            return { RuntimeOperation::TryAddBigInts, RuntimeOperation::TryAddBigIntsInFrame, bigIntSlotLength };
        case ArithmeticOperator::Sub:
            return { RuntimeOperation::TrySubBigInts, RuntimeOperation::TrySubBigIntsInFrame, bigIntSlotLength };
        case ArithmeticOperator::Mul:
            return { RuntimeOperation::TryMulBigInts, RuntimeOperation::TryMulBigIntsInFrame, productSlotLength };
        case ArithmeticOperator::Div:
            return { RuntimeOperation::TryDivBigInts, RuntimeOperation::TryDivBigIntsInFrame, bigIntSlotLength };
        case ArithmeticOperator::Mod:
            return { RuntimeOperation::TryModBigInts, RuntimeOperation::TryModBigIntsInFrame, bigIntSlotLength };
        case ArithmeticOperator::Pow:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    m_out.noteForm(Form::Closed, value->form() == OperatorForm::BigInts ? StateOfKnowledge::BigInts : StateOfKnowledge::Generic);
    Boxed left = boxedOperand(value->leftOperand());
    Boxed right = boxedOperand(value->rightOperand());
    auto make = [&] {
        return arithmeticOnBoxedOperands(value, op, left, right);
    };
    Operations operations = operationsOf(op);
    if (ValueID quotient = bigIntValues().quotientForRemainder(value.id())) {
        Boxed found = bigIntValues().mayBeInFrame(value.id()) ? callLeaf<Boxed>(RuntimeOperation::TryModBigIntsByQuotientInFrame, { left, right, boxedOperand(quotient), placeOfBigInt(value, bigIntSlotLength) })
            : callLeaf<Boxed>(RuntimeOperation::TryModBigIntsByQuotient, { left, right, boxedOperand(quotient), Pointer(m_instance) });
        return foundByLeaf(value, found, make);
    }
    bool isInFrame = bigIntValues().mayBeInFrame(value.id());
    Pointer address = isInFrame ? placeOfBigInt(value, operations.lengthOfPlace) : Pointer(m_instance);
    if (op == ArithmeticOperator::Mod)
        address = withTagForSum(value, address);
    return foundByLeaf(value, callLeaf<Boxed>(isInFrame ? operations.inFrame : operations.inHeap, { left, right, address }), make);
}

bool OperatorEmitter::leavesProductToRemainder(BlockID block, ValueID id, const Operation& product, Vector<BlockID, 2>& rareBlocksWithProduct)
{
    constexpr unsigned mostBlocksOnTheWay = 8;
    auto takes = [](const Operation& operation, ValueID value) {
        unsigned count = 0;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            count += operand == value;
        });
        return count;
    };
    if ((product.form() != OperatorForm::Generic && product.form() != OperatorForm::BigInts) || typeOf(product.leftOperand()).hasObjects() || typeOf(product.rightOperand()).hasObjects())
        return false;
    if (m_code.at(block).frequency == BlockFrequency::Rare)
        return false;
    uint32_t usesLeft = m_code.useCountOf(id);
    ValueID onlyUser = m_code.onlyUserOf(id);
    if (usesLeft == std::numeric_limits<uint8_t>::max() || (usesLeft == 1 && (!onlyUser || operationAt(onlyUser).opcode != Opcode::Mod)))
        return false;
    BlockID current = block;
    bool isBehindProduct = false;
    for (unsigned blocksLeft = mostBlocksOnTheWay; blocksLeft; --blocksLeft) {
        const Block& onTheWay = m_code.at(current);
        if (onTheWay.handler)
            return false;
        std::span<const ValueID> values = m_code.valuesOf(onTheWay);
        for (size_t index = 0; index < values.size(); ++index) {
            if (current == block && !isBehindProduct) {
                isBehindProduct = values[index] == id;
                continue;
            }
            const Operation& operation = operationAt(values[index]);
            if (operation.opcode == Opcode::Mod && operation.leftOperand() == id) {
                if (operation.form() != product.form() || takes(operation, id) != 1 || operation.representation != product.representation)
                    return false;
                --usesLeft;
                if (!bigIntValues().isLikely(id))
                    return false;
                for (BlockID rare : rareBlocksWithProduct) {
                    for (ValueID inRareBlock : m_code.valuesOf(rare))
                        usesLeft -= std::min(usesLeft, takes(operationAt(inRareBlock), id));
                }
                return !usesLeft;
            }
            if (!operation.traits().isUnobservable() || takes(operation, id))
                return false;
        }
        const Terminator& terminator = onTheWay.terminator;
        EdgeID next;
        if (terminator.kind == TerminatorKind::Jump)
            next = terminator.next();
        else if (terminator.kind == TerminatorKind::Branch) {
            auto isRare = [&](EdgeID edge) {
                return m_code.at(m_code.at(edge).to).frequency == BlockFrequency::Rare;
            };
            bool takenIsRare = isRare(terminator.taken());
            if (takenIsRare == isRare(terminator.notTaken()))
                return false;
            next = takenIsRare ? terminator.notTaken() : terminator.taken();
            const Edge& rare = m_code.at(takenIsRare ? terminator.taken() : terminator.notTaken());
            if (std::ranges::contains(rare.arguments(), id) || generator().onlyPredecessorOf(rare.to) != current || m_code.at(rare.to).handler)
                return false;
            rareBlocksWithProduct.append(rare.to);
        } else
            return false;
        const Edge& edge = m_code.at(next);
        if (std::ranges::contains(edge.arguments(), id) || m_code.at(edge.to).isLoopHead || generator().onlyPredecessorOf(edge.to) != current)
            return false;
        current = edge.to;
    }
    return false;
}

Operand OperatorEmitter::remainderOfProduct(const ValueContext& value)
{
    ValueID product = value->leftOperand();
    Boxed left = boxedOperand(operationAt(product).leftOperand());
    Boxed right = boxedOperand(operationAt(product).rightOperand());
    Boxed divisor = boxedOperand(value->rightOperand());
    m_out.noteForm(Form::Closed, value->form() == OperatorForm::BigInts ? StateOfKnowledge::BigInts : StateOfKnowledge::Generic);
    bool isInFrame = bigIntValues().mayBeInFrame(value.id());
    Pointer address = withTagForSum(value, isInFrame ? placeOfBigInt(value, bigIntSlotLength) : Pointer(m_instance));
    Boxed found = callLeaf<Boxed>(isInFrame ? RuntimeOperation::TryRemainderOfProductInFrame : RuntimeOperation::TryRemainderOfProduct, { left, right, divisor, address });
    return foundByLeaf(value, found, [&] {
        m_makesProductOfRemainder = true;
        Operand made = generator().generateProductHere(value.block(), product);
        m_makesProductOfRemainder = false;
        Boxed madeBoxed = made.is<CellPointer>() ? boxCell(made.as<CellPointer>()) : made.as<Boxed>();
        return arithmeticOnBoxedOperands(value, ArithmeticOperator::Mod, madeBoxed, divisor);
    });
}

bool OperatorEmitter::mayBothBeInt32(const ValueContext& value) const
{
    return typeOf(value->leftOperand()).primitives & Primitive::Integer && typeOf(value->rightOperand()).primitives & Primitive::Integer;
}

LValue OperatorEmitter::callBinaryStub(const ValueContext& value, BinaryOperator op, Boxed left, Boxed right)
{
    bool isAfterInt32Check = !mayBothBeInt32(value) && hasFormAfterInt32Check(op);
    return callStub(value, binaryOperation(op, isAfterInt32Check), { left.value(), right.value() }).first;
}

Boxed OperatorEmitter::arithmeticOnBoxedOperands(const ValueContext& value, ArithmeticOperator op, Boxed left, Boxed right)
{
    if (op == ArithmeticOperator::Pow)
        return callOperation<Boxed>(value, RuntimeOperation::ValuePow, { left, right });
    return Boxed(callBinaryStub(value, traitsOfArithmeticOperator(op).stub, left, right));
}

bool OperatorEmitter::speaksForNumber(ValueID operand, unsigned depth)
{
    static constexpr OpcodeSet preserving { Opcode::Box, Opcode::Narrowed, Opcode::Unbox };
    while (!m_code.isConstant(operand) && preserving.contains(opcodeOf(operand)))
        operand = m_code.at(operand).operand(0);
    if (typeOf(operand).isNumber())
        return true;
    if (!depth || m_code.isConstant(operand))
        return false;
    const Operation& made = m_code.at(operand);
    if (made.opcode == Opcode::Parameter)
        return !typeOf(operand).hasObjects();
    return made.opcode == Opcode::Add && (speaksForNumber(made.operand(0), depth - 1) || speaksForNumber(made.operand(1), depth - 1));
}

bool OperatorEmitter::sumsInt32sInPlace(const ValueContext& value, ArithmeticOperator op)
{
    if ((op != ArithmeticOperator::Add && op != ArithmeticOperator::Sub) || (!generator().isInLoop(value.block()) && !m_facts.linked().functionsReachedByRecursion.get(m_unit.function.index())) || standsInRareBlock(value))
        return false;
    if (op == ArithmeticOperator::Add && !speaksForNumber(value->leftOperand()) && !speaksForNumber(value->rightOperand()))
        return false;
    return typeOf(value->leftOperand()).primitives & Primitive::Integer && typeOf(value->rightOperand()).primitives & Primitive::Integer;
}

Boxed OperatorEmitter::sumOfAnyValues(const ValueContext& value, ArithmeticOperator op, Boxed left, Boxed right)
{
    auto isInt32Box = [&](ValueID operand) {
        return typeOf(operand).isInt32() && (representationOf(operand) == Representation::Int32 || emitter<ElementAccessEmitter>().boxIsMadeInFormOfType(operand));
    };
    bool isLeftInt32 = isInt32Box(value->leftOperand());
    bool isRightInt32 = isInt32Box(value->rightOperand());
    Boxed tested = isLeftInt32 ? right : isRightInt32 ? left : Boxed(m_out.bitAnd(left.value(), right.value()));
    LBasicBlock int32Case = newBlock();
    LBasicBlock otherCase = newBlock();
    Join<Boxed> join(m_out);
    Condition areInt32 = isInt32(tested);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(areInt32.value(), normalEdge(int32Case), unlikelyEdge(otherCase));

    m_out.appendToArm(int32Case, fork);
    Integer x = signExtended(int32InBox(left));
    Integer y = signExtended(int32InBox(right));
    Integer exact = op == ArithmeticOperator::Add ? add(x, y) : sub(x, y);
    join.add(emitter<ConversionEmitter>().boxInteger(exact, { 2 * static_cast<int64_t>(INT32_MIN), 2 * static_cast<int64_t>(INT32_MAX) + 1 }));

    m_out.appendToArm(otherCase, fork);
    join.add(arithmeticOnBoxedOperands(value, op, left, right));
    return join.finishBehind(fork);
}

Number OperatorEmitter::arithmeticOnNumberAndValue(const ValueContext& value, ArithmeticOperator op)
{
    bool numberIsOnRight = !typeOf(value->leftOperand()).isNumber();
    ValueID numberID = numberIsOnRight ? value->rightOperand() : value->leftOperand();
    ValueID otherID = numberIsOnRight ? value->leftOperand() : value->rightOperand();
    Number numberOperand = in<Number>(numberID);
    Boxed otherOperand = in<Boxed>(otherID);
    ArithmeticOperatorTraits traits = traitsOfArithmeticOperator(op);
    Word32 typeAndNumberSide = word32(static_cast<int32_t>(NumberAndValueBits::encode(0, numberIsOnRight)));
    auto byStub = [&] {
        return Number(callStub(value, binaryWithDouble(traits.stub, numberIsOnRight), { numberOperand.value(), otherOperand.value(), typeAndNumberSide.value() }).first);
    };
    if (typeOf(otherID).primitives & Primitive::Number) {
        m_out.noteForm(Form::Closed, StateOfKnowledge::NumberAndValue);
        if (standsInRareBlock(value))
            return byStub();
        auto withNumber = [&](Number otherNumber) {
            return numberIsOnRight ? arithmeticOnNumbers(op, otherNumber, numberOperand, &value) : arithmeticOnNumbers(op, numberOperand, otherNumber, &value);
        };
        LBasicBlock int32Case = newBlock();
        LBasicBlock noInt32Case = newBlock();
        LBasicBlock doubleCase = newBlock();
        LBasicBlock otherCase = newBlock();
        Join<Number> join(m_out);
        Condition holdsInt32 = isInt32(otherOperand);
        ScopeFork fork = m_out.forkScope();
        m_out.branch(holdsInt32.value(), normalEdge(int32Case), unlikelyEdge(noInt32Case));
        m_out.appendToArm(int32Case, fork);
        join.add(withNumber(toNumber(int32InBox(otherOperand))));
        m_out.appendToArm(noInt32Case, fork);
        Condition holdsDouble = isNumber(otherOperand);
        ScopeFork forkOfNoInt32 = m_out.forkScope();
        m_out.branch(holdsDouble.value(), normalEdge(doubleCase), rareEdge(otherCase));
        m_out.appendToArm(doubleCase, forkOfNoInt32);
        join.add(withNumber(unboxDouble(otherOperand)));
        m_out.appendToArm(otherCase, forkOfNoInt32);
        join.add(byStub());
        return join.finishBehind(fork);
    }
    m_out.noteForm(Form::Closed, StateOfKnowledge::NumberAndOtherValue);
    return byStub();
}

template<typename Functor>
Operand OperatorEmitter::withIntegerOperand(const ValueContext& value, const Functor& functor)
{
    ValueID operand = value->valueOperand();
    switch (representationOf(operand)) {
    case Representation::Int32:
        return functor(in<Word32>(operand));
    case Representation::Int64:
        return functor(in<Integer>(operand));
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
    return failWithZero(value);
}

Operand OperatorEmitter::unaryOnLikelyBigInt(const ValueContext& value, UnaryOperator op)
{
    struct Operations {
        RuntimeOperation inHeap;
        RuntimeOperation inFrame;
    };
    auto operationsOf = [](UnaryOperator op) -> Operations {
        switch (op) {
        case UnaryOperator::Negate:
            return { RuntimeOperation::TryNegateBigInt, RuntimeOperation::TryNegateBigIntInFrame };
        case UnaryOperator::Increment:
            return { RuntimeOperation::TryIncrementBigInt, RuntimeOperation::TryIncrementBigIntInFrame };
        case UnaryOperator::Decrement:
            return { RuntimeOperation::TryDecrementBigInt, RuntimeOperation::TryDecrementBigIntInFrame };
        case UnaryOperator::BitNot:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    bool isBigInt = value->form() == OperatorForm::BigInts;
    m_out.noteForm(Form::Closed, isBigInt ? StateOfKnowledge::BigInts : StateOfKnowledge::Generic);
    Boxed operand = boxedOperand(value->valueOperand());
    auto make = [&] {
        return isBigInt ? callOperation<Boxed>(value, traitsOfUnaryOperator(op).onBigInt, { operand }) : Boxed(callStub(value, unaryOperation(traitsOfUnaryOperator(op).stub), { operand.value() }).first);
    };
    Operations operations = operationsOf(op);
    Boxed found = bigIntValues().mayBeInFrame(value.id()) ? callLeaf<Boxed>(operations.inFrame, { operand, placeOfBigInt(value, bigIntSlotLength) })
        : callLeaf<Boxed>(operations.inHeap, { operand, Pointer(m_instance) });
    return foundByLeaf(value, found, make);
}

Operand OperatorEmitter::unaryResult(const ValueContext& value, UnaryOperator op)
{
    if (isArithmeticOnLikelyBigInts(value)) [[unlikely]]
        return unaryOnLikelyBigInt(value, op);
    ValueID operand = value->valueOperand();
    switch (value->form()) {
    case OperatorForm::Generic:
        m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
        return Boxed(callStub(value, unaryOperation(traitsOfUnaryOperator(op).stub), { in<Boxed>(operand).value() }).first);
    case OperatorForm::Integers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Integers);
        return unaryOnIntegers(value, op);
    case OperatorForm::CheckedIntegers:
        m_out.noteForm(Form::Open, StateOfKnowledge::CheckedIntegers);
        return unaryOnCheckedIntegers(value, op);
    case OperatorForm::Numbers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        if (value.representation() == Representation::JSValue) {
            RELEASE_ASSERT(op == UnaryOperator::Increment || op == UnaryOperator::Decrement);
            ArithmeticOperator sum = op == UnaryOperator::Increment ? ArithmeticOperator::Add : ArithmeticOperator::Sub;
            Boxed stepped = in<Boxed>(operand);
            return sumOfBoxedNumbers(sum, stepped, typeOf(operand), word32(1), typeOf(operand));
        }
        if (handsOnBitsOfNaN(value.value()) && generator().holdsAnyNaN(operand))
            return generator().settleNaNOf(value, neg(generator().getWithAnyNaN(operand).as<Number>()));
        return unaryOnNumbers(op, operand);
    case OperatorForm::BigInts:
        m_out.noteForm(Form::Runtime, StateOfKnowledge::BigInts);
        return unboxCell(callOperation<Boxed>(value, traitsOfUnaryOperator(op).onBigInt, { boxCell(in<CellPointer>(operand)) }));
    case OperatorForm::Unselected:
    case OperatorForm::NumberAndValue:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::Strings:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
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
    return failWithZero(value);
}

Operand OperatorEmitter::unaryOnIntegers(const ValueContext& value, UnaryOperator op)
{
    switch (op) {
    case UnaryOperator::Negate:
        return withIntegerOperand(value, [&](auto operand) -> Operand {
            return neg(operand);
        });
    case UnaryOperator::Increment:
        return arithmeticOnIntegers(value, ArithmeticOperator::Add);
    case UnaryOperator::Decrement:
        return arithmeticOnIntegers(value, ArithmeticOperator::Sub);
    case UnaryOperator::BitNot:
        return bitXor(in<Word32>(value->valueOperand()), -1);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand OperatorEmitter::unaryOnCheckedIntegers(const ValueContext& value, UnaryOperator op)
{
    switch (op) {
    case UnaryOperator::Negate:
        return withIntegerOperand(value, [&](auto operand) -> Operand {
            return neg(toNumber(operand));
        });
    case UnaryOperator::Increment:
        return arithmeticOnCheckedIntegers(value, ArithmeticOperator::Add);
    case UnaryOperator::Decrement:
        return arithmeticOnCheckedIntegers(value, ArithmeticOperator::Sub);
    case UnaryOperator::BitNot:
        break;
    }
    return failWithZero(value);
}

Operand OperatorEmitter::unaryOnNumbers(UnaryOperator op, ValueID id)
{
    if (op == UnaryOperator::BitNot && representationOf(id) == Representation::Int64)
        return bitXor(low32InWords(in<Integer>(id)), -1);
    Number operand = asNumericOperand(id);
    switch (op) {
    case UnaryOperator::Negate:
        return neg(operand);
    case UnaryOperator::Increment:
        return add(operand, number(1));
    case UnaryOperator::Decrement:
        return sub(operand, number(1));
    case UnaryOperator::BitNot:
        return bitXor(emitter<ConversionEmitter>().toInt32(operand, ConversionEmitter::BitsOfInt32::All), -1);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number OperatorEmitter::asNumberLikeOperand(ValueID id)
{
    if (representationOf(id) == Representation::Boolean)
        return toNumber(asWord32(in<Condition>(id)));
    Boxed value = in<Boxed>(id);
    Condition isTrue = equal(value, Boxed(m_out.constInt64(JSValue::ValueTrue)));
    Condition isUndefined = equal(value, undefined());
    Number notANumber = number(PNaN);
    Number convertedPrimitive = select(isUndefined, notANumber, toNumber(asWord32(isTrue)));
    Condition valueIsNumber = isNumber(value);
    return select(valueIsNumber, numberToDouble(value), convertedPrimitive);
}

template<typename Functor>
Operand OperatorEmitter::convertedToNumber(const ValueContext& value, const Functor& convertValue)
{
    ValueID operand = value->valueOperand();
    switch (value->form()) {
    case OperatorForm::Generic:
        return convertValue(in<Boxed>(operand));
    case OperatorForm::Integers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Integers);
        return get(operand);
    case OperatorForm::Numbers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        return asNumericOperand(operand, &value);
    case OperatorForm::NullableNumber:
        m_out.noteForm(Form::Open, StateOfKnowledge::NullableNumber);
        return asNumberLikeOperand(operand);
    case OperatorForm::Booleans:
        m_out.noteForm(Form::Open, StateOfKnowledge::Booleans);
        return asWord32(in<Condition>(operand));
    case OperatorForm::Unselected:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::NumberAndValue:
    case OperatorForm::Bits:
    case OperatorForm::BigInts:
    case OperatorForm::Strings:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    return failWithZero(value);
}

bool OperatorEmitter::isOnlyTruncatedToInt32(ValueID number)
{
    ValueID user = generator().code().useCountOf(number) == 1 ? generator().code().onlyUserOf(number) : ValueID();
    return user && (opcodeOf(user) == Opcode::ToInt32 || opcodeOf(user) == Opcode::ToUint32);
}

void OperatorEmitter::generateToNumber(const ValueContext& value)
{
    if (ValueID operand = value->valueOperand(); handsOnBitsOfNaN(value.value()) && (generator().holdsAnyNaN(operand) || generator().takesUndefinedForNaN(operand))) {
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        define(value, generator().settleNaNOf(value, asNumericOperandWithAnyNaN(operand)));
        return;
    }
    if (ValueID operand = value->valueOperand(); representationOf(operand) == Representation::OptionalDouble && value->form() == OperatorForm::Numbers && isOnlyTruncatedToInt32(value.id())) {
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        define(value, asNumber(get(operand), typeOf(operand), PayloadOfNaN::Stays));
        return;
    }
    define(value, convertedToNumber(value, [&](Boxed operand) -> Operand {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
        return Number(callStub(value, toNumberAsDouble(), { operand.value() }).first);
    }));
}

void OperatorEmitter::generateToNumeric(const ValueContext& value)
{
    define(value, convertedToNumber(value, [&](Boxed operand) -> Operand {
        m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
        return Boxed(callStub(value, unaryOperation(UnaryOperationForm::ToNumeric), { operand.value() }).first);
    }));
}

void OperatorEmitter::generateLogicalNot(const ValueContext& value)
{
    define(value, logicalNot(in<Condition>(value->valueOperand())));
}

void OperatorEmitter::generateTypeOf(const ValueContext& value)
{
    auto stringOf = [&](TypeofType type) {
        return CellPointer(loadImmutablePointer(AOT_INSTANCE_OFFSET(typeStrings) + static_cast<ptrdiff_t>(type) * sizeof(JSCell*)).value());
    };
    Boxed operand = in<Boxed>(value->valueOperand());
    const Type& type = typeOf(value->valueOperand());
    if (type.hasOnlyPrimitives(Primitive::Undefined | Primitive::AnyString) && generator().usesLoopForms(value.block()))
        return define(value, select(isCell(operand), stringOf(TypeofType::String), stringOf(TypeofType::Undefined)));
    if (type.hasOnlyPrimitives(Primitive::Number | Primitive::AnyString) && generator().usesLoopForms(value.block()))
        return define(value, select(isCell(operand), stringOf(TypeofType::String), stringOf(TypeofType::Number)));
    if (type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) && generator().usesLoopForms(value.block()))
        return define(value, select(isNumber(operand), stringOf(TypeofType::Number), stringOf(TypeofType::Undefined)));
    define(value, CellPointer(callStub(value, typeofString(), { operand.value() }).first));
}

template<typename T, typename Functor>
T OperatorEmitter::passingOrConverted(Boxed value, const TypeTestPlan& plan, Form formOfConversion, const Functor& convert)
{
    switch (plan.verdict) {
    case TypeTestVerdict::AlwaysPasses:
        m_out.noteForm(Form::Open, StateOfKnowledge::PassesTest);
        return T(value.value());
    case TypeTestVerdict::NeverPasses:
        m_out.noteForm(formOfConversion, StateOfKnowledge::FailsTest);
        return convert(CallOptions { });
    case TypeTestVerdict::NeedsTest:
        break;
    }

    m_out.noteForm(Form::Open, StateOfKnowledge::NeedsTest);
    LBasicBlock passes = newBlock();
    LBasicBlock fails = newBlock();
    Join<T> join(m_out);
    branchOnType(value, plan, { normalEdge(passes), rareEdge(fails) });

    m_out.appendTo(passes);
    join.add(T(value.value()));

    m_out.appendTo(fails);
    join.add(convert(CallOptions { .isOnRarePath = true }));
    return join.finish();
}

void OperatorEmitter::generateToObject(const ValueContext& value)
{
    bool hasResult = !isNeverType(value->type);
    Boxed operand = in<Boxed>(value->valueOperand());
    const TypeTestPlan& plan = typeTestPlanFor(typeIDOf(value->valueOperand()), Representation::JSValue, TypeTable::object);
    if (hasResult && plan.verdict == TypeTestVerdict::AlwaysPasses) {
        m_out.noteForm(Form::Open, StateOfKnowledge::PassesTest);
        defineNarrowed(value, operand);
        return;
    }

    StringID message = value->message() ? value->message() : m_program.strings.emptyString();
    CellPointer object = passingOrConverted<CellPointer>(operand, plan, Form::Runtime, [&](const CallOptions& options) {
        return callOperation<CellPointer>(value, RuntimeOperation::ToObject, { operand, numberOf(m_mentions.add(NumberSpace::ShownString, message)) }, options);
    });
    if (hasResult)
        define(value, object);
}

void OperatorEmitter::generateToThis(const ValueContext& value)
{
    Boxed operand = in<Boxed>(value->valueOperand());
    const TypeTestPlan& plan = typeTestPlanFor(typeIDOf(value->valueOperand()), Representation::JSValue, TypeTable::object);
    if (plan.verdict == TypeTestVerdict::AlwaysPasses) {
        m_out.noteForm(Form::Open, StateOfKnowledge::PassesTest);
        defineNarrowed(value, operand);
        return;
    }
    define(value, passingOrConverted<CellPointer>(operand, plan, Form::Runtime, [&](const CallOptions&) {
        return unboxCell(Boxed(callStub(value, AOT::toThis(), { operand.value() }).first));
    }));
}

void OperatorEmitter::generateToPropertyKey(const ValueContext& value)
{
    Boxed operand = in<Boxed>(value->valueOperand());
    constexpr TypeID keyType = typeOfPrimitives(Primitive::String | Primitive::Symbol);
    const TypeTestPlan& plan = typeTestPlanFor(typeIDOf(value->valueOperand()), Representation::JSValue, keyType);
    define(value, unboxCell(passingOrConverted<Boxed>(operand, plan, Form::Closed, [&](const CallOptions&) {
        return Boxed(callStub(value, AOT::toPropertyKey(), { operand.value() }).first);
    })));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
