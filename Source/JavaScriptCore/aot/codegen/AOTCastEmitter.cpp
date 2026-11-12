/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCastEmitter.h"

#if ENABLE(AOT)

#include "AOTClosureTestEmitter.h"
#include "AOTCompileUnit.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsNet.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTOpcodeSets.h"
#include "AOTSlotEmitter.h"
#include "AOTStubsClosures.h"
#include "AOTStubsValues.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestEmitter.h"
#include "AOTTypeTestPlan.h"
#include <algorithm>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

void CastEmitter::generateNarrowed(const ValueContext& value)
{
    Operand operand = get(value->valueOperand());
    if (operand.is<Boxed>() && value->testOperand() && !value->isOnTakenEdge()) {
        const Operation& test = operationAt(value->testOperand());
        bool isNotUndefined = test.opcode == Opcode::TypeTest && test.valueOperand() == value->valueOperand() && types().isSubset(TypeTable::undefined, test.testedType());
        if (LValue word = isNotUndefined ? emitter<SlotEmitter>().findSlotWord(operand.value) : nullptr)
            operand = Boxed(word);
    }
    defineNarrowed(value, operand);
}

bool CastEmitter::tryTestSlotWord(const ValueContext& test, Boxed value, const TypeTestPlan& plan)
{
    constexpr int64_t bitsOutsideUndefined = ~static_cast<int64_t>(JSValue::ValueUndefined);
    LValue word = emitter<SlotEmitter>().findSlotWord(value.value());
    if (!word || !Machine::isEncodableBitImmediate(Width64, bitsOutsideUndefined))
        return false;
    bool isTestEmpty = plan.verdict == TypeTestVerdict::NeedsTest && (plan.passingPrimitives & Primitive::Undefined) && !(plan.passingPrimitives & ~Primitive::UndefinedOrNull) && !plan.integers && plan.stringLiterals.isEmpty() && plan.cellTests.isEmpty() && plan.objects != ObjectsVerdict::AllPass;
    if (!isTestEmpty)
        return false;
    bool nullFails = plan.failingPrimitives & Primitive::Null;
    if (nullFails && !test.isBranchCondition())
        return false;
    Condition holdsNothing(m_out.testIsZero64(word, m_out.constInt64(bitsOutsideUndefined)));
    if (!nullFails) {
        if (test.isBranchCondition())
            branch(holdsNothing, test.branchTargets());
        else
            define(test, holdsNothing);
        return true;
    }
    BranchTargets targets = test.branchTargets();
    LBasicBlock mayHoldNull = newBlock();
    m_out.branch(holdsNothing.value(), normalEdge(mayHoldNull), targets.notTaken);
    m_out.appendToContinuation(mayHoldNull);
    m_out.branch(m_out.equal(word, m_out.constInt64(JSValue::ValueNull)), targets.notTaken, targets.taken);
    return true;
}

void CastEmitter::generateCheckInitialized(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    emitter<ThrowEmitter>().requireInitialized(value, operand, typeOf(source), value->name());
    defineNarrowed(value, operand);
}

void CastEmitter::generateRequireObjectCoercible(const ValueContext& value)
{
    requireObjectCoercible(value);
    defineNarrowed(value, get(value->valueOperand()));
}

void CastEmitter::requireObjectCoercible(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    if (value->isProvenByMirror() && !emitter<ElementAccessEmitter>().checksBounds())
        return;
    emitter<ThrowEmitter>().requireNotNullish(value, get(source), typeOf(source), value->nullishAccess(), value->name());
}

void CastEmitter::generateRequireObject(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    const TypeTestPlan& plan = planFor(source, TypeTable::object);
    if (plan.verdict == TypeTestVerdict::NeverPasses)
        m_out.noteForm(Form::Runtime, StateOfKnowledge::FailsTest);
    else
        m_out.noteForm(Form::Open, plan.verdict == TypeTestVerdict::AlwaysPasses ? StateOfKnowledge::PassesTest : StateOfKnowledge::NeedsTest);
    if (plan.verdict != TypeTestVerdict::AlwaysPasses) {
        LBasicBlock isNotObject = newBlock();
        LBasicBlock continuation = newBlock();
        ScopeFork fork = m_out.forkScope();
        branchOnType(operand, plan, { normalEdge(continuation), rareEdge(isNotObject) });

        m_out.appendTo(isNotObject);
        static_assert(mayCollect(Opcode::RequireObject) || !declaredBoundOf(RuntimeOperation::ThrowRuntimeError).mayCollect);
        callVoidOperation(value, RuntimeOperation::ThrowRuntimeError, { word32(static_cast<int32_t>(value->runtimeError())) }, { .isOnRarePath = true });
        m_out.unreachable();

        m_out.appendToJoin(continuation, fork);
    }
    defineNarrowed(value, operand);
}

bool CastEmitter::asksForRangeOfInt32(const ValueContext& test)
{
    const Type& tested = types().at(test->testedType());
    return tested.isInteger() && !tested.integers.isEmpty() && tested.integers != Range::int32() && tested.integers.isSubsetOf(Range::int32());
}

