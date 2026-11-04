/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCode.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTHeldOperands.h"
#include "AOTIRProgram.h"
#include "AOTOperandKinds.h"
#include "AOTPlan.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTSlowFrames.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// No fact is asked. Which entry parameters get no word, what a slot is laid out for, and what a variable holds whose object waits in the variable beside it, is Plan's to say, as it is for the walk.
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
        const DeferredDefinitions& definitions = m_plan->deferredDefinitions();
        if (definitions.objectBeside(variable))
            return TypeArena::fromPrimitives(Primitive::Absent | Primitive::Undefined);
        if (definitions.findDeferred(variable))
            return TypeTable::anythingOrAbsent;
        return { };
    }

    TypeID environmentField(TypeLevel, LayoutID, uint32_t) const final { return { }; }
    // As Walker::PositionsOfWalk::slot() answers. No answer would hold a store to what is written of the field, and a field of the result of a built-in is written as the built-in fills it:
    // the program may store anything there, and then the slot is laid out for more, and read as more.
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
    TypeID contents(TypeLevel, ObjectID, ContentsSlot) const final { return { }; }
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

class CodeVerifier {
    WTF_MAKE_NONCOPYABLE(CodeVerifier);
public:
    CodeVerifier(const CodeTextContext&, const Code&);

    String run();
    InternalErrorKind kindOfFailure() const { return m_kindOfFailure; }

private:
    static constexpr uint32_t notListed = UINT32_MAX;
    static constexpr uint32_t notReached = UINT32_MAX;

    void fail(BlockID, ValueID record, ASCIILiteral rule, ValueID other = { }, InternalErrorKind = InternalErrorKind::OpcodeNotInForm);
    bool hasFailed() const { return !m_failure.isNull(); }

    bool isRecord(ValueID id) const { return id && id.index() < m_code.numberOfValues(); }
    bool isBlock(BlockID id) const { return id && id.index() < m_code.numberOfBlocks(); }
    std::span<const ValueID> operationsOf(BlockID) const;
    std::span<const EdgeID> incomingEdgesOf(BlockID) const;

    // A throw leaves a block in front of whatever the block defines from its first operation that may throw on.
    // So a block is two nodes of the flow graph. The handler edge leaves the head, which holds the parameters and the operations in front of that one. The edges of the end leave the tail.
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
    bool isDefinedInFrontOf(ValueID definition, BlockID, uint32_t place) const;
    bool isDefinedWhereBlockMayThrow(ValueID definition, BlockID) const;

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
    void verifyFrameCells();

    template<typename Entry> void verifyWords(BlockID, ValueID, const Instruction&, Entry);
    void verifyBlock(BlockID);
    void verifyOperation(BlockID, uint32_t place, ValueID, const Operation&);
    void verifyEnd(BlockID, uint32_t place);
    void verifyReturn(BlockID, const Terminator&);
    void verifyArguments(BlockID from, const Edge&, uint32_t place, uint32_t numberOfDeclaredResults, bool isHandlerEdge);
    void verifyNarrowed(BlockID, ValueID, const Operation&);
    void verifyLeave(BlockID, ValueID, const Operation&);
    void verifyPointerIntoStorage(BlockID, ValueID access, ValueID pointer, Opcode readOfPointer, ValueID owner);
    void verifyFramePointersTakenBy(BlockID, ValueID, const Operation&);
    void verifyFramePointersTakenByEnd(BlockID, const Terminator&);

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
    static ValueID valueOwnedBy(const Terminator&);
    static size_t numberOfWrittenCasesOf(const Terminator&);
    static EffectTraits boundOnTraitsOf(const Operation&);
    static bool handsOperandsToCallee(Opcode);
    static ValueID operandHandedBackBy(const Operation&);
    ValueID valueBeforeConversions(ValueID) const;
    ValueID holderOfSlotRead(ValueID read) const;
    ValueID cellBeforeForwarding(ValueID) const;
    ValueID cellBehindNames(ValueID) const;
    void collectCellsThatArriveUnder(ValueID name, Vector<ValueID, 4>& cells) const;
    bool mayPointIntoFrame(ValueID id) const { return m_mayPointIntoFrame.quickGet(id.index()); }
    bool handsBackPointerIntoFrame(const Operation&) const;

