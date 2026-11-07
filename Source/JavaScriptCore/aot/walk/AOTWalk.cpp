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
#include "AOTBuiltinMembers.h"
#include "AOTCallingConventions.h"
#include "AOTEffectsNet.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTModuleEvaluationPlan.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTOperatorForms.h"
#include "AOTPlan.h"
#include "AOTPrimitives.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTSweep.h"
#include "AOTTypeNarrowing.h"
#include "AOTTypeText.h"
#include "AOTWalkCoroutinePositions.h"
#include "AOTWalkCoroutinePromises.h"
#include "AOTWalkCoroutineCalls.h"
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

static constexpr OpcodeSet callsWithTailFormInWalk { Opcode::Call, Opcode::CallList, Opcode::CallDirect };

namespace {

enum class WhoAnswersInWalk : uint8_t { LinkedTablesUnread, LinkedTables, HeldTablesUnread, HeldTables, Nobody };
static constexpr unsigned numberWhoAnswerInWalk = static_cast<unsigned>(WhoAnswersInWalk::Nobody) + 1;
static constexpr unsigned numberOfQuestionsAtTwoLevelsInWalk = 4;
static constexpr unsigned numberOfQuestionsAtOneLevelInWalk = 3;
static_assert(static_cast<unsigned>(AnswerKeptInArena::FirstOfWalk) + (2 * numberOfQuestionsAtTwoLevelsInWalk + numberOfQuestionsAtOneLevelInWalk) * numberWhoAnswerInWalk <= static_cast<unsigned>(AnswerKeptInArena::CellsOfFieldArm));
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
    TypeID contents(TypeLevel, ObjectID, ContentsSlot) const final { return { }; }
    TypeID result(TypeLevel, FunctionID, uint32_t) const final { return { }; }
    TypeID entryParameter(TypeLevel, FunctionID, uint32_t) const final { return { }; }
    TypeID resultOfCallBeingTyped(TypeLevel, uint32_t) const final { return { }; }
    TypeID askedOfEachObject(QuestionOfEachObject question, TypeLevel level, ObjectSetID objects, uint32_t word, NOESCAPE const ScopedLambda<TypeID()>& askEachObject) const final { return askOnceForBuild(m_types, WhoAnswersInWalk::Nobody, static_cast<unsigned>(question), level, objects, word, askEachObject); }
    TypeID askedOfEachLayout(QuestionOfEachLayout question, TypeLevel level, LayoutSetID layouts, PropertyKeyID key, NOESCAPE const ScopedLambda<TypeID()>& askEachLayout) const final { return askOnceForBuild(m_types, WhoAnswersInWalk::Nobody, 2 + static_cast<unsigned>(question), level, layouts, key.bits(), askEachLayout); }

