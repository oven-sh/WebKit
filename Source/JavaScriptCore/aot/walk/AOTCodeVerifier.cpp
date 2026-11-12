/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCode.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTClosureWord.h"
#include "AOTConvention.h"
#include "AOTEffectsDeclared.h"
#include "AOTHeldOperands.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOperandKinds.h"
#include "AOTPlan.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTSumOverRecords.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTWalk.h"
#include <algorithm>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class PositionsOfCodeVerifier final : public PositionTypes {
    WTF_MAKE_NONCOPYABLE(PositionsOfCodeVerifier);
public:
    PositionsOfCodeVerifier(const Program& program, const Plan* plan)
        : m_program(program)
        , m_plan(plan)
    {
    }

    TypeID variable(TypeLevel, VariableID variable) const final
    {
        if (!m_plan)
            return { };
        if (m_plan->deferredDefinitions().findDeferred(variable))
            return TypeTable::anythingOrAbsent;
        return { };
    }

    TypeID environmentField(TypeLevel, LayoutID, uint32_t) const final { return { }; }
    bool environmentLiesAmongVariables(LayoutID layout) const final { return m_plan && m_plan->promotedEnvironments().isPromoted(layout); }
    TypeID slot(TypeLevel, LayoutID layout, uint32_t field) const final
    {
        if (!m_plan)
            return { };
        if (!m_plan->isInUse(layout))
            return TypeTable::never;
        const PlacedSlot& placed = m_plan->slot(layout, field);
        return placed.hasStorage() ? placed.type : TypeID();
    }
    TypeID property(TypeLevel, ObjectID, PropertyKeyID) const final { return { }; }
    TypeID result(TypeLevel, FunctionID, uint32_t) const final { return { }; }
    TypeID entryParameter(TypeLevel, FunctionID, uint32_t) const final { return { }; }
    TypeID resultOfCallBeingTyped(TypeLevel, uint32_t) const final { return { }; }

    bool entryParameterIsLeftOutOfCalls(FunctionID function, uint32_t entryParameterIndex) const final
    {
        if (!m_plan || entryParameterIndex >= m_plan->conventions().of(function).numberOfEntryParameters)
            return false;
        return m_plan->conventions().parameterRepresentation(m_program, { function, CodeRole::Body }, entryParameterIndex) == Representation::Void;
    }

private:
    const Program& m_program;
    const Plan* m_plan;
};

static constexpr OpcodeSet arrayAccessOpcodes { Opcode::HasContiguousIndex, Opcode::IsElementPresent, Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::PutContiguousIndex, Opcode::PutDoubleIndex };

class CodeVerifier {
    WTF_MAKE_NONCOPYABLE(CodeVerifier);
public:
    CodeVerifier(const CodeTextContext&, const Code&);

    String run();
    String runOnSweptCode();
    String runOnFrameCellsAlone();
    InternalErrorKind kindOfFailure() const { return m_kindOfFailure; }

private:
    using Stage = void (CodeVerifier::*)();
    String run(std::span<const Stage>);

    static constexpr uint32_t notListed = UINT32_MAX;
    static constexpr uint32_t notReached = UINT32_MAX;

    void fail(BlockID, ValueID record, ASCIILiteral rule, ValueID other = { }, InternalErrorKind = InternalErrorKind::OpcodeNotInForm);
    bool hasFailed() const { return !m_failure.isNull(); }

    bool isRecord(ValueID id) const { return id && id.index() < m_code.numberOfValues(); }
    bool isBlock(BlockID id) const { return id && id.index() < m_code.numberOfBlocks(); }
    std::span<const ValueID> operationsOf(BlockID) const;
    std::span<const EdgeID> incomingEdgesOf(BlockID) const;

    static uint32_t headOf(BlockID block) { return block.index() * 2; }
    static uint32_t tailOf(BlockID block) { return block.index() * 2 + 1; }
    static bool isTail(uint32_t node) { return node & 1; }
    uint32_t root() const { return m_code.numberOfBlocks() * 2; }
    uint32_t numberOfSuccessorsOf(uint32_t node) const;
    uint32_t successorOf(uint32_t node, uint32_t index) const;
    uint32_t nodeThatEdgeLeaves(EdgeID) const;
    uint32_t nodeThatDefines(ValueID) const;
    bool dominates(uint32_t dominator, uint32_t node) const;
    bool isReached(BlockID block) const { return m_numbers[headOf(block)] != notReached; }
    bool isWhereWalkBegan(BlockID block) const { return m_code.at(block).isEntry; }
    bool isDefinedBefore(ValueID definition, BlockID, uint32_t place) const;
    bool isDefinedWhereBlockMayThrow(ValueID definition, BlockID) const;
    EdgeID findAnswerEdge(ValueID test, bool testResult, BlockID from) const;
    bool isAnsweredAt(ValueID test, bool testResult, BlockID, uint32_t place);
    bool everyPathPassesOneOf(std::span<const ValueID> tests, std::span<const ValueID> cells, BlockID) const;
    bool areSameCells(ValueID, ValueID) const;
    bool mustAlias(ValueID, ValueID) const;
    ValueID findArrayWriteBetween(ValueID ground, ValueID access) const;
    bool isLengthOfView(ValueID length, ValueID view, BlockID);
    struct ListsMade {
        uint64_t leastLength;
        bool areWhole;
    };
    std::optional<ListsMade> listsMadeUnder(ValueID name, BlockID);
    const DataNode::Array* findWrittenDataShownOf(ValueID cell, BlockID);
    std::optional<Vector<ValueID>> m_testsForWrittenData;

    void listRecords();
    void verifyEdges();
    void verifyEntries();
    void orderNodes();
    void listIncomingEdges();
    void computeDominators();
    void numberDominatorTree();
    void verifyWordsOfRecords();
    void noteValuesOwnedByEnds();
    void verifyRecords();
    void verifyNarrowedValues();
    void verifyUsersOfHolesReadAsNaN();
    void verifyUsersOfReadNumbers();
    bool treatsUndefinedAsNaN(ValueID, const Operation& user, uint32_t operandIndex) const;
    bool isOperatorSpecializationCheck(ValueID test) const;
    void verifyTestsPassed();
    void verifyLengthsHeldAgainst();
    void verifyKindsOfStorageNamed();
    void verifyFrameCells();
    void verifyWordsInPlaceOfCaptures();

    template<typename Entry> void verifyWords(BlockID, ValueID, const Instruction&, Entry);
    void verifyBlock(BlockID);
    void verifyOperation(BlockID, uint32_t place, ValueID, const Operation&);
    void verifyEnd(BlockID, uint32_t place);
    void verifyReturn(BlockID, const Terminator&);
    void verifyArguments(BlockID from, const Edge&, uint32_t place, uint32_t numberOfDeclaredResults, bool isHandlerEdge);
    void verifyNarrowed(BlockID, ValueID, const Operation&);
    void verifyPlainFieldWrites(BlockID, ValueID, const Operation&);
    void verifyPointerIntoStorage(BlockID, ValueID access, ValueID pointer, Opcode readOfPointer, ValueID owner);
    void verifyFramePointersTakenBy(BlockID, ValueID, const Operation&);
    void verifyFramePointersTakenByEnd(BlockID, const Terminator&);
    void verifyMakerOfClosure(BlockID, ValueID, const Operation&);
    void verifyClosuresOfFrameTakenBy(BlockID, ValueID, const Operation&);
    void verifyEnvironmentsHeldByClosuresOfFrame(BlockID);

    template<typename Record> void collectOperands(BlockID, uint32_t place, ValueID user, const Record&, ValueID ownedValueItMayUse);
    template<typename Record> void verifyFormsOfOperands(BlockID, ValueID, const Record&);
    bool hasLegalForm(const Operation&) const;
    std::optional<uint32_t> numberOfResultsGivenBy(const Operation& producer) const;
    bool holdsNoneOfItsResults(const Operation& producer) const;

    static ValueID operandAt(const Instruction& instruction, uint32_t operandIndex) { return operandIndex < instruction.numberOfOperands() ? instruction.operand(operandIndex) : ValueID(); }
    static bool isFrameCell(const Operation& record) { return PayloadWord::storage(record.opcode) && record.storage() != AllocationStorage::Heap; }
    static bool mayNameProducerOfSeveralResults(const Operation& user, uint32_t operandIndex) { return user.operandKind(operandIndex) == OperandKind::Results; }
    static bool mayNameProducerOfSeveralResults(const Terminator& user, uint32_t) { return user.kind == TerminatorKind::Return || user.kind == TerminatorKind::ReturnCall; }
    static bool hasOnlyLowestBitsOfInt32(const Operation& record) { return record.opcode == Opcode::ToInt32 && record.givesOnlyLowestBits(); }
    bool looksOnlyAtLowestBitsOfOperand(const Operation& user, uint32_t operandIndex) const { return user.opcode == Opcode::BitAnd && isMaskOfLowestBitsOfInt32(m_types.at(m_code.at(user.operand(1 - operandIndex)).type)); }
    bool looksOnlyAtLowestBitsOfOperand(const Terminator&, uint32_t) const { return false; }
    bool isListOfFlatEntries(ValueID) const;
    static bool readsListOfFlatEntries(const Operation& user) { return user.opcode == Opcode::GetArrayLength || user.opcode == Opcode::GetArrayElement || cellPreservingOpcodes.contains(user.opcode); }
    static bool readsListOfFlatEntries(const Terminator&) { return false; }
    static ValueID valueOwnedBy(const Terminator&);
    static size_t numberOfWrittenCasesOf(const Terminator&);
    static EffectTraits boundOnTraitsOf(const Operation&);
    static bool handsOperandsToCallee(Opcode);
    static ValueID operandHandedBackBy(const Operation&);
    ValueID valueBeforeConversions(ValueID) const;
    ValueID holderOfSlotRead(ValueID read) const;
    ValueID cellBeforeForwarding(ValueID) const;
    ValueID underlyingCell(ValueID) const;
    void collectUnderlyingCells(ValueID name, Vector<ValueID, 4>& cells) const;
    bool mayPointIntoFrame(ValueID id) const { return m_mayPointIntoFrame.quickGet(id.index()); }
    bool handsBackPointerIntoFrame(const Operation&) const;
    bool isClosureOfFrame(ValueID name) const;
    bool onlyCallsWhatArrivesAt(FunctionID function, uint32_t entryParameter) const { return m_facts && m_facts->linked().onlyCallsWhatArrivesAt(function, entryParameter); }
    bool loanCoversOperandOfCall(const Operation& call, uint32_t operandIndex, ValueID operand) const;
    bool isCallOfTailCall(BlockID block, ValueID call) const { return m_code.at(block).terminator.kind == TerminatorKind::ReturnCall && m_code.at(block).terminator.callOperand() == call; }
    LayoutID layoutOfEnvironment(ValueID) const;

    const Program& m_program;
    TypeArena& m_types;
    const Plan* m_plan;
    const Facts* m_facts;
    const Code& m_code;
    PositionsOfCodeVerifier m_positions;
    TypingContext m_typing;
    String m_failure;
    InternalErrorKind m_kindOfFailure { InternalErrorKind::OpcodeNotInForm };

    Vector<uint32_t> m_places;
    BitVector m_isOwnedByEnd;
    BitVector m_mayPointIntoFrame;
    BitVector m_namesOfClosuresTaken;
    Vector<uint32_t> m_firstOperation;
    Vector<uint32_t> m_placeOfFirstThrow;
    Vector<uint32_t> m_firstIncomingEdge;
    Vector<uint32_t> m_numbers;
    Vector<uint32_t> m_dominators;
    Vector<uint32_t> m_firstNumberOfSubtree;
    Vector<uint32_t> m_lastNumberOfSubtree;

    Vector<ValueID> m_operations;
    Vector<EdgeID> m_incomingEdges;
    Vector<BlockID, 2> m_entryBlocks;
    Vector<uint32_t> m_nodesInOrder;
    Vector<BlockID> m_reachedBlocks;
    Vector<TypeID, 16> m_operandTypes;
    Vector<Representation, 16> m_operandForms;
    HashMap<uint64_t, BitVector, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_reachedWithoutAnswers;
};

CodeVerifier::CodeVerifier(const CodeTextContext& context, const Code& code)
    : m_program(context.program)
    , m_types(context.types)
    , m_plan(context.plan)
    , m_facts(context.facts)
    , m_code(code)
    , m_positions(context.program, context.plan)
    , m_typing { context.program, context.types, m_positions, TypeLevel::Proven }
{
}

String CodeVerifier::run()
{
    static constexpr Stage stages[] = {
        &CodeVerifier::listRecords,
        &CodeVerifier::verifyEdges,
        &CodeVerifier::verifyEntries,
        &CodeVerifier::orderNodes,
        &CodeVerifier::listIncomingEdges,
        &CodeVerifier::computeDominators,
        &CodeVerifier::numberDominatorTree,
        &CodeVerifier::verifyWordsOfRecords,
        &CodeVerifier::noteValuesOwnedByEnds,
        &CodeVerifier::verifyRecords,
        &CodeVerifier::verifyNarrowedValues,
        &CodeVerifier::verifyUsersOfHolesReadAsNaN,
        &CodeVerifier::verifyUsersOfReadNumbers,
        &CodeVerifier::verifyTestsPassed,
        &CodeVerifier::verifyLengthsHeldAgainst,
        &CodeVerifier::verifyKindsOfStorageNamed,
        &CodeVerifier::verifyFrameCells,
        &CodeVerifier::verifyWordsInPlaceOfCaptures,
    };
    return run(stages);
}

String CodeVerifier::runOnSweptCode()
{
    static constexpr Stage stages[] = {
        &CodeVerifier::listRecords,
        &CodeVerifier::verifyEdges,
        &CodeVerifier::verifyEntries,
        &CodeVerifier::orderNodes,
        &CodeVerifier::listIncomingEdges,
        &CodeVerifier::computeDominators,
        &CodeVerifier::numberDominatorTree,
        &CodeVerifier::verifyWordsOfRecords,
        &CodeVerifier::noteValuesOwnedByEnds,
        &CodeVerifier::verifyRecords,
        &CodeVerifier::verifyNarrowedValues,
        &CodeVerifier::verifyUsersOfHolesReadAsNaN,
        &CodeVerifier::verifyUsersOfReadNumbers,
        &CodeVerifier::verifyTestsPassed,
        &CodeVerifier::verifyLengthsHeldAgainst,
        &CodeVerifier::verifyKindsOfStorageNamed,
        &CodeVerifier::verifyFrameCells,
        &CodeVerifier::verifyWordsInPlaceOfCaptures,
    };
    return run(stages);
}

String CodeVerifier::runOnFrameCellsAlone()
{
    static constexpr Stage stages[] = {
        &CodeVerifier::listRecords,
        &CodeVerifier::verifyEntries,
        &CodeVerifier::orderNodes,
        &CodeVerifier::listIncomingEdges,
        &CodeVerifier::verifyFrameCells,
    };
    return run(stages);
}

String CodeVerifier::run(std::span<const Stage> stages)
{
    for (Stage stage : stages) {
        (this->*stage)();
        if (hasFailed())
            break;
    }
    return m_failure;
}

void CodeVerifier::fail(BlockID block, ValueID record, ASCIILiteral rule, ValueID other, InternalErrorKind kind)
{
    if (hasFailed())
        return;
    m_kindOfFailure = kind;
    StringBuilder text;
    auto appendRecord = [&](ASCIILiteral prefix, ValueID id) {
        text.append(prefix, id.index());
        if (isRecord(id) && static_cast<unsigned>(m_code.at(id).opcode) < numberOfEnumerators<Opcode>)
            text.append(" ("_s, nameOf(m_code.at(id).opcode), ')');
    };
    text.append('f', m_code.function().index(), ' ', nameOf(m_code.role()));
    if (block)
        text.append(" b"_s, block.index());
    if (record)
        appendRecord(" v"_s, record);
    else if (isBlock(block) && m_code.at(block).hasEnded() && static_cast<unsigned>(m_code.at(block).terminator.kind) < numberOfEnumerators<TerminatorKind>)
        text.append(" (ends in "_s, nameOf(m_code.at(block).terminator.kind), ')');
    text.append(": "_s, rule);
    if (other)
        appendRecord(": v"_s, other);
    m_failure = text.toString();
}