    const Program& m_program;
    TypeArena& m_types;
    const Plan* m_plan;
    const Code& m_code;
    PositionsOfCodeVerifier m_positions;
    TypingContext m_typing;
    String m_failure;
    InternalErrorKind m_kindOfFailure { InternalErrorKind::OpcodeNotInForm };

    // By record. A parameter has the place zero. The operations of a block count from one on.
    Vector<uint32_t> m_places;
    BitVector m_isOwnedByEnd;
    BitVector m_mayPointIntoFrame;
    // By block.
    Vector<uint32_t> m_firstOperation;
    Vector<uint32_t> m_placeOfFirstThrow;
    Vector<uint32_t> m_firstIncomingEdge;
    // By node.
    Vector<uint32_t> m_numbers;
    Vector<uint32_t> m_dominators;
    // The dominator tree is numbered in preorder: what a node dominates has the numbers from the first to the last.
    Vector<uint32_t> m_firstNumberOfSubtree;
    Vector<uint32_t> m_lastNumberOfSubtree;

    Vector<ValueID> m_operations;
    Vector<EdgeID> m_incomingEdges;
    Vector<BlockID, 2> m_entryBlocks;
    // In reverse postorder.
    Vector<uint32_t> m_nodesInOrder;
    Vector<BlockID> m_reachedBlocks;
    // Of the record being verified, by the schema's operand index.
    Vector<TypeID, 16> m_operandTypes;
    Vector<Representation, 16> m_operandForms;
};

CodeVerifier::CodeVerifier(const CodeTextContext& context, const Code& code)
    : m_program(context.program)
    , m_types(context.types)
    , m_plan(context.plan)
    , m_code(code)
    , m_positions(context.program, context.plan)
    , m_typing { context.program, context.types, m_positions, TypeLevel::Proven }
{
}