ALWAYS_INLINE_INTO_OTHER_FILES ValueID CastEmitter::unboxingBehindTestForInt32(const ValueContext& test)
{
    const Terminator& end = m_code.at(test.block()).terminator;
    if ((test->testedType() != TypeTable::int32 && !asksForRangeOfInt32(test)) || end.kind != TerminatorKind::Branch || end.conditionOperand() != test.id())
        return { };
    const Edge& taken = m_code.at(end.taken());
    if (!taken.arguments().empty() || generator().singlePredecessorOf(taken.to) != test.block() || generator().leavesTurnLaidOut(taken.to))
        return { };
    bool mayUnboxLater = !generator().keepsValuesInHeapFrame();
    bool isInLoop = m_code.at(test.block()).isInLoop;
    auto followsChecks = [&](BlockID block) {
        static constexpr unsigned mostChecks = 8;
        for (unsigned checks = 0; block && checks < mostChecks; ++checks) {
            if (block == taken.to)
                return true;
            block = generator().singlePredecessorOf(block);
        }
        return false;
    };
    for (ValueID id : m_code.valuesOf(taken.to)) {
        ValueID user = mayUnboxLater && m_code.at(id).opcode == Opcode::Narrowed ? m_code.onlyUserOf(id) : id;
        const Operation& unboxing = m_code.at(user ? user : id);
        if (unboxing.opcode != Opcode::Unbox || (unboxing.representation != Representation::Int32 && unboxing.representation != Representation::Int64))
            continue;
        if (isInLoop && user && user != id && !followsChecks(m_code.blockOf(user)))
            continue;
        const Operation& narrowed = m_code.at(unboxing.valueOperand());
        if (narrowed.opcode == Opcode::Narrowed && narrowed.isOnTakenEdge() && narrowed.testOperand() == test.id() && narrowed.valueOperand() == test->valueOperand())
            return user ? user : id;
    }
    return { };
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CastEmitter::tryBranchOnInt32AndHandItOn(const ValueContext& test, Operand operand)
{
    if (!operand.is<Boxed>())
        return false;
    ValueID unboxing = unboxingBehindTestForInt32(test);
    if (!unboxing)
        return false;
    RELEASE_ASSERT(!m_integersHandedOn.contains(unboxing.index()));

    Boxed value = operand.as<Boxed>();
    BranchTargets targets = test.branchTargets();
    if (test->mayRejectWholeDoubles()) {
        Word32 word = unboxInt32(value);
        m_out.branch(isInt32(value).value(), targets.taken, targets.notTaken);
        m_integersHandedOn.add(unboxing.index(), Operand(word));
        build().statistics().count(StatisticID::UnboxingsFusedIntoTypeChecks);
        return true;
    }
    LBasicBlock isTagged = newBlock();
    LBasicBlock isNotTagged = newBlock();
    LBasicBlock isDouble = newBlock();
    bool asksForRange = asksForRangeOfInt32(test);
    Range range = types().at(test->testedType()).integers;
    bool isNatural = asksForRange && !range.minimum;
    BlockEdge hasInteger = asksForRange && !isNatural ? normalEdge(newBlock()) : targets.taken;
    bool isWide = isNatural && m_code.at(unboxing).representation == Representation::Int64;
    LValue integer = m_out.phiIn(hasInteger.block(), isWide ? Machine::Int64 : Machine::Int32);
    auto handOn = [&](Operand found) {
        m_out.upsilon(found.value, integer);
        m_out.jump(hasInteger);
    };
    LValue untagged = isNatural ? m_out.sub(value.value(), m_numberTag) : nullptr;
    Condition isTaggedInteger = isNatural ? belowOrEqual(Integer(untagged), Integer(m_out.constInt64(range.maximum))) : isInt32(value);
    m_out.branch(isTaggedInteger.value(), normalEdge(isTagged), rareEdge(isNotTagged));

    m_out.appendTo(isTagged);
    handOn(isWide ? Operand(Integer(untagged)) : Operand(isNatural ? low32(Integer(untagged)) : unboxInt32(value)));

    m_out.appendTo(isNotTagged);
    m_out.branch(isNumber(value).value(), normalEdge(isDouble), targets.notTaken);

    m_out.appendTo(isDouble);
    Word32 whole = emitter<ConversionEmitter>().int32OrElse(unboxDouble(value), targets.notTaken.block());
    if (isNatural) {
        LBasicBlock isSmall = newBlock();
        m_out.branch(belowOrEqual(whole, word32(static_cast<int32_t>(range.maximum))).value(), normalEdge(isSmall), targets.notTaken);
        m_out.appendTo(isSmall);
    }
    handOn(isWide ? Operand(zeroExtended(whole)) : Operand(whole));

    if (asksForRange && !isNatural) {
        m_out.appendTo(hasInteger.block());
        Word32 fromMinimum = sub(Word32(integer), word32(static_cast<int32_t>(range.minimum)));
        m_out.branch(above(fromMinimum, word32(static_cast<int32_t>(range.maximum - range.minimum))).value(), targets.notTaken, targets.taken);
    }
    m_integersHandedOn.add(unboxing.index(), isWide ? Operand(Integer(integer)) : Operand(Word32(integer)));
    build().statistics().count(StatisticID::UnboxingsFusedIntoTypeChecks);
    return true;
}

ALWAYS_INLINE_INTO_OTHER_FILES ValueID CastEmitter::unboxingToDoubleBehindTestForNumber(const ValueContext& test)
{
    const Terminator& end = m_code.at(test.block()).terminator;
    if (test->testedType() != TypeTable::number || end.kind != TerminatorKind::Branch || end.conditionOperand() != test.id())
        return { };
    const Edge& taken = m_code.at(end.taken());
    if (!taken.arguments().empty() || generator().singlePredecessorOf(taken.to) != test.block() || generator().leavesTurnLaidOut(taken.to))
        return { };
    for (ValueID id : m_code.valuesOf(taken.to)) {
        const Operation& narrowed = m_code.at(id);
        if (narrowed.opcode != Opcode::Narrowed || !narrowed.isOnTakenEdge() || narrowed.testOperand() != test.id() || narrowed.valueOperand() != test->valueOperand())
            continue;
        ValueID user = m_code.onlyUserOf(id);
        if (!user || m_code.at(user).opcode != Opcode::Unbox || m_code.at(user).representation != Representation::Double)
            continue;
        if (generator().keepsValuesInHeapFrame()) {
            if (m_code.blockOf(user) != taken.to)
                continue;
            bool maySuspendBetween = false;
            for (ValueID between : m_code.valuesOf(taken.to))
                maySuspendBetween |= between > id && between < user && m_code.at(between).traits().maySuspend();
            if (maySuspendBetween)
                continue;
        }
        return user;
    }
    return { };
}

ALWAYS_INLINE_INTO_OTHER_FILES bool CastEmitter::tryBranchOnNumberAndHandItOn(const ValueContext& test, Operand operand)
{
    if (!operand.is<Boxed>() || !(typeOf(test->valueOperand()).primitives & Primitive::Integer))
        return false;
    ValueID unboxing = unboxingToDoubleBehindTestForNumber(test);
    if (!unboxing)
        return false;

    Boxed value = operand.as<Boxed>();
    BranchTargets targets = test.branchTargets();
    LBasicBlock isTagged = newBlock();
    LBasicBlock isNotTagged = newBlock();
    LBasicBlock isDouble = newBlock();
    LValue number = m_out.phiIn(targets.taken.block(), Machine::Double);
    m_out.branch(isNotInt32(value).value(), normalEdge(isNotTagged), normalEdge(isTagged));

    m_out.appendTo(isTagged);
    m_out.upsilon(toNumber(unboxInt32(value)).value(), number);
    m_out.jump(targets.taken);

    m_out.appendTo(isNotTagged);
    m_out.branch(isNumber(value).value(), normalEdge(isDouble), targets.notTaken);

    m_out.appendTo(isDouble);
    m_out.upsilon(unboxDouble(value).value(), number);
    m_out.jump(targets.taken);

    m_integersHandedOn.set(unboxing.index(), Operand(Number(number)));
    build().statistics().count(StatisticID::UnboxingsFusedIntoTypeChecks);
    return true;
}

ValueID CastEmitter::primitiveTakenForBits(const ValueContext& test)
{
    const Terminator& end = m_code.at(test.block()).terminator;
    if (test->testedType() != TypeArena::fromPrimitives(Primitive::NumberLike) || end.kind != TerminatorKind::Branch || end.conditionOperand() != test.id() || generator().keepsValuesInHeapFrame())
        return { };
    const Edge& taken = m_code.at(end.taken());
    if (!taken.arguments().empty() || generator().singlePredecessorOf(taken.to) != test.block() || generator().leavesTurnLaidOut(taken.to))
        return { };
    for (ValueID id : m_code.valuesOf(taken.to)) {
        const Operation& narrowed = m_code.at(id);
        if (narrowed.opcode != Opcode::Narrowed || !narrowed.isOnTakenEdge() || narrowed.testOperand() != test.id() || narrowed.valueOperand() != test->valueOperand() || narrowed.representation != Representation::JSValue)
            continue;
        ValueID user = m_code.useCountOf(id) == 1 ? m_code.onlyUserOf(id) : ValueID();
        bool takesBits = user && allBitwiseOperators.contains(m_code.at(user).opcode) && m_code.at(user).opcode != Opcode::BitNot && m_code.at(user).form() == OperatorForm::Numbers && m_code.blockOf(user) == taken.to;
        return takesBits ? id : ValueID();
    }
    return { };
}

bool CastEmitter::tryBranchOnBitsOfPrimitive(const ValueContext& test, Operand operand)
{
    const Type& known = typeOf(test->valueOperand());
    if (!operand.is<Boxed>() || emittersCheckProofs() || !(known.primitives & Primitive::Integer) || (known.primitives & Primitive::Unspecified))
        return false;
    ValueID primitive = primitiveTakenForBits(test);
    if (!primitive)
        return false;

    Boxed value = operand.as<Boxed>();
    BranchTargets targets = test.branchTargets();
    LBasicBlock isTagged = newBlock();
    LBasicBlock isNotTagged = newBlock();
    LBasicBlock isNoCell = newBlock();
    LBasicBlock isDouble = newBlock();
    LBasicBlock isOther = newBlock();
    LValue bits = m_out.phiIn(targets.taken.block(), Machine::Int32);
    m_out.branch(isInt32(value).value(), normalEdge(isTagged), normalEdge(isNotTagged));

    m_out.appendTo(isTagged);
    m_out.upsilon(unboxInt32(value).value(), bits);
    m_out.jump(targets.taken);

    m_out.appendTo(isNotTagged);
    m_out.branch(isCell(value).value(), targets.notTaken, normalEdge(isNoCell));

    m_out.appendTo(isNoCell);
    m_out.branch(belowOrEqual(value, undefined()).value(), normalEdge(isOther), normalEdge(isDouble));

    m_out.appendTo(isOther);
    m_out.upsilon(emitter<ConversionEmitter>().bitsOfTag(value).value(), bits);
    m_out.jump(targets.taken);

    m_out.appendTo(isDouble);
    m_out.upsilon(emitter<ConversionEmitter>().toInt32(unboxDouble(value)).value(), bits);
    m_out.jump(targets.taken);

    m_bitsHandedOn.set(primitive.index(), bits);
    build().statistics().count(StatisticID::UnboxingsFusedIntoTypeChecks);
    return true;
}

std::optional<Word32> CastEmitter::takeBitsHandedOnTo(ValueID primitive)
{
    LValue bits = m_bitsHandedOn.isEmpty() ? nullptr : m_bitsHandedOn.take(primitive.index());
    return bits ? std::optional(Word32(bits)) : std::nullopt;
}

ValueID CastEmitter::conversionAfterInt32Test(const ValueContext& test)
{
    ValueID truncation = test->valueOperand();
    if (test->testedType() != TypeTable::int32 || m_code.isConstant(truncation) || generator().keepsValuesInHeapFrame())
        return { };
    const Operation& truncated = m_code.at(truncation);
    if (truncated.opcode != Opcode::ToIntegerOrInfinity || truncated.representation != Representation::Double || representationOf(truncated.valueOperand()) != Representation::Double)
        return { };
    const Edge& taken = m_code.at(m_code.at(test.block()).terminator.taken());
    if (!taken.arguments().empty() || generator().singlePredecessorOf(taken.to) != test.block() || generator().leavesTurnLaidOut(taken.to))
        return { };
    for (ValueID id : m_code.valuesOf(taken.to)) {
        const Operation& narrowed = m_code.at(id);
        if (narrowed.opcode != Opcode::Narrowed || !narrowed.isOnTakenEdge() || narrowed.testOperand() != test.id() || narrowed.valueOperand() != truncation)
            continue;
        ValueID conversion = m_code.onlyUserOf(id);
        if (conversion && m_code.at(conversion).opcode == Opcode::NumberToInteger)
            return conversion;
    }
    return { };
}

ValueID CastEmitter::integerAfterDoubleTest(const ValueContext& test)
{
    const Terminator& end = m_code.at(test.block()).terminator;
    if (test->testedType() != TypeTable::int32 || end.kind != TerminatorKind::Branch || end.conditionOperand() != test.id() || generator().keepsValuesInHeapFrame())
        return { };
    const Edge& taken = m_code.at(end.taken());
    if (!taken.arguments().empty() || generator().singlePredecessorOf(taken.to) != test.block() || generator().leavesTurnLaidOut(taken.to))
        return { };
    for (ValueID id : m_code.valuesOf(taken.to)) {
        const Operation& narrowed = m_code.at(id);
        if (narrowed.opcode != Opcode::Narrowed || !narrowed.isOnTakenEdge() || narrowed.testOperand() != test.id() || narrowed.valueOperand() != test->valueOperand())
            continue;
        ValueID conversion = m_code.onlyUserOf(id);
        if (conversion && m_code.at(conversion).opcode == Opcode::NumberToInteger && m_code.blockOf(conversion) == taken.to)
            return conversion;
    }
    return { };
}

bool CastEmitter::tryBranchOnWholeDouble(const ValueContext& test, Operand operand)
{
    if (operand.representation != Representation::Double)
        return false;
    ValueID conversion = integerAfterDoubleTest(test);
    if (!conversion)
        return false;
    BranchTargets targets = test.branchTargets();
    Word32 whole = emitter<ConversionEmitter>().int32OrElse(operand.as<Number>(), targets.notTaken.block());
    m_out.jump(targets.taken);
    m_integersHandedOn.set(conversion.index(), Operand(whole));
    return true;
}

bool CastEmitter::tryBranchOnTruncatedInt32(const ValueContext& test, Operand operand)
{
    if (operand.representation != Representation::Double)
        return false;
    ValueID conversion = conversionAfterInt32Test(test);
    if (!conversion)
        return false;

    Number number = in<Number>(m_code.at(test->valueOperand()).valueOperand());
    Integer truncated = emitter<ConversionEmitter>().truncateUnchecked(number);
    Word32 word = low32(truncated);
    BranchTargets targets = test.branchTargets();
    LBasicBlock isInt32 = newBlock();
    LBasicBlock isOther = newBlock();
    LBasicBlock isNaN = newBlock();
    bool isWide = m_code.at(conversion).representation == Representation::Int64;
    LValue integer = m_out.phiIn(targets.taken.block(), isWide ? Machine::Int64 : Machine::Int32);
    m_out.branch(equal(signExtended(word), truncated).value(), normalEdge(isInt32), rareEdge(isOther));

    m_out.appendTo(isInt32);
    m_out.upsilon(isWide ? truncated.value() : word.value(), integer);
    m_out.jump(targets.taken);

    m_out.appendTo(isOther);
    m_out.branch(equal(number, number).value(), targets.notTaken, rareEdge(isNaN));

    m_out.appendTo(isNaN);
    m_out.upsilon(isWide ? m_out.constInt64(0) : m_out.constInt32(0), integer);
    m_out.jump(targets.taken);

    m_integersHandedOn.set(conversion.index(), isWide ? Operand(Integer(integer)) : Operand(Word32(integer)));
    return true;
}

ValueID CastEmitter::sumAfterNumberTest(const ValueContext& test)
{
    if (test->testedType() != TypeTable::number || generator().keepsValuesInHeapFrame())
        return { };
    auto isNumberTested = [&](ValueID id) {
        const Operation& narrowed = m_code.at(id);
        return narrowed.opcode == Opcode::Narrowed && narrowed.isOnTakenEdge() && narrowed.testOperand() == test.id() && narrowed.valueOperand() == test->valueOperand();
    };
    auto isBoxedSumTerm = [&](ValueID id) {
        return representationOf(id) == Representation::JSValue || representationOf(id) == Representation::Int32;
    };
    ValueID number;
    BlockID from = test.block();
    ValueID condition = test.id();
    for (bool isAfterOtherTest : { false, true }) {
        const Terminator& end = m_code.at(from).terminator;
        if (end.kind != TerminatorKind::Branch || end.conditionOperand() != condition)
            return { };
        const Edge& passes = m_code.at(end.taken());
        const Edge& fails = m_code.at(end.notTaken());
        if (!passes.arguments().empty() || !fails.arguments().empty() || m_code.at(fails.to).frequency != BlockFrequency::Rare || generator().singlePredecessorOf(passes.to) != from || generator().leavesTurnLaidOut(passes.to))
            return { };
        ValueID next;
        for (ValueID id : m_code.valuesOf(passes.to)) {
            if (m_code.at(id).opcode != Opcode::Narrowed) {
                next = id;
                break;
            }
            if (isNumberTested(id)) {
                if (number)
                    return { };
                number = id;
            }
        }
        if (!next || !number)
            return { };
        const Operation& operation = m_code.at(next);
        if (operation.opcode == Opcode::Add || operation.opcode == Opcode::Sub) {
            bool takesNumber = operation.leftOperand() == number || operation.rightOperand() == number;
            bool isBoxedSum = operation.form() == OperatorForm::Numbers && operation.representation == Representation::JSValue && isBoxedSumTerm(operation.leftOperand()) && isBoxedSumTerm(operation.rightOperand());
            return takesNumber && isBoxedSum ? next : ValueID();
        }
        bool testsOtherOperand = operation.opcode == Opcode::TypeTest && operation.testedType() == TypeTable::number && m_code.useCountOf(number) == 1;
        if (isAfterOtherTest || !testsOtherOperand)
            return { };
        from = passes.to;
        condition = next;
    }
    return { };
}

bool CastEmitter::tryDeferTestToSum(const ValueContext& test, Operand operand)
{
    if (!operand.is<Boxed>() || !(typeOf(test->valueOperand()).primitives & Primitive::Integer) || AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckEffectsAtRunTime))
        return false;
    ValueID sum = sumAfterNumberTest(test);
    if (!sum)
        return false;
    BranchTargets targets = test.branchTargets();
    m_numberChecksOfSums.ensure(sum.index(), [] { return Vector<DeferredNumberCheck, 2>(); }).iterator->value.append({ operand.as<Boxed>(), targets.notTaken });
    m_out.jump(targets.taken);
    build().statistics().count(StatisticID::NumberTestsDeferredToSums);
    return true;
}