std::span<const ValueID> CodeVerifier::operationsOf(BlockID block) const
{
    uint32_t first = m_firstOperation[block.index()];
    return m_operations.span().subspan(first, m_firstOperation[block.index() + 1] - first);
}

std::span<const EdgeID> CodeVerifier::incomingEdgesOf(BlockID block) const
{
    uint32_t first = m_firstIncomingEdge[block.index()];
    return m_incomingEdges.span().subspan(first, m_firstIncomingEdge[block.index() + 1] - first);
}

void CodeVerifier::listRecords()
{
    uint32_t numberOfValues = m_code.numberOfValues();
    m_places.fill(notListed, numberOfValues);
    BitVector isLinkOfList(numberOfValues);

    for (uint32_t blockIndex = 0; blockIndex < m_code.numberOfBlocks(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID parameter : m_code.at(block).parameters()) {
            if (!isRecord(parameter) || m_code.at(parameter).opcode != Opcode::Parameter || m_code.blockOf(parameter) != block)
                return fail(block, parameter, "the block lists a parameter that is none of its own"_s);
            if (std::exchange(m_places[parameter.index()], 0u) != notListed)
                return fail(block, parameter, "the record is listed twice"_s);
        }

        uint32_t first = m_operations.size();
        m_firstOperation.append(first);
        if (m_code.isSwept())
            m_operations.append(m_code.valuesOf(block));
        else {
            for (ValueID id = m_code.lastOperationOf(block); id; id = m_code.at(id).previous) {
                if (!isRecord(id) || isLinkOfList.quickSet(id.index()))
                    return fail(block, id, "the operations of the block do not form a list"_s);
                if (m_code.at(id).opcode != Opcode::Parameter)
                    m_operations.append(id);
            }
            std::ranges::reverse(m_operations.mutableSpan().subspan(first));
        }

        uint32_t place = 0;
        uint32_t placeOfFirstThrow = 0;
        for (ValueID id : m_operations.span().subspan(first)) {
            if (!isRecord(id) || m_code.at(id).opcode == Opcode::Parameter || m_code.blockOf(id) != block)
                return fail(block, id, "the block lists an operation that is none of its own"_s);
            if (std::exchange(m_places[id.index()], ++place) != notListed)
                return fail(block, id, "the record is listed twice"_s);
            if (!placeOfFirstThrow && m_code.at(id).traits().mayThrow())
                placeOfFirstThrow = place;
        }
        m_placeOfFirstThrow.append(placeOfFirstThrow ? placeOfFirstThrow : place + 1);
    }
    m_firstOperation.append(m_operations.size());
}

void CodeVerifier::verifyEdges()
{
    uint32_t numberOfEdges = m_code.numberOfEdges();
    BitVector hasOwner(numberOfEdges);
    auto claim = [&](BlockID block, EdgeID edge) {
        if (!edge || edge.index() >= numberOfEdges || m_code.at(edge).from != block || hasOwner.quickSet(edge.index()))
            fail(block, { }, "the block ends in an edge that is not its own"_s);
    };
    for (uint32_t blockIndex = 0; blockIndex < m_code.numberOfBlocks(); ++blockIndex) {
        BlockID block(blockIndex);
        const Block& contents = m_code.at(block);
        if (m_code.isSwept() && !m_code.isReached(block)) {
            if (contents.isEntry)
                return fail(block, { }, "the sweep has passed an entry of the unit"_s);
            continue;
        }
        if (contents.hasEnded()) {
            for (uint32_t targetIndex = 0; targetIndex < contents.terminator.numberOfEdges; ++targetIndex)
                claim(block, EdgeID::fromBits(contents.terminator.firstEdge.bits() + targetIndex));
        }
        if (EdgeID handler = m_code.handlerOf(block))
            claim(block, handler);
        if (hasFailed())
            return;
    }

    for (uint32_t edgeIndex = 0; edgeIndex < numberOfEdges; ++edgeIndex) {
        const Edge& edge = m_code.at(EdgeID(edgeIndex));
        if (m_code.isAbandoned(EdgeID(edgeIndex)))
            continue;
        if (!hasOwner.quickGet(edgeIndex)) {
            if (m_code.isSwept())
                continue;
            return fail(edge.from, { }, "an edge belongs to the end of no block: its block has ended twice"_s);
        }
        if (edge.isOpen())
            return fail(edge.from, { }, "an edge of the block is open"_s);
        if (!isBlock(edge.to))
            return fail(edge.from, { }, "an edge of the block leads to no block"_s);
        if (m_code.isSwept() && !m_code.isReached(edge.to))
            return fail(edge.from, { }, "an edge of the block leads to a block that the sweep has passed"_s);
        for (ValueID argument : edge.arguments()) {
            if (!isRecord(argument))
                return fail(edge.from, argument, "an argument of an edge is no record"_s);
            if (m_code.isAbandoned(argument))
                return fail(edge.from, argument, "an argument of an edge was made by a walk of a loop that was undone"_s);
        }
    }
}

void CodeVerifier::verifyEntries()
{
    if (m_code.entries().empty())
        return fail({ }, { }, "the unit has no entry"_s);
    if (m_code.entries().size() != 1 && isBodyOfCoroutine(m_code.role(), m_program.at(m_code.function()).isCoroutine()))
        return fail({ }, { }, "a body of a coroutine has an entry beside its start"_s);
    for (const EntryOfCode& entry : m_code.entries()) {
        if (!isBlock(entry.block) || !m_code.at(entry.block).isEntry || m_code.at(entry.block).isHandlerBlock())
            return fail(entry.block, { }, "an entry of the unit is no ordinary block that is marked as an entry"_s);
    }
    for (uint32_t blockIndex = 0; blockIndex < m_code.numberOfBlocks(); ++blockIndex) {
        if (isWhereWalkBegan(BlockID(blockIndex)))
            m_entryBlocks.append(BlockID(blockIndex));
    }

    std::span<const InlineFrame> frames = m_code.inlineFrames();
    if (frames.empty())
        return fail({ }, { }, "the unit has no inline frame for its own function"_s);
    for (uint32_t frameIndex = 1; frameIndex < frames.size(); ++frameIndex) {
        if (frames[frameIndex].parent >= frameIndex)
            return fail({ }, { }, "an inline frame does not stand behind its parent"_s);
    }
}

uint32_t CodeVerifier::numberOfSuccessorsOf(uint32_t node) const
{
    if (node == root())
        return m_entryBlocks.size();
    BlockID block(node / 2);
    if (isTail(node))
        return m_code.at(block).hasEnded() ? m_code.at(block).terminator.numberOfEdges : 0;
    return m_code.handlerOf(block) ? 2 : 1;
}

uint32_t CodeVerifier::successorOf(uint32_t node, uint32_t index) const
{
    if (node == root())
        return headOf(m_entryBlocks[index]);
    BlockID block(node / 2);
    if (isTail(node))
        return headOf(m_code.at(m_code.at(block).terminator.edge(index)).to);
    return index ? headOf(m_code.at(m_code.handlerOf(block)).to) : tailOf(block);
}

void CodeVerifier::orderNodes()
{
    struct Visit {
        uint32_t node;
        uint32_t nextSuccessor;
    };

    uint32_t numberOfNodes = root() + 1;
    m_numbers.fill(notReached, numberOfNodes);
    BitVector isSeen(numberOfNodes);
    isSeen.quickSet(root());
    Vector<Visit, 64> stack;
    stack.append(Visit { root(), 0 });
    while (!stack.isEmpty()) {
        Visit& visit = stack.last();
        if (visit.nextSuccessor == numberOfSuccessorsOf(visit.node)) {
            m_nodesInOrder.append(visit.node);
            stack.removeLast();
            continue;
        }
        uint32_t successor = successorOf(visit.node, visit.nextSuccessor++);
        if (!isSeen.quickSet(successor))
            stack.append(Visit { successor, 0 });
    }
    std::ranges::reverse(m_nodesInOrder.mutableSpan());

    for (uint32_t number = 0; number < m_nodesInOrder.size(); ++number) {
        uint32_t node = m_nodesInOrder[number];
        m_numbers[node] = number;
        if (node == root() || isTail(node))
            continue;
        BlockID block(node / 2);
        if (!m_code.at(block).hasEnded())
            return fail(block, { }, "the block is reached and has no end"_s);
        m_reachedBlocks.append(block);
    }
}

void CodeVerifier::listIncomingEdges()
{
    uint32_t numberOfBlocks = m_code.numberOfBlocks();
    m_firstIncomingEdge.fill(0, numberOfBlocks + 1);
    for (BlockID block : m_reachedBlocks) {
        m_code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
            ++m_firstIncomingEdge[edge.to.index() + 1];
        });
    }
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex)
        m_firstIncomingEdge[blockIndex + 1] += m_firstIncomingEdge[blockIndex];

    m_incomingEdges.fill(EdgeID(), m_firstIncomingEdge.last());
    Vector<uint32_t> next(m_firstIncomingEdge);
    for (BlockID block : m_reachedBlocks) {
        m_code.forEachEdgeOf(block, [&](EdgeID id, const Edge& edge) {
            m_incomingEdges[next[edge.to.index()]++] = id;
        });
    }
}

uint32_t CodeVerifier::nodeThatEdgeLeaves(EdgeID edge) const
{
    BlockID from = m_code.at(edge).from;
    return m_code.handlerOf(from) == edge ? headOf(from) : tailOf(from);
}

uint32_t CodeVerifier::nodeThatDefines(ValueID id) const
{
    BlockID block = m_code.blockOf(id);
    return m_places[id.index()] < m_placeOfFirstThrow[block.index()] ? headOf(block) : tailOf(block);
}

bool CodeVerifier::dominates(uint32_t dominator, uint32_t node) const
{
    return m_firstNumberOfSubtree[dominator] <= m_firstNumberOfSubtree[node] && m_firstNumberOfSubtree[node] <= m_lastNumberOfSubtree[dominator];
}

void CodeVerifier::computeDominators()
{
    m_dominators.fill(notReached, m_numbers.size());
    m_dominators[root()] = root();

    Vector<uint32_t> epochOfLastClimb;
    epochOfLastClimb.fill(0, m_numbers.size());
    uint32_t epoch = 0;

    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        for (uint32_t node : m_nodesInOrder.span().subspan(1)) {
            ++epoch;
            uint32_t dominator = notReached;
            auto meet = [&](uint32_t predecessor) {
                if (m_dominators[predecessor] == notReached)
                    return;
                if (dominator == notReached) {
                    dominator = predecessor;
                    return;
                }
                while (predecessor != dominator) {
                    if (m_numbers[predecessor] < m_numbers[dominator]) {
                        epochOfLastClimb[dominator] = epoch;
                        dominator = m_dominators[dominator];
                        continue;
                    }
                    if (std::exchange(epochOfLastClimb[predecessor], epoch) == epoch)
                        return;
                    predecessor = m_dominators[predecessor];
                }
            };
            BlockID block(node / 2);
            if (isTail(node))
                meet(headOf(block));
            else {
                if (isWhereWalkBegan(block))
                    meet(root());
                for (EdgeID edge : incomingEdgesOf(block))
                    meet(nodeThatEdgeLeaves(edge));
            }
            if (m_dominators[node] != dominator) {
                m_dominators[node] = dominator;
                hasChanged = true;
            }
        }
    }
}

void CodeVerifier::numberDominatorTree()
{
    uint32_t numberOfNodes = m_numbers.size();

    Vector<uint32_t> sizeOfSubtree;
    sizeOfSubtree.fill(1, numberOfNodes);
    for (size_t index = m_nodesInOrder.size(); --index;) {
        uint32_t node = m_nodesInOrder[index];
        sizeOfSubtree[m_dominators[node]] += sizeOfSubtree[node];
    }

    m_firstNumberOfSubtree.fill(0, numberOfNodes);
    m_lastNumberOfSubtree.fill(0, numberOfNodes);
    m_lastNumberOfSubtree[root()] = sizeOfSubtree[root()] - 1;
    Vector<uint32_t> nextNumberBelow;
    nextNumberBelow.fill(1, numberOfNodes);
    for (uint32_t node : m_nodesInOrder.span().subspan(1)) {
        uint32_t first = nextNumberBelow[m_dominators[node]];
        nextNumberBelow[m_dominators[node]] += sizeOfSubtree[node];
        nextNumberBelow[node] = first + 1;
        m_firstNumberOfSubtree[node] = first;
        m_lastNumberOfSubtree[node] = first + sizeOfSubtree[node] - 1;
    }
}

bool CodeVerifier::isDefinedBefore(ValueID definition, BlockID block, uint32_t place) const
{
    if (m_code.isConstant(definition))
        return true;
    if (!block || m_places[definition.index()] == notListed)
        return false;
    BlockID home = m_code.blockOf(definition);
    if (home == block)
        return m_places[definition.index()] < place;
    return isReached(home) && dominates(nodeThatDefines(definition), headOf(block));
}

bool CodeVerifier::isDefinedWhereBlockMayThrow(ValueID definition, BlockID block) const
{
    if (m_code.isConstant(definition))
        return true;
    if (m_places[definition.index()] == notListed || !isReached(m_code.blockOf(definition)))
        return false;
    return dominates(nodeThatDefines(definition), headOf(block));
}

template<typename Entry>
void CodeVerifier::verifyWords(BlockID block, ValueID id, const Instruction& instruction, Entry entry)
{
    if (static_cast<unsigned>(entry) >= numberOfEnumerators<Entry>)
        return fail(block, id, "the record names no entry of the schema"_s);
    uint32_t numberOfOperands = instruction.numberOfOperands();
    uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(entry);
    if (instruction.numberOfNamedOperands() != numberOfNamedOperands || numberOfOperands < numberOfNamedOperands || (numberOfOperands > numberOfNamedOperands && !hasVariadicOperand(entry)))
        return fail(block, id, "the record has other operands than the schema declares"_s);
    if (instruction.flags & ~declaredFlagsOf(entry))
        return fail(block, id, "a flag is set that the schema does not declare"_s);
    if (instruction.inlineFrame >= m_code.inlineFrames().size())
        return fail(block, id, "the record names an inline frame that the unit does not have"_s);

    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(entry, operandIndex);
        ValueID operand = instruction.operand(operandIndex);
        if (!operand && !info.isOptional) {
            bool isFirstFailure = !hasFailed();
            fail(block, id, "a required operand is absent"_s);
            if (isFirstFailure)
                m_failure = makeString(m_failure, ": operand "_s, operandIndex);
            return;
        }
        if (operand && !isRecord(operand))
            return fail(block, id, "an operand is no record"_s, operand);
        if (operand && m_code.isAbandoned(operand) && !m_code.isAbandoned(id))
            return fail(block, id, "an operand was made by a walk of a loop that was undone"_s, operand);
    }
}

void CodeVerifier::verifyWordsOfRecords()
{
    for (uint32_t index = 0; index < m_code.numberOfValues(); ++index) {
        ValueID id(index);
        if (!m_code.isConstant(id))
            continue;
        verifyWords({ }, id, m_code.at(id), m_code.at(id).opcode);
        if (hasFailed())
            return;
    }
    for (BlockID block : m_reachedBlocks) {
        const Block& contents = m_code.at(block);
        if (contents.subject && !isRecord(contents.subject))
            return fail(block, contents.subject, "the subject of the block is no record"_s);
        for (ValueID id : operationsOf(block)) {
            verifyWords(block, id, m_code.at(id), m_code.at(id).opcode);
            if (hasFailed())
                return;
        }
        verifyWords(block, { }, contents.terminator, contents.terminator.kind);
        if (hasFailed())
            return;
    }
}

ValueID CodeVerifier::valueOwnedBy(const Terminator& end)
{
    if (end.kind == TerminatorKind::TestAndBranch)
        return end.conditionOperand();
    if (end.kind == TerminatorKind::ReturnCall)
        return end.callOperand();
    return { };
}