String CodeVerifier::run()
{
    using Stage = void (CodeVerifier::*)();
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
        &CodeVerifier::verifyFrameCells,
    };
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
    // Code is gone when the text is read, so each number brings the name of what it stood for.
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
        if (contents.hasEnded()) {
            for (uint32_t targetIndex = 0; targetIndex < contents.terminator.numberOfEdges; ++targetIndex)
                claim(block, EdgeID::fromBits(contents.terminator.firstEdge.bits() + targetIndex));
        }
        if (contents.handler)
            claim(block, contents.handler);
        if (hasFailed())
            return;
    }

    for (uint32_t edgeIndex = 0; edgeIndex < numberOfEdges; ++edgeIndex) {
        const Edge& edge = m_code.at(EdgeID(edgeIndex));
        if (!hasOwner.quickGet(edgeIndex))
            return fail(edge.from, { }, "an edge belongs to the end of no block: its block has ended twice"_s);
        if (edge.isOpen())
            return fail(edge.from, { }, "an edge of the block is open"_s);
        if (!isBlock(edge.to))
            return fail(edge.from, { }, "an edge of the block leads to no block"_s);
        for (ValueID argument : edge.arguments()) {
            if (!isRecord(argument))
                return fail(edge.from, argument, "an argument of an edge is no record"_s);
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

    // By node: the epoch in which a climb last left it. Each search for the dominator of one node is an epoch.
    // Whatever a climb has left in an epoch lies below the dominator found so far, and so does a predecessor that climbs to such a node.
    // So all the blocks that throw to one handler block cost one climb together, and not one each.
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

    // In m_nodesInOrder a dominator stands in front of whatever it dominates.
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

bool CodeVerifier::isDefinedInFrontOf(ValueID definition, BlockID block, uint32_t place) const
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
        if (info.kind == OperandKind::Guard || info.kind == OperandKind::Fact) {
            if (operand)
                return fail(block, id, "an operand that is a guard or a fact is not empty"_s, operand);
            continue;
        }
        if (!operand && !info.isOptional) {
            bool isFirstFailure = !hasFailed();
            fail(block, id, "a required operand is absent"_s);
            if (isFirstFailure)
                m_failure = makeString(m_failure, ": operand "_s, operandIndex);
            return;
        }
        if (operand && !isRecord(operand))
            return fail(block, id, "an operand is no record"_s, operand);
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
        if (!isDefinedInFrontOf(operand, block, place))
            return fail(block, user, "the definition of an operand does not stand in front of the use on every path"_s, operand, InternalErrorKind::UseNotDominated);
        if (resultRuleOf(definition.opcode) == ResultRule::None)
            return fail(block, user, "an operand has no result"_s, operand);
        if (holdsNoneOfItsResults(definition) && !mayNameProducerOfSeveralResults(record, operandIndex))
            return fail(block, user, "an operand gives several results and is taken for one value"_s, operand);
        if (m_isOwnedByEnd.quickGet(operand.index()) && operand != ownedValueItMayUse)
            return fail(block, user, "an operand is generated by the end of its block, for that end alone"_s, operand);
        if (hasOnlyLowestBitsOfInt32(definition) && !looksOnlyAtLowestBitsOfOperand(record, operandIndex))
            return fail(block, user, "an operand has only the lowest bits of an int32, and its user is no BitAnd with a mask of those"_s, operand);
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

// What has no register holds no value, whatever its type: a closure that its slot leaves unmade. A use that needs the value asks for a form that has a register.
bool CodeVerifier::hasLegalForm(const Operation& record) const
{
    if (!record.type || record.representation == Representation::Unchosen)
        return false;
    if (!hasRegister(record.representation) || record.representation == Representation::RawWord)
        return true;
    return isLegalRepresentation(m_types.at(record.type), record.representation);
}

// Nothing: how many results a direct call hands back is Plan's to say, and there is no plan.
std::optional<uint32_t> CodeVerifier::numberOfResultsGivenBy(const Operation& producer) const
{
    if (producer.opcode != Opcode::CallDirect)
        return numberOfResultsOf(m_program, producer).value_or(1);
    if (!m_plan)
        return std::nullopt;
    const CallingConventions& conventions = m_plan->conventions();
    return conventions.numberOfResults(conventions.unitEnteredBy(producer));
}

// By its type such a record is undefined, which passes under every kind that takes a value of JavaScript. Its word holds none of the results: a ResultAt reads one, and an end that returns hands all of them on.
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
    return conventions.exists(entered) && (conventions.numberOfResults(entered) != 1 || conventions.of(entered.function).hasSplitResult);
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
    // Under a handler whatever may collect may throw for lack of memory: Walker::append().
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

    if (OperandPosition::storage(opcode))
        verifyPointerIntoStorage(block, id, operation.storageOperand(), Opcode::GetArrayStorage, operation.arrayOperand());
    else if (OperandPosition::vector(opcode))
        verifyPointerIntoStorage(block, id, operation.vectorOperand(), Opcode::GetTypedArrayVector, operation.viewOperand());
}

ValueID CodeVerifier::cellBehindNames(ValueID id) const
{
    while (opcodesThatGiveSameCell.contains(m_code.at(id).opcode))
        id = m_code.at(id).operand(0);
    return id;
}

// Known::setLatest(): a parameter of a join whose every argument is a name of one value is a name of it too. So two names of one value have the same answer here: whatever the edges bring, behind names and parameters, ascending.
void CodeVerifier::collectCellsThatArriveUnder(ValueID name, Vector<ValueID, 4>& cells) const
{
    Vector<ValueID, 8> waiting { name };
    Vector<ValueID, 8> parametersSeen;
    while (!waiting.isEmpty()) {
        ValueID cell = cellBehindNames(waiting.takeLast());
        std::span<const EdgeID> incoming;
        size_t index = 0;
        if (m_code.at(cell).opcode == Opcode::Parameter && !m_code.at(m_code.blockOf(cell)).isEntry) {
            std::span<const ValueID> parameters = m_code.at(m_code.blockOf(cell)).parameters();
            index = std::ranges::find(parameters, cell) - parameters.begin();
            incoming = incomingEdgesOf(m_code.blockOf(cell));
        }
        // What is left stands for a result that the end of a block declares.
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

// The emitter loads and stores through the pointer and never looks at the cell. That nothing has made the storage anew in between is Known's to say.
void CodeVerifier::verifyPointerIntoStorage(BlockID block, ValueID access, ValueID pointer, Opcode readOfPointer, ValueID owner)
{
    if (!pointer)
        return;
    const Operation& read = m_code.at(pointer);
    if (read.opcode != readOfPointer)
        return fail(block, access, "an access takes for a pointer into storage what no read of such a pointer gives"_s, pointer);
    ValueID ownerOfPointer = read.operand(*baseOf(readOfPointer));
    if (cellBehindNames(ownerOfPointer) == cellBehindNames(owner))
        return;
    Vector<ValueID, 4> accessed;
    Vector<ValueID, 4> pointedInto;
    collectCellsThatArriveUnder(owner, accessed);
    collectCellsThatArriveUnder(ownerOfPointer, pointedInto);
    if (accessed != pointedInto)
        fail(block, access, "an access takes a pointer into the storage of another object"_s, pointer);
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

// Most lists of results are read by ResultAt alone and take their values as they are. The one that the unit returns lies where the convention of the unit says.
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
        // Nobody takes a result that has no word, so what stands in its place says nothing: appendResultOfSplitReturn().
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
        if (!(isHandlerEdge ? isDefinedWhereBlockMayThrow(argument, from) : isDefinedInFrontOf(argument, from, place)))
            return fail(from, { }, "the definition of an argument of an edge does not stand in front of the edge on every path"_s, argument, InternalErrorKind::UseNotDominated);
        if (resultRuleOf(m_code.at(argument).opcode) == ResultRule::None || m_isOwnedByEnd.quickGet(argument.index()))
            return fail(from, { }, "an argument of an edge has no result to hand on"_s, argument);
        if (hasOnlyLowestBitsOfInt32(m_code.at(argument)))
            return fail(from, { }, "an argument of an edge has only the lowest bits of an int32, and a parameter does not say so"_s, argument);
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
    // A handler block that only throws on what it takes is given up by every block that throws to it, so nothing reaches it: Sweep::handlerOnlyThrowsOutOfUnit().
    if (contents.handler && isReached(m_code.at(contents.handler).to) && (m_placeOfFirstThrow[block.index()] < place || declaredBoundOf(end.kind).mayThrow))
        fail(block, { }, "the block stands under a handler, may throw, and has lost its handler edge"_s);
}

// Nothing is claimed of a value that crosses: the slow body takes each for anything. What is asked: the words speak of as many values as there are operands, each frame names a function and lists values of its body, ascending,
// and each operand has a form that the row of a point takes.
void CodeVerifier::verifyLeave(BlockID block, ValueID id, const Operation& leave)
{
    std::span<const uint32_t> words = leave.frames();
    std::span<const ValueID> values = leave.valuesOperands();
    size_t numberOfValues = 0;
    if (words.empty())
        return fail(block, id, "a Leave names no frame"_s);
    if (leave.opcode == Opcode::LeaveCopies && (words.size() < 3 || !(words[2] & (1u << 16))))
        return fail(block, id, "copies are left for frames that are not entered at their heads"_s);
    while (!words.empty()) {
        if (words.size() < 4 || words.size() - 4 < words[3])
            return fail(block, id, "the words of a Leave end in the middle of a frame"_s);
        // Between the frame of a constructor and that of its caller: `this` and nothing else.
        if (words[0] == UINT32_MAX) {
            if (words[3] != 1 || words.data() == leave.frames().data() || words.size() == 5)
                return fail(block, id, "the frame of a Leave that selects the result of a construction does not hold one value between two frames"_s);
            ++numberOfValues;
            words = words.subspan(5);
            continue;
        }
        constexpr uint32_t arrivesAtHead = 1u << 16;
        constexpr uint32_t isArgument = 1u << 31;
        constexpr uint32_t isThisOfConstruction = 1u << 30;
        if (words[0] >= m_program.functions.size() || words[2] > (UINT16_MAX | arrivesAtHead))
            return fail(block, id, "a frame of a Leave names no function, or an entry that no unit can have"_s);
        // The slow body is entered at its head. So are all frames of the record or none. Two more words: the number of heap words of the cell, and one more than the word of the operation in whose place the copy above stood.
        // Its list: arguments, ascending by entry parameter, then the heap words that are filled, ascending, then `this` where that copy stood for a construction.
        bool isInnermost = words.data() == leave.frames().data();
        if (!isInnermost && !!(words[2] & arrivesAtHead) != !!(leave.frames()[2] & arrivesAtHead))
            return fail(block, id, "a frame of a Leave that arrives at the head of a slow body stands beside one that does not"_s);
        if (words[2] & arrivesAtHead) {
            if (words.size() < 6 || words.size() - 6 < words[3] || words[2] != arrivesAtHead || words[1] > static_cast<uint32_t>(INT32_MAX))
                return fail(block, id, "a frame of a Leave that arrives at the head of a slow body ends in the middle, or names an entry, or a number that is no int32"_s);
            std::span<const uint32_t> list = words.subspan(6, words[3]);
            uint32_t numberOfHeapWords = words[4];
            bool hasThis = !list.empty() && list.back() == isThisOfConstruction;
            if (isInnermost ? words[5] || hasThis : !words[4])
                return fail(block, id, "the innermost frame of a Leave waits for a copy to return, or another frame has no cell"_s);
            if (words[5] > words[4])
                return fail(block, id, "what a copy returns goes into a word that the cell of its caller does not have"_s);
            numberOfValues += list.size();
            words = words.subspan(6 + list.size());
            list = list.first(list.size() - hasThis);
            for (size_t index = 0; index < list.size(); ++index) {
                bool isArgumentHere = list[index] & isArgument;
                bool isInOrder = !index || (list[index - 1] < list[index] && isArgumentHere == !!(list[index - 1] & isArgument)) || (!isArgumentHere && (list[index - 1] & isArgument));
                if (!isInOrder || (!isArgumentHere && list[index] >= numberOfHeapWords))
                    return fail(block, id, "the list of a Leave that arrives at a head does not hold arguments, ascending, and then words of the cell, ascending"_s);
            }
            continue;
        }
        uint32_t numberOfValuesOfBody = m_program.at(FunctionID(words[0])).body.numberOfValues();
        std::span<const uint32_t> liveList = words.subspan(4, words[3]);
        for (size_t index = 0; index < liveList.size(); ++index) {
            if (liveList[index] >= numberOfValuesOfBody || (index && liveList[index - 1] >= liveList[index]))
                return fail(block, id, "the live list of a frame of a Leave does not ascend through values of the body"_s);
        }
        numberOfValues += liveList.size();
        words = words.subspan(4 + liveList.size());
    }
    if (numberOfValues != values.size())
        return fail(block, id, "a Leave has another number of operands than its frames list"_s);
    for (ValueID value : values) {
        if (!value || !crossesInItsForm(m_code.at(value).representation))
            return fail(block, id, "an operand of a Leave has a form that does not cross"_s, value);
    }
}

void CodeVerifier::verifyBlock(BlockID block)
{
    const Block& contents = m_code.at(block);
    for (ValueID parameter : contents.parameters()) {
        if (!hasLegalForm(m_code.at(parameter)))
            return fail(block, parameter, "the form of the record is none that its proven type allows"_s);
    }

    bool standsAtHead = true;
    ValueID leave;
    bool followsOnlyCatches = true;
    uint32_t place = 0;
    for (ValueID id : operationsOf(block)) {
        const Operation& operation = m_code.at(id);
        if (leave)
            return fail(block, id, "an operation stands behind a Leave"_s, leave);
        if (operation.opcode == Opcode::LeaveCopies) {
            verifyLeave(block, id, operation);
            if (hasFailed())
                return;
        }
        if (operation.opcode == Opcode::Leave) {
            leave = id;
            verifyLeave(block, id, operation);
            if (hasFailed())
                return;
        }
        if (operation.opcode != Opcode::Narrowed)
            standsAtHead &= operationsThatStandAmongNarrowedValues.contains(operation.opcode);
        else if (!standsAtHead)
            return fail(block, id, "a Narrowed stands behind another operation of its block"_s);
        if (familyOf(operation.opcode) != OperationFamily::Exception)
            followsOnlyCatches = false;
        else if (!followsOnlyCatches || !contents.isHandlerBlock())
            return fail(block, id, "what was thrown is read elsewhere than at the head of a handler block"_s, { }, InternalErrorKind::InvalidCatch);
        verifyOperation(block, ++place, id, operation);
        if (hasFailed())
            return;
    }
    if (leave && contents.terminator.kind != TerminatorKind::Unreachable)
        return fail(block, leave, "the block of a Leave does not end in Unreachable"_s);
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
        if (!opcodesThatGiveSameValue.contains(record.opcode))
            return id;
        id = record.valueOperand();
    }
}

// typeOfNarrowed() goes by the same reads. Null: `id` is none of them.
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

    // Its code tests nothing: the word of the value is handed on as it is, as a narrow reference that stays one, or goes through conversions that are right for the narrower type.
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
        // Which producers, and which of their results, is typeOfNarrowed()'s question.
        ValueID producer = operandAt(m_code.at(test), narrowed.operandIndex());
        bool readOneProducer = producer && m_code.at(test).opcode == Opcode::ResultAt && m_code.at(value).opcode == Opcode::ResultAt && m_code.at(value).resultsOperand() == producer;
        if (!readOneProducer || value == test || narrowed.narrowsHolderOfOperand())
            return fail(block, id, "the value of a Narrowed is no other result of the operation that its test reads a result of"_s, test);
        return;
    }
    ValueID tested = valueThatTestSpeaksOf(m_code, test, narrowed.operandIndex());
    if (tested)
        tested = valueBeforeConversions(tested);
    if (tested && narrowed.narrowsHolderOfOperand())
        tested = holderOfSlotRead(tested);
    if (tested != value)
        return fail(block, id, "the value of a Narrowed is not what its test has looked at"_s, test);
}