Vector<CastEmitter::DeferredNumberCheck, 2> CastEmitter::takeNumberChecksOf(ValueID sum)
{
    return m_numberChecksOfSums.isEmpty() ? Vector<DeferredNumberCheck, 2>() : m_numberChecksOfSums.take(sum.index());
}

std::optional<Operand> CastEmitter::takeIntegerHandedOnTo(ValueID unboxing)
{
    return m_integersHandedOn.takeOptional(unboxing.index());
}

FunctionID CastEmitter::singleTestedClosure(ValueID id)
{
    const Operation& test = m_code.at(id);
    if (test.opcode != Opcode::TypeTest || types().at(test.testedType()).primitives)
        return { };
    ValueID source = test.valueOperand();
    Representation representation = representationOf(source);
    if (representation != Representation::JSValue && (representation != Representation::Cell || test.asksInFull()))
        return { };
    bool mayBeHole = test.asksInFull() ? !hasTestedForHole(operationAt(definitionOf(source))) || generator().holeIsLeftToTestBehind(definitionOf(source)) : typeOf(source).primitives & Primitive::Absent;
    if (mayBeHole)
        return { };
    const TypeTestPlan& plan = test.asksInFull() ? typeTestPlanFor(TypeTable::anything, representation, test.testedType()) : planFor(source, test.testedType());
    bool testsSingleClosure = plan.verdict == TypeTestVerdict::NeedsTest && !plan.passingPrimitives && plan.stringLiterals.isEmpty() && plan.objects == ObjectsVerdict::NeedTest && plan.cellTests.size() == 1 && plan.cellTests[0].kind == CellTestKind::IsClosureOf;
    return testsSingleClosure && emitter<ClosureTestEmitter>().closureWordNames(plan.cellTests[0].function) ? plan.cellTests[0].function : FunctionID();
}

