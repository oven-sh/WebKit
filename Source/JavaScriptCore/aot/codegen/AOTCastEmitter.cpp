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
#include "AOTOpcodeSets.h"
#include "AOTStubsClosures.h"
#include "AOTStubsValues.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestPlan.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

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

ValueID CastEmitter::unboxingBehindTestForInt32(const ValueContext& test)
{
    const Terminator& end = m_code.at(test.block()).terminator;
    if (test->testedType() != TypeTable::int32 || end.kind != TerminatorKind::Branch || end.conditionOperand() != test.id())
        return { };
    const Edge& taken = m_code.at(end.taken());
    if (!taken.arguments().empty() || generator().singlePredecessorOf(taken.to) != test.block())
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

bool CastEmitter::tryBranchOnInt32AndHandItOn(const ValueContext& test, Operand operand)
{
    if (!operand.is<Boxed>())
        return false;
    ValueID unboxing = unboxingBehindTestForInt32(test);
    if (!unboxing)
        return false;
    RELEASE_ASSERT(!m_integersHandedOn.containsIf([&](const IntegerHandedOn& handedOn) {
        return handedOn.unboxing == unboxing;
    }));

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

    m_integersHandedOn.append({ unboxing, isWide ? Operand(Integer(integer)) : Operand(Word32(integer)) });
    build().statistics().count(StatisticID::IntegersHandedOnByTheirTests);
    return true;
}

std::optional<Operand> CastEmitter::takeIntegerHandedOnTo(ValueID unboxing)
{
    size_t place = m_integersHandedOn.findIf([&](const IntegerHandedOn& handedOn) {
        return handedOn.unboxing == unboxing;
    });
    if (place == notFound)
        return std::nullopt;
    Operand integer = m_integersHandedOn[place].integer;
    m_integersHandedOn.removeAt(place);
    return integer;
}

void CastEmitter::generateTypeTest(const ValueContext& value)
{
    ValueID source = value->valueOperand();
    Operand operand = get(source);
    if (value->asksInFull())
        operand = Operand { Representation::JSValue, operand.value };
    TypeID whatMayCome = hasTestedForHole(operationAt(definitionOf(source))) ? TypeTable::anything : TypeTable::anythingOrAbsent;
    bool passesWhateverComes = false;
    if (uint32_t word = build().options().rowThatSaysLess) [[unlikely]] {
        RowThatSaysLess row = RowThatSaysLess::decode(word);
        CodeUnit unit = generator().unit();
        passesWhateverComes = row.part == PartOfEffects::TestsOfGuard && unit.role == CodeRole::ParameterGuard && unit.function.index() == row.row;
    }
    const TypeTestPlan& plan = passesWhateverComes ? typeTestPlanFor(whatMayCome, Representation::JSValue, whatMayCome) : value->asksInFull() ? typeTestPlanFor(whatMayCome, Representation::JSValue, value->testedType()) : planFor(source, value->testedType());
    if (!value.isBranchCondition()) {
        define(value, hasType(operand, plan));
        return;
    }
    if (!passesWhateverComes && !value->asksInFull() && tryBranchOnInt32AndHandItOn(value, operand))
        return;
    branchOnType(operand, plan, value.branchTargets());
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

void CastEmitter::generateCheckProof(const ValueContext& value)
{
    if (value->isLeftToTestBehind())
        return;
    Operand source = get(value->valueOperand());
    if (numberRepresentations.contains(source.representation) || optionalRepresentations.contains(source.representation)) {
        emitter<ThrowEmitter>().checkOrStopAtFalseProof(value, hasType(source, typeTestPlanFor(value->typeWithoutProof(), source.representation, value->checkedType())), value->source());
        return;
    }
    bool isAnswerOfTest = source.representation == Representation::Boolean;
    Boxed found(isAnswerOfTest ? m_out.constInt64(JSValue::encode(JSValue())) : source.value);
    if (isAnswerOfTest) {
        emitter<ThrowEmitter>().requireOrStopAtFalseProof(value, Condition(source.value), std::nullopt, value->source());
        return;
    }
    bool asksForType = value->checkedType() != value->typeWithoutProof();
    bool looksAtCell = sourcesOfProofsCheckedUnder(build().options().testing);
    if (!asksForType && !looksAtCell)
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
        branchOnType(found, typeTestPlanFor(value->typeWithoutProof(), Representation::JSValue, value->checkedType()), { normalEdge(hasCheckedType), rareEdge(stops) });
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
    emitter<ThrowEmitter>().stopAtFalseProof(value, found, value->source());
    m_out.appendToJoin(continuation, fork);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
