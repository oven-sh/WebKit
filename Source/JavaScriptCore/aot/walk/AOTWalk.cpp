/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Mechanism 1: the blocks of a body in the order of the stream, each operation once through the chain, its closed form at the end of it. Never withheld.
// At a suspension: nothing. The operation's effects end what they write, as at a call.
// At a throw: each block of Code gets its handler edge in front of its first operation that may throw, with boxed arguments. A block of a callee stands under the handler of its call.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTCallingConventions.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTModuleEvaluationPlan.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
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
#include "AOTWalkJSONWrites.h"
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

// The emitter of every other call stops the process where a ReturnCall owns the call.
static constexpr OpcodeSet callsWithTailFormInWalk { Opcode::Call, Opcode::CallList, Opcode::CallDirect, Opcode::CallForwardVarargs };

namespace {

// PositionTypes::askedOfEachObject() and askedOfEachLayout(). The program, Link and Plan are done, so a round is made once for the build: TypeArena::rememberRound().
// The numbers are AnswerKeptInArena::FirstOfWalk and up: four questions at two levels for each of the five that answer, and behind them the two questions that are asked at the level Proven alone.
enum class WhoAnswersInWalk : uint8_t { LinkedTablesUnread, LinkedTables, HeldTablesUnread, HeldTables, Nobody };
static constexpr unsigned numberWhoAnswerInWalk = static_cast<unsigned>(WhoAnswersInWalk::Nobody) + 1;
static constexpr unsigned numberOfQuestionsAtTwoLevelsInWalk = 4;
static_assert(static_cast<unsigned>(AnswerKeptInArena::FirstOfWalk) + (2 * numberOfQuestionsAtTwoLevelsInWalk + 2) * numberWhoAnswerInWalk <= static_cast<unsigned>(AnswerKeptInArena::CellsOfFieldArm));
static_assert(2 + static_cast<unsigned>(PositionTypes::QuestionOfEachLayout::FoundAboveObjects) == numberOfQuestionsAtTwoLevelsInWalk && 2 + static_cast<unsigned>(PositionTypes::QuestionOfEachLayout::ReadByNameOffObjects) == numberOfQuestionsAtTwoLevelsInWalk + 1);

template<typename SetID>
static TypeID askOnceForBuild(TypeArena& types, WhoAnswersInWalk who, unsigned question, TypeLevel level, SetID set, uint32_t word, NOESCAPE const ScopedLambda<TypeID()>& askEachMember)
{
    // Whoever plans the guards of parameters walks with an arena of the program, which keeps no answer from outside.
    if (types.keeps() != ArenaKeeps::UntilReset)
        return askEachMember();
    unsigned place = 2 * numberOfQuestionsAtTwoLevelsInWalk * static_cast<unsigned>(who) + 2 * question + (level == TypeLevel::Expected ? 1 : 0);
    if (question >= numberOfQuestionsAtTwoLevelsInWalk) {
        RELEASE_ASSERT(level == TypeLevel::Proven);
        place = 2 * numberOfQuestionsAtTwoLevelsInWalk * numberWhoAnswerInWalk + 2 * static_cast<unsigned>(who) + question - numberOfQuestionsAtTwoLevelsInWalk;
    }
    uint8_t number = static_cast<uint8_t>(static_cast<unsigned>(AnswerKeptInArena::FirstOfWalk) + place);
    TypeArena::SetOfRound round = types.setOfRound(set);
    if (std::optional<TypeArena::WordsOfRound> kept = types.rememberedRound(number, 1, round, word))
        return TypeID::fromBits((*kept)[0]);
    TypeID answer = askEachMember();
    types.rememberRound(number, 1, answer ? 1 : 0, round, word, { answer.bits() });
    return answer;
}

// PositionTypes::knowingNothing(), but that it keeps the rounds. What is written of a name is asked of every layout of a family, at every read that a guess looks at.
class PositionsKnowingNothingInWalk final : public PositionTypes {
    WTF_MAKE_NONCOPYABLE(PositionsKnowingNothingInWalk);
public:
    explicit PositionsKnowingNothingInWalk(TypeArena& types)
        : m_types(types)
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

private:
    TypeArena& m_types;
};

class PositionsOfWalk final : public PositionTypes {
    WTF_MAKE_NONCOPYABLE(PositionsOfWalk);
public:
    // `tables`: Facts::linked(), or Facts::held() for whoever only guesses.
    // `believesTables` is false for Walker::checkProof() alone: what is left is what holds whatever Link says, because somebody enforces it: the run time, a guard, the making of the thing,
    // or, of a place that compiled code alone fills (a result, a variable, a capture, a field of an environment), the checks that stand where a value goes into it: Walker::checkProofsOfWhatIsStoredBy().
    // Such a check says nothing of the order of things: that no read comes in front of the first store is believed of no place. It keeps no round.
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
        bool isFilledByCompiledCodeAlone = m_believesTables || !m_plan.runTimeMayFill(variable);
        TypeID proven = variable.index() < m_linked.variables.size() && isFilledByCompiledCodeAlone ? filledByCompiledCode(level, &m_linked.variables[variable.index()]) : TypeID();
        if (proven && !m_believesTables) {
            VariableInitialization born = m_program.at(variable).initialization;
            proven = m_types.withPrimitives(proven, born == VariableInitialization::Uninitialized ? Primitive::Absent : born == VariableInitialization::Undefined ? Primitive::Undefined : 0);
        }
        // Link and the record of the variable speak of the program as written.
        const DeferredVariable* deferred = level == TypeLevel::Proven ? m_plan.deferredDefinitions().findDeferred(variable) : nullptr;
        return deferred ? m_types.withPrimitives(proven ? proven : TypeID(TypeTable::anything), deferred->mark) : proven;
    }

    TypeID environmentField(TypeLevel level, LayoutID layout, uint32_t field) const final
    {
        TypeID proven = filledByCompiledCode(level, cellAt(m_linked.firstFieldOfLayout, m_linked.fields, layout.index(), field));
        return proven && !m_believesTables ? m_types.withPrimitives(proven, Primitive::Absent) : proven;
    }

    // The rules ask readOfSlot() only where this gives a type. A boxed slot has none, so it says anything where Link has more to say. Elsewhere what is established of the field stands.
    TypeID slot(TypeLevel level, LayoutID layout, uint32_t field) const final
    {
        if (!m_readsTables)
            return { };
        if (!m_plan.isInUse(layout))
            return TypeTable::never;
        const PlacedSlot& placed = m_plan.slot(layout, field);
        if (!placed.hasStorage())
            return { };
        // That every store is held to it is the word of the doors: Walker::checkProof() does not take it.
        if (placed.type && m_believesTables)
            return placed.type;
        // The slot of an accessor holds the getter and the setter, and what arrives at the field goes to the setter. The brand holds the token of its class, which is no value of JavaScript, as is established of the field.
        const Vector<Field>& fields = m_program.at(layout).fields;
        if (placed.holdsAccessor || (field < fields.size() && (fields[field].form.contains(FieldForm::IsAccessor) || fields[field].flags.contains(FieldFlags::IsBrand))))
            return { };
        return readOfSlot(level, layout, field) ? TypeID(TypeTable::anything) : TypeID();
    }

    TypeID readOfSlot(TypeLevel level, LayoutID layout, uint32_t field) const final { return provenOf(level, m_linked.findCellOfField({ layout, field })); }
    TypeID halfOfAccessorOfField(TypeLevel level, LayoutID layout, uint32_t field, bool isSetter) const final
    {
        if (!m_readsTables || !m_believesTables || level != TypeLevel::Proven)
            return { };
        const LinkedAccessorsOfField* laid = m_linked.findAccessorsExpectedIn({ layout, field });
        if (!laid || !(isSetter ? laid->setterIsProven : laid->getterIsProven))
            return { };
        return m_types.forFunction(isSetter ? laid->setter : laid->getter);
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
        return m_plan.isInUse(layout) ? typeReadByNameOffObjectsOf(m_program, m_linked, m_types, layout, key) : TypeID(TypeTable::never);
    }

    LayoutID classAbove(LayoutID classLayout) const final { return m_readsTables && m_believesTables ? classProvenAbove(m_program, m_linked, classLayout) : LayoutID(); }

    // Of a class object and of the prototype object of a class: the closures of the one method that a lookup from it upward finds in every run.
    // Of a built-in object: the member that a lookup from it upward finds in every run, where that is an object or a constant of the table.
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
        // A read that stays a read asks the host, which makes such a member with code of the library when it is first asked for. Where that fails, at the end of the stack, the read gives undefined.
        // buildReadOfBuiltinMember() puts the cell of the built-in in the place of the read, for whose making the run time keeps stack.
        if (isMadeWithCodeOfLibrary(m_program.builtins(), member))
            return { };
        return typeOfBuiltinObject(TypingContext { m_program, m_types, *this }, member);
    }

    TypeID contents(TypeLevel, ObjectID, ContentsSlot) const final { return { }; }

    TypeID awaited(TypeLevel level, ObjectID object) const final
    {
        // Out of Facts::held() alone, for whoever guesses and tests: the host hands a promise that was rejected with nobody waiting to the listeners of the process, and no flow shows that.
        if (!m_readsTables || !m_believesTables || level != TypeLevel::Proven || &m_linked == &m_facts.linked())
            return { };
        return awaitedOfObjectOfCoroutine(m_types, m_linked, &m_linked == &m_facts.linked(), object);
    }
    TypeID resultOfBodyThatNeverSuspends(TypeLevel level, FunctionID function) const final
    {
        if (!m_readsTables || !m_believesTables || level != TypeLevel::Proven)
            return { };
        return returnedByBodyThatNeverSuspends(m_plan.conventions(), m_linked, function);
    }

    // What `return` hands over in a coroutine is not what its caller gets.
    TypeID result(TypeLevel level, FunctionID function, uint32_t resultIndex) const final
    {
        if (level != TypeLevel::Proven || resultIndex || function.index() >= m_linked.functions.size() || m_program.at(function).isCoroutine())
            return { };
        // A raw form rests on the proof: whoever takes the word takes the proof with it.
        if ((!m_readsTables || (!m_believesTables && m_plan.resultsPassNoReturn(function))) && m_plan.conventions().resultRepresentation({ function, CodeRole::Body }) == Representation::JSValue)
            return { };
        return m_linked.functions[function.index()].result;
    }

    // Whatever is withheld in the unit being walked: whoever enters a function behind its guard has to have proven what the guard tests.
    TypeID entryParameter(TypeLevel level, FunctionID function, uint32_t entryParameter) const final { return level == TypeLevel::Proven ? m_plan.conventions().typeBehindGuard(function, entryParameter) : TypeID(); }
    TypeID resultOfSelector(TypeLevel, uint32_t selector) const final { return m_plan.conventions().methodSelector(selector).typeOfResult; }
    TypeID capture(TypeLevel level, FunctionID function, uint32_t capture) const final { return filledByCompiledCode(level, cellAt(m_linked.firstCaptureOfFunction, m_linked.captures, function.index(), capture)); }
    TypeID resultOfCallBeingTyped(TypeLevel level, uint32_t resultIndex) const final { return m_resultOfCallBeingTyped && level == TypeLevel::Proven && !resultIndex ? *m_resultOfCallBeingTyped : TypeID(); }
    TypeID resultOfReadBeingTyped(TypeLevel level) const final { return m_resultOfReadBeingTyped && level == TypeLevel::Proven ? *m_resultOfReadBeingTyped : TypeID(); }
    TypeID contentsBeingTyped(TypeLevel level) const final { return m_contentsBeingTyped && level == TypeLevel::Proven ? *m_contentsBeingTyped : TypeID(); }

private:
    // A method: its closures. An accessor: what Link has proven of the getter's result, and undefined where there is no getter.
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

    static const LinkedCell* cellAt(const Vector<uint32_t>& firstOfOwner, const Vector<LinkedCell>& cells, uint32_t owner, uint32_t index)
    {
        if (owner + 1 >= firstOfOwner.size() || index >= firstOfOwner[owner + 1] - firstOfOwner[owner])
            return nullptr;
        return &cells[firstOfOwner[owner] + index];
    }

    // The scan and Link let `absent` arrive at whatever starts empty, so a type without it says that no read comes in front of the first store.
    TypeID provenOf(TypeLevel level, const LinkedCell* cell) const
    {
        return m_believesTables ? filledByCompiledCode(level, cell) : TypeID();
    }

    TypeID filledByCompiledCode(TypeLevel level, const LinkedCell* cell) const
    {
        return m_readsTables && level == TypeLevel::Proven && cell && cell->everyWriterIsKnown ? cell->proven : TypeID();
    }

    const Program& m_program;
    const Facts& m_facts;
    const LinkTables& m_linked;
    const Plan& m_plan;
    TypeArena& m_types;
    bool m_readsTables;
    bool m_believesTables;
    WhoAnswersInWalk m_whoAnswers;
    // Null: nobody hands these positions the type of a call. So in a slow body, for what is held and where a proof is checked.
    const TypeID* m_resultOfCallBeingTyped;
    const TypeID* m_resultOfReadBeingTyped;
    const TypeID* m_contentsBeingTyped;
};

} // anonymous namespace

// ---- The rule of Narrowed