bool CastEmitter::tryBeginClosureChain(const ValueContext& test)
{
    FunctionID function = singleTestedClosure(test.id());
    if (!function || generator().keepsValuesInHeapFrame() || generator().isLayingOutTurns() || generator().testOfBranch(test.block()) != test.id())
        return false;
    auto bringsNothing = [&](EdgeID id) {
        const Edge& edge = m_code.at(id);
        return edge.arguments().empty() && m_code.at(edge.to).parameters().empty();
    };
    struct TestInChain {
        BlockID block;
        FunctionID function;
    };
    Vector<TestInChain, 4> tests;
    BlockID block = test.block();
    for (ValueID asking = test.id();;) {
        const Terminator& end = m_code.at(block).terminator;
        if (!bringsNothing(end.taken()) || !bringsNothing(end.notTaken()))
            break;
        tests.append({ block, function });
        BlockID next = m_code.at(end.notTaken()).to;
        if (generator().singlePredecessorOf(next) != block || m_code.at(next).handler != m_code.at(block).handler)
            break;
        ValueID nextTest = generator().testOfBranch(next);
        std::span<const ValueID> values = m_code.valuesOf(next);
        if (!nextTest || values.back() != nextTest)
            break;
        auto isNarrowedByFailure = [&](ValueID id) {
            const Operation& narrowed = m_code.at(id);
            return narrowed.opcode == Opcode::Narrowed && !narrowed.isOnTakenEdge() && narrowed.testOperand() == asking && narrowed.valueOperand() == m_code.at(asking).valueOperand() && narrowed.representation == representationOf(narrowed.valueOperand());
        };
        ValueID asked = m_code.at(nextTest).opcode == Opcode::TypeTest ? m_code.at(nextTest).valueOperand() : ValueID();
        bool testsSameValue = asked && (asked == m_code.at(asking).valueOperand() || (!m_code.isConstant(asked) && m_code.blockOf(asked) == next && isNarrowedByFailure(asked)));
        if (!testsSameValue || !std::ranges::all_of(values.first(values.size() - 1), isNarrowedByFailure))
            break;
        function = singleTestedClosure(nextTest);
        if (!function || std::ranges::any_of(tests, [&](const TestInChain& earlier) { return earlier.function == function; }))
            break;
        asking = nextTest;
        block = next;
    }
    if (tests.size() < 2)
        return false;

    const Edge& otherwise = m_code.at(m_code.at(tests.last().block).terminator.notTaken());
    uint32_t number = m_closureChains.size();
    Vector<ClosureTestEmitter::ClosureCase, 4> cases;
    for (uint32_t place = 0; place < tests.size(); ++place) {
        const Edge& taken = m_code.at(m_code.at(tests[place].block).terminator.taken());
        cases.append({ tests[place].function, generator().edgeWithoutArguments(taken) });
        generator().expectOtherJumps(taken.to);
        if (place)
            m_closureChainPositions.set(tests[place].block.index(), ClosureChainPosition { number, place });
    }
    generator().expectOtherJumps(otherwise.to);

    Machine::DeclaredAccessesSuspension suspension(m_out);
    BranchTargets targets = test.branchTargets();
    ValueID source = test->valueOperand();
    bool isBoxed = representationOf(source) == Representation::JSValue;
    if (isBoxed && (test->asksInFull() || (typeOf(source).primitives & ~cellPrimitives))) {
        LBasicBlock isCellBlock = newBlock();
        m_out.branch(isCell(in<Boxed>(source)).value(), normalEdge(isCellBlock), generator().edgeWithoutArguments(otherwise));
        m_out.appendToContinuation(isCellBlock);
    }
    CellPointer closure = isBoxed ? unboxCell(in<Boxed>(source)) : in<CellPointer>(source);
    Word32 word = emitter<ClosureTestEmitter>().upperHalfOfWordAtSlimClosureWord(closure);
    m_out.branch(equal(word, emitter<ClosureTestEmitter>().upperHalfOfClosureWordOf(tests[0].function)).value(), targets.taken, targets.notTaken);
    m_closureChains.append({ closure, word, WTF::move(cases) });
    build().statistics().count(StatisticID::ClosureTestsChained);
    return true;
}