void CodeVerifier::noteValuesOwnedByEnds()
{
    m_isOwnedByEnd.ensureSize(m_code.numberOfValues());
    for (BlockID block : m_reachedBlocks) {
        ValueID owned = valueOwnedBy(m_code.at(block).terminator);
        if (!owned)
            continue;
        std::span<const ValueID> operations = operationsOf(block);
        if (operations.empty() || operations.back() != owned)
            return fail(block, owned, "the end of the block generates an operation that is not the last of the block"_s);
        m_isOwnedByEnd.quickSet(owned.index());
    }
}

template<typename Record>
void CodeVerifier::collectOperands(BlockID block, uint32_t place, ValueID user, const Record& record, ValueID ownedValueItMayUse)
{
    m_operandTypes.shrink(0);
    m_operandForms.shrink(0);
    for (uint32_t operandIndex = 0; operandIndex < record.numberOfOperands(); ++operandIndex) {
        ValueID operand = record.operand(operandIndex);
        if (!operand) {
            m_operandTypes.append(TypeID());
            m_operandForms.append(Representation::Unchosen);
            continue;
        }
        const Operation& definition = m_code.at(operand);
        if (!isDefinedBefore(operand, block, place))
            return fail(block, user, "the definition of an operand does not stand in front of the use on every path"_s, operand, InternalErrorKind::UseNotDominated);
        if (resultRuleOf(definition.opcode) == ResultRule::None)
            return fail(block, user, "an operand has no result"_s, operand);
        if (holdsNoneOfItsResults(definition) && !mayNameProducerOfSeveralResults(record, operandIndex))
            return fail(block, user, "an operand gives several results and is taken for one value"_s, operand);
        if (m_isOwnedByEnd.quickGet(operand.index()) && operand != ownedValueItMayUse)
            return fail(block, user, "an operand is generated by the end of its block, for that end alone"_s, operand);
        if (hasOnlyLowestBitsOfInt32(definition) && !looksOnlyAtLowestBitsOfOperand(record, operandIndex))
            return fail(block, user, "an operand has only the lowest bits of an int32, and its user is no BitAnd with a mask of those"_s, operand);
        if (!readsListOfFlatEntries(record) && isListOfFlatEntries(operand))
            return fail(block, user, "an operand is a list of keys and values in a row, and its user is no read of the loop that it was made for"_s, operand);
        m_operandTypes.append(definition.type);
        m_operandForms.append(definition.representation);
    }
}

template<typename Record>
void CodeVerifier::verifyFormsOfOperands(BlockID block, ValueID id, const Record& record)
{
    if (!m_plan)
        return;
    RepresentationContext context { m_program, *m_plan, m_types, m_code.unit() };
    OperandForms forms { m_operandTypes.span(), m_operandForms.span() };
    for (uint32_t operandIndex = 0; operandIndex < record.numberOfOperands(); ++operandIndex) {
        if (!record.operand(operandIndex))
            continue;
        Representation wanted = wantedRepresentationOf(context, record, forms, operandIndex);
        if (hasRegister(wanted) && !isSameWord(m_operandForms[operandIndex], wanted))
            return fail(block, id, "an operand is not in the form in which the operation takes it"_s, record.operand(operandIndex), InternalErrorKind::WrongRepresentation);
    }
}

bool CodeVerifier::hasLegalForm(const Operation& record) const
{
    if (!record.type || record.representation == Representation::Unchosen)
        return false;
    if (!hasRegister(record.representation) || record.representation == Representation::RawWord)
        return true;
    return isLegalRepresentation(m_types.at(record.type), record.representation);
}

std::optional<uint32_t> CodeVerifier::numberOfResultsGivenBy(const Operation& producer) const
{
    if (producer.opcode != Opcode::CallDirect)
        return numberOfResultsOf(m_program, producer).value_or(1);
    if (!m_plan)
        return std::nullopt;
    const CallingConventions& conventions = m_plan->conventions();
    return conventions.numberOfResults(conventions.unitEnteredBy(producer));
}

bool CodeVerifier::holdsNoneOfItsResults(const Operation& producer) const
{
    if (!opcodesThatMayHaveSeveralResults.contains(producer.opcode))
        return false;
    if (numberOfResultsOf(m_program, producer))
        return true;
    if (producer.opcode != Opcode::CallDirect || !m_plan)
        return false;
    const CallingConventions& conventions = m_plan->conventions();
    CodeUnit entered = conventions.unitEnteredBy(producer);
    return conventions.exists(entered) && conventions.numberOfResults(entered) != 1;
}

EffectTraits CodeVerifier::boundOnTraitsOf(const Operation& operation)
{
    Opcode opcode = operation.opcode;
    EffectTraits bound(declaredBoundOf(opcode));
    if (testsItsOwnGuess(operation))
        bound |= EffectTraits(declaredBoundOf(*genericOperationOf(opcode)));
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        if (!protocolsOf(opcode, operandIndex).isEmpty() || operandUsesOf(opcode, operandIndex).containsAny({ OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly }))
            bound |= EffectTraits(Effects::unknownCode());
    }
    if (bound.mayCollect()) {
        Effects ofFailedAllocation = Effects::none();
        ofFailedAllocation.mayThrow = ThrowEffect::onlyForLackOfMemory();
        bound |= EffectTraits(ofFailedAllocation);
    }
    return bound;
}

