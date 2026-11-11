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
        if (LValue word = isNotUndefined ? emitter<SlotEmitter>().findWordOfSlot(operand.value) : nullptr)
            operand = Boxed(word);
    }
    defineNarrowed(value, operand);
}

bool CastEmitter::tryTestWordOfSlot(const ValueContext& test, Boxed value, const TypeTestPlan& plan)
{
    constexpr int64_t bitsOutsideUndefined = ~static_cast<int64_t>(JSValue::ValueUndefined);
    LValue word = emitter<SlotEmitter>().findWordOfSlot(value.value());
    if (!word || !Machine::isEncodableBitImmediate(Width64, bitsOutsideUndefined))
        return false;
    bool asksForNothing = plan.verdict == TypeTestVerdict::NeedsTest && (plan.passingPrimitives & Primitive::Undefined) && !(plan.passingPrimitives & ~Primitive::UndefinedOrNull) && !plan.integers && plan.stringLiterals.isEmpty() && plan.cellTests.isEmpty() && plan.objects != ObjectsVerdict::AllPass;
    if (!asksForNothing)
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

ALWAYS_INLINE_INTO_OTHER_FILES ValueID CastEmitter::unboxingBehindTestForInt32(const ValueContext& test)
{
    const Terminator& end = m_code.at(test.block()).terminator;
    if (test->testedType() != TypeTable::int32 || end.kind != TerminatorKind::Branch || end.conditionOperand() != test.id())
        return { };
    const Edge& taken = m_code.at(end.taken());
    if (!taken.arguments().empty() || generator().singlePredecessorOf(taken.to) != test.block() || generator().leavesTurnLaidOut(taken.to))
        return { };
    for (ValueID id : m_code.valuesOf(taken.to)) {
        const Operation& unboxing = m_code.at(id);
        if (unboxing.opcode != Opcode::Unbox || (unboxing.representation != Representation::Int32 && unboxing.representation != Representation::Int64))
            continue;
        const Operation& narrowed = m_code.at(unboxing.valueOperand());
        if (narrowed.opcode == Opcode::Narrowed && narrowed.isOnTakenEdge() && narrowed.testOperand() == test.id() && narrowed.valueOperand() == test->valueOperand())
            return id;
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
    LBasicBlock isTagged = newBlock();
    LBasicBlock isNotTagged = newBlock();
    LBasicBlock isDouble = newBlock();
    bool isWide = m_code.at(unboxing).representation == Representation::Int64;
    LValue integer = m_out.phiIn(targets.taken.block(), isWide ? Machine::Int64 : Machine::Int32);
    auto handOn = [&](Word32 word) {
        m_out.upsilon(isWide ? signExtended(word).value() : word.value(), integer);
        m_out.jump(targets.taken);
    };
    m_out.branch(isInt32(value).value(), normalEdge(isTagged), rareEdge(isNotTagged));

    m_out.appendTo(isTagged);
    handOn(unboxInt32(value));

    m_out.appendTo(isNotTagged);
    m_out.branch(isNumber(value).value(), normalEdge(isDouble), targets.notTaken);

    m_out.appendTo(isDouble);
    handOn(emitter<ConversionEmitter>().int32OrElse(unboxDouble(value), targets.notTaken.block()));

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
    m_out.branch(isInt32(value).value(), normalEdge(isTagged), normalEdge(isNotTagged));

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

ValueID CastEmitter::sumBehindTestForNumber(const ValueContext& test)
{
    if (test->testedType() != TypeTable::number || generator().keepsValuesInHeapFrame())
        return { };
    auto isNumberTested = [&](ValueID id) {
        const Operation& narrowed = m_code.at(id);
        return narrowed.opcode == Opcode::Narrowed && narrowed.isOnTakenEdge() && narrowed.testOperand() == test.id() && narrowed.valueOperand() == test->valueOperand();
    };
    auto isTermOfBoxedSum = [&](ValueID id) {
        return representationOf(id) == Representation::JSValue || representationOf(id) == Representation::Int32;
    };
    ValueID number;
    BlockID from = test.block();
    ValueID condition = test.id();
    for (bool isBehindOtherTest : { false, true }) {
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
            bool isBoxedSum = operation.form() == OperatorForm::Numbers && operation.representation == Representation::JSValue && isTermOfBoxedSum(operation.leftOperand()) && isTermOfBoxedSum(operation.rightOperand());
            return takesNumber && isBoxedSum ? next : ValueID();
        }
        bool isTestOfOtherOperand = operation.opcode == Opcode::TypeTest && operation.testedType() == TypeTable::number && m_code.useCountOf(number) == 1;
        if (isBehindOtherTest || !isTestOfOtherOperand)
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
    ValueID sum = sumBehindTestForNumber(test);
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

FunctionID CastEmitter::closureThatTestAsksFor(ValueID id)
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
    bool asksForOneClosure = plan.verdict == TypeTestVerdict::NeedsTest && !plan.passingPrimitives && plan.stringLiterals.isEmpty() && plan.objects == ObjectsVerdict::NeedTest && plan.cellTests.size() == 1 && plan.cellTests[0].kind == CellTestKind::IsClosureOf;
    return asksForOneClosure && emitter<ClosureTestEmitter>().closureWordNames(plan.cellTests[0].function) ? plan.cellTests[0].function : FunctionID();
}

bool CastEmitter::tryBeginClosureChain(const ValueContext& test)
{
    FunctionID function = closureThatTestAsksFor(test.id());
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
        bool asksSameValue = asked && (asked == m_code.at(asking).valueOperand() || (!m_code.isConstant(asked) && m_code.blockOf(asked) == next && isNarrowedByFailure(asked)));
        if (!asksSameValue || !std::ranges::all_of(values.first(values.size() - 1), isNarrowedByFailure))
            break;
        function = closureThatTestAsksFor(nextTest);
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
            m_placesInClosureChains.set(tests[place].block.index(), PlaceInClosureChain { number, place });
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
    auto place = m_placesInClosureChains.find(test.block().index());
    if (place == m_placesInClosureChains.end() || generator().isLayingOutTurns())
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

void CastEmitter::generateTypeTest(const ValueContext& value)
{
    if (value.isBranchCondition() && (tryContinueClosureChain(value) || tryBeginClosureChain(value)))
        return;
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    if (value->asksInFull())
        operand = Operand { Representation::JSValue, operand.value };
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
    if (!passesWhateverComes && !value->asksInFull() && operand.is<Boxed>() && tryTestWordOfSlot(value, operand.as<Boxed>(), plan))
        return;
    if (!value.isBranchCondition()) {
        define(value, hasType(operand, plan));
        return;
    }
    if (!passesWhateverComes && !value->asksInFull() && (tryBranchOnInt32AndHandItOn(value, operand) || tryBranchOnNumberAndHandItOn(value, operand) || tryDeferTestToSum(value, operand)))
        return;
    branchOnType(operand, plan, value.branchTargets());
}

void CastEmitter::generateIsUnlockedInstance(const ValueContext& value)
{
    bool mayBeLocked = instancesMayBeLocked(m_program, m_facts.linked(), value->testedType());
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

void CastEmitter::generateCheckProof(const ValueContext& value)
{
    if (value->isLeftToTestBehind())
        return;
    Operand source = get(value->valueOperand());
    uint32_t expectedType = FalseProofWord::summarize(types().at(value->checkedType()).primitives, types().at(value->checkedType()).hasObjects());
    if (numberRepresentations.contains(source.representation) || optionalRepresentations.contains(source.representation)) {
        emitter<ThrowEmitter>().checkOrCrash(value, hasType(source, typeTestPlanFor(value->typeWithoutProof(), source.representation, value->checkedType())), value->source(), expectedType);
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
    bool looksAtCell = sourcesOfProofsCheckedUnder(build().options().testing);
    if (!asksForType && !looksAtCell)
        return;
    if (!looksAtCell && generator().leaveCheckToTruthTest(value))
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
        emitter<TypeTestEmitter>().emitPerFunctionBranch(found, typeTestPlanFor(value->typeWithoutProof(), Representation::JSValue, value->checkedType()), { normalEdge(hasCheckedType), rareEdge(stops) });
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