bool CastEmitter::tryContinueClosureChain(const ValueContext& test)
{
    if (m_closureChains.isEmpty())
        return false;
    auto place = m_closureChainPositions.find(test.block().index());
    if (place == m_closureChainPositions.end() || generator().isLayingOutTurns())
        return false;
    const ClosureChain& chain = m_closureChains[place->value.chain];
    const ClosureTestEmitter::ClosureCase& closureCase = chain.cases[place->value.place];
    bool isLast = place->value.place + 1 == chain.cases.size();
    Machine::DeclaredAccessesSuspension suspension(m_out);
    BranchTargets targets = test.branchTargets();
    LBasicBlock hasOtherWord = isLast ? newBlock() : nullptr;
    m_out.branch(equal(chain.word, emitter<ClosureTestEmitter>().upperHalfOfClosureWordOf(closureCase.function)).value(), targets.taken, isLast ? normalEdge(hasOtherWord) : targets.notTaken);
    if (!isLast)
        return true;
    m_out.appendToContinuation(hasOtherWord);
    emitter<ClosureTestEmitter>().branchOnChangedClosure(chain.closure, chain.cases.span(), targets.notTaken);
    return true;
}

const Operation* CastEmitter::throwAfterFailedTest(const ValueContext& test)
{
    if (!isX86_64() || !test->asksInFull() || !test.isBranchCondition() || sourcesOfProofsCheckedUnder(build().options().testing) || !generator().mayThrowInPlace())
        return nullptr;
    const Edge& failing = m_code.at(m_code.at(test.block()).terminator.notTaken());
    std::span<const ValueID> values = m_code.valuesOf(failing.to);
    if (!failing.arguments().empty() || generator().singlePredecessorOf(failing.to) != test.block() || m_code.at(failing.to).terminator.kind != TerminatorKind::Unreachable || values.empty())
        return nullptr;
    auto asksValueOfTest = [&](const Operation& operation) {
        return operation.valueOperand() == test->valueOperand() && operation.origin().inlineFrame == test->origin().inlineFrame;
    };
    if (m_code.at(test.block()).handler || m_code.at(failing.to).handler)
        return nullptr;
    const Operation& coercion = m_code.at(values.back());
    if (coercion.opcode != Opcode::RequireObjectCoercible || !asksValueOfTest(coercion) || coercion.origin().position != test->origin().position)
        return nullptr;
    bool areOthersChecks = std::ranges::all_of(values.first(values.size() - 1), [&](ValueID id) {
        return m_code.at(id).opcode == Opcode::CheckProof && asksValueOfTest(m_code.at(id));
    });
    return areOthersChecks ? &coercion : nullptr;
}