void CodeVerifier::verifyOperation(BlockID block, uint32_t place, ValueID id, const Operation& operation)
{
    Opcode opcode = operation.opcode;
    if (opcodesThatStateTypes.contains(opcode))
        return fail(block, id, "the operation states a type"_s);
    if (opcodesWithoutEmitter().contains(opcode))
        return fail(block, id, "the operation has no emitter"_s);

    collectOperands(block, place, id, operation, opcode == Opcode::Narrowed ? operation.testOperand() : ValueID());
    if (hasFailed())
        return;
    if (opcode == Opcode::ResultAt) {
        std::optional<uint32_t> numberOfResults = numberOfResultsGivenBy(m_code.at(operation.resultsOperand()));
        if (numberOfResults && operation.index() >= *numberOfResults)
            return fail(block, id, "the producer has no such result"_s, operation.resultsOperand());
        m_operandTypes[OperandIndex::ResultAt::results] = operation.type;
    }
    if (std::optional<IllTypedOperand> illTyped = findIllTypedOperand(m_typing, operation, OperandTypes { m_operandTypes.span() }))
        return fail(block, id, nameOf(illTyped->error), operandAt(operation, illTyped->operandIndex), illTyped->error);
    verifyFormsOfOperands(block, id, operation);
    if (hasFailed())
        return;

    if (resultRuleOf(opcode) != ResultRule::None && !numberOfResultsOf(m_program, operation) && !hasLegalForm(operation))
        return fail(block, id, "the form of the record is none that its proven type allows"_s);

    EffectTraits bound = boundOnTraitsOf(operation);
    EffectTraits united = bound;
    united |= operation.traits();
    if (united != bound)
        return fail(block, id, "the traits of the record lie outside the bound that the schema declares"_s);

    if (branchesOf(opcode) == Branches::Only && !m_isOwnedByEnd.quickGet(id.index()))
        return fail(block, id, "a test that only branches is not the condition of the end of its block"_s);

    if (opcode == Opcode::FindsDeclaredMember && operation.receivers() && m_plan) {
        for (LayoutID member : membersOf(m_types, operation.receivers())) {
            MemberConditionRecord guard { MemberConditionKind::MemberUnchanged, declaredLayoutOf(m_program, member), ClassSide::Prototype, operation.key() };
            bool isListed = !m_plan->isInUse(member);
            for (MemberConditionRecord listed : operation.conditions())
                isListed |= listed == guard;
            if (!isListed)
                return fail(block, id, "a question about a declared member lists no guard of the class of one of its receivers"_s);
        }
    }

    if (opcode == Opcode::CallTable && m_plan && !isNeverType(m_code.at(operation.receiverOperand()).type)) {
        Type receiver = m_types.at(m_code.at(operation.receiverOperand()).type);
        if (receiver.primitives || !receiver.hasOnlyLayouts() || receiver.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
            return fail(block, id, "a call through the method table takes what may be no object of a layout"_s, operation.receiverOperand());
        for (LayoutID member : membersOf(m_types, receiver)) {
            if (m_plan->isInUse(member) && !m_plan->conventions().findRowOfMethodTable(operation.selector(), member))
                return fail(block, id, "a call through the method table takes a receiver that has no row"_s, operation.receiverOperand());
        }
    }

    if (opcode == Opcode::PutHeapFrameWord && !operation.remembersFrame() && m_types.at(m_code.at(operation.valueOperand()).type).hasCells())
        return fail(block, id, "what may be a cell is stored into a word of a frame in the heap, and the store does not remember the frame"_s, operation.valueOperand());

    if ((opcode == Opcode::GetArrayElement || opcode == Opcode::IsElementPresent) && operation.holdsDoublesOperand()) {
        const Operation& kind = m_code.at(operation.holdsDoublesOperand());
        if (kind.opcode != Opcode::HasArrayStorage || kind.arrayStorage() != ArrayStorage::Double || kind.admitsInt32Storage() || !areSameCells(kind.arrayOperand(), operation.arrayOperand()))
            return fail(block, id, "a read takes for the kind of its list what no question of that list for doubles gives"_s, operation.holdsDoublesOperand());
    }
    verifyPlainFieldWrites(block, id, operation);
    if (hasFailed())
        return;
    if (OperandPosition::storage(opcode))
        verifyPointerIntoStorage(block, id, operation.storageOperand(), Opcode::GetArrayStorage, operation.arrayOperand());
    else if (OperandPosition::vector(opcode))
        verifyPointerIntoStorage(block, id, operation.vectorOperand(), Opcode::GetTypedArrayVector, operation.viewOperand());
}

void CodeVerifier::verifyPlainFieldWrites(BlockID block, ValueID id, const Operation& operation)
{
    if (!m_facts || !m_plan)
        return;
    auto requirePlainField = [&](FieldReference field, ASCIILiteral rule) {
        FieldFacts facts = m_facts->field(field);
        if (hasFailed() || (facts.formOfValue.isEmpty() && !facts.facts.contains(FieldFact::IsAccessor)))
            return;
        fail(block, id, rule);
        Vector<char16_t> buffer;
        PropertyKeyID key = m_plan->slots().keyOf(m_program, field);
        m_failure = makeString(m_failure, ": L"_s, field.layout.index(), ".field"_s, field.field, " \""_s, key ? m_program.strings.viewOf(m_program.at(key).text, buffer) : StringView(), '"');
    };
    switch (operation.opcode) {
    case Opcode::NewObject:
        if (!operation.layout() || m_program.at(operation.layout()).isEnvironment())
            return;
        for (size_t place = 0; place < operation.fields().size(); ++place) {
            ValueID value = place < operation.valuesOperands().size() ? operation.valuesOperands()[place] : ValueID();
            if (!value || !m_types.isSubset(m_code.at(value).type, TypeTable::absent))
                requirePlainField({ operation.layout(), operation.fields()[place] }, "an allocation writes a field that only definitions may write"_s);
        }
        return;
    case Opcode::TryCopyFields: {
        static constexpr ASCIILiteral rule = "a copy of own keys writes a field that only definitions may write"_s;
        LayoutID source = operation.sourceLayout() ? operation.sourceLayout() : operation.layout();
        auto requirePlainFieldPair = [&](uint32_t fieldOfSource) {
            requirePlainField({ source, fieldOfSource }, rule);
            PropertyKeyID key = source != operation.layout() ? m_plan->slots().keyOf(m_program, { source, fieldOfSource }) : PropertyKeyID();
            if (std::optional<FieldIndex> field = key ? m_plan->slots().findField(m_program, operation.layout(), key) : std::nullopt)
                requirePlainField({ operation.layout(), *field }, rule);
        };
        for (uint32_t field : operation.fields())
            requirePlainFieldPair(field);
        for (uint32_t field = 0; operation.fields().empty() && field < m_plan->layout(source).slots.size(); ++field)
            requirePlainFieldPair(field);
        return;
    }
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        for (LayoutID member : membersOf(m_types, operation.receivers())) {
            if (std::optional<FieldIndex> field = m_plan->slots().findField(m_program, member, operation.key()))
                requirePlainField({ member, *field }, "a store writes a field that only definitions may write"_s);
        }
        return;
    default:
        return;
    }
}

ValueID CodeVerifier::underlyingCell(ValueID id) const
{
    while (cellPreservingOpcodes.contains(m_code.at(id).opcode))
        id = m_code.at(id).operand(0);
    return id;
}

bool CodeVerifier::isListOfFlatEntries(ValueID id) const
{
    const BuiltinTable& table = m_program.builtins();
    const Operation& made = m_code.at(underlyingCell(id));
    return made.opcode == Opcode::CallBuiltin && table.contains(Builtins::CompilerHelpersOwnEntriesFlat) && table.at(made.overload()).builtin == table.canonicalID(Builtins::CompilerHelpersOwnEntriesFlat);
}

void CodeVerifier::collectUnderlyingCells(ValueID name, Vector<ValueID, 4>& cells) const
{
    Vector<ValueID, 8> waiting { name };
    Vector<ValueID, 8> parametersSeen;
    while (!waiting.isEmpty()) {
        ValueID cell = underlyingCell(waiting.takeLast());
        std::span<const EdgeID> incoming;
        size_t index = 0;
        if (m_code.at(cell).opcode == Opcode::Parameter && !m_code.at(m_code.blockOf(cell)).isEntry) {
            std::span<const ValueID> parameters = m_code.at(m_code.blockOf(cell)).parameters();
            index = std::ranges::find(parameters, cell) - parameters.begin();
            incoming = incomingEdgesOf(m_code.blockOf(cell));
        }
        bool isFilledByArguments = !incoming.empty() && std::ranges::all_of(incoming, [&](EdgeID edge) { return index < m_code.at(edge).arguments().size(); });
        if (!isFilledByArguments) {
            if (!cells.contains(cell))
                cells.append(cell);
            continue;
        }
        if (parametersSeen.contains(cell))
            continue;
        parametersSeen.append(cell);
        for (EdgeID edge : incoming)
            waiting.append(m_code.at(edge).arguments()[index]);
    }
    std::ranges::sort(cells, { }, &ValueID::index);
}

void CodeVerifier::verifyPointerIntoStorage(BlockID block, ValueID access, ValueID pointer, Opcode readOfPointer, ValueID owner)
{
    if (!pointer)
        return;
    const Operation& read = m_code.at(pointer);
    if (read.opcode != readOfPointer)
        return fail(block, access, "an access takes for a pointer into storage what no read of such a pointer gives"_s, pointer);
    if (!areSameCells(read.operand(*baseOf(readOfPointer)), owner))
        return fail(block, access, "an access takes a pointer into the storage of another object"_s, pointer);
    if (readOfPointer != Opcode::GetArrayStorage)
        return;
    if (ValueID writer = findArrayWriteBetween(pointer, access))
        fail(block, access, "the storage of an array is written between the read of a pointer into it and an access through that pointer"_s, writer);
}

bool CodeVerifier::areSameCells(ValueID one, ValueID other) const
{
    if (underlyingCell(one) == underlyingCell(other))
        return true;
    Vector<ValueID, 4> underOne;
    Vector<ValueID, 4> underOther;
    collectUnderlyingCells(one, underOne);
    collectUnderlyingCells(other, underOther);
    return underOne == underOther;
}

bool CodeVerifier::mustAlias(ValueID one, ValueID other) const
{
    if (underlyingCell(one) == underlyingCell(other))
        return true;
    Vector<ValueID, 4> underOne;
    Vector<ValueID, 4> underOther;
    collectUnderlyingCells(one, underOne);
    collectUnderlyingCells(other, underOther);
    return underOne.size() == 1 && underOne == underOther;
}

ValueID CodeVerifier::findArrayWriteBetween(ValueID ground, ValueID access) const
{
    static constexpr AbstractHeapSet heapsOfLengths { AbstractHeap::ArrayStorage, AbstractHeap::ArrayLengths };
    const AbstractHeapSet heapsOfLists = m_code.at(ground).opcode == Opcode::GetArrayStorage ? heapsOfLengths | AbstractHeapSet { AbstractHeap::ArrayCapacity } : heapsOfLengths;
    static_assert(!declaredBoundOf(Opcode::ConvertToDoubleStorage).writes.contains(AbstractHeap::ArrayLengths));
    const bool isLength = m_code.at(ground).opcode == Opcode::GetArrayLength;
    BlockID home = m_code.blockOf(ground);
    BlockID block = m_code.blockOf(access);
    ValueID list = m_code.at(access).operand(0);
    auto writesLists = [&](const Operation& operation) {
        FunctionID callee = operation.opcode == Opcode::CallDirect && m_facts ? operation.function() : FunctionID();
        if (!callee)
            return declaredBoundOf(operation.opcode).writes.containsAny(heapsOfLists);
        const LinkedFunction& linked = m_facts->linked().function(callee);
        return linked.effectsOnReturn().writes.containsAny(heapsOfLists) || linked.effectsOnThrow().writes.containsAny(heapsOfLists);
    };
    auto findIn = [&](BlockID in, uint32_t after, uint32_t before) -> ValueID {
        for (ValueID each : operationsOf(in)) {
            uint32_t place = m_places[each.index()];
            const Operation& operation = m_code.at(each);
            if (place <= after || place >= before || operation.opcode == Opcode::ExtendArrayLength || (isLength && operation.opcode == Opcode::ConvertToDoubleStorage) || operation.traits().mayRunOtherCode() || !operation.traits().mayWriteObservableState())
                continue;
            if (writesLists(operation) && operation.numberOfOperands() && operation.operand(0) && areSameCells(operation.operand(0), list))
                return each;
        }
        return { };
    };
    if (home == block && m_places[ground.index()] < m_places[access.index()])
        return findIn(block, m_places[ground.index()], m_places[access.index()]);
    if (ValueID found = findIn(block, 0, m_places[access.index()]))
        return found;
    BitVector seen(m_code.numberOfBlocks());
    Vector<BlockID, 16> waiting;
    bool searchedBehindAccess = false;
    seen.set(block.index());
    waiting.append(block);
    while (!waiting.isEmpty()) {
        BlockID to = waiting.takeLast();
        for (EdgeID edge = m_code.at(to).firstEdgeIn(); edge; edge = m_code.at(edge).nextInto()) {
            BlockID from = m_code.at(edge).from;
            if (from == block && from != home && !std::exchange(searchedBehindAccess, true)) {
                if (ValueID found = findIn(block, m_places[access.index()], UINT32_MAX))
                    return found;
            }
            if (seen.set(from.index()))
                continue;
            if (ValueID found = findIn(from, from == home ? m_places[ground.index()] : 0, UINT32_MAX))
                return found;
            if (from != home)
                waiting.append(from);
        }
    }
    return { };
}

size_t CodeVerifier::numberOfWrittenCasesOf(const Terminator& end)
{
    switch (end.kind) {
    case TerminatorKind::Switch:
        return end.cases().size();
    case TerminatorKind::SwitchString:
        return end.strings().size();
    case TerminatorKind::SwitchOnShape:
        return end.layoutSets().size();
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CodeVerifier::verifyReturn(BlockID block, const Terminator& end)
{
    ValueID returned = end.valueOperand();
    if (!m_plan || !returned || m_code.at(returned).opcode != Opcode::Results)
        return;
    const CallingConventions& conventions = m_plan->conventions();
    CodeUnit unit = m_code.unit();
    std::span<const ValueID> values = m_code.at(returned).valuesOperands();
    if (values.size() != conventions.numberOfResults(unit))
        return fail(block, returned, "the unit returns another number of results than its convention has"_s);
    for (uint32_t index = 0; index < values.size(); ++index) {
        Representation planned = conventions.resultRepresentation(unit, index);
        if (values[index] && planned == Representation::Void)
            continue;
        if (!values[index] || !isSameWord(m_code.at(values[index]).representation, planned))
            return fail(block, returned, "a result that the unit returns is not in the form that its convention gives it"_s, values[index]);
    }
}

void CodeVerifier::verifyArguments(BlockID from, const Edge& edge, uint32_t place, uint32_t numberOfDeclaredResults, bool isHandlerEdge)
{
    std::span<const ValueID> arguments = edge.arguments();
    std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
    if (parameters.size() != arguments.size() + numberOfDeclaredResults)
        return fail(from, { }, "the arguments of an edge and the results that its terminator declares do not fill the parameters of the target"_s);

    for (size_t index = 0; index < arguments.size(); ++index) {
        if (m_code.isSwept() && !m_code.isNeeded(parameters[index]))
            continue;
        ValueID argument = arguments[index];
        if (!(isHandlerEdge ? isDefinedWhereBlockMayThrow(argument, from) : isDefinedBefore(argument, from, place)))
            return fail(from, { }, "the definition of an argument of an edge does not stand in front of the edge on every path"_s, argument, InternalErrorKind::UseNotDominated);
        if (resultRuleOf(m_code.at(argument).opcode) == ResultRule::None || m_isOwnedByEnd.quickGet(argument.index()))
            return fail(from, { }, "an argument of an edge has no result to hand on"_s, argument);
        if (hasOnlyLowestBitsOfInt32(m_code.at(argument)))
            return fail(from, { }, "an argument of an edge has only the lowest bits of an int32, and a parameter does not say so"_s, argument);
        if (isListOfFlatEntries(argument))
            return fail(from, { }, "an argument of an edge is a list of keys and values in a row, and a parameter does not say so"_s, argument);
        Representation wanted = m_code.at(parameters[index]).representation;
        if (hasRegister(wanted) && !isSameWord(m_code.at(argument).representation, wanted))
            return fail(from, { }, "an argument of an edge is not in the form of its parameter"_s, argument);
    }
}

void CodeVerifier::verifyEnd(BlockID block, uint32_t place)
{
    const Block& contents = m_code.at(block);
    const Terminator& end = contents.terminator;

    collectOperands(block, place, { }, end, valueOwnedBy(end));
    if (hasFailed())
        return;
    if (std::optional<IllTypedOperand> illTyped = findIllTypedOperandOfTerminator(m_typing, end, OperandTypes { m_operandTypes.span() }, m_code.function()))
        return fail(block, { }, nameOf(illTyped->error), operandAt(end, illTyped->operandIndex), illTyped->error);
    verifyFormsOfOperands(block, { }, end);
    if (hasFailed())
        return;

    if (end.numberOfEdges != numberOfFixedTargetsOf(end.kind).value_or(numberOfWrittenCasesOf(end) + 1))
        return fail(block, { }, "the end of the block has another number of edges than its kind and its cases ask for"_s);

    switch (end.kind) {
    case TerminatorKind::Return:
        verifyReturn(block, end);
        break;
    case TerminatorKind::ReturnCall:
        if (m_code.handlerOf(block) || !handsOperandsToCallee(m_code.at(end.callOperand()).opcode))
            fail(block, end.callOperand(), "a tail call stands under a handler, or is no call"_s);
        break;
    case TerminatorKind::TestAndBranch:
        if (branchesOf(m_code.at(end.conditionOperand()).opcode) == Branches::No)
            fail(block, end.conditionOperand(), "the end of the block fuses with a condition that cannot branch"_s);
        break;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TryCallInline:
        break;
    }
    if (hasFailed())
        return;

    for (uint32_t targetIndex = 0; targetIndex < end.numberOfEdges; ++targetIndex) {
        const Edge& edge = m_code.at(end.edge(targetIndex));
        if (m_code.at(edge.to).isHandlerBlock())
            return fail(block, { }, "an edge that is no handler edge enters a handler block"_s);
        const TargetInfo* target = infoOfTarget(end.kind, targetIndex, end.numberOfEdges);
        verifyArguments(block, edge, place, target ? target->numberOfResults : 0, false);
        if (hasFailed())
            return;
    }

    if (EdgeID handler = m_code.handlerOf(block)) {
        if (!m_code.at(m_code.at(handler).to).isHandlerBlock())
            return fail(block, { }, "the handler edge of the block enters a block that is no handler block"_s);
        verifyArguments(block, m_code.at(handler), place, 0, true);
        return;
    }
    if (!m_code.isSwept() && contents.handler && isReached(m_code.at(contents.handler).to) && (m_placeOfFirstThrow[block.index()] < place || declaredBoundOf(end.kind).mayThrow))
        fail(block, { }, "the block stands under a handler, may throw, and has lost its handler edge"_s);
}

void CodeVerifier::verifyBlock(BlockID block)
{
    const Block& contents = m_code.at(block);
    for (ValueID parameter : contents.parameters()) {
        if (!hasLegalForm(m_code.at(parameter)))
            return fail(block, parameter, "the form of the record is none that its proven type allows"_s);
    }

    bool standsAtHead = true;
    bool followsOnlyCatches = true;
    uint32_t place = 0;
    for (ValueID id : operationsOf(block)) {
        const Operation& operation = m_code.at(id);
        if (operation.opcode != Opcode::Narrowed)
            standsAtHead &= operationsThatStandAmongNarrowedValues.contains(operation.opcode);
        else if (!standsAtHead && !m_code.isSwept())
            return fail(block, id, "a Narrowed stands behind another operation of its block"_s);
        if (familyOf(operation.opcode) != OperationFamily::Exception)
            followsOnlyCatches = false;
        else if (!followsOnlyCatches || !contents.isHandlerBlock())
            return fail(block, id, "what was thrown is read elsewhere than at the head of a handler block"_s, { }, InternalErrorKind::InvalidCatch);
        verifyOperation(block, ++place, id, operation);
        if (hasFailed())
            return;
    }
    verifyEnd(block, ++place);
}

void CodeVerifier::verifyRecords()
{
    for (uint32_t index = 0; index < m_code.numberOfValues(); ++index) {
        ValueID id(index);
        if (!m_code.isConstant(id))
            continue;
        verifyOperation({ }, 0, id, m_code.at(id));
        if (hasFailed())
            return;
    }
    for (BlockID block : m_reachedBlocks) {
        verifyBlock(block);
        if (hasFailed())
            return;
    }
}

ValueID CodeVerifier::valueBeforeConversions(ValueID id) const
{
    for (;;) {
        const Operation& record = m_code.at(id);
        if (!valuePreservingOpcodes.contains(record.opcode))
            return id;
        id = record.valueOperand();
    }
}

ValueID CodeVerifier::holderOfSlotRead(ValueID id) const
{
    const Operation& read = m_code.at(id);
    if (!readsThatNarrowTheirHolders.contains(read.opcode))
        return { };
    return valueBeforeConversions(read.objectOperand());
}

void CodeVerifier::verifyNarrowed(BlockID block, ValueID id, const Operation& narrowed)
{
    std::span<const EdgeID> incoming = incomingEdgesOf(block);
    if (incoming.size() != 1 || m_code.at(block).isEntry)
        return fail(block, id, "a Narrowed stands in a block that has not exactly one incoming edge"_s);
    BlockID from = m_code.at(incoming[0]).from;
    if (m_code.handlerOf(from) == incoming[0])
        return fail(block, id, "a Narrowed stands behind a handler edge"_s);
    const Terminator& end = m_code.at(from).terminator;
    uint32_t targetIndex = incoming[0].index() - end.firstEdge.index();
    ValueID value = valueBeforeConversions(narrowed.valueOperand());

    Representation formOfValue = m_code.at(narrowed.valueOperand()).representation;
    if (hasRegister(narrowed.representation) && !findConversion(m_types, narrowed.type, formOfValue, narrowed.representation))
        return fail(block, id, "no conversion leads from the form of the value of a Narrowed to the form of the Narrowed"_s, narrowed.valueOperand());

    ValueID test = narrowed.testOperand();
    if (!test) {
        ValueID subject = end.hasCases() ? valueBeforeConversions(end.subjectOperand()) : ValueID();
        if (subject && narrowed.narrowsHolderOfOperand())
            subject = end.kind == TerminatorKind::SwitchOnShape ? ValueID() : holderOfSlotRead(subject);
        if (subject != value)
            return fail(block, id, "a Narrowed without a test does not stand behind a switch on its value, or on a slot of it"_s);
        if (targetIndex != narrowed.caseIndex().value_or(end.numberOfCases()))
            return fail(block, id, "a Narrowed names another case than the edge that leads to it"_s);
        return;
    }

    if (!end.isConditionalBranch())
        return fail(block, id, "a Narrowed does not stand behind a branch on its test"_s, test);
    bool testIsTrue = !targetIndex;
    for (ValueID condition = end.conditionOperand(); condition != test; condition = m_code.at(condition).valueOperand()) {
        Opcode opcode = m_code.at(condition).opcode;
        if (opcode == Opcode::LogicalNot)
            testIsTrue = !testIsTrue;
        else if (opcode != Opcode::ToBoolean && !conversionsOfForm.contains(opcode))
            return fail(block, id, "a Narrowed does not stand behind a branch on its test"_s, test);
    }
    if (testIsTrue != narrowed.isOnTakenEdge())
        return fail(block, id, "a Narrowed stands on the side of the branch on which its test has the other outcome"_s, test);

    if (narrowed.narrowsOtherResultOfOperand()) {
        ValueID producer = operandAt(m_code.at(test), narrowed.operandIndex());
        bool readOneProducer = producer && m_code.at(test).opcode == Opcode::ResultAt && m_code.at(value).opcode == Opcode::ResultAt && m_code.at(value).resultsOperand() == producer;
        if (!readOneProducer || value == test || narrowed.narrowsHolderOfOperand())
            return fail(block, id, "the value of a Narrowed is no other result of the operation that its test reads a result of"_s, test);
        return;
    }
    if (narrowed.narrowsWithCounter()) {
        ValueID counter = m_code.at(test).numberOfOperands() == 2 ? valueBeforeConversions(m_code.at(test).operand(narrowed.operandIndex() & 1)) : ValueID();
        if (!counter || m_code.at(counter).opcode != Opcode::Parameter || m_code.at(value).opcode != Opcode::Parameter || m_code.blockOf(counter) != m_code.blockOf(value))
            return fail(block, id, "a Narrowed is bounded beside what is no counter of its head"_s, test);
        return;
    }
    ValueID tested = valueThatTestSpeaksOf(m_code, test, narrowed.operandIndex());
    if (tested)
        tested = valueBeforeConversions(tested);
    if (tested && narrowed.narrowsHolderOfOperand())
        tested = holderOfSlotRead(tested);
    if (tested && narrowed.narrowsTermOfOperand()) {
        const Operation& sum = m_code.at(tested);
        bool isTerm = sum.opcode == Opcode::Add && !narrowed.narrowsHolderOfOperand() && (valueBeforeConversions(sum.operand(0)) == value) != (valueBeforeConversions(sum.operand(1)) == value);
        if (!isTerm)
            return fail(block, id, "the value of a Narrowed is no term of the sum that its test has looked at"_s, test);
        return;
    }
    if (tested != value)
        return fail(block, id, "the value of a Narrowed is not what its test has looked at"_s, test);
}

void CodeVerifier::verifyNarrowedValues()
{
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            if (operationsThatStandAmongNarrowedValues.contains(m_code.at(id).opcode))
                continue;
            if (m_code.at(id).opcode != Opcode::Narrowed) {
                if (m_code.isSwept())
                    continue;
                break;
            }
            verifyNarrowed(block, id, m_code.at(id));
            if (hasFailed())
                return;
        }
    }
}

static constexpr OpcodeSet arithmeticOnTwoNumbersInCodeVerifier { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };

bool CodeVerifier::isOperatorSpecializationCheck(ValueID test) const
{
    const Operation& asked = m_code.at(test);
    BitVector names(m_code.numberOfValues());
    bool isUsedOnPass = false;
    bool isUsedOnFailure = false;
    for (bool someNameIsNew = true; std::exchange(someNameIsNew, false);) {
        for (BlockID block : m_reachedBlocks) {
            for (ValueID id : operationsOf(block)) {
                const Operation& record = m_code.at(id);
                bool isName = record.opcode == Opcode::Narrowed ? record.testOperand() == test || names.get(record.valueOperand().index()) : valuePreservingOpcodes.contains(record.opcode) && names.get(record.operand(0).index());
                if (isName && !names.set(id.index()))
                    someNameIsNew = true;
            }
        }
    }
    auto isOnPassingEdge = [&](ValueID name) {
        while (m_code.at(name).opcode != Opcode::Narrowed || m_code.at(name).testOperand() != test)
            name = m_code.at(name).operand(0);
        return m_code.at(name).isOnTakenEdge();
    };
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            const Operation& user = m_code.at(id);
            for (uint32_t operandIndex = 0; operandIndex < user.numberOfOperands(); ++operandIndex) {
                if (user.operand(operandIndex) == test && (user.opcode != Opcode::Narrowed || user.valueOperand() == test))
                    return false;
                if (!user.operand(operandIndex) || !names.get(user.operand(operandIndex).index()) || names.get(id.index()))
                    continue;
                if (user.position != asked.position || user.inlineFrame != asked.inlineFrame)
                    return false;
                (isOnPassingEdge(user.operand(operandIndex)) ? isUsedOnPass : isUsedOnFailure) = true;
            }
        }
        const Terminator& end = m_code.at(block).terminator;
        for (uint32_t operandIndex = 0; operandIndex < end.numberOfOperands(); ++operandIndex) {
            if (end.operand(operandIndex) == test && (!end.isConditionalBranch() || end.conditionOperand() != test))
                return false;
        }
        bool isEdgeArgument = false;
        m_code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
            isEdgeArgument |= std::ranges::contains(edge.arguments(), test);
        });
        if (isEdgeArgument)
            return false;
    }
    return isUsedOnPass && isUsedOnFailure;
}