static ValueID valueBehindNamesInWalk(const Code& code, ValueID value)
{
    while (value && opcodesThatGiveSameValue.contains(code.at(value).opcode)) {
        ValueID source = code.at(value).valueOperand();
        // An operand is older than its user. Parsed text that says otherwise must not make this loop forever.
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

// Code keeps the first of the edges that enter a block, and no list of them. The block on the edge of a case has that edge alone, however many cases the switch has.
// For any other: the walk makes the block on an edge shortly after the block that the edge leaves, so the search starts there and goes backward.
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

// `Record` is the Draft that is about to stand in `block`, or the Operation that does.
// Null: it names no test and no case that speaks of its value, or the refusal of a test that may have turned the value away falsely.
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

        // A case speaks of the subject alone, so it cuts the type that the subject had in front of the terminator, never that of the holder.
        if (terminator->kind == TerminatorKind::SwitchOnShape)
            return { };
        ValueID subject = terminator->subjectOperand();
        ValueID read = slotReadBehindNamesInWalk(code, subject);
        if (!read || !code.at(subject).type)
            return { };
        return narrowHolderOfSubjectByCase(context, *terminator, code.at(subject).type, code.at(read), typeOfValue, narrowed.caseIndex());
    }

    // A value that the branch asks directly is its own test.
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

    // The value may be a later name of the operand than the test took. It is the same value, so tests that have passed add up.
    typesOfTestOperands[operandIndex] = typeOfValue;
    TypeID type = narrowOperandByTest(context, testOperation, OperandTypes { typesOfTestOperands.span() }, operandIndex, side);
    if (side == EdgeSide::Taken || !type || typeOfValue == inFrontOfTest)
        return type;

    // The code of the test goes by the type that its operand had in front of it. Its refusal takes away of the later name no more than it takes away of that type.
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

// ---- What the rows say

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

// The record has a type all the same, so that it is a value that ResultAt can name.
template<typename Record>
static bool holdsNoneOfItsResultsInWalk(const Program& program, const Record& record)
{
    return opcodesThatMayHaveSeveralResults.contains(record.opcode) && numberOfResultsOf(program, record);
}

// ---- The unit

Walker::Walker(const WalkInputs& inputs, TypeArena& types, Arrivals& arrivals)
    : m_inputs(inputs)
    , m_types(types)
    , m_arrivals(arrivals)
    , m_mechanismsOfBuild(withoutMechanismsMissingDependencies(inputs.options.mechanisms))
    , m_mechanisms(m_mechanismsOfBuild)
    , m_positionsOfBuild(makeUniqueWithoutFastMallocCheck<PositionsOfWalk>(inputs.program, inputs.facts, inputs.facts.linked(), inputs.plan, types, m_mechanismsOfBuild.contains(Mechanism::TypedOperations), true, &m_resultOfCallBeingTyped, &m_resultOfReadBeingTyped, &m_contentsBeingTyped))
    , m_positionsOfSlowBody(makeUniqueWithoutFastMallocCheck<PositionsOfWalk>(inputs.program, inputs.facts, inputs.facts.linked(), inputs.plan, types, false))
    , m_positions(m_positionsOfBuild.get())
    , m_positionsKnowingNothing(makeUniqueWithoutFastMallocCheck<PositionsKnowingNothingInWalk>(types))
    , m_heldPositions(makeUniqueWithoutFastMallocCheck<PositionsOfWalk>(inputs.program, inputs.facts, inputs.facts.held(), inputs.plan, types, m_mechanismsOfBuild.contains(Mechanism::TypedOperations)))
    , m_explainsClosedAccessesTo(inputs.options.explainsClosedAccessesTo)
{
    if (inputs.options.checksProofs)
        m_positionsWithoutProofs = makeUniqueWithoutFastMallocCheck<PositionsOfWalk>(inputs.program, inputs.facts, inputs.facts.linked(), inputs.plan, types, m_mechanismsOfBuild.contains(Mechanism::TypedOperations), false);
    m_state.setProgram(inputs.program);
    m_state.setSlotPlan(inputs.plan.slots());
    m_state.setNumbers(m_numbersOfStates);
    m_state.setAnswers(m_answersOfStates);
}

Walker::~Walker() = default;

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

void Walker::beginUnit(CodeUnit unit)
{
    const Function& function = program().at(unit.function);

    // The slow version is the closed body, and nothing that Link has proven. Of the mechanisms it keeps what makes it smaller:
    // Folding, without which it would enter blocks that the scan has passed over (ThinSummary::blocksNeverEntered), and direct calls, which need no value of the callee.
    bool isSlowBody = unit.role == CodeRole::SlowBody;
    m_mechanisms = isSlowBody ? m_mechanismsOfBuild & Mechanisms { Mechanism::Folding, Mechanism::DirectCalls } : m_mechanismsOfBuild;
    m_positions = isSlowBody ? m_positionsOfSlowBody.get() : m_positionsOfBuild.get();

    m_code.reset(unit);
    m_state.clear();
    m_numbersOfStates.clear();
    m_answersOfStates.clear();
    // Two doors ask whether it is computed (ArgumentsMadeWhereTheyLeave, KnownThreading::letTerminatorBeWalkedForEachWay()): by this unit, not by whatever the worker walked before.
    m_liveness.forget();
    m_block = { };
    m_frames.shrink(0);
    m_copyThatMakesNewList = { };
    m_opensGuesses = true;
    m_numberOfGuessesOpened = 0;
    m_numberOfLoopsBuiltTwice = 0;
    m_numberOfAssignmentsWrittenOut = 0;
    m_numberOfJoinsWalkedForEachWay = 0;
    m_numberOfTurnsThatBeginAgain = 0;
    m_fillersOfAllocations.clear();
    m_numberOfTypedResultsSelected = 0;
    m_numberOfSlicesMadeOfElementsInSight = 0;
    m_countsOfUnit.shrink(0);
    m_numberOfValuesWalked = 0;
    m_keptByStrings = nullptr;
    m_numbersOfProofsChecked.fill(0);
    m_functionsInReachOfUnit.clear();
    m_listsOfMembersByTag.shrink(0);
    m_membersByTagOfSets.clear();
    m_numberOfProofsThatNoTestTells = 0;
    m_callIsAccountedFor = false;
    m_hasFailed = false;
    m_failure = { };
    m_origin = { .position = function.position, .inlineFrame = 0, .isTailCall = false, .isPartOfConstruction = false };
    m_openEdges.shrink(0);
    m_firstFreeOpenEdge = noOpenEdge;
    m_pendingBlocks.shrink(0);
    m_bindings.shrink(0);
    m_numberOfClosedArms = 0;
    m_sharedConstants.shrink(0);
    m_numberOfSharedConstants = 0;
    m_holdsReadsOfVariablesAsConstants = false;
    m_narrowedTypedLast = { };
    m_copiesOfConstants.shrink(0);
    m_numberOfCopiesOfConstants = 0;
    m_iterationsByIndex.shrink(0);
    m_iterationsOfHeads.clear();
    m_inputHeadsOfIterations.clear();
    m_numberOfIterationsInFrontOfTheirLoops = 0;
    m_isLeadingHandlerEdge = false;
    m_openLoops.shrink(0);
    m_placesOfLoopsBehindBlocks.shrink(0);
    m_numberOfLoopsThatMayBeLeft = UINT32_MAX;
    m_numberOfScopesWhereProofsHoldEverywhere = 0;
    m_isWalkingAccess = false;
    m_numberOfCopiesBuiltForSize = 0;
    m_coroutineState = { };
    m_loopsOverGenerators = { };
    m_makesProxyInUnit = false;
    m_environmentCellsOfUnit = { };
    m_environmentCellsOfUnit.mayNameSome = isHandedEnvironmentWithByte(*this, unit.function);
    m_textsInStringBuilders.shrink(0);
    m_hasURLsThatAreNeverMade = false;

    InlineFrame ownFrame { };
    ownFrame.callee = unit.function;
    ownFrame.callPosition = Instruction::noPosition;
    ownFrame.parent = 0;
    ownFrame.module = function.module;
    m_code.addInlineFrame(ownFrame);

    bool bodyStandsBehindUnit = walksInputBody(unit.role) && (unit.role != CodeRole::Body || !function.isCoroutine());
    Frame frame;
    frame.function = unit.function;
    if (bodyStandsBehindUnit) {
        frame.index = facts().bodies().find(unit.function);
        if (frame.index) {
            frame.body = &frame.index->body();
            frame.references.fill(Reference(), frame.index->numberOfValues());
        }
    }
    // Room for the frames that looks lay: Walker::layFrameForLookIntoCopy().
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
    if (!std::exchange(m_walksUnitAgain, false))
        m_loopsOfWalksGivenUp.shrink(0);
    beginUnit(unit);
    bool bodyStandsBehindUnit = walksInputBody(unit.role) && (unit.role != CodeRole::Body || !program().at(unit.function).isCoroutine());

    bool isBuilt;
    if (bodyStandsBehindUnit)
        isBuilt = walkBody();
    else if (unit.role == CodeRole::Body)
        isBuilt = walkRampOfCoroutine(*this);
    else if (unit.role == CodeRole::Serializer)
        isBuilt = walkSerializerUnit(*this);
    else
        isBuilt = walkEntryUnit(*this, unit);
    if (m_walksUnitAgain) {
        m_reports.shrink(0);
        return walk(unit);
    }
    if (!isBuilt && !m_hasFailed)
        fail("the unit was not built, and nobody says why"_s);
    if (m_hasFailed)
        return false;

    if (m_frames.size() != 1)
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

void Walker::fail(ASCIILiteral what)
{
    if (!m_hasFailed) {
        m_hasFailed = true;
        m_failure = what;
        dataLogLn("AOT: the walk of f", unit().function.index(), " ", nameOf(unit().role), " failed, in f", m_frames.isEmpty() ? unit().function.index() : currentFunction().index(), ": ", what);
    }
    // The way on from a failure is unreachable, so the rest of the walk meets no state that it does not know.
    endAsUnreachable(m_origin);
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
    // A way back is closed only at the head of a loop, which a cycle with two ways in does not have. No front end sends one.
    if (index.hasIrreducibleFlow()) {
        fail("the body holds a cycle with two ways in"_s);
        return false;
    }

    takeEntryParameters();
    if (m_environmentCellsOfUnit.mayNameSome && !m_hasFailed)
        markEnvironmentsThatCoroutineIsHanded(*this);
    if (m_hasFailed)
        return false;
    if (has(Mechanism::Arrivals))
        takeArrivals(*this);
    if (checksProofsOf(ProofSource::EntryParameter) && (unit().role == CodeRole::Body || unit().role == CodeRole::CoroutineBody))
        checkProofsOfWhatComesThroughFusedEntry();
    beginModuleInBody();

    for (uint32_t number = 0; number < index.numberOfBlocks() && !m_hasFailed; ++number)
        walkBlock(BlockID(number));
    endBlocksInFrontOfLoops(0, { });
    return !m_hasFailed;
}

TypeID Walker::typeArrivingAtEntryParameter(unsigned index, bool believesCallers)
{
    FunctionID id = unit().function;
    TypingContext withoutFacts { program(), m_types, PositionTypes::knowingNothing(), TypeLevel::Proven };
    const DeclaredType* declared = program().at(id).declaredEntryParameter(index);
    TypeID type = declared ? program().provenTypeOf(*declared) : declaredEntryTypeOf(withoutFacts, id, index);
    if (!type)
        type = TypeTable::anything;

    // Both hold. Where nothing is left of the two the wider one is taken, which claims less.
    TypeID known = has(Mechanism::Arrivals) && believesCallers ? m_arrivals.provenAt(m_types, id, index) : TypeID();
    TypeID both = known && m_types.at(type).isJavaScriptValue() ? m_types.intersectionOf(type, known) : TypeID(TypeTable::never);
    if (!isNeverType(both))
        return both;
    return !declared || !declared->isEstablished() ? m_types.withSeeds(type, UnknownSeed::ParameterOfEscapedFunction) : type;
}

// Code takes the type of an entry parameter here and never again: what holds of it without a test, cut by what the callers have proven. takeArrivals() notes in Code what that leans on.
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
        // Whoever enters the fast version has come through the guard, or has proven as much himself.
        // Where nothing of what arrives passes, nobody enters: the type is what the convention was laid out for.
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

// ---- TestingSwitch::CheckProofsAtRunTime

// WHERE A VALUE GOES INTO A PLACE THAT COMPILED CODE ALONE FILLS, not where it comes out: there are fewer such operations, and of most of them the walk proves on the spot that the value fits, so nothing is built.
// An entry parameter is filled by a direct call, and by the entries and the guard of its function for whoever calls by value, native code among them; a unit that holds its own entry by value asks for itself.
// A variable, a capture and a field of an environment are filled by the operation that stores.
// A result is filled by a return: endWithReturn().
TypeID Walker::typeThatStoreTakesOnWordOfWalk(const Draft& store)
{
    static_assert(OperandIndex::PutSlot::value == OperandIndex::PutSlotOrAdd::value);
    if (store.level() == KnowledgeLevel::Proven)
        return requiredTypeOfOperand(typing(), store, OperandIndex::PutSlot::value);
    Reference value = store.valueOperand();
    TypeID claimed = value && value.isValue() ? typeAsBuilt(value) : TypeID();
    TypeID taken;
    for (LayoutID member : membersOf(m_types, store.receivers())) {
        std::optional<FieldIndex> field = plan().slots().findField(program(), member, store.key());
        TypeID type = field ? plan().slots().slot(member, *field).type : TypeID();
        if (!claimed || !field || (type && !m_types.isSubset(claimed, type)))
            return { };
        if (type)
            taken = taken ? m_types.intersectionOf(taken, type) : type;
    }
    return taken;
}

void Walker::checkProofsOfWhatIsStoredBy(const Draft& draft)
{
    Origin origin = draft.origin();
    switch (draft.opcode) {
    case Opcode::CallDirect:
        checkProofsOfOperandsOfDirectCall(draft);
        return;
    // A store asks that the object is not frozen, and one that may create the property, like a read that may find the slot empty, that no field lies outside its slot and that nothing above answers.
    case Opcode::PutSlotOrAdd:
    case Opcode::GetSlotOrUndefined:
    case Opcode::GetDeferredClosureSlotOrUndefined:
        if (draft.level() == KnowledgeLevel::Proven)
            checkFactsRememberedOfCell(draft.objectOperand(), origin);
        if (draft.opcode == Opcode::PutSlotOrAdd)
            checkThatSlotTakes(draft.valueOperand(), typeThatStoreTakesOnWordOfWalk(draft), origin);
        return;
    case Opcode::GetSlot:
    case Opcode::PutSlot:
        if (draft.opcode == Opcode::PutSlot && draft.level() == KnowledgeLevel::Proven)
            checkFactsRememberedOfCell(draft.objectOperand(), origin);
        checkProofThatSlotIsFilled(draft);
        if (draft.opcode == Opcode::PutSlot)
            checkThatSlotTakes(draft.valueOperand(), typeThatStoreTakesOnWordOfWalk(draft), origin);
        return;
    case Opcode::GetDeferredClosureSlot:
    case Opcode::GetSlotGetter:
    case Opcode::GetSlotSetter:
        checkProofThatSlotIsFilled(draft);
        return;
    case Opcode::PutModuleVariable:
        checkProofOfCopy(draft.valueOperand(), m_positions->variable(TypeLevel::Proven, draft.variable()), ProofSource::Variable, origin);
        return;
    case Opcode::PutEnvironmentField:
        checkProofOfCopy(draft.valueOperand(), m_positions->environmentField(TypeLevel::Proven, draft.layout(), draft.field()), ProofSource::Capture, origin);
        return;
    case Opcode::NewClosure: {
        uint32_t index = 0;
        for (Reference captured : draft.capturesOperands())
            checkProofOfCopy(captured, m_positions->capture(TypeLevel::Proven, draft.function(), index++), ProofSource::Capture, origin);
        return;
    }
    case Opcode::NewObject: {
        if (program().at(draft.layout()).kind != LayoutKind::Environment) {
            std::span<const Reference> values = draft.valuesOperands();
            for (uint32_t index = 0; index < std::min(draft.fields().size(), values.size()); ++index)
                checkThatSlotTakes(values[index], requiredTypeOfOperand(typing(), draft, numberOfNamedOperandsOf(Opcode::NewObject) + index), origin);
            return;
        }
        std::span<const FieldIndex> fields = draft.fields();
        std::span<const Reference> values = draft.valuesOperands();
        for (size_t index = 0; index < std::min(fields.size(), values.size()); ++index)
            checkProofOfCopy(values[index], m_positions->environmentField(TypeLevel::Proven, draft.layout(), fields[index]), ProofSource::Capture, origin);
        return;
    }
    default:
        return;
    }
}

// WHAT GOES INTO A TYPED SLOT. Whoever reads a filled slot trusts the bits. Every door of the run time holds what it stores to the type of the slot (slotAdmits(), branchUnlessTypeAccepts()); a typed store of compiled code
// writes on the walk's word that the type of the value lies within it. Here the VALUE is asked, whatever its record claims: so a claim of the walk's own rules is heard too, which no check at a source holds against anything.
// A raw word tells nothing.
void Walker::checkThatSlotTakes(Reference value, TypeID taken, Origin origin)
{
    if (!value || !value.isValue() || !taken || isNeverType(taken) || isUnreachable())
        return;
    TypeID claimed = typeAsBuilt(value);
    if (!claimed || isNeverType(claimed) || !m_types.at(claimed).isJavaScriptValue() || !m_types.at(taken).isJavaScriptValue() || m_types.at(taken).isAnything())
        return;
    // A raw boolean is a raw word like a raw number; checkProof() would take it for the answer of a test, which has to be true.
    Representation representation = representationOf(value);
    if (representation != Representation::JSValue && representation != Representation::Cell && representation != Representation::Cell32)
        return;
    // An allocation lists the empty word for a field that is not filled yet: the mark of the empty slot, which is no value.
    if (m_types.at(claimed).primitives & Primitive::Absent)
        taken = m_types.withPrimitives(taken, Primitive::Absent);
    origin.isTailCall = false;
    checkProof(value, taken, m_types.withPrimitives(TypeTable::anything, Primitive::Absent), ProofSource::Field, origin);
}

// THAT A SLOT IS FILLED is a proof like any other, whoever gave it: Link (isFilled, isFilledAtEveryRead), a birth's mark, a tag that is taken for the layout. A read that counts on it takes the mark of the empty slot for a value:
// a raw slot gives 0 for undefined. So the slot is asked in front of every such read, but where Code itself shows the test that has passed: the object of the read is what that test has narrowed.
void Walker::checkProofThatSlotIsFilled(const Draft& read)
{
    // A read on a guess asks the cell for itself, and does the read as written where it finds no value.
    if (!checksProofs() || isUnreachable() || read.level() != KnowledgeLevel::Proven)
        return;
    // A RAW NUMBER OR BOOLEAN: the type of the read, cut to what Link has proven of the field, leaves no room for undefined and null, and no test tells anything of the word that the read gives.
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
    // A raw form: the read takes a mark (absent, undefined, null) for a number or a reference where it counts on the declared type, and wherever it gives a raw word. SlotEmitter::holdsDeclaredType() tells a mark from a value.
    test.setHoldsDeclaredType(read.opcode == Opcode::GetSlot && (read.holdsDeclaredType() || givesRawWord));
    Reference isFilled = build(WTF::move(test));
    if (isFilled && isFilled.isValue() && !isUnreachable())
        checkProof(isFilled, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::Field, read.origin());
}

// Whoever calls by value, native code among them, comes into such a unit through a few moves that the emitter lays in front of it: emitFusedCallEntry(). No operation stands for them, so the unit asks for itself, of every caller.
void Walker::checkProofsOfWhatComesThroughFusedEntry()
{
    if (!plan().conventions().holdsFusedCallEntry(unit()))
        return;
    FunctionID id = unit().function;
    BodyBlock entry = m_frames[0].index->block(entryBlock);
    for (unsigned index = 0; index < entry.numberOfParameters() && !isUnreachable(); ++index) {
        TypeID withoutProof = typeArrivingAtEntryParameter(index, false);
        if (TypeID tested = unit().role == CodeRole::Body ? plan().conventions().typeBehindGuard(id, index) : TypeID()) {
            TypeID passed = m_types.intersectionOf(withoutProof, tested);
            withoutProof = isNeverType(passed) ? tested : passed;
        }
        checkProof(referenceBoundTo(entry.parameter(index)), withoutProof, ProofSource::EntryParameter, m_origin);
    }
}

// That every caller is known, and what each of them hands over, is Link's word. What a guard has tested is not.
void Walker::checkProofsOfOperandsOfDirectCall(const Draft& call)
{
    FunctionID callee = call.function();
    const LinkTables& link = facts().linked();
    // The slow version takes nothing for granted.
    if (callee.index() >= link.functions.size() || (call.role() != CodeRole::Body && call.role() != CodeRole::CoroutineBody && call.role() != CodeRole::ParameterGuard))
        return;
    const LinkedFunction& linked = link.function(callee);
    if (!linked.everyCallerIsKnown || plan().conventions().holdsFusedCallEntry({ callee, call.role() }))
        return;

    bool isWalkingBody = unit().role == CodeRole::Body || unit().role == CodeRole::CoroutineBody || unit().role == CodeRole::SlowBody;
    // The ramp of a coroutine hands on what it was handed, and an entry that goes on to the guard leaves the question to the guard.
    if (unit().function == callee && (isWalkingBody ? call.role() == CodeRole::CoroutineBody : call.role() == CodeRole::ParameterGuard))
        return;
    // An entry and the guard are walked with the body: what the callers have left is all there, and it is what the body goes by. A caller is walked earlier, and knows Link's word alone.
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
    case Opcode::GetEncodedModuleVariable:
        return ProofSource::Variable;
    case Opcode::GetCapture:
    case Opcode::GetEnvironmentField:
        return ProofSource::Capture;
    default:
        break;
    }
    if (slotOfHolderOf(opcode) != SlotOfHolder::None)
        return ProofSource::Contents;
    switch (accessClassOf(opcode)) {
    case AccessClass::DirectCall:
    case AccessClass::IndirectCall:
        return ProofSource::ResultOfCall;
    case AccessClass::Slot:
    case AccessClass::ByName:
        return ProofSource::Field;
    case AccessClass::None:
        return ProofSource::OtherOperation;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// WHATEVER A READ CLAIMS OF WHAT IT GIVES, `anything` included, it gives a value of the language: not the empty word, unless its type says so, and no cell that is no object, text, symbol or big integer:
// the pair of an accessor, an environment, a table of the run time. CastEmitter::generateCheckProof() asks that of every value, so a read that claims nothing else gets a check that asks nothing else.
static constexpr OpcodeSet readsThatHandOverWhatLies {
    Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::GetDeferredClosureSlot, Opcode::GetDeferredClosureSlotOrUndefined, Opcode::GetStaticFieldOrAbsent, Opcode::GetEnvironmentField, Opcode::GetModuleVariable,
    Opcode::GetCapture, Opcode::GetGlobalBinding, Opcode::GetArrayElement, Opcode::GetContiguousIndex, Opcode::GetOwnContiguousIndex,
};

// The rule of the row is asked a second time, by somebody who believes nothing that Link says of a position and takes of each operand only what a test could have shown:
// what a list holds is part of the type of the list, and no test of the list sees it. Where the record claims more than that gives, Link alone vouches for the rest.
void Walker::checkProofOfRecord(ValueID record, const Draft& draft, TypeID proven)
{
    // A NEW PROOF ABOUT AN OLD VALUE, whoever has built it: the walk behind a test of the program, a guess behind its own test, a case of a switch, a field that tells its holders apart.
    // That the record stands on the edge on which its test says so is the word of whoever built it. The value is asked as what it was in front of the test.
    if (draft.opcode == Opcode::Narrowed) {
        if (Reference narrowed = draft.valueOperand(); narrowed && narrowed.isValue())
            checkProof(Reference::value(record), typeAsBuilt(narrowed), ProofSource::Test, draft.origin());
        return;
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
    // Nothing stands behind a call that returns in place of the unit. Whoever called the unit looks at what comes back.
    if (unitReturnsAtOnce(draft))
        return;

    TypeID withoutProof = TypeTable::anything;
    // What a producer of several results gives in one place is asked of the producer, whose own rule has many forms. Nothing is taken for known of it.
    if (draft.opcode != Opcode::ResultAt) {
        typesOfOperands(draft);
        for (TypeID& type : m_operandTypes) {
            if (type && m_types.at(type).isJavaScriptValue())
                type = m_types.testablePartOf(type);
        }
        if (TypeID given = Typed::resultTypeOf(TypingContext { program(), m_types, *m_positionsWithoutProofs, TypeLevel::Proven }, draft, OperandTypes { m_operandTypes.span() }))
            withoutProof = given;
    }
    // That a variable is filled where it is read is a proof like any other, whoever gives it: Link, or the type that the front end has written on the variable. The empty word passes for a cell in every test of the tag bits.
    if (draft.opcode == Opcode::GetModuleVariable || draft.opcode == Opcode::GetEnvironmentField || draft.opcode == Opcode::GetCapture)
        withoutProof = m_types.withPrimitives(withoutProof, Primitive::Absent);
    ProofSource source = sourceOfProofInWalk(draft.opcode);
    uint32_t numberChecked = numberOfProofsChecked(source);
    if (withoutProof != proven)
        checkProof(Reference::value(record), withoutProof, source, draft.origin());
    if (numberChecked == numberOfProofsChecked(source) && readsThatHandOverWhatLies.contains(draft.opcode))
        checkThatReadGivesValue(Reference::value(record), source, draft.origin());
}

void Walker::checkThatReadGivesValue(Reference value, ProofSource source, Origin origin)
{
    // A read that claims nothing: the whole word alone asks it.
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
    // A raw word is what it is: whoever made it out of a value had to prove the form or trap.
    Representation representation = representationOf(value);
    if (!proven || isNeverType(proven) || (representation != Representation::JSValue && representation != Representation::Cell))
        return;
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
    if (!own || isNeverType(own) || (representation != Representation::JSValue && representation != Representation::Cell))
        return;
    if (!m_types.at(proven).isJavaScriptValue() || !m_types.at(own).isJavaScriptValue() || m_types.isSubset(own, proven))
        return;
    checkProof(value, proven, own, source, origin);
}

void Walker::checkProof(Reference value, TypeID proven, TypeID withoutProof, ProofSource source, Origin origin)
{
    if (!checksProofsOf(source))
        return;
    // The emitter tests a boxed value, a cell, or the answer of a test. A narrow reference is made a boxed value first; of any other raw word no test tells anything.
    switch (representationOf(value)) {
    case Representation::JSValue:
    case Representation::Cell:
    case Representation::Boolean:
        break;
    case Representation::Cell32:
        value = convert(value, Representation::JSValue, origin);
        if (!value || !value.isValue() || isUnreachable())
            return;
        break;
    default:
        ++m_numberOfProofsThatNoTestTells;
        return;
    }
    TypeID checked = m_types.testablePartOf(proven);
    // The code of a test for one object mentions it: the closures of a function, the namespace of a module. What stands behind a mention is compiled, and the code of a unit may mention only what its function leads to: forEachUnitBehind(), LinkTables::referrers.
    // A value that only passes through a function gives it no such way. There the test asks whether the value is a function, or an object.
    if (Type type = m_types.at(checked); type.hasObjects()) {
        auto isInReach = [&](FunctionID function) {
            return m_functionsInReachOfUnit.ensure(function.index(), [&] {
                return facts().linked().leadsTo(unit(), function);
            }).iterator->value;
        };
        bool namesWhatIsOutOfReach = false;
        m_types.at(type.layouts).forEach([&](LayoutID layout) {
            const Layout& record = program().at(layout);
            if (record.kind != LayoutKind::ModuleNamespace)
                return;
            FunctionID body = record.namespaceOf ? program().at(record.namespaceOf).body : FunctionID();
            namesWhatIsOutOfReach |= !body || !isInReach(body);
        });
        m_types.at(type.objects).forEach([&](ObjectID object) {
            const ObjectRecord& record = m_types.objects().at(object);
            FunctionID behind;
            switch (record.kind) {
            case ObjectKind::Closure:
                behind = FunctionID(record.payload);
                // The mention of its closures brings its class too, with every member that the record of the class creates.
                namesWhatIsOutOfReach |= isClassConstructorOf(program().at(behind).kind) || program().at(behind).constructedClass;
                break;
            case ObjectKind::Namespace:
                behind = program().at(ModuleID(record.payload)).body;
                namesWhatIsOutOfReach |= !behind;
                break;
            default:
                return;
            }
            namesWhatIsOutOfReach |= behind && !isInReach(behind);
        });
        if (namesWhatIsOutOfReach)
            checked = m_types.unionOf(m_types.withoutObjects(checked), type.hasOnlyClosures() ? TypeTable::function : TypeTable::object);
    }
    if (m_types.isSubset(withoutProof, checked)) {
        ++m_numberOfProofsThatNoTestTells;
        return;
    }
    origin.isTailCall = false;
    Draft check(Opcode::CheckProof, origin);
    check.setValueOperand(value);
    check.setCheckedType(checked);
    // That a word is filled is a proof like any other, and the code of a test that does not count on an empty word takes it for a cell.
    check.setTypeWithoutProof(m_types.withPrimitives(withoutProof, Primitive::Absent));
    check.setSource(source);
    build(WTF::move(check));
    ++m_numbersOfProofsChecked[static_cast<unsigned>(source)];
}

// ModuleStart::beginsInBody: the body opens with BeginModuleEvaluation, returns undefined where that says no, and has FinishModuleEvaluation in front of each Return.
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

// ---- What every handler may ask

const Body& Walker::currentBody() const
{
    return *m_frames.last().body;
}

// A chain of calls is copied one into the other, and each copy asks this of the next callee.
bool Walker::isBeingWalked(FunctionID function) const
{
    constexpr size_t mostFramesLookedThrough = 8;
    if (m_frames.size() <= mostFramesLookedThrough) {
        return m_frames.containsIf([&](const Frame& frame) {
            return frame.function == function;
        });
    }
    // The last is for a frame behind which no function stands.
    size_t numberOfFunctions = program().functions.size();
    if (m_numbersOfFramesOfFunctions.isEmpty())
        m_numbersOfFramesOfFunctions.fill(0, numberOfFunctions + 1);
    auto numberOfFramesOf = [&](FunctionID walked) -> uint32_t& {
        return m_numbersOfFramesOfFunctions[walked ? walked.index() : numberOfFunctions];
    };
    // A frame is laid on frames that were counted, or on one that was not and is counted now.
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
    // The longer of the two is followed until they meet. What the list in the tables has on the way goes out, the first name first.
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
    // Two jumps of the same length in a row are taken in one.
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
    Reference reference = narrowestNameOf(frame.references[input.index()]);
    if (!m_state.hasCheckedNames()) [[likely]]
        return reference;
    // An edge may have narrowed the name behind a check in its turn.
    while (Reference checked = m_state.checkedNameOf(reference))
        reference = narrowestNameOf(checked);
    return reference;
}

// To whoever asks, a text in a builder is a string: the type of the builder says nothing of the text, so nobody decides by it. Whoever takes the builder for an operand is stopped in build().
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
        if (binding.frame < m_walker.m_frames.size())
            m_walker.m_frames[binding.frame].references[binding.input.index()] = binding.previous;
    }
    --m_walker.m_numberOfClosedArms;
    m_walker.m_opensGuesses = m_openedGuesses;
}

// ---- Types and forms

OperandTypes Walker::typesOfOperands(const DraftWords& words)
{
    std::span<const Reference> operands = words.operands();
    m_operandTypes.shrink(0);
    m_operandTypes.reserveCapacity(operands.size());
    for (Reference operand : operands)
        m_operandTypes.unsafeAppendWithoutCapacityCheck(typeAsBuilt(operand));
    return { m_operandTypes.span() };
}

TypeID Walker::joinTypes(TypeID first, TypeID second)
{
    return first == second ? first : joinOf(m_types, facts().families(), first, second);
}

// Null: the producer has no such result.
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

// The rule of an allocation reads the place of its prototype as new.target. Any object says that nothing is known of new.target, also of a prototype that is itself a function.
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

TypeID Walker::resultTypeOf(const Draft& draft, bool& listsOwnTypes)
{
    listsOwnTypes = false;
    if (resultRuleInWalk(draft.opcode) == ResultRule::None)
        return { };
    if (draft.opcode == Opcode::Narrowed) {
        Reference value = draft.valueOperand();
        Reference test = draft.testOperand();
        if (!value || !value.isRecord() || (test && !test.isRecord()))
            return { };
        // Whoever builds one has asked what is left of the value first, and build() asks once more. The rule reads the words of the draft, records that never change and the end of a block that has ended.
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
    if (holdsNoneOfItsResultsInWalk(program(), draft))
        return TypeTable::undefined;

    // The two places in which the rules are promised another type than the operand's own.
    typesOfOperands(draft);
    if (draft.opcode == Opcode::ResultAt)
        m_operandTypes[OperandIndex::ResultAt::results] = typeOfResultAskedFor(draft);
    else if (draft.opcode == Opcode::NewObject && draft.prototypeOperand())
        m_operandTypes[OperandIndex::NewObject::prototype] = typeOfNewTargetBehind(draft.prototypeOperand());
    else
        listsOwnTypes = true;
    // A value in another form is the same value.
    if (conversionsOfForm.contains(draft.opcode) && m_operandTypes.size() == 1 && m_operandTypes[0])
        return m_operandTypes[0];
    return Typed::resultTypeOf(typing(), draft, OperandTypes { m_operandTypes.span() });
}

// No arguments: the form of the type.
Representation Walker::formOfParameter(TypeID type, std::span<const Representation> arguments)
{
    RepresentationContext context { program(), plan(), m_types, unit() };
    Representation form = arguments.empty() ? formOfTypeInWalk(context, type) : formOfJoinInWalk(context, type, arguments);
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

// One form for all that a handler edge brings, whichever block throws: nothing can be built on such an edge.
Representation Walker::boxedFormOf(TypeID type)
{
    if (isLegalRepresentation(m_types.at(type), Representation::JSValue))
        return Representation::JSValue;
    return formOfTypeInWalk(RepresentationContext { program(), plan(), m_types, unit() }, type);
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

// A place of m_sharedConstants: the number of the record, and the mark of the one record among those with the same words that has the form of its type.
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
}

Reference Walker::convertWithoutCode(Reference reference, Representation wanted)
{
    // A cell and its boxed form are one word, and still two forms: an emitter that stores what it is handed asks for the form of the place. The Box or Unbox between them makes no code.
    if (!reference || !hasRegister(wanted) || representationOf(reference) == wanted)
        return reference;
    if (!reference.isConstant())
        return { };

    // A constant has no form of its own: each use gets a copy in the form that it wants.
    if (ValueID made = findCopyOfConstant(reference.id(), wanted))
        return Reference::constant(made);
    const Operation& record = m_code.at(reference.id());
    if (!constantsInFormOfUses.contains(record.opcode))
        return { };
    // The cell of a constant is made at run time, so no constant is a narrow reference to one.
    if (!isLegalRepresentation(m_types, record.type, wanted) || (wanted == Representation::Cell32 && m_types.at(record.type).hasCells()))
        return { };
    // Plan gives the literals their words in front of Compile.
    if (wanted == Representation::LiteralString && record.opcode == Opcode::String && liesInThread(record.string()))
        return { };
    if (m_hasFailed)
        return { };

    // As constant() answers to a draft with the words of the record and the form: the type follows from the words, and the form is legal.
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
    if (Reference ready = convertWithoutCode(reference, wanted))
        return ready;
    if (!reference)
        return { };

    // What is built in front of a loop takes no record of the loop's inside, and the state is that of the inside.
    if (Reference made = findConversionMade(*this, reference, wanted); made && (m_numberOfLoopsThatMayBeLeft >= m_openLoops.size() || standsInFrontOfLoop(m_numberOfLoopsThatMayBeLeft, made)))
        return made;
    Reference asHandedIn = reference;
    Representation held = representationOf(reference);
    std::optional<ConversionPath> path;
    if (held != Representation::Unchosen)
        path = findConversion(m_types, typeOf(reference), held, wanted);
    if (!path) {
        fail("a value has no conversion into the form in which it is wanted"_s);
        return { };
    }
    origin.isTailCall = false;
    for (ConversionStep step : *path) {
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

// A Box, an Unbox and a Compress have one operand of the kind Convertible, the type of that operand and no effect. No link of build() but those asked about here has anything to say of them:
// they take no text as a key, no answer is kept under them, they name the cell of their operand, and they take it in the form that it has.
Reference Walker::tryBuildConversionOnStraightRoad(Opcode opcode, Representation to, Reference operand, Origin origin)
{
    constexpr OpcodeSet conversionsOnStraightRoad { Opcode::Box, Opcode::Unbox, Opcode::Compress };
    static_assert((conversionsOnStraightRoad | conversionsOfForm) == conversionsOfForm && (conversionsOnStraightRoad | opcodesThatGiveSameCell) == opcodesThatGiveSameCell);
    static_assert(!conversionsOnStraightRoad.intersects(opcodesThatStateTypes));
    if (!conversionsOnStraightRoad.contains(opcode) || !operand.isRecord() || isUnreachable())
        return { };
    if (hasTextsInStringBuilders() || m_positionsWithoutProofs)
        return { };
    if (!opcodesWithPureDeclaredBound().contains(opcode) || opcodesWithoutEmitter().contains(opcode) || opcodesThatTakeTextsAsKeys.contains(opcode)
        || howOperandsAreTakenBy(opcode) != OperandsAreTaken::AsBuilt || operandKindOf(opcode, 0) != OperandKind::Convertible)
        return { };

    // Whatever findIllTypedOperand() would have to look at twice goes the long way, and so does what ends the way on.
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
    // Asked of the draft as it came: a form may follow from the forms that the other operands have.
    uint32_t numberOfOperands = draft.numberOfOperands();
    if constexpr (std::is_same_v<DraftType, Draft>) {
        constexpr uint32_t mostOperandsTakenByKindAtOnce = 8;
        OperandsAreTaken way = howOperandsAreTakenBy(draft.opcode);
        if (way == OperandsAreTaken::AsBuilt)
            return;
        bool hasOperand = false;
        for (Reference operand : draft.operands())
            hasOperand |= !!operand;
        if (!hasOperand)
            return;
        // Each argument is an entry parameter of the unit that is entered, whatever the others are.
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
        if (way == OperandsAreTaken::ByKind && numberOfOperands <= mostOperandsTakenByKindAtOnce && !hasTextsInStringBuilders() && !testsItsOwnGuess(draft)) {
            std::array<Representation, mostOperandsTakenByKindAtOnce> formsToBring;
            bool everyOperandIsReady = true;
            for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
                formsToBring[operandIndex] = Representation::Unchosen;
                Reference operand = draft.operand(operandIndex);
                TypeID type = operand ? typeAsBuilt(operand) : TypeID();
                if (!type)
                    continue;
                Representation wanted = representationWantedByKind(m_types, draft.operandKind(operandIndex), type);
                if (!hasRegister(wanted) || wanted == m_code.at(operand.id()).representation)
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
    }
    WantedRepresentations formsWanted = wantedRepresentationsOf(*this, draft);

    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (!hasRegister(formsWanted[operandIndex]) || formsWanted[operandIndex] == representationOf(draft.operand(operandIndex)))
            continue;
        Reference converted = convert(draft.operand(operandIndex), formsWanted[operandIndex], draft.origin());
        if (!converted)
            return;
        draft.setOperand(operandIndex, converted);
    }
}

// The answer of a test is a boolean in every run, but its type says less once it has come round a loop whose preview gave up.
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

static void logMisfitInWalk(const TypeArena& types, ASCIILiteral takenBy, const IllTypedOperand& misfit)
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

// A parameter of a block is another name of what an edge hands it, and no store through another name reaches what is known of a fresh object.
void Walker::letLeave(WalkState& state, Reference reference) const
{
    if (has(Mechanism::EscapeAnalysis) && reference.isValue())
        state.markEscaped(referenceTo(rootOfCellInSweptCode(m_code, reference.id())));
}

// ---- Building

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
    copyFields(value, draft, [&](ValueID operand) {
        Reference reference = referenceOf(operand);
        lacksOperand |= !reference;
        return reference;
    });

    // A stack trace leaves out the frame that makes such a call once the callee is entered, whether or not the tail call stays one. The record cannot be marked later.
    // A function that is constructed waits for what its last call returns, so the copy of one is listed.
    if (value.id().index() + 1 == frame.endOfValues) {
        BodyTerminator end = frame.index->terminator(frame.block);
        if (end.kind() == TerminatorKind::ReturnCall && end.operand(0) == value.id()) {
            draft.isTailCall = !frame.inlineFrame || !m_code.inlineFrames()[frame.inlineFrame].isConstruction;
            draft.resultIsReturnedAtOnce = true;
        }
    }

    // No value is live for the sake of a hint.
    if (lacksOperand && value.opcode() != Opcode::ReleaseHint && !isUnreachable())
        fail("an operation takes a value of the body that nothing was built for"_s);
    return draft;
}

// A TEST FOR A CLOSURE MENTIONS THE FUNCTION, and a mention needs a road in LinkTables::referrers: it may bring a unit, and the queue ends the build where nobody has waited for it.
// Link lays a road for what it saw arrive at the positions of a body. What a guess expects, and what either reading says of a field of a layout that is only EXPECTED at a read, may name a function that the flow never saw arrive there.
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

Reference Walker::build(Draft&& draft, const Effects* foundByCaller)
{
    if (isUnreachable())
        return { };
    // The test is built without such a function: its closures fail the test. No rule derives more from a test than the test asks, on either edge, so whoever has built it claims no more than that.
    if (draft.opcode == Opcode::TypeTest)
        draft.setTestedType(withoutFunctionsThatMayNotBeMentioned(draft.testedType()));
    if (opcodesThatStateTypes.contains(draft.opcode) || opcodesWithoutEmitter().contains(draft.opcode)) {
        fail("an operation that no emitter stands behind is built"_s);
        return { };
    }
    bool takesBoolean = false;
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
        // No proof is a value in Code.
        OperandKind kind = draft.operandKind(operandIndex);
        if (kind != OperandKind::StringBuilder && hasTextsInStringBuilders() && isTextInStringBuilder(*this, operand)) {
            fail("an operation that is none of a builder's takes a text in its builder"_s);
            return { };
        }
        if (kind == OperandKind::Guard || kind == OperandKind::Fact)
            draft.setOperand(operandIndex, Reference());
        takesBoolean |= kind == OperandKind::Boolean;
    }
    takeTextsUsedAsKeysAsAtoms(*this, draft);
    if (takesBoolean && !testsItsOwnGuess(draft)) {
        askTruthOfOperandsNotProvenBoolean(draft);
        if (isUnreachable())
            return { };
    }

    if (m_positionsWithoutProofs) {
        checkProofsOfWhatIsStoredBy(draft);
        if (isUnreachable())
            return { };
    }

    bool listsOwnTypes;
    TypeID proven = resultTypeOf(draft, listsOwnTypes);
    if (!proven && resultRuleInWalk(draft.opcode) != ResultRule::None && !opcodesThatMayHaveSeveralResults.contains(draft.opcode)) {
        fail("the result rule of an operation gives no type"_s);
        return { };
    }

    // In every build: an operation that takes one kind of value has no form for any other.
    if (!listsOwnTypes)
        typesOfOperands(draft);
    if (draft.opcode == Opcode::ResultAt)
        m_operandTypes[OperandIndex::ResultAt::results] = proven;
    if (std::optional<IllTypedOperand> misfit = findIllTypedOperand(typing(), draft, OperandTypes { m_operandTypes.span() })) {
        logMisfitInWalk(m_types, nameOf(draft.opcode), *misfit);
        fail("an operand is not of the kind that its operation takes"_s);
        return { };
    }

    Effects effects = Effects::none();
    // Nothing below changes them where the row declares none.
    bool hasNoEffects = opcodesWithPureDeclaredBound().contains(draft.opcode);
    if (!hasNoEffects) {
        // No other rule of effects looks at the result.
        TypeID result = opcodesWithEffectsOfOperatorsInWalk.contains(draft.opcode) ? proven : TypeID();
        effects = Typed::effectsOf(typing(), draft, draft.opcode == Opcode::ResultAt ? typesOfOperands(draft) : OperandTypes { m_operandTypes.span() }, result, foundByCaller);
    }
    // An operation whose allocation fails throws when it returns, so under a handler whatever may collect may throw.
    // Not what takes the thrown value: the run time throws nothing there, and what the edge brings would be built in front of it, at the head of its handler block.
    if (effects.mayCollect && !m_isLeadingHandlerEdge && !catchOpcodes.contains(draft.opcode) && isUnderHandler())
        effects.mayThrow |= ThrowEffect::onlyForLackOfMemory();

    if (!m_isWalkingAccess && !m_openLoops.isEmpty() && !looksAtLoopsAroundHaveCounted(draft, effects)) {
        // The looks have counted on a guess that the walk has not opened. The closed copy does what is written.
        if (mayBeginTurnAgainInClosedCopy()) {
            beginTurnAgainInClosedCopy();
            return { };
        }
        if (walkUnitAgainWithLoopsThatMayDoAnything())
            return { };
        dataLogLn("AOT: a ", nameOf(draft.opcode), " is built in a loop of f", currentFunction().index(), ", where a ", nameOf(m_opcodeBeingWalked), " is written");
        fail("what is built in a loop does what the looks at the loop have not counted on"_s);
        return { };
    }
    if (m_isWalkingAccess && !m_openLoops.isEmpty() && !looksAtLoopsAroundHaveSeen(effects)) {
        if (mayBeginTurnAgainInClosedCopy()) {
            beginTurnAgainInClosedCopy();
            return { };
        }
        if (walkUnitAgainWithLoopsThatMayDoAnything())
            return { };
        dataLogLn("AOT: ", nameOf(draft.opcode), " stands for an access in a loop");
        fail("an access in a loop does what the look at the loop has not counted on"_s);
        return { };
    }
    // What gives the same in every turn stands in front of the loop, with the conversions of its operands.
    if (!m_openLoops.isEmpty() && proven && !isNeverType(proven)) {
        if (std::optional<uint32_t> loop = findLoopToBuildInFrontOf(draft, effects)) {
            SetForScope blockInFront(m_block, m_openLoops[*loop].blockInFront);
            SetForScope loopsThatMayBeLeft(m_numberOfLoopsThatMayBeLeft, *loop);
            return build(WTF::move(draft), foundByCaller);
        }
    }

    // What is built in front of a loop takes no record of the loop's inside, and the state is that of the inside.
    if (Reference equal = findEqualOperationBuilt(*this, draft, effects); equal && (m_numberOfLoopsThatMayBeLeft >= m_openLoops.size() || standsInFrontOfLoop(m_numberOfLoopsThatMayBeLeft, equal)))
        return equal;

    // The state goes by names of values, and a conversion is no name of what it converts.
    if (has(Mechanism::EscapeAnalysis) && m_state.holdsFreshObjects())
        markEscapingOperands(*this, draft, foundByCaller);
    if (m_environmentCellsOfUnit.mayNameSome && mayKeepEnvironment(draft.opcode))
        markEnvironmentsThatOperationKeeps(*this, draft);

    convertOperands(draft);
    if (isUnreachable())
        return { };
    if (effects.mayThrow && !m_isLeadingHandlerEdge) {
        leadHandlerEdge();
        if (isUnreachable())
            return { };
    }

    if (draft.representation == Representation::Unchosen) {
        if (!proven)
            draft.representation = Representation::Void;
        else if (draft.opcode == Opcode::Narrowed)
            draft.representation = representationOf(draft.valueOperand());
        else if (Representation form = formOfResultOfEvery(draft.opcode); form != Representation::Unchosen)
            draft.representation = form;
        else if (opcodesWhoseResultsHaveFormsOfTheirTypes.contains(draft.opcode))
            draft.representation = formOfTypeInWalk(RepresentationContext { program(), plan(), m_types, unit() }, proven);
        else
            draft.representation = resultRepresentationOf(*this, draft, proven);
    }
    bool holdsValue = proven && !isNeverType(proven) && hasRegister(draft.representation) && draft.representation != Representation::RawWord;
    if (draft.representation == Representation::Unchosen || (holdsValue && !isLegalRepresentation(m_types, proven, draft.representation))) {
        fail("the result of an operation is kept in a form that its type does not allow"_s);
        return { };
    }

    if (!hasNoEffects)
        endWhatOperationWrites(*this, draft, effects);
    if (effects.mayThrow && !m_isLeadingHandlerEdge && has(Mechanism::Handlers))
        noteStateAtThrowForHandler();
    bool standsInFrontOfLoop = m_numberOfLoopsThatMayBeLeft < m_openLoops.size();
    if (ValueID same = standsInFrontOfLoop ? findSameInFrontOfLoop(m_numberOfLoopsThatMayBeLeft, draft) : ValueID())
        return Reference::value(same);
    ValueID id = m_code.append(m_block, draft, proven, EffectTraits(effects));
    noteOperationBuilt(*this, draft, effects, Reference::value(id));
    if (m_resultOfReadBeingTyped && readsTypedByWhatIsProvenOfInput.contains(draft.opcode) && m_positions == m_positionsOfBuild.get())
        m_code.addFactUse(FactUse { .record = id, .kind = FactKind::ProvenContents, .subjectKind = FactSubject::Kind::Operation, .type = m_resultOfReadBeingTyped });
    if (m_contentsBeingTyped && recordsTypedByWhatIsProvenOfContents.contains(draft.opcode) && m_positions == m_positionsOfBuild.get())
        m_code.addFactUse(FactUse { .record = id, .kind = FactKind::ProvenContents, .subjectKind = FactSubject::Kind::Operation, .type = m_contentsBeingTyped });
    if (standsInFrontOfLoop)
        noteBuiltInFrontOfLoop(m_numberOfLoopsThatMayBeLeft, id);
    if (draft.opcode == Opcode::CallDirect || draft.opcode == Opcode::AwaitCallBySavingFrame || draft.opcode == Opcode::Construct)
        noteDirectCallForStackTests(draft);
    // Under a thin word only what a call gives, and what a read of a field gives, is asked; a Narrowed is the proof of a test.
    bool checksRecord = checksProofs() || (draft.opcode != Opcode::Narrowed && (unit().role == CodeRole::Body || unit().role == CodeRole::CoroutineBody) && checksProofsOf(sourceOfProofInWalk(draft.opcode)));
    if (checksRecord && holdsValue)
        checkProofOfRecord(id, draft, proven);

    if (!hasNoEffects)
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

    // The state knows an element by the Reference of its index, and a join needs no parameter for what every edge brings under one Reference.
    // The type and the form follow from the words of the draft alone, so whoever asks again with the same words is answered without either.
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
        // The rules leave the form of such a constant to whoever builds it. The record gets the form of its type.
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

    // Only a form that was left open can turn out to be that of a record that is there.
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
    // Where proofs are checked the read is built, so that what it finds can be held against the proof.
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
    // BindingEmitter::generateGetModuleVariable(): closures of the function that the variable is born as and nothing else, no undefined and no null: no store has put anything else there, and the word of the variable is not asked.
    // `function current() { .. }   const before = current; current = other; before()`: a variable that is stored into is read where the read is written.
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
}

bool Walker::copy(const BodyValue& value)
{
    Draft draft = draftOf(value);
    if (isUnreachable())
        return true;
    if (draft.opcode == Opcode::ReleaseHint && (!draft.valueOperand() || isWithoutNameBehindLoopBuiltTwice(value.operand(0))))
        return true;

    chooseConservativeFields(draft);
    if (draft.opcode == Opcode::Call && has(Mechanism::BuiltinCalls))
        nameMethodOfReceiverAsMade(*this, value, draft);
    if (draft.opcode == Opcode::StrictEq && has(Mechanism::Guesses) && m_opensGuesses)
        sayWhatIsExpectedOfComparedOperands(*this, value, draft);
    bool givesResult = resultRuleOf(draft.opcode) != ResultRule::None;
    if (draft.opcode == Opcode::Builtin && has(Mechanism::Folding)) {
        if (Reference cell = cellThatRunTimeKeeps(draft)) {
            bind(value.id(), cell);
            return true;
        }
    }
    Reference built = constantsInFormOfUses.contains(draft.opcode) ? constant(WTF::move(draft)) : build(WTF::move(draft));
    if (built && givesResult)
        bind(value.id(), built);
    return true;
}

Reference Walker::addIterationByIndex(IterationByIndex&& iteration)
{
    iteration.inlineFrame = m_frames.last().inlineFrame;
    m_numberOfIterationsInFrontOfTheirLoops += !!iteration.inputHead;
    if (iteration.inputHead)
        m_inputHeadsOfIterations.add(inputHeadInFrame(iteration.inlineFrame, iteration.inputHead));
    m_iterationsByIndex.append(WTF::move(iteration));
    return Reference::virtualObject(VirtualObjectID(m_iterationsByIndex.size() - 1));
}

void Walker::leaveArrivals(FunctionID callee, std::span<const Reference> arguments)
{
    for (uint32_t entryParameter = 0; entryParameter < arguments.size(); ++entryParameter) {
        TypeID proven = ownTypeOf(arguments[entryParameter]);
        m_arrivals.leave(m_types, callee, entryParameter, proven ? proven : TypeID(TypeTable::anything));
    }
    m_arrivals.leaveUndefinedFrom(m_types, callee, static_cast<uint32_t>(arguments.size()));
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = callee;
}

// ---- Blocks that stand for no block of a body

Walker::PendingBlock* Walker::findPendingBlock(BlockID block)
{
    for (size_t index = m_pendingBlocks.size(); index--;) {
        if (m_pendingBlocks[index].block == block)
            return &m_pendingBlocks[index];
    }
    return nullptr;
}

Walker::PendingBlock& Walker::pendingBlock(BlockID block)
{
    if (PendingBlock* found = findPendingBlock(block))
        return *found;
    m_pendingBlocks.constructAndAppend();
    m_pendingBlocks.last().block = block;
    // Whoever made it has given it its parameters.
    m_pendingBlocks.last().parametersAreSet = true;
    return m_pendingBlocks.last();
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
    m_code.setLoopMarks(block, !m_numberOfCopiesBuiltForSize && isInLoop(), false);
    m_pendingBlocks.constructAndAppend();
    m_pendingBlocks.last().block = block;
    m_pendingBlocks.last().isMadeByAddBlock = true;
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

void Walker::jumpTo(BlockID next, std::span<const Reference> arguments)
{
    settleParametersOf(next);
    if (!isUnreachable()) {
        const Block& target = m_code.at(next);
        std::span<const ValueID> parameters = target.parameters();
        if (parameters.size() != arguments.size()) {
            fail("a jump does not fill the parameters of its target"_s);
            return;
        }

        // A parameter of a join has the join of what arrives. One of a head holds whatever arrives.
        bool isFirstArrival = !pendingBlock(next).numberOfArrivals;
        bool typesFollowArrivals = !target.isLoopHead && !target.isEntry && !target.hasEnded() && !m_code.lastOperationOf(next);
        for (size_t index = 0; index < arguments.size(); ++index) {
            TypeID arriving = ownTypeOf(arguments[index]);
            TypeID held = m_code.at(parameters[index]).type;
            if (!arriving) {
                fail("a jump hands on what has no type"_s);
                return;
            }
            if (typesFollowArrivals)
                m_code.setTypeOfJoinParameter(parameters[index], isFirstArrival ? arriving : joinTypes(held, arriving));
            else if (!m_types.isSubset(arriving, held)) {
                fail("what a jump hands on lies outside the type of the parameter that takes it"_s);
                return;
            }
            letLeave(m_state, arguments[index]);
        }
        endWithJumpInto(next, arguments);
        if (m_hasFailed)
            return;
        arriveAt(next, WTF::move(m_state));
    }

    PendingBlock& pending = pendingBlock(next);
    if (!pending.numberOfArrivals) {
        m_block = { };
        m_state.clear();
        return;
    }
    m_block = next;
    if (std::exchange(pending.hasState, false))
        m_state = WTF::move(pending.state);
    else
        m_state.clear();
}

void Walker::branch(Reference condition, BlockID taken, BlockID notTaken)
{
    if (isUnreachable())
        return;
    TerminatorDraft terminator(TerminatorKind::Branch, m_origin);
    terminator.setConditionOperand(condition);
    if (!operandsAreAsTaken(terminator)) {
        askTruthOfOperandsNotProvenBoolean(terminator);
        if (isUnreachable() || !checkOperandKinds(terminator))
            return;
        convertOperands(terminator);
        if (isUnreachable())
            return;
    }

    BlockID from = m_block;
    m_code.end(from, terminator, 2);
    EdgeID edgeTaken = m_code.at(from).terminator.taken();
    EdgeID edgeNotTaken = m_code.at(from).terminator.notTaken();
    settleParametersOf(taken);
    settleParametersOf(notTaken);
    m_code.closeEdge(edgeTaken, taken, { });
    m_code.closeEdge(edgeNotTaken, notTaken, { });
    arriveAt(taken, WalkState(m_state));
    arriveAt(notTaken, WTF::move(m_state));
    m_block = { };
    m_state.clear();
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

    // By the uses of the row each operand is retained, returned or called.
    for (Reference operand : terminator.operands()) {
        if (operand)
            letLeave(m_state, operand);
    }
    convertOperands(terminator);
    if (isUnreachable())
        return { };
    if (effects.mayThrow) {
        leadHandlerEdge();
        if (isUnreachable())
            return { };
    }
    m_state.invalidate(effects);
    if (effects.mayThrow && has(Mechanism::Handlers))
        noteStateAtThrowForHandler(terminator.operands());

    // What a terminator hands over comes in the form of its type, as the result of an operation comes in the form that its row gives.
    Reference result = addParameter(done, given, formOfTypeInWalk(RepresentationContext { program(), plan(), m_types, unit() }, given));
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
    if (m_block == block)
        return;
    // Whoever comes back to a block that he has left without an end finds what held there.
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
    settleParametersOf(block);
    if (pending->isMadeByAddBlock && !pending->numberOfArrivals && !pending->hasState && !m_code.at(block).isEntry) {
        m_block = { };
        return;
    }
    m_block = block;
    if (std::exchange(pending->hasState, false))
        m_state = WTF::move(pending->state);
}

// ---- Edges

Walker::InputBlock& Walker::inputBlock(size_t frameIndex, BlockID block)
{
    Frame& frame = m_frames[frameIndex];
    if (frame.blocks.isEmpty())
        frame.blocks.fill(InputBlock { }, frame.index->numberOfBlocks());
    return frame.blocks[block.index()];
}

uint32_t Walker::addOpenEdge(size_t frameIndex, BlockID target)
{
    InputBlock& record = inputBlock(frameIndex, target);
    if (record.isEntered) {
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
    m_openEdges[place].next = std::exchange(record.firstOpenEdge, place);
    return place;
}

// In the order in which they were led.
Vector<uint32_t, 4> Walker::takeOpenEdges(BlockID block)
{
    InputBlock& record = inputBlock(m_frames.size() - 1, block);
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
    edge.names.shrink(0);
    edge.isLedByClosedArm = false;
    edge.firstNarrowerName = noNarrowerName;
    edge.next = std::exchange(m_firstFreeOpenEdge, place);
}

BlockID Walker::addBlockFor(BlockID inputBlock, BlockRole role, BlockFrequency frequency)
{
    const Frame& frame = m_frames.last();
    BlockID block = m_code.addBlock(role, m_numberOfCopiesBuiltForSize ? BlockFrequency::Rare : frequency);
    m_code.setLoopMarks(block, !m_numberOfCopiesBuiltForSize && (frame.loopDepthAtCall || frame.index->loopDepthOf(inputBlock)), false);
    return block;
}

// The parameters of `to` are set. The walk stands nowhere behind it.
void Walker::endWithJumpInto(BlockID to, std::span<const Reference> arguments)
{
    std::span<const ValueID> parameters = m_code.at(to).parameters();
    RELEASE_ASSERT(parameters.size() == arguments.size());
    Vector<ValueID, 8> converted;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (hasTextsInStringBuilders() && isTextInStringBuilder(*this, arguments[index]) && !isTextInStringBuilder(*this, Reference::value(parameters[index]))) {
            fail("an edge hands a text in its builder to a parameter that holds no builder"_s);
            return;
        }
        Representation form = m_code.at(parameters[index]).representation;
        Reference argument = isSameWord(representationOf(arguments[index]), form) ? arguments[index] : convert(arguments[index], form, m_origin);
        if (!argument || isUnreachable()) {
            fail("an edge hands on a value that nothing was built for"_s);
            return;
        }
        converted.append(argument.id());
    }
    BlockID from = m_block;
    m_draftOfJump.setOrigin(m_origin);
    m_code.end(from, m_draftOfJump, 1);
    m_code.closeEdge(m_code.at(from).terminator.next(), to, converted.span());
    m_block = { };
}

// The block that the edge leaves has ended, so what takes code is built in a block on the edge. m_state is what holds on the edge.
// An edge hands over a word, so it takes a cell for its boxed form and the other way round.
void Walker::closeEdgeWithArguments(EdgeID edge, BlockID to, std::span<const Reference> arguments)
{
    std::span<const ValueID> parameters = m_code.at(to).parameters();
    RELEASE_ASSERT(parameters.size() == arguments.size());
    Vector<ValueID, 8> ready;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (hasTextsInStringBuilders() && isTextInStringBuilder(*this, arguments[index]) && !isTextInStringBuilder(*this, Reference::value(parameters[index]))) {
            fail("an edge hands a text in its builder to a parameter that holds no builder"_s);
            return;
        }
        Representation form = m_code.at(parameters[index]).representation;
        Reference argument = isSameWord(representationOf(arguments[index]), form) ? arguments[index] : convertWithoutCode(arguments[index], form);
        if (!argument)
            break;
        ready.append(argument.id());
    }
    if (ready.size() == arguments.size()) {
        m_code.closeEdge(edge, to, ready.span());
        return;
    }

    if (m_code.at(m_code.at(edge).from).handler == edge) {
        fail("what a handler edge brings is not in the form of the handler's parameter"_s);
        return;
    }
    BlockID onEdge = m_code.addBlock(BlockRole::Ordinary, m_code.at(to).frequency);
    m_code.setLoopMarks(onEdge, m_code.at(to).isInLoop, false);
    m_code.closeEdge(edge, onEdge, { });
    m_block = onEdge;
    endWithJumpInto(to, arguments);
}

// `edge` is null where the walk stands in the block that the edge is to leave.
void Walker::closeBackEdge(EdgeID edge, BlockID head, std::span<const Reference> arguments)
{
    noteEdgeCameRound(head);
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
        arguments = withPositions.span();
    }
    if (parameters.size() != arguments.size()) {
        fail("an edge that comes round does not fill the parameters of the head"_s);
        return;
    }
    for (size_t index = 0; index < arguments.size(); ++index) {
        TypeID comingRound = ownTypeOf(arguments[index]);
        TypeID previewed = m_code.at(parameters[index]).type;
        if (!comingRound || !m_types.isSubset(comingRound, previewed)) {
            if (comingRound && walkUnitAgainWithWiderHeadOfCode(head, index, previewed, comingRound))
                return;
            if (comingRound)
                dataLogLn("AOT: parameter ", index, " of the head b", head.index(), ": ", TypeInText { m_types, comingRound }, " comes round in v", arguments[index].id().index(), ", a ", nameOf(m_code.at(arguments[index].id()).opcode), ", and ", TypeInText { m_types, previewed }, " was previewed");
            fail("what comes round to the head of a loop lies outside the previewed type of its parameter"_s);
            return;
        }
    }
    if (edge)
        closeEdgeWithArguments(edge, head, arguments);
    else
        endWithJumpInto(head, arguments);
    m_block = { };
}

// Gives `block` a parameter for each parameter of the input block that the edges bring different values for, closes the edges and meets their states. The walk goes on in `block`.
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

    if (hasTextsInStringBuilders() && !isHandler) {
        appendPartsOfTextsOnEdgesInto(inputBlock, block, edges);
        if (m_hasFailed)
            return;
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
    // What a loop built twice has defined arrives as an argument does.
    Vector<uint32_t, 4> joinedNames;
    for (uint32_t index = 0; index < m_openEdges[edges[0]].names.size(); ++index) {
        auto [value, first] = m_openEdges[edges[0]].names[index];
        TypeID type = ownTypeOf(first);
        bool allBringOneName = true;
        forms.shrink(0);
        for (uint32_t place : edges) {
            if (m_openEdges[place].names.size() != m_openEdges[edges[0]].names.size() || m_openEdges[place].names[index].first != value) {
                fail("the edges that enter a block do not bring the same values of loops that were built twice"_s);
                return;
            }
            Reference name = m_openEdges[place].names[index].second;
            allBringOneName &= name == first;
            type = joinTypes(type, ownTypeOf(name));
            forms.append(representationOf(name));
        }
        if (allBringOneName) {
            bind(value, first);
            continue;
        }
        parameters.append(m_code.appendParameter(block, type, formOfParameter(type, forms.span()), m_origin));
        joinedNames.append(index);
        bind(value, Reference::value(parameters.last()));
    }
    if (!parameters.isEmpty())
        m_code.setParameters(block, parameters.span());

    WalkState met;
    bool hasMet = false;
    Vector<Reference, 8> arguments;
    for (uint32_t place : edges) {
        // What is built on the edge may lead an edge of its own, which moves the list.
        EdgeID edge = m_openEdges[place].edge;
        m_state = WTF::move(m_openEdges[place].state);
        arguments.shrink(0);
        for (uint32_t index : joined) {
            arguments.append(m_openEdges[place].arguments[index]);
            letLeave(m_state, arguments.last());
        }
        for (uint32_t index : joinedNames) {
            arguments.append(m_openEdges[place].names[index].second);
            letLeave(m_state, arguments.last());
        }
        releaseOpenEdge(place);

        closeEdgeWithArguments(edge, block, arguments.span());
        if (m_hasFailed)
            return;
        if (std::exchange(hasMet, true))
            met.meetWith(m_state);
        else
            met = WTF::move(m_state);
    }
    m_state = WTF::move(met);
    m_block = block;
}

std::optional<Draft> Walker::draftOfNarrowedOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult& result, Reference holderOfSubject)
{
    targetIndex = m_edgeOfCodeBeingLed.value_or(targetIndex);
    Reference subject = referenceOf(result.subject);
    if (!has(Mechanism::TypedOperations) || !subject || !subject.isValue())
        return std::nullopt;
    Reference named = holderOfSubject ? holderOfSubject : subject;
    // All that is known of a fresh object is known under one name, and its type says what was made.
    if (!named.isValue() || m_state.isFresh(referenceTo(rootOfCellInSweptCode(m_code, named.id()))))
        return std::nullopt;

    const Terminator& end = m_code.at(from).terminator;
    ValueID root = valueBehindNamesInWalk(m_code, subject.id());
    Draft narrowed(Opcode::Narrowed, m_origin);
    narrowed.setValueOperand(named);
    narrowed.setNarrowsHolderOfOperand(!!holderOfSubject);

    if (result.test && !end.hasCases()) {
        Reference test = referenceOf(result.test);
        if (!test || !test.isValue() || !end.isConditionalBranch())
            return std::nullopt;
        bool testIsTrue = !targetIndex;
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
        narrowed.setTestOperand(test);
        narrowed.setOperandIndex(result.operandIndex);
        narrowed.setIsOnTakenEdge(testIsTrue);
    } else {
        if (!end.hasCases() || !root || valueBehindNamesInWalk(m_code, end.subjectOperand()) != root)
            return std::nullopt;
        narrowed.setOperandIndex(0);
        narrowed.setCaseIndex(targetIndex < end.numberOfCases() ? std::optional<uint32_t>(targetIndex) : std::nullopt);
    }

    TypeID type = resultTypeOf(narrowed);
    if (!type || type == m_code.at(named.id()).type)
        return std::nullopt;
    return narrowed;
}

// One value in every run that `===` tells by itself: a text, an integer, a boolean, undefined, null. Nothing: the type holds more, or what no such number names.
static std::optional<uint64_t> numberOfTagInWalk(const TypeArena& types, TypeID id)
{
    Type type = types.at(id);
    if (type.hasObjects() || !isSingleValue(types, type))
        return std::nullopt;
    switch (type.primitives) {
    case Primitive::LiteralString:
        return uint64_t(1) << 62 | singleStringLiteralOf(type).bits();
    // The integers that types track take 63 bits, and the kind stands in the two at the top: 512 and 512 - 2 ** 62 had one number, and `case -4611686018427387392:` took the objects whose tag is 512.
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

// THE MEMBERS OF A UNION BY THE ONE VALUE THAT EACH HOLDS UNDER A NAME, listed once for the union and the name: memberCanHoldFieldValue() asks a member about 2,000 instructions' worth, and a row of tests, or the cases of a switch,
// would ask every member each time: members x tests. What a test of the tag leaves of the union goes by the list of the union: shareMembersByTag().
// Null: some member may hold more than one value there, has the field from a layout above, or has an accessor under the name.
const Walker::MembersByTag* Walker::findMembersByTag(LayoutSetID layouts, const Operation& read)
{
    // A slot that may be empty gives undefined whatever the layout, and a read that tests its own guess may have read by name.
    if (read.opcode != Opcode::GetSlot || testsItsOwnGuess(read))
        return nullptr;
    PropertyKeyID key = read.key();
    auto known = m_membersByTagOfSets.add(static_cast<uint64_t>(layouts.bits()) << 32 | key.bits(), 0);
    if (!known.isNewEntry)
        return known.iterator->value ? m_listsOfMembersByTag[known.iterator->value - 1].get() : nullptr;

    TypingContext context = typing();
    auto list = makeUnique<MembersByTag>();
    // Asking about a member may make types and sets, so no set is read meanwhile.
    Vector<LayoutID, 16> members;
    members.append(membersOf(m_types, layouts));
    for (LayoutID member : members) {
        std::optional<FieldReference> reference = findField(program(), member, key);
        if (!reference || reference->layout != member || fieldAt(program(), *reference).form.contains(FieldForm::IsAccessor))
            return nullptr;
        TypeID arriving = context.positions.readOfSlot(context.level, reference->layout, reference->field);
        // `{ kind: "none" }` is written of whatever has the name, the layouts of libraries too. Nothing arrives at the field of a layout that nobody makes, and the read finds its slot filled: the holder is no such object.
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

// Those of `layouts` that hold the value, ascending. The list may be that of a larger union.
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

// `e.kind === "num"`: each layout that the holder may have is proven to hold one value under the name. So which layout it has answers: nothing is read, nothing compared.
// With proofs checked at run time the comparison stays as it is written, and narrowHolderOfTestedFieldOnEdge() holds the layout against its answer.
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
        // All of them or none: the types decide that, and folding has been asked.
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

// `switch (e.kind) { case "num": .. }`, and a row of `if (e.kind === "num") .. else if (e.kind === "add") ..` that ends its first block in one switch: the same for all the cases at once: a switch over the shape.
// `overTags`: the switch over what was read. `casesLeft`: the places of its cases that some member holds, in their order: no run takes another.
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
        // Every member is listed, so every tag is an int32: numberOfTagInWalk().
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

// The comparison that the Branch of its own block asks waits for the end of the block: a row of such comparisons is one switch (endBlockWithSwitchOverTestsOfField()), which asks the shape once for all of them.
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
    // A constant: folding has decided.
    Reference built = referenceOf(condition);
    if (!built || !built.isValue())
        return;
    if (Reference test = buildTestOfTagAsTestOfShape(index.value(condition)))
        bind(condition, test);
}

// `o.kind = src.v; if (o.kind === "x") ..`: the state answers the read with what was stored, and that is a read of another name off another object. What the test says of it says nothing of `o`:
// a Narrowed that narrows the holder is typed by the key and the object of the read that stands behind its test.
bool Walker::isReadOfSlotOffHolder(Reference read, Reference holder, PropertyKeyID key) const
{
    ValueID slotRead = read && read.isValue() && holder.isValue() ? slotReadBehindNamesInWalk(m_code, read.id()) : ValueID();
    return slotRead && m_code.at(slotRead).key() == key && valueBehindNamesInWalk(m_code, m_code.at(slotRead).objectOperand()) == valueBehindNamesInWalk(m_code, holder.id());
}

// The cell that a test of the shape, or a switch over it, was asked of is the value under another name. Whoever goes down the names of both finds it: behind a row of tests of one value that is tests x tests.
// A Narrowed is built only over what its test has looked at. So a name that a test of the shape OF THE SAME CELL has left is a name of that cell, and each test of a row finds the name that the test in front of it left.
// What the body says is not enough: `o.kind = from.kind; if (o.kind === "circle")`: the state answers the read with a read off `from`, and the shape that is asked is that of `from`.
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
    // All that is known of a fresh object is known under one name, and its type says what was made.
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

// `if (e.kind === "num") return e.value;`: the front end names the holder anew behind a test of what was read out of it: NarrowByField, at the head of the block. Where the test of the tag was built as a test of the shape,
// or the switch over it as a switch over the shape, the holder has the layouts that the edge leaves, in all that the block dominates. False: nothing is left of the holder, so no run takes the edge.
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
            // The comparison stands as it is written. What it says of the holder by Link's word is held against the holder that runs.
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
        // The name at the head of the block is written over the name that the holder had in front of the test, and whatever the block dominates reads the holder under it. noteNarrowerName() lays a name over every older
        // one too: a row of tests would lay tests x tests.
        size_t frameIndex = m_frames.size() - 1;
        uint32_t& firstName = inputBlock(frameIndex, to).firstNarrowerName;
        if (firstName == narrowerNamesOfDominator)
            firstName = m_frames[frameIndex].firstNarrowerName;
        m_frames[frameIndex].addNarrowerName(firstName, holder, narrowed);
        // Whoever asks goes from what the value is bound to, in one step.
        if (Reference bound = referenceBoundTo(written.valueOperand()); bound != holder)
            m_frames[frameIndex].addNarrowerName(firstName, bound, narrowed);
        return !isUnreachable();
    }
    return true;
}

// `if (e.kind === "num") return ..; log(); if (e.kind === "add") ..`: the second read is off the name that the holder has behind the first test, and that name says what the test was asked of. A tag is written at birth
// alone, so whatever has run since, it is what it was: nothing is read again. (A read off what each test has left of a large union would go through all that is left: tests x members.)
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
    // `o.kind = from.kind; if (o.kind === "circle") ..; meddle(); if (o.kind === "square") ..`: what the first test was asked of was read off `from`, and `o.kind` may be another by now.
    if (!slotRead || m_code.at(slotRead).key() != written.key() || !isNameOfCellAskedForShape(referenceOf(object), m_code.at(slotRead).objectOperand()))
        return { };
    TypeID typeOfHolder = m_code.at(m_code.at(slotRead).objectOperand()).type;
    if (!typeOfHolder || !m_types.at(typeOfHolder).hasOnlyLayouts() || !findMembersByTag(m_types.at(typeOfHolder).layouts, m_code.at(slotRead)))
        return { };
    return earlier;
}

// What stands below the negations and the questions for truth. `isNegated` changes with each negation that is passed.
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

// `a === b || c`: nobody but the Branch reads the comparison then, so it leaves its answer in no register.
Reference Walker::booleanAskedByBranchOnEdge(BlockID from, uint32_t targetIndex, Reference value)
{
    if (!has(Mechanism::Folding) || !value || !value.isValue())
        return { };
    const Terminator& end = m_code.at(from).terminator;
    TypeID type = typeOf(value);
    if (end.kind != TerminatorKind::Branch || !type || !isBooleanType(type))
        return { };

    bool isNegated = false;
    ValueID asked = valueWhoseTruthIsToldInWalk(m_code, end.conditionOperand(), isNegated);
    if (!asked || asked != valueWhoseTruthIsToldInWalk(m_code, value.id(), isNegated))
        return { };
    bool conditionIsTrue = !m_edgeOfCodeBeingLed.value_or(targetIndex);
    Draft answer(Opcode::Boolean);
    answer.setBoolean(conditionIsTrue != isNegated);
    return constant(WTF::move(answer));
}

// `const same = a === b; if (same) return same;`. A Boolean that the test alone reads needs no name.
void Walker::nameBooleansAskedByBranch(BlockID from, const BodyTerminator& terminator, uint32_t targetIndex, BlockID to)
{
    if (!has(Mechanism::Folding) || terminator.kind() != TerminatorKind::Branch)
        return;
    const BodyIndex& index = *m_frames.last().index;
    for (ValueID asked = terminator.operand(0);;) {
        if (!index.hasOneUse(asked)) {
            Reference value = referenceOf(asked);
            if (Reference answer = booleanAskedByBranchOnEdge(from, targetIndex, value))
                noteNarrowerName(to, value, answer);
        }
        BodyValue written = index.value(asked);
        Opcode opcode = written.opcode();
        if (opcode != Opcode::ToBoolean && opcode != Opcode::LogicalNot && !inputOperationsThatOnlyHandOn.contains(opcode))
            return;
        asked = written.operand(0);
    }
}

// The walk stands in a new block that the edge alone enters. What the test or the case leaves of the value, or the value as it is.
Reference Walker::narrowOnEdge(BlockID from, uint32_t targetIndex, const BodyEdgeResult& result)
{
    // Nothing reads the parameter that takes it.
    if (!checksProofs() && m_frames.last().index->isUnused(result.parameter))
        return referenceOf(result.subject);
    if (Reference answer = booleanAskedByBranchOnEdge(from, targetIndex, referenceOf(result.subject)))
        return answer;
    std::optional<Draft> narrowed = draftOfNarrowedOnEdge(from, targetIndex, result);
    return narrowed ? build(WTF::move(*narrowed)) : referenceOf(result.subject);
}

// `to` is entered by one edge, which leaves the block being walked. `value` is a name as referenceOf() gives it there, so whatever has it for its narrowest name gets the new one too.
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

// Both name one handler block and hand it the same values, or neither names any: then a block of a callee stands under the handler of the call, as the other does.
static bool standUnderSameHandler(const BodyIndex& index, BlockID one, BlockID other)
{
    BodyBlock first = index.block(one);
    BodyBlock second = index.block(other);
    if (first.hasHandler() != second.hasHandler())
        return false;
    return !first.hasHandler() || (first.handler().block() == second.handler().block() && std::ranges::equal(first.handler().arguments(), second.handler().arguments()));
}

// The block `from` has ended, `edge` is open, and m_state is what holds on the edge.
void Walker::leadEdge(EdgeID edge, BlockID from, const BodyTerminator& terminator, uint32_t targetIndex, BodyTarget target)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    if (const Terminator& end = m_code.at(from).terminator; end.kind != TerminatorKind::Jump)
        noteWhatBranchDecidesOnEdge(*this, from, edge.index() - end.firstEdge.index());
    BlockID to = target.block();
    BlockID head = inputBlock(frameIndex, to).head;
    uint32_t numberOfProofs = static_cast<uint32_t>(target.proofs().size());
    bool entersAlone = !head && index.numberOfIncomingEdges(to) == 1 && !index.isLoopHead(to) && !leavesLoopBeingBuiltTwice(frameIndex, to) && !isWalkingBlockForEachWay(frameIndex);

    Vector<Reference, 8> arguments;
    for (ValueID argument : target.arguments())
        arguments.append(referenceOf(argument));

    m_block = { };
    if (entersAlone || numberOfProofs) {
        m_block = addBlockFor(to);
        m_code.closeEdge(edge, m_block, { });
        edge = { };
        for (uint32_t proofIndex = 0; proofIndex < numberOfProofs; ++proofIndex) {
            std::optional<BodyEdgeResult> result = index.edgeResult(terminator, targetIndex, target, proofIndex);
            if (!result) {
                fail("an edge hands on a proof that its terminator does not give"_s);
                return;
            }
            arguments.append(narrowOnEdge(from, targetIndex, *result));
            // Nothing is left of the value, so no run takes the edge.
            if (isUnreachable() || (entersAlone && !narrowHolderOfTestedFieldOnEdge(from, targetIndex, *result, to))) {
                m_block = { };
                return;
            }
        }
    }
    // The narrower name is handed on in the place of the operand, and where the edge alone enters the block it stands for the operand in all that the block dominates.
    if (std::optional<std::array<BodyEdgeResult, 2>> bounds = terminator.isConditionalBranch() ? boundsProvenOnEdge(index, terminator, targetIndex) : std::nullopt) {
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
            // Nothing is left of the operand, so no run takes the edge.
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
    // `$isJSArray(x)`: a question for the cell's type that was built as the test of a class: replaceTestOfCellTypeByTestOfClass(). The body writes no proof for it. Behind the test the operand is of the class in all that the block dominates.
    ValueID cellTypeAsked = entersAlone && terminator.isConditionalBranch() ? terminator.operand(0) : ValueID();
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
            // Nothing is left of the operand, so no run takes the edge.
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
    if (entersAlone && terminator.isConditionalBranch())
        nameBooleansAskedByBranch(from, terminator, targetIndex, to);
    // A Narrowed stands at the head of its block, so the names come first.
    TextsTakenOutOnEdge textsTakenOut;
    bool endsBlockOnEdge = !entersAlone;
    if (hasTextsInStringBuilders() && edgeLeavesLoopOfTextInStringBuilder(*this, to)) {
        if (!m_block) {
            m_block = addBlockFor(to);
            m_code.closeEdge(edge, m_block, { });
            edge = { };
        }
        takeTextsOutOfStringBuildersOnEdge(*this, to, target, arguments, textsTakenOut);
        // `for (;;) { try { s += x; if (done) break; } catch { .. } } boom(s);`: the string is made on the edge, may throw, and does so under the handler of the block that is left. A block of Code has ONE handler for all that stands in it:
        // in the block of `to` itself that handler would take what `boom` throws. The other way round the handler of `to` would take what the making of the string throws. So where the two differ the edge has a block of its own.
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
        BlockID onEdge = m_block;
        m_draftOfJump.setOrigin(m_origin);
        m_code.end(onEdge, m_draftOfJump, 1);
        edge = m_code.at(onEdge).terminator.next();
        m_block = { };
    }

    uint32_t place = addOpenEdge(frameIndex, to);
    if (place == noOpenEdge)
        return;
    OpenEdge& open = m_openEdges[place];
    open.edge = edge;
    open.block = m_block;
    open.firstNarrowerName = m_firstNarrowerNameOnEdge;
    for (Reference argument : arguments) {
        open.arguments.append(argument);
        open.types.append(ownTypeOf(argument));
    }
    open.state = WTF::move(m_state);
    open.isLedByClosedArm = !m_opensGuesses;
    if (!m_frames[frameIndex].valuesBuiltTwice.isEmpty()) {
        takeNamesOntoEdge(open, frameIndex, to);
        bringStringsInPlaceOfTexts(textsTakenOut, open.names);
    }
    m_block = { };
}

bool Walker::isUnderHandler() const
{
    // A handler of a mechanism's own, which no block of a body names.
    if (m_block && m_code.at(m_block).handler)
        return true;
    return numberOfFramesUpToHandlerInForce();
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

// In front of the first operation of a block of Code that may throw. A block of a callee without a handler of its own stands under the handler of the call.
void Walker::leadHandlerEdge()
{
    if (m_code.at(m_block).handler)
        return;
    size_t frameIndex = numberOfFramesUpToHandlerInForce();
    if (!frameIndex--)
        return;

    SetForScope isLeading(m_isLeadingHandlerEdge, true);
    BodyTarget target = m_frames[frameIndex].index->block(m_frames[frameIndex].block).handler();
    Vector<Reference, 4> arguments;
    Vector<TypeID, 4> types;
    for (ValueID argument : target.arguments()) {
        Reference reference = m_frames[frameIndex].references[argument.index()];
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

    if (hasTextsInStringBuilders() && leadHandlerEdgeBehindStringsOfTexts(frameIndex, target, arguments, types))
        return;
    uint32_t place = addOpenEdge(frameIndex, target.block());
    if (place == noOpenEdge)
        return;
    OpenEdge& open = m_openEdges[place];
    open.edge = m_code.addHandlerEdge(m_block);
    open.arguments = WTF::move(arguments);
    open.types = WTF::move(types);
}

// ---- Entering a block of the body

void Walker::standIn(BlockID inputBlock)
{
    Frame& frame = m_frames.last();
    if (frame.block == inputBlock)
        return;
    BodyBlock block = frame.index->block(inputBlock);
    frame.block = inputBlock;
    frame.endOfValues = block.endOfValues().index();
    frame.hasHandler = block.hasHandler();
    frame.standsInFrontOfHead = false;

    uint32_t& first = this->inputBlock(m_frames.size() - 1, inputBlock).firstNarrowerName;
    if (first == narrowerNamesOfDominator) {
        BlockID dominator = frame.index->immediateDominator(inputBlock);
        first = dominator ? frame.blocks[dominator.index()].firstNarrowerName : noNarrowerName;
        // Nothing reaches a block whose dominator was not walked.
        if (first == narrowerNamesOfDominator)
            first = noNarrowerName;
    }
    frame.firstNarrowerName = first;
    frame.listOfNarrowerNamesIsLong = frame.lengthOfNarrowerNames(first) >= Frame::lengthOfLongListOfNarrowerNames;
}

void Walker::enterJoin(BlockID inputBlock)
{
    InputBlock& record = this->inputBlock(m_frames.size() - 1, inputBlock);
    uint32_t only = record.firstOpenEdge;
    if (m_openEdges[only].next != noOpenEdge || !m_openEdges[only].block) {
        Vector<uint32_t, 4> edges = takeOpenEdges(inputBlock);
        closeEdgesInto(addBlockFor(inputBlock), inputBlock, edges.span(), false);
        return;
    }
    record.isEntered = true;
    record.firstOpenEdge = noOpenEdge;

    BodyBlock input = currentBodyIndex().block(inputBlock);
    OpenEdge& edge = m_openEdges[only];
    if (edge.arguments.size() != input.numberOfParameters()) {
        fail("the arguments and proofs of an edge do not fill the parameters of its target"_s);
        return;
    }
    for (uint32_t index = 0; index < input.numberOfParameters(); ++index)
        bind(input.parameter(index), edge.arguments[index]);
    for (auto [value, name] : edge.names)
        bind(value, name);
    m_block = edge.block;
    m_state = WTF::move(edge.state);
    releaseOpenEdge(only);
}

void Walker::enterLoopHead(BlockID inputHead)
{
    size_t frameIndex = m_frames.size() - 1;
    BodyBlock input = currentBodyIndex().block(inputHead);
    uint32_t numberOfParameters = input.numberOfParameters();
    uint32_t depthAroundLoop = m_frames[frameIndex].loopDepthAtCall + currentBodyIndex().loopDepthOf(inputHead) - 1;
    m_frames[frameIndex].standsInFrontOfHead = true;

    // All that enters meets in front of the head, where whatever is hoisted stands.
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
        m_iterationsByIndex[m_frames[frameIndex].placeOfForInWhosePositionEdgesBring].position = referenceBoundTo(step);
    if (writeOutTurnsOfLoop(inputHead, entering.span()) || writeOutTurnsOfForIn(inputHead, entering.span()))
        return;
    // The parameter of the head is a second name of what enters, and the turns store through it. No effect ends what is known of a fresh object, so it leaves in front of what the turns do.
    for (Reference argument : entering)
        letLeave(m_state, argument);
    if (hasTextsInStringBuilders())
        typeTextsInStringBuildersForLooks(*this, entering.span(), entryTypes);
    Frame& frame = m_frames[frameIndex];
    // The looks at a loop do not stand around it: where it is built a second time, the copy has looks of its own.
    bool isInsideLoopLookedAt = frame.loopLookedAt && frame.loopLookedAt != currentBodyIndex().loopOf(inputHead) && currentBodyIndex().isInLoop(inputHead, frame.loopLookedAt);
    std::span<const TypeID> settledAround;
    if (isInsideLoopLookedAt)
        settledAround = frame.typesOfLooks.span().subspan(input.firstParameter().index() - frame.firstValueLookedAt, numberOfParameters);
    std::unique_ptr<LoopPreview> madeInFront = !m_numberOfCopiesBuiltForSize && looksCountOnTurnsThatBeginAgain(inputHead) ? std::exchange(m_previewOfTypedCopyAboutToBeEntered, nullptr) : nullptr;
    LoopPreview preview = madeInFront ? WTF::move(*madeInFront) : previewLoop(*this, inputHead, entryTypes.span(), settledAround);
    if (m_hasFailed)
        return;
    if (preview.typesOfParameters.size() != numberOfParameters) {
        fail("the preview of a loop types other parameters than the head has"_s);
        return;
    }
    if (has(Mechanism::LoopInvariants) && buildLoopTwice(inputHead, preview, entering.span()))
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
        // The frame is asked for what the looks have settled on for the heads of the loops inside, and for nothing else. These looks may have assumed more on their way than the looks around:
        // both hold of what the walk builds, and the frame keeps the narrower where one lies within the other.
        const BodyIndex& index = currentBodyIndex();
        LoopID loop = index.loopOf(inputHead);
        uint32_t numberOfLoopsLeft = preview.typesOfValuesLieInFrame || preview.typesOfValues.isEmpty() ? 0 : index.loop(loop).numberOfLoopsWithin - 1;
        for (uint32_t inner = loop.index() + 1; numberOfLoopsLeft && inner < index.numberOfLoops(); ++inner) {
            BodyBlock innerHead = index.block(index.loop(LoopID(inner)).head);
            if (!index.isInLoop(innerHead.id(), loop))
                continue;
            --numberOfLoopsLeft;
            for (uint32_t parameter = 0; parameter < innerHead.numberOfParameters(); ++parameter) {
                uint32_t value = innerHead.parameter(parameter).index();
                TypeID found = preview.typesOfValues[value - preview.firstValue];
                TypeID& around = frame.typesOfLooks[value - frame.firstValueLookedAt];
                if (found && (!around || m_types.isSubset(found, around)))
                    around = found;
            }
        }
    } else if (!preview.typesOfValues.isEmpty()) {
        frame.loopLookedAt = currentBodyIndex().loopOf(inputHead);
        frame.firstValueLookedAt = preview.firstValue;
        frame.typesOfLooks = WTF::move(preview.typesOfValues);
    }
    noteTypesOfLooksLieInFrame(*this);

    // No effect ends what is known of a fresh object, and the blocks of the loop are read here no third time.
    if (has(Mechanism::EscapeAnalysis) && !preview.hasLetMentionsLeave)
        m_state.clear();
    else
        m_state.invalidate(preview.effects);

    BlockID head = m_code.addBlock(BlockRole::Ordinary, frequency);
    m_code.setLoopMarks(head, !m_numberOfCopiesBuiltForSize, true);
    Vector<ValueID, 8> parameters;
    for (uint32_t index = 0; index < numberOfParameters; ++index) {
        TypeID type = preview.typesOfParameters[index];
        if (!type || !entryTypes[index] || !m_types.isSubset(entryTypes[index], type)) {
            if (type && entryTypes[index] && walkUnitAgainWithWiderHead(inputHead, index, type, entryTypes[index]))
                return;
            fail("what enters a loop lies outside the previewed type of the head's parameter"_s);
            return;
        }
        // Whoever wants the integer in a turn converts it without a test, which costs less than the way there and back at each step.
        Representation form = formOfParameter(type, { });
        if (form == Representation::Int64 && index < preview.parametersSteppedOnDoubles.size() && preview.parametersSteppedOnDoubles[index])
            form = Representation::Double;
        parameters.append(m_code.appendParameter(head, type, form, m_origin));
        bind(input.parameter(index), Reference::value(parameters.last()));
    }
    // The turns step an iteration by index that was opened in front of the loop. No value of the input stands for its position, which comes round like one.
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

    for (Reference argument : entering)
        letLeave(m_state, argument);
    if (!preview.effects.maySuspend)
        leaveBlockOpenInFrontOfLoop(inputHead, head, entering.span(), preview.effects);
    else
        endWithJumpInto(head, entering.span());
    if (!m_hasFailed)
        m_block = head;
    noteHeadOfTurn(inputHead);
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

WalkState* Walker::Frame::findStateOfHandler(BlockID handler)
{
    size_t place = placeOfStateOfHandler(handler);
    return place == notFound ? nullptr : &statesOfHandlers[place].state;
}

void Walker::Frame::addStateOfHandler(BlockID handler, WalkState&& state)
{
    constexpr size_t mostLookedThrough = 8;
    statesOfHandlers.append(StateOfHandler { handler, WTF::move(state) });
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
    // The last takes its place.
    if (place + 1 < statesOfHandlers.size()) {
        statesOfHandlers[place] = WTF::move(statesOfHandlers.last());
        if (!placesOfStatesOfHandlers.isEmpty())
            placesOfStatesOfHandlers.set(statesOfHandlers[place].handler.index(), static_cast<uint32_t>(place));
    }
    statesOfHandlers.removeLast();
    placesOfStatesOfHandlers.remove(handler.index());
    return true;
}

void Walker::noteStateAtHeadForHandler()
{
    Frame& frame = m_frames.last();
    BlockID handler = frame.index->block(frame.block).handler().block();
    if (WalkState* known = frame.findStateOfHandler(handler)) {
        if (!known->isEmpty())
            known->meetWith(m_state);
        return;
    }
    frame.addStateOfHandler(handler, WalkState(m_state));
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
    m_frames.last().takeStateOfHandler(inputBlock, m_state);
    enterHandler(*this, inputBlock);
}

// ---- The walk

void Walker::walkBlock(BlockID inputBlock)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    BodyBlock block = index.block(inputBlock);
    // A callee is walked in the middle of an operation of its caller, whose blocks may wait.
    if (!frameIndex)
        m_pendingBlocks.shrink(0);
    if (!m_openLoops.isEmpty())
        endBlocksInFrontOfLoops(frameIndex, inputBlock);

    if (inputBlock != entryBlock) {
        m_block = { };
        if (this->inputBlock(frameIndex, inputBlock).firstOpenEdge == noOpenEdge)
            return;
        // Link has heard of nothing that stands there.
        const Vector<BlockID>& passedOverByScan = facts().summaries().at(currentFunction()).blocksNeverEntered;
        if (!passedOverByScan.isEmpty() && std::ranges::binary_search(passedOverByScan, inputBlock)) {
            fail("an edge leads into a block that the scan has passed over"_s);
            return;
        }
        standIn(inputBlock);
        m_origin = { .position = Instruction::noPosition, .inlineFrame = m_frames[frameIndex].inlineFrame, .isTailCall = false, .isPartOfConstruction = false };
        if (block.isHandlerBlock())
            enterHandlerBlock(inputBlock);
        else if (m_frames[frameIndex].headOfTurnsWrittenOut == inputBlock) {
            enterJoin(inputBlock);
            this->inputBlock(frameIndex, inputBlock).isEntered = false;
            if (m_frames[frameIndex].turnsOfForIn && !isUnreachable())
                standAtHeadOfTurnOfForIn(inputBlock);
        } else if (index.isLoopHead(inputBlock))
            enterLoopHead(inputBlock);
        else if (walkBlockApartFromClosedWays(inputBlock) || walkBlockForEachWayThatBringsObjectInSight(inputBlock))
            return;
        else
            enterJoin(inputBlock);
    } else {
        standIn(inputBlock);
        this->inputBlock(frameIndex, inputBlock).isEntered = true;
    }
    if (isUnreachable()) {
        m_block = { };
        return;
    }

    // `if (scale < 0) throw new RangeError("scale")`: what makes the error runs other code, and no turn follows it. The other copy throws: LooksAtLoops::lookAtBlock() passes the block over.
    if (block.terminator().kind() == TerminatorKind::Throw && !block.hasHandler() && standsInTurnThatLooksCountOn()) {
        // enterBehindHeadOfHandlerBlock() has said where the walk of this block begins, and walkOperationsOf(), which takes that back, is not reached.
        m_frames[frameIndex].firstOperationToWalk = 0;
        beginTurnAgainInClosedCopy();
        return;
    }

    if (block.hasHandler() && has(Mechanism::Handlers))
        noteStateAtHeadForHandler();
    m_frames[frameIndex].walkedUpTo = 0;
    walkOperationsOf(inputBlock);
    walkTerminator(block.terminator());
}

void Walker::walkStretch(BlockID inputBlock, uint32_t begin, uint32_t end)
{
    size_t frameIndex = m_frames.size() - 1;
    standIn(inputBlock);
    const BodyIndex& index = *m_frames[frameIndex].index;
    uint32_t firstValue = index.block(inputBlock).firstValue().index();
    // The numbers of values run on through the blocks: behind the last value of this one stands the first parameter of the next.
    if (end > index.block(inputBlock).numberOfValues()) {
        fail("a stretch ends behind the last operation of its block"_s);
        return;
    }

    // A link of the chain may walk a stretch that begins at its operation and goes on past it.
    uint32_t walkedBefore = std::exchange(m_frames[frameIndex].walkedUpTo, std::max(begin, std::exchange(m_frames[frameIndex].headWalkedForEachWay, 0u)));
    uint32_t endBefore = std::exchange(m_frames[frameIndex].endOfStretch, end);
    ++m_frames[frameIndex].numberOfStretchesBeingWalked;
    for (uint32_t place = begin; place < end && !isUnreachable(); ++place) {
        if (place >= m_frames[frameIndex].walkedUpTo)
            walkOperation(index.value(ValueID(firstValue + place)));
    }
    Frame& frame = m_frames[frameIndex];
    frame.walkedUpTo = std::max({ walkedBefore, frame.walkedUpTo, end });
    frame.endOfStretch = endBefore;
    --frame.numberOfStretchesBeingWalked;
}

bool Walker::endArmWithReturnOf(BlockID inputBlock)
{
    walkTerminator(m_frames.last().index->block(inputBlock).terminator());
    return true;
}

template<Opcode known> bool Walker::walkThroughChainOf(const BodyValue& walked)
{
    BodyValueOf<known> value(walked);
    if (hasTextsInStringBuilders() && bindCastOfTextInStringBuilder(*this, value))
        return false;
    if (value.opcode() == Opcode::Builtin)
        buildFirstReadOfNamedBuiltin(*this, value);
    // Every link builds an allocation as it stands, so what it lacks is asked in front of them.
    if (value.opcode() == Opcode::NewObject && has(Mechanism::Guesses) && m_opensGuesses && openGuessAtAllocation(*this, value))
        return true;
    // `for (const k in o) n += o[k]`. In front of every link: BuiltinCalls takes `Object.hasOwn(o, k)`, and TypedOperations takes `o[k]` wherever it knows a layout of `o`, each by the text of the key.
    if (has(Mechanism::Guesses) && m_opensGuesses && useKeyOfForInBehindTest(*this, value))
        return true;
    // `line += `${c},``. In front of TypedOperations, which builds the conversion of anything.
    if (has(Mechanism::Guesses) && m_opensGuesses && guessThatConvertedPartOfTextInBuilderHasText(*this, value))
        return true;
    // `super(...args)` in a copy of the constructor that the language gives a class. In front of TypedOperations, which writes a list that the state knows out into the closed construction.
    if (value.opcode() == Opcode::SuperConstructList && has(Mechanism::Guesses) && m_opensGuesses && !findOnlyCallee(*this, value, referenceOf(value.parentOperand()))) {
        if (GuessOpened opened = openGuessAt(*this, value); opened != GuessOpened::None)
            return opened == GuessOpened::WithArms;
    }
    bool isBuilt = (hasURLsThatAreNeverMade() && answerFromURLThatIsNeverMade(*this, value))
        || buildDeferredDefinition(*this, value)
        || (has(Mechanism::Folding) && foldOperation(*this, value))
        || (has(Mechanism::Folding) && foldOperationOnKnownTexts(*this, value))
        || (has(Mechanism::BuiltinCalls) && buildDefinitionByWrittenDescriptor(*this, value))
        || (has(Mechanism::EscapeAnalysis) && m_opensGuesses && listStoresOfCopiedConstructor(*this, value))
        || (has(Mechanism::EscapeAnalysis) && m_opensGuesses && listStoresBehindLiteral(*this, value))
        || buildCoroutineOperation(*this, value)
        || (has(Mechanism::Guesses) && m_opensGuesses && has(Mechanism::TypedOperations) && openLoopOverIteratorThatEngineMakes(*this, value))
        || (has(Mechanism::EscapeAnalysis) && buildFreshObject(*this, value))
        || (has(Mechanism::Guesses) && m_opensGuesses && buildArmsInFrontOfTypedForms(*this, value))
        || (has(Mechanism::Guesses) && m_opensGuesses && openGuessAtElementOfNumbers(*this, value))
        || (has(Mechanism::BuiltinCalls) && buildOwnKeyList(*this, value))
        || buildCopyOrKeyOrderForm(*this, value)
        || (m_makesProxyInUnit && buildAccessThroughProxyMadeInUnit(*this, value))
        || buildCopyWithWhatFollows(*this, value)
        || (has(Mechanism::BuiltinCalls) && callBuiltin(*this, value))
        || (has(Mechanism::Inlining) && inlineCalleeAsLinkOfChain(value))
        || (has(Mechanism::DirectCalls) && callDirectly(*this, value))
        || (has(Mechanism::DirectCalls) && callProvenAccessor(*this, value))
        || (has(Mechanism::Folding) && unit().role == CodeRole::SlowBody && program().admitsReplacedBuiltins && buildBuiltinThatExecutableNames(*this, value))
        || (has(Mechanism::TypedOperations) && buildTypedOperation(*this, value));
    if (!isBuilt && value.opcode() == Opcode::PropGet && !m_waysIntoClosedCopies.isEmpty() && has(Mechanism::Guesses) && m_opensGuesses && askObjectAtHeadOfTypedCopy(value))
        isBuilt = buildTypedOperation(*this, value);
    if (!isBuilt && has(Mechanism::Guesses) && m_opensGuesses) {
        GuessOpened opened = openGuessAt(*this, value);
        if (opened == GuessOpened::WithArms)
            return true;
        isBuilt = opened == GuessOpened::AsOneOperation || (value.opcode() == Opcode::FindsDeclaredMember && askGuardsAboutDeclaredMember(*this, value));
    }
    if (!isBuilt && m_explainsClosedAccessesTo) [[unlikely]]
        explainClosedAccess(*this, value, *m_explainsClosedAccessesTo);
    if (!isBuilt)
        isBuilt = buildOperatorBesideOneNumber(*this, value);
    if (!isBuilt && callThroughTargetResolvedOnce(*this, value))
        return false;
    if (!isBuilt && !replaceOperation(*this, value))
        copy(value);
    return false;
}

// AOTWalkLinks.h: one copy of the chain an opcode.
bool Walker::walkThroughChain(const BodyValue& written)
{
    using Chain = bool (Walker::*)(const BodyValue&);
    static constexpr auto chains = []<size_t... opcodes>(std::index_sequence<opcodes...>) {
        return std::array<Chain, sizeof...(opcodes)> { &Walker::walkThroughChainOf<static_cast<Opcode>(opcodes)>... };
    }(std::make_index_sequence<numberOfEnumerators<Opcode>>());
    BodyValue value = asWalked(written);
    if (!m_waysIntoClosedCopies.isEmpty())
        proveWhatLooksCountOnAt(value);
    return (this->*chains[static_cast<size_t>(value.opcode())])(value);
}

static constexpr OpcodeSet readsThatMemoryAnswers { Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::PropGet, Opcode::PrivateGet, Opcode::ElemGet, Opcode::PropHas, Opcode::PrivateHas };

// TestingSwitch::CheckProofsAtRunTime. THE WALK's OWN BELIEFS: it has answered a read from what it remembers of the place: that an object is fresh, that no callee stores there, that a store in a loop is not lost.
// The read is built as well, by a walk that asks the state for no answer, and what it finds is held against what was remembered. True: each arm has walked the operation.
bool Walker::checkWhatIsRememberedOf(const BodyValue& read)
{
    // The read is built again without the answers of the state, and the note of the row in front of the loop is one of them: it would test for itself, where the look at the loop has counted on the form behind the row.
    if (isInLoop() && read.opcode() == Opcode::ElemGet && findListTestedInFrontOfLoop(*this, read)) {
        ++m_numberOfProofsThatNoTestTells;
        return false;
    }
    Reference remembered = referenceOf(read.id());
    TypeID type = remembered ? typeOf(remembered) : TypeID();
    // An empty place is no operand of a comparison.
    if (!type || !m_types.at(type).isJavaScriptValue() || (m_types.at(type).primitives & Primitive::Absent)) {
        ++m_numberOfProofsThatNoTestTells;
        return false;
    }
    bool mayBeNaN = m_types.at(type).primitives & Primitive::Double;
    // The walk goes on with what it believed, or the first check would end the beliefs that the next ones are to try. What is built here reads, and may collect.
    WalkState believed = m_state;
    auto goOnAsBelieved = makeScopeExit([&] {
        m_state = WTF::move(believed);
        Effects effects = Effects::none();
        effects.mayCollect = true;
        m_state.invalidate(effects);
    });
    {
        SetForScope withoutAnswersOfState(m_mechanisms, withoutMechanismsMissingDependencies(m_mechanisms - Mechanism::Folding));
        if (walkThroughChain(read)) {
            goOnAsBelieved.release();
            return true;
        }
    }
    Reference found = referenceOf(read.id());
    // The length of a fresh list, an element of it: others than Folding answer by the state too. In an empty state they find nothing.
    // Not in a loop: the read is built as it is written there, and the look at the loop has counted on its typed form.
    if (!isInLoop() && !isUnreachable() && !hasFailed() && found && (!found.isValue() || found == remembered)) {
        m_state.clear();
        SetForScope withoutAnswersOfState(m_mechanisms, withoutMechanismsMissingDependencies(m_mechanisms - Mechanism::Folding));
        if (walkThroughChain(read)) {
            goOnAsBelieved.release();
            return true;
        }
        found = referenceOf(read.id());
    }
    bind(read.id(), remembered);
    if (isUnreachable() || hasFailed() || !found || !found.isValue() || found == remembered)
        return false;
    auto isSame = [&](Reference left, Reference right) -> Reference {
        if (!left || !right || isUnreachable())
            return { };
        Draft comparison(Opcode::StrictEq, m_origin);
        chooseConservativeFields(comparison);
        comparison.setLeftOperand(left);
        comparison.setRightOperand(right);
        return build(WTF::move(comparison));
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
    // NaN is not itself. The two are the same value where they compare equal and each is itself, or where neither is itself.
    Reference foundIsItself = isSame(found, found);
    Reference rememberedIsItself = isSame(remembered, remembered);
    check(isSame(areSame, foundIsItself));
    check(isSame(foundIsItself, rememberedIsItself));
    return false;
}

// One cell for each object, which nobody but the object can have seen: so nobody can tell whether it is made now or at the first read.
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
    ++m_numberOfValuesWalked;
    m_opcodeBeingWalked = value.opcode();
    // No loop, no poll.
    if (value.opcode() == Opcode::LoopHint && m_frames.last().headOfTurnsWrittenOut == m_frames.last().block)
        return;
    if (value.opcode() == Opcode::NewObject)
        noteClosuresThatAllocationAloneTakes(value);
    FunctionID function = currentFunction();
    m_origin = value.origin();
    m_origin.inlineFrame = m_frames.last().inlineFrame;
    m_callIsAccountedFor = false;
    // A CONSTANT GOES STRAIGHT TO copy(): it has no operand, no link of the chain takes one, it runs no code, and a look at a loop types it by the row that types its record.
    // Whoever gives a link one of these opcodes takes it out of this line.
    if (constantsInFormOfUses.contains(value.opcode())) {
        copy(value);
        return;
    }
    SetForScope isWalkingAccess(m_isWalkingAccess, effectRuleOf(value.opcode()) == EffectRule::Access);
    SetForScope resultOfRead(m_resultOfReadBeingTyped, value.opcode() == Opcode::ElemGet ? facts().linked().resultOfReadByKey(function, value.id()) : TypeID());

    std::optional<StringsInPlaceOfTextsInBuilders> strings;
    if (hasTextsInStringBuilders()) {
        // The string is no part of the access that reads it, and no look at a loop around has been promised anything of it.
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
    if (strings && strings->bindsWhatNoLookHasSeen())
        numberOfRecordsInFront = UINT32_MAX;
    // Each arm has walked the operation through its chain once more, and has left what it had to leave.
    if (!(strings && strings->hasBoundOperation()) && walkThroughChain(value))
        return;

    // Nothing was built, so an older value was handed on in the place of the operation. A cast is its operand in a look too. Of any other operation: widestTypeOfOperation().
    // Only what the state answers is handed on so. Without that the operation is built, and the state learns what it gives.
    if (m_code.numberOfValues() == numberOfRecordsInFront && has(Mechanism::Folding) && !opcodesThatStateTypes.contains(value.opcode()) && !isUnreachable() && isInLoop()) {
        TypeID handedOn = ownTypeOf(referenceOf(value.id()));
        TypeID widest = handedOn ? widestTypeOfOperation(*this, value) : TypeID();
        if (widest && !m_types.isSubset(handedOn, widest)) {
            SetForScope withoutAnswersOfState(m_mechanisms, withoutMechanismsMissingDependencies(m_mechanisms - Mechanism::Folding));
            if (walkThroughChain(value))
                return;
        }
    }

    // What a look at a builder has answered is no belief of the state, and the operand is the builder still.
    bool isAnsweredByBuilder = strings && strings->bindsWhatNoLookHasSeen();
    if (checksProofs() && !isAnsweredByBuilder && has(Mechanism::Folding) && readsThatMemoryAnswers.contains(value.opcode()) && !isUnreachable() && !hasFailed()) {
        // A getter that was run and gives a constant has been run: only where nothing but constants was built is the read walked a second time.
        bool hasBuiltOnlyConstants = true;
        for (uint32_t index = numberOfRecordsInFront; index < m_code.numberOfValues(); ++index)
            hasBuiltOnlyConstants &= m_code.isConstant(ValueID(index));
        if (hasBuiltOnlyConstants && referenceOf(value.id()) && checkWhatIsRememberedOf(value))
            return;
    }

    // THE ONE PLACE THAT KEEPS ARRIVALS SOUND: AOTArrivals.h.
    const LinkTables& linked = facts().linked();
    static const OpcodeSet opcodesThatMayRunOtherCode = opcodesWhere([](Opcode opcode) {
        return declaredBoundInTable(opcode).mayRunOtherCode() || operandsMayRunCode(opcode);
    });
    if (!opcodesThatMayRunOtherCode.contains(value.opcode()) || linked.calls.isEmpty())
        return;
    // What was built may call another function than the one that Link lists here: the close of a loop calls a function of the library, and that calls `return` of the program's iterator.
    FunctionID callee = linked.calleeAt(function, value.id());
    if (callee && !(m_callIsAccountedFor && m_calleeAccountedFor == callee))
        m_arrivals.leaveAnything(callee);
}

void Walker::walkJump(const BodyTerminator& terminator)
{
    BodyTarget target = terminator.next();
    // What comes round is brought into its form in the block that it leaves.
    if (BlockID head = inputBlock(m_frames.size() - 1, target.block()).head; head && target.proofs().empty()) {
        Vector<Reference, 8> arguments;
        for (ValueID argument : target.arguments())
            arguments.append(referenceOf(argument));
        closeBackEdge({ }, head, arguments.span());
        return;
    }

    BlockID from = m_block;
    m_draftOfJump.setOrigin(m_origin);
    m_code.end(from, m_draftOfJump, 1);
    leadEdge(m_code.at(from).terminator.firstEdge, from, terminator, 0, target);
    if (m_hasFailed)
        return;
    m_block = { };
    m_state.clear();
}

void Walker::walkTerminator(const BodyTerminator& terminator)
{
    if (isUnreachable())
        return;
    size_t frameIndex = m_frames.size() - 1;
    m_origin = terminator.origin();
    m_origin.inlineFrame = m_frames[frameIndex].inlineFrame;
    if (hasTextsInStringBuilders())
        takeTextsOutOfStringBuildersAtEndOfWay(*this, terminator);

    if (terminator.kind() == TerminatorKind::ReturnCall) {
        for (TailCast cast : terminator.as<TerminatorKind::ReturnCall>().casts())
            checkInt32Promise(*this, cast.declared, terminator.operand(0), m_origin);
    }

    // It has no operand: nobody ends the block in another way, nothing is asked, converted or ended.
    if (terminator.kind() == TerminatorKind::Jump && !hasTextsInStringBuilders()) {
        walkJump(terminator);
        return;
    }
    if (endBlockWithSwitchOverTestsOfField(terminator))
        return;
    askShapeInPlaceOfTagAtBranch(terminator);
    if (endBlockInAnotherWay(terminator))
        return;
    switch (terminator.kind()) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        if (!m_environmentCellsOfUnit.names.isEmpty() && terminator.kind() == TerminatorKind::Return)
            handBackEnvironmentsAtReturn(*this, terminator.block());
        // Of any other callee: the block stays open, and walkInline() goes on behind the call.
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

    // `switch (s)`, `if (s)`, `throw s`: whoever decides by the operands where the terminator leads reads the text, and the type of a builder says nothing of a text.
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
    // It leads the edges too, whose arguments are names of the text.
    if (!strings && leaveBlockAnotherWay(terminator))
        return;
    TerminatorDraft draft(terminator.kind(), m_origin);
    bool lacksOperand = false;
    copyFields(terminator, draft, [&](ValueID operand) {
        Reference reference = referenceOf(operand);
        lacksOperand |= !reference;
        return reference;
    });
    if (lacksOperand) {
        fail("a terminator takes a value of the body that nothing was built for"_s);
        return;
    }

    // A branch on a constant is a jump.
    std::optional<uint32_t> onlyTarget;
    if (has(Mechanism::Folding)) {
        onlyTarget = onlyTargetKnownToFolding(*this, terminator);
        if (onlyTarget)
            draft.become(TerminatorKind::Jump);
    }

    std::optional<SwitchOnNumbers> switchOnNumbers = !onlyTarget && terminator.hasCases() ? planSwitchOnNumbers(*this, terminator) : std::nullopt;
    if (switchOnNumbers && !switchOnNumbers->cases.isEmpty()) {
        draft.become(TerminatorKind::Switch);
        draft.setCases(switchOnNumbers->cases.span());
    }
    if (switchOnNumbers && switchOnNumbers->subject)
        draft.setSubjectOperand(switchOnNumbers->subject);
    strings.reset();

    // What comes round is brought into its form in the block that it leaves.
    if (draft.kind == TerminatorKind::Jump) {
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

    if (!operandsAreAsTaken(draft)) {
        askTruthOfOperandsNotProvenBoolean(draft);
        if (isUnreachable() || !checkOperandKinds(draft))
            return;
        convertOperands(draft);
        if (isUnreachable())
            return;
    }
    if (!isDeclaredPure(draft.kind)) {
        Effects effects = effectsOfTerminator(typing(), draft, typesOfOperands(draft));
        if (effects.mayCollect && isUnderHandler())
            effects.mayThrow |= ThrowEffect::onlyForLackOfMemory();
        if (effects.mayThrow) {
            leadHandlerEdge();
            if (isUnreachable())
                return;
        }
        m_state.invalidate(effects);
        if (effects.mayThrow && has(Mechanism::Handlers))
            noteStateAtThrowForHandler(draft.operands());
    }

    BlockID from = m_block;
    uint32_t numberOfEdges = onlyTarget ? 1 : terminator.numberOfTargets();
    m_code.end(from, draft, numberOfEdges);
    m_block = { };
    if (!numberOfEdges)
        return;

    uint32_t firstEdge = m_code.at(from).terminator.firstEdge.index();
    WalkState atEnd;
    if (numberOfEdges > 1)
        atEnd = WTF::move(m_state);
    uint32_t targetIndex = 0;
    uint32_t numberOfEdgesLed = 0;
    for (BodyTarget target : terminator.targets()) {
        uint32_t indexOfTarget = targetIndex++;
        if (onlyTarget && *onlyTarget != indexOfTarget)
            continue;
        EdgeID edge(firstEdge + numberOfEdgesLed++);
        if (numberOfEdgesLed < numberOfEdges)
            m_state = atEnd;
        else if (numberOfEdges > 1)
            m_state = WTF::move(atEnd);
        leadEdge(edge, from, terminator, indexOfTarget, target);
        if (m_hasFailed)
            return;
    }
    m_block = { };
    m_state.clear();
}

// ---- Returns

// The terminator generates the call, so the call is the last operation of the block and nothing may be built behind it.
bool Walker::staysTailCall(Reference call) const
{
    if (!returnsResultOfFunction(unit().role) || isUnderHandler() || m_code.at(m_block).handler || m_code.holdsFrameCells())
        return false;
    if (plan().moduleEvaluation().moduleThatBeginsIn(unit().function))
        return false;
    if (!call.isValue() || m_code.lastOperationOf(m_block) != call.id())
        return false;
    const Operation& record = m_code.at(call.id());
    if (!callsWithTailFormInWalk.contains(record.opcode) || !record.isTailCall)
        return false;

    // The callee returns in place of the unit. Whatever is entered by value returns one boxed value.
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

    if (terminator.kind() == TerminatorKind::Return)
        finishBirthAtReturnOfClassConstructor(*this, returned, m_origin);
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
    if (terminator.kind() == TerminatorKind::Return)
        finishBirthAtReturnOfClassConstructor(*this, value, m_origin);
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

    // A cast of the input becomes its operand, so the casts of a tail call build nothing.
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

// The plan gave the result a raw form because Link has proven it a number or a boolean. The walk's own type of the value may say less: it has come round a loop whose preview gave up, or out of a closed operation.
// Code has no way to state a type. So the type is tested for, and the failing side traps: it is no guess and has no closed arm, and only a false claim of Link takes it. Null: no run comes here.
static Reference proveFormOfResultOrTrapInWalk(Walker& walker, Reference value, Representation form, Origin origin)
{
    constexpr RepresentationSet rawFormsOfResults { Representation::Boolean, Representation::Int32, Representation::Double, Representation::OptionalInt64, Representation::OptionalDouble };
    if (!rawFormsOfResults.contains(form) || isLegalRepresentation(walker.types().at(walker.typeOf(value)), form))
        return value;

    origin.isTailCall = false;
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(value);
    test.setTestedType(widestTypeOf(form));
    Reference passes = walker.build(WTF::move(test));
    if (!passes)
        return { };

    BlockID passing = walker.addBlock();
    BlockID failing = walker.addBlock(BlockFrequency::Rare);
    walker.branch(passes, passing, failing);
    walker.continueIn(failing);
    walker.code().end(failing, TerminatorDraft(TerminatorKind::Unreachable, origin), 0);
    walker.continueIn(passing);

    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(passes);
    narrowed.setOperandIndex(OperandIndex::TypeTest::value);
    narrowed.setIsOnTakenEdge(true);
    return walker.build(WTF::move(narrowed));
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
        // Whoever calls the function takes Link's word for what comes back. Here it goes out.
        if (m_positionsWithoutProofs && returnsResultOfFunction(unit().role) && !program().at(unit().function).isCoroutine() && unit().function.index() < facts().linked().functions.size())
            checkProofOfCopy(values[0], facts().linked().function(unit().function).result, ProofSource::ResultOfCall, origin);
        returned = proveFormOfResultOrTrapInWalk(*this, values[0], conventions.resultRepresentation(unit()), origin);
        if (!returned || isUnreachable())
            return;
    } else {
        // No rule can tell the list that a unit returns from one that only ResultAt reads, so each value is brought into its place here and a new list is returned.
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