bool CastEmitter::tryBranchOnTypeOfCell(const ValueContext& test, Operand operand)
{
    if (!operand.is<Boxed>())
        return false;
    const Operation* coercion = throwAfterFailedTest(test);
    if (!coercion)
        return false;
    TypeID cells = types().withoutPrimitives(TypeTable::anything, static_cast<uint16_t>(~cellPrimitives));
    const TypeTestPlan& plan = typeTestPlanFor(cells, Representation::JSValue, test->testedType());
    if ((types().at(test->testedType()).primitives & ~cellPrimitives) || plan.verdict != TypeTestVerdict::NeedsTest || !plan.stringLiterals.isEmpty())
        return false;
    if (!emitter<ThrowEmitter>().trapIfNullish(test, operand.as<Boxed>(), coercion->nullishAccess(), coercion->name()))
        return false;
    branchOnType(operand, plan, test.branchTargets());
    return true;
}

static bool isUsuallyComputed(const Code& code, ValueID value)
{
    const Operation& definition = code.at(value);
    if (definition.opcode != Opcode::Parameter)
        return allNumericOperators.contains(definition.opcode);
    BlockID join = definition.block();
    if (code.at(join).isLoopHead || code.at(join).isEntry)
        return false;
    std::span<const ValueID> parameters = code.at(join).parameters();
    size_t position = std::ranges::find(parameters, value) - parameters.begin();
    bool isComputed = false;
    bool bringsOther = position >= parameters.size();
    code.forEachIncomingEdgeUnsealed(join, [&](const Edge& edge) {
        if (bringsOther || code.at(edge.from).frequency == BlockFrequency::Rare || position >= edge.arguments().size())
            return;
        bool isOperator = allNumericOperators.contains(code.at(edge.arguments()[position]).opcode);
        isComputed |= isOperator;
        bringsOther |= !isOperator;
    });
    return isComputed && !bringsOther;
}

void CastEmitter::generateTypeTest(const ValueContext& value)
{
    if (value.isBranchCondition() && (tryContinueClosureChain(value) || tryBeginClosureChain(value)))
        return;
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    if (value->asksInFull())
        operand = Operand { Representation::JSValue, operand.value };
    if (LValue untagged = generator().takeUntaggedElement(value.id())) {
        emitter<ConversionEmitter>().noteSignExtended(operand.as<Word32>(), Integer(untagged));
        branch(belowOrEqual(Integer(untagged), integer(types().at(value->testedType()).integers.maximum)), value.branchTargets());
        return;
    }
    if (tryBranchOnTypeOfCell(value, operand))
        return;
    bool holeIsLeftToThisTest = generator().holeIsLeftToTestBehind(definitionOf(source));
    SetForScope wordAsked(generator().wordWhoseHoleIsLeftToTestAtWork(), holeIsLeftToThisTest ? operand.value : nullptr);
    TypeID whatMayCome = hasTestedForHole(operationAt(definitionOf(source))) && !holeIsLeftToThisTest ? TypeTable::anything : TypeTable::anythingOrAbsent;
    bool passesWhateverComes = false;
    if (uint32_t word = build().options().understatedEffects) [[unlikely]] {
        UnderstatedEffects row = UnderstatedEffects::decode(word);
        CodeUnit unit = generator().unit();
        passesWhateverComes = row.part == PartOfEffects::TestsOfGuard && unit.role == CodeRole::ParameterGuard && unit.function.index() == row.row;
    }
    const TypeTestPlan& plan = passesWhateverComes ? typeTestPlanFor(whatMayCome, Representation::JSValue, whatMayCome) : value->asksInFull() ? typeTestPlanFor(whatMayCome, Representation::JSValue, value->testedType()) : planFor(source, value->testedType());
    if (!passesWhateverComes && !value->asksInFull() && operand.is<Boxed>() && tryTestSlotWord(value, operand.as<Boxed>(), plan))
        return;
    if (!value.isBranchCondition()) {
        define(value, hasType(operand, plan));
        return;
    }
    if (!passesWhateverComes && !value->asksInFull() && (tryBranchOnInt32AndHandItOn(value, operand) || tryBranchOnNumberAndHandItOn(value, operand) || tryBranchOnBitsOfPrimitive(value, operand) || tryBranchOnTruncatedInt32(value, operand) || tryBranchOnWholeDouble(value, operand) || tryDeferTestToSum(value, operand)))
        return;
    SetForScope testsNumbersFirst(emitter<TypeTestEmitter>().testsNumbersFirst(), operand.is<Boxed>() && (plan.passingPrimitives & Primitive::Number) && plan.someCellPasses() && isUsuallyComputed(m_code, definitionOf(source)));
    bool testsForArrayAlone = !passesWhateverComes && plan.verdict == TypeTestVerdict::NeedsTest && !plan.passingPrimitives && plan.objects == ObjectsVerdict::NeedTest && plan.cellTests.size() == 1 && plan.cellTests[0].kind == CellTestKind::CellTypeIs && plan.cellTests[0].cellTypes.first == ArrayType && plan.cellTests[0].cellTypes.last == ArrayType;
    std::optional<ArrayHeaderTest> headerTest;
    if (ValueID pristineTest = testsForArrayAlone ? generator().findPristineTestAfter(value.id()) : ValueID(); pristineTest && (isX86_64() || !generator().leadsToInnerLoop(value.block()))) {
        headerTest = ArrayHeaderTest { };
        ValueID storageTest = generator().findStorageTestAfter(pristineTest, true);
        if (storageTest)
            headerTest->addStorage(operationAt(storageTest).arrayStorage(), operationAt(storageTest).isWritable());
        generator().noteHeaderTested(pristineTest, value.branchTargets().taken.block(), !!storageTest);
    }
    SetForScope headerTested(emitter<TypeTestEmitter>().arrayHeaderTest(), headerTest);
    branchOnType(operand, plan, value.branchTargets());
}