bool CodeVerifier::treatsUndefinedAsNaN(ValueID id, const Operation& user, uint32_t operandIndex) const
{
    if (user.opcode == Opcode::Add) {
        if (operandIndex >= 2)
            return false;
        const Type& other = m_types.at(m_code.at(user.operand(1 - operandIndex)).type);
        return other.primitives & Primitive::Number && other.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined);
    }
    if (arithmeticOnTwoNumbersInCodeVerifier.contains(user.opcode) || allRelations.contains(user.opcode))
        return operandIndex < 2;
    if (user.opcode == Opcode::ToInt32 || user.opcode == Opcode::ToUint32 || user.opcode == Opcode::ToNumber || user.opcode == Opcode::IntegerBound || allBitwiseOperators.contains(user.opcode))
        return true;
    if (user.opcode == Opcode::TypeTest) {
        const Type& tested = m_types.at(user.testedType());
        if (tested.mayBeNaN() == !!(tested.primitives & Primitive::Undefined))
            return true;
        return isOperatorSpecializationCheck(id);
    }
    return user.opcode == Opcode::PutTypedArrayElement && user.valueOperand() == user.operand(operandIndex) && user.viewOperand() != user.operand(operandIndex) && user.positionOperand() != user.operand(operandIndex) && !isBigInt(user.typedArrayType());
}

void CodeVerifier::verifyUsersOfReadNumbers()
{
    Vector<ValueID, 8> numbers;
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            const Operation& read = m_code.at(id);
            if (read.opcode == Opcode::NumberOrNaN && !m_types.at(m_code.at(read.valueOperand()).type).hasOnlyPrimitives(Primitive::Number | Primitive::Undefined | Primitive::Absent))
                numbers.append(id);
        }
    }
    if (numbers.isEmpty())
        return;
    Vector<ValueID, 8> tests;
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            const Operation& test = m_code.at(id);
            if (test.opcode == Opcode::StrictEq && test.leftOperand() == test.rightOperand() && numbers.contains(test.leftOperand()))
                tests.append(id);
        }
    }
    Vector<std::pair<ValueID, ValueID>, 8> computed;
    auto numberBehind = [&](ValueID value) -> ValueID {
        bool isNumberByType = false;
        for (; valuePreservingOpcodes.contains(m_code.at(value).opcode); value = m_code.at(value).operand(0))
            isNumberByType |= m_code.at(value).opcode == Opcode::Narrowed && !m_types.at(m_code.at(value).type).mayBeNaN();
        if (numbers.contains(value))
            return isNumberByType ? ValueID() : value;
        size_t place = computed.findIf([&](const auto& entry) { return entry.first == value; });
        return place == notFound ? ValueID() : computed[place].second;
    };
    auto isTakenUntested = [&](ValueID operand, BlockID block, uint32_t place, EdgeID edge) -> bool {
        ValueID number = operand ? numberBehind(operand) : ValueID();
        return number && std::ranges::none_of(tests, [&](ValueID test) {
            return m_code.at(test).leftOperand() == number && ((edge && findAnswerEdge(test, true, block) == edge) || isAnsweredAt(test, true, block, place));
        });
    };
    auto isExempt = [&](const Operation& user) {
        bool isTestWithoutNaN = user.opcode == Opcode::TypeTest && !m_types.at(user.testedType()).mayBeNaN();
        return valuePreservingOpcodes.contains(user.opcode) || user.opcode == Opcode::StrictEq || user.opcode == Opcode::CheckProof || isTestWithoutNaN;
    };
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        for (BlockID block : m_reachedBlocks) {
            for (ValueID id : operationsOf(block)) {
                const Operation& user = m_code.at(id);
                if (!user.traits().isPure() || isExempt(user) || computed.containsIf([&](const auto& entry) { return entry.first == id; }))
                    continue;
                for (uint32_t operandIndex = 0; operandIndex < user.numberOfOperands(); ++operandIndex) {
                    if (!isTakenUntested(user.operand(operandIndex), block, m_places[id.index()], { }))
                        continue;
                    computed.append({ id, numberBehind(user.operand(operandIndex)) });
                    hasChanged = true;
                    break;
                }
            }
        }
    }
    constexpr ASCIILiteral rule = "a number is read out of a value and taken where it may be NaN for what is no number"_s;
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            const Operation& user = m_code.at(id);
            if (user.traits().isPure() || isExempt(user))
                continue;
            for (uint32_t operandIndex = 0; operandIndex < user.numberOfOperands(); ++operandIndex) {
                if (isTakenUntested(user.operand(operandIndex), block, m_places[id.index()], { }))
                    return fail(block, id, rule, user.operand(operandIndex));
            }
        }
        uint32_t placeOfEnd = operationsOf(block).size() + 1;
        const Terminator& end = m_code.at(block).terminator;
        for (uint32_t operandIndex = 0; operandIndex < end.numberOfOperands(); ++operandIndex) {
            if (isTakenUntested(end.operand(operandIndex), block, placeOfEnd, { }))
                return fail(block, end.operand(operandIndex), rule);
        }
        ValueID passedOn;
        m_code.forEachEdgeOf(block, [&](EdgeID id, const Edge& edge) {
            for (ValueID argument : edge.arguments()) {
                if (isTakenUntested(argument, block, placeOfEnd, id))
                    passedOn = argument;
            }
        });
        if (passedOn)
            return fail(block, passedOn, rule);
    }
}

void CodeVerifier::verifyUsersOfHolesReadAsNaN()
{
    bool someHoleIsReadAsNaN = false;
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block))
            someHoleIsReadAsNaN |= m_code.at(id).opcode == Opcode::GetArrayElement && m_code.at(id).readsHoleAsNaN();
    }
    if (!someHoleIsReadAsNaN)
        return;
    auto programOnlyReckonsWith = [](const BodyIndex& index, ValueID name) {
        ValueID user = index.onlyUsingValue(name);
        for (; user && opcodesThatStateTypes.contains(index.opcodeOf(user)); user = index.onlyUsingValue(user))
            name = user;
        auto reckons = [&](ValueID user) {
            bool isStored = index.opcodeOf(user) == Opcode::ElemSet && index.value(user).valueOperand() == name && index.value(user).objectOperand() != name && index.value(user).propertyOperand() != name;
            return isStored || arithmeticOnTwoNumbersInCodeVerifier.contains(index.opcodeOf(user)) || (allBitwiseOperators | allRelations).contains(index.opcodeOf(user));
        };
        if (user)
            return reckons(user);
        uint32_t usesLeft = index.useCount(name);
        if (usesLeft == 1 || usesLeft >= BodyIndex::maxCountedUses)
            return false;
        for (BodyValue operation : index.block(index.blockOf(name)).values()) {
            uint32_t usesHere = 0;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                usesHere += operand == name;
            });
            if (!usesHere)
                continue;
            if (!reckons(operation.id()))
                return false;
            usesLeft -= usesHere;
        }
        return !usesLeft;
    };
    auto programOnlyTakesBits = [](const BodyIndex& index, ValueID written) {
        static constexpr size_t maxNames = 16;
        Vector<ValueID, maxNames> names { written };
        bool onlyTakesBits = true;
        for (size_t place = 0; place < names.size() && onlyTakesBits; ++place) {
            ValueID name = names[place];
            uint32_t usesLeft = index.useCount(name);
            auto follow = [&](ValueID next) {
                if (next && names.contains(next))
                    return;
                onlyTakesBits &= next && names.size() < maxNames;
                if (onlyTakesBits)
                    names.append(next);
            };
            auto visitOperation = [&](const BodyValue& user) {
                user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    if (operand != name)
                        return;
                    --usesLeft;
                    if (!allBitwiseOperators.contains(user.opcode()))
                        follow(opcodesThatStateTypes.contains(user.opcode()) && !operandIndex ? user.id() : ValueID());
                });
            };
            auto visitEnd = [&](BlockID from) {
                BodyBlock block = index.block(from);
                block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
                    onlyTakesBits &= operand != name;
                });
                block.forEachOutgoingEdge([&](BodyTarget target, bool) {
                    BodyBlock join = index.block(target.block());
                    for (uint32_t position = 0; position < target.arguments().size(); ++position) {
                        if (target.arguments()[position] != name)
                            continue;
                        --usesLeft;
                        follow(position < join.numberOfParameters() && !index.isLoopHead(join.id()) ? join.parameter(position) : ValueID());
                    }
                });
                index.forEachEdgeResult(from, [&](const BodyEdgeResult& result) {
                    onlyTakesBits &= result.subject != name;
                });
                onlyTakesBits &= block.subject() != name;
            };
            if (std::optional<BodyUser> user = index.onlyUser(name)) {
                if (user->kind == BodyUseKind::Operand)
                    visitOperation(index.value(user->value));
                else
                    visitEnd(user->block);
            } else if (usesLeft && usesLeft < BodyIndex::maxCountedUses) {
                BodyBlock block = index.block(index.blockOf(name));
                for (BodyValue user : block.values())
                    visitOperation(user);
                visitEnd(block.id());
            }
            onlyTakesBits &= !usesLeft;
        }
        return onlyTakesBits;
    };
    auto callerComputesWith = [&](const BodyIndex& index, ValueID written, ValueID read) {
        for (ValueID user = index.onlyUsingValue(written); user && opcodesThatStateTypes.contains(index.opcodeOf(user)); user = index.onlyUsingValue(written))
            written = user;
        BodyTerminator end = index.block(index.blockOf(written)).terminator();
        if (!index.hasOneUse(written) || end.kind() != TerminatorKind::Return || end.operand(0) != written)
            return false;
        return std::ranges::any_of(m_code.readsAnsweredByHolesReadAsNaN(), [&](const Code::InputOfHoleReadAsNaN& taken) {
            const BodyIndex* caller = taken.read == read ? m_facts->bodies().find(taken.function) : nullptr;
            return caller && !caller->isParameter(taken.written) && caller->opcodeOf(taken.written) == Opcode::Call && programOnlyReckonsWith(*caller, taken.written);
        });
    };
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            if (m_code.at(id).opcode != Opcode::GetArrayElement || !m_code.at(id).readsHoleAsNaN() || !m_facts)
                continue;
            std::span<const Code::InputOfHoleReadAsNaN> noted = m_code.inputsOfHolesReadAsNaN();
            auto found = std::ranges::lower_bound(noted, id.index(), { }, [](const Code::InputOfHoleReadAsNaN& input) { return input.read.index(); });
            const BodyIndex* index = found != noted.end() && found->read == id && found->written ? m_facts->bodies().find(found->function) : nullptr;
            if (!index)
                return fail(block, id, "a hole is read as NaN and the read names no value of the program"_s);
            if (!programOnlyReckonsWith(*index, found->written) && !programOnlyTakesBits(*index, found->written) && !callerComputesWith(*index, found->written, id))
                return fail(block, id, "a hole is read as NaN and the program does more with the value than arithmetic"_s);
            for (const Code::InputOfHoleReadAsNaN& answered : m_code.readsAnsweredByHolesReadAsNaN()) {
                const BodyIndex* indexOfAnswered = answered.read == id ? m_facts->bodies().find(answered.function) : nullptr;
                if (answered.read == id && (!indexOfAnswered || (!programOnlyReckonsWith(*indexOfAnswered, answered.written) && !programOnlyTakesBits(*indexOfAnswered, answered.written) && !callerComputesWith(*indexOfAnswered, answered.written, id))))
                    return fail(block, id, "a hole is read as NaN and the program does more with the value than arithmetic"_s);
            }
        }
    }
    Vector<ValueID> readsThatArriveInParameters(m_code.numberOfValues());
    BitVector standsForFoldedArithmetic(m_code.numberOfValues());
    for (ValueID given : m_code.valuesThatFoldedArithmeticGives())
        standsForFoldedArithmetic.set(given.index());
    auto readBehind = [&](ValueID value) {
        for (;;) {
            if (standsForFoldedArithmetic.get(value.index()))
                return ValueID();
            if (ValueID arrives = readsThatArriveInParameters[value.index()])
                return arrives;
            const Operation& record = m_code.at(value);
            if (record.opcode == Opcode::Narrowed && !m_types.at(record.type).mayBeNaN() && !(m_types.at(record.type).primitives & Primitive::Undefined))
                return ValueID();
            const Operation* laidTogether = record.opcode == Opcode::ResultAt && m_code.at(record.resultsOperand()).opcode == Opcode::Results ? &m_code.at(record.resultsOperand()) : nullptr;
            if (valuePreservingOpcodes.contains(record.opcode))
                value = record.operand(0);
            else if (laidTogether && record.index() < laidTogether->numberOfOperands() && laidTogether->operand(record.index()))
                value = laidTogether->operand(record.index());
            else
                return record.opcode == Opcode::GetArrayElement && record.readsHoleAsNaN() ? value : ValueID();
        }
    };
    auto readLaidInto = [&](ValueID value) {
        const Operation& record = m_code.at(value);
        for (uint32_t operandIndex = 0; record.opcode == Opcode::Results && operandIndex < record.numberOfOperands(); ++operandIndex) {
            if (ValueID read = record.operand(operandIndex) ? readBehind(record.operand(operandIndex)) : ValueID())
                return read;
        }
        return ValueID();
    };
    for (bool someReadArrivesFurther = true; std::exchange(someReadArrivesFurther, false);) {
        for (BlockID block : m_reachedBlocks) {
            m_code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
                std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
                std::span<const ValueID> arguments = edge.arguments();
                for (size_t index = 0; index < std::min(arguments.size(), parameters.size()); ++index) {
                    if (!arguments[index] || readsThatArriveInParameters[parameters[index].index()])
                        continue;
                    if (ValueID read = readBehind(arguments[index])) {
                        readsThatArriveInParameters[parameters[index].index()] = read;
                        someReadArrivesFurther = true;
                    }
                }
            });
        }
    }
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            const Operation& user = m_code.at(id);
            if (valuePreservingOpcodes.contains(user.opcode) || user.opcode == Opcode::CheckProof || user.opcode == Opcode::Results)
                continue;
            for (uint32_t operandIndex = 0; operandIndex < user.numberOfOperands(); ++operandIndex) {
                ValueID read = user.operand(operandIndex) ? readBehind(user.operand(operandIndex)) : ValueID();
                if (read && !treatsUndefinedAsNaN(id, user, operandIndex))
                    return fail(block, id, "a hole is read as NaN for an operation that tells NaN from undefined"_s, read);
                ValueID laid = user.operand(operandIndex) && user.opcode != Opcode::ResultAt ? readLaidInto(user.operand(operandIndex)) : ValueID();
                if (laid)
                    return fail(block, id, "a hole is read as NaN for an operation that tells NaN from undefined"_s, laid);
            }
        }
        const Terminator& end = m_code.at(block).terminator;
        for (uint32_t operandIndex = 0; operandIndex < end.numberOfOperands(); ++operandIndex) {
            ValueID read = end.operand(operandIndex) ? readBehind(end.operand(operandIndex)) : ValueID();
            if (!read && end.operand(operandIndex))
                read = readLaidInto(end.operand(operandIndex));
            if (read)
                return fail(block, read, "a hole is read as NaN for the end of a block"_s, read);
        }
        bool leaves = false;
        ValueID left;
        m_code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
            std::span<const ValueID> arguments = edge.arguments();
            for (size_t index = 0; index < arguments.size(); ++index) {
                ValueID read = !leaves && arguments[index] ? readLaidInto(arguments[index]) : ValueID();
                if (!read && !leaves && arguments[index] && index >= m_code.at(edge.to).parameters().size())
                    read = readBehind(arguments[index]);
                if (read) {
                    leaves = true;
                    left = read;
                }
            }
        });
        if (leaves)
            return fail(block, left, "a hole is read as NaN for what an edge hands on and no parameter takes"_s, left);
    }
}