void CodeVerifier::verifyNarrowedValues()
{
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            if (operationsThatStandAmongNarrowedValues.contains(m_code.at(id).opcode))
                continue;
            if (m_code.at(id).opcode != Opcode::Narrowed)
                break;
            verifyNarrowed(block, id, m_code.at(id));
            if (hasFailed())
                return;
        }
    }
}

// What a callee does with an argument is its summary's to say, and Facts has allowed the frame by it.
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

// The operand that the result surely is. Where the schema calls the result unknown, what is handed back is only one of the values that may come: DirectEval gives its text, or what the text evaluates to.
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

void CodeVerifier::verifyFramePointersTakenBy(BlockID block, ValueID id, const Operation& operation)
{
    if (handsOperandsToCallee(operation.opcode)) {
        // No callee is handed what an awaited call holds: the saved frame of this function keeps it, and comes back at another address.
        for (uint32_t operandIndex = 0; operandIndex < operation.numberOfOperands(); ++operandIndex) {
            ValueID operand = operation.operand(operandIndex);
            if (operand && mayPointIntoFrame(operand) && isHeldOperand(operation, operandIndex))
                return fail(block, id, "a cell of the frame is held across a suspension"_s, operand, InternalErrorKind::InvalidSuspension);
        }
        return;
    }
    for (uint32_t operandIndex = 0; operandIndex < operation.numberOfOperands(); ++operandIndex) {
        ValueID operand = operation.operand(operandIndex);
        if (!operand || !mayPointIntoFrame(operand))
            continue;
        // A Narrowed, or a check that hands its operand on, may give it another form as a Compress does.
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
            if (LayoutID environment = holder && m_plan && mayPointIntoFrame(holder) ? m_types.at(m_code.at(holder).type).environment : LayoutID())
                staysInFrame |= m_plan->layout(environment).liesOnlyInFrames;
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
            // NewClosure[isLent]: the code of a tail call by value keeps the frame, or hands over a copy on the heap.
            const Operation& made = m_code.at(cellBeforeForwarding(operand));
            bool isMovedOutOfFrame = m_code.at(end.callOperand()).opcode == Opcode::Call && made.opcode == Opcode::NewClosure && made.isLent();
            if (mayPointIntoFrame(operand) && !isMovedOutOfFrame)
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
        return;
    }
    end.forEachChild([&](ValueID operand) {
        if (mayPointIntoFrame(operand))
            fail(block, { }, rule, operand);
    });
}