void CastEmitter::generateIsUnlockedInstance(const ValueContext& value)
{
    bool mayBeLocked = hasLockableInstances(m_program, m_facts.linked(), value->testedType());
    Operand operand = get(value->valueOperand());
    if (value.isBranchCondition()) {
        if (mayBeLocked)
            branch(boolean(false), value.branchTargets());
        else
            branchOnType(operand, planFor(value->valueOperand(), value->testedType()), value.branchTargets());
        return;
    }
    if (mayBeLocked)
        define(value, boolean(false));
    else
        define(value, hasType(operand, planFor(value->valueOperand(), value->testedType())));
}

auto CastEmitter::questionForStubThatTells(TypeID type) -> std::optional<QuestionForStub>
{
    if (!types().at(type).isJavaScriptValue())
        return std::nullopt;
    const TypeTestPlan& plan = typeTestPlanFor(TypeTable::anythingOrAbsent, Representation::JSValue, type);
    if (plan.verdict == TypeTestVerdict::AlwaysPasses)
        return QuestionForStub { checkProofOfKinds(), KindsOfProof::all, std::nullopt, std::nullopt };
    if (plan.verdict == TypeTestVerdict::NeverPasses)
        return std::nullopt;

    uint16_t numbers = plan.passingPrimitives & Primitive::Number;
    bool kindsTellPrimitives = !plan.integers && plan.stringLiterals.isEmpty() && (!numbers || numbers == Primitive::Number);
    uint32_t kinds = plan.passingPrimitives & KindsOfProof::primitives;
    if (plan.integers || numbers != Primitive::Number)
        kinds &= ~KindsOfProof::everyNumber;
    if (!plan.stringLiterals.isEmpty())
        kinds &= ~Primitive::String;
    if (plan.objects == ObjectsVerdict::AllPass)
        kinds |= KindsOfProof::everyObject;

    auto hasKind = [](CellTestKind kind) {
        return [kind](const CellTest& test) {
            return test.kind == kind;
        };
    };
    if (kindsTellPrimitives && plan.objects != ObjectsVerdict::NeedTest && std::ranges::all_of(plan.cellTests, hasKind(CellTestKind::IsObject)))
        return QuestionForStub { checkProofOfKinds(), kinds, std::nullopt, std::nullopt };
    if (kindsTellPrimitives && plan.cellTests.size() == 1 && plan.cellTests[0].kind == CellTestKind::IsClosureOf) {
        if (std::optional<uint32_t> mention = emitter<ClosureTestEmitter>().mentionOfUpperHalfThatNames(plan.cellTests[0].function))
            return QuestionForStub { checkProofOfClosure(), kinds, mention, std::nullopt };
    }

    bool testInLineIsShort = kindsTellPrimitives && plan.cellTests.size() == 1 && plan.cellTests[0].kind != CellTestKind::IsClosureOf;
    bool stubAnswersItself = std::ranges::count_if(plan.cellTests, hasKind(CellTestKind::IsClosureOf)) <= 1 && std::ranges::all_of(plan.cellTests, [&](const CellTest& test) {
        return test.kind == CellTestKind::IsObject || test.kind == CellTestKind::IsCallable || test.kind == CellTestKind::CellTypeIs || test.kind == CellTestKind::IsClosureOf || (test.kind == CellTestKind::ShapeIn && hasOneInstance());
    });
    if (testInLineIsShort || !stubAnswersItself || !recordInTablesTellsType(types(), type))
        return std::nullopt;
    return QuestionForStub { checkProofOfType(), kinds, std::nullopt, m_mentions.add(NumberSpace::Type, type.bits()) };
}

void CastEmitter::checkBooleanOrBranch(const ValueContext& test, ValueID checkID)
{
    const Operation& check = generator().code().at(checkID);
    Boxed found = in<Boxed>(test->valueOperand());
    LBasicBlock stops = newBlock();
    LBasicBlock continuation = test.isBranchCondition() ? nullptr : newBlock();
    BlockEdge hasCheckedType = continuation ? normalEdge(continuation) : test.branchTargets().notTaken;
    if (!continuation) {
        BranchTargets targets = test.branchTargets();
        LBasicBlock isNotFalse = newBlock();
        LBasicBlock isNoBoolean = newBlock();
        m_out.branch(equal(found, boxedConstant(jsBoolean(false))).value(), targets.notTaken, normalEdge(isNotFalse));
        m_out.appendTo(isNotFalse);
        m_out.branch(equal(found, boxedConstant(jsBoolean(true))).value(), targets.taken, rareEdge(isNoBoolean));
        m_out.appendTo(isNoBoolean);
    }
    emitter<TypeTestEmitter>().emitPerFunctionBranch(found, typeTestPlanFor(check.typeWithoutProof(), Representation::JSValue, check.checkedType()), { hasCheckedType, rareEdge(stops) });
    m_out.appendTo(stops);
    emitter<ThrowEmitter>().crashOnFalseProof(test, found, check.source(), FalseProofWord::summarize(types().at(check.checkedType()).primitives, types().at(check.checkedType()).hasObjects()));
    if (continuation)
        m_out.appendTo(continuation);
}