bool CodeVerifier::handsOperandsToCallee(Opcode opcode)
{
    switch (effectRuleOf(opcode)) {
    case EffectRule::Call:
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return true;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
        break;
    }
    switch (accessClassOf(opcode)) {
    case AccessClass::DirectCall:
    case AccessClass::IndirectCall:
        return true;
    case AccessClass::None:
    case AccessClass::Slot:
    case AccessClass::ByName:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID CodeVerifier::operandHandedBackBy(const Operation& operation)
{
    if (handsOperandsToCallee(operation.opcode) || resultRuleOf(operation.opcode) == ResultRule::Unknown)
        return { };
    ValueID handedBack;
    for (uint32_t operandIndex = 0; operandIndex < operation.numberOfNamedOperands(); ++operandIndex) {
        if (!operation.operandUses(operandIndex).contains(OperandUse::Returns))
            continue;
        if (handedBack)
            return { };
        handedBack = operation.operand(operandIndex);
    }
    return handedBack;
}

ValueID CodeVerifier::cellBeforeForwarding(ValueID id) const
{
    while (ValueID handedBack = operandHandedBackBy(m_code.at(id)))
        id = handedBack;
    return id;
}

static constexpr OperandUses usesThatTakeNoFramePointerInCodeVerifier {
    OperandUse::Inspects, OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::ReadsContents, OperandUse::WritesContents,
    OperandUse::RetainsContents, OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName,
    OperandUse::TakesDescriptor, OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype, OperandUse::PassesToUserCode,
    OperandUse::AppliesTransitively, OperandUse::AppliesToContents,
};
static_assert(everyOperandUseIsInExactlyOneOf({ OperandUse::Retains, OperandUse::Returns, usesThatTakeNoFramePointerInCodeVerifier }));

bool CodeVerifier::handsBackPointerIntoFrame(const Operation& operation) const
{
    if (handsOperandsToCallee(operation.opcode) || (hasRegister(operation.representation) && !holdsCells(operation.representation)))
        return false;
    for (uint32_t operandIndex = 0; operandIndex < operation.numberOfOperands(); ++operandIndex) {
        ValueID operand = operation.operand(operandIndex);
        if (operand && mayPointIntoFrame(operand) && operation.operandUses(operandIndex).contains(OperandUse::Returns))
            return true;
    }
    return false;
}

bool CodeVerifier::loanCoversOperandOfCall(const Operation& call, uint32_t operandIndex, ValueID operand) const
{
    const Operation& made = m_code.at(cellBeforeForwarding(operand));
    if (made.opcode == Opcode::NewClosure)
        return made.loan() != LoanOfClosure::None;
    uint32_t numberOfNamedOperands = call.numberOfNamedOperands();
    if (!nothingButOwnFieldsAnswersFor(m_program, made) || call.opcode != Opcode::CallDirect || operandIndex < numberOfNamedOperands || !returnsResultOfFunction(call.role()) || call.numberOfBoundArguments())
        return false;
    if (std::ranges::any_of(made.valuesOperands(), [&](ValueID value) { return value && mayPointIntoFrame(value); }))
        return false;
    if (m_plan && !objectOfLayoutMayLieInFrame(m_plan->layout(made.layout())))
        return false;
    return m_facts && m_facts->linked().onlyLooksAtWhatArrivesAt(call.function(), operandIndex - numberOfNamedOperands);
}

const DataNode::Array* CodeVerifier::findWrittenDataShownOf(ValueID cell, BlockID block)
{
    if (!m_testsForWrittenData) {
        m_testsForWrittenData.emplace();
        for (BlockID each : m_reachedBlocks) {
            const Terminator& end = m_code.at(each).terminator;
            ValueID test = end.isConditionalBranch() ? end.conditionOperand() : ValueID();
            if (test && !m_code.isConstant(test) && m_code.at(test).opcode == Opcode::HasArrayStorage && m_code.at(test).data())
                m_testsForWrittenData->append(test);
        }
    }
    ValueID asked = underlyingCell(cell);
    for (ValueID test : *m_testsForWrittenData) {
        if (underlyingCell(m_code.at(test).arrayOperand()) != asked)
            continue;
        if (isAnsweredAt(test, true, block, 0))
            return m_program.at(m_code.at(test).data()).root.tryAs<DataNode::Array>();
    }
    return nullptr;
}

auto CodeVerifier::listsMadeUnder(ValueID name, BlockID block) -> std::optional<ListsMade>
{
    if (const auto* written = findWrittenDataShownOf(name, block)) {
        return ListsMade { written->elements.size(), std::ranges::none_of(written->elements, [](const DataNode& element) {
            return element.is<DataNode::Hole>();
        }) };
    }
    Vector<ValueID, 4> cells;
    collectUnderlyingCells(name, cells);
    std::optional<ListsMade> lists;
    for (ValueID cell : cells) {
        const Operation& made = m_code.at(cell);
        const auto* written = made.opcode == Opcode::Data && made.encoding() == DataEncoding::CopyOnWriteArray && made.data() ? m_program.at(made.data()).root.tryAs<DataNode::Array>() : nullptr;
        if (!written && made.opcode != Opcode::NewArray)
            return std::nullopt;
        uint64_t length = written ? written->elements.size() : made.elementsOperands().size() + made.holes().size();
        bool hasHole = written ? std::ranges::any_of(written->elements, [](const DataNode& element) { return element.is<DataNode::Hole>(); }) : !made.holes().empty();
        lists = ListsMade { std::min(lists ? lists->leastLength : length, length), (!lists || lists->areWhole) && !hasHole };
    }
    return lists;
}

bool CodeVerifier::isLengthOfView(ValueID length, ValueID view, BlockID block)
{
    length = underlyingCell(length);
    if (m_code.at(length).opcode == Opcode::GetTypedArrayLength && areSameCells(m_code.at(length).viewOperand(), view))
        return true;
    if (m_code.at(length).opcode == Opcode::GetArrayLength && areSameCells(m_code.at(length).arrayOperand(), view))
        return true;
    if (std::optional<ListsMade> made = listsMadeUnder(view, block))
        return m_code.at(length).opcode == Opcode::NewArray || (m_code.at(length).opcode == Opcode::Integer && m_code.at(length).integer() >= 0 && static_cast<uint64_t>(m_code.at(length).integer()) <= made->leastLength);
    const Operation& maker = m_code.at(underlyingCell(view));
    bool isListBornWithLength = maker.opcode == Opcode::NewArrayLike;
    if (!isListBornWithLength && (maker.opcode != Opcode::CallInline || maker.inlineBuiltin() != InlineBuiltin::TypedArrayWithLength || !maker.numberOfOperands()))
        return false;
    ValueID given = valueBeforeConversions(isListBornWithLength ? maker.lengthOperand() : maker.operand(maker.numberOfOperands() - 1));
    ValueID counted = valueBeforeConversions(length);
    return given == counted || (m_code.at(given).opcode == Opcode::Integer && m_code.at(counted).opcode == Opcode::Integer && m_code.at(given).integer() == m_code.at(counted).integer());
}

EdgeID CodeVerifier::findAnswerEdge(ValueID test, bool testResult, BlockID from) const
{
    const Terminator& end = m_code.at(from).terminator;
    ValueID condition = end.isConditionalBranch() ? end.conditionOperand() : ValueID();
    bool saysYes = testResult;
    for (; condition && condition != test && (m_code.at(condition).opcode == Opcode::LogicalNot || m_code.at(condition).opcode == Opcode::ToBoolean || conversionsOfForm.contains(m_code.at(condition).opcode)); condition = m_code.at(condition).operand(0))
        saysYes ^= m_code.at(condition).opcode == Opcode::LogicalNot;
    return condition == test ? (saysYes ? end.taken() : end.notTaken()) : EdgeID();
}

bool CodeVerifier::everyPathPassesOneOf(std::span<const ValueID> tests, std::span<const ValueID> cells, BlockID block) const
{
    if (tests.size() < 2 || std::ranges::any_of(cells, [&](ValueID cell) { return m_code.isConstant(cell); }))
        return false;
    BitVector reached(m_code.numberOfBlocks());
    Vector<BlockID, 16> waiting;
    auto reach = [&](BlockID to) {
        if (!reached.set(to.index()))
            waiting.append(to);
    };
    for (ValueID cell : cells)
        reach(m_code.blockOf(cell));
    while (!waiting.isEmpty()) {
        BlockID from = waiting.takeLast();
        m_code.forEachEdgeOf(from, [&](EdgeID id, const Edge& edge) {
            if (std::ranges::none_of(tests, [&](ValueID test) { return findAnswerEdge(test, true, from) == id; }))
                reach(edge.to);
        });
    }
    return !reached.get(block.index());
}

bool CodeVerifier::isAnsweredAt(ValueID test, bool testResult, BlockID block, uint32_t place)
{
    if (m_code.isConstant(test) || m_code.blockOf(test) == block || !isDefinedBefore(test, block, place))
        return false;
    if (EdgeID passes = findAnswerEdge(test, testResult, m_code.blockOf(test))) {
        BlockID behind = m_code.at(passes).to;
        if (m_code.at(behind).firstEdgeIn() == passes && !m_code.at(passes).nextInto() && isReached(behind) && dominates(headOf(behind), headOf(block)))
            return true;
    }
    uint64_t asked = static_cast<uint64_t>(test.index()) << 1 | testResult;
    if (auto kept = m_reachedWithoutAnswers.find(asked); kept != m_reachedWithoutAnswers.end())
        return !kept->value.get(block.index());
    BitVector reached(m_code.numberOfBlocks());
    Vector<BlockID, 16> waiting;
    waiting.append(m_code.blockOf(test));
    while (!waiting.isEmpty()) {
        BlockID from = waiting.takeLast();
        EdgeID passes = findAnswerEdge(test, testResult, from);
        m_code.forEachEdgeOf(from, [&](EdgeID id, const Edge& edge) {
            if (id != passes && !reached.set(edge.to.index()))
                waiting.append(edge.to);
        });
    }
    bool isAnswered = !reached.get(block.index());
    m_reachedWithoutAnswers.add(asked, WTF::move(reached));
    return isAnswered;
}

void CodeVerifier::verifyTestsPassed()
{
    if (!m_code.keepsTestsPassed())
        return;
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            const Operation& access = m_code.at(id);
            bool isOfView = access.opcode == Opcode::GetTypedArrayElement || access.opcode == Opcode::PutTypedArrayElement;
            bool isRawStore = access.opcode == Opcode::PutContiguousIndex || access.opcode == Opcode::PutDoubleIndex;
            if (isRawStore && !access.isInBounds() && m_program.at(FunctionID(m_code.inlineFrames()[access.inlineFrame].callee)).role != FunctionRole::Library)
                return fail(block, id, "a raw store into a list that the library has not written names no ground"_s);
            bool isBoundedByCapacity = access.opcode == Opcode::ExtendArrayLength;
            bool isMarkedInBounds = isBoundedByCapacity || (isOfView ? access.isInsideView() : arrayAccessOpcodes.contains(access.opcode) && access.isInBounds());
            bool isPresent = access.opcode == Opcode::GetArrayElement && access.isPresent();
            if (!isMarkedInBounds && !isPresent)
                continue;
            uint32_t place = m_places[id.index()];
            const PassedTest* passed = m_code.testsPassedFor(id);
            if (!passed)
                return fail(block, id, "an access is taken to lie inside its view and names no ground"_s);
            for (const PassedTest::Side& side : passed->sides()) {
                if (!isAnsweredAt(side.test, side.testResult, block, place))
                    return fail(block, id, "a test that an access counts on is not passed on every path to it"_s, side.test);
            }
            if (!passed->groundsOfPosition())
                return fail(block, id, "an access is taken to lie inside its view and names no length"_s);
            const GroundsOfPosition& grounds = *passed->groundsOfPosition();
            static constexpr OpcodeSet readsOfLists { Opcode::GetArrayLength, Opcode::GetArrayCapacity, Opcode::IsElementPresent, Opcode::HasContiguousIndex };
            auto findArrayWriteAfter = [&](ValueID named) {
                named = named ? valueBeforeConversions(underlyingCell(named)) : ValueID();
                if (!named || m_code.isConstant(named) || !readsOfLists.contains(m_code.at(named).opcode) || !areSameCells(m_code.at(named).operand(0), access.operand(0)))
                    return ValueID();
                return findArrayWriteBetween(named, id);
            };
            bool someGroundIsSwept = false;
            for (ValueID named : { grounds.length, grounds.isBelow, grounds.isFromZeroUp, grounds.counter, grounds.start, grounds.stepNonNegativeCheck, grounds.limitWithinLengthCheck, grounds.loweredCounter, grounds.loweredFrom, grounds.steppedBeside.stepped, grounds.steppedBeside.counter, grounds.steppedBeside.steppedStart, grounds.steppedBeside.counterStart })
                someGroundIsSwept |= named && m_code.isSwept() && !m_code.isConstant(named) && !m_code.isNeeded(named);
            static constexpr OpcodeSet presenceTests { Opcode::IsElementPresent, Opcode::HasContiguousIndex };
            ValueID writtenBy = findArrayWriteAfter(grounds.length);
            for (const PassedTest::Side& side : passed->sides()) {
                const Operation& test = m_code.at(side.test);
                if (!writtenBy)
                    writtenBy = findArrayWriteAfter(side.test);
                if (!presenceTests.contains(test.opcode) && !someGroundIsSwept)
                    continue;
                ValueID position = valueBeforeConversions(underlyingCell(access.operand(1)));
                for (uint32_t operand = 0; !writtenBy && operand < test.numberOfOperands(); ++operand) {
                    if (test.operand(operand) && valueBeforeConversions(underlyingCell(test.operand(operand))) != position)
                        writtenBy = findArrayWriteAfter(test.operand(operand));
                }
            }
            if (writtenBy)
                return fail(block, id, "the storage or the length of a list is written between a ground of an access and the access"_s, writtenBy);
            auto isSamePosition = [&](ValueID one, ValueID other) {
                one = valueBeforeConversions(underlyingCell(one));
                other = valueBeforeConversions(underlyingCell(other));
                return one == other || (m_code.at(one).opcode == Opcode::Integer && m_code.at(other).opcode == Opcode::Integer && m_code.at(one).integer() == m_code.at(other).integer());
            };
            if (!areSameCells(grounds.view, access.operand(0)) || !isSamePosition(grounds.position, access.operand(1)))
                return fail(block, id, "the grounds of an access speak of another holder or of another position"_s, grounds.view);
            bool isBoundedByLiteral = !isOfView && !isBoundedByCapacity && grounds.length && m_code.at(underlyingCell(grounds.length)).opcode == Opcode::NewArray;
            bool wasFoundPresent = !isOfView && !isBoundedByCapacity && std::ranges::any_of(passed->sides(), [&](const PassedTest::Side& side) {
                const Operation& test = m_code.at(side.test);
                return side.testResult && test.opcode == Opcode::IsElementPresent && areSameCells(test.arrayOperand(), access.operand(0)) && isSamePosition(test.positionOperand(), access.operand(1));
            });
            std::optional<ListsMade> made = isPresent && !wasFoundPresent && !isBoundedByLiteral ? listsMadeUnder(access.operand(0), block) : std::nullopt;
            if (isPresent && !wasFoundPresent && !isBoundedByLiteral && (!made || !made->areWhole))
                return fail(block, id, "an element is taken to be present and no test of its presence has passed"_s);
            bool wasFoundAtOrAfter = !isOfView && !isBoundedByCapacity && testsProveElementAtOrAfter(m_code, m_types, access.operand(0), access.operand(1), passed->sides());
            if (!isMarkedInBounds || wasFoundPresent || wasFoundAtOrAfter)
                continue;
            if (!grounds.length)
                return fail(block, id, "an access is taken to lie inside its view and names no length"_s);
            if (isBoundedByLiteral) {
                const Operation& word = m_code.at(valueBeforeConversions(underlyingCell(access.operand(1))));
                const Operation& made = m_code.at(underlyingCell(grounds.length));
                if (!made.holes().empty() || word.opcode != Opcode::Integer || word.integer() < 0 || static_cast<uint64_t>(word.integer()) >= made.elementsOperands().size())
                    return fail(block, id, "a word is taken to lie inside a list that is not made with so many"_s, grounds.length);
                continue;
            }
            if (const Operation& raise = m_code.at(grounds.length); raise.opcode == Opcode::ExtendArrayLength) {
                if (!isRawStore || !mustAlias(raise.arrayOperand(), access.operand(0)) || !isSamePosition(raise.positionOperand(), access.operand(1)) || !isDefinedBefore(grounds.length, block, place))
                    return fail(block, id, "a store is taken to lie below a length that no raise in front of it has made"_s, grounds.length);
                continue;
            }
            if (someGroundIsSwept)
                continue;
            for (ValueID named : { grounds.length, grounds.isBelow, grounds.isFromZeroUp, grounds.counter, grounds.start, grounds.limitWithinLengthCheck, grounds.loweredCounter, grounds.loweredFrom, grounds.steppedBeside.stepped, grounds.steppedBeside.counter, grounds.steppedBeside.steppedStart, grounds.steppedBeside.counterStart }) {
                if (named && !isDefinedBefore(named, block, place))
                    return fail(block, id, "a ground of an access inside its view is not made in front of it on every path"_s, named);
            }
            const Operation& counted = m_code.at(underlyingCell(grounds.length));
            bool isRoomOfList = isBoundedByCapacity && counted.opcode == Opcode::GetArrayCapacity && mustAlias(counted.arrayOperand(), access.operand(0));
            if (!isRoomOfList && !isLengthOfView(grounds.length, access.operand(0), block))
                return fail(block, id, "the length that an access counts on is not that of its view"_s, grounds.length);
            if (grounds.counterTest) {
                BlockID header = m_code.blockOf(underlyingCell(grounds.counter));
                for (ValueID named : { grounds.start, grounds.loweredFrom }) {
                    if (!named || !isDefinedBefore(named, header, 0))
                        return fail(block, id, "a limit of a second counter is not computed before its loop"_s, named);
                }
                if (!isAnsweredAt(grounds.counterTest, grounds.counterTestResult, block, place) || !secondCounterStaysWithin(m_code, m_types, grounds))
                    return fail(block, id, "the second counter that an access counts on may lie outside its limits"_s, grounds.counter);
                if (!groundsShowPositionInside(m_code, m_types, access.operand(1), grounds, scopedLambda<bool(ValueID, bool)>([&](ValueID relation, bool testResult) {
                    return isAnsweredAt(relation, testResult, block, place);
                })))
                    return fail(block, id, "the grounds of an access do not show that it lies inside its view"_s, grounds.isBelow);
                continue;
            }
            bool stepHolds = !grounds.counter || !grounds.stepNonNegativeCheck || isAnsweredAt(grounds.stepNonNegativeCheck, true, m_code.blockOf(grounds.counter), 0);
            if (grounds.counter && !(stepHolds && everyWayStepsUpFrom(m_code, m_types, grounds.counter, grounds.start, grounds.stepNonNegativeCheck, scopedLambda<bool(ValueID)>([&](ValueID each) {
                return isDefinedBefore(grounds.start, m_code.blockOf(each), m_places[each.index()]);
            }), scopedLambda<bool(BlockID)>([&](BlockID each) {
                return isReached(each);
            }))))
                return fail(block, id, "the counter that an access counts on may lie below its start"_s, grounds.counter);
            if (grounds.loweredCounter && !everyWayStepsDownFrom(m_code, m_types, grounds.loweredCounter, grounds.loweredFrom, scopedLambda<bool(ValueID)>([&](ValueID each) {
                return isDefinedBefore(grounds.loweredFrom, m_code.blockOf(each), m_places[each.index()]);
            }), scopedLambda<bool(BlockID)>([&](BlockID each) {
                return isReached(each);
            })))
                return fail(block, id, "the counter that an access counts on may lie above its start"_s, grounds.loweredCounter);
            if (grounds.steppedBeside.stepped && !isSteppedWithCounter(m_code, m_types, grounds.steppedBeside))
                return fail(block, id, "the key that an access counts on is not stepped beside its counter"_s, grounds.steppedBeside.stepped);
            if (!groundsShowPositionInside(m_code, m_types, access.operand(1), grounds, scopedLambda<bool(ValueID, bool)>([&](ValueID relation, bool testResult) {
                return isAnsweredAt(relation, testResult, block, place);
            })))
                return fail(block, id, "the grounds of an access do not show that it lies inside its view"_s, grounds.isBelow);
        }
    }
}