void CodeVerifier::verifyFrameCells()
{
    m_mayPointIntoFrame.ensureSize(m_code.numberOfValues());
    bool hasFrameCells = false;
    bool hasFlatStrings = false;
    for (BlockID block : m_reachedBlocks) {
        for (ValueID id : operationsOf(block)) {
            hasFlatStrings |= m_code.at(id).opcode == Opcode::FlattenString;
            if (!isFrameCell(m_code.at(id)))
                continue;
            m_mayPointIntoFrame.quickSet(id.index());
            hasFrameCells = true;
        }
    }
    if (!hasFrameCells && !hasFlatStrings)
        return;

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
            if (hasFailed())
                return;
        }
        verifyFramePointersTakenByEnd(block, m_code.at(block).terminator);
        if (hasFailed())
            return;
    }

    // The copy of an environment that a call is lent dies with the call and the reads behind it: behind a parameter of a block nobody could tell it from the environment itself.
    // An environment whose layout lies only in frames is no copy, and its type says where it lies behind any parameter.
    for (BlockID block : m_reachedBlocks) {
        m_code.forEachEdgeOf(block, [&](EdgeID, const Edge& edge) {
            for (ValueID argument : edge.arguments()) {
                const Operation& made = m_code.at(cellBeforeForwarding(argument));
                if (made.opcode != Opcode::NewObject || made.storage() != AllocationStorage::Frame || !m_program.at(made.layout()).isEnvironment())
                    continue;
                if (!m_plan || !m_plan->layout(made.layout()).liesOnlyInFrames)
                    fail(block, { }, "an environment that lies in the frame is an argument of an edge"_s, argument);
            }
        });
        if (hasFailed())
            return;
    }

    // A saved frame resumes at another address. So nothing suspends on any way from what makes a pointer into the frame to whatever reads it.
    // The end of a block is no suspension: the edge to a handler ends the block between a closure that is left to the stub of a call and the call.
    if (!isBodyOfCoroutine(m_code.role(), m_program.at(m_code.function()).isCoroutine()))
        return;

    // By block. Zero: nothing in the block suspends.
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

    // The maker dominates the reader, so every way back from the head of the reader ends at the maker. A reader in a loop that the maker stands in front of lies on such a way itself.
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

    // The emitter holds the characters of a flat string by a raw pointer. While the body is suspended the text may become a key, which gives it other characters, and nothing on a stack keeps the old ones.
    auto isFlatString = [&](ValueID id) {
        TypeID type = m_code.at(id).type;
        return type && m_types.at(type).isOpaque(OpaqueKind::FlatString);
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
            if (!isCellOfFrame && !isFlatString(operand))
                return;
            bool outlivesSuspension = m_code.blockOf(operand) == block
                ? m_places[operand.index()] < firstPlaceBehindSuspension
                : firstPlaceBehindSuspension || maySuspendBetweenMakerAndHeadOf(block, operand);
            if (outlivesSuspension)
                fail(block, user, isCellOfFrame ? "a cell of the frame of a coroutine body outlives a suspension"_s : "a flat string outlives a suspension"_s, operand, InternalErrorKind::InvalidSuspension);
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

} // anonymous namespace

String verifyCode(const CodeTextContext& context, const Code& code, InternalErrorKind* kindOfBrokenRule)
{
    CodeVerifier verifier(context, code);
    String brokenRule = verifier.run();
    if (kindOfBrokenRule)
        *kindOfBrokenRule = verifier.kindOfFailure();
    return brokenRule;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
