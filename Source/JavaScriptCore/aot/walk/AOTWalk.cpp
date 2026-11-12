/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ASAN_ENABLED
#include <sanitizer/asan_interface.h>
#endif

#if ENABLE(AOT)

#include "AOTAccessPlan.h"
#include "AOTArrayStorageForms.h"
#include "AOTBuiltinMembers.h"
#include "AOTCallingConventions.h"
#include "AOTEffectsNet.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkTables.h"
#include "AOTModuleEvaluationPlan.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTOperatorForms.h"
#include "AOTPlan.h"
#include "AOTPrimitives.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTSumOverRecords.h"
#include "AOTSweep.h"
#include "AOTTypeNarrowing.h"
#include "AOTTypeText.h"
#include "AOTWalkCountersOfLoops.h"
#include "AOTWalkCoroutinePositions.h"
#include "AOTWalkCoroutinePromises.h"
#include "AOTWalkCoroutineCalls.h"
#include "AOTWalkLoopsOverGenerators.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkTextBuilders.h"
#include "AOTWalkTextBuilderRows.h"
#include "AOTWalkTestRuns.h"
#include "AOTWalkNumberForms.h"
#include "AOTWalkStringFolds.h"
#include "AOTWalkKnownCalls.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListedOwnKeys.h"
#include "AOTWalkResolvedCalls.h"
#include "AOTWalkBirths.h"
#include "AOTWalkWrittenDescriptors.h"
#include <algorithm>
#include <wtf/DataLog.h>
#include <wtf/HashFunctions.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

bool buildReadWhereSomeReceiversLackField(Walker&, const BodyValue&);

static constexpr OpcodeSet callsWithTailFormInWalk { Opcode::Call, Opcode::CallList, Opcode::CallDirect, Opcode::CallTable };

namespace {

enum class WhoAnswersInWalk : uint8_t { LinkedTablesUnread, LinkedTables, HeldTablesUnread, HeldTables, Nobody };
static constexpr unsigned numberWhoAnswerInWalk = static_cast<unsigned>(WhoAnswersInWalk::Nobody) + 1;
static constexpr unsigned numberOfQuestionsAtTwoLevelsInWalk = 4;
static constexpr unsigned numberOfQuestionsAtOneLevelInWalk = 3;
static_assert(static_cast<unsigned>(AnswerKeptInArena::FirstOfWalk) + (2 * numberOfQuestionsAtTwoLevelsInWalk + numberOfQuestionsAtOneLevelInWalk) * numberWhoAnswerInWalk == static_cast<unsigned>(AnswerKeptInArena::LastOfWalk) + 1);
static_assert(2 + static_cast<unsigned>(PositionTypes::QuestionOfEachLayout::ResultOfTableRow) + 1 == numberOfQuestionsAtTwoLevelsInWalk + numberOfQuestionsAtOneLevelInWalk);
static_assert(2 + static_cast<unsigned>(PositionTypes::QuestionOfEachLayout::FoundAboveObjects) == numberOfQuestionsAtTwoLevelsInWalk && 2 + static_cast<unsigned>(PositionTypes::QuestionOfEachLayout::ReadByNameOffObjects) == numberOfQuestionsAtTwoLevelsInWalk + 1);

template<typename SetID>
static TypeID askOnceForBuild(TypeArena& types, WhoAnswersInWalk who, unsigned question, TypeLevel level, SetID set, uint32_t word, NOESCAPE const ScopedLambda<TypeID()>& askEachMember)
{
    if (types.keeps() != ArenaKeeps::UntilReset)
        return askEachMember();
    unsigned place = 2 * numberOfQuestionsAtTwoLevelsInWalk * static_cast<unsigned>(who) + 2 * question + (level == TypeLevel::Expected ? 1 : 0);
    if (question >= numberOfQuestionsAtTwoLevelsInWalk) {
        RELEASE_ASSERT(level == TypeLevel::Proven);
        place = 2 * numberOfQuestionsAtTwoLevelsInWalk * numberWhoAnswerInWalk + numberOfQuestionsAtOneLevelInWalk * static_cast<unsigned>(who) + question - numberOfQuestionsAtTwoLevelsInWalk;
    }
    uint8_t number = static_cast<uint8_t>(static_cast<unsigned>(AnswerKeptInArena::FirstOfWalk) + place);
    TypeArena::SetOfRound round = types.setOfRound(set);
    if (std::optional<TypeArena::WordsOfRound> kept = types.rememberedRound(number, 1, round, word))
        return TypeID::fromBits((*kept)[0]);
    TypeID answer = askEachMember();
    types.rememberRound(number, 1, answer ? 1 : 0, round, word, { answer.bits() });
    return answer;
}

class PositionsKnowingNothingInWalk final : public PositionTypes {
    WTF_MAKE_NONCOPYABLE(PositionsKnowingNothingInWalk);
public:
    PositionsKnowingNothingInWalk(const Program& program, const Plan& plan, TypeArena& types)
        : m_program(program)
        , m_plan(plan)
        , m_types(types)
    {
    }

    TypeID variable(TypeLevel, VariableID) const final { return { }; }
    TypeID environmentField(TypeLevel, LayoutID, uint32_t) const final { return { }; }
    TypeID slot(TypeLevel, LayoutID, uint32_t) const final { return { }; }
    TypeID property(TypeLevel, ObjectID, PropertyKeyID) const final { return { }; }
    TypeID result(TypeLevel, FunctionID, uint32_t) const final { return { }; }
    TypeID entryParameter(TypeLevel, FunctionID, uint32_t) const final { return { }; }
    TypeID resultOfCallBeingTyped(TypeLevel, uint32_t) const final { return { }; }
    TypeID askedOfEachObject(QuestionOfEachObject question, TypeLevel level, ObjectSetID objects, uint32_t word, NOESCAPE const ScopedLambda<TypeID()>& askEachObject) const final { return askOnceForBuild(m_types, WhoAnswersInWalk::Nobody, static_cast<unsigned>(question), level, objects, word, askEachObject); }
    TypeID askedOfEachLayout(QuestionOfEachLayout question, TypeLevel level, LayoutSetID layouts, PropertyKeyID key, NOESCAPE const ScopedLambda<TypeID()>& askEachLayout) const final { return askOnceForBuild(m_types, WhoAnswersInWalk::Nobody, 2 + static_cast<unsigned>(question), level, layouts, key.bits(), askEachLayout); }

    bool collectQueriedLayouts(PropertyKeyID key, LayoutSetID layouts, bool includesInheritedWrites, Vector<LayoutID, 16>& asked) const final
    {
        uint32_t numberInUse = 0;
        return layouts && !holdsOneMember(layouts) && m_plan.slots().listMembersAskedAbout(m_program, key, layouts, static_cast<SlotPlan::WhatIsWrittenAbove>(includesInheritedWrites), asked, numberInUse);
    }

private:
    const Program& m_program;
    const Plan& m_plan;
    TypeArena& m_types;
};

class PositionsKnowingWhatFunctionsHandBackInWalk final : public PositionTypes {
    WTF_MAKE_NONCOPYABLE(PositionsKnowingWhatFunctionsHandBackInWalk);
public:
    explicit PositionsKnowingWhatFunctionsHandBackInWalk(const PositionTypes& positionsOfBuild)
        : m_positionsOfBuild(positionsOfBuild)
    {
    }

    TypeID variable(TypeLevel, VariableID) const final { return { }; }
    TypeID environmentField(TypeLevel, LayoutID, uint32_t) const final { return { }; }
    TypeID slot(TypeLevel, LayoutID, uint32_t) const final { return { }; }
    TypeID property(TypeLevel, ObjectID, PropertyKeyID) const final { return { }; }
    TypeID result(TypeLevel level, FunctionID function, uint32_t resultIndex) const final { return m_positionsOfBuild.result(level, function, resultIndex); }
    TypeID entryParameter(TypeLevel, FunctionID, uint32_t) const final { return { }; }
    TypeID resultOfCallBeingTyped(TypeLevel, uint32_t) const final { return { }; }
    bool collectQueriedLayouts(PropertyKeyID key, LayoutSetID layouts, bool includesInheritedWrites, Vector<LayoutID, 16>& asked) const final { return m_positionsOfBuild.collectQueriedLayouts(key, layouts, includesInheritedWrites, asked); }

private:
    const PositionTypes& m_positionsOfBuild;
};

class PositionsOfWalk final : public PositionTypes {
    WTF_MAKE_NONCOPYABLE(PositionsOfWalk);
public:
    PositionsOfWalk(const Program& program, const Facts& facts, const LinkTables& tables, const Plan& plan, TypeArena& types, bool readsTables, bool believesTables = true, const TypeID* resultOfCallBeingTyped = nullptr, const TypeID* resultOfReadBeingTyped = nullptr, const TypeID* contentsBeingTyped = nullptr)
        : m_program(program)
        , m_facts(facts)
        , m_linked(tables)
        , m_plan(plan)
        , m_types(types)
        , m_readsTables(readsTables)
        , m_believesTables(believesTables)
        , m_whoAnswers(static_cast<WhoAnswersInWalk>((&tables == &facts.held() ? 2 : 0) + (readsTables ? 1 : 0)))
        , m_resultOfCallBeingTyped(resultOfCallBeingTyped)
        , m_resultOfReadBeingTyped(resultOfReadBeingTyped)
        , m_contentsBeingTyped(contentsBeingTyped)
    {
    }

    TypeID askedOfEachObject(QuestionOfEachObject question, TypeLevel level, ObjectSetID objects, uint32_t word, NOESCAPE const ScopedLambda<TypeID()>& askEachObject) const final { return !m_believesTables ? askEachObject() : askOnceForBuild(m_types, m_whoAnswers, static_cast<unsigned>(question), level, objects, word, askEachObject); }
    TypeID askedOfEachLayout(QuestionOfEachLayout question, TypeLevel level, LayoutSetID layouts, PropertyKeyID key, NOESCAPE const ScopedLambda<TypeID()>& askEachLayout) const final { return !m_believesTables ? askEachLayout() : askOnceForBuild(m_types, m_whoAnswers, 2 + static_cast<unsigned>(question), level, layouts, key.bits(), askEachLayout); }

    void noteThatNoTextWasEvaluated() { m_noTextWasEvaluated = true; }

    bool holdsOneSymbolForGood(VariableID variable) const
    {
        const LinkedCell& cell = m_linked.variables[variable.index()];
        if (!cell.proven || !cell.everyWriterIsKnown || !m_program.at(variable).flags.contains(VariableFlags::IsImmutable) || m_plan.runTimeMayFill(variable))
            return false;
        Type type = m_types.at(m_types.withoutPrimitives(cell.proven, Primitive::Absent));
        return type.primitives == Primitive::LiteralSymbol && !type.hasObjects() && holdsOneMember(type.symbolLiterals);
    }

    TypeID variable(TypeLevel level, VariableID variable) const final
    {
        if (const PromotedVariable* promoted = m_plan.promotedEnvironments().find(variable))
            return typeOfPromotedVariable(level, *promoted);
        if (TypeID known = m_noTextWasEvaluated && level == TypeLevel::Proven ? m_facts.beforeEvaluation().typeOf(variable) : TypeID(); known && !m_plan.deferredDefinitions().findDeferred(variable))
            return known;
        if (!m_believesTables && variable.index() < m_program.variables.size() && m_program.at(variable).declared.isEstablished())
            return { };
        bool isFilledByCompiledCodeAlone = !m_plan.runTimeMayFill(variable);
        TypeID proven = variable.index() < m_linked.variables.size() && isFilledByCompiledCodeAlone ? filledByCompiledCode(level, &m_linked.variables[variable.index()]) : TypeID();
        if (!proven && !m_readsTables && level == TypeLevel::Proven && variable.index() < m_linked.variables.size() && (m_plan.variable(variable).representation != SlotRepresentation::JSValue || holdsOneSymbolForGood(variable)))
            proven = m_linked.variables[variable.index()].proven;
        if (proven && !m_believesTables) {
            VariableInitialization born = m_program.at(variable).initialization;
            proven = m_types.withPrimitives(proven, born == VariableInitialization::Uninitialized ? Primitive::Absent : born == VariableInitialization::Undefined ? Primitive::Undefined : 0);
        }
        const DeferredVariable* deferred = level == TypeLevel::Proven ? m_plan.deferredDefinitions().findDeferred(variable) : nullptr;
        if (!deferred)
            return proven;
        if (!proven)
            proven = m_types.withPrimitives(TypeTable::anything, m_program.at(variable).initialization == VariableInitialization::Uninitialized ? Primitive::Absent : 0);
        return m_types.withPrimitives(proven, deferred->mark);
    }

    TypeID environmentField(TypeLevel level, LayoutID layout, uint32_t field) const final
    {
        if (const Vector<Field>& fields = m_program.at(layout).fields; !m_believesTables && field < fields.size() && fields[field].declared.isEstablished())
            return { };
        if (TypeID known = m_noTextWasEvaluated && level == TypeLevel::Proven ? m_facts.beforeEvaluation().typeOf(FieldReference { layout, field }) : TypeID())
            return known;
        TypeID proven = filledByCompiledCode(level, cellAt(m_linked.firstFieldOfLayout, m_linked.fields, layout.index(), field));
        if (!proven && !m_readsTables && level == TypeLevel::Proven && m_plan.slot(layout, field).hasStorage() && m_plan.slot(layout, field).representation != SlotRepresentation::JSValue)
            proven = m_facts.linked().provenOfField({ layout, field });
        return proven && !m_believesTables ? m_types.withPrimitives(proven, Primitive::Absent) : proven;
    }

    bool environmentLiesAmongVariables(LayoutID layout) const final { return m_plan.promotedEnvironments().isPromoted(layout); }

    TypeID slot(TypeLevel level, LayoutID layout, uint32_t field) const final
    {
        if (!m_readsTables)
            return { };
        if (!m_plan.isInUse(layout))
            return TypeTable::never;
        const PlacedSlot& placed = m_plan.slot(layout, field);
        if (!placed.hasStorage())
            return { };
        if (placed.type && m_believesTables)
            return placed.type;
        const Vector<Field>& fields = m_program.at(layout).fields;
        if (placed.holdsAccessor || (field < fields.size() && (fields[field].form.contains(FieldForm::IsAccessor) || fields[field].flags.contains(FieldFlags::IsBrand))))
            return { };
        return readOfSlot(level, layout, field) ? TypeID(TypeTable::anything) : TypeID();
    }

    TypeID readOfSlot(TypeLevel level, LayoutID layout, uint32_t field) const final
    {
        if (&m_linked != &m_facts.linked())
            return provenOf(level, m_linked.findCellOfField({ layout, field }));
        if (!m_readsTables || !m_believesTables || level != TypeLevel::Proven)
            return { };
        const PlacedSlot& placed = m_plan.slot(layout, field);
        if (placed.provenType || placed.hasStorage())
            return placed.provenType;
        const Vector<Field>& fields = m_program.at(layout).fields;
        if (field >= fields.size() || !fields[field].flags.contains(FieldFlags::IsPrivate) || fields[field].flags.containsAny({ FieldFlags::IsBrand, FieldFlags::IsHidden }) || fields[field].form.contains(FieldForm::IsAccessor))
            return { };
        return provenOf(level, m_linked.findCellOfField({ layout, field }));
    }
    TypeID halfOfAccessorOfField(TypeLevel level, LayoutID layout, uint32_t field, bool isSetter) const final
    {
        if (!m_readsTables || !m_believesTables || level != TypeLevel::Proven)
            return { };
        const LinkedAccessorsOfField* laid = m_linked.findAccessorsExpectedIn({ layout, field });
        if (!laid || !(isSetter ? laid->setterIsProven : laid->getterIsProven))
            return { };
        return m_types.forFunction(isSetter ? laid->setter : laid->getter);
    }
    bool collectQueriedLayouts(PropertyKeyID key, LayoutSetID layouts, bool includesInheritedWrites, Vector<LayoutID, 16>& asked) const final
    {
        uint32_t numberInUse = 0;
        return layouts && !holdsOneMember(layouts) && m_plan.slots().listMembersAskedAbout(m_program, key, layouts, static_cast<SlotPlan::WhatIsWrittenAbove>(includesInheritedWrites), asked, numberInUse);
    }
    std::optional<uint32_t> appendedField(LayoutID layout, PropertyKeyID key) const final
    {
        std::optional<FieldIndex> field = m_plan.slots().findField(m_program, layout, key);
        return field && *field >= m_program.at(layout).fields.size() ? field : std::nullopt;
    }
    TypeID foundAboveObjectsOf(TypeLevel level, LayoutID layout, PropertyKeyID key) const final
    {
        if (!m_readsTables || !m_believesTables || level != TypeLevel::Proven)
            return { };
        if (!m_plan.isInUse(layout))
            return TypeTable::never;
        return typeReadOffMember(level, findMemberAboveObjectsOf(m_program, m_linked, layout, key));
    }

    TypeID readByNameOffObjectsOf(TypeLevel level, LayoutID layout, PropertyKeyID key) const final
    {
        if (!m_readsTables || !m_believesTables || level != TypeLevel::Proven)
            return { };
        return m_plan.isInUse(layout) ? typeReadByNameOffObjectsOf(m_program, m_linked, &m_linked == &m_facts.linked() ? &m_plan : nullptr, m_types, layout, key) : TypeID(TypeTable::never);
    }

    LayoutID classAbove(LayoutID classLayout) const final { return m_readsTables && m_believesTables ? classProvenAbove(m_program, m_linked, classLayout) : LayoutID(); }

    TypeID property(TypeLevel level, ObjectID object, PropertyKeyID key) const final
    {
        if (!m_readsTables || level != TypeLevel::Proven)
            return { };
        if (TypeID read = m_believesTables ? typeReadOffMember(level, findMemberOfNamedObject(m_program, m_linked, m_types.objects().at(object), key)) : TypeID())
            return read;
        BuiltinID holder = m_readsTables && level == TypeLevel::Proven ? m_types.objects().builtinOf(object) : BuiltinID();
        BuiltinID member = holder ? findConstantMemberFrom(m_program, m_facts, holder, key) : BuiltinID();
        if (!member)
            return { };
        BuiltinKind kind = m_program.builtins().at(m_program.builtins().canonicalID(member)).kind;
        if (!isObjectKind(kind) && kind != BuiltinKind::Constant)
            return { };
        if (isMadeWithCodeOfLibrary(m_program.builtins(), member))
            return { };
        return typeOfBuiltinObject(TypingContext { m_program, m_types, *this }, member);
    }


    TypeID resultOfBodyThatNeverSuspends(TypeLevel level, FunctionID function) const final
    {
        if (!m_readsTables || !m_believesTables || level != TypeLevel::Proven)
            return { };
        return m_plan.typeHeldOfResult(m_program, m_linked, CodeUnit { function, CodeRole::CoroutineBody });
    }

    TypeID result(TypeLevel level, FunctionID function, uint32_t resultIndex) const final
    {
        if (level != TypeLevel::Proven || resultIndex)
            return { };
        if (TypeID known = m_noTextWasEvaluated ? m_facts.beforeEvaluation().resultOf(function) : TypeID(); known && !m_plan.resultsPassNoReturn(function) && m_plan.conventions().resultRepresentation({ function, CodeRole::Body }) == Representation::JSValue)
            return known;
        if (!m_readsTables && m_plan.conventions().resultRepresentation({ function, CodeRole::Body }) == Representation::JSValue)
            return { };
        return m_plan.typeHeldOfResult(m_program, m_linked, function);
    }

    TypeID entryParameter(TypeLevel level, FunctionID function, uint32_t entryParameter) const final { return level == TypeLevel::Proven ? m_plan.conventions().typeBehindGuard(function, entryParameter) : TypeID(); }
    bool entryParameterIsLeftOutOfCalls(FunctionID function, uint32_t entryParameter) const final { return m_plan.promotedEnvironments().isRemoved(function, entryParameter); }
    bool neverReturns(FunctionID function) const
    {
        const BodyIndex* body = m_facts.bodies().find(function);
        if (!body || m_program.at(function).isCoroutine())
            return false;
        for (uint32_t number = body->numberOfBlocks(); number--;) {
            TerminatorKind end = body->terminator(BlockID(number)).kind();
            if (end == TerminatorKind::Return || end == TerminatorKind::ReturnCall)
                return false;
        }
        return true;
    }
    TypeID resultOfSelector(TypeLevel, uint32_t selector) const final { return m_plan.conventions().methodSelector(selector).typeOfResult; }
    TypeID resultOfTableRow(TypeLevel level, uint32_t selector, LayoutID receiver, bool& hasRow) const final
    {
        const MethodTableRow* row = m_plan.conventions().findRowOfMethodTable(selector, receiver);
        hasRow = row;
        TypeID returned = row && !row->guardedClass ? result(level, row->method, 0) : TypeID();
        return !returned && row && !row->guardedClass && m_readsTables && m_believesTables && level == TypeLevel::Proven && neverReturns(row->method) ? TypeID(TypeTable::never) : returned;
    }
    TypeID capture(TypeLevel level, FunctionID function, uint32_t capture) const final
    {
        if (const auto& captures = m_program.at(function).captures; !m_believesTables && capture < captures.size() && captures[capture].isEstablished())
            return { };
        return filledByCompiledCode(level, cellAt(m_linked.firstCaptureOfFunction, m_linked.captures, function.index(), capture));
    }
    TypeID resultOfCallBeingTyped(TypeLevel level, uint32_t resultIndex) const final { return m_resultOfCallBeingTyped && level == TypeLevel::Proven && !resultIndex ? asFarAsDoorTellsClasses(*m_resultOfCallBeingTyped) : TypeID(); }
    TypeID resultOfReadBeingTyped(TypeLevel level) const final { return m_resultOfReadBeingTyped && level == TypeLevel::Proven ? asFarAsDoorTellsClasses(*m_resultOfReadBeingTyped) : TypeID(); }
    TypeID contentsBeingTyped(TypeLevel level) const final { return m_contentsBeingTyped && level == TypeLevel::Proven ? asFarAsDoorTellsClasses(*m_contentsBeingTyped) : TypeID(); }

private:
    TypeID typeOfPromotedVariable(TypeLevel level, const PromotedVariable& promoted) const
    {
        if (promoted.layout)
            return environmentField(level, promoted.layout, promoted.field);
        TypeID established = m_program.provenTypeOf(m_program.at(promoted.function).captures[promoted.capture]);
        TypeID proven = capture(level, promoted.function, promoted.capture);
        TypeID both = proven ? m_types.intersectionOf(established, proven) : TypeID();
        return both && !isNeverType(both) ? both : established;
    }

    TypeID typeReadOffMember(TypeLevel level, const MemberFound& found) const
    {
        switch (found.kind) {
        case MemberFound::Kind::Unknown:
            return { };
        case MemberFound::Kind::Method:
            return m_types.forFunction(found.method);
        case MemberFound::Kind::Accessor:
            return found.getter ? result(level, found.getter, 0) : TypeID(TypeTable::undefined);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static const LinkedCell* cellAt(const Vector<uint32_t>& firstOfOwner, const LargeList<LinkedCell>& cells, uint32_t owner, uint32_t index)
    {
        if (owner + 1 >= firstOfOwner.size() || index >= firstOfOwner[owner + 1] - firstOfOwner[owner])
            return nullptr;
        return &cells[firstOfOwner[owner] + index];
    }

    TypeID provenOf(TypeLevel level, const LinkedCell* cell) const
    {
        return m_believesTables ? filledByCompiledCode(level, cell) : TypeID();
    }

    ALWAYS_INLINE TypeID asFarAsDoorTellsClasses(TypeID proven) const
    {
        if (!proven || holdsOnlyPrimitives(proven))
            return proven;
        return asFarAsDoorTellsClassesOfObjects(proven);
    }

    NEVER_INLINE TypeID asFarAsDoorTellsClassesOfObjects(TypeID proven) const
    {
        if (!m_types.at(proven).isJavaScriptValue())
            return proven;
        TypeID testable = m_types.testablePartOf(proven);
        Type asked = m_types.at(testable);
        Type named = m_types.at(proven);
        return (asked.anyObject && !named.anyObject) || (asked.anyFunction && !named.anyFunction) ? testable : proven;
    }

    TypeID filledByCompiledCode(TypeLevel level, const LinkedCell* cell) const
    {
        return m_readsTables && level == TypeLevel::Proven && cell && cell->everyWriterIsKnown ? asFarAsDoorTellsClasses(cell->proven) : TypeID();
    }

    const Program& m_program;
    const Facts& m_facts;
    const LinkTables& m_linked;
    const Plan& m_plan;
    TypeArena& m_types;
    bool m_readsTables;
    bool m_believesTables;
    bool m_noTextWasEvaluated { false };
    WhoAnswersInWalk m_whoAnswers;
    const TypeID* m_resultOfCallBeingTyped;
    const TypeID* m_resultOfReadBeingTyped;
    const TypeID* m_contentsBeingTyped;
};

} // anonymous namespace

static ValueID valueBehindNamesInWalk(const Code& code, ValueID value)
{
    return code.valueBehindNames<valuePreservingOpcodes, Code::SourcesOfNames::StandEarlier>(value);
}

static ValueID slotReadBehindNamesInWalk(const Code& code, ValueID name)
{
    ValueID read = valueBehindNamesInWalk(code, name);
    return read && readsThatNarrowTheirHolders.contains(code.at(read).opcode) ? read : ValueID();
}

TypeID narrowOtherResultByTest(const TypingContext& context, const Code& code, ValueID test, ValueID value, EdgeSide side)
{
    ValueID read = valueBehindNamesInWalk(code, value);
    if (!read || code.at(test).opcode != Opcode::ResultAt || code.at(read).opcode != Opcode::ResultAt)
        return { };
    ValueID producer = code.at(test).resultsOperand();
    if (!producer || code.at(read).resultsOperand() != producer)
        return { };

    Vector<TypeID, 8> typesOfProducerOperands;
    for (ValueID operand : code.at(producer).operands())
        typesOfProducerOperands.append(operand ? code.at(operand).type : TypeID());
    return narrowResultByOtherResult(context, code.at(producer).opcode, OperandTypes { typesOfProducerOperands.span() }, code.at(test).index(), code.at(read).index(), code.at(value).type, side);
}

static bool caseEdgeEntersBlockInWalk(const Code& code, const Terminator& terminator, std::optional<uint32_t> caseIndex, BlockID block)
{
    if (!terminator.isSet || !terminator.hasCases() || !terminator.numberOfEdges)
        return false;
    if (caseIndex && *caseIndex >= terminator.numberOfCases())
        return false;
    return code.at(caseIndex ? terminator.caseEdge(*caseIndex) : terminator.otherwise()).to == block;
}

static const Terminator* findSwitchAboveBlockInWalk(const Code& code, BlockID block, std::optional<uint32_t> caseIndex)
{
    if (EdgeID first = code.at(block).firstEdgeIn()) {
        const Terminator& terminator = code.at(code.at(first).from).terminator;
        if (caseEdgeEntersBlockInWalk(code, terminator, caseIndex, block))
            return &terminator;
    }
    for (uint32_t index = block.index(); index--;) {
        const Terminator& terminator = code.at(BlockID(index)).terminator;
        if (caseEdgeEntersBlockInWalk(code, terminator, caseIndex, block))
            return &terminator;
    }
    for (uint32_t index = block.index() + 1; index < code.numberOfBlocks(); ++index) {
        const Terminator& terminator = code.at(BlockID(index)).terminator;
        if (caseEdgeEntersBlockInWalk(code, terminator, caseIndex, block))
            return &terminator;
    }
    return nullptr;
}

static TypeID narrowTermOfSumByTest(const TypingContext& context, const Code& code, const Operation& test, uint32_t operandIndex, ValueID term, EdgeSide side)
{
    static constexpr Range whereRoundingDecidesNothing { -maxExactInteger / 2, maxExactInteger / 2 };
    if (provesOf(test.opcode) != Proof::IntegerBound || operandIndex > 1 || !test.operand(0) || !test.operand(1))
        return { };
    const Operation& sum = code.at(valueBehindNamesInWalk(code, test.operand(operandIndex)));
    if (sum.opcode != Opcode::Add)
        return { };
    ValueID root = valueBehindNamesInWalk(code, term);
    bool isLeft = valueBehindNamesInWalk(code, sum.operand(0)) == root;
    if (isLeft == (valueBehindNamesInWalk(code, sum.operand(1)) == root))
        return { };
    TypeArena& types = context.types;
    TypeID typeOfTerm = code.at(term).type;
    TypeID typeOfOtherTerm = code.at(sum.operand(isLeft ? 1 : 0)).type;
    std::array<TypeID, 2> typesOfTestOperands { code.at(test.operand(0)).type, code.at(test.operand(1)).type };
    TypeID typeOfBound = typesOfTestOperands[1 - operandIndex];
    if (!typeOfTerm || !typeOfOtherTerm || !typeOfBound || !types.at(typeOfTerm).isInteger() || !types.at(typeOfOtherTerm).isInteger() || !types.at(typeOfBound).isInteger())
        return { };
    if (!types.at(typeOfBound).integers.isSubsetOf(whereRoundingDecidesNothing) || types.at(typeOfOtherTerm).integers.minimum < whereRoundingDecidesNothing.minimum)
        return { };
    typesOfTestOperands[operandIndex] = types.withIntegers(typeOfTerm, Range::exact());
    TypeID typeOfSum = narrowOperandByTest(context, test, OperandTypes { typesOfTestOperands }, operandIndex, side);
    if (!typeOfSum || !types.at(typeOfSum).isInteger() || types.at(typeOfSum).integers.maximum > whereRoundingDecidesNothing.maximum)
        return { };
    Range ofTerm = types.at(typeOfTerm).integers;
    int64_t greatest = types.at(typeOfSum).integers.maximum - types.at(typeOfOtherTerm).integers.minimum;
    if (greatest >= ofTerm.maximum || greatest < ofTerm.minimum)
        return { };
    return types.withIntegers(typeOfTerm, Range { ofTerm.minimum, greatest });
}

template<typename Record>
static std::optional<SecondCounter> secondCounterNarrowed(const Code& code, const Record& narrowed, ValueID value, ValueID test)
{
    if (provesOf(code.at(test).opcode) != Proof::IntegerBound)
        return std::nullopt;
    SecondCounter second;
    second.counter = valueBehindNamesInWalk(code, code.at(test).operand(narrowed.operandIndex() & 1));
    second.stepped = valueBehindNamesInWalk(code, value);
    if (!second.counter || !second.stepped || code.at(second.counter).opcode != Opcode::Parameter || code.at(second.stepped).opcode != Opcode::Parameter)
        return std::nullopt;
    second.steps = Range { -static_cast<int64_t>(narrowed.caseIndex().value_or(0)), static_cast<int64_t>(narrowed.operandIndex() >> 2) };
    second.counterRises = narrowed.operandIndex() & 2;
    return second;
}

template<typename Record>
static TypeID typeBesideCounter(TypeArena& types, const Code& code, const Record& narrowed, ValueID value, ValueID test, const SecondCounter& second, const EnteringCounters& entering)
{
    std::optional<Range> range = rangeOfSecondCounter(code, types, second, entering, code.at(test), narrowed.operandIndex() & 1, narrowed.isOnTakenEdge());
    TypeID typeOfValue = code.at(value).type;
    if (!range || !typeOfValue || !types.at(typeOfValue).isInteger())
        return { };
    Range held = types.at(typeOfValue).integers.intersectWith(*range);
    return held.isEmpty() ? TypeID() : types.withIntegers(typeOfValue, held);
}

static NEVER_INLINE TypeID finishedSecondCounterType(TypeArena& types, const Code& code, const Operation& narrowed)
{
    ValueID value = narrowed.valueOperand();
    ValueID test = narrowed.testOperand();
    std::optional<SecondCounter> second = test ? secondCounterNarrowed(code, narrowed, value, test) : std::nullopt;
    std::optional<EnteringCounters> entering = second ? findEnteringCounters(code, types, *second) : std::nullopt;
    TypeID shown = entering ? typeBesideCounter(types, code, narrowed, value, test, *second, *entering) : TypeID();
    bool isProven = shown && types.isSubset(shown, narrowed.type) && types.isSubset(narrowed.type, code.at(value).type);
    return isProven ? narrowed.type : shown;
}

template<typename Record>
static TypeID typeOfNarrowedRecordInWalk(const TypingContext& context, const Code& code, const Record& narrowed, ValueID value, ValueID test, BlockID block)
{
    if (!value)
        return { };
    TypeID typeOfValue = code.at(value).type;
    if (!typeOfValue)
        return { };
    EdgeSide side = narrowed.isOnTakenEdge() ? EdgeSide::Taken : EdgeSide::NotTaken;

    if (narrowed.narrowsOtherResultOfOperand()) {
        if (!test || narrowed.narrowsHolderOfOperand() || narrowed.operandIndex() != OperandIndex::ResultAt::results)
            return { };
        return narrowOtherResultByTest(context, code, test, value, side);
    }
    if (narrowed.narrowsTermOfOperand())
        return test && !narrowed.narrowsHolderOfOperand() ? narrowTermOfSumByTest(context, code, code.at(test), narrowed.operandIndex(), value, side) : TypeID();

    if (!test) {
        const Terminator* terminator = block ? findSwitchAboveBlockInWalk(code, block, narrowed.caseIndex()) : nullptr;
        if (!terminator)
            return { };
        if (!narrowed.narrowsHolderOfOperand())
            return narrowSubjectByCase(context, *terminator, typeOfValue, narrowed.caseIndex());

        if (terminator->kind == TerminatorKind::SwitchOnShape)
            return { };
        ValueID subject = terminator->subjectOperand();
        ValueID read = slotReadBehindNamesInWalk(code, subject);
        if (!read || !code.at(subject).type)
            return { };
        return narrowHolderOfSubjectByCase(context, *terminator, code.at(subject).type, code.at(read), typeOfValue, narrowed.caseIndex());
    }

    const Operation& testOperation = code.at(test);
    if (provesOf(testOperation.opcode) == Proof::None) {
        if (!narrowed.narrowsHolderOfOperand())
            return narrowByTruthiness(context, typeOfValue, side);
        ValueID read = slotReadBehindNamesInWalk(code, test);
        if (!read || !testOperation.type)
            return { };
        return narrowHolderByCondition(context, testOperation.type, code.at(read), typeOfValue, side);
    }

    uint32_t operandIndex = narrowed.operandIndex();
    if (operandIndex >= testOperation.numberOfOperands() || !testOperation.operand(operandIndex))
        return { };
    Vector<TypeID, 8> typesOfTestOperands;
    for (ValueID operand : testOperation.operands())
        typesOfTestOperands.append(operand ? code.at(operand).type : TypeID());
    TypeID inFrontOfTest = typesOfTestOperands[operandIndex];
    if (!inFrontOfTest)
        return { };

    if (narrowed.narrowsHolderOfOperand()) {
        ValueID read = slotReadBehindNamesInWalk(code, testOperation.operand(operandIndex));
        if (!read)
            return { };
        return narrowHolderByTest(context, testOperation, OperandTypes { typesOfTestOperands.span() }, operandIndex, code.at(read), typeOfValue, side);
    }

    typesOfTestOperands[operandIndex] = typeOfValue;
    TypeID type = narrowOperandByTest(context, testOperation, OperandTypes { typesOfTestOperands.span() }, operandIndex, side);
    if (side == EdgeSide::Taken || !type || typeOfValue == inFrontOfTest)
        return type;

    typesOfTestOperands[operandIndex] = inFrontOfTest;
    TypeID leftByRefusal = narrowOperandByTest(context, testOperation, OperandTypes { typesOfTestOperands.span() }, operandIndex, side);
    if (!leftByRefusal || !context.types.isSubset(context.types.intersectionOf(typeOfValue, leftByRefusal), type))
        return { };
    return type;
}

TypeID typeOfNarrowed(const TypingContext& context, const Code& code, ValueID narrowed)
{
    RELEASE_ASSERT(narrowed.index() < code.numberOfValues());
    const Operation& record = code.at(narrowed);
    RELEASE_ASSERT(record.opcode == Opcode::Narrowed);
    if (record.narrowsWithCounter()) [[unlikely]]
        return finishedSecondCounterType(context.types, code, record);
    return typeOfNarrowedRecordInWalk(context, code, record, record.valueOperand(), record.testOperand(), record.block());
}

static constexpr std::array<ResultRule, numberOfOpcodes> resultRulesByOpcodeInWalk = []() consteval {
    std::array<ResultRule, numberOfOpcodes> rules { };
    for (Opcode opcode : allMembersOf<Opcode>)
        rules[static_cast<unsigned>(opcode)] = resultRuleOf(opcode);
    return rules;
}();

static ALWAYS_INLINE ResultRule resultRuleInWalk(Opcode opcode)
{
    return resultRulesByOpcodeInWalk[static_cast<unsigned>(opcode)];
}

static_assert(!EffectTraits(Effects::none()).bits());

static constexpr OpcodeSet opcodesWithEffectsOfOperatorsInWalk = opcodesWhere([](Opcode opcode) { return effectRuleOf(opcode) == EffectRule::Operator; });
static constexpr OpcodeSet opcodesWhoseEffectsAreFoundByWalk = opcodesWhere([](Opcode opcode) { return effectRuleOf(opcode) != EffectRule::Declared; });

static constexpr OpcodeSet opcodesThatPollsAreToldOfInWalk = opcodesWhere([](Opcode opcode) {
    if (opcode == Opcode::LoopHint || opcode == Opcode::PropSet)
        return true;
    if (!runsOwnerBarrierOf(opcode))
        return false;
    Effects bound = declaredBoundOf(opcode);
    return bound.writes.contains(AbstractHeap::AllocatorState) && !bound.mayCollect && !bound.mayThrow;
});

template<typename Record>
static bool holdsNoneOfItsResultsInWalk(const Program& program, const Record& record)
{
    return opcodesThatMayHaveSeveralResults.contains(record.opcode) && numberOfResultsOf(program, record);
}

static void closeRoomOfWalker(void*);

KeptBetweenUnits::~KeptBetweenUnits()
{
#if ASAN_ENABLED
    __asan_unpoison_memory_region(roomsOfWalkers.data(), sizeof(roomsOfWalkers));
#endif
}

KeptBetweenUnits::KeptBetweenUnits(const WalkInputs& inputs, TypeArena& types)
    : arenaOfPositions(types)
    , positionsOfSlowBody(makeUniqueWithoutFastMallocCheck<PositionsOfWalk>(inputs.program, inputs.facts, inputs.facts.linked(), inputs.plan, types, false))
    , positionsKnowingNothing(makeUniqueWithoutFastMallocCheck<PositionsKnowingNothingInWalk>(inputs.program, inputs.plan, types))
    , heldPositions(makeUniqueWithoutFastMallocCheck<PositionsOfWalk>(inputs.program, inputs.facts, inputs.facts.held(), inputs.plan, types, withoutMechanismsMissingDependencies(inputs.options.mechanisms).contains(Mechanism::TypedOperations)))
    , mechanismsGiven(inputs.options.mechanisms)
    , mechanismsOfBuild(withoutMechanismsMissingDependencies(inputs.options.mechanisms))
{
    for (auto& room : roomsOfWalkers)
        closeRoomOfWalker(room.data());
}

Walker::Walker(const WalkInputs& inputs, TypeArena& types, Arrivals& arrivals, KeptBetweenUnits& kept)
    : methodsGivenUnderKeys(kept.methodsGivenUnderKeys)
    , receiversMadeInBodies(kept.receiversMadeInBodies)
    , gettersExpectedInFunction(kept.gettersExpectedInFunction)
    , m_keptByGeneratorsInLoops(kept.keptByGeneratorsInLoops)
    , m_keptByTypedAccesses(kept.keptByTypedAccesses)
    , m_keptByUnionTableCalls(kept.keptByUnionTableCalls)
    , m_keptByCopiesOwnKeys(kept.keptByCopiesOwnKeys)
    , m_keptByInstanceOf(kept.keptByInstanceOf)
    , m_keptByPositionsInTexts(kept.keptByPositionsInTexts)
    , m_roundsKept(kept.roundsKept)
    , m_listsOfNames(kept.listsOfNames)
    , m_numbersOfListsOfNames(kept.numbersOfListsOfNames)
    , m_surveysOfConstants(kept.surveysOfConstants)
    , m_constantsThatListFieldsHeldFilled(kept.constantsThatListFieldsHeldFilled)
    , m_accessesOfScopesByFunctions(kept.accessesOfScopesByFunctions)
    , m_isArrayIteratorProtocolIntact(kept.isArrayIteratorProtocolIntact)
    , m_objectPrototypeMayGainName(kept.objectPrototypeMayGainName)
    , m_block(kept.code)
    , m_numbersOfFramesOfFunctions(kept.numbersOfFramesOfFunctions)
    , m_workerHasBegunUnit(kept.workerHasBegunUnit)
    , m_newestWalkerOfWorker(kept.newestWalker)
    , m_pendingBlocks(kept.roomOfPendingBlocks.take())
    , m_bindings(kept.roomOfBindings.take())
    , m_unspecifiedConstants(kept.roomOfUnspecifiedConstants.take())
    , m_blocksWithFieldsOfScopesInOrder(kept.roomOfBlocksWithFieldsOfScopesInOrder.take())
    , m_preheaderDepths(kept.roomOfPreheaderDepths.take())
    , m_countsOfUnit(kept.roomOfCountsOfUnit.take())
    , m_trialVisits(kept.roomOfTrialVisits.take())
    , m_inputBlocksInTries(kept.roomOfInputBlocksInTries.take())
    , m_statesOfHandlersInTries(kept.roomOfStatesOfHandlersInTries.take())
    , m_placesOfOpenLoopsInTries(kept.roomOfPlacesOfOpenLoopsInTries.take())
    , m_inputHeadsOfIterationsAddedInTries(kept.roomOfInputHeadsOfIterationsAddedInTries.take())
    , m_typesChangedInTries(kept.roomOfTypesChangedInTries.take())
    , m_entriesOfCopiesOfLoopsInTries(kept.roomOfEntriesOfCopiesOfLoopsInTries.take())
    , m_preheaderDepthUndoLog(kept.roomOfPlacesOfLoopsChangedInTries.take())
    , m_sharedConstantsAddedInTries(kept.roomOfSharedConstantsAddedInTries.take())
    , m_copiesOfConstantsAddedInTries(kept.roomOfCopiesOfConstantsAddedInTries.take())
    , m_operationsBeingWalkedInFrames(kept.roomOfOperationsBeingWalkedInFrames.take())
    , m_frameLogSizes(kept.roomOfFrameLogSizes.take())
    , m_expectationsInTries(kept.roomOfExpectationsInTries.take())
    , m_headParametersWithSources(kept.roomOfHeadParametersWithSources.take())
    , m_stepsOfBuildByOpcode(kept.stepsOfBuildByOpcode)
    , m_stepsOfWalkByOpcode(kept.stepsOfWalkByOpcode)
    , m_code(kept.code)
    , m_inputs(inputs)
    , m_types(types)
    , m_arrivals(arrivals)
    , m_mechanismsOfBuild(kept.mechanismsOfBuild)
    , m_mechanisms(m_mechanismsOfBuild)
    , m_positionsOfBuild(makeUniqueWithoutFastMallocCheck<PositionsOfWalk>(inputs.program, inputs.facts, inputs.facts.linked(), inputs.plan, types, m_mechanismsOfBuild.contains(Mechanism::TypedOperations), true, &m_resultOfCallBeingTyped, &m_resultOfReadBeingTyped, &m_contentsBeingTyped))
    , m_positionsOfSlowBody(kept.positionsOfSlowBody)
    , m_positions(m_positionsOfBuild.get())
    , m_positionsKnowingNothing(kept.positionsKnowingNothing)
    , m_positionsKnowingWhatFunctionsHandBack(makeUniqueWithoutFastMallocCheck<PositionsKnowingWhatFunctionsHandBackInWalk>(*m_positionsOfBuild))
    , m_heldPositions(kept.heldPositions)
    , m_explainsClosedAccessesTo(inputs.options.explainsClosedAccessesTo)
{
    RELEASE_ASSERT(&kept.arenaOfPositions == &types && kept.mechanismsGiven == inputs.options.mechanisms);
    m_newestWalkerOfWorker = this;
    if (inputs.options.checksProofs)
        m_positionsWithoutProofs = makeUniqueWithoutFastMallocCheck<PositionsOfWalk>(inputs.program, inputs.facts, inputs.facts.linked(), inputs.plan, types, m_mechanismsOfBuild.contains(Mechanism::TypedOperations), false);
    m_state.setProgram(inputs.program);
    m_state.setSlotPlan(inputs.plan.slots());
    m_state.setNumbers(m_numbersOfStates);
    m_state.setAnswers(m_answersOfStates);
}

Walker::~Walker() = default;

static void closeRoomOfWalker(void* room)
{
#if ASSERT_ENABLED
    memset(room, byteOfRoomThatIsTaken, sizeof(Walker));
#endif
#if ASAN_ENABLED
    __asan_poison_memory_region(room, sizeof(Walker));
#endif
    UNUSED_PARAM(room);
}

WalkerInRoom KeptBetweenUnits::makeWalker(const WalkInputs& inputs, TypeArena& types, Arrivals& arrivals)
{
    static_assert(alignof(Walker) <= 16);
    size_t room = roomOfWalkerIsTaken[0];
    RELEASE_ASSERT(!std::exchange(roomOfWalkerIsTaken[room], true));
#if ASAN_ENABLED
    __asan_unpoison_memory_region(roomsOfWalkers[room].data(), sizeof(Walker));
#endif
    return WalkerInRoom(new (NotNull, roomsOfWalkers[room].data()) Walker(inputs, types, arrivals, *this), EndsWalkerInRoom { &roomOfWalkerIsTaken[room] });
}

void EndsWalkerInRoom::operator()(Walker* walker) const
{
    walker->~Walker();
    closeRoomOfWalker(walker);
    *roomIsTaken = false;
}

void Walker::noteThatCopyMakesNewList(FunctionID callee, Reference receiver, bool runOverBoxedValuesTakesListFirst)
{
    m_copyThatMakesNewList = receiver && receiver.isValue() ? callee : FunctionID();
    m_runOverBoxedValuesTakesNewListOfCopyFirst = m_copyThatMakesNewList && runOverBoxedValuesTakesListFirst;
    if (!m_copyThatMakesNewList)
        return;
    m_receiverOfCopyThatMakesNewList = underlyingCellOf(m_code, receiver.id());
    m_depthOfCopyThatMakesNewList = m_frames.size() + 1;
}

bool Walker::takeWordThatCopyMakesNewList(Reference receiver)
{
    FunctionID copied = std::exchange(m_copyThatMakesNewList, FunctionID());
    return copied && copied == currentFunction() && m_frames.size() == m_depthOfCopyThatMakesNewList && receiver && receiver.isValue()
        && underlyingCellOf(m_code, receiver.id()) == m_receiverOfCopyThatMakesNewList;
}

void Walker::takeRoomOf(Walker& former)
{
    RELEASE_ASSERT(m_frames.isEmpty());
    m_numbersOfStates.takeRoomOf(former.m_numbersOfStates);
    m_answersOfStates.takeRoomOf(former.m_answersOfStates);
    m_frames.takeRoomOf(former.m_frames);
    m_openEdges.takeRoomOf(former.m_openEdges);
    m_indexedIterations.takeRoomOf(former.m_indexedIterations);
    m_stringsInBuilders.takeRoomOf(former.m_stringsInBuilders);
    m_openLoops.takeRoomOf(former.m_openLoops);
    takeRoom(m_loopSummaries, former.m_loopSummaries);
}

bool someLoopGivesSlowVersionSomethingToReckon(const BodyIndex& index)
{
    static constexpr OpcodeSet reckoning = allNumericOperators | allRelations | allEqualities | OpcodeSet { Opcode::Strcat };
    static constexpr OpcodeSet reckoningAtHead = reckoning | OpcodeSet { Opcode::IteratorResume };
    if (!index.hasLoops() || !index.opcodes().intersects(reckoningAtHead))
        return false;
    for (uint32_t loop = 0; loop < index.numberOfLoops(); ++loop) {
        BlockID head = index.loop(LoopID(loop)).head;
        for (BlockID member : index.membersOf(LoopID(loop))) {
            BodyBlock block = index.block(member);
            const OpcodeSet& counted = member == head ? reckoningAtHead : reckoning;
            for (uint32_t place = 0; place < block.numberOfValues(); ++place) {
                if (counted.contains(index.opcodeOf(ValueID(block.firstValue().index() + place))))
                    return true;
            }
        }
    }
    return false;
}

void Walker::beginUnit(CodeUnit unit)
{
    RELEASE_ASSERT(!std::exchange(m_hasBegunUnit, true) && m_newestWalkerOfWorker == this);
    const Function& function = program().at(unit.function);

    bool hasVersionBeforeEvaluation = facts().beforeEvaluation().holdsIn(unit.function) && plan().conventions().hasParameterGuard(unit.function);
    m_noTextWasEvaluated = hasVersionBeforeEvaluation && unit.role == CodeRole::Body;
    m_comparesElementsOfViews = false;
    if (m_noTextWasEvaluated)
        static_cast<PositionsOfWalk&>(*m_positionsOfBuild).noteThatNoTextWasEvaluated();
    bool isSlowBody = unit.role == CodeRole::SlowBody && !hasVersionBeforeEvaluation;
    const BodyIndex* bodyOfSlowVersion = isSlowBody ? facts().bodies().find(unit.function) : nullptr;
    Mechanisms mechanismsOfSlowVersion { Mechanism::Folding, Mechanism::DirectCalls };
    if (bodyOfSlowVersion && someLoopGivesSlowVersionSomethingToReckon(*bodyOfSlowVersion))
        mechanismsOfSlowVersion = mechanismsOfSlowVersion | Mechanisms { Mechanism::UnboxedNumbers };
    m_mechanisms = isSlowBody ? m_mechanismsOfBuild & mechanismsOfSlowVersion : mechanismsOfWay(m_way, m_mechanismsOfBuild);
    m_positions = isSlowBody || m_way == WalkMode::Closed ? m_positionsOfSlowBody.get() : m_positionsOfBuild.get();
    if (isSlowBody || m_way == WalkMode::Closed)
        m_numbersOfStates.rememberNoPlace();

    m_code.reset(unit);
    m_code.setMarksLoops(!isSlowBody);
    m_code.setKeepsTestsPassed(m_inputs.options.verifiesCode && (!m_inputs.options.shouldAssumeIndexesInBounds || m_inputs.options.shouldVerifyAssumedBounds));
    if (m_way != WalkMode::AsBuilt)
        m_code.withhold(m_mechanismsOfBuild - m_mechanisms);
    if ((m_inputs.options.shufflesBlockFrequencies || m_inputs.options.ignoresBranchHints) && walksInputBody(unit.role)) [[unlikely]]
        m_code.takeFrequenciesOfBlocks(m_inputs.options.shufflesBlockFrequencies ? Code::FrequenciesOfBlocks::DrawnBySeed : Code::FrequenciesOfBlocks::AllNormal, m_inputs.options.shuffleSeed);
    m_origin = { .position = function.position, .inlineFrame = 0, .isTailCall = false, .isPartOfConstruction = false };
    m_environmentCellsOfUnit.mayNameSome = isHandedEnvironmentWithByte(*this, unit.function);
    if (std::exchange(m_workerHasBegunUnit, true) && m_inputs.options.shouldDisableLaterUnitSpeculation) [[unlikely]]
        m_opensGuesses = false;

    InlineFrame ownFrame { };
    ownFrame.callee = unit.function;
    ownFrame.callPosition = Instruction::noPosition;
    ownFrame.parent = 0;
    ownFrame.module = function.module;
    m_code.addInlineFrame(ownFrame);

    bool bodyStandsBehindUnit = walksInputBody(unit.role) && (unit.role != CodeRole::Body || !function.isCoroutine());
    m_frames.reserveCapacity(16);
    Frame& frame = m_frames.appendNew();
    frame.function = unit.function;
    frame.variablesStoredFirst = firstStores().variablesStoredFirstIn(unit.function);
    frame.isOfUnit = true;
    if (bodyStandsBehindUnit) {
        frame.index = facts().bodies().find(unit.function);
        if (frame.index) {
            frame.body = &frame.index->body();
            frame.references.lay(frame.index->numberOfValues());
        }
    }
}

bool Walker::standInFrontOfBodyOf(FunctionID function)
{
    beginUnit({ function, program().at(function).isCoroutine() ? CodeRole::CoroutineBody : CodeRole::Body });
    return m_frames[0].index;
}

bool Walker::walk(CodeUnit unit, WalkMode way)
{
    m_way = way;
    beginUnit(unit);
    bool bodyStandsBehindUnit = walksInputBody(unit.role) && (unit.role != CodeRole::Body || !program().at(unit.function).isCoroutine());

    bool isBuilt;
    if (bodyStandsBehindUnit)
        isBuilt = walkBody();
    else if (unit.role == CodeRole::Body)
        isBuilt = walkRampOfCoroutine(*this);
    else
        isBuilt = walkEntryUnit(*this, unit);
    if (!isBuilt && !m_hasFailed)
        fail("the unit was not built, and nobody says why"_s);
    if (!m_hasFailed && !m_whatJudgeOfOperandsRefused.isNull())
        fail(m_whatJudgeOfOperandsRefused, InternalErrorKind::TypeOutOfDate);
    if (m_hasFailed) {
        if (!m_failureIsSaid)
            sayFailure();
        return false;
    }

    if (!walksOwnBodyOfUnit())
        fail("the walk is over and the frame of a callee is left"_s);
    else if (!isUnreachable())
        fail("the walk is over and the block that it stands in has no end"_s);
    else if (m_code.entries().empty())
        fail("the walk is over and the unit has no entry"_s);
    for (uint32_t index = 0; index < m_code.numberOfEdges() && !m_hasFailed; ++index) {
        const Edge& edge = m_code.at(EdgeID(index));
        if (edge.isOpen())
            fail("the walk is over and an edge is open"_s);
        else if (!m_code.at(edge.to).hasEnded())
            fail("the walk is over and an edge leads to a block that has no end"_s);
    }
    return !m_hasFailed;
}

bool Walker::hasLastWalksOfLoopsToWalkAlone() const
{
    return m_inputs.options.replaysFinalLoopVisits && !m_hasFailed && !m_isWalkingLastAlone && !m_loopSummaries.isEmpty();
}

bool Walker::walkLastWalksOfLoopsAlone(Walker& first)
{
    m_isWalkingLastAlone = true;
    m_loopSummaries = WTF::move(first.m_loopSummaries);
    m_loopSummaryIndices = WTF::move(first.m_loopSummaryIndices);
    m_carriedResultRequests = WTF::move(first.m_carriedResultRequests);
    m_summariesRevisitedForCheck = WTF::move(first.m_summariesRevisitedForCheck);
    m_summariesAtLastBuildVisit = WTF::move(first.m_summariesAtLastBuildVisit);
    m_unguardedSumLoops = WTF::move(first.m_unguardedSumLoops);
    m_rowsWhoseKeysAreReadOffLook = WTF::move(first.m_rowsWhoseKeysAreReadOffLook);
    m_loopsWhoseTextsStayOutOfBuilders = WTF::move(first.m_loopsWhoseTextsStayOutOfBuilders);
    for (LoopSummary& summary : m_loopSummaries) {
        if (std::shared_ptr<const LoopSummary> walked = summary.summaryAtLastVisit)
            restoreSummary(summary, *walked);
    }
    return walk(first.unit(), first.m_way);
}

void Walker::restoreSummary(LoopSummary& summary, const LoopSummary& walked)
{
    summary.types = walked.types;
    summary.rungs = walked.rungs;
    summary.effects = walked.effects;
    summary.knowsEffects = walked.knowsEffects;
    summary.isPessimistic = walked.isPessimistic;
    summary.typesOfValues = walked.typesOfValues;
    summary.numberOfRecordsWhenTypesOfValuesWereNoted = 0;
    summary.firstValue = walked.firstValue;
    summary.steppedOnDoubles = walked.steppedOnDoubles;
    summary.mentionsThatLeave = walked.mentionsThatLeave;
    summary.storesIntoScopes = walked.storesIntoScopes;
}

bool Walker::tryRestoreLastVisitSummary(size_t place, bool isWalkedInBuild)
{
    if (!m_isWalkingLastAlone)
        return false;
    std::shared_ptr<const LoopSummary> walked = m_loopSummaries[place].summaryAtLastVisit;
    if (isWalkedInBuild)
        walked = place < m_summariesAtLastBuildVisit.size() ? m_summariesAtLastBuildVisit[place] : nullptr;
    if (!walked)
        return false;
    restoreSummary(m_loopSummaries[place], *walked);
    return true;
}

void Walker::recordSummaryAtLastVisit(size_t place, bool isWalkedInBuild, const std::shared_ptr<const LoopSummary>& row)
{
    if (isWalkedInBuild && m_summariesAtLastBuildVisit.size() <= place)
        m_summariesAtLastBuildVisit.grow(place + 1);
    auto& noted = isWalkedInBuild ? m_summariesAtLastBuildVisit[place] : m_loopSummaries[place].summaryAtLastVisit;
    noted = row;
    if (m_inputs.options.shouldUnderstateLoopEffects) {
        auto less = std::make_shared<LoopSummary>(*row);
        less->effects = Effects::none();
        less->knowsEffects = true;
        less->isPessimistic = false;
        noted = WTF::move(less);
    }
}

void Walker::fail(ASCIILiteral what, InternalErrorKind kind)
{
    if (!m_hasFailed) {
        m_hasFailed = true;
        m_failure = what;
        m_kindOfFailure = kind;
        m_functionOfFailure = m_frames.isEmpty() ? unit().function : currentFunction();
        if (m_trialVisits.isEmpty() && !m_snapshotsHeld)
            sayFailure();
    }
    endAsUnreachable(m_origin);
}

void Walker::sayFailure()
{
    dataLogLn("AOT: the walk of f", unit().function.index(), " ", nameOf(unit().role), " failed, in f", m_functionOfFailure.index(), ": ", m_failure);
    m_failureIsSaid = true;
}

void Walker::endAsUnreachable(Origin origin)
{
    if (!isUnreachable())
        m_code.end(m_block, TerminatorDraft(TerminatorKind::Unreachable, origin), 0);
    m_block = { };
}

bool Walker::walkBody()
{
    if (!m_frames[0].index) {
        fail("the function of the unit has no body"_s);
        return false;
    }
    const BodyIndex& index = *m_frames[0].index;
    if (index.hasIrreducibleFlow()) {
        fail("the body holds a cycle with two ways in"_s);
        return false;
    }

    takeEntryParameters();
    return walkBodyBehindItsParameters();
}

bool Walker::visitBodyFromEntry(std::span<const Reference> entryArguments, BlockID whereListsAreMade, const RestArrayAllocator& makerOfLists)
{
    Frame& frame = m_frames[0];
    frame.index = facts().bodies().find(unit().function);
    if (!frame.index || frame.index->hasIrreducibleFlow()) {
        fail("an entry walks a body that cannot be walked"_s);
        return false;
    }
    frame.body = &frame.index->body();
    frame.references.lay(frame.index->numberOfValues());
    BodyBlock entry = frame.index->block(entryBlock);
    if (entry.numberOfParameters() != entryArguments.size() || entryArguments.size() != program().at(unit().function).numberOfEntryParameters()) {
        fail("the entry block has other parameters than the function has entry parameters"_s);
        return false;
    }
    setCurrentInputBlock(entryBlock);
    for (unsigned index = 0; index < entryArguments.size(); ++index) {
        if (!entryArguments[index])
            continue;
        bind(entry.parameter(index), entryArguments[index]);
        if (program().at(unit().function).entryParameterKind(index) == EntryParameter::This)
            noteThisAtEntry(*this, entryArguments[index]);
    }
    m_code.noteThatEntryWalksBody();
    m_argumentsObjectsPutOffBehindEntry.shrink(0);
    m_blocksBuiltWhereCountStoodForLength.shrink(0);
    m_blockBuiltWhereListsWereMade = { };
    SetForScope restArrayBlockScope(m_restArrayAllocationBlock, whereListsAreMade);
    SetForScope restArrayAllocatorScope(m_restArrayAllocator, &makerOfLists);
    return walkBodyBehindItsParameters() && verifyThatCountStoodForLengthInFrontOfMaking();
}

bool Walker::verifyThatCountStoodForLengthInFrontOfMaking()
{
    if (!m_blockBuiltWhereListsWereMade || m_blocksBuiltWhereCountStoodForLength.isEmpty())
        return true;
    BitVector liesBehindMaking(m_code.numberOfBlocks());
    Vector<BlockID, 16> toLookAt;
    toLookAt.append(m_blockBuiltWhereListsWereMade);
    while (!toLookAt.isEmpty()) {
        m_code.forEachEdgeOf(toLookAt.takeLast(), [&](EdgeID, const Edge& edge) {
            if (!liesBehindMaking.quickSet(edge.to.index()))
                toLookAt.append(edge.to);
        });
    }
    for (BlockID block : m_blocksBuiltWhereCountStoodForLength) {
        if (liesBehindMaking.quickGet(block.index())) {
            fail("the count of arguments stood for the length of an object that a way to the read has made"_s);
            return false;
        }
    }
    return true;
}

void Walker::allocateRestArrays()
{
    m_blockBuiltWhereListsWereMade = m_block;
    const Function& function = program().at(unit().function);
    BodyBlock entry = m_frames.last().index->block(entryBlock);
    for (unsigned index = 0; index < function.numberOfEntryParameters() && !isUnreachable(); ++index) {
        EntryParameter kind = function.entryParameterKind(index);
        if (kind != EntryParameter::HiddenRest && (kind != EntryParameter::Parameter || index - function.firstParameterIndex() < function.numberOfPositionalParameters()))
            continue;
        Reference list = (*m_restArrayAllocator)(index);
        if (!list)
            return;
        bind(entry.parameter(index), list);
        noteListMadeByCaller(*this, list);
    }
    for (ValueID object : m_argumentsObjectsPutOffBehindEntry) {
        if (isUnreachable())
            return;
        walkOperation(m_frames.last().index->value(object));
        storeValueBehindItsDefinition(object);
    }
}

Reference Walker::argumentOfEntry(uint32_t position, Origin origin)
{
    Draft read(Opcode::GetArgument, origin);
    read.setArgumentIndex(plan().conventions().genericEntry(unit()).takesNewTarget + position);
    chooseConservativeFields(read);
    return build(WTF::move(read));
}

Reference Walker::rematerializeHiddenRestArray()
{
    const Function& function = program().at(unit().function);
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        EntryParameter kind = function.entryParameterKind(index);
        if (kind != EntryParameter::HiddenRest && (kind != EntryParameter::Parameter || index - function.firstParameterIndex() < function.numberOfPositionalParameters()))
            continue;
        Reference list = (*m_restArrayAllocator)(index);
        if (list)
            noteListMadeByCaller(*this, list);
        return list;
    }
    return { };
}

bool Walker::leaveArgumentsObjectToWhereListsAreMade(const BodyValue& value)
{
    const BodyIndex& index = *m_frames.last().index;
    if (value.opcode() == Opcode::CreateUnmappedArguments) {
        if (referenceBoundTo(value.as<Opcode::CreateUnmappedArguments>().restOperand()))
            return false;
        m_argumentsObjectsPutOffBehindEntry.append(value.id());
        return true;
    }
    if (value.opcode() != Opcode::PropGet || !m_argumentsObjectsPutOffBehindEntry.contains(value.objectOperand()) || value.key() != program().keyOf(BuiltinNames::length))
        return false;
    if (index.dominates(m_restArrayAllocationBlock, m_frames.last().block))
        return false;
    bind(value.id(), referenceOf(index.value(value.objectOperand()).as<Opcode::CreateUnmappedArguments>().countOperand()));
    if (m_inputs.options.verifiesCode)
        m_blocksBuiltWhereCountStoodForLength.append(m_block);
    return true;
}

bool Walker::walkBodyBehindItsParameters()
{
    const BodyIndex& index = *m_frames[0].index;
    if (m_environmentCellsOfUnit.mayNameSome && !m_hasFailed)
        markEnvironmentsThatCoroutineIsHanded(*this);
    if (m_hasFailed)
        return false;
    if (has(Mechanism::Arrivals))
        takeArrivals(*this);
    takeWhatIsNoLongerCaptured(*this);
    if (checksProofsOf(ProofSource::EntryParameter) && (unit().role == CodeRole::Body || unit().role == CodeRole::CoroutineBody))
        checkProofsOfWhatComesThroughFusedEntry();
    beginModuleInBody();
    if (facts().isTakenToRunOnce(unit().function) && !isUnreachable()) {
        Draft enter(Opcode::EnterBodyThatRunsOnce, m_origin);
        enter.setFunctionNumber(unit().function.index());
        build(WTF::move(enter));
    }
    stopAtSecondEntryOfMaker(*this);
    storeParametersNoLongerCaptured();

    for (uint32_t number = 0; number < index.numberOfBlocks() && !m_hasFailed; ++number)
        walkBlock(BlockID(number));
    endBlocksInFrontOfLoops(0, { });
    if (checksWalk() || checksProofs()) {
        m_typesShown.clear();
        askParametersOfHeadsForWhatIsShown();
    } else
        dropChecksProvedAfterLoops();
    return !m_hasFailed;
}

TypeID Walker::typeArrivingAtEntryParameter(unsigned index)
{
    FunctionID id = unit().function;
    TypeID known = has(Mechanism::Arrivals) ? m_arrivals.provenAt(m_types, id, index) : TypeID();
    return facts().typeBelievedOfEntryParameter(m_types, id, index, known);
}

TypeWithoutProof TypeWithoutProof::ofEntryParameter(Walker& walker, unsigned index)
{
    TypeArena& types = walker.types();
    FunctionID id = walker.unit().function;
    TypeID withoutProof = walker.facts().typeBelievedOfEntryParameter(types, id, index, TypeID());
    if (const DeclaredType* declared = walker.program().at(id).declaredEntryParameter(index); declared && declared->isEstablished() && types.at(withoutProof).isJavaScriptValue())
        withoutProof = types.at(withoutProof).primitives & Primitive::Absent ? TypeTable::anythingOrAbsent : TypeTable::anything;
    return TypeWithoutProof(withoutProof, FoundWhileWalking { });
}

TypeWithoutProof TypeWithoutProof::metWithWhatFormTells(TypeArena& types, Representation form) const
{
    TypeID toldByForm = widestTypeOf(form);
    TypeID both = types.intersectionOf(m_type, toldByForm);
    return TypeWithoutProof(isNeverType(both) ? toldByForm : both, FoundWhileWalking { });
}

void Walker::takeEntryParameters()
{
    FunctionID id = unit().function;
    const Function& function = program().at(id);
    const CallingConventions& conventions = plan().conventions();
    BodyBlock entry = m_frames[0].index->block(entryBlock);
    unsigned numberOfEntryParameters = function.numberOfEntryParameters();
    if (entry.numberOfParameters() != numberOfEntryParameters) {
        fail("the entry block has other parameters than the function has entry parameters"_s);
        return;
    }

    setCurrentInputBlock(entryBlock);
    BlockID block = addBlockFor(entryBlock);
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        TypeID type = typeArrivingAtEntryParameter(index);
        if (TypeID tested = unit().role == CodeRole::Body ? conventions.typeBehindGuard(id, index) : TypeID()) {
            TypeID passed = m_types.intersectionOf(type, tested);
            type = isNeverType(passed) ? tested : passed;
        }

        Representation representation = conventions.parameterRepresentation(program(), unit(), index);
        if (hasRegister(representation) && !isLegalRepresentation(m_types.at(type), representation)) {
            fail("an entry parameter arrives in a form that its type does not allow"_s);
            return;
        }
        bind(entry.parameter(index), addParameter(block, type, representation));
        EntryParameter kind = function.entryParameterKind(index);
        if (kind == EntryParameter::HiddenRest || (kind == EntryParameter::Parameter && index - function.firstParameterIndex() >= function.numberOfPositionalParameters()))
            noteListMadeByCaller(*this, referenceBoundTo(entry.parameter(index)));
        if (kind == EntryParameter::This && unit().role == CodeRole::Body)
            noteThisAtEntry(*this, referenceBoundTo(entry.parameter(index)));
    }
    if (unit().role == CodeRole::CoroutineBody && !addDriverParameterOfCoroutine(*this, block)) {
        fail("the body of a coroutine has got no driver"_s);
        return;
    }

    settleParametersOf(block);
    m_code.addEntry(block);
    m_block = block;
}

TypeID Walker::typeThatStoreTakesOnWordOfWalk(const Draft& store)
{
    static_assert(OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);
    bool testsForItself = store.level() != KnowledgeLevel::Proven;
    Reference value = store.valueOperand();
    TypeID claimed = testsForItself && value && value.isValue() ? typeAsBuilt(value) : TypeID();
    TypeID taken;
    for (LayoutID member : membersOf(m_types, store.receivers())) {
        if (!plan().isInUse(member))
            continue;
        std::optional<FieldIndex> field = plan().slots().findField(program(), member, store.key());
        TypeID type = field ? plan().slots().slot(member, *field).typeThatStoresAreHeldTo() : TypeID();
        if (testsForItself && (!claimed || !field || (type && !m_types.isSubset(claimed, type))))
            return { };
        if (type)
            taken = taken ? m_types.intersectionOf(taken, type) : type;
    }
    return taken;
}

static constexpr OpcodeSet storesIntoSlotsInWalk { Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::NewObject, Opcode::DefineOwnProperty };

static bool givesItsOperandInWalk(Opcode opcode)
{
    return opcode == Opcode::Narrowed || (opcode != Opcode::Parameter && (resultRuleOf(opcode) == ResultRule::Cast || familyOf(opcode) == OperationFamily::Representation));
}

bool Walker::makeTestOnTheWayAskInFull(Reference value, TypeID taken)
{
    for (ValueID id = value.id(); id && !m_code.isConstant(id); id = m_code.at(id).operand(0)) {
        const Operation& record = m_code.at(id);
        if (!givesItsOperandInWalk(record.opcode))
            return false;
        ValueID testID = record.opcode == Opcode::Narrowed && record.isOnTakenEdge() && !record.narrowsHolderOfOperand() && !record.narrowsOtherResultOfOperand() ? record.testOperand() : ValueID();
        if (!testID)
            continue;
        const Operation& test = m_code.at(testID);
        TypeID passing = test.opcode == Opcode::TypeTest ? test.testedType() : test.opcode == Opcode::ShapeTest ? m_types.forLayoutSet(test.layouts()) : TypeID();
        if (!passing || !m_types.isSubset(passing, taken) || m_types.testablePartOf(passing) != passing)
            continue;
        Representation form = m_code.at(test.operand(0)).representation;
        if (form != Representation::JSValue && form != Representation::Cell)
            continue;
        m_code.makeTestAskInFull(testID);
        for (ValueID between = value.id(); between != id; between = m_code.at(between).operand(0))
            removeTypeShown(between);
        removeTypeShown(id);
        return true;
    }
    return false;
}

TypeID Walker::typeThatDoorsHoldOfSlotRead(const Operation& read)
{
    LayoutSetID receivers = read.receivers();
    PropertyKeyID key = read.key();
    uint64_t receiversAndKey = static_cast<uint64_t>(receivers.bits()) << 32 | key.bits();
    TypeHeldOfSlots& remembered = m_typesHeldOfSlots[(receivers.bits() ^ key.bits()) % m_typesHeldOfSlots.size()];
    if (remembered.receiversAndKey == receiversAndKey)
        return remembered.type;
    TypeID held;
    TypeID heldBefore;
    for (LayoutID member : membersOf(m_types, receivers)) {
        if (!plan().isInUse(member))
            continue;
        std::optional<FieldIndex> field = plan().slots().findField(program(), member, key);
        TypeID type = field ? plan().slots().slot(member, *field).typeThatStoresAreHeldTo() : TypeID();
        if (type && type == heldBefore)
            continue;
        heldBefore = type;
        if (!type) {
            held = { };
            break;
        }
        type = typeThatUnitMayTestFor(m_types.testablePartOf(type), true, true);
        held = held ? m_types.unionOf(held, type) : type;
    }
    remembered = { receiversAndKey, held };
    return held;
}

bool Walker::makeObjectOfSlotReadShown(Reference value, TypeID taken)
{
    ValueID id = value.id();
    while (id && !m_code.isConstant(id) && givesItsOperandInWalk(m_code.at(id).opcode))
        id = m_code.at(id).operand(0);
    if (!id || m_code.isConstant(id))
        return false;
    const Operation& read = m_code.at(id);
    bool looksAbove = read.opcode == Opcode::GetSlotOrUndefined && read.looksAboveEmptySlot();
    if (!looksAbove && (read.opcode != Opcode::GetSlot || read.level() != KnowledgeLevel::Proven))
        return false;
    TypeID held = typeThatDoorsHoldOfSlotRead(read);
    ValueID object = read.objectOperand();
    if (!held || !m_types.isSubset(held, taken) || !object || !makeTestOnTheWayAskInFull(Reference::value(object), m_types.forLayoutSet(read.receivers())))
        return false;
    for (ValueID between = value.id(); between != id; between = m_code.at(between).operand(0))
        removeTypeShown(between);
    removeTypeShown(id);
    return true;
}

void Walker::holdStoredValuesToSlots(const Draft& draft)
{
    if (draft.opcode == Opcode::PutSlot && draft.undoesIterationStore())
        return;
    auto isBoxedOrCell = [&](Reference value) {
        if (!value || !value.isValue())
            return false;
        Representation form = representationOf(value);
        return form == Representation::JSValue || form == Representation::Cell || form == Representation::Cell32;
    };
    auto holdToSlotOfObjectThatMayBeOld = [&](Reference value, TypeID taken) {
        if (taken && !m_types.at(taken).isAnything()) {
            checkThatSlotTakes(value, taken, draft.origin());
            return;
        }
        TypeID claimed = typeAsBuilt(value);
        if (!claimed || isNeverType(claimed) || !m_types.at(claimed).isJavaScriptValue() || m_types.at(claimed).hasCells() || isUnreachable())
            return;
        Origin origin = draft.origin();
        origin.isTailCall = false;
        TypeID noCell = TypeArena::fromPrimitives((Primitive::All & ~cellPrimitives) | (m_types.at(claimed).primitives & Primitive::Absent));
        if (!isShown(value, noCell))
            buildCheckOfProof(value, noCell, TypeTable::anythingOrAbsent, ProofSource::Field, origin, false, false, false, true);
    };
    auto holdRawWordToPartOfItsKind = [&](Reference value, TypeID taken) {
        if (!value || !value.isValue() || !taken || isUnreachable())
            return;
        Representation form = representationOf(value);
        if (!numberRepresentations.contains(form) && !optionalRepresentations.contains(form) && form != Representation::Boolean)
            return;
        TypeID claimed = typeAsBuilt(value);
        if (!claimed || isNeverType(claimed) || m_types.isSubset(widestTypeOf(form), taken))
            return;
        if (m_types.at(claimed).primitives & Primitive::Absent)
            taken = m_types.withPrimitives(taken, Primitive::Absent);
        TypeID both = m_types.isSubset(claimed, taken) ? claimed : m_types.intersectionOf(claimed, taken);
        if (isNeverType(both))
            both = taken;
        if (isShown(value, both))
            return;
        Origin origin = draft.origin();
        origin.isTailCall = false;
        buildCheckOfProof(value, both, TypeWithoutProof::toldByForm(form), ProofSource::Field, origin, false, true, false, true);
    };
    if (draft.opcode == Opcode::DefineOwnProperty) {
        if (!draft.valueOperand() || !draft.valueOperand().isValue())
            return;
        std::optional<FieldAccess> created = fieldCreatedInSlotBy(AccessContext { program(), WalkFacts(facts()), plan(), m_types, currentFunction() }, {
            typeAsBuilt(draft.objectOperand()), typeAsBuilt(draft.computedKeyOperand()),
            typeAsBuilt(draft.valueOperand()), typeAsBuilt(draft.getterOperand()), typeAsBuilt(draft.setterOperand()), draft.descriptorFlags()
        });
        if (created && isBoxedOrCell(draft.valueOperand()))
            holdToSlotOfObjectThatMayBeOld(draft.valueOperand(), created->slot.typeThatStoresAreHeldTo());
        else if (created)
            holdRawWordToPartOfItsKind(draft.valueOperand(), created->slot.typeThatStoresAreHeldTo());
        return;
    }
    if (draft.opcode != Opcode::NewObject) {
        if (isBoxedOrCell(draft.valueOperand()))
            holdToSlotOfObjectThatMayBeOld(draft.valueOperand(), typeThatStoreTakesOnWordOfWalk(draft));
        else
            holdRawWordToPartOfItsKind(draft.valueOperand(), typeThatStoreTakesOnWordOfWalk(draft));
        return;
    }
    if (program().at(draft.layout()).kind == LayoutKind::Environment)
        return;
    std::span<const Reference> values = draft.valuesOperands();
    for (uint32_t index = 0; index < std::min(draft.fields().size(), values.size()); ++index) {
        if (isBoxedOrCell(values[index]))
            checkThatSlotTakes(values[index], plan().slot(draft.layout(), draft.fields()[index]).typeThatStoresAreHeldTo(), draft.origin());
        else
            holdRawWordToPartOfItsKind(values[index], plan().slot(draft.layout(), draft.fields()[index]).typeThatStoresAreHeldTo());
    }
}

void Walker::holdWhatIsFoundInPlace(Reference found, TypeID held, ProofSource source)
{
    if (!optionalRepresentations.contains(representationOf(found))) {
        buildCheckOfProof(found, held, TypeTable::anythingOrAbsent, source, m_origin, true);
        return;
    }
    TypeID whatFormHolds = typeThatFormSays(found);
    TypeID stored = m_types.withPrimitives(TypeTable::number, m_types.at(held).primitives & representedMarkerPrimitives);
    if (m_types.isSubset(whatFormHolds, stored))
        return;
    Origin origin = m_origin;
    origin.isTailCall = false;
    Draft check(Opcode::CheckProof, origin);
    check.setValueOperand(found);
    check.setCheckedType(stored);
    check.setTypeWithoutProof(whatFormHolds);
    check.setSource(source);
    build(WTF::move(check));
    ++m_numbersOfProofsChecked[static_cast<unsigned>(source)];
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::holdVariableOfAnotherFileStoredFirst(VariableID variable)
{
    if (program().at(variable).module == program().at(currentFunction()).module || !facts().linked().variables[variable.index()].isFilled)
        return;
    if (m_state.isPastDeadZone(variable) || plan().deferredDefinitions().findDeferred(variable))
        return;
    TypeID held = m_positionsOfBuild->variable(TypeLevel::Proven, variable);
    if (!held)
        return;
    Draft read(Opcode::GetModuleVariable, m_origin);
    read.setVariable(variable);
    chooseConservativeFields(read);
    Reference found = build(WTF::move(read));
    if (!found || !found.isValue() || isUnreachable())
        return;
    count(StatisticID::CrossModuleVariableChecksEmitted);
    holdWhatIsFoundInPlace(found, held, ProofSource::Variable);
    m_state.noteEndOfDeadZone(variable);
}

void Walker::holdVariablesStoredFirstWhereFunctionsAreUsed(std::span<const LinkedVariableStoredFirst> claims, std::span<const FunctionID> used, VariableID storedByOperation)
{
    bool isListOfUnitItself = claims.data() == m_frames.first().variablesStoredFirst.data();
    for (FunctionID function : used) {
        if (isListOfUnitItself && m_state.claimsAreAnswered(function))
            continue;
        bool everyClaimIsAnswered = isListOfUnitItself;
        for (const LinkedVariableStoredFirst& claim : std::ranges::equal_range(claims, function, { }, &LinkedVariableStoredFirst::functionWhoseUseCounts)) {
            everyClaimIsAnswered &= claim.variable != storedByOperation;
            if (claim.variable == storedByOperation || isUnreachable() || m_state.isPastDeadZone(claim.variable) || plan().deferredDefinitions().findDeferred(claim.variable))
                continue;
            TypeID held = m_positionsOfBuild->variable(TypeLevel::Proven, claim.variable);
            if (!held)
                continue;
            Draft read(Opcode::GetModuleVariable, m_origin);
            read.setVariable(claim.variable);
            chooseConservativeFields(read);
            Reference found = build(WTF::move(read));
            if (!found || !found.isValue() || isUnreachable())
                return;
            holdWhatIsFoundInPlace(found, held, ProofSource::Variable);
            m_state.noteEndOfDeadZone(claim.variable);
        }
        if (everyClaimIsAnswered && !isUnreachable())
            m_state.noteThatClaimsAreAnswered(function);
    }
}

void Walker::holdSlotsOfScopesThatLeaveAt(std::span<const SummaryScopeStoredFirst> scopes, uint32_t position, bool standsBehindOperationsOfBlock)
{
    uint32_t block = m_frames.last().block.index();
    auto place = std::ranges::lower_bound(scopes, std::pair { block, position }, { }, [&](const SummaryScopeStoredFirst& atPlace) {
        const SummaryScopeStoredFirst& scope = scopes[atPlace.scopeAtThisPlaceInOrderOfLeaving];
        return std::pair { scope.blockOfLeaving, scope.positionOfLeaving };
    });
    for (; place != scopes.end() && !isUnreachable(); ++place) {
        const SummaryScopeStoredFirst& scope = scopes[place->scopeAtThisPlaceInOrderOfLeaving];
        if (scope.blockOfLeaving != block || (!standsBehindOperationsOfBlock && scope.positionOfLeaving != position))
            return;
        Reference environment = referenceOf(scope.allocation);
        if (!environment || !environment.isRecord())
            continue;
        LayoutID layout = currentBodyIndex().value(scope.allocation).as<Opcode::NewObject>().layout();
        for (uint32_t field : scope.fields)
            holdSlotStoredFirst(environment, { layout, field }, { });
    }
}

static ALWAYS_INLINE bool namesSlotStoredFirst(Walker& walker, const BodyValue& access)
{
    std::span<const SummaryScopeStoredFirst> scopes = walker.facts().summaries().at(walker.currentFunction()).scopesStoredFirst.span();
    if (scopes.empty())
        return false;
    ValueID allocation = walker.currentBodyIndex().stripCasts(access.environmentOperand());
    auto scope = std::ranges::lower_bound(scopes, allocation, { }, &SummaryScopeStoredFirst::allocation);
    return scope != scopes.end() && scope->allocation == allocation && std::ranges::binary_search(scope->fields, access.field());
}

void Walker::holdWhatBodyReadsOfItsOwnScope(const BodyValue& read, Reference environment, Reference found)
{
    if (namesSlotStoredFirst(*this, read))
        holdSlotStoredFirst(environment, { read.layout(), read.field() }, found);
}

void Walker::noteStoreIntoSlotStoredFirst(const BodyValue& store, Reference environment)
{
    if (namesSlotStoredFirst(*this, store))
        m_state.noteEndOfDeadZone(environment, { store.layout(), store.field() });
}

void Walker::holdSlotStoredFirst(Reference environment, FieldReference field, Reference found)
{
    if (isUnreachable() || m_state.isPastDeadZone(environment, field))
        return;
    TypeID held = m_positionsOfBuild->environmentField(TypeLevel::Proven, field.layout, field.field);
    if (!held)
        return;
    if (Reference known = m_state.environmentField(environment, field); known && known.isConstant() && m_inputs.options.verifiesCode && !m_types.isSubset(typeOf(known), held)) {
        fail("the scan says that a slot of a scope is stored above the point at which the scope leaves, and the walk finds there what the allocation left in it"_s);
        return;
    }
    if (!found) {
        Draft read(Opcode::GetEnvironmentField, m_origin);
        read.setEnvironmentOperand(environment);
        read.setLayout(field.layout);
        read.setField(field.field);
        chooseConservativeFields(read);
        found = build(WTF::move(read));
    }
    if (!found || !found.isValue() || isUnreachable())
        return;
    holdWhatIsFoundInPlace(found, held, ProofSource::SlotOfScope);
    m_state.noteEndOfDeadZone(environment, field);
}

void Walker::stopAtMakerUnless(Reference holds, Origin origin)
{
    origin.isTailCall = false;
    Draft check(Opcode::CheckProof, origin);
    check.setValueOperand(holds);
    check.setCheckedType(TypeArena::fromPrimitives(Primitive::True));
    check.setTypeWithoutProof(TypeTable::boolean);
    check.setSource(ProofSource::Field);
    build(WTF::move(check));
}

void Walker::stopWhereWordOfLinkIsFalse(Origin origin)
{
    origin.isTailCall = false;
    Draft no(Opcode::Boolean, origin);
    no.setBoolean(false);
    Draft check(Opcode::CheckProof, origin);
    check.setValueOperand(constant(WTF::move(no)));
    check.setCheckedType(TypeArena::fromPrimitives(Primitive::True));
    check.setTypeWithoutProof(TypeTable::boolean);
    check.setSource(ProofSource::Contents);
    build(WTF::move(check));
    endAsUnreachable(origin);
}

void Walker::stopAtMaker(Origin origin)
{
    count(StatisticID::IncompleteAllocationsTrapped);
    Draft no(Opcode::Boolean, origin);
    no.setBoolean(false);
    stopAtMakerUnless(constant(WTF::move(no)), origin);
    endAsUnreachable(origin);
}

static BitVector fieldsThatAllocationListsInWalk(Walker& walker, std::span<const FieldIndex> fields, const auto& valueAt)
{
    BitVector listed;
    for (size_t place = 0; place < fields.size(); ++place) {
        if (Reference value = valueAt(place); value && !(walker.types().at(walker.typeOf(value)).primitives & Primitive::Absent))
            listed.set(fields[place]);
    }
    return listed;
}

bool Walker::allocationHoldsWhatIsHeldFilled(const Draft& allocation)
{
    if (m_isMakingObjectsOfConstant || program().at(allocation.layout()).kind == LayoutKind::Instance)
        return true;
    if (isWrittenInTopFrame(allocation) && facts().summaries().at(currentFunction()).makerEndsWhereItLeaves(allocation.inputValueGiven))
        return true;
    Vector<FieldIndex, 8> held = fieldsHeldFilledInSlots(program(), facts().linked(), plan().slots(), allocation.layout());
    if (held.isEmpty())
        return true;
    std::span<const Reference> values = allocation.valuesOperands();
    BitVector listed = fieldsThatAllocationListsInWalk(*this, allocation.fields(), [&](size_t place) { return place < values.size() ? values[place] : Reference(); });
    if (std::ranges::all_of(held, [&](FieldIndex field) { return listed.get(field); }))
        return true;
    const BodyIndex& index = currentBodyIndex();
    if (!m_valueBeingWalked || !index.holdsAnyOf({ Opcode::FinishObject }))
        return false;
    for (uint32_t number = m_valueBeingWalked.index() + 1; number < index.numberOfValues(); ++number) {
        ValueID id(number);
        if (index.isParameter(id) || index.opcodeOf(id) != Opcode::FinishObject)
            continue;
        ValueID literal = index.stripCasts(index.value(id).operand(0));
        if (literal.index() <= m_valueBeingWalked.index() && !index.isParameter(literal) && index.opcodeOf(literal) == Opcode::NewObject && index.value(literal).as<Opcode::NewObject>().layout() == allocation.layout())
            return true;
    }
    return false;
}

void Walker::showThatMakersThatLeaveAtHoldWhatIsHeldFilled(std::span<const SummaryMakerThatEndsWhereItLeaves> makers, uint32_t position, bool standsBehindOperationsOfBlock)
{
    uint32_t block = m_frames.last().block.index();
    UNUSED_PARAM(standsBehindOperationsOfBlock);
    for (const SummaryMakerThatEndsWhereItLeaves& maker : std::ranges::equal_range(makers, position, { }, &SummaryMakerThatEndsWhereItLeaves::positionOfLeaving)) {
        if (maker.blockOfLeaving != block || isUnreachable())
            continue;
        BodyValue made = currentBodyIndex().value(maker.allocation);
        LayoutID layout = made.opcode() == Opcode::NewObject ? made.as<Opcode::NewObject>().layout() : program().at(made.as<Opcode::Data>().data()).root.as<DataNode::Object>().layout;
        if (Reference object = referenceOf(maker.allocation))
            showThatObjectHoldsWhatIsHeldFilled(object, layout);
    }
}

bool Walker::constantListsFieldsHeldFilled(DataID data, RootOfConstant whereRootEnds)
{
    uint32_t question = data.index() * 2 + (whereRootEnds == RootOfConstant::EndsWhereItLeaves);
    return m_constantsThatListFieldsHeldFilled.ensure(question, [&] {
        return constantListsFieldsHeldFilledInSlots(program(), facts().linked(), plan().slots(), data, whereRootEnds);
    }).iterator->value;
}

bool Walker::constantEndsWhereItLeaves(ValueID written, DataID data) const
{
    const BodyIndex& index = currentBodyIndex();
    if (!written || written.index() >= index.numberOfValues() || index.isParameter(written) || index.opcodeOf(written) != Opcode::Data || index.value(written).as<Opcode::Data>().data() != data)
        return false;
    return facts().summaries().at(currentFunction()).makerEndsWhereItLeaves(written);
}

void Walker::showThatLiteralHoldsWhatIsHeldFilled(const BodyValue& finishOrData)
{
    const BodyIndex& index = currentBodyIndex();
    ValueID literal = index.stripCasts(finishOrData.operand(0));
    if (facts().summaries().at(currentFunction()).makerEndsWhereItLeaves(literal))
        return;
    Reference object = referenceOf(finishOrData.operand(0));
    if (object && !index.isParameter(literal) && index.opcodeOf(literal) == Opcode::NewObject)
        showThatObjectHoldsWhatIsHeldFilled(object, index.value(literal).as<Opcode::NewObject>().layout(), index.opcodes().contains(Opcode::CopyDataProperties));
}

void Walker::showThatObjectHoldsWhatIsHeldFilled(Reference object, LayoutID id, bool mayBeFilledByCopies)
{
    Vector<FieldIndex, 8> fieldsToAsk = fieldsHeldFilledThatAreNotShown(object, id);
    bool recordOfLayoutNamesThemAll = plan().layout(id).holdsCompiledObjects() && std::ranges::all_of(fieldsToAsk, [&](FieldIndex field) {
        return plan().slot(id, field).indexInLayoutRecord.has_value();
    });
    bool asksForFieldOfClassAbove = std::ranges::any_of(fieldsToAsk, [&](FieldIndex field) {
        return program().at(id).fields[field].flags.contains(FieldFlags::IsInherited);
    });
    bool checksAllTogether = asksForFieldOfClassAbove ? fieldsToAsk.size() > 1 : mayBeFilledByCopies && fieldsToAsk.size() > 2;
    if (checksAllTogether && recordOfLayoutNamesThemAll) {
        count(StatisticID::ObjectInitializationChecksEmitted);
        Draft test(Opcode::AreHeldFieldsFilled, m_origin);
        test.setObjectOperand(object);
        test.setLayout(id);
        if (Reference areFilled = build(WTF::move(test)); areFilled && !isUnreachable())
            stopAtMakerUnless(areFilled, m_origin);
        return;
    }
    for (FieldIndex field : fieldsToAsk) {
        count(StatisticID::FieldInitializationChecksEmitted);
        Draft test(Opcode::IsSlotFilled, m_origin);
        test.setObjectOperand(object);
        test.setKey(program().at(id).fields[field].key);
        test.setReceivers(LayoutSets::of(id));
        test.setLevel(KnowledgeLevel::Proven);
        Reference isFilled = build(WTF::move(test));
        if (!isFilled || isUnreachable())
            return;
        stopAtMakerUnless(isFilled, m_origin);
    }
}

Vector<FieldIndex, 8> Walker::fieldsHeldFilledThatAreNotShown(Reference object, LayoutID id)
{
    Reference made = object;
    while (made.isValue() && cellPreservingOpcodes.contains(m_code.at(made.id()).opcode))
        made = Reference::value(m_code.at(made.id()).operand(0));
    bool allocationIsInSight = made.isValue() && m_code.at(made.id()).opcode == Opcode::NewObject && m_code.at(made.id()).layout() == id;
    BitVector listed;
    if (allocationIsInSight) {
        const Operation& allocation = m_code.at(made.id());
        listed = fieldsThatAllocationListsInWalk(*this, allocation.fields(), [&](size_t place) { return place < allocation.valuesOperands().size() ? Reference::value(allocation.valuesOperands()[place]) : Reference(); });
    }
    Vector<FieldIndex, 8> fieldsToAsk;
    for (FieldIndex field : fieldsHeldFilledInSlots(program(), facts().linked(), plan().slots(), id)) {
        if (listed.get(field))
            continue;
        Reference held = m_state.slot(made, { id, field });
        if (!held || (m_types.at(typeOf(held)).primitives & Primitive::Absent))
            fieldsToAsk.append(field);
    }
    return fieldsToAsk;
}

void Walker::checkProofsOfWhatIsStoredBy(const Draft& draft)
{
    Origin origin = draft.origin();
    if (reliesOf(draft.opcode).contains(Proof::Pristine) && !m_isBuildingInPlace && m_numberOfLoopsThatMayBeLeft >= m_openLoops.size())
        verifyPristineObject(draft.operand(0));
    switch (draft.opcode) {
    case Opcode::CallDirect:
        checkProofsOfOperandsOfDirectCall(draft);
        return;
    case Opcode::FindsNoMember:
        if (!m_isBuildingInPlace && m_numberOfLoopsThatMayBeLeft >= m_openLoops.size())
            askWhatLookupLeansOn(*this, draft);
        return;
    case Opcode::PutSlotOrAdd:
    case Opcode::GetSlotOrUndefined:
    case Opcode::GetDeferredClosureSlotOrUndefined:
        if (draft.level() == KnowledgeLevel::Proven && m_numberOfLoopsThatMayBeLeft >= m_openLoops.size())
            checkFactsRememberedOfCell(draft.objectOperand(), origin, factsThatSlotOperationLeansOn(*this, draft));
        if (isUnreachable() || (draft.level() == KnowledgeLevel::Proven && (draft.opcode != Opcode::PutSlotOrAdd || draft.isDefinition())))
            return;
        if ((draft.opcode == Opcode::GetSlotOrUndefined || draft.opcode == Opcode::PutSlotOrAdd) && !m_isBuildingInPlace && m_numberOfLoopsThatMayBeLeft >= m_openLoops.size())
            askWhatLookupLeansOn(*this, draft);
        return;
    case Opcode::GetSlot:
    case Opcode::PutSlot:
        if (draft.opcode == Opcode::PutSlot && draft.level() == KnowledgeLevel::Proven)
            checkFactsRememberedOfCell(draft.objectOperand(), origin, factsThatSlotOperationLeansOn(*this, draft));
        checkProofThatSlotIsFilled(draft);
        return;
    case Opcode::GetDeferredClosureSlot:
    case Opcode::GetSlotGetter:
    case Opcode::GetSlotSetter:
        checkProofThatSlotIsFilled(draft);
        return;
    default:
        return;
    }
}

static constexpr OpcodeSet storesIntoPlacesInWalk { Opcode::PutModuleVariable, Opcode::PutEnvironmentField, Opcode::NewObject, Opcode::NewClosure, Opcode::TryReExportFields };

void Walker::proveThatFormOfVariableTakesStoredValue(Draft& store)
{
    Representation form = Typed::representationOf(plan().variable(store.variable()).representation);
    Reference stored = store.valueOperand();
    if (form == Representation::JSValue || !stored || !stored.isRecord())
        return;
    TypeID type = typeOf(stored);
    if (!type || isNeverType(type) || isLegalRepresentation(m_types, type, form))
        return;
    TypeID held = m_positionsOfBuild->variable(TypeLevel::Proven, store.variable());
    if (Reference proven = proveOrTrap(stored, held ? m_types.testablePartOf(m_types.withoutPrimitives(held, Primitive::Absent)) : widestTypeOf(form), ProofSource::Variable, store.origin()))
        store.setValueOperand(proven);
}

void Walker::proveThatSlotsOfScopeTakeStoredValues(Draft& store)
{
    static constexpr RepresentationSet rawForms { Representation::OptionalInt64, Representation::OptionalDouble };
    auto proven = [&](FieldIndex field, Reference stored) -> Reference {
        Representation form = Typed::representationOf(plan().slot(store.layout(), field).representation);
        TypeID type = stored && stored.isRecord() ? typeOf(stored) : TypeID();
        if (!rawForms.contains(form) || !type || isNeverType(type) || isLegalRepresentation(m_types, type, form))
            return stored;
        TypeID held = m_positionsOfBuild->environmentField(TypeLevel::Proven, store.layout(), field);
        return proveOrTrap(stored, held ? m_types.testablePartOf(m_types.withoutPrimitives(held, Primitive::Absent)) : widestTypeOf(form), ProofSource::SlotOfScope, store.origin());
    };
    if (store.opcode == Opcode::PutEnvironmentField) {
        if (Reference value = proven(store.field(), store.valueOperand()))
            store.setValueOperand(value);
        return;
    }
    Vector<Reference, 16> values;
    for (size_t index = 0; index < store.valuesOperands().size(); ++index) {
        Reference value = isUnreachable() || index >= store.fields().size() ? Reference() : proven(store.fields()[index], store.valuesOperands()[index]);
        values.append(value ? value : Reference(store.valuesOperands()[index]));
    }
    store.setValuesOperands(values.span());
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::holdStoredValuesToPlaces(const Draft& draft)
{
    Origin origin = draft.origin();
    bool storesWrittenConstant = false;
    if ((draft.opcode == Opcode::PutModuleVariable || draft.opcode == Opcode::PutEnvironmentField) && isWrittenInTopFrame(draft)) {
        const BodyIndex& index = currentBodyIndex();
        Opcode written = index.opcodeOf(draft.inputValueGiven);
        ValueID stored = written == Opcode::PutModuleVariable || written == Opcode::PutEnvironmentField ? index.value(draft.inputValueGiven).valueOperand() : ValueID();
        if (stored)
            stored = index.stripCasts(stored);
        storesWrittenConstant = stored && !index.isParameter(stored) && constantsInFormOfUses.contains(index.opcodeOf(stored));
    }
    if (draft.opcode == Opcode::PutModuleVariable) {
        const PromotedVariable* promoted = plan().promotedEnvironments().find(draft.variable());
        if (promoted && promoted->function)
            return;
        holdToTypeOfPlace(draft.valueOperand(), m_positionsOfBuild->variable(TypeLevel::Proven, draft.variable()), promoted ? ProofSource::Capture : ProofSource::Variable, origin, storesWrittenConstant);
        return;
    }
    if (draft.opcode == Opcode::PutEnvironmentField) {
        if (Reference born = draft.valueOperand(); born && born.isConstant() && typeOf(born) == TypeID(TypeTable::undefined) && plan().promotedEnvironments().isPromoted(draft.layout()))
            return;
        holdToTypeOfPlace(draft.valueOperand(), m_positionsOfBuild->environmentField(TypeLevel::Proven, draft.layout(), draft.field()), ProofSource::SlotOfScope, origin, storesWrittenConstant);
        return;
    }
    if (draft.opcode == Opcode::TryReExportFields) {
        holdToTypeOfPlace(draft.sourceOperand(), m_positionsOfBuild->capture(TypeLevel::Proven, draft.function(), draft.keyIsFirstCapture() ? 1 : 0), ProofSource::Capture, origin);
        return;
    }
    if (draft.opcode == Opcode::NewClosure) {
        uint32_t index = 0;
        for (Reference captured : draft.capturesOperands())
            holdToTypeOfPlace(captured, m_positionsOfBuild->capture(TypeLevel::Proven, draft.function(), index++), ProofSource::Capture, origin);
        return;
    }
    if (program().at(draft.layout()).kind != LayoutKind::Environment)
        return;
    std::span<const FieldIndex> fields = draft.fields();
    std::span<const Reference> values = draft.valuesOperands();
    for (size_t index = 0; index < std::min(fields.size(), values.size()); ++index) {
        if (values[index] && values[index].isConstant() && typeOf(values[index]) == TypeID(TypeTable::undefined))
            continue;
        holdToTypeOfPlace(values[index], m_positionsOfBuild->environmentField(TypeLevel::Proven, draft.layout(), fields[index]), ProofSource::SlotOfScope, origin);
    }
}

void Walker::holdToTypeOfPlace(Reference value, TypeID held, ProofSource source, Origin origin, bool isConstantWrittenInBody)
{
    if (!value || !value.isRecord() || !held || isUnreachable())
        return;
    TypeID claimed = value.isConstant() ? typeOf(value) : typeAsBuilt(value);
    if (!claimed || isNeverType(claimed) || !m_types.at(held).isJavaScriptValue() || !m_types.at(claimed).isJavaScriptValue() || m_types.at(held).isAnything())
        return;
    if (value.isConstant() && (m_types.at(claimed).primitives & Primitive::Absent))
        return;
    origin.isTailCall = false;
    Representation form = representationOf(value);
    bool hasBox = !value.isConstant() && (form == Representation::JSValue || form == Representation::Cell || form == Representation::Cell32);
    if (hasBox && !checksProofsOf(source) && (isShown(value, held) || makeTestOnTheWayAskInFull(value, held)))
        return;
    buildCheckOfProof(value, held, TypeTable::anythingOrAbsent, source, origin, true, false, isConstantWrittenInBody);
}

void Walker::checkThatSlotTakes(Reference value, TypeID taken, Origin origin)
{
    if (!value || !value.isValue() || !taken || isNeverType(taken) || isUnreachable())
        return;
    TypeID claimed = typeAsBuilt(value);
    if (!claimed || isNeverType(claimed) || !m_types.at(claimed).isJavaScriptValue() || !m_types.at(taken).isJavaScriptValue() || m_types.at(taken).isAnything())
        return;
    Representation representation = representationOf(value);
    if (representation != Representation::JSValue && representation != Representation::Cell && representation != Representation::Cell32)
        return;
    if (m_types.at(claimed).primitives & Primitive::Absent)
        taken = m_types.withPrimitives(taken, Primitive::Absent);
    origin.isTailCall = false;
    if (TypeID both = m_types.isSubset(claimed, taken) ? claimed : m_types.intersectionOf(claimed, taken); !isNeverType(both))
        taken = both;
    auto isShownToFit = [&] {
        TypeID shown = typeShown(value);
        if (m_types.at(shown).primitives & Primitive::Absent)
            shown = m_types.withoutPrimitives(shown, Primitive::Absent);
        return m_types.isSubset(shown, taken);
    };
    if (checksProofsOf(ProofSource::Field) || (!isShownToFit() && !makeTestOnTheWayAskInFull(value, taken) && !(makeObjectOfSlotReadShown(value, taken) && isShownToFit())))
        buildCheckOfProof(value, taken, TypeTable::anythingOrAbsent, ProofSource::Field, origin, true, false, false, true);
}

void Walker::checkProofThatSlotIsFilled(const Draft& read)
{
    if (!checksProofs() || isUnreachable() || read.level() != KnowledgeLevel::Proven)
        return;
    Representation form = read.opcode != Opcode::GetSlot || read.representation != Representation::Unchosen ? read.representation : resultRepresentationOf(*this, read);
    bool givesRawWord = read.opcode == Opcode::GetSlot && (numberRepresentations.contains(form) || form == Representation::Boolean);
    for (Reference object = read.objectOperand(); object && object.isValue();) {
        const Operation& record = m_code.at(object.id());
        if (record.opcode != Opcode::Narrowed)
            break;
        if (ValueID test = record.testOperand(); test && m_code.at(test).opcode == Opcode::IsSlotFilled && m_code.at(test).key() == read.key() && (!givesRawWord || m_code.at(test).holdsDeclaredType()))
            return;
        object = Reference::value(record.valueOperand());
    }
    Draft test(Opcode::IsSlotFilled, read.origin());
    test.setObjectOperand(read.objectOperand());
    test.setKey(read.key());
    test.setReceivers(read.receivers());
    test.setLevel(read.level());
    test.setHoldsDeclaredType(read.opcode == Opcode::GetSlot && (read.holdsDeclaredType() || givesRawWord));
    Reference isFilled = buildTestOfCheck(WTF::move(test));
    if (isFilled && isFilled.isValue() && !isUnreachable())
        checkProof(isFilled, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::Field, read.origin());
}

void Walker::checkProofsOfWhatComesThroughFusedEntry()
{
    const CallingConventions& conventions = plan().conventions();
    if (!checksProofs() && !conventions.holdsFusedCallEntry(unit()) && !(unit().role == CodeRole::Body && conventions.hasParameterGuard(unit().function)))
        return;
    BodyBlock entry = m_frames[0].index->block(entryBlock);
    for (unsigned index = 0; index < entry.numberOfParameters() && !isUnreachable(); ++index)
        checkProof(referenceBoundTo(entry.parameter(index)), TypeWithoutProof::ofEntryParameter(*this, index), ProofSource::EntryParameter, m_origin);
}

void Walker::holdOperandsToWhatCalleeBelieves(const Draft& call)
{
    FunctionID callee = call.function();
    const LinkTables& link = facts().linked();
    CodeRole role = call.role();
    if (!m_mechanismsOfBuild.contains(Mechanism::Arrivals) || callee.index() >= link.functions.size() || (role != CodeRole::Body && role != CodeRole::SlowBody && role != CodeRole::CoroutineBody && role != CodeRole::ParameterGuard))
        return;
    if (unit().function == callee && namesOnlyUnitsOfItsFunction(unit().role) && !m_restArrayAllocator)
        return;

    const Function& function = program().at(callee);
    std::span<const Reference> operands = call.opcode == Opcode::CallDirect ? call.variadicOperands() : call.argumentsOperands();
    size_t numberOfParameters = std::min<size_t>(operands.size(), function.numberOfEntryParameters());
    for (unsigned index = 0; index < numberOfParameters && !isUnreachable(); ++index) {
        Reference operand = operands[index];
        TypeID known = facts().typeOfEntryParameter(callee, index).proven;
        EntryParameter kind = function.entryParameterKind(index);
        bool isWrittenByProgram = kind == EntryParameter::This || (kind == EntryParameter::Parameter && index - function.firstParameterIndex() < function.numberOfPositionalParameters());
        if (!operand || !operand.isRecord() || !known || !isWrittenByProgram)
            continue;
        TypeID held = facts().typeBelievedOfEntryParameter(m_types, callee, index, known);
        if (TypeID tested = role == CodeRole::Body ? plan().conventions().typeBehindGuard(callee, index) : TypeID()) {
            TypeID passed = m_types.intersectionOf(held, tested);
            held = isNeverType(passed) ? tested : passed;
        }
        Representation form = representationOf(operand);
        TypeID shown = form == Representation::Void ? typeOf(operand) : typeShown(operand);
        if (!m_types.at(held).isJavaScriptValue() || m_types.isSubset(shown, held))
            continue;
        if (form != Representation::JSValue && form != Representation::Cell && form != Representation::Cell32 && !numberRepresentations.contains(form))
            operand = convert(operand, Representation::JSValue, call.origin());
        if (operand && !isUnreachable())
            buildCheckOfProof(operand, held, TypeWithoutProof::restingOnBelief(shown), ProofSource::EntryParameter, call.origin(), true);
    }
}

void Walker::checkProofsOfOperandsOfDirectCall(const Draft& call)
{
    FunctionID callee = call.function();
    const LinkTables& link = facts().linked();
    if (callee.index() >= link.functions.size() || (call.role() != CodeRole::Body && call.role() != CodeRole::CoroutineBody && call.role() != CodeRole::ParameterGuard))
        return;
    const LinkedFunction& linked = link.function(callee);
    if (!linked.everyCallerIsKnown || plan().conventions().holdsFusedCallEntry({ callee, call.role() }))
        return;

    bool isWalkingBody = unit().role == CodeRole::Body || unit().role == CodeRole::CoroutineBody || unit().role == CodeRole::SlowBody;
    if (unit().function == callee && (isWalkingBody ? call.role() == CodeRole::CoroutineBody : call.role() == CodeRole::ParameterGuard))
        return;
    bool knowsWhatBodyGoesBy = unit().function == callee && !isWalkingBody;
    uint32_t index = 0;
    for (Reference operand : call.variadicOperands()) {
        TypeID proven = knowsWhatBodyGoesBy ? typeArrivingAtEntryParameter(index) : index < linked.numberOfParameters ? link.parameters[linked.firstParameter + index].proven : TypeID();
        checkProofOfCopy(operand, proven, ProofSource::EntryParameter, call.origin());
        ++index;
    }
}

static ALWAYS_INLINE ProofSource sourceOfProofInWalk(Opcode opcode)
{
    switch (opcode) {
    case Opcode::GetModuleVariable:
        return ProofSource::Variable;
    case Opcode::GetCapture:
        return ProofSource::Capture;
    case Opcode::GetEnvironmentField:
        return ProofSource::SlotOfScope;
    default:
        break;
    }
    if (slotOfHolderOf(opcode) != SlotOfHolder::None)
        return ProofSource::Contents;
    switch (accessClassOf(opcode)) {
    case AccessClass::DirectCall:
    case AccessClass::IndirectCall:
        return opcode == Opcode::CallDirect || opcode == Opcode::CallTable ? ProofSource::ResultOfCall : ProofSource::ResultOfOtherCall;
    case AccessClass::Slot:
    case AccessClass::ByName:
        return ProofSource::Field;
    case AccessClass::None:
        return ProofSource::OtherOperation;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr OpcodeSet readsThatHandOverWhatLies {
    Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::GetDeferredClosureSlot, Opcode::GetDeferredClosureSlotOrUndefined, Opcode::GetStaticFieldOrAbsent, Opcode::GetEnvironmentField, Opcode::GetModuleVariable,
    Opcode::GetCapture, Opcode::GetGlobalBinding, Opcode::GetArrayElement, Opcode::GetContiguousIndex, Opcode::GetOwnContiguousIndex, Opcode::GetPropertyOfHeir, Opcode::GetWordOfBoundFunction, Opcode::GetAssignedMember,
};

static constexpr RepresentationSet rawFormsOfJavaScriptValuesInWalk { Representation::Boolean, Representation::Int32, Representation::Double, Representation::OptionalInt64, Representation::OptionalDouble };
static constexpr OpcodeSet callsByValueInWalk = opcodesWhere([](Opcode opcode) {
    return accessClassOf(opcode) == AccessClass::IndirectCall && resultRuleOf(opcode) == ResultRule::Unknown && opcode != Opcode::CallTable;
});

TypeID Walker::typeThatCallByValueIsShownToGive(const auto& call, std::span<const TypeID> shownOfOperands)
{
    TypeID given = Typed::resultTypeOf(TypingContext { program(), m_types, *m_positionsKnowingWhatFunctionsHandBack, TypeLevel::Proven }, call, OperandTypes { shownOfOperands });
    if (!given)
        return TypeTable::anything;
    TypeID callee = shownOfOperands[OperandIndex::Call::callee];
    bool entersBuiltin = callee && !holdsOnlyPrimitives(callee) && m_types.at(callee).isObject() && singleBuiltinOf(m_types, m_types.at(callee));
    return entersBuiltin ? m_types.wholeKindsAndBuiltinClassesOf(given) : given;
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::askWhatCallByValueGives(ValueID call, const Draft& draft, TypeID proven)
{
    if (m_types.isUninformative(proven) || unitReturnsAtOnce(draft))
        return;
    Vector<TypeID, 8> shownOfOperands;
    for (Reference operand : draft.operands())
        shownOfOperands.append(operand && operand.isRecord() ? typeShown(operand) : TypeID());
    TypeID shown = typeThatCallByValueIsShownToGive(draft, shownOfOperands.span());
    if (m_types.isSubset(shown, proven))
        return;
    if (!isNeverType(proven)) {
        buildCheckOfProof(Reference::value(call), proven, TypeWithoutProof::restingOnBelief(shown), ProofSource::ResultOfOtherCall, draft.origin(), true);
        return;
    }
    if (Reference callee = draft.operands()[OperandIndex::Call::callee]; callee && callee.isValue())
        buildCheckOfProof(callee, TypeTable::never, TypeTable::anything, ProofSource::ResultOfOtherCall, draft.origin());
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Walker::isTypedByWhatLinkSaysOfContents(const Draft& draft) const
{
    return m_positions == m_positionsOfBuild.get()
        && ((m_resultOfReadBeingTyped && readsTypedByWhatIsProvenOfInput.contains(draft.opcode)) || (m_contentsBeingTyped && recordsTypedByWhatIsProvenOfContents.contains(draft.opcode)));
}

ProofSource Walker::sourceOfProofOf(const Draft& draft) const
{
    return isTypedByWhatLinkSaysOfContents(draft) ? ProofSource::Contents : sourceOfProofInWalk(draft.opcode);
}

void Walker::checkProofOfRecord(ValueID record, const Draft& draft, TypeID proven)
{
    if (draft.opcode == Opcode::Narrowed) {
        Representation form = m_code.at(record).representation;
        bool hasBox = form == Representation::JSValue || form == Representation::Cell || (form == Representation::Cell32 && checksProofs());
        bool isRawNumberHeldUnderWord = checksProofs() && (numberRepresentations.contains(form) || optionalRepresentations.contains(form));
        if (Reference narrowed = draft.valueOperand(); narrowed && narrowed.isValue() && (hasBox || isRawNumberHeldUnderWord || checksWalk()))
            checkProof(Reference::value(record), TypeWithoutProof::restingOnBelief(typeAsBuilt(narrowed)), ProofSource::Test, draft.origin());
        return;
    }
    if (draft.opcode == Opcode::GetModuleVariable && !draft.returnsRawSlot()) {
        PlannedVariable planned = plan().variable(draft.variable());
        bool givesClosureThatTableKeeps = planned.function && (!planned.hasWordInStatics() || planned.encoding == VariableEncoding::FunctionAsZero) && !m_types.at(proven).primitives && singleFunctionOf(m_types, m_types.at(proven)) == planned.function;
        if (givesClosureThatTableKeeps) {
            Draft word(Opcode::GetModuleVariable, draft.origin());
            word.setVariable(draft.variable());
            chooseConservativeFields(word);
            word.setReturnsRawSlot(true);
            build(WTF::move(word));
            return;
        }
    }
    switch (resultRuleOf(draft.opcode)) {
    case ResultRule::Natural:
    case ResultRule::Declared:
    case ResultRule::Unknown:
        break;
    case ResultRule::None:
    case ResultRule::Constant:
    case ResultRule::Allocation:
    case ResultRule::Cast:
    case ResultRule::Results:
        return;
    }
    if (draft.opcode == Opcode::Narrowed || holdsNoneOfItsResultsInWalk(program(), draft))
        return;
    if (unitReturnsAtOnce(draft))
        return;

    TypeWithoutProof withoutProof = TypeWithoutProof::ofRecord(*this, draft, record);
    ProofSource source = sourceOfProofOf(draft);
    uint32_t numberChecked = numberOfProofsChecked(source);
    if (withoutProof.type() != proven)
        checkProof(Reference::value(record), withoutProof, source, draft.origin());
    if (numberChecked == numberOfProofsChecked(source) && readsThatHandOverWhatLies.contains(draft.opcode))
        checkThatReadGivesValue(Reference::value(record), source, draft.origin());
}

static bool codeGoesByTypesOfOperandsInWalk(const Operation& record)
{
    if (recordsTypedByWhatIsProvenOfContents.contains(record.opcode))
        return true;
    return familyOf(record.opcode) == OperationFamily::Operator && !(PayloadWord::form(record.opcode) && record.form() == OperatorForm::Generic);
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Walker::entryParameterIsCapture(const Block& entry, ValueID parameter) const
{
    if (namesOnlyUnitsOfItsFunction(unit().role))
        return false;
    const Function& function = program().at(unit().function);
    size_t index = entry.placeOfParameter(parameter);
    return index < function.numberOfEntryParameters() && function.entryParameterKind(static_cast<unsigned>(index)) == EntryParameter::Capture;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Walker::entryParameterIsShown(const Block& entry, ValueID parameter) const
{
    switch (unit().role) {
    case CodeRole::Body:
    case CodeRole::CoroutineBody:
    case CodeRole::SlowBody:
    case CodeRole::ParameterGuard:
    case CodeRole::ModuleStart:
        return isEnforced(entryParameterIsCapture(entry, parameter) ? ProofSource::Capture : ProofSource::EntryParameter);
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        return namesOnlyUnitsOfItsFunction(unit().role) && isEnforced(ProofSource::EntryParameter);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeID typeOfFunctionThatVariableIsBornWith(const Program& program, const Plan& plan, VariableID id)
{
    if (plan.promotedEnvironments().find(id))
        return { };
    const Variable& variable = program.at(id);
    if (variable.initialization != VariableInitialization::Function || variable.owner || !variable.flags.contains(VariableFlags::IsImmutable) || !variable.declared.isEstablished())
        return { };
    if (variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }) || plan.runTimeMayFill(id))
        return { };
    return variable.declared.written();
}

TypeID Walker::typeShownAtOnce(ValueID id) const
{
    if (!m_typesAsked.isEmpty()) {
        if (auto asked = m_typesAsked.find(id.index()); asked != m_typesAsked.end())
            return asked->value;
    }
    const Operation& record = m_code.at(id);
    if (m_code.isConstant(id))
        return record.type;
    bool isEntryParameter = record.opcode == Opcode::Parameter && m_code.at(record.block()).isEntry;
    bool isReadOfSlotWithDoors = isEnforced(ProofSource::Field) && record.opcode == Opcode::GetSlot && record.level() == KnowledgeLevel::Proven;
    bool isParameterOfJoin = m_dropsChecksOfProofs && record.opcode == Opcode::Parameter && !isEntryParameter;
    if (numberRepresentations.contains(record.representation) && resultRuleOf(record.opcode) != ResultRule::Natural && record.opcode != Opcode::Narrowed && !isEntryParameter && !isParameterOfJoin && !isReadOfSlotWithDoors)
        return widestTypeOf(record.representation);
    if (record.representation == Representation::Boolean)
        return TypeTable::boolean;
    if (record.opcode == Opcode::Parameter) {
        const Block& block = m_code.at(record.block());
        if (block.isEntry && !numberRepresentations.contains(record.representation) && entryParameterIsCapture(block, id) && entryParameterIsShown(block, id))
            return { };
        if (block.isEntry)
            return entryParameterIsShown(block, id) && !entryParameterIsCapture(block, id) ? record.type : numberRepresentations.contains(record.representation) ? widestTypeOf(record.representation) : TypeID(TypeTable::anything);
        if (!block.isHandlerBlock() && (!block.isLoopHead || m_headsOfClosedLoops.contains(record.block().index())) && !m_genericVersionEntries.contains(record.block().index()))
            return { };
        return numberRepresentations.contains(record.representation) ? widestTypeOf(record.representation) : TypeID(TypeTable::anything);
    }
    if (record.opcode == Opcode::Narrowed)
        return { };
    if (record.opcode == Opcode::GetArrayElement && record.representation == Representation::OptionalDouble && record.arrayStorage() == ArrayStorage::Double && record.level() == KnowledgeLevel::Proven)
        return TypeArena::fromPrimitives(Primitive::Number | Primitive::Undefined);
    switch (resultRuleOf(record.opcode)) {
    case ResultRule::None:
        return TypeTable::anything;
    case ResultRule::Constant:
    case ResultRule::Allocation:
        return record.type;
    case ResultRule::Cast:
        return { };
    case ResultRule::Natural:
    case ResultRule::Declared:
    case ResultRule::Unknown:
    case ResultRule::Results:
        break;
    }
    if (record.opcode == Opcode::CallBuiltin) {
        Type written = m_types.at(record.type);
        return written.isJavaScriptValue() && !(written.primitives & (Primitive::Absent | Primitive::Unspecified)) ? m_types.wholeKindsAndBuiltinClassesOf(record.type) : TypeID(TypeTable::anything);
    }
    if (record.opcode == Opcode::GetModuleVariable) {
        if (TypeID born = typeOfFunctionThatVariableIsBornWith(program(), plan(), record.variable()))
            return born;
    }
    ProofSource source = sourceOfProofInWalk(record.opcode);
    if (isEnforced(source) && (source == ProofSource::SlotOfScope || source == ProofSource::Capture || source == ProofSource::Variable || callsByValueInWalk.contains(record.opcode) || (record.opcode == Opcode::GetSlot && record.level() == KnowledgeLevel::Proven) || (record.opcode == Opcode::GetSlotOrUndefined && record.looksAboveEmptySlot())))
        return { };
    if (record.opcode == Opcode::ResultAt || (source != ProofSource::OtherOperation && !recordsTypedByWhatIsProvenOfContents.contains(record.opcode)))
        return isEnforced(source) && source != ProofSource::Field && source != ProofSource::ResultOfOtherCall ? record.type : TypeID(TypeTable::anything);
    TypeID widest = widestPrimitiveResultOf(record.opcode);
    return widest == record.type && !codeGoesByTypesOfOperandsInWalk(record) ? widest : TypeID();
}

void Walker::forEachValueThatTypeShownLeansOn(ValueID id, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    const Operation& record = m_code.at(id);
    if (record.opcode == Opcode::Parameter) {
        const Block& block = m_code.at(record.block());
        size_t index = block.placeOfParameter(id);
        m_code.seal(record.block()).forEach([&](const Edge& edge) {
            if (index < edge.arguments().size())
                functor(edge.arguments()[index]);
        });
        return;
    }
    if (record.opcode != Opcode::Narrowed && resultRuleOf(record.opcode) != ResultRule::Cast) {
        for (ValueID operand : record.operands()) {
            if (operand)
                functor(operand);
        }
        return;
    }
    if (ValueID behind = record.operand(0))
        functor(behind);
    if (ValueID test = record.opcode == Opcode::Narrowed ? record.testOperand() : ValueID()) {
        for (ValueID operand : m_code.at(test).operands()) {
            if (operand)
                functor(operand);
        }
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES TypeID Walker::computeTypeShown(ValueID id)
{
    const Operation& record = m_code.at(id);
    auto shown = [&](ValueID value) {
        TypeID atOnce = typeShownAtOnce(value);
        return atOnce ? atOnce : m_typesShown.get(value.index());
    };
    if (record.opcode == Opcode::Parameter && m_code.at(record.block()).isEntry)
        return typeThatUnitMayTestFor(m_types.testablePartOf(record.type), true, true);
    if (record.opcode == Opcode::Parameter) {
        const Block& block = m_code.at(record.block());
        size_t index = block.placeOfParameter(id);
        TypeID joined;
        bool everyEdgeHandsOverEachParameter = true;
        m_code.seal(record.block()).forEach([&](const Edge& edge) {
            if (edge.arguments().size() != block.parameters().size()) {
                everyEdgeHandsOverEachParameter = false;
                return;
            }
            TypeID arriving = shown(edge.arguments()[index]);
            joined = joined ? m_types.unionOf(joined, arriving) : arriving;
        });
        if (!everyEdgeHandsOverEachParameter || !joined)
            joined = TypeTable::anything;
        if (!numberRepresentations.contains(record.representation))
            return joined;
        TypeID toldByForm = widestTypeOf(record.representation);
        TypeID both = m_types.intersectionOf(joined, toldByForm);
        return isNeverType(both) ? toldByForm : both;
    }
    auto besideEmptyWord = [&](TypeID type) {
        TypeID rest = m_types.at(type).primitives & Primitive::Absent ? m_types.withoutPrimitives(type, Primitive::Absent) : type;
        return isNeverType(rest) ? type : rest;
    };
    auto withUnclaimedEmptyWord = [&](TypeID type, ValueID value) {
        return m_types.at(m_code.at(value).type).primitives & Primitive::Absent ? type : besideEmptyWord(type);
    };
    TypingContext believingNothing { program(), m_types, *m_positionsKnowingNothing, TypeLevel::Proven };
    Vector<TypeID, 8> operands;
    if (record.opcode == Opcode::GetEnvironmentField || record.opcode == Opcode::GetCapture || record.opcode == Opcode::GetModuleVariable)
        return typeThatUnitMayTestFor(m_types.testablePartOf(record.type), true, true);
    if (record.opcode == Opcode::GetSlotOrUndefined && record.looksAboveEmptySlot() && !numberRepresentations.contains(record.representation)) {
        ValueID object = record.objectOperand();
        TypeID held = object && m_types.isSubset(besideEmptyWord(shown(object)), m_types.forLayoutSet(record.receivers())) ? typeThatDoorsHoldOfSlotRead(record) : TypeID();
        return held ? m_types.unionOf(held, m_types.withoutObjects(record.type)) : TypeID(TypeTable::anything);
    }
    if (record.opcode == Opcode::GetSlot) {
        ValueID object = record.objectOperand();
        TypeID held = object && m_types.isSubset(besideEmptyWord(shown(object)), m_types.forLayoutSet(record.receivers())) ? typeThatDoorsHoldOfSlotRead(record) : TypeID();
        if (numberRepresentations.contains(record.representation)) {
            TypeID toldByForm = widestTypeOf(record.representation);
            TypeID both = held ? m_types.intersectionOf(held, toldByForm) : toldByForm;
            return isNeverType(both) ? toldByForm : both;
        }
        if (!held)
            return TypeTable::anything;
        return m_types.withPrimitives(held, Primitive::Absent);
    }
    if (callsByValueInWalk.contains(record.opcode)) {
        for (ValueID operand : record.operands())
            operands.append(operand ? shown(operand) : TypeID());
        return typeThatCallByValueIsShownToGive(record, operands.span());
    }
    if (record.opcode != Opcode::Narrowed && resultRuleOf(record.opcode) != ResultRule::Cast) {
        bool codeGoesByTypesOfOperands = codeGoesByTypesOfOperandsInWalk(record);
        auto asFormHolds = [&](TypeID type) {
            if (!numberRepresentations.contains(record.representation))
                return type;
            TypeID both = m_types.intersectionOf(type, widestTypeOf(record.representation));
            return isNeverType(both) ? widestTypeOf(record.representation) : both;
        };
        for (ValueID operand : record.operands()) {
            TypeID type = operand ? shown(operand) : TypeID();
            Representation form = operand ? m_code.at(operand).representation : Representation::Void;
            if (operand && codeGoesByTypesOfOperands && !numberRepresentations.contains(form) && form != Representation::Boolean && !m_types.isSubset(type, m_code.at(operand).type))
                return asFormHolds(TypeTable::anything);
            operands.append(type);
        }
        if (findIllTypedOperand(believingNothing, record, OperandTypes { operands.span() }))
            return asFormHolds(TypeTable::anything);
        TypeID given = Typed::resultTypeOf(believingNothing, record, OperandTypes { operands.span() });
        return asFormHolds(given ? given : TypeID(TypeTable::anything));
    }
    ValueID behind = record.operand(0);
    TypeID ofValue = behind ? shown(behind) : TypeID(TypeTable::anything);
    uint16_t emptyWord = behind ? m_types.at(ofValue).primitives & Primitive::Absent & ~m_types.at(m_code.at(behind).type).primitives : 0;
    if (behind && passThroughChecks.contains(record.opcode) && m_types.isSubset(withUnclaimedEmptyWord(ofValue, behind), m_code.at(behind).type)) {
        operands.append(withUnclaimedEmptyWord(ofValue, behind));
        TypeID passed = Typed::resultTypeOf(believingNothing, record, OperandTypes { operands.span() });
        return passed ? m_types.withPrimitives(passed, emptyWord) : ofValue;
    }
    ValueID testID = record.opcode == Opcode::Narrowed ? record.testOperand() : ValueID();
    if (!testID || record.narrowsHolderOfOperand() || record.narrowsOtherResultOfOperand() || record.narrowsTermOfOperand())
        return ofValue;
    const Operation& test = m_code.at(testID);
    uint32_t operandIndex = record.operandIndex();
    if (provesOf(test.opcode) == Proof::None || operandIndex >= test.numberOfOperands() || !test.operand(operandIndex))
        return ofValue;
    bool asksInFull = FlagMask::asksInFull(test.opcode) && test.asksInFull();
    for (ValueID operand : test.operands()) {
        TypeID type = !operand ? TypeID() : asksInFull ? shown(operand) : withUnclaimedEmptyWord(shown(operand), operand);
        if (operand && !asksInFull && !m_types.isSubset(type, m_code.at(operand).type))
            return ofValue;
        operands.append(type);
    }
    operands[operandIndex] = behind && !asksInFull ? withUnclaimedEmptyWord(ofValue, behind) : ofValue;
    TypeID left = narrowOperandByTest(believingNothing, test, OperandTypes { operands.span() }, operandIndex, record.isOnTakenEdge() ? EdgeSide::Taken : EdgeSide::NotTaken);
    bool hasReadThroughCell = test.opcode == Opcode::ShapeTest && record.isOnTakenEdge() && operandIndex == OperandIndex::ShapeTest::object;
    return !left ? ofValue : asksInFull || hasReadThroughCell ? left : m_types.withPrimitives(left, emptyWord);
}

NEVER_INLINE bool Walker::isShownAndReadThrough(Reference value, TypeID shown, TypeID held)
{
    if (!value.isValue() || !(m_types.at(shown).primitives & Primitive::Absent) || !m_types.isSubset(m_types.withoutPrimitives(shown, Primitive::Absent), held))
        return false;
    auto isConversion = [&](ValueID id) {
        Opcode opcode = m_code.at(id).opcode;
        return !m_code.isConstant(id) && opcode != Opcode::Parameter && (familyOf(opcode) == OperationFamily::Representation || opcode == Opcode::Narrowed || passThroughChecks.contains(opcode));
    };
    ValueID origin = value.id();
    while (isConversion(origin))
        origin = m_code.at(origin).operand(0);
    if (m_code.isConstant(origin))
        return false;
    for (ValueID id = m_code.lastOperationOf(m_code.at(origin).block()); id && id != origin; id = m_code.at(id).previous) {
        const Operation& read = m_code.at(id);
        if (read.opcode != Opcode::GetSlot || read.level() != KnowledgeLevel::Proven)
            continue;
        ValueID object = read.objectOperand();
        while (object && object != origin && isConversion(object))
            object = m_code.at(object).operand(0);
        if (object == origin)
            return true;
    }
    return false;
}

NEVER_INLINE TypeID Walker::typeClaimedOfHeadParameter(ValueID parameter, bool mayBeEmptyWord)
{
    const Operation& record = m_code.at(parameter);
    if (!mayBeEmptyWord || !record.type || !m_types.at(record.type).isJavaScriptValue() || (record.representation != Representation::JSValue && record.representation != Representation::Cell32))
        return record.type;
    return m_types.withPrimitives(record.type, Primitive::Absent);
}

NEVER_INLINE void Walker::dropChecksProvedAfterLoops()
{
    if (m_hasFailed || m_headsOfClosedLoops.isEmpty() || !m_openLoops.isEmpty())
        return;
    if (!m_numbersOfProofsChecked[static_cast<unsigned>(ProofSource::Field)] && !m_numbersOfProofsChecked[static_cast<unsigned>(ProofSource::EntryParameter)])
        return;
    auto hasOperandRepresentation = [&](ValueID id) {
        Opcode opcode = m_code.at(id).opcode;
        return opcode != Opcode::Parameter && (familyOf(opcode) == OperationFamily::Representation || opcode == Opcode::Narrowed || passThroughChecks.contains(opcode));
    };
    static constexpr unsigned mostSteps = 8;
    auto comesFromHeadOfLoop = [&](this const auto& self, ValueID id, unsigned steps) -> bool {
        while (id && !m_code.isConstant(id) && hasOperandRepresentation(id))
            id = m_code.at(id).operand(0);
        if (!id || m_code.isConstant(id) || steps == mostSteps)
            return false;
        const Operation& maker = m_code.at(id);
        if (maker.opcode == Opcode::Parameter)
            return !m_code.at(maker.block()).isEntry;
        if (maker.opcode == Opcode::GetSlot || (maker.opcode == Opcode::GetSlotOrUndefined && maker.looksAboveEmptySlot()))
            return self(maker.objectOperand(), steps + 1);
        return numberRepresentations.contains(maker.representation) && allNumericOperators.contains(maker.opcode) && std::ranges::any_of(maker.operands(), [&](ValueID operand) {
            return operand && self(operand, steps + 1);
        });
    };
    Vector<ValueID, 8> checks;
    for (uint32_t index = 0; index < m_code.numberOfValues(); ++index) {
        const Operation& check = m_code.at(ValueID(index));
        if (check.opcode != Opcode::CheckProof || check.isLeftToTestBehind() || (check.source() != ProofSource::Field && check.source() != ProofSource::EntryParameter) || !m_code.at(check.block()).isInLoop)
            continue;
        if (!comesFromHeadOfLoop(check.valueOperand(), 0))
            continue;
        checks.append(ValueID(index));
        for (ValueID id = check.valueOperand(); !m_code.isConstant(id); id = m_code.at(id).operand(0)) {
            removeTypeAsked(id);
            if (!hasOperandRepresentation(id))
                break;
        }
    }
    if (checks.isEmpty())
        return;
    m_typesShown.clear();
    SetForScope dropsChecksOfProofs(m_dropsChecksOfProofs, true);
    for (ValueID id : checks) {
        const Operation& check = m_code.at(id);
        if (m_code.isConstant(check.valueOperand()))
            continue;
        TypeID shown = typeShown(Reference::value(check.valueOperand()));
        if (check.source() == ProofSource::Field)
            shown = m_types.withoutPrimitives(shown, Primitive::Absent);
        if (isNeverType(shown) || (!m_types.isSubset(shown, check.checkedType()) && !isShownAndReadThrough(Reference::value(check.valueOperand()), shown, check.checkedType())))
            continue;
        m_code.leaveCheckOfProofToTestBehind(id);
    }
}

TypeID Walker::typeShown(Reference value)
{
    if (!value || !value.isRecord())
        return TypeTable::anything;
    if (TypeID atOnce = typeShownAtOnce(value.id()))
        return atOnce;
    auto isKnown = [&](ValueID id) {
        return typeShownAtOnce(id) || m_typesShown.contains(id.index());
    };
    Vector<ValueID, 16> setInThisTry;
    bool onlyEmptyWordIsNotClaimed = false;
    auto tryTakingHeadsAs = [&](bool asClaimed, bool mayBeEmptyWord) {
        Vector<ValueID, 16> waiting { value.id() };
        HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> comingRound;
        bool hasMetHead = false;
        while (!waiting.isEmpty()) {
            ValueID id = waiting.last();
            bool isComingRound = !comingRound.isEmpty() && comingRound.contains(id.index());
            if (!isComingRound && isKnown(id)) {
                waiting.removeLast();
                continue;
            }
            if (!isComingRound && m_code.at(id).opcode == Opcode::Parameter && m_code.at(m_code.at(id).block()).isLoopHead) {
                comingRound.add(id.index());
                hasMetHead = true;
                TypeID claimed = mayBeEmptyWord ? typeClaimedOfHeadParameter(id, true) : m_code.at(id).type;
                setTypeShown(id, asClaimed && claimed ? claimed : TypeID(TypeTable::anything));
                if (asClaimed)
                    setInThisTry.append(id);
            }
            size_t numberWaiting = waiting.size();
            forEachValueThatTypeShownLeansOn(id, [&](ValueID leanedOn) {
                if (!isKnown(leanedOn))
                    waiting.append(leanedOn);
            });
            if (waiting.size() != numberWaiting)
                continue;
            TypeID computed = computeTypeShown(id);
            bool isHeadComingRound = !comingRound.isEmpty() && comingRound.contains(id.index());
            if (TypeID claimed = mayBeEmptyWord ? typeClaimedOfHeadParameter(id, true) : m_code.at(id).type; asClaimed && isHeadComingRound && claimed && !m_types.isSubset(computed, claimed)) {
                onlyEmptyWordIsNotClaimed = m_types.isSubset(m_types.withoutPrimitives(computed, Primitive::Absent), claimed);
                return false;
            }
            setTypeShown(id, computed);
            if (asClaimed && hasMetHead)
                setInThisTry.append(id);
            if (!comingRound.isEmpty())
                comingRound.remove(id.index());
            waiting.removeLast();
        }
        return true;
    };
    bool isShown = tryTakingHeadsAs(true, false);
    for (bool asClaimed : { true, false }) {
        if (isShown)
            break;
        if (asClaimed && !onlyEmptyWordIsNotClaimed)
            continue;
        for (ValueID id : setInThisTry)
            removeTypeShown(id);
        setInThisTry.shrink(0);
        isShown = tryTakingHeadsAs(asClaimed, true);
    }
    RELEASE_ASSERT(isShown);
    return m_typesShown.get(value.id().index());
}

void Walker::checkThatReadGivesValue(Reference value, ProofSource source, Origin origin)
{
    if (!checksProofs() || !value || !value.isValue() || isUnreachable())
        return;
    TypeID claimed = typeAsBuilt(value);
    Representation representation = representationOf(value);
    if (!claimed || isNeverType(claimed) || !m_types.at(claimed).isJavaScriptValue() || (representation != Representation::JSValue && representation != Representation::Cell && (representation != Representation::Cell32 || !checksProofs())))
        return;
    TypeID anyWord = m_types.withPrimitives(TypeTable::anything, Primitive::Absent);
    origin.isTailCall = false;
    Draft check(Opcode::CheckProof, origin);
    check.setValueOperand(value);
    check.setCheckedType(m_types.at(claimed).primitives & Primitive::Absent ? anyWord : TypeID(TypeTable::anything));
    check.setTypeWithoutProof(anyWord);
    check.setSource(source);
    build(WTF::move(check));
}

constinit const OpcodeSet opcodesWhoseRuleGivesCells { Opcode::GetClassPrototype, Opcode::GetConstructPrototype, Opcode::NewDictionary, Opcode::NewRuntimeError, Opcode::NewSuppressedError, Opcode::CopyObject, Opcode::CopyObjectAndDefine, Opcode::CopyTwoObjects, Opcode::ToObject, Opcode::ToThis, Opcode::CreateAsyncFromSyncIterator, Opcode::TypeOf, Opcode::ToString, Opcode::Strcat, Opcode::StringBuilderToString, Opcode::StringBuilderFinish, Opcode::StackStringBuilderToString, Opcode::StackStringBuilderFinish, Opcode::Substring, Opcode::SubstringOrUndefined, Opcode::GetSuperBase, Opcode::ResolveHeritage, Opcode::FormatDateWithPattern, Opcode::FormatNumberWithPattern, Opcode::ToPropertyKey };

TypeWithoutProof TypeWithoutProof::ofRecord(Walker& walker, const Draft& draft, ValueID record)
{
    RELEASE_ASSERT(walker.m_positionsWithoutProofs);
    TypeArena& types = walker.types();
    bool isRawNumberOfOperator = allNumericOperators.contains(draft.opcode) && numberRepresentations.contains(walker.code().at(record).representation);
    TypeID withoutProof = TypeTable::anything;
    bool isCodeInLineTypedByRow = Operation::opcodesWithOverload.contains(draft.opcode) && draft.opcode != Opcode::CallBuiltin && walker.code().at(record).representation != Representation::Void;
    Representation formOfRecord = walker.code().at(record).representation;
    bool isBoxedAndTypedByRuleOfOurs = opcodesWhoseRuleGivesCells.contains(draft.opcode) && (formOfRecord == Representation::JSValue || formOfRecord == Representation::Cell);
    if (draft.opcode != Opcode::ResultAt && !isRawNumberOfOperator && !isCodeInLineTypedByRow && !isBoxedAndTypedByRuleOfOurs) {
        walker.typesOfOperands(draft);
        for (TypeID& type : walker.m_operandTypes) {
            if (type && types.at(type).isJavaScriptValue())
                type = types.testablePartOf(type);
        }
        if (TypeID given = Typed::resultTypeOf(TypingContext { walker.program(), types, *walker.m_positionsWithoutProofs, TypeLevel::Proven, false }, draft, OperandTypes { walker.m_operandTypes.span() }))
            withoutProof = given;
    }
    if (draft.opcode == Opcode::GetModuleVariable || draft.opcode == Opcode::GetEnvironmentField || draft.opcode == Opcode::GetCapture)
        withoutProof = types.withPrimitives(withoutProof, Primitive::Absent);
    return TypeWithoutProof(withoutProof, FoundWhileWalking { });
}

void Walker::checkProof(Reference value, TypeWithoutProof withoutProof, ProofSource source, Origin origin)
{
    if (!value || !value.isValue() || isUnreachable())
        return;
    TypeID proven = typeAsBuilt(value);
    Representation representation = representationOf(value);
    bool liesRaw = numberRepresentations.contains(representation) || optionalRepresentations.contains(representation) || representation == Representation::Boolean;
    if (!proven || isNeverType(proven) || (!liesRaw && representation != Representation::JSValue && representation != Representation::Cell && (representation != Representation::Cell32 || !checksProofs())))
        return;
    if (liesRaw && m_types.at(withoutProof.type()).isJavaScriptValue())
        withoutProof = withoutProof.metWithWhatFormTells(m_types, representation);
    if (!m_types.at(proven).isJavaScriptValue() || !m_types.at(withoutProof.type()).isJavaScriptValue() || m_types.isSubset(withoutProof.type(), proven))
        return;
    checkProof(value, proven, withoutProof, source, origin);
}

void Walker::checkProofOfCopy(Reference value, TypeID proven, ProofSource source, Origin origin)
{
    if (!value || !value.isValue() || !proven || isUnreachable())
        return;
    Representation representation = representationOf(value);
    TypeID own = typeAsBuilt(value);
    bool liesRaw = numberRepresentations.contains(representation) || optionalRepresentations.contains(representation) || representation == Representation::Boolean;
    if (!own || isNeverType(own) || (!liesRaw && representation != Representation::JSValue && representation != Representation::Cell && (representation != Representation::Cell32 || !checksProofs())))
        return;
    if (!m_types.at(proven).isJavaScriptValue() || !m_types.at(own).isJavaScriptValue() || m_types.isSubset(own, proven))
        return;
    checkProof(value, proven, TypeWithoutProof::restingOnBelief(own), source, origin);
}

void Walker::checkRawNumberThatFoldRestsOn(Reference number, Origin origin)
{
    if (!checksProofs() || !number || !number.isValue() || !numberRepresentations.contains(representationOf(number)))
        return;
    origin.isTailCall = false;
    checkProof(number, TypeTable::anything, ProofSource::Test, origin);
}

TypeID Walker::asFarAsUnitMayAsk(TypeID claimed)
{
    if (claimed && !isNeverType(claimed) && !holdsOnlyPrimitives(claimed) && m_types.at(claimed).hasUnboundedUnknown() && m_types.at(claimed).primitives & Primitive::Number && m_types.at(claimed).primitives & Primitive::AnyString)
        return TypeID();
    if (claimed && (m_types.at(claimed).primitives & Primitive::Integer) && !m_types.at(claimed).integers.isSingleValue() && m_types.at(claimed).integers.isSubsetOf(Range::int32()))
        claimed = m_types.unionOf(claimed, TypeTable::int32);
    if (!claimed || holdsOnlyPrimitives(claimed) || !m_types.at(claimed).isJavaScriptValue())
        return claimed;
    TypeID testable = m_types.testablePartOf(claimed);
    TypeID asked = typeThatUnitMayTestFor(testable);
    return asked == testable ? claimed : asked;
}

TypeID Walker::typeThatUnitMayTestFor(TypeID tested, bool mayBringClasses, bool asOfUnitWithoutReach)
{
    bool mentionsWhatIsOutOfReach = facts().testMentionsWhatIsOutOfReach(m_types, tested, scopedLambda<bool(FunctionID)>([&](FunctionID function) {
        if (asOfUnitWithoutReach)
            return false;
        if (!mayBringClasses && (isClassConstructorOf(program().at(function).kind) || program().at(function).constructedClass))
            return false;
        return m_functionsInReachOfUnit.ensure(function.index(), [&] {
            return facts().linked().mayBeMentionedBy(unit(), function);
        }).iterator->value;
    }));
    if (!mentionsWhatIsOutOfReach)
        return tested;
    return m_types.unionOf(m_types.withoutObjects(tested), m_types.at(tested).hasOnlyClosures() ? TypeTable::function : TypeTable::object);
}

Reference Walker::proveOrTrap(Reference value, TypeID tested, ProofSource source, Origin origin)
{
    origin.isTailCall = false;
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(value);
    test.setTestedType(tested);
    Reference passes = build(WTF::move(test));
    if (!passes)
        return { };

    BlockID passing = addBlock();
    BlockID failing = addBlock(BlockFrequency::Rare);
    branch(passes, passing, failing);
    continueIn(failing);
    buildCheckOfProof(value, TypeTable::never, TypeTable::anything, source, origin);
    endAsUnreachable(origin);
    continueIn(passing);

    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(passes);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    return build(WTF::move(narrowed));
}

Reference Walker::buildTestOfCheck(Draft&& test)
{
    uint32_t numberOfRecordsInFront = m_code.numberOfValues();
    SetForScope buildInPlaceScope(m_isBuildingInPlace, true);
    Reference answer = build(WTF::move(test));
    if (!isUnreachable() && !hasFailed() && !wasBuiltInPlaceSince(numberOfRecordsInFront, answer))
        ++m_foldedProofCheckCount;
    return answer;
}

bool Walker::comesFromInFrontOfLoopAround(Reference answer) const
{
    uint32_t numberOfLoopsAround = std::min<uint32_t>(m_openLoops.size(), m_numberOfLoopsThatMayBeLeft);
    if (!numberOfLoopsAround)
        return false;
    while (answer && answer.isValue()) {
        const Operation& record = m_code.at(answer.id());
        if (record.opcode != Opcode::Narrowed && record.opcode != Opcode::Box && record.opcode != Opcode::Unbox)
            break;
        answer = Reference::value(record.valueOperand());
    }
    return answer && answer.isValue() && isInvariantInLoop(numberOfLoopsAround - 1, answer);
}

bool Walker::wasBuiltInPlaceSince(uint32_t numberOfRecordsInFront, Reference answer) const
{
    return answer && answer.isValue() && answer.id().index() >= numberOfRecordsInFront && !comesFromInFrontOfLoopAround(answer);
}

void Walker::checkThatMemoryAnswersAsBelieved(Reference believed, Draft&& readAgain)
{
    if (isUnreachable() || hasFailed() || !believed)
        return;
    Origin origin = readAgain.origin();
    origin.isTailCall = false;
    uint32_t numberOfRecordsInFront = m_code.numberOfValues();
    Reference found = buildTestOfCheck(WTF::move(readAgain));
    if (isUnreachable() || !wasBuiltInPlaceSince(numberOfRecordsInFront, found))
        return;
    checkThatValuesAreTheSame(found, believed, origin);
}

void Walker::checkThatValuesAreTheSame(Reference found, Reference believed, Origin origin)
{
    auto isSame = [&](Reference left, Reference right) -> Reference {
        if (!left || !right || isUnreachable())
            return { };
        Draft comparison(Opcode::StrictEq, origin);
        chooseConservativeFields(comparison);
        comparison.setLeftOperand(left);
        comparison.setRightOperand(right);
        return buildTestOfCheck(WTF::move(comparison));
    };
    auto check = [&](Reference holds) {
        if (holds && holds.isValue() && !isUnreachable())
            buildCheckOfProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
    };
    Reference areSame = isSame(found, believed);
    if (!(m_types.at(typeOf(believed)).primitives & Primitive::Double)) {
        check(areSame);
        return;
    }
    Reference foundIsItself = isSame(found, found);
    check(isSame(areSame, foundIsItself));
    check(isSame(foundIsItself, isSame(believed, believed)));
}

void Walker::verifyRememberedTest(Draft&& test)
{
    if (m_isBuildingInPlace)
        return;
    uint32_t numberOfRecordsInFront = m_code.numberOfValues();
    Origin origin = test.origin();
    Reference holds = buildTestOfCheck(WTF::move(test));
    if (holds && holds.isValue() && !isUnreachable())
        buildCheckOfProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
    for (uint32_t index = numberOfRecordsInFront; index < m_code.numberOfValues(); ++index)
        m_numberOfRecordsBuiltToAskGuardsAgain += !m_code.isConstant(ValueID(index));
}

void Walker::askQuestionOfTruthAgainWhereWaysAnswerIt(const BodyValue& question)
{
    if (!checksProofsOf(ProofSource::RememberedFact) || m_isBuildingInPlace || isUnreachable() || hasFailed())
        return;
    Draft again = draftOf(question);
    chooseConservativeFields(again);
    Draft comparison(Opcode::StrictEq, m_origin);
    chooseConservativeFields(comparison);
    comparison.setLeftOperand(buildTestOfCheck(WTF::move(again)));
    comparison.setRightOperand(booleanOfUnit(m_threadedConditionValue));
    verifyRememberedTest(WTF::move(comparison));
}

void Walker::askByteOfMemberAgainWhereNoteIsLeanedOn(BuiltinID member)
{
    if (!checksProofs() || m_isBuildingInPlace || isUnreachable() || hasFailed())
        return;
    uint32_t numberOfRecordsInFront = m_code.numberOfValues();
    Origin origin = m_origin;
    origin.isTailCall = false;
    Draft test(Opcode::IsMemberAsBuilt, origin);
    chooseConservativeFields(test);
    test.setBuiltin(member);
    Reference holds = buildTestOfCheck(WTF::move(test));
    if (holds && holds.isValue() && !isUnreachable())
        checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
    for (uint32_t index = numberOfRecordsInFront; index < m_code.numberOfValues(); ++index)
        m_numberOfRecordsBuiltToAskGuardsAgain += !m_code.isConstant(ValueID(index));
}

void Walker::askGuardsAgainWhereTheyAreLeanedOn(std::span<const MemberConditionRecord> remembered, Origin origin)
{
    origin.isTailCall = false;
    Draft test(Opcode::AreMembersUnchanged, origin);
    test.setConditions(remembered);
    verifyRememberedTest(WTF::move(test));
}

void Walker::checkProof(Reference value, TypeID proven, TypeWithoutProof withoutProof, ProofSource source, Origin origin)
{
    if (checksProofsOf(source))
        buildCheckOfProof(value, proven, withoutProof, source, origin, true);
}

std::optional<Origin> Walker::leaveCheckOfWhatContainerHasJustGivenToTestBehind(Reference given)
{
    if (Representation form = representationOf(given); form != Representation::JSValue && form != Representation::Cell)
        return std::nullopt;
    auto isCheckOfGiven = [&](const Operation& record) {
        return record.opcode == Opcode::CheckProof && record.valueOperand() == given.id();
    };
    uint32_t end = m_code.numberOfValues();
    uint32_t place = end;
    while (place && !isCheckOfGiven(m_code.at(ValueID(place - 1)))) {
        EffectTraits between = m_code.at(ValueID(--place)).traits();
        if (!between.isUnobservable() || between.mayCollect())
            return std::nullopt;
    }
    if (!place)
        return std::nullopt;
    ValueID asked(place - 1);
    const Operation& check = m_code.at(asked);
    if (check.source() != ProofSource::Contents || check.isLeftToTestBehind())
        return std::nullopt;
    Vector<ValueID, 2> names { given.id() };
    for (; place < end; ++place) {
        const Operation& between = m_code.at(ValueID(place));
        bool mentionsGiven = false;
        for (uint32_t operandIndex = 0; operandIndex < between.numberOfOperands(); ++operandIndex)
            mentionsGiven |= names.contains(between.operand(operandIndex));
        if (!mentionsGiven)
            continue;
        if (between.opcode != Opcode::Results)
            return std::nullopt;
        names.append(ValueID(place));
    }
    for (BlockID block = check.block(); block != m_block;) {
        const Terminator& last = m_code.at(block).terminator;
        if (!last.isSet || last.kind != TerminatorKind::Jump)
            return std::nullopt;
        const Edge& next = m_code.at(last.next());
        if (next.isOpen() || m_code.at(next.to).isLoopHead || std::ranges::find_first_of(next.arguments(), names) != next.arguments().end())
            return std::nullopt;
        block = next.to;
    }
    Origin origin = check.origin();
    m_code.leaveCheckOfProofToTestBehind(asked);
    removeTypeAsked(given.id());
    return origin;
}

BlockID Walker::splitNumberFromContainer(Reference given, Reference& number)
{
    if (!given.isValue() || !m_typesAsked.contains(given.id().index()))
        return { };
    TypeID rest = m_types.withoutNullish(typeOf(given));
    TypeID nullish = m_types.differenceOf(typeAsBuilt(given), rest);
    if (!m_types.at(rest).isNumber() || m_types.testablePartOf(rest) != rest || isNeverType(nullish) || !m_types.at(nullish).hasOnlyPrimitives(Primitive::UndefinedOrNull))
        return { };
    std::optional<Origin> originOfCheck = leaveCheckOfWhatContainerHasJustGivenToTestBehind(given);
    if (!originOfCheck)
        return { };
    Draft test(Opcode::TypeTest, *originOfCheck);
    test.setValueOperand(given);
    test.setTestedType(rest);
    Reference passes = build(WTF::move(test));
    if (!passes || !passes.isValue()) {
        fail("a test of what a container has given is decided without a look"_s);
        return { };
    }
    m_code.makeTypeTestAskInFull(passes.id());
    BlockID isOfRest = addBlock();
    BlockID isOther = addBlock(BlockFrequency::Rare);
    branch(passes, isOfRest, isOther);
    continueIn(isOther);
    buildCheckOfProof(given, nullish, TypeTable::anything, ProofSource::Contents, *originOfCheck, true);
    BlockID whereNullish = m_block;
    continueIn(isOfRest);
    Draft narrowed(Opcode::Narrowed, *originOfCheck);
    narrowed.setValueOperand(given);
    narrowed.setTestOperand(passes);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    number = build(WTF::move(narrowed));
    return whereNullish;
}

bool Walker::splitNullishOffWhatContainerHasJustGiven(Reference given, TypeID rest, Draft&& throwsOffNullish, const Effects* ofThrow)
{
    if (!given.isValue() || !m_typesAsked.contains(given.id().index()) || isNeverType(rest) || m_types.testablePartOf(rest) != rest)
        return false;
    TypeID nullish = m_types.differenceOf(typeAsBuilt(given), rest);
    if (isNeverType(nullish) || !m_types.at(nullish).hasOnlyPrimitives(Primitive::UndefinedOrNull))
        return false;
    std::optional<Origin> originOfCheck = leaveCheckOfWhatContainerHasJustGivenToTestBehind(given);
    if (!originOfCheck)
        return false;

    Origin origin = throwsOffNullish.origin();
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(given);
    test.setTestedType(rest);
    Reference passes = build(WTF::move(test));
    if (!passes)
        return true;
    if (!passes.isValue()) {
        fail("a test of what a container has given is decided without a look"_s);
        return true;
    }
    m_code.makeTypeTestAskInFull(passes.id());

    BlockID isOfRest = addBlock();
    BlockID isOther = addBlock(BlockFrequency::Rare);
    branch(passes, isOfRest, isOther);

    continueIn(isOther);
    buildCheckOfProof(given, nullish, TypeTable::anything, ProofSource::Contents, *originOfCheck, true);
    if (!isUnreachable()) {
        if (!isUnderHandler())
            m_state.clear();
        build(WTF::move(throwsOffNullish), ofThrow);
    }
    if (!isUnreachable())
        endAsUnreachable(origin);

    continueIn(isOfRest);
    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(given);
    narrowed.setTestOperand(passes);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    if (Reference tested = build(WTF::move(narrowed)))
        m_state.noteCheckedName(given, tested);
    return true;
}

bool Walker::letTestInFrontOfBranchAskWhatContainerHasJustGiven(const BodyValue& test)
{
    ValueID input = test.as<Opcode::TypeTest>().valueOperand();
    Reference given = referenceOf(input);
    if (!given || !given.isValue() || !m_typesAsked.contains(given.id().index()))
        return false;
    const BodyIndex& index = currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(test.id()));
    BodyTerminator end = block.terminator();
    if (end.kind() != TerminatorKind::Branch || end.operand(0) != test.id() || test.id().index() + 1 != block.firstValue().index() + block.numberOfValues())
        return false;
    TypeID whole = typeOf(given);
    TypeID tested = test.as<Opcode::TypeTest>().testedType();
    TypeID passing = m_types.intersectionOf(whole, tested);
    if (!m_types.isSubset(passing, tested))
        return false;
    TypeID failing = m_types.differenceOf(whole, passing);
    if (isNeverType(passing) || isNeverType(failing) || m_types.testablePartOf(passing) != passing || m_types.testablePartOf(failing) != failing)
        return false;
    std::optional<Origin> originOfCheck = canThreadBranchAfter(test) ? leaveCheckOfWhatContainerHasJustGivenToTestBehind(given) : std::nullopt;
    if (!originOfCheck)
        return false;

    bool asksForFailingSide = m_types.at(passing).isUndefinedOrNull();
    Origin origin = originOf(test);
    Draft first(Opcode::TypeTest, origin);
    first.setValueOperand(given);
    first.setTestedType(asksForFailingSide ? failing : passing);
    Reference isOfFirst = build(WTF::move(first));
    if (!isOfFirst)
        return true;
    if (!isOfFirst.isValue()) {
        fail("a test of what a container has given is decided without a look"_s);
        return true;
    }
    m_code.makeTypeTestAskInFull(isOfFirst.id());

    BlockID asked = addBlock();
    BlockID other = addBlock();
    branch(isOfFirst, asked, other);
    TypeID askedOnOtherWay = asksForFailingSide ? passing : failing;
    auto endArm = [&](bool isOnTakenEdge) {
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(given);
        narrowed.setTestOperand(isOfFirst);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(isOnTakenEdge);
        Draft answer(Opcode::Boolean, origin);
        answer.setBoolean(isOnTakenEdge != asksForFailingSide);
        answer.representation = Representation::JSValue;
        Reference part = build(WTF::move(narrowed));
        if (!isOnTakenEdge && part && part.isValue() && !isUnreachable() && m_code.at(part.id()).opcode == Opcode::Narrowed && m_code.at(part.id()).block() == m_block)
            setTypeAsked(part.id(), typeThatUnitMayTestFor(askedOnOtherWay, true));
        bind(input, part);
        bind(test.id(), constant(WTF::move(answer)));
        if (!m_hasFailed && !threadBranchAfter(test))
            fail("a way of a test does not walk the end of its block"_s);
        bind(input, given);
    };
    continueIn(other);
    buildCheckOfProof(given, askedOnOtherWay, TypeTable::anything, ProofSource::Contents, *originOfCheck, true);
    endArm(false);
    continueIn(asked);
    endArm(true);
    return true;
}

TypeID Walker::typeThatFormSays(Reference value)
{
    if (value.isConstant())
        return typeOf(value);
    switch (representationOf(value)) {
    case Representation::Void:
        return typeAsBuilt(value);
    case Representation::Boolean:
        return TypeTable::boolean;
    case Representation::Double:
        if (codeUnitBesideCharCode(*this, value))
            return m_types.withPrimitives(m_types.integer({ 0, UINT16_MAX }), Primitive::WholeNumber);
        [[fallthrough]];
    case Representation::Int32:
    case Representation::Int64:
        return TypeTable::number;
    case Representation::OptionalInt64:
        if (const Operation& read = m_code.at(value.id()); read.opcode == Opcode::GetTypedArrayElement && read.typedArrayType() != NotTypedArray)
            return m_types.withPrimitives(elementTypeOf(read.typedArrayType()), Primitive::Undefined);
        [[fallthrough]];
    case Representation::OptionalDouble:
        return m_types.withPrimitives(TypeTable::number, representedMarkerPrimitives);
    case Representation::Unchosen:
    case Representation::RawWord:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
        return { };
    }
    return { };
}

void Walker::buildCheckOfProof(Reference value, TypeID proven, TypeWithoutProof told, ProofSource source, Origin origin, bool mayBringClasses, bool isNotShownToFit, bool isConstantWrittenInBody, bool standsInFrontOfStoreOfValue)
{
    TypeID withoutProof = told.type();
    Reference asked = value;
    Representation form = representationOf(value);
    bool isAnswerOfTest = form == Representation::Boolean && !value.isConstant() && proven == TypeArena::fromPrimitives(Primitive::True);
    bool hasBox = !value.isConstant() && (form == Representation::JSValue || form == Representation::Cell || form == Representation::Cell32);
    bool isAskedInRawForm = false;
    TypeID saidWhereNothingPasses;
    if (!isAnswerOfTest && !hasBox) {
        TypeID said = typeThatFormSays(value);
        if (!said) {
            fail("a door is asked of a value whose form says nothing of it"_s);
            return;
        }
        bool isRawNumber = !value.isConstant() && (m_types.at(said).primitives & Primitive::Number);
        bool mayBeNumberThatIsProven = isRawNumber && (m_types.at(proven).primitives & Primitive::Number);
        if (m_types.isSubset(said, proven) || (mayBeNumberThatIsProven && source != ProofSource::EntryParameter && !checksProofsOf(source) && !isNotShownToFit && m_types.isSubset(typeAsBuilt(value), proven))) {
            ++m_impliedProofCount;
            return;
        }
        isAskedInRawForm = mayBeNumberThatIsProven && (numberRepresentations.contains(form) || ((checksProofsOf(source) || isNotShownToFit) && optionalRepresentations.contains(form)));
        if (!isNeverType(proven) && isNeverType(m_types.intersectionOf(said, proven))) {
            ++m_unsatisfiableProofCheckCount;
            bool goesToPlaceWithoutReceiver = source == ProofSource::Variable || source == ProofSource::Capture || source == ProofSource::SlotOfScope;
            if (m_inputs.options.verifiesCode && isConstantWrittenInBody && asked.isConstant() && goesToPlaceWithoutReceiver) {
                dataLogLn("AOT: f", currentFunction().index(), " at ", origin.position, ": a door of kind ", static_cast<unsigned>(source), " can never pass: what comes is ", TypeInText { m_types, said }, ", and what is proven is ", TypeInText { m_types, proven });
                fail("a door can never pass: a constant is disjoint from what is proven of the place that it goes to"_s, InternalErrorKind::StoredTypeMismatch);
                return;
            }
            saidWhereNothingPasses = said;
        }
    }
    bool isAskedAsNarrowReference = hasBox && form == Representation::Cell32 && checksProofs() && checksProofsOf(source);
    if (!isAnswerOfTest && !isAskedInRawForm && !isAskedAsNarrowReference && form != Representation::JSValue && form != Representation::Cell) {
        value = convert(value, Representation::JSValue, origin);
        if (!value || !value.isRecord() || isUnreachable())
            return;
    }
    TypeID checked = typeThatUnitMayTestFor(m_types.testablePartOf(proven), mayBringClasses);
    if (hasBox && asked.isValue() && !checksProofsOf(source) && (m_types.at(checked).primitives & Primitive::LiteralString)) {
        if (TypeID shown = m_types.intersectionOf(withoutProof, typeShown(asked)); !isNeverType(shown))
            withoutProof = shown;
    }
    if (m_types.isSubset(withoutProof, checked)) {
        ++m_untestableProofCount;
        return;
    }
    origin.isTailCall = false;
    Draft check(Opcode::CheckProof, origin);
    check.setValueOperand(value);
    check.setCheckedType(checked);
    check.setTypeWithoutProof(isAskedInRawForm || (value.isValue() && hasTestedForHole(m_code.at(value.id()))) ? withoutProof : m_types.withPrimitives(withoutProof, Primitive::Absent));
    check.setSource(source);
    check.setGoesWithItsValue(standsInFrontOfStoreOfValue && value == asked && value.isValue() && !saidWhereNothingPasses && !checksProofsOf(source));
    build(WTF::move(check));
    ++m_numbersOfProofsChecked[static_cast<unsigned>(source)];
    if (saidWhereNothingPasses && isNeverType(m_types.intersectionOf(saidWhereNothingPasses, checked))) {
        endAsUnreachable(origin);
        return;
    }
    if (isUnreachable() || !asked.isValue() || representationOf(asked) == Representation::Boolean || m_code.at(asked.id()).block() != m_block)
        return;
    TypeID shown = typeShown(asked);
    if (m_types.isSubset(shown, m_types.withPrimitives(withoutProof, Primitive::Absent)))
        setTypeAsked(asked.id(), m_types.intersectionOf(shown, checked));
}

void Walker::beginModuleInBody()
{
    ModuleID module = plan().moduleEvaluation().moduleThatBeginsIn(unit().function);
    if (!module || isUnreachable())
        return;

    Draft begin(Opcode::BeginModuleEvaluation, m_origin);
    begin.setModule(module);
    chooseConservativeFields(begin);
    Reference begins = build(WTF::move(begin));
    if (!begins)
        return;

    BlockID goesOn = addBlock();
    BlockID hasBegun = addBlock(BlockFrequency::Unlikely);
    branch(begins, goesOn, hasBegun);
    continueIn(hasBegun);
    TerminatorDraft terminator(TerminatorKind::Return, m_origin);
    terminator.setValueOperand(constant(Draft(Opcode::Undefined, m_origin)));
    endWithoutEdges(terminator);
    continueIn(goesOn);
}

const Body& Walker::currentBody() const
{
    return *m_frames.last().body;
}

bool isDirectlyRecursive(const Program& program, const BodyIndex& index, FunctionID id)
{
    const Function& function = program.at(id);
    auto inFrontOfChecks = [&](ValueID value) {
        while (value && !index.isParameter(value) && (index.opcodeOf(value) == Opcode::CheckInitialized || opcodesThatStateTypes.contains(index.opcodeOf(value))))
            value = index.value(value).operand(0);
        return value;
    };
    auto isEntryParameter = [&](ValueID value, EntryParameter kind) {
        return value && index.isParameter(value) && index.blockOf(value) == entryBlock && value.index() < function.numberOfEntryParameters() && function.entryParameterKind(value.index()) == kind;
    };
    auto takesCallee = [&] {
        for (uint32_t parameter = 0; parameter < function.numberOfEntryParameters(); ++parameter) {
            if (function.entryParameterKind(parameter) == EntryParameter::Callee)
                return true;
        }
        return false;
    };
    bool mayCallItself = index.opcodes().contains(Opcode::CallDirect)
        || (index.opcodes().contains(Opcode::Call) && (index.opcodes().contains(Opcode::GetModuleVariable) || (function.kind == FunctionKind::Method && index.opcodes().contains(Opcode::PropGet)) || takesCallee()));
    if (!mayCallItself)
        return false;
    bool callsItself = false;
    index.body().forEachBlock([&](BodyBlock block) {
        for (BodyValue value : block.values()) {
            if (callsItself)
                return;
            if (value.opcode() == Opcode::CallDirect) {
                callsItself = value.function() == id && !value.isConstruction();
                continue;
            }
            if (value.opcode() != Opcode::Call)
                continue;
            ValueID callee = inFrontOfChecks(value.as<Opcode::Call>().calleeOperand());
            if (!callee)
                continue;
            if (index.isParameter(callee)) {
                callsItself = isEntryParameter(callee, EntryParameter::Callee);
                continue;
            }
            BodyValue read = index.value(callee);
            if (read.opcode() == Opcode::GetModuleVariable) {
                const auto& variable = program.at(read.variable());
                callsItself = variable.initialization == VariableInitialization::Function && variable.function == id && variable.flags.contains(VariableFlags::IsImmutable);
            } else if (read.opcode() == Opcode::PropGet && function.kind == FunctionKind::Method && !read.isFreeName()) {
                const PropertyKey& key = program.at(read.key());
                callsItself = key.kind == PropertyKeyKind::Text && key.text == function.name && isEntryParameter(inFrontOfChecks(read.as<Opcode::PropGet>().objectOperand()), EntryParameter::This);
            }
        }
    });
    return callsItself;
}

static bool isRecursiveThroughVariable(const Walker& walker, const BodyIndex& index, FunctionID id)
{
    const LinkTables& linked = walker.facts().linked();
    if (!id || id.index() >= linked.functions.size() || !index.opcodes().contains(Opcode::Call))
        return false;
    bool someCallEntersIt = std::ranges::any_of(linked.callsOf(id), [&](const LinkedCall& call) {
        return call.callee == id;
    });
    if (!someCallEntersIt)
        return false;
    bool callsItself = false;
    index.body().forEachBlock([&](BodyBlock block) {
        for (BodyValue value : block.values())
            callsItself |= value.opcode() == Opcode::Call && linked.calleeAt(id, value.id()) == id;
    });
    return callsItself;
}

bool Walker::isFrameRecursive(const Frame& frame) const
{
    if (!frame.index)
        return false;
    if (!frame.isRecursive)
        frame.isRecursive = isDirectlyRecursive(program(), *frame.index, frame.function) || isRecursiveThroughPrototype(*this, *frame.index, frame.function) || isRecursiveThroughVariable(*this, *frame.index, frame.function);
    return *frame.isRecursive;
}

bool Walker::isBeingWalked(FunctionID function) const
{
    constexpr size_t mostFramesLookedThrough = 8;
    if (m_frames.size() <= mostFramesLookedThrough) {
        return m_frames.containsIf([&](const Frame& frame) {
            return frame.function == function;
        });
    }
    size_t numberOfFunctions = program().functions.size();
    if (m_numbersOfFramesOfFunctions.isEmpty())
        m_numbersOfFramesOfFunctions.fill(0, numberOfFunctions + 1);
    auto numberOfFramesOf = [&](FunctionID walked) -> uint32_t& {
        return m_numbersOfFramesOfFunctions[walked ? walked.index() : numberOfFunctions];
    };
    for (size_t index = m_frames.size(); index-- && !m_frames[index].countAmongFramesOfFunction;)
        m_frames[index].countAmongFramesOfFunction.count(numberOfFramesOf(m_frames[index].function));
    return numberOfFramesOf(function);
}

void Walker::Frame::numberReferencesOfNarrowerName(uint32_t place)
{
    auto numberOf = [&](Reference reference) {
        uint32_t number = numbersOfNarrowedReferences.add(reference.bits(), numbersOfNarrowedReferences.size()).iterator->value;
        if (number == firstNarrowerNamesInTables.size())
            firstNarrowerNamesInTables.append({ });
        return number;
    };
    ASSERT(narrowerNamesInTables.size() == place);
    uint32_t numberOfValue = numberOf(narrowerNames[place].value);
    narrowerNamesInTables.append({ numberOfValue, numberOf(narrowerNames[place].name), noNarrowerName, noNarrowerName });
}

void Walker::Frame::bringTablesOfNarrowerNamesTo(uint32_t first) const
{
    Vector<uint32_t, 32> namesToPutIn;
    uint32_t left = listInTables;
    uint32_t entered = first;
    while (left != entered) {
        uint32_t lengthLeft = lengthOfNarrowerNames(left);
        uint32_t lengthEntered = lengthOfNarrowerNames(entered);
        if (lengthLeft >= lengthEntered) {
            const NarrowerNameInTables& name = narrowerNamesInTables[left];
            firstNarrowerNamesInTables[name.numberOfValue].ofValue = name.nextOfValue;
            firstNarrowerNamesInTables[name.numberOfName].underName = name.nextUnderName;
            left = narrowerNames[left].next;
        }
        if (lengthEntered >= lengthLeft) {
            namesToPutIn.append(entered);
            entered = narrowerNames[entered].next;
        }
    }
    while (!namesToPutIn.isEmpty()) {
        uint32_t place = namesToPutIn.takeLast();
        NarrowerNameInTables& name = narrowerNamesInTables[place];
        name.nextOfValue = std::exchange(firstNarrowerNamesInTables[name.numberOfValue].ofValue, place);
        name.nextUnderName = std::exchange(firstNarrowerNamesInTables[name.numberOfName].underName, place);
    }
    listInTables = first;
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::Frame::addNarrowerName(uint32_t& first, Reference value, Reference name)
{
    ASSERT(value && name);
    PlaceOfNarrowerName place;
    place.length = lengthOfNarrowerNames(first) + 1;
    place.values = valuesOfNarrowerNames(first) | bitOfNarrowedValue(value);
    narrowerNames.append(NarrowerName { value, name, first });
    placesOfNarrowerNames.append(place);
    first = narrowerNames.size() - 1;

    if (findsNarrowerNamesByReference) {
        numberReferencesOfNarrowerName(first);
        return;
    }
    if (place.length < lengthOfLongListOfNarrowerNames)
        return;
    findsNarrowerNamesByReference = true;
    for (uint32_t earlier = 0; earlier < narrowerNames.size(); ++earlier)
        numberReferencesOfNarrowerName(earlier);
}

template<Walker::Frame::NarrowerNamesAsked asked, typename Functor>
void Walker::Frame::forEachNarrowerName(Reference reference, const Functor& functor) const
{
    constexpr bool ofValue = asked == NarrowerNamesAsked::OfValue;
    if (!reference)
        return;
    if (listOfNarrowerNamesIsLong) {
        auto found = numbersOfNarrowedReferences.find(reference.bits());
        if (found == numbersOfNarrowedReferences.end())
            return;
        if (listInTables != firstNarrowerName)
            bringTablesOfNarrowerNamesTo(firstNarrowerName);
        const FirstNarrowerNames& first = firstNarrowerNamesInTables[found->value];
        for (uint32_t place = ofValue ? first.ofValue : first.underName; place != noNarrowerName; place = ofValue ? narrowerNamesInTables[place].nextOfValue : narrowerNamesInTables[place].nextUnderName) {
            if (functor(narrowerNames[place]) == IterationStatus::Done)
                return;
        }
        return;
    }
    for (uint32_t place = firstNarrowerName; place != noNarrowerName; place = narrowerNames[place].next) {
        const NarrowerName& narrower = narrowerNames[place];
        if ((ofValue ? narrower.value : narrower.name) == reference && functor(narrower) == IterationStatus::Done)
            return;
    }
}

bool Walker::Frame::isNarrowerNameInLongList(Reference reference) const
{
    bool isName = false;
    forEachNarrowerName<NarrowerNamesAsked::UnderName>(reference, [&](const NarrowerName&) {
        isName = true;
        return IterationStatus::Done;
    });
    return isName;
}

Reference Walker::Frame::narrowestNameInLongList(Reference reference) const
{
    Reference named = reference;
    forEachNarrowerName<NarrowerNamesAsked::UnderName>(reference, [&](const NarrowerName& narrower) {
        if (isNarrowerNameInLongList(narrower.value))
            return IterationStatus::Continue;
        named = narrower.value;
        return IterationStatus::Done;
    });
    forEachNarrowerName<NarrowerNamesAsked::OfValue>(named, [&](const NarrowerName& narrower) {
        reference = narrower.name;
        return IterationStatus::Done;
    });
    return reference;
}

ALWAYS_INLINE_INTO_OTHER_FILES Reference Walker::narrowestNameOf(Reference reference) const
{
    const Frame& frame = m_frames.last();
    if (!(frame.narrowedValues & Frame::bitOfNarrowedValue(reference)))
        return reference;
    if (frame.listOfNarrowerNamesIsLong) [[unlikely]]
        return frame.narrowestNameInLongList(reference);
    for (uint32_t place = frame.firstNarrowerName; place != noNarrowerName; place = frame.narrowerNames[place].next) {
        if (frame.narrowerNames[place].value == reference)
            return frame.narrowerNames[place].name;
    }
    return reference;
}

Reference Walker::nameNarrowedBehindTests(Reference reference) const
{
    return narrowestNameOf(reference);
}

Reference Walker::narrowestReferenceOf(ValueID input) const
{
    const Frame& frame = m_frames.last();
    Reference reference = narrowestNameOf(referenceBoundIn(frame, input));
    while (m_numbersOfStates.newestCheckedNameOf(reference)) {
        Reference checked = m_state.checkedNameOf(reference);
        if (!checked)
            break;
        reference = narrowestNameOf(checked);
    }
    return reference;
}

TypeID Walker::typeOfTextOrRecord(Reference reference) const
{
    return typeOfRecordForLooks(*this, typeAsBuilt(reference));
}

Walker::ClosedArm::ClosedArm(Walker& walker, bool restartsIterationIfPossible)
    : m_walker(walker)
    , m_firstBinding(walker.m_bindings.size())
    , m_openedGuesses(std::exchange(walker.m_opensGuesses, false))
{
    if (restartsIterationIfPossible && walker.canRestartIteration())
        walker.restartIteration();
    ++walker.m_numberOfClosedArms;
}

Walker::ClosedArm::~ClosedArm()
{
    Vector<Binding>& bindings = m_walker.m_bindings;
    while (bindings.size() > m_firstBinding) {
        Binding binding = bindings.takeLast();
        if (binding.frame >= m_walker.m_frames.size())
            continue;
        Frame& frame = m_walker.m_frames[binding.frame];
        frame.references.placeOf<NamesOfValues::Writer::PutsBackWhatLogHolds>(binding.input.index()) = binding.previous;
        if (binding.previous && frame.duplicatedValues && frame.duplicatedValues->slotOf(binding.input) != Frame::DuplicatedValues::noSlot)
            frame.duplicatedValues->boundOnWay.append(binding.input);
    }
    --m_walker.m_numberOfClosedArms;
    m_walker.m_opensGuesses = m_openedGuesses;
}

TypeID Walker::joinTypes(TypeID first, TypeID second)
{
    return first == second ? first : joinOf(m_types, facts().families(), first, second);
}

TypeID Walker::typeOfResultAskedFor(const Draft& resultAt)
{
    Reference results = resultAt.resultsOperand();
    if (!results || !results.isRecord())
        return { };
    const Operation& producer = m_code.at(results.id());
    uint32_t index = resultAt.index();

    std::optional<uint32_t> numberOfResults = numberOfResultsOf(program(), producer);
    if (numberOfResults && index >= *numberOfResults)
        return { };

    Vector<TypeID, 8> typesOfProducerOperands;
    for (ValueID operand : producer.operands())
        typesOfProducerOperands.append(operand ? m_code.at(operand).type : TypeID());
    return Typed::resultTypeOf(typing(), producer, OperandTypes { typesOfProducerOperands.span() }, ObjectID(), index);
}

TypeID Walker::typeOfNewTargetBehind(Reference prototype) const
{
    if (!prototype.isRecord())
        return TypeTable::object;
    ValueID root = prototype.id();
    while (opcodesThatNameTheirOperand.contains(m_code.at(root).opcode))
        root = m_code.at(root).operand(0);
    const Operation& read = m_code.at(root);
    if (read.opcode == Opcode::GetClassPrototype)
        return m_code.at(read.classObjectOperand()).type;
    if (read.opcode == Opcode::GetConstructPrototype)
        return m_code.at(read.newTargetOperand()).type;
    return TypeTable::object;
}

TypeID Walker::resultTypeOf(const Draft& draft)
{
    bool listsOwnTypes;
    return resultTypeOf(draft, listsOwnTypes);
}

struct StepOfBuildInWalk {
    static constexpr uint32_t isTypeTest = 1u << 0;
    static constexpr uint32_t mayHaveNoEmitter = 1u << 1;
    static constexpr uint32_t looksAtKindsOfOperands = 1u << 2;
    static constexpr uint32_t takesTextsAsKeys = 1u << 3;
    static constexpr uint32_t storesIntoSlots = 1u << 4;
    static constexpr uint32_t givesNoResult = 1u << 5;
    static constexpr uint32_t isNarrowed = 1u << 6;
    static constexpr uint32_t mayHaveSeveralResults = 1u << 7;
    static constexpr uint32_t isResultAt = 1u << 8;
    static constexpr uint32_t isNewObject = 1u << 9;
    static constexpr uint32_t isConversionOfForm = 1u << 10;
    static constexpr uint32_t mayTestItsOwnGuess = 1u << 11;
    static constexpr uint32_t kindsAreSeenAtAGlance = 1u << 12;
    static constexpr uint32_t hasPureBound = 1u << 13;
    static constexpr uint32_t hasEffectsThatHangOnResult = 1u << 14;
    static constexpr uint32_t isCatch = 1u << 15;
    static constexpr uint32_t findsEqualOperation = 1u << 16;
    static constexpr uint32_t takesOperandsAsBuilt = 1u << 17;
    static constexpr uint32_t resultHasFormOfItsType = 1u << 18;
    static constexpr uint32_t notesOperationBuilt = 1u << 19;
    static constexpr uint32_t isTypedByWhatIsProven = 1u << 20;
    static constexpr uint32_t isCallForStackTests = 1u << 21;
    static constexpr uint32_t isToldToPolls = 1u << 22;
    static constexpr uint32_t isConversionOnStraightRoad = 1u << 23;
    static constexpr uint32_t takesOperandsByKind = 1u << 24;
    static constexpr uint32_t isLookedAtByCopy = 1u << 25;
    static constexpr uint32_t isConstantInFormOfUses = 1u << 26;
    static constexpr uint32_t takesCellsAsTheyAre = 1u << 27;
    static constexpr uint32_t shiftOfFormOfEveryResult = 28;
};
static constexpr uint32_t mostOperandsTakenByKindAtOnceInWalk = 8;
static_assert(numberOfEnumerators<Representation> <= 1u << (32 - StepOfBuildInWalk::shiftOfFormOfEveryResult));

TypeID Walker::typeOfSecondCounter(const Draft& draft)
{
    ValueID value = draft.valueOperand().id();
    ValueID test = draft.testOperand() ? draft.testOperand().id() : ValueID();
    std::optional<SecondCounter> second = test ? secondCounterNarrowed(m_code, draft, value, test) : std::nullopt;
    if (!second)
        return { };
    BlockID header = m_code.blockOf(second->stepped);
    for (const OpenLoop& loop : m_openLoops) {
        if (loop.head != header)
            continue;
        std::span<const ValueID> entering = loop.recordsEntering();
        auto rangeEntering = [&](ValueID parameter) {
            size_t place = m_code.at(header).placeOfParameter(parameter);
            TypeID type = place < entering.size() ? m_code.at(entering[place]).type : TypeID();
            return type && m_types.at(type).isInteger() ? m_types.at(type).integers : Range::empty();
        };
        return typeBesideCounter(m_types, m_code, draft, value, test, *second, EnteringCounters { rangeEntering(second->counter), rangeEntering(second->stepped) });
    }
    return { };
}

bool Walker::hasUnprovenSecondCounter()
{
    for (uint32_t index = 0; m_claimsSecondCounters && index < m_code.numberOfValues(); ++index) {
        const Operation& record = m_code.at(ValueID(index));
        if (record.opcode == Opcode::Narrowed && record.narrowsWithCounter() && typeOfNarrowed(typing(), m_code, ValueID(index)) != record.type)
            return true;
        if ((record.opcode != Opcode::GetTypedArrayElement && record.opcode != Opcode::PutTypedArrayElement) || !record.isInsideView())
            continue;
        const PassedTest* passed = m_code.testsPassedFor(ValueID(index));
        if (passed && passed->groundsOfPosition() && passed->groundsOfPosition()->counterTest && !secondCounterStaysWithin(m_code, m_types, *passed->groundsOfPosition()))
            return true;
    }
    return std::ranges::any_of(m_code.steppedWithCounters(), [&](const SteppedWithCounter& beside) { return !isSteppedWithCounter(m_code, m_types, beside); });
}

TypeID Walker::typeOfNarrowedDraft(const Draft& draft)
{
    Reference value = draft.valueOperand();
    Reference test = draft.testOperand();
    if (!value || !value.isRecord() || (test && !test.isRecord()))
        return { };
    if (draft.narrowsWithCounter()) [[unlikely]]
        return typeOfSecondCounter(draft);
    std::span<const uint32_t> payload = draft.payload();
    NarrowedTypedLast& last = m_narrowedTypedLast;
    bool isSameQuestion = last.type && last.value == value && last.test == test && last.block == m_block && last.flags == draft.flags && last.typeOfValue == m_code.at(value.id()).type
        && payload.size() == last.numberOfPayloadWords && std::equal(payload.begin(), payload.end(), last.payload.begin());
    if (isSameQuestion)
        return last.type;
    TypeID type = typeOfNarrowedRecordInWalk(typing(), m_code, draft, value.id(), test ? test.id() : ValueID(), m_block);
    last.type = { };
    if (payload.size() <= last.payload.size()) {
        last = { value, test, m_block, m_code.at(value.id()).type, type, draft.flags, static_cast<uint8_t>(payload.size()), { } };
        std::copy(payload.begin(), payload.end(), last.payload.begin());
    }
    return type;
}

ALWAYS_INLINE_INTO_OTHER_FILES TypeID Walker::resultTypeByStepsOf(const TypingContext& context, const Draft& draft, uint32_t steps, bool& listsOwnTypes)
{
    listsOwnTypes = false;
    if (steps & StepOfBuildInWalk::givesNoResult)
        return { };
    if (steps & StepOfBuildInWalk::isNarrowed)
        return typeOfNarrowedDraft(draft);
    if ((steps & StepOfBuildInWalk::mayHaveSeveralResults) && numberOfResultsOf(program(), draft))
        return TypeTable::undefined;

    typesOfOperands(draft);
    if (steps & StepOfBuildInWalk::isResultAt)
        m_operandTypes[OperandIndex::ResultAt::results] = typeOfResultAskedFor(draft);
    else if ((steps & StepOfBuildInWalk::isNewObject) && draft.prototypeOperand())
        m_operandTypes[OperandIndex::NewObject::prototype] = typeOfNewTargetBehind(draft.prototypeOperand());
    else
        listsOwnTypes = true;
    if ((steps & StepOfBuildInWalk::isConversionOfForm) && m_operandTypes.size() == 1 && m_operandTypes[0])
        return m_operandTypes[0];
    return Typed::resultTypeOf(context, draft, OperandTypes { m_operandTypes.span() });
}

TypeID Walker::resultTypeOf(const Draft& draft, bool& listsOwnTypes)
{
    return resultTypeByStepsOf(typing(), draft, stepsOfBuildOf(draft.opcode), listsOwnTypes);
}

Representation Walker::formOfParameter(TypeID type, std::span<const Representation> arguments)
{
    Representation form = arguments.empty() ? Typed::representationOf(TypeReader(m_types), type) : representationOfJoin(m_types.at(type), arguments);
    if (form == Representation::OptionalDouble && has(Mechanism::UnboxedNumbers) && Typed::representationOf(TypeReader(m_types), type) == form)
        return form;
    if (form == Representation::OptionalInt64 && has(Mechanism::UnboxedNumbers) && arguments.empty())
        return form;
    auto isIntegerForm = [](Representation argument) {
        return argument == Representation::Int32 || argument == Representation::Int64;
    };
    if (form == Representation::OptionalInt64 && has(Mechanism::UnboxedNumbers) && std::ranges::any_of(arguments, isIntegerForm) && (std::ranges::contains(arguments, Representation::OptionalInt64) || !m_types.at(type).integers.isSubsetOf(Range::int32())))
        return form;
    if (numberRepresentations.contains(form) || optionalRepresentations.contains(form))
    {
        Representation natural = representationOfNumber(*this, type);
        bool allComeAsDoubles = !arguments.empty() && std::ranges::all_of(arguments, [](Representation argument) {
            return argument == Representation::Double;
        });
        return natural == Representation::Int64 && allComeAsDoubles ? Representation::Double : natural;
    }
    return form;
}

Representation Walker::boxedFormOf(TypeID type)
{
    if (isLegalRepresentation(m_types.at(type), Representation::JSValue))
        return Representation::JSValue;
    return Typed::representationOf(TypeReader(m_types), type);
}

template<typename Record>
static ALWAYS_INLINE unsigned hashOfWordsOfConstantInWalk(const Record& record)
{
    unsigned hash = (static_cast<unsigned>(record.opcode) << 8 | static_cast<unsigned>(record.flags)) * 0x9E3779B1u;
    for (uint32_t word : record.payload())
        hash = (hash ^ word) * 0x85EBCA6Bu + (hash >> 15);
    return hash ^ hash >> 16;
}

template<typename Record>
static ALWAYS_INLINE bool haveSameWordsInWalk(const Operation& constant, const Record& record)
{
    if (constant.opcode != record.opcode || constant.flags != record.flags)
        return false;
    std::span<const uint32_t> first = constant.payload();
    std::span<const uint32_t> second = record.payload();
    if (first.size() != second.size())
        return false;
    for (size_t index = 0; index < first.size(); ++index) {
        if (first[index] != second[index])
            return false;
    }
    return true;
}

static constexpr uint32_t noSharedConstantInWalk = UINT32_MAX;
static constexpr uint32_t hasFormOfItsTypeInWalk = 1u << 31;
static_assert(Reference::indexMask < hasFormOfItsTypeInWalk);

void Walker::addSharedConstant(unsigned hashOfWords, uint32_t added)
{
    if ((m_numberOfSharedConstants + 1) * 2 > m_sharedConstants.size()) {
        Vector<uint32_t, 16> former = std::exchange(m_sharedConstants, { });
        m_sharedConstants.fill(noSharedConstantInWalk, std::max<size_t>(16, former.size() * 2));
        size_t mask = m_sharedConstants.size() - 1;
        for (uint32_t shared : former) {
            if (shared == noSharedConstantInWalk)
                continue;
            size_t place = hashOfWordsOfConstantInWalk(m_code.at(ValueID(shared & ~hasFormOfItsTypeInWalk))) & mask;
            while (m_sharedConstants[place] != noSharedConstantInWalk)
                place = (place + 1) & mask;
            m_sharedConstants[place] = shared;
        }
    }
    size_t mask = m_sharedConstants.size() - 1;
    size_t place = hashOfWords & mask;
    while (m_sharedConstants[place] != noSharedConstantInWalk)
        place = (place + 1) & mask;
    m_sharedConstants[place] = added;
    ++m_numberOfSharedConstants;
    if (m_trialVisitDepth) [[unlikely]]
        m_sharedConstantsAddedInTries.append(added & ~hasFormOfItsTypeInWalk);
}

void Walker::forgetSharedConstantsAddedBehind(size_t numberLogged)
{
    while (m_sharedConstantsAddedInTries.size() > numberLogged) {
        uint32_t added = m_sharedConstantsAddedInTries.takeLast();
        size_t mask = m_sharedConstants.size() - 1;
        size_t hole = hashOfWordsOfConstantInWalk(m_code.at(ValueID(added))) & mask;
        while (m_sharedConstants[hole] == noSharedConstantInWalk || (m_sharedConstants[hole] & ~hasFormOfItsTypeInWalk) != added) {
            RELEASE_ASSERT(m_sharedConstants[hole] != noSharedConstantInWalk);
            hole = (hole + 1) & mask;
        }
        for (size_t next = (hole + 1) & mask; m_sharedConstants[next] != noSharedConstantInWalk; next = (next + 1) & mask) {
            size_t home = hashOfWordsOfConstantInWalk(m_code.at(ValueID(m_sharedConstants[next] & ~hasFormOfItsTypeInWalk))) & mask;
            if (hole <= next ? (hole < home && home <= next) : (hole < home || home <= next))
                continue;
            m_sharedConstants[hole] = m_sharedConstants[next];
            hole = next;
        }
        m_sharedConstants[hole] = noSharedConstantInWalk;
        --m_numberOfSharedConstants;
    }
}

Reference Walker::convertWithoutCode(Reference reference, Representation wanted)
{
    if (!reference || !hasRegister(wanted) || representationOf(reference) == wanted)
        return reference;
    if (!reference.isConstant())
        return { };

    if (ValueID made = findCopyOfConstant(reference.id(), wanted))
        return Reference::constant(made);
    const Operation& record = m_code.at(reference.id());
    if (!constantsInFormOfUses.contains(record.opcode))
        return { };
    if (!isLegalRepresentation(m_types, record.type, wanted) || (wanted == Representation::Cell32 && m_types.at(record.type).hasCells()))
        return { };
    if (m_hasFailed)
        return { };

    unsigned hash = hashOfWordsOfConstantInWalk(record);
    ValueID made;
    if (!m_sharedConstants.isEmpty()) {
        size_t mask = m_sharedConstants.size() - 1;
        for (size_t place = hash & mask; m_sharedConstants[place] != noSharedConstantInWalk && !made; place = (place + 1) & mask) {
            ValueID shared(m_sharedConstants[place] & ~hasFormOfItsTypeInWalk);
            const Operation& other = m_code.at(shared);
            if (other.representation == wanted && haveSameWordsInWalk(other, record))
                made = shared;
        }
    }
    if (!made) {
        made = m_code.addCopyOfConstant(reference.id(), wanted);
        addSharedConstant(hash, made.index());
    }
    noteCopyOfConstant(reference.id(), wanted, made);
    return Reference::constant(made);
}

Reference Walker::convert(Reference reference, Representation wanted, Origin origin)
{
    if (!reference)
        return { };
    if (!hasRegister(wanted) || representationOf(reference) == wanted)
        return reference;
    if (reference.isConstant()) {
        if (Reference ready = convertWithoutCode(reference, wanted))
            return ready;
    }

    if (Reference made = findConversionMade(*this, reference, wanted); made && (m_numberOfLoopsThatMayBeLeft >= m_openLoops.size() || isInvariantInLoop(m_numberOfLoopsThatMayBeLeft, made)))
        return made;
    Reference asHandedIn = reference;
    Representation held = representationOf(reference);
    ConversionInSteps path;
    if (held != Representation::Unchosen)
        path = findConversionInSteps(m_types, typeOf(reference), held, wanted);
    if (!path.isFound) {
        fail("a value has no conversion into the form in which it is wanted"_s);
        return { };
    }
    origin.isTailCall = false;
    for (uint8_t index = 0; index < path.numberOfSteps; ++index) {
        ConversionStep step = path.steps[index];
        if (Reference built = tryBuildConversionOnStraightRoad(step.opcode, step.to, reference, origin)) {
            reference = built;
            continue;
        }
        Draft conversion(step.opcode, origin);
        conversion.setOperand(0, reference);
        conversion.representation = step.to;
        reference = build(WTF::move(conversion));
        if (!reference)
            return { };
    }
    noteConversionMade(*this, asHandedIn, wanted, reference);
    return reference;
}

static constexpr OpcodeSet conversionsOnStraightRoadInWalk { Opcode::Box, Opcode::Unbox, Opcode::Compress };
static_assert((conversionsOnStraightRoadInWalk | conversionsOfForm) == conversionsOfForm && (conversionsOnStraightRoadInWalk | cellPreservingOpcodes) == cellPreservingOpcodes);
static_assert(!conversionsOnStraightRoadInWalk.intersects(opcodesThatStateTypes));

ALWAYS_INLINE_INTO_OTHER_FILES Reference Walker::tryBuildConversionOnStraightRoad(Opcode opcode, Representation to, Reference operand, Origin origin)
{
    if (!(stepsOfBuildOf(opcode) & StepOfBuildInWalk::isConversionOnStraightRoad) || !operand.isRecord() || isUnreachable())
        return { };
    if (hasStringsInBuilders() || m_positionsWithoutProofs)
        return { };

    TypeID proven = typeAsBuilt(operand);
    if (!proven || isNeverType(proven))
        return { };
    if (holdsOnlyPrimitives(proven)) {
        if (primitivesInID(proven) & Primitive::Unspecified)
            return { };
    } else {
        const Type& type = m_types.recordOf(proven);
        if (type.mayBeUnspecified() || type.opaque)
            return { };
    }
    if (!hasRegister(to) || to == Representation::RawWord || !isLegalRepresentation(m_types, proven, to))
        return { };
    if (!m_openLoops.isEmpty())
        return appendConversionAmongOpenLoops(opcode, to, operand, proven, origin);
    return Reference::value(m_code.appendConversion(m_block, opcode, operand.id(), to, proven, origin));
}

template<typename DraftType>
void Walker::convertOperands(DraftType& draft)
{
    uint32_t numberOfOperands = draft.numberOfOperands();
    if constexpr (std::is_same_v<DraftType, Draft>) {
        constexpr uint32_t mostOperandsTakenByKindAtOnce = mostOperandsTakenByKindAtOnceInWalk;
        OperandsAreTaken way = howOperandsAreTakenBy(draft.opcode);
        if (way == OperandsAreTaken::AsBuilt)
            return;
        bool takesEachByKind = (stepsOfBuildOf(draft.opcode) & StepOfBuildInWalk::takesOperandsByKind) && numberOfOperands <= mostOperandsTakenByKindAtOnce && !hasStringsInBuilders() && !testsItsOwnGuess(draft) && !(draft.opcode == Opcode::PutContiguousIndex && draft.goesByKindOfStorage());
        if (!takesEachByKind) {
            bool hasOperand = false;
            for (Reference operand : draft.operands())
                hasOperand |= !!operand;
            if (!hasOperand)
                return;
        }
        if (draft.opcode == Opcode::CallDirect && !hasStringsInBuilders()) {
            static_assert(!numberOfNamedOperandsOf(Opcode::CallDirect) && !genericOperationOf(Opcode::CallDirect));
            RELEASE_ASSERT(draft.operandKind(0) == OperandKind::Declared);
            RepresentationContext context { program(), plan(), m_types, unit() };
            CodeUnit entered = plan().conventions().unitEnteredBy(draft);
            for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
                Reference operand = draft.operand(operandIndex);
                if (!operand || !typeAsBuilt(operand))
                    continue;
                Representation wanted = representationOfArgument(context, entered, operandIndex);
                if (!hasRegister(wanted) || wanted == m_code.at(operand.id()).representation)
                    continue;
                Reference converted = convert(operand, wanted, draft.origin());
                if (!converted)
                    return;
                draft.setOperand(operandIndex, converted);
            }
            return;
        }
        if (takesEachByKind) {
            std::array<Representation, mostOperandsTakenByKindAtOnce> formsToBring;
            bool everyOperandIsReady = true;
            const OperandInfosOfEntry& operandInfos = operandInfosByOpcode[static_cast<unsigned>(draft.opcode)];
            for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
                formsToBring[operandIndex] = Representation::Unchosen;
                Reference operand = draft.operand(operandIndex);
                TypeID type = operand ? typeAsBuilt(operand) : TypeID();
                if (!type)
                    continue;
                OperandKind kind = operandInfoIn(operandInfos, operandIndex).kind;
                Representation held = m_code.at(operand.id()).representation;
                if (way == OperandsAreTaken::ByKindOrAsCell && kind == OperandKind::Any && held == Representation::Cell)
                    continue;
                Representation wanted = representationWantedByKind(m_types, kind, type);
                if (!hasRegister(wanted) || wanted == held)
                    continue;
                formsToBring[operandIndex] = wanted;
                everyOperandIsReady = false;
            }
            if (everyOperandIsReady)
                return;
            for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
                if (formsToBring[operandIndex] == Representation::Unchosen)
                    continue;
                Reference converted = convert(draft.operand(operandIndex), formsToBring[operandIndex], draft.origin());
                if (!converted)
                    return;
                draft.setOperand(operandIndex, converted);
            }
            return;
        }
        if (way == OperandsAreTaken::ByFormOfOperator && !hasStringsInBuilders() && !testsItsOwnGuess(draft)) {
            OperatorForm form = draft.form();
            if (Representation wanted = formTakenByEveryOperandOf(form, allBitwiseOperators.contains(draft.opcode)); wanted != Representation::Unchosen) {
                for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
                    Reference operand = draft.operand(operandIndex);
                    if (!operand || !typeAsBuilt(operand))
                        continue;
                    RELEASE_ASSERT(isLegalOperatorForm(draft.opcode, form));
                    if (wanted == m_code.at(operand.id()).representation)
                        continue;
                    Reference converted = convert(operand, wanted, draft.origin());
                    if (!converted)
                        return;
                    draft.setOperand(operandIndex, converted);
                }
                return;
            }
        }
    }
    WantedRepresentations formsWanted = wantedRepresentationsOf(*this, draft);
    RELEASE_ASSERT(formsWanted.size() >= numberOfOperands);
    const Representation* forms = formsWanted.span().data();

    bool comparesRawDoubles = false;
    if constexpr (requires { draft.opcode; }) {
        if (!m_openLoops.isEmpty() && numberOfOperands == 2 && (allRelations.contains(draft.opcode) || draft.opcode == Opcode::StrictEq) && draft.form() == OperatorForm::Numbers) {
            bool bothAreNumbers = true;
            for (uint32_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
                Reference side = draft.operand(operandIndex);
                bothAreNumbers &= side && side.isRecord() && m_types.at(typeOf(side)).isNumber();
                comparesRawDoubles |= side && side.isValue() && representationOf(side) == Representation::JSValue && !m_types.at(typeOf(side)).isInteger() && isInvariantInLoop(m_openLoops.size() - 1, side);
            }
            comparesRawDoubles &= bothAreNumbers;
        }
    }
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        Representation wanted = comparesRawDoubles ? Representation::Double : forms[operandIndex];
        if (!hasRegister(wanted))
            continue;
        Reference operand = draft.operand(operandIndex);
        if (wanted == representationOf(operand))
            continue;
        Reference converted = convert(operand, wanted, draft.origin());
        if (!converted)
            return;
        draft.setOperand(operandIndex, converted);
    }
}

template<typename DraftType>
void Walker::askTruthOfOperandsNotProvenBoolean(DraftType& draft)
{
    Origin origin = draft.origin();
    origin.isTailCall = false;
    for (uint32_t operandIndex = 0; operandIndex < draft.numberOfOperands(); ++operandIndex) {
        Reference operand = draft.operand(operandIndex);
        if (!operand || draft.operandKind(operandIndex) != OperandKind::Boolean)
            continue;
        TypeID type = typeOf(operand);
        if (!type || isNeverType(type) || isBooleanType(type) || !m_types.at(type).isJavaScriptValue())
            continue;

        Draft test(Opcode::ToBoolean, origin);
        test.setValueOperand(operand);
        chooseConservativeFields(test);
        Reference answer = build(WTF::move(test));
        if (!answer)
            return;
        draft.setOperand(operandIndex, answer);
    }
}

COLD NEVER_INLINE static void logMisfitInWalk(const TypeArena& types, ASCIILiteral takenBy, const IllTypedOperand& misfit)
{
    dataLogLn("AOT: ", takenBy, ", operand ", misfit.operandIndex, ": ", nameOf(misfit.error), ": it is ", TypeInText { types, misfit.proven }, ", and ", TypeInText { types, misfit.required }, " is taken");
}

bool Walker::operandsAreAsTaken(const TerminatorDraft& draft) const
{
    if (!draft.numberOfOperands())
        return true;
    if (draft.kind != TerminatorKind::Branch || hasStringsInBuilders())
        return false;
    Reference condition = draft.conditionOperand();
    TypeID type = typeOf(condition);
    return type && isBooleanType(type) && representationOf(condition) == wantsOf(OperandKind::Boolean);
}

bool Walker::isDeclaredPure(TerminatorKind kind)
{
    static constexpr TerminatorKindSet kindsDeclaredPure = [] {
        TerminatorKindSet pure;
        for (unsigned number = 0; number < numberOfEnumerators<TerminatorKind>; ++number) {
            if (declaredBoundOf(static_cast<TerminatorKind>(number)).isPure())
                pure.add(static_cast<TerminatorKind>(number));
        }
        return pure;
    }();
    return kindsDeclaredPure.contains(kind);
}

bool Walker::checkOperandKinds(const TerminatorDraft& draft)
{
    for (Reference operand : draft.operands()) {
        if (hasStringsInBuilders() && isStringInBuilder(*this, operand)) {
            fail("a terminator takes a text in its builder"_s);
            return false;
        }
    }
    std::optional<IllTypedOperand> misfit = findIllTypedOperandOfTerminator(typing(), draft, typesOfOperands(draft), unit().function);
    if (!misfit)
        return true;
    logMisfitInWalk(m_types, nameOf(draft.kind), *misfit);
    fail("an operand is not of the kind that its terminator takes"_s);
    return false;
}

void Walker::letLeave(WalkState& state, Reference reference) const
{
    if (!reference.isValue())
        return;
    ValueID cell = underlyingCellOf(m_code, reference.id());
    state.markEscaped(referenceTo(cell));
    if (m_scopesThatClosuresHold.isEmpty() || m_code.at(cell).opcode != Opcode::NewClosure)
        return;
    for (ValueID capture : m_code.at(cell).capturesOperands()) {
        if (capture)
            state.markEscaped(referenceTo(underlyingCellOf(m_code, capture)));
    }
}

void Walker::noteOperationBuiltWhereCodeIsVerified(ValueID id, const Draft& draft)
{
    static constexpr OpcodeSet closuresAndClassesMade { Opcode::NewClosure, Opcode::NewClass, Opcode::NewGenerator, Opcode::NewGeneratorOfLibrary, Opcode::NewAsyncGenerator, Opcode::NewPromise, Opcode::NewRegExp };
    if (draft.opcode == Opcode::GetArrayElement && draft.readsHoleAsNaN())
        m_code.noteInputOfHoleReadAsNaN({ id, currentFunction(), draft.inputValueGiven });
    if (draft.opcode == Opcode::GetModuleVariable && !draft.inputValueGiven && !draft.returnsRawSlot())
        m_code.recordSyntheticVariableRead(id);
    if (draft.opcode == Opcode::GetEnvironmentField && !draft.inputValueGiven && draft.isKnownInitialized())
        m_code.recordSyntheticVariableRead(id);
    if (!isWrittenInTopFrame(draft) || opcodesThatMakeArguments.contains(draft.opcode) || (resultRuleInWalk(draft.opcode) != ResultRule::Allocation && !closuresAndClassesMade.contains(draft.opcode)))
        return;
    const Frame& frame = m_frames.last();
    Reference earlier = referenceBoundIn(frame, draft.inputValueGiven);
    if (!earlier || !earlier.isValue() || earlier.id() == id || m_code.at(earlier.id()).opcode != draft.opcode || frame.blocks.isEmpty())
        return;
    uint32_t endOfEarlierWalkOfBlock = frame.blocks[frame.block.index()].numberOfRecordsBehindEnd;
    if (endOfEarlierWalkOfBlock == UINT32_MAX || earlier.id().index() >= endOfEarlierWalkOfBlock)
        m_code.noteCellsMadeForOneOperation({ earlier.id(), id });
}

Draft Walker::draftOf(const BodyValue& value)
{
    const Frame& frame = m_frames.last();
    Origin origin = value.origin();
    origin.inlineFrame = frame.inlineFrame;

    Draft draft(value.opcode(), origin);
    draft.inputValueGiven = value.id();
    ASSERT(value.flags() <= UINT8_MAX);
    draft.flags = static_cast<uint8_t>(value.flags());
    bool lacksOperand = false;
    copyFields(value, draft, [&](ValueID operand) ALWAYS_INLINE_LAMBDA {
        Reference reference = referenceOf(operand);
        lacksOperand |= !reference;
        return reference;
    });

    if (value.id().index() + 1 == frame.endOfValues) {
        BodyTerminator end = frame.index->terminator(frame.block);
        if (end.kind() == TerminatorKind::ReturnCall && end.operand(0) == value.id()) {
            draft.isTailCall = !frame.inlineFrame || !m_code.inlineFrames()[frame.inlineFrame].isConstruction;
            draft.resultIsReturnedAtOnce = true;
        }
    }

    if (lacksOperand && !isUnreachable())
        fail("an operation takes a value of the body that nothing was built for"_s);
    return draft;
}

TypeID Walker::withoutFunctionsThatMayNotBeMentioned(TypeID type)
{
    if (!type || holdsOnlyPrimitives(type))
        return type;
    Vector<FunctionID, 4> refused;
    forEachFunctionOf(m_types, m_types.at(type), scopedLambda<void(FunctionID)>([&](FunctionID named) {
        if (!facts().linked().mayBeMentionedBy(unit(), named))
            refused.append(named);
    }));
    for (FunctionID function : refused)
        type = m_types.differenceOf(type, m_types.forFunction(function));
    return type;
}

namespace {

enum class RecordSeenAtAGlanceInWalk : uint8_t { Never, AnyValue, ObjectsAlone, NothingOpaque, PrimitivesAlone, Whatever };

struct GlanceAtKindInWalk {
    RecordSeenAtAGlanceInWalk record { RecordSeenAtAGlanceInWalk::Never };
    uint16_t primitives { 0 };
    bool formFollowsFromKind { false };
    Representation formTaken { Representation::Unchosen };
};

} // anonymous namespace

static constexpr GlanceAtKindInWalk glanceAtKindInWalk(OperandKind kind)
{
    switch (kind) {
    case OperandKind::Any:
        return { RecordSeenAtAGlanceInWalk::AnyValue, Primitive::Defined & ~Primitive::Unspecified, true, wantsOf(kind) };
    case OperandKind::Convertible:
        return { RecordSeenAtAGlanceInWalk::NothingOpaque, Primitive::Defined & ~Primitive::Unspecified, true, Representation::Unchosen };
    case OperandKind::Results:
        return { RecordSeenAtAGlanceInWalk::Whatever, Primitive::Defined & ~Primitive::Unspecified, true, Representation::Void };
    case OperandKind::Object:
        return { RecordSeenAtAGlanceInWalk::ObjectsAlone, 0, true, wantsOf(kind) };
    case OperandKind::Boolean:
        return { RecordSeenAtAGlanceInWalk::PrimitivesAlone, Primitive::Boolean, true, wantsOf(kind) };
    case OperandKind::Integer:
        return { RecordSeenAtAGlanceInWalk::PrimitivesAlone, Primitive::Integer, false, Representation::Unchosen };
    case OperandKind::Number:
        return { RecordSeenAtAGlanceInWalk::PrimitivesAlone, Primitive::Number, true, wantsOf(kind) };
    case OperandKind::String:
        return { RecordSeenAtAGlanceInWalk::PrimitivesAlone, Primitive::AnyString, true, wantsOf(kind) };
    case OperandKind::PropertyKey:
        return { RecordSeenAtAGlanceInWalk::PrimitivesAlone, Primitive::AnyString | Primitive::AnySymbol, true, wantsOf(kind) };
    default:
        return { };
    }
}

static constexpr std::array<GlanceAtKindInWalk, numberOfEnumerators<OperandKind>> glancesAtKindsInWalk = []() consteval {
    std::array<GlanceAtKindInWalk, numberOfEnumerators<OperandKind>> glances { };
    for (OperandKind kind : allMembersOf<OperandKind>)
        glances[static_cast<unsigned>(kind)] = glanceAtKindInWalk(kind);
    return glances;
}();
static_assert([]() consteval {
    for (OperandKind kind : { OperandKind::Any, OperandKind::Object, OperandKind::Boolean, OperandKind::Number, OperandKind::String, OperandKind::PropertyKey }) {
        if (!(kindsTakenAsSchemaWants >> static_cast<unsigned>(kind) & 1))
            return false;
    }
    return true;
}());

static ALWAYS_INLINE bool everyOperandFitsItsKindAtAGlanceInWalk(const TypeArena& types, const Code& code, const Draft& draft, std::span<const TypeID> proven, const OperandInfosOfEntry& operandInfos, bool takesEveryOperandAsAny, bool takesCellsAsTheyAre, bool& everyOperandHasFormOfItsKind)
{
    std::span<const Reference> operands = draft.operands();
    if (proven.size() != operands.size())
        return false;
    for (size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex) {
        Reference operand = operands[operandIndex];
        if (!operand)
            continue;
        TypeID id = proven[operandIndex];
        if (!id)
            return false;
        OperandKind kind = takesEveryOperandAsAny ? OperandKind::Any : operandInfoIn(operandInfos, static_cast<uint32_t>(operandIndex)).kind;
        const GlanceAtKindInWalk& glance = glancesAtKindsInWalk[static_cast<unsigned>(kind)];
        if (holdsOnlyPrimitives(id)) {
            if (primitivesInID(id) & ~glance.primitives)
                return false;
        } else {
            const Type& type = types.recordOf(id);
            if (type.mayBeUnspecified())
                return false;
            switch (glance.record) {
            case RecordSeenAtAGlanceInWalk::Never:
                return false;
            case RecordSeenAtAGlanceInWalk::AnyValue:
                if (!type.isJavaScriptValue())
                    return false;
                break;
            case RecordSeenAtAGlanceInWalk::ObjectsAlone:
                if (!type.isJavaScriptValue() || type.primitives)
                    return false;
                break;
            case RecordSeenAtAGlanceInWalk::NothingOpaque:
                if (type.opaque)
                    return false;
                break;
            case RecordSeenAtAGlanceInWalk::PrimitivesAlone:
                if (!type.hasOnlyPrimitives(glance.primitives))
                    return false;
                break;
            case RecordSeenAtAGlanceInWalk::Whatever:
                break;
            }
        }
        if (!everyOperandHasFormOfItsKind)
            continue;
        const Operation& record = code.at(operand.id());
        bool staysAsCell = takesCellsAsTheyAre && kind == OperandKind::Any && record.representation == Representation::Cell;
        everyOperandHasFormOfItsKind &= glance.formFollowsFromKind && record.type == id && (!hasRegister(glance.formTaken) || glance.formTaken == record.representation || staysAsCell);
    }
    return true;
}

std::array<uint32_t, numberOfOpcodes> Walker::findStepsOfBuildByOpcode()
{
    using Step = StepOfBuildInWalk;
    std::array<uint32_t, numberOfOpcodes> stepsByOpcode { };
    for (Opcode opcode : allMembersOf<Opcode>) {
        Representation formOfEveryResult = formOfResultOfEvery(opcode);
        if (formOfEveryResult == Representation::Unchosen)
            formOfEveryResult = formsOfResultsOfOperatorsByOpcode[static_cast<unsigned>(opcode)];
        uint32_t steps = static_cast<uint32_t>(formOfEveryResult) << Step::shiftOfFormOfEveryResult;
        auto add = [&](uint32_t step, bool applies) {
            if (applies)
                steps |= step;
        };
        bool hasPureBound = opcodesWithPureDeclaredBound().contains(opcode);
        RELEASE_ASSERT(!hasPureBound || (!declaredBoundInTable(opcode).mayRunOtherCode() && !operandsMayRunCode(opcode)));
        bool hasNoEmitter = opcodesWithoutEmitter().contains(opcode);
        bool takesTextsAsKeys = opcodesThatTakeTextsAsKeys.contains(opcode);
        OperandsAreTaken way = howOperandsAreTakenBy(opcode);
        bool takesOperandsAsBuilt = way == OperandsAreTaken::AsBuilt;
        add(Step::isTypeTest, opcode == Opcode::TypeTest);
        add(Step::mayHaveNoEmitter, hasNoEmitter || Operation::opcodesWithAdmitsInt32Storage.contains(opcode));
        add(Step::takesTextsAsKeys, takesTextsAsKeys);
        add(Step::storesIntoSlots, storesIntoSlotsInWalk.contains(opcode) || storesIntoPlacesInWalk.contains(opcode));
        add(Step::givesNoResult, resultRuleInWalk(opcode) == ResultRule::None);
        add(Step::isNarrowed, opcode == Opcode::Narrowed);
        add(Step::mayHaveSeveralResults, opcodesThatMayHaveSeveralResults.contains(opcode));
        add(Step::isResultAt, opcode == Opcode::ResultAt);
        add(Step::isNewObject, opcode == Opcode::NewObject);
        add(Step::isConversionOfForm, conversionsOfForm.contains(opcode));
        add(Step::mayTestItsOwnGuess, genericOperationOf(opcode) && opcode != Opcode::GetEnvironmentField && opcode != Opcode::PutEnvironmentField);
        add(Step::hasPureBound, hasPureBound);
        add(Step::hasEffectsThatHangOnResult, opcodesWithEffectsOfOperatorsInWalk.contains(opcode) || opcode == Opcode::GetArrayElement);
        add(Step::isCatch, catchOpcodes.contains(opcode));
        add(Step::findsEqualOperation, opcodesThatFindEqualOperationBuiltTakes.contains(opcode));
        add(Step::takesOperandsAsBuilt, takesOperandsAsBuilt);
        add(Step::takesCellsAsTheyAre, way == OperandsAreTaken::ByKindOrAsCell);
        add(Step::resultHasFormOfItsType, opcodesWhoseResultsHaveFormsOfTheirTypes.contains(opcode));
        add(Step::notesOperationBuilt, opcodesThatNoteOperationBuiltTakes.contains(opcode));
        add(Step::isTypedByWhatIsProven, readsTypedByWhatIsProvenOfInput.contains(opcode) || recordsTypedByWhatIsProvenOfContents.contains(opcode) || callsByValueInWalk.contains(opcode));
        add(Step::isCallForStackTests, opcode == Opcode::CallDirect || opcode == Opcode::AwaitCallBySavingFrame || opcode == Opcode::Construct);
        add(Step::isToldToPolls, opcodesThatPollsAreToldOfInWalk.contains(opcode));
        add(Step::isLookedAtByCopy, opcode == Opcode::Call || opcode == Opcode::CallList || opcode == Opcode::ConstructList || opcode == Opcode::SuperConstructList || opcode == Opcode::StrictEq || opcode == Opcode::ToBoolean || opcode == Opcode::Builtin);
        add(Step::isConstantInFormOfUses, constantsInFormOfUses.contains(opcode));
        add(Step::isConversionOnStraightRoad, conversionsOnStraightRoadInWalk.contains(opcode) && hasPureBound && !hasNoEmitter && !takesTextsAsKeys && takesOperandsAsBuilt && operandKindOf(opcode, 0) == OperandKind::Convertible);

        bool looksAtKindsOfOperands = false;
        bool kindsAreSeenAtAGlance = true;
        bool hasDeclaredOperand = false;
        const OperandInfosOfEntry& operandInfos = operandInfosByOpcode[static_cast<unsigned>(opcode)];
        for (uint32_t index = 0; index < operandInfos.size; ++index) {
            OperandKind kind = operandInfos.first[index].kind;
            looksAtKindsOfOperands |= kind == OperandKind::Boolean;
            kindsAreSeenAtAGlance &= glanceAtKindInWalk(kind).record != RecordSeenAtAGlanceInWalk::Never;
            hasDeclaredOperand |= kind == OperandKind::Declared;
        }
        add(Step::looksAtKindsOfOperands, looksAtKindsOfOperands);
        add(Step::kindsAreSeenAtAGlance, kindsAreSeenAtAGlance);
        add(Step::takesOperandsByKind, way == OperandsAreTaken::ByKind || way == OperandsAreTaken::ByKindOrAsCell || (way == OperandsAreTaken::ByKindWhereNotDeclared && !hasDeclaredOperand));
        stepsByOpcode[static_cast<unsigned>(opcode)] = steps;
    }
    return stepsByOpcode;
}

static void letObjectsLeaveAtSuspension(Walker& walker)
{
    if (walker.unit().role != CodeRole::CoroutineBody)
        return;
    for (Reference object : walker.state().listFreshObjects()) {
        const Operation& made = walker.code().at(object.id());
        bool isPartOfScope = made.opcode == Opcode::NewClosure || (made.opcode == Opcode::NewObject && walker.program().at(made.layout()).kind == LayoutKind::Environment);
        if (!isPartOfScope)
            walker.state().markEscaped(object);
    }
}

uint16_t Walker::initialPrimitiveOf(const Program& program, VariableID variable)
{
    VariableInitialization born = program.at(variable).initialization;
    return born == VariableInitialization::Uninitialized ? Primitive::Absent : born == VariableInitialization::Undefined ? Primitive::Undefined : 0;
}

bool Walker::excludesInitialPrimitive(const Draft& draft, TypeID proven) const
{
    if (draft.opcode != Opcode::GetModuleVariable || plan().variable(draft.variable()).function)
        return false;
    uint16_t word = initialPrimitiveOf(program(), draft.variable());
    return word && !(m_types.at(proven).primitives & word);
}

bool Walker::isArrayOrInitialPrimitive(const Draft& draft, TypeID proven) const
{
    if (draft.opcode != Opcode::GetModuleVariable || draft.returnsRawSlot() || plan().variable(draft.variable()).function)
        return false;
    uint16_t word = initialPrimitiveOf(program(), draft.variable());
    if (!word || !(m_types.at(proven).primitives & word))
        return false;
    TypeID stored = m_types.withoutPrimitives(proven, word);
    return !isNeverType(stored) && hasKind(m_types, stored, OperandKind::Array);
}

Reference Walker::tryHoistCapturedVariableRead(const Draft& draft, const Effects& effects, TypeID proven)
{
    constexpr IterationPurity purity = IterationPurity::KnownEffectsOnlyWithInlinees;
    std::optional<uint32_t> loop = specializedOpenLoop(purity);
    if (!loop)
        return { };
    Draft read(draft);
    read.setIsKnownInitialized(false);
    std::optional<uint32_t> outermostThatMayBeLeft = findLoopToBuildInFrontOf(read, effects, proven);
    if (!outermostThatMayBeLeft || *outermostThatMayBeLeft > *loop)
        return { };
    Reference found;
    Reference isEmpty;
    {
        SpecializedPreheaderScope preheaderScope(*this, true, purity);
        found = build(WTF::move(read));
        if (!found || !found.isValue() || isUnreachable())
            return { };
        if (!(m_types.at(typeOf(found)).primitives & Primitive::Absent)) {
            count(StatisticID::CapturedVariableReadsHoisted);
            return found;
        }
        if (isNeverType(m_types.withoutPrimitives(typeOf(found), Primitive::Absent)))
            return { };
        Draft test(Opcode::TypeTest, draft.origin());
        test.setValueOperand(found);
        test.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
        isEmpty = build(WTF::move(test));
    }
    if (!isEmpty || !isEmpty.isValue() || !tryBranchInPreheader(isEmpty, false, purity, GuardedIterations::All))
        return { };
    recordSpeculation();
    count(StatisticID::CapturedVariableReadsHoisted);
    SpecializedPreheaderScope preheaderScope(*this, true, purity);
    Draft narrowed(Opcode::Narrowed, draft.origin());
    narrowed.setValueOperand(found);
    narrowed.setTestOperand(isEmpty);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(false);
    return build(WTF::move(narrowed));
}

Reference Walker::tryHoistModuleVariableRead(const Draft& draft, uint32_t outermostLoopThatMayBeLeft)
{
    constexpr IterationPurity purity = IterationPurity::KnownEffectsOnly;
    std::optional<uint32_t> loop = specializedOpenLoop(purity);
    if (!loop || *loop < outermostLoopThatMayBeLeft)
        return { };
    Reference found;
    Reference isAsBorn;
    {
        SpecializedPreheaderScope preheaderScope(*this, true, purity);
        Draft read(draft);
        read.setReturnsRawSlot(true);
        found = build(WTF::move(read));
        if (!found || !found.isValue() || isUnreachable())
            return { };
        TypeID stored = m_types.withoutPrimitives(typeOf(found), initialPrimitiveOf(program(), draft.variable()));
        if (isNeverType(stored) || !m_types.at(stored).isJavaScriptValue())
            return { };
        Draft test(Opcode::TypeTest, draft.origin());
        test.setValueOperand(found);
        test.setTestedType(TypeArena::fromPrimitives(initialPrimitiveOf(program(), draft.variable())));
        isAsBorn = build(WTF::move(test));
    }
    if (!isAsBorn || !isAsBorn.isValue() || !tryBranchInPreheader(isAsBorn, false, purity, GuardedIterations::Some))
        return { };
    recordSpeculation();
    SpecializedPreheaderScope preheaderScope(*this, true, purity);
    Draft narrowed(Opcode::Narrowed, draft.origin());
    narrowed.setValueOperand(found);
    narrowed.setTestOperand(isAsBorn);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(false);
    return build(WTF::move(narrowed));
}

Reference Walker::build(Draft&& draft, const Effects* foundByCaller)
{
    using Step = StepOfBuildInWalk;
    if (isUnreachable())
        return { };
    RELEASE_ASSERT(!m_hasFailed);
    const uint32_t steps = stepsOfBuildOf(draft.opcode);
    if (steps & (Step::isTypeTest | Step::mayHaveNoEmitter | Step::storesIntoSlots)) [[unlikely]] {
        if (steps & Step::storesIntoSlots) {
            m_valueBeforePendingStore = { };
            if (draft.opcode != Opcode::PutEnvironmentField && draft.opcode != Opcode::PutModuleVariable)
                m_pendingUndoableStore = 0;
            if (draft.opcode == Opcode::PutSlot && !m_isUndoingStores && draft.level() == KnowledgeLevel::Proven && !draft.isDefinition() && canRestartIteration() && m_code.at(m_block).frequency == BlockFrequency::Normal) {
                m_valueBeforePendingStore = readFilledFieldOfProvenObject(*this, draft.objectOperand(), draft.key(), draft.origin());
                if (m_hasFailed || isUnreachable())
                    return { };
                if (m_valueBeforePendingStore) {
                    Draft undoing(Opcode::PutSlot, draft.origin());
                    chooseConservativeFields(undoing);
                    undoing.setObjectOperand(draft.objectOperand());
                    undoing.setValueOperand(m_valueBeforePendingStore);
                    undoing.setKey(draft.key());
                    undoing.setReceivers(draft.receivers());
                    undoing.setLevel(KnowledgeLevel::Proven);
                    undoing.setMode(draft.mode());
                    undoing.setIsDefinition(false);
                    if (findIllTypedOperand(typing(), undoing, typesOfOperands(undoing)))
                        m_valueBeforePendingStore = { };
                }
            }
        }
        if (steps & Step::isTypeTest)
            draft.setTestedType(withoutFunctionsThatMayNotBeMentioned(draft.testedType()));
        if ((steps & Step::mayHaveNoEmitter) && Operation::opcodesWithAdmitsInt32Storage.contains(draft.opcode)) {
            if (!accessToElementsHasCode(draft.opcode, draft.arrayStorage(), draft.admitsInt32Storage(), draft.opcode == Opcode::HasArrayStorage && draft.isWritable())) {
                if (m_inputs.options.verifiesCode) {
                    fail("an access to elements is written in a form that has no code"_s, InternalErrorKind::OpcodeWithoutCode);
                    return { };
                }
                count(StatisticID::UnsupportedElementAccessesDemoted);
                if (draft.opcode == Opcode::HasArrayStorage) {
                    Draft no(Opcode::Boolean, draft.origin());
                    no.setBoolean(false);
                    return constant(WTF::move(no));
                }
                draft.setArrayStorage(ArrayStorage::Other);
                draft.setAdmitsInt32Storage(false);
            }
        } else if (steps & Step::mayHaveNoEmitter) {
            fail("an operation that no emitter stands behind is built"_s);
            return { };
        }
    }
    bool takesBoolean = false;
    const OperandInfosOfEntry& operandInfos = operandInfosByOpcode[static_cast<unsigned>(draft.opcode)];
    if (!(steps & Step::looksAtKindsOfOperands) && !m_holdsReadsOfVariablesAsConstants && !hasStringsInBuilders()) [[likely]] {
        for (Reference operand : draft.operands()) {
            if (operand && !operand.isRecord()) {
                fail("an operand names no record of Code"_s);
                return { };
            }
        }
    } else {
        for (uint32_t operandIndex = 0; operandIndex < draft.numberOfOperands(); ++operandIndex) {
            Reference operand = draft.operand(operandIndex);
            if (!operand)
                continue;
            if (!operand.isRecord()) {
                fail("an operand names no record of Code"_s);
                return { };
            }
            if (m_holdsReadsOfVariablesAsConstants && operand.isConstant()) [[unlikely]] {
                operand = readVariableWhereItIsTaken(operand, draft.origin());
                if (!operand || isUnreachable())
                    return { };
                draft.setOperand(operandIndex, operand);
            }
            OperandKind kind = operandInfoIn(operandInfos, operandIndex).kind;
            if (kind != OperandKind::StringBuilder && hasStringsInBuilders() && isStringInBuilder(*this, operand)) {
                fail("an operation that is none of a builder's takes a text in its builder"_s);
                return { };
            }
            takesBoolean |= kind == OperandKind::Boolean;
        }
    }
    if (steps & Step::takesTextsAsKeys)
        takeTextsUsedAsKeysAsAtomsOfItsOpcodes(*this, draft);
    if (takesBoolean && !testsItsOwnGuess(draft)) {
        askTruthOfOperandsNotProvenBoolean(draft);
        if (isUnreachable())
            return { };
    }

    if ((steps & Step::isCallForStackTests) && draft.opcode != Opcode::Construct) {
        holdOperandsToWhatCalleeBelieves(draft);
        if (isUnreachable())
            return { };
    }
    if (steps & Step::storesIntoSlots) {
        if (storesIntoSlotsInWalk.contains(draft.opcode))
            holdStoredValuesToSlots(draft);
        if (draft.opcode == Opcode::PutModuleVariable && !isUnreachable())
            proveThatFormOfVariableTakesStoredValue(draft);
        bool fillsScope = draft.opcode == Opcode::PutEnvironmentField || (draft.opcode == Opcode::NewObject && program().at(draft.layout()).kind == LayoutKind::Environment);
        if (fillsScope && !isUnreachable())
            proveThatSlotsOfScopeTakeStoredValues(draft);
        if (storesIntoPlacesInWalk.contains(draft.opcode) && !isUnreachable())
            holdStoredValuesToPlaces(draft);
        if (isUnreachable())
            return { };
    }
    if (m_positionsWithoutProofs) {
        checkProofsOfWhatIsStoredBy(draft);
        if (isUnreachable())
            return { };
    }
    if (draft.opcode == Opcode::NewObject && !draft.restIsDefinedBehind && !allocationHoldsWhatIsHeldFilled(draft)) [[unlikely]] {
        stopAtMaker(draft.origin());
        return { };
    }

    TypingContext context = typing();
    bool listsOwnTypes;
    TypeID proven = resultTypeByStepsOf(context, draft, steps, listsOwnTypes);
    if (!proven && !(steps & (Step::givesNoResult | Step::mayHaveSeveralResults))) {
        fail("the result rule of an operation gives no type"_s);
        return { };
    }

    if (!listsOwnTypes)
        typesOfOperands(draft);
    if (steps & Step::isResultAt)
        m_operandTypes[OperandIndex::ResultAt::results] = proven;
    bool takesEveryOperandAsAny = (steps & Step::mayTestItsOwnGuess) && testsItsOwnGuess(draft);
    bool everyOperandHasFormOfItsKind = (steps & Step::takesOperandsByKind) && !(steps & Step::takesOperandsAsBuilt) && !takesEveryOperandAsAny;
    bool everyOperandFitsAtAGlance = (takesEveryOperandAsAny || (steps & Step::kindsAreSeenAtAGlance)) && everyOperandFitsItsKindAtAGlanceInWalk(m_types, m_code, draft, m_operandTypes.span(), operandInfos, takesEveryOperandAsAny, steps & Step::takesCellsAsTheyAre, everyOperandHasFormOfItsKind);
    bool operandsAreInFormsTaken = (steps & Step::takesOperandsAsBuilt)
        || (everyOperandFitsAtAGlance && everyOperandHasFormOfItsKind && (steps & Step::takesOperandsByKind) && !takesEveryOperandAsAny && !hasStringsInBuilders() && draft.numberOfOperands() <= mostOperandsTakenByKindAtOnceInWalk);
    if (!everyOperandFitsAtAGlance) {
        if (std::optional<IllTypedOperand> misfit = findIllTypedOperand(context, draft, OperandTypes { m_operandTypes.span() })) {
            logMisfitInWalk(m_types, nameOf(draft.opcode), *misfit);
            fail("an operand is not of the kind that its operation takes"_s);
            return { };
        }
    }
    if (m_inputs.options.verifiesCode && isWrittenInTopFrame(draft) && takesWhatRowOfInputOperandCannotGive(draft)) [[unlikely]] {
        m_whatJudgeOfOperandsRefused = "an operation that stands for one of the input takes what the row of the input's operand cannot give"_s;
        fail(m_whatJudgeOfOperandsRefused, InternalErrorKind::TypeOutOfDate);
        return { };
    }

    bool hasNoEffects = steps & Step::hasPureBound;
    Effects effects = hasNoEffects ? Effects::none()
        : Typed::effectsOf(context, draft, (steps & Step::isResultAt) ? typesOfOperands(draft) : OperandTypes { m_operandTypes.span() }, (steps & Step::hasEffectsThatHangOnResult) ? proven : TypeID(), foundByCaller);
    if ((draft.opcode == Opcode::PutSlot || draft.opcode == Opcode::PutSlotOrAdd) && !testsItsOwnGuess(draft)
        && storeMaySetMarkOfCell(*this, membersOf(m_types, draft.receivers()), draft.key(), typeOf(draft.valueOperand())))
        effects.writes.add(AbstractHeap::NamedProperties);
    if (draft.opcode == Opcode::GetPropertyOfHeir || draft.opcode == Opcode::PutPropertyOfHeir) [[unlikely]]
        nameFieldInEffectsOfAccessToHeir(program(), draft.opcode, fieldAt(program(), FieldReference { draft.layout(), draft.field() }).key, effects);
    if (draft.opcode == Opcode::GetGlobalProperty || draft.opcode == Opcode::PutGlobalBinding) [[unlikely]]
        narrowGlobalAccessEffects(program(), draft.opcode, draft.key(), effects);
    if (draft.opcode == Opcode::GetKeyedProperty || draft.opcode == Opcode::PutKeyedProperty || draft.opcode == Opcode::GetKeyedValueOrAbsent) [[unlikely]]
        narrowKeyedAccessEffects(*this, draft, effects);
    if (draft.opcode == Opcode::GetOwnValueUnderKeyOrAbsent) [[unlikely]]
        effects.reads.merge(readsOfOwnKeyLookup(program()));
    const bool isStableRead = draft.opcode == Opcode::GetSlot && readsStableField(draft);
    if (isStableRead) {
        effects.reads.remove(AbstractHeapSet { AbstractHeap::ObjectFields });
        if (Reference kept = valueOfStableField(draft, proven))
            return kept;
    }
    applyUnderstatedEffects(draft, effects);
    if ((steps & Step::storesIntoSlots) && draft.opcode == Opcode::PutSlot) [[unlikely]] {
        static constexpr AbstractHeapSet irreversibleHeaps { AbstractHeap::NamedProperties, AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::PrototypeLinks };
        if (Reference held = std::exchange(m_valueBeforePendingStore, Reference()); held && !effects.mayRunOtherCode() && !effects.writes.containsAny(irreversibleHeaps)) {
            UndoableStore store { 0, draft.objectOperand(), held, draft.key(), draft.receivers(), draft.mode() };
            store.isIntoIterationObject = isAllocatedInRestartableIteration(draft.objectOperand());
            recordPendingUndoableStore(store);
        }
    }
    if (effects.mayCollect && !m_isLeadingHandlerEdge && !(steps & Step::isCatch) && isUnderHandler())
        effects.mayThrow |= ThrowEffect::onlyForLackOfMemory();

    static const OpcodeSet opcodesThatMayEnterByValue = opcodesWhere([](Opcode opcode) {
        return (declaredBoundInTable(opcode).mayRunOtherCode() || operandsMayRunCode(opcode)) && opcode != Opcode::CallDirect && opcode != Opcode::AwaitCallBySavingFrame && opcode != Opcode::GetConstructPrototype;
    });
    if (!hasNoEffects && opcodesThatMayEnterByValue.contains(draft.opcode))
        ++m_numberOfOperationsBuiltThatMayEnterByValue;
    if (m_frameOfFusedChain && !hasNoEffects && mayLeaveFusedChainHere()) {
        if (draft.opcode == Opcode::RequireObjectCoercible)
            return requireCoercibleInFusedChain(draft);
        if (leaveFusedChain(draft, effects))
            return { };
    }
    if (!hasNoEffects && !effects.mayBeRunAgainUnseen()) {
        ++m_numberOfOperationsBuiltThatAreSeenIfRunAgain;
        if (tryLeaveInlinedHead())
            return { };
    }
    const bool isIgnoredArrayStore = m_inputs.options.shouldIgnoreLoopArrayStores && draft.opcode == Opcode::PutArrayElement && !m_openLoops.isEmpty();
    if (!m_openLoops.isEmpty() && !isIgnoredArrayStore) {
        if (!m_isWalkingAccess && !m_openLoops.isEmpty() && effects.mayRunOtherCode() && (m_code.at(m_block).frequency == BlockFrequency::Rare ? canRestartIteration() : isUncaughtIterationEffectFree()) && !looksAtLoopsAroundHaveCounted(draft, effects)) {
            bool isOnUsualWay = m_code.at(m_block).frequency == BlockFrequency::Normal && !effects.maySuspend;
            m_turnOfTypedBuildLeavesWithoutTest |= isOnUsualWay;
            m_innerLoopExitsOnLikelyPath |= isOnUsualWay && isInLoopOfIteration();
            m_versionedLoops.last().isLeftAtOtherCodeOnUsualWay |= isOnUsualWay;
            restartIteration();
            return { };
        }
        if (m_isWalkingAccess && !m_openLoops.isEmpty() && effects.mayRunOtherCode() && (m_code.at(m_block).frequency == BlockFrequency::Rare ? canRestartIteration() : isUncaughtIterationEffectFree()) && !looksAtLoopsAroundHaveSeen(effects)) {
            m_versionedLoops.last().isLeftAtAccessOnUsualWay |= m_code.at(m_block).frequency == BlockFrequency::Normal;
            m_innerLoopExitsOnLikelyPath |= m_code.at(m_block).frequency == BlockFrequency::Normal && isInLoopOfIteration();
            restartIteration();
            return { };
        }
    }
    bool endsNoGuard = (!m_trialVisits.isEmpty() || m_inputs.options.checksEffects) && effects.writes.contains(AbstractHeap::MemberGuards) && !effects.mayRunOtherCode() && writerEndsNoGuard(*this, draft);
    if (!m_trialVisits.isEmpty() && (!hasNoEffects || m_inputs.options.loopVisitAbortPeriod) && draft.opcode != Opcode::EndCellLifetime && !isIgnoredArrayStore) {
        bool isCall = effects.mayRunOtherCode() && effectRuleOf(draft.opcode) == EffectRule::Call;
        FunctionID callee = isCall && isWrittenInTopFrame(draft) && !facts().linked().calls.isEmpty() ? facts().linked().calleeAt(currentFunction(), draft.inputValueGiven) : FunctionID();
        if (callee) {
            Effects ofCallee = facts().linked().function(callee).effectsOnReturn();
            ofCallee.merge(facts().linked().function(callee).effectsOnThrow());
            Effects heard = Typed::effectsOf(typing(), draft, typesOfOperands(draft), { }, &ofCallee);
            recordTrialEffects(heard);
            if (isCall && !isCallQuietForArguments(callee))
                noteCallOnUsualWayInTries(heard);
        } else if (endsNoGuard) {
            Effects heard = effects;
            heard.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
            recordTrialEffects(heard);
        } else if (std::optional<Vector<uint32_t, 2>> made = !effects.mayRunOtherCode() && effects.writes.containsAny(perObjectHeaps) ? objectsMadeHereThatAloneAreWritten(*this, draft, effects) : std::nullopt)
            recordTrialEffectsOfWriter(effects, made);
        else {
            recordTrialEffects(effects);
            if (isCall)
                noteCallOnUsualWayInTries(effects);
        }
    }
    if (BlockID row = m_numberOfLoopsThatMayBeLeft < m_openLoops.size() ? rowInFrontOfLoopFor(m_numberOfLoopsThatMayBeLeft, draft, effects) : BlockID(); row && row != m_block) {
        BlockID otherRow = std::exchange(m_block, row);
        Reference built = build(WTF::move(draft), foundByCaller);
        m_block = m_hasFailed ? BlockID() : otherRow;
        return built;
    }
    if (!m_openLoops.isEmpty()) {
        if (!m_openLoops.isEmpty() && proven && !isNeverType(proven)) {
            if (draft.opcode == Opcode::GetEnvironmentField && draft.isKnownInitialized()) {
                if (Reference asked = tryHoistCapturedVariableRead(draft, effects, proven))
                    return asked;
                if (m_hasFailed || isUnreachable())
                    return { };
            }
            std::optional<uint32_t> loop = findLoopToBuildInFrontOf(draft, effects, proven);
            if (loop && excludesInitialPrimitive(draft, proven)) {
                std::optional<uint32_t> ofOwnFrame = findLoopThatReaderVouchesFor(draft, *loop);
                if (ofOwnFrame != loop) {
                    if (Reference asked = tryHoistModuleVariableRead(draft, *loop))
                        return asked;
                    if (m_hasFailed || isUnreachable())
                        return { };
                }
                count(ofOwnFrame ? StatisticID::VariableReadsHoistedWithinFrame : StatisticID::VariableReadsNotHoisted);
                loop = ofOwnFrame;
            } else if (loop && isArrayOrInitialPrimitive(draft, proven)) {
                if (Reference asked = tryHoistModuleVariableRead(draft, *loop))
                    return asked;
                if (m_hasFailed || isUnreachable())
                    return { };
            }
            if (loop) {
            bool isHeldAgainstMemory = checksBounds() && effects.reads.containsAny(AbstractHeapSet { AbstractHeap::ArrayStorage, AbstractHeap::TypedArrayStorage }) && m_types.isSubset(proven, m_types.unionOf(TypeTable::integer, TypeTable::boolean));
            std::optional<Draft> readAgain = isHeldAgainstMemory ? std::optional<Draft>(draft) : std::nullopt;
            if (draft.opcode == Opcode::GetValueAtSiteOrAbsent)
                count(StatisticID::SiteCachedReadsHoisted);
            if (Operation::opcodesWithListHasNoHoles.contains(draft.opcode))
                draft.setListHasNoHoles(false);
            BlockID inside = std::exchange(m_block, m_openLoops[*loop].preheaderBlock);
            Reference built;
            {
                SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, *loop);
                built = build(WTF::move(draft), foundByCaller);
            }
            m_block = m_hasFailed ? BlockID() : inside;
            if (readAgain && built && built.isValue() && representationOf(built) != Representation::RawWord) {
                WalkState believed = m_state;
                checkThatMemoryAnswersAsBelieved(built, WTF::move(*readAgain));
                m_state = WTF::move(believed);
            }
            return built;
            }
        }
    }

    if ((steps & Step::findsEqualOperation) && !m_isBuildingInPlace) {
        if (Reference equal = findEqualOperationBuiltOfItsOpcodes(*this, draft, effects); equal && (m_numberOfLoopsThatMayBeLeft >= m_openLoops.size() || isInvariantInLoop(m_numberOfLoopsThatMayBeLeft, equal)))
            return equal;
    }

    if (effects.maySuspend)
        letObjectsLeaveAtSuspension(*this);
    if (m_state.holdsFreshObjects())
        markEscapingOperands(*this, draft, foundByCaller);
    if (m_environmentCellsOfUnit.mayNameSome && mayKeepEnvironment(draft.opcode))
        markEnvironmentsThatOperationKeeps(*this, draft);

    if (!operandsAreInFormsTaken)
        [[clang::always_inline]] convertOperands(draft);
    if (isUnreachable())
        return { };
    if (effects.mayThrow && !m_isLeadingHandlerEdge && !m_code.at(m_block).handler && (m_handlerMadeOverCopies || numberOfFramesUpToHandlerInForce())) {
        leadHandlerEdge();
        if (isUnreachable())
            return { };
    }

    if (draft.representation == Representation::Unchosen) {
        if (!proven)
            draft.representation = Representation::Void;
        else if (steps & Step::isNarrowed)
            draft.representation = representationOf(draft.valueOperand());
        else if (Representation form = static_cast<Representation>(steps >> Step::shiftOfFormOfEveryResult); form != Representation::Unchosen)
            draft.representation = form;
        else if (steps & Step::resultHasFormOfItsType)
            draft.representation = Typed::representationOf(TypeReader(m_types), proven);
        else
            draft.representation = resultRepresentationOf(*this, draft, proven);
    }
    if ((steps & Step::isTypedByWhatIsProven) && proven && m_positions == m_positionsOfBuild.get() && m_types.at(proven).isJavaScriptValue() && (rawFormsOfJavaScriptValuesInWalk.contains(draft.representation) || (draft.representation == Representation::Void && !isNeverType(proven)))
        && ((m_resultOfReadBeingTyped && draft.opcode == Opcode::ElemGet) || (m_contentsBeingTyped && recordsTypedByWhatIsProvenOfContents.contains(draft.opcode))))
        draft.representation = Representation::JSValue;
    bool checksRecord = m_positionsWithoutProofs && (checksProofs() || (draft.opcode != Opcode::Narrowed && walksBodyWhoseProofsAreChecked() && checksProofsOf(sourceOfProofOf(draft))));
    if (checksRecord) [[unlikely]] {
        if (m_isBuildingInPlace && effects.mayRunOtherCode())
            count(StatisticID::ProofCheckOperationsRunningCode);
        if (draft.opcode == Opcode::CallInline && draft.representation == Representation::Void && proven && !isNeverType(proven) && isLegalRepresentation(m_types, proven, Representation::JSValue))
            draft.representation = Representation::JSValue;
    }
    bool holdsValue = proven && !isNeverType(proven) && hasRegister(draft.representation) && draft.representation != Representation::RawWord;
    if (draft.representation == Representation::Unchosen || (holdsValue && !isLegalRepresentation(m_types, proven, draft.representation))) {
        fail("the result of an operation is kept in a form that its type does not allow"_s);
        return { };
    }

    if (!hasNoEffects)
        endWhatOperationWrites(*this, draft, effects);
    if (effects.mayThrow && !m_isLeadingHandlerEdge && has(Mechanism::Handlers) && numberOfFramesUpToHandlerInForce())
        noteStateAtThrowForHandler();
    bool isBuiltInPreheader = m_numberOfLoopsThatMayBeLeft < m_openLoops.size();
    if (ValueID same = isBuiltInPreheader && !m_isBuildingInPlace ? findSameInFrontOfLoop(m_numberOfLoopsThatMayBeLeft, draft) : m_buildsTestsBesidePlainWalk && !m_isBuildingInPlace ? findSameInFrontOfPlainWalk(draft) : ValueID()) {
        if (steps & Step::notesOperationBuilt)
            noteOperationBuiltOfItsOpcodes(*this, draft, effects, Reference::value(same));
        return Reference::value(same);
    }
    if (!m_openLoops.isEmpty() && isBuildingInPreheader() && !areOperandsLoopInvariant(draft)) [[unlikely]] {
        m_hoistedDiamond = std::nullopt;
        fail("an operation that is built in a preheader takes a value that is defined in the iterations of that loop"_s, InternalErrorKind::HoistedOperandNotInvariant);
        return { };
    }
    ValueID id = m_code.append(m_block, draft, proven, hasNoEffects ? EffectTraits() : EffectTraits(effects));
    if (m_inputs.options.checksEffects && m_operationWritesWhatTurnHasMade && !hasNoEffects && !effects.mayBeRunAgainUnseen()) [[unlikely]]
        m_code.noteThatOperationWritesWhatTurnHasMade(id);
    if (m_inputs.options.verifiesCode) [[unlikely]]
        noteOperationBuiltWhereCodeIsVerified(id, draft);
    if (m_inputs.options.checksEffects && (foundByCaller || endsNoGuard || opcodesWhoseEffectsAreFoundByWalk.contains(draft.opcode))) [[unlikely]] {
        Effects believed = effects;
        if (endsNoGuard)
            believed.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
        m_code.noteEffectsBelieved(id, believed);
    }
    if (draft.testsPassed && m_code.keepsTestsPassed()) [[unlikely]]
        m_code.noteTestsPassedFor(id, *draft.testsPassed);
    if (steps & Step::notesOperationBuilt)
        noteOperationBuiltOfItsOpcodes(*this, draft, effects, Reference::value(id));
    if (steps & Step::isTypedByWhatIsProven) {
        if (m_resultOfReadBeingTyped && readsTypedByWhatIsProvenOfInput.contains(draft.opcode) && m_positions == m_positionsOfBuild.get())
            m_code.addFactUse(FactUse { .record = id, .kind = FactKind::ProvenContents, .subjectKind = FactSubject::Kind::Operation, .type = m_positions->resultOfReadBeingTyped(TypeLevel::Proven) });
        if (m_contentsBeingTyped && recordsTypedByWhatIsProvenOfContents.contains(draft.opcode) && m_positions == m_positionsOfBuild.get())
            m_code.addFactUse(FactUse { .record = id, .kind = FactKind::ProvenContents, .subjectKind = FactSubject::Kind::Operation, .type = m_positions->contentsBeingTyped(TypeLevel::Proven) });
    }
    if (isBuiltInPreheader)
        noteBuiltInFrontOfLoop(m_numberOfLoopsThatMayBeLeft, id);
    if (steps & Step::isCallForStackTests)
        noteDirectCallForStackTests(draft);
    if (checksRecord && holdsValue)
        checkProofOfRecord(id, draft, proven);
    else if ((steps & Step::isTypedByWhatIsProven) && holdsValue && proven && !m_resultOfReadIsRemembered && isTypedByWhatLinkSaysOfContents(draft))
        buildCheckOfProof(Reference::value(id), proven, TypeTable::anything, ProofSource::Contents, draft.origin(), true);
    else if ((steps & Step::isTypedByWhatIsProven) && proven && (holdsValue || isNeverType(proven)) && callsByValueInWalk.contains(draft.opcode))
        askWhatCallByValueGives(id, draft, proven);

    if (!hasNoEffects && ((steps & Step::isToldToPolls) || (effects.mayCollect && has(Mechanism::Handlers) && !m_openLoops.isEmpty())))
        noteRecordBuiltForPolls(id, draft, effects);
    if (proven && isNeverType(proven)) {
        if (std::ranges::any_of(m_openLoops, [&](const auto& loop) { return loop.preheaderBlock == m_block; }))
            count(StatisticID::PreheadersProvenUnreachable);
        endAsUnreachable(m_origin);
        return { };
    }
    if (isStableRead)
        noteStableField(draft, Reference::value(id));
    return Reference::value(id);
}

Reference Walker::constant(Draft&& draft)
{
    if (m_hasFailed)
        return { };
    for (Reference operand : draft.operands()) {
        if (operand) {
            fail("what has an operand is no constant"_s);
            return { };
        }
    }

    bool isShared = constantsInFormOfUses.contains(draft.opcode);
    bool leavesFormOpen = draft.representation == Representation::Unchosen;
    unsigned hash = 0;
    if (isShared) {
        hash = hashOfWordsOfConstantInWalk(draft);
        if (!m_sharedConstants.isEmpty()) {
            size_t mask = m_sharedConstants.size() - 1;
            for (size_t place = hash & mask; m_sharedConstants[place] != noSharedConstantInWalk; place = (place + 1) & mask) {
                uint32_t shared = m_sharedConstants[place];
                if (leavesFormOpen && !(shared & hasFormOfItsTypeInWalk))
                    continue;
                ValueID id(shared & ~hasFormOfItsTypeInWalk);
                const Operation& record = m_code.at(id);
                if ((leavesFormOpen || record.representation == draft.representation) && haveSameWordsInWalk(record, draft))
                    return Reference::constant(id);
            }
        }
    }

    TypeID proven = isShared ? typeOfConstant(typing(), draft) : resultTypeOf(draft);
    if (proven && draft.representation == Representation::Unchosen) {
        if (isShared)
            draft.representation = Typed::representationOf(TypeReader(m_types), proven);
        else
            draft.representation = resultRepresentationOf(*this, draft, proven);
    }
    if (!proven || draft.representation == Representation::Unchosen) {
        fail("the rules of a constant give no type or no form"_s);
        return { };
    }
    if (hasRegister(draft.representation) && !isLegalRepresentation(m_types, proven, draft.representation)) {
        fail("a constant is kept in a form that its type does not allow"_s);
        return { };
    }
    if (!isShared)
        return Reference::constant(m_code.addConstant(draft, proven));

    if (leavesFormOpen && !m_sharedConstants.isEmpty()) {
        size_t mask = m_sharedConstants.size() - 1;
        for (size_t place = hash & mask; m_sharedConstants[place] != noSharedConstantInWalk; place = (place + 1) & mask) {
            ValueID id(m_sharedConstants[place] & ~hasFormOfItsTypeInWalk);
            const Operation& record = m_code.at(id);
            if (record.representation == draft.representation && haveSameWordsInWalk(record, draft)) {
                m_sharedConstants[place] |= hasFormOfItsTypeInWalk;
                return Reference::constant(id);
            }
        }
    }
    ValueID added = m_code.addConstant(draft, proven);
    addSharedConstant(hash, added.index() | (leavesFormOpen ? hasFormOfItsTypeInWalk : 0));
    return Reference::constant(added);
}

Reference Walker::cellThatRunTimeKeeps(Draft& draft)
{
    static constexpr OpcodeSet cellsThatRunTimeKeeps { Opcode::Builtin, Opcode::GetModuleVariable };
    static_assert(!cellsThatRunTimeKeeps.intersects(constantsInFormOfUses));
    if (m_hasFailed || !cellsThatRunTimeKeeps.contains(draft.opcode) || m_positionsWithoutProofs || hasStringsInBuilders())
        return { };
    FunctionID born;
    if (draft.opcode == Opcode::GetModuleVariable) {
        PlannedVariable planned = plan().variable(draft.variable());
        if (planned.encoding != VariableEncoding::FunctionAsZero || !planned.function)
            return { };
        born = planned.function;
    }

    unsigned hash = hashOfWordsOfConstantInWalk(draft);
    if (!m_sharedConstants.isEmpty()) {
        size_t mask = m_sharedConstants.size() - 1;
        for (size_t place = hash & mask; m_sharedConstants[place] != noSharedConstantInWalk; place = (place + 1) & mask) {
            ValueID id(m_sharedConstants[place] & ~hasFormOfItsTypeInWalk);
            if (haveSameWordsInWalk(m_code.at(id), draft))
                return Reference::constant(id);
        }
    }

    TypeID proven = resultTypeOf(draft);
    if (!proven || isNeverType(proven))
        return { };
    if (born) {
        Type read = m_types.at(proven);
        if (read.primitives || !read.isJavaScriptValue() || singleFunctionOf(m_types, read) != born)
            return { };
    }
    Representation form = resultRepresentationOf(*this, draft, proven);
    if (!hasRegister(form) || !isLegalRepresentation(m_types, proven, form))
        return { };
    draft.representation = form;
    ValueID added = m_code.addConstant(draft, proven);
    addSharedConstant(hash, added.index() | hasFormOfItsTypeInWalk);
    m_holdsReadsOfVariablesAsConstants |= !!born;
    return Reference::constant(added);
}

ALWAYS_INLINE_INTO_OTHER_FILES Reference Walker::readVariableWhereItIsTaken(Reference constant, Origin origin)
{
    if (m_code.at(constant.id()).opcode != Opcode::GetModuleVariable)
        return constant;
    VariableID variable = m_code.at(constant.id()).variable();
    TypeID type = m_code.at(constant.id()).type;
    auto canTake = [&](Reference value) {
        return value && value.isValue() && (m_openLoops.isEmpty() || !isBuildingInPreheader() || isOperandLoopInvariant(value));
    };
    if (Reference held = m_state.variable(variable); canTake(held) && typeOf(held) == type)
        return held;
    if (Reference kept = valueOfStableVariable(*this, variable, type); canTake(kept))
        return kept;
    origin.isTailCall = false;
    Draft read(Opcode::GetModuleVariable, origin);
    read.setVariable(variable);
    read.flags = m_code.at(constant.id()).flags;
    Reference built = build(WTF::move(read));
    if (!built)
        return { };
    m_state.noteVariable(variable, built);
    noteValueOfStableBinding(*this, variable, built);
    return built;
}

static constexpr uint32_t bitsOfFormOfCopyInWalk = 4;
static_assert(numberOfEnumerators<Representation> <= 1u << bitsOfFormOfCopyInWalk);

static ALWAYS_INLINE size_t placeOfCopyOfConstantInWalk(uint32_t constantAndForm)
{
    return (constantAndForm * 0x9E3779B1u) >> 12;
}

ValueID Walker::findCopyOfConstant(ValueID constant, Representation form) const
{
    if (m_copiesOfConstants.isEmpty() || constant.index() >= 1u << (32 - bitsOfFormOfCopyInWalk))
        return { };
    uint32_t sought = constant.index() << bitsOfFormOfCopyInWalk | static_cast<uint32_t>(form);
    size_t mask = m_copiesOfConstants.size() - 1;
    for (size_t place = placeOfCopyOfConstantInWalk(sought) & mask; m_copiesOfConstants[place].copy; place = (place + 1) & mask) {
        if (m_copiesOfConstants[place].constantAndForm == sought)
            return m_copiesOfConstants[place].copy;
    }
    return { };
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::noteCopyOfConstant(ValueID constant, Representation form, ValueID copy)
{
    if (constant.index() >= 1u << (32 - bitsOfFormOfCopyInWalk))
        return;
    auto placeFor = [&](uint32_t constantAndForm) {
        size_t mask = m_copiesOfConstants.size() - 1;
        size_t place = placeOfCopyOfConstantInWalk(constantAndForm) & mask;
        while (m_copiesOfConstants[place].copy)
            place = (place + 1) & mask;
        return place;
    };
    if ((m_numberOfCopiesOfConstants + 1) * 2 > m_copiesOfConstants.size()) {
        Vector<CopyOfConstant, 16> former = std::exchange(m_copiesOfConstants, { });
        m_copiesOfConstants.fill(CopyOfConstant { }, std::max<size_t>(16, former.size() * 2));
        for (const CopyOfConstant& earlier : former) {
            if (earlier.copy)
                m_copiesOfConstants[placeFor(earlier.constantAndForm)] = earlier;
        }
    }
    ++m_numberOfCopiesOfConstants;
    uint32_t constantAndForm = constant.index() << bitsOfFormOfCopyInWalk | static_cast<uint32_t>(form);
    m_copiesOfConstants[placeFor(constantAndForm)] = { constantAndForm, copy };
    if (m_trialVisitDepth) [[unlikely]]
        m_copiesOfConstantsAddedInTries.append(constantAndForm);
}

void Walker::forgetCopiesOfConstantsAddedBehind(size_t numberLogged)
{
    while (m_copiesOfConstantsAddedInTries.size() > numberLogged) {
        uint32_t added = m_copiesOfConstantsAddedInTries.takeLast();
        size_t mask = m_copiesOfConstants.size() - 1;
        size_t hole = placeOfCopyOfConstantInWalk(added) & mask;
        while (m_copiesOfConstants[hole].constantAndForm != added) {
            RELEASE_ASSERT(m_copiesOfConstants[hole].copy);
            hole = (hole + 1) & mask;
        }
        for (size_t next = (hole + 1) & mask; m_copiesOfConstants[next].copy; next = (next + 1) & mask) {
            size_t home = placeOfCopyOfConstantInWalk(m_copiesOfConstants[next].constantAndForm) & mask;
            if (hole <= next ? (hole < home && home <= next) : (hole < home || home <= next))
                continue;
            m_copiesOfConstants[hole] = m_copiesOfConstants[next];
            hole = next;
        }
        m_copiesOfConstants[hole] = CopyOfConstant { };
        --m_numberOfCopiesOfConstants;
    }
}

bool Walker::copy(const BodyValue& value)
{
    Draft draft = draftOf(value);
    if (isUnreachable())
        return true;

    chooseConservativeFields(draft);
    if (draft.opcode == Opcode::PropGet && has(Mechanism::Guesses) && !draft.isFreeName())
        draft.setReceiverIsNoObjectOfCompiledCode(isShapedLikeLayoutsThatItCanNeverBe(*this, value.objectOperand()));
    if (draft.opcode == Opcode::PropGet && has(Mechanism::Guesses) && !draft.isFreeName())
        draft.setIsErrorMessageExpected(isInLoop() && isErrorMessageExpected(*this, value.objectOperand(), draft.key()));
    if (draft.opcode == Opcode::PropGet && has(Mechanism::Guesses) && !draft.isFreeName() && !liesInThread(draft.key()) && program().nameClassOf(draft.key()) == NameClass::Length) {
        draft.setReceiverIsAskedForNameOfNoBuiltin(m_walksCopyWhereHolderIsNoBuiltin || isAskedForNameThatNoBuiltinAnswers(*this, value.objectOperand()));
        draft.setReceiverIsExpectedToBeArguments(isInLoop() && !draft.receiverIsAskedForNameOfNoBuiltin() && isExpectedToBeArgumentsObject(*this, value.objectOperand()));
        if (draft.receiverIsExpectedToBeArguments())
            count(StatisticID::ArgumentsLengthsReadInline);
    }
    bool givesResult;
    TypeID libraryObjectType;
    if (stepsOfBuildOf(draft.opcode) & StepOfBuildInWalk::isLookedAtByCopy) {
        if (draft.opcode == Opcode::Call && !draft.calleeKey() && draft.calleeOperand().isValue() && draft.thisValueOperand().isRecord()) {
            const Operation& read = m_code.at(draft.calleeOperand().id());
            if (read.opcode == Opcode::PropGet && !read.isFreeName() && read.objectOperand() == draft.thisValueOperand().id())
                draft.setCalleeKey(read.key());
        }
        if (draft.opcode == Opcode::Call && has(Mechanism::BuiltinCalls))
            nameMethodOfReceiverAsMade(*this, value, draft);
        if ((draft.opcode == Opcode::CallList || draft.opcode == Opcode::ConstructList || draft.opcode == Opcode::SuperConstructList) && has(Mechanism::EscapeAnalysis))
            elideSpreadCopy(*this, value, draft);
        if (draft.opcode == Opcode::StrictEq && has(Mechanism::Guesses) && m_opensGuesses)
            sayWhatIsExpectedOfComparedOperands(*this, value, draft);
        if (draft.opcode == Opcode::ToBoolean && has(Mechanism::Guesses) && m_opensGuesses)
            sayWhatIsExpectedOfValueAskedForTruth(*this, value, draft);
        if (TypeID given = draft.opcode == Opcode::Call ? asFarAsUnitMayAsk(facts().linked().nativeCallResult(currentFunction(), value.id())) : TypeID(); given && has(Mechanism::BuiltinCalls)) {
            if (Reference built = buildCallAsWrittenThatGives(*this, WTF::move(draft), given))
                bind(value.id(), built);
            return true;
        }
        givesResult = resultRuleInWalk(draft.opcode) != ResultRule::None;
        if (draft.opcode == Opcode::Builtin)
            libraryObjectType = typeOfLibraryObject(draft.builtin());
        if (draft.opcode == Opcode::Builtin && has(Mechanism::Folding)) {
            if (Reference cell = cellThatRunTimeKeeps(draft)) {
                if (libraryObjectType)
                    cell = proveOrTrap(cell, libraryObjectType, ProofSource::ResultOfCall, m_origin);
                if (cell)
                    bind(value.id(), cell);
                return true;
            }
        }
    } else
        givesResult = !(stepsOfBuildOf(draft.opcode) & StepOfBuildInWalk::givesNoResult);
    Reference built = (stepsOfBuildOf(draft.opcode) & StepOfBuildInWalk::isConstantInFormOfUses) ? constant(WTF::move(draft)) : build(WTF::move(draft));
    if (built && libraryObjectType)
        built = proveOrTrap(built, libraryObjectType, ProofSource::ResultOfCall, m_origin);
    if (built && givesResult)
        bind(value.id(), built);
    return true;
}

TypeID Walker::typeOfLibraryObject(BuiltinID builtin)
{
    ModuleID file = builtin && !program().builtins().at(builtin).holder ? compiledModuleWithObject(program(), program().builtins().canonicalID(builtin)) : ModuleID();
    if (!file || !facts().linked().modulesWhoseValueStaysIn.get(file.index()) || !objectOfLibraryFileIsResultOfBody(program(), file))
        return { };
    TypeID returned = asFarAsUnitMayAsk(m_positionsOfBuild->result(TypeLevel::Proven, program().at(file).body, 0));
    if (!returned || holdsOnlyPrimitives(returned))
        return { };
    TypeID testable = m_types.testablePartOf(returned);
    return m_types.at(testable).hasOnlyLayouts() ? testable : TypeID();
}

Reference Walker::addIndexedIteration(IndexedIteration&& iteration)
{
    iteration.inlineFrame = m_frames.last().inlineFrame;
    m_numberOfIterationsInFrontOfTheirLoops += !!iteration.inputHead;
    if (iteration.inputHead)
        if (m_inputHeadsOfIterations.add(inputHeadInFrame(iteration.inlineFrame, iteration.inputHead)).isNewEntry && m_trialVisitDepth)
            m_inputHeadsOfIterationsAddedInTries.append(inputHeadInFrame(iteration.inlineFrame, iteration.inputHead));
    m_indexedIterations.append(WTF::move(iteration));
    return Reference::virtualObject(VirtualObjectID(m_indexedIterations.size() - 1));
}

void Walker::applyUnderstatedEffectsSlow(const Draft& draft, Effects& effects) const
{
    UnderstatedEffects row = UnderstatedEffects::decode(m_inputs.options.understatedEffects);
    row.cut(KindOfEffectsRow::Operation, static_cast<unsigned>(draft.opcode), effects);
    if (Operation::opcodesWithOverload.contains(draft.opcode))
        row.cut(KindOfEffectsRow::Builtin, BuiltinTable::singleton().at(draft.overload()).builtin.index(), effects);
    if (draft.opcode == Opcode::CallDirect)
        row.cut(KindOfEffectsRow::Callee, draft.function().index(), effects);
}

void Walker::leaveArrivals(FunctionID callee, std::span<const Reference> arguments)
{
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = callee;
    if (!m_arrivals.takesWhatWasLeft(callee))
        return;
    const Function& function = program().at(callee);
    for (uint32_t entryParameter = 0; entryParameter < arguments.size(); ++entryParameter) {
        Reference argument = arguments[entryParameter];
        bool goesByWhatIsShown = argument && argument.isRecord() && (entryParameter >= function.numberOfEntryParameters() || function.entryParameterKind(entryParameter) != EntryParameter::Capture);
        TypeID proven = goesByWhatIsShown ? typeShown(argument) : ownTypeOf(argument);
        if (TypeID own = goesByWhatIsShown ? ownTypeOf(argument) : TypeID(); proven && own && m_types.at(own).isJavaScriptValue() && !m_types.isSubset(own, proven))
            proven = m_types.unionOf(proven, own);
        if (!proven)
            proven = TypeTable::anything;
        m_arrivalsHeld.append({ callee, entryParameter, proven, ArrivalHeld::Kind::Proven });
    }
    m_arrivalsHeld.append({ callee, static_cast<uint32_t>(arguments.size()), { }, ArrivalHeld::Kind::UndefinedFrom });
}

void Walker::leaveAnythingAt(FunctionID callee)
{
    m_arrivalsHeld.append({ callee, 0, { }, ArrivalHeld::Kind::Anything });
}

void Walker::handOverArrivals()
{
    for (const ArrivalHeld& held : m_arrivalsHeld) {
        switch (held.kind) {
        case ArrivalHeld::Kind::Proven:
            m_arrivals.leave(m_types, held.callee, held.entryParameter, held.proven);
            break;
        case ArrivalHeld::Kind::UndefinedFrom:
            m_arrivals.leaveUndefinedFrom(m_types, held.callee, held.entryParameter);
            break;
        case ArrivalHeld::Kind::Anything:
            m_arrivals.leaveAnything(held.callee);
            break;
        case ArrivalHeld::Kind::MayBeEnteredByValue:
            break;
        }
    }
    m_arrivalsHeld.shrink(0);
}

static ALWAYS_INLINE void takeStateOfBlockThatIsBegun(WalkState& state, WalkState& stateOfBlock)
{
    state = WTF::move(stateOfBlock);
    stateOfBlock.freeWhatIsHeld();
}

Walker::PendingBlock* Walker::findPendingBlock(BlockID block)
{
    PendingBlock* first = m_pendingBlocks.begin();
    for (PendingBlock* pending = m_pendingBlocks.end(); pending != first;) {
        if ((--pending)->block == block)
            return pending;
    }
    return nullptr;
}

Walker::PendingBlock& Walker::pendingBlock(BlockID block)
{
    if (PendingBlock* found = findPendingBlock(block))
        return *found;
    moveWhereSanitizerWatches(m_pendingBlocks);
    m_pendingBlocks.constructAndAppend();
    PendingBlock& pending = m_pendingBlocks.last();
    pending.block = block;
    pending.parametersAreSet = true;
    return pending;
}

void Walker::releaseEndedPendingBlocks()
{
    moveWhereSanitizerWatches(m_pendingBlocks);
    m_pendingBlocks.removeAllMatching([&](const PendingBlock& pending) {
        return m_code.at(pending.block).hasEnded();
    });
}

void Walker::settleParametersOf(BlockID block)
{
    PendingBlock& pending = pendingBlock(block);
    if (std::exchange(pending.parametersAreSet, true) || pending.parameters.isEmpty())
        return;
    m_code.setParameters(block, pending.parameters.span());
}

void Walker::arriveAt(BlockID block, WalkState&& state)
{
    pendingBlock(block).addArrival(WTF::move(state));
}

bool Walker::isOutsideLoopsOfAsyncBody() const
{
    if (unit().role != CodeRole::CoroutineBody || isInLoop())
        return false;
    const Function& function = program().at(unit().function);
    return function.has(FunctionFlags::IsAsync) && !function.has(FunctionFlags::IsGenerator);
}

BlockID Walker::addBlock(BlockFrequency frequency)
{
    BlockID block = m_code.addBlock(BlockRole::Ordinary, m_coldCopyDepth ? BlockFrequency::Rare : frequency);
    if (!m_coldCopyDepth && isInLoop())
        m_code.setLoopMarks(block, true, false);
    moveWhereSanitizerWatches(m_pendingBlocks);
    m_pendingBlocks.constructAndAppend();
    PendingBlock& pending = m_pendingBlocks.last();
    pending.block = block;
    pending.isMadeByAddBlock = true;
    return block;
}

Reference Walker::addParameter(BlockID block, TypeID type, Representation representation)
{
    ValueID parameter = m_code.appendParameter(block, type ? type : TypeID(TypeTable::never), representation, m_origin);
    PendingBlock& pending = pendingBlock(block);
    pending.parameters.append(parameter);
    pending.parametersAreSet = false;
    return Reference::value(parameter);
}

template<typename PendingBlock>
static ALWAYS_INLINE void settleParametersInWalk(Code& code, BlockID block, PendingBlock& pending)
{
    if (!std::exchange(pending.parametersAreSet, true) && !pending.parameters.isEmpty())
        code.setParameters(block, pending.parameters.span());
}

static ALWAYS_INLINE bool isAsBranchTakesItInWalk(const Walker& walker, Reference condition)
{
    if (walker.hasStringsInBuilders())
        return false;
    TypeID type = walker.typeOf(condition);
    return type && isBooleanType(type) && walker.representationOf(condition) == wantsOf(OperandKind::Boolean);
}

void Walker::jumpTo(BlockID next, std::span<const Reference> arguments)
{
    PendingBlock* pending = &pendingBlock(next);
    settleParametersInWalk(m_code, next, *pending);
    if (!isUnreachable()) {
        const Block& target = m_code.at(next);
        std::span<const ValueID> parameters = target.parameters();
        if (parameters.size() != arguments.size()) {
            fail("a jump does not fill the parameters of its target"_s);
            return;
        }

        bool isFirstArrival = !pending->numberOfArrivals;
        bool typesFollowArrivals = !target.isLoopHead && !target.isEntry && !target.hasEnded() && !m_code.lastOperationOf(next);
        for (size_t index = 0; index < arguments.size(); ++index) {
            TypeID arriving = ownTypeOf(arguments[index]);
            TypeID held = m_code.at(parameters[index]).type;
            if (!arriving) {
                fail("a jump hands on what has no type"_s);
                return;
            }
            if (typesFollowArrivals) {
                TypeID joined = isFirstArrival ? arriving : joinTypes(held, arriving);
                if (joined != held)
                    m_code.setTypeOfJoinParameter(parameters[index], joined);
            } else if (!m_types.isSubset(arriving, held)) {
                fail("what a jump hands on lies outside the type of the parameter that takes it"_s);
                return;
            }
        }
        size_t numberOfPendingBlocks = m_pendingBlocks.size();
        crossEdge({ }, next, arguments);
        if (m_hasFailed)
            return;
        if (m_pendingBlocks.size() != numberOfPendingBlocks)
            pending = &pendingBlock(next);
        bool knowsNothing = pending->hasLostState();
        ++pending->numberOfArrivals;
        m_block = next;
        if (std::exchange(pending->hasState, false)) {
            pending->state.meetWith(m_state);
            takeStateOfBlockThatIsBegun(m_state, pending->state);
        } else if (knowsNothing)
            m_state.clear();
        return;
    }

    if (m_hasFailed || !pending->numberOfArrivals) {
        m_block = { };
        m_state.clear();
        return;
    }
    m_block = next;
    if (std::exchange(pending->hasState, false))
        takeStateOfBlockThatIsBegun(m_state, pending->state);
    else
        m_state.clear();
}

void Walker::branch(Reference condition, BlockID taken, BlockID notTaken, bool shouldRecordEdgeOutcomes)
{
    if (isUnreachable())
        return;
    BlockID from = m_block;
    bool isAsTaken = isAsBranchTakesItInWalk(*this, condition);
    if (!isAsTaken) {
        TerminatorDraft terminator(TerminatorKind::Branch, m_origin);
        terminator.setConditionOperand(condition);
        askTruthOfOperandsNotProvenBoolean(terminator);
        if (isUnreachable() || !checkOperandKinds(terminator))
            return;
        convertOperands(terminator);
        if (isUnreachable())
            return;
        from = m_block;
        m_code.end(from, terminator, 2);
    }


    PendingBlock* pendingTaken = &pendingBlock(taken);
    settleParametersInWalk(m_code, taken, *pendingTaken);
    size_t numberOfPendingBlocks = m_pendingBlocks.size();
    PendingBlock* pendingNotTaken = &pendingBlock(notTaken);
    if (m_pendingBlocks.size() != numberOfPendingBlocks)
        pendingTaken = &pendingBlock(taken);
    settleParametersInWalk(m_code, notTaken, *pendingNotTaken);
    if (isAsTaken)
        m_code.endWithBranch(from, condition.id(), m_origin, taken, notTaken);
    else {
        const Terminator& end = m_code.at(from).terminator;
        EdgeID edgeTaken = end.taken();
        EdgeID edgeNotTaken = end.notTaken();
        m_code.closeEdge(edgeTaken, taken, { });
        m_code.closeEdge(edgeNotTaken, notTaken, { });
    }
    std::optional<WalkState> atEnd;
    if (shouldRecordEdgeOutcomes) {
        atEnd = m_state;
        recordEdgeOutcome(*this, from, 0);
    }
    pendingTaken->addArrival(m_state);
    if (atEnd) {
        m_state = WTF::move(*atEnd);
        recordEdgeOutcome(*this, from, 1);
    }
    pendingNotTaken->addArrival(WTF::move(m_state));
    m_block = { };
    m_state.clear();
}

std::optional<PassedTest> Walker::findPassedTest(Reference test, bool passesWithYes)
{
    if (!test || !test.isRecord())
        return std::nullopt;
    if (!test.isValue()) {
        const Operation& constant = m_code.at(test.id());
        if (constant.opcode != Opcode::Boolean || constant.boolean() != passesWithYes)
            return std::nullopt;
        return PassedTest();
    }
    if (outcomeKnownOf(*this, test) != std::optional<bool>(passesWithYes))
        return std::nullopt;
    return PassedTest(test.id(), passesWithYes);
}

std::optional<PassedTest> Walker::findWhatHoldsWordInsideListMadeByWalk(Reference listInParameter, Reference listAsMade, Reference word)
{
    if (!listInParameter || !listInParameter.isRecord() || !listAsMade || !listAsMade.isValue() || !word || !word.isConstant() || m_code.at(word.id()).opcode != Opcode::Integer)
        return std::nullopt;
    const Operation& made = m_code.at(listAsMade.id());
    int64_t place = m_code.at(word.id()).integer();
    if (made.opcode != Opcode::NewArray || !made.holes().empty() || place < 0 || static_cast<uint64_t>(place) >= made.elementsOperands().size())
        return std::nullopt;
    PassedTest inside;
    inside.m_groundsOfPosition = GroundsOfPosition { .view = listInParameter.id(), .position = word.id(), .length = listAsMade.id() };
    return inside;
}

std::optional<PassedTest> Walker::boundsProofAfterComparison(Reference list, Reference position, Reference length, Reference lengthCheck)
{
    std::optional<PassedTest> below = findPassedTest(lengthCheck);
    std::optional<PassedTest> fromZeroUp = below ? findNonNegativeProof(position) : std::nullopt;
    return fromZeroUp ? boundsProofFromChecks(list, position, length, *fromZeroUp, *below) : std::nullopt;
}

std::optional<PassedTest> Walker::boundsProofAfterExtension(Reference list, Reference position, Reference raise)
{
    if (!list || !list.isRecord() || !position || !position.isRecord() || !raise || !raise.isValue() || m_code.at(raise.id()).opcode != Opcode::ExtendArrayLength)
        return std::nullopt;
    PassedTest inside;
    inside.m_groundsOfPosition = GroundsOfPosition { .view = list.id(), .position = position.id(), .length = raise.id() };
    return inside;
}

std::optional<PassedTest> Walker::showElementPresentBehindTest(Reference list, Reference position, const std::optional<PassedTest>& passed)
{
    if (!passed || !list || !list.isRecord() || !position || !position.isRecord() || !testsShowElementPresent(m_code, list.id(), position.id(), passed->sides()))
        return std::nullopt;
    PassedTest present = *passed;
    present.m_groundsOfPosition = GroundsOfPosition { .view = list.id(), .position = position.id() };
    return present;
}

bool Walker::stillHolds(const PassedTest& passed)
{
    return std::ranges::all_of(passed.sides(), [&](const PassedTest::Side& side) {
        return !!findPassedTest(referenceTo(side.test), side.testResult);
    });
}

std::optional<PassedTest> Walker::continueIfCheckPasses(Reference test, BlockID whereItFails, bool passesWithYes)
{
    if (isUnreachable())
        return std::nullopt;
    if (std::optional<PassedTest> known = findPassedTest(test, passesWithYes))
        return known;
    if (!test || !test.isValue())
        return std::nullopt;
    BlockID passes = addBlock();
    if (passesWithYes)
        branchOnTest(test, passes, whereItFails);
    else
        branchOnTest(test, whereItFails, passes);
    continueIn(passes);
    return isUnreachable() ? std::nullopt : findPassedTest(test, passesWithYes);
}

Reference Walker::tryCallInline(TerminatorDraft&& terminator, BlockID done, BlockID miss, const Effects* foundByCaller)
{
    RELEASE_ASSERT(terminator.kind == TerminatorKind::TryCallInline);
    if (isUnreachable() || !checkOperandKinds(terminator))
        return { };
    constexpr uint32_t targetOfDone = 0;
    TypeID given = edgeResultTypeOf(typing(), terminator, typesOfOperands(terminator), targetOfDone, 0);
    if (!given) {
        fail("the result rule of a terminator gives no type"_s);
        return { };
    }
    Effects effects = effectsOfTerminator(typing(), terminator, typesOfOperands(terminator), foundByCaller);
    if (effects.mayCollect && isUnderHandler())
        effects.mayThrow |= ThrowEffect::onlyForLackOfMemory();

    for (Reference operand : terminator.operands()) {
        if (operand)
            letLeave(operand);
    }
    convertOperands(terminator);
    if (isUnreachable())
        return { };
    if (effects.mayThrow) {
        leadHandlerEdge();
        if (isUnreachable())
            return { };
    }
    std::optional<Vector<uint32_t, 2>> receiverMadeHere = m_trialVisits.isEmpty() ? std::nullopt : receiverMadeHereThatAloneIsWritten(*this, terminator, effects);
    if (receiverMadeHere)
        count(StatisticID::LocalArrayMethodEffectsNarrowed);
    recordTrialEffectsOfWriter(effects, receiverMadeHere);
    m_state.invalidate(effects);
    if (effects.mayThrow && has(Mechanism::Handlers))
        noteStateAtThrowForHandler(terminator.operands());

    Representation formOfAnswer = Typed::representationOf(TypeReader(m_types), given);
    if (checksProofs() && formOfAnswer == Representation::Void && !isNeverType(given) && isLegalRepresentation(m_types, given, Representation::JSValue))
        formOfAnswer = Representation::JSValue;
    bool isTypedByContents = m_contentsBeingTyped && m_positions == m_positionsOfBuild.get() && rawFormsOfJavaScriptValuesInWalk.contains(formOfAnswer);
    if (isTypedByContents)
        formOfAnswer = Representation::JSValue;
    Reference result = addParameter(done, given, formOfAnswer);
    if (m_contentsBeingTyped && m_positions == m_positionsOfBuild.get() && result && result.isValue())
        m_code.addFactUse(FactUse { .record = result.id(), .kind = FactKind::ProvenContents, .subjectKind = FactSubject::Kind::Operation, .type = m_positions->contentsBeingTyped(TypeLevel::Proven) });
    settleParametersOf(done);
    settleParametersOf(miss);
    BlockID from = m_block;
    m_code.end(from, terminator, 2);
    m_code.closeEdge(m_code.at(from).terminator.done(), done, { });
    m_code.closeEdge(m_code.at(from).terminator.miss(), miss, { });
    arriveAt(done, WalkState(m_state));
    arriveAt(miss, WTF::move(m_state));
    m_state.clear();
    m_block = isNeverType(given) ? done : BlockID();
    endAsUnreachable(terminator.origin());
    return result;
}

void Walker::leaveBlockPending()
{
    if (!isUnreachable()) {
        PendingBlock& left = pendingBlock(m_block);
        left.state = WTF::move(m_state);
        left.hasState = true;
    }
    m_block = { };
    m_state.clear();
}

void Walker::continueIn(BlockID block)
{
    if (m_hasFailed) {
        m_block = { };
        return;
    }
    if (m_block == block)
        return;
    leaveBlockPending();

    PendingBlock* pending = findPendingBlock(block);
    if (!pending) {
        m_block = block;
        return;
    }
    settleParametersInWalk(m_code, block, *pending);
    if (pending->isMadeByAddBlock && !pending->numberOfArrivals && !pending->hasState && !m_code.at(block).isEntry) {
        m_block = { };
        return;
    }
    m_block = block;
    if (std::exchange(pending->hasState, false))
        takeStateOfBlockThatIsBegun(m_state, pending->state);
}

bool Walker::joinArms(std::span<const JoinArm> arms, BlockID meeting, std::span<Reference> met)
{
    Vector<size_t, 8> positionsOfParameters;
    Vector<Representation, 4> forms;
    PendingBlock* pendingMeeting = nullptr;
    auto addParameterOfMeeting = [&](TypeID type, Representation representation) {
        if (!pendingMeeting)
            pendingMeeting = &pendingBlock(meeting);
        ValueID parameter = m_code.appendParameter(meeting, type ? type : TypeID(TypeTable::never), representation, m_origin);
        pendingMeeting->parameters.append(parameter);
        pendingMeeting->parametersAreSet = false;
        return Reference::value(parameter);
    };
    for (size_t position = 0; position < met.size() && !arms.empty(); ++position) {
        Reference first = arms.front().values[position];
        met[position] = first;
        bool allBringOneValue = true;
        for (const JoinArm& arm : arms) {
            if (!arm.values[position]) {
                fail("an arm has no value for what meets behind it"_s);
                return false;
            }
            allBringOneValue &= arm.values[position] == first;
        }
        if (allBringOneValue)
            continue;

        TypeID type;
        forms.shrink(0);
        Representation representationOfLikelyArms = Representation::Unchosen;
        bool usualArmsAgree = true;
        for (const JoinArm& arm : arms) {
            TypeID own = m_code.at(arm.values[position].id()).type;
            type = type ? joinOf(types(), facts().families(), type, own) : own;
            Representation form = representationOf(arm.values[position]);
            forms.append(form);
            if (form == Representation::JSValue && m_code.at(arm.last).frequency == BlockFrequency::Rare)
                continue;
            usualArmsAgree &= representationOfLikelyArms == Representation::Unchosen || representationOfLikelyArms == form;
            representationOfLikelyArms = form;
        }
        bool holdsRepresentationOfLikelyArms = usualArmsAgree && has(Mechanism::UnboxedNumbers) && optionalRepresentations.contains(representationOfLikelyArms) && Typed::representationOf(TypeReader(m_types), type) == representationOfLikelyArms;
        met[position] = addParameterOfMeeting(type, holdsRepresentationOfLikelyArms ? representationOfLikelyArms : formOfParameter(type, forms.span()));
        positionsOfParameters.append(position);
    }
    pendingMeeting = nullptr;
    Vector<FieldOfScope, 4> fieldsThatMeet;
    if (unitHasMadeScopes() || !m_fieldsOfScopesOfHeads.isEmpty() || !m_carriedResultRequests.isEmpty() || mayMeetUnmadeRecords(m_state)) {
        Vector<const WalkState*, 4> states;
        for (const JoinArm& arm : arms) {
            const PendingBlock* left = arm.last == m_block ? nullptr : findPendingBlock(arm.last);
            if (arm.last != m_block && (!left || !left->hasState)) {
                states.shrink(0);
                break;
            }
            states.append(left ? &left->state : &m_state);
        }
        findFieldsOfScopesThatMeet(states.span(), fieldsThatMeet);
        for (FieldOfScope& field : fieldsThatMeet)
            field.parameter = addParameter(meeting, field.type, field.form);
    }

    Vector<Reference, 8> arguments;
    size_t way = 0;
    for (const JoinArm& arm : arms) {
        arguments.shrink(0);
        for (size_t position : positionsOfParameters)
            arguments.append(arm.values[position]);
        for (const FieldOfScope& field : fieldsThatMeet)
            arguments.append(field.values[way]);
        ++way;
        continueIn(arm.last);
        jumpTo(meeting, arguments.span());
        if (hasFailed())
            return false;
    }
    continueIn(meeting);
    if (isUnreachable())
        return false;
    noteFieldsOfScopes(fieldsThatMeet.span());
    return true;
}

Walker::StretchInArms Walker::stretchInArms(BlockID inputBlock, uint32_t begin, uint32_t end) const
{
    const BodyIndex& index = currentBodyIndex();
    uint32_t first = index.block(inputBlock).firstValue().index() + begin;
    uint32_t size = end - begin;
    StretchInArms stretch { inputBlock, begin, end, false, { } };

    Vector<uint32_t, 32> mentions;
    mentions.fill(0, size);
    for (uint32_t place = 1; place < size; ++place) {
        index.value(ValueID(first + place)).forEachOperand([&](ValueID operand, uint32_t) {
            if (operand && operand.index() >= first)
                ++mentions[operand.index() - first];
        });
    }
    for (uint32_t place = 0; place < size; ++place) {
        uint32_t uses = index.useCount(ValueID(first + place));
        if (uses == BodyIndex::maxCountedUses || uses > mentions[place])
            stretch.mentionedBehind.append(ValueID(first + place));
    }
    BodyTerminator terminator = index.terminator(inputBlock);
    stretch.isReturnedAtOnce = terminator.kind() == TerminatorKind::ReturnCall && terminator.operand(0) == ValueID(first + size - 1);
    return stretch;
}

void Walker::listArm(const StretchInArms& stretch, ArmsThatMeet& arms)
{
    if (stretch.isReturnedAtOnce && !isUnreachable())
        threadReturnOf(stretch.inputBlock);
    if (isUnreachable() || hasFailed())
        return;
    if (isInTrialVisitOfCurrentFrame()) {
        const BodyIndex& index = currentBodyIndex();
        uint32_t firstOfBlock = index.block(stretch.inputBlock).firstValue().index();
        for (uint32_t place = stretch.begin; place < stretch.end; ++place) {
            ValueID sum(firstOfBlock + place);
            if (index.opcodeOf(sum) == Opcode::Add && !stretch.mentionedBehind.contains(sum))
                noteTypeOfSumLeftInArm(sum);
        }
    }
    JoinArm arm { m_block, { } };
    for (ValueID mentioned : stretch.mentionedBehind) {
        Reference built = referenceOf(mentioned);
        if (!built || !built.isRecord())
            return fail("an arm has no value for what is read behind it"_s);
        arm.values.append(built);
    }
    arms.append(WTF::move(arm));
}

void Walker::walkClosedArm(BlockID closed, const StretchInArms& stretch, ArmsThatMeet& arms)
{
    continueIn(closed);
    ClosedArm closedArm(*this);
    walkStretch(stretch.inputBlock, stretch.begin, stretch.end);
    listArm(stretch, arms);
}

bool Walker::joinArms(const StretchInArms& stretch, const ArmsThatMeet& arms, BlockID meeting)
{
    uint32_t firstOfBlock = currentBodyIndex().block(stretch.inputBlock).firstValue().index();
    size_t nextMentionedBehind = 0;
    for (uint32_t place = stretch.begin; place < stretch.end; ++place) {
        bool isMentionedBehind = nextMentionedBehind < stretch.mentionedBehind.size() && stretch.mentionedBehind[nextMentionedBehind] == ValueID(firstOfBlock + place);
        nextMentionedBehind += isMentionedBehind;
        if (arms.isEmpty() || !isMentionedBehind)
            bind(ValueID(firstOfBlock + place), Reference());
    }
    Vector<Reference, 8> met(stretch.mentionedBehind.size());
    if (!joinArms(arms.span(), meeting, met.mutableSpan()))
        return false;
    for (size_t position = 0; position < met.size(); ++position)
        bind(stretch.mentionedBehind[position], met[position]);
    return true;
}

ALWAYS_INLINE_INTO_OTHER_FILES uint32_t Walker::addOpenEdge(InputBlock& target)
{
    if (target.isEntered) {
        fail("an edge leads back to a block that is no head of a loop"_s);
        return noOpenEdge;
    }

    uint32_t place = m_firstFreeOpenEdge;
    if (place != noOpenEdge)
        m_firstFreeOpenEdge = m_openEdges[place].next;
    else {
        place = m_openEdges.size();
        m_openEdges.constructAndAppend();
    }
    m_openEdges[place].next = std::exchange(target.firstOpenEdge, place);
    return place;
}

uint32_t Walker::addOpenEdge(size_t frameIndex, BlockID target)
{
    return addOpenEdge(inputBlock(frameIndex, target));
}

Vector<uint32_t, 4> Walker::takeOpenEdges(BlockID block)
{
    InputBlock& record = inputBlock(m_frames.last(), block);
    record.isEntered = true;
    Vector<uint32_t, 4> edges;
    for (uint32_t place = std::exchange(record.firstOpenEdge, noOpenEdge); place != noOpenEdge; place = m_openEdges[place].next)
        edges.append(place);
    edges.reverse();
    return edges;
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::releaseOpenEdge(uint32_t place)
{
    OpenEdge& edge = m_openEdges[place];
    edge.edge = { };
    edge.block = { };
    edge.arguments.shrink(0);
    edge.types.shrink(0);
    edge.state.clear();
    edge.names.clear();
    edge.inputBlockLeft = { };
    edge.isLedByClosedArm = false;
    edge.comesFromClosedArm = false;
    edge.isLedByClosedArmThatKeepsSites = false;
    edge.next = std::exchange(m_firstFreeOpenEdge, place);
}

BlockID Walker::addBlockFor(BlockID inputBlock, BlockRole role, BlockFrequency frequency)
{
    const Frame& frame = m_frames.last();
    BlockID block = m_code.addBlock(role, m_coldCopyDepth || frame.blockFollowsClosedArms ? BlockFrequency::Rare : frequency);
    if (!m_coldCopyDepth && (frame.loopDepthAtCall || loopDepthInCodeOf(inputBlock)))
        m_code.setLoopMarks(block, true, false);
    return block;
}

void Walker::layBlockOnEdge(EdgeID& edge, BlockID to)
{
    BlockID onEdge = m_code.addBlock(BlockRole::Ordinary, m_code.at(to).frequency);
    m_code.setLoopMarks(onEdge, m_code.at(to).isInLoop, false);
    m_code.closeEdge(edge, onEdge, { });
    m_block = onEdge;
    edge = { };
}

bool Walker::makeReadyToCross(BlockID to, std::span<const Reference> arguments, bool mayBuild, ArgumentsOfEdge& ready)
{
    std::span<const ValueID> parameters = m_code.at(to).parameters();
    if (parameters.size() != arguments.size()) {
        fail("what an edge brings does not fill the parameters of its target"_s);
        return false;
    }
    if (m_state.holdsFreshObjects()) {
        for (Reference argument : arguments)
            letLeave(argument);
    }
    holdWhatArrives(to);
    if (mayBuild && m_hasFailed && isUnreachable())
        return false;
    ready.m_heldFor = to;
    ready.m_records.shrink(0);
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (isStringInBuilder(*this, arguments[index]) && !isStringInBuilder(*this, Reference::value(parameters[index]))) {
            fail("an edge hands a text in its builder to a parameter that holds no builder"_s);
            return false;
        }
        Representation form = m_code.at(parameters[index]).representation;
        Reference argument = arguments[index];
        if (!isSameWord(representationOf(argument), form))
            argument = mayBuild ? convert(argument, form, m_origin) : convertWithoutCode(argument, form);
        if (!argument || (mayBuild && isUnreachable())) {
            if (mayBuild)
                fail("an edge hands on a value that nothing was built for"_s);
            return false;
        }
        ready.m_records.append(argument.id());
    }
    return true;
}

bool Walker::passesUnescapedObject(std::span<const Reference> arguments) const
{
    if (!m_state.holdsFreshObjects())
        return false;
    return std::ranges::any_of(arguments, [&](Reference argument) {
        if (!argument || !argument.isValue())
            return false;
        ValueID cell = underlyingCellOf(m_code, argument.id());
        return !m_code.isConstant(cell) && m_code.at(cell).opcode == Opcode::NewObject && !isEnvironmentAllocation(Reference::value(cell)) && m_state.isFresh(Reference::value(cell));
    });
}

void Walker::crossEdge(EdgeID edge, BlockID to, std::span<const Reference> arguments)
{
    ArgumentsOfEdge ready;
    if (edge && isInLoop() && m_code.at(m_code.at(edge).from).handler != edge && passesUnescapedObject(arguments))
        layBlockOnEdge(edge, to);
    if (edge) {
        if (makeReadyToCross(to, arguments, false, ready)) {
            m_code.closeEdge(edge, to, ready);
            return;
        }
        if (m_hasFailed)
            return;
        if (m_code.at(m_code.at(edge).from).handler == edge) {
            fail("what a handler edge brings is not in the form of the handler's parameter"_s);
            return;
        }
        layBlockOnEdge(edge, to);
    }
    if (!makeReadyToCross(to, arguments, true, ready))
        return;
    m_code.endWithJump(m_block, m_origin, to, ready);
    m_block = { };
}

void Walker::closeBackEdge(EdgeID edge, BlockID head, std::span<const Reference> arguments)
{
    if (!m_hasFailed && m_headsOfClosedLoops.contains(head.index())) [[unlikely]] {
        dataLogLn("AOT: the walk of f", unit().function.index(), " ", nameOf(unit().role), ": an edge comes round to b", head.index(), ", the head of a loop that is closed");
        RELEASE_ASSERT_NOT_REACHED();
    }
    noteEdgeCameRound(head);
    for (size_t place = m_trialVisits.size(); place--;) {
        TrialVisit& trialVisit = m_trialVisits[place];
        if (trialVisit.frameIndex < m_frames.size() && inputBlock(trialVisit.frameIndex, trialVisit.inputHead).head == head) {
            ++trialVisit.numberOfEdgesThatCameRound;
            noteSlotsThatComeRound(trialVisit, arguments);
            break;
        }
    }
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    Vector<Reference, 8> withPositions;
    if (parameters.size() != arguments.size()) {
        withPositions.append(arguments);
        if (auto iterations = m_iterationsOfHeads.find(head.index()); iterations != m_iterationsOfHeads.end()) {
            for (uint32_t place : iterations->value) {
                withPositions.append(m_indexedIterations[place].position);
                if (m_indexedIterations[place].table)
                    withPositions.append(m_indexedIterations[place].table);
                if (m_indexedIterations[place].count)
                    withPositions.append(countOfNamesComesRound(*this, m_indexedIterations[place]) ? m_indexedIterations[place].count : constant(Draft(Opcode::Integer, m_origin)));
            }
        }
        if (m_loopsOverGenerators.hasStepsWrittenByHand)
            appendCarriedSteps(*this, head, arguments, withPositions);
        if (!m_generatorsWalkedInConsumers.isEmpty())
            appendCarriedValuesThatComeRound(head, withPositions);
        if (!bringFieldsOfScopesRound(edge, head, withPositions))
            return;
        appendCarriedResults(head, withPositions);
        parameters = m_code.at(head).parameters();
        arguments = withPositions.span();
    }
    if (parameters.size() != arguments.size()) {
        fail("an edge that comes round does not fill the parameters of the head"_s);
        return;
    }
    Vector<Reference, 8> askedAgain;
    bool misfitsInTry = false;
    if (m_state.holdsFreshObjects()) {
        for (Reference argument : arguments)
            letLeave(argument);
    }
    holdWhatArrives(head);
    for (size_t index = 0; index < arguments.size(); ++index) {
        TypeID comingRound = ownTypeOf(arguments[index]);
        TypeID previewed = m_code.at(parameters[index]).type;
        bool needsRecheck = comingRound && needsBackEdgeRecheck(head, static_cast<uint32_t>(index), arguments[index]);
        if (comingRound && (needsRecheck || !m_types.isSubset(comingRound, previewed))) {
            TypeID heldTo = m_versionedLoops.isEmpty() ? TypeID() : backEdgeCheckType(m_versionedLoops.last().inputHead, static_cast<uint32_t>(index));
            size_t placeInRow = 0;
            const TrialVisit* trialVisit = heldTo ? tryOfHeadParameter(parameters[index], placeInRow) : nullptr;
            if (trialVisit && placeInRow < 64 && (m_loopSummaries[trialVisit->summaryIndex].sumsThatStartAtNumberThatEnters >> placeInRow & 1))
                heldTo = { };
            m_sumOfTypedBuildComesRoundUnproven |= heldTo == TypeID(TypeTable::number) && !m_types.isSubset(comingRound, TypeTable::number) && inputBlock(m_versionedLoops.last().frameIndex, m_versionedLoops.last().inputHead).head == head;
            if (!heldTo)
                heldTo = typeAskedOfWhatOtherCodeBringsRound(head, previewed, comingRound);
            Reference proven = askAgainWhereItComesRound(edge, head, heldTo, arguments.first(arguments.size() - numberOfFieldsOfScopesThatBlockTakes(head)), scopedLambda<Reference()>([&] {
                return arguments[index];
            }));
            if (proven) {
                if (askedAgain.isEmpty())
                    askedAgain.append(arguments);
                askedAgain[index] = proven;
                arguments = askedAgain.span();
                comingRound = ownTypeOf(proven);
            } else if (needsRecheck)
                comingRound = m_types.unionOf(comingRound, TypeArena::fromPrimitives(Primitive::UndefinedOrNull));
            if (m_hasFailed)
                return;
        }
        if (comingRound && !m_trialVisits.isEmpty())
            noteAllThatComesRoundInTry(head, index, comingRound, arguments[index]);
        if (!comingRound || !m_types.isSubset(comingRound, previewed)) {
            if (comingRound && noteWhatCameRoundInTry(head, index, previewed, comingRound)) {
                misfitsInTry = true;
                continue;
            }
            if (comingRound)
                dataLogLn("AOT: parameter ", index, " of the head b", head.index(), ": ", TypeInText { m_types, comingRound }, " comes round in v", arguments[index].id().index(), ", a ", nameOf(m_code.at(arguments[index].id()).opcode), ", and ", TypeInText { m_types, previewed }, " was previewed");
            fail("what comes round to the head of a loop lies outside the previewed type of its parameter"_s);
            return;
        }
    }
    if (!misfitsInTry && (!counterComesRoundNotBelowItself(head, arguments) || !loweredCountersComeRound(head, arguments) || !secondCountersComeRound(head, arguments)))
        misfitsInTry = true;
    if (m_hasFailed)
        return;
    if (misfitsInTry) {
        if (!edge)
            endAsUnreachable(m_origin);
        m_block = { };
        return;
    }
    if (holdBackEdgeOfTurnThatDidSomething(edge, head, arguments))
        return;
    crossEdge(edge, head, arguments);
    m_block = { };
}

void Walker::closeEdgesInto(BlockID block, BlockID inputBlock, std::span<const uint32_t> edges, bool isHandler)
{
    BodyBlock input = currentBodyIndex().block(inputBlock);
    uint32_t numberOfParameters = input.numberOfParameters();
    for (uint32_t place : edges) {
        if (m_openEdges[place].arguments.size() != numberOfParameters) {
            fail("the arguments and proofs of an edge do not fill the parameters of its target"_s);
            return;
        }
    }

    auto edgeOutOfBranchThatLeadsHereThroughEmptyBlocks = [&](EdgeID edge) -> EdgeID {
        while (true) {
            BlockID from = m_code.at(edge).from;
            const Block& left = m_code.at(from);
            if (left.terminator.kind == TerminatorKind::Branch)
                return edge;
            if (left.terminator.kind != TerminatorKind::Jump || m_code.lastOperationOf(from) || !left.parameters().empty() || left.isHandlerBlock() || left.isEntry || left.isLoopHead || !left.firstEdgeIn() || m_code.at(left.firstEdgeIn()).nextInto())
                return { };
            edge = left.firstEdgeIn();
        }
    };
    auto integerWhoseTruthChoosesBetweenOneAndZero = [&](uint32_t index) -> Reference {
        if (isHandler || edges.size() != 2)
            return { };
        std::array<EdgeID, 2> outOfBranch;
        std::array<int64_t, 2> brought;
        for (uint32_t way = 0; way < 2; ++way) {
            const OpenEdge& open = m_openEdges[edges[way]];
            Type type = m_types.at(open.types[index]);
            if (!type.isInteger() || type.integers.minimum != type.integers.maximum || representationOf(open.arguments[index]) != Representation::Int32)
                return { };
            brought[way] = type.integers.minimum;
            outOfBranch[way] = edgeOutOfBranchThatLeadsHereThroughEmptyBlocks(open.edge);
            if (!outOfBranch[way])
                return { };
        }
        BlockID asks = m_code.at(outOfBranch[0]).from;
        if (asks != m_code.at(outOfBranch[1]).from)
            return { };
        const Terminator& branch = m_code.at(asks).terminator;
        uint32_t taken = outOfBranch[0] == branch.taken() ? 0 : 1;
        if (outOfBranch[taken] != branch.taken() || outOfBranch[1 - taken] != branch.notTaken() || brought[taken] != 1 || brought[1 - taken])
            return { };
        const Operation& truth = m_code.at(branch.conditionOperand());
        if (truth.opcode != Opcode::ToBoolean)
            return { };
        Reference tested = Reference::value(truth.valueOperand());
        Type type = m_types.at(m_code.at(tested.id()).type);
        if (!type.isInteger() || type.integers.minimum < 0 || type.integers.maximum > 1 || representationOf(tested) != Representation::Int32)
            return { };
        return tested;
    };

    auto isNarrowing = [&](Reference name) {
        return name.isValue() && m_code.at(name.id()).opcode == Opcode::Narrowed;
    };
    auto valueBehindNarrowing = [&](Reference name) -> Reference {
        if (!isNarrowing(name) || m_code.at(name.id()).narrowsHolderOfOperand() || m_code.at(name.id()).narrowsOtherResultOfOperand())
            return { };
        return Reference::value(m_code.at(name.id()).operand(0));
    };
    auto narrowestCommonName = [&](TypeID type, const Invocable<Reference(uint32_t)> auto& nameOnWay) -> Reference {
        Reference common = nameOnWay(edges[0]);
        for (uint32_t place : edges) {
            Reference name = nameOnWay(place);
            while (common != name) {
                if (!common || !name || !common.isValue() || !name.isValue())
                    return { };
                Reference& later = common.id().index() > name.id().index() ? common : name;
                later = valueBehindNarrowing(later);
            }
        }
        if (!common || !common.isValue() || m_code.isConstant(common.id()) || !m_types.isSubset(ownTypeOf(common), type))
            return { };
        bool hasOneForm = std::ranges::all_of(edges, [&](uint32_t place) {
            return representationOf(nameOnWay(place)) == representationOf(common);
        });
        return hasOneForm ? common : Reference();
    };
    Vector<uint32_t, 8> joined;
    Vector<ValueID, 8> parameters;
    Vector<Representation, 4> forms;
    for (uint32_t index = 0; index < numberOfParameters; ++index) {
        Reference first = m_openEdges[edges[0]].arguments[index];
        bool allBringOneValue = std::ranges::all_of(edges, [&](uint32_t place) {
            return m_openEdges[place].arguments[index] == first;
        });
        if (allBringOneValue) {
            bind(input.parameter(index), first);
            continue;
        }
        if (Reference tested = integerWhoseTruthChoosesBetweenOneAndZero(index)) {
            count(StatisticID::ZeroOrOnePhisFolded);
            bind(input.parameter(index), tested);
            continue;
        }

        TypeID type = m_openEdges[edges[0]].types[index];
        forms.shrink(0);
        bool someIsNarrowing = false;
        for (uint32_t place : edges) {
            const OpenEdge& open = m_openEdges[place];
            Reference argument = open.arguments[index];
            type = joinTypes(type, open.types[index]);
            forms.append(representationOf(argument));
            someIsNarrowing |= isNarrowing(argument);
        }
        Reference common = someIsNarrowing && !isHandler ? narrowestCommonName(type, [&](uint32_t place) { return m_openEdges[place].arguments[index]; }) : Reference();
        if (common) {
            count(StatisticID::NarrowedArgumentsFoldedAtJoins);
            bind(input.parameter(index), common);
            continue;
        }
        Representation form = isHandler ? boxedFormOf(type) : formOfParameter(type, forms.span());
        parameters.append(m_code.appendParameter(block, type, form, m_origin));
        joined.append(index);
        bind(input.parameter(index), Reference::value(parameters.last()));
    }
    Frame& frame = m_frames.last();
    NamesOfWay names;
    Vector<uint32_t, 4> joinedNames;
    if (frame.duplicatedValues) {
        names = m_openEdges[edges.back()].names;
        for (size_t index = 1; index < edges.size(); ++index) {
            m_openEdges[edges[index]].names.forEachSlotNamedOtherwiseBy(m_openEdges[edges[index - 1]].names, [&](uint32_t slot) {
                if (isNamedByWaysInto(frame, frame.duplicatedValues->slots[slot].value, inputBlock))
                    joinedNames.append(slot);
            });
        }
        std::ranges::sort(joinedNames, { }, [&](uint32_t slot) {
            return frame.duplicatedValues->slots[slot].ordinal - 1;
        });
        joinedNames.shrink(std::ranges::unique(joinedNames).begin() - joinedNames.begin());
        joinedNames.removeAllMatching([&](uint32_t slot) {
            bool someIsNarrowing = std::ranges::any_of(edges, [&](uint32_t place) {
                Reference name = m_openEdges[place].names.nameIn(slot);
                return name && isNarrowing(name);
            });
            if (!someIsNarrowing)
                return false;
            TypeID type;
            for (uint32_t place : edges) {
                Reference name = m_openEdges[place].names.nameIn(slot);
                if (!name)
                    return false;
                type = type ? joinTypes(type, ownTypeOf(name)) : ownTypeOf(name);
            }
            Reference common = narrowestCommonName(type, [&](uint32_t place) { return m_openEdges[place].names.nameIn(slot); });
            if (!common)
                return false;
            count(StatisticID::NarrowedBindingsFoldedAtJoins);
            names.setNameIn(slot, common);
            frame.duplicatedValues->noteSeveralNamesIn(slot);
            frame.references.placeOf<NamesOfValues::Writer::ClosesEdgesIntoBlock>(frame.duplicatedValues->slots[slot].value.index()) = Reference();
            return true;
        });
    }
    for (uint32_t slot : joinedNames) {
        TypeID type = ownTypeOf(m_openEdges[edges[0]].names.nameIn(slot));
        forms.shrink(0);
        for (uint32_t place : edges) {
            Reference name = m_openEdges[place].names.nameIn(slot);
            if (!name) {
                fail("the edges that enter a block do not bind the same duplicated values"_s);
                return;
            }
            type = joinTypes(type, ownTypeOf(name));
            forms.append(representationOf(name));
        }
        Representation form = isHandler ? boxedFormOf(type) : formOfParameter(type, forms.span());
        if (isHandler && !isSameWord(forms[0], form) && isLegalRepresentation(m_types.at(type), forms[0]) && std::ranges::all_of(forms, [&](Representation brought) { return brought == forms[0]; }))
            form = forms[0];
        parameters.append(m_code.appendParameter(block, type, form, m_origin));
        names.setNameIn(slot, Reference::value(parameters.last()));
        frame.duplicatedValues->noteSeveralNamesIn(slot);
        frame.references.placeOf<NamesOfValues::Writer::ClosesEdgesIntoBlock>(frame.duplicatedValues->slots[slot].value.index()) = Reference();
    }
    Vector<ObjectsMetAtJoin, 1> objectsMet;
    if (!isHandler && !joined.isEmpty() && has(Mechanism::EscapeAnalysis))
        findObjectsThatMeetAt(block, inputBlock, edges, joined.span(), joinedNames.span(), parameters, objectsMet);
    Vector<uint32_t, 2> stepsMet;
    if (m_loopsOverGenerators.hasStepsWrittenByHand && !isHandler && edges.size() > 1) {
        for (uint32_t place = 0; place < joined.size() + joinedNames.size(); ++place) {
            ValueID step = place < joined.size() ? input.parameter(joined[place]) : frame.duplicatedValues->slots[joinedNames[place - joined.size()]].value;
            if (!isOnlyReadStep(*this, step))
                continue;
            auto resultOfWay = [&](uint32_t edge) {
                return place < joined.size() ? m_openEdges[edge].arguments[joined[place]] : m_openEdges[edge].names.nameIn(joinedNames[place - joined.size()]);
            };
            if (std::ranges::none_of(edges, [&](uint32_t edge) { return hasStepParts(*this, resultOfWay(edge)); }))
                continue;
            bool isRarelyAnswered = std::ranges::all_of(edges, [&](uint32_t edge) { return isUnlikelyStepResult(*this, resultOfWay(edge)); });
            addStepParameters(*this, block, Reference::value(parameters[place]), isRarelyAnswered, parameters, m_origin);
            stepsMet.append(place);
        }
    }
    Vector<FieldOfScope, 4> fieldsThatMeet;
    if (!isHandler && edges.size() > 1 && (unitHasMadeScopes() || !m_fieldsOfScopesOfHeads.isEmpty() || !m_carriedResultRequests.isEmpty() || mayMeetUnmadeRecords(edges))) {
        Vector<const WalkState*, 4> states;
        for (uint32_t place : edges)
            states.append(&m_openEdges[place].state);
        findFieldsOfScopesThatMeet(states.span(), fieldsThatMeet);
    }
    if (!isHandler && edges.size() > 1 && !parameters.isEmpty() && (m_carriesSlotsOfObjects || !m_trialVisits.isEmpty()))
        findSlotsOfObjectsThatMeet(edges, joined.span(), joinedNames.span(), parameters.span(), objectsMet.span(), fieldsThatMeet);
    for (FieldOfScope& field : fieldsThatMeet) {
        parameters.append(m_code.appendParameter(block, field.type, field.form, m_origin));
        field.parameter = Reference::value(parameters.last());
    }
    struct NamesThatMeet {
        Reference value;
        Reference parameter;
        Vector<Reference, 4> ofWays;
    };
    Vector<NamesThatMeet, 2> namesThatMeet;
    bool everyWayHoldsCheckedNames = !isHandler && edges.size() > 1 && std::ranges::all_of(edges, [&](uint32_t place) { return m_openEdges[place].state.hasCheckedNames(); });
    for (size_t other = 1; everyWayHoldsCheckedNames && other < edges.size(); ++other) {
        m_openEdges[edges[0]].state.forEachValueWithCheckedNameThatIsNotHeldBy(m_openEdges[edges[other]].state, [&](Reference value) {
            if (!value.isValue() || m_openEdges[edges[0]].state.valueNamedAnewBy(value) || namesThatMeet.containsIf([&](const NamesThatMeet& met) { return met.value == value; }))
                return;
            NamesThatMeet met;
            met.value = value;
            TypeID type;
            forms.shrink(0);
            for (uint32_t place : edges) {
                Reference name = value;
                while (Reference checked = m_openEdges[place].state.checkedNameOf(name))
                    name = checked;
                if (name == value || !name.isValue())
                    return;
                met.ofWays.append(name);
                type = type ? joinTypes(type, ownTypeOf(name)) : ownTypeOf(name);
                forms.append(representationOf(name));
            }
            if (!type || m_types.isSubset(ownTypeOf(value), type))
                return;
            parameters.append(m_code.appendParameter(block, type, formOfParameter(type, forms.span()), m_origin));
            met.parameter = Reference::value(parameters.last());
            namesThatMeet.append(WTF::move(met));
            count(StatisticID::NarrowedValuesMergedAtJoins);
        });
    }
    std::optional<MergedUndoableStores> mergedStores = isHandler ? std::nullopt : mergeUndoableStores(edges);
    if (mergedStores) {
        for (MergedUndoableStore& field : mergedStores->stores) {
            field.store.held = field.heldOnPaths[0];
            TypeID type;
            forms.shrink(0);
            for (Reference held : field.heldOnPaths) {
                type = type ? joinTypes(type, ownTypeOf(held)) : ownTypeOf(held);
                forms.append(representationOf(held));
                field.hasParameter |= held != field.store.held;
            }
            if (!field.hasParameter)
                continue;
            parameters.append(m_code.appendParameter(block, type, formOfParameter(type, forms.span()), m_origin));
            field.store.held = Reference::value(parameters.last());
        }
    }
    auto takeCommonStore = [&](WalkState& state) {
        if (mergedStores)
            state.recordUndoableStore(m_versionedLoops.last().restartPoint, mergedStores->commonStore);
    };
    if (parameters.isEmpty()) {
        RELEASE_ASSERT(m_code.at(block).parameters().empty());
        bool hasTakenState = false;
        for (uint32_t place : edges) {
            OpenEdge& open = m_openEdges[place];
            m_code.closeEdge(open.edge, block, { });
            takeCommonStore(open.state);
            if (std::exchange(hasTakenState, true))
                m_state.meetWith(open.state);
            else
                m_state = WTF::move(open.state);
            releaseOpenEdge(place);
        }
        m_block = block;
        if (mergedStores)
            recordMergedUndoableStores(*mergedStores);
        if (frame.duplicatedValues)
            takeNamesOfWay(inputBlock, WTF::move(names));
        return;
    }
    m_code.setParameters(block, parameters.span());
    Vector<std::pair<Reference, uint64_t>, 2> joinedArrayGuards;
    for (size_t place = 0; !isHandler && !m_versionedLoops.isEmpty() && edges.size() > 1 && place < joined.size() + joinedNames.size(); ++place) {
        std::optional<uint64_t> guard;
        for (uint32_t edge : edges) {
            const OpenEdge& open = m_openEdges[edge];
            std::optional<uint64_t> brought = findArrayGuard(*this, open.state, place < joined.size() ? open.arguments[joined[place]] : open.names.nameIn(joinedNames[place - joined.size()]));
            if (!brought || (guard && *guard != *brought)) {
                guard = std::nullopt;
                break;
            }
            guard = brought;
        }
        if (guard)
            joinedArrayGuards.append({ Reference::value(parameters[place]), *guard });
    }

    WalkState met;
    bool hasMet = false;
    Vector<Reference, 8> arguments;
    size_t way = 0;
    for (uint32_t place : edges) {
        EdgeID edge = m_openEdges[place].edge;
        m_state = WTF::move(m_openEdges[place].state);
        arguments.shrink(0);
        for (uint32_t index : joined)
            arguments.append(m_openEdges[place].arguments[index]);
        for (uint32_t slot : joinedNames)
            arguments.append(m_openEdges[place].names.nameIn(slot));
        for (const ObjectsMetAtJoin& met : objectsMet) {
            for (const ObjectsThatMeet::Slot& slot : met.objects.slots) {
                if (!slot.valueOnEveryWay)
                    arguments.append(slot.valuesOfWays[way]);
            }
        }
        for (uint32_t place : stepsMet)
            appendStepParts(*this, Reference(arguments[place]), arguments);
        for (const FieldOfScope& field : fieldsThatMeet)
            arguments.append(field.values[way] ? field.values[way] : readSlotOnEdge(edge, block, field, arguments[*field.positionOfHolder]));
        if (m_hasFailed || (!edge && isUnreachable()))
            return;
        for (const NamesThatMeet& met : namesThatMeet)
            arguments.append(met.ofWays[way]);
        if (mergedStores) {
            for (const MergedUndoableStore& field : mergedStores->stores) {
                if (field.hasParameter)
                    arguments.append(field.heldOnPaths[way]);
            }
        }
        ++way;
        releaseOpenEdge(place);

        crossEdge(edge, block, arguments.span());
        if (m_hasFailed)
            return;
        takeCommonStore(m_state);
        if (std::exchange(hasMet, true))
            met.meetWith(m_state);
        else
            met = WTF::move(m_state);
    }
    m_state = WTF::move(met);
    m_block = block;
    if (mergedStores)
        recordMergedUndoableStores(*mergedStores);
    for (const ObjectsMetAtJoin& objects : objectsMet) {
        Vector<Reference, 8> parametersOfSlots;
        for (ValueID parameter : parameters.span().subspan(objects.firstParameterOfSlots, objects.numberOfParametersOfSlots))
            parametersOfSlots.append(Reference::value(parameter));
        noteObjectsThatHaveMetAsOne(*this, Reference::value(parameters[objects.placeAmongParameters]), objects.objects, parametersOfSlots.span());
    }
    for (auto [joinedArray, guard] : joinedArrayGuards)
        noteJoinedArrayGuard(*this, joinedArray, guard);
    noteFieldsOfScopes(fieldsThatMeet.span());
    for (const NamesThatMeet& met : namesThatMeet) {
        if (!m_state.checkedNameOf(met.value))
            m_state.noteCheckedName(met.value, met.parameter);
    }
    if (frame.duplicatedValues)
        takeNamesOfWay(inputBlock, WTF::move(names));
}

Reference Walker::addParameterForSlotOfObjectsThatMeet(BlockID block, const ObjectsThatMeet::Slot& slot)
{
    TypeID type;
    Vector<Representation, 4> forms;
    for (Reference value : slot.valuesOfWays) {
        type = type ? joinTypes(type, ownTypeOf(value)) : ownTypeOf(value);
        forms.append(representationOf(value));
    }
    return Reference::value(m_code.appendParameter(block, type, formOfParameter(type, forms.span()), m_origin));
}

void Walker::findObjectsThatMeetAt(BlockID block, BlockID inputBlock, std::span<const uint32_t> edges, std::span<const uint32_t> joined, std::span<const uint32_t> joinedNames, Vector<ValueID, 8>& parameters, Vector<ObjectsMetAtJoin, 1>& found)
{
    const BodyIndex& index = currentBodyIndex();
    BlockID dominator = index.immediateDominator(inputBlock);
    if (!dominator || index.isLoopHead(inputBlock) || index.block(inputBlock).isHandlerBlock())
        return;
    Vector<const WalkState*, 4> ways;
    for (uint32_t place : edges) {
        if (!m_openEdges[place].state.holdsFreshObjects())
            return;
        ways.append(&m_openEdges[place].state);
    }
    uint32_t firstRecordBehindParting = this->inputBlock(m_frames.last(), dominator).numberOfRecordsBehindEnd;

    auto cellOf = [&](Reference name) {
        return name && name.isValue() ? underlyingCellOf(m_code, name.id()).index() : UINT32_MAX;
    };
    Vector<Vector<uint32_t, 8>, 4> cellsNamedOnWays;
    auto isHandedOverOnce = [&](size_t way, Reference name) {
        if (cellsNamedOnWays.isEmpty()) {
            for (uint32_t place : edges) {
                const OpenEdge& open = m_openEdges[place];
                Vector<uint32_t, 8> cells;
                for (Reference other : open.arguments)
                    cells.append(cellOf(other));
                for (uint32_t slot : joinedNames)
                    cells.append(cellOf(open.names.nameIn(slot)));
                std::ranges::sort(cells);
                cellsNamedOnWays.append(WTF::move(cells));
            }
        }
        return std::ranges::equal_range(cellsNamedOnWays[way], cellOf(name)).size() == 1;
    };
    Vector<Reference, 4> arguments;
    for (uint32_t placeAmongParameters = 0; placeAmongParameters < joined.size(); ++placeAmongParameters) {
        arguments.shrink(0);
        for (uint32_t place : edges)
            arguments.append(m_openEdges[place].arguments[joined[placeAmongParameters]]);
        std::optional<ObjectsThatMeet> objects = findObjectsThatMeetAsOne(*this, ways.span(), arguments.span(), firstRecordBehindParting);
        if (!objects)
            continue;
        bool everyObjectIsHandedOverOnce = true;
        for (size_t way = 0; way < arguments.size(); ++way)
            everyObjectIsHandedOverOnce &= isHandedOverOnce(way, arguments[way]);
        if (!everyObjectIsHandedOverOnce)
            continue;
        ObjectsMetAtJoin met { placeAmongParameters, static_cast<uint32_t>(parameters.size()), 0, WTF::move(*objects) };
        for (const ObjectsThatMeet::Slot& slot : met.objects.slots) {
            if (!slot.valueOnEveryWay)
                parameters.append(addParameterForSlotOfObjectsThatMeet(block, slot).id());
        }
        met.numberOfParametersOfSlots = static_cast<uint32_t>(parameters.size()) - met.firstParameterOfSlots;
        found.append(WTF::move(met));
    }
}

std::optional<Draft> Walker::draftOfNarrowedOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult& result, Reference holderOfSubject, Reference termOfSubject)
{
    targetIndex = m_edgeOfCodeBeingLed.value_or(targetIndex);
    Reference subject = referenceOf(result.subject);
    if (!reckonsOnWhatItHasBuilt() || !subject || !subject.isValue())
        return std::nullopt;
    Reference named = holderOfSubject ? holderOfSubject : termOfSubject ? termOfSubject : subject;
    if (!named.isValue() || m_state.isFresh(referenceTo(underlyingCellOf(m_code, named.id()))))
        return std::nullopt;

    const Terminator& end = m_code.at(from).terminator;
    ValueID root = valueBehindNamesInWalk(m_code, subject.id());
    bool goesByTest = !!result.test;
    Reference test;
    bool testIsTrue = !targetIndex;
    if (goesByTest) {
        test = referenceOf(result.test);
        if (!test || !test.isValue() || !end.isConditionalBranch())
            return std::nullopt;
        for (ValueID condition = end.conditionOperand(); condition != test.id(); condition = m_code.at(condition).valueOperand()) {
            Opcode opcode = m_code.at(condition).opcode;
            if (opcode == Opcode::LogicalNot)
                testIsTrue = !testIsTrue;
            else if (opcode != Opcode::ToBoolean && !conversionsOfForm.contains(opcode))
                return std::nullopt;
        }
        ValueID tested = valueThatTestSpeaksOf(m_code, test.id(), result.operandIndex);
        if (!tested || !root || valueBehindNamesInWalk(m_code, tested) != root)
            return std::nullopt;
    } else if (!end.hasCases() || !root || valueBehindNamesInWalk(m_code, end.subjectOperand()) != root)
        return std::nullopt;

    std::optional<Draft> narrowed(std::in_place, Opcode::Narrowed, m_origin);
    narrowed->setValueOperand(named);
    narrowed->setNarrowsHolderOfOperand(!!holderOfSubject);
    narrowed->setNarrowsTermOfOperand(!!termOfSubject);
    if (goesByTest) {
        narrowed->setTestOperand(test);
        narrowed->setOperandIndex(result.operandIndex);
        narrowed->setIsOnTakenEdge(testIsTrue);
    } else {
        narrowed->setOperandIndex(0);
        narrowed->setCaseIndex(targetIndex < end.numberOfCases() ? std::optional<uint32_t>(targetIndex) : std::nullopt);
    }

    TypeID type = resultTypeOf(*narrowed);
    if (!type || type == m_code.at(named.id()).type)
        narrowed.reset();
    return narrowed;
}

static std::optional<uint64_t> numberOfTagInWalk(const TypeArena& types, TypeID id)
{
    Type type = types.at(id);
    if (type.hasObjects() || !isSingleValue(types, type))
        return std::nullopt;
    switch (type.primitives) {
    case Primitive::LiteralString:
        return uint64_t(1) << 62 | singleStringLiteralOf(type).bits();
    case Primitive::Integer:
        if (!Range::int32().contains(type.integers.minimum))
            return std::nullopt;
        return uint64_t(2) << 62 | static_cast<uint32_t>(type.integers.minimum);
    case Primitive::Undefined:
    case Primitive::Null:
    case Primitive::False:
    case Primitive::True:
        return uint64_t(3) << 62 | type.primitives;
    default:
        return std::nullopt;
    }
}

const Walker::MembersByTag* Walker::findMembersByTag(LayoutSetID layouts, const Operation& read)
{
    if (read.opcode != Opcode::GetSlot || testsItsOwnGuess(read))
        return nullptr;
    PropertyKeyID key = read.key();
    auto known = m_membersByTagOfSets.add(static_cast<uint64_t>(layouts.bits()) << 32 | key.bits(), 0);
    if (!known.isNewEntry)
        return known.iterator->value ? m_listsOfMembersByTag[known.iterator->value - 1].get() : nullptr;

    TypingContext context = typing();
    auto list = makeUnique<MembersByTag>();
    Vector<LayoutID, 16> members;
    members.append(membersOf(m_types, layouts));
    for (LayoutID member : members) {
        std::optional<FieldReference> reference = findField(program(), member, key);
        if (!reference || reference->layout != member || fieldAt(program(), *reference).form.contains(FieldForm::IsAccessor))
            return nullptr;
        TypeID arriving = context.positions.readOfSlot(context.level, reference->layout, reference->field);
        if (arriving && isNeverType(arriving))
            continue;
        std::optional<uint64_t> tag = arriving ? numberOfTagInWalk(m_types, arriving) : std::nullopt;
        if (!tag)
            return nullptr;
        list->members.add(*tag, Vector<LayoutID, 1> { }).iterator->value.append(member);
    }
    m_listsOfMembersByTag.append(WTF::move(list));
    m_membersByTagOfSets.set(static_cast<uint64_t>(layouts.bits()) << 32 | key.bits(), static_cast<uint32_t>(m_listsOfMembersByTag.size()));
    return m_listsOfMembersByTag.last().get();
}

void Walker::shareMembersByTag(LayoutSetID whole, LayoutSetID part, PropertyKeyID key)
{
    if (!whole || !part || whole == part)
        return;
    auto known = m_membersByTagOfSets.find(static_cast<uint64_t>(whole.bits()) << 32 | key.bits());
    if (known == m_membersByTagOfSets.end())
        return;
    uint32_t place = known->value;
    m_membersByTagOfSets.add(static_cast<uint64_t>(part.bits()) << 32 | key.bits(), place);
}

Vector<LayoutID, 4> Walker::membersThatHoldTag(const MembersByTag& list, LayoutSetID layouts, uint64_t tag)
{
    Vector<LayoutID, 4> holders;
    auto listed = list.members.find(tag);
    if (listed == list.members.end())
        return holders;
    std::span<const LayoutID> members = membersOf(m_types, layouts);
    for (LayoutID member : listed->value) {
        if (std::ranges::binary_search(members, member))
            holders.append(member);
    }
    return holders;
}

void Walker::checkThatTagAnswers(Reference tag, Reference asked, Reference answer, Origin origin)
{
    if (!checksProofs() || isUnreachable() || hasFailed())
        return;
    origin.isTailCall = false;
    auto isSame = [&](Reference left, Reference right) -> Reference {
        if (!left || !right || isUnreachable())
            return { };
        Draft comparison(Opcode::StrictEq, origin);
        chooseConservativeFields(comparison);
        comparison.setLeftOperand(left);
        comparison.setRightOperand(right);
        return buildTestOfCheck(WTF::move(comparison));
    };
    Reference holds = isSame(isSame(tag, asked), answer);
    if (holds && holds.isValue() && !isUnreachable())
        checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::Field, origin);
}

void Walker::checkThatTagLeadsWhereShapeLeads(const BodyTerminator& overTags, uint32_t targetIndex)
{
    TerminatorDraft asWritten(overTags.kind(), m_origin);
    copyFields(overTags, asWritten, [&](ValueID operand) {
        return referenceOf(operand);
    });
    bool comparesTexts = overTags.kind() == TerminatorKind::SwitchString;
    bool isCase = targetIndex < overTags.numberOfCases();
    Draft led(Opcode::Boolean, m_origin);
    led.setBoolean(isCase);
    Reference answer = constant(WTF::move(led));
    for (uint32_t caseIndex = isCase ? targetIndex : 0; caseIndex < (isCase ? targetIndex + 1 : overTags.numberOfCases()); ++caseIndex) {
        Draft written(comparesTexts ? Opcode::String : Opcode::Integer, m_origin);
        if (comparesTexts)
            written.setString(asWritten.strings()[caseIndex]);
        else
            written.setInteger(asWritten.cases()[caseIndex]);
        checkThatTagAnswers(asWritten.subjectOperand(), constant(WTF::move(written)), answer, m_origin);
    }
}

Reference Walker::buildTestOfTagAsTestOfShape(const BodyValue& comparison)
{
    if (!has(Mechanism::TypedOperations))
        return { };
    for (bool readIsLeft : { true, false }) {
        Reference read = referenceOf(comparison.operand(readIsLeft ? 0 : 1));
        Reference other = referenceOf(comparison.operand(readIsLeft ? 1 : 0));
        ValueID slotRead = read && read.isValue() && other ? slotReadBehindNamesInWalk(m_code, read.id()) : ValueID();
        if (!slotRead)
            continue;
        const Operation& get = m_code.at(slotRead);
        TypeID typeOfHolder = m_code.at(get.objectOperand()).type;
        TypeID typeOfOther = ownTypeOf(other);
        std::optional<uint64_t> tag = typeOfOther ? numberOfTagInWalk(m_types, typeOfOther) : std::nullopt;
        if (!tag || !typeOfHolder || !m_types.at(typeOfHolder).hasOnlyLayouts())
            continue;
        LayoutSetID layouts = m_types.at(typeOfHolder).layouts;
        const MembersByTag* list = findMembersByTag(layouts, get);
        if (!list)
            continue;
        Vector<LayoutID, 4> holders = membersThatHoldTag(*list, layouts, *tag);
        if (holders.isEmpty() || holders.size() == membersOf(m_types, layouts).size())
            continue;
        ValueID holder = get.objectOperand();
        Draft test(Opcode::ShapeTest, originOf(comparison));
        test.setObjectOperand(Reference::value(holder));
        test.setLayouts(m_types.layoutSets().ofSorted(holders.span()));
        Reference byShape = build(WTF::move(test));
        checkThatTagAnswers(read, other, byShape, originOf(comparison));
        return byShape;
    }
    return { };
}

static std::optional<bool> truthOfTagInWalk(uint64_t tag)
{
    switch (tag >> 62) {
    case 2:
        return !!static_cast<uint32_t>(tag);
    case 3:
        return static_cast<uint16_t>(tag) == Primitive::True;
    default:
        return std::nullopt;
    }
}

Reference Walker::buildTestOfTruthOfTagAsTestOfShape(const BodyValue& truth)
{
    if (!has(Mechanism::TypedOperations))
        return { };
    Reference read = referenceOf(truth.operand(0));
    ValueID slotRead = read && read.isValue() ? slotReadBehindNamesInWalk(m_code, read.id()) : ValueID();
    if (!slotRead)
        return { };
    const Operation& get = m_code.at(slotRead);
    ValueID holder = get.objectOperand();
    TypeID typeOfHolder = m_code.at(holder).type;
    if (!typeOfHolder || !m_types.at(typeOfHolder).hasOnlyLayouts())
        return { };
    LayoutSetID layouts = m_types.at(typeOfHolder).layouts;
    const MembersByTag* list = findMembersByTag(layouts, get);
    if (!list)
        return { };
    Vector<LayoutID, 4> holders;
    for (auto& entry : list->members) {
        std::optional<bool> isTrue = truthOfTagInWalk(entry.key);
        if (!isTrue)
            return { };
        if (*isTrue)
            holders.appendVector(membersThatHoldTag(*list, layouts, entry.key));
    }
    if (holders.isEmpty() || holders.size() == membersOf(m_types, layouts).size())
        return { };
    std::ranges::sort(holders, { }, &LayoutID::index);
    Draft test(Opcode::ShapeTest, originOf(truth));
    test.setObjectOperand(Reference::value(holder));
    test.setLayouts(m_types.layoutSets().ofSorted(holders.span()));
    Reference byShape = build(WTF::move(test));
    Draft isTrue(Opcode::Boolean, originOf(truth));
    isTrue.setBoolean(true);
    checkThatTagAnswers(referenceOf(truth.id()), constant(WTF::move(isTrue)), byShape, originOf(truth));
    return byShape;
}

std::optional<TerminatorDraft> Walker::draftOfSwitchOverShapes(const TerminatorDraft& overTags, Vector<uint32_t, 16>& casesLeft)
{
    if (!has(Mechanism::TypedOperations) || (overTags.kind != TerminatorKind::Switch && overTags.kind != TerminatorKind::SwitchString))
        return std::nullopt;
    Reference subject = overTags.subjectOperand();
    ValueID slotRead = subject && subject.isValue() ? slotReadBehindNamesInWalk(m_code, subject.id()) : ValueID();
    if (!slotRead)
        return std::nullopt;
    const Operation& get = m_code.at(slotRead);
    ValueID holder = get.objectOperand();
    TypeID typeOfHolder = m_code.at(holder).type;
    if (!typeOfHolder || !m_types.at(typeOfHolder).hasOnlyLayouts())
        return std::nullopt;
    LayoutSetID layouts = m_types.at(typeOfHolder).layouts;
    const MembersByTag* list = findMembersByTag(layouts, get);
    if (!list)
        return std::nullopt;

    bool comparesTexts = overTags.kind == TerminatorKind::SwitchString;
    uint32_t numberOfCases = static_cast<uint32_t>(comparesTexts ? overTags.strings().size() : overTags.cases().size());
    Vector<LayoutSetID, 16> cases;
    for (uint32_t caseIndex = 0; caseIndex < numberOfCases; ++caseIndex) {
        if (!comparesTexts && !Range::int32().contains(overTags.cases()[caseIndex]))
            continue;
        uint64_t tag = comparesTexts
            ? uint64_t(1) << 62 | overTags.strings()[caseIndex].bits()
            : uint64_t(2) << 62 | static_cast<uint32_t>(overTags.cases()[caseIndex]);
        Vector<LayoutID, 4> holders = membersThatHoldTag(*list, layouts, tag);
        if (holders.isEmpty())
            continue;
        cases.append(m_types.layoutSets().ofSorted(holders.span()));
        casesLeft.append(caseIndex);
    }
    if (cases.isEmpty())
        return std::nullopt;
    TerminatorDraft draft(TerminatorKind::SwitchOnShape, m_origin);
    draft.setSubjectOperand(Reference::value(holder));
    draft.setLayoutSets(cases.span());
    return draft;
}

bool Walker::endBlockWithSwitchOverShapes(const BodyTerminator& terminator)
{
    if (terminator.kind() != TerminatorKind::Switch && terminator.kind() != TerminatorKind::SwitchString)
        return false;
    TerminatorDraft asWritten(terminator.kind(), m_origin);
    copyFields(terminator, asWritten, [&](ValueID operand) {
        return referenceOf(operand);
    });
    Vector<uint32_t, 16> casesLeft;
    std::optional<TerminatorDraft> draft = draftOfSwitchOverShapes(asWritten, casesLeft);
    if (!draft)
        return false;
    Vector<EdgeToTargetOfBody, 16> edges;
    for (uint32_t caseIndex : casesLeft)
        edges.append(EdgeToTargetOfBody { terminator.block(), caseIndex });
    edges.append(EdgeToTargetOfBody { terminator.block(), terminator.numberOfCases() });
    endBlockLeadingTo(WTF::move(*draft), edges.span());
    return true;
}

bool Walker::isAskedByBranchOfItsBlock(const BodyValue& comparison) const
{
    const BodyIndex& index = currentBodyIndex();
    BodyTerminator end = index.terminator(inputBlockOf(comparison.id()));
    return end.kind() == TerminatorKind::Branch && end.operand(0) == comparison.id();
}

void Walker::askShapeInPlaceOfTagAtBranch(const BodyTerminator& terminator)
{
    if (terminator.kind() != TerminatorKind::Branch)
        return;
    const BodyIndex& index = currentBodyIndex();
    ValueID condition = terminator.operand(0);
    if (!condition || index.isParameter(condition) || (index.opcodeOf(condition) != Opcode::StrictEq && index.opcodeOf(condition) != Opcode::ToBoolean) || !index.isInBlock(condition, terminator.block()))
        return;
    Reference built = referenceOf(condition);
    if (!built || !built.isValue())
        return;
    if (Reference test = index.opcodeOf(condition) == Opcode::StrictEq ? buildTestOfTagAsTestOfShape(index.value(condition)) : buildTestOfTruthOfTagAsTestOfShape(index.value(condition)))
        bind(condition, test);
}

bool Walker::isReadOfSlotOffHolder(Reference read, Reference holder, PropertyKeyID key) const
{
    ValueID slotRead = read && read.isValue() && holder.isValue() ? slotReadBehindNamesInWalk(m_code, read.id()) : ValueID();
    return slotRead && m_code.at(slotRead).key() == key && valueBehindNamesInWalk(m_code, m_code.at(slotRead).objectOperand()) == valueBehindNamesInWalk(m_code, holder.id());
}

bool Walker::isNameOfCellAskedForShape(Reference name, ValueID asked) const
{
    if (!name || !name.isValue() || !asked)
        return false;
    const Operation& named = m_code.at(name.id());
    if (named.opcode == Opcode::Narrowed && !named.narrowsHolderOfOperand() && named.testOperand() && named.operandIndex() == OperandIndex::ShapeTest::object) {
        const Operation& test = m_code.at(named.testOperand());
        if (test.opcode == Opcode::ShapeTest && test.objectOperand() == asked)
            return true;
    }
    ValueID root = valueBehindNamesInWalk(m_code, name.id());
    return root && root == valueBehindNamesInWalk(m_code, asked);
}

std::optional<Draft> Walker::draftOfHolderNarrowedByShape(BlockID from, uint32_t targetIndex, Reference holder, Reference test)
{
    targetIndex = m_edgeOfCodeBeingLed.value_or(targetIndex);
    const Terminator& end = m_code.at(from).terminator;
    ValueID asked = end.hasCases() ? end.subjectOperand() : m_code.at(test.id()).objectOperand();
    if (!has(Mechanism::TypedOperations) || !isNameOfCellAskedForShape(holder, asked))
        return std::nullopt;
    if (m_state.isFresh(referenceTo(underlyingCellOf(m_code, asked))))
        return std::nullopt;

    Draft narrowed(Opcode::Narrowed, m_origin);
    narrowed.setValueOperand(holder);
    narrowed.setNarrowsHolderOfOperand(false);
    if (end.hasCases()) {
        narrowed.setOperandIndex(0);
        narrowed.setCaseIndex(targetIndex < end.numberOfCases() ? std::optional<uint32_t>(targetIndex) : std::nullopt);
    } else {
        if (!end.isConditionalBranch())
            return std::nullopt;
        bool testIsTrue = !targetIndex;
        for (ValueID condition = end.conditionOperand(); condition != test.id(); condition = m_code.at(condition).valueOperand()) {
            Opcode opcode = m_code.at(condition).opcode;
            if (opcode == Opcode::LogicalNot)
                testIsTrue = !testIsTrue;
            else if (opcode != Opcode::ToBoolean && !conversionsOfForm.contains(opcode))
                return std::nullopt;
        }
        narrowed.setTestOperand(test);
        narrowed.setOperandIndex(OperandIndex::ShapeTest::object);
        narrowed.setIsOnTakenEdge(testIsTrue);
    }
    TypeID type = resultTypeOf(narrowed);
    if (!type || type == m_code.at(holder.id()).type)
        return std::nullopt;
    return narrowed;
}

bool Walker::narrowHolderOfTestedFieldOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult& result, BlockID to)
{
    const BodyIndex& index = *m_frames.last().index;
    for (BodyValue first : index.block(to).values()) {
        if (first.opcode() != Opcode::NarrowByField)
            return true;
        auto written = first.as<Opcode::NarrowByField>();
        if (written.readOperand() != result.parameter)
            continue;
        Reference holder = referenceOf(written.valueOperand());
        if (!holder || !holder.isValue())
            return true;
        Reference test = result.test ? referenceOf(result.test) : Reference();
        bool shapeWasAsked = m_code.at(from).terminator.kind == TerminatorKind::SwitchOnShape || (test && test.isValue() && m_code.at(test.id()).opcode == Opcode::ShapeTest);
        std::optional<Draft> draft;
        if (shapeWasAsked)
            draft = draftOfHolderNarrowedByShape(from, targetIndex, holder, test);
        else if (checksProofs()) {
            Reference read = referenceOf(result.subject);
            ValueID slotRead = read && read.isValue() ? slotReadBehindNamesInWalk(m_code, read.id()) : ValueID();
            TypeID typeOfObject = slotRead ? m_code.at(m_code.at(slotRead).objectOperand()).type : TypeID();
            if (typeOfObject && m_types.at(typeOfObject).hasOnlyLayouts() && isReadOfSlotOffHolder(read, holder, written.key()) && findMembersByTag(m_types.at(typeOfObject).layouts, m_code.at(slotRead)))
                draft = draftOfNarrowedOnEdge(from, targetIndex, result, holder);
        }
        if (!draft)
            return true;
        TypeID inFront = ownTypeOf(holder);
        Reference narrowed = build(WTF::move(*draft));
        if (isUnreachable())
            return false;
        if (checksProofs())
            checkProof(narrowed, TypeWithoutProof::restingOnBelief(inFront), ProofSource::Field, m_origin);
        if (TypeID left = ownTypeOf(narrowed); left && m_types.at(left).hasOnlyLayouts())
            shareMembersByTag(m_types.at(m_types.withoutNullish(inFront)).layouts, m_types.at(left).layouts, written.key());
        size_t frameIndex = m_frames.size() - 1;
        uint32_t& firstName = inputBlock(frameIndex, to).firstNarrowerName;
        if (firstName == narrowerNamesOfDominator)
            firstName = m_frames[frameIndex].firstNarrowerName;
        m_frames[frameIndex].addNarrowerName(firstName, holder, narrowed);
        if (Reference bound = referenceBoundTo(written.valueOperand()); bound != holder)
            m_frames[frameIndex].addNarrowerName(firstName, bound, narrowed);
        return !isUnreachable();
    }
    return true;
}

ALWAYS_INLINE_INTO_OTHER_FILES Reference Walker::tagReadInFrontOfTest(const BodyValue& read)
{
    if (!has(Mechanism::TypedOperations))
        return { };
    const BodyIndex& index = currentBodyIndex();
    auto written = read.as<Opcode::PropGet>();
    ValueID object = written.objectOperand();
    if (!object || index.isParameter(object) || index.opcodeOf(object) != Opcode::NarrowByField)
        return { };
    auto name = index.value(object).as<Opcode::NarrowByField>();
    if (name.key() != written.key())
        return { };
    Reference earlier = referenceOf(name.readOperand());
    ValueID slotRead = earlier && earlier.isValue() ? slotReadBehindNamesInWalk(m_code, earlier.id()) : ValueID();
    if (!slotRead || m_code.at(slotRead).key() != written.key() || !isNameOfCellAskedForShape(referenceOf(object), m_code.at(slotRead).objectOperand()))
        return { };
    TypeID typeOfHolder = m_code.at(m_code.at(slotRead).objectOperand()).type;
    if (!typeOfHolder || !m_types.at(typeOfHolder).hasOnlyLayouts() || !findMembersByTag(m_types.at(typeOfHolder).layouts, m_code.at(slotRead)))
        return { };
    return earlier;
}

static ALWAYS_INLINE ValueID valueWhoseTruthIsToldInWalk(const Code& code, ValueID value, bool& isNegated)
{
    while ((value = valueBehindNamesInWalk(code, value))) {
        Opcode opcode = code.at(value).opcode;
        if (opcode == Opcode::LogicalNot)
            isNegated = !isNegated;
        else if (opcode != Opcode::ToBoolean)
            break;
        value = code.at(value).valueOperand();
    }
    return value;
}

ALWAYS_INLINE_INTO_OTHER_FILES Reference Walker::booleanAskedByBranchOnEdge(BlockID from, uint32_t targetIndex, Reference value)
{
    if (!has(Mechanism::Folding) || !value || !value.isValue())
        return { };
    const Terminator& end = m_code.at(from).terminator;
    TypeID type = typeOf(value);
    if (end.kind != TerminatorKind::Branch || !type || !isBooleanType(type))
        return { };

    bool isNegated = false;
    ValueID condition = end.conditionOperand();
    ValueID asked = valueWhoseTruthIsToldInWalk(m_code, condition, isNegated);
    if (!asked)
        return { };
    if (condition == value.id())
        isNegated = false;
    else if (asked != valueWhoseTruthIsToldInWalk(m_code, value.id(), isNegated))
        return { };
    bool conditionIsTrue = !m_edgeOfCodeBeingLed.value_or(targetIndex);
    static_assert(constantsInFormOfUses.contains(Opcode::Boolean));
    if (m_hasFailed)
        return { };
    return booleanOfUnit(conditionIsTrue != isNegated);
}

Reference Walker::booleanOfUnit(bool value)
{
    Reference& shared = m_frames.first().booleansOfUnit[value];
    if (!shared) {
        Draft answer(Opcode::Boolean);
        answer.setBoolean(value);
        shared = constant(WTF::move(answer));
    }
    return shared;
}

static ALWAYS_INLINE bool isValueProvenBooleanInWalk(const Walker& walker, Reference value)
{
    if (!value || !value.isValue())
        return false;
    TypeID type = walker.typeOf(value);
    return type && isBooleanType(type);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::nameBooleansAskedByBranch(BlockID from, const BodyTerminator& terminator, uint32_t targetIndex, BlockID to)
{
    if (!has(Mechanism::Folding) || terminator.kind() != TerminatorKind::Branch)
        return;
    const BodyIndex& index = *m_frames.last().index;
    for (ValueID asked = terminator.as<TerminatorKind::Branch>().conditionOperand();;) {
        if (!index.hasOneUse(asked)) {
            Reference value = referenceOf(asked);
            Reference answer = isValueProvenBooleanInWalk(*this, value) ? booleanAskedByBranchOnEdge(from, targetIndex, value) : Reference();
            if (answer)
                noteNarrowerName(to, value, answer);
        }
        BodyValue written = index.value(asked);
        Opcode opcode = written.opcode();
        if (opcode != Opcode::ToBoolean && opcode != Opcode::LogicalNot && !inputIdentityOpcodes.contains(opcode))
            return;
        asked = written.operand(0);
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES Reference Walker::narrowOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult& result)
{
    Reference subject = referenceOf(result.subject);
    if (!checksProofs() && m_frames.last().index->isUnused(result.parameter))
        return subject;
    if (isValueProvenBooleanInWalk(*this, subject)) {
        if (Reference answer = booleanAskedByBranchOnEdge(from, targetIndex, subject))
            return answer;
    }
    std::optional<Draft> narrowed = draftOfNarrowedOnEdge(from, targetIndex, result);
    return narrowed ? build(WTF::move(*narrowed)) : subject;
}

void Walker::narrowTestedConversion(Reference subject, Reference name)
{
    if (!subject.isValue() || !name.isValue() || name == subject || m_hasFailed || isUnreachable())
        return;
    const Operation& record = m_code.at(name.id());
    if (record.opcode != Opcode::Narrowed || record.valueOperand() != subject.id() || !record.testOperand() || record.narrowsHolderOfOperand() || record.narrowsOtherResultOfOperand() || record.narrowsTermOfOperand())
        return;
    const Operation& test = m_code.at(record.testOperand());
    if (record.operandIndex() >= test.numberOfOperands())
        return;
    ValueID tested = test.operand(record.operandIndex());
    if (!tested || m_code.isConstant(tested) || !conversionsOfForm.contains(m_code.at(tested).opcode) || m_code.at(tested).operand(0) != subject.id())
        return;
    TypeID type = record.type;
    Draft narrowed(Opcode::Narrowed, record.origin());
    narrowed.setValueOperand(Reference::value(tested));
    narrowed.setTestOperand(Reference::value(record.testOperand()));
    narrowed.setOperandIndex(record.operandIndex());
    narrowed.setIsOnTakenEdge(record.isOnTakenEdge());
    if (resultTypeOf(narrowed) != type)
        return;
    Reference converted = build(WTF::move(narrowed));
    if (converted && converted.isValue() && representationOf(converted) == m_code.at(tested).representation)
        noteConversionMade(*this, name, representationOf(converted), converted);
}

void Walker::noteNarrowerName(BlockID to, Reference value, Reference name)
{
    size_t frameIndex = m_frames.size() - 1;
    uint32_t& first = inputBlock(frameIndex, to).firstNarrowerName;
    Frame& frame = m_frames[frameIndex];
    if (first == narrowerNamesOfDominator)
        first = frame.firstNarrowerName;

    Vector<Reference, 4> values { value };
    frame.forEachNarrowerName<Frame::NarrowerNamesAsked::UnderName>(value, [&](const Frame::NarrowerName& narrower) {
        if (!frame.listOfNarrowerNamesIsLong || !frame.isNarrowerNameInLongList(narrower.value))
            values.append(narrower.value);
        return IterationStatus::Continue;
    });
    for (Reference named : values)
        frame.addNarrowerName(first, named, name);
    if (!frameIndex || !value.isValue() || !name.isValue() || value.id().index() >= frame.numberOfValuesBuiltBefore || m_code.at(name.id()).opcode != Opcode::Narrowed || m_state.checkedNameOf(value))
        return;
    const Operation& given = m_code.at(value.id());
    bool isNameThatEndedCopyProved = given.opcode == Opcode::Narrowed;
    for (size_t around = 0; around < frameIndex && isNameThatEndedCopyProved; ++around)
        isNameThatEndedCopyProved = m_frames[around].inlineFrame != given.inlineFrame;
    if (isNameThatEndedCopyProved)
        return;
    Reference named = value;
    for (ValueID at = value.id(); at;) {
        if (Reference earlier = m_state.valueNamedAnewBy(Reference::value(at))) {
            if (representationOf(earlier) == representationOf(name))
                named = earlier;
            break;
        }
        ValueID source = m_code.at(at).opcode == Opcode::Narrowed ? m_code.at(at).valueOperand() : ValueID();
        at = source && source.index() < at.index() ? source : ValueID();
    }
    m_state.nameAnew(named, name);
}

OperatorForm operatorFormFor(Walker&, const Draft&);

static constexpr OpcodeSet stepsRedefinedAfterTests { Opcode::Increment, Opcode::Decrement };

void Walker::redefineStepsAfterTest(ValueID test, Reference tested, Reference narrowed, BlockID to, Vector<Reference, 8>& arguments)
{
    const BodyIndex& index = currentBodyIndex();
    for (BodyValue step : index.block(index.blockOf(test)).values()) {
        if (step.id() == test)
            return;
        if (!stepsRedefinedAfterTests.contains(step.opcode()) || referenceOf(step.operand(0)) != tested)
            continue;
        Reference stepped = referenceOf(step.id());
        if (!stepped || !stepped.isValue() || !m_types.at(ownTypeOf(stepped)).isInteger())
            continue;
        Draft again(step.opcode(), m_origin);
        again.setOperand(0, narrowed);
        chooseConservativeFields(again);
        again.setForm(operatorFormFor(*this, again));
        Reference restated = again.form() == OperatorForm::Integers ? build(WTF::move(again)) : Reference();
        if (!restated)
            continue;
        for (Reference& argument : arguments) {
            if (argument == stepped)
                argument = restated;
        }
        noteNarrowerName(to, stepped, restated);
        count(StatisticID::StepsRedefinedAfterTests);
    }
}

static bool isReadAgainInBlock(const BodyIndex& index, const BodyEdgeResult& result, BlockID block)
{
    static constexpr OpcodeSet testsOfKinds { Opcode::TypeTest, Opcode::ToBoolean };
    if (!result.test || index.isParameter(result.test) || !testsOfKinds.contains(index.opcodeOf(result.test)))
        return false;
    ValueID read = index.stripCasts(result.subject);
    if (index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet)
        return false;
    auto written = index.value(read).as<Opcode::PropGet>();
    if (written.isFreeName())
        return false;
    ValueID object = index.stripCasts(written.objectOperand());
    return std::ranges::any_of(index.block(block).values(), [&](BodyValue other) {
        return other.opcode() == Opcode::PropGet && other.key() == written.key() && index.stripCasts(other.objectOperand()) == object;
    });
}

static bool isReceiverInBlock(const BodyIndex& index, const BodyEdgeResult& result, BlockID block)
{
    static constexpr OpcodeSet readsAndStores { Opcode::PropGet, Opcode::PropSet, Opcode::ElemGet, Opcode::ElemSet };
    if (!result.test || index.isParameter(result.test) || index.opcodeOf(result.test) != Opcode::ToBoolean)
        return false;
    ValueID tested = index.stripCasts(result.subject);
    return std::ranges::any_of(index.block(block).values(), [&](BodyValue access) {
        return readsAndStores.contains(access.opcode()) && access.objectOperand() && index.stripCasts(access.objectOperand()) == tested;
    });
}

static bool standUnderSameHandler(const BodyIndex& index, BlockID one, BlockID other)
{
    BodyBlock first = index.block(one);
    BodyBlock second = index.block(other);
    if (first.hasHandler() != second.hasHandler())
        return false;
    return !first.hasHandler() || (first.handler().block() == second.handler().block() && std::ranges::equal(first.handler().arguments(), second.handler().arguments()));
}

static NEVER_INLINE bool entersLoopAtHead(const BodyIndex& index, BlockID from, BlockID to)
{
    return index.isLoopHead(from) && index.isInLoop(to, index.loopOf(from));
}

void Walker::leadEdge(EdgeID edge, BlockID from, const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target)
{
    size_t frameIndex = m_frames.size() - 1;
    if (const Terminator& end = m_code.at(from).terminator; end.kind != TerminatorKind::Jump) {
        uint32_t number = edge.index() - end.firstEdge.index();
        recordEdgeOutcome(*this, from, number);
        if (m_negatedTest)
            m_state.answers().noteOutcome(m_negatedTest, !!number);
    }
    Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    BlockID to = target.block();
    InputBlock& entered = inputBlock(frame, to);
    BlockID head = entered.head;
    uint32_t numberOfProofs = static_cast<uint32_t>(target.proofs().size());
    bool isOnlyEntry = !head && index.numberOfIncomingEdges(to) == 1 && !index.isLoopHead(to);
    bool entersAlone = isOnlyEntry && ((frame.loopsBeingVersioned.isEmpty() && m_reversionableLoops.isEmpty()) || !exitsLoopBeingVersioned(frameIndex, to))
        && (frame.blocksBeingTailDuplicated.isEmpty() || !isTailDuplicatingBlock(frameIndex));

    Vector<Reference, 8> arguments;
    for (ValueID argument : target.arguments())
        arguments.append(referenceOf(argument));

    m_block = { };
    if (entersAlone || numberOfProofs) {
        m_block = m_code.addBlockBehindEdge(edge, m_coldCopyDepth ? BlockFrequency::Rare : BlockFrequency::Normal, !m_coldCopyDepth && (frame.loopDepthAtCall || loopDepthInCodeOf(to)));
        edge = { };
        bool namesHolderAnew = false;
        if (entersAlone && numberOfProofs) {
            BodyBlock blockEntered = index.block(to);
            namesHolderAnew = blockEntered.numberOfValues() && index.opcodeOf(blockEntered.firstValue()) == Opcode::NarrowByField;
        }
        for (uint32_t proofIndex = 0; proofIndex < numberOfProofs; ++proofIndex) {
            std::optional<BodyEdgeResult> result = index.edgeResult(terminator, targetIndex, target, proofIndex);
            if (!result) {
                fail("an edge hands on a proof that its terminator does not give"_s);
                return;
            }
            Reference subject = referenceOf(result->subject);
            bool isReadAgain = has(Mechanism::Folding) && (isReadAgainInBlock(index, *result, to) || (subject && !types().isUninformative(typeOf(subject)) && types().at(typeOf(subject)).primitives & Primitive::UndefinedOrNull && isReceiverInBlock(index, *result, to)));
            bool isRead = checksProofs() || isReadAgain || !index.isUnused(result->parameter);
            if (isReadAgain) {
                std::optional<Draft> draft = draftOfNarrowedOnEdge(from, targetIndex, *result);
                Reference narrowed = draft ? build(WTF::move(*draft)) : Reference();
                if (narrowed && entersAlone)
                    noteNarrowerName(to, subject, narrowed);
                else if (narrowed && !m_state.checkedNameOf(subject))
                    m_state.noteCheckedName(subject, narrowed);
                arguments.append(narrowed ? narrowed : subject);
            } else
                arguments.append(isRead ? narrowOnEdge(from, targetIndex, *result) : subject);
            if (((isRead || namesHolderAnew) && isUnreachable()) || (namesHolderAnew && !narrowHolderOfTestedFieldOnEdge(from, targetIndex, *result, to))) {
                m_block = { };
                return;
            }
        }
    }
    if (checksProofs() && m_code.at(from).terminator.kind == TerminatorKind::SwitchOnShape) {
        if (!m_block) {
            m_block = addBlockFor(to);
            m_code.closeEdge(edge, m_block, { });
            edge = { };
        }
        checkThatTagLeadsWhereShapeLeads(terminator, targetIndex);
        if (isUnreachable()) {
            m_block = { };
            return;
        }
    }
    ValueID condition = terminator.isConditionalBranch() ? terminator.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    bool asksRelation = condition && !index.isParameter(condition) && provesOf(index.opcodeOf(condition)) == Proof::IntegerBound;
    asksRelation = asksRelation && !(m_comparesElementsOfViews && isInLoop() && relationBoundsNothing(index, index.value(condition)));
    bool restatesSteps = loopDepthInCodeOf(to) && index.holdsAnyOf(stepsRedefinedAfterTests);
    Vector<std::pair<Reference, Reference>, 2> testedNumbers;
    if (std::optional<std::array<BodyEdgeResult, 2>> bounds = asksRelation ? boundsProvenOnEdge(index, terminator, targetIndex) : std::nullopt) {
        for (const BodyEdgeResult& bound : *bounds) {
            Reference operand = referenceOf(bound.subject);
            if (!entersAlone && !arguments.contains(operand))
                continue;
            std::optional<Draft> draft = draftOfNarrowedOnEdge(from, targetIndex, bound);
            if (!draft)
                continue;
            if (!m_block) {
                m_block = addBlockFor(to);
                m_code.closeEdge(edge, m_block, { });
                edge = { };
            }
            Reference narrowed = build(WTF::move(*draft));
            if (isUnreachable()) {
                m_block = { };
                return;
            }
            for (Reference& argument : arguments) {
                if (argument == operand)
                    argument = narrowed;
            }
            if (entersAlone) {
                noteNarrowerName(to, operand, narrowed);
                renameCarriedFields(operand, narrowed);
            }
            if (entersAlone)
                narrowTestedConversion(operand, narrowed);
            if (entersAlone && restatesSteps)
                testedNumbers.append({ operand, narrowed });
        }
        for (const BodyEdgeResult& bound : *bounds) {
            ValueID sum = entersAlone ? index.stripCasts(bound.subject) : ValueID();
            if (!sum || index.isParameter(sum) || index.opcodeOf(sum) != Opcode::Add)
                continue;
            for (uint32_t place = 0; place < 2; ++place) {
                Reference term = referenceOf(index.value(sum).operand(place));
                std::optional<Draft> draft = term && term.isValue() ? draftOfNarrowedOnEdge(from, targetIndex, bound, { }, term) : std::nullopt;
                if (!draft)
                    continue;
                Reference narrowed = build(WTF::move(*draft));
                if (isUnreachable()) {
                    m_block = { };
                    return;
                }
                for (Reference& argument : arguments) {
                    if (argument == term)
                        argument = narrowed;
                }
                noteNarrowerName(to, term, narrowed);
                count(StatisticID::AddendsNarrowedByComparisons);
            }
        }
    }
    if (condition && !asksRelation && entersAlone && !m_fieldsOfScopesOfHeads.isEmpty() && !narrowCarriedFields(edge, from, to, targetIndex)) {
        m_block = { };
        return;
    }
    if (asksRelation && entersAlone && entersLoopAtHead(index, terminator.block(), to) && !narrowSecondCounters(edge, from, to, targetIndex, arguments)) [[unlikely]] {
        m_block = { };
        return;
    }
    ValueID truthAsked = entersAlone && !targetIndex && restatesSteps ? condition : ValueID();
    if (truthAsked && !index.isParameter(truthAsked) && index.opcodeOf(truthAsked) == Opcode::ToBoolean) {
        BodyEdgeResult proven;
        proven.test = truthAsked;
        proven.subject = index.value(truthAsked).operand(0);
        proven.operandIndex = OperandIndex::ToBoolean::value;
        proven.isOnTakenEdge = true;
        Reference operand = referenceOf(proven.subject);
        std::optional<Draft> draft = operand && operand.isValue() && m_types.at(ownTypeOf(operand)).isInteger() ? draftOfNarrowedOnEdge(from, targetIndex, proven) : std::nullopt;
        if (draft) {
            Reference narrowed = build(WTF::move(*draft));
            if (isUnreachable()) {
                m_block = { };
                return;
            }
            for (Reference& argument : arguments) {
                if (argument == operand)
                    argument = narrowed;
            }
            noteNarrowerName(to, operand, narrowed);
            testedNumbers.append({ operand, narrowed });
        }
    }
    ValueID cellTypeAsked = entersAlone && index.opcodes().contains(Opcode::HasJSType) ? condition : ValueID();
    if (cellTypeAsked && !index.isParameter(cellTypeAsked) && index.opcodeOf(cellTypeAsked) == Opcode::ToBoolean)
        cellTypeAsked = index.value(cellTypeAsked).operand(0);
    if (cellTypeAsked && !index.isParameter(cellTypeAsked) && index.opcodeOf(cellTypeAsked) == Opcode::HasJSType) {
        BodyEdgeResult proven;
        proven.test = cellTypeAsked;
        proven.subject = index.value(cellTypeAsked).operand(0);
        proven.operandIndex = OperandIndex::TypeTest::value;
        proven.isOnTakenEdge = !targetIndex;
        Reference operand = referenceOf(proven.subject);
        Reference test = referenceOf(proven.test);
        std::optional<Draft> draft = test && test.isValue() && m_code.at(test.id()).opcode == Opcode::TypeTest ? draftOfNarrowedOnEdge(from, targetIndex, proven) : std::nullopt;
        if (draft) {
            Reference narrowed = build(WTF::move(*draft));
            if (isUnreachable()) {
                m_block = { };
                return;
            }
            for (Reference& argument : arguments) {
                if (argument == operand)
                    argument = narrowed;
            }
            noteNarrowerName(to, operand, narrowed);
        }
    }
    ValueID classAsked = entersAlone && index.opcodes().contains(Opcode::InstanceOf) ? condition : ValueID();
    if (classAsked && !index.isParameter(classAsked) && index.opcodeOf(classAsked) == Opcode::ToBoolean)
        classAsked = index.value(classAsked).operand(0);
    if (classAsked && !index.isParameter(classAsked) && index.opcodeOf(classAsked) == Opcode::InstanceOf) {
        Reference operand = referenceOf(index.value(classAsked).valueOperand());
        Reference test = referenceOf(classAsked);
        bool shapeWasAsked = operand && operand.isValue() && test && test.isValue() && m_code.at(test.id()).opcode == Opcode::ShapeTest;
        if (std::optional<Draft> draft = shapeWasAsked ? draftOfHolderNarrowedByShape(from, targetIndex, operand, test) : std::nullopt) {
            Reference narrowed = build(WTF::move(*draft));
            if (isUnreachable()) {
                m_block = { };
                return;
            }
            for (Reference& argument : arguments) {
                if (argument == operand)
                    argument = narrowed;
            }
            noteNarrowerName(to, operand, narrowed);
            count(StatisticID::ValuesNarrowedByInstanceOf);
        }
    }
    ValueID classAnswered = isOnlyEntry && index.opcodes().contains(Opcode::InstanceOf) ? condition : ValueID();
    while (classAnswered && !index.isParameter(classAnswered) && (index.opcodeOf(classAnswered) == Opcode::ToBoolean || index.opcodeOf(classAnswered) == Opcode::Convert))
        classAnswered = index.value(classAnswered).operand(0);
    if (classAnswered && !index.isParameter(classAnswered) && index.opcodeOf(classAnswered) == Opcode::InstanceOf)
        noteInstanceOfResult(*this, index.value(classAnswered), !targetIndex);
    static constexpr OpcodeSet questionsWhoseYesTeaches { Opcode::IsPlainInstanceOf, Opcode::IsPlainHeirOf, Opcode::IsAsBorn, Opcode::IsAsBornExceptForAddedNames, Opcode::FindsDeclaredMember };
    static_assert(OperandIndex::FindsDeclaredMember::object == OperandIndex::IsPlainInstanceOf::object);
    ValueID questionAsked = entersAlone && !targetIndex && index.holdsAnyOf(questionsWhoseYesTeaches) ? condition : ValueID();
    while (questionAsked && !index.isParameter(questionAsked) && (index.opcodeOf(questionAsked) == Opcode::ToBoolean || index.opcodeOf(questionAsked) == Opcode::Convert))
        questionAsked = index.value(questionAsked).operand(0);
    if (questionAsked && !index.isParameter(questionAsked) && questionsWhoseYesTeaches.contains(index.opcodeOf(questionAsked))) {
        BodyEdgeResult proven;
        proven.test = questionAsked;
        proven.subject = index.value(questionAsked).operand(0);
        proven.operandIndex = OperandIndex::IsPlainInstanceOf::object;
        proven.isOnTakenEdge = true;
        Reference operand = referenceOf(proven.subject);
        Reference test = referenceOf(proven.test);
        Opcode asked = test && test.isValue() ? m_code.at(test.id()).opcode : Opcode::Parameter;
        bool isStamped = (asked == Opcode::IsPlainInstanceOf || asked == Opcode::FindsDeclaredMember) && m_code.at(test.id()).receivers();
        Reference named = operand;
        if (std::optional<Draft> draft = isStamped ? draftOfNarrowedOnEdge(from, targetIndex, proven) : std::nullopt) {
            named = build(WTF::move(*draft));
            if (isUnreachable()) {
                m_block = { };
                return;
            }
            for (Reference& argument : arguments) {
                if (argument == operand)
                    argument = named;
            }
            noteNarrowerName(to, operand, named);
        }
        bool answerStands = (isStamped || asked == Opcode::AreMembersUnchanged) && named && named.isValue() && outcomeKnownOf(*this, test) == true && findKnown(*this, KnownNote::AnswerOfQuestionStillStands, operand, test);
        if (answerStands && asked == Opcode::IsPlainInstanceOf && m_code.at(test.id()).isBornWithoutPrototype())
            m_state.recordCellFacts(named, { CellFact::NoFieldOutsideSlot, CellFact::BearsNoMark });
        else if (answerStands && asked == Opcode::IsPlainInstanceOf)
            m_state.recordCellFacts(named, { CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype, CellFact::BearsNoMark });
        else if (answerStands) {
            if (isStamped)
                m_state.recordCellFacts(named, CellFact::HasLayoutPrototype);
            Vector<MemberConditionRecord, 8> guards;
            for (MemberConditionRecord guard : m_code.at(test.id()).conditions())
                guards.append(guard);
            noteGuardsShownByTest(*this, guards.span(), test);
        }
    }
    ValueID absenceAsked = !targetIndex && index.opcodes().contains(Opcode::FindsNoMember) ? condition : ValueID();
    while (absenceAsked && !index.isParameter(absenceAsked) && (index.opcodeOf(absenceAsked) == Opcode::ToBoolean || index.opcodeOf(absenceAsked) == Opcode::Convert))
        absenceAsked = index.value(absenceAsked).operand(0);
    if (absenceAsked && !index.isParameter(absenceAsked) && index.opcodeOf(absenceAsked) == Opcode::FindsNoMember) {
        Reference test = referenceOf(absenceAsked);
        const Terminator& end = m_code.at(from).terminator;
        if (test && test.isValue() && m_code.at(test.id()).opcode == Opcode::FindsNoMember && m_code.at(test.id()).receivers() && end.isConditionalBranch() && end.conditionOperand() == test.id() && findKnown(*this, KnownNote::AnswerOfQuestionStillStands, Reference::value(m_code.at(test.id()).objectOperand()), test)) {
            const Operation& question = m_code.at(test.id());
            OptionSet<CellFact> shown;
            if (question.asksForNoExtraPropertyUnderKey())
                noteKnown(*this, KnownNote::NoExtraPropertyUnderKey, Reference::value(question.objectOperand()), { }, question.key().bits(), LocationMasks(AbstractHeapSet { AbstractHeap::NamedProperties }), 1, true);
            if (question.asksForNoFieldOutsideSlot())
                shown.add(CellFact::NoFieldOutsideSlot);
            if (question.asksForLayoutPrototype())
                shown.add(CellFact::HasLayoutPrototype);
            if (!shown.isEmpty())
                m_state.recordCellFacts(Reference::value(question.objectOperand()), shown);
            for (MemberConditionRecord condition : question.conditions())
                m_state.noteGuardOfReadsHolds(condition.layout, condition.key, test);
        }
    }
    if (entersAlone && terminator.isConditionalBranch())
        nameBooleansAskedByBranch(from, terminator, targetIndex, to);
    for (auto [tested, narrowed] : testedNumbers)
        redefineStepsAfterTest(condition, tested, narrowed, to, arguments);
    StringsFinishedOnEdge textsTakenOut;
    bool endsBlockOnEdge = !entersAlone;
    if (hasStringsInBuilders() && edgeExitsBuilderLoop(*this, to)) {
        if (!m_block) {
            m_block = addBlockFor(to);
            m_code.closeEdge(edge, m_block, { });
            edge = { };
        }
        {
            SetForScope loopsAroundEdge(m_numberOfLoopsAroundEdgeBeingLed, numberOfOpenLoopsAround(frameIndex, to));
            finishStringsInBuildersOnEdge(*this, to, target, arguments, textsTakenOut);
        }
        endsBlockOnEdge |= !standUnderSameHandler(index, m_frames[frameIndex].block, to);
        if (isUnreachable()) {
            m_block = { };
            return;
        }
    }
    if (hasStringsInBuilders() && !head && edgeResetsStringInBuilder(*this, to, arguments.span())) {
        if (!m_block) {
            m_block = addBlockFor(to);
            m_code.closeEdge(edge, m_block, { });
            edge = { };
        }
        {
            SetForScope loopsAroundEdge(m_numberOfLoopsAroundEdgeBeingLed, numberOfOpenLoopsAround(frameIndex, to));
            resetStringsInBuildersOnEdge(*this, to, arguments);
        }
        if (isUnreachable()) {
            m_block = { };
            return;
        }
    }
    if (hasStringsInBuilders() && !head && edgePassesStringInBuilder(*this, to, arguments.span().first(target.arguments().size()))) {
        if (!m_block) {
            m_block = addBlockFor(to);
            m_code.closeEdge(edge, m_block, { });
            edge = { };
        }
        {
            SetForScope loopsAroundEdge(m_numberOfLoopsAroundEdgeBeingLed, numberOfOpenLoopsAround(frameIndex, to));
            materializeStringsPassedOnEdge(*this, to, arguments.mutableSpan().first(target.arguments().size()));
        }
        endsBlockOnEdge |= !standUnderSameHandler(index, m_frames[frameIndex].block, to);
        if (isUnreachable()) {
            m_block = { };
            return;
        }
    }
    for (Reference argument : arguments) {
        if (!ownTypeOf(argument)) {
            fail("an edge hands on a value that nothing was built for"_s);
            return;
        }
    }

    if (head) {
        closeBackEdge(edge, head, arguments.span());
        return;
    }
    if (m_block && endsBlockOnEdge) {
        edge = m_code.endWithJump(m_block, m_origin);
        m_block = { };
    }

    uint32_t place = addOpenEdge(entered);
    if (place == noOpenEdge)
        return;
    OpenEdge& open = m_openEdges[place];
    open.edge = edge;
    open.block = m_block;
    for (Reference argument : arguments) {
        open.arguments.append(argument);
        open.types.append(ownTypeOf(argument));
    }
    open.state = WTF::move(m_state);
    open.inputBlockLeft = m_frames[frameIndex].block;
    open.isLedByClosedArm = !m_opensGuesses;
    open.comesFromClosedArm = m_comparesElementsOfViews && m_frames[frameIndex].loopDepthAtCall && (m_frames[frameIndex].blockFollowsClosedArms || (!m_opensGuesses && m_code.at(from).frequency == BlockFrequency::Rare));
    open.isLedByClosedArmThatKeepsSites = !m_opensGuesses && m_closedArmKeepsSites;
    if (Frame::DuplicatedValues* listed = m_frames[frameIndex].duplicatedValues.get()) [[unlikely]] {
        takeNamesOntoEdge(open, frameIndex);
        auto recordOf = [](const std::pair<Reference, Reference>& taken) { return taken.first.bits(); };
        std::ranges::stable_sort(textsTakenOut.strings, { }, recordOf);
        for (ValueID value : textsTakenOut.names) {
            Reference name = listed->nameOn(open.names, value);
            auto taken = std::ranges::lower_bound(textsTakenOut.strings, name.bits(), { }, recordOf);
            if (taken != textsTakenOut.strings.end() && taken->first == name)
                listed->nameOtherwiseOn(open.names, value, taken->second);
        }
    }
    m_block = { };
}

size_t Walker::findFramesUpToHandlersBelow() const
{
    size_t numberOfFrames = m_frames.size();
    size_t lowestNotAsked = numberOfFrames;
    while (lowestNotAsked && m_frames[lowestNotAsked - 1].numberOfFramesUpToHandlerBelow == Frame::notAskedYet)
        --lowestNotAsked;
    for (size_t index = lowestNotAsked; index < numberOfFrames; ++index)
        m_frames[index].numberOfFramesUpToHandlerBelow = !index ? 0 : m_frames[index - 1].hasHandler ? static_cast<uint32_t>(index) : m_frames[index - 1].numberOfFramesUpToHandlerBelow;
    return m_frames[numberOfFrames - 1].numberOfFramesUpToHandlerBelow;
}

bool Walker::isWhatHeadNamesForHandler(size_t frameIndex, ValueID argument, Reference handed)
{
    const Frame& frame = m_frames[frameIndex];
    if (frame.handlerBindingsBlock != frame.block || frame.hasPreheaderHandlerBindings != frame.isInPreheader || hasStringsInBuilders())
        return true;
    if (m_generatorsWalkedInConsumers.containsIf([&](const GeneratorWalkedInConsumer& walked) { return walked.frameOfConsumer == frameIndex; }))
        return true;
    auto behindNamesAndChecks = [&](ValueID read) {
        while ((read = valueBehindNamesInWalk(m_code, read)) && m_code.at(read).opcode == Opcode::RequireObjectCoercible)
            read = m_code.at(read).valueOperand();
        return read;
    };
    for (auto [value, name] : frame.handlerBindings) {
        if (value != argument || name == handed || !name.isRecord() || !handed.isRecord())
            continue;
        if (handed.isConstant())
            return m_types.isSubset(typeOf(handed), typeOf(name));
        ValueID behindHanded = behindNamesAndChecks(handed.id());
        ValueID behindName = behindNamesAndChecks(name.id());
        return !behindHanded || !behindName || (!name.isConstant() && behindHanded == behindName);
    }
    return true;
}

const Walker::OpenLoop* Walker::loopLeftByHoisting(size_t frameIndex) const
{
    if (m_numberOfLoopsThatMayBeLeft >= m_openLoops.size())
        return nullptr;
    const OpenLoop& loop = m_openLoops[m_numberOfLoopsThatMayBeLeft];
    return loop.frame == frameIndex && loop.loop ? &loop : nullptr;
}

Reference Walker::valueEnteringLoop(const OpenLoop& loop, Reference reference) const
{
    if (!reference || !reference.isValue() || m_code.isConstant(reference.id()))
        return reference;
    std::span<const ValueID> parameters = m_code.at(loop.head).parameters();
    ValueID name = reference.id();
    while (conversionsOfForm.contains(m_code.at(name).opcode) || valuePreservingOpcodes.contains(m_code.at(name).opcode))
        name = m_code.at(name).operand(0);
    size_t place = std::ranges::find(parameters, name) - parameters.begin();
    if (place >= loop.recordsEntering().size())
        return reference;
    ValueID entering = loop.recordsEntering()[place];
    return m_code.isConstant(entering) ? Reference::constant(entering) : Reference::value(entering);
}

void Walker::leadHandlerEdge()
{
    if (m_code.at(m_block).handler)
        return;
    if (handlerMadeOverCopiesIsInForce()) {
        m_code.closeEdge(m_code.addHandlerEdge(m_block), m_handlerMadeOverCopies, { });
        return;
    }
    size_t frameIndex = numberOfFramesUpToHandlerInForce();
    if (BlockID handler = m_numberOfFramesOverHandlersOfCode ? handlerOfCodeInForce(frameIndex) : BlockID()) {
        m_code.closeEdge(m_code.addHandlerEdge(m_block), handler, { });
        return;
    }
    if (!frameIndex--)
        return;
    if (!m_generatorsWalkedInConsumers.isEmpty())
        bindParametersOfHeadForHandlerOfStep(frameIndex);

    SetForScope isLeading(m_isLeadingHandlerEdge, true);
    BodyTarget target = m_frames[frameIndex].index->block(m_frames[frameIndex].block).handler();
    const OpenLoop* loopLeft = loopLeftByHoisting(frameIndex);
    if (loopLeft) {
        BodyBlock header = m_frames[frameIndex].index->block(m_frames[frameIndex].index->loop(loopLeft->loop).head);
        if (header.hasHandler() && header.handler().block() == target.block())
            target = header.handler();
        else
            loopLeft = nullptr;
    }
    Vector<Reference, 4> arguments;
    Vector<TypeID, 4> types;
    for (ValueID argument : target.arguments()) {
        Reference reference = nameBehindStandIn(frameIndex, argument, referenceBoundIn(m_frames[frameIndex], argument));
        if (loopLeft)
            reference = valueEnteringLoop(*loopLeft, reference);
        TypeID type = ownTypeOf(reference);
        if (!type) {
            fail("a handler edge hands on a value that nothing was built for"_s);
            return;
        }
        if (m_inputs.options.verifiesCode && !loopLeft && !isWhatHeadNamesForHandler(frameIndex, argument, reference)) [[unlikely]] {
            fail("a handler edge passes a placeholder for a value"_s, InternalErrorKind::HandlerEdgePassesPlaceholder);
            return;
        }
        Representation form = boxedFormOf(type);
        if (!isSameWord(representationOf(reference), form)) {
            if (isInLoop())
                m_code.noteBoxForHandler();
            reference = convert(reference, form, m_origin);
        }
        if (!reference)
            return;
        arguments.append(reference);
        types.append(type);
    }

    Vector<std::pair<ValueID, Reference>, 2> names;
    if (m_frames[frameIndex].references.size() > m_frames[frameIndex].index->numberOfValues() && !takeCarriedValuesOntoHandlerEdge(frameIndex, names))
        return;
    if (m_frames[frameIndex].duplicatedValues && !boxValuesThatHandlerOutsideLoopReads(frameIndex, target.block(), loopLeft, names))
        return;
    if (hasStringsInBuilders() && leadHandlerEdgeBehindStringsOfTexts(frameIndex, target, arguments, types, names))
        return;
    uint32_t place = addOpenEdge(frameIndex, target.block());
    if (place == noOpenEdge)
        return;
    OpenEdge& open = m_openEdges[place];
    open.edge = m_code.addHandlerEdge(m_block);
    open.arguments = WTF::move(arguments);
    open.types = WTF::move(types);
    takeNamesOntoEdge(open, frameIndex);
    for (auto [value, name] : names)
        m_frames[frameIndex].duplicatedValues->nameOtherwiseOn(open.names, value, name);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::setCurrentInputBlock(Frame& frame, const BodyBlock& block, InputBlock& record)
{
    frame.block = block.id();
    frame.endOfValues = block.endOfValues().index();
    frame.hasHandler = block.hasHandler();
    if (frame.hasHandler && throwsPastHandlerOf(block))
        frame.hasHandler = false;
    frame.handlerOnlyClosesIterations = frame.hasHandler && handlerOnlyClosesIterations(frame, block);
    frame.isInPreheader = false;

    uint32_t& first = record.firstNarrowerName;
    if (first == narrowerNamesOfDominator) {
        BlockID dominator = frame.index->immediateDominator(block.id());
        first = dominator ? frame.blocks[dominator.index()].firstNarrowerName : noNarrowerName;
        if (first == narrowerNamesOfDominator)
            first = noNarrowerName;
    }
    frame.firstNarrowerName = first;
    frame.listOfNarrowerNamesIsLong = frame.lengthOfNarrowerNames(first) >= Frame::lengthOfLongListOfNarrowerNames;
    frame.narrowedValues = frame.valuesOfNarrowerNames(first);
}

void Walker::switchToInputBlock(BlockID inputBlock)
{
    Frame& frame = m_frames.last();
    setCurrentInputBlock(frame, frame.index->block(inputBlock), this->inputBlock(frame, inputBlock));
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::enterJoin(BlockID inputBlock, InputBlock& record, const BodyBlock& input)
{
    uint32_t only = record.firstOpenEdge;
    OpenEdge& edge = m_openEdges[only];
    if (edge.next != noOpenEdge || (!edge.block && hasStringsInBuilders())) {
        Vector<uint32_t, 4> edges = takeOpenEdges(inputBlock);
        closeEdgesInto(addBlockFor(inputBlock), inputBlock, edges.span(), false);
        return;
    }
    record.isEntered = true;
    record.firstOpenEdge = noOpenEdge;


    uint32_t numberOfParameters = input.numberOfParameters();
    if (edge.arguments.size() != numberOfParameters) {
        fail("the arguments and proofs of an edge do not fill the parameters of its target"_s);
        return;
    }
    for (uint32_t index = 0; index < numberOfParameters; ++index)
        bind(input.parameter(index), edge.arguments[index]);
    if (m_frames.last().duplicatedValues)
        takeNamesOfWay(inputBlock, WTF::move(edge.names));
    m_block = edge.block;
    if (!m_block) {
        m_block = addBlockFor(inputBlock);
        m_code.closeEdge(edge.edge, m_block, { });
    }
    m_state = WTF::move(edge.state);
    releaseOpenEdge(only);
}

void Walker::noteTestSeenInTries(Reference value, ValueID operation, TypeID tested)
{
    if (m_trialVisits.isEmpty() || !value || !value.isValue())
        return;
    if (TrialVisit& innermost = m_trialVisits.last(); operation && tested && innermost.frameIndex == m_frames.size() - 1) {
        currentBodyIndex().value(operation).forEachOperand([&](ValueID operand, auto...) {
            if (operand && referenceOf(operand) == value && innermost.operandsOfTestsSeen.add(static_cast<uint64_t>(operation.index()) << 32 | operand.index()).isNewEntry)
                innermost.testsSeen.append(GuessCountedOnByLooks { operation, operand, tested });
        });
    }
}

void Walker::noteGuardOfStoresAskedInTries(const MemberConditionRecord& condition)
{
    if (m_trialVisits.isEmpty() || m_trialVisits.last().guardsOfStoresAsked.contains(condition))
        return;
    m_trialVisits.last().guardsOfStoresAsked.append(condition);
}

void Walker::noteBirthInTries(LayoutID classLayout)
{
    if (!m_trialVisits.isEmpty() && !m_trialVisits.last().classesOfBirths.contains(classLayout))
        m_trialVisits.last().classesOfBirths.append(classLayout);
}

void Walker::noteClassesOfBirthsAtOrBelowInTries(LayoutID classLayout)
{
    if (m_trialVisits.isEmpty())
        return;
    Vector<LayoutID, 2> born = m_trialVisits.last().classesOfBirths;
    for (LayoutID made : born) {
        size_t stepsLeft = program().layouts.size();
        for (LayoutID current = made; current && stepsLeft--; current = parentNamedByClassRecord(program(), facts(), current).layout) {
            if (current == classLayout) {
                noteGuardOfStoresAskedInTries(MemberConditionRecord { MemberConditionKind::ClassEvaluatedOnce, made, ClassSide::Prototype, { } });
                break;
            }
        }
    }
}

void Walker::noteGuardsBegunInTries(LayoutID classLayout)
{
    if (!m_trialVisits.isEmpty() && !m_trialVisits.last().classesWhoseGuardsTurnBegins.contains(classLayout))
        m_trialVisits.last().classesWhoseGuardsTurnBegins.append(classLayout);
}

bool Walker::turnMakesObjectAtOrBelow(LayoutID classLayout) const
{
    if (m_trialVisits.isEmpty())
        return false;
    for (LayoutID born : m_trialVisits.last().classesOfBirths) {
        size_t stepsLeft = program().layouts.size();
        for (LayoutID current = born; current && stepsLeft--; current = parentNamedByClassRecord(program(), facts(), current).layout) {
            if (current == classLayout)
                return true;
        }
    }
    return false;
}

void Walker::noteMemberAskedByItsByteInTries(const BodyValue& read, TypeID typeOfHolder)
{
    if (m_trialVisits.isEmpty() || m_trialVisits.last().frameIndex != m_frames.size() - 1)
        return;
    Vector<GuessCountedOnByLooks, 4>& seen = m_trialVisits.last().testsSeen;
    if (seen.isEmpty() || seen.last().operation != read.id()) {
        seen.append(GuessCountedOnByLooks { read.id(), read.objectOperand(), typeOfHolder });
        m_trialVisits.last().operandsOfTestsSeen.add(static_cast<uint64_t>(read.id().index()) << 32 | read.objectOperand().index());
    }
}

static bool comesRoundAsNarrowReference(Walker& walker, BlockID inputHead, uint32_t position, TypeID type, Reference entering)
{
    TypeArena& types = walker.types();
    if (!entering || !entering.isValue() || walker.representationOf(entering) != Representation::Cell32 || !types.at(type).hasCells() || !isLegalRepresentation(types, type, Representation::Cell32))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    LoopID loop = index.loopOf(inputHead);
    ValueID parameter = index.block(inputHead).parameter(position);
    bool isRead = false;
    for (const BodyIncomingEdge& edge : walker.incomingEdgesOf(index, inputHead)) {
        if (!index.isInLoop(edge.from, loop))
            continue;
        std::span<const ValueID> arguments = edge.target.arguments();
        ValueID next = position < arguments.size() && arguments[position] ? index.stripCasts(arguments[position]) : ValueID();
        if (!next)
            return false;
        if (next == parameter)
            continue;
        if (index.isParameter(next) || index.opcodeOf(next) != Opcode::PropGet)
            return false;
        isRead = true;
    }
    return isRead;
}

void Walker::enterLoopHead(BlockID inputHead)
{
    if (walkIsCutShortHere())
        return;
    size_t frameIndex = m_frames.size() - 1;
    BodyBlock input = currentBodyIndex().block(inputHead);
    uint32_t numberOfParameters = input.numberOfParameters();
    uint32_t depthAroundLoop = m_frames[frameIndex].loopDepthAtCall + currentBodyIndex().loopDepthOf(inputHead) - 1;
    m_frames[frameIndex].isInPreheader = true;

    Vector<uint32_t, 4> edges = takeOpenEdges(inputHead);
    BlockFrequency frequency = m_coldCopyDepth ? BlockFrequency::Rare : BlockFrequency::Normal;
    BlockID preheader = m_code.addBlock(BlockRole::Ordinary, frequency);
    m_code.setLoopMarks(preheader, !m_coldCopyDepth && depthAroundLoop, false);
    closeEdgesInto(preheader, inputHead, edges.span(), false);
    if (m_hasFailed)
        return;

    Vector<Reference, 8> entering;
    Vector<TypeID, 8> entryTypes;
    for (uint32_t index = 0; index < numberOfParameters; ++index) {
        entering.append(referenceOf(input.parameter(index)));
        entryTypes.append(ownTypeOf(entering.last()));
    }
    if (ValueID step = std::exchange(m_frames[frameIndex].stepWhosePositionEdgesBring, ValueID()))
        m_indexedIterations[m_frames[frameIndex].placeOfForInWhosePositionEdgesBring].position = m_frames[frameIndex].duplicatedValues->nameOn(m_frames[frameIndex].duplicatedValues->namesOfWay, step);
    if (m_frames.size() > 1)
        takePassedGeneratorForLoop(inputHead);
    if (!loopStepsGeneratorTaken(inputHead) && (writeOutTurnsOfLoop(inputHead, entering.span()) || unrollOverListedKeys(inputHead, entering.span()) || writeOutTurnsOfForIn(inputHead, entering.span()) || writeOutTurnsOfForOf(inputHead, entering.span())))
        return;
    if (!hasOpenTrialVisit(frameIndex, inputHead) && !m_loopsInFinalBuild.contains(std::pair { static_cast<uint32_t>(frameIndex), inputHead }))
        reserveRoomOfListsThatLoopFills(*this, inputHead, entering.span());
    if (m_hasFailed || isUnreachable())
        return;
    if (runTrialVisits(inputHead, entering.span())) {
        if (!m_frames[frameIndex].loopsBeingVersioned.contains(currentBodyIndex().loopOf(inputHead)))
            letGoOfTextsThatLoopTook(inputHead);
        m_loopThatItsTryHasWalked = { frameIndex, currentBodyIndex().loopOf(inputHead) };
        return;
    }
    for (Reference argument : entering)
        letLeave(argument);
    if (hasStringsInBuilders())
        typeBuildersAsStrings(*this, entering.span(), entryTypes);
    Frame& frame = m_frames[frameIndex];
    bool isInsideLoopLookedAt = frame.lookedAt.loop && frame.lookedAt.loop != currentBodyIndex().loopOf(inputHead) && currentBodyIndex().isInLoop(inputHead, frame.lookedAt.loop);
    Vector<TypeID, 8> settledAround;
    if (isInsideLoopLookedAt) {
        uint32_t placeOfHead = input.firstParameter().index() - frame.lookedAt.firstValue;
        RELEASE_ASSERT(!numberOfParameters || frame.lookedAt.types->holdsInOneRun(placeOfHead, numberOfParameters));
        for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter)
            settledAround.append((*frame.lookedAt.types)[placeOfHead + parameter]);
    }
    std::optional<EqualityLoopTest> equality = findEqualityLoopTest(*this, inputHead);
    LoopPreview preview = hasOpenTrialVisit(frameIndex, inputHead) ? makePreviewFromSummary(inputHead, entryTypes.span(), equality ? &*equality : nullptr) : makePessimisticPreview(*this, inputHead, entryTypes.span(), settledAround.span());
    if (m_hasFailed)
        return;
    if (preview.typesOfParameters.size() != numberOfParameters) {
        fail("a loop preview has a different number of parameter types than the loop header has parameters"_s);
        return;
    }
    if (!m_generatorsTakenByConsumers.isEmpty() && walkGeneratorInPlaceOfLoop(inputHead, preview.typesOfParameters.span(), entering.span()))
        return;
    bool turnsBorrowScopes = !preview.scopesThatTurnsOnlyBorrow.isEmpty();
    if (!preview.hasLetMentionsLeave && m_inheritedSummaryHeader == inputHead && m_inheritedSummaryFrame == frameIndex)
        m_inheritedScopeStores.areKnown = false;
    if (!m_versionedLoops.isEmpty() && canUseIterationGuards(inputHead))
        m_versionedLoops.last().guessesCountedOn = WTF::move(preview.guessesCountedOn);
    buildPollInFrontOfLoop(inputHead, preview, entering.span());
    if (m_hasFailed)
        return;
    beginStringsInBuilders(*this, inputHead, preview, entering, entryTypes);
    if (m_hasFailed || isUnreachable())
        return;
    if (isInsideLoopLookedAt) {
        const BodyIndex& index = currentBodyIndex();
        LoopID loop = index.loopOf(inputHead);
        uint32_t numberOfLoopsLeft = preview.typesOfValues.isEmpty() || frame.lookedAt.types->holdsAt(preview.firstValue - frame.lookedAt.firstValue, preview.typesOfValues) ? 0 : index.loop(loop).numberOfLoopsWithin - 1;
        for (uint32_t inner = loop.index() + 1; numberOfLoopsLeft && inner < index.numberOfLoops(); ++inner) {
            BodyBlock innerHead = index.block(index.loop(LoopID(inner)).head);
            if (!index.isInLoop(innerHead.id(), loop))
                continue;
            --numberOfLoopsLeft;
            for (uint32_t parameter = 0; parameter < innerHead.numberOfParameters(); ++parameter) {
                uint32_t value = innerHead.parameter(parameter).index();
                TypeID found = preview.typesOfValues[value - preview.firstValue];
                uint32_t place = value - frame.lookedAt.firstValue;
                TypeID around = (*frame.lookedAt.types)[place];
                if (!frame.lookedAt.types->holds(place) || !found || (around && !m_types.isSubset(found, around)))
                    continue;
                if (frame.lookedAt.types.use_count() != 1)
                    frame.lookedAt.types = std::make_shared<TypesOfValuesOfLoop>(*frame.lookedAt.types);
                frame.lookedAt.types->set(place, found);
            }
        }
    } else if (!preview.typesOfValues.isEmpty()) {
        frame.lookedAt = { currentBodyIndex().loopOf(inputHead), preview.firstValue, std::make_shared<TypesOfValuesOfLoop>(WTF::move(preview.typesOfValues)) };
    }

    Vector<FieldOfScope, 4> fieldsThatComeRound;
    if (preview.effects.maySuspend)
        letObjectsLeaveAtSuspension(*this);
    else if (turnsBorrowScopes)
        findFieldsOfScopesInFrontOfLoop(preview, fieldsThatComeRound);
    findFieldsOfScopesFromOutsideThatQuietTurnsStore(inputHead, preview.effects, preview.slots.span(), fieldsThatComeRound, true);
    findSlotsOfObjectsThatComeRound(inputHead, preview, entering.span(), fieldsThatComeRound);
    if (hasOpenTrialVisit(frameIndex, inputHead)) {
        m_trialVisits.last().assumed = preview.effects;
        m_trialVisits.last().knowsWhatIsAssumed = true;
        m_trialVisits.last().numberOfRecordsAtHead = m_code.numberOfValues();
        if (m_trialVisits.last().wantsStateInFront)
            m_trialVisits.last().stateInFront = std::make_shared<WalkState>(m_state);
    }
    if (!preview.hasLetMentionsLeave)
        m_state.clear();
    else {
        if (turnsBorrowScopes)
            endFieldsOfScopesThatTurnsStore(preview);
        if (preview.effects.writesObjectsMadeHereAlone && !preview.effects.mayRunOtherCode() && hasOpenTrialVisit(frameIndex, inputHead)) {
            AllocationsUsedInLoop mentioned = allocationsUsedInLoop(inputHead);
            m_trialVisits.last().othersAreSparedAtHead = mentioned;
            Vector<uint32_t, 2> listsTested;
            if (m_headOfTypedBuildOverListsTestedInFront == inputHead && m_frameOfTypedBuildOverListsTestedInFront == frameIndex)
                listsTested = m_listsMadeHereOfTypedBuildOverListsTestedInFront;
            m_trialVisits.last().listsMadeHereThatAreSparedAtHead = listsTested;
            m_state.invalidate(preview.effects, [&](Reference holder) {
                std::optional<IsListedAllocation> other = classifyAgainstAllocations(mentioned, holder);
                return !other || (*other == IsListedAllocation::Yes && !listsTested.contains(underlyingCellOf(m_code, holder.id()).index()));
            }, [](LayoutID, PropertyKeyID) {
                return true;
            });
        } else
            m_state.invalidate(preview.effects);
    }
    if (!fieldsThatComeRound.isEmpty())
        keepFieldsOfScopesThatLoopWrites(inputHead, fieldsThatComeRound);

    BlockID head = m_code.addBlock(BlockRole::Ordinary, frequency);
    m_code.setLoopMarks(head, !m_coldCopyDepth, true);
    Vector<ValueID, 8> parameters;
    for (uint32_t index = 0; index < numberOfParameters; ++index) {
        TypeID type = preview.typesOfParameters[index];
        if (!type || !entryTypes[index] || !m_types.isSubset(entryTypes[index], type)) {
            fail("what enters a loop lies outside the previewed type of the head's parameter"_s);
            return;
        }
        Representation form = formOfParameter(type, { });
        if (form == Representation::Int64 && index < preview.parametersSteppedOnDoubles.size() && preview.parametersSteppedOnDoubles[index])
            form = Representation::Double;
        if (form == Representation::JSValue && comesRoundAsNarrowReference(*this, inputHead, index, type, entering[index]))
            form = Representation::Cell32;
        parameters.append(m_code.appendParameter(head, type, form, m_origin));
        bind(input.parameter(index), Reference::value(parameters.last()));
    }
    for (size_t place = m_indexedIterations.size(); place-- && m_numberOfIterationsInFrontOfTheirLoops;) {
        IndexedIteration& iteration = m_indexedIterations[place];
        if (iteration.head || iteration.inputHead != inputHead || iteration.inlineFrame != m_frames[frameIndex].inlineFrame)
            continue;
        --m_numberOfIterationsInFrontOfTheirLoops;
        TypeID type = m_types.integer(Range { 0, doneIterationPosition });
        entering.append(iteration.position);
        parameters.append(m_code.appendParameter(head, type, formOfParameter(type, { }), m_origin));
        iteration.position = Reference::value(parameters.last());
        if (iteration.table) {
            entering.append(iteration.table);
            parameters.append(m_code.appendParameter(head, TypeTable::anything, Representation::JSValue, m_origin));
            iteration.table = Reference::value(parameters.last());
        }
        if (iteration.count && !countOfNamesComesRound(*this, iteration))
            iteration.count = { };
        if (iteration.count) {
            TypeID typeOfCount = m_types.integer(Range { 0, UINT32_MAX });
            entering.append(iteration.count);
            parameters.append(m_code.appendParameter(head, typeOfCount, formOfParameter(typeOfCount, { }), m_origin));
            iteration.count = Reference::value(parameters.last());
        }
        iteration.head = head;
        m_iterationsOfHeads.add(head.index(), Vector<uint32_t, 2> { }).iterator->value.append(static_cast<uint32_t>(place));
    }
    if (m_loopsOverGenerators.hasStepsWrittenByHand)
        carryStepParts(*this, inputHead, head, entering, parameters, m_origin);
    if (!m_generatorsWalkedInConsumers.isEmpty())
        carryValuesRoundLoopOfGenerator(inputHead, head, entering, parameters);
    for (FieldOfScope& field : fieldsThatComeRound) {
        entering.append(field.values[0]);
        parameters.append(m_code.appendParameter(head, field.type, field.form, m_origin));
        field.parameter = Reference::value(parameters.last());
        noteWhatParameterOfFieldIsShownToBe(field);
        if (field.positionOfHolder) {
            field.scope = Reference::value(parameters[*field.positionOfHolder]);
            m_carriesSlotsOfObjects = true;
            count(StatisticID::SlotsOfObjectsCarriedAroundLoops);
        } else if (field.slotKey)
            count(StatisticID::SlotsCarriedAroundLoops);
    }
    m_pendingCarriedResultCount = 0;
    if (!m_carriedResultRequests.isEmpty() && hasOpenTrialVisit(frameIndex, inputHead) && has(Mechanism::LoopInvariants) && !preview.effects.maySuspend) {
        if (auto requests = m_carriedResultRequests.find(m_trialVisits.last().summaryIndex); requests != m_carriedResultRequests.end()) {
            for (const CarriedResult& requested : requests->value) {
                entering.append(constant(Draft(Opcode::Absent, m_origin)));
                parameters.append(m_code.appendParameter(head, requested.type, Representation::JSValue, m_origin));
            }
            m_pendingCarriedResultCount = requests->value.size();
        }
    }
    if (!parameters.isEmpty())
        m_code.setParameters(head, parameters.span());
    InputBlock& opened = inputBlock(frameIndex, inputHead);
    opened.head = head;
    opened.hasEqualityTest = equality.has_value();
    opened.isGuardedByEndTest = false;
    m_normalizesLoopTests |= opened.hasEqualityTest;
    opened.counterStaysWithinBound = equality && counterStaysWithinBound(*this, inputHead, *equality, entryTypes[equality->position]);

    if (has(Mechanism::LoopInvariants))
        hoistInvariants(*this, inputHead, preview, entering.span());
    m_frames[frameIndex].isInPreheader = false;
    if (isUnreachable()) {
        m_block = { };
        return;
    }

    if (!preview.effects.maySuspend)
    {
        m_numberOfFieldsOfScopesOfHeadBeingOpened = fieldsThatComeRound.size() + m_pendingCarriedResultCount;
        leaveBlockOpenInFrontOfLoop(inputHead, head, entering.span(), preview.effects);
        m_numberOfFieldsOfScopesOfHeadBeingOpened = 0;
    }
    else
        crossEdge({ }, head, entering.span());
    if (!m_hasFailed && !fieldsThatComeRound.isEmpty()) {
        noteFieldsOfScopes(fieldsThatComeRound.span());
        fieldsOfScopesLaidAt(head).append(fieldsThatComeRound.span());
    }
    if (!m_hasFailed && turnsBorrowScopes)
        m_trialVisits.last().keepsScopesAtHead = true;
    if (!m_hasFailed)
        standAheadOfEdges(head);
    if (checksWalk() || checksProofs())
        layChecksOfParametersOfHead(head, numberOfParameters);
    markIterationRestartPoint(inputHead, preview.effects);
    noteHeadOfQuietTurns(head);
}

size_t Walker::Frame::placeOfStateOfHandler(BlockID handler) const
{
    if (placesOfStatesOfHandlers.isEmpty()) {
        return statesOfHandlers.findIf([&](const StateOfHandler& known) {
            return known.handler == handler;
        });
    }
    auto found = placesOfStatesOfHandlers.find(handler.index());
    return found == placesOfStatesOfHandlers.end() ? notFound : found->value;
}

Walker::StateOfHandler* Walker::Frame::findStateOfHandler(BlockID handler)
{
    size_t place = placeOfStateOfHandler(handler);
    return place == notFound ? nullptr : &statesOfHandlers[place];
}

void Walker::Frame::addStateOfHandler(BlockID handler, WalkState&& state)
{
    constexpr size_t mostLookedThrough = 8;
    statesOfHandlers.append(StateOfHandler { handler, WTF::move(state), { } });
    if (!placesOfStatesOfHandlers.isEmpty())
        placesOfStatesOfHandlers.add(handler.index(), statesOfHandlers.size() - 1);
    else if (statesOfHandlers.size() > mostLookedThrough) {
        for (uint32_t place = 0; place < statesOfHandlers.size(); ++place)
            placesOfStatesOfHandlers.add(statesOfHandlers[place].handler.index(), place);
    }
}

bool Walker::Frame::takeStateOfHandler(BlockID handler, WalkState& state)
{
    size_t place = placeOfStateOfHandler(handler);
    if (place == notFound)
        return false;
    state = WTF::move(statesOfHandlers[place].state);
    if (place + 1 < statesOfHandlers.size()) {
        statesOfHandlers[place] = WTF::move(statesOfHandlers.last());
        if (!placesOfStatesOfHandlers.isEmpty())
            placesOfStatesOfHandlers.set(statesOfHandlers[place].handler.index(), static_cast<uint32_t>(place));
    }
    statesOfHandlers.removeLast();
    placesOfStatesOfHandlers.remove(handler.index());
    return true;
}

void Walker::keepStateOfHandlerForUndo(size_t frameIndex, BlockID handler)
{
    if (!m_serialOfNewestMark)
        return;
    StateOfHandler* known = m_frames[frameIndex].findStateOfHandler(handler);
    if (known && known->serialLogged >= m_serialOfNewestMark)
        return;
    m_statesOfHandlersInTries.append(StateOfHandlerInTry { static_cast<uint32_t>(frameIndex), handler, known ? std::optional<StateOfHandler>(*known) : std::nullopt });
    if (known)
        known->serialLogged = m_serialOfNewestMark;
}

void Walker::noteStateAtHeadForHandler()
{
    Frame& frame = m_frames.last();
    BodyTarget target = frame.index->block(frame.block).handler();
    if (m_inputs.options.verifiesCode) [[unlikely]] {
        frame.handlerBindingsBlock = frame.block;
        frame.hasPreheaderHandlerBindings = frame.isInPreheader;
        frame.handlerBindings.shrink(0);
        for (ValueID argument : target.arguments())
            frame.handlerBindings.append({ argument, referenceBoundIn(frame, argument) });
    }
    BlockID handler = target.block();
    keepStateOfHandlerForUndo(m_frames.size() - 1, handler);
    if (StateOfHandler* known = frame.findStateOfHandler(handler)) {
        if (!known->state.isEmpty())
            known->state.meetWith(m_state, &known->lastMet);
        return;
    }
    frame.addStateOfHandler(handler, WalkState(m_state));
    frame.statesOfHandlers.last().serialLogged = m_serialOfNewestMark;
}

void Walker::enterHandlerBlock(BlockID inputBlock)
{
    BodyBlock input = currentBodyIndex().block(inputBlock);
    Vector<uint32_t, 4> edges = takeOpenEdges(inputBlock);
    if (has(Mechanism::Handlers) && enterBehindHeadOfHandlerBlock(inputBlock, edges.span()))
        return;
    BlockID block = addBlockFor(inputBlock, input.role(), BlockFrequency::Unlikely);
    closeEdgesInto(block, inputBlock, edges.span(), true);
    if (hasStringsInBuilders() && !m_hasFailed)
        bindTextsAtHeadOfHandlerBlock(inputBlock);
    if (m_hasFailed)
        return;

    if (ValueID subject = input.role() == BlockRole::IterationCleanup ? input.subject() : ValueID()) {
        if (Reference reference = referenceOf(subject); reference && reference.isRecord())
            m_code.setSubject(block, reference.id());
    }

    m_state.clear();
    if (!has(Mechanism::Handlers))
        return;
    keepStateOfHandlerForUndo(m_frames.size() - 1, inputBlock);
    m_frames.last().takeStateOfHandler(inputBlock, m_state);
    enterHandler(*this, inputBlock);
}

void Walker::walkBlock(BlockID inputBlock, bool isWalkedOnPaths)
{
    size_t frameIndex = m_frames.size() - 1;
    auto noteEndOfBlock = makeScopeExit([&] {
        if (frameIndex >= m_frames.size())
            return;
        this->inputBlock(frameIndex, inputBlock).numberOfRecordsBehindEnd = m_code.numberOfValues();
        if (Vector<BlockID, 2>& duplicated = m_frames[frameIndex].blocksBeingTailDuplicated; !isWalkedOnPaths && duplicated.contains(inputBlock)) [[unlikely]]
            duplicated.shrink(0);
    });
    const BodyIndex& index = *m_frames.last().index;
    BodyBlock block = index.block(inputBlock);
    if (!frameIndex && !m_pendingBlocks.isEmpty()) {
        if (m_frames[0].loopsBeingVersioned.isEmpty() && m_reversionableLoops.isEmpty())
            m_pendingBlocks.shrink(0);
        else
            releaseEndedPendingBlocks();
    }
    if (!m_openLoops.isEmpty() && !m_walksCleanupBesideCopyOfLoop) {
        const OpenLoop& newest = m_openLoops.last();
        if (newest.frame > frameIndex || (newest.frame == frameIndex && inputBlock.index() > index.loop(newest.loop).last.index()))
            endBlocksInFrontOfLoops(frameIndex, inputBlock);
    }

    Frame& frame = m_frames.last();
    InputBlock& record = this->inputBlock(frame, inputBlock);
    if (inputBlock != entryBlock) {
        m_block = { };
        uint32_t firstOpenEdge = record.firstOpenEdge;
        if (firstOpenEdge == noOpenEdge) {
            if (m_hoistedDiamond && m_hoistedDiamond->inputJoin == inputBlock) [[unlikely]]
                fail("no way leads to where the arms walked in front of a loop meet"_s);
            return;
        }
        std::span<const BlockID> passedOverByScan = facts().summaries().at(frame.function).blocksNeverEntered.span();
        if (!passedOverByScan.empty() && std::ranges::binary_search(passedOverByScan, inputBlock)) {
            fail("an edge leads into a block that the scan has passed over"_s);
            return;
        }
        if (frame.block != inputBlock)
            setCurrentInputBlock(frame, block, record);
        if (frame.duplicatedValues)
            forgetNamesOfWay(frame);
        m_origin = { .position = Instruction::noPosition, .inlineFrame = frame.inlineFrame, .isTailCall = false, .isPartOfConstruction = false };
        bool severalEdgesEnter = m_openEdges[firstOpenEdge].next != noOpenEdge;
        frame.blockFollowsClosedArms = m_comparesElementsOfViews && frame.loopDepthAtCall;
        for (uint32_t place = firstOpenEdge; frame.blockFollowsClosedArms && place != noOpenEdge; place = m_openEdges[place].next)
            frame.blockFollowsClosedArms = m_openEdges[place].comesFromClosedArm;
        bool isFullyUnrolledHeader = frame.fullyUnrolledHeader == inputBlock;
        bool isPastInlinedHead = frame.inlinedHeadBlocks && !frame.inlinedHeadBlocks->get(inputBlock.index()) && frame.mayLeaveForCall && !block.hasHandler() && !block.isHandlerBlock();
        for (uint32_t place = firstOpenEdge; place != noOpenEdge && isPastInlinedHead; place = m_openEdges[place].next)
            isPastInlinedHead = m_openEdges[place].state.isInlineeEffectFree(frame.inlineeRestartPoint);
        if (block.isHandlerBlock())
            enterHandlerBlock(inputBlock);
        else if (!isFullyUnrolledHeader && !isPastInlinedHead && index.isLoopHead(inputBlock))
            enterLoopHead(inputBlock);
        else if (!isFullyUnrolledHeader && !isPastInlinedHead && severalEdgesEnter && !m_hoistedDiamond && !(m_restArrayAllocator && frame.isOfUnit && inputBlock == m_restArrayAllocationBlock) && walkBlockApart(inputBlock))
            return;
        else {
            enterJoin(inputBlock, record, block);
            if (isFullyUnrolledHeader) {
                record.isEntered = false;
                if (m_frames[frameIndex].turnsOfForIn && !isUnreachable())
                    standAtHeadOfTurnOfForIn(inputBlock);
            }
        }
    } else {
        if (frame.block != inputBlock)
            setCurrentInputBlock(frame, block, record);
        record.isEntered = true;
    }
    if (isUnreachable()) {
        if (m_hoistedDiamond && m_hoistedDiamond->inputJoin == inputBlock) [[unlikely]]
            fail("where the arms walked in front of a loop meet cannot be reached"_s);
        m_block = { };
        return;
    }
    if (m_hoistedDiamond) [[unlikely]]
        finishHoistingConstantDiamond(inputBlock);
    if (block.frequency() == BlockFrequency::Unlikely)
        m_code.makeBlockUnlikely(m_block);
    if (!m_generatorsWalkedInConsumers.isEmpty()) [[unlikely]]
        chooseInlineFrameOfStep();
    if (const BitVector* head = m_frames[frameIndex].inlinedHeadBlocks; head && !head->get(inputBlock.index()) && tryLeaveInlinedHead())
        return;

    if (m_restArrayAllocator && m_frames[frameIndex].isOfUnit && inputBlock == m_restArrayAllocationBlock) [[unlikely]] {
        allocateRestArrays();
        if (isUnreachable()) {
            m_block = { };
            return;
        }
    }

    if (!m_versionedLoops.isEmpty() && !m_frames[frameIndex].firstOperationToWalk)
        readElementsAhead(inputBlock);

    BodyTerminator terminator = block.terminator();
    if (terminator.kind() == TerminatorKind::Throw && !block.hasHandler() && canRestartAtThrow()) {
        m_frames[frameIndex].firstOperationToWalk = 0;
        restartIterationAtThrow();
        return;
    }
    if (m_frameOfFusedChain && terminator.kind() == TerminatorKind::Throw && mayLeaveFusedChainHere()) {
        m_frames[frameIndex].firstOperationToWalk = 0;
        jumpTo(blockWhereFusedChainIsLeft());
        leaveBlockPending();
        return;
    }

    if (block.hasHandler() && has(Mechanism::Handlers))
        noteStateAtHeadForHandler();
    Frame& entered = m_frames[frameIndex];
    entered.walkedUpTo = 0;
    if (block.numberOfValues())
        walkOperationsOf(inputBlock);
    else {
        setCurrentInputBlock(inputBlock);
        entered.firstOperationToWalk = 0;
        entered.hasPassedByWhatFeedsThrow = false;
    }
    walkTerminator(terminator);
}

bool Walker::takesWhatRowOfInputOperandCannotGive(const Draft& draft) const
{
    const BodyIndex& index = *m_frames.last().index;
    if (index.isParameter(draft.inputValueGiven))
        return false;
    BodyValue written = index.value(draft.inputValueGiven);
    if (written.opcode() != draft.opcode || numberOfNamedOperandsOf(draft.opcode) + written.numberOfVariadicOperands() != draft.numberOfOperands())
        return false;
    for (uint32_t operandIndex = 0; operandIndex < draft.numberOfOperands(); ++operandIndex) {
        ValueID operandWritten = written.operand(operandIndex);
        Reference built = draft.operand(operandIndex);
        if (!operandWritten || !built || !built.isRecord() || index.isParameter(operandWritten))
            continue;
        Opcode maker = index.opcodeOf(operandWritten);
        TypeID givenByRow = widestPrimitiveResultOf(maker);
        ValueID judged = built.id();
        if (m_code.at(judged).opcode == Opcode::NumberOrNaN)
            judged = m_code.at(judged).operand(0);
        TypeID recorded = m_code.at(judged).type;
        if (!givenByRow || !recorded || !m_types.at(recorded).isJavaScriptValue() || m_types.isSubset(recorded, givenByRow))
            continue;
        bool isFillerOfHeldSlot = draft.opcode == Opcode::NewObject && m_code.isConstant(judged) && !m_types.overlap(recorded, givenByRow);
        if (isFillerOfHeldSlot)
            continue;
        bool takerMakesTextOfEveryOperandItself = maker == Opcode::ToString && widestPrimitiveResultOf(draft.opcode) == TypeTable::string;
        if (takerMakesTextOfEveryOperandItself)
            continue;
        bool isTruthTakenAsNumber = givenByRow == TypeTable::boolean && m_types.isSubset(recorded, TypeTable::number) && hasOperatorForm(draft.opcode) && draft.form() != OperatorForm::Generic && draft.form() != OperatorForm::Unselected;
        if (isTruthTakenAsNumber)
            continue;
        dataLogLn("AOT: f", currentFunction().index(), " v", draft.inputValueGiven.index(), ": ", nameOf(draft.opcode), " takes ", TypeInText { m_types, recorded }, " as operand ", operandIndex, ", which the input makes by ", nameOf(maker), ": its row gives ", TypeInText { m_types, givenByRow });
        return true;
    }
    return false;
}

bool Walker::enterIterationAtHint()
{
    if (m_frames.last().fullyUnrolledHeader == m_frames.last().block)
        return false;
    splitHeaderTestedAtEnd(m_frames.last().block);
    return true;
}

void Walker::splitHeaderTestedAtEnd(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    if (isUnreachable() || m_hasFailed || m_frames[frameIndex].blocks.isEmpty() || m_frames[frameIndex].blocks[inputHead.index()].head != m_block)
        return;
    std::optional<EndLoopTest> test = findEndLoopTest(*this, inputHead);
    if (!test)
        return;
    BlockID body = addBlock();
    jumpTo(body, { });
    continueIn(body);

    bool isOwnVersion = !m_versionedLoops.isEmpty() && m_versionedLoops.last().frameIndex == frameIndex && m_versionedLoops.last().inputHead == inputHead;
    ValueID parameter = currentBodyIndex().block(inputHead).parameter(test->position);
    Reference counter = referenceOf(parameter);
    Reference bound = isOwnVersion ? boundOfEndLoopTest(inputHead, *test) : Reference();
    if (!isOwnVersion || !m_opensGuesses || !canRestartIteration() || !bound || !counter || !counter.isValue() || !m_types.at(typeOf(counter)).isInteger() || !boundsPositions(*this, *test, bound))
        return;
    Reference guard = buildIntegerOperator(*this, test->relation, counter, bound, m_origin);
    if (!guard || !guard.isValue() || isUnreachable() || m_code.isConstant(guard.id()))
        return;
    count(StatisticID::EqualityLoopTestsNormalized);
    BlockID passed = addBlock();
    BlockID failed = addBlock(BlockFrequency::Rare);
    letGuessFailUnasked(failed, m_origin);
    branch(guard, passed, failed, true);
    continueIn(failed);
    if (!isUnreachable())
        restartIteration();
    continueIn(passed);
    recordSpeculation();
    Draft narrowing(Opcode::Narrowed, m_origin);
    narrowing.setValueOperand(counter);
    narrowing.setTestOperand(guard);
    narrowing.setOperandIndex(OperandIndex::Less::left);
    narrowing.setIsOnTakenEdge(true);
    if (Reference narrowed = build(WTF::move(narrowing)))
        bind(parameter, narrowed);
    inputBlock(frameIndex, inputHead).isGuardedByEndTest = true;
    m_normalizesLoopTests = true;
}

Reference Walker::boundOfEndLoopTest(BlockID inputHead, const EndLoopTest& test)
{
    const BodyIndex& index = currentBodyIndex();
    ValueID written = test.bound ? index.stripCasts(test.bound) : ValueID();
    Reference bound;
    if (written && written.index() < index.block(inputHead).firstParameter().index())
        bound = referenceOf(written);
    else if (!written || (!index.isParameter(written) && index.opcodeOf(written) == Opcode::Integer)) {
        Draft integer(Opcode::Integer, m_origin);
        integer.setInteger(written ? index.value(written).as<Opcode::Integer>().integer() : 0);
        bound = constant(WTF::move(integer));
    }
    return bound && bound.isRecord() && m_types.at(typeOf(bound)).isInteger() ? bound : Reference();
}

bool Walker::branchOnEndTest(const BodyTerminator& terminator)
{
    const BodyIndex& index = currentBodyIndex();
    LoopID loop = m_versionedLoops.isEmpty() ? LoopID() : index.loopOf(terminator.block());
    BlockID inputHead = loop ? index.loop(loop).head : BlockID();
    if (!inputHead || isUnreachable() || hasStringsInBuilders() || m_frames.last().blocks.isEmpty() || !m_frames.last().blocks[inputHead.index()].isGuardedByEndTest)
        return false;
    std::optional<EndLoopTest> test = findEndLoopTest(*this, inputHead);
    BodyTarget target = terminator.targets()[test ? 1 - test->exitTarget : 0];
    if (!test || test->isRelation || test->block != terminator.block())
        return false;
    bool someProofIsRead = false;
    index.forEachEdgeResult(terminator.block(), [&](const BodyEdgeResult& result) {
        someProofIsRead |= !index.isUnused(result.parameter);
    });
    Reference tested = referenceOf(test->stepped);
    Reference bound = boundOfEndLoopTest(inputHead, *test);
    Reference condition = referenceOf(terminator.as<TerminatorKind::Branch>().conditionOperand());
    if (someProofIsRead || !tested || !tested.isValue() || !bound || !condition || !condition.isValue() || !m_types.at(typeOf(tested)).isInteger())
        return false;
    Reference relation = buildIntegerOperator(*this, test->relation, tested, bound, m_origin);
    if (!relation || !relation.isValue() || isUnreachable() || m_code.isConstant(relation.id()))
        return false;
    EdgeToTargetOfBody exit { terminator.block(), test->exitTarget };
    EdgeToTargetOfBody stay { terminator.block(), 1 - test->exitTarget };
    BlockID continuing = addBlock();
    BlockID leaving = addBlock();
    branch(relation, continuing, leaving, true);
    continueIn(continuing);
    Draft narrowing(Opcode::Narrowed, m_origin);
    narrowing.setValueOperand(tested);
    narrowing.setTestOperand(relation);
    narrowing.setOperandIndex(OperandIndex::Less::left);
    narrowing.setIsOnTakenEdge(true);
    Reference narrowed = build(WTF::move(narrowing));
    if (checksBounds() && !isUnreachable())
        checkThatValuesAreTheSame(condition, booleanOfUnit(test->exitTarget), m_origin);
    if (narrowed && narrowed.isValue() && narrowed != tested && target.block() != inputHead)
        noteNarrowerName(target.block(), tested, narrowed);
    else if (narrowed)
        bind(test->stepped, narrowed);
    endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(stay));
    if (m_hasFailed)
        return true;
    continueIn(leaving);
    bind(test->stepped, tested);
    if (checksBounds() && !isUnreachable())
        checkThatValuesAreTheSame(condition, booleanOfUnit(!test->exitTarget), m_origin);
    endBlockLeadingTo(TerminatorDraft(TerminatorKind::Jump, m_origin), singleElementSpan(exit));
    return true;
}

void Walker::walkStretch(BlockID inputBlock, uint32_t begin, uint32_t end)
{
    SetForScope deeper(m_nestingDepth, m_nestingDepth + 1);
    size_t frameIndex = m_frames.size() - 1;
    setCurrentInputBlock(inputBlock);
    const BodyIndex& index = *m_frames[frameIndex].index;
    uint32_t firstValue = index.block(inputBlock).firstValue().index();
    if (end > index.block(inputBlock).numberOfValues()) {
        fail("a stretch ends behind the last operation of its block"_s);
        return;
    }

    uint32_t walkedBefore = std::exchange(m_frames[frameIndex].walkedUpTo, begin);
    uint32_t endBefore = std::exchange(m_frames[frameIndex].endOfStretch, end);
    uint32_t beginBefore = std::exchange(m_frames[frameIndex].beginOfStretch, begin);
    ++m_frames[frameIndex].numberOfStretchesBeingWalked;
    for (uint32_t place = begin; place < end && !isUnreachable() && !m_hasFailed; ++place) {
        if (Reference made = m_operatorsMadeInFrontOfLoops.isEmpty() ? Reference() : nameOfOperatorMadeInFrontOfLoop(ValueID(firstValue + place)))
            bind(ValueID(firstValue + place), made);
        else if (Reference held = m_hoistedRows.isEmpty() ? Reference() : findHoistedFieldInCopy(*this, ValueID(firstValue + place)))
            bind(ValueID(firstValue + place), held);
        else if (place >= m_frames[frameIndex].walkedUpTo) {
            [[clang::always_inline]] walkOperation(index.value(ValueID(firstValue + place)));
            if (m_inputs.options.verifiesCode && index.opcodeOf(ValueID(firstValue + place)) == Opcode::ElemGet) [[unlikely]]
                holdReadToRecordMadeForIt(frameIndex, ValueID(firstValue + place));
        }
        storeValueBehindItsDefinition(ValueID(firstValue + place));
    }
    Frame& frame = m_frames[frameIndex];
    frame.walkedUpTo = std::max({ walkedBefore, frame.walkedUpTo, end });
    frame.endOfStretch = endBefore;
    frame.beginOfStretch = beginBefore;
    --frame.numberOfStretchesBeingWalked;
}

static bool callsHelperWithoutFunctionOfItsOwn(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID callee = call.as<Opcode::Call>().calleeOperand();
    if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID helper = index.value(callee).as<Opcode::Builtin>().builtin();
    return table.at(helper).holder == BuiltinID(Builtins::CompilerHelpers) && std::ranges::all_of(table.overloadSummariesOf(helper), [](const BuiltinOverloadSummary& overload) {
        return overload.kind == BuiltinImplementationKind::HostCall;
    });
}

void Walker::threadReturnOf(BlockID inputBlock)
{
    walkTerminator(m_frames.last().index->block(inputBlock).terminator());
}

template<Opcode known> static ALWAYS_INLINE bool isReadThatOnlyCallUsesInChain(const Walker& walker, const BodyValueOf<known>& value)
{
    if constexpr (opcodesThatOpenLoopOverIteratorThatEngineMakesTakes.contains(known)) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID user = index.onlyUsingValue(value.id());
        return user && index.opcodeOf(user) == Opcode::Call;
    } else
        return false;
}

template<Opcode known> bool Walker::walkThroughChainOf(const BodyValue& walked)
{
    BodyValueOf<known> value(walked);
    if ((known == Opcode::PropGet || known == Opcode::Call || known == Opcode::IsPristineArray) && m_hasTakenGeneratorsSteppedByHand && answerStepOfGeneratorTaken(walked))
        return true;
    if (known == Opcode::PropGet && m_frames.size() > 1 && currentBodyIndex().blockOf(value.id()) == entryBlock && takePassedGeneratorForSteps(walked))
        return answerStepOfGeneratorTaken(walked);
    if (value.opcode() == Opcode::Call && !referenceOf(value.calleeOperand()) && !callStepsGenerator(*this, value) && callMethodThroughTable(*this, value))
        return false;
    if (hasStringsInBuilders() && bindCastOfStringInBuilder(*this, value))
        return false;
    if (value.opcode() == Opcode::Builtin)
        buildFirstReadOfNamedBuiltin(*this, value);
    if (value.opcode() == Opcode::NewObject && leaveEnvironmentAmongVariables(*this, value))
        return false;
    if (value.opcode() == Opcode::NewObject && has(Mechanism::Guesses) && m_opensGuesses && openGuessAtAllocation(*this, value))
        return true;
    if (has(Mechanism::Guesses) && m_opensGuesses && useKeyOfForInBehindTest(*this, value))
        return true;
    if (value.opcode() == Opcode::ElemGet && has(Mechanism::Guesses) && m_opensGuesses && !m_versionedLoops.isEmpty() && tellRowsApartByKindOfStorage(*this, value))
        return true;
    if ((value.opcode() == Opcode::ElemGet || value.opcode() == Opcode::PropGet) && has(Mechanism::Guesses) && m_opensGuesses && !m_versionedLoops.isEmpty() && tryGuardInvariantBaseAsArray(*this, value))
        return true;
    if (has(Mechanism::Guesses) && m_opensGuesses && askRowOnceWhereStretchStoresIntoIt(*this, value))
        return true;
    if (known == Opcode::PropGet && has(Mechanism::Guesses) && m_opensGuesses) {
        if (ReadInArm taken = tryReadInArm(*this, value); taken != ReadInArm::No)
            return taken == ReadInArm::WalkedStretch;
    }
    if (m_readArm && allNumericOperators.contains(known))
        askOperandsInArm(*this, value);
    if (has(Mechanism::Guesses) && m_opensGuesses && guessThatConvertedPartOfTextInBuilderHasText(*this, value))
        return true;
    if (value.opcode() == Opcode::SuperConstructList && has(Mechanism::Guesses) && m_opensGuesses && !findOnlyCallee(*this, value, referenceOf(value.parentOperand()))) {
        if (GuessOpened opened = openGuessAt(*this, value); opened != GuessOpened::None)
            return opened == GuessOpened::WithArms;
    }
    bool isBuilt = (value.opcode() == Opcode::NewArray && has(Mechanism::TypedOperations) && buildLiteralLoopFills(*this, value))
        || (hasURLsThatAreNeverMade() && answerFromURLThatIsNeverMade(*this, value))
        || (m_loopsOverGenerators.hasStepsWrittenByHand && answerFromStepWrittenByHand(*this, value))
        || buildDeferredDefinition(*this, value)
        || (has(Mechanism::Folding) && foldOperation(*this, value))
        || (has(Mechanism::Folding) && foldOperationOnKnownTexts(*this, value))
        || (has(Mechanism::BuiltinCalls) && buildDefinitionByWrittenDescriptor(*this, value))
        || (has(Mechanism::BuiltinCalls) && expandObjectCreateDescriptors(*this, value))
        || (has(Mechanism::BuiltinCalls) && expandObjectCreateAtDefinition(*this, value))
        || (has(Mechanism::EscapeAnalysis) && m_opensGuesses && listStoresOfCopiedConstructor(*this, value))
        || (opcodesOfGeneratorsInLoops.contains(value.opcode()) && has(Mechanism::Inlining) && walkOperationOfGeneratorInLoop(value))
        || buildCoroutineOperation(*this, value)
        || (opcodesThatOpenLoopOverIteratorThatEngineMakesTakes.contains(known) && has(Mechanism::Guesses) && m_opensGuesses && has(Mechanism::TypedOperations) && currentBodyIndex().opcodes().contains(Opcode::IteratorOpen) && isReadThatOnlyCallUsesInChain(*this, value) && openLoopOverIteratorThatEngineMakes(*this, value))
        || (opcodesThatOpenLoopOverIteratorThatEngineMakesTakes.contains(known) && has(Mechanism::Guesses) && m_opensGuesses && has(Mechanism::TypedOperations) && currentBodyIndex().opcodes().contains(Opcode::AppendSpread) && isReadThatOnlyCallUsesInChain(*this, value) && appendMembersInPlaceOfIteratorThatEngineMakes(*this, value))
        || (opcodesThatOpenLoopOverIteratorThatEngineMakesTakes.contains(known) && has(Mechanism::Guesses) && m_opensGuesses && has(Mechanism::TypedOperations) && isReadThatOnlyCallUsesInChain(*this, value) && takeFirstStepOfIteratorThatEngineMakes(*this, value))
        || (opcodesThatOpenLoopOverIteratorThatEngineMakesTakes.contains(known) && has(Mechanism::Guesses) && m_opensGuesses && has(Mechanism::TypedOperations) && isInLoop() && isReadThatOnlyCallUsesInChain(*this, value) && (leaveNextToStep(*this, value) || openTestOfListAtReadOfMethod(*this, value)))
        || (has(Mechanism::EscapeAnalysis) && buildFreshObject(*this, value))
        || (has(Mechanism::Guesses) && m_opensGuesses && buildArmsInFrontOfTypedForms(*this, value))
        || (has(Mechanism::Guesses) && m_opensGuesses && openGuessAtElementOfNumbers(*this, value))
        || (has(Mechanism::BuiltinCalls) && buildOwnKeyList(*this, value))
        || buildCopyOrKeyOrderForm(*this, value)
        || buildCopyWithWhatFollows(*this, value)
        || ((has(Mechanism::BuiltinCalls) || callsHelperWithoutFunctionOfItsOwn(*this, value)) && callBuiltin(*this, value))
        || (opcodesThatInlineCalleeTakes.contains(known) && has(Mechanism::Inlining) && inlineCalleeAsLinkOfChainOfItsOpcodes(value))
        || (has(Mechanism::DirectCalls) && callDirectly(*this, value))
        || (has(Mechanism::DirectCalls) && callProvenAccessor(*this, value))
        || (has(Mechanism::Folding) && unit().role == CodeRole::SlowBody && program().admitsReplacedBuiltins && buildBuiltinThatExecutableNames(*this, value))
        || (has(Mechanism::TypedOperations) && buildTypedOperation(*this, value))
        || (!has(Mechanism::TypedOperations) && reckonsOnWhatItHasBuilt() && buildIterationThatAsksItsListItself(*this, value))
        || (!has(Mechanism::TypedOperations) && reckonsOnWhatItHasBuilt() && buildOperatorOnWhatWalkHasBuilt(*this, value))
        || (value.opcode() == Opcode::PropGet && has(Mechanism::TypedOperations) && has(Mechanism::DirectCalls) && leaveReadOfMethodToTableCall(*this, value));
    if (!isBuilt && value.opcode() == Opcode::PropGet && !m_versionedLoops.isEmpty() && has(Mechanism::Guesses) && m_opensGuesses && tryGuardObjectAtHeader(value))
        isBuilt = buildTypedOperation(*this, value);
    if (!isBuilt && has(Mechanism::Guesses) && m_opensGuesses) {
        GuessOpened opened = openGuessAt(*this, value);
        if (opened == GuessOpened::WithArms)
            return true;
        isBuilt = opened == GuessOpened::AsOneOperation || (value.opcode() == Opcode::FindsDeclaredMember && (askWhetherMemberOfBuiltinIsAsBuilt(*this, value) || askGuardsAboutDeclaredMember(*this, value))) || ((value.opcode() == Opcode::IsAsBorn || value.opcode() == Opcode::IsAsBornExceptForAddedNames) && askCellWhetherItIsAsBorn(*this, value)) || (value.opcode() == Opcode::IsPrototypeChainPlain && askGuardsWhetherPrototypeChainIsPlain(*this, value)) || ((value.opcode() == Opcode::IsPlainInstanceOf || value.opcode() == Opcode::IsPlainHeirOf) && askCellWhetherItIsPlainInstance(*this, value)) || (value.opcode() == Opcode::FindsNoMember && askCellAndGuardsWhetherNoMemberIsFound(*this, value));
        if (!isBuilt && (value.opcode() == Opcode::FindsDeclaredMember || value.opcode() == Opcode::FindsNoMember))
            ++m_genericMemberLookupCount;
    }
    if (!isBuilt && value.opcode() == Opcode::GetSuperBase && has(Mechanism::Guesses) && m_opensGuesses && callThroughSuperBehindGuards(*this, value))
        return true;
    if (!isBuilt && has(Mechanism::Guesses) && askWhetherHolderOfStoreInClosedArmOfLoopIsList(*this, value))
        return true;
    if (!isBuilt && value.opcode() == Opcode::PropGet && has(Mechanism::TypedOperations))
        isBuilt = buildReadWhereSomeReceiversLackField(*this, value);
    if (!isBuilt && has(Mechanism::Guesses) && m_opensGuesses && readBehindTestsOfShape(*this, value))
        return true;
    if (!isBuilt && value.opcode() == Opcode::PropGet && has(Mechanism::Guesses) && m_opensGuesses) {
        GuessOpened opened = openGuessAtOnlyProvenHolderOfName(*this, value);
        if (opened == GuessOpened::WithArms)
            return true;
        isBuilt = opened == GuessOpened::AsOneOperation;
    }
    if (!isBuilt && m_explainsClosedAccessesTo) [[unlikely]]
        explainClosedAccess(*this, value, *m_explainsClosedAccessesTo);
    if (effectRuleOf(known) == EffectRule::Operator && !isBuilt && !m_versionedLoops.isEmpty() && askValueBesideNumberForNumber(value))
        isBuilt = has(Mechanism::TypedOperations) && buildTypedOperation(*this, value);
    if (!isBuilt)
        isBuilt = buildOperatorBesideOneNumber(*this, value) || buildAccessByIndexOffExpectedView(*this, value) || buildReadByIndexOffAnyBase(*this, value) || askPlainVectorBehindRow(*this, value) || tryReadAtSite(*this, value);
    if (!isBuilt && callThroughTargetResolvedOnce(*this, value))
        return false;
    if (!isBuilt && !replaceOperation(*this, value))
        copy(value);
    return false;
}

ALWAYS_INLINE_INTO_OTHER_FILES Walker::ChainOfOpcode Walker::chainOf(Opcode opcode)
{
    static constexpr auto chains = []<size_t... opcodes>(std::index_sequence<opcodes...>) {
        return std::array<ChainOfOpcode, sizeof...(opcodes)> { &Walker::walkThroughChainOf<static_cast<Opcode>(opcodes)>... };
    }(std::make_index_sequence<numberOfEnumerators<Opcode>>());
    return chains[static_cast<size_t>(opcode)];
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Walker::walkThroughChain(const BodyValue& written)
{
    if (accessesUnderComputedKeys.contains(written.opcode()) || !m_versionedLoops.isEmpty()) [[unlikely]]
        return walkThroughChainAfterPreparing(written);
    return (this->*chainOf(written.opcode()))(written);
}

NEVER_INLINE bool Walker::walkThroughChainAfterPreparing(const BodyValue& written)
{
    if (!accessesUnderComputedKeys.contains(written.opcode())) {
        proveWhatLooksCountOnAt(written);
        if (askLesserBoundAt(written))
            return false;
        if (written.opcode() == Opcode::GetModuleVariable && tryHoistKeyVariable(*this, written))
            return false;
        if (written.opcode() == Opcode::TypeTest && tryGuardKeyAsString(*this, written))
            return false;
        if (m_hasHoistedFields && written.opcode() == Opcode::PropGet && takeHoistedUnknownField(*this, written))
            return false;
        if (!m_versionedLoops.isEmpty() && tryAccessAtKeyedAddress(*this, written))
            return false;
        return (this->*chainOf(written.opcode()))(written);
    }
    if (written.opcode() == Opcode::ElemGet && !m_sharedGenericVersions.isEmpty()) {
        if (m_isBuildingInPlace) {
            if (tryReadAtSite(*this, written))
                return false;
        } else if (Reference row = rowRememberedOfReadBeingWalked(*this, written)) {
            bool isHeldAgainstReadInTurn = holdRowRememberedAgainstReadInTurn(*this, written, row);
            bind(written.id(), row);
            return isHeldAgainstReadInTurn;
        } else if (readRowOfCopyAgainInTurn(*this, written))
            return false;
    }
    BodyValue value = written;
    if (accessesUnderComputedKeys.contains(written.opcode())) {
        PropertyKeyID chosen;
        value = accessAsWalked(written, &chosen);
        switch (chosen ? compareKeyOfAccessWithSymbol(written, chosen) : accessesUnderComputedKeys.contains(value.opcode()) ? tellKeyOfAccessApartByItsTexts(written) : KeyOfAccess::IsAnyValue) {
        case KeyOfAccess::IsAnyValue:
            break;
        case KeyOfAccess::IsSymbol:
            if (isUnreachable())
                return false;
            value = accessByName(written, chosen);
            break;
        case KeyOfAccess::ArmsHaveWalkedAccess:
            return true;
        }
    }
    if (!m_versionedLoops.isEmpty()) {
        proveWhatLooksCountOnAt(value);
        if (tryAccessAtKeyedAddress(*this, value))
            return false;
    }
    return (this->*chainOf(value.opcode()))(value);
}

static constexpr OpcodeSet readsThatMemoryAnswers { Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::PropGet, Opcode::PrivateGet, Opcode::ElemGet, Opcode::PropHas, Opcode::PrivateHas };

void Walker::askObjectAgainForGetterThatNameFinds(const BodyValue& read, Reference object, FunctionID getter)
{
    PropertyKeyID key = read.opcode() == Opcode::PropGet && !read.isFreeName() ? read.key() : read.opcode() == Opcode::ElemGet ? keyThatAccessGoesBy(*this, read) : PropertyKeyID();
    if (!checksWalk() || !key || program().at(key).kind == PropertyKeyKind::PrivateName || object != referenceOf(read.objectOperand()) || !facts().linked().mayBeMentionedBy(unit(), getter))
        return;
    askObjectAgainForFunctionThatNameFinds(object, key, getter, m_origin, { }, true);
}

void Walker::askObjectAgainForFunctionThatNameFinds(Reference object, PropertyKeyID key, FunctionID function, Origin origin, Reference onlyWhere, bool isGetter)
{
    if (!object || !object.isRecord() || isUnreachable() || hasFailed())
        return;
    origin.isTailCall = false;
    Draft ask(Opcode::AskObjectAgain, origin);
    ask.setObjectOperand(object);
    ask.setKey(key);
    ask.setFunction(function);
    ask.setFunctionIsGetter(isGetter);
    if (onlyWhere)
        ask.setOnlyWhereOperand(onlyWhere);
    SetForScope buildInPlaceScope(m_isBuildingInPlace, true);
    build(WTF::move(ask));
}

void Walker::askClassAgainForChainAbovePrototype(Reference classObject, FunctionID constructor, Origin origin, Reference onlyWhere)
{
    if (!classObject || !classObject.isRecord() || isUnreachable() || hasFailed() || !facts().linked().mayBeMentionedBy(unit(), constructor))
        return;
    origin.isTailCall = false;
    Draft ask(Opcode::AskObjectAgain, origin);
    ask.setObjectOperand(classObject);
    ask.setKey(program().keyOf(BuiltinNames::prototype));
    ask.setFunction(constructor);
    ask.setAsksChainAbovePrototype(true);
    if (onlyWhere)
        ask.setOnlyWhereOperand(onlyWhere);
    SetForScope buildInPlaceScope(m_isBuildingInPlace, true);
    build(WTF::move(ask));
}

void Walker::askObjectAgainWhereStateHasAnswered(const BodyValue& read, PropertyKeyID key)
{
    Reference object = referenceOf(read.objectOperand());
    Reference answered = referenceOf(read.id());
    TypeID type = answered && answered.isRecord() ? typeOf(answered) : TypeID();
    if (!object || !object.isRecord() || !type || !m_types.at(type).isJavaScriptValue() || (m_types.at(type).primitives & Primitive::Absent) || program().at(key).kind == PropertyKeyKind::PrivateName)
        return;
    auto isCellOfBuiltin = [&](Reference reference) {
        const Operation* record = &m_code.at(reference.id());
        while (record->opcode == Opcode::Narrowed || record->opcode == Opcode::Box || record->opcode == Opcode::Unbox)
            record = &m_code.at(record->valueOperand());
        return record->opcode == Opcode::Builtin;
    };
    if (isCellOfBuiltin(object) || isCellOfBuiltin(answered))
        return;
    if (hasURLsThatAreNeverMade() && standsForMethodOfURLThatIsNeverMade(*this, read))
        return;
    const Operation* made = &m_code.at(answered.id());
    while (made->opcode == Opcode::Narrowed || made->opcode == Opcode::Box || made->opcode == Opcode::Unbox)
        made = &m_code.at(made->valueOperand());
    if (made->opcode == Opcode::NewClosure && (made->capturesOperands().size() != program().at(made->function()).captures.size() || made->closureForm() == ClosureForm::Deferred)) {
        if (facts().linked().mayBeMentionedBy(unit(), made->function()))
            askObjectAgainForFunctionThatNameFinds(object, key, made->function(), m_origin, { }, false);
        return;
    }
    Origin origin = m_origin;
    origin.isTailCall = false;
    Draft ask(Opcode::AskObjectAgain, origin);
    ask.setObjectOperand(object);
    ask.setAnsweredOperand(answered);
    ask.setKey(key);
    ask.setAnswerIsMethod(made->opcode == Opcode::NewClosure && made->isHeldByClosureTable());
    SetForScope buildInPlaceScope(m_isBuildingInPlace, true);
    build(WTF::move(ask));
}

bool Walker::checkWhatIsRememberedOf(const BodyValue& read)
{
    if ((isInLoop() || guardedReadArray()) && ((read.opcode() == Opcode::ElemGet && findHoistedArrayGuard(*this, read)) || readsLengthOfViewTestedInFrontOfLoop(*this, read))) {
        ++m_untestableProofCount;
        return false;
    }
    Reference remembered = referenceOf(read.id());
    TypeID type = remembered ? typeOf(remembered) : TypeID();
    if (!type || !m_types.at(type).isJavaScriptValue() || (m_types.at(type).primitives & Primitive::Absent)) {
        ++m_untestableProofCount;
        return false;
    }
    WalkState believed = m_state;
    auto goOnAsBelieved = makeScopeExit([&] {
        m_state = WTF::move(believed);
        Effects effects = Effects::none();
        effects.mayCollect = true;
        m_state.invalidate(effects);
    });
    {
        SetForScope withoutAnswersOfState(m_mechanisms, withoutMechanismsMissingDependencies(m_mechanisms - Mechanism::Folding));
        SetForScope buildInPlaceScope(m_isBuildingInPlace, true);
        if (walkThroughChain(read)) {
            goOnAsBelieved.release();
            return true;
        }
    }
    Reference found = referenceOf(read.id());
    if (!isInLoop() && !isUnreachable() && !hasFailed() && found && (!found.isValue() || found == remembered)) {
        m_state.clear();
        SetForScope buildInPlaceScope(m_isBuildingInPlace, true);
        SetForScope withoutAnswersOfState(m_mechanisms, withoutMechanismsMissingDependencies(m_mechanisms - Mechanism::Folding));
        if (walkThroughChain(read)) {
            goOnAsBelieved.release();
            return true;
        }
        found = referenceOf(read.id());
    }
    bind(read.id(), remembered);
    bool isAnsweredByWalk = !found || !found.isValue() || found == remembered || comesFromInFrontOfLoopAround(found);
    if (!isUnreachable() && !hasFailed() && isAnsweredByWalk)
        ++m_foldedProofCheckCount;
    if (isUnreachable() || hasFailed() || isAnsweredByWalk)
        return false;
    checkThatValuesAreTheSame(found, remembered, m_origin);
    return false;
}

void Walker::noteClosuresThatAllocationAloneTakes(const BodyValue& allocation)
{
    m_closuresThatAllocationAloneTakes.shrink(0);
    auto made = allocation.as<Opcode::NewObject>();
    if (!plan().layout(made.layout()).environmentOfDeferredClosures)
        return;
    const BodyIndex& index = currentBodyIndex();
    auto note = [&](ValueID operand) {
        ValueID written = operand;
        while (written && !index.isParameter(written) && index.hasOneUse(written) && inputIdentityOpcodes.contains(index.opcodeOf(written)))
            written = index.value(written).operand(0);
        if (!written || index.isParameter(written) || !index.hasOneUse(written) || index.opcodeOf(written) != Opcode::NewClosure)
            return;
        BlockID writtenIn = index.blockOf(written);
        BlockID bornIn = index.blockOf(allocation.id());
        if (writtenIn != bornIn && (index.hasIrreducibleFlow() || index.loopOf(writtenIn) != index.loopOf(bornIn)))
            return;
        if (Reference closure = referenceOf(operand); closure && closure.isValue())
            m_closuresThatAllocationAloneTakes.append(closure);
    };
    for (ValueID operand : made.valuesOperands())
        note(operand);
    for (ValueID operand : m_valuesWrittenBehindBirth)
        note(operand);
}

static constexpr OpcodeSet constantsThatBirthLists { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt };

void Walker::walkWhatIsDefinedBehindBirth(const BodyValue& allocation)
{
    Frame& frame = m_frames.last();
    frame.firstValueListedByBirthInFront = 0;
    frame.endOfValuesListedByBirthInFront = 0;
    m_valuesDefinedBehindBirth.shrink(0);
    m_valuesWrittenBehindBirth.shrink(0);
    m_layoutOfBirthThatListsWhatIsBehind = { };
    auto made = allocation.as<Opcode::NewObject>();
    const PlannedLayout& planned = plan().layout(made.layout());
    if (!planned.environmentOfDeferredClosures || made.fields().size() != made.valuesOperands().size())
        return;

    const BodyIndex& index = currentBodyIndex();
    const Layout& layout = program().at(made.layout());
    uint32_t first = allocation.id().index() + 1;
    uint32_t endOfBlock = index.block(index.blockOf(allocation.id())).endOfValues().index();
    uint32_t end = first;
    Vector<FieldIndex, 16> fieldsBehind;
    size_t numberTaken = 0;
    BitVector isListed(planned.numberOfFields());
    for (FieldIndex field : made.fields())
        isListed.quickSet(field);
    Vector<uint32_t, 64> usesToCome;
    uint32_t numberWithUsesToCome = 0;
    auto noteUse = [&](ValueID used) {
        if (used && used.index() >= first && used.index() - first < usesToCome.size() && usesToCome[used.index() - first] != BodyIndex::maxCountedUses && !--usesToCome[used.index() - first])
            --numberWithUsesToCome;
    };
    for (uint32_t id = first; id < endOfBlock; ++id) {
        BodyValue behind = index.value(ValueID(id));
        bool handsOn = inputIdentityOpcodes.contains(behind.opcode());
        if (behind.opcode() == Opcode::NewClosure || handsOn || constantsThatBirthLists.contains(behind.opcode())) {
            if (handsOn)
                noteUse(behind.operand(0));
            usesToCome.append(index.useCount(ValueID(id)));
            numberWithUsesToCome += !!usesToCome.last();
            continue;
        }
        if (behind.opcode() != Opcode::PropDefine)
            break;
        usesToCome.append(0);
        auto definition = behind.as<Opcode::PropDefine>();
        std::optional<uint32_t> field = definition.objectOperand() == allocation.id() ? layout.findField(definition.key()) : std::nullopt;
        if (!field || !planned.slots[*field].hasStorage() || isListed.quickGet(*field))
            break;
        ValueID written = definition.valueOperand();
        while (written && !index.isParameter(written) && inputIdentityOpcodes.contains(index.opcodeOf(written)))
            written = index.value(written).operand(0);
        if (!written)
            break;
        if (planned.slots[*field].representation == SlotRepresentation::DeferredClosure) {
            if (index.isParameter(written) || index.opcodeOf(written) != Opcode::NewClosure || index.value(written).as<Opcode::NewClosure>().function() != planned.slots[*field].deferredFunction)
                break;
        } else if (!index.isParameter(written) && written.index() >= first && !constantsThatBirthLists.contains(index.opcodeOf(written)))
            break;
        if (hasStringsInBuilders() && isStringInBuilder(*this, nameLastBoundTo(written)))
            break;
        isListed.quickSet(*field);
        fieldsBehind.append(*field);
        m_valuesWrittenBehindBirth.append(definition.valueOperand());
        noteUse(definition.valueOperand());
        if (numberWithUsesToCome)
            continue;
        numberTaken = fieldsBehind.size();
        end = id + 1;
    }
    m_valuesWrittenBehindBirth.shrink(numberTaken);
    if (end == first)
        return;

    for (uint32_t id = first; id < end && !isUnreachable() && !hasFailed(); ++id) {
        if (BodyValue behind = index.value(ValueID(id)); behind.opcode() != Opcode::PropDefine)
            walkOperation(behind);
    }
    if (isUnreachable() || hasFailed())
        return;
    for (size_t place = 0; place < m_valuesWrittenBehindBirth.size(); ++place)
        m_valuesDefinedBehindBirth.append({ fieldsBehind[place], referenceOf(m_valuesWrittenBehindBirth[place]) });
    m_layoutOfBirthThatListsWhatIsBehind = made.layout();
    Frame& frameBehindWalk = m_frames.last();
    frameBehindWalk.firstValueListedByBirthInFront = first;
    frameBehindWalk.endOfValuesListedByBirthInFront = end;
}

FunctionID Walker::calleeListedAt(FunctionID function, ValueID site) const
{
    static constexpr unsigned mostCallsPassedOneByOne = 4;
    CallsListedOfFunction& listed = m_callsListedOfFunction;
    if (listed.function != function) [[unlikely]]
        listed = { function, facts().linked().callsOf(function), 0 };
    std::span<const LinkedCall> calls = listed.calls;
    size_t& next = listed.next;
    bool liesBehind = !next || calls[next - 1].site < site;
    for (unsigned steps = 0; liesBehind && steps < mostCallsPassedOneByOne && next < calls.size() && calls[next].site < site; ++steps)
        ++next;
    if (!liesBehind || (next < calls.size() && calls[next].site < site))
        next = std::ranges::lower_bound(calls, site, { }, &LinkedCall::site) - calls.begin();
    FunctionID callee = next < calls.size() && calls[next].site == site ? calls[next].callee : FunctionID();
    if (m_inputs.options.verifiesCode) [[unlikely]]
        RELEASE_ASSERT(callee == facts().linked().calleeAt(function, site));
    return callee;
}

void Walker::holdReadToRecordMadeForIt(size_t frameIndex, ValueID read)
{
    std::span<const Code::InputOfHoleReadAsNaN> noted = m_code.inputsOfHolesReadAsNaN();
    if (noted.empty() || isUnreachable() || m_hasFailed || frameIndex >= m_frames.size())
        return;
    Reference bound = referenceBoundIn(m_frames[frameIndex], read);
    if (!bound || !bound.isValue())
        return;
    ValueID record = bound.id();
    while (valuePreservingOpcodes.contains(m_code.at(record).opcode))
        record = m_code.at(record).operand(0);
    if (m_code.at(record).opcode != Opcode::GetArrayElement || !m_code.at(record).readsHoleAsNaN())
        return;
    for (ValueID value = bound.id();; value = m_code.at(value).operand(0)) {
        if (std::ranges::contains(m_code.valuesThatFoldedArithmeticGives(), value))
            return;
        if (value == record)
            break;
    }
    auto found = std::ranges::lower_bound(noted, record.index(), { }, [](const Code::InputOfHoleReadAsNaN& input) { return input.read.index(); });
    if (found != noted.end() && found->read == record && found->written == read && found->function == m_frames[frameIndex].function)
        return;
    for (const Code::InputOfHoleReadAsNaN& answered : m_code.readsAnsweredByHolesReadAsNaN()) {
        if (answered.read == record && answered.written == read && answered.function == m_frames[frameIndex].function)
            return;
    }
    m_whatJudgeOfOperandsRefused = "a read of the program is answered by a record that takes NaN for a hole and was made for another read"_s;
    fail(m_whatJudgeOfOperandsRefused, InternalErrorKind::TypeOutOfDate);
}

void Walker::noteReadAnsweredByHoleReadAsNaN(ValueID read, Reference answer)
{
    noteHoleForCaller({ currentFunction(), &currentBodyIndex(), read }, answer);
}

void Walker::noteHoleForCaller(const CallOfCopy& taker, Reference answer)
{
    if (!verifiesCode() || !answer.isValue())
        return;
    ValueID record = answer.id();
    while (valuePreservingOpcodes.contains(m_code.at(record).opcode))
        record = m_code.at(record).operand(0);
    m_code.noteReadAnsweredByHoleReadAsNaN({ record, taker.caller, taker.call });
}

std::optional<Walker::CallOfCopy> Walker::callOfCopy(size_t framesOutward) const
{
    if (m_frames.size() < framesOutward + 2)
        return std::nullopt;
    size_t place = m_frames.size() - 1 - framesOutward;
    const Frame& copy = m_frames[place];
    if (!copy.callCopied || copy.inlinedHeadBlocks || copy.returnsInPlaceOfUnit)
        return std::nullopt;
    if (m_generatorsWalkedInConsumers.containsIf([&](const GeneratorWalkedInConsumer& walked) { return walked.frameOfTurn == place; }))
        return std::nullopt;
    const Frame& caller = m_frames[place - 1];
    ValueID call = copy.callCopied;
    if (call.index() >= caller.index->numberOfValues() || caller.index->isParameter(call) || caller.index->opcodeOf(call) != Opcode::Call)
        return std::nullopt;
    return CallOfCopy { caller.function, caller.index, call };
}

struct StepOfWalkInWalk {
    static constexpr uint8_t isConstantInFormOfUses = 1u << 0;
    static constexpr uint8_t isAccess = 1u << 1;
    static constexpr uint8_t mayTakeArgumentsLast = 1u << 2;
    static constexpr uint8_t mayRunOtherCode = 1u << 3;
};

std::array<uint8_t, numberOfOpcodes> Walker::findStepsOfWalkByOpcode()
{
    using Step = StepOfWalkInWalk;
    std::array<uint8_t, numberOfOpcodes> stepsByOpcode { };
    for (Opcode opcode : allMembersOf<Opcode>) {
        uint8_t steps = 0;
        if (constantsInFormOfUses.contains(opcode))
            steps |= Step::isConstantInFormOfUses;
        if (effectRuleOf(opcode) == EffectRule::Access)
            steps |= Step::isAccess;
        if (opcodesThatMayTakeArgumentsLast.contains(opcode))
            steps |= Step::mayTakeArgumentsLast;
        if (declaredBoundInTable(opcode).mayRunOtherCode() || operandsMayRunCode(opcode))
            steps |= Step::mayRunOtherCode;
        stepsByOpcode[static_cast<unsigned>(opcode)] = steps;
    }
    return stepsByOpcode;
}

NEVER_INLINE bool Walker::summaryListsEscapes(FunctionID function) const
{
    const ThinSummary& summary = facts().summaries().at(function);
    return !summary.makersThatEndWhereTheyLeave.span().empty() || !summary.scopesStoredFirst.span().empty();
}

NEVER_INLINE bool Walker::checkWalkedOperation(const BodyValue& value, const RecordCounts& before, bool isAnsweredByBuilder)
{
    PropertyKeyID keyOfRead = !checksWalk() ? PropertyKeyID() : value.opcode() == Opcode::PropGet && !value.isFreeName() ? value.key() : value.opcode() == Opcode::ElemGet ? keyThatAccessGoesBy(*this, value) : PropertyKeyID();
    if (keyOfRead && !isAnsweredByBuilder && !isUnreachable() && !hasFailed()) {
        bool holdsReadThatIsAskedWhole = false;
        bool hasDoneMoreThanLook = false;
        static constexpr OpcodeSet marksOfNetOnEffects { Opcode::CheckIterationRestart, Opcode::CheckInlineeFallback, Opcode::CheckReloadBypass };
        for (uint32_t index = before.values; index < m_code.numberOfValues(); ++index) {
            const Operation& built = m_code.at(ValueID(index));
            holdsReadThatIsAskedWhole |= built.opcode == Opcode::PropGet || built.opcode == Opcode::ElemGet || built.opcode == Opcode::GetGlobalProperty || built.opcode == Opcode::GetKeyedProperty || built.opcode == Opcode::GetKeyedValueOrAbsent || built.opcode == Opcode::GetValueAtSiteOrAbsent || built.opcode == Opcode::GetOwnValueUnderKeyOrAbsent || built.opcode == Opcode::GetValueUnderKeyOrAbsent || built.opcode == Opcode::GetElementInStorageOrAbsent || ((built.opcode == Opcode::GetSlot || built.opcode == Opcode::GetSlotOrUndefined) && built.level() != KnowledgeLevel::Proven);
            hasDoneMoreThanLook |= (built.opcode == Opcode::AskObjectAgain && built.functionIsGetter()) || (!marksOfNetOnEffects.contains(built.opcode) && (built.traits().mayWriteObservableState() || built.traits().mayRunOtherCode()));
        }
        if (!holdsReadThatIsAskedWhole && !hasDoneMoreThanLook)
            askObjectAgainWhereStateHasAnswered(value, keyOfRead);
    }
    if (checksProofs() && !isAnsweredByBuilder && readsThatMemoryAnswers.contains(value.opcode()) && !isUnreachable() && !hasFailed()) {
        bool hasBuiltOnlyConstants = has(Mechanism::Folding);
        uint32_t numberOfRecordsThatAreNoConstants = 0;
        bool mayBeWalkedAgainUnseen = before.operationsSeenIfRunAgain == m_numberOfOperationsBuiltThatAreSeenIfRunAgain;
        for (uint32_t index = before.values; index < m_code.numberOfValues(); ++index) {
            numberOfRecordsThatAreNoConstants += !m_code.isConstant(ValueID(index));
            Opcode built = m_code.at(ValueID(index)).opcode;
            mayBeWalkedAgainUnseen &= Operation::opcodesWithOverload.contains(built) || (declaredBoundInTable(built).mayBeRunAgainUnseen() && !operandsMayRunCode(built));
        }
        Reference answer = referenceOf(value.id());
        hasBuiltOnlyConstants &= numberOfRecordsThatAreNoConstants == m_numberOfRecordsBuiltToAskGuardsAgain - before.operationsThatAskGuardsAgain;
        if (answer && (hasBuiltOnlyConstants || (mayBeWalkedAgainUnseen && comesFromInFrontOfLoopAround(answer))) && checkWhatIsRememberedOf(value))
            return true;
    }
    return false;
}

ALWAYS_INLINE void Walker::walkMaterializedOperation(const BodyValue& value, uint32_t steps, FunctionID function, uint32_t numberThatMayEnterByValueInFront, bool isAnsweredByBuilder, bool hasBoundOperation)
{
    using Step = StepOfWalkInWalk;
    uint32_t firstRecordOfOperation = m_code.numberOfValues();
    uint32_t iterationRestartCountBefore = m_iterationRestartCount;
    if (checksWalk() || m_positionsWithoutProofs) [[unlikely]] {
        RecordCounts before = recordCounts();
        if (isAnsweredByBuilder)
            before.values = UINT32_MAX;
        if (!hasBoundOperation && walkThroughChain(value))
            return;
        if (checkWalkedOperation(value, before, isAnsweredByBuilder))
            return;
    } else if (!hasBoundOperation && walkThroughChain(value))
        return;

    const LinkTables& linked = facts().linked();
    if (!(steps & Step::mayRunOtherCode) || linked.calls.isEmpty())
        return;
    FunctionID callee = calleeListedAt(function, value.id());
    if (callee && m_numberOfOperationsBuiltThatMayEnterByValue != numberThatMayEnterByValueInFront)
        m_arrivalsHeld.append({ callee, 0, { }, ArrivalHeld::Kind::MayBeEnteredByValue });
    m_numberOfOperationsBuiltThatMayEnterByValue = numberThatMayEnterByValueInFront;
    if (!callee || (m_callIsAccountedFor && m_calleeAccountedFor == callee))
        return;
    if (isUnreachable() && m_iterationRestartCount != iterationRestartCountBefore) {
        bool hasBuiltCall = false;
        for (uint32_t index = firstRecordOfOperation; index < m_code.numberOfValues() && !hasBuiltCall; ++index)
            hasBuiltCall = declaredBoundInTable(m_code.at(ValueID(index)).opcode).mayRunOtherCode();
        if (!hasBuiltCall)
            return;
    }
    leaveAnythingAt(callee);
}

static bool isOnlyStoredByKey(const BodyIndex& index, ValueID read)
{
    ValueID user = index.onlyUsingValue(read);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        read = user;
        user = index.onlyUsingValue(read);
    }
    return user && index.opcodeOf(user) == Opcode::ElemSet && index.value(user).operand(OperandIndex::ElemSet::value) == read && index.value(user).operand(OperandIndex::ElemSet::object) != read && index.value(user).operand(OperandIndex::ElemSet::property) != read;
}

static TypeID typeOfElementUnlessMoved(const BodyValue& read, const Walker& walker)
{
    TypeID type = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), read.id());
    if (!type || (walker.types().at(type).primitives & Primitive::Number) || !isOnlyStoredByKey(walker.currentBodyIndex(), read.id()))
        return type;
    return { };
}

NEVER_INLINE void Walker::materializeOperandsAndWalk(const BodyValue& value, uint32_t steps, FunctionID function, uint32_t numberThatMayEnterByValueInFront)
{
    using Step = StepOfWalkInWalk;
    std::optional<MaterializedStringsScope> strings;
    if (hasStringsInBuilders()) {
        SetForScope stringIsNoPartOfAccess(m_isWalkingAccess, false);
        strings.emplace(*this, value);
        if (isUnreachable())
            return;
    }
    std::optional<ArgumentsMadeWhereTheyLeave> arguments;
    if ((steps & Step::mayTakeArgumentsLast) && has(Mechanism::EscapeAnalysis) && (currentBodyIndex().holdsAnyOf(opcodesThatMakeArguments) || (depthOfCopies() && program().at(function).has(FunctionFlags::HasRestParameter)))) {
        arguments.emplace(*this, value);
        if (isUnreachable())
            return;
    }
    walkMaterializedOperation(value, steps, function, numberThatMayEnterByValueInFront, strings && strings->bindsWhatNoLookHasSeen(), strings && strings->hasBoundOperation());
}

void Walker::walkOperation(const BodyValue& value)
{
    using Step = StepOfWalkInWalk;
    if (!m_trialVisits.isEmpty()) {
        if (walkIsCutShortHere())
            return;
        if (m_operationsBeingWalkedInFrames.size() < m_frames.size())
            m_operationsBeingWalkedInFrames.grow(m_frames.size());
        m_operationsBeingWalkedInFrames[m_frames.size() - 1] = value.id();
        static constexpr OpcodeSet takePositions { Opcode::ElemGet, Opcode::ElemSet, Opcode::Call };
        if (takePositions.contains(value.opcode()))
            notePositions(value);
    }
    ++m_numberOfValuesWalked;
    ++m_numberOfValuesWalkedAndKept;
    m_opcodeBeingWalked = value.opcode();
    const uint32_t steps = m_stepsOfWalkByOpcode[static_cast<unsigned>(value.opcode())];
    if (value.opcode() == Opcode::LoopHint && !enterIterationAtHint()) [[unlikely]]
        return;
    if (m_frames.last().turnsOfForIn && bindListedKey(value)) [[unlikely]]
        return;
    if (m_restArrayAllocator && m_frames.last().isOfUnit && leaveArgumentsObjectToWhereListsAreMade(value)) [[unlikely]]
        return;
    if (value.id().index() >= m_frames.last().firstValueListedByBirthInFront && value.id().index() < m_frames.last().endOfValuesListedByBirthInFront)
        return;
    if (value.opcode() == Opcode::NewObject) {
        walkWhatIsDefinedBehindBirth(value);
        if (isUnreachable() || hasFailed())
            return;
        m_opcodeBeingWalked = value.opcode();
        noteClosuresThatAllocationAloneTakes(value);
    }
    SetForScope valueBeingWalked(m_valueBeingWalked, value.id());
    FunctionID function = currentFunction();
    uint32_t numberThatMayEnterByValueInFront = m_numberOfOperationsBuiltThatMayEnterByValue;
    auto leaveAnythingWhereOperationMayEnterByValue = makeScopeExit([&] {
        if (m_numberOfOperationsBuiltThatMayEnterByValue == numberThatMayEnterByValueInFront)
            return;
        m_numberOfOperationsBuiltThatMayEnterByValue = numberThatMayEnterByValueInFront;
        if (facts().linked().calls.isEmpty())
            return;
        if (FunctionID callee = calleeListedAt(function, value.id()))
            m_arrivalsHeld.append({ callee, 0, { }, ArrivalHeld::Kind::MayBeEnteredByValue });
    });
    m_origin = value.origin();
    m_origin.inlineFrame = m_frames.last().inlineFrame;
    m_callIsAccountedFor = false;
    if (checksProofsOf(ProofSource::Lifetime) && value.id().index() + 1 == m_frames.last().endOfValues && (m_frames.size() == 1 || m_frames.last().returnsInPlaceOfUnit)) {
        endLifetimesAtTailCall(value);
        if (isUnreachable())
            return;
    }
    if (value.opcode() == Opcode::FinishObject) {
        showThatLiteralHoldsWhatIsHeldFilled(value);
        if (isUnreachable())
            return;
        finishBirthWhereLiteralEnds(value);
        if (isUnreachable())
            return;
    }
    const bool listsEscapes = summaryListsEscapes();
    if (listsEscapes) [[unlikely]] {
        if (std::span<const SummaryMakerThatEndsWhereItLeaves> makers = facts().summaries().at(function).makersThatEndWhereTheyLeave.span(); !makers.empty()) {
            showThatMakersThatLeaveAtHoldWhatIsHeldFilled(makers, value.id().index(), false);
            if (isUnreachable())
                return;
        }
    }
    if (steps & Step::isConstantInFormOfUses) {
        copy(value);
        return;
    }
    if (listsEscapes) [[unlikely]] {
        if (std::span<const SummaryScopeStoredFirst> scopes = facts().summaries().at(function).scopesStoredFirst.span(); !scopes.empty()) {
            holdSlotsOfScopesThatLeaveAt(scopes, value.id().index(), false);
            if (isUnreachable())
                return;
        }
    }
    if (std::span<const LinkedVariableStoredFirst> claims = m_frames.last().variablesStoredFirst; !claims.empty()) {
        Vector<FunctionID, 4> used;
        listFunctionsUsedByOperation(program(), firstStores(), currentFunction(), currentBodyIndex(), value, used);
        holdVariablesStoredFirstWhereFunctionsAreUsed(claims, used.span(), value.opcode() == Opcode::PutModuleVariable ? value.variable() : VariableID());
        if (isUnreachable())
            return;
    }
    if (value.opcode() == Opcode::GetModuleVariable) {
        holdVariableOfAnotherFileStoredFirst(value.variable());
        if (isUnreachable())
            return;
    }
    SetForScope isWalkingAccess(m_isWalkingAccess, !!(steps & Step::isAccess));
    SetForScope resultOfRead(m_resultOfReadBeingTyped, value.opcode() == Opcode::ElemGet ? asFarAsUnitMayAsk(typeOfElementUnlessMoved(value, *this)) : TypeID());

    bool takesMadeOperands = hasStringsInBuilders()
        || ((steps & Step::mayTakeArgumentsLast) && has(Mechanism::EscapeAnalysis) && (currentBodyIndex().holdsAnyOf(opcodesThatMakeArguments) || (depthOfCopies() && program().at(function).has(FunctionFlags::HasRestParameter))));
    if (takesMadeOperands) [[unlikely]]
        materializeOperandsAndWalk(value, steps, function, numberThatMayEnterByValueInFront);
    else
        walkMaterializedOperation(value, steps, function, numberThatMayEnterByValueInFront, false, false);
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::walkJump(const BodyTerminator& terminator)
{
    BodyTarget target = terminator.next();
    if (BlockID head = inputBlock(m_frames.last(), target.block()).head; head && target.proofs().empty()) {
        Vector<Reference, 8> arguments;
        for (ValueID argument : target.arguments())
            arguments.append(referenceOf(argument));
        closeBackEdge({ }, head, arguments.span());
        return;
    }

    BlockID from = m_block;
    leadEdge(m_code.endWithJump(from, m_origin), from, terminator, 0, target);
    if (m_hasFailed)
        return;
    m_block = { };
    m_state.clear();
}

static ALWAYS_INLINE bool mayBeTrueOrFalseInWalk(const Walker& walker, Reference condition)
{
    if (walker.hasStringsInBuilders() || !condition || !condition.isValue())
        return false;
    Opcode opcode = walker.code().at(condition.id()).opcode;
    if (opcode == Opcode::Double || opcode == Opcode::String)
        return false;
    TypeID type = walker.typeOf(condition);
    return type && isBooleanType(type) && primitivesInID(type) == Primitive::Boolean;
}

bool Walker::handTurnOverWhereLesserBoundIsReached(const BodyTerminator& terminator)
{
    if (m_versionedLoops.isEmpty())
        return false;
    const VersionedLoop& versionedLoop = m_versionedLoops.last();
    if (!versionedLoop.headAsksLesserBound || versionedLoop.frameIndex != m_frames.size() - 1 || terminator.block() != versionedLoop.inputHead)
        return false;
    Reference condition = referenceOf(terminator.as<TerminatorKind::Branch>().conditionOperand());
    if (!condition || !condition.isValue() || !isAsBranchTakesItInWalk(*this, condition) || !canRestartIteration()) {
        fail("the head of a typed build asks the lesser bound and cannot hand the turn over"_s);
        return true;
    }
    BlockID from = m_block;
    m_code.endWithBranch(from, condition.id(), m_origin);
    m_block = { };
    uint32_t firstEdge = m_code.at(from).terminator.firstEdge.index();
    WalkState atEnd(m_state);
    leadEdge(EdgeID(firstEdge), from, terminator, 0, *terminator.targets().begin());
    if (m_hasFailed)
        return true;
    m_state = WTF::move(atEnd);
    BlockID handsOver = m_code.addBlock(BlockRole::Ordinary, BlockFrequency::Normal);
    m_code.setLoopMarks(handsOver, isInLoop(), false);
    m_code.closeEdge(EdgeID(firstEdge + 1), handsOver, { });
    m_block = handsOver;
    if (Reference bound = m_versionedLoops.last().boundBesideLesserBound) {
        BodyValue relation = currentBodyIndex().value(terminator.as<TerminatorKind::Branch>().conditionOperand());
        Reference goesOn = buildIntegerOperator(*this, relation.opcode(), referenceOf(relation.operand(0)), bound, m_origin);
        if (goesOn && goesOn.isValue() && !isUnreachable()) {
            BlockID reached = m_block;
            m_code.endWithBranch(reached, goesOn.id(), m_origin);
            m_block = { };
            uint32_t firstEdgeBehind = m_code.at(reached).terminator.firstEdge.index();
            WalkState whereItIsReached(m_state);
            leadEdge(EdgeID(firstEdgeBehind + 1), reached, terminator, 1, *++terminator.targets().begin());
            if (m_hasFailed)
                return true;
            m_state = WTF::move(whereItIsReached);
            m_block = m_code.addBlock(BlockRole::Ordinary, BlockFrequency::Rare);
            m_code.setLoopMarks(m_block, isInLoop(), false);
            m_code.closeEdge(EdgeID(firstEdgeBehind), m_block, { });
        }
    }
    restartIteration();
    m_block = { };
    m_state.clear();
    return true;
}

void Walker::walkTerminator(const BodyTerminator& terminator)
{
    if (walkIsCutShortHere() || isUnreachable() || m_hasFailed)
        return;
    size_t frameIndex = m_frames.size() - 1;
    m_origin = terminator.origin();
    m_origin.inlineFrame = m_frames.last().inlineFrame;
    if (summaryListsEscapes()) [[unlikely]] {
        if (std::span<const SummaryMakerThatEndsWhereItLeaves> makers = facts().summaries().at(currentFunction()).makersThatEndWhereTheyLeave.span(); !makers.empty()) {
            showThatMakersThatLeaveAtHoldWhatIsHeldFilled(makers, m_frames[frameIndex].index->block(m_frames[frameIndex].block).endOfValues().index(), true);
            if (isUnreachable())
                return;
        }
        if (std::span<const SummaryScopeStoredFirst> scopes = facts().summaries().at(currentFunction()).scopesStoredFirst.span(); !scopes.empty()) {
            holdSlotsOfScopesThatLeaveAt(scopes, m_frames[frameIndex].index->block(m_frames[frameIndex].block).endOfValues().index(), true);
            if (isUnreachable())
                return;
        }
    }
    if (std::span<const LinkedVariableStoredFirst> claims = m_frames[frameIndex].variablesStoredFirst; !claims.empty()) {
        Vector<FunctionID, 4> used;
        listFunctionsUsedBy(program(), firstStores(), currentFunction(), currentBodyIndex(), terminator, true, 0, used);
        holdVariablesStoredFirstWhereFunctionsAreUsed(claims, used.span(), VariableID());
        if (isUnreachable())
            return;
    }
    if (hasStringsInBuilders())
        materializeReturnedStringInBuilder(*this, terminator);

    if (terminator.kind() == TerminatorKind::ReturnCall) {
        for (TailCast cast : terminator.as<TerminatorKind::ReturnCall>().casts())
            checkInt32Promise(*this, cast.declared, terminator.operand(0), m_origin);
    }

    if (terminator.kind() == TerminatorKind::Jump && checksProofsOf(ProofSource::Lifetime)) {
        endLifetimesAtJump(terminator.next());
        if (isUnreachable())
            return;
    }
    if (terminator.kind() == TerminatorKind::Jump && !hasStringsInBuilders()) {
        walkJump(terminator);
        return;
    }
    if (terminator.kind() == TerminatorKind::Branch) {
        if (!m_unswitchCollectors.isEmpty()) [[unlikely]] {
            if (isInUnswitchedVersion() && tryUnswitchBranch(terminator))
                return;
            noteUnswitchCandidate(terminator);
        }
        const BodyIndex& index = *m_frames.last().index;
        ValueID asked = terminator.as<TerminatorKind::Branch>().conditionOperand();
        if (asked == m_threadedCondition && asked && frameIndex == m_threadedConditionFrame) [[unlikely]]
            askQuestionOfTruthAgainWhereWaysAnswerIt(index.value(asked));
        Opcode opcodeAsked = asked ? index.opcodeOf(asked) : Opcode::Parameter;
        if (opcodeAsked == Opcode::StrictEq)
            askShapeInPlaceOfTagAtBranch(terminator);
        else if (opcodeAsked == Opcode::ToBoolean) {
            ValueID tested = index.value(asked).operand(OperandIndex::ToBoolean::value);
            Opcode opcodeTested = tested ? index.opcodeOf(tested) : Opcode::Parameter;
            if ((opcodeTested == Opcode::StrictEq || opcodeTested == Opcode::LogicalNot) && endBlockInAnotherWay(terminator))
                return;
            if (opcodeTested == Opcode::PropGet)
                askShapeInPlaceOfTagAtBranch(terminator);
        }
    }
    switch (terminator.kind()) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        if (!m_frames.last().environmentsMade.isEmpty() && terminator.kind() == TerminatorKind::Return)
            handBackEnvironmentsAtReturn(*this, terminator.block());
        if (!frameIndex || m_frames[frameIndex].returnsInPlaceOfUnit)
            walkReturn(terminator);
        else
            noteReturnOfCopy(terminator);
        return;
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        fail("a body holds a terminator that only the walk builds"_s);
        return;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
        break;
    }

    std::optional<MaterializedStringsScope> strings;
    if (hasStringsInBuilders() && terminatorUsesStringInBuilder(*this, terminator)) {
        strings.emplace(*this, terminator);
        if (isUnreachable())
            return;
        if (terminatorUsesStringInBuilder(*this, terminator)) {
            fail("a terminator is decided by a text in its builder"_s);
            return;
        }
    }
    if (!strings && leaveBlockAnotherWay(terminator))
        return;
    bool isBranch = terminator.kind() == TerminatorKind::Branch;
    std::optional<TerminatorDraft> draft;
    Reference condition;
    bool lacksOperand = false;
    auto builtFor = [&](ValueID operand) {
        Reference reference = referenceOf(operand);
        lacksOperand |= !reference;
        return reference;
    };
    if (isBranch) {
        if (ValueID written = terminator.as<TerminatorKind::Branch>().conditionOperand()) {
            condition = referenceOf(written);
            lacksOperand = !condition;
        }
    } else {
        draft.emplace(terminator.kind(), m_origin);
        copyFields(terminator, *draft, builtFor);
    }
    if (lacksOperand) {
        fail("a terminator takes a value of the body that nothing was built for"_s);
        return;
    }

    std::optional<uint32_t> onlyTarget;
    if (has(Mechanism::Folding)) {
        onlyTarget = isBranch && mayBeTrueOrFalseInWalk(*this, condition) ? onlyTargetDecidedOnWay(*this, terminator) : onlyTargetKnownToFolding(*this, terminator);
    } else
        onlyTarget = onlyTargetOfWrittenConstants(*this, terminator);
    if (onlyTarget && draft)
        draft->become(TerminatorKind::Jump);

    if (isBranch && !onlyTarget && isAsBranchTakesItInWalk(*this, condition)) {
        RELEASE_ASSERT(terminator.numberOfTargets() == 2);
        if (!m_openLoops.isEmpty() && condition.isValue() && m_code.blockOf(condition.id()) != m_block)
            beginHoistingConstantDiamond(terminator, condition);
        BlockID from = m_block;
        m_code.endWithBranch(from, condition.id(), m_origin);
        m_block = { };
        uint32_t firstEdge = m_code.at(from).terminator.firstEdge.index();
        BodyTargets::Iterator target = terminator.targets().begin();
        WalkState atEnd(m_state);
        leadEdge(EdgeID(firstEdge), from, terminator, 0, *target);
        if (m_hasFailed)
            return;
        m_state = WTF::move(atEnd);
        leadEdge(EdgeID(firstEdge + 1), from, terminator, 1, *++target);
        if (m_hasFailed)
            return;
        m_block = { };
        m_state.clear();
        return;
    }

    std::optional<SwitchOnNumbers> switchOnNumbers = !onlyTarget && terminator.hasCases() ? planSwitchOnNumbers(*this, terminator) : std::nullopt;
    if (switchOnNumbers && !switchOnNumbers->cases.isEmpty()) {
        draft->become(TerminatorKind::Switch);
        draft->setCases(switchOnNumbers->cases.span());
    }
    if (switchOnNumbers && switchOnNumbers->subject)
        draft->setSubjectOperand(switchOnNumbers->subject);
    strings.reset();

    if (draft ? draft->kind == TerminatorKind::Jump : onlyTarget.has_value()) {
        BodyTarget target = terminator.targets()[onlyTarget.value_or(0)];
        if (BlockID head = inputBlock(frameIndex, target.block()).head; head && target.proofs().empty()) {
            Vector<Reference, 8> arguments;
            for (ValueID argument : target.arguments())
                arguments.append(referenceOf(argument));
            if (hasStringsInBuilders() && edgeExitsBuilderLoop(*this, target.block())) {
                StringsFinishedOnEdge textsTakenOut;
                finishStringsInBuildersOnEdge(*this, target.block(), target, arguments, textsTakenOut);
                if (isUnreachable())
                    return;
            }
            closeBackEdge({ }, head, arguments.span());
            return;
        }
    }

    if (isBranch && !onlyTarget && !isAsBranchTakesItInWalk(*this, condition)) {
        draft.emplace(TerminatorKind::Branch, m_origin);
        draft->setConditionOperand(condition);
    }
    if (draft && !operandsAreAsTaken(*draft)) {
        askTruthOfOperandsNotProvenBoolean(*draft);
        if (isUnreachable() || !checkOperandKinds(*draft))
            return;
        convertOperands(*draft);
        if (isUnreachable())
            return;
    }
    if (draft && !isDeclaredPure(draft->kind)) {
        Effects effects = effectsOfTerminator(typing(), *draft, typesOfOperands(*draft));
        if (effects.mayCollect && isUnderHandler())
            effects.mayThrow |= ThrowEffect::onlyForLackOfMemory();
        if (effects.mayThrow) {
            leadHandlerEdge();
            if (isUnreachable())
                return;
        }
        recordTrialEffects(effects);
        m_state.invalidate(effects);
        if (effects.mayThrow && has(Mechanism::Handlers))
            noteStateAtThrowForHandler(draft->operands());
    }
    if (draft && checksProofsOf(ProofSource::Lifetime) && (draft->kind == TerminatorKind::Throw || draft->kind == TerminatorKind::Rethrow)) {
        endLifetimesAtThrow(draft->operands());
        if (isUnreachable())
            return;
    }

    BlockID from = m_block;
    uint32_t numberOfEdges = onlyTarget ? 1 : terminator.numberOfTargets();
    if (draft)
        m_code.end(from, *draft, numberOfEdges);
    else if (onlyTarget)
        m_code.endWithJump(from, m_origin);
    else {
        RELEASE_ASSERT(numberOfEdges == 2);
        m_code.endWithBranch(from, condition.id(), m_origin);
    }
    m_block = { };
    if (!numberOfEdges)
        return;

    uint32_t firstEdge = m_code.at(from).terminator.firstEdge.index();
    WalkState atEnd;

    uint32_t targetIndex = 0;
    uint32_t numberOfEdgesLed = 0;
    for (BodyTarget target : terminator.targets()) {
        uint32_t indexOfTarget = targetIndex++;
        if (onlyTarget && *onlyTarget != indexOfTarget)
            continue;
        EdgeID edge(firstEdge + numberOfEdgesLed++);
        if (numberOfEdgesLed == numberOfEdges) {
            if (numberOfEdges > 1)
                m_state = WTF::move(atEnd);
        } else if (numberOfEdgesLed == 1)
            atEnd = m_state;
        else
            m_state = atEnd;
        leadEdge(edge, from, terminator, indexOfTarget, target);
        if (m_hasFailed)
            return;
    }
    m_block = { };
    m_state.clear();
}

static bool takesClosureThatMayLieInFrameInWalk(const Walker& walker, const Operation& call)
{
    if (call.opcode != Opcode::CallDirect || call.numberOfBoundArguments() || !walker.has(Mechanism::CellsInFrames))
        return false;
    const Code& code = walker.code();
    std::span<const ValueID> arguments = call.argumentsOperands();
    for (uint32_t position = 0; position < arguments.size(); ++position) {
        ValueID cell = arguments[position] ? underlyingCellOf(code, arguments[position]) : ValueID();
        if (!cell || code.isConstant(cell) || code.at(cell).opcode != Opcode::NewClosure || code.at(cell).blockIndex != call.blockIndex)
            continue;
        if (walker.facts().linked().onlyCallsWhatArrivesAt(call.function(), position))
            return true;
    }
    return false;
}

bool Walker::staysTailCall(Reference call) const
{
    if ((!returnsResultOfFunction(unit().role) && !m_restArrayAllocator) || isUnderHandler() || m_code.at(m_block).handler || m_code.holdsFrameCells())
        return false;
    if (plan().moduleEvaluation().moduleThatBeginsIn(unit().function))
        return false;
    if (!call.isValue() || m_code.lastOperationOf(m_block) != call.id())
        return false;
    const Operation& record = m_code.at(call.id());
    if (!callsWithTailFormInWalk.contains(record.opcode) || !record.isTailCall)
        return false;
    if (requirementOfTailCall(*this, call) == TailCallRequirement::Allowed && takesClosureThatMayLieInFrameInWalk(*this, record))
        return false;
    if (record.opcode == Opcode::Call) {
        ValueID callee = record.calleeOperand();
        while (cellPreservingOpcodes.contains(m_code.at(callee).opcode))
            callee = m_code.at(callee).operand(0);
        bool entersNativeCode = m_code.at(callee).opcode == Opcode::Builtin && std::ranges::none_of(program().builtins().overloadSummariesOf(m_code.at(callee).builtin()), [](const BuiltinOverloadSummary& row) {
            return row.callee == BuiltinCallee::Function;
        });
        if (entersNativeCode)
            return false;
    }

    bool mayNeedReturn = plan().resultsPassNoReturn(unit().function) || plan().endsItsTailCalls(unit().function);
    if (TypeID held = mayNeedReturn ? plan().typeHeldOfResult(program(), facts().linked(), unit().function) : TypeID()) {
        TypeID heldByCallee = record.opcode == Opcode::CallDirect ? plan().typeHeldOfResult(program(), facts().linked(), record.function()) : TypeID();
        if (!heldByCallee || !m_types.isSubset(heldByCallee, held))
            return false;
    }

    const CallingConventions& conventions = plan().conventions();
    if (conventions.numberOfResults(unit()) != 1 || conventions.savesRegistersOfCallers(unit()))
        return false;
    Representation returned = Representation::JSValue;
    if (record.opcode == Opcode::CallDirect) {
        CodeUnit entered = conventions.unitEnteredBy(record);
        if (conventions.numberOfResults(entered) != 1)
            return false;
        returned = conventions.resultRepresentation(entered);
    } else if (record.opcode == Opcode::CallTable)
        returned = conventions.methodSelector(record.selector()).result;
    return returned == conventions.resultRepresentation(unit()) && record.representation == returned;
}

ALWAYS_INLINE_INTO_OTHER_FILES void Walker::walkReturn(const BodyTerminator& terminator)
{
    Reference returned;
    if (ValueID value = terminator.operand(0)) {
        returned = referenceOf(value);
        if (!returned) {
            fail("a return hands on a value of the body that nothing was built for"_s);
            return;
        }
    }

    if (terminator.kind() == TerminatorKind::Return) {
        beginGuardsOfClassesAtEndOfModule(*this, m_origin);
        returned = allocateFilledObjectAtReturn(*this, returned, m_origin);
        finishBirthAtReturnOfClassConstructor(*this, returned, m_origin);
        showWhatIsHeldFilledAtReturnOfClassConstructor(*this, returned, m_origin);
        if (isUnreachable())
            return;
    }
    if (terminator.kind() == TerminatorKind::ReturnCall)
        returnAtOnce(returned);
    else
        returnFromUnit(returned);
}

void Walker::noteReturnOfCopy(const BodyTerminator& terminator)
{
    Frame& frame = m_frames.last();
    Reference value;
    if (ValueID returned = terminator.operand(0)) {
        value = referenceOf(returned);
        if (!value) {
            fail("a return hands on a value of the body that nothing was built for"_s);
            return;
        }
    }
    if (!frame.returnsOfCopy) {
        fail("a return of a callee is walked where nobody goes on behind the call"_s);
        return;
    }
    if (terminator.kind() == TerminatorKind::Return) {
        value = allocateFilledObjectAtReturn(*this, value, m_origin);
        finishBirthAtReturnOfClassConstructor(*this, value, m_origin);
        showWhatIsHeldFilledAtReturnOfClassConstructor(*this, value, m_origin);
        if (isUnreachable())
            return;
    }
    if (!m_generatorsWalkedInConsumers.isEmpty())
        noteCarriedValuesAtReturnOfGenerator();
    endFreshCellLifetimes(singleElementSpan(value), m_origin, frame.numberOfValuesBuiltBefore);
    if (isUnreachable())
        return;
    frame.returnsOfCopy->append(Frame::ReturnOfCopy { m_block, value, WTF::move(m_state), frame.index->block(terminator.block()).hasHandler(), !m_opensGuesses || (frame.blockFollowsClosedArms && isInLoop()) });
    m_block = { };
    m_state.clear();
}

void Walker::returnAtOnce(Reference call)
{
    if (isUnreachable())
        return;
    Origin origin = m_origin;
    origin.isTailCall = false;
    SetForScope originOfReturn(m_origin, origin);

    if (staysTailCall(call)) {
        TerminatorDraft draft(TerminatorKind::ReturnCall, m_origin);
        draft.setCallOperand(call);
        draft.setRequirement(requirementOfTailCall(*this, call));
        m_code.end(m_block, draft, 0);
        m_block = { };
        return;
    }
    returnFromUnit(call);
}

void Walker::returnFromUnit(Reference returned)
{
    if (ModuleID module = plan().moduleEvaluation().moduleThatBeginsIn(unit().function)) {
        Draft finish(Opcode::FinishModuleEvaluation, m_origin);
        finish.setModule(module);
        chooseConservativeFields(finish);
        build(WTF::move(finish));
    }
    returned = convertReturnedValueOfCoroutine(*this, returned, m_origin);
    if (isUnreachable())
        return;
    if (!returned)
        returned = constant(Draft(Opcode::Undefined, m_origin));

    unsigned numberOfResults = plan().conventions().numberOfResults(unit());
    if (numberOfResults == 1) {
        endWithReturn(singleElementSpan(returned), m_origin);
        return;
    }

    Vector<Reference, 4> values;
    if (const Operation& record = m_code.at(returned.id()); record.opcode == Opcode::Results && record.valuesOperands().size() == numberOfResults) {
        for (ValueID value : record.valuesOperands())
            values.append(referenceTo(value));
    } else {
        for (unsigned index = 0; index < numberOfResults; ++index) {
            Draft result(Opcode::ResultAt, m_origin);
            result.setResultsOperand(returned);
            result.setIndex(index);
            values.append(build(WTF::move(result)));
            if (!values.last())
                return;
        }
    }
    endWithReturn(values.span(), m_origin);
}

Reference Walker::holdResultToItsType(Reference value, Origin origin)
{
    constexpr RepresentationSet rawFormsOfResults { Representation::Boolean, Representation::Int32, Representation::Int64, Representation::Double, Representation::OptionalInt64, Representation::OptionalDouble };
    TypeID held = plan().typeHeldOfResult(program(), facts().linked(), m_restArrayAllocator ? CodeUnit { unit().function, CodeRole::Body } : unit());
    if (held && !m_types.at(held).isJavaScriptValue())
        held = { };
    if (Representation form = plan().conventions().resultRepresentation(unit()); rawFormsOfResults.contains(form) && !isLegalRepresentation(m_types.at(typeOf(value)), form))
        return proveOrTrap(value, held ? m_types.testablePartOf(held) : widestTypeOf(form), ProofSource::ResultOfCall, origin);
    bool asksFromNothing = false;
    if (checksProofsOf(ProofSource::ResultOfCall)) {
        Representation formOfValue = representationOf(value);
        bool hasBox = formOfValue == Representation::JSValue || formOfValue == Representation::Cell || formOfValue == Representation::Cell32;
        ResultRule ruleOfMaker = value.isValue() ? resultRuleOf(m_code.at(underlyingCellOf(m_code, value.id())).opcode) : ResultRule::Constant;
        asksFromNothing = hasBox && ruleOfMaker != ResultRule::Allocation && ruleOfMaker != ResultRule::Constant;
    }
    if (held && value.isRecord() && (asksFromNothing || !isShown(value, held)))
        buildCheckOfProof(value, held, asksFromNothing ? TypeWithoutProof(TypeTable::anythingOrAbsent) : TypeWithoutProof::restingOnBelief(typeShown(value)), ProofSource::ResultOfCall, origin, true);
    return isUnreachable() ? Reference() : value;
}

void Walker::endFreshCellLifetimes(std::span<const Reference> liveValues, Origin origin, uint32_t numberOfValuesBuiltBefore)
{
    if (!checksProofsOf(ProofSource::Lifetime) || !m_state.holdsFreshObjects())
        return;

    BitVector liveCells(m_code.numberOfValues());
    for (Reference value : liveValues) {
        if (value && value.isValue())
            liveCells.quickSet(underlyingCellOf(m_code, value.id()).index());
    }

    Vector<Reference, 8> cells;
    m_state.forEachFreshObject([&](Reference object) {
        if (object.id().index() < numberOfValuesBuiltBefore || liveCells.quickGet(object.id().index()))
            return;
        const Operation& made = m_code.at(object.id());
        bool isHeldByCallerToo = made.opcode == Opcode::Parameter && !m_mergedFreshObjects.get(object.id().index());
        if (!isHeldByCallerToo && made.opcode != Opcode::NewClosure && made.representation == Representation::Cell && !(made.opcode == Opcode::NewObject && program().at(made.layout()).isEnvironment()))
            cells.append(object);
        else
            ++m_skippedFreshCellCount;
    });

    origin.isTailCall = false;
    for (Reference cell : cells) {
        Draft end(Opcode::EndCellLifetime, origin);
        end.setCellOperand(cell);
        build(WTF::move(end));
        if (isUnreachable())
            return;
        m_state.markEscaped(cell);
        ++m_numbersOfProofsChecked[static_cast<unsigned>(ProofSource::Lifetime)];
    }
}

bool Walker::shouldEndLifetimesOnEdges() const
{
    return checksProofsOf(ProofSource::Lifetime) && m_state.holdsFreshObjects() && !program().at(currentFunction()).isCoroutine();
}

bool Walker::standsForFrameOfGenerator(Reference state) const
{
    return m_generatorsTakenByConsumers.containsIf([&](const GeneratorTakenByConsumer& taken) { return taken.state == state; })
        || m_generatorsWalkedInConsumers.containsIf([&](const GeneratorWalkedInConsumer& walked) { return walked.state == state; });
}

void Walker::endUnreferencedCellLifetimes(Vector<Reference, 16>& named, uint32_t numberOfValuesBuiltBefore)
{
    bool everyNameIsBuilt = true;
    for (size_t place = named.size(); place--;) {
        Reference name = named[place];
        if (name.isRecord())
            continue;
        if (!name.isVirtual() || standsForFrameOfGenerator(name))
            return;
        indexedIteration(name).forEachValueHeld([&](Reference value) {
            everyNameIsBuilt &= !value || value.isRecord();
            named.append(value);
        });
    }
    if (!everyNameIsBuilt)
        return;
    m_state.forEachValueHeldByAnother([&](Reference value) {
        if (value.isValue())
            named.append(value);
    });
    endFreshCellLifetimes(named.span(), m_origin, numberOfValuesBuiltBefore);
}

void Walker::endLifetimesAtJump(const BodyTarget& target)
{
    if (!shouldEndLifetimesOnEdges())
        return;
    const Frame& frame = m_frames.last();
    if (!m_liveness.isComputedFor(*frame.index))
        m_liveness.compute(*frame.index);
    Vector<Reference, 16> named;
    auto note = [&](ValueID value) {
        if (Reference name = referenceOf(value))
            named.append(name);
    };
    m_liveness.forEachLiveAtHead(target.block(), note);
    for (ValueID argument : target.arguments())
        note(argument);
    endUnreferencedCellLifetimes(named, m_frames.size() == 1 ? 0 : frame.numberOfValuesBuiltBefore);
}

void Walker::endLifetimesAtThrow(std::span<const Reference> thrown)
{
    if (!shouldEndLifetimesOnEdges() || isUnderHandler())
        return;
    Vector<Reference, 16> named;
    named.append(thrown);
    endUnreferencedCellLifetimes(named, 0);
}

void Walker::endLifetimesAtTailCall(const BodyValue& call)
{
    if (!shouldEndLifetimesOnEdges() || isUnderHandler())
        return;
    const Frame& frame = m_frames.last();
    BodyTerminator end = frame.index->terminator(frame.block);
    if (end.kind() != TerminatorKind::ReturnCall || end.operand(0) != call.id())
        return;
    if (!m_liveness.isComputedFor(*frame.index))
        m_liveness.compute(*frame.index);
    Vector<Reference, 16> named;
    m_liveness.forEachLiveBefore(call.id(), frame.block, [&](ValueID value) {
        if (Reference name = referenceOf(value))
            named.append(name);
    });
    endUnreferencedCellLifetimes(named, 0);
}

void Walker::endWithReturn(std::span<const Reference> values, Origin origin)
{
    if (isUnreachable())
        return;
    const CallingConventions& conventions = plan().conventions();
    if (values.size() != conventions.numberOfResults(unit())) {
        fail("a return hands over another number of values than the unit has results"_s);
        return;
    }

    Reference returned;
    if (values.size() == 1) {
        returned = holdResultToItsType(values[0], origin);
        if (!returned || isUnreachable())
            return;
    } else {
        Vector<Reference, 4> placed;
        for (unsigned index = 0; index < values.size(); ++index) {
            placed.append(convert(values[index], conventions.resultRepresentation(unit(), index), origin));
            if (!placed.last() || isUnreachable())
                return;
        }
        Draft results(Opcode::Results, origin);
        results.setValuesOperands(placed.span());
        returned = build(WTF::move(results));
        if (!returned)
            return;
    }

    endFreshCellLifetimes(values, origin, 0);
    if (isUnreachable())
        return;
    TerminatorDraft draft(TerminatorKind::Return, origin);
    draft.setValueOperand(returned);
    endWithoutEdges(draft);
}

void Walker::endWithThrow(Reference thrown, Origin origin)
{
    TerminatorDraft draft(TerminatorKind::Throw, origin);
    draft.setValueOperand(thrown);
    endWithoutEdges(draft);
}

void Walker::endWithoutEdges(TerminatorDraft& draft)
{
    if (isUnreachable() || !checkOperandKinds(draft))
        return;
    convertOperands(draft);
    if (isUnreachable())
        return;
    if (!isDeclaredPure(draft.kind) && effectsOfTerminator(typing(), draft, typesOfOperands(draft)).mayThrow) {
        leadHandlerEdge();
        if (isUnreachable())
            return;
        if (has(Mechanism::Handlers))
            noteStateAtThrowForHandler(draft.operands());
    }
    m_code.end(m_block, draft, 0);
    m_block = { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