void CodeVerifier::verifyLengthsHeldAgainst()
{
    static constexpr OpcodeSet accessesHeldAgainstLengths { Opcode::GetArrayElement, Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement, Opcode::IsElementPresent, Opcode::HasArrayElement, Opcode::AppendElement };
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            const Operation& access = m_code.at(id);
            if (!accessesHeldAgainstLengths.contains(access.opcode))
                continue;
            for (ValueID length : access.lengthOperands()) {
                if (!isLengthOfView(length, access.operand(0), block))
                    return fail(block, id, "the length that an access is held against is not that of its holder"_s, length);
            }
        }
    }
}

void CodeVerifier::verifyKindsOfStorageNamed()
{
    static constexpr OpcodeSet accessesAtLevels { Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::HasArrayElement };
    std::optional<Vector<ValueID, 8>> tests;
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            const Operation& access = m_code.at(id);
            if (access.opcode == Opcode::PutArrayElement && access.fillsPlaceOfListMadeHere()) {
                std::optional<uint64_t> length = lengthThatListIsMadeWith(m_code, m_code.at(underlyingCell(access.arrayOperand())));
                const Operation& word = m_code.at(valueBeforeConversions(underlyingCell(access.positionOperand())));
                if (access.level() != KnowledgeLevel::Proven || !length || word.opcode != Opcode::Integer || word.integer() < 0 || word.integer() >= static_cast<int64_t>(UINT32_MAX))
                    return fail(block, id, "a store is taken to lay an element into a list that is not made here, or under what is no known index"_s, access.arrayOperand());
                continue;
            }
            bool isNumericStore = access.opcode == Opcode::PutContiguousIndex && access.hasNumericStorage();
            if (!isNumericStore && (!accessesAtLevels.contains(access.opcode) || access.level() != KnowledgeLevel::Proven || !hasArrayStorageForm(access.arrayStorage())))
                continue;
            ArrayStorage named = isNumericStore ? ArrayStorage::Other : access.arrayStorage();
            bool admitsInt32Storage = access.opcode == Opcode::GetArrayElement && access.admitsInt32Storage();
            Vector<ValueID, 4> cells;
            collectUnderlyingCells(access.operand(0), cells);
            ValueID bornOtherwise;
            bool everyMakerIsInSight = !cells.isEmpty();
            for (ValueID cell : cells) {
                const Operation& made = m_code.at(cell);
                const auto* written = made.opcode == Opcode::Data && made.encoding() == DataEncoding::CopyOnWriteArray && made.data() ? m_program.at(made.data()).root.tryAs<DataNode::Array>() : nullptr;
                everyMakerIsInSight &= written || made.opcode == Opcode::NewArray;
                if (!everyMakerIsInSight)
                    break;
                ArrayStorage born = written ? arrayStorageOfData(m_program, *written) : storageThatLiteralIsSurelyBornWith(m_program, m_types, m_code, made);
                if (isNumericStore ? !isNumericStorage(born) : born != named && !(admitsInt32Storage && born == ArrayStorage::Int32))
                    bornOtherwise = cell;
            }
            if (everyMakerIsInSight ? !bornOtherwise : !isNumericStore)
                continue;
            if (!tests) {
                tests.emplace();
                for (BlockID each : m_reachedBlocks) {
                    for (ValueID test : operationsOf(each)) {
                        if (m_code.at(test).opcode == Opcode::HasArrayStorage)
                            tests->append(test);
                    }
                }
            }
            auto showsKind = [&](const Operation& test) {
                return isNumericStore ? test.isWritable() && isNumericStorage(test.arrayStorage()) : test.arrayStorage() == named;
            };
            Vector<ValueID, 8> testsOfList;
            for (ValueID test : *tests) {
                if (showsKind(m_code.at(test)) && areSameCells(m_code.at(test).arrayOperand(), access.operand(0)))
                    testsOfList.append(test);
            }
            bool hasPassedTest = std::ranges::any_of(testsOfList, [&](ValueID test) {
                return isAnsweredAt(test, true, block, m_places[id.index()]);
            });
            bool everyCellHasPassed = !hasPassedTest && cells.size() > 1 && std::ranges::all_of(cells, [&](ValueID cell) {
                return std::ranges::any_of(*tests, [&](ValueID test) {
                    return showsKind(m_code.at(test)) && areSameCells(m_code.at(test).arrayOperand(), cell) && isAnsweredAt(test, true, block, m_places[id.index()]);
                });
            });
            if (!hasPassedTest && !everyCellHasPassed && !everyPathPassesOneOf(testsOfList, cells, block))
                return fail(block, id, isNumericStore ? "a store names a numeric kind of storage that no passed test of its array shows"_s : "an access names a kind of storage that a maker of its list is not surely born with, and no test of that kind has passed"_s, isNumericStore ? access.arrayOperand() : bornOtherwise, InternalErrorKind::InvalidGuard);
        }
    }
}

void CodeVerifier::verifyFramePointersTakenBy(BlockID block, ValueID id, const Operation& operation)
{
    if (handsOperandsToCallee(operation.opcode)) {
        for (uint32_t operandIndex = 0; operandIndex < operation.numberOfOperands(); ++operandIndex) {
            ValueID operand = operation.operand(operandIndex);
            if (operand && mayPointIntoFrame(operand) && isHeldOperand(operation, operandIndex))
                return fail(block, id, "a cell of the frame is held across a suspension"_s, operand, InternalErrorKind::InvalidSuspension);
            if (operand && mayPointIntoFrame(operand) && !isCallOfTailCall(block, id) && !loanCoversOperandOfCall(operation, operandIndex, operand))
                return fail(block, id, "a cell of the frame is an operand of a call that no loan covers"_s, operand);
        }
        return;
    }
    for (uint32_t operandIndex = 0; operandIndex < operation.numberOfOperands(); ++operandIndex) {
        ValueID operand = operation.operand(operandIndex);
        if (!operand || !mayPointIntoFrame(operand))
            continue;
        bool handsOnAsNarrowReference = operation.representation == Representation::Cell32 && operation.operandUses(operandIndex).contains(OperandUse::Returns);
        if (operation.opcode == Opcode::Compress || handsOnAsNarrowReference)
            return fail(block, id, "the address of a cell of the frame is made a 32-bit reference"_s, operand);
        if (!operation.operandUses(operandIndex).contains(OperandUse::Retains))
            continue;

        bool staysInFrame = false;
        switch (retainedInOf(operation.opcode, operandIndex)) {
        case RetainedIn::UnknownMemory:
            break;
        case RetainedIn::Result:
            staysInFrame = isFrameCell(operation);
            break;
        case RetainedIn::Operand: {
            ValueID holder = operandAt(operation, retainingOperandOf(operation.opcode, operandIndex).value_or(UINT8_MAX));
            staysInFrame = holder && isFrameCell(m_code.at(cellBeforeForwarding(holder)));
            break;
        }
        }
        if (!staysInFrame)
            return fail(block, id, "a cell of the frame is stored into what is no cell of the frame"_s, operand);
    }
}

void CodeVerifier::verifyFramePointersTakenByEnd(BlockID block, const Terminator& end)
{
    ASCIILiteral rule;
    switch (end.kind) {
    case TerminatorKind::ReturnCall:
        m_code.at(end.callOperand()).forEachChild([&](ValueID operand) {
            if (mayPointIntoFrame(operand))
                fail(block, end.callOperand(), "a cell of the frame is an operand of a tail call"_s, operand);
        });
        [[fallthrough]];
    case TerminatorKind::Return:
        rule = "a cell of the frame is returned"_s;
        break;
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
        rule = "a cell of the frame is thrown"_s;
        break;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        end.forEachChild([&](ValueID operand) {
            if (mayPointIntoFrame(operand) && isClosureOfFrame(operand))
                fail(block, { }, "a closure of the frame is read by the end of a block"_s, operand);
        });
        return;
    }
    end.forEachChild([&](ValueID operand) {
        if (mayPointIntoFrame(operand))
            fail(block, { }, rule, operand);
    });
}

bool CodeVerifier::isClosureOfFrame(ValueID name) const
{
    const Operation& made = m_code.at(underlyingCell(name));
    return made.opcode == Opcode::NewClosure && isFrameCell(made);
}

LayoutID CodeVerifier::layoutOfEnvironment(ValueID id) const
{
    TypeID type = m_code.at(id).type;
    if (!type || holdsOnlyPrimitives(type))
        return { };
    return m_types.at(type).environment;
}

void CodeVerifier::verifyMakerOfClosure(BlockID block, ValueID id, const Operation& made)
{
    if (isFrameCell(made) != (made.loan() != LoanOfClosure::None))
        return fail(block, id, "where a closure lies and who borrows it do not agree"_s);
    if (!isFrameCell(made))
        return;
    if (made.closureForm() != ClosureForm::Slim && made.closureForm() != ClosureForm::Full)
        return fail(block, id, "a closure of the frame has a form that has no cell of its own"_s);
    if (!closuresOfFunctionMayBeLent(m_program, made.function()))
        return fail(block, id, "a closure that its own function names lies in the frame"_s);
    bool isCoroutine = m_program.at(m_code.function()).isCoroutine();
    if (isBodyOfCoroutine(m_code.role(), isCoroutine) || (isCoroutine && walksInputBody(m_code.role())))
        return fail(block, id, "a closure lies in the frame of a coroutine"_s, { }, InternalErrorKind::InvalidSuspension);
}

void CodeVerifier::verifyClosuresOfFrameTakenBy(BlockID block, ValueID id, const Operation& taker)
{
    uint32_t numberOfNamedOperands = taker.numberOfNamedOperands();
    for (uint32_t operandIndex = 0; operandIndex < taker.numberOfOperands(); ++operandIndex) {
        ValueID name = taker.operand(operandIndex);
        if (!name || !mayPointIntoFrame(name) || !isClosureOfFrame(name))
            continue;
        ValueID closure = underlyingCell(name);
        const Operation& made = m_code.at(closure);
        if (m_namesOfClosuresTaken.quickSet(name.index()))
            return fail(block, id, "a closure of the frame is taken twice"_s, name);
        if (m_code.blockOf(closure) != block)
            return fail(block, id, "a closure of the frame is taken outside the block that makes it"_s, name);
        if (!operandIndex && cellPreservingOpcodes.contains(taker.opcode))
            continue;

        bool isArgument = operandIndex >= numberOfNamedOperands;
        uint32_t position = isArgument ? operandIndex - numberOfNamedOperands : 0;
        switch (taker.opcode) {
        case Opcode::CallDirect:
            if (made.loan() != LoanOfClosure::ToFunctionEntered || !returnsResultOfFunction(taker.role()) || taker.numberOfBoundArguments() || !onlyCallsWhatArrivesAt(taker.function(), position))
                return fail(block, id, "a closure of the frame is handed to a function that may do more than call it"_s, name);
            break;
        case Opcode::Call: {
            bool isLookedAtByStub = isArgument && position < ClosureWord::numberOfBorrowedArguments && taker.numberOfVariadicOperands() <= numberOfArgumentGPRs;
            if (BuiltinID method = taker.methodOfReceiverAsMade(); method && isLookedAtByStub)
                isLookedAtByStub = !position && m_facts && stubOfMethodEntersFunctionsThatOnlyCallFirstArgument(m_program, *m_facts, method);
            if (made.loan() != LoanOfClosure::LeftToStubOfCall || made.closureForm() != ClosureForm::Slim || !isLookedAtByStub)
                return fail(block, id, "a closure of the frame is handed to a call whose stub does not look at it"_s, name);
            break;
        }
        case Opcode::NewClosure:
            if (made.loan() != LoanOfClosure::ToFunctionEntered || !onlyCallsWhatArrivesAt(taker.function(), position))
                return fail(block, id, "a closure of the frame is captured by a closure that may do more than call it"_s, name);
            break;
        default:
            return fail(block, id, "a closure of the frame is taken by what is neither a call nor a closure of the frame"_s, name);
        }
    }
}