    bool listLayoutsAskedAbout(PropertyKeyID key, LayoutSetID layouts, bool asksWhatIsWrittenAbove, Vector<LayoutID, 16>& asked) const final
    {
        uint32_t numberInUse = 0;
        return layouts && !holdsOneMember(layouts) && m_plan.slots().listMembersAskedAbout(m_program, key, layouts, static_cast<SlotPlan::WhatIsWrittenAbove>(asksWhatIsWrittenAbove), asked, numberInUse);
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
    TypeID contents(TypeLevel, ObjectID, ContentsSlot) const final { return { }; }
    TypeID result(TypeLevel level, FunctionID function, uint32_t resultIndex) const final { return m_positionsOfBuild.result(level, function, resultIndex); }
    TypeID entryParameter(TypeLevel, FunctionID, uint32_t) const final { return { }; }
    TypeID resultOfCallBeingTyped(TypeLevel, uint32_t) const final { return { }; }
    bool listLayoutsAskedAbout(PropertyKeyID key, LayoutSetID layouts, bool asksWhatIsWrittenAbove, Vector<LayoutID, 16>& asked) const final { return m_positionsOfBuild.listLayoutsAskedAbout(key, layouts, asksWhatIsWrittenAbove, asked); }

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

    TypeID variable(TypeLevel level, VariableID variable) const final
    {
        if (const PromotedVariable* promoted = m_plan.promotedEnvironments().find(variable))
            return typeOfPromotedVariable(level, *promoted);
        if (!m_believesTables && variable.index() < m_program.variables.size() && m_program.at(variable).declared.isEstablished())
            return { };
        bool isFilledByCompiledCodeAlone = !m_plan.runTimeMayFill(variable);
        TypeID proven = variable.index() < m_linked.variables.size() && isFilledByCompiledCodeAlone ? filledByCompiledCode(level, &m_linked.variables[variable.index()]) : TypeID();
        if (!proven && !m_readsTables && level == TypeLevel::Proven && variable.index() < m_linked.variables.size() && m_plan.variable(variable).representation != SlotRepresentation::JSValue)
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
        TypeID proven = filledByCompiledCode(level, cellAt(m_linked.firstFieldOfLayout, m_linked.fields, layout.index(), field));
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
        return m_readsTables && m_believesTables && level == TypeLevel::Proven ? m_plan.slot(layout, field).provenType : TypeID();
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
    bool listLayoutsAskedAbout(PropertyKeyID key, LayoutSetID layouts, bool asksWhatIsWrittenAbove, Vector<LayoutID, 16>& asked) const final
    {
        uint32_t numberInUse = 0;
        return layouts && !holdsOneMember(layouts) && m_plan.slots().listMembersAskedAbout(m_program, key, layouts, static_cast<SlotPlan::WhatIsWrittenAbove>(asksWhatIsWrittenAbove), asked, numberInUse);
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

    TypeID contents(TypeLevel, ObjectID, ContentsSlot) const final { return { }; }

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
        if (!m_readsTables && m_plan.conventions().resultRepresentation({ function, CodeRole::Body }) == Representation::JSValue)
            return { };
        return m_plan.typeHeldOfResult(m_program, m_linked, function);
    }

    TypeID entryParameter(TypeLevel level, FunctionID function, uint32_t entryParameter) const final { return level == TypeLevel::Proven ? m_plan.conventions().typeBehindGuard(function, entryParameter) : TypeID(); }
    bool entryParameterIsLeftOutOfCalls(FunctionID function, uint32_t entryParameter) const final { return m_plan.promotedEnvironments().isRemoved(function, entryParameter); }
    TypeID resultOfSelector(TypeLevel, uint32_t selector) const final { return m_plan.conventions().methodSelector(selector).typeOfResult; }
    TypeID resultOfTableRow(TypeLevel level, uint32_t selector, LayoutID receiver, bool& hasRow) const final
    {
        const MethodTableRow* row = m_plan.conventions().findRowOfMethodTable(selector, receiver);
        hasRow = row;
        return row && !row->guardedClass ? result(level, row->method, 0) : TypeID();
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
        case MemberFound::Kind::Nothing:
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

    TypeID asFarAsDoorTellsClasses(TypeID proven) const
    {
        if (!proven || holdsOnlyPrimitives(proven) || !m_types.at(proven).isJavaScriptValue())
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
    WhoAnswersInWalk m_whoAnswers;
    const TypeID* m_resultOfCallBeingTyped;
    const TypeID* m_resultOfReadBeingTyped;
    const TypeID* m_contentsBeingTyped;
};

} // anonymous namespace

static ValueID valueBehindNamesInWalk(const Code& code, ValueID value)
{
    while (value && opcodesThatGiveSameValue.contains(code.at(value).opcode)) {
        ValueID source = code.at(value).valueOperand();
        if (source.index() >= value.index())
            return { };
        value = source;
    }
    return value;
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
static constexpr OpcodeSet opcodesWhoseEffectsAreFoundByWalk = opcodesWhere([](Opcode opcode) { return effectRuleOf(opcode) == EffectRule::Call || effectRuleOf(opcode) == EffectRule::Builtin || effectRuleOf(opcode) == EffectRule::Inline; });

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
    , m_keptByPositionsInTexts(kept.keptByPositionsInTexts)
    , m_roundsKept(kept.roundsKept)
    , m_kindsHeldUnderNames(kept.kindsHeldUnderNames)
    , m_listsOfNames(kept.listsOfNames)
    , m_numbersOfListsOfNames(kept.numbersOfListsOfNames)
    , m_surveysOfConstants(kept.surveysOfConstants)
    , m_constantsThatListFieldsHeldFilled(kept.constantsThatListFieldsHeldFilled)
    , m_accessesOfScopesByFunctions(kept.accessesOfScopesByFunctions)
    , m_listsAreIteratedByIndex(kept.listsAreIteratedByIndex)
    , m_numbersOfFramesOfFunctions(kept.numbersOfFramesOfFunctions)
    , m_workerHasBegunUnit(kept.workerHasBegunUnit)
    , m_newestWalkerOfWorker(kept.newestWalker)
    , m_pendingBlocks(kept.roomOfPendingBlocks.take())
    , m_bindings(kept.roomOfBindings.take())
    , m_wordsThatSayNothing(kept.roomOfWordsThatSayNothing.take())
    , m_blocksWithFieldsOfScopesInOrder(kept.roomOfBlocksWithFieldsOfScopesInOrder.take())
    , m_placesOfLoopsBehindBlocks(kept.roomOfPlacesOfLoopsBehindBlocks.take())
    , m_countsOfUnit(kept.roomOfCountsOfUnit.take())
    , m_triesOfLoops(kept.roomOfTriesOfLoops.take())
    , m_inputBlocksInTries(kept.roomOfInputBlocksInTries.take())
    , m_statesOfHandlersInTries(kept.roomOfStatesOfHandlersInTries.take())
    , m_placesOfOpenLoopsInTries(kept.roomOfPlacesOfOpenLoopsInTries.take())
    , m_inputHeadsOfIterationsAddedInTries(kept.roomOfInputHeadsOfIterationsAddedInTries.take())
    , m_typesChangedInTries(kept.roomOfTypesChangedInTries.take())
    , m_entriesOfCopiesOfLoopsInTries(kept.roomOfEntriesOfCopiesOfLoopsInTries.take())
    , m_placesOfLoopsChangedInTries(kept.roomOfPlacesOfLoopsChangedInTries.take())
    , m_sharedConstantsAddedInTries(kept.roomOfSharedConstantsAddedInTries.take())
    , m_copiesOfConstantsAddedInTries(kept.roomOfCopiesOfConstantsAddedInTries.take())
    , m_operationsBeingWalkedInFrames(kept.roomOfOperationsBeingWalkedInFrames.take())
    , m_sizesOfLogsWhereFramesWentOnTop(kept.roomOfSizesOfLogsWhereFramesWentOnTop.take())
    , m_expectationsInTries(kept.roomOfExpectationsInTries.take())
    , m_headParametersWithSources(kept.roomOfHeadParametersWithSources.take())
    , m_stepsOfBuildByOpcode(kept.stepsOfBuildByOpcode)
    , m_inputs(inputs)
    , m_types(types)
    , m_arrivals(arrivals)
    , m_mechanismsOfBuild(withoutMechanismsMissingDependencies(inputs.options.mechanisms))
    , m_mechanisms(m_mechanismsOfBuild)
    , m_positionsOfBuild(makeUniqueWithoutFastMallocCheck<PositionsOfWalk>(inputs.program, inputs.facts, inputs.facts.linked(), inputs.plan, types, m_mechanismsOfBuild.contains(Mechanism::TypedOperations), true, &m_resultOfCallBeingTyped, &m_resultOfReadBeingTyped, &m_contentsBeingTyped))
    , m_positionsOfSlowBody(kept.positionsOfSlowBody)
    , m_positions(m_positionsOfBuild.get())
    , m_positionsKnowingNothing(kept.positionsKnowingNothing)
    , m_positionsKnowingWhatFunctionsHandBack(makeUniqueWithoutFastMallocCheck<PositionsKnowingWhatFunctionsHandBackInWalk>(*m_positionsOfBuild))
    , m_heldPositions(kept.heldPositions)
    , m_explainsClosedAccessesTo(inputs.options.explainsClosedAccessesTo)
{
    RELEASE_ASSERT(&kept.arenaOfPositions == &types);
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

void Walker::noteThatCopyMakesNewList(FunctionID callee, Reference receiver)
{
    m_copyThatMakesNewList = receiver && receiver.isValue() ? callee : FunctionID();
    if (!m_copyThatMakesNewList)
        return;
    m_receiverOfCopyThatMakesNewList = rootOfCellInSweptCode(m_code, receiver.id());
    m_depthOfCopyThatMakesNewList = m_frames.size() + 1;
}

bool Walker::takeWordThatCopyMakesNewList(Reference receiver)
{
    FunctionID copied = std::exchange(m_copyThatMakesNewList, FunctionID());
    return copied && copied == currentFunction() && m_frames.size() == m_depthOfCopyThatMakesNewList && receiver && receiver.isValue()
        && rootOfCellInSweptCode(m_code, receiver.id()) == m_receiverOfCopyThatMakesNewList;
}

void Walker::takeRoomOf(Walker& former)
{
    RELEASE_ASSERT(m_frames.isEmpty());
    m_code.takeRoomOf(former.m_code);
    m_numbersOfStates.takeRoomOf(former.m_numbersOfStates);
    m_answersOfStates.takeRoomOf(former.m_answersOfStates);
    m_frames.takeRoomOf(former.m_frames);
    m_openEdges.takeRoomOf(former.m_openEdges);
    m_iterationsByIndex.takeRoomOf(former.m_iterationsByIndex);
    m_textsInStringBuilders.takeRoomOf(former.m_textsInStringBuilders);
    m_openLoops.takeRoomOf(former.m_openLoops);
    takeRoom(m_answersOfLoops, former.m_answersOfLoops);
}

void Walker::beginUnit(CodeUnit unit)
{
    RELEASE_ASSERT(!std::exchange(m_hasBegunUnit, true) && m_newestWalkerOfWorker == this);
    const Function& function = program().at(unit.function);

    bool isSlowBody = unit.role == CodeRole::SlowBody;
    m_mechanisms = isSlowBody ? m_mechanismsOfBuild & Mechanisms { Mechanism::Folding, Mechanism::DirectCalls } : m_mechanismsOfBuild;
    m_positions = isSlowBody ? m_positionsOfSlowBody.get() : m_positionsOfBuild.get();

    m_code.reset(unit);
    if ((m_inputs.options.shufflesBlockFrequencies || m_inputs.options.ignoresBranchHints) && walksInputBody(unit.role)) [[unlikely]]
        m_code.takeFrequenciesOfBlocks(m_inputs.options.shufflesBlockFrequencies ? Code::FrequenciesOfBlocks::DrawnBySeed : Code::FrequenciesOfBlocks::AllNormal, m_inputs.options.seedOfShuffles);
    m_origin = { .position = function.position, .inlineFrame = 0, .isTailCall = false, .isPartOfConstruction = false };
    m_environmentCellsOfUnit.mayNameSome = isHandedEnvironmentWithByte(*this, unit.function);
    if (std::exchange(m_workerHasBegunUnit, true) && m_inputs.options.laterUnitsOpenNoGuesses) [[unlikely]]
        m_opensGuesses = false;

    InlineFrame ownFrame { };
    ownFrame.callee = unit.function;
    ownFrame.callPosition = Instruction::noPosition;
    ownFrame.parent = 0;
    ownFrame.module = function.module;
    m_code.addInlineFrame(ownFrame);

    bool bodyStandsBehindUnit = walksInputBody(unit.role) && (unit.role != CodeRole::Body || !function.isCoroutine());
    Frame frame;
    frame.function = unit.function;
    frame.variablesStoredFirst = facts().linked().variablesStoredFirstIn(unit.function);
    frame.isOfUnit = true;
    if (bodyStandsBehindUnit) {
        frame.index = facts().bodies().find(unit.function);
        if (frame.index) {
            frame.body = &frame.index->body();
            frame.references.fill(Reference(), frame.index->numberOfValues());
        }
    }
    m_frames.reserveCapacity(16);
    m_frames.append(WTF::move(frame));
}

bool Walker::standInFrontOfBodyOf(FunctionID function)
{
    beginUnit({ function, program().at(function).isCoroutine() ? CodeRole::CoroutineBody : CodeRole::Body });
    return m_frames[0].index;
}

bool Walker::walk(CodeUnit unit)
{
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
    return m_inputs.options.walksLastWalksOfLoopsAlone && !m_hasFailed && !m_isWalkingLastAlone && !m_answersOfLoops.isEmpty();
}

bool Walker::walkLastWalksOfLoopsAlone(Walker& first)
{
    m_isWalkingLastAlone = true;
    m_answersOfLoops = WTF::move(first.m_answersOfLoops);
    m_placesOfAnswersOfLoops = WTF::move(first.m_placesOfAnswersOfLoops);
    m_rowsWalkedAgainForCheck = WTF::move(first.m_rowsWalkedAgainForCheck);
    m_loopsWhoseTextsStayOutOfBuilders = WTF::move(first.m_loopsWhoseTextsStayOutOfBuilders);
    for (AnswerOfLoop& answer : m_answersOfLoops) {
        if (!answer.asWalkedLast)
            continue;
        std::shared_ptr<const AnswerOfLoop> walked = answer.asWalkedLast;
        answer.types = walked->types;
        answer.rungs = walked->rungs;
        answer.effects = walked->effects;
        answer.knowsEffects = walked->knowsEffects;
        answer.claimsNothing = walked->claimsNothing;
        answer.typesOfValues = walked->typesOfValues;
        answer.numberOfRecordsWhenTypesOfValuesWereNoted = 0;
        answer.firstValue = walked->firstValue;
        answer.steppedOnDoubles = walked->steppedOnDoubles;
        answer.mentionsThatLeave = walked->mentionsThatLeave;
        answer.storesIntoScopes = walked->storesIntoScopes;
    }
    return walk(first.unit());
}

void Walker::fail(ASCIILiteral what, InternalErrorKind kind)
{
    if (!m_hasFailed) {
        m_hasFailed = true;
        m_failure = what;
        m_kindOfFailure = kind;
        m_functionOfFailure = m_frames.isEmpty() ? unit().function : currentFunction();
        if (m_triesOfLoops.isEmpty())
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

bool Walker::walkBodyBehindEntry(std::span<const Reference> entryArguments, BlockID whereListsAreMade, const MakerOfListBehindEntry& makerOfLists)
{
    Frame& frame = m_frames[0];
    frame.index = facts().bodies().find(unit().function);
    if (!frame.index || frame.index->hasIrreducibleFlow()) {
        fail("an entry walks a body that cannot be walked"_s);
        return false;
    }
    frame.body = &frame.index->body();
    frame.references.fill(Reference(), frame.index->numberOfValues());
    BodyBlock entry = frame.index->block(entryBlock);
    if (entry.numberOfParameters() != entryArguments.size() || entryArguments.size() != program().at(unit().function).numberOfEntryParameters()) {
        fail("the entry block has other parameters than the function has entry parameters"_s);
        return false;
    }
    standIn(entryBlock);
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
    SetForScope whereListsAreMadeInUnit(m_whereListsAreMadeBehindEntry, whereListsAreMade);
    SetForScope makerOfListsInUnit(m_makerOfListsBehindEntry, &makerOfLists);
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

void Walker::makeListsBehindEntry()
{
    m_blockBuiltWhereListsWereMade = m_block;
    const Function& function = program().at(unit().function);
    BodyBlock entry = m_frames.last().index->block(entryBlock);
    for (unsigned index = 0; index < function.numberOfEntryParameters() && !isUnreachable(); ++index) {
        EntryParameter kind = function.entryParameterKind(index);
        if (kind != EntryParameter::HiddenRest && (kind != EntryParameter::Parameter || index - function.firstParameterIndex() < function.numberOfPositionalParameters()))
            continue;
        Reference list = (*m_makerOfListsBehindEntry)(index);
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
    read.setArgumentIndex(position);
    chooseConservativeFields(read);
    return build(WTF::move(read));
}

Reference Walker::makeHiddenListOfEntryAgain()
{
    const Function& function = program().at(unit().function);
    for (unsigned index = 0; index < function.numberOfEntryParameters(); ++index) {
        if (function.entryParameterKind(index) == EntryParameter::HiddenRest)
            return (*m_makerOfListsBehindEntry)(index);
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
    if (index.dominates(m_whereListsAreMadeBehindEntry, m_frames.last().block))
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
    return !m_hasFailed;
}

TypeID Walker::typeArrivingAtEntryParameter(unsigned index, bool believesCallers)
{
    FunctionID id = unit().function;
    TypeID known = has(Mechanism::Arrivals) && believesCallers ? m_arrivals.provenAt(m_types, id, index) : TypeID();
    return facts().typeBelievedOfEntryParameter(m_types, id, index, known);
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

    standIn(entryBlock);
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
    TypeID held;
    for (LayoutID member : membersOf(m_types, read.receivers())) {
        std::optional<FieldIndex> field = plan().slots().findField(program(), member, read.key());
        TypeID type = field ? plan().slots().slot(member, *field).typeThatStoresAreHeldTo() : TypeID();
        if (!type)
            return { };
        type = typeThatUnitMayTestFor(m_types.testablePartOf(type), true, true);
        held = held ? m_types.unionOf(held, type) : type;
    }
    return held;
}

bool Walker::makeObjectOfSlotReadShown(Reference value, TypeID taken)
{
    ValueID id = value.id();
    while (id && !m_code.isConstant(id) && givesItsOperandInWalk(m_code.at(id).opcode))
        id = m_code.at(id).operand(0);
    if (!id || m_code.isConstant(id) || m_code.at(id).opcode != Opcode::GetSlot || m_code.at(id).level() != KnowledgeLevel::Proven)
        return false;
    const Operation& read = m_code.at(id);
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
            buildCheckOfProof(value, noCell, m_types.withPrimitives(TypeTable::anything, Primitive::Absent), ProofSource::Field, origin);
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
        buildCheckOfProof(value, both, widestTypeOf(form), ProofSource::Field, origin, false, true);
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
        buildCheckOfProof(found, held, m_types.withPrimitives(TypeTable::anything, Primitive::Absent), source, m_origin, true);
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

void Walker::holdVariableOfAnotherFileStoredFirst(VariableID variable)
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
    count(StatisticID::ReadsOfOtherFilesVariablesHeld);
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

static bool namesSlotStoredFirst(Walker& walker, const BodyValue& access)
{
    std::span<const SummaryScopeStoredFirst> scopes = walker.facts().summaries().at(walker.currentFunction()).scopesStoredFirst.span();
    if (scopes.empty())
        return false;
    ValueID allocation = walker.currentBodyIndex().valueBehindCasts(access.environmentOperand());
    auto scope = std::ranges::lower_bound(scopes, allocation, { }, &SummaryScopeStoredFirst::allocation);
    return scope != scopes.end() && scope->allocation == allocation && scope->fields.contains(access.field());
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

void Walker::stopAtMaker(Origin origin)
{
    count(StatisticID::MakersWithoutFieldsHeldFilled);
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
    if (isWrittenInFrameOnTop(allocation) && facts().summaries().at(currentFunction()).makerEndsWhereItLeaves(allocation.inputValueGiven))
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
        ValueID literal = index.valueBehindCasts(index.value(id).operand(0));
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
    ValueID literal = index.valueBehindCasts(finishOrData.operand(0));
    if (facts().summaries().at(currentFunction()).makerEndsWhereItLeaves(literal))
        return;
    Reference object = referenceOf(finishOrData.operand(0));
    if (object && !index.isParameter(literal) && index.opcodeOf(literal) == Opcode::NewObject)
        showThatObjectHoldsWhatIsHeldFilled(object, index.value(literal).as<Opcode::NewObject>().layout());
}

void Walker::showThatObjectHoldsWhatIsHeldFilled(Reference object, LayoutID id)
{
    Vector<FieldIndex, 8> fieldsToAsk = fieldsHeldFilledThatAreNotShown(object, id);
    bool recordOfLayoutNamesThemAll = plan().layout(id).holdsCompiledObjects() && std::ranges::all_of(fieldsToAsk, [&](FieldIndex field) {
        return plan().slot(id, field).indexInLayoutRecord.has_value();
    });
    bool asksForFieldOfClassAbove = std::ranges::any_of(fieldsToAsk, [&](FieldIndex field) {
        return program().at(id).fields[field].flags.contains(FieldFlags::IsInherited);
    });
    if (fieldsToAsk.size() > 1 && asksForFieldOfClassAbove && recordOfLayoutNamesThemAll) {
        count(StatisticID::ObjectsAskedWhereMakersEnd);
        Draft test(Opcode::AreHeldFieldsFilled, m_origin);
        test.setObjectOperand(object);
        test.setLayout(id);
        if (Reference areFilled = build(WTF::move(test)); areFilled && !isUnreachable())
            stopAtMakerUnless(areFilled, m_origin);
        return;
    }
    for (FieldIndex field : fieldsToAsk) {
        count(StatisticID::FieldsAskedWhereMakersEnd);
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
    while (made.isValue() && opcodesThatGiveSameCell.contains(m_code.at(made.id()).opcode))
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
    switch (draft.opcode) {
    case Opcode::CallDirect:
        checkProofsOfOperandsOfDirectCall(draft);
        return;
    case Opcode::PutSlotOrAdd:
    case Opcode::GetSlotOrUndefined:
    case Opcode::GetDeferredClosureSlotOrUndefined:
        if (draft.level() == KnowledgeLevel::Proven)
            checkFactsRememberedOfCell(draft.objectOperand(), origin);
        return;
    case Opcode::GetSlot:
    case Opcode::PutSlot:
        if (draft.opcode == Opcode::PutSlot && draft.level() == KnowledgeLevel::Proven)
            checkFactsRememberedOfCell(draft.objectOperand(), origin);
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

void Walker::holdStoredValuesToPlaces(const Draft& draft)
{
    Origin origin = draft.origin();
    if (draft.opcode == Opcode::PutModuleVariable) {
        holdToTypeOfPlace(draft.valueOperand(), m_positionsOfBuild->variable(TypeLevel::Proven, draft.variable()), plan().promotedEnvironments().find(draft.variable()) ? ProofSource::Capture : ProofSource::Variable, origin);
        return;
    }
    if (draft.opcode == Opcode::PutEnvironmentField) {
        if (Reference born = draft.valueOperand(); born && born.isConstant() && typeOf(born) == TypeID(TypeTable::undefined) && plan().promotedEnvironments().isPromoted(draft.layout()))
            return;
        holdToTypeOfPlace(draft.valueOperand(), m_positionsOfBuild->environmentField(TypeLevel::Proven, draft.layout(), draft.field()), ProofSource::SlotOfScope, origin);
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

void Walker::holdToTypeOfPlace(Reference value, TypeID held, ProofSource source, Origin origin)
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
    buildCheckOfProof(value, held, m_types.withPrimitives(TypeTable::anything, Primitive::Absent), source, origin, true);
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
        buildCheckOfProof(value, taken, m_types.withPrimitives(TypeTable::anything, Primitive::Absent), ProofSource::Field, origin, true);
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
    if (!checksProofs() && !plan().conventions().holdsFusedCallEntry(unit()))
        return;
    FunctionID id = unit().function;
    BodyBlock entry = m_frames[0].index->block(entryBlock);
    for (unsigned index = 0; index < entry.numberOfParameters() && !isUnreachable(); ++index) {
        TypeID withoutProof = typeArrivingAtEntryParameter(index, false);
        if (const DeclaredType* declared = program().at(id).declaredEntryParameter(index); declared && declared->isEstablished() && m_types.at(withoutProof).isJavaScriptValue())
            withoutProof = m_types.at(withoutProof).primitives & Primitive::Absent ? TypeTable::anythingOrAbsent : TypeTable::anything;
        if (TypeID tested = unit().role == CodeRole::Body ? plan().conventions().typeBehindGuard(id, index) : TypeID()) {
            TypeID passed = m_types.intersectionOf(withoutProof, tested);
            withoutProof = isNeverType(passed) ? tested : passed;
        }
        checkProof(referenceBoundTo(entry.parameter(index)), withoutProof, ProofSource::EntryParameter, m_origin);
    }
}

void Walker::holdOperandsToWhatCalleeBelieves(const Draft& call)
{
    FunctionID callee = call.function();
    const LinkTables& link = facts().linked();
    CodeRole role = call.role();
    if (!m_mechanismsOfBuild.contains(Mechanism::Arrivals) || callee.index() >= link.functions.size() || (role != CodeRole::Body && role != CodeRole::SlowBody && role != CodeRole::CoroutineBody && role != CodeRole::ParameterGuard))
        return;
    if (unit().function == callee && namesOnlyUnitsOfItsFunction(unit().role) && !m_makerOfListsBehindEntry)
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
            buildCheckOfProof(operand, held, shown, ProofSource::EntryParameter, call.origin(), true);
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

static ProofSource sourceOfProofInWalk(Opcode opcode)
{
    switch (opcode) {
    case Opcode::GetModuleVariable:
        return ProofSource::Variable;
    case Opcode::GetCapture:
        return ProofSource::Capture;
    case Opcode::GetEnvironmentField:
    case Opcode::AssumeType:
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
    Opcode::GetCapture, Opcode::GetGlobalBinding, Opcode::GetArrayElement, Opcode::GetContiguousIndex, Opcode::GetOwnContiguousIndex,
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

void Walker::askWhatCallByValueGives(ValueID call, const Draft& draft, TypeID proven)
{
    if (m_types.saysNothing(proven) || unitReturnsAtOnce(draft))
        return;
    Vector<TypeID, 8> shownOfOperands;
    for (Reference operand : draft.operands())
        shownOfOperands.append(operand && operand.isRecord() ? typeShown(operand) : TypeID());
    TypeID shown = typeThatCallByValueIsShownToGive(draft, shownOfOperands.span());
    if (m_types.isSubset(shown, proven))
        return;
    if (!isNeverType(proven)) {
        buildCheckOfProof(Reference::value(call), proven, shown, ProofSource::ResultOfOtherCall, draft.origin(), true);
        return;
    }
    if (Reference callee = draft.operands()[OperandIndex::Call::callee]; callee && callee.isValue())
        buildCheckOfProof(callee, TypeTable::never, TypeTable::anything, ProofSource::ResultOfOtherCall, draft.origin());
}

bool Walker::isTypedByWhatLinkSaysOfContents(const Draft& draft) const
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
        bool hasBox = form == Representation::JSValue || form == Representation::Cell;
        if (Reference narrowed = draft.valueOperand(); narrowed && narrowed.isValue() && (hasBox || checksWalk()))
            checkProof(Reference::value(record), typeAsBuilt(narrowed), ProofSource::Test, draft.origin());
        return;
    }
    if (draft.opcode == Opcode::AssumeType) {
        Reference assumed = draft.valueOperand();
        if (numberRepresentations.contains(draft.representation))
            checkProof(assumed, proven, typeAsBuilt(assumed), draft.source(), draft.origin());
        else
            checkProof(Reference::value(record), typeAsBuilt(assumed), draft.source(), draft.origin());
        return;
    }
    if (draft.opcode == Opcode::GetModuleVariable && !draft.givesWordAsItLies()) {
        PlannedVariable planned = plan().variable(draft.variable());
        bool givesClosureThatTableKeeps = planned.function && (!planned.hasWordInStatics() || planned.encoding == VariableEncoding::FunctionAsZero) && !m_types.at(proven).primitives && singleFunctionOf(m_types, m_types.at(proven)) == planned.function;
        if (givesClosureThatTableKeeps) {
            Draft word(Opcode::GetModuleVariable, draft.origin());
            word.setVariable(draft.variable());
            chooseConservativeFields(word);
            word.setGivesWordAsItLies(true);
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

    TypeID withoutProof = TypeTable::anything;
    if (draft.opcode != Opcode::ResultAt) {
        typesOfOperands(draft);
        for (TypeID& type : m_operandTypes) {
            if (type && m_types.at(type).isJavaScriptValue())
                type = m_types.testablePartOf(type);
        }
        if (TypeID given = Typed::resultTypeOf(TypingContext { program(), m_types, *m_positionsWithoutProofs, TypeLevel::Proven, false }, draft, OperandTypes { m_operandTypes.span() }))
            withoutProof = given;
    }
    if (draft.opcode == Opcode::GetModuleVariable || draft.opcode == Opcode::GetEnvironmentField || draft.opcode == Opcode::GetCapture)
        withoutProof = m_types.withPrimitives(withoutProof, Primitive::Absent);
    ProofSource source = sourceOfProofOf(draft);
    uint32_t numberChecked = numberOfProofsChecked(source);
    if (withoutProof != proven)
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

bool Walker::entryParameterIsCapture(const Block& entry, ValueID parameter) const
{
    if (namesOnlyUnitsOfItsFunction(unit().role))
        return false;
    const Function& function = program().at(unit().function);
    size_t index = std::ranges::find(entry.parameters(), parameter) - entry.parameters().begin();
    return index < function.numberOfEntryParameters() && function.entryParameterKind(static_cast<unsigned>(index)) == EntryParameter::Capture;
}

bool Walker::entryParameterIsShown(const Block& entry, ValueID parameter) const
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
    if (numberRepresentations.contains(record.representation) && resultRuleOf(record.opcode) != ResultRule::Natural && record.opcode != Opcode::Narrowed && !isEntryParameter && !isReadOfSlotWithDoors)
        return widestTypeOf(record.representation);
    if (record.representation == Representation::Boolean)
        return TypeTable::boolean;
    if (record.opcode == Opcode::Parameter) {
        const Block& block = m_code.at(record.block());
        if (block.isEntry && !numberRepresentations.contains(record.representation) && entryParameterIsCapture(block, id) && entryParameterIsShown(block, id))
            return { };
        if (block.isEntry)
            return entryParameterIsShown(block, id) && !entryParameterIsCapture(block, id) ? record.type : numberRepresentations.contains(record.representation) ? widestTypeOf(record.representation) : TypeID(TypeTable::anything);
        return block.isHandlerBlock() || (block.isLoopHead && !m_headsOfClosedLoops.contains(record.block().index())) || m_entriesOfClosedCopies.contains(record.block().index()) ? TypeID(TypeTable::anything) : TypeID();
    }
    if (record.opcode == Opcode::Narrowed)
        return { };
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
    if (isEnforced(source) && (source == ProofSource::SlotOfScope || source == ProofSource::Capture || source == ProofSource::Variable || callsByValueInWalk.contains(record.opcode) || (record.opcode == Opcode::GetSlot && record.level() == KnowledgeLevel::Proven)))
        return { };
    if (record.opcode == Opcode::ResultAt || (source != ProofSource::OtherOperation && !recordsTypedByWhatIsProvenOfContents.contains(record.opcode)))
        return isEnforced(source) && source != ProofSource::Field && source != ProofSource::ResultOfOtherCall ? record.type : TypeID(TypeTable::anything);
    TypeID widest = widestPrimitiveResultOf(record.opcode);
    return widest == record.type && !codeGoesByTypesOfOperandsInWalk(record) ? widest : TypeID();
}

void Walker::forEachValueThatTypeShownLeansOn(ValueID id, NOESCAPE const Invocable<void(ValueID)> auto& functor) const
{
    const Operation& record = m_code.at(id);
    if (record.opcode == Opcode::Parameter) {
        const Block& block = m_code.at(record.block());
        size_t index = std::ranges::find(block.parameters(), id) - block.parameters().begin();
        m_code.forEachEdgeInto(record.block(), [&](const Edge& edge) {
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

TypeID Walker::computeTypeShown(ValueID id)
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
        size_t index = std::ranges::find(block.parameters(), id) - block.parameters().begin();
        TypeID joined;
        bool everyEdgeHandsOverEachParameter = true;
        m_code.forEachEdgeInto(record.block(), [&](const Edge& edge) {
            if (edge.arguments().size() != block.parameters().size()) {
                everyEdgeHandsOverEachParameter = false;
                return;
            }
            TypeID arriving = shown(edge.arguments()[index]);
            joined = joined ? m_types.unionOf(joined, arriving) : arriving;
        });
        return everyEdgeHandsOverEachParameter && joined ? joined : TypeID(TypeTable::anything);
    }
    TypingContext believingNothing { program(), m_types, *m_positionsKnowingNothing, TypeLevel::Proven };
    Vector<TypeID, 8> operands;
    if (record.opcode == Opcode::GetEnvironmentField || record.opcode == Opcode::GetCapture || record.opcode == Opcode::GetModuleVariable)
        return typeThatUnitMayTestFor(m_types.testablePartOf(record.type), true, true);
    if (record.opcode == Opcode::AssumeType) {
        TypeID heldAtDoors = typeThatUnitMayTestFor(m_types.testablePartOf(record.provenType()), true, true);
        TypeID both = m_types.intersectionOf(shown(record.valueOperand()), heldAtDoors);
        return isNeverType(both) ? heldAtDoors : both;
    }
    if (record.opcode == Opcode::GetSlot) {
        ValueID object = record.objectOperand();
        TypeID held = object && m_types.isSubset(shown(object), m_types.forLayoutSet(record.receivers())) ? typeThatDoorsHoldOfSlotRead(record) : TypeID();
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
    if (behind && checksThatHandOnOrThrow.contains(record.opcode) && m_types.isSubset(ofValue, m_code.at(behind).type)) {
        operands.append(ofValue);
        TypeID passed = Typed::resultTypeOf(believingNothing, record, OperandTypes { operands.span() });
        return passed ? passed : ofValue;
    }
    ValueID testID = record.opcode == Opcode::Narrowed ? record.testOperand() : ValueID();
    if (!testID || record.narrowsHolderOfOperand() || record.narrowsOtherResultOfOperand())
        return ofValue;
    const Operation& test = m_code.at(testID);
    uint32_t operandIndex = record.operandIndex();
    if (provesOf(test.opcode) == Proof::None || operandIndex >= test.numberOfOperands() || !test.operand(operandIndex))
        return ofValue;
    for (ValueID operand : test.operands()) {
        TypeID type = operand ? shown(operand) : TypeID();
        if (operand && !(FlagMask::asksInFull(test.opcode) && test.asksInFull()) && !m_types.isSubset(type, m_code.at(operand).type))
            return ofValue;
        operands.append(type);
    }
    operands[operandIndex] = ofValue;
    TypeID left = narrowOperandByTest(believingNothing, test, OperandTypes { operands.span() }, operandIndex, record.isOnTakenEdge() ? EdgeSide::Taken : EdgeSide::NotTaken);
    return left ? left : ofValue;
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
    Vector<ValueID, 16> waiting { value.id() };
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> comingRound;
    while (!waiting.isEmpty()) {
        ValueID id = waiting.last();
        bool isComingRound = !comingRound.isEmpty() && comingRound.contains(id.index());
        if (!isComingRound && isKnown(id)) {
            waiting.removeLast();
            continue;
        }
        if (!isComingRound && m_code.at(id).opcode == Opcode::Parameter && m_code.at(m_code.at(id).block()).isLoopHead) {
            comingRound.add(id.index());
            setTypeShown(id, TypeTable::anything);
        }
        size_t numberWaiting = waiting.size();
        forEachValueThatTypeShownLeansOn(id, [&](ValueID leanedOn) {
            if (!isKnown(leanedOn))
                waiting.append(leanedOn);
        });
        if (waiting.size() != numberWaiting)
            continue;
        setTypeShown(id, computeTypeShown(id));
        if (!comingRound.isEmpty())
            comingRound.remove(id.index());
        waiting.removeLast();
    }
    return m_typesShown.get(value.id().index());
}

void Walker::checkThatReadGivesValue(Reference value, ProofSource source, Origin origin)
{
    if (!checksProofs() || !value || !value.isValue() || isUnreachable())
        return;
    TypeID claimed = typeAsBuilt(value);
    Representation representation = representationOf(value);
    if (!claimed || isNeverType(claimed) || !m_types.at(claimed).isJavaScriptValue() || (representation != Representation::JSValue && representation != Representation::Cell))
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

void Walker::checkProof(Reference value, TypeID withoutProof, ProofSource source, Origin origin)
{
    if (!value || !value.isValue() || isUnreachable())
        return;
    TypeID proven = typeAsBuilt(value);
    Representation representation = representationOf(value);
    bool liesRaw = numberRepresentations.contains(representation) || optionalRepresentations.contains(representation) || representation == Representation::Boolean;
    if (!proven || isNeverType(proven) || (!liesRaw && representation != Representation::JSValue && representation != Representation::Cell))
        return;
    if (liesRaw && m_types.at(withoutProof).isJavaScriptValue()) {
        TypeID toldByForm = widestTypeOf(representation);
        TypeID both = m_types.intersectionOf(withoutProof, toldByForm);
        withoutProof = isNeverType(both) ? toldByForm : both;
    }
    if (!m_types.at(proven).isJavaScriptValue() || !m_types.at(withoutProof).isJavaScriptValue() || m_types.isSubset(withoutProof, proven))
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
    if (!own || isNeverType(own) || (!liesRaw && representation != Representation::JSValue && representation != Representation::Cell))
        return;
    if (!m_types.at(proven).isJavaScriptValue() || !m_types.at(own).isJavaScriptValue() || m_types.isSubset(own, proven))
        return;
    checkProof(value, proven, own, source, origin);
}

TypeID Walker::asFarAsUnitMayAsk(TypeID claimed)
{
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
    SetForScope whereItStands(m_buildsWhereItStands, true);
    Reference answer = build(WTF::move(test));
    if (!isUnreachable() && !hasFailed() && !isBuiltWhereItStandsSince(numberOfRecordsInFront, answer))
        ++m_numberOfChecksThatWalkAnswers;
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
    return answer && answer.isValue() && standsInFrontOfLoop(numberOfLoopsAround - 1, answer);
}

bool Walker::isBuiltWhereItStandsSince(uint32_t numberOfRecordsInFront, Reference answer) const
{
    return answer && answer.isValue() && answer.id().index() >= numberOfRecordsInFront && !comesFromInFrontOfLoopAround(answer);
}

void Walker::checkThatMemoryAnswersAsBelieved(Reference believed, Draft&& readAgain)
{
    if (!checksProofs() || isUnreachable() || hasFailed() || !believed)
        return;
    Origin origin = readAgain.origin();
    origin.isTailCall = false;
    uint32_t numberOfRecordsInFront = m_code.numberOfValues();
    Reference found = buildTestOfCheck(WTF::move(readAgain));
    if (isUnreachable() || !isBuiltWhereItStandsSince(numberOfRecordsInFront, found))
        return;
    Draft comparison(Opcode::StrictEq, origin);
    chooseConservativeFields(comparison);
    comparison.setLeftOperand(found);
    comparison.setRightOperand(believed);
    Reference holds = buildTestOfCheck(WTF::move(comparison));
    if (holds && holds.isValue() && !isUnreachable())
        checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::WhatIsRemembered, origin);
}

void Walker::askGuardsAgainWhereTheyAreLeanedOn(std::span<const MemberConditionRecord> remembered, Origin origin)
{
    origin.isTailCall = false;
    Draft test(Opcode::AreMembersUnchanged, origin);
    test.setConditions(remembered);
    Reference holds = buildTestOfCheck(WTF::move(test));
    if (holds && holds.isValue() && !isUnreachable())
        buildCheckOfProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::WhatIsRemembered, origin);
}

void Walker::checkProof(Reference value, TypeID proven, TypeID withoutProof, ProofSource source, Origin origin)
{
    if (checksProofsOf(source))
        buildCheckOfProof(value, proven, withoutProof, source, origin, true);
}

std::optional<Origin> Walker::leaveCheckOfWhatContainerHasJustGivenToTestBehind(Reference given)
{
    auto isCheckOfGiven = [&](const Operation& record) {
        return record.opcode == Opcode::CheckProof && record.valueOperand() == given.id();
    };
    uint32_t end = m_code.numberOfValues();
    uint32_t place = end;
    while (place && !isCheckOfGiven(m_code.at(ValueID(place - 1)))) {
        if (!m_code.at(ValueID(--place)).traits().isPure())
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

bool Walker::splitNullishOffWhatContainerHasJustGiven(Reference given, TypeID rest, Draft&& throwsOffNullish)
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
    if (!isUnreachable())
        build(WTF::move(throwsOffNullish));
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
    TypeID passing = m_types.intersectionOf(whole, test.as<Opcode::TypeTest>().testedType());
    TypeID failing = m_types.differenceOf(whole, passing);
    if (isNeverType(passing) || isNeverType(failing) || m_types.testablePartOf(passing) != passing || m_types.testablePartOf(failing) != failing)
        return false;
    std::optional<Origin> originOfCheck = armsMayEndWithBranchBehind(test) ? leaveCheckOfWhatContainerHasJustGivenToTestBehind(given) : std::nullopt;
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
    auto endArm = [&](bool isOnTakenEdge) {
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(given);
        narrowed.setTestOperand(isOfFirst);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(isOnTakenEdge);
        Draft answer(Opcode::Boolean, origin);
        answer.setBoolean(isOnTakenEdge != asksForFailingSide);
        answer.representation = Representation::JSValue;
        bind(input, build(WTF::move(narrowed)));
        bind(test.id(), constant(WTF::move(answer)));
        if (!m_hasFailed && !endArmWithBranchBehind(test))
            fail("a way of a test does not walk the end of its block"_s);
        bind(input, given);
    };
    continueIn(other);
    buildCheckOfProof(given, asksForFailingSide ? passing : failing, TypeTable::anything, ProofSource::Contents, *originOfCheck, true);
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
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
        return TypeTable::number;
    case Representation::OptionalInt64:
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

void Walker::buildCheckOfProof(Reference value, TypeID proven, TypeID withoutProof, ProofSource source, Origin origin, bool mayBringClasses, bool isNotShownToFit)
{
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
            ++m_numberOfProofsThatFormTells;
            return;
        }
        isAskedInRawForm = mayBeNumberThatIsProven && (numberRepresentations.contains(form) || ((checksProofsOf(source) || isNotShownToFit) && optionalRepresentations.contains(form)));
        if (!isNeverType(proven) && isNeverType(m_types.intersectionOf(said, proven))) {
            ++m_numberOfDoorsThatCanNeverPass;
            saidWhereNothingPasses = said;
        }
    }
    if (!isAnswerOfTest && !isAskedInRawForm && form != Representation::JSValue && form != Representation::Cell) {
        value = convert(value, Representation::JSValue, origin);
        if (!value || !value.isRecord() || isUnreachable())
            return;
    }
    TypeID checked = typeThatUnitMayTestFor(m_types.testablePartOf(proven), mayBringClasses);
    if (m_types.isSubset(withoutProof, checked)) {
        ++m_numberOfProofsThatNoTestTells;
        return;
    }
    origin.isTailCall = false;
    Draft check(Opcode::CheckProof, origin);
    check.setValueOperand(value);
    check.setCheckedType(checked);
    check.setTypeWithoutProof(isAskedInRawForm || (value.isValue() && hasTestedForHole(m_code.at(value.id()))) ? withoutProof : m_types.withPrimitives(withoutProof, Primitive::Absent));
    check.setSource(source);
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

void Walker::Frame::addNarrowerName(uint32_t& first, Reference value, Reference name)
{
    PlaceOfNarrowerName place;
    place.length = lengthOfNarrowerNames(first) + 1;
    uint32_t jump = first == noNarrowerName ? noNarrowerName : placesOfNarrowerNames[first].jump;
    uint32_t jumpOfJump = jump == noNarrowerName ? noNarrowerName : placesOfNarrowerNames[jump].jump;
    bool jumpsAreAlike = lengthOfNarrowerNames(first) - lengthOfNarrowerNames(jump) == lengthOfNarrowerNames(jump) - lengthOfNarrowerNames(jumpOfJump);
    place.jump = jumpsAreAlike ? jumpOfJump : first;
    narrowerNames.append(NarrowerName { value, name, first });
    narrowedValues |= bitOfNarrowedValue(value);
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
    if (listOfNarrowerNamesIsLong && reference) {
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

Reference Walker::Frame::narrowestNameInLongList(Reference reference) const
{
    forEachNarrowerName<NarrowerNamesAsked::OfValue>(reference, [&](const NarrowerName& narrower) {
        reference = narrower.name;
        return IterationStatus::Done;
    });
    return reference;
}

Reference Walker::narrowestReferenceOf(ValueID input) const
{
    const Frame& frame = m_frames.last();
    auto narrowestNameOf = [&](Reference reference) {
        if (!(frame.narrowedValues & Frame::bitOfNarrowedValue(reference)))
            return reference;
        if (frame.listOfNarrowerNamesIsLong) [[unlikely]]
            return frame.narrowestNameInLongList(reference);
        for (uint32_t place = frame.firstNarrowerName; place != noNarrowerName; place = frame.narrowerNames[place].next) {
            if (frame.narrowerNames[place].value == reference)
                return frame.narrowerNames[place].name;
        }
        return reference;
    };
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

Walker::ClosedArm::ClosedArm(Walker& walker, bool beginsTurnAgainWhereItMay)
    : m_walker(walker)
    , m_firstBinding(walker.m_bindings.size())
    , m_openedGuesses(std::exchange(walker.m_opensGuesses, false))
{
    if (beginsTurnAgainWhereItMay && walker.mayBeginTurnAgainInClosedCopy())
        walker.beginTurnAgainInClosedCopy();
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
        frame.references[binding.input.index()] = binding.previous;
        if (binding.previous && frame.valuesBuiltTwice && frame.valuesBuiltTwice->slotOf(binding.input) != Frame::ValuesBuiltTwice::noSlot)
            frame.valuesBuiltTwice->boundOnWay.append(binding.input);
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
    static constexpr uint32_t hasNoEmitter = 1u << 1;
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
    static constexpr uint32_t hasEffectsOfOperator = 1u << 14;
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

TypeID Walker::typeOfNarrowedDraft(const Draft& draft)
{
    Reference value = draft.valueOperand();
    Reference test = draft.testOperand();
    if (!value || !value.isRecord() || (test && !test.isRecord()))
        return { };
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

ALWAYS_INLINE TypeID Walker::resultTypeByStepsOf(const TypingContext& context, const Draft& draft, uint32_t steps, bool& listsOwnTypes)
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
    if (m_numberOfMarksOfTries) [[unlikely]]
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

    if (Reference made = findConversionMade(*this, reference, wanted); made && (m_numberOfLoopsThatMayBeLeft >= m_openLoops.size() || standsInFrontOfLoop(m_numberOfLoopsThatMayBeLeft, made)))
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
static_assert((conversionsOnStraightRoadInWalk | conversionsOfForm) == conversionsOfForm && (conversionsOnStraightRoadInWalk | opcodesThatGiveSameCell) == opcodesThatGiveSameCell);
static_assert(!conversionsOnStraightRoadInWalk.intersects(opcodesThatStateTypes));

Reference Walker::tryBuildConversionOnStraightRoad(Opcode opcode, Representation to, Reference operand, Origin origin)
{
    if (!(stepsOfBuildOf(opcode) & StepOfBuildInWalk::isConversionOnStraightRoad) || !operand.isRecord() || isUnreachable())
        return { };
    if (hasTextsInStringBuilders() || m_positionsWithoutProofs)
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
        bool takesEachByKind = (stepsOfBuildOf(draft.opcode) & StepOfBuildInWalk::takesOperandsByKind) && numberOfOperands <= mostOperandsTakenByKindAtOnce && !hasTextsInStringBuilders() && !testsItsOwnGuess(draft);
        if (!takesEachByKind) {
            bool hasOperand = false;
            for (Reference operand : draft.operands())
                hasOperand |= !!operand;
            if (!hasOperand)
                return;
        }
        if (draft.opcode == Opcode::CallDirect && !hasTextsInStringBuilders()) {
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
        if (way == OperandsAreTaken::ByFormOfOperator && !hasTextsInStringBuilders() && !testsItsOwnGuess(draft)) {
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

    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        Representation wanted = forms[operandIndex];
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
    if (draft.kind != TerminatorKind::Branch || hasTextsInStringBuilders())
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
        if (hasTextsInStringBuilders() && isTextInStringBuilder(*this, operand)) {
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
    ValueID cell = rootOfCellInSweptCode(m_code, reference.id());
    state.markEscaped(referenceTo(cell));
    if (m_scopesThatClosuresHold.isEmpty() || m_code.at(cell).opcode != Opcode::NewClosure)
        return;
    for (ValueID capture : m_code.at(cell).capturesOperands()) {
        if (capture)
            state.markEscaped(referenceTo(rootOfCellInSweptCode(m_code, capture)));
    }
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

    if (lacksOperand && value.opcode() != Opcode::ReleaseHint && !isUnreachable())
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
    everyOperandHasFormOfItsKind = true;
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
        bool hasNoEmitter = opcodesWithoutEmitter().contains(opcode);
        bool takesTextsAsKeys = opcodesThatTakeTextsAsKeys.contains(opcode);
        OperandsAreTaken way = howOperandsAreTakenBy(opcode);
        bool takesOperandsAsBuilt = way == OperandsAreTaken::AsBuilt;
        add(Step::isTypeTest, opcode == Opcode::TypeTest);
        add(Step::hasNoEmitter, hasNoEmitter);
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
        add(Step::hasEffectsOfOperator, opcodesWithEffectsOfOperatorsInWalk.contains(opcode));
        add(Step::isCatch, catchOpcodes.contains(opcode));
        add(Step::findsEqualOperation, opcodesThatFindEqualOperationBuiltTakes.contains(opcode));
        add(Step::takesOperandsAsBuilt, takesOperandsAsBuilt);
        add(Step::takesCellsAsTheyAre, way == OperandsAreTaken::ByKindOrAsCell);
        add(Step::resultHasFormOfItsType, opcodesWhoseResultsHaveFormsOfTheirTypes.contains(opcode));
        add(Step::notesOperationBuilt, opcodesThatNoteOperationBuiltTakes.contains(opcode));
        add(Step::isTypedByWhatIsProven, readsTypedByWhatIsProvenOfInput.contains(opcode) || recordsTypedByWhatIsProvenOfContents.contains(opcode) || callsByValueInWalk.contains(opcode));
        add(Step::isCallForStackTests, opcode == Opcode::CallDirect || opcode == Opcode::AwaitCallBySavingFrame || opcode == Opcode::Construct);
        add(Step::isToldToPolls, opcodesThatPollsAreToldOfInWalk.contains(opcode));
        add(Step::isLookedAtByCopy, opcode == Opcode::Call || opcode == Opcode::StrictEq || opcode == Opcode::Builtin);
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

static uint16_t wordThatVariableIsBornWithInWalk(const Program& program, VariableID variable)
{
    VariableInitialization born = program.at(variable).initialization;
    return born == VariableInitialization::Uninitialized ? Primitive::Absent : born == VariableInitialization::Undefined ? Primitive::Undefined : 0;
}

bool Walker::leavesOutWordThatVariableIsBornWith(const Draft& draft, TypeID proven) const
{
    if (draft.opcode != Opcode::GetModuleVariable || plan().variable(draft.variable()).function)
        return false;
    uint16_t word = wordThatVariableIsBornWithInWalk(program(), draft.variable());
    return word && !(m_types.at(proven).primitives & word);
}

Reference Walker::askVariableInFrontOfTypedCopy(const Draft& draft, uint32_t outermostLoopThatMayBeLeft)
{
    std::optional<uint32_t> loop = openLoopOfTypedCopy();
    if (!loop || *loop < outermostLoopThatMayBeLeft)
        return { };
    Reference found;
    Reference isAsBorn;
    {
        InFrontOfTypedCopy inFront(*this, true);
        Draft read(Opcode::GetModuleVariable, draft.origin());
        read.setVariable(draft.variable());
        chooseConservativeFields(read);
        read.setGivesWordAsItLies(true);
        found = build(WTF::move(read));
        if (!found || !found.isValue() || isUnreachable() || !m_types.at(m_types.withoutPrimitives(typeOf(found), wordThatVariableIsBornWithInWalk(program(), draft.variable()))).isJavaScriptValue())
            return { };
        Draft test(Opcode::TypeTest, draft.origin());
        test.setValueOperand(found);
        test.setTestedType(TypeArena::fromPrimitives(wordThatVariableIsBornWithInWalk(program(), draft.variable())));
        isAsBorn = build(WTF::move(test));
    }
    if (!isAsBorn || !isAsBorn.isValue() || !branchInFrontOfTypedCopy(isAsBorn, false))
        return { };
    noteGuessOpened();
    InFrontOfTypedCopy inFront(*this, true);
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
    const uint32_t steps = stepsOfBuildOf(draft.opcode);
    if (steps & (Step::isTypeTest | Step::hasNoEmitter)) [[unlikely]] {
        if (steps & Step::isTypeTest)
            draft.setTestedType(withoutFunctionsThatMayNotBeMentioned(draft.testedType()));
        if (steps & Step::hasNoEmitter) {
            fail("an operation that no emitter stands behind is built"_s);
            return { };
        }
    }
    bool takesBoolean = false;
    const OperandInfosOfEntry& operandInfos = operandInfosByOpcode[static_cast<unsigned>(draft.opcode)];
    if (!(steps & Step::looksAtKindsOfOperands) && !m_holdsReadsOfVariablesAsConstants && !hasTextsInStringBuilders()) [[likely]] {
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
            if (kind != OperandKind::StringBuilder && hasTextsInStringBuilders() && isTextInStringBuilder(*this, operand)) {
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
    bool everyOperandHasFormOfItsKind = false;
    bool everyOperandFitsAtAGlance = (takesEveryOperandAsAny || (steps & Step::kindsAreSeenAtAGlance)) && everyOperandFitsItsKindAtAGlanceInWalk(m_types, m_code, draft, m_operandTypes.span(), operandInfos, takesEveryOperandAsAny, steps & Step::takesCellsAsTheyAre, everyOperandHasFormOfItsKind);
    bool operandsAreInFormsTaken = (steps & Step::takesOperandsAsBuilt)
        || (everyOperandFitsAtAGlance && everyOperandHasFormOfItsKind && (steps & Step::takesOperandsByKind) && !takesEveryOperandAsAny && !hasTextsInStringBuilders() && draft.numberOfOperands() <= mostOperandsTakenByKindAtOnceInWalk);
    if (!everyOperandFitsAtAGlance) {
        if (std::optional<IllTypedOperand> misfit = findIllTypedOperand(context, draft, OperandTypes { m_operandTypes.span() })) {
            logMisfitInWalk(m_types, nameOf(draft.opcode), *misfit);
            fail("an operand is not of the kind that its operation takes"_s);
            return { };
        }
    }

    bool hasNoEffects = steps & Step::hasPureBound;
    Effects effects = hasNoEffects ? Effects::none()
        : Typed::effectsOf(context, draft, (steps & Step::isResultAt) ? typesOfOperands(draft) : OperandTypes { m_operandTypes.span() }, (steps & Step::hasEffectsOfOperator) ? proven : TypeID(), foundByCaller);
    if ((draft.opcode == Opcode::PutSlot || draft.opcode == Opcode::PutSlotOrAdd) && !testsItsOwnGuess(draft)
        && storeMaySetMarkOfCell(*this, membersOf(m_types, draft.receivers()), draft.key(), typeOf(draft.valueOperand())))
        effects.writes.add(AbstractHeap::NamedProperties);
    if (m_inputs.options.rowThatSaysLess) [[unlikely]] {
        RowThatSaysLess row = RowThatSaysLess::decode(m_inputs.options.rowThatSaysLess);
        row.cut(KindOfEffectsRow::Operation, static_cast<unsigned>(draft.opcode), effects);
        if (Operation::opcodesWithOverload.contains(draft.opcode))
            row.cut(KindOfEffectsRow::Builtin, BuiltinTable::singleton().at(draft.overload()).builtin.index(), effects);
        if (draft.opcode == Opcode::CallDirect)
            row.cut(KindOfEffectsRow::Callee, draft.function().index(), effects);
    }
    if (effects.mayCollect && !m_isLeadingHandlerEdge && !(steps & Step::isCatch) && isUnderHandler())
        effects.mayThrow |= ThrowEffect::onlyForLackOfMemory();

    if (!hasNoEffects && !effects.mayBeRunAgainUnseen()) {
        ++m_numberOfOperationsBuiltThatAreSeenIfRunAgain;
        if (leaveCopyOfHeadForCall())
            return { };
    }
    if (!m_openLoops.isEmpty()) {
        if (!m_isWalkingAccess && !m_openLoops.isEmpty() && effects.mayRunOtherCode() && mayBeginTurnAgainInClosedCopy() && !looksAtLoopsAroundHaveCounted(draft, effects)) {
            m_turnOfTypedBuildLeavesWithoutTest |= m_code.at(m_block).frequency == BlockFrequency::Normal;
            beginTurnAgainInClosedCopy();
            return { };
        }
        if (m_isWalkingAccess && !m_openLoops.isEmpty() && effects.mayRunOtherCode() && mayBeginTurnAgainInClosedCopy() && !looksAtLoopsAroundHaveSeen(effects)) {
            beginTurnAgainInClosedCopy();
            return { };
        }
    }
    if (!m_triesOfLoops.isEmpty() && draft.opcode != Opcode::EndLifeOfCell) {
        FunctionID callee = effects.mayRunOtherCode() && effectRuleOf(draft.opcode) == EffectRule::Call && isWrittenInFrameOnTop(draft) && !facts().linked().calls.isEmpty() ? facts().linked().calleeAt(currentFunction(), draft.inputValueGiven) : FunctionID();
        if (callee) {
            Effects ofCallee = facts().linked().function(callee).effectsOnReturn();
            ofCallee.merge(facts().linked().function(callee).effectsOnThrow());
            hearEffectsInTries(Typed::effectsOf(typing(), draft, typesOfOperands(draft), { }, &ofCallee));
        } else if (effects.writes.contains(AbstractHeap::MemberGuards) && !effects.mayRunOtherCode() && writerEndsNoGuard(*this, draft)) {
            Effects heard = effects;
            heard.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
            hearEffectsInTries(heard);
        } else
            hearEffectsInTries(effects);
        if (m_hasFailed) [[unlikely]] {
            endAsUnreachable(m_origin);
            return { };
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
            std::optional<uint32_t> loop = findLoopToBuildInFrontOf(draft, effects);
            if (loop && leavesOutWordThatVariableIsBornWith(draft, proven)) {
                std::optional<uint32_t> ofOwnFrame = findLoopThatReaderVouchesFor(draft, *loop);
                if (ofOwnFrame != loop) {
                    if (Reference asked = askVariableInFrontOfTypedCopy(draft, *loop))
                        return asked;
                    if (m_hasFailed || isUnreachable())
                        return { };
                }
                count(ofOwnFrame ? StatisticID::ReadsOfVariablesThatTheirReaderVouchesFor : StatisticID::ReadsOfVariablesThatStayInTheTurn);
                loop = ofOwnFrame;
            }
            if (loop) {
            BlockID inside = std::exchange(m_block, m_openLoops[*loop].blockInFront);
            SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, *loop);
            Reference built = build(WTF::move(draft), foundByCaller);
            m_block = m_hasFailed ? BlockID() : inside;
            return built;
            }
        }
    }

    if ((steps & Step::findsEqualOperation) && !m_buildsWhereItStands) {
        if (Reference equal = findEqualOperationBuiltOfItsOpcodes(*this, draft, effects); equal && (m_numberOfLoopsThatMayBeLeft >= m_openLoops.size() || standsInFrontOfLoop(m_numberOfLoopsThatMayBeLeft, equal)))
            return equal;
    }

    if (effects.maySuspend)
        letObjectsLeaveAtSuspension(*this);
    if (m_state.holdsFreshObjects())
        markEscapingOperands(*this, draft, foundByCaller);
    if (m_environmentCellsOfUnit.mayNameSome && mayKeepEnvironment(draft.opcode))
        markEnvironmentsThatOperationKeeps(*this, draft);

    if (!operandsAreInFormsTaken)
        convertOperands(draft);
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
    if ((steps & Step::isTypedByWhatIsProven) && proven && m_positions == m_positionsOfBuild.get() && m_types.at(proven).isJavaScriptValue() && rawFormsOfJavaScriptValuesInWalk.contains(draft.representation)
        && ((m_resultOfReadBeingTyped && draft.opcode == Opcode::ElemGet) || (m_contentsBeingTyped && recordsTypedByWhatIsProvenOfContents.contains(draft.opcode))))
        draft.representation = Representation::JSValue;
    bool holdsValue = proven && !isNeverType(proven) && hasRegister(draft.representation) && draft.representation != Representation::RawWord;
    if (draft.representation == Representation::Unchosen || (holdsValue && !isLegalRepresentation(m_types, proven, draft.representation))) {
        fail("the result of an operation is kept in a form that its type does not allow"_s);
        return { };
    }

    if (!hasNoEffects)
        endWhatOperationWrites(*this, draft, effects);
    if (effects.mayThrow && !m_isLeadingHandlerEdge && has(Mechanism::Handlers) && numberOfFramesUpToHandlerInForce())
        noteStateAtThrowForHandler();
    bool standsInFrontOfLoop = m_numberOfLoopsThatMayBeLeft < m_openLoops.size();
    if (ValueID same = standsInFrontOfLoop && !m_buildsWhereItStands ? findSameInFrontOfLoop(m_numberOfLoopsThatMayBeLeft, draft) : m_buildsTestsBesidePlainWalk && !m_buildsWhereItStands ? findSameInFrontOfPlainWalk(draft) : ValueID()) {
        if (steps & Step::notesOperationBuilt)
            noteOperationBuiltOfItsOpcodes(*this, draft, effects, Reference::value(same));
        return Reference::value(same);
    }
    ValueID id = m_code.append(m_block, draft, proven, hasNoEffects ? EffectTraits() : EffectTraits(effects));
    if (m_inputs.options.checksEffects && opcodesWhoseEffectsAreFoundByWalk.contains(draft.opcode)) [[unlikely]]
        m_code.noteEffectsBelieved(id, effects);
    if (steps & Step::notesOperationBuilt)
        noteOperationBuiltOfItsOpcodes(*this, draft, effects, Reference::value(id));
    if (steps & Step::isTypedByWhatIsProven) {
        if (m_resultOfReadBeingTyped && readsTypedByWhatIsProvenOfInput.contains(draft.opcode) && m_positions == m_positionsOfBuild.get())
            m_code.addFactUse(FactUse { .record = id, .kind = FactKind::ProvenContents, .subjectKind = FactSubject::Kind::Operation, .type = m_positions->resultOfReadBeingTyped(TypeLevel::Proven) });
        if (m_contentsBeingTyped && recordsTypedByWhatIsProvenOfContents.contains(draft.opcode) && m_positions == m_positionsOfBuild.get())
            m_code.addFactUse(FactUse { .record = id, .kind = FactKind::ProvenContents, .subjectKind = FactSubject::Kind::Operation, .type = m_positions->contentsBeingTyped(TypeLevel::Proven) });
    }
    if (standsInFrontOfLoop)
        noteBuiltInFrontOfLoop(m_numberOfLoopsThatMayBeLeft, id);
    if (steps & Step::isCallForStackTests)
        noteDirectCallForStackTests(draft);
    bool checksRecord = m_positionsWithoutProofs && (checksProofs() || (draft.opcode != Opcode::Narrowed && walksBodyWhoseProofsAreChecked() && checksProofsOf(sourceOfProofOf(draft))));
    if (checksRecord && holdsValue)
        checkProofOfRecord(id, draft, proven);
    else if ((steps & Step::isTypedByWhatIsProven) && holdsValue && proven && isTypedByWhatLinkSaysOfContents(draft))
        buildCheckOfProof(Reference::value(id), proven, TypeTable::anything, ProofSource::Contents, draft.origin(), true);
    else if ((steps & Step::isTypedByWhatIsProven) && proven && (holdsValue || isNeverType(proven)) && callsByValueInWalk.contains(draft.opcode))
        askWhatCallByValueGives(id, draft, proven);

    if (!hasNoEffects && ((steps & Step::isToldToPolls) || (effects.mayCollect && has(Mechanism::Handlers) && !m_openLoops.isEmpty())))
        noteRecordBuiltForPolls(id, draft, effects);
    if (proven && isNeverType(proven)) {
        endAsUnreachable(m_origin);
        return { };
    }
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
    if (m_hasFailed || !cellsThatRunTimeKeeps.contains(draft.opcode) || m_positionsWithoutProofs || hasTextsInStringBuilders())
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

Reference Walker::readVariableWhereItIsTaken(Reference constant, Origin origin)
{
    if (m_code.at(constant.id()).opcode != Opcode::GetModuleVariable)
        return constant;
    VariableID variable = m_code.at(constant.id()).variable();
    TypeID type = m_code.at(constant.id()).type;
    if (Reference held = m_state.variable(variable); held && held.isValue() && typeOf(held) == type)
        return held;
    if (Reference kept = valueOfStableVariable(*this, variable, type); kept && kept.isValue())
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

void Walker::noteCopyOfConstant(ValueID constant, Representation form, ValueID copy)
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
    if (m_numberOfMarksOfTries) [[unlikely]]
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
    if (draft.opcode == Opcode::ReleaseHint && (!draft.valueOperand() || isTextInStringBuilder(*this, draft.valueOperand()) || isWithoutNameBehindLoopBuiltTwice(value.operand(0))))
        return true;

    chooseConservativeFields(draft);
    bool givesResult;
    if (stepsOfBuildOf(draft.opcode) & StepOfBuildInWalk::isLookedAtByCopy) {
        if (draft.opcode == Opcode::Call && !draft.calleeKey() && draft.calleeOperand().isValue() && draft.thisValueOperand().isRecord()) {
            const Operation& read = m_code.at(draft.calleeOperand().id());
            if (read.opcode == Opcode::PropGet && !read.isFreeName() && read.objectOperand() == draft.thisValueOperand().id())
                draft.setCalleeKey(read.key());
        }
        if (draft.opcode == Opcode::Call && has(Mechanism::BuiltinCalls))
            nameMethodOfReceiverAsMade(*this, value, draft);
        if (draft.opcode == Opcode::StrictEq && has(Mechanism::Guesses) && m_opensGuesses)
            sayWhatIsExpectedOfComparedOperands(*this, value, draft);
        givesResult = resultRuleInWalk(draft.opcode) != ResultRule::None;
        if (draft.opcode == Opcode::Builtin && has(Mechanism::Folding)) {
            if (Reference cell = cellThatRunTimeKeeps(draft)) {
                bind(value.id(), cell);
                return true;
            }
        }
    } else
        givesResult = !(stepsOfBuildOf(draft.opcode) & StepOfBuildInWalk::givesNoResult);
    Reference built = (stepsOfBuildOf(draft.opcode) & StepOfBuildInWalk::isConstantInFormOfUses) ? constant(WTF::move(draft)) : build(WTF::move(draft));
    if (built && givesResult)
        bind(value.id(), built);
    return true;
}

Reference Walker::addIterationByIndex(IterationByIndex&& iteration)
{
    iteration.inlineFrame = m_frames.last().inlineFrame;
    m_numberOfIterationsInFrontOfTheirLoops += !!iteration.inputHead;
    if (iteration.inputHead)
        if (m_inputHeadsOfIterations.add(inputHeadInFrame(iteration.inlineFrame, iteration.inputHead)).isNewEntry && m_numberOfMarksOfTries)
            m_inputHeadsOfIterationsAddedInTries.append(inputHeadInFrame(iteration.inlineFrame, iteration.inputHead));
    m_iterationsByIndex.append(WTF::move(iteration));
    return Reference::virtualObject(VirtualObjectID(m_iterationsByIndex.size() - 1));
}

void Walker::leaveArrivals(FunctionID callee, std::span<const Reference> arguments)
{
    const Function& function = program().at(callee);
    for (uint32_t entryParameter = 0; entryParameter < arguments.size(); ++entryParameter) {
        Reference argument = arguments[entryParameter];
        bool goesByWhatIsShown = argument && argument.isRecord() && (entryParameter >= function.numberOfEntryParameters() || function.entryParameterKind(entryParameter) != EntryParameter::Capture);
        TypeID proven = goesByWhatIsShown ? typeShown(argument) : ownTypeOf(argument);
        if (!proven)
            proven = TypeTable::anything;
        m_arrivalsHeld.append({ callee, entryParameter, proven, ArrivalHeld::Kind::Proven });
    }
    m_arrivalsHeld.append({ callee, static_cast<uint32_t>(arguments.size()), { }, ArrivalHeld::Kind::UndefinedFrom });
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = callee;
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

void Walker::settleParametersOf(BlockID block)
{
    PendingBlock& pending = pendingBlock(block);
    if (std::exchange(pending.parametersAreSet, true) || pending.parameters.isEmpty())
        return;
    m_code.setParameters(block, pending.parameters.span());
}

void Walker::arriveAt(BlockID block, WalkState&& state)
{
    PendingBlock& pending = pendingBlock(block);
    ++pending.numberOfArrivals;
    if (std::exchange(pending.hasState, true))
        pending.state.meetWith(state);
    else
        pending.state = WTF::move(state);
}

BlockID Walker::addBlock(BlockFrequency frequency)
{
    BlockID block = m_code.addBlock(BlockRole::Ordinary, m_numberOfCopiesBuiltForSize ? BlockFrequency::Rare : frequency);
    if (!m_numberOfCopiesBuiltForSize && isInLoop())
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
    if (walker.hasTextsInStringBuilders())
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
        ++pending->numberOfArrivals;
        m_block = next;
        if (std::exchange(pending->hasState, false)) {
            pending->state.meetWith(m_state);
            takeStateOfBlockThatIsBegun(m_state, pending->state);
        }
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

void Walker::branch(Reference condition, BlockID taken, BlockID notTaken, bool laysWhatItDecidesOnItsEdges)
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
    if (laysWhatItDecidesOnItsEdges) {
        atEnd = m_state;
        noteWhatBranchDecidesOnEdge(*this, from, 0);
    }
    ++pendingTaken->numberOfArrivals;
    if (std::exchange(pendingTaken->hasState, true))
        pendingTaken->state.meetWith(m_state);
    else
        pendingTaken->state = m_state;
    if (atEnd) {
        m_state = WTF::move(*atEnd);
        noteWhatBranchDecidesOnEdge(*this, from, 1);
    }
    ++pendingNotTaken->numberOfArrivals;
    if (std::exchange(pendingNotTaken->hasState, true))
        pendingNotTaken->state.meetWith(m_state);
    else
        pendingNotTaken->state = WTF::move(m_state);
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

bool Walker::stillHolds(const PassedTest& passed)
{
    return std::ranges::all_of(passed.sides(), [&](const PassedTest::Side& side) {
        return !!findPassedTest(referenceTo(side.test), side.answersYes);
    });
}

std::optional<PassedTest> Walker::goOnWhereTestPasses(Reference test, BlockID whereItFails, bool passesWithYes)
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
    hearEffectsInTries(effects);
    m_state.invalidate(effects);
    if (effects.mayThrow && has(Mechanism::Handlers))
        noteStateAtThrowForHandler(terminator.operands());

    Reference result = addParameter(done, given, Typed::representationOf(TypeReader(m_types), given));
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

void Walker::continueIn(BlockID block)
{
    if (m_hasFailed) {
        m_block = { };
        return;
    }
    if (m_block == block)
        return;
    if (!isUnreachable()) {
        PendingBlock& left = pendingBlock(m_block);
        left.state = WTF::move(m_state);
        left.hasState = true;
    }
    m_state.clear();

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

bool Walker::letArmsMeet(std::span<const ArmThatMeets> arms, BlockID meeting, std::span<Reference> met)
{
    Vector<size_t, 8> positionsOfParameters;
    Vector<Representation, 4> forms;
    for (size_t position = 0; position < met.size() && !arms.empty(); ++position) {
        Reference first = arms.front().values[position];
        met[position] = first;
        bool allBringOneValue = true;
        for (const ArmThatMeets& arm : arms) {
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
        for (const ArmThatMeets& arm : arms) {
            TypeID own = m_code.at(arm.values[position].id()).type;
            type = type ? joinOf(types(), facts().families(), type, own) : own;
            forms.append(representationOf(arm.values[position]));
        }
        met[position] = addParameter(meeting, type, formOfParameter(type, forms.span()));
        positionsOfParameters.append(position);
    }
    Vector<FieldOfScope, 4> fieldsThatMeet;
    if (unitHasMadeScopes()) {
        Vector<const WalkState*, 4> states;
        for (const ArmThatMeets& arm : arms) {
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
    for (const ArmThatMeets& arm : arms) {
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

Walker::StretchInArms Walker::stretchInArms(BlockID inputBlock, uint32_t begin, uint32_t end, bool armsMayEndWithReturn) const
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
    if (armsMayEndWithReturn) {
        BodyTerminator terminator = index.terminator(inputBlock);
        stretch.isReturnedAtOnce = terminator.kind() == TerminatorKind::ReturnCall && terminator.operand(0) == ValueID(first + size - 1);
    }
    return stretch;
}

void Walker::listArm(const StretchInArms& stretch, ArmsThatMeet& arms)
{
    if (stretch.isReturnedAtOnce && !isUnreachable())
        endArmWithReturnOf(stretch.inputBlock);
    if (isUnreachable() || hasFailed())
        return;
    ArmThatMeets arm { m_block, { } };
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

bool Walker::letArmsMeet(const StretchInArms& stretch, const ArmsThatMeet& arms, BlockID meeting)
{
    uint32_t firstOfBlock = currentBodyIndex().block(stretch.inputBlock).firstValue().index();
    for (uint32_t place = stretch.begin; place < stretch.end; ++place) {
        if (arms.isEmpty() || !stretch.mentionedBehind.contains(ValueID(firstOfBlock + place)))
            bind(ValueID(firstOfBlock + place), Reference());
    }
    Vector<Reference, 8> met(stretch.mentionedBehind.size());
    if (!letArmsMeet(arms.span(), meeting, met.mutableSpan()))
        return false;
    for (size_t position = 0; position < met.size(); ++position)
        bind(stretch.mentionedBehind[position], met[position]);
    return true;
}

ALWAYS_INLINE uint32_t Walker::addOpenEdge(InputBlock& target)
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

void Walker::releaseOpenEdge(uint32_t place)
{
    OpenEdge& edge = m_openEdges[place];
    edge.edge = { };
    edge.block = { };
    edge.arguments.shrink(0);
    edge.types.shrink(0);
    edge.state.clear();
    edge.names.clear();
    edge.isLedByClosedArm = false;
    edge.next = std::exchange(m_firstFreeOpenEdge, place);
}

BlockID Walker::addBlockFor(BlockID inputBlock, BlockRole role, BlockFrequency frequency)
{
    const Frame& frame = m_frames.last();
    BlockID block = m_code.addBlock(role, m_numberOfCopiesBuiltForSize ? BlockFrequency::Rare : frequency);
    if (!m_numberOfCopiesBuiltForSize && (frame.loopDepthAtCall || frame.index->loopDepthOf(inputBlock)))
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
    ready.m_records.shrink(0);
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (isTextInStringBuilder(*this, arguments[index]) && !isTextInStringBuilder(*this, Reference::value(parameters[index]))) {
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

void Walker::crossEdge(EdgeID edge, BlockID to, std::span<const Reference> arguments)
{
    ArgumentsOfEdge ready;
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
    noteEdgeCameRound(head);
    for (size_t place = m_triesOfLoops.size(); place--;) {
        TryOfLoop& tried = m_triesOfLoops[place];
        if (tried.frameIndex < m_frames.size() && inputBlock(tried.frameIndex, tried.inputHead).head == head) {
            ++tried.numberOfEdgesThatCameRound;
            break;
        }
    }
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    Vector<Reference, 8> withPositions;
    if (parameters.size() != arguments.size()) {
        withPositions.append(arguments);
        if (auto iterations = m_iterationsOfHeads.find(head.index()); iterations != m_iterationsOfHeads.end()) {
            for (uint32_t place : iterations->value) {
                withPositions.append(m_iterationsByIndex[place].position);
                if (m_iterationsByIndex[place].table)
                    withPositions.append(m_iterationsByIndex[place].table);
                if (m_iterationsByIndex[place].count)
                    withPositions.append(countOfNamesComesRound(*this, m_iterationsByIndex[place]) ? m_iterationsByIndex[place].count : constant(Draft(Opcode::Integer, m_origin)));
            }
        }
        if (!m_generatorsWalkedInConsumers.isEmpty())
            appendCarriedValuesThatComeRound(head, withPositions);
        if (!bringFieldsOfScopesRound(edge, head, withPositions))
            return;
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
    if (!m_triesOfLoops.isEmpty())
        holdFreshObjectsAgainstWhatComesRound(head);
    for (size_t index = 0; index < arguments.size(); ++index) {
        TypeID comingRound = ownTypeOf(arguments[index]);
        TypeID previewed = m_code.at(parameters[index]).type;
        if (comingRound && !m_types.isSubset(comingRound, previewed)) {
            TypeID heldTo = m_waysIntoClosedCopies.isEmpty() ? TypeID() : typeThatCounterIsHeldTo(m_waysIntoClosedCopies.last().inputHead, static_cast<uint32_t>(index));
            Reference proven = askAgainWhereItComesRound(edge, head, heldTo, arguments.first(arguments.size() - numberOfFieldsOfScopesThatBlockTakes(head)), scopedLambda<Reference()>([&] {
                return arguments[index];
            }));
            if (proven) {
                if (askedAgain.isEmpty())
                    askedAgain.append(arguments);
                askedAgain[index] = proven;
                arguments = askedAgain.span();
                comingRound = ownTypeOf(proven);
            }
            if (m_hasFailed)
                return;
        }
        if (comingRound && !m_triesOfLoops.isEmpty())
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

        TypeID type = m_openEdges[edges[0]].types[index];
        forms.shrink(0);
        for (uint32_t place : edges) {
            type = joinTypes(type, m_openEdges[place].types[index]);
            forms.append(representationOf(m_openEdges[place].arguments[index]));
        }
        Representation form = isHandler ? boxedFormOf(type) : formOfParameter(type, forms.span());
        parameters.append(m_code.appendParameter(block, type, form, m_origin));
        joined.append(index);
        bind(input.parameter(index), Reference::value(parameters.last()));
    }
    Frame& frame = m_frames.last();
    NamesOfWay names;
    Vector<uint32_t, 4> joinedNames;
    if (frame.valuesBuiltTwice) {
        names = m_openEdges[edges.back()].names;
        for (size_t index = 1; index < edges.size(); ++index) {
            m_openEdges[edges[index]].names.forEachSlotNamedOtherwiseBy(m_openEdges[edges[index - 1]].names, [&](uint32_t slot) {
                if (isNamedByWaysInto(frame, frame.valuesBuiltTwice->slots[slot].value, inputBlock))
                    joinedNames.append(slot);
            });
        }
        std::ranges::sort(joinedNames, { }, [&](uint32_t slot) {
            return frame.valuesBuiltTwice->slots[slot].ordinal - 1;
        });
        joinedNames.shrink(std::ranges::unique(joinedNames).begin() - joinedNames.begin());
    }
    for (uint32_t slot : joinedNames) {
        TypeID type = ownTypeOf(m_openEdges[edges[0]].names.nameIn(slot));
        forms.shrink(0);
        for (uint32_t place : edges) {
            Reference name = m_openEdges[place].names.nameIn(slot);
            if (!name) {
                fail("the edges that enter a block do not bring the same values of loops that were built twice"_s);
                return;
            }
            type = joinTypes(type, ownTypeOf(name));
            forms.append(representationOf(name));
        }
        parameters.append(m_code.appendParameter(block, type, isHandler ? boxedFormOf(type) : formOfParameter(type, forms.span()), m_origin));
        names.setNameIn(slot, Reference::value(parameters.last()));
        frame.valuesBuiltTwice->slots[slot].hasSeveralNames = true;
        frame.references[frame.valuesBuiltTwice->slots[slot].value.index()] = Reference();
    }
    Vector<ObjectsMetAtJoin, 1> objectsMet;
    if (!isHandler && !joined.isEmpty() && has(Mechanism::EscapeAnalysis))
        findObjectsThatMeetAt(block, inputBlock, edges, joined.span(), joinedNames.span(), parameters, objectsMet);
    Vector<FieldOfScope, 4> fieldsThatMeet;
    if ((unitHasMadeScopes() || !m_fieldsOfScopesOfHeads.isEmpty()) && !isHandler && edges.size() > 1) {
        Vector<const WalkState*, 4> states;
        for (uint32_t place : edges)
            states.append(&m_openEdges[place].state);
        findFieldsOfScopesThatMeet(states.span(), fieldsThatMeet);
        for (FieldOfScope& field : fieldsThatMeet) {
            parameters.append(m_code.appendParameter(block, field.type, field.form, m_origin));
            field.parameter = Reference::value(parameters.last());
        }
    }
    if (parameters.isEmpty()) {
        RELEASE_ASSERT(m_code.at(block).parameters().empty());
        bool hasTakenState = false;
        for (uint32_t place : edges) {
            OpenEdge& open = m_openEdges[place];
            m_code.closeEdge(open.edge, block, { });
            if (std::exchange(hasTakenState, true))
                m_state.meetWith(open.state);
            else
                m_state = WTF::move(open.state);
            releaseOpenEdge(place);
        }
        m_block = block;
        if (frame.valuesBuiltTwice)
            takeNamesOfWay(inputBlock, WTF::move(names));
        return;
    }
    m_code.setParameters(block, parameters.span());

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
        for (const FieldOfScope& field : fieldsThatMeet)
            arguments.append(field.values[way]);
        ++way;
        releaseOpenEdge(place);

        crossEdge(edge, block, arguments.span());
        if (m_hasFailed)
            return;
        if (std::exchange(hasMet, true))
            met.meetWith(m_state);
        else
            met = WTF::move(m_state);
    }
    m_state = WTF::move(met);
    m_block = block;
    for (const ObjectsMetAtJoin& objects : objectsMet) {
        Vector<Reference, 8> parametersOfSlots;
        for (ValueID parameter : parameters.span().subspan(objects.firstParameterOfSlots, objects.numberOfParametersOfSlots))
            parametersOfSlots.append(Reference::value(parameter));
        noteObjectsThatHaveMetAsOne(*this, Reference::value(parameters[objects.placeAmongParameters]), objects.objects, parametersOfSlots.span());
    }
    noteFieldsOfScopes(fieldsThatMeet.span());
    if (frame.valuesBuiltTwice)
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
        return name && name.isValue() ? rootOfCellInSweptCode(m_code, name.id()).index() : UINT32_MAX;
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

std::optional<Draft> Walker::draftOfNarrowedOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult& result, Reference holderOfSubject)
{
    targetIndex = m_edgeOfCodeBeingLed.value_or(targetIndex);
    Reference subject = referenceOf(result.subject);
    if (!has(Mechanism::TypedOperations) || !subject || !subject.isValue())
        return std::nullopt;
    Reference named = holderOfSubject ? holderOfSubject : subject;
    if (!named.isValue() || m_state.isFresh(referenceTo(rootOfCellInSweptCode(m_code, named.id()))))
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

Reference Walker::buildTestOfTagAsTestOfShape(const BodyValue& comparison)
{
    if (!has(Mechanism::TypedOperations) || checksProofs())
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
        return build(WTF::move(test));
    }
    return { };
}

std::optional<TerminatorDraft> Walker::draftOfSwitchOverShapes(const TerminatorDraft& overTags, Vector<uint32_t, 16>& casesLeft)
{
    if (!has(Mechanism::TypedOperations) || checksProofs() || (overTags.kind != TerminatorKind::Switch && overTags.kind != TerminatorKind::SwitchString))
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
    BodyTerminator end = index.terminator(index.blockOf(comparison.id()));
    return end.kind() == TerminatorKind::Branch && end.operand(0) == comparison.id();
}

void Walker::askShapeInPlaceOfTagAtBranch(const BodyTerminator& terminator)
{
    if (terminator.kind() != TerminatorKind::Branch)
        return;
    const BodyIndex& index = currentBodyIndex();
    ValueID condition = terminator.operand(0);
    if (!condition || index.isParameter(condition) || index.opcodeOf(condition) != Opcode::StrictEq || index.blockOf(condition) != terminator.block())
        return;
    Reference built = referenceOf(condition);
    if (!built || !built.isValue())
        return;
    if (Reference test = buildTestOfTagAsTestOfShape(index.value(condition)))
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
    if (m_state.isFresh(referenceTo(rootOfCellInSweptCode(m_code, asked))))
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
            checkProof(narrowed, inFront, ProofSource::Field, m_origin);
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

Reference Walker::tagReadInFrontOfTest(const BodyValue& read)
{
    if (!has(Mechanism::TypedOperations) || checksProofs())
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

static ValueID valueWhoseTruthIsToldInWalk(const Code& code, ValueID value, bool& isNegated)
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

Reference Walker::booleanAskedByBranchOnEdge(BlockID from, uint32_t targetIndex, Reference value)
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
    Reference& shared = m_frames.first().booleansOfUnit[conditionIsTrue != isNegated];
    if (!shared) {
        Draft answer(Opcode::Boolean);
        answer.setBoolean(conditionIsTrue != isNegated);
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

ALWAYS_INLINE void Walker::nameBooleansAskedByBranch(BlockID from, const BodyTerminator& terminator, uint32_t targetIndex, BlockID to)
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
        if (opcode != Opcode::ToBoolean && opcode != Opcode::LogicalNot && !inputOperationsThatOnlyHandOn.contains(opcode))
            return;
        asked = written.operand(0);
    }
}

Reference Walker::narrowOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult& result)
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

void Walker::noteNarrowerName(BlockID to, Reference value, Reference name)
{
    size_t frameIndex = m_frames.size() - 1;
    uint32_t& first = inputBlock(frameIndex, to).firstNarrowerName;
    Frame& frame = m_frames[frameIndex];
    if (first == narrowerNamesOfDominator)
        first = frame.firstNarrowerName;

    Vector<Reference, 4> values { value };
    frame.forEachNarrowerName<Frame::NarrowerNamesAsked::UnderName>(value, [&](const Frame::NarrowerName& narrower) {
        values.append(narrower.value);
        return IterationStatus::Continue;
    });
    for (Reference named : values)
        frame.addNarrowerName(first, named, name);
}

static bool standUnderSameHandler(const BodyIndex& index, BlockID one, BlockID other)
{
    BodyBlock first = index.block(one);
    BodyBlock second = index.block(other);
    if (first.hasHandler() != second.hasHandler())
        return false;
    return !first.hasHandler() || (first.handler().block() == second.handler().block() && std::ranges::equal(first.handler().arguments(), second.handler().arguments()));
}

void Walker::leadEdge(EdgeID edge, BlockID from, const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target)
{
    size_t frameIndex = m_frames.size() - 1;
    if (const Terminator& end = m_code.at(from).terminator; end.kind != TerminatorKind::Jump)
        noteWhatBranchDecidesOnEdge(*this, from, edge.index() - end.firstEdge.index());
    Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    BlockID to = target.block();
    InputBlock& entered = inputBlock(frame, to);
    BlockID head = entered.head;
    uint32_t numberOfProofs = static_cast<uint32_t>(target.proofs().size());
    bool entersAlone = !head && index.numberOfIncomingEdges(to) == 1 && !index.isLoopHead(to)
        && (frame.loopsBeingBuiltTwice.isEmpty() || !leavesLoopBeingBuiltTwice(frameIndex, to))
        && (frame.blocksWalkedForEachWay.isEmpty() || !isWalkingBlockForEachWay(frameIndex));

    Vector<Reference, 8> arguments;
    for (ValueID argument : target.arguments())
        arguments.append(referenceOf(argument));

    m_block = { };
    if (entersAlone || numberOfProofs) {
        m_block = m_code.addBlockBehindEdge(edge, m_numberOfCopiesBuiltForSize ? BlockFrequency::Rare : BlockFrequency::Normal, !m_numberOfCopiesBuiltForSize && (frame.loopDepthAtCall || index.loopDepthOf(to)));
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
            bool isRead = checksProofs() || !index.isUnused(result->parameter);
            arguments.append(isRead ? narrowOnEdge(from, targetIndex, *result) : referenceOf(result->subject));
            if (((isRead || namesHolderAnew) && isUnreachable()) || (namesHolderAnew && !narrowHolderOfTestedFieldOnEdge(from, targetIndex, *result, to))) {
                m_block = { };
                return;
            }
        }
    }
    ValueID condition = terminator.isConditionalBranch() ? terminator.as<TerminatorKind::Branch>().conditionOperand() : ValueID();
    bool asksRelation = condition && !index.isParameter(condition) && provesOf(index.opcodeOf(condition)) == Proof::IntegerBound;
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
            if (entersAlone)
                noteNarrowerName(to, operand, narrowed);
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
    ValueID plainInstanceAsked = entersAlone && !targetIndex && (index.opcodes().contains(Opcode::IsPlainInstanceOf) || index.opcodes().contains(Opcode::IsPlainHeirOf)) ? condition : ValueID();
    while (plainInstanceAsked && !index.isParameter(plainInstanceAsked) && (index.opcodeOf(plainInstanceAsked) == Opcode::ToBoolean || index.opcodeOf(plainInstanceAsked) == Opcode::Convert))
        plainInstanceAsked = index.value(plainInstanceAsked).operand(0);
    if (plainInstanceAsked && !index.isParameter(plainInstanceAsked) && (index.opcodeOf(plainInstanceAsked) == Opcode::IsPlainInstanceOf || index.opcodeOf(plainInstanceAsked) == Opcode::IsPlainHeirOf)) {
        BodyEdgeResult proven;
        proven.test = plainInstanceAsked;
        proven.subject = index.value(plainInstanceAsked).operand(0);
        proven.operandIndex = OperandIndex::IsPlainInstanceOf::object;
        proven.isOnTakenEdge = true;
        Reference operand = referenceOf(proven.subject);
        Reference test = referenceOf(proven.test);
        bool isStamped = test && test.isValue() && m_code.at(test.id()).opcode == Opcode::IsPlainInstanceOf && m_code.at(test.id()).receivers();
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
        if (isStamped && named && named.isValue() && outcomeKnownOf(*this, test) == true && findKnown(*this, KnownNote::AnswerOfQuestionStillStands, Reference::value(m_code.at(test.id()).objectOperand()), test))
            m_state.noteFactsOfCell(named, { FactOfCell::NoFieldOutsideSlot, FactOfCell::HasLayoutPrototype, FactOfCell::BearsNoMark });
    }
    ValueID absenceAsked = !targetIndex && index.opcodes().contains(Opcode::FindsNoMember) ? condition : ValueID();
    while (absenceAsked && !index.isParameter(absenceAsked) && (index.opcodeOf(absenceAsked) == Opcode::ToBoolean || index.opcodeOf(absenceAsked) == Opcode::Convert))
        absenceAsked = index.value(absenceAsked).operand(0);
    if (absenceAsked && !index.isParameter(absenceAsked) && index.opcodeOf(absenceAsked) == Opcode::FindsNoMember) {
        Reference test = referenceOf(absenceAsked);
        const Terminator& end = m_code.at(from).terminator;
        if (test && test.isValue() && m_code.at(test.id()).opcode == Opcode::FindsNoMember && m_code.at(test.id()).receivers() && end.isConditionalBranch() && end.conditionOperand() == test.id() && findKnown(*this, KnownNote::AnswerOfQuestionStillStands, Reference::value(m_code.at(test.id()).objectOperand()), test)) {
            const Operation& question = m_code.at(test.id());
            OptionSet<FactOfCell> shown;
            if (question.asksForNoExtraProperties())
                shown.add(FactOfCell::NoExtraProperties);
            if (question.asksForNoFieldOutsideSlot())
                shown.add(FactOfCell::NoFieldOutsideSlot);
            if (question.asksForLayoutPrototype())
                shown.add(FactOfCell::HasLayoutPrototype);
            if (!shown.isEmpty())
                m_state.noteFactsOfCell(Reference::value(question.objectOperand()), shown);
            for (MemberConditionRecord condition : question.conditions())
                m_state.noteGuardOfReadsHolds(condition.layout, condition.key, test);
        }
    }
    if (entersAlone && terminator.isConditionalBranch())
        nameBooleansAskedByBranch(from, terminator, targetIndex, to);
    TextsTakenOutOnEdge textsTakenOut;
    bool endsBlockOnEdge = !entersAlone;
    if (hasTextsInStringBuilders() && edgeLeavesLoopOfTextInStringBuilder(*this, to)) {
        if (!m_block) {
            m_block = addBlockFor(to);
            m_code.closeEdge(edge, m_block, { });
            edge = { };
        }
        {
            SetForScope loopsAroundEdge(m_numberOfLoopsAroundEdgeBeingLed, numberOfOpenLoopsAround(frameIndex, to));
            takeTextsOutOfStringBuildersOnEdge(*this, to, target, arguments, textsTakenOut);
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
    open.isLedByClosedArm = !m_opensGuesses;
    if (Frame::ValuesBuiltTwice* listed = m_frames[frameIndex].valuesBuiltTwice.get()) [[unlikely]] {
        takeNamesOntoEdge(open, frameIndex);
        for (ValueID value : textsTakenOut.names) {
            for (auto [record, string] : textsTakenOut.strings) {
                if (listed->nameOn(open.names, value) == record)
                    listed->nameOtherwiseOn(open.names, value, string);
            }
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
    Vector<Reference, 4> arguments;
    Vector<TypeID, 4> types;
    for (ValueID argument : target.arguments()) {
        Reference reference = nameBehindStandIn(frameIndex, argument, referenceBoundIn(m_frames[frameIndex], argument));
        TypeID type = ownTypeOf(reference);
        if (!type) {
            fail("a handler edge hands on a value that nothing was built for"_s);
            return;
        }
        Representation form = boxedFormOf(type);
        if (!isSameWord(representationOf(reference), form))
            reference = convert(reference, form, m_origin);
        if (!reference)
            return;
        arguments.append(reference);
        types.append(type);
    }

    Vector<std::pair<ValueID, Reference>, 2> names;
    if (m_frames[frameIndex].references.size() > m_frames[frameIndex].index->numberOfValues() && !takeCarriedValuesOntoHandlerEdge(frameIndex, names))
        return;
    if (hasTextsInStringBuilders() && leadHandlerEdgeBehindStringsOfTexts(frameIndex, target, arguments, types, names))
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
        m_frames[frameIndex].valuesBuiltTwice->nameOtherwiseOn(open.names, value, name);
}

ALWAYS_INLINE void Walker::standIn(Frame& frame, const BodyBlock& block, InputBlock& record)
{
    frame.block = block.id();
    frame.endOfValues = block.endOfValues().index();
    frame.hasHandler = block.hasHandler();
    if (frame.hasHandler && throwsPastHandlerOf(block))
        frame.hasHandler = false;
    frame.standsInFrontOfHead = false;

    uint32_t& first = record.firstNarrowerName;
    if (first == narrowerNamesOfDominator) {
        BlockID dominator = frame.index->immediateDominator(block.id());
        first = dominator ? frame.blocks[dominator.index()].firstNarrowerName : noNarrowerName;
        if (first == narrowerNamesOfDominator)
            first = noNarrowerName;
    }
    frame.firstNarrowerName = first;
    frame.listOfNarrowerNamesIsLong = frame.lengthOfNarrowerNames(first) >= Frame::lengthOfLongListOfNarrowerNames;
}

void Walker::standInOtherBlock(BlockID inputBlock)
{
    Frame& frame = m_frames.last();
    standIn(frame, frame.index->block(inputBlock), this->inputBlock(frame, inputBlock));
}

ALWAYS_INLINE void Walker::enterJoin(BlockID inputBlock, InputBlock& record, const BodyBlock& input)
{
    uint32_t only = record.firstOpenEdge;
    OpenEdge& edge = m_openEdges[only];
    if (edge.next != noOpenEdge || (!edge.block && hasTextsInStringBuilders())) {
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
    if (m_frames.last().valuesBuiltTwice)
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
    if (m_triesOfLoops.isEmpty() || !value || !value.isValue())
        return;
    if (TryOfLoop& innermost = m_triesOfLoops.last(); operation && tested && innermost.frameIndex == m_frames.size() - 1) {
        currentBodyIndex().value(operation).forEachOperand([&](ValueID operand, auto...) {
            if (operand && referenceOf(operand) == value && !innermost.testsSeen.containsIf([&](const GuessCountedOnByLooks& seen) { return seen.operation == operation && seen.operand == operand; }))
                innermost.testsSeen.append(GuessCountedOnByLooks { operation, operand, tested });
        });
    }
}

void Walker::noteMemberAskedByItsByteInTries(const BodyValue& read, TypeID typeOfHolder)
{
    if (m_triesOfLoops.isEmpty() || m_triesOfLoops.last().frameIndex != m_frames.size() - 1)
        return;
    Vector<GuessCountedOnByLooks, 4>& seen = m_triesOfLoops.last().testsSeen;
    if (seen.isEmpty() || seen.last().operation != read.id())
        seen.append(GuessCountedOnByLooks { read.id(), read.objectOperand(), typeOfHolder });
}

void Walker::enterLoopHead(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    BodyBlock input = currentBodyIndex().block(inputHead);
    uint32_t numberOfParameters = input.numberOfParameters();
    uint32_t depthAroundLoop = m_frames[frameIndex].loopDepthAtCall + currentBodyIndex().loopDepthOf(inputHead) - 1;
    m_frames[frameIndex].standsInFrontOfHead = true;

    Vector<uint32_t, 4> edges = takeOpenEdges(inputHead);
    BlockFrequency frequency = m_numberOfCopiesBuiltForSize ? BlockFrequency::Rare : BlockFrequency::Normal;
    BlockID preheader = m_code.addBlock(BlockRole::Ordinary, frequency);
    m_code.setLoopMarks(preheader, !m_numberOfCopiesBuiltForSize && depthAroundLoop, false);
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
        m_iterationsByIndex[m_frames[frameIndex].placeOfForInWhosePositionEdgesBring].position = m_frames[frameIndex].valuesBuiltTwice->nameOn(m_frames[frameIndex].valuesBuiltTwice->namesOfWay, step);
    if (writeOutTurnsOfLoop(inputHead, entering.span()) || writeOutTurnsOfForIn(inputHead, entering.span()) || writeOutTurnsOfForOf(inputHead, entering.span()))
        return;
    if (!isBeingTried(frameIndex, inputHead) && !m_loopsBeingBought.contains(std::pair { static_cast<uint32_t>(frameIndex), inputHead }))
        reserveRoomOfListsThatLoopFills(*this, inputHead, entering.span());
    if (tryLoop(inputHead, entering.span())) {
        if (!m_frames[frameIndex].loopsBeingBuiltTwice.contains(currentBodyIndex().loopOf(inputHead)))
            letGoOfTextsThatLoopTook(inputHead);
        return;
    }
    for (Reference argument : entering)
        letLeave(argument);
    if (hasTextsInStringBuilders())
        typeTextsInStringBuildersForLooks(*this, entering.span(), entryTypes);
    Frame& frame = m_frames[frameIndex];
    bool isInsideLoopLookedAt = frame.loopLookedAt && frame.loopLookedAt != currentBodyIndex().loopOf(inputHead) && currentBodyIndex().isInLoop(inputHead, frame.loopLookedAt);
    std::span<const TypeID> settledAround;
    if (isInsideLoopLookedAt)
        settledAround = frame.typesOfLooks.spanAt(input.firstParameter().index() - frame.firstValueLookedAt, numberOfParameters);
    LoopPreview preview = isBeingTried(frameIndex, inputHead) ? previewOutOfTries(inputHead, entryTypes.span()) : previewThatClaimsNothingOfLoops(*this, inputHead, entryTypes.span(), settledAround);
    if (m_hasFailed)
        return;
    if (preview.typesOfParameters.size() != numberOfParameters) {
        fail("the preview of a loop types other parameters than the head has"_s);
        return;
    }
    if (!m_generatorsTakenByConsumers.isEmpty() && walkGeneratorInPlaceOfLoop(inputHead, preview.typesOfParameters.span(), entering.span()))
        return;
    bool turnsBorrowScopes = !preview.scopesThatTurnsOnlyBorrow.isEmpty();
    if (!preview.hasLetMentionsLeave && m_headOfRowHandedOn == inputHead && m_frameOfRowHandedOn == frameIndex)
        m_storesIntoScopesHandedOn.areKnown = false;
    if (has(Mechanism::LoopInvariants) && mayBuildLoopSeveralTimes(inputHead) && (buildLoopForEachMember(inputHead, preview, entering.span()) || buildLoopTwice(inputHead, preview, entering.span())))
        return;
    if (!m_waysIntoClosedCopies.isEmpty() && looksCountOnTurnsThatBeginAgain(inputHead))
        m_waysIntoClosedCopies.last().guessesCountedOn = WTF::move(preview.guessesCountedOn);
    buildPollInFrontOfLoop(inputHead, preview);
    if (m_hasFailed)
        return;
    beginTextsInStringBuilders(*this, inputHead, preview, entering, entryTypes);
    if (m_hasFailed || isUnreachable())
        return;
    if (isInsideLoopLookedAt) {
        const BodyIndex& index = currentBodyIndex();
        LoopID loop = index.loopOf(inputHead);
        uint32_t numberOfLoopsLeft = preview.typesOfValues.isEmpty() ? 0 : index.loop(loop).numberOfLoopsWithin - 1;
        for (uint32_t inner = loop.index() + 1; numberOfLoopsLeft && inner < index.numberOfLoops(); ++inner) {
            BodyBlock innerHead = index.block(index.loop(LoopID(inner)).head);
            if (!index.isInLoop(innerHead.id(), loop))
                continue;
            --numberOfLoopsLeft;
            for (uint32_t parameter = 0; parameter < innerHead.numberOfParameters(); ++parameter) {
                uint32_t value = innerHead.parameter(parameter).index();
                TypeID found = preview.typesOfValues[value - preview.firstValue];
                TypeID* around = frame.typesOfLooks.slotAt(value - frame.firstValueLookedAt);
                if (around && found && (!*around || m_types.isSubset(found, *around)))
                    *around = found;
            }
        }
    } else if (!preview.typesOfValues.isEmpty()) {
        frame.loopLookedAt = currentBodyIndex().loopOf(inputHead);
        frame.firstValueLookedAt = preview.firstValue;
        frame.typesOfLooks = WTF::move(preview.typesOfValues);
    }

    Vector<FieldOfScope, 4> fieldsThatComeRound;
    if (preview.effects.maySuspend)
        letObjectsLeaveAtSuspension(*this);
    else if (turnsBorrowScopes)
        findFieldsOfScopesInFrontOfLoop(preview, fieldsThatComeRound);
    findFieldsOfScopesFromOutsideThatQuietTurnsStore(inputHead, preview.effects, fieldsThatComeRound, true);
    if (isBeingTried(frameIndex, inputHead)) {
        m_triesOfLoops.last().assumed = preview.effects;
        m_triesOfLoops.last().knowsWhatIsAssumed = true;
        if (m_triesOfLoops.last().wantsStateInFront)
            m_triesOfLoops.last().stateInFront = std::make_shared<WalkState>(m_state);
    }
    if (has(Mechanism::EscapeAnalysis) && !preview.hasLetMentionsLeave)
        m_state.clear();
    else {
        if (turnsBorrowScopes)
            endFieldsOfScopesThatTurnsStore(preview);
        m_state.invalidate(preview.effects);
    }
    if (!fieldsThatComeRound.isEmpty())
        keepFieldsOfScopesThatLoopWrites(inputHead, fieldsThatComeRound);

    BlockID head = m_code.addBlock(BlockRole::Ordinary, frequency);
    m_code.setLoopMarks(head, !m_numberOfCopiesBuiltForSize, true);
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
        parameters.append(m_code.appendParameter(head, type, form, m_origin));
        bind(input.parameter(index), Reference::value(parameters.last()));
    }
    for (size_t place = m_iterationsByIndex.size(); place-- && m_numberOfIterationsInFrontOfTheirLoops;) {
        IterationByIndex& iteration = m_iterationsByIndex[place];
        if (iteration.head || iteration.inputHead != inputHead || iteration.inlineFrame != m_frames[frameIndex].inlineFrame)
            continue;
        --m_numberOfIterationsInFrontOfTheirLoops;
        TypeID type = m_types.integer(Range { 0, positionBehindEveryEnd });
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
    if (!m_generatorsWalkedInConsumers.isEmpty())
        carryValuesRoundLoopOfGenerator(inputHead, head, entering, parameters);
    for (FieldOfScope& field : fieldsThatComeRound) {
        entering.append(field.values[0]);
        parameters.append(m_code.appendParameter(head, field.type, field.form, m_origin));
        field.parameter = Reference::value(parameters.last());
        noteWhatParameterOfFieldIsShownToBe(field);
    }
    if (!parameters.isEmpty())
        m_code.setParameters(head, parameters.span());
    inputBlock(frameIndex, inputHead).head = head;

    if (has(Mechanism::LoopInvariants))
        hoistInvariants(*this, inputHead, preview, entering.span());
    m_frames[frameIndex].standsInFrontOfHead = false;
    if (isUnreachable()) {
        m_block = { };
        return;
    }

    if (!preview.effects.maySuspend)
    {
        m_numberOfFieldsOfScopesOfHeadBeingOpened = fieldsThatComeRound.size();
        leaveBlockOpenInFrontOfLoop(inputHead, head, entering.span(), preview.effects);
        m_numberOfFieldsOfScopesOfHeadBeingOpened = 0;
    }
    else
        crossEdge({ }, head, entering.span());
    if (!m_hasFailed)
        m_block = head;
    if (!m_hasFailed && !fieldsThatComeRound.isEmpty()) {
        noteFieldsOfScopes(fieldsThatComeRound.span());
        fieldsOfScopesLaidAt(head).append(fieldsThatComeRound.span());
    }
    if (!m_hasFailed && turnsBorrowScopes)
        m_triesOfLoops.last().stateAtHeadThatKeepsScopes = std::make_shared<WalkState>(m_state);
    noteHeadOfTurn(inputHead);
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
    BlockID handler = frame.index->block(frame.block).handler().block();
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
    if (hasTextsInStringBuilders() && !m_hasFailed)
        bindTextsAtHeadOfHandlerBlock(inputBlock);
    if (m_hasFailed)
        return;

    if (ValueID subject = input.subject()) {
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

void Walker::walkBlock(BlockID inputBlock)
{
    size_t frameIndex = m_frames.size() - 1;
    auto noteEndOfBlock = makeScopeExit([&] {
        if (frameIndex < m_frames.size())
            this->inputBlock(frameIndex, inputBlock).numberOfRecordsBehindEnd = m_code.numberOfValues();
    });
    const BodyIndex& index = *m_frames.last().index;
    BodyBlock block = index.block(inputBlock);
    if (!frameIndex && !m_pendingBlocks.isEmpty())
        m_pendingBlocks.shrink(0);
    if (!m_openLoops.isEmpty()) {
        const OpenLoop& newest = m_openLoops.last();
        if (newest.frame > frameIndex || (newest.frame == frameIndex && inputBlock.index() > index.loop(newest.loop).last.index()))
            endBlocksInFrontOfLoops(frameIndex, inputBlock);
    }

    Frame& frame = m_frames.last();
    InputBlock& record = this->inputBlock(frame, inputBlock);
    if (inputBlock != entryBlock) {
        m_block = { };
        uint32_t firstOpenEdge = record.firstOpenEdge;
        if (firstOpenEdge == noOpenEdge)
            return;
        std::span<const BlockID> passedOverByScan = facts().summaries().at(frame.function).blocksNeverEntered.span();
        if (!passedOverByScan.empty() && std::ranges::binary_search(passedOverByScan, inputBlock)) {
            fail("an edge leads into a block that the scan has passed over"_s);
            return;
        }
        if (frame.block != inputBlock)
            standIn(frame, block, record);
        if (frame.valuesBuiltTwice)
            forgetNamesOfWay(frame);
        m_origin = { .position = Instruction::noPosition, .inlineFrame = frame.inlineFrame, .isTailCall = false, .isPartOfConstruction = false };
        bool severalEdgesEnter = m_openEdges[firstOpenEdge].next != noOpenEdge;
        bool isHeadOfTurnsWrittenOut = frame.headOfTurnsWrittenOut == inputBlock;
        bool liesBehindHeadCopied = frame.blocksOfHead && !frame.blocksOfHead->get(inputBlock.index()) && frame.mayLeaveForCall && !block.hasHandler() && !block.isHandlerBlock();
        for (uint32_t place = firstOpenEdge; place != noOpenEdge && liesBehindHeadCopied; place = m_openEdges[place].next)
            liesBehindHeadCopied = m_openEdges[place].state.nothingWasDoneSinceCopyBegan(frame.numberOfCopy);
        if (block.isHandlerBlock())
            enterHandlerBlock(inputBlock);
        else if (!isHeadOfTurnsWrittenOut && !liesBehindHeadCopied && index.isLoopHead(inputBlock))
            enterLoopHead(inputBlock);
        else if (!isHeadOfTurnsWrittenOut && !liesBehindHeadCopied && severalEdgesEnter && !(m_makerOfListsBehindEntry && frame.isOfUnit && inputBlock == m_whereListsAreMadeBehindEntry) && (walkBlockApartFromClosedWays(inputBlock) || walkBlockApartFromWaysThatDidSomething(inputBlock) || walkBlockForEachWayThatBringsObjectInSight(inputBlock)))
            return;
        else {
            enterJoin(inputBlock, record, block);
            if (isHeadOfTurnsWrittenOut) {
                record.isEntered = false;
                if (m_frames[frameIndex].turnsOfForIn && !isUnreachable())
                    standAtHeadOfTurnOfForIn(inputBlock);
            }
        }
    } else {
        if (frame.block != inputBlock)
            standIn(frame, block, record);
        record.isEntered = true;
    }
    if (isUnreachable()) {
        m_block = { };
        return;
    }
    if (block.frequency() == BlockFrequency::Unlikely)
        m_code.makeBlockUnlikely(m_block);
    if (const BitVector* head = m_frames[frameIndex].blocksOfHead; head && !head->get(inputBlock.index()) && leaveCopyOfHeadForCall())
        return;

    if (m_makerOfListsBehindEntry && m_frames[frameIndex].isOfUnit && inputBlock == m_whereListsAreMadeBehindEntry) [[unlikely]] {
        makeListsBehindEntry();
        if (isUnreachable()) {
            m_block = { };
            return;
        }
    }

    BodyTerminator terminator = block.terminator();
    if (terminator.kind() == TerminatorKind::Throw && !block.hasHandler() && standsInTurnThatLooksCountOn()) {
        m_frames[frameIndex].firstOperationToWalk = 0;
        beginTurnAgainInClosedCopy();
        return;
    }

    if (block.hasHandler() && has(Mechanism::Handlers))
        noteStateAtHeadForHandler();
    Frame& entered = m_frames[frameIndex];
    entered.walkedUpTo = 0;
    if (block.numberOfValues())
        walkOperationsOf(inputBlock);
    else {
        standIn(inputBlock);
        entered.firstOperationToWalk = 0;
        entered.hasPassedByWhatFeedsThrow = false;
    }
    walkTerminator(terminator);
}

void Walker::walkStretch(BlockID inputBlock, uint32_t begin, uint32_t end)
{
    SetForScope deeper(m_nestingDepth, m_nestingDepth + 1);
    size_t frameIndex = m_frames.size() - 1;
    standIn(inputBlock);
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
        if (place >= m_frames[frameIndex].walkedUpTo)
            walkOperation(index.value(ValueID(firstValue + place)));
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

bool Walker::endArmWithReturnOf(BlockID inputBlock)
{
    walkTerminator(m_frames.last().index->block(inputBlock).terminator());
    return true;
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
    if (value.opcode() == Opcode::Call && !referenceOf(value.calleeOperand()) && callMethodThroughTable(*this, value))
        return false;
    if (hasTextsInStringBuilders() && bindCastOfTextInStringBuilder(*this, value))
        return false;
    if (value.opcode() == Opcode::Builtin)
        buildFirstReadOfNamedBuiltin(*this, value);
    if ((value.opcode() == Opcode::NewObject || value.opcode() == Opcode::ReleaseHint) && leaveEnvironmentAmongVariables(*this, value))
        return false;
    if (value.opcode() == Opcode::NewObject && has(Mechanism::Guesses) && m_opensGuesses && openGuessAtAllocation(*this, value))
        return true;
    if (has(Mechanism::Guesses) && m_opensGuesses && useKeyOfForInBehindTest(*this, value))
        return true;
    if (has(Mechanism::Guesses) && m_opensGuesses && guessThatConvertedPartOfTextInBuilderHasText(*this, value))
        return true;
    if (value.opcode() == Opcode::SuperConstructList && has(Mechanism::Guesses) && m_opensGuesses && !findOnlyCallee(*this, value, referenceOf(value.parentOperand()))) {
        if (GuessOpened opened = openGuessAt(*this, value); opened != GuessOpened::None)
            return opened == GuessOpened::WithArms;
    }
    bool isBuilt = (hasURLsThatAreNeverMade() && answerFromURLThatIsNeverMade(*this, value))
        || (m_loopsOverGenerators.hasStepsWrittenByHand && answerFromStepWrittenByHand(*this, value))
        || buildDeferredDefinition(*this, value)
        || (has(Mechanism::Folding) && foldOperation(*this, value))
        || (has(Mechanism::Folding) && foldOperationOnKnownTexts(*this, value))
        || (has(Mechanism::BuiltinCalls) && buildDefinitionByWrittenDescriptor(*this, value))
        || (has(Mechanism::EscapeAnalysis) && m_opensGuesses && listStoresOfCopiedConstructor(*this, value))
        || (opcodesOfGeneratorsInLoops.contains(value.opcode()) && has(Mechanism::Inlining) && walkOperationOfGeneratorInLoop(value))
        || buildCoroutineOperation(*this, value)
        || (opcodesThatOpenLoopOverIteratorThatEngineMakesTakes.contains(known) && has(Mechanism::Guesses) && m_opensGuesses && has(Mechanism::TypedOperations) && currentBodyIndex().opcodes().contains(Opcode::IteratorOpen) && isReadThatOnlyCallUsesInChain(*this, value) && openLoopOverIteratorThatEngineMakes(*this, value))
        || (opcodesThatOpenLoopOverIteratorThatEngineMakesTakes.contains(known) && has(Mechanism::Guesses) && m_opensGuesses && has(Mechanism::TypedOperations) && isInLoop() && isReadThatOnlyCallUsesInChain(*this, value) && openTestOfListAtReadOfMethod(*this, value))
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
        || (value.opcode() == Opcode::PropGet && has(Mechanism::TypedOperations) && has(Mechanism::DirectCalls) && leaveReadOfMethodToTableCall(*this, value));
    if (!isBuilt && value.opcode() == Opcode::PropGet && !m_waysIntoClosedCopies.isEmpty() && has(Mechanism::Guesses) && m_opensGuesses && askObjectAtHeadOfTypedCopy(value))
        isBuilt = buildTypedOperation(*this, value);
    if (!isBuilt && has(Mechanism::Guesses) && m_opensGuesses) {
        GuessOpened opened = openGuessAt(*this, value);
        if (opened == GuessOpened::WithArms)
            return true;
        isBuilt = opened == GuessOpened::AsOneOperation || (value.opcode() == Opcode::FindsDeclaredMember && askGuardsAboutDeclaredMember(*this, value)) || ((value.opcode() == Opcode::IsPlainInstanceOf || value.opcode() == Opcode::IsPlainHeirOf) && askCellWhetherItIsPlainInstance(*this, value)) || (value.opcode() == Opcode::FindsNoMember && askCellAndGuardsWhetherNoMemberIsFound(*this, value));
    }
    if (!isBuilt && value.opcode() == Opcode::PropGet && has(Mechanism::TypedOperations))
        isBuilt = buildReadWhereSomeReceiversLackField(*this, value);
    if (!isBuilt && m_explainsClosedAccessesTo) [[unlikely]]
        explainClosedAccess(*this, value, *m_explainsClosedAccessesTo);
    if (!isBuilt)
        isBuilt = buildOperatorBesideOneNumber(*this, value) || buildAccessByIndexOffExpectedView(*this, value) || buildReadByIndexOffAnyBase(*this, value) || askPlainVectorBehindRow(*this, value);
    if (!isBuilt && callThroughTargetResolvedOnce(*this, value))
        return false;
    if (!isBuilt && !replaceOperation(*this, value))
        copy(value);
    return false;
}

bool Walker::walkThroughChain(const BodyValue& written)
{
    using Chain = bool (Walker::*)(const BodyValue&);
    static constexpr auto chains = []<size_t... opcodes>(std::index_sequence<opcodes...>) {
        return std::array<Chain, sizeof...(opcodes)> { &Walker::walkThroughChainOf<static_cast<Opcode>(opcodes)>... };
    }(std::make_index_sequence<numberOfEnumerators<Opcode>>());
    if (!accessesUnderComputedKeys.contains(written.opcode())) [[likely]] {
        if (!m_waysIntoClosedCopies.isEmpty()) {
            proveWhatLooksCountOnAt(written);
            if (askLesserBoundAt(written))
                return false;
        }
        return (this->*chains[static_cast<size_t>(written.opcode())])(written);
    }
    BodyValue value = written;
    if (accessesUnderComputedKeys.contains(written.opcode())) {
        PropertyKeyID chosen;
        value = accessAsWalked(written, &chosen);
        switch (chosen ? compareKeyOfAccessWithSymbol(written, chosen) : KeyOfAccess::IsAnyValue) {
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
    if (!m_waysIntoClosedCopies.isEmpty())
        proveWhatLooksCountOnAt(value);
    return (this->*chains[static_cast<size_t>(value.opcode())])(value);
}

static constexpr OpcodeSet readsThatMemoryAnswers { Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::PropGet, Opcode::PrivateGet, Opcode::ElemGet, Opcode::PropHas, Opcode::PrivateHas };

bool Walker::checkWhatIsRememberedOf(const BodyValue& read)
{
    if (isInLoop() && ((read.opcode() == Opcode::ElemGet && findListTestedInFrontOfLoop(*this, read)) || readsLengthOfViewTestedInFrontOfLoop(*this, read))) {
        ++m_numberOfProofsThatNoTestTells;
        return false;
    }
    Reference remembered = referenceOf(read.id());
    TypeID type = remembered ? typeOf(remembered) : TypeID();
    if (!type || !m_types.at(type).isJavaScriptValue() || (m_types.at(type).primitives & Primitive::Absent)) {
        ++m_numberOfProofsThatNoTestTells;
        return false;
    }
    bool mayBeNaN = m_types.at(type).primitives & Primitive::Double;
    WalkState believed = m_state;
    auto goOnAsBelieved = makeScopeExit([&] {
        m_state = WTF::move(believed);
        Effects effects = Effects::none();
        effects.mayCollect = true;
        m_state.invalidate(effects);
    });
    {
        SetForScope withoutAnswersOfState(m_mechanisms, withoutMechanismsMissingDependencies(m_mechanisms - Mechanism::Folding));
        SetForScope whereItStands(m_buildsWhereItStands, true);
        if (walkThroughChain(read)) {
            goOnAsBelieved.release();
            return true;
        }
    }
    Reference found = referenceOf(read.id());
    if (!isInLoop() && !isUnreachable() && !hasFailed() && found && (!found.isValue() || found == remembered)) {
        m_state.clear();
        SetForScope whereItStands(m_buildsWhereItStands, true);
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
        ++m_numberOfChecksThatWalkAnswers;
    if (isUnreachable() || hasFailed() || isAnsweredByWalk)
        return false;
    auto isSame = [&](Reference left, Reference right) -> Reference {
        if (!left || !right || isUnreachable())
            return { };
        Draft comparison(Opcode::StrictEq, m_origin);
        chooseConservativeFields(comparison);
        comparison.setLeftOperand(left);
        comparison.setRightOperand(right);
        return buildTestOfCheck(WTF::move(comparison));
    };
    auto check = [&](Reference holds) {
        if (holds && holds.isValue() && !isUnreachable())
            checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::WhatIsRemembered, m_origin);
    };
    Reference areSame = isSame(found, remembered);
    if (!mayBeNaN) {
        check(areSame);
        return false;
    }
    Reference foundIsItself = isSame(found, found);
    Reference rememberedIsItself = isSame(remembered, remembered);
    check(isSame(areSame, foundIsItself));
    check(isSame(foundIsItself, rememberedIsItself));
    return false;
}

void Walker::noteClosuresThatAllocationAloneTakes(const BodyValue& allocation)
{
    m_closuresThatAllocationAloneTakes.shrink(0);
    auto made = allocation.as<Opcode::NewObject>();
    if (!plan().layout(made.layout()).environmentOfDeferredClosures)
        return;
    const BodyIndex& index = currentBodyIndex();
    for (ValueID operand : made.valuesOperands()) {
        ValueID written = operand;
        while (written && !index.isParameter(written) && index.hasOneUse(written) && inputOperationsThatOnlyHandOn.contains(index.opcodeOf(written)))
            written = index.value(written).operand(0);
        if (!written || index.isParameter(written) || !index.hasOneUse(written) || index.opcodeOf(written) != Opcode::NewClosure || index.blockOf(written) != index.blockOf(allocation.id()))
            continue;
        if (Reference closure = referenceOf(operand); closure && closure.isValue())
            m_closuresThatAllocationAloneTakes.append(closure);
    }
}

void Walker::walkOperation(const BodyValue& value)
{
    if (!m_triesOfLoops.isEmpty()) {
        if (m_operationsBeingWalkedInFrames.size() < m_frames.size())
            m_operationsBeingWalkedInFrames.grow(m_frames.size());
        m_operationsBeingWalkedInFrames[m_frames.size() - 1] = value.id();
    }
    ++m_numberOfValuesWalked;
    ++m_numberOfValuesWalkedAndKept;
    m_opcodeBeingWalked = value.opcode();
    if (value.opcode() == Opcode::LoopHint && m_frames.last().headOfTurnsWrittenOut == m_frames.last().block)
        return;
    if (m_makerOfListsBehindEntry && m_frames.last().isOfUnit && leaveArgumentsObjectToWhereListsAreMade(value)) [[unlikely]]
        return;
    if (value.opcode() == Opcode::NewObject)
        noteClosuresThatAllocationAloneTakes(value);
    SetForScope valueBeingWalked(m_valueBeingWalked, value.id());
    FunctionID function = currentFunction();
    m_origin = value.origin();
    m_origin.inlineFrame = m_frames.last().inlineFrame;
    m_callIsAccountedFor = false;
    if (checksProofsOf(ProofSource::Lifetime) && value.id().index() + 1 == m_frames.last().endOfValues && (m_frames.size() == 1 || m_frames.last().returnsInPlaceOfUnit)) {
        endLivesInFrontOfCallReturnedAtOnce(value);
        if (isUnreachable())
            return;
    }
    if (value.opcode() == Opcode::FinishObject) {
        showThatLiteralHoldsWhatIsHeldFilled(value);
        if (isUnreachable())
            return;
    }
    const ThinSummary& summary = facts().summaries().at(function);
    if (std::span<const SummaryMakerThatEndsWhereItLeaves> makers = summary.makersThatEndWhereTheyLeave.span(); !makers.empty()) {
        showThatMakersThatLeaveAtHoldWhatIsHeldFilled(makers, value.id().index(), false);
        if (isUnreachable())
            return;
    }
    if (constantsInFormOfUses.contains(value.opcode())) {
        copy(value);
        return;
    }
    if (std::span<const SummaryScopeStoredFirst> scopes = summary.scopesStoredFirst.span(); !scopes.empty()) {
        holdSlotsOfScopesThatLeaveAt(scopes, value.id().index(), false);
        if (isUnreachable())
            return;
    }
    if (std::span<const LinkedVariableStoredFirst> claims = m_frames.last().variablesStoredFirst; !claims.empty()) {
        Vector<FunctionID, 4> used;
        listFunctionsUsedByOperation(program(), currentBodyIndex(), value, used);
        holdVariablesStoredFirstWhereFunctionsAreUsed(claims, used.span(), value.opcode() == Opcode::PutModuleVariable ? value.variable() : VariableID());
        if (isUnreachable())
            return;
    }
    if (value.opcode() == Opcode::GetModuleVariable) {
        holdVariableOfAnotherFileStoredFirst(value.variable());
        if (isUnreachable())
            return;
    }
    SetForScope isWalkingAccess(m_isWalkingAccess, effectRuleOf(value.opcode()) == EffectRule::Access);
    SetForScope resultOfRead(m_resultOfReadBeingTyped, value.opcode() == Opcode::ElemGet ? asFarAsUnitMayAsk(facts().linked().resultOfReadByKey(function, value.id())) : TypeID());

    std::optional<StringsInPlaceOfTextsInBuilders> strings;
    if (hasTextsInStringBuilders()) {
        SetForScope stringIsNoPartOfAccess(m_isWalkingAccess, false);
        strings.emplace(*this, value);
        if (isUnreachable())
            return;
    }
    std::optional<ArgumentsMadeWhereTheyLeave> arguments;
    if (opcodesThatMayTakeArgumentsLast.contains(value.opcode()) && has(Mechanism::EscapeAnalysis) && currentBodyIndex().holdsAnyOf(opcodesThatMakeArguments)) {
        arguments.emplace(*this, value);
        if (isUnreachable())
            return;
    }
    uint32_t numberOfRecordsInFront = m_code.numberOfValues();
    uint32_t firstRecordOfOperation = numberOfRecordsInFront;
    uint32_t numberOfOperationsSeenIfRunAgainInFront = m_numberOfOperationsBuiltThatAreSeenIfRunAgain;
    uint32_t numberOfTurnsThatBeganAgainInFront = m_numberOfTurnsThatBeginAgain;
    if (strings && strings->bindsWhatNoLookHasSeen())
        numberOfRecordsInFront = UINT32_MAX;
    if (!(strings && strings->hasBoundOperation()) && walkThroughChain(value))
        return;

    bool isAnsweredByBuilder = strings && strings->bindsWhatNoLookHasSeen();
    if (checksProofs() && !isAnsweredByBuilder && readsThatMemoryAnswers.contains(value.opcode()) && !isUnreachable() && !hasFailed()) {
        bool hasBuiltOnlyConstants = has(Mechanism::Folding);
        bool mayBeWalkedAgainUnseen = numberOfOperationsSeenIfRunAgainInFront == m_numberOfOperationsBuiltThatAreSeenIfRunAgain;
        for (uint32_t index = numberOfRecordsInFront; index < m_code.numberOfValues(); ++index) {
            hasBuiltOnlyConstants &= m_code.isConstant(ValueID(index));
            Opcode built = m_code.at(ValueID(index)).opcode;
            mayBeWalkedAgainUnseen &= Operation::opcodesWithOverload.contains(built) || (declaredBoundInTable(built).mayBeRunAgainUnseen() && !operandsMayRunCode(built));
        }
        Reference answer = referenceOf(value.id());
        if (answer && (hasBuiltOnlyConstants || (mayBeWalkedAgainUnseen && comesFromInFrontOfLoopAround(answer))) && checkWhatIsRememberedOf(value))
            return;
    }

    const LinkTables& linked = facts().linked();
    static const OpcodeSet opcodesThatMayRunOtherCode = opcodesWhere([](Opcode opcode) {
        return declaredBoundInTable(opcode).mayRunOtherCode() || operandsMayRunCode(opcode);
    });
    if (!opcodesThatMayRunOtherCode.contains(value.opcode()) || linked.calls.isEmpty())
        return;
    FunctionID callee = linked.calleeAt(function, value.id());
    if (!callee || (m_callIsAccountedFor && m_calleeAccountedFor == callee))
        return;
    if (isUnreachable() && m_numberOfTurnsThatBeginAgain != numberOfTurnsThatBeganAgainInFront) {
        bool hasBuiltCall = false;
        for (uint32_t index = firstRecordOfOperation; index < m_code.numberOfValues() && !hasBuiltCall; ++index)
            hasBuiltCall = declaredBoundInTable(m_code.at(ValueID(index)).opcode).mayRunOtherCode();
        if (!hasBuiltCall)
            return;
    }
    leaveAnythingAt(callee);
}

ALWAYS_INLINE void Walker::walkJump(const BodyTerminator& terminator)
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
    if (walker.hasTextsInStringBuilders() || !condition || !condition.isValue())
        return false;
    Opcode opcode = walker.code().at(condition.id()).opcode;
    if (opcode == Opcode::Double || opcode == Opcode::String)
        return false;
    TypeID type = walker.typeOf(condition);
    return type && isBooleanType(type) && primitivesInID(type) == Primitive::Boolean;
}

bool Walker::handTurnOverWhereLesserBoundIsReached(const BodyTerminator& terminator)
{
    if (m_waysIntoClosedCopies.isEmpty())
        return false;
    const WayIntoClosedCopy& way = m_waysIntoClosedCopies.last();
    if (!way.headAsksLesserBound || way.frameIndex != m_frames.size() - 1 || terminator.block() != way.inputHead)
        return false;
    Reference condition = referenceOf(terminator.as<TerminatorKind::Branch>().conditionOperand());
    if (!condition || !condition.isValue() || !isAsBranchTakesItInWalk(*this, condition) || !mayBeginTurnAgainInClosedCopy()) {
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
    if (Reference bound = m_waysIntoClosedCopies.last().boundBesideLesserBound) {
        BodyValue relation = currentBodyIndex().value(terminator.as<TerminatorKind::Branch>().conditionOperand());
        Reference goesOn = buildOperatorOnIntegersOfLoop(*this, relation.opcode(), referenceOf(relation.operand(0)), bound, m_origin);
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
    beginTurnAgainInClosedCopy();
    m_block = { };
    m_state.clear();
    return true;
}

void Walker::walkTerminator(const BodyTerminator& terminator)
{
    if (isUnreachable() || m_hasFailed)
        return;
    size_t frameIndex = m_frames.size() - 1;
    m_origin = terminator.origin();
    m_origin.inlineFrame = m_frames.last().inlineFrame;
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
    if (std::span<const LinkedVariableStoredFirst> claims = m_frames[frameIndex].variablesStoredFirst; !claims.empty()) {
        Vector<FunctionID, 4> used;
        listFunctionsUsedBy(program(), currentBodyIndex(), terminator, true, true, VariableID(), used);
        holdVariablesStoredFirstWhereFunctionsAreUsed(claims, used.span(), VariableID());
        if (isUnreachable())
            return;
    }
    if (hasTextsInStringBuilders())
        takeTextsOutOfStringBuildersAtEndOfWay(*this, terminator);

    if (terminator.kind() == TerminatorKind::ReturnCall) {
        for (TailCast cast : terminator.as<TerminatorKind::ReturnCall>().casts())
            checkInt32Promise(*this, cast.declared, terminator.operand(0), m_origin);
    }

    if (terminator.kind() == TerminatorKind::Jump && checksProofsOf(ProofSource::Lifetime)) {
        endLivesAtJump(terminator.next());
        if (isUnreachable())
            return;
    }
    if (terminator.kind() == TerminatorKind::Jump && !hasTextsInStringBuilders()) {
        walkJump(terminator);
        return;
    }
    if (terminator.kind() == TerminatorKind::Branch) {
        const BodyIndex& index = *m_frames.last().index;
        ValueID asked = terminator.as<TerminatorKind::Branch>().conditionOperand();
        Opcode opcodeAsked = asked ? index.opcodeOf(asked) : Opcode::Parameter;
        if (opcodeAsked == Opcode::StrictEq)
            askShapeInPlaceOfTagAtBranch(terminator);
        else if (opcodeAsked == Opcode::ToBoolean) {
            ValueID tested = index.value(asked).operand(OperandIndex::ToBoolean::value);
            Opcode opcodeTested = tested ? index.opcodeOf(tested) : Opcode::Parameter;
            if ((opcodeTested == Opcode::StrictEq || opcodeTested == Opcode::LogicalNot) && endBlockInAnotherWay(terminator))
                return;
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

    std::optional<StringsInPlaceOfTextsInBuilders> strings;
    if (hasTextsInStringBuilders() && terminatorTakesTextInStringBuilder(*this, terminator)) {
        strings.emplace(*this, terminator);
        if (isUnreachable())
            return;
        if (terminatorTakesTextInStringBuilder(*this, terminator)) {
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
        if (onlyTarget && draft)
            draft->become(TerminatorKind::Jump);
    }

    if (isBranch && !onlyTarget && isAsBranchTakesItInWalk(*this, condition)) {
        RELEASE_ASSERT(terminator.numberOfTargets() == 2);
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
            if (hasTextsInStringBuilders() && edgeLeavesLoopOfTextInStringBuilder(*this, target.block())) {
                TextsTakenOutOnEdge textsTakenOut;
                takeTextsOutOfStringBuildersOnEdge(*this, target.block(), target, arguments, textsTakenOut);
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
        hearEffectsInTries(effects);
        m_state.invalidate(effects);
        if (effects.mayThrow && has(Mechanism::Handlers))
            noteStateAtThrowForHandler(draft->operands());
    }
    if (draft && checksProofsOf(ProofSource::Lifetime) && (draft->kind == TerminatorKind::Throw || draft->kind == TerminatorKind::Rethrow)) {
        endLivesAtThrow(draft->operands());
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
        ValueID cell = arguments[position] ? rootOfCellInSweptCode(code, arguments[position]) : ValueID();
        if (!cell || code.isConstant(cell) || code.at(cell).opcode != Opcode::NewClosure || code.at(cell).blockIndex != call.blockIndex)
            continue;
        if (walker.facts().linked().onlyCallsWhatArrivesAt(call.function(), position))
            return true;
    }
    return false;
}

bool Walker::staysTailCall(Reference call) const
{
    if ((!returnsResultOfFunction(unit().role) && !m_makerOfListsBehindEntry) || isUnderHandler() || m_code.at(m_block).handler || m_code.holdsFrameCells())
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
        while (opcodesThatGiveSameCell.contains(m_code.at(callee).opcode))
            callee = m_code.at(callee).operand(0);
        bool entersNativeCode = m_code.at(callee).opcode == Opcode::Builtin && std::ranges::none_of(program().builtins().overloadSummariesOf(m_code.at(callee).builtin()), [](const BuiltinOverloadSummary& row) {
            return row.callee == BuiltinCallee::Function;
        });
        if (entersNativeCode)
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
    }
    return returned == conventions.resultRepresentation(unit()) && record.representation == returned;
}

void Walker::walkReturn(const BodyTerminator& terminator)
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
        finishBirthAtReturnOfClassConstructor(*this, value, m_origin);
        showWhatIsHeldFilledAtReturnOfClassConstructor(*this, value, m_origin);
        if (isUnreachable())
            return;
    }
    if (!m_generatorsWalkedInConsumers.isEmpty())
        noteCarriedValuesAtReturnOfGenerator();
    endLivesOfCellsThatHaveNotLeft(singleElementSpan(value), m_origin, frame.numberOfValuesBuiltBefore);
    if (isUnreachable())
        return;
    frame.returnsOfCopy->append(Frame::ReturnOfCopy { m_block, value, WTF::move(m_state), frame.index->block(terminator.block()).hasHandler(), !m_opensGuesses });
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
    constexpr RepresentationSet rawFormsOfResults { Representation::Boolean, Representation::Int32, Representation::Double, Representation::OptionalInt64, Representation::OptionalDouble };
    TypeID held = plan().typeHeldOfResult(program(), facts().linked(), m_makerOfListsBehindEntry ? CodeUnit { unit().function, CodeRole::Body } : unit());
    if (held && !m_types.at(held).isJavaScriptValue())
        held = { };
    if (Representation form = plan().conventions().resultRepresentation(unit()); rawFormsOfResults.contains(form) && !isLegalRepresentation(m_types.at(typeOf(value)), form))
        return proveOrTrap(value, held ? m_types.testablePartOf(held) : widestTypeOf(form), ProofSource::ResultOfCall, origin);
    if (held && value.isRecord() && !isShown(value, held))
        buildCheckOfProof(value, held, typeShown(value), ProofSource::ResultOfCall, origin, true);
    return isUnreachable() ? Reference() : value;
}

void Walker::endLivesOfCellsThatHaveNotLeft(std::span<const Reference> handedBack, Origin origin, uint32_t numberOfValuesBuiltBefore)
{
    if (!checksProofsOf(ProofSource::Lifetime) || !m_state.holdsFreshObjects())
        return;

    BitVector cellsHandedBack(m_code.numberOfValues());
    for (Reference value : handedBack) {
        if (value && value.isValue())
            cellsHandedBack.quickSet(rootOfCellInSweptCode(m_code, value.id()).index());
    }

    Vector<Reference, 8> cells;
    m_state.forEachFreshObject([&](Reference object) {
        if (object.id().index() < numberOfValuesBuiltBefore || cellsHandedBack.quickGet(object.id().index()))
            return;
        const Operation& made = m_code.at(object.id());
        bool isHeldByCallerToo = made.opcode == Opcode::Parameter && !m_objectsMetAsOne.get(object.id().index());
        if (!isHeldByCallerToo && made.opcode != Opcode::NewClosure && made.representation == Representation::Cell && !(made.opcode == Opcode::NewObject && program().at(made.layout()).isEnvironment()))
            cells.append(object);
        else
            ++m_numberOfFreshCellsLeftAloneAtReturns;
    });

    origin.isTailCall = false;
    for (Reference cell : cells) {
        Draft end(Opcode::EndLifeOfCell, origin);
        end.setCellOperand(cell);
        build(WTF::move(end));
        if (isUnreachable())
            return;
        m_state.markEscaped(cell);
        ++m_numbersOfProofsChecked[static_cast<unsigned>(ProofSource::Lifetime)];
    }
}

bool Walker::endsLivesOnWays() const
{
    return checksProofsOf(ProofSource::Lifetime) && m_state.holdsFreshObjects() && !program().at(currentFunction()).isCoroutine();
}

bool Walker::standsForFrameOfGenerator(Reference state) const
{
    return m_generatorsTakenByConsumers.containsIf([&](const GeneratorTakenByConsumer& taken) { return taken.state == state; })
        || m_generatorsWalkedInConsumers.containsIf([&](const GeneratorWalkedInConsumer& walked) { return walked.state == state; });
}

void Walker::endLivesOfCellsThatOnlyDeadValuesName(Vector<Reference, 16>& named, uint32_t numberOfValuesBuiltBefore)
{
    bool everyNameIsBuilt = true;
    for (size_t place = named.size(); place--;) {
        Reference name = named[place];
        if (name.isRecord())
            continue;
        if (!name.isVirtual() || standsForFrameOfGenerator(name))
            return;
        iterationByIndex(name).forEachValueHeld([&](Reference value) {
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
    endLivesOfCellsThatHaveNotLeft(named.span(), m_origin, numberOfValuesBuiltBefore);
}

void Walker::endLivesAtJump(const BodyTarget& target)
{
    if (!endsLivesOnWays())
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
    endLivesOfCellsThatOnlyDeadValuesName(named, m_frames.size() == 1 ? 0 : frame.numberOfValuesBuiltBefore);
}

void Walker::endLivesAtThrow(std::span<const Reference> thrown)
{
    if (!endsLivesOnWays() || isUnderHandler())
        return;
    Vector<Reference, 16> named;
    named.append(thrown);
    endLivesOfCellsThatOnlyDeadValuesName(named, 0);
}

void Walker::endLivesInFrontOfCallReturnedAtOnce(const BodyValue& call)
{
    if (!endsLivesOnWays() || isUnderHandler())
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
    endLivesOfCellsThatOnlyDeadValuesName(named, 0);
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

    endLivesOfCellsThatHaveNotLeft(values, origin, 0);
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