TypeID CastEmitter::typeOfCellsToCheck(const ValueContext& value)
{
    const Type& checked = types().at(value->checkedType());
    if (!isX86_64() || !checked.hasCells() || (checked.primitives & ~cellPrimitives))
        return { };
    TypeID cells = types().withoutPrimitives(value->typeWithoutProof(), static_cast<uint16_t>(~cellPrimitives));
    if (cells == value->typeWithoutProof() || isNeverType(cells))
        return { };
    const TypeTestPlan& plan = typeTestPlanFor(cells, Representation::JSValue, value->checkedType());
    return plan.verdict == TypeTestVerdict::NeedsTest && plan.stringLiterals.isEmpty() && !plan.everyCellPasses() ? cells : TypeID();
}

void CastEmitter::generateCheckProof(const ValueContext& value)
{
    if (value->isLeftToTestBehind())
        return;
    Operand source = get(value->valueOperand());
    uint32_t expectedType = FalseProofWord::summarize(types().at(value->checkedType()).primitives, types().at(value->checkedType()).hasObjects());
    if (numberRepresentations.contains(source.representation) || optionalRepresentations.contains(source.representation)) {
        const TypeTestPlan& plan = typeTestPlanFor(value->typeWithoutProof(), source.representation, value->checkedType());
        if (!optionalRepresentations.contains(source.representation) || plan.verdict != TypeTestVerdict::NeedsTest || generator().isInitializingCell()) {
            emitter<ThrowEmitter>().checkOrCrash(value, hasType(source, plan), value->source(), expectedType);
            return;
        }
        LBasicBlock stops = newBlock();
        LBasicBlock continuation = newBlock();
        ScopeFork fork = m_out.forkScope();
        branchOnType(source, plan, { normalEdge(continuation), rareEdge(stops) });
        m_out.appendToJoin(stops, fork);
        emitter<ThrowEmitter>().crashOnFalseProof(value, std::nullopt, value->source(), expectedType);
        m_out.appendToJoin(continuation, fork);
        return;
    }
    if (source.representation == Representation::Cell32) {
        NarrowReference word = source.as<NarrowReference>();
        const Type& checked = types().at(value->checkedType());
        Condition isWordOfCheckedType = checked.hasCells() ? isCellReferenceAmongAllWords(word) : boolean(false);
        for (auto [primitive, marker] : { std::pair { Primitive::Undefined, undefinedCell32 }, std::pair { Primitive::Null, nullCell32 }, std::pair { Primitive::Absent, absentCell32 } }) {
            if (checked.primitives & primitive)
                isWordOfCheckedType = logicalOr(isWordOfCheckedType, equal(Word32(word.value()), word32(static_cast<int32_t>(marker))));
        }
        emitter<ThrowEmitter>().requireOrCrash(value, isWordOfCheckedType, std::nullopt, value->source(), expectedType);
        source = emitter<ConversionEmitter>().decode(word, types().at(TypeTable::cellOrMarker), Representation::JSValue);
    }
    bool isAnswerOfTest = source.representation == Representation::Boolean;
    Boxed found(isAnswerOfTest ? m_out.constInt64(JSValue::encode(JSValue())) : source.value);
    if (isAnswerOfTest) {
        emitter<ThrowEmitter>().requireOrCrash(value, Condition(source.value), std::nullopt, value->source());
        return;
    }
    bool asksForType = value->checkedType() != value->typeWithoutProof();
    bool looksAtCell = sourcesOfProofsCheckedUnder(build().options().testing) && types().at(value->checkedType()).isJavaScriptValue();
    if (!asksForType && !looksAtCell)
        return;
    if (!looksAtCell && generator().deferCheckToTruthTest(value))
        return;
    bool asksStub = looksAtCell && !generator().isInitializingCell();
    if (asksStub) {
        if (std::optional<QuestionForStub> asked = asksForType ? questionForStubThatTells(value->checkedType()) : std::optional(QuestionForStub { checkProofOfKinds(), KindsOfProof::all, std::nullopt, std::nullopt })) {
            emitter<ThrowEmitter>().stopUnlessStubPasses(value, found, asked->stub, asked->kinds, value->source(), asked->mentionAboveKinds, asked->numberInRegister);
            return;
        }
    }
    LBasicBlock stops = newBlock();
    LBasicBlock continuation = newBlock();
    ScopeFork fork = m_out.forkScope();
    if (asksForType) {
        LBasicBlock hasCheckedType = looksAtCell ? newBlock() : continuation;
        TypeID typeWithoutProof = value->typeWithoutProof();
        if (TypeID cells = looksAtCell ? TypeID() : typeOfCellsToCheck(value); cells && emitter<ThrowEmitter>().trapUnlessCell(value, found, value->source(), expectedType))
            typeWithoutProof = cells;
        else
            generator().noteWordAskedByMask(found.value());
        emitter<TypeTestEmitter>().emitPerFunctionBranch(found, typeTestPlanFor(typeWithoutProof, Representation::JSValue, value->checkedType()), { normalEdge(hasCheckedType), rareEdge(stops) });
        if (looksAtCell)
            m_out.appendToJoin(hasCheckedType, fork);
    }
    if (asksStub) {
        emitter<ThrowEmitter>().stopUnlessStubPasses(value, found, checkProofOfKinds(), KindsOfProof::all, value->source());
        m_out.jump(continuation);
    } else if (looksAtCell) {
        LBasicBlock isCellToLookAt = newBlock();
        m_out.branch(logicalOr(logicalNot(isCell(found)), isEmpty(found)).value(), normalEdge(continuation), normalEdge(isCellToLookAt));
        m_out.appendToJoin(isCellToLookAt, fork);
        CellPointer cell = unboxCell(found);
        m_out.branch(logicalOr(hasCellTypeIn(cell, { StringType, SymbolType }), isObjectCell(cell)).value(), normalEdge(continuation), rareEdge(stops));
    }
    m_out.appendToJoin(stops, fork);
    emitter<ThrowEmitter>().crashOnFalseProof(value, found, value->source(), expectedType);
    m_out.appendToJoin(continuation, fork);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