void CodeVerifier::verifyEnvironmentsHeldByClosuresOfFrame(BlockID block)
{
    Vector<ValueID, 4> closuresThatAwaitTheirCalls;
    auto noteTaken = [&](ValueID name) {
        Vector<ValueID, 4> taken { underlyingCell(name) };
        while (!taken.isEmpty()) {
            ValueID closure = taken.takeLast();
            if (!closuresThatAwaitTheirCalls.removeFirst(closure))
                continue;
            m_code.at(closure).forEachChild([&](ValueID capture) {
                taken.append(underlyingCell(capture));
            });
        }
    };
    for (ValueID id : operationsOf(block)) {
        const Operation& operation = m_code.at(id);
        if (operation.opcode == Opcode::NewClosure) {
            if (isFrameCell(operation))
                closuresThatAwaitTheirCalls.append(id);
            continue;
        }
        if (closuresThatAwaitTheirCalls.isEmpty())
            continue;
        if (handsOperandsToCallee(operation.opcode)) {
            operation.forEachChild(noteTaken);
            continue;
        }
        if (operation.opcode != Opcode::ReleaseEnvironment || (m_code.isSwept() && !m_code.marksOf(id).contains(SweepMark::ReleasesOperand)))
            continue;
        LayoutID handedBack = layoutOfEnvironment(operation.environmentOperand());
        for (ValueID closure : closuresThatAwaitTheirCalls) {
            m_code.at(closure).forEachChild([&](ValueID capture) {
                if (handedBack && layoutOfEnvironment(capture) == handedBack)
                    fail(block, id, "the environment that a closure of the frame holds is handed back in front of the call that takes the closure"_s, closure);
            });
        }
        if (hasFailed())
            return;
    }
}

void CodeVerifier::verifyFrameCells()
{
    m_mayPointIntoFrame.ensureSize(m_code.numberOfValues());
    bool hasFrameCells = false;
    bool hasClosuresOfFrame = false;
    bool hasFlatStrings = false;
    bool hasTextsInFrame = false;
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            const Operation& operation = m_code.at(id);
            hasFlatStrings |= operation.opcode == Opcode::FlattenString;
            hasTextsInFrame |= operation.opcode == Opcode::StackStringBuilderBegin;
            if (operation.opcode == Opcode::NewClosure) {
                verifyMakerOfClosure(block, id, operation);
                if (hasFailed())
                    return;
            }
            if (!isFrameCell(operation))
                continue;
            m_mayPointIntoFrame.quickSet(id.index());
            hasFrameCells = true;
            hasClosuresOfFrame |= operation.opcode == Opcode::NewClosure;
        }
    }
    if (!hasFrameCells && !hasFlatStrings && !hasTextsInFrame)
        return;
    m_namesOfClosuresTaken.ensureSize(m_code.numberOfValues());

    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        for (BlockID block : m_reachedBlocks) {
            for (ValueID id : operationsOf(block)) {
                if (!mayPointIntoFrame(id) && handsBackPointerIntoFrame(m_code.at(id))) {
                    m_mayPointIntoFrame.quickSet(id.index());
                    hasChanged = true;
                }
            }
            m_code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
                std::span<const ValueID> arguments = edge.arguments();
                std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
                for (size_t index = 0; index < arguments.size(); ++index) {
                    if (mayPointIntoFrame(arguments[index]) && !m_mayPointIntoFrame.quickSet(parameters[index].index()))
                        hasChanged = true;
                }
            });
        }
    }

    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            verifyFramePointersTakenBy(block, id, m_code.at(id));
            if (hasClosuresOfFrame && !hasFailed())
                verifyClosuresOfFrameTakenBy(block, id, m_code.at(id));
            if (hasFailed())
                return;
        }
        verifyFramePointersTakenByEnd(block, m_code.at(block).terminator);
        if (ValueID subject = m_code.at(block).subject; subject && mayPointIntoFrame(subject) && isClosureOfFrame(subject))
            fail(block, { }, "a closure of the frame is read by the end of a block"_s, subject);
        if (hasClosuresOfFrame && !hasFailed())
            verifyEnvironmentsHeldByClosuresOfFrame(block);
        if (hasFailed())
            return;
    }

    for (BlockID block : m_reachedBlocks) {
        m_code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
            std::span<const ValueID> arguments = edge.arguments();
            std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
            for (size_t index = 0; index < arguments.size(); ++index) {
                ValueID argument = arguments[index];
                bool isRead = !m_code.isSwept() || index >= parameters.size() || m_code.isNeeded(parameters[index]);
                if (isRead && mayPointIntoFrame(argument) && isClosureOfFrame(argument))
                    fail(block, { }, "a closure of the frame is an argument of an edge"_s, argument);
                const Operation& made = m_code.at(cellBeforeForwarding(argument));
                if (made.opcode != Opcode::NewObject || made.storage() != AllocationStorage::Frame || !m_program.at(made.layout()).isEnvironment())
                    continue;
                fail(block, { }, "an environment that lies in the frame is an argument of an edge"_s, argument);
            }
        });
        if (hasFailed())
            return;
    }

    if (!isBodyOfCoroutine(m_code.role(), m_program.at(m_code.function()).isCoroutine()))
        return;

    Vector<uint32_t> placeOfLastSuspension;
    placeOfLastSuspension.fill(0, m_code.numberOfBlocks());
    for (BlockID block : m_reachedBlocks) {
        uint32_t place = 0;
        for (ValueID id : operationsOf(block)) {
            ++place;
            if (m_code.at(id).traits().maySuspend())
                placeOfLastSuspension[block.index()] = place;
        }
    }

    BitVector isOnWay(m_code.numberOfBlocks());
    Vector<BlockID, 16> blocksToLookAt;
    auto maySuspendBetweenMakerAndHeadOf = [&](BlockID reader, ValueID made) {
        BlockID maker = m_code.blockOf(made);
        isOnWay.clearAll();
        blocksToLookAt.shrink(0);
        auto lookAtBlocksInFrontOf = [&](BlockID block) {
            for (EdgeID edge : incomingEdgesOf(block)) {
                BlockID from = m_code.at(edge).from;
                if (!isOnWay.quickSet(from.index()))
                    blocksToLookAt.append(from);
            }
        };
        lookAtBlocksInFrontOf(reader);
        while (!blocksToLookAt.isEmpty()) {
            BlockID block = blocksToLookAt.takeLast();
            if (block == maker) {
                if (placeOfLastSuspension[block.index()] > m_places[made.index()])
                    return true;
                continue;
            }
            if (placeOfLastSuspension[block.index()])
                return true;
            lookAtBlocksInFrontOf(block);
        }
        return false;
    };

    auto isFlatString = [&](ValueID id) {
        TypeID type = m_code.at(id).type;
        return type && m_types.at(type).isOpaque(OpaqueKind::FlatString);
    };
    auto isTextInBuilder = [&](ValueID id) {
        TypeID type = m_code.at(id).type;
        if (!hasTextsInFrame || !type)
            return false;
        return m_types.at(type).isOpaque(OpaqueKind::BuilderCursor) || (m_types.at(type).isOpaque(OpaqueKind::StringBuilder) && !stringBuilderIsCellOfHeap(m_code, id));
    };
    for (BlockID block : m_reachedBlocks) {
        for (ValueID parameter : m_code.at(block).parameters()) {
            if (mayPointIntoFrame(parameter))
                return fail(block, parameter, "a cell of the frame of a coroutine body crosses an edge"_s, { }, InternalErrorKind::InvalidSuspension);
            if (isFlatString(parameter))
                return fail(block, parameter, "a flat string of a coroutine body crosses an edge"_s, { }, InternalErrorKind::InvalidSuspension);
        }
        uint32_t place = 0;
        uint32_t firstPlaceBehindSuspension = 0;
        auto verifyUse = [&](ValueID user, ValueID operand) {
            bool isCellOfFrame = mayPointIntoFrame(operand);
            bool isText = isTextInBuilder(operand);
            if (!isCellOfFrame && !isText && !isFlatString(operand))
                return;
            bool outlivesSuspension = m_code.blockOf(operand) == block
                ? m_places[operand.index()] < firstPlaceBehindSuspension
                : firstPlaceBehindSuspension || maySuspendBetweenMakerAndHeadOf(block, operand);
            if (outlivesSuspension)
                fail(block, user, isCellOfFrame ? "a cell of the frame of a coroutine body outlives a suspension"_s : isText ? "a text in a builder of the frame outlives a suspension"_s : "a flat string outlives a suspension"_s, operand, InternalErrorKind::InvalidSuspension);
        };
        for (ValueID id : operationsOf(block)) {
            const Operation& operation = m_code.at(id);
            ++place;
            if (operation.traits().maySuspend())
                firstPlaceBehindSuspension = place + 1;
            operation.forEachChild([&](ValueID operand) {
                verifyUse(id, operand);
            });
            if (hasFailed())
                return;
        }
        m_code.at(block).terminator.forEachChild([&](ValueID operand) {
            verifyUse({ }, operand);
        });
        if (hasFailed())
            return;
    }
}

void CodeVerifier::verifyWordsInPlaceOfCaptures()
{
    if (!m_plan || !m_plan->promotedEnvironments().isMaker(m_code.function()))
        return;
    const PromotedEnvironments& promoted = m_plan->promotedEnvironments();
    auto isFilledByThisFunction = [&](VariableID word) {
        const PromotedVariable* variable = promoted.find(word);
        return variable && variable->maker == m_code.function();
    };
    auto wordFilledBy = [&](const Operation& operation) {
        VariableID word;
        if (operation.opcode == Opcode::PutModuleVariable)
            word = operation.variable();
        else if (operation.opcode == Opcode::PutEnvironmentField)
            word = promoted.variableOf(operation.layout(), 0);
        return word && isFilledByThisFunction(word) ? word : VariableID();
    };

    Vector<VariableID, 8> words;
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            if (VariableID word = wordFilledBy(m_code.at(id)))
                words.append(word);
        }
    }
    std::ranges::sort(words);
    words.shrink(std::ranges::unique(words).begin() - words.begin());
    auto numberOf = [&](VariableID variable) -> std::optional<size_t> {
        auto found = std::ranges::lower_bound(words, variable);
        if (found == words.end() || *found != variable)
            return std::nullopt;
        return static_cast<size_t>(found - words.begin());
    };

    uint32_t numberOfNodes = m_numbers.size();
    Vector<BitVector> filledIn;
    Vector<BitVector> filledBehind;
    filledIn.fill(BitVector(words.size()), numberOfNodes);
    filledBehind.fill(BitVector(words.size()), numberOfNodes);
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            if (VariableID word = wordFilledBy(m_code.at(id)))
                filledIn[nodeThatDefines(id)].quickSet(*numberOf(word));
        }
    }

    BitVector isJudged(numberOfNodes);
    isJudged.quickSet(root());
    auto filledInFrontOf = [&](uint32_t node) {
        BitVector filled(words.size());
        bool hasWay = false;
        auto meet = [&](uint32_t predecessor) {
            if (!isJudged.quickGet(predecessor))
                return;
            if (std::exchange(hasWay, true))
                filled.filter(filledBehind[predecessor]);
            else
                filled = filledBehind[predecessor];
        };
        BlockID block(node / 2);
        if (isTail(node))
            meet(headOf(block));
        else {
            if (isWhereWalkBegan(block))
                meet(root());
            for (EdgeID edge : incomingEdgesOf(block))
                meet(nodeThatEdgeLeaves(edge));
        }
        return filled;
    };
    for (bool hasChanged = true; hasChanged;) {
        hasChanged = false;
        for (uint32_t node : m_nodesInOrder.span().subspan(1)) {
            BitVector filled = filledInFrontOf(node);
            filled.merge(filledIn[node]);
            if (isJudged.quickSet(node) && filled == filledBehind[node])
                continue;
            filledBehind[node] = WTF::move(filled);
            hasChanged = true;
        }
    }

    for (BlockID block : m_reachedBlocks) {
        BitVector filled = filledInFrontOf(headOf(block));
        for (ValueID id : operationsOf(block)) {
            const Operation& operation = m_code.at(id);
            if (VariableID word = wordFilledBy(operation)) {
                filled.quickSet(*numberOf(word));
                continue;
            }
            bool makesClosure = operation.opcode == Opcode::NewClosure;
            if ((!makesClosure && operation.opcode != Opcode::CallDirect) || operation.origin().inlineFrame)
                continue;
            auto mustBeFilled = [&](VariableID word) {
                if (!isFilledByThisFunction(word))
                    return;
                std::optional<size_t> number = numberOf(word);
                if (!number || !filled.quickGet(*number))
                    fail(block, id, "a word in the place of captures is not filled on every way to whoever may read it"_s, { }, InternalErrorKind::UseNotDominated);
            };
            std::span<const ValueID> passedOperands = operation.variadicOperands();
            for (const RemovedCapture& removed : promoted.removedCapturesOf(operation.function())) {
                if (removed.variable) {
                    ValueID handed = removed.capture < passedOperands.size() ? passedOperands[removed.capture] : ValueID();
                    if (makesClosure || (handed && m_code.at(handed).opcode != Opcode::Unspecified))
                        mustBeFilled(removed.variable);
                    continue;
                }
                LayoutID scope = makesClosure ? m_program.at(m_program.provenTypeOf(m_program.at(removed.function).captures[removed.capture])).environment : LayoutID();
                bool listsValuesWhereItIsMade = scope && std::ranges::any_of(m_program.at(scope).fields, [](const Field& field) {
                    return field.flags.contains(FieldFlags::IsRequired);
                });
                if (listsValuesWhereItIsMade)
                    mustBeFilled(promoted.variableOf(scope, 0));
            }
            if (hasFailed())
                return;
        }
    }
}

} // anonymous namespace

String verifyCode(const CodeTextContext& context, const Code& code, InternalErrorKind* kindOfBrokenRule)
{
    CodeVerifier verifier(context, code);
    String brokenRule = verifier.run();
    if (kindOfBrokenRule)
        *kindOfBrokenRule = verifier.kindOfFailure();
    return brokenRule;
}

String verifySweptCode(const CodeTextContext& context, const Code& code, InternalErrorKind* kindOfBrokenRule)
{
    RELEASE_ASSERT(code.isSwept());
    CodeVerifier verifier(context, code);
    String brokenRule = verifier.runOnSweptCode();
    if (kindOfBrokenRule)
        *kindOfBrokenRule = verifier.kindOfFailure();
    return brokenRule;
}

String verifyFrameCellsOfSweptCode(const CodeTextContext& context, const Code& code, InternalErrorKind* kindOfBrokenRule)
{
    RELEASE_ASSERT(code.isSwept());
    CodeVerifier verifier(context, code);
    String brokenRule = verifier.runOnFrameCellsAlone();
    if (kindOfBrokenRule)
        *kindOfBrokenRule = verifier.kindOfFailure();
    return brokenRule;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
